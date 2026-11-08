"""L0 golden: struct packing with explicit byte orders (standard sizes only)."""
import struct

print("pack <I", struct.pack("<I", 0x01020304))
print("pack >I", struct.pack(">I", 0x01020304))
print("pack !H", struct.pack("!H", 258))
print("pack <h", struct.pack("<h", -2))
print("pack q", struct.pack("<q", -1), struct.pack(">Q", 2**64 - 1))
print("pack b/B", struct.pack("<bB", -128, 255))
print("pack ?", struct.pack("<???", True, False, 5))
print("pack f", struct.pack("<f", 1.5), struct.pack(">d", -0.0))
print("pack e", struct.pack("<e", 0.5), struct.unpack("<e", b"\x00\x3c"))
print("pack s", struct.pack("<5s", b"ab"), struct.pack("<2s", b"abcdef"))
print("pack p", struct.pack("<5p", b"abc"))
print("pack c", struct.pack("<3c", b"x", b"y", b"z"))
print("pack x", struct.pack("<BxxH", 1, 2))
print("pack count", struct.pack("<3H", 1, 2, 3))
print("pack mixed", struct.pack(">Hb?4sd", 513, -1, True, b"spam", 2.25))

print("unpack", struct.unpack("<I", b"\x04\x03\x02\x01"))
print("unpack >hh", struct.unpack(">hh", b"\xff\xfe\x00\x01"))
print("unpack f", struct.unpack("<f", struct.pack("<f", 0.1)))
print("unpack d", struct.unpack(">d", struct.pack(">d", 0.1)))
print("unpack s", struct.unpack("<3s2B", b"abc\x01\x02"))
print("unpack p", struct.unpack("<5p", b"\x03abcd"))
print("unpack ?", struct.unpack("<3?", b"\x00\x01\x02"))
print("unpack inf nan", struct.unpack(">dd", struct.pack(">dd", float("inf"), float("nan"))))

for fmt in ["<b", "<h", "<i", "<l", "<q", "<f", "<d", "<e", "<?", "<5s", "<BxH", ">IH", "!3i", "=c", "<0s", "<"]:
    print("calcsize", repr(fmt), struct.calcsize(fmt))

buf = struct.pack("<HHI", 1, 2, 3) + struct.pack("<HHI", 4, 5, 6)
print("unpack_from", struct.unpack_from("<HHI", buf, 8))
print("unpack_from offset 2", struct.unpack_from("<H", buf, offset=2))
print("iter_unpack", list(struct.iter_unpack("<HHI", buf)))
print("iter_unpack bytes", [x for (x,) in struct.iter_unpack(">H", b"\x00\x01\x00\x02\x00\x03")])

ba = bytearray(10)
struct.pack_into("<HI", ba, 2, 0xAABB, 7)
print("pack_into", ba)

s = struct.Struct(">I2s")
print("Struct", s.format, s.size)
print("Struct pack", s.pack(9, b"ok"))
print("Struct unpack", s.unpack(b"\x00\x00\x00\x09ok"))
print("Struct iter", list(s.iter_unpack(b"\x00\x00\x00\x01ab\x00\x00\x00\x02cd")))

errors = [
    lambda: struct.pack("<B", 256),
    lambda: struct.pack("<b", -129),
    lambda: struct.pack("<H", -1),
    lambda: struct.pack("<i", 2**31),
    lambda: struct.pack("<I", 1.5),
    lambda: struct.pack("<I", "x"),
    lambda: struct.pack("<II", 1),
    lambda: struct.pack("<I", 1, 2),
    lambda: struct.pack("<4s", "str"),
    lambda: struct.pack("<c", b"ab"),
    lambda: struct.unpack("<I", b"\x00\x00"),
    lambda: struct.unpack_from("<I", b"\x00\x00\x00\x00", 2),
    lambda: list(struct.iter_unpack("<I", b"\x00" * 5)),
    lambda: struct.calcsize("<Z"),
    lambda: struct.pack("<e", 1e10),
    lambda: struct.pack("<f", 1e40),
    lambda: struct.Struct("").size,
]
for i, fn in enumerate(errors):
    try:
        print("no error", i, fn())
    except (struct.error, TypeError, OverflowError, ValueError) as e:
        print("error", i, type(e).__name__, e)

print("error class", struct.error.__name__, issubclass(struct.error, Exception))
print("roundtrip", struct.unpack("<qd", struct.pack("<qd", -123456789012, 3.5)))
