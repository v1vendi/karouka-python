#include "LocalImageCodecPlugin.h"

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibCore/Promise.h>
#include <LibGfx/Bitmap.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Karouka {

// KR_MEM_TAG_DECODED_IMAGES. Bitmaps are reference counted and LibWeb keeps them wherever it likes
// (decoded image data, display-list resource storage), so each decoded frame is handed out as a
// wrapper over the decoder's pixels whose destruction callback lets go of them and of their count.
// No pixels are copied. Bitmaps release from whichever thread drops the last reference.
static Atomic<u64> s_decoded_image_bytes;
static Atomic<u64> s_decoded_image_peak_bytes;
// Every byte ever counted in and out of s_decoded_image_bytes, for KR_GC_TRACE=2.
static Atomic<u64> s_decoded_image_added_total;
static Atomic<u64> s_decoded_image_released_total;

u64 LocalImageCodecPlugin::decoded_image_bytes()
{
    return s_decoded_image_bytes.load(AK::MemoryOrder::memory_order_relaxed);
}

u64 LocalImageCodecPlugin::decoded_image_peak_bytes()
{
    return s_decoded_image_peak_bytes.load(AK::MemoryOrder::memory_order_relaxed);
}

static NonnullRefPtr<Gfx::Bitmap> counted(NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    u64 bytes = bitmap->data_size();
    auto* pixels = bitmap->scanline_u8(0);
    auto wrapper = Gfx::Bitmap::create_wrapper(bitmap->format(), bitmap->alpha_type(), bitmap->size(), bitmap->pitch(), pixels, [bitmap, bytes] {
        s_decoded_image_bytes.fetch_sub(bytes, AK::MemoryOrder::memory_order_relaxed);
        s_decoded_image_released_total.fetch_add(bytes, AK::MemoryOrder::memory_order_relaxed);
    });
    if (wrapper.is_error())
        return bitmap;
    u64 now = s_decoded_image_bytes.fetch_add(bytes, AK::MemoryOrder::memory_order_relaxed) + bytes;
    s_decoded_image_added_total.fetch_add(bytes, AK::MemoryOrder::memory_order_relaxed);
    u64 peak = s_decoded_image_peak_bytes.load(AK::MemoryOrder::memory_order_relaxed);
    while (now > peak && !s_decoded_image_peak_bytes.compare_exchange_strong(peak, now, AK::MemoryOrder::memory_order_relaxed)) { }
    return wrapper.release_value();
}

// LibWeb's AnimatedBitmapDecodedImageData keeps a pool of this many frames and plays an animation
// that fits in it without asking for more (BUFFER_POOL_SIZE). Such an animation is decoded whole and
// needs no decoder afterwards; a longer one starts with the first batch (STREAMING_BATCH_SIZE in the
// ImageDecoder service) and keeps a session: a copy of the encoded bytes and the decoder over them.
static constexpr u32 frame_pool_size = 8;
static constexpr u32 streaming_batch_size = 4;

LocalImageCodecPlugin::~LocalImageCodecPlugin() = default;

static ErrorOr<Gfx::ImageFrameDescriptor> decode_frame(Gfx::ImageDecoder& decoder, size_t index)
{
    auto frame = TRY(decoder.frame(index));
    frame.image->set_alpha_type_destructive(Gfx::AlphaType::Premultiplied);
    return frame;
}

ErrorOr<Web::Platform::DecodedImage> LocalImageCodecPlugin::decode(ReadonlyBytes bytes, size_t& external_bytes)
{
    auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(bytes));
    if (!decoder)
        return Error::from_string_literal("Unsupported image format");

    Web::Platform::DecodedImage image;
    image.is_animated = decoder->is_animated();
    image.loop_count = decoder->loop_count();
    // The image's cICP chunk or ICC profile, as Ladybird's ImageDecoder sends it: LibWeb tags every
    // frame with it and Skia converts to the sRGB surface. Left default, tagged images draw
    // unconverted. An unsupported profile draws as sRGB, as in Ladybird.
    if (auto color_space = decoder->color_space(); !color_space.is_error())
        image.color_space = color_space.release_value();
    u32 frame_count = decoder->frame_count();
    if (frame_count == 0)
        return Error::from_string_literal("Could not decode image");

    // A still image, or a single frame: LibWeb keeps the first frame in a BitmapDecodedImageData.
    // LibWeb doesn't count its pixels toward the next collection; decode_image does, unless the
    // decode trigger budgets them.
    if (!image.is_animated || frame_count == 1) {
        auto frame = TRY(decode_frame(*decoder, 0));
        image.all_durations.append(frame.duration);
        image.frames.append({ counted(frame.image), static_cast<size_t>(frame.duration) });
        external_bytes = image.frames.first().bitmap->data_size();
        image.frame_count = 1;
        return image;
    }

    // Animated: a streaming session (session_id != 0) makes LibWeb play it.
    bool const streams = frame_count > frame_pool_size;
    OwnPtr<AnimationSession> session;
    if (streams) {
        // The caller's bytes don't outlive this call; the session decodes later frames from a copy.
        session = make<AnimationSession>();
        session->encoded = TRY(ByteBuffer::copy(bytes));
        session->decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(session->encoded.bytes()));
        if (!session->decoder)
            return Error::from_string_literal("Unsupported image format");
        session->frame_count = frame_count;
        decoder = session->decoder;
    }

    image.all_durations.ensure_capacity(frame_count);
    for (u32 i = 0; i < frame_count; ++i)
        image.all_durations.unchecked_append(decoder->frame_duration(i));

    u32 const initial_frames = streams ? streaming_batch_size : frame_count;
    for (u32 i = 0; i < initial_frames; ++i) {
        auto frame_or_error = decode_frame(*decoder, i);
        if (frame_or_error.is_error())
            break;
        auto frame = frame_or_error.release_value();
        if (image.all_durations[i] == 0)
            image.all_durations[i] = frame.duration;
        image.frames.append({ counted(frame.image), static_cast<size_t>(frame.duration) });
    }
    if (image.frames.is_empty())
        return Error::from_string_literal("Could not decode image");

    // Frames that fail to decode are left out, as before sessions: play what decoded, and show a
    // lone frame as a still image (no session, no animation timer).
    if (!streams && image.frames.size() < frame_count) {
        frame_count = image.frames.size();
        image.all_durations.shrink(frame_count);
    }
    image.frame_count = frame_count;
    // LibWeb doesn't count an animation's pixels toward the next collection either, so count what
    // the image data will hold once its pool is full, and the session's encoded bytes.
    external_bytes = image.frames.first().bitmap->data_size() * min(frame_count, frame_pool_size);
    if (frame_count == 1)
        return image;

    MutexLocker locker { m_sessions_mutex };
    image.session_id = m_next_session_id++;
    if (session) {
        external_bytes += session->encoded.size();
        m_sessions.set(image.session_id, session.release_nonnull());
    }
    return image;
}

// LibGC already counts decoded pixels, both as they are allocated (BitmapDecodedImageData,
// and decode_image below for animations) and as live external memory of the image data cells. But
// a page that swaps images can hold several times what it shows: the next collection waits for
// 1.75x the last live bytes and sweeps on a timer, so pages/tests/animated_churn.html (~5 GIFs of
// 4 MB a frame) reaches 44 MB of decoded pixels without this. So once decoded pixels grew by
// max(8 MB, 3/4 of what was left after the last such collection) since then, collect, sweeping at
// once: before a decode, or at the frame boundary when the frame's last decode took them past it
// (below). The document's image caches mark only what their prune would keep of
// unused images (patch 0081), so that collection frees the pixels of the replaced elements
// it finalizes. When it frees no pixels, it collects once more (once in 600 frames of
// resource_churn, never on animated_churn), so a page whose images are all still in use pays two
// collections per 3/4 of its live pixels decoded. KR_IMAGE_GC=0 turns it off.
//
// while the trigger is on, decoded pixels don't count toward LibGC's own threshold (8 MB
// allocated at least): the trigger already collects for them. Counted, they reached that threshold
// between two triggers on every other batch of animated_churn, one more collection per two
// triggers (1796 instead of 1198 per 600 frames) at the same 16 MB peak. With the trigger off they
// count as they decode: nothing else would collect for them.
//
// KR_IMAGE_GC_STEP_MB=N replaces the 8 MB minimum step with N MB, for sweeps of the step's
// collections-for-peak trade-off; unset, empty or 0 keeps 8 MB.
static u64 decoded_image_growth_min()
{
    static u64 const step = [] {
        auto const* value = getenv("KR_IMAGE_GC_STEP_MB");
        u64 megabytes = value ? strtoull(value, nullptr, 10) : 0;
        return (megabytes ? megabytes : 8) * MiB;
    }();
    return step;
}
static u64 s_decoded_image_floor;

static bool decode_trigger_enabled()
{
    static bool const enabled = [] {
        auto const* value = getenv("KR_IMAGE_GC");
        return !value || strcmp(value, "0") != 0;
    }();
    return enabled;
}

LocalImageCodecPlugin::LocalImageCodecPlugin() = default;

// KR_GC_TRACE=1 names what started each main-thread collection and prints the counts at exit
// (`kr_gc_sources`); KR_GC_TRACE=2 also prints a line per collection. A post-GC task, re-queued
// after each collection, runs inside collect_garbage(), so its stack names the caller: the decode
// trigger below (which says so itself), LibGC's allocation threshold (will_allocate, from a cell or
// from external memory), its idle timer, a deferral ending or anything else.
static int gc_trace_level()
{
    static int const level = [] {
        auto const* value = getenv("KR_GC_TRACE");
        return value ? atoi(value) : 0;
    }();
    return level;
}

enum class GCSource : u8 {
    Trigger,
    FrameBoundary,
    TriggerRetry,
    ThresholdCell,
    ThresholdExternal,
    Idle,
    DeferralEnd,
    Other,
    Count,
};
static constexpr char const* gc_source_names[] = { "trigger", "frame_boundary", "trigger_retry", "threshold_cell", "threshold_external", "idle", "deferral_end", "other" };
static u64 s_gc_source_counts[to_underlying(GCSource::Count)];
static Optional<GCSource> s_gc_source_now;
static char s_gc_other_example[1024];

static GCSource classify_collection_from_stack()
{
    void* frames[48];
    int count = backtrace(frames, 48);
    bool after_will_allocate = false;
    for (int i = 0; i < count; ++i) {
        Dl_info info;
        if (!dladdr(frames[i], &info) || !info.dli_sname)
            continue;
        StringView name { info.dli_sname, strlen(info.dli_sname) };
        if (name.contains("did_allocate_external_memory"sv))
            return GCSource::ThresholdExternal;
        if (after_will_allocate)
            return GCSource::ThresholdCell;
        if (name.contains("will_allocate"sv)) {
            after_will_allocate = true;
            continue;
        }
        if (name.contains("idle_gc_on_timer"sv) || name.contains("start_idle_gc_timer"sv))
            return GCSource::Idle;
        if (name.contains("undefer_gc"sv) || name.contains("DeferGC"sv))
            return GCSource::DeferralEnd;
    }
    if (after_will_allocate)
        return GCSource::ThresholdCell;
    if (!s_gc_other_example[0]) {
        size_t used = 0;
        bool past_collect_garbage = false;
        for (int i = 0; i < count && used + 2 < sizeof(s_gc_other_example); ++i) {
            Dl_info info;
            if (!dladdr(frames[i], &info) || !info.dli_sname)
                continue;
            if (!past_collect_garbage) {
                past_collect_garbage = strstr(info.dli_sname, "collect_garbage");
                continue;
            }
            used += snprintf(s_gc_other_example + used, sizeof(s_gc_other_example) - used, "%s%s", used ? " < " : "", info.dli_sname);
        }
    }
    return GCSource::Other;
}

static void print_gc_sources()
{
    u64 total = 0;
    for (auto count : s_gc_source_counts)
        total += count;
    fprintf(stderr, "kr_gc_sources total=%llu", static_cast<unsigned long long>(total));
    for (size_t i = 0; i < to_underlying(GCSource::Count); ++i)
        fprintf(stderr, " %s=%llu", gc_source_names[i], static_cast<unsigned long long>(s_gc_source_counts[i]));
    fprintf(stderr, "\n");
    if (s_gc_other_example[0])
        fprintf(stderr, "kr_gc_sources first_other=%s\n", s_gc_other_example);
}

static void enqueue_gc_source_trace()
{
    GC::Heap::the().enqueue_post_gc_task([] {
        auto source = s_gc_source_now.value_or_lazy_evaluated([] { return classify_collection_from_stack(); });
        ++s_gc_source_counts[to_underlying(source)];
        if (gc_trace_level() >= 2) {
            // Decoded and released since the last collection ended; released includes what this
            // one freed (it sweeps at once unless it is LibGC's own, incrementally swept one).
            static u64 s_added_seen, s_released_seen;
            u64 added = s_decoded_image_added_total.load(AK::MemoryOrder::memory_order_relaxed);
            u64 released = s_decoded_image_released_total.load(AK::MemoryOrder::memory_order_relaxed);
            fprintf(stderr, "kr_gc source=%s decoded_mb=%.1f decoded_since_mb=%.1f released_since_mb=%.1f\n", gc_source_names[to_underlying(source)],
                LocalImageCodecPlugin::decoded_image_bytes() / double(MiB), (added - s_added_seen) / double(MiB), (released - s_released_seen) / double(MiB));
            s_added_seen = added;
            s_released_seen = released;
        }
        enqueue_gc_source_trace();
    });
}

void LocalImageCodecPlugin::start_gc_source_trace_if_enabled()
{
    if (gc_trace_level() <= 0)
        return;
    enqueue_gc_source_trace();
    atexit(print_gc_sources);
}

// How far decoded pixels have grown since the last trigger's collection, and the step it waits for.
struct DecodedImageGrowth {
    u64 now;
    u64 growth;
    u64 step;
};
static DecodedImageGrowth decoded_image_growth()
{
    u64 now = LocalImageCodecPlugin::decoded_image_bytes();
    // A collection of LibGC's own freed some: grow from there.
    s_decoded_image_floor = min(s_decoded_image_floor, now);
    return { now, now - s_decoded_image_floor, max(decoded_image_growth_min(), s_decoded_image_floor / 4 * 3) };
}

// a decode runs inside a task, in the middle of the host's frame. The trigger's collection
// also runs at the frame boundary (collect_decoded_images_at_frame_boundary: after the render's
// submit, while the GPU draws, or before the next advance's tasks) when the frame's last decodes
// took decoded pixels past the step; a decode collects once they are past the ceiling,
// KR_IMAGE_GC_CEILING=P% of the step (unset, empty or below 100 keeps 100). Above 100 a decode leaves
// the collection to the boundary, which trades peak for collections as a larger step does: at 200,
// 299 collections all at the boundary and an 18 MB peak per 600 frames of resource_churn (375 and
// 16 MB at 100, 75 of them at the boundary), but on animated_churn (~5 GIFs of 4 MB a frame) 599
// collections still in the decodes and a 24 MB peak (1198 and 16 MB at 100).
static u64 decoded_image_ceiling(u64 step)
{
    static u64 const percent = [] {
        auto const* value = getenv("KR_IMAGE_GC_CEILING");
        u64 parsed = value ? strtoull(value, nullptr, 10) : 0;
        return parsed >= 100 ? parsed : 100ull;
    }();
    return step / 100 * percent;
}

static void collect_decoded_images(DecodedImageGrowth growth, GCSource source)
{
    auto& heap = GC::Heap::the();
    // NB: Nothing else turns incremental sweeping off (LibGC has no getter to restore from).
    heap.set_incremental_sweep_enabled(false);
    s_gc_source_now = source;
    heap.collect_garbage();
    if (LocalImageCodecPlugin::decoded_image_bytes() >= growth.now) {
        s_gc_source_now = GCSource::TriggerRetry;
        heap.collect_garbage();
    }
    s_gc_source_now = {};
    heap.set_incremental_sweep_enabled(true);
    s_decoded_image_floor = LocalImageCodecPlugin::decoded_image_bytes();
    // collect_garbage() leaves LibGC's count of bytes allocated since the last collection alone (only
    // its own threshold and idle collections reset it), though it does set the next threshold from
    // what these left live. Count from here, as LibGC's own collection would: freeing more than
    // the count zeroes it. Giving back only the pixels they freed left the cells and the pixels still
    // live counted, so every ~6 triggers LibGC collected on its own, incrementally swept.
    heap.did_free_external_memory(NumericLimits<size_t>::max());
}

static void collect_if_decoded_images_grew()
{
    if (!decode_trigger_enabled())
        return;
    auto growth = decoded_image_growth();
    if (growth.growth < decoded_image_ceiling(growth.step))
        return;
    collect_decoded_images(growth, GCSource::Trigger);
}

void LocalImageCodecPlugin::collect_decoded_images_at_frame_boundary()
{
    if (!decode_trigger_enabled())
        return;
    auto growth = decoded_image_growth();
    if (growth.growth < growth.step)
        return;
    collect_decoded_images(growth, GCSource::FrameBoundary);
}

NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> LocalImageCodecPlugin::decode_image(ReadonlyBytes bytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    auto promise = Core::Promise<Web::Platform::DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    collect_if_decoded_images_grew();

    size_t external_bytes = 0;
    auto result = decode(bytes, external_bytes);
    if (result.is_error()) {
        promise->reject(result.release_error());
        return promise;
    }
    promise->resolve(result.release_value());
    if (external_bytes && !decode_trigger_enabled())
        GC::Heap::the().did_allocate_external_memory(external_bytes);
    return promise;
}

void LocalImageCodecPlugin::request_animation_frames(i64 session_id, u32 start_frame_index, u32 count)
{
    Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps;
    {
        MutexLocker locker { m_sessions_mutex };
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end())
            return;
        auto& session = *it->value;
        if (start_frame_index >= session.frame_count)
            return;
        u32 const end_index = start_frame_index + min(count, session.frame_count - start_frame_index);
        bitmaps.ensure_capacity(end_index - start_frame_index);
        for (u32 i = start_frame_index; i < end_index; ++i) {
            auto frame_or_error = decode_frame(*session.decoder, i);
            if (frame_or_error.is_error())
                break;
            bitmaps.unchecked_append(counted(*frame_or_error.value().image));
        }
    }

    // Delivered outside the lock: LibWeb stores them in the session's AnimatedBitmapDecodedImageData,
    // whose request is still in flight (it reads which frame they start at from the request).
    if (bitmaps.is_empty()) {
        if (on_animation_decode_failed)
            on_animation_decode_failed(session_id);
        return;
    }
    if (on_animation_frames_decoded)
        on_animation_frames_decoded(session_id, move(bitmaps));
}

void LocalImageCodecPlugin::stop_animation_decode(i64 session_id)
{
    // The image data is gone (finalized): drop the decoder and the encoded copy with it.
    MutexLocker locker { m_sessions_mutex };
    m_sessions.remove(session_id);
}

}
