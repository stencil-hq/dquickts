# Decorators part 2: registries, arbitrary decorator expressions (PEP 614), decorators on
# methods/properties, decorators returning non-callables, and decorated classes' methods.

log = []


def trace(func):
    def wrapper(*args, **kwargs):
        log.append("call " + func.__name__)
        return func(*args, **kwargs)

    return wrapper


def preserve(func):
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)

    wrapper.__name__ = func.__name__
    wrapper.__doc__ = func.__doc__
    return wrapper


def register(registry):
    def decorator(func):
        registry[func.__name__] = func
        return func

    return decorator


handlers = {}


@register(handlers)
def on_start():
    return "started"


@register(handlers)
def on_stop():
    return "stopped"


print("registry", sorted(handlers), handlers["on_stop"](), on_start())


@lambda f: f()
def evaluated_now():
    return "value"


print("decorator expression", evaluated_now)

decorators = [trace]


@decorators[0]
def via_subscript():
    return 1


print("subscript decorator", via_subscript(), log[-1])


class Service:
    @staticmethod
    @trace
    def ping():
        return "pong"

    @property
    @preserve
    def name(self):
        """Service name."""
        return "svc"

    @classmethod
    @trace
    def build(cls):
        return cls.__name__


print("method decorators", Service.ping(), Service().name, log[-1])
print("classmethod over wrapper", Service.build(), log[-1])
print("property doc from wrapper", Service.name.__doc__)


def returns_none(func):
    return None


@returns_none
def gone():
    pass


print("decorator returning None", gone)


@str.upper
@lambda f: f.__name__
def shout():
    pass


print("decorator returning str", shout)


def counting(cls):
    original = cls.__init__
    cls.instances = 0

    def __init__(self, *args):
        cls.instances += 1
        original(self, *args)

    cls.__init__ = __init__
    return cls


@counting
class Item:
    def __init__(self, name):
        self.name = name


items = [Item("a"), Item("b")]
print("class decorator wraps init", Item.instances, [i.name for i in items])


def bad_decorator(func):
    raise TypeError("decorator refused " + func.__name__)


try:

    @bad_decorator
    def refused():
        pass

except TypeError as e:
    print(type(e).__name__, e)
print("name not bound after failed decoration", "refused" in globals())
