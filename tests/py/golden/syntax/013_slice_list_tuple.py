# Slicing lists and tuples: copies, steps, bounds, nested, indexing errors.

lst = list(range(10))
print("full copy", lst[:], lst[:] is not lst, lst[:] == lst)
print("[3:7]", lst[3:7])
print("[-4:]", lst[-4:])
print("[:-7]", lst[:-7])
print("[::2]", lst[::2])
print("[1::3]", lst[1::3])
print("[::-1]", lst[::-1])
print("[7:1:-2]", lst[7:1:-2])
print("[-1:-5:-1]", lst[-1:-5:-1])
print("[5:2] empty", lst[5:2])
print("[2:5:-1] empty", lst[2:5:-1])
print("[20:30] empty", lst[20:30])
print("[-20:2]", lst[-20:2])
print("[-20::-1] empty", lst[-20::-1])
print("[20::-4]", lst[20::-4])

t = tuple("python")
print("tuple [1:4]", t[1:4])
print("tuple [::-1]", t[::-1])
print("tuple [::2]", t[::2])
print("tuple empty", t[4:1], type(t[4:1]).__name__)
print("tuple full copy eq", t[:] == t, type(t[:]).__name__)
print("tuple index", t[0], t[-1], t[-6])

# slices of slices
print("chain slice", lst[2:9][1:-1][::2])

# slices are shallow copies
nested = [[1, 2], [3, 4]]
copy = nested[:]
copy[0].append(99)
copy[1] = "replaced"
print("shallow copy", nested, copy)

# reversing a list via slice vs reversed vs reverse()
a = [1, 2, 3]
b = a[::-1]
a.reverse()
print("reverse ways", a, b, list(reversed(a)))

# negative indexing
print("neg index", lst[-1], lst[-10], [None][-1])
for i in [10, -11, 99]:
    try:
        lst[i]
    except IndexError as e:
        print("list index", i, type(e).__name__, e)

for i in [6, -7]:
    try:
        t[i]
    except IndexError as e:
        print("tuple index", i, type(e).__name__, e)

try:
    [][0]
except IndexError as e:
    print("empty list index", type(e).__name__, e)

try:
    ()[0]
except IndexError as e:
    print("empty tuple index", type(e).__name__, e)

try:
    lst["1"]
except TypeError as e:
    print("list str index", type(e).__name__, e)

try:
    t[1.5]
except TypeError as e:
    print("tuple float index", type(e).__name__, e)

try:
    lst[::0]
except ValueError as e:
    print("list step 0", type(e).__name__, e)

try:
    lst[1, 2]
except TypeError as e:
    print("list tuple index", type(e).__name__, e)

try:
    lst[None]
except TypeError as e:
    print("list None index", type(e).__name__, e)

# indexing with bool and __index__
class Idx:
    def __init__(self, v):
        self.v = v

    def __index__(self):
        return self.v

print("bool index", lst[True], lst[False])
print("__index__", lst[Idx(3)], lst[Idx(-2)], lst[Idx(1):Idx(4)])

# nested indexing
grid = [[1, 2, 3], [4, 5, 6]]
print("grid", grid[1][2], grid[-1][-3], grid[0][1:], [row[1] for row in grid])

# tuple immutability
try:
    t[0] = "P"
except TypeError as e:
    print("tuple setitem", type(e).__name__, e)

try:
    del t[0]
except TypeError as e:
    print("tuple delitem", type(e).__name__, e)

# slice with large step on short list
print("step > len", [1, 2, 3][::5], [1, 2, 3][::-5])

# list of slices via comprehension
print("windows", [lst[i:i + 3] for i in range(0, 8, 3)])
print("pairs", list(zip(lst[::2], lst[1::2])))
