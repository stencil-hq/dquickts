# __eq__ / __hash__ interplay: default identity semantics, __hash__ = None when __eq__ is
# defined, explicit hashing, use as dict keys and set members.


class Plain:
    pass


p1, p2 = Plain(), Plain()
print("default eq identity", p1 == p1, p1 == p2, p1 != p2)
print("default hashable", hash(p1) == hash(p1), {p1: 1}[p1])
print("default __hash__ present", Plain.__hash__ is not None)


class EqOnly:
    def __init__(self, v):
        self.v = v

    def __eq__(self, other):
        return isinstance(other, EqOnly) and self.v == other.v


print("eq only __hash__ is None", EqOnly.__hash__ is None)
print("eq only compares", EqOnly(1) == EqOnly(1), EqOnly(1) != EqOnly(2))
for label, action in [
    ("hash()", lambda: hash(EqOnly(1))),
    ("set member", lambda: {EqOnly(1)}),
    ("dict key", lambda: {EqOnly(1): 0}),
]:
    try:
        action()
    except TypeError as e:
        print(label, type(e).__name__, e)


class Hashable:
    def __init__(self, v):
        self.v = v

    def __eq__(self, other):
        if not isinstance(other, Hashable):
            return NotImplemented
        return self.v == other.v

    def __hash__(self):
        return hash(self.v)

    def __repr__(self):
        return "H(" + repr(self.v) + ")"


print("hash equal values", hash(Hashable(5)) == hash(Hashable(5)), hash(Hashable(5)))
s = {Hashable(1), Hashable(1), Hashable(2)}
print("set dedup", len(s))
d = {Hashable("a"): 1}
d[Hashable("a")] = 2
print("dict overwrite", d)
print("lookup", d[Hashable("a")], Hashable("b") in d)
print("eq NotImplemented", Hashable(1) == 1, Hashable(1) != 1)


class ChildOfHashable(Hashable):
    def __eq__(self, other):
        return True


print("child eq resets hash", ChildOfHashable.__hash__ is None)


class KeepsHash(Hashable):
    def __eq__(self, other):
        return True

    __hash__ = Hashable.__hash__


print("explicit keep hash", hash(KeepsHash(7)) == hash(7))


class Unhashable:
    __hash__ = None


try:
    hash(Unhashable())
except TypeError as e:
    print(type(e).__name__, e)


class BadHash:
    def __hash__(self):
        return "nope"


try:
    hash(BadHash())
except TypeError as e:
    print(type(e).__name__, e)


class BigHash:
    def __hash__(self):
        return 2**100


print("big hash reduced", hash(BigHash()) == hash(2**100))


class MinusOne:
    def __hash__(self):
        return -1


print("hash -1 becomes -2", hash(MinusOne()))

calls = []


class Tracking:
    def __init__(self, name):
        self.name = name

    def __eq__(self, other):
        calls.append(self.name + "==" + other.name)
        return NotImplemented

    __hash__ = object.__hash__


a, b = Tracking("a"), Tracking("b")
print("both NotImplemented falls back to identity", a == b, a == a, a != b)
print("eq calls", calls)


class AlwaysEq:
    def __eq__(self, other):
        return True

    __hash__ = None


print("list contains uses eq", AlwaysEq() in [1, 2], [1, AlwaysEq()].index(AlwaysEq()))
print("list count", [1, 2, 3].count(AlwaysEq()))
print("reflected eq", 5 == AlwaysEq(), "x" != AlwaysEq())
nan = float("nan")
print("identity shortcut in containers", nan in [nan], nan == nan, [nan] == [nan])
print("int float hash", hash(1) == hash(1.0) == hash(True), {1: "a", 1.0: "b", True: "c"})
print("tuple hash", hash((1, 2)) == hash((1, 2)), hash(()) == hash(()))
