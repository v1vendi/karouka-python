/* Bytes per subsystem (kr_system_memory_usage). Checked through the public ABI: a page
 * with text, a decoded image and a 2D canvas reports a GC heap whose live bytes fit in its blocks, a
 * display list, the image's pixels, glyphs in the font cache, a GPU cache, malloc and surface
 * totals, and the canvas's two surfaces (also in kr_view_stats.canvas_bytes); an out-of-range tag
 * and a null system report nothing. kr_view_destroy drops its page's canvas surfaces at once, and
 * only its page's: a second view's canvas stays. That includes surfaces made with no script running:
 * an OffscreenCanvas resized by script gets its backing storage at the next rendering update's
 * placeholder commit.
 * Built as karouka_memory_usage_test (C, public header only) and run by tools/dev.sh check. */

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

/* A 16x16 opaque red PNG: 1 KiB of decoded pixels. */
static char const page[] =
    "<!doctype html><title>loading</title>"
    "<body style='margin:0'>"
    "<p>Some text for the glyph cache.</p>"
    "<img id=image width=16 height=16 src='data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAGUlEQVR4nGP4z8DwnxLMMGrAqAGjBgwXAwAwxP4QHCfkAAAAAABJRU5ErkJggg=='>"
    "<canvas id=canvas width=20 height=10></canvas>"
    "<script>"
    "var context = document.getElementById('canvas').getContext('2d');"
    "context.fillStyle = 'blue'; context.fillRect(0, 0, 5, 5);"
    "var image = document.getElementById('image');"
    "function done() { document.title = image.naturalWidth == 16 ? 'ready' : 'broken'; }"
    "if (image.complete) done(); else { image.onload = done; image.onerror = done; }"
    "</script></body>";

/* Resizing the OffscreenCanvas drops its backing storage; the rendering update's placeholder commit
 * makes the new one (two 14x10 surfaces) outside any script, then copies it to the placeholder. */
static char const offscreen_page[] =
    "<!doctype html><title>loading</title>"
    "<body style='margin:0'><canvas id=canvas width=20 height=10></canvas>"
    "<script>"
    "var offscreen = document.getElementById('canvas').transferControlToOffscreen();"
    "offscreen.getContext('2d');"
    "offscreen.width = 14;"
    "document.title = 'ready';"
    "</script></body>";

static double clock_ms;

static void run_frames(kr_view* view)
{
    for (int frame = 0; frame < 30; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_memory_usage_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_DISPLAY_LIST) == 0);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_SURFACES) == 0);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_CANVAS) == 0);

    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-memory-usage-test/") == KR_OK);
    run_frames(view);
    CHECK(strcmp(kr_view_title(view), "ready") == 0);

    size_t usage[KR_MEM_TAG_COUNT];
    for (int tag = 0; tag < KR_MEM_TAG_COUNT; ++tag) {
        CHECK(kr_mem_tag_name((kr_mem_tag)tag) != NULL);
        usage[tag] = kr_system_memory_usage(system, (kr_mem_tag)tag);
    }
    CHECK(usage[KR_MEM_TAG_GC_HEAP_LIVE] > 0);
    CHECK(usage[KR_MEM_TAG_GC_HEAP_LIVE] <= usage[KR_MEM_TAG_GC_HEAP_ALLOCATED]);
    CHECK(usage[KR_MEM_TAG_GC_HEAP_ALLOCATED] % (16 * 1024) == 0);
    CHECK(usage[KR_MEM_TAG_GPU_CACHE] > 0);
    CHECK(usage[KR_MEM_TAG_DISPLAY_LIST] > 0);
    CHECK(usage[KR_MEM_TAG_DECODED_IMAGES] >= 16 * 16 * 4);
    CHECK(usage[KR_MEM_TAG_FONT_CACHE] > 0);
    kr_memory_stats memory = { .struct_size = sizeof(kr_memory_stats) };
    CHECK(kr_system_query_memory_stats(system, &memory) == KR_OK);
    CHECK(memory.font_cache_strikes > 0 && memory.font_cache_strikes <= 256);
    CHECK(usage[KR_MEM_TAG_MALLOC_TOTAL] > usage[KR_MEM_TAG_DECODED_IMAGES]);
    CHECK(usage[KR_MEM_TAG_SURFACES] == 64 * 64 * 4);
    CHECK(usage[KR_MEM_TAG_DECODED_IMAGES_PEAK] >= usage[KR_MEM_TAG_DECODED_IMAGES]);
    CHECK(usage[KR_MEM_TAG_CANVAS] == 2 * 20 * 10 * 4);
    kr_view_stats stats = { .struct_size = sizeof(kr_view_stats) };
    CHECK(kr_view_query_stats(view, &stats) == KR_OK);
    CHECK(stats.canvas_bytes == usage[KR_MEM_TAG_CANVAS]);

    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_COUNT) == 0);
    CHECK(kr_system_memory_usage(NULL, KR_MEM_TAG_MALLOC_TOTAL) == 0);
    CHECK(kr_mem_tag_name(KR_MEM_TAG_COUNT) == NULL);

    if (failures) {
        for (int tag = 0; tag < KR_MEM_TAG_COUNT; ++tag)
            fprintf(stderr, "  %s=%zu\n", kr_mem_tag_name((kr_mem_tag)tag), usage[tag]);
    }

    kr_view* second = NULL;
    CHECK(kr_view_create2(system, &view_desc, &second) == KR_OK);
    CHECK(kr_view_load_html(second, page, strlen(page), "file:///karouka-memory-usage-test/") == KR_OK);
    run_frames(second);
    CHECK(strcmp(kr_view_title(second), "ready") == 0);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_CANVAS) == 2 * 2 * 20 * 10 * 4);

    kr_view_destroy(view);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_CANVAS) == 2 * 20 * 10 * 4);
    kr_view_destroy(second);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_CANVAS) == 0);

    kr_view* offscreen = NULL;
    CHECK(kr_view_create2(system, &view_desc, &offscreen) == KR_OK);
    CHECK(kr_view_load_html(offscreen, offscreen_page, strlen(offscreen_page), "file:///karouka-memory-usage-test/") == KR_OK);
    run_frames(offscreen);
    CHECK(strcmp(kr_view_title(offscreen), "ready") == 0);
    size_t offscreen_bytes = kr_system_memory_usage(system, KR_MEM_TAG_CANVAS);
    CHECK(offscreen_bytes == 3 * 14 * 10 * 4);
    kr_view_destroy(offscreen);
    size_t offscreen_left = kr_system_memory_usage(system, KR_MEM_TAG_CANVAS);
    CHECK(offscreen_left == 0);
    if (offscreen_bytes != 3 * 14 * 10 * 4 || offscreen_left)
        fprintf(stderr, "  offscreen canvas bytes: %zu, after kr_view_destroy: %zu\n", offscreen_bytes, offscreen_left);
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_memory_usage_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
