#!/usr/bin/env bash
# Builds karouka's Python wheel from source: Ladybird at its pinned commit plus
# patches/ladybird.patch, karouka's src/ built into the _karouka module, packed by tools/wheel.py.
#
#   ./build.sh [setup|build|wheel|smoke|test|all]...     (default: all = setup build wheel smoke)
#
# Environment:
#   KAROUKA_PYTHON   the CPython 3.14 (with the GIL) to build for   (default: python3.14 on PATH)
#   JOBS             parallel compile jobs                          (default: all cores)
#   LADYBIRD_DIR     where Ladybird is cloned                       (default: third_party/ladybird)
#   DIST             where the wheel goes                           (default: dist)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LADYBIRD_URL=https://github.com/LadybirdBrowser/ladybird.git
LADYBIRD_PIN=1a9bd18fc433dc187252fc9014c43b837d3a98fb
LADYBIRD_DIR="${LADYBIRD_DIR:-$ROOT/third_party/ladybird}"
BUILD="$LADYBIRD_DIR/Build/release"
KR_BUILD="$BUILD/karouka"
DIST="${DIST:-$ROOT/dist}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
TOOLCHAIN="$ROOT/.toolchain"
PATCH="$ROOT/patches/ladybird.patch"

export RUSTUP_HOME="$TOOLCHAIN/rustup" CARGO_HOME="$TOOLCHAIN/cargo"
export CC="${CC:-/opt/homebrew/opt/llvm/bin/clang}" CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
export LADYBIRD_SOURCE_DIR="$LADYBIRD_DIR"
export CARGO_BUILD_JOBS="$JOBS" VCPKG_MAX_CONCURRENCY="$JOBS"
export PATH="$TOOLCHAIN/cmake-venv/bin:$PATH"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

python_for_module() {
    local python="${KAROUKA_PYTHON:-python3.14}" path kind
    path="$(command -v "$python")" || die "no $python on PATH; set KAROUKA_PYTHON to a CPython 3.14"
    kind="$("$path" -c 'import sys, sysconfig; print(sys.implementation.name, "%d.%d" % sys.version_info[:2], "free-threaded" if sysconfig.get_config_var("Py_GIL_DISABLED") else "gil")')"
    [[ "$kind" == "cpython 3.14 gil" ]] || die "$path is $kind; the module needs CPython 3.14 with the GIL"
    echo "$path"
}

rust_bin() {
    local version
    version="$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$LADYBIRD_DIR/rust-toolchain.toml")"
    echo "$RUSTUP_HOME/toolchains/$version-aarch64-apple-darwin/bin"
}

# Ladybird at the pin with karouka's changes as one commit on top, and the build toolchain.
setup() {
    [[ "$(uname -s)-$(uname -m)" == Darwin-arm64 ]] || die "karouka builds on Apple Silicon macOS only"
    local pkg missing=()
    for pkg in autoconf autoconf-archive automake ccache libtool nasm ninja pkgconf llvm; do
        brew list --versions "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done
    ((${#missing[@]} == 0)) || die "missing Homebrew packages: brew install ${missing[*]}"
    python_for_module >/dev/null

    say "Ladybird at ${LADYBIRD_PIN:0:10} + patches/ladybird.patch"
    if [[ ! -d "$LADYBIRD_DIR/.git" ]]; then
        mkdir -p "$(dirname "$LADYBIRD_DIR")"
        git clone --filter=blob:none --no-checkout "$LADYBIRD_URL" "$LADYBIRD_DIR"
    fi
    local applied="$LADYBIRD_DIR/.git/karouka-patch"
    local want
    want="$(shasum -a 256 "$PATCH" | cut -d' ' -f1)"
    if [[ "$(cat "$applied" 2>/dev/null)" != "$want" ]]; then
        git -C "$LADYBIRD_DIR" cat-file -e "$LADYBIRD_PIN^{commit}" 2>/dev/null ||
            git -C "$LADYBIRD_DIR" fetch --filter=blob:none origin "$LADYBIRD_PIN"
        git -C "$LADYBIRD_DIR" -c advice.detachedHead=false checkout -q -B karouka "$LADYBIRD_PIN"
        git -C "$LADYBIRD_DIR" -c user.name=karouka -c user.email=karouka@localhost \
            am -q --whitespace=nowarn --committer-date-is-author-date "$PATCH"
        echo "$want" >"$applied"
    fi
    git -C "$LADYBIRD_DIR" log --oneline -2

    say "toolchain in .toolchain/"
    mkdir -p "$TOOLCHAIN"
    if [[ ! -x "$TOOLCHAIN/cmake-venv/bin/cmake" ]]; then
        python3 -m venv "$TOOLCHAIN/cmake-venv"
        "$TOOLCHAIN/cmake-venv/bin/pip" -q install 'cmake>=3.30,<4'
    fi
    local rust version
    rust="$(rust_bin)"
    version="$(basename "$(dirname "$rust")")"
    if [[ ! -x "$rust/rustc" ]]; then
        curl --proto '=https' --tlsv1.2 -sSf --retry 5 https://sh.rustup.rs -o "$TOOLCHAIN/rustup-init.sh"
        sh "$TOOLCHAIN/rustup-init.sh" -y -q --no-modify-path --profile minimal \
            --default-toolchain "${version%-aarch64-apple-darwin}"
    fi
    cmake --version | head -1
    "$rust/rustc" --version
}

# The _karouka module and the karouka package, in $KR_BUILD/lib.
build() {
    [[ -f "$LADYBIRD_DIR/.git/karouka-patch" ]] || die "run ./build.sh setup first"
    local python rust
    python="$(python_for_module)"
    rust="$(rust_bin)"
    export PATH="$rust:$PATH"
    say "vcpkg dependencies (the first run builds them: long)"
    (cd "$LADYBIRD_DIR" && python3 Meta/ladybird.py vcpkg -j "$JOBS")
    export VCPKG_ROOT="$LADYBIRD_DIR/Build/vcpkg-release"
    export PATH="$VCPKG_ROOT:$PATH"

    say "configure"
    if [[ ! -f "$BUILD/build.ninja" ]]; then
        cmake --preset Release -S "$LADYBIRD_DIR" -B "$BUILD" \
            -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
            -DBUILD_TESTING=OFF -DENABLE_CRANELIFT_JIT=OFF \
            -DRUST_CARGO="$rust/cargo" -DRUST_RUSTC="$rust/rustc" \
            -DKAROUKA_SOURCE_DIR="$ROOT" -DKAROUKA_PYTHON="$python"
    else
        cmake -S "$LADYBIRD_DIR" -B "$BUILD" -DKAROUKA_SOURCE_DIR="$ROOT" -DKAROUKA_PYTHON="$python" >/dev/null
    fi

    say "build _karouka with $JOBS jobs"
    # A generated header some sources include without an ordering dependency: make it first.
    ninja -C "$BUILD" Libraries/LibCompositing/WebGL/GLFunctions.h
    ninja -C "$BUILD" -j "$JOBS" _karouka
    ls -l "$KR_BUILD"/lib/_karouka.*.so
}

# dist/karouka-<version>-cp314-cp314-macosx_<minos>_arm64.whl
wheel() {
    say "wheel"
    python3 "$ROOT/tools/wheel.py" --build "$KR_BUILD" --resources "$LADYBIRD_DIR/Base/res" --out "$DIST" \
        --forbid "$ROOT" --forbid "$LADYBIRD_DIR"
}

# Installs the wheel into a fresh venv outside the checkout and runs a page from it with the checkout
# and the Ladybird tree unreadable (sandbox-exec): the wheel must carry everything it loads.
smoke() {
    local whl python tmp deny
    whl="$(ls "$DIST"/karouka-*.whl 2>/dev/null | head -1)" || true
    [[ -n "$whl" ]] || die "no wheel in $DIST; run ./build.sh wheel first"
    python="$(python_for_module)"
    say "smoke test of $(basename "$whl")"
    tmp="$(realpath "$(mktemp -d "${TMPDIR:-/tmp}/karouka-smoke.XXXXXX")")"
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/app"
    cp -R "$ROOT/examples/python_viewer" "$tmp/example"
    cat >"$tmp/app/index.html" <<'HTML'
<!doctype html><html><head><title>smoke</title></head><body><p id="out"></p>
<script type="text/python">
import pathlib
document.getElementById("out").textContent = f"{window.innerWidth}"
pathlib.Path(document.URL.removeprefix("file://")).with_name("ran.txt").write_text(document.getElementById("out").textContent)
</script></body></html>
HTML
    "$python" -m venv "$tmp/venv"
    "$tmp/venv/bin/python" -m pip install -q --no-index --no-deps --disable-pip-version-check "$whl"
    deny="(version 1)(allow default)(deny file-read* (subpath \"$ROOT\") (subpath \"$LADYBIRD_DIR\"))"
    (cd "$tmp" && env -u KR_RESOURCE_ROOT -u PYTHONPATH -u PYTHONHOME sandbox-exec -p "$deny" \
        "$tmp/venv/bin/python" -I -m karouka --software --headless --frames 60 --size 800x600 \
        --capture "$tmp/example.png" "$tmp/example/page.html")
    (cd "$tmp" && env -u KR_RESOURCE_ROOT -u PYTHONPATH -u PYTHONHOME sandbox-exec -p "$deny" \
        "$tmp/venv/bin/python" -I -m karouka --software --headless --frames 10 --size 800x600 "$tmp/app/index.html")
    [[ -s "$tmp/example.png" ]] || die "smoke: the example page wrote no capture"
    [[ "$(cat "$tmp/app/ran.txt" 2>/dev/null)" == 800 ]] || die "smoke: the page's text/python didn't run"
    echo "smoke: ok (venv install, example page rendered, text/python ran)"
}

# tests/python against the build's module (not the wheel), one script at a time.
test_python() {
    local python failed=0 test log
    python="$(python_for_module)"
    [[ -d "$KR_BUILD/lib" ]] || die "no build at $KR_BUILD; run ./build.sh build first"
    mkdir -p "$KR_BUILD/python-tests"
    for test in "$ROOT"/tests/python/test_*.py; do
        log="$KR_BUILD/python-tests/$(basename "$test" .py).log"
        if PYTHONPATH="$KR_BUILD/lib" KR_SOURCE_DIR="$ROOT" "$python" -s "$test" >"$log" 2>&1; then
            echo "ok   $(basename "$test")"
        else
            echo "FAIL $(basename "$test") (see $log)"
            failed=1
        fi
    done
    return "$failed"
}

(($#)) || set -- all
for step in "$@"; do
    case "$step" in
        setup) setup ;;
        build) build ;;
        wheel) wheel ;;
        smoke) smoke ;;
        test) test_python ;;
        all) setup && build && wheel && smoke ;;
        -h|--help|help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown step: $step (setup, build, wheel, smoke, test, all)" ;;
    esac
done
