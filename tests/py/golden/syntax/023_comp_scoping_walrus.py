# Comprehension scoping: loop variables do not leak; walrus binds in enclosing scope.

x = "outer x"
r = [x for x in range(3)]
print("no leak list", r, repr(x))
r = {x for x in range(3)}
print("no leak set", repr(x))
r = {x: x for x in range(3)}
print("no leak dict", repr(x))
r = list(x for x in range(3))
print("no leak genexpr", repr(x))

try:
    [leak_check for leak_check in range(2)]
    print(leak_check)
except NameError as e:
    print("loop var undefined after", type(e).__name__, e)

# a regular for loop does leak
for leaked in range(2):
    pass
print("for loop leaks", leaked)

# walrus inside comprehension binds in containing scope
data = [3, 8, 1, 9, 4]
big = [y for v in data if (y := v * 2) > 6]
print("walrus filter", big, "last y", y)

total = 0
running = [total := total + v for v in data]
print("running total", running, total)

# walrus captures the last matching item (any short-circuits)
if any((hit := v) > 5 for v in data):
    print("first > 5", hit)

# walrus in a function binds a function local
def f(seq):
    out = [last := s for s in seq]
    return out, last

print("walrus in function", f("abc"))

# walrus in nested comprehension binds outermost function scope
def g():
    res = [[(z := a * b) for b in range(3)] for a in range(3)]
    return res, z

print("nested walrus", g())

# walrus cannot rebind the comprehension iteration variable
for src in ["[i := 0 for i in range(3)]",
            "[[(j := 1) for j in range(2)] for i in range(2)]",
            "[x for x in (y := [1, 2])]",
            "class C: [(q := 1) for _ in range(1)]"]:
    try:
        compile(src, "<t>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax error", repr(src), e.msg)

# closures inside comprehensions capture per-iteration? No: late binding
fns = [lambda: i for i in range(3)]
print("late-bound lambdas", [fn() for fn in fns])
fns = [lambda i=i: i for i in range(3)]
print("default-bound lambdas", [fn() for fn in fns])

# comprehension sees enclosing function locals
def outer():
    base = 100
    return [base + i for i in range(3)]

print("enclosing locals", outer())

# comprehension in a nested function sees its free variables
def make_adder_list(n):
    def build():
        return [n + k for k in range(2)]
    return build()

print("free var", make_adder_list(5))

# global statement and comprehension
counter = 0
def bump_all(items):
    global counter
    return [(counter := counter + 1) for _ in items]

print("global walrus", bump_all("abc"), counter)

# iteration variable shadows outer only inside
name = "kept"
print("shadow", [name * 2 for name in ["a", "b"]], name)

# the outermost iterable is evaluated in the enclosing scope
class C:
    items = [1, 2, 3]
    doubled = [v * 2 for v in items]
    first = items[0]

print("class outermost iterable", C.doubled)

# comprehension with same name as builtin does not shadow it outside
r = [len for len in range(2)]
print("builtin intact", len("abc"), r)

# walrus at statement level in comprehension condition with side effects
log = []
vals = [w for v in range(5) if (w := (log.append(v) or v * v)) > 3]
print("walrus side effects", vals, log, w)
