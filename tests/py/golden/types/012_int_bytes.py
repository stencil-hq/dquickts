"""L0 golden: int.to_bytes and int.from_bytes.

Covers byteorder big/little, signed two's complement, length defaults,
OverflowError for values that do not fit, and inputs from various buffers.

Usage: python3.15 -I -X utf8 tests/py/golden/types/012_int_bytes.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("(1).to_bytes()", lambda: (1).to_bytes())
show("(255).to_bytes()", lambda: (255).to_bytes())
show("(0).to_bytes(0)", lambda: (0).to_bytes(0))
show("(256).to_bytes()", lambda: (256).to_bytes())
show("(1024).to_bytes(2, 'big')", lambda: (1024).to_bytes(2, "big"))
show("(1024).to_bytes(2, 'little')", lambda: (1024).to_bytes(2, "little"))
show("(1024).to_bytes(4)", lambda: (1024).to_bytes(4))
show("(1024).to_bytes(4, byteorder='little')", lambda: (1024).to_bytes(4, byteorder="little"))
show("(1024).to_bytes(1)", lambda: (1024).to_bytes(1))
show("(-1).to_bytes(2)", lambda: (-1).to_bytes(2))
show("(-1).to_bytes(2, signed=True)", lambda: (-1).to_bytes(2, signed=True))
show("(-128).to_bytes(1, signed=True)", lambda: (-128).to_bytes(1, signed=True))
show("(-129).to_bytes(1, signed=True)", lambda: (-129).to_bytes(1, signed=True))
show("(128).to_bytes(1, signed=True)", lambda: (128).to_bytes(1, signed=True))
show("(127).to_bytes(1, signed=True)", lambda: (127).to_bytes(1, signed=True))
show("(-256).to_bytes(4, 'little', signed=True)", lambda: (-256).to_bytes(4, "little", signed=True))
show("(2**64-1).to_bytes(8)", lambda: (2 ** 64 - 1).to_bytes(8))
show("(2**64).to_bytes(8)", lambda: (2 ** 64).to_bytes(8))
show("(2**100).to_bytes(13, 'little')", lambda: (2 ** 100).to_bytes(13, "little"))
show("(-2**100).to_bytes(13, signed=True)", lambda: (-2 ** 100).to_bytes(13, signed=True))
show("(1).to_bytes(-1)", lambda: (1).to_bytes(-1))
show("(1).to_bytes(1, 'middle')", lambda: (1).to_bytes(1, "middle"))
show("(1).to_bytes(1, 1)", lambda: (1).to_bytes(1, 1))
show("True.to_bytes(2)", lambda: True.to_bytes(2))
show("type of to_bytes", lambda: type((5).to_bytes(1)).__name__)

show("from_bytes(b'\\x04\\x00')", lambda: int.from_bytes(b"\x04\x00"))
show("from_bytes(b'\\x04\\x00', 'little')", lambda: int.from_bytes(b"\x04\x00", "little"))
show("from_bytes(b'')", lambda: int.from_bytes(b""))
show("from_bytes(b'', signed=True)", lambda: int.from_bytes(b"", signed=True))
show("from_bytes(b'\\xff')", lambda: int.from_bytes(b"\xff"))
show("from_bytes(b'\\xff', signed=True)", lambda: int.from_bytes(b"\xff", signed=True))
show("from_bytes(b'\\x80\\x00', signed=True)", lambda: int.from_bytes(b"\x80\x00", signed=True))
show("from_bytes(b'\\x00\\x80', 'little', signed=True)", lambda: int.from_bytes(b"\x00\x80", "little", signed=True))
show("from_bytes(b'\\xff'*9, signed=True)", lambda: int.from_bytes(b"\xff" * 9, signed=True))
show("from_bytes(b'\\xff'*9)", lambda: int.from_bytes(b"\xff" * 9))
show("from_bytes(b'\\x01' + b'\\x00'*16)", lambda: int.from_bytes(b"\x01" + b"\x00" * 16))
show("from_bytes([1, 2, 3])", lambda: int.from_bytes([1, 2, 3]))
show("from_bytes((255, 0), 'little')", lambda: int.from_bytes((255, 0), "little"))
show("from_bytes(bytearray(b'\\x01\\x02'))", lambda: int.from_bytes(bytearray(b"\x01\x02")))
show("from_bytes(memoryview(b'\\x01\\x02'))", lambda: int.from_bytes(memoryview(b"\x01\x02")))
show("from_bytes([256])", lambda: int.from_bytes([256]))
show("from_bytes('ab')", lambda: int.from_bytes("ab"))
show("from_bytes(5)", lambda: int.from_bytes(5))
show("from_bytes(b'\\x01', 'mid')", lambda: int.from_bytes(b"\x01", "mid"))
show("bool.from_bytes(b'\\x01')", lambda: bool.from_bytes(b"\x01"))
show("bool.from_bytes(b'\\x00')", lambda: bool.from_bytes(b"\x00"))
show("type(bool.from_bytes(b'\\x05'))", lambda: type(bool.from_bytes(b"\x05")).__name__)

for v in (0, 1, -1, 127, -128, 255, -32768, 2 ** 63 - 1, -2 ** 63, 12345678901234567890, -98765432109876543210):
    for order in ("big", "little"):
        n = (v.bit_length() + 8) // 8
        data = v.to_bytes(n, order, signed=True)
        print("roundtrip", v, order, data.hex(), int.from_bytes(data, order, signed=True) == v)
