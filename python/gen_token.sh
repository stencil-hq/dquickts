#!/bin/sh
# Regenerate python/token.h, python/token.c and python/lexer_unicode.h from
# cpython/Grammar/Tokens and the host CPython 3.15 Unicode database.
# Run from the repository root.
set -e
${HOST_PYTHON:-python3.15} python/gen_token.py cpython/Grammar/Tokens \
    python/token.h python/token.c python/lexer_unicode.h
