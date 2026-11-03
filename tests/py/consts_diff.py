"""co_consts differential: compile every ``*.py`` under cpython/Lib with the
host CPython and with an implementation under test, walk the code objects
and compare their ``co_consts`` (contents and order).

Usage (host CPython 3.15)::

    python3.15 tests/py/consts_diff.py --impl ./qpy [-j 12] [-v] [paths...]

``paths`` (files or directories, default ``cpython/Lib``) select the
sources.  The implementation runs this same script with ``--dump`` and
answers one JSON line per file on stdout.  Constants are encoded
canonically (type tag + value; floats by ``float.hex``; frozensets as
sorted lists; nested code objects as ``["code", qualname]``).

Intentional exceptions (counted separately, not failures):

- surrogates: the implementation stores str as UTF-16, so a pair of
  surrogate code points written as two escapes ('\\ud83d\\udc0d') is the
  same string as the astral character ('\\U0001f40d'); the two constants
  CPython keeps apart are one constant there.  A code object whose
  constants only differ that way (pairs joined, then duplicates dropped)
  is reported as an exception.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def enc(v):
    """Canonical JSON-able encoding of a constant."""
    t = type(v)
    if v is None:
        return ["None"]
    if v is Ellipsis:
        return ["Ellipsis"]
    if t is bool:
        return ["bool", v]
    if t is int:
        return ["int", str(v)]
    if t is float:
        return ["float", v.hex()]
    if t is complex:
        return ["complex", v.real.hex(), v.imag.hex()]
    if t is str:
        return ["str", v]
    if t is bytes:
        return ["bytes", v.hex()]
    if t is tuple:
        return ["tuple", [enc(x) for x in v]]
    if t is frozenset:
        return ["frozenset", sorted((enc(x) for x in v), key=json.dumps)]
    if t is slice:
        return ["slice", enc(v.start), enc(v.stop), enc(v.step)]
    if hasattr(v, "co_code"):
        return ["code", v.co_qualname]
    return ["other", t.__name__, repr(v)]


def walk(code, out):
    out.append([code.co_qualname, code.co_firstlineno, [enc(k) for k in code.co_consts]])
    for k in code.co_consts:
        if hasattr(k, "co_code"):
            walk(k, out)


def dump_file(path):
    try:
        with open(path, "rb") as f:
            src = f.read()
        code = compile(src, path, "exec", dont_inherit=True)
    except BaseException as e:  # SyntaxError, ValueError, RecursionError, ...
        return {"file": path, "error": "%s: %s" % (type(e).__name__, e)}
    out = []
    walk(code, out)
    return {"file": path, "units": out}


def dump_main(paths):
    for p in paths:
        print(json.dumps(dump_file(p)), flush=True)


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for d, dirs, names in os.walk(p):
                dirs.sort()
                files.extend(os.path.join(d, n) for n in sorted(names) if n.endswith(".py"))
        else:
            files.append(p)
    return files


def run_impl(impl, chunk, timeout):
    cmd = [impl, os.path.abspath(__file__), "--dump", *chunk]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {p: {"file": p, "error": "TIMEOUT"} for p in chunk}
    res = {}
    for line in r.stdout.splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        res[d["file"]] = d
    for p in chunk:
        if p not in res:
            res[p] = {"file": p, "error": "CRASH rc=%s %s" % (r.returncode, r.stderr[-300:])}
    return res


def units_map(units):
    m = {}
    for q, line, consts in units:
        k = (q, line)
        n = 0
        while (k + (n,)) in m:
            n += 1
        m[k + (n,)] = consts
    return m


def short(v, n=200):
    s = json.dumps(v)
    return s if len(s) <= n else s[:n] + "..."


def join_surrogates(v):
    """Encoded constant with surrogate pairs joined (str stored as UTF-16)."""
    if v[0] == "str":
        return ["str", v[1].encode("utf-16-le", "surrogatepass").decode("utf-16-le", "surrogatepass")]
    if v[0] == "tuple":
        return ["tuple", [join_surrogates(x) for x in v[1]]]
    if v[0] == "frozenset":
        return ["frozenset", sorted((join_surrogates(x) for x in v[1]), key=json.dumps)]
    return v


def utf16_view(consts):
    out = []
    for v in map(join_surrogates, consts):
        if v not in out:
            out.append(v)
    return out


def compare(ref, imp, exceptions):
    """List of mismatch descriptions for one file; ``exceptions`` counts
    the intentional ones."""
    if "error" in imp:
        return ["impl error: " + imp["error"]]
    rm, im = units_map(ref["units"]), units_map(imp["units"])
    out = []
    for k, rc in rm.items():
        ic = im.get(k)
        if ic is None:
            out.append("%s:%d: missing code object" % (k[0], k[1]))
        elif ic != rc and utf16_view(rc) == utf16_view(ic):
            exceptions["surrogates"] = exceptions.get("surrogates", 0) + 1
        elif ic != rc:
            i = 0
            while i < min(len(rc), len(ic)) and rc[i] == ic[i]:
                i += 1
            out.append("%s:%d: first difference at [%d] (len %d/%d)\n      ref  %s\n      impl %s"
                       % (k[0], k[1], i, len(rc), len(ic), short(rc[i:], 400), short(ic[i:], 400)))
    for k in im:
        if k not in rm:
            out.append("%s:%d: extra code object" % (k[0], k[1]))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true", help="dump mode (run by the implementation)")
    ap.add_argument("--impl", default=os.path.join(ROOT, "qpy"))
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--chunk", type=int, default=40)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("-v", action="store_true", help="print every mismatching code object")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args(argv)
    if a.dump:
        dump_main(a.paths)
        return 0
    files = collect(a.paths or [os.path.join(ROOT, "cpython", "Lib")])
    refs = {}
    for p in files:
        r = dump_file(p)
        if "error" not in r:
            refs[p] = r
    todo = list(refs)
    chunks = [todo[i:i + a.chunk] for i in range(0, len(todo), a.chunk)]
    imps = {}
    with ThreadPoolExecutor(a.j) as ex:
        for res in ex.map(lambda c: run_impl(a.impl, c, a.timeout), chunks):
            imps.update(res)
    nbad = nunits = nunits_bad = 0
    exceptions = {}
    for p in todo:
        mism = compare(refs[p], imps[p], exceptions)
        nunits += len(refs[p]["units"])
        if mism:
            nbad += 1
            nunits_bad += len(mism)
            rel = os.path.relpath(p, ROOT)
            print("MISMATCH %s (%d)" % (rel, len(mism)))
            for m in (mism if a.v else mism[:3]):
                print("    " + m)
    print("files: %d/%d match (%d skipped: CPython compile error); code objects: %d/%d match"
          % (len(todo) - nbad, len(todo), len(files) - len(todo), nunits - nunits_bad, nunits))
    if exceptions:
        print("intentional exceptions (code objects): " +
              ", ".join("%s %d" % kv for kv in sorted(exceptions.items())))
    return 1 if nbad else 0


if __name__ == "__main__":
    sys.exit(main())
