"""L0 golden: set mutation methods and frozenset hashing.

Covers add, discard, remove (KeyError message), pop (single element and
empty), update/intersection_update/difference_update/
symmetric_difference_update, copy, clear, frozenset as dict key and set
element, constructor forms and reprs (set(), frozenset()).

Usage: python3.15 -I -X utf8 tests/py/golden/types/050_set_methods.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("set()", lambda: set())
show("frozenset()", lambda: frozenset())
show("{1}", lambda: {1})
show("frozenset({5})", lambda: frozenset({5}))
show("set('a')", lambda: set("a"))
show("set('aaa')", lambda: set("aaa"))
show("sorted set('hello')", lambda: sorted(set("hello")))
show("set(range(3))", lambda: set(range(3)))
show("set(5)", lambda: set(5))
show("set([[1]])", lambda: set([[1]]))
show("{[]}", lambda: {[]})
show("set of dict", lambda: sorted(set({"b": 1, "a": 2})))
show("bool", lambda: (bool(set()), bool({0})))

s = set()
s.add("x")
s.add("x")
s.add(("t", 1))
show("add dedup", lambda: (len(s), "x" in s))
show("add returns", lambda: s.add("y"))
show("add unhashable", lambda: s.add([]))
show("discard present", lambda: s.discard("y"))
show("discard missing", lambda: s.discard("nope"))
show("discard unhashable", lambda: s.discard([]))
show("discard set (frozen lookup)", lambda: s.discard(set()))
show("remove present", lambda: s.remove("x"))
show("remove missing str", lambda: s.remove("x"))
show("remove missing int", lambda: s.remove(5))
show("remove missing tuple", lambda: s.remove((1, 2)))
show("remove unhashable", lambda: s.remove({}))
try:
    set().remove("k")
except KeyError as e:
    print("remove KeyError args", e.args, repr(str(e)))
show("pop single", lambda: {42}.pop())
show("pop single str", lambda: {"only"}.pop())
show("pop empty", lambda: set().pop())
popped = {7, 8}
values = sorted([popped.pop(), popped.pop()])
show("pop drains", lambda: (values, popped))

u = {1, 2}
u.update([3], (4,), range(5, 7))
show("update multi", lambda: sorted(u))
u.update()
show("update none", lambda: sorted(u))
u.intersection_update({1, 2, 3, 99}, [1, 3])
show("intersection_update", lambda: sorted(u))
u.difference_update([1])
show("difference_update", lambda: sorted(u))
u.symmetric_difference_update([3, 4])
show("symmetric_difference_update", lambda: sorted(u))
show("update returns None", lambda: u.update([0]))
show("update non-iterable", lambda: u.update(5))
show("update str", lambda: (u.update("ab"), len(u))[1])

v = {1, 2}
w = v.copy()
w.add(3)
show("copy independent", lambda: (sorted(v), sorted(w)))
alias = v
v.clear()
show("clear", lambda: (v, alias))

fs = frozenset([3, 1, 2])
show("frozenset hashable", lambda: hash(fs) == hash(frozenset({1, 2, 3})))
show("frozenset dict key", lambda: {fs: "val"}[frozenset([1, 2, 3])])
show("set lookup by set", lambda: {fs: 1}[{1, 2, 3}])
show("set in set of frozensets", lambda: {1, 2, 3} in {fs})
show("frozenset in set", lambda: sorted(map(sorted, {fs, frozenset(), frozenset([1])})))
show("frozenset no add", lambda: fs.add(4))
show("frozenset copy identity", lambda: fs.copy() is fs)
show("frozenset(frozenset) identity", lambda: frozenset(fs) is fs)
show("set hash", lambda: hash({1}))
show("set as key", lambda: {{1}: 0})
show("nested frozenset repr", lambda: frozenset({frozenset()}))
show("frozenset eq set", lambda: fs == {1, 2, 3})
show("frozenset methods", lambda: sorted(fs.union([4])))
show("set of mixed numeric", lambda: len({1, 1.0, True, 2, 2.0}))
show("isinstance", lambda: (isinstance(fs, frozenset), isinstance(fs, set)))
show("type names", lambda: (type(set()).__name__, type(fs).__name__))
