"""L0 golden: dict repr, recursion, equality and key semantics.

Covers nested and recursive reprs ({...}), equality across nesting and value
types, hash-based key identity (-1/-2, 1/1.0/True), unhashable keys,
__missing__ on subclasses, and dict comparisons that are unsupported.

Usage: python3.15 -I -X utf8 tests/py/golden/types/048_dict_repr_misc.py
"""


class DefaultZero(dict):
    """dict subclass returning 0 for missing keys via __missing__."""

    def __missing__(self, key):
        return 0


class Key:
    """Hashable key with value equality; counts __eq__ calls."""

    eq_calls = 0

    def __init__(self, name: str) -> None:
        self.name = name

    def __hash__(self) -> int:
        return 42

    def __eq__(self, other) -> bool:
        Key.eq_calls += 1
        return isinstance(other, Key) and self.name == other.name

    def __repr__(self) -> str:
        return f"Key({self.name!r})"


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("empty", lambda: {})
show("nested", lambda: {"a": {"b": {"c": []}}, 1: (2,), None: {1, 2}})
show("str keys quoting", lambda: {"it's": "x\n", 'q"': b"\x00"})
rec = {"name": "root"}
rec["self"] = rec
show("recursive", lambda: rec)
show("recursive str", lambda: str(rec))
r2 = {}
r2["list"] = [r2]
show("recursive via list", lambda: r2)
show("recursive eq self", lambda: rec == rec)

show("eq", lambda: {"a": [1, 2]} == {"a": [1, 2]})
show("eq numeric values", lambda: {"a": 1} == {"a": 1.0})
show("ne different keys", lambda: {"a": 1} != {"b": 1})
show("eq vs items list", lambda: {"a": 1} == [("a", 1)])
show("lt unsupported", lambda: {} < {})
show("dict + dict", lambda: {} + {})
show("dict | list", lambda: {} | [])
show("bool", lambda: (bool({}), bool({0: 0})))

show("-1 and -2 distinct keys", lambda: {-1: "m1", -2: "m2"})
show("1/1.0/True merge", lambda: {1: "a", 1.0: "b", True: "c"})
show("0/-0.0/False merge", lambda: {0: "a", -0.0: "b", False: "c"})
show("str vs bytes keys", lambda: {"a": 1, b"a": 2})
show("tuple keys", lambda: {(): 0, (1,): 1, ((),): 2})
show("frozenset key", lambda: {frozenset({1}): "fs"}[frozenset([1])])
show("list key", lambda: {[1]: 0})
show("set key", lambda: {set(): 0})
show("dict key", lambda: {{}: 0})
show("nan keys", lambda: len({float("nan"): 1, float("nan"): 2}))

Key.eq_calls = 0
kd = {Key("a"): 1, Key("b"): 2}
show("colliding hashes", lambda: kd)
show("colliding lookup", lambda: kd[Key("b")])
show("eq was called", lambda: Key.eq_calls > 0)
show("colliding missing", lambda: kd[Key("c")])

dz = DefaultZero(a=1)
show("__missing__", lambda: (dz["a"], dz["zz"]))
show("__missing__ not stored", lambda: dz)
show("__missing__ not used by get", lambda: dz.get("zz"))
show("subclass repr", lambda: DefaultZero({"x": 1}))
show("subclass eq dict", lambda: DefaultZero(x=1) == {"x": 1})
show("subclass | type", lambda: type(DefaultZero() | {}).__name__)

show("kwargs keys must be str", lambda: dict(**{1: 2}))
show("dict(**{'a': 1})", lambda: dict(**{"a": 1}))
show("dict unpack merge", lambda: {**{"a": 1, "b": 2}, **{"b": 3}, "c": 4})
show("len after overwrite", lambda: len({"a": 1, "a": 2}))
show("literal dup key last wins", lambda: {"a": 1, "a": 2})
show("format", lambda: f"{ {'k': 1} }")
show("str of dict", lambda: str({1: "1"}))
show("in checks keys", lambda: (1 in {1: 2}, 2 in {1: 2}))
show("iter gives keys", lambda: list(iter({"x": 1, "y": 2})))
show("del missing", lambda: {}.__delitem__("k"))
show("keys order big ints", lambda: list({2 ** 64: 0, -1: 0, 0: 0}))
show("max dict", lambda: max({"b": 1, "a": 9}))
show("sorted by value", lambda: sorted({"b": 1, "a": 9, "c": 5}, key={"b": 1, "a": 9, "c": 5}.get))
