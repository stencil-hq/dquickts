"""L0 golden: dict methods and their error paths.

Covers get (default None), setdefault, pop (with/without default, KeyError
message repr), popitem on empty, update (mapping, pairs, kwargs, errors),
fromkeys (shared value), copy (shallow), clear, in, len.

Usage: python3.15 -I -X utf8 tests/py/golden/types/046_dict_methods.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


d = {"a": 1, "b": 2, 3: "three", (1, 2): "tuple"}
show("get", lambda: d.get("a"))
show("get missing", lambda: d.get("zz"))
show("get default", lambda: d.get("zz", 0))
show("get tuple key", lambda: d.get((1, 2)))
show("get unhashable", lambda: d.get([1]))
show("get float==int key", lambda: d.get(3.0))
show("[] missing str", lambda: d["missing"])
show("[] missing int", lambda: d[42])
show("[] missing tuple", lambda: d[(9, 9)])
show("[] missing empty str", lambda: d[""])
show("[] missing None", lambda: d[None])
show("[] unhashable", lambda: d[{}])

try:
    d["nope"]
except KeyError as e:
    print("KeyError args", e.args, "str", str(e), "repr", repr(e))
try:
    d[("x",)]
except KeyError as e:
    print("KeyError tuple str", str(e))

show("setdefault new", lambda: d.setdefault("c", []))
show("setdefault existing", lambda: d.setdefault("a", 100))
show("setdefault no default", lambda: d.setdefault("n"))
d.setdefault("c").append(1)
show("setdefault mutable", lambda: d["c"])
show("pop", lambda: d.pop("a"))
show("pop missing", lambda: d.pop("a"))
show("pop default", lambda: d.pop("a", "dflt"))
show("pop default None", lambda: d.pop("a", None))
show("pop no args", lambda: d.pop())
show("in", lambda: ("b" in d, "a" in d, 3 in d, 3.0 in d, "3" in d))
show("in unhashable", lambda: [] in d)
show("len", lambda: len(d))
show("popitem", lambda: d.popitem())
show("popitem empty", lambda: {}.popitem())

e = {}
e.update({"x": 1})
e.update([("y", 2), ["z", 3]])
e.update(w=4)
e.update({"x": 10}, x=100)
show("update chain", lambda: e)
show("update bad seq elem", lambda: e.update([("a", 1, 2)]))
show("update bad seq elem 2", lambda: e.update(["ab", "c"]))
show("update non-iterable", lambda: e.update(5))
show("update two positional", lambda: e.update({}, {}))
show("update returns", lambda: e.update())

f = dict.fromkeys(["a", "b"], [])
f["a"].append(1)
show("fromkeys shared value", lambda: f)
show("fromkeys default None", lambda: dict.fromkeys("xy"))
show("fromkeys range", lambda: dict.fromkeys(range(3), 0))
show("fromkeys unhashable", lambda: dict.fromkeys([[1]]))

g = {"k": [1, 2]}
h = g.copy()
h["k"].append(3)
h["new"] = 1
show("copy shallow orig", lambda: g)
show("copy shallow copy", lambda: h)
show("copy is new", lambda: h is not g)
alias = g
g.clear()
show("clear", lambda: (g, alias))
show("dict()", lambda: dict())
show("dict(a=1)", lambda: dict(a=1, b=2))
show("dict pairs", lambda: dict([(1, 2), (3, 4)]))
show("dict pairs dup", lambda: dict([(1, "a"), (1, "b")]))
show("dict bad", lambda: dict([1, 2]))
show("dict str pairs", lambda: dict(["ab", "cd"]))
show("dict comprehension", lambda: {k: v for k, v in zip("abc", range(3)) if v != 1})
show("dict int/str keys", lambda: {1: "int", "1": "str", 1.5: "float", None: "none"})
show("dict.get unbound", lambda: dict.get({"a": 1}, "a"))
show("keys type", lambda: type({}.keys()).__name__)
show("values type", lambda: type({}.values()).__name__)
show("items type", lambda: type({}.items()).__name__)
