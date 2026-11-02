"""L0 golden: str predicate methods over ASCII and Unicode samples.

Prints a row per sample for isdigit, isdecimal, isnumeric, isalpha, isalnum,
isspace, isupper, islower, istitle, isidentifier, isprintable and isascii.

Usage: python3.15 -I -X utf8 tests/py/golden/types/028_str_predicates.py
"""

PREDICATES = ["isdigit", "isdecimal", "isnumeric", "isalpha", "isalnum", "isspace", "isupper",
              "islower", "istitle", "isidentifier", "isprintable", "isascii"]

SAMPLES = [
    "", "abc", "ABC", "Abc", "aBc", "Abc Def", "Abc def", "123", "12.5", "-1", "abc123", "abc 123",
    " ", "\t\n", "\x0b\x0c\x1c", "\u00a0", "\u3000", "\u200b", "\u2028", "\x85", "\x00", "\x7f",
    "\u00b2", "\u00bd", "\u0663", "\uff11", "\u2167", "\u4e00", "\u00e9", "\u00c9t\u00e9", "ß",
    "ǅ", "ǅa", "_x", "x1", "1x", "class", "None", "a-b", "π", "℘", "😀", "a😀", "\ud800",
    "Hello World!", "HELLO 123", "hello 123", "ǅungla", "\u0300", "a\u0300", "\u2160", "\u1d2c",
    "ª", "\u0f33", "\U0001d7ce", "\U00010107",
]

for s in SAMPLES:
    flags = "".join("1" if getattr(s, p)() else "0" for p in PREDICATES)
    print(ascii(s), flags)

print("columns", " ".join(PREDICATES))
print("isidentifier keywords", [w.isidentifier() for w in ("if", "match", "_", "__init__", "0a")])
print("isdigit vs isdecimal", "²".isdigit(), "²".isdecimal())
print("isnumeric fraction", "½".isnumeric(), "½".isdigit())
print("isprintable space", " ".isprintable(), "\n".isprintable())
print("isupper no cased", "123".isupper(), "A1".isupper())
print("istitle edge", "A".istitle(), "Aa Bb".istitle(), "Aa bB".istitle(), "1A".istitle())
print("isascii empty", "".isascii(), "\x7f".isascii(), "\x80".isascii())
print("isdecimal non-BMP digit", "\U0001d7ce".isdecimal(), "\U0001d7ce".isdigit())
print("isnumeric CJK", "\u4e00".isnumeric(), "\u4e94".isnumeric(), "\u5341".isnumeric())
print("isalpha combining", "e\u0301".isalpha(), "\u0301".isalpha())
print("isalnum mixed scripts", "abcαβγ123".isalnum(), "abc def".isalnum())
print("isspace each", [chr(c).isspace() for c in (9, 10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0)])
print("isspace not", [chr(c).isspace() for c in (0x200b, 0x180e, 0xfeff, 0)])
print("isidentifier unicode", ["é".isidentifier(), "Σx".isidentifier(), "a·b".isidentifier()])
print("isidentifier other", ["a-b".isidentifier(), "a b".isidentifier(), "".isidentifier()])
print("isidentifier digits", ["x\u0661".isidentifier(), "\u0661x".isidentifier()])
print("isprintable each", [chr(c).isprintable() for c in (0x20, 0x7e, 0x7f, 0xa0, 0xad, 0x2028)])
print("isprintable empty", "".isprintable())
print("istitle digraph", "ǅ".istitle(), "ǅx Ǉy".istitle())
print("isupper/islower digraph", "ǅ".isupper(), "ǅ".islower())
print("islower with digits", "abc123".islower(), "123abc".islower(), "ß".islower())
print("isupper Roman numeral", "\u2167".isupper(), "\u2177".islower())
try:
    "a".isdigit(1)
except TypeError as e:
    print("arg error", type(e).__name__, str(e))
