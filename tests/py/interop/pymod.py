# Python module imported by test_interop.js through the ES module loader.
import js

counter = 0
items = [1, 2, 3]
d = {"a": 1, "b": [1, 2]}
t = (1, "two", None)
s = {7}
_private = "hidden"
stored = ValueError("stored")


def add(a, b):
    return a + b


def kw(a, b=2, *, c=3):
    return [a, b, c]


def ident(x):
    return x


def is_same(a, b):
    return a is b


def type_name(x):
    return type(x).__name__


def raise_value_error():
    raise ValueError("from python")


def raise_stored():
    raise stored


def catch_any(f):
    try:
        f()
    except BaseException as e:
        return e
    return None


def catch_js(f):
    try:
        f()
    except js.JsError as e:
        return e
    return None


def call_js(f, *args):
    return f(*args)


def typeof_args(f):
    # Python -> JS: int outside int32 within +-2**53 -> Number, beyond -> BigInt
    return [f(5), f(2**40), f(-(2**53)), f(2**60), f(1.5)]


def ret_type(f):
    # JS -> Python: an integral float64 result becomes int
    return type(f()).__name__


def kw_to_js(f):
    return f(1, x=2, y=3)


def get_attr(o, name):
    return getattr(o, name)


def missing_attr(o):
    try:
        o.no_such_property
    except AttributeError:
        return True
    return False


def jsobj_info(o):
    return [type(o).__name__, repr(o), o.x, o["x"], "x" in o, "zz" in o]


def call_method(o):
    return o.getX()


def map_info(m):
    return [m["a"], len(m), "a" in m, "zz" in m, list(m), m.to_py()]


def deep(o):
    return o.to_py()


def set_attr(o, name, v):
    setattr(o, name, v)


async def coro_add(p, x):
    v = await p
    return v + x


async def coro_fail():
    raise ValueError("async boom")


async def await_reject(p):
    try:
        await p
    except js.JsError as e:
        return e.js_error
    return None


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm1(self):
        return abs(self.x) + abs(self.y)

    def __str__(self):
        return f"Point({self.x}, {self.y})"


class Box:
    pass


def make_box():
    return Box()
