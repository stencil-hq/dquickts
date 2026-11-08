"""L0 golden: int shift operators.

Left shifts grow without bound, right shifts of negatives round toward
negative infinity, and negative shift counts raise ValueError.

Usage: python3.15 -I -X utf8 tests/py/golden/types/006_int_shifts.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


for n in (0, 1, 7, 31, 32, 33, 52, 53, 63, 64, 65, 100):
    print("1 <<", n, 1 << n)
for n in (1, 31, 32, 63, 64, 200):
    print("-1 <<", n, -1 << n)

show("1 >> 1", lambda: 1 >> 1)
show("-1 >> 1", lambda: -1 >> 1)
show("-1 >> 100", lambda: -1 >> 100)
show("-2 >> 1", lambda: -2 >> 1)
show("-3 >> 1", lambda: -3 >> 1)
show("-7 >> 2", lambda: -7 >> 2)
show("7 >> 2", lambda: 7 >> 2)
show("-8 >> 2", lambda: -8 >> 2)
show("-9 >> 2", lambda: -9 >> 2)
show("2**200 >> 199", lambda: 2 ** 200 >> 199)
show("2**200 >> 201", lambda: 2 ** 200 >> 201)
show("-(2**200) >> 199", lambda: -(2 ** 200) >> 199)
show("-(2**200) >> 1000", lambda: -(2 ** 200) >> 1000)
show("(2**200 + 1) >> 0", lambda: (2 ** 200 + 1) >> 0)
show("5 >> 2**100", lambda: 5 >> 2 ** 100)
show("-5 >> 2**100", lambda: -5 >> 2 ** 100)
show("0 << 2**100", lambda: 0 << 2 ** 100)
show("0xFFFFFFFF << 1", lambda: 0xFFFFFFFF << 1)
show("0x7FFFFFFF << 1", lambda: 0x7FFFFFFF << 1)
show("-0x80000000 << 1", lambda: -0x80000000 << 1)
show("3 << 62", lambda: 3 << 62)
show("(1 << 64) >> 63", lambda: (1 << 64) >> 63)
show("x >> n == x // 2**n neg", lambda: all((-12345 >> n) == -12345 // 2 ** n for n in range(40)))
show("x << n == x * 2**n neg", lambda: all((-12345 << n) == -12345 * 2 ** n for n in range(80)))
show("True << 3", lambda: True << 3)
show("1 << True", lambda: 1 << True)

show("1 << -1", lambda: 1 << -1)
show("1 >> -1", lambda: 1 >> -1)
show("1 << -(2**100)", lambda: 1 << -(2 ** 100))
show("1 << 1.0", lambda: 1 << 1.0)
show("1.0 << 1", lambda: 1.0 << 1)
show("1 >> None", lambda: 1 >> None)
show("'a' << 1", lambda: "a" << 1)

x = 1
x <<= 70
show("<<=", lambda: x)
x >>= 68
show(">>=", lambda: x)
y = -100
y >>= 3
show(">>= neg", lambda: y)
show("precedence", lambda: (1 + 2 << 3, 1 << 2 + 3, 2 * 3 >> 1, 16 >> 1 >> 1))
