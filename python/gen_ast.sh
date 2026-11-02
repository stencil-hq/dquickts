#!/bin/sh
# Regenerate python/ast.h, python/ast.c, python/ast_json.c and python/mod_ast.c from CPython's
# Parser/Python.asdl. Run from the repository root.
set -e
${HOST_PYTHON:-python3.15} python/asdl_qp.py cpython/Parser/Python.asdl \
    --header python/ast.h --source python/ast.c --json python/ast_json.c
${HOST_PYTHON:-python3.15} python/gen_ast_module.py cpython/Parser/Python.asdl python/mod_ast.c
