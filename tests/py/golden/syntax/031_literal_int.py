# Integer literals: decimal, hex, octal, binary, underscores, big values, int() parsing.

print("decimal", 0, 7, 1234567890, -42, +42, 00, 0_0)
print("hex", 0xff, 0XFF, 0xDeadBeef, 0x_ff, 0x0, -0x10)
print("octal", 0o17, 0O777, 0o_7_7, 0o0)
print("binary", 0b1010, 0B11, 0b_1_0, 0b0, -0b1)
print("underscores", 1_000_000, 0x_FF_FF, 0b_1111_0000, 1_2_3)
print("big", 123456789012345678901234567890)
print("big hex", 0xFFFFFFFFFFFFFFFFFFFFFFFF)
print("powers", 2**63, 2**64, -(2**63), 10**30)
print("big repr type", type(10**100).__name__, len(str(10**100)))
print("boundaries", 2**31 - 1, 2**31, 2**32, 2**53 + 1, 2**62)

# conversions to text
n = 255
print("hex()", hex(n), hex(-n), hex(0), hex(2**70))
print("oct()", oct(n), oct(-n), oct(0))
print("bin()", bin(n), bin(-n), bin(0))
print("str/repr", str(-17), repr(10**20))

# int() from strings
def parse(label, *args):
    try:
        print(label, repr(args), int(*args))
    except (ValueError, TypeError, OverflowError) as e:
        print(label, repr(args), type(e).__name__, e)

parse("plain", "42")
parse("spaces", "  -17  ")
parse("plus", "+5")
parse("underscore", "1_000")
parse("hex base 16", "ff", 16)
parse("hex prefix base 16", "0xff", 16)
parse("hex prefix base 0", "0xff", 0)
parse("octal base 0", "0o17", 0)
parse("binary base 0", "0b101", 0)
parse("base 0 leading zero", "010", 0)
parse("base 0 zero", "000", 0)
parse("base 36", "zz", 36)
parse("base 2", "1101", 2)
parse("base 2 invalid", "102", 2)
parse("empty", "")
parse("float str", "1.5")
parse("double underscore", "1__0")
parse("trailing underscore", "10_")
parse("leading underscore", "_10")
parse("base 1", "1", 1)
parse("base 37", "1", 37)
parse("unicode digits", "\u0663\u0664")
parse("fullwidth", "\uff11\uff12")
parse("bytes", b"12")
parse("none", None)
parse("from float", 3.99)
parse("from neg float", -3.99)
parse("from bool", True)
parse("no args")
parse("base without str", 12, 16)
parse("inf", float("inf"))
parse("nan", float("nan"))

# integer literal syntax errors
for src in ["012", "0x", "0b2", "0o8", "1__0", "1_", "0_x1", "0xg", "08"]:
    try:
        compile(src, "<lit>", "eval")
        print("compiles", repr(src), eval(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)
    except Exception as e:
        print("runtime", repr(src), type(e).__name__, e)

# arithmetic on big ints is exact
big = 2**200
print("big arith", big + 1 - big, big // 2**199, big % 7, -big // 3, (-big) % 3)
print("big compare", big > 2**199, big == 2**200, big != big + 1)
print("int bit_length", (0).bit_length(), (255).bit_length(), (-256).bit_length(), big.bit_length())
print("bit_count", (255).bit_count(), (-1).bit_count())
print("to_bytes", (1024).to_bytes(2, "big"), (1024).to_bytes(4, "little"), int.from_bytes(b"\x01\x00", "big"))
print("signed bytes", (-1).to_bytes(2, "big", signed=True), int.from_bytes(b"\xff\xfe", "big", signed=True))
print("as_integer_ratio", (10).as_integer_ratio())
print("int attrs", (5).real, (5).imag, (5).numerator, (5).denominator, (5).conjugate())

# the str-digits limit for huge conversions
try:
    int("1" * 5000)
except ValueError as e:
    print("digit limit", type(e).__name__, str(e)[:60])
