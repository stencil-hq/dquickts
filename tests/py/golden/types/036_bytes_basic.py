"""L0 golden: bytes literals, construction, indexing, slicing, repr and comparisons.

Indexing a bytes object yields an int; slicing yields bytes.  repr uses
b'...' with \\xNN escapes for non-printable bytes and the same quote choice
rules as str.

Usage: python3.15 -I -X utf8 tests/py/golden/types/036_bytes_basic.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


b = b"hello"
show("literal", lambda: b)
show("b[0]", lambda: b[0])
show("b[-1]", lambda: b[-1])
show("type(b[0])", lambda: type(b[0]).__name__)
show("b[1:3]", lambda: b[1:3])
show("b[::-1]", lambda: b[::-1])
show("b[1:2]", lambda: b[1:2])
show("b[10:]", lambda: b[10:])
show("b[10]", lambda: b[10])
show("b[0] = 1", lambda: b.__setitem__(0, 1))
show("list(b)", lambda: list(b))
show("len(b)", lambda: len(b))
show("escapes", lambda: b"\x00\x7f\x80\xff\t\n\r\\")
show("quote choice '", lambda: b"it's")
show("quote choice both", lambda: b"'\"")
show("raw bytes", lambda: rb"\n")
show("octal escape", lambda: b"\101\0")
show("hex escape", lambda: b"\x41")
show("all bytes repr", lambda: bytes(range(32, 40)))
show("high bytes repr", lambda: bytes(range(0x7d, 0x83)))

show("bytes()", lambda: bytes())
show("bytes(3)", lambda: bytes(3))
show("bytes(0)", lambda: bytes(0))
show("bytes(-1)", lambda: bytes(-1))
show("bytes([65, 66])", lambda: bytes([65, 66]))
show("bytes(range(5))", lambda: bytes(range(5)))
show("bytes(gen)", lambda: bytes(x * 2 for x in range(4)))
show("bytes([256])", lambda: bytes([256]))
show("bytes([-1])", lambda: bytes([-1]))
show("bytes(['a'])", lambda: bytes(["a"]))
show("bytes('abc')", lambda: bytes("abc"))
show("bytes('é', 'utf-8')", lambda: bytes("é", "utf-8"))
show("bytes('é', 'ascii')", lambda: bytes("é", "ascii"))
show("bytes(b'x', 'utf-8')", lambda: bytes(b"x", "utf-8"))
show("bytes(bytearray)", lambda: bytes(bytearray(b"ab")))
show("bytes(memoryview)", lambda: bytes(memoryview(b"cd")))
show("bytes(1.5)", lambda: bytes(1.5))
show("bytes(None)", lambda: bytes(None))
show("bytes.fromhex", lambda: bytes.fromhex("de ad BE EF"))

show("b'a' + b'b'", lambda: b"a" + b"b")
show("b'a' + bytearray", lambda: b"a" + bytearray(b"b"))
show("b'a' + 'b'", lambda: b"a" + "b")
show("b'a' + 1", lambda: b"a" + 1)
show("b'ab' * 3", lambda: b"ab" * 3)
show("b'ab' * -1", lambda: b"ab" * -1)
show("b'a' == 'a'", lambda: b"a" == "a")
show("b'a' == bytearray(b'a')", lambda: b"a" == bytearray(b"a"))
show("b'a' < b'b'", lambda: b"a" < b"b")
show("b'ab' < b'abc'", lambda: b"ab" < b"abc")
show("b'\\xff' > b'a'", lambda: b"\xff" > b"a")
show("b'a' < 'b'", lambda: b"a" < "b")
show("b'' < b'\\x00'", lambda: b"" < b"\x00")
show("sorted", lambda: sorted([b"b", b"a", b"", b"ab", b"\x00"]))

show("97 in b'abc'", lambda: 97 in b"abc")
show("b'b' in b'abc'", lambda: b"b" in b"abc")
show("b'bc' in b'abc'", lambda: b"bc" in b"abc")
show("b'' in b'abc'", lambda: b"" in b"abc")
show("256 in b'abc'", lambda: 256 in b"abc")
show("'a' in b'abc'", lambda: "a" in b"abc")
show("bytearray in bytes", lambda: bytearray(b"a") in b"abc")
show("hash eq", lambda: hash(b"abc") == hash(b"abc"))
show("dict key", lambda: {b"k": 1}[b"k"])
show("bytes is immutable hashable", lambda: hash(bytearray(b"k")))
show("iteration", lambda: [x for x in b"\x01\x02"])
show("str(b)", lambda: str(b"x\n"))
show("bool(b'')", lambda: bool(b""))
show("bool(b'\\x00')", lambda: bool(b"\x00"))
show("int key on bytes", lambda: b"abc"[True])
show("% formatting", lambda: b"%d-%s-%b" % (5, b"x", b"y"))
show("% with str", lambda: b"%s" % "x")
show("% with %r alias", lambda: b"%r" % b"x")
show("% x", lambda: b"%02x" % 255)
show("max/min", lambda: (max(b"hello"), min(b"hello")))
show("sum", lambda: sum(b"\x01\x02\x03"))
