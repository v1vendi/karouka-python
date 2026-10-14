/* kr_view_set_output2 refuses a host output that breaks a rule of kr_metal_output, as
 * validate_output in src/engine/HostMetal.mm checks them: each texture below is the accepted one
 * (64x64, 2D, BGRA8Unorm, one sample, render target) with one property changed, so the refusal
 * comes from that property. A texture without MTLTextureUsageRenderTarget, a multisample one and a
 * memoryless one, then the other rules: pixel format, a command buffer from another queue or
 * already committed, and objects that are not a command buffer and a texture. The legacy
 * kr_view_set_output validates the same way. Nothing changes on an error: a refused output leaves
 * the target set before it (the render draws into it, so the view has no IOSurface after it), or
 * none (the render draws into the IOSurface).
 * Needs the host's Metal device and queue (one system per process); memoryless textures need an
 * Apple GPU, so that case is skipped, with a message, on a GPU without them.
 * Built as karouka_output_texture_test (C, public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

/* For hosts built before struct_size: command_buffer and texture, 16 bytes. */
kr_result kr_view_set_output(kr_view* view, void const* output);

void* MTLCreateSystemDefaultDevice(void);
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);

/* Metal's enum values (MTLPixelFormat, MTLTextureType, MTLTextureUsage, MTLStorageMode). */
enum {
    PIXEL_FORMAT_RGBA8_UNORM = 70,
    PIXEL_FORMAT_BGRA8_UNORM = 80,
    TEXTURE_TYPE_2D = 2,
    TEXTURE_TYPE_2D_MULTISAMPLE = 4,
    USAGE_SHADER_READ = 1,
    USAGE_RENDER_TARGET = 4,
    STORAGE_PRIVATE = 2,
    STORAGE_MEMORYLESS = 3,
    GPU_FAMILY_APPLE1 = 1001,
};

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

typedef struct texture_desc {
    unsigned long pixel_format;
    unsigned long type;
    unsigned long samples;
    unsigned long usage;
    unsigned long storage;
} texture_desc;

/* What kr_metal_output accepts; each case changes one member. */
static texture_desc const accepted = { PIXEL_FORMAT_BGRA8_UNORM, TEXTURE_TYPE_2D, 1, USAGE_RENDER_TARGET | USAGE_SHADER_READ, STORAGE_PRIVATE };

static void* make_texture(void* device, texture_desc desc)
{
    void* descriptor = send((void*)objc_getClass("MTLTextureDescriptor"), "new");
    send_ulong(descriptor, "setPixelFormat:", desc.pixel_format);
    send_ulong(descriptor, "setTextureType:", desc.type);
    send_ulong(descriptor, "setWidth:", 64);
    send_ulong(descriptor, "setHeight:", 64);
    send_ulong(descriptor, "setSampleCount:", desc.samples);
    send_ulong(descriptor, "setUsage:", desc.usage);
    send_ulong(descriptor, "setStorageMode:", desc.storage);
    void* texture = ((void* (*)(void*, SEL, void*))objc_msgSend)(device, sel_registerName("newTextureWithDescriptor:"), descriptor);
    send_void(descriptor, "release");
    return texture;
}

static kr_result set_output(kr_view* view, void* command_buffer, void* texture)
{
    kr_metal_output output = { .struct_size = sizeof(kr_metal_output), .command_buffer = command_buffer, .texture = texture };
    return kr_view_set_output2(view, &output);
}

/* Renders a frame (the clock moves, so there is one) and commits command_buffer after it, as a host
 * does; returns whether the view drew into its IOSurface. */
static bool render_into_iosurface(kr_view* view, void* command_buffer)
{
    static double clock_ms;
    clock_ms += 16.0;
    bool rendered = false;
    CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
    CHECK(kr_view_render(view, &rendered) == KR_OK);
    CHECK(rendered);
    send_void(command_buffer, "commit");
    send_void(command_buffer, "waitUntilCompleted");
    return kr_view_iosurface(view) != NULL;
}

int main(void)
{
    void* pool = objc_autoreleasePoolPush();
    void* device = MTLCreateSystemDefaultDevice();
    void* queue = device ? send(device, "newCommandQueue") : NULL;
    void* other_queue = device ? send(device, "newCommandQueue") : NULL;
    if (!queue || !other_queue) {
        fprintf(stderr, "karouka_output_texture_test: no Metal device\n");
        return 1;
    }
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .mtl_device = device, .mtl_queue = queue };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_output_texture_test: engine failed to start on the host's queue\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 64, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view)
        return 1;
    static char const page[] = "<body style=\"background: teal\">";
    CHECK(kr_view_load_html(view, page, sizeof page - 1, "about:blank") == KR_OK);

    void* target = make_texture(device, accepted);
    void* commands = send(queue, "commandBuffer");
    CHECK(target && commands);
    if (!target || !commands)
        return 1;
    /* The accepted texture is accepted (the cases below differ from it in one property). */
    CHECK(set_output(view, commands, target) == KR_OK);
    CHECK(kr_view_set_output2(view, NULL) == KR_OK);

    /* The cases the task names: without MTLTextureUsageRenderTarget, multisample, memoryless. */
    texture_desc no_render_target = accepted;
    no_render_target.usage = USAGE_SHADER_READ;
    texture_desc multisample = accepted;
    multisample.type = TEXTURE_TYPE_2D_MULTISAMPLE;
    multisample.samples = 4;
    multisample.usage = USAGE_RENDER_TARGET;
    texture_desc memoryless = accepted;
    memoryless.usage = USAGE_RENDER_TARGET;
    memoryless.storage = STORAGE_MEMORYLESS;
    texture_desc rgba = accepted;
    rgba.pixel_format = PIXEL_FORMAT_RGBA8_UNORM;
    bool const apple_gpu = ((bool (*)(void*, SEL, long))objc_msgSend)(device, sel_registerName("supportsFamily:"), GPU_FAMILY_APPLE1);

    struct {
        char const* name;
        texture_desc desc;
        bool supported;
    } const cases[] = {
        { "no RenderTarget usage", no_render_target, true },
        { "multisample", multisample, true },
        { "memoryless", memoryless, apple_gpu },
        { "RGBA8Unorm", rgba, true },
    };
    void* refused_textures[sizeof cases / sizeof cases[0]] = { 0 };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (!cases[i].supported) {
            printf("karouka_output_texture_test: %s skipped (the GPU has no memoryless textures)\n", cases[i].name);
            continue;
        }
        void* texture = make_texture(device, cases[i].desc);
        refused_textures[i] = texture;
        if (!texture) {
            fprintf(stderr, "karouka_output_texture_test: Metal made no %s texture\n", cases[i].name);
            ++failures;
            continue;
        }
        if (set_output(view, commands, texture) != KR_ERROR_INVALID_ARGUMENT) {
            fprintf(stderr, "karouka_output_texture_test: %s texture accepted\n", cases[i].name);
            ++failures;
        }
        /* The legacy entry point (16 bytes: command_buffer, texture) refuses it too. */
        void* legacy[2] = { commands, texture };
        CHECK(kr_view_set_output(view, legacy) == KR_ERROR_INVALID_ARGUMENT);
    }

    /* The command buffer: one from another queue (not ordered after karouka's drawing), one already
     * committed. Then objects that are not what the members name: the two swapped, and the queue. */
    void* foreign = send(other_queue, "commandBuffer");
    void* committed = send(queue, "commandBuffer");
    send_void(committed, "commit");
    send_void(committed, "waitUntilCompleted");
    CHECK(set_output(view, foreign, target) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(set_output(view, committed, target) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(set_output(view, target, commands) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(set_output(view, commands, queue) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(set_output(view, NULL, target) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(set_output(view, commands, NULL) == KR_ERROR_INVALID_ARGUMENT);

    /* Nothing changes on an error. With no target set, a refused one leaves none: the render draws
     * into the IOSurface. */
    CHECK(set_output(view, commands, refused_textures[0]) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(render_into_iosurface(view, commands));
    /* With a target set, a refused one leaves it: the render draws into the host's texture. */
    commands = send(queue, "commandBuffer");
    CHECK(set_output(view, commands, target) == KR_OK);
    CHECK(set_output(view, commands, refused_textures[0]) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(!render_into_iosurface(view, commands));

    kr_view_destroy(view);
    kr_system_destroy(system);
    for (size_t i = 0; i < sizeof refused_textures / sizeof refused_textures[0]; ++i) {
        if (refused_textures[i])
            send_void(refused_textures[i], "release");
    }
    send_void(target, "release");
    objc_autoreleasePoolPop(pool);
    if (failures)
        fprintf(stderr, "karouka_output_texture_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
