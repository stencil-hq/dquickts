"""L0 golden: list methods and their error messages.

Covers append, extend, insert (negative/out-of-range positions), pop, remove,
index (with start/stop), count, reverse, copy (shallow), clear.

Usage: python3.15 -I -X utf8 tests/py/golden/types/040_list_methods.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


a = [1, 2, 3]
show("append returns", lambda: a.append(4))
show("after append", lambda: a)
a.append([5, 6])
show("append list nests", lambda: a)
a.pop()
a.extend([5, 6])
show("extend list", lambda: a)
a.extend("ab")
show("extend str", lambda: a)
a.extend(range(2))
show("extend range", lambda: a)
a.extend(())
show("extend empty", lambda: a)
show("extend int", lambda: a.extend(5))
show("extend None", lambda: a.extend(None))
a = [1, 2, 3]
a.extend(a)
show("extend self", lambda: a)

b = ["a", "b", "c"]
b.insert(0, "start")
b.insert(len(b), "end")
b.insert(-1, "before-end")
b.insert(100, "far")
b.insert(-100, "far-neg")
show("insert", lambda: b)
show("insert float", lambda: b.insert(1.0, "x"))
show("insert missing arg", lambda: b.insert(1))

c = [10, 20, 30, 40, 50]
show("pop()", lambda: c.pop())
show("pop(0)", lambda: c.pop(0))
show("pop(-2)", lambda: c.pop(-2))
show("after pops", lambda: c)
show("pop(10)", lambda: c.pop(10))
show("pop(-10)", lambda: c.pop(-10))
show("pop empty", lambda: [].pop())
show("pop str", lambda: c.pop("0"))

d = [1, 2, 3, 2, 1]
d.remove(2)
show("remove first", lambda: d)
show("remove missing", lambda: d.remove(9))
show("remove missing str", lambda: ["a"].remove("b"))
show("remove by equality", lambda: (lambda l: (l.remove(1.0), l)[1])([True, 1, 1.0]))

e = ["a", "b", "c", "b", "a"]
show("index", lambda: e.index("b"))
show("index start", lambda: e.index("b", 2))
show("index start stop", lambda: e.index("a", 1, 5))
show("index neg start", lambda: e.index("a", -1))
show("index missing", lambda: e.index("z"))
show("index missing in range", lambda: e.index("a", 1, 4))
show("index None", lambda: e.index(None))
show("count", lambda: e.count("a"))
show("count missing", lambda: e.count("z"))
show("count numeric eq", lambda: [1, 1.0, True, "1"].count(1))
show("count nested", lambda: [[1], [1], 1].count([1]))
show("count no arg", lambda: e.count())

f = [1, 2, 3]
show("reverse returns", lambda: f.reverse())
show("after reverse", lambda: f)
show("reverse empty", lambda: (lambda l: (l.reverse(), l)[1])([]))
g = [[1, 2], 3]
h = g.copy()
h[0].append(99)
h[1] = 0
show("copy shallow orig", lambda: g)
show("copy shallow copy", lambda: h)
show("copy is new", lambda: h is not g)
show("copy via slice", lambda: g[:] == g and g[:] is not g)
k = [1, 2]
alias = k
k.clear()
show("clear", lambda: (k, alias, alias is k))
show("clear empty", lambda: [].clear())
show("methods on tuple", lambda: (1,).append(2))
show("list.append unbound", lambda: (lambda l: (list.append(l, 7), l)[1])([]))
