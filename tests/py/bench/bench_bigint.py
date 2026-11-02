# Arbitrary-precision integers: factorials, modular pow, pi-digit spigot
# (big multiply / floor-divide), bit ops on big values.
import sys
import time


def pidigits(ndigits):
    # Gibbons' unbounded spigot.
    q, r, t, k, n, l = 1, 0, 1, 1, 3, 3
    digits = []
    while len(digits) < ndigits:
        if 4 * q + r - t < n * t:
            digits.append(n)
            q, r, t, k, n, l = 10 * q, 10 * (r - n * t), t, k, (10 * (3 * q + r)) // t - 10 * n, l
        else:
            q, r, t, k, n, l = q * k, (2 * q + r) * l, t * l, k + 1, (q * (7 * k + 2) + r * l) // (t * l), l + 2
    return digits


def factorial(n):
    r = 1
    for i in range(2, n + 1):
        r *= i
    return r


def run(n):
    digits = pidigits(n)
    dsum = sum(digits)
    f = factorial(n * 2)
    m = (1 << 127) - 1
    fm = f % m
    p = pow(3, f % 100003 + 10000, m)
    big = (f >> 7) ^ (f << 3)
    bits = big.bit_length() + bin(fm).count("1")
    q, rem = divmod(f, factorial(n))
    return f"{''.join(map(str, digits[-20:]))} {dsum} {fm % 1000000007} {p % 1000000007} {bits} {rem} {q % 99991}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1600
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
