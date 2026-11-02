"""L0 golden: float() parsing of strings and conversion of other objects.

Covers whitespace, signs, underscores, exponent forms, inf/nan spellings
(case-insensitive, 'infinity'), correctly rounded decimal parsing, bytes
input, non-ASCII digits and the ValueError/TypeError messages.

Usage: python3.15 -I -X utf8 tests/py/golden/types/017_float_parse.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


GOOD = [
    "0", "1", "-1", "+1", "1.", ".5", "-.5", "1.5", "  2.5  ", "\t\n3.25\r\n", "1e3", "1E3",
    "1e+3", "1e-3", "-1.5e-3", "1_0.5", "1_000.000_1", "1e1_0", "0.1", "0.30000000000000004",
    "123456789012345678901234567890", "1e308", "1.7976931348623157e308", "1.7976931348623158e308",
    "1.7976931348623159e308", "1e309", "-1e309", "5e-324", "2e-324", "3e-324", "1e-400", "-1e-400",
    "inf", "INF", "-inf", "+inf", "Inf", "infinity", "Infinity", "-INFINITY", "  inf  ",
    "nan", "NaN", "-nan", "+NAN", "0e0", "-0", "-0.0", "00001.5", "1.0000000000000002",
    "9007199254740993", "9007199254740993.0000000001", "\u0661.\u0662", "\uff15",
    "\u00a01.5\u2003", "2.2250738585072011e-308", "4.9406564584124654e-324",
]
for text in GOOD:
    show("float(%r)" % text, lambda: float(text))

BAD = [
    "", " ", "abc", "1.5.5", "1e", "e3", "1e3.5", "1__0", "_1", "1_", "1_.5", "1._5", "1e_3",
    "0x10", "1,5", "- 1", "--1", "infinit", "infinityy", "nann", "in f", "1 e3", "1j", "\x00",
    "1.5\x00", "\u00bd", "+", ".", "-.", "e", "0b1",
]
for text in BAD:
    show("float(%r)" % text, lambda: float(text))

show("float()", lambda: float())
show("float(b'1.5')", lambda: float(b"1.5"))
show("float(bytearray(b' -2 '))", lambda: float(bytearray(b" -2 ")))
show("float(b'inf')", lambda: float(b"inf"))
show("float(b'x')", lambda: float(b"x"))
show("float(7)", lambda: float(7))
show("float(-7)", lambda: float(-7))
show("float(True)", lambda: float(True))
show("float(2**53 + 1)", lambda: float(2 ** 53 + 1))
show("float(10**400)", lambda: float(10 ** 400))
show("float(None)", lambda: float(None))
show("float([1])", lambda: float([1]))
show("float(1j)", lambda: float(1j))
show("float(x='1')", lambda: float(x="1"))
show("float('1', '2')", lambda: float("1", "2"))
show("float.fromhex('0x1p-1')", lambda: float.fromhex("0x1p-1"))
show("type(float('1'))", lambda: type(float("1")).__name__)
show("float('0.1') == 0.1", lambda: float("0.1") == 0.1)
show("float('1e23') == 1e23", lambda: float("1e23") == 1e23)
show("float('1e16') repr", lambda: repr(float("1e16")))
show("float(str(1/3)) == 1/3", lambda: float(str(1 / 3)) == 1 / 3)
show("float('-0').hex()", lambda: float("-0").hex())
show("long digit string", lambda: float("0." + "0" * 300 + "1"))
show("long mantissa", lambda: float("1" + "0" * 400 + "e-400"))
