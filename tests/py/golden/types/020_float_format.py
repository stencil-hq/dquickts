"""L0 golden: float.__format__, %-formatting and round(2.675, 2).

Covers the f/e/g/%/n presentation types, default and explicit precision,
alignment, sign, grouping, '#' alternate form, 'z' negative-zero coercion,
special values and invalid specs.

Usage: python3.15 -I -X utf8 tests/py/golden/types/020_float_format.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("round(2.675, 2)", lambda: round(2.675, 2))
show("format(2.675, '.2f')", lambda: format(2.675, ".2f"))
show("format(2.665, '.2f')", lambda: format(2.665, ".2f"))
show("format(0.125, '.2f')", lambda: format(0.125, ".2f"))
show("format(0.375, '.2f')", lambda: format(0.375, ".2f"))
show("format(2.5, '.0f')", lambda: format(2.5, ".0f"))
show("format(3.5, '.0f')", lambda: format(3.5, ".0f"))

SPECS = ["", "f", ".0f", ".1f", ".3f", "10.2f", "<10.2f", "^10.2f", ">10.2f", "010.2f", "+.2f", " .2f",
         "e", ".2e", "E", "g", ".2g", ".10g", "G", "%", ".1%", ",", ",.2f", "_.3f", "#.0f", "#g",
         "*^12.3f", "=+12.2f", ".3", "n", ".20f", "z.2f"]
VALUES = [3.14159, -2.5, 1234567.891, 0.000012345, 1e16, 0.0, -0.0, 100.0]
for spec in SPECS:
    print("spec", repr(spec), [format(v, spec) for v in VALUES])

show("format(1.5, 'r')", lambda: format(1.5, "r"))

show("format(1e16, '')", lambda: format(1e16, ""))
show("format(1e15, 'g')", lambda: format(1e15, "g"))
show("format(1e-5, 'g')", lambda: format(1e-5, "g"))
show("format(1e-4, 'g')", lambda: format(1e-4, "g"))
show("format(123456789.0, 'g')", lambda: format(123456789.0, "g"))
show("format(0.1, '.17f')", lambda: format(0.1, ".17f"))
show("format(0.1, '.55f')", lambda: format(0.1, ".55f"))
show("format(1e300, 'f') len", lambda: len(format(1e300, "f")))
show("format(5e-324, '.3e')", lambda: format(5e-324, ".3e"))
show("format(inf, 'f')", lambda: format(float("inf"), "f"))
show("format(-inf, 'E')", lambda: format(float("-inf"), "E"))
show("format(nan, '10.2f')", lambda: format(float("nan"), "10.2f"))
show("format(nan, 'F')", lambda: format(float("nan"), "F"))
show("format(inf, '%')", lambda: format(float("inf"), "%"))
show("format(1.5, 'd')", lambda: format(1.5, "d"))
show("format(1.5, 'x')", lambda: format(1.5, "x"))
show("format(1.5, 's')", lambda: format(1.5, "s"))
show("format(1.5, 'c')", lambda: format(1.5, "c"))
show("format(1.5, '.f2')", lambda: format(1.5, ".f2"))
show("format(1.5, ',_')", lambda: format(1.5, ",_"))
show("format(1234.5, '.2f,')", lambda: format(1234.5, ".2f,"))
show("f'{1234.5678:.2f}'", lambda: f"{1234.5678:.2f}")
show("f'{1234.5678:,.1f}'", lambda: f"{1234.5678:,.1f}")
show("f'{0.5:.0%}'", lambda: f"{0.5:.0%}")
show("f'{-1.0:+}'", lambda: f"{-1.0:+}")
show("f'{1.0:+}'", lambda: f"{1.0:+}")
show("f'{1.0: }'", lambda: f"{1.0: }")
show("f'{1.0:5}'", lambda: f"{1.0:5}")
show("f'{12.0:.3}'", lambda: f"{12.0:.3}")
show("f'{1e20:.3}'", lambda: f"{1e20:.3}")
show("'%f' % 1.5", lambda: "%f" % 1.5)
show("'%.2f' % 2.675", lambda: "%.2f" % 2.675)
show("'%e' % 12345.678", lambda: "%e" % 12345.678)
show("'%g' % 0.00001", lambda: "%g" % 0.00001)
show("'%10.3f|%-10.3f|' % (...)", lambda: "%10.3f|%-10.3f|" % (3.14159, 3.14159))
show("'%+.1f %+.1f' % (...)", lambda: "%+.1f %+.1f" % (1.25, -1.25))
show("'%s %r' % (0.1, 1e16)", lambda: "%s %r" % (0.1, 1e16))
show("'%d' % 3.99", lambda: "%d" % 3.99)
show("'%i' % -3.99", lambda: "%i" % -3.99)
show("'%f' % 'x'", lambda: "%f" % "x")
show("'%.3g' % 1234567", lambda: "%.3g" % 1234567)
show("'%#.3g' % 1.0", lambda: "%#.3g" % 1.0)
show("'%f' % float('inf')", lambda: "%f" % float("inf"))
show("'{:.2f}'.format(1/3)", lambda: "{:.2f}".format(1 / 3))
show("'{0:>8.3f}'.format(-1.0)", lambda: "{0:>8.3f}".format(-1.0))
