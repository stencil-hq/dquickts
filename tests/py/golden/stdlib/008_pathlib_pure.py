"""L0 golden: pathlib.PurePosixPath operations (no file system access)."""
import os
from pathlib import PurePosixPath as P
from pathlib import PureWindowsPath as W

p = P("/usr/local/lib/python3.15/site.tar.gz")
print("repr", repr(p))
print("str", str(p))
print("name", p.name)
print("stem", p.stem)
print("suffix", p.suffix)
print("suffixes", p.suffixes)
print("parent", p.parent)
print("parents", [str(x) for x in p.parents])
print("parents[1]", p.parents[1], "parents[-1]", p.parents[-1])
print("parts", p.parts)
print("root/anchor", repr(p.root), repr(p.anchor), repr(p.drive))
print("is_absolute", p.is_absolute(), P("a/b").is_absolute())

# joining
print("slash", P("/a") / "b" / "c.txt")
print("slash abs", P("/a") / "/etc" / "x")
print("rslash", "rel" / P("x"))
print("joinpath", P("a").joinpath("b", "c", "d.py"))
print("joinpath path", P("a").joinpath(P("b/c")))
print("normalize", P("a//b/./c/"), P("./a"), P("."), P(""))
print("dotdot kept", P("a/../b"))
print("double slash root", P("//x/y"), P("///x"))

# edge names
for s in ["", ".", "/", "a.", ".bashrc", "a.b.c", "/x/y/", "archive.tar.gz", "noext", "..", "a/.."]:
    q = P(s)
    print("edge", repr(s), repr(q.name), repr(q.stem), repr(q.suffix), q.suffixes, str(q.parent))

# with_*
f = P("dir/report.draft.txt")
print("with_suffix", f.with_suffix(".md"), f.with_suffix(""))
print("with_name", f.with_name("other.csv"))
print("with_stem", f.with_stem("final"))
for bad in [lambda: f.with_suffix("md"), lambda: f.with_name(""), lambda: P("/").with_name("x"), lambda: f.with_name("a/b")]:
    try:
        bad()
    except ValueError as e:
        print("with error", type(e).__name__, e)

# relative_to / is_relative_to
base = P("/home/user")
target = P("/home/user/src/app/main.py")
print("relative_to", target.relative_to(base))
print("relative_to self", target.relative_to(target))
print("is_relative_to", target.is_relative_to(base), target.is_relative_to("/etc"))
print("prefix not component", P("/home/username").is_relative_to("/home/user"))
try:
    target.relative_to("/etc")
except ValueError as e:
    print("relative_to error", type(e).__name__, e)
print("walk_up", P("/a/b/c").relative_to("/a/x/y", walk_up=True))

# match / full_match
print("match name", P("a/b/c.py").match("*.py"), P("a/b/c.py").match("b/*.py"), P("a/b/c.py").match("a/*.py"))
print("match abs", P("/a/b.py").match("/*.py"), P("/b.py").match("/*.py"))
print("full_match", P("a/b/c.py").full_match("**/*.py"), P("a/b/c.py").full_match("*.py"))
print("full_match exact", P("a/b/c.py").full_match("a/*/c.py"), P("x/c.py").full_match("a/**"))

# comparison / hashing-neutral equality
print("eq", P("a/b") == P("a//b/"), P("a") == P("A"), P("a") < P("b"))
print("sorted", [str(x) for x in sorted([P("b"), P("a/z"), P("a")])])
print("as_posix", P("x/y").as_posix())
print("fspath", repr(os.fspath(P("a/b"))))
print("bytes", bytes(P("/é")))

# Windows flavour, pure
w = W("C:\\Users\\Me\\file.TXT")
print("win parts", w.parts, w.drive, w.root, w.name, w.suffix)
print("win posix", w.as_posix())
print("win eq case", W("c:/users") == W("C:\\USERS"))
print("win join", W("C:/a") / "D:b", W("C:/a") / "/x")
print("win unc", W("//server/share/f.txt").anchor)
print("win relative", w.relative_to("C:/users"))
