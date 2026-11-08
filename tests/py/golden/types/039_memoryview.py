"""L0 golden: memoryview basics over bytes and bytearray.

Covers len, indexing (int), slicing (zero-copy views), tobytes, tolist,
hex, readonly flag, writes through a bytearray view, comparisons, format/
itemsize/nbytes/ndim/shape, release() and resize-while-exported errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/039_memoryview.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


m = memoryview(b"abcdef")
show("len", lambda: len(m))
show("m[0]", lambda: m[0])
show("m[-1]", lambda: m[-1])
show("m[10]", lambda: m[10])
show("type slice", lambda: type(m[1:3]).__name__)
show("slice tobytes", lambda: m[1:3].tobytes())
show("step slice tobytes", lambda: m[::2].tobytes())
show("reverse slice tolist", lambda: m[::-1].tolist())
show("tobytes", lambda: m.tobytes())
show("tolist", lambda: m.tolist())
show("hex", lambda: m.hex())
show("hex sep", lambda: m[:3].hex(":"))
show("bytes(m)", lambda: bytes(m))
show("readonly", lambda: m.readonly)
show("format", lambda: m.format)
show("itemsize", lambda: m.itemsize)
show("nbytes", lambda: m.nbytes)
show("ndim", lambda: m.ndim)
show("shape", lambda: m.shape)
show("strides", lambda: m.strides)
show("contiguous", lambda: m.contiguous)
show("obj is source", lambda: m.obj == b"abcdef")
show("write readonly", lambda: m.__setitem__(0, 1))
show("eq bytes", lambda: m == b"abcdef")
show("eq slice", lambda: m[:2] == b"ab")
show("eq list", lambda: m == [97, 98, 99, 100, 101, 102])
show("eq memoryview", lambda: m == memoryview(b"abcdef"))
show("lt", lambda: m < b"b")
show("in", lambda: 97 in m)
show("iter", lambda: list(iter(m[:3])))
show("hash readonly", lambda: hash(memoryview(b"ab")) == hash(b"ab"))

ba = bytearray(b"hello")
mv = memoryview(ba)
mv[0] = ord("J")
show("write through", lambda: ba)
mv[1:3] = b"EE"
show("slice write", lambda: ba)
show("slice write size mismatch", lambda: mv.__setitem__(slice(0, 2), b"xyz"))
show("write out of range", lambda: mv.__setitem__(0, 300))
show("readonly bytearray view", lambda: mv.readonly)
sub = mv[2:]
sub[0] = ord("!")
show("sub view write", lambda: ba)
show("hash writable", lambda: hash(mv))
show("resize while exported", lambda: ba.append(1))
show("toreadonly", lambda: mv.toreadonly().readonly)
mv.release()
sub.release()
show("after release len", lambda: len(mv))
show("after release tobytes", lambda: mv.tobytes())
ba.append(33)
show("resize after release", lambda: ba)

with memoryview(b"ctx") as cm:
    show("context manager", lambda: cm.tobytes())
show("context released", lambda: cm[0])

show("memoryview(str)", lambda: memoryview("abc"))
show("memoryview(int)", lambda: memoryview(5))
show("memoryview(memoryview)", lambda: memoryview(memoryview(b"xy")).tobytes())
show("cast B->c", lambda: memoryview(b"ab").cast("c").tolist())
show("cast B->H len", lambda: len(memoryview(b"abcd").cast("H")))
show("cast bad", lambda: memoryview(b"abc").cast("H"))
show("int.from_bytes(mv)", lambda: int.from_bytes(memoryview(b"\x01\x00"), "little"))
show("bytearray(mv)", lambda: bytearray(memoryview(b"q")))
show("empty", lambda: (len(memoryview(b"")), memoryview(b"").tolist()))
