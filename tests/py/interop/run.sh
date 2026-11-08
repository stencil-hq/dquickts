#!/bin/sh
# JS <-> Python interop tests (plan D3, milestone M4).
# Usage: sh tests/py/interop/run.sh   (from anywhere; needs ./qjs and ./qpy)
# Exits non-zero when any test fails.
cd "$(dirname "$0")/../../.." || exit 2
QJS=${QJS:-./qjs}
QPY=${QPY:-./qpy}
dir=tests/py/interop
fail=0

run() {
    name=$1
    shift
    out=$("$@" 2>&1)
    status=$?
    if [ $status -eq 0 ] && printf '%s\n' "$out" | grep -q "^PASS $name"; then
        printf '%s\n' "$out" | grep "^PASS $name"
    else
        printf 'FAIL %s (exit %s)\n%s\n' "$name" "$status" "$out"
        fail=1
    fi
}

run test_interop.js "$QJS" -m "$dir/test_interop.js"
run test_interop.py "$QPY" "$dir/test_interop.py"
run test_timers.py env QJS="$QJS" "$QPY" "$dir/test_timers.py"
exit $fail
