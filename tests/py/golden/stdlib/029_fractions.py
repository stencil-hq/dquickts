"""L0 golden: fractions.Fraction arithmetic and conversions."""
import math
from decimal import Decimal
from fractions import Fraction as F

print("construct", repr(F(3, 4)), F(6, 8), F(-3, -9), F(3, -9), F(5))
print("from str", F("3/7"), F(" -1.25 "), F("1e-3"), F("7"), F("1_000/3"))
print("from float", F(0.5), F(0.1), F(1e-20).denominator.bit_length())
print("from_float", F.from_float(2.75), F.from_decimal(Decimal("0.125")))
print("from Decimal", F(Decimal("1.1")))
print("parts", F(6, 8).numerator, F(6, 8).denominator, F(-1, 2).as_integer_ratio())
print("add", F(1, 3) + F(1, 6), F(1, 2) + 1, 1 - F(1, 3))
print("mul/div", F(2, 3) * F(9, 4), F(2, 3) / F(4, 9), F(1, 2) / 2)
print("floordiv/mod", F(7, 2) // F(1, 3), F(7, 2) % F(1, 3), divmod(F(-7, 2), 2))
print("pow", F(2, 3) ** 2, F(2, 3) ** -2, F(4, 9) ** F(1, 2), 2 ** F(2))
print("float mix", F(1, 2) + 0.25, type(F(1, 2) + 0.25).__name__)
print("complex mix", F(1, 2) + 1j)
print("neg/abs", -F(1, 3), abs(F(-5, 7)), +F(2, 4))
print("compare", F(1, 3) < F(1, 2), F(1, 2) == 0.5, F(1, 3) == 1 / 3, F(2, 4) == F(1, 2))
print("sorted", sorted([F(1, 2), F(1, 3), 0.4, 1]))
print("bool", bool(F(0)), bool(F(1, 100)))
print("int/float", int(F(-7, 2)), float(F(1, 3)), math.floor(F(-7, 2)), math.ceil(F(7, 2)))
print("round", round(F(5, 2)), round(F(7, 2)), round(F(1, 3), 2), round(F(-5, 2)))
print("is_integer", F(4, 2).is_integer(), F(1, 2).is_integer())

pi = F(math.pi)
print("pi exact", pi)
for d in (1, 10, 100, 1000, 10**6):
    print("limit_denominator", d, pi.limit_denominator(d))
print("limit 0.1", F(0.1).limit_denominator())
print("limit already small", F(3, 7).limit_denominator(100))
try:
    F(1, 3).limit_denominator(0)
except ValueError as e:
    print("limit 0", type(e).__name__, e)

for bad in [lambda: F(1, 0), lambda: F("1/0"), lambda: F("abc"), lambda: F(float("nan")), lambda: F(float("inf")),
            lambda: F(1.5, 2), lambda: F(1, 2) / 0]:
    try:
        bad()
    except (ZeroDivisionError, ValueError, OverflowError, TypeError) as e:
        print("error", type(e).__name__, e)

print("format", f"{F(1, 3):.5f}", f"{F(22, 7):.3e}", f"{F(1, 8):%}", format(F(3, 4), ""))
print("format align", f"{F(3, 4):>8}|", f"{F(-1, 3):+.2f}")
print("str/repr", str(F(1, 2)), repr(F(1, 2)), str(F(4, 2)))
print("hash", hash(F(1, 2)) == hash(0.5), hash(F(3)) == hash(3))
print("sum", sum(F(1, n) for n in range(1, 11)))
print("harmonic float", float(sum(F(1, n) for n in range(1, 11))))
print("big", F(10**30 + 1, 10**30), F(2**100, 6**50))
print("limit large", F(10**30 + 1, 10**30).limit_denominator(1000))
print("from_number", F.from_number(3), F.from_number(0.75))
