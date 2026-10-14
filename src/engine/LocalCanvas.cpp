/*
 * Canvas 2D in-process; see LocalCanvas.h. The replay follows Ladybird's Compositor CanvasHost
 * (2D contexts only): one command stream for every canvas, replayed strictly in recording order so
 * a DrawCanvas sees exactly the commands recorded before it.
 */
#include "LocalCanvas.h"

#include <AK/Random.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <core/SkTextBlob.h>

namespace Karouka {

namespace {

class LocalCanvas2DTransport final : public Web::HTML::RemoteCanvas2DTransport {
public:
    LocalCanvas2DTransport(NonnullRefPtr<LocalCanvasHost> host, Web::Page const& page)
        : m_host(move(host))
        , m_page(&page)
    {
    }

    virtual ~LocalCanvas2DTransport() override { destroy_context(); }

private:
    virtual bool create_context(Gfx::IntSize size, bool alpha) override
    {
        VERIFY(!m_canvas_id.has_value());
        m_canvas_id = m_host->create_context(size, alpha, *m_page);
        return m_canvas_id.has_value();
    }

    virtual Optional<Compositing::CanvasId> canvas_id() const override { return m_canvas_id; }

    virtual void destroy_context() override
    {
        if (!m_canvas_id.has_value())
            return;
        m_host->destroy_context(*m_canvas_id);
        m_canvas_id.clear();
    }

    virtual void did_become_not_origin_clean() override
    {
        if (m_canvas_id.has_value())
            m_host->set_not_origin_clean(*m_canvas_id);
    }

    virtual Compositing::Canvas2DCommandStream& shared_stream() override { return m_host->stream(); }
    virtual void flush_shared_stream() override { m_host->flush_canvas_2d_stream(); }

    virtual RefPtr<Gfx::Bitmap> read_back_pixels(Gfx::IntRect const& rect) override
    {
        if (!m_canvas_id.has_value())
            return nullptr;
        return m_host->read_back_pixels(*m_canvas_id, rect);
    }

    NonnullRefPtr<LocalCanvasHost> m_host;
    // The page LibWeb made the transport for: an identity, never dereferenced.
    Web::Page const* m_page { nullptr };
    Optional<Compositing::CanvasId> m_canvas_id;
};

}

RefPtr<Web::HTML::RemoteCanvas2DTransport> LocalCanvasHost::create_canvas_2d_transport(Web::Page const& page)
{
    if (m_shut_down)
        return nullptr;
    return adopt_ref(*new LocalCanvas2DTransport(*this, page));
}

Optional<Compositing::CanvasId> LocalCanvasHost::create_context(Gfx::IntSize size, bool alpha, Web::Page const& page)
{
    if (m_shut_down || size.is_empty() || static_cast<i64>(size.width()) * size.height() > Gfx::max_canvas_area)
        return {};

    auto format = alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto canvas_surface_resolver = [this](u64 canvas_id) -> Gfx::PaintingSurface const* {
        // A source canvas resolves to its live draw surface: the stream replays in recording order,
        // so it holds exactly the commands recorded before the DrawCanvas.
        auto it = m_contexts.find(Compositing::CanvasId { canvas_id });
        if (it == m_contexts.end())
            return nullptr;
        return it->value.player->surface().ptr();
    };
    auto text_blob_resolver = [this](u64 font_id, ReadonlySpan<Gfx::CanvasGlyph> glyphs) {
        if (!m_text_resources.has_font(Compositing::FontResourceId { font_id }))
            return sk_sp<SkTextBlob> {};
        Vector<Compositing::DisplayListGlyph> display_list_glyphs;
        display_list_glyphs.ensure_capacity(glyphs.size());
        for (auto const& glyph : glyphs)
            display_list_glyphs.unchecked_append({ .position = glyph.position, .glyph_id = glyph.glyph_id });
        return m_text_resources.text_blob(Compositing::FontResourceId { font_id }, 1, display_list_glyphs, 0, Compositing::TextRasterizationMode::Unhinted);
    };
    auto player = make<Gfx::CanvasCommandPlayer>(m_skia, size, format, Gfx::AlphaType::Premultiplied, move(canvas_surface_resolver), move(text_blob_resolver));

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // A context without alpha starts opaque black; Skia's surfaces start transparent.
    if (!alpha)
        player->clear(Gfx::Color::Black);

    auto presented_surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia);
    presented_surface->copy_from_surface(player->surface());
    auto canvas_id = m_registry.create_canvas_surface(presented_surface);
    m_contexts.set(canvas_id, Context { .player = move(player), .presented_surface = move(presented_surface), .page = &page });
    return canvas_id;
}

void LocalCanvasHost::destroy_context(Compositing::CanvasId canvas_id)
{
    if (m_contexts.remove(canvas_id))
        m_registry.remove_canvas_surface(canvas_id);
}

void LocalCanvasHost::destroy_contexts_of(Web::Page const& page)
{
    Vector<Compositing::CanvasId> canvas_ids;
    for (auto const& it : m_contexts) {
        if (it.value.page == &page)
            canvas_ids.append(it.key);
    }
    for (auto canvas_id : canvas_ids)
        destroy_context(canvas_id);

    Vector<Compositing::CanvasId> placeholder_ids;
    for (auto const& it : m_placeholders) {
        if (it.value.page == &page)
            placeholder_ids.append(it.key);
    }
    for (auto canvas_id : placeholder_ids)
        release_placeholder_canvas(canvas_id);
}

void LocalCanvasHost::flush_canvas_2d_stream()
{
    if (m_stream->is_empty())
        return;
    auto segments = m_stream->take_segments();
    auto fonts = m_stream->take_fonts();
    if (m_shut_down)
        return;

    Compositing::DisplayListResourceSet resources;
    for (auto const& font : fonts) {
        // Font ids are immutable; keep the storage cached text blobs refer to.
        if (!m_text_resources.has_font(font.id))
            m_text_resources.set_font(font.id, font.font);
        resources.fonts.set(font.id);
    }
    m_text_resources.retain_only(resources);

    for (auto const& segment : segments) {
        // The canvas may have been destroyed while its segment waited in the stream.
        auto it = m_contexts.find(segment.canvas_id);
        if (it == m_contexts.end())
            continue;
        auto& context = it->value;
        if (!segment.commands.is_empty()) {
            context.player->play(segment.commands);
            context.has_unpresented_commands = true;
        }
        if (segment.present && context.has_unpresented_commands) {
            context.presented_surface->copy_from_surface(context.player->surface());
            // Bumps the canvas's content generation in the registry, as the compositor does.
            m_registry.set_canvas_surface(segment.canvas_id, context.presented_surface);
            context.has_unpresented_commands = false;
        }
    }
}

void LocalCanvasHost::set_not_origin_clean(Compositing::CanvasId canvas_id)
{
    if (auto it = m_contexts.find(canvas_id); it != m_contexts.end())
        it->value.origin_clean = false;
}

LocalCanvasHost::CanvasInput LocalCanvasHost::canvas_input(Compositing::CanvasId canvas_id) const
{
    auto const* surface = m_registry.canvas_surface(canvas_id);
    auto generation = m_registry.canvas_content_generation(canvas_id);
    if (auto it = m_contexts.find(canvas_id); it != m_contexts.end())
        return { surface, generation, it->value.origin_clean };
    if (auto it = m_placeholders.find(canvas_id); it != m_placeholders.end())
        return { surface, generation, it->value.origin_clean };
    return { nullptr, generation, false };
}

Optional<Web::Compositor::PlaceholderCanvasLink> LocalCanvasHost::allocate_placeholder_canvas(Web::Page const& page)
{
    if (m_shut_down)
        return {};
    // The registry's ids are shared with the 2D contexts, so a placeholder never aliases a canvas.
    auto canvas_id = m_registry.allocate_canvas_id();
    auto secret = get_random<u64>();
    m_placeholders.set(canvas_id, Placeholder { .secret = secret, .surface = nullptr, .origin_clean = true, .page = &page });
    return Web::Compositor::PlaceholderCanvasLink { canvas_id, secret };
}

void LocalCanvasHost::release_placeholder_canvas(Compositing::CanvasId canvas_id)
{
    if (m_placeholders.remove(canvas_id))
        m_registry.remove_canvas_surface(canvas_id);
}

void LocalCanvasHost::commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink link, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean)
{
    // As Compositor's CompositorState::commit_placeholder_canvas, with the source canvas's
    // presented surface (the caller flushed its present marker just before).
    auto it = m_placeholders.find(link.canvas_id);
    if (it == m_placeholders.end() || it->value.secret != link.secret)
        return;
    auto& placeholder = it->value;

    RefPtr<Gfx::PaintingSurface> source_surface;
    if (source_canvas_id.has_value()) {
        auto context = m_contexts.find(*source_canvas_id);
        // A source whose context was dropped keeps the last committed frame, as the compositor does.
        if (context == m_contexts.end())
            return;
        source_surface = context->value.presented_surface;
    }
    placeholder.origin_clean = origin_clean;
    if (!source_surface) {
        placeholder.surface = nullptr;
        m_registry.remove_canvas_surface(link.canvas_id);
        return;
    }
    if (!placeholder.surface || placeholder.surface->size() != source_surface->size())
        placeholder.surface = Gfx::PaintingSurface::create_with_size(source_surface->size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia);
    placeholder.surface->copy_from_surface(*source_surface);
    // Bumps the placeholder's content generation in the registry.
    m_registry.set_canvas_surface(link.canvas_id, *placeholder.surface);
}

Web::Compositor::PlaceholderCanvasPixels LocalCanvasHost::read_placeholder_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto it = m_placeholders.find(canvas_id);
    if (it == m_placeholders.end())
        return {};
    auto& placeholder = it->value;
    if (!placeholder.surface)
        return { .bitmap = nullptr, .origin_clean = placeholder.origin_clean };
    return { .bitmap = read_back_surface(*placeholder.surface, rect), .origin_clean = placeholder.origin_clean };
}

RefPtr<Gfx::Bitmap> LocalCanvasHost::read_back_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto it = m_contexts.find(canvas_id);
    if (it == m_contexts.end())
        return nullptr;
    return read_back_surface(*it->value.player->surface(), rect);
}

RefPtr<Gfx::Bitmap> LocalCanvasHost::read_back_surface(Gfx::PaintingSurface& surface, Gfx::IntRect rect)
{
    auto clipped_rect = rect.intersected(surface.rect());
    if (clipped_rect.is_empty())
        return nullptr;
    auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, clipped_rect.size());
    if (bitmap_or_error.is_error())
        return nullptr;
    auto bitmap = bitmap_or_error.release_value();
    surface.flush();
    surface.read_into_bitmap(*bitmap, clipped_rect.location());
    return bitmap;
}

size_t LocalCanvasHost::surface_bytes() const
{
    auto bytes_of = [](Gfx::PaintingSurface const& surface) {
        return static_cast<size_t>(surface.size().width()) * surface.size().height() * 4;
    };
    size_t total = 0;
    for (auto const& it : m_contexts)
        total += bytes_of(*it.value.player->surface()) + bytes_of(*it.value.presented_surface);
    for (auto const& it : m_placeholders) {
        if (it.value.surface)
            total += bytes_of(*it.value.surface);
    }
    return total;
}

void LocalCanvasHost::shut_down()
{
    m_shut_down = true;
    (void)m_stream->take_segments();
    (void)m_stream->take_fonts();
    for (auto canvas_id : m_contexts.keys())
        m_registry.remove_canvas_surface(canvas_id);
    m_contexts.clear();
    for (auto canvas_id : m_placeholders.keys())
        m_registry.remove_canvas_surface(canvas_id);
    m_placeholders.clear();
    m_text_resources.retain_only({});
    m_skia = nullptr;
}

}
