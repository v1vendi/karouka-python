/*
 * karouka_ordertrace: records which functions of libkarouka run, in the order they first run, for
 * the linker order file (tools/orderfile.py). No instrumented build: inserted with
 * DYLD_INSERT_LIBRARIES, its initializer runs before libkarouka's (dyld runs inserted images'
 * initializers first) and writes a `brk` over the first instruction of every function libkarouka's
 * LC_FUNCTION_STARTS lists. The first thread to enter a function traps; the SIGTRAP handler records
 * the function and puts its instruction back, and the thread resumes there. So each function costs
 * one trap, however often it runs, and the program otherwise runs at full speed.
 *
 *   DYLD_INSERT_LIBRARIES=.../libkarouka_ordertrace.dylib KR_ORDERTRACE_OUT=calls.txt \
 *       karouka_viewer page.html --headless --frames 1 --mem-phases
 *
 * The output has one line per function, its address in the image file ("0x..."), and a "# phase"
 * line wherever the viewer's --mem-phases marked a phase (kr_miprof_mark, which the viewer looks up
 * with dlsym). Diagnostic only: the patched pages are private copies of libkarouka's __TEXT.
 *
 * KR_ORDERTRACE_DATA=1 also records the data the program reads: every whole page of
 * __TEXT,__const, __TEXT,__cstring and __DATA_CONST,__const is made inaccessible, and a read faults.
 * The handler records the address ("data 0x...", in the image file, each address once) and opens
 * the page, closing again the page opened KR_ORDERTRACE_DATA_WINDOW (2: one load can span two pages)
 * faults before, so that a later read of another datum on it faults too, and a thread closes the
 * open pages every KR_ORDERTRACE_DATA_INTERVAL_US (10) microseconds, so that a page read in a loop
 * still reports the next datum read on it. When the read is libkarouka's own instruction (or a libSystem
 * routine libkarouka called, by its return address), the handler also puts a `brk` after it, which
 * closes the page again as soon as that instruction ran: then a page holding the hot data still
 * reports each datum's first read, which keeps the trace the same whatever the layout. A page read in
 * a loop would fault on every read, so every KR_ORDERTRACE_DATA_BUDGET (65535) faults on addresses read
 * before (a byte loop over a new string reads new ones) it stays open for
 * a while, 10 ms the first time, twice as long each time after, up to 500 ms, then closes again. A system call given a pointer into a closed page
 * fails with EFAULT instead of faulting, so the traced run can differ a little from a real one.
 */

#include <atomic>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t trap_instruction = 0xd4200000u | (0x4b0eu << 5); // brk #0x4b0e

struct Function {
    uintptr_t address; // in the running image
    uint32_t instruction;
};

struct Event {
    uintptr_t address; // 0 for a phase mark
    char const* phase;
};

uintptr_t s_slide;
uintptr_t s_text_start;
uintptr_t s_text_end;
std::vector<Function>* s_functions;
// Preallocated so the signal handler never allocates: at most one event per function, plus marks.
Event* s_events;
size_t s_event_capacity;
std::atomic<size_t> s_event_count { 0 };
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
size_t s_page_size;
struct sigaction s_previous_trap;
struct sigaction s_previous_segv;
struct sigaction s_previous_bus;

// the data pages KR_ORDERTRACE_DATA closes. One range per section, whole pages only.
struct DataRange {
    uintptr_t start; // page-aligned, in the running image
    uintptr_t end;
    int protection; // what the page is when open
    size_t first_page; // index into s_page_state
};
enum : uint8_t { PageClosed, PageOpen, PageStaysOpen };
DataRange s_data_ranges[4];
size_t s_data_range_count;
uint8_t* s_page_state;
uint16_t* s_page_faults;
uintptr_t s_window[8];
size_t s_window_size = 2;
size_t s_window_next;
unsigned s_fault_budget = 65535;
uint64_t* s_page_reclose; // mach_absolute_time() at which a page that stays open closes again
uint8_t* s_page_backoffs;
uint64_t s_ticks_per_ms;
unsigned s_close_interval_us = 10;
// The distinct data addresses read, in order (preallocated), and an open-addressing set of them.
uintptr_t* s_data_events;
size_t s_data_capacity;
size_t s_data_count;
uintptr_t* s_data_set;
size_t s_data_set_mask;
size_t s_data_faults;
// A brk put after a read: reached, it puts the instruction back and closes the pages.
struct Step {
    uintptr_t address; // 0 when free
    uint32_t instruction;
    uintptr_t pages[2];
};
Step s_steps[64];
size_t s_unstepped;
uintptr_t s_unstepped_pc[8];

struct DataRange;
DataRange const* data_range(uintptr_t address);
void close_page(uintptr_t page);

void lock()
{
    while (s_lock.test_and_set(std::memory_order_acquire)) { }
}

void unlock() { s_lock.clear(std::memory_order_release); }

void add_event(uintptr_t address, char const* phase)
{
    size_t index = s_event_count.fetch_add(1);
    if (index < s_event_capacity)
        s_events[index] = { address, phase };
}

// Makes [address, address + size) writable, then executable again after `write` ran. Between the
// two, other threads that run code on those pages fault; fault_handler waits for the lock and
// retries them.
template<typename Write>
bool patch(uintptr_t address, size_t size, Write write)
{
    uintptr_t start = address & ~(s_page_size - 1);
    uintptr_t end = (address + size + s_page_size - 1) & ~(s_page_size - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE) != 0)
        return false;
    write();
    mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(reinterpret_cast<void*>(address), size);
    return true;
}

void chain(struct sigaction const& previous, int signal, siginfo_t* info, void* context)
{
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
        return;
    }
    ::signal(signal, SIG_DFL);
    raise(signal);
}

void trap_handler(int signal, siginfo_t* info, void* context)
{
    auto* uc = static_cast<ucontext_t*>(context);
    uintptr_t pc = uc->uc_mcontext->__ss.__pc;
    if (pc >= s_text_start && pc < s_text_end && s_page_state) {
        lock();
        for (auto& step : s_steps) {
            if (step.address != pc)
                continue;
            uint32_t original = step.instruction;
            patch(pc, 4, [&] { *reinterpret_cast<uint32_t*>(pc) = original; });
            for (uintptr_t page : step.pages) {
                if (page)
                    close_page(page);
            }
            step = {};
            unlock();
            return;
        }
        // Another thread reached the same brk and put the instruction back while this one waited.
        bool restored = *reinterpret_cast<uint32_t*>(pc) != trap_instruction;
        unlock();
        if (restored)
            return;
    }
    if (pc >= s_text_start && pc < s_text_end) {
        auto& functions = *s_functions;
        auto it = std::lower_bound(functions.begin(), functions.end(), pc,
            [](Function const& f, uintptr_t a) { return f.address < a; });
        if (it != functions.end() && it->address == pc) {
            lock();
            // Another thread may have restored it while this one waited.
            if (*reinterpret_cast<uint32_t*>(pc) == trap_instruction) {
                add_event(pc - s_slide, nullptr);
                uint32_t original = it->instruction;
                patch(pc, 4, [&] { *reinterpret_cast<uint32_t*>(pc) = original; });
            }
            unlock();
            return; // resume at pc, now the original instruction
        }
    }
    chain(s_previous_trap, signal, info, context);
}

DataRange const* data_range(uintptr_t address)
{
    for (size_t i = 0; i < s_data_range_count; ++i) {
        if (address >= s_data_ranges[i].start && address < s_data_ranges[i].end)
            return &s_data_ranges[i];
    }
    return nullptr;
}

// Whether the address is new.
bool record_data(uintptr_t address)
{
    uintptr_t key = address - s_slide;
    for (size_t slot = ((key * 0x9E3779B97F4A7C15ull) >> 40) & s_data_set_mask;; slot = (slot + 1) & s_data_set_mask) {
        if (s_data_set[slot] == key)
            return false;
        if (!s_data_set[slot]) {
            if (s_data_count >= s_data_capacity)
                return false;
            s_data_set[slot] = key;
            s_data_events[s_data_count++] = key;
            return true;
        }
    }
}

// Under the lock. An open page goes back to closed.
void close_page(uintptr_t page)
{
    DataRange const* range = data_range(page);
    if (!range)
        return;
    size_t index = range->first_page + (page - range->start) / s_page_size;
    if (s_page_state[index] == PageOpen) {
        mprotect(reinterpret_cast<void*>(page), s_page_size, PROT_NONE);
        s_page_state[index] = PageClosed;
    }
}

// Under the lock: closes `page` again once the instruction at `pc` ran, by a brk on the next
// instruction (or on `lr`, the return into libkarouka, when `pc` is outside it).
void step_over(uintptr_t pc, uintptr_t lr, uintptr_t page)
{
    uintptr_t at = (pc >= s_text_start && pc + 4 < s_text_end) ? pc + 4 : (lr >= s_text_start && lr < s_text_end) ? lr : 0;
    if (!at) {
        if (s_unstepped < 8)
            s_unstepped_pc[s_unstepped] = pc;
        ++s_unstepped;
        return;
    }
    Step* free_step = nullptr;
    for (auto& step : s_steps) {
        if (step.address == at) { // a read that spans two pages: close both
            if (!step.pages[1] && step.pages[0] != page)
                step.pages[1] = page;
            return;
        }
        if (!step.address && !free_step)
            free_step = &step;
    }
    if (!free_step || *reinterpret_cast<uint32_t*>(at) == trap_instruction)
        return; // the window and the closing thread close it
    *free_step = { at, *reinterpret_cast<uint32_t*>(at), { page, 0 } };
    patch(at, 4, [&] { *reinterpret_cast<uint32_t*>(at) = trap_instruction; });
}

// Under the lock: a read of `address`, in a closed or just-opened data page.
void data_fault(DataRange const& range, uintptr_t address, uintptr_t pc, uintptr_t lr)
{
    ++s_data_faults;
    bool is_new = record_data(address);
    uintptr_t page = address & ~(s_page_size - 1);
    size_t index = range.first_page + (page - range.start) / s_page_size;
    if (s_page_state[index] != PageClosed)
        return; // another thread opened it while this one waited for the lock
    mprotect(reinterpret_cast<void*>(page), s_page_size, range.protection);
    if (!is_new && ++s_page_faults[index] >= s_fault_budget) {
        // Read in a loop: leave it open for a while (10 ms << backoffs, up to 500 ms).
        s_page_faults[index] = 0;
        uint64_t ms = std::min<uint64_t>(10ull << std::min<uint8_t>(s_page_backoffs[index]++, 6), 500);
        s_page_reclose[index] = mach_absolute_time() + ms * s_ticks_per_ms;
        s_page_state[index] = PageStaysOpen;
        return;
    }
    s_page_state[index] = PageOpen;
    step_over(pc, lr, page);
    uintptr_t evicted = s_window[s_window_next];
    s_window[s_window_next] = address;
    s_window_next = (s_window_next + 1) % s_window_size;
    if (!evicted)
        return;
    DataRange const* evicted_range = data_range(evicted);
    uintptr_t evicted_page = evicted & ~(s_page_size - 1);
    size_t evicted_index = evicted_range->first_page + (evicted_page - evicted_range->start) / s_page_size;
    if (s_page_state[evicted_index] == PageOpen && evicted_page != page) {
        mprotect(reinterpret_cast<void*>(evicted_page), s_page_size, PROT_NONE);
        s_page_state[evicted_index] = PageClosed;
    }
}

// A thread that ran into a page while trap_handler had it writable: wait, then retry. Or a read of
// a data page KR_ORDERTRACE_DATA closed.
void fault_handler(int signal, siginfo_t* info, void* context)
{
    auto* uc = static_cast<ucontext_t*>(context);
    uintptr_t pc = uc->uc_mcontext->__ss.__pc;
    if (auto const* range = s_page_state ? data_range(reinterpret_cast<uintptr_t>(info->si_addr)) : nullptr) {
        lock();
        data_fault(*range, reinterpret_cast<uintptr_t>(info->si_addr), pc, uc->uc_mcontext->__ss.__lr);
        unlock();
        return;
    }
    if (pc >= s_text_start && pc < s_text_end && reinterpret_cast<uintptr_t>(info->si_addr) == pc) {
        lock();
        unlock();
        return;
    }
    chain(signal == SIGBUS ? s_previous_bus : s_previous_segv, signal, info, context);
}

void write_output()
{
    char const* path = getenv("KR_ORDERTRACE_OUT");
    FILE* out = fopen(path ? path : "ordertrace.txt", "w");
    if (!out)
        return;
    lock();
    size_t count = std::min(s_event_count.load(), s_event_capacity);
    for (size_t i = 0; i < count; ++i) {
        if (s_events[i].phase)
            fprintf(out, "# %s\n", s_events[i].phase);
        else
            fprintf(out, "0x%lx\n", static_cast<unsigned long>(s_events[i].address));
    }
    for (size_t i = 0; i < s_data_count; ++i)
        fprintf(out, "data 0x%lx\n", static_cast<unsigned long>(s_data_events[i]));
    unlock();
    if (s_page_state) {
        size_t open = 0, total = 0;
        for (size_t r = 0; r < s_data_range_count; ++r)
            total += (s_data_ranges[r].end - s_data_ranges[r].start) / s_page_size;
        for (size_t p = 0; p < total; ++p)
            open += s_page_backoffs[p] != 0;
        fprintf(stderr, "karouka_ordertrace: %zu data faults, %zu distinct addresses, %zu of %zu pages read in loops\n",
            s_data_faults, s_data_count, open, total);
        fprintf(stderr, "karouka_ordertrace: %zu faults not stepped over, first at", s_unstepped);
        for (size_t i = 0; i < std::min<size_t>(s_unstepped, 8); ++i) {
            Dl_info dl;
            fprintf(stderr, " %s", dladdr(reinterpret_cast<void*>(s_unstepped_pc[i]), &dl) && dl.dli_sname ? dl.dli_sname : "?");
        }
        fprintf(stderr, "\n");
    }
    fclose(out);
}

uint64_t read_uleb(uint8_t const*& p, uint8_t const* end)
{
    uint64_t value = 0;
    int shift = 0;
    while (p < end) {
        uint8_t byte = *p++;
        value |= uint64_t(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80))
            break;
    }
    return value;
}

int current_protection(uintptr_t address)
{
    mach_vm_address_t region = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
        return -1;
    return info.protection;
}

// The whole pages of the data sections KR_ORDERTRACE_DATA traces.
void collect_data_ranges(mach_header_64 const* header, intptr_t slide)
{
    if (char const* window = getenv("KR_ORDERTRACE_DATA_WINDOW"))
        s_window_size = std::clamp<size_t>(strtoul(window, nullptr, 10), 2, 8);
    if (char const* budget = getenv("KR_ORDERTRACE_DATA_BUDGET"))
        s_fault_budget = std::clamp<unsigned>(strtoul(budget, nullptr, 10), 1, 65535);
    if (char const* interval = getenv("KR_ORDERTRACE_DATA_INTERVAL_US"))
        s_close_interval_us = std::clamp<unsigned>(strtoul(interval, nullptr, 10), 10, 1000000);
    size_t pages = 0;
    auto const* command = reinterpret_cast<load_command const*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto const* segment = reinterpret_cast<segment_command_64 const*>(command);
            auto const* section = reinterpret_cast<section_64 const*>(segment + 1);
            for (uint32_t s = 0; s < segment->nsects; ++s) {
                bool traced = (!strcmp(segment->segname, "__TEXT") && (!strcmp(section[s].sectname, "__const") || !strcmp(section[s].sectname, "__cstring")))
                    || (!strcmp(segment->segname, "__DATA_CONST") && !strcmp(section[s].sectname, "__const"));
                if (!traced || s_data_range_count == std::size(s_data_ranges))
                    continue;
                uintptr_t start = (section[s].addr + slide + s_page_size - 1) & ~(s_page_size - 1);
                uintptr_t end = (section[s].addr + slide + section[s].size) & ~(s_page_size - 1);
                int protection = current_protection(start);
                if (end <= start || protection < 0)
                    continue;
                s_data_ranges[s_data_range_count++] = { start, end, protection, pages };
                pages += (end - start) / s_page_size;
            }
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<uint8_t const*>(command) + command->cmdsize);
    }
    s_page_state = static_cast<uint8_t*>(calloc(pages, 1));
    s_page_faults = static_cast<uint16_t*>(calloc(pages, sizeof(uint16_t)));
    s_page_reclose = static_cast<uint64_t*>(calloc(pages, sizeof(uint64_t)));
    s_page_backoffs = static_cast<uint8_t*>(calloc(pages, 1));
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    s_ticks_per_ms = 1000000ull * timebase.denom / timebase.numer;
    s_data_capacity = 1 << 22;
    s_data_events = static_cast<uintptr_t*>(calloc(s_data_capacity, sizeof(uintptr_t)));
    s_data_set_mask = (s_data_capacity << 1) - 1;
    s_data_set = static_cast<uintptr_t*>(calloc(s_data_set_mask + 1, sizeof(uintptr_t)));
    fprintf(stderr, "karouka_ordertrace: tracing reads of %zu data pages\n", pages);
}

void* close_open_pages(void*)
{
    for (;;) {
        usleep(s_close_interval_us);
        lock();
        uint64_t now = mach_absolute_time();
        for (size_t r = 0; r < s_data_range_count; ++r) {
            auto const& range = s_data_ranges[r];
            for (uintptr_t page = range.start; page < range.end; page += s_page_size) {
                size_t index = range.first_page + (page - range.start) / s_page_size;
                if (s_page_state[index] == PageStaysOpen && now >= s_page_reclose[index]) {
                    mprotect(reinterpret_cast<void*>(page), s_page_size, PROT_NONE);
                    s_page_state[index] = PageClosed;
                }
            }
        }
        for (size_t i = 0; i < s_window_size; ++i) {
            uintptr_t address = s_window[i];
            s_window[i] = 0;
            DataRange const* range = address ? data_range(address) : nullptr;
            if (!range)
                continue;
            uintptr_t page = address & ~(s_page_size - 1);
            size_t index = range->first_page + (page - range->start) / s_page_size;
            if (s_page_state[index] == PageOpen) {
                mprotect(reinterpret_cast<void*>(page), s_page_size, PROT_NONE);
                s_page_state[index] = PageClosed;
            }
        }
        unlock();
    }
    return nullptr;
}

void close_data_pages()
{
    if (!s_page_state)
        return;
    for (size_t i = 0; i < s_data_range_count; ++i)
        mprotect(reinterpret_cast<void*>(s_data_ranges[i].start), s_data_ranges[i].end - s_data_ranges[i].start, PROT_NONE);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, close_open_pages, nullptr) == 0)
        pthread_detach(thread);
}

bool install(mach_header_64 const* header, intptr_t slide)
{
    segment_command_64 const* linkedit = nullptr;
    linkedit_data_command const* starts = nullptr;
    uint64_t text_vmaddr = 0, text_section_addr = 0, text_section_end = 0;
    auto const* command = reinterpret_cast<load_command const*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto const* segment = reinterpret_cast<segment_command_64 const*>(command);
            if (!strcmp(segment->segname, "__TEXT")) {
                text_vmaddr = segment->vmaddr;
                auto const* section = reinterpret_cast<section_64 const*>(segment + 1);
                for (uint32_t s = 0; s < segment->nsects; ++s) {
                    if (!strcmp(section[s].sectname, "__text")) {
                        text_section_addr = section[s].addr;
                        text_section_end = section[s].addr + section[s].size;
                    }
                }
            } else if (!strcmp(segment->segname, "__LINKEDIT")) {
                linkedit = segment;
            }
        } else if (command->cmd == LC_FUNCTION_STARTS) {
            starts = reinterpret_cast<linkedit_data_command const*>(command);
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<uint8_t const*>(command) + command->cmdsize);
    }
    if (!linkedit || !starts || !text_section_end)
        return false;
    s_slide = slide;
    s_text_start = text_section_addr + slide;
    s_text_end = text_section_end + slide;
    auto const* p = reinterpret_cast<uint8_t const*>(linkedit->vmaddr + slide + starts->dataoff - linkedit->fileoff);
    auto const* end = p + starts->datasize;
    s_functions = new std::vector<Function>;
    uint64_t address = text_vmaddr;
    while (p < end) {
        uint64_t delta = read_uleb(p, end);
        if (!delta)
            break;
        address += delta;
        if (address >= text_section_addr && address < text_section_end)
            s_functions->push_back({ uintptr_t(address + slide), 0 });
    }
    s_event_capacity = s_functions->size() + 256;
    s_events = static_cast<Event*>(calloc(s_event_capacity, sizeof(Event)));
    if (getenv("KR_ORDERTRACE_DATA"))
        collect_data_ranges(header, slide);

    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = trap_handler;
    sigaction(SIGTRAP, &action, &s_previous_trap);
    action.sa_sigaction = fault_handler;
    sigaction(SIGSEGV, &action, &s_previous_segv);
    sigaction(SIGBUS, &action, &s_previous_bus);

    bool ok = patch(s_text_start, s_text_end - s_text_start, [] {
        for (auto& function : *s_functions) {
            auto* instruction = reinterpret_cast<uint32_t*>(function.address);
            function.instruction = *instruction;
            *instruction = trap_instruction;
        }
    });
    fprintf(stderr, "karouka_ordertrace: %zu functions in libkarouka's __text%s\n", s_functions->size(), ok ? "" : ", mprotect failed");
    close_data_pages();
    return ok;
}

__attribute__((constructor)) void initialize()
{
    s_page_size = getpagesize();
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        char const* name = _dyld_get_image_name(i);
        char const* slash = strrchr(name, '/');
        if (!strcmp(slash ? slash + 1 : name, "libkarouka.dylib")) {
            if (install(reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(i)), _dyld_get_image_vmaddr_slide(i)))
                atexit(write_output);
            return;
        }
    }
    fprintf(stderr, "karouka_ordertrace: libkarouka.dylib is not loaded\n");
}

}

// The viewer's --mem-phases marks (looked up with dlsym, as for karouka_miprof).
extern "C" __attribute__((visibility("default"))) void kr_miprof_mark(char const* phase);
extern "C" __attribute__((visibility("default"))) void kr_miprof_mark(char const* phase)
{
    if (!s_events)
        return;
    char const* copy = strdup(phase);
    lock();
    add_event(0, copy);
    unlock();
}
