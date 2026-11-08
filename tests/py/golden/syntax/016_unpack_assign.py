# Unpacking assignment: swaps, nested, starred, chained, from any iterable.

a, b = 1, 2
a, b = b, a
print("swap", a, b)

a, b, c = 1, 2, 3
a, b, c = c, a, b
print("rotate", a, b, c)

x = [10, 20, 30]
x[0], x[2] = x[2], x[0]
print("swap list items", x)

# right side fully evaluated before assigning
i = 0
lst = [0, 0]
i, lst[i] = 1, "set"
print("left-to-right targets", i, lst)

(a, (b, c)) = (1, (2, 3))
print("nested paren", a, b, c)
a, (b, c), d = 1, [2, 3], 4
print("nested list rhs", a, b, c, d)
[a, b] = "xy"
print("list target str", a, b)
((a, b), (c, d)) = ("ab", "cd")
print("nested strings", a, b, c, d)

# starred
first, *rest = [1, 2, 3, 4]
print("first, *rest", first, rest)
*init, last = "hello"
print("*init, last", init, last)
a, *mid, z = range(6)
print("a, *mid, z", a, mid, z)
a, *mid, z = (1, 2)
print("empty starred", a, mid, z)
*everything, = {"k": 1, "j": 2}
print("*all from dict", everything)
a, (b, *c), *d = 1, (2, 3, 4), 5, 6
print("nested starred", a, b, c, d)
print("starred type always list", type(rest).__name__, type(init).__name__)

# unpack from various iterables
a, b, c = range(3)
print("from range", a, b, c)
a, b = {"one": 1, "two": 2}
print("from dict keys", a, b)
a, b = sorted({5, 3})
print("from sorted set", a, b)
a, b, c = (n * n for n in range(1, 4))
print("from generator", a, b, c)
a, b = b"AB"
print("from bytes", a, b)
a, b = iter([7, 8])
print("from iterator", a, b)
(a,) = [42]
print("single element", a)
a, = "z"
print("trailing comma", a)

# chained assignment
a = b = c = []
a.append(1)
print("chained same object", a, b, c, a is c)
a, b = c, d = 5, 6
print("chained unpack", a, b, c, d)
x = y = 0
x += 1
print("chained ints independent", x, y)

# chained assignment evaluates targets left to right
log = []
class T:
    def __setattr__(self, name, value):
        log.append(name)
        object.__setattr__(self, name, value)

t = T()
t.p = t.q = 1
print("chained order", log)

# targets: attributes and subscripts
class O:
    pass

o = O()
o.x, o.y = 3, 4
d = {}
d["k"], d["v"] = "key", "val"
print("attr/subscript targets", o.x, o.y, d)

# swap via unpacking in a loop (fibonacci)
a, b = 0, 1
fib = []
for _ in range(10):
    fib.append(a)
    a, b = b, a + b
print("fibonacci", fib)

# underscore as throwaway
_, second, _ = (1, 2, 3)
print("underscore", second, _)

# annotated assignment
n: int = 5
m: "anything"
print("annotated", n)
try:
    print(m)
except NameError as e:
    print("annotation only", type(e).__name__, e)
