/*
 * The budget of Skia's GPU text blob cache (sktext::gpu::TextBlobRedrawCoordinator), which Skia
 * fixes at 4 MiB and exposes no setter for.
 *
 * Ganesh keeps one sktext::gpu::TextBlob per SkTextBlob it has drawn (its glyph sub runs and atlas
 * locations, ~1.1 KB of system malloc for a short label), keyed by the SkTextBlob's id, and drops it
 * when that SkTextBlob dies or the cache goes over budget. LibCompositing's display list resources
 * keep every SkTextBlob they make (up to 16 MiB of them, DisplayListResourceStorage::text_blob), so a
 * page that writes new text every frame (a counter, a clock) fills Skia's cache to its budget: ~6 MB
 * of system malloc that nothing redraws.
 */
#pragma once

#include <stddef.h>

class GrDirectContext;

namespace Karouka {

// Sets the budget of `context`'s text blob cache, in Skia's own accounting (TextBlob::size(), the
// arena each blob's sub runs live in). Before the context draws anything: it trims nothing.
void set_text_blob_cache_budget(GrDirectContext& context, size_t bytes);

// The bytes the text blob cache holds, in the same accounting.
size_t text_blob_cache_used_bytes(GrDirectContext& context);

}
