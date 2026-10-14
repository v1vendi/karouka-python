#include "BindingModel.h"

#include <AK/Utf8View.h>
#include <string.h>

namespace Karouka {

static bool is_identifier_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_identifier_rest(char c) { return is_identifier_start(c) || (c >= '0' && c <= '9'); }
static bool same_bits(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

bool BindingModel::path_is_valid(StringView path)
{
    if (path.is_empty())
        return false;
    bool at_segment_start = true;
    for (char c : path) {
        if (at_segment_start) {
            if (!is_identifier_start(c))
                return false;
            at_segment_start = false;
        } else if (c == '.') {
            at_segment_start = true;
        } else if (!is_identifier_rest(c)) {
            return false;
        }
    }
    return !at_segment_start; // a trailing dot leaves an empty last segment
}

kr_result BindingModel::resolve(StringView path, kr_binding_path* out)
{
    if (!out || !path_is_valid(path))
        return KR_ERROR_INVALID_ARGUMENT;
    if (path.length() > max_path_bytes)
        return KR_ERROR_LIMIT_EXCEEDED;
    if (auto found = m_index.get(path); found.has_value()) {
        *out = *found;
        return KR_OK;
    }
    if (m_slots.size() >= max_paths)
        return KR_ERROR_LIMIT_EXCEEDED;
    auto handle = static_cast<kr_binding_path>(m_slots.size() + 1);
    ByteString interned { path };
    Slot fresh;
    fresh.path = interned;
    m_slots.append(move(fresh));
    m_index.set(move(interned), handle);
    *out = handle;
    return KR_OK;
}

BindingModel::Slot* BindingModel::slot(kr_binding_path handle)
{
    return is_valid(handle) ? &m_slots[handle - 1] : nullptr;
}

BindingModel::Slot const* BindingModel::slot(kr_binding_path handle) const
{
    return is_valid(handle) ? &m_slots[handle - 1] : nullptr;
}

void BindingModel::mark_dirty(kr_binding_path handle, Slot& slot)
{
    if (slot.dirty)
        return; // coalesced: already queued in this batch, and now holds the latest value
    slot.dirty = true;
    m_dirty.append(handle);
}

kr_result BindingModel::set_number(kr_binding_path handle, double value)
{
    auto* s = slot(handle);
    if (!s)
        return KR_ERROR_INVALID_ARGUMENT;
    if (s->kind != Kind::Unset && s->kind != Kind::Number)
        return KR_ERROR_INVALID_STATE;
    if (s->kind == Kind::Number && same_bits(s->number, value))
        return KR_OK;
    s->kind = Kind::Number;
    s->number = value;
    mark_dirty(handle, *s);
    return KR_OK;
}

kr_result BindingModel::set_bool(kr_binding_path handle, bool value)
{
    auto* s = slot(handle);
    if (!s)
        return KR_ERROR_INVALID_ARGUMENT;
    if (s->kind != Kind::Unset && s->kind != Kind::Bool)
        return KR_ERROR_INVALID_STATE;
    if (s->kind == Kind::Bool && s->boolean == value)
        return KR_OK;
    s->kind = Kind::Bool;
    s->boolean = value;
    mark_dirty(handle, *s);
    return KR_OK;
}

kr_result BindingModel::set_string(kr_binding_path handle, StringView value)
{
    auto* s = slot(handle);
    if (!s)
        return KR_ERROR_INVALID_ARGUMENT;
    if (s->kind != Kind::Unset && s->kind != Kind::String)
        return KR_ERROR_INVALID_STATE;
    if (value.length() > max_string_bytes)
        return KR_ERROR_LIMIT_EXCEEDED;
    if (!Utf8View(value).validate())
        return KR_ERROR_INVALID_ARGUMENT;
    if (s->kind == Kind::String && s->string == value)
        return KR_OK;
    s->kind = Kind::String;
    s->string = value;
    mark_dirty(handle, *s);
    return KR_OK;
}

StringView BindingModel::path(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s ? s->path.view() : StringView {};
}

BindingModel::Kind BindingModel::kind(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s ? s->kind : Kind::Unset;
}

double BindingModel::number(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s ? s->number : 0;
}

bool BindingModel::boolean(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s && s->boolean;
}

StringView BindingModel::string(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s ? s->string.view() : StringView {};
}

bool BindingModel::is_dirty(kr_binding_path handle) const
{
    auto const* s = slot(handle);
    return s && s->dirty;
}

Vector<kr_binding_path> BindingModel::take_dirty()
{
    for (auto handle : m_dirty)
        m_slots[handle - 1].dirty = false;
    auto taken = move(m_dirty);
    m_dirty.clear();
    return taken;
}

}
