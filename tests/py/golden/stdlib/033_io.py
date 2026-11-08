"""L0 golden: io.StringIO / io.BytesIO and text wrappers in memory."""
import io

s = io.StringIO()
print("write returns", s.write("hello "), s.write("world\n"), s.write(""))
print("tell", s.tell())
print("getvalue", repr(s.getvalue()))
print("read at end", repr(s.read()))
s.seek(0)
print("read 5", repr(s.read(5)), s.tell())
print("readline", repr(s.readline()), repr(s.readline()))
s.seek(6)
s.write("W")
print("overwrite", repr(s.getvalue()))
s.seek(0, io.SEEK_END)
s.write("more")
print("append", repr(s.getvalue()))
s.seek(3)
print("truncate", s.truncate(), repr(s.getvalue()), s.tell())
s.seek(10)
s.write("x")
print("write past end pads", repr(s.getvalue()))

s2 = io.StringIO("line1\nline2\r\nline3")
print("initial pos", s2.tell(), "lines", s2.readlines())
s2.seek(0)
print("iterate", [line for line in s2])
print("newline=None", repr(io.StringIO("a\r\nb\rc", newline=None).read()))
print("newline=''", io.StringIO("a\r\nb\rc", newline="").readlines())
print("newline=crlf write", repr(io.StringIO(newline="\r\n").getvalue()))
nw = io.StringIO(newline="\r\n")
nw.write("x\ny\n")
print("crlf translate", repr(nw.getvalue()))
print("flags", s.readable(), s.writable(), s.seekable(), s.closed)
try:
    s.seek(-1)
except ValueError as e:
    print("negative seek", type(e).__name__, e)
try:
    s.seek(1, io.SEEK_CUR)
except OSError as e:
    print("relative seek", type(e).__name__, e)
s.close()
try:
    s.getvalue()
except ValueError as e:
    print("closed", type(e).__name__, e)
try:
    io.StringIO(b"bytes")
except TypeError as e:
    print("bytes init", type(e).__name__, e)
with io.StringIO("ctx") as cm:
    print("context", cm.read())
print("context closed", cm.closed)

b = io.BytesIO(b"0123456789")
print("bytes read", b.read(3), b.tell())
print("seek", b.seek(-2, io.SEEK_END), b.read())
print("seek cur", b.seek(-5, io.SEEK_CUR), b.read(2))
b.seek(0)
b.write(b"AB")
print("write", b.getvalue())
b.seek(20)
b.write(b"Z")
print("write past end", b.getvalue())
print("truncate", b.truncate(4), b.getvalue(), b.tell())
buf = bytearray(3)
b.seek(0)
print("readinto", b.readinto(buf), buf)
print("read1", b.read1(), b.read1())
view = b.getbuffer()
view[0] = ord("q")
print("getbuffer", bytes(view), view.nbytes)
try:
    b.write(b"resize")
except BufferError as e:
    print("export locks", type(e).__name__, e)
view.release()
b.write(b"!")
print("after release", b.getvalue())
lines = io.BytesIO(b"a\nbb\r\nccc")
print("bytes lines", lines.readlines())
print("bytes iterate", list(io.BytesIO(b"x\ny")))
try:
    io.BytesIO("text")
except TypeError as e:
    print("str init", type(e).__name__, e)
try:
    io.BytesIO().write("str")
except TypeError as e:
    print("write str", type(e).__name__, e)

raw = io.BytesIO()
tw = io.TextIOWrapper(raw, encoding="utf-8", newline="\n", write_through=True)
tw.write("café\n")
tw.flush()
print("TextIOWrapper bytes", raw.getvalue())
tr = io.TextIOWrapper(io.BytesIO(b"\xff\xfeline"), encoding="utf-8", errors="replace")
print("TextIOWrapper decode", repr(tr.read()))
br = io.BufferedReader(io.BytesIO(b"buffered data"))
print("BufferedReader", br.peek(1)[:1], br.read(8), br.read())
print("classes", issubclass(io.StringIO, io.TextIOBase), issubclass(io.BytesIO, io.BufferedIOBase))
print("UnsupportedOperation", issubclass(io.UnsupportedOperation, OSError), issubclass(io.UnsupportedOperation, ValueError))
try:
    io.BufferedReader(io.BytesIO(b"")).write(b"x")
except io.UnsupportedOperation as e:
    print("read-only write", type(e).__name__, e)
print("open_code exists", callable(io.open_code), io.DEFAULT_BUFFER_SIZE > 0)
