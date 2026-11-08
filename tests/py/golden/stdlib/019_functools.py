"""L0 golden: functools reduce/partial/lru_cache/wraps/cmp_to_key."""
import functools
import operator

print("reduce", functools.reduce(operator.add, [1, 2, 3, 4]))
print("reduce initial", functools.reduce(operator.mul, [], 1), functools.reduce(lambda a, b: a + [b], "ab", []))
print("reduce str", functools.reduce(lambda a, b: b + a, "abc"))
try:
    functools.reduce(operator.add, [])
except TypeError as e:
    print("reduce empty", type(e).__name__, e)

# partial
def power(base, exp, *, mod=None):
    return pow(base, exp, mod)


square = functools.partial(power, exp=2)
print("partial", square(7), square(3, exp=3))
print("partial attrs", square.func is power, square.args, square.keywords)
cube_mod = functools.partial(power, exp=3, mod=10)
print("partial mod", cube_mod(7))
base2 = functools.partial(power, 2)
print("partial positional", base2(10), functools.partial(base2, 3)())
print("partial nested flatten", functools.partial(base2, 5).args)
print("partial placeholder", functools.partial(power, functools.Placeholder, 3)(2))
print("partial sorted", sorted(["bb", "a", "ccc"], key=functools.partial(len)))

# lru_cache
calls = []


@functools.lru_cache(maxsize=2)
def slow(n):
    calls.append(n)
    return n * 10


print("lru", slow(1), slow(2), slow(1), slow(3), slow(2))
print("lru calls", calls)
print("cache_info", slow.cache_info())
slow.cache_clear()
print("cleared", slow.cache_info())
print("cache_parameters", slow.cache_parameters())


@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


print("fib 80", fib(80), fib.cache_info())


@functools.cache
def typed_free(x):
    return type(x).__name__


print("cache 1 vs 1.0", typed_free(1), typed_free(1.0), typed_free.cache_info().currsize)
print("wrapped", fib.__wrapped__.__name__)
try:
    slow([1])
except TypeError as e:
    print("unhashable arg", type(e).__name__, e)

# wraps
def logged(fn):
    @functools.wraps(fn)
    def inner(*args, **kwargs):
        return ("logged", fn(*args, **kwargs))
    return inner


@logged
def add(a, b):
    """Add two numbers."""
    return a + b


print("wraps", add.__name__, add.__doc__, add.__qualname__, add(1, 2))
print("wraps wrapped", add.__wrapped__(1, 2))
print("WRAPPER_ASSIGNMENTS", functools.WRAPPER_ASSIGNMENTS)

# cmp_to_key
def by_len_then_rev(a, b):
    if len(a) != len(b):
        return len(a) - len(b)
    return (a < b) - (a > b)


print("cmp_to_key", sorted(["bb", "a", "ccc", "aa", "c"], key=functools.cmp_to_key(by_len_then_rev)))
key = functools.cmp_to_key(by_len_then_rev)
print("cmp_to_key objects", key("ab") < key("abc"), key("b") < key("a"), key("x") == key("x"))
print("reduce max", functools.reduce(lambda a, b: a if a >= b else b, [3, 9, 2]))
