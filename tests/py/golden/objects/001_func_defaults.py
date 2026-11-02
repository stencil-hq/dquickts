# Function default values: evaluated once at definition time, mutable default gotcha,
# __defaults__ / __kwdefaults__ introspection and mutation.

calls = []


def make_default(tag):
    calls.append(tag)
    return tag


def f(a, b=make_default("b"), *, c=make_default("c")):
    return (a, b, c)


print("defaults evaluated at def", calls)
print("call 1", f(1))
print("call 2", f(1, 2, c=3))
print("no re-evaluation", calls)


def append_to(x, acc=[]):
    acc.append(x)
    return acc


print("gotcha 1", append_to(1))
print("gotcha 2", append_to(2))
print("gotcha explicit", append_to(3, []))
print("gotcha 3", append_to(4))
print("shared default", append_to.__defaults__)


def append_safe(x, acc=None):
    if acc is None:
        acc = []
    acc.append(x)
    return acc


print("safe 1", append_safe(1))
print("safe 2", append_safe(2))

print("__defaults__ f", f.__defaults__)
print("__kwdefaults__ f", f.__kwdefaults__)


def g(a, b):
    return a + b


print("__defaults__ none", g.__defaults__)
print("__kwdefaults__ none", g.__kwdefaults__)

g.__defaults__ = (10,)
print("patched defaults", g(1))
g.__defaults__ = (100, 200)
print("patched both", g())
g.__defaults__ = None
try:
    g()
except TypeError as e:
    print(type(e).__name__, e)

try:
    g.__defaults__ = [1]
except TypeError as e:
    print(type(e).__name__, e)


def h(*, k=1, m):
    return k, m


print("kw only defaults", h.__kwdefaults__)
h.__kwdefaults__ = {"k": 5, "m": 6}
print("patched kwdefaults", h())
h.__kwdefaults__ = None
try:
    h()
except TypeError as e:
    print(type(e).__name__, e)

counter = 0


def bump():
    global counter
    counter += 1
    return counter


def uses_global_default(x=bump()):
    return x


print("global default", uses_global_default(), uses_global_default(), counter)

x = 5


def captures_value(v=x):
    return v


x = 99
print("default captured value", captures_value(), x)


def dict_default(key, d={}):
    d[key] = d.get(key, 0) + 1
    return d


dict_default("a")
dict_default("b")
print("dict default", dict_default("a"))
print("lambda default", (lambda p, q=[1, 2]: p + len(q))(3))
print("defaults are tuple", type(f.__defaults__).__name__)
