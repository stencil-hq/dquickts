"""L0 golden: set and frozenset operators and subset relations.

Results are printed via sorted() so output never depends on hash iteration
order.  Covers | & - ^ (operators and methods, which accept any iterable),
<= < >= > relations, isdisjoint, result types for mixed set/frozenset.

Usage: python3.15 -I -X utf8 tests/py/golden/types/049_set_ops.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


a = {"a", "b", "c", "d"}
b = {"c", "d", "e"}
show("a | b", lambda: sorted(a | b))
show("a & b", lambda: sorted(a & b))
show("a - b", lambda: sorted(a - b))
show("b - a", lambda: sorted(b - a))
show("a ^ b", lambda: sorted(a ^ b))
show("union", lambda: sorted(a.union(b, ["z"], "xy")))
show("intersection", lambda: sorted(a.intersection(b, "cde")))
show("intersection none", lambda: sorted(a.intersection()))
show("difference", lambda: sorted(a.difference(b, ["a"])))
show("symmetric_difference", lambda: sorted(a.symmetric_difference(["a", "q"])))
show("union no args copy", lambda: sorted(a.union()))
show("operator with list", lambda: a | ["x"])
show("operator with frozenset", lambda: sorted(a & frozenset("cx")))
show("type set|frozenset", lambda: type(a | frozenset()).__name__)
show("type frozenset|set", lambda: type(frozenset() | a).__name__)
show("type frozenset.union(set)", lambda: type(frozenset().union(a)).__name__)
show("type set.union(frozenset)", lambda: type(a.union(frozenset())).__name__)

s123 = {1, 2, 3}
show("{1} <= s", lambda: {1} <= s123)
show("{1} < s", lambda: {1} < s123)
show("s <= s", lambda: s123 <= s123)
show("s < s", lambda: s123 < s123)
show("s >= {1, 2}", lambda: s123 >= {1, 2})
show("s > s", lambda: s123 > s123)
show("{4} <= s", lambda: {4} <= s123)
show("{1, 4} < s", lambda: {1, 4} < s123)
show("{1, 4} > s", lambda: {1, 4} > s123)
show("set() <= s", lambda: set() <= s123)
show("set() < set()", lambda: set() < set())
show("issubset iterable", lambda: s123.issubset(range(5)))
show("issuperset iterable", lambda: s123.issuperset([1, 1, 2]))
show("<= with list", lambda: s123 <= [1, 2, 3])
show("isdisjoint", lambda: s123.isdisjoint({4, 5}))
show("isdisjoint false", lambda: s123.isdisjoint([3]))
show("isdisjoint empty", lambda: set().isdisjoint(set()))
show("isdisjoint str", lambda: {"a"}.isdisjoint("bcd"))
show("set == frozenset", lambda: {1, 2} == frozenset({2, 1}))
show("set != different", lambda: {1} != {1, 2})
show("set == list", lambda: {1} == [1])
show("numeric merging", lambda: {1, 2} == {1.0, 2.0, True})
show("min/max", lambda: (min(s123), max(s123)))
show("sum", lambda: sum({1, 2, 3}))
show("len", lambda: len(a | b))
show("in", lambda: ("a" in a, "z" in a, 1.0 in s123))
show("in unhashable", lambda: [] in s123)
show("in set-of-set", lambda: {1} in {frozenset({1})})

c = {1, 2, 3}
alias = c
c |= {4}
show("|= in place", lambda: (sorted(c), alias is c))
c &= {1, 2, 4, 9}
show("&= ", lambda: sorted(c))
c -= {1}
show("-= ", lambda: sorted(c))
c ^= {2, 7}
show("^= ", lambda: (sorted(c), alias is c))
f = frozenset({1, 2})
falias = f
f |= {3}
show("frozenset |= rebinds", lambda: (sorted(f), falias is f, sorted(falias)))
show("|= with list", lambda: c.__ior__([1]))
show("set + set", lambda: {1} + {2})
show("set * 2", lambda: {1} * 2)
show("chained ops", lambda: sorted((a | b) - (a & b)) == sorted(a ^ b))
show("de morgan-ish", lambda: sorted(a - (a - b)) == sorted(a & b))
show("comprehension", lambda: sorted({x % 4 for x in range(20)}))
show("small int set repr", lambda: {3, 1, 2})
show("empty set ops", lambda: (set() | set(), set() & {1}, frozenset() - {1}))
