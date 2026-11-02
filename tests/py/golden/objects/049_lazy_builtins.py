# Laziness and exhaustion of zip/map/filter/enumerate/reversed/range iterators.

log = []


def noisy(name, items):
    for item in items:
        log.append(name + ":" + repr(item))
        yield item


m = map(lambda x: x * 2, noisy("m", [1, 2, 3]))
print("map lazy", type(m).__name__, log)
print("map first", next(m), log)
print("map rest", list(m), "exhausted", list(m))
log.clear()

f = filter(lambda x: x > 1, noisy("f", [1, 2, 3]))
print("filter lazy", log)
print("filter first", next(f), log)
log.clear()

z = zip(noisy("a", [1, 2, 3]), noisy("b", "xy"))
print("zip", list(z))
print("zip pull order", log)
log.clear()
z = zip(noisy("a", [1, 2]), noisy("b", "xyz"))
print("zip shorter first", list(z), log)
log.clear()

print("zip empty", list(zip()), list(zip([])), list(zip([1], [])))
print("zip three", list(zip("ab", [1, 2], (True, False))))
try:
    list(zip([1, 2], [1], strict=True))
except ValueError as e:
    print(type(e).__name__, e)
try:
    list(zip([1], [1, 2], strict=True))
except ValueError as e:
    print(type(e).__name__, e)
print("zip strict ok", list(zip("ab", "cd", strict=True)))
print("unzip", list(zip(*[(1, "a"), (2, "b")])))

e = enumerate(noisy("e", "ab"), start=10)
print("enumerate lazy", log)
print("enumerate", list(e), list(e))
print("enumerate negative start", list(enumerate("xy", -1)))
log.clear()

rv = reversed([1, 2, 3])
print("reversed type", type(rv).__name__, list(rv), list(rv))
print("reversed range", list(reversed(range(1, 10, 3))))
print("reversed str", "".join(reversed("hello")))
print("reversed dict", list(reversed({"a": 1, "b": 2})))
lst = [1, 2, 3]
rl = reversed(lst)
lst.pop()
print("reversed after shrink", list(rl))

rg = range(3)
print("range reusable", list(rg), list(rg), len(rg), rg[-1])
ri = iter(rg)
print("range iterator single use", list(ri), list(ri))
print("range slicing", range(10)[2:8:2], list(range(10)[::-3]))
print("range contains", 4 in range(0, 10, 2), 5 in range(0, 10, 2), 10**20 in range(10**21))
print("range repr", range(5), range(1, 5), range(1, 5, 2))
print("range empty", list(range(5, 0)), bool(range(0)), range(5, 0) == range(0))

src = iter([1, 2, 3, 4, 5, 6])
print("shared iterator zip", list(zip(src, src)))
src = iter(range(5))
print("map over shared", list(map(lambda a, b: a + b, src, src)))
print("filter None", list(filter(None, [0, "", None, 3, "x", [], [1]])))
print("map multiple stops shortest", list(map(pow, [2, 3, 4], [5, 2])))

consumed = iter(range(10))
print("any consumes until true", any(x == 3 for x in consumed), next(consumed))
consumed = iter(range(10))
print("in consumes until found", 4 in consumed, next(consumed))

squares = (x * x for x in range(4))
print("sum then empty", sum(squares), sum(squares))
words = map(str.upper, ["a", "b"])
print("map consumed by tuple", tuple(words), tuple(words))
try:
    len(map(str, []))
except TypeError as e:
    print(type(e).__name__, e)
try:
    map(str, [])[0]
except TypeError as e:
    print(type(e).__name__, e)
try:
    reversed(x for x in [])
except TypeError as e:
    print(type(e).__name__, e)
try:
    map(5, [1]).__next__()
except TypeError as e:
    print(type(e).__name__, e)
try:
    enumerate(5)
except TypeError as e:
    print(type(e).__name__, e)
