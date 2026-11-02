"""L0 golden: collections.Counter and collections.defaultdict."""
from collections import Counter, defaultdict

c = Counter("abracadabra")
print("counter", c)
print("most_common", c.most_common())
print("most_common 2", c.most_common(2))
print("ties keep insertion order", Counter("bbaacc").most_common())
print("missing key", c["z"], "z" in c)
print("total", c.total())
print("elements", sorted(c.elements()))
print("elements order", "".join(Counter(b=2, a=1, c=0, d=-1).elements()))
print("keys order", list(c))

d = Counter(a=3, b=1, c=-2)
e = Counter(a=1, b=2, d=4)
print("add", d + e)
print("sub", d - e)
print("and", d & e)
print("or", d | e)
print("unary +", +d)
print("unary -", -d)
print("eq ignores zero", Counter(a=1, b=0) == Counter(a=1))
print("le", Counter(a=1) <= Counter(a=2, b=1), Counter(a=3) <= Counter(a=2))

u = Counter(a=1)
u.update("aab")
u.update({"c": 5})
u.update(a=10)
print("update", u)
u.subtract({"c": 7, "z": 1})
print("subtract", u)
u["b"] += 1
del u["a"]
print("mutated", u)
print("from dict", Counter({"x": 2, "y": 0}))
print("from list", Counter([1, 2, 2, 3, 3, 3]).most_common(1))
words = "the cat and the hat and the bat".split()
print("word counts", Counter(words).most_common(3))
print("sorted items", sorted(Counter(words).items()))
print("repr empty", Counter())
print("fromkeys", end=" ")
try:
    Counter.fromkeys("abc")
except NotImplementedError as e:
    print(type(e).__name__, e)
print("is dict", isinstance(c, dict))

# defaultdict
dl = defaultdict(list)
for k, v in [("a", 1), ("b", 2), ("a", 3)]:
    dl[k].append(v)
print("dd list", dl)
print("dd list dict", dict(dl))
di = defaultdict(int)
for ch in "mississippi":
    di[ch] += 1
print("dd int", sorted(di.items()))
print("default_factory", di.default_factory)
print("access creates", di["zz"], "zz" in di)
print("get does not create", di.get("yy"), "yy" in di)

tree = defaultdict(lambda: defaultdict(int))
tree["x"]["a"] += 1
tree["x"]["b"] += 2
tree["y"]["a"] += 3
print("nested", {k: dict(v) for k, v in tree.items()})

ds = defaultdict(set)
ds["k"].add(3)
ds["k"].add(1)
print("dd set", sorted(ds["k"]))

none_dd = defaultdict()
try:
    none_dd["missing"]
except KeyError as e:
    print("no factory", type(e).__name__, e)
print("copy", defaultdict(int, {"a": 1}).copy())
merged = defaultdict(int, a=1) | {"b": 2}
print("or", type(merged).__name__, merged)
try:
    defaultdict(5)
except TypeError as e:
    print("bad factory", type(e).__name__, e)
