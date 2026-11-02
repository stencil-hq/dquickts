"""L0 golden: float methods and attributes.

Covers is_integer, as_integer_ratio, hex/fromhex (normal, subnormal, signed
zero, extremes, rounding of long hex strings, errors), real/imag/conjugate,
__trunc__/__floor__/__ceil__ and float.from_number.

Usage: python3.15 -I -X utf8 tests/py/golden/types/018_float_methods.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for x in (0.0, -0.0, 1.0, -1.0, 1.5, 2.0, 0.1, 1e16, 1e300, 5e-324, 123.0, -2.5, 2.0 ** 52 + 0.5):
    print("is_integer", repr(x), x.is_integer())

for x in (0.0, 1.0, -1.0, 0.5, 0.1, -0.75, 2.5, 1e16, 1e-10, 3.0, 5e-324, 1.7976931348623157e308):
    print("as_integer_ratio", repr(x), x.as_integer_ratio())

for x in (0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 0.1, 3.14, 1e16, 5e-324, 2.2250738585072014e-308,
          1.7976931348623157e308, 1 / 3, -1e-300):
    print("hex", repr(x), x.hex())

FROMHEX = [
    "0x1p0", "0x1.8p1", "-0x1p-1", "0x.8p1", "0x1.", "1p4", "0X1P4", "  0x1p0  ", "0x1.fffffffffffffp+1023",
    "0x1p+1024", "0x1p-1074", "0x1p-1075", "0x1.0000000000001p-1075", "0x3p-1076", "0x0p0", "-0x0p0",
    "inf", "-Infinity", "nan", "0x1.0000000000000800p0", "0x1.0000000000000801p0",
    "0x1.0000000000001800p0", "0xffffffffffffffffp0", "0x1_0p0", "", "0x", "0xg", "0x1p", "1.5.p0",
    "0x1p0x", "0x1p99999999999999999999",
]
for text in FROMHEX:
    show("fromhex(%r)" % text, lambda: float.fromhex(text))

show("roundtrip hex", lambda: all(float.fromhex(x.hex()) == x for x in (0.1, 1e300, 5e-324, -7.25)))
show("fromhex type", lambda: type(float.fromhex("0x1p0")).__name__)
show("fromhex(5)", lambda: float.fromhex(5))
show("(1.5).real", lambda: (1.5).real)
show("(1.5).imag", lambda: (1.5).imag)
show("(1.5).conjugate()", lambda: (1.5).conjugate())
show("(-2.5).__trunc__()", lambda: (-2.5).__trunc__())
show("(-2.5).__floor__()", lambda: (-2.5).__floor__())
show("(-2.5).__ceil__()", lambda: (-2.5).__ceil__())
show("(2.5).__floor__()", lambda: (2.5).__floor__())
show("(2.5).__ceil__()", lambda: (2.5).__ceil__())
show("(1e300).__floor__() bits", lambda: (1e300).__floor__().bit_length())
show("float('inf').__floor__()", lambda: float("inf").__floor__())
show("float('nan').__ceil__()", lambda: float("nan").__ceil__())
show("(2.5).__round__()", lambda: (2.5).__round__())
show("(2.567).__round__(2)", lambda: (2.567).__round__(2))
show("float.from_number(3)", lambda: float.from_number(3))
show("float.from_number(2.5)", lambda: float.from_number(2.5))
show("float.from_number('3')", lambda: float.from_number("3"))
show("float.__getformat__('double')", lambda: float.__getformat__("double"))
show("(0.1).__abs__()", lambda: (-0.1).__abs__())
show("(3.0).__neg__()", lambda: (3.0).__neg__())
show("(7.0).__divmod__(2)", lambda: (7.0).__divmod__(2))
show("(7.0).__rsub__(1)", lambda: (7.0).__rsub__(1))
show("(7.0).__add__('a')", lambda: (7.0).__add__("a"))
show("float.__add__(1.0, 2)", lambda: float.__add__(1.0, 2))
show("float.__add__(1, 2.0)", lambda: float.__add__(1, 2.0))
