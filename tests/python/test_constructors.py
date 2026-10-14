"""IDL constructors, static members, dictionary arguments, [CreatesPromise] operations,
named getters and [PutForwards] from Python (docs/PYTHON.md § Bindings).

Run by tools/dev.sh python (and check). Loads tests/python/pages/constructors.html headless,
whose Python constructs Event/URL/Blob and the rest and publishes what it saw; then does the
same from the app's interpreter (View.window, whose one open document the objects are made in).
Also checks that _karouka._bindings_report() lists no constructor, static member, named getter or
[CreatesPromise] operation among the unbound. Exits 1 on any failure.
"""

import json
import os
import sys

import _karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


source = os.environ["KR_SOURCE_DIR"]
system = _karouka.System(backend="software")
view = system.view(400, 300)
view.load_file(os.path.join(source, "tests/python/pages/constructors.html"))
clock = 0.0
published = {}
for frame in range(300):
    clock += 16.0
    view.advance_time(clock)
    view.render()
    published = json.loads(view.document.getElementById("results").textContent)
    if published.get("values", {}).get("done") or "main" in published.get("errors", {}):
        break
values = published.get("values", {})
errors = published.get("errors", {})
print(f"test_constructors: page values {values}, errors {errors}", file=sys.stderr)


def expect(name, expected):
    check(f"{name}: {values.get(name)!r} == {expected!r}", values.get(name) == expected)


check(f"the page ran to the end ({errors.get('main')!r})", values.get("done") is True)
expect("Event", ["Event", "ping", True, True, False])
expect("Event defaults", [False, False, False])
expect("dispatched", ["d"])
expect("CustomEvent.detail", 5)
expect("MouseEvent", [7, True])
expect("URL", ["URL", "https://example.com/a/b/c?x=1#h", "1"])
check(f"URL('nope') raises TypeError ({errors.get('URL invalid')!r})", str(errors.get("URL invalid")).startswith("TypeError"))
expect("Blob", ["Blob", 6, "text/plain"])
expect("Blob()", 0)
expect("Blob text", "abcdef")
expect("Text", "t")
expect("DocumentFragment", 0)
expect("AbortController", False)
check(f"Node() raises TypeError ({errors.get('Node()')!r})", str(errors.get("Node()")).startswith("TypeError"))
expect("URL.canParse", [True, False])
expect("URL.parse", "/y")
expect("AbortSignal.abort", True)
expect("Response.json", 1)
expect("Response.text", "body text")
expect("Request.text", "posted")
expect("consumed body", "JSError")
expect("dataset", "x")
check(f"dataset missing raises KeyError ({errors.get('dataset missing')!r})", str(errors.get("dataset missing")).startswith("KeyError"))
expect("forms named", "f")
expect("form named", "q")
expect("children[0]", "d")
expect("classList=", "x y")
expect("style=", "red")

# The app's interpreter: constructors and statics make their objects in View.document's realm.
window = view.window
event = window.Event("app", {"cancelable": True})
check(f"View.window.Event ({event.type!r}, {event.cancelable!r})", event.type == "app" and event.cancelable)
url = window.URL("/p", "https://example.com")
check(f"View.window.URL ({url.href!r})", url.href == "https://example.com/p")
blob = window.Blob(["xyz"])
check(f"View.window.Blob ({blob.size!r})", blob.size == 3)
check("View.window.URL.canParse", window.URL.canParse("https://x"))
promise = window.Response("r").text()
check(f"a [CreatesPromise] operation returns a JSValue ({type(promise).__name__})", type(promise).__name__ == "JSValue")
try:
    window.Event()
    check("Event() with no type raises TypeError", False)
except TypeError:
    pass

report = _karouka._bindings_report()
unbound = report["unbound"]
for marker in ("constructor", "[name]"):
    found = [name for name in unbound if marker in name]
    check(f"no unbound {marker} members ({found})", not found)
for name in ("Response.text/0", "Request.json/0", "HTMLMediaElement.play/0", "Element.scroll/1"):
    check(f"{name} is bound", name not in unbound)
print(f"test_constructors: {report['members']} members, {len(unbound)} unbound", file=sys.stderr)

view.close()
if failures:
    print(f"test_constructors: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_constructors: ok", file=sys.stderr)
