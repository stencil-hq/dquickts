import asyncio

async def forever():
    await asyncio.sleep(3600)

async def main():
    asyncio.get_running_loop().create_task(forever())
    await asyncio.sleep(0)

loop = asyncio.new_event_loop()
loop.run_until_complete(main())
print("done")
