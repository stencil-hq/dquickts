# f-strings with nested format specs and dynamic width/precision/fill/align.

x = 3.14159265
w = 10
p = 3
print(f"[{x:{w}.{p}f}]")
print(f"[{x:{w}}]")
print(f"[{x:.{p}}]")
print(f"[{x:<{w}.{p - 1}f}]")
print(f"[{x:>{w + 2}.{p}e}]")
for prec in range(5):
    print("prec", prec, f"[{x:.{prec}f}]")
for width in [1, 5, 9]:
    print("width", width, f"[{x:{width}.2f}]")

fill, align = "*", "^"
print(f"[{'mid':{fill}{align}{w}}]")
for a in "<>^":
    print("align", a, f"[{'ab':{a}6}]", f"[{12:{a}6}]")
for fl in "-0#.":
    print("fill", repr(fl), f"[{7:{fl}>5}]")

# dynamic type code and sign
n = 255
for code in "dxXobc":
    print("code", code, f"[{n:{code}}]")
for sign in "+- ":
    print("sign", repr(sign), f"[{5:{sign}d}]", f"[{-5:{sign}d}]")

# nested spec built from a string
spec = ">8.2f"
print(f"[{x:{spec}}]")
spec2 = "08,d"
print(f"[{1234:{spec2}}]")

# nested specs referencing attributes and items
class Cfg:
    width = 7
    prec = 1

cfg = Cfg()
opts = {"w": 9, "p": 4}
print(f"[{x:{cfg.width}.{cfg.prec}f}]")
print(f"[{x:{opts['w']}.{opts['p']}f}]")

# conversion plus nested spec
s = "hi"
print(f"[{s!r:{w}}]")
print(f"[{s!r:^{w}}]")

# debug specifier with nested spec
print(f"{x=:{w}.{p}f}|")

# table formatting
rows = [("apple", 1.5, 3), ("kiwi", 0.25, 12), ("watermelon", 3.0, 1)]
name_w = max(len(r[0]) for r in rows)
for name, price, qty in rows:
    print(f"|{name:<{name_w}}|{price:>6.2f}|{qty:>3d}|{price * qty:>8.3f}|")

# datetime-free custom __format__ receives the expanded nested spec
class Spec:
    def __format__(self, spec):
        return "<" + spec + ">"

print(f"{Spec():{w}.{p}x}", f"{Spec():}", f"{Spec()}")

# errors in nested specs
try:
    print(f"{x:{'bad'}}")
except ValueError as e:
    print("bad nested spec", type(e).__name__, e)

print("minus sign then width 1", f"[{x:{-1}}]", f"[{-x:{-1}}]")

try:
    print(f"{'s':{w}d}")
except ValueError as e:
    print("d on str", type(e).__name__, e)

# width larger than value, zero width
print(f"[{'abc':{1}}]", f"[{'abc':{2}}]", f"[{'abc':.{1}}]", f"[{'abc':.{0}}]")

# format of format result
inner = f"{x:.2f}"
print(f"[{inner:>8}]", f"[{f'{x:.1f}':*^9}]")
