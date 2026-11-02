#!/usr/bin/env python3
"""Compare host CPython against ./qpy on the micro benchmarks in this directory.

Each bench_*.py prints `time <seconds>` (hot section only) and
`check <result>`. Every benchmark runs in fresh processes under the
reference interpreter and the implementation; the min of -n repetitions is
reported. A check mismatch, crash or timeout makes the runner exit non-zero.
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(BENCH_DIR, "..", "..", ".."))

# A qpy run may take at most this multiple of the reference run's wall time
# (plus the startup difference) before it is killed and reported as timeout.
TIMEOUT_FACTOR = 10.0
MIN_TIMEOUT = 2.0
# Stop repeating a benchmark under one interpreter once its runs have used
# this much wall time (at least one run always happens).
REPEAT_BUDGET = 2.0


class RunError(Exception):
    def __init__(self, status, detail):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def resolve_impl(path):
    if os.sep in path or path.startswith("."):
        return os.path.normpath(path if os.path.isabs(path) else os.path.join(REPO_ROOT, path))
    return shutil.which(path) or path


def run_once(interp, script, timeout):
    """Run one benchmark process; return (hot_seconds, check, wall_seconds)."""
    cmd = [interp, "-I", script]
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, cwd=BENCH_DIR)
    except subprocess.TimeoutExpired:
        raise RunError("timeout", f"killed after {timeout:.1f}s")
    except OSError as e:
        raise RunError("error", str(e))
    wall = time.perf_counter() - t0
    hot = check = None
    for line in proc.stdout.splitlines():
        if line.startswith("time "):
            hot = float(line[5:])
        elif line.startswith("check "):
            check = line[6:]
    if proc.returncode != 0 or hot is None or check is None:
        tail = (proc.stderr.strip().splitlines() or proc.stdout.strip().splitlines() or [""])[-1]
        raise RunError("error", f"exit {proc.returncode}: {tail}")
    return hot, check, wall


def measure(interp, script, reps, timeout):
    """Return (min_hot, check, min_wall); checks must agree across reps."""
    best_hot = best_wall = None
    check = None
    spent = 0.0
    for i in range(reps):
        hot, chk, wall = run_once(interp, script, timeout)
        if check is not None and chk != check:
            raise RunError("unstable", f"check changed between runs: {check!r} vs {chk!r}")
        check = chk
        best_hot = hot if best_hot is None else min(best_hot, hot)
        best_wall = wall if best_wall is None else min(best_wall, wall)
        spent += wall
        if spent > REPEAT_BUDGET:
            break
    return best_hot, check, best_wall


def startup(interp, reps):
    best = None
    for _ in range(reps):
        t0 = time.perf_counter()
        proc = subprocess.run([interp, "-I", "-c", "pass"], capture_output=True, timeout=30)
        dt = time.perf_counter() - t0
        if proc.returncode != 0:
            return None
        best = dt if best is None else min(best, dt)
    return best


def fmt_ms(ms):
    return "-" if ms is None else f"{ms:.1f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--impl", default="./qpy", help="implementation under test (default ./qpy, repo-relative)")
    ap.add_argument("--ref", default="python3.15", help="reference interpreter (default python3.15)")
    ap.add_argument("-n", type=int, default=3, help="repetitions per benchmark; min is reported (default 3)")
    ap.add_argument("--filter", default="", help="only run benchmarks whose name contains this substring")
    ap.add_argument("--json", metavar="PATH", help="also write results as JSON")
    args = ap.parse_args()

    impl = resolve_impl(args.impl)
    ref = resolve_impl(args.ref)
    reps = max(1, args.n)
    names = sorted(f[6:-3] for f in os.listdir(BENCH_DIR)
                   if f.startswith("bench_") and f.endswith(".py") and args.filter in f[6:-3])
    if not names:
        print(f"no benchmarks match --filter {args.filter!r}", file=sys.stderr)
        return 2

    suite_t0 = time.perf_counter()
    ref_start = startup(ref, reps)
    impl_start = startup(impl, reps)
    if ref_start is None:
        print(f"reference interpreter {ref!r} failed to start", file=sys.stderr)
        return 2
    if impl_start is None:
        print(f"implementation {impl!r} failed to start", file=sys.stderr)
        return 2

    rows = []
    failed = False
    for name in names:
        script = os.path.join(BENCH_DIR, f"bench_{name}.py")
        row = {"name": name, "ref_ms": None, "impl_ms": None, "ratio": None,
               "status": "ok", "ref_check": None, "impl_check": None, "detail": ""}
        try:
            ref_hot, ref_check, ref_wall = measure(ref, script, reps, timeout=60)
        except RunError as e:
            row.update(status=f"ref-{e.status}", detail=e.detail)
            rows.append(row)
            failed = True
            print(f"  {name}: reference {e.status}: {e.detail}", file=sys.stderr)
            continue
        row.update(ref_ms=ref_hot * 1000, ref_check=ref_check)
        timeout = max(MIN_TIMEOUT, TIMEOUT_FACTOR * ref_wall + max(0.0, impl_start - ref_start))
        try:
            impl_hot, impl_check, _ = measure(impl, script, reps, timeout)
        except RunError as e:
            row.update(status=e.status, detail=e.detail)
            failed = True
            print(f"  {name}: impl {e.status}: {e.detail}", file=sys.stderr)
        else:
            row.update(impl_ms=impl_hot * 1000, impl_check=impl_check,
                       ratio=impl_hot / ref_hot if ref_hot > 0 else None)
            if impl_check != ref_check:
                row.update(status="MISMATCH", detail=f"ref={ref_check!r} impl={impl_check!r}")
                failed = True
                print(f"  {name}: CHECK MISMATCH ref={ref_check!r} impl={impl_check!r}", file=sys.stderr)
        rows.append(row)

    ratios = [r["ratio"] for r in rows if r["status"] == "ok" and r["ratio"]]
    geomean = math.exp(sum(math.log(x) for x in ratios) / len(ratios)) if ratios else None
    elapsed = time.perf_counter() - suite_t0

    print()
    print("| benchmark | CPython ms | qpy ms | qpy/CPython | status |")
    print("|---|---:|---:|---:|---|")
    for r in rows:
        ratio = "-" if r["ratio"] is None else f"{r['ratio']:.2f}x"
        status = r["status"] if r["status"] == "ok" else f"**{r['status']}**"
        print(f"| {r['name']} | {fmt_ms(r['ref_ms'])} | {fmt_ms(r['impl_ms'])} | {ratio} | {status} |")
    gm = "-" if geomean is None else f"{geomean:.2f}x"
    print(f"| **geomean** ({len(ratios)} ok) | | | {gm} | |")
    print(f"| startup (`-I -c pass`) | {fmt_ms(ref_start * 1000)} | {fmt_ms(impl_start * 1000)} | "
          f"{impl_start / ref_start:.2f}x | |")
    print()
    print(f"ref: {ref}  impl: {impl}  reps: {reps} (min reported)  suite wall: {elapsed:.1f}s")
    for r in rows:
        if r["status"] != "ok":
            print(f"FAIL {r['name']}: {r['status']} {r['detail']}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"ref": ref, "impl": impl, "reps": reps,
                       "startup_ms": {"ref": ref_start * 1000, "impl": impl_start * 1000},
                       "geomean_ratio": geomean, "benchmarks": rows,
                       "suite_seconds": elapsed}, f, indent=2)
            f.write("\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
