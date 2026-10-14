/*
 * Canvas 2D in-process. Ladybird replays a canvas's recorded commands in its Compositor process
 * (Services/Compositor/CanvasHost); karouka has no compositor, so this host replays them on the
 * main thread with the system's Skia context (or Skia's CPU rasterizer), keeps each canvas's last
 * presented pixels in a CanvasSurfaceRegistry that kr_view_render hands its player for DrawCanvas,
 * and reads pixels back for getImageData/toDataURL (patch 0090, Web::Embedder::LocalCanvas2DHost).
 * It also keeps the placeholder canvases of transferControlToOffscreen(): each rendering update
 * commits a copy of its OffscreenCanvas's presented pixels under the placeholder's id (patch 0094).
 */
#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/CanvasSurfaceRegistry.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/CanvasCommandPlayer.h>
#include <LibWeb/Embedder/LocalCanvas.h>
#include <LibWeb/Forward.h>

namespace Karouka {

class LocalCanvasHost final
    : public RefCounted<LocalCanvasHost>
    , public Web::Embedder::LocalCanvas2DHost {
public:
    static NonnullRefPtr<LocalCanvasHost> create(RefPtr<Gfx::SkiaBackendContext> skia) { return adopt_ref(*new LocalCanvasHost(move(skia))); }

    // LocalCanvas2DHost
    virtual RefPtr<Web::HTML::RemoteCanvas2DTransport> create_canvas_2d_transport(Web::Page const&) override;
    virtual void flush_canvas_2d_stream() override;
    virtual Optional<Web::Compositor::PlaceholderCanvasLink> allocate_placeholder_canvas(Web::Page const&) override;
    virtual void release_placeholder_canvas(Compositing::CanvasId) override;
    virtual void commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean) override;
    virtual Web::Compositor::PlaceholderCanvasPixels read_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect) override;

    Optional<Compositing::CanvasId> create_context(Gfx::IntSize, bool alpha, Web::Page const&);
    void destroy_context(Compositing::CanvasId);

    // Drops the surfaces of every canvas of the page (kr_view_destroy), and its
    // placeholders, without waiting for the GC to finalize its 2D contexts and canvas elements.
    // Their transports keep the dead ids: drawing into them is dropped, readbacks find nothing,
    // commits are ignored and destroying or releasing them later does nothing.
    void destroy_contexts_of(Web::Page const&);
    RefPtr<Gfx::Bitmap> read_back_pixels(Compositing::CanvasId, Gfx::IntRect);
    Compositing::Canvas2DCommandStream& stream() { return *m_stream; }

    // The context's origin-clean flag went false (Canvas2DContextBase::set_not_origin_clean, patch 0110): its
    // presented pixels are script-unreadable from now on, as long as it lives.
    void set_not_origin_clean(Compositing::CanvasId);
    // A canvas a shader() samples (ShaderImages::CanvasInput): its presented surface, content generation and
    // origin-clean flag (a placeholder's from its last commit). A canvas id with no context is not origin-clean.
    struct CanvasInput {
        Gfx::PaintingSurface const* surface { nullptr };
        u64 content_generation { 0 };
        bool origin_clean { false };
    };
    CanvasInput canvas_input(Compositing::CanvasId) const;

    // What kr_view_render's player resolves DrawCanvas against.
    Compositing::CanvasSurfaceRegistry const& registry() const { return m_registry; }

    // Pixel bytes of every live canvas's two surfaces, its draw surface and its presented copy, and
    // of every committed placeholder's copy, at 4 bytes a pixel (kr_view_stats.canvas_bytes, KR_MEM_TAG_CANVAS). Metal textures with a Skia
    // context, kmalloc'd bitmaps on Skia's CPU rasterizer.
    size_t surface_bytes() const;

    // Drops every canvas's surfaces (before the Skia context goes); later calls make nothing.
    void shut_down();

private:
    static RefPtr<Gfx::Bitmap> read_back_surface(Gfx::PaintingSurface&, Gfx::IntRect);

    explicit LocalCanvasHost(RefPtr<Gfx::SkiaBackendContext> skia)
        : m_skia(move(skia))
        , m_stream(adopt_ref(*new Compositing::Canvas2DCommandStream()))
    {
    }

    struct Context {
        NonnullOwnPtr<Gfx::CanvasCommandPlayer> player;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
        bool has_unpresented_commands { false };
        // Cleared once the context draws an image that isn't origin-clean; never set again.
        bool origin_clean { true };
        // The page LibWeb made the canvas's transport for (patch 0098), an identity never
        // dereferenced; set whether or not a script was running when the backing storage was made.
        Web::Page const* page { nullptr };
    };

    struct Placeholder {
        u64 secret { 0 };
        // The last committed copy of the OffscreenCanvas's presented pixels; null before the first
        // commit and after one from an OffscreenCanvas without a 2D context.
        RefPtr<Gfx::PaintingSurface> surface;
        bool origin_clean { true };
        Web::Page const* page { nullptr };
    };

    RefPtr<Gfx::SkiaBackendContext> m_skia;
    bool m_shut_down { false };
    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_stream;
    Compositing::CanvasSurfaceRegistry m_registry;
    HashMap<Compositing::CanvasId, Context> m_contexts;
    HashMap<Compositing::CanvasId, Placeholder> m_placeholders;
    Compositing::DisplayListResourceStorage m_text_resources;
};

}
