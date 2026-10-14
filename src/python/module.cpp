/*
 * _karouka: the engine as a CPython extension module (docs/PYTHON.md).
 *
 * The module is a second image linked from libkarouka's objects (src/CMakeLists.txt), so it calls
 * the C API (include/karouka/karouka.h) inside its own copy of the engine; it exports only
 * PyInit__karouka. The API is a thin wrapper over that C API (page scripts and the DOM proxies are
 * dom.cpp's):
 *
 *     system = _karouka.System(backend="software")    # or "metal" (the default)
 *     view = system.view(800, 600)                     # scale=1.0, transparent=False
 *     view.load_file("page.html")                      # or view.load_html(html, base_url)
 *     view.advance_time(16.0); view.render()           # render() -> whether the output changed
 *     view.capture_png("out.png"); view.title; view.close()
 *     view.reload(); view.inject_input("mouse_down", x, y); system.memory_stats()
 *     view.document; view.window; view.app_root           # dom.cpp
 *     app = _karouka.App(); app.open("page.html"); app.run()  # app.mm: native windows
 *
 * A failed call raises _karouka.Error, its args (message, kr_result code).
 *
 * Plain CPython C API (Ladybird compiles with -fno-exceptions), multi-phase init, heap types and
 * per-module state, so each interpreter that imports the module gets its own types and the module
 * can be imported in a subinterpreter with its own GIL (Py_MOD_PER_INTERPRETER_GIL_SUPPORTED).
 *
 * The engine itself is process-wide and single-threaded: one kr_system per process (a second
 * System(), from any interpreter, raises Error) and every call on the main thread. With a GIL per
 * interpreter two threads can run Python at once, so every entry point checks pthread_main_np() and
 * raises Error elsewhere, which keeps the engine on one thread whichever interpreter calls it.
 * Destruction can't refuse: a View or System whose last reference goes on another thread (or on the
 * main thread while the engine runs Python) is queued, and the main thread destroys it, in order,
 * from a pending call (Py_AddPendingCall: the main interpreter's main thread) or at its next entry
 * point.
 */

#include "module.h"

#include <atomic>
#include <pthread.h>
#include <string.h>

using namespace Karouka::Python;

namespace {

struct SystemObject {
    PyObject_HEAD
    kr_system* system;
};

// A view keeps its System alive: kr_system_destroy destroys the views still alive. hooks is its
// Python side (dom.cpp): text/python, the app root and its documents' contexts.
struct ViewObject {
    PyObject_HEAD
    kr_view* view;
    PyObject* system;
    ViewHooks* hooks;
};

char const* result_name(kr_result result)
{
    switch (result) {
    case KR_OK:
        return "KR_OK";
    case KR_ERROR_INVALID_ARGUMENT:
        return "KR_ERROR_INVALID_ARGUMENT";
    case KR_ERROR_IO:
        return "KR_ERROR_IO";
    case KR_ERROR_GPU:
        return "KR_ERROR_GPU";
    case KR_ERROR_INTERNAL:
        return "KR_ERROR_INTERNAL";
    case KR_ERROR_LIMIT_EXCEEDED:
        return "KR_ERROR_LIMIT_EXCEEDED";
    case KR_ERROR_INVALID_STATE:
        return "KR_ERROR_INVALID_STATE";
    }
    return "kr_result ?";
}

// ---- Deferred destruction ----

// A View's or System's engine side, queued until the main thread can destroy it. A view goes in
// before its ViewObject drops its System, so a view is always destroyed before its system.
struct Deferred {
    kr_view* view;
    ViewHooks* hooks;
    kr_system* system;
    Deferred* next;
};

pthread_mutex_t s_deferred_lock = PTHREAD_MUTEX_INITIALIZER;
Deferred* s_deferred_head;
Deferred* s_deferred_tail;
// Under s_deferred_lock: queued now, queued ever, destroyed from the queue (on the main thread).
size_t s_deferred_pending;
size_t s_deferred_total;
size_t s_deferred_destroyed;
std::atomic<bool> s_drain_scheduled;
bool s_draining; // main thread only

// The main thread, outside any Python the engine runs and outside a drain.
bool can_destroy_now()
{
    return pthread_main_np() && !dom_busy() && !s_draining;
}

void destroy_now(kr_view* view, ViewHooks* hooks, kr_system* system)
{
    if (hooks)
        dom_detach_view(hooks);
    if (view)
        kr_view_destroy(view);
    if (system)
        kr_system_destroy(system);
}

// Destroys the queue in order, when it can (a destruction that queues more is destroyed too).
void drain_deferred()
{
    if (!can_destroy_now())
        return;
    s_draining = true;
    for (;;) {
        pthread_mutex_lock(&s_deferred_lock);
        Deferred* entry = s_deferred_head;
        if (entry) {
            s_deferred_head = entry->next;
            if (!s_deferred_head)
                s_deferred_tail = nullptr;
            --s_deferred_pending;
        }
        pthread_mutex_unlock(&s_deferred_lock);
        if (!entry)
            break;
        destroy_now(entry->view, entry->hooks, entry->system);
        delete entry;
        pthread_mutex_lock(&s_deferred_lock);
        ++s_deferred_destroyed;
        pthread_mutex_unlock(&s_deferred_lock);
    }
    s_draining = false;
}

// Py_AddPendingCall's: the main interpreter's main thread, between bytecodes. Busy: the next entry
// point drains.
int drain_pending_call(void*)
{
    s_drain_scheduled = false;
    drain_deferred();
    return 0;
}

void defer(kr_view* view, ViewHooks* hooks, kr_system* system)
{
    auto* entry = new Deferred { view, hooks, system, nullptr };
    pthread_mutex_lock(&s_deferred_lock);
    if (s_deferred_tail)
        s_deferred_tail->next = entry;
    else
        s_deferred_head = entry;
    s_deferred_tail = entry;
    ++s_deferred_pending;
    ++s_deferred_total;
    pthread_mutex_unlock(&s_deferred_lock);
    // The pending-call queue is small (and full fails): one scheduled drain at a time.
    if (!s_drain_scheduled.exchange(true) && Py_AddPendingCall(drain_pending_call, nullptr) < 0)
        s_drain_scheduled = false;
}

// Destroys now (after what is queued), or queues for the main thread.
void release_engine(kr_view* view, ViewHooks* hooks, kr_system* system)
{
    if (!can_destroy_now()) {
        defer(view, hooks, system);
        return;
    }
    drain_deferred();
    destroy_now(view, hooks, system);
}

}

namespace Karouka::Python {

ModuleState* state_of(PyObject* module)
{
    return static_cast<ModuleState*>(PyModule_GetState(module));
}

ModuleState* state_of_type(PyTypeObject* type)
{
    PyObject* module = PyType_GetModuleByDef(type, definition());
    return module ? state_of(module) : nullptr;
}

PyObject* raise_error(PyTypeObject* type, char const* message, int code)
{
    ModuleState* state = state_of_type(type);
    if (!state)
        return nullptr;
    PyObject* args = Py_BuildValue("(si)", message, code);
    if (args) {
        PyErr_SetObject(state->error, args);
        Py_DECREF(args);
    }
    return nullptr;
}

bool on_main_thread(PyTypeObject* type)
{
    if (pthread_main_np()) {
        drain_deferred();
        return true;
    }
    raise_error(type, "karouka runs on the main thread only", static_cast<int>(KR_ERROR_INVALID_STATE));
    return false;
}

}

namespace {

PyObject* raise_result(PyTypeObject* type, char const* call, kr_result result)
{
    PyObject* message = PyUnicode_FromFormat("%s failed: %s", call, result_name(result));
    if (!message)
        return nullptr;
    raise_error(type, PyUnicode_AsUTF8(message), static_cast<int>(result));
    Py_DECREF(message);
    return nullptr;
}

// A PyCFunction from another method signature, through void(*)() so that no cast warning fires.
template<typename Function>
PyCFunction method(Function function)
{
    return reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(function));
}

// ---- System ----

PyObject* system_new(PyTypeObject* type, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "backend", "verbose", nullptr };
    char const* backend = "metal";
    int verbose = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$sp:System", keywords, &backend, &verbose))
        return nullptr;
    kr_system_desc desc = {};
    desc.struct_size = sizeof(desc);
    desc.verbose = verbose;
    if (strcmp(backend, "metal") == 0) {
        desc.backend = KR_BACKEND_METAL;
    } else if (strcmp(backend, "software") == 0) {
        desc.backend = KR_BACKEND_SOFTWARE;
    } else {
        PyErr_Format(PyExc_ValueError, "backend must be 'metal' or 'software', not '%s'", backend);
        return nullptr;
    }
    if (!on_main_thread(type))
        return nullptr;
    kr_system* system = nullptr;
    kr_result result = kr_system_create2(&desc, &system);
    if (result != KR_OK)
        return raise_result(type, "kr_system_create2 (one System per process)", result);
    auto* self = reinterpret_cast<SystemObject*>(type->tp_alloc(type, 0));
    if (!self) {
        kr_system_destroy(system);
        return nullptr;
    }
    self->system = system;
    return reinterpret_cast<PyObject*>(self);
}

void system_dealloc(PyObject* object)
{
    auto* self = reinterpret_cast<SystemObject*>(object);
    PyTypeObject* type = Py_TYPE(object);
    if (self->system)
        release_engine(nullptr, nullptr, self->system);
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* system_view(PyObject* object, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "width", "height", "scale", "transparent", nullptr };
    auto* self = reinterpret_cast<SystemObject*>(object);
    PyTypeObject* type = Py_TYPE(object);
    unsigned int width = 0;
    unsigned int height = 0;
    float scale = 1;
    int transparent = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "II|fp:view", keywords, &width, &height, &scale, &transparent))
        return nullptr;
    if (!on_main_thread(type))
        return nullptr;
    ModuleState* state = state_of_type(type);
    if (!state)
        return nullptr;
    kr_view_desc desc = {};
    desc.struct_size = sizeof(desc);
    desc.width = width;
    desc.height = height;
    desc.scale = scale;
    desc.transparent_background = transparent;
    kr_view* view = nullptr;
    kr_result result = kr_view_create2(self->system, &desc, &view);
    if (result != KR_OK)
        return raise_result(type, "kr_view_create2", result);
    auto* view_object = reinterpret_cast<ViewObject*>(state->view_type->tp_alloc(state->view_type, 0));
    if (!view_object) {
        kr_view_destroy(view);
        return nullptr;
    }
    view_object->view = view;
    view_object->system = Py_NewRef(object);
    view_object->hooks = dom_attach_view(view);
    return reinterpret_cast<PyObject*>(view_object);
}

PyObject* system_memory_stats(PyObject* object, PyObject*)
{
    auto* self = reinterpret_cast<SystemObject*>(object);
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    kr_memory_stats stats = {};
    stats.struct_size = sizeof(stats);
    kr_result result = kr_system_query_memory_stats(self->system, &stats);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_system_query_memory_stats", result);
    return Py_BuildValue("{sKsKsKsKsKsKsKsK}",
        "footprint_bytes", static_cast<unsigned long long>(stats.footprint_bytes),
        "malloc_zone_bytes", static_cast<unsigned long long>(stats.malloc_zone_bytes),
        "kmalloc_bytes", static_cast<unsigned long long>(stats.kmalloc_bytes),
        "gc_heap_bytes", static_cast<unsigned long long>(stats.gc_heap_bytes),
        "gpu_cache_bytes", static_cast<unsigned long long>(stats.gpu_cache_bytes),
        "gpu_cache_budget_bytes", static_cast<unsigned long long>(stats.gpu_cache_budget_bytes),
        "surface_bytes", static_cast<unsigned long long>(stats.surface_bytes),
        "font_cache_strikes", static_cast<unsigned long long>(stats.font_cache_strikes));
}

PyMethodDef system_methods[] = {
    { "memory_stats", system_memory_stats, METH_NOARGS,
        PyDoc_STR("memory_stats() -> dict\n\nThe process's memory by owner (kr_system_query_memory_stats).") },
    { "view", method(system_view), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("view(width, height, scale=1.0, transparent=False) -> View\n\n"
                  "A view of width x height device pixels (kr_view_create2).") },
    { nullptr, nullptr, 0, nullptr },
};

PyType_Slot system_slots[] = {
    { Py_tp_new, reinterpret_cast<void*>(system_new) },
    { Py_tp_dealloc, reinterpret_cast<void*>(system_dealloc) },
    { Py_tp_methods, system_methods },
    { Py_tp_doc, const_cast<char*>(PyDoc_STR(
                     "System(*, backend='metal', verbose=False)\n\n"
                     "The engine (kr_system_create2): one per process. backend is 'metal' or 'software'."))
    },
    { 0, nullptr },
};

PyType_Spec system_spec = {
    "_karouka.System",
    sizeof(SystemObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
    system_slots,
};

// ---- View ----

}

kr_view* Karouka::Python::view_handle(PyObject* view)
{
    return reinterpret_cast<ViewObject*>(view)->view;
}

namespace {

// The view's handle, or nullptr with Error raised: the view is closed, or this isn't the main thread.
kr_view* live_view(PyObject* object)
{
    auto* self = reinterpret_cast<ViewObject*>(object);
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    if (!self->view) {
        raise_error(Py_TYPE(object), "the view is closed", static_cast<int>(KR_ERROR_INVALID_STATE));
        return nullptr;
    }
    return self->view;
}

// Off the main thread (dealloc) or inside Python the engine runs, the view is queued (see
// release_engine) before its System reference goes.
void view_release(ViewObject* self)
{
    if (self->view || self->hooks)
        release_engine(self->view, self->hooks, nullptr);
    self->view = nullptr;
    self->hooks = nullptr;
    Py_CLEAR(self->system);
}

void view_dealloc(PyObject* object)
{
    PyTypeObject* type = Py_TYPE(object);
    view_release(reinterpret_cast<ViewObject*>(object));
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* view_close(PyObject* object, PyObject*)
{
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    view_release(reinterpret_cast<ViewObject*>(object));
    Py_RETURN_NONE;
}

PyObject* view_load_file(PyObject* object, PyObject* arg)
{
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    PyObject* path = nullptr;
    if (!PyUnicode_FSConverter(arg, &path))
        return nullptr;
    dom_note_load_file(reinterpret_cast<ViewObject*>(object)->hooks, PyBytes_AS_STRING(path));
    kr_result result = kr_view_load_file(view, PyBytes_AS_STRING(path));
    Py_DECREF(path);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_load_file", result);
    Py_RETURN_NONE;
}

PyObject* view_load_html(PyObject* object, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "html", "base_url", nullptr };
    char const* html = nullptr;
    Py_ssize_t length = 0;
    char const* base_url = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|z:load_html", keywords, &html, &length, &base_url))
        return nullptr;
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    kr_result result = kr_view_load_html(view, html, static_cast<size_t>(length), base_url);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_load_html", result);
    Py_RETURN_NONE;
}

PyObject* view_advance_time(PyObject* object, PyObject* arg)
{
    double now_ms = PyFloat_AsDouble(arg);
    if (now_ms == -1 && PyErr_Occurred())
        return nullptr;
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    kr_result result = kr_view_advance_time(view, now_ms);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_advance_time", result);
    Py_RETURN_NONE;
}

PyObject* view_render(PyObject* object, PyObject*)
{
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    bool rendered = false;
    kr_result result = kr_view_render(view, &rendered);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_render", result);
    return PyBool_FromLong(rendered);
}

PyObject* view_capture_png(PyObject* object, PyObject* arg)
{
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    PyObject* path = nullptr;
    if (!PyUnicode_FSConverter(arg, &path))
        return nullptr;
    kr_result result = kr_view_capture_png(view, PyBytes_AS_STRING(path));
    Py_DECREF(path);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_capture_png", result);
    Py_RETURN_NONE;
}

PyObject* view_title(PyObject* object, void*)
{
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    char const* title = kr_view_title(view);
    return PyUnicode_DecodeUTF8(title ? title : "", title ? static_cast<Py_ssize_t>(strlen(title)) : 0, "replace");
}

PyObject* view_reload(PyObject* object, PyObject*)
{
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    kr_result result = kr_view_reload(view);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_reload", result);
    Py_RETURN_NONE;
}

PyObject* view_inject_input(PyObject* object, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "type", "x", "y", "button", "modifiers", "delta_x", "delta_y", "key_code", "code_point", nullptr };
    static char const* const types[] = { "mouse_move", "mouse_down", "mouse_up", "wheel", "key_down", "key_up" };
    char const* type = nullptr;
    kr_input_event event = {};
    event.struct_size = sizeof(event);
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ff$IIffII:inject_input", keywords, &type, &event.x, &event.y,
            &event.button, &event.modifiers, &event.delta_x, &event.delta_y, &event.key_code, &event.code_point))
        return nullptr;
    size_t index = 0;
    while (index < sizeof(types) / sizeof(*types) && strcmp(types[index], type) != 0)
        ++index;
    if (index == sizeof(types) / sizeof(*types)) {
        PyErr_Format(PyExc_ValueError, "unknown input type '%s'", type);
        return nullptr;
    }
    event.type = static_cast<kr_input_type>(index);
    kr_view* view = live_view(object);
    if (!view)
        return nullptr;
    kr_result result = kr_view_inject_input2(view, &event);
    if (result != KR_OK)
        return raise_result(Py_TYPE(object), "kr_view_inject_input2", result);
    Py_RETURN_NONE;
}

PyObject* view_document(PyObject* object, void*)
{
    if (!live_view(object))
        return nullptr;
    return dom_view_document(object, reinterpret_cast<ViewObject*>(object)->hooks, false);
}

PyObject* view_window(PyObject* object, void*)
{
    if (!live_view(object))
        return nullptr;
    return dom_view_document(object, reinterpret_cast<ViewObject*>(object)->hooks, true);
}

PyObject* view_app_root(PyObject* object, void*)
{
    if (!live_view(object))
        return nullptr;
    return dom_app_root(reinterpret_cast<ViewObject*>(object)->hooks);
}

int view_set_app_root(PyObject* object, PyObject* value, void*)
{
    if (!live_view(object))
        return -1;
    return dom_set_app_root(reinterpret_cast<ViewObject*>(object)->hooks, value);
}

PyMethodDef view_methods[] = {
    { "reload", view_reload, METH_NOARGS,
        PyDoc_STR("reload()\n\nLoad the current page again (kr_view_reload).") },
    { "inject_input", method(view_inject_input), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("inject_input(type, x=0, y=0, *, button=0, modifiers=0, delta_x=0, delta_y=0, key_code=0, code_point=0)\n\n"
                  "Queue one input event (kr_view_inject_input2); type is 'mouse_move', 'mouse_down', 'mouse_up',\n"
                  "'wheel', 'key_down' or 'key_up', positions in device pixels.") },
    { "load_file", view_load_file, METH_O,
        PyDoc_STR("load_file(path)\n\nLoad a page from disk (kr_view_load_file).") },
    { "load_html", method(view_load_html), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("load_html(html, base_url=None)\n\nLoad markup; base_url resolves relative URLs (kr_view_load_html).") },
    { "advance_time", view_advance_time, METH_O,
        PyDoc_STR("advance_time(now_ms)\n\nMove the engine's clock and run its event loop (kr_view_advance_time).") },
    { "render", view_render, METH_NOARGS,
        PyDoc_STR("render() -> bool\n\nRasterize if anything changed; whether the output changed (kr_view_render).") },
    { "capture_png", view_capture_png, METH_O,
        PyDoc_STR("capture_png(path)\n\nWrite the last rendered frame to a PNG file (kr_view_capture_png).") },
    { "close", view_close, METH_NOARGS,
        PyDoc_STR("close()\n\nDestroy the view now (kr_view_destroy); later calls raise Error.") },
    { nullptr, nullptr, 0, nullptr },
};

PyGetSetDef view_getset[] = {
    { "title", view_title, nullptr, PyDoc_STR("The document's title (kr_view_title); '' without one."), nullptr },
    { "document", view_document, nullptr, PyDoc_STR("The active document, as a proxy in this interpreter (None without one)."), nullptr },
    { "window", view_window, nullptr, PyDoc_STR("The active document's Window, as a proxy in this interpreter."), nullptr },
    { "app_root", view_app_root, view_set_app_root,
        PyDoc_STR("The directory whose file: documents may run text/python (None: none yet; load_file sets it\n"
                  "to the file's directory when unset)."), nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

PyType_Slot view_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(view_dealloc) },
    { Py_tp_methods, view_methods },
    { Py_tp_getset, view_getset },
    { Py_tp_doc, const_cast<char*>(PyDoc_STR("A view of a System (System.view): one page rendered into one surface.")) },
    { 0, nullptr },
};

PyType_Spec view_spec = {
    "_karouka.View",
    sizeof(ViewObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    view_slots,
};

// ---- Module ----

int module_exec(PyObject* module)
{
    ModuleState* state = state_of(module);
    state->system_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &system_spec, nullptr));
    if (!state->system_type || PyModule_AddType(module, state->system_type) < 0)
        return -1;
    state->view_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &view_spec, nullptr));
    if (!state->view_type || PyModule_AddType(module, state->view_type) < 0)
        return -1;
    state->error = PyErr_NewExceptionWithDoc("_karouka.Error",
        "A karouka call failed: args are (message, kr_result code).", nullptr, nullptr);
    if (!state->error || PyModule_AddObjectRef(module, "Error", state->error) < 0)
        return -1;
    if (dom_exec(module, *state) < 0)
        return -1;
    return app_exec(module, *state);
}

int module_traverse(PyObject* module, visitproc visit, void* arg)
{
    ModuleState* state = state_of(module);
    Py_VISIT(state->system_type);
    Py_VISIT(state->view_type);
    Py_VISIT(state->error);
    if (int result = app_traverse(*state, visit, arg))
        return result;
    return dom_traverse(*state, visit, arg);
}

int module_clear(PyObject* module)
{
    ModuleState* state = state_of(module);
    Py_CLEAR(state->system_type);
    Py_CLEAR(state->view_type);
    Py_CLEAR(state->error);
    app_clear(*state);
    dom_clear(*state);
    return 0;
}

void module_free(void* module)
{
    module_clear(static_cast<PyObject*>(module));
}

// Any thread: the deferred-destruction queue's counts.
PyObject* debug_deferred(PyObject*, PyObject*)
{
    pthread_mutex_lock(&s_deferred_lock);
    size_t pending = s_deferred_pending;
    size_t total = s_deferred_total;
    size_t destroyed = s_deferred_destroyed;
    pthread_mutex_unlock(&s_deferred_lock);
    return Py_BuildValue("{snsnsn}", "pending", static_cast<Py_ssize_t>(pending),
        "deferred", static_cast<Py_ssize_t>(total), "destroyed", static_cast<Py_ssize_t>(destroyed));
}

PyMethodDef module_methods[] = {
    { "_debug_deferred", debug_deferred, METH_NOARGS,
        PyDoc_STR("_debug_deferred() -> dict\n\nViews and Systems queued for the main thread: pending now, deferred\n"
                  "and destroyed ever (tests; any thread).") },
    { "_debug_counts", dom_debug_counts, METH_NOARGS,
        PyDoc_STR("_debug_counts() -> dict\n\nLive document contexts, interpreters, proxies and listener functions (tests).") },
    { nullptr, nullptr, 0, nullptr },
};

PyModuleDef_Slot module_slots[] = {
    { Py_mod_exec, reinterpret_cast<void*>(module_exec) },
    { Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED },
    { 0, nullptr },
};

// Constant-initialized: every interpreter that imports the module calls PyInit__karouka with it.
PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_karouka",
    PyDoc_STR("karouka's engine (include/karouka/karouka.h) as a CPython module; see docs/PYTHON.md."),
    sizeof(ModuleState),
    module_methods,
    module_slots,
    module_traverse,
    module_clear,
    module_free,
};

}

PyModuleDef* Karouka::Python::definition()
{
    return &module_def;
}

// The module's one exported symbol (-exported_symbol in src/CMakeLists.txt).
PyMODINIT_FUNC PyInit__karouka();

PyMODINIT_FUNC PyInit__karouka()
{
    return PyModuleDef_Init(&module_def);
}
