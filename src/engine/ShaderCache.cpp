#include "ShaderCache.h"
#include "HostInterface.h"

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>

#include <core/SkData.h>

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Karouka {

// The file: magic, format version, stamp length and stamp, entry count, then per entry the key's
// length and bytes, the data's length and bytes, its place on the warm list and its place on the
// later list (0: not on it). Lengths and places are 32-bit little-endian. Version 1 had
// no warm list and version 2 no later list; both are still read.
static constexpr u32 file_magic = 0x4353524b; // "KRSC"
static constexpr u32 file_version = 3;
// What one process stores at most. A page's first frames store ~10-20 programs of 2-10 KB each; a
// run that keeps meeting new ones (generated content) stops adding past this and keeps working.
static constexpr size_t max_cache_bytes = 8 * MiB;
static constexpr size_t max_file_bytes = max_cache_bytes + 1 * MiB;

namespace {

class Reader {
public:
    explicit Reader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    Optional<u32> u32_value()
    {
        auto bytes = take(sizeof(u32));
        if (!bytes.has_value())
            return {};
        return static_cast<u32>((*bytes)[0]) | static_cast<u32>((*bytes)[1]) << 8 | static_cast<u32>((*bytes)[2]) << 16 | static_cast<u32>((*bytes)[3]) << 24;
    }

    Optional<ReadonlyBytes> sized_bytes()
    {
        auto size = u32_value();
        if (!size.has_value())
            return {};
        return take(*size);
    }

private:
    Optional<ReadonlyBytes> take(size_t size)
    {
        if (m_bytes.size() - m_offset < size)
            return {};
        auto bytes = m_bytes.slice(m_offset, size);
        m_offset += size;
        return bytes;
    }

    ReadonlyBytes m_bytes;
    size_t m_offset { 0 };
};

void append_u32(ByteBuffer& buffer, u32 value)
{
    u8 bytes[4] = { static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(value >> 16), static_cast<u8>(value >> 24) };
    buffer.append(bytes, sizeof bytes);
}

void append_sized(ByteBuffer& buffer, ReadonlyBytes bytes)
{
    append_u32(buffer, static_cast<u32>(bytes.size()));
    buffer.append(bytes);
}

bool write_all(int fd, ReadonlyBytes bytes)
{
    while (!bytes.is_empty()) {
        auto written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes = bytes.slice(written);
    }
    return true;
}

}

NonnullOwnPtr<ShaderCache> ShaderCache::open(ByteString path, ByteString stamp)
{
    auto cache = adopt_own(*new ShaderCache(move(path), move(stamp)));
    cache->read_file();
    return cache;
}

void ShaderCache::read_file()
{
    int fd = ::open(m_path.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ScopeGuard close_fd = [fd] { ::close(fd); };
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > max_file_bytes)
        return;
    auto buffer_or_error = ByteBuffer::create_uninitialized(st.st_size);
    if (buffer_or_error.is_error())
        return;
    auto buffer = buffer_or_error.release_value();
    size_t offset = 0;
    while (offset < buffer.size()) {
        auto bytes_read = ::read(fd, buffer.data() + offset, buffer.size() - offset);
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read <= 0)
            return;
        offset += bytes_read;
    }

    Reader reader { buffer.bytes() };
    if (reader.u32_value() != file_magic)
        return;
    auto version = reader.u32_value();
    if (!version.has_value() || *version < 1u || *version > file_version)
        return;
    auto stamp = reader.sized_bytes();
    if (!stamp.has_value() || *stamp != m_stamp.bytes())
        return;
    auto count = reader.u32_value();
    if (!count.has_value())
        return;
    HashMap<ByteString, Entry> entries;
    size_t bytes = 0;
    for (u32 i = 0; i < *count; ++i) {
        auto key = reader.sized_bytes();
        auto data = key.has_value() ? reader.sized_bytes() : Optional<ReadonlyBytes> {};
        auto rank = *version < 2u ? Optional<u32> { 0 } : data.has_value() ? reader.u32_value() : Optional<u32> {};
        auto later_rank = *version < 3u ? Optional<u32> { 0 } : rank.has_value() ? reader.u32_value() : Optional<u32> {};
        // A truncated file: none of it is trusted. (Version 1 has no rank to fail on, so a missing
        // key or data must be checked itself.)
        if (!key.has_value() || !data.has_value() || !later_rank.has_value())
            return;
        bytes += key->size() + data->size();
        entries.set(ByteString(*key), Entry {
                                          .data = SkData::MakeWithCopy(data->data(), data->size()),
                                          .loaded_rank = *rank <= max_warm_entries ? *rank : 0,
                                          .loaded_later_rank = *later_rank <= max_later_entries ? *later_rank : 0,
                                      });
    }
    m_entries = move(entries);
    m_bytes = bytes;
    m_loaded_count = m_entries.size();
}

sk_sp<SkData> ShaderCache::load(SkData const& key)
{
    auto it = m_entries.find(ByteString(ReadonlyBytes { key.bytes(), key.size() }));
    if (it == m_entries.end()) {
        ++m_misses;
        return nullptr;
    }
    ++m_hits;
    mark_used(it->value);
    return it->value.data;
}

void ShaderCache::mark_used(Entry& entry)
{
    if (m_recording == Recording::FirstFrame && !entry.use_rank)
        entry.use_rank = m_next_use_rank++;
    else if (m_recording == Recording::LaterFrame && !entry.later_use_rank)
        entry.later_use_rank = m_next_later_use_rank++;
}

namespace {

// The keys a list gets: those asked for in this process (`use_rank`), earliest first, then those
// on the file's list (`loaded_rank`) never asked for, in its order, up to `limit`, skipping `taken`.
template<typename Entries, typename Rank>
HashMap<ByteString, u32> list_ranks(Entries const& entries, Rank use_rank, Rank loaded_rank, size_t limit, HashMap<ByteString, u32> const* taken)
{
    Vector<ByteString const*> used;
    Vector<ByteString const*> carried;
    for (auto& [key, entry] : entries) {
        if (taken && taken->contains(key))
            continue;
        if (entry.*use_rank)
            used.append(&key);
        else if (entry.*loaded_rank)
            carried.append(&key);
    }
    quick_sort(used, [&](auto* a, auto* b) { return (*entries.get(*a)).*use_rank < (*entries.get(*b)).*use_rank; });
    quick_sort(carried, [&](auto* a, auto* b) { return (*entries.get(*a)).*loaded_rank < (*entries.get(*b)).*loaded_rank; });
    HashMap<ByteString, u32> ranks;
    for (auto* key : used) {
        if (ranks.size() < limit)
            ranks.set(*key, ranks.size() + 1);
    }
    for (auto* key : carried) {
        if (ranks.size() < limit)
            ranks.set(*key, ranks.size() + 1);
    }
    return ranks;
}

}

void ShaderCache::store(SkData const& key, SkData const& data, SkString const&)
{
    if (m_bytes + key.size() + data.size() > max_cache_bytes)
        return;
    auto& entry = m_entries.ensure(ByteString(ReadonlyBytes { key.bytes(), key.size() }), [&] {
        m_bytes += key.size() + data.size();
        return Entry {};
    });
    entry.data = SkData::MakeWithCopy(data.data(), data.size());
    mark_used(entry);
    m_dirty = true;
}

void ShaderCache::save()
{
    // The warm list the file gets: what first frames asked for, earliest first, then what was
    // precompiled from the old list (never asked for, see precompile_warm_entries). The later list:
    // the same for later frames, without the warm list's programs.
    auto warm_ranks = list_ranks(m_entries, &Entry::use_rank, &Entry::loaded_rank, max_warm_entries, nullptr);
    auto later_ranks = list_ranks(m_entries, &Entry::later_use_rank, &Entry::loaded_later_rank, max_later_entries, &warm_ranks);
    bool warm_list_changed = false;
    for (auto& [key, entry] : m_entries) {
        if ((entry.loaded_rank != 0) != warm_ranks.contains(key) || (entry.loaded_later_rank != 0) != later_ranks.contains(key))
            warm_list_changed = true;
    }
    if (!m_dirty && !warm_list_changed)
        return;
    m_dirty = false;
    for (auto& [key, entry] : m_entries) {
        entry.loaded_rank = warm_ranks.get(key).value_or(0);
        entry.loaded_later_rank = later_ranks.get(key).value_or(0);
    }

    ByteBuffer buffer;
    append_u32(buffer, file_magic);
    append_u32(buffer, file_version);
    append_sized(buffer, m_stamp.bytes());
    append_u32(buffer, static_cast<u32>(m_entries.size()));
    for (auto& [key, entry] : m_entries) {
        append_sized(buffer, key.bytes());
        append_sized(buffer, ReadonlyBytes { entry.data->bytes(), entry.data->size() });
        append_u32(buffer, warm_ranks.get(key).value_or(0));
        append_u32(buffer, later_ranks.get(key).value_or(0));
    }

    auto directory = LexicalPath::dirname(m_path);
    if (!directory.is_empty())
        mkdir(directory.characters(), 0755); // EEXIST, the common case, is fine.
    auto temporary_path = ByteString::formatted("{}.{}.tmp", m_path, getpid());
    int fd = ::open(temporary_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        log(KR_LOG_WARN, "shader cache: can't write {}: {}", temporary_path, strerror(errno));
        return;
    }
    bool written = write_all(fd, buffer.bytes());
    ::close(fd);
    if (!written || rename(temporary_path.characters(), m_path.characters()) < 0) {
        log(KR_LOG_WARN, "shader cache: can't write {}: {}", m_path, strerror(errno));
        unlink(temporary_path.characters());
    }
}

}
