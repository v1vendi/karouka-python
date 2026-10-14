/* Off-thread work lands at the same frame on every run (patch 0051). A fetched classic
 * script and an async one compile on a LibThreading pool thread, and a linked stylesheet
 * parses on one; on the host clock each pump waits for that work first, so the page below loads,
 * runs its scripts and applies its sheet at one fixed frame, however long the compiles take on the
 * wall clock. Before, a headless frame took microseconds and the results came back on the wall
 * clock: demo/ranger_sector's 32 KB script ran anywhere from frame 31 to frame 69 by machine load.
 * The scripts here are ~400 KB each, so their compiles outlast many frames (without the wait the
 * page was still loading after 4 frames, every load). Loads the page six times on one system, a new
 * view each time: every load must reach its load event within the first few frames with every
 * script run and the sheet applied, and the loads after the first at the same frame with the same
 * title. The first load in a fresh system lands a frame later (frame 2, then frame 1), the same on
 * every run, so it is not compared. Built as karouka_script_frame_test (as C, against the
 * public header only). */

#include "test_util.h"

#include <karouka/karouka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static char dir[64];
static double clock_ms;

/* The load event ends at most this many frames in. */
static int const max_load_frames = 4;
static int const loads = 6;

static FILE* open_in_dir(char const* name)
{
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* file = fopen(path, "w");
    CHECK(file != NULL);
    return file;
}

/* A script of `functions` small functions (~40 bytes each) and then `tail`. */
static void write_script(char const* name, int functions, char const* tail)
{
    FILE* file = open_in_dir(name);
    if (!file)
        return;
    for (int i = 0; i < functions; ++i)
        fprintf(file, "function f%d(a, b) { return a * %d + b - %d; }\n", i, i, i % 7);
    fprintf(file, "%s\n", tail);
    fclose(file);
}

static void write_text(char const* name, char const* contents)
{
    FILE* file = open_in_dir(name);
    if (!file)
        return;
    fputs(contents, file);
    fclose(file);
}

static void write_stylesheet(char const* name, int rules)
{
    FILE* file = open_in_dir(name);
    if (!file)
        return;
    for (int i = 0; i < rules; ++i)
        fprintf(file, ".c%d { margin: %dpx; color: rgb(%d, 0, 0); }\n", i, i % 9, i % 256);
    fputs("#box { width: 37px; height: 5px; }\n", file);
    fclose(file);
}

static char const page[] = "<!doctype html><title>loading</title>"
                           "<link rel=stylesheet href=big.css>"
                           "<body><div id=box></div>"
                           "<script>window.order = [];</script>"
                           "<script async src=async.js></script>"
                           "<script src=big.js></script>"
                           "<script>window.order.push('inline');"
                           "addEventListener('load', () => { document.title = 'loaded ' + getComputedStyle(box).width + ' '"
                           " + window.order.join(',') + ' big at ' + window.bigAt; });</script></body>";

int main(void)
{
    strcpy(dir, "/tmp/karouka-script-frame-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    /* file: URLs name the real path: /tmp is a symlink to /private/tmp. */
    char* real = realpath(dir, NULL);
    if (real) {
        snprintf(dir, sizeof dir, "%s", real);
        free(real);
    }
    write_text("page.html", page);
    write_stylesheet("big.css", 4000);
    /* Whole milliseconds of host time, so the title is the same whenever the script runs in a frame. */
    write_script("big.js", 10000, "window.order.push('big'); window.bigAt = Math.floor(performance.now());");
    write_script("async.js", 10000, "window.order.push('async');");

    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_script_frame_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 100, .height = 100, .scale = 1 };
    char page_path[128];
    snprintf(page_path, sizeof page_path, "%s/page.html", dir);

    int first_frame = -1;
    char first_title[256] = "";
    for (int load = 0; load < loads; ++load) {
        /* A new view on the system's clock each time; the clock only moves on, so frames count from the load. */
        kr_view* view = NULL;
        CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
        if (!view)
            break;
        CHECK(kr_view_load_file(view, page_path) == KR_OK);
        double load_ms = clock_ms;
        int frame = 0;
        while (frame < max_load_frames && strncmp(kr_view_title(view), "loaded", 6) != 0) {
            CHECK(run_frames_on(view, &clock_ms, 1));
            ++frame;
        }
        char const* title = kr_view_title(view);
        printf("load %d: '%s' at frame %d (host %.0f ms after the load call)\n", load, title, frame, clock_ms - load_ms);
        if (strncmp(title, "loaded 37px ", 12) != 0 || !strstr(title, "big") || !strstr(title, "async")) {
            fprintf(stderr, "load %d: title '%s' after %d frames; want the load event's, with the sheet's 37px and every script\n", load, title, frame);
            ++failures;
        }
        if (load == 0) {
            /* Checked above, not compared. */
        } else if (load == 1) {
            first_frame = frame;
            snprintf(first_title, sizeof first_title, "%s", title);
        } else if (frame != first_frame || strcmp(title, first_title) != 0) {
            fprintf(stderr, "load %d: '%s' at frame %d; load 1 was '%s' at frame %d\n", load, title, frame, first_title, first_frame);
            ++failures;
        }
        kr_view_destroy(view);
    }

    kr_system_destroy(system);
    char const* names[] = { "page.html", "big.css", "big.js", "async.js" };
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        char path[128];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
    if (failures) {
        fprintf(stderr, "karouka_script_frame_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_script_frame_test: ok\n");
    return 0;
}
