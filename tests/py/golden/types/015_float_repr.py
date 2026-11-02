"""L0 golden: float repr/str use the shortest round-tripping representation.

Covers the switch to exponent notation (>= 1e16, < 1e-4), trailing '.0',
classic rounding artefacts (0.1 + 0.2), subnormals and extreme values.

Usage: python3.15 -I -X utf8 tests/py/golden/types/015_float_repr.py
"""

CASES = [
    0.1, 0.2, 0.1 + 0.2, 0.3, 1.1 * 3, 1 / 3, 2 / 3, 1.0, 2.0, 100.0, -1.0, 0.0, -0.0,
    1.5, 0.5, 0.25, 0.125, 3.14159, 2.718281828459045, 1e1, 1e5, 1e15, 9999999999999998.0,
    1e16, 1e16 + 2, 1e17, 1e22, 1e23, 1e+16, 123456789012345678.0, 12345678901234567.0,
    1234567890123456.0, 1e-4, 1e-5, 0.0001, 0.00001, 0.00012345, 1.2345e-5, 1e-7, 1e-300,
    5e-324, 1e-323, 2.2250738585072014e-308, 2.225073858507201e-308, 1.7976931348623157e308,
    4.35, 0.07, 1.005, 2.675, 9.995, 100.5, 1e100, 1.5e300, -2.5e-10, 123.456, 1e21, 1e20,
    2.0 ** 53, 2.0 ** 53 + 2, 2.0 ** 63, 2.0 ** 64, 0.1 * 3, 0.7 + 0.1, 0.8 - 0.1,
]

for x in CASES:
    print("repr", repr(x), "str", str(x))

print("float(1)", repr(float(1)))
print("float(-0)", repr(float(-0)))
print("float(10**16)", repr(float(10 ** 16)))
print("float(10**15)", repr(float(10 ** 15)))
print("float(2**70)", repr(float(2 ** 70)))
print("3 * 1.1", 3 * 1.1)
print("sum of tenths", sum([0.1] * 10))
print("0.1 + 0.7", 0.1 + 0.7)
print("1 - 0.9", 1 - 0.9)
print("1e16 + 1", 1e16 + 1)
print("1e16 - 1", 1e16 - 1)
print("99999999999999999.0", 99999999999999999.0)
print("0.1 ** 2", 0.1 ** 2)
print("1 / 7", 1 / 7)
print("22 / 7", 22 / 7)
print("1e308 * 10", 1e308 * 10)
print("-1e308 * 10", -1e308 * 10)
print("5e-324 / 2", 5e-324 / 2)
print("-5e-324 / 2", -5e-324 / 2)
print("in list", [0.1, 1.0, 1e16, -0.0])
print("in tuple", (0.5,))
print("in dict", {1.5: 2.0})
print("f-string default", f"{0.1 + 0.2}", f"{1e16}", f"{1.0}")
print("str.format default", "{}".format(1e-5))
print("%s and %r", "%s %r" % (0.1, 0.1))
print("roundtrip all", all(float(repr(x)) == x for x in CASES))
print("literal forms", 1., .5, 1_000.000_1, 0e0, 1E3, 1e-0, 0x10 * 1.0)
print("repr type", type(repr(1.0)).__name__)
print("eval-free roundtrip", float("0.30000000000000004") == 0.1 + 0.2)
print("neg", -0.1, -1e16, -1e-5, -123456789012345678.0)
