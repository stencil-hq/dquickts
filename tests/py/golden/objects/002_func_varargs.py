# *args / **kwargs collection, ordering and unpacking at call sites.


def collect(*args, **kwargs):
    return args, kwargs


print("empty", collect())
print("positional", collect(1, 2, 3))
print("keywords order", collect(z=1, a=2, m=3))
print("mixed", collect(1, "two", key=None))
print("args type", type(collect(1)[0]).__name__)
print("kwargs type", type(collect(a=1)[1]).__name__)


def mixed(a, b=2, *rest, c, d=4, **extra):
    return a, b, rest, c, d, extra


print("mixed 1", mixed(1, c=3))
print("mixed 2", mixed(1, 2, 3, 4, 5, c=6))
print("mixed 3", mixed(1, c=3, d=5, e=6, f=7))
print("mixed 4", mixed(c=0, a=9, zz=1))

args = (1, 2, 3)
kwargs = {"x": 10, "y": 20}
print("unpack call", collect(*args, **kwargs))
print("multi unpack", collect(*args, *"ab", 0, **kwargs, **{"w": 1}))
print("unpack range", collect(*range(3)))
print("unpack dict keys", collect(*{"k1": 1, "k2": 2}))
print("unpack gen", collect(*(i * i for i in range(4))))

try:
    collect(**{"x": 1}, **{"x": 2})
except TypeError as e:
    print(type(e).__name__, e)

try:
    collect(**{1: 2})
except TypeError as e:
    print(type(e).__name__, e)

try:
    collect(*5)
except TypeError as e:
    print(type(e).__name__, e)

try:
    collect(**[1])
except TypeError as e:
    print(type(e).__name__, e)


def kw_names(**kw):
    return sorted(kw)


print("kw sorted", kw_names(b=1, a=2, c=3))
print("kw odd keys", collect(**{"not an identifier": 1, "class": 2}))


def first_and_rest(first, *rest):
    return first, rest


print("first rest", first_and_rest(1))
print("first rest 2", first_and_rest(1, 2, 3))
try:
    first_and_rest()
except TypeError as e:
    print(type(e).__name__, e)


def mutate_kwargs(**kw):
    kw["added"] = True
    return kw


source = {"a": 1}
result = mutate_kwargs(**source)
print("kwargs is a copy", source, result)


def mutate_args(*a):
    return a + ("extra",)


src = [1, 2]
print("args from list", mutate_args(*src), src)


def defaults_and_star(a, b=10, *args):
    return a, b, args


print("defaults star 1", defaults_and_star(1))
print("defaults star 2", defaults_and_star(1, 2, 3))


def passthrough(*args, **kwargs):
    return collect(*args, **kwargs)


print("passthrough", passthrough(1, 2, k="v"))
print("dict order kept", list(collect(c=3, b=2, a=1)[1].items()))
print("star in list", [*args, *kwargs])
print("star in dict", {**kwargs, "x": 0})
print("star in tuple", (*"hi", *[1]))
print("star in set sorted", sorted({*args, *[3, 4]}))
