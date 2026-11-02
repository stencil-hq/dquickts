# io.BytesIO.truncate converts its argument before re-checking closed and
# exported buffers; __init__ refuses to drop an exported buffer.
import io


class Size:
    def __init__(self, action, value=0):
        self.action = action
        self.value = value

    def __index__(self):
        self.result = self.action()
        return self.value


stream = io.BytesIO(b"abc")
try:
    stream.truncate(Size(stream.close))
except ValueError as e:
    print("closed", e)

stream = io.BytesIO(b"abc")
size = Size(stream.getbuffer)
try:
    stream.truncate(size)
except BufferError as e:
    print("export", e)
print(stream.getvalue())
size.result.release()

stream = io.BytesIO(b"abcd")
with stream.getbuffer() as view:
    for call in [lambda: stream.truncate(5), lambda: stream.truncate(), lambda: stream.truncate(1),
                 lambda: stream.__init__(b"replacement"), lambda: stream.write(b"xyz12"),
                 lambda: stream.close()]:
        try:
            print(call())
        except BufferError as e:
            print("BufferError", e)
    print(stream.tell(), stream.getvalue(), bytes(view))
stream.write(b"X")
print(stream.getvalue())

s = io.BytesIO(b"abcdef")
print(s.truncate(10), s.getvalue(), s.truncate(2), s.getvalue(), s.tell())
for arg in ["x", 1.5, -1, 2**70, None]:
    try:
        print(arg, io.BytesIO(b"abc").truncate(arg))
    except Exception as e:
        print(arg, type(e).__name__, e)
s = io.BytesIO(b"abc")
s.close()
try:
    s.truncate("x")
except ValueError as e:
    print(e)
print(io.BytesIO.truncate.__doc__)
