# Descriptor protocol: __get__/__set__/__delete__/__set_name__, data vs non-data descriptor
# precedence relative to the instance __dict__, and functions as descriptors.

events = []


class Typed:
    def __set_name__(self, owner, name):
        events.append("set_name " + owner.__name__ + "." + name)
        self.name = name
        self.storage = "_" + name

    def __init__(self, kind):
        self.kind = kind

    def __get__(self, instance, owner=None):
        if instance is None:
            return self
        return getattr(instance, self.storage, "<unset>")

    def __set__(self, instance, value):
        if not isinstance(value, self.kind):
            raise TypeError(self.name + " must be " + self.kind.__name__)
        setattr(instance, self.storage, value)

    def __delete__(self, instance):
        events.append("delete " + self.name)
        delattr(instance, self.storage)


class Person:
    name = Typed(str)
    age = Typed(int)

    def __init__(self, name, age):
        self.name = name
        self.age = age


print("set_name events", events)
pp = Person("ann", 30)
print("get", pp.name, pp.age)
try:
    pp.age = "thirty"
except TypeError as e:
    print(type(e).__name__, e)
print("class access returns descriptor", type(Person.age).__name__, Person.age.name)
print("storage in dict", sorted(vars(pp)))
del pp.age
print("after delete", pp.age, events[-1])


class NonData:
    def __get__(self, instance, owner):
        return "from non-data descriptor"


class Data:
    def __get__(self, instance, owner):
        return "from data descriptor"

    def __set__(self, instance, value):
        raise AttributeError("read-only data descriptor")


class Holder:
    nd = NonData()
    d = Data()


h = Holder()
print("before dict", h.nd, h.d)
h.__dict__["nd"] = "instance wins over non-data"
h.__dict__["d"] = "instance loses to data"
print("precedence", h.nd, "|", h.d)
h.nd = "assign shadows non-data"
print("assign non-data", h.nd)
try:
    h.d = 1
except AttributeError as e:
    print(type(e).__name__, e)


class GetOnlyWithDelete:
    def __get__(self, instance, owner):
        return "get"

    def __delete__(self, instance):
        print("delete hook")


class H2:
    attr = GetOnlyWithDelete()


h2 = H2()
h2.__dict__["attr"] = "dict"
print("__delete__ makes it data", h2.attr)
del h2.attr


class Owner:
    def __get__(self, instance, owner):
        return (instance is None, owner.__name__)


class UsesOwner:
    o = Owner()


class SubOwner(UsesOwner):
    pass


print("owner arg", UsesOwner.o, UsesOwner().o, SubOwner.o)


def func(self):
    return "func called on " + type(self).__name__


class F:
    pass


bound = func.__get__(F(), F)
print("function __get__", bound(), type(bound).__name__)
print("function __get__ None", func.__get__(None, F) is func)
print("staticmethod __get__", staticmethod(func).__get__(None, F) is func)
print("classmethod __get__", classmethod(func).__get__(None, F)())


class BadSetName:
    def __set_name__(self, owner, name):
        raise ValueError("refused " + name)


try:

    class UsesBad:
        attr = BadSetName()

except ValueError as e:
    print(type(e).__name__, e, "note" if hasattr(e, "__notes__") else "no note")
