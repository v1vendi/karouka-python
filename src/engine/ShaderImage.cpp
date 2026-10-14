/*
 * shader() images' embedder content registry and SkSL compiler; see ShaderImage.h.
 */
#include "ShaderImage.h"

#include <AK/AnyOf.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <core/SkCanvas.h>
#include <core/SkData.h>
#include <core/SkImage.h>
#include <core/SkSamplingOptions.h>
#include <LibGfx/PaintingSurface.h>
#include <core/SkMatrix.h>
#include <core/SkPaint.h>
#include <core/SkRect.h>
#include <core/SkShader.h>
#include <core/SkString.h>
#include <effects/SkRuntimeEffect.h>
#include <math.h>
#include <string.h>

namespace Karouka {

EmbedderContentRegistry::EmbedderContentRegistry()
    : m_resolver([this](u64 content_id, Gfx::FloatRect const& dst_rect, SkCanvas& canvas, i64 time_ns) {
        if (auto family = content_id >> 32; family != 0) {
            auto entry = m_family_painters.find(family);
            if (entry == m_family_painters.end())
                return;
            m_replay.drew_content_depending_on_time |= entry->value.depends_on_time == DependsOnTime::Yes;
            entry->value.painter(static_cast<u32>(content_id), canvas, dst_rect, time_ns - m_time_origin_ns);
            return;
        }
        auto entry = m_painters.find(content_id);
        if (entry == m_painters.end())
            return;
        m_replay.drew_content_depending_on_time |= entry->value.depends_on_time == DependsOnTime::Yes;
        entry->value.painter(canvas, dst_rect, time_ns - m_time_origin_ns);
    })
{
}

u64 EmbedderContentRegistry::add(EmbedderContentPainter painter, DependsOnTime depends_on_time)
{
    auto content_id = m_next_content_id++;
    VERIFY(content_id >> 32 == 0);
    m_painters.set(content_id, { move(painter), depends_on_time });
    return content_id;
}

u64 EmbedderContentRegistry::add_family(EmbedderContentFamilyPainter painter, DependsOnTime depends_on_time)
{
    auto family = m_next_family++;
    VERIFY(family >> 32 == 0);
    m_family_painters.set(family, { move(painter), depends_on_time });
    return family;
}

void EmbedderContentRegistry::note_sampled_canvas(u64 canvas_id, u64 content_generation)
{
    Replay::SampledCanvas sampled { canvas_id, content_generation };
    if (!m_replay.sampled_canvases.contains_slow(sampled))
        m_replay.sampled_canvases.append(sampled);
}

void EmbedderContentRegistry::remove(u64 content_id_or_family)
{
    if (!m_painters.remove(content_id_or_family))
        m_family_painters.remove(content_id_or_family);
}

ShaderImages::ShaderImages(EmbedderContentRegistry& registry, Log log, CanvasInputs canvas_inputs)
    : m_registry(registry)
    , m_log(move(log))
    , m_canvas_inputs(move(canvas_inputs))
{
}

ShaderImages::~ShaderImages()
{
    for (auto& shader : m_shaders) {
        for (auto& family : shader.value->families) {
            if (family.value.has_value())
                m_registry.remove(*family.value);
        }
    }
}

NonnullOwnPtr<ShaderImages::Shader> ShaderImages::compile(StringView source)
{
    auto shader = make<Shader>();
    auto result = SkRuntimeEffect::MakeForShader(SkString(source.characters_without_null_termination(), source.length()));
    if (!result.effect) {
        m_log(KR_LOG_ERROR, ByteString::formatted("shader(): the SkSL doesn't compile, so the image paints nothing: {}", StringView { result.errorText.c_str(), result.errorText.size() }));
        return shader;
    }
    for (auto const& child : result.effect->children()) {
        StringView name { child.name.data(), child.name.size() };
        if (child.type != SkRuntimeEffect::ChildType::kShader) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): {} is a child color filter or blender, which no CSS argument supplies (a `uniform shader` takes element(#id)), so the image paints nothing", name));
            return shader;
        }
        shader->children.append(ByteString { name });
    }
    for (auto const& uniform : result.effect->uniforms()) {
        StringView name { uniform.name.data(), uniform.name.size() };
        auto is_single = [&](SkRuntimeEffect::Uniform::Type type) { return uniform.type == type && !uniform.isArray(); };
        if (name == "uSize"sv && is_single(SkRuntimeEffect::Uniform::Type::kFloat2)) {
            shader->size_offset = uniform.offset;
        } else if (name == "uDevicePixelRatio"sv && is_single(SkRuntimeEffect::Uniform::Type::kFloat)) {
            shader->device_pixel_ratio_offset = uniform.offset;
        } else if (name == "uTime"sv && is_single(SkRuntimeEffect::Uniform::Type::kFloat)) {
            shader->time_offset = uniform.offset;
        } else if (name == "uSize"sv || name == "uDevicePixelRatio"sv || name == "uTime"sv) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): uniform {} must be a {}, so the image paints nothing", name, name == "uSize"sv ? "float2"sv : "float"sv));
            return shader;
        } else if (is_single(SkRuntimeEffect::Uniform::Type::kFloat) || is_single(SkRuntimeEffect::Uniform::Type::kFloat2) || is_single(SkRuntimeEffect::Uniform::Type::kFloat4)) {
            // An author uniform, set by the image's --name argument.
            auto float_count = uniform.type == SkRuntimeEffect::Uniform::Type::kFloat ? 1u : uniform.type == SkRuntimeEffect::Uniform::Type::kFloat2 ? 2u : 4u;
            shader->author_uniforms.append({ ByteString { name }, uniform.offset, float_count });
        } else {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): no CSS argument sets uniform {} (a float, float2 or float4, or their half forms, can be set), so the image paints nothing", name));
            return shader;
        }
    }
    shader->effect = move(result.effect);
    return shader;
}

static StringView describe(Web::Embedder::ShaderArgument::Type type)
{
    switch (type) {
    case Web::Embedder::ShaderArgument::Type::Number:
        return "a <number>"sv;
    case Web::Embedder::ShaderArgument::Type::Percentage:
        return "a <percentage>"sv;
    case Web::Embedder::ShaderArgument::Type::Length:
        return "a <length>"sv;
    case Web::Embedder::ShaderArgument::Type::NumberPair:
        return "two numbers"sv;
    case Web::Embedder::ShaderArgument::Type::Color:
        return "a <color>"sv;
    case Web::Embedder::ShaderArgument::Type::Canvas:
    case Web::Embedder::ShaderArgument::Type::RefusedCanvas:
        return "element(), a canvas"sv;
    case Web::Embedder::ShaderArgument::Type::Unsupported:
        break;
    }
    return "a value no uniform takes"sv;
}

static StringView describe_uniform(size_t float_count)
{
    return float_count == 1 ? "a float"sv : float_count == 2 ? "a float2"sv : "a float4"sv;
}

ByteString ShaderImages::arguments_key(ReadonlySpan<Web::Embedder::ShaderArgument> arguments)
{
    StringBuilder key;
    for (auto const& argument : arguments) {
        key.append(argument.name);
        key.append('\0');
        key.append(static_cast<char>(argument.type));
        key.append(StringView { reinterpret_cast<char const*>(argument.values.data()), argument.value_count() * sizeof(float) });
        if (argument.type == Web::Embedder::ShaderArgument::Type::Canvas || argument.type == Web::Embedder::ShaderArgument::Type::RefusedCanvas) {
            key.append(StringView { reinterpret_cast<char const*>(&argument.canvas_id), sizeof(argument.canvas_id) });
            key.append(static_cast<char>(argument.canvas_refusal));
            key.append(argument.element_id);
            key.append('\0');
        }
    }
    return key.to_byte_string();
}

Optional<sk_sp<SkData>> ShaderImages::pack_author_uniforms(Shader const& shader, ReadonlySpan<Web::Embedder::ShaderArgument> arguments)
{
    auto uniforms = SkData::MakeZeroInitialized(shader.effect->uniformSize());
    auto* bytes = static_cast<u8*>(uniforms->writable_data());
    auto find_argument = [&](StringView uniform_name) -> Web::Embedder::ShaderArgument const* {
        for (auto const& argument : arguments) {
            auto name = argument.name.bytes_as_string_view();
            if (name.starts_with("--"sv) && name.substring_view(2) == uniform_name)
                return &argument;
        }
        return nullptr;
    };
    for (auto const& uniform : shader.author_uniforms) {
        auto const* argument = find_argument(uniform.name);
        if (!argument) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): uniform {} has no --{} argument, so the image paints nothing", uniform.name, uniform.name));
            return {};
        }
        if (argument->value_count() != uniform.float_count) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): {} is {}, but uniform {} is {}, so the image paints nothing", argument->name, describe(argument->type), uniform.name, describe_uniform(uniform.float_count)));
            return {};
        }
        memcpy(bytes + uniform.offset, argument->values.data(), uniform.float_count * sizeof(float));
    }
    for (auto const& argument : arguments) {
        auto name = argument.name.bytes_as_string_view();
        auto names = [&](ByteString const& uniform_name) { return name.starts_with("--"sv) && name.substring_view(2) == uniform_name.view(); };
        auto sets_uniform = any_of(shader.author_uniforms, [&](auto const& uniform) { return names(uniform.name); }) || any_of(shader.children, names);
        if (!sets_uniform)
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): {} sets no uniform the shader declares, so it is ignored", name));
    }
    return uniforms;
}

static StringView describe(Web::Embedder::ShaderArgument::CanvasRefusal refusal)
{
    using enum Web::Embedder::ShaderArgument::CanvasRefusal;
    switch (refusal) {
    case NoElement:
        return "no element has that id"sv;
    case NotACanvas:
        return "the element isn't a <canvas>"sv;
    case NoContent:
        return "the canvas has no 2D context yet"sv;
    case NotOriginClean:
        return "the canvas isn't origin-clean (it holds pixels script can't read)"sv;
    case InsidePaintedElement:
        return "the canvas is inside the element the image paints, which would paint itself"sv;
    case None:
        break;
    }
    return "it wasn't resolved"sv;
}

Optional<Vector<u64>> ShaderImages::bind_children(Shader const& shader, ReadonlySpan<Web::Embedder::ShaderArgument> arguments)
{
    Vector<u64> canvas_ids;
    for (auto const& child : shader.children) {
        Web::Embedder::ShaderArgument const* argument = nullptr;
        for (auto const& each : arguments) {
            auto name = each.name.bytes_as_string_view();
            if (name.starts_with("--"sv) && name.substring_view(2) == child.view()) {
                argument = &each;
                break;
            }
        }
        if (!argument) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): uniform shader {} has no --{} element(#id) argument, so the image paints nothing", child, child));
            return {};
        }
        if (argument->type == Web::Embedder::ShaderArgument::Type::RefusedCanvas) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): {} element(#{}) is refused: {}, so the image paints nothing", argument->name, argument->element_id, describe(argument->canvas_refusal)));
            return {};
        }
        if (argument->type != Web::Embedder::ShaderArgument::Type::Canvas) {
            m_log(KR_LOG_WARN, ByteString::formatted("shader(): {} is {}, but uniform shader {} takes element(#id), so the image paints nothing", argument->name, describe(argument->type), child));
            return {};
        }
        canvas_ids.append(argument->canvas_id);
    }
    return canvas_ids;
}

Optional<Web::Embedder::ShaderContent> ShaderImages::content(StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments)
{
    if (!m_shaders.contains(source))
        m_shaders.set(ByteString { source }, compile(source));
    auto& shader = **m_shaders.get(source);
    if (!shader.effect)
        return {};
    if (!(raster_scale > 0) || isinf(raster_scale))
        raster_scale = 1;
    auto key = arguments_key(arguments);
    auto family = shader.families.get(key);
    if (!family.has_value()) {
        // The first time this source is asked for with these arguments: pack them, or log why not.
        Optional<u64> new_family;
        auto author_uniforms = pack_author_uniforms(shader, arguments);
        auto child_canvas_ids = author_uniforms.has_value() ? bind_children(shader, arguments) : Optional<Vector<u64>> {};
        if (author_uniforms.has_value() && child_canvas_ids.has_value()) {
            BuiltinUniforms builtins { shader.size_offset, shader.device_pixel_ratio_offset, shader.time_offset };
            OwnPtr<ChildCanvases> children;
            if (!shader.children.is_empty())
                children = make<ChildCanvases>(ChildCanvases { move(*child_canvas_ids), shader.children, false });
            // A shader with canvas inputs and no uTime changes only with its canvases.
            auto depends_on_time = shader.time_offset.has_value() || shader.children.is_empty() ? EmbedderContentRegistry::DependsOnTime::Yes : EmbedderContentRegistry::DependsOnTime::No;
            new_family = m_registry.add_family([this, effect = shader.effect, builtins, author_uniforms = shader.author_uniforms.is_empty() ? nullptr : move(*author_uniforms), children = move(children)](u32 scale_bits, SkCanvas& canvas, Gfx::FloatRect const& dst_rect, i64 time_ns) {
                paint_shader(*effect, builtins, author_uniforms.get(), children.ptr(), bit_cast<float>(scale_bits), time_ns, canvas, dst_rect);
            },
                depends_on_time);
        }
        shader.families.set(key, new_family);
        family = new_family;
    }
    if (!family->has_value())
        return {};
    // Live when it animates or samples canvases, whose pixels change with no paint from LibWeb.
    auto is_live = shader.time_offset.has_value() || !shader.children.is_empty();
    return Web::Embedder::ShaderContent { EmbedderContentRegistry::family_content_id(**family, bit_cast<u32>(raster_scale)), is_live };
}

void ShaderImages::paint_shader(SkRuntimeEffect const& effect, BuiltinUniforms builtins, SkData const* author_uniforms, ChildCanvases* children, float raster_scale, i64 time_ns, SkCanvas& canvas, Gfx::FloatRect const& dst_rect)
{
    // content_id() only makes ids for positive scales; any other key paints nothing.
    if (!(raster_scale > 0) || isinf(raster_scale))
        return;
    auto uniforms = author_uniforms ? SkData::MakeWithCopy(author_uniforms->data(), author_uniforms->size()) : SkData::MakeZeroInitialized(effect.uniformSize());
    auto* bytes = static_cast<u8*>(uniforms->writable_data());
    if (builtins.size_offset.has_value()) {
        float size[2] = { dst_rect.width() / raster_scale, dst_rect.height() / raster_scale };
        memcpy(bytes + *builtins.size_offset, size, sizeof(size));
    }
    if (builtins.device_pixel_ratio_offset.has_value())
        memcpy(bytes + *builtins.device_pixel_ratio_offset, &raster_scale, sizeof(raster_scale));
    if (builtins.time_offset.has_value()) {
        // Seconds since the document's timeline origin (the registry's time origin).
        auto time = static_cast<float>(static_cast<double>(time_ns) / 1'000'000'000.0);
        memcpy(bytes + *builtins.time_offset, &time, sizeof(time));
    }
    // The shader's coord is in CSS pixels from the rectangle's top-left.
    auto local_matrix = SkMatrix::Translate(dst_rect.x(), dst_rect.y());
    local_matrix.preScale(raster_scale, raster_scale);
    // Each child samples its canvas's presented pixels, a canvas pixel per CSS pixel of the parent's coord.
    Vector<SkRuntimeEffect::ChildPtr> child_shaders;
    if (children) {
        for (size_t index = 0; index < children->canvas_ids.size(); ++index) {
            auto canvas_id = children->canvas_ids[index];
            auto input = m_canvas_inputs ? m_canvas_inputs(canvas_id) : CanvasInput {};
            m_registry.note_sampled_canvas(canvas_id, input.content_generation);
            // The canvas was tainted after LibWeb bound it: refuse it as LibWeb would have.
            if (input.surface && !input.origin_clean) {
                if (!children->logged_refusal) {
                    children->logged_refusal = true;
                    m_log(KR_LOG_WARN, ByteString::formatted("shader(): the canvas uniform shader {} samples isn't origin-clean any more (it holds pixels script can't read), so the image paints nothing", children->names[index]));
                }
                return;
            }
            sk_sp<SkShader> child;
            if (input.surface) {
                if (auto image = input.surface->sk_image_snapshot<sk_sp<SkImage>>())
                    child = image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, SkSamplingOptions { SkFilterMode::kLinear });
            }
            // A canvas with no presented pixels is transparent.
            child_shaders.append(SkRuntimeEffect::ChildPtr { move(child) });
        }
    }
    auto sk_shader = effect.makeShader(move(uniforms), SkSpan { child_shaders.data(), child_shaders.size() }, &local_matrix);
    if (!sk_shader)
        return;
    SkPaint paint;
    paint.setShader(move(sk_shader));
    canvas.drawRect(SkRect::MakeXYWH(dst_rect.x(), dst_rect.y(), dst_rect.width(), dst_rect.height()), paint);
}

Gfx::Color solid_color_test_content_color(i64 time_ns)
{
    // Floor division, so a time before the clock's origin still steps once a second.
    auto seconds = time_ns >= 0 ? time_ns / 1'000'000'000 : (time_ns + 1) / 1'000'000'000 - 1;
    auto step = static_cast<u8>((seconds * 40) & 0xff);
    return Gfx::Color { step, static_cast<u8>(255 - step), 128 };
}

EmbedderContentPainter solid_color_test_content()
{
    return [](SkCanvas& canvas, Gfx::FloatRect const& dst_rect, i64 time_ns) {
        auto color = solid_color_test_content_color(time_ns);
        SkPaint paint;
        paint.setColor(SkColorSetARGB(color.alpha(), color.red(), color.green(), color.blue()));
        canvas.drawRect(SkRect::MakeXYWH(dst_rect.x(), dst_rect.y(), dst_rect.width(), dst_rect.height()), paint);
    };
}

}
