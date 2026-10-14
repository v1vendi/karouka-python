"""the generated bindings (docs/PYTHON.md § Bindings).

Run by ./build.sh test. Two pages, headless on the software backend:
- pages/globals.html lists, from JS, every interface object on window; each must be reachable from
  Python as an attribute of window (the generated type, or an alias such as Image), except the
  ECMAScript built-ins below, which are JS's own.
- pages/wpt_dom.html runs Python ports of WPT dom/ testharness tests in the page's interpreter;
  every one must pass.
It also prints _karouka._bindings_report()'s counts. Exits 1 on any failure.
"""

import os
import sys

import _karouka

# ECMAScript's own constructors (InternalError is LibJS's).
ECMASCRIPT = set("""
AggregateError Array ArrayBuffer AsyncDisposableStack BigInt BigInt64Array BigUint64Array Boolean
DataView Date DisposableStack Error EvalError InternalError FinalizationRegistry Float16Array Float32Array
Float64Array Function Int16Array Int32Array Int8Array Iterator Map Number Object Promise Proxy
RangeError ReferenceError RegExp Set SharedArrayBuffer String SuppressedError Symbol SyntaxError
TypeError URIError Uint16Array Uint32Array Uint8Array Uint8ClampedArray WeakMap WeakRef WeakSet
""".split())

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


source = os.environ["KR_SOURCE_DIR"]
system = _karouka.System(backend="software")


def load(name):
    view = system.view(800, 600)
    view.load_file(os.path.join(source, "tests/python/pages", name))
    for frame in range(30):
        view.advance_time(16.0 * (frame + 1))
        view.render()
    return view


view = load("globals.html")
js_names = set(view.document.getElementById("globals").textContent.split())
check("JS lists interface objects", len(js_names) > 100)
window = view.window
missing = sorted(name for name in js_names - ECMASCRIPT if not isinstance(getattr(window, name, None), type))
check(f"every JS interface object is reachable from Python; missing {missing}", not missing)


def is_type(name):
    try:
        return isinstance(getattr(window, name), type)
    except Exception:  # an attribute whose C++ isn't bound yet raises NotImplementedError
        return False


python_names = {name for name in dir(window) if not name.startswith('__') and is_type(name)}
print(f"interface objects: JS {len(js_names - ECMASCRIPT)}, Python {len(python_names)}; "
      f"Python only: {sorted(python_names - js_names)}")
view.close()

view = load("wpt_dom.html")
results = view.document.getElementById("results").textContent.splitlines()
check("the WPT ports ran", len(results) >= 9)
for line in results:
    print(line)
    check(line, line.startswith("PASS "))
view.close()

report = _karouka._bindings_report()
print(f"bindings: {report['interfaces']} interfaces, {report['members']} members, "
      f"{len(report['unbound'])} whose C++ didn't resolve")

if failures:
    print(f"test_bindings: {len(failures)} failure(s)")
    sys.exit(1)
print("test_bindings: ok")
