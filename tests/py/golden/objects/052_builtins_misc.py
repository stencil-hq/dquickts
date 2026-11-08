# Misc builtins: callable, hash of ints/tuples, repr of builtins and classes, object()
# sentinels and identity via `is`, format(), type().


class Foo:
    pass


def func():
    pass


print("repr builtin functions", repr(len), repr(print), repr(isinstance), repr(sorted))
print("repr classes", repr(int), repr(str), repr(type), repr(object), repr(Foo))
print("repr exceptions classes", repr(ValueError), repr(BaseException))
print("repr None etc", repr(None), repr(NotImplemented), repr(...), repr(Ellipsis))
print("repr builtin types", repr(type(None)), repr(type(len)), repr(type(func)))
print("str of class", str(Foo), f"{int}")
print("method descriptors", repr(str.join), repr(int.__add__), repr(dict.fromkeys.__name__))
print("names", len.__name__, len.__qualname__, Foo.__name__, int.__qualname__)
print("builtin module", len.__module__, int.__module__, Foo.__module__)

print("callable", [callable(x) for x in (len, Foo, Foo(), func, 1, "s", print, type, lambda: 0)])


class WithCall:
    def __call__(self):
        return "called"


wc = WithCall()
print("callable instance", callable(wc), wc())
wc.__call__ = None
print("instance __call__ ignored", callable(wc), wc())

print("hash ints", hash(0), hash(1), hash(-1), hash(-2), hash(10**20), hash(2**61 - 1), hash(2**61))
print("hash bool float", hash(True), hash(1.0), hash(0.5), hash(-0.0), hash(1.5))
print("hash tuples", hash(()) == hash(()), hash((1, 2)) == hash((1, 2)), hash((1, 2)) == hash((2, 1)))
print("hash tuple values", hash((1, 2)), hash((0,)), hash(((1,), 2)))
print("hash frozenset eq", hash(frozenset([1, 2])) == hash(frozenset([2, 1])))
print("hash None callable", hash(None) == hash(None), hash(len) == hash(len))
for bad in ([], {}, set(), ([],)):
    try:
        hash(bad)
    except TypeError as e:
        print(type(e).__name__, e)

MISSING = object()
other = object()
print("sentinel identity", MISSING is MISSING, MISSING is other, MISSING == other, MISSING != other)
print("object type", type(MISSING).__name__, type(MISSING) is object)


def get(d, key, default=MISSING):
    if key in d:
        return d[key]
    if default is MISSING:
        return "no default given"
    return default


print("sentinel use", get({}, "k"), get({}, "k", None), get({"k": 1}, "k"))
try:
    MISSING.attr = 1
except AttributeError as e:
    print(type(e).__name__, e)

a = [1, 2]
b = a
c = [1, 2]
print("is vs ==", a is b, a is c, a == c, a is not c)
print("None singleton", None is None, type(None)() is None)
print("singletons", True is True, ... is Ellipsis, NotImplemented is NotImplemented, bool(1) is True)
x = "interned"
print("same object through names", x is x, (lambda: b)() is a)

print("format", format(42), format(42, "05d"), format(3.5, ".3e"), format("s", ">3"), format(True, "^6"))
print("format bin hex", format(10, "b"), format(10, "#x"), format(-10, "o"), format(1234.5678, ",.2f"))
print("format percent", format(0.125, ".1%"), format(5, "+"), format(-5, " "), format(5, " "))
try:
    format(1, "z")
except ValueError as e:
    print(type(e).__name__, e)
try:
    format("s", "d")
except ValueError as e:
    print(type(e).__name__, e)

print("type one arg", type(1), type("s"), type(Foo()), type(Foo))
T = type("T", (Foo,), {"attr": 5, "m": lambda self: self.attr * 2})
print("type three arg", T, T.__mro__, T().m(), T.__qualname__)
print("type eq", type(1) == int, type(True) == int, type(T()) is T)
print("isinstance basics", isinstance(T(), Foo), isinstance(1, (str, int)), isinstance(T, type))
print("ascii", ascii("héllo"), ascii(["€"]), ascii(1))
print("bool", bool(), bool(0), bool("0"), bool([]), bool([0]), bool(None), bool(0.0), bool(0j))
