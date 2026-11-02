#!/bin/sh
# Build Pillow from source against the dquickts C API (Tier S: version-specific
# extension modules named *.quickpy-315-<multiarch>.so) and optionally run a
# Pillow test subset under qpy with pytest.
#
# usage: tools/build_pillow.sh [-v VERSION] [-w WORKDIR] [-t] [QPY]
#   QPY      qpy binary (default: ./qpy of this tree)
#   -v       Pillow version (default: latest sdist on PyPI)
#   -w       work directory: venv, sdist, wheel (default: /tmp/qpy-pillow)
#   -t       run the M7 test subset after installing
#
# External libraries come from pkg-config (Homebrew on macOS: jpeg-turbo,
# libpng, zlib, freetype, little-cms2, webp, libtiff, openjpeg...); Pillow's
# setup.py enables what it finds.
#
# Steps:
#  1. venv from qpy (`qpy -m venv`, bundled pip) with setuptools, pybind11
#     and pytest; the build runs with --no-build-isolation so they are
#     installed once.
#  2. setuptools' install_lib._gen_exclusion_paths concatenates
#     sys.implementation.cache_tag unconditionally (eagerly, through
#     itertools.product), but PEP 421 lets it be None and qpy's is None (no
#     bytecode caches): the venv's copy is patched to skip the cache paths
#     when cache_tag is None, as distutils' other users do.
#  3. `pip wheel` of the sdist.  On macOS --plat-name is passed: without it
#     bdist_wheel derives the macOS platform tag from the Mach-O headers of
#     the built extensions through wheel.macosx_libfile, which needs ctypes.
#  4. `pip install` of the wheel (tag quickpy315-quickpy_315_<multiarch>).
#
# M7 result (Pillow 13.0.0, macOS arm64): test_image 186 passed/2 skipped,
# test_image_resize 90, test_file_png 73/1, test_file_jpeg 107/1,
# test_imagedraw 263/1 xfailed (all skips are upstream conditions).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=
WORK=/tmp/qpy-pillow
RUN_TESTS=0
while getopts v:w:t opt; do
    case $opt in
    v) VERSION=$OPTARG ;;
    w) WORK=$OPTARG ;;
    t) RUN_TESTS=1 ;;
    *) sed -n '5,10p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
QPY=${1:-$ROOT/qpy}
case $QPY in /*) ;; *) QPY=$(pwd)/$QPY ;; esac

mkdir -p "$WORK"
VENV=$WORK/venv
PY=$VENV/bin/python

# 1. venv + build/test requirements
if [ ! -x "$PY" ]; then
    "$QPY" -m venv "$VENV"
fi
"$PY" -m pip install -q setuptools pybind11 pytest

# 2. setuptools vs. cache_tag None
"$PY" - <<'EOF'
import os, setuptools.command.install_lib as m
p = m.__file__
s = open(p).read()
old = "        if not hasattr(sys, 'implementation'):\n            return\n"
new = "        if getattr(sys.implementation, 'cache_tag', None) is None:\n            return\n"
if new not in s:
    if old not in s:
        raise SystemExit("build_pillow: setuptools install_lib layout changed: " + p)
    open(p, "w").write(s.replace(old, new, 1))
EOF

# 3. sdist + wheel
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL https://pypi.org/pypi/pillow/json |
              "$PY" -c 'import json, sys; print(json.load(sys.stdin)["info"]["version"])')
fi
SRC=$WORK/pillow-$VERSION
if [ ! -d "$SRC" ]; then
    URL=$(curl -fsSL "https://pypi.org/pypi/pillow/$VERSION/json" |
          "$PY" -c 'import json, sys; print([u["url"] for u in json.load(sys.stdin)["urls"] if u["packagetype"] == "sdist"][0])')
    curl -fsSL "$URL" | tar xzf - -C "$WORK"
fi

if command -v brew >/dev/null 2>&1; then
    for f in jpeg-turbo libpng zlib; do
        d=$(brew --prefix "$f" 2>/dev/null || true)
        [ -d "$d/lib/pkgconfig" ] && PKG_CONFIG_PATH=$d/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
    done
    export PKG_CONFIG_PATH
fi

# pybind11's ParallelCompile (Pillow's setup.py) compiles through
# multiprocessing.pool.ThreadPool, whose locks need _multiprocessing.
if ! "$PY" -c 'import _multiprocessing' 2>/dev/null; then
    export MAX_CONCURRENCY=1
fi

set --
if [ "$(uname -s)" = Darwin ]; then
    set -- "-C--build-option=--plat-name=macosx_11_0_$(uname -m)"
fi
rm -rf "$WORK/wheels"
"$PY" -m pip wheel --no-build-isolation --no-deps -w "$WORK/wheels" "$@" "$SRC"

# 4. install
"$PY" -m pip install --force-reinstall --no-deps "$WORK"/wheels/pillow-*.whl
"$PY" -c 'from PIL import features; features.pilinfo(supported_formats=False)'

if [ "$RUN_TESTS" = 1 ]; then
    # junitxml needs xml.etree and faulthandler is not available in qpy
    cd "$SRC"
    "$PY" -m pytest -p no:cacheprovider -p no:junitxml -p no:faulthandler -q \
        Tests/test_image.py Tests/test_image_resize.py Tests/test_file_png.py \
        Tests/test_file_jpeg.py Tests/test_imagedraw.py
fi
