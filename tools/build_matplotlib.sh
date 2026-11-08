#!/bin/sh
# Build matplotlib and its compiled dependencies from source against
# quickpy's own C API headers (plan D5 Tier S) and install them into a qpy
# venv.  tests/py/mpl/run.sh then runs the demo and the test subset.
#
#   tools/build_matplotlib.sh [QPY [WORKDIR]]     (network access required)
#   JOBS=4 tools/build_matplotlib.sh ...          limit the parallel compile jobs
#
# Steps (WORKDIR defaults to /tmp/qpy-mpl):
#  1. numpy: tools/build_numpy.sh QPY WORKDIR, which makes the venv
#     WORKDIR/qvenv (`qpy -m venv`) used by everything below;
#  2. Pillow: tools/build_pillow.sh's wheel (WORKDIR/pillow), installed in
#     the venv;
#  3. the pure runtime dependencies and the build tools, all running under
#     qpy: meson, meson-python, ninja (the PyPI binary), pybind11, cppy,
#     setuptools-scm;
#  4. kiwisolver (setuptools + cppy), contourpy (meson-python + pybind11)
#     and matplotlib (meson-python + pybind11) from their sdists with
#     --no-build-isolation: meson, its subproject downloads/extraction
#     (FreeType, HarfBuzz, libraqm, SheenBidi, Qhull, bundled as in the
#     PyPI wheels) and setuptools all run under qpy and introspect qpy, so
#     the extensions are named *.quickpy-315-<multiarch>.so and link with
#     `-undefined dynamic_lookup` against the symbols qpy exports.
#     kiwisolver's pyproject asks for setuptools<=72.1.0 on implementations
#     other than CPython (a PyPy constraint); the venv's current setuptools
#     builds it;
#  5. matplotlib's test data (baseline_images, data), which the wheel does
#     not install (meson install tags data,python-runtime,runtime), is
#     copied from the sdist into the installed matplotlib/tests.
# None of the packages is patched.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
QPY=$(cd "$(dirname "${1:-$HERE/../qpy}")" && pwd)/$(basename "${1:-qpy}")
WORK=${2:-${TMPDIR:-/tmp}/qpy-mpl}
MPL_VERSION=${MPL_VERSION:-3.11.2}
KIWI_VERSION=${KIWI_VERSION:-1.5.1}
CONTOURPY_VERSION=${CONTOURPY_VERSION:-1.4.0}
PILLOW_VERSION=${PILLOW_VERSION:-13.0.0}

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
PY=$WORK/qvenv/bin/python

# 1. numpy (+ the venv, meson-python)
sh "$HERE/build_numpy.sh" "$QPY" "$WORK"

# 2. Pillow (pybind11's ParallelCompile reads MAX_CONCURRENCY)
[ -z "${JOBS:-}" ] || export MAX_CONCURRENCY="$JOBS"
sh "$HERE/build_pillow.sh" -v "$PILLOW_VERSION" -w "$WORK/pillow" "$QPY"
"$PY" -m pip install -q --force-reinstall --no-deps "$WORK"/pillow/wheels/pillow-"$PILLOW_VERSION"-*.whl

# 3. pure dependencies and build tools
"$PY" -m pip install -q cycler==0.12.1 fonttools==4.68.0 packaging==26.3 pyparsing==3.3.3 \
    python-dateutil==4.0.0 meson==1.12.1 ninja==1.13.2 pybind11==3.1.0 cppy==1.3.1 \
    "setuptools_scm>=7,<10" setuptools pytest==9.2.0

# 4. kiwisolver, contourpy, matplotlib; meson-python finds meson and ninja
# on PATH
PATH=$WORK/qvenv/bin:$PATH
export PATH
"$PY" -m pip install --no-cache-dir --no-build-isolation --no-deps --force-reinstall \
    --no-binary kiwisolver "kiwisolver==$KIWI_VERSION"
"$PY" -m pip install --no-cache-dir --no-build-isolation --no-deps --force-reinstall \
    --no-binary contourpy \
    "contourpy==$CONTOURPY_VERSION" ${JOBS:+-Ccompile-args=-j$JOBS}
SRC=$WORK/matplotlib-$MPL_VERSION
if [ ! -d "$SRC" ]; then
    "$PY" - "$MPL_VERSION" "$WORK" <<'EOF'
import json, sys, tarfile, urllib.request
version, work = sys.argv[1], sys.argv[2]
with urllib.request.urlopen("https://pypi.org/pypi/matplotlib/%s/json" % version) as r:
    url = next(u["url"] for u in json.load(r)["urls"] if u["packagetype"] == "sdist")
path = "%s/matplotlib-%s.tar.gz" % (work, version)
urllib.request.urlretrieve(url, path)
with tarfile.open(path) as t:
    t.extractall(work, filter="data")
EOF
fi
"$PY" -m pip install --no-build-isolation --no-deps --force-reinstall "$SRC" \
    -Cbuild-dir="$WORK/build-mpl" ${JOBS:+-Ccompile-args=-j$JOBS}

# 5. test data
TESTS=$("$PY" -c 'import matplotlib, os; print(os.path.join(os.path.dirname(matplotlib.__file__), "tests"))')
for d in baseline_images data; do
    rm -rf "$TESTS/$d"
    cp -R "$SRC/lib/matplotlib/tests/$d" "$TESTS/$d"
done
"$PY" -c 'import matplotlib, matplotlib.ft2font as f, kiwisolver, contourpy, PIL; print("matplotlib", matplotlib.__version__, "FreeType", f.__freetype_version__, "kiwisolver", kiwisolver.__version__, "contourpy", contourpy.__version__, "Pillow", PIL.__version__)'
