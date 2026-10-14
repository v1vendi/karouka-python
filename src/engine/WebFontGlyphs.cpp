/*
 * Warming a web font's glyphs as soon as its typeface exists; see WebFontGlyphs.h.
 */
#include "WebFontGlyphs.h"

#include "HostInterface.h"

#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCore/File.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/ShapeFeature.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/PaintingSurface.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkFont.h>
#include <core/SkImageInfo.h>
#include <core/SkPaint.h>
#include <core/SkSurface.h>
#include <core/SkTextBlob.h>
#include <core/SkTypeface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace Karouka {

bool web_font_glyphs_enabled()
{
    static bool const enabled = [] {
        auto const* value = getenv("KR_WEBFONT_GLYPHS");
        return !value || strcmp(value, "0") != 0;
    }();
    return enabled;
}

namespace {

constexpr u32 file_magic = 0x4c47524b; // "KRGL"
constexpr u32 file_version = 3;
// The list keeps this many glyphs at most; a save over it keeps the glyphs of this run's fonts first.
constexpr size_t max_entries = 16 * 1024;
// A recorded frame adds at most this many offscreen glyphs: a long page's text below the fold is
// warmed only between renders, but it still costs the worker's time and the list's room.
constexpr size_t max_offscreen_entries_per_frame = 2048;
// GrContextOptions::fMinDistanceFieldFontSize, which karouka leaves at Skia's default
// (create_skia_metal_context): Ganesh draws a run this size or larger as distance fields.
constexpr float min_distance_field_text_size = 18;
// sktext::gpu's kSkSideTooBigForAtlas: larger glyphs are drawn as paths.
constexpr float max_mask_text_size = 256;

enum FlagBits : u8 {
    Subpixel = 1 << 0,
    LinearMetrics = 1 << 1,
    Embolden = 1 << 2,
    BaselineSnap = 1 << 3,
    ForceAutoHinting = 1 << 4,
};

// Not cryptographic: the list only needs to tell one font's data from another's. An sfnt's table
// directory, at its start, holds each table's checksum, so the data's start, end and size are enough,
// and the hook stays at a few microseconds for a font of hundreds of KB.
u64 hash_font_data(ReadonlyBytes bytes)
{
    constexpr size_t head_bytes = 64 * 1024;
    constexpr size_t tail_bytes = 4 * 1024;
    u64 hash = 0xcbf29ce484222325ull ^ bytes.size();
    auto mix = [&](ReadonlyBytes part) {
        size_t i = 0;
        for (; i + 8 <= part.size(); i += 8) {
            u64 word;
            memcpy(&word, part.data() + i, 8);
            hash = (hash ^ word) * 0x100000001b3ull;
            hash ^= hash >> 29;
        }
        for (; i < part.size(); ++i)
            hash = (hash ^ part[i]) * 0x100000001b3ull;
    };
    if (bytes.size() <= head_bytes + tail_bytes) {
        mix(bytes);
    } else {
        mix(bytes.slice(0, head_bytes));
        mix(bytes.slice(bytes.size() - tail_bytes));
    }
    return hash;
}

// Skia's SkPackedGlyphID rounding: a glyph's device position plus 1/8 px, in quarters of a pixel.
u8 quarter_pixel(float position)
{
    auto quarters = static_cast<int>(floorf((position + 0.125f) * 4));
    return static_cast<u8>(((quarters % 4) + 4) % 4);
}

// Walks the commands a replay into the damage would play (or, with `cull` false, every command),
// drawing nothing, and collects the glyphs of the horizontal runs in a web font drawn with no more
// than a translation (as DisplayListCensus walks a frame).
class GlyphCollector final : public Compositing::DisplayListPlayer {
public:
    GlyphCollector(Vector<Gfx::IntRect, 8> const& damage, bool cull, bool distance_field_text, HashMap<u8 const*, u64> const& font_hashes, Function<u32(Gfx::Font const&, u64)> face, Function<void(WebFontGlyphs::Entry const&)> add)
        : m_damage(damage)
        , m_cull(cull)
        , m_distance_field_text(distance_field_text)
        , m_font_hashes(font_hashes)
        , m_face(move(face))
        , m_add(move(add))
    {
    }

    void flush(Gfx::PaintingSurface&) override { }

private:
#define COLLECT_PLAY_COMMAND(command, player_method) \
    void play_command(Compositing::command const& c) override { on(c); }
    ENUMERATE_DISPLAY_LIST_COMMANDS(COLLECT_PLAY_COMMAND)
#undef COLLECT_PLAY_COMMAND

    template<typename T>
    void on(T const&) { }

    void on(Compositing::DrawGlyphRun const& c)
    {
        if (!m_state.translation_only || c.orientation != Gfx::Orientation::Horizontal || c.color.alpha() == 0)
            return;
        auto glyphs = inline_objects<Compositing::DisplayListGlyph>(c.glyphs);
        if (glyphs.is_empty() || !resource_storage().has_font(c.font_id))
            return;
        auto const& font = resource_storage().font(c.font_id);
        if (!font.typeface().is_skia())
            return;
        // A typeface LibWeb derives for variations shares the web font's data.
        auto font_hash = m_font_hashes.get(as<Gfx::TypefaceSkia>(font.typeface()).buffer().data());
        if (!font_hash.has_value() || font.is_invisible())
            return;
        auto blob = resource_storage().text_blob(c.font_id, c.scale, glyphs, c.font_smoothing);
        if (!blob)
            return;
        auto origin = SkPoint::Make(m_state.offset.x() + c.translation.x(), m_state.offset.y() + c.translation.y());
        SkTextBlob::Iter iterator { *blob };
        SkTextBlob::Iter::ExperimentalRun run;
        Optional<u32> face;
        while (iterator.experimentalNext(&run)) {
            auto const& sk_font = run.font;
            auto size = sk_font.getSize();
            if (size <= 0 || size >= max_mask_text_size || (m_distance_field_text && size >= min_distance_field_text_size))
                continue;
            if (!face.has_value())
                face = m_face(font, *font_hash);
            WebFontGlyphs::Entry entry {};
            entry.face = *face;
            entry.size = size;
            entry.scale_x = sk_font.getScaleX();
            entry.skew_x = sk_font.getSkewX();
            entry.color = SkColorSetARGB(c.color.alpha(), c.color.red(), c.color.green(), c.color.blue());
            entry.flags = (sk_font.isSubpixel() ? Subpixel : 0) | (sk_font.isLinearMetrics() ? LinearMetrics : 0)
                | (sk_font.isEmbolden() ? Embolden : 0) | (sk_font.isBaselineSnap() ? BaselineSnap : 0)
                | (sk_font.isForceAutoHinting() ? ForceAutoHinting : 0);
            entry.edging = static_cast<u8>(sk_font.getEdging());
            entry.hinting = static_cast<u8>(sk_font.getHinting());
            for (int i = 0; i < run.count; ++i) {
                entry.glyph = run.glyphs[i];
                entry.subpixel_x = quarter_pixel(origin.x() + run.positions[i].x());
                entry.subpixel_y = quarter_pixel(origin.y() + run.positions[i].y());
                m_add(entry);
            }
        }
    }

    void on(Compositing::DrawIsolatedGroup const& c)
    {
        execute_command_bytes(inline_data(c.content), active_scroll_state());
    }

    void set_matrix(Gfx::FloatMatrix4x4 const& matrix) override
    {
        m_state.matrix = matrix;
        m_state.translation_only = matrix[0, 0] == 1 && matrix[1, 1] == 1 && matrix[2, 2] == 1 && matrix[3, 3] == 1
            && matrix[0, 1] == 0 && matrix[1, 0] == 0 && matrix[0, 2] == 0 && matrix[1, 2] == 0
            && matrix[2, 0] == 0 && matrix[2, 1] == 0 && matrix[3, 0] == 0 && matrix[3, 1] == 0 && matrix[3, 2] == 0;
        m_state.offset = { matrix[0, 3], matrix[1, 3] };
    }
    Gfx::FloatMatrix4x4 canvas_matrix() const override { return m_state.matrix; }

    // The replay skips a command whose bounds miss its clip, which is the damage at most.
    bool would_be_fully_clipped_by_painter(Gfx::IntRect rect) const override
    {
        if (!m_cull || !m_state.translation_only)
            return false;
        auto device_rect = rect.to_type<float>().translated(m_state.offset).to_type<int>().inflated(2, 2);
        for (auto const& damage : m_damage) {
            if (damage.intersects(device_rect))
                return false;
        }
        return true;
    }

    void push_clip(Compositing::ReplayClip const&) override { save(); }
    void push_clip_path(Gfx::Path const&, Gfx::WindingRule) override { save(); }
    void push_transform(Gfx::AffineTransform const& transform) override
    {
        save();
        if (transform.is_identity_or_translation())
            m_state.offset.translate_by(transform.e(), transform.f());
        else
            m_state.translation_only = false;
    }
    void push_layer(Compositing::ReplayLayer const&) override { save(); }
    void push_mask(Compositing::ReplayMask const&) override { save(); }
    void pop_mask(Compositing::ReplayMask const&, Compositing::EffectNodeIndex) override { pop(); }
    void pop() override
    {
        if (!m_saved.is_empty())
            m_state = m_saved.take_last();
    }
    void push_device_space_plane_clip(Gfx::Path const&) override { save(); }

    void save() { m_saved.append(m_state); }

    struct State {
        Gfx::FloatMatrix4x4 matrix { Gfx::FloatMatrix4x4::identity() };
        Gfx::FloatPoint offset;
        bool translation_only { true };
    };
    Vector<Gfx::IntRect, 8> const& m_damage;
    bool m_cull { true };
    bool m_distance_field_text { false };
    HashMap<u8 const*, u64> const& m_font_hashes;
    Function<u32(Gfx::Font const&, u64)> m_face;
    Function<void(WebFontGlyphs::Entry const&)> m_add;
    State m_state;
    Vector<State, 16> m_saved;
};

}

WebFontGlyphs::WebFontGlyphs(ByteString path, bool distance_field_text)
    : m_path(move(path))
    , m_distance_field_text(distance_field_text)
{
}

WebFontGlyphs::~WebFontGlyphs()
{
    stop();
}

// The file: magic, version, the faces (font hash, point size, variation axes, shape features), the
// entries, and the (page, font hash) pairs whose offscreen glyphs were looked for, all in this
// machine's byte order.
void WebFontGlyphs::load_if_needed()
{
    if (exchange(m_loaded, true))
        return;
    auto file = Core::File::open(m_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto read_bytes = file.value()->read_until_eof();
    if (read_bytes.is_error())
        return;
    ReadonlyBytes bytes = read_bytes.value();
    size_t offset = 0;
    auto read = [&](void* out, size_t size) {
        if (offset + size > bytes.size())
            return false;
        memcpy(out, bytes.data() + offset, size);
        offset += size;
        return true;
    };
    u32 header[3];
    if (!read(header, sizeof(header)) || header[0] != file_magic || header[1] != file_version)
        return;
    Vector<Face> faces;
    for (u32 i = 0; i < header[2]; ++i) {
        Face face;
        u32 counts[2];
        if (!read(&face.font_hash, 8) || !read(&face.point_size, 4) || !read(counts, sizeof(counts)) || counts[0] > 64 || counts[1] > 64)
            return;
        face.variation_tags.resize(counts[0]);
        face.variation_values.resize(counts[0]);
        face.features.resize(counts[1]);
        if (!read(face.variation_tags.data(), counts[0] * 4) || !read(face.variation_values.data(), counts[0] * 4) || !read(face.features.data(), counts[1] * 8))
            return;
        faces.append(move(face));
    }
    u32 entry_count;
    if (!read(&entry_count, 4) || bytes.size() - offset < static_cast<size_t>(entry_count) * sizeof(Entry))
        return;
    Vector<Entry> entries;
    entries.resize(entry_count);
    read(entries.data(), entry_count * sizeof(Entry));
    for (auto const& entry : entries) {
        if (entry.face >= faces.size() || entry.offscreen > 1)
            return;
    }
    u32 walk_count;
    if (!read(&walk_count, 4) || bytes.size() - offset != static_cast<size_t>(walk_count) * 16)
        return;
    HashMap<u32, HashTable<u64>> offscreen_walks;
    for (u32 i = 0; i < walk_count; ++i) {
        u32 page[2];
        u64 font_hash;
        read(page, sizeof(page));
        read(&font_hash, 8);
        offscreen_walks.ensure(page[0]).set(font_hash);
    }
    m_faces = move(faces);
    m_entries = move(entries);
    m_offscreen_walks = move(offscreen_walks);
    for (size_t i = 0; i < m_entries.size(); ++i)
        m_entry_indices.set(m_entries[i], i);
}

u32 WebFontGlyphs::face_index(Face&& face)
{
    for (u32 i = 0; i < m_faces.size(); ++i) {
        if (m_faces[i] == face)
            return i;
    }
    m_faces.append(move(face));
    return m_faces.size() - 1;
}

u32 WebFontGlyphs::page_hash(StringView url)
{
    return url.hash();
}

void WebFontGlyphs::typeface_created(Gfx::Typeface const& typeface, Vector<u32> const& pages)
{
    if (!typeface.is_skia())
        return;
    auto const& skia_typeface = static_cast<Gfx::TypefaceSkia const&>(typeface);
    auto const* sk_typeface = skia_typeface.sk_typeface();
    if (!sk_typeface)
        return;
    auto start = host_now_ms();
    auto font_hash = hash_font_data(skia_typeface.buffer());
    m_font_hashes.set(skia_typeface.buffer().data(), font_hash);
    load_if_needed();
    u32 glyphs[2] = { 0, 0 };
    for (auto const& entry : m_entries) {
        if (m_faces[entry.face].font_hash == font_hash && pages.contains_slow(entry.page))
            ++glyphs[entry.offscreen];
    }
    if (debug_enabled())
        log(KR_LOG_DEBUG, "web font glyphs: '{}' ({} bytes, {:016x}) made, {} glyphs to warm and {} offscreen ({:.3f} ms)", typeface.family(), skia_typeface.buffer().size(), font_hash, glyphs[0], glyphs[1], host_now_ms() - start);
    if (!glyphs[0] && !glyphs[1])
        return;

    std::lock_guard lock(m_mutex);
    for (auto const& job : m_jobs) {
        if (job->typeface.ptr() == &typeface && job->pages == pages)
            return;
    }
    m_jobs.append(adopt_own(*new Job(typeface, font_hash, pages)));
    m_queued[0] += glyphs[0];
    m_queued[1] += glyphs[1];
    // Offscreen glyphs wait for resume(), after a render.
    if (glyphs[0])
        start_worker();
}

// Under m_mutex.
void WebFontGlyphs::start_worker()
{
    if (m_running)
        return;
    // A worker that ran out of jobs has returned or is about to.
    if (m_thread.joinable())
        m_thread.join();
    m_running = true;
    m_stop.store(false, std::memory_order_relaxed);
    m_started = MonotonicTime::now();
    m_worked_us = 0;
    m_thread = std::thread([this] { work(); });
}

void WebFontGlyphs::work()
{
    // Skia's CPU text path picks the strike as Ganesh does for a target with the view surface's color
    // space (sRGB: kFakeGammaAndBoostContrast) and surface props (none: no LCD), and makes every
    // glyph's image as it digests the run, before it culls glyphs to the canvas. 16x16 pixels are
    // enough: each glyph is moved by whole pixels to overlap them, which keeps its subpixel offset.
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(16, 16, SkColorSpace::MakeSRGB()));
    if (!surface) {
        std::lock_guard lock(m_mutex);
        m_running = false;
        return;
    }
    auto& canvas = *surface->getCanvas();
    ScopeGuard account = [&] { m_worked_us.store((MonotonicTime::now() - m_started).to_microseconds(), std::memory_order_relaxed); };
    // The main thread changes m_entries and m_faces only while no worker runs.
    auto const entry_count = m_entries.size();
    while (true) {
        Job* job = nullptr;
        u8 offscreen = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_stop.load(std::memory_order_relaxed)) {
                m_running = false;
                return;
            }
            // Every font's onscreen glyphs first: the next frame may draw them.
            for (auto& candidate : m_jobs) {
                if (candidate->next_entry[0] < entry_count) {
                    job = candidate.ptr();
                    break;
                }
            }
            if (!job) {
                offscreen = 1;
                for (auto& candidate : m_jobs) {
                    if (candidate->offscreen_allowed && candidate->next_entry[1] < entry_count) {
                        job = candidate.ptr();
                        break;
                    }
                }
            }
            if (!job) {
                m_running = false;
                return;
            }
        }
        auto& next_entry = job->next_entry[offscreen];
        while (next_entry < entry_count && !m_stop.load(std::memory_order_relaxed)) {
            auto const& entry = m_entries[next_entry++];
            if (entry.offscreen != offscreen || m_faces[entry.face].font_hash != job->font_hash || !job->pages.contains_slow(entry.page))
                continue;
            if (job->face_typefaces.size() <= entry.face)
                job->face_typefaces.resize(m_faces.size());
            auto& sk_typeface = job->face_typefaces[entry.face];
            if (!sk_typeface) {
                // The font LibWeb's style will ask the typeface for (its cache keeps it): with
                // variations, the SkTypeface it derives for them is made here, and the strikes are
                // that SkTypeface's.
                auto const& face = m_faces[entry.face];
                Gfx::FontVariationSettings variations;
                for (size_t i = 0; i < face.variation_tags.size(); ++i)
                    variations.axes.set(Gfx::FourCC::from_u32(face.variation_tags[i]), face.variation_values[i]);
                Gfx::ShapeFeatures features;
                for (auto feature : face.features) {
                    Gfx::ShapeFeature shape_feature {};
                    u32 tag = feature >> 32;
                    memcpy(shape_feature.tag, &tag, 4);
                    shape_feature.value = static_cast<u32>(feature);
                    features.append(shape_feature);
                }
                auto font = job->typeface->font(face.point_size, variations, features);
                sk_typeface = sk_ref_sp(const_cast<SkTypeface*>(as<Gfx::TypefaceSkia>(font->typeface()).sk_typeface()));
            }
            SkFont sk_font { sk_typeface, entry.size, entry.scale_x, entry.skew_x };
            sk_font.setSubpixel(entry.flags & Subpixel);
            sk_font.setLinearMetrics(entry.flags & LinearMetrics);
            sk_font.setEmbolden(entry.flags & Embolden);
            sk_font.setBaselineSnap(entry.flags & BaselineSnap);
            sk_font.setForceAutoHinting(entry.flags & ForceAutoHinting);
            sk_font.setEdging(static_cast<SkFont::Edging>(entry.edging));
            sk_font.setHinting(static_cast<SkFontHinting>(entry.hinting));
            SkPaint paint;
            paint.setColor(entry.color);
            // One glyph a draw: the CPU path holds the strike's lock while it digests a run.
            SkTextBlobBuilder builder;
            auto const& buffer = builder.allocRunPos(sk_font, 1);
            buffer.glyphs[0] = entry.glyph;
            buffer.points()[0] = SkPoint::Make(entry.subpixel_x / 4.0f, entry.subpixel_y / 4.0f);
            auto blob = builder.make();
            if (!blob)
                continue;
            auto bounds = blob->bounds();
            canvas.resetMatrix();
            canvas.translate(-floorf(bounds.left()), -floorf(bounds.top()));
            canvas.drawTextBlob(blob.get(), 0, 0, paint);
            m_warmed[offscreen].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void WebFontGlyphs::stop()
{
    if (!m_thread.joinable())
        return;
    m_stop.store(true, std::memory_order_relaxed);
    auto stop_start = MonotonicTime::now();
    m_thread.join();
    m_thread = {};
    if (debug_enabled())
        log(KR_LOG_DEBUG, "web font glyphs: warmed {} of {} glyphs and {} of {} offscreen; the worker ran {:.3f} ms of the {:.3f} ms before the render, which waited {:.3f} ms for it", m_warmed[0].load(), m_queued[0], m_warmed[1].load(), m_queued[1], m_worked_us.load() / 1000.0, (stop_start - m_started).to_microseconds() / 1000.0, (MonotonicTime::now() - stop_start).to_microseconds() / 1000.0);
    // A font whose glyphs are all warmed is done, and its typeface LibWeb's alone again.
    std::lock_guard lock(m_mutex);
    m_jobs.remove_all_matching([&](auto const& job) {
        return job->offscreen_allowed && !job->has_work(m_entries.size());
    });
    if (m_jobs.is_empty()) {
        m_warmed[0] = 0;
        m_warmed[1] = 0;
        m_queued[0] = 0;
        m_queued[1] = 0;
    }
}

void WebFontGlyphs::resume()
{
    std::lock_guard lock(m_mutex);
    bool has_work = false;
    for (auto& job : m_jobs) {
        job->offscreen_allowed = true;
        has_work |= job->has_work(m_entries.size());
    }
    if (has_work)
        start_worker();
}

void WebFontGlyphs::record(
    u32 page,
    Compositing::DisplayList const& display_list,
    Compositing::AccumulatedVisualContextTree const& visual_context_tree,
    Compositing::DisplayListResourceStorage const& resource_storage,
    Compositing::ScrollStateSnapshot const& scroll_state,
    RefPtr<Gfx::PaintingSurface> surface,
    Vector<Gfx::IntRect, 8> const& damage)
{
    if (m_font_hashes.is_empty())
        return;
    VERIFY(!m_thread.joinable());
    load_if_needed();
    auto face = [this](Gfx::Font const& font, u64 font_hash) {
        Face face;
        face.font_hash = font_hash;
        face.point_size = font.point_size();
        for (auto const& axis : font.variation_settings().to_sorted_list()) {
            face.variation_tags.append(axis.tag.to_u32());
            face.variation_values.append(axis.value);
        }
        for (auto const& feature : font.features()) {
            u32 tag;
            memcpy(&tag, feature.tag, 4);
            face.features.append((static_cast<u64>(tag) << 32) | feature.value);
        }
        return face_index(move(face));
    };
    // The damage's glyphs first, then the rest of the display list's as offscreen. That walk takes
    // longer (0.1-0.6 ms a frame on a long page, mostly making the text blobs of runs no replay drew),
    // so a page's text in a font is looked for offscreen on the first run that draws the font there,
    // until the list starts again.
    auto& walked = m_offscreen_walks.ensure(page);
    bool walk_offscreen = false;
    for (auto const& it : m_font_hashes)
        walk_offscreen |= !walked.contains(it.value);
    HashTable<u64> found;
    size_t offscreen_added = 0;
    for (u8 offscreen = 0; offscreen < (walk_offscreen ? 2 : 1); ++offscreen) {
        auto add = [&](Entry entry) {
            entry.page = page;
            entry.offscreen = offscreen;
            if (auto index = m_entry_indices.get(entry); index.has_value()) {
                // A glyph a frame draws is onscreen from then on.
                if (!offscreen && m_entries[*index].offscreen) {
                    m_entries[*index].offscreen = 0;
                    m_dirty = true;
                }
                return;
            }
            if (offscreen && offscreen_added++ >= max_offscreen_entries_per_frame)
                return;
            m_entry_indices.set(entry, m_entries.size());
            m_entries.append(entry);
            m_dirty = true;
        };
        GlyphCollector collector { damage, !offscreen, m_distance_field_text, m_font_hashes, [&](Gfx::Font const& font, u64 font_hash) {
                                      if (offscreen)
                                          found.set(font_hash);
                                      return face(font, font_hash); }, move(add) };
        collector.execute(display_list, visual_context_tree, resource_storage, scroll_state, surface);
    }
    // Once the page draws in a web font, the others made by then are taken to have nothing for it:
    // one it hasn't restyled with yet (a frame drawn before the font's load task) found nothing at all.
    if (found.is_empty())
        return;
    for (auto const& it : m_font_hashes) {
        if (walked.set(it.value) == HashSetResult::InsertedNewEntry)
            m_dirty = true;
    }
}

void WebFontGlyphs::save()
{
    stop();
    m_jobs.clear();
    if (!m_dirty)
        return;
    // Over the cap the list starts again from this run's glyphs: the fonts of earlier runs' pages are
    // dropped, and this run's are never more than a few pages' worth.
    if (m_entries.size() > max_entries) {
        HashTable<u64> current;
        for (auto const& it : m_font_hashes)
            current.set(it.value);
        Vector<Face> faces;
        Vector<Entry> entries;
        HashMap<u32, u32> face_map;
        for (auto entry : m_entries) {
            if (!current.contains(m_faces[entry.face].font_hash) || entries.size() == max_entries)
                continue;
            auto mapped = face_map.ensure(entry.face, [&] { faces.append(m_faces[entry.face]); return static_cast<u32>(faces.size() - 1); });
            entry.face = mapped;
            entries.append(entry);
        }
        m_faces = move(faces);
        m_entries = move(entries);
        for (auto& it : m_offscreen_walks)
            it.value.remove_all_matching([&](u64 font_hash) { return !current.contains(font_hash); });
    }
    auto temporary = ByteString::formatted("{}.tmp", m_path);
    auto file = Core::File::open(temporary, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return;
    ByteBuffer out;
    auto append = [&](void const* data, size_t size) { out.append(data, size); };
    u32 header[3] = { file_magic, file_version, static_cast<u32>(m_faces.size()) };
    append(header, sizeof(header));
    for (auto const& face : m_faces) {
        u32 counts[2] = { static_cast<u32>(face.variation_tags.size()), static_cast<u32>(face.features.size()) };
        append(&face.font_hash, 8);
        append(&face.point_size, 4);
        append(counts, sizeof(counts));
        append(face.variation_tags.data(), counts[0] * 4);
        append(face.variation_values.data(), counts[0] * 4);
        append(face.features.data(), counts[1] * 8);
    }
    u32 entry_count = m_entries.size();
    append(&entry_count, 4);
    append(m_entries.data(), m_entries.size() * sizeof(Entry));
    u32 walk_count = 0;
    for (auto const& it : m_offscreen_walks)
        walk_count += it.value.size();
    append(&walk_count, 4);
    for (auto const& it : m_offscreen_walks) {
        for (auto font_hash : it.value) {
            u32 page[2] = { it.key, 0 };
            append(page, sizeof(page));
            append(&font_hash, 8);
        }
    }
    if (file.value()->write_until_depleted(out).is_error()) {
        (void)unlink(temporary.characters());
        return;
    }
    file.value()->close();
    if (rename(temporary.characters(), m_path.characters()) != 0)
        (void)unlink(temporary.characters());
    else
        m_dirty = false;
    if (debug_enabled())
        log(KR_LOG_DEBUG, "web font glyphs: {} glyphs saved to {}", m_entries.size(), m_path);
}

}
