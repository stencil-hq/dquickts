#!/usr/bin/env python3.15
"""Sync the pinned pure-Python standard library from cpython/Lib into pylib/.

pylib/ holds the CPython 3.15.2 standard-library modules that quickpy runs
as Python source (everything else is provided natively by the VM or by
CPython's C modules built over the C API layer).  It is the whole of Lib/
except the entries listed in EXCLUDED below (the test suite, which L3 runs
from cpython/Lib/test directly, and the Tk-based GUI packages).

pylib/MANIFEST.txt describes the copied set: one path per line, relative
to CPython's Lib/ directory (``os.py`` for a module file, ``json/`` for a
package directory, copied recursively without ``test``/``tests``/
``idle_test`` subdirectories, ``__pycache__`` and ``*.pyc``).  ``#``
starts a comment; the trailing comment sections of the generated manifest
list the native (C) modules each pure module imports.

Files are read from the git objects of the pinned tag, not from the work
tree, so local edits or patches in cpython/ never leak into pylib/.  The
one exception is WHEEL_EDITS: members of the wheels bundled with ensurepip
that quickpy has to adjust (see there); the wheel is rewritten with the
edited members and its RECORD updated, and ``--check`` compares the
members' contents.  pylib/site-packages/ (where pip installs into the
default environment) and pylib/test (a symlink to cpython/Lib/test that
``tests/py/run.py l3`` creates, so that ``qpy -I`` subprocesses of the
tests find the ``test`` package as in a CPython install) are never touched.

Usage:
  tools/pylib_sync.py                 sync pylib/ from pylib/MANIFEST.txt
  tools/pylib_sync.py --check         exit 1 if pylib/ differs from a sync
  tools/pylib_sync.py --manifest      regenerate MANIFEST.txt (Lib/ minus
                                      EXCLUDED, native-module sections from
                                      a static import scan), then sync
  tools/pylib_sync.py --manifest --check
                                      also exit 1 if MANIFEST.txt is stale

Stdlib only; run with the host CPython 3.15 (needed by --manifest for the
ast module and sys.stdlib_module_names of the same version).
"""

import argparse
import ast
import base64
import csv
import fnmatch
import hashlib
import io
import os
import re
import subprocess
import zipfile
import sys

CPYTHON_TAG = "v3.15.2"

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPYTHON_DIR = os.path.join(ROOT_DIR, "cpython")
PYLIB_DIR = os.path.join(ROOT_DIR, "pylib")
MANIFEST = "MANIFEST.txt"
SOURCE = "SOURCE"
KEEP = {MANIFEST, SOURCE}
# Top-level pylib/ directories owned by the user (pip's default install
# target, on sys.path after the stdlib): never listed as stale or removed.
USER_DIRS = {"site-packages"}
# pylib/test: symlink to cpython/Lib/test made by tests/py/run.py l3
# (gitignored); never listed as stale or removed.
TEST_LINK = "test"
# pylib/_test*.so: symlinks to the test-only C extension modules made by
# tests/py/run.py (gitignored); never listed as stale or removed.
TEST_MODULE_LINKS = "_test*.so"

# Subdirectories never copied when a package directory is copied.
EXCLUDED_DIRS = {"test", "tests", "idle_test", "__pycache__"}

# Top-level Lib/ entries that are not copied (module or package names;
# every other file and directory of Lib/ is).
EXCLUDED = {
    "test": "CPython's test suite; L3 runs it from cpython/Lib/test",
    "idlelib": "IDLE (Tk GUI)",
    "tkinter": "Tk GUI bindings",
    "turtle": "Tk turtle graphics",
    "turtledemo": "turtle demos",
    "site-packages": "pip's install target (README.txt only)",
}

# Edits of members of wheels bundled under ensurepip/_bundled:
# {wheel: {member: [(old, new), ...]}}; each `old` must occur exactly once.
#
# pip's vendored packaging.tags decides which wheels this interpreter can
# install from sys.implementation.name.  quickpy is not "cpython", so
# unmodified packaging only offers py3-none-* and quickpy315-* tags and no
# binary wheel from PyPI matches.  quickpy loads CPython's stable ABI
# (abi3, python/capi_dynload.c) and nothing version-specific, so the edits
# make packaging treat "quickpy" as a cp interpreter (cp3XX-abi3-<platform>
# for X <= 15, cp315-none-*, py3-none-*) whose own ABI list is empty, which
# drops cp315-cp315-* wheels.  Wheels built from source against quickpy's own
# headers (plan D5 Tier S: EXT_SUFFIX ".quickpy-315-<multiarch>.so", which
# meson-python and setuptools tag quickpy315-quickpy_315_<multiarch>-<plat>)
# come first in cpython_tags(), which both sys_tags() and pip's
# compatibility_tags.get_supported() call.
PIP_WHEEL = "ensurepip/_bundled/pip-26.2.2-py3-none-any.whl"
WHEEL_EDITS = {
    PIP_WHEEL: {
        "pip/_vendor/packaging/tags.py": [
            ('    "cpython": "cp",\n',
             '    "cpython": "cp",\n'
             '    # quickpy (dquickts) runs CPython\'s stable ABI; see _cpython_abis\n'
             '    "quickpy": "cp",\n'),
            ('    py_version = tuple(py_version)  # To allow for version comparison.\n'
             '    abis = []\n',
             '    py_version = tuple(py_version)  # To allow for version comparison.\n'
             '    if sys.implementation.name == "quickpy":\n'
             '        # quickpy loads stable-ABI (abi3) extension modules only, never\n'
             '        # version-specific cpXY ones: cpython_tags() then offers the abi3\n'
             '        # and "none" tags.\n'
             '        return []\n'
             '    abis = []\n'),
            ('    interpreter = f"cp{_version_nodot(python_version[:2])}"\n',
             '    interpreter = f"cp{_version_nodot(python_version[:2])}"\n'
             '    if (sys.implementation.name == "quickpy"\n'
             '            and tuple(python_version[:2]) == sys.version_info[:2]):\n'
             '        # quickpy\'s own ABI (Tier S source builds against its headers,\n'
             '        # EXT_SUFFIX ".quickpy-315-<multiarch>.so"), before the stable ABI\n'
             '        platforms = list(platforms or platform_tags())\n'
             '        tier_s = f"quickpy{_version_nodot(python_version[:2])}"\n'
             '        for abi in _generic_abi():\n'
             '            for platform_ in platforms:\n'
             '                yield Tag(tier_s, abi, platform_)\n'),
        ],
        # uninstalling (also pip install --force-reinstall/--upgrade) asks
        # importlib.util.cache_from_source for each .py file's __pycache__
        # entry; quickpy writes no bytecode caches (sys.implementation.
        # cache_tag is None), where cache_from_source raises
        # NotImplementedError
        "pip/_internal/req/req_uninstall.py": [
            ('        if os.path.splitext(path)[1] == ".py":\n',
             '        if (os.path.splitext(path)[1] == ".py"\n'
             '                and sys.implementation.cache_tag is not None):\n'),
        ],
    },
}

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


def wheel_members(data):
    """{member name: bytes} of a wheel (zip) given as bytes."""
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        return {info.filename: zf.read(info) for info in zf.infolist()}


def edit_wheel(data, edits, name):
    """The wheel `data` with WHEEL_EDITS applied and its RECORD updated."""
    with zipfile.ZipFile(io.BytesIO(data)) as src:
        infos = src.infolist()
        members = {info.filename: src.read(info) for info in infos}
    new = {}
    for member, pairs in edits.items():
        if member not in members:
            die("%s: no member %s to edit" % (name, member))
        text = members[member].decode("utf-8")
        for old, repl in pairs:
            if text.count(old) != 1:
                die("%s: %s: edit anchor found %d times (expected once): %r"
                    % (name, member, text.count(old), old))
            text = text.replace(old, repl)
        new[member] = text.encode("utf-8")
    records = [m for m in members if m.endswith(".dist-info/RECORD")]
    if len(records) != 1:
        die("%s: expected one .dist-info/RECORD" % name)
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    for row in csv.reader(io.StringIO(members[records[0]].decode("utf-8"))):
        if row and row[0] in new:
            digest = hashlib.sha256(new[row[0]]).digest()
            row = [row[0], "sha256=" + base64.urlsafe_b64encode(digest)
                   .rstrip(b"=").decode("ascii"), str(len(new[row[0]]))]
        writer.writerow(row)
    new[records[0]] = out.getvalue().encode("utf-8")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as dst:
        for info in infos:
            zi = zipfile.ZipInfo(info.filename, info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            zi.create_system = info.create_system
            dst.writestr(zi, new.get(info.filename, members[info.filename]))
    return buf.getvalue()


def expected_data(tree, path):
    """Content of pylib/<path> after a sync (WHEEL_EDITS applied)."""
    data = tree.read(path)
    if path in WHEEL_EDITS:
        data = edit_wheel(data, WHEEL_EDITS[path], path)
    return data


def source_text(tree):
    return ("# Generated by tools/pylib_sync.py; do not edit.\n"
            "tag: %s\ncommit: %s\n" % (CPYTHON_TAG, tree.commit)).encode()


def existing_files():
    """Files currently under pylib/, relative to it (sorted)."""
    found = []
    for dirpath, dirnames, filenames in os.walk(PYLIB_DIR):
        dirnames.sort()
        rel = os.path.relpath(dirpath, PYLIB_DIR)
        # __pycache__/: bytecode caches qpy writes at import time
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        if rel == ".":
            dirnames[:] = [d for d in dirnames
                           if d not in USER_DIRS and d != TEST_LINK]
            filenames = [f for f in filenames if f != TEST_LINK
                         and not (fnmatch.fnmatchcase(f, TEST_MODULE_LINKS)
                                  and os.path.islink(os.path.join(dirpath, f)))]
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
                have = f.read()
            if path in WHEEL_EDITS:
                # the deflate output depends on the zlib version: compare
                # the members
                try:
                    same = (wheel_members(have)
                            == wheel_members(expected_data(tree, path)))
                except zipfile.BadZipFile:
                    same = False
                if same:
                    continue
            elif blob_id(have, len(oid)) == oid:
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
        data = source_text(tree) if p == SOURCE else expected_data(tree, p)
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
        rel = os.path.relpath(dirpath, PYLIB_DIR).replace(os.sep, "/")
        if rel.split("/")[0] in USER_DIRS:
            continue
        if dirpath != PYLIB_DIR and not os.listdir(dirpath):
            os.rmdir(dirpath)
    print("pylib_sync: %d file(s) written, %d removed, %d selected"
          % (len(to_write), len(to_remove), len(selected)))
    return True


# ---------------------------------------------------------------------------
# Manifest generation (--manifest)


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
            # CPython's test suite (Lib/test, pkg/tests, ...) and EXCLUDED
            # are never copied.
            if parts[0] in EXCLUDED or any(s in EXCLUDED_DIRS for s in parts):
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


def manifest_entries(tree):
    """Top-level Lib/ entries minus EXCLUDED: `name.py` files, `dir/`s."""
    entries = set()
    for path in tree.files:
        top = path.split("/", 1)[0]
        name = top[:-3] if top.endswith(".py") else top
        if name in EXCLUDED or top in EXCLUDED_DIRS or top.endswith(".pyc"):
            continue
        entries.add(top + "/" if "/" in path else top)
    return sorted(entries, key=lambda e: e.rstrip("/"))


def scan_natives(tree, selected):
    """Classify the imports of the copied modules that are not copied.

    Returns (natives, missing_sub, excluded), each mapping a module name to
    {"importers": set, "required": bool}: natives are top-level modules
    that are not Python files in Lib/ (C extensions, builtins, frozen
    aliases, third-party optional imports); missing_sub are submodules of
    a copied package that do not exist; excluded are modules of EXCLUDED
    and of CPython's test suite.  required means at least one unguarded
    import of it exists.
    """
    idx = ModuleIndex(tree)
    selected = set(selected)
    natives = {}
    missing_sub = {}
    excluded = {}
    aliases = set()     # names installed into sys.modules by scanned code

    def note(table, name, importer, guarded):
        info = table.setdefault(name, {"importers": set(), "required": False})
        info["importers"].add(importer)
        if not guarded:
            info["required"] = True

    def resolve(name, importer, guarded):
        """Record an import of dotted `name`; return True if it is a module."""
        parts = name.split(".")
        for i in range(1, len(parts) + 1):
            n = ".".join(parts[:i])
            if n not in idx.path:
                if n in idx.excluded or parts[0] in EXCLUDED:
                    table = excluded
                else:
                    table = natives if i == 1 else missing_sub
                note(table, n, importer, guarded)
                return False
            if n not in idx.packages:
                return True     # rest is an attribute / sys.modules alias
        return True

    for mod in sorted(idx.path):
        path = idx.path[mod]
        if path not in selected:
            continue
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
            resolve(base, mod, guarded)

    for name in aliases:
        missing_sub.pop(name, None)
    return natives, missing_sub, excluded


def render_manifest(tree):
    if sys.version_info[:2] != (3, 15):
        die("--manifest needs the host CPython 3.15 (sys.stdlib_module_names)")
    entries = manifest_entries(tree)
    natives, missing_sub, excluded = scan_natives(
        tree, parse_manifest(tree, "\n".join(entries)))
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
    not_copied = [fmt(name, excluded[name]) for name in sorted(excluded)]

    lines = [
        "# pylib/MANIFEST.txt -- pure-Python stdlib copied into pylib/ from",
        "# cpython/Lib at %s by tools/pylib_sync.py." % CPYTHON_TAG,
        "#",
        "# One path per line relative to Lib/: `name.py` copies that file,",
        "# `dir/` copies the package recursively (without test/, tests/,",
        "# idle_test/, __pycache__/).  `#` starts a comment.",
        "#",
        "# Generated by `tools/pylib_sync.py --manifest`: every top-level entry",
        "# of Lib/ except the ones below (EXCLUDED in the script); edit EXCLUDED",
        "# and regenerate rather than editing this list by hand.",
        "#",
        "# not copied:",
    ]
    for name in sorted(EXCLUDED):
        lines.append("#   %-16s %s" % (name, EXCLUDED[name]))
    lines.append("")
    lines += entries
    lines += [
        "",
        "# Native modules imported by the copied modules (static scan of",
        "# module- and function-level imports; `name <- importers`).",
        "#",
        "# provided natively:",
        "# Imported unconditionally by at least one module (C extensions,",
        "# builtins, frozen aliases); the importers need them to work.",
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
        "# Not copied (EXCLUDED / CPython test suite; imported only by test",
        "# helpers, demos or optional GUI features):",
    ] + not_copied
    return "\n".join(lines) + "\n"


def regen_manifest(tree, check):
    text = render_manifest(tree)
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
                    help="regenerate pylib/MANIFEST.txt first")
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
