"""L0 golden: str.splitlines line boundaries and keepends.

str.splitlines treats \\n, \\r, \\r\\n, \\v, \\f, \\x1c, \\x1d, \\x1e, \\x85,
\\u2028 and \\u2029 as boundaries; bytes.splitlines only \\n, \\r, \\r\\n.

Usage: python3.15 -I -X utf8 tests/py/golden/types/024_str_splitlines.py
"""

TEXTS = [
    "", "a", "a\n", "a\nb", "a\nb\n", "\n", "\n\n", "a\r\nb", "a\rb", "a\r\rb", "a\n\rb",
    "a\r\n\r\nb", "a\x0bb", "a\x0cb", "a\x1cb", "a\x1db", "a\x1eb", "a\x1fb", "a\x85b",
    "a\u2028b", "a\u2029b", "a\u000db\u000a", "trailing\r", "\r\n", "x\ty", "one\ntwo\r\nthree\rfour",
]

for t in TEXTS:
    print("splitlines", repr(t), t.splitlines())
    print("keepends  ", repr(t), t.splitlines(True))

print("keepends kw", "a\nb".splitlines(keepends=True))
print("keepends False kw", "a\nb\n".splitlines(keepends=False))
print("keepends int", "a\nb".splitlines(1))
print("vs split('\\n')", "a\nb\n".split("\n"), "a\nb\n".splitlines())
print("vs split('\\n') empty", "".split("\n"), "".splitlines())
print("crlf split('\\n')", "a\r\nb".split("\n"))
print("join roundtrip", "".join("a\r\nb\rc\n".splitlines(True)) == "a\r\nb\rc\n")
print("len lines", len("1\n2\n3\n".splitlines()))

BYTES = [b"a\nb", b"a\r\nb", b"a\rb", b"a\x0bb", b"a\x0cb", b"a\x1cb", b"a\x85b", b"a\n", b""]
for b in BYTES:
    print("bytes splitlines", repr(b), b.splitlines(), b.splitlines(keepends=True))

print("bytearray splitlines", bytearray(b"x\ny\r\n").splitlines(True))

try:
    "a".splitlines("yes")
except TypeError as e:
    print("bad keepends", type(e).__name__, str(e))

try:
    "a".splitlines(True, False)
except TypeError as e:
    print("too many args", type(e).__name__, str(e))

text = "first\nsecond\r\nthird"
for number, line in enumerate(text.splitlines(), 1):
    print("line", number, repr(line))

print("rstrip each", [ln.rstrip() for ln in "a  \nb\t\n".splitlines()])
print("non-BMP", "😀\n😀\u2028x".splitlines())
print("only separators", "\r\n\n\r".splitlines(), "\r\n\n\r".splitlines(True))
print("count lines with keepends", sum(1 for _ in "a\nb\nc".splitlines(True)))
