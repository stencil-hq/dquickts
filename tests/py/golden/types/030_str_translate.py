"""L0 golden: str.maketrans and str.translate.

Covers the dict form (int/str keys, str/int/None values), the two- and
three-argument forms, multi-character replacements, LookupError-based
passthrough via a custom mapping, and the related errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/030_str_translate.py
"""


class Rot13:
    """Mapping that rotates ASCII letters and leaves other characters untouched."""

    def __getitem__(self, code: int) -> int:
        if 97 <= code <= 122:
            return (code - 97 + 13) % 26 + 97
        if 65 <= code <= 90:
            return (code - 65 + 13) % 26 + 65
        raise LookupError(code)


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


def sorted_table(table: dict) -> list:
    """Return the maketrans table as a list of items sorted by code point."""
    return sorted(table.items())


show("maketrans 2-arg", lambda: sorted_table(str.maketrans("abc", "xyz")))
show("maketrans 3-arg", lambda: sorted_table(str.maketrans("ab", "AB", "cd")))
show("maketrans dict", lambda: sorted_table(str.maketrans({"a": "1", "b": None, 99: "zz", "d": 101})))
show("maketrans empty", lambda: str.maketrans(""))
show("maketrans unequal", lambda: str.maketrans("abc", "xy"))
show("maketrans dict multi-char key", lambda: str.maketrans({"ab": "x"}))
show("maketrans dict bad key", lambda: str.maketrans({1.5: "x"}))
show("maketrans one str", lambda: str.maketrans("abc"))
show("maketrans non-BMP", lambda: sorted_table(str.maketrans("😀", "x")))

show("translate basic", lambda: "abcabc".translate(str.maketrans("abc", "xyz")))
show("translate delete", lambda: "hello world".translate(str.maketrans("", "", "lo")))
show("translate dict ord", lambda: "abc".translate({ord("a"): "AA", ord("b"): None}))
show("translate int value", lambda: "abc".translate({97: 65}))
show("translate non-BMP value", lambda: "a-b".translate({ord("-"): "😀"}))
show("translate non-BMP key", lambda: "x😀y".translate({0x1F600: "!"}))
show("translate str key ignored", lambda: "abc".translate({"a": "X"}))
show("translate empty table", lambda: "abc".translate({}))
show("translate rot13", lambda: "Hello, World!".translate(Rot13()))
show("translate list table", lambda: "\x00\x01\x02".translate(["a", "b"]))
show("translate bad value", lambda: "abc".translate({97: 1.5}))
show("translate value too big", lambda: "abc".translate({97: 0x110000}))
show("translate negative value", lambda: "abc".translate({97: -1}))
show("translate None table", lambda: "abc".translate(None))
show("translate 3-arg delete wins", lambda: "abc".translate(str.maketrans("a", "x", "a")))
show("translate empty string", lambda: "".translate({97: "x"}))
show("translate unicode", lambda: "Ünïcödé".translate(str.maketrans("Üïöé", "Uioe")))

show("bytes.maketrans", lambda: bytes.maketrans(b"ab", b"xy")[97:99])
show("bytes.maketrans len", lambda: len(bytes.maketrans(b"", b"")))
show("bytes translate", lambda: b"abcabc".translate(bytes.maketrans(b"ab", b"xy")))
show("bytes translate delete", lambda: b"hello".translate(None, b"l"))
show("bytes translate bad table", lambda: b"abc".translate(b"short"))
show("bytes maketrans unequal", lambda: bytes.maketrans(b"ab", b"x"))
show("bytearray translate", lambda: bytearray(b"abc").translate(None, b"b"))
