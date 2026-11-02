"""L0 golden: list repr (nested, self-referencing) and iteration builtins.

Covers list() construction, len, min/max (key, default, empty errors),
sum (start, non-numeric errors), enumerate(start), zip (strict=True
errors), reversed, any/all, and recursive container reprs.

Usage: python3.15 -I -X utf8 tests/py/golden/types/043_list_builtins.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("nested repr", lambda: [1, [2, [3, []]], "s", (4,), {"k": [5]}])
self_ref = [1, 2]
self_ref.append(self_ref)
show("self ref", lambda: self_ref)
show("self ref str", lambda: str(self_ref))
show("[[...]]", lambda: (lambda l: (l.append(l), [l])[1])([]))
x = []
y = [x]
x.append(y)
show("mutual ref", lambda: x)
d = {"list": []}
d["list"].append(d)
show("dict in list ref", lambda: d)
show("self ref len", lambda: len(self_ref))
show("self ref eq self", lambda: self_ref == self_ref)

show("list()", lambda: list())
show("list(range(5))", lambda: list(range(5)))
show("list(range(5, 0, -2))", lambda: list(range(5, 0, -2)))
show("list('ab')", lambda: list("ab"))
show("list({'a': 1})", lambda: list({"a": 1}))
show("list(5)", lambda: list(5))
show("list(None)", lambda: list(None))
show("list(gen)", lambda: list(i * i for i in range(4)))
show("list copies", lambda: (lambda l: list(l) is l)([1]))
show("len([])", lambda: len([]))
show("len nested", lambda: len([[1, 2], [3]]))

show("min", lambda: min([3, 1, 2]))
show("max", lambda: max([3, 1, 2]))
show("min args", lambda: min(3, 1, 2))
show("max key", lambda: max(["aa", "b", "ccc"], key=len))
show("max key tie first", lambda: max(["ab", "cd", "e"], key=len))
show("min key tie first", lambda: min(["ab", "cd", "e", "f"], key=len))
show("min empty", lambda: min([]))
show("max empty", lambda: max([]))
show("min empty default", lambda: min([], default="none"))
show("max default unused", lambda: max([1], default=0))
show("max args default", lambda: max(1, 2, default=0))
show("max no args", lambda: max())
show("min int", lambda: min(5))
show("max mixed", lambda: max([1, "a"]))
show("max str", lambda: max(["apple", "Banana", "cherry"]))
show("sum", lambda: sum([1, 2, 3]))
show("sum start", lambda: sum([1, 2], 10))
show("sum start kw", lambda: sum([1, 2], start=10))
show("sum lists", lambda: sum([[1], [2, 3]], []))
show("sum floats", lambda: sum([0.5, 0.25]))
show("sum bools", lambda: sum([True, True]))
show("sum empty", lambda: sum([]))
show("sum str", lambda: sum(["a", "b"], ""))
show("sum bytes", lambda: sum([b"a"], b""))
show("sum str items", lambda: sum(["a", "b"]))
show("sum big", lambda: sum([2 ** 64, 2 ** 64, -1]))

show("enumerate", lambda: list(enumerate("abc")))
show("enumerate start", lambda: list(enumerate("abc", 1)))
show("enumerate start kw", lambda: list(enumerate("ab", start=-5)))
show("enumerate big start", lambda: list(enumerate("ab", 2 ** 64)))
show("enumerate float start", lambda: list(enumerate("ab", 1.5)))
show("zip", lambda: list(zip([1, 2, 3], "ab")))
show("zip three", lambda: list(zip("ab", "cd", "ef")))
show("zip none", lambda: list(zip()))
show("zip one", lambda: list(zip([1, 2])))
show("zip strict ok", lambda: list(zip([1, 2], "ab", strict=True)))
show("zip strict short 2nd", lambda: list(zip([1, 2, 3], "ab", strict=True)))
show("zip strict long 2nd", lambda: list(zip([1], "ab", strict=True)))
show("zip strict 3rd", lambda: list(zip([1], "a", (1, 2), strict=True)))
show("unzip", lambda: list(zip(*[(1, "a"), (2, "b")])))
show("reversed list", lambda: list(reversed([1, 2, 3])))
show("reversed str", lambda: list(reversed("abc")))
show("reversed range", lambda: list(reversed(range(3))))
show("reversed tuple", lambda: list(reversed((1, 2))))
show("reversed gen", lambda: reversed(i for i in range(3)))
show("reversed set", lambda: reversed({1}))
show("reversed type", lambda: type(reversed([1])).__name__)
show("any/all", lambda: (any([]), all([]), any([0, "", 1]), all([1, "x", []])))
show("map", lambda: list(map(str, [1, 2])))
show("filter", lambda: list(filter(None, [0, 1, "", "a", None, []])))
show("filter func", lambda: list(filter(lambda v: v % 2, range(7))))
