"""L0 golden: dict views (keys, values, items).

Views are live (reflect later mutations), support len/in/iteration/reversed,
keys and items views support set operations returning sets (printed via
sorted), values views do not.  Mutating a dict while iterating raises
RuntimeError.

Usage: python3.15 -I -X utf8 tests/py/golden/types/047_dict_views.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


d = {"a": 1, "b": 2}
keys = d.keys()
values = d.values()
items = d.items()
show("keys repr", lambda: keys)
show("values repr", lambda: values)
show("items repr", lambda: items)
d["c"] = 3
show("keys live", lambda: keys)
show("values live", lambda: list(values))
show("items live", lambda: list(items))
del d["a"]
show("len live", lambda: (len(keys), len(values), len(items)))
show("'b' in keys", lambda: "b" in keys)
show("2 in values", lambda: 2 in values)
show("('b', 2) in items", lambda: ("b", 2) in items)
show("('b', 3) in items", lambda: ("b", 3) in items)
show("['b', 2] in items", lambda: ["b", 2] in items)
show("unhashable in items", lambda: ("b", []) in items)
show("reversed keys", lambda: list(reversed(keys)))
show("reversed values", lambda: list(reversed(values)))
show("reversed items", lambda: list(reversed(items)))
show("reversed dict", lambda: list(reversed(d)))

k1 = {"a": 1, "b": 2, "c": 3}.keys()
k2 = {"b": 0, "c": 0, "d": 0}.keys()
show("keys & keys", lambda: sorted(k1 & k2))
show("keys | keys", lambda: sorted(k1 | k2))
show("keys - keys", lambda: sorted(k1 - k2))
show("keys ^ keys", lambda: sorted(k1 ^ k2))
show("keys & set", lambda: sorted(k1 & {"a", "z"}))
show("keys | list", lambda: sorted(k1 | ["z"]))
show("set - keys", lambda: sorted({"a", "q"} - k1))
show("result type", lambda: type(k1 & k2).__name__)
show("keys == set", lambda: k1 == {"a", "b", "c"})
show("keys <= set", lambda: k1 <= {"a", "b", "c", "d"})
show("keys < keys", lambda: {"a": 0}.keys() < k1)
show("keys isdisjoint", lambda: k1.isdisjoint(["x", "y"]))
show("keys & int", lambda: k1 & 5)
i1 = {"a": 1, "b": 2}.items()
i2 = {"a": 1, "b": 3}.items()
show("items & items", lambda: sorted(i1 & i2))
show("items - items", lambda: sorted(i1 - i2))
show("items | items", lambda: sorted(i1 | i2))
show("items ^ items", lambda: sorted(i1 ^ i2))
show("items == set", lambda: i1 == {("a", 1), ("b", 2)})
show("values == values", lambda: {"a": 1}.values() == {"a": 1}.values())
show("values & values", lambda: {"a": 1}.values() & {"a": 1}.values())
show("values unhashable ok", lambda: list({"a": [1]}.values()))
show("items with unhashable & ", lambda: {"a": [1]}.items() & {("a", 1)})
show("keys hash", lambda: hash({}.keys()))
show("mapping attr", lambda: k1.mapping)
show("keys index", lambda: k1[0])

e = {i: i for i in range(5)}
try:
    for k in e:
        e[k + 100] = 0
except RuntimeError as exc:
    print("add during iteration", type(exc).__name__, str(exc))
f = {i: i for i in range(5)}
try:
    for k in f:
        del f[k]
except RuntimeError as exc:
    print("del during iteration", type(exc).__name__, str(exc))
g = {i: i for i in range(5)}
for k in g:
    g[k] = -k
print("overwrite during iteration ok", g)
h = {i: i for i in range(5)}
try:
    for k in h.keys():
        h.pop(k)
except RuntimeError as exc:
    print("pop during keys iteration", type(exc).__name__, str(exc))
j = {i: i for i in range(3)}
try:
    for v in j.values():
        j.clear()
except RuntimeError as exc:
    print("clear during values iteration", type(exc).__name__, str(exc))
m = {1: 1}
for k in list(m):
    m[k + 1] = 0
print("iterate over list copy ok", m)
