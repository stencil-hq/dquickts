# PEP 479 (StopIteration inside generators becomes RuntimeError), StopIteration values,
# exhausted generator behaviour and interaction with for loops and builtins.


def leaks_stop():
    yield 1
    raise StopIteration("leaked")


g = leaks_stop()
print("first", next(g))
try:
    next(g)
except RuntimeError as e:
    print(type(e).__name__, e)
    print("cause", type(e.__cause__).__name__, e.__cause__)
print("after RuntimeError exhausted", next(g, "exhausted"))


def next_on_empty():
    it = iter([])
    yield next(it)


try:
    list(next_on_empty())
except RuntimeError as e:
    print("next() inside generator", type(e).__name__, e)


def safe_next():
    it = iter([])
    yield next(it, "default")


print("next with default inside", list(safe_next()))


def stop_subclass():
    yield
    raise StopAsyncIteration


try:
    list(stop_subclass())
except StopAsyncIteration as e:
    print("StopAsyncIteration not converted in sync gen", type(e).__name__)

try:
    list(x for x in [1] if next(iter([])))
except RuntimeError as e:
    print("genexp StopIteration", type(e).__name__, e)

print("list comprehension lets it through", end=" ")
try:
    [next(iter([])) for _ in [1]]
except StopIteration as e:
    print(type(e).__name__)


def returns_value():
    yield 1
    return {"answer": 42}


g = returns_value()
next(g)
try:
    next(g)
except StopIteration as e:
    print("return dict value", e.value, e.args)
for _ in range(2):
    try:
        next(g)
    except StopIteration as e:
        print("exhausted again value", e.value)


def returns_tuple():
    return (1, 2)
    yield


try:
    next(returns_tuple())
except StopIteration as e:
    print("tuple return", e.value, e.args)


def returns_stop():
    return StopIteration("x")
    yield


try:
    next(returns_stop())
except StopIteration as e:
    print("return exception object", repr(e.value))


class ManualIter:
    def __init__(self):
        self.i = 0

    def __iter__(self):
        return self

    def __next__(self):
        self.i += 1
        if self.i > 2:
            raise StopIteration("custom stop")
        return self.i


print("manual StopIteration fine", list(ManualIter()))
mi = ManualIter()
for v in mi:
    pass
print("manual iter can be resumed", next(mi, "x"), mi.i)

si = StopIteration(1, 2)
print("StopIteration multiple args", si.value, si.args)
print("StopIteration default", StopIteration().value)


def generator_return_in_loop():
    for i in range(10):
        if i == 3:
            return i
        yield i


gr = generator_return_in_loop()
print("for ignores return value", list(gr))
print("map stops cleanly", list(map(lambda x: x * 2, generator_return_in_loop())))
print("zip shortest stops", list(zip(generator_return_in_loop(), "abcdef")))
