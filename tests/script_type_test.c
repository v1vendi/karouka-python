/* Embedder script types (kr_view_set_script_type_callback, patch 0099; docs/PYTHON.md § Scripts).
 * A page from a temporary directory mixes JavaScript and text/python scripts: inline, src=,
 * defer and async, one whose src= is missing and one the callback fails. The JavaScript side logs
 * into document.title; the callback merges that log (read through kr_view_title as it is called)
 * with its own calls, so the run's order is checked against HTML's order for classic scripts:
 * parser-blocking scripts in document order, `load` after a src= script, `error` for a missing src=
 * or a failed script, then defer scripts in order before DOMContentLoaded, an async one before the
 * window's load. The callback gets UTF-8 text, the URL and the start line. With the callback
 * removed, or for a type never registered, the scripts are ignored and fire nothing. Built as
 * karouka_script_type_test (as C, against the public header only) and run by tools/dev.sh check. */

#include "test_util.h"

#include <karouka/karouka.h>

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

static char dir[64];
static double clock_ms;

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

/* The merged order: JavaScript's entries (from the title) and "py:<source>" per callback. */
static char merged[2048];
static int js_merged;
static int calls;

#define MAX_CALLS 16
static struct {
    char source[64];
    char url[256];
    uint32_t line;
    size_t length;
} call_log[MAX_CALLS];

static void append(char const* token)
{
    if (merged[0])
        strncat(merged, " ", sizeof merged - strlen(merged) - 1);
    strncat(merged, token, sizeof merged - strlen(merged) - 1);
}

/* Appends the JavaScript entries the title holds beyond those already merged. */
static void merge_js(kr_view* view)
{
    char title[1024];
    snprintf(title, sizeof title, "%s", kr_view_title(view));
    int index = 0;
    for (char* token = strtok(title, " "); token; token = strtok(NULL, " "), ++index) {
        if (index >= js_merged)
            append(token);
    }
    if (index > js_merged)
        js_merged = index;
}

static bool on_script(kr_view* view, char const* source, size_t source_length, char const* url, uint32_t line, void* user_data)
{
    CHECK(user_data == (void*)&calls);
    CHECK(strlen(source) == source_length);
    merge_js(view);
    char token[80];
    snprintf(token, sizeof token, "py:%s", source);
    append(token);
    if (calls < MAX_CALLS) {
        snprintf(call_log[calls].source, sizeof call_log[calls].source, "%s", source);
        snprintf(call_log[calls].url, sizeof call_log[calls].url, "%s", url);
        call_log[calls].line = line;
        call_log[calls].length = source_length;
    }
    ++calls;
    return strcmp(source, "fail") != 0;
}

static void reset_log(void)
{
    merged[0] = 0;
    js_merged = 0;
    calls = 0;
    memset(call_log, 0, sizeof call_log);
}

static int find_call(char const* source)
{
    for (int i = 0; i < calls && i < MAX_CALLS; ++i) {
        if (strcmp(call_log[i].source, source) == 0)
            return i;
    }
    return -1;
}

static bool ends_with(char const* string, char const* suffix)
{
    size_t length = strlen(string), suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(string + length - suffix_length, suffix) == 0;
}

/* Loads `name` and drives frames until its title ends with " load", at most 20, then two more. */
static void load_and_settle(kr_view* view, char const* name)
{
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(kr_view_load_file(view, path) == KR_OK);
    for (int frame = 0; frame < 20 && !ends_with(kr_view_title(view), " load"); ++frame)
        CHECK(run_frames_on(view, &clock_ms, 1));
    CHECK(run_frames_on(view, &clock_ms, 2));
    merge_js(view);
}

static char const mixed_page[] =
    "<!doctype html><title></title>\n"
    "<script>var order = []; function log(x) { order.push(x); document.title = order.join(' '); }\n"
    "document.addEventListener('DOMContentLoaded', () => log('dcl'));\n"
    "addEventListener('load', () => log('load'));</script>\n"
    "<script>log('js1')</script>\n"
    "<script type=\"text/python\">inline1</script>\n" /* line 6 */
    "<script src=\"a.js\"></script>\n"
    "<script type=\"Text/Python; version=3\" src=\"b.py\" onload=\"log('b-load')\" onerror=\"log('b-error')\"></script>\n"
    "<script defer src=\"d1.js\"></script>\n"
    "<script defer type=\"text/python\" src=\"d2.py\"></script>\n"
    "<script async type=\"text/python\" src=\"as.py\"></script>\n"
    "<script type=\"text/python\" src=\"missing.py\" onerror=\"log('missing-error')\"></script>\n"
    "<script type=\"text/python\" onerror=\"log('fail-error')\" onload=\"log('fail-load')\">fail</script>\n"
    "<script type=\"text/ruby\" onerror=\"log('ruby-error')\">ruby</script>\n"
    "<script>log('js2')</script>\n";

static char const ignored_page[] =
    "<!doctype html><title></title>\n"
    "<script>var order = []; function log(x) { order.push(x); document.title = order.join(' '); }\n"
    "addEventListener('load', () => log('load'));</script>\n"
    "<script type=\"text/python\" onerror=\"log('inline-error')\">inline1</script>\n"
    "<script type=\"text/python\" src=\"b.py\" onload=\"log('b-load')\" onerror=\"log('b-error')\"></script>\n"
    "<script>log('js')</script>\n";

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/karouka-script-type-XXXXXX");
    if (!mkdtemp(dir)) {
        fprintf(stderr, "karouka_script_type_test: mkdtemp failed\n");
        return 1;
    }
    write_file("mixed.html", mixed_page);
    write_file("ignored.html", ignored_page);
    write_file("a.js", "log('js-src')");
    write_file("b.py", "b-\xc3\xa9");
    write_file("d1.js", "log('js-defer')");
    write_file("d2.py", "defer2");
    write_file("as.py", "async");

    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_script_type_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* Refused: no view, no type, not a MIME type, JavaScript. */
    CHECK(kr_view_set_script_type_callback(NULL, "text/python", on_script, &calls) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_set_script_type_callback(view, NULL, on_script, &calls) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_set_script_type_callback(view, "python", on_script, &calls) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_set_script_type_callback(view, "text/javascript", on_script, &calls) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_set_script_type_callback(view, "TEXT/PYTHON", on_script, &calls) == KR_OK);

    reset_log();
    load_and_settle(view, "mixed.html");
    /* The async script runs once, at a time HTML leaves open, but before the window's load. */
    char const* async_at = strstr(merged, "py:async");
    char const* load_at = strstr(merged, " load");
    CHECK(async_at && load_at && async_at < load_at);
    if (async_at) {
        char without_async[sizeof merged];
        snprintf(without_async, sizeof without_async, "%.*s%s", (int)(async_at - merged), merged, async_at + strlen("py:async "));
        snprintf(merged, sizeof merged, "%s", without_async);
    }
    char const* expected = "js1 py:inline1 js-src py:b-\xc3\xa9 b-load missing-error py:fail fail-error js2 js-defer py:defer2 dcl load";
    if (strcmp(merged, expected) != 0) {
        fprintf(stderr, "order is '%s',\n     want '%s'\n", merged, expected);
        ++failures;
    }
    /* missing.py fires error and is never handed over; text/ruby is ignored. */
    CHECK(strstr(kr_view_title(view), "missing-error") != NULL);
    CHECK(strstr(kr_view_title(view), "ruby") == NULL);
    CHECK(calls == 5);

    int inline1 = find_call("inline1");
    CHECK(inline1 >= 0);
    if (inline1 >= 0) {
        CHECK(ends_with(call_log[inline1].url, "/mixed.html"));
        CHECK(strncmp(call_log[inline1].url, "file://", 7) == 0);
        if (call_log[inline1].line != 6)
            fprintf(stderr, "inline script line is %u, want 6\n", call_log[inline1].line), ++failures;
    }
    int b = find_call("b-\xc3\xa9");
    CHECK(b >= 0);
    if (b >= 0) {
        CHECK(ends_with(call_log[b].url, "/b.py"));
        CHECK(call_log[b].line == 1);
        CHECK(call_log[b].length == 4);
    }

    /* A type the view registered nothing for, or no longer, is ignored: no call, no event. */
    CHECK(kr_view_set_script_type_callback(view, "text/python", NULL, NULL) == KR_OK);
    reset_log();
    load_and_settle(view, "ignored.html");
    CHECK(calls == 0);
    if (strcmp(kr_view_title(view), "js load") != 0)
        fprintf(stderr, "ignored page title is '%s', want 'js load'\n", kr_view_title(view)), ++failures;

    /* A second view with no callback ignores the type while the first runs it. */
    kr_view* other = NULL;
    CHECK(kr_view_create2(system, &view_desc, &other) == KR_OK);
    CHECK(kr_view_set_script_type_callback(view, "text/python", on_script, &calls) == KR_OK);
    reset_log();
    load_and_settle(other, "ignored.html");
    CHECK(calls == 0);
    CHECK(strcmp(kr_view_title(other), "js load") == 0);
    reset_log();
    load_and_settle(view, "ignored.html");
    CHECK(calls == 2);
    if (strcmp(merged, "py:inline1 py:b-\xc3\xa9 b-load js load") != 0)
        fprintf(stderr, "registered page order is '%s'\n", merged), ++failures;

    kr_view_destroy(other);
    kr_view_destroy(view);
    kr_system_destroy(system);

    char const* names[] = { "mixed.html", "ignored.html", "a.js", "b.py", "d1.js", "d2.py", "as.py" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char path[128];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);

    if (failures) {
        fprintf(stderr, "karouka_script_type_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_script_type_test: ok\n");
    return 0;
}
