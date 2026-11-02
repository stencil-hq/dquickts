"""L0 golden: decimal arithmetic, quantize, contexts and signals."""
import decimal
from decimal import ROUND_DOWN, ROUND_HALF_EVEN, ROUND_HALF_UP, Decimal, localcontext

print("construct", repr(Decimal("1.10")), repr(Decimal(5)), repr(Decimal("-0")), repr(Decimal()))
print("from float", repr(Decimal(0.1)))
print("from_float", repr(Decimal.from_float(0.5)), repr(Decimal((0, (3, 1, 4), -2))))
print("special", repr(Decimal("NaN")), repr(Decimal("-Infinity")), repr(Decimal("sNaN")))
print("str/sci", str(Decimal("1E+3")), str(Decimal("0.000001")), str(Decimal("0.0000001")), Decimal("123E-10"))
print("add", Decimal("0.1") + Decimal("0.2"), 0.1 + 0.2)
print("sub/mul", Decimal("1.30") - Decimal("1.2"), Decimal("1.5") * Decimal("2.00"))
print("div", Decimal(1) / Decimal(3), Decimal(10) / Decimal(4), Decimal(2) / Decimal("0.5"))
print("floordiv/mod neg", Decimal(-7) // Decimal(2), Decimal(-7) % Decimal(2), -7 // 2, -7 % 2)
print("divmod", divmod(Decimal("7.5"), Decimal(2)))
print("pow", Decimal(2) ** 10, Decimal("1.1") ** 2, Decimal(2) ** Decimal("0.5"))
print("int mix", Decimal("1.5") + 1, 2 * Decimal("0.25"))
try:
    Decimal("1.5") + 1.5
except TypeError as e:
    print("float mix", type(e).__name__, e)
print("compare float", Decimal("0.5") == 0.5, Decimal("0.1") == 0.1, Decimal(1) < 1.5)
print("trailing zeros eq", Decimal("1.0") == Decimal("1.00"), Decimal("1.0") == 1)
print("sqrt/exp/ln", Decimal(2).sqrt(), Decimal(1).exp(), Decimal(10).ln(), Decimal(1000).log10())
print("normalize", Decimal("1.2300").normalize(), Decimal("100").normalize(), Decimal("0.00").normalize())
print("as_tuple", Decimal("-3.140").as_tuple())
print("exponent", Decimal("3.140").as_tuple().exponent, Decimal("1.5").adjusted())
print("as_integer_ratio", Decimal("0.75").as_integer_ratio())
print("int/float", int(Decimal("-3.99")), float(Decimal("2.5")), round(Decimal("2.5")), round(Decimal("3.5")))
print("round ndigits", round(Decimal("2.675"), 2), round(Decimal("1.2345"), 3))
print("predicates", Decimal("NaN").is_nan(), Decimal("-0").is_signed(), Decimal("inf").is_infinite(), Decimal(0).is_zero())

# quantize
price = Decimal("19.995")
print("quantize half even", price.quantize(Decimal("0.01")))
print("quantize half up", price.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))
print("quantize down", price.quantize(Decimal("0.01"), rounding=ROUND_DOWN))
print("quantize int", Decimal("2.5").quantize(Decimal("1")), Decimal("3.5").quantize(Decimal(1)))
print("quantize pad", Decimal("7").quantize(Decimal("0.000")))
try:
    Decimal("1e30").quantize(Decimal("0.01"))
except decimal.InvalidOperation as e:
    print("quantize too big", type(e).__name__)

# contexts
ctx = decimal.getcontext()
print("default prec", ctx.prec, ctx.rounding, ctx.Emax, ctx.Emin)
with localcontext() as lc:
    lc.prec = 5
    print("prec 5", Decimal(1) / Decimal(7), Decimal("123456789") + 0)
    lc.rounding = ROUND_DOWN
    print("prec 5 down", Decimal(2) / Decimal(3))
print("restored", Decimal(1) / Decimal(7))
with localcontext(prec=50):
    print("prec 50", Decimal(2).sqrt())
c3 = decimal.Context(prec=3, rounding=ROUND_HALF_EVEN)
print("context ops", c3.add(Decimal("1.234"), Decimal("1")), c3.divide(Decimal(10), Decimal(3)), c3.create_decimal("3.14159"))
print("plus applies context", c3.plus(Decimal("9.999")))

# signals and traps
for label, fn in [
    ("div by zero", lambda: Decimal(1) / Decimal(0)),
    ("invalid op", lambda: Decimal(0) / Decimal(0)),
    ("conversion", lambda: Decimal("abc")),
    ("sqrt neg", lambda: Decimal(-1).sqrt()),
    ("inf - inf", lambda: Decimal("Infinity") - Decimal("Infinity")),
]:
    try:
        fn()
    except decimal.DecimalException as e:
        print("signal", label, type(e).__name__, isinstance(e, ArithmeticError))
with localcontext() as lc:
    lc.traps[decimal.DivisionByZero] = False
    lc.traps[decimal.InvalidOperation] = False
    print("untrapped", Decimal(1) / Decimal(0), Decimal(-1) / Decimal(0), Decimal(0) / Decimal(0))
    print("flags set", bool(lc.flags[decimal.DivisionByZero]), bool(lc.flags[decimal.InvalidOperation]))
with localcontext(prec=2) as lc:
    lc.clear_flags()
    r = Decimal(1) / Decimal(3)
    print("inexact/rounded", r, bool(lc.flags[decimal.Inexact]), bool(lc.flags[decimal.Rounded]))
    lc.traps[decimal.Inexact] = True
    try:
        Decimal(1) / Decimal(3)
    except decimal.Inexact as e:
        print("trap inexact", type(e).__name__)
print("sum", sum([Decimal("0.1")] * 10), sum([Decimal("0.1")] * 10) == 1)
print("format", f"{Decimal('1234.5678'):.2f}", f"{Decimal('1234.5'):,}", f"{Decimal('0.5'):%}", format(Decimal("1e3"), "f"))
print("hash eq", hash(Decimal("1.0")) == hash(1), hash(Decimal("0.5")) == hash(0.5))
print("sorted", sorted([Decimal("1.1"), Decimal("-2"), Decimal("1.01")]))
