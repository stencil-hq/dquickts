# Instance methods, classmethod, staticmethod, alternative constructors, bound method behaviour.


class Temperature:
    unit = "C"
    created = 0

    def __init__(self, degrees):
        self.degrees = degrees
        Temperature.created += 1

    def describe(self):
        return str(self.degrees) + self.unit

    @classmethod
    def from_fahrenheit(cls, f):
        return cls(round((f - 32) * 5 / 9, 1))

    @classmethod
    def kind(cls):
        return cls.__name__

    @staticmethod
    def is_freezing(degrees):
        return degrees <= 0

    @staticmethod
    def no_args():
        return "static no args"


t = Temperature(21)
print("instance method", t.describe())
print("via class", Temperature.describe(t))
print("classmethod ctor", Temperature.from_fahrenheit(212).describe())
print("classmethod via instance", t.kind())
print("staticmethod", Temperature.is_freezing(-1), t.is_freezing(5))
print("static no args", Temperature.no_args(), t.no_args())
print("created", Temperature.created)


class Kelvin(Temperature):
    unit = "K"


k = Kelvin.from_fahrenheit(32)
print("classmethod subclass", type(k).__name__, k.describe(), Kelvin.kind())

bound = t.describe
print("bound self", bound.__self__ is t)
print("bound func", bound.__func__ is Temperature.describe)
print("bound call", bound())
print("bound equal", t.describe == t.describe, t.describe == Temperature(1).describe)
print("bound type", type(bound).__name__)
cm = Temperature.kind
print("classmethod bound to class", cm.__self__ is Temperature)
print("staticmethod plain function", type(Temperature.is_freezing).__name__)
print("raw dict entries", type(Temperature.__dict__["kind"]).__name__, type(Temperature.__dict__["is_freezing"]).__name__)
print("staticmethod object callable", Temperature.__dict__["is_freezing"](10))
print("classmethod __func__", Temperature.__dict__["kind"].__func__.__name__)

try:
    Temperature.describe()
except TypeError as e:
    print(type(e).__name__, e)


class Chain:
    def __init__(self):
        self.ops = []

    def push(self, x):
        self.ops.append(x)
        return self

    def result(self):
        return self.ops


print("chaining", Chain().push(1).push(2).push(3).result())


class Callable:
    def method(self, *args):
        return (type(self).__name__,) + args


c = Callable()
f = c.method
print("stored bound", f(1, 2))
methods = [c.method, Callable.method]
print("unbound vs bound", methods[0](9), methods[1](c, 9))


class SelfNaming:
    def who(this):
        return "this works " + type(this).__name__


print("self is convention", SelfNaming().who())


class Getter:
    def __init__(self):
        self.value = 1

    def get(self):
        return self.value


g = Getter()
getter = g.get
g.value = 2
print("bound reads current", getter())
print("getattr method", getattr(g, "get")())


def plain(self):
    return "plain " + str(self.value)


Getter.plain = plain
print("function becomes method", g.plain())
Getter.sm = staticmethod(lambda: "added static")
Getter.cm = classmethod(lambda cls: "added class " + cls.__name__)
print("added descriptors", g.sm(), g.cm(), Getter.cm())
