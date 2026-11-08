# __setattr__ / __delattr__ hooks, frozen objects, setattr/delattr builtins and their errors.


class Logged:
    def __init__(self):
        self.log = []
        self.a = 1

    def __setattr__(self, name, value):
        if name != "log":
            self.log.append("set " + name + "=" + repr(value))
        object.__setattr__(self, name, value)

    def __delattr__(self, name):
        self.log.append("del " + name)
        object.__delattr__(self, name)


obj = Logged()
obj.b = [2]
obj.a += 10
setattr(obj, "c", "three")
del obj.b
delattr(obj, "c")
print("log", obj.log)
print("dict", sorted(obj.__dict__))
try:
    del obj.zzz
except AttributeError as e:
    print(type(e).__name__, e)


class Frozen:
    def __init__(self, x):
        object.__setattr__(self, "x", x)

    def __setattr__(self, name, value):
        raise AttributeError("Frozen is immutable: " + name)

    def __delattr__(self, name):
        raise AttributeError("cannot delete " + name)


fz = Frozen(5)
print("frozen read", fz.x)
for action in (lambda: setattr(fz, "x", 6), lambda: delattr(fz, "x")):
    try:
        action()
    except AttributeError as e:
        print(type(e).__name__, e)
object.__setattr__(fz, "x", 7)
print("bypass", fz.x)


class Validated:
    def __setattr__(self, name, value):
        if name == "age" and not isinstance(value, int):
            raise TypeError("age must be int, got " + type(value).__name__)
        super().__setattr__(name, value)


v = Validated()
v.age = 3
try:
    v.age = "old"
except TypeError as e:
    print(type(e).__name__, e)
print("validated", v.age)


class Plain:
    pass


p = Plain()
setattr(p, "dynamic", 1)
setattr(p, "with space", 2)
print("setattr weird name", getattr(p, "with space"), sorted(vars(p)))
delattr(p, "dynamic")
print("hasattr after delattr", hasattr(p, "dynamic"))
for bad in [lambda: setattr(p, 1, 2), lambda: delattr(p, None), lambda: getattr(p)]:
    try:
        bad()
    except TypeError as e:
        print(type(e).__name__, e)
for target in [5, "s", None, [1]]:
    try:
        setattr(target, "x", 1)
    except AttributeError as e:
        print(type(e).__name__, e)
try:
    object().x = 1
except AttributeError as e:
    print(type(e).__name__, e)
try:
    int.new_attr = 1
except TypeError as e:
    print(type(e).__name__, e)
try:
    del Plain.missing
except AttributeError as e:
    print(type(e).__name__, e)

Plain.shared = "on class"
print("class setattr", p.shared)
delattr(Plain, "shared")
print("class delattr", hasattr(p, "shared"))


class DictWriter:
    def __setattr__(self, name, value):
        self.__dict__[name] = value * 2


s = DictWriter()
s.n = 21
print("setattr through __dict__", s.n)


class Counter:
    sets = 0

    def __setattr__(self, name, value):
        type(self).sets += 1
        super().__setattr__(name, value)


cnt = Counter()
cnt.a = 1
cnt.a = 2
cnt.b = 3
print("count sets", Counter.sets, cnt.a, cnt.b)
print("dict assignment", end=" ")
cnt.__dict__ = {"replaced": True}
print(sorted(vars(cnt)), Counter.sets)
try:
    cnt.__dict__ = 5
except TypeError as e:
    print(type(e).__name__, e)
