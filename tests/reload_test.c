/* kr_view_reload and kr_view_get_dependencies. A page loaded from a temporary directory
 * lists its own file and every file: subresource it requested (one that is missing too, failed);
 * after its stylesheet and script change on disk, kr_view_reload shows the new ones, keeps the
 * viewport's scroll position (clamped to the new page's height) and lists the same files under a new
 * revision. A reload whose file is gone fails and keeps the page. A kr_view_load_html page lists its
 * subresources but no document, and reloads its own markup. Built as karouka_reload_test (C, public
 * header only) and run by tools/dev.sh check. A load is waited for by its title (settle_title), at
 * most 10 frames. */

#include "test_util.h"

#include <karouka/karouka.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static double clock_ms;
static char dir[64];

static void run_frames(kr_view* view, int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
}

static void write_file(char const* name, char const* contents)
{
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* file = fopen(path, "w");
    CHECK(file != NULL);
    if (!file)
        return;
    fputs(contents, file);
    fclose(file);
}

static void expect_title(kr_view* view, char const* expected, int line)
{
    char const* title = kr_view_title(view);
    if (strcmp(title, expected) != 0) {
        fprintf(stderr, "reload_test.c:%d: title is '%s', want '%s'\n", line, title, expected);
        ++failures;
    }
}
#define EXPECT_TITLE(view, expected) expect_title(view, expected, __LINE__)

/* Drives frames until the page has loaded and its title reads `expected`, at most 10 (test_util.h).
 * script.js is compiled on a pool thread; the event loop waits for that compile on the host clock
 *. Before, on a loaded machine the compile could outlast 10 frames, and the title was
 * still '' at frame 10 with the <img> request landing in the frames after it. */
#define SETTLE_TITLE(view, expected)                  \
    do {                                              \
        if (!settle_title(view, &clock_ms, expected)) \
            ++failures;                               \
    } while (0)

/* The entry for `name` (a file in dir), or NULL. */
static kr_view_dependency const* find(kr_view_dependencies const* deps, char const* name)
{
    char url[160];
    snprintf(url, sizeof url, "file://%s/%s", dir, name);
    for (size_t i = 0; i < deps->count; ++i) {
        if (strcmp(deps->entries[i].url, url) == 0)
            return &deps->entries[i];
    }
    return NULL;
}

static void expect_dependency(kr_view_dependencies const* deps, char const* name, uint32_t kinds, kr_result status, int line)
{
    kr_view_dependency const* entry = find(deps, name);
    if (!entry) {
        fprintf(stderr, "reload_test.c:%d: %s is not a dependency; there are %zu:\n", line, name, deps->count);
        for (size_t i = 0; i < deps->count; ++i)
            fprintf(stderr, "  %s\n", deps->entries[i].url);
        ++failures;
        return;
    }
    if (entry->kinds != kinds || entry->pending || entry->status != status) {
        fprintf(stderr, "reload_test.c:%d: %s: kinds %u pending %d status %d, want kinds %u pending 0 status %d\n", line, name,
            entry->kinds, entry->pending, entry->status, kinds, status);
        ++failures;
    }
}
#define EXPECT_DEPENDENCY(deps, name, kinds, status) expect_dependency(deps, name, kinds, status, __LINE__)

static kr_view_dependencies dependencies_of(kr_view* view)
{
    kr_view_dependencies deps = { .struct_size = sizeof(kr_view_dependencies) };
    CHECK(kr_view_get_dependencies(view, &deps) == KR_OK);
    return deps;
}

/* The page reports its stylesheet's width, its script's value and the viewport's scroll position in
 * its title, every frame from its load event on. */
static char const page[] = "<!doctype html><html><head>"
                           "<link rel=stylesheet href=style.css><script src=script.js></script>"
                           "</head><body style='margin:0'><div id=box></div><img src=missing.png hidden>"
                           "<script>addEventListener('load', () => { (function f() {"
                           "document.title = getComputedStyle(box).width + ' ' + window.scriptValue + ' ' + scrollY;"
                           "requestAnimationFrame(f); })(); });</script></body></html>";

int main(void)
{
    strcpy(dir, "/tmp/karouka-reload-XXXXXX");
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

    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_reload_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 200, .height = 200, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* Nothing loaded: nothing to reload, no dependencies. */
    CHECK(kr_view_reload(NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_reload(view) == KR_ERROR_INVALID_STATE);
    kr_view_dependencies deps = dependencies_of(view);
    CHECK(deps.count == 0);
    CHECK(deps.revision != 0);
    CHECK(kr_view_get_dependencies(view, NULL) == KR_ERROR_INVALID_ARGUMENT);
    kr_view_dependencies unsized = { .struct_size = 0 };
    CHECK(kr_view_get_dependencies(view, &unsized) == KR_ERROR_INVALID_ARGUMENT);

    write_file("page.html", page);
    write_file("style.css", "#box { width: 10px; height: 1000px; }");
    write_file("script.js", "window.scriptValue = 'one';");
    char page_path[128];
    snprintf(page_path, sizeof page_path, "%s/page.html", dir);
    CHECK(kr_view_load_file(view, page_path) == KR_OK);
    SETTLE_TITLE(view, "10px one 0");

    deps = dependencies_of(view);
    CHECK(deps.count == 4);
    CHECK(!deps.truncated);
    CHECK(deps.count > 0 && strstr(deps.entries[0].url, "/page.html") != NULL); /* first: the document */
    EXPECT_DEPENDENCY(&deps, "page.html", KR_DEPENDENCY_DOCUMENT, KR_OK);
    EXPECT_DEPENDENCY(&deps, "style.css", KR_DEPENDENCY_SUBRESOURCE, KR_OK);
    EXPECT_DEPENDENCY(&deps, "script.js", KR_DEPENDENCY_SUBRESOURCE, KR_OK);
    EXPECT_DEPENDENCY(&deps, "missing.png", KR_DEPENDENCY_SUBRESOURCE, KR_ERROR_IO);
    /* Nothing loads: the same answer again. */
    uint64_t first_revision = deps.revision;
    run_frames(view, 2);
    CHECK(dependencies_of(view).revision == first_revision);

    /* A smaller struct gets the members that fit: entries, nothing past it. */
    struct {
        kr_view_dependencies deps;
        unsigned char guard[8];
    } small;
    memset(&small, 0xAB, sizeof small);
    small.deps.struct_size = (uint32_t)(offsetof(kr_view_dependencies, entries) + sizeof(small.deps.entries));
    CHECK(kr_view_get_dependencies(view, &small.deps) == KR_OK);
    CHECK(small.deps.entries != NULL);
    unsigned char const* count_bytes = (unsigned char const*)&small.deps.count;
    CHECK(count_bytes[0] == 0xAB && count_bytes[sizeof(size_t) - 1] == 0xAB);
    deps = dependencies_of(view); /* the small call replaced the array */

    /* Scroll down 500px with the wheel. */
    kr_input_event wheel = { .struct_size = sizeof(kr_input_event), .type = KR_INPUT_WHEEL, .x = 100, .y = 100, .delta_y = 500 };
    CHECK(kr_view_inject_input2(view, &wheel) == KR_OK);
    run_frames(view, 4);
    EXPECT_TITLE(view, "10px one 500");

    /* The stylesheet and the script change on disk; a reload shows both, scrolled where it was. */
    write_file("style.css", "#box { width: 20px; height: 1000px; }");
    write_file("script.js", "window.scriptValue = 'two';");
    CHECK(kr_view_reload(view) == KR_OK);
    SETTLE_TITLE(view, "20px two 500");
    kr_view_dependencies reloaded = dependencies_of(view);
    CHECK(reloaded.revision != first_revision);
    CHECK(reloaded.count == 4);
    EXPECT_DEPENDENCY(&reloaded, "page.html", KR_DEPENDENCY_DOCUMENT, KR_OK);
    EXPECT_DEPENDENCY(&reloaded, "style.css", KR_DEPENDENCY_SUBRESOURCE, KR_OK);
    EXPECT_DEPENDENCY(&reloaded, "script.js", KR_DEPENDENCY_SUBRESOURCE, KR_OK);

    /* A shorter page: the position is clamped to its new bottom (300 - 200). */
    write_file("style.css", "#box { width: 30px; height: 300px; }");
    CHECK(kr_view_reload(view) == KR_OK);
    SETTLE_TITLE(view, "30px two 100");

    /* A page that scrolls itself on load keeps its own position. */
    write_file("script.js", "window.scriptValue = 'three'; addEventListener('load', () => scrollTo(0, 40));");
    CHECK(kr_view_reload(view) == KR_OK);
    SETTLE_TITLE(view, "30px three 40");

    /* The page's file is gone: the reload fails and the page stays, dependencies and all. */
    uint64_t before_failure = dependencies_of(view).revision;
    char moved_path[160];
    snprintf(moved_path, sizeof moved_path, "%s/page.moved", dir);
    CHECK(rename(page_path, moved_path) == 0);
    CHECK(kr_view_reload(view) == KR_ERROR_IO);
    run_frames(view, 2);
    EXPECT_TITLE(view, "30px three 40");
    CHECK(dependencies_of(view).revision == before_failure);
    CHECK(rename(moved_path, page_path) == 0);

    /* Markup from the host: its subresources are listed, no document; a reload replays the markup. */
    char base_url[96];
    snprintf(base_url, sizeof base_url, "file://%s/", dir);
    char const markup[] = "<!doctype html><link rel=stylesheet href=style.css><div id=box></div>"
                          "<script>addEventListener('load', () => { document.title = 'markup ' + getComputedStyle(box).width; });</script>";
    CHECK(kr_view_load_html(view, markup, strlen(markup), base_url) == KR_OK);
    SETTLE_TITLE(view, "markup 30px");
    deps = dependencies_of(view);
    CHECK(deps.count == 1);
    EXPECT_DEPENDENCY(&deps, "style.css", KR_DEPENDENCY_SUBRESOURCE, KR_OK);
    write_file("style.css", "#box { width: 40px; }");
    CHECK(kr_view_reload(view) == KR_OK);
    SETTLE_TITLE(view, "markup 40px");

    kr_view_destroy(view);
    kr_system_destroy(system);

    char const* names[] = { "page.html", "style.css", "script.js" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char path[160];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);

    if (failures) {
        fprintf(stderr, "karouka_reload_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_reload_test: ok\n");
    return 0;
}
