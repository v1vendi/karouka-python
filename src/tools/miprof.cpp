/*
 * karouka_miprof: a counting interposer for mimalloc, for attributing what Ladybird keeps in
 * kmalloc (mimalloc, VM tag 240) to call sites without patching Ladybird.
 *
 * libkarouka calls mimalloc through its dylib's exports (mi_malloc, mi_heap_malloc, ...), so an
 * image inserted with DYLD_INSERT_LIBRARIES can interpose those calls. Every allocation records its
 * usable size, heap and call stack (return addresses); every free drops the record. The viewer's
 * --mem-phases calls kr_miprof_mark(phase) at each phase (looked up with dlsym, so the viewer works
 * without it); each mark writes KR_MIPROF_DIR/NN-phase.tsv with the live bytes per call site, the
 * cumulative bytes allocated per site, each heap's mimalloc pages (committed vs used, from
 * mi_heap_visit_blocks) and the loaded images, for tools/miprof.py to diff and symbolize.
 *
 *   DYLD_INSERT_LIBRARIES=.../libkarouka_miprof.dylib KR_MIPROF_DIR=out karouka_viewer page.html \
 *       --headless --frames 1 --mem-phases
 *
 * tools/dev.sh mem --attribute does this. Diagnostic only: it slows allocation down ~10x.
 */

#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mimalloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr int max_frames = 40;

struct Site {
    int frame_count { 0 };
    void* frames[max_frames] {};
    uint32_t heap { 0 };
    size_t live_bytes { 0 };
    size_t live_count { 0 };
    size_t total_bytes { 0 };
    size_t total_count { 0 };
};

struct Record {
    uint32_t site;
    size_t size;
};

struct State {
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    std::vector<Site> sites;
    std::unordered_map<uint64_t, uint32_t> site_by_hash;
    std::unordered_map<void*, Record> live;
    // Per OS page, the site of the last block freed on it and whether a thread other than the
    // heap's owner freed it (such frees wait in the page's thread-free list until the owner
    // collects them), for attributing dirty pages that hold no live block.
    std::unordered_map<uintptr_t, std::pair<uint32_t, bool>> last_free;
    size_t cross_thread_freed_bytes { 0 };
    std::vector<mi_heap_t*> heaps;
    std::vector<pthread_t> heap_threads; // the thread that first allocated from each heap
    unsigned mark_count { 0 };
};

// Never destroyed: allocations can arrive during exit, after static destructors.
State& state()
{
    static State* s = new State;
    return *s;
}

thread_local bool t_inside = false;

uint32_t heap_id(State& s, mi_heap_t* heap)
{
    for (size_t i = 0; i < s.heaps.size(); ++i) {
        if (s.heaps[i] == heap)
            return static_cast<uint32_t>(i);
    }
    s.heaps.push_back(heap);
    s.heap_threads.push_back(pthread_self());
    return static_cast<uint32_t>(s.heaps.size() - 1);
}

void record_allocation(void* ptr, mi_heap_t* heap)
{
    if (!ptr || t_inside)
        return;
    t_inside = true;
    void* frames[max_frames + 2];
    // Frame 0 is this function, frame 1 the interposed mi_* call; keep the caller's frames.
    int count = backtrace(frames, max_frames + 2);
    int skip = count > 2 ? 2 : count;
    uint64_t hash = 1469598103934665603ull;
    for (int i = skip; i < count; ++i)
        hash = (hash ^ reinterpret_cast<uintptr_t>(frames[i])) * 1099511628211ull;
    size_t size = mi_usable_size(ptr);
    State& s = state();
    os_unfair_lock_lock(&s.lock);
    uint32_t heap_index = heap_id(s, heap);
    hash ^= heap_index;
    auto it = s.site_by_hash.find(hash);
    uint32_t site;
    if (it == s.site_by_hash.end()) {
        site = static_cast<uint32_t>(s.sites.size());
        s.sites.emplace_back();
        Site& new_site = s.sites.back();
        new_site.frame_count = count - skip;
        memcpy(new_site.frames, frames + skip, sizeof(void*) * (count - skip));
        new_site.heap = heap_index;
        s.site_by_hash.emplace(hash, site);
    } else {
        site = it->second;
    }
    Site& entry = s.sites[site];
    entry.live_bytes += size;
    entry.live_count++;
    entry.total_bytes += size;
    entry.total_count++;
    s.live[ptr] = Record { site, size };
    os_unfair_lock_unlock(&s.lock);
    t_inside = false;
}

void record_free(void* ptr)
{
    if (!ptr || t_inside)
        return;
    t_inside = true;
    State& s = state();
    os_unfair_lock_lock(&s.lock);
    if (auto it = s.live.find(ptr); it != s.live.end()) {
        Site& site = s.sites[it->second.site];
        bool cross = !pthread_equal(s.heap_threads[site.heap], pthread_self());
        if (cross)
            s.cross_thread_freed_bytes += it->second.size;
        uintptr_t first = reinterpret_cast<uintptr_t>(ptr) / vm_page_size;
        uintptr_t last = (reinterpret_cast<uintptr_t>(ptr) + it->second.size - 1) / vm_page_size;
        for (uintptr_t page = first; page <= last; ++page)
            s.last_free[page] = { it->second.site, cross };
        site.live_bytes -= it->second.size;
        site.live_count--;
        s.live.erase(it);
    }
    os_unfair_lock_unlock(&s.lock);
    t_inside = false;
}

void* kr_mi_malloc(size_t size)
{
    void* p = mi_malloc(size);
    record_allocation(p, mi_heap_get_default());
    return p;
}

void* kr_mi_calloc(size_t count, size_t size)
{
    void* p = mi_calloc(count, size);
    record_allocation(p, mi_heap_get_default());
    return p;
}

void* kr_mi_malloc_aligned(size_t size, size_t alignment)
{
    void* p = mi_malloc_aligned(size, alignment);
    record_allocation(p, mi_heap_get_default());
    return p;
}

void* kr_mi_realloc(void* ptr, size_t size)
{
    record_free(ptr);
    void* p = mi_realloc(ptr, size);
    record_allocation(p, mi_heap_get_default());
    return p;
}

void* kr_mi_heap_malloc(mi_heap_t* heap, size_t size)
{
    void* p = mi_heap_malloc(heap, size);
    record_allocation(p, heap);
    return p;
}

void* kr_mi_heap_malloc_aligned(mi_heap_t* heap, size_t size, size_t alignment)
{
    void* p = mi_heap_malloc_aligned(heap, size, alignment);
    record_allocation(p, heap);
    return p;
}

void* kr_mi_heap_realloc(mi_heap_t* heap, void* ptr, size_t size)
{
    record_free(ptr);
    void* p = mi_heap_realloc(heap, ptr, size);
    record_allocation(p, heap);
    return p;
}

void kr_mi_free(void* ptr)
{
    record_free(ptr);
    mi_free(ptr);
}

struct AreaTotals {
    FILE* out;
    unsigned heap;
};

bool visit_area(mi_heap_t const*, mi_heap_area_t const* area, void*, size_t, void* arg)
{
    auto* totals = static_cast<AreaTotals*>(arg);
    fprintf(totals->out, "area\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%p\n", totals->heap, area->block_size, area->used,
        area->committed, area->reserved, area->full_block_size, area->blocks);
    return true;
}

// Writes the runs of dirty (or compressed) pages in VM regions tagged 240, the tag AK/kmalloc.cpp
// gives mimalloc: "dirty\t<address>\t<pages>". Counted the way kr_system_query_memory_stats counts.
void write_dirty_runs(FILE* out)
{
    mach_vm_address_t address = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_extended_info_data_t info {};
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
            break;
        if (info.user_tag == 240 && info.protection != VM_PROT_NONE && (info.pages_resident || info.pages_swapped_out)) {
            fprintf(out, "region\t%p\t%llu\n", reinterpret_cast<void*>(address), static_cast<unsigned long long>(size));
            static constexpr size_t chunk_pages = 4096;
            static int dispositions[chunk_pages];
            mach_vm_address_t run_start = 0;
            size_t run_pages = 0;
            for (mach_vm_size_t offset = 0; offset < size; offset += chunk_pages * vm_page_size) {
                mach_vm_size_t pages = (size - offset) / vm_page_size;
                if (pages > chunk_pages)
                    pages = chunk_pages;
                if (mach_vm_page_range_query(mach_task_self(), address + offset, pages * vm_page_size, reinterpret_cast<mach_vm_address_t>(dispositions), &pages) != KERN_SUCCESS)
                    break;
                for (mach_vm_size_t i = 0; i < pages; ++i) {
                    int disposition = dispositions[i];
                    bool dirty = ((disposition & VM_PAGE_QUERY_PAGE_DIRTY) && !(disposition & VM_PAGE_QUERY_PAGE_REUSABLE))
                        || (disposition & VM_PAGE_QUERY_PAGE_PAGED_OUT);
                    mach_vm_address_t page = address + offset + i * vm_page_size;
                    if (dirty && run_pages && run_start + run_pages * vm_page_size == page) {
                        run_pages++;
                    } else if (dirty) {
                        if (run_pages)
                            fprintf(out, "dirty\t%p\t%zu\n", reinterpret_cast<void*>(run_start), run_pages);
                        run_start = page;
                        run_pages = 1;
                    }
                }
            }
            if (run_pages)
                fprintf(out, "dirty\t%p\t%zu\n", reinterpret_cast<void*>(run_start), run_pages);
        }
        address += size;
    }
}

}

#define KR_INTERPOSE(replacement, replacee)                                                         \
    __attribute__((used)) static struct {                                                            \
        void const* replacement_fn;                                                                  \
        void const* replacee_fn;                                                                     \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {                        \
        reinterpret_cast<void const*>(&replacement), reinterpret_cast<void const*>(&replacee)        \
    };

KR_INTERPOSE(kr_mi_malloc, mi_malloc)
KR_INTERPOSE(kr_mi_calloc, mi_calloc)
KR_INTERPOSE(kr_mi_malloc_aligned, mi_malloc_aligned)
KR_INTERPOSE(kr_mi_realloc, mi_realloc)
KR_INTERPOSE(kr_mi_heap_malloc, mi_heap_malloc)
KR_INTERPOSE(kr_mi_heap_malloc_aligned, mi_heap_malloc_aligned)
KR_INTERPOSE(kr_mi_heap_realloc, mi_heap_realloc)
KR_INTERPOSE(kr_mi_free, mi_free)

// Writes the live and cumulative bytes per call site to KR_MIPROF_DIR/NN-<phase>.tsv.
extern "C" __attribute__((visibility("default"))) void kr_miprof_mark(char const* phase);
void kr_miprof_mark(char const* phase)
{
    char const* dir = getenv("KR_MIPROF_DIR");
    if (!dir)
        return;
    t_inside = true;
    State& s = state();
    os_unfair_lock_lock(&s.lock);
    // KR_MIPROF_COLLECT=<phase>: first collect every heap this thread owns (mi_heap_collect, force),
    // to see what a trim at that point would return.
    if (char const* collect = getenv("KR_MIPROF_COLLECT"); collect && strcmp(collect, phase) == 0) {
        for (size_t i = 0; i < s.heaps.size(); ++i) {
            if (pthread_equal(s.heap_threads[i], pthread_self()))
                mi_heap_collect(s.heaps[i], true);
        }
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%02u-%s.tsv", dir, s.mark_count++, phase);
    FILE* out = fopen(path, "w");
    if (out) {
        size_t elapsed, user, system, rss, peak_rss, commit, peak_commit, faults;
        mi_process_info(&elapsed, &user, &system, &rss, &peak_rss, &commit, &peak_commit, &faults);
        fprintf(out, "phase\t%s\n", phase);
        fprintf(out, "process\t%zu\t%zu\t%zu\t%zu\n", rss, peak_rss, commit, peak_commit);
        for (uint32_t i = 0; i < _dyld_image_count(); ++i)
            fprintf(out, "image\t%p\t%s\n", static_cast<void const*>(_dyld_get_image_header(i)), _dyld_get_image_name(i));
        fprintf(out, "pagesize\t%zu\n", static_cast<size_t>(vm_page_size));
        write_dirty_runs(out);
        for (size_t i = 0; i < s.heaps.size(); ++i) {
            bool own = pthread_equal(s.heap_threads[i], pthread_self());
            fprintf(out, "heap\t%zu\t%p\t%s\n", i, static_cast<void*>(s.heaps[i]), own ? "visited" : "other-thread");
            // mimalloc heaps belong to one thread, and a thread's heaps are freed when it exits,
            // so only this thread's heaps can be walked.
            if (!own)
                continue;
            AreaTotals totals { out, static_cast<unsigned>(i) };
            mi_heap_visit_blocks(s.heaps[i], false, visit_area, &totals);
        }
        // Live bytes per OS page and the heap of the page's first block seen, so dirty pages
        // can be told apart: holding live blocks, or holding none (freed, not yet purged).
        std::unordered_map<uintptr_t, std::pair<size_t, uint32_t>> pages;
        for (auto const& [ptr, record] : s.live) {
            uintptr_t first = reinterpret_cast<uintptr_t>(ptr) / vm_page_size;
            uintptr_t last = (reinterpret_cast<uintptr_t>(ptr) + record.size - 1) / vm_page_size;
            for (uintptr_t page = first; page <= last; ++page) {
                auto& entry = pages.try_emplace(page, 0, s.sites[record.site].heap).first->second;
                entry.first += record.size / (last - first + 1);
            }
        }
        for (auto const& [page, entry] : s.last_free) {
            if (!pages.contains(page))
                fprintf(out, "freedpage\t%p\t%u\t%d\n", reinterpret_cast<void*>(page * vm_page_size), entry.first, entry.second ? 1 : 0);
        }
        fprintf(out, "crossfree\t%zu\n", s.cross_thread_freed_bytes);
        for (auto const& [page, entry] : pages)
            fprintf(out, "livepage\t%p\t%zu\t%u\n", reinterpret_cast<void*>(page * vm_page_size), entry.first, entry.second);
        for (size_t i = 0; i < s.sites.size(); ++i) {
            Site const& site = s.sites[i];
            fprintf(out, "site\t%zu\t%u\t%zu\t%zu\t%zu\t%zu\t", i, site.heap, site.live_bytes, site.live_count,
                site.total_bytes, site.total_count);
            for (int f = 0; f < site.frame_count; ++f)
                fprintf(out, f ? ",%p" : "%p", site.frames[f]);
            fputc('\n', out);
        }
        fclose(out);
    }
    os_unfair_lock_unlock(&s.lock);
    t_inside = false;
}
