# property: getter/setter/deleter, read-only properties, validation, inheritance and overriding.


class Circle:
    def __init__(self, radius):
        self.radius = radius

    @property
    def radius(self):
        """The radius."""
        return self._radius

    @radius.setter
    def radius(self, value):
        if value < 0:
            raise ValueError("radius must be >= 0")
        self._radius = value

    @radius.deleter
    def radius(self):
        print("deleting radius")
        del self._radius

    @property
    def diameter(self):
        return self._radius * 2


c = Circle(3)
print("get", c.radius, c.diameter)
c.radius = 5
print("set", c.radius, c.diameter)
try:
    c.radius = -1
except ValueError as e:
    print(type(e).__name__, e)
print("unchanged", c.radius)
try:
    c.diameter = 4
except AttributeError as e:
    print(type(e).__name__, e)
try:
    del c.diameter
except AttributeError as e:
    print(type(e).__name__, e)
del c.radius
try:
    c.radius
except AttributeError as e:
    print(type(e).__name__, e)
print("instance dict", c.__dict__)
print("doc", Circle.radius.__doc__)
print("property type", type(Circle.radius).__name__)
print("fget name", Circle.radius.fget.__name__, Circle.diameter.fset, Circle.diameter.fdel)


class Legacy:
    def _get(self):
        return self._x * 10

    def _set(self, v):
        self._x = v

    x = property(_get, _set, None, "legacy doc")


lg = Legacy()
lg.x = 4
print("property() call", lg.x, Legacy.x.__doc__)


class Square:
    def __init__(self, side):
        self._side = side

    @property
    def area(self):
        return self._side**2


class Cube(Square):
    @property
    def area(self):
        return 6 * super().area


print("override property", Square(2).area, Cube(2).area)


class Lazy:
    computed = 0

    @property
    def value(self):
        Lazy.computed += 1
        return 42


lz = Lazy()
print("property recomputes", lz.value, lz.value, Lazy.computed)


class Shadow:
    @property
    def p(self):
        return "property"


s = Shadow()
s.__dict__["p"] = "instance dict"
print("data descriptor wins", s.p)


class Base:
    @property
    def name(self):
        return "base"


class Derived(Base):
    @Base.name.setter
    def name(self, value):
        self._name = value


d = Derived()
d.name = "x"
print("extended property", d.name, d._name, Base().name)
try:
    Base().name = "y"
except AttributeError as e:
    print(type(e).__name__, e)


class Raising:
    @property
    def bad(self):
        raise KeyError("inner")

    def __getattr__(self, name):
        return "fallback " + name


print("property AttributeError fallback", Raising().missing)
try:
    Raising().bad
except KeyError as e:
    print(type(e).__name__, e)
print("class access returns property", isinstance(Circle.radius, property))
print("getter copy distinct", Circle.radius.getter(lambda self: 0) is not Circle.radius)
