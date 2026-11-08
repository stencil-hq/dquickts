# Decorators: plain, with arguments, stacked, class-based, manual metadata preservation.

log = []


def trace(func):
    def wrapper(*args, **kwargs):
        log.append("call " + func.__name__)
        return func(*args, **kwargs)

    return wrapper


@trace
def add(a, b):
    """Add two numbers."""
    return a + b


print("traced", add(1, 2), log)
print("lost name", add.__name__, add.__qualname__, add.__doc__)


def preserve(func):
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)

    wrapper.__name__ = func.__name__
    wrapper.__qualname__ = func.__qualname__
    wrapper.__doc__ = func.__doc__
    wrapper.__wrapped__ = func
    return wrapper


@preserve
def sub(a, b):
    """Subtract."""
    return a - b


print("preserved", sub.__name__, sub.__qualname__, sub.__doc__, sub(5, 3))
print("wrapped original", sub.__wrapped__(10, 1))


def repeat(times):
    def decorator(func):
        def wrapper(*args):
            return [func(*args) for _ in range(times)]

        return wrapper

    return decorator


@repeat(3)
def greet(name):
    return "hi " + name


print("with args", greet("bob"))


def tag(label):
    def decorator(func):
        def wrapper():
            return "<" + label + ">" + func() + "</" + label + ">"

        return wrapper

    return decorator


@tag("b")
@tag("i")
def text():
    return "x"


print("stacked order", text())

order = []


def mark(name):
    order.append("factory " + name)

    def decorator(func):
        order.append("apply " + name)
        return func

    return decorator


@mark("outer")
@mark("inner")
def marked():
    pass


print("evaluation order", order)


class CountCalls:
    def __init__(self, func):
        self.func = func
        self.calls = 0

    def __call__(self, *args):
        self.calls += 1
        return self.func(*args)


@CountCalls
def square(x):
    return x * x


print("class decorator", square(3), square(4), square.calls)
print("decorated type", type(square).__name__)
