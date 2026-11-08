# yield from: delegation of iteration and send, recursive delegation, return value propagation.


def inner():
    received = yield "inner-1"
    print("  inner got", repr(received))
    received = yield "inner-2"
    print("  inner got", repr(received))
    return "inner result"


def outer():
    result = yield from inner()
    print("  outer got result", repr(result))
    yield "outer-after"
    return "outer result"


g = outer()
print("first", next(g))
print("send x", g.send("x"))
print("send y", g.send("y"))
try:
    next(g)
except StopIteration as e:
    print("outer return", e.value)

print("yield from iterables", list((lambda: (yield from [1, 2]))()))


def chain(*iterables):
    for it in iterables:
        yield from it


print("chain", list(chain("ab", [1, 2], range(3), (x * x for x in range(3)))))


def tree_walk(node):
    value, children = node
    yield value
    for child in children:
        yield from tree_walk(child)


tree = ("root", [("a", [("a1", []), ("a2", [])]), ("b", [("b1", [("b1x", [])])])])
print("recursive delegation", list(tree_walk(tree)))


def returns_value(n):
    yield n
    return n * 10


def sum_returns():
    total = 0
    for i in range(1, 4):
        total += yield from returns_value(i)
    return total


try:
    gen = sum_returns()
    print("delegated yields", [next(gen) for _ in range(3)])
    next(gen)
except StopIteration as e:
    print("sum of sub-returns", e.value)


class Countdown:
    def __init__(self, n):
        self.n = n

    def __iter__(self):
        while self.n > 0:
            yield self.n
            self.n -= 1


def from_custom():
    yield from Countdown(3)


print("yield from custom iterable", list(from_custom()))


def stop_value():
    return (yield from [])


try:
    next(stop_value())
except StopIteration as e:
    print("empty delegation returns None", e.value)
