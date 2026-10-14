#!/usr/bin/env python3
"""Check that a karouka wheel runs on every Mac its platform tag claims.

    tools/check_portability.py dist/karouka-<version>-cp314-cp314-macosx_14_0_arm64.whl

For the Mach-O files in the wheel:
- every file's LC_BUILD_VERSION minos is no newer than the tag's macOS version (macosx_14_0: 14.0);
- every symbol a file imports from a system library or framework (libSystem, libc++, libobjc, the
  frameworks) is exported by that macOS version's SDK, read from its .tbd stubs. Weak imports may
  be missing. A missing symbol is a load failure on that macOS. Without that SDK (Command Line
  Tools or Xcode, MacOSX<major>.sdk) the check is skipped, or fails with --require-sdk;
- the _karouka module holds no instruction the Apple M1 lacks: BF16 and I8MM (M2 and later), SVE,
  SME (M4), WFET/WFIT and LS64. The module is what Ladybird's flags compile: build.sh builds with
  ENABLE_CI_BASELINE_CPU (-mcpu=apple-m1, Rust target-cpu=apple-m1), and this catches a build that
  compiled for the host's CPU instead. The vcpkg dylibs are built without CPU flags, and some
  (codecs) carry newer instructions behind a runtime CPU check, which a scan can't tell from
  unconditional use, so they aren't scanned. Needs llvm-objdump (Homebrew's llvm, which the build
  uses).

Exits 0 when every check passed, 1 when one failed, 2 on a usage error.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

SYSTEM_LIBRARIES = ("libSystem", "libc++", "libobjc")
SDK_ROOTS = ("/Library/Developer/CommandLineTools/SDKs",)
# Mnemonics (Apple syntax, before any ".arrangement" suffix) of extensions the M1 doesn't have.
POST_M1 = {
    "bfdot", "bfmmla", "bfmlalb", "bfmlalt", "bfcvt", "bfcvtn", "bfcvtn2",  # BF16
    "smmla", "ummla", "usmmla", "usdot", "sudot",  # I8MM
    "smstart", "smstop",  # SME
    "wfet", "wfit",  # WFxT
    "ld64b", "st64b", "st64bv", "st64bv0",  # LS64
}
SVE_OPERAND = re.compile(r"\b(?:z\d+\.[bhsdq]|za\d*[hv]?\b|p\d+/[mz])")


def is_macho(path):
    with open(path, "rb") as f:
        return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


def minos(path):
    out = subprocess.run(["otool", "-l", path], check=True, capture_output=True, text=True).stdout
    match = re.search(r"cmd LC_BUILD_VERSION.*?minos (\S+)", out, re.S)
    return tuple(int(x) for x in match.group(1).split(".")) if match else None


def find_sdk(major):
    roots = list(SDK_ROOTS)
    developer = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True).stdout.strip()
    if developer:
        roots.append(os.path.join(developer, "Platforms/MacOSX.platform/Developer/SDKs"))
    for root in roots:
        for name in (f"MacOSX{major}.sdk", f"MacOSX{major}.0.sdk"):
            if os.path.isdir(os.path.join(root, name)):
                return pathlib.Path(root, name)
    return None


def sdk_symbols(sdk):
    exported = {"dyld_stub_binder"}
    stubs = list((sdk / "usr/lib").rglob("*.tbd")) + list((sdk / "System/Library/Frameworks").rglob("*.tbd"))
    for stub in stubs:
        text = stub.read_text(errors="ignore")
        for block, prefixes in (("objc-classes", ("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_")),
                                ("objc-eh-types", ("_OBJC_EHTYPE_$_",)), ("objc-ivars", ("_OBJC_IVAR_$_",))):
            for match in re.finditer(block + r":\s*\[([^\]]*)\]", text):
                for name in re.findall(r"[A-Za-z_$][\w.$]*", match.group(1)):
                    exported.update(prefix + name.lstrip("_") for prefix in prefixes)
        exported.update(re.findall(r"'?(_[\w$.@<>:,~*&\[\] ]*?)'?(?=[,\]\s])", text))
    return exported


def system_imports(path):
    out = subprocess.run(["nm", "-u", "-m", path], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        match = re.match(r"\s*\(undefined\)\s*(weak )?external (\S+) \(from (\S+)\)", line)
        if not match or match.group(1):
            continue
        library = match.group(3)
        if library in SYSTEM_LIBRARIES or not library.startswith("lib"):
            yield library, match.group(2)


def post_m1_instructions(objdump, path):
    found = {}
    proc = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", "--no-leading-addr",
                             "--mattr=+bf16,+i8mm,+sve,+sve2,+sme,+sme2,+wfxt,+ls64", path],
                            stdout=subprocess.PIPE, text=True)
    for line in proc.stdout:
        parts = line.split(None, 1)
        if not parts:
            continue
        mnemonic = parts[0].split(".")[0]
        if mnemonic in POST_M1 or (len(parts) > 1 and SVE_OPERAND.search(parts[1])):
            found[mnemonic] = found.get(mnemonic, 0) + 1
    proc.wait()
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wheel")
    parser.add_argument("--require-sdk", action="store_true", help="fail when the tag's macOS SDK isn't installed")
    args = parser.parse_args()
    tag = re.search(r"-macosx_(\d+)_(\d+)_arm64\.whl$", args.wheel)
    if not tag:
        print(f"check_portability: {args.wheel} isn't a macosx_*_arm64 wheel", file=sys.stderr)
        return 2
    target = (int(tag.group(1)), int(tag.group(2)))
    objdump = shutil.which("llvm-objdump") or "/opt/homebrew/opt/llvm/bin/llvm-objdump"
    if not os.access(objdump, os.X_OK):
        print("check_portability: no llvm-objdump (brew install llvm)", file=sys.stderr)
        return 2

    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        zipfile.ZipFile(args.wheel).extractall(tmp)
        machos = sorted(p for p in pathlib.Path(tmp).rglob("*") if p.is_file() and is_macho(p))
        names = {p: str(p.relative_to(tmp)) for p in machos}

        newer = [f"{names[p]} ({'.'.join(map(str, v))})" for p in machos if (v := minos(p)) and v > target]
        if newer:
            failures.append(f"minos newer than {target[0]}.{target[1]}: " + ", ".join(newer))
        print(f"{'FAIL' if newer else 'ok  '} minos <= {target[0]}.{target[1]} ({len(machos)} Mach-O files)")

        sdk = find_sdk(target[0])
        if sdk:
            exported = sdk_symbols(sdk)
            missing = {}
            for p in machos:
                for library, symbol in system_imports(p):
                    if symbol not in exported:
                        missing.setdefault(names[p], []).append(f"{symbol} ({library})")
            for name, symbols in missing.items():
                failures.append(f"{name} imports what macOS {target[0]} lacks: " + ", ".join(symbols[:5]))
            print(f"{'FAIL' if missing else 'ok  '} system imports exist in {sdk.name}")
        elif args.require_sdk:
            failures.append(f"no MacOSX{target[0]}.sdk to check system imports against")
            print(f"FAIL no MacOSX{target[0]}.sdk installed")
        else:
            print(f"skip system imports: no MacOSX{target[0]}.sdk installed")

        newer_cpu = {}
        modules = [p for p in machos if p.name.startswith("_karouka.")]
        if not modules:
            failures.append("no _karouka module in the wheel")
        for p in modules:
            if found := post_m1_instructions(objdump, p):
                newer_cpu[names[p]] = found
        for name, found in newer_cpu.items():
            failures.append(f"{name} has instructions the M1 lacks: " + ", ".join(f"{m} x{n}" for m, n in sorted(found.items())))
        print(f"{'FAIL' if newer_cpu else 'ok  '} no instructions beyond the Apple M1 in the module")

    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
