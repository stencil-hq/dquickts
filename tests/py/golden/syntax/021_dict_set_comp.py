# Dict and set comprehensions.

print("squares dict", {x: x * x for x in range(5)})
print("filtered dict", {x: str(x) for x in range(10) if x % 4 == 0})
print("invert", {v: k for k, v in {"a": 1, "b": 2, "c": 3}.items()})
print("from zip", {k: v for k, v in zip("xyz", range(3))})
print("dup keys last wins", {x % 3: x for x in range(10)})
print("key order is insertion", list({c: None for c in "hello"}))
print("nested value", {i: [j for j in range(i)] for i in range(4)})
print("dict of dicts", {a: {b: a * b for b in range(2)} for a in range(3)})
print("tuple keys", {(a, b): a + b for a in range(2) for b in range(2)})
print("empty dict comp", {k: v for k, v in []})
print("ternary value", {n: ("even" if n % 2 == 0 else "odd") for n in range(4)})
print("str keys", {w: len(w) for w in "the quick brown fox".split()})
print("counting", {c: "mississippi".count(c) for c in "mississippi"})

# set comprehensions (print sorted to be order-independent)
print("set squares", sorted({x * x for x in range(-3, 4)}))
print("set dedup", sorted({c for c in "abracadabra"}))
print("set filter", sorted({x % 5 for x in range(100) if x % 2}))
print("set of tuples", sorted({(min(a, b), max(a, b)) for a in range(3) for b in range(3)}))
print("set empty", {x for x in ()}, type({x for x in ()}).__name__)
print("set of frozensets", sorted(sorted(fs) for fs in {frozenset((a, a + 1)) for a in range(3)}))
print("set len", len({x // 3 for x in range(30)}))

# evaluation order in dict comprehension: key before value
log = []
def k(x):
    log.append("k" + str(x))
    return x

def v(x):
    log.append("v" + str(x))
    return x

r = {k(i): v(i) for i in range(2)}
print("dict comp order", r, log)

# unhashable keys
try:
    {[x]: x for x in range(2)}
except TypeError as e:
    print("unhashable dict key", type(e).__name__, e)

try:
    {[x] for x in range(2)}
except TypeError as e:
    print("unhashable set element", type(e).__name__, e)

# exception mid-comprehension leaves no partial result
result = "untouched"
try:
    result = {x: 10 // x for x in [5, 2, 0, 1]}
except ZeroDivisionError as e:
    print("partial", type(e).__name__, e, result)

# equal keys of different types collapse
print("1 and 1.0 and True", {key: type(key).__name__ for key in [1, 1.0, True]})

# dict comprehension from enumerate with unpack
print("enum dict", {i: ch for i, ch in enumerate("abc", 1)})

# nested target and starred in dict comp
rows = [("a", 1, 2, 3), ("b", 4)]
print("starred rows", {name: rest for name, *rest in rows})

# dict comp with walrus in condition
print("walrus filter", {x: y for x in range(6) if (y := x * x) % 2})

# set comprehension vs set literal vs set()
print("set literal eq", {1, 2} == {x for x in (1, 2)} == set([2, 1]))

# comprehension referencing a mutable outer dict
base = {"a": 1}
copy = {k: v * 10 for k, v in base.items()}
base["b"] = 2
print("independent copy", base, copy)

# modifying the dict being iterated raises
d = {1: 1, 2: 2}
try:
    {d.setdefault(k + 10, 0): 0 for k in d}
except RuntimeError as e:
    print("mutating source", type(e).__name__, e)

# dict.fromkeys vs comprehension
print("fromkeys", dict.fromkeys("ab", 0), {c: 0 for c in "ab"})
