#!/bin/sh
# Regenerate python/ast.h, python/ast.c and python/ast_json.c from CPython's
# Parser/Python.asdl. Run from the repository root.
set -e
${HOST_PYTHON:-python3.15} python/asdl_qp.py cpython/Parser/Python.asdl \
    --header python/ast.h --source python/ast.c --json python/ast_json.c
