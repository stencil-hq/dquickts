# __getattr__ vs __getattribute__: fallback-only vs every lookup, AttributeError interplay.


class Fallback:
    existing = "class attr"

    def __init__(self):
        self.inst = "instance attr"

    def __getattr__(self, name):
        if name.startswith("dyn_"):
            return "dynamic " + name[4:]
        raise AttributeError("no attribute " + repr(name))


f = Fallback()
print("normal lookup", f.existing, f.inst)
print("fallback", f.dyn_x, getattr(f, "dyn_y"))
print("hasattr", hasattr(f, "dyn_z"), hasattr(f, "other"))
try:
    f.other
except AttributeError as e:
    print(type(e).__name__, e)
print("getattr default", getattr(f, "other", "default"))
print("explicit dunder lookup uses getattr", hasattr(f, "__len__"))
try:
    len(f)
except TypeError as e:
    print("implicit dunder skips getattr", type(e).__name__, e)

log = []


class Everything:
    def __init__(self):
        self.real = 1

    def __getattribute__(self, name):
        log.append(name)
        if name == "secret":
            return "intercepted"
        return object.__getattribute__(self, name)

    def method(self):
        return self.real


e = Everything()
log.clear()
print("intercept", e.secret, e.real, e.method())
print("logged", log)
try:
    e.missing
except AttributeError as ex:
    print(type(ex).__name__, ex)


class Both:
    def __getattribute__(self, name):
        if name == "boom":
            raise AttributeError("from getattribute")
        return object.__getattribute__(self, name)

    def __getattr__(self, name):
        return "getattr saw " + name


b = Both()
print("getattr after AttributeError", b.boom, b.missing)


class Proxy:
    def __init__(self, target):
        object.__setattr__(self, "_target", target)

    def __getattr__(self, name):
        return getattr(self._target, name)


class Target:
    color = "red"

    def shout(self, s):
        return s.upper()


px = Proxy(Target())
print("proxy", px.color, px.shout("hi"))
try:
    px.nothing
except AttributeError as ex:
    print(type(ex).__name__, ex)


class Recursive:
    def __getattr__(self, name):
        if name == "loop":
            return self.loop2
        if name == "loop2":
            return "resolved"
        raise AttributeError(name)


print("chained getattr", Recursive().loop)


class ErrorInGetattr:
    def __getattr__(self, name):
        raise KeyError(name)


try:
    ErrorInGetattr().x
except KeyError as ex:
    print("non-AttributeError propagates", type(ex).__name__, ex)
try:
    hasattr(ErrorInGetattr(), "x")
except KeyError as ex:
    print("hasattr propagates", type(ex).__name__, ex)
print("getattr default only for AttributeError", getattr(Fallback(), "z", None))


class ClassLevel:
    pass


ClassLevel.__getattr__ = lambda self, name: "patched " + name
print("patched getattr", ClassLevel().foo)
print("int no attribute", end=" ")
try:
    (1).x
except AttributeError as ex:
    print(type(ex).__name__, ex)
try:
    None.foo
except AttributeError as ex:
    print(type(ex).__name__, ex)
try:
    int.foo
except AttributeError as ex:
    print(type(ex).__name__, ex)
print("AttributeError name obj", end=" ")
try:
    "s".nope
except AttributeError as ex:
    print(ex.name, ex.obj)
