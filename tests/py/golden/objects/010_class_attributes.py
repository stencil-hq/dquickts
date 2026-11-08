# Instance vs class attributes: shadowing, shared mutable class state, attribute lookup order.


class Counter:
    count = 0
    shared = []
    label = "class"

    def __init__(self, name):
        self.name = name
        Counter.count += 1

    def add(self, item):
        self.shared.append(item)


a = Counter("a")
b = Counter("b")
print("class counter", Counter.count, a.count, b.count)
a.add(1)
b.add(2)
print("shared list", a.shared, b.shared, Counter.shared)
print("same object", a.shared is b.shared)

a.label = "instance"
print("shadowing", a.label, b.label, Counter.label)
print("instance dict", a.__dict__)
del a.label
print("after del", a.label)
try:
    del a.label
except AttributeError as e:
    print(type(e).__name__, e)

a.count = 100
print("instance count", a.count, Counter.count)
Counter.label = "changed"
print("class change visible", a.label, b.label)

b.shared = ["own"]
print("rebinding breaks sharing", b.shared, Counter.shared)


class Base:
    x = "base"
    y = "base"


class Child(Base):
    y = "child"


c = Child()
print("lookup", c.x, c.y)
Base.x = "base2"
print("dynamic lookup", c.x)
c.x = "inst"
print("instance first", c.x, Child.x, Base.x)
print("class dict has", "x" in Child.__dict__, "y" in Child.__dict__)


class Empty:
    pass


e = Empty()
e.anything = 1
Empty.later = "added later"
print("added later", e.later, e.anything)
print("class attr not in instance dict", e.__dict__)
try:
    e.nope
except AttributeError as ex:
    print(type(ex).__name__, ex)
try:
    Empty.nope
except AttributeError as ex:
    print(type(ex).__name__, ex)


class WithMethodsAsAttrs:
    def method(self):
        return "method"


w = WithMethodsAsAttrs()
w.method = lambda: "instance override"
print("instance override method", w.method(), WithMethodsAsAttrs().method())
WithMethodsAsAttrs.method = lambda self: "class patched"
print("class patched", WithMethodsAsAttrs().method())


def external(self):
    return "external " + type(self).__name__


WithMethodsAsAttrs.ext = external
print("attached function", w.ext())
print("class name attrs", Counter.__name__, Counter.__module__)
print("bases", Child.__bases__ == (Base,), Base.__bases__ == (object,))
print("class of instance", type(c) is Child, c.__class__ is Child)


class Mutable:
    items = []

    def __init__(self):
        self.items = self.items + [len(self.items)]


m1 = Mutable()
m2 = Mutable()
print("augmented copy", m1.items, m2.items, Mutable.items)


class Aug:
    items = []

    def __init__(self):
        self.items += [1]


Aug()
Aug()
print("augmented in place", Aug.items)
