"""L0 golden: list operators, comparisons, repetition aliasing and slice assignment.

Covers lexicographic comparison, `*` repetition (shared nested references),
`+` creating a new list vs `+=` extending in place (identity), slice
assignment (grow/shrink/extended), del statements and index errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/042_list_ops.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("[1,2] < [1,3]", lambda: [1, 2] < [1, 3])
show("[1,2] < [1,2,0]", lambda: [1, 2] < [1, 2, 0])
show("[] < [0]", lambda: [] < [0])
show("[2] > [1, 99]", lambda: [2] > [1, 99])
show("[1,2] == [1,2]", lambda: [1, 2] == [1, 2])
show("[1,2] <= [1,2]", lambda: [1, 2] <= [1, 2])
show("[1,'a'] < [1,'b']", lambda: [1, "a"] < [1, "b"])
show("[1,'a'] < [2, 3]", lambda: [1, "a"] < [2, 3])
show("[1,'a'] < [1, 3]", lambda: [1, "a"] < [1, 3])
show("[[1]] < [[1, 0]]", lambda: [[1]] < [[1, 0]])
show("[1] == (1,)", lambda: [1] == (1,))
show("[1] < (1,)", lambda: [1] < (1,))
show("[] == []", lambda: [] == [])
show("[nan] identity eq", lambda: (lambda n: [n] == [n])(float("nan")))

grid = [[0] * 3] * 2
grid[0][0] = 1
show("* aliasing nested", lambda: grid)
show("* aliasing identity", lambda: grid[0] is grid[1])
fixed = [[0] * 3 for _ in range(2)]
fixed[0][0] = 1
show("comprehension no aliasing", lambda: fixed)
show("[1,2] * 0", lambda: [1, 2] * 0)
show("[1,2] * -1", lambda: [1, 2] * -1)
show("2 * [None]", lambda: 2 * [None])
show("[1] * 1.0", lambda: [1] * 1.0)
show("[1] * True", lambda: [1] * True)
r = [1]
r *= 3
show("*= ", lambda: r)

a = [1, 2]
alias = a
a = a + [3]
show("+ new object", lambda: (a, alias, a is alias))
a = alias
a += [3]
show("+= same object", lambda: (a, alias, a is alias))
a += (4, 5)
show("+= tuple ok", lambda: a)
a += "xy"
show("+= str ok", lambda: a)
show("+ tuple error", lambda: [1] + (2,))
show("+ str error", lambda: [1] + "x")
show("+= int error", lambda: (lambda l: l.__iadd__(5))([]))

s = list(range(10))
s[2:5] = ["a"]
show("slice shrink", lambda: s)
s[0:0] = [-2, -1]
show("slice insert front", lambda: s)
s[len(s):] = [99]
show("slice append", lambda: s)
s[1:3] = []
show("slice delete", lambda: s)
s[:] = "abc"
show("slice replace all from str", lambda: s)
s[10:20] = [1]
show("slice past end", lambda: s)
s[-100:1] = ["start"]
show("slice neg past start", lambda: s)
t = list(range(8))
t[::2] = "WXYZ"
show("extended slice assign", lambda: t)
show("extended slice wrong len", lambda: t.__setitem__(slice(None, None, 2), [1]))
t[::-3] = [None, None, None]
show("negative step assign", lambda: t)
show("slice assign non-iterable", lambda: t.__setitem__(slice(0, 1), 5))
u = [1, 2, 3]
u[1:2] = u
show("slice assign self", lambda: u)
u[5:1] = ["x"]
show("slice start > stop inserts", lambda: u)

v = list(range(10))
del v[0]
show("del item", lambda: v)
del v[-1]
show("del neg", lambda: v)
del v[::2]
show("del extended", lambda: v)
del v[1:2]
show("del slice", lambda: v)
del v[100:]
show("del slice past end", lambda: v)
show("del out of range", lambda: v.__delitem__(100))
show("index out of range", lambda: v[100])
show("index neg out of range", lambda: v[-100])
show("assign out of range", lambda: v.__setitem__(100, 0))
show("index str", lambda: v["0"])
show("final", lambda: v)
