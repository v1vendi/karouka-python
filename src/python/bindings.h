/*
 * The C++ half of the Python bindings (docs/PYTHON.md § Bindings): what the generated glue
 * (src/python/gen/generate_python_bindings.py → PythonBindings*.cpp in the build) and the
 * hand-written overrides in dom.cpp share. The generator emits only member-specific code: which
 * C++ method to call, with which converted arguments. Converting values, entering the realm, the
 * [CEReactions] queue and mapping exceptions are here.
 *
 * Conversions follow the C++ type the JS glue uses for the IDL type (Ladybird's cpp_types.py):
 * convert() turns a Python object into it, to_py_value() turns a C++ result into Python. A C++
 * type neither knows raises TypeError (argument) or NotImplementedError (result) at run time, so
 * the glue for every interface compiles whatever its C++ looks like.
 */
#pragma once

#include "module.h"

#include <PythonBindings.h>

#include <AK/ByteString.h>
#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

#include <type_traits>

namespace Karouka::Python {

struct Context;

struct Proxy {
    PyObject_HEAD
    GC::Cell* cell;              // null once stale
    GC::Root<GC::Cell>* root;    // keeps cell alive while the proxy is
    Context* context;            // null once stale
};

// ---- dom.cpp ----

// The proxy's live context, or nullptr with StaleObjectError (or Error off the main thread) raised.
Context* live_context(PyObject* self);
JS::Realm& realm_of(Context&);
// The proxy of `cell` in `context`: its most derived generated type (by the cell's C++ class), or
// `static_type` when the class has no type of its own. None for null.
PyObject* wrap(Context&, GC::Cell* cell, DomType static_type);
PyTypeObject* dom_type(Context&, DomType);
void raise_exception(Context&, Web::WebIDL::Exception const&);
PyObject* to_py(Utf16View const&);
PyObject* to_py_utf8(StringView);
// The context a static member or constructor of `type` (a proxy type) runs in: the current
// document interpreter's, or, in the app's interpreter, its one open document's. Nullptr with
// TypeError raised when there is none or more than one (or Error off the main thread).
Context* static_context(PyObject* type);
// A Python str as UTF-8; nothing, with TypeError raised, for anything else.
Optional<StringView> utf8_of(PyObject*, char const* what);

// ---- js.cpp ----

// A JS value in Python: None (undefined, null), bool, int (an integral Number up to 2^53, a
// BigInt), float, str, a DOM object's proxy, the Python callable a JS function stands for, or a
// JSValue proxy. Nullptr with an exception raised.
PyObject* to_py_js(Context&, JS::Value);
// A Python value in JS: None is null, a coroutine or other awaitable a Promise, a callable a
// function, an exception an Error, dict and list/tuple new objects and arrays. Nothing, with
// TypeError raised, for what has no JS form.
Optional<JS::Value> to_js(Context&, PyObject*, char const* where);
// `object` as a JS function, for an IDL callback (a callable, or a JSValue function).
GC::Ptr<Web::WebIDL::CallbackType> callback_of(Context&, PyObject* object, char const* where);
// Raises _karouka.JSError for a value JS threw (a JS::ThrowCompletionOr's error); returns nullptr.
PyObject* raise_js_error(Context&, JS::Value thrown);

// The proxy's cell as its C++ class; nullptr for a class that isn't a GC cell (whose members the
// generated glue never calls).
template<typename T>
T* cell_of(PyObject* self)
{
    if constexpr (std::is_base_of_v<GC::Cell, T>)
        return static_cast<T*>(reinterpret_cast<Proxy*>(self)->cell);
    else
        return nullptr;
}

// One proxy call, as the JS glue makes it: the realm's execution context and a [CEReactions]
// element queue for its duration, then a microtask checkpoint if it was the outermost call and
// no script is running ("clean up after running script").
class Call {
public:
    explicit Call(Context&);
    ~Call();

private:
    Optional<Web::HTML::TemporaryExecutionContext> m_execution_context;
};

// Starts a proxy method or accessor: `impl` is the cell, `context` its context, `call` the scope.
#define BEGIN(T)                                    \
    Context* context = live_context(self);          \
    if (!context)                                   \
        return nullptr;                             \
    [[maybe_unused]] auto* impl = cell_of<T>(self); \
    Call call(*context)

// Starts a static member or constructor: `self` is the type, `context` static_context's.
#define BEGIN_STATIC()                       \
    Context* context = static_context(self); \
    if (!context)                            \
        return nullptr;                      \
    Call call(*context)

#define BEGIN_SETTER(T)                                                     \
    if (!value) {                                                           \
        PyErr_SetString(PyExc_TypeError, "the attribute can't be deleted"); \
        return -1;                                                          \
    }                                                                       \
    Context* context = live_context(self);                                  \
    if (!context)                                                           \
        return -1;                                                          \
    [[maybe_unused]] auto* impl = cell_of<T>(self);                         \
    Call call(*context)

// ---- The generated tables (PythonBindingsTable.cpp) ----

struct ConstantDefinition {
    char const* name;
    long long value;
    bool is_bool;
};

// Whether a member's C++ call resolved at compile time, by "Interface.member" (operations
// "Interface.name/argument count"). _karouka._bindings_report() lists the ones that did not, and
// the [NeedsCallerRealm] ones that did (`caller_realm`).
struct BoundMember {
    char const* name;
    bool bound;
    bool caller_realm;
};

struct SequenceFunctions {
    lenfunc length;
    ssizeargfunc item;
};

struct GeneratedType {
    char const* python_name;      // "_karouka.Node"
    char const* name;             // the IDL name
    char const* implemented_name; // the C++ class, as GC::Cell::class_name() gives it
    DomType parent;               // DomType::Count for none
    bool window_exposed;          // an interface object on window in JS
    PyGetSetDef* getset;
    PyMethodDef* methods;
    ConstantDefinition const* constants;
    BoundMember const* bound;
    SequenceFunctions const* sequence;
    reprfunc const* str;
    PyCFunctionFast const* construct; // the IDL constructor (self is the type); null for none
    binaryfunc const* subscript;      // obj[name]: the named getter; null for none
};

struct WindowAlias {
    char const* name;
    DomType type;
};

extern GeneratedType const g_generated_types[];
extern WindowAlias const g_window_aliases[];

// ---- Errors the glue raises ----

inline PyObject* unbound(char const* where)
{
    PyErr_Format(PyExc_NotImplementedError, "%s: karouka's Python bindings don't reach its C++ yet", where);
    return nullptr;
}

inline int unbound_setter(char const* where)
{
    unbound(where);
    return -1;
}

inline PyObject* wrong_argument_count(char const* where, Py_ssize_t count, int low, int high)
{
    if (high >= 10000)
        PyErr_Format(PyExc_TypeError, "%s takes at least %d arguments (%zd given)", where, low, count);
    else
        PyErr_Format(PyExc_TypeError, "%s takes %d to %d arguments (%zd given)", where, low, high, count);
    return nullptr;
}

// ---- Type traits ----

template<typename T>
struct Traits {
    static constexpr bool is_optional = false;
    static constexpr bool is_vector = false;
    static constexpr bool is_variant = false;
    static constexpr bool is_gc_pointer = false;
    static constexpr bool is_exception_or = false;
    static constexpr bool is_throw_completion_or = false;
};
template<typename T>
struct Traits<Optional<T>> : Traits<void> {
    static constexpr bool is_optional = true;
    using Inner = T;
};
template<typename T, size_t N>
struct Traits<Vector<T, N>> : Traits<void> {
    static constexpr bool is_vector = true;
    using Inner = T;
};
// sequence<object> and variadic `any...` (a transfer list, TrustedTypePolicy.createHTML's arguments).
template<typename T, size_t N>
struct Traits<GC::RootVector<T, N>> : Traits<void> {
    static constexpr bool is_vector = true;
    using Inner = T;
};
// sequence<BlobPart> (Blob's constructor).
template<typename T, size_t N>
struct Traits<GC::ConservativeVector<T, N>> : Traits<void> {
    static constexpr bool is_vector = true;
    using Inner = T;
};
template<typename... Ts>
struct Traits<Variant<Ts...>> : Traits<void> {
    static constexpr bool is_variant = true;
};
template<typename T>
struct Traits<GC::Ref<T>> : Traits<void> {
    static constexpr bool is_gc_pointer = true;
    static constexpr bool nullable = false;
    using Inner = T;
};
template<typename T>
struct Traits<GC::Ptr<T>> : Traits<void> {
    static constexpr bool is_gc_pointer = true;
    static constexpr bool nullable = true;
    using Inner = T;
};
template<typename T>
struct Traits<GC::Root<T>> : Traits<void> {
    static constexpr bool is_gc_pointer = true;
    static constexpr bool nullable = true;
    using Inner = T;
};
template<typename T>
struct Traits<Web::WebIDL::ExceptionOr<T>> : Traits<void> {
    static constexpr bool is_exception_or = true;
    using Inner = T;
};
// What [ImplementedInBindings] glue such as CustomElementRegistry.define returns: a JS throw.
template<typename T>
struct Traits<JS::ThrowCompletionOr<T>> : Traits<void> {
    static constexpr bool is_throw_completion_or = true;
    using Inner = T;
};

// What a GC::Ref, Ptr or Root points to; void for anything else.
template<typename T>
struct GcInnerOf {
    using Type = void;
};
template<typename T>
struct GcInnerOf<GC::Ref<T>> {
    using Type = std::remove_cv_t<T>;
};
template<typename T>
struct GcInnerOf<GC::Ptr<T>> {
    using Type = std::remove_cv_t<T>;
};
template<typename T>
struct GcInnerOf<GC::Root<T>> {
    using Type = std::remove_cv_t<T>;
};
template<typename T>
using GcInner = typename GcInnerOf<T>::Type;

template<typename T>
concept HasDomType = requires { DomTypeOf<std::remove_cv_t<T>>::value; } && std::is_base_of_v<GC::Cell, std::remove_cv_t<T>>;

template<typename T>
concept Utf16Like = std::is_same_v<T, Utf16String> || std::is_same_v<T, Utf16FlyString> || std::is_same_v<T, Utf16View>;

template<typename T>
concept Utf8Like = std::is_same_v<T, String> || std::is_same_v<T, FlyString> || std::is_same_v<T, ByteString> || std::is_same_v<T, StringView>;

// ---- Python → C++ ----

template<typename T>
bool convert(Context&, PyObject*, Optional<T>& out, char const* where);

// Whether `object` is the kind of Python value a union alternative of type T takes.
template<typename T>
bool accepts(Context& context, PyObject* object)
{
    if constexpr (Utf16Like<T> || Utf8Like<T>)
        return PyUnicode_Check(object);
    else if constexpr (std::is_same_v<T, bool>)
        return PyBool_Check(object);
    else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>)
        return (PyLong_Check(object) || PyFloat_Check(object)) && !PyBool_Check(object);
    else if constexpr (std::is_enum_v<T>)
        return PyUnicode_Check(object);
    else if constexpr (Traits<T>::is_optional)
        return object == Py_None || accepts<typename Traits<T>::Inner>(context, object);
    else if constexpr (Traits<T>::is_vector)
        return PyList_Check(object) || PyTuple_Check(object);
    else if constexpr (Traits<T>::is_gc_pointer) {
        if constexpr (HasDomType<typename Traits<T>::Inner>)
            return PyObject_TypeCheck(object, dom_type(context, DomTypeOf<std::remove_cv_t<typename Traits<T>::Inner>>::value));
        else
            return false;
    } else if constexpr (std::is_same_v<T, Empty>)
        return object == Py_None;
    else if constexpr (requires(JS::VM& vm) { karouka_dictionary_from_js(vm, JS::js_undefined(), std::type_identity<T> {}); })
        return PyDict_Check(object);
    else
        return false;
}

// Whether `object` could be an argument of type T, for choosing between overloads that take the
// same number of arguments (postMessage(message, transfer) or (message, options)): its Python kind,
// as accepts() tells it; a JS value, JS object or callback takes anything.
template<typename... Ts>
bool plausible_variant(Context&, PyObject*, Variant<Ts...>*);

template<typename T>
bool plausible(Context& context, PyObject* object)
{
    if constexpr (std::is_same_v<T, JS::Value> || std::is_same_v<GcInner<T>, Web::WebIDL::CallbackType> || std::is_same_v<GcInner<T>, Web::WebIDL::Promise>)
        return true;
    else if constexpr (std::is_base_of_v<JS::Object, GcInner<T>> && !HasDomType<GcInner<T>>)
        return true;
    else if constexpr (Traits<T>::is_optional)
        return object == Py_None || plausible<typename Traits<T>::Inner>(context, object);
    else if constexpr (Traits<T>::is_variant)
        return plausible_variant(context, object, static_cast<T*>(nullptr));
    else
        return accepts<T>(context, object);
}

template<typename... Ts>
bool plausible_variant(Context& context, PyObject* object, Variant<Ts...>*)
{
    return (plausible<Ts>(context, object) || ...);
}

template<typename V, typename... Ts>
bool convert_variant(Context& context, PyObject* object, Optional<V>& out, char const* where, Variant<Ts...>*)
{
    bool done = false;
    bool failed = false;
    auto attempt = [&]<typename A>() {
        if (done || failed || !accepts<A>(context, object))
            return;
        Optional<A> value;
        if (!convert(context, object, value, where)) {
            failed = true;
            return;
        }
        out = V { value.release_value() };
        done = true;
    };
    (attempt.template operator()<Ts>(), ...);
    if (!done && !failed)
        PyErr_Format(PyExc_TypeError, "%s: %.100s is none of the union's types", where, Py_TYPE(object)->tp_name);
    return done;
}

template<typename T>
bool convert(Context& context, PyObject* object, Optional<T>& out, char const* where)
{
    if constexpr (std::is_same_v<T, JS::Value>) {
        auto value = to_js(context, object, where);
        if (!value.has_value())
            return false;
        out = *value;
        return true;
    } else if constexpr (requires(JS::VM& vm) { { karouka_dictionary_from_js(vm, JS::js_undefined(), std::type_identity<T> {}) } -> std::same_as<JS::ThrowCompletionOr<T>>; }) {
        // A dictionary: LibWeb's converter (defaults, required members, nested types) of the JS
        // object the dict becomes; None is undefined, every member's default.
        JS::Value value = JS::js_undefined();
        if (object != Py_None) {
            auto converted = to_js(context, object, where);
            if (!converted.has_value())
                return false;
            value = *converted;
        }
        auto result = karouka_dictionary_from_js(realm_of(context).vm(), value, std::type_identity<T> {});
        if (result.is_error()) {
            raise_js_error(context, result.error_value());
            return false;
        }
        out = result.release_value();
        return true;
    } else if constexpr (std::is_same_v<GcInner<T>, Web::WebIDL::CallbackType>) {
        if constexpr (Traits<T>::nullable) {
            if (object == Py_None) {
                out = T {};
                return true;
            }
        }
        auto callback = callback_of(context, object, where);
        if (!callback)
            return false;
        out = T { *callback };
        return true;
    } else if constexpr (std::is_same_v<GcInner<T>, Web::WebIDL::Promise>) {
        // A Promise argument: any value, resolved as JS would (an awaitable becomes a Promise first).
        auto value = to_js(context, object, where);
        if (!value.has_value())
            return false;
        out = T { Web::WebIDL::create_resolved_promise(realm_of(context), *value) };
        return true;
    } else if constexpr (std::is_base_of_v<JS::Object, GcInner<T>> && !HasDomType<GcInner<T>>) {
        using Inner = std::remove_cv_t<typename Traits<T>::Inner>;
        if constexpr (Traits<T>::nullable) {
            if (object == Py_None) {
                out = T {};
                return true;
            }
        }
        auto value = to_js(context, object, where);
        if (!value.has_value())
            return false;
        bool matches = value->is_object();
        if constexpr (!std::is_same_v<Inner, JS::Object>)
            matches = matches && is<Inner>(value->as_object());
        if (!matches) {
            PyErr_Format(PyExc_TypeError, "%s: expected a JS object of the argument's type, not %.100s", where, Py_TYPE(object)->tp_name);
            return false;
        }
        out = T { static_cast<Inner&>(value->as_object()) };
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        int truth = PyObject_IsTrue(object);
        if (truth < 0)
            return false;
        out = truth != 0;
        return true;
    } else if constexpr (std::is_enum_v<T>) {
        auto utf8 = utf8_of(object, where);
        if (!utf8.has_value())
            return false;
        if constexpr (requires(T& value) { karouka_enum_from_string(std::declval<Utf16View const&>(), value); }) {
            auto string = Utf16String::from_utf8_without_validation(*utf8);
            T value {};
            if (!karouka_enum_from_string(string.utf16_view(), value)) {
                PyErr_Format(PyExc_ValueError, "%s: '%s' is not one of the enumeration's values", where, PyUnicode_AsUTF8(object));
                return false;
            }
            out = value;
            return true;
        } else {
            PyErr_Format(PyExc_TypeError, "%s: this enumeration is not converted from Python yet", where);
            return false;
        }
    } else if constexpr (std::is_integral_v<T>) {
        if (PyFloat_Check(object)) {
            PyErr_Format(PyExc_TypeError, "%s must be int, not float", where);
            return false;
        }
        PyObject* index = PyNumber_Index(object);
        if (!index)
            return false;
        // Modular, as WebIDL's ConvertToInt without [EnforceRange].
        unsigned long long bits = PyLong_AsUnsignedLongLongMask(index);
        Py_DECREF(index);
        if (bits == static_cast<unsigned long long>(-1) && PyErr_Occurred())
            return false;
        out = static_cast<T>(bits);
        return true;
    } else if constexpr (std::is_floating_point_v<T>) {
        double value = PyFloat_AsDouble(object);
        if (value == -1.0 && PyErr_Occurred())
            return false;
        out = static_cast<T>(value);
        return true;
    } else if constexpr (std::is_same_v<T, Utf16String>) {
        auto utf8 = utf8_of(object, where);
        if (!utf8.has_value())
            return false;
        out = Utf16String::from_utf8_without_validation(*utf8);
        return true;
    } else if constexpr (std::is_same_v<T, Utf16FlyString>) {
        auto utf8 = utf8_of(object, where);
        if (!utf8.has_value())
            return false;
        out = Utf16FlyString::from_utf8(*utf8);
        return true;
    } else if constexpr (std::is_same_v<T, String> || std::is_same_v<T, FlyString> || std::is_same_v<T, ByteString>) {
        auto utf8 = utf8_of(object, where);
        if (!utf8.has_value())
            return false;
        if constexpr (std::is_same_v<T, ByteString>)
            out = ByteString(*utf8);
        else
            out = T::from_utf8_without_validation(utf8->bytes());
        return true;
    } else if constexpr (Traits<T>::is_optional) {
        if (object == Py_None) {
            out = T {};
            return true;
        }
        Optional<typename Traits<T>::Inner> inner;
        if (!convert(context, object, inner, where))
            return false;
        out = T { inner.release_value() };
        return true;
    } else if constexpr (Traits<T>::is_gc_pointer) {
        using Inner = std::remove_cv_t<typename Traits<T>::Inner>;
        if constexpr (Traits<T>::nullable) {
            if (object == Py_None) {
                out = T {};
                return true;
            }
        }
        if constexpr (HasDomType<Inner>) {
            PyTypeObject* type = dom_type(context, DomTypeOf<Inner>::value);
            if (!PyObject_TypeCheck(object, type)) {
                PyErr_Format(PyExc_TypeError, "%s: expected %s%s, not %.100s", where, type->tp_name, Traits<T>::nullable ? " or None" : "", Py_TYPE(object)->tp_name);
                return false;
            }
            if (!live_context(object))
                return false;
            if constexpr (Traits<T>::nullable)
                out = T { cell_of<Inner>(object) };
            else
                out = T { *cell_of<Inner>(object) };
            return true;
        } else {
            PyErr_Format(PyExc_TypeError, "%s: this type is not converted from Python yet", where);
            return false;
        }
    } else if constexpr (Traits<T>::is_vector) {
        PyObject* sequence = PySequence_Fast(object, where);
        if (!sequence)
            return false;
        T result;
        Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
        for (Py_ssize_t i = 0; i < count; ++i) {
            Optional<typename Traits<T>::Inner> element;
            if (!convert(context, PySequence_Fast_GET_ITEM(sequence, i), element, where)) {
                Py_DECREF(sequence);
                return false;
            }
            result.append(element.release_value());
        }
        Py_DECREF(sequence);
        out = move(result);
        return true;
    } else if constexpr (Traits<T>::is_variant) {
        return convert_variant(context, object, out, where, static_cast<T*>(nullptr));
    } else {
        PyErr_Format(PyExc_TypeError, "%s: this type is not converted from Python yet", where);
        return false;
    }
}

// ---- C++ → Python ----

template<typename T>
PyObject* to_py_value(Context& context, T const& value)
{
    if constexpr (std::is_same_v<T, JS::Value>) {
        return to_py_js(context, value);
    } else if constexpr (std::is_same_v<T, Web::WebIDL::CallbackType>) {
        return to_py_js(context, JS::Value(value.callback.ptr()));
    } else if constexpr (std::is_same_v<T, Web::WebIDL::Promise>) {
        // A JSValue proxy of the promise: awaitable.
        return to_py_js(context, JS::Value(value.promise().ptr()));
    } else if constexpr (std::is_base_of_v<JS::Object, T> && !HasDomType<T>) {
        return to_py_js(context, JS::Value(&value));
    } else if constexpr (std::is_same_v<T, bool>) {
        return PyBool_FromLong(value);
    } else if constexpr (std::is_enum_v<T>) {
        if constexpr (requires { idl_enum_to_string(value); }) {
            auto string = idl_enum_to_string(value);
            return to_py(string.utf16_view());
        }
        else
            return unbound("an enumeration value");
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_unsigned_v<T>)
            return PyLong_FromUnsignedLongLong(value);
        else
            return PyLong_FromLongLong(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return PyFloat_FromDouble(value);
    } else if constexpr (std::is_same_v<T, Utf16String>) {
        return to_py(value.utf16_view());
    } else if constexpr (std::is_same_v<T, Utf16FlyString>) {
        return to_py(value.view());
    } else if constexpr (std::is_same_v<T, Utf16View>) {
        return to_py(value);
    } else if constexpr (std::is_same_v<T, String> || std::is_same_v<T, FlyString>) {
        return to_py_utf8(value.bytes_as_string_view());
    } else if constexpr (std::is_same_v<T, ByteString>) {
        return to_py_utf8(value.view());
    } else if constexpr (std::is_same_v<T, StringView>) {
        return to_py_utf8(value);
    } else if constexpr (std::is_same_v<T, Empty>) {
        Py_RETURN_NONE;
    } else if constexpr (Traits<T>::is_optional) {
        if (!value.has_value())
            Py_RETURN_NONE;
        return to_py_value(context, *value);
    } else if constexpr (std::is_pointer_v<T>) {
        using Inner = std::remove_cv_t<std::remove_pointer_t<T>>;
        if (!value)
            Py_RETURN_NONE;
        return to_py_value(context, static_cast<Inner const&>(*value));
    } else if constexpr (Traits<T>::is_gc_pointer) {
        using Inner = std::remove_cv_t<typename Traits<T>::Inner>;
        if constexpr (Traits<T>::nullable) {
            if (!value)
                Py_RETURN_NONE;
        }
        return to_py_value(context, static_cast<Inner const&>(*value));
    } else if constexpr (std::is_same_v<T, Web::HTML::WindowProxy>) {
        // A WindowProxy is its window, in Python.
        return wrap(context, const_cast<Web::HTML::Window*>(value.window().ptr()), DomType::Window);
    } else if constexpr (HasDomType<T>) {
        return wrap(context, const_cast<T*>(&value), DomTypeOf<T>::value);
    } else if constexpr (Traits<T>::is_variant) {
        return value.visit([&](auto const& alternative) { return to_py_value(context, alternative); });
    } else if constexpr (requires { value.begin(); value.end(); value.size(); }) {
        PyObject* list = PyList_New(0);
        if (!list)
            return nullptr;
        for (auto const& element : value) {
            PyObject* item = to_py_value(context, element);
            if (!item || PyList_Append(list, item) < 0) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    } else {
        return unbound("a value of this C++ type");
    }
}

// Calls `call` (a member's C++ call) and converts what it returns, raising its WebIDL exception.
template<typename F>
PyObject* result_to_py(Context& context, F&& call)
{
    using R = decltype(call());
    if constexpr (std::is_void_v<R>) {
        call();
        Py_RETURN_NONE;
    } else {
        decltype(auto) result = call();
        using D = std::remove_cvref_t<R>;
        if constexpr (Traits<D>::is_exception_or) {
            if (result.is_error()) {
                raise_exception(context, result.exception());
                return nullptr;
            }
            if constexpr (std::is_void_v<typename Traits<D>::Inner>)
                Py_RETURN_NONE;
            else {
                auto value = result.release_value();
                return to_py_value(context, value);
            }
        } else if constexpr (Traits<D>::is_throw_completion_or) {
            if (result.is_error())
                return raise_js_error(context, result.error_value());
            if constexpr (std::is_void_v<typename Traits<D>::Inner>)
                Py_RETURN_NONE;
            else {
                auto value = result.release_value();
                return to_py_value(context, value);
            }
        } else {
            return to_py_value(context, result);
        }
    }
}

// A [CreatesPromise] operation: its C++ settles `promise`, which the call returns (a JSValue to
// await); what the C++ throws rejects it, as the JS glue does.
template<typename F>
PyObject* promise_result(Context& context, JS::Realm& realm, GC::Ref<Web::WebIDL::Promise> promise, F&& call)
{
    auto completion = Web::WebIDL::throw_dom_exception_if_needed(realm.vm(), realm, [&]() -> decltype(auto) { return call(); });
    if (completion.is_error())
        Web::WebIDL::reject_promise(realm, *promise, completion.error_value());
    return to_py_js(context, JS::Value(promise->promise().ptr()));
}

// obj[key] for a key that isn't a str: the indexed getter (sequence item), if any.
inline PyObject* sequence_subscript(PyObject* self, PyObject* key)
{
    if (!Py_TYPE(self)->tp_as_sequence || !Py_TYPE(self)->tp_as_sequence->sq_item) {
        PyErr_Format(PyExc_TypeError, "%.100s indices must be str", Py_TYPE(self)->tp_name);
        return nullptr;
    }
    Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
    if (index == -1 && PyErr_Occurred())
        return nullptr;
    return PySequence_GetItem(self, index);
}

// A named getter's result: KeyError for no such property (null, undefined or an empty Optional).
template<typename F>
PyObject* named_result(Context& context, PyObject* key, F&& call)
{
    PyObject* result = result_to_py(context, call);
    if (result == Py_None) {
        Py_DECREF(result);
        PyErr_SetObject(PyExc_KeyError, key);
        return nullptr;
    }
    return result;
}

template<typename F>
int setter_result(Context& context, F&& call)
{
    using R = std::remove_cvref_t<decltype(call())>;
    if constexpr (Traits<R>::is_exception_or) {
        auto result = call();
        if (result.is_error()) {
            raise_exception(context, result.exception());
            return -1;
        }
    } else if constexpr (Traits<R>::is_throw_completion_or) {
        auto result = call();
        if (result.is_error()) {
            raise_js_error(context, result.error_value());
            return -1;
        }
    } else {
        call();
    }
    return 0;
}

// ---- [Reflect] attributes: the content attribute, as the JS glue reads and writes it ----

template<typename E>
PyObject* reflected_get_boolean(Context&, E& element, Utf16FlyString const& name)
{
    return PyBool_FromLong(element.has_attribute(name));
}

template<typename E>
PyObject* reflected_get_DOMString(Context& context, E& element, Utf16FlyString const& name)
{
    auto value = element.attribute(name);
    if (!value.has_value())
        return PyUnicode_FromString("");
    return to_py_value(context, *value);
}

template<typename E>
PyObject* reflected_get_USVString(Context& context, E& element, Utf16FlyString const& name)
{
    return reflected_get_DOMString(context, element, name);
}

template<typename E>
PyObject* reflected_get_long(Context&, E& element, Utf16FlyString const& name)
{
    auto value = element.attribute(name);
    if (value.has_value()) {
        if (auto parsed = Web::HTML::parse_integer(*value); parsed.has_value())
            return PyLong_FromLong(*parsed);
    }
    return PyLong_FromLong(0);
}

template<typename E>
PyObject* reflected_get_unsigned_long(Context&, E& element, Utf16FlyString const& name)
{
    auto value = element.attribute(name);
    if (value.has_value()) {
        if (auto parsed = Web::HTML::parse_non_negative_integer(*value); parsed.has_value() && *parsed <= 2147483647u)
            return PyLong_FromUnsignedLong(*parsed);
    }
    return PyLong_FromLong(0);
}

template<typename E>
int reflected_set_boolean(Context&, E& element, Utf16FlyString const& name, PyObject* value, char const*)
{
    int truth = PyObject_IsTrue(value);
    if (truth < 0)
        return -1;
    if (truth)
        element.set_attribute_value(name, Utf16String {});
    else
        element.remove_attribute(name);
    return 0;
}

template<typename E>
int reflected_set_DOMString(Context& context, E& element, Utf16FlyString const& name, PyObject* value, char const* where)
{
    Optional<Utf16String> string;
    if (!convert(context, value, string, where))
        return -1;
    element.set_attribute_value(name, string.release_value());
    return 0;
}

template<typename E>
int reflected_set_USVString(Context& context, E& element, Utf16FlyString const& name, PyObject* value, char const* where)
{
    return reflected_set_DOMString(context, element, name, value, where);
}

template<typename E>
int reflected_set_long(Context& context, E& element, Utf16FlyString const& name, PyObject* value, char const* where)
{
    Optional<i32> number;
    if (!convert(context, value, number, where))
        return -1;
    element.set_attribute_value(name, Utf16String::number(*number));
    return 0;
}

template<typename E>
int reflected_set_unsigned_long(Context& context, E& element, Utf16FlyString const& name, PyObject* value, char const* where)
{
    Optional<u32> number;
    if (!convert(context, value, number, where))
        return -1;
    u32 new_value = *number <= 2147483647u ? *number : 0;
    element.set_attribute_value(name, Utf16String::number(new_value));
    return 0;
}

}
