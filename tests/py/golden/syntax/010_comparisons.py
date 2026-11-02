# Comparisons: chained, mixed types, side effects evaluated once, is / is not.

print("1<2<3", 1 < 2 < 3)
print("1<3<2", 1 < 3 < 2)
print("3>2>1", 3 > 2 > 1)
print("1<2>0", 1 < 2 > 0)
print("1==1==1", 1 == 1 == 1)
print("1==1!=2", 1 == 1 != 2)
print("1<=1<=1<2", 1 <= 1 <= 1 < 2)
print("0<1>2", 0 < 1 > 2)
print("a<b<c str", "a" < "b" < "c")
print("chain not transitive", 1 < 5 > 2)

# chain is not (1 < 2) < 3
print("parenthesised", (3 < 2) < 1, 3 < 2 < 1)
print("bool compare", (1 < 2) == True)

# middle operand evaluated once
log = []
def f(name, v):
    log.append(name)
    return v

print("chain once", f("a", 1) < f("b", 2) < f("c", 3), log)
log.clear()
print("chain short-circuit", f("a", 3) < f("b", 2) < f("c", 1), log)
log.clear()
print("long chain", f("a", 1) < f("b", 2) < f("c", 3) < f("d", 0) < f("e", 9), log)

# numeric comparisons across types
print("int float eq", 1 == 1.0, 2 < 2.5, -1 > -1.5)
print("int complex eq", 1 == 1 + 0j, 0 == 0j)
print("bool int", True == 1, False < 1, True > 0.5)
print("big int float", 2**53 + 1 == float(2**53 + 1), 2**53 + 1 > float(2**53))
print("nan compare", float("nan") == float("nan"), float("nan") != float("nan"), float("nan") < 1)
print("inf compare", float("inf") > 10**400, -float("inf") < -10**400)
print("neg zero", 0.0 == -0.0, -0.0 < 0.0)

# sequence comparisons are lexicographic
print("list lex", [1, 2] < [1, 3], [1, 2] < [1, 2, 0], [2] > [1, 9, 9])
print("tuple lex", (1, "b") < (1, "c"), () < (0,), (1, 2) == (1, 2))
print("str lex", "abc" < "abd", "ab" < "abc", "B" < "a", "" < "a")
print("bytes lex", b"a" < b"b", b"ab" < b"a\x00\x00")
print("list tuple eq", [1, 2] == (1, 2))
print("dict eq order", {"a": 1, "b": 2} == {"b": 2, "a": 1})
print("set compare", {1, 2} < {1, 2, 3}, {1, 2} <= {1, 2}, {1, 4} > {1}, {1} == frozenset({1}))

# unorderable types
def cmp(label, fn):
    try:
        print(label, fn())
    except TypeError as e:
        print(label, type(e).__name__, e)

cmp("int < str", lambda: 1 < "1")
cmp("str >= list", lambda: "a" >= ["a"])
cmp("None < None", lambda: None < None)
cmp("dict < dict", lambda: {} < {})
cmp("complex < complex", lambda: 1j < 2j)
cmp("list < tuple", lambda: [1] < (1,))
cmp("eq mixed is fine", lambda: 1 == "1")
cmp("ne mixed is fine", lambda: [] != ())
cmp("mixed list elements", lambda: [1, "a"] < [1, 2])

# is / is not
x = None
print("is None", x is None, x is not None)
print("not x is None", not x is None)
print("True is True", True is True, (1 == 1) is True)
print("False is not True", False is not True)
a = [1]
b = a
c = [1]
print("same list", a is b, a is c, a == c, a is not c)
print("empty tuple eq", () == (), [] == [], [] == ())
print("type is", type(1) is int, type(True) is bool, type(True) is int)
print("Ellipsis is", ... is Ellipsis)
print("NotImplemented is", NotImplemented is NotImplemented)

# is binds like comparison operators (can be chained)
print("is chain", a is b is not c)

# rich comparison methods returning non-bools
class Weird:
    def __lt__(self, other):
        return "less!"

    def __eq__(self, other):
        return 0

print("non-bool lt", repr(Weird() < 1))
print("non-bool eq", repr(Weird() == 1))
print("non-bool in chain", repr(0 < 1 and Weird() < 5))
print("chain with weird", repr(Weird() < 1 < 2))

# reflected comparisons
class Ten:
    def __gt__(self, other):
        return "Ten.__gt__"

print("reflected", repr(5 < Ten()))

# min/max with comparisons
print("max chain", max(3, 1, 2), min("b", "a", "c"), max([1, 2], [1, 3]))
