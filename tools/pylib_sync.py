#!/usr/bin/env python3.15
"""Sync the pinned pure-Python stdlib subset from cpython/Lib into pylib/.

pylib/ holds the CPython 3.15.2 standard-library modules that quickpy runs
as Python source (everything else is provided natively by the VM).  The
subset is described by pylib/MANIFEST.txt: one path per line, relative to
CPython's Lib/ directory (``os.py`` for a module file, ``json/`` for a
package directory, copied recursively without ``test``/``tests``/
``idle_test`` subdirectories, ``__pycache__`` and ``*.pyc``).  ``#`` starts a
comment; the trailing comment sections of the generated manifest list the
native (C) modules the closure needs.

Files are read from the git objects of the pinned tag, not from the work
tree, so local edits or patches in cpython/ never leak into pylib/.

Usage:
  tools/pylib_sync.py                 sync pylib/ from pylib/MANIFEST.txt
  tools/pylib_sync.py --check         exit 1 if pylib/ differs from a sync
  tools/pylib_sync.py --manifest      regenerate MANIFEST.txt from ROOTS
                                      (static import closure), then sync
  tools/pylib_sync.py --manifest --check
                                      also exit 1 if MANIFEST.txt is stale

Stdlib only; run with the host CPython 3.15 (needed by --manifest for the
ast module and sys.stdlib_module_names of the same version).
"""

import argparse
import ast
import hashlib
import os
import re
import subprocess
import sys

CPYTHON_TAG = "v3.15.2"

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPYTHON_DIR = os.path.join(ROOT_DIR, "cpython")
PYLIB_DIR = os.path.join(ROOT_DIR, "pylib")
MANIFEST = "MANIFEST.txt"
SOURCE = "SOURCE"
KEEP = {MANIFEST, SOURCE}

# Subdirectories never copied when a package directory is copied.
EXCLUDED_DIRS = {"test", "tests", "idle_test", "__pycache__"}

# Roots of the import closure written by --manifest.  Packages among them
# are taken whole; every other package only contributes the modules that
# are actually imported (plus the __init__.py of each parent).
ROOTS = """
json re collections pathlib subprocess os glob shutil time struct textwrap
difflib ast tempfile hashlib sqlite3 datetime urllib concurrent.futures
statistics random importlib math itertools shlex functools dataclasses enum
typing copy operator heapq bisect fractions decimal string contextlib io
pprint keyword unicodedata csv base64 binascii zlib types abc warnings
traceback inspect dis linecache tokenize unittest doctest argparse logging
weakref threading queue signal locale codecs encodings fnmatch posixpath
genericpath stat errno numbers reprlib gettext selectors socket tarfile
zipfile gzip bz2 lzma calendar email html http asyncio pdb site sysconfig
platform uuid
""".split()

# Exception names whose handler makes a `try:` body an optional import.
IMPORT_GUARDS = {"ImportError", "ModuleNotFoundError", "Exception",
                 "BaseException"}

# Native modules that are not C extensions: frozen aliases and modules the
# interpreter creates itself.
RUNTIME_NOTES = {
    "_frozen_importlib": "frozen importlib._bootstrap",
    "_frozen_importlib_external": "frozen importlib._bootstrap_external",
    "__main__": "created by the interpreter",
}


def die(msg):
    print("pylib_sync: error: " + msg, file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Pinned CPython tree (read from git objects)


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
    """The Lib/ tree of the pinned tag: {relpath: blob id} plus blob reads."""

    def __init__(self):
        if not os.path.isdir(CPYTHON_DIR):
            die("%s is missing (run `make testpy-bootstrap`)" % CPYTHON_DIR)
        desc = git("describe", "--tags").decode().strip()
        if desc != CPYTHON_TAG:
            die("cpython/ is at %r, expected tag %s" % (desc, CPYTHON_TAG))
        self.commit = git("rev-parse", "HEAD").decode().strip()
        tag_commit = git("rev-parse", CPYTHON_TAG + "^{commit}").decode().strip()
        if tag_commit != self.commit:
            die("cpython/ HEAD %s is not tag %s (%s)"
                % (self.commit, CPYTHON_TAG, tag_commit))
        self.files = {}         # path relative to Lib -> blob id
        out = git("ls-tree", "-r", "-z", "--full-tree", self.commit, "--", "Lib")
        for rec in out.split(b"\0"):
            if not rec:
                continue
            meta, path = rec.split(b"\t", 1)
            mode, kind, oid = meta.decode().split()
            if kind != "blob" or mode == "120000":
                continue
            self.files[path.decode()[len("Lib/"):]] = oid
        self.dirs = set()
        for path in self.files:
            parts = path.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                self.dirs.add("/".join(parts[:i]))
        self._cat = None

    def read(self, path):
        """Return the bytes of Lib/<path> at the pinned commit."""
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
        return data

    def close(self):
        if self._cat is not None:
            self._cat.stdin.close()
            self._cat.wait()
            self._cat = None

    def walk_dir(self, d):
        """Files under directory d, minus excluded subdirectories and *.pyc."""
        prefix = d + "/"
        for path in self.files:
            if not path.startswith(prefix) or path.endswith(".pyc"):
                continue
            if any(p in EXCLUDED_DIRS for p in path[len(prefix):].split("/")[:-1]):
                continue
            yield path


def blob_id(data, oid_len):
    """git object id of a blob with the given content."""
    h = hashlib.sha1() if oid_len == 40 else hashlib.sha256()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Manifest parsing and sync


def parse_manifest(tree, text):
    """Return the sorted list of Lib-relative files selected by the manifest."""
    selected = set()
    for lineno, line in enumerate(text.splitlines(), 1):
        entry = line.split("#", 1)[0].strip().rstrip("/")
        if not entry:
            continue
        if entry.startswith("/") or ".." in entry.split("/"):
            die("%s:%d: invalid path %r" % (MANIFEST, lineno, entry))
        if entry in tree.files:
            selected.add(entry)
        elif entry in tree.dirs:
            selected.update(tree.walk_dir(entry))
        else:
            die("%s:%d: %r is not in cpython/Lib at %s"
                % (MANIFEST, lineno, entry, CPYTHON_TAG))
    clash = KEEP & selected
    if clash:
        die("%s: selects reserved name(s) %s" % (MANIFEST, sorted(clash)))
    return sorted(selected)


def source_text(tree):
    return ("# Generated by tools/pylib_sync.py; do not edit.\n"
            "tag: %s\ncommit: %s\n" % (CPYTHON_TAG, tree.commit)).encode()


def existing_files():
    """Files currently under pylib/, relative to it (sorted)."""
    found = []
    for dirpath, dirnames, filenames in os.walk(PYLIB_DIR):
        dirnames.sort()
        rel = os.path.relpath(dirpath, PYLIB_DIR)
        for name in sorted(filenames):
            found.append(name if rel == "." else os.path.join(rel, name)
                         .replace(os.sep, "/"))
    return found


def plan(tree, selected):
    """Compute (to_write, to_remove) needed to make pylib/ match a sync."""
    to_write = []
    for path in selected:
        dest = os.path.join(PYLIB_DIR, path)
        oid = tree.files[path]
        if os.path.isfile(dest) and not os.path.islink(dest):
            with open(dest, "rb") as f:
                if blob_id(f.read(), len(oid)) == oid:
                    continue
        to_write.append(path)
    src = os.path.join(PYLIB_DIR, SOURCE)
    want = source_text(tree)
    have = None
    if os.path.isfile(src):
        with open(src, "rb") as f:
            have = f.read()
    if have != want:
        to_write.append(SOURCE)
    keep = set(selected) | KEEP
    to_remove = [p for p in existing_files() if p not in keep]
    return to_write, to_remove


def sync(tree, selected, check):
    to_write, to_remove = plan(tree, selected)
    if check:
        for p in to_write:
            print("pylib_sync: out of date: pylib/%s" % p)
        for p in to_remove:
            print("pylib_sync: stale: pylib/%s" % p)
        return not to_write and not to_remove
    for p in to_remove:
        os.remove(os.path.join(PYLIB_DIR, p))
    for p in to_write:
        dest = os.path.join(PYLIB_DIR, p)
        data = source_text(tree) if p == SOURCE else tree.read(p)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.lexists(dest) and not os.path.isfile(dest):
            die("pylib/%s exists and is not a regular file" % p)
        tmp = dest + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, dest)
    # Drop directories emptied by removals.
    for dirpath, dirnames, filenames in os.walk(PYLIB_DIR, topdown=False):
        if dirpath != PYLIB_DIR and not os.listdir(dirpath):
            os.rmdir(dirpath)
    print("pylib_sync: %d file(s) written, %d removed, %d selected"
          % (len(to_write), len(to_remove), len(selected)))
    return True


# ---------------------------------------------------------------------------
# Import closure (--manifest)


class ModuleIndex:
    """Map dotted module names to Lib files of the pinned tree."""

    def __init__(self, tree):
        self.tree = tree
        self.path = {}          # dotted name -> Lib-relative .py file
        self.packages = set()   # dotted names that are packages
        self.excluded = set()   # dotted names inside excluded directories
        for p in tree.files:
            if not p.endswith(".py"):
                continue
            parts = p[:-3].split("/")
            if parts[-1] == "__init__":
                parts.pop()
                pkg = True
            else:
                pkg = False
            if not parts or not all(s.isidentifier() for s in parts):
                continue
            # CPython's test suite (Lib/test, pkg/tests, ...) is never copied,
            # so it is not followed either.
            if any(s in EXCLUDED_DIRS for s in parts):
                self.excluded.add(".".join(parts))
                continue
            # A module is importable only if every parent is a package.
            if any("/".join(parts[:i]) + "/__init__.py" not in tree.files
                   for i in range(1, len(parts))):
                continue
            name = ".".join(parts)
            self.path[name] = p
            if pkg:
                self.packages.add(name)

    def submodules(self, pkg):
        """All modules of package pkg, minus excluded subdirectories."""
        d = pkg.replace(".", "/")
        files = set(self.tree.walk_dir(d))
        return sorted(n for n, p in self.path.items()
                      if p in files and (n == pkg or n.startswith(pkg + ".")))


def _is_type_checking(test):
    if isinstance(test, ast.Constant) and test.value is False:
        return True
    if isinstance(test, ast.Name) and test.id == "TYPE_CHECKING":
        return True
    return isinstance(test, ast.Attribute) and test.attr == "TYPE_CHECKING"


def _guards_import(handler_type):
    if handler_type is None:
        return True
    elts = handler_type.elts if isinstance(handler_type, ast.Tuple) else [handler_type]
    for e in elts:
        name = e.id if isinstance(e, ast.Name) else (
            e.attr if isinstance(e, ast.Attribute) else None)
        if name in IMPORT_GUARDS:
            return True
    return False


def scan_imports(source, filename):
    """Return (imports, aliases) for a module source.

    imports: (kind, data, guarded) for every executed import statement.
    kind "import": data is the dotted name.  kind "from": data is
    (level, module or "", [names]).  guarded is True when the statement is
    inside the body of a try whose handlers catch ImportError (or a
    superclass); `if TYPE_CHECKING:` / `if False:` bodies are skipped.
    aliases: module names installed via `<sys>.modules["name"] = ...`.
    """
    tree = ast.parse(source, filename)
    out = []
    aliases = set()

    def visit(node, guarded):
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if (isinstance(t, ast.Subscript)
                        and isinstance(t.value, ast.Attribute)
                        and t.value.attr == "modules"
                        and isinstance(t.slice, ast.Constant)
                        and isinstance(t.slice.value, str)):
                    aliases.add(t.slice.value)
        if isinstance(node, ast.Import):
            for alias in node.names:
                out.append(("import", alias.name, guarded))
            return
        if isinstance(node, ast.ImportFrom):
            out.append(("from", (node.level, node.module or "",
                                 [a.name for a in node.names]), guarded))
            return
        if isinstance(node, ast.If) and _is_type_checking(node.test):
            for child in node.orelse:
                visit(child, guarded)
            return
        if isinstance(node, (ast.Try, ast.TryStar)):
            body_guarded = guarded or any(_guards_import(h.type)
                                          for h in node.handlers)
            for child in node.body:
                visit(child, body_guarded)
            for h in node.handlers:
                visit(h, guarded)
            for child in node.orelse + node.finalbody:
                visit(child, guarded)
            return
        if isinstance(node, ast.With) and any(
                isinstance(it.context_expr, ast.Call)
                and isinstance(it.context_expr.func, (ast.Name, ast.Attribute))
                and getattr(it.context_expr.func, "id",
                            getattr(it.context_expr.func, "attr", None)) == "suppress"
                and any(_guards_import(a) for a in it.context_expr.args)
                for it in node.items):
            for child in node.body:
                visit(child, True)
            return
        for child in ast.iter_child_nodes(node):
            visit(child, guarded)

    visit(tree, False)
    return out, aliases


def posix_native_names():
    """Native modules CPython builds on POSIX platforms."""
    names = set(sys.builtin_module_names)
    mods = os.path.join(CPYTHON_DIR, "Modules")
    for fn in ("Setup", "Setup.bootstrap.in", "Setup.stdlib.in"):
        with open(os.path.join(mods, fn), encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line or line.startswith("*"):
                    continue
                word = re.sub(r"^@MODULE_\w+_TRUE@", "", line.split()[0])
                if word.isidentifier():
                    names.add(word)
    with open(os.path.join(mods, "config.c.in"), encoding="utf-8") as f:
        names.update(re.findall(r'\{"(\w+)",', f.read()))
    return names


def compute_closure(tree):
    """Return (idx, files, whole, natives, missing_sub, excluded) for ROOTS.

    files: Lib-relative .py files of the closure; whole: root packages.
    natives / missing_sub / excluded map a module name that is not part of
    the closure (top-level non-Lib module / missing submodule of a Lib
    package / module of CPython's test suite) to
    {"importers": set, "required": bool}, where required means at least one
    unguarded import of it exists.
    """
    idx = ModuleIndex(tree)
    modules = set()
    whole = set()       # packages taken whole (roots)
    natives = {}
    missing_sub = {}    # dotted name inside a pure package -> natives-like info
    aliases = set()     # names installed into sys.modules by scanned code
    excluded = {}       # test-suite modules (never copied)
    work = []

    def add(name):
        parts = name.split(".")
        for i in range(1, len(parts) + 1):
            n = ".".join(parts[:i])
            if n not in modules:
                modules.add(n)
                work.append(n)

    def note(table, name, importer, guarded):
        info = table.setdefault(name, {"importers": set(), "required": False})
        info["importers"].add(importer)
        if not guarded:
            info["required"] = True

    def resolve(name, importer, guarded):
        """Record an import of dotted `name`; return True if it is a module."""
        parts = name.split(".")
        if parts[0] not in idx.path:
            note(excluded if parts[0] in idx.excluded else natives,
                 parts[0], importer, guarded)
            return False
        for i in range(1, len(parts) + 1):
            n = ".".join(parts[:i])
            if n not in idx.path:
                note(excluded if n in idx.excluded else missing_sub,
                     n, importer, guarded)
                return False
            add(n)
            if n not in idx.packages:
                return True     # rest is an attribute / sys.modules alias
        return True

    for root in ROOTS:
        if root in idx.packages:
            whole.add(root)
            for sub in idx.submodules(root):
                add(sub)
        resolve(root, "<root>", False)

    while work:
        mod = work.pop()
        path = idx.path[mod]
        is_pkg = mod in idx.packages
        imports, mod_aliases = scan_imports(tree.read(path), "Lib/" + path)
        aliases |= mod_aliases
        for kind, data, guarded in imports:
            if kind == "import":
                resolve(data, mod, guarded)
                continue
            level, base, names = data
            if level:
                pkg_parts = mod.split(".") if is_pkg else mod.split(".")[:-1]
                if level - 1 > len(pkg_parts) - 1:
                    die("Lib/%s: relative import beyond top-level package" % path)
                pkg_parts = pkg_parts[:len(pkg_parts) - (level - 1)]
                base = ".".join(pkg_parts + ([base] if base else []))
            if not resolve(base, mod, guarded) or base not in idx.packages:
                continue
            for n in names:
                if n != "*" and base + "." + n in idx.path:
                    add(base + "." + n)

    files = {idx.path[m] for m in modules}
    for name in aliases:
        missing_sub.pop(name, None)
    return idx, files, whole, natives, missing_sub, excluded


def manifest_entries(tree, idx, files, whole):
    """Collapse the closure into manifest paths: complete dirs or files."""
    complete = set()
    for pkg in idx.packages:
        d = pkg.replace(".", "/")
        if pkg in whole:
            complete.add(d)
            continue
        if d.split("/")[-1] in EXCLUDED_DIRS:
            continue
        py = [p for p in tree.walk_dir(d) if p.endswith(".py")]
        if py and all(p in files for p in py):
            complete.add(d)
    # Keep only the topmost complete directories (children are implied).
    top = {d for d in complete
           if not any(d.startswith(c + "/") for c in complete)}
    entries = [d + "/" for d in top]
    entries += [p for p in files if not any(p.startswith(d + "/") for d in top)]
    return sorted(entries, key=lambda e: e.rstrip("/"))


def render_manifest(tree, idx, files, whole, natives, missing_sub, excluded):
    if sys.version_info[:2] != (3, 15):
        die("--manifest needs the host CPython 3.15 (sys.stdlib_module_names)")
    stdlib = set(sys.stdlib_module_names)
    posix = posix_native_names()

    def fmt(name, info, note=""):
        imps = sorted(info["importers"])
        shown = ", ".join(imps[:4]) + (", +%d more" % (len(imps) - 4)
                                       if len(imps) > 4 else "")
        extra = "  [%s]" % note if note else ""
        return "#   %-28s <- %s%s" % (name, shown, extra)

    required, optional, windows, absent = [], [], [], []
    for name in sorted(natives):
        info = natives[name]
        if name in RUNTIME_NOTES or name in posix:
            (required if info["required"] else optional).append(
                fmt(name, info, RUNTIME_NOTES.get(name, "")))
        elif name in stdlib:
            windows.append(fmt(name, info))
        else:
            absent.append(fmt(name, info, "" if not info["required"] else "unguarded"))
    for name in sorted(missing_sub):
        info = missing_sub[name]
        absent.append(fmt(name, info, "" if not info["required"] else "unguarded"))
    tests = [fmt(name, excluded[name]) for name in sorted(excluded)]

    lines = [
        "# pylib/MANIFEST.txt -- pure-Python stdlib subset copied into pylib/",
        "# from cpython/Lib at %s by tools/pylib_sync.py." % CPYTHON_TAG,
        "#",
        "# One path per line relative to Lib/: `name.py` copies that file,",
        "# `dir/` copies the package recursively (without test/, tests/,",
        "# idle_test/, __pycache__/).  `#` starts a comment.",
        "#",
        "# Generated by `tools/pylib_sync.py --manifest`: the transitive static",
        "# import closure (module- and function-level imports) of the roots",
        "# below; root packages are taken whole.  Edit ROOTS in the script",
        "# and regenerate rather than editing this list by hand.",
        "#",
        "# roots:",
    ]
    line = "#  "
    for r in ROOTS:
        if len(line) + len(r) + 1 > 76:
            lines.append(line)
            line = "#  "
        line += " " + r
    lines.append(line)
    lines.append("")
    lines += manifest_entries(tree, idx, files, whole)
    lines += [
        "",
        "# provided natively:",
        "# Modules imported by the closure that are not Python files in Lib",
        "# (C extensions, builtins, frozen aliases); M3 must provide them.",
    ] + required + [
        "",
        "# provided natively, optional:",
        "# Only imported under try/except ImportError (C accelerators and",
        "# optional features with a fallback); not needed for correctness.",
    ] + optional + [
        "",
        "# Windows-only native modules (imported on win32 code paths only):",
    ] + windows + [
        "",
        "# Not in CPython (third-party / other-implementation optional imports):",
    ] + absent + [
        "",
        "# CPython test suite (not copied; imported only by test helpers):",
    ] + tests
    return "\n".join(lines) + "\n"


def regen_manifest(tree, check):
    text = render_manifest(tree, *compute_closure(tree))
    path = os.path.join(PYLIB_DIR, MANIFEST)
    old = None
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    if old == text:
        return text, True
    if check:
        print("pylib_sync: out of date: pylib/%s" % MANIFEST)
        return text, False
    os.makedirs(PYLIB_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("pylib_sync: wrote pylib/%s" % MANIFEST)
    return text, True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if pylib/ is out of date")
    ap.add_argument("--manifest", action="store_true",
                    help="regenerate pylib/MANIFEST.txt from ROOTS first")
    args = ap.parse_args()

    tree = CPythonTree()
    try:
        ok = True
        if args.manifest:
            text, ok = regen_manifest(tree, args.check)
        else:
            path = os.path.join(PYLIB_DIR, MANIFEST)
            if not os.path.isfile(path):
                die("pylib/%s is missing (run with --manifest)" % MANIFEST)
            with open(path, encoding="utf-8") as f:
                text = f.read()
        selected = parse_manifest(tree, text)
        ok = sync(tree, selected, args.check) and ok
    finally:
        tree.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
