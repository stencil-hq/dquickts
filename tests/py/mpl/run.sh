#!/bin/sh
# Run tests/py/mpl/demo.py under qpy and CPython 3.15, diff their stdout and
# compare the PNGs pixel by pixel; with -t, also run a matplotlib test
# subset with pytest under both and compare the pass/fail sets.
#
#   tests/py/mpl/run.sh [-t] [WORKDIR]
#
# WORKDIR is tools/build_matplotlib.sh's (default /tmp/qpy-mpl): the qpy
# venv is WORKDIR/qvenv.  The CPython side is a python3.15 venv
# (WORKDIR/hostvenv, made on first use) with the PyPI wheels of the
# versions installed in the qpy venv.  Outputs go to WORKDIR/out-{qpy,host}.
# Exit status 0 when stdout is identical and the PNGs are pixel-identical
# (and, with -t, the two runs fail the same tests).
set -eu
TESTS=0
if [ "${1:-}" = -t ]; then
    TESTS=1
    shift
fi
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(cd "${1:-${TMPDIR:-/tmp}/qpy-mpl}" && pwd)
HOSTPY=${HOSTPY:-python3.15}
QPY=$WORK/qvenv/bin/python
HPY=$WORK/hostvenv/bin/python

if [ ! -x "$HPY" ]; then
    "$HOSTPY" -m venv "$WORK/hostvenv"
    # pip freeze names the source-built packages by their local path
    "$QPY" -m pip list --format=freeze |
        grep -E -i '^(matplotlib|numpy|pillow|kiwisolver|contourpy|cycler|fonttools|packaging|pyparsing|python-dateutil|pytest)==' \
        > "$WORK/host-requirements.txt"
    "$HPY" -m pip install -q --only-binary=:all: -r "$WORK/host-requirements.txt"
fi

status=0
for impl in qpy host; do
    py=$QPY; [ $impl = host ] && py=$HPY
    rm -rf "$WORK/out-$impl"
    mkdir -p "$WORK/out-$impl" "$WORK/mplconfig-$impl"
    # separate font caches; stderr (font cache notice) kept apart
    MPLCONFIGDIR=$WORK/mplconfig-$impl "$py" "$HERE/demo.py" "$WORK/out-$impl" \
        > "$WORK/out-$impl/stdout.txt" 2> "$WORK/out-$impl/stderr.txt" || {
        echo "demo failed under $impl:"; cat "$WORK/out-$impl/stderr.txt"; status=1; }
done
if diff -u "$WORK/out-host/stdout.txt" "$WORK/out-qpy/stdout.txt"; then
    echo "stdout: identical ($(wc -l < "$WORK/out-qpy/stdout.txt") lines)"
else
    echo "stdout: DIFFERENT"; status=1
fi

"$HPY" - "$WORK/out-host" "$WORK/out-qpy" <<'EOF' || status=1
import sys
import numpy as np
from PIL import Image
host, qpy = sys.argv[1:3]
bad = False
for name in ("demo.png", "layout.png"):
    a = np.asarray(Image.open("%s/%s" % (host, name)).convert("RGBA")).astype(np.int16)
    b = np.asarray(Image.open("%s/%s" % (qpy, name)).convert("RGBA")).astype(np.int16)
    if a.shape != b.shape:
        print("%s: shape %s vs %s" % (name, a.shape, b.shape))
        bad = True
        continue
    d = np.abs(a - b)
    px = (d.max(axis=-1) > 0)
    print("%s: %s max abs diff %d, differing pixels %d/%d (%.6f%%), mean abs diff %.6f"
          % (name, "x".join(map(str, a.shape)), d.max(), px.sum(), px.size,
             100.0 * px.mean(), d.mean()))
    bad |= bool(px.any())
for name in ("demo.svg", "demo.pdf"):
    same = open("%s/%s" % (host, name), "rb").read() == open("%s/%s" % (qpy, name), "rb").read()
    # the zlib builds differ (the demo's stdout hashes the decoded content)
    print("%s: %s" % (name, "byte-identical" if same else "bytes differ (zlib streams)"))
sys.exit(1 if bad else 0)
EOF
echo "outputs: $WORK/out-qpy $WORK/out-host"
[ $TESTS = 1 ] || exit $status

# matplotlib's test subset; the image baselines come from the sdist
# (tools/build_matplotlib.sh copies them into the qpy venv)
version=$("$HPY" -c 'import matplotlib; print(matplotlib.__version__)')
tdir=$("$HPY" -c 'import matplotlib, os; print(os.path.join(os.path.dirname(matplotlib.__file__), "tests"))')
for d in baseline_images data; do
    [ -d "$tdir/$d" ] || cp -R "$WORK/matplotlib-$version/lib/matplotlib/tests/$d" "$tdir/$d"
done
for t in test_figure test_axes test_colors test_contour test_text test_mathtext test_agg \
         test_backend_svg; do
    for impl in qpy host; do
        py=$QPY; [ $impl = host ] && py=$HPY
        mkdir -p "$WORK/tests-$impl"
        (cd "$WORK/tests-$impl" && MPLCONFIGDIR=$WORK/mplconfig-$impl "$py" -m pytest \
            -p no:cacheprovider -q -rfE --pyargs "matplotlib.tests.$t" > "$t.log" 2>&1) || true
        grep -E '^(FAILED|ERROR) ' "$WORK/tests-$impl/$t.log" | sed 's/ - .*//' | sort \
            > "$WORK/tests-$impl/$t.failed" || true
    done
    printf '%s: qpy: %s | CPython: %s\n' "$t" "$(tail -1 "$WORK/tests-qpy/$t.log")" \
        "$(tail -1 "$WORK/tests-host/$t.log")"
    diff "$WORK/tests-host/$t.failed" "$WORK/tests-qpy/$t.failed" || status=1
done
exit $status
