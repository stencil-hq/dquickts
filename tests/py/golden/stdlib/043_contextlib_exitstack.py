"""L0 golden: contextlib ExitStack, nullcontext, closing, ContextDecorator, async managers."""
import asyncio
import contextlib

order = []


@contextlib.contextmanager
def tag(name):
    order.append(f"enter {name}")
    try:
        yield name
    finally:
        order.append(f"exit {name}")


with contextlib.ExitStack() as stack:
    for i in range(3):
        stack.callback(order.append, f"cb{i}")
    print("enter_context returns", stack.enter_context(tag("stacked")))
    order.append("body")
print("ExitStack LIFO", order)
order.clear()
with contextlib.ExitStack() as stack:
    stack.callback(order.append, "never")
    moved = stack.pop_all()
print("pop_all deferred", order)
moved.close()
print("closed later", order)


def suppressing_exit(*exc):
    order.append(f"exit saw {exc[0].__name__}")
    return True


with contextlib.ExitStack() as stack:
    stack.push(suppressing_exit)
    raise RuntimeError("boom")
print("push suppress", order[-1])
try:
    with contextlib.ExitStack() as stack:
        stack.enter_context(tag("outer"))
        stack.enter_context(tag("inner"))
        raise KeyError("k")
except KeyError as e:
    print("exception passes through", repr(e), order[-4:])
try:
    with contextlib.ExitStack() as stack:
        stack.enter_context(object())
except TypeError as e:
    print("not a context manager", type(e).__name__)

with contextlib.nullcontext(42) as v:
    print("nullcontext", v)
with contextlib.nullcontext() as v:
    print("nullcontext none", v)


class Resource:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


r = Resource()
with contextlib.closing(r) as same:
    print("closing yields same", same is r, r.closed)
print("closing closed", r.closed)


class Managed(contextlib.ContextDecorator):
    def __enter__(self):
        order.append("M enter")
        return self

    def __exit__(self, *exc):
        order.append("M exit")
        return False


@Managed()
def work():
    order.append("work")


order.clear()
work()
print("ContextDecorator", order)


class ACM(contextlib.AbstractContextManager):
    def __exit__(self, *exc):
        return None


with ACM() as a:
    print("AbstractContextManager returns self", isinstance(a, ACM))


@contextlib.asynccontextmanager
async def atag(name):
    order.append(f"aenter {name}")
    yield name.upper()
    order.append(f"aexit {name}")


async def amain():
    order.clear()
    async with atag("x") as val:
        order.append(f"abody {val}")
    async with contextlib.AsyncExitStack() as astack:
        await astack.enter_async_context(atag("y"))
        astack.callback(order.append, "sync cb")
    async with contextlib.aclosing(agen()) as g:
        order.append(f"first {await anext(g)}")
    return order


async def agen():
    try:
        yield 1
        yield 2
    finally:
        order.append("agen closed")


print("async managers", asyncio.run(amain()))
