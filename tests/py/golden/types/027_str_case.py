"""L0 golden: str case conversion methods.

Covers lower, upper, title, capitalize, casefold and swapcase including
Unicode special casing (ß -> SS, ŉ, ǅ titlecase digraphs, Greek final sigma,
ligatures) and the title() word-boundary rule (apostrophes, digits).

Usage: python3.15 -I -X utf8 tests/py/golden/types/027_str_case.py
"""

TEXTS = [
    "", "hello", "HELLO", "Hello World", "hello world", "hELLO wORLD", "they're bill's friends",
    "3rd place", "a1b2c3", "x_y z-w", "straße", "STRASSE", "ǆemal", "ǅ", "ﬁne ﬂow", "ŉ",
    "ΣΊΣΥΦΟΣ", "όσος", "İstanbul", "ÀÉÎÕÜ", "ẞ", "µ", "ÿ", "ǰ", "😀smile", "mixed😀CASE",
    "\u0130", "ΑΣ ΑΣ.", "ﬃ", "123", "  spaced  out  ",
]

for t in TEXTS:
    print("text", repr(t))
    print("  lower", repr(t.lower()))
    print("  upper", repr(t.upper()))
    print("  title", repr(t.title()))
    print("  capitalize", repr(t.capitalize()))
    print("  casefold", repr(t.casefold()))
    print("  swapcase", repr(t.swapcase()))

print("len change upper ß", len("ß"), len("ß".upper()))
print("len change lower İ", len("İ"), len("İ".lower()))
print("casefold eq", "Straße".casefold() == "STRASSE".casefold())
print("lower eq", "Straße".lower() == "STRASSE".lower())
print("swapcase twice", "ß".swapcase().swapcase())
print("upper idempotent", "abc".upper().upper())
print("capitalize rest lower", "hELLO".capitalize())
print("capitalize titlecase digraph", "ǆa".capitalize())
print("title after digit", "1st 2nd".title())
print("title with apostrophe", "don't stop".title())
print("type", type("a".upper()).__name__)
print("upper of ŉ expands", repr("ŉ".upper()), len("ŉ".upper()))
print("lower of ẞ", repr("ẞ".lower()))
print("casefold of ẞ", repr("ẞ".casefold()))
print("upper ligature ﬀ", repr("ﬀ".upper()))
print("title ligature ﬁ", repr("ﬁsh".title()))
print("final sigma word end", repr("ΟΔΟΣ".lower()))
print("final sigma before punct", repr("ΟΔΟΣ.".lower()))
print("sigma alone", repr("Σ".lower()))
print("sigma mid word", repr("ΑΣΑ".lower()))
print("title Greek", repr("όσος ΣΟΦΟΣ".title()))
print("swapcase sigma", repr("ΣΑΣ".swapcase()))
print("lower ASCII unaffected bytes-like", "ABC".lower() == "abc")
print("title non-letters split words", repr("hello-world_foo.bar baz9qux"))
print("title result", repr("hello-world_foo.bar baz9qux".title()))
print("capitalize first non-letter", repr("1abc DEF".capitalize()))
print("capitalize empty", repr("".capitalize()))
print("upper non-BMP Deseret", repr("\U00010428".upper()), repr("\U00010400".lower()))
print("case-insensitive compare", sorted(["b", "A", "C", "a"], key=str.casefold))
for name in ("lower", "upper", "title", "capitalize", "casefold", "swapcase"):
    try:
        getattr("a", name)(1)
    except TypeError as e:
        print("arg error", name, type(e).__name__, str(e))
