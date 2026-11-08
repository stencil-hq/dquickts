#!/bin/sh
# Python thread tests (M5).  Each NNN_name.py is run like an L0 golden
# script; its stdout (+ "[exit N] <last stderr line>" on failure) must match
# NNN_name.out, written by `sh tests/py/threads/run.sh --regen` with
# python3.15.  1NN_* scripts use `threading` and are skipped when it cannot
# be imported (it needs the C-API itertools/_collections).
# Usage: sh tests/py/threads/run.sh [--regen]   (needs ./qpy)
cd "$(dirname "$0")/../../.." || exit 2
QPY=${QPY:-./qpy}
PY=${PY:-python3.15}
dir=tests/py/threads
fail=0
run() {
    out=$(cd "$dir" && PYTHONHASHSEED=0 PYTHONIOENCODING=utf-8 LC_ALL=C.UTF-8 \
          timeout 60 "$1" -I -X utf8 "$2" 2>/tmp/qpthreads.err)
    status=$?
    printf '%s\n' "$out"
    [ $status -ne 0 ] && printf '[exit %s] %s\n' "$status" "$(grep . /tmp/qpthreads.err | tail -1)"
}
if [ "$1" = "--regen" ]; then
    for f in "$dir"/[0-9]*.py; do
        n=$(basename "$f"); run "$PY" "$n" > "${f%.py}.out"
    done
    exit 0
fi
case $QPY in /*) ;; *) QPY=$PWD/$QPY ;; esac
have_threading=1
"$QPY" -I -c "import threading, queue" >/dev/null 2>&1 || have_threading=0
for f in "$dir"/[0-9]*.py; do
    n=$(basename "$f" .py)
    case $n in 1*) if [ $have_threading = 0 ]; then echo "SKIP $n (threading needs the C API)"; continue; fi ;; esac
    if run "$QPY" "$n.py" | cmp -s - "$dir/$n.out"; then
        echo "PASS $n"
    else
        echo "FAIL $n"; run "$QPY" "$n.py" | diff "$dir/$n.out" - | head -20; fail=1
    fi
done
exit $fail
