# Spectral norm (nested loops, float division, zip/sum over lists).
import sys
import time


def eval_a(i, j):
    return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)


def eval_times_u(func, u):
    return [func((i, u)) for i in range(len(u))]


def part_a_times_u(i_u):
    i, u = i_u
    partial_sum = 0.0
    for j, u_j in enumerate(u):
        partial_sum += eval_a(i, j) * u_j
    return partial_sum


def part_at_times_u(i_u):
    i, u = i_u
    partial_sum = 0.0
    for j, u_j in enumerate(u):
        partial_sum += eval_a(j, i) * u_j
    return partial_sum


def eval_ata_times_u(u):
    return eval_times_u(part_at_times_u, eval_times_u(part_a_times_u, u))


def run(n):
    u = [1.0] * n
    for _ in range(10):
        v = eval_ata_times_u(u)
        u = eval_ata_times_u(v)
    vbv = vv = 0.0
    for ue, ve in zip(u, v):
        vbv += ue * ve
        vv += ve * ve
    return f"{(vbv / vv) ** 0.5:.9f}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 180
    run(n // 4)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
