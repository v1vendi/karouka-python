/*
 * The per-view scalar binding model behind kr_binding_* (include/karouka/karouka.h).
 *
 * Tessera's contract: Tessera's data-binding spec § "2026-09-30 decision" § "Model paths
 * and the C ABI". This port of Tessera's implementation
 * keeps its behaviour and result codes:
 *
 * - A path is `identifier ("." identifier)*` over ASCII, identifier = [A-Za-z_][A-Za-z0-9_]*.
 *   Resolving interns it for the model's (the view's) lifetime; the same path gives the same handle.
 *   Handles are slot index + 1, so 0 is never a handle.
 * - A slot is unset until its first successful setter, which fixes its type (bool, number, string)
 *   for good: a setter of another type is KR_ERROR_INVALID_STATE and changes nothing.
 * - An unchanged write (numbers compare bitwise, so NaN twice is unchanged) marks nothing dirty.
 *   Changed writes queue the handle once per batch, in first-change order, holding the last value.
 *   take_dirty() ends the batch.
 * - Ceilings are inclusive and per item: max_paths paths, max_path_bytes bytes per path,
 *   max_string_bytes bytes per stored string. One more is KR_ERROR_LIMIT_EXCEEDED and changes
 *   nothing; resolving a path that already exists still succeeds when the table is full.
 * - Strings are copied; they must be valid UTF-8 (the DOM side converts them to UTF-16).
 *
 * No script runs and nothing is called back from here. Allocation failure aborts, as everywhere
 * else in LibWeb, so there is no out-of-memory result.
 */
#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <karouka/karouka.h>

namespace Karouka {

class BindingModel {
    AK_MAKE_NONCOPYABLE(BindingModel);
    AK_MAKE_NONMOVABLE(BindingModel);

public:
    static constexpr size_t max_paths = KR_BINDING_MAX_PATHS;
    static constexpr size_t max_path_bytes = KR_BINDING_MAX_PATH_BYTES;
    static constexpr size_t max_string_bytes = KR_BINDING_MAX_STRING_BYTES;

    enum class Kind : u8 {
        Unset,
        Bool,
        Number,
        String,
    };

    BindingModel() = default;

    static bool path_is_valid(StringView path);

    // On failure *out is untouched and nothing is interned.
    kr_result resolve(StringView path, kr_binding_path* out);

    kr_result set_number(kr_binding_path, double);
    kr_result set_bool(kr_binding_path, bool);
    kr_result set_string(kr_binding_path, StringView utf8);

    bool is_valid(kr_binding_path handle) const { return handle != 0 && handle <= m_slots.size(); }
    size_t path_count() const { return m_slots.size(); }
    StringView path(kr_binding_path) const;
    Kind kind(kr_binding_path) const;
    double number(kr_binding_path) const;
    bool boolean(kr_binding_path) const;
    StringView string(kr_binding_path) const;

    bool is_dirty(kr_binding_path) const;
    bool has_dirty() const { return !m_dirty.is_empty(); }
    // Handles changed since the last take, each once, in first-change order.
    Vector<kr_binding_path> const& dirty() const { return m_dirty; }
    // Ends the batch: returns the dirty handles and clears every dirty mark.
    Vector<kr_binding_path> take_dirty();

private:
    struct Slot {
        ByteString path;
        ByteString string;
        double number { 0 };
        Kind kind { Kind::Unset };
        bool boolean { false };
        bool dirty { false };
    };

    Slot* slot(kr_binding_path);
    Slot const* slot(kr_binding_path) const;
    void mark_dirty(kr_binding_path, Slot&);

    Vector<Slot> m_slots;
    HashMap<ByteString, kr_binding_path> m_index;
    Vector<kr_binding_path> m_dirty;
};

}
