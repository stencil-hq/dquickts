"""L0 golden: float arithmetic, mixed int/float result types and overflow.

Covers floor division and modulo with negative float operands, divmod,
result types for every int/float/bool/complex combination, OverflowError on
int->float conversion and float power overflow.

Usage: python3.15 -I -X utf8 tests/py/golden/types/019_float_arith.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for a, b in ((7.5, 2), (-7.5, 2), (7.5, -2), (-7.5, -2), (7, 2.5), (-7, 2.5), (0.0, 3), (-0.0, 3),
             (1.0, 0.1), (-1.0, 0.1), (1e300, 3.0), (-1e-300, 1.0), (5.0, 1e300), (-5.0, 1e300),
             (0.3, 0.1), (6.0, -3.0), (-6.0, 3.0)):
    show("%r // %r" % (a, b), lambda: a // b)
    show("%r %% %r" % (a, b), lambda: a % b)
    show("divmod(%r, %r)" % (a, b), lambda: divmod(a, b))

OPERANDS = [1, 1.0, True, 1j]
for a in OPERANDS:
    for b in OPERANDS:
        print("types", type(a).__name__, type(b).__name__, "+", type(a + b).__name__,
              "*", type(a * b).__name__, "/", type(a / b).__name__)

show("1 + 1.0", lambda: 1 + 1.0)
show("2 * 0.5", lambda: 2 * 0.5)
show("3 - 3.0", lambda: 3 - 3.0)
show("10 ** 2.0", lambda: 10 ** 2.0)
show("10.0 ** 2", lambda: 10.0 ** 2)
show("4 ** 0.5", lambda: 4 ** 0.5)
show("(-8) ** (1/3) type", lambda: type((-8) ** (1 / 3)).__name__)
show("(-8.0) ** 2.0", lambda: (-8.0) ** 2.0)
show("(-8.0) ** 3", lambda: (-8.0) ** 3)
show("0.0 ** 0.0", lambda: 0.0 ** 0.0)
show("0.0 ** 0.5", lambda: 0.0 ** 0.5)
show("10.0 ** 308", lambda: 10.0 ** 308)
show("10.0 ** 309", lambda: 10.0 ** 309)
show("2.0 ** 1023 * 2", lambda: 2.0 ** 1023 * 2)
show("1e308 + 1e308 (no error)", lambda: 1e308 + 1e308)
show("10 ** 309 * 1.0", lambda: 10 ** 309 * 1.0)
show("10 ** 309 + 1.0", lambda: 10 ** 309 + 1.0)
show("float(10 ** 309)", lambda: float(10 ** 309))
show("1.0 * 2 ** 1024", lambda: 1.0 * 2 ** 1024)
show("2 ** 1024 / 2.0", lambda: 2 ** 1024 / 2.0)
show("2 ** 1024 // 2.0", lambda: 2 ** 1024 // 2.0)
show("2 ** 1023 // 2.0", lambda: 2 ** 1023 // 2.0)
show("1.5 + True", lambda: 1.5 + True)
show("0.1 * 3 == 0.3", lambda: 0.1 * 3 == 0.3)
show("0.5 * 4 == 2", lambda: 0.5 * 4 == 2)
show("1.0 + 2**53", lambda: 1.0 + 2 ** 53)
show("2**53 + 1.0 == 2**53", lambda: 2 ** 53 + 1.0 == 2 ** 53)
show("-(-1.5)", lambda: -(-1.5))
show("+(-1.5)", lambda: +(-1.5))
show("abs(-1.5)", lambda: abs(-1.5))
show("1.0 + 'a'", lambda: 1.0 + "a")
show("1.0 * 'a'", lambda: 1.0 * "a")
show("1.0 // 1j", lambda: 1.0 // 1j)
show("1.0 % None", lambda: 1.0 % None)
show("-1.0 ** 0.5", lambda: -1.0 ** 0.5)
show("pow(-1.0, 0.5) type", lambda: type(pow(-1.0, 0.5)).__name__)
show("0.0 ** -1.0", lambda: 0.0 ** -1.0)
show("(-0.0) ** -1", lambda: (-0.0) ** -1)
show("1e-320 / 1e10", lambda: 1e-320 / 1e10)
show("1e-320 * 1e-320", lambda: 1e-320 * 1e-320)
show("sum floats", lambda: sum([0.1, 0.2, 0.3]))
show("sum floats start", lambda: sum([0.1, 0.2], 1.0))
show("sum ints then float", lambda: sum([1, 2, 3.5]))
show("sum compensated", lambda: sum([1e100, 1.0, -1e100]))
show("sum many tenths", lambda: sum([0.1] * 10))
acc = 0.0
for _ in range(10):
    acc += 0.1
show("x += 0.1 loop", lambda: acc)

x = 5
x /= 2
show("int /= -> float", lambda: x)
y = 5.0
y //= 2
show("float //= ", lambda: y)
z = -5.5
z %= 2
show("float %= ", lambda: z)
