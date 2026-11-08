"""L0 golden: copy.copy/deepcopy semantics and the operator module."""
import collections
import copy
import operator

nested = {"list": [1, [2, 3]], "tuple": (4, [5]), "str": "s"}
shallow = copy.copy(nested)
deep = copy.deepcopy(nested)
print("shallow is new", shallow is not nested, shallow == nested)
print("shallow shares", shallow["list"] is nested["list"])
print("deep separate", deep["list"] is not nested["list"], deep["list"][1] is not nested["list"][1])
nested["list"][1].append(99)
print("after mutation", shallow["list"], deep["list"])
print("deep tuple with list copied", deep["tuple"] is not nested["tuple"], deep["tuple"] == (4, [5]))
print("immutable tuple kept", copy.deepcopy((1, 2, "x")) is not None, copy.copy((1, 2)) == (1, 2))
print("str identity", copy.deepcopy(nested["str"]) is nested["str"])

cyc = [1, 2]
cyc.append(cyc)
cc = copy.deepcopy(cyc)
print("cycle", cc[2] is cc, cc[2] is not cyc, cc[:2])
d = {"self": None}
d["self"] = d
dc = copy.deepcopy(d)
print("dict cycle", dc["self"] is dc)
shared = [0]
pair = [shared, shared]
pc = copy.deepcopy(pair)
print("shared preserved", pc[0] is pc[1], pc[0] is not shared)
memo = {}
copy.deepcopy([shared], memo)
print("memo used", any(v is not None for v in memo.values()))


class Node:
    def __init__(self, name, children=None):
        self.name = name
        self.children = children or []


tree = Node("root", [Node("a"), Node("b")])
t2 = copy.deepcopy(tree)
t3 = copy.copy(tree)
print("object deepcopy", t2.children[0].name, t2.children is not tree.children, t3.children is tree.children)


class Custom:
    def __init__(self, v):
        self.v = v

    def __copy__(self):
        return Custom(("copied", self.v))

    def __deepcopy__(self, memo):
        return Custom(("deep", copy.deepcopy(self.v, memo)))


print("hooks", copy.copy(Custom(1)).v, copy.deepcopy(Custom([1])).v)
P = collections.namedtuple("P", "x y")
print("replace", copy.replace(P(1, 2), y=5))
print("list.copy vs slicing", [1, [2]].copy()[1] is not None, list({"a": 1}.copy().items()))

# operator
print("itemgetter", operator.itemgetter(1)([10, 20, 30]), operator.itemgetter(0, 2)("abc"))
print("itemgetter dict", operator.itemgetter("k")({"k": "v"}))
rows = [("b", 2), ("a", 2), ("c", 1)]
print("sort itemgetter", sorted(rows, key=operator.itemgetter(1, 0)))
print("itemgetter slice", operator.itemgetter(slice(1, None))([1, 2, 3]))
tree.extra = Node("deep")
print("attrgetter", operator.attrgetter("name")(tree), operator.attrgetter("extra.name", "name")(tree))
print("methodcaller", operator.methodcaller("split", ",")("a,b"), operator.methodcaller("replace", "a", "o", 1)("banana"))
print("methodcaller kw", operator.methodcaller("to_bytes", 2, byteorder="big")(258))
print("repr getters", repr(operator.itemgetter(1, "x")), repr(operator.methodcaller("m", 1, k=2)))
print("arith", operator.add(2, 3), operator.sub(2, 3), operator.mul("ab", 2), operator.truediv(7, 2))
print("arith 2", operator.floordiv(-7, 2), operator.mod(-7, 2), operator.pow(2, 8), operator.neg(5), operator.matmul.__name__)
print("compare", operator.lt(1, 2), operator.eq("a", "a"), operator.ne(1, 1.0), operator.ge(3, 3))
print("logic", operator.not_(0), operator.truth([]), operator.is_(None, None), operator.is_none(None))
print("bitwise", operator.and_(12, 10), operator.or_(12, 10), operator.xor(12, 10), operator.invert(0), operator.lshift(1, 4))
print("sequence", operator.concat([1], [2]), operator.contains("abc", "b"), operator.countOf([1, 2, 1], 1), operator.indexOf("abc", "c"))
lst = [1, 2, 3]
operator.setitem(lst, 0, 9)
operator.delitem(lst, 1)
print("setitem/delitem", lst, operator.getitem(lst, -1))
x = [1]
y = operator.iadd(x, [2])
print("iadd in place", y is x, x)
s = "a"
print("iadd immutable", operator.iadd(s, "b"), s)
print("index", operator.index(7), operator.length_hint(iter([1, 2, 3])), operator.length_hint(object(), 5))
try:
    operator.index(1.5)
except TypeError as e:
    print("index float", type(e).__name__, e)
try:
    operator.itemgetter(5)([1])
except IndexError as e:
    print("itemgetter missing", type(e).__name__, e)
try:
    operator.attrgetter("nope")(tree)
except AttributeError as e:
    print("attrgetter missing", type(e).__name__, e)
print("call", operator.call(max, 3, 9, 4))
