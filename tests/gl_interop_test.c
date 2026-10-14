/* OpenGL consumption of kr_view_iosurface, done the way karouka.h documents it: the
 * IOSurface bound to a GL_TEXTURE_RECTANGLE with CGLTexImageIOSurface2D (GL_RGBA, GL_BGRA,
 * GL_UNSIGNED_INT_8_8_8_8_REV), read at the frame-complete point kr_view_render documents, then
 * read back through a framebuffer with glReadPixels.
 *
 * Each backend runs in its own child process (one system per process): "metal" (karouka's own
 * device and queue: the frame is complete when kr_view_render returns), "hostqueue" (the host's
 * device and queue: complete once a command buffer the host commits afterwards has completed) and
 * "software" (KR_BACKEND_SOFTWARE: complete when kr_view_render returns). Each child checks that
 * GL reads exactly the IOSurface's pixels, top row first, for two pages rendered into the same
 * surface (bound once: GL sees the second frame without binding again) and again after a resize
 * (a new surface, bound again), and writes its GL readbacks to a file. The parent then holds the
 * Metal and software readbacks to each other.
 *
 * Each child also reads every frame through kr_view_lock_pixels at the same
 * frame-complete point and holds it to GL's readback: the IOSurface's bytes at its row pitch, with
 * render, resize and a second lock refused while locked, and no frame to lock before the first
 * render or after a resize until the next. The host-queue child also locks around a render into a
 * host texture (kr_view_set_output2): the locked render is refused and the target stays pending,
 * the view then has no frame to lock, and the next render without a target gives it one again.
 *
 * With no OpenGL renderer on the host (CGLChoosePixelFormat finds none) the test prints that it
 * skipped and passes: macOS still ships OpenGL (on Metal) on every Mac karouka builds for.
 * Built as karouka_gl_interop_test (C, public header only) and run by tools/dev.sh check. */

#define GL_SILENCE_DEPRECATION 1

#include <karouka/karouka.h>

#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

enum { W = 64, H = 64, RW = 32, RH = 16 };

/* A 64x64 viewport of axis-aligned, pixel-aligned boxes (no anti-aliased edges, so the backends
 * agree exactly): the left half one color, the right half another, a black 8x8 square at (28,28)
 * and a blue 64x4 strip along the top, so a flipped readback can't pass. */
#define PAGE(left, right)                                                                                       \
    "<!doctype html><body style='margin:0'>"                                                                  \
    "<div style='position:absolute;left:0;top:0;width:32px;height:64px;background:" left "'></div>"           \
    "<div style='position:absolute;left:32px;top:0;width:32px;height:64px;background:" right "'></div>"       \
    "<div style='position:absolute;left:28px;top:28px;width:8px;height:8px;background:rgb(0,0,0)'></div>"     \
    "<div style='position:absolute;left:0;top:0;width:64px;height:4px;background:rgb(0,0,255)'></div>"        \
    "</body>"

static char const first_page[] = PAGE("rgb(255,0,0)", "rgb(0,128,0)");
static char const second_page[] = PAGE("rgb(0,128,0)", "rgb(255,0,0)");

/* ---- Metal from C, for the host-queue case. */

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

static void send_ulong(void* object, char const* selector, unsigned long value)
{
    ((void (*)(void*, SEL, unsigned long))objc_msgSend)(object, sel_registerName(selector), value);
}

enum {
    MTLPixelFormatBGRA8Unorm = 80,
    MTLTextureUsageShaderRead = 1,
    MTLTextureUsageRenderTarget = 4,
    MTLStorageModePrivate = 2,
};

/* A private BGRA8 render target of `device`, as karouka.h asks of kr_metal_output.texture (+1). */
static void* new_render_target(void* device, int width, int height)
{
    void* descriptor = ((void* (*)(void*, SEL, unsigned long, unsigned long, unsigned long, bool))objc_msgSend)(
        (void*)objc_getClass("MTLTextureDescriptor"), sel_registerName("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
        MTLPixelFormatBGRA8Unorm, (unsigned long)width, (unsigned long)height, false);
    if (!descriptor)
        return NULL;
    send_ulong(descriptor, "setUsage:", MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    send_ulong(descriptor, "setStorageMode:", MTLStorageModePrivate);
    return ((void* (*)(void*, SEL, void*))objc_msgSend)(device, sel_registerName("newTextureWithDescriptor:"), descriptor);
}

/* ---- GL: a headless context, the IOSurface bound to a rectangle texture, read back through a
 * framebuffer. */

typedef struct gl_state {
    CGLContextObj context;
    GLuint texture;
    GLuint framebuffer;
    IOSurfaceRef bound; /* the surface the texture is bound to */
} gl_state;

/* 0: no OpenGL renderer on this host. */
static int gl_create(gl_state* gl)
{
    CGLPixelFormatAttribute const attributes[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj format = NULL;
    GLint formats = 0;
    if (CGLChoosePixelFormat(attributes, &format, &formats) != kCGLNoError || !format)
        return 0;
    CGLError error = CGLCreateContext(format, NULL, &gl->context);
    CGLReleasePixelFormat(format);
    if (error != kCGLNoError || !gl->context)
        return 0;
    CGLSetCurrentContext(gl->context);
    glGenTextures(1, &gl->texture);
    glGenFramebuffers(1, &gl->framebuffer);
    gl->bound = NULL;
    return 1;
}

static void gl_destroy(gl_state* gl)
{
    glDeleteFramebuffers(1, &gl->framebuffer);
    glDeleteTextures(1, &gl->texture);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(gl->context);
}

/* Binds the view's current surface, if it isn't the one bound already: a surface stays bound
 * across renders; kr_view_resize (or a render into a host texture) replaces it. */
static int gl_bind(gl_state* gl, IOSurfaceRef surface)
{
    if (surface == gl->bound)
        return 1;
    glBindTexture(GL_TEXTURE_RECTANGLE, gl->texture);
    CGLError error = CGLTexImageIOSurface2D(gl->context, GL_TEXTURE_RECTANGLE, GL_RGBA,
        (GLsizei)IOSurfaceGetWidth(surface), (GLsizei)IOSurfaceGetHeight(surface),
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0);
    if (error != kCGLNoError) {
        fprintf(stderr, "CGLTexImageIOSurface2D: %s\n", CGLErrorString(error));
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, gl->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;
    gl->bound = surface;
    return 1;
}

/* GL row 0 is the texture's first row, which is the IOSurface's first row: the page's top. */
static int gl_read(gl_state* gl, IOSurfaceRef surface, unsigned char* out, int width, int height)
{
    if (!gl_bind(gl, surface))
        return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out);
    return glGetError() == GL_NO_ERROR;
}

/* ---- The IOSurface read on the CPU, and pixel helpers (0xAARRGGBB; the surface is BGRA8). */

static int surface_read(IOSurfaceRef surface, unsigned char* out, int width, int height)
{
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess)
        return 0;
    unsigned char const* base = (unsigned char const*)IOSurfaceGetBaseAddress(surface);
    size_t stride = IOSurfaceGetBytesPerRow(surface);
    for (int y = 0; y < height; ++y)
        memcpy(out + (size_t)y * width * 4, base + (size_t)y * stride, (size_t)width * 4);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    return 1;
}

static unsigned pixel(unsigned char const* pixels, int width, int x, int y)
{
    unsigned char const* p = pixels + ((size_t)y * width + x) * 4;
    return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
}

/* ---- One backend, in a child process. */

static void render_frames(kr_view* view, double* clock_ms, int count)
{
    for (int frame = 0; frame < count; ++frame) {
        *clock_ms += 16.0;
        CHECK(kr_view_advance_time(view, *clock_ms) == KR_OK);
        CHECK(kr_view_render(view, NULL) == KR_OK);
    }
}

/* With the host's queue kr_view_render returns before the GPU has drawn: the frame is complete
 * once a command buffer committed to that queue afterwards has completed. */
static void wait_for_frame(void* queue)
{
    if (!queue)
        return;
    void* fence = send(queue, "commandBuffer");
    send_void(fence, "commit");
    send_void(fence, "waitUntilCompleted");
}

/* No frame to lock: refused, and nothing written. */
static void check_nothing_to_lock(kr_view* view)
{
    void const* pixels = &pixels;
    size_t bytes_per_row = 7;
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_ERROR_INVALID_STATE);
    CHECK(pixels == &pixels && bytes_per_row == 7);
}

/* Locks the frame (complete: the caller waited as kr_view_iosurface documents) and holds it to
 * `expected`, a tightly packed width x height BGRA8 readback of `surface`. Locked, the view refuses
 * a second lock, a render and a resize, keeps its surface, and the pixels stay as they were. */
static void check_locked_frame(kr_view* view, IOSurfaceRef surface, unsigned char const* expected, int width, int height)
{
    void const* pixels = NULL;
    size_t bytes_per_row = 0;
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_OK);
    CHECK(pixels != NULL);
    CHECK(bytes_per_row == IOSurfaceGetBytesPerRow(surface));
    if (!pixels || bytes_per_row < (size_t)width * 4) {
        CHECK(!"locked frame unusable");
        kr_view_unlock_pixels(view);
        return;
    }
    int differing_rows = 0;
    for (int y = 0; y < height; ++y)
        differing_rows += memcmp((unsigned char const*)pixels + (size_t)y * bytes_per_row, expected + (size_t)y * width * 4, (size_t)width * 4) != 0;
    if (differing_rows)
        fprintf(stderr, "karouka_gl_interop_test: %d locked row(s) differ from GL's readback\n", differing_rows);
    CHECK(differing_rows == 0);

    void const* again = NULL;
    size_t again_row = 0;
    CHECK(kr_view_lock_pixels(view, &again, &again_row) == KR_ERROR_INVALID_STATE);
    CHECK(again == NULL && again_row == 0);
    bool rendered = true;
    CHECK(kr_view_render(view, &rendered) == KR_ERROR_INVALID_STATE);
    CHECK(!rendered);
    CHECK(kr_view_resize(view, (uint32_t)width + 1, (uint32_t)height, 1) == KR_ERROR_INVALID_STATE);
    CHECK(kr_view_iosurface(view) == (void*)surface);
    CHECK(memcmp(pixels, expected, (size_t)width * 4) == 0);

    CHECK(kr_view_unlock_pixels(view) == KR_OK);
    CHECK(kr_view_unlock_pixels(view) == KR_ERROR_INVALID_STATE);
}

/* Renders `html` and checks that GL and kr_view_lock_pixels read the surface's pixels; leaves GL's
 * readback in `out`. */
static void render_and_compare(kr_view* view, gl_state* gl, void* queue, double* clock_ms, char const* html,
    unsigned char* out, IOSurfaceRef* surface_out)
{
    CHECK(kr_view_load_html(view, html, strlen(html), "file:///karouka-gl-interop-test/") == KR_OK);
    render_frames(view, clock_ms, 6);
    wait_for_frame(queue);
    IOSurfaceRef surface = (IOSurfaceRef)kr_view_iosurface(view);
    CHECK(surface != NULL);
    if (!surface)
        return;
    CHECK(IOSurfaceGetWidth(surface) == W && IOSurfaceGetHeight(surface) == H);
    static unsigned char cpu[W * H * 4];
    CHECK(surface_read(surface, cpu, W, H));
    CHECK(gl_read(gl, surface, out, W, H));
    CHECK(memcmp(cpu, out, sizeof cpu) == 0);
    check_locked_frame(view, surface, out, W, H);
    *surface_out = surface;
}

/* Host queue only: a lock around a render into a host texture. Locked, the render is refused and
 * the target stays pending; unlocked, the render draws into the texture and the view has no
 * IOSurface, so no frame to lock; the next render without a target draws the whole frame into a
 * new surface, which locks again. `expected` is the view's current frame, width x height. */
static void check_lock_with_host_texture(kr_view* view, void* device, void* queue, double* clock_ms,
    unsigned char const* expected, int width, int height)
{
    void* texture = new_render_target(device, width, height);
    CHECK(texture != NULL);
    if (!texture)
        return;
    void const* pixels = NULL;
    size_t bytes_per_row = 0;
    CHECK(kr_view_lock_pixels(view, &pixels, &bytes_per_row) == KR_OK);
    void* commands = send(queue, "commandBuffer");
    kr_metal_output output = { .struct_size = sizeof(kr_metal_output), .command_buffer = commands, .texture = texture };
    CHECK(kr_view_set_output2(view, &output) == KR_OK);
    *clock_ms += 16.0;
    CHECK(kr_view_advance_time(view, *clock_ms) == KR_OK);
    CHECK(kr_view_render(view, NULL) == KR_ERROR_INVALID_STATE);
    CHECK(kr_view_unlock_pixels(view) == KR_OK);

    /* The target was kept: this render consumes it. */
    bool rendered = false;
    CHECK(kr_view_render(view, &rendered) == KR_OK);
    CHECK(rendered);
    send_void(commands, "commit");
    send_void(commands, "waitUntilCompleted");
    CHECK(kr_view_iosurface(view) == NULL);
    check_nothing_to_lock(view);

    render_frames(view, clock_ms, 1);
    wait_for_frame(queue);
    IOSurfaceRef surface = (IOSurfaceRef)kr_view_iosurface(view);
    CHECK(surface != NULL);
    if (surface) {
        CHECK((int)IOSurfaceGetWidth(surface) == width && (int)IOSurfaceGetHeight(surface) == height);
        check_locked_frame(view, surface, expected, width, height);
    }
    send_void(texture, "release");
}

static int run_backend(char const* mode, char const* out_path)
{
    void* pool = objc_autoreleasePoolPush();
    gl_state gl;
    if (!gl_create(&gl)) {
        printf("karouka_gl_interop_test %s: skipped, no OpenGL renderer on this host\n", mode);
        return 77;
    }
    printf("karouka_gl_interop_test %s: %s, %s\n", mode, glGetString(GL_RENDERER), glGetString(GL_VERSION));

    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    void* device = NULL;
    void* queue = NULL;
    if (strcmp(mode, "software") == 0) {
        system_desc.backend = KR_BACKEND_SOFTWARE;
    } else if (strcmp(mode, "hostqueue") == 0) {
        device = MTLCreateSystemDefaultDevice();
        queue = device ? send(device, "newCommandQueue") : NULL;
        if (!queue) {
            fprintf(stderr, "karouka_gl_interop_test: no Metal device\n");
            return 1;
        }
        system_desc.mtl_device = device;
        system_desc.mtl_queue = queue;
    }
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_gl_interop_test %s: engine failed to start\n", mode);
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = W, .height = H, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return 1;
    check_nothing_to_lock(view); /* before the first render */

    static unsigned char first[W * H * 4], second[W * H * 4];
    double clock_ms = 0;
    IOSurfaceRef first_surface = NULL, second_surface = NULL;
    render_and_compare(view, &gl, queue, &clock_ms, first_page, first, &first_surface);
    CHECK(pixel(first, W, 4, 1) == 0xFF0000FFu);   /* the top strip: row 0 is the page's top */
    CHECK(pixel(first, W, 4, 60) == 0xFFFF0000u);  /* bottom left */
    CHECK(pixel(first, W, 60, 60) == 0xFF008000u); /* bottom right */
    CHECK(pixel(first, W, 31, 31) == 0xFF000000u);
    CHECK(pixel(first, W, 32, 32) == 0xFF000000u);

    /* The same surface, still bound: GL reads the new frame without binding it again. */
    render_and_compare(view, &gl, queue, &clock_ms, second_page, second, &second_surface);
    CHECK(second_surface == first_surface);
    CHECK(pixel(second, W, 4, 60) == 0xFF008000u);
    CHECK(pixel(second, W, 60, 60) == 0xFFFF0000u);
    CHECK(pixel(second, W, 4, 1) == 0xFF0000FFu);

    /* A resize replaces the surface; the new one is bound again. */
    CHECK(kr_view_resize(view, RW, RH, 1) == KR_OK);
    check_nothing_to_lock(view); /* the old size's surface is gone until the next render */
    render_frames(view, &clock_ms, 3);
    wait_for_frame(queue);
    IOSurfaceRef resized = (IOSurfaceRef)kr_view_iosurface(view);
    CHECK(resized != NULL);
    if (resized) {
        CHECK(IOSurfaceGetWidth(resized) == RW && IOSurfaceGetHeight(resized) == RH);
        static unsigned char small[RW * RH * 4], small_cpu[RW * RH * 4];
        CHECK(gl_read(&gl, resized, small, RW, RH));
        CHECK(gl.bound == resized);
        CHECK(surface_read(resized, small_cpu, RW, RH));
        CHECK(memcmp(small, small_cpu, sizeof small) == 0);
        CHECK(pixel(small, RW, 4, 1) == 0xFF0000FFu);
        CHECK(pixel(small, RW, 4, 12) == 0xFF008000u);
        check_locked_frame(view, resized, small, RW, RH);
        if (queue)
            check_lock_with_host_texture(view, device, queue, &clock_ms, small, RW, RH);
    }

    FILE* file = fopen(out_path, "wb");
    CHECK(file != NULL);
    if (file) {
        CHECK(fwrite(first, 1, sizeof first, file) == sizeof first);
        CHECK(fwrite(second, 1, sizeof second, file) == sizeof second);
        fclose(file);
    }

    gl_destroy(&gl);
    kr_view_destroy(view);
    kr_system_destroy(system);
    objc_autoreleasePoolPop(pool);
    if (failures)
        fprintf(stderr, "karouka_gl_interop_test %s: %d check(s) failed\n", mode, failures);
    return failures ? 1 : 0;
}

/* ---- The parent: one child per backend, then the backends' readbacks compared. */

static int run_child(char const* mode, char const* out_path)
{
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return 1;
    char mode_arg[16], out_arg[256];
    snprintf(mode_arg, sizeof mode_arg, "%s", mode);
    snprintf(out_arg, sizeof out_arg, "%s", out_path);
    char* child_argv[] = { path, mode_arg, out_arg, NULL };
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, child_argv, environ) != 0) {
        fprintf(stderr, "karouka_gl_interop_test: can't run the %s case\n", mode);
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        fprintf(stderr, "karouka_gl_interop_test: the %s case crashed (status %d)\n", mode, status);
        return 1;
    }
    return WEXITSTATUS(status);
}

static int read_file(char const* path, unsigned char* out, size_t size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    size_t got = fread(out, 1, size, file);
    fclose(file);
    return got == size;
}

int main(int argc, char** argv)
{
    if (argc == 3)
        return run_backend(argv[1], argv[2]);

    char dir[] = "/tmp/karouka-gl-interop-XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "karouka_gl_interop_test: no temporary directory\n");
        return 1;
    }
    char const* const modes[] = { "metal", "hostqueue", "software" };
    enum { MODES = 3, FRAME = W * H * 4 };
    static unsigned char readback[MODES][2 * FRAME];
    char paths[MODES][256];
    int skipped = 0;
    for (int i = 0; i < MODES; ++i) {
        snprintf(paths[i], sizeof paths[i], "%s/%s.bgra", dir, modes[i]);
        int status = run_child(modes[i], paths[i]);
        if (status == 77) {
            skipped = 1;
            break;
        }
        if (status != 0) {
            fprintf(stderr, "karouka_gl_interop_test: the %s case failed\n", modes[i]);
            ++failures;
            continue;
        }
        CHECK(read_file(paths[i], readback[i], sizeof readback[i]));
        unlink(paths[i]);
    }
    rmdir(dir);
    if (skipped) {
        printf("karouka_gl_interop_test: skipped (no OpenGL)\n");
        return 0;
    }

    /* The backends agree: these pages have no anti-aliased edges, so to the byte. */
    if (!failures) {
        for (int i = 1; i < MODES; ++i) {
            int differing = 0;
            for (size_t p = 0; p < sizeof readback[0]; p += 4)
                differing += memcmp(readback[0] + p, readback[i] + p, 4) != 0;
            if (differing)
                fprintf(stderr, "karouka_gl_interop_test: %s differs from metal in %d pixel(s)\n", modes[i], differing);
            CHECK(differing == 0);
        }
    }

    if (failures)
        fprintf(stderr, "karouka_gl_interop_test: %d check(s) failed\n", failures);
    else
        printf("karouka_gl_interop_test: ok\n");
    return failures ? 1 : 0;
}
