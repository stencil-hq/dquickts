"""L0 golden: arbitrary-precision int arithmetic around machine-word boundaries.

Covers factorials, large powers of two, negative big ints and the carry/borrow
behaviour at the 2**31, 2**32, 2**53, 2**63 and 2**64 boundaries where a VM
with fast small-int paths must promote to big ints.

Usage: python3.15 -I -X utf8 tests/py/golden/types/001_int_bigint.py
"""


def factorial(n: int) -> int:
    """Return n! computed with a plain loop (no math import)."""
    result = 1
    for i in range(2, n + 1):
        result *= i
    return result


for n in (0, 1, 5, 10, 12, 13, 20, 21, 25, 30):
    print("fact", n, factorial(n))
print("fact 100", factorial(100))
print("fact 100 digits", len(str(factorial(100))))
print("fact 100 trailing zeros", len(str(factorial(100))) - len(str(factorial(100)).rstrip("0")))
print("fact 70 / fact 69", factorial(70) // factorial(69))

big = 2 ** 200
print("2**200", big)
print("-2**200", -2 ** 200)
print("(-2)**201", (-2) ** 201)
print("(-2)**200 > 0", (-2) ** 200 > 0)
print("big + 1", big + 1)
print("big - 1", big - 1)
print("big - big", repr(big - big))
print("big * big", big * big)
print("big * -1", big * -1)
print("big * 0", repr(big * 0))
print("-big - big", -big - big)
print("big + (-big - 1)", big + (-big - 1))
print("big // 3", big // 3)
print("big % 7", big % 7)
print("-big // 3", -big // 3)
print("-big % 7", -big % 7)
print("big // -big", big // -big)
print("big // (big + 1)", big // (big + 1))
print("big == 2**200", big == 2 ** 200)
print("big > 2**199", big > 2 ** 199)
print("-big < -(2**199)", -big < -(2 ** 199))
print("type big", type(big).__name__)

# 32-bit boundaries
i31 = 2 ** 31 - 1
print("i31 + 1", i31 + 1)
print("-i31 - 2", -i31 - 2)
print("i31 * 2", i31 * 2)
print("i31 * i31", i31 * i31)
print("-(-2**31)", -(-2 ** 31))
print("abs(-2**31)", abs(-2 ** 31))
print("2**32 - 1 + 1", 2 ** 32 - 1 + 1)
print("65536 * 65536", 65536 * 65536)
print("46341 * 46341", 46341 * 46341)
print("-46341 * 46341", -46341 * 46341)

# 53-bit (float mantissa) boundaries: ints stay exact
m53 = 2 ** 53
print("2**53 + 1", m53 + 1)
print("2**53 + 1 == 2**53", m53 + 1 == m53)
print("2**53 * 3 + 1", m53 * 3 + 1)
print("9007199254740993 - 1", 9007199254740993 - 1)
print("(2**53+1) * (2**53+1)", (m53 + 1) * (m53 + 1))

# 64-bit boundaries
i63 = 2 ** 63 - 1
print("i63 + 1", i63 + 1)
print("-i63 - 1 - 1", -i63 - 1 - 1)
print("-(-2**63)", -(-2 ** 63))
print("(2**64 - 1) + 1", (2 ** 64 - 1) + 1)
print("2**32 * 2**32", 2 ** 32 * 2 ** 32)
print("(2**64 - 1) * (2**64 - 1)", (2 ** 64 - 1) * (2 ** 64 - 1))
print("-2**63 // -1", -2 ** 63 // -1)
print("-2**63 % -1", repr(-2 ** 63 % -1))
print("-2**31 // -1", -2 ** 31 // -1)

# accumulate and cancel
acc = 0
for k in range(1, 40):
    acc += 3 ** k
print("sum 3**k", acc)
for k in range(1, 40):
    acc -= 3 ** k
print("cancelled", repr(acc))

# augmented assignment on big ints
x = 1
for _ in range(100):
    x *= 10
print("10**100 via *=", x == 10 ** 100, len(str(x)))
x //= 10 ** 99
print("after //=", x)
x -= 11
print("after -=", x)
print("negative big repr", repr(-(10 ** 30)))
print("unary plus big", +(-(10 ** 25)))
print("mixed signs product", (-3) ** 41 * (-5) ** 3)
