/*
 * KR_DEBUG's display-list census.
 */
#include "DisplayListCensus.h"

#include <AK/StringBuilder.h>
#include <stdlib.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/PaintingSurface.h>

namespace Karouka {

namespace {

enum class Command : u8 {
#define CENSUS_COMMAND_ENUM(command, player_method) command,
    ENUMERATE_DISPLAY_LIST_COMMANDS(CENSUS_COMMAND_ENUM)
#undef CENSUS_COMMAND_ENUM
        Count,
};

constexpr char const* command_names[] = {
#define CENSUS_COMMAND_NAME(command, player_method) #command,
    ENUMERATE_DISPLAY_LIST_COMMANDS(CENSUS_COMMAND_NAME)
#undef CENSUS_COMMAND_NAME
};

// The features a command-list player has to translate beyond the command itself.
struct Features {
    size_t glyphs { 0 };
    size_t vertical_glyph_runs { 0 };
    size_t gradient_paint_styles { 0 }; // FillPath/StrokePath painted with a gradient
    size_t pattern_paint_styles { 0 };
    size_t dashed_strokes { 0 };
    size_t non_normal_blend_draws { 0 };
    size_t isolated_group_filters { 0 };
    size_t isolated_group_masks { 0 };
    size_t nested_display_lists_missing { 0 };

    size_t clips { 0 };
    size_t rounded_clips { 0 };
    size_t difference_clips { 0 };
    size_t clip_paths { 0 };
    size_t plane_clips { 0 };
    size_t transforms { 0 };
    size_t matrices { 0 };
    size_t projective_matrices { 0 };
    size_t layers { 0 };
    size_t layer_opacity { 0 };
    size_t layer_blend { 0 };
    size_t layer_filter { 0 };
    size_t layer_backdrop_filter { 0 };
    size_t masks { 0 };
    size_t luminance_masks { 0 };
};

class CensusPlayer final : public Compositing::DisplayListPlayer {
public:
    Array<size_t, to_underlying(Command::Count)> counts {};
    Features features;

    void flush(Gfx::PaintingSurface&) override { }

private:
    template<typename T>
    void count(Command command, T const&) { ++counts[to_underlying(command)]; }

    void blend(Gfx::CompositingAndBlendingOperator op)
    {
        if (op != Gfx::CompositingAndBlendingOperator::Normal)
            ++features.non_normal_blend_draws;
    }

    void paint_style(Compositing::DisplayListPaintStyle const& style)
    {
        switch (style.paint_style_type) {
        case Compositing::DisplayListPaintStyleType::None:
            break;
        case Compositing::DisplayListPaintStyleType::Pattern:
            ++features.pattern_paint_styles;
            break;
        default:
            ++features.gradient_paint_styles;
            break;
        }
    }

#define CENSUS_PLAY_COMMAND(command, player_method) \
    void play_command(Compositing::command const& c) override { count(Command::command, c); on(c); }
    ENUMERATE_DISPLAY_LIST_COMMANDS(CENSUS_PLAY_COMMAND)
#undef CENSUS_PLAY_COMMAND

    template<typename T>
    void on(T const&) { }
    void on(Compositing::DrawGlyphRun const& c)
    {
        features.glyphs += inline_objects<Compositing::DisplayListGlyph>(c.glyphs).size();
        if (c.orientation == Gfx::Orientation::Vertical)
            ++features.vertical_glyph_runs;
    }
    void on(Compositing::FillRect const& c) { blend(c.compositing_and_blending_operator); }
    void on(Compositing::PaintLinearGradient const& c) { blend(c.compositing_and_blending_operator); }
    void on(Compositing::PaintRadialGradient const& c) { blend(c.compositing_and_blending_operator); }
    void on(Compositing::PaintConicGradient const& c) { blend(c.compositing_and_blending_operator); }
    void on(Compositing::FillPath const& c)
    {
        paint_style(c.paint_style);
        blend(c.compositing_and_blending_operator);
    }
    void on(Compositing::StrokePath const& c)
    {
        paint_style(c.paint_style);
        if (c.dash_array.size)
            ++features.dashed_strokes;
    }
    void on(Compositing::DrawIsolatedGroup const& c)
    {
        blend(c.compositing_and_blending_operator);
        if (c.filter.size)
            ++features.isolated_group_filters;
        execute_command_bytes(inline_data(c.content), active_scroll_state());
        if (c.mask.size) {
            ++features.isolated_group_masks;
            execute_command_bytes(inline_data(c.mask), active_scroll_state());
        }
    }
    void on(Compositing::DeclareMaskContent const& c) { declare_mask_content(c.effect, inline_data(c.content)); }
    void on(Compositing::PaintNestedDisplayList const& c)
    {
        if (!resource_storage().has_display_list(c.display_list_id)) {
            ++features.nested_display_lists_missing;
            return;
        }
        auto const& nested = resource_storage().display_list_resource(c.display_list_id);
        Compositing::ScrollStateSnapshot scroll_state;
        execute_nested_display_list(*nested.display_list, nested.visual_context_tree, scroll_state);
    }

    void set_matrix(Gfx::FloatMatrix4x4 const& matrix) override
    {
        ++features.matrices;
        if (matrix[3, 0] != 0 || matrix[3, 1] != 0 || matrix[3, 3] != 1)
            ++features.projective_matrices;
        m_matrix = matrix;
    }
    Gfx::FloatMatrix4x4 canvas_matrix() const override { return m_matrix; }
    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override { return false; }

    void push_clip(Compositing::ReplayClip const& clip) override
    {
        save();
        ++features.clips;
        if (clip.corner_radii.has_any_radius())
            ++features.rounded_clips;
        if (clip.mode == Compositing::ClipMode::Difference)
            ++features.difference_clips;
    }
    void push_clip_path(Gfx::Path const&, Gfx::WindingRule) override
    {
        save();
        ++features.clip_paths;
    }
    void push_transform(Gfx::AffineTransform const&) override
    {
        save();
        ++features.transforms;
    }
    void push_layer(Compositing::ReplayLayer const& layer) override
    {
        save();
        ++features.layers;
        if (layer.opacity < 1.0f)
            ++features.layer_opacity;
        if (layer.blend_mode != Gfx::CompositingAndBlendingOperator::Normal)
            ++features.layer_blend;
        if (layer.filter_bytes_size)
            ++features.layer_filter;
        if (layer.backdrop_filter_bytes_size)
            ++features.layer_backdrop_filter;
    }
    void push_mask(Compositing::ReplayMask const& mask) override
    {
        save();
        ++features.masks;
        if (mask.kind == Gfx::MaskKind::Luminance)
            ++features.luminance_masks;
    }
    void pop_mask(Compositing::ReplayMask const&, Compositing::EffectNodeIndex effect) override
    {
        if (auto content = declared_mask_content(effect); content.has_value())
            execute_command_bytes(*content, active_scroll_state());
        pop();
    }
    void pop() override
    {
        if (!m_saved.is_empty())
            m_matrix = m_saved.take_last();
    }
    void push_device_space_plane_clip(Gfx::Path const&) override
    {
        save();
        ++features.plane_clips;
    }

    void save() { m_saved.append(m_matrix); }

    Gfx::FloatMatrix4x4 m_matrix { Gfx::FloatMatrix4x4::identity() };
    Vector<Gfx::FloatMatrix4x4, 16> m_saved;
};

}

bool census_enabled()
{
    static bool const enabled = getenv("KR_DEBUG_CENSUS") != nullptr;
    return enabled;
}

ByteString describe_display_list_census(
    Compositing::DisplayList const& display_list,
    Compositing::AccumulatedVisualContextTree const& visual_context_tree,
    Compositing::DisplayListResourceStorage const& resource_storage,
    Compositing::ScrollStateSnapshot const& scroll_state,
    RefPtr<Gfx::PaintingSurface> surface)
{
    CensusPlayer player;
    player.execute(display_list, visual_context_tree, resource_storage, scroll_state, move(surface));

    StringBuilder builder;
    builder.append("census commands:"sv);
    for (size_t i = 0; i < player.counts.size(); ++i) {
        if (player.counts[i])
            builder.appendff(" {}={}", command_names[i], player.counts[i]);
    }
    auto const& f = player.features;
    builder.appendff(" | glyphs={} vertical_runs={} gradient_paint={} pattern_paint={} dashed={} blend_draws={} group_filters={} group_masks={} nested_missing={}",
        f.glyphs, f.vertical_glyph_runs, f.gradient_paint_styles, f.pattern_paint_styles, f.dashed_strokes, f.non_normal_blend_draws, f.isolated_group_filters, f.isolated_group_masks, f.nested_display_lists_missing);
    builder.appendff(" | clips={} rounded={} difference={} clip_paths={} plane_clips={} transforms={} matrices={} projective={} layers={} opacity={} blend={} filter={} backdrop={} masks={} luminance={}",
        f.clips, f.rounded_clips, f.difference_clips, f.clip_paths, f.plane_clips, f.transforms, f.matrices, f.projective_matrices, f.layers, f.layer_opacity, f.layer_blend, f.layer_filter, f.layer_backdrop_filter, f.masks, f.luminance_masks);
    return builder.to_byte_string();
}

}
