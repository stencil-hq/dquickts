# Float and complex literals: forms, exponents, underscores, special values, float() parsing.

print("plain", 1.0, 1., .5, 0.0, -0.0, 10.25)
print("exponent", 1e3, 1E3, 1e-3, 1.5e+2, 2.5E-4, 0e0, 1e0)
print("underscores", 1_000.5, 1_0.0_1, 1e1_0, 0.000_001)
print("large", 1e308, 1e309, -1e309, 1.7976931348623157e308)
print("small", 1e-320, 5e-324, 1e-400, 2.2250738585072014e-308)
print("repr", repr(0.1), repr(1e16), repr(1e-5), repr(123.0), repr(1e22), repr(1e21))
print("integral", 3.0 == 3, float(10**15), float(10**16), 1e15, 1e16)
print("leading zeros ok", 00.5, 007.25, 0e5, 09.0)
print("type", type(1.0).__name__, type(1e3).__name__, type(.5).__name__)

# complex
print("imag", 1j, 2J, 0j, 1.5j, 1e3j, 1_0j, -1j)
print("complex sum", 1 + 2j, 1.5 - 0.5j, -1 - 1j, (1 + 2j) * 1j)
print("complex parts", (3 + 4j).real, (3 + 4j).imag, (3 + 4j).conjugate(), abs(3 + 4j))
print("complex repr", repr(1 + 0j), repr(0 + 1j), repr(-0j), repr(complex(0, -0.0)), repr(complex(-0.0, 0)))
print("complex type", type(1j).__name__, type(1j.imag).__name__)
print("complex eq", 1 + 0j == 1, 1j * 1j == -1, (1j) ** 2)
print("complex special", complex("inf"), complex("nan+nanj"), complex(1e309, -1e309))

def parse(label, fn, arg):
    try:
        print(label, repr(arg), repr(fn(arg)))
    except (ValueError, TypeError) as e:
        print(label, repr(arg), type(e).__name__, e)

for s in ["1.5", " -2.5e3 ", "+.5", "1_000.25", "inf", "-Infinity", "nan", "NaN", "-nan",
          "1e500", "-1e500", "1e-500", "0x10", "1.5.2", "", "1__0", "1_", "infinity", " iNF ",
          "1e", "e5", ".", "\u0661.\u0662", "١٢٣"]:
    parse("float", float, s)

for v in [1, -1, 10**20, True, None, "x", b"3.5", [1]]:
    parse("float()", float, v)

try:
    float(10**400)
except OverflowError as e:
    print("float big int", type(e).__name__, e)

for s in ["1+2j", "1-2J", "j", "-j", "2.5j", " (1+1j) ", "1+j", "1 + 2j", "", "1+2", "inf+nanj", "j1"]:
    parse("complex", complex, s)

print("complex ctor", complex(1, 2), complex(), complex(2), complex(1j), complex(real=3, imag=4))

# float methods and attributes
print("is_integer", (3.0).is_integer(), (3.5).is_integer())
print("as_integer_ratio", (0.75).as_integer_ratio(), (0.1).as_integer_ratio())
print("hex", (1.0).hex(), (-0.1).hex(), (0.0).hex(), float.fromhex("0x1.8p1"), float.fromhex("-0x1p-2"))
print("float int conversion", int(2.9), int(-2.9), round(2.5), round(3.5), round(-2.5), round(2.675, 2))
print("round ndigits", round(1234.5678, -2), round(1234.5678, 1), round(0.5), type(round(0.5)).__name__)

# float literal syntax errors
for src in ["1.e", "1e+", "1_.5", "1._5", "1e_5", "1.5_", "0x1.5", "1__0.5", "1.5jj"]:
    try:
        compile(src, "<lit>", "eval")
        print("compiles", repr(src), eval(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)
    except Exception as e:
        print("runtime", repr(src), type(e).__name__, e)

# attribute access on numeric literals
print("literal attr", 1.0.real, 1 .real, (1).imag, 1.5.hex(), 0j.imag, 1e0.is_integer())

# nan and inf behaviour
inf = float("inf")
nan = float("nan")
print("inf arith", inf + 1, inf - inf, inf * 0, -inf * -1, 1 / inf)
print("nan arith", nan + 1, nan * 0, nan == nan, nan is nan, [nan] == [nan])
print("special repr", repr(inf), repr(-inf), repr(nan), str(-nan))
