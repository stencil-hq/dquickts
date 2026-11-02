# Sorting with key functions, list/set comprehensions, sorted() variants,
# min/max with key, list methods.
import sys
import time


class Rec:
    def __init__(self, ident, score, name):
        self.ident = ident
        self.score = score
        self.name = name


def lcg_list(n, seed):
    out = []
    for _ in range(n):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(seed >> 8)
    return out


def run(n):
    data = lcg_list(n, 42)
    a = sorted(data)
    b = sorted(data, reverse=True)
    c = sorted(data, key=lambda x: (x % 1000, -x))
    recs = [Rec(i, v % 997, "n%d" % (v % 113)) for i, v in enumerate(data)]
    recs.sort(key=lambda r: (r.score, r.name, r.ident))
    squares = [x * x for x in data if x & 1]
    uniq = {x % 5000 for x in data}
    pairs = [(x % 100, y % 100) for x, y in zip(data, data[1:])]
    pairs.sort()
    strs = sorted([str(x) for x in data[: n // 4]], key=len)
    best = max(recs, key=lambda r: r.score * 1000 + r.ident)
    worst = min(data, key=lambda x: x ^ 0x5555)
    chk = (a[0] + a[-1] + b[1] + c[0] + recs[0].ident + recs[-1].ident
           + len(squares) + len(uniq) + pairs[n // 2][0] + len(strs[0])
           + best.ident + worst)
    return chk


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 50000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
