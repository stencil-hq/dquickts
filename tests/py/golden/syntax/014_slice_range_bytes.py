# Slicing ranges, bytes, bytearrays; indexing errors on them.

r = range(0, 20, 3)
print("range", list(r), len(r))
print("range slice", r[1:4], list(r[1:4]))
print("range rev", r[::-1], list(r[::-1]))
print("range step slice", r[::2], list(r[::2]))
print("range neg slice", r[-3:], list(r[-3:]))
print("range empty slice", r[5:2], len(r[5:2]))
print("range index", r[0], r[-1], r[3])
print("range of neg step", range(10, 0, -2)[1:3], list(range(10, 0, -2)[1:3]))
print("empty range slice", range(0)[:], range(0)[::-1])
print("big range", range(10**20)[-1], range(10**20)[10**19:10**19 + 3])

for i in [7, -8]:
    try:
        r[i]
    except IndexError as e:
        print("range index", i, type(e).__name__, e)

try:
    r[::0]
except ValueError as e:
    print("range step 0", type(e).__name__, e)

try:
    r["0"]
except TypeError as e:
    print("range str index", type(e).__name__, e)

try:
    range(1, 2, 0)
except ValueError as e:
    print("range() step 0", type(e).__name__, e)

try:
    len(range(10**20))
except OverflowError as e:
    print("len huge range", type(e).__name__, e)

# range equality is by sequence
print("range eq", range(0) == range(5, 5), range(0, 3) == range(0, 3, 1), range(0, 4, 2) == range(0, 3, 2))

b = b"hello world"
print("bytes slice", b[0:5], b[-5:], b[::-1], b[::2])
print("bytes index is int", b[0], b[-1], type(b[0]).__name__)
print("bytes slice is bytes", b[0:1], type(b[0:1]).__name__)
print("bytes empty", b[5:2], b[100:])
print("bytes neg step", b[8:2:-2])

for i in [11, -12]:
    try:
        b[i]
    except IndexError as e:
        print("bytes index", i, type(e).__name__, e)

try:
    b[0] = 72
except TypeError as e:
    print("bytes setitem", type(e).__name__, e)

ba = bytearray(b"abcdef")
print("bytearray slice", ba[1:4], ba[::-1], ba[0])
ba[0] = 65
print("bytearray setitem", ba)
ba[1:3] = b"XYZ"
print("bytearray slice assign grow", ba)
ba[::2] = b"1234"
print("bytearray extended assign", ba)
del ba[::3]
print("bytearray del step", ba)

try:
    ba[0] = 256
except ValueError as e:
    print("bytearray 256", type(e).__name__, e)

try:
    ba[0] = b"a"
except TypeError as e:
    print("bytearray bytes item", type(e).__name__, e)

try:
    ba[::2] = b"toolong"
except ValueError as e:
    print("bytearray extended wrong len", type(e).__name__, e)

# memoryview slicing
mv = memoryview(b"abcdef")
print("memoryview", mv[1:4].tobytes(), mv[-1], mv[::2].tobytes(), len(mv[2:]))

# slice on str subclasses returns base type
class S(str):
    pass

print("subclass slice type", type(S("abc")[1:]).__name__, type(S("abc")[0]).__name__)

class L(list):
    pass

print("list subclass slice type", type(L([1, 2])[:]).__name__)

# custom __getitem__ receives slice objects
class Show:
    def __getitem__(self, key):
        return repr(key)

sh = Show()
print("getitem key", sh[1], sh[1:2], sh[::3], sh[1:2, 3], sh[...], sh[:, None])
