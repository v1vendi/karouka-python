/*
 * Warming a web font's glyphs as soon as its typeface exists, and the rest between frames.
 *
 * A page's first frame with @font-face text spends most of its flush in CoreText, rasterizing that
 * text's glyphs one by one (SkScalerContext_Mac::generateImage) as Ganesh fills its atlas:
 * showcase_page's second frame, the first with its "Demo" font, flushed in 4-5 ms. The glyphs and the
 * font's typeface only exist after that frame's own font load, and warming them at render time made
 * the frame slower (CoreText doesn't scale across threads, and the replay and flush waited
 * on the strikes' locks a worker held).
 *
 * So the glyphs a view's first recorded_frames frames drew in a web font are kept in a list next to
 * the shader cache (kr_system_desc.shader_cache_path + ".glyphs"): per face (a hash of the font's
 * data, the point size, variation axes and shape features LibWeb asked the typeface for) the strike's
 * SkFont parameters, the paint color, and each glyph with its quarter-pixel offset. The list also
 * keeps, marked offscreen, the glyphs those frames' display lists hold outside the damage: text below
 * the fold or scrolled away, which a later scroll draws. Those are looked for once per page and font
 * (the walk costs 0.1-0.6 ms a frame on a long page), on the first run that draws the font there.
 * When LibWeb makes a web font's typeface (patch 0075: an @font-face rule's or a FontFace's, before
 * the restyle and layout the font triggers), one worker thread draws the list's onscreen glyphs for
 * that font one by one into a 16x16 raster canvas, during the rest of kr_view_advance_time. Its
 * offscreen glyphs wait for the first render after that: the worker warms them between renders. It
 * gets each face from Gfx::Typeface::font(), whose cache LibWeb's style then hits: with variations,
 * every Font has its own SkTypeface clone, and strikes are per SkTypeface, so warming any other
 * typeface would fill strikes nothing draws from. Skia's CPU text path asks the same
 * strike as Ganesh's direct masks when the font, paint color, color space (sRGB), surface props (none)
 * and matrix (up to a translation by whole pixels) are the same, and makes every glyph's image as it
 * digests a run, so the flush finds them. Drawing each glyph at its quarter-pixel offset
 * picks the same subpixel image.
 *
 * The render stops the worker before it replays (stop(): the glyph in progress finishes), so nothing
 * runs alongside the replay or flush, which rasterizes what the worker hasn't reached. After the
 * render, resume() starts it again on what is left, so glyphs the render didn't draw are still warmed
 * before a later frame draws them.
 *
 * Not warmed: runs Ganesh draws as distance fields (18 device px and up, GrContextOptions'
 * fMinDistanceFieldFontSize) or as paths, vertical runs, runs under a transform other than a
 * translation, and the software backend. KR_WEBFONT_GLYPHS=0 turns it off.
 */
#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <atomic>
#include <core/SkRefCnt.h>
#include <core/SkTypeface.h>
#include <mutex>
#include <thread>

namespace Gfx {
class Typeface;
}

namespace Karouka {

bool web_font_glyphs_enabled();

class WebFontGlyphs {
public:
    AK_ALLOC_WITH_KMALLOC;

    // `path`: the list's file, read at the first web font and written by save().
    // `distance_field_text`: the views draw through Ganesh, whose distance-field runs aren't warmed.
    WebFontGlyphs(ByteString path, bool distance_field_text);
    ~WebFontGlyphs();

    // LibWeb made a web font's typeface (main thread) while the views showed `pages` (page_hash()).
    // Starts the worker on the glyphs those pages drew onscreen with it.
    void typeface_created(Gfx::Typeface const&, Vector<u32> const& pages);

    // Before a render's replay: lets the worker finish its glyph and joins it. What it didn't reach
    // stays queued for resume().
    void stop();

    // After a render (and its record()): starts the worker on what is left, offscreen glyphs included.
    void resume();

    // After a recorded frame's replay: adds the glyphs the frame of `page` drew inside `damage` in a
    // web font to the list, and those its display list holds elsewhere as offscreen.
    void record(
        u32 page,
        Compositing::DisplayList const&,
        Compositing::AccumulatedVisualContextTree const&,
        Compositing::DisplayListResourceStorage const&,
        Compositing::ScrollStateSnapshot const&,
        RefPtr<Gfx::PaintingSurface>,
        Vector<Gfx::IntRect, 8> const& damage);

    static u32 page_hash(StringView url);

    bool has_web_fonts() const { return !m_font_hashes.is_empty(); }

    // Stops the worker for good and writes the list if record() added to it.
    void save();

    // A font LibWeb asks the web font's typeface for (Gfx::Typeface::font's key): each one has its own
    // SkTypeface when the font has variations, so its own strikes.
    struct Face {
        u64 font_hash { 0 }; // of the web font's data
        float point_size { 0 };
        Vector<u32> variation_tags; // sorted
        Vector<float> variation_values;
        Vector<u64> features; // ShapeFeature: tag << 32 | value

        bool operator==(Face const&) const = default;
    };

    // A glyph to draw: the strike (face + SkFont parameters + paint color) and the glyph at its
    // quarter-pixel offset. Written to the file as is.
    struct Entry {
        u32 page; // page_hash() of the document that drew it
        u32 face;
        float size;
        float scale_x;
        float skew_x;
        u32 color;
        u16 glyph;
        u8 flags; // FlagBits
        u8 subpixel_x; // 0-3: quarters of a pixel
        u8 subpixel_y;
        u8 edging;
        u8 hinting;
        u8 offscreen; // 1: no recorded frame drew it, but its display list held it outside the damage

        // The same glyph image, onscreen or not.
        bool operator==(Entry const& other) const
        {
            return page == other.page && face == other.face && size == other.size && scale_x == other.scale_x
                && skew_x == other.skew_x && color == other.color && glyph == other.glyph && flags == other.flags
                && subpixel_x == other.subpixel_x && subpixel_y == other.subpixel_y && edging == other.edging
                && hinting == other.hinting;
        }
    };
    static_assert(sizeof(Entry) == 32);

private:
    void load_if_needed();
    void work();
    void start_worker();
    u32 face_index(Face&&);

    // A web font's glyphs to warm. Jobs are freed only while no worker runs.
    struct Job {
        AK_MAKE_NONCOPYABLE(Job);
        AK_MAKE_NONMOVABLE(Job);

    public:
        AK_ALLOC_WITH_KMALLOC;

        Job(RefPtr<Gfx::Typeface const> typeface, u64 font_hash, Vector<u32> pages)
            : typeface(move(typeface))
            , font_hash(font_hash)
            , pages(move(pages))
        {
        }

        bool has_work(size_t entries) const { return next_entry[0] < entries || (offscreen_allowed && next_entry[1] < entries); }

        RefPtr<Gfx::Typeface const> typeface;
        u64 font_hash;
        Vector<u32> pages;
        // The next of m_entries to look at for onscreen [0] and offscreen [1] glyphs (worker only).
        size_t next_entry[2] { 0, 0 };
        // A render has happened since the font was made (under m_mutex).
        bool offscreen_allowed { false };
        // The SkTypeface of each face's Font (worker only).
        Vector<sk_sp<SkTypeface>> face_typefaces;
    };

    ByteString m_path;
    bool m_distance_field_text { false };
    bool m_loaded { false };
    bool m_dirty { false };
    // Read by the worker; changed by the main thread only while no worker runs.
    Vector<Face> m_faces;
    Vector<Entry> m_entries;
    HashMap<Entry, size_t> m_entry_indices;
    // Per page, the web fonts whose glyphs record() has looked for offscreen.
    HashMap<u32, HashTable<u64>> m_offscreen_walks;
    // The data of each web font LibWeb made (shared by the typefaces it derives for variations) → its
    // hash (main thread only).
    HashMap<u8 const*, u64> m_font_hashes;

    std::mutex m_mutex;
    Vector<NonnullOwnPtr<Job>> m_jobs; // under m_mutex
    std::thread m_thread;
    bool m_running { false }; // under m_mutex: m_thread has jobs to do
    std::atomic<bool> m_stop { false };
    // Stats for KR_DEBUG.
    MonotonicTime m_started { MonotonicTime::now_coarse() };
    std::atomic<i64> m_worked_us { 0 }; // from m_started to the worker's return
    std::atomic<u32> m_warmed[2] { 0, 0 }; // onscreen, offscreen
    u32 m_queued[2] { 0, 0 };
};

}

template<>
struct AK::Traits<Karouka::WebFontGlyphs::Entry> : public AK::DefaultTraits<Karouka::WebFontGlyphs::Entry> {
    static unsigned hash(Karouka::WebFontGlyphs::Entry const& entry)
    {
        return pair_int_hash(pair_int_hash(entry.page, entry.face), pair_int_hash(bit_cast<u32>(entry.size), pair_int_hash(entry.color, (entry.glyph << 16) | (entry.subpixel_y << 8) | entry.subpixel_x)));
    }
};
