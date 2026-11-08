# Unpacking in calls: f(*args), f(**kw), multiple unpackings, errors.

def show(*args, **kwargs):
    return "args=" + repr(args) + " kwargs=" + repr(kwargs)

def abc(a, b, c=3):
    return (a, b, c)

print("star list", abc(*[1, 2]))
print("star tuple full", abc(*(1, 2, 30)))
print("star str", abc(*"xy"))
print("star range", abc(*range(3)))
print("star generator", abc(*(i * i for i in range(3))))
print("mixed positional", abc(1, *[2]))
print("star then positional", abc(*[1], 2))
print("two stars", abc(*[1], *[2], *[]))
print("kw unpack", abc(**{"a": 1, "b": 2}))
print("kw and star", abc(*[1], **{"c": 9, "b": 8}))
print("kw after star", abc(*[1, 2], c="kw"))
print("show plain", show(1, 2, x=3))
print("show stars", show(*[1, 2], *"ab", **{"k": 1}, **{"j": 2}))
print("show kw between", show(1, *[2], z=0, **{"y": 1}))
print("show empty", show(*[], **{}))
print("show dict keys star", show(*{"p": 1, "q": 2}))

# keyword unpacking with explicit kw interleaved (order preserved)
print("kw order", show(b=1, **{"a": 2}, c=3))

# errors
def attempt(label, fn):
    try:
        print(label, fn())
    except TypeError as e:
        print(label, type(e).__name__, e)

attempt("too many via star", lambda: abc(*[1, 2, 3, 4]))
attempt("too few via star", lambda: abc(*[1]))
attempt("duplicate kw", lambda: abc(1, 2, **{"a": 5}))
attempt("duplicate kw two dicts", lambda: show(**{"x": 1}, **{"x": 2}))
attempt("unexpected kw", lambda: abc(1, 2, **{"d": 4}))
attempt("non-str kw key", lambda: show(**{1: 2}))
attempt("star non-iterable", lambda: show(*5))
attempt("star None", lambda: show(*None))
attempt("kw non-mapping", lambda: show(**[1]))
attempt("kw None", lambda: show(**None))

# custom mapping for **
class Mapping:
    def keys(self):
        return ["a", "b"]

    def __getitem__(self, key):
        return key * 2

print("custom mapping", abc(**Mapping()))

# custom iterable for *
class Iter:
    def __iter__(self):
        yield "i1"
        yield "i2"

print("custom iterable", abc(*Iter()))

# builtins with unpacking
nums = [3, 1, 2]
print("max star", max(*nums))
print("print star:", *nums)
print("print star sep:", *nums, sep="-")
print("zip star transpose", list(zip(*[(1, "a"), (2, "b"), (3, "c")])))
print("dict from kw", dict(**{"a": 1}, b=2))
print("format kw", "{x}-{y}".format(**{"x": 1, "y": 2}))
print("str.join star", "+".join([*"abc"]))

# args tuple and kwargs dict are fresh objects
def capture(*args, **kwargs):
    return args, kwargs

lst = [1, 2]
kw = {"k": 1}
args, kwargs = capture(*lst, **kw)
kwargs["new"] = True
print("fresh objects", type(args).__name__, args, kw, kwargs)

# nested call unpacking
def inner(*a, **k):
    return show(*a, **k)

def outer(*a, **k):
    return inner(*a, extra=True, **k)

print("forwarding", outer(1, 2, mode="x"))

# evaluation order: positional, star, keyword, double-star in source order
log = []
def f(tag, v):
    log.append(tag)
    return v

show(f("p1", 1), *f("s", [2]), k=f("kw", 3), **f("ds", {"z": 4}))
print("eval order", log)

# methods with unpacking
class C:
    def m(self, x, y):
        return x - y

print("method star", C().m(*[10, 3]), C().m(**{"y": 1, "x": 5}))
print("unbound star", C.m(*[C(), 7, 2]))
