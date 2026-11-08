"""L0 golden: range objects.

Covers construction errors, len (including huge ranges and OverflowError),
indexing with negatives, slicing returning a range (repr), negative steps,
membership (int fast path, floats, non-numbers), equality by sequence
(range(0) == range(2, 2)), index/count, reversed, hashing and attributes.

Usage: python3.15 -I -X utf8 tests/py/golden/types/051_range.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("range(5)", lambda: range(5))
show("range(1, 5)", lambda: range(1, 5))
show("range(1, 5, 1)", lambda: range(1, 5, 1))
show("range(10, 0, -3)", lambda: range(10, 0, -3))
show("list(range(10, 0, -3))", lambda: list(range(10, 0, -3)))
show("list(range(0, -5, -1))", lambda: list(range(0, -5, -1)))
show("list(range(5, 5))", lambda: list(range(5, 5)))
show("list(range(5, 0))", lambda: list(range(5, 0)))
show("range(0, 1, 0)", lambda: range(0, 1, 0))
show("range()", lambda: range())
show("range(1.0)", lambda: range(1.0))
show("range('1')", lambda: range("1"))
show("range(1, 2, 3, 4)", lambda: range(1, 2, 3, 4))
show("range(stop=3)", lambda: range(stop=3))

r = range(3, 20, 4)
show("len", lambda: len(r))
show("attrs", lambda: (r.start, r.stop, r.step))
show("r[0]", lambda: r[0])
show("r[-1]", lambda: r[-1])
show("r[-5]", lambda: r[-5])
show("r[5]", lambda: r[5])
show("r[-6]", lambda: r[-6])
show("r[1:3]", lambda: r[1:3])
show("r[::-1]", lambda: r[::-1])
show("r[::2]", lambda: r[::2])
show("r[10:]", lambda: r[10:])
show("r[-2:]", lambda: r[-2:])
show("r[::-1] list", lambda: list(r[::-1]))
show("range(10)[2:8:3]", lambda: range(10)[2:8:3])
show("range(10)[::-3]", lambda: range(10)[::-3])
show("range(0, 10, 3)[:-1]", lambda: range(0, 10, 3)[:-1])
show("r['a']", lambda: r["a"])
show("r[1.0]", lambda: r[1.0])

show("len huge", lambda: len(range(2 ** 62)))
show("len too huge", lambda: len(range(2 ** 64)))
show("huge last", lambda: range(2 ** 64)[-1])
show("huge slice", lambda: range(2 ** 100)[2 ** 99 :: 2 ** 98])
show("huge in", lambda: 2 ** 99 in range(0, 2 ** 100, 2))
show("huge index", lambda: range(0, 2 ** 100, 3).index(3 * 2 ** 90))

show("7 in r", lambda: 7 in r)
show("8 in r", lambda: 8 in r)
show("7.0 in r", lambda: 7.0 in r)
show("7.5 in r", lambda: 7.5 in r)
show("True in range(2)", lambda: True in range(2))
show("'a' in r", lambda: "a" in r)
show("None in r", lambda: None in r)
show("-1 in range(-5, 0)", lambda: -1 in range(-5, 0))
show("0 in range(0)", lambda: 0 in range(0))
show("in negative step", lambda: (4 in range(10, 0, -3), 3 in range(10, 0, -3)))

show("range(0) == range(2, 2)", lambda: range(0) == range(2, 2))
show("range(0, 3, 2) == range(0, 4, 2)", lambda: range(0, 3, 2) == range(0, 4, 2))
show("range(1) == range(1, 5, 10)", lambda: range(1) == range(0, 1, 7))
show("range(3) == [0, 1, 2]", lambda: range(3) == [0, 1, 2])
show("range(3) == range(3)", lambda: range(3) == range(3))
show("range(3) != range(4)", lambda: range(3) != range(4))
show("range < range", lambda: range(1) < range(2))
show("hash equal ranges", lambda: hash(range(0)) == hash(range(5, 5)))
show("hash equal ranges 2", lambda: hash(range(0, 3, 2)) == hash(range(0, 4, 2)))

show("index", lambda: r.index(11))
show("index missing", lambda: r.index(12))
show("index float", lambda: r.index(7.0))
show("count", lambda: r.count(7))
show("count missing", lambda: r.count(8))
show("count float", lambda: r.count(7.0))
show("reversed", lambda: list(reversed(r)))
show("reversed empty", lambda: list(reversed(range(0))))
show("reversed type", lambda: type(reversed(r)).__name__)
show("iter type", lambda: type(iter(r)).__name__)
show("sum", lambda: sum(range(101)))
show("bool", lambda: (bool(range(0)), bool(range(1))))
show("tuple(range)", lambda: tuple(range(-2, 2)))
show("min/max", lambda: (min(r), max(r)))
show("max empty", lambda: max(range(0)))
show("range is not list", lambda: type(range(1)).__name__)
show("range setitem", lambda: range(3).__setitem__(0, 1))
show("range + range", lambda: range(1) + range(2))
show("range * 2", lambda: range(1) * 2)
show("str", lambda: str(range(-1, -10, -2)))
show("range(True)", lambda: range(True))
show("enumerate range", lambda: list(enumerate(range(5, 8))))
