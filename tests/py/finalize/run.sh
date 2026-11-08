#!/bin/sh
# Interpreter shutdown (Py_FinalizeEx): the __del__ methods of the cyclic
# garbage and asyncio's "Task was destroyed but it is pending!" are
# printed as by CPython, and weakref callbacks queued while the modules
# are cleared survive the collections. The capi_* cases finalize objects
# held by C containers (C API objects): in cycles collected by gc.collect(),
# in the globals of __main__ and of the modules cleared at exit, and with
# weakref.finalize. Compares stdout, stderr and the exit status with
# the host python3.15.
#
#   sh tests/py/finalize/run.sh [QPY]
HERE=$(cd "$(dirname "$0")" && pwd)
QPY=${1:-$HERE/../../../qpy}
HOSTPY=${HOSTPY:-python3.15}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/qpy_fin.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
for t in "$HERE"/*.py; do
    name=$(basename "$t")
    (cd "$HERE" && "$HOSTPY" "$name" > "$tmp/host" 2>&1; echo "status $?" >> "$tmp/host")
    (cd "$HERE" && "$QPY" "$name" > "$tmp/qpy" 2>&1; echo "status $?" >> "$tmp/qpy")
    if cmp -s "$tmp/host" "$tmp/qpy"; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        diff "$tmp/host" "$tmp/qpy"
        fail=1
    fi
done
exit $fail
