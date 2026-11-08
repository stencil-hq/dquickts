# Recursive Fibonacci (function call overhead, int add, comparisons).
import sys
import time


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def fib_kw(n, *, acc=0):
    # Keyword-only argument path, smaller tree.
    if n < 2:
        return acc + n
    return fib_kw(n - 1, acc=acc) + fib_kw(n - 2)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 31
    fib(n - 10)
    t0 = time.perf_counter()
    a = fib(n)
    b = fib_kw(n - 4, acc=1)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {a} {b}")


main()
