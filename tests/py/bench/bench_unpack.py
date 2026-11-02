# Sequence unpacking and tuple ops: multiple assignment, swaps, star-unpacking,
# tuple building/concatenation/indexing, enumerate/zip unpacking.
import sys
import time


def run(n):
    acc = 0
    t = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    lst = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    for i in range(n):
        a, b, c, d, e, f, g, h, j, k = t
        a, b, c, d, e, f, g, h, j, k = lst
        a, b = b, a
        a, b, c = c, a, b
        first, *mid, last = t
        head, *rest = lst
        x = (a, b, c) + (i, i + 1)
        p, q, r, s, u = x
        (m, n2), (o, w) = (p, q), (r, s)
        acc += a + b + c + d + e + f + g + h + j + k
        acc += first + last + len(mid) + head + len(rest)
        acc += p + q + r + s + u + m + n2 + o + w
        acc += x[i % 5] + t[-1] + len(t[2:7])
    for idx, (v1, v2) in enumerate(zip(t, lst)):
        acc += idx * v1 - v2
    pts = [(i, i * 2, i * 3) for i in range(n // 10)]
    for px, py, pz in pts:
        acc += px - py + pz
    dd = {(i, i + 1): i for i in range(1000)}
    for (k1, k2), v in dd.items():
        acc += k1 + k2 - v
    return acc


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
