# Slice assignment (grow, shrink, extended slices) and del on slices/items.

def fresh():
    return list(range(10))

a = fresh()
a[2:5] = ["x"]
print("shrink", a)

a = fresh()
a[2:3] = ["x", "y", "z"]
print("grow", a)

a = fresh()
a[3:3] = ["ins1", "ins2"]
print("insert at 3", a)

a = fresh()
a[100:200] = ["far"]
print("out-of-range slice assign appends", a)

a = fresh()
a[-3:] = []
print("clear tail", a)

a = fresh()
a[:] = "abc"
print("replace all with str iterable", a)

a = fresh()
a[1:4] = range(3)
print("assign range", a)

a = fresh()
a[2:6] = (c for c in "gen")
print("assign generator", a)

a = fresh()
a[5:2] = ["between"]
print("reversed bounds insert", a)

# assigning a list slice to itself
a = [1, 2, 3]
a[1:] = a
print("self slice assign", a)

# extended slices require same length
a = fresh()
a[::2] = ["e"] * 5
print("extended assign", a)

a = fresh()
a[::-3] = "WXYZ"
print("extended reverse assign", a)

a = fresh()
try:
    a[::2] = [1, 2, 3]
except ValueError as e:
    print("extended wrong len", type(e).__name__, e)

a = fresh()
a[::1] = [7]
print("step 1 is not extended", a)

try:
    a = fresh()
    a[1:3] = 5
except TypeError as e:
    print("assign non-iterable", type(e).__name__, e)

try:
    a = fresh()
    a[::2] = None
except TypeError as e:
    print("extended non-iterable", type(e).__name__, e)

# item assignment
a = fresh()
a[0] = "first"
a[-1] = "last"
print("item assign", a)
for i in [10, -11]:
    try:
        a[i] = 0
    except IndexError as e:
        print("item assign", i, type(e).__name__, e)

# del slices
a = fresh()
del a[2:5]
print("del [2:5]", a)

a = fresh()
del a[::2]
print("del [::2]", a)

a = fresh()
del a[::-3]
print("del [::-3]", a)

a = fresh()
del a[:]
print("del [:]", a)

a = fresh()
del a[50:60]
print("del out of range slice", a)

a = fresh()
del a[-1]
del a[0]
print("del items", a)

try:
    del a[100]
except IndexError as e:
    print("del item out of range", type(e).__name__, e)

# del multiple targets
a = fresh()
del a[0], a[0]
print("del two targets", a)

# dict item assignment and deletion
d = {"a": 1}
d["b"] = 2
del d["a"]
print("dict assign/del", d)
try:
    del d["zz"]
except KeyError as e:
    print("dict del missing", type(e).__name__, e)

# custom __setitem__/__delitem__ receive slice objects
class Rec:
    def __setitem__(self, k, v):
        print("  setitem", repr(k), repr(v))

    def __delitem__(self, k):
        print("  delitem", repr(k))

r = Rec()
r[1:2] = "v"
r[::-1] = []
r[1, 2:3] = 0
del r[:5]
del r[..., 0]
