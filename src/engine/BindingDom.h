/*
 * The page side of data binding: data-bind-* attributes on the view's document, kept in step with
 * the view's BindingModel by direct DOM mutation. No script runs.
 *
 * Tessera's markup surface (Tessera's data-binding spec § "Declarative authoring and
 * event direction"):
 *
 *     data-bind-text="expr"            the element's text content
 *     data-bind-class-<name>="expr"    toggles class <name>; expr must be bool
 *     data-bind-style-<property>="e"   sets the inline style property <property>
 *     data-bind-value="expr"           an <input>/<textarea> value, one way (editing writes nothing)
 *
 * Each expression (BindingExpression.h) compiles once when the document is scanned. A bad one
 * disables only its own attribute and logs a diagnostic with its source offset. A binding whose
 * evaluation fails (an unset path, a type mismatch) leaves the authored target as it is.
 *
 * update() runs once per kr_view_advance_time, before the rendering opportunity. A document that is
 * new, whose readiness changed, or whose tree changed since update() last left it (the parser or a
 * script inserted, removed or re-attributed nodes: Node::dom_tree_version moved past the version
 * our own writes left) is scanned and every binding evaluated once, which hands bindings values set
 * before they existed. A binding that survives a rescan (same element, attribute and source) keeps
 * its compiled expression and last written text, so a rescan rewrites only targets whose value
 * differs; a diagnostic is logged once per document, however often the markup is rescanned.
 * Otherwise only the bindings depending on the model's dirty paths are evaluated, through a
 * path -> bindings index: a frame with no tree change walks neither the document nor the binding
 * list. A page whose script mutates the tree every frame (an animated inline style, a title) gets a
 * walk of its elements each frame. A target already showing the evaluated text is not written again.
 */
#pragma once

#include "BindingExpression.h"
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DocumentReadyState.h>

namespace Karouka {

class DocumentBindings {
    AK_MAKE_NONCOPYABLE(DocumentBindings);
    AK_MAKE_NONMOVABLE(DocumentBindings);

public:
    DocumentBindings();
    ~DocumentBindings();

    void update(BindingModel&, Web::DOM::Document*);

    size_t binding_count() const { return m_bindings.size(); }
    size_t applied_count() const { return m_applied; }

private:
    enum class Target : u8 {
        Text,
        Class,
        Style,
        Value,
    };
    struct Binding {
        GC::Root<Web::DOM::Element> element;
        Target target { Target::Text };
        Utf16FlyString name;
        Utf16String source;
        BindingExpression expression;
        Optional<Utf16String> last;
    };

    void scan(BindingModel&, Web::DOM::Document&);
    void apply(BindingModel const&, Binding&);
    void report(ByteString);

    Vector<Binding> m_bindings;
    HashMap<kr_binding_path, Vector<size_t>> m_by_path;
    Vector<u32> m_seen;
    u32 m_generation { 0 };
    GC::Root<Web::DOM::Document> m_document;
    Optional<Web::HTML::DocumentReadyState> m_readiness;
    // m_document's dom_tree_version when update() last returned, after its own writes.
    u64 m_tree_version { 0 };
    // Diagnostics already logged for m_document.
    HashTable<ByteString> m_reported;
    // 72 KiB, allocated on first evaluation.
    BindingExpression::Workspace* m_workspace { nullptr };
    size_t m_applied { 0 };
};

}
