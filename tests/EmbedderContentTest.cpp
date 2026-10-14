// DrawEmbedderContent (patch 0101, src/engine/ShaderImage.h): a display list with the command, replayed by
// DisplayListPlayerSkia on the Metal and software backends, draws the embedder content's pixels at the frame's
// host time, directly and through a nested display list (which the player would otherwise cache as a raster after
// its second paint). shader() images (ShaderImages, LibWeb's Embedder/ShaderDecodedImageData.h): a compiled shader
// recorded at two raster scales paints with the uniforms it declares, 1000 raster scales keep the registry at one
// entry, a shader that declares uTime is live and draws its time since the registry's time origin at each replay,
// directly and as a repeated tile (which the player would otherwise cache), and a shader that doesn't compile or
// declares a uniform karouka doesn't supply logs once and records nothing. Built as karouka_embedder_content_test and
// run by tools/dev.sh check.

#include "ShaderImage.h"

#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <LibCompositing/DisplayList/CanvasSurfaceRegistry.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListPlayerSkia.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Embedder/ShaderDecodedImageData.h>
#include <core/SkCanvas.h>
#include <core/SkColor.h>

static int s_failures;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            warnln("{}:{}: CHECK failed: {}", __FILE__, __LINE__, #expr); \
            ++s_failures;                                                 \
        }                                                                 \
    } while (0)

static Gfx::IntSize const surface_size { 16, 16 };
static Gfx::IntRect const content_rect { 4, 4, 8, 8 };
static constexpr i64 nanoseconds_per_second = 1'000'000'000;

struct TestDisplayList {
    ByteBuffer bytes;
    Vector<Compositing::DisplayListCommandRun> runs;
};

// Where records inside a command (a DisplayListDataSpan's offset) start in its payload: after the command.
template<Compositing::DisplayListCommand Command>
static u32 inline_records_offset()
{
    return align_up_to(Compositing::display_list_object_bytes(Command {}).size(), Compositing::DisplayList::command_alignment);
}

// One record as the Rust builder writes it (Tests/LibCompositing/DisplayListTestHelpers.h), in one run, with
// inline_records (the records a span of the command names) at inline_records_offset.
template<Compositing::DisplayListCommand Command>
static void append_command(TestDisplayList& list, Command const& command, Gfx::IntRect bounding_rect, ReadonlyBytes inline_records = {})
{
    auto offset = list.bytes.size();
    auto payload = MUST(ByteBuffer::copy(Compositing::display_list_object_bytes(command)));
    if (!inline_records.is_empty()) {
        payload.resize(inline_records_offset<Command>(), ByteBuffer::ZeroFillNewElements::Yes);
        payload.append(inline_records);
    }
    auto record_size = sizeof(Compositing::DisplayListCommandHeader) + payload.size();
    auto payload_size = align_up_to(record_size, Compositing::DisplayList::command_alignment) - sizeof(Compositing::DisplayListCommandHeader);
    Compositing::DisplayListCommandHeader header {
        .command_type = Command::command_type,
        .has_bounding_rect = true,
        .inline_clip_count = 0,
        .has_inline_transform = false,
        .payload_size = static_cast<u32>(payload_size),
        .bounding_rect = bounding_rect,
    };
    list.bytes.append(Compositing::display_list_object_bytes(header));
    list.bytes.append(payload);
    list.bytes.resize(align_up_to(list.bytes.size(), Compositing::DisplayList::command_alignment), ByteBuffer::ZeroFillNewElements::Yes);
    if (list.runs.is_empty()) {
        Compositing::DisplayListCommandRun run {};
        run.offset = offset;
        list.runs.append(run);
    }
    list.runs.last().size += list.bytes.size() - offset;
    list.runs.last().ink_bounds.unite(bounding_rect);
}

static NonnullRefPtr<Compositing::DisplayList> finish(Compositing::AccumulatedVisualContextTree const& tree, TestDisplayList list)
{
    return Compositing::DisplayList::create_from_command_bytes(tree, move(list.bytes), move(list.runs));
}

static Gfx::Color pixel_at(Gfx::PaintingSurface& surface, Gfx::IntPoint point)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, surface.size()));
    surface.read_into_bitmap(*bitmap);
    return bitmap->get_pixel(point.x(), point.y());
}

static void test_backend(StringView backend, RefPtr<Gfx::SkiaBackendContext> skia)
{
    Karouka::EmbedderContentRegistry registry;
    auto content_id = registry.add(Karouka::solid_color_test_content());
    auto tree = Compositing::VisualContextTreeTestBuilder().finish();
    Compositing::DisplayListResourceStorage resource_storage;

    TestDisplayList direct;
    append_command(direct, Compositing::DrawEmbedderContent { content_rect.to_type<float>(), content_id, true }, content_rect);
    auto direct_list = finish(tree, move(direct));
    CHECK(resource_storage.draws_live_embedder_content(*direct_list));

    TestDisplayList nested_content;
    append_command(nested_content, Compositing::DrawEmbedderContent { Gfx::FloatRect { 0, 0, 8, 8 }, content_id, true }, { 0, 0, 8, 8 });
    auto nested_id = resource_storage.add_display_list(finish(tree, move(nested_content)), tree);
    TestDisplayList outer;
    append_command(outer, Compositing::PaintNestedDisplayList { nested_id, content_rect.to_type<float>(), { 8, 8 } }, content_rect);
    auto nested_list = finish(tree, move(outer));
    CHECK(resource_storage.draws_live_embedder_content(*nested_list));

    TestDisplayList still;
    append_command(still, Compositing::DrawEmbedderContent { content_rect.to_type<float>(), content_id, false }, content_rect);
    CHECK(!resource_storage.draws_live_embedder_content(*finish(tree, move(still))));

    auto surface = Gfx::PaintingSurface::create_with_size(surface_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia);
    Compositing::DisplayListPlayerSkia player { skia };
    Compositing::ScrollStateSnapshot scroll_state;

    auto replay = [&](Compositing::DisplayList const& list, i64 time_ns, bool with_resolver) {
        surface->canvas().clear(SK_ColorWHITE);
        player.set_caret_time_ns(time_ns);
        player.set_embedder_content_resolver(with_resolver ? &registry.resolver() : nullptr);
        player.execute(list, tree, resource_storage, scroll_state, surface, nullptr, nullptr);
        player.flush(*surface);
    };
    auto expect = [&](StringView what, i64 time_ns, Gfx::Color expected) {
        auto inside = pixel_at(*surface, content_rect.center());
        auto outside = pixel_at(*surface, { 1, 1 });
        if (inside != expected || outside != Gfx::Color::White) {
            warnln("{}: {} at {} ns: inside {} (expected {}), outside {}", backend, what, time_ns, inside, expected, outside);
            ++s_failures;
        }
    };

    // Two host times a second apart draw the two colors of the test content.
    for (i64 time_ns : { nanoseconds_per_second / 2, nanoseconds_per_second * 3 / 2 }) {
        replay(*direct_list, time_ns, true);
        expect("direct"sv, time_ns, Karouka::solid_color_test_content_color(time_ns));
    }
    CHECK(Karouka::solid_color_test_content_color(nanoseconds_per_second / 2) != Karouka::solid_color_test_content_color(nanoseconds_per_second * 3 / 2));

    // A nested list is cached as a raster from its second paint on, unless it draws embedder content.
    for (i64 time_ns : { nanoseconds_per_second / 2, nanoseconds_per_second * 3 / 2, nanoseconds_per_second * 5 / 2 }) {
        replay(*nested_list, time_ns, true);
        expect("nested"sv, time_ns, Karouka::solid_color_test_content_color(time_ns));
    }

    // No resolver, or an id with no painter, paints nothing.
    replay(*direct_list, nanoseconds_per_second / 2, false);
    expect("no resolver"sv, nanoseconds_per_second / 2, Gfx::Color::White);
    registry.remove(content_id);
    replay(*direct_list, nanoseconds_per_second / 2, true);
    expect("removed content"sv, nanoseconds_per_second / 2, Gfx::Color::White);
}

// Left half red and right half blue across uSize (CSS pixels, as coord is); green is uDevicePixelRatio / 4.
static constexpr auto split_shader = R"(
uniform float2 uSize;
uniform float uDevicePixelRatio;
half4 main(float2 coord) {
    half green = half(uDevicePixelRatio / 4.0);
    return coord.x < uSize.x * 0.5 ? half4(1.0, green, 0.0, 1.0) : half4(0.0, green, 1.0, 1.0);
}
)"sv;
static constexpr auto broken_shader = "half4 main(float2 coord) { return no_such_color; }"sv;
// The author's uniforms: tint (a <color>) scaled by level (a <number>, <percentage> or <length>).
static constexpr auto tint_shader = "uniform half4 tint; uniform float level; half4 main(float2 coord) { return half4(tint.rgb * half(level), 1.0); }"sv;
static constexpr auto time_shader = "uniform float uTime; half4 main(float2 coord) { return half4(half(fract(uTime)), 0.0, 0.0, 1.0); }"sv;
static constexpr auto wrong_time_shader = "uniform float2 uTime; half4 main(float2 coord) { return half4(half(uTime.x), 0.0, 0.0, 1.0); }"sv;

static bool near(Gfx::Color actual, Gfx::Color expected)
{
    auto close = [](u8 a, u8 b) { return (a > b ? a - b : b - a) <= 3; };
    return close(actual.red(), expected.red()) && close(actual.green(), expected.green()) && close(actual.blue(), expected.blue()) && close(actual.alpha(), expected.alpha());
}

static void test_shader_images(StringView backend, RefPtr<Gfx::SkiaBackendContext> skia)
{
    Karouka::EmbedderContentRegistry registry;
    Vector<ByteString> log;
    Karouka::ShaderImages shader_images { registry, [&](kr_log_level, StringView message) { log.append(message); } };
    Web::Embedder::set_shader_content_hook([&](StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments) { return shader_images.content(source, raster_scale, arguments); });

    auto tree = Compositing::VisualContextTreeTestBuilder().finish();
    Compositing::DisplayListResourceStorage resource_storage;
    Gfx::IntSize const big_surface_size { 32, 32 };
    auto surface = Gfx::PaintingSurface::create_with_size(big_surface_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia);
    Compositing::DisplayListPlayerSkia player { skia };
    player.set_embedder_content_resolver(&registry.resolver());
    Compositing::ScrollStateSnapshot scroll_state;

    // An 8x8 CSS pixel image at 4,4 on the surface, recorded at each scale and drawn 1:1 in device pixels.
    for (float raster_scale : { 1.0f, 2.0f }) {
        auto recording = Web::Embedder::record_shader_display_list(split_shader, { 8, 8 }, raster_scale);
        if (!recording.has_value()) {
            warnln("{}: split shader at scale {} recorded nothing; log: {}", backend, raster_scale, log);
            ++s_failures;
            continue;
        }
        CHECK(!resource_storage.draws_live_embedder_content(*recording->display_list));
        int list_side = static_cast<int>(8 * raster_scale);
        auto nested_id = resource_storage.add_display_list(recording.release_value());
        Gfx::IntRect dst { 4, 4, list_side, list_side };
        TestDisplayList outer;
        append_command(outer, Compositing::PaintNestedDisplayList { nested_id, dst.to_type<float>(), { list_side, list_side } }, dst);
        auto list = finish(tree, move(outer));
        // Twice: the second paint is the nested list's cached raster.
        for (int paint = 0; paint < 2; ++paint) {
            surface->canvas().clear(SK_ColorWHITE);
            player.execute(*list, tree, resource_storage, scroll_state, surface, nullptr, nullptr);
            player.flush(*surface);
            auto green = static_cast<u8>(lroundf(raster_scale / 4 * 255));
            struct Probe {
                Gfx::IntPoint point;
                Gfx::Color expected;
            };
            Probe const probes[] = {
                { { 4, 4 }, Gfx::Color { 255, green, 0 } },
                { { 4 + list_side / 2 - 1, 4 + list_side - 1 }, Gfx::Color { 255, green, 0 } },
                { { 4 + list_side / 2, 4 }, Gfx::Color { 0, green, 255 } },
                { { 4 + list_side - 1, 4 + list_side - 1 }, Gfx::Color { 0, green, 255 } },
                { { 4 + list_side, 4 }, Gfx::Color::White },
                { { 2, 2 }, Gfx::Color::White },
            };
            for (auto const& probe : probes) {
                auto actual = pixel_at(*surface, probe.point);
                if (!near(actual, probe.expected)) {
                    warnln("{}: split shader at scale {}, paint {}: {} at {} (expected {})", backend, raster_scale, paint, actual, probe.point, probe.expected);
                    ++s_failures;
                }
            }
        }
    }
    // One compile, a content id per scale, and the same id when a scale is asked for again.
    CHECK(shader_images.shader_count() == 1);
    CHECK(shader_images.content_id(split_shader, 1) != shader_images.content_id(split_shader, 2));
    CHECK(shader_images.content_id(split_shader, 2) == shader_images.content_id(split_shader, 2));
    CHECK(log.is_empty());

    // A scale that keeps changing (an animated transform) adds nothing to the registry: 1000 scales, each
    // recorded through LibWeb's hook, leave it at one entry for the one shader, and each id paints at its scale.
    CHECK(registry.size() == 1);
    for (int step = 0; step < 1000; ++step) {
        float raster_scale = 0.25f + static_cast<float>(step) * 0.0137f;
        auto content_id = shader_images.content_id(split_shader, raster_scale);
        CHECK(content_id.has_value() && Karouka::ShaderImages::raster_scale_of(*content_id) == raster_scale);
        CHECK(Web::Embedder::record_shader_display_list(split_shader, { 8, 8 }, raster_scale).has_value());
    }
    if (registry.size() != 1) {
        warnln("{}: 1000 raster scales left {} registry entries (expected 1)", backend, registry.size());
        ++s_failures;
    }

    // A shader that declares uTime is live and paints its time in seconds since the registry's time origin (the
    // document's timeline origin) at each replay: as a nested list painted directly (never kept as a raster), and as a
    // repeated tile (never taken from the tile cache).
    {
        auto recording = Web::Embedder::record_shader_display_list(time_shader, { 8, 8 }, 1);
        CHECK(recording.has_value());
        if (recording.has_value()) {
            CHECK(resource_storage.draws_live_embedder_content(*recording->display_list));
            CHECK(shader_images.content(time_shader, 1).has_value() && shader_images.content(time_shader, 1)->is_live);
            CHECK(!shader_images.content(split_shader, 1)->is_live);
            auto nested_id = resource_storage.add_display_list(recording.release_value());
            Gfx::IntRect dst { 4, 4, 8, 8 };
            TestDisplayList direct;
            append_command(direct, Compositing::PaintNestedDisplayList { nested_id, dst.to_type<float>(), { 8, 8 } }, dst);
            auto direct_list = finish(tree, move(direct));

            TestDisplayList tile_records;
            append_command(tile_records, Compositing::PaintNestedDisplayList { nested_id, Gfx::FloatRect { 0, 0, 8, 8 }, { 8, 8 } }, { 0, 0, 8, 8 });
            Gfx::IntRect tiled_rect { 0, 0, 32, 32 };
            Compositing::DrawRepeatedTile repeated_tile {
                .dst_rect = dst.to_type<float>(),
                .clip_rect = tiled_rect,
                .tile_size = { 8, 8 },
                .tile_step = { 8, 8 },
                .tile = { inline_records_offset<Compositing::DrawRepeatedTile>(), static_cast<u32>(tile_records.bytes.size()) },
                .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
                .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::Normal,
                .repeat = { true, true },
            };
            TestDisplayList tiled;
            append_command(tiled, repeated_tile, tiled_rect, tile_records.bytes);
            auto tiled_list = finish(tree, move(tiled));

            constexpr i64 time_origin_ns = 5 * nanoseconds_per_second;
            registry.set_time_origin_ns(time_origin_ns);
            for (auto const* list : { direct_list.ptr(), tiled_list.ptr() }) {
                for (i64 time_ns : { nanoseconds_per_second / 4, nanoseconds_per_second * 3 / 4, nanoseconds_per_second * 3 / 2, nanoseconds_per_second / 4 }) {
                    surface->canvas().clear(SK_ColorWHITE);
                    player.set_caret_time_ns(time_origin_ns + time_ns);
                    player.execute(*list, tree, resource_storage, scroll_state, surface, nullptr, nullptr);
                    player.flush(*surface);
                    auto seconds = static_cast<double>(time_ns) / nanoseconds_per_second;
                    Gfx::Color expected { static_cast<u8>(lround((seconds - floor(seconds)) * 255)), 0, 0 };
                    // The tile's second copy, right of and below the first, as well as the first.
                    for (Gfx::IntPoint point : { Gfx::IntPoint { 8, 8 }, Gfx::IntPoint { 16, 16 } }) {
                        if (list == direct_list.ptr() && point != Gfx::IntPoint { 8, 8 })
                            continue;
                        auto actual = pixel_at(*surface, point);
                        if (!near(actual, expected)) {
                            warnln("{}: uTime shader ({}) at {} s: {} at {} (expected {})", backend, list == direct_list.ptr() ? "nested"sv : "repeated tile"sv, seconds, actual, point, expected);
                            ++s_failures;
                        }
                    }
                }
            }
            registry.set_time_origin_ns(0);
        }
        CHECK(log.is_empty());
    }

    // A shader that doesn't compile, or that declares a uniform karouka doesn't supply, records nothing (so it paints
    // nothing) and logs one line however often it is asked for.
    for (auto source : { broken_shader, wrong_time_shader }) {
        log.clear();
        for (float raster_scale : { 1.0f, 2.0f, 1.0f })
            CHECK(!Web::Embedder::record_shader_display_list(source, { 8, 8 }, raster_scale).has_value());
        CHECK(!shader_images.content_id(source, 1).has_value());
        if (log.size() != 1) {
            warnln("{}: {} logged {} lines (expected 1): {}", backend, source, log.size(), log);
            ++s_failures;
        }
    }
    CHECK(log.size() == 1 && log[0].contains("uniform uTime must be a float"sv));
    log.clear();
    (void)Web::Embedder::record_shader_display_list(broken_shader, { 8, 8 }, 1);
    CHECK(log.is_empty());

    // Uniform arguments: each sets the uniform it names, packed by the uniform's type; one content id per set of
    // values. A mismatched or missing argument records nothing and logs one line however often it is asked for; an
    // argument no uniform takes is ignored with one line.
    using Argument = Web::Embedder::ShaderArgument;
    auto argument = [](StringView name, Argument::Type type, Array<float, 4> values) {
        return Argument { MUST(String::from_utf8(name)), type, values, 0, Argument::CanvasRefusal::None, {} };
    };
    auto paint_center = [&](Web::Embedder::ShaderArgument const* arguments, size_t count) -> Optional<Gfx::Color> {
        auto recording = Web::Embedder::record_shader_display_list(tint_shader, { 8, 8 }, 1, { arguments, count });
        if (!recording.has_value())
            return {};
        auto nested_id = resource_storage.add_display_list(recording.release_value());
        Gfx::IntRect dst { 4, 4, 8, 8 };
        TestDisplayList outer;
        append_command(outer, Compositing::PaintNestedDisplayList { nested_id, dst.to_type<float>(), { 8, 8 } }, dst);
        auto list = finish(tree, move(outer));
        surface->canvas().clear(SK_ColorWHITE);
        player.execute(*list, tree, resource_storage, scroll_state, surface, nullptr, nullptr);
        player.flush(*surface);
        return pixel_at(*surface, { 8, 8 });
    };
    struct UniformCase {
        StringView what;
        Vector<Argument> arguments;
        Optional<Gfx::Color> expected;
        StringView logged;
    };
    auto orange = Array<float, 4> { 1, 0.5f, 0, 1 };
    UniformCase const uniform_cases[] = {
        { "color and number"sv, { argument("--tint"sv, Argument::Type::Color, orange), argument("--level"sv, Argument::Type::Number, { 1 }) }, Gfx::Color { 255, 128, 0 }, {} },
        { "percentage"sv, { argument("--level"sv, Argument::Type::Percentage, { 0.5f }), argument("--tint"sv, Argument::Type::Color, orange) }, Gfx::Color { 128, 64, 0 }, {} },
        { "number for a half4"sv, { argument("--tint"sv, Argument::Type::Number, { 7 }), argument("--level"sv, Argument::Type::Number, { 1 }) }, {}, "--tint is a <number>, but uniform tint is a float4"sv },
        { "missing argument"sv, { argument("--tint"sv, Argument::Type::Color, orange) }, {}, "uniform level has no --level argument"sv },
        { "unsupported value"sv, { argument("--tint"sv, Argument::Type::Color, orange), argument("--level"sv, Argument::Type::Unsupported, {}) }, {}, "uniform level is a float"sv },
        { "extra argument"sv, { argument("--tint"sv, Argument::Type::Color, orange), argument("--level"sv, Argument::Type::Length, { 1 }), argument("--speed"sv, Argument::Type::Number, { 2 }) }, Gfx::Color { 255, 128, 0 }, "--speed sets no uniform"sv },
    };
    for (auto const& uniform_case : uniform_cases) {
        log.clear();
        for (int ask = 0; ask < 3; ++ask) {
            auto actual = paint_center(uniform_case.arguments.data(), uniform_case.arguments.size());
            if (actual.has_value() != uniform_case.expected.has_value() || (actual.has_value() && !near(*actual, *uniform_case.expected))) {
                warnln("{}: uniforms ({}): painted {} (expected {})", backend, uniform_case.what, actual, uniform_case.expected);
                ++s_failures;
            }
        }
        auto expected_log_lines = uniform_case.logged.is_empty() ? 0u : 1u;
        if (log.size() != expected_log_lines || (expected_log_lines && !log[0].contains(uniform_case.logged))) {
            warnln("{}: uniforms ({}): logged {} (expected one line with '{}')", backend, uniform_case.what, log, uniform_case.logged);
            ++s_failures;
        }
    }
    auto first = shader_images.content_id(tint_shader, 1, uniform_cases[0].arguments);
    CHECK(first.has_value() && first == shader_images.content_id(tint_shader, 1, uniform_cases[0].arguments));
    CHECK(first != shader_images.content_id(tint_shader, 1, uniform_cases[1].arguments));

    // Without a hook, nothing is recorded.
    Web::Embedder::set_shader_content_hook(nullptr);
    CHECK(!Web::Embedder::record_shader_display_list(split_shader, { 8, 8 }, 1).has_value());
}

// shader() canvas inputs: `uniform shader tex` samples the canvas `--tex element(#id)` binds, one canvas pixel per CSS
// pixel; a refused, missing or mismatched input paints nothing with one line, as a canvas tainted after binding does
// at replay. Such a shader is live but doesn't depend on time: the registry's replay reports the canvas it sampled.
static void test_shader_canvas_inputs(StringView backend, RefPtr<Gfx::SkiaBackendContext> skia)
{
    static constexpr auto sampling_shader = "uniform shader tex; half4 main(float2 coord) { return tex.eval(coord); }"sv;
    Karouka::EmbedderContentRegistry registry;
    Vector<ByteString> log;
    Compositing::CanvasSurfaceRegistry canvases;
    bool origin_clean = true;
    auto canvas_surface = Gfx::PaintingSurface::create_with_size({ 16, 16 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia);
    canvas_surface->canvas().clear(SkColorSetARGB(255, 0, 0, 255));
    canvas_surface->flush();
    auto canvas_id = canvases.create_canvas_surface(canvas_surface);
    Karouka::ShaderImages shader_images {
        registry, [&](kr_log_level, StringView message) { log.append(message); },
        [&](u64 id) -> Karouka::ShaderImages::CanvasInput {
            return { canvases.canvas_surface(Compositing::CanvasId { id }), canvases.canvas_content_generation(Compositing::CanvasId { id }), origin_clean };
        }
    };
    Web::Embedder::set_shader_content_hook([&](StringView source, float raster_scale, ReadonlySpan<Web::Embedder::ShaderArgument> arguments) { return shader_images.content(source, raster_scale, arguments); });

    auto tree = Compositing::VisualContextTreeTestBuilder().finish();
    Compositing::DisplayListResourceStorage resource_storage;
    auto surface = Gfx::PaintingSurface::create_with_size({ 32, 32 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia);
    Compositing::DisplayListPlayerSkia player { skia };
    player.set_embedder_content_resolver(&registry.resolver());
    Compositing::ScrollStateSnapshot scroll_state;

    using Argument = Web::Embedder::ShaderArgument;
    auto canvas_argument = [&](StringView name, Argument::Type type, Argument::CanvasRefusal refusal = Argument::CanvasRefusal::None) {
        return Argument { MUST(String::from_utf8(name)), type, {}, type == Argument::Type::Canvas ? canvas_id.value() : 0, refusal, "source"_string };
    };
    // Paints an 8x8 image at 4,4 and answers the pixel at 8,8 (the image's 4,4), or nothing when nothing was recorded.
    auto paint = [&](Vector<Argument> const& arguments) -> Optional<Gfx::Color> {
        auto recording = Web::Embedder::record_shader_display_list(sampling_shader, { 8, 8 }, 1, arguments);
        if (!recording.has_value())
            return {};
        CHECK(resource_storage.draws_live_embedder_content(*recording->display_list));
        auto nested_id = resource_storage.add_display_list(recording.release_value());
        Gfx::IntRect dst { 4, 4, 8, 8 };
        TestDisplayList outer;
        append_command(outer, Compositing::PaintNestedDisplayList { nested_id, dst.to_type<float>(), { 8, 8 } }, dst);
        auto list = finish(tree, move(outer));
        surface->canvas().clear(SK_ColorWHITE);
        registry.begin_replay();
        player.execute(*list, tree, resource_storage, scroll_state, surface, nullptr, nullptr);
        player.flush(*surface);
        return pixel_at(*surface, { 8, 8 });
    };

    auto blue = paint({ canvas_argument("--tex"sv, Argument::Type::Canvas) });
    if (!blue.has_value() || !near(*blue, Gfx::Color { 0, 0, 255 })) {
        warnln("{}: canvas input painted {} (expected blue); log: {}", backend, blue, log);
        ++s_failures;
    }
    auto replay = registry.take_replay();
    CHECK(!replay.drew_content_depending_on_time);
    CHECK(replay.sampled_canvases.size() == 1 && replay.sampled_canvases[0].canvas_id == canvas_id.value() && replay.sampled_canvases[0].content_generation == canvases.canvas_content_generation(canvas_id));
    CHECK(log.is_empty());

    // New pixels presented under the same id are what the next replay samples.
    canvas_surface->canvas().clear(SkColorSetARGB(255, 0, 255, 0));
    canvas_surface->flush();
    canvases.set_canvas_surface(canvas_id, canvas_surface);
    auto green = paint({ canvas_argument("--tex"sv, Argument::Type::Canvas) });
    CHECK(green.has_value() && near(*green, Gfx::Color { 0, 255, 0 }));

    struct RefusedCase {
        StringView what;
        Vector<Argument> arguments;
        StringView logged;
    };
    RefusedCase const refused_cases[] = {
        { "tainted"sv, { canvas_argument("--tex"sv, Argument::Type::RefusedCanvas, Argument::CanvasRefusal::NotOriginClean) }, "--tex element(#source) is refused: the canvas isn't origin-clean"sv },
        { "inside the painted element"sv, { canvas_argument("--tex"sv, Argument::Type::RefusedCanvas, Argument::CanvasRefusal::InsidePaintedElement) }, "inside the element the image paints"sv },
        { "missing"sv, {}, "uniform shader tex has no --tex element(#id) argument"sv },
        { "a number"sv, { Argument { "--tex"_string, Argument::Type::Number, { 1 }, 0, Argument::CanvasRefusal::None, {} } }, "--tex is a <number>, but uniform shader tex takes element(#id)"sv },
    };
    for (auto const& refused_case : refused_cases) {
        log.clear();
        for (int ask = 0; ask < 3; ++ask) {
            if (auto painted = paint(refused_case.arguments); painted.has_value()) {
                warnln("{}: canvas input ({}) painted {} (expected nothing)", backend, refused_case.what, painted);
                ++s_failures;
            }
        }
        if (log.size() != 1 || !log[0].contains(refused_case.logged)) {
            warnln("{}: canvas input ({}): logged {} (expected one line with '{}')", backend, refused_case.what, log, refused_case.logged);
            ++s_failures;
        }
    }

    // A canvas tainted after it was bound paints nothing at replay, with one line however often it is drawn.
    log.clear();
    origin_clean = false;
    for (int frame = 0; frame < 3; ++frame) {
        auto painted = paint({ canvas_argument("--tex"sv, Argument::Type::Canvas) });
        CHECK(painted.has_value() && near(*painted, Gfx::Color { 255, 255, 255 }));
    }
    if (log.size() != 1 || !log[0].contains("isn't origin-clean any more"sv)) {
        warnln("{}: canvas tainted after binding: logged {} (expected one line)", backend, log);
        ++s_failures;
    }

    Web::Embedder::set_shader_content_hook(nullptr);
}

int main()
{
    test_backend("software"sv, nullptr);
    test_shader_images("software"sv, nullptr);
    test_shader_canvas_inputs("software"sv, nullptr);

    auto metal = Gfx::get_metal_context();
    auto skia = metal ? Gfx::SkiaBackendContext::create_metal_context(*metal) : nullptr;
    if (!skia) {
        warnln("no Metal device: the Metal backend can't be tested");
        ++s_failures;
    } else {
        test_backend("metal"sv, skia);
        test_shader_images("metal"sv, skia);
        test_shader_canvas_inputs("metal"sv, skia);
    }

    if (s_failures) {
        warnln("karouka_embedder_content_test: {} failure(s)", s_failures);
        return 1;
    }
    outln("karouka_embedder_content_test: ok");
    return 0;
}
