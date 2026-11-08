"""L0 golden: int exponentiation, three-argument pow and modular inverse.

Covers `**` with negative exponents (float result), zero to negative powers,
`pow(a, b, m)` including negative modulus and negative exponent (modular
inverse), and the related TypeError/ValueError messages.

Usage: python3.15 -I -X utf8 tests/py/golden/types/003_int_pow.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("2**10", lambda: 2 ** 10)
show("2**0", lambda: 2 ** 0)
show("0**0", lambda: 0 ** 0)
show("0**5", lambda: 0 ** 5)
show("(-1)**1001", lambda: (-1) ** 1001)
show("(-1)**1000", lambda: (-1) ** 1000)
show("-1**2", lambda: -1 ** 2)
show("2**3**2", lambda: 2 ** 3 ** 2)
show("(2**3)**2", lambda: (2 ** 3) ** 2)
show("2**-1", lambda: 2 ** -1)
show("2**-2", lambda: 2 ** -2)
show("10**-1", lambda: 10 ** -1)
show("10**-3", lambda: 10 ** -3)
show("(-2)**-3", lambda: (-2) ** -3)
show("1**-100", lambda: 1 ** -100)
show("2**-1 type", lambda: type(2 ** -1).__name__)
show("0**-1", lambda: 0 ** -1)
show("0**-2", lambda: 0 ** -2)
show("2**0.5", lambda: 2 ** 0.5)
show("4**0.5", lambda: 4 ** 0.5)
show("2**-1074", lambda: 2 ** -1074)
show("2**-1080", lambda: 2 ** -1080)
show("2**1023 type", lambda: type(2 ** 1023).__name__)
show("10**30", lambda: 10 ** 30)
show("7**77", lambda: 7 ** 77)
show("(-3)**33", lambda: (-3) ** 33)
show("True**2", lambda: True ** 2)
show("2**True", lambda: 2 ** True)

show("pow(2, 10)", lambda: pow(2, 10))
show("pow(2, -1)", lambda: pow(2, -1))
show("pow(3, 4, 5)", lambda: pow(3, 4, 5))
show("pow(-3, 3, 5)", lambda: pow(-3, 3, 5))
show("pow(3, 3, -5)", lambda: pow(3, 3, -5))
show("pow(-3, 3, -5)", lambda: pow(-3, 3, -5))
show("pow(3, 0, 1)", lambda: pow(3, 0, 1))
show("pow(3, 0, 7)", lambda: pow(3, 0, 7))
show("pow(0, 0, 7)", lambda: pow(0, 0, 7))
show("pow(3, -1, 7)", lambda: pow(3, -1, 7))
show("pow(3, -2, 7)", lambda: pow(3, -2, 7))
show("pow(-3, -1, 7)", lambda: pow(-3, -1, 7))
show("pow(3, -1, -7)", lambda: pow(3, -1, -7))
show("pow(38, -1, 97)", lambda: pow(38, -1, 97))
show("inverse check", lambda: 38 * pow(38, -1, 97) % 97)
show("pow(2, -1, 4)", lambda: pow(2, -1, 4))
show("pow(0, -1, 5)", lambda: pow(0, -1, 5))
show("pow(3, 2, 0)", lambda: pow(3, 2, 0))
show("pow(3, -1, 1)", lambda: pow(3, -1, 1))
show("pow(2, 100, 10**9+7)", lambda: pow(2, 100, 10 ** 9 + 7))
show("pow(2**100, 2**100, 10**9+7)", lambda: pow(2 ** 100, 2 ** 100, 10 ** 9 + 7))
show("pow(12345, 10**18, 2**61-1)", lambda: pow(12345, 10 ** 18, 2 ** 61 - 1))
show("pow(7, 2**64 + 3, 2**127 - 1)", lambda: pow(7, 2 ** 64 + 3, 2 ** 127 - 1))
show("pow(2, 0.5, 3)", lambda: pow(2, 0.5, 3))
show("pow(2.0, 2, 3)", lambda: pow(2.0, 2, 3))
show("pow(2, 2, 3.0)", lambda: pow(2, 2, 3.0))
show("pow('2', 2)", lambda: pow("2", 2))
show("pow(base=2, exp=5)", lambda: pow(base=2, exp=5))
show("pow(base=2, exp=5, mod=3)", lambda: pow(base=2, exp=5, mod=3))
show("pow(2, 3, None)", lambda: pow(2, 3, None))
show("2 ** None", lambda: 2 ** None)

x = 3
x **= 4
show("**= ", lambda: x)
y = 2
y **= -2
show("**= neg", lambda: y)
