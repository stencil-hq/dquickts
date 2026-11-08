"""L0 golden: asyncio synchronization primitives, TaskGroup, wait/as_completed, async generators.

All orderings are deterministic: they rely on FIFO scheduling and sleep(0),
or on sleeps that differ by a wide margin.
"""
import asyncio


async def worker(name, steps):
    for _ in range(steps):
        await asyncio.sleep(0)
    return f"{name} done"


async def fails(msg):
    await asyncio.sleep(0)
    raise ValueError(msg)


async def forever():
    await asyncio.sleep(3600)


async def main():
    lock = asyncio.Lock()
    events = []

    async def critical(n):
        async with lock:
            events.append(f"in{n}")
            await asyncio.sleep(0)
            events.append(f"out{n}")

    await asyncio.gather(critical(1), critical(2), critical(3))
    print("lock", events, lock.locked())

    ev = asyncio.Event()

    async def waiter(i):
        await ev.wait()
        return f"waiter{i} saw event"

    waiters = [asyncio.create_task(waiter(i)) for i in range(2)]
    await asyncio.sleep(0)
    print("event before set", [w.done() for w in waiters], ev.is_set())
    ev.set()
    print("event", await asyncio.gather(*waiters))
    ev.clear()
    print("event cleared", ev.is_set())

    cond = asyncio.Condition()
    seen = []

    async def consumer(i):
        async with cond:
            await cond.wait()
            seen.append(i)

    consumers = [asyncio.create_task(consumer(i)) for i in range(3)]
    await asyncio.sleep(0)
    async with cond:
        cond.notify(2)
    await asyncio.sleep(0)
    async with cond:
        cond.notify_all()
    await asyncio.gather(*consumers)
    print("condition", seen)

    sem = asyncio.Semaphore(2)
    active = []
    peak = []

    async def limited(i):
        async with sem:
            active.append(i)
            peak.append(len(active))
            await asyncio.sleep(0)
            active.remove(i)

    await asyncio.gather(*(limited(i) for i in range(5)))
    print("semaphore peak", max(peak), sem.locked())
    bs = asyncio.BoundedSemaphore(1)
    try:
        bs.release()
    except ValueError as e:
        print("bounded release", type(e).__name__, e)

    async with asyncio.TaskGroup() as tg:
        g1 = tg.create_task(worker("g", 1))
        g2 = tg.create_task(worker("h", 2))
    print("TaskGroup", g1.result(), g2.result())
    try:
        async with asyncio.TaskGroup() as tg:
            tg.create_task(fails("tg-err"))
            sibling = tg.create_task(forever())
    except* ValueError as eg:
        print("TaskGroup error", [str(e) for e in eg.exceptions], sibling.cancelled())

    done, pending = await asyncio.wait([asyncio.create_task(worker("d", 1))])
    print("wait all", len(done), len(pending), [t.result() for t in done])
    slow = asyncio.create_task(asyncio.sleep(10))
    fast = asyncio.create_task(worker("f", 1))
    done, pending = await asyncio.wait([slow, fast], return_when=asyncio.FIRST_COMPLETED)
    print("wait first", fast in done, slow in pending)
    slow.cancel()
    order = []
    for coro in asyncio.as_completed([asyncio.sleep(0.5, result="slow"), asyncio.sleep(0, result="fast")]):
        order.append(await coro)
    print("as_completed", order)

    async def squares(n):
        for i in range(n):
            await asyncio.sleep(0)
            yield i * i

    print("async gen", [v async for v in squares(4)])
    agen = squares(3)
    print("anext", await anext(agen), await anext(agen), await anext(agen), await anext(agen, "end"))
    print("current task", asyncio.current_task().get_name() is not None)
    shielded = asyncio.shield(worker("s", 1))
    print("shield", await shielded)
    loop = asyncio.get_running_loop()
    fut = loop.create_future()
    loop.call_soon(fut.set_result, "future value")
    print("future", await fut, fut.done())
    return "main done"


print("asyncio.run", asyncio.run(main()))
