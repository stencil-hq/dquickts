# Generator expressions: laziness, single consumption, early binding of outer iterable.

log = []
def f(x):
    log.append(x)
    return x * 10

g = (f(x) for x in range(3))
print("created, nothing evaluated", log)
print("type", type(g).__name__)
print("next", next(g), log)
print("next", next(g), log)
print("rest", list(g), log)
print("exhausted", list(g))
try:
    next(g)
except StopIteration as e:
    print("next on exhausted", type(e).__name__, repr(e.value))

# sum/any/all with genexpr and short-circuiting
log.clear()
print("any short-circuit", any(f(x) > 10 for x in range(10)), log)
log.clear()
print("all short-circuit", all(f(x) < 10 for x in range(10)), log)
print("sum genexpr", sum(x * x for x in range(10)))
print("max genexpr", max(len(w) for w in ["a", "abc", "ab"]))
print("min with default", min((x for x in []), default="empty"))
print("join genexpr", ",".join(str(x) for x in range(4)))
print("sorted genexpr", sorted((x % 3, x) for x in range(6)))
print("tuple genexpr", tuple(c for c in "hey"))
print("dict from genexpr", dict((k, len(k)) for k in ["aa", "b"]))

# the outermost iterable is evaluated immediately
log.clear()
def src():
    log.append("src evaluated")
    return [1, 2]

g = (x for x in src())
print("outermost eager", log)
print("values", list(g))

# outermost iterable errors happen at creation time
try:
    g = (x for x in 5)
except TypeError as e:
    print("eager error", type(e).__name__, e)

# inner iterables and conditions are lazy
g = (y for x in [1] for y in undefined_inner)
print("created with undefined inner name")
try:
    next(g)
except NameError as e:
    print("lazy inner error", type(e).__name__, e)

# late binding of names used in the element expression
n = 1
g = (x * n for x in range(3))
n = 100
print("late binding element", list(g))

# early binding of the outer iterable
items = [1, 2, 3]
g = (x for x in items)
items = ["replaced"]
print("early bound iterable", list(g))

# mutating the underlying list is visible
items = [1, 2]
g = (x for x in items)
items.append(3)
print("mutation visible", list(g))

# chained generators (pipeline)
nums = (x for x in range(20))
evens = (x for x in nums if x % 2 == 0)
squares = (x * x for x in evens)
print("pipeline", list(squares))

# infinite source consumed lazily
def naturals():
    n = 0
    while True:
        yield n
        n += 1

firsts = []
for v in (x * x for x in naturals()):
    if v > 50:
        break
    firsts.append(v)
print("lazy infinite", firsts)

# genexpr as sole argument may omit parentheses; otherwise needs them
print("sole arg", list(x for x in "ab"))
print("with other arg", sorted((x for x in [3, 1, 2]), reverse=True))
for src_code in ["f(x for x in y, 1)", "f(1, x for x in y)", "f((x for x in y), 1)"]:
    try:
        compile(src_code, "<t>", "eval")
        print("compiles", src_code)
    except SyntaxError as e:
        print("syntax error", src_code, e.msg)

# exceptions raised inside propagate from next()
g = (1 // x for x in [1, 0])
print("first ok", next(g))
try:
    next(g)
except ZeroDivisionError as e:
    print("lazy exception", type(e).__name__, e)
print("after exception exhausted", list(g))

# generator expression close
g = (x for x in range(5))
next(g)
g.close()
print("closed", list(g))
