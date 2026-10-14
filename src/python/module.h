/*
 * What module.cpp (System, View), dom.cpp (page scripts and the DOM proxies) and app.mm (App,
 * AppWindow: the app shell) share: the per-module state and the module's definition. See
 * docs/PYTHON.md.
 */
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <karouka/karouka.h>

namespace Karouka::Python {

struct ModuleState {
    PyTypeObject* system_type;
    PyTypeObject* view_type;
    PyObject* error;
    PyTypeObject** dom_types; // one per generated interface type, indexed by DomType (bindings.h)
    PyTypeObject* console_type;
    PyObject* stale_object_error;
    PyObject* dom_exception;
    PyTypeObject* js_value_type;
    PyTypeObject* js_bound_method_type;
    PyObject* js_error;
    PyTypeObject* app_type;
    PyTypeObject* window_type;
};

PyModuleDef* definition();
ModuleState* state_of(PyObject* module);
// The state of the module that defined `type` (or its base), or nullptr with an exception set.
ModuleState* state_of_type(PyTypeObject* type);
// Raises _karouka.Error(message, code) for the module of `type`; returns nullptr.
PyObject* raise_error(PyTypeObject* type, char const* message, int code);
// The engine runs on the main thread only; false with Error raised elsewhere. On the main thread it
// first destroys the Views and Systems whose last reference went on another thread.
bool on_main_thread(PyTypeObject* type);

// The kr_view of a _karouka.View, nullptr once it is closed (no exception set). Main thread.
kr_view* view_handle(PyObject* view);

// ---- dom.cpp ----

// Adds the DOM types, Console, StaleObjectError and DOMException to the module.
int dom_exec(PyObject* module, ModuleState&);
int dom_traverse(ModuleState&, visitproc, void*);
void dom_clear(ModuleState&);

// A view's Python side: its text/python handler, its app root and the contexts of its documents.
struct ViewHooks;
// Registers text/python and the active-document handler on the view.
ViewHooks* dom_attach_view(kr_view*);
// Before kr_view_destroy: discards the contexts of the view's documents and the handlers.
void dom_detach_view(ViewHooks*);
// View.load_file(path): the app root becomes the file's directory unless one is set.
void dom_note_load_file(ViewHooks*, char const* path);
PyObject* dom_app_root(ViewHooks*);
int dom_set_app_root(ViewHooks*, PyObject* value);
// View.document and View.window: proxies, in the calling interpreter, of the active document.
PyObject* dom_view_document(PyObject* view_object, ViewHooks*, bool window);
// Whether the engine is inside a call that runs Python (a script, a listener, a proxy call) or a
// discard is releasing a context's Python references: no View or System may be destroyed now.
bool dom_busy();
// _karouka._debug_counts(): {"contexts", "interpreters", "proxies", "functions"} across the process.
PyObject* dom_debug_counts(PyObject* module, PyObject*);

// ---- js.cpp ----

// Adds JSValue, JSError and the asyncio loop's host functions to the module.
int js_exec(PyObject* module, ModuleState&);
int js_traverse(ModuleState&, visitproc, void*);
void js_clear(ModuleState&);

// ---- app.mm ----

// Adds App and AppWindow to the module.
int app_exec(PyObject* module, ModuleState&);
int app_traverse(ModuleState&, visitproc, void*);
void app_clear(ModuleState&);

}
