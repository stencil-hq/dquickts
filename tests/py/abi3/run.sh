#!/bin/sh
# Tier B (plan D5, M5) smoke tests: install pinned abi3 wheels from PyPI
# with qpy's own pip into a qpy venv and with host python3.15 into a host
# venv, run each smoke_<pkg>.py under both and compare the output.
#
#   tests/py/abi3/run.sh [QPY [WORKDIR]]   (network access required)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
QPY=$(cd "$(dirname "${1:-$HERE/../../../qpy}")" && pwd)/$(basename "${1:-qpy}")
WORK=${2:-${TMPDIR:-/tmp}/qpy-abi3}
HOSTPY=${HOSTPY:-python3.15}
PKGS="bcrypt==5.0.0 cramjam==2.1.0 nh3==0.3.7 psutil==7.2.2 rpds-py==0.7.1 safetensors==0.8.0
      charset-normalizer==3.5.2 uuid-utils==1.2.0 css-inline==0.23.1 jsonschema-rs==0.60.3
      html-to-markdown==3.19.0 hf-xet==1.8.0 watchfiles==1.3.0"

mkdir -p "$WORK"
[ -x "$WORK/qvenv/bin/python" ] || "$QPY" -m venv "$WORK/qvenv" || exit 2
[ -x "$WORK/hvenv/bin/python" ] || "$HOSTPY" -m venv "$WORK/hvenv" || exit 2
"$WORK/qvenv/bin/python" -m pip install -q --only-binary=:all: $PKGS || exit 2
"$WORK/hvenv/bin/python" -m pip install -q --only-binary=:all: $PKGS || exit 2

fail=0
for t in "$HERE"/smoke_*.py; do
    name=$(basename "$t")
    "$WORK/hvenv/bin/python" "$t" > "$WORK/$name.host" 2>&1
    "$WORK/qvenv/bin/python" "$t" > "$WORK/$name.qpy" 2>&1
    if cmp -s "$WORK/$name.host" "$WORK/$name.qpy"; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        diff "$WORK/$name.host" "$WORK/$name.qpy" | head -20
        fail=1
    fi
done
exit $fail
