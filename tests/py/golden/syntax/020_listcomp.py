# List comprehensions: filters, multiple for clauses, nesting, targets.

print("squares", [x * x for x in range(6)])
print("filter", [x for x in range(20) if x % 3 == 0])
print("two filters", [x for x in range(30) if x % 2 if x % 3])
print("filter and", [x for x in range(30) if x % 2 and x % 5 == 0])
print("map str", [str(x) for x in [1, 2.5, None]])
print("from string", [c.upper() for c in "abc"])
print("ternary element", [x if x > 0 else 0 for x in [-2, -1, 0, 1, 2]])
print("empty source", [x for x in []])
print("all filtered", [x for x in range(5) if x > 10])
print("constant element", [0 for _ in range(4)])
print("tuple element", [(x, x * x) for x in range(3)])

# multiple for clauses: leftmost is outermost
print("product", [(a, b) for a in "ab" for b in range(2)])
print("dependent inner", [(a, b) for a in range(4) for b in range(a)])
print("filter between fors", [(a, b) for a in range(4) if a % 2 for b in range(a) if b])
print("flatten", [x for row in [[1, 2], [3], []] for x in row])
print("three fors", [a + b + c for a in "x" for b in "yz" for c in "12"])

# nested comprehension
print("matrix", [[r * 3 + c for c in range(3)] for r in range(3)])
m = [[1, 2, 3], [4, 5, 6]]
print("transpose", [[row[i] for row in m] for i in range(3)])
print("identity", [[1 if i == j else 0 for j in range(3)] for i in range(3)])

# tuple and nested targets
pairs = [(1, "one"), (2, "two")]
print("tuple target", [name for num, name in pairs])
print("nested target", [a + b + c for a, (b, c) in [(1, (2, 3)), (4, (5, 6))]])
print("starred target", [rest for first, *rest in ["abc", "de"]])
print("enumerate", [i * v for i, v in enumerate([5, 6, 7])])
print("zip", [a * b for a, b in zip([1, 2, 3], [4, 5, 6])])
print("dict items", [k + "=" + str(v) for k, v in {"x": 1, "y": 2}.items()])

# comprehension result is a new list each time
def make():
    return [i for i in range(2)]

print("fresh list", make() == make(), make() is not make())

# using outer variables
factor = 10
print("outer var", [x * factor for x in range(3)])

# function calls in comprehension
def sq(x):
    return x * x

print("call", [sq(x) for x in range(4)])
print("method", [s.strip() for s in [" a ", "b  ", "  c"]])

# exceptions inside comprehensions propagate
try:
    print([1 / x for x in [1, 0, 2]])
except ZeroDivisionError as e:
    print("exception", type(e).__name__, e)

try:
    print([x for x in 42])
except TypeError as e:
    print("non-iterable source", type(e).__name__, e)

try:
    print([undefined_name for x in range(1)])
except NameError as e:
    print("name error inside", type(e).__name__, e)

# the outermost iterable is evaluated once, before the loop
log = []
def src():
    log.append("src")
    return range(3)

r = [(log.append("body"), x)[1] for x in src()]
print("outermost once", r, log)

# inner iterables are evaluated for each outer item
log.clear()
def inner(a):
    log.append(a)
    return range(a)

r = [b for a in range(3) for b in inner(a)]
print("inner each time", r, log)

# comprehensions in class bodies cannot see class variables in inner loops
class K:
    vals = [1, 2, 3]
    doubled = [v * 2 for v in vals]
    try:
        cross = [(a, b) for a in vals for b in vals]
    except NameError as e:
        cross = "NameError: " + str(e)

print("class body", K.doubled, K.cross)

# big comprehension
print("sum of comp", sum([x for x in range(1000) if x % 7 == 0]))
print("len of nested", len([(a, b) for a in range(30) for b in range(30) if a < b]))
