#!/bin/sh
# coverage.py end to end over sys.monitoring (COVERAGE_CORE=sysmon): the
# line and branch report of qpy must equal python3.15's for each sample
# (cov_*.py next to this script).
# Usage: tests/py/monitoring/run.sh [QPY] [PYTHON]
# Skips (exit 0) when $PYTHON's pip, coverage or the network is unavailable.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
QPY=${1:-$root/qpy}
CPY=${2:-python3.15}
work=$(mktemp -d "${TMPDIR:-/tmp}/qp-monitoring.XXXXXX")
trap 'rm -rf "$work"' EXIT

skip() { echo "SKIP: $*"; exit 0; }

command -v "$CPY" >/dev/null 2>&1 || skip "$CPY not found"
[ -x "$QPY" ] || skip "$QPY not built"
# qpy: the pure-Python wheel (no C tracer: the sysmon core does not need
# it), installed by $CPY's pip
"$CPY" -m pip install -q --no-compile --no-deps --only-binary=:all: --platform any \
    --implementation py --python-version 3.15 --target "$work/qplib" coverage >/dev/null 2>&1 ||
    skip "pip install coverage failed (network?)"
"$CPY" -m pip install -q --target "$work/cplib" coverage >/dev/null 2>&1 ||
    skip "pip install coverage for $CPY failed (network?)"

report() {   # PY LIB PROG OUT
    (cd "$work" && rm -f .coverage &&
     COVERAGE_CORE=sysmon PYTHONPATH="$2" "$1" -m coverage run --branch "$3" >/dev/null &&
     PYTHONPATH="$2" "$1" -m coverage report -m --include="$3") > "$4" 2>&1
}
fail=0
for sample in "$here"/cov_*.py; do
    prog=$(basename "$sample")
    cp "$sample" "$work/$prog"
    report "$CPY" "$work/cplib" "$prog" "$work/cpython.txt" ||
        { cat "$work/cpython.txt"; skip "coverage under $CPY failed ($prog)"; }
    if ! report "$QPY" "$work/qplib" "$prog" "$work/qpy.txt"; then
        cat "$work/qpy.txt"
        echo "FAIL: $prog: coverage under qpy"
        fail=1
    elif diff "$work/cpython.txt" "$work/qpy.txt"; then
        echo "PASS: $prog: coverage sysmon report matches $CPY"
        sed -n 3p "$work/qpy.txt"
    else
        echo "FAIL: $prog: coverage sysmon report differs from $CPY"
        fail=1
    fi
done
exit $fail
