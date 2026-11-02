# Python micro benchmarks: CPython vs qpy

`make bench-py` (or `python3.15 tests/py/bench/run.py`) runs every
`bench_*.py` here under host CPython (`python3.15 -I`) and `./qpy -I`, each in
a fresh process, and prints a markdown table of hot-loop times, qpy/CPython
ratios, their geometric mean and interpreter startup (`-I -c pass`).

Options: `--impl PATH` (default `./qpy`, repo-relative), `--ref EXE`
(default `python3.15`), `-n N` repetitions (default 3, min reported; a
benchmark stops repeating after ~2 s of wall time), `--filter SUBSTR`,
`--json PATH`.

The runner exits non-zero if any benchmark crashes, times out (a qpy run is
killed after 10x the CPython wall time, floor 2 s) or prints a different
`check` line than CPython. A mismatch is a qpy bug.

Benchmark contract: a standalone script that imports only `sys` and `time`
(no stdlib modules or C accelerators), sized to ~80-200 ms on CPython, runs a
small warm-up, times only the hot part with `time.perf_counter()`, and prints
exactly `time <seconds>` and `check <deterministic result>`. An optional
integer `sys.argv[1]` overrides the workload size for calibration.
