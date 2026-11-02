# Rich comparisons: __lt__/__le__/__gt__/__ge__, reflection, sorting with __lt__ only,
# NotImplemented handling and TypeError messages.


class Version:
    def __init__(self, *parts):
        self.parts = parts

    def __lt__(self, other):
        if not isinstance(other, Version):
            return NotImplemented
        return self.parts < other.parts

    def __eq__(self, other):
        if not isinstance(other, Version):
            return NotImplemented
        return self.parts == other.parts

    __hash__ = None

    def __repr__(self):
        return "v" + ".".join(str(p) for p in self.parts)


vs = [Version(1, 10), Version(1, 2), Version(0, 9, 9), Version(1, 2, 0)]
print("sorted by __lt__", sorted(vs))
print("sorted reverse", sorted(vs, reverse=True))
print("min max", min(vs), max(vs))
print("gt reflects to lt", Version(2) > Version(1), Version(1) > Version(2))
try:
    Version(1) <= Version(2)
except TypeError as e:
    print(type(e).__name__, e)
try:
    Version(1) < 5
except TypeError as e:
    print(type(e).__name__, e)
try:
    5 < Version(1)
except TypeError as e:
    print(type(e).__name__, e)
print("eq other type", Version(1) == 1)
print("list.sort stable", sorted([Version(1), Version(0)], key=lambda v: 0))


class Plain:
    pass


for op, fn in [("<", lambda a, b: a < b), (">=", lambda a, b: a >= b)]:
    try:
        fn(Plain(), Plain())
    except TypeError as e:
        print(op, type(e).__name__, e)
try:
    sorted([Plain(), Plain()])
except TypeError as e:
    print("sort plain", type(e).__name__, e)
try:
    1 < "a"
except TypeError as e:
    print(type(e).__name__, e)
try:
    [1] < (1,)
except TypeError as e:
    print(type(e).__name__, e)

log = []


class Loud:
    def __init__(self, name):
        self.name = name

    def __lt__(self, other):
        log.append(self.name + ".__lt__")
        return NotImplemented

    def __gt__(self, other):
        log.append(self.name + ".__gt__")
        return NotImplemented


try:
    Loud("a") < Loud("b")
except TypeError as e:
    print("both NotImplemented", type(e).__name__, e)
print("try order", log)


class Base:
    def __lt__(self, other):
        return "Base.__lt__"


class Sub(Base):
    def __gt__(self, other):
        return "Sub.__gt__"


print("subclass reflected first", Base() < Sub())
print("same class no priority", Base() < Base())


class Truthy:
    def __lt__(self, other):
        return "yes"


print("non-bool result", Truthy() < 1)
print("non-bool in sort", sorted([Truthy(), Truthy()]) is not None)


class Weird:
    def __eq__(self, other):
        return "maybe"

    def __ne__(self, other):
        return "maybe not"

    __hash__ = object.__hash__


print("eq ne results", Weird() == 1, Weird() != 1)


class NoNe:
    def __eq__(self, other):
        return self is other

    __hash__ = object.__hash__


n = NoNe()
print("ne inverts eq", n != n, n != NoNe())
print("chained", Version(1) < Version(2) < Version(3), 1 < 2 > 0, 1 < 3 < 2)
print("tuple compare elementwise", (1, Version(2)) < (1, Version(3)), (2,) < (1, Version(1)))
words = ["banana", "Apple", "cherry"]
print("key compare", sorted(words), sorted(words, key=str.lower), max(words, key=len))
print("sort by tuple key", sorted([(2, "b"), (1, "z"), (2, "a")], key=lambda t: (-t[0], t[1])))
