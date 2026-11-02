# Iterator protocol: custom iterators, iterables vs iterators, iter(callable, sentinel),
# next() with defaults, and iter() errors.


class CountUp:
    """An iterator: __iter__ returns self, __next__ raises StopIteration when done."""

    def __init__(self, limit):
        self.limit = limit
        self.current = 0

    def __iter__(self):
        return self

    def __next__(self):
        if self.current >= self.limit:
            raise StopIteration
        self.current += 1
        return self.current


it = CountUp(3)
print("iterator is own iter", iter(it) is it)
print("list", list(it))
print("single use", list(it))
print("next default", next(it, "done"))


class Range3:
    """An iterable: each __iter__ call returns a fresh iterator."""

    def __iter__(self):
        return CountUp(3)


r = Range3()
print("reiterable", list(r), list(r))
print("nested loops", [(a, b) for a in r for b in r if a < b])
it1, it2 = iter(r), iter(r)
print("independent iterators", next(it1), next(it1), next(it2))

counter = {"n": 0}


def tick():
    counter["n"] += 1
    return counter["n"]


print("iter callable sentinel", list(iter(tick, 4)))
print("sentinel never hit stops by break", end=" ")
for v in iter(tick, -1):
    if v > 6:
        break
print(v)
items = iter(["a", "b", "STOP", "c"])
print("sentinel with next method", list(iter(items.__next__, "STOP")), list(items))
try:
    iter(5, 0)
except TypeError as e:
    print(type(e).__name__, e)

for obj in [5, 3.5, None, len]:
    try:
        iter(obj)
    except TypeError as e:
        print(type(e).__name__, e)
for obj in [[1], "s", (1,)]:
    try:
        next(obj)
    except TypeError as e:
        print(type(e).__name__, e)

e_it = iter([])
print("next default on empty", next(e_it, None), next(e_it, 0))
try:
    next(e_it)
except StopIteration as e:
    print("next no default", type(e).__name__, e.args)

lst = [1, 2, 3]
li = iter(lst)
next(li)
lst.append(4)
print("list iterator sees appends", list(li))
d = {"a": 1}
di = iter(d)
d["b"] = 2
try:
    next(di)
except RuntimeError as e:
    print(type(e).__name__, e)
s = {1}
si = iter(s)
s.add(2)
try:
    next(si)
except RuntimeError as e:
    print(type(e).__name__, e)

print("iterator types", [type(iter(x)).__name__ for x in ([], (), {}, range(1), {}.items())])


class Fib:
    def __init__(self):
        self.a, self.b = 0, 1

    def __iter__(self):
        return self

    def __next__(self):
        value = self.a
        self.a, self.b = self.b, self.a + self.b
        return value


f = Fib()
print("infinite iterator", [next(f) for _ in range(10)])
print("for with break", [x for x, _ in zip(Fib(), range(5))])


class RaisesInNext:
    def __iter__(self):
        return self

    def __next__(self):
        raise ValueError("next failed")


try:
    for _ in RaisesInNext():
        pass
except ValueError as e:
    print("error from __next__", type(e).__name__, e)
print("unpacking iterator", *CountUp(4))
a, *b = CountUp(4)
print("star unpack", a, b)
try:
    x, y = CountUp(3)
except ValueError as e:
    print(type(e).__name__, e)
print("dict from iterator of pairs", dict(zip(CountUp(3), "abc")))
