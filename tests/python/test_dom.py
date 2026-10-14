"""<script type="text/python"> in a per-document subinterpreter, against LibWeb's DOM.

Run by tools/dev.sh python (and check). Loads tests/python/pages/dom.html headless on the software
backend: its Python builds a list, listens to clicks next to JS listeners and raises from a script
and from a listener (pages/dom.html says what it checks itself). From the app's own interpreter
this test reads the page through View.document, injects a click with View.inject_input
(kr_view_inject_input2) and checks that preventDefault took effect, that a removed listener never
ran, that both exceptions reached window.onerror, that Python is refused outside the app root, and
that a reload ends the old document's interpreter: its proxies raise StaleObjectError, and 50
reloads leave one interpreter and no growth beyond noise. Exits 1 on any failure.
"""

import os
import re
import sys
import tempfile

import _karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


source = os.environ["KR_SOURCE_DIR"]
page = os.path.join(source, "tests/python/pages/dom.html")
with open(page, encoding="utf-8") as f:
    want_title = re.search(r'<meta name="karouka-check-title" content="([^"]*)">', f.read()).group(1)

clock = [0.0]


def pump(view, frames):
    for _ in range(frames):
        clock[0] += 16.0
        view.advance_time(clock[0])
        view.render()


def log_of(view):
    return view.document.getElementById("log").textContent


system = _karouka.System(backend="software")
view = system.view(800, 600)
check("app_root is None before a load", view.app_root is None)
view.load_file(page)
check(f"load_file set app_root to the page's directory ({view.app_root})",
      view.app_root == os.path.realpath(os.path.dirname(page)))
pump(view, 30)

check(f"title {view.title!r} matches {want_title!r}", re.search(want_title, view.title) is not None)
document = view.document
check("View.document is a Document proxy", isinstance(document, _karouka.Document))
check("one proxy per node", document.body is view.document.body and isinstance(document.body, _karouka.HTMLElement))
check("View.window is a Window proxy", isinstance(view.window, _karouka.Window) and view.window.document is document)
items = [node.textContent for node in document.querySelectorAll("#list li")]
check(f"Python edited the list ({items})", items == ["first", "item 0", "item 1", "item 2"])
check("Python set a class and data attribute",
      document.querySelector("li.n1") is not None and document.querySelector("li.n1").getAttribute("data-index") == "1")
check("Python set an inline style", "color" in document.querySelector("#list li:last-child").style.cssText)
log = log_of(view)
check(f"the script's exception reached window.onerror ({log!r})", "onerror:" in log and "script boom" in log)
check(f"only that one error so far ({log!r})", log.count("onerror:") == 1)
counts = _karouka._debug_counts()
check(f"one document interpreter ({counts})", counts["interpreters"] == 1)

# A click on #go, through kr_view_inject_input2: JS before, Python (preventDefault), JS after.
view.inject_input("mouse_move", 50, 50)
view.inject_input("mouse_down", 50, 50)
view.inject_input("mouse_up", 50, 50)
pump(view, 10)
log = log_of(view)
print(f"log after the click: {log!r}")
check("JS listener before Python's saw no preventDefault", "js-before:false;" in log)
check("the Python listener ran with a MouseEvent", "py:click:MouseEvent:True;" in log)
check("JS listener after Python's saw preventDefault", "js-after:true;" in log)
check("preventDefault kept the link from navigating", "hash:;" in log and "#navigated" not in log)
check("the removed listener never ran", "removed-listener-ran" not in log)

# The app's interpreter calls into the page: click() dispatches to the Python listener that raises.
document.getElementById("boom").click()
log = log_of(view)
check(f"the listener's exception reached window.onerror ({log!r})", "listener boom" in log and log.count("onerror:") == 2)

# DOM errors become Python exceptions.
try:
    document.body.appendChild(document)
except _karouka.DOMException as e:
    check(f"appendChild(document) is a HierarchyRequestError ({e.name})", e.name == "HierarchyRequestError")
else:
    check("appendChild(document) raises DOMException", False)
try:
    document.querySelector("[")
except _karouka.DOMException as e:
    check(f"a bad selector is a SyntaxError ({e.name})", e.name == "SyntaxError")
else:
    check("a bad selector raises DOMException", False)

# A reload ends the old document's interpreter; the app's proxies of it go stale.
old_body = document.body
view.reload()
pump(view, 30)
try:
    old_body.textContent
except _karouka.StaleObjectError:
    pass
else:
    check("an old document's proxy raises StaleObjectError", False)
check(f"reloaded title {view.title!r}", re.search(want_title, view.title) is not None)
counts = _karouka._debug_counts()
check(f"one document interpreter after the reload ({counts})", counts["interpreters"] == 1)

# 50 more reloads after a warm-up: still one interpreter, and memory flat but for noise. The
# first ~30 reloads raise the allocators' high-water mark by ~13 MB with or without Python (the
# same page with Python refused grows alike), so the baseline is taken after 30.
before = None
for i in range(80):
    view.reload()
    pump(view, 12)
    if i == 29:
        before = system.memory_stats()
after = system.memory_stats()
counts = _karouka._debug_counts()
check(f"one document interpreter after 80 reloads ({counts})", counts["interpreters"] == 1)
mb = 1024 * 1024
growth = {key: (after[key] - before[key]) / mb for key in ("footprint_bytes", "kmalloc_bytes", "gc_heap_bytes", "malloc_zone_bytes")}
print("reloads 30 -> 80: " + ", ".join(f"{key} {before[key] / mb:.1f} -> {after[key] / mb:.1f} MB" for key in growth))
check(f"footprint grew under 16 MB over 50 reloads ({growth['footprint_bytes']:.1f} MB)", growth["footprint_bytes"] < 16)

# Python runs only in file: documents under the app root.
with tempfile.TemporaryDirectory() as elsewhere:
    other = system.view(800, 600)
    other.app_root = elsewhere
    other.load_file(page)
    pump(other, 30)
    check(f"outside the app root no Python runs (title {other.title!r})", other.title == "dom: pending")
    other.close()

view.close()
try:
    old_body.textContent
except _karouka.StaleObjectError:
    pass
counts = _karouka._debug_counts()
check(f"closing the view ends every context ({counts})", counts == {"contexts": 0, "interpreters": 0, "proxies": 0, "functions": 0})

del view, system, document, old_body
if failures:
    print(f"test_dom: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_dom: ok")
