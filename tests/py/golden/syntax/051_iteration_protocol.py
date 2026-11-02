# Iteration protocol as seen by for loops: __iter__/__next__, StopIteration, iter(callable, sentinel).

class CountDown:
    def __init__(self, start):
        self.current = start

    def __iter__(self):
        return self

    def __next__(self):
        if self.current <= 0:
            raise StopIteration
        self.current -= 1
        return self.current + 1

print("custom iterator", list(CountDown(3)))
cd = CountDown(2)
for v in cd:
    print("loop value", v)
print("exhausted iterator", list(cd))

# separate iterable and iterator: re-iterable
class Range2:
    def __init__(self, n):
        self.n = n

    def __iter__(self):
        return CountDown(self.n)

r = Range2(2)
print("re-iterable", list(r), list(r))

# iter() of an iterator returns itself
it = iter([1, 2, 3])
print("iter is identity", iter(it) is it)
print("partial consume", next(it), list(it), list(it))

# next() with default
it = iter([])
print("next default", next(it, "dflt"))
try:
    next(it)
except StopIteration as e:
    print("next raises", type(e).__name__, repr(e.value))

# iter(callable, sentinel)
data = [3, 2, 1, 0, 5]
pos = [0]
def reader():
    v = data[pos[0]]
    pos[0] += 1
    return v

print("callable sentinel", list(iter(reader, 0)), pos)

# __iter__ returning a non-iterator
class Bad:
    def __iter__(self):
        return 5

try:
    for _ in Bad():
        pass
except TypeError as e:
    print("bad __iter__", type(e).__name__, e)

# __next__ raising other errors propagates
class Explodes:
    def __iter__(self):
        return self

    def __next__(self):
        raise LookupError("explode")

try:
    for _ in Explodes():
        pass
except LookupError as e:
    print("next raises other", type(e).__name__, e)

# StopIteration inside a generator body becomes RuntimeError
def gen_with_stop():
    yield 1
    raise StopIteration("inside generator")

try:
    print(list(gen_with_stop()))
except RuntimeError as e:
    print("PEP 479", type(e).__name__, e, "cause", type(e.__cause__).__name__)

# modifying a dict/set during iteration
d = {1: 1, 2: 2}
try:
    for k in d:
        d[k + 10] = 0
except RuntimeError as e:
    print("dict changed", type(e).__name__, e)

s = {1, 2}
try:
    for v in s:
        s.add(v + 10)
except RuntimeError as e:
    print("set changed", type(e).__name__, e)

# changing values (not size) during dict iteration is fine
d = {"a": 1, "b": 2}
for k in d:
    d[k] *= 10
print("dict values updated", d)

# list removal during iteration skips elements
lst = [1, 2, 3, 4]
for v in lst:
    if v % 2 == 0:
        lst.remove(v)
print("remove while iterating", lst)
lst = [1, 2, 2, 3]
for v in lst[:]:
    if v == 2:
        lst.remove(v)
print("remove from copy", lst)

# enumerate/reversed/map/filter are lazy iterators
it = map(lambda v: v * 2, [1, 2, 3])
print("map lazy", next(it), list(it))
try:
    reversed(iter([1, 2]))
except TypeError as e:
    print("reversed iterator", type(e).__name__, e)

class Rev:
    def __len__(self):
        return 3

    def __getitem__(self, i):
        return "abc"[i]

print("reversed via sequence protocol", list(reversed(Rev())))

# unpacking consumes exactly what is needed from iterators
it = iter(range(10))
a, b = next(it), next(it)
first, *rest = it
print("unpack from iterator", a, b, first, rest)
