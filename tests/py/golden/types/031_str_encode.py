"""L0 golden: str.encode and bytes.decode with the built-in codecs and error handlers.

Covers utf-8 (1-4 byte sequences, surrogates), ascii and latin-1 with the
strict/ignore/replace/backslashreplace/xmlcharrefreplace/namereplace/
surrogateescape/surrogatepass handlers, utf-16/utf-32 and decode errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/031_str_encode.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


text = "aé€😀"
show("encode()", lambda: text.encode())
show("encode utf-8", lambda: text.encode("utf-8"))
show("encode UTF8 alias", lambda: text.encode("UTF8"))
show("encode utf_8 alias", lambda: text.encode("utf_8"))
show("len per char", lambda: [len(c.encode()) for c in text])
show("ascii strict", lambda: text.encode("ascii"))
show("ascii ignore", lambda: text.encode("ascii", "ignore"))
show("ascii replace", lambda: text.encode("ascii", "replace"))
show("ascii backslashreplace", lambda: text.encode("ascii", "backslashreplace"))
show("ascii xmlcharrefreplace", lambda: text.encode("ascii", "xmlcharrefreplace"))
show("ascii namereplace", lambda: text.encode("ascii", "namereplace"))
show("ascii errors kw", lambda: text.encode(encoding="ascii", errors="replace"))
show("latin-1", lambda: "aé".encode("latin-1"))
show("latin-1 strict", lambda: text.encode("latin-1"))
show("latin-1 replace", lambda: text.encode("latin-1", "replace"))
show("unknown encoding", lambda: "a".encode("no-such-codec"))
show("unknown errors", lambda: "é".encode("ascii", "no-such-handler"))
show("unknown errors unused", lambda: "a".encode("ascii", "no-such-handler"))
show("utf-16", lambda: "a😀".encode("utf-16-le"))
show("utf-16-be", lambda: "a😀".encode("utf-16-be"))
show("utf-32-le", lambda: "a😀".encode("utf-32-le"))
show("utf-16 bom len", lambda: len("a".encode("utf-16")))
show("lone surrogate strict", lambda: "a\ud800b".encode("utf-8"))
show("lone surrogate surrogatepass", lambda: "a\ud800b".encode("utf-8", "surrogatepass"))
show("lone surrogate replace", lambda: "a\ud800b".encode("utf-8", "replace"))
show("lone surrogate ignore", lambda: "a\ud800b".encode("utf-8", "ignore"))
show("lone surrogate backslash", lambda: "a\ud800b".encode("utf-8", "backslashreplace"))
show("surrogateescape encode", lambda: "a\udcffb".encode("utf-8", "surrogateescape"))
show("surrogate pair not joined", lambda: "\ud83d\ude00".encode("utf-8", "surrogatepass"))
show("encode empty", lambda: "".encode())
show("encode type", lambda: type("x".encode()).__name__)
show("encode bad arg", lambda: "x".encode(1))

data = b"a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"
show("decode()", lambda: data.decode())
show("decode utf-8", lambda: data.decode("utf-8"))
show("decode invalid start", lambda: b"a\xffb".decode())
show("decode truncated", lambda: b"a\xe2\x82".decode())
show("decode overlong", lambda: b"\xc0\xaf".decode())
show("decode surrogate bytes", lambda: b"\xed\xa0\x80".decode())
show("decode surrogatepass", lambda: b"\xed\xa0\x80".decode("utf-8", "surrogatepass"))
show("decode replace", lambda: b"a\xffb\xe2\x82".decode("utf-8", "replace"))
show("decode ignore", lambda: b"a\xffb".decode("utf-8", "ignore"))
show("decode backslashreplace", lambda: b"a\xffb".decode("utf-8", "backslashreplace"))
show("decode surrogateescape", lambda: b"a\xffb".decode("utf-8", "surrogateescape"))
show("roundtrip surrogateescape", lambda: b"a\xffb".decode("utf-8", "surrogateescape").encode("utf-8", "surrogateescape"))
show("decode ascii", lambda: b"abc".decode("ascii"))
show("decode ascii fail", lambda: b"ab\x80".decode("ascii"))
show("decode latin-1", lambda: bytes(range(0xe0, 0xe5)).decode("latin-1"))
show("decode utf-16-le", lambda: b"a\x00=\xd8\x00\xde".decode("utf-16-le"))
show("decode utf-16 odd", lambda: b"a\x00b".decode("utf-16-le"))
show("decode utf-8-sig", lambda: b"\xef\xbb\xbfhi".decode("utf-8-sig"))
show("decode utf-8 keeps bom", lambda: b"\xef\xbb\xbfhi".decode("utf-8"))
show("decode bytearray", lambda: bytearray(b"\xc3\xa9").decode())
show("decode memoryview", lambda: str(memoryview(b"ok"), "ascii"))
show("str(bytes, enc)", lambda: str(b"\xc3\xa9", "utf-8"))
show("str(bytes, enc, errors)", lambda: str(b"\xff", "utf-8", "replace"))
show("str(str, enc)", lambda: str("x", "utf-8"))

try:
    b"a\xffb".decode("utf-8")
except UnicodeDecodeError as e:
    print("decode error attrs", e.encoding, e.object, e.start, e.end, e.reason)
try:
    "aé".encode("ascii")
except UnicodeEncodeError as e:
    print("encode error attrs", e.encoding, repr(e.object), e.start, e.end, e.reason)
    print("is ValueError", isinstance(e, ValueError), isinstance(e, UnicodeError))
