# coverage sample (run.sh): async for, async with, await in try/except, async comprehension
import asyncio

async def agen(n):
    for i in range(n):
        if i:
            yield i
        await asyncio.sleep(0)

async def worker(n):
    total = 0
    async for v in agen(n):
        if v > 1:
            total += v
    async with Lock() as l:
        if l:
            total += 1
    try:
        await bad()
    except RuntimeError:
        total -= 1
    return total

async def bad():
    raise RuntimeError

class Lock:
    async def __aenter__(self):
        return self
    async def __aexit__(self, *a):
        return False

async def main():
    r = [x async for x in agen(4) if x != 2]
    return await worker(4), await worker(0), r

print(asyncio.run(main()))
