# Fannkuch-redux (list slicing, slice assignment, integer loops).
import sys
import time


def fannkuch(n):
    count = list(range(1, n + 1))
    max_flips = 0
    m = n - 1
    r = n
    checksum = 0
    perm1 = list(range(n))
    perm = list(range(n))
    perm1_ins = perm1.insert
    perm1_pop = perm1.pop
    sign = 1
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        if perm1[0] != 0 and perm1[m] != m:
            perm = perm1[:]
            flips = 0
            k = perm[0]
            while k:
                perm[:k + 1] = perm[k::-1]
                flips += 1
                k = perm[0]
            if flips > max_flips:
                max_flips = flips
            checksum += flips if sign > 0 else -flips
        while r != n:
            perm1_ins(r, perm1_pop(0))
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        else:
            return checksum, max_flips
        sign = -sign


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    fannkuch(n - 2)
    t0 = time.perf_counter()
    for _ in range(5):
        checksum, flips = fannkuch(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {checksum} {flips}")


main()
