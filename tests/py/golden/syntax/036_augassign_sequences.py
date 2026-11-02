# Augmented assignment on strs, lists, tuples, sets, dicts: in-place vs rebinding.

s = "ab"
alias = s
s += "cd"
print("str +=", s, alias, s is alias)
s *= 2
print("str *=", s)

lst = [1, 2]
alias = lst
lst += [3]
print("list += in place", lst, alias, lst is alias)
lst += (4, 5)
print("list += tuple", lst)
lst += "xy"
print("list += str", lst)
lst += range(2)
print("list += range", lst)
lst *= 2
print("list *= in place", len(lst), alias is lst)
lst = [1, 2]
other = lst
lst = lst + [3]
print("list = list + rebinds", lst, other, lst is other)

try:
    lst2 = [1]
    lst2 = lst2 + (2,)
except TypeError as e:
    print("list + tuple", type(e).__name__, e)

t = (1, 2)
alias = t
t += (3,)
print("tuple += rebinds", t, alias, t is alias)
t *= 2
print("tuple *=", t)
try:
    t += [4]
except TypeError as e:
    print("tuple += list", type(e).__name__, e)

# set operators
st = {1, 2, 3}
alias = st
st |= {4}
st &= {1, 2, 4, 5}
st -= {1}
st ^= {2, 9}
print("set ops in place", sorted(st), alias is st)
fs = frozenset({1})
alias = fs
fs |= {2}
print("frozenset rebinds", sorted(fs), sorted(alias), fs is alias)

# dict |=
d = {"a": 1}
alias = d
d |= {"b": 2}
d |= [("c", 3)]
print("dict |= in place", d, alias is d)
try:
    d |= 5
except TypeError as e:
    print("dict |= int", type(e).__name__, e)

# bytes and bytearray
b = b"ab"
alias = b
b += b"c"
print("bytes += rebinds", b, alias, b is alias)
ba = bytearray(b"ab")
alias = ba
ba += b"c"
ba *= 2
print("bytearray in place", ba, alias is ba)

# tuple containing a list: += mutates then fails to assign
t = ([1],)
try:
    t[0] += [2]
except TypeError as e:
    print("tuple item +=", type(e).__name__, e)
print("but list was extended", t)

# list *= 0 and negative
lst = [1, 2, 3]
alias = lst
lst *= 0
print("list *= 0", lst, alias)

# mutable default shared via +=
def append_to(x, acc=[]):
    acc += [x]
    return acc

print("shared default", append_to(1), append_to(2))

# custom __iadd__ vs __add__ fallback
class OnlyAdd:
    def __init__(self, v):
        self.v = v

    def __add__(self, other):
        print("  __add__ called")
        return OnlyAdd(self.v + other)

class WithIadd(OnlyAdd):
    def __iadd__(self, other):
        print("  __iadd__ called")
        self.v += other
        return self

a = OnlyAdd(1)
orig = a
a += 5
print("fallback to __add__", a.v, a is orig)
a = WithIadd(1)
orig = a
a += 5
print("uses __iadd__", a.v, a is orig)

class IaddNotImpl(OnlyAdd):
    def __iadd__(self, other):
        print("  __iadd__ NotImplemented")
        return NotImplemented

a = IaddNotImpl(10)
a += 1
print("iadd NotImplemented falls back", a.v, type(a).__name__)

class IaddReturnsOther:
    def __iadd__(self, other):
        return "replaced"

a = IaddReturnsOther()
a += 1
print("iadd return value is bound", repr(a))

# __radd__ used when left operand has no __iadd__/__add__ support
class R:
    def __radd__(self, other):
        return "radd(" + repr(other) + ")"

n = 5
n += R()
print("radd fallback", n)
