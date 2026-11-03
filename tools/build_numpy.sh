#!/bin/sh
# Build numpy from source against quickpy's own C API headers (plan D5
# Tier S, M7), install it into a qpy venv and optionally run a numpy test
# subset with pytest under qpy.
#
#   tools/build_numpy.sh [QPY [WORKDIR]]          (network access required)
#   NUMPY_TESTS=1 tools/build_numpy.sh ...        also run the test subset
#   JOBS=4 tools/build_numpy.sh ...               limit the ninja jobs
#
# Who runs what:
#  - pip and meson-python run under qpy (WORKDIR/qvenv): meson-python asks
#    qpy's sysconfig for the wheel tag (quickpy315-quickpy_315_<multiarch>)
#    and writes a native file whose `python` is qpy, so meson's python
#    module introspects qpy: INCLUDEPY (capi/include), EXT_SUFFIX
#    (.quickpy-315-<multiarch>.so), LIBPYTHON '' (extensions link with
#    `-undefined dynamic_lookup` against the symbols qpy exports);
#  - meson itself (numpy's vendored fork, MESON=WORKDIR/bin/meson), its
#    code generators and Cython run under the host python3.15 (HOSTPY):
#    they only configure and generate C; Cython's output is compiled against
#    capi/include, whose pyconfig.h selects Cython's limited-API code paths;
#  - ninja and the C/C++ compilers are the host's.
# numpy itself is not patched.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
QPY=$(cd "$(dirname "${1:-$HERE/../qpy}")" && pwd)/$(basename "${1:-qpy}")
WORK=${2:-${TMPDIR:-/tmp}/qpy-numpy}
HOSTPY=${HOSTPY:-python3.15}
NUMPY_VERSION=${NUMPY_VERSION:-2.5.4}
CYTHON_VERSION=${CYTHON_VERSION:-3.3.1}
MESONPY_VERSION=${MESONPY_VERSION:-0.22.1}
PYTEST_VERSION=${PYTEST_VERSION:-9.2.0}
HYPOTHESIS_VERSION=${HYPOTHESIS_VERSION:-6.170.14}

mkdir -p "$WORK/bin"
WORK=$(cd "$WORK" && pwd)
SRC=$WORK/numpy-$NUMPY_VERSION

# build tools: qpy venv (pip, meson-python), host venv (Cython, ninja)
[ -x "$WORK/qvenv/bin/python" ] || "$QPY" -m venv "$WORK/qvenv"
[ -x "$WORK/hvenv/bin/python" ] || "$HOSTPY" -m venv "$WORK/hvenv"
"$WORK/qvenv/bin/python" -m pip install -q "meson-python==$MESONPY_VERSION" pyproject-metadata packaging
"$WORK/hvenv/bin/python" -m pip install -q "cython==$CYTHON_VERSION" ninja
ln -sf "$WORK/hvenv/bin/cython" "$WORK/bin/cython"
ln -sf "$WORK/hvenv/bin/ninja" "$WORK/bin/ninja"

# numpy sdist
if [ ! -d "$SRC" ]; then
    "$HOSTPY" - "$NUMPY_VERSION" "$WORK" <<'EOF'
import json, sys, tarfile, urllib.request
version, work = sys.argv[1], sys.argv[2]
with urllib.request.urlopen("https://pypi.org/pypi/numpy/%s/json" % version) as r:
    url = next(u["url"] for u in json.load(r)["urls"] if u["packagetype"] == "sdist")
path = "%s/numpy-%s.tar.gz" % (work, version)
urllib.request.urlretrieve(url, path)
with tarfile.open(path) as t:
    t.extractall(work, filter="data")
EOF
fi

# meson under the host interpreter (meson-python runs $MESON directly)
cat > "$WORK/bin/meson" <<EOF
#!/bin/sh
exec "$HOSTPY" "$SRC/vendored-meson/meson/meson.py" "\$@"
EOF
chmod +x "$WORK/bin/meson"

# configure, build and install the wheel (quickpy315-quickpy_315_*.whl)
PATH=$WORK/bin:$PATH MESON=$WORK/bin/meson \
    "$WORK/qvenv/bin/python" -m pip install --no-build-isolation --force-reinstall --no-deps \
    "$SRC" -Cbuild-dir="$WORK/build" ${JOBS:+-Ccompile-args=-j$JOBS}

[ "${NUMPY_TESTS:-0}" = 1 ] || exit 0

# test subset: pytest + hypothesis (pure wheels) under qpy, from outside the
# source tree so that the installed numpy is imported
"$WORK/qvenv/bin/python" -m pip install -q "pytest==$PYTEST_VERSION" "hypothesis==$HYPOTHESIS_VERSION"
SP=$("$WORK/qvenv/bin/python" -c 'import numpy, os; print(os.path.dirname(numpy.__file__))')
cd "$WORK"
status=0
for t in _core/tests/test_multiarray.py _core/tests/test_umath.py _core/tests/test_numeric.py \
         linalg/tests/test_linalg.py random/tests/test_random.py; do
    name=$(basename "$t" .py)
    "$WORK/qvenv/bin/python" -m pytest -q -p no:cacheprovider "$SP/$t" \
        --junitxml="$WORK/$name.xml" > "$WORK/$name.log" 2>&1 || status=1
    printf '%s: %s\n' "$name" "$(tail -1 "$WORK/$name.log")"
done
exit $status
