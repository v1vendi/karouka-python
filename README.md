# karouka

HTML/CSS user interfaces for Python desktop apps, rendered in-process by
[Ladybird](https://ladybird.org)'s browser engine. A page's logic is Python in
`<script type="text/python">`, with `window` and `document` bound straight to LibWeb's C++ DOM:
no JS shim, no JSON bridge, no web server, no second process.

```html
<button id="go">Go</button><p id="out"></p>
<script type="text/python">
def on_click(e):
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

## Status

Early: version 0.1.0. **macOS 14+ on Apple Silicon, CPython 3.14** (with the GIL) only.

- One process, one thread, no networking: `file:`, `data:` and `about:` load, `http(s)` doesn't.
- Images: PNG, APNG, JPEG, JPEG XL, WebP, GIF, BMP, ICO and SVG. AVIF is left out of the build.
- Python runs only in pages loaded from `file:` URLs under the app's directory, and has full OS
  access, like the app itself.
- Each document gets its own Python subinterpreter and asyncio loop, driven by the engine's clock:
  `await asyncio.sleep(1)` and `await window.fetch(url)` work in page scripts.
- 474 DOM interfaces are bound, generated from LibWeb's WebIDL with exact WebIDL names
  (`getElementById`, `textContent`, `addEventListener`). MDN is the API reference.
- JavaScript on the same page keeps working and shares the DOM and event listeners with Python.

## Install

From a release wheel:

```bash
python3.14 -m pip install karouka-0.1.0-cp314-cp314-macosx_14_0_arm64.whl
python3.14 -m karouka examples/python_viewer/page.html
```

The wheel is self-contained: the engine module, the dylibs it loads (Skia, ICU, HarfBuzz and the
image codecs) and Ladybird's resources (default stylesheets, icons).

To build it yourself, see [docs/BUILDING.md](docs/BUILDING.md): `./build.sh` clones Ladybird at
the pinned commit, applies `patches/ladybird.patch`, builds the module and packs the wheel.

## Try it

```bash
python3.14 examples/python_viewer/viewer.py                       # a window
python3.14 -m karouka --headless --software --frames 60 --capture out.png page.html
```

## Documentation

- [docs/PYTHON.md](docs/PYTHON.md): the Python API, how the bindings work, threads, object
  lifetime, asyncio, JS interop and how the wheel is packed.
- [docs/BUILDING.md](docs/BUILDING.md): building from source and what the Ladybird patch changes.
- `include/karouka/karouka.h`: the C API underneath (render a page into an IOSurface or a Metal
  texture from a native host).

## Layout

| Path | What |
| --- | --- |
| `patches/ladybird.patch` | All of karouka's changes to Ladybird, one commit on top of the pinned upstream commit |
| `src/python/` | The `_karouka` extension module, the DOM bindings and their generator, the `karouka` package |
| `src/engine/` | The embedder: page loading, frame loop, rendering, the C API's implementation |
| `src/viewer/` | `karouka_viewer`, a native demo viewer, and the NSEvent mapping the app shell shares |
| `include/karouka/karouka.h` | The C API |
| `tests/python/` | Python tests (`./build.sh test`); `tests/*.c` are the C API's tests |
| `examples/python_viewer/` | A page whose Python scripts run in the page, and a small viewer |
| `tools/wheel.py` | Packs the build into a relocatable wheel |
| `build.sh` | Setup, build, wheel, smoke test |

## License

karouka is released under the BSD 2-Clause license (`LICENSE`), as Ladybird is. The wheel also
carries the third-party libraries listed in `THIRD_PARTY_NOTICES.md`, under their own licenses.
