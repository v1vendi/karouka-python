"""a View or System whose last reference goes on another thread is destroyed on the main thread.

Run by tools/dev.sh python (and check). The engine runs on the main thread only, so module.cpp
queues a View or System dropped elsewhere and the main thread destroys it from a pending call
(Py_AddPendingCall) or at its next entry point. This test drops a View running
tests/python/pages/dom.html (a document interpreter, an app-side proxy) on a worker thread, then a
View and the System together: the worker sees them queued, and once the main thread runs bytecode
the queue is empty, the document's interpreter is gone, the proxy is stale and a new System() is
refused as after kr_system_destroy. Exits 1 on any failure.
"""

import os
import sys
import threading

import _karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


def drop_on_thread(box):
    """Clears `box` (the only references) on a worker thread; what the queue looked like after."""
    seen = []

    def drop():
        try:
            box.clear()
            seen.append(_karouka._debug_deferred())
        except BaseException as e:  # noqa: BLE001 - reported below
            seen.append(e)

    thread = threading.Thread(target=drop)
    thread.start()
    thread.join()
    return seen[0] if seen else None


def run_bytecode():
    """A loop in the main interpreter on the main thread: its pending calls run between bytecodes."""
    total = 0
    for i in range(10000):
        total += i
    return total


source = os.environ["KR_SOURCE_DIR"]
page = os.path.join(source, "tests/python/pages/dom.html")

system = _karouka.System(backend="software")
view = system.view(800, 600)  # pages/dom.html asserts the width
view.load_file(page)
for frame in range(1, 21):
    view.advance_time(frame * 16.0)
    view.render()
document = view.document
counts = _karouka._debug_counts()
check(f"the page has a document interpreter ({counts})", counts["interpreters"] == 1)

# The View alone: its last reference goes on the worker.
box = [view]
del view
seen = drop_on_thread(box)
print(f"after the worker dropped the view: {seen}")
check(f"the worker dropped the view without error ({seen})", isinstance(seen, dict))
check(f"the view was queued, not destroyed on the worker ({seen})",
      isinstance(seen, dict) and seen["deferred"] == 1 and seen["pending"] + seen["destroyed"] == 1)
run_bytecode()
deferred = _karouka._debug_deferred()
check(f"the main thread destroyed it from a pending call ({deferred})",
      deferred == {"pending": 0, "deferred": 1, "destroyed": 1})
counts = _karouka._debug_counts()
check(f"its document's interpreter and contexts are gone ({counts})",
      counts["interpreters"] == 0 and counts["contexts"] == 0)
try:
    document.title
    check("the dropped view's document proxy is stale", False)
except _karouka.StaleObjectError:
    pass

# A View and the System together: the view's destruction is queued before the system's.
second = system.view(64, 64)
second.advance_time(16.0)
second.render()
box = [system, second]
del system, second
seen = drop_on_thread(box)
print(f"after the worker dropped the view and the system: {seen}")
check(f"the worker dropped both without error ({seen})", isinstance(seen, dict))
check(f"both were queued ({seen})", isinstance(seen, dict) and seen["deferred"] == 3)
run_bytecode()
deferred = _karouka._debug_deferred()
check(f"the main thread destroyed both ({deferred})", deferred == {"pending": 0, "deferred": 3, "destroyed": 3})
try:
    _karouka.System(backend="software")
    check("a System() after the deferred kr_system_destroy is refused", False)
except _karouka.Error as e:
    check(f"a System() after the deferred kr_system_destroy is KR_ERROR_INVALID_STATE ({e.args})", e.args[1] == 6)

if failures:
    print(f"test_threads: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_threads: ok")
