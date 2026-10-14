/* No public entry point leaves autoreleased objects in the host's pool. A C host's
 * frame loop drains no autorelease pool, so whatever a call autoreleases into the caller's pool stays
 * until the host's outermost pool goes, often never: kr_view_render's Metal command buffers leaked
 * that way, ~2 KB a frame. Each call that can reach Metal, Skia's GPU context, CoreText or
 * Cocoa runs under its own pool instead; this test counts what each one leaves in a pool of the
 * test's.
 *
 * The count is the distance between two objc_autoreleasePoolPush tokens around the call: a token
 * is the address of the pool boundary it pushed, and each object autoreleased between them takes
 * one slot of the same pool page (an object autoreleased again right after shares its slot, so the
 * count can be low, never zero). A calibration first holds the method to an autorelease of one
 * object and to none; on a runtime where tokens stop meaning that, it fails here instead of
 * passing silently.
 *
 * Without their pools kr_view_capture_png (Skia's readback: a command buffer, a blit encoder, a
 * buffer), kr_view_resize and kr_view_destroy (freeing the IOSurface's Metal texture autoreleases
 * the device), kr_system_destroy (the device again), kr_system_add_font2 and
 * kr_system_add_font_file2 (CoreText's font registry: an XPC proxy, URLs, a locale; 41 objects for
 * the first face, routed or not) and kr_system_set_default_font (a locale) each leave objects; a
 * failure prints the thread's pools, the objects named, so the call that left them can be found.
 *
 * Two processes, because the host io is fixed when the system is created (one system per process):
 *
 * The parent runs on the host's device and queue (kr_view_set_output2 needs them): kr_system_create2,
 * kr_view_create2, a load, advance_time and a render into the IOSurface, kr_view_lock_pixels and
 * unlock, kr_view_capture_png, kr_view_resize, kr_view_set_output2 (a refused output, an accepted
 * one, NULL), a render into the host's texture, kr_view_inject_input2, the stats queries; then the
 * host's fonts (kr_system_add_font2, plain and KR_FONT_FALLBACK, kr_system_add_font_file2 from a
 * mapping of the file, kr_system_set_default_font and back), the binding calls, the engine channel
 * (kr_view_set_engine_callbacks2, the page's engine.trigger and engine.call reaching the callbacks
 * inside advance_time, kr_view_engine_trigger, kr_view_engine_resolve), kr_view_load_file of a page
 * with a stylesheet and kr_view_reload of it, kr_view_get_dependencies, kr_view_load_html with a
 * file: base and its reload, every frame of those probed; then kr_view_destroy holding an IOSurface
 * and kr_system_destroy.
 *
 * The child (argv: --host-io DIR) routes every file: load through a kr_host_io of the test's, on a
 * device karouka makes: kr_system_add_font_file2 completed inside the request and later,
 * kr_view_load_file, the stylesheet it requests and kr_view_reload, each completed later by a
 * probed kr_io_complete (copied and borrowed), then kr_view_load_file completed inside the request,
 * and kr_io_complete refusing an id that is not pending.
 *
 * Built as karouka_autorelease_test (C, public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

void* MTLCreateSystemDefaultDevice(void);
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
void _objc_autoreleasePoolPrint(void); /* libobjc's debugging dump, to stderr */

static int failures;
static bool quiet; /* the calibration autoreleases on purpose: no pool dump */

static void* send(void* object, char const* selector)
{
    return ((void* (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static void send_ulong(void* object, char const* selector, unsigned long value)
{
    ((void (*)(void*, SEL, unsigned long))objc_msgSend)(object, sel_registerName(selector), value);
}

/* PROBE(name, statement): runs the statement between two pool tokens and fails the test if it left
 * any object in the pool, after printing the thread's pools (the objects it left are the last ones
 * above the last two boundaries). */
static void* probe_begin(void)
{
    return objc_autoreleasePoolPush();
}

static long probe_end(void* before)
{
    void* after = objc_autoreleasePoolPush();
    long count = ((char*)after - (char*)before) / (long)sizeof(void*) - 1;
    if (count != 0 && !quiet)
        _objc_autoreleasePoolPrint();
    objc_autoreleasePoolPop(after);
    objc_autoreleasePoolPop(before);
    return count;
}

#define PROBE(name, statement)                                                                                     \
    do {                                                                                                           \
        void* probe_before = probe_begin();                                                                        \
        statement;                                                                                                 \
        long probe_count = probe_end(probe_before);                                                                \
        if (probe_count != 0) {                                                                                    \
            fprintf(stderr, "karouka_autorelease_test: %s left %ld object(s) in the caller's autorelease pool\n", \
                name, probe_count);                                                                                \
            ++failures;                                                                                            \
        }                                                                                                          \
    } while (0)

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static bool calibrate(void)
{
    quiet = true;
    void* before = probe_begin();
    long none = probe_end(before);
    before = probe_begin();
    send(send(send((void*)objc_getClass("NSObject"), "alloc"), "init"), "autorelease");
    long one = probe_end(before);
    if (none != 0 || one != 1) {
        fprintf(stderr, "karouka_autorelease_test: pool tokens count %ld for no autorelease and %ld for one, want 0 and 1: this runtime's tokens don't count slots\n", none, one);
        return false;
    }
    quiet = false;
    return true;
}

static void* make_target(void* device)
{
    void* descriptor = send((void*)objc_getClass("MTLTextureDescriptor"), "new");
    send_ulong(descriptor, "setPixelFormat:", 80); /* BGRA8Unorm */
    send_ulong(descriptor, "setWidth:", 64);
    send_ulong(descriptor, "setHeight:", 64);
    send_ulong(descriptor, "setUsage:", 4 | 1); /* RenderTarget | ShaderRead */
    send_ulong(descriptor, "setStorageMode:", 2); /* Private */
    void* texture = ((void* (*)(void*, SEL, void*))objc_msgSend)(device, sel_registerName("newTextureWithDescriptor:"), descriptor);
    send(descriptor, "release");
    return texture;
}

/* A host command buffer, owned by the caller (commandBuffer's is autoreleased, which would count). */
static void* new_command_buffer(void* queue)
{
    void* pool = objc_autoreleasePoolPush();
    void* command_buffer = send(send(queue, "commandBuffer"), "retain");
    objc_autoreleasePoolPop(pool);
    return command_buffer;
}

static void finish(void* command_buffer)
{
    send(command_buffer, "commit");
    send(command_buffer, "waitUntilCompleted");
    send(command_buffer, "release");
}

/* ---- Pages, fonts and the engine channel, shared by both runs. */

/* pages/fonts/NAME in $KR_SOURCE_DIR (dev.sh check: the checkout it checks), else in the
 * source it was built from. Two results live at once. */
static char const* font_file(char const* name)
{
    static char paths[2][4096];
    static unsigned next;
    char const* root = getenv("KR_SOURCE_DIR");
    char* path = paths[next++ % 2];
    snprintf(path, sizeof paths[0], "%s/pages/fonts/%s", root && *root ? root : KAROUKA_SOURCE_DIR, name);
    return path;
}

static void* read_file(char const* path, size_t* out_size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = size >= 0 ? malloc(size ? (size_t)size : 1) : NULL;
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *out_size = data ? (size_t)size : 0;
    return data;
}

static bool write_file(char const* dir, char const* name, char const* text)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* file = fopen(path, "wb");
    if (!file)
        return false;
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && ok;
}

/* The page uses the host's fonts (through its stylesheet), the binding model and both directions of
 * the engine channel; its title reads "<last ping>|<call's answer>". */
static char const page_html[] =
    "<!doctype html><title>loading</title><link rel=stylesheet href=style.css><body>"
    "<p data-bind-text=\"'hp ' + player.health\">hp</p><p data-bind-text=\"player.name\">name</p>"
    "<p class=deva>\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87</p>"
    "<p>\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7</p>"
    "<script>"
    "var got = 'none', answer = 'none';"
    "function show() { document.title = got + '|' + answer; }"
    "engine.on('ping', function (a) { got = 'ping' + a; show(); });"
    "engine.call('ask', 1).then(function (v) { answer = v; show(); });"
    "engine.trigger('hello', 2);"
    "show();"
    "</script></body>";
static char const style_css[] = "body { font-family: HostSans; background: teal } .deva { font-family: HostDeva }";

static int triggers;
static int calls;
static uint64_t last_call;

static void on_trigger(void* user_data, kr_view* view, char const* name, char const* json_args)
{
    (void)user_data, (void)view, (void)name, (void)json_args;
    ++triggers;
}

static void on_call(void* user_data, kr_view* view, char const* name, char const* json_args, uint64_t call_id)
{
    (void)user_data, (void)view, (void)name, (void)json_args;
    ++calls;
    last_call = call_id;
}

/* The host's queue when the system runs on it: each frame then goes in a host command buffer,
 * committed and waited for outside the probes. */
static void* frame_queue;
static void (*between_frames)(void); /* the child's: completes the loads the frames requested */

static void probed_frame(kr_view* view, double* clock_ms, char const* label)
{
    char name[128];
    void* commands = frame_queue ? new_command_buffer(frame_queue) : NULL;
    *clock_ms += 16;
    bool rendered = false;
    snprintf(name, sizeof name, "kr_view_advance_time (%s)", label);
    PROBE(name, CHECK(kr_view_advance_time(view, *clock_ms) == KR_OK));
    snprintf(name, sizeof name, "kr_view_render (%s)", label);
    PROBE(name, CHECK(kr_view_render(view, &rendered) == KR_OK));
    if (commands)
        finish(commands);
}

/* Probed frames until the title reads `expected` (at most 12), then two more. */
static void settle(kr_view* view, double* clock_ms, char const* expected, char const* label)
{
    int extra = 2;
    for (int frame = 0; frame < 14 && extra > 0; ++frame) {
        if (between_frames)
            between_frames();
        probed_frame(view, clock_ms, label);
        char const* title = NULL;
        PROBE("kr_view_title", title = kr_view_title(view));
        if (strcmp(title, expected) == 0)
            --extra;
    }
    if (strcmp(kr_view_title(view), expected) != 0) {
        fprintf(stderr, "karouka_autorelease_test: %s: title is '%s', want '%s'\n", label, kr_view_title(view), expected);
        ++failures;
    }
}

static void set_up_view(kr_view* view)
{
    kr_engine_callbacks callbacks = { .struct_size = sizeof(kr_engine_callbacks), .trigger = on_trigger, .call = on_call };
    PROBE("kr_view_set_engine_callbacks2", CHECK(kr_view_set_engine_callbacks2(view, &callbacks) == KR_OK));
    kr_binding_path health = 0, name = 0, dead = 0;
    PROBE("kr_binding_path_resolve", CHECK(kr_binding_path_resolve(view, "player.health", &health) == KR_OK));
    CHECK(kr_binding_path_resolve(view, "player.name", &name) == KR_OK);
    CHECK(kr_binding_path_resolve(view, "player.dead", &dead) == KR_OK);
    PROBE("kr_binding_set_number", CHECK(kr_binding_set_number(view, health, 82.5) == KR_OK));
    PROBE("kr_binding_set_string", CHECK(kr_binding_set_string(view, name, "ada", 3) == KR_OK));
    PROBE("kr_binding_set_bool", CHECK(kr_binding_set_bool(view, dead, false) == KR_OK));
}

/* After a load of page_html settled: the page's trigger and call reached the callbacks; the host
 * pings and answers; the page sees both. */
static void exercise_engine(kr_view* view, double* clock_ms, char const* label)
{
    CHECK(triggers > 0 && calls > 0);
    int calls_before = calls;
    PROBE("kr_view_engine_trigger", CHECK(kr_view_engine_trigger(view, "ping", "[1]") == KR_OK));
    PROBE("kr_view_engine_resolve", CHECK(kr_view_engine_resolve(view, last_call, true, "\"yes\"") == KR_OK));
    settle(view, clock_ms, "ping1|yes", label);
    CHECK(calls == calls_before);
}

/* ---- The parent: the host's device and queue, file: loads read by karouka. */

static int run_host_device(char const* dir)
{
    void* pool = objc_autoreleasePoolPush();
    if (!calibrate())
        return 1;
    void* device = MTLCreateSystemDefaultDevice();
    void* queue = device ? send(device, "newCommandQueue") : NULL;
    void* other_queue = device ? send(device, "newCommandQueue") : NULL;
    if (!queue || !other_queue) {
        fprintf(stderr, "karouka_autorelease_test: no Metal device\n");
        return 1;
    }
    void* target = make_target(device);
    if (!target) {
        fprintf(stderr, "karouka_autorelease_test: Metal made no target texture\n");
        return 1;
    }

    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .mtl_device = device, .mtl_queue = queue };
    kr_system* system = NULL;
    kr_result result = KR_ERROR_INTERNAL;
    PROBE("kr_system_create2", result = kr_system_create2(&system_desc, &system));
    if (result != KR_OK || !system) {
        fprintf(stderr, "karouka_autorelease_test: engine failed to start on the host's queue\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    PROBE("kr_view_create2", CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK));
    if (!view)
        return 1;
    static char const page[] = "<body style=\"background: teal\">x";
    PROBE("kr_view_load_html", CHECK(kr_view_load_html(view, page, sizeof page - 1, "about:blank") == KR_OK));

    double clock_ms = 0;
    bool rendered = false;
    void* commands = new_command_buffer(queue);
    clock_ms += 16;
    PROBE("kr_view_advance_time", CHECK(kr_view_advance_time(view, clock_ms) == KR_OK));
    PROBE("kr_view_render (IOSurface)", CHECK(kr_view_render(view, &rendered) == KR_OK));
    CHECK(rendered);
    finish(commands);

    void const* pixels = NULL;
    size_t bytes_per_row = 0;
    PROBE("kr_view_lock_pixels", CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_OK));
    CHECK(pixels && bytes_per_row >= 64 * 4);
    PROBE("kr_view_unlock_pixels", CHECK(kr_view_unlock_pixels(view) == KR_OK));

    char png_path[] = "/tmp/karouka_autorelease_test.XXXXXX";
    int fd = mkstemp(png_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
        PROBE("kr_view_capture_png", CHECK(kr_view_capture_png(view, png_path) == KR_OK));
        unlink(png_path);
    }

    PROBE("kr_view_resize", CHECK(kr_view_resize(view, 64, 48, 1) == KR_OK));
    PROBE("kr_view_resize (back)", CHECK(kr_view_resize(view, 64, 64, 1) == KR_OK));

    commands = new_command_buffer(queue);
    void* foreign = new_command_buffer(other_queue);
    kr_metal_output output = { .struct_size = sizeof(kr_metal_output), .command_buffer = commands, .texture = target };
    kr_metal_output refused = { .struct_size = sizeof(kr_metal_output), .command_buffer = foreign, .texture = target };
    PROBE("kr_view_set_output2 (refused)", CHECK(kr_view_set_output2(view, &refused) == KR_ERROR_INVALID_ARGUMENT));
    PROBE("kr_view_set_output2", CHECK(kr_view_set_output2(view, &output) == KR_OK));
    PROBE("kr_view_set_output2 (NULL)", CHECK(kr_view_set_output2(view, NULL) == KR_OK));
    PROBE("kr_view_set_output2 (again)", CHECK(kr_view_set_output2(view, &output) == KR_OK));
    clock_ms += 16;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    PROBE("kr_view_render (host texture)", CHECK(kr_view_render(view, &rendered) == KR_OK));
    CHECK(rendered && !kr_view_iosurface(view));
    finish(commands);
    send(foreign, "release");

    /* Back to the IOSurface, so the view goes with an IOSurface-backed surface; and input. */
    commands = new_command_buffer(queue);
    CHECK(kr_view_set_output2(view, NULL) == KR_OK);
    kr_input_event move = { .struct_size = sizeof(kr_input_event), .type = KR_INPUT_MOUSE_MOVE, .x = 10, .y = 10 };
    PROBE("kr_view_inject_input2", CHECK(kr_view_inject_input2(view, &move) == KR_OK));
    clock_ms += 16;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    CHECK(kr_view_render(view, &rendered) == KR_OK);
    CHECK(rendered && kr_view_iosurface(view));
    finish(commands);
    kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
    PROBE("kr_view_query_stats", CHECK(kr_view_query_stats(view, &stats) == KR_OK));
    kr_memory_stats memory = { .struct_size = sizeof(kr_memory_stats) };
    PROBE("kr_system_query_memory_stats", CHECK(kr_system_query_memory_stats(system, &memory) == KR_OK));


    /* The host's fonts, on a page that is loaded. */
    frame_queue = queue;
    size_t sans_size = 0, arabic_size = 0;
    void* sans = read_file(font_file("NotoSans-Regular.ttf"), &sans_size);
    void* arabic = read_file(font_file("NotoSansArabic-Regular.ttf"), &arabic_size);
    CHECK(sans && arabic);
    kr_font_desc sans_desc = { .struct_size = sizeof(kr_font_desc), .family = "HostSans" };
    kr_font_desc arabic_desc = { .struct_size = sizeof(kr_font_desc), .family = "HostArabic", .flags = KR_FONT_FALLBACK };
    kr_font_desc deva_desc = { .struct_size = sizeof(kr_font_desc), .family = "HostDeva" };
    if (sans)
        PROBE("kr_system_add_font2", CHECK(kr_system_add_font2(system, sans, sans_size, &sans_desc) == KR_OK));
    if (arabic)
        PROBE("kr_system_add_font2 (fallback)", CHECK(kr_system_add_font2(system, arabic, arabic_size, &arabic_desc) == KR_OK));
    PROBE("kr_system_add_font2 (not a font)", CHECK(kr_system_add_font2(system, page_html, sizeof page_html - 1, NULL) == KR_ERROR_INVALID_ARGUMENT));
    free(sans);
    free(arabic);
    PROBE("kr_system_add_font_file2", CHECK(kr_system_add_font_file2(system, font_file("NotoSansDevanagari-Regular.ttf"), &deva_desc) == KR_OK));
    PROBE("kr_system_set_default_font", CHECK(kr_system_set_default_font(system, "HostSans") == KR_OK));
    probed_frame(view, &clock_ms, "fonts registered");
    PROBE("kr_system_set_default_font (NULL)", CHECK(kr_system_set_default_font(system, NULL) == KR_OK));
    probed_frame(view, &clock_ms, "default font cleared");

    /* A page from disk, its reload, and markup with a file: base and its reload. */
    set_up_view(view);
    char page_path[4096], base_url[4200];
    snprintf(page_path, sizeof page_path, "%s/page.html", dir);
    snprintf(base_url, sizeof base_url, "file://%s/", dir);
    PROBE("kr_view_load_file", CHECK(kr_view_load_file(view, page_path) == KR_OK));
    settle(view, &clock_ms, "none|none", "load_file");
    kr_view_dependencies dependencies = { .struct_size = sizeof(kr_view_dependencies) };
    PROBE("kr_view_get_dependencies", CHECK(kr_view_get_dependencies(view, &dependencies) == KR_OK));
    CHECK(dependencies.count == 2); /* page.html, style.css */
    exercise_engine(view, &clock_ms, "load_file, engine");
    PROBE("kr_view_reload (file)", CHECK(kr_view_reload(view) == KR_OK));
    settle(view, &clock_ms, "none|none", "reload (file)");
    exercise_engine(view, &clock_ms, "reload (file), engine");
    PROBE("kr_view_load_html (file: base)", CHECK(kr_view_load_html(view, page_html, sizeof page_html - 1, base_url) == KR_OK));
    settle(view, &clock_ms, "none|none", "load_html");
    PROBE("kr_view_reload (markup)", CHECK(kr_view_reload(view) == KR_OK));
    settle(view, &clock_ms, "none|none", "reload (markup)");
    exercise_engine(view, &clock_ms, "reload (markup), engine");
    frame_queue = NULL;

    PROBE("kr_view_destroy", kr_view_destroy(view));
    PROBE("kr_system_destroy", kr_system_destroy(system));
    send(target, "release");
    send(other_queue, "release");
    objc_autoreleasePoolPop(pool);
    return failures ? 1 : 0;
}

/* ---- The child: every file: load through the test's kr_host_io. */

enum { MAX_PENDING = 16 };
static kr_system* io_system;
static bool complete_inline;
static uint64_t pending_ids[MAX_PENDING];
static char pending_paths[MAX_PENDING][4096];
static int pending_count;
static int completions;
static int releases;

/* An absolute file: URL's path, percent-decoded. */
static bool path_of(char const* url, char* out, size_t size)
{
    if (strncmp(url, "file://", 7) != 0)
        return false;
    size_t length = 0;
    for (char const* c = url + 7; *c; ++c) {
        if (length + 1 >= size)
            return false;
        unsigned value;
        if (*c == '%' && c[1] && c[2] && sscanf(c + 1, "%2x", &value) == 1) {
            out[length++] = (char)value;
            c += 2;
        } else {
            out[length++] = *c;
        }
    }
    out[length] = 0;
    return true;
}

/* Completes one load from disk: copied when `borrow` is false, else borrowed until release. */
static kr_result complete_from_disk(uint64_t id, char const* path, bool borrow)
{
    size_t size = 0;
    void* data = read_file(path, &size);
    kr_result result = kr_io_complete(io_system, id, data ? KR_OK : KR_ERROR_IO, data, size,
        borrow && data ? KR_IO_OWNERSHIP_BORROW_UNTIL_RELEASE : KR_IO_OWNERSHIP_COPY);
    if (!(borrow && data))
        free(data);
    ++completions;
    return result;
}

static kr_result io_request(void* user_data, char const* url, uint64_t request_id)
{
    (void)user_data;
    char path[4096];
    if (!path_of(url, path, sizeof path))
        return KR_ERROR_IO;
    if (complete_inline) {
        CHECK(complete_from_disk(request_id, path, false) == KR_OK);
        return KR_OK;
    }
    if (pending_count == MAX_PENDING)
        return KR_ERROR_LIMIT_EXCEEDED;
    pending_ids[pending_count] = request_id;
    snprintf(pending_paths[pending_count], sizeof pending_paths[0], "%s", path);
    ++pending_count;
    return KR_OK;
}

static void io_release(void* user_data, uint64_t request_id, void const* data)
{
    (void)user_data, (void)request_id;
    free((void*)(uintptr_t)data); /* the buffer complete_from_disk lent */
    ++releases;
}

/* Completes every pending load, each under its own probe, alternately borrowed and copied. */
static void serve_pending(void)
{
    while (pending_count > 0) {
        uint64_t id = pending_ids[0];
        char path[4096];
        snprintf(path, sizeof path, "%s", pending_paths[0]);
        --pending_count;
        memmove(pending_ids, pending_ids + 1, sizeof pending_ids[0] * (size_t)pending_count);
        memmove(pending_paths, pending_paths + 1, sizeof pending_paths[0] * (size_t)pending_count);
        bool borrow = completions % 2 == 1;
        PROBE(borrow ? "kr_io_complete (borrowed)" : "kr_io_complete (copied)", CHECK(complete_from_disk(id, path, borrow) == KR_OK));
    }
}

static int run_host_io(char const* dir)
{
    void* pool = objc_autoreleasePoolPush();
    if (!calibrate())
        return 1;
    kr_host_interface host = { .struct_size = sizeof(kr_host_interface), .io = { .request = io_request, .release = io_release } };
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .host = &host };
    kr_result result = KR_ERROR_INTERNAL;
    PROBE("kr_system_create2 (host io)", result = kr_system_create2(&system_desc, &io_system));
    if (result != KR_OK || !io_system) {
        fprintf(stderr, "karouka_autorelease_test: engine failed to start with a host io\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    PROBE("kr_view_create2 (host io)", CHECK(kr_view_create2(io_system, &view_desc, &view) == KR_OK));
    if (!view)
        return 1;
    set_up_view(view);
    between_frames = serve_pending;
    double clock_ms = 0;

    kr_font_desc sans_desc = { .struct_size = sizeof(kr_font_desc), .family = "HostSans" };
    kr_font_desc deva_desc = { .struct_size = sizeof(kr_font_desc), .family = "HostDeva" };
    complete_inline = true;
    PROBE("kr_system_add_font_file2 (completed in the request)", CHECK(kr_system_add_font_file2(io_system, font_file("NotoSans-Regular.ttf"), &sans_desc) == KR_OK));
    complete_inline = false;
    PROBE("kr_system_add_font_file2 (completed later)", CHECK(kr_system_add_font_file2(io_system, font_file("NotoSansDevanagari-Regular.ttf"), &deva_desc) == KR_OK));
    CHECK(pending_count == 1);
    serve_pending();
    probed_frame(view, &clock_ms, "font registered later");

    char page_path[4096];
    snprintf(page_path, sizeof page_path, "%s/page.html", dir);
    PROBE("kr_view_load_file (host io)", CHECK(kr_view_load_file(view, page_path) == KR_OK));
    CHECK(pending_count == 1);
    settle(view, &clock_ms, "none|none", "load_file (host io)");
    exercise_engine(view, &clock_ms, "load_file (host io), engine");
    PROBE("kr_view_reload (host io)", CHECK(kr_view_reload(view) == KR_OK));
    settle(view, &clock_ms, "none|none", "reload (host io)");
    exercise_engine(view, &clock_ms, "reload (host io), engine");
    complete_inline = true;
    PROBE("kr_view_load_file (completed in the request)", CHECK(kr_view_load_file(view, page_path) == KR_OK));
    settle(view, &clock_ms, "none|none", "load_file (completed in the request)");
    PROBE("kr_io_complete (not pending)", CHECK(kr_io_complete(io_system, 1u << 30, KR_OK, NULL, 0, KR_IO_OWNERSHIP_COPY) == KR_ERROR_INVALID_ARGUMENT));
    CHECK(releases > 0 && pending_count == 0);
    between_frames = NULL;

    PROBE("kr_view_destroy (host io)", kr_view_destroy(view));
    PROBE("kr_system_destroy (host io)", kr_system_destroy(io_system));
    objc_autoreleasePoolPop(pool);
    return failures ? 1 : 0;
}

static int run_child(char const* dir)
{
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return 1;
    char mode_arg[] = "--host-io";
    char dir_arg[4096];
    snprintf(dir_arg, sizeof dir_arg, "%s", dir);
    char* child_argv[] = { path, mode_arg, dir_arg, NULL };
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, child_argv, environ) != 0) {
        fprintf(stderr, "karouka_autorelease_test: can't run the host io case\n");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        fprintf(stderr, "karouka_autorelease_test: the host io case crashed (status %d)\n", status);
        return 1;
    }
    return WEXITSTATUS(status);
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--host-io") == 0) {
        int status = run_host_io(argv[2]);
        if (failures)
            fprintf(stderr, "karouka_autorelease_test --host-io: %d check(s) failed\n", failures);
        return status;
    }

    char dir[] = "/tmp/karouka_autorelease_test.XXXXXX";
    if (!mkdtemp(dir) || !write_file(dir, "page.html", page_html) || !write_file(dir, "style.css", style_css)) {
        fprintf(stderr, "karouka_autorelease_test: can't write the test page\n");
        return 1;
    }
    int status = run_host_device(dir);
    int child_status = run_child(dir);
    char path[4096];
    snprintf(path, sizeof path, "%s/page.html", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/style.css", dir);
    unlink(path);
    rmdir(dir);
    if (failures)
        fprintf(stderr, "karouka_autorelease_test: %d check(s) failed\n", failures);
    if (status || child_status)
        return 1;
    printf("karouka_autorelease_test: no call left objects in the caller's pool\n");
    return 0;
}
