"""L0 golden: int helper builtins and methods.

Covers abs, hex/oct/bin (negatives, big), bit_length, bit_count, is_integer,
as_integer_ratio, numerator/denominator, conjugate and int.__format__ basics.

Usage: python3.15 -I -X utf8 tests/py/golden/types/011_int_methods.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


VALUES = [0, 1, -1, 7, -7, 255, -256, 2 ** 31 - 1, -2 ** 31, 2 ** 64, -(2 ** 64) + 1, 3 ** 50]

for v in VALUES:
    print("abs", v, abs(v))
    print("hex/oct/bin", v, hex(v), oct(v), bin(v))
    print("bit_length/bit_count", v, v.bit_length(), v.bit_count())

show("abs(-2**200)", lambda: abs(-2 ** 200))
show("abs(-0)", lambda: abs(-0))
show("abs(False)", lambda: abs(False))
show("abs('1')", lambda: abs("1"))
show("hex(1.0)", lambda: hex(1.0))
show("bin('1')", lambda: bin("1"))
show("oct(-0)", lambda: oct(-0))
show("hex(2**100)", lambda: hex(2 ** 100))
show("bin(-2**40)", lambda: bin(-2 ** 40))
show("(0).bit_length()", lambda: (0).bit_length())
show("(2**1000).bit_length()", lambda: (2 ** 1000).bit_length())
show("(2**1000-1).bit_count()", lambda: (2 ** 1000 - 1).bit_count())
show("(-2**1000).bit_count()", lambda: (-2 ** 1000).bit_count())
show("(5).is_integer()", lambda: (5).is_integer())
show("(5).as_integer_ratio()", lambda: (5).as_integer_ratio())
show("(-5).as_integer_ratio()", lambda: (-5).as_integer_ratio())
show("True.as_integer_ratio()", lambda: True.as_integer_ratio())
show("(7).numerator", lambda: (7).numerator)
show("(7).denominator", lambda: (7).denominator)
show("(7).real", lambda: (7).real)
show("(7).imag", lambda: (7).imag)
show("(-7).conjugate()", lambda: (-7).conjugate())
show("int.__trunc__(-7)", lambda: int.__trunc__(-7))
show("(-7).__floor__()", lambda: (-7).__floor__())
show("(-7).__ceil__()", lambda: (-7).__ceil__())
show("(17).__round__(-1)", lambda: (17).__round__(-1))

for spec in ("", "d", "5d", "<5d", "^5", ">5", "05d", "+d", " d", "x", "X", "#x", "#X", "o", "#o",
             "b", "#b", "08b", ",", "_", "_x", "_b", "c", "e", ".2f", "%", "n", "=+8", "*^9"):
    show("format(255, %r)" % spec, lambda: format(255, spec))

for spec in ("d", "x", "#b", "+", "05", ",", "_x"):
    show("format(-255, %r)" % spec, lambda: format(-255, spec))

show("format(10**20, ',')", lambda: format(10 ** 20, ","))
show("format(-10**20, '_')", lambda: format(-10 ** 20, "_"))
show("format(1, 's')", lambda: format(1, "s"))
show("format(1, '.2')", lambda: format(1, ".2"))
show("format(1, ',d,')", lambda: format(1, ",d,"))
show("format(0x110000, 'c')", lambda: format(0x110000, "c"))
show("format(-1, 'c')", lambda: format(-1, "c"))
show("format(65, 'c')", lambda: format(65, "c"))
show("'%d %i %x %X %o' % (...)", lambda: "%d %i %x %X %o" % (-255, 255, 255, 255, 8))
show("'%#x %#o %+d % d' % (...)", lambda: "%#x %#o %+d % d" % (255, 8, 5, 5))
show("'%5d|%-5d|%05d' % (...)", lambda: "%5d|%-5d|%05d" % (42, 42, -42))
show("'%c' % 97", lambda: "%c" % 97)
show("'%d' % 2.9", lambda: "%d" % 2.9)
show("'%x' % 2.0", lambda: "%x" % 2.0)
show("'%d' % '1'", lambda: "%d" % "1")
show("f'{12345678:,}'", lambda: f"{12345678:,}")
show("f'{-42:=+10}'", lambda: f"{-42:=+10}")
