#!/usr/bin/env python3
"""Build karouka's Python wheel from the build's _karouka module.

  tools/wheel.py --build KR_BUILD --resources BASE_RES --out DIR [--forbid PATH ...]

Writes DIR/karouka-<version>-cp3XX-cp3XX-macosx_<minos>_arm64.whl (DIR is replaced), holding:

- `_karouka.cpython-3XX-darwin.so`, stripped of its local symbols (strip -x), at the top level,
  where `from _karouka import ...` finds it;
- the `karouka` package (KR_BUILD/lib/karouka, which the build copies from src/python/karouka);
- `karouka/.dylibs/`: every @rpath dylib the module loads, and theirs in turn (the vcpkg dylibs,
  Skia's), resolved through each original file's own rpaths as dyld would;
- `karouka/res/`: Ladybird's Base/res (fonts, default stylesheets), which karouka/__init__.py
  points the engine at (KR_RESOURCE_ROOT) when the installed package has it.

The module's rpaths become `@loader_path/karouka/.dylibs`, each dylib's `@loader_path`, each dylib's
id `@rpath/NAME`, and every changed file is ad-hoc re-signed. The CPython tag comes from the
module's file name, the platform tag from the highest LC_BUILD_VERSION minos of the Mach-O files,
and the version from karouka/__init__.py's __version__.

Fails when a Mach-O isn't arm64-only, when a dependency is neither a system library nor in
karouka/.dylibs, when an absolute rpath is left, or when any load command names a --forbid path
(dev.sh wheel passes the checkout, the Ladybird tree and its build). The libpython the module runs
against isn't packaged or linked: it is built with -undefined dynamic_lookup, and the interpreter
that imports it supplies Python's symbols. Prints the wheel's path and size.
"""

import argparse
import base64
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snapshot_build import is_macho, otool, rpath_deps, rpaths  # noqa: E402

SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")
DYLIBS = "karouka/.dylibs"
ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)


def run(*command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"wheel: {' '.join(command)} failed: {result.stderr.strip()}")


def dependencies(path):
    """Every library a Mach-O loads (otool -L), its own install name excluded."""
    own = otool("-D", path)[1:]
    names = [line.strip().split(" (compatibility", 1)[0] for line in otool("-L", path)[1:]]
    return [name for name in names if name not in own]


def minos(path):
    lines = otool("-l", path)
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_BUILD_VERSION":
            for follow in lines[i + 1:i + 6]:
                parts = follow.split()
                if parts and parts[0] == "minos":
                    return tuple(int(part) for part in parts[1].split("."))
    sys.exit(f"wheel: {path} has no LC_BUILD_VERSION")


def gather(build, stage):
    """Copies the module and the @rpath closure of its dylibs into stage; returns the module's
    staged path and every staged Mach-O."""
    lib = os.path.join(build, "lib")
    modules = [name for name in os.listdir(lib) if re.fullmatch(r"_karouka\.cpython-\d+-darwin\.so", name)]
    if len(modules) != 1:
        sys.exit(f"wheel: expected one _karouka.cpython-*-darwin.so in {lib}, found {modules}")
    module = os.path.join(stage, modules[0])
    shutil.copy2(os.path.join(lib, modules[0]), module)
    dylibs = os.path.join(stage, DYLIBS)
    os.makedirs(dylibs)
    work = [(module, os.path.join(lib, modules[0]))]
    files = []
    while work:
        path, origin = work.pop(0)
        files.append(path)
        origin_rpaths = rpaths(origin)
        for dep in rpath_deps(path):
            if os.path.exists(os.path.join(dylibs, dep)):
                continue
            for rpath in origin_rpaths:
                candidate = os.path.join(rpath.replace("@loader_path", os.path.dirname(origin)), dep)
                if os.path.exists(candidate):
                    shutil.copy2(os.path.realpath(candidate), os.path.join(dylibs, dep))
                    work.append((os.path.join(dylibs, dep), candidate))
                    break
            else:
                sys.exit(f"wheel: {origin} links @rpath/{dep}, found in none of its rpaths {origin_rpaths}")
    return module, files


def relink(module, files):
    """Rpaths relative to each file, ids @rpath/NAME, local symbols stripped, ad-hoc signatures."""
    for path in files:
        os.chmod(path, 0o755)
        wanted = "@loader_path/" + DYLIBS if path == module else "@loader_path"
        have = rpaths(path)
        command = ["install_name_tool"]
        for rpath in dict.fromkeys(have):
            if rpath != wanted:
                command += ["-delete_rpath", rpath]
        if wanted not in have:
            command += ["-add_rpath", wanted]
        if path != module:
            command += ["-id", "@rpath/" + os.path.basename(path)]
        run(*command, path)
        run("strip", "-x", path)
        run("codesign", "--force", "--sign", "-", path)


def verify(stage, files, forbid):
    dylibs = os.path.join(stage, DYLIBS)
    for path in files:
        name = os.path.relpath(path, stage)
        archs = subprocess.run(["lipo", "-archs", path], check=True, capture_output=True, text=True).stdout.split()
        if archs != ["arm64"]:
            sys.exit(f"wheel: {name} is {archs}, not arm64 only")
        for dep in dependencies(path):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if dep.startswith("@rpath/") and os.path.isfile(os.path.join(dylibs, dep[len("@rpath/"):])):
                continue
            sys.exit(f"wheel: {name} loads {dep}, neither a system library nor in {DYLIBS}")
        absolute = [r for r in rpaths(path) if not r.startswith("@loader_path")]
        if absolute:
            sys.exit(f"wheel: {name} keeps rpaths {absolute}")
        commands = "\n".join(otool("-l", path)[1:])  # [0] is the path otool read
        for root in forbid:
            if root in commands:
                sys.exit(f"wheel: a load command of {name} names {root}")
        run("codesign", "--verify", path)


def write_wheel(stage, path, tag, version, python):
    dist_info = f"karouka-{version}.dist-info"
    meta = {
        f"{dist_info}/METADATA": (
            "Metadata-Version: 2.1\n"
            "Name: karouka\n"
            f"Version: {version}\n"
            "Summary: HTML/CSS user interfaces with Python page scripts, rendered by karouka's engine\n"
            f"Requires-Python: =={python[0]}.{python[1:]}.*\n"
        ),
        f"{dist_info}/WHEEL": (
            "Wheel-Version: 1.0\n"
            "Generator: karouka tools/wheel.py\n"
            "Root-Is-Purelib: false\n"
            f"Tag: {tag}\n"
        ),
        f"{dist_info}/top_level.txt": "_karouka\nkarouka\n",
    }
    record = []
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as wheel:
        def add(arcname, data, mode):
            info = zipfile.ZipInfo(arcname, ZIP_EPOCH)
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            wheel.writestr(info, data, compresslevel=9)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            record.append(f"{arcname},sha256={digest},{len(data)}")

        for root, dirs, names in os.walk(stage):
            dirs[:] = sorted(d for d in dirs if d != "__pycache__")
            for name in sorted(names):
                full = os.path.join(root, name)
                with open(full, "rb") as f:
                    add(os.path.relpath(full, stage), f.read(), os.stat(full).st_mode & 0o777)
        for arcname, text in meta.items():
            add(arcname, text.encode(), 0o644)
        record.append(f"{dist_info}/RECORD,,")
        info = zipfile.ZipInfo(f"{dist_info}/RECORD", ZIP_EPOCH)
        info.external_attr = 0o100644 << 16
        wheel.writestr(info, "\n".join(record) + "\n", zipfile.ZIP_DEFLATED)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", required=True, help="the karouka build dir (KR_BUILD)")
    parser.add_argument("--resources", required=True, help="Ladybird's Base/res")
    parser.add_argument("--out", required=True, help="the directory the wheel goes to (replaced)")
    parser.add_argument("--forbid", action="append", default=[], help="a path no load command may name")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    stage = os.path.join(out, "stage")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(stage)
    package = os.path.join(args.build, "lib", "karouka")
    shutil.copytree(package, os.path.join(stage, "karouka"), ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(args.resources, os.path.join(stage, "karouka", "res"))
    version_match = re.search(r'^__version__ = "([^"]+)"$', open(os.path.join(package, "__init__.py")).read(), re.M)
    if not version_match:
        sys.exit(f"wheel: no __version__ in {package}/__init__.py")
    version = version_match.group(1)

    module, files = gather(args.build, stage)
    relink(module, files)
    verify(stage, files, [os.path.abspath(p) for p in args.forbid])

    python = re.fullmatch(r"_karouka\.cpython-(\d+)-darwin\.so", os.path.basename(module)).group(1)
    target = max(minos(path) for path in files)
    tag = f"cp{python}-cp{python}-macosx_{target[0]}_{target[1] if len(target) > 1 else 0}_arm64"
    wheel = os.path.join(out, f"karouka-{version}-{tag}.whl")
    unpacked = sum(os.path.getsize(path) for path in files)
    write_wheel(stage, wheel, tag, version, python)
    shutil.rmtree(stage)
    print(f"wheel {wheel}: {os.path.getsize(wheel)} bytes ({len(files)} Mach-O files, "
          f"{len(files) - 1} dylibs, {unpacked} bytes of Mach-O unpacked)")


if __name__ == "__main__":
    main()
