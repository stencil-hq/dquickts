"""L0 golden: int true division always yields a correctly rounded float.

Covers `/` on ints (exact results still float), huge operands whose
quotient fits in a float, results that overflow or underflow, and the
ZeroDivisionError messages for every division-like operator.

Usage: python3.15 -I -X utf8 tests/py/golden/types/004_int_truediv.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("4/2", lambda: 4 / 2)
show("1/2", lambda: 1 / 2)
show("1/3", lambda: 1 / 3)
show("2/3", lambda: 2 / 3)
show("-1/3", lambda: -1 / 3)
show("-7/2", lambda: -7 / 2)
show("0/5", lambda: 0 / 5)
show("0/-5", lambda: 0 / -5)
show("-0/5", lambda: -0 / 5)
show("10/4", lambda: 10 / 4)
show("10**20/10", lambda: 10 ** 20 / 10)
show("10**30/3", lambda: 10 ** 30 / 3)
show("(2**53+1)/1", lambda: (2 ** 53 + 1) / 1)
show("(2**53+3)/1", lambda: (2 ** 53 + 3) / 1)
show("2**1000/2**999", lambda: 2 ** 1000 / 2 ** 999)
show("2**1100/2**1099", lambda: 2 ** 1100 / 2 ** 1099)
show("(10**400+1)/10**399", lambda: (10 ** 400 + 1) / 10 ** 399)
show("1/2**1100", lambda: 1 / 2 ** 1100)
show("-1/2**1100", lambda: -1 / 2 ** 1100)
show("2**1100/3", lambda: 2 ** 1100 / 3)
show("2**1024/1", lambda: 2 ** 1024 / 1)
show("(2**1024-1)/1", lambda: (2 ** 1024 - 1) / 1)
show("2**1023/1", lambda: 2 ** 1023 / 1)
show("1/2**1074", lambda: 1 / 2 ** 1074)
show("1/2**1075", lambda: 1 / 2 ** 1075)
show("3/2**1075", lambda: 3 / 2 ** 1075)
show("True/2", lambda: True / 2)
show("type(6/3)", lambda: type(6 / 3).__name__)
show("6/3 == 2", lambda: 6 / 3 == 2)

show("1/0", lambda: 1 / 0)
show("0/0", lambda: 0 / 0)
show("1/False", lambda: 1 / False)
show("1.0/0", lambda: 1.0 / 0)
show("1/0.0", lambda: 1 / 0.0)
show("1//0", lambda: 1 // 0)
show("1%0", lambda: 1 % 0)
show("1.0//0.0", lambda: 1.0 // 0.0)
show("1.0%0.0", lambda: 1.0 % 0.0)
show("divmod(1.0, 0)", lambda: divmod(1.0, 0))
show("pow(0, -1)", lambda: pow(0, -1))
show("0.0**-1", lambda: 0.0 ** -1)
show("2**200/0", lambda: 2 ** 200 / 0)
show("1j/0", lambda: 1j / 0)

try:
    1 / 0
except ZeroDivisionError as e:
    print("isinstance ArithmeticError", isinstance(e, ArithmeticError))
    print("args", e.args)

x = 9
x /= 3
show("/= result", lambda: x)
