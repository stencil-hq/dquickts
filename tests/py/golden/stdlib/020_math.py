"""L0 golden: math module functions and their error paths."""
import math

print("consts", math.pi, math.e, math.tau, math.inf, -math.inf, math.nan)
print("floor", math.floor(2.7), math.floor(-2.1), math.floor(5), type(math.floor(2.5)).__name__)
print("ceil", math.ceil(2.1), math.ceil(-2.7), math.ceil(10**20 + 0.5))
print("trunc", math.trunc(2.9), math.trunc(-2.9))
print("floor big float", math.floor(1e20))
for f in (math.floor, math.ceil, math.trunc):
    for bad in (math.inf, math.nan):
        try:
            f(bad)
        except (OverflowError, ValueError) as e:
            print(f.__name__, bad, type(e).__name__, e)

print("isclose", math.isclose(0.1 + 0.2, 0.3), math.isclose(1, 1.1), math.isclose(1, 1.1, rel_tol=0.1))
print("isclose abs", math.isclose(0, 1e-10), math.isclose(0, 1e-10, abs_tol=1e-9))
print("isclose inf/nan", math.isclose(math.inf, math.inf), math.isclose(math.nan, math.nan))
print("gcd", math.gcd(12, 18), math.gcd(0, 0), math.gcd(-4, 6), math.gcd(), math.gcd(12, 18, 27))
print("lcm", math.lcm(4, 6), math.lcm(3, 5, 7), math.lcm(), math.lcm(0, 5))
print("factorial 50", math.factorial(50))
print("factorial 0", math.factorial(0))
for bad in (-1, 2.0, 2.5):
    try:
        math.factorial(bad)
    except (ValueError, TypeError) as e:
        print("factorial", bad, type(e).__name__, e)
print("comb", math.comb(10, 3), math.comb(5, 0), math.comb(5, 6), math.comb(100, 50))
print("perm", math.perm(5, 2), math.perm(5), math.perm(5, 0), math.perm(3, 5))
try:
    math.comb(-1, 2)
except ValueError as e:
    print("comb neg", type(e).__name__, e)

print("sqrt", math.sqrt(16), math.sqrt(2), math.sqrt(0.0), math.sqrt(-0.0))
print("isqrt", math.isqrt(15), math.isqrt(16), math.isqrt(10**40 + 1))
for bad in (lambda: math.sqrt(-1), lambda: math.isqrt(-1), lambda: math.log(0), lambda: math.log(-1),
            lambda: math.log10(0), lambda: math.acos(2), lambda: math.exp(1000), lambda: math.pow(0, -1)):
    try:
        bad()
    except (ValueError, OverflowError, ZeroDivisionError) as e:
        print("domain", type(e).__name__, e)

print("log", math.log(math.e), math.log(8, 2), math.log(100, 10), math.log(1))
print("log big int", math.log(10**400), math.log2(2**1000), math.log10(10**500))
print("log2/log10", math.log2(1024), math.log10(0.001), math.log1p(0.0), math.expm1(0.0))
print("exp", math.exp(0), math.exp(1), math.exp(-math.inf), math.exp2(10))
print("pow", math.pow(2, 10), math.pow(2, 0.5), math.pow(-8, 3), 2 ** -1)
print("nan", math.isnan(math.nan), math.isnan(1.0), math.nan == math.nan)
print("inf", math.isinf(-math.inf), math.isfinite(1e308), math.isfinite(math.inf))
print("fsum", math.fsum([0.1] * 10), sum([0.1] * 10))
print("fsum cancel", math.fsum([1e100, 1.0, -1e100, 1e-100]))
print("prod", math.prod([1, 2, 3, 4]), math.prod([]), math.prod([2, 0.5]), math.prod(range(1, 6), start=10))
print("dist", math.dist((0, 0), (3, 4)), math.dist([1, 1, 1], [2, 2, 2]))
try:
    math.dist((0, 0), (1, 2, 3))
except ValueError as e:
    print("dist mismatch", type(e).__name__, e)
print("hypot", math.hypot(3, 4), math.hypot(), math.hypot(1, 2, 2), math.hypot(-5))
print("copysign", math.copysign(3, -0.0), math.copysign(-2, 1), math.copysign(1, math.nan) in (1.0, -1.0))
print("fabs", math.fabs(-3), math.fmod(7, -3), 7 % -3, math.remainder(7, 3))
print("modf", math.modf(3.25), math.modf(-3.25))
print("frexp/ldexp", math.frexp(8.0), math.ldexp(0.5, 4))
print("trig", math.sin(0), math.cos(0), math.tan(0), round(math.sin(math.pi / 2), 12))
print("atan2", math.atan2(1, 1) == math.pi / 4, math.atan2(0.0, -1.0), math.atan2(-0.0, -1.0))
print("degrees", math.degrees(math.pi), math.radians(180))
print("hyperbolic", math.sinh(0), math.cosh(0), math.tanh(math.inf))
print("nextafter", math.nextafter(1.0, 2.0), math.nextafter(0.0, -1.0))
print("ulp", math.ulp(1.0), math.ulp(0.0))
print("sumprod", math.sumprod([1, 2, 3], [4, 5, 6]))
print("cbrt", math.cbrt(27), math.cbrt(-8))
print("fma", math.fma(2.0, 3.0, 1.0))
print("erf", math.erf(0), round(math.erfc(0), 12), math.gamma(5), math.lgamma(1))
print("int ops", math.floor(-0.5), round(2.5), round(3.5), round(-2.5), round(2.675, 2))
print("float info", (0.1).as_integer_ratio(), (2.5).is_integer(), float.fromhex("0x1.8p1"))
try:
    math.sqrt("4")
except TypeError as e:
    print("type error", type(e).__name__, e)
print("big int float conv", float(2**1023) * 2)
try:
    float(2**1024)
except OverflowError as e:
    print("int too large", type(e).__name__, e)
