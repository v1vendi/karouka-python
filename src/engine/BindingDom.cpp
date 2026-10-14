/*
 * See BindingDom.h. Tessera has a counterpart of this file.
 */
#include "BindingDom.h"
#include "HostInterface.h"
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/DOM/Attr.h>
#include <AK/StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>

namespace Karouka {

DocumentBindings::DocumentBindings() = default;
DocumentBindings::~DocumentBindings()
{
    delete m_workspace;
}

static constexpr auto prefix = "data-bind-"sv;

void DocumentBindings::report(ByteString message)
{
    if (m_reported.set(message) == HashSetResult::InsertedNewEntry)
        Karouka::log(KR_LOG_WARN, "{}", message);
}

void DocumentBindings::scan(BindingModel& model, Web::DOM::Document& document)
{
    // The bindings of the last scan, by element, to carry over those whose markup is unchanged.
    auto previous = move(m_bindings);
    m_bindings.clear();
    HashMap<Web::DOM::Element const*, Vector<size_t, 4>> previous_by_element;
    for (size_t i = 0; i < previous.size(); ++i)
        previous_by_element.ensure(previous[i].element.ptr()).append(i);
    m_by_path.clear();

    document.for_each_in_inclusive_subtree_of_type<Web::DOM::Element>([&](Web::DOM::Element& element) {
        element.for_each_attribute([&](Web::DOM::Attr const& attribute) {
            auto name = MUST(attribute.local_name().view().to_byte_string());
            if (!name.starts_with(prefix))
                return;
            auto rest = name.view().substring_view(prefix.length());
            Binding binding;
            if (rest == "text"sv) {
                binding.target = Target::Text;
            } else if (rest == "value"sv) {
                binding.target = Target::Value;
            } else if (rest.starts_with("class-"sv) && rest.length() > 6) {
                binding.target = Target::Class;
                binding.name = Utf16FlyString::from_utf8(rest.substring_view(6));
            } else if (rest.starts_with("style-"sv) && rest.length() > 6) {
                binding.target = Target::Style;
                binding.name = Utf16FlyString::from_utf8(rest.substring_view(6));
            } else {
                report(ByteString::formatted("unknown binding attribute {} on <{}>", name, element.local_name()));
                return;
            }
            binding.source = attribute.value();

            Binding* kept = nullptr;
            if (auto it = previous_by_element.find(&element); it != previous_by_element.end()) {
                for (auto index : it->value) {
                    auto& candidate = previous[index];
                    if (candidate.element && candidate.target == binding.target && candidate.name == binding.name && candidate.source == binding.source) {
                        kept = &candidate;
                        break;
                    }
                }
            }
            if (kept) {
                binding.expression = move(kept->expression);
                binding.last = move(kept->last);
                kept->element = {}; // Taken; a duplicate attribute cannot match it twice.
            } else {
                auto source = binding.source.to_byte_string();
                if (auto result = binding.expression.compile(source.view(), model); result != KR_OK) {
                    report(ByteString::formatted("{} on <{}> disabled: error {} at offset {} in \"{}\"", name, element.local_name(), to_underlying(result), binding.expression.diagnostic().offset, source));
                    return;
                }
            }
            binding.element = GC::make_root(element);
            auto index = m_bindings.size();
            for (auto path : binding.expression.dependencies())
                m_by_path.ensure(path).append(index);
            m_bindings.append(move(binding));
        });
        return Web::TraversalDecision::Continue;
    });
    m_seen.clear();
    m_seen.resize(m_bindings.size());
    m_generation = 0;
}

void DocumentBindings::apply(BindingModel const& model, Binding& binding)
{
    if (!m_workspace)
        m_workspace = new BindingExpression::Workspace;
    BindingValue value;
    if (binding.expression.evaluate(model, *m_workspace, &value) != KR_OK)
        return; // An unset path or a type error leaves the authored target as it is.

    auto& element = *binding.element;
    if (binding.target == Target::Class) {
        if (value.kind != BindingModel::Kind::Bool)
            return;
        auto text = Utf16String::from_utf8(value.boolean ? "1"sv : "0"sv);
        if (binding.last.has_value() && *binding.last == text)
            return;
        binding.last = text;
        // DOMTokenList's update steps, by hand (its methods are not exported from LibWeb): the
        // class attribute becomes the ordered set of class names with <name> added or removed.
        if (element.has_class(binding.name) != value.boolean) {
            StringBuilder builder;
            for (auto const& class_name : element.class_names()) {
                if (class_name == binding.name)
                    continue;
                if (!builder.is_empty())
                    builder.append(' ');
                builder.append(MUST(class_name.view().to_utf8()));
            }
            if (value.boolean) {
                if (!builder.is_empty())
                    builder.append(' ');
                builder.append(MUST(binding.name.view().to_utf8()));
            }
            element.set_attribute_value(Utf16FlyString::from_utf8("class"sv), Utf16String::from_utf8(builder.string_view()));
        }
        ++m_applied;
        return;
    }

    char number_buffer[64];
    StringView utf8;
    if (value.kind == BindingModel::Kind::String)
        utf8 = value.string;
    else if (value.kind == BindingModel::Kind::Bool)
        utf8 = value.boolean ? "true"sv : "false"sv;
    else
        utf8 = StringView { number_buffer, format_binding_number(value.number, number_buffer, sizeof number_buffer) };
    auto text = Utf16String::from_utf8(utf8);
    if (binding.last.has_value() && *binding.last == text)
        return;

    switch (binding.target) {
    case Target::Text:
        (void)element.set_text_content(text);
        break;
    case Target::Style:
        (void)element.style()->set_property(binding.name, text.utf16_view(), u""sv);
        break;
    case Target::Value:
        if (auto* input = as_if<Web::HTML::HTMLInputElement>(element))
            (void)input->set_value(text.utf16_view());
        else if (auto* textarea = as_if<Web::HTML::HTMLTextAreaElement>(element))
            textarea->set_value(text.utf16_view());
        else
            return;
        break;
    case Target::Class:
        VERIFY_NOT_REACHED();
    }
    binding.last = move(text);
    ++m_applied;
}

void DocumentBindings::update(BindingModel& model, Web::DOM::Document* document)
{
    if (!document) {
        // Nothing to bind to yet; the next document applies the model's current values in full.
        (void)model.take_dirty();
        return;
    }

    auto readiness = document->readiness();
    if (document != m_document.ptr() || readiness != m_readiness || document->dom_tree_version() != m_tree_version) {
        if (document != m_document.ptr()) {
            m_bindings.clear(); // Nothing of another document carries over.
            m_reported.clear();
        }
        m_document = GC::make_root(*document);
        m_readiness = readiness;
        scan(model, *document);
        (void)model.take_dirty();
        for (auto& binding : m_bindings)
            apply(model, binding);
        m_tree_version = document->dom_tree_version();
        return;
    }

    if (!model.has_dirty())
        return;
    auto dirty = model.take_dirty();
    if (++m_generation == 0) {
        m_seen.fill(0);
        m_generation = 1;
    }
    // Evaluate each binding once per batch, in document order of first dependency hit.
    for (auto path : dirty) {
        auto it = m_by_path.find(path);
        if (it == m_by_path.end())
            continue;
        for (auto index : it->value) {
            if (m_seen[index] == m_generation)
                continue;
            m_seen[index] = m_generation;
            apply(model, m_bindings[index]);
        }
    }
    m_tree_version = document->dom_tree_version();
}

}
