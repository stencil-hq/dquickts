# Async generators driven manually: __anext__, asend, athrow, aclose, StopAsyncIteration,
# and async generator expressions.


def run(aw):
    """Drive an awaitable that never suspends to the loop; return its result."""
    try:
        aw.send(None)
    except StopIteration as e:
        return e.value
    raise RuntimeError("unexpected suspension")


async def agen():
    received = yield "first"
    print("  agen received", repr(received))
    try:
        received = yield "second"
        print("  agen received", repr(received))
    except ValueError as e:
        yield "handled " + str(e)
    yield "last"


ag = agen()
print("type", type(ag).__name__, ag.__name__)
print("anext", run(ag.__anext__()))
print("asend", run(ag.asend("hello")))
print("athrow", run(ag.athrow(ValueError("boom"))))
print("anext after throw", run(ag.__anext__()))
try:
    run(ag.__anext__())
except StopAsyncIteration as e:
    print("exhausted", type(e).__name__)

ag2 = agen()
try:
    run(ag2.asend("not None"))
except TypeError as e:
    print(type(e).__name__, e)
run(ag2.aclose())


async def numbers(n):
    for i in range(n):
        yield i


async def consume():
    out = []
    async for v in numbers(4):
        out.append(v)
    return out


print("async for over async gen", run(consume()))


async def comprehension():
    return [v async for v in numbers(6) if v % 2]


print("async comprehension", run(comprehension()))


async def async_genexp():
    ge = (v * 10 async for v in numbers(3))
    print("  genexp type", type(ge).__name__)
    return [v async for v in ge]


print("async genexp", run(async_genexp()))


async def with_cleanup():
    try:
        yield 1
        yield 2
    finally:
        print("  async gen finally")


cg = with_cleanup()
print("before aclose", run(cg.__anext__()))
print("aclose result", run(cg.aclose()))
try:
    run(cg.__anext__())
except StopAsyncIteration:
    print("closed gen raises StopAsyncIteration")


async def refuses_close():
    try:
        yield 1
    except GeneratorExit:
        yield "refused"


rc = refuses_close()
run(rc.__anext__())
try:
    run(rc.aclose())
except RuntimeError as e:
    print(type(e).__name__, e)


async def throw_unhandled():
    yield 1


tu = throw_unhandled()
run(tu.__anext__())
try:
    run(tu.athrow(KeyError("k")))
except KeyError as e:
    print("athrow propagates", type(e).__name__, e)


async def stop_inside():
    yield 1
    raise StopAsyncIteration


si = stop_inside()
run(si.__anext__())
try:
    run(si.__anext__())
except RuntimeError as e:
    print("StopAsyncIteration inside", type(e).__name__, e)


async def awaits_inside():
    value = await helper()
    yield value


async def helper():
    return "awaited value"


print("await inside async gen", run(awaits_inside().__anext__()))
fresh = numbers(1)
print("aclose unstarted", run(fresh.aclose()))
try:
    run(fresh.__anext__())
except StopAsyncIteration:
    print("unstarted closed is exhausted")
print("anext builtin default", run(anext(numbers(0), "empty")))
print("aiter is self", aiter(fresh) is fresh)
