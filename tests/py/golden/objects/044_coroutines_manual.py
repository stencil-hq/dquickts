# Coroutines without an event loop: async def objects driven with send(None), await chains,
# exception propagation, close(), reuse errors.


def run(coro):
    """Drive a coroutine to completion, collecting anything it yields to the 'loop'."""
    yielded = []
    while True:
        try:
            yielded.append(coro.send(None))
        except StopIteration as e:
            return e.value, yielded


async def answer():
    return 42


c = answer()
print("coroutine type", type(c).__name__)
print("name", c.__name__, c.__qualname__)
try:
    c.send(None)
except StopIteration as e:
    print("result via StopIteration", e.value)
try:
    c.send(None)
except RuntimeError as e:
    print("reuse", type(e).__name__, e)


async def add(a, b):
    return a + b


async def compute():
    x = await add(1, 2)
    y = await add(x, 10)
    return [x, y]


print("await chain", run(compute()))


async def nothing():
    pass


print("implicit None", run(nothing()))


async def fails():
    raise ValueError("inside coroutine")


async def catches():
    try:
        await fails()
    except ValueError as e:
        return "caught " + str(e)


print("exception caught by awaiter", run(catches()))
try:
    run(fails())
except ValueError as e:
    print("exception escapes send", type(e).__name__, e)


async def deep(n):
    if n == 0:
        return "bottom"
    return "d" + str(n) + "/" + await deep(n - 1)


print("recursive awaits", run(deep(4)))

unstarted = answer()
unstarted.close()
try:
    unstarted.send(None)
except RuntimeError as e:
    print("send after close", type(e).__name__, e)


async def await_non_awaitable():
    await 5


try:
    run(await_non_awaitable())
except TypeError as e:
    print(type(e).__name__, e)


async def awaits_same(coro):
    first = await coro
    second = await coro
    return first, second


try:
    run(awaits_same(answer()))
except RuntimeError as e:
    print("double await", type(e).__name__, e)

c = answer()
try:
    next(c)
except TypeError as e:
    print("coroutines are not iterators", type(e).__name__, e)
try:
    iter(c)
except TypeError as e:
    print("not iterable", type(e).__name__, e)
c.close()

c = compute()
try:
    c.throw(KeyError("thrown"))
except KeyError as e:
    print("throw into unstarted coroutine", type(e).__name__, e)


async def guarded():
    try:
        await add(0, 0)
        return "normal"
    finally:
        print("  coroutine finally")


print("finally in coroutine", run(guarded()))
print("async def is function", type(answer).__name__)
print("coroutine flag check", answer.__code__.co_flags & 0x80 != 0, add.__code__.co_flags & 0x80 != 0)


async def returns_coroutine():
    return answer


result, _ = run(returns_coroutine())
print("returns function", result.__name__)
