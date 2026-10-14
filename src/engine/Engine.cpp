/*
 * The karouka engine: one process, one thread, no IPC. Ladybird's LibWeb parses, styles, lays out
 * and runs script; its Skia display-list player rasterizes into an IOSurface the host samples.
 *
 * What Ladybird's WebContent process gets from other processes, karouka does locally:
 *   - navigation (the UI process's session history)  -> Page::load builds navigation params here
 *   - image decoding (ImageDecoder process)           -> LocalImageCodecPlugin
 *   - compositing (Compositor process)                -> render(): record + DisplayListPlayerSkia
 *   - networking (RequestServer)                      -> none; ResourceLoader runs without a client
 */

#include "BindingDom.h"
#include "BindingModel.h"
#include "Dependencies.h"
#include "DisplayListCensus.h"
#include "EngineChannel.h"
#include "HostEventLoop.h"
#include "HostFonts.h"
#include "HostInterface.h"
#include "HostMetal.h"
#include "ScriptTypes.h"
#include "ShaderCache.h"
#include "ShaderImage.h"
#include "TextBlobCache.h"
#include "SoftwareSurface.h"
#include "StructSize.h"
#include "SystemUIFontProvider.h"
#include "WebFontGlyphs.h"
#include "LocalCanvas.h"
#include "LocalImageCodecPlugin.h"

#include <AK/LexicalPath.h>
#include <AK/Math.h>
#include <AK/Time.h>
#include <thread>
#include <karouka/karouka.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>
#include <LibCompositing/DisplayList/DisplayListPlayerSkia.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/IOSurface.h>
#include <LibCore/Platform/ScopedAutoreleasePool.h>
#include <LibCore/Resource.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibCore/System.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/SkiaBackendContext.h>
#include <core/SkCanvas.h>
#include <core/SkGraphics.h>
#include <core/SkRegion.h>
#include <LibGfx/SystemTheme.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/HeapRegion.h>
#include <LibGC/PrimitiveStorage.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/URL.h>
#include <LibThreading/ThreadPool.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Embedder/WebFonts.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Embedder/FileFetch.h>
#include <LibWeb/Embedder/HostClock.h>
#include <LibWeb/Embedder/InPlaceLoad.h>
#include <LibWeb/Embedder/ScriptTypes.h>
#include <LibWeb/Embedder/ChildNavigables.h>
#include <LibWeb/Embedder/ShaderDecodedImageData.h>
#include <LibWeb/Embedder/StageThreads.h>
#include <LibWeb/Embedder/SystemFonts.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

namespace Web::Bindings {
class FocusEventPrototype;
}

#ifndef KAROUKA_DEFAULT_RESOURCE_ROOT
#    error "KAROUKA_DEFAULT_RESOURCE_ROOT must point at Ladybird's Base/res"
#endif

namespace Karouka {

// Why a view renders its next frame (KR_DEBUG logs them with each render, and their totals when the
// view is destroyed). Bits, since one frame can have several.
enum RenderReason : u8 {
    RenderReasonLoad = 1 << 0,              // a document was loaded into the view
    RenderReasonResize = 1 << 1,            // kr_view_resize
    RenderReasonPaint = 1 << 2,             // LibWeb painted the page (collect_paints)
    RenderReasonVisualAnimation = 1 << 3,   // an on-screen visual-context-tree animation runs
    RenderReasonVisualAnimationEnd = 1 << 4, // the frame after one stopped or left the viewport
    RenderReasonTarget = 1 << 5,            // the render draws into another surface than the last (kr_view_set_output)
    RenderReasonCaretBlink = 1 << 6,        // the last frame's blinking caret has shown or hidden since
    RenderReasonShader = 1 << 7,            // the last frame drew live embedder content and the host's time moved since
    RenderReasonCount = 8,
};

static constexpr StringView render_reason_names[RenderReasonCount] = { "load"sv, "resize"sv, "paint"sv, "visual-animation"sv, "visual-animation-end"sv, "target"sv, "caret-blink"sv, "shader"sv };

// Fires a trusted FocusEvent named `name` at `target` as LibWeb's fire_a_focus_event does: no
// relatedTarget, `view` the target's window, composed. LibWeb doesn't export FocusEvent::create, so
// the event comes from the realm's FocusEvent interface object (not whatever the page left in
// window.FocusEvent), with an init dictionary that has no prototype for the page to have changed.
static void fire_focus_event(Web::DOM::EventTarget& target, Web::HTML::Window& window, Utf16FlyString const& name, bool bubbles)
{
    auto& realm = Web::HTML::relevant_realm(window);
    auto& vm = realm.vm();
    GC::Ptr<Web::DOM::Event> event;
    {
        Web::HTML::TemporaryExecutionContext context(realm);
        auto& constructor = Web::Bindings::host_defined_intrinsics(realm).ensure_web_constructor<Web::Bindings::FocusEventPrototype>("FocusEvent"_utf16_fly_string);
        auto init = JS::Object::create(realm, nullptr);
        MUST(init->create_data_property_or_throw(JS::PropertyKey { "view"_utf16_fly_string }, &window.window()->object()));
        MUST(init->create_data_property_or_throw(JS::PropertyKey { "relatedTarget"_utf16_fly_string }, JS::js_null()));
        MUST(init->create_data_property_or_throw(JS::PropertyKey { "bubbles"_utf16_fly_string }, JS::Value(bubbles)));
        MUST(init->create_data_property_or_throw(JS::PropertyKey { "composed"_utf16_fly_string }, JS::Value(true)));
        auto result = JS::construct(vm, constructor, JS::PrimitiveString::create(vm, name.to_utf16_string()), init);
        if (result.is_error())
            return;
        event = Web::Bindings::impl_from<Web::DOM::Event>(result.value().ptr());
    }
    if (!event)
        return;
    event->set_is_trusted(true);
    target.dispatch_event(*event);
}

// The events of the host window's focus change (kr_view_set_focused) along `chain`, the focus
// chain of the currently focused area (Web::HTML::focus_chain): losing focus, each entry from the
// focused area up gets blur; gaining it, each entry from the top down gets focus. An element also
// gets focusout or focusin, a document's window blur or focus alone, and the focused area stays
// designated (document.activeElement doesn't change). LibWeb's own focus update steps would
// undesignate and redesignate it, which commits a text field and drops the selection. Within one
// document this matches Chrome and Firefox (focus).
static void fire_window_focus_change(Vector<GC::Root<Web::DOM::Node>> const& chain, bool has_focus)
{
    auto fire_at = [&](Web::DOM::Node& entry) {
        auto& window = Web::HTML::relevant_window(entry);
        if (entry.is_document()) {
            fire_focus_event(window, window, has_focus ? "focus"_utf16_fly_string : "blur"_utf16_fly_string, false);
            return;
        }
        if (!entry.is_element())
            return;
        fire_focus_event(entry, window, has_focus ? "focus"_utf16_fly_string : "blur"_utf16_fly_string, false);
        fire_focus_event(entry, window, has_focus ? "focusin"_utf16_fly_string : "focusout"_utf16_fly_string, true);
    };
    if (has_focus) {
        for (auto const& entry : chain.in_reverse())
            fire_at(*entry);
    } else {
        for (auto const& entry : chain)
            fire_at(*entry);
    }
}

class PageClient final : public Web::PageClient {
    GC_CELL(PageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(PageClient);

public:
    virtual ~PageClient() override = default;

    void set_page(GC::Ref<Web::Page> page) { m_page = page; }
    void set_viewport(Web::DevicePixelSize size, double scale)
    {
        m_viewport = size;
        m_scale = scale;
    }

    // The RenderReason bits set since the last render; 0 when the page need not render.
    u8 take_render_reasons() { return exchange(m_render_reasons, 0); }
    void set_needs_render(RenderReason reason) { m_render_reasons |= reason; }

    // Whether the rendering update that just ran may have painted this page. One update services
    // every page in the process, and its paint counter is process-wide, so each page tracks its
    // own requests: LibWeb calls request_frame() whenever a page's local root needs a repaint
    // (Document::request_frame_for_pending_repaint), and for rAF, observers and input. A request
    // that is still pending (the page's traversable still needs a repaint) carries over to the next
    // update. Conservative: a page that requested a frame and was not repainted renders once more.
    bool take_painted()
    {
        if (!m_repaint_requested)
            return false;
        m_repaint_requested = m_page && m_page->has_local_traversable() && m_page->local_traversable()->needs_repaint();
        return true;
    }

    // Ids must be unique across every page in the process: LibWeb finds navigables by id in one
    // process-wide set (local_navigable_with_id), and the history executor resolves its steps
    // through it. Ladybird's UI process hands each WebContent page its own namespace; here each
    // page's id is its namespace.
    virtual Web::PageId id() const override { return Web::PageId { m_id }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return Gfx::Palette(*m_palette_impl); }
    virtual Web::DevicePixelRect screen_rect() const override { return { {}, m_viewport }; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return m_scale; }
    virtual double device_pixels_per_css_pixel() const override { return m_scale; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Light; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::NoPreference; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { return m_input_event_queue; }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { m_id, m_next_cross_process_id++ }; }
    virtual bool is_headless() const override { return false; }

    virtual void request_frame() override
    {
        m_repaint_requested = true;
        Web::HTML::main_thread_event_loop().request_rendering_update();
    }

    // The host window's focus (kr_view_set_focused). Unfocused, LibWeb paints selections in the
    // palette's InactiveSelection, draws no caret or focus ring, and document.hasFocus() is false.
    virtual bool has_focus() const override { return m_has_focus; }

    // WebContent's PageClient::set_has_focus: everything painted depends on the focus state, so the
    // documents the page hosts drop their cached paint. Unlike there, the window focus change also
    // fires blur or focus (fire_window_focus_change) in a user-interaction task of the top-level
    // document. The focus chain is taken while the page has focus (currently_focused_area is null
    // without it): before the change when losing focus, after it when gaining focus.
    virtual void set_has_focus(bool has_focus) override
    {
        if (m_has_focus == has_focus || !m_page)
            return;
        m_page->invalidate_compositor_keyboard_scroll_state();
        GC::Ptr<Web::HTML::LocalTraversableNavigable> traversable;
        if (m_page->has_top_level_traversable())
            traversable = as_if<Web::HTML::LocalTraversableNavigable>(*m_page->top_level_traversable());
        Vector<GC::Root<Web::DOM::Node>> focus_chain;
        if (traversable && !has_focus)
            focus_chain = Web::HTML::focus_chain(traversable->currently_focused_area());
        m_has_focus = has_focus;
        if (traversable && has_focus)
            focus_chain = Web::HTML::focus_chain(traversable->currently_focused_area());
        if (auto document = traversable ? traversable->active_document() : nullptr; document && !focus_chain.is_empty()) {
            Web::HTML::queue_global_task(Web::HTML::Task::Source::UserInteraction, Web::HTML::relevant_realm(*document).global_object(),
                GC::create_function(document->heap(), [focus_chain = move(focus_chain), has_focus] {
                    fire_window_focus_change(focus_chain, has_focus);
                }));
        }
        for (auto const& navigable : m_page->hosted_navigables()) {
            auto document = navigable->active_document();
            if (!document)
                continue;
            if (has_focus && navigable->is_local_root())
                document->reset_cursor_blink_cycle();
            // Focus can change while layout is invalidated; only the current paintable tree's cached
            // paint is dropped, without bringing layout up to date first.
            if (document->has_committed_viewport_box())
                document->paint_state().invalidate_all_cached_paint(*document);
        }
        request_frame();
    }


    virtual void request_file(Web::FileRequest request) override
    {
        record_dependency(request);
        // With a host io, the host reads the file (kr_host_io); the page's loads are cancelled
        // with the view, which owns this client.
        if (Karouka::HostIo::is_routed()) {
            Karouka::HostIo::request_file(move(request), this);
            return;
        }
        auto file = Core::File::open(request.path(), Core::File::OpenMode::Read);
        if (file.is_error()) {
            request.on_file_request_finish(file.release_error());
            return;
        }
        request.on_file_request_finish(file.value()->leak_fd());
    }

    // Ladybird hands navigation to the browser process's session history. karouka has none: a link
    // or location change of the top-level document to a file: URL reloads the view with that page;
    // anything else is refused. There are no child navigables to navigate: karouka turns them off
    // (patch 0100, Embedder/ChildNavigables), so an iframe stays an empty box.
    virtual void request_navigation_start(Web::HTML::LocalNavigable&, Web::NavigationTarget target, URL::URL const& url, Utf16String, Optional<Web::HTML::NavigationStartRequest>) override
    {
        if (target == Web::NavigationTarget::IFrame)
            return;
        // The root keeps the client alive until the deferred call runs; a view destroyed meanwhile
        // has cleared on_navigate.
        Core::deferred_invoke([client = GC::make_root(*this), url] {
            if (client->on_navigate)
                client->on_navigate(url);
        });
    }
    // Only a child navigable asks for these; karouka loads its top-level documents in place.
    virtual void request_navigation_population(Web::HTML::LocalNavigable&, Web::NavigationTarget, Web::HTML::NavigationPopulationRequest) override { }
    virtual void navigation_params_creation_finished(Web::HTML::LocalNavigable&, Web::HTML::NavigationPopulationRequest, Web::HTML::NavigationPopulationResult) override { }
    virtual void history_navigation_params_creation_finished(Web::HTML::CrossProcessId, Web::HTML::HistoryNavigationPopulation) override { }

    // The current document's dependency set (kr_view_get_dependencies); each load starts a new one.
    NonnullRefPtr<Karouka::DependencySet> dependencies { Karouka::DependencySet::create() };

    // Lists the file in the dependency set, and its outcome once the request finishes. The URL is
    // the one HostIo::request_file hands kr_host_io.request.
    void record_dependency(Web::FileRequest& request)
    {
        auto url = URL::create_with_file_scheme(request.path());
        if (!url.has_value())
            return;
        auto set = dependencies;
        auto index = set->request(url->serialize().to_byte_string(), KR_DEPENDENCY_SUBRESOURCE);
        if (!index.has_value())
            return;
        request.on_file_request_finish = [set = move(set), index = *index, finish = move(request.on_file_request_finish)](ErrorOr<i32> fd_or_error) mutable {
            set->finish(index, fd_or_error.is_error() ? KR_ERROR_IO : KR_OK);
            finish(move(fd_or_error));
        };
    }

    Function<void(URL::URL const&)> on_navigate;

    // The view installs the `engine` global (EngineChannel) on each top-level document's Window as
    // it becomes active, before its parser runs any page script, and that discards the previous
    // page's endpoints.
    Function<void(Web::DOM::Document&)> on_new_document;
    virtual void page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document& document) override
    {
        if (on_new_document)
            on_new_document(document);
    }
    virtual void page_did_request_cursor_change(Gfx::Cursor const&) override { }

    // Layout update passes run for this page's documents (kr_view_stats.layout_updates).
    // LibWeb calls flush_pending_dom_mutations once at the end of each pass, and only then
    // (Layout/UpdateLayout.cpp): every update_style_and_layout_once of Document::update_layout (one
    // per call, more when an image or a web face arriving makes it lay out again) and every layout a
    // rendering update takes in from a flight. A call that finds the document inactive runs no pass
    // and isn't counted. karouka listens for no DOM mutations, so there is nothing to flush.
    u64 layout_updates { 0 };
    virtual void flush_pending_dom_mutations() override { ++layout_updates; }

private:
    explicit PageClient(u64 id)
        : m_id(id)
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
        auto* theme = buffer.data<Gfx::SystemTheme>();
        theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::White).value();
        theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Black).value();
        theme->color[to_underlying(Gfx::ColorRole::Base)] = Color(Color::White).value();
        theme->color[to_underlying(Gfx::ColorRole::BaseText)] = Color(Color::Black).value();
        // A selection in the page's normal color scheme paints in the palette's selection color;
        // left at 0 it is transparent and selections don't show. Ladybird's Default.ini values.
        theme->color[to_underlying(Gfx::ColorRole::Selection)] = Color(0x84, 0x35, 0x1a).value();
        theme->color[to_underlying(Gfx::ColorRole::InactiveSelection)] = Color(0x60, 0x60, 0x60).value();
        m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(move(buffer));
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Queue<Web::QueuedInputEvent> m_input_event_queue;
    u64 m_id { 0 };
    u64 m_next_cross_process_id { 1 };
    Web::DevicePixelSize m_viewport;
    double m_scale { 1.0 };
    u8 m_render_reasons { RenderReasonLoad };
    bool m_repaint_requested { true };
    bool m_has_focus { true };
};

GC_DEFINE_ALLOCATOR(PageClient);

// karouka's own timings read the host's clock (kr_host_clock) when it has one.
static double now_ms()
{
    return host_now_ms();
}

}

// LibWeb's host clock (patch 0021) at its installation; the first kr_view_advance_time's now_ms
// maps to it (host_time_in_libweb_clock).
static constexpr double host_clock_start_ms = 1000.0;

// Every view of a system shares one main-thread VM, one HTML event loop and one rendering update:
// LibWeb's rendering update services every document in the process at once. Every view also
// rasterizes through the system's one GPU context: one MTLDevice, one MTLCommandQueue and one
// GrDirectContext, whose resource cache (glyph atlases, gradients, layer textures, image uploads)
// all views share under one budget. A view adds only its render target (its IOSurface, or the
// host's texture) and its player.
// The device and queue are the host's when kr_system_desc names them (host_queue).
// With KR_BACKEND_SOFTWARE (software) there is no GPU context at all: skia stays null, and every view
// rasterizes with Skia's CPU rasterizer into its IOSurface's memory.
struct kr_system {
    OwnPtr<Core::EventLoop> event_loop;
    // kr_system_desc.shader_cache_path's programs, which the GPU context reads and adds to; declared
    // before it so that it outlives it.
    OwnPtr<Karouka::ShaderCache> shader_cache;
    // The glyphs views drew in web fonts, kept next to the shader cache and warmed on a worker as
    // soon as LibWeb makes the font's typeface (WebFontGlyphs.h).
    OwnPtr<Karouka::WebFontGlyphs> web_font_glyphs;
    RefPtr<Gfx::SkiaBackendContext> skia;
    // Canvas 2D contexts replay here, on the main thread, with `skia` (LocalCanvas.h).
    RefPtr<Karouka::LocalCanvasHost> canvas_host;
    // What draws the DrawEmbedderContent commands of shader() images (ShaderImage.h).
    Karouka::EmbedderContentRegistry embedder_content;
    // shader() images' compiled SkSL, which paints through embedder_content; declared after it so
    // that it goes first and takes its content out.
    OwnPtr<Karouka::ShaderImages> shader_images;
    bool host_queue { false };
    bool software { false };
    Vector<kr_view*> views;
    u64 next_page_id { 1 };
    // The HTML event loop's paint counter when the views last looked; see collect_paints().
    u64 paints_seen { 0 };
    // The host time and the view whose kr_view_advance_time last gave LibWeb a rendering
    // opportunity; see kr_view_advance_time.
    Optional<double> last_opportunity_time;
    kr_view* last_opportunity_view { nullptr };
    // LibWeb's clock minus the host's, fixed at the first advance; see host_time_in_libweb_clock().
    Optional<double> host_clock_offset;
    // kr_system_desc.real_time_clock: LibWeb reads the system's monotonic clock, and no host clock
    // is installed. Otherwise the time LibWeb's host clock was last set to; see step_host_clock_to().
    bool real_time_clock { false };
    double host_clock_time { host_clock_start_ms };
    // Idle trimming; see trim_allocator_when_idle(). The event loop's update requests plus paints
    // at the last rendering opportunity, how many opportunities in a row have seen neither change,
    // and whether the allocator has been trimmed since the last one that did.
    u64 activity_seen { 0 };
    u32 idle_opportunities { 0 };
    bool trimmed_since_activity { false };
    // Opportunities that saw a change since the side threads last collected; see trim_allocator_when_idle().
    u32 busy_opportunities { 0 };
};

struct kr_view {
    kr_system* system { nullptr };
    GC::Root<Karouka::PageClient> client;
    GC::Root<Web::Page> page;
    // The fonts, image frames, video sinks and nested display lists the page's recordings reference.
    // Recording only adds to it; prune_resource_storage drops what the current frame no longer uses.
    Compositing::DisplayListResourceStorage resource_storage;
    // The display list resource_storage was last pruned to (DisplayList::id()), 0 before the first.
    u64 resource_storage_pruned_for { 0 };
    // Whether a nested display list (by DisplayList::id()) can paint other pixels under the same id
    // (nested_display_list_is_live). Lists are immutable and ids unique, so a verdict never goes stale;
    // the map is cleared when it grows past a bound.
    mutable HashMap<u64, bool> nested_display_list_liveness;
    // The same for live embedder content alone (records_draw_live_embedder_content).
    mutable HashMap<u64, bool> nested_display_list_draws_live_embedder_content;
    // What the view draws into: its own IOSurface (buffer), or, when the render was given one
    // (kr_view_set_output), the host's texture (surface_texture, retained by the surface). Never both.
    Optional<Gfx::SharedImageBuffer> buffer;
    void const* surface_texture { nullptr };
    RefPtr<Gfx::PaintingSurface> surface;
    OwnPtr<Compositing::DisplayListPlayerSkia> player;
    // Whether the surface holds a frame: one was rasterized into it since it was created.
    bool surface_has_frame { false };
    // kr_view_lock_pixels' read lock on the IOSurface, held until kr_view_unlock_pixels. While it
    // is held nothing renders into, drops or replaces the surface.
    OwnPtr<Karouka::IOSurfaceCPUAccess> pixel_lock;
    // The frame last rasterized into the surface, what a paint is compared against to find out
    // what changed in the viewport (frame_damage). Dropped with the surface.
    struct RasterizedFrame {
        NonnullRefPtr<Compositing::DisplayList> display_list;
        Compositing::AccumulatedVisualContextTree visual_context_tree;
        Compositing::ScrollStateSnapshot scroll_state;
    };
    Optional<RasterizedFrame> last_rasterized_frame;
    // KR_DEBUG: paints not rasterized because nothing changed in the viewport.
    u32 paints_without_viewport_damage { 0 };
    // Whether LibWeb painted this view's page since its last render. Its transform and opacity
    // animations are then handed to the visual context tree again before recording (rasterize).
    bool main_thread_painted { true };
    // Whether, at the last host frame, the page's visual context tree had an animation running
    // whose target could paint into the viewport; see request_render_for_visual_animations.
    bool visual_animation_was_on_screen { false };
    // The blinking caret the last rendered frame drew, if any, and whether it showed in that frame: a caret
    // blinks in the player (DisplayListPlayerSkia, at the host's time), with no paint from LibWeb, so the view
    // renders again when the host's time takes it to the other half of its cycle (request_render_for_caret_blink).
    Optional<Compositing::PaintCaret> blinking_caret;
    bool blinking_caret_shown { false };
    // Whether the last rendered frame drew live embedder content in the viewport, a shader() image whose
    // pixels change with the host's time while its display list doesn't, and the host time that frame was drawn at:
    // the view renders again at each new host time (request_render_for_embedder_content).
    bool draws_live_embedder_content { false };
    i64 live_embedder_content_time_ns { 0 };
    // What the embedder content painters drew in the last frame replayed: content that depends on time, and the
    // canvases shader() canvas inputs sampled at their content generations then.
    Karouka::EmbedderContentRegistry::Replay embedder_content_replay;
    // KR_DEBUG: the last on-screen verdict logged, and how many renders each RenderReason caused.
    Optional<bool> logged_visual_animation_on_screen;
    Array<u32, Karouka::RenderReasonCount> render_reason_counts {};
    // kr_view_set_output's target, consumed by the next kr_view_render.
    Optional<kr_metal_output> output;
    Web::DevicePixelSize size;
    double scale { 1.0 };
    bool transparent { false };
    kr_view_stats stats {};
    unsigned mouse_buttons { 0 };
    kr_navigate_callback navigate_callback { nullptr };
    void* navigate_user_data { nullptr };
    // Embedder script types (<script type="text/python">) and what runs them; kept across loads.
    Karouka::ViewScriptTypes script_types;
    // Told of each new active top-level document (the Python module, ScriptTypes.h).
    Karouka::ActiveDocumentHandler active_document_handler;
    // kr_view_title's answer, and the document state it was read from.
    ByteString title_utf8;
    struct TitleSource {
        Web::DOM::Document const* document { nullptr };
        u64 dom_tree_version { 0 };
        u64 character_data_version { 0 };
        bool operator==(TitleSource const&) const = default;
    } title_source;
    // Lives as long as the view, across loads: a new document binds to the same game state.
    Karouka::BindingModel binding;
    // The current document's data-bind-* attributes, driven from `binding` (BindingDom.h).
    Karouka::DocumentBindings document_bindings;
    // The page <-> host `engine` channel; reset by each new top-level document.
    Karouka::EngineChannel engine { this };
    // What kr_view_reload loads again: the URL of the last successful load and, for
    // kr_view_load_html, a copy of its markup (a file is read again).
    struct LoadedPage {
        URL::URL url;
        Optional<ByteBuffer> markup;
    };
    Optional<LoadedPage> loaded_page;
    // The viewport scroll position a reload carries over, applied once the new document has loaded
    // (restore_scroll_after_reload).
    Optional<Web::CSSPixelPoint> scroll_to_restore;
    // kr_view_get_dependencies' answer, valid until its next call: the entries point into urls.
    Vector<kr_view_dependency> dependency_entries;
    Vector<ByteString> dependency_urls;
};

static kr_system* s_system;

// LibWeb's one script type handler: finds the view whose page holds the script's document and runs
// that view's handler for the type (ScriptTypes.h).
class ScriptTypeDispatch final : public Web::Embedder::ScriptTypeHandler {
public:
    virtual bool handles_script_type(Web::DOM::Document const& document, StringView mime_essence) override
    {
        auto* view = view_of(document);
        return view && view->script_types.find(mime_essence);
    }

    virtual bool run_script(Web::HTML::HTMLScriptElement& element, Web::Embedder::EmbedderScript const& script) override
    {
        auto* view = view_of(element.document());
        auto entry = view ? view->script_types.find(script.mime_essence()) : nullptr;
        // Removed since the script was prepared: nothing runs it.
        if (!entry)
            return false;
        return entry->handler(element, script);
    }

private:
    static kr_view* view_of(Web::DOM::Document const& document);
};
static ScriptTypeDispatch s_script_type_dispatch;
// Set once kr_system_create2 starts LibWeb's process-wide setup (main-thread VM, plugins, font
// database). That setup can't run twice, and kr_system_destroy halts LibWeb's stage threads and the
// thread pool for good, so every later create fails, after a destroy or a create that failed past
// this point alike.
static bool s_libweb_initialized;

// The shared Skia resource cache budget when kr_system_desc.gpu_cache_bytes is 0. Ladybird's default
// (256 MiB) is sized for a browser tab; an embedded page texture needs far less, and every view of
// the system draws from the same budget.
static constexpr size_t default_gpu_cache_bytes = 64 * MiB;

// The budget of Skia's GPU text blob cache (TextBlobCache.h), in Skia's accounting; Skia's own is
// 4 MiB. LibCompositing keeps every SkTextBlob it makes, so a page that writes new text every frame
// fills the cache to its budget, ~1.07 bytes of system malloc per byte counted. A screen
// of dense 11 px text at 1280x800 holds 0.56 MiB, most demo pages 0.02-0.05 MiB (hud_page's ticking
// readouts 0.6 MiB by frame 300); past the budget a redraw only builds the evicted blob again.
static constexpr size_t text_blob_cache_bytes = 1 * MiB;

// KR_DEBUG=1 logs each rendered frame and the rendering scheduler's counters (HostInterface.h).
using Karouka::debug_enabled;

// Skia's glyph cache strike count limit; see kr_system_create2.
static constexpr int font_cache_strike_limit = 256;

// KR_FONT_HINTING=none|slight|normal|full: the hinting every glyph rasterizes with.
// Unset (or anything else): Skia's default, kNormal, which Skia's CoreText scaler takes
// as leave to use CoreGraphics' font smoothing, dilating glyphs past their outlines. karouka_viewer
// --font-hinting sets it; reftests want none, as they compare glyph edges.
static Optional<Gfx::FontHintingStyle> font_hinting_from_environment()
{
    auto const* value = getenv("KR_FONT_HINTING");
    if (!value)
        return {};
    StringView name { value, strlen(value) };
    if (name == "none"sv)
        return Gfx::FontHintingStyle::None;
    if (name == "slight"sv)
        return Gfx::FontHintingStyle::Slight;
    if (name == "normal"sv)
        return Gfx::FontHintingStyle::Normal;
    if (name == "full"sv)
        return Gfx::FontHintingStyle::Full;
    Karouka::log(KR_LOG_WARN, "KR_FONT_HINTING={} is not none, slight, normal or full; ignored", name);
    return {};
}

static ByteString describe_render_reasons(u8 reasons)
{
    StringBuilder builder;
    for (size_t i = 0; i < Karouka::RenderReasonCount; ++i) {
        if (!(reasons & (1u << i)))
            continue;
        if (!builder.is_empty())
            builder.append('|');
        builder.append(Karouka::render_reason_names[i]);
    }
    return builder.to_byte_string();
}

// After LibWeb painted anything, marks for rendering every view whose page may have been painted.
static void collect_paints(kr_system& system)
{
    auto paints = Web::HTML::main_thread_event_loop().rendering_scheduler_counters().paints;
    if (paints == system.paints_seen)
        return;
    system.paints_seen = paints;
    for (auto* view : system.views) {
        if (view->client->take_painted()) {
            view->client->set_needs_render(Karouka::RenderReasonPaint);
            view->main_thread_painted = true;
        }
    }
}

// Host frames in a row without a rendering update request or a paint before the allocator is
// trimmed: half a second at 60 Hz, so a page that pauses between bursts of work is not trimmed
// between every two of them.
static constexpr u32 idle_opportunities_before_trim = 30;

// Host frames with a rendering update request or a paint before the StyleLayout, Paint and pool
// threads are made to collect anyway: two seconds at 60 Hz. An animated page never goes idle, and a
// thread purges the pages it freed only when it next allocates or frees pages in the same segment, so
// the Paint thread held ~30 MB of expired purges through a whole run of animated_churn.
// KR_BUSY_COLLECT_FRAMES=N sets the count, 0 turns the busy collect off.
static u32 busy_opportunities_before_collect()
{
    static u32 const frames = [] {
        auto const* value = getenv("KR_BUSY_COLLECT_FRAMES");
        if (!value || !*value)
            return 120u;
        return static_cast<u32>(strtoul(value, nullptr, 10));
    }();
    return frames;
}

// Returns free allocator memory to the OS once the system goes idle. mimalloc purges the pages a
// thread frees 1 ms later (purge_delay, AK/kmalloc.cpp), but only when that thread next allocates,
// frees or collects, and it keeps the pages it holds in the thread's caches and the delayed frees
// other threads handed back; a forced collect (ak_kmalloc_collect: the default heap and the
// thread-local Buffer and String partitions, not the shared ArrayBuffer, JSObjectStorage and Layout
// heaps) purges and releases what of them is free, on the calling thread only. So every thread that
// allocates is made to collect its own heaps: this one here, the StyleLayout and Paint threads (they
// park between jobs and never collect on their own) through a collect handed to them (patch 0030),
// and the pool threads now rather than a second after their last job (patch 0078). Theirs run there
// while this thread goes on. The system malloc zones (Skia, Metal, Objective-C, CoreText) keep free
// pages too; pressure relief returns them. Once per
// idle period: called for each host frame's rendering opportunity, it trims on the frame that
// completes idle_opportunities_before_trim idle frames and not again until the event loop sees a
// rendering update request or a paint. It walks the main thread's heap and every malloc zone:
// 1.3 to 2 ms on an empty page. A page that keeps changing has the other threads collect every
// busy_opportunities_before_collect frames instead; theirs run between their jobs, and this thread,
// which allocates every frame, doesn't collect.
static void trim_allocator_when_idle(kr_system& system)
{
    auto const& counters = Web::HTML::main_thread_event_loop().rendering_scheduler_counters();
    u64 activity = counters.update_requests + counters.paints;
    if (activity != system.activity_seen) {
        system.activity_seen = activity;
        system.idle_opportunities = 0;
        system.trimmed_since_activity = false;
        auto collect_every = busy_opportunities_before_collect();
        if (collect_every && ++system.busy_opportunities >= collect_every) {
            system.busy_opportunities = 0;
            Web::Embedder::collect_stage_thread_heaps();
            Threading::ThreadPool::collect_idle_threads();
        }
        return;
    }
    if (system.trimmed_since_activity || ++system.idle_opportunities < idle_opportunities_before_trim)
        return;
    auto start = Karouka::now_ms();
    Web::Embedder::collect_stage_thread_heaps();
    Threading::ThreadPool::collect_idle_threads();
    ak_kmalloc_collect();
    malloc_zone_pressure_relief(nullptr, 0);
    system.trimmed_since_activity = true;
    system.busy_opportunities = 0;
    if (debug_enabled())
        Karouka::log(KR_LOG_DEBUG, "idle for {} frames, trimmed the allocator in {:.2} ms", system.idle_opportunities, Karouka::now_ms() - start);
}

static Web::HTML::LocalTraversableNavigable& traversable_of(kr_view& view)
{
    return as<Web::HTML::LocalTraversableNavigable>(*view.page->top_level_traversable());
}

static Web::CSSPixelSize css_size_of(kr_view const& view)
{
    return { Web::CSSPixels(view.size.width().value() / view.scale), Web::CSSPixels(view.size.height().value() / view.scale) };
}

static void release_surface(kr_view& view)
{
    view.pixel_lock = nullptr; // kr_view_destroy; render and resize refuse while it is held
    view.surface = nullptr;
    view.buffer.clear();
    view.surface_texture = nullptr;
    view.surface_has_frame = false;
    view.last_rasterized_frame.clear();
}

// Whether the view's surface is the one a render into `output` (or, NULL, into the IOSurface)
// draws into: then it holds the last frame, if any.
static bool surface_is_target(kr_view const& view, kr_metal_output const* output)
{
    if (!view.surface)
        return false;
    if (output)
        return view.surface_texture == output->texture;
    auto size = Gfx::IntSize { view.size.width().value(), view.size.height().value() };
    return view.buffer.has_value() && view.surface->size() == size;
}

// A render with a target (kr_view_set_output) draws straight into the host's texture: the view has
// no IOSurface then. Without one the IOSurface is created on the first render that needs
// it, at exactly the view's device size, so a view that never renders costs no surface memory.
// Resizing to a new size drops either; switching between them drops the other.
static kr_result ensure_surface(kr_view& view, kr_metal_output const* output)
{
    if (surface_is_target(view, output))
        return KR_OK;
    release_surface(view);
    auto size = Gfx::IntSize { view.size.width().value(), view.size.height().value() };
    if (size.is_empty())
        return KR_ERROR_INVALID_ARGUMENT;
    if (view.system->software) {
        // kr_view_set_output2 refuses a target without a host queue, so there is none here.
        view.buffer = Gfx::SharedImageBuffer::create(size);
        view.surface = Karouka::create_software_surface(*view.buffer);
        return KR_OK;
    }
    if (!view.system->skia)
        return KR_ERROR_GPU;
    if (output) {
        view.surface = Karouka::create_output_surface(*view.system->skia, *output);
        if (!view.surface)
            return KR_ERROR_GPU;
        view.surface_texture = output->texture;
        return KR_OK;
    }
    view.buffer = Gfx::SharedImageBuffer::create(size);
    view.surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*view.buffer, *view.system->skia);
    return KR_OK;
}

// The members after struct_size of the versioned public structs, in order (StructSize.h).
static constexpr Karouka::StructMember system_desc_members[] = {
    KR_STRUCT_MEMBER(kr_system_desc, resource_root),
    KR_STRUCT_MEMBER(kr_system_desc, verbose),
    KR_STRUCT_MEMBER(kr_system_desc, gpu_cache_bytes),
    KR_STRUCT_MEMBER(kr_system_desc, host),
    KR_STRUCT_MEMBER(kr_system_desc, mtl_device),
    KR_STRUCT_MEMBER(kr_system_desc, mtl_queue),
    KR_STRUCT_MEMBER(kr_system_desc, real_time_clock),
    KR_STRUCT_MEMBER(kr_system_desc, backend),
    KR_STRUCT_MEMBER(kr_system_desc, shader_cache_path),
};
static_assert(Karouka::lists_every_member<kr_system_desc>(system_desc_members));

static constexpr Karouka::StructMember view_desc_members[] = {
    KR_STRUCT_MEMBER(kr_view_desc, width),
    KR_STRUCT_MEMBER(kr_view_desc, height),
    KR_STRUCT_MEMBER(kr_view_desc, scale),
    KR_STRUCT_MEMBER(kr_view_desc, transparent_background),
};
static_assert(Karouka::lists_every_member<kr_view_desc>(view_desc_members));

static constexpr Karouka::StructMember view_stats_members[] = {
    KR_STRUCT_MEMBER(kr_view_stats, frames_rendered),
    KR_STRUCT_MEMBER(kr_view_stats, updates_run),
    KR_STRUCT_MEMBER(kr_view_stats, last_update_ms),
    KR_STRUCT_MEMBER(kr_view_stats, last_record_ms),
    KR_STRUCT_MEMBER(kr_view_stats, last_raster_ms),
    KR_STRUCT_MEMBER(kr_view_stats, malloc_bytes),
    KR_STRUCT_MEMBER(kr_view_stats, layout_updates),
    KR_STRUCT_MEMBER(kr_view_stats, event_loop_pumps),
    KR_STRUCT_MEMBER(kr_view_stats, last_raster_pixels),
    KR_STRUCT_MEMBER(kr_view_stats, canvas_bytes),
};
static_assert(Karouka::lists_every_member<kr_view_stats>(view_stats_members));

static constexpr Karouka::StructMember view_dependencies_members[] = {
    KR_STRUCT_MEMBER(kr_view_dependencies, entries),
    KR_STRUCT_MEMBER(kr_view_dependencies, count),
    KR_STRUCT_MEMBER(kr_view_dependencies, revision),
    KR_STRUCT_MEMBER(kr_view_dependencies, truncated),
};
static_assert(Karouka::lists_every_member<kr_view_dependencies>(view_dependencies_members));

static constexpr Karouka::StructMember memory_stats_members[] = {
    KR_STRUCT_MEMBER(kr_memory_stats, footprint_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, malloc_zone_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, kmalloc_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, gc_heap_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, gpu_cache_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, gpu_cache_budget_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, surface_bytes),
    KR_STRUCT_MEMBER(kr_memory_stats, font_cache_strikes),
};
static_assert(Karouka::lists_every_member<kr_memory_stats>(memory_stats_members));

static constexpr Karouka::StructMember input_event_members[] = {
    KR_STRUCT_MEMBER(kr_input_event, type),
    KR_STRUCT_MEMBER(kr_input_event, x),
    KR_STRUCT_MEMBER(kr_input_event, y),
    KR_STRUCT_MEMBER(kr_input_event, delta_x),
    KR_STRUCT_MEMBER(kr_input_event, delta_y),
    KR_STRUCT_MEMBER(kr_input_event, button),
    KR_STRUCT_MEMBER(kr_input_event, modifiers),
    KR_STRUCT_MEMBER(kr_input_event, key_code),
    KR_STRUCT_MEMBER(kr_input_event, code_point),
};
static_assert(Karouka::lists_every_member<kr_input_event>(input_event_members));

static constexpr Karouka::StructMember metal_output_members[] = {
    KR_STRUCT_MEMBER(kr_metal_output, command_buffer),
    KR_STRUCT_MEMBER(kr_metal_output, texture),
};
static_assert(Karouka::lists_every_member<kr_metal_output>(metal_output_members));

static constexpr Karouka::StructMember engine_callbacks_members[] = {
    KR_STRUCT_MEMBER(kr_engine_callbacks, trigger),
    KR_STRUCT_MEMBER(kr_engine_callbacks, call),
    KR_STRUCT_MEMBER(kr_engine_callbacks, user_data),
};
static_assert(Karouka::lists_every_member<kr_engine_callbacks>(engine_callbacks_members));

extern "C" {

kr_result kr_system_create2(kr_system_desc const* host_desc, kr_system** out_system)
{
    // NULL: every member's default; so are the members past a short struct_size. The same for the
    // host interface, refused here, before anything process-wide starts, when it can't hold io.
    kr_system_desc sized_desc {};
    if (!out_system || (host_desc && !Karouka::read_sized(*host_desc, system_desc_members, sized_desc)))
        return KR_ERROR_INVALID_ARGUMENT;
    if (!Karouka::host_interface_is_valid(sized_desc.host))
        return KR_ERROR_INVALID_ARGUMENT;
    auto const* desc = &sized_desc;
    if (s_system) // LibWeb's main-thread VM and plugins are process-wide.
        return KR_ERROR_INVALID_ARGUMENT;
    if (s_libweb_initialized) // A system was destroyed (or failed to start): no other one can run.
        return KR_ERROR_INVALID_STATE;
    // The backend, and the host's device and queue, are checked before anything process-wide is
    // initialized. The software backend uses no GPU, so a host GPU given with it is a mistake.
    if (desc->backend != KR_BACKEND_METAL && desc->backend != KR_BACKEND_SOFTWARE)
        return KR_ERROR_INVALID_ARGUMENT;
    bool software = desc->backend == KR_BACKEND_SOFTWARE;
    if (software && (desc->mtl_device || desc->mtl_queue))
        return KR_ERROR_INVALID_ARGUMENT;
    RefPtr<Gfx::MetalContext> host_metal_context;
    if (desc && (desc->mtl_device || desc->mtl_queue)) {
        host_metal_context = Karouka::create_host_metal_context(desc->mtl_device, desc->mtl_queue);
        if (!host_metal_context)
            return KR_ERROR_INVALID_ARGUMENT;
    }

    auto create_start = MonotonicTime::now();
    s_libweb_initialized = true;
    auto* system = new kr_system;
    // kr_system_desc.real_time_clock: LibWeb reads the system's monotonic clock, and so do the main
    // thread's Core::Timers. Otherwise both run on the host's clock (see below).
    system->real_time_clock = desc && desc->real_time_clock;
    // The main thread's Core::EventLoop runs only when kr_view_advance_time pumps it, never sleeps,
    // and fires its Core::Timers on the host's clock (HostEventLoop.h).
    Karouka::install_host_event_loop(system->real_time_clock, host_clock_start_ms);
    system->event_loop = make<Core::EventLoop>();
    Karouka::install_host_interface(desc ? desc->host : nullptr, desc && desc->verbose);

    // The only GPU context of the process: every view's surface and player use it (LibWeb's own
    // main-thread context stays uninitialized, so canvases rasterize on the CPU). On the host's
    // device and queue when it passes them, otherwise on a device and queue LibGfx makes. None with
    // the software backend: no Metal device is even looked up. karouka makes the GrDirectContext
    // itself (HostMetal.mm), so that Skia reads and adds to the persistent shader cache.
    // It is made on a thread, with its warm-up (the first command buffer, the shader cache's warm
    // list precompiled), while this thread sets up LibWeb and the VM, and joined before the
    // fonts; nothing on this thread touches the GPU until then.
    system->software = software;
    system->host_queue = !host_metal_context.is_null();
    Karouka::GpuSetup gpu;
    std::thread gpu_thread;
    std::atomic<bool> joining_gpu_thread { false };
    if (!software)
        gpu_thread = std::thread([&] { gpu = Karouka::set_up_gpu(move(host_metal_context), desc->shader_cache_path, joining_gpu_thread); });

    WebView::platform_init();
    // Every file: URL has the one tuple origin ("file", "", null), Ladybird's --tuple-file-origins
    // (LibURL, URL::origin()), set before any URL computes its origin. Without it a file: page's
    // origin is opaque, so a module script it loads from a file: URL (a "cors" request) fails
    // Fetch's same-origin step and then "cors mode needs an HTTP(S) URL" in main fetch: the
    // script's result is null and it never runs (pages/tests/external_module.html).
    // Classic scripts, sheets and images are "no-cors" and fonts have an ad-hoc pass, so they
    // loaded either way. Scheme fetch serves file: URLs only to a file: client. Side effects: file:
    // documents are same-origin with each other and self.origin reads "file://", not "null".
    if (!URL::file_scheme_urls_have_tuple_origins())
        URL::set_file_scheme_urls_have_tuple_origins();
    // Upstream scheme fetch also refuses file: URLs to requests without a destination, fetch()
    // among them. karouka has no network to exfiltrate through, so a file: page may fetch() other
    // local files (patch 0092 Embedder/FileFetch, pages/tests/fetch_file_url.html).
    Web::Embedder::set_fetch_reads_file_urls(true);
    // <script> types a view registered run through it (kr_view_set_script_type_callback, ScriptTypes.h).
    Web::Embedder::set_script_type_handler(&s_script_type_dispatch);
    // Without one from the host, $KR_RESOURCE_ROOT, then the build's.
    auto const* env_resource_root = getenv("KR_RESOURCE_ROOT");
    auto resource_root = ByteString(desc && desc->resource_root ? desc->resource_root
            : env_resource_root && *env_resource_root             ? env_resource_root
                                                                  : KAROUKA_DEFAULT_RESOURCE_ROOT);
    WebView::s_ladybird_resource_root = resource_root;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(resource_root))));

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
    // LibWeb's clock is the host's (patch 0021): it stands at a fixed time until the first
    // kr_view_advance_time, then moves only with now_ms; see host_time_in_libweb_clock(). With
    // real_time_clock it stays the system's monotonic clock.
    if (!system->real_time_clock)
        Web::Embedder::install_host_clock(host_clock_start_ms);
    // One system font, San Francisco, for every family the page does not define with @font-face, and
    // no system glyph fallback (SystemUIFontProvider.h). No font directory is read: FontPlugin
    // scans them only for a PathFontProvider. FontPlugin's constructor makes SF, CoreText's first font of
    // the process (~6 ms cold: fontd, the font registry, then the default fonts' metrics), so it runs
    // on a thread while the VM and the GPU context are set up, and is installed after them.
    auto& system_fonts = Gfx::FontDatabase::the().install_system_font_provider(make<Karouka::SystemUIFontProvider>());
    // system-ui, ui-serif, ui-monospace, ui-rounded and -apple-system too: without this, LibWeb sends
    // them to CoreText (SF, New York, SF Mono, SF Rounded) past the provider and the host's default
    // font (patch 0031).
    Web::Embedder::set_system_ui_families_from_font_provider(true);
    // A view has one document: an iframe, frame or object never gets a content navigable, so it
    // loads nothing and paints as an empty box, and its contentDocument is null (patch 0100).
    Web::Embedder::set_child_navigables_enabled(false);
    if (auto hinting = font_hinting_from_environment(); hinting.has_value())
        Gfx::force_hinting_for_testing(hinting);
    // Skia's glyph cache evicts by the bytes of glyph images and paths it counts, about 2 KB a strike for short text,
    // while a strike really holds 20-40 KB more: its scaler context, CoreText font and metrics, and the typeface
    // clone a variable font makes for each size (optical size). A page that animates font-size makes a strike at every
    // size, and under the 2 MB budget about 900 of them stayed. Pages use far fewer (the demo pages 14 to 18), so the
    // count limit goes from Skia's 2048 to 256.
    SkGraphics::SetFontCacheCountLimit(font_cache_strike_limit);
    Web::Platform::FontPlugin* font_plugin = nullptr;
    std::thread font_plugin_thread([&] { font_plugin = new Web::Platform::FontPlugin(false, &system_fonts); });
    Web::Platform::ImageCodecPlugin::install(*new Karouka::LocalImageCodecPlugin);

    Web::Bindings::initialize_main_thread_vm(Web::HTML::AgentType::SimilarOriginWindow);
    Web::ResourceLoader::initialize_without_network(Web::Bindings::main_thread_vm().heap());
    Karouka::LocalImageCodecPlugin::start_gc_source_trace_if_enabled();

    // The GPU thread's context (started above) is the process's.
    if (gpu_thread.joinable()) {
        auto gpu_wait_start = MonotonicTime::now();
        // The GPU thread precompiles the shader cache's later list until now.
        joining_gpu_thread.store(true, std::memory_order_relaxed);
        gpu_thread.join();
        system->shader_cache = move(gpu.shader_cache);
        system->skia = move(gpu.skia);
        if (debug_enabled()) {
            if (system->shader_cache)
                Karouka::log(KR_LOG_DEBUG, "shader cache: {} programs from {}, {} precompiled ({} of them later frames')", system->shader_cache->loaded_count(), desc->shader_cache_path, system->shader_cache->precompiled_count(), gpu.later_precompiled);
            Karouka::log(KR_LOG_DEBUG, "gpu: context {:.2} ms, warm-up {:.2} ms, later frames' programs {:.2} ms, kr_system_create2 waited {:.2} ms", gpu.context_ms, gpu.warm_up_ms, gpu.later_ms, (MonotonicTime::now() - gpu_wait_start).to_microseconds() / 1000.0);
        }
    }
    auto fonts_wait_start = MonotonicTime::now();
    font_plugin_thread.join();
    Web::Platform::FontPlugin::install(*font_plugin);
    // Like karouka's other DEBUG lines, only with KR_DEBUG=1 (karouka.h, kr_host_log).
    if (debug_enabled())
        Karouka::log(KR_LOG_DEBUG, "fonts: system UI font ready, kr_system_create2 waited {:.2} ms", (MonotonicTime::now() - fonts_wait_start).to_microseconds() / 1000.0);
    if (!system->skia && !system->software) {
        Karouka::uninstall_host_interface();
        delete system;
        return KR_ERROR_GPU;
    }
    if (system->skia)
        system->skia->sk_context()->setResourceCacheLimit(desc && desc->gpu_cache_bytes ? desc->gpu_cache_bytes : default_gpu_cache_bytes);
    if (system->skia)
        Karouka::set_text_blob_cache_budget(*system->skia->sk_context(), text_blob_cache_bytes);
    // Canvas 2D has no backing store without a compositor; the system replays it itself.
    system->canvas_host = Karouka::LocalCanvasHost::create(system->skia);
    Web::Embedder::set_local_canvas_2d_host(system->canvas_host.ptr());
    // A shader()'s problems are the page's, so they go to the log as its console's do.
    // A shader()'s canvas inputs sample the canvases' presented pixels, from the canvas host while it lives.
    system->shader_images = make<Karouka::ShaderImages>(
        system->embedder_content, [](kr_log_level level, StringView message) {
            if (Karouka::log_enabled(level))
                Karouka::log_line(level, "console"sv, message);
        },
        [system](u64 canvas_id) -> Karouka::ShaderImages::CanvasInput {
            if (!system->canvas_host)
                return {};
            auto input = system->canvas_host->canvas_input(Compositing::CanvasId { canvas_id });
            return { input.surface, input.content_generation, input.origin_clean };
        });
    Web::Embedder::set_shader_content_hook([shader_images = system->shader_images.ptr()](StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments) {
        return shader_images->content(source, raster_scale, arguments);
    });

    if (debug_enabled())
        Karouka::log(KR_LOG_DEBUG, "kr_system_create2 {:.2} ms", (MonotonicTime::now() - create_start).to_microseconds() / 1000.0);
    s_system = system;
    // Only a host that keeps a shader cache keeps the list; LibWeb calls this from its event loop,
    // which runs only inside kr_view_* calls.
    if (desc && desc->shader_cache_path && *desc->shader_cache_path && !system->software && Karouka::web_font_glyphs_enabled()) {
        system->web_font_glyphs = make<Karouka::WebFontGlyphs>(ByteString::formatted("{}.glyphs", desc->shader_cache_path), true);
        Web::Embedder::set_web_font_typeface_created_callback([](Gfx::Typeface const& typeface) {
            if (!s_system || !s_system->web_font_glyphs)
                return;
            // The font is for one of the views' documents; their glyphs are the ones to warm.
            Vector<u32> pages;
            for (auto* view : s_system->views) {
                if (!view->page || traversable_of(*view).has_been_destroyed())
                    continue;
                if (auto document = traversable_of(*view).active_document())
                    pages.append(Karouka::WebFontGlyphs::page_hash(document->url().serialize()));
            }
            s_system->web_font_glyphs->typeface_created(typeface, pages);
        });
    }
    Karouka::install_host_fonts(system, static_cast<Karouka::SystemUIFontProvider&>(system_fonts));
    *out_system = system;
    return KR_OK;
}

void kr_system_destroy(kr_system* system)
{
    if (!system)
        return;
    if (system->skia && debug_enabled())
        Karouka::log(KR_LOG_DEBUG, "text blob cache: {} of {} bytes", Karouka::text_blob_cache_used_bytes(*system->skia->sk_context()), text_blob_cache_bytes);
    // Views still alive go first; their handles become invalid.
    while (!system->views.is_empty())
        kr_view_destroy(system->views.last());
    // The teardown below autoreleases the Metal device, seven times in karouka_autorelease_test.
    Core::ScopedAutoreleasePool autorelease_pool;
    // Loads still pending with the host belong to no view (none here); the host hears of them.
    Karouka::HostIo::cancel_all(nullptr);
    // A layout or paint job may still be running on LibWeb's stage threads (a frame in flight, a
    // speculative layout), and an off-thread script compile on the thread pool. Left running, they
    // race the teardown below and exit()'s static destructors: Skia's typeface cache is torn down
    // under a font fallback lookup on the StyleLayout thread, and a finished compile posts to the
    // event loop deleted below. No system can be created again, so these threads are halted for
    // good once the work handed to them is done.
    // The other threads that outlive this call need no halt: GCD's idle workers; LibGC's
    // DecommitWorker, which only touches the never-destroyed shared BlockAllocator; HostIo's pipe
    // writers, which own their bytes and fd. Skia's teardown below waits for the GPU (GrMtlGpu's
    // kForce_SyncQueue). LibWeb's RenderClock thread only starts with a compositor, which karouka
    // doesn't have; image decoding is synchronous (LocalImageCodecPlugin).
    // The thread pool starts on first use (an off-thread script compile, a FontFace load); most
    // pages never use it, and the() would start its four 8 MiB-stack threads only to halt them
    //, so only a started pool is halted.
    Web::Embedder::halt_stage_threads();
    Threading::ThreadPool::halt_if_started();
    // LibWeb's process-wide state (main-thread VM, plugins) cannot be torn down and rebuilt; the
    // event loop and GPU context go, the rest is reclaimed at exit.
    if (system->web_font_glyphs) {
        Web::Embedder::set_web_font_typeface_created_callback(nullptr);
        system->web_font_glyphs->save();
    }
    // Canvas contexts outlive the system (LibWeb's heap stays); their surfaces go before the context.
    Web::Embedder::set_local_canvas_2d_host(nullptr);
    Web::Embedder::set_script_type_handler(nullptr);
    Web::Embedder::set_shader_content_hook(nullptr);
    if (system->canvas_host) {
        system->canvas_host->shut_down();
        system->canvas_host = nullptr;
    }
    system->skia = nullptr;
    if (system->shader_cache) {
        if (debug_enabled())
            Karouka::log(KR_LOG_DEBUG, "shader cache: {} hits, {} misses, {} precompiled, {} programs", system->shader_cache->hits(), system->shader_cache->misses(), system->shader_cache->precompiled_count(), system->shader_cache->entry_count());
        system->shader_cache->save();
    }
    Karouka::uninstall_host_fonts(system);
    if (s_system == system)
        s_system = nullptr;
    delete system;
    Karouka::uninstall_host_interface();
}

kr_result kr_view_create2(kr_system* system, kr_view_desc const* host_desc, kr_view** out_view)
{
    kr_view_desc sized_desc {};
    if (!system || !host_desc || !out_view || !Karouka::read_sized(*host_desc, view_desc_members, sized_desc))
        return KR_ERROR_INVALID_ARGUMENT;
    auto const* desc = &sized_desc;
    if (desc->width == 0 || desc->height == 0)
        return KR_ERROR_INVALID_ARGUMENT;
    if (!system->skia && !system->software)
        return KR_ERROR_GPU;

    auto* view = new kr_view;
    view->system = system;
    view->scale = desc->scale > 0 ? desc->scale : 1.0;
    view->size = { Web::DevicePixels(desc->width), Web::DevicePixels(desc->height) };
    view->transparent = desc->transparent_background;

    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<Karouka::PageClient>(system->next_page_id++);
    client->set_viewport(view->size, view->scale);
    auto page = Web::Page::create(client);
    client->set_page(page);
    page->set_is_scripting_enabled(true);
    page->set_window_size(view->size);
    // A primary (X11/Wayland "selection") clipboard: Ladybird turns it off where the platform has
    // none (Settings.cpp, supports_clipboard_type), and karouka has none either. Left on, every
    // selectionchange event (each click collapses the selection) laid the document out to read the
    // selected text for a page_did_update_primary_selection nobody overrides.
    page->set_enable_primary_paste(false);
    // The UI process normally reports whether the tab is shown. A karouka view is always shown;
    // a hidden one gets no animation frames or CSS animation updates.
    page->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    traversable->set_viewport_size(css_size_of(*view), Web::InvalidateDisplayList::PaintCommandsAndHitTestList);

    client->on_navigate = [view](URL::URL const& url) {
        if (url.scheme() != "file"sv) {
            Karouka::log(KR_LOG_WARN, "refusing navigation to {}", url);
            return;
        }
        auto path = URL::percent_decode(url.serialize_path());
        if (view->navigate_callback && !view->navigate_callback(view, path.characters(), view->navigate_user_data))
            return;
        (void)kr_view_load_file(view, path.characters());
    };
    client->on_new_document = [view](Web::DOM::Document& document) {
        Karouka::install_console_client(document.relevant_settings_object().realm());
        view->engine.install_top_level(document);
        if (view->active_document_handler)
            view->active_document_handler(document);
    };
    view->client = GC::make_root(client);
    view->page = GC::make_root(page);
    view->player = make<Compositing::DisplayListPlayerSkia>(system->skia);

    system->views.append(view);
    *out_view = view;
    return KR_OK;
}

void kr_view_destroy(kr_view* view)
{
    if (!view)
        return;
    // Freeing the IOSurface's Metal texture autoreleases the device in Apple's driver.
    Core::ScopedAutoreleasePool autorelease_pool;
    if (debug_enabled()) {
        StringBuilder counts;
        for (size_t i = 0; i < Karouka::RenderReasonCount; ++i)
            counts.appendff(" {}={}", Karouka::render_reason_names[i], view->render_reason_counts[i]);
        Karouka::log(KR_LOG_DEBUG, "view destroyed: rendered={} of {} updates; renders by reason:{}; paints without viewport damage={}", view->stats.frames_rendered, view->stats.updates_run, counts.string_view(), view->paints_without_viewport_damage);
    }
    auto& system = *view->system;
    system.views.remove_all_matching([view](auto* other) { return other == view; });
    if (system.last_opportunity_view == view)
        system.last_opportunity_view = nullptr;

    // Close the page the way WebContent's force_close ends up doing, without the history step that
    // gets there: the traversable leaves the process-wide navigable set at once, so no later
    // rendering update, id lookup or input sees it, and its document is destroyed by a task the
    // next kr_view_advance_time of any view runs. No unload events fire. Deferred navigation
    // requests from this page find on_navigate cleared.
    view->client->on_navigate = nullptr;
    view->client->on_new_document = nullptr;
    // Loads pending with the host for this view (kr_view_load_file) or its page (subresources).
    Karouka::HostIo::cancel_all(view);
    Karouka::HostIo::cancel_all(view->client.ptr());
    if (auto& traversable = traversable_of(*view); !traversable.has_been_destroyed())
        traversable.destroy_top_level_traversable();
    // The page's canvases would keep their two surfaces each until a later collection finalizes
    // their contexts, after the document's destruction task has run.
    if (system.canvas_host)
        system.canvas_host->destroy_contexts_of(*view->page);

    view->player = nullptr;
    release_surface(*view);
    view->page = {};
    view->client = {};
    delete view;
}

// Where a load's markup came from: a file (kr_view_load_file; read again by a reload) or the host's
// memory (kr_view_load_html; a reload replays a copy).
enum class LoadSource {
    File,
    Markup,
};

static kr_result load(kr_view& view, URL::URL const& url, ReadonlyBytes html, LoadSource source)
{
    // The markup a reload replays: html is the host's memory, which it may free once this returns.
    Optional<ByteBuffer> markup;
    if (source == LoadSource::Markup) {
        auto copy = ByteBuffer::copy(html);
        if (copy.is_error())
            return KR_ERROR_INTERNAL;
        markup = copy.release_value();
    }
    // The new document's subresources are requested as it parses, from inside load_html_in_place.
    auto dependencies = Karouka::DependencySet::create();
    if (source == LoadSource::File)
        dependencies->add_loaded(url.serialize().to_byte_string(), KR_DEPENDENCY_DOCUMENT);
    auto previous_dependencies = view.client->dependencies;
    view.client->dependencies = dependencies;

    auto& traversable = traversable_of(view);
    if (!Web::Embedder::load_html_in_place(traversable, url, html, view.client->allocate_cross_process_id())) {
        view.client->dependencies = move(previous_dependencies);
        return KR_ERROR_INTERNAL;
    }
    traversable.set_viewport_size(css_size_of(view), Web::InvalidateDisplayList::PaintCommandsAndHitTestList);
    view.resource_storage = {};
    view.resource_storage_pruned_for = 0;
    view.loaded_page = kr_view::LoadedPage { url, move(markup) };
    view.scroll_to_restore = {};
    view.client->set_needs_render(Karouka::RenderReasonLoad);
    view.client->request_frame();
    return KR_OK;
}

// Whether a load carries the viewport's scroll position over to the new document (a reload).
enum class KeepScroll {
    No,
    Yes,
};

// Loads `html` in place of the current document, after a reload has read the scroll position to
// carry over: the old document's, at the swap.
static kr_result load_keeping_scroll(kr_view& view, URL::URL const& url, ReadonlyBytes html, LoadSource source, KeepScroll keep_scroll)
{
    auto scroll = traversable_of(view).viewport_scroll_offset();
    auto result = load(view, url, html, source);
    if (result == KR_OK && keep_scroll == KeepScroll::Yes && scroll != Web::CSSPixelPoint {})
        view.scroll_to_restore = scroll;
    return result;
}

// Reads the file: URL (through the host when it routes io) and loads it. With an asynchronous host
// io the load happens when the host completes.
static kr_result load_file_url(kr_view& view, URL::URL const& url, KeepScroll keep_scroll)
{
    auto load_and_report = [&view, url, keep_scroll](ReadonlyBytes html) {
        return load_keeping_scroll(view, url, html, LoadSource::File, keep_scroll);
    };

    // With a host io the host reads the page; it may answer later, and the page loads then.
    if (Karouka::HostIo::is_routed()) {
        auto result = Karouka::HostIo::request(url.serialize().to_byte_string(), &view, [url, load_and_report](kr_result status, ReadonlyBytes html) {
            if (status != KR_OK) {
                Karouka::log(KR_LOG_WARN, "the host could not load {} (error {})", url, to_underlying(status));
                return status;
            }
            return load_and_report(html);
        });
        return result.value_or(KR_OK);
    }

    auto file = Core::File::open(url.file_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return KR_ERROR_IO;
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return KR_ERROR_IO;
    return load_and_report(bytes.value());
}

kr_result kr_view_load_file(kr_view* view, char const* path)
{
    if (!view || !path)
        return KR_ERROR_INVALID_ARGUMENT;
    auto cwd = Core::System::getcwd();
    if (cwd.is_error())
        return KR_ERROR_IO;
    auto absolute = LexicalPath::absolute_path(cwd.value(), StringView { path, strlen(path) });
    auto url = URL::create_with_file_scheme(absolute);
    if (!url.has_value())
        return KR_ERROR_INVALID_ARGUMENT;
    return load_file_url(*view, *url, KeepScroll::No);
}

kr_result kr_view_load_html(kr_view* view, char const* html, size_t length, char const* base_url)
{
    if (!view || !html)
        return KR_ERROR_INVALID_ARGUMENT;
    auto url = URL::about_blank();
    if (base_url) {
        auto parsed = URL::Parser::basic_parse(StringView { base_url, strlen(base_url) });
        if (!parsed.has_value())
            return KR_ERROR_INVALID_ARGUMENT;
        url = parsed.release_value();
    }
    return load(*view, url, ReadonlyBytes { html, length }, LoadSource::Markup);
}

kr_result kr_view_reload(kr_view* view)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    if (!view->loaded_page.has_value())
        return KR_ERROR_INVALID_STATE;
    auto url = view->loaded_page->url;
    if (!view->loaded_page->markup.has_value())
        return load_file_url(*view, url, KeepScroll::Yes);
    // load() replaces loaded_page, so the markup it reads can't be the copy it replaces.
    auto markup = ByteBuffer::copy(view->loaded_page->markup->bytes());
    if (markup.is_error())
        return KR_ERROR_INTERNAL;
    return load_keeping_scroll(*view, url, markup.value().bytes(), LoadSource::Markup, KeepScroll::Yes);
}

// The viewport position a reload carried over, once the new document has loaded (its load event:
// images and fonts in, so its height is final). A document that scrolled itself by then (a
// fragment, a script) keeps its own position.
static void restore_scroll_after_reload(kr_view& view)
{
    if (!view.scroll_to_restore.has_value())
        return;
    auto& traversable = traversable_of(view);
    auto document = traversable.active_document();
    if (!document || !document->is_completely_loaded())
        return;
    auto position = view.scroll_to_restore.release_value();
    if (traversable.viewport_scroll_offset() != Web::CSSPixelPoint {})
        return;
    document->update_layout(Web::DOM::UpdateLayoutReason::NavigableViewportScroll);
    traversable.perform_scroll_of_viewport_scrolling_box(position);
    traversable.clamp_viewport_scroll_offset();
}

kr_result kr_view_get_dependencies(kr_view* view, kr_view_dependencies* out)
{
    if (!view || !out || Karouka::whole_members_end(out->struct_size, view_dependencies_members) == 0)
        return KR_ERROR_INVALID_ARGUMENT;
    auto const& set = *view->client->dependencies;
    view->dependency_entries.clear_with_capacity();
    view->dependency_urls.clear_with_capacity();
    view->dependency_entries.ensure_capacity(set.entries().size());
    view->dependency_urls.ensure_capacity(set.entries().size());
    for (auto const& entry : set.entries()) {
        view->dependency_urls.append(entry.url);
        view->dependency_entries.append({
            .url = view->dependency_urls.last().characters(),
            .kinds = entry.kinds,
            .pending = entry.in_flight > 0,
            .status = entry.status,
        });
    }
    kr_view_dependencies answer {};
    answer.struct_size = sizeof(kr_view_dependencies);
    answer.entries = view->dependency_entries.data();
    answer.count = view->dependency_entries.size();
    answer.revision = set.revision();
    answer.truncated = set.truncated();
    Karouka::write_sized(answer, view_dependencies_members, *out);
    return KR_OK;
}

kr_view* ScriptTypeDispatch::view_of(Web::DOM::Document const& document)
{
    if (!s_system)
        return nullptr;
    for (auto* view : s_system->views) {
        if (view->page && &document.page() == view->page.ptr())
            return view;
    }
    return nullptr;
}

// (C++ linkage: this file's C API definitions sit in an extern "C" block.)
extern "C++" {
kr_result Karouka::set_script_type_handler(kr_view* view, StringView mime, Karouka::ScriptTypeHandler handler)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    auto essence = Karouka::script_type_essence(mime);
    if (!essence.has_value())
        return KR_ERROR_INVALID_ARGUMENT;
    view->script_types.set(essence.release_value(), move(handler));
    return KR_OK;
}

void Karouka::set_active_document_handler(kr_view* view, Karouka::ActiveDocumentHandler handler)
{
    if (view)
        view->active_document_handler = move(handler);
}

Web::DOM::Document* Karouka::active_document(kr_view* view)
{
    if (!view || !view->page)
        return nullptr;
    auto& traversable = traversable_of(*view);
    if (traversable.has_been_destroyed())
        return nullptr;
    return traversable.active_document().ptr();
}
}

kr_result kr_view_set_script_type_callback(kr_view* view, char const* mime, kr_script_type_callback callback, void* user_data)
{
    if (!mime)
        return KR_ERROR_INVALID_ARGUMENT;
    Karouka::ScriptTypeHandler handler;
    if (callback) {
        handler = [view, callback, user_data](Web::HTML::HTMLScriptElement&, Web::Embedder::EmbedderScript const& script) {
            auto url = script.url().serialize().to_byte_string();
            auto source = script.source_text().to_byte_string();
            return callback(view, source.characters(), source.length(), url.characters(), static_cast<uint32_t>(script.line()), user_data);
        };
    }
    return Karouka::set_script_type_handler(view, StringView { mime, strlen(mime) }, move(handler));
}

void kr_view_set_navigate_callback(kr_view* view, kr_navigate_callback callback, void* user_data)
{
    if (!view)
        return;
    view->navigate_callback = callback;
    view->navigate_user_data = user_data;
}

kr_result kr_view_set_focused(kr_view* view, bool focused)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    view->client->set_has_focus(focused);
    return KR_OK;
}

char const* kr_view_title(kr_view* view)
{
    if (!view)
        return "";
    // Read from the top-level document itself. LibWeb reports a title to the PageClient only when a
    // <title>'s children change: not when the element is removed, inserted already filled, or its
    // text node edited, and not for a new document without one. The tree and
    // character-data versions come from one global counter, so they change with any edit that
    // could change the title.
    auto document = traversable_of(*view).active_document();
    if (!document)
        return "";
    kr_view::TitleSource source { document.ptr(), document->dom_tree_version(), document->character_data_version() };
    if (source != view->title_source) {
        view->title_source = source;
        view->title_utf8 = document->title().to_byte_string();
    }
    return view->title_utf8.characters();
}

kr_result kr_view_resize(kr_view* view, uint32_t width, uint32_t height, float scale)
{
    if (!view || width == 0 || height == 0)
        return KR_ERROR_INVALID_ARGUMENT;
    if (view->pixel_lock)
        return KR_ERROR_INVALID_STATE;
    // Freeing the IOSurface's Metal texture autoreleases the device in Apple's driver.
    Core::ScopedAutoreleasePool autorelease_pool;
    if (view->size.width().value() != static_cast<int>(width) || view->size.height().value() != static_cast<int>(height))
        release_surface(*view); // the next render allocates at the new size
    view->size = { Web::DevicePixels(width), Web::DevicePixels(height) };
    if (scale > 0)
        view->scale = scale;
    view->client->set_viewport(view->size, view->scale);
    view->page->set_window_size(view->size);
    traversable_of(*view).set_viewport_size(css_size_of(*view), Web::InvalidateDisplayList::PaintCommandsAndHitTestList);
    view->client->set_needs_render(Karouka::RenderReasonResize);
    view->client->request_frame();
    return KR_OK;
}

static unsigned to_modifiers(uint32_t modifiers)
{
    unsigned result = Web::UIEvents::Mod_None;
    if (modifiers & KR_MOD_SHIFT)
        result |= Web::UIEvents::Mod_Shift;
    if (modifiers & KR_MOD_CTRL)
        result |= Web::UIEvents::Mod_Ctrl;
    if (modifiers & KR_MOD_ALT)
        result |= Web::UIEvents::Mod_Alt;
    if (modifiers & KR_MOD_SUPER)
        result |= Web::UIEvents::Mod_Super;
    return result;
}

static unsigned to_button(uint32_t button)
{
    switch (button) {
    case 1:
        return Web::UIEvents::MouseButton::Secondary;
    case 2:
        return Web::UIEvents::MouseButton::Middle;
    default:
        return Web::UIEvents::MouseButton::Primary;
    }
}

// macOS virtual key codes (HIToolbox/Events.h) for the keys that produce no text.
static Web::UIEvents::KeyCode to_key_code(uint32_t mac_key_code, uint32_t code_point)
{
    using namespace Web::UIEvents;
    switch (mac_key_code) {
    case 0x24: return Key_Return;
    case 0x30: return Key_Tab;
    case 0x31: return Key_Space;
    case 0x33: return Key_Backspace;
    case 0x35: return Key_Escape;
    case 0x73: return Key_Home;
    case 0x74: return Key_PageUp;
    case 0x75: return Key_Delete;
    case 0x77: return Key_End;
    case 0x79: return Key_PageDown;
    case 0x7B: return Key_Left;
    case 0x7C: return Key_Right;
    case 0x7D: return Key_Down;
    case 0x7E: return Key_Up;
    default: return code_point ? code_point_to_key_code(code_point) : Key_Invalid;
    }
}

kr_result kr_view_inject_input2(kr_view* view, kr_input_event const* host_event)
{
    // The members past a short struct_size are zero.
    kr_input_event sized_event;
    if (!view || !host_event || !Karouka::read_sized(*host_event, input_event_members, sized_event))
        return KR_ERROR_INVALID_ARGUMENT;
    auto const* event = &sized_event;
    auto& page = *view->page;
    auto position = Web::DevicePixelPoint { Web::DevicePixels(static_cast<int>(event->x)), Web::DevicePixels(static_cast<int>(event->y)) };
    auto modifiers = to_modifiers(event->modifiers);
    switch (event->type) {
    case KR_INPUT_MOUSE_MOVE:
        page.handle_mousemove(position, position, view->mouse_buttons, modifiers);
        break;
    case KR_INPUT_MOUSE_DOWN: {
        auto button = to_button(event->button);
        view->mouse_buttons |= button;
        page.handle_mousedown(position, position, button, view->mouse_buttons, modifiers, 1);
        break;
    }
    case KR_INPUT_MOUSE_UP: {
        auto button = to_button(event->button);
        view->mouse_buttons &= ~button;
        page.handle_mouseup(position, position, button, view->mouse_buttons, modifiers);
        break;
    }
    case KR_INPUT_WHEEL:
        page.handle_mousewheel(position, position, 0, view->mouse_buttons, modifiers, event->delta_x, event->delta_y, Web::WheelDeltaPrecision::Precise);
        break;
    case KR_INPUT_KEY_DOWN:
        page.handle_keydown(to_key_code(event->key_code, event->code_point), modifiers, event->code_point, false, event->code_point != 0);
        break;
    case KR_INPUT_KEY_UP:
        page.handle_keyup(to_key_code(event->key_code, event->code_point), modifiers, event->code_point, false);
        break;
    }
    view->client->request_frame();
    return KR_OK;
}

// The host's frame time in LibWeb's clock (HighResolutionTime::unsafe_shared_current_time), which
// is the host's clock (Web::Embedder::install_host_clock in kr_system_create2): it stands at a fixed
// time until the first advance, and from there moves exactly as now_ms does. rAF timestamps, the
// document timeline (so CSS animations), performance.now() and timers all read it, so a host that
// steps now_ms by a fixed amount gets the same frames on every run.
static double host_time_in_libweb_clock(kr_system& system, double now_ms)
{
    if (!system.host_clock_offset.has_value())
        system.host_clock_offset = Web::HighResolutionTime::unsafe_shared_current_time() - now_ms;
    return now_ms + *system.host_clock_offset;
}

// The time the visual context tree's animations are sampled at, in ns on the clock their timing
// anchors are on: LibWeb's shared clock, which is the host's (install_host_clock), so a frame's
// samples are the values the style path would have computed for the same host time.
static i64 visual_animation_sample_time_ns()
{
    return static_cast<i64>(Web::HighResolutionTime::unsafe_shared_current_time() * 1'000'000.0);
}

// Paint of an animated subtree that can leave its layout boxes' border boxes and still be bounded:
// glyph overhang, outlines, small shadows. In CSS px, on every side.
static constexpr int visual_animation_paint_margin = 32;

// Animated subtrees larger than this are taken to be on screen rather than walked every frame.
static constexpr size_t max_layout_nodes_per_visual_animation = 512;

// Whether the subtree of `target`'s box may paint into `viewport` with the visual context tree's
// transforms as `to_viewport` samples them (an animated transform included, so a target that moves
// in from off screen counts from the first frame it reaches the viewport). Conservative: no box,
// a filter or box shadow, or too many boxes answer yes. Ancestor clips are ignored.
static bool may_paint_into_viewport(Web::DOM::Element& target, Web::Layout::BegunRead const& read, Web::Layout::RustFFI::FfiRectToViewportTransform const& to_viewport, Web::CSSPixelRect const& viewport)
{
    auto const* layout_node = target.unsafe_layout_node(read);
    if (!layout_node)
        return true;
    bool may_paint = false;
    size_t nodes = 0;
    layout_node->for_each_in_inclusive_subtree([&](Web::Layout::Node const& node) {
        if (++nodes > max_layout_nodes_per_visual_animation) {
            may_paint = true;
            return Web::TraversalDecision::Break;
        }
        if (auto const* styled = as_if<Web::Layout::NodeWithStyle>(node); styled && (styled->filter().has_filters() || !styled->box_shadow().is_empty())) {
            may_paint = true;
            return Web::TraversalDecision::Break;
        }
        // Text has no client rect of its own; its glyphs are laid out in its ancestors' boxes, and
        // what overhangs them by less than the margin is covered.
        if (node.is_text_node())
            return Web::TraversalDecision::Continue;
        auto rect = Web::Painting::bounding_client_rect(node, to_viewport).inflated(2 * visual_animation_paint_margin, 2 * visual_animation_paint_margin);
        if (rect.intersects(viewport)) {
            may_paint = true;
            return Web::TraversalDecision::Break;
        }
        return Web::TraversalDecision::Continue;
    });
    return may_paint;
}

// Whether any animation the visual context tree runs may change pixels in the viewport at the
// tree's samples for the current host time. The tree does not say which animations it runs, so the
// candidates are the document's effects that rasterize handed to it (compositor-driven or retaining
// compositor animations); none found answers yes.
static bool visual_animation_may_paint_into_viewport(kr_view& view, Web::DOM::Document& document, Compositing::AccumulatedVisualContextTree const& sampled_tree, Web::Layout::BegunRead const& read)
{
    auto to_viewport = Web::Painting::rect_to_viewport_transform(document, sampled_tree);
    Web::CSSPixelRect viewport { {}, css_size_of(view) };
    bool found_candidate = false;
    bool on_screen = false;
    StringBuilder targets;
    for (auto const& timeline : document.associated_animation_timelines()) {
        for (auto& animation : timeline->associated_animations()) {
            auto* effect = as_if<Web::Animations::KeyframeEffect>(animation.effect().ptr());
            if (!effect || !(effect->is_compositor_driven() || effect->has_retained_compositor_animations()))
                continue;
            auto target = effect->target();
            if (!target)
                continue;
            found_candidate = true;
            bool target_on_screen = may_paint_into_viewport(*target, read, to_viewport, viewport);
            on_screen |= target_on_screen;
            if (!debug_enabled()) {
                if (on_screen)
                    return true;
                continue;
            }
            auto const* layout_node = target->unsafe_layout_node(read);
            targets.appendff(" {}={}@{}", target->debug_description(), target_on_screen ? "on" : "off", layout_node ? Web::Painting::bounding_client_rect(*layout_node, to_viewport) : Web::CSSPixelRect {});
        }
    }
    if (!found_candidate)
        on_screen = true;
    if (debug_enabled() && view.logged_visual_animation_on_screen != on_screen) {
        view.logged_visual_animation_on_screen = on_screen;
        Karouka::log(KR_LOG_DEBUG, "visual animations {} screen (viewport {}):{}", on_screen ? "on" : "off", viewport, found_candidate ? targets.string_view() : " (no handed-off effect found)"sv);
    }
    return on_screen;
}

// Transform and opacity animations that rasterize handed to the visual context tree run without
// LibWeb restyling or painting the page, so no paint marks the view for rendering: the view renders
// while one of them runs and may paint into the viewport, and once more after the last one stops
// or leaves it, so the surface does not keep a sample from before. An animation that runs entirely
// off screen does not render the view: its pixels are not in the frame, and a scroll that brings it
// in paints. (An animation that ends changes the effect's phase, which the main thread restyles and
// paints for too; this frame does not depend on it.)
static void request_render_for_visual_animations(kr_view& view)
{
    bool on_screen = false;
    if (auto document = traversable_of(view).active_document()) {
        Web::Layout::ForcedReadScope read { *document };
        // A document that has not painted yet has no paint state: one whose rendering a pending script
        // blocks, or whose layout tree was torn down.
        if (document->has_paint_state() && document->paint_state().has_visual_context_tree(read)) {
            auto sample_time = visual_animation_sample_time_ns();
            auto tree = document->paint_state().visual_context_tree(*document);
            if (tree.has_active_visual_animation_at(sample_time))
                on_screen = visual_animation_may_paint_into_viewport(view, *document, tree.with_visual_animation_samples(sample_time), read);
        }
    }
    if (on_screen)
        view.client->set_needs_render(Karouka::RenderReasonVisualAnimation);
    else if (view.visual_animation_was_on_screen)
        view.client->set_needs_render(Karouka::RenderReasonVisualAnimationEnd);
    view.visual_animation_was_on_screen = on_screen;
}

// A focused field's caret blinks on the host clock: the player shows or hides it at the frame's
// time (set_caret_time_ns), and the caret's blink cycle starts on the same clock (Document::
// reset_cursor_blink_cycle). LibWeb doesn't paint for a blink (the compositor replays its display list),
// so the view asks for a render when the caret its last frame drew has shown or hidden since.
static void request_render_for_caret_blink(kr_view& view)
{
    if (!view.blinking_caret.has_value())
        return;
    if (Compositing::caret_is_visible_at_time(*view.blinking_caret, visual_animation_sample_time_ns()) != view.blinking_caret_shown)
        view.client->set_needs_render(Karouka::RenderReasonCaretBlink);
}

// Live embedder content (shader() images, ShaderImage.h) draws in the player at the host's time, as a blinking caret
// does, with no paint from LibWeb: the view asks for a render at each new host time while its last frame drew some.
// Live content that samples canvases alone (a shader() with canvas inputs and no uTime) changes only when one of them
// presents new pixels, so a frame that drew only such content asks for a render when a canvas it sampled moves.
static void request_render_for_embedder_content(kr_view& view)
{
    if (!view.draws_live_embedder_content)
        return;
    auto const& replay = view.embedder_content_replay;
    if ((replay.drew_content_depending_on_time || !replay.drew_anything()) && visual_animation_sample_time_ns() != view.live_embedder_content_time_ns) {
        view.client->set_needs_render(Karouka::RenderReasonShader);
        return;
    }
    if (!view.system->canvas_host)
        return;
    for (auto const& sampled : replay.sampled_canvases) {
        if (view.system->canvas_host->canvas_input(Compositing::CanvasId { sampled.canvas_id }).content_generation != sampled.content_generation) {
            view.client->set_needs_render(Karouka::RenderReasonShader);
            return;
        }
    }
}

// The first blinking caret a frame draws, and whether it shows at that frame's time.
static void note_blinking_caret(kr_view& view, Compositing::DisplayList const& display_list)
{
    view.blinking_caret.clear();
    display_list.for_each_command_header([&](Compositing::ContextRef, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (view.blinking_caret.has_value() || header.command_type != Compositing::DisplayListCommandType::PaintCaret)
            return;
        auto caret = Compositing::read_display_list_object<Compositing::PaintCaret>(payload);
        if (caret.should_blink)
            view.blinking_caret = caret;
    });
    if (view.blinking_caret.has_value())
        view.blinking_caret_shown = Compositing::caret_is_visible_at_time(*view.blinking_caret, visual_animation_sample_time_ns());
}

// Upper bound on the event loop pumps that wait for the rendering task: tasks queued before it run
// first, one per pump; a page that keeps the queue busy longer still renders on the next frame.
static constexpr int max_pumps_for_rendering_task = 64;

// Runs the rendering update a rendering opportunity queued to its end, so that the frame the host
// renders next is the one for this host time. LibWeb lets the style (or the first layout) of a
// rendering update fly on its stage threads and ends the rendering task there; the rest of the
// update runs once the event loop takes the result in between two tasks, and the next rendering
// task keeps its place in the queue until then (TaskQueue::take_first_runnable,
// EventLoop::holds_rendering_opportunity). Nothing wakes the host's poll for it, so with frames
// shorter than a flight most opportunities found the last update still flying and only queued
// behind it: headless hud_page rendered 97 of 600 frames. Taking the flight in here waits for the
// stage thread, as kr_view_render's update_layout would, and as a nested event loop does
// (EventLoop::spin_until).
static void run_rendering_update_to_its_end(Core::EventLoop& event_loop, Web::HTML::EventLoop& html_event_loop)
{
    for (int pumps = 0; pumps < max_pumps_for_rendering_task; ++pumps) {
        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        // An update still in flight from before holds the queued task back; one that ran here flew.
        html_event_loop.finish_rendering_update_in_flight();
        html_event_loop.take_committed_frames_in();
        html_event_loop.perform_a_microtask_checkpoint();
        if (!html_event_loop.rendering_task_queued_or_running())
            return;
    }
}

// Host clock steps at most this many per advance: in a host jump longer than that many
// milliseconds, the timers due within one step of each other fire together, so one long pause does
// not cost a pass per timer deadline (a 1 ms interval) it lasted.
static constexpr int max_host_clock_steps_per_advance = 1000;
// Event loop pumps per step for the tasks due then: a timer's task and the ones it queues at once.
// A page that keeps queueing (a MessageChannel loop) goes on at the next step.
static constexpr int max_pumps_per_host_clock_step = 8;
// Event loop pumps for the tasks left over from the last frame: those its rendering update queued
// (an <img> src set from rAF callbacks) and the rest of the chains its pumps didn't finish. LibWeb's
// event loop runs one task per pump (HTML::EventLoop::process), and a browser runs every task that is
// ready between two frames. With one pump a frame for them, a chain advanced one task a frame: an
// <img> given a new data: URL (fetch, body read, decode, load event: 4-5 tasks) loaded only after the
// next frame's rAF callbacks, so a page setting src every frame never saw load or error. On
// the host clock they run before the frame (at the whole millisecond before it, or the last timer
// deadline after that), not at the frame's own time: there an idle period that starts finds its
// deadline (the next rendering opportunity) already reached, skips its callbacks and costs them the
// 50 ms until the next one. A page that keeps queueing goes on at the next frame.
static constexpr int max_pumps_for_leftover_tasks = 32;

// The earliest deadline after the host clock's time of a timer that runs on it: LibWeb's HTML and
// Platform timers (patches 0021, 0025, 0043) and the main thread's Core::Timers.
static Optional<double> next_host_timer_deadline()
{
    auto web = Web::Embedder::next_host_clock_deadline();
    auto core = Karouka::event_loop_next_timer_deadline();
    if (!web.has_value())
        return core;
    if (!core.has_value())
        return web;
    return min(*web, *core);
}

static void set_host_clock(kr_system& system, double time)
{
    Karouka::set_event_loop_time(time);
    Web::Embedder::set_host_clock_time(time);
    system.host_clock_time = time;
}

// Moves LibWeb's host clock to frame_time through each timer deadline between the time it was last
// set to and frame_time, running at each the tasks the timers due then queued. Patch 0021 fires a
// timer at most once per move of the clock, and a timer whose firing is still queued drops the
// next one, so moving the clock once per frame ran setInterval(f, 10) once a frame (60 times a
// second at 60 Hz) instead of 100 times. Each step fires what is due and lets the event
// loop run those tasks, at the timer's own time, before the clock moves on: a timer re-armed from
// its callback gets its next deadline from there, so it fires on time too.
// Previously, the clock stepped through every whole millisecond instead, as HostClock did not tell
// its next deadline: a deadline between two of them fired up to 1 ms late, an idle advance moved the
// clock 1000 times a second, and while a page had idle callbacks every step pumped, since the event
// loop's idle-period timer (a Platform::Timer, patch 0025) queues no task but schedules the loop.
// Now every step is some timer's deadline and pumps, which also starts that idle period on time.
static bool step_host_clock_to(kr_system& system, Core::EventLoop& event_loop, Web::HTML::EventLoop& html_event_loop, double frame_time)
{
    auto from = system.host_clock_time;
    if (frame_time <= from)
        return false;
    auto min_step = (frame_time - from) / max_host_clock_steps_per_advance;
    auto const& tasks = html_event_loop.task_queue();
    for (int steps = 0; steps < max_host_clock_steps_per_advance; ++steps) {
        auto deadline = next_host_timer_deadline();
        if (!deadline.has_value() || *deadline >= frame_time)
            break;
        // The tasks already queued run ahead of the step's timer tasks, one a pump: another view's
        // burst of tasks would hold a timer's task past the next deadline, and an interval whose
        // firing is still queued then loses one (interval_rate four views to a system).
        // So the step pumps for them too, up to the leftover-task bound.
        auto queued_ahead = min(tasks.size_slow(), static_cast<size_t>(max_pumps_for_leftover_tasks));
        set_host_clock(system, min(max(*deadline, system.host_clock_time + min_step), frame_time));
        if (system.host_clock_time >= frame_time)
            break;
        // The HTML timer tasks the step queued, the main thread's Core::Timers that came due (LibGC's
        // sweep and idle-collection timers, media's: they fire at the pump), and whatever a
        // Platform::Timer's callback scheduled.
        for (int pumps = 0; pumps < max_pumps_per_host_clock_step + static_cast<int>(queued_ahead); ++pumps) {
            if (event_loop.pump(Core::EventLoop::WaitMode::PollForEvents) == 0)
                break;
        }
    }
    // The tasks left from the last frame, and those the steps queued and didn't run, at the whole
    // millisecond (of a long jump, step) before the frame, where they ran when the clock stepped
    // through each: no timer is due before frame_time any more, so moving there fires none.
    auto step = max(1.0, AK::ceil((frame_time - from) / max_host_clock_steps_per_advance));
    auto before_frame = AK::ceil(frame_time / step) * step - step;
    if (before_frame > system.host_clock_time && before_frame < frame_time)
        set_host_clock(system, before_frame);
    for (int pumps = 0; pumps < max_pumps_for_leftover_tasks; ++pumps) {
        if (event_loop.pump(Core::EventLoop::WaitMode::PollForEvents) == 0)
            break;
    }
    auto queued = tasks.size_slow();
    set_host_clock(system, frame_time);
    // The timer tasks due at the frame's time. Pumping only for them keeps a static page's advance at
    // one pump, the one above.
    for (int pumps = 0; (tasks.size_slow() != queued || Karouka::event_loop_has_due_timers()) && pumps < max_pumps_per_host_clock_step; ++pumps) {
        if (event_loop.pump(Core::EventLoop::WaitMode::PollForEvents) == 0)
            break;
    }
    return true;
}

kr_result kr_view_advance_time(kr_view* view, double now_ms)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    auto start = Karouka::now_ms();
    auto& system = *view->system;
    auto& event_loop = *system.event_loop;

    // The rendering update "paints" a navigable that needs it, which without a compositor only clears
    // its needs_repaint flag. karouka paints that frame itself in kr_view_render.
    auto& html_event_loop = Web::HTML::main_thread_event_loop();

    // Due timers and queued tasks (parsing, fetch completions, script) first, for every view. A host
    // clock that stepped ran them, and the tasks left from the last frame, at its last step. The
    // real-time clock has no steps: they run here. Another view that advanced to this time already ran
    // them; one pump picks up what came after (as every advance did before ).
    // A collection a decode left to the frame boundary that no render took, before this
    // frame's tasks decode more.
    Karouka::LocalImageCodecPlugin::collect_decoded_images_at_frame_boundary();

    auto frame_time = host_time_in_libweb_clock(system, now_ms);
    bool stepped = !system.real_time_clock && step_host_clock_to(system, event_loop, html_event_loop, frame_time);
    auto pumps_here = system.real_time_clock ? max_pumps_for_leftover_tasks : stepped ? 0 : 1;
    for (int pumps = 0; pumps < pumps_here; ++pumps) {
        if (event_loop.pump(Core::EventLoop::WaitMode::PollForEvents) == 0)
            break;
    }

    // The model writes since this view's last advance, coalesced, form one batch, applied to the
    // bound DOM nodes before the rendering opportunity so that its style and layout see them.
    view->document_bindings.update(view->binding, traversable_of(*view).active_document().ptr());
    // Before the update, so that its paint shows the restored position.
    restore_scroll_after_reload(*view);

    // One rendering opportunity per host frame: rAF callbacks, animations, style, layout, for every
    // view's document at once, run to its end before this returns. Another view
    // already gave one for this host time, so this view's page has had its update this frame. The
    // same view advancing twice at one time still gets an update each call.
    bool already_updated = system.last_opportunity_time == now_ms && system.last_opportunity_view && system.last_opportunity_view != view;
    if (!already_updated) {
        system.last_opportunity_time = now_ms;
        system.last_opportunity_view = view;
        if (html_event_loop.rendering_opportunity(frame_time, Web::HTML::EventLoop::RenderingOpportunitySource::LocalTimer)) {
            run_rendering_update_to_its_end(event_loop, html_event_loop);
            // The update ran for every view's document, so it counts for each: the views that
            // advance after this one at the same host time take it as theirs (already_updated).
            for (auto* each : system.views)
                ++each->stats.updates_run;
        }
    }

    if (debug_enabled()) {
        auto const& c = html_event_loop.rendering_scheduler_counters();
        Karouka::log(KR_LOG_DEBUG, "counters requests={} opportunities={} queued={} updates={} skipped={} paints={} layouts={} core_timers={} pumps={}", c.update_requests, c.opportunities_received, c.opportunities_that_queued_a_task, c.updates_run, c.updates_skipped_as_unnecessary, c.paints, view->client->layout_updates, Karouka::event_loop_timers_fired(), Karouka::event_loop_pumps());
    }
    collect_paints(system);
    request_render_for_visual_animations(*view);
    request_render_for_caret_blink(*view);
    request_render_for_embedder_content(*view);
    if (!already_updated)
        trim_allocator_when_idle(system);
    view->stats.last_update_ms = Karouka::now_ms() - start;
    return KR_OK;
}

// Where a frame differs from the frame the surface holds: rectangles in the viewport's device pixels,
// none when nothing in the viewport changed, the whole viewport when the change can't be bounded.
using DamageRects = Vector<Gfx::IntRect, 8>;

static u64 rect_area(Gfx::IntRect const& rect)
{
    return rect.is_empty() ? 0 : static_cast<u64>(rect.width()) * rect.height();
}

// At most this many rectangles go to the raster: the damage's pieces are merged down to them, the pair
// that grows the least first. Pieces whose union adds no more than free_merge_growth pixels to the two
// are merged whatever the count: each rectangle is a replay of its own (replay_into_damage), and on
// hud_page at 1920x1080 merging up to a 128x128 tile's worth took raster CPU p50 from ~0.62 to ~0.55 ms
// (7 replays to 5) for ~16k more pixels and no measurable GPU time (64k px gained no CPU and
// cost GPU).
static constexpr size_t max_damage_rects = 8;
static constexpr i64 free_merge_growth = 128 * 128;

static void merge_damage_rects(DamageRects& rects)
{
    rects.remove_all_matching([](auto const& rect) { return rect.is_empty(); });
    for (;;) {
        Optional<size_t> best_a, best_b;
        i64 best_growth = NumericLimits<i64>::max();
        for (size_t a = 0; a < rects.size(); ++a) {
            for (size_t b = a + 1; b < rects.size(); ++b) {
                auto growth = static_cast<i64>(rect_area(rects[a].united(rects[b]))) - static_cast<i64>(rect_area(rects[a]) + rect_area(rects[b]));
                if (growth < best_growth) {
                    best_growth = growth;
                    best_a = a;
                    best_b = b;
                }
            }
        }
        if (!best_a.has_value() || (best_growth > free_merge_growth && rects.size() <= max_damage_rects))
            return;
        rects[*best_a] = rects[*best_a].united(rects[*best_b]);
        rects.remove(*best_b);
    }
}

// compute_display_list_damage unions the bounds of every changed command into one rectangle, so a
// frame that changes a few small things far apart (hud_page's 20 values in the four corners of the
// viewport) rasterized 90% of it. This splits that damage without a LibCompositing change:
// each list is cut in two at the same command index, and each half is diffed against the other
// list's half on its own, as a display list holding only those commands in their runs' contexts
// (DisplayList::create_from_command_bytes, with the same visual context trees and scroll offsets),
// down to pieces that are small or that don't split any further (each level copies and diffs about
// one list's worth of commands). Any cut holds, as the pieces paint
// in order: a run's draws depend only on the pixel they land on, so a pixel outside every piece's
// damage gets the same draws from each piece over the same backdrop. A cut that pairs the wrong
// commands (one list has more) only makes the pieces' damage larger, and then the parent's
// rectangle is kept where both halves are about as large as it. Not so for a filter or a backdrop filter, which read the pixels around one: a
// tree that has one keeps the single rectangle. So does a piece whose diff can't be bounded.
struct DamageSplitter {
    struct CommandSpan {
        u32 run { 0 };
        u32 offset { 0 };
        u32 size { 0 };
    };
    struct Side {
        Compositing::DisplayList const& list;
        Compositing::AccumulatedVisualContextTree const& tree;
        Compositing::ScrollStateSnapshot const& scroll_state;
        Vector<CommandSpan> commands {};
    };
    Side old_side;
    Side new_side;
    Gfx::IntRect viewport_rect;

    static constexpr unsigned max_depth = 8;
    // Each level of the split copies and diffs about one list's worth of commands.
    static constexpr size_t max_split_command_bytes = 2 * MiB;

    static bool tree_has_filters(Compositing::AccumulatedVisualContextTree const& tree)
    {
        bool has_filters = false;
        tree.for_each_effects_filter_bytes([&](ReadonlyBytes) { has_filters = true; });
        return has_filters;
    }

    bool can_split() const
    {
        return old_side.list.command_bytes().size() <= max_split_command_bytes && new_side.list.command_bytes().size() <= max_split_command_bytes
            && !tree_has_filters(old_side.tree) && !tree_has_filters(new_side.tree);
    }

    static void collect_commands(Side& side)
    {
        auto runs = side.list.command_runs();
        for (u32 run = 0; run < runs.size(); ++run) {
            u32 offset = runs[run].offset;
            Compositing::DisplayList::for_each_command_header(side.list.command_bytes_of_run(runs[run]), [&](auto const& header, auto) {
                u32 size = sizeof(header) + header.payload_size;
                side.commands.append({ run, offset, size });
                offset += size;
            });
        }
    }

    // A list of commands [first, end) of the side's list, each in its run's context.
    static NonnullRefPtr<Compositing::DisplayList> list_of_commands(Side const& side, size_t first, size_t end)
    {
        auto runs = side.list.command_runs();
        auto all_bytes = side.list.command_bytes();
        size_t size = 0;
        for (size_t i = first; i < end; ++i)
            size += side.commands[i].size;
        auto bytes = MUST(ByteBuffer::create_uninitialized(size));
        Vector<Compositing::DisplayListCommandRun> sub_runs;
        u32 written = 0;
        for (size_t i = first; i < end; ++i) {
            auto const& command = side.commands[i];
            memcpy(bytes.data() + written, all_bytes.data() + command.offset, command.size);
            if (i == first || side.commands[i - 1].run != command.run) {
                auto run = runs[command.run];
                run.offset = written;
                run.size = 0;
                sub_runs.append(run);
            }
            sub_runs.last().size += command.size;
            written += command.size;
        }
        return Compositing::DisplayList::create_from_command_bytes(side.tree, move(bytes), move(sub_runs));
    }

    Optional<Gfx::IntRect> damage_of(size_t old_first, size_t old_end, size_t new_first, size_t new_end) const
    {
        auto old_part = list_of_commands(old_side, old_first, old_end);
        auto new_part = list_of_commands(new_side, new_first, new_end);
        return Compositing::compute_display_list_damage(*old_part, old_side.tree, old_side.scroll_state, *new_part, new_side.tree, new_side.scroll_state, viewport_rect);
    }

    // `damage` is the damage of the old list's commands [old_first, old_end) against the new list's
    // [new_first, new_end).
    void split(size_t old_first, size_t old_end, size_t new_first, size_t new_end, Gfx::IntRect damage, unsigned depth, DamageRects& out) const
    {
        if (depth == max_depth || (old_end - old_first < 2 && new_end - new_first < 2) || rect_area(damage) * 32 <= rect_area(viewport_rect)) {
            out.append(damage);
            return;
        }
        auto longer = max(old_end - old_first, new_end - new_first);
        auto old_middle = min(old_first + longer / 2, old_end);
        auto new_middle = min(new_first + longer / 2, new_end);
        auto first_half = damage_of(old_first, old_middle, new_first, new_middle);
        auto second_half = damage_of(old_middle, old_end, new_middle, new_end);
        // Each half's damage is as large as the whole's: the change spans the cut, or a cut that
        // misaligns the lists made both larger.
        if (!first_half.has_value() || !second_half.has_value()
            || min(rect_area(*first_half), rect_area(*second_half)) * 4 >= rect_area(damage) * 3) {
            out.append(damage);
            return;
        }
        if (!first_half->is_empty())
            split(old_first, old_middle, new_first, new_middle, first_half->intersected(damage), depth + 1, out);
        if (!second_half->is_empty())
            split(old_middle, old_end, new_middle, new_end, second_half->intersected(damage), depth + 1, out);
    }

    void split(Gfx::IntRect damage, DamageRects& out)
    {
        collect_commands(old_side);
        collect_commands(new_side);
        split(0, old_side.commands.size(), 0, new_side.commands.size(), damage, 0, out);
    }
};

// LibWeb repaints the whole document for any change, wherever it is (a layout commits with
// Document::after_layout_commit, a text or child change with set_needs_layout_tree_update):
// sashimi's order-status timer rewrites a card below the fold every 1.2 s and repainted the page
// each time, and a hover over one row replayed the whole list into the whole surface
//. Where a change lands is only known once it is recorded, so this diffs the display lists
// the way the compositor finds its partial-raster damage (ContextState::damage_since_last_raster,
// compute_display_list_damage), including its full-repaint cases: a new clear color, a new number
// of scroll nodes, and a damage without bounds. A scroll offset that changed is left to the diff,
// which damages what the scroll moved (a nested scroller's content and scrollbar; most of the
// viewport for the document's own scroll). Content whose pixels can change while its commands stay
// the same (a canvas, a video frame, a blinking caret, another context's, live embedder content, or a
// nested display list or the records inside a command, such as a repeated tile, holding one: command_is_live)
// is damaged wherever it lands, every frame; the whole viewport when it has no bounds or the tree has a
// filter that could spread it.
// A nested display list never changes under its id (DisplayList is immutable and its id unique), and
// compute_display_list_damage finds a PaintNestedDisplayList whose id, rect or size changed. Its pixels
// change under the same command only when, at any depth, it holds content resolved at replay (a canvas,
// a video frame, another context, live embedder content, a blinking caret) or visual animations. Only
// those are damaged every frame, so an SVG image or a still shader() image repaints only when its own
// command changes. A list the storage doesn't hold, or nested deeper than this looks, counts as live.
static bool nested_display_list_is_live(kr_view const& view, Compositing::DisplayListResourceId id, unsigned depth = 0);
static bool nested_display_list_draws_live_embedder_content(kr_view const& view, Compositing::DisplayListResourceId id, unsigned depth = 0);

// What makes records live: Replayed, content resolved at replay (nested_display_list_is_live's), or EmbedderContent,
// live embedder content alone (a view renders again at each host time only for that).
enum class Liveness : u8 {
    Replayed,
    EmbedderContent,
};

// Calls `callback` with each byte range of records a command holds inside it: an isolated group's content and mask, a
// repeated tile, a mask's content, a pattern's tile. (C++ linkage: this file's C API definitions sit in an extern "C"
// block.)
extern "C++" template<typename Command, typename Callback>
static void for_each_record_range_inside(Command const& command, ReadonlyBytes payload, Callback&& callback)
{
    auto inside = [&](Compositing::DisplayListDataSpan span) { callback(payload.slice(span.offset, span.size)); };
    if constexpr (IsSame<Command, Compositing::DrawIsolatedGroup>) {
        inside(command.content);
        if (command.mask.size != 0)
            inside(command.mask);
    } else if constexpr (IsSame<Command, Compositing::DrawRepeatedTile>) {
        inside(command.tile);
    } else if constexpr (IsSame<Command, Compositing::DeclareMaskContent>) {
        inside(command.content);
    } else if constexpr (requires { command.paint_style; command.paint_kind; }) {
        if (command.paint_kind == decltype(command.paint_kind)::PaintStyle
            && command.paint_style.paint_style_type == Compositing::DisplayListPaintStyleType::Pattern)
            inside(command.paint_style.pattern_tile);
    }
}

// Whether one command of a display list is live as `liveness` says, in itself, the records inside it or the nested
// display lists it paints, at any depth.
static bool command_is_live(kr_view const& view, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload, Liveness liveness, unsigned depth);

static bool records_are_live(kr_view const& view, ReadonlyBytes command_bytes, Liveness liveness, unsigned depth)
{
    bool live = false;
    Compositing::DisplayList::for_each_command_header(command_bytes, [&](Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        live = live || command_is_live(view, header, payload, liveness, depth);
    });
    return live;
}

static bool command_is_live(kr_view const& view, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload, Liveness liveness, unsigned depth)
{
    bool live = false;
    Compositing::visit_display_list_command(header.command_type, payload, [&](auto const& command) {
        using Command = RemoveCVReference<decltype(command)>;
        if constexpr (IsSame<Command, Compositing::DrawVideoFrame> || IsSame<Command, Compositing::DrawCanvas> || IsSame<Command, Compositing::DrawCompositedContext>) {
            live = liveness == Liveness::Replayed;
        } else if constexpr (IsSame<Command, Compositing::DrawEmbedderContent>) {
            live = command.is_live;
        } else if constexpr (IsSame<Command, Compositing::PaintCaret>) {
            live = liveness == Liveness::Replayed && command.should_blink;
        } else if constexpr (IsSame<Command, Compositing::PaintNestedDisplayList>) {
            live = liveness == Liveness::Replayed
                ? nested_display_list_is_live(view, command.display_list_id, depth + 1)
                : nested_display_list_draws_live_embedder_content(view, command.display_list_id, depth + 1);
        } else {
            for_each_record_range_inside(command, payload, [&](ReadonlyBytes records) {
                live = live || records_are_live(view, records, liveness, depth);
            });
        }
    });
    return live;
}

static bool nested_display_list_is_live(kr_view const& view, Compositing::DisplayListResourceId id, Liveness liveness, HashMap<u64, bool>& verdicts, unsigned depth)
{
    if (auto verdict = verdicts.get(id.value()); verdict.has_value())
        return *verdict;
    if (depth > 8 || !view.resource_storage.has_display_list(id))
        return true;
    auto const& resource = view.resource_storage.display_list_resource(id);
    bool live = liveness == Liveness::Replayed && resource.visual_context_tree.has_visual_animations();
    for (auto const& run : resource.display_list->command_runs()) {
        if (live)
            break;
        live = records_are_live(view, resource.display_list->command_bytes_of_run(run), liveness, depth);
    }
    if (verdicts.size() >= 4096)
        verdicts.clear();
    verdicts.set(id.value(), live);
    return live;
}

// Whether a nested display list can paint other pixels under the same id. Lists are immutable under their id, so
// this is memoized per id.
static bool nested_display_list_is_live(kr_view const& view, Compositing::DisplayListResourceId id, unsigned depth)
{
    return nested_display_list_is_live(view, id, Liveness::Replayed, view.nested_display_list_liveness, depth);
}

// Whether a nested display list draws live embedder content (an animated shader() image), at any depth.
static bool nested_display_list_draws_live_embedder_content(kr_view const& view, Compositing::DisplayListResourceId id, unsigned depth)
{
    return nested_display_list_is_live(view, id, Liveness::EmbedderContent, view.nested_display_list_draws_live_embedder_content, depth);
}

// The viewport bounds of a command of `display_list` (enclosing, unclipped by the viewport), or nothing when it has none.
static Optional<Gfx::IntRect> command_viewport_rect(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state, Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header)
{
    if (!header.has_bounding_rect)
        return {};
    auto rect = visual_context_tree.transform_rect_to_viewport(context.spatial, header.bounding_rect.to_type<float>(), scroll_state);
    if (!isfinite(rect.x()) || !isfinite(rect.y()) || !isfinite(rect.width()) || !isfinite(rect.height()))
        return {};
    return Gfx::enclosing_int_rect(rect);
}

// Whether a frame drawn from `display_list` draws live embedder content in the viewport (bounds that reach it, or
// no bounds): an animated shader() scrolled or placed out of view doesn't keep the view rendering. Nothing is
// registered (no shader() image exists) on most systems, which then skip the walk.
static bool draws_live_embedder_content_in_viewport(kr_view const& view, Compositing::DisplayList const& display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state)
{
    if (view.system->embedder_content.is_empty())
        return false;
    Gfx::IntRect viewport_rect { 0, 0, view.size.width().value(), view.size.height().value() };
    bool draws = false;
    display_list.for_each_command_header([&](Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (draws || Compositing::display_list_command_is_compositor_metadata(header.command_type))
            return;
        if (!command_is_live(view, header, payload, Liveness::EmbedderContent, 0))
            return;
        auto rect = command_viewport_rect(visual_context_tree, scroll_state, context, header);
        draws = !rect.has_value() || rect->inflated(1, 1, 1, 1).intersects(viewport_rect);
    });
    return draws;
}

static DamageRects frame_damage(kr_view const& view, Compositing::DisplayList const& display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state)
{
    Gfx::IntRect viewport_rect { 0, 0, view.size.width().value(), view.size.height().value() };
    DamageRects whole_viewport { viewport_rect };
    if (!view.surface_has_frame || !view.last_rasterized_frame.has_value())
        return whole_viewport;
    auto const& last = *view.last_rasterized_frame;
    if (last.display_list->surface_clear_color() != display_list.surface_clear_color())
        return whole_viewport;
    if (last.scroll_state.device_offsets().size() != scroll_state.device_offsets().size())
        return whole_viewport;

    // The diff damages what a scroll moves only for scrollbars (it compares two runs' commands and
    // chains, which hold no scroll offsets), so the content of each scroll node whose offset changed
    // (a nested scroller, a sticky box; the document's own scroll moves nearly everything) is damaged
    // here: where it was and where it is, unclipped by its scrollport.
    Gfx::IntRect scrolled_rect;
    {
        auto last_offsets = last.scroll_state.device_offsets();
        auto offsets = scroll_state.device_offsets();
        Vector<Compositing::SpatialNodeIndex> scrolled;
        for (size_t i = 0; i < offsets.size(); ++i) {
            if (last_offsets[i] != offsets[i])
                scrolled.append(Compositing::SpatialNodeIndex { static_cast<u32>(i) });
        }
        bool unbounded = false;
        auto add_scrolled_content = [&](Compositing::DisplayList const& list, Compositing::AccumulatedVisualContextTree const& tree, Compositing::ScrollStateSnapshot const& state) {
            if (scrolled.last().value() >= tree.spatial_node_count()) {
                unbounded = true;
                return;
            }
            auto in_scrolled = tree.spatial_nodes_in_subtrees_of(scrolled);
            list.for_each_command_header([&](Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes) {
                if (unbounded || context.spatial.value() >= in_scrolled.size() || !in_scrolled[context.spatial.value()])
                    return;
                if (!header.has_bounding_rect) {
                    if (!Compositing::display_list_command_is_compositor_metadata(header.command_type))
                        unbounded = true;
                    return;
                }
                auto rect = tree.transform_rect_to_viewport(context.spatial, header.bounding_rect.to_type<float>(), state);
                if (!isfinite(rect.x()) || !isfinite(rect.y()) || !isfinite(rect.width()) || !isfinite(rect.height())) {
                    unbounded = true;
                    return;
                }
                rect.intersect(viewport_rect.to_type<float>());
                if (!rect.is_empty())
                    scrolled_rect.unite(Gfx::enclosing_int_rect(rect).inflated(1, 1, 1, 1));
                if (scrolled_rect.contains(viewport_rect))
                    unbounded = true;
            });
        };
        if (!scrolled.is_empty()) {
            add_scrolled_content(*last.display_list, last.visual_context_tree, last.scroll_state);
            if (!unbounded)
                add_scrolled_content(display_list, visual_context_tree, scroll_state);
            if (unbounded)
                return whole_viewport;
        }
    }

    auto damage = Compositing::compute_display_list_damage(*last.display_list, last.visual_context_tree, last.scroll_state, display_list, visual_context_tree, scroll_state, viewport_rect);
    if (!damage.has_value())
        return whole_viewport;
    DamageRects rects;
    if (!scrolled_rect.is_empty())
        rects.append(scrolled_rect.intersected(viewport_rect));
    auto damage_rect = damage->intersected(viewport_rect);
    DamageSplitter splitter { { *last.display_list, last.visual_context_tree, last.scroll_state }, { display_list, visual_context_tree, scroll_state }, viewport_rect };
    if (rect_area(damage_rect) * 4 > rect_area(viewport_rect) && splitter.can_split())
        splitter.split(damage_rect, rects);
    else
        rects.append(damage_rect);

    // Adds the viewport bounds of a command; false when it has none.
    auto add_command_bounds = [&](Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header) {
        if (!header.has_bounding_rect)
            return false;
        auto rect = visual_context_tree.transform_rect_to_viewport(context.spatial, header.bounding_rect.to_type<float>(), scroll_state);
        if (!isfinite(rect.x()) || !isfinite(rect.y()) || !isfinite(rect.width()) || !isfinite(rect.height()))
            return false;
        rect.intersect(viewport_rect.to_type<float>());
        if (!rect.is_empty())
            rects.append(Gfx::enclosing_int_rect(rect).inflated(1, 1, 1, 1).intersected(viewport_rect));
        return true;
    };

    bool whole = false;
    Optional<bool> has_filters;
    auto add_live_content = [&](Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header) {
        if (!has_filters.has_value())
            has_filters = DamageSplitter::tree_has_filters(visual_context_tree);
        if (*has_filters || !add_command_bounds(context, header))
            whole = true;
    };
    display_list.for_each_command_header([&](Compositing::ContextRef context, Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (whole)
            return;
        switch (header.command_type) {
        case Compositing::DisplayListCommandType::DrawCanvas:
        case Compositing::DisplayListCommandType::DrawVideoFrame:
        case Compositing::DisplayListCommandType::DrawCompositedContext:
            add_live_content(context, header);
            break;
        case Compositing::DisplayListCommandType::PaintNestedDisplayList:
            if (nested_display_list_is_live(view, Compositing::read_display_list_object<Compositing::PaintNestedDisplayList>(payload).display_list_id))
                add_live_content(context, header);
            break;
        case Compositing::DisplayListCommandType::PaintCaret:
            if (Compositing::read_display_list_object<Compositing::PaintCaret>(payload).should_blink)
                add_live_content(context, header);
            break;
        case Compositing::DisplayListCommandType::DrawEmbedderContent:
            if (Compositing::read_display_list_object<Compositing::DrawEmbedderContent>(payload).is_live)
                add_live_content(context, header);
            break;
        // Commands that hold records (the diff compares their bytes): an animated shader() under
        // background-repeat is a nested list inside a repeated tile.
        case Compositing::DisplayListCommandType::DrawIsolatedGroup:
        case Compositing::DisplayListCommandType::DrawRepeatedTile:
        case Compositing::DisplayListCommandType::DeclareMaskContent:
            if (command_is_live(view, header, payload, Liveness::Replayed, 0))
                add_live_content(context, header);
            break;
        default:
            break;
        }
    });
    if (whole)
        return whole_viewport;
    // Merging is quadratic in the pieces; a frame with many live or animated pieces takes their union.
    if (rects.size() > 64) {
        Gfx::IntRect united;
        for (auto const& rect : rects)
            united.unite(rect);
        rects = { united };
    }
    merge_damage_rects(rects);
    return rects;
}

// Drops from the view's resource storage what neither this frame nor the page's paint command cache
// references, as NavigablePresenter::build_frame does for a compositor. Recording only ever adds
// (each decoded image frame, font and nested SVG display list a recording uses), so without this a
// page that keeps changing what it paints holds every resource it ever painted until it navigates
//. What stays: the display list's commands and the nested display lists they paint, the
// visual context tree's (filters' images), and the paint command cache source's, whose commands the
// next recording copies without adding their resources again. The previous frame's display list
// needs nothing here: frame_damage only diffs commands.
static void prune_resource_storage(Compositing::DisplayListResourceStorage& resource_storage, u64& pruned_for, Web::DOM::Document& document, Compositing::DisplayList const& display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree)
{
    // An unchanged list that added nothing references what the last prune kept.
    if (display_list.id() == pruned_for && !resource_storage.has_resources_added_since_last_retain())
        return;
    auto retained = resource_storage.collect_referenced_resources(display_list);
    retained.include(resource_storage.collect_referenced_resources(visual_context_tree));
    document.paint_state().append_paint_command_cache_source_resources(retained);
    resource_storage.retain_only(retained);
    pruned_for = display_list.id();
}

// KR_FULL_RASTER=1 rasterizes every rendered frame whole, as before what a capture shows
// then can't depend on the damage a frame's diff found (tests/partial_raster_test.c compares the two
// through fresh views instead).
static bool full_raster_forced()
{
    static bool const forced = [] {
        auto const* value = getenv("KR_FULL_RASTER");
        return value && *value && strcmp(value, "0") != 0;
    }();
    return forced;
}

// Replays the frame into `damage` (rectangles in the viewport's device pixels, within the frame) of the
// view's surface and leaves the rest of the surface as the last frame left it, as the
// compositor's partial raster does (ContextState::paint_current_display_list) for a surface it doesn't
// present: the canvas is clipped to the damage, which also bounds the player's layers. Each rectangle
// gets its own replay under a plain rectangle clip: one replay through an SkRegion clip of
// several rectangles cost ~0.15 ms more CPU on hud_page, as Ganesh turns a non-rectangular
// region into a path clip, a clip mask for every draw on its edge. A replay skips the commands outside
// its rectangle (would_be_fully_clipped_by_painter), so the extra walks are cheap, and nearby pieces
// are merged first (free_merge_growth); rectangles that overlap each clear and redraw the overlap
// whole, so it ends with the same pixels. Not upstream's damage-sized
// surface (d236b0ebb9) for GPU surfaces: drawn at an offset, a gradient's dither and
// edges' coverage land on other pixels than a whole raster's, so a view's pixels would depend on the
// damage of the frames before (karouka_partial_raster_test compares views exactly). A
// host texture larger than the view keeps its pixels outside the frame.
static void replay_into_damage(kr_view& view, Web::DOM::Document& document, Compositing::DisplayList const& display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state, DamageRects const& damage)
{
    auto& canvas = view.surface->canvas();
    // A blinking caret shows or hides at the host's time, as the visual context tree's samples are taken.
    view.player->set_caret_time_ns(visual_animation_sample_time_ns());
    // Embedder content draws at the same time, counted from the document's timeline origin (a shader()'s uTime is
    // its performance.now() in seconds). The time origin is on LibWeb's shared clock, in ms.
    view.system->embedder_content.set_time_origin_ns(static_cast<i64>(document.relevant_settings_object().time_origin() * 1'000'000.0));
    view.player->set_embedder_content_resolver(&view.system->embedder_content.resolver());
    view.system->embedder_content.begin_replay();
    for (auto const& rect : damage) {
        auto save_count = canvas.save();
        canvas.clipIRect(SkIRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
        canvas.clear(view.transparent ? SK_ColorTRANSPARENT : SK_ColorWHITE);
        view.player->execute(display_list, visual_context_tree, view.resource_storage, scroll_state, *view.surface, &view.system->canvas_host->registry(), nullptr);
        canvas.restoreToCount(save_count);
    }
    view.embedder_content_replay = view.system->embedder_content.take_replay();
}

// Records the page's display list and replays it into the render's target (`output`, or the view's
// IOSurface), if anything changed since the last render or the target is not the surface that holds
// the last frame; *rendered says whether it did.
static kr_result rasterize(kr_view* view, kr_metal_output const* output, bool& rendered)
{
    auto reasons = view->client->take_render_reasons();
    // A new target gets the current frame, but none before the view's first.
    if (view->surface_has_frame && !surface_is_target(*view, output))
        reasons |= Karouka::RenderReasonTarget;
    if (!reasons)
        return KR_OK;

    auto document = traversable_of(*view).active_document();
    if (!document)
        return KR_OK;
    if (auto result = ensure_surface(*view, output); result != KR_OK)
        return result;

    auto record_start = Karouka::now_ms();
    // A render right after the rendering update kr_view_advance_time ran to its end finds the page laid
    // out (step 16, and step 19 if anything invalidated it since), so this lays it out only for style
    // or layout work queued after that update (patch 0036).
    bool laid_out = document->update_layout_if_needed(Web::DOM::UpdateLayoutReason::HTMLEventLoopRenderingUpdate);
    Web::Layout::ForcedReadScope read { *document };
    Web::HTML::PaintConfig paint_config {
        .paint_overlay = false,
        .canvas_fill_rect = Gfx::IntRect { 0, 0, view->size.width().value(), view->size.height().value() },
    };
    // What LocalNavigable::commit_frame does for a compositor before it records: hand the page's
    // transform, opacity, background-color and filter animations that can run without style to the
    // visual context tree (Document::update_compositor_animations). Their effects then skip the
    // per-frame style update (KeyframeEffect::can_skip_per_frame_style_update), and each render
    // samples them into the tree below instead. Only after LibWeb painted: that is when the
    // animations or the tree they target can have changed, as only a painted frame is committed.
    if (exchange(view->main_thread_painted, false)) {
        document->update_paint_and_hit_testing_properties_if_needed();
        document->update_compositor_animations(read);
    }
    auto display_list = document->record_display_list(read, paint_config, view->resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite);
    if (!display_list)
        return KR_OK;
    auto visual_context_tree = document->paint_state().visual_context_tree(*document);
    if (visual_context_tree.has_visual_animations())
        visual_context_tree = visual_context_tree.with_visual_animation_samples(visual_animation_sample_time_ns());
    if (debug_enabled())
        Karouka::log(KR_LOG_DEBUG, "render reasons={} url={} title='{}' children={} command_bytes={} clear={} visual_animations={} laid_out={}", describe_render_reasons(reasons), document->url(), document->title(), document->child_count(), display_list->command_bytes().size(), display_list->surface_clear_color(), visual_context_tree.visual_animation_summary().count, laid_out);
    prune_resource_storage(view->resource_storage, view->resource_storage_pruned_for, *document, *display_list, visual_context_tree);
    view->stats.last_record_ms = Karouka::now_ms() - record_start;

    // Only what changed since the frame the surface holds is rasterized: a paint or a visual
    // animation that changed nothing in the viewport keeps the surface's pixels, and one that did
    // replays the display list into its damage alone. A load, a resize or a new target draws the
    // whole frame (a new target or size has no frame to keep; a load is a new page).
    auto const& scroll_state = document->scroll_state_snapshot();
    Gfx::IntRect frame_rect { 0, 0, view->size.width().value(), view->size.height().value() };
    constexpr u8 partial_reasons = Karouka::RenderReasonPaint | Karouka::RenderReasonVisualAnimation | Karouka::RenderReasonVisualAnimationEnd | Karouka::RenderReasonCaretBlink | Karouka::RenderReasonShader;
    // Taken before a render that changes nothing in the viewport returns: its frame is the last one's.
    note_blinking_caret(*view, *display_list);
    view->draws_live_embedder_content = draws_live_embedder_content_in_viewport(*view, *display_list, visual_context_tree, scroll_state);
    view->live_embedder_content_time_ns = visual_animation_sample_time_ns();
    auto damage_rects = (reasons & ~partial_reasons) || full_raster_forced() ? DamageRects { frame_rect } : frame_damage(*view, *display_list, visual_context_tree, scroll_state);
    SkRegion damage;
    for (auto const& rect : damage_rects)
        damage.op(SkIRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()), SkRegion::kUnion_Op);
    u64 damage_pixels = 0;
    for (SkRegion::Iterator it(damage); !it.done(); it.next())
        damage_pixels += static_cast<u64>(it.rect().width()) * it.rect().height();
    if (damage.isEmpty()) {
        ++view->paints_without_viewport_damage;
        if (debug_enabled())
            Karouka::log(KR_LOG_DEBUG, "render skipped: the frame changed nothing in the viewport (record and diff {:.3f} ms)", Karouka::now_ms() - record_start);
        return KR_OK;
    }
    if (debug_enabled()) {
        StringBuilder rects;
        for (auto const& rect : damage_rects)
            rects.appendff("{}{},{} {}x{}", rects.is_empty() ? "" : " + ", rect.x(), rect.y(), rect.width(), rect.height());
        Karouka::log(KR_LOG_DEBUG, "render damage={} ({} of {} px)", rects.string_view(), damage_pixels, frame_rect.width() * frame_rect.height());
    }

    if (debug_enabled() && Karouka::census_enabled())
        Karouka::log(KR_LOG_DEBUG, "{}", Karouka::describe_display_list_census(*display_list, visual_context_tree, view->resource_storage, scroll_state, view->surface));

    // The worker warming a web font's glyphs stops before the replay: alongside it, the replay and
    // the flush would wait on the strikes it holds. The flush rasterizes what it didn't reach.
    if (view->system->web_font_glyphs)
        view->system->web_font_glyphs->stop();

    auto raster_start = Karouka::now_ms();
    // Glyph images the replay and flush made (Skia's strike cache grows by them): what warming didn't reach.
    auto glyph_cache_start = debug_enabled() ? SkGraphics::GetFontCacheUsed() : 0;
    // The programs a view's first frame needs go on the shader cache's warm list, which the next
    // run precompiles at kr_system_create2, and those of its next few frames on its later
    // list, precompiled after them while the main thread is busy. Skia builds them in the flush.
    bool recorded_frame = view->stats.frames_rendered < Karouka::ShaderCache::recorded_frames;
    if (recorded_frame && view->system->shader_cache)
        view->system->shader_cache->set_recording(view->stats.frames_rendered == 0 ? Karouka::ShaderCache::Recording::FirstFrame : Karouka::ShaderCache::Recording::LaterFrame);
    {
        // The software backend writes the IOSurface's pixels on the CPU, under its lock.
        Karouka::IOSurfaceCPUAccess cpu_access(view->system->software ? &view->buffer->iosurface_handle() : nullptr, false);
        replay_into_damage(*view, *document, *display_list, visual_context_tree, scroll_state, damage_rects);
        auto flush_start = Karouka::now_ms();
        // Flushes this view's surface for presentation and submits it to the shared queue. Both
        // flushes also age out cache entries no view has used for 5 s.
        // On the host's queue nothing waits (SkiaBackendContext::flush_and_submit_async, GrSyncCpu::kNo):
        // Metal runs the host's later command buffers on that queue after this one, which is all the
        // host needs to sample the frame, from the IOSurface or the texture it drew into (kr_view_set_output). The finished callback has
        // nothing to do; Skia tracks its own resources' GPU use.
        // On karouka's own queue the host has no such order, so this waits for the GPU
        // (SkiaBackendContext::flush_and_submit, GrSyncCpu::kYes): the frame is complete when the host
        // samples the IOSurface, and raster time includes GPU work (as tessera_viewer --headless
        // measures it). Views render one after another on one thread and each render ends with this
        // wait, so the context holds no other view's work when it starts and the wait covers exactly
        // this view's frame.
        // Without a GPU context (the software backend) the player's flush only finishes the surface:
        // the pixels are in the IOSurface once execute returns.
        // A collection a decode left to the frame boundary runs once the frame is submitted, while the
        // GPU draws it: on karouka's own queue the flush is split into its submit and its wait
        // (flush_and_submit's submit with GrSyncCpu::kYes waits for every command buffer submitted),
        // with the collection between them.
        if (view->system->host_queue) {
            view->player->flush_async(*view->surface, [] { });
            Karouka::LocalImageCodecPlugin::collect_decoded_images_at_frame_boundary();
        } else if (view->system->skia) {
            view->player->flush_async(*view->surface, [] { });
            Karouka::LocalImageCodecPlugin::collect_decoded_images_at_frame_boundary();
            view->system->skia->sk_context()->submit(GrSyncCpu::kYes);
        } else {
            view->player->flush(*view->surface);
            Karouka::LocalImageCodecPlugin::collect_decoded_images_at_frame_boundary();
        }
        // The flush is where Skia builds the frame's Metal pipelines, so the first frame's flush is
        // mostly shader compilation unless the persistent shader cache had them.
        if (debug_enabled())
            Karouka::log(KR_LOG_DEBUG, "render replay {:.3f} ms, flush {:.3f} ms, glyph cache {:+} bytes", flush_start - raster_start, Karouka::now_ms() - flush_start, static_cast<i64>(SkGraphics::GetFontCacheUsed()) - static_cast<i64>(glyph_cache_start));
    }
    if (recorded_frame && view->system->shader_cache)
        view->system->shader_cache->set_recording(Karouka::ShaderCache::Recording::None);
    if (recorded_frame && view->system->web_font_glyphs && view->system->web_font_glyphs->has_web_fonts()) {
        auto record_glyphs_start = Karouka::now_ms();
        view->system->web_font_glyphs->record(Karouka::WebFontGlyphs::page_hash(document->url().serialize()), *display_list, visual_context_tree, view->resource_storage, scroll_state, view->surface, damage_rects);
        if (debug_enabled())
            Karouka::log(KR_LOG_DEBUG, "web font glyphs: recorded the frame's in {:.3f} ms", Karouka::now_ms() - record_glyphs_start);
    }
    // Until the next render's replay, the worker warms what it hasn't reached, and the offscreen
    // glyphs of the fonts this render could draw.
    if (view->system->web_font_glyphs)
        view->system->web_font_glyphs->resume();
    view->surface_has_frame = true;
    view->last_rasterized_frame = kr_view::RasterizedFrame { *display_list, visual_context_tree, scroll_state };
    view->stats.last_raster_ms = Karouka::now_ms() - raster_start;
    view->stats.last_raster_pixels = damage_pixels;
    if (debug_enabled() && view->system->skia) {
        int resources = 0;
        size_t resource_bytes = 0;
        view->system->skia->sk_context()->getResourceCacheUsage(&resources, &resource_bytes);
        Karouka::log(KR_LOG_DEBUG, "gpu cache resources={} bytes={} budget={}", resources, resource_bytes, view->system->skia->sk_context()->getResourceCacheLimit());
    }

    ++view->stats.frames_rendered;
    for (size_t i = 0; i < Karouka::RenderReasonCount; ++i) {
        if (reasons & (1u << i))
            ++view->render_reason_counts[i];
    }
    rendered = true;
    return KR_OK;
}

kr_result kr_view_render(kr_view* view, bool* out_rendered)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    if (out_rendered)
        *out_rendered = false;
    if (view->pixel_lock)
        return KR_ERROR_INVALID_STATE;
    // Skia's Metal backend autoreleases the command buffers it makes (GrMtlCommandBuffer::Make,
    // through -[MTLCommandQueue commandBufferWithDescriptor:]) and what they hold. Without a pool of
    // our own they pile up in the host's until it drains one, which a C host's frame loop never
    // does: a headless loop kept every frame's command buffer, ~2 KB of system malloc a frame.
    Core::ScopedAutoreleasePool autorelease_pool;
    auto output = exchange(view->output, {});
    bool rendered = false;
    auto result = rasterize(view, output.has_value() ? &*output : nullptr, rendered);
    if (out_rendered)
        *out_rendered = rendered;
    return result;
}

kr_result kr_view_set_output2(kr_view* view, kr_metal_output const* host_output)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    if (!host_output) {
        view->output.clear();
        return KR_OK;
    }
    // A struct_size short of texture leaves it NULL, which validate_output refuses.
    kr_metal_output output;
    if (!Karouka::read_sized(*host_output, metal_output_members, output))
        return KR_ERROR_INVALID_ARGUMENT;
    if (!view->system->host_queue)
        return KR_ERROR_INVALID_STATE;
    if (auto result = Karouka::validate_output(view->system->skia->metal_context(), output); result != KR_OK)
        return result;
    view->output = output;
    return KR_OK;
}

void* kr_view_iosurface(kr_view* view)
{
    if (!view || !view->buffer.has_value())
        return nullptr;
    return view->buffer->iosurface_handle().core_foundation_pointer();
}

kr_result kr_view_lock_pixels(kr_view* view, void const** out_pixels, size_t* out_bytes_per_row)
{
    if (!view || !out_pixels || !out_bytes_per_row)
        return KR_ERROR_INVALID_ARGUMENT;
    if (view->pixel_lock || !view->buffer.has_value() || !view->surface_has_frame)
        return KR_ERROR_INVALID_STATE;
    // Read-only: IOSurfaceLock makes what the GPU or the software rasterizer wrote visible to the
    // CPU, and the surface's seed is left alone.
    auto lock = make<Karouka::IOSurfaceCPUAccess>(&view->buffer->iosurface_handle(), true);
    if (!lock->is_locked())
        return KR_ERROR_INTERNAL;
    *out_pixels = lock->base_address();
    *out_bytes_per_row = lock->bytes_per_row();
    view->pixel_lock = move(lock);
    return KR_OK;
}

kr_result kr_view_unlock_pixels(kr_view* view)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    if (!view->pixel_lock)
        return KR_ERROR_INVALID_STATE;
    view->pixel_lock = nullptr;
    return KR_OK;
}

kr_result kr_view_capture_png(kr_view* view, char const* path)
{
    if (!view || !path || !view->surface)
        return KR_ERROR_INVALID_ARGUMENT;
    // A GPU surface's readback autoreleases a command buffer, a blit encoder and a buffer.
    Core::ScopedAutoreleasePool autorelease_pool;
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { view->size.width().value(), view->size.height().value() });
    if (bitmap.is_error())
        return KR_ERROR_INTERNAL;
    {
        Karouka::IOSurfaceCPUAccess cpu_access(view->system->software ? &view->buffer->iosurface_handle() : nullptr, true);
        view->surface->read_into_bitmap(*bitmap.value());
    }
    auto png = Gfx::PNGWriter::encode(*bitmap.value());
    if (png.is_error())
        return KR_ERROR_INTERNAL;
    auto file = Core::File::open(StringView { path, strlen(path) }, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error() || file.value()->write_until_depleted(png.value()).is_error())
        return KR_ERROR_IO;
    return KR_OK;
}

// Every canvas's surfaces; the canvas host is the system's, so this is the same for every view.
static size_t canvas_bytes(kr_system const& system)
{
    return system.canvas_host ? system.canvas_host->surface_bytes() : 0;
}

static kr_view_stats current_stats(kr_view* view)
{
    // The system malloc zones' bytes in use, the same figure for every view. It leaves out the GC
    // heap and kmalloc (mimalloc); kr_system_query_memory_stats has those totals.
    view->stats.malloc_bytes = mstats().bytes_used;
    view->stats.layout_updates = view->client->layout_updates;
    view->stats.event_loop_pumps = Karouka::event_loop_pumps();
    view->stats.canvas_bytes = canvas_bytes(*view->system);
    return view->stats;
}

kr_result kr_view_query_stats(kr_view* view, kr_view_stats* out_stats)
{
    if (!view || !out_stats || !Karouka::write_sized(current_stats(view), view_stats_members, *out_stats))
        return KR_ERROR_INVALID_ARGUMENT;
    return KR_OK;
}

// VM_MEMORY_APPLICATION_SPECIFIC_1 and _2 (the latter is missing from SDKs before macOS 26).
static constexpr unsigned kmalloc_vm_tag = 240;
static constexpr unsigned gc_heap_vm_tag = 241;

// LibGC reserves two 4 TiB ranges (tag 241), which the kernel keeps as ~32k map entries each: its
// heap region, whose 2 MiB chunks are committed in order from its base, so the first inaccessible
// entry in it ends what is in use; and its primitive storage cage (strings, array buffers), whose
// allocations lie anywhere in it. Walking every entry of both took ~60 ms on a demo page.
static mach_vm_address_t gc_heap_region_end()
{
    return js_heap_region_base ? js_heap_region_base + GC::HEAP_REGION_SIZE : 0;
}

static mach_vm_address_t primitive_storage_cage_end()
{
    // The cage is followed by one inaccessible guard page (PrimitiveStorage::Allocator::ensure_cage).
    return js_primitive_storage_cage_base ? js_primitive_storage_cage_base + GC::PrimitiveStorage::default_cage_size + vm_page_size : 0;
}

enum class PrimitiveStorageCage {
    Walk,
    Skip,
};

// Calls callback(address, size, user_tag) for every accessible VM region of the process in
// [start, end), past the unused part of the GC heap region and, with PrimitiveStorageCage::Skip,
// past the whole primitive storage cage. The short submap info doesn't count each region's resident
// pages, as VM_REGION_EXTENDED_INFO does (that alone was ~10 ms a walk).
// (C++ linkage: this file's C API definitions sit in an extern "C" block.)
extern "C++" {
template<typename Callback>
static void for_each_accessible_vm_region(mach_vm_address_t start, mach_vm_address_t end, PrimitiveStorageCage cage, Callback callback)
{
    mach_vm_address_t address = start;
    natural_t depth = 0;
    while (address < end) {
        mach_vm_size_t size = 0;
        vm_region_submap_short_info_data_64_t info {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS || address >= end)
            break;
        if (cage == PrimitiveStorageCage::Skip && js_primitive_storage_cage_base && address >= js_primitive_storage_cage_base && address < primitive_storage_cage_end()) {
            address = primitive_storage_cage_end();
            continue;
        }
        if (info.protection == VM_PROT_NONE && js_heap_region_base && address >= js_heap_region_base && address < gc_heap_region_end()) {
            address = gc_heap_region_end();
            continue;
        }
        if (info.protection != VM_PROT_NONE && !info.is_submap)
            callback(address, size, info.user_tag);
        address += size;
    }
}
}

// kmalloc (mimalloc) and LibGC tag their VM regions (AK/kmalloc.cpp's mi_option_os_tag,
// Core::System::MemoryTag::GarbageCollector). What they add to the footprint is their pages that are
// dirty and not reusable (mimalloc purges with MADV_FREE_REUSABLE) or compressed, page by page as
// vmmap and footprint(1) count them. Reserved, inaccessible space is skipped. Without gc_heap_bytes
// the primitive storage cage isn't walked.
static void count_tagged_dirty_bytes(u64* kmalloc_bytes, u64* gc_heap_bytes)
{
    auto cage = gc_heap_bytes ? PrimitiveStorageCage::Walk : PrimitiveStorageCage::Skip;
    for_each_accessible_vm_region(0, NumericLimits<mach_vm_address_t>::max(), cage, [&](mach_vm_address_t address, mach_vm_size_t size, unsigned user_tag) {
        u64* total = user_tag == kmalloc_vm_tag ? kmalloc_bytes : user_tag == gc_heap_vm_tag ? gc_heap_bytes : nullptr;
        if (!total)
            return;
        static constexpr size_t chunk_pages = 4096;
        int dispositions[chunk_pages];
        for (mach_vm_size_t offset = 0; offset < size; offset += chunk_pages * vm_page_size) {
            mach_vm_size_t pages = min<mach_vm_size_t>(chunk_pages, (size - offset) / vm_page_size);
            if (mach_vm_page_range_query(mach_task_self(), address + offset, pages * vm_page_size, reinterpret_cast<mach_vm_address_t>(dispositions), &pages) != KERN_SUCCESS)
                break;
            for (mach_vm_size_t i = 0; i < pages; ++i) {
                auto disposition = dispositions[i];
                bool dirty = (disposition & VM_PAGE_QUERY_PAGE_DIRTY) && !(disposition & VM_PAGE_QUERY_PAGE_REUSABLE);
                if (dirty || (disposition & VM_PAGE_QUERY_PAGE_PAGED_OUT))
                    *total += vm_page_size;
            }
        }
    });
}

// The GC heap's blocks in use and the bytes of their live cells. LibGC keeps no byte totals it
// exposes, so this finds the blocks itself: its BlockAllocator carves 16 KiB HeapBlocks out of the
// chunks committed in the heap region, and the heap knows which of those slots hold a block in use
// (is_live_heap_block). Only those are read; free slots may be decommitted. The region is shared by
// every Heap, but karouka has one, the main thread VM's.
static void count_gc_heap_bytes(u64* live_bytes, u64* allocated_bytes)
{
    if (!js_heap_region_base)
        return;
    auto& heap = Web::Bindings::main_thread_vm().heap();
    for_each_accessible_vm_region(js_heap_region_base, gc_heap_region_end(), PrimitiveStorageCage::Skip, [&](mach_vm_address_t address, mach_vm_size_t size, unsigned) {
        auto first = align_up_to(address, GC::HeapBlock::BLOCK_SIZE);
        for (auto slot = first; slot + GC::HeapBlock::BLOCK_SIZE <= address + size; slot += GC::HeapBlock::BLOCK_SIZE) {
            auto* block = reinterpret_cast<GC::HeapBlock*>(slot);
            if (!heap.is_live_heap_block(block))
                continue;
            *allocated_bytes += GC::HeapBlock::BLOCK_SIZE;
            size_t live_cells = 0;
            block->for_each_cell_in_state<GC::Cell::State::Live>([&](GC::Cell*) { ++live_cells; });
            *live_bytes += live_cells * block->cell_size();
        }
    });
}

static u64 gpu_cache_bytes(kr_system const& system)
{
    if (!system.skia)
        return 0;
    size_t bytes = 0;
    system.skia->sk_context()->getResourceCacheUsage(nullptr, &bytes);
    return bytes;
}

static u64 surface_bytes(kr_system const& system)
{
    u64 bytes = 0;
    for (auto* view : system.views) {
        if (view->buffer.has_value())
            bytes += static_cast<u64>(view->size.width().value()) * view->size.height().value() * 4;
    }
    return bytes;
}

// Each view's last rasterized display list and the nested ones it draws (SVG images,
// cached subtrees), from the view's resource storage. What the storage keeps beyond those, and its
// text-blob and tile caches, has no public accessor.
static u64 display_list_bytes(kr_system const& system)
{
    u64 bytes = 0;
    for (auto* view : system.views) {
        if (!view->last_rasterized_frame.has_value())
            continue;
        auto const& frame = *view->last_rasterized_frame;
        bytes += frame.display_list->command_bytes().size();
        auto referenced = view->resource_storage.collect_referenced_resources(*frame.display_list);
        referenced.include(view->resource_storage.collect_referenced_resources(frame.visual_context_tree));
        for (auto id : referenced.display_lists) {
            if (view->resource_storage.has_display_list(id))
                bytes += view->resource_storage.display_list(id).command_bytes().size();
        }
    }
    return bytes;
}

kr_result kr_system_query_memory_stats(kr_system* system, kr_memory_stats* out_stats)
{
    // Refused before the walk below (~45 ms) is paid for.
    if (!system || !out_stats || Karouka::whole_members_end(out_stats->struct_size, memory_stats_members) == 0)
        return KR_ERROR_INVALID_ARGUMENT;
    kr_memory_stats stats {};

    task_vm_info_data_t vm_info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS)
        stats.footprint_bytes = vm_info.phys_footprint;
    stats.malloc_zone_bytes = mstats().bytes_used;
    count_tagged_dirty_bytes(&stats.kmalloc_bytes, &stats.gc_heap_bytes);
    stats.gpu_cache_bytes = gpu_cache_bytes(*system);
    if (system->skia)
        stats.gpu_cache_budget_bytes = system->skia->sk_context()->getResourceCacheLimit();
    stats.surface_bytes = surface_bytes(*system);
    stats.font_cache_strikes = static_cast<u64>(SkGraphics::GetFontCacheCountUsed());
    Karouka::write_sized(stats, memory_stats_members, *out_stats);
    return KR_OK;
}

size_t kr_system_memory_usage(kr_system const* system, kr_mem_tag tag)
{
    if (!system)
        return 0;
    switch (tag) {
    case KR_MEM_TAG_GC_HEAP_LIVE:
    case KR_MEM_TAG_GC_HEAP_ALLOCATED: {
        u64 live = 0;
        u64 allocated = 0;
        count_gc_heap_bytes(&live, &allocated);
        return tag == KR_MEM_TAG_GC_HEAP_LIVE ? live : allocated;
    }
    case KR_MEM_TAG_GPU_CACHE:
        return gpu_cache_bytes(*system);
    case KR_MEM_TAG_DISPLAY_LIST:
        return display_list_bytes(*system);
    case KR_MEM_TAG_DECODED_IMAGES:
        return Karouka::LocalImageCodecPlugin::decoded_image_bytes();
    case KR_MEM_TAG_DECODED_IMAGES_PEAK:
        return Karouka::LocalImageCodecPlugin::decoded_image_peak_bytes();
    case KR_MEM_TAG_FONT_CACHE:
        return SkGraphics::GetFontCacheUsed();
    case KR_MEM_TAG_MALLOC_TOTAL: {
        u64 kmalloc = 0;
        count_tagged_dirty_bytes(&kmalloc, nullptr);
        return mstats().bytes_used + kmalloc;
    }
    case KR_MEM_TAG_SURFACES:
        return surface_bytes(*system);
    case KR_MEM_TAG_CANVAS:
        return canvas_bytes(*system);
    default:
        return 0;
    }
}

char const* kr_mem_tag_name(kr_mem_tag tag)
{
    switch (tag) {
    case KR_MEM_TAG_GC_HEAP_LIVE:
        return "gc_heap_live";
    case KR_MEM_TAG_GC_HEAP_ALLOCATED:
        return "gc_heap_allocated";
    case KR_MEM_TAG_GPU_CACHE:
        return "gpu_cache";
    case KR_MEM_TAG_DISPLAY_LIST:
        return "display_list";
    case KR_MEM_TAG_DECODED_IMAGES:
        return "decoded_images";
    case KR_MEM_TAG_DECODED_IMAGES_PEAK:
        return "decoded_images_peak";
    case KR_MEM_TAG_FONT_CACHE:
        return "font_cache";
    case KR_MEM_TAG_MALLOC_TOTAL:
        return "malloc_total";
    case KR_MEM_TAG_SURFACES:
        return "surfaces";
    case KR_MEM_TAG_CANVAS:
        return "canvas";
    default:
        return nullptr;
    }
}

// Host io: Karouka::HostIo (HostInterface.h).

kr_result kr_io_complete(kr_system* system, uint64_t request_id, kr_result status, void const* data, size_t size, kr_io_ownership ownership)
{
    if (!system || system != s_system)
        return KR_ERROR_INVALID_ARGUMENT;
    return Karouka::HostIo::complete(request_id, status, data, size, ownership);
}

// Data binding: the model is Karouka::BindingModel (BindingModel.h), one per view.

kr_result kr_binding_path_resolve(kr_view* view, char const* path, kr_binding_path* out_path)
{
    if (!view || !path || !out_path)
        return KR_ERROR_INVALID_ARGUMENT;
    return view->binding.resolve(StringView { path, strlen(path) }, out_path);
}

kr_result kr_binding_set_number(kr_view* view, kr_binding_path path, double value)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    return view->binding.set_number(path, value);
}

kr_result kr_binding_set_bool(kr_view* view, kr_binding_path path, bool value)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    return view->binding.set_bool(path, value);
}

kr_result kr_binding_set_string(kr_view* view, kr_binding_path path, char const* value, size_t length)
{
    if (!view || (!value && length))
        return KR_ERROR_INVALID_ARGUMENT;
    return view->binding.set_string(path, StringView { value, length });
}

// Page <-> host messages: Karouka::EngineChannel (EngineChannel.h), one per view.

kr_result kr_view_set_engine_callbacks2(kr_view* view, kr_engine_callbacks const* host_callbacks)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    // The members past a short struct_size are NULL: no such callback.
    kr_engine_callbacks callbacks;
    if (host_callbacks && !Karouka::read_sized(*host_callbacks, engine_callbacks_members, callbacks))
        return KR_ERROR_INVALID_ARGUMENT;
    view->engine.set_callbacks(host_callbacks ? &callbacks : nullptr);
    return KR_OK;
}

kr_result kr_view_engine_trigger(kr_view* view, char const* name, char const* json_args)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    return view->engine.host_trigger(name, json_args);
}

kr_result kr_view_engine_resolve(kr_view* view, uint64_t call_id, bool success, char const* json_value)
{
    if (!view)
        return KR_ERROR_INVALID_ARGUMENT;
    return view->engine.host_resolve(call_id, success, json_value);
}

}
