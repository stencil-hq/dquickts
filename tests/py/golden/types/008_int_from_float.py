"""L0 golden: int() of floats truncates toward zero; huge floats become big ints.

Also covers OverflowError for infinities, ValueError for NaN, and exactness of
int(float) for values beyond 2**53.

Usage: python3.15 -I -X utf8 tests/py/golden/types/008_int_from_float.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for f in (0.0, -0.0, 0.5, -0.5, 0.9999999999999999, 1.0, 1.5, -1.5, 2.5, -2.5, 3.7, -3.7,
          1e-300, -1e-300, 123456.789, -123456.789, 2147483647.9, -2147483648.9,
          4294967296.5, 9007199254740993.0, 1e16, -1e16, 1e18, 2.0 ** 63, -(2.0 ** 63),
          2.0 ** 64, 1e20, 1e22, 1e23, 1.7976931348623157e308, 5e-324):
    show("int(%r)" % f, lambda: int(f))

show("int(1e100) digits", lambda: len(str(int(1e100))))
show("int(1e100) == 10**100", lambda: int(1e100) == 10 ** 100)
show("int(2.0**200) == 2**200", lambda: int(2.0 ** 200) == 2 ** 200)
show("int(-2.0**1000) bits", lambda: int(-2.0 ** 1000).bit_length())
show("int(inf)", lambda: int(float("inf")))
show("int(-inf)", lambda: int(float("-inf")))
show("int(nan)", lambda: int(float("nan")))
show("int(1.0) type", lambda: type(int(1.0)).__name__)
show("int(-0.0) repr", lambda: repr(int(-0.0)))

show("trunc toward zero pos", lambda: [int(x) for x in (0.1, 0.9, 1.1, 1.9)])
show("trunc toward zero neg", lambda: [int(x) for x in (-0.1, -0.9, -1.1, -1.9)])
show("floor div vs int", lambda: [(-7.5 // 1, int(-7.5)), (7.5 // 1, int(7.5))])

show("int('1.0')", lambda: int("1.0"))
show("int(float('1.0'))", lambda: int(float("1.0")))
show("int(3.0 + 0j)", lambda: int(3.0 + 0j))
show("int(1e308 * 10)", lambda: int(1e308 * 10))
show("int(2**53 + 1.0)", lambda: int(2 ** 53 + 1.0))
show("int(2**53 + 2.0)", lambda: int(2 ** 53 + 2.0))
show("int(0.1 * 3)", lambda: int(0.1 * 3))
show("int(2.9999999999999996)", lambda: int(2.9999999999999996))
show("int(2.99999999999999999)", lambda: int(2.99999999999999999))
show("int(float('-1e-320'))", lambda: int(float("-1e-320")))

show("float(int) roundtrip", lambda: all(float(int(x)) == x for x in (1e15, 1e16, 2.0 ** 70, -3.0e200)))
show("int(x) == x for integral floats", lambda: [int(x) == x for x in (3.0, -4.0, 1e30, 0.5)])
show("bool(int(0.4))", lambda: bool(int(0.4)))
show("int(True + 0.5)", lambda: int(True + 0.5))
show("int(-True - 0.5)", lambda: int(-True - 0.5))
show("int(4.0) // 3", lambda: int(4.0) // 3)
show("int(4.0) / 3 type", lambda: type(int(4.0) / 3).__name__)
show("int(1e300) % 7", lambda: int(1e300) % 7)
show("int(-1e300) % 7", lambda: int(-1e300) % 7)
