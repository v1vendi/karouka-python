/*
 * JS values in Python and the asyncio loop (docs/PYTHON.md § JS values, asyncio).
 *
 * A JS object with no DOM proxy of its own is a JSValue proxy: attribute and item access read
 * and write its properties, calling it calls it (a function read as an attribute is bound to its
 * object), .new() constructs, and awaiting it awaits it as JS's await would (a Promise settles a
 * Future of the running loop; a rejection raises JSError). A JSValue is a Proxy like a DOM
 * proxy: rooted, cached per context and stale once its document is discarded.
 *
 * A Python callable passed to JS becomes one JS::NativeFunction per (context, callable), which
 * converts its arguments to Python and its result back, and throws a Python exception as an
 * Error ("Type: message"; a JSError throws its JS value again). Converting that function back
 * gives the callable. A coroutine or other awaitable passed to JS becomes a Promise that the
 * loop settles when it is done.
 *
 * Each document interpreter has one asyncio loop, always running (asyncio.get_running_loop()
 * works anywhere in page Python). Its call_soon callbacks run in one LibWeb task per batch and
 * its call_later timers on one Platform::Timer, which runs on karouka's host clock; the loop's
 * time() is LibWeb's (the host clock's) time. So `await asyncio.sleep(1)` resumes when
 * kr_view_advance_time reaches its deadline, at that exact host time.
 */
#include "context.h"

#include "../engine/HostInterface.h"

#include <LibGC/Function.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebIDL/Promise.h>

#include <limits.h>
#include <math.h>

using namespace Web;

namespace Karouka::Python {

// ---- The loop (Python) ----

// Runs in each document interpreter; the host side is _karouka._loop_now/_loop_wake/_loop_arm.
static char const loop_source[] = R"PY(
import asyncio
import collections
import heapq
import math
from asyncio import events

import _karouka


class PageEventLoop(asyncio.AbstractEventLoop):
    """The document's asyncio loop: callbacks run in LibWeb tasks and timers on the host clock,
    so asyncio.sleep follows kr_view_advance_time. It is always running; run_forever and
    run_until_complete raise."""

    _clock_resolution = 1e-6

    def __init__(self):
        self._ready = collections.deque()
        self._scheduled = []
        self._closed = False
        self._debug = False
        self._exception_handler = None
        self._task_factory = None

    def time(self):
        return _karouka._loop_now() / 1000.0

    def is_running(self):
        return not self._closed

    def is_closed(self):
        return self._closed

    def close(self):
        self._closed = True
        self._ready.clear()
        self._scheduled.clear()
        _karouka._loop_arm(-1)

    def stop(self):
        pass

    def run_forever(self):
        raise RuntimeError("a page's event loop runs on the engine's tasks; it is always running")

    def run_until_complete(self, future):
        self.run_forever()

    async def shutdown_asyncgens(self):
        pass

    async def shutdown_default_executor(self, timeout=None):
        pass

    def get_debug(self):
        return self._debug

    def set_debug(self, enabled):
        self._debug = enabled

    def _check_closed(self):
        if self._closed:
            raise RuntimeError("Event loop is closed")

    def call_soon(self, callback, *args, context=None):
        self._check_closed()
        handle = events.Handle(callback, args, self, context)
        self._ready.append(handle)
        _karouka._loop_wake()
        return handle

    call_soon_threadsafe = call_soon

    def call_later(self, delay, callback, *args, context=None):
        return self.call_at(self.time() + delay, callback, *args, context=context)

    def call_at(self, when, callback, *args, context=None):
        self._check_closed()
        timer = events.TimerHandle(when, callback, args, self, context)
        heapq.heappush(self._scheduled, timer)
        timer._scheduled = True
        self._arm()
        return timer

    def _timer_handle_cancelled(self, handle):
        pass

    def _arm(self):
        while self._scheduled and self._scheduled[0]._cancelled:
            heapq.heappop(self._scheduled)._scheduled = False
        if not self._scheduled:
            _karouka._loop_arm(-1)
            return
        delay = (self._scheduled[0]._when - self.time()) * 1000.0
        _karouka._loop_arm(max(0, math.ceil(delay - 1e-3)))

    def _run_once(self):
        end = self.time() + self._clock_resolution
        while self._scheduled and self._scheduled[0]._when < end:
            handle = heapq.heappop(self._scheduled)
            handle._scheduled = False
            if not handle._cancelled:
                self._ready.append(handle)
        for _ in range(len(self._ready)):
            handle = self._ready.popleft()
            if not handle._cancelled:
                handle._run()
        if self._ready:
            _karouka._loop_wake()
        self._arm()

    def create_future(self):
        return asyncio.Future(loop=self)

    def create_task(self, coro, *, name=None, context=None, **kwargs):
        self._check_closed()
        if self._task_factory is None:
            return asyncio.Task(coro, loop=self, name=name, context=context, **kwargs)
        task = self._task_factory(self, coro, context=context, **kwargs)
        if name is not None:
            task.set_name(name)
        return task

    def set_task_factory(self, factory):
        self._task_factory = factory

    def get_task_factory(self):
        return self._task_factory

    def get_exception_handler(self):
        return self._exception_handler

    def set_exception_handler(self, handler):
        self._exception_handler = handler

    def default_exception_handler(self, context):
        _karouka._loop_report(context.get("message") or "Unhandled exception in the event loop",
                              context.get("exception"))

    def call_exception_handler(self, context):
        if self._exception_handler is None:
            self.default_exception_handler(context)
            return
        try:
            self._exception_handler(self, context)
        except (SystemExit, KeyboardInterrupt):
            raise
        except BaseException as exc:
            self.default_exception_handler({"message": "Unhandled error in the exception handler",
                                            "exception": exc})

    def _to_promise(self, awaitable, resolve, reject):
        future = asyncio.ensure_future(awaitable, loop=self)

        def settle(future):
            if future.cancelled():
                reject(asyncio.CancelledError("the awaitable was cancelled"))
            elif future.exception() is not None:
                reject(future.exception())
            else:
                resolve(future.result())

        future.add_done_callback(settle)
)PY";

// ---- Python → JS ----

static bool is_js_value(Context& context, PyObject* object)
{
    return PyObject_TypeCheck(object, state_of_context(context).js_value_type);
}

static JS::Object& object_of(PyObject* proxy)
{
    return *static_cast<JS::Object*>(reinterpret_cast<Proxy*>(proxy)->cell);
}

static PyObject* wrap_js(Context& context, JS::Object& object)
{
    if (auto it = context.proxies.find(&object); it != context.proxies.end())
        return Py_NewRef(reinterpret_cast<PyObject*>(it->value));
    PyTypeObject* type = state_of_context(context).js_value_type;
    auto* proxy = reinterpret_cast<Proxy*>(type->tp_alloc(type, 0));
    if (!proxy)
        return nullptr;
    proxy->cell = &object;
    proxy->root = new GC::Root<GC::Cell>(&object);
    proxy->context = &context;
    context.proxies.set(&object, proxy);
    return reinterpret_cast<PyObject*>(proxy);
}

// The JS value a raised or passed Python exception stands for: a JSError's own value, else an
// Error whose message is "Type: message".
static JS::Value js_error_for(Context& context, PyObject* exception)
{
    auto& realm = realm_of(context);
    if (PyObject_TypeCheck(exception, reinterpret_cast<PyTypeObject*>(state_of_context(context).js_error))) {
        PyObject* value = PyObject_GetAttrString(exception, "value");
        Optional<JS::Value> js;
        if (value)
            js = to_js(context, value, "JSError.value");
        Py_XDECREF(value);
        if (js.has_value())
            return *js;
        PyErr_Clear();
    }
    PyObject* text = PyObject_Str(exception);
    char const* utf8 = text ? PyUnicode_AsUTF8(text) : nullptr;
    auto message = ByteString::formatted("{}: {}", Py_TYPE(exception)->tp_name, utf8 ? utf8 : "?");
    Py_XDECREF(text);
    PyErr_Clear();
    return JS::Value(JS::Error::create(realm, Utf16String::from_utf8(message.view())).ptr());
}

// The raised Python exception (cleared) as a JS value to throw.
static JS::Value take_js_error(Context& context)
{
    PyObject* exception = PyErr_GetRaisedException();
    if (!exception)
        return JS::Value(JS::Error::create(realm_of(context), Utf16String::from_utf8("a Python call failed"sv)).ptr());
    auto value = js_error_for(context, exception);
    Py_DECREF(exception);
    return value;
}

// The one JS function standing for `callable` in `context` (made on first use).
static GC::Ref<JS::NativeFunction> js_function_for(Context& context, PyObject* callable)
{
    if (auto it = context.callables.find(callable); it != context.callables.end())
        return *it->value;
    auto& realm = realm_of(context);
    auto handle = context.handle;
    auto function = JS::NativeFunction::create(realm, "python"_utf16_fly_string, [handle, callable](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        Context* context = handle->context;
        if (!context)
            return JS::js_undefined();
        JS::Value result = JS::js_undefined();
        bool threw = false;
        {
            RunInContext run(*context);
            PyObject* arguments = PyTuple_New(static_cast<Py_ssize_t>(vm.argument_count()));
            for (size_t i = 0; arguments && i < vm.argument_count(); ++i) {
                PyObject* argument = to_py_js(*context, vm.argument(i));
                if (!argument) {
                    Py_CLEAR(arguments);
                    break;
                }
                PyTuple_SET_ITEM(arguments, static_cast<Py_ssize_t>(i), argument);
            }
            PyObject* returned = arguments ? PyObject_Call(callable, arguments, nullptr) : nullptr;
            Py_XDECREF(arguments);
            if (returned && returned != Py_None) {
                auto value = to_js(*context, returned, "a Python function's result");
                if (value.has_value())
                    result = *value;
            }
            if (PyErr_Occurred()) {
                result = take_js_error(*context);
                threw = true;
            }
            Py_XDECREF(returned);
        }
        after_run(*context);
        if (threw)
            return JS::throw_completion(result);
        return result;
    });
    context.callables.set(Py_NewRef(callable), GC::make_root(function));
    context.callable_of_function.set(function.ptr(), callable);
    return function;
}

struct BoundMethod {
    PyObject_HEAD
    PyObject* function; // JSValue
    PyObject* self;     // JSValue
};

static Optional<JS::Value> to_js_at(Context& context, PyObject* object, char const* where, int depth)
{
    // A JS method read as an attribute is its function again (Reflect.apply(proto.f, ...)).
    if (PyObject_TypeCheck(object, state_of_context(context).js_bound_method_type))
        object = reinterpret_cast<BoundMethod*>(object)->function;
    auto& realm = realm_of(context);
    if (depth > 64) {
        PyErr_Format(PyExc_ValueError, "%s: nested too deeply (a cycle?)", where);
        return {};
    }
    if (object == Py_None)
        return JS::js_null();
    if (PyBool_Check(object))
        return JS::Value(object == Py_True);
    if (PyLong_Check(object)) {
        double value = PyLong_AsDouble(object);
        if (value == -1.0 && PyErr_Occurred())
            return {};
        return JS::Value(value);
    }
    if (PyFloat_Check(object))
        return JS::Value(PyFloat_AS_DOUBLE(object));
    if (PyUnicode_Check(object)) {
        auto utf8 = utf8_of(object, where);
        if (!utf8.has_value())
            return {};
        return JS::Value(JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8_without_validation(*utf8)).ptr());
    }
    if (Py_TYPE(object)->tp_dealloc == proxy_dealloc) {
        Context* owner = live_context(object);
        if (!owner)
            return {};
        if (owner->document.ptr() != context.document.ptr()) {
            PyErr_Format(PyExc_TypeError, "%s: the object belongs to another document", where);
            return {};
        }
        if (is_js_value(*owner, object))
            return JS::Value(&object_of(object));
        auto* cell = reinterpret_cast<Proxy*>(object)->cell;
        auto& world = Bindings::host_defined_wrapper_world(realm);
        if (PyObject_TypeCheck(object, dom_type(context, DomType::Window))) {
            auto browsing_context = static_cast<HTML::Window*>(cell)->browsing_context();
            if (!browsing_context) {
                PyErr_Format(PyExc_TypeError, "%s: the window has no browsing context", where);
                return {};
            }
            return JS::Value(&browsing_context->window_proxy_for(world, realm)->object());
        }
        return JS::Value(Bindings::wrap(world, realm, GC::Ref<Bindings::Wrappable> { *static_cast<Bindings::Wrappable*>(cell) }).ptr());
    }
    if (PyExceptionInstance_Check(object))
        return js_error_for(context, object);
    if (PyDict_Check(object)) {
        auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());
        PyObject* key;
        PyObject* item;
        Py_ssize_t position = 0;
        while (PyDict_Next(object, &position, &key, &item)) {
            auto name = utf8_of(key, where);
            if (!name.has_value())
                return {};
            auto value = to_js_at(context, item, where, depth + 1);
            if (!value.has_value())
                return {};
            MUST(result->create_data_property_or_throw(JS::PropertyKey(Utf16FlyString::from_utf8(*name)), *value));
        }
        return JS::Value(result.ptr());
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        PyObject* sequence = PySequence_Fast(object, where);
        if (!sequence)
            return {};
        GC::RootVector<JS::Value> values;
        for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(sequence); ++i) {
            auto value = to_js_at(context, PySequence_Fast_GET_ITEM(sequence, i), where, depth + 1);
            if (!value.has_value()) {
                Py_DECREF(sequence);
                return {};
            }
            values.append(*value);
        }
        Py_DECREF(sequence);
        return JS::Value(JS::Array::create_from(realm, values.span()).ptr());
    }
    if (Py_TYPE(object)->tp_as_async && Py_TYPE(object)->tp_as_async->am_await) {
        if (!context.loop) {
            PyErr_Format(PyExc_TypeError, "%s: an awaitable becomes a Promise only in the page's own interpreter", where);
            return {};
        }
        auto capability = WebIDL::create_promise(realm);
        PyObject* resolve = wrap_js(context, *capability->resolve());
        PyObject* reject = resolve ? wrap_js(context, *capability->reject()) : nullptr;
        PyObject* done = reject ? PyObject_CallMethod(context.loop, "_to_promise", "OOO", object, resolve, reject) : nullptr;
        Py_XDECREF(resolve);
        Py_XDECREF(reject);
        if (!done)
            return {};
        Py_DECREF(done);
        return JS::Value(capability->promise().ptr());
    }
    if (PyCallable_Check(object))
        return JS::Value(js_function_for(context, object).ptr());
    PyErr_Format(PyExc_TypeError, "%s: %.100s has no JS form", where, Py_TYPE(object)->tp_name);
    return {};
}

Optional<JS::Value> to_js(Context& context, PyObject* object, char const* where)
{
    return to_js_at(context, object, where, 0);
}

GC::Ptr<WebIDL::CallbackType> callback_of(Context& context, PyObject* object, char const* where)
{
    auto value = to_js(context, object, where);
    if (!value.has_value())
        return nullptr;
    if (!value->is_function()) {
        PyErr_Format(PyExc_TypeError, "%s: expected a callable, not %.100s", where, Py_TYPE(object)->tp_name);
        return nullptr;
    }
    auto& realm = realm_of(context);
    return realm.create<WebIDL::CallbackType>(value->as_object(), realm);
}

// ---- JS → Python ----

// Raises JSError for a thrown (or rejected) JS value; returns nullptr.
static PyObject* make_js_error(Context& context, JS::Value thrown)
{
    auto& vm = realm_of(context).vm();
    auto string = thrown.to_utf16_string(vm);
    auto text = string.is_error() ? thrown.to_utf16_string_without_side_effects() : string.release_value();
    PyObject* message = to_py(text.utf16_view());
    PyObject* exception = message ? PyObject_CallOneArg(state_of_context(context).js_error, message) : nullptr;
    Py_XDECREF(message);
    if (!exception)
        return nullptr;
    PyObject* value = to_py_js(context, thrown);
    if (!value) {
        PyErr_Clear();
        value = Py_NewRef(Py_None);
    }
    int set = PyObject_SetAttrString(exception, "value", value);
    Py_DECREF(value);
    if (set < 0) {
        Py_DECREF(exception);
        return nullptr;
    }
    return exception;
}

PyObject* raise_js_error(Context& context, JS::Value thrown)
{
    PyObject* exception = make_js_error(context, thrown);
    if (exception)
        PyErr_SetRaisedException(exception);
    return nullptr;
}

PyObject* to_py_js(Context& context, JS::Value value)
{
    if (value.is_nullish())
        Py_RETURN_NONE;
    if (value.is_boolean())
        return PyBool_FromLong(value.as_bool());
    if (value.is_number()) {
        double number = value.as_double();
        if (isfinite(number) && trunc(number) == number && fabs(number) <= 9007199254740992.0)
            return PyLong_FromLongLong(static_cast<long long>(number));
        return PyFloat_FromDouble(number);
    }
    if (value.is_string())
        return to_py(value.as_string().utf16_string_view());
    if (value.is_bigint()) {
        auto text = value.to_utf16_string_without_side_effects();
        PyObject* string = to_py(text.utf16_view());
        PyObject* number = string ? PyLong_FromUnicodeObject(string, 10) : nullptr;
        Py_XDECREF(string);
        return number;
    }
    if (!value.is_object()) {
        PyErr_SetString(PyExc_TypeError, "a JS symbol has no Python form");
        return nullptr;
    }
    auto& object = value.as_object();
    if (auto* window_proxy = HTML::WindowProxy::from_object(object)) {
        if (auto window = window_proxy->window())
            return wrap(context, window.ptr(), DomType::Window);
    }
    if (auto callable = context.callable_of_function.get(&object); callable.has_value())
        return Py_NewRef(*callable);
    if (auto* wrappable = Bindings::wrappable_impl_from(&object)) {
        if (auto type = dom_type_of_class(*wrappable); type.has_value())
            return wrap(context, wrappable, *type);
    }
    return wrap_js(context, object);
}

// ---- JSValue ----

#define BEGIN_JS(failure)                       \
    Context* context = live_context(self);      \
    if (!context)                               \
        return failure;                         \
    auto& object = object_of(self);             \
    auto& realm = realm_of(*context);           \
    [[maybe_unused]] auto& vm = realm.vm();     \
    Call call(*context)

static Optional<JS::PropertyKey> property_key(PyObject* key)
{
    if (PyLong_Check(key)) {
        long long index = PyLong_AsLongLong(key);
        if (index == -1 && PyErr_Occurred())
            return {};
        if (index < 0 || index > UINT32_MAX - 1) {
            PyErr_SetString(PyExc_IndexError, "a JS index is from 0 to 2^32 - 2");
            return {};
        }
        return JS::PropertyKey(static_cast<u32>(index));
    }
    auto utf8 = utf8_of(key, "a JS property name");
    if (!utf8.has_value())
        return {};
    return JS::PropertyKey(Utf16FlyString::from_utf8(*utf8));
}

static PyObject* new_bound_method(Context&, PyObject* function, PyObject* self);

static PyObject* get_property(PyObject* self, PyObject* key, bool attribute)
{
    BEGIN_JS(nullptr);
    auto property = property_key(key);
    if (!property.has_value())
        return nullptr;
    auto has = object.has_property(*property);
    if (has.is_error())
        return raise_js_error(*context, has.error_value());
    if (!has.value()) {
        if (attribute)
            PyErr_Format(PyExc_AttributeError, "the JS object has no property '%U'", key);
        else if (PyLong_Check(key))
            PyErr_SetObject(PyExc_IndexError, key);
        else
            PyErr_SetObject(PyExc_KeyError, key);
        return nullptr;
    }
    auto value = object.get(*property);
    if (value.is_error())
        return raise_js_error(*context, value.error_value());
    PyObject* result = to_py_js(*context, value.value());
    if (result && attribute && value.value().is_function() && is_js_value(*context, result)) {
        PyObject* bound = new_bound_method(*context, result, self);
        Py_DECREF(result);
        return bound;
    }
    return result;
}

static int set_property(PyObject* self, PyObject* key, PyObject* item)
{
    BEGIN_JS(-1);
    auto property = property_key(key);
    if (!property.has_value())
        return -1;
    if (!item) {
        auto deleted = object.delete_property_or_throw(*property);
        if (deleted.is_error()) {
            raise_js_error(*context, deleted.error_value());
            return -1;
        }
        return 0;
    }
    auto value = to_js(*context, item, "a JS property");
    if (!value.has_value())
        return -1;
    auto set = object.set(*property, *value, JS::Object::ShouldThrowExceptions::Yes);
    if (set.is_error()) {
        raise_js_error(*context, set.error_value());
        return -1;
    }
    return 0;
}

static PyObject* js_value_getattro(PyObject* self, PyObject* name)
{
    PyObject* found = PyObject_GenericGetAttr(self, name);
    if (found || !PyErr_ExceptionMatches(PyExc_AttributeError))
        return found;
    Py_ssize_t length = 0;
    char const* utf8 = PyUnicode_AsUTF8AndSize(name, &length);
    // Python's own protocols (copy, pickle, ...) never reach JS.
    if (!utf8 || (length > 4 && utf8[0] == '_' && utf8[1] == '_' && utf8[length - 1] == '_' && utf8[length - 2] == '_'))
        return nullptr;
    PyErr_Clear();
    return get_property(self, name, true);
}

static int js_value_setattro(PyObject* self, PyObject* name, PyObject* value)
{
    return set_property(self, name, value);
}

static PyObject* js_value_subscript(PyObject* self, PyObject* key)
{
    return get_property(self, key, false);
}

static int js_value_ass_subscript(PyObject* self, PyObject* key, PyObject* value)
{
    return set_property(self, key, value);
}

static Py_ssize_t js_value_length(PyObject* self)
{
    BEGIN_JS(-1);
    auto length = JS::length_of_array_like(vm, object);
    if (length.is_error()) {
        raise_js_error(*context, length.error_value());
        return -1;
    }
    return static_cast<Py_ssize_t>(min(length.value(), static_cast<size_t>(PY_SSIZE_T_MAX)));
}

// Calls the JSValue `function` with `this_value` and Python arguments.
static PyObject* call_js(PyObject* self, Optional<JS::Value> this_value, PyObject* args, PyObject* kwargs, bool construct)
{
    BEGIN_JS(nullptr);
    if (kwargs && PyDict_GET_SIZE(kwargs) > 0) {
        PyErr_SetString(PyExc_TypeError, "a JS function takes no keyword arguments");
        return nullptr;
    }
    if (!object.is_function()) {
        PyErr_SetString(PyExc_TypeError, "the JS value is not a function");
        return nullptr;
    }
    GC::RootVector<JS::Value> arguments;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); ++i) {
        auto argument = to_js(*context, PyTuple_GET_ITEM(args, i), "a JS function's argument");
        if (!argument.has_value())
            return nullptr;
        arguments.append(*argument);
    }
    if (construct) {
        if (!JS::Value(&object).is_constructor()) {
            PyErr_SetString(PyExc_TypeError, "the JS function is not a constructor");
            return nullptr;
        }
        auto result = JS::construct_impl(vm, static_cast<JS::FunctionObject&>(object), arguments.span());
        if (result.is_error())
            return raise_js_error(*context, result.error_value());
        return to_py_js(*context, JS::Value(result.value().ptr()));
    }
    auto result = JS::call(vm, JS::Value(&object), this_value.value_or(JS::js_undefined()), arguments.span());
    if (result.is_error())
        return raise_js_error(*context, result.error_value());
    return to_py_js(*context, result.value());
}

static PyObject* js_value_call(PyObject* self, PyObject* args, PyObject* kwargs)
{
    return call_js(self, {}, args, kwargs, false);
}

static PyObject* js_value_new(PyObject* self, PyObject* args, PyObject* kwargs)
{
    return call_js(self, {}, args, kwargs, true);
}

static void settle_future(NonnullRefPtr<ContextHandle> const& handle, PyObject* future, bool fulfilled, JS::Value value)
{
    Context* context = handle->context;
    // A discarded context's interpreter is gone, and the future with it.
    if (!context)
        return;
    {
        RunInContext run(*context);
        PyObject* done = PyObject_CallMethod(future, "done", nullptr);
        if (done == Py_False) {
            PyObject* result = nullptr;
            if (fulfilled) {
                PyObject* converted = to_py_js(*context, value);
                result = converted ? PyObject_CallMethod(future, "set_result", "O", converted) : nullptr;
                Py_XDECREF(converted);
            } else {
                PyObject* exception = make_js_error(*context, value);
                result = exception ? PyObject_CallMethod(future, "set_exception", "O", exception) : nullptr;
                Py_XDECREF(exception);
            }
            if (!result && PyErr_Occurred()) {
                // The value didn't convert: the awaiting coroutine gets that error.
                PyObject* error = PyErr_GetRaisedException();
                result = PyObject_CallMethod(future, "set_exception", "O", error);
                Py_DECREF(error);
            }
            Py_XDECREF(result);
        }
        Py_XDECREF(done);
        if (PyErr_Occurred())
            report_exception(*context);
        Py_DECREF(future);
    }
    after_run(*context);
}

// await: a Future of the running loop, settled when the value (resolved as a Promise) settles.
static PyObject* js_value_await(PyObject* self)
{
    BEGIN_JS(nullptr);
    PyObject* asyncio = PyImport_ImportModule("asyncio");
    PyObject* loop = asyncio ? PyObject_CallMethod(asyncio, "get_running_loop", nullptr) : nullptr;
    Py_XDECREF(asyncio);
    PyObject* future = loop ? PyObject_CallMethod(loop, "create_future", nullptr) : nullptr;
    Py_XDECREF(loop);
    if (!future)
        return nullptr;
    auto promise = WebIDL::create_resolved_promise(realm, JS::Value(&object));
    auto handle = context->handle;
    // One reference for whichever reaction runs.
    Py_INCREF(future);
    WebIDL::react_to_promise(*promise,
        GC::create_function(realm.heap(), [handle, future](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            settle_future(handle, future, true, value);
            return JS::js_undefined();
        }),
        GC::create_function(realm.heap(), [handle, future](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            settle_future(handle, future, false, value);
            return JS::js_undefined();
        }));
    PyObject* iterator = PyObject_CallMethod(future, "__await__", nullptr);
    Py_DECREF(future);
    return iterator;
}

static PyObject* js_value_repr(PyObject* self)
{
    Context* context = live_context(self);
    if (!context)
        return nullptr;
    auto text = JS::Value(&object_of(self)).to_utf16_string_without_side_effects();
    PyObject* inner = to_py(text.utf16_view());
    PyObject* repr = inner ? PyUnicode_FromFormat("<JSValue %U>", inner) : nullptr;
    Py_XDECREF(inner);
    return repr;
}

static PyObject* js_value_iter(PyObject* self)
{
    // Array-likes iterate by index; get_property raises IndexError past the end.
    return PySeqIter_New(self);
}

static PyMethodDef js_value_methods[] = {
    { "new", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(js_value_new)), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("new(*args): constructs, as JS's `new` does") },
    { nullptr, nullptr, 0, nullptr },
};

static PyType_Slot js_value_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(proxy_dealloc) },
    { Py_tp_getattro, reinterpret_cast<void*>(js_value_getattro) },
    { Py_tp_setattro, reinterpret_cast<void*>(js_value_setattro) },
    { Py_tp_call, reinterpret_cast<void*>(js_value_call) },
    { Py_tp_repr, reinterpret_cast<void*>(js_value_repr) },
    { Py_tp_iter, reinterpret_cast<void*>(js_value_iter) },
    { Py_tp_methods, js_value_methods },
    { Py_mp_subscript, reinterpret_cast<void*>(js_value_subscript) },
    { Py_mp_ass_subscript, reinterpret_cast<void*>(js_value_ass_subscript) },
    { Py_mp_length, reinterpret_cast<void*>(js_value_length) },
    { Py_am_await, reinterpret_cast<void*>(js_value_await) },
    { Py_tp_doc, const_cast<char*>("A JS object: attributes and items are its properties, calling calls it, awaiting awaits it.") },
    { 0, nullptr },
};

static PyType_Spec js_value_spec = {
    "_karouka.JSValue", sizeof(Proxy), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, js_value_slots
};

// ---- A JS function read as an attribute: bound to its object ----

static PyObject* new_bound_method(Context& context, PyObject* function, PyObject* self)
{
    PyTypeObject* type = state_of_context(context).js_bound_method_type;
    auto* bound = PyObject_GC_New(BoundMethod, type);
    if (!bound)
        return nullptr;
    bound->function = Py_NewRef(function);
    bound->self = Py_NewRef(self);
    PyObject_GC_Track(bound);
    return reinterpret_cast<PyObject*>(bound);
}

static PyObject* bound_call(PyObject* object, PyObject* args, PyObject* kwargs)
{
    auto* bound = reinterpret_cast<BoundMethod*>(object);
    if (!live_context(bound->self))
        return nullptr;
    return call_js(bound->function, JS::Value(&object_of(bound->self)), args, kwargs, false);
}

static PyObject* bound_repr(PyObject* object)
{
    auto* bound = reinterpret_cast<BoundMethod*>(object);
    return PyUnicode_FromFormat("<bound JS method %R of %R>", bound->function, bound->self);
}

// Anything else is the function's: js.Array.new(...), js.console.log.name.
static PyObject* bound_getattro(PyObject* object, PyObject* name)
{
    PyObject* found = PyObject_GenericGetAttr(object, name);
    if (found || !PyErr_ExceptionMatches(PyExc_AttributeError))
        return found;
    PyErr_Clear();
    return PyObject_GetAttr(reinterpret_cast<BoundMethod*>(object)->function, name);
}

static int bound_traverse(PyObject* object, visitproc visit, void* arg)
{
    auto* bound = reinterpret_cast<BoundMethod*>(object);
    Py_VISIT(Py_TYPE(object));
    Py_VISIT(bound->function);
    Py_VISIT(bound->self);
    return 0;
}

static int bound_clear(PyObject* object)
{
    auto* bound = reinterpret_cast<BoundMethod*>(object);
    Py_CLEAR(bound->function);
    Py_CLEAR(bound->self);
    return 0;
}

static void bound_dealloc(PyObject* object)
{
    PyTypeObject* type = Py_TYPE(object);
    PyObject_GC_UnTrack(object);
    bound_clear(object);
    type->tp_free(object);
    Py_DECREF(type);
}

static PyMemberDef bound_members[] = {
    { "__func__", Py_T_OBJECT_EX, offsetof(BoundMethod, function), Py_READONLY, nullptr },
    { "__self__", Py_T_OBJECT_EX, offsetof(BoundMethod, self), Py_READONLY, nullptr },
    { nullptr, 0, 0, 0, nullptr },
};

static PyType_Slot bound_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(bound_dealloc) },
    { Py_tp_traverse, reinterpret_cast<void*>(bound_traverse) },
    { Py_tp_clear, reinterpret_cast<void*>(bound_clear) },
    { Py_tp_call, reinterpret_cast<void*>(bound_call) },
    { Py_tp_repr, reinterpret_cast<void*>(bound_repr) },
    { Py_tp_getattro, reinterpret_cast<void*>(bound_getattro) },
    { Py_tp_members, bound_members },
    { 0, nullptr },
};

static PyType_Spec bound_spec = {
    "_karouka.JSBoundMethod", sizeof(BoundMethod), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_HAVE_GC, bound_slots
};

// ---- Window.fetch ----

// Fetch::fetch reads its caller's function from the running execution context (FetchMethod.cpp,
// "this's relevant Realm"), which a Python call has none of: Window.fetch calls the global's JS
// fetch, LibWeb's own binding, with the arguments converted as JS values (an init dict becomes
// the RequestInit object it reads).
PyObject* window_fetch(PyObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    Context* context = live_context(self);
    if (!context)
        return nullptr;
    auto& realm = realm_of(*context);
    auto& vm = realm.vm();
    Call call(*context);
    auto& global = realm.global_object();
    auto fetch = global.get(JS::PropertyKey(Utf16FlyString::from_utf8("fetch"sv)));
    if (fetch.is_error())
        return raise_js_error(*context, fetch.error_value());
    GC::RootVector<JS::Value> arguments;
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        auto argument = to_js(*context, args[i], "Window.fetch");
        if (!argument.has_value())
            return nullptr;
        arguments.append(*argument);
    }
    auto result = JS::call(vm, fetch.value(), JS::Value(&global), arguments.span());
    if (result.is_error())
        return raise_js_error(*context, result.error_value());
    return to_py_js(*context, result.value());
}

// ---- The loop's host side ----

static void run_loop(NonnullRefPtr<ContextHandle> const& handle, bool from_task)
{
    Context* context = handle->context;
    if (!context)
        return;
    if (from_task)
        context->loop_task_queued = false;
    if (!context->loop)
        return;
    {
        RunInContext run(*context);
        PyObject* result = PyObject_CallMethod(context->loop, "_run_once", nullptr);
        if (result)
            Py_DECREF(result);
        else
            report_exception(*context);
    }
    after_run(*context);
}

// Queues the task that runs the loop's ready callbacks, once.
static void queue_loop_task(Context& context)
{
    if (context.loop_task_queued)
        return;
    context.loop_task_queued = true;
    auto handle = context.handle;
    auto& heap = realm_of(context).heap();
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, context.document.ptr(), GC::create_function(heap, [handle] {
        run_loop(handle, true);
    }));
}

static Context* loop_context()
{
    Context* context = current_document_context();
    if (!context)
        PyErr_SetString(PyExc_RuntimeError, "the page's event loop runs only in its document's interpreter");
    return context;
}

static PyObject* loop_now(PyObject*, PyObject*)
{
    return PyFloat_FromDouble(HighResolutionTime::unsafe_shared_current_time());
}

static PyObject* loop_wake(PyObject*, PyObject*)
{
    Context* context = loop_context();
    if (!context)
        return nullptr;
    queue_loop_task(*context);
    Py_RETURN_NONE;
}

static PyObject* loop_arm(PyObject*, PyObject* argument)
{
    Context* context = loop_context();
    if (!context)
        return nullptr;
    long long delay = PyLong_AsLongLong(argument);
    if (delay == -1 && PyErr_Occurred())
        return nullptr;
    if (delay < 0) {
        if (context->loop_timer)
            context->loop_timer->stop();
        Py_RETURN_NONE;
    }
    int interval = static_cast<int>(min<long long>(delay, INT_MAX));
    if (!context->loop_timer) {
        auto handle = context->handle;
        auto& heap = realm_of(*context).heap();
        // The timer's firing queues the loop's task: the callbacks then run as a task, with the
        // microtask checkpoint after it, at the timer's host time.
        context->loop_timer = GC::make_root(Platform::Timer::create_single_shot(heap, interval, GC::create_function(heap, [handle] {
            if (Context* context = handle->context)
                queue_loop_task(*context);
        })));
    }
    context->loop_timer->restart(interval);
    Py_RETURN_NONE;
}

static PyObject* loop_report(PyObject*, PyObject* args)
{
    PyObject* message;
    PyObject* exception;
    if (!PyArg_ParseTuple(args, "UO", &message, &exception))
        return nullptr;
    Context* context = current_document_context();
    // A document being discarded drops its pending tasks quietly.
    if (!context)
        Py_RETURN_NONE;
    if (exception != Py_None && PyExceptionInstance_Check(exception)) {
        PyErr_SetRaisedException(Py_NewRef(exception));
        report_exception(*context);
    } else {
        log_line(KR_LOG_ERROR, "python"sv, StringView { PyUnicode_AsUTF8(message), strlen(PyUnicode_AsUTF8(message)) });
    }
    Py_RETURN_NONE;
}

static PyMethodDef js_functions[] = {
    { "_loop_now", loop_now, METH_NOARGS, nullptr },
    { "_loop_wake", loop_wake, METH_NOARGS, nullptr },
    { "_loop_arm", loop_arm, METH_O, nullptr },
    { "_loop_report", loop_report, METH_VARARGS, nullptr },
    { nullptr, nullptr, 0, nullptr },
};

int js_exec(PyObject* module, ModuleState& state)
{
    if (PyModule_AddFunctions(module, js_functions) < 0)
        return -1;
    state.js_value_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &js_value_spec, nullptr));
    if (!state.js_value_type || PyModule_AddType(module, state.js_value_type) < 0)
        return -1;
    state.js_bound_method_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &bound_spec, nullptr));
    if (!state.js_bound_method_type || PyModule_AddType(module, state.js_bound_method_type) < 0)
        return -1;
    state.js_error = PyErr_NewExceptionWithDoc("_karouka.JSError",
        "A JS exception or Promise rejection; .value is the thrown value.", nullptr, nullptr);
    if (!state.js_error || PyModule_AddObjectRef(module, "JSError", state.js_error) < 0)
        return -1;
    return 0;
}

int js_traverse(ModuleState& state, visitproc visit, void* arg)
{
    Py_VISIT(state.js_value_type);
    Py_VISIT(state.js_bound_method_type);
    Py_VISIT(state.js_error);
    return 0;
}

void js_clear(ModuleState& state)
{
    Py_CLEAR(state.js_value_type);
    Py_CLEAR(state.js_bound_method_type);
    Py_CLEAR(state.js_error);
}

bool js_set_up(Context& context, PyObject* globals)
{
    PyObject* namespace_ = PyDict_New();
    PyObject* name = PyUnicode_FromString("_karouka_loop");
    bool ok = namespace_ && name && PyDict_SetItemString(namespace_, "__builtins__", PyEval_GetBuiltins()) == 0
        && PyDict_SetItemString(namespace_, "__name__", name) == 0;
    PyObject* ran = ok ? PyRun_String(loop_source, Py_file_input, namespace_, namespace_) : nullptr;
    Py_XDECREF(ran);
    PyObject* loop_type = ran ? PyDict_GetItemString(namespace_, "PageEventLoop") : nullptr;
    context.loop = loop_type ? PyObject_CallNoArgs(loop_type) : nullptr;
    Py_XDECREF(namespace_);
    Py_XDECREF(name);
    if (!context.loop)
        return false;
    // Always the running loop of this interpreter (its one thread state).
    PyObject* events = PyImport_ImportModule("asyncio.events");
    PyObject* set = events ? PyObject_CallMethod(events, "_set_running_loop", "O", context.loop) : nullptr;
    Py_XDECREF(events);
    if (!set)
        return false;
    Py_DECREF(set);

    // `js`: JS's global object, also importable (`from js import fetch`).
    auto& realm = realm_of(context);
    PyObject* global = wrap_js(context, realm.global_object());
    PyObject* modules = PyImport_GetModuleDict();
    ok = global && PyDict_SetItemString(globals, "js", global) == 0 && PyDict_SetItemString(modules, "js", global) == 0;
    Py_XDECREF(global);
    return ok;
}

void js_discard(Context& context)
{
    if (context.loop_timer) {
        context.loop_timer->stop();
        context.loop_timer = nullptr;
    }
    if (context.loop) {
        PyObject* closed = PyObject_CallMethod(context.loop, "close", nullptr);
        if (closed)
            Py_DECREF(closed);
        else
            PyErr_Clear();
        Py_CLEAR(context.loop);
    }
    context.callable_of_function.clear();
    for (auto& entry : context.callables)
        Py_DECREF(entry.key);
    context.callables.clear();
}

}
