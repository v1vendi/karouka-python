/*
 * Embedder script types; see ScriptTypes.h.
 */
#include "ScriptTypes.h"

#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Karouka {

void ViewScriptTypes::set(String essence, ScriptTypeHandler handler)
{
    if (!handler) {
        m_entries.remove(essence);
        return;
    }
    m_entries.set(move(essence), adopt_ref(*new Entry(move(handler))));
}

RefPtr<ViewScriptTypes::Entry> ViewScriptTypes::find(StringView essence) const
{
    auto it = m_entries.find(essence);
    if (it == m_entries.end())
        return nullptr;
    return it->value;
}

Optional<String> script_type_essence(StringView mime)
{
    auto mime_type = Web::MimeSniff::MimeType::parse(mime);
    if (!mime_type.has_value() || mime_type->is_javascript())
        return {};
    return mime_type->essence();
}

}
