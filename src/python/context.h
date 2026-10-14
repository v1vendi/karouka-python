/*
 * A context: one (document, interpreter) pair, and the scopes that run its Python
 * (docs/PYTHON.md § Object lifetime). Shared by dom.cpp (scripts, proxies) and js.cpp (JS values,
 * functions, the asyncio loop).
 */
#pragma once

#include "bindings.h"

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Platform/Timer.h>

namespace Karouka::Python {

// Python calls on the stack that entered through the engine (scripts, callbacks, proxy calls).
extern int g_call_depth;

// Cleared when its context is discarded; a listener function outliving its context does nothing.
struct ContextHandle : RefCounted<ContextHandle> {
    Context* context { nullptr };
};

struct Context {
    Context() { handle->context = this; }

    kr_view* view { nullptr };
    GC::Root<Web::DOM::Document> document;
    PyInterpreterState* interpreter { nullptr };
    // The document's own interpreter's thread state, or the app interpreter's main-thread one.
    PyThreadState* thread_state { nullptr };
    bool owns_interpreter { false };
    PyObject* module { nullptr }; // _karouka in that interpreter: the proxy types
    HashMap<GC::Cell*, Proxy*> proxies;
    // Each Python callable passed as a listener (a strong reference) and its one JS function.
    HashMap<PyObject*, GC::Root<JS::NativeFunction>> functions;
    // Each Python callable passed to JS as a value (a strong reference) and its one JS function,
    // and back: the callable a JS function stands for (borrowed from `callables`). js.cpp.
    HashMap<PyObject*, GC::Root<JS::NativeFunction>> callables;
    HashMap<JS::Object const*, PyObject*> callable_of_function;
    // The document interpreter's asyncio loop (js.cpp), its host-clock timer and whether a task to
    // run its ready callbacks is queued.
    PyObject* loop { nullptr };
    GC::Root<Web::Platform::Timer> loop_timer;
    bool loop_task_queued { false };
    NonnullRefPtr<ContextHandle> handle { adopt_ref(*new ContextHandle) };
    // Python of this context running now (scripts and listeners); a discard waits for 0.
    int running { 0 };
    bool discard_pending { false };
};

// ---- Interpreter switching ----

// Makes `thread_state` current for a scope, releasing the current interpreter's GIL and taking
// thread_state's, and back. Nothing happens when it is current already.
class EnterInterpreter {
public:
    explicit EnterInterpreter(PyThreadState* thread_state)
    {
        PyThreadState* current = PyThreadState_GetUnchecked();
        if (current == thread_state)
            return;
        m_outer = current ? PyEval_SaveThread() : nullptr;
        PyEval_RestoreThread(thread_state);
        m_swapped = true;
    }
    ~EnterInterpreter()
    {
        if (!m_swapped)
            return;
        PyEval_SaveThread();
        if (m_outer)
            PyEval_RestoreThread(m_outer);
    }

private:
    PyThreadState* m_outer { nullptr };
    bool m_swapped { false };
};

// Python of `context` runs for a scope: entered, counted, and a discard that came meanwhile
// happens once the outermost run has left the interpreter.
class RunInContext {
public:
    explicit RunInContext(Context& context)
        : m_context(context)
        , m_enter(context.thread_state)
    {
        ++m_context.running;
        ++g_call_depth;
    }
    ~RunInContext()
    {
        --g_call_depth;
        --m_context.running;
    }

private:
    Context& m_context;
    EnterInterpreter m_enter;
};

// Ends a discard that waited for the context's Python to return.
void after_run(Context&);
// Logs the raised Python exception's traceback and fires an ErrorEvent at the context's window.
void report_exception(Context&);

// ---- dom.cpp ----

void proxy_dealloc(PyObject*);
// The generated type of `cell`'s own C++ class, if it has one.
Optional<DomType> dom_type_of_class(GC::Cell const&);
// The context of the document interpreter that is current, or nullptr.
Context* current_document_context();
ModuleState& state_of_context(Context&);

// ---- js.cpp ----

// In a new document interpreter (current): its asyncio loop, the `js` global and module.
bool js_set_up(Context&, PyObject* globals);
// Window.fetch(input, init=None), through the global's JS fetch.
PyObject* window_fetch(PyObject* self, PyObject* const* args, Py_ssize_t nargs);
// Under the context's interpreter, as it is discarded: the loop, its timer and the JS functions.
void js_discard(Context&);

}
