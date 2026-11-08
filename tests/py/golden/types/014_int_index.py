"""L0 golden: the __index__ protocol and integer-required contexts.

Objects defining __index__ are accepted wherever an exact integer is needed
(indexing, slicing, range, hex, bin, repetition); floats and __int__-only
objects are rejected with TypeError.

Usage: python3.15 -I -X utf8 tests/py/golden/types/014_int_index.py
"""


class Idx:
    """Integer-like object exposing only __index__."""

    def __init__(self, value: int) -> None:
        self.value = value

    def __index__(self) -> int:
        return self.value


class OnlyInt:
    """Object exposing only __int__ (not usable as an index)."""

    def __int__(self) -> int:
        return 3


class BadIndex:
    """__index__ that returns a non-int."""

    def __index__(self):
        return 1.5


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


data = [10, 20, 30, 40, 50]
show("(5).__index__()", lambda: (5).__index__())
show("True.__index__()", lambda: True.__index__())
show("(2**100).__index__()", lambda: (2 ** 100).__index__())
show("list[Idx(1)]", lambda: data[Idx(1)])
show("list[Idx(-1)]", lambda: data[Idx(-1)])
show("list[Idx(1):Idx(3)]", lambda: data[Idx(1):Idx(3)])
show("list[::Idx(2)]", lambda: data[::Idx(2)])
show("'abc'[Idx(2)]", lambda: "abc"[Idx(2)])
show("b'abc'[Idx(0)]", lambda: b"abc"[Idx(0)])
show("(1, 2)[Idx(0)]", lambda: (1, 2)[Idx(0)])
show("range(Idx(3))", lambda: range(Idx(3)))
show("list(range(Idx(1), Idx(7), Idx(2)))", lambda: list(range(Idx(1), Idx(7), Idx(2))))
show("range(10)[Idx(4)]", lambda: range(10)[Idx(4)])
show("hex(Idx(255))", lambda: hex(Idx(255)))
show("bin(Idx(5))", lambda: bin(Idx(5)))
show("oct(Idx(8))", lambda: oct(Idx(8)))
show("'ab' * Idx(3)", lambda: "ab" * Idx(3))
show("[0] * Idx(2)", lambda: [0] * Idx(2))
show("bytes(Idx(3))", lambda: bytes(Idx(3)))
show("int(Idx(7))", lambda: int(Idx(7)))
show("float(Idx(7))", lambda: float(Idx(7)))
show("complex(Idx(7))", lambda: complex(Idx(7)))
show("operator-free index", lambda: [10, 20, 30].pop(Idx(1)))
show("list.insert(Idx)", lambda: (lambda l: (l.insert(Idx(0), "x"), l)[1])([1, 2]))
show("chr(Idx(65))", lambda: chr(Idx(65)))
show("round(1.23456, Idx(2))", lambda: round(1.23456, Idx(2)))
show("pow(2, Idx(5))", lambda: pow(2, Idx(5)))
show("slice(Idx(1), Idx(4)).indices(3)", lambda: slice(Idx(1), Idx(4)).indices(3))
show("int.from_bytes chosen length", lambda: (300).to_bytes(Idx(2)))

show("list[1.0]", lambda: data[1.0])
show("list[OnlyInt()]", lambda: data[OnlyInt()])
show("int(OnlyInt())", lambda: int(OnlyInt()))
show("range(OnlyInt())", lambda: range(OnlyInt()))
show("range(1.0)", lambda: range(1.0))
show("hex(OnlyInt())", lambda: hex(OnlyInt()))
show("'ab' * 2.0", lambda: "ab" * 2.0)
show("list['1']", lambda: data["1"])
show("list[None]", lambda: data[None])
show("list[BadIndex()]", lambda: data[BadIndex()])
show("hex(BadIndex())", lambda: hex(BadIndex()))
half = 1.5
show("'abc'[half]", lambda: "abc"[half])
show("(1,)[None:None]", lambda: (1,)[None:None])
show("data['a':]", lambda: data["a":])
show("data[1.0:]", lambda: data[1.0:])
show("data[10**30]", lambda: data[10 ** 30])
show("data[-10**30]", lambda: data[-10 ** 30])
show("data[10**30:]", lambda: data[10 ** 30:])
show("data[-10**30:2]", lambda: data[-10 ** 30:2])
show("data[::10**30]", lambda: data[::10 ** 30])
show("data[::0]", lambda: data[::0])
show("isinstance index result", lambda: type(Idx(4).__index__()).__name__)
