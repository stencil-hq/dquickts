# Bytes literals and how repr() chooses quotes for str and bytes.

print("bytes", b"abc", B"ABC", b'single', b"""triple""")
print("bytes escapes", b"\x00\x7f\x80\xff", b"\n\t\r\\", b"\101\0")
print("bytes raw", rb"\x00", br"\n", len(rb"\x00"))
print("bytes concat", b"ab" b"cd", b"x" + b"y", b"-" * 3)
print("bytes printable repr", repr(b" ~"), repr(b"\x1f"), repr(b"\x7f"), repr(b"\x20"))
print("bytes index", b"A"[0], list(b"hi"), bytes([104, 105]), bytes(3), bytes(range(65, 70)))
print("bytes str mix", b"abc" == "abc", str(b"abc"), b"abc".decode(), "é".encode())
print("bytearray", bytearray(b"xy"), repr(bytearray()), bytearray("é", "utf-8"))
print("bytes hex", b"\xde\xad".hex(), bytes.fromhex("de ad be ef"), b"abc".hex(":"))

try:
    b"a" + "b"
except TypeError as e:
    print("bytes + str", type(e).__name__, e)

try:
    bytes([256])
except ValueError as e:
    print("bytes 256", type(e).__name__, e)

try:
    bytes("abc")
except TypeError as e:
    print("bytes from str no encoding", type(e).__name__, e)

try:
    bytes.fromhex("zz")
except ValueError as e:
    print("bad hex", type(e).__name__, e)

# repr quote selection for str
cases = [
    "plain",
    "it's",
    'say "hi"',
    "both ' and \"",
    "",
    "'",
    '"',
    "'\"",
    "back\\slash",
    "new\nline",
    "tab\t",
    "nul\0",
    "bell\a",
    "del\x7f",
    "latin é ñ",
    "greek αβγ",
    "emoji \U0001F600",
    "zero width \u200b",
    "line sep \u2028",
    "nbsp \xa0",
    "soft hyphen \xad",
    "private \ue000",
    "combining e\u0301",
    "replacement \ufffd",
]
for s in cases:
    print("repr", repr(s), "ascii", ascii(s))

# repr quote selection for bytes
bcases = [b"plain", b"it's", b'say "hi"', b"both ' and \"", b"", b"'", b'"', b"\\", b"\x00\xff"]
for b in bcases:
    print("bytes repr", repr(b))

# repr of str inside containers uses repr
print("containers", ["it's", 'q"'], ("a",), {"k'": "v\""}, {"x\n"})

# eval(repr(s)) round-trips
ok = True
for s in cases:
    if eval(repr(s)) != s:
        ok = False
        print("round-trip failed", ascii(s))
for b in bcases:
    if eval(repr(b)) != b:
        ok = False
        print("bytes round-trip failed", b)
print("round-trip all", ok)

# str subclass repr
class S(str):
    pass

print("subclass repr", repr(S("it's")), str(S("x")), type(str(S("x"))).__name__)

class R(str):
    def __repr__(self):
        return "R<" + str.__repr__(self) + ">"

print("custom repr", repr(R("v")), [R("w")], "%r" % R("z"), f"{R('q')!r}")

# str() of bytes with -b flag absent just returns repr
print("str of bytes", str(b"q'"), str(bytearray(b"\x01")))

# bytes methods returning bytes keep the type
print("bytes methods", b"Hello".upper(), b"a,b".split(b","), b"  x ".strip(), b"ab".replace(b"a", b"z"))
print("bytes find", b"hello".find(b"l"), b"hello".count(b"l"), b"hello".startswith(b"he"))
print("bytes slices", b"hello"[1:3], b"hello"[::-1], b"hello"[-1:])
