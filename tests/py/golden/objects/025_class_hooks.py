# Class-level hooks: class decorators, __init_subclass__, __class_getitem__, __set_name__ order,
# __prepare__-free namespace inspection.

registry = {}


def register(cls):
    registry[cls.__name__] = cls
    return cls


def add_method(name, value):
    def decorator(cls):
        setattr(cls, name, lambda self: value)
        return cls

    return decorator


@register
@add_method("greet", "hello")
class Widget:
    pass


print("class decorator", sorted(registry), Widget().greet())


class Plugin:
    plugins = []

    def __init_subclass__(cls, /, prefix="", **kwargs):
        super().__init_subclass__(**kwargs)
        cls.full_name = prefix + cls.__name__
        Plugin.plugins.append(cls.full_name)


class Alpha(Plugin):
    pass


class Beta(Plugin, prefix="x-"):
    pass


class Gamma(Beta):
    pass


print("init_subclass", Plugin.plugins)
print("full names", Alpha.full_name, Beta.full_name, Gamma.full_name)
print("base untouched", hasattr(Plugin, "full_name"))

try:

    class Wrong(Plugin, unknown=1):
        pass

except TypeError as e:
    print(type(e).__name__, e)

try:

    class NoHook(unknown=1):
        pass

except TypeError as e:
    print(type(e).__name__, e)


class Strict:
    def __init_subclass__(cls):
        if not hasattr(cls, "required"):
            raise TypeError(cls.__name__ + " lacks 'required'")


class Good(Strict):
    required = True


try:

    class Bad(Strict):
        pass

except TypeError as e:
    print(type(e).__name__, e)
print("init_subclass is classmethod", type(Strict.__dict__["__init_subclass__"]).__name__)


class Box:
    def __class_getitem__(cls, item):
        return cls.__name__ + "[" + repr(item) + "]"


print("class_getitem", Box[int], Box["x"], Box[int, str])
print("class_getitem implicit classmethod", type(Box.__dict__["__class_getitem__"]).__name__)
try:
    Box()[0]
except TypeError as e:
    print(type(e).__name__, e)
print("builtin generic alias", list[int], dict[str, list[int]], tuple[int, ...])
print("alias origin args", list[int].__origin__ is list, dict[str, int].__args__)
try:
    int[str]
except TypeError as e:
    print(type(e).__name__, e)

order = []


class Field:
    def __init__(self, tag):
        self.tag = tag

    def __set_name__(self, owner, name):
        order.append(self.tag + "->" + name)


def deco(cls):
    order.append("decorator " + cls.__name__)
    return cls


class Base:
    def __init_subclass__(cls, **kw):
        order.append("init_subclass " + cls.__name__)


@deco
class Model(Base):
    order.append("body start")
    first = Field("f1")
    second = Field("f2")
    order.append("body end")


print("creation order", order)
print("namespace order", [k for k in Model.__dict__ if not k.startswith("__")])
print("dunder module qualname", Model.__module__, Model.__qualname__)
print("firstlineno is int", type(Model.__firstlineno__).__name__)
print("static attributes", Plugin.__static_attributes__, Field.__static_attributes__)
