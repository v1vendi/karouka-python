#!/usr/bin/env python3
"""Snapshot the shared karouka build so dev.sh check can render without the build lock.

  tools/snapshot_build.py --build KR_BUILD --resources BASE_RES --out DIR

Clones (APFS clonefile, `cp -c`; a plain copy where cloning isn't supported) KR_BUILD/bin,
KR_BUILD/lib and KR_BUILD/libkarouka.map into DIR/{bin,lib,libkarouka.map} and Ladybird's Base/res
into DIR/res, then makes every Mach-O in the snapshot load its dylibs from DIR/lib only:

- each @rpath dependency the snapshot doesn't already hold (libkarouka's vcpkg and Skia dylibs,
  and theirs in turn) is resolved through the original file's own rpaths, as dyld would, and
  cloned into DIR/lib;
- each absolute LC_RPATH (KR_BUILD/lib, Build/release/lib, vcpkg_installed/.../lib) is deleted and
  DIR/lib added in its place (@loader_path ones stay: they now mean DIR/lib), and the file is
  ad-hoc re-signed, since install_name_tool invalidates the linker's signature.

After that nothing the snapshot loads lives in the shared build tree, so another checkout's build
can relink or replace it while the snapshot runs. Fails if any dependency can't be resolved or an
absolute rpath outside DIR is left. Prints one summary line.
"""

import argparse
import os
import shutil
import subprocess
import sys

MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xfe\xed\xfa\xcf"}


def clone(src, dst):
    """cp -c (clonefile) keeps the copy free on APFS; -R for directories, -P keeps symlinks."""
    flags = ["-R"] if os.path.isdir(src) else []
    if subprocess.run(["cp", "-c", "-P", *flags, src, dst], stderr=subprocess.DEVNULL).returncode != 0:
        subprocess.run(["cp", "-P", *flags, src, dst], check=True)


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        return f.read(4) in MACHO_MAGICS


def otool(flag, path):
    return subprocess.run(["otool", flag, path], check=True, capture_output=True, text=True).stdout.splitlines()


def rpaths(path):
    lines = otool("-l", path)
    found = []
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for follow in lines[i + 1:i + 4]:
                follow = follow.strip()
                if follow.startswith("path "):
                    found.append(follow[len("path "):].rsplit(" (offset", 1)[0])
                    break
    return found


def rpath_deps(path):
    """The @rpath/NAME libraries a Mach-O links, its own install name excluded."""
    own = otool("-D", path)[1:]
    deps = []
    for line in otool("-L", path)[1:]:
        name = line.strip().split(" (compatibility", 1)[0]
        if name.startswith("@rpath/") and name not in own:
            deps.append(name[len("@rpath/"):])
    return deps


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", required=True, help="the shared karouka build dir (KR_BUILD)")
    parser.add_argument("--resources", required=True, help="Ladybird's Base/res")
    parser.add_argument("--out", required=True, help="the snapshot dir (removed first)")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    lib = os.path.join(out, "lib")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for name in ("bin", "lib", "libkarouka.map"):
        clone(os.path.join(args.build, name), os.path.join(out, name))
    clone(args.resources, os.path.join(out, "res"))

    # (snapshot path, the path it was cloned from): @loader_path in a file's rpaths means the
    # original's directory, where its dependencies were found.
    work = []
    for sub in ("bin", "lib"):
        for name in sorted(os.listdir(os.path.join(out, sub))):
            path = os.path.join(out, sub, name)
            if is_macho(path):
                work.append((path, os.path.join(args.build, sub, name)))
    files = []
    pulled = 0
    while work:
        path, origin = work.pop(0)
        files.append(path)
        origin_rpaths = rpaths(path)
        for dep in rpath_deps(path):
            if os.path.exists(os.path.join(lib, dep)):
                continue
            for rpath in origin_rpaths:
                candidate = os.path.join(rpath.replace("@loader_path", os.path.dirname(origin))
                                         .replace("@executable_path", os.path.join(args.build, "bin")), dep)
                if os.path.exists(candidate):
                    clone(os.path.realpath(candidate), os.path.join(lib, dep))
                    work.append((os.path.join(lib, dep), candidate))
                    pulled += 1
                    break
            else:
                sys.exit(f"snapshot_build: {path} links @rpath/{dep}, found in none of its rpaths {origin_rpaths}")

    resigned = 0
    for path in files:
        absolute = [r for r in rpaths(path) if r.startswith("/")]
        if not absolute:
            continue
        command = ["install_name_tool"]
        for rpath in dict.fromkeys(absolute):
            command += ["-delete_rpath", rpath]
        command += ["-add_rpath", lib, path]
        subprocess.run(command, check=True, capture_output=True)
        subprocess.run(["codesign", "--force", "--sign", "-", path], check=True, capture_output=True)
        resigned += 1
        left = [r for r in rpaths(path) if r.startswith("/") and r != lib]
        if left:
            sys.exit(f"snapshot_build: {path} still has rpaths outside the snapshot: {left}")
    print(f"snapshot {out}: {len(files)} Mach-O files ({pulled} dylibs from outside the karouka build), "
          f"{resigned} re-pointed at its lib/ and re-signed")


if __name__ == "__main__":
    main()
