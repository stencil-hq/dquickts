#!/bin/sh
# POSIX tests of the posix/select/signal modules (wave 5).  Each test_*.py
# is run like an L0 golden script; its stdout (+ "[exit N] <last stderr
# line>" on failure) must match test_*.out, written by
# `sh tests/py/posix/run.sh --regen` with python3.15.  test_*_darwin.py
# only run on macOS.
# Usage: sh tests/py/posix/run.sh [--regen]   (needs ./qpy)
cd "$(dirname "$0")/../../.." || exit 2
QPY=${QPY:-./qpy}
PY=${PY:-python3.15}
dir=tests/py/posix
fail=0
run() {
    out=$(cd "$dir" && PYTHONHASHSEED=0 PYTHONIOENCODING=utf-8 LC_ALL=C.UTF-8 \
          timeout 120 "$1" -I -X utf8 "$2" 2>/tmp/qpposix.err)
    status=$?
    printf '%s\n' "$out"
    [ $status -ne 0 ] && printf '[exit %s] %s\n' "$status" "$(grep . /tmp/qpposix.err | tail -1)"
}
if [ "$1" = "--regen" ]; then
    for f in "$dir"/test_*.py; do
        n=$(basename "$f"); run "$PY" "$n" > "${f%.py}.out"
    done
    exit 0
fi
case $QPY in /*) ;; *) QPY=$PWD/$QPY ;; esac
for f in "$dir"/test_*.py; do
    n=$(basename "$f" .py)
    case $n in *_darwin) if [ "$(uname -s)" != Darwin ]; then echo "SKIP $n"; continue; fi ;; esac
    if run "$QPY" "$n.py" | cmp -s - "$dir/$n.out"; then
        echo "PASS $n"
    else
        echo "FAIL $n"; run "$QPY" "$n.py" | diff "$dir/$n.out" - | head -20; fail=1
    fi
done
exit $fail
