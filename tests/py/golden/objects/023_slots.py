# __slots__: no instance __dict__, AttributeError on new attributes, inheritance and
# interaction with class attributes and __dict__ slots.


class Point:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y


p = Point(1, 2)
print("slots values", p.x, p.y)
p.x = 10
print("assign slot", p.x)
try:
    p.z = 3
except AttributeError as e:
    print(type(e).__name__, e)
print("has __dict__", hasattr(p, "__dict__"))
try:
    vars(p)
except TypeError as e:
    print(type(e).__name__, e)
del p.y
try:
    p.y
except AttributeError as e:
    print(type(e).__name__, e)
try:
    del p.y
except AttributeError as e:
    print(type(e).__name__, e)
p.y = 5
print("reassigned", p.y)
print("slot descriptor type", type(Point.x).__name__)
print("slots attr", Point.__slots__)


class Unset:
    __slots__ = ["a"]


try:
    Unset().a
except AttributeError as e:
    print(type(e).__name__, e)


class Point3(Point):
    __slots__ = ("z",)


p3 = Point3(1, 2)
p3.z = 3
print("subclass slots", p3.x, p3.y, p3.z)
try:
    p3.w = 4
except AttributeError as e:
    print(type(e).__name__, e)


class NoSlotsChild(Point):
    pass


n = NoSlotsChild(1, 2)
n.extra = "ok"
print("child without slots has dict", n.extra, vars(n))


class WithDict:
    __slots__ = ("a", "__dict__")


w = WithDict()
w.a = 1
w.b = 2
print("slots plus dict", w.a, vars(w))

try:

    class Conflict:
        __slots__ = ("v",)
        v = 1

except ValueError as e:
    print(type(e).__name__, e)


class ClassAttr:
    __slots__ = ("inst",)
    shared = "class level"


ca = ClassAttr()
print("class attr readable", ca.shared)
try:
    ca.shared = "x"
except AttributeError as e:
    print(type(e).__name__, e)


class Empty:
    __slots__ = ()


try:
    Empty().anything = 1
except AttributeError as e:
    print(type(e).__name__, e)


class Mangled:
    __slots__ = ("__private",)

    def __init__(self):
        self.__private = "hidden"

    def get(self):
        return self.__private


print("mangled slot", Mangled().get(), Mangled.__slots__, hasattr(Mangled, "_Mangled__private"))


class SlotsWithProperty:
    __slots__ = ("_v",)

    def __init__(self):
        self._v = 0

    @property
    def v(self):
        return self._v

    @v.setter
    def v(self, value):
        self._v = value * 10


sp = SlotsWithProperty()
sp.v = 4
print("property over slot", sp.v)
print("getattr setattr on slots", getattr(p, "x"), setattr(p, "x", 99), p.x)
