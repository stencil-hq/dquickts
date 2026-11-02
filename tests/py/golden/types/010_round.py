"""L0 golden: round() with banker's rounding for ints and floats.

Covers round-half-to-even, ndigits positive/zero/negative/None, return types,
exact decimal behaviour of binary floats (2.675 -> 2.67), big ints and errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/010_round.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for x in (0.5, 1.5, 2.5, 3.5, -0.5, -1.5, -2.5, 0.4999999999999999, 0.5000000000000001,
          2.4, 2.6, -2.6, 1e16 + 2, 1e300, 0.0, -0.0):
    show("round(%r)" % x, lambda: round(x))

show("type(round(2.5))", lambda: type(round(2.5)).__name__)
show("type(round(2.5, 0))", lambda: type(round(2.5, 0)).__name__)
show("type(round(2.5, None))", lambda: type(round(2.5, None)).__name__)
show("round(2.5, 0)", lambda: round(2.5, 0))
show("round(3.5, 0)", lambda: round(3.5, 0))
show("round(-0.5, 0)", lambda: round(-0.5, 0))
show("round(-0.4)", lambda: round(-0.4))
show("round(-0.4, 0)", lambda: round(-0.4, 0))

for x, n in ((2.675, 2), (1.005, 2), (0.125, 2), (0.375, 2), (2.5, 1), (0.05, 1), (0.15, 1),
             (0.25, 1), (0.35, 1), (1.23456789, 4), (-1.23456789, 4), (123.456, -1),
             (125.0, -1), (135.0, -1), (155.0, -1), (1234.5, -2), (1e300, -300), (1.5e300, -300),
             (5e-324, 323), (5e-324, 400), (1.25, 1000), (1e308, -308), (12.34, -5)):
    show("round(%r, %d)" % (x, n), lambda: round(x, n))

for x, n in ((15, -1), (25, -1), (35, -1), (-25, -1), (-35, -1), (1234, -2), (1250, -2),
             (1350, -2), (5, -1), (-5, -1), (4, -1), (6, -1), (12345, 0), (12345, 3),
             (2 ** 100, -10), (10 ** 30 + 5 * 10 ** 9, -10), (10 ** 30 + 15 * 10 ** 9, -10),
             (999, -3), (500, -3), (1500, -3), (7, -100)):
    show("round(%r, %d)" % (x, n), lambda: round(x, n))

show("type(round(25, -1))", lambda: type(round(25, -1)).__name__)
show("type(round(True))", lambda: type(round(True)).__name__)
show("round(True, 2)", lambda: round(True, 2))
show("round(float('inf'))", lambda: round(float("inf")))
show("round(float('nan'))", lambda: round(float("nan")))
show("round(float('inf'), 2)", lambda: round(float("inf"), 2))
show("round(float('nan'), 2)", lambda: round(float("nan"), 2))
show("round(1.5, 1.0)", lambda: round(1.5, 1.0))
show("round('1.5')", lambda: round("1.5"))
show("round(1j)", lambda: round(1j))
show("round()", lambda: round())
show("round(2.5, ndigits=None)", lambda: round(2.5, ndigits=None))
show("round(number=2.567, ndigits=1)", lambda: round(number=2.567, ndigits=1))
show("round(1.7976931348623157e308, -308)", lambda: round(1.7976931348623157e308, -308))
show("round(0.1 + 0.2, 15)", lambda: round(0.1 + 0.2, 15))
show("round(0.1 + 0.2, 16)", lambda: round(0.1 + 0.2, 16))
show("round(0.1 + 0.2, 17)", lambda: round(0.1 + 0.2, 17))
show("round(-0.0, 1)", lambda: round(-0.0, 1))
show("round(-0.04, 1)", lambda: round(-0.04, 1))
show("round(2**1023 * 1.0, -300) > 0", lambda: round(2.0 ** 1023, -300) > 0)
show("round(10**400, -399)", lambda: round(10 ** 400, -399) == 10 ** 400)
