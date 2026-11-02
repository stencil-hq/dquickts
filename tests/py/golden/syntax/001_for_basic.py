# for loops: iteration over builtin iterables, loop variable after loop, ranges.

print("range5", [i for i in range(5)])
acc = []
for i in range(3):
    acc.append(i * 10)
print("range3 acc", acc)
print("loop var after", i)

for i in range(0):
    pass
print("empty range keeps old i", i)

for ch in "abc":
    print("char", repr(ch))
print("last char", repr(ch))

for x in [1, 2.5, "s", None, (1, 2)]:
    print("item", repr(x))

for x in (10, 20):
    print("tuple item", x)

d = {"one": 1, "two": 2, "three": 3}
for k in d:
    print("dict key", k, d[k])
for k, v in d.items():
    print("dict item", k, v)
for v in d.values():
    print("dict value", v)

for n in sorted({3, 1, 2}):
    print("sorted set", n)

for i in range(10, 0, -3):
    print("range step -3", i)
for i in range(-2, 3, 2):
    print("range neg start", i)

for b in b"AZ":
    print("bytes item", repr(b))

for i, ch in enumerate("xyz"):
    print("enumerate", i, ch)
for i, ch in enumerate("xy", start=5):
    print("enumerate start", i, ch)

for a, b in zip([1, 2, 3], "ab"):
    print("zip", a, b)

for x in reversed([1, 2, 3]):
    print("reversed", x)

for x in iter([7, 8]):
    print("iter", x)

# mutating the list while iterating over it (append extends the loop)
lst = [1, 2, 3]
seen = []
for x in lst:
    seen.append(x)
    if x < 3 and len(lst) < 5:
        lst.append(x + 10)
print("mutate append", seen, lst)

# rebinding the loop variable does not affect iteration
out = []
for i in range(3):
    out.append(i)
    i = 100
print("rebind loop var", out, i)

# iterating over a generator
def gen():
    yield "g1"
    yield "g2"

for s in gen():
    print("gen item", s)

# loop target can be an attribute or subscript
class Box:
    pass

box = Box()
for box.val in [1, 2, 3]:
    pass
print("attr target", box.val)

store = [0, 0]
for store[1] in "pq":
    pass
print("subscript target", store)

# iterating a non-iterable
try:
    for x in 5:
        pass
except TypeError as e:
    print("non-iterable", type(e).__name__, e)

try:
    for x in None:
        pass
except TypeError as e:
    print("none iterable", type(e).__name__, e)

# a custom iterable via __iter__
class Count:
    def __init__(self, n):
        self.n = n

    def __iter__(self):
        return iter(range(self.n))

print("custom iterable", [x for x in Count(4)])

# a custom iterable via __getitem__ (old protocol)
class Seq:
    def __getitem__(self, i):
        if i >= 3:
            raise IndexError(i)
        return i * i

for x in Seq():
    print("getitem protocol", x)

total = 0
for i in range(1, 101):
    total += i
print("sum 1..100", total)
