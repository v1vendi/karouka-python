# Python integration

karouka as a Python library for desktop apps: a page's UI is HTML/CSS, and its logic is Python in
`<script type="text/python">`, with `window` and `document` as globals bound straight to LibWeb's
C++ DOM objects. No JS shim and no JSON in between: a Python call on a node calls the same C++
method the JS bindings call.

Status: the extension module is built (`_karouka`, `src/python/module.cpp`), a thin wrapper over
the C API: `System(backend=...)`, `System.view(w, h)`, `System.memory_stats()`, `View.load_file`,
`load_html`, `reload`, `inject_input`, `advance_time`, `render`, `capture_png`, `close`, `title`,
`app_root`, and `document`/`window` (proxies of the active document in the calling interpreter),
with failures raised as `_karouka.Error(message, kr_result)`. Page scripts run
(`src/python/dom.cpp`): `<script type="text/python">` executes in its document's subinterpreter
against proxy types generated from LibWeb's IDL, one per interface the pruned build keeps (474),
with the interface objects JS has on window as attributes of `window` and as page globals. JS
values reach Python as `JSValue` proxies and Python callables reach JS as functions
(`src/python/js.cpp`), and each document interpreter runs an asyncio loop on karouka's host clock,
so page Python awaits `asyncio.sleep` and Promises (`await window.fetch(url)`). The app
shell is built (`src/python/app.mm`): `App(backend=, headless=)`, `App.open(path, title=, size=)`
returning an `AppWindow`, `App.run(on_frame=None)` and `App.stop()`, and the `karouka` package
(`src/python/karouka/`, copied next to the module by the build) with `python -m karouka page.html`.
`./build.sh test` runs `tests/python/` (`test_dom.py` drives
`tests/python/pages/dom.html`, `test_app.py` drives `App` headless, `test_bindings.py` compares
window's interface objects with JS's and runs Python ports of WPT dom/ tests, `test_async.py` drives
`tests/python/pages/async.html`: asyncio on the host clock, fetch, Promises and JS values, and
`test_caller_realm.py` calls every bound `[NeedsCallerRealm]` member from Python, and
`test_constructors.py` constructs `Event`/`URL`/`Blob` and calls statics, `[CreatesPromise]`
operations, named getters and `[PutForwards]` setters).
`./build.sh wheel` builds a pip-installable wheel (§ Wheel; building from source is in `docs/BUILDING.md`).

```html
<button id="go">Go</button><p id="out"></p>
<script type="text/python">
def on_click(e):
    e.preventDefault()
    document.getElementById("out").textContent = f"{window.innerWidth}px"
document.getElementById("go").addEventListener("click", on_click)
</script>
```

```python
import karouka
app = karouka.App()
app.open("ui/index.html", title="Demo", size=(900, 600))
app.run()          # or: python -m karouka ui/index.html
```

## Decisions

| Question | Decision |
| --- | --- |
| Who owns the interpreter | The Python app. `python app.py` imports `karouka`, which runs the engine in-process. libkarouka does not embed libpython; a native host that wants Python pages is out of scope. |
| Isolation | One interpreter per document: each document gets its own subinterpreter (PEP 684, `Py_NewInterpreterFromConfig` with its own GIL), created at its first Python script and finalized when the document is discarded. All Python scripts of one document share its `__main__` globals, as JS classic scripts share a realm. |
| Names | Exact WebIDL names: `getElementById`, `textContent`, `addEventListener`. MDN documents the API. No snake_case aliases. |
| Trust | Page Python has full OS access. Python runs only in documents loaded from `file:` URLs under the app's root directory: `View.app_root`, which `View.load_file` sets to the file's directory when unset. Elsewhere a `text/python` script fires `error` at its element and runs nothing. |
| Frames | karouka drops iframe support, so a view has one document and one Python interpreter at a time. |

## Architecture

```
python app.py  /  python -m karouka index.html
  └─ karouka/ (package)                App, AppWindow, System, View re-exported; __main__: python -m karouka
     └─ _karouka.cpython-3XX-darwin.so    the whole engine: karouka's objects + the Lagom objects, one image
       ├─ app shell (app.mm)          NSApplication, NSWindow + CAMetalLayer, display link, NSEvent → kr_input_event
       ├─ script hook                 <script type="text/python"> → exec in the document's interpreter
       └─ bindings                    one Python type per IDL interface, calling LibWeb's C++ directly
```

### Why the module is the engine

LibWeb is linked statically into `libkarouka.dylib`, which exports only `_kr_*`
(`src/CMakeLists.txt`). The extension can't reach LibWeb's C++ through libkarouka, so it is built
in the CMake hook `patches/ladybird.patch` adds to Ladybird, from the same objects (`karouka_engine` and the Lagom object
file list) and exports only `PyInit__karouka`. It is compiled with Ladybird's toolchain and flags,
against one CPython 3.14 interpreter with the GIL (the `KAROUKA_PYTHON` cache variable in
`src/CMakeLists.txt`, which `build.sh` sets from `KAROUKA_PYTHON`, default `python3.14` on `PATH`;
its `EXT_SUFFIX` names the module and `lib/_karouka.python` records its path). A build dir builds
the module for one interpreter at a time. It links with
`-undefined dynamic_lookup`, so the interpreter that loads it supplies the Python symbols. A
post-build step fails the link when the module exports anything else, or leaves any symbol but
Python's (and AK's weak `_ak_assertion_handler`) to be looked up at load time: dynamic_lookup would
otherwise turn a missing engine symbol into an import error. The module is about as large as
`libkarouka.dylib` (58.3 MB unstripped).

The engine is process-wide: one `System` per process, from whichever interpreter (a second raises
`Error`, as `kr_system_create2` refuses it), and every call on the main thread. With a GIL per
interpreter two threads can run Python at once, so each entry point checks `pthread_main_np()`
and raises `Error` on any other thread. Destruction can't raise: a `View` or `System` whose last
reference goes on another thread, or on the main thread while the engine runs Python (a script, a
listener, a proxy call), is queued (`src/python/module.cpp`), and the main thread destroys the
queue in order (a view before its system) from a pending call (`Py_AddPendingCall`: the main
interpreter's main thread, between bytecodes) or at its next entry point.
`_karouka._debug_deferred()` reports the queue (`tests/python/test_threads.py`).

### Wheel

`./build.sh wheel` runs `tools/wheel.py` over the build and writes `dist/`
`karouka-<version>-cp314-cp314-macosx_14_0_arm64.whl` (the version is `karouka.__version__`; the
CPython tag comes from the module's `EXT_SUFFIX`, the platform tag from the highest
`LC_BUILD_VERSION` minos of what it packs). It holds `_karouka.cpython-314-darwin.so` at the top
level, stripped of its local symbols (`strip -x`; 58 MB to 38 MB), the `karouka` package, Ladybird's
`Base/res` as `karouka/res/` (less `fonts/`, which only Ladybird's layout-test mode reads), and in `karouka/.dylibs/` the 22 dylibs the module loads (the vcpkg
ones, Skia's, and theirs in turn, resolved as dyld would). The module's only rpath is
`@loader_path/karouka/.dylibs`, each dylib's `@loader_path` and its id `@rpath/NAME`; each file is
ad-hoc re-signed. `tools/wheel.py` fails when a load command names the checkout or the Ladybird tree,
an rpath is absolute, a dependency is neither a system library nor packed, or a file isn't arm64
only. The module still holds source paths as strings (assertion messages, and
`KAROUKA_DEFAULT_RESOURCE_ROOT`, the build's `Base/res`): `karouka/__init__.py` sets
`KR_RESOURCE_ROOT` to its `res/` (unless already set) before any `System` exists, so the engine
reads the packaged resources; importing `_karouka` alone skips that.

`./build.sh smoke` installs the wheel with pip into a fresh venv of the interpreter the module was
built for, outside the checkout, and runs `python -I -m karouka --software --headless` from it under
`sandbox-exec` with the checkout and the Ladybird tree unreadable: on `pages/demo/demo_page.html`,
which must render, and on a page whose `text/python` writes a file.

### App shell

`src/python/app.mm` (Objective-C++, ARC). The App owns the process's `System` (`app.system`) and
its open windows (`app.windows`). `app.open(path, title=None, size=(1024, 768))` makes a View of
`size` points times the screen's `backingScaleFactor` in device pixels, loads the file, and shows it
in an NSWindow whose content view is a CAMetalLayer pane; it returns an `AppWindow` (`view`, `title`,
`size`, `scale`, `closed`, `resize(w, h)`, `close()`). The type is `AppWindow` because
`_karouka.Window` is the IDL `Window`'s proxy.

`app.run()` runs NSApplication with the GIL released. Each window's display link (the vsync of its
screen, 30 to 60 Hz) takes the GIL and runs one frame: `kr_view_advance_time`, `kr_view_render`, and,
when the frame changed, a blit of the view's IOSurface into the layer's next drawable, waited for
before the next render rewrites the surface. The window's title is `document.title`, or open()'s
title (default: the file's name) while the document has none. A resize, or a move to a screen of
another scale, resizes the layer and the view to the new device size. NSEvents become
`kr_input_event`s through `src/viewer/EventInput.h`, the viewer's mapping; key-window changes call
`kr_view_set_focused`. Closing a window (its close button, Cmd+W, `window.close()`) closes its View;
closing the last one, Cmd+Q (which closes them all) or `app.stop()` returns from `run()`, and the
process stays the Python app's. `run(on_frame)` calls `on_frame(window, now_ms)` after each window's
frame; what it raises, or a KeyboardInterrupt, stops `run()`, which raises it.

`App(headless=True)` has no NSApplication and nothing on screen: `run()` steps every open window on a
60 Hz engine clock, back to back. `tests/python/test_app.py` drives it on the software backend, and
`python -m karouka --headless --frames N --capture out.png page.html` runs a page that way.

### Bindings

- **Plain CPython C API, no nanobind/pybind11.** Ladybird compiles with `-fno-exceptions`
  (`Meta/CMake/compile_options.cmake`); generated C API glue needs no exceptions.
- **Generated from LibWeb's IDL.** `src/python/gen/generate_python_bindings.py` imports the JS
  bindings generator's IDL parser, pruning and C++ type mapping
  (`third_party/ladybird/Meta/Generators/libweb_bindings/`, `Meta/Utils/webidl_parser.py`; no
  Ladybird patch) and reads the IDL files `idl_files.cmake` lists, less LibWeb's pruned families,
  as LibWeb's own bindings do (`src/CMakeLists.txt` derives both lists). It writes
  `Build/release/karouka/python_bindings/`: `PythonBindings.h` (`DomType`, one enumerator per
  interface, and `DomTypeOf<C++ class>`), eight `PythonBindings<N>.cpp` chunks, the type table
  `PythonBindingsTable.cpp`, and `PythonBindings.report`, every member it left out and why. Each
  attribute, operation (overloads by argument count; overloads of the same count by their
  arguments' Python kinds, such as `postMessage(message, [transfer])` against
  `postMessage(message, {options})`; optional trailing arguments it can't convert get the C++
  default JS would pass), constant, indexed getter (`len`, `[i]`, iteration), named getter
  (`dataset["fooBar"]`, `document.forms["f"]`: `KeyError` for an unsupported name; any other key
  goes to the indexed getter) and stringifier attribute (`str()`) calls the C++ the JS glue calls,
  trying the same forms (no realm, the global object, the realm, the VM) with `if constexpr`;
  [Reflect] reads and writes the content attribute, and a `[PutForwards]` attribute's setter sets
  the forwarded attribute (`el.classList = "a b"`, `el.style = "color: red"`). The value
  conversions are C++ templates over the C++ type (`src/python/bindings.h`), so every chunk
  compiles whatever its C++ looks like: a member whose call resolves to nothing raises
  `NotImplementedError`, and `_karouka._bindings_report()` lists those (94 of 4041 members:
  members LibWeb leaves unimplemented, and `[Default] toJSON()`, which the JS glue builds from the
  attributes).
  - **Constructors** are the types' `tp_new`: `Event("ping", {"bubbles": True})`, `URL(u, base)`,
    `Blob(["ab", blob], {"type": "text/plain"})` call the static factory the JS glue calls
    (`construct_impl`, `create_for_constructor`, `create`; an `Event`'s with its time stamp) and
    return the new object's proxy. A type without an IDL constructor can't be instantiated
    (`Node()` raises `TypeError`), nor can one with `[HTMLConstructor]` (custom elements defined in
    Python). **Static operations** are class methods (`URL.canParse(u)`,
    `AbortSignal.abort()`; not `[ImplementedInBindings]` ones such as `Document.parseHTMLUnsafe`), except one whose name an instance
    operation also has (`Response.json(data)`: Python has one attribute per name). Both run in the
    current document interpreter's context or, from the app's interpreter, in its one open
    document's (`View.window.Event(...)`; with several open, `TypeError`), so a
    `[NeedsCallerRealm]` static gets the document's realm.
  - **Dictionaries** (arguments): a `dict` (or `None`, every member's default) becomes the JS object
    § JS values describes, which LibWeb's own generated converter
    (`convert_to_idl_value_for_<dictionary>`) turns into the C++ struct: defaults, required members
    and nested types as JS gets them. A union argument converts when one of its alternatives does
    (BlobPart's `USVString` and `Blob`; a buffer source raises `TypeError`).
  - **`[CreatesPromise]` operations** (`Body.text()`/`json()`, `HTMLMediaElement.play()`,
    `Clipboard` and `AudioContext` methods) get the promise the glue makes, and return it as an
    awaitable `JSValue`; what their C++ throws rejects it, as in JS.

  Not generated yet, so left out (`PythonBindings.report`): callback interfaces other than
  `EventListener` (`NodeFilter`), buffer sources and records, dictionary return values and
  attributes, static attributes, pair iterables, maplike/setlike, named setters and deleters,
  indexed setters, [Reflect] of enumerated, URL, nullable and element types, and namespaces
  (`CSS`). A variadic of JS values or GC pointers is a `GC::RootVector`, as LibWeb takes it, and a
  `sequence<object>` (a transfer list) converts to one; glue that returns `JS::ThrowCompletionOr`
  (`customElements.define`) raises `JSError` for what it throws. `setTimeout`/`setInterval` are
  bound, but their `TimerHandler` union takes no Python callable yet: asyncio does timers.
- **Overrides** (`s_overrides` in `src/python/dom.cpp`) replace or add members by name:
  `EventTarget.addEventListener`/`removeEventListener` (a Python callable as the listener),
  `Window`'s interface objects (`window.HTMLElement`, aliases such as `window.Image`) and `__dir__`,
  `CSSStyleDeclaration`'s camel-cased CSS properties (`style.backgroundColor`), and `Window.fetch`,
  which calls the global's JS `fetch` (LibWeb's own binding): `Fetch::fetch` reads its caller's
  function from the running execution context, which a Python call has none of.
- **Caller realm.** A Python call runs in the document realm's execution context, entered with
  callbacks enabled, so the entry and incumbent realms are the document's and `[NeedsCallerRealm]`
  members get that realm; the context has no function and no script. `Fetch::fetch` is the only
  LibWeb code that reads the running function (`FetchMethod.cpp`), hence the override.
  `_karouka._bindings_report()["caller_realm"]` lists the bound `[NeedsCallerRealm]` members, and
  `tests/python/test_caller_realm.py` fails when its page doesn't call one of them (the
  `postMessage` of `Worker` and `DedicatedWorkerGlobalScope` excepted: no Python reaches a worker).
- **Multi-phase init, heap types, per-module state**, as subinterpreters with their own GIL
  require: no static `PyTypeObject`, no process-global Python state.
- **Conversions:**

  | IDL | Python |
  | --- | --- |
  | `DOMString`, `USVString` | `str` |
  | integer types / `double` | `int` / `float` |
  | `boolean` | `bool` |
  | nullable | `None` |
  | `sequence<T>` / dictionary | `list` / `dict` |
  | enum | `str` |
  | union | runtime type dispatch |
  | interface | its proxy type |
  | callback (`EventListener`, timers, `MutationCallback`) | any Python callable |
  | `any` / `object` | `JSValue` proxy |
  | `Promise<T>` | awaitable |

  Built today: strings, numbers, booleans, nullable, enums, sequences, unions (by the Python
  value's type), interfaces, callback functions (`onclick`, `queueMicrotask`; any callable),
  `any`/`object` and `Promise` (§ JS values), and `EventListener` callbacks (an override). A proxy type is a heap
  type named `_karouka.<IDL name>`, subclassing its IDL parent (`HTMLDivElement` → `HTMLElement` →
  `Element` → `Node` → `EventTarget`), and an object's proxy is the generated type of its C++
  class (`GC::Cell::class_name()`), or the IDL type the member returns when that class has none.
  A `WindowProxy` is its `Window`. Each interpreter builds all 474 types when it imports
  `_karouka` (about 11 ms for a new subinterpreter on a loaded machine).

- **What the JS glue does, the Python glue does too:** enter the document realm's execution
  context (`HTML::TemporaryExecutionContext`), push and pop the `[CEReactions]` stack, map
  `WebIDL::ExceptionOr` errors to Python exceptions (`_karouka.DOMException` with its `name`, such
  as `HierarchyRequestError`; `TypeError`; `ValueError` for `RangeError`), and run a microtask
  checkpoint when the outermost Python call returns to no running script ("clean up after running
  script").

### Callbacks and events

A Python callable passed where IDL wants a callback is wrapped in a `JS::NativeFunction` and then
a `WebIDL::CallbackType` (for listeners, an `IDLEventListener`). No LibWeb change is needed. Each
(context, callable) pair has one function, so `removeEventListener` with the same callable finds
the listener; the context keeps the function and the callable until its document is discarded.
Dispatch is synchronous on the engine thread, so a handler can change the DOM and
`preventDefault()` works. JS on the same page keeps working and sees the same listeners. A
listener's function hands it the event's proxy and logs what it raises (with its traceback); other
callback-typed members (`onclick`, `queueMicrotask`) take the general function of § JS values,
which converts every argument and throws what the callable raises into JS, where LibWeb reports
it.

### JS values

`src/python/js.cpp`. A JS value in Python is `None` (undefined and null), `bool`, `int` (an
integral Number up to 2^53, or a BigInt), `float`, `str`, a DOM object's proxy (a wrapper's
`Wrappable` with a generated type; a `WindowProxy` is its `Window`), the Python callable a JS
function was made from, or else a `_karouka.JSValue`: attributes and items are the object's
properties (a missing one raises `AttributeError`, `KeyError` or `IndexError`), `len()` is its
`length`, iteration goes by index, calling it calls it (a function read as an attribute is bound to
its object, as a `JSBoundMethod`, which is the bare function again when passed back to JS), `.new(*args)` constructs, and `await` awaits it as JS's `await`
would. A JSValue is a proxy like a DOM one: rooted, one per object per context, stale once its
document is discarded.

A Python value in JS: `None` is null (a Python function returning `None` returns undefined),
`bool`, `int` and `float` are Numbers, `str` a string, a proxy its JS object (a `Window` its
`WindowProxy`), a dict a new object and a list or tuple a new array (copies, 64 levels at most), an
exception an `Error` whose message is `Type: message`, a coroutine or other awaitable a Promise the
loop settles, and any other callable a function: one `JS::NativeFunction` per (context, callable),
which converts its arguments to Python and its result back and throws what the callable raises.

JS's global object is `js` in page Python, and the module `js` (`from js import fetch`); setting
`js.name = f` makes `f` a JS global. A JS exception, or a rejected Promise that Python awaited,
raises `_karouka.JSError`, whose `str()` is the thrown value's `String()` (`TypeError: Failed to
fetch`) and whose `.value` is the thrown value; a `JSError` thrown back into JS throws that value.

### asyncio

Each document interpreter has one asyncio loop (`PageEventLoop`, Python source in `js.cpp`), set
as the interpreter's running loop for good: `asyncio.get_running_loop()` and `create_task` work in
scripts and callbacks, and `asyncio.run()`, `run_forever()` and `run_until_complete()` raise. Its
`call_soon` callbacks run as a batch in one LibWeb task (`HTML::queue_a_task` on the document,
followed by its microtask checkpoint); its `call_later`/`call_at` timers share one
`Platform::Timer`, which runs on karouka's host clock, set to the earliest deadline; its `time()`
is LibWeb's clock (`HighResolutionTime::unsafe_shared_current_time`, the host clock's time), in
seconds. `kr_view_advance_time` steps the clock to each timer deadline, so `await
asyncio.sleep(1)` resumes at exactly its deadline's host time, whatever the frame step
(`tests/python/test_async.py`). Awaiting a JSValue resolves it as a Promise and settles a Future of
the running loop from the Promise's reactions. Exceptions nobody retrieved go to the log and fire
an `ErrorEvent` at the window, as a listener's do. The app's own interpreter has no page loop:
awaiting a JSValue there raises `RuntimeError` unless the app runs a loop of its own, and an
awaitable passed to JS from there raises `TypeError`.

### Object lifetime

- A proxy belongs to a context: one (document, interpreter) pair, the document's own interpreter
  or the app's (through `View.document`). It holds a `GC::Root` to its cell, and the context's
  weak cache keyed by the `Cell*` gives the same proxy for the same node.
- A cycle across the two heaps leaks until its document dies. Example: a closure captures an
  element, and that element's listener holds the closure.
- Discarding a document (another document becoming the view's active one, through
  `Karouka::set_active_document_handler`, or the view closing) finalizes its interpreter and drops
  every root of its contexts. Proxies the app's own interpreter still holds raise
  `StaleObjectError`. At the main interpreter's exit an `atexit` hook ends every document
  interpreter first.

### Scripts

- **The Ladybird patch** (`Web::Embedder::set_script_type_handler`, `Embedder/ScriptTypes`):
  "prepare the script element" accepts the embedder's script types per document. `text/python`
  follows classic-script ordering (parser-blocking, `defer`, `async`), fetches `src=` through the
  normal loader (decoded as UTF-8), and sets `document.currentScript`. The engine keeps each view's
  handlers (`src/engine/ScriptTypes.h`): the module registers with
  `Karouka::set_script_type_handler(view, "text/python", handler)`, whose handler gets the
  `HTMLScriptElement` and the `EmbedderScript` (UTF-8 source, URL, start line) and runs inside the
  document's realm, a microtask checkpoint following. Returning false fires `error` at the element
  in place of `load`. C hosts reach the same hook through `kr_view_set_script_type_callback`
  (`tests/script_type_test.c`).
- **Namespace:** `window`, `document` and `js` (§ JS values) are globals, plus `console` (`log`, `info`, `debug`,
  `warn`, `error`: one line through `kr_host_log`, category `python`). `document.currentScript` is
  the running `text/python` element.
- **Imports:** `sys.path` starts with the page's directory, so `import widgets` loads
  `widgets.py` next to the page.
- **Errors:** an uncaught exception's traceback goes to `kr_host_log` (category `python`) and
  fires an `ErrorEvent` on `window` whose message is `Type: message`; the script still fires
  `load`, as a classic script that throws does.

### Threads

Everything runs on the macOS main thread, which also drives the engine. Python callbacks and the
asyncio loop's callbacks run inside `kr_view_advance_time`.

## Not in scope

Python in iframes, workers or remote pages. A native (C) host running Python pages. Windows and
Linux: karouka is macOS-only.
