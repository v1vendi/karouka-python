/*
 * Embedder script types: <script type="text/python"> and any other type a view registers
 * (docs/PYTHON.md § Scripts). Ladybird patch 0099 (LibWeb/Embedder/ScriptTypes.h) runs such a
 * script on the classic-script path (parser-blocking, defer and async ordering, src= fetched as a
 * file: subresource, currentScript, load and error) and hands it to one process-wide handler;
 * Engine.cpp's handler finds the view whose page holds the script's document and calls the view's
 * handler for the type's MIME essence. A view with no handler for a type leaves it ignored.
 *
 * C hosts register through kr_view_set_script_type_callback; the Python module calls
 * set_script_type_handler directly, with the element and the script. The module also follows the
 * view's documents (set_active_document_handler, active_document): a document's Python
 * interpreter ends when another document replaces it.
 */
#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <karouka/karouka.h>
#include <LibWeb/Forward.h>

namespace Web::Embedder {
class EmbedderScript;
}

namespace Karouka {

// Runs one script of the type; false when it failed (the element then fires error, and no load).
// Called inside the document's realm; a microtask checkpoint follows when it returns to no script.
using ScriptTypeHandler = Function<bool(Web::HTML::HTMLScriptElement&, Web::Embedder::EmbedderScript const&)>;

// The script types of one view, by MIME essence. Kept across loads.
class ViewScriptTypes {
public:
    struct Entry : RefCounted<Entry> {
        explicit Entry(ScriptTypeHandler handler)
            : handler(move(handler))
        {
        }
        ScriptTypeHandler handler;
    };

    // A null handler removes the type.
    void set(String essence, ScriptTypeHandler);
    // Held by the caller while the handler runs, so the handler may replace or remove its own type.
    RefPtr<Entry> find(StringView essence) const;
    bool is_empty() const { return m_entries.is_empty(); }

private:
    HashMap<String, NonnullRefPtr<Entry>> m_entries;
};

// The MIME essence of `mime` ("Text/Python; x=y" -> "text/python"), or nothing for a string that
// is not a MIME type or names JavaScript, which always runs as JavaScript.
Optional<String> script_type_essence(StringView mime);

// `view` runs <script type="mime"> with `handler` from its next prepared script on (a null handler
// removes it). Returns KR_ERROR_INVALID_ARGUMENT, nothing changed, for a NULL view or a `mime`
// script_type_essence refuses. Defined in Engine.cpp.
kr_result set_script_type_handler(kr_view*, StringView mime, ScriptTypeHandler);

// Called with the view's new top-level document each time one becomes active, before its parser
// runs a script; kept across loads, until replaced (a null handler removes it). The previous
// document is discarded from then on. Defined in Engine.cpp.
using ActiveDocumentHandler = Function<void(Web::DOM::Document&)>;
void set_active_document_handler(kr_view*, ActiveDocumentHandler);

// The view's active top-level document, or null (a closed page). Defined in Engine.cpp.
Web::DOM::Document* active_document(kr_view*);

}
