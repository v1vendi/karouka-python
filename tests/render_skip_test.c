/* Paints that change nothing in the viewport are not rasterized. LibWeb repaints the
 * whole document for a change anywhere in it; kr_view_render compares the new display list with
 * the frame the surface holds and keeps the surface when nothing visible differs. Checked through
 * the public ABI: changes below the fold report no render, while an off-screen change that moves
 * visible content, and a visible change, do. The page's handlers run from engine triggers.
 * Animated shader() images: one below the fold renders nothing, and in view it renders every
 * frame, rasterizing its own box (a repeated tile) and not the still shader() beside it.
 * Built as karouka_render_skip_test (C, public header only) and run by tools/dev.sh check. */

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

/* A 64x64 viewport. #box starts at 20 px and #mark at 50% of its height: at 50 px (visible)
 * while #grow is empty, at 70 px (below the viewport) once #grow is 40 px tall. #grow (80 px
 * down) and #far are always off-screen. */
static char const page[] =
    "<!doctype html><title>loading</title>"
    "<body style='margin:0'>"
    "<div id=top style='height:20px;background:rgb(0,0,255)'></div>"
    "<div id=box style='position:relative'>"
    "<div style='height:60px'></div>"
    "<div id=grow style='height:0'></div>"
    "<div id=mark style='position:absolute;left:0;top:50%;width:64px;height:10px;background:rgb(255,0,0)'></div>"
    "</div>"
    "<div style='height:2000px'></div>"
    "<div id=far style='color:black'>far</div>"
    "<script>"
    "function $(id) { return document.getElementById(id); }"
    "engine.on('far-color', function () { $('far').style.color = 'green'; });"
    "engine.on('far-text', function () { $('far').textContent = 'farther'; });"
    "engine.on('grow', function () { $('grow').style.height = '40px'; });"
    "engine.on('top-color', function () { $('top').style.background = 'rgb(0,255,0)'; });"
    "document.title = 'ready';"
    "</script></body>";

/* A still shader() (no uTime) across the top, and an animated one (uTime) tiled at 8x8 px in #anim,
 * which 'show' moves from below the fold into the viewport and 'hide' moves back. */
static char const shader_page[] =
    "<!doctype html><title>loading</title>"
    "<body style='margin:0'>"
    "<div style='height:20px;background-image:shader(\"half4 main(float2 c) { return half4(0.0, 0.0, 1.0, 1.0); }\")'></div>"
    "<div id=anim style='position:absolute;left:8px;top:2000px;width:16px;height:16px;background-size:8px 8px;"
    "background-image:shader(\"uniform float uTime; half4 main(float2 c) { return half4(half(fract(uTime)), 0.5, 0.0, 1.0); }\")'></div>"
    "<div style='height:3000px'></div>"
    "<script>"
    "engine.on('show', function () { document.getElementById('anim').style.top = '30px'; });"
    "engine.on('hide', function () { document.getElementById('anim').style.top = '2000px'; });"
    "document.title = 'ready';"
    "</script></body>";

static double clock_ms;

/* Drives `frames` frames; returns how many of them rendered, or -1 on an error. */
static int run_frames(kr_view* view, int frames)
{
    int rendered_frames = 0;
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return -1;
        rendered_frames += rendered;
    }
    return rendered_frames;
}

/* Triggers the page's handler `name`, then returns how many of the next frames rendered. */
static int renders_after(kr_view* view, char const* name)
{
    if (kr_view_engine_trigger(view, name, NULL) != KR_OK)
        return -1;
    return run_frames(view, 10);
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_render_skip_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-render-skip-test/") == KR_OK);
    CHECK(run_frames(view, 30) >= 1);
    CHECK(strcmp(kr_view_title(view), "ready") == 0);
    CHECK(run_frames(view, 10) == 0);

    /* Paint-only and layout changes wholly below the fold keep the surface. */
    int far_color = renders_after(view, "far-color");
    int far_text = renders_after(view, "far-text");
    CHECK(far_color == 0);
    CHECK(far_text == 0);

    /* #grow is off-screen too, but it moves #mark out of the viewport: that renders. */
    int grow = renders_after(view, "grow");
    CHECK(grow >= 1);

    /* A visible change renders. */
    int top_color = renders_after(view, "top-color");
    CHECK(top_color >= 1);

    if (failures)
        fprintf(stderr, "  renders: far-color=%d far-text=%d grow=%d top-color=%d\n", far_color, far_text, grow, top_color);

    /* Animated shader() images keep a view rendering only while one is in the viewport. */
    CHECK(kr_view_load_html(view, shader_page, strlen(shader_page), "file:///karouka-render-skip-test/") == KR_OK);
    CHECK(run_frames(view, 30) >= 1);
    CHECK(strcmp(kr_view_title(view), "ready") == 0);
    int offscreen = run_frames(view, 10);
    CHECK(offscreen == 0);
    int shown = renders_after(view, "show");
    CHECK(shown == 10);
    kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
    CHECK(kr_view_query_stats(view, &stats) == KR_OK);
    /* #anim's 16x16 box, inflated by a pixel: the still shader above it isn't rasterized again. */
    CHECK(stats.last_raster_pixels >= 16 * 16 && stats.last_raster_pixels <= 18 * 18);
    int hidden = renders_after(view, "hide");
    CHECK(hidden >= 1 && hidden <= 2);
    int idle = run_frames(view, 10);
    CHECK(idle == 0);
    if (failures)
        fprintf(stderr, "  shader renders: offscreen=%d shown=%d (last raster %llu px) hidden=%d idle=%d\n", offscreen, shown, (unsigned long long)stats.last_raster_pixels, hidden, idle);

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_render_skip_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
