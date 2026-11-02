"""L0 golden: slice objects and slice semantics on sequences.

Covers slice() construction and repr, start/stop/step attributes,
indices() normalisation (negative, out of range, negative step, zero step
error), equality/hashing, and applying slice objects to list/str/tuple/
bytes/range.

Usage: python3.15 -I -X utf8 tests/py/golden/types/052_slice.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("slice(5)", lambda: slice(5))
show("slice(1, 10)", lambda: slice(1, 10))
show("slice(1, 10, 2)", lambda: slice(1, 10, 2))
show("slice(None)", lambda: slice(None))
show("slice('a', [1], 2.5)", lambda: slice("a", [1], 2.5))
show("slice()", lambda: slice())
show("slice(1, 2, 3, 4)", lambda: slice(1, 2, 3, 4))
s = slice(1, 10, 2)
show("attrs", lambda: (s.start, s.stop, s.step))
show("slice(5) attrs", lambda: (slice(5).start, slice(5).stop, slice(5).step))
show("readonly", lambda: setattr(s, "start", 0))

for args, length in (((1, 10, 2), 5), ((None, None, None), 5), ((-3, None, None), 5),
                     ((None, -1, None), 5), ((-100, 100, None), 5), ((None, None, -1), 5),
                     ((10, -10, -2), 5), ((3, 1, None), 5), ((2, 2, None), 5),
                     ((None, None, 3), 10), ((None, None, -3), 10), ((5, None, -1), 10),
                     ((None, 5, -1), 10), ((0, 0, -1), 0), ((1, 2, 2 ** 70), 10),
                     ((-2 ** 70, 2 ** 70, None), 3)):
    show("slice%r.indices(%d)" % (args, length), lambda: slice(*args).indices(length))

show("indices step 0", lambda: slice(None, None, 0).indices(5))
show("indices negative length", lambda: slice(1).indices(-1))
show("indices float", lambda: slice(1.5).indices(5))
show("indices huge length", lambda: slice(None).indices(2 ** 70))
show("range(*indices)", lambda: list(range(*slice(None, None, -2).indices(7))))

show("slice eq", lambda: slice(1, 2) == slice(1, 2))
show("slice eq none", lambda: slice(1, 2) == slice(1, 2, None))
show("slice ne", lambda: slice(1, 2) != slice(1, 3))
show("slice lt", lambda: slice(1, 2) < slice(1, 3))
show("slice hash eq", lambda: hash(slice(1, 2)) == hash(slice(1, 2)))
show("slice unhashable parts", lambda: hash(slice([], 1)))
show("slice as dict key", lambda: {slice(1, 2): "x"}[slice(1, 2)])

data = list(range(10))
show("list[s]", lambda: data[s])
show("list[slice(None, None, -1)]", lambda: data[slice(None, None, -1)])
show("list[slice(-3, None)]", lambda: data[slice(-3, None)])
show("str[s]", lambda: "abcdefghij"[s])
show("tuple[s]", lambda: tuple(data)[s])
show("bytes[s]", lambda: b"abcdefghij"[s])
show("range[s]", lambda: range(10)[s])
show("list[slice('a')]", lambda: data[slice("a")])
show("list[slice(0, 5, 0)]", lambda: data[slice(0, 5, 0)])
show("list[::-1][:3]", lambda: data[::-1][:3])
show("list[-1:-4:-1]", lambda: data[-1:-4:-1])
show("list[8:2:-2]", lambda: data[8:2:-2])
show("list[2:8:-1]", lambda: data[2:8:-1])
show("list[True:]", lambda: data[True:])
show("str[::2] non-BMP", lambda: "a😀b😀c"[::2])
show("__getitem__ with slice", lambda: data.__getitem__(slice(2, 4)))
show("slice in subscript repr", lambda: type(data[1:2]).__name__)


class Recorder:
    """Records the key passed to __getitem__."""

    def __getitem__(self, key):
        return key


rec = Recorder()
show("obj[1:2]", lambda: rec[1:2])
show("obj[::]", lambda: rec[::])
show("obj[:, 1]", lambda: rec[:, 1])
show("obj[1:2, ::3]", lambda: rec[1:2, ::3])
show("obj[...]", lambda: rec[...])
show("obj['a':'b']", lambda: rec["a":"b"])
show("obj[-1:]", lambda: rec[-1:])
