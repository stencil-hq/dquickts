"""L0 golden: dict insertion order semantics.

Overwriting a key keeps its original position; deleting and re-inserting
moves it to the end; update/|= follow the same rules; popitem is LIFO;
equality ignores order.

Usage: python3.15 -I -X utf8 tests/py/golden/types/045_dict_order.py
"""

d = {}
d["b"] = 1
d["a"] = 2
d["c"] = 3
print("insert order", d)
d["b"] = 10
print("overwrite keeps position", d)
del d["b"]
d["b"] = 20
print("del + reinsert moves to end", d)
d.update({"a": 0, "z": 26})
print("update overwrite + append", d)
d.update([("y", 25), ("c", 33)])
print("update pairs", d)
d.update(x=24, a=1)
print("update kwargs", d)
print("keys", list(d.keys()))
print("values", list(d.values()))
print("items", list(d.items()))
print("popitem", d.popitem())
print("popitem", d.popitem())
print("after popitem", d)
d.setdefault("q", 0)
d.setdefault("a", 999)
print("setdefault appends only new", d)
d.pop("c")
d["c"] = 3
print("pop + reinsert", d)

ints = {}
for k in (5, 3, 9, 1, 7):
    ints[k] = k * k
print("int keys keep insertion order", ints)
print("sorted keys", sorted(ints))
ints[3] = -1
print("int overwrite", ints)
print("reversed", list(reversed(ints)))
print("reversed items", list(reversed(ints.items())))
print("reversed values", list(reversed(ints.values())))

a = {"x": 1, "y": 2}
b = {"y": 2, "x": 1}
print("eq ignores order", a == b)
print("list(keys) order differs", list(a) == list(b))
print("items eq", a.items() == b.items())
print("merge |", a | {"z": 3, "x": 0})
print("merge | right wins position left", {"x": 0, "z": 3} | a)
a |= {"y": 20, "w": 4}
print("|= ", a)
print("dict(zip)", dict(zip("cab", range(3))))
print("fromkeys order", dict.fromkeys("hello"))
print("comprehension order", {c: ord(c) for c in "zyx"})
print("dict(dict) order", dict({"b": 1, "a": 2}))
print("dict(pairs, **kw) order", dict([("b", 1)], a=2, b=3))
print("copy order", {"q": 1, "p": 2}.copy())

many = {}
for i in range(100):
    many[str(i)] = i
for i in range(0, 100, 2):
    del many[str(i)]
for i in range(0, 10):
    many[str(i)] = -i
print("after churn first 10", list(many)[:10])
print("after churn last 10", list(many)[-10:])
print("after churn len", len(many))
big = {i: None for i in range(1000)}
for i in range(999):
    del big[i]
big["new"] = 1
print("after mass delete", big)
