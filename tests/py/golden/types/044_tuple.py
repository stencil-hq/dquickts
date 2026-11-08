"""L0 golden: tuple repr, hashing, comparison and methods.

Covers () / (1,) reprs, nesting, hashability (TypeError when containing a
list), lexicographic comparisons, count/index, concatenation/repetition,
tuples as dict keys, unpacking and immutability errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/044_tuple.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("()", lambda: ())
show("(1,)", lambda: (1,))
show("(1, 2)", lambda: (1, 2))
show("nested", lambda: ((1, (2, ())), [3], "s"))
show("tuple()", lambda: tuple())
show("tuple('ab')", lambda: tuple("ab"))
show("tuple([1, [2]])", lambda: tuple([1, [2]]))
show("tuple(range(3))", lambda: tuple(range(3)))
show("tuple(5)", lambda: tuple(5))
show("tuple of tuple identity", lambda: (lambda t: tuple(t) is t)((1, 2)))
show("str((1,))", lambda: str((1,)))
show("repr (('a',),)", lambda: (("a",),))
show("tuple with list repr", lambda: ([],))
t_list = ([1],)
t_list[0].append(2)
show("mutate list inside tuple", lambda: t_list)
show("+= on element raises", lambda: t_list.__setitem__(0, [9]))

show("hash eq", lambda: hash((1, 2)) == hash((1, 2)))
show("hash numeric", lambda: hash((1, 2.0)) == hash((1.0, 2)))
show("hash ()", lambda: hash(()) == hash(tuple()))
show("hash with list", lambda: hash((1, [2])))
show("hash nested list", lambda: hash((1, (2, [3]))))
show("hash with dict", lambda: hash(({},)))
show("dict key tuple", lambda: {(1, 2): "a", (1, 2.0): "b"})
show("dict key list tuple", lambda: {(1, [2]): 0})
show("set of tuples", lambda: sorted({(2, 1), (1, 2), (1, 2)}))

show("(1,2) < (1,3)", lambda: (1, 2) < (1, 3))
show("(1,2) < (1,2,0)", lambda: (1, 2) < (1, 2, 0))
show("() < (0,)", lambda: () < (0,))
show("(2,) > (1, 99)", lambda: (2,) > (1, 99))
show("(1, 'a') < (1, 'b')", lambda: (1, "a") < (1, "b"))
show("(1, 'a') < (1, 2)", lambda: (1, "a") < (1, 2))
show("(1, 'a') < (2, 'b')", lambda: (1, "a") < (2, 2))
show("(1,) == [1]", lambda: (1,) == [1])
show("(1,) < [2]", lambda: (1,) < [2])
show("(1, 2) == (1.0, 2.0)", lambda: (1, 2) == (1.0, 2.0))
show("(nan,) identity", lambda: (lambda n: (n,) == (n,))(float("nan")))
show("max tuples", lambda: max([(1, "b"), (1, "a"), (0, "z")]))

t = (1, 2, 3, 2, 1, 2)
show("count", lambda: t.count(2))
show("count missing", lambda: t.count(9))
show("index", lambda: t.index(2))
show("index start", lambda: t.index(2, 2))
show("index start stop", lambda: t.index(1, 1, 5))
show("index missing", lambda: t.index(9))
show("index missing range", lambda: t.index(3, 3))
show("len", lambda: len(t))
show("in", lambda: 3 in t)
show("slice", lambda: t[1:4])
show("slice type", lambda: type(t[1:4]).__name__)
show("step slice", lambda: t[::-2])
show("index error", lambda: t[10])
show("setitem error", lambda: t.__setitem__(0, 0))
show("delitem error", lambda: t.__delitem__(0))
show("no append", lambda: t.append(1))

show("+", lambda: (1, 2) + (3,))
show("+ empty", lambda: () + ())
show("+ list error", lambda: (1,) + [2])
show("* 3", lambda: (1, 2) * 3)
show("* 0", lambda: (1,) * 0)
show("* -1", lambda: (1,) * -1)
u = (1,)
alias = u
u += (2,)
show("+= rebinds", lambda: (u, alias, u is alias))
show("* 1 identity", lambda: (lambda x: x * 1 is x)((1, 2)))

a, *rest = (1, 2, 3)
show("star unpack", lambda: (a, rest))
show("unpack too many", lambda: (lambda: [(p, q) for p, q in [(1, 2, 3)]])())
show("unpack too few", lambda: (lambda: [(p, q, r) for p, q, r in [(1, 2)]])())
show("single element no comma", lambda: type((1)).__name__)
show("bool(())", lambda: (bool(()), bool((0,))))
show("sorted tuple", lambda: sorted((3, 1, 2)))
show("tuple of mixed sort error", lambda: sorted(((1,), ("a",))))
