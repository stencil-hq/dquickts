# Unpacking inside displays: [*a, *b], (*a,), {*s}, {**d1, **d2}.

a = [1, 2]
b = (3, 4)
print("list concat", [*a, *b])
print("list mixed", [0, *a, 9, *b, *"xy"])
print("tuple star", (*a, *b), (*a,), (*"ab", 5))
print("empty star", [*[], *()], (*[],), [*a, *[]])
print("from range", [*range(3), *range(5, 7)])
print("from dict keys", [*{"k1": 1, "k2": 2}])
print("from generator", [*(x * 2 for x in range(3))])
print("from string", [*"hey"])
print("from bytes", [*b"hi"])
print("from sorted set", [*sorted({3, 1, 2})])

s1 = {1, 2}
s2 = {2, 3}
print("set star", sorted({*s1, *s2}))
print("set star with items", sorted({0, *s1, 10}))
print("set star dedup", sorted({*[1, 1, 1], *(1, 2)}))
print("set from str", sorted({*"banana"}))

d1 = {"a": 1, "b": 2}
d2 = {"b": 20, "c": 30}
print("dict merge", {**d1, **d2})
print("dict merge reversed", {**d2, **d1})
print("dict merge with keys", {"z": 0, **d1, "a": 100})
print("dict key override order", {"a": 0, **{"a": 1}})
print("dict empty merge", {**{}, **{}})
print("dict merge copies", {**d1} == d1, {**d1} is not d1)
print("dict union op", d1 | d2, d2 | d1)

# ** requires a mapping
try:
    {**[1, 2]}
except TypeError as e:
    print("dict star list", type(e).__name__, e)

try:
    {**None}
except TypeError as e:
    print("dict star None", type(e).__name__, e)

# custom mapping: needs keys() and __getitem__
class M:
    def keys(self):
        return ["p", "q"]

    def __getitem__(self, k):
        return k.upper()

print("custom mapping", {**M()})

# non-string keys are fine in displays
print("int keys", {**{1: "one"}, **{2: "two"}})

# * requires an iterable
try:
    [*5]
except TypeError as e:
    print("star int", type(e).__name__, e)

try:
    (*None,)
except TypeError as e:
    print("star None", type(e).__name__, e)

try:
    {*{}.get("x")}
except TypeError as e:
    print("set star None", type(e).__name__, e)

# evaluation order of elements
log = []
def f(tag, v):
    log.append(tag)
    return v

r = [f("a", 1), *f("b", [2, 3]), f("c", 4)]
print("list eval order", r, log)
log.clear()
r = {f("k1", "x"): f("v1", 1), **f("m", {"y": 2}), f("k2", "z"): f("v2", 3)}
print("dict eval order", r, log)

# unhashable element in set display
try:
    {*[[1]]}
except TypeError as e:
    print("set unhashable", type(e).__name__, e)

# starred in subscripts (tuple index) and return/yield
def ret(*xs):
    return *xs, "end"

print("return star", ret(1, 2))

def gen():
    yield *"ab", 3

print("yield star", list(gen()))
d = {}
d[*"ab",] = 1
print("subscript star", d)
for x in *a, *b:
    print("for over star", x)
