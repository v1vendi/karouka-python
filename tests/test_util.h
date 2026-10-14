/* Helpers shared by the C tests that drive a kr_view through the public header. Header-only and
 * static, so each test (one translation unit) gets its own copy; included as "test_util.h" from the
 * test's own directory, so no include path is needed. */

#ifndef KAROUKA_TESTS_TEST_UTIL_H
#define KAROUKA_TESTS_TEST_UTIL_H

#include <karouka/karouka.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Drives `frames` frames, advancing the host clock `*clock_ms` by 16 ms before each. Returns 0 if a
 * frame failed. */
static inline int run_frames_on(kr_view* view, double* clock_ms, int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        *clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, *clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return 0;
    }
    return 1;
}

/* Drives frames until the title reads `expected`, at most 10, then two more, and checks it still
 * does, so the parser, any off-thread compile and the page's scripts have run. A fetched script or
 * stylesheet is compiled or parsed on a pool thread, and on the host clock the event loop waits for
 * that before each pump, so a load takes the same frames on every run, however loaded
 * the machine is. Until then the results came back on the wall clock, and this waited up to 10 s of
 * wall time for them. Returns 1 if every frame succeeded and the title reads
 * `expected`; otherwise prints why, tagged `file:line`, and returns 0. */
static inline int settle_title_at(kr_view* view, double* clock_ms, char const* expected, char const* file, int line)
{
    int ok = 1;
    for (int frame = 0; ok && frame < 10 && strcmp(kr_view_title(view), expected) != 0; ++frame)
        ok = run_frames_on(view, clock_ms, 1);
    if (ok)
        ok = run_frames_on(view, clock_ms, 2);
    if (!ok) {
        fprintf(stderr, "%s:%d: a frame failed while waiting for title '%s'\n", file, line, expected);
        return 0;
    }
    char const* title = kr_view_title(view);
    if (strcmp(title, expected) != 0) {
        fprintf(stderr, "%s:%d: title is '%s' after 10 frames and 2 more, want '%s'\n", file, line, title, expected);
        return 0;
    }
    return 1;
}

/* settle_title_at, tagged with the caller's file and line. */
#define settle_title(view, clock_ms, expected) settle_title_at(view, clock_ms, expected, __FILE__, __LINE__)

#endif
