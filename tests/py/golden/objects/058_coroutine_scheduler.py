# A hand-written round-robin scheduler driving coroutines through custom awaitables:
# interleaving order, results, cancellation via close(), and errors from tasks.


class Sleep:
    """Awaitable that suspends `ticks` times, yielding control to the scheduler."""

    def __init__(self, ticks):
        self.ticks = ticks

    def __await__(self):
        for _ in range(self.ticks):
            yield "sleep"


trace = []


def scheduler(tasks, max_steps=100):
    """Round-robin coroutines; return completion order, errors and the step count."""
    ready = list(tasks.items())
    finished = []
    steps = 0
    while ready and steps < max_steps:
        name, coro = ready.pop(0)
        try:
            coro.send(None)
            steps += 1
            trace.append(name)
            ready.append((name, coro))
        except StopIteration as e:
            finished.append((name, e.value))
        except Exception as e:
            finished.append((name, type(e).__name__ + ": " + str(e)))
    for name, coro in ready:
        coro.close()
        finished.append((name, "cancelled"))
    return finished, steps


async def job(name, ticks):
    await Sleep(ticks)
    return name + " slept " + str(ticks)


result = scheduler({"slow": job("slow", 3), "fast": job("fast", 1), "zero": job("zero", 0)})
print("scheduler", result)
print("interleaving", trace)
trace.clear()


async def multi_step(name, n):
    total = 0
    for i in range(n):
        await Sleep(1)
        total += i
    return name + " total " + str(total)


async def failing(name):
    await Sleep(1)
    raise ValueError(name + " failed")


print("mixed", scheduler({"a": multi_step("a", 3), "b": failing("b"), "c": multi_step("c", 1)}))
print("trace", trace)
trace.clear()


async def forever():
    try:
        while True:
            await Sleep(1)
    finally:
        print("  forever cleaned up on close")


print("cancel", scheduler({"loop": forever(), "short": job("short", 2)}, max_steps=5))
trace.clear()


async def parent():
    first = await job("child1", 1)
    second = await job("child2", 2)
    return [first, second]


print("nested awaits", scheduler({"parent": parent()}))
print("parent steps", len(trace))
trace.clear()


class Channel:
    def __init__(self):
        self.items = []

    def put(self, item):
        self.items.append(item)

    async def get(self):
        while not self.items:
            await Sleep(1)
        return self.items.pop(0)


chan = Channel()


async def producer():
    for i in range(3):
        chan.put(i)
        await Sleep(2)
    chan.put(None)
    return "producer done"


async def consumer():
    got = []
    while (item := await chan.get()) is not None:
        got.append(item)
    return got


print("producer/consumer", scheduler({"consumer": consumer(), "producer": producer()}))
