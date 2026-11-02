# Slicing strings: bounds, negative indices, steps, out-of-range, empty results.

s = "abcdefghij"
print("full", repr(s[:]))
print("[2:5]", repr(s[2:5]))
print("[:3]", repr(s[:3]))
print("[7:]", repr(s[7:]))
print("[-3:]", repr(s[-3:]))
print("[:-3]", repr(s[:-3]))
print("[-5:-2]", repr(s[-5:-2]))
print("[2:-2]", repr(s[2:-2]))
print("[-2:2] empty", repr(s[-2:2]))
print("[5:5] empty", repr(s[5:5]))
print("[6:3] empty", repr(s[6:3]))
print("[::2]", repr(s[::2]))
print("[1::2]", repr(s[1::2]))
print("[::3]", repr(s[::3]))
print("[::-1]", repr(s[::-1]))
print("[::-2]", repr(s[::-2]))
print("[8:2:-1]", repr(s[8:2:-1]))
print("[8:2:-3]", repr(s[8:2:-3]))
print("[2:8:-1] empty", repr(s[2:8:-1]))
print("[-1:-4:-1]", repr(s[-1:-4:-1]))
print("[:-4:-1]", repr(s[:-4:-1]))
print("[3::-1]", repr(s[3::-1]))
print("[:3:-1]", repr(s[:3:-1]))

# out-of-range bounds are clamped
print("[0:100]", repr(s[0:100]))
print("[-100:3]", repr(s[-100:3]))
print("[100:]", repr(s[100:]))
print("[-100:-50]", repr(s[-100:-50]))
print("[100::-1]", repr(s[100::-1]))
print("[-100::-1]", repr(s[-100::-1]))
print("[::100]", repr(s[::100]))
print("[::-100]", repr(s[::-100]))
print("huge bounds", repr(s[-10**30:10**30]))
print("huge step", repr(s[::10**30]))

# None in slices
print("[None:None]", repr(s[None:None]))
print("[None:4:None]", repr(s[None:4:None]))
print("[None:None:-1]", repr(s[None:None:-1]))

# slice objects
sl = slice(1, 7, 2)
print("slice obj", repr(s[sl]), sl.start, sl.stop, sl.step)
print("slice repr", repr(sl), repr(slice(3)), repr(slice(None, None, -1)))
print("slice indices", sl.indices(10), slice(None, None, -1).indices(10), slice(-3, 100).indices(5))
print("slice eq", slice(1, 2) == slice(1, 2), slice(1, 2) == slice(1, 3))

# step zero error
try:
    s[::0]
except ValueError as e:
    print("step 0", type(e).__name__, e)

# non-integer bounds
for bad in [1.5, "1", [1]]:
    try:
        s[bad:]
    except TypeError as e:
        print("bad bound", repr(bad), type(e).__name__, e)

# __index__ objects work as bounds
class Two:
    def __index__(self):
        return 2

print("__index__ bound", repr(s[Two():6]), repr(s[::Two()]), repr(s[Two()]))
print("bool bounds", repr(s[True:]), repr(s[False:True]))

# empty string slicing
e = ""
print("empty slices", repr(e[:]), repr(e[1:]), repr(e[::-1]), repr(e[-1:]))

# unicode
u = "héllo wörld ✓"
print("unicode slice", repr(u[1:5]), repr(u[-1]), repr(u[::-1]))
print("astral", repr("a\U0001F600b"[1]), len("a\U0001F600b"))

# indexing
print("index 0 and -1", repr(s[0]), repr(s[-1]), repr(s[-10]))
for i in [10, -11, 10**20]:
    try:
        s[i]
    except IndexError as e:
        print("index", i, type(e).__name__, e)

try:
    s["a"]
except TypeError as e:
    print("str index", type(e).__name__, e)

try:
    s[1.0]
except TypeError as e:
    print("float index", type(e).__name__, e)

# strings are immutable
try:
    s[0] = "z"
except TypeError as e:
    print("str setitem", type(e).__name__, e)

try:
    del s[0]
except TypeError as e:
    print("str delitem", type(e).__name__, e)

# common idioms
word = "racecar"
print("palindrome", word == word[::-1])
print("drop first/last", repr(word[1:-1]))
print("every other reversed", repr(word[::-2]))
print("rotate", repr(s[3:] + s[:3]))
print("chunks", [s[i:i + 3] for i in range(0, len(s), 3)])
