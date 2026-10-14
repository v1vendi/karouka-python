/*
 * Page scripts and the DOM proxies (docs/PYTHON.md § Scripts, Bindings, Object lifetime).
 *
 * <script type="text/python"> runs in its document's own subinterpreter (own GIL), made at the
 * document's first Python script and ended when another document replaces it in the view
 * (Karouka::set_active_document_handler) or the view closes. Its __main__ holds window, document
 * and console; sys.path starts with the page's directory. Python runs only in file: documents
 * under the view's app root.
 *
 * A proxy is a Python object for one LibWeb cell, typed by its IDL interface and named with its
 * IDL members. It belongs to a context: one (document, interpreter) pair. The document's own
 * interpreter has one; View.document makes one for the app's interpreter. A context holds a GC::Root
 * per proxy and a weak cache from Cell* to proxy, so one cell has one proxy per interpreter.
 * Discarding a document drops every context of it: its proxies, in any interpreter, lose their
 * cells and raise StaleObjectError, and its own interpreter ends.
 *
 * Each proxy call does what the JS glue does: enter the realm's execution context, push and pop a
 * [CEReactions] element queue, turn a WebIDL::ExceptionOr error into a Python exception, and run a
 * microtask checkpoint when the outermost call returns to no script. A Python callable passed as a
 * listener becomes one JS::NativeFunction per (context, callable), wrapped in a
 * WebIDL::CallbackType and a DOM::IDLEventListener, so removeEventListener finds it again and JS
 * and Python listeners share the target's list. An uncaught exception in a script or a listener
 * logs its traceback through kr_host_log (category "python") and fires an ErrorEvent at the window.
 *
 * Everything here runs on the main thread, which drives the engine. The proxy types and their IDL
 * members are generated (src/python/gen/generate_python_bindings.py, conversions in bindings.h);
 * this file builds them per interpreter and adds the overrides (s_overrides): listeners, the
 * window's interface objects and camel-cased CSS properties.
 */
#include "context.h"

#include "../engine/HostInterface.h"
#include "../engine/ScriptTypes.h"

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Embedder/ScriptTypes.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactions.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

using namespace Web;

namespace Karouka::Python {

struct ViewHooks {
    kr_view* view { nullptr };
    ByteString app_root; // realpath, "" for none
};

static Vector<Context*> s_contexts;
int g_call_depth;
// discard() releasing a context's Python references, whose finalizers can run Python.
static int s_discarding;

// ---- Strings and errors ----

PyObject* to_py(Utf16View const& view)
{
    auto utf8 = view.to_utf8_but_should_be_ported_to_utf16();
    auto bytes = utf8.bytes_as_string_view();
    return PyUnicode_DecodeUTF8(bytes.characters_without_null_termination(), static_cast<Py_ssize_t>(bytes.length()), "replace");
}

static PyObject* to_py(Utf16String const& string) { return to_py(string.utf16_view()); }
static PyObject* to_py(Utf16FlyString const& string) { return to_py(string.view()); }

PyObject* to_py_utf8(StringView string)
{
    return PyUnicode_DecodeUTF8(string.characters_without_null_termination(), static_cast<Py_ssize_t>(string.length()), "replace");
}

// A Python str as UTF-8; nothing, with TypeError raised, for anything else.
Optional<StringView> utf8_of(PyObject* object, char const* what)
{
    if (!PyUnicode_Check(object)) {
        PyErr_Format(PyExc_TypeError, "%s must be str, not %.100s", what, Py_TYPE(object)->tp_name);
        return {};
    }
    Py_ssize_t length = 0;
    char const* characters = PyUnicode_AsUTF8AndSize(object, &length);
    if (!characters)
        return {};
    return StringView { characters, static_cast<size_t>(length) };
}

static Optional<Utf16String> to_utf16(PyObject* object, char const* what)
{
    auto utf8 = utf8_of(object, what);
    if (!utf8.has_value())
        return {};
    return Utf16String::from_utf8_without_validation(*utf8);
}

static Optional<Utf16FlyString> to_utf16_fly(PyObject* object, char const* what)
{
    auto utf8 = utf8_of(object, what);
    if (!utf8.has_value())
        return {};
    return Utf16FlyString::from_utf8(*utf8);
}

static void raise_exception(ModuleState& state, WebIDL::Exception const& exception)
{
    exception.visit(
        [](WebIDL::SimpleException const& simple) {
            PyObject* type = PyExc_RuntimeError;
            switch (simple.type) {
            case WebIDL::SimpleExceptionType::TypeError:
                type = PyExc_TypeError;
                break;
            case WebIDL::SimpleExceptionType::RangeError:
                type = PyExc_ValueError;
                break;
            case WebIDL::SimpleExceptionType::ReferenceError:
                type = PyExc_NameError;
                break;
            default:
                break;
            }
            PyObject* message = to_py(simple.message);
            if (message) {
                PyErr_SetObject(type, message);
                Py_DECREF(message);
            }
        },
        [&](GC::Ref<WebIDL::DOMException> const& dom_exception) {
            PyObject* message = to_py(dom_exception->message());
            PyObject* name = to_py(dom_exception->name());
            PyObject* instance = message && name ? PyObject_CallFunctionObjArgs(state.dom_exception, message, name, nullptr) : nullptr;
            if (instance && PyObject_SetAttrString(instance, "name", name) == 0)
                PyErr_SetObject(state.dom_exception, instance);
            Py_XDECREF(instance);
            Py_XDECREF(message);
            Py_XDECREF(name);
        },
        [](JS::Completion const& completion) {
            auto text = ByteString::formatted("JavaScript exception: {}", completion.value());
            PyErr_SetString(PyExc_RuntimeError, text.characters());
        });
}

void raise_exception(Context& context, WebIDL::Exception const& exception)
{
    raise_exception(*state_of(context.module), exception);
}

// ---- Proxies ----

ModuleState& state_of_context(Context& context) { return *state_of(context.module); }

JS::Realm& realm_of(Context& context) { return context.document->relevant_settings_object().realm(); }

PyTypeObject* dom_type(Context& context, DomType type) { return state_of_context(context).dom_types[static_cast<int>(type)]; }

// The generated type of each C++ class that implements an interface, by GC::Cell::class_name().
static HashMap<StringView, DomType>& class_types()
{
    static HashMap<StringView, DomType>* types;
    if (!types) {
        types = new HashMap<StringView, DomType>;
        for (int i = 0; i < static_cast<int>(DomType::Count); ++i)
            types->set(StringView { g_generated_types[i].implemented_name, strlen(g_generated_types[i].implemented_name) }, static_cast<DomType>(i));
    }
    return *types;
}

PyObject* wrap(Context& context, GC::Cell* cell, DomType static_type)
{
    if (!cell)
        Py_RETURN_NONE;
    if (auto it = context.proxies.find(cell); it != context.proxies.end())
        return Py_NewRef(reinterpret_cast<PyObject*>(it->value));
    PyTypeObject* python_type = dom_type(context, static_type);
    if (auto dynamic = class_types().get(cell->class_name()); dynamic.has_value()) {
        PyTypeObject* candidate = dom_type(context, *dynamic);
        if (PyType_IsSubtype(candidate, python_type))
            python_type = candidate;
    }
    auto* proxy = reinterpret_cast<Proxy*>(python_type->tp_alloc(python_type, 0));
    if (!proxy)
        return nullptr;
    proxy->cell = cell;
    proxy->root = new GC::Root<GC::Cell>(cell);
    proxy->context = &context;
    context.proxies.set(cell, proxy);
    return reinterpret_cast<PyObject*>(proxy);
}

Optional<DomType> dom_type_of_class(GC::Cell const& cell)
{
    return class_types().get(cell.class_name());
}

Context* current_document_context()
{
    PyThreadState* current = PyThreadState_GetUnchecked();
    if (!current)
        return nullptr;
    for (auto* context : s_contexts) {
        if (context->owns_interpreter && context->thread_state == current && !context->discard_pending)
            return context;
    }
    return nullptr;
}

Context* static_context(PyObject* type)
{
    if (!on_main_thread(reinterpret_cast<PyTypeObject*>(type)))
        return nullptr;
    if (Context* context = current_document_context())
        return context;
    // The app's interpreter: View.document's context, when there is exactly one.
    PyInterpreterState* interpreter = PyInterpreterState_Get();
    Context* found = nullptr;
    for (auto* context : s_contexts) {
        if (context->owns_interpreter || context->interpreter != interpreter || context->discard_pending)
            continue;
        if (found) {
            PyErr_Format(PyExc_TypeError, "%s: more than one document is open in this interpreter; call it from a page's Python",
                reinterpret_cast<PyTypeObject*>(type)->tp_name);
            return nullptr;
        }
        found = context;
    }
    if (!found)
        PyErr_Format(PyExc_TypeError, "%s: no document to create it in (open a view's document first)", reinterpret_cast<PyTypeObject*>(type)->tp_name);
    return found;
}

static PyObject* wrap_node(Context& context, DOM::Node* node) { return wrap(context, node, DomType::Node); }
static PyObject* wrap_target(Context& context, DOM::EventTarget* target) { return wrap(context, target, DomType::EventTarget); }
static PyObject* wrap_event(Context& context, DOM::Event* event) { return wrap(context, event, DomType::Event); }

Context* live_context(PyObject* self)
{
    if (!on_main_thread(Py_TYPE(self)))
        return nullptr;
    auto* proxy = reinterpret_cast<Proxy*>(self);
    if (!proxy->context) {
        if (ModuleState* state = state_of_type(Py_TYPE(self)))
            PyErr_SetString(state->stale_object_error, "the proxy's document was discarded");
        return nullptr;
    }
    return proxy->context;
}

void proxy_dealloc(PyObject* object)
{
    auto* proxy = reinterpret_cast<Proxy*>(object);
    PyTypeObject* type = Py_TYPE(object);
    if (proxy->context)
        proxy->context->proxies.remove(proxy->cell);
    // A root is only touched on the main thread; one dropped elsewhere leaks its cell.
    if (proxy->root && pthread_main_np())
        delete proxy->root;
    type->tp_free(object);
    Py_DECREF(type);
}

Call::Call(Context& context)
{
    m_execution_context.emplace(realm_of(context), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    ++g_call_depth;
    Bindings::main_thread_similar_origin_window_agent().custom_element_reactions_stack.element_queue_stack.append({});
}

Call::~Call()
{
    auto queue = Bindings::main_thread_similar_origin_window_agent().custom_element_reactions_stack.element_queue_stack.take_last();
    HTML::invoke_custom_element_reactions(queue);
    m_execution_context.clear();
    if (--g_call_depth == 0 && Bindings::main_thread_vm().execution_context_stack().is_empty())
        HTML::perform_a_microtask_checkpoint();
}

// Returns from a method on an ExceptionOr error, with the Python exception raised.
#define TRY_DOM(expression, failure)                                       \
    ({                                                                     \
        auto _result = (expression);                                       \
        if (_result.is_error()) {                                          \
            raise_exception(state_of_context(*context), _result.exception()); \
            return failure;                                                \
        }                                                                  \
        _result.release_value();                                           \
    })

#define TRY_DOM_VOID(expression, failure)                                  \
    do {                                                                   \
        auto _result = (expression);                                       \
        if (_result.is_error()) {                                          \
            raise_exception(state_of_context(*context), _result.exception()); \
            return failure;                                                \
        }                                                                  \
    } while (0)

// ---- Listeners and errors ----

void report_exception(Context& context)
{
    PyObject* exception = PyErr_GetRaisedException();
    if (!exception)
        return;
    ByteString traceback_text;
    ByteString message;
    PyObject* traceback = PyImport_ImportModule("traceback");
    PyObject* lines = traceback ? PyObject_CallMethod(traceback, "format_exception", "O", exception) : nullptr;
    PyObject* separator = PyUnicode_FromString("");
    PyObject* text = lines && separator ? PyUnicode_Join(separator, lines) : nullptr;
    if (char const* utf8 = text ? PyUnicode_AsUTF8(text) : nullptr)
        traceback_text = utf8;
    Py_XDECREF(text);
    Py_XDECREF(separator);
    Py_XDECREF(lines);
    Py_XDECREF(traceback);
    PyObject* string = PyObject_Str(exception);
    char const* utf8 = string ? PyUnicode_AsUTF8(string) : nullptr;
    message = ByteString::formatted("{}: {}", Py_TYPE(exception)->tp_name, utf8 ? utf8 : "?");
    Py_XDECREF(string);
    Py_DECREF(exception);
    PyErr_Clear();
    log_line(KR_LOG_ERROR, "python"sv, traceback_text.is_empty() ? message.view() : traceback_text.view().trim_whitespace(TrimMode::Right));

    auto window = context.document->window();
    if (!window)
        return;
    auto& realm = realm_of(context);
    HTML::TemporaryExecutionContext execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    window->report_an_exception(JS::Error::create(realm, Utf16String::from_utf8(message.view())));
}

// The one JS function standing for `callable` in `context` (made on first use).
static GC::Ref<JS::NativeFunction> function_for(Context& context, PyObject* callable)
{
    if (auto it = context.functions.find(callable); it != context.functions.end())
        return *it->value;
    auto& realm = realm_of(context);
    auto handle = context.handle;
    auto function = JS::NativeFunction::create(realm, "python"_utf16_fly_string, [handle, callable](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        Context* context = handle->context;
        if (!context)
            return JS::js_undefined();
        auto argument = vm.argument(0);
        // LibWeb hands listeners the event's JS wrapper; unwrap it to the DOM::Event.
        DOM::Event* event = Bindings::event_from_value(argument);
        {
            RunInContext run(*context);
            PyObject* python_event = wrap_event(*context, event);
            PyObject* result = python_event ? PyObject_CallOneArg(callable, python_event) : nullptr;
            Py_XDECREF(python_event);
            if (result)
                Py_DECREF(result);
            else
                report_exception(*context);
        }
        after_run(*context);
        return JS::js_undefined();
    });
    context.functions.set(Py_NewRef(callable), GC::make_root(function));
    return function;
}

static GC::Ref<DOM::IDLEventListener> listener_for(Context& context, PyObject* callable)
{
    auto& realm = realm_of(context);
    auto function = function_for(context, callable);
    auto callback = realm.create<WebIDL::CallbackType>(*function, realm);
    return DOM::IDLEventListener::create(callback);
}

// ---- Contexts ----

static Context* find_context(DOM::Document& document, PyInterpreterState* interpreter)
{
    for (auto* context : s_contexts) {
        if (context->document.ptr() == &document && context->interpreter == interpreter && !context->discard_pending)
            return context;
    }
    return nullptr;
}

static void discard(Context*);

void after_run(Context& context)
{
    if (context.running == 0 && context.discard_pending)
        discard(&context);
}

static void discard(Context* context)
{
    PyThreadState* current = PyThreadState_GetUnchecked();
    // Its Python is on the stack (or, for its own interpreter, current): wait until it returns.
    if (context->running > 0 || (context->owns_interpreter && current == context->thread_state)) {
        context->discard_pending = true;
        return;
    }
    ++s_discarding;
    s_contexts.remove_first_matching([context](auto* other) { return other == context; });
    context->handle->context = nullptr;
    for (auto& entry : context->proxies) {
        auto* proxy = entry.value;
        proxy->context = nullptr;
        proxy->cell = nullptr;
        delete proxy->root;
        proxy->root = nullptr;
    }
    context->proxies.clear();

    // Its Python references go under its own interpreter.
    PyThreadState* outer = nullptr;
    bool swapped = current != context->thread_state;
    if (swapped) {
        outer = current ? PyEval_SaveThread() : nullptr;
        PyEval_RestoreThread(context->thread_state);
    }
    for (auto& entry : context->functions)
        Py_DECREF(entry.key);
    context->functions.clear();
    js_discard(*context);
    Py_CLEAR(context->module);
    if (context->owns_interpreter) {
        // Leaves no thread state current.
        Py_EndInterpreter(context->thread_state);
        if (outer)
            PyEval_RestoreThread(outer);
    } else if (swapped) {
        PyEval_SaveThread();
        if (outer)
            PyEval_RestoreThread(outer);
    }
    delete context;
    --s_discarding;
}

static void discard_contexts(kr_view* view, DOM::Document* keep)
{
    Vector<Context*> doomed;
    for (auto* context : s_contexts) {
        if (context->view == view && context->document.ptr() != keep)
            doomed.append(context);
    }
    for (auto* context : doomed)
        discard(context);
}

static ByteString document_path(DOM::Document& document)
{
    auto const& url = document.url();
    if (url.scheme() != "file"sv)
        return {};
    return url.file_path();
}

static bool python_allowed(ViewHooks& hooks, DOM::Document& document)
{
    auto path = document_path(document);
    if (path.is_empty() || hooks.app_root.is_empty())
        return false;
    char resolved[PATH_MAX];
    if (!realpath(path.characters(), resolved))
        return false;
    StringView file { resolved, strlen(resolved) };
    auto root = hooks.app_root.view();
    if (root == "/"sv)
        return true;
    return file.starts_with(root) && file.length() > root.length() && file[root.length()] == '/';
}

static PyObject* new_console(ModuleState&);
static HashMap<StringView, DomType>& window_names();

// In the new interpreter (current): _karouka, sys.path and __main__'s globals.
static bool set_up_interpreter(Context& context)
{
    PyObject* types = PyImport_ImportModule("types");
    PyObject* spec = types ? PyObject_CallMethod(types, "SimpleNamespace", nullptr) : nullptr;
    Py_XDECREF(types);
    PyObject* name = PyUnicode_FromString("_karouka");
    bool named = spec && name && PyObject_SetAttrString(spec, "name", name) == 0;
    Py_XDECREF(name);
    if (!named) {
        Py_XDECREF(spec);
        return false;
    }
    PyObject* module = PyModule_FromDefAndSpec(definition(), spec);
    Py_DECREF(spec);
    if (!module || PyModule_ExecDef(module, definition()) < 0 || PyDict_SetItemString(PyImport_GetModuleDict(), "_karouka", module) < 0) {
        Py_XDECREF(module);
        return false;
    }
    context.module = module;

    LexicalPath page_path { document_path(*context.document) };
    auto directory = page_path.dirname();
    PyObject* sys_path = PySys_GetObject("path");
    PyObject* page_directory = to_py_utf8(directory);
    if (!sys_path || !page_directory || PyList_Insert(sys_path, 0, page_directory) < 0) {
        Py_XDECREF(page_directory);
        return false;
    }
    Py_DECREF(page_directory);

    PyObject* main = PyImport_AddModuleRef("__main__");
    if (!main)
        return false;
    PyObject* globals = PyModule_GetDict(main);
    PyObject* document = wrap_node(context, context.document.ptr());
    PyObject* window = wrap_target(context, context.document->window().ptr());
    PyObject* console = new_console(state_of_context(context));
    bool ok = document && window && console
        && PyDict_SetItemString(globals, "document", document) == 0
        && PyDict_SetItemString(globals, "window", window) == 0
        && PyDict_SetItemString(globals, "console", console) == 0;
    // The interface objects are globals, as they are in JS (window is JS's global object).
    for (auto const& entry : window_names()) {
        if (!ok)
            break;
        auto* type = reinterpret_cast<PyObject*>(state_of_context(context).dom_types[static_cast<int>(entry.value)]);
        ok = PyDict_SetItemString(globals, ByteString(entry.key).characters(), type) == 0;
    }
    ok = ok && js_set_up(context, globals);
    Py_XDECREF(document);
    Py_XDECREF(window);
    Py_XDECREF(console);
    Py_DECREF(main);
    return ok;
}

// The document's own context, its interpreter made now if it has none.
static Context* document_context(ViewHooks& hooks, DOM::Document& document)
{
    for (auto* context : s_contexts) {
        if (context->owns_interpreter && context->document.ptr() == &document)
            return context->discard_pending ? nullptr : context;
    }
    PyThreadState* outer = PyThreadState_GetUnchecked();
    if (!outer)
        return nullptr;
    PyInterpreterConfig config = {
        .use_main_obmalloc = 0,
        .allow_fork = 0,
        .allow_exec = 1,
        .allow_threads = 1,
        .allow_daemon_threads = 0,
        .check_multi_interp_extensions = 1,
        .gil = PyInterpreterConfig_OWN_GIL,
    };
    PyThreadState* thread_state = nullptr;
    PyStatus status = Py_NewInterpreterFromConfig(&thread_state, &config);
    if (PyStatus_Exception(status) || !thread_state) {
        if (PyThreadState_GetUnchecked() != outer)
            PyEval_RestoreThread(outer);
        log_line(KR_LOG_ERROR, "python"sv, "a document's subinterpreter could not be created"sv);
        return nullptr;
    }
    auto* context = new Context;
    context->view = hooks.view;
    context->document = GC::make_root(document);
    context->interpreter = PyThreadState_GetInterpreter(thread_state);
    context->thread_state = thread_state;
    context->owns_interpreter = true;
    s_contexts.append(context);
    bool ok = set_up_interpreter(*context);
    if (!ok) {
        PyErr_Print();
        log_line(KR_LOG_ERROR, "python"sv, "a document's subinterpreter could not be set up"sv);
    }
    // Back to the caller's interpreter.
    PyEval_SaveThread();
    PyEval_RestoreThread(outer);
    if (!ok) {
        discard(context);
        return nullptr;
    }
    return context;
}

static bool run_script(ViewHooks& hooks, HTML::HTMLScriptElement& element, Embedder::EmbedderScript const& script)
{
    auto& document = element.document();
    if (!python_allowed(hooks, document)) {
        log_line(KR_LOG_WARN, "python"sv, ByteString::formatted("text/python refused outside the app root: {}", document.url().serialize()));
        return false;
    }
    Context* context = document_context(hooks, document);
    if (!context)
        return false;
    {
        RunInContext run(*context);
        // Blank lines put the script's first line at its line in the file, for tracebacks.
        StringBuilder source;
        for (size_t line = 1; line < script.line(); ++line)
            source.append('\n');
        source.append(script.source_text().bytes_as_string_view());
        auto filename = script.url().scheme() == "file"sv ? script.url().file_path() : script.url().serialize().to_byte_string();
        auto source_text = source.to_byte_string();
        PyObject* code = Py_CompileString(source_text.characters(), filename.characters(), Py_file_input);
        PyObject* main = code ? PyImport_AddModuleRef("__main__") : nullptr;
        PyObject* result = main ? PyEval_EvalCode(code, PyModule_GetDict(main), PyModule_GetDict(main)) : nullptr;
        if (result)
            Py_DECREF(result);
        else
            report_exception(*context);
        Py_XDECREF(main);
        Py_XDECREF(code);
    }
    after_run(*context);
    return true;
}

// ---- Console ----

struct ConsoleObject {
    PyObject_HEAD
};

static PyObject* console_write(kr_log_level level, PyObject* args)
{
    PyObject* separator = PyUnicode_FromString(" ");
    PyObject* strings = PyList_New(0);
    for (Py_ssize_t i = 0; strings && separator && i < PyTuple_GET_SIZE(args); ++i) {
        PyObject* string = PyObject_Str(PyTuple_GET_ITEM(args, i));
        if (!string || PyList_Append(strings, string) < 0) {
            Py_XDECREF(string);
            Py_CLEAR(strings);
            break;
        }
        Py_DECREF(string);
    }
    PyObject* line = strings ? PyUnicode_Join(separator, strings) : nullptr;
    Py_XDECREF(strings);
    Py_XDECREF(separator);
    if (!line)
        return nullptr;
    if (char const* utf8 = PyUnicode_AsUTF8(line))
        log_line(level, "python"sv, StringView { utf8, strlen(utf8) });
    Py_DECREF(line);
    Py_RETURN_NONE;
}

static PyObject* console_log(PyObject*, PyObject* args) { return console_write(KR_LOG_INFO, args); }
static PyObject* console_debug(PyObject*, PyObject* args) { return console_write(KR_LOG_DEBUG, args); }
static PyObject* console_warn(PyObject*, PyObject* args) { return console_write(KR_LOG_WARN, args); }
static PyObject* console_error(PyObject*, PyObject* args) { return console_write(KR_LOG_ERROR, args); }

static PyMethodDef console_methods[] = {
    { "log", console_log, METH_VARARGS, PyDoc_STR("log(*values): one line at INFO through kr_host_log.") },
    { "info", console_log, METH_VARARGS, PyDoc_STR("info(*values): as log.") },
    { "debug", console_debug, METH_VARARGS, PyDoc_STR("debug(*values): one line at DEBUG.") },
    { "warn", console_warn, METH_VARARGS, PyDoc_STR("warn(*values): one line at WARN.") },
    { "error", console_error, METH_VARARGS, PyDoc_STR("error(*values): one line at ERROR.") },
    { nullptr, nullptr, 0, nullptr },
};

static void console_dealloc(PyObject* object)
{
    PyTypeObject* type = Py_TYPE(object);
    type->tp_free(object);
    Py_DECREF(type);
}

static PyType_Slot console_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(console_dealloc) },
    { Py_tp_methods, console_methods },
    { Py_tp_doc, const_cast<char*>(PyDoc_STR("A page's console: log, info, debug, warn and error write through kr_host_log.")) },
    { 0, nullptr },
};

static PyType_Spec console_spec = {
    "_karouka.Console", sizeof(ConsoleObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, console_slots
};

static PyObject* new_console(ModuleState& state)
{
    return state.console_type->tp_alloc(state.console_type, 0);
}

// ---- EventTarget ----

// addEventListener(type, callback, options=None): options is a bool (capture) or a dict with
// capture, once and passive.
static bool listener_options(PyObject* options, bool& capture, bool& once, Optional<bool>& passive)
{
    capture = false;
    once = false;
    if (!options || options == Py_None)
        return true;
    if (PyBool_Check(options)) {
        capture = options == Py_True;
        return true;
    }
    if (!PyDict_Check(options)) {
        PyErr_SetString(PyExc_TypeError, "options must be a bool or a dict");
        return false;
    }
    auto flag = [&](char const* name, auto&& set) {
        PyObject* value = PyDict_GetItemString(options, name);
        if (!value)
            return true;
        int truth = PyObject_IsTrue(value);
        if (truth < 0)
            return false;
        set(truth != 0);
        return true;
    };
    return flag("capture", [&](bool value) { capture = value; })
        && flag("once", [&](bool value) { once = value; })
        && flag("passive", [&](bool value) { passive = value; });
}

static PyObject* event_target_add_event_listener(PyObject* self, PyObject* args)
{
    PyObject* type_object = nullptr;
    PyObject* callback = nullptr;
    PyObject* options = nullptr;
    if (!PyArg_ParseTuple(args, "OO|O:addEventListener", &type_object, &callback, &options))
        return nullptr;
    auto type = to_utf16_fly(type_object, "type");
    if (!type.has_value())
        return nullptr;
    if (callback != Py_None && !PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "the listener must be callable or None");
        return nullptr;
    }
    DOM::EventTarget::AddEventListenerOptions listener_options_value;
    if (!listener_options(options, listener_options_value.capture, listener_options_value.once, listener_options_value.passive))
        return nullptr;
    BEGIN(DOM::EventTarget);
    if (callback == Py_None)
        Py_RETURN_NONE;
    impl->add_event_listener(*type, listener_for(*context, callback), listener_options_value);
    Py_RETURN_NONE;
}

static PyObject* event_target_remove_event_listener(PyObject* self, PyObject* args)
{
    PyObject* type_object = nullptr;
    PyObject* callback = nullptr;
    PyObject* options = nullptr;
    if (!PyArg_ParseTuple(args, "OO|O:removeEventListener", &type_object, &callback, &options))
        return nullptr;
    auto type = to_utf16_fly(type_object, "type");
    if (!type.has_value())
        return nullptr;
    bool capture = false;
    bool once = false;
    Optional<bool> passive;
    if (!listener_options(options, capture, once, passive))
        return nullptr;
    BEGIN(DOM::EventTarget);
    // A callable never added has no function, so nothing to remove.
    if (callback == Py_None || !context->functions.contains(callback))
        Py_RETURN_NONE;
    impl->remove_event_listener(*type, listener_for(*context, callback), DOM::EventTarget::EventListenerOptions { .capture = capture });
    Py_RETURN_NONE;
}

static PyMethodDef event_target_methods[] = {
    { "addEventListener", event_target_add_event_listener, METH_VARARGS, nullptr },
    { "removeEventListener", event_target_remove_event_listener, METH_VARARGS, nullptr },
    { nullptr, nullptr, 0, nullptr },
};

// ---- CSSStyleDeclaration ----

// style.backgroundColor: a CSS property by its camel-cased name (what IDL can't express).
static Optional<Utf16FlyString> css_property_of(PyObject* name)
{
    if (!PyUnicode_Check(name))
        return {};
    char const* utf8 = PyUnicode_AsUTF8(name);
    if (!utf8) {
        PyErr_Clear();
        return {};
    }
    auto id = CSS::property_id_from_camel_case_string(StringView { utf8, strlen(utf8) });
    if (!id.has_value())
        return {};
    return CSS::string_from_property_id(*id);
}

static PyObject* style_getattro(PyObject* self, PyObject* name)
{
    PyObject* value = PyObject_GenericGetAttr(self, name);
    if (value || !PyErr_ExceptionMatches(PyExc_AttributeError))
        return value;
    auto property = css_property_of(name);
    if (!property.has_value())
        return nullptr;
    PyErr_Clear();
    BEGIN(CSS::CSSStyleDeclaration);
    return to_py(impl->get_property_value(*property));
}

static int style_setattro(PyObject* self, PyObject* name, PyObject* value)
{
    auto property = css_property_of(name);
    if (!property.has_value())
        return PyObject_GenericSetAttr(self, name, value);
    Optional<Utf16String> text;
    if (value && !(text = to_utf16(value, "value")).has_value())
        return -1;
    Context* context = live_context(self);
    if (!context)
        return -1;
    auto* impl = cell_of<CSS::CSSStyleDeclaration>(self);
    Call call(*context);
    if (!value) {
        TRY_DOM_VOID(impl->remove_property(*property), -1);
        return 0;
    }
    TRY_DOM_VOID(impl->set_property(*property, text->utf16_view(), Utf16View {}), -1);
    return 0;
}

// ---- Window: interface objects ----

// What JS has on window as interface objects (window.HTMLElement), with [LegacyWindowAlias] and
// [LegacyFactoryFunction] names: the generated types, by name.
static HashMap<StringView, DomType>& window_names()
{
    static HashMap<StringView, DomType>* names;
    if (!names) {
        names = new HashMap<StringView, DomType>;
        for (int i = 0; i < static_cast<int>(DomType::Count); ++i) {
            if (g_generated_types[i].window_exposed)
                names->set(StringView { g_generated_types[i].name, strlen(g_generated_types[i].name) }, static_cast<DomType>(i));
        }
        for (auto const* alias = g_window_aliases; alias->name; ++alias)
            names->set(StringView { alias->name, strlen(alias->name) }, alias->type);
    }
    return *names;
}

static PyObject* window_getattro(PyObject* self, PyObject* name)
{
    PyObject* value = PyObject_GenericGetAttr(self, name);
    if (value || !PyErr_ExceptionMatches(PyExc_AttributeError) || !PyUnicode_Check(name))
        return value;
    char const* utf8 = PyUnicode_AsUTF8(name);
    Optional<DomType> type;
    if (utf8) {
        if (auto found = window_names().get(StringView { utf8, strlen(utf8) }); found.has_value())
            type = *found;
    }
    ModuleState* state = type.has_value() ? state_of_type(Py_TYPE(self)) : nullptr;
    if (!state)
        return nullptr;
    PyErr_Clear();
    return Py_NewRef(reinterpret_cast<PyObject*>(state->dom_types[static_cast<int>(*type)]));
}

static PyObject* window_dir(PyObject* self, PyObject*)
{
    PyObject* names = PyObject_CallMethod(reinterpret_cast<PyObject*>(&PyBaseObject_Type), "__dir__", "O", self);
    if (!names)
        return nullptr;
    for (auto const& entry : window_names()) {
        PyObject* name = to_py_utf8(entry.key);
        if (!name || PyList_Append(names, name) < 0) {
            Py_XDECREF(name);
            Py_DECREF(names);
            return nullptr;
        }
        Py_DECREF(name);
    }
    return names;
}

static PyMethodDef window_methods[] = {
    { "__dir__", window_dir, METH_NOARGS, nullptr },
    { "fetch", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(window_fetch)), METH_FASTCALL, nullptr },
    { nullptr, nullptr, 0, nullptr },
};

// ---- Types ----

// tp_new of a type with an IDL constructor: `Event("click", {"bubbles": True})` calls the
// generated constructor of the nearest generated type (a Python subclass gets its base's object).
static PyObject* proxy_new(PyTypeObject* type, PyObject* args, PyObject* kwargs)
{
    if (kwargs && PyDict_GET_SIZE(kwargs)) {
        PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", type->tp_name);
        return nullptr;
    }
    ModuleState* state = state_of_type(type);
    if (!state)
        return nullptr;
    for (PyTypeObject* candidate = type; candidate; candidate = candidate->tp_base) {
        for (int i = 0; i < static_cast<int>(DomType::Count); ++i) {
            if (state->dom_types[i] != candidate || !*g_generated_types[i].construct)
                continue;
            return (*g_generated_types[i].construct)(reinterpret_cast<PyObject*>(candidate), &PyTuple_GET_ITEM(args, 0), PyTuple_GET_SIZE(args));
        }
    }
    PyErr_Format(PyExc_TypeError, "%s has no constructor", type->tp_name);
    return nullptr;
}

// The hand-written overrides: members that replace or add to a generated type's by name, and slots.
// EventTarget's listeners take Python callables (the generator converts no callbacks yet); Window
// has the interface objects; CSSStyleDeclaration has camel-cased CSS properties (style.color).
struct Override {
    DomType type;
    PyMethodDef const* methods;
    getattrofunc getattro;
    setattrofunc setattro;
};

static Override const s_overrides[] = {
    { DomType::EventTarget, event_target_methods, nullptr, nullptr },
    { DomType::Window, window_methods, window_getattro, nullptr },
    { DomType::CSSStyleDeclaration, nullptr, style_getattro, style_setattro },
};

static unsigned constexpr proxy_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_BASETYPE;

// One spec per generated type, with the overrides merged in; built once, shared by every interpreter.
struct TypeSpec {
    PyType_Spec spec;
    Vector<PyType_Slot> slots;
    Vector<PyMethodDef> methods;
    ByteString doc;
};

static Vector<TypeSpec>& type_specs()
{
    static Vector<TypeSpec>* specs;
    if (specs)
        return *specs;
    specs = new Vector<TypeSpec>;
    specs->resize(static_cast<int>(DomType::Count));
    for (int i = 0; i < static_cast<int>(DomType::Count); ++i) {
        auto const& generated = g_generated_types[i];
        auto& spec = (*specs)[i];
        Override const* override_entry = nullptr;
        for (auto const& candidate : s_overrides) {
            if (static_cast<int>(candidate.type) == i)
                override_entry = &candidate;
        }
        auto overridden = [&](char const* name) {
            for (auto const* method = override_entry && override_entry->methods ? override_entry->methods : nullptr; method && method->ml_name; ++method) {
                if (!strcmp(method->ml_name, name))
                    return true;
            }
            return false;
        };
        for (auto const* method = generated.methods; method->ml_name; ++method) {
            if (!overridden(method->ml_name))
                spec.methods.append(*method);
        }
        for (auto const* method = override_entry && override_entry->methods ? override_entry->methods : nullptr; method && method->ml_name; ++method)
            spec.methods.append(*method);
        spec.methods.append({ nullptr, nullptr, 0, nullptr });
        spec.doc = ByteString::formatted("{}: a LibWeb object, by its IDL names (generated from LibWeb's IDL).", generated.name);
        spec.slots.append({ Py_tp_dealloc, reinterpret_cast<void*>(proxy_dealloc) });
        spec.slots.append({ Py_tp_doc, const_cast<char*>(spec.doc.characters()) });
        spec.slots.append({ Py_tp_methods, spec.methods.data() });
        if (generated.getset->name)
            spec.slots.append({ Py_tp_getset, generated.getset });
        if (generated.sequence->length) {
            spec.slots.append({ Py_sq_length, reinterpret_cast<void*>(generated.sequence->length) });
            spec.slots.append({ Py_sq_item, reinterpret_cast<void*>(generated.sequence->item) });
        }
        if (*generated.str)
            spec.slots.append({ Py_tp_str, reinterpret_cast<void*>(*generated.str) });
        if (*generated.construct)
            spec.slots.append({ Py_tp_new, reinterpret_cast<void*>(proxy_new) });
        if (*generated.subscript)
            spec.slots.append({ Py_mp_subscript, reinterpret_cast<void*>(*generated.subscript) });
        if (override_entry && override_entry->getattro)
            spec.slots.append({ Py_tp_getattro, reinterpret_cast<void*>(override_entry->getattro) });
        if (override_entry && override_entry->setattro)
            spec.slots.append({ Py_tp_setattro, reinterpret_cast<void*>(override_entry->setattro) });
        spec.slots.append({ 0, nullptr });
        unsigned flags = *generated.construct ? proxy_flags & ~Py_TPFLAGS_DISALLOW_INSTANTIATION : proxy_flags;
        spec.spec = { generated.python_name, sizeof(Proxy), 0, flags, spec.slots.data() };
    }
    return *specs;
}

// _karouka._bindings_report(): {"interfaces": n, "members": n, "unbound": [...], "caller_realm": [...]}:
// generated members whose C++ call didn't resolve at compile time (they raise NotImplementedError),
// and the bound [NeedsCallerRealm] ones (tests/python/test_caller_realm.py calls each).
static PyObject* bindings_report(PyObject*, PyObject*)
{
    PyObject* unbound_members = PyList_New(0);
    PyObject* caller_realm_members = PyList_New(0);
    if (!unbound_members || !caller_realm_members) {
        Py_XDECREF(unbound_members);
        Py_XDECREF(caller_realm_members);
        return nullptr;
    }
    long members = 0;
    for (int i = 0; i < static_cast<int>(DomType::Count); ++i) {
        for (auto const* member = g_generated_types[i].bound; member->name; ++member) {
            ++members;
            PyObject* list = !member->bound ? unbound_members : member->caller_realm ? caller_realm_members : nullptr;
            if (!list)
                continue;
            PyObject* name = PyUnicode_FromString(member->name);
            if (!name || PyList_Append(list, name) < 0) {
                Py_XDECREF(name);
                Py_DECREF(unbound_members);
                Py_DECREF(caller_realm_members);
                return nullptr;
            }
            Py_DECREF(name);
        }
    }
    return Py_BuildValue("{s:i,s:l,s:N,s:N}", "interfaces", static_cast<int>(DomType::Count), "members", members,
        "unbound", unbound_members, "caller_realm", caller_realm_members);
}

// At the main interpreter's exit, before Python finalizes the subinterpreters itself: every
// document's interpreter ends here, and every proxy goes stale.
static PyObject* discard_all(PyObject*, PyObject*)
{
    while (!s_contexts.is_empty()) {
        auto* context = s_contexts.last();
        if (context->running > 0 || context->discard_pending)
            break;
        discard(context);
    }
    Py_RETURN_NONE;
}

static PyMethodDef discard_all_method = { "_discard_all_documents", discard_all, METH_NOARGS, nullptr };

static PyMethodDef dom_functions[] = {
    { "_bindings_report", bindings_report, METH_NOARGS, nullptr },
    { nullptr, nullptr, 0, nullptr },
};

int dom_exec(PyObject* module, ModuleState& state)
{
    if (PyInterpreterState_Get() == PyInterpreterState_Main()) {
        PyObject* atexit = PyImport_ImportModule("atexit");
        PyObject* function = atexit ? PyCFunction_New(&discard_all_method, nullptr) : nullptr;
        PyObject* result = function ? PyObject_CallMethod(atexit, "register", "O", function) : nullptr;
        Py_XDECREF(result);
        Py_XDECREF(function);
        Py_XDECREF(atexit);
        if (!result)
            return -1;
    }
    if (PyModule_AddFunctions(module, dom_functions) < 0)
        return -1;
    auto& specs = type_specs();
    state.dom_types = static_cast<PyTypeObject**>(PyMem_Calloc(static_cast<size_t>(DomType::Count), sizeof(PyTypeObject*)));
    if (!state.dom_types)
        return -1;
    for (int i = 0; i < static_cast<int>(DomType::Count); ++i) {
        auto const& generated = g_generated_types[i];
        PyObject* base = generated.parent != DomType::Count ? reinterpret_cast<PyObject*>(state.dom_types[static_cast<int>(generated.parent)]) : nullptr;
        auto* type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &specs[i].spec, base));
        state.dom_types[i] = type;
        if (!type)
            return -1;
        if (generated.constants->name) {
            PyObject* dict = PyType_GetDict(type);
            for (auto const* constant = generated.constants; constant->name; ++constant) {
                PyObject* value = constant->is_bool ? PyBool_FromLong(constant->value) : PyLong_FromLongLong(constant->value);
                if (!value || PyDict_SetItemString(dict, constant->name, value) < 0) {
                    Py_XDECREF(value);
                    Py_DECREF(dict);
                    return -1;
                }
                Py_DECREF(value);
            }
            Py_DECREF(dict);
            PyType_Modified(type);
        }
        if (PyModule_AddType(module, type) < 0)
            return -1;
    }
    state.console_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &console_spec, nullptr));
    if (!state.console_type || PyModule_AddType(module, state.console_type) < 0)
        return -1;
    state.stale_object_error = PyErr_NewExceptionWithDoc("_karouka.StaleObjectError",
        "The proxy's document was discarded (a reload or another load replaced it, or the view closed).", nullptr, nullptr);
    if (!state.stale_object_error || PyModule_AddObjectRef(module, "StaleObjectError", state.stale_object_error) < 0)
        return -1;
    state.dom_exception = PyErr_NewExceptionWithDoc("_karouka.DOMException",
        "A DOMException: args are (message, name); name is also an attribute (e.g. 'HierarchyRequestError').", nullptr, nullptr);
    if (!state.dom_exception || PyModule_AddObjectRef(module, "DOMException", state.dom_exception) < 0)
        return -1;
    if (js_exec(module, state) < 0)
        return -1;
    return 0;
}

int dom_traverse(ModuleState& state, visitproc visit, void* arg)
{
    for (int i = 0; state.dom_types && i < static_cast<int>(DomType::Count); ++i)
        Py_VISIT(state.dom_types[i]);
    Py_VISIT(state.console_type);
    Py_VISIT(state.stale_object_error);
    Py_VISIT(state.dom_exception);
    js_traverse(state, visit, arg);
    return 0;
}

void dom_clear(ModuleState& state)
{
    for (int i = 0; state.dom_types && i < static_cast<int>(DomType::Count); ++i)
        Py_CLEAR(state.dom_types[i]);
    PyMem_Free(state.dom_types);
    state.dom_types = nullptr;
    Py_CLEAR(state.console_type);
    Py_CLEAR(state.stale_object_error);
    Py_CLEAR(state.dom_exception);
    js_clear(state);
}

// ---- Views ----

ViewHooks* dom_attach_view(kr_view* view)
{
    auto* hooks = new ViewHooks;
    hooks->view = view;
    Karouka::set_script_type_handler(view, "text/python"sv, [hooks](HTML::HTMLScriptElement& element, Embedder::EmbedderScript const& script) {
        return run_script(*hooks, element, script);
    });
    Karouka::set_active_document_handler(view, [view](DOM::Document& document) {
        discard_contexts(view, &document);
    });
    return hooks;
}

void dom_detach_view(ViewHooks* hooks)
{
    Karouka::set_script_type_handler(hooks->view, "text/python"sv, nullptr);
    Karouka::set_active_document_handler(hooks->view, nullptr);
    discard_contexts(hooks->view, nullptr);
    delete hooks;
}

static ByteString resolved_directory(char const* path)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return {};
    return ByteString { resolved, strlen(resolved) };
}

void dom_note_load_file(ViewHooks* hooks, char const* path)
{
    if (!hooks->app_root.is_empty())
        return;
    char resolved[PATH_MAX];
    if (realpath(path, resolved))
        hooks->app_root = LexicalPath { ByteString { resolved, strlen(resolved) } }.dirname();
}

PyObject* dom_app_root(ViewHooks* hooks)
{
    if (hooks->app_root.is_empty())
        Py_RETURN_NONE;
    return to_py_utf8(hooks->app_root.view());
}

int dom_set_app_root(ViewHooks* hooks, PyObject* value)
{
    if (!value || value == Py_None) {
        hooks->app_root = {};
        return 0;
    }
    PyObject* path = nullptr;
    if (!PyUnicode_FSConverter(value, &path))
        return -1;
    auto directory = resolved_directory(PyBytes_AS_STRING(path));
    Py_DECREF(path);
    if (directory.is_empty()) {
        PyErr_SetString(PyExc_ValueError, "app_root must be an existing directory");
        return -1;
    }
    hooks->app_root = directory;
    return 0;
}

PyObject* dom_view_document(PyObject* view_object, ViewHooks* hooks, bool window)
{
    auto* document = Karouka::active_document(hooks->view);
    if (!document)
        Py_RETURN_NONE;
    PyInterpreterState* interpreter = PyInterpreterState_Get();
    Context* context = find_context(*document, interpreter);
    if (!context) {
        PyObject* module = PyType_GetModuleByDef(Py_TYPE(view_object), definition());
        if (!module)
            return nullptr;
        context = new Context;
        context->view = hooks->view;
        context->document = GC::make_root(*document);
        context->interpreter = interpreter;
        context->thread_state = PyThreadState_Get();
        context->module = Py_NewRef(module);
        s_contexts.append(context);
    }
    if (window)
        return wrap_target(*context, document->window().ptr());
    return wrap_node(*context, document);
}

bool dom_busy()
{
    return g_call_depth > 0 || s_discarding > 0;
}

PyObject* dom_debug_counts(PyObject*, PyObject*)
{
    size_t interpreters = 0;
    size_t proxies = 0;
    size_t functions = 0;
    for (auto* context : s_contexts) {
        interpreters += context->owns_interpreter;
        proxies += context->proxies.size();
        functions += context->functions.size();
    }
    return Py_BuildValue("{snsnsnsn}", "contexts", static_cast<Py_ssize_t>(s_contexts.size()),
        "interpreters", static_cast<Py_ssize_t>(interpreters), "proxies", static_cast<Py_ssize_t>(proxies),
        "functions", static_cast<Py_ssize_t>(functions));
}

}
