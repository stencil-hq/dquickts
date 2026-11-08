"""L0 golden: os.path pure string functions with fixed POSIX paths."""
import os
import posixpath

print("sep", repr(os.sep), repr(os.extsep), repr(os.curdir), repr(os.pardir), repr(os.altsep))
print("posixpath is os.path", os.path is posixpath)

J = posixpath.join
print("join", J("a", "b", "c.txt"))
print("join abs resets", J("/a", "b", "/c", "d"))
print("join trailing", J("a/", "b"), J("a", ""), J("", "b"))
print("join empty", repr(J("")))
print("join bytes", J(b"a", b"b"))
try:
    J("a", b"b")
except TypeError as e:
    print("join mixed", type(e).__name__, e)

for p in ["/usr/lib/libc.so.6", "a/b/", "file", "/", "", ".hidden", "dir/.hidden.txt", "//x", "a.b/c"]:
    print(
        "split", repr(p), posixpath.split(p),
        "ext", posixpath.splitext(p),
        "base", repr(posixpath.basename(p)),
        "dir", repr(posixpath.dirname(p)),
    )

print("splitdrive", posixpath.splitdrive("/x/y"))
print("splitroot", posixpath.splitroot("//a/b"), posixpath.splitroot("///a"), posixpath.splitroot("a/b"))

for p in ["a//b", "a/./b", "a/b/../c", "/../x", "../../a", "a/..", "", ".", "//a//b/", "///a", "/a/b/../../.."]:
    print("normpath", repr(p), "->", repr(posixpath.normpath(p)))

print("relpath", posixpath.relpath("/a/b/c", start="/a"))
print("relpath up", posixpath.relpath("/a/x", start="/a/b/c"))
print("relpath same", posixpath.relpath("/a", start="/a"))
print("relpath root", posixpath.relpath("/", start="/a/b"))
print("relpath rel", posixpath.relpath("x/y", start="x/z"))
try:
    posixpath.relpath("")
except ValueError as e:
    print("relpath empty", type(e).__name__, e)

print("isabs", posixpath.isabs("/a"), posixpath.isabs("a"), posixpath.isabs(""))
print("commonpath", posixpath.commonpath(["/usr/lib", "/usr/local/lib", "/usr/lib2"]))
print("commonprefix", posixpath.commonprefix(["/usr/lib", "/usr/local/lib", "/usr/lib2"]))
try:
    posixpath.commonpath(["/a", "b"])
except ValueError as e:
    print("commonpath mix", type(e).__name__, e)
try:
    posixpath.commonpath([])
except ValueError as e:
    print("commonpath empty", type(e).__name__, e)

print("normcase", posixpath.normcase("A/B"))
print("expanduser no tilde", posixpath.expanduser("a/~b"))
print("splitext dots", posixpath.splitext("a.tar.gz"), posixpath.splitext("..."), posixpath.splitext(".a.b"))
print("fspath", os.fspath("x"), os.fspath(b"y"))
try:
    os.fspath(3)
except TypeError as e:
    print("fspath bad", type(e).__name__, e)
print("fsencode", os.fsencode("é"), os.fsdecode(b"\xc3\xa9"))
print("join bytes normpath", posixpath.normpath(b"/a/./b/.."))
