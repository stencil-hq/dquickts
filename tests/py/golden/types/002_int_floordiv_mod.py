"""L0 golden: int floor division, modulo and divmod with mixed-sign operands.

Python rounds `//` toward negative infinity and `%` takes the sign of the
divisor; the identity `(a // b) * b + a % b == a` must always hold.

Usage: python3.15 -I -X utf8 tests/py/golden/types/002_int_floordiv_mod.py
"""

PAIRS = [
    (7, 2), (-7, 2), (7, -2), (-7, -2),
    (6, 3), (-6, 3), (6, -3), (-6, -3),
    (0, 5), (0, -5), (1, 3), (-1, 3), (1, -3), (-1, -3),
    (5, 10), (-5, 10), (5, -10),
    (1, 10 ** 20), (-1, 10 ** 20), (1, -10 ** 20),
    (10 ** 20, -3), (-10 ** 20, 3), (10 ** 20 + 1, 10 ** 10),
    (2 ** 63, -1), (-2 ** 63, -1), (-2 ** 31, -1), (-2 ** 31, 2 ** 31),
    (2 ** 100, 2 ** 50 + 1), (-2 ** 100, 2 ** 50 + 1),
]

for a, b in PAIRS:
    q, r = divmod(a, b)
    print("pair", a, b, "//", a // b, "%", a % b, "divmod", (q, r), "ok", q * b + r == a)

print("sign of % follows divisor")
for a in (-9, -1, 0, 1, 9):
    print("  ", a, "% 4 =", a % 4, ";", a, "% -4 =", a % -4)

print("floor vs truncation")
for a, b in ((-7, 2), (7, -2), (-1, 2)):
    print("  ", a, b, "floor", a // b, "trunc", int(a / b))

print("divmod type", type(divmod(7, 2)).__name__)
print("divmod bool", divmod(True, 2), divmod(7, True))

for label, thunk in (
    ("1 // 0", lambda: 1 // 0),
    ("1 % 0", lambda: 1 % 0),
    ("divmod(1, 0)", lambda: divmod(1, 0)),
    ("0 // 0", lambda: 0 // 0),
    ("big // 0", lambda: 2 ** 200 // 0),
    ("big % 0", lambda: 2 ** 200 % 0),
    ("1 // False", lambda: 1 // False),
    ("1 // 'a'", lambda: 1 // "a"),
    ("divmod('a', 1)", lambda: divmod("a", 1)),
    ("1 % None", lambda: 1 % None),
):
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))

x = -17
x //= 5
print("augmented //=", x)
x = -17
x %= 5
print("augmented %=", x)
x = 17
x %= -5
print("augmented %= neg", x)
print("chained", -100 // 7 // 3, -100 % 7 % 3)
print("precedence", -7 // 2 * 2, -(7 // 2) * 2, -7 % 3 ** 2)
