#!/bin/sh
# Regenerate python/parser.c from CPython's grammar with CPython's own PEG
# parser generator (Tools/peg_generator), then apply mechanical
# post-processing for the dquickts unity build. Run from the repository root.
#
# Post-processing (and nothing else) applied to the pegen output:
#  1. A "Derived from CPython" header line is added after the @generated line.
#  2. `#include "pegen.h"` and `#include "pycore_ceval.h"` are removed:
#     parser.c is #included by python/quickpy.c right after pegen.c, which
#     already provides pegen.h and the unit-local CPython names
#     (pegen_compat.h).
#  3. In action lines (`_res = ...;`) that call AST constructors, the literal
#     NULL becomes 0: identifier/string fields are JSAtom integers here, and
#     0 is both a null pointer constant and JS_ATOM_NULL.
#  4. The soft keyword table becomes `static const char *soft_keywords[]`
#     (string literals; Parser.soft_keywords is `const char **`), so the
#     unit compiles warning-free with -Wwrite-strings.
#  5. Every macro parser.c defines is #undef'd at its end, followed by
#     #include "pegen_undef.h" (drops the unit-local CPython names).
set -e

PY=${HOST_PYTHON:-python3.15}
OUT=python/parser.c
TMP=python/parser.c.tmp

PYTHONPATH=cpython/Tools/peg_generator "$PY" -m pegen -q c \
    cpython/Grammar/python.gram cpython/Grammar/Tokens -o "$TMP"

"$PY" - "$TMP" "$OUT" <<'EOF'
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8") as f:
    lines = f.read().split("\n")

out = []
defined = []
for i, line in enumerate(lines):
    if i == 0 and line.startswith("// @generated"):
        out.append(line)
        out.append("// Derived from CPython Grammar/python.gram and Tools/peg_generator "
                   "(PSF-2.0); post-processed by python/gen_parser.sh.")
        continue
    if line in ('#include "pegen.h"', '#include "pycore_ceval.h"'):
        continue
    if line == "static char *soft_keywords[] = {":
        line = "static const char *soft_keywords[] = {"
    m = re.match(r"#\s*define\s+([A-Za-z_][A-Za-z_0-9]*)", line)
    if m and m.group(1) not in defined:
        defined.append(m.group(1))
    if re.match(r"\s*_res = ", line) and "_PyAST_" in line:
        line = re.sub(r"\bNULL\b", "0", line)
    out.append(line)

while out and out[-1] == "":
    out.pop()
out.append("")
out.append("/* end of the parser unit */")
out.extend("#undef " + name for name in defined)
out.append('#include "pegen_undef.h"')
out.append("")

with open(dst, "w", encoding="utf-8") as f:
    f.write("\n".join(out))
EOF
rm -f "$TMP"
