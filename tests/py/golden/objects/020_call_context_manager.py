# __call__ objects and the context manager protocol (__enter__/__exit__), including
# exception suppression and nesting order.


class Adder:
    def __init__(self, n):
        self.n = n

    def __call__(self, x, *, scale=1):
        return (x + self.n) * scale


add5 = Adder(5)
print("call", add5(1), add5(1, scale=3))
print("callable", callable(add5), callable(Adder), callable(5))
print("map with callable obj", list(map(add5, [0, 10])))
try:
    add5()
except TypeError as e:
    print(type(e).__name__, e)

log = []


class Resource:
    def __init__(self, name, suppress=False):
        self.name = name
        self.suppress = suppress

    def __enter__(self):
        log.append("enter " + self.name)
        return self.name.upper()

    def __exit__(self, exc_type, exc, tb):
        kind = exc_type.__name__ if exc_type else None
        log.append("exit " + self.name + " " + str(kind) + " " + str(exc))
        return self.suppress


with Resource("a") as value:
    log.append("body " + value)
print("normal", log)

log.clear()
try:
    with Resource("b"):
        raise ValueError("bad")
except ValueError as e:
    log.append("caught " + str(e))
print("propagated", log)

log.clear()
with Resource("c", suppress=True):
    raise KeyError("hidden")
log.append("after suppressed")
print("suppressed", log)

log.clear()
with Resource("outer") as o, Resource("inner") as i:
    log.append("body " + o + " " + i)
print("nested order", log)

log.clear()
with (
    Resource("p1"),
    Resource("p2", suppress=True),
):
    raise RuntimeError("x")
print("parenthesized", log)


def returns_inside():
    with Resource("ret"):
        return "returned"


log.clear()
print("return in with", returns_inside(), log)


class NoExit:
    def __enter__(self):
        return self


try:
    with NoExit():
        pass
except TypeError as e:
    print(type(e).__name__, e)


class NoEnter:
    def __exit__(self, *a):
        return False


try:
    with NoEnter():
        pass
except TypeError as e:
    print(type(e).__name__, e)


class FailingEnter:
    def __enter__(self):
        raise OSError("enter failed")

    def __exit__(self, *a):
        print("exit must not run")


try:
    with FailingEnter():
        print("body must not run")
except OSError as e:
    print(type(e).__name__, e)


class ReplacingExit:
    def __enter__(self):
        return None

    def __exit__(self, exc_type, exc, tb):
        raise TypeError("from exit")


try:
    with ReplacingExit() as nothing:
        raise ValueError("original")
except TypeError as e:
    print("exit raised", type(e).__name__, e, "context", type(e.__context__).__name__)
print("as binding None", nothing)


class Truthy:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return "non-empty string"


with Truthy():
    1 / 0
print("truthy exit suppresses ZeroDivisionError")
