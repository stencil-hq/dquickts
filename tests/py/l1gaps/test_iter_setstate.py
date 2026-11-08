# Iterator __setstate__ argument conversion and errors (rangeiter uses
# PyLong_AsLong, which honours __index__; the sequence iterators use
# PyLong_AsSsize_t, which does not).
import array
import collections


class Index:
    def __index__(self):
        return 1


def iterators():
    return [
        ("list", iter([1, 2, 3])),
        ("tuple", iter((1, 2, 3))),
        ("range", iter(range(3))),
        ("longrange", iter(range(2**70, 2**70 + 3))),
        ("str", iter("abc")),
        ("str-wide", iter("a\u20acb")),
        ("bytes", iter(b"abc")),
        ("bytearray", iter(bytearray(b"abc"))),
        ("reversed", reversed([1, 2, 3])),
        ("array", iter(array.array("i", [1, 2, 3]))),
        ("seq", iter(type("S", (), {"__getitem__": lambda s, i: i if i < 3 else 1 / 0})())),
    ]


for arg in ["x", None, 1.5, Index(), True, 2**100, -2**100, -5, 1, 100]:
    for name, it in iterators():
        try:
            it.__setstate__(arg)
            try:
                rest = list(it)
            except ZeroDivisionError:
                rest = "seq-end"
            print(name, repr(arg) if not isinstance(arg, Index) else "Index()", "ok", rest)
        except Exception as e:
            print(name, repr(arg) if not isinstance(arg, Index) else "Index()",
                  type(e).__name__, e)

for name, it in [("deque", iter(collections.deque([1]))), ("enumerate", enumerate([1])),
                 ("callable", iter(lambda: 1, 2)), ("zip", zip([1]))]:
    print(name, hasattr(it, "__setstate__"))
