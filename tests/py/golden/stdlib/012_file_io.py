"""L0 golden: built-in open() modes, text/binary I/O, encodings, newlines, seek/tell.

All files live in a temporary directory whose path is never printed.
"""
import errno
import os
import tempfile

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "t.txt")

    with open(path, "w", encoding="utf-8") as f:
        print("write returns", f.write("line one\n"), f.write("line two\nline three"))
        print("mode/closed", f.mode, f.closed, f.writable(), f.readable())
    print("closed after with", f.closed)
    try:
        f.write("x")
    except ValueError as e:
        print("write closed", type(e).__name__, e)

    with open(path, encoding="utf-8") as f:
        print("read all", repr(f.read()))
        print("read at eof", repr(f.read()))
    with open(path, encoding="utf-8") as f:
        print("readline", repr(f.readline()), repr(f.readline()))
        print("readline rest", repr(f.readline()), repr(f.readline()))
    with open(path, encoding="utf-8") as f:
        print("readlines", f.readlines())
    with open(path, encoding="utf-8") as f:
        print("iterate", [line.rstrip("\n") for line in f])
    with open(path, encoding="utf-8") as f:
        print("read(4)", repr(f.read(4)), repr(f.readline(3)))

    with open(path, "a", encoding="utf-8") as f:
        f.write("\nappended")
    with open(path, encoding="utf-8") as f:
        print("after append", f.read().splitlines())

    with open(path, "r+", encoding="utf-8") as f:
        f.write("LINE")
    with open(path, encoding="utf-8") as f:
        print("r+ overwrite", repr(f.readline()))

    try:
        open(path, "x")
    except FileExistsError as e:
        print("x existing", type(e).__name__, errno.errorcode[e.errno])
    with open(os.path.join(tmp, "new.txt"), "x", encoding="utf-8") as f:
        f.write("fresh")
    print("x new ok", os.path.getsize(os.path.join(tmp, "new.txt")))
    try:
        open(os.path.join(tmp, "missing.txt"))
    except FileNotFoundError as e:
        print("missing", type(e).__name__, errno.errorcode[e.errno])
    for bad_mode in ["rw", "z", "rbt", "U"]:
        try:
            open(path, bad_mode)
        except ValueError as e:
            print("bad mode", repr(bad_mode), type(e).__name__, e)

    # encodings and errors=
    epath = os.path.join(tmp, "enc.txt")
    with open(epath, "w", encoding="utf-8") as f:
        f.write("naïve café €")
    with open(epath, "rb") as f:
        raw = f.read()
    print("utf-8 bytes", raw, len(raw))
    with open(epath, encoding="latin-1") as f:
        print("as latin-1", repr(f.read()))
    try:
        with open(epath, encoding="ascii") as f:
            f.read()
    except UnicodeDecodeError as e:
        print("ascii strict", type(e).__name__, e.reason, e.start, e.end)
    with open(epath, encoding="ascii", errors="replace") as f:
        print("errors=replace", repr(f.read()))
    with open(epath, encoding="ascii", errors="ignore") as f:
        print("errors=ignore", repr(f.read()))
    with open(epath, encoding="ascii", errors="surrogateescape") as f:
        print("errors=surrogateescape", ascii(f.read()))
    with open(epath, "w", encoding="ascii", errors="xmlcharrefreplace") as f:
        f.write("ü€")
    with open(epath, "rb") as f:
        print("xmlcharrefreplace", f.read())
    with open(epath, "w", encoding="utf-16-be") as f:
        f.write("hi€")
    with open(epath, "rb") as f:
        print("utf-16-be", f.read())

    # newline translation
    npath = os.path.join(tmp, "nl.txt")
    with open(npath, "w", encoding="utf-8", newline="") as f:
        f.write("a\nb\r\nc\rd")
    with open(npath, encoding="utf-8") as f:
        print("universal", repr(f.read()), repr(f.newlines))
    with open(npath, encoding="utf-8", newline="") as f:
        print("newline=''", f.readlines())
    with open(npath, encoding="utf-8", newline="\r\n") as f:
        print("newline=crlf", f.readlines())
    with open(npath, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("x\ny\n")
    with open(npath, "rb") as f:
        print("write crlf", f.read())

    # binary seek/tell
    bpath = os.path.join(tmp, "b.bin")
    with open(bpath, "wb") as f:
        f.write(bytes(range(16)))
        print("tell after write", f.tell())
    with open(bpath, "rb") as f:
        print("read 4", f.read(4), f.tell())
        print("seek set", f.seek(10), f.read(2))
        print("seek cur", f.seek(-4, os.SEEK_CUR), f.read(1))
        print("seek end", f.seek(-2, os.SEEK_END), f.read())
        print("read eof", f.read(), f.tell())
        f.seek(0)
        buf = bytearray(5)
        print("readinto", f.readinto(buf), buf)
        print("read1", f.read1(3))
        print("mode", f.mode, type(f).__name__)
    with open(bpath, "r+b") as f:
        f.seek(2)
        f.write(b"\xff\xfe")
        f.truncate(6)
    with open(bpath, "rb") as f:
        print("r+b truncate", f.read())
    with open(bpath, "rb", buffering=0) as f:
        print("unbuffered", type(f).__name__, f.read(3))
    with open(path, encoding="utf-8") as f:
        try:
            f.seek(3, os.SEEK_CUR)
        except OSError as e:
            print("text relative seek", type(e).__name__, e)
