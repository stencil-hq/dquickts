# io.StringIO follows Modules/_io/stringio.c: newline translation on write,
# character positions, overseek padding, pickling state and the argument
# and error messages of the C methods.
import copy
import io
import pickle

S = io.StringIO
print(S.__mro__)
print(isinstance(S(), io.TextIOWrapper), isinstance(S(), io.TextIOBase))

for nl in ["__default__", None, "", "\n", "\r", "\r\n", "\n\0x"]:
    kw = {} if nl == "__default__" else {"newline": nl}
    for init in [None, "a\rb\r\nc\nd"]:
        s = S(init, **kw) if init is not None else S(**kw)
        s.seek(0, 2)
        s.write("x\ry\r\nz\n\u20ac\r")
        s.write("\nq")
        print(repr(nl), repr(init), repr(s.getvalue()), s.tell(), repr(s.newlines))
        s.seek(0)
        print("  lines", [s.readline() for _ in range(8)], s.tell())
        s.seek(0)
        print("  read", repr(s.read(5)), s.tell(), repr(s.read()), s.tell())
        print("  list", list(S("a\rb\r\nc", **kw)), S("a\rb\r\nc\r", **kw).readlines())
        print("  state", s.__getstate__())

s = S("h\u20acllo\U0001f600 w\xf6rld")
print(s.read(3), s.tell(), s.read(4), s.tell(), s.seek(2), s.read(), s.tell())
s.seek(20)
s.write("Z")
print(repr(s.getvalue()), s.tell())
s = S("\U0001f600\U0001f600")
print(s.read(1), s.tell(), len(s.getvalue()))
s = S()
s.seek(3)
s.write("q")
print(repr(s.getvalue()))
s = S("abcdef")
print(s.truncate(2), s.tell(), s.getvalue(), s.truncate(10), s.getvalue())
s = S("abc")
s.seek(1)
s.truncate()
print(repr(s.getvalue()), s.tell())
s = S()
s.write("abc")
s.seek(1)
print(s.write("Z"), s.getvalue(), s.read())
s = S("abc")
s.seek(10)
print(repr(s.read()), s.tell(), repr(s.getvalue()), repr(s.readline()))
s = S("abc\ndef\n")
print(s.readline(2), s.readline(-1), s.readline(None), repr(s.readline()))
print(S("a\nb\nc").readlines(2), S("a\nb\nc").readlines(3))
s = S(newline=None)
s.write("a\r\n")
print(repr(s.newlines))
s.write("b\r")
print(s.newlines)


class MyStr(str):
    def __str__(self):
        return "nope"


s = S()
s.write(MyStr("hey"))
print(type(s.getvalue()), s.getvalue())


class I:
    def __index__(self):
        return 1


s = S("abc")
print(s.seek(I()), s.read(I()), s.truncate(I()))

calls = [("read", (1, 2)), ("read", ("x",)), ("readline", (1, 2)), ("readline", (1.5,)),
         ("write", ()), ("write", (1, 2)), ("write", (b"x",)), ("seek", ()), ("seek", (1, 2, 3)),
         ("seek", (1.5,)), ("seek", (0, 1.5)), ("seek", (-1,)), ("seek", (1, 1)), ("seek", (0, 3)),
         ("seek", (2**70,)), ("seek", (0, 2**40)), ("read", (2**70,)), ("truncate", (1, 2)),
         ("truncate", ("x",)), ("truncate", (-1,)), ("truncate", (2**70,)), ("tell", (1,)),
         ("getvalue", (1,)), ("close", (1,)), ("readable", (1,)), ("__getstate__", (1,)),
         ("__setstate__", ()), ("__setstate__", (1, 2))]
for m, a in calls:
    s = S("abc")
    try:
        print(m, a, "->", getattr(s, m)(*a))
    except Exception as e:
        print(m, a, type(e).__name__, e)
for m, kw in [("read", {"size": 1}), ("write", {"s": "x"}), ("seek", {"whence": 0}),
              ("tell", {"x": 1})]:
    try:
        getattr(S(), m)(**kw)
    except TypeError as e:
        print(e)
for args, kw in [((), {"x": 1}), ((), {"initial_value": "a", "x": 1}),
                 ((), {"newline": None, "x": 1, "y": 2}), (("a", "\n", 3), {}),
                 (("a",), {"initial_value": "b"}), ((), {"newline": "x"}),
                 ((), {"newline": 5}), ((5,), {}), ((b"a",), {}), ((), {"newline": "\n\n"})]:
    try:
        S(*args, **kw)
        print(args, kw, "ok")
    except Exception as e:
        print(args, kw, type(e).__name__, e)

s = S("abc")
s.close()
for name in ["flush", "readlines", "__iter__", "__enter__", "isatty", "fileno", "detach",
             "closed", "line_buffering", "newlines", "encoding", "errors", "__getstate__",
             "readable", "truncate", "readline", "__next__", "read", "getvalue", "tell"]:
    try:
        a = getattr(s, name)
        r = a() if callable(a) else a
        print(name, "ok", r if not isinstance(r, S) else "self")
    except Exception as e:
        print(name, type(e).__name__, e)
try:
    s.write("x")
except ValueError as e:
    print("write", e)
s.__init__("re")
print(s.closed, s.read())

u = S.__new__(S)
for name in ["read", "write", "closed", "getvalue", "__getstate__", "newlines",
             "line_buffering", "close"]:
    try:
        a = getattr(u, name)
        print(name, "ok", a("x") if name == "write" else (a() if callable(a) else a))
    except Exception as e:
        print("u", name, type(e).__name__, e)
u = S.__new__(S)
print(u.__setstate__(("ab", None, 1, None)), u.read())


class T(S):
    def readline(self, *a):
        return b"x"


try:
    next(T("a"))
except OSError as e:
    print(e)


class T2(S):
    def __init__(self):
        pass


try:
    T2().read()
except ValueError as e:
    print(e)

s = S("hello", newline="\r")
s.seek(2)
s.x = 1
print(s.__getstate__())
s2 = pickle.loads(pickle.dumps(s))
print(s2.getvalue(), s2.tell(), s2.x, s2.newlines)
s3 = copy.copy(S("a\r\nb", newline=None))
print(repr(s3.getvalue()), s3.newlines)
s = S("x")
print(s.__getstate__())
s.__dict__
print(s.__getstate__())
for bad in [("a",), ["a", None, 0, None], ("a", 5, 0, None), (5, None, 0, None),
            ("a", None, "x", None), ("a", None, -1, None), ("a", None, 2**70, None),
            ("a", None, 0, 5), ("a", "x", 0, None)]:
    try:
        S().__setstate__(bad)
        print("ok")
    except Exception as e:
        print(type(e).__name__, e)
s = S()
s.close()
try:
    s.__setstate__(("a", None, 0, None))
except ValueError as e:
    print(e)
with S("w") as s:
    pass
print(s.closed)
print(S().write(""), S("x").seek(0, 2))
