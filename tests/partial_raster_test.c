/* Partial raster: a frame that changes part of the viewport replays its display list into
 * that part's damage alone and keeps the rest of the last frame. Checked through the public ABI, on
 * the Metal backend and on the software one (a second process: one system per process): after each
 * change a view that rendered every frame since its load (so each change was rasterized into its
 * damage, kr_view_stats.last_raster_pixels below the frame's area) must hold the pixels of a fresh
 * view that made the same changes and rendered once, whole: exactly on Metal; on the software
 * backend within software_tolerance per channel, as Skia's CPU rasterizer covers an anti-aliased
 * edge a little differently when the damage's clip cuts through its shape (a rotated box, a rounded
 * corner: up to 8 of 255 here), while the GPU's coverage doesn't depend on the clip. The changes cover what the damage
 * must enclose: a color, a box moved away from where it was, text, a shadow outside its box, an
 * opacity group, a rotated box, and a box shrunk to nothing. Two boxes in opposite corners that change
 * together must rasterize less than a quarter of the frame (the damage is several rectangles,
 * not their union), and a nested scroller scrolled by script only its own content. Last, a box whose
 * background color a compositor animation moves (no restyle, no new display list) must take each
 * sampled color under partial raster: two views loaded and stepped on the same clock, one rendered
 * every frame, the other once at the end (the damage diff once called any two
 * background-color animation nodes equal, whatever colors they were sampled at).
 * Built as karouka_partial_raster_test (C, public header only). */

#include <karouka/karouka.h>

#include <spawn.h>
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

enum { width = 160, height = 120 };

/* The largest per-channel difference from a whole raster the software backend may show (the software
 * backend's captures are held to Metal's within 16 elsewhere too). */
enum { software_tolerance = 16 };

static char const page[] =
    "<!doctype html><title>loading</title>"
    "<style>"
    "* { margin: 0 }"
    "body { width: 160px; height: 120px; background: linear-gradient(135deg, rgb(230,236,246), rgb(200,210,230)) }"
    "div { position: absolute }"
    "#color { left: 4px; top: 4px; width: 30px; height: 14px; background: rgb(255,255,255); border-radius: 4px }"
    "#move { left: 40px; top: 4px; width: 20px; height: 14px; background: rgb(40,120,40) }"
    "#text { left: 4px; top: 24px; font: 12px sans-serif; color: rgb(20,20,60) }"
    "#shadow { left: 100px; top: 24px; width: 40px; height: 16px; background: rgb(250,250,250); border-radius: 6px }"
    "#group { left: 4px; top: 48px; width: 60px; height: 30px; opacity: 0.5; background: rgb(0,0,200) }"
    "#group span { position: absolute; left: 6px; top: 6px; width: 16px; height: 16px; background: rgb(200,0,0) }"
    "#rotated { left: 90px; top: 60px; width: 30px; height: 30px; transform: rotate(30deg); background: rgb(200,120,0) }"
    "#shrink { left: 4px; top: 90px; width: 120px; height: 20px; background: rgb(120,0,120) }"
    "#nw { left: 0; top: 0; width: 3px; height: 3px; background: rgb(0,0,0) }"
    "#se { left: 150px; top: 112px; width: 8px; height: 6px; background: rgb(0,0,0) }"
    "#scroller { left: 130px; top: 4px; width: 24px; height: 16px; overflow: hidden }"
    "#scroller p { margin: 0; height: 8px }"
    "</style>"
    "<body>"
    "<div id=color></div><div id=move></div><div id=text>order 1001</div><div id=shadow></div>"
    "<div id=group><span></span></div><div id=rotated></div><div id=shrink></div><div id=nw></div><div id=se></div>"
    "<div id=scroller><p style='background: rgb(200,40,40)'></p><p style='background: rgb(40,200,40)'></p>"
    "<p style='background: rgb(40,40,200)'></p><p style='background: rgb(200,200,40)'></p></div>"
    "<script>"
    "function $(id) { return document.getElementById(id); }"
    "engine.on('color', function () { $('color').style.background = 'rgb(60,110,220)'; });"
    "engine.on('move', function () { $('move').style.left = '64px'; });"
    "engine.on('text', function () { $('text').textContent = 'order 1002 shipped'; });"
    "engine.on('shadow', function () { $('shadow').style.boxShadow = '0 4px 10px rgba(0,0,0,0.6)'; });"
    "engine.on('group', function () { document.querySelector('#group span').style.left = '30px'; });"
    "engine.on('rotated', function () { $('rotated').style.background = 'rgb(0,160,160)'; });"
    "engine.on('shrink', function () { $('shrink').style.width = '0'; });"
    "engine.on('corners', function () { $('nw').style.background = 'rgb(255,0,0)'; $('se').style.background = 'rgb(0,0,255)'; });"
    "engine.on('scroll', function () { $('scroller').scrollTop = 12; });"
    "document.title = 'ready';"
    "</script></body>";

/* A box whose background color a compositor animation moves, with text inside it, and a box that
 * stays put. */
static char const animated_page[] =
    "<!doctype html><title>ready</title>"
    "<style>"
    "* { margin: 0 }"
    "body { width: 160px; height: 120px; background: rgb(230,236,246) }"
    "@keyframes pulse { from { background-color: rgb(255,0,0) } to { background-color: rgb(0,0,255) } }"
    "#pulse { position: absolute; left: 10px; top: 10px; width: 50px; height: 30px; border-radius: 6px;"
    "  font: 10px sans-serif; animation: pulse 2s linear infinite alternate }"
    "#still { position: absolute; left: 90px; top: 70px; width: 40px; height: 30px; background: rgb(40,120,40) }"
    "</style>"
    "<body><div id=pulse>a 1</div><div id=still></div></body>";

static char const* const changes[] = { "color", "move", "text", "shadow", "group", "rotated", "shrink", "corners", "scroll" };
enum { change_count = sizeof(changes) / sizeof(changes[0]) };

static double clock_ms;

/* Advances `frames` frames, rendering each when `render`; returns the pixels the renders
 * rasterized, or -1 on an error. */
static long long run_frames(kr_view* view, int frames, int render)
{
    long long pixels = 0;
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        if (kr_view_advance_time(view, clock_ms) != KR_OK)
            return -1;
        if (!render)
            continue;
        bool rendered = false;
        if (kr_view_render(view, &rendered) != KR_OK)
            return -1;
        if (rendered) {
            kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
            if (kr_view_query_stats(view, &stats) != KR_OK)
                return -1;
            pixels += (long long)stats.last_raster_pixels;
        }
    }
    return pixels;
}

static kr_view* create_view_with(kr_system* system, char const* html)
{
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = width;
    view_desc.height = height;
    view_desc.scale = 1;
    kr_view* view = NULL;
    if (kr_view_create2(system, &view_desc, &view) != KR_OK)
        return NULL;
    if (kr_view_load_html(view, html, strlen(html), "file:///karouka-partial-raster-test/") != KR_OK) {
        kr_view_destroy(view);
        return NULL;
    }
    return view;
}

static kr_view* create_view(kr_system* system)
{
    return create_view_with(system, page);
}

/* Copies the view's last frame into `pixels` (width * height BGRA8). */
static int read_frame(kr_view* view, unsigned char* pixels)
{
    void const* base = NULL;
    size_t bytes_per_row = 0;
    if (kr_view_lock_pixels(view, &base, &bytes_per_row) != KR_OK)
        return 0;
    for (int y = 0; y < height; ++y)
        memcpy(pixels + (size_t)y * width * 4, (unsigned char const*)base + (size_t)y * bytes_per_row, (size_t)width * 4);
    kr_view_unlock_pixels(view);
    return 1;
}

/* The bytes in which `partial` differs from `whole`; `*most` is the largest difference, `*first`
 * the first differing pixel or -1. */
static int compare_frames(unsigned char const* partial, unsigned char const* whole, int* most, int* first)
{
    int differing = 0;
    *most = 0;
    *first = -1;
    for (int i = 0; i < width * height * 4; ++i) {
        if (partial[i] != whole[i]) {
            if (*first < 0)
                *first = i / 4;
            ++differing;
            int delta = abs((int)partial[i] - (int)whole[i]);
            *most = delta > *most ? delta : *most;
        }
    }
    return differing;
}

/* The compositor background-color animation: `view` renders every frame, `twin` (loaded and stepped
 * on the same clock) renders once, whole, at the end; both sample the animation at the same time. */
static void check_background_color_animation(kr_system* system, char const* label, int tolerance, unsigned char* partial, unsigned char* whole)
{
    kr_view* view = create_view_with(system, animated_page);
    kr_view* twin = create_view_with(system, animated_page);
    CHECK(view != NULL && twin != NULL);
    if (!view || !twin) {
        if (view)
            kr_view_destroy(view);
        if (twin)
            kr_view_destroy(twin);
        return;
    }
    long long pixels = 0;
    int rendered_frames = 0;
    int ok = 1;
    for (int frame = 0; frame < 40 && ok; ++frame) {
        clock_ms += 16.0;
        ok = kr_view_advance_time(view, clock_ms) == KR_OK && kr_view_advance_time(twin, clock_ms) == KR_OK;
        bool rendered = false;
        ok = ok && kr_view_render(view, &rendered) == KR_OK;
        kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
        ok = ok && kr_view_query_stats(view, &stats) == KR_OK;
        /* The first frames load and raster whole; from frame 10 on only the animation changes. */
        if (ok && rendered && frame >= 10) {
            ++rendered_frames;
            pixels += (long long)stats.last_raster_pixels;
        }
    }
    CHECK(ok);
    /* Every frame from 10 on moved the color, and rasterized more than nothing but less than all. */
    int damage_ok = rendered_frames == 30 && pixels > 0 && pixels < (long long)rendered_frames * width * height / 4;
    CHECK(damage_ok);

    bool rendered = false;
    CHECK(kr_view_render(twin, &rendered) == KR_OK && rendered);
    int read = read_frame(view, partial) && read_frame(twin, whole);
    CHECK(read);
    int most = 0, first = -1;
    int differing = read ? compare_frames(partial, whole, &most, &first) : 0;
    CHECK(most <= tolerance);
    if (!damage_ok || most > tolerance)
        fprintf(stderr, "  %s background-color animation: %d of 30 frames rendered, rasterized %lld px; %d bytes differ from a whole raster (by up to %d), first at (%d,%d)\n", label, rendered_frames, pixels, differing, most, first < 0 ? -1 : first % width, first < 0 ? -1 : first / width);
    kr_view_destroy(twin);
    kr_view_destroy(view);
}

static int run(kr_backend backend, char const* label)
{
    int tolerance = backend == KR_BACKEND_SOFTWARE ? software_tolerance : 0;
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    system_desc.backend = backend;
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_partial_raster_test %s: engine failed to start\n", label);
        return 1;
    }
    static unsigned char partial[width * height * 4], whole[width * height * 4];

    kr_view* view = create_view(system);
    CHECK(view != NULL);
    if (!view)
        return 1;
    CHECK(run_frames(view, 30, 1) >= width * height);
    CHECK(strcmp(kr_view_title(view), "ready") == 0);
    CHECK(run_frames(view, 5, 1) == 0);

    for (int change = 0; change < change_count; ++change) {
        CHECK(kr_view_engine_trigger(view, changes[change], NULL) == KR_OK);
        long long pixels = run_frames(view, 5, 1);
        int damage_ok = pixels > 0 && pixels < width * height;
        if (strcmp(changes[change], "corners") == 0)
            damage_ok = damage_ok && pixels < width * height / 4;
        CHECK(damage_ok);

        /* The reference: a fresh view that made every change so far, rendered once (a load's
         * render draws the whole frame). */
        kr_view* fresh = create_view(system);
        CHECK(fresh != NULL);
        if (!fresh)
            break;
        CHECK(run_frames(fresh, 30, 0) == 0);
        for (int made = 0; made <= change; ++made)
            CHECK(kr_view_engine_trigger(fresh, changes[made], NULL) == KR_OK);
        CHECK(run_frames(fresh, 5, 0) == 0);
        bool rendered = false;
        CHECK(kr_view_render(fresh, &rendered) == KR_OK && rendered);
        kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
        CHECK(kr_view_query_stats(fresh, &stats) == KR_OK && stats.last_raster_pixels == (uint64_t)width * height);

        int read = read_frame(view, partial) && read_frame(fresh, whole);
        CHECK(read);
        int first = -1, most = 0;
        int differing = read ? compare_frames(partial, whole, &most, &first) : 0;
        CHECK(most <= tolerance);
        if (!damage_ok || most > tolerance)
            fprintf(stderr, "  %s %s: rasterized %lld of %d px; %d bytes differ from a whole raster (by up to %d), first at (%d,%d)\n", label, changes[change], pixels, width * height, differing, most, first < 0 ? -1 : first % width, first < 0 ? -1 : first / width);
        kr_view_destroy(fresh);
    }

    kr_view_destroy(view);
    check_background_color_animation(system, label, tolerance, partial, whole);
    kr_system_destroy(system);
    return failures;
}

int main(int argc, char** argv)
{
    if (argc > 1)
        return run(strcmp(argv[1], "software") == 0 ? KR_BACKEND_SOFTWARE : KR_BACKEND_METAL, argv[1]) ? 1 : 0;
    /* One system per process: each backend runs in a process of its own. */
    static char backend_names[2][9] = { "metal", "software" };
    int failed = 0;
    for (int i = 0; i < 2; ++i) {
        char* child_argv[] = { argv[0], backend_names[i], NULL };
        pid_t pid;
        int status = 0;
        if (posix_spawn(&pid, argv[0], NULL, NULL, child_argv, environ) != 0 || waitpid(pid, &status, 0) != pid
            || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "karouka_partial_raster_test: the %s backend failed\n", backend_names[i]);
            failed = 1;
        }
    }
    return failed;
}
