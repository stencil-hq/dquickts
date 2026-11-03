#!/usr/bin/env python3.15
"""Import every module vendored in pylib/ under quickpy and classify failures.

Each module (every top-level module and every package submodule of pylib/,
minus `__main__` modules and modules whose import has external side
effects, see SKIP) is imported in a fresh `qpy -c "import <name>"` process.
A failing import is also tried under the host CPython 3.15 (same version
as pylib/), and the failure is classified:

  host     the import fails under the host CPython as well (Windows-only
           modules, C modules disabled in the host build, ...)
  native   ModuleNotFoundError for a native module that is listed in the
           native sections of pylib/MANIFEST.txt but that quickpy does not
           provide (yet)
  bug      anything else: an exception, crash or hang that CPython does
           not have; these are quickpy bugs

Usage:
  tools/pylib_sweep.py [--qpy PATH] [-j N] [-v] [MODULE ...]

Exits 1 if any import is classified `bug`.  -v also lists the `native` and
`host` failures.  Stdlib only; run with the host CPython 3.15.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYLIB_DIR = os.path.join(ROOT_DIR, "pylib")
MANIFEST = os.path.join(PYLIB_DIR, "MANIFEST.txt")

# Modules never imported by the sweep: importing them does something
# outside the process.
SKIP = {
    "antigravity": "opens a web browser",
}

TIMEOUT = 60

# "No module named 'x'", or for sys.stdlib_module_names entries "Standard
# library module ['disabled during build ']'x' was not found".
MISSING_RE = re.compile(r"^ModuleNotFoundError: (?:No module named"
                        r"|Standard library module(?: [\w ]+)?) '([\w.]+)'")


def pylib_modules():
    """Dotted names of every importable module under pylib/ (sorted)."""
    names = []
    for dirpath, dirnames, filenames in os.walk(PYLIB_DIR):
        rel = os.path.relpath(dirpath, PYLIB_DIR)
        parts = [] if rel == "." else rel.split(os.sep)
        if parts and (not all(p.isidentifier() for p in parts)
                      or "__init__.py" not in filenames):
            dirnames[:] = []
            continue
        if not parts:
            # pylib/test: symlink to CPython's test suite (tests/py/run.py l3)
            dirnames[:] = [d for d in dirnames
                           if d not in ("site-packages", "test")]
        dirnames.sort()
        for fn in sorted(filenames):
            if not fn.endswith(".py"):
                continue
            stem = fn[:-3]
            if stem == "__init__":
                if parts:
                    names.append(".".join(parts))
                continue
            if not stem.isidentifier() or stem == "__main__":
                continue
            names.append(".".join(parts + [stem]))
    return sorted(n for n in names if n.split(".")[0] not in SKIP)


def manifest_natives():
    """Native module names listed in the native sections of the manifest."""
    names = set()
    with open(MANIFEST, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^#   ([\w.]+)\s+<- ", line)
            if m:
                names.add(m.group(1))
    return names


def run_import(exe, name, cwd):
    """(status, last line) of importing `name` under `exe`."""
    try:
        res = subprocess.run(
            [exe, "-c", "import " + name], cwd=cwd, stdin=subprocess.DEVNULL,
            capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "hung for %ds" % TIMEOUT
    if res.returncode == 0:
        return "ok", ""
    lines = res.stderr.decode(errors="replace").strip().splitlines()
    last = lines[-1] if lines else ""
    if res.returncode < 0:
        return "crash", "signal %d: %s" % (-res.returncode, last)
    return "error", last


def check(exe, host, name, natives, cwd):
    status, msg = run_import(exe, name, cwd)
    if status == "ok":
        return name, "ok", ""
    hstatus, hmsg = run_import(host, name, cwd)
    if hstatus != "ok":
        return name, "host", "%s (CPython: %s)" % (msg, hmsg)
    if status == "error":
        m = MISSING_RE.match(msg)
        if m and m.group(1).split(".")[0] in natives:
            return name, "native", msg
    return name, "bug", msg


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--qpy", default=os.path.join(ROOT_DIR, "qpy"),
                    help="quickpy executable (default: ./qpy)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4,
                    help="parallel imports")
    ap.add_argument("-v", action="store_true",
                    help="also list native and host failures")
    ap.add_argument("modules", nargs="*", help="modules (default: all)")
    args = ap.parse_args()
    if sys.version_info[:2] != (3, 15):
        sys.exit("pylib_sweep: run with the host CPython 3.15")
    exe = os.path.abspath(args.qpy)
    natives = manifest_natives()
    modules = args.modules or pylib_modules()
    results = {}
    with tempfile.TemporaryDirectory() as cwd, \
            concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        futs = [pool.submit(check, exe, sys.executable, m, natives, cwd)
                for m in modules]
        for fut in concurrent.futures.as_completed(futs):
            name, kind, msg = fut.result()
            results[name] = (kind, msg)

    counts = {}
    missing = {}
    for name in sorted(results):
        kind, msg = results[name]
        counts[kind] = counts.get(kind, 0) + 1
        if kind == "native":
            mod = MISSING_RE.match(msg).group(1).split(".")[0]
            missing.setdefault(mod, []).append(name)
        if kind == "bug" or (args.v and kind == "host"):
            print("%-5s %-40s %s" % (kind, name, msg))
    if missing:
        print("missing native modules (listed in MANIFEST.txt):")
        for mod in sorted(missing):
            names = missing[mod]
            shown = ", ".join(names if args.v else names[:4])
            if not args.v and len(names) > 4:
                shown += ", +%d more" % (len(names) - 4)
            print("  %-20s <- %s" % (mod, shown))
    print("pylib_sweep: %d modules: %s" % (len(results), ", ".join(
        "%d %s" % (counts[k], k) for k in ("ok", "native", "host", "bug")
        if k in counts)))
    return 1 if counts.get("bug") else 0


if __name__ == "__main__":
    sys.exit(main())
