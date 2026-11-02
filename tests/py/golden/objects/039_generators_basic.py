# Generator basics: lazy execution, yield values, return value via StopIteration.value,
# exhausted generators, generator objects' identity as their own iterator.

trace = []


def counter(n):
    trace.append("start")
    for i in range(n):
        trace.append("yield " + str(i))
        yield i
    trace.append("end")
    return "done"


g = counter(3)
print("type", type(g).__name__)
print("nothing ran yet", trace)
print("iter is self", iter(g) is g)
print("first", next(g), trace)
print("rest", list(g), trace)
try:
    next(g)
except StopIteration as e:
    print("exhausted", type(e).__name__, repr(e.value))

g = counter(1)
next(g)
try:
    next(g)
except StopIteration as e:
    print("return value", repr(e.value), e.args)


def gen_return_none():
    yield 1


g = gen_return_none()
next(g)
try:
    next(g)
except StopIteration as e:
    print("implicit return", e.value, e.args)


def empty():
    return
    yield


print("empty generator", list(empty()))


def only_return():
    if False:
        yield
    return 42


try:
    next(only_return())
except StopIteration as e:
    print("immediate return value", e.value)

print("generator name", counter.__name__, counter(0).__name__, counter(0).__qualname__)
print("for consumes return silently", [x for x in counter(2)])
print("next with default", next(empty(), "default"))


def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b


out = []
for value in fib():
    if value > 100:
        break
    out.append(value)
print("infinite with break", out)

f = fib()
print("take 5 via zip", [v for _, v in zip(range(5), f)])
print("resume same generator", next(f), next(f))


def yield_expressions():
    x = yield
    print("  received", repr(x))
    y = yield x
    print("  received", repr(y))


ye = yield_expressions()
print("bare yield", next(ye))
print("yield value", ye.send("hello"))
try:
    ye.send("world")
except StopIteration:
    print("finished after send")


def multiple_yields_in_expr():
    result = (yield 1) + (yield 2)
    yield "sum " + str(result)


m = multiple_yields_in_expr()
print("expr yields", next(m), m.send(10), m.send(20))


def gen_with_locals(n):
    total = 0
    for i in range(n):
        total += i
        yield total


print("running totals", list(gen_with_locals(6)))
print("unpack generator", *counter(3))
a, b, c = counter(3)
print("tuple unpack", a, b, c)
print("in operator consumes", 1 in counter(5))
g = counter(5)
print("partial consume with in", 2 in g, list(g))
print("bool of generator is True", bool(empty()))
print("sum max", sum(counter(5)), max(counter(4)))
