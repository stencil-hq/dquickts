# Generator expressions (laziness, scoping, single use) and generator-based pipelines.

log = []


def source(n):
    for i in range(n):
        log.append("produce " + str(i))
        yield i


ge = (x * 10 for x in source(3))
print("genexp type", type(ge).__name__, "log empty", log)
print("first", next(ge), log)
print("rest", list(ge))
print("second pass empty", list(ge))

log.clear()
first_iter_evaluated = (x for x in source(2))
print("outermost iterable evaluated eagerly", log == [])
try:
    bad = (x for x in 5)
except TypeError as e:
    print("outermost evaluated at creation", type(e).__name__, e)
lazy_inner = (y for x in [1] for y in 5)
try:
    next(lazy_inner)
except TypeError as e:
    print("inner evaluated lazily", type(e).__name__, e)

base = [1, 2, 3]
ge = (x for x in base)
base = [100]
print("outermost bound at creation", list(ge))
k = 2
ge = (x * k for x in range(3))
k = 10
print("free vars resolved late", list(ge))

print("sum genexp", sum(x * x for x in range(10)))
print("any short-circuits", any(x > 2 for x in source(100)), len(log))
log.clear()
print("all short-circuits", all(x < 1 for x in source(100)), len(log))
print("max with genexp", max((len(w), w) for w in "a bbb cc".split()))
print("dict from genexp", dict((str(i), i) for i in range(3)))
print("sorted genexp", sorted((x % 3 for x in range(7)), reverse=True))
print("nested genexp", [list(row) for row in ((i * j for j in range(3)) for i in range(3))])
print("conditional genexp", list(x for x in range(10) if x % 3 == 0 if x))


def read_lines():
    for line in ["  alpha ", "", "# comment", "beta", "  gamma", "", "delta  "]:
        yield line


def strip(lines):
    for line in lines:
        yield line.strip()


def non_empty(lines):
    for line in lines:
        if line and not line.startswith("#"):
            yield line


def numbered(lines, start=1):
    n = start
    for line in lines:
        yield str(n) + ": " + line
        n += 1


print("pipeline", list(numbered(non_empty(strip(read_lines())))))


def take(n, it):
    for _, value in zip(range(n), it):
        yield value


def naturals():
    n = 0
    while True:
        yield n
        n += 1


def squares(it):
    return (x * x for x in it)


def evens(it):
    return (x for x in it if x % 2 == 0)


print("infinite pipeline", list(take(5, evens(squares(naturals())))))


def batched(it, size):
    batch = []
    for item in it:
        batch.append(item)
        if len(batch) == size:
            yield tuple(batch)
            batch = []
    if batch:
        yield tuple(batch)


print("batched", list(batched(range(7), 3)))


def running_avg():
    total = count = 0
    avg = None
    while True:
        value = yield avg
        total += value
        count += 1
        avg = total / count


ra = running_avg()
next(ra)
print("coroutine-style generator", [ra.send(v) for v in (10, 20, 60)])


def interleave(*gens):
    gens = [iter(g) for g in gens]
    while gens:
        for g in list(gens):
            try:
                yield next(g)
            except StopIteration:
                gens.remove(g)


print("interleave", list(interleave("abc", [1, 2], "XYZW")))
print("genexp as sole argument", list(x for x in "hi"))
print("tuple from genexp", tuple(c.upper() for c in "abc"))
print("join genexp", "-".join(str(i) for i in range(4)))
