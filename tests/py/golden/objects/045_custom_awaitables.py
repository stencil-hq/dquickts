# Custom awaitables via __await__: protocol errors, throw/close through awaitables, values flowing through
# yields of awaitables down to the driver.


class Tick:
    """Awaitable that suspends once, yielding a request to the driver, and returns the reply."""

    def __init__(self, label):
        self.label = label

    def __await__(self):
        reply = yield ("tick", self.label)
        return "reply to " + self.label + ": " + repr(reply)


class Immediate:
    def __init__(self, value):
        self.value = value

    def __await__(self):
        return self.value
        yield


async def worker():
    a = await Tick("a")
    b = await Tick("b")
    c = await Immediate("no suspension")
    return [a, b, c]


coro = worker()
request = coro.send(None)
print("first request", request)
request = coro.send(100)
print("second request", request)
try:
    coro.send(200)
except StopIteration as e:
    print("result", e.value)


class BadAwait:
    def __await__(self):
        return 5


async def use_bad():
    return await BadAwait()


c = use_bad()
try:
    c.send(None)
except TypeError as e:
    print(type(e).__name__, e)


class AwaitReturnsCoroutine:
    def __await__(self):
        self.inner = worker()
        return self.inner


arc = AwaitReturnsCoroutine()


async def use_coroutine_as_iterator():
    return await arc


c = use_coroutine_as_iterator()
try:
    c.send(None)
except TypeError as e:
    print(type(e).__name__, e)
arc.inner.close()


class Failing:
    def __await__(self):
        yield "about to fail"
        raise LookupError("awaitable failed")


async def handle_failure():
    try:
        await Failing()
    except LookupError as e:
        return "handled " + str(e)


hf = handle_failure()
print("yielded", hf.send(None))
try:
    hf.send(None)
except StopIteration as e:
    print("failure result", e.value)


async def thrower_target():
    try:
        await Tick("waiting")
    except KeyError as e:
        return "coroutine handled thrown " + str(e)


tt = thrower_target()
tt.send(None)
try:
    tt.throw(KeyError("injected"))
except StopIteration as e:
    print("throw into suspended coroutine", e.value)


class Cleanup:
    def __await__(self):
        try:
            yield "suspended"
        finally:
            print("  awaitable finally on close")


async def closable():
    await Cleanup()


cl = closable()
cl.send(None)
cl.close()
print("closed suspended coroutine")


async def gather(*aws):
    return [await a for a in aws]


g = gather(Immediate(1), Immediate(2), Tick("t"))
print("gather suspends", g.send(None))
try:
    g.send("x")
except StopIteration as e:
    print("gather result", e.value)
