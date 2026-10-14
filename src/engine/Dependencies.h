/*
 * What a view's current document was loaded from (kr_view_get_dependencies): its own file
 * and every file: URL the page's PageClient was asked to open (request_file), deduplicated, in
 * first-request order. One set per load; a request still in flight when the next load replaces the
 * set finishes into the old one, which nothing reads any more.
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <karouka/karouka.h>

namespace Karouka {

class DependencySet : public RefCounted<DependencySet> {
public:
    // kr_view_dependencies.truncated: a page requesting more distinct URLs than this (a query
    // string per request, say) doesn't grow the set without bound.
    static constexpr size_t max_entries = 4096;

    struct Entry {
        ByteString url;
        u32 kinds { 0 };
        u32 in_flight { 0 };
        kr_result status { KR_OK };
    };

    static NonnullRefPtr<DependencySet> create() { return adopt_ref(*new DependencySet); }

    // A request for `url` as `kind` starts. Returns its entry, or empty when the ceiling refused it.
    Optional<size_t> request(ByteString const& url, u32 kind)
    {
        Optional<size_t> index;
        if (auto known = m_index_of_url.get(url); known.has_value())
            index = *known;
        if (!index.has_value()) {
            if (m_entries.size() >= max_entries) {
                if (!m_truncated)
                    changed();
                m_truncated = true;
                return {};
            }
            index = m_entries.size();
            m_entries.append({ url, 0, 0, KR_OK });
            m_index_of_url.set(url, *index);
        }
        auto& entry = m_entries[*index];
        entry.kinds |= kind;
        ++entry.in_flight;
        changed();
        return index;
    }

    // A request request() started has finished with `status`.
    void finish(size_t index, kr_result status)
    {
        auto& entry = m_entries[index];
        if (entry.in_flight > 0)
            --entry.in_flight;
        entry.status = status;
        changed();
    }

    // An entry that was never in flight: the document's own file, read before the set existed.
    void add_loaded(ByteString const& url, u32 kind)
    {
        if (auto index = request(url, kind); index.has_value())
            finish(*index, KR_OK);
    }

    Vector<Entry> const& entries() const { return m_entries; }
    u64 revision() const { return m_revision; }
    bool truncated() const { return m_truncated; }

private:
    DependencySet() { changed(); }

    // Revisions come from one counter for every set, so a new set never reads as an old one.
    void changed()
    {
        static u64 s_next_revision = 0;
        m_revision = ++s_next_revision;
    }

    Vector<Entry> m_entries;
    HashMap<ByteString, size_t> m_index_of_url;
    u64 m_revision { 0 };
    bool m_truncated { false };
};

}
