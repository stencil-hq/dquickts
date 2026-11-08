"""L0 golden: int() construction from strings, bytes and other objects.

Covers explicit bases 2-36, base 0 prefix detection, underscores, surrounding
whitespace, signs, non-ASCII digits and the ValueError/TypeError messages.

Usage: python3.15 -I -X utf8 tests/py/golden/types/007_int_parse.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for text in ("0", "42", "-42", "+42", "  17  ", "\t\n 8 \r\n", "007", "-0", "1_000_000",
             "123456789012345678901234567890", "\u0663\u0664", "\uff11\uff12", "\u00a012\u2003"):
    show("int(%r)" % text, lambda: int(text))

for text in ("", " ", "1.5", "1e3", "abc", "1__0", "_1", "1_", "--1", "+-1", "1 2", "0x10",
             "\u00b2", "1\x00", "١_٢"):
    show("int(%r)" % text, lambda: int(text))

for text, base in (("ff", 16), ("FF", 16), ("-ff", 16), ("0xff", 16), ("0XfF", 16), ("777", 8),
                   ("0o777", 8), ("101", 2), ("0b101", 2), ("z", 36), ("ZZ", 36), ("10", 36),
                   ("1_0", 2), ("0x_ff", 16), ("12", 3), ("  -1a  ", 11)):
    show("int(%r, %d)" % (text, base), lambda: int(text, base))

for text, base in (("2", 2), ("8", 8), ("g", 16), ("0b1", 16 - 6), ("0x1", 10), ("10", 37),
                   ("10", 1), ("10", -1), ("0o8", 8)):
    show("int(%r, %d)" % (text, base), lambda: int(text, base))

for text in ("0x1f", "0o17", "0b11", "0X1F", "0O17", "0B11", "42", "0", "00", "000", "-0x10",
             "0_0", "0x_1", "010", "0_7", "1_0", "  0b1_1  "):
    show("int(%r, 0)" % text, lambda: int(text, 0))

show("int(b'12')", lambda: int(b"12"))
show("int(b'ff', 16)", lambda: int(b"ff", 16))
show("int(bytearray(b'-7'))", lambda: int(bytearray(b"-7")))
show("int(b' 1 ')", lambda: int(b" 1 "))
show("int()", lambda: int())
show("int(x='5')", lambda: int(x="5"))
show("int('5', base=8)", lambda: int("5", base=8))
show("int(5, 10)", lambda: int(5, 10))
show("int(None)", lambda: int(None))
show("int([1])", lambda: int([1]))
show("int('1', '2')", lambda: int("1", "2"))
show("int(True)", lambda: int(True))
show("type(int(True))", lambda: type(int(True)).__name__)
show("int('9' * 50)", lambda: int("9" * 50))
show("int('-' + '1' * 40, 2)", lambda: int("-" + "1" * 40, 2))
show("int('1' * 5000)", lambda: int("1" * 5000))
show("int('1' * 5000, 16) bits", lambda: int("1" * 5000, 16).bit_length())
show("int('1' * 4300) digits", lambda: len(repr(int("1" * 4300))))
show("int(' 0x10 ', 0)", lambda: int(" 0x10 ", 0))
show("int('0x', 0)", lambda: int("0x", 0))
show("int('0x', 16)", lambda: int("0x", 16))
show("int('-0b101', 0)", lambda: int("-0b101", 0))
show("int('Infinity')", lambda: int("Infinity"))
show("int('nan')", lambda: int("nan"))
