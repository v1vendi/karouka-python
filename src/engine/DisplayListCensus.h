/*
 * KR_DEBUG's display-list census: what a frame's display list asks of
 * a player, counted by a DisplayListPlayer that draws nothing. It is the coverage measurement behind
 * the command-list decision, and shows that a non-Skia player subclasses LibCompositing's exported
 * DisplayListPlayer from karouka's own code, with no Ladybird patch.
 */
#pragma once

#include <AK/ByteString.h>
#include <AK/RefPtr.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>

namespace Karouka {

// KR_DEBUG_CENSUS is set (with KR_DEBUG): Engine.cpp logs a census line before each frame it rasterizes.
bool census_enabled();

// One line: each command type with its count, then the replay calls (clips, transforms, layers,
// masks) and the features inside them a command-list player would have to translate. The surface is
// only for DisplayListPlayer::execute, which requires one; nothing is drawn into it.
ByteString describe_display_list_census(
    Compositing::DisplayList const&,
    Compositing::AccumulatedVisualContextTree const&,
    Compositing::DisplayListResourceStorage const&,
    Compositing::ScrollStateSnapshot const&,
    RefPtr<Gfx::PaintingSurface>);

}
