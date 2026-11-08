# isinstance / issubclass with tuples and unions, type() identity checks, __class__,
# __instancecheck__ / __subclasscheck__ hooks.


class Animal:
    pass


class Dog(Animal):
    pass


class Cat(Animal):
    pass


d = Dog()
print("isinstance basic", isinstance(d, Dog), isinstance(d, Animal), isinstance(d, Cat))
print("isinstance tuple", isinstance(d, (Cat, Dog)), isinstance(d, (int, str)))
print("nested tuple", isinstance(d, (int, (str, (Animal,)))))
print("empty tuple", isinstance(d, ()))
print("union", isinstance(1, int | str), isinstance(1.0, int | str), isinstance(None, int | None))
print("issubclass tuple", issubclass(Dog, (Cat, Animal)), issubclass(Dog, (int,)))
print("issubclass self", issubclass(Dog, Dog), issubclass(Dog, object))
print("type is", type(d) is Dog, type(d) is Animal, type(d) == Dog)
print("__class__", d.__class__ is Dog, d.__class__.__name__)
print("builtins", isinstance(True, int), issubclass(bool, int), type(True) is int)
print("object", isinstance(None, object), isinstance(int, object), isinstance(object, type))
print("type of type", type(int), type(type), type(None))
print("NoneType name", type(None).__name__, type(...).__name__, type(NotImplemented).__name__)

print("tuple short-circuits", isinstance(d, (Dog, 5)))
for args in [(d, 5), (d, "Dog"), (d, (Cat, 5))]:
    try:
        isinstance(*args)
    except TypeError as e:
        print(type(e).__name__, e)
for args in [(d, Dog), (Dog, 5)]:
    try:
        issubclass(*args)
    except TypeError as e:
        print(type(e).__name__, e)


class Meta(type):
    def __instancecheck__(cls, obj):
        return isinstance(obj, int) and obj > 0

    def __subclasscheck__(cls, sub):
        return sub.__name__.startswith("Pos")


class Positive(metaclass=Meta):
    pass


class PosThing:
    pass


print("instancecheck hook", isinstance(5, Positive), isinstance(-5, Positive), isinstance("x", Positive))
print("subclasscheck hook", issubclass(PosThing, Positive), issubclass(int, Positive))
print("hook in tuple", isinstance(3, (str, Positive)))


class Changeling:
    pass


class Other:
    def kind(self):
        return "other"


ch = Changeling()
ch.__class__ = Other
print("class assignment", type(ch).__name__, ch.kind(), isinstance(ch, Changeling))
try:
    ch.__class__ = int
except TypeError as e:
    print(type(e).__name__, e)
try:
    (5).__class__ = bool
except TypeError as e:
    print(type(e).__name__, e)


class FakeClass:
    @property
    def __class__(self):
        return Dog


fc = FakeClass()
print("fake __class__", isinstance(fc, Dog), type(fc) is FakeClass, fc.__class__.__name__)
print("type names", [type(x).__name__ for x in (1, 1.0, 1j, "s", b"b", [], (), {}, set(), None, range(1))])
print("bool subclass of int", isinstance(False, (float,)), isinstance(False, (int,)))
nums = [1, 2.5, "3", True, None, 4]
print("filter ints", [x for x in nums if isinstance(x, int) and not isinstance(x, bool)])
print("exact type check", [x for x in nums if type(x) is int])
print("callable checks", callable(type), callable(isinstance), callable(Dog()), callable(Dog))
print("type is instance of itself", isinstance(type, type), isinstance(object, object))
print("issubclass union", issubclass(bool, int | str), issubclass(float, int | str))
print("union repr", int | str, int | None, (int | str) == (str | int))
