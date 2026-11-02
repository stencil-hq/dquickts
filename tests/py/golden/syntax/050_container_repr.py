# repr/str of containers and literals: nesting, recursion, empty forms, displays.

print("empty displays", [], (), {}, set(), frozenset(), "", b"")
print("singleton tuple", (1,), ("a",), ((),), tuple([None]))
print("nested", [1, [2, [3, [4]]]], (1, (2, (3,))), {"a": {"b": {"c": None}}})
print("mixed types", [1, 2.0, "3", b"4", None, True, 1j, ..., (5,), [6], {7: 8}])
print("float reprs in containers", [0.1, 1e100, -0.0, float("inf"), float("nan")])
print("str quotes in containers", ["a'b", 'a"b', "a'\"b", "\n", "\t", "é", "\x00"])
print("dict reprs", {1: "one", "two": 2, (3,): [3], None: None, 4.5: True})
print("dict str vs repr equal", str({"a": 1}) == repr({"a": 1}))
print("set of ints sorted", sorted({5, 3, 9, 1}), frozenset({2}))
print("range repr", range(5), range(1, 5), range(1, 10, 2), range(0, -5, -1))
print("slice repr", slice(1, 2), slice(None), slice(1, None, -1))
print("bool/None", [True, False, None])
print("bytearray in list", [bytearray(b"x")])
print("type objects", [int, str, list, type(None)])

# recursive containers
lst = [1, 2]
lst.append(lst)
print("recursive list", lst)
d = {"k": 1}
d["self"] = d
print("recursive dict", d)
t = ([],)
t[0].append(t)
print("recursive via tuple", t)
nested = [[]]
nested[0].append(nested)
print("indirect recursion", nested)

# custom repr in containers
class R:
    def __init__(self, n):
        self.n = n

    def __repr__(self):
        return "R" + str(self.n)

print("custom repr", [R(1), R(2)], {R(3): R(4)}, (R(5),))

class StrOnly:
    def __str__(self):
        return "STR"

    def __repr__(self):
        return "REPR"

print("containers use repr", [StrOnly()], str([StrOnly()]), StrOnly())

# repr raising inside a container propagates
class BadRepr:
    def __repr__(self):
        raise ValueError("repr failed")

try:
    repr([1, BadRepr()])
except ValueError as e:
    print("bad repr in list", type(e).__name__, e)

class NonStrRepr:
    def __repr__(self):
        return 42

try:
    repr([NonStrRepr()])
except TypeError as e:
    print("non-str repr", type(e).__name__, e)

# subclass reprs
class MyList(list):
    pass

class MyDict(dict):
    pass

class MyTuple(tuple):
    pass

print("subclass reprs", MyList([1]), MyDict(a=1), MyTuple((1, 2)))

# deeply nested repr
deep = []
cur = deep
for _ in range(50):
    nxt = []
    cur.append(nxt)
    cur = nxt
print("deep nesting length", len(repr(deep)))

# large containers
print("long list repr length", len(repr(list(range(1000)))))
print("dict views", {"a": 1}.keys(), {"a": 1}.values(), {"a": 1}.items())
print("empty views", {}.keys(), {}.values(), {}.items())

# str vs repr of str inside containers
s = "quote's"
print("str/repr", str(s), repr(s), str([s]), repr([s]))

# eval(repr(x)) round trip for literal containers
for obj in [[1, "a", (2.5, None)], {"k": [1, {2}]}, (1,), {1: {2: {3: ()}}}, [b"x", True, -0.0]]:
    print("round trip", eval(repr(obj)) == obj, repr(obj))

# tuple/list display edge syntax
print("trailing commas", [1, 2,], (1, 2,), {1: 2,}, {3,})
print("parenthesized expr is not tuple", (1), type((1)).__name__, type((1,)).__name__)
print("bare tuple", type(eval("1, 2")).__name__)
