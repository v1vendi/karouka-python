/* The software backend (kr_system_desc.backend = KR_BACKEND_SOFTWARE): no GPU context, Skia's
 * CPU rasterizer writes each frame into the view's IOSurface before kr_view_render returns. Checked
 * through the public ABI: the desc's refusals, the frame's pixels read from the IOSurface and through
 * kr_view_lock_pixels, the PNG capture, kr_view_set_output2's refusal and the GPU cache
 * counters at 0.
 * Built as karouka_software_backend_test (C, public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

/* backend starts past the 64 bytes of the struct before it, so an older host's tail padding is
 * never read as a backend (karouka.h). */
_Static_assert(offsetof(kr_system_desc, backend) >= 64, "backend must start past the older struct");

/* A 64x64 viewport: the left half red, the right half green, a black 8x8 square at (28,28). */
static char const page[] =
    "<!doctype html><title>software</title>"
    "<body style='margin:0'>"
    "<div style='position:absolute;left:0;top:0;width:32px;height:64px;background:rgb(255,0,0)'></div>"
    "<div style='position:absolute;left:32px;top:0;width:32px;height:64px;background:rgb(0,128,0)'></div>"
    "<div style='position:absolute;left:28px;top:28px;width:8px;height:8px;background:rgb(0,0,0)'></div>"
    "</body>";

/* A 64x64 view at scale 2 is 32x32 CSS pixels. A shader() image's coord and uSize are CSS pixels and its
 * uDevicePixelRatio is 2, whether it is drawn once (the left 16x32 box, no-repeat) or repeated as a tile (the
 * right box, 8x8 tiles). Each paints red left of its middle and green right of it; a wrong uSize or ratio
 * paints blue. Device coordinates would move each edge left by half. */
#define SCALED_SHADER(W, H) "background-image:shader(\"uniform float2 uSize; uniform float uDevicePixelRatio;" \
    " half4 main(float2 c) { if (abs(uSize.x - " #W ".0) > 0.5 || abs(uSize.y - " #H ".0) > 0.5 || abs(uDevicePixelRatio - 2.0) > 0.01)" \
    " return half4(0, 0, 1, 1); return c.x < " #W ".0 / 2.0 ? half4(1, 0, 0, 1) : half4(0, 1, 0, 1); }\");"
static char const scaled_page[] =
    "<!doctype html><title>scaled</title>"
    "<body style='margin:0'>"
    "<div style='position:absolute;left:0;top:0;width:16px;height:32px;background-repeat:no-repeat;" SCALED_SHADER(16, 32) "'></div>"
    "<div style='position:absolute;left:16px;top:0;width:16px;height:32px;background-size:8px 8px;" SCALED_SHADER(8, 8) "'></div>"
    "</body>";

/* The IOSurface's pixel at (x, y) as 0xAARRGGBB (the surface is BGRA8). */
static unsigned pixel(IOSurfaceRef surface, int x, int y)
{
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess)
        return 0;
    unsigned char const* row = (unsigned char const*)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
    unsigned char const* p = row + (size_t)x * 4;
    unsigned value = ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    return value;
}

/* The pixel at (x, y) of a frame kr_view_lock_pixels returned, as 0xAARRGGBB (BGRA bytes). */
static unsigned locked_pixel(void const* pixels, size_t bytes_per_row, int x, int y)
{
    unsigned char const* p = (unsigned char const*)pixels + (size_t)y * bytes_per_row + (size_t)x * 4;
    return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
}

int main(void)
{
    kr_system* system = NULL;

    /* Refused before anything process-wide starts: an unknown backend, and a host GPU with the
     * software backend (the pointers are never read). */
    kr_system_desc bad = { .struct_size = sizeof(kr_system_desc), .backend = 2 };
    CHECK(kr_system_create2(&bad, &system) == KR_ERROR_INVALID_ARGUMENT);
    bad.backend = KR_BACKEND_SOFTWARE;
    bad.mtl_device = (void*)&bad;
    bad.mtl_queue = (void*)&bad;
    CHECK(kr_system_create2(&bad, &system) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(system == NULL);

    /* gpu_cache_bytes is ignored: there is no GPU cache. */
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .backend = KR_BACKEND_SOFTWARE, .gpu_cache_bytes = 32u << 20 };
    if (kr_system_create2(&system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_software_backend_test: engine failed to start on the software backend\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return 1;
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-software-backend-test/") == KR_OK);

    /* No frame to lock before the first render; nothing written on a refusal. */
    void const* pixels = &pixels;
    size_t bytes_per_row = 7;
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_ERROR_INVALID_STATE);
    CHECK(pixels == &pixels && bytes_per_row == 7);
    CHECK(kr_view_lock_pixels(NULL, &pixels, &bytes_per_row) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_lock_pixels(view, NULL, &bytes_per_row) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_lock_pixels(view, &pixels, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_unlock_pixels(view) == KR_ERROR_INVALID_STATE);
    CHECK(kr_view_unlock_pixels(NULL) == KR_ERROR_INVALID_ARGUMENT);

    double clock_ms = 0;
    int rendered_frames = 0;
    for (int frame = 0; frame < 6; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
        rendered_frames += rendered;
    }
    CHECK(rendered_frames > 0);
    CHECK(strcmp(kr_view_title(view), "software") == 0);

    /* The frame is in the IOSurface when kr_view_render returns. */
    IOSurfaceRef surface = (IOSurfaceRef)kr_view_iosurface(view);
    CHECK(surface != NULL);
    if (surface) {
        CHECK(IOSurfaceGetWidth(surface) == 64 && IOSurfaceGetHeight(surface) == 64);
        CHECK(pixel(surface, 4, 4) == 0xFFFF0000u);
        CHECK(pixel(surface, 60, 60) == 0xFF008000u);
        CHECK(pixel(surface, 31, 31) == 0xFF000000u);
        CHECK(pixel(surface, 32, 32) == 0xFF000000u);
    }

    /* The same frame through the CPU pixel API: the IOSurface's bytes at its row pitch. */
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_OK);
    CHECK(pixels != NULL && pixels != &pixels);
    CHECK(bytes_per_row >= 64u * 4u);
    if (surface)
        CHECK(bytes_per_row == IOSurfaceGetBytesPerRow(surface));
    if (pixels && pixels != &pixels && bytes_per_row >= 64u * 4u) {
        CHECK(locked_pixel(pixels, bytes_per_row, 4, 4) == 0xFFFF0000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 60, 60) == 0xFF008000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 31, 31) == 0xFF000000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 32, 32) == 0xFF000000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 63, 0) == 0xFF008000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 0, 63) == 0xFFFF0000u);
    }
    /* While locked: no second lock, no render, no resize; the surface stays and capture still reads. */
    void const* again = NULL;
    size_t again_row = 0;
    CHECK(kr_view_lock_pixels(view, &again, &again_row) == KR_ERROR_INVALID_STATE);
    CHECK(again == NULL && again_row == 0);
    bool locked_rendered = true;
    CHECK(kr_view_render(view, &locked_rendered) == KR_ERROR_INVALID_STATE);
    CHECK(!locked_rendered);
    CHECK(kr_view_resize(view, 32, 16, 1) == KR_ERROR_INVALID_STATE);
    CHECK(kr_view_iosurface(view) == (void*)surface);
    char locked_path[] = "/tmp/karouka-software-backend-locked-XXXXXX";
    int locked_fd = mkstemp(locked_path);
    CHECK(locked_fd >= 0);
    if (locked_fd >= 0) {
        close(locked_fd);
        CHECK(kr_view_capture_png(view, locked_path) == KR_OK);
        unlink(locked_path);
    }
    if (pixels && pixels != &pixels && bytes_per_row >= 64u * 4u)
        CHECK(locked_pixel(pixels, bytes_per_row, 4, 4) == 0xFFFF0000u);
    CHECK(kr_view_unlock_pixels(view) == KR_OK);
    CHECK(kr_view_unlock_pixels(view) == KR_ERROR_INVALID_STATE);
    /* Unlocked, rendering works again (nothing changed, so nothing is drawn). */
    clock_ms += 16.0;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    CHECK(kr_view_render(view, NULL) == KR_OK);

    /* No host queue, so no host texture to draw into. */
    kr_metal_output output = { .struct_size = sizeof(kr_metal_output), .command_buffer = (void*)&output, .texture = (void*)&output };
    CHECK(kr_view_set_output2(view, &output) == KR_ERROR_INVALID_STATE);

    /* No GPU cache; the surface is the view's 64x64 BGRA8 IOSurface. */
    kr_memory_stats memory = { .struct_size = sizeof(kr_memory_stats) };
    CHECK(kr_system_query_memory_stats(system, &memory) == KR_OK);
    CHECK(memory.gpu_cache_bytes == 0);
    CHECK(memory.gpu_cache_budget_bytes == 0);
    CHECK(memory.surface_bytes == 64u * 64u * 4u);
    CHECK(kr_system_memory_usage(system, KR_MEM_TAG_GPU_CACHE) == 0);

    /* The capture reads the same pixels back on the CPU. */
    char path[] = "/tmp/karouka-software-backend-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
        CHECK(kr_view_capture_png(view, path) == KR_OK);
        struct stat info;
        CHECK(stat(path, &info) == 0 && info.st_size > 64);
        unlink(path);
    }

    /* A resize gets a new surface at the new size, rendered the same way. Until that render there
     * is no frame to lock. */
    CHECK(kr_view_resize(view, 32, 16, 1) == KR_OK);
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_ERROR_INVALID_STATE);
    for (int frame = 0; frame < 3; ++frame) {
        clock_ms += 16.0;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, NULL) == KR_OK);
    }
    surface = (IOSurfaceRef)kr_view_iosurface(view);
    CHECK(surface != NULL);
    if (surface) {
        CHECK(IOSurfaceGetWidth(surface) == 32 && IOSurfaceGetHeight(surface) == 16);
        CHECK(pixel(surface, 4, 4) == 0xFFFF0000u);
    }

    /* The resized frame through the API; the view is destroyed still locked (destroy unlocks). */
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_OK);
    CHECK(bytes_per_row >= 32u * 4u);
    if (pixels && bytes_per_row >= 32u * 4u) {
        CHECK(locked_pixel(pixels, bytes_per_row, 4, 4) == 0xFFFF0000u);
        CHECK(locked_pixel(pixels, bytes_per_row, 31, 15) == 0xFFFF0000u); /* the left half's red */
    }

    kr_view_destroy(view);

    /* A shader() image at scale 2 works in CSS pixels. */
    kr_view_desc scaled_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 2 };
    kr_view* scaled = NULL;
    CHECK(kr_view_create2(system, &scaled_desc, &scaled) == KR_OK);
    if (scaled) {
        CHECK(kr_view_load_html(scaled, scaled_page, strlen(scaled_page), "file:///karouka-software-backend-test/") == KR_OK);
        for (int frame = 0; frame < 3; ++frame) {
            clock_ms += 16.0;
            CHECK(kr_view_advance_time(scaled, clock_ms) == KR_OK);
            CHECK(kr_view_render(scaled, NULL) == KR_OK);
        }
        IOSurfaceRef scaled_surface = (IOSurfaceRef)kr_view_iosurface(scaled);
        CHECK(scaled_surface != NULL);
        if (scaled_surface) {
            CHECK(pixel(scaled_surface, 12, 20) == 0xFFFF0000u); /* CSS x 6 of the 16px box */
            CHECK(pixel(scaled_surface, 20, 20) == 0xFF00FF00u); /* CSS x 10 */
            CHECK(pixel(scaled_surface, 36, 20) == 0xFFFF0000u); /* CSS x 2 of an 8px tile */
            CHECK(pixel(scaled_surface, 44, 20) == 0xFF00FF00u); /* CSS x 6 of the tile */
        }
        kr_view_destroy(scaled);
    }
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_software_backend_test: %d check(s) failed\n", failures);
    else
        printf("karouka_software_backend_test: ok\n");
    return failures ? 1 : 0;
}
