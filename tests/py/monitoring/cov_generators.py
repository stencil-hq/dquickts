# coverage sample (run.sh): generators, yield from, comprehensions (nested, filtered, dict/set), generator expressions
def gen(n):
    i = 0
    while i < n:
        if i % 3 == 0:
            yield i
        elif i % 3 == 1:
            yield -i
        i += 1
    return i

def deleg(n):
    r = yield from gen(n)
    if r > 2:
        yield r

def comps(xs):
    a = [x * 2 for x in xs if x > 1]
    b = {x: y for x in xs for y in range(x) if y}
    c = {x for x in xs if x if x < 5}
    d = sum(x for x in xs if x % 2)
    e = [[y for y in range(x)] for x in xs]
    return a, b, c, d, e

def gx(xs):
    g = (x for x in xs)
    return next(g, None)

print(list(gen(5)), list(deleg(4)), list(deleg(1)))
print(comps([0, 1, 2, 3]), comps([]))
print(gx([]), gx([1]))
