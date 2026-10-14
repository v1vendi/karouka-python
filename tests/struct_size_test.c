/* kr_system_desc, kr_view_desc and kr_memory_stats, kr_font_desc, kr_input_event,
 * kr_metal_output and kr_engine_callbacks are versioned by struct_size, as kr_view_stats
 * is (view_stats_test.c). For each, a host built against an older header
 * (a smaller struct_size), against a newer one (a larger struct_size, members karouka doesn't know
 * at its end) and one passing a size between members: karouka reads or writes only the whole
 * members within struct_size. Each struct sits in a buffer filled with a sentinel past what the
 * host set, so a descriptor member read past struct_size would be garbage (a pointer karouka would
 * use) and a stats member written past it would change the sentinel. Also the entry points hosts
 * built before struct_size call (kr_system_create, kr_view_create, kr_system_get_memory_stats,
 * kr_system_add_font, kr_system_add_font_file, kr_view_inject_input, kr_view_set_output,
 * kr_view_set_engine_callbacks). And kr_host_interface: a log within struct_size gets the
 * page's console lines, one that ends past it (a size between members) is not read, and a
 * struct_size that can't hold io is refused.
 *
 * One system per process: the main run creates its system from an older header's desc; it runs
 * itself again ("newer", "legacy") for the other two system cases, and ("metal") for
 * kr_metal_output, which needs a system with the host's Metal device and queue.
 * Built as karouka_struct_size_test (C, public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char** environ;

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

/* What hosts built before struct_size call; their structs had no struct_size member. */
kr_result kr_system_create(void const* desc, kr_system** out_system);
kr_result kr_view_create(kr_system* system, void const* desc, kr_view** out_view);
kr_result kr_system_get_memory_stats(kr_system* system, void* out_stats);
kr_result kr_system_add_font(kr_system* system, void const* data, size_t size, void const* desc);
kr_result kr_system_add_font_file(kr_system* system, char const* path, void const* desc);
kr_result kr_view_inject_input(kr_view* view, void const* event);
kr_result kr_view_set_output(kr_view* view, void const* output);
void kr_view_set_engine_callbacks(kr_view* view, void const* callbacks);

#define SENTINEL 0xAB
#define MiB (1024u * 1024u)

/* 8-byte aligned, as a host's struct would be, with room for a newer header's members. */
static union {
    kr_system_desc system_desc;
    kr_view_desc view_desc;
    kr_memory_stats memory;
    kr_font_desc font_desc;
    kr_input_event input;
    kr_metal_output output;
    kr_engine_callbacks callbacks;
    uint64_t align;
    unsigned char bytes[128];
} buffer;

static void fill_sentinel(void)
{
    memset(buffer.bytes, SENTINEL, sizeof buffer.bytes);
}

/* The sentinel, then the first `end` bytes of `value` over it: what a host whose struct ends at `end`
 * passes, with a newer header's (or garbage) bytes after. */
static void fill_from(void const* value, size_t end)
{
    fill_sentinel();
    memcpy(buffer.bytes, value, end);
}

/* Whether bytes [from, sizeof buffer) all still hold the sentinel. */
static int untouched_from(size_t from)
{
    for (size_t i = from; i < sizeof buffer.bytes; ++i) {
        if (buffer.bytes[i] != SENTINEL)
            return 0;
    }
    return 1;
}

static uint64_t u64_at(size_t offset)
{
    uint64_t value;
    memcpy(&value, buffer.bytes + offset, sizeof value);
    return value;
}

static uint64_t gpu_cache_budget(kr_system* system)
{
    kr_memory_stats memory = { .struct_size = sizeof(kr_memory_stats) };
    CHECK(kr_system_query_memory_stats(system, &memory) == KR_OK);
    return memory.gpu_cache_budget_bytes;
}

/* A system desc with the members before `end` set (gpu_cache_bytes = budget) and the sentinel from
 * `end` on: past struct_size, mtl_device, mtl_queue and host would be garbage pointers. */
static void fill_system_desc(size_t end, uint32_t struct_size, size_t budget)
{
    fill_sentinel();
    kr_system_desc desc = { .struct_size = struct_size, .gpu_cache_bytes = budget };
    memcpy(buffer.bytes, &desc, end);
}

static double clock_ms;

/* ---- kr_host_interface */

static union {
    kr_host_interface host;
    uint64_t align;
    unsigned char bytes[128];
} host_buffer;

static int console_lines;

static void count_console_line(void* user_data, kr_log_level level, char const* category, char const* message)
{
    (void)user_data;
    (void)level;
    if (strcmp(category, "console") == 0 && strcmp(message, "struct-size-test") == 0)
        ++console_lines;
}

/* A host interface with the members before `end` set (log.write counts the page's console lines,
 * from TRACE) and the sentinel from `end` on. */
static kr_host_interface const* fill_host_interface(size_t end, uint32_t struct_size)
{
    memset(host_buffer.bytes, SENTINEL, sizeof host_buffer.bytes);
    kr_host_interface host = { .struct_size = struct_size };
    host.log.write = count_console_line;
    host.log.min_level = KR_LOG_TRACE;
    memcpy(host_buffer.bytes, &host, end);
    return &host_buffer.host;
}

/* How many times a page's console.log reached the host log. */
static int page_console_lines(kr_system* system)
{
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return -1;
    console_lines = 0;
    static char const page[] = "<!doctype html><script>console.log('struct-size-test');</script>";
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-struct-size-test/") == KR_OK);
    for (int frame = 0; frame < 6; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
    kr_view_destroy(view);
    return console_lines;
}

/* ---- views */

/* The view's devicePixelRatio, as its page reports it in the title. */
static char const* page_scale(kr_view* view)
{
    static char const page[] = "<!doctype html><script>document.title = String(devicePixelRatio);</script>";
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-struct-size-test/") == KR_OK);
    for (int frame = 0; frame < 6; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
    return kr_view_title(view);
}

/* A view from buffer.view_desc; its page's devicePixelRatio, or "" if karouka refused it. */
static char const* create_view_scale(kr_system* system, kr_result expect)
{
    kr_view* view = NULL;
    kr_result result = kr_view_create2(system, &buffer.view_desc, &view);
    CHECK(result == expect);
    if (result != KR_OK)
        return "";
    static char title[16];
    snprintf(title, sizeof title, "%s", page_scale(view));
    kr_view_destroy(view);
    return title;
}

/* width and height 64, scale 2 and transparent_background, the sentinel from `end` on. */
static void fill_view_desc(size_t end, uint32_t struct_size)
{
    fill_sentinel();
    kr_view_desc desc = { .struct_size = struct_size, .width = 64, .height = 64, .scale = 2, .transparent_background = true };
    memcpy(buffer.bytes, &desc, end);
}

static void check_view_desc(kr_system* system)
{
    size_t const scale_at = offsetof(kr_view_desc, scale);

    /* The current header. */
    fill_view_desc(sizeof(kr_view_desc), sizeof(kr_view_desc));
    CHECK(strcmp(create_view_scale(system, KR_OK), "2") == 0);

    /* A header from before scale: scale (2) and transparent_background past struct_size are not
     * read, so they are their defaults and the scale is 1. */
    fill_view_desc(sizeof(kr_view_desc), (uint32_t)scale_at);
    CHECK(strcmp(create_view_scale(system, KR_OK), "1") == 0);
    /* A size between members: none of scale's bytes. */
    fill_view_desc(sizeof(kr_view_desc), (uint32_t)(scale_at + 2));
    CHECK(strcmp(create_view_scale(system, KR_OK), "1") == 0);

    /* A newer header's larger struct, with members karouka doesn't know at its end. */
    fill_view_desc(sizeof(kr_view_desc), (uint32_t)sizeof buffer.bytes);
    CHECK(strcmp(create_view_scale(system, KR_OK), "2") == 0);

    /* No size, or too small for width: refused. Width only: height is 0, refused too. */
    fill_view_desc(sizeof(kr_view_desc), 0);
    create_view_scale(system, KR_ERROR_INVALID_ARGUMENT);
    fill_view_desc(sizeof(kr_view_desc), offsetof(kr_view_desc, width) + 3);
    create_view_scale(system, KR_ERROR_INVALID_ARGUMENT);
    fill_view_desc(sizeof(kr_view_desc), offsetof(kr_view_desc, height));
    create_view_scale(system, KR_ERROR_INVALID_ARGUMENT);
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, NULL, &view) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(view == NULL);

    /* A host built before struct_size: 16 bytes of width, height, scale, transparent_background. */
    fill_sentinel();
    struct {
        uint32_t width, height;
        float scale;
        bool transparent_background;
    } old_desc = { 64, 64, 2, false };
    memcpy(buffer.bytes, &old_desc, sizeof old_desc);
    CHECK(kr_view_create(system, buffer.bytes, &view) == KR_OK);
    if (view) {
        CHECK(strcmp(page_scale(view), "2") == 0);
        kr_view_destroy(view);
    }
}

/* ---- memory stats */

static void check_memory_stats(kr_system* system)
{
    size_t const footprint_at = offsetof(kr_memory_stats, footprint_bytes);
    size_t const budget_at = offsetof(kr_memory_stats, gpu_cache_budget_bytes);
    size_t const surface_at = offsetof(kr_memory_stats, surface_bytes);
    /* A rendered view, so that surface_bytes is not 0. */
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (view)
        CHECK(strcmp(page_scale(view), "1") == 0);

    /* The current header: every member, nothing past the struct. */
    fill_sentinel();
    buffer.memory.struct_size = sizeof(kr_memory_stats);
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_OK);
    kr_memory_stats const full = buffer.memory;
    CHECK(full.struct_size == sizeof(kr_memory_stats));
    CHECK(full.footprint_bytes > 0);
    CHECK(full.gpu_cache_budget_bytes > 0);
    CHECK(full.surface_bytes == 64 * 64 * 4);
    CHECK(untouched_from(sizeof(kr_memory_stats)));

    /* A header from before surface_bytes: the members before it, and not one byte more. */
    fill_sentinel();
    buffer.memory.struct_size = (uint32_t)surface_at;
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_OK);
    CHECK(buffer.memory.struct_size == surface_at);
    CHECK(buffer.memory.gpu_cache_budget_bytes == full.gpu_cache_budget_bytes);
    CHECK(untouched_from(surface_at));

    /* A size between members gets whole members only. */
    fill_sentinel();
    buffer.memory.struct_size = (uint32_t)(budget_at + 7);
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_OK);
    CHECK(u64_at(footprint_at) > 0);
    CHECK(untouched_from(budget_at));

    /* A newer header's larger struct: the members this library knows; the rest stays the host's. */
    fill_sentinel();
    buffer.memory.struct_size = (uint32_t)sizeof buffer.bytes;
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_OK);
    CHECK(buffer.memory.surface_bytes == full.surface_bytes);
    CHECK(untouched_from(sizeof(kr_memory_stats)));

    /* No size, too small for footprint_bytes, or NULL: refused, nothing written. */
    fill_sentinel();
    buffer.memory.struct_size = 0;
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));
    buffer.memory.struct_size = (uint32_t)(footprint_at + 7);
    CHECK(kr_system_query_memory_stats(system, &buffer.memory) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));
    buffer.memory.struct_size = sizeof(kr_memory_stats);
    CHECK(kr_system_query_memory_stats(NULL, &buffer.memory) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_query_memory_stats(system, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));

    /* A host built before struct_size: 48 bytes from footprint_bytes to gpu_cache_budget_bytes,
     * nothing past them, whatever its struct's first bytes held. */
    fill_sentinel();
    CHECK(kr_system_get_memory_stats(system, buffer.bytes) == KR_OK);
    CHECK(u64_at(0) > 0);
    CHECK(u64_at(40) == full.gpu_cache_budget_bytes);
    CHECK(untouched_from(48));
    CHECK(kr_system_get_memory_stats(system, NULL) == KR_ERROR_INVALID_ARGUMENT);
    kr_view_destroy(view);
}

/* ---- font descs: past struct_size, collection_index would be 0xABABABAB, a face the font lacks */

/* pages/fonts/NotoSans-Regular.ttf in $KR_SOURCE_DIR (dev.sh check: the checkout it checks; the
 * build's karouka-src link moves to the next checkout that builds), else in the source it
 * was built from. */
static char const* font_path(void)
{
    static char path[4096];
    char const* root = getenv("KR_SOURCE_DIR");
    snprintf(path, sizeof path, "%s/pages/fonts/NotoSans-Regular.ttf", root && *root ? root : KAROUKA_SOURCE_DIR);
    return path;
}
#define FONT_PATH font_path()

static unsigned char font_bytes[1 << 20];
static size_t font_size;

/* Registers NotoSans from buffer.font_desc as `family`; whether the family then answers. */
static int family_answers(kr_system* system, char const* family)
{
    int answers = kr_system_set_default_font(system, family) == KR_OK;
    CHECK(kr_system_set_default_font(system, NULL) == KR_OK);
    return answers;
}

static void fill_font_desc(char const* family, uint32_t collection_index, size_t end, uint32_t struct_size)
{
    kr_font_desc desc = { .struct_size = struct_size, .family = family, .collection_index = collection_index };
    fill_from(&desc, end);
}

static void check_font_desc(kr_system* system)
{
    FILE* file = fopen(FONT_PATH, "rb");
    CHECK(file != NULL);
    if (!file)
        return;
    font_size = fread(font_bytes, 1, sizeof font_bytes, file);
    fclose(file);
    CHECK(font_size > 0 && font_size < sizeof font_bytes);
    size_t const index_at = offsetof(kr_font_desc, collection_index);

    /* The current header; a collection index the font lacks is refused (what reading the sentinel
     * past a short struct_size would do). */
    fill_font_desc("SizedCurrent", 0, sizeof(kr_font_desc), sizeof(kr_font_desc));
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_OK);
    CHECK(family_answers(system, "SizedCurrent"));
    fill_font_desc("SizedBadIndex", 7, sizeof(kr_font_desc), sizeof(kr_font_desc));
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(!family_answers(system, "SizedBadIndex"));

    /* A header with family only: collection_index and flags past struct_size are not read. */
    fill_font_desc("SizedOlder", 0, index_at, (uint32_t)index_at);
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_OK);
    CHECK(family_answers(system, "SizedOlder"));
    fill_font_desc("SizedOlderFile", 0, index_at, (uint32_t)index_at);
    CHECK(kr_system_add_font_file2(system, FONT_PATH, &buffer.font_desc) == KR_OK);
    CHECK(family_answers(system, "SizedOlderFile"));
    /* A size between members: none of collection_index's bytes. */
    fill_font_desc("SizedBetween", 0, index_at, (uint32_t)(index_at + 2));
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_OK);
    CHECK(family_answers(system, "SizedBetween"));

    /* A newer header's larger struct. */
    fill_font_desc("SizedNewer", 0, sizeof(kr_font_desc), (uint32_t)sizeof buffer.bytes);
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_OK);
    CHECK(family_answers(system, "SizedNewer"));

    /* No size, or too small for family: refused, nothing registered. */
    fill_font_desc("SizedRefused", 0, sizeof(kr_font_desc), 0);
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font_file2(system, FONT_PATH, &buffer.font_desc) == KR_ERROR_INVALID_ARGUMENT);
    fill_font_desc("SizedRefused", 0, sizeof(kr_font_desc), (uint32_t)index_at - 4);
    CHECK(kr_system_add_font2(system, font_bytes, font_size, &buffer.font_desc) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(!family_answers(system, "SizedRefused"));

    /* A host built before struct_size: 16 bytes of family, collection_index and flags. */
    struct {
        char const* family;
        uint32_t collection_index, flags;
    } old_desc = { "SizedLegacy", 0, KR_FONT_FLAGS_NONE };
    fill_from(&old_desc, sizeof old_desc);
    CHECK(kr_system_add_font(system, font_bytes, font_size, buffer.bytes) == KR_OK);
    CHECK(family_answers(system, "SizedLegacy"));
    old_desc.family = "SizedLegacyFile";
    fill_from(&old_desc, sizeof old_desc);
    CHECK(kr_system_add_font_file(system, FONT_PATH, buffer.bytes) == KR_OK);
    CHECK(family_answers(system, "SizedLegacyFile"));
    CHECK(kr_system_add_font(system, font_bytes, font_size, NULL) == KR_OK);
}

/* ---- input events: past struct_size, modifiers would be 0xABABABAB (shift, ctrl and super) */

/* The page titles its nth mousedown "n button:shift ctrl alt meta". */
static char const input_page[] =
    "<!doctype html><title>idle</title><body style='margin:0;height:100vh'><script>"
    "var n = 0;"
    "addEventListener('mousedown', function (e) {"
    "  document.title = (++n) + ' ' + e.button + ':' + (+e.shiftKey) + (+e.ctrlKey) + (+e.altKey) + (+e.metaKey); });"
    "</script></body>";

static int mouse_downs;

/* The last mousedown's "button:modifiers" if it was the page's nth one, the one expected next. */
static char const* mouse_down_title(kr_view* view)
{
    char prefix[16];
    snprintf(prefix, sizeof prefix, "%d ", ++mouse_downs);
    char const* title = kr_view_title(view);
    return strncmp(title, prefix, strlen(prefix)) == 0 ? title + strlen(prefix) : "missing";
}

static void load_page(kr_view* view, char const* html)
{
    CHECK(kr_view_load_html(view, html, strlen(html), "file:///karouka-struct-size-test/") == KR_OK);
    for (int frame = 0; frame < 6; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
}

static void next_frame(kr_view* view)
{
    clock_ms += 16.0;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
}

/* A secondary-button mousedown with alt, its first `end` bytes over the sentinel. What the page saw
 * of it, or "refused"; a full mouseup follows so no button stays down. */
static char const* mouse_down(kr_view* view, size_t end, uint32_t struct_size)
{
    kr_input_event event = { .struct_size = struct_size, .type = KR_INPUT_MOUSE_DOWN, .x = 10, .y = 10, .button = 1, .modifiers = KR_MOD_ALT };
    fill_from(&event, end);
    static char title[32];
    kr_result result = kr_view_inject_input2(view, &buffer.input);
    next_frame(view);
    snprintf(title, sizeof title, "%s", result == KR_OK ? mouse_down_title(view) : "refused");
    kr_input_event up = { .struct_size = sizeof(kr_input_event), .type = KR_INPUT_MOUSE_UP, .x = 10, .y = 10, .button = 1 };
    CHECK(kr_view_inject_input2(view, &up) == KR_OK);
    next_frame(view);
    return title;
}

static void check_input_event(kr_system* system)
{
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return;
    load_page(view, input_page);
    size_t const button_at = offsetof(kr_input_event, button);
    size_t const modifiers_at = offsetof(kr_input_event, modifiers);
    char const* title;

    /* The current header: the secondary button (DOM button 2), alt. */
    title = mouse_down(view, sizeof(kr_input_event), sizeof(kr_input_event));
    CHECK(strcmp(title, "2:0010") == 0);

    /* A header from before modifiers: none, not the sentinel's shift, ctrl and super. */
    title = mouse_down(view, modifiers_at, (uint32_t)modifiers_at);
    CHECK(strcmp(title, "2:0000") == 0);
    /* A size between members: none of modifiers' bytes. */
    title = mouse_down(view, modifiers_at, (uint32_t)(modifiers_at + 2));
    CHECK(strcmp(title, "2:0000") == 0);
    /* A header from before button: the primary button (DOM button 0). */
    title = mouse_down(view, button_at, (uint32_t)button_at);
    CHECK(strcmp(title, "0:0000") == 0);

    /* A newer header's larger struct. */
    title = mouse_down(view, sizeof(kr_input_event), (uint32_t)sizeof buffer.bytes);
    CHECK(strcmp(title, "2:0010") == 0);

    /* No size, or too small for type: refused. */
    CHECK(strcmp(mouse_down(view, sizeof(kr_input_event), 0), "refused") == 0);
    CHECK(strcmp(mouse_down(view, sizeof(kr_input_event), offsetof(kr_input_event, type) + 2), "refused") == 0);
    CHECK(kr_view_inject_input2(view, NULL) == KR_ERROR_INVALID_ARGUMENT);
    kr_input_event event = { .struct_size = sizeof(kr_input_event) };
    CHECK(kr_view_inject_input2(NULL, &event) == KR_ERROR_INVALID_ARGUMENT);

    /* A host built before struct_size: the 36 bytes from type to code_point. */
    struct {
        uint32_t type;
        float x, y, delta_x, delta_y;
        uint32_t button, modifiers, key_code, code_point;
    } old_event = { KR_INPUT_MOUSE_DOWN, 10, 10, 0, 0, 1, KR_MOD_ALT, 0, 0 };
    fill_from(&old_event, sizeof old_event);
    CHECK(kr_view_inject_input(view, buffer.bytes) == KR_OK);
    next_frame(view);
    CHECK(strcmp(mouse_down_title(view), "2:0010") == 0);
    old_event.type = KR_INPUT_MOUSE_UP;
    fill_from(&old_event, sizeof old_event);
    CHECK(kr_view_inject_input(view, buffer.bytes) == KR_OK);
    next_frame(view);
    CHECK(kr_view_inject_input(view, NULL) == KR_ERROR_INVALID_ARGUMENT);
    kr_view_destroy(view);
}

/* ---- engine callbacks: past struct_size, call and user_data would be garbage pointers */

/* On the nth "go" the page triggers "t" and calls "c", and titles how the call settled: "n:resolved"
 * or "n:rejected". */
static char const engine_page[] =
    "<!doctype html><title>idle</title><script>"
    "var n = 0;"
    "engine.on('go', function () {"
    "  var go = ++n;"
    "  document.title = go + ':pending';"
    "  engine.trigger('t');"
    "  engine.call('c').then(function () { document.title = go + ':resolved'; }, function () { document.title = go + ':rejected'; });"
    "});"
    "</script>";

static int go_count;

static int engine_marker;
static int triggers, calls;
static void* trigger_user_data;
static void* call_user_data;

static void on_trigger(void* user_data, kr_view* view, char const* name, char const* json_args)
{
    (void)view;
    (void)name;
    (void)json_args;
    ++triggers;
    trigger_user_data = user_data;
}

static void on_call(void* user_data, kr_view* view, char const* name, char const* json_args, uint64_t call_id)
{
    (void)name;
    (void)json_args;
    ++calls;
    call_user_data = user_data;
    CHECK(kr_view_engine_resolve(view, call_id, true, NULL) == KR_OK);
}

/* Sends "go"; how the page's call settled ("resolved", "rejected"), with the callbacks' counts and
 * user data left in the globals above. */
static char const* engine_go(kr_view* view)
{
    triggers = calls = 0;
    trigger_user_data = call_user_data = (void*)&triggers; /* neither NULL nor the marker */
    CHECK(kr_view_engine_trigger(view, "go", NULL) == KR_OK);
    char prefix[16];
    snprintf(prefix, sizeof prefix, "%d:", ++go_count);
    static char title[32];
    for (int frame = 0; frame < 30; ++frame) {
        next_frame(view);
        char const* page_title = kr_view_title(view);
        if (strncmp(page_title, prefix, strlen(prefix)) == 0) {
            snprintf(title, sizeof title, "%s", page_title + strlen(prefix));
            if (strcmp(title, "resolved") == 0 || strcmp(title, "rejected") == 0)
                return title;
        }
    }
    return "unsettled";
}

static void fill_callbacks(size_t end, uint32_t struct_size)
{
    kr_engine_callbacks callbacks = { .struct_size = struct_size, .trigger = on_trigger, .call = on_call, .user_data = &engine_marker };
    fill_from(&callbacks, end);
}

/* Both callbacks ran once with the marker as user data, and the call resolved. */
static int every_callback(char const* settled)
{
    return strcmp(settled, "resolved") == 0 && triggers == 1 && calls == 1 && trigger_user_data == &engine_marker && call_user_data == &engine_marker;
}

static void check_engine_callbacks(kr_system* system)
{
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return;
    load_page(view, engine_page);
    size_t const call_at = offsetof(kr_engine_callbacks, call);
    size_t const user_data_at = offsetof(kr_engine_callbacks, user_data);
    char const* settled;

    /* The current header. */
    fill_callbacks(sizeof(kr_engine_callbacks), sizeof(kr_engine_callbacks));
    CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_OK);
    CHECK(every_callback(engine_go(view)));

    /* A header from before user_data: both callbacks, with NULL user data. */
    fill_callbacks(user_data_at, (uint32_t)user_data_at);
    CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_OK);
    settled = engine_go(view);
    CHECK(strcmp(settled, "resolved") == 0);
    CHECK(triggers == 1 && calls == 1 && trigger_user_data == NULL && call_user_data == NULL);

    /* A header with trigger only: no call callback, so engine.call rejects. A size between
     * members gets none of call's bytes either. */
    for (int between = 0; between <= 4; between += 4) {
        fill_callbacks(call_at, (uint32_t)(call_at + between));
        CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_OK);
        settled = engine_go(view);
        CHECK(strcmp(settled, "rejected") == 0);
        CHECK(triggers == 1 && calls == 0 && trigger_user_data == NULL);
    }

    /* A newer header's larger struct. */
    fill_callbacks(sizeof(kr_engine_callbacks), (uint32_t)sizeof buffer.bytes);
    CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_OK);
    CHECK(every_callback(engine_go(view)));

    /* No size, or too small for trigger: refused, and the callbacks set before stay. */
    fill_callbacks(sizeof(kr_engine_callbacks), 0);
    CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_ERROR_INVALID_ARGUMENT);
    fill_callbacks(sizeof(kr_engine_callbacks), (uint32_t)call_at - 4);
    CHECK(kr_view_set_engine_callbacks2(view, &buffer.callbacks) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(every_callback(engine_go(view)));
    CHECK(kr_view_set_engine_callbacks2(NULL, &buffer.callbacks) == KR_ERROR_INVALID_ARGUMENT);

    /* NULL removes both. */
    CHECK(kr_view_set_engine_callbacks2(view, NULL) == KR_OK);
    settled = engine_go(view);
    CHECK(strcmp(settled, "rejected") == 0 && triggers == 0 && calls == 0);

    /* A host built before struct_size: the 24 bytes of trigger, call and user_data. */
    struct {
        void (*trigger)(void*, kr_view*, char const*, char const*);
        void (*call)(void*, kr_view*, char const*, char const*, uint64_t);
        void* user_data;
    } old_callbacks = { on_trigger, on_call, &engine_marker };
    fill_from(&old_callbacks, sizeof old_callbacks);
    kr_view_set_engine_callbacks(view, buffer.bytes);
    CHECK(every_callback(engine_go(view)));
    kr_view_set_engine_callbacks(view, NULL);
    settled = engine_go(view);
    CHECK(strcmp(settled, "rejected") == 0 && triggers == 0);
    kr_view_destroy(view);
}

/* ---- the system desc; each case's process creates its one system */

/* An older header's desc, without host, mtl_device, mtl_queue and real_time_clock: garbage there
 * would be dereferenced if read. Then the view and memory stats cases on that system. */
static int run_older(void)
{
    size_t const host_at = offsetof(kr_system_desc, host);
    kr_system* system = NULL;

    /* Refused before anything process-wide starts: no size, too small for resource_root, between
     * struct_size and the end of resource_root. */
    fill_system_desc(host_at, 0, 32 * MiB);
    CHECK(kr_system_create2(&buffer.system_desc, &system) == KR_ERROR_INVALID_ARGUMENT);
    fill_system_desc(host_at, offsetof(kr_system_desc, resource_root) + 4, 32 * MiB);
    CHECK(kr_system_create2(&buffer.system_desc, &system) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(system == NULL);

    fill_system_desc(host_at, (uint32_t)host_at, 32 * MiB);
    if (kr_system_create2(&buffer.system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_struct_size_test: engine failed to start from an older header's desc\n");
        return 1;
    }
    CHECK(gpu_cache_budget(system) == 32 * MiB);
    /* karouka copied the desc: nothing of it was written. */
    CHECK(buffer.system_desc.struct_size == host_at);
    CHECK(untouched_from(host_at));

    check_view_desc(system);
    check_memory_stats(system);
    check_font_desc(system);
    check_input_event(system);
    check_engine_callbacks(system);

    /* kr_metal_output's checks of struct_size come before the host queue's ("metal" has one). */
    kr_view* view = NULL;
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    fill_sentinel();
    buffer.output.struct_size = 0;
    CHECK(kr_view_set_output2(view, &buffer.output) == KR_ERROR_INVALID_ARGUMENT);
    buffer.output.struct_size = sizeof(kr_metal_output);
    CHECK(kr_view_set_output2(view, &buffer.output) == KR_ERROR_INVALID_STATE);
    kr_view_destroy(view);

    kr_system_destroy(system);
    return 0;
}

/* A newer header's desc: struct_size past every member this library knows, garbage after them. */
static int run_newer(void)
{
    fill_system_desc(sizeof(kr_system_desc), (uint32_t)sizeof buffer.bytes, 40 * MiB);
    kr_system* system = NULL;

    /* A host interface whose struct_size can't hold io is refused, like the other
     * versioned structs, before anything process-wide starts (the create below still runs): no
     * size, and one ending inside io (the whole interface in place, so it isn't refused for
     * garbage). */
    size_t const io_end = offsetof(kr_host_interface, io) + sizeof(kr_host_io);
    buffer.system_desc.host = fill_host_interface(sizeof(kr_host_interface), 0);
    CHECK(kr_system_create2(&buffer.system_desc, &system) == KR_ERROR_INVALID_ARGUMENT);
    buffer.system_desc.host = fill_host_interface(sizeof(kr_host_interface), (uint32_t)(io_end - 1));
    CHECK(kr_system_create2(&buffer.system_desc, &system) == KR_ERROR_INVALID_ARGUMENT);
    buffer.system_desc.host = fill_host_interface(sizeof(kr_host_interface), (uint32_t)offsetof(kr_host_interface, io));
    CHECK(kr_system_create2(&buffer.system_desc, &system) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(system == NULL);

    /* A host interface whose struct_size ends inside log, after its write: log is not whole, so
     * none of it is read (write included; min_level and user_data would be the sentinel). */
    size_t const log_write_end = offsetof(kr_host_interface, log) + sizeof(void*);
    buffer.system_desc.host = fill_host_interface(log_write_end, (uint32_t)log_write_end);
    if (kr_system_create2(&buffer.system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_struct_size_test: engine failed to start from a newer header's desc\n");
        return 1;
    }
    CHECK(gpu_cache_budget(system) == 40 * MiB);
    CHECK(page_console_lines(system) == 0);
    kr_system_destroy(system);
    return 0;
}

/* ---- kr_metal_output, on a system with the host's Metal device and queue. Past struct_size,
 * texture would be the sentinel; a struct_size short of it leaves none, which is refused. */

void* MTLCreateSystemDefaultDevice(void);
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);

static void* send(void* object, char const* selector)
{
    return ((void* (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static void send_void(void* object, char const* selector)
{
    ((void (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static int run_metal(void)
{
    void* pool = objc_autoreleasePoolPush();
    void* device = MTLCreateSystemDefaultDevice();
    void* queue = device ? send(device, "newCommandQueue") : NULL;
    if (!queue) {
        fprintf(stderr, "karouka_struct_size_test: no Metal device\n");
        return 1;
    }
    /* With a newer header's host interface, larger than ours, whose log karouka uses. */
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .mtl_device = device, .mtl_queue = queue };
    system_desc.host = fill_host_interface(sizeof(kr_host_interface), (uint32_t)sizeof host_buffer.bytes);
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_struct_size_test: engine failed to start on the host's queue\n");
        return 1;
    }
    /* A 64x64 BGRA8Unorm (80) render target (4) the view may also read (1). */
    void* descriptor = ((void* (*)(void*, SEL, unsigned long, unsigned long, unsigned long, bool))objc_msgSend)(
        (void*)objc_getClass("MTLTextureDescriptor"), sel_registerName("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, 64, 64, false);
    ((void (*)(void*, SEL, unsigned long))objc_msgSend)(descriptor, sel_registerName("setUsage:"), 4 | 1);
    void* texture = ((void* (*)(void*, SEL, void*))objc_msgSend)(device, sel_registerName("newTextureWithDescriptor:"), descriptor);
    void* commands = send(queue, "commandBuffer");
    CHECK(texture && commands);

    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return 1;
    CHECK(strcmp(page_scale(view), "1") == 0);
    CHECK(kr_view_iosurface(view) != NULL);
    CHECK(page_console_lines(system) == 1);
    size_t const texture_at = offsetof(kr_metal_output, texture);
    kr_metal_output output = { .struct_size = sizeof(kr_metal_output), .command_buffer = commands, .texture = texture };

    /* A header from before texture, with the texture in the bytes past it: not read, so refused.
     * Likewise a size between members, no size, and one too small for command_buffer. */
    uint32_t const refused_sizes[] = { (uint32_t)texture_at, (uint32_t)texture_at + 4, 0, 4 };
    for (size_t i = 0; i < sizeof refused_sizes / sizeof refused_sizes[0]; ++i) {
        output.struct_size = refused_sizes[i];
        fill_from(&output, sizeof output);
        CHECK(kr_view_set_output2(view, &buffer.output) == KR_ERROR_INVALID_ARGUMENT);
    }

    /* The current header, a newer header's larger struct, and a host built before struct_size
     * (the 16 bytes of command_buffer and texture): each names the target; the last one set is
     * what the render draws into, so the view has no IOSurface after it. */
    output.struct_size = sizeof(kr_metal_output);
    fill_from(&output, sizeof output);
    CHECK(kr_view_set_output2(view, &buffer.output) == KR_OK);
    CHECK(kr_view_set_output2(view, NULL) == KR_OK);
    output.struct_size = sizeof buffer.bytes;
    fill_from(&output, sizeof output);
    CHECK(kr_view_set_output2(view, &buffer.output) == KR_OK);
    void* old_output[2] = { commands, texture };
    fill_from(old_output, sizeof old_output);
    CHECK(kr_view_set_output(view, buffer.bytes) == KR_OK);
    clock_ms += 16.0;
    bool rendered = false;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    CHECK(kr_view_render(view, &rendered) == KR_OK);
    CHECK(rendered);
    CHECK(kr_view_iosurface(view) == NULL);
    send_void(commands, "commit");
    send_void(commands, "waitUntilCompleted");
    CHECK(kr_view_set_output(view, NULL) == KR_OK);

    kr_view_destroy(view);
    kr_system_destroy(system);
    objc_autoreleasePoolPop(pool);
    return 0;
}

/* A host built before struct_size: its first header's 16 bytes (resource_root, verbose) and garbage
 * after them, where the later headers had gpu_cache_bytes, host and the Metal objects. */
static int run_legacy(void)
{
    fill_sentinel();
    struct {
        char const* resource_root;
        bool verbose;
    } old_desc = { NULL, false };
    memcpy(buffer.bytes, &old_desc, sizeof old_desc);
    kr_system* system = NULL;
    if (kr_system_create(buffer.bytes, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_struct_size_test: engine failed to start from the legacy entry point\n");
        return 1;
    }
    CHECK(gpu_cache_budget(system) == 64 * MiB); /* the default: gpu_cache_bytes was not read */
    kr_system_destroy(system);
    return 0;
}

static int run_child(char const* mode)
{
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return 1;
    char mode_arg[16];
    snprintf(mode_arg, sizeof mode_arg, "%s", mode);
    char* child_argv[] = { path, mode_arg, NULL };
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, child_argv, environ) != 0) {
        fprintf(stderr, "karouka_struct_size_test: can't run the %s case\n", mode);
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "karouka_struct_size_test: the %s case failed (status %d)\n", mode, status);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    int result;
    if (argc > 1 && strcmp(argv[1], "newer") == 0)
        result = run_newer();
    else if (argc > 1 && strcmp(argv[1], "legacy") == 0)
        result = run_legacy();
    else if (argc > 1 && strcmp(argv[1], "metal") == 0)
        result = run_metal();
    else
    {
        result = run_older();
        result |= run_child("newer");
        result |= run_child("legacy");
        result |= run_child("metal");
    }
    if (failures)
        fprintf(stderr, "karouka_struct_size_test%s%s: %d check(s) failed\n", argc > 1 ? " " : "", argc > 1 ? argv[1] : "", failures);
    return result || failures ? 1 : 0;
}
