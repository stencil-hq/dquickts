/*
 * Compile-time SyntaxError detection for the Python front end.
 *
 * Derived from CPython Python/codegen.c, Python/compile.c and
 * Python/ast_preprocess.c (PSF-2.0), v3.15.2.
 *
 * qp_validate() is the part of CPython's compile() that runs after the
 * symbol table: it walks the AST in code generation order and raises every
 * SyntaxError that CPython's code generator raises, with the same message
 * and location, without generating any code.  See validate.c for the list
 * of replicated CPython call sites.
 *
 * Contract notes (python/m1 contract):
 * - `flags` is the merged feature word compile.c uses: the module's
 *   __future__ features (_PyFutureFeatures.ff_features) OR'ed with the
 *   compile flags.  Only CO_FUTURE_ANNOTATIONS changes what is checked
 *   (annotations are not evaluated, hence not checked, under
 *   `from __future__ import annotations`).
 * - Locations follow CPython's _PyCompile_Error exactly: lineno/end_lineno
 *   are the AST node's, offset = col_offset + 1 and end_offset =
 *   end_col_offset + 1 where the columns are the AST's UTF-8 byte offsets
 *   (CPython does not convert them to characters on this path); errors at
 *   CPython's NO_LOCATION have lineno = end_lineno = -1 and offset =
 *   end_offset = 0.  No source text is attached (CPython only re-reads the
 *   line from a real file named by `filename`).
 * - The AST is not modified: the constant folding CPython performs before
 *   code generation (ast_preprocess.c) is evaluated on the fly where it
 *   changes the outcome (match pattern values/keys, `__debug__`).
 * - Assumes the default optimization level (0), as compile() with
 *   optimize=-1 in an interpreter started without -O.
 */
#ifndef QP_VALIDATE_H
#define QP_VALIDATE_H

/* Returns 0 if the module compiles, -1 with `err` set otherwise
   (QP_ERR_SYNTAX for CPython's SyntaxErrors; QP_ERR_MEMORY,
   QP_ERR_RECURSION, QP_ERR_VALUE for the non-SyntaxError failures CPython
   can hit on the same path).  `st` must be the symbol table built for
   `mod`. */
static int qp_validate(struct symtable *st, mod_ty mod, int flags, qp_error *err);

#endif /* QP_VALIDATE_H */
