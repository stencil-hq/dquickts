"""L0 golden: shutil copy/move/rmtree, glob, and tempfile inside a temp dir.

Temporary paths are never printed; only paths relative to the temp root.
"""
import glob
import os
import shutil
import tempfile


def listing(root):
    """Return sorted relative paths of every file and directory below *root*."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames:
            out.append(os.path.relpath(os.path.join(dirpath, name), root) + "/")
        for name in filenames:
            out.append(os.path.relpath(os.path.join(dirpath, name), root))
    return sorted(out)


def write(path, text):
    """Write *text* to *path* as UTF-8."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def read(path):
    """Return the UTF-8 text of *path*."""
    with open(path, encoding="utf-8") as fh:
        return fh.read()


with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, "src")
    os.makedirs(os.path.join(src, "pkg", "sub"))
    write(os.path.join(src, "a.txt"), "alpha")
    write(os.path.join(src, "b.log"), "beta")
    write(os.path.join(src, "pkg", "__init__.py"), "")
    write(os.path.join(src, "pkg", "mod.py"), "x = 1\n")
    write(os.path.join(src, "pkg", "sub", "deep.txt"), "deep")
    print("initial", listing(tmp))

    # copy / copy2 / copyfile
    dest = shutil.copy(os.path.join(src, "a.txt"), os.path.join(tmp, "copy.txt"))
    print("copy returns", os.path.relpath(dest, tmp), read(dest))
    os.mkdir(os.path.join(tmp, "out"))
    dest = shutil.copy(os.path.join(src, "b.log"), os.path.join(tmp, "out"))
    print("copy into dir", os.path.relpath(dest, tmp))
    shutil.copyfile(os.path.join(src, "a.txt"), os.path.join(tmp, "out", "cf.txt"))
    shutil.copy2(os.path.join(src, "a.txt"), os.path.join(tmp, "out", "c2.txt"))
    print("copyfile/copy2", sorted(os.listdir(os.path.join(tmp, "out"))))
    try:
        shutil.copyfile(os.path.join(src, "a.txt"), os.path.join(src, "a.txt"))
    except shutil.SameFileError as e:
        print("same file", type(e).__name__)
    try:
        shutil.copy(os.path.join(tmp, "missing"), tmp)
    except FileNotFoundError as e:
        print("copy missing", type(e).__name__)

    # copytree
    dst = shutil.copytree(src, os.path.join(tmp, "tree"))
    print("copytree", os.path.relpath(dst, tmp), listing(dst))
    try:
        shutil.copytree(src, os.path.join(tmp, "tree"))
    except FileExistsError as e:
        print("copytree exists", type(e).__name__)
    shutil.copytree(src, os.path.join(tmp, "tree"), dirs_exist_ok=True)
    print("dirs_exist_ok ok")
    shutil.copytree(src, os.path.join(tmp, "filtered"), ignore=shutil.ignore_patterns("*.log", "sub"))
    print("ignore_patterns", listing(os.path.join(tmp, "filtered")))

    # move
    moved = shutil.move(os.path.join(tmp, "copy.txt"), os.path.join(tmp, "out"))
    print("move into dir", os.path.relpath(moved, tmp))
    moved = shutil.move(os.path.join(tmp, "filtered"), os.path.join(tmp, "renamed"))
    print("move dir", os.path.relpath(moved, tmp), os.path.exists(os.path.join(tmp, "filtered")))

    # rmtree
    shutil.rmtree(os.path.join(tmp, "tree"))
    print("rmtree", os.path.exists(os.path.join(tmp, "tree")))
    try:
        shutil.rmtree(os.path.join(tmp, "tree"))
    except FileNotFoundError as e:
        print("rmtree missing", type(e).__name__)
    shutil.rmtree(os.path.join(tmp, "tree"), ignore_errors=True)
    print("after ops", listing(tmp))

    # glob, relative to root_dir and with absolute patterns
    print("glob *", sorted(glob.glob("*", root_dir=src)))
    print("glob *.txt", sorted(glob.glob("*.txt", root_dir=src)))
    print("glob recursive", sorted(glob.glob("**/*.py", root_dir=src, recursive=True)))
    print("glob all recursive", sorted(glob.glob("**", root_dir=src, recursive=True)))
    print("glob charclass", sorted(glob.glob("[ab].*", root_dir=src)))
    print("glob question", sorted(glob.glob("?.log", root_dir=src)))
    abs_hits = glob.glob(os.path.join(glob.escape(src), "pkg", "*"))
    print("glob absolute", sorted(os.path.relpath(p, src) for p in abs_hits))
    print("glob none", glob.glob("*.zzz", root_dir=src))
    print("glob temp root", sorted(glob.glob("*", root_dir=tmp)))
    print("glob escape", glob.escape("a[b]*?.txt"))
    print("iglob", sorted(glob.iglob("pkg/*", root_dir=src)))

    # tempfile, names never printed
    with tempfile.NamedTemporaryFile("w+", dir=tmp, prefix="pre_", suffix=".tmp", encoding="utf-8") as nt:
        nt.write("named temp\n")
        nt.flush()
        nt.seek(0)
        base = os.path.basename(nt.name)
        print("named read", repr(nt.read()))
        print("named prefix/suffix", base.startswith("pre_"), base.endswith(".tmp"))
        print("named exists", os.path.exists(nt.name))
        name = nt.name
    print("named deleted", os.path.exists(name))
    with tempfile.TemporaryFile() as tf:
        tf.write(b"anon")
        tf.seek(0)
        print("TemporaryFile", tf.read())
    with tempfile.SpooledTemporaryFile(max_size=10) as sp:
        sp.write(b"spool")
        sp.seek(0)
        print("Spooled", sp.read())
    sub = tempfile.mkdtemp(dir=tmp)
    print("mkdtemp is dir", os.path.isdir(sub), os.path.dirname(sub) == tmp)
    fd, fpath = tempfile.mkstemp(dir=tmp, suffix=".x")
    os.close(fd)
    print("mkstemp", os.path.isfile(fpath), fpath.endswith(".x"))
print("cleaned up", os.path.exists(tmp))
