/* What an idle 1 s kr_view_advance_time costs in main-thread event loop pumps
 * (kr_view_stats.event_loop_pumps). Now the host clock steps from one timer deadline
 * to the next and pumps at each, instead of moving through every whole millisecond; before it a page
 * with idle callbacks pumped at all 1000 steps of a 1 s advance. Each page here runs 30 frames, then
 * one advance of 1000 ms with no frame in between; the pumps it took must stay within the ceiling,
 * and the timers must have fired as often as their intervals say (a cheaper advance that drops
 * firings is no fix). The ceilings are the counts measured when this was written plus a little room:
 * a change that pumps more per step, or steps where no timer is due, fails here.
 * Built as karouka_host_clock_pumps_test (C, public header only). */

#include <karouka/karouka.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static double clock_ms;

static uint64_t pumps(kr_view* view)
{
    kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
    CHECK(kr_view_query_stats(view, &stats) == KR_OK);
    return stats.event_loop_pumps;
}

static void run_frames(kr_view* view, int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 1000.0 / 60.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
}

/* The page's counters, from its title "a=.. b=.. c=.. d=.."; -1 for those it doesn't show. */
typedef struct {
    int a, b, c, d;
} counts;

static counts title_counts(kr_view* view)
{
    counts result = { -1, -1, -1, -1 };
    sscanf(kr_view_title(view), "a=%d b=%d c=%d d=%d", &result.a, &result.b, &result.c, &result.d);
    return result;
}

/* Loads `html`, runs 30 frames, then advances the host clock 1000 ms at once; returns the pumps that
 * advance took, and the page's counters before and after it in *start and *end. */
static uint64_t idle_second_pumps(kr_view* view, char const* name, char const* html, counts* start, counts* end)
{
    CHECK(kr_view_load_html(view, html, strlen(html), "file:///karouka-host-clock-pumps-test/") == KR_OK);
    run_frames(view, 30);
    *start = title_counts(view);
    uint64_t before = pumps(view);
    clock_ms += 1000.0;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    uint64_t taken = pumps(view) - before;
    *end = title_counts(view);
    fprintf(stderr, "karouka_host_clock_pumps_test: %s: %llu pumps, a +%d b +%d c +%d d +%d\n", name, (unsigned long long)taken,
        end->a - start->a, end->b - start->b, end->c - start->c, end->d - start->d);
    return taken;
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_host_clock_pumps_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* A static page: nothing is due, so the advance pumps for the tasks left from the last frame
     * and the rendering update (2 when this was written, as measured). */
    static char const static_page[] = "<!doctype html><body style='background:rgb(0,0,255)'><p>static</p></body>";
    counts start, end;
    uint64_t static_pumps = idle_second_pumps(view, "static page", static_page, &start, &end);
    CHECK(static_pumps >= 1 && static_pumps <= 2);

    /* One 250 ms interval: four deadlines in the second (10 pumps when this was written). */
    static char const interval_page[] =
        "<!doctype html><script>"
        "let a = 0;"
        "setInterval(() => { document.title = 'a=' + ++a; }, 250);"
        "</script>";
    uint64_t interval_pumps = idle_second_pumps(view, "250 ms interval", interval_page, &start, &end);
    CHECK(end.a - start.a == 4);
    CHECK(interval_pumps >= 4 && interval_pumps <= 12);

    /* Timers at 10 and 7.5 ms (7: WebIDL's long truncates it), a 33 ms setTimeout chain, and a
     * requestIdleCallback loop, whose idle periods last up to 50 ms. Previously, the idle callbacks
     * made every one of the 1000 millisecond steps pump: 1313 pumps; 578 when this was written. */
    static char const busy_page[] =
        "<!doctype html><script>"
        "let a = 0, b = 0, c = 0, d = 0;"
        "function show() { document.title = `a=${a} b=${b} c=${c} d=${d}`; }"
        "setInterval(() => { ++a; show(); }, 10);"
        "setInterval(() => { ++b; show(); }, 7.5);"
        "(function chain() { setTimeout(() => { ++c; show(); chain(); }, 33); })();"
        "(function idle() { requestIdleCallback(() => { ++d; show(); idle(); }); })();"
        "</script>";
    uint64_t busy_pumps = idle_second_pumps(view, "timers and idle callbacks", busy_page, &start, &end);
    CHECK(end.a - start.a == 100);
    /* 142 and 30 when this was written; where the frames before fell decides whether one more fits. */
    CHECK(end.b - start.b >= 142 && end.b - start.b <= 143);
    CHECK(end.c - start.c >= 30 && end.c - start.c <= 31);
    CHECK(end.d - start.d >= 20);
    CHECK(busy_pumps <= 640);

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_host_clock_pumps_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
