# Exceptions: raise/except in hot loops, class hierarchies, try/finally,
# raise ... from, exception attributes, else clauses, builtin errors.
import sys
import time


class AppError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


class NotFound(AppError):
    pass


class Invalid(AppError):
    pass


def lookup(d, k):
    try:
        return d[k]
    except KeyError:
        raise NotFound(k) from None


def check(i):
    if i % 3 == 0:
        raise Invalid(i)
    if i % 5 == 0:
        raise NotFound(i)
    return i


def deep(n):
    if n == 0:
        raise ValueError("bottom")
    return deep(n - 1)


def run(n):
    acc = 0
    finals = 0
    d = {i: i for i in range(0, 100, 2)}
    for i in range(n):
        try:
            acc += check(i)
        except Invalid as e:
            acc += e.code * 2
        except AppError as e:
            acc -= e.code
        else:
            acc += 1
        finally:
            finals += 1
        try:
            acc += lookup(d, i % 100)
        except NotFound as e:
            acc += e.args[0]
        try:
            int("x%d" % (i % 10)) if i % 4 == 0 else None
        except ValueError:
            acc += 3
        try:
            [][i]
        except IndexError:
            acc += 1
    chained = 0
    for i in range(n // 20):
        try:
            try:
                deep(10)
            except ValueError as e:
                raise RuntimeError("wrap") from e
        except RuntimeError as e:
            if isinstance(e.__cause__, ValueError):
                chained += 1
    return f"{acc} {finals} {chained}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 140000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
