"""L0 golden: str operators, comparisons, ord/chr and construction.

Covers repetition (including zero/negative counts), concatenation errors,
code point ordering comparisons, ord/chr across the whole code space
(including non-BMP and the ValueError/TypeError paths), and str() of
basic objects.

Usage: python3.15 -I -X utf8 tests/py/golden/types/032_str_ops_ordchr.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("'ab' * 3", lambda: "ab" * 3)
show("3 * 'ab'", lambda: 3 * "ab")
show("'ab' * 0", lambda: "ab" * 0)
show("'ab' * -2", lambda: "ab" * -2)
show("'' * 100", lambda: "" * 100)
show("'ab' * True", lambda: "ab" * True)
show("'ab' * 2.0", lambda: "ab" * 2.0)
show("'ab' * 'c'", lambda: "ab" * "c")
show("'ab' * 2**62", lambda: "ab" * 2 ** 62)
show("'😀' * 3", lambda: "😀" * 3)
show("'a' + 'b'", lambda: "a" + "b")
show("'a' + 1", lambda: "a" + 1)
show("1 + 'a'", lambda: 1 + "a")
show("'a' + None", lambda: "a" + None)
show("'a' + b'b'", lambda: "a" + b"b")
show("'a' - 'b'", lambda: "a" - "b")
s = "x"
s += "y"
s *= 2
show("augmented", lambda: s)

show("'a' < 'b'", lambda: "a" < "b")
show("'Z' < 'a'", lambda: "Z" < "a")
show("'abc' < 'abd'", lambda: "abc" < "abd")
show("'ab' < 'abc'", lambda: "ab" < "abc")
show("'' < 'a'", lambda: "" < "a")
show("'b' > 'abc'", lambda: "b" > "abc")
show("'é' > 'z'", lambda: "é" > "z")
show("'\\uffff' < '😀'", lambda: "\uffff" < "😀")
show("'\\ue000' < '😀'", lambda: "\ue000" < "😀")
show("'a' == 'a'", lambda: "a" == "a")
show("'a' != 'A'", lambda: "a" != "A")
show("'a' < 1", lambda: "a" < 1)
show("'a' == 1", lambda: "a" == 1)
show("'1' == 1", lambda: "1" == 1)
show("sorted strs", lambda: sorted(["b", "A", "a", "B", "é", "10", "9", "", "😀", "\uffff"]))
show("max str", lambda: max("hello"))
show("min str", lambda: min(["pear", "apple", "fig"]))
show("max key len", lambda: max(["pear", "apple", "fig"], key=len))

for c in ("a", "A", "0", " ", "\n", "\x00", "\x7f", "é", "€", "\uffff", "😀", "\U0010ffff", "\ud800"):
    print("ord", ascii(c), ord(c))
for n in (0, 9, 10, 32, 65, 127, 128, 255, 256, 0x20ac, 0xd7ff, 0xe000, 0xfffd, 0x1f600, 0x10ffff):
    print("chr", n, ascii(chr(n)))
show("chr(0xd800)", lambda: chr(0xD800))
show("chr(0x110000)", lambda: chr(0x110000))
show("chr(-1)", lambda: chr(-1))
show("chr(2**63)", lambda: chr(2 ** 63))
show("chr(65.0)", lambda: chr(65.0))
show("chr('a')", lambda: chr("a"))
show("chr(True)", lambda: chr(True))
show("ord('')", lambda: ord(""))
show("ord('ab')", lambda: ord("ab"))
show("ord(b'a')", lambda: ord(b"a"))
show("ord(bytearray(b'z'))", lambda: ord(bytearray(b"z")))
show("ord(b'ab')", lambda: ord(b"ab"))
show("ord(65)", lambda: ord(65))
show("ord('\\ud83d\\ude00')", lambda: ord("\ud83d\ude00"))
show("chr/ord roundtrip", lambda: all(ord(chr(i)) == i for i in range(0, 0x110000, 4099)))

show("str()", lambda: str())
show("str(42)", lambda: str(42))
show("str(-0.0)", lambda: str(-0.0))
show("str(None)", lambda: str(None))
show("str('x')", lambda: str("x"))
show("str(b'x')", lambda: str(b"x"))
show("str([1, 'a'])", lambda: str([1, "a"]))
show("str(object=5)", lambda: str(object=5))
show("type(str(1))", lambda: type(str(1)).__name__)
show("'abc'[1]", lambda: "abc"[1])
show("'abc'[-1]", lambda: "abc"[-1])
show("'abc'[3]", lambda: "abc"[3])
show("'abc'[::-1]", lambda: "abc"[::-1])
show("'abcdef'[1:5:2]", lambda: "abcdef"[1:5:2])
show("'abc'[10:]", lambda: "abc"[10:])
show("str item assign", lambda: "abc".__setitem__(0, "x"))
