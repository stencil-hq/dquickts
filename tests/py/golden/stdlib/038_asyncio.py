"""L0 golden: asyncio with deterministic scheduling only.

Ordering relies on FIFO ready-queue semantics and sleep(0); timeouts use
sleeps far longer than the timeout so the outcome is never racy.
"""
import asyncio
import inspect

trace = []


async def worker(name, steps):
    for i in range(steps):
        trace.append(f"{name}{i}")
        await asyncio.sleep(0)
    return f"{name} done"


async def fails(msg):
    await asyncio.sleep(0)
    raise ValueError(msg)


async def main():
    print("run result", await asyncio.sleep(0, result="slept"))

    results = await asyncio.gather(worker("a", 3), worker("b", 2), worker("c", 1))
    print("gather results", results)
    print("interleaving", trace)

    out = await asyncio.gather(worker("x", 1), fails("bad"), return_exceptions=True)
    print("return_exceptions", [type(r).__name__ + ":" + str(r) for r in out])
    try:
        await asyncio.gather(fails("first"), worker("y", 1))
    except ValueError as e:
        print("gather raises", type(e).__name__, e)

    trace.clear()
    t1 = asyncio.create_task(worker("t", 2), name="task-t")
    print("task created, not started", trace, t1.get_name(), t1.done())
    await asyncio.sleep(0)
    print("after one yield", trace)
    print("await task", await t1, t1.done(), t1.result())

    async def forever():
        await asyncio.sleep(3600)

    t2 = asyncio.create_task(forever())
    await asyncio.sleep(0)
    t2.cancel()
    try:
        await t2
    except asyncio.CancelledError:
        print("cancelled", t2.cancelled(), t2.done())

    try:
        await asyncio.wait_for(asyncio.sleep(10), timeout=0.01)
    except TimeoutError as e:
        print("wait_for timeout", type(e).__name__, isinstance(e, asyncio.TimeoutError))
    print("wait_for ok", await asyncio.wait_for(worker("w", 1), timeout=10))
    try:
        async with asyncio.timeout(0.01):
            await asyncio.sleep(10)
    except TimeoutError:
        print("timeout cm", "expired")

    q = asyncio.Queue(maxsize=2)
    consumed = []

    async def producer():
        for i in range(5):
            await q.put(i)
            consumed.append(f"put{i}")
        await q.put(None)

    async def consumer():
        while (item := await q.get()) is not None:
            consumed.append(f"got{item}")
            q.task_done()
        q.task_done()

    await asyncio.gather(producer(), consumer())
    await q.join()
    print("queue order", consumed)
    print("queue empty", q.empty(), q.qsize(), q.maxsize)
    try:
        q.get_nowait()
    except asyncio.QueueEmpty as e:
        print("get_nowait", type(e).__name__)
    full = asyncio.Queue(1)
    full.put_nowait("a")
    try:
        full.put_nowait("b")
    except asyncio.QueueFull as e:
        print("put_nowait", type(e).__name__)
    pq = asyncio.PriorityQueue()
    for item in [(3, "c"), (1, "a"), (2, "b")]:
        pq.put_nowait(item)
    print("priority", [pq.get_nowait()[1] for _ in range(3)])

    return "main done"


print("asyncio.run", asyncio.run(main()))
coro = worker("z", 0)
print("iscoroutinefunction", inspect.iscoroutinefunction(main), asyncio.iscoroutine(coro))
coro.close()
print("second run", asyncio.run(worker("r", 1)), trace[-1])
