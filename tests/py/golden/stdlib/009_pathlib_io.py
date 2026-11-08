"""L0 golden: pathlib.Path file system I/O inside a temporary directory.

Only paths relative to the temporary directory are printed.
"""
import errno
import tempfile
from pathlib import Path


def rel(paths, root):
    """Return sorted POSIX strings of *paths* relative to *root*."""
    return sorted(p.relative_to(root).as_posix() for p in paths)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    print("root exists", root.exists(), root.is_dir(), root.is_file())

    f = root / "hello.txt"
    print("not yet", f.exists())
    print("write_text returns", f.write_text("héllo\nworld\n", encoding="utf-8"))
    print("read_text", repr(f.read_text(encoding="utf-8")))
    print("read_bytes", f.read_bytes())
    print("size", f.stat().st_size)
    print("exists", f.exists(), f.is_file(), f.is_dir())

    b = root / "data.bin"
    print("write_bytes returns", b.write_bytes(bytes(range(8))))
    print("read back", list(b.read_bytes()))

    deep = root / "a" / "b" / "c"
    deep.mkdir(parents=True)
    print("mkdir parents", deep.is_dir())
    deep.mkdir(parents=True, exist_ok=True)
    print("exist_ok ok")
    try:
        deep.mkdir()
    except FileExistsError as e:
        print("mkdir exists", type(e).__name__, errno.errorcode[e.errno])
    try:
        (root / "x" / "y").mkdir()
    except FileNotFoundError as e:
        print("mkdir no parent", type(e).__name__, errno.errorcode[e.errno])

    (root / "a" / "one.py").write_text("print(1)\n")
    (root / "a" / "b" / "two.py").write_text("print(2)\n")
    (root / "a" / "b" / "c" / "three.txt").write_text("3\n")
    (root / "a" / "b" / "notes.md").write_text("# n\n")

    print("iterdir root", rel(root.iterdir(), root))
    print("iterdir a", rel((root / "a").iterdir(), root))
    print("glob *.txt", rel(root.glob("*.txt"), root))
    print("glob a/*/*.py", rel(root.glob("a/*/*.py"), root))
    print("glob **/*.py", rel(root.glob("**/*.py"), root))
    print("rglob *.txt", rel(root.rglob("*.txt"), root))
    print("rglob *", rel(root.rglob("*"), root))
    print("glob none", rel(root.glob("*.nothing"), root))

    # rename / replace
    moved = f.rename(root / "renamed.txt")
    print("rename result", moved.relative_to(root).as_posix(), f.exists(), moved.exists())
    (root / "other.txt").write_text("other")
    (root / "other.txt").replace(moved)
    print("replace content", moved.read_text())

    # append via open
    with moved.open("a", encoding="utf-8") as fh:
        fh.write("+more")
    print("appended", moved.read_text())
    with moved.open("rb") as fh:
        print("open rb", fh.read(5))

    # touch and unlink
    t = root / "touched"
    t.touch()
    print("touch size", t.stat().st_size)
    t.unlink()
    print("unlinked", t.exists())
    try:
        t.unlink()
    except FileNotFoundError as e:
        print("unlink missing", type(e).__name__, errno.errorcode[e.errno])
    t.unlink(missing_ok=True)
    print("missing_ok ok")

    try:
        (root / "nope.txt").read_text()
    except FileNotFoundError as e:
        print("read missing", type(e).__name__, errno.errorcode[e.errno])
    try:
        (root / "a").read_text()
    except IsADirectoryError as e:
        print("read dir", type(e).__name__, errno.errorcode[e.errno])

    # rmdir
    empty = root / "empty"
    empty.mkdir()
    empty.rmdir()
    print("rmdir", empty.exists())
    try:
        (root / "a").rmdir()
    except OSError as e:
        print("rmdir nonempty", type(e).__name__, errno.errorcode[e.errno])

    # resolve and samefile (printed relative)
    r = (root / "a" / ".." / "renamed.txt").resolve()
    print("resolve rel", r.relative_to(root.resolve()).as_posix())
    print("samefile", moved.samefile(root / "a" / ".." / "renamed.txt"))
    print("absolute", (root / "z").is_absolute())
    print("final tree", rel(root.rglob("*"), root))
