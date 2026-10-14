/*
 * shader() images: the pixels of a DrawEmbedderContent display-list command (patch 0101) come from
 * here. LibWeb records the command with an id karouka gave the content; at replay the view's player
 * calls the system's EmbedderContentRegistry with that id, the destination rectangle, its canvas
 * (set to the command's transform and clip) and the frame's host time, and the registry runs the
 * content's painter. An id with no painter paints nothing. A painter is given the frame's time since the
 * time origin the registry was last given, which the view sets to its document's timeline origin.
 *
 * ShaderImages compiles a shader()'s SkSL and registers its painters: LibWeb asks it, through
 * Web::Embedder::set_shader_content_hook, for the content id that paints a source at a raster scale.
 */
#pragma once

#include <AK/BitCast.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/kmalloc.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/DisplayListPlayerSkia.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Embedder/ShaderDecodedImageData.h>
#include <core/SkRefCnt.h>
#include <karouka/karouka.h>

class SkCanvas;
class SkData;
class SkRuntimeEffect;

namespace Gfx {
class PaintingSurface;
}

namespace Karouka {

// Draws one embedder content into dst_rect on the canvas, for the frame at time_ns: the host clock's
// time a blinking caret is taken at (kr_view_advance_time's), less the registry's time origin.
using EmbedderContentPainter = Function<void(SkCanvas&, Gfx::FloatRect const& dst_rect, i64 time_ns)>;
// A painter for a family of contents, one per 32-bit key, all registered at once: it is given the key
// of the content id it draws (the id's low 32 bits; family_content_id).
using EmbedderContentFamilyPainter = Function<void(u32 key, SkCanvas&, Gfx::FloatRect const& dst_rect, i64 time_ns)>;

// Content ids below 2^32 name one painter each (add); an id at or above it is a family's (add_family):
// its high 32 bits are the family, its low 32 bits the key the family's painter is given. So a family
// costs one entry however many of its ids display lists hold, and no id needs freeing while a list
// still names it.
class EmbedderContentRegistry {
    AK_MAKE_NONCOPYABLE(EmbedderContentRegistry);
    AK_MAKE_NONMOVABLE(EmbedderContentRegistry);

public:
    EmbedderContentRegistry();

    // Whether a content's pixels change with the frame's time (a shader() declaring uTime, the test content), or
    // only with the canvases it samples (a shader() with canvas inputs alone), which its painter reports with
    // note_sampled_canvas as it draws.
    enum class DependsOnTime : bool {
        No,
        Yes,
    };

    // The id a DrawEmbedderContent command names the content by; never 0.
    u64 add(EmbedderContentPainter, DependsOnTime = DependsOnTime::Yes);
    // The family's handle (never 0); its content ids are family_content_id(family, key).
    u64 add_family(EmbedderContentFamilyPainter, DependsOnTime = DependsOnTime::Yes);
    static u64 family_content_id(u64 family, u32 key) { return (family << 32) | key; }
    // Takes an id from add, or a family handle from add_family.
    void remove(u64 content_id_or_family);
    // Painters and families registered.
    size_t size() const { return m_painters.size() + m_family_painters.size(); }
    bool is_empty() const { return size() == 0; }

    // What a player draws DrawEmbedderContent commands with (DisplayListPlayerSkia::set_embedder_content_resolver).
    Compositing::EmbedderContentResolver const& resolver() const { return m_resolver; }

    // The host clock's time (ns) the painters' time_ns counts from, until set again: the timeline origin of the
    // document whose frame is drawn next (a view sets it before each replay). 0 at first.
    void set_time_origin_ns(i64 time_origin_ns) { m_time_origin_ns = time_origin_ns; }

    // What the painters drew between begin_replay and take_replay: whether any content that depends on time, and
    // the canvases (id and content generation, as CanvasSurfaceRegistry has them) that content sampled. A view
    // whose frame drew only canvas-sampling content need render again only when one of those canvases moves.
    struct Replay {
        bool drew_content_depending_on_time { false };
        struct SampledCanvas {
            u64 canvas_id { 0 };
            u64 content_generation { 0 };
            bool operator==(SampledCanvas const&) const = default;
        };
        Vector<SampledCanvas> sampled_canvases;
        bool drew_anything() const { return drew_content_depending_on_time || !sampled_canvases.is_empty(); }
    };
    void begin_replay() { m_replay = {}; }
    Replay take_replay() { return move(m_replay); }
    // A painter sampled this canvas at this content generation.
    void note_sampled_canvas(u64 canvas_id, u64 content_generation);

private:
    template<typename Painter>
    struct Entry {
        Painter painter;
        DependsOnTime depends_on_time { DependsOnTime::Yes };
    };
    i64 m_time_origin_ns { 0 };
    u64 m_next_content_id { 1 };
    u64 m_next_family { 1 };
    HashMap<u64, Entry<EmbedderContentPainter>> m_painters;
    HashMap<u64, Entry<EmbedderContentFamilyPainter>> m_family_painters;
    Replay m_replay;
    Compositing::EmbedderContentResolver m_resolver;
};

// shader() sources compiled with Skia's SkRuntimeEffect::MakeForShader (strict ES2, SkSL's
// `half4 main(float2 coord)`), each compiled once and kept by its text, and the embedder content that
// paints them. A shader paints into the DrawEmbedderContent's destination rectangle, which is the
// nested list's pixels at raster_scale per CSS pixel; its coord is in CSS pixels from the rectangle's
// top-left. It is given the uniforms it declares of these, and only those:
//   uniform float2 uSize;              the rectangle's size in CSS pixels
//   uniform float uDevicePixelRatio;   raster_scale
//   uniform float uTime;               the frame's time in seconds since the document's timeline origin
//                                      (the painter's time_ns), so the shader animates on the host clock
// A shader that declares uTime is live: its DrawEmbedderContent draws at every frame's time with no
// style, layout or paint from LibWeb, its bounds are damaged every frame they are on screen, and a view
// that drew one on screen renders at each new host time. One that doesn't is painted like a gradient.
// Every other uniform it declares is the author's: the image's `--name value` argument sets uniform
// `name` (shader(url(x.sksl), --speed 0.3) sets `uniform float speed`). A float or half takes a
// <number>, a <percentage> (divided by 100) or a <length> (in CSS px); a float2/half2 two numbers; a
// float4/half4 a <color> (unpremultiplied RGBA in 0-1). An author uniform with no argument or with an
// argument of another type paints nothing; an argument no uniform takes is ignored. Each of these
// logs one line for each source and set of arguments, the first time it is asked for.
// A `uniform shader name;` child is a canvas input: the image's `--name element(#id)` argument binds the <canvas>
// with that id (LibWeb resolves it, Web::Embedder::ShaderArgument::Type::Canvas), and the child samples the
// canvas's presented pixels, one canvas pixel per CSS pixel of the shader's coord, transparent outside it. A
// child with no such argument, or with one LibWeb refused (no such element, not a canvas, a canvas inside the
// element painted, one that isn't origin-clean or has no content yet), paints nothing and logs one line, as a
// canvas tainted after it was bound does at replay. A shader with a canvas input is live, as one declaring uTime
// is, but it depends on its canvases rather than on time: a view renders again for it when one of the canvases its
// last frame sampled presents new content (EmbedderContentRegistry::Replay).
// A shader that doesn't compile, or that declares a child color filter or blender, a uniform of another type (int,
// float3, a matrix, an array) or uSize/uDevicePixelRatio/uTime with another type, paints nothing; the log gets one line about it the first time its source is asked for
// (Skia's message for a compile error).
// Each shader that paints is one registry family per set of argument values, its key the raster
// scale's bits, so a scale that keeps changing (an animated transform) adds no registry entries. The
// families stay registered while the ShaderImages lives, so an argument whose value keeps changing
// (an animated var()) adds one for each value it takes: display lists retain the ids, and nothing
// tells the registry when the last one naming a family is gone.
class ShaderImages {
    AK_MAKE_NONCOPYABLE(ShaderImages);
    AK_MAKE_NONMOVABLE(ShaderImages);
    AK_ALLOC_WITH_KMALLOC;

public:
    using Log = Function<void(kr_log_level, StringView message)>;
    // A canvas a shader samples, by its id in the canvas surface registry: its presented surface (null when it has
    // none), its content generation, and whether script may read its pixels.
    struct CanvasInput {
        Gfx::PaintingSurface const* surface { nullptr };
        u64 content_generation { 0 };
        bool origin_clean { false };
    };
    using CanvasInputs = Function<CanvasInput(u64 canvas_id)>;

    ShaderImages(EmbedderContentRegistry&, Log, CanvasInputs = {});
    ~ShaderImages();

    // What Web::Embedder::set_shader_content_hook takes: the content that paints `source` with
    // `arguments` at raster_scale (live when the shader declares uTime), or nothing when it paints nothing.
    Optional<Web::Embedder::ShaderContent> content(StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments = {});
    Optional<u64> content_id(StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments = {})
    {
        auto shader_content = content(source, raster_scale, arguments);
        if (!shader_content.has_value())
            return {};
        return shader_content->content_id;
    }

    size_t shader_count() const { return m_shaders.size(); }

    // The raster scale a content id from content_id() paints at.
    static float raster_scale_of(u64 content_id) { return bit_cast<float>(static_cast<u32>(content_id)); }

private:
    struct Shader {
        AK_ALLOC_WITH_KMALLOC;

        // Null when the source paints nothing.
        sk_sp<SkRuntimeEffect> effect;
        Optional<size_t> size_offset;
        Optional<size_t> device_pixel_ratio_offset;
        Optional<size_t> time_offset;
        // The uniforms the image's arguments set, in declaration order.
        struct AuthorUniform {
            ByteString name;
            size_t offset { 0 };
            size_t float_count { 1 };
        };
        Vector<AuthorUniform> author_uniforms;
        // The `uniform shader` children, in declaration order (SkRuntimeEffect::children()), each a canvas input.
        Vector<ByteString> children;
        // The registry family that paints it with each set of arguments (by arguments_key) at every
        // raster scale, once asked for; nothing for a set that paints nothing.
        HashMap<ByteString, Optional<u64>> families;
    };
    NonnullOwnPtr<Shader> compile(StringView source);
    // The uniform values `arguments` set, or nothing (logged) when they don't fit the shader.
    Optional<sk_sp<SkData>> pack_author_uniforms(Shader const&, ReadonlySpan<Web::Embedder::ShaderArgument>);
    // The canvas ids of the shader's children, in order, or nothing (logged) when an argument doesn't bind one.
    Optional<Vector<u64>> bind_children(Shader const&, ReadonlySpan<Web::Embedder::ShaderArgument>);
    static ByteString arguments_key(ReadonlySpan<Web::Embedder::ShaderArgument>);
    struct BuiltinUniforms {
        Optional<size_t> size_offset;
        Optional<size_t> device_pixel_ratio_offset;
        Optional<size_t> time_offset;
    };
    struct ChildCanvases {
        AK_ALLOC_WITH_KMALLOC;

        Vector<u64> canvas_ids;
        Vector<ByteString> names;
        // Whether a tainted canvas was already logged for this family.
        bool logged_refusal { false };
    };
    void paint_shader(SkRuntimeEffect const&, BuiltinUniforms, SkData const* author_uniforms, ChildCanvases*, float raster_scale, i64 time_ns, SkCanvas&, Gfx::FloatRect const& dst_rect);

    EmbedderContentRegistry& m_registry;
    Log m_log;
    CanvasInputs m_canvas_inputs;
    HashMap<ByteString, NonnullOwnPtr<Shader>> m_shaders;
};

// Test content: dst_rect filled with one opaque color, which changes
// every whole second of host time (solid_color_test_content_color).
EmbedderContentPainter solid_color_test_content();
Gfx::Color solid_color_test_content_color(i64 time_ns);

}
