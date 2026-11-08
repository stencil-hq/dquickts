# Format specs on ints: width, zero padding, grouping, bases, sign, alignment.

values = [0, 7, -7, 42, -42, 1234, -1234, 1234567, 255, 10**20, -(2**64)]
specs = ["", "d", "2d", "3d", "4d", "5d", "02d", "05d", "+d", " d", "-d", ",", "_",
         ",d", "_d", "10,", "010,", "x", "X", "#x", "#X", "o", "#o", "b", "#b",
         "_b", "_x", "#010x", ">8", "<5", "^9", "*^9", "=+8", "0=8", "e", ".2f", "%"]

for v in values[:5]:
    print("int", v, " ".join("[" + format(v, s) + "]" for s in specs))

for v in values[5:]:
    print("int", v, " ".join("[" + format(v, s) + "]" for s in specs[:20]))

# f-string forms
n = 42
print(f"[{n:2d}] [{n:3d}] [{n:4d}] [{n:5d}] [{n:02d}] [{n:+d}]")
print(f"[{-n:2d}] [{-n:3d}] [{-n:4d}] [{-n:5d}] [{-n:02d}] [{-n:05d}]")
print(f"[{0:2d}] [{0:+d}] [{0:x}] [{0:#x}] [{0:b}] [{0:#o}]")
print(f"[{1000000:,}] [{1000000:_}] [{-1000000:,}] [{999:,}] [{1000:,}]")
print(f"[{255:x}] [{255:X}] [{255:#x}] [{255:#X}] [{255:o}] [{255:#o}] [{255:b}] [{255:#b}]")
print(f"[{-255:x}] [{-255:#x}] [{-255:b}] [{-255:#b}]")
print(f"[{n:>8}] [{n:<5}] [{n:^9}] [{n:*^9}] [{n:-<6}] [{n:0>4}]")
print(f"[{n:c}] [{9731:c}] [{65:c}]")
print(f"[{0xDEADBEEF:_x}] [{0b101010:08b}] [{0o777:#o}]")

# bool formats like int
print(f"[{True:d}] [{False:5}] [{True:#x}] [{True}]")

# ints with float presentation types
print(f"[{n:.0f}] [{n:.1f}] [{n:.2f}] [{n:e}] [{n:g}] [{n:%}] [{n:.1%}]")
print(f"[{10**20:e}] [{10**20:.3g}]")

# big ints
big = 2**100
print(f"[{big:,}] [{big:x}] [{big:_d}] [{big:40}]")
print(f"[{-big:,}]")

# precision is not allowed for integer types
def bad(v, spec):
    try:
        print("format", repr(v), repr(spec), format(v, spec))
    except (ValueError, OverflowError) as e:
        print("format", repr(v), repr(spec), type(e).__name__, e)

bad(5, ".2d")
bad(5, "s")
bad(5, ",x")
bad(5, "_c")
bad(5, "+c")
bad(-1, "c")
bad(2**32, "c")
bad(5, "z")
bad(5, "=")
bad(5, "#c")
bad(5, ",_")
bad(5, "__")

# int subclasses
class MyInt(int):
    pass

print("subclass", format(MyInt(12), "04d"), f"{MyInt(255):#x}")

# __format__ called with empty spec equals str()
print("empty spec", format(-17, "") == str(-17), format(10**30, ""))

# zero padding with sign and alternate forms
print(f"[{-5:05}] [{5:+05}] [{-0x1f:#06x}] [{31:#06b}]")
print(f"[{-5:=6}] [{-5:*=6}] [{5: =6}] [{-5:<6}]")
