# Coroutines: generator send() pipelines and async/await chains driven by a
# tiny hand-written round-robin scheduler (no asyncio).
import sys
import time


def accumulator():
    total = 0
    while True:
        x = yield total
        if x is None:
            break
        total += x
    return total


def filter_stage(target):
    while True:
        x = yield
        if x % 3 != 0:
            target.send(x * 2)


def send_pipeline(n):
    acc = accumulator()
    next(acc)
    f = filter_stage(acc)
    next(f)
    for i in range(n):
        f.send(i)
    try:
        acc.send(None)
    except StopIteration as e:
        return e.value
    return -1


class Suspend:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __await__(self):
        got = yield self.value
        return got


async def leaf(x):
    y = await Suspend(x)
    return y + 1


async def middle(x):
    a = await leaf(x)
    b = await leaf(a)
    return a + b


async def worker(ident, steps):
    total = 0
    for i in range(steps):
        total += await middle(i + ident)
    return total


def scheduler(tasks):
    ready = [(t, None) for t in tasks]
    results = []
    yields = 0
    while ready:
        nxt = []
        for coro, val in ready:
            try:
                out = coro.send(val)
            except StopIteration as e:
                results.append(e.value)
                continue
            yields += 1
            nxt.append((coro, out * 2))
        ready = nxt
    return sum(results), yields


def run(n):
    p = send_pipeline(n)
    total, yields = scheduler([worker(k, n // 40) for k in range(10)])
    return f"{p} {total} {yields}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 420000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
