# Custom exceptions: args/str/repr, custom __init__ and __str__, attributes, exceptions raised in
# __init__, and builtin exception args formatting.


def show(e):
    print(" ", type(e).__name__, "args=" + repr(e.args), "str=" + repr(str(e)), "repr=" + repr(e))


print("builtin exceptions")
for e in [ValueError(), ValueError("one"), ValueError("a", 2), ValueError(None), KeyError("k"), KeyError(), Exception(1, [2])]:
    show(e)


class AppError(Exception):
    pass


print("plain subclass")
show(AppError())
show(AppError("msg"))
show(AppError("msg", 42))


class CodeError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


print("custom init calling super")
ce = CodeError(404, "not found")
show(ce)
print("  code attr", ce.code)


class NoSuperInit(Exception):
    def __init__(self, a, b):
        self.total = a + b


print("custom init without super")
ns = NoSuperInit(1, 2)
show(ns)
print("  total", ns.total)


class WithStr(Exception):
    def __init__(self, field):
        self.field = field
        super().__init__(field)

    def __str__(self):
        return "invalid field " + repr(self.field)


print("custom __str__")
show(WithStr("name"))


class WithRepr(Exception):
    def __repr__(self):
        return "<WithRepr custom>"


print("custom __repr__")
show(WithRepr("x"))

e = AppError("x")
e.args = ("changed", 1)
print("args assignable", e.args, str(e))
e.args = "abc"
print("args converted to tuple", e.args)
e.extra = "dynamic attribute"
print("dynamic attr", e.extra)


class Hierarchy(AppError):
    pass


try:
    raise Hierarchy("deep")
except AppError as caught:
    print("caught by base", type(caught).__name__, caught)


class BadInit:
    def __init__(self, value):
        if value < 0:
            raise ValueError("negative value: " + str(value))
        self.value = value


try:
    obj = BadInit(-1)
except ValueError as err:
    print("init raised", type(err).__name__, err)
print("name not bound after failed init", "obj" in globals())


class BadNew:
    def __new__(cls, *args):
        raise RuntimeError("no instances")


try:
    BadNew()
except RuntimeError as err:
    print("new raised", err)


class NotException:
    pass


try:
    raise NotException()
except TypeError as err:
    print(type(err).__name__, err)


class ExcWithDefault(Exception):
    def __init__(self, msg="default message"):
        super().__init__(msg)


try:
    raise ExcWithDefault
except ExcWithDefault as err:
    print("raise class with default", err)

try:
    raise CodeError
except TypeError as err:
    print("raise class needing args", type(err).__name__, err)

print("with_traceback returns self", e.with_traceback(None) is e)
print("exception equality is identity", ValueError(1) == ValueError(1))
print("exceptions are hashable", hash(e) == hash(e))
print("str of nested args", str(ValueError(("a", 1))), str(ValueError([])))
print("BaseException kwargs", end=" ")
try:
    ValueError(x=1)
except TypeError as err:
    print(type(err).__name__, err)
