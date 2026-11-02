#!/usr/bin/env python3.15
"""Vendor the CPython headers and accelerator modules of the C API (D5).

Two areas are kept equal to the pinned CPython tag plus recorded patches:

  capi/include/   Include/*.h and Include/cpython/*.h (no internal/: the
                  internal headers are the hand-written shims of
                  capi/compat/), patched by capi/include/PATCHES.
                  Local (not upstream) file: pyconfig.h.
  capi/modules/   the sources of every module listed in capi/Setup.qp and
                  every file they include with #include "..." (clinic
                  *.c.h, local headers, _sre/, _sqlite/, _ssl/, _hacl/,
                  expat/), plus expat/COPYING, patched by
                  capi/modules/PATCHES.  SOURCE records the tag and commit.
  capi/compat/    the internal headers that are layout-independent CPython
                  code (COMPAT_COPIES, from Include/internal/), patched by
                  capi/compat/PATCHES.  Every other file there is a
                  hand-written shim (local).

PATCHES are unified diffs (paths a/<file>, b/<file> relative to the area;
/dev/null for files that do not exist upstream).  `#` lines before a file's
diff are its notes and are kept by --record.  Patches apply strictly: the
context must match exactly (no fuzz), at any offset.

Files are read from the git objects of the pinned tag, not from the work
tree, so local edits or patches in cpython/ never leak into capi/.

Usage:
  tools/capi_sync.py              create missing files; refuse to overwrite
                                  files that differ from tag + PATCHES or to
                                  remove stale ones
  tools/capi_sync.py --force      also overwrite differing files and remove
                                  stale ones (after a tag change)
  tools/capi_sync.py --check      exit 1 if capi/ differs from tag + PATCHES
  tools/capi_sync.py --record     rewrite PATCHES from the current capi/
                                  files (edit the files, then record)

Not vendored: Modules/_decimal (needs libmpdec; decimal uses
Lib/_pydecimal.py).  Stdlib only; deterministic output.
"""

import argparse
import difflib
import os
import posixpath
import re
import subprocess
import sys

CPYTHON_TAG = "v3.15.2"

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPYTHON_DIR = os.path.join(ROOT_DIR, "cpython")
CAPI_DIR = os.path.join(ROOT_DIR, "capi")
SETUP = os.path.join(CAPI_DIR, "Setup.qp")
PATCHES = "PATCHES"
SOURCE = "SOURCE"

# Directories searched for #include "..." (relative to Modules/), after the
# directory of the including file: CPython's -I flags of the modules,
# LIBHACL_FLAG_I and LIBEXPAT_CFLAGS.
INCLUDE_DIRS = ["", "_hacl", "_hacl/include", "expat"]
# Vendored in addition to the include closure (licences).
EXTRA_MODULE_FILES = ["expat/COPYING"]
# Include/internal/ headers copied into capi/compat/ (capi-design.md §3).
COMPAT_COPIES = [
    "pycore_bitutils.h", "pycore_blocks_output_buffer.h",
    "pycore_hashtable.h", "pycore_intrinsics.h", "pycore_llist.h",
    "pycore_lock.h", "pycore_long.h", "pycore_opcode_metadata.h",
    "pycore_opcode_utils.h",
    "pycore_parking_lot.h", "pycore_pyatomic_ft_wrappers.h",
    "pycore_pyhash.h", "pycore_pymath.h", "pycore_strhex.h",
    "pycore_time.h", "pycore_ucnhash.h", "pycore_uop_ids.h",
]

ENC = "latin-1"     # lossless bytes <-> str for diffing


def die(msg):
    print("capi_sync: error: " + msg, file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Pinned CPython tree (git objects)


def git(*args):
    try:
        res = subprocess.run(["git", "-C", CPYTHON_DIR, *args],
                             capture_output=True, check=False)
    except OSError as exc:
        die("cannot run git: %s" % exc)
    if res.returncode != 0:
        die("git %s failed: %s" % (" ".join(args),
                                   res.stderr.decode(errors="replace").strip()))
    return res.stdout


class CPythonTree:
    """Include/ and Modules/ of the pinned tag: {path: blob id} + reads."""

    def __init__(self):
        if not os.path.isdir(CPYTHON_DIR):
            die("%s is missing (run `make testpy-bootstrap`)" % CPYTHON_DIR)
        self.commit = git("rev-parse", "HEAD").decode().strip()
        tag_commit = git("rev-parse", CPYTHON_TAG + "^{commit}").decode().strip()
        if tag_commit != self.commit:
            die("cpython/ HEAD %s is not tag %s (%s)"
                % (self.commit, CPYTHON_TAG, tag_commit))
        self.files = {}
        out = git("ls-tree", "-r", "-z", "--full-tree", self.commit,
                  "--", "Include", "Modules")
        for rec in out.split(b"\0"):
            if not rec:
                continue
            meta, path = rec.split(b"\t", 1)
            mode, kind, oid = meta.decode().split()
            if kind == "blob" and mode != "120000":
                self.files[path.decode()] = oid
        self._cat = None

    def read(self, path):
        if self._cat is None:
            self._cat = subprocess.Popen(
                ["git", "-C", CPYTHON_DIR, "cat-file", "--batch"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        oid = self.files[path]
        self._cat.stdin.write(oid.encode() + b"\n")
        self._cat.stdin.flush()
        header = self._cat.stdout.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            die("git cat-file: bad object %s for %s" % (oid, path))
        data = self._cat.stdout.read(int(header[2]))
        self._cat.stdout.read(1)
        return data.decode(ENC)

    def close(self):
        if self._cat is not None:
            self._cat.stdin.close()
            self._cat.wait()
            self._cat = None


# ---------------------------------------------------------------------------
# Upstream file sets


def setup_sources():
    """Every source named in capi/Setup.qp (all conditions), in order."""
    srcs = []
    with open(SETUP, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            for tok in line[1:]:
                if tok.startswith(("@", "-", "$(")):
                    continue
                if not tok.endswith(".c"):
                    die("Setup.qp:%d: unexpected token %r" % (lineno, tok))
                if tok not in srcs:
                    srcs.append(tok)
    return srcs


INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)


def module_closure(tree):
    """Modules-relative files: Setup.qp sources + their #include closure."""
    have = {p[len("Modules/"):] for p in tree.files if p.startswith("Modules/")}
    todo = []
    for src in setup_sources():
        if src not in have:
            die("Setup.qp source %s is not in Modules/ of %s" % (src, CPYTHON_TAG))
        todo.append(src)
    for extra in EXTRA_MODULE_FILES:
        if extra not in have:
            die("%s is not in Modules/ of %s" % (extra, CPYTHON_TAG))
    seen = set(todo) | set(EXTRA_MODULE_FILES)
    while todo:
        path = todo.pop()
        text = tree.read("Modules/" + path)
        for name in INCLUDE_RE.findall(text):
            here = posixpath.dirname(path)
            for base in [here] + INCLUDE_DIRS:
                cand = posixpath.normpath(posixpath.join(base, name))
                if cand in have:
                    if cand not in seen:
                        seen.add(cand)
                        todo.append(cand)
                    break
            # Unresolved names are Include/ headers (Python.h, pycore_*.h:
            # capi/include, capi/compat) or system headers.
    return sorted(seen)


def include_files(tree):
    """Include-relative public headers (Include/*.h, Include/cpython/*.h)."""
    out = []
    for p in tree.files:
        if not p.startswith("Include/") or not p.endswith(".h"):
            continue
        rel = p[len("Include/"):]
        if "/" not in rel or rel.startswith("cpython/") and rel.count("/") == 1:
            out.append(rel)
    return sorted(out)


class Area:
    def __init__(self, name, prefix, files, local):
        self.name = name                # capi/<name>
        self.dir = os.path.join(CAPI_DIR, name)
        self.prefix = prefix            # upstream directory
        self.files = files              # upstream relative paths
        # hand-written files, never touched; None: every non-upstream file
        self.local = set(local) if local is not None else None
        self.patch_path = os.path.join(self.dir, PATCHES)

    def is_local(self, rel):
        if self.local is None:
            return rel not in self.files
        return rel in self.local


# ---------------------------------------------------------------------------
# Unified diffs


class PatchSet:
    """Parsed PATCHES: preamble comments + {path: (notes, old, new, hunks)}."""

    def __init__(self):
        self.preamble = []
        self.entries = {}       # path -> dict(notes, old, new, hunks)
        self.order = []


HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def parse_patches(path):
    ps = PatchSet()
    if not os.path.exists(path):
        return ps
    with open(path, encoding=ENC, newline="") as f:
        lines = f.read().splitlines(keepends=True)
    i = 0
    notes = []
    first = True
    while i < len(lines):
        line = lines[i]
        if line.startswith("--- "):
            if i + 1 >= len(lines) or not lines[i + 1].startswith("+++ "):
                die("%s:%d: '--- ' without '+++ '" % (path, i + 1))
            old = line[4:].rstrip("\r\n").split("\t")[0]
            new = lines[i + 1][4:].rstrip("\r\n").split("\t")[0]
            rel = (new if new != "/dev/null" else old)
            if not rel.startswith(("a/", "b/")):
                die("%s:%d: bad path %r" % (path, i + 1, rel))
            rel = rel[2:]
            i += 2
            hunks = []
            while i < len(lines) and lines[i].startswith("@@"):
                m = HUNK_RE.match(lines[i])
                if not m:
                    die("%s:%d: bad hunk header" % (path, i + 1))
                ostart = int(m.group(1))
                olen = int(m.group(2)) if m.group(2) is not None else 1
                nlen = int(m.group(4)) if m.group(4) is not None else 1
                i += 1
                a, b = [], []
                last = None
                while (olen > 0 or nlen > 0 or
                       (i < len(lines) and lines[i].startswith("\\"))):
                    if i >= len(lines):
                        die("%s: truncated hunk for %s" % (path, rel))
                    hl = lines[i]
                    tag, body = hl[:1], hl[1:]
                    if tag == "\\":
                        # "\ No newline at end of file": strip the previous
                        # line's newline on the side(s) it belongs to.
                        if last is None:
                            die("%s:%d: misplaced '\\'" % (path, i + 1))
                        for side in last:
                            side[-1] = side[-1].rstrip("\n")
                        last = None
                    elif tag == " " or hl in ("\n", "\r\n"):
                        if hl in ("\n", "\r\n"):
                            body = hl
                        a.append(body)
                        b.append(body)
                        olen -= 1
                        nlen -= 1
                        last = (a, b)
                    elif tag == "-":
                        a.append(body)
                        olen -= 1
                        last = (a,)
                    elif tag == "+":
                        b.append(body)
                        nlen -= 1
                        last = (b,)
                    else:
                        die("%s:%d: bad hunk line" % (path, i + 1))
                    if olen < 0 or nlen < 0:
                        die("%s:%d: hunk longer than its header" % (path, i + 1))
                    i += 1
                hunks.append((ostart, a, b))
            if rel in ps.entries:
                die("%s: %s patched twice" % (path, rel))
            ps.entries[rel] = dict(notes=notes, old=old, new=new, hunks=hunks)
            ps.order.append(rel)
            notes = []
            first = False
        elif line.startswith("#") or not line.strip():
            if first:
                ps.preamble.append(line)
            elif line.strip():
                notes.append(line)
            i += 1
        else:
            die("%s:%d: unexpected line outside a diff" % (path, i + 1))
    if notes:
        die("%s: notes at the end without a diff" % path)
    return ps


def apply_hunks(rel, text, hunks, where):
    lines = text.splitlines(keepends=True) if text is not None else []
    offset = 0
    for ostart, a, b in hunks:
        want = ostart - 1 + offset if a else ostart + offset
        if not a:
            pos = min(max(want, 0), len(lines))
        else:
            pos = None
            n = len(a)
            # nearest exact match to the expected position
            for d in range(0, len(lines) + 1):
                for cand in (want - d, want + d) if d else (want,):
                    if 0 <= cand <= len(lines) - n and lines[cand:cand + n] == a:
                        pos = cand
                        break
                if pos is not None:
                    break
            if pos is None:
                die("%s: hunk at line %d of %s does not apply"
                    % (where, ostart, rel))
        lines[pos:pos + len(a)] = b
        offset += len(b) - len(a) + (pos - want if a else 0)
    return "".join(lines)


def make_diff(rel, old, new):
    a = old.splitlines(keepends=True) if old is not None else []
    b = new.splitlines(keepends=True) if new is not None else []
    out = []
    for line in difflib.unified_diff(
            a, b, "a/" + rel if old is not None else "/dev/null",
            "b/" + rel if new is not None else "/dev/null", n=3):
        if not line.endswith("\n"):
            line += "\n\\ No newline at end of file\n"
        out.append(line)
    return out


# ---------------------------------------------------------------------------
# Expected trees


def pristine(tree, area):
    return {rel: tree.read(area.prefix + rel) for rel in area.files}


def expected(tree, area, base):
    want = dict(base)
    ps = parse_patches(area.patch_path)
    where = "capi/%s/%s" % (area.name, PATCHES)
    for rel in ps.order:
        e = ps.entries[rel]
        if (rel in (PATCHES, SOURCE) or
                (area.local is None and rel not in base) or
                (area.local is not None and rel in area.local)):
            die("%s patches the local file %s" % (where, rel))
        if e["old"] == "/dev/null":
            if rel in want:
                die("%s creates %s, which exists upstream" % (where, rel))
            src = None
        else:
            if rel not in want:
                die("%s patches %s, which is not vendored" % (where, rel))
            src = want[rel]
        if e["new"] == "/dev/null":
            del want[rel]
        else:
            want[rel] = apply_hunks(rel, src, e["hunks"], where)
    return want


def source_text(tree):
    return ("# Generated by tools/capi_sync.py; do not edit.\n"
            "tag: %s\ncommit: %s\n" % (CPYTHON_TAG, tree.commit))


def disk_files(area):
    out = []
    if not os.path.isdir(area.dir):
        return out
    for dirpath, dirnames, filenames in os.walk(area.dir):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            out.append(os.path.relpath(full, area.dir).replace(os.sep, "/"))
    return sorted(out)


def read_disk(area, rel):
    with open(os.path.join(area.dir, rel), encoding=ENC, newline="") as f:
        return f.read()


def write_disk(area, rel, text):
    dest = os.path.join(area.dir, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".tmp"
    with open(tmp, "w", encoding=ENC, newline="") as f:
        f.write(text)
    os.chmod(tmp, 0o644)
    os.replace(tmp, dest)


def compare(area, want):
    """(missing, differing, stale) relative paths."""
    have = set(disk_files(area))
    missing = sorted(p for p in want if p not in have)
    differing = sorted(p for p in want if p in have and read_disk(area, p) != want[p])
    stale = sorted(p for p in have
                   if p not in want and p != PATCHES and not area.is_local(p))
    return missing, differing, stale


# ---------------------------------------------------------------------------
# Modes


def do_record(tree, area):
    base = pristine(tree, area)
    old = parse_patches(area.patch_path)
    have = set(disk_files(area))
    missing = [p for p in base if p not in have]
    if missing:
        die("capi/%s is incomplete (%d upstream file(s) missing, e.g. %s); "
            "sync first" % (area.name, len(missing), missing[0]))
    out = list(old.preamble)
    if not out:
        out = ["# capi/%s patches against cpython %s (tools/capi_sync.py --record).\n"
               % (area.name, CPYTHON_TAG), "\n"]
    n = 0
    for rel in sorted(have):
        if rel in (PATCHES, SOURCE) or area.is_local(rel):
            continue
        cur = read_disk(area, rel)
        src = base.get(rel)
        if src == cur:
            continue
        diff = make_diff(rel, src, cur)
        e = old.entries.get(rel)
        if out and out[-1].strip():
            out.append("\n")
        if e:
            out.extend(e["notes"])
        out.extend(diff)
        n += 1
    text = "".join(out)
    cur = read_disk(area, PATCHES) if PATCHES in have else None
    if cur != text:
        write_disk(area, PATCHES, text)
    print("capi_sync: capi/%s/%s: %d patched file(s)" % (area.name, PATCHES, n))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if capi/ differs from tag + PATCHES")
    ap.add_argument("--force", action="store_true",
                    help="overwrite differing files and remove stale ones")
    ap.add_argument("--record", action="store_true",
                    help="rewrite PATCHES from the current files")
    ap.add_argument("--area", choices=["include", "compat", "modules"],
                    action="append", help="restrict to one area (default: all)")
    args = ap.parse_args()
    if sum((args.check, args.force, args.record)) > 1:
        die("--check, --force and --record are exclusive")

    tree = CPythonTree()
    areas = []
    names = args.area or ["include", "compat", "modules"]
    if "include" in names:
        areas.append(Area("include", "Include/", include_files(tree),
                          ["pyconfig.h"]))
    if "compat" in names:
        for rel in COMPAT_COPIES:
            if "Include/internal/" + rel not in tree.files:
                die("Include/internal/%s is not in %s" % (rel, CPYTHON_TAG))
        areas.append(Area("compat", "Include/internal/", list(COMPAT_COPIES),
                          None))
    if "modules" in names:
        areas.append(Area("modules", "Modules/", module_closure(tree), []))

    ok = True
    for area in areas:
        if args.record:
            do_record(tree, area)
            continue
        want = expected(tree, area, pristine(tree, area))
        if area.name == "modules":
            want[SOURCE] = source_text(tree)
        missing, differing, stale = compare(area, want)
        if args.check:
            for p in missing:
                print("capi_sync: missing: capi/%s/%s" % (area.name, p))
            for p in differing:
                print("capi_sync: differs from %s + PATCHES: capi/%s/%s"
                      % (CPYTHON_TAG, area.name, p))
            for p in stale:
                print("capi_sync: stale: capi/%s/%s" % (area.name, p))
            ok = ok and not (missing or differing or stale)
            continue
        if (differing or stale) and not args.force:
            # Creating missing files never loses work: do it anyway.
            for p in missing:
                write_disk(area, p, want[p])
            for p in differing:
                print("capi_sync: local change not in PATCHES: capi/%s/%s"
                      % (area.name, p), file=sys.stderr)
            for p in stale:
                print("capi_sync: stale: capi/%s/%s" % (area.name, p),
                      file=sys.stderr)
            die("refusing to overwrite capi/%s (%d missing file(s) written; "
                "use --record to keep the changes, --force to discard them)"
                % (area.name, len(missing)))
        for p in missing + differing:
            write_disk(area, p, want[p])
        for p in stale:
            os.remove(os.path.join(area.dir, p))
        for dirpath, dirnames, filenames in os.walk(area.dir, topdown=False):
            if dirpath != area.dir and not os.listdir(dirpath):
                os.rmdir(dirpath)
        print("capi_sync: capi/%s: %d written, %d removed, %d file(s)"
              % (area.name, len(missing) + len(differing), len(stale), len(want)))
    tree.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
