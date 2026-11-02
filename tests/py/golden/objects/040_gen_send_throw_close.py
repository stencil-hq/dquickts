# Generator send/throw/close: first send must be None, exceptions thrown into generators,
# GeneratorExit handling, yielding during close, running generators.


def echo():
    received = []
    try:
        while True:
            value = yield len(received)
            received.append(value)
    finally:
        print("  echo finally, received", received)


g = echo()
try:
    g.send("too early")
except TypeError as e:
    print(type(e).__name__, e)
print("prime", g.send(None))
print("send a", g.send("a"))
print("send b", g.send("b"))
g.close()
print("send after close", end=" ")
try:
    g.send(1)
except StopIteration:
    print("StopIteration")
g.close()
print("close twice ok")


def catcher():
    while True:
        try:
            yield "waiting"
        except ValueError as e:
            yield "caught " + str(e)


c = catcher()
next(c)
print("throw caught", c.throw(ValueError("v1")))
print("next after throw", next(c))
try:
    c.throw(KeyError("k"))
except KeyError as e:
    print("throw uncaught propagates", type(e).__name__, e)
print("now exhausted", next(c, "exhausted"))

fresh = catcher()
try:
    fresh.throw(ValueError("before start"))
except ValueError as e:
    print("throw into unstarted", type(e).__name__, e)
print("unstarted closed after throw", next(fresh, "exhausted"))

try:
    c.throw(TypeError("into exhausted"))
except TypeError as e:
    print("throw into exhausted", e)


def handles_exit():
    try:
        yield 1
    except GeneratorExit:
        print("  got GeneratorExit")
        raise


h = handles_exit()
next(h)
h.close()
print("close unstarted is silent", end=" ")
handles_exit().close()
print("ok")


def ignores_exit():
    try:
        yield 1
    except GeneratorExit:
        yield "refuse"


ig = ignores_exit()
next(ig)
try:
    ig.close()
except RuntimeError as e:
    print(type(e).__name__, e)


def returns_on_exit():
    try:
        yield 1
    except GeneratorExit:
        return "close value"


r = returns_on_exit()
next(r)
print("close return value", repr(r.close()))
print("close unstarted value", repr(returns_on_exit().close()))


def self_reference():
    yield next(me)


me = self_reference()
try:
    next(me)
except ValueError as e:
    print(type(e).__name__, e)


def accumulate():
    total = 0
    while True:
        value = yield total
        if value is None:
            return total
        total += value


acc = accumulate()
next(acc)
for v in [5, 10, 20]:
    acc.send(v)
try:
    acc.send(None)
except StopIteration as e:
    print("accumulated via send", e.value)

g2 = echo()
next(g2)
try:
    g2.throw(GeneratorExit())
except GeneratorExit:
    print("throw GeneratorExit propagates")
print("send after throw GeneratorExit", next(g2, "exhausted"))
