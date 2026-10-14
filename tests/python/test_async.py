"""asyncio on the host clock, Promises and JS values in page Python.

Run by tools/dev.sh python (and check). Loads tests/python/pages/async.html headless on the
software backend and steps the host clock 16 ms a frame (kr_view_advance_time). The page's Python
sleeps (asyncio.sleep(1), then four sleeps of 0.25 s), awaits fetch() of a file: URL through the
generated window.fetch and through JS's global, awaits a missing file's and Promise.reject()'s
rejections, calls JS functions and gives JS Python functions (one async), sets an onclick
handler and queues a microtask; it writes what it saw into #results and the JS side into
#jsresults. This test checks that each sleep resumed at its deadline's host time exactly, though
no frame falls on it, and the rest. Exits 1 on any failure.
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
page = os.path.join(source, "tests/python/pages/async.html")
with open(os.path.join(source, "tests/python/pages/async-data.txt"), encoding="utf-8") as f:
    data = f.read()

system = _karouka.System(backend="software")
view = system.view(400, 300)
view.load_file(page)

clock = 0.0
results = {}
js_results = {}
for frame in range(400):
    clock += 16.0
    view.advance_time(clock)
    view.render()
    results = json.loads(view.document.getElementById("results").textContent)
    js_results = json.loads(view.document.getElementById("jsresults").textContent)
    if "quarters" in results and results.get("fetch_done") and "async" in js_results and "microtask" in results:
        break
print(f"test_async: settled after {frame + 1} frames of 16 ms", file=sys.stderr)

# asyncio.sleep on the host clock: exact deadlines, though frames are 16 ms apart.
sleep = results.get("sleep")
check(f"sleep(1) resumed at its deadline's host time ({sleep})",
      sleep is not None and abs(sleep[1] - sleep[0] - 1000.0) < 1e-6)
quarters = results.get("quarters")
check(f"four sleep(0.25) resumed 250 ms apart exactly ({quarters})",
      sleep is not None and quarters is not None and len(quarters) == 4
      and all(abs(q - (sleep[1] + 250.0 * (i + 1))) < 1e-6 for i, q in enumerate(quarters)))

# Promises: fetch() of a file: URL, and rejections raising JSError.
check(f"window.fetch status {results.get('fetch_status')}", results.get("fetch_status") == 200)
check(f"await response.text() {results.get('fetch_text')!r}", results.get("fetch_text") == data)
check(f"js.fetch() text {results.get('js_fetch_text')!r}", results.get("js_fetch_text") == data)
check(f"a missing file's fetch raised JSError TypeError ({results.get('fetch_missing')!r})",
      str(results.get("fetch_missing", "")).startswith("TypeError"))
check(f"Promise.reject(Error) raised JSError ({results.get('rejected')!r})",
      results.get("rejected") == ["Error: boom", "boom"])

# Python calling JS.
check(f"js.jsMul(6, 7) {results.get('js_mul')!r}", results.get("js_mul") == 42)
check(f"a JS throw raised JSError ({results.get('js_throws')!r})", results.get("js_throws") == "RangeError: from JS")
check(f"dict/list to JS ({results.get('json')!r})", results.get("json") == '{"a":[1,2.5,"x",null,true]}')
check(f"JSValue items ({results.get('parse')!r})", results.get("parse") == 2)
check(f"len() of a JS array ({results.get('array_len')!r})", results.get("array_len") == 3)
check("a Python function given to JS comes back as itself", results.get("roundtrip") is True)

# JS calling Python.
check(f"JS called a Python function ({js_results.get('add')!r})", js_results.get("add") == 5)
check(f"a Python raise threw in JS ({js_results.get('raises')!r})",
      js_results.get("raises") == "Error: ValueError: from Python")
check(f"an async Python function returned a Promise to JS ({js_results.get('async')!r})", js_results.get("async") == 42)

# Callback-typed IDL members.
check(f"onclick ran a Python handler ({results.get('onclick')!r})", results.get("onclick") == ["click"])
check("onclick reads back", results.get("onclick_get") is True)
check("queueMicrotask didn't run its callback at once", results.get("microtask_queued") is True)
check(f"queueMicrotask ran a Python callback ({results.get('microtask')!r})", results.get("microtask") == ["ran"])

view.close()
if failures:
    print(f"test_async: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_async: ok", file=sys.stderr)
