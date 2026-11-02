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
   parse, then run the __future__ checks. src[len] must be '\0'.  `module`
   is the SyntaxWarning module (qp_parse_string).
   Returns 0 with *out set (valid until the arena is freed), or -1 with err set. */
int qp_parse(JSContext *ctx, qp_arena *arena, const char *src, size_t len,
             const char *filename, JSValueConst module, int mode, int flags, mod_ty *out,
             qp_error *err);
#endif

/* compile() without code generation: parse, __future__ checks, symbol table
   and compiler-detected SyntaxErrors (SyntaxWarnings are dropped).
   src[len] must be '\0'. All memory is
   released before returning. Returns 0 if the source compiles, -1 with err
   set (unless err is NULL) otherwise. */
int qp_check(JSContext *ctx, const char *src, size_t len, const char *filename,
             int mode, int flags, struct qp_error *err);

/* `qpy --dump-ast-json-batch`: serve the JSON-lines AST protocol (harness
   contract, L2 section, with the "check" and "top_level_await" request
   extensions) from `in` to `out` until EOF. Returns 0 at EOF, -1 on an
   I/O or fatal memory error. */
int qp_dump_ast_json_batch(JSContext *ctx, FILE *in, FILE *out);

/* CPython's `python` program (Py_BytesMain): parse the CPython command line
   (argv[0] is the program name used in messages), honour the PYTHON*
   environment variables unless -E/-I, create a runtime and a context with
   the Python state, run `-c cmd`, `-m mod`, a script, stdin or the
   interactive loop as __main__, flush sys.stdout/sys.stderr and return the
   process exit status (CPython rules: 0, 1 for an uncaught exception,
   SystemExit codes, 2 for usage errors and unopenable scripts, 120 when
   flushing the standard streams fails, 130 for an unhandled
   KeyboardInterrupt).  Used by qpy and by `qjs file.py` / `qjs --python`. */
int qp_main(int argc, char **argv);

/* The base snapshot of qp_main (python/pythonrun.c, plan section 1): the
   interpreter state after the part of the startup that does not depend on
   the process, written at build time by qp_write_base_snapshot() (`qpy
   --write-base-snapshot FILE DIR`: the base of the qpy of directory DIR,
   plus the make dependencies of FILE in FILE.d; returns the exit status)
   and embedded in qpy, which gives it to qp_set_base_snapshot() before
   qp_main().  qp_main() starts from it when it applies (same build,
   unchanged stdlib sources, a configuration that gives the same state),
   normally otherwise.  'data' must stay valid. */
int qp_write_base_snapshot(const char *path, const char *dir);
void qp_set_base_snapshot(const uint8_t *data, size_t size);

/* `qjs --drepl FILE --python` (python/pythonrun.c): qjs keeps the line
   editor and runs each input in the session context `ctx`, whose runtime
   it saves with JS_WriteSnapshot() as a delta of a new session.
   qp_drepl_init() makes the state of a new session (sys.ps1/ps2, __main__,
   the SIGINT handler); it only depends on the build, argv0 (its
   executable) and the hash seed, so that the base of the deltas can be
   rebuilt in another process (posix.environ is empty, the working
   directory is not in the state).  qp_drepl_attach(), called after the
   base of a new session is written and after a restore, checks that the
   context is a Python session (-1 if not), gives it the signals and fills
   posix.environ from the environment.  qp_drepl_environ(ctx, 0) empties
   posix.environ before each save, qp_drepl_environ(ctx, 1) fills it
   again after it: a session file never holds the environment.
   qp_drepl_signals() installs the session's SIGINT handler for the run of
   an input and returns the previous one.  qp_drepl_input_complete() is
   codeop's decision (1 run it, 0 read more lines).  qp_drepl_exec() runs
   one input (top-level await allowed) and returns 0, 1 if an exception was
   reported, or 2 with *pstatus for SystemExit.  qp_drepl_prompt() is
   str(sys.ps1) or str(sys.ps2) and qp_drepl_names() the completions of a
   word (see pythonrun.c); both return strings freed with JS_FreeCString(),
   or NULL. */
int qp_drepl_init(JSContext *ctx, const char *argv0, uint32_t hash_seed);
int qp_drepl_attach(JSContext *ctx);
void qp_drepl_environ(JSContext *ctx, int fill);
void (*qp_drepl_signals(JSContext *ctx))(int);
void qp_drepl_set_stack(JSRuntime *rt);
void qp_drepl_banner(char *buf, size_t size);
const char *qp_drepl_prompt(JSContext *ctx, int more);
int qp_drepl_input_complete(JSContext *ctx, const char *src, size_t len);
int qp_drepl_exec(JSContext *ctx, const char *src, size_t len, int *pstatus);
const char *qp_drepl_names(JSContext *ctx, const char *expr, const char *prefix);

#endif /* QUICKPY_H */
