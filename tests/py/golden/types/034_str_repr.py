"""L0 golden: str repr escaping rules.

repr chooses single quotes unless the string contains ' and not ", escapes
backslash, \\t \\n \\r, other C0/C1 controls as \\xNN, non-printable BMP
characters as \\uNNNN and astral ones as \\UNNNNNNNN, and keeps printable
non-ASCII characters (é, €, 😀) as is.  Lone surrogates are escaped.

Usage: python3.15 -I -X utf8 tests/py/golden/types/034_str_repr.py
"""

CASES = [
    "", "abc", "it's", 'say "hi"', "both ' and \"", "back\\slash", "tab\tnl\ncr\r", "\x00\x01\x1f",
    "\x7f", "\x80", "\x9f", "\xa0", "\xad", "é", "€", "😀", "\u200b", "\u200e", "\u2028", "\u2029",
    "\ufeff", "\ufffd", "\uffff", "\ue000", "\U000e0001", "\U0010ffff", "\U0001f600", "\ud800",
    "\udfff", "\ud83d\ude00", "a\ud800b", "\x0b\x0c", "\x1b[0m", "\u0300", "e\u0301", "\u3000",
    "日本語", "\u0378", "'", '"', "'\"", "\\", "\\n", "\u00a0x",
]

for s in CASES:
    print(repr(s), ascii(s), len(s))

print("repr of repr", repr(repr("a'b")))
print("nested in list", ["it's", "x\ny", "é"])
print("nested in tuple", ("\t",))
print("nested in dict", {"k'": "v\""})
print("str vs repr", str("a\nb") == "a\nb", repr("a\nb") == "'a\\nb'")
print("%r", "%r" % "x'y")
print("!r", f"{'tab\there'!r}")
print("format !r with width", f"{'ab'!r:>8}")
print("!a", f"{'é😀'!a}")
print("repr type", type(repr("x")).__name__)
print("repr len escapes", len(repr("\x00")), len(repr("\ud800")), len(repr("\U000e0001")))
print("isprintable vs repr", [c.isprintable() for c in ("\x7f", "\xa0", "\u200b", "é")])
long = "x" * 5 + "\n" + "y" * 5
print("long", repr(long))
print("all controls", repr("".join(chr(i) for i in range(32))))
print("C1 controls", repr("".join(chr(i) for i in range(0x7f, 0xa1))))
print("printable latin1", repr("".join(chr(i) for i in range(0xa1, 0xb0))))
print("str of str subclass", repr(type("S", (str,), {})("x")))
print("eval-free roundtrip check", repr("a'\"b") == "'a\\'\"b'")

# Category spot checks: (code point, repr) per line; covers Zs/Zl/Zp/Cc/Cf/Co/Cn/Mn/So.
CODE_POINTS = [
    0x20, 0x85, 0xa0, 0x1680, 0x2000, 0x200a, 0x200c, 0x200d, 0x202f, 0x205f, 0x2060, 0x2066,
    0x3000, 0xfff9, 0xfffc, 0xd7ff, 0xe000, 0xf8ff, 0xfdd0, 0x10000, 0x1d173, 0xe0100, 0xf0000,
    0x1f1e6, 0x2603, 0x0915, 0x093f, 0x0600, 0x061c, 0x110bd,
]
for cp in CODE_POINTS:
    print("cp", hex(cp), repr(chr(cp)))

print("quote choice", repr("'"), repr('"'), repr("'\""), repr("a'b'c"), repr('a"b"c'))
print("backslash before quote", repr("\\'"), repr('\\"'))
print("escape forms", repr("\a\b\f\v"), repr("\0"), repr("\177"))
print("repr list of quotes", ["'", '"', "'\""])
print("repr bytes vs str", repr("a\xffb"), repr(b"a\xffb"))
print("repr concat", repr("a") + repr("b"))
print("str() of escapes len", len(str("\t")), len(repr("\t")))
