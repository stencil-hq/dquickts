# async with via __aenter__/__aexit__ (enter/exit order, suppression, propagation, protocol
# errors), driven manually without an event loop.


def run(coro):
    """Drive a coroutine that never suspends to the loop; return its result."""
    try:
        coro.send(None)
    except StopIteration as e:
        return e.value
    coro.close()
    raise RuntimeError("coroutine suspended unexpectedly")


log = []


class AsyncResource:
    def __init__(self, name, suppress=False):
        self.name = name
        self.suppress = suppress

    async def __aenter__(self):
        log.append("aenter " + self.name)
        return self.name + "-handle"

    async def __aexit__(self, exc_type, exc, tb):
        log.append("aexit " + self.name + " " + (exc_type.__name__ if exc_type else "None"))
        return self.suppress


async def use_resource():
    async with AsyncResource("r1") as h1, AsyncResource("r2") as h2:
        log.append("body " + h1 + " " + h2)
    return "done"


print("async with", run(use_resource()), log)
log.clear()


async def suppressing():
    async with AsyncResource("s", suppress=True):
        raise ValueError("suppressed")
    return "after suppression"


print("async with suppress", run(suppressing()), log)
log.clear()


async def propagating():
    async with AsyncResource("p"):
        raise KeyError("propagates")


try:
    run(propagating())
except KeyError as e:
    print("async with propagate", type(e).__name__, e, log)
log.clear()


async def early_return():
    async with AsyncResource("ret"):
        return "returned inside"


print("return inside async with", run(early_return()), log)
log.clear()


class Suspending:
    """__aenter__ awaits an awaitable that yields to the driver once."""

    class _Pause:
        def __await__(self):
            reply = yield "paused in aenter"
            return reply

    async def __aenter__(self):
        return await self._Pause()

    async def __aexit__(self, *exc):
        return False


async def uses_suspending():
    async with Suspending() as value:
        return "entered with " + repr(value)


coro = uses_suspending()
print("suspends to driver", coro.send(None))
try:
    coro.send("resume value")
except StopIteration as e:
    print("after resume", e.value)


class SyncOnly:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


async def wrong_with():
    async with SyncOnly():
        pass


try:
    run(wrong_with())
except TypeError as e:
    print(type(e).__name__, e)


async def wrong_sync_with():
    with AsyncResource("x"):
        pass


try:
    run(wrong_sync_with())
except TypeError as e:
    print(type(e).__name__, e)


class NonAwaitableEnter:
    def __aenter__(self):
        return "not awaitable"

    async def __aexit__(self, *a):
        return False


async def bad_aenter():
    async with NonAwaitableEnter():
        pass


try:
    run(bad_aenter())
except TypeError as e:
    print(type(e).__name__, e)
