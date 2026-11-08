# Augmented assignment to subscripts and attributes: target expressions evaluated once.

log = []

def obj_expr(o, tag):
    log.append(tag)
    return o

def key_expr(k, tag):
    log.append(tag)
    return k

class Tracing:
    def __init__(self, data):
        self.data = data

    def __getitem__(self, k):
        log.append("get " + repr(k))
        return self.data[k]

    def __setitem__(self, k, v):
        log.append("set " + repr(k) + "=" + repr(v))
        self.data[k] = v

t = Tracing({"a": 1})
obj_expr(t, "obj")[key_expr("a", "key")] += 10
print("subscript once", log, t.data)

log.clear()
lst = [1, 2, 3]
obj_expr(lst, "list")[key_expr(-1, "idx")] *= 5
print("list item", log, lst)

log.clear()
d = {}
try:
    d["missing"] += 1
except KeyError as e:
    print("missing key", type(e).__name__, e)

# attributes
class A:
    def __init__(self):
        object.__setattr__(self, "x", 1)

    def __getattribute__(self, name):
        log.append("getattr " + name)
        return object.__getattribute__(self, name)

    def __setattr__(self, name, value):
        log.append("setattr " + name + "=" + repr(value))
        object.__setattr__(self, name, value)

log.clear()
a = A()
obj_expr(a, "obj").x += 41
print("attr once", log, object.__getattribute__(a, "x"))

class Plain:
    pass

p = Plain()
try:
    p.counter += 1
except AttributeError as e:
    print("missing attr", type(e).__name__, e)
p.counter = 0
for _ in range(3):
    p.counter += 2
print("attr counter", p.counter)

# class attribute vs instance attribute
class Counter:
    total = 0

c1 = Counter()
c1.total += 5
print("instance shadows class", c1.total, Counter.total)
Counter.total += 1
print("class attr", Counter.total, Counter().total, c1.total)

# shared mutable class attribute is mutated in place by +=
class Shared:
    items = []

s1, s2 = Shared(), Shared()
s1.items += ["x"]
print("shared list", s2.items, Shared.items, "items" in vars(s1))

# nested subscripts
grid = [[0, 0], [0, 0]]
grid[1][0] += 7
grid[-1][-1] -= 1
print("nested", grid)
dd = {"outer": {"inner": 1}}
dd["outer"]["inner"] <<= 3
print("nested dict", dd)

# slice targets with augmented assignment
lst = [1, 2, 3, 4]
lst[1:3] += ["a"]
print("slice +=", lst)
lst = [1, 2, 3, 4]
lst[::2] *= 1
print("extended slice *= 1", lst)
try:
    lst[::2] += [9]
except ValueError as e:
    print("extended slice += grows", type(e).__name__, e)

# the right-hand side is evaluated after the target's current value is fetched
log.clear()
t = Tracing({"k": 1})
def rhs():
    log.append("rhs")
    return 2
t["k"] += rhs()
print("order", log)

# augmented assignment to a global from a function
g = 1
def bump():
    global g
    g += 1
    return g

print("global", bump(), bump(), g)

# augmented assignment to a closure variable
def make_counter():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    return inc

inc = make_counter()
print("nonlocal", inc(), inc(), inc())

# string keys and augmented string building
counts = {}
for w in "a b a c a b".split():
    counts[w] = counts.get(w, 0) + 1
text = {"s": ""}
for w in sorted(counts):
    text["s"] += w + str(counts[w])
print("counts", counts, text)
