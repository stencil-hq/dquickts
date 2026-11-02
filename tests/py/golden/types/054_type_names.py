"""L0 golden: type(x).__name__, isinstance/issubclass and MRO of builtin types.

Covers every common builtin type (numbers, text, containers, iterators,
views, functions, methods, code-ish objects) plus type()/__qualname__/
__module__ attributes and the bool/int relationship.

Usage: python3.15 -I -X utf8 tests/py/golden/types/054_type_names.py
"""


def func():
    """Plain function used to produce function/generator objects."""
    yield 1


async def coro_func():
    """Coroutine function (never awaited; closed immediately)."""


class Plain:
    """Plain user class with one method."""

    def method(self):
        return self


coro = coro_func()
OBJECTS = [
    ("int", 1), ("big int", 2 ** 100), ("bool", True), ("float", 1.5), ("complex", 1j),
    ("str", "s"), ("bytes", b"b"), ("bytearray", bytearray()), ("memoryview", memoryview(b"")),
    ("list", []), ("tuple", ()), ("dict", {}), ("set", set()), ("frozenset", frozenset()),
    ("range", range(1)), ("slice", slice(1)), ("None", None), ("Ellipsis", ...),
    ("NotImplemented", NotImplemented), ("function", func), ("lambda", lambda: 0),
    ("generator", func()), ("coroutine", coro), ("builtin function", len),
    ("builtin method", [].append), ("method", Plain().method), ("class", Plain),
    ("instance", Plain()), ("type", type), ("object", object()), ("exception", ValueError("x")),
    ("list iterator", iter([])), ("tuple iterator", iter(())),
    ("range iterator", iter(range(1))), ("set iterator", iter(set())), ("dict keyiterator", iter({})),
    ("dict keys", {}.keys()), ("dict values", {}.values()), ("dict items", {}.items()),
    ("reversed", reversed([])), ("enumerate", enumerate([])), ("zip", zip()), ("map", map(abs, [])),
    ("filter", filter(None, [])), ("property", property()), ("staticmethod", staticmethod(func)),
    ("classmethod", classmethod(func)), ("super", super(Plain, Plain())), ("code", func.__code__),
    ("wrapper descriptor", object.__init__), ("method descriptor", str.join),
    ("method-wrapper", (1).__add__), ("getset descriptor", int.real),
    ("function in class dict", Plain.__dict__["method"]), ("class of builtin function", type(len)),
    ("bytes iterator", iter(b"")), ("callable_iterator", iter(int, 1)),
]
coro.close()

for label, obj in OBJECTS:
    print(label, "->", type(obj).__name__)

show_types = [int, bool, float, complex, str, bytes, bytearray, list, tuple, dict, set, frozenset,
              range, slice, type, object, memoryview, BaseException, Exception, KeyError]
for t in show_types:
    print("type", t.__name__, t.__qualname__, t.__module__, [c.__name__ for c in t.__mro__])

print("type(type)", type(type).__name__, "type(object)", type(object).__name__)
print("type(Plain)", type(Plain).__name__, Plain.__name__, Plain.__qualname__, Plain.__module__)
print("type(Plain.method)", type(Plain.method).__name__)
print("isinstance(True, int)", isinstance(True, int))
print("isinstance(1, bool)", isinstance(1, bool))
print("isinstance(1, float)", isinstance(1, float))
print("isinstance(1, (str, int))", isinstance(1, (str, int)))
print("isinstance(1, int | str)", isinstance(1, int | str))
print("isinstance(None, type(None))", isinstance(None, type(None)))
print("isinstance(int, type)", isinstance(int, type))
print("isinstance(type, object)", isinstance(type, object))
print("isinstance(object, type)", isinstance(object, type))
print("issubclass(bool, int)", issubclass(bool, int))
print("issubclass(int, bool)", issubclass(int, bool))
print("issubclass(int, object)", issubclass(int, object))
print("issubclass(KeyError, LookupError)", issubclass(KeyError, LookupError))
print("issubclass(frozenset, set)", issubclass(frozenset, set))
print("type(1) is int", type(1) is int, "type(True) is int", type(True) is int)
print("type(True) == bool", type(True) == bool)
print("type of union", type(int | str).__name__)
print("repr of union", repr(int | None))
print("repr of generic alias", repr(list[int]), repr(dict[str, list[int]]))
print("type of generic alias", type(list[int]).__name__)
print("callable", [callable(x) for x in (len, int, 1, "s", Plain, Plain())])
try:
    isinstance(1, 1)
except TypeError as e:
    print("isinstance bad", type(e).__name__, str(e))
try:
    issubclass(1, int)
except TypeError as e:
    print("issubclass bad", type(e).__name__, str(e))
try:
    type()
except TypeError as e:
    print("type() bad", type(e).__name__, str(e))
