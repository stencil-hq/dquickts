# Generators: yield, yield from, generator pipelines, generator expressions.
import sys
import time


def count_up(n):
    i = 0
    while i < n:
        yield i
        i += 1


def evens(it):
    for x in it:
        if x % 2 == 0:
            yield x


def squares(it):
    for x in it:
        yield x * x


def chunked(it, size):
    buf = []
    for x in it:
        buf.append(x)
        if len(buf) == size:
            yield tuple(buf)
            buf = []
    if buf:
        yield tuple(buf)


def tree(depth):
    # Recursive delegation through yield from.
    if depth == 0:
        yield 1
        return
    yield from tree(depth - 1)
    yield depth
    yield from tree(depth - 1)


def fib_gen():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, (a + b) % 1000003


def run(n):
    total = sum(squares(evens(count_up(n)))) % 1000000007
    chunks = 0
    for c in chunked(count_up(n), 7):
        chunks += c[0] + len(c)
    gx = sum(x * 3 for x in range(n) if x & 3 == 1)
    nested = sum(a * b for a in range(60) for b in range(a, 60))
    tr = sum(tree(12))
    fg = fib_gen()
    fs = 0
    for _ in range(n // 2):
        fs += next(fg)
    anyall = (any(x > n for x in range(n)), all(x >= 0 for x in range(n)))
    return f"{total} {chunks} {gx} {nested} {tr} {fs} {anyall}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
