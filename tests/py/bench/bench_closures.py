# Closures: nonlocal counters, closure factories, nested scopes, lambdas,
# decorators implemented with closures.
import sys
import time


def make_counter(step):
    count = 0

    def inc():
        nonlocal count
        count += step
        return count

    return inc


def make_adder(n):
    return lambda x: x + n


def compose(f, g):
    def h(x):
        return f(g(x))
    return h


def memo(fn):
    cache = {}
    hits = 0

    def wrapper(x):
        nonlocal hits
        if x in cache:
            hits += 1
            return cache[x]
        r = fn(x)
        cache[x] = r
        return r

    wrapper.stats = lambda: (hits, len(cache))
    return wrapper


def outer(a):
    def middle(b):
        def inner(c):
            nonlocal a
            a += 1
            return a + b + c
        return inner
    return middle


def run(n):
    counters = [make_counter(k) for k in range(1, 6)]
    adders = [make_adder(k) for k in range(5)]
    pipeline = compose(adders[1], compose(adders[2], adders[3]))

    @memo
    def slow_sq(x):
        return x * x

    acc = 0
    inner = outer(10)(20)
    for i in range(n):
        acc += counters[i % 5]()
        acc += adders[i % 5](i)
        acc += pipeline(i)
        acc += slow_sq(i % 500)
        acc += inner(i & 7)
    return f"{acc} {slow_sq.stats()}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 270000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
