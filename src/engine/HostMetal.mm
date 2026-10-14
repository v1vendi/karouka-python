/*
 * The host's Metal device and queue under LibGfx's SkiaBackendContext, and painting surfaces over
 * the host's textures. Compiled as Objective-C++ without ARC, like LibGfx's MetalContext.mm.
 */
#include "HostMetal.h"

#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>

#include <core/SkMilestone.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <core/SkCanvas.h>
#include <core/SkSurface.h>
#include "ShaderCache.h"

#import <Metal/Metal.h>

namespace Karouka {

namespace {

class HostMetalTexture final : public Gfx::MetalTexture {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit HostMetalTexture(id<MTLTexture> texture)
        : m_texture(texture)
    {
    }
    ~HostMetalTexture() override { [m_texture release]; }

    void const* texture() const override { return m_texture; }
    size_t width() const override { return m_texture.width; }
    size_t height() const override { return m_texture.height; }

private:
    id<MTLTexture> m_texture;
};

MTLPixelFormat pixel_format_of(Gfx::MetalTextureFormat format)
{
    switch (format) {
    case Gfx::MetalTextureFormat::BGRA8:
        return MTLPixelFormatBGRA8Unorm;
    case Gfx::MetalTextureFormat::R8:
        return MTLPixelFormatR8Unorm;
    case Gfx::MetalTextureFormat::RG8:
        return MTLPixelFormatRG8Unorm;
    case Gfx::MetalTextureFormat::R16:
        return MTLPixelFormatR16Unorm;
    case Gfx::MetalTextureFormat::RG16:
        return MTLPixelFormatRG16Unorm;
    }
    VERIFY_NOT_REACHED();
}

// LibGfx's MetalContextImpl (MetalContext.mm), over a device and queue the host made: karouka
// creates neither, so Skia's command buffers go to the host's queue, in order with the host's own.
class HostMetalContext final : public Gfx::MetalContext {
public:
    AK_ALLOC_WITH_KMALLOC;

    HostMetalContext(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : m_device([device retain])
        , m_queue([queue retain])
    {
    }
    ~HostMetalContext() override
    {
        [m_queue release];
        [m_device release];
    }

    void const* device() const override { return m_device; }
    void const* queue() const override { return m_queue; }

    OwnPtr<Gfx::MetalTexture> create_texture_from_iosurface(Core::IOSurfaceHandle const& iosurface, Gfx::MetalTextureFormat format, size_t plane) override
    {
        auto* descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.pixelFormat = pixel_format_of(format);
        descriptor.width = iosurface.plane_count() > 0 ? iosurface.plane_width(plane) : iosurface.width();
        descriptor.height = iosurface.plane_count() > 0 ? iosurface.plane_height(plane) : iosurface.height();
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor iosurface:(IOSurfaceRef)iosurface.core_foundation_pointer() plane:plane];
        [descriptor release];
        if (!texture)
            return {};
        return make<HostMetalTexture>(texture);
    }

private:
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
};

// LibGfx's SkiaMetalBackendContext (SkiaBackendContext.cpp), which isn't exported.
class SkiaMetalContext final : public Gfx::SkiaBackendContext {
public:
    AK_ALLOC_WITH_KMALLOC;

    SkiaMetalContext(sk_sp<GrDirectContext> context, NonnullRefPtr<Gfx::MetalContext> metal_context)
        : m_context(move(context))
        , m_metal_context(move(metal_context))
    {
    }
    ~SkiaMetalContext() override { m_context.reset(); }

    GrDirectContext* sk_context() const override { return m_context.get(); }
    Gfx::MetalContext& metal_context() override { return m_metal_context; }
    Gfx::VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullRefPtr<Gfx::MetalContext> m_metal_context;
};

}

RefPtr<Gfx::SkiaBackendContext> create_skia_metal_context(NonnullRefPtr<Gfx::MetalContext> metal_context, GrContextOptions::PersistentCache* shader_cache)
{
    GrMtlBackendContext backend_context;
    backend_context.fDevice.retain(metal_context->device());
    backend_context.fQueue.retain(metal_context->queue());
    GrContextOptions options;
    options.fPersistentCache = shader_cache;
    sk_sp<GrDirectContext> context = GrDirectContexts::MakeMetal(backend_context, options);
    if (!context)
        return nullptr;
    return adopt_ref(*new SkiaMetalContext(move(context), move(metal_context)));
}

ByteString shader_cache_stamp(Gfx::MetalContext const& metal_context)
{
    auto device = (id<MTLDevice>)metal_context.device();
    return ByteString::formatted("skia m{} {}", SK_MILESTONE, device.name.UTF8String);
}

RefPtr<Gfx::MetalContext> create_host_metal_context(void* device_handle, void* queue_handle)
{
    auto device = (id<MTLDevice>)device_handle;
    auto queue = (id<MTLCommandQueue>)queue_handle;
    if (![(id)device conformsToProtocol:@protocol(MTLDevice)] || ![(id)queue conformsToProtocol:@protocol(MTLCommandQueue)] || queue.device != device)
        return nullptr;
    return adopt_ref(*new HostMetalContext(device, queue));
}

kr_result validate_output(Gfx::MetalContext const& context, kr_metal_output const& output)
{
    auto command_buffer = (id<MTLCommandBuffer>)output.command_buffer;
    auto texture = (id<MTLTexture>)output.texture;
    if (![(id)command_buffer conformsToProtocol:@protocol(MTLCommandBuffer)] || ![(id)texture conformsToProtocol:@protocol(MTLTexture)])
        return KR_ERROR_INVALID_ARGUMENT;
    // The host's command buffer has to run after the Skia work kr_view_render submits, which only
    // the queue's order guarantees, and command_buffer.status says it has not been committed yet.
    if ((void const*)command_buffer.commandQueue != context.queue() || command_buffer.status != MTLCommandBufferStatusNotEnqueued)
        return KR_ERROR_INVALID_ARGUMENT;
    // Skia draws into it as a render target, and may read it back (a destination copy, a capture).
    if ((void const*)texture.device != context.device() || texture.textureType != MTLTextureType2D
        || texture.pixelFormat != MTLPixelFormatBGRA8Unorm || texture.framebufferOnly || texture.sampleCount != 1
        || !(texture.usage & MTLTextureUsageRenderTarget) || texture.storageMode == MTLStorageModeMemoryless)
        return KR_ERROR_INVALID_ARGUMENT;
    return KR_OK;
}

RefPtr<Gfx::PaintingSurface> create_output_surface(NonnullRefPtr<Gfx::SkiaBackendContext> context, kr_metal_output const& output)
{
    // The surface keeps its own reference (Skia's); this one goes with the wrapper.
    HostMetalTexture texture { [(id<MTLTexture>)output.texture retain] };
    return Gfx::PaintingSurface::create_from_metal_texture(texture, move(context));
}

GpuSetup set_up_gpu(RefPtr<Gfx::MetalContext> host_metal_context, char const* shader_cache_path, std::atomic<bool> const& main_thread_waiting)
{
    GpuSetup setup;
    @autoreleasepool {
        auto start = MonotonicTime::now();
        auto metal_context = host_metal_context ? host_metal_context : Gfx::get_metal_context();
        if (!metal_context)
            return setup;
        if (shader_cache_path && *shader_cache_path)
            setup.shader_cache = ShaderCache::open(shader_cache_path, shader_cache_stamp(*metal_context));
        setup.skia = create_skia_metal_context(metal_context.release_nonnull(), setup.shader_cache.ptr());
        if (!setup.skia)
            return setup;
        auto warm_up_start = MonotonicTime::now();
        setup.context_ms = (warm_up_start - start).to_microseconds() / 1000.0;

        auto* context = setup.skia->sk_context();
        // The programs the last run used first, so that the first frame finds them built.
        if (setup.shader_cache) {
            setup.shader_cache->precompile_warm_entries([&](SkData const& key, SkData const& data) {
                return context->precompileShader(key, data);
            });
        }
        // The context's first command buffer and render pass, and the driver work behind them
        // (~1-2 ms), here rather than in the first frame's flush: a small clear, submitted without
        // waiting. A clear is a load action, so it builds no program.
        auto info = SkImageInfo::Make(16, 16, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        if (auto surface = SkSurfaces::RenderTarget(context, skgpu::Budgeted::kNo, info, 1, kTopLeft_GrSurfaceOrigin, nullptr)) {
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            context->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
        }
        auto later_start = MonotonicTime::now();
        setup.warm_up_ms = (later_start - warm_up_start).to_microseconds() / 1000.0;
        // The programs the last run's next few frames used, for as long as the main thread is busy
        //. They come after the clear, so their pipeline builds queue behind the first
        // frame's; Metal finishes them long before a second frame.
        if (setup.shader_cache) {
            auto before = setup.shader_cache->precompiled_count();
            setup.shader_cache->precompile_later_entries([&](SkData const& key, SkData const& data) {
                return context->precompileShader(key, data);
            },
                main_thread_waiting);
            setup.later_precompiled = setup.shader_cache->precompiled_count() - before;
        }
        setup.later_ms = (MonotonicTime::now() - later_start).to_microseconds() / 1000.0;
    }
    return setup;
}

}
