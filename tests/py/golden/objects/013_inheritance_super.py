# Inheritance, method overriding, super() in zero-arg and two-arg forms.


class Animal:
    sound = "..."

    def __init__(self, name):
        self.name = name

    def speak(self):
        return self.name + " says " + self.sound

    def kind(self):
        return "animal"

    def describe(self):
        return self.kind() + ":" + self.name


class Dog(Animal):
    sound = "woof"

    def kind(self):
        return "dog"


class Puppy(Dog):
    def __init__(self, name, age):
        super().__init__(name)
        self.age = age

    def speak(self):
        return super().speak() + " (tiny)"

    def kind(self):
        return "puppy/" + super().kind()


a = Animal("generic")
d = Dog("rex")
p = Puppy("bit", 1)
print("base", a.speak(), a.describe())
print("override attr", d.speak(), d.describe())
print("super chain", p.speak(), p.describe(), p.age)
print("isinstance", isinstance(p, Dog), isinstance(p, Animal), isinstance(d, Puppy))
print("issubclass", issubclass(Puppy, Animal), issubclass(Animal, Dog), issubclass(Dog, Dog))


class TwoArg(Dog):
    def kind(self):
        return "two-arg/" + super(TwoArg, self).kind() + "/" + super(Dog, self).kind()


print("two-arg super", TwoArg("t").kind())


class Static(Animal):
    @classmethod
    def make(cls):
        return super().__new__(cls)

    @classmethod
    def parent_kind(cls):
        return super(Static, cls).kind


obj = Static.make()
print("super in classmethod", type(obj).__name__, hasattr(obj, "name"))
print("unbound via super", Static.parent_kind()(Static("s")))

sp = super(Puppy, p)
print("super object attr", sp.sound, sp.kind())
print("super type name", type(sp).__name__)


def outside():
    return super().kind()


try:
    outside()
except RuntimeError as e:
    print(type(e).__name__, e)

try:
    super(Dog, 5)
except TypeError as e:
    print(type(e).__name__, e)


class NoInit(Animal):
    pass


print("inherited init", NoInit("n").name)
try:
    NoInit()
except TypeError as e:
    print(type(e).__name__, e)


class Overridden(Animal):
    def __init__(self):
        pass


o = Overridden()
print("init not chained", hasattr(o, "name"))


class Shape:
    def area(self):
        raise NotImplementedError(type(self).__name__ + ".area")


class Rect(Shape):
    def __init__(self, w, h):
        self.w, self.h = w, h

    def area(self):
        return self.w * self.h


print("abstract-ish", Rect(2, 3).area())
try:
    Shape().area()
except NotImplementedError as e:
    print(type(e).__name__, e)


class Attr(Dog):
    def get(self):
        return super().sound


print("super class attr", Attr("x").get())
try:
    super(Attr, Attr("x")).missing
except AttributeError as e:
    print(type(e).__name__, e)
print("__class__ cell", Puppy.speak.__code__.co_freevars)
print("subclasses", [c.__name__ for c in Animal.__subclasses__()])
