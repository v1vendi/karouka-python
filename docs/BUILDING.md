# Building from source

karouka is built inside Ladybird's build: Ladybird's CMake, with `patches/ladybird.patch`
applied, picks up karouka's `src/` through `-DKAROUKA_SOURCE_DIR` and builds the `_karouka`
module from the same objects as LibWeb. `build.sh` does every step.

## Requirements

- macOS 14 or later on Apple Silicon, with the Xcode command line tools.
- Homebrew packages: `brew install autoconf autoconf-archive automake ccache libtool nasm ninja pkgconf llvm`.
  The build compiles with Homebrew's LLVM (`/opt/homebrew/opt/llvm`); set `CC`/`CXX` to use another.
- CPython 3.14 with the GIL (not the free-threaded build), e.g. `brew install python@3.14`.
  The module is built for exactly one interpreter; set `KAROUKA_PYTHON` to pick it.
- About 15 GB of disk for Ladybird's tree, its vcpkg dependencies and the build.

`build.sh setup` installs CMake (>= 3.30, in a venv) and the Rust version Ladybird pins into
`.toolchain/`, without touching a system CMake or Rust.

## Build

```bash
./build.sh            # setup, build, wheel, smoke: the wheel lands in dist/
```

or step by step:

| Step | What it does |
| --- | --- |
| `./build.sh setup` | Clones Ladybird into `third_party/ladybird` (a blobless clone), checks out the pin, applies `patches/ladybird.patch` with `git am` as one commit, installs the toolchain |
| `./build.sh build` | Builds the vcpkg dependencies, configures Ladybird's `Release` preset with karouka's hook, and builds `_karouka` |
| `./build.sh wheel` | Packs `dist/karouka-<version>-cp314-cp314-macosx_<minos>_arm64.whl` (`tools/wheel.py`) |
| `./build.sh smoke` | Installs the wheel in a fresh venv and renders a page from it with the checkout unreadable |
| `./build.sh test` | Runs `tests/python/` against the build's module |

The first build compiles Ladybird's libraries and its vcpkg dependencies and takes an hour or
more; later builds are incremental. `JOBS` caps the parallel jobs (default: all cores).
`LADYBIRD_DIR` and `DIST` move the Ladybird tree and the wheel's directory.

To use the build without a wheel, put its lib dir on `PYTHONPATH`:

```bash
PYTHONPATH=third_party/ladybird/Build/release/karouka/lib python3.14 -m karouka page.html
```

## The Ladybird patch

`patches/ladybird.patch` is a single commit on top of Ladybird
[`1a9bd18fc4`](https://github.com/LadybirdBrowser/ladybird/commit/1a9bd18fc433dc187252fc9014c43b837d3a98fb)
(`LADYBIRD_PIN` in `build.sh`). Its commit message lists what it changes. In short:

- **Build:** a CMake hook that builds karouka's sources against Ladybird's objects, and pruned
  builds that leave out the browser, its services and their dependencies.
- **No network:** resource loading without a RequestServer; no TLS, HTTP cache, HSTS list,
  libpsl or OpenSSL in the link.
- **Pruned Web APIs:** IndexedDB, WebSockets, WebDriver, Service Workers, WebGL, WebAssembly,
  SubtleCrypto, Notifications and others a UI layer doesn't need.
- **Embedder hooks:** a host clock for timers, animations and `Date`, a font provider, embedder
  script types (`<script type="text/python">`), and control over background threads.
- **Rendering and memory:** drawing into a Metal texture, layout only when something invalidated
  it, and trimmed ICU and libxml2 data.
- **No AVIF:** the `LADYBIRD_AVIF` option (off when building for karouka) leaves out the AVIF
  decoder and its libavif, dav1d and libyuv dependencies, and LibWeb stops claiming `image/avif`.
  Pass `-DLADYBIRD_AVIF=ON` at configure time to bring it back.

To look at it as a commit, run `./build.sh setup` and then `git -C third_party/ladybird show`.

### Moving to a newer Ladybird

```bash
cd third_party/ladybird
git fetch origin && git rebase origin/master      # resolve conflicts, then build and test
git format-patch -1 --stdout --zero-commit > ../../patches/ladybird.patch
```

Then set `LADYBIRD_PIN` in `build.sh` to the new base commit (`git rev-parse HEAD~1`).

## Releasing a wheel

- Build in a neutral directory, such as a fresh clone under `/tmp`. The module keeps source file
  paths as strings (assertion messages and the build's resource directory), so they show the
  build machine's paths. The installed package points the engine at its own `res/`, so the
  build's paths are never read.
- Run `./build.sh test` and `./build.sh smoke`, then attach `dist/*.whl` to a GitHub release.
- The wheel's tag names the CPython it was built for (`cp314`) and the oldest macOS its binaries
  allow (`macosx_14_0`).
