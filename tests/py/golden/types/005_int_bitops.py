"""L0 golden: bitwise operators on negative and big ints.

Python ints behave like infinite two's-complement bit strings, so `&`, `|`,
`^` and `~` on negative or huge values must never truncate to 32/64 bits.

Usage: python3.15 -I -X utf8 tests/py/golden/types/005_int_bitops.py
"""

VALUES = [0, 1, -1, 5, -5, 0xFF, -256, 2 ** 31, -2 ** 31, 2 ** 32 - 1, 2 ** 64 + 7, -(2 ** 70) + 3]

for a in VALUES:
    print("invert", a, ~a)

for a, b in ((12, 10), (-12, 10), (12, -10), (-12, -10), (-1, 2 ** 100), (2 ** 100 + 5, -3), (-(2 ** 65), -(2 ** 64))):
    print("and", a, b, a & b)
    print("or ", a, b, a | b)
    print("xor", a, b, a ^ b)

print("mask low byte of -1", -1 & 0xFF)
print("mask low 64 of -1", -1 & (2 ** 64 - 1))
print("mask low 32 of -2**31", -2 ** 31 & 0xFFFFFFFF)
print("big & small", (2 ** 200 + 0b1011) & 0b1110)
print("big | big", (2 ** 200) | (2 ** 100))
print("big ^ itself", repr((2 ** 200 + 12345) ^ (2 ** 200 + 12345)))
print("~big", ~(2 ** 200))
print("~~x", ~~-123456789)
print("-x == ~x + 1", all(-x == ~x + 1 for x in VALUES))
print("x ^ -1 == ~x", all(x ^ -1 == ~x for x in VALUES))
print("de morgan", all(~(a & b) == (~a | ~b) for a in VALUES for b in VALUES))

print("bool ops", True & False, True | False, True ^ True, repr(True & 1), repr(False | 0))
print("bool & type", type(True & True).__name__, type(True & 1).__name__)
print("int & bool", 3 & True, type(3 & True).__name__)

for label, thunk in (
    ("1 & 1.0", lambda: 1 & 1.0),
    ("~1.5", lambda: ~1.5),
    ("1 | 'a'", lambda: 1 | "a"),
    ("None ^ 1", lambda: None ^ 1),
):
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))

x = 0b1100
x &= 0b1010
print("&=", bin(x))
x |= 0b0101
print("|=", bin(x))
x ^= 0b1111
print("^=", bin(x))
print("precedence", 1 | 2 ^ 3 & 4, (1 | 2) ^ (3 & 4), 1 + 2 & 3, 1 << 2 | 1)
print("bin of results", bin(-12 & 10), bin(-12 | 10), bin(-12 ^ 10))
