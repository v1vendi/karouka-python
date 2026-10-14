/*
 * Skia's persistent shader cache, kept in one file (kr_system_desc.shader_cache_path).
 *
 * Skia's Metal backend builds a pipeline for each new combination of draw and paint it meets: it
 * generates SkSL, compiles that to Metal Shading Language, and hands the MSL to Metal, which
 * compiles it and creates the pipeline state. A page's first frame meets a dozen or so of them, all
 * inside the flush. Given a GrContextOptions::PersistentCache, GrMtlPipelineStateBuilder looks each
 * program up by its key before generating it and stores the MSL (with the pipeline's vertex layout)
 * after, so a later process that loads the entries skips SkSL generation and compilation. Metal's
 * own compile of that MSL is then served from the system's Metal shader cache, which keys on the
 * source text.
 *
 * Even then a first frame's flush paid ~0.15 ms a program for that Metal compile and pipeline
 * build, plus ~2 ms for the context's first command buffer. So the file also lists the
 * programs views' first frames used in the process that wrote it (up to max_warm_entries), and
 * kr_system_create2 has Skia precompile those on its GPU thread (GrDirectContext::precompileShader:
 * the two MTLLibrary compiles, then an asynchronous pipeline build Metal caches), alongside the VM's
 * setup, before any view renders.
 *
 * A second list, the later list, holds the programs views' next few frames (up to recorded_frames)
 * asked for that the first frame didn't (showcase_page's second frame loaded 9 from the cache
 * in its flush, ~0.15 ms each). The GPU thread precompiles those after the first frame's and the warm-up
 * clear, for as long as the main thread is still busy with its own setup, so kr_system_create2 never
 * waits on more than the one program in progress; those it doesn't reach are loaded in the flush, as
 * before.
 */
#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>

#include <atomic>

#include <core/SkData.h>
#include <gpu/ganesh/GrContextOptions.h>

namespace Karouka {

class ShaderCache final : public GrContextOptions::PersistentCache {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The cache at `path`, with the entries of the file there when it was written for the same
    // `stamp` (the Skia version and GPU; see shader_cache_stamp); empty otherwise (no file yet, or
    // one from another build or GPU, which the next save replaces).
    static NonnullOwnPtr<ShaderCache> open(ByteString path, ByteString stamp);

    // How many programs the warm list holds at most. Each costs ~0.15 ms on kr_system_create2's
    // GPU thread (that thread, not the main thread, so up to the main thread's own setup it costs
    // nothing); a page's first frame uses 2-20.
    static constexpr size_t max_warm_entries = 24;
    // How many the later list holds at most, and how many of a view's first rendered frames ask
    // for programs that go on the warm list (the first) or the later list (the rest).
    static constexpr size_t max_later_entries = 32;
    static constexpr u64 recorded_frames = 4;

    // Calls `precompile(key, data)` for each entry of the file's warm list, in its order, and
    // counts those it returns true for (precompiled_count()). Skia never asks load() for a program
    // it precompiled, so whether this process used one is unknown: it stays on the list the next
    // save writes, after the programs first frames asked for, and leaves it once the list is full
    // of newer ones.
    template<typename Callback>
    void precompile_warm_entries(Callback precompile)
    {
        precompile_list(&Entry::loaded_rank, precompile, nullptr);
    }

    // The same for the file's later list, stopping before the next entry once `stop` is set.
    template<typename Callback>
    void precompile_later_entries(Callback precompile, std::atomic<bool> const& stop)
    {
        precompile_list(&Entry::loaded_later_rank, precompile, &stop);
    }

    enum class Recording : u8 {
        None,
        FirstFrame,
        LaterFrame,
    };
    // While not None (kr_view_render, around a view's first recorded_frames frames), the programs
    // Skia asks for go on the warm list (FirstFrame) or the later list (LaterFrame) the next save
    // writes, in the order asked. Frames after those build their programs as before.
    void set_recording(Recording recording) { m_recording = recording; }

    sk_sp<SkData> load(SkData const& key) override;
    void store(SkData const& key, SkData const& data, SkString const& description) override;

    // Writes every entry back when one was stored since the file was read, or when the warm list
    // changed (the programs first frames asked for, in order, then the file's warm entries they did
    // not ask for, up to max_warm_entries; compared as a set) or the later list did (the same for
    // later frames, up to max_later_entries, without the warm list's): to a temporary file in
    // the same directory, renamed over the old one, so a process reading it concurrently sees the
    // old file or the new one. Creates the directory if it is missing.
    void save();

    size_t entry_count() const { return m_entries.size(); }
    size_t loaded_count() const { return m_loaded_count; }
    size_t hits() const { return m_hits; }
    size_t misses() const { return m_misses; }
    size_t precompiled_count() const { return m_precompiled_count; }

private:
    ShaderCache(ByteString path, ByteString stamp)
        : m_path(move(path))
        , m_stamp(move(stamp))
    {
    }

    void read_file();

    struct Entry;
    template<typename Callback>
    void precompile_list(u32 Entry::* rank, Callback& precompile, std::atomic<bool> const* stop)
    {
        Vector<ByteString const*> list;
        for (auto& [key, entry] : m_entries) {
            if (entry.*rank)
                list.append(&key);
        }
        quick_sort(list, [&](auto* a, auto* b) { return (*m_entries.get(*a)).*rank < (*m_entries.get(*b)).*rank; });
        for (auto* key : list) {
            if (stop && stop->load(std::memory_order_relaxed))
                break;
            if (precompile(*SkData::MakeWithoutCopy(key->characters(), key->length()), *m_entries.get(*key)->data))
                ++m_precompiled_count;
        }
    }

    struct Entry {
        sk_sp<SkData> data;
        // 1-based place on the file's warm list; 0 when not on it.
        u32 loaded_rank { 0 };
        // 1-based place on the file's later list; 0 when not on it.
        u32 loaded_later_rank { 0 };
        // 1-based order in which a first frame of this process first asked for it (load or store);
        // 0 when none did.
        u32 use_rank { 0 };
        // The same for the frames after a view's first, up to recorded_frames.
        u32 later_use_rank { 0 };
    };

    void mark_used(Entry&);

    ByteString m_path;
    ByteString m_stamp;
    HashMap<ByteString, Entry> m_entries;
    u32 m_next_use_rank { 1 };
    u32 m_next_later_use_rank { 1 };
    Recording m_recording { Recording::None };
    size_t m_bytes { 0 };
    size_t m_loaded_count { 0 };
    size_t m_hits { 0 };
    size_t m_misses { 0 };
    size_t m_precompiled_count { 0 };
    bool m_dirty { false };
};

}
