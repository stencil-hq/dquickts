# Higher-order functions, composition, partial application and memoization without imports.


def compose(*funcs):
    def composed(x):
        for f in reversed(funcs):
            x = f(x)
        return x

    return composed


inc = lambda x: x + 1
dbl = lambda x: x * 2
print("compose", compose(inc, dbl)(5), compose(dbl, inc)(5), compose()(7))


def partial(func, *bound, **bound_kw):
    def call(*args, **kwargs):
        return func(*bound, *args, **{**bound_kw, **kwargs})

    return call


def power(base, exp):
    return base**exp


square = partial(power, exp=2)
two_to = partial(power, 2)
print("partial", square(9), two_to(10), square(3, exp=3))

print("map", list(map(inc, [1, 2, 3])))
print("map two seqs", list(map(power, [2, 3, 4], [3, 2])))
print("filter", list(filter(lambda x: x % 2, range(10))))
print("filter None", list(filter(None, [0, 1, "", "a", None, [], [0]])))
print("sorted key", sorted(["bb", "a", "ccc"], key=len))
print("max key", max(["bb", "a", "ccc"], key=len))


def reduce(func, items, initial):
    acc = initial
    for item in items:
        acc = func(acc, item)
    return acc


print("reduce sum", reduce(lambda a, b: a + b, range(5), 0))
print("reduce concat", reduce(lambda a, b: b + a, "abc", ""))

calls = []


def memoize(func):
    cache = {}

    def wrapper(*args):
        if args not in cache:
            cache[args] = func(*args)
        return cache[args]

    wrapper.cache = cache
    return wrapper


@memoize
def slow_fib(n):
    calls.append(n)
    return n if n < 2 else slow_fib(n - 1) + slow_fib(n - 2)


print("memo fib 40", slow_fib(40))
print("memo calls", len(calls))
print("memo cache size", len(slow_fib.cache))
print("memo hit", slow_fib(40), len(calls))

try:
    memoize(len)([1])
except TypeError as e:
    print(type(e).__name__, e)


def curry(func, arity):
    def step(*collected):
        if len(collected) >= arity:
            return func(*collected)
        return lambda *more: step(*collected, *more)

    return step


add3 = curry(lambda a, b, c: a + b + c, 3)
print("curry", add3(1)(2)(3), add3(1, 2)(3), add3(1, 2, 3))


def pipeline(value, *steps):
    for step in steps:
        value = step(value)
    return value


print("pipeline", pipeline("  Hello World ", str.strip, str.lower, str.split))
print("unbound method as func", list(map(str.upper, ["a", "b"])))
print("builtin as func", list(map(len, ["", "ab", "abc"])))
print("type as func", list(map(int, ["1", "22"])), list(map(str, [1, 2])))


def apply_n(f, n):
    return lambda x: x if n == 0 else apply_n(f, n - 1)(f(x))


print("apply_n", apply_n(dbl, 10)(1))
fs = {"inc": inc, "dbl": dbl}
print("dispatch table", [fs[name](10) for name in ("inc", "dbl", "inc")])
print("immediately invoked", (lambda *a, **k: (a, k))(1, z=2))
print("lambda conditional", (lambda x: "neg" if x < 0 else "nonneg")(-1))
