"""karouka.App (src/python/app.mm) and python -m karouka, headless on the software backend.

Run by tools/dev.sh python (and check) with the built lib dir (the _karouka module and the karouka
package) on PYTHONPATH. One App drives tests/python/pages/app.html: clicks, keys and a resize reach
the page's Python, the window's title follows document.title, closing the last window returns from
run(), stop() returns with the windows open, and what on_frame raises comes out of run(). Then
`python -m karouka --headless` runs as a child process and captures a PNG. Exits 1 on any failure.
"""

import os
import subprocess
import sys
import tempfile

import karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


page = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pages", "app.html")

app = karouka.App(backend="software", headless=True)
check("app.headless", app.headless)
check("app.system is a System", isinstance(app.system, karouka.System))
try:
    karouka.App(headless=True)
    check("a second App raises Error", False)
except karouka.Error:
    pass

window = app.open(page, title="App test", size=(320, 200))
check("open() returns an AppWindow", isinstance(window, karouka.AppWindow))
check("_karouka.Window is still the IDL Window's proxy", karouka.AppWindow.__name__ == "AppWindow" and
      __import__("_karouka").Window.__name__ == "Window" and __import__("_karouka").Window is not karouka.AppWindow)
check("app.windows", app.windows == [window])
check("window.size", window.size == (320, 200))
check("window.scale headless", window.scale == 1.0)

seen = {}
frames = []


def on_frame(w, now_ms):
    frames.append(now_ms)
    n = len(frames)
    view = w.view
    if n == 3:
        seen["initial"] = w.title
        view.inject_input("mouse_move", 50, 25)
        view.inject_input("mouse_down", 50, 25)
        view.inject_input("mouse_up", 50, 25)
    elif n == 6:
        seen["clicked"] = w.title
        view.inject_input("key_down", key_code=0, code_point=ord("a"))
        view.inject_input("key_up", key_code=0, code_point=ord("a"))
    elif n == 9:
        seen["typed"] = w.title
        w.resize(400, 240)
    elif n == 14:
        seen["resized"] = w.title
        seen["size"] = w.size
        w.close()
    elif n > 30:
        app.stop()


app.run(on_frame)
check(f"initial title {seen.get('initial')!r}", seen.get("initial") == "clicks=0 keys= width=320")
check(f"clicked title {seen.get('clicked')!r}", seen.get("clicked") == "clicks=1 keys= width=320")
check(f"typed title {seen.get('typed')!r}", seen.get("typed") == "clicks=1 keys=a width=320")
check(f"resized title {seen.get('resized')!r}", seen.get("resized") == "clicks=1 keys=a width=400")
check(f"resized size {seen.get('size')}", seen.get("size") == (400, 240))
check(f"run() returned when the last window closed ({len(frames)} frames)", len(frames) == 14)
check("frames on a 60 Hz clock", abs(frames[1] - frames[0] - 1000 / 60) < 1e-6)
check("window.closed", window.closed)
check("app.windows empty", app.windows == [])
check("the closed window keeps open()'s title", window.title == "App test")
try:
    window.view.render()
    check("the window's view is closed", False)
except karouka.Error:
    pass
app.run()  # no windows: returns at once

# stop() returns with the windows open; run() again continues them.
second = app.open(page)
check("app.windows after a second open()", app.windows == [second])
count = 0


def stop_at_3(w, now_ms):
    global count
    count += 1
    if count == 3:
        app.stop()


app.run(stop_at_3)
check(f"stop() returned after 3 frames ({count})", count == 3)
check("stop() keeps the window open", not second.closed and app.windows == [second])


class Boom(Exception):
    pass


def boom(w, now_ms):
    raise Boom()


try:
    app.run(boom)
    check("run() raises what on_frame raised", False)
except Boom:
    pass
check("the window stays open after on_frame raised", not second.closed)
second.close()
check("close() is idempotent", second.close() is None and second.closed)
del app, window, second

# python -m karouka, headless, in a child process (its own System).
with tempfile.TemporaryDirectory() as tmp:
    png = os.path.join(tmp, "app.png")
    run = subprocess.run([sys.executable, "-s", "-m", "karouka", "--headless", "--software", "--frames", "5",
                          "--size", "200x100", "--capture", png, page], capture_output=True, text=True, timeout=120)
    check(f"python -m karouka exits 0 ({run.returncode}: {run.stderr[-500:]})", run.returncode == 0)
    check("python -m karouka --capture writes a PNG",
          os.path.exists(png) and open(png, "rb").read(8) == b"\x89PNG\r\n\x1a\n")
    bad = subprocess.run([sys.executable, "-s", "-m", "karouka", "--headless", page], capture_output=True, text=True,
                         timeout=60)
    check("python -m karouka --headless without --frames is a usage error", bad.returncode == 2)

if failures:
    print(f"test_app: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_app: ok")
