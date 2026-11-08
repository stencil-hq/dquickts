# lambda expressions: defaults, closures, late vs early binding, usage as key functions.

sq = lambda x: x * x
print("simple", sq(5), sq(-1.5))
print("no args", (lambda: "const")())
print("multiple args", (lambda a, b, c: a + b * c)(1, 2, 3))
print("defaults", (lambda a, b=10: a + b)(1), (lambda a, b=10: a + b)(1, 2))
print("keyword call", (lambda a, b: a - b)(b=1, a=10))
print("varargs", (lambda *args: args)(1, 2, 3), (lambda *args: args)())
print("kwargs", (lambda **kw: sorted(kw.items()))(z=1, a=2))
print("kw-only", (lambda *, k=5: k)(), (lambda *, k=5: k)(k=6))
print("pos-only", (lambda a, /, b: (a, b))(1, b=2))
print("all kinds", (lambda a, /, b, *c, d, **e: (a, b, c, d, e))(1, 2, 3, 4, d=5, f=6))
print("nested lambda", (lambda x: lambda y: x + y)(10)(5))
print("immediately invoked ternary", (lambda n: "even" if n % 2 == 0 else "odd")(7))
print("returns tuple", (lambda: (1, 2))())
print("name", sq.__name__, (lambda: 0).__name__)
print("type", type(sq).__name__)

# late binding in closures created in a loop
fns = []
for i in range(3):
    fns.append(lambda: i)
print("late binding", [f() for f in fns])
i = 100
print("late binding sees rebinding", [f() for f in fns])

# early binding with default arguments
fns = []
for i in range(3):
    fns.append(lambda i=i: i)
print("default binding", [f() for f in fns])
print("default overridable", fns[0](99))

# closure factory binds per call
def make(n):
    return lambda x: x * n

doublers = [make(n) for n in range(1, 4)]
print("factory binding", [f(10) for f in doublers])

# defaults evaluated once at definition time
counter = [0]
def next_id():
    counter[0] += 1
    return counter[0]

f = lambda x=next_id(): x
print("default evaluated once", f(), f(), counter)

# mutable default shared across calls
g = lambda item, acc=[]: (acc.append(item), acc)[1]
print("mutable default", g(1), g(2))

# lambdas as key functions
words = ["banana", "Apple", "cherry", "date"]
print("sort key", sorted(words, key=lambda w: w.lower()))
print("sort len then alpha", sorted(words, key=lambda w: (len(w), w)))
print("max key", max(words, key=lambda w: w[-1]))
print("map", list(map(lambda x: x ** 2, range(5))))
print("filter", list(filter(lambda x: x % 3 == 0, range(10))))
pairs = [(1, "b"), (2, "a"), (0, "c")]
print("sort by second", sorted(pairs, key=lambda p: p[1]))

# lambda capturing an enclosing variable that changes
def outer():
    v = 1
    get = lambda: v
    v = 2
    return get()

print("captures variable not value", outer())

# lambda bodies are single expressions
for src in ["lambda: return 1", "lambda: x = 1", "lambda: (yield)", "lambda x, x: 0",
            "lambda *: 0", "lambda x=1, y: 0", "lambda: pass"]:
    try:
        compile(src, "<l>", "eval")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# errors when calling lambdas
def attempt(label, fn):
    try:
        print(label, fn())
    except TypeError as e:
        print(label, type(e).__name__, e)

attempt("missing arg", lambda: (lambda a: a)())
attempt("too many args", lambda: (lambda a: a)(1, 2))
attempt("unexpected kw", lambda: (lambda a: a)(b=1))
attempt("pos-only as kw", lambda: (lambda a, /: a)(a=1))
attempt("kw-only missing", lambda: (lambda *, k: k)())

# recursion through a name
fact = lambda n: 1 if n <= 1 else n * fact(n - 1)
print("recursive lambda", fact(10))

# lambda in dict dispatch
ops = {"+": lambda a, b: a + b, "-": lambda a, b: a - b, "*": lambda a, b: a * b}
print("dispatch", [ops[o](6, 3) for o in "+-*"])
