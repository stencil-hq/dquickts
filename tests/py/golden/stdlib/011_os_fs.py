"""L0 golden: os file system functions inside a temporary directory.

Only names relative to the temporary directory are printed.
"""
import errno
import os
import tempfile


def tree(root):
    """Return a sorted list of 'dir/' and 'file' entries below *root*, via os.walk."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        reldir = os.path.relpath(dirpath, root)
        for d in dirnames:
            out.append(os.path.normpath(os.path.join(reldir, d)) + "/")
        for f in sorted(filenames):
            out.append(os.path.normpath(os.path.join(reldir, f)))
    return sorted(out)


def touch(path, text=""):
    """Create *path* containing *text*."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


with tempfile.TemporaryDirectory() as tmp:
    print("listdir empty", os.listdir(tmp))
    os.makedirs(os.path.join(tmp, "a", "b", "c"))
    print("makedirs", tree(tmp))
    os.makedirs(os.path.join(tmp, "a", "b"), exist_ok=True)
    try:
        os.makedirs(os.path.join(tmp, "a", "b"))
    except FileExistsError as e:
        print("makedirs exists", type(e).__name__, errno.errorcode[e.errno])
    os.mkdir(os.path.join(tmp, "d"))
    try:
        os.mkdir(os.path.join(tmp, "q", "r"))
    except FileNotFoundError as e:
        print("mkdir missing parent", type(e).__name__, errno.errorcode[e.errno])

    touch(os.path.join(tmp, "top.txt"), "top")
    touch(os.path.join(tmp, "a", "x.py"), "x = 1\n")
    touch(os.path.join(tmp, "a", "b", "y.py"), "y = 2\n")
    touch(os.path.join(tmp, "a", "b", "c", "z.dat"), "zzz")
    print("listdir", sorted(os.listdir(tmp)))
    print("listdir a", sorted(os.listdir(os.path.join(tmp, "a"))))
    print("walk", tree(tmp))

    walked = []
    for dirpath, dirnames, filenames in os.walk(tmp, topdown=False):
        walked.append((os.path.relpath(dirpath, tmp), sorted(dirnames), sorted(filenames)))
    print("walk bottom-up sorted", sorted(walked))

    pruned = []
    for dirpath, dirnames, filenames in os.walk(tmp):
        dirnames[:] = sorted(d for d in dirnames if d != "b")
        pruned.append(os.path.relpath(dirpath, tmp))
    print("walk pruned", sorted(pruned))

    entries = sorted((e.name, e.is_dir(), e.is_file()) for e in os.scandir(tmp))
    print("scandir", entries)

    p = os.path.join(tmp, "top.txt")
    print("exists", os.path.exists(p), os.path.isfile(p), os.path.isdir(p))
    print("getsize", os.path.getsize(p), os.stat(p).st_size)

    os.rename(p, os.path.join(tmp, "d", "moved.txt"))
    print("after rename", tree(tmp))
    try:
        os.rename(os.path.join(tmp, "nope"), os.path.join(tmp, "nope2"))
    except FileNotFoundError as e:
        print("rename missing", type(e).__name__, errno.errorcode[e.errno])
    os.replace(os.path.join(tmp, "a", "x.py"), os.path.join(tmp, "d", "moved.txt"))
    with open(os.path.join(tmp, "d", "moved.txt"), encoding="utf-8") as fh:
        print("replace content", repr(fh.read()))

    os.remove(os.path.join(tmp, "d", "moved.txt"))
    print("after remove", tree(tmp))
    try:
        os.remove(os.path.join(tmp, "d", "moved.txt"))
    except FileNotFoundError as e:
        print("remove missing", type(e).__name__, errno.errorcode[e.errno])
    try:
        os.rmdir(os.path.join(tmp, "a"))
    except OSError as e:
        print("rmdir non-empty", type(e).__name__, errno.errorcode[e.errno])
    os.rmdir(os.path.join(tmp, "d"))
    os.remove(os.path.join(tmp, "a", "b", "c", "z.dat"))
    os.rmdir(os.path.join(tmp, "a", "b", "c"))
    print("after rmdir", tree(tmp))

    # low level fd I/O
    fd = os.open(os.path.join(tmp, "raw.bin"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    print("os.write", os.write(fd, b"abcdef"))
    os.close(fd)
    fd = os.open(os.path.join(tmp, "raw.bin"), os.O_RDONLY)
    print("os.read", os.read(fd, 3), os.lseek(fd, 0, os.SEEK_CUR), os.read(fd, 100), os.read(fd, 10))
    os.close(fd)
    try:
        os.close(fd)
    except OSError as e:
        print("double close", type(e).__name__, errno.errorcode[e.errno])
    print("final", tree(tmp))
