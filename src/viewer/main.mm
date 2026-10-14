/*
 * karouka_viewer: shows a page rendered by karouka in a Metal window, or renders it headless.
 * Flags follow Tessera's tessera_viewer so bench/compare.py can drive both the same way.
 *
 *   karouka_viewer [page.html] [--size WxH] [--scale N] [--frames N] [--headless]
 *                  [--capture out.png] [--stats] [--scroll DX,DY[@F|@A..B]]...
 *                  [--poke X,Y@F]... [--move X0,Y0:X1,Y1@F[xSTEPS]]... [--key VK@F[..L]]... [--blur F]... [--focus F]... [--hold]
 *                  [--second page2.html]... [--input-view N] [--close-second F] [--mem-phases] [--mem-phases-heap]
 *                  [--no-render] [--engine-gpu] [--output-texture] [--backend metal|software] [--fps N]
 *                  [--shader-cache PATH | --no-shader-cache] [--capture-again N:out.png] [--check-full-raster]
 *   karouka_viewer test.html --headless --frames N --testharness [--doc-root WPT_ROOT]
 *   karouka_viewer page.html --print-meta NAME
 *
 * --capture-again N:out.png (headless, after --frames F, N > F) runs on past frame F, where --capture,
 * --check-title, --stats and the other end-of-run checks happen as usual, to frame N and captures the
 * first view again to out.png, so a page's frames 30 and 60 compare in one run.
 * --check-full-raster (headless, the viewer's GPU) checks partial raster at the end of
 * the run, after the rest: the pixels the first view's partial rasters left in its IOSurface are copied,
 * karouka draws the same frame whole into a fresh texture of the viewer's (a new target rasters whole),
 * and the two must match exactly, or the run exits 4 (1 when another check failed too). It prints "karouka_viewer: full raster check ...
 * matches" or "DIFFERS FROM".
 *
 * --frames N steps the page's clock by a fixed 16.67 ms per frame, as tessera_viewer does, so a
 * run's frames and --capture depend on N only; without it the viewer passes the real clock.
 * --real-time makes the page's clock the system's monotonic clock (kr_system_desc.real_time_clock):
 * performance.now() moves while a script runs, so a page that busy-waits on it finishes, and frames
 * depend on what they cost, --frames or not.
 * --fps N (headless, tessera_viewer's flag) paces the run to N frames a second of real time: after
 * each frame the viewer sleeps to the next deadline of a free-running 1/N s period (a frame that
 * overruns its deadline starts the next period at its end, without catching up). The page's clock
 * still steps 16.67 ms per frame. bench/compare.py's paced scenario runs --fps 60, so CPU % at
 * 60 fps is measured, not derived from an unpaced run. Without it a headless run is unpaced.
 * --stats prints, per view and at the end of the run, the karouka-stats line and a frame-loop line
 * (the format tessera_viewer is to print as well): the frame loop after the first frame,
 * from the end of frame 0 to the end of the last, timed in process. `frames` is the frames in it
 * (N - 1), wall_s and cpu_s its wall and process CPU time (getrusage, user + sys, every thread),
 * frame_p50/frame_p95 the wall time of those frames (advance + render + GPU wait, without --fps's
 * sleep) and cpu_p50/cpu_p95 the process CPU time each of them took, in ms. main_cpu_s is the main
 * thread's own CPU time in those frames and off_cpu_s their wall less it (waits on other threads, on
 * the GPU and on a core; the --fps sleep is outside the frames), main_cpu_p50 and off_cpu_p50/p95
 * the same per frame, in ms.
 * --headless --frames 0 loads the page and never renders it (use it to check that
 * nothing is allocated for rendering before the first render). --hold stops the process (SIGSTOP)
 * after a headless run so vmmap/footprint can inspect it, views still alive; SIGCONT lets it exit.
 * --memory-stats prints, after a headless run, the allocator totals (karouka-memory, kr_memory_stats)
 * and the bytes per subsystem (karouka-memory-usage, kr_system_memory_usage, with the glyph cache's
 * font_cache_strikes after font_cache_mb) to stdout.
 * --mem-phases prints the process footprint (now and lifetime peak) and system malloc bytes to
 * stderr after system and view creation, the load, and each advance/render of the first frames
 * (karouka-mem-phase lines, with ms since exec and CPU ms), and once more after teardown.
 * --mem-phases-heap (implies --mem-phases) also prints each phase's kmalloc, GC heap and canvas bytes
 * (kr_system_query_memory_stats) and the glyph cache's strike count. That walk costs ~70 ms per phase, which then shows up in every later
 * phase's t_ms/cpu_ms, so it is off by default and its own cost is printed as walk_ms.
 * KR_MEM_PHASE_EVERY=N with --mem-phases adds a frame-F phase after every Nth frame.
 * --no-render advances time but never renders, so a run measures parse/style/layout/script apart
 * from rasterization.
 * --max-layouts-per-frame N[@F] fails a headless run (exit 1) if any view's page ran more than N
 * layout update passes in one frame from frame F (0-based, default 0) on, or ran none in any frame,
 * which would mean the counter is broken (kr_view_stats.layout_updates, counted from the end of one
 * frame to the end of the next; the load before the first frame isn't a frame). A page lays out
 * several times a frame while it loads (animations_page.html: 2-4 passes in frames 0-2), hence F;
 * a static page lays out in no frame after that, so N may be 0. --stats reports each
 * view's passes over all frames (layouts=) and the most in one frame (layouts_frame_max=).
 * --print-meta NAME prints the content of the page's first <meta name=NAME> and exits (1 when there
 * is none) without creating a GPU device or a karouka system, with the same scan --check-title uses.
 *
 * Without a page it opens pages/index.html, the demo picker. In a window, the Pages menu switches
 * pages: Cmd+0 the start page, Cmd+1..9 the demo pages, Cmd+Shift+] / Cmd+Shift+[ the next or
 * previous page (start page, demo pages, then test pages), Cmd+[ or Esc back, Cmd+R reload
 * (kr_view_reload: the scroll position is kept).
 *
 * Live reload (as tessera_viewer's): in a window, every 30 frames the viewer stats the files
 * each view's page was loaded from (kr_view_get_dependencies) and reloads the page when one changed
 * (its mtime, or a missing one appearing). --no-reload turns it off. A headless run doesn't watch
 * unless --live-reload asks it to: its frames depend on --frames and the page only.
 *
 * --second opens another view of the same system beside the first; repeat it for more views (four
 * views: --second b.html --second c.html --second d.html). Every view is --size and renders through
 * the system's one Metal device and Skia context. Scripted input goes to the first view, or to view
 * N with --input-view N (1-based); --capture out.png writes view N (N >= 2) to out-N.png; --stats prints one line per view. --close-second F
 * destroys the second view at frame F while the others keep running. In a window, the Pages menu
 * acts on the view that was clicked last.
 *
 * --blur F / --focus F (repeatable) script the window's key state: in frame F the input view's host
 * gets the setFocused: NO / YES that windowDidResignKey / windowDidBecomeKey send every pane, so
 * kr_view_set_focused runs headless. pages/tests/window_focus.html checks it through
 * document.hasFocus().
 *
 * The viewer hands karouka its own Metal device and queue (kr_system_desc.mtl_device/mtl_queue), as
 * a game would: in a window karouka draws each view straight into a private texture of the viewer's
 * (kr_view_set_output2; the view has no IOSurface), which the viewer's command buffer copies into the
 * layer's drawable, and a headless run waits for the GPU itself after each rendered frame (the gpu_*
 * stats). --engine-gpu leaves the GPU to karouka instead (its own device and queue; kr_view_render
 * waits). --output-texture (headless) has karouka draw every view into a
 * private texture of the viewer's the same way, runs a reference view of the same page beside each
 * (same input and clock, rendering into its own IOSurface), and at the end checks each texture
 * against its reference's IOSurface, failing the run if they differ or the view has an IOSurface.
 * --backend software (tessera_viewer's flag; "cpu" too) has karouka rasterize with Skia's CPU
 * rasterizer (kr_system_desc.backend = KR_BACKEND_SOFTWARE): it makes no GPU context and
 * renders into each view's IOSurface, which a window then shows as with --engine-gpu. Headless, the
 * viewer itself makes no Metal device either, so the run needs no GPU. --backend metal is the default.
 * --shader-cache PATH has karouka keep the GPU programs Skia builds in PATH between runs
 * (kr_system_desc.shader_cache_path), so a run's first frames take them from there instead of
 * compiling them; the default is ~/Library/Caches/karouka/skia-shaders.bin, and --no-shader-cache
 * builds every program in every run (the first frame of a cold start). With KR_SHADER_CACHE_DIR=DIR
 * the default is instead the page's own file in DIR, DIR/<name>-<hash of the page's real path>.bin
 * (the first view's page), so a run's precompiled programs are that page's and not whatever page ran
 * last on the machine.
 * With KR_DEBUG=1 the viewer prints what kr_system_create2 cost besides its wall time: the
 * process's CPU, runnable time and instructions, and the main thread's CPU and off-CPU time.
 *
 * --doc-root DIR serves root-relative loads the way a web server rooted at DIR would (as a
 * WPT runner needs: `/fonts/ahem.css` resolves to file:///fonts/ahem.css, which reads
 * DIR/fonts/ahem.css). A file: URL whose path is outside DIR and names no file is read from DIR + its
 * path; every other path is read as it is. Implies --host-io sync unless --host-io names a mode.
 *
 * --testharness runs a WPT testharness.js document and
 * prints what it reported. The viewer is the embedder wptrunner leaves testharnessreport.js to: a read
 * of .../resources/testharnessreport.js that names no file gets testharness_reporter below, which turns
 * the harness's own output off and, on completion, console.logs one report (testharness_marker, then
 * `HARNESS\t<status>\t<message>` and a `SUBTEST\t<status>\t<name>\t<message>` line per test, the format
 * Tessera's tessera_wpt_dom reads back; tabs and newlines in a field become spaces). The run stops at
 * the end of the frame the report arrived in, or after --frames N (the harness's own timeouts are on
 * the page clock: 10 s is 600 frames), and prints the report's lines to stdout. Exit 3 when none came.
 * Implies --host-io sync and --host-log info (both needed to serve and read the report), and needs
 * --headless.
 * It also stands in for wptserve's `?pipe=trickle(dN)` (Tessera's runner does the
 * same): the response waits N seconds of page clock, so the execution-timing tests' "slow"
 * script or sheet is slow. The request URL the host sees keeps its query (Web::FileRequest carries it),
 * and the read is held until the page clock reaches the request's time plus N (only the leading dN;
 * the rest of the pipe is not emulated).
 *
 * --font-hinting none|slight|normal|full sets KR_FONT_HINTING before the engine starts:
 * every glyph rasterizes with that Skia hinting. Without it macOS gets Skia's default, kNormal,
 * which CoreGraphics renders with font smoothing that dilates glyphs past their outlines; reftests
 * want none, as they compare glyph edges.
 *
 * Any view showing a page named hud_page.html is driven by a stand-in game (HudGame below) that sets
 * the page's data-bind-* model with kr_binding_set_* before every kr_view_advance_time.
 */

#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <karouka/karouka.h>

#include "EventInput.h"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <libproc.h>
#include <map>
#include <optional>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

struct ScriptedInput {
    uint32_t frame;
    kr_input_event event;
};

struct Options {
    std::string page;
    uint32_t width { 960 };
    uint32_t height { 640 };
    float scale { 0 }; // 0: 1 for scripted runs, the screen's backing scale otherwise
    uint32_t frames { 0 };
    bool frames_given { false };
    bool headless { false };
    bool hold { false };
    bool stats { false };
    uint32_t fps { 0 }; // --fps: headless pacing, 0 = unpaced
    bool mem_phases { false };
    bool mem_phases_heap { false };
    bool no_render { false };
    bool engine_gpu { false };
    // --shader-cache PATH / --no-shader-cache: kr_system_desc.shader_cache_path. Empty: the default,
    // default_shader_cache_path(page).
    std::string shader_cache_path;
    bool no_shader_cache { false };
    bool output_texture { false };
    // --capture-again N:PATH: frame N, after --frames, captured to PATH (0: none).
    uint32_t capture_again_frames { 0 };
    std::string capture_again;
    // --check-full-raster: the first view's last frame, rasterized whole, must match its partial rasters.
    bool check_full_raster { false };
    // --backend software: KR_BACKEND_SOFTWARE (implies engine_gpu's IOSurface path).
    bool software { false };
    std::string capture;
    std::vector<ScriptedInput> inputs;
    // --blur F / --focus F: in frame F the window resigns / becomes key, as windowDidResignKey and
    // windowDidBecomeKey tell the panes (KRHost setFocused:), for the view that takes scripted input.
    std::vector<std::pair<uint32_t, bool>> focus_changes;
    std::vector<std::string> extra_pages; // --second, in order: views 2, 3, ...
    uint32_t input_view { 1 }; // --input-view: the view (1-based) that receives --scroll/--poke/--move/--key/--blur/--focus
    uint32_t close_second_at { 0 };
    // The host interface (kr_host_interface): --host-io sync|async, --host-log LEVEL, --host-clock.
    enum class HostIo { None, Sync, Async } host_io { HostIo::None };
    std::string doc_root; // --doc-root: file: paths outside it are read from inside it
    int host_log_level { -1 }; // -1: no host log
    bool testharness { false }; // --testharness
    bool host_clock { false };
    // --real-time: the page's clock is the system's (kr_system_desc.real_time_clock).
    bool real_time { false };
    // Live reload: on in a window unless --no-reload, off headless unless --live-reload.
    std::optional<bool> live_reload;
    bool verbose { false };
    bool memory_stats { false };
    // --check-title: each view whose page declares a karouka-check-title meta must end with a title
    // that matches it (title_expectation), or the run exits 1.
    bool check_title { false };
    // --max-layouts-per-frame N@F: the most layout update passes a frame may run, from frame F on;
    // -1 checks nothing.
    int64_t max_layouts_per_frame { -1 };
    uint32_t layout_check_from { 0 };
    // --print-meta NAME: print the content of the page's first <meta name=NAME> (meta_content) and
    // exit without starting karouka; exit 1 when the page has none.
    std::optional<std::string> print_meta;
    // Host fonts (kr_system_add_font_file2): path, family ("" = the font's own), glyph fallback.
    struct Font {
        std::string path;
        std::string family;
        bool fallback { false };
    };
    std::vector<Font> fonts;
    std::string default_font;
    // --font-hinting: KR_FONT_HINTING for the engine ("" = leave the environment as it is).
    std::string font_hinting;
};

// Live reload: how often the files a page was loaded from are stat'ed, as tessera_viewer does.
constexpr uint32_t reload_poll_frames = 30;

// The files one view's page was loaded from, and their modification times as first seen. A new
// entry (the page requested another file) is stat'ed the frame it shows up; every entry is
// compared every reload_poll_frames frames.
class DependencyWatch {
public:
    // The changed file, or "" when none changed.
    std::string poll(kr_view* view, bool compare)
    {
        kr_view_dependencies deps {};
        deps.struct_size = sizeof(kr_view_dependencies);
        if (!view || kr_view_get_dependencies(view, &deps) != KR_OK)
            return {};
        if (deps.revision != m_revision) {
            m_revision = deps.revision;
            for (size_t i = 0; i < deps.count; ++i) {
                if (deps.entries[i].pending)
                    continue;
                auto path = path_of(deps.entries[i].url);
                if (!path.empty() && !m_mtimes.count(path))
                    m_mtimes[path] = mtime_of(path);
            }
        }
        if (compare) {
            for (auto const& [path, mtime] : m_mtimes) {
                if (mtime_of(path) != mtime)
                    return path;
            }
        }
        return {};
    }

    // A new document: its set is the one to watch, as it is first read.
    void reset()
    {
        m_revision = 0;
        m_mtimes.clear();
    }

private:
    using Mtime = std::pair<long, long>;

    static Mtime mtime_of(std::string const& path)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return { -1, -1 };
        return { (long)st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec };
    }

    // The path a file: URL names (percent-decoded), "" for anything else.
    static std::string path_of(char const* url)
    {
        std::string_view rest = url;
        if (rest.substr(0, 7) != "file://")
            return {};
        rest.remove_prefix(7);
        std::string path;
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == '%' && i + 2 < rest.size() && isxdigit((unsigned char)rest[i + 1]) && isxdigit((unsigned char)rest[i + 2])) {
                path += (char)std::stoi(std::string(rest.substr(i + 1, 2)), nullptr, 16);
                i += 2;
            } else {
                path += rest[i];
            }
        }
        return path;
    }

    uint64_t m_revision { 0 };
    std::map<std::string, Mtime> m_mtimes;
};

double now_ms()
{
    return CACurrentMediaTime() * 1000.0;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    auto index = std::min(values.size() - 1, static_cast<size_t>(p * (values.size() - 1) + 0.5));
    return values[index];
}

uint64_t resident_bytes()
{
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
}

// The view's stats, sized as this viewer was compiled (kr_view_stats.struct_size).
kr_view_stats query_view_stats(kr_view* view)
{
    kr_view_stats stats {};
    stats.struct_size = sizeof(stats);
    kr_view_query_stats(view, &stats);
    return stats;
}

// Milliseconds since the kernel started this process (exec), so pre-main time (dyld loading the
// Ladybird dylibs, static initializers) counts toward the first phase.
double ms_since_exec()
{
    proc_bsdinfo info {};
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return 0;
    timeval now {};
    gettimeofday(&now, nullptr);
    return (now.tv_sec - (double)info.pbi_start_tvsec) * 1000.0 + (now.tv_usec - (double)info.pbi_start_tvusec) / 1000.0;
}

// The process's current and lifetime-peak physical footprint (what /usr/bin/time -l reports as
// "peak memory footprint"), the system malloc zones' bytes in use, and the time since exec and the
// CPU time used so far (for startup breakdowns), for --mem-phases.
void report_mem_phase(char const* phase)
{
    rusage_info_v4 info {};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return;
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    double cpu_ms = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
    fprintf(stderr, "karouka-mem-phase %-14s footprint_mb=%.1f peak_mb=%.1f malloc_mb=%.1f t_ms=%.1f cpu_ms=%.1f\n", phase,
        info.ri_phys_footprint / 1048576.0, info.ri_lifetime_max_phys_footprint / 1048576.0, mstats().bytes_used / 1048576.0,
        ms_since_exec(), cpu_ms);
}

bool s_mem_phases = false;
bool s_mem_phases_heap = false;
kr_system* s_mem_system = nullptr;

// kmalloc's pages (VM tag 240) that are dirty but still flagged reusable: mimalloc purged them with
// MADV_FREE_REUSABLE and wrote to them again without MADV_FREE_REUSE, so neither kmalloc_mb nor the
// physical footprint counts them until the kernel takes them back.
double kmalloc_reusable_dirty_mb()
{
    double total = 0;
    mach_vm_address_t address = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS)
            break;
        if (info.user_tag == VM_MEMORY_APPLICATION_SPECIFIC_1 && info.protection != VM_PROT_NONE) {
            static int dispositions[4096];
            for (mach_vm_size_t offset = 0; offset < size; offset += 4096 * vm_page_size) {
                mach_vm_size_t pages = std::min<mach_vm_size_t>(4096, (size - offset) / vm_page_size);
                if (mach_vm_page_range_query(mach_task_self(), address + offset, pages * vm_page_size, (mach_vm_address_t)dispositions, &pages) != KERN_SUCCESS)
                    break;
                for (mach_vm_size_t i = 0; i < pages; ++i)
                    if ((dispositions[i] & VM_PAGE_QUERY_PAGE_DIRTY) && (dispositions[i] & VM_PAGE_QUERY_PAGE_REUSABLE))
                        total += vm_page_size;
            }
        }
        address += size;
    }
    return total / 1048576.0;
}

// KR_MEM_HOLD=<phase> stops the process (SIGSTOP) right after that phase is reported, for vmmap.
// With --mem-phases-heap, once the system exists, also prints its kmalloc (mimalloc, VM tag 240)
// and GC heap dirty bytes and how long that walk took (it is not free), and, when karouka_miprof is inserted (DYLD_INSERT_LIBRARIES), has it write its
// per-call-site snapshot for the phase.
void mem_phase(char const* phase)
{
    if (!s_mem_phases)
        return;
    report_mem_phase(phase);
    if (s_mem_phases_heap && s_mem_system) {
        double walk_start = now_ms();
        kr_memory_stats memory {};
        memory.struct_size = sizeof(memory);
        kr_system_query_memory_stats(s_mem_system, &memory);
        double walk_ms = now_ms() - walk_start;
        fprintf(stderr, "karouka-mem-phase %-14s kmalloc_mb=%.2f kmalloc_reusable_dirty_mb=%.2f gc_heap_mb=%.2f gc_live_mb=%.2f decoded_images_mb=%.2f decoded_images_peak_mb=%.2f canvas_mb=%.2f font_cache_strikes=%llu walk_ms=%.1f\n", phase,
            memory.kmalloc_bytes / 1048576.0, kmalloc_reusable_dirty_mb(), memory.gc_heap_bytes / 1048576.0,
            kr_system_memory_usage(s_mem_system, KR_MEM_TAG_GC_HEAP_LIVE) / 1048576.0,
            kr_system_memory_usage(s_mem_system, KR_MEM_TAG_DECODED_IMAGES) / 1048576.0,
            kr_system_memory_usage(s_mem_system, KR_MEM_TAG_DECODED_IMAGES_PEAK) / 1048576.0,
            kr_system_memory_usage(s_mem_system, KR_MEM_TAG_CANVAS) / 1048576.0, (unsigned long long)memory.font_cache_strikes, walk_ms);
    }
    using MarkFunction = void (*)(char const*);
    static auto mark = reinterpret_cast<MarkFunction>(dlsym(RTLD_DEFAULT, "kr_miprof_mark"));
    if (mark)
        mark(phase);
    if (auto hold = getenv("KR_MEM_HOLD"); hold && strcmp(hold, phase) == 0)
        raise(SIGSTOP);
}

// The viewer's GPU, windowed or headless, picked the way LibGfx's get_metal_context() picks the
// engine's (patch 0019), so --engine-gpu draws on the device the window's layers present from: the
// high-power, built-in GPU, which is what MTLCreateSystemDefaultDevice() returns on a dual-GPU Mac.
// MTLCopyAllDevices() also skips the window-server handshake MTLCreateSystemDefaultDevice() makes
// first, ~25 ms that a headless run does not need (a window pays it when NSApplication starts).
id<MTLDevice> pick_metal_device()
{
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> device in devices) {
        if (!device.isLowPower && !device.isRemovable)
            return device;
    }
    return devices.firstObject ?: MTLCreateSystemDefaultDevice();
}

double cpu_seconds()
{
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

// Per-frame timings, reported in one line at exit so the bench harness can parse it.
struct FrameStats {
    std::vector<double> frame, update, record, raster, gpu;
    // Device pixels each rendered frame rasterized (kr_view_stats.last_raster_pixels).
    std::vector<double> raster_pixels;
    // Process CPU ms each frame took (getrusage over the same span as `frame`), and the wall clock and
    // process CPU time at the end of the first and of the last frame: the frame-loop line.
    std::vector<double> cpu;
    // The main thread's own CPU ms in each frame (CLOCK_THREAD_CPUTIME_ID), so the frame's off-CPU
    // time is its wall less that: waits on other threads, on the GPU and on a core.
    std::vector<double> main_cpu;
    double first_end_ms { 0 }, first_end_cpu { 0 }, last_end_ms { 0 }, last_end_cpu { 0 };
    double start_ms { now_ms() };
    double start_cpu { cpu_seconds() };
    uint64_t peak_rss { 0 };
    uint32_t rendered { 0 };
    // kr_view_stats.layout_updates at the end of the last frame (after the load before the first),
    // the layout update passes the frames ran and the most one frame ran, over every frame and over
    // the frames --max-layouts-per-frame checks (from layout_check_from on); with the first frame
    // that ran that many.
    uint64_t layouts_at_frame_end { 0 };
    struct Layouts {
        uint64_t total { 0 };
        uint64_t most { 0 };
        size_t most_at { 0 };
        void add(uint64_t in_frame, size_t index)
        {
            total += in_frame;
            if (in_frame > most) {
                most = in_frame;
                most_at = index;
            }
        }
    } layouts, checked_layouts;
    size_t layout_check_from { 0 };

    void record_layouts(uint64_t layout_updates)
    {
        uint64_t in_frame = layout_updates - layouts_at_frame_end;
        layouts_at_frame_end = layout_updates;
        layouts.add(in_frame, frame.size());
        if (frame.size() >= layout_check_from)
            checked_layouts.add(in_frame, frame.size());
    }

    void record_frame(double frame_ms, double cpu_ms, double main_cpu_ms, double end_ms, double end_cpu)
    {
        frame.push_back(frame_ms);
        cpu.push_back(cpu_ms);
        main_cpu.push_back(main_cpu_ms);
        if (frame.size() == 1) {
            first_end_ms = end_ms;
            first_end_cpu = end_cpu;
        }
        last_end_ms = end_ms;
        last_end_cpu = end_cpu;
    }

    // The frame loop after the first frame, in the format bench/compare.py reads from both
    // viewers: totals in seconds, per-frame percentiles in ms.
    void report_frame_loop(char const* page)
    {
        size_t frames = frame.size() > 1 ? frame.size() - 1 : 0;
        std::vector<double> loop_frame(frame.begin() + (frames ? 1 : frame.size()), frame.end());
        std::vector<double> loop_cpu(cpu.begin() + (frames ? 1 : cpu.size()), cpu.end());
        std::vector<double> loop_main_cpu(main_cpu.begin() + (frames ? 1 : main_cpu.size()), main_cpu.end());
        std::vector<double> loop_off_cpu;
        double main_cpu_s = 0, off_cpu_s = 0;
        for (size_t i = 0; i < loop_frame.size(); ++i) {
            loop_off_cpu.push_back(std::max(0.0, loop_frame[i] - loop_main_cpu[i]));
            main_cpu_s += loop_main_cpu[i] / 1000.0;
            off_cpu_s += loop_off_cpu.back() / 1000.0;
        }
        double wall = frames ? (last_end_ms - first_end_ms) / 1000.0 : 0;
        double cpu_s = frames ? last_end_cpu - first_end_cpu : 0;
        printf("frame-loop engine=karouka page=%s frames=%zu wall_s=%.6f cpu_s=%.6f cpu_pct=%.2f "
               "frame_p50=%.4f frame_p95=%.4f cpu_p50=%.4f cpu_p95=%.4f "
               "main_cpu_s=%.6f off_cpu_s=%.6f main_cpu_p50=%.4f off_cpu_p50=%.4f off_cpu_p95=%.4f\n",
            page, frames, wall, cpu_s, wall > 0 ? 100.0 * cpu_s / wall : 0.0,
            percentile(loop_frame, 0.5), percentile(loop_frame, 0.95),
            percentile(loop_cpu, 0.5), percentile(loop_cpu, 0.95),
            main_cpu_s, off_cpu_s, percentile(loop_main_cpu, 0.5),
            percentile(loop_off_cpu, 0.5), percentile(loop_off_cpu, 0.95));
        fflush(stdout);
    }

    void report(char const* page, uint64_t updates)
    {
        auto wall = (now_ms() - start_ms) / 1000.0;
        auto cpu = cpu_seconds() - start_cpu;
        printf("karouka-stats page=%s frames=%zu rendered=%u updates=%llu layouts=%llu layouts_frame_max=%llu "
               "wall_s=%.3f cpu_s=%.3f cpu_pct=%.1f "
               "frame_p50=%.3f frame_p95=%.3f frame_max=%.3f update_p50=%.3f update_p95=%.3f "
               "record_p50=%.3f record_p95=%.3f raster_p50=%.3f raster_p95=%.3f gpu_p50=%.3f gpu_p95=%.3f "
               "raster_px_p50=%.0f raster_px_p95=%.0f rss_mb=%.1f peak_rss_mb=%.1f\n",
            page, frame.size(), rendered, (unsigned long long)updates, (unsigned long long)layouts.total,
            (unsigned long long)layouts.most, wall, cpu, wall > 0 ? 100.0 * cpu / wall : 0.0,
            percentile(frame, 0.5), percentile(frame, 0.95), percentile(frame, 1.0),
            percentile(update, 0.5), percentile(update, 0.95),
            percentile(record, 0.5), percentile(record, 0.95),
            percentile(raster, 0.5), percentile(raster, 0.95),
            percentile(gpu, 0.5), percentile(gpu, 0.95),
            percentile(raster_pixels, 0.5), percentile(raster_pixels, 0.95),
            resident_bytes() / 1048576.0, std::max(peak_rss, resident_bytes()) / 1048576.0);
        fflush(stdout);
    }
};

bool parse_xy(char const* text, float& x, float& y)
{
    return sscanf(text, "%f,%f", &x, &y) == 2;
}

void usage()
{
    fprintf(stderr, "usage: karouka_viewer [page.html] [--size WxH] [--scale N] [--frames N] [--headless]\n"
                    "                      [--capture out.png] [--stats] [--scroll DX,DY[@F|@A..B]]\n"
                    "                      [--poke X,Y@F] [--move X0,Y0:X1,Y1@F[xSTEPS]] [--key VK@F[..L]] [--blur F] [--focus F] [--hold]\n"
                    "                      [--second page2.html]... [--input-view N] [--close-second F] [--mem-phases] [--mem-phases-heap]\n"
                    "                      [--no-render] [--host-io sync|async] [--host-log trace|debug|info|warn|error] [--host-clock] [--real-time] [--doc-root DIR]\n"
                    "                      [--verbose] [--memory-stats] [--check-title] [--max-layouts-per-frame N[@F]] [--engine-gpu] [--output-texture]\n"
                    "                      [--shader-cache PATH | --no-shader-cache] [--capture-again N:out.png] [--check-full-raster]\n"
                    "                      [--backend metal|software] [--fps N]\n"
                    "                      [--print-meta NAME] [--no-reload] [--live-reload] [--testharness]\n"
                    "                      [--font PATH[=FAMILY]]... [--fallback-font PATH]... [--default-font FAMILY]\n"
                    "                      [--font-hinting none|slight|normal|full]\n");
}

// Where the viewer keeps Skia's programs between runs, as a game would ship or cache them
// (kr_system_desc.shader_cache_path): ~/Library/Caches/karouka/skia-shaders.bin, or PAGE's
// own file under $KR_SHADER_CACHE_DIR. Empty without either.
std::string default_shader_cache_path(std::string const& page)
{
    // KR_SHADER_CACHE_DIR keeps one file per page, keyed by the page's real path (the
    // argument as given when it names no file, as a URL does). FNV-1a, so the name is the same in
    // every build; the page's file name in front of it only for whoever lists the directory.
    if (char const* dir = getenv("KR_SHADER_CACHE_DIR"); dir && *dir) {
        std::error_code error;
        std::string key = std::filesystem::exists(page, error) ? std::filesystem::canonical(page, error).string() : std::string();
        if (key.empty())
            key = page;
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : key)
            hash = (hash ^ c) * 1099511628211ull;
        std::string name = std::filesystem::path(key).stem().string().substr(0, 40);
        for (char& c : name) {
            if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
                c = '_';
        }
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "-%016llx.bin", (unsigned long long)hash);
        return std::string(dir) + "/" + name + suffix;
    }
    char const* home = getenv("HOME");
    if (!home || !*home)
        return {};
    return std::string(home) + "/Library/Caches/karouka/skia-shaders.bin";
}

bool parse_options(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> char const* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--size") {
            auto value = next();
            if (!value || sscanf(value, "%ux%u", &options.width, &options.height) != 2)
                return false;
        } else if (arg == "--scale") {
            auto value = next();
            if (!value)
                return false;
            options.scale = strcmp(value, "auto") == 0 ? 0 : atof(value);
        } else if (arg == "--frames") {
            auto value = next();
            if (!value)
                return false;
            options.frames = atoi(value);
            options.frames_given = true;
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--hold") {
            options.hold = true;
        } else if (arg == "--mem-phases") {
            options.mem_phases = true;
        } else if (arg == "--mem-phases-heap") {
            options.mem_phases = true;
            options.mem_phases_heap = true;
        } else if (arg == "--no-render") {
            options.no_render = true;
        } else if (arg == "--engine-gpu") {
            options.engine_gpu = true;
        } else if (arg == "--shader-cache") {
            auto value = next();
            if (!value || !*value) {
                fprintf(stderr, "--shader-cache needs a path\n");
                return false;
            }
            options.shader_cache_path = value;
        } else if (arg == "--no-shader-cache") {
            options.no_shader_cache = true;
        } else if (arg == "--output-texture") {
            options.output_texture = true;
        } else if (arg == "--backend") {
            auto value = next();
            if (!value)
                return false;
            if (strcmp(value, "software") == 0 || strcmp(value, "cpu") == 0)
                options.software = true;
            else if (strcmp(value, "metal") == 0)
                options.software = false;
            else
                return false;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--fps") {
            auto value = next();
            if (!value)
                return false;
            options.fps = (uint32_t)atoi(value);
            if (options.fps == 0 || options.fps > 1000) {
                fprintf(stderr, "--fps wants 1..1000\n");
                return false;
            }
        } else if (arg == "--capture") {
            auto value = next();
            if (!value)
                return false;
            options.capture = value;
        } else if (arg == "--capture-again") {
            auto value = next();
            unsigned frames = 0;
            int consumed = 0;
            if (!value || sscanf(value, "%u:%n", &frames, &consumed) != 1 || !consumed || !value[consumed]) {
                fprintf(stderr, "--capture-again needs N:PATH\n");
                return false;
            }
            options.capture_again_frames = frames;
            options.capture_again = value + consumed;
        } else if (arg == "--check-full-raster") {
            options.check_full_raster = true;
        } else if (arg == "--scroll") {
            auto value = next();
            float dx, dy;
            if (!value || !parse_xy(value, dx, dy))
                return false;
            uint32_t first = 5, last = 5;
            if (auto at = strchr(value, '@')) {
                if (sscanf(at + 1, "%u..%u", &first, &last) != 2)
                    last = first = atoi(at + 1);
            }
            for (uint32_t frame = first; frame <= last; ++frame) {
                kr_input_event event {};
                event.struct_size = sizeof(event);
                event.type = KR_INPUT_WHEEL;
                event.delta_x = dx;
                event.delta_y = dy;
                options.inputs.push_back({ frame, event }); // position filled in at dispatch: the view's center
            }
        } else if (arg == "--poke") {
            auto value = next();
            float x, y;
            auto at = value ? strchr(value, '@') : nullptr;
            if (!value || !parse_xy(value, x, y))
                return false;
            uint32_t frame = at ? atoi(at + 1) : 12;
            for (auto [offset, type] : { std::pair { 0u, KR_INPUT_MOUSE_MOVE }, { 1u, KR_INPUT_MOUSE_DOWN }, { 2u, KR_INPUT_MOUSE_UP } }) {
                kr_input_event event {};
                event.struct_size = sizeof(event);
                event.type = type;
                event.x = x;
                event.y = y;
                options.inputs.push_back({ frame + offset, event });
            }
        } else if (arg == "--move") {
            auto value = next();
            float x0, y0, x1, y1;
            uint32_t frame = 5, steps = 8;
            if (!value || sscanf(value, "%f,%f:%f,%f@%u", &x0, &y0, &x1, &y1, &frame) < 4)
                return false;
            if (auto times = strrchr(value, 'x'))
                steps = std::max(1, atoi(times + 1));
            for (uint32_t step = 0; step < steps; ++step) {
                float t = steps == 1 ? 1.0f : static_cast<float>(step) / (steps - 1);
                kr_input_event event {};
                event.struct_size = sizeof(event);
                event.type = KR_INPUT_MOUSE_MOVE;
                event.x = x0 + (x1 - x0) * t;
                event.y = y0 + (y1 - y0) * t;
                options.inputs.push_back({ frame + step, event });
            }
        } else if (arg == "--key") {
            // --key VK[@F[..L]]: a key press (down, then up) of macOS virtual key code VK in frame F (default 5),
            // or in every frame from F to L; 125 is the down arrow.
            auto value = next();
            if (!value)
                return false;
            uint32_t key_code = atoi(value), first = 5, last = 5;
            if (auto at = strchr(value, '@')) {
                if (sscanf(at + 1, "%u..%u", &first, &last) != 2)
                    last = first = atoi(at + 1);
            }
            for (uint32_t frame = first; frame <= last; ++frame) {
                for (auto type : { KR_INPUT_KEY_DOWN, KR_INPUT_KEY_UP }) {
                    kr_input_event event {};
                    event.struct_size = sizeof(event);
                    event.type = type;
                    event.key_code = key_code;
                    options.inputs.push_back({ frame, event });
                }
            }
        } else if (arg == "--blur" || arg == "--focus") {
            // --blur F / --focus F: the window resigns / becomes key in frame F.
            auto value = next();
            if (!value)
                return false;
            options.focus_changes.push_back({ static_cast<uint32_t>(atoi(value)), arg == "--focus" });
        } else if (arg == "--second") {
            auto value = next();
            if (!value)
                return false;
            options.extra_pages.push_back(value);
        } else if (arg == "--input-view") {
            auto value = next();
            if (!value || atoi(value) < 1)
                return false;
            options.input_view = atoi(value);
        } else if (arg == "--close-second") {
            auto value = next();
            if (!value)
                return false;
            options.close_second_at = atoi(value);
        } else if (arg == "--host-io") {
            auto value = next();
            if (!value || (strcmp(value, "sync") != 0 && strcmp(value, "async") != 0))
                return false;
            options.host_io = strcmp(value, "sync") == 0 ? Options::HostIo::Sync : Options::HostIo::Async;
        } else if (arg == "--doc-root") {
            auto value = next();
            if (!value || !*value)
                return false;
            options.doc_root = value;
            while (options.doc_root.size() > 1 && options.doc_root.back() == '/')
                options.doc_root.pop_back();
        } else if (arg == "--host-log") {
            auto value = next();
            char const* levels[] = { "trace", "debug", "info", "warn", "error" };
            for (int level = 0; value && level < 5; ++level) {
                if (strcmp(value, levels[level]) == 0)
                    options.host_log_level = level;
            }
            if (options.host_log_level < 0)
                return false;
        } else if (arg == "--font" || arg == "--fallback-font") {
            auto value = next();
            if (!value)
                return false;
            std::string spec = value;
            auto equals = spec.find('=');
            Options::Font font;
            font.path = spec.substr(0, equals);
            if (equals != std::string::npos)
                font.family = spec.substr(equals + 1);
            font.fallback = arg == "--fallback-font";
            options.fonts.push_back(font);
        } else if (arg == "--default-font") {
            auto value = next();
            if (!value)
                return false;
            options.default_font = value;
        } else if (arg == "--font-hinting") {
            auto value = next();
            if (!value)
                return false;
            std::string hinting = value;
            if (hinting != "none" && hinting != "slight" && hinting != "normal" && hinting != "full")
                return false;
            options.font_hinting = hinting;
        } else if (arg == "--host-clock") {
            options.host_clock = true;
        } else if (arg == "--real-time") {
            options.real_time = true;
        } else if (arg == "--no-reload") {
            options.live_reload = false;
        } else if (arg == "--live-reload") {
            options.live_reload = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--memory-stats") {
            options.memory_stats = true;
        } else if (arg == "--check-title") {
            options.check_title = true;
        } else if (arg == "--testharness") {
            options.testharness = true;
        } else if (arg == "--max-layouts-per-frame") {
            char const* value = next();
            long long limit = -1;
            unsigned from = 0;
            int consumed = 0;
            bool parsed = value
                && (sscanf(value, "%lld@%u%n", &limit, &from, &consumed) == 2 || (from = 0, sscanf(value, "%lld%n", &limit, &consumed) == 1))
                && value[consumed] == '\0' && limit >= 0;
            if (!parsed) {
                fprintf(stderr, "--max-layouts-per-frame needs N or N@FRAME\n");
                return false;
            }
            options.max_layouts_per_frame = limit;
            options.layout_check_from = from;
        } else if (arg == "--print-meta") {
            auto value = next();
            if (!value)
                return false;
            options.print_meta = value;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else if (arg.rfind("--", 0) == 0) {
            fprintf(stderr, "unknown flag %s\n", arg.c_str());
            return false;
        } else {
            options.page = arg;
        }
    }
    if (options.headless && !options.frames_given) {
        fprintf(stderr, "--headless needs --frames N\n");
        return false;
    }
    if (options.output_texture && (!options.headless || options.engine_gpu || options.software)) {
        fprintf(stderr, "--output-texture needs --headless and the viewer's GPU (no --engine-gpu or --backend software)\n");
        return false;
    }
    if (options.capture_again_frames && (!options.headless || options.testharness || options.capture_again_frames <= options.frames)) {
        fprintf(stderr, "--capture-again N:PATH needs --headless and N past --frames (no --testharness)\n");
        return false;
    }
    if (options.check_full_raster && (!options.headless || options.engine_gpu || options.software || options.output_texture)) {
        fprintf(stderr, "--check-full-raster needs --headless and the viewer's GPU (no --engine-gpu, --backend software or --output-texture)\n");
        return false;
    }
    // The software backend has no GPU to share: the view renders into its IOSurface, which a window
    // shows the way --engine-gpu's does.
    if (options.software)
        options.engine_gpu = true;
    if (options.testharness && !options.headless) {
        fprintf(stderr, "--testharness needs --headless\n");
        return false;
    }
    if (options.hold && !options.headless) {
        fprintf(stderr, "--hold needs --headless\n");
        return false;
    }
    if (options.fps && !options.headless) {
        fprintf(stderr, "--fps needs --headless (a window paces to the display)\n");
        return false;
    }
    return true;
}

// The viewer as a host (kr_host_interface): it reads file: URLs itself, from inside the request
// (sync) or at the start of the next frame (async), prints the log to stderr, and lends its clock.
namespace HostCallbacks {

kr_system* s_system;
std::string s_doc_root; // --doc-root
bool s_testharness;      // --testharness
std::optional<std::string> s_testharness_report;

// What --testharness serves as testharnessreport.js. setup({output: false}) as wptrunner's own does;
// the report goes through console.log, which the viewer's write_log reads (category "console").
constexpr char testharness_marker[] = "karouka-testharness-report\n";
constexpr char testharness_reporter[] = R"JS(setup({output: false});
add_completion_callback(function (tests, harness_status) {
    var subtest_status = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
    var harness_statuses = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
    function field(s) { return String(s === null || s === undefined ? '' : s).replace(/[\r\n\t]/g, ' '); }
    var lines = ['HARNESS\t' + (harness_statuses[harness_status.status] || 'UNKNOWN(' + harness_status.status + ')') +
                 '\t' + field(harness_status.message)];
    for (var i = 0; i < tests.length; i++) {
        var t = tests[i];
        lines.push('SUBTEST\t' + (subtest_status[t.status] || 'UNKNOWN(' + t.status + ')') + '\t' + field(t.name) +
                   '\t' + field(t.message));
    }
    console.log('karouka-testharness-report\n' + lines.join('\n'));
});
)JS";

bool ends_with(std::string const& text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::vector<std::pair<uint64_t, std::string>> s_queued_reads;
uint64_t s_reads, s_cancels;

// --testharness: reads held for `?pipe=trickle(dN)` until the page clock reaches due_ms.
struct DelayedRead {
    uint64_t id;
    std::string path;
    double due_ms;
};
std::vector<DelayedRead> s_delayed_reads;
// The page clock of the frame being advanced (the time the viewer passes kr_view_advance).
double s_page_time_ms;

// The delay wptserve's `?pipe=trickle(dN...)` in the URL's query starts the response with, in ms; 0 when
// the query has none.
double trickle_delay_ms(std::string_view url)
{
    auto query = url.find('?');
    if (query == std::string_view::npos)
        return 0;
    auto at = url.find("pipe=trickle(d", query);
    if (at == std::string_view::npos)
        return 0;
    std::string seconds(url.substr(at + 14, url.find_first_not_of("0123456789.", at + 14) - (at + 14)));
    double value = seconds.empty() ? 0 : atof(seconds.c_str());
    return value > 0 ? value * 1000.0 : 0;
}

std::map<uint64_t, std::vector<char>> s_borrowed;

std::string path_of_file_url(char const* url)
{
    std::string path = strncmp(url, "file://", 7) == 0 ? url + 7 : url;
    // The query and fragment aren't the file's (the query is there for `?pipe=`, trickle_delay_ms).
    path = path.substr(0, path.find_first_of("?#"));
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() && isxdigit(path[i + 1]) && isxdigit(path[i + 2])) {
            decoded += static_cast<char>(strtol(path.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            decoded += path[i];
        }
    }
    // --doc-root: a root-relative load the file system doesn't have. Files that exist where they are
    // (the host's own fonts, --font) are read from there.
    if (!s_doc_root.empty() && decoded.rfind(s_doc_root + "/", 0) != 0 && access(decoded.c_str(), F_OK) != 0)
        decoded = s_doc_root + decoded;
    return decoded;
}

void complete_read(uint64_t id, std::string const& path)
{
    ++s_reads;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file && s_testharness && ends_with(path, "/resources/testharnessreport.js")) {
        kr_io_complete(s_system, id, KR_OK, testharness_reporter, sizeof(testharness_reporter) - 1, KR_IO_OWNERSHIP_COPY);
        return;
    }
    if (!file) {
        kr_io_complete(s_system, id, KR_ERROR_IO, nullptr, 0, KR_IO_OWNERSHIP_COPY);
        return;
    }
    std::vector<char> bytes;
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), file)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + n);
    fclose(file);
    // Borrowed: karouka hands the buffer back through release before kr_io_complete returns.
    auto const& borrowed = s_borrowed[id] = std::move(bytes);
    if (kr_io_complete(s_system, id, KR_OK, borrowed.data(), borrowed.size(), KR_IO_OWNERSHIP_BORROW_UNTIL_RELEASE) != KR_OK)
        s_borrowed.erase(id); // not pending any more (cancelled): nothing was released
    else if (s_borrowed.count(id))
        fprintf(stderr, "karouka_viewer: request %llu was completed but its buffer not released\n", (unsigned long long)id);
}

kr_result request(void* user_data, char const* url, uint64_t id)
{
    auto mode = static_cast<Options::HostIo>(reinterpret_cast<uintptr_t>(user_data));
    if (s_testharness) {
        if (double delay = trickle_delay_ms(url); delay > 0) {
            s_delayed_reads.push_back({ id, path_of_file_url(url), s_page_time_ms + delay });
            return KR_OK;
        }
    }
    if (mode == Options::HostIo::Async)
        s_queued_reads.push_back({ id, path_of_file_url(url) });
    else
        complete_read(id, path_of_file_url(url));
    return KR_OK;
}

void cancel(void*, uint64_t id)
{
    ++s_cancels;
    std::erase_if(s_queued_reads, [id](auto const& read) { return read.first == id; });
    std::erase_if(s_delayed_reads, [id](auto const& read) { return read.id == id; });
}

void release(void*, uint64_t id, void const*)
{
    s_borrowed.erase(id);
}

void complete_queued_reads()
{
    auto reads = std::move(s_queued_reads);
    s_queued_reads.clear();
    for (auto const& [id, path] : reads)
        complete_read(id, path);
}

// The held reads whose time has come by `now_ms` on the page clock, earliest due first.
void complete_delayed_reads(double now_ms)
{
    std::vector<DelayedRead> due;
    std::erase_if(s_delayed_reads, [&](DelayedRead& read) {
        if (read.due_ms > now_ms)
            return false;
        due.push_back(std::move(read));
        return true;
    });
    std::stable_sort(due.begin(), due.end(), [](auto const& a, auto const& b) { return a.due_ms < b.due_ms; });
    for (auto const& read : due)
        complete_read(read.id, read.path);
}

void write_log(void*, kr_log_level level, char const* category, char const* message)
{
    if (s_testharness && strcmp(category, "console") == 0 && strncmp(message, testharness_marker, sizeof(testharness_marker) - 1) == 0) {
        if (!s_testharness_report)
            s_testharness_report = std::string(message + sizeof(testharness_marker) - 1);
        return;
    }
    char const* names[] = { "trace", "debug", "info", "warn", "error" };
    fprintf(stderr, "[%s %s] %s\n", category, level <= KR_LOG_ERROR ? names[level] : "?", message);
}

uint64_t monotonic_ns(void*)
{
    return static_cast<uint64_t>(now_ms() * 1'000'000.0);
}

}

std::string default_page()
{
    // pages/index.html next to the source tree this viewer was built from.
    return std::string(KAROUKA_SOURCE_DIR) + "/pages/index.html";
}

struct PageEntry {
    std::string path;
    std::string label;
    bool is_demo;
};

// What the Pages menu switches between: the start page, the demo pages it links to (in its order),
// then pages/tests/*.html.
std::vector<PageEntry> page_list()
{
    namespace fs = std::filesystem;
    std::vector<PageEntry> pages;
    fs::path root = fs::path(KAROUKA_SOURCE_DIR) / "pages";
    pages.push_back({ (root / "index.html").string(), "Start page", false });

    std::ifstream index(root / "index.html");
    std::stringstream html;
    html << index.rdbuf();
    std::string text = html.str();
    std::regex href(R"re(href="(demo/[^"]+\.html)")re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), href); it != std::sregex_iterator(); ++it) {
        fs::path page = root / (*it)[1].str();
        if (fs::exists(page))
            pages.push_back({ page.string(), page.stem().string(), true });
    }

    std::vector<fs::path> tests;
    std::error_code error;
    for (auto const& entry : fs::directory_iterator(root / "tests", error))
        if (entry.path().extension() == ".html")
            tests.push_back(entry.path());
    std::sort(tests.begin(), tests.end());
    for (auto const& test : tests)
        pages.push_back({ test.string(), "tests/" + test.stem().string(), false });
    return pages;
}

// A stand-in game for pages/demo/hud_page.html, which has no script: its 20 data-bind-* values (16
// texts, 4 meter widths) read model paths this sets with kr_binding_set_* before every advance. Each
// value is a function of the game frame alone, the same ones Tessera's copy of the page computes in
// requestAnimationFrame, so captures and bench runs are deterministic. Paths resolve once per view.
class HudGame {
public:
    static bool drives(std::string const& page) { return std::filesystem::path(page).filename() == "hud_page.html"; }

    void restart() { m_frame = 0; }

    void step(kr_view* view)
    {
        if (!m_resolved && !resolve(view))
            return;
        uint32_t frame = ++m_frame;
        static char const* const weapons[] = { "RIFLE", "SMG", "SHOTGUN" };
        double const numbers[] = {
            double(frame % 100),            // objective.capture
            double(frame % 7),              // objective.escort
            double(frame % 60),             // objective.hold
            double(300 - frame % 300),      // round.remaining
            double(frame) * 125,            // match.score
            double(frame >> 2),             // match.kills
            1200 + double(frame) * 3,       // player.x
            840 - double(frame) * 2,        // player.y
            double((frame * 9) % 360),      // player.heading
            double(100 - (frame * 3) % 100),  // player.health
            double(100 - (frame * 7) % 100),  // player.shield
            double(100 - (frame * 11) % 100), // player.stamina
            double((frame * 5) % 100),      // player.xp
            double(1 + (frame >> 5)),       // player.level
            double(30 - frame % 31),        // weapon.ammo
            double(frame % 4),              // weapon.grenades
            double(500 - frame % 500),      // alert.range
        };
        static_assert(sizeof numbers / sizeof numbers[0] == number_count);
        for (size_t i = 0; i < number_count; ++i)
            (void)kr_binding_set_number(view, m_paths[i], numbers[i]);
        char const* weapon = weapons[(frame >> 4) % 3];
        (void)kr_binding_set_string(view, m_paths[number_count], weapon, strlen(weapon));
    }

private:
    static constexpr size_t number_count = 17;
    static constexpr char const* paths[] = {
        "objective.capture", "objective.escort", "objective.hold", "round.remaining", "match.score",
        "match.kills", "player.x", "player.y", "player.heading", "player.health", "player.shield",
        "player.stamina", "player.xp", "player.level", "weapon.ammo", "weapon.grenades", "alert.range",
        "weapon.name",
    };
    static_assert(sizeof paths / sizeof paths[0] == number_count + 1);

    bool resolve(kr_view* view)
    {
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
            if (kr_binding_path_resolve(view, paths[i], &m_paths[i]) != KR_OK) {
                fprintf(stderr, "karouka_viewer: cannot resolve model path %s\n", paths[i]);
                return false;
            }
        }
        m_resolved = true;
        return true;
    }

    kr_binding_path m_paths[number_count + 1] {};
    bool m_resolved { false };
    uint32_t m_frame { 0 };
};

bool same_file(std::string const& a, std::string const& b)
{
    std::error_code error;
    return std::filesystem::equivalent(a, b, error);
}

// The content attribute of the first <meta> in `html` whose name attribute is `name` (ASCII
// case-insensitive, as HTML compares meta names). Attributes may come in any order, quoted with " or
// ' or unquoted; the first of a repeated attribute counts. Values are taken as written (no
// character references). nullopt when no such meta exists; "" when it has no content.
std::optional<std::string> meta_content(std::string const& html, std::string_view name)
{
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    auto equals_ci = [&](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
    };
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    auto const size = html.size();
    for (size_t i = 0; (i = html.find('<', i)) != std::string::npos;) {
        ++i;
        if (i + 4 > size || !equals_ci(std::string_view(html).substr(i, 4), "meta"))
            continue;
        i += 4;
        if (i < size && !is_space(html[i]) && html[i] != '/' && html[i] != '>')
            continue; // <metadata> and the like
        std::optional<std::string> tag_name, content;
        while (i < size) {
            while (i < size && (is_space(html[i]) || html[i] == '/'))
                ++i;
            if (i >= size || html[i] == '>')
                break;
            auto start = i;
            while (i < size && !is_space(html[i]) && html[i] != '=' && html[i] != '>' && html[i] != '/')
                ++i;
            auto attribute = std::string_view(html).substr(start, i - start);
            while (i < size && is_space(html[i]))
                ++i;
            std::string value;
            if (i < size && html[i] == '=') {
                ++i;
                while (i < size && is_space(html[i]))
                    ++i;
                if (i < size && (html[i] == '"' || html[i] == '\'')) {
                    auto close = html.find(html[i], i + 1);
                    if (close == std::string::npos)
                        close = size;
                    value = html.substr(i + 1, close - i - 1);
                    i = std::min(close + 1, size);
                } else {
                    start = i;
                    while (i < size && !is_space(html[i]) && html[i] != '>')
                        ++i;
                    value = html.substr(start, i - start);
                }
            }
            if (equals_ci(attribute, "name") && !tag_name)
                tag_name = std::move(value);
            else if (equals_ci(attribute, "content") && !content)
                content = std::move(value);
        }
        if (tag_name && equals_ci(*tag_name, name))
            return content.value_or(std::string());
    }
    return std::nullopt;
}

// The title a test page must end with under --check-title, from
//   <meta name="karouka-check-title" content="^engine channel ok$">
// in the page's file (meta_content: any attribute order or quoting): an ECMAScript regex searched
// for in the final document.title. Empty when the page declares none or isn't a readable file.
std::optional<std::string> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string title_expectation(std::string const& page)
{
    auto html = read_file(page);
    if (!html)
        return {};
    return meta_content(*html, "karouka-check-title").value_or(std::string());
}

// --print-meta NAME page.html: the page's meta NAME content on stdout and 0, nothing and 1 when the
// page declares none, 2 when the page can't be read. No Metal device, no karouka system.
int print_meta(std::string const& name, std::string const& page)
{
    auto html = read_file(page);
    if (!html) {
        fprintf(stderr, "karouka_viewer: cannot read %s\n", page.c_str());
        return 2;
    }
    auto content = meta_content(*html, name);
    if (!content)
        return 1;
    printf("%s\n", content->c_str());
    return 0;
}

// Where --capture writes view `index` (0-based): out.png for the first, out-2.png, out-3.png, ...
std::string capture_path(std::string const& path, size_t index)
{
    if (index == 0 || path.empty())
        return path;
    auto suffix = "-" + std::to_string(index + 1);
    auto dot = path.rfind('.');
    auto slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + suffix;
    return path.substr(0, dot) + suffix + path.substr(dot);
}

} // namespace

// Set when --output-texture finds the texture and the reference's IOSurface differ, or when --check-title finds
// a view's final title doesn't match its page's expectation; the run then fails.
static bool s_output_check_failed = false;
static bool s_title_check_failed = false;
// Set when --max-layouts-per-frame finds a frame that ran more layout update passes, or none ran.
static bool s_layout_check_failed = false;
// Set when --check-full-raster finds the whole raster and the partial rasters' pixels differ.
static bool s_full_raster_check_failed = false;

// Drives one view: input, time, render, and copying the IOSurface into whatever presents it. The
// driver below steps every host through each phase in turn, so all views see one host frame.
@interface KRHost : NSObject
@property(nonatomic) kr_view* view;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic) BOOL scripted; // receives --scroll/--poke/--move/--key/--blur/--focus
- (std::vector<PageEntry> const&)pages;
- (void)watchDependenciesAtFrame:(uint32_t)frame;
- (void)setFocused:(BOOL)focused;
@end

@implementation KRHost {
    Options _options;
    std::string _page;
    FrameStats _stats;
    id<MTLTexture> _source;
    void* _source_surface;
    id<MTLTexture> _output; // what karouka draws into on the viewer's queue (kr_view_set_output2)
    kr_view* _reference; // --output-texture: the same page, rendered into its own IOSurface
    HudGame _reference_hud;
    std::vector<std::string> _history;
    std::vector<PageEntry> _pages;
    CAMetalLayer* _layer;
    HudGame _hud;
    bool _drives_hud;
    DependencyWatch _watch;
    bool _has_frame; // a frame was drawn into _output (in a window)
}

- (instancetype)initWithOptions:(Options const&)options page:(std::string const&)page view:(kr_view*)view device:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue
{
    self = [super init];
    _options = options;
    _page = page;
    _view = view;
    _device = device;
    _queue = queue;
    _history.push_back(page);
    _drives_hud = HudGame::drives(page);
    _pages = page_list();
    kr_view_stats view_stats = query_view_stats(_view);
    _stats.layouts_at_frame_end = view_stats.layout_updates;
    _stats.layout_check_from = options.layout_check_from;
    if (getenv("KR_DEBUG")) {
        for (size_t i = 0; i < _pages.size(); ++i)
            fprintf(stderr, "karouka_viewer: page %zu %s%s\n", i, _pages[i].label.c_str(), _pages[i].is_demo ? " (demo)" : "");
    }
    return self;
}

- (void)attachLayer:(CAMetalLayer*)layer
{
    _layer = layer;
}

- (void)setReference:(kr_view*)reference
{
    _reference = reference;
}

- (id<MTLTexture>)sourceTexture
{
    void* surface = kr_view_iosurface(_view);
    if (!surface)
        return nil;
    if (surface != _source_surface) {
        IOSurfaceRef io = (IOSurfaceRef)surface;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                        width:IOSurfaceGetWidth(io)
                                                                       height:IOSurfaceGetHeight(io)
                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        _source = [_device newTextureWithDescriptor:desc iosurface:io plane:0];
        _source_surface = surface;
    }
    return _source;
}

- (void)dispatchScriptedInputForFrame:(uint32_t)frame
{
    if (!_view || !_scripted)
        return;
    // The window's key state first, through the same setFocused: the window delegate calls.
    for (auto [at, focused] : _options.focus_changes) {
        if (at == frame)
            [self setFocused:focused];
    }
    for (auto const& input : _options.inputs) {
        if (input.frame != frame)
            continue;
        kr_input_event event = input.event;
        float scale = _options.scale > 0 ? _options.scale : 1;
        if (event.type == KR_INPUT_WHEEL) {
            event.x = _options.width * scale / 2;
            event.y = _options.height * scale / 2;
            event.delta_x *= scale;
            event.delta_y *= scale;
        } else {
            event.x *= scale;
            event.y *= scale;
        }
        kr_view_inject_input2(_view, &event);
        if (_reference)
            kr_view_inject_input2(_reference, &event);
    }
}

// The window's key state: an unfocused page paints its selection inactive and draws no caret.
- (void)setFocused:(BOOL)focused
{
    if (_view)
        kr_view_set_focused(_view, focused);
    if (_reference)
        kr_view_set_focused(_reference, focused);
}

- (void)advance:(double)time
{
    if (!_view)
        return;
    if (_drives_hud)
        _hud.step(_view);
    kr_view_advance_time(_view, time);
    if (_reference) {
        if (_drives_hud)
            _reference_hud.step(_reference);
        kr_view_advance_time(_reference, time);
    }
}

// Live reload: notes the files the page has loaded since the last frame and, on a polling frame,
// reloads the page if one of them changed on disk.
- (void)watchDependenciesAtFrame:(uint32_t)frame
{
    if (!_view)
        return;
    auto changed = _watch.poll(_view, frame % reload_poll_frames == 0);
    if (changed.empty())
        return;
    fprintf(stderr, "karouka_viewer: %s changed, reloading %s\n", changed.c_str(), _history.back().c_str());
    [self reloadPage];
}

- (void)reloadPage
{
    if (!_view)
        return;
    auto result = kr_view_reload(_view);
    if (result != KR_OK) {
        fprintf(stderr, "karouka_viewer: cannot reload %s (error %d)\n", _history.back().c_str(), (int)result);
        return;
    }
    if (_reference)
        kr_view_reload(_reference);
    [self pageChangedTo:_history.back()];
}

// Called whenever the view's document changes (a reload too): a new HUD page starts its game from
// frame 0, as Tessera's scripted copy restarts its counter on every load, and live reload watches
// the new document's files.
- (void)pageChangedTo:(std::string const&)path
{
    _watch.reset();
    _drives_hud = HudGame::drives(path);
    _hud.restart();
    _reference_hud.restart();
}

// --engine-gpu: karouka's own queue, so the viewer samples the IOSurface (complete, since
// kr_view_render waited for the GPU) through its own texture and copies it into the drawable.
- (void)presentFromSurface
{
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    id<MTLTexture> source = [self sourceTexture];
    if (!drawable || !source)
        return;
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    NSUInteger width = std::min(source.width, drawable.texture.width);
    NSUInteger height = std::min(source.height, drawable.texture.height);
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:drawable.texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
}

// The texture karouka draws the view into, at the given size: kept while the size stays, so karouka
// draws only frames that changed.
- (id<MTLTexture>)outputTextureWithWidth:(NSUInteger)width height:(NSUInteger)height
{
    if (!_output || _output.width != width || _output.height != height) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        _output = [_device newTextureWithDescriptor:desc];
    }
    return _output;
}

- (void)renderAndPresent
{
    if (!_view)
        return;
    bool rendered = false;
    if (_options.engine_gpu) {
        kr_view_render(_view, &rendered);
        if (_layer)
            [self presentFromSurface];
    } else if (_layer) {
        // On the viewer's queue: karouka draws the frame into the viewer's texture (only when it
        // changed), and the viewer's command buffer, which Metal runs after karouka's drawing, copies
        // it into the drawable. Nothing waits for the GPU.
        id<MTLCommandBuffer> commands = [_queue commandBuffer];
        CGSize size = _layer.drawableSize;
        id<MTLTexture> texture = [self outputTextureWithWidth:(NSUInteger)size.width height:(NSUInteger)size.height];
        kr_metal_output output { sizeof(kr_metal_output), (__bridge void*)commands, (__bridge void*)texture };
        bool drawn = texture && kr_view_set_output2(_view, &output) == KR_OK;
        kr_view_render(_view, &rendered);
        _has_frame = _has_frame || rendered;
        id<CAMetalDrawable> drawable = drawn && _has_frame ? [_layer nextDrawable] : nil;
        if (drawable) {
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            NSUInteger width = std::min(texture.width, drawable.texture.width);
            NSUInteger height = std::min(texture.height, drawable.texture.height);
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                        toTexture:drawable.texture destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [commands presentDrawable:drawable];
        }
        [commands commit];
    } else {
        // Headless on the viewer's queue: kr_view_render returns once the frame is submitted, so the
        // viewer waits for it here (a command buffer committed after it completes after it), and the
        // frame is complete and timed as tessera_viewer --headless does: raster + gpu.
        id<MTLCommandBuffer> commands = [_queue commandBuffer];
        if (_options.output_texture) {
            id<MTLTexture> texture = [self outputTextureWithWidth:(NSUInteger)(_options.width * _options.scale)
                                                           height:(NSUInteger)(_options.height * _options.scale)];
            kr_metal_output output { sizeof(kr_metal_output), (__bridge void*)commands, (__bridge void*)texture };
            if (kr_view_set_output2(_view, &output) != KR_OK)
                fprintf(stderr, "karouka_viewer: kr_view_set_output2 failed\n");
        }
        kr_view_render(_view, &rendered);
        // Submitted ahead of the viewer's command buffer, which the wait below then covers.
        if (_reference)
            kr_view_render(_reference, nullptr);
        if (rendered || _options.output_texture) {
            double gpu_start = now_ms();
            [commands commit];
            [commands waitUntilCompleted];
            if (rendered)
                _stats.gpu.push_back(now_ms() - gpu_start);
        }
    }

    kr_view_stats view_stats = query_view_stats(_view);
    _stats.update.push_back(view_stats.last_update_ms);
    if (rendered) {
        ++_stats.rendered;
        _stats.record.push_back(view_stats.last_record_ms);
        _stats.raster.push_back(view_stats.last_raster_ms);
        _stats.raster_pixels.push_back((double)view_stats.last_raster_pixels);
    }
}

- (void)recordFrameTime:(double)frame_ms cpu:(double)cpu_ms mainCpu:(double)main_cpu_ms endMs:(double)end_ms endCpu:(double)end_cpu
{
    if (!_view)
        return;
    // Before the frame is recorded, so that max_layouts_frame is this frame's index.
    kr_view_stats view_stats = query_view_stats(_view);
    _stats.record_layouts(view_stats.layout_updates);
    _stats.record_frame(frame_ms, cpu_ms, main_cpu_ms, end_ms, end_cpu);
    _stats.peak_rss = std::max(_stats.peak_rss, resident_bytes());
}

- (std::string)title
{
    return _view ? std::string(kr_view_title(_view)) : std::string();
}

- (void)finishWithCapture:(std::string const&)path
{
    if (!_view)
        return;
    if (getenv("KR_DEBUG"))
        fprintf(stderr, "karouka_viewer: final page=%s title='%s'\n", _page.c_str(), kr_view_title(_view));
    [self captureTo:path];
    if (_options.check_title)
        [self checkTitle];
    if (_options.max_layouts_per_frame >= 0)
        [self checkLayouts];
    if (_options.output_texture)
        [self checkOutputTexture];
    if (_options.stats) {
        kr_view_stats view_stats = query_view_stats(_view);
        _stats.report(_page.c_str(), view_stats.updates_run);
        _stats.report_frame_loop(_page.c_str());
    }
    // Last: it renders once more, into another target.
    if (_options.check_full_raster && !_options.capture_again_frames)
        [self checkFullRaster];
}

- (void)captureTo:(std::string const&)path
{
    if (_view && !path.empty() && kr_view_capture_png(_view, path.c_str()) != KR_OK)
        fprintf(stderr, "karouka_viewer: capture to %s failed\n", path.c_str());
}

// --capture-again: the end of the run past --frames.
- (void)finishAgainWithCapture:(std::string const&)path
{
    [self captureTo:path];
    if (_options.check_full_raster)
        [self checkFullRaster];
}

// --check-full-raster: copies what the view's partial rasters left in its IOSurface, then has
// karouka draw the same frame into a fresh texture of the viewer's, which it rasterizes whole (a new
// target; the view's IOSurface is released for it), and compares the two exactly. Damage that missed
// a change leaves stale pixels in the IOSurface that the whole raster doesn't have.
- (void)checkFullRaster
{
    auto surface = _view ? (IOSurfaceRef)kr_view_iosurface(_view) : nullptr;
    if (!surface) {
        fprintf(stderr, "karouka_viewer: full raster check: the view has no IOSurface frame\n");
        s_full_raster_check_failed = true;
        return;
    }
    NSUInteger width = IOSurfaceGetWidth(surface);
    NSUInteger height = IOSurfaceGetHeight(surface);
    std::vector<uint32_t> partial(width * height);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    auto const* surface_bytes = static_cast<uint8_t const*>(IOSurfaceGetBaseAddress(surface));
    auto stride = IOSurfaceGetBytesPerRow(surface);
    for (NSUInteger y = 0; y < height; ++y)
        memcpy(&partial[y * width], surface_bytes + y * stride, width * 4);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    surface = nullptr; // released by the render below

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [_device newTextureWithDescriptor:desc];
    id<MTLBuffer> buffer = [_device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    kr_metal_output output { sizeof(kr_metal_output), (__bridge void*)commands, (__bridge void*)texture };
    bool rendered = false;
    if (!texture || !buffer || kr_view_set_output2(_view, &output) != KR_OK || kr_view_render(_view, &rendered) != KR_OK || !rendered) {
        [commands commit];
        fprintf(stderr, "karouka_viewer: full raster check: karouka drew no frame into the texture\n");
        s_full_raster_check_failed = true;
        return;
    }
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:buffer destinationOffset:0 destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];

    uint64_t differing = 0;
    auto const* whole = static_cast<uint32_t const*>(buffer.contents);
    for (size_t i = 0; i < partial.size(); ++i)
        differing += partial[i] != whole[i];
    fprintf(stderr, "karouka_viewer: full raster check page=%s %lux%lu: the whole raster %s the partial rasters' IOSurface (%llu pixels differ)\n",
        _page.c_str(), (unsigned long)width, (unsigned long)height, differing ? "DIFFERS FROM" : "matches", (unsigned long long)differing);
    if (differing)
        s_full_raster_check_failed = true;
}

// --check-title: the final title against the current page's karouka-check-title (title_expectation).
- (void)checkTitle
{
    auto const& page = _history.back();
    auto expected = title_expectation(page);
    if (expected.empty())
        return;
    std::string title = kr_view_title(_view);
    // Logged first: without exceptions, a bad regex aborts the run in std::regex's constructor.
    fprintf(stderr, "karouka_viewer: title check page=%s title='%s' expected /%s/: ", page.c_str(), title.c_str(), expected.c_str());
    fflush(stderr);
    bool matches = std::regex_search(title, std::regex(expected, std::regex::ECMAScript));
    fprintf(stderr, "%s\n", matches ? "ok" : "FAILED");
    if (!matches)
        s_title_check_failed = true;
}

// --max-layouts-per-frame N@F: no frame from F on may have run more layout update passes than N, and
// some frame must have run one: every page lays out as it loads, in frames 0-2, so a run that counts
// none has a counter that stopped counting. The frames from F on may run none.
- (void)checkLayouts
{
    auto limit = (uint64_t)_options.max_layouts_per_frame;
    auto const& checked = _stats.checked_layouts;
    bool ok = _stats.layouts.total > 0 && checked.most <= limit;
    fprintf(stderr, "karouka_viewer: layout check page=%s frames %u-%zu ran %llu layout updates, most %llu in frame %zu, limit %llu: %s\n",
        _page.c_str(), _options.layout_check_from, _stats.frame.size() ? _stats.frame.size() - 1 : 0, (unsigned long long)checked.total,
        (unsigned long long)checked.most, checked.most_at, (unsigned long long)limit,
        ok ? "ok" : _stats.layouts.total ? "FAILED" : "FAILED (no layout update counted)");
    if (!ok)
        s_layout_check_failed = true;
}

// --output-texture: the private texture karouka drew the view into (kr_view_set_output2) must hold
// exactly the pixels the reference view rendered into its IOSurface, and the view must have no
// IOSurface of its own. Reads both back after the GPU is done with them.
- (void)checkOutputTexture
{
    if (kr_view_iosurface(_view)) {
        fprintf(stderr, "karouka_viewer: output texture: the view has an IOSurface besides the texture\n");
        s_output_check_failed = true;
    }
    auto surface = _reference ? (IOSurfaceRef)kr_view_iosurface(_reference) : nullptr;
    if (!surface || !_output) {
        fprintf(stderr, "karouka_viewer: output texture: no frame rendered\n");
        s_output_check_failed = true;
        return;
    }
    NSUInteger width = std::min<NSUInteger>(_output.width, IOSurfaceGetWidth(surface));
    NSUInteger height = std::min<NSUInteger>(_output.height, IOSurfaceGetHeight(surface));
    id<MTLBuffer> buffer = [_device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:_output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:buffer destinationOffset:0 destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];

    uint64_t differing = 0;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    auto const* surface_bytes = static_cast<uint8_t const*>(IOSurfaceGetBaseAddress(surface));
    auto stride = IOSurfaceGetBytesPerRow(surface);
    auto const* texture_bytes = static_cast<uint8_t const*>(buffer.contents);
    for (NSUInteger y = 0; y < height; ++y) {
        auto const* a = reinterpret_cast<uint32_t const*>(surface_bytes + y * stride);
        auto const* b = reinterpret_cast<uint32_t const*>(texture_bytes + y * width * 4);
        for (NSUInteger x = 0; x < width; ++x)
            differing += a[x] != b[x];
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    fprintf(stderr, "karouka_viewer: output texture %lux%lu %s the reference view's IOSurface (%llu pixels differ)\n", (unsigned long)width, (unsigned long)height,
        differing ? "DIFFERS FROM" : "matches", (unsigned long long)differing);
    if (differing)
        s_output_check_failed = true;
}

- (void)close
{
    _source = nil;
    _source_surface = nullptr;
    kr_view_destroy(_reference);
    _reference = nullptr;
    kr_view_destroy(_view);
    _view = nullptr;
}

- (void)loadPage:(std::string const&)path pushHistory:(BOOL)push
{
    if (!_view)
        return;
    if (kr_view_load_file(_view, path.c_str()) != KR_OK) {
        fprintf(stderr, "karouka_viewer: cannot load %s\n", path.c_str());
        return;
    }
    if (_reference)
        kr_view_load_file(_reference, path.c_str());
    if (push)
        _history.push_back(path);
    [self pageChangedTo:path];
}

- (void)goBack
{
    if (_history.size() < 2)
        return;
    _history.pop_back();
    [self loadPage:_history.back() pushHistory:NO];
}

- (void)didNavigateTo:(char const*)path
{
    _history.push_back(path);
    [self pageChangedTo:path];
}

- (std::vector<PageEntry> const&)pages
{
    return _pages;
}

- (void)openPageAtIndex:(size_t)index
{
    if (index >= _pages.size() || same_file(_pages[index].path, _history.back()))
        return;
    [self loadPage:_pages[index].path pushHistory:YES];
}

// Steps through the page list from the current page, wrapping at either end. A page that isn't in
// the list (one reached by a link elsewhere) steps from the start page.
- (void)stepPage:(int)delta
{
    if (_pages.empty())
        return;
    size_t current = 0;
    for (size_t i = 0; i < _pages.size(); ++i) {
        if (same_file(_pages[i].path, _history.back())) {
            current = i;
            break;
        }
    }
    auto count = (long)_pages.size();
    [self openPageAtIndex:(size_t)((((long)current + delta) % count + count) % count)];
}

- (void)openPageFromMenu:(NSMenuItem*)item { [self openPageAtIndex:(size_t)item.tag]; }
- (void)nextPage:(id)sender { [self stepPage:1]; }
- (void)previousPage:(id)sender { [self stepPage:-1]; }
- (void)back:(id)sender { [self goBack]; }
- (void)reload:(id)sender { [self reloadPage]; }

@end

static bool on_navigate(kr_view*, char const* path, void* user_data)
{
    [(__bridge KRHost*)user_data didNavigateTo:path];
    return true;
}

// A stand-in game for the `engine` channel (pages/tests/engine_channel.html): logs page triggers
// under KR_DEBUG, answers a trigger named "echo" with a "echoed" event carrying the same arguments,
// resolves engine.call("echo", ...) with its argument array and rejects any other call.
static void on_engine_trigger(void*, kr_view* view, char const* name, char const* json_args)
{
    if (getenv("KR_DEBUG"))
        fprintf(stderr, "karouka_viewer: engine.trigger %s %s\n", name, json_args);
    if (strcmp(name, "echo") == 0)
        (void)kr_view_engine_trigger(view, "echoed", json_args);
}

static void on_engine_call(void*, kr_view* view, char const* name, char const* json_args, uint64_t call_id)
{
    if (getenv("KR_DEBUG"))
        fprintf(stderr, "karouka_viewer: engine.call %s %s #%llu\n", name, json_args, (unsigned long long)call_id);
    if (strcmp(name, "echo") == 0)
        (void)kr_view_engine_resolve(view, call_id, true, json_args);
    else
        (void)kr_view_engine_resolve(view, call_id, false, "\"unknown call\"");
}

// Steps every view through one host frame: input for all, then time for all with the same frame
// time (so the system runs one rendering update for them), then render and present each.
@interface KRDriver : NSObject
@property(nonatomic, strong) NSArray<KRHost*>* hosts;
@property(nonatomic, strong) NSWindow* window;
- (BOOL)tick;
- (void)finish;
@end

@implementation KRDriver {
    Options _options;
    uint32_t _frame;
    bool _finished; // --capture-again: the run's end at --frames is behind it
}

- (instancetype)initWithOptions:(Options const&)options hosts:(NSArray<KRHost*>*)hosts
{
    self = [super init];
    _options = options;
    _hosts = hosts;
    return self;
}

// One frame. Returns whether the run should continue.
- (BOOL)tick
{
    if (_options.close_second_at && _frame == _options.close_second_at && _hosts.count > 1 && _hosts[1].view) {
        if (getenv("KR_DEBUG"))
            fprintf(stderr, "karouka_viewer: closing the second view at frame %u\n", _frame);
        [_hosts[1] close];
    }

    double frame_start = now_ms();
    // Process CPU only under --stats: getrusage walks every thread of the process.
    double frame_start_cpu = _options.stats ? cpu_seconds() : 0;
    uint64_t frame_start_main_cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    // --host-io async: the reads requested since the last frame complete now; the pages act on
    // them in this frame's advance.
    HostCallbacks::complete_queued_reads();
    // A scripted run (--frames) steps the host clock by a fixed 16.67 ms per frame, as
    // tessera_viewer does, so its frames and captures depend on the frame count only and not on
    // what the frames cost. An interactive run passes the real clock.
    double host_time = _options.frames ? _frame * (1000.0 / 60.0) : frame_start;
    // --testharness: the trickled reads due by this frame's time complete now, and the reads the
    // pages request during its advance are timed from it.
    HostCallbacks::complete_delayed_reads(host_time);
    HostCallbacks::s_page_time_ms = host_time;
    for (KRHost* host in _hosts)
        [host dispatchScriptedInputForFrame:_frame];
    for (KRHost* host in _hosts)
        [host advance:host_time];
    char phase[32];
    if (_frame < 3) {
        snprintf(phase, sizeof(phase), "advance-%u", _frame);
        mem_phase(phase);
    }
    if (!_options.no_render) {
        for (KRHost* host in _hosts)
            [host renderAndPresent];
        if (_frame < 3) {
            snprintf(phase, sizeof(phase), "render-%u", _frame);
            mem_phase(phase);
        }
    }
    // KR_MEM_PHASE_EVERY=N: a "frame-F" phase after every Nth frame too, so one long run shows when
    // the heap grows.
    static unsigned const phase_every = [] {
        auto every = getenv("KR_MEM_PHASE_EVERY");
        return every ? static_cast<unsigned>(strtoul(every, nullptr, 10)) : 0u;
    }();
    if (phase_every && _frame && (_frame + 1) % phase_every == 0) {
        snprintf(phase, sizeof(phase), "frame-%u", _frame + 1);
        mem_phase(phase);
        if (s_mem_phases && _hosts.count)
            fprintf(stderr, "karouka-mem-phase %-14s title='%s'\n", phase, [_hosts.firstObject title].c_str());
    }
    double frame_end = now_ms();
    double frame_end_cpu = _options.stats ? cpu_seconds() : 0;
    double frame_main_cpu_ms = (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - frame_start_main_cpu) / 1e6;
    double frame_ms = frame_end - frame_start;
    for (KRHost* host in _hosts)
        [host recordFrameTime:frame_ms cpu:(frame_end_cpu - frame_start_cpu) * 1000.0 mainCpu:frame_main_cpu_ms endMs:frame_end endCpu:frame_end_cpu];
    // After the frame is timed: the stat calls are the viewer's, not the page's.
    if (_options.live_reload.value_or(!_options.headless)) {
        for (KRHost* host in _hosts)
            [host watchDependenciesAtFrame:_frame];
    }

    if (_window) {
        std::string title;
        for (KRHost* host in _hosts) {
            if (!host.view)
                continue;
            auto host_title = [host title];
            if (!title.empty())
                title += "  |  ";
            title += host_title.empty() ? "karouka" : host_title;
        }
        _window.title = [NSString stringWithUTF8String:title.c_str()];
    }

    ++_frame;
    // --testharness: done once the page reported (the report arrives during an advance).
    if (_options.frames && !_finished && (_frame >= _options.frames || HostCallbacks::s_testharness_report)) {
        [self finish];
        _finished = true;
        if (!_options.capture_again_frames)
            return NO;
    }
    // --capture-again: on to its frame, then the first view again.
    if (_finished && _frame >= _options.capture_again_frames) {
        if (_hosts.count)
            [_hosts.firstObject finishAgainWithCapture:_options.capture_again];
        return NO;
    }
    return YES;
}

- (void)finish
{
    for (NSUInteger i = 0; i < _hosts.count; ++i)
        [_hosts[i] finishWithCapture:capture_path(_options.capture, i)];
}

// Views go before the system that made them; the last view created goes first.
- (void)closeAll
{
    for (KRHost* host in _hosts.reverseObjectEnumerator)
        [host close];
}

@end

// One view's pane: a CAMetalLayer, and the source of input events. The Pages menu sends its
// actions to the first responder, so they act on the pane clicked last.
@interface KRView : NSView
@property(nonatomic, weak) KRHost* host;
@property(nonatomic) float scale;
@end

@implementation KRView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }

- (void)updateTrackingAreas
{
    for (NSTrackingArea* area in self.trackingAreas)
        [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
    [super updateTrackingAreas];
}

- (void)sendMouse:(NSEvent*)event type:(kr_input_type)type button:(uint32_t)button
{
    if (!_host.view)
        return;
    kr_input_event input = Karouka::EventInput::mouse(self, event, type, button, _scale);
    kr_view_inject_input2(_host.view, &input);
}

- (void)mouseMoved:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_MOVE button:0]; }
- (void)mouseDragged:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_MOVE button:0]; }
- (void)mouseDown:(NSEvent*)event
{
    [self.window makeFirstResponder:self];
    [self sendMouse:event type:KR_INPUT_MOUSE_DOWN button:0];
}
- (void)mouseUp:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_UP button:0]; }
- (void)rightMouseDown:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_DOWN button:1]; }
- (void)rightMouseUp:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_UP button:1]; }

- (void)scrollWheel:(NSEvent*)event
{
    if (!_host.view)
        return;
    kr_input_event input = Karouka::EventInput::wheel(self, event, _scale);
    kr_view_inject_input2(_host.view, &input);
}

- (void)sendKey:(NSEvent*)event type:(kr_input_type)type
{
    if (!_host.view)
        return;
    kr_input_event input = Karouka::EventInput::key(event, type);
    kr_view_inject_input2(_host.view, &input);
}

- (void)keyDown:(NSEvent*)event
{
    // Esc goes back; the other shortcuts are key equivalents in the Pages menu.
    if (event.keyCode == 0x35) {
        [_host goBack];
        return;
    }
    [self sendKey:event type:KR_INPUT_KEY_DOWN];
}

- (void)keyUp:(NSEvent*)event { [self sendKey:event type:KR_INPUT_KEY_UP]; }

- (void)openPageFromMenu:(NSMenuItem*)item { [_host openPageFromMenu:item]; }
- (void)nextPage:(id)sender { [_host nextPage:sender]; }
- (void)previousPage:(id)sender { [_host previousPage:sender]; }
- (void)back:(id)sender { [_host back:sender]; }
- (void)reload:(id)sender { [_host reload:sender]; }

@end

@interface KRAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) KRDriver* driver;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSArray<KRView*>* panes;
@property(nonatomic, strong) CADisplayLink* displayLink;
@end

@implementation KRAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    [NSApp activateIgnoringOtherApps:YES];
    [_window makeKeyAndOrderFront:nil];
    fprintf(stderr, "karouka_viewer: window open. Pages menu: Cmd+0 start page, Cmd+1..9 demo pages, "
                    "Cmd+Shift+] / Cmd+Shift+[ next / previous page, Esc or Cmd+[ back, Cmd+R reload, Cmd+Q quit.\n");
    // Frames follow the window's display: a tick per vsync of the screen it is on, at 60 a second
    // (a ProMotion screen too), where a 60 Hz timer drifted against vsync and fell to the next timer
    // slot whenever a tick ran long. Common modes keep it ticking through resizes and menus.
    _displayLink = [_window.contentView displayLinkWithTarget:self selector:@selector(step:)];
    _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)step:(CADisplayLink*)link
{
    if (![_driver tick]) {
        [link invalidate];
        [NSApp terminate:nil];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    for (KRView* pane in _panes)
        [pane.host setFocused:YES];
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    for (KRView* pane in _panes)
        [pane.host setFocused:NO];
}

// The panes split the window's width evenly.
- (void)windowDidResize:(NSNotification*)notification
{
    NSSize size = _window.contentView.bounds.size;
    CGFloat pane_width = floor(size.width / _panes.count);
    for (NSUInteger i = 0; i < _panes.count; ++i) {
        KRView* pane = _panes[i];
        pane.frame = NSMakeRect(pane_width * i, 0, pane_width, size.height);
        float scale = pane.scale;
        CAMetalLayer* layer = (CAMetalLayer*)pane.layer;
        layer.drawableSize = CGSizeMake(pane_width * scale, size.height * scale);
        if (pane.host.view)
            kr_view_resize(pane.host.view, (uint32_t)(pane_width * scale), (uint32_t)(size.height * scale), scale);
    }
}

@end

static NSMenuItem* menu_item(NSString* title, SEL action, NSString* key, NSEventModifierFlags modifiers, id target)
{
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    item.target = target;
    return item;
}

static NSMenuItem* submenu_item(NSString* title, NSMenu* submenu)
{
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = submenu;
    return item;
}

// The page items have no target: they go to the first responder, the pane clicked last.
static NSMenu* build_menu(KRHost* host)
{
    NSMenu* bar = [NSMenu new];

    NSMenu* app = [[NSMenu alloc] initWithTitle:@"karouka_viewer"];
    [app addItem:menu_item(@"Quit karouka_viewer", @selector(terminate:), @"q", NSEventModifierFlagCommand, NSApp)];
    [bar addItem:submenu_item(@"", app)];

    NSMenu* pages = [[NSMenu alloc] initWithTitle:@"Pages"];
    pages.autoenablesItems = NO;
    [pages addItem:menu_item(@"Back", @selector(back:), @"[", NSEventModifierFlagCommand, nil)];
    [pages addItem:menu_item(@"Reload", @selector(reload:), @"r", NSEventModifierFlagCommand, nil)];
    [pages addItem:menu_item(@"Next Page", @selector(nextPage:), @"}", NSEventModifierFlagCommand, nil)];
    [pages addItem:menu_item(@"Previous Page", @selector(previousPage:), @"{", NSEventModifierFlagCommand, nil)];
    [pages addItem:[NSMenuItem separatorItem]];

    NSMenu* tests = [[NSMenu alloc] initWithTitle:@"Test Pages"];
    tests.autoenablesItems = NO;
    int digit = 1;
    auto const& list = [host pages];
    for (size_t i = 0; i < list.size(); ++i) {
        NSString* label = [NSString stringWithUTF8String:list[i].label.c_str()];
        if (i == 0) {
            NSMenuItem* item = menu_item(label, @selector(openPageFromMenu:), @"0", NSEventModifierFlagCommand, nil);
            item.tag = (NSInteger)i;
            [pages addItem:item];
        } else if (list[i].is_demo) {
            NSString* key = digit <= 9 ? [NSString stringWithFormat:@"%d", digit++] : @"";
            NSMenuItem* item = menu_item(label, @selector(openPageFromMenu:), key, NSEventModifierFlagCommand, nil);
            item.tag = (NSInteger)i;
            [pages addItem:item];
        } else {
            NSMenuItem* item = menu_item(label, @selector(openPageFromMenu:), @"", 0, nil);
            item.tag = (NSInteger)i;
            [tests addItem:item];
        }
    }
    if (tests.numberOfItems > 0) {
        [pages addItem:[NSMenuItem separatorItem]];
        [pages addItem:submenu_item(@"Test Pages", tests)];
    }
    [bar addItem:submenu_item(@"Pages", pages)];
    return bar;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        Options options;
        if (!parse_options(argc, argv, options)) {
            usage();
            return 2;
        }
        if (options.print_meta) {
            if (options.page.empty()) {
                fprintf(stderr, "--print-meta needs a page\n");
                return 2;
            }
            return print_meta(*options.print_meta, options.page);
        }
        if (options.page.empty())
            options.page = default_page();
        bool scripted = options.frames > 0 || !options.capture.empty();
        if (options.scale <= 0)
            options.scale = (scripted || options.headless) ? 1.0f : (float)NSScreen.mainScreen.backingScaleFactor;

        s_mem_phases = options.mem_phases;
        s_mem_phases_heap = options.mem_phases_heap;
        mem_phase("start");
        // The viewer's GPU, which it hands to karouka as a game would (kr_system_desc.mtl_device),
        // unless --engine-gpu. None for a headless run on the software backend: nothing draws on it.
        id<MTLDevice> device = options.software && options.headless ? nil : pick_metal_device();
        if (options.verbose && device)
            fprintf(stderr, "karouka_viewer: GPU %s (registry id %llu)\n", device.name.UTF8String, (unsigned long long)device.registryID);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        mem_phase("host-device");

        kr_system* system = nullptr;
        kr_system_desc system_desc {};
        system_desc.struct_size = sizeof(system_desc);
        system_desc.verbose = options.verbose;
        system_desc.real_time_clock = options.real_time;
        std::string shader_cache_path = options.no_shader_cache ? std::string() : options.shader_cache_path.empty() ? default_shader_cache_path(options.page) : options.shader_cache_path;
        system_desc.shader_cache_path = shader_cache_path.empty() ? nullptr : shader_cache_path.c_str();
        system_desc.backend = options.software ? KR_BACKEND_SOFTWARE : KR_BACKEND_METAL;
        if (!options.engine_gpu) {
            system_desc.mtl_device = (__bridge void*)device;
            system_desc.mtl_queue = (__bridge void*)queue;
        }
        kr_host_interface host_interface {};
        host_interface.struct_size = sizeof(host_interface);
        if (!options.doc_root.empty()) {
            HostCallbacks::s_doc_root = options.doc_root;
            if (options.host_io == Options::HostIo::None)
                options.host_io = Options::HostIo::Sync;
        }
        if (options.testharness) {
            HostCallbacks::s_testharness = true;
            if (options.host_io == Options::HostIo::None)
                options.host_io = Options::HostIo::Sync;
            if (options.host_log_level < 0 || options.host_log_level > KR_LOG_INFO)
                options.host_log_level = KR_LOG_INFO;
        }
        if (options.host_io != Options::HostIo::None)
            host_interface.io = { HostCallbacks::request, HostCallbacks::cancel, HostCallbacks::release, reinterpret_cast<void*>(static_cast<uintptr_t>(options.host_io)) };
        if (options.host_log_level >= 0)
            host_interface.log = { HostCallbacks::write_log, static_cast<kr_log_level>(options.host_log_level), nullptr };
        if (options.host_clock)
            host_interface.clock = { HostCallbacks::monotonic_ns, nullptr };
        system_desc.host = &host_interface;
        if (!options.font_hinting.empty())
            setenv("KR_FONT_HINTING", options.font_hinting.c_str(), 1);
        bool startup_stats = getenv("KR_DEBUG");
        rusage_info_v4 usage_before {};
        uint64_t main_cpu_before = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        auto wall_before = std::chrono::steady_clock::now();
        if (startup_stats)
            proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage_before));
        if (kr_system_create2(&system_desc, &system) != KR_OK) {
            fprintf(stderr, "karouka_viewer: engine failed to start (no Metal device?)\n");
            return 1;
        }
        // kr_system_create2's cost in terms the machine's load moves less than its wall time, for the
        // startup gate: instructions retired by every thread
        // (the work done, whatever the core and its clock) and the main thread's off-CPU time, its
        // wall time less its own CPU time (sleeps, waits on other threads and on a CPU). Runnable
        // and CPU are the process's: all its threads, summed.
        if (startup_stats) {
            rusage_info_v4 usage_after {};
            if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage_after)) == 0) {
                mach_timebase_info_data_t timebase {};
                mach_timebase_info(&timebase);
                auto ms = [&](uint64_t ticks) { return ticks * (double)timebase.numer / timebase.denom / 1e6; };
                double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_before).count();
                double main_cpu = (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - main_cpu_before) / 1e6;
                fprintf(stderr, "karouka_viewer: kr_system_create2 runnable %.2f ms, cpu %.2f ms, main cpu %.2f ms, off-cpu %.2f ms, instructions %.2f M, cycles %.2f M\n",
                    ms(usage_after.ri_runnable_time - usage_before.ri_runnable_time),
                    ms(usage_after.ri_user_time + usage_after.ri_system_time - usage_before.ri_user_time - usage_before.ri_system_time),
                    main_cpu, wall - main_cpu, (usage_after.ri_instructions - usage_before.ri_instructions) / 1e6, (usage_after.ri_cycles - usage_before.ri_cycles) / 1e6);
            }
        }
        s_mem_system = system;
        mem_phase("system");
        HostCallbacks::s_system = system;
        for (auto const& font : options.fonts) {
            kr_font_desc font_desc {};
            font_desc.struct_size = sizeof(font_desc);
            font_desc.family = font.family.empty() ? nullptr : font.family.c_str();
            font_desc.flags = font.fallback ? KR_FONT_FALLBACK : KR_FONT_FLAGS_NONE;
            if (auto result = kr_system_add_font_file2(system, font.path.c_str(), &font_desc); result != KR_OK)
                fprintf(stderr, "karouka_viewer: could not add the font %s (error %d)\n", font.path.c_str(), (int)result);
        }
        if (!options.default_font.empty()) {
            if (auto result = kr_system_set_default_font(system, options.default_font.c_str()); result != KR_OK)
                fprintf(stderr, "karouka_viewer: no registered family %s (error %d)\n", options.default_font.c_str(), (int)result);
        }
        std::vector<std::string> view_pages { options.page };
        view_pages.insert(view_pages.end(), options.extra_pages.begin(), options.extra_pages.end());
        if (options.input_view > view_pages.size()) {
            fprintf(stderr, "karouka_viewer: --input-view %u, but there are only %zu views\n", options.input_view, view_pages.size());
            return 1;
        }

        NSMutableArray<KRHost*>* hosts = [NSMutableArray array];
        for (auto const& page : view_pages) {
            kr_view_desc view_desc {};
            view_desc.struct_size = sizeof(view_desc);
            view_desc.width = (uint32_t)(options.width * options.scale);
            view_desc.height = (uint32_t)(options.height * options.scale);
            view_desc.scale = options.scale;
            kr_view* view = nullptr;
            if (kr_view_create2(system, &view_desc, &view) != KR_OK) {
                fprintf(stderr, "karouka_viewer: cannot create view\n");
                return 1;
            }
            mem_phase("view");
            kr_engine_callbacks engine_callbacks { sizeof(kr_engine_callbacks), on_engine_trigger, on_engine_call, nullptr };
            kr_view_set_engine_callbacks2(view, &engine_callbacks);
            if (kr_view_load_file(view, page.c_str()) != KR_OK) {
                fprintf(stderr, "karouka_viewer: cannot load %s\n", page.c_str());
                return 1;
            }
            mem_phase("load");
            KRHost* host = [[KRHost alloc] initWithOptions:options page:page view:view device:device queue:queue];
            host.scripted = hosts.count + 1 == options.input_view;
            kr_view_set_navigate_callback(view, on_navigate, (__bridge void*)host);
            if (options.output_texture) {
                // Navigates by itself (no callback), as the view does when the host lets it.
                kr_view* reference = nullptr;
                if (kr_view_create2(system, &view_desc, &reference) != KR_OK) {
                    fprintf(stderr, "karouka_viewer: cannot create the reference view\n");
                    return 1;
                }
                [host setReference:reference];
                kr_view_set_engine_callbacks2(reference, &engine_callbacks);
                if (kr_view_load_file(reference, page.c_str()) != KR_OK) {
                    fprintf(stderr, "karouka_viewer: cannot load %s\n", page.c_str());
                    return 1;
                }
            }
            [hosts addObject:host];
        }
        KRDriver* driver = [[KRDriver alloc] initWithOptions:options hosts:hosts];

        if (options.headless) {
            // Unpaced: each frame is advance + render + wait for the GPU, like tessera_viewer --headless.
            // --fps N: paced to a free-running 1/N s deadline, like tessera_viewer --headless --fps N.
            if (options.frames == 0)
                [driver finish];
            else if (!options.fps)
                while ([driver tick]) { }
            else {
                double period_ms = 1000.0 / options.fps;
                double deadline = now_ms();
                while ([driver tick]) {
                    deadline += period_ms;
                    double now = now_ms();
                    if (now >= deadline)
                        deadline = now; // overran: the next period starts now, no catching up
                    else
                        usleep((useconds_t)((deadline - now) * 1000.0));
                }
            }
            mem_phase("end");
            if (options.memory_stats) {
                kr_memory_stats memory {};
                memory.struct_size = sizeof(memory);
                kr_system_query_memory_stats(system, &memory);
                printf("karouka-memory footprint_mb=%.1f malloc_zone_mb=%.1f kmalloc_mb=%.1f gc_heap_mb=%.1f gpu_cache_mb=%.1f gpu_cache_budget_mb=%.1f surface_mb=%.1f\n",
                    memory.footprint_bytes / 1048576.0, memory.malloc_zone_bytes / 1048576.0, memory.kmalloc_bytes / 1048576.0, memory.gc_heap_bytes / 1048576.0,
                    memory.gpu_cache_bytes / 1048576.0, memory.gpu_cache_budget_bytes / 1048576.0, memory.surface_bytes / 1048576.0);
                // Per subsystem (kr_system_memory_usage), read by bench/compare.py (its _mb fields only),
                // with the glyph cache's strike count (kr_memory_stats) after font_cache_mb.
                printf("karouka-memory-usage");
                for (int tag = 0; tag < KR_MEM_TAG_COUNT; ++tag) {
                    printf(" %s_mb=%.2f", kr_mem_tag_name((kr_mem_tag)tag), kr_system_memory_usage(system, (kr_mem_tag)tag) / 1048576.0);
                    if (tag == KR_MEM_TAG_FONT_CACHE)
                        printf(" font_cache_strikes=%llu", (unsigned long long)memory.font_cache_strikes);
                }
                printf("\n");
            }
            if (options.hold)
                raise(SIGSTOP);
            if (options.host_io != Options::HostIo::None)
                fprintf(stderr, "karouka_viewer: host io served %llu reads, %llu cancelled, %zu queued\n", (unsigned long long)HostCallbacks::s_reads,
                    (unsigned long long)HostCallbacks::s_cancels, HostCallbacks::s_queued_reads.size());
            [driver closeAll];
            kr_system_destroy(system);
            s_mem_system = nullptr;
            mem_phase("destroyed");
            if (options.testharness) {
                if (!HostCallbacks::s_testharness_report) {
                    fprintf(stderr, "karouka_viewer: --testharness: no report in %u frames\n", options.frames);
                    return 3;
                }
                printf("%s\n", HostCallbacks::s_testharness_report->c_str());
                fflush(stdout);
            }
            // 4: --check-full-raster failed and nothing else did.
            return s_output_check_failed || s_title_check_failed || s_layout_check_failed ? 1 : s_full_raster_check_failed ? 4 : 0;
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, options.width * hosts.count, options.height);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [window center];
        NSView* container = [[NSView alloc] initWithFrame:frame];
        NSMutableArray<KRView*>* panes = [NSMutableArray array];
        for (NSUInteger i = 0; i < hosts.count; ++i) {
            KRView* pane = [[KRView alloc] initWithFrame:NSMakeRect(options.width * i, 0, options.width, options.height)];
            pane.wantsLayer = YES;
            pane.host = hosts[i];
            pane.scale = options.scale;
            CAMetalLayer* layer = (CAMetalLayer*)pane.layer;
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = NO;
            layer.contentsScale = options.scale;
            layer.drawableSize = CGSizeMake(options.width * options.scale, options.height * options.scale);
            [container addSubview:pane];
            [hosts[i] attachLayer:layer];
            [panes addObject:pane];
        }
        window.contentView = container;
        [window makeFirstResponder:panes[0]];
        driver.window = window;

        NSApp.mainMenu = build_menu(hosts[0]);

        KRAppDelegate* delegate = [KRAppDelegate new];
        delegate.driver = driver;
        delegate.window = window;
        delegate.panes = panes;
        window.delegate = delegate;
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
