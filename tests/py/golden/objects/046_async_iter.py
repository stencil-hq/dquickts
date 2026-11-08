# async for over __aiter__/__anext__ classes, async comprehensions, aiter()/anext();
# all driven manually without an event loop.


def run(coro):
    """Drive a coroutine that never suspends to the loop; return its result."""
    try:
        coro.send(None)
    except StopIteration as e:
        return e.value
    coro.close()
    raise RuntimeError("coroutine suspended unexpectedly")


class ARange:
    def __init__(self, n):
        self.n = n
        self.i = 0

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self.i >= self.n:
            raise StopAsyncIteration
        self.i += 1
        return self.i - 1


async def collect():
    out = []
    async for x in ARange(4):
        out.append(x)
    return out


print("async for", run(collect()))


async def comprehension():
    return [x * x async for x in ARange(5) if x % 2 == 0]


print("async comprehension", run(comprehension()))


async def for_else():
    async for x in ARange(2):
        pass
    else:
        return "else ran after " + str(x)


print("async for else", run(for_else()))


async def for_break():
    async for x in ARange(10):
        if x == 3:
            break
    return x


print("async for break", run(for_break()))


async def not_async_iterable():
    async for x in [1, 2]:
        pass


try:
    run(not_async_iterable())
except TypeError as e:
    print(type(e).__name__, e)


class BadANext:
    def __aiter__(self):
        return self

    def __anext__(self):
        return 5


async def bad_anext():
    async for x in BadANext():
        pass


try:
    run(bad_anext())
except TypeError as e:
    print(type(e).__name__, e)

ar = ARange(2)
print("manual anext", run(ar.__anext__()), run(ar.__anext__()))
try:
    run(ar.__anext__())
except StopAsyncIteration as e:
    print("manual exhaustion", type(e).__name__)
print("aiter anext builtins", run(anext(aiter(ARange(3)))), run(anext(ARange(0), "default")))
