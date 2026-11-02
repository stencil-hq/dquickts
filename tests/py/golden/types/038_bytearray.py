"""L0 golden: bytearray construction and in-place mutation.

Covers append/extend/insert/pop/remove/reverse/clear/copy, item and slice
assignment (including size-changing and extended slices), del, +=/*=, repr,
comparisons with bytes, and value range errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/038_bytearray.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("bytearray()", lambda: bytearray())
show("bytearray(3)", lambda: bytearray(3))
show("bytearray(b'abc')", lambda: bytearray(b"abc"))
show("bytearray([1, 2])", lambda: bytearray([1, 2]))
show("bytearray('é', 'utf-8')", lambda: bytearray("é", "utf-8"))
show("bytearray('x')", lambda: bytearray("x"))
show("bytearray([300])", lambda: bytearray([300]))

ba = bytearray(b"hello")
ba.append(33)
show("append", lambda: ba)
ba.extend(b" xy")
show("extend bytes", lambda: ba)
ba.extend([65, 66])
show("extend list", lambda: ba)
ba.extend(range(48, 50))
show("extend range", lambda: ba)
ba.insert(0, 62)
show("insert", lambda: ba)
ba.insert(-1, 95)
show("insert neg", lambda: ba)
ba.insert(100, 46)
show("insert past end", lambda: ba)
show("pop", lambda: ba.pop())
show("pop(0)", lambda: ba.pop(0))
show("after pops", lambda: ba)
ba.remove(ord("l"))
show("remove first l", lambda: ba)
show("remove missing", lambda: ba.remove(200))
show("append 256", lambda: ba.append(256))
show("append -1", lambda: ba.append(-1))
show("append bytes", lambda: ba.append(b"x"))
show("extend str", lambda: ba.extend("x"))
show("extend bad ints", lambda: ba.extend([1, 999]))
show("after failed extend", lambda: ba)
ba[0] = 72
show("setitem", lambda: ba)
show("setitem 256", lambda: ba.__setitem__(0, 256))
show("setitem bytes", lambda: ba.__setitem__(0, b"x"))
ba[1:3] = b"EE"
show("slice same size", lambda: ba)
ba[1:3] = b""
show("slice shrink", lambda: ba)
ba[0:0] = b"[["
show("slice insert", lambda: ba)
ba[-2:] = [93, 93, 93]
show("slice grow list", lambda: ba)
ba[::2] = bytes(len(ba[::2]))
show("extended slice", lambda: ba)
show("extended slice wrong size", lambda: ba.__setitem__(slice(None, None, 2), b"x"))
del ba[0]
show("del item", lambda: ba)
del ba[::3]
show("del extended slice", lambda: ba)
del ba[:2]
show("del slice", lambda: ba)

b2 = bytearray(b"ab")
alias = b2
b2 += b"cd"
show("+= in place", lambda: (b2, alias is b2))
b2 *= 2
show("*= in place", lambda: (b2, alias is b2))
b3 = b2 + b"!"
show("+ new object", lambda: (b3, b3 is b2, type(b3).__name__))
show("bytes + bytearray type", lambda: type(b"x" + bytearray(b"y")).__name__)
show("reverse", lambda: (b2.reverse(), b2))
show("copy", lambda: b2.copy())
show("copy independent", lambda: b2.copy() is not b2)
show("clear", lambda: (b2.clear(), b2))
show("pop empty", lambda: b2.pop())
show("eq bytes", lambda: bytearray(b"x") == b"x")
show("lt bytes", lambda: bytearray(b"a") < b"b")
show("hash", lambda: hash(bytearray(b"x")))
show("repr quotes", lambda: bytearray(b"it's"))
show("repr escapes", lambda: bytearray(b"\x00\xff\n"))
show("str()", lambda: str(bytearray(b"q")))
show("methods return bytearray", lambda: type(bytearray(b" a ").strip()).__name__)
show("split returns bytearrays", lambda: bytearray(b"a b").split())
show("upper", lambda: bytearray(b"abc").upper())
show("fromhex", lambda: bytearray.fromhex("0a0b"))
show("in int", lambda: 98 in bytearray(b"abc"))
show("index int", lambda: bytearray(b"abc").index(99))
show("iterate yields ints", lambda: [x for x in bytearray(b"ab")])
show("len after ops", lambda: len(bytearray(10)))
show("as dict key", lambda: {bytearray(b"k"): 1})
