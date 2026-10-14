/*
 * Decodes images in-process with LibImageDecoders, instead of Ladybird's ImageDecoder service.
 */
#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/OwnPtr.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Karouka {

class LocalImageCodecPlugin final : public Web::Platform::ImageCodecPlugin {
public:
    // Tells LibWeb whether the decode trigger (KR_IMAGE_GC) collects for decoded pixels.
    LocalImageCodecPlugin();
    virtual ~LocalImageCodecPlugin() override;

    virtual NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    // Animated images go to LibWeb as a streaming session (AnimatedBitmapDecodedImageData), as the
    // ImageDecoder service sends them: LibWeb keeps a pool of 8 frames and asks for the next ones as
    // the animation plays. Frames are decoded and delivered synchronously, inside the request.
    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;

    // Pixel bytes of the decoded frames still referenced anywhere (KR_MEM_TAG_DECODED_IMAGES).
    static u64 decoded_image_bytes();
    // The most decoded_image_bytes() has been since the process started (KR_MEM_TAG_DECODED_IMAGES_PEAK).
    static u64 decoded_image_peak_bytes();

    // The decode trigger's collection, once decoded pixels have grown by its step since the last
    // one: a decode leaves it here unless they grew past the ceiling. The engine calls
    // this between frames: after a render's flush, and before an advance's tasks.
    static void collect_decoded_images_at_frame_boundary();

    // KR_GC_TRACE=1: counts each main-thread collection by what started it, printed at exit
    //. Call once the main thread's VM exists.
    static void start_gc_source_trace_if_enabled();

private:
    struct AnimationSession {
        AK_ALLOC_WITH_KMALLOC;

        ByteBuffer encoded;
        RefPtr<Gfx::ImageDecoder> decoder;
        u32 frame_count { 0 };
    };

    ErrorOr<Web::Platform::DecodedImage> decode(ReadonlyBytes, size_t& external_bytes);

    Mutex m_sessions_mutex;
    HashMap<i64, NonnullOwnPtr<AnimationSession>> m_sessions;
    i64 m_next_session_id { 1 };
};

}
