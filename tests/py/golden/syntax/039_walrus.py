# Assignment expressions (:=) in if, while, calls, and their scope rules.

data = [5, 12, 7, 30, 2]
if (n := len(data)) > 3:
    print("if walrus", n)
print("walrus visible after if", n)

if (m := 0):
    print("never")
else:
    print("falsy walrus", m)

# while loop reading chunks
source = iter(["alpha", "beta", "", "gamma"])
read = []
while (line := next(source)):
    read.append(line)
print("while walrus", read, repr(line))

# while with walrus and comparison
stack = [1, 2, 3, 4]
popped = []
while (top := stack.pop()) != 2:
    popped.append(top)
print("pop until 2", popped, stack, top)

# walrus in elif
def describe(x):
    if (r := x % 3) == 0:
        return "div3"
    elif (q := x // 3) > 2:
        return "big q=" + str(q) + " r=" + str(r)
    return "small r=" + str(r)

print("elif walrus", describe(9), describe(10), describe(4))

# walrus in function arguments and expressions
print("in call", max((a := 3), 2), a)
print("in arithmetic", (b := 4) * b + b)
print("nested", (c := (d := 2) + 1), c, d)
print("in list display", [e := 1, e + 1, e + 2])
print("in tuple", ((f := "x"), f * 2))
print("in subscript", data[(i := 1)], i)
print("in fstring", f"{(g := 10)} {g * 2}")

# walrus in a ternary condition
val = "yes" if (h := 5) > 3 else "no"
print("ternary walrus", val, h)

# reusing an expensive computation in a comprehension
calls = []
def expensive(x):
    calls.append(x)
    return x * x

res = [y for x in range(5) if (y := expensive(x)) % 2 == 0]
print("compute once", res, calls)

# walrus inside a lambda body binds lambda local
fn = lambda v: (w := v + 1) * w
print("lambda walrus", fn(3))
try:
    w
except NameError as e:
    print("lambda local not leaked", type(e).__name__, e)

# walrus inside a function binds a local
def local_walrus():
    if (z := 42):
        pass
    return z

print("function local", local_walrus())
try:
    z
except NameError as e:
    print("no global z", type(e).__name__, e)

# invalid uses (compile only)
for src in ["x := 1", "(x.y := 1)", "(x[0] := 1)", "(a, b := 1, 2)", "def f(x := 1): pass",
            "f(x := 1)", "f(a=(x := 1))", "f(a=x := 1)", "[(x := 1) for x in y]", "y = x := 1",
            "(lambda: x := 1)", "(lambda: (x := 1))", "with (x := open) as y: pass"]:
    try:
        compile(src, "<w>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# tuple unpacking is not allowed directly; walrus binds the tuple element
t = (p := 1, 2)
print("walrus in tuple binds first only", t, p)

# walrus at module level with parentheses is a statement expression
(top_level := "ok")
print("statement walrus", top_level)

# walrus in while with counter
count = 0
while (count := count + 1) < 5:
    pass
print("counter", count)

# walrus in assert and return
def check(v):
    assert (ok := v > 0), "not positive"
    return (doubled := v * 2), ok, doubled

print("assert/return walrus", check(3))
try:
    check(-1)
except AssertionError as e:
    print("assert walrus fail", type(e).__name__, e)
