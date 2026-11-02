# for loop targets: tuple, nested, starred, list targets, and target errors.

for a, b in [(1, 2), (3, 4)]:
    print("tuple target", a, b)
print("after loop", a, b)

for (a, b) in [("x", "y")]:
    print("paren tuple target", a, b)

for [a, b] in [[5, 6]]:
    print("list target", a, b)

for a, (b, c) in [(1, (2, 3)), (4, (5, 6))]:
    print("nested target", a, b, c)

for (a, b), (c, d) in [((1, 2), (3, 4))]:
    print("two nested", a, b, c, d)

for a, *rest in [(1, 2, 3), (4,), (5, 6)]:
    print("starred tail", a, rest)

for *init, last in ["abc", "z"]:
    print("starred head", init, last)

for first, *mid, last in [range(5), (1, 2)]:
    print("starred middle", first, mid, last)

for a, (b, *c) in [(1, "xyz")]:
    print("nested starred", a, b, c)

for k, v in {"a": 1, "b": 2}.items():
    print("items", k, v)

for i, (k, v) in enumerate({"p": 10, "q": 20}.items()):
    print("enumerate items", i, k, v)

for x, y in zip("ab", "cd"):
    print("zip strings", x, y)

for c1, c2 in ["hi", "yo"]:
    print("unpack 2-char strings", c1, c2)

# single-element tuple target
for (x,) in [(1,), (2,)]:
    print("single tuple target", x)

for x, in [[9]]:
    print("trailing comma target", x)

# unpack errors inside loops
try:
    for a, b in [(1, 2), (1, 2, 3)]:
        print("ok pair", a, b)
except ValueError as e:
    print("too many", type(e).__name__, e)

try:
    for a, b in [(1,)]:
        pass
except ValueError as e:
    print("not enough", type(e).__name__, e)

try:
    for a, b in [5]:
        pass
except TypeError as e:
    print("non-iterable element", type(e).__name__, e)

try:
    for a, *b, c in ["x"]:
        pass
except ValueError as e:
    print("starred not enough", type(e).__name__, e)

# targets that are subscripts with tuple
d = {}
for d["k"], d["v"] in [(1, 2), (3, 4)]:
    pass
print("subscript tuple target", d)

lst = [None, None, None]
for i, lst[i] in [(0, "a"), (2, "c")]:
    pass
print("index uses freshly bound i", lst)

# loop variable from enclosing scope is overwritten
x = "outer"
for x in range(2):
    pass
print("overwritten outer", x)

# nested loops sharing a variable name
out = []
for i in range(2):
    for i in range(3, 5):
        out.append(i)
    out.append(("outer-after-inner", i))
print("shared name", out)

# iterate pairs of a dict sorted by value
scores = {"bob": 3, "amy": 5, "cid": 1}
for name, score in sorted(scores.items(), key=lambda kv: kv[1]):
    print("by score", name, score)

# deep nesting
for ((a, b), c), d in [(((1, 2), 3), 4)]:
    print("deep nest", a, b, c, d)

# starred with nothing to capture
for *s, in [(1, 2), ()]:
    print("only starred", s)
