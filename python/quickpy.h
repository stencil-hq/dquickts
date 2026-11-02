/*
 * QuickPy front-end entry points (Python 3.15 lexer/parser/symtable/validator).
 *
 * The implementation (python/quickpy.c) is compiled as part of the quickjs.c
 * translation unit when CONFIG_PYTHON is defined. This header is included both
 * from that unit (after python/compat.h and python/ast.h, with
 * QUICKPY_INTERNAL defined) and from standalone programs (qpy.c, fuzz
 * targets), which only see the declarations that do not need the internal
 * AST/arena types.
 *
 * Contract deviation (M1Cli): qp_check() accepts err == NULL (the error is
 * then discarded), so that programs outside the quickjs.c translation unit,
 * which cannot see struct qp_error, can call it.
 */
#ifndef QUICKPY_H
#define QUICKPY_H

#include <stddef.h>
#include <stdio.h>
#include "../quickjs.h"

/* compile() start symbols (values of CPython's Py_*_input) */
#define QP_INPUT_SINGLE    256
#define QP_INPUT_FILE      257
#define QP_INPUT_EVAL      258
#define QP_INPUT_FUNC_TYPE 345

struct qp_error;

#ifdef QUICKPY_INTERNAL
/* ast.parse() semantics (compile() with PyCF_ONLY_AST): reject NUL bytes,
   parse, then run the __future__ checks. src[len] must be '\0'.
   Returns 0 with *out set (valid until the arena is freed), or -1 with err set. */
int qp_parse(JSContext *ctx, qp_arena *arena, const char *src, size_t len,
             const char *filename, int mode, int flags, mod_ty *out, qp_error *err);
#endif

/* compile() without code generation: parse, __future__ checks, symbol table
   and compiler-detected SyntaxErrors. src[len] must be '\0'. All memory is
   released before returning. Returns 0 if the source compiles, -1 with err
   set (unless err is NULL) otherwise. */
int qp_check(JSContext *ctx, const char *src, size_t len, const char *filename,
             int mode, int flags, struct qp_error *err);

/* `qpy --dump-ast-json-batch`: serve the JSON-lines AST protocol (harness
   contract, L2 section, with the "check" and "top_level_await" request
   extensions) from `in` to `out` until EOF. Returns 0 at EOF, -1 on an
   I/O or fatal memory error. */
int qp_dump_ast_json_batch(JSContext *ctx, FILE *in, FILE *out);

#endif /* QUICKPY_H */
