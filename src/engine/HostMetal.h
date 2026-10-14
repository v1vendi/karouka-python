/*
 * The host's Metal device and command queue as LibGfx's MetalContext, and the painting surface a
 * view draws into a host texture through (kr_system_desc.mtl_device/mtl_queue, kr_view_set_output).
 */
#pragma once

#include <atomic>

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/MetalContext.h>
#include <gpu/ganesh/GrContextOptions.h>
#include <karouka/karouka.h>

#include "ShaderCache.h"

namespace Gfx {
class PaintingSurface;
class SkiaBackendContext;
}

namespace Karouka {

// A MetalContext over the host's id<MTLDevice> and id<MTLCommandQueue>, retained until it goes.
// NULL if the queue is not one of the device's.
RefPtr<Gfx::MetalContext> create_host_metal_context(void* device, void* queue);

// KR_OK if `output` can be drawn into on `context`'s queue: a command buffer of that queue not yet
// enqueued, and a 2D BGRA8Unorm single-sample render target texture of its device that is not
// framebufferOnly or memoryless.
kr_result validate_output(Gfx::MetalContext const& context, kr_metal_output const& output);

// A painting surface over output.texture (validated), the size of the texture, which keeps the
// texture alive while it lives. NULL if Skia can't wrap it.
// The process's Skia context on `metal_context`'s device and queue: LibGfx's
// SkiaBackendContext::create_metal_context, with Skia's persistent shader cache when there is one
// (ShaderCache.h, kr_system_desc.shader_cache_path). LibGfx makes its GrDirectContext with
// default options, so karouka makes its own and wraps it the same way.
RefPtr<Gfx::SkiaBackendContext> create_skia_metal_context(NonnullRefPtr<Gfx::MetalContext> metal_context, GrContextOptions::PersistentCache* shader_cache);

// What a persistent shader cache written on this device by this build of Skia is valid for: Skia's
// milestone and the GPU's name. A cache file written under another stamp is not read.
ByteString shader_cache_stamp(Gfx::MetalContext const&);

// The process's GPU context, made on kr_system_create2's GPU thread while the main thread sets up
// the VM: the Metal device and queue (the host's when given), the persistent shader cache
// at shader_cache_path (none when NULL or ""), the Skia context over them, then a warm-up so that the
// first frame's flush doesn't pay for first use: the cache's warm list precompiled (ShaderCache.h)
// and a small clear submitted; then the cache's later list precompiled until `main_thread_waiting`
// is set (the main thread sets it when it is about to join). No skia when there is no Metal
// device or Skia can't use it. Touches nothing of the main thread's, and neither the host's clock
// nor its log, which need not be thread-safe; the main thread logs the times once it has joined.
struct GpuSetup {
    OwnPtr<ShaderCache> shader_cache;
    RefPtr<Gfx::SkiaBackendContext> skia;
    double context_ms { 0 };
    double warm_up_ms { 0 };
    double later_ms { 0 };
    size_t later_precompiled { 0 };
};
GpuSetup set_up_gpu(RefPtr<Gfx::MetalContext> host_metal_context, char const* shader_cache_path, std::atomic<bool> const& main_thread_waiting);

RefPtr<Gfx::PaintingSurface> create_output_surface(NonnullRefPtr<Gfx::SkiaBackendContext>, kr_metal_output const& output);

}
