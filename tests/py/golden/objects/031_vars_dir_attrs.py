# vars(), __dict__, dir() (filtered to non-dunder names), getattr/setattr/hasattr/delattr.


class Base:
    base_attr = 1

    def base_method(self):
        pass


class Thing(Base):
    cls_attr = "c"

    def __init__(self):
        self.x = 1
        self.y = 2

    def method(self):
        pass

    @property
    def prop(self):
        return "p"


def public(names):
    return sorted(n for n in names if not n.startswith("__"))


t = Thing()
print("vars", vars(t))
print("__dict__ same object", vars(t) is t.__dict__)
t.__dict__["z"] = 3
print("write through __dict__", t.z)
print("dir instance", public(dir(t)))
print("dir class", public(dir(Thing)))
print("class __dict__ keys", public(Thing.__dict__))
print("class __dict__ type", type(Thing.__dict__).__name__)
try:
    Thing.__dict__["new"] = 1
except TypeError as e:
    print(type(e).__name__, e)
print("dir builtin", public(dir(1))[:6])
print("dir list", public(dir([])))
print("dir sorted", dir(t) == sorted(dir(t)))


class CustomDir:
    def __dir__(self):
        return ["b", "a", "c", "a"]


print("custom __dir__", dir(CustomDir()))


class BadDir:
    def __dir__(self):
        return 5


try:
    dir(BadDir())
except TypeError as e:
    print(type(e).__name__, e)

print("getattr", getattr(t, "x"), getattr(t, "missing", "dflt"), getattr(t, "prop"))
print("getattr default None", getattr(t, "missing", None))
try:
    getattr(t, "missing")
except AttributeError as e:
    print(type(e).__name__, e)
setattr(t, "x", 100)
print("setattr", t.x)
print("hasattr", hasattr(t, "x"), hasattr(t, "cls_attr"), hasattr(t, "base_method"), hasattr(t, "nope"))
delattr(t, "x")
print("delattr", hasattr(t, "x"), vars(t))
try:
    delattr(t, "cls_attr")
except AttributeError as e:
    print(type(e).__name__, e)
try:
    delattr(t, "prop")
except AttributeError as e:
    print(type(e).__name__, e)

for name in ["y", "cls_attr", "base_attr", "method"]:
    print("lookup", name, "in instance dict:", name in vars(t), "hasattr:", hasattr(t, name))

try:
    vars(5)
except TypeError as e:
    print(type(e).__name__, e)
try:
    vars(t, t)
except TypeError as e:
    print(type(e).__name__, e)


def f():
    pass


f.attr = 1
print("vars of function", vars(f))
print("vars of class has methods", "method" in vars(Thing), "base_method" in vars(Thing))
print("getattr on class", getattr(Thing, "cls_attr"), getattr(Base, "cls_attr", "absent"))


class Obj:
    pass


o = Obj()
for i in range(3):
    setattr(o, "attr" + str(i), i * i)
print("dynamic names", vars(o), [getattr(o, "attr" + str(i)) for i in range(3)])
o.__dict__.clear()
print("cleared", vars(o), hasattr(o, "attr0"))
print("dir of module-level names", [n for n in public(dir()) if n in ("Base", "Thing", "public", "o")])
print("getattr dunder", getattr(t, "__class__").__name__, getattr(Thing, "__name__"))
print("hasattr dunders", hasattr(t, "__dict__"), hasattr(1, "__dict__"), hasattr(Thing, "__mro__"))
