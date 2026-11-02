# Metaclasses: type(name, bases, ns), custom metaclass __new__/__init__/__call__/__prepare__,
# metaclass inheritance and conflicts.

Dyn = type("Dyn", (), {"x": 1, "double": lambda self: self.x * 2})
print("type 3-arg", Dyn.__name__, Dyn().double(), Dyn.__bases__ == (object,))
Sub = type("Sub", (Dyn,), {"x": 5})
print("type subclass", Sub().double(), [c.__name__ for c in Sub.__mro__])
print("type of class", type(Dyn) is type, type(type) is type, isinstance(Dyn, type))
for bad in [("N", 1, {}), ("N", (), 1), (1, (), {})]:
    try:
        type(*bad)
    except TypeError as e:
        print(type(e).__name__, e)
try:
    type("A", "B")
except TypeError as e:
    print(type(e).__name__, e)

log = []


class Meta(type):
    def __new__(mcls, name, bases, ns, **kwargs):
        log.append("new " + name + " " + repr(sorted(k for k in ns if not k.startswith("__"))))
        ns["injected"] = "by meta"
        return super().__new__(mcls, name, bases, ns)

    def __init__(cls, name, bases, ns, **kwargs):
        log.append("init " + name + " " + repr(kwargs))
        super().__init__(name, bases, ns)

    def __call__(cls, *args, **kwargs):
        log.append("call " + cls.__name__)
        instance = super().__call__(*args, **kwargs)
        instance.created_by_meta = True
        return instance

    def describe(cls):
        return "class " + cls.__name__


class Model(metaclass=Meta, flavor="x"):
    a = 1

    def __init__(self, v=0):
        self.v = v


m = Model(7)
print("meta log", log)
print("injected", Model.injected, m.created_by_meta, m.v)
print("meta method", Model.describe())
print("meta method not on instance", hasattr(m, "describe"))
print("type of class", type(Model).__name__, isinstance(Model, Meta))


class Child(Model):
    b = 2


print("inherited metaclass", type(Child).__name__, log[-2:])


class SingletonMeta(type):
    def __call__(cls, *args):
        if "_instance" not in cls.__dict__:
            cls._instance = super().__call__(*args)
        return cls._instance


class Config(metaclass=SingletonMeta):
    def __init__(self, n):
        self.n = n


c1, c2 = Config(1), Config(2)
print("singleton", c1 is c2, c2.n)


class OrderedMeta(type):
    @classmethod
    def __prepare__(mcls, name, bases):
        log.append("prepare " + name)
        return {"preset": "from prepare"}


class Prepared(metaclass=OrderedMeta):
    pass


print("prepare", Prepared.preset, log[-1])


class MetaA(type):
    pass


class MetaB(type):
    pass


class A(metaclass=MetaA):
    pass


class B(metaclass=MetaB):
    pass


try:

    class AB(A, B):
        pass

except TypeError as e:
    print(type(e).__name__, e)


class MetaASub(MetaA):
    pass


class C(metaclass=MetaASub):
    pass


class AC(A, C):
    pass


print("most derived metaclass", type(AC).__name__)


def function_meta(name, bases, ns):
    return "not a class: " + name


class Weird(metaclass=function_meta):
    pass


print("callable metaclass", Weird)


print("__class__ chain", m.__class__.__name__, m.__class__.__class__.__name__, type.__class__.__name__)
print("metaclass attr lookup", Model.__name__, Model.__mro__[0] is Model)
print("issubclass meta", issubclass(Meta, type), issubclass(type, object), isinstance(type, object))
