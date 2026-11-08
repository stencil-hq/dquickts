# Format specs on floats: precision, width, exponent, general, percent, special values.

values = [0.0, -0.0, 1.0, -1.5, 3.14159, -2.71828, 0.5, 2.5, 1234.5678, -0.000123, 1e16, 1.5e-7]
specs = [".0f", ".1f", ".2f", ".3f", ".4f", "5.1f", "6.2f", "e", "E", ".2e", "g", "G",
         ".3g", "%", ".1%", ",.2f", "_.1f", "+.1f", " .1f", "08.2f", "<9.1f", "^9.1f", "*>9.1f"]

for v in values:
    print("float", repr(v), " ".join("[" + format(v, s) + "]" for s in specs))

# no spec and empty spec
for v in [0.1, 1e16, 1e-5, 123456789012345678.0, 1.0, 1e22, 1.23e-10]:
    print("repr vs format", repr(v), format(v), format(v, ""), str(v), f"{v:g}", f"{v:.12g}")

# rounding: round-half-even on exact binary values
for v in [0.5, 1.5, 2.5, 0.125, 0.375, 2.675, 1.005, 0.045]:
    print("round fmt", repr(v), f"{v:.0f}", f"{v:.2f}", f"{v:.1f}")

# special values
inf = float("inf")
nan = float("nan")
for v in [inf, -inf, nan]:
    print("special", f"[{v}] [{v:f}] [{v:F}] [{v:e}] [{v:E}] [{v:g}] [{v:8.2f}] [{v:+}] [{v:08}] [{v:%}]")

# negative zero and z option
print("neg zero", f"[{-0.0}] [{-0.0:.1f}] [{-0.0:z.1f}] [{-0.001:.1f}] [{-0.001:z.1f}] [{0.0:+.1f}]")

# alternate form keeps the decimal point
print("alt form", f"[{1.0:#.0f}] [{1.0:.0f}] [{1.0:#g}] [{100.0:#.3g}] [{1.0:#e}]")

# general format switching between fixed and exponent
for exp in range(-6, 18, 3):
    v = 1.5 * 10.0 ** exp
    print("g switch", exp, f"{v:g}", f"{v:.3g}", f"{v:G}")

# large precision
print("precision 20", f"{0.1:.20f}", f"{1 / 3:.17g}", f"{2 / 3:.30f}")
print("tiny", f"{5e-324:e}", f"{5e-324:.3g}", f"{2.2250738585072014e-308:.3e}")
print("huge", f"{1.7976931348623157e308:e}", f"{1e300:,.0f}"[:20])

# int-only presentation types are errors for float
def try_fmt(v, spec):
    try:
        print("fmt", repr(v), repr(spec), format(v, spec))
    except ValueError as e:
        print("fmt", repr(v), repr(spec), type(e).__name__, e)

for spec in ["d", "x", "b", "c", "o", "s", ".2d", ",_f", "=", "z"]:
    try_fmt(1.5, spec)

# float subclass
class F(float):
    pass

print("subclass", f"{F(2.5):.2f}", format(F(-1.25), "08.3f"))

# percent of values
print("percent", f"{0.25:%}", f"{0.25:.0%}", f"{1:.2%}", f"{-0.5:+.1%}", f"{1 / 3:.3%}")

# complex formatting
z = complex(1.5, -2.25)
print("complex", f"{z}", f"{z:.1f}", f"{z:>20}", f"{z:e}", f"{1j}", f"{complex(0, 0)}", f"{-1j:g}")
print("complex repr", repr(complex(-0.0, 0.0)), repr(complex(1, float("inf"))), repr(complex(0, -0.0)))

# repr of floats is shortest round-trip
for v in [0.1 + 0.2, 1 / 3, 2 / 3, 100.0, 1e15, 1e16, 1.5e-5, 0.0001, 123e-7, 9007199254740993.0]:
    print("repr", repr(v), str(v))
