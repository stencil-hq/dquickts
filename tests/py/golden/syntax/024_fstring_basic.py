# f-strings: conversions, debug specifier, nested quotes, multi-line, brace escaping.

name = "Ann"
n = 42
s = "it's \"q\" é ✓"
print(f"plain {name} is {n}")
print(f"expr {n + 1} {n * 2} {n // 5} {n % 5} {-n}")
print(f"call {len(name)} {name.upper()} {max(1, 2)}")
print(f"index {name[0]} {[1, 2, 3][-1]} { {'k': 'v'}['k'] }")
print(f"conv r {name!r} s {name!s} a {s!a}")
print(f"conv r on str with quotes {s!r}")
print(f"conv on int {n!r} {n!s} {n!a}")
print(f"conv with spec {name!r:>10}|{name!s:<6}|")
print(f"none {None} {None!r} bool {True}")
print(f"float {1.5} {0.1 + 0.2} {1e100} {-0.0}")
print(f"list {[1, 'a']} tuple {(1,)} dict {({'a': 1})}")

# debug specifier
x = 10
y = "yy"
print(f"{x=}")
print(f"{x = }")
print(f"{x= }|{x =}|")
print(f"{y=}")
print(f"{y=!s}")
print(f"{x=:5d}|")
print(f"{x * 2 + 1=}")
print(f"{name.lower()=}")
print(f"{3.14159=:.2f}")
print(f"{x=!r:>6}|")

# PEP 701: reuse the same quote type inside the replacement field
d = {"key": "value"}
print(f"same quotes {d["key"]}")
print(f'single {d['key']}')
print(f"nested f-string {f"{f"{n}"}"}")
print(f"join {", ".join(["a", "b", "c"])}")
print(f"backslash in expr {"\n".join(["l1", "l2"])}")

# braces escaping
print(f"{{literal}} {{{n}}} {{}}")
print(f"{{ {name} }}")
print(f"}}{{")

# multi-line f-strings
msg = (f"line one {n} "
       f"line two {name}")
print(msg)
tri = f"""triple
quoted {n}
  indented {name!r}"""
print(tri)
expr_multiline = f"{
    n
    + 1
}"
print("multiline expr", expr_multiline)

# comments inside multi-line replacement fields (PEP 701)
val = f"{n  # a comment
    * 2}"
print("comment inside", val)

# lambda and ternary need parentheses
print(f"lambda {(lambda v: v * 2)(4)}")
print(f"ternary {'yes' if n > 40 else 'no'}")
print(f"walrus {(w := 5)} then {w}")

# raw f-strings
print(rf"raw \n {name}")
print(fr"raw2 \t{n}\d")

# format spec from expressions
width = 8
print(f"[{name:>{width}}]")
print(f"[{name:{'^'}{width}}]")

# objects with __format__, __str__, __repr__
class P:
    def __str__(self):
        return "P-str"

    def __repr__(self):
        return "P-repr"

    def __format__(self, spec):
        return "P-format(" + spec + ")"

p = P()
print(f"{p} {p!s} {p!r} {p:xyz} {p!r:>8}|")

# errors
for src in ['f"{}"', 'f"{x!z}"', 'f"{"', 'f"}"', 'f"{x:{y:{z}}}"']:
    try:
        compile(src, "<f>", "eval")
        print("compiles", src)
    except SyntaxError as e:
        print("syntax", src, type(e).__name__, e.msg)

try:
    print(f"{n:s}")
except ValueError as e:
    print("bad spec for int", type(e).__name__, e)

try:
    print(f"{undefined_var}")
except NameError as e:
    print("undefined in f-string", type(e).__name__, e)

# f-string results are str
print("type", type(f"{n}").__name__, f"" == "", repr(f""))
