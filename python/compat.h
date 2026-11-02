/*
 * Shared base types of the dquickts Python front end (lexer, parser, AST,
 * symbol table, validator).
 *
 * The whole Python front end is compiled as part of the quickjs.c translation
 * unit (python/quickpy.c is included at the end of quickjs.c), so every
 * function here is static.  The implementations live in python/ast.c.
 *
 * Only Py_ssize_t and the PY_SSIZE_T_* limits use CPython public-looking
 * names; everything else is qp_* or an internal-name alias (PyArena,
 * _PyArena_Malloc) used by code ported from CPython.
 */
#ifndef QP_COMPAT_H
#define QP_COMPAT_H

typedef intptr_t Py_ssize_t;           /* the one public-looking name allowed globally */
#define PY_SSIZE_T_MAX INTPTR_MAX
#define PY_SSIZE_T_MIN INTPTR_MIN

/* quickjs.c turns malloc/calloc/realloc/free into *_is_forbidden() calls.
   Memory tied to a JSContext/JSRuntime uses js_malloc()/js_malloc_rt().
   Context-free transient buffers (lexer buffers, dtoa bigints, error text)
   use these, which bypass the function-like macros by parenthesising the
   name. */
#define qp_sys_malloc(size)       (malloc)(size)
#define qp_sys_calloc(n, size)    (calloc)(n, size)
#define qp_sys_realloc(ptr, size) (realloc)(ptr, size)
#define qp_sys_free(ptr)          (free)(ptr)

/* Errors produced by lexer/parser/symtable/validator. One per compile. */
typedef enum {
    QP_ERR_NONE = 0,
    QP_ERR_SYNTAX,          /* SyntaxError */
    QP_ERR_INDENTATION,     /* IndentationError */
    QP_ERR_TAB,             /* TabError */
    QP_ERR_INCOMPLETE,      /* _IncompleteInputError (SyntaxError subclass; interactive mode) */
    QP_ERR_MEMORY,          /* MemoryError */
    QP_ERR_VALUE,           /* ValueError (e.g. "source code string cannot contain null bytes") */
    QP_ERR_UNICODE_DECODE,  /* UnicodeDecodeError / SyntaxError for bad encodings as CPython does */
    QP_ERR_RECURSION,       /* RecursionError ("maximum recursion depth exceeded during compilation") */
    QP_ERR_OVERFLOW,        /* OverflowError */
    QP_ERR_PENDING,         /* a Python exception is pending in the JSContext (raised while
                               emitting a compile-time SyntaxWarning) */
} qp_error_kind;

typedef struct qp_error {
    qp_error_kind kind;
    char *msg;              /* malloc'd UTF-8, NULL if none */
    int lineno, offset;     /* CPython SyntaxError semantics: offset is 1-based CHARACTER offset; 0 = unknown
                               unless end_lineno is set; negative = reported as is */
    int end_lineno, end_offset; /* 0 = unknown */
    char *text;             /* malloc'd UTF-8 source line, may be NULL */
    /* SyntaxError._metadata of parser errors (_PyPegen_set_syntax_error_metadata):
       (metadata_lineno, metadata_col_offset, source); source is a malloc'd
       UTF-8 copy or NULL for None. */
    int has_metadata;
    int metadata_lineno, metadata_col_offset;
    char *metadata_source;
} qp_error;

/* First error wins: qp_error_set does nothing if err->kind != QP_ERR_NONE.
   The message is formatted with vsnprintf. If the message cannot be
   allocated the error becomes QP_ERR_MEMORY with msg = NULL. */
static __maybe_unused void qp_error_set(qp_error *err, qp_error_kind kind, int lineno, int offset,
                         int end_lineno, int end_offset, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
/* Attach a copy of the source line to the error. Does nothing if a text is
   already present (it belongs to the first error), so call it right after the
   qp_error_set of the same error. */
static __maybe_unused void qp_error_set_text(qp_error *err, const char *utf8, size_t len);
static __maybe_unused void qp_error_clear(qp_error *err);       /* frees msg/text, kind = NONE */

/* Arena: all AST memory; also owns JSAtoms/JSValues registered with it. */
typedef struct qp_arena qp_arena;
static __maybe_unused qp_arena *qp_arena_new(JSContext *ctx, qp_error *err);
static __maybe_unused void qp_arena_free(qp_arena *a);          /* frees blocks, JS_FreeAtom/JS_FreeValue registered items */
static __maybe_unused void *qp_arena_malloc(qp_arena *a, size_t size); /* NOT zeroed, 8-byte aligned; NULL + QP_ERR_MEMORY on OOM */
static __maybe_unused int qp_arena_add_atom(qp_arena *a, JSAtom atom);    /* takes ownership; -1 + QP_ERR_MEMORY on OOM (atom freed) */
static __maybe_unused int qp_arena_add_value(qp_arena *a, JSValue v);     /* takes ownership; -1 + QP_ERR_MEMORY on OOM (value freed) */
static __maybe_unused JSContext *qp_arena_ctx(qp_arena *a);
static __maybe_unused qp_error *qp_arena_error(qp_arena *a);
/* ast.parse(feature_version=(3, minor)): the grammar version qp_parse_string
   accepts (CPython cf_feature_version; -1, the default: the current one) */
static __maybe_unused void qp_arena_set_feature_version(qp_arena *a, int minor);
static __maybe_unused int qp_arena_feature_version(qp_arena *a);
#define PyArena qp_arena               /* internal-name aliases used by ported code */
#define _PyArena_Malloc qp_arena_malloc

/* Constants (Constant.value, MatchSingleton.value) */
typedef enum {
    QP_CONST_NONE, QP_CONST_TRUE, QP_CONST_FALSE, QP_CONST_ELLIPSIS,
    QP_CONST_INT,     /* v: JS_TAG_INT, JS_TAG_SHORT_BIG_INT or JS_TAG_BIG_INT (arbitrary precision) */
    QP_CONST_FLOAT,   /* v: JS_TAG_FLOAT64 made with __JS_NewFloat64 (never normalised to int) */
    QP_CONST_COMPLEX, /* v: JS_TAG_FLOAT64 imaginary part (real part is always 0 in source) */
    QP_CONST_STR,     /* v: JSString (8/16-bit; may contain lone surrogates; never a rope) */
    QP_CONST_BYTES,   /* v: 8-bit JSString whose chars are the raw byte values 0..255 */
    QP_CONST_OBJECT,  /* v: any other Python value (tuple, frozenset, complex with a real part,
                         or an invalid type rejected by validation); only made by the _ast
                         object converter (mod_ast.c), never by the parser */
} qp_const_kind;
typedef struct qp_const { qp_const_kind kind; JSValue v; } qp_const;
/* Shared singletons, v = JS_UNDEFINED (tentative definitions; initialised in ast.c). */
static __maybe_unused qp_const qp_const_none, qp_const_true, qp_const_false, qp_const_ellipsis;
static __maybe_unused qp_const *qp_const_new(qp_arena *a, qp_const_kind kind, JSValue v); /* takes ownership of v (registered with arena) */

#endif /* QP_COMPAT_H */
