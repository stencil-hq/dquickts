"""L0 golden: functools cached_property, total_ordering, singledispatch(method)."""
import functools


class Circle:
    def __init__(self, r):
        self.r = r
        self.computed = 0

    @functools.cached_property
    def area(self):
        self.computed += 1
        return 3 * self.r * self.r


c = Circle(2)
print("cached_property", c.area, c.area, c.computed, "area" in vars(c))
del c.area
print("after del", c.area, c.computed)
c.area = 99
print("assignable", c.area, c.computed)
print("class access", type(Circle.area).__name__, Circle.area.attrname)


class Slotted:
    __slots__ = ("r",)

    @functools.cached_property
    def area(self):
        return 1


try:
    Slotted().area
except TypeError as e:
    print("no __dict__", type(e).__name__, e)


@functools.total_ordering
class Version:
    def __init__(self, *parts):
        self.parts = parts

    def __eq__(self, other):
        return self.parts == other.parts

    def __lt__(self, other):
        return self.parts < other.parts

    def __repr__(self):
        return "V" + ".".join(map(str, self.parts))


v1, v2 = Version(1, 2), Version(1, 10)
print("total_ordering", v1 < v2, v1 <= v2, v1 > v2, v1 >= v2, v1 >= Version(1, 2))
print("sorted versions", sorted([Version(2), Version(1, 10), Version(1, 2)]))
print("max/min", max(v1, v2), min(v1, v2))
try:
    @functools.total_ordering
    class NoOrder:
        pass
except ValueError as e:
    print("total_ordering needs one", type(e).__name__, e)


@functools.singledispatch
def describe(x):
    return f"object {x!r}"


@describe.register
def _(x: int):
    return f"int {x}"


@describe.register(list)
def _(x):
    return "list of " + ", ".join(describe(i) for i in x)


@describe.register(bool)
def _(x):
    return f"bool {x}"


@describe.register(float | complex)
def _(x):
    return f"number {x}"


print("singledispatch", describe(3), "|", describe(True), "|", describe("s"), "|", describe([1, 2.5]))
print("union register", describe(1j), "|", describe(0.5))
print("dispatch", describe.dispatch(bool).__name__, describe.dispatch(str) is describe.registry[object])
print("registry keys", sorted(t.__name__ for t in describe.registry))
try:
    describe()
except TypeError as e:
    print("no args", type(e).__name__, e)


class Shape:
    @functools.singledispatchmethod
    def scale(self, arg):
        return f"generic {arg}"

    @scale.register
    def _(self, arg: int):
        return f"int x{arg}"

    @scale.register
    def _(self, arg: str):
        return f"named {arg}"


print("singledispatchmethod", Shape().scale(3), Shape().scale("big"), Shape().scale(2.5))
