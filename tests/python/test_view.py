"""the _karouka extension module (src/python/module.cpp) drives a real system and view.

Run by ./build.sh test with the built module on PYTHONPATH and KR_SOURCE_DIR set
to the checkout: it renders a pages/tests page headless on the software backend, captures a PNG and
checks the page's title against its karouka-check-title meta; checks the module exports only
PyInit__karouka; and imports it in a subinterpreter with its own GIL. Exits 1 on any failure.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import threading

import _karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


def raises(what, call, error=_karouka.Error):
    try:
        call()
    except error as e:
        return e
    check(f"{what} raises {error.__name__}", False)
    return None


source = os.environ["KR_SOURCE_DIR"]
page = os.path.join(source, "pages/tests/external_module.html")
with open(page, encoding="utf-8") as f:
    want_title = re.search(r'<meta name="karouka-check-title" content="([^"]*)">', f.read()).group(1)

# Only PyInit__karouka leaves the image: the engine's C API and LibWeb's C++ stay local.
exports = subprocess.run(["nm", "-gU", _karouka.__file__], check=True, capture_output=True, text=True).stdout.split()
check(f"exports only _PyInit__karouka (got {exports[2::3]})", exports[2::3] == ["_PyInit__karouka"])

raises("System(backend='opengl')", lambda: _karouka.System(backend="opengl"), ValueError)
raises("View()", lambda: _karouka.View(), TypeError)
system = _karouka.System(backend="software")

# One system per process: a second one, from this interpreter or another, is refused.
error = raises("a second System()", lambda: _karouka.System(backend="software"))
check(f"a second System() is KR_ERROR_INVALID_ARGUMENT ({error and error.args})",
      error is not None and error.args[1] == 1)

# The engine runs on the main thread only.
thread_errors = []


def off_main_thread():
    try:
        system.view(64, 64)
    except _karouka.Error as e:
        thread_errors.append(e)


thread = threading.Thread(target=off_main_thread)
thread.start()
thread.join()
check("System.view off the main thread raises Error", len(thread_errors) == 1)

raises("view(0, 0)", lambda: system.view(0, 0))

view = system.view(800, 600)
view.load_file(page)
rendered = 0
for frame in range(1, 31):
    view.advance_time(frame * 16.0)
    rendered += view.render()
print(f"external_module.html: title {view.title!r} after {rendered} rendered frames")
check(f"{rendered} frames rendered", rendered > 0)
check(f"title {view.title!r} matches {want_title!r}", re.search(want_title, view.title) is not None)

with tempfile.TemporaryDirectory() as tmp:
    png = os.path.join(tmp, "external_module.png")
    view.capture_png(png)
    with open(png, "rb") as f:
        head = f.read(24)
    check("capture is a PNG", head[:8] == b"\x89PNG\r\n\x1a\n" and head[12:16] == b"IHDR")
    check(f"capture is 800x600 (got {struct.unpack('>II', head[16:24])})", struct.unpack(">II", head[16:24]) == (800, 600))

view.load_html("<!doctype html><title>from html é</title><p>hi</p>", "file:///karouka-python-test/")
for frame in range(31, 41):
    view.advance_time(frame * 16.0)
    view.render()
check(f"load_html title {view.title!r}", view.title == "from html é")

view.close()
raises("render after close", view.render)

# The module imports in a subinterpreter with its own GIL (concurrent.interpreters makes isolated
# ones), which refuses a module without Py_MOD_PER_INTERPRETER_GIL_SUPPORTED; it gets its own
# module object and types, and the process's one system is still taken.
import _interpreters
from concurrent import interpreters

interp = interpreters.create()
config = _interpreters.get_config(interp.id)
check(f"the subinterpreter has its own GIL and checks extensions ({config})",
      config.gil == "own" and config.check_multi_interp_extensions)
try:
    interp.exec(f"""
import sys
sys.path[:0] = {[os.path.dirname(_karouka.__file__)]!r}
import _karouka
assert _karouka.System.__module__ == "_karouka"
try:
    _karouka.System(backend="software")
except _karouka.Error as e:
    assert e.args[1] == 1, e.args
else:
    raise AssertionError("a second System() in a subinterpreter was not refused")
""")
except interpreters.ExecutionFailed as e:
    check(f"subinterpreter import: {e}", False)
finally:
    interp.close()

del view, system
if failures:
    print(f"test_view: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_view: ok")
