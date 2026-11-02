/*
 * Compile-time SyntaxError detection for the Python front end.
 *
 * Derived from CPython Python/codegen.c, Python/compile.c,
 * Python/ast_preprocess.c, Objects/complexobject.c (mixed-mode add/sub,
 * complex_repr), Python/pystrtod.c (float repr), Objects/stringlib/repr.h
 * (str repr) and Objects/bytesobject.c (bytes repr) (PSF-2.0), v3.15.2.
 *
 * The validator walks the AST exactly in the order of CPython's code
 * generator (codegen_visit_stmt/codegen_visit_expr and helpers) and keeps
 * the compiler state the errors depend on: the stack of compiler units
 * (scope type + symbol table entry), each unit's frame-block stack
 * (u_fblock, CO_MAXBLOCKS = 21) including the re-visits of `finally`
 * bodies that unwinding (return/break/continue) performs, the deferred
 * module/class annotations (PEP 649 __annotate__ bodies, compiled after
 * the body), and the pattern-matching capture lists.  The first error
 * raised wins, as in CPython.
 *
 * Replicated CPython call sites (all SyntaxError unless noted):
 *   compile.c   _PyCompile_PushFBlock          "too many statically nested blocks"
 *   compile.c   _PyCompile_Error               location conversion (col+1, byte based)
 *   codegen.c   codegen_unwind_fblock_stack    "'break', 'continue' and 'return' cannot
 *                                              appear in an except* block"
 *   codegen.c   codegen_type_params (x3)       "non-default type parameter '%U' follows
 *                                              default type parameter"
 *   codegen.c   codegen_return                 "'return' outside function",
 *                                              "'return' with value in async generator"
 *   codegen.c   codegen_break                  "'break' outside loop"
 *   codegen.c   codegen_continue               "'continue' not properly in loop"
 *   codegen.c   codegen_try_except             "default 'except:' must be last"
 *   codegen.c   codegen_validate_lazy_import   "lazy imports only allowed in module scope" (*)
 *   codegen.c   codegen_from_import            "cannot lazy import *" (*)
 *   codegen.c   unpack_helper                  "too many expressions in star-unpacking
 *                                              assignment", "multiple starred expressions
 *                                              in assignment"
 *   codegen.c   codegen_validate_keywords      "keyword argument repeated: %U"
 *   codegen.c   codegen_visit_expr             "'yield' outside function", "'yield from'
 *                                              outside function", "'yield from' inside async
 *                                              function", "starred assignment target must be
 *                                              in a list or tuple", "can't use starred
 *                                              expression here"
 *   codegen.c   codegen_error_duplicate_store  "multiple assignments to name %R in pattern"
 *                                              (from codegen_pattern_helper_store_name and
 *                                              codegen_pattern_or)
 *   codegen.c   codegen_pattern_unpack_helper  "too many expressions in star-unpacking
 *                                              sequence pattern"; its "multiple starred
 *                                              expressions in sequence pattern" is
 *                                              unreachable (codegen_pattern_sequence rejects
 *                                              a second star first) but kept
 *   codegen.c   codegen_pattern_as             "name capture %R makes remaining patterns
 *                                              unreachable", "wildcard makes remaining
 *                                              patterns unreachable"
 *   codegen.c   validate_kwd_attrs             "attribute name repeated in class pattern: %U"
 *   codegen.c   codegen_pattern_mapping_key    "mapping pattern checks duplicate key (%R)",
 *                                              "mapping pattern keys may only match literals
 *                                              and attribute lookups"
 *   codegen.c   codegen_pattern_or             "alternative patterns bind different names"
 *   codegen.c   codegen_pattern_sequence       "multiple starred names in sequence pattern"
 *   codegen.c   codegen_pattern_value          "patterns may only match literals and
 *                                              attribute lookups"
 *   codegen.c   codegen_visit_expr             Py_EnterRecursiveCall -> RecursionError (also
 *                                              checked per statement/pattern to protect the
 *                                              C stack)
 *   longobject.c long_to_decimal_string        ValueError "Exceeds the limit (4300 digits) ..."
 *                                              when %R formats a huge int mapping key
 *   (*) symtable.c rejects lazy imports outside module scope (and `lazy from
 *       ... import *`) before code generation, so these are unreachable from
 *       source but kept for fidelity.
 *
 * Call sites documented as unreachable for parser-produced ASTs (the
 * parser never builds the offending shape):
 *   codegen.c   codegen_pattern_class   "kwd_attrs (%d) / kwd_patterns (%d) length
 *                                       mismatch in class pattern" (parser builds both
 *                                       sequences from the same keyword list) and
 *                                       "too many sub-patterns in class pattern %R"
 *                                       (needs > INT_MAX sub-patterns)
 *   codegen.c   codegen_pattern_mapping "keys (%d) / patterns (%d) length mismatch in
 *                                       mapping pattern", "too many sub-patterns in
 *                                       mapping pattern" (> INT_MAX)
 *   codegen.c   codegen_pattern_mapping_key "can't use NULL keys in MatchMapping (set
 *                                       'rest' parameter instead)" (keys are never NULL)
 *   codegen.c   codegen_pattern         "invalid match pattern node in AST (kind=%d)"
 *                                       (kept as a defensive default)
 *   compile.c   compiler_codegen        SystemError "module kind %d should not be
 *                                       possible" (FunctionType modules are rejected by
 *                                       compile() and by the symbol table first; kept,
 *                                       reported as QP_ERR_VALUE)
 *   codegen.c   _PyCompile_Warn sites   SyntaxWarnings ("is" with a literal, "assertion
 *                                       is always true", "... is not callable/
 *                                       subscriptable", "... indices must be integers")
 *                                       only become SyntaxErrors under a warnings filter
 *                                       of "error"; warnings are not modelled.
 *   flowgraph.c                         raises no SyntaxError.
 *   ast_preprocess.c                    raises no SyntaxError (only the PEP 765
 *                                       "'return'/'break'/'continue' in 'finally'"
 *                                       SyntaxWarning).  Its constant folding is
 *                                       replicated where it changes code generation
 *                                       errors: fold_const_match_patterns (signed and
 *                                       complex literals in MatchValue / MatchMapping
 *                                       keys, including the int-to-float OverflowError
 *                                       that leaves a pattern unfolded) and the
 *                                       `__debug__` -> Constant fold (observable through
 *                                       codegen_return's location choice).
 *
 * Dependencies inside the unity build: symtable.h (entries, qp_symmap_next,
 * _PyST_IsFunctionLike, SRC_LOCATION_FROM_AST, NO_LOCATION), lexer.c
 * (qp_lex_is_printable = Py_UNICODE_ISPRINTABLE for str repr), quickjs.c
 * (string_get, js_bigint_to_string, js_bigint_to_float64, js_dtoa).
 */

#define QPV_MAXBLOCKS 21                /* CO_MAXBLOCKS */
#define QPV_INT_MAX_STR_DIGITS 4300     /* _PY_LONG_DEFAULT_MAX_STR_DIGITS */
#define QPV_LOC(x) SRC_LOCATION_FROM_AST(x)

/* Compiler unit scope types (COMPILE_SCOPE_*). */
enum {
    QPV_SCOPE_MODULE,
    QPV_SCOPE_CLASS,
    QPV_SCOPE_FUNCTION,
    QPV_SCOPE_ASYNC_FUNCTION,
    QPV_SCOPE_LAMBDA,
    QPV_SCOPE_COMPREHENSION,
    QPV_SCOPE_ANNOTATIONS,
};

/* Frame block types (COMPILE_FBLOCK_*). */
enum {
    QPV_FB_WHILE_LOOP,
    QPV_FB_FOR_LOOP,
    QPV_FB_ASYNC_FOR_LOOP,
    QPV_FB_TRY_EXCEPT,
    QPV_FB_FINALLY_TRY,
    QPV_FB_FINALLY_END,
    QPV_FB_WITH,
    QPV_FB_ASYNC_WITH,
    QPV_FB_HANDLER_CLEANUP,
    QPV_FB_POP_VALUE,
    QPV_FB_EXCEPTION_HANDLER,
    QPV_FB_EXCEPTION_GROUP_HANDLER,
    QPV_FB_ASYNC_COMPREHENSION_GENERATOR,
    QPV_FB_INLINED_COMPREHENSION,
    QPV_FB_STOP_ITERATION,
};

/* Comprehension kinds (COMP_*) and IterStackPosition. */
enum { QPV_COMP_GENEXP, QPV_COMP_LISTCOMP, QPV_COMP_SETCOMP, QPV_COMP_DICTCOMP };
enum { QPV_ITERABLE_IN_LOCAL = 0, QPV_ITERABLE_ON_STACK = 1, QPV_ITERATOR_ON_STACK = 2 };

typedef struct qpv_fblock {
    int type;
    _Py_SourceLocation loc;
    asdl_stmt_seq *finalbody;   /* QPV_FB_FINALLY_TRY: body re-visited on unwind */
} qpv_fblock;

/* One compiler unit (struct compiler_unit): the parts errors depend on. */
typedef struct qpv_unit {
    struct qpv_unit *prev;
    PySTEntryObject *ste;
    int scope_type;
    int nfblocks;
    qpv_fblock fblock[QPV_MAXBLOCKS];
    stmt_ty *deferred;          /* u_deferred_annotations (AnnAssign statements) */
    Py_ssize_t ndeferred, deferred_cap;
} qpv_unit;

typedef struct qpv {
    struct symtable *st;
    JSContext *ctx;
    qp_error *err;
    int features;               /* future features | compile flags */
    const char *filename;       /* SyntaxWarning location (c_filename) */
    JSValueConst module;        /* SyntaxWarning module (c_module) */
    int disable_warning;        /* c_disable_warning: inside FINALLY_END blocks */
    qpv_unit *u;
    JSAtom atom_debug;          /* "__debug__" */
    JSAtom atom_star;           /* "*" */
} qpv;

/* pattern_context: the names captured so far and whether an irrefutable
   pattern is allowed at this point. */
typedef struct qpv_pc {
    JSAtom *stores;
    Py_ssize_t nstores, cap;
    int allow_irrefutable;
} qpv_pc;

/* A (possibly folded) constant: the PyObject CPython's codegen sees in
   Constant.value after ast_preprocess. */
enum {
    QPV_V_NONE, QPV_V_ELLIPSIS, QPV_V_BOOL, QPV_V_INT, QPV_V_FLOAT,
    QPV_V_COMPLEX, QPV_V_STR, QPV_V_BYTES,
};

typedef struct qpv_val {
    int kind;
    int bval;                   /* QPV_V_BOOL */
    int neg;                    /* QPV_V_INT: the value is -(v) */
    JSValue v;                  /* INT, STR, BYTES (borrowed from the AST) */
    double re, im;              /* FLOAT: re; COMPLEX: re + im j */
    char *dec;                  /* QPV_V_INT: cached signed decimal digits (js_malloc_rt) */
} qpv_val;

static int qpv_visit_expr(qpv *c, expr_ty e);
static int qpv_visit_stmt(qpv *c, stmt_ty s);
static int qpv_pattern(qpv *c, pattern_ty p, qpv_pc *pc);

/* ---------------------------------------------------------------------- */
/* Errors */

static int qpv_no_memory(qpv *c)
{
    qp_error_set(c->err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
    return -1;
}

/* Drop a pending JS exception (only out-of-memory can occur here). */
static int qpv_js_failure(qpv *c)
{
    JS_FreeValue(c->ctx, JS_GetException(c->ctx));
    return qpv_no_memory(c);
}

/* _PyCompile_Error: SyntaxError at `loc`, offsets are col_offset + 1. */
static int qpv_error(qpv *c, _Py_SourceLocation loc, const char *msg)
{
    qp_error_set(c->err, QP_ERR_SYNTAX, loc.lineno, loc.col_offset + 1,
                 loc.end_lineno, loc.end_col_offset + 1, "%s", msg);
    return -1;
}

/* _PyCompile_Error with a message built in `b` (consumed). */
static int qpv_error_buf(qpv *c, _Py_SourceLocation loc, DynBuf *b)
{
    if (dbuf_putc(b, '\0') || b->error) {
        dbuf_free(b);
        return qpv_no_memory(c);
    }
    qpv_error(c, loc, (const char *)b->buf);
    dbuf_free(b);
    return -1;
}

/* Py_EnterRecursiveCall(" during compilation") */
static int qpv_check_recursion(qpv *c)
{
    if (js_check_stack_overflow(c->ctx->rt, 0)) {
        qp_error_set(c->err, QP_ERR_RECURSION, 0, 0, 0, 0, "%s",
                     "maximum recursion depth exceeded during compilation");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* SyntaxWarnings (codegen.c) */

/* _PyCompile_Warn: 0, or -1 with the error set (SyntaxError when the
   warning is raised as an error). */
static int qpv_warn(qpv *c, _Py_SourceLocation loc, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int qpv_warn(qpv *c, _Py_SourceLocation loc, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    if (c->disable_warning)
        return 0;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    return qp_syntax_warn_err(c->ctx, c->err, c->filename, c->module, msg,
                              loc.lineno, loc.col_offset + 1,
                              loc.end_lineno, loc.end_col_offset + 1);
}

/* tp_name of a Constant value */
static const char *qpv_const_type_name(qpv *c, const qp_const *v)
{
    switch (v->kind) {
    case QP_CONST_NONE: return "NoneType";
    case QP_CONST_TRUE:
    case QP_CONST_FALSE: return "bool";
    case QP_CONST_ELLIPSIS: return "ellipsis";
    case QP_CONST_INT: return "int";
    case QP_CONST_FLOAT: return "float";
    case QP_CONST_COMPLEX: return "complex";
    case QP_CONST_STR: return "str";
    case QP_CONST_BYTES: return "bytes";
    default: {
        QPType *t = qp_type_of(c->ctx, v->v);
        return t && t->tp_name ? t->tp_name : "object";
    }
    }
}

/* infer_type: the tp_name of the type of `e` when known from its kind,
   else NULL. */
static const char *qpv_infer_type(qpv *c, expr_ty e)
{
    switch (e->kind) {
    case Tuple_kind:
        return "tuple";
    case List_kind:
    case ListComp_kind:
        return "list";
    case Dict_kind:
    case DictComp_kind:
        return "dict";
    case Set_kind:
    case SetComp_kind:
        return "set";
    case GeneratorExp_kind:
        return "generator";
    case Lambda_kind:
        return "function";
    case TemplateStr_kind:
    case Interpolation_kind:
        return "string.templatelib.Template";
    case JoinedStr_kind:
    case FormattedValue_kind:
        return "str";
    case Constant_kind:
        return qpv_const_type_name(c, e->v.Constant.value);
    default:
        return NULL;
    }
}

/* Return false if the expression is a constant value except named singletons.
   Return true otherwise. */
static BOOL qpv_check_is_arg(expr_ty e)
{
    if (e->kind == Tuple_kind) {
        asdl_expr_seq *elts = e->v.Tuple.elts;
        for (Py_ssize_t i = 0; i < asdl_seq_LEN(elts); i++) {
            if (((expr_ty)asdl_seq_GET(elts, i))->kind != Constant_kind)
                return TRUE;
        }
        return FALSE;
    }
    if (e->kind != Constant_kind)
        return TRUE;
    qp_const_kind k = e->v.Constant.value->kind;
    return (k == QP_CONST_NONE
         || k == QP_CONST_FALSE
         || k == QP_CONST_TRUE
         || k == QP_CONST_ELLIPSIS);
}

/* codegen_check_compare: check operands of identity checks ("is" and
   "is not").  Emit a warning if any operand is a constant except named
   singletons. */
static int qpv_check_compare(qpv *c, expr_ty e)
{
    Py_ssize_t i, n;
    BOOL left = qpv_check_is_arg(e->v.Compare.left);
    expr_ty left_expr = e->v.Compare.left;
    n = asdl_seq_LEN(e->v.Compare.ops);
    for (i = 0; i < n; i++) {
        cmpop_ty op = (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, i);
        expr_ty right_expr = (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i);
        BOOL right = qpv_check_is_arg(right_expr);
        if (op == Is || op == IsNot) {
            if (!right || !left) {
                expr_ty literal = !left ? left_expr : right_expr;
                return qpv_warn(c, QPV_LOC(e),
                                op == Is
                                ? "\"is\" with '%.200s' literal. Did you mean \"==\"?"
                                : "\"is not\" with '%.200s' literal. Did you mean \"!=\"?",
                                qpv_infer_type(c, literal));
            }
        }
        left = right;
        left_expr = right_expr;
    }
    return 0;
}

/* check_caller */
static int qpv_check_caller(qpv *c, expr_ty e)
{
    switch (e->kind) {
    case Constant_kind:
    case Tuple_kind:
    case List_kind:
    case ListComp_kind:
    case Dict_kind:
    case DictComp_kind:
    case Set_kind:
    case SetComp_kind:
    case GeneratorExp_kind:
    case JoinedStr_kind:
    case TemplateStr_kind:
    case FormattedValue_kind:
    case Interpolation_kind:
        return qpv_warn(c, QPV_LOC(e), "'%.200s' object is not callable; "
                                       "perhaps you missed a comma?",
                        qpv_infer_type(c, e));
    default:
        return 0;
    }
}

/* check_subscripter */
static int qpv_check_subscripter(qpv *c, expr_ty e)
{
    switch (e->kind) {
    case Constant_kind: {
        const qp_const *v = e->v.Constant.value;
        switch (v->kind) {
        case QP_CONST_NONE:
        case QP_CONST_ELLIPSIS:
        case QP_CONST_TRUE:
        case QP_CONST_FALSE:
        case QP_CONST_INT:
        case QP_CONST_FLOAT:
        case QP_CONST_COMPLEX:
            break;
        case QP_CONST_OBJECT:
            if (qp_long_check(c->ctx, v->v) || qp_float_check(c->ctx, v->v) ||
                qp_complex_check(c->ctx, v->v) || qp_anyset_check(c->ctx, v->v))
                break;
            return 0;
        default:
            return 0;
        }
        break;
    }
    case Set_kind:
    case SetComp_kind:
    case GeneratorExp_kind:
    case TemplateStr_kind:
    case Interpolation_kind:
    case Lambda_kind:
        break;
    default:
        return 0;
    }
    return qpv_warn(c, QPV_LOC(e), "'%.200s' object is not subscriptable; "
                                   "perhaps you missed a comma?",
                    qpv_infer_type(c, e));
}

/* check_index */
static int qpv_check_index(qpv *c, expr_ty e, expr_ty s)
{
    const char *index_type = qpv_infer_type(c, s);

    if (index_type == NULL)
        return 0;
    if (s->kind == Constant_kind) {
        /* PyType_FastSubclass(index_type, Py_TPFLAGS_LONG_SUBCLASS) or
           PySlice_Type */
        const qp_const *v = s->v.Constant.value;
        if (v->kind == QP_CONST_INT || v->kind == QP_CONST_TRUE || v->kind == QP_CONST_FALSE)
            return 0;
        if (v->kind == QP_CONST_OBJECT &&
            (qp_long_check(c->ctx, v->v) || !strcmp(index_type, "slice")))
            return 0;
    }

    switch (e->kind) {
    case Constant_kind: {
        const qp_const *v = e->v.Constant.value;
        if (v->kind == QP_CONST_STR || v->kind == QP_CONST_BYTES)
            break;
        if (v->kind == QP_CONST_OBJECT &&
            (qp_str_check(c->ctx, v->v) || qp_bytes_check(c->ctx, v->v) ||
             qp_tuple_check(c->ctx, v->v)))
            break;
        return 0;
    }
    case Tuple_kind:
    case List_kind:
    case ListComp_kind:
    case JoinedStr_kind:
    case FormattedValue_kind:
        break;
    default:
        return 0;
    }
    return qpv_warn(c, QPV_LOC(e), "%.200s indices must be integers "
                                   "or slices, not %.200s; "
                                   "perhaps you missed a comma?",
                    qpv_infer_type(c, e), index_type);
}

/* ---------------------------------------------------------------------- */
/* str / bytes / identifier formatting (%U and %R of PyUnicode_FromFormat) */

static void qpv_put_cp_utf8(DynBuf *b, uint32_t ch)
{
    uint8_t buf[UTF8_CHAR_LEN_MAX];
    dbuf_put(b, buf, unicode_to_utf8(buf, ch));
}

/* Code point at *pi of a UTF-16 JSString, joining surrogate pairs. */
static uint32_t qpv_string_next(const JSString *p, uint32_t *pi)
{
    uint32_t i = *pi, c = string_get(p, i++);
    if (is_hi_surrogate(c) && i < p->len) {
        uint32_t c2 = string_get(p, i);
        if (is_lo_surrogate(c2)) {
            c = from_surrogate(c, c2);
            i++;
        }
    }
    *pi = i;
    return c;
}

/* unicode_repr + stringlib repr: the repr() of a str. */
static void qpv_put_str_repr(DynBuf *b, const JSString *p)
{
    static const char hexdigits[] = "0123456789abcdef";
    uint32_t i, ch, quote = '\'';
    int squote = 0, dquote = 0;

    for (i = 0; i < p->len; i++) {
        ch = string_get(p, i);
        if (ch == '\'')
            squote = 1;
        else if (ch == '"')
            dquote = 1;
    }
    if (squote && !dquote)
        quote = '"';
    dbuf_putc(b, quote);
    i = 0;
    while (i < p->len) {
        ch = qpv_string_next(p, &i);
        if (ch == quote || ch == '\\') {
            dbuf_putc(b, '\\');
            dbuf_putc(b, ch);
        } else if (ch == '\t') {
            dbuf_putstr(b, "\\t");
        } else if (ch == '\n') {
            dbuf_putstr(b, "\\n");
        } else if (ch == '\r') {
            dbuf_putstr(b, "\\r");
        } else if (ch < ' ' || ch == 0x7f) {
            dbuf_putstr(b, "\\x");
            dbuf_putc(b, hexdigits[(ch >> 4) & 0xf]);
            dbuf_putc(b, hexdigits[ch & 0xf]);
        } else if (ch < 0x7f) {
            dbuf_putc(b, ch);
        } else if (!qp_lex_is_printable(ch)) {
            int n, k;
            dbuf_putc(b, '\\');
            if (ch <= 0xff) {
                dbuf_putc(b, 'x');
                n = 2;
            } else if (ch <= 0xffff) {
                dbuf_putc(b, 'u');
                n = 4;
            } else {
                dbuf_putc(b, 'U');
                n = 8;
            }
            for (k = n - 1; k >= 0; k--)
                dbuf_putc(b, hexdigits[(ch >> (4 * k)) & 0xf]);
        } else {
            qpv_put_cp_utf8(b, ch);
        }
    }
    dbuf_putc(b, quote);
}

/* _Py_bytes_repr(smartquotes=1): the repr() of a bytes object whose bytes
   are the chars of `p`. */
static void qpv_put_bytes_repr(DynBuf *b, const JSString *p)
{
    static const char hexdigits[] = "0123456789abcdef";
    uint32_t i, ch, quote = '\'';
    int squote = 0, dquote = 0;

    for (i = 0; i < p->len; i++) {
        ch = string_get(p, i);
        if (ch == '\'')
            squote = 1;
        else if (ch == '"')
            dquote = 1;
    }
    if (squote && !dquote)
        quote = '"';
    dbuf_putc(b, 'b');
    dbuf_putc(b, quote);
    for (i = 0; i < p->len; i++) {
        ch = string_get(p, i) & 0xff;
        if (ch == quote || ch == '\\') {
            dbuf_putc(b, '\\');
            dbuf_putc(b, ch);
        } else if (ch == '\t') {
            dbuf_putstr(b, "\\t");
        } else if (ch == '\n') {
            dbuf_putstr(b, "\\n");
        } else if (ch == '\r') {
            dbuf_putstr(b, "\\r");
        } else if (ch < ' ' || ch >= 0x7f) {
            dbuf_putstr(b, "\\x");
            dbuf_putc(b, hexdigits[(ch >> 4) & 0xf]);
            dbuf_putc(b, hexdigits[ch & 0xf]);
        } else {
            dbuf_putc(b, ch);
        }
    }
    dbuf_putc(b, quote);
}

/* %U of an identifier. */
static int qpv_put_atom_utf8(qpv *c, DynBuf *b, JSAtom atom)
{
    size_t len;
    const char *s = JS_AtomToCStringLen(c->ctx, &len, atom);
    if (!s)
        return qpv_js_failure(c);
    dbuf_put(b, (const uint8_t *)s, len);
    JS_FreeCString(c->ctx, s);
    return 0;
}

/* %R of an identifier (a str). */
static int qpv_put_atom_repr(qpv *c, DynBuf *b, JSAtom atom)
{
    JSValue s = JS_AtomToString(c->ctx, atom);
    if (JS_IsException(s))
        return qpv_js_failure(c);
    qpv_put_str_repr(b, JS_VALUE_GET_STRING(s));
    JS_FreeValue(c->ctx, s);
    return 0;
}

/* SyntaxError "<prefix><name><suffix>" where name is formatted with %R
   (use_repr) or %U. */
static int qpv_error_name(qpv *c, _Py_SourceLocation loc, const char *prefix,
                          JSAtom name, const char *suffix, int use_repr)
{
    DynBuf b;
    int r;

    dbuf_init(&b);
    dbuf_putstr(&b, prefix);
    r = use_repr ? qpv_put_atom_repr(c, &b, name) : qpv_put_atom_utf8(c, &b, name);
    if (r < 0) {
        dbuf_free(&b);
        return -1;
    }
    dbuf_putstr(&b, suffix);
    return qpv_error_buf(c, loc, &b);
}

/* ---------------------------------------------------------------------- */
/* Constants: folding (ast_preprocess.c), equality (set membership) and repr */

static double qpv_number(JSValue v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return JS_VALUE_GET_INT(v);
    return JS_VALUE_GET_FLOAT64(v);
}

static void qpv_val_init(qpv_val *out, int kind)
{
    out->kind = kind;
    out->bval = 0;
    out->neg = 0;
    out->v = JS_UNDEFINED;
    out->re = 0.0;
    out->im = 0.0;
    out->dec = NULL;
}

static void qpv_val_free(qpv *c, qpv_val *v)
{
    js_free_rt(c->ctx->rt, v->dec);
    v->dec = NULL;
}

/* The value of a Constant node. */
static void qpv_val_from_const(const qp_const *k, qpv_val *out)
{
    switch (k->kind) {
    case QP_CONST_NONE:
        qpv_val_init(out, QPV_V_NONE);
        break;
    case QP_CONST_TRUE:
    case QP_CONST_FALSE:
        qpv_val_init(out, QPV_V_BOOL);
        out->bval = k->kind == QP_CONST_TRUE;
        break;
    case QP_CONST_ELLIPSIS:
        qpv_val_init(out, QPV_V_ELLIPSIS);
        break;
    case QP_CONST_INT:
        qpv_val_init(out, QPV_V_INT);
        out->v = k->v;
        break;
    case QP_CONST_FLOAT:
        qpv_val_init(out, QPV_V_FLOAT);
        out->re = qpv_number(k->v);
        break;
    case QP_CONST_COMPLEX:
        /* an imaginary literal: complex(0.0, imag) */
        qpv_val_init(out, QPV_V_COMPLEX);
        out->im = qpv_number(k->v);
        break;
    case QP_CONST_STR:
        qpv_val_init(out, QPV_V_STR);
        out->v = k->v;
        break;
    case QP_CONST_OBJECT:
        /* constant of an AST object: only an exact complex can reach
           pattern checks (_PyAST_Validate rejects the other types) */
        if (JS_VALUE_GET_TAG(k->v) == JS_TAG_OBJECT &&
            JS_VALUE_GET_OBJ(k->v)->class_id == JS_CLASS_PY_COMPLEX) {
            const QPComplex *z = JS_VALUE_GET_OBJ(k->v)->u.opaque;
            qpv_val_init(out, QPV_V_COMPLEX);
            out->re = z->real;
            out->im = z->imag;
            break;
        }
        /* fall through */
    case QP_CONST_BYTES:
    default:
        qpv_val_init(out, QPV_V_BYTES);
        out->v = k->v;
        break;
    }
}

/* PyNumber_Negative (neg) / PyNumber_Positive on a constant: 1 if folded,
   0 on TypeError (non-numeric operand: left unfolded, as make_const does). */
static int qpv_fold_unary(const qpv_val *a, int neg, qpv_val *out)
{
    switch (a->kind) {
    case QPV_V_BOOL:
        qpv_val_init(out, QPV_V_INT);
        out->v = JS_MKVAL(JS_TAG_INT, a->bval);
        out->neg = neg;
        return 1;
    case QPV_V_INT:
        *out = *a;
        out->dec = NULL;
        if (neg)
            out->neg = !a->neg;
        return 1;
    case QPV_V_FLOAT:
        *out = *a;
        if (neg)
            out->re = -a->re;
        return 1;
    case QPV_V_COMPLEX:
        *out = *a;
        if (neg) {
            out->re = -a->re;
            out->im = -a->im;
        }
        return 1;
    default:
        return 0;
    }
}

/* PyLong_AsDouble of an int constant: 0 and *d, or 1 on OverflowError. */
static int qpv_int_to_double(qpv *c, const qpv_val *a, double *d)
{
    JSValue v = a->v;
    double x;

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
        x = JS_VALUE_GET_INT(v);
        break;
    case JS_TAG_SHORT_BIG_INT:
        x = (double)(int64_t)JS_VALUE_GET_SHORT_BIG_INT(v);
        break;
    default:
        /* correctly rounded, +/-inf when the magnitude needs >= 2**1024 */
        x = js_bigint_to_float64(c->ctx, JS_VALUE_GET_PTR(v));
        if (isinf(x))
            return 1;
        break;
    }
    *d = a->neg ? -x : x;
    return 0;
}

/* PyNumber_Add / PyNumber_Subtract as reached by fold_const_match_patterns.
   The grammar (complex_number: signed_real_number ('+'|'-')
   imaginary_number) makes the right operand an imaginary literal, so only
   complex_add/complex_sub with a complex right operand apply (C11 Annex G
   mixed-mode rules).  1 if folded, 0 if CPython leaves the node unfolded
   (int too large for a float, or an operand type the grammar never
   produces). */
static int qpv_fold_add_sub(qpv *c, const qpv_val *l, const qpv_val *r,
                            int is_sub, qpv_val *out)
{
    double a;

    if (r->kind != QPV_V_COMPLEX)
        return 0;
    qpv_val_init(out, QPV_V_COMPLEX);
    if (l->kind == QPV_V_COMPLEX) {
        /* _Py_c_sum / _Py_c_diff */
        out->re = is_sub ? l->re - r->re : l->re + r->re;
        out->im = is_sub ? l->im - r->im : l->im + r->im;
        return 1;
    }
    if (l->kind == QPV_V_FLOAT) {
        a = l->re;
    } else if (l->kind == QPV_V_INT) {
        if (qpv_int_to_double(c, l, &a))
            return 0;   /* OverflowError: make_const clears it, no fold */
    } else if (l->kind == QPV_V_BOOL) {
        a = l->bval;
    } else {
        return 0;
    }
    if (is_sub) {
        /* _Py_rc_diff */
        out->re = a - r->re;
        out->im = -r->im;
    } else {
        /* _Py_rc_sum */
        out->re = r->re + a;
        out->im = r->im;
    }
    return 1;
}

/* The Constant node `e` would be after fold_const_match_patterns:
   1 with *out if `e` is or folds to a constant, 0 otherwise. */
static int qpv_fold_match_const(qpv *c, expr_ty e, qpv_val *out)
{
    qpv_val a, b;

    switch (e->kind) {
    case Constant_kind:
        qpv_val_from_const(e->v.Constant.value, out);
        return 1;
    case UnaryOp_kind:
        if ((e->v.UnaryOp.op == USub || e->v.UnaryOp.op == UAdd) &&
            e->v.UnaryOp.operand->kind == Constant_kind) {
            qpv_val_from_const(e->v.UnaryOp.operand->v.Constant.value, &a);
            return qpv_fold_unary(&a, e->v.UnaryOp.op == USub, out);
        }
        return 0;
    case BinOp_kind:
        if ((e->v.BinOp.op == Add || e->v.BinOp.op == Sub) &&
            e->v.BinOp.right->kind == Constant_kind) {
            if (!qpv_fold_match_const(c, e->v.BinOp.left, &a))
                return 0;
            qpv_val_from_const(e->v.BinOp.right->v.Constant.value, &b);
            return qpv_fold_add_sub(c, &a, &b, e->v.BinOp.op == Sub, out);
        }
        return 0;
    default:
        return 0;
    }
}

/* Signed decimal digits of an int value (no "-0"): either written to
   `buf` (>= 32 bytes) or cached in v->dec.  NULL with the error set on
   OOM. */
static const char *qpv_int_digits(qpv *c, qpv_val *v, char *buf)
{
    JSValue s;
    const char *cs, *digits;
    size_t len;
    int64_t x;

    switch (JS_VALUE_GET_TAG(v->v)) {
    case JS_TAG_INT:
        x = JS_VALUE_GET_INT(v->v);
        snprintf(buf, 32, "%" PRId64, v->neg ? -x : x);
        return buf;
    case JS_TAG_SHORT_BIG_INT:
        x = (int64_t)JS_VALUE_GET_SHORT_BIG_INT(v->v);
        if (!v->neg)
            snprintf(buf, 32, "%" PRId64, x);
        else if (x == INT64_MIN)
            snprintf(buf, 32, "9223372036854775808");
        else
            snprintf(buf, 32, "%" PRId64, -x);
        return buf;
    default:
        break;
    }
    if (v->dec)
        return v->dec;
    s = js_bigint_to_string(c->ctx, v->v);
    if (JS_IsException(s)) {
        qpv_js_failure(c);
        return NULL;
    }
    cs = JS_ToCStringLen(c->ctx, &len, s);
    JS_FreeValue(c->ctx, s);
    if (!cs) {
        qpv_js_failure(c);
        return NULL;
    }
    v->dec = js_malloc_rt(c->ctx->rt, len + 2);
    if (!v->dec) {
        JS_FreeCString(c->ctx, cs);
        qpv_no_memory(c);
        return NULL;
    }
    digits = cs[0] == '-' ? cs + 1 : cs;
    if (v->neg == (cs[0] == '-') || strcmp(digits, "0") == 0)
        strcpy(v->dec, digits);
    else
        snprintf(v->dec, len + 2, "-%s", digits);
    JS_FreeCString(c->ctx, cs);
    return v->dec;
}

/* int == float (float_richcompare with an int operand): exact. */
static int qpv_int_eq_double(qpv *c, qpv_val *iv, double d)
{
    char buf[32], fbuf[400];
    const char *digits;

    if (!isfinite(d) || floor(d) != d)
        return 0;
    if (JS_VALUE_GET_TAG(iv->v) == JS_TAG_INT) {
        double x = JS_VALUE_GET_INT(iv->v);
        return (iv->neg ? -x : x) == d;
    }
    digits = qpv_int_digits(c, iv, buf);
    if (!digits)
        return -1;
    /* an integral double below 2**1024 printed exactly */
    snprintf(fbuf, sizeof(fbuf), "%.0f", d);
    if (strcmp(fbuf, "-0") == 0)
        strcpy(fbuf, "0");
    return strcmp(digits, fbuf) == 0;
}

static int qpv_int_eq_int(qpv *c, qpv_val *a, qpv_val *b)
{
    char buf1[32], buf2[32];
    const char *d1, *d2;

    if (JS_VALUE_GET_TAG(a->v) == JS_TAG_INT && JS_VALUE_GET_TAG(b->v) == JS_TAG_INT) {
        int64_t x = JS_VALUE_GET_INT(a->v), y = JS_VALUE_GET_INT(b->v);
        return (a->neg ? -x : x) == (b->neg ? -y : y);
    }
    d1 = qpv_int_digits(c, a, buf1);
    if (!d1)
        return -1;
    d2 = qpv_int_digits(c, b, buf2);
    if (!d2)
        return -1;
    return strcmp(d1, d2) == 0;
}

static int qpv_jsstring_eq(JSValue a, JSValue b)
{
    const JSString *p1 = JS_VALUE_GET_STRING(a), *p2 = JS_VALUE_GET_STRING(b);
    uint32_t i;

    if (p1->len != p2->len)
        return 0;
    for (i = 0; i < p1->len; i++) {
        if (string_get(p1, i) != string_get(p2, i))
            return 0;
    }
    return 1;
}

static int qpv_is_number(int kind)
{
    return kind == QPV_V_BOOL || kind == QPV_V_INT ||
           kind == QPV_V_FLOAT || kind == QPV_V_COMPLEX;
}

/* Python `a == b` for two constants (what PySet_Contains decides for the
   hashable constants a pattern key can be): 1, 0, or -1 on error. */
static int qpv_val_eq(qpv *c, qpv_val *a, qpv_val *b)
{
    qpv_val ta, tb;

    if (qpv_is_number(a->kind) && qpv_is_number(b->kind)) {
        /* bool is an int subclass */
        if (a->kind == QPV_V_BOOL) {
            qpv_fold_unary(a, 0, &ta);
            a = &ta;
        }
        if (b->kind == QPV_V_BOOL) {
            qpv_fold_unary(b, 0, &tb);
            b = &tb;
        }
        if (a->kind == QPV_V_COMPLEX || b->kind == QPV_V_COMPLEX) {
            qpv_val *z, *x;
            qpv_val real;
            if (a->kind == QPV_V_COMPLEX && b->kind == QPV_V_COMPLEX)
                return a->re == b->re && a->im == b->im;
            z = a->kind == QPV_V_COMPLEX ? a : b;
            x = z == a ? b : a;
            if (z->im != 0.0)
                return 0;
            qpv_val_init(&real, QPV_V_FLOAT);
            real.re = z->re;
            a = &real;
            b = x;
            if (b->kind == QPV_V_FLOAT)
                return a->re == b->re;
            return qpv_int_eq_double(c, b, a->re);
        }
        if (a->kind == QPV_V_FLOAT && b->kind == QPV_V_FLOAT)
            return a->re == b->re;
        if (a->kind == QPV_V_FLOAT)
            return qpv_int_eq_double(c, b, a->re);
        if (b->kind == QPV_V_FLOAT)
            return qpv_int_eq_double(c, a, b->re);
        return qpv_int_eq_int(c, a, b);
    }
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case QPV_V_STR:
    case QPV_V_BYTES:
        return qpv_jsstring_eq(a->v, b->v);
    default:
        return 1;   /* None, Ellipsis: singletons */
    }
}

/* PyOS_double_to_string(x, 'r', 0, flags) (format_float_short, mode 0):
   the shortest round-tripping digits, exponent notation when the decimal
   point position is <= -4 or > 16. */
static void qpv_put_double_repr(DynBuf *b, double x, int always_add_sign,
                                int add_dot_0_if_integer)
{
    char buf[64], digits[40], out[64];
    char *p;
    JSDTOATempMem tmp;
    int digits_len = 0, decpt, use_exp = 0, exp = 0, i, len;
    int vdigits_start, vdigits_end;

    if (isnan(x)) {
        /* unreachable for folded pattern literals */
        if (always_add_sign)
            dbuf_putc(b, '+');
        dbuf_putstr(b, "nan");
        return;
    }
    if (signbit(x))
        dbuf_putc(b, '-');
    else if (always_add_sign)
        dbuf_putc(b, '+');
    x = fabs(x);
    if (isinf(x)) {
        dbuf_putstr(b, "inf");
        return;
    }
    if (x == 0.0) {
        digits[digits_len++] = '0';
        decpt = 1;
    } else {
        /* "d[.ddd]e[+-]N" */
        len = js_dtoa(buf, x, 10, 0, JS_DTOA_FORMAT_FREE | JS_DTOA_EXP_ENABLED, &tmp);
        exp = 0;
        for (i = 0; i < len && buf[i] != 'e'; i++) {
            if (buf[i] >= '0' && buf[i] <= '9' && digits_len < (int)sizeof(digits))
                digits[digits_len++] = buf[i];
        }
        if (i < len)
            exp = atoi(buf + i + 1);
        decpt = exp + 1;
        exp = 0;
    }

    if (decpt <= -4 || decpt > 16)
        use_exp = 1;
    vdigits_end = digits_len;
    if (use_exp) {
        exp = decpt - 1;
        decpt = 1;
    }
    vdigits_start = decpt <= 0 ? decpt - 1 : 0;
    if (!use_exp && add_dot_0_if_integer)
        vdigits_end = vdigits_end > decpt ? vdigits_end : decpt + 1;
    else
        vdigits_end = vdigits_end > decpt ? vdigits_end : decpt;

    p = out;
    if (decpt <= 0) {
        memset(p, '0', decpt - vdigits_start);
        p += decpt - vdigits_start;
        *p++ = '.';
        memset(p, '0', 0 - decpt);
        p += 0 - decpt;
    } else {
        memset(p, '0', 0 - vdigits_start);
        p += 0 - vdigits_start;
    }
    if (0 < decpt && decpt <= digits_len) {
        memcpy(p, digits, decpt);
        p += decpt;
        *p++ = '.';
        memcpy(p, digits + decpt, digits_len - decpt);
        p += digits_len - decpt;
    } else {
        memcpy(p, digits, digits_len);
        p += digits_len;
    }
    if (digits_len < decpt) {
        memset(p, '0', decpt - digits_len);
        p += decpt - digits_len;
        *p++ = '.';
        memset(p, '0', vdigits_end - decpt);
        p += vdigits_end - decpt;
    } else {
        memset(p, '0', vdigits_end - digits_len);
        p += vdigits_end - digits_len;
    }
    if (p[-1] == '.')
        p--;
    dbuf_put(b, (const uint8_t *)out, p - out);
    if (use_exp)
        dbuf_printf(b, "e%+.02d", exp);
}

/* repr() of a constant (%R): 0, or -1 with the error set (OOM, or the
   ValueError of int-to-str conversion beyond 4300 digits). */
static int qpv_put_val_repr(qpv *c, DynBuf *b, qpv_val *v)
{
    char buf[32];
    const char *digits;
    size_t ndigits;

    switch (v->kind) {
    case QPV_V_NONE:
        dbuf_putstr(b, "None");
        break;
    case QPV_V_ELLIPSIS:
        dbuf_putstr(b, "Ellipsis");
        break;
    case QPV_V_BOOL:
        dbuf_putstr(b, v->bval ? "True" : "False");
        break;
    case QPV_V_INT:
        digits = qpv_int_digits(c, v, buf);
        if (!digits)
            return -1;
        ndigits = strlen(digits) - (digits[0] == '-');
        if (ndigits > QPV_INT_MAX_STR_DIGITS) {
            qp_error_set(c->err, QP_ERR_VALUE, 0, 0, 0, 0,
                         "Exceeds the limit (%d digits) for integer string conversion; "
                         "use sys.set_int_max_str_digits() to increase the limit",
                         QPV_INT_MAX_STR_DIGITS);
            return -1;
        }
        dbuf_putstr(b, digits);
        break;
    case QPV_V_FLOAT:
        qpv_put_double_repr(b, v->re, 0, 1);
        break;
    case QPV_V_COMPLEX:
        /* complex_repr */
        if (v->re == 0.0 && copysign(1.0, v->re) == 1.0) {
            qpv_put_double_repr(b, v->im, 0, 0);
            dbuf_putc(b, 'j');
        } else {
            dbuf_putc(b, '(');
            qpv_put_double_repr(b, v->re, 0, 0);
            qpv_put_double_repr(b, v->im, 1, 0);
            dbuf_putstr(b, "j)");
        }
        break;
    case QPV_V_STR:
        qpv_put_str_repr(b, JS_VALUE_GET_STRING(v->v));
        break;
    case QPV_V_BYTES:
        qpv_put_bytes_repr(b, JS_VALUE_GET_STRING(v->v));
        break;
    }
    return 0;
}

/* Constant after ast_preprocess: a Constant node, or a `__debug__` load
   (folded to True by astfold_expr). */
static int qpv_is_folded_constant(qpv *c, expr_ty e)
{
    return e->kind == Constant_kind ||
           (e->kind == Name_kind && e->v.Name.ctx == Load &&
            e->v.Name.id == c->atom_debug);
}

/* ---------------------------------------------------------------------- */
/* Compiler units and frame blocks (compile.c) */

/* _PyCompile_EnterScope with an already looked-up entry. */
static void qpv_enter_scope_ste(qpv *c, qpv_unit *u, int scope_type, PySTEntryObject *ste)
{
    u->prev = c->u;
    u->ste = ste;
    u->scope_type = scope_type;
    u->nfblocks = 0;
    u->deferred = NULL;
    u->ndeferred = 0;
    u->deferred_cap = 0;
    c->u = u;
}

/* _PyCompile_EnterScope: the unit's entry is st_blocks[key]. */
static int qpv_enter_scope(qpv *c, qpv_unit *u, int scope_type, void *key)
{
    PySTEntryObject *ste = _PySymtable_Lookup(c->st, key);
    if (!ste)
        return -1;
    qpv_enter_scope_ste(c, u, scope_type, ste);
    return 0;
}

static void qpv_exit_scope(qpv *c)
{
    qpv_unit *u = c->u;
    js_free_rt(c->ctx->rt, u->deferred);
    c->u = u->prev;
}

/* _PyCompile_PushFBlock */
static int qpv_push_fblock(qpv *c, _Py_SourceLocation loc, int type,
                           asdl_stmt_seq *finalbody)
{
    qpv_unit *u = c->u;
    qpv_fblock *f;

    if (u->nfblocks >= QPV_MAXBLOCKS)
        return qpv_error(c, loc, "too many statically nested blocks");
    f = &u->fblock[u->nfblocks++];
    f->type = type;
    f->loc = loc;
    f->finalbody = finalbody;
    if (type == QPV_FB_FINALLY_END)
        c->disable_warning++;
    return 0;
}

/* _PyCompile_PopFBlock */
static void qpv_pop_fblock(qpv *c)
{
    qpv_unit *u = c->u;

    if (u->fblock[--u->nfblocks].type == QPV_FB_FINALLY_END)
        c->disable_warning--;
}

/* _PyCompile_AddDeferredAnnotation */
static int qpv_add_deferred_annotation(qpv *c, stmt_ty s)
{
    qpv_unit *u = c->u;

    if (u->ndeferred == u->deferred_cap) {
        Py_ssize_t cap = u->deferred_cap ? u->deferred_cap * 2 : 8;
        stmt_ty *d = js_realloc_rt(c->ctx->rt, u->deferred, cap * sizeof(*d));
        if (!d)
            return qpv_no_memory(c);
        u->deferred = d;
        u->deferred_cap = cap;
    }
    u->deferred[u->ndeferred++] = s;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Sequences */

static int qpv_visit_stmts(qpv *c, asdl_stmt_seq *seq)
{
    Py_ssize_t i;
    for (i = 0; i < asdl_seq_LEN(seq); i++) {
        if (qpv_visit_stmt(c, asdl_seq_GET(seq, i)) < 0)
            return -1;
    }
    return 0;
}

static int qpv_visit_exprs(qpv *c, asdl_expr_seq *seq)
{
    Py_ssize_t i;
    for (i = 0; i < asdl_seq_LEN(seq); i++) {
        if (qpv_visit_expr(c, asdl_seq_GET(seq, i)) < 0)
            return -1;
    }
    return 0;
}

/* starunpack_helper: elements in order, a Starred element contributes its
   value only. */
static int qpv_starunpack(qpv *c, asdl_expr_seq *elts)
{
    Py_ssize_t i;
    for (i = 0; i < asdl_seq_LEN(elts); i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (qpv_visit_expr(c, elt->kind == Starred_kind ? elt->v.Starred.value : elt) < 0)
            return -1;
    }
    return 0;
}

/* unpack_helper + assignment_helper (List/Tuple store targets). */
static int qpv_assignment_helper(qpv *c, _Py_SourceLocation loc, asdl_expr_seq *elts)
{
    Py_ssize_t i, n = asdl_seq_LEN(elts);
    int seen_star = 0;

    for (i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind && !seen_star) {
            if ((i >= (1 << 8)) || (n - i - 1 >= (INT_MAX >> 8)))
                return qpv_error(c, loc, "too many expressions in star-unpacking assignment");
            seen_star = 1;
        } else if (elt->kind == Starred_kind) {
            return qpv_error(c, loc, "multiple starred expressions in assignment");
        }
    }
    return qpv_starunpack(c, elts);
}

/* ---------------------------------------------------------------------- */
/* Calls */

/* codegen_validate_keywords */
static int qpv_validate_keywords(qpv *c, asdl_keyword_seq *keywords)
{
    Py_ssize_t i, j, n = asdl_seq_LEN(keywords);

    for (i = 0; i < n; i++) {
        keyword_ty key = asdl_seq_GET(keywords, i);
        if (key->arg == JS_ATOM_NULL)
            continue;
        for (j = i + 1; j < n; j++) {
            keyword_ty other = asdl_seq_GET(keywords, j);
            if (other->arg != JS_ATOM_NULL && other->arg == key->arg)
                return qpv_error_name(c, QPV_LOC(other), "keyword argument repeated: ",
                                      key->arg, "", 0);
        }
    }
    return 0;
}

/* codegen_call_helper_impl: keyword check, then positional arguments
   (starred ones by value) and keyword values, in order. */
static int qpv_call_helper(qpv *c, asdl_expr_seq *args, asdl_keyword_seq *keywords)
{
    Py_ssize_t i;

    if (qpv_validate_keywords(c, keywords) < 0)
        return -1;
    if (qpv_starunpack(c, args) < 0)
        return -1;
    for (i = 0; i < asdl_seq_LEN(keywords); i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        if (qpv_visit_expr(c, kw->value) < 0)
            return -1;
    }
    return 0;
}

/* maybe_optimize_function_call: all/any/tuple/list/set called with a single
   (non-async) generator expression get an inlined loop over it before the
   fallback call, so the genexp is compiled twice. */
static int qpv_optimizes_function_call(qpv *c, expr_ty e)
{
    static const char *const names[] = { "all", "any", "tuple", "list", "set" };
    expr_ty func = e->v.Call.func;
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *name;
    PySTEntryObject *entry;
    size_t i;

    if (!(func->kind == Name_kind &&
          asdl_seq_LEN(e->v.Call.args) == 1 &&
          asdl_seq_LEN(e->v.Call.keywords) == 0 &&
          asdl_seq_GET(e->v.Call.args, 0)->kind == GeneratorExp_kind))
        return 0;
    entry = _PySymtable_Lookup(c->st, asdl_seq_GET(e->v.Call.args, 0));
    if (!entry || entry->ste_coroutine)
        return 0;
    name = JS_AtomGetStr(c->ctx, buf, sizeof(buf), func->v.Name.id);
    for (i = 0; i < countof(names); i++) {
        if (!strcmp(name, names[i]))
            return 1;
    }
    return 0;
}

/* codegen_call.  maybe_optimize_method_call and can_optimize_super_call
   visit the same sub-expressions in the same order as the generic path
   (receiver, arguments, keyword values), so only the generic order is
   walked; check_caller cannot warn for the Attribute callee of an
   optimized method call. */
static int qpv_call(qpv *c, expr_ty e)
{
    if (qpv_validate_keywords(c, e->v.Call.keywords) < 0)
        return -1;
    if (qpv_check_caller(c, e->v.Call.func) < 0)
        return -1;
    if (qpv_visit_expr(c, e->v.Call.func) < 0)
        return -1;
    if (qpv_optimizes_function_call(c, e) &&
        qpv_visit_expr(c, asdl_seq_GET(e->v.Call.args, 0)) < 0)
        return -1;
    return qpv_call_helper(c, e->v.Call.args, e->v.Call.keywords);
}

/* ---------------------------------------------------------------------- */
/* Comprehensions */

/* codegen_push_inlined_comprehension_locals: an INLINED_COMPREHENSION
   block is pushed when some name is bound locally in the comprehension.
   (Its `in_class_block` term is always false there: it is evaluated after
   _PyCompile_TweakInlinedComprehensionScopes incremented
   u_in_inlined_comp.) */
static int qpv_comp_pushes_locals(PySTEntryObject *entry)
{
    Py_ssize_t pos = 0;
    JSAtom name;
    long flags;

    while (qp_symmap_next(&entry->ste_symbols, &pos, &name, &flags)) {
        if ((flags & DEF_LOCAL) && !(flags & DEF_NONLOCAL))
            return 1;
    }
    return 0;
}

/* codegen_sync_comprehension_generator / codegen_async_comprehension_generator */
static int qpv_comprehension_generator(qpv *c, _Py_SourceLocation loc,
                                       asdl_comprehension_seq *generators,
                                       int gen_index, expr_ty elt, expr_ty val,
                                       int type, int iter_pos)
{
    comprehension_ty gen = asdl_seq_GET(generators, gen_index);
    Py_ssize_t i;

    if (iter_pos == QPV_ITERABLE_IN_LOCAL && gen_index != 0) {
        /* sub-iterator, computed on the fly (the `for y in [f(x)]` fast
           path visits the single element, which is what visiting the
           List/Tuple does) */
        if (qpv_visit_expr(c, gen->iter) < 0)
            return -1;
    }
    if (gen->is_async) {
        /* the runtime pushes a block around __anext__ */
        if (qpv_push_fblock(c, loc, QPV_FB_ASYNC_COMPREHENSION_GENERATOR, NULL) < 0)
            return -1;
    }
    if (qpv_visit_expr(c, gen->target) < 0)
        return -1;
    for (i = 0; i < asdl_seq_LEN(gen->ifs); i++) {
        if (qpv_visit_expr(c, asdl_seq_GET(gen->ifs, i)) < 0)
            return -1;
    }
    if (++gen_index < asdl_seq_LEN(generators)) {
        if (qpv_comprehension_generator(c, loc, generators, gen_index, elt, val,
                                        type, QPV_ITERABLE_IN_LOCAL) < 0)
            return -1;
    }
    if (gen_index >= asdl_seq_LEN(generators)) {
        if (type == QPV_COMP_DICTCOMP) {
            if (qpv_visit_expr(c, elt) < 0)
                return -1;
            if (val && qpv_visit_expr(c, val) < 0)
                return -1;
        } else {
            if (qpv_visit_expr(c, elt->kind == Starred_kind ? elt->v.Starred.value : elt) < 0)
                return -1;
        }
    }
    if (gen->is_async)
        qpv_pop_fblock(c);
    return 0;
}

/* codegen_comprehension */
static int qpv_comprehension(qpv *c, expr_ty e, int type,
                             asdl_comprehension_seq *generators,
                             expr_ty elt, expr_ty val)
{
    PySTEntryObject *entry = _PySymtable_Lookup(c->st, e);
    comprehension_ty outermost;
    _Py_SourceLocation loc = QPV_LOC(e);
    qpv_unit u;
    int pushed = 0, iter_state, ret;

    if (!entry)
        return -1;
    outermost = asdl_seq_GET(generators, 0);
    if (entry->ste_comp_inlined) {
        if (qpv_visit_expr(c, outermost->iter) < 0)
            return -1;
        if (qpv_comp_pushes_locals(entry)) {
            if (qpv_push_fblock(c, loc, QPV_FB_INLINED_COMPREHENSION, NULL) < 0)
                return -1;
            pushed = 1;
        }
        ret = qpv_comprehension_generator(c, loc, generators, 0, elt, val, type,
                                          QPV_ITERABLE_ON_STACK);
        if (ret == 0 && pushed)
            qpv_pop_fblock(c);
        return ret;
    }
    /* receive the outermost iterable as an implicit argument */
    qpv_enter_scope_ste(c, &u, QPV_SCOPE_COMPREHENSION, entry);
    iter_state = type == QPV_COMP_GENEXP ? QPV_ITERATOR_ON_STACK : QPV_ITERABLE_IN_LOCAL;
    ret = qpv_comprehension_generator(c, loc, generators, 0, elt, val, type, iter_state);
    qpv_exit_scope(c);
    if (ret < 0)
        return -1;
    return qpv_visit_expr(c, outermost->iter);
}

/* ---------------------------------------------------------------------- */
/* Functions, lambdas, classes, type parameters, annotations */

/* codegen_default_arguments: positional defaults, then keyword-only
   defaults in parameter order. */
static int qpv_default_arguments(qpv *c, arguments_ty args)
{
    Py_ssize_t i;

    if (qpv_visit_exprs(c, args->defaults) < 0)
        return -1;
    for (i = 0; i < asdl_seq_LEN(args->kwonlyargs); i++) {
        expr_ty default_ = asdl_seq_GET(args->kw_defaults, i);
        if (default_ && qpv_visit_expr(c, default_) < 0)
            return -1;
    }
    return 0;
}

/* codegen_type_param_bound_or_default: evaluated in its own annotation
   scope keyed by `key`. */
static int qpv_type_param_bound_or_default(qpv *c, expr_ty e, void *key, int allow_starred)
{
    qpv_unit u;
    int ret;

    if (qpv_enter_scope(c, &u, QPV_SCOPE_ANNOTATIONS, key) < 0)
        return -1;
    if (allow_starred && e->kind == Starred_kind)
        ret = qpv_visit_expr(c, e->v.Starred.value);
    else
        ret = qpv_visit_expr(c, e);
    qpv_exit_scope(c);
    return ret;
}

#define QPV_NONDEFAULT_MSG_PREFIX "non-default type parameter '"
#define QPV_NONDEFAULT_MSG_SUFFIX "' follows default type parameter"

/* codegen_type_params */
static int qpv_type_params(qpv *c, asdl_type_param_seq *type_params)
{
    Py_ssize_t i;
    int seen_default = 0;

    for (i = 0; i < asdl_seq_LEN(type_params); i++) {
        type_param_ty tp = asdl_seq_GET(type_params, i);
        _Py_SourceLocation loc = QPV_LOC(tp);
        switch (tp->kind) {
        case TypeVar_kind:
            if (tp->v.TypeVar.bound &&
                qpv_type_param_bound_or_default(c, tp->v.TypeVar.bound, (void *)tp, 0) < 0)
                return -1;
            if (tp->v.TypeVar.default_value) {
                seen_default = 1;
                if (qpv_type_param_bound_or_default(c, tp->v.TypeVar.default_value,
                                                    (void *)((uintptr_t)tp + 1), 0) < 0)
                    return -1;
            } else if (seen_default) {
                return qpv_error_name(c, loc, QPV_NONDEFAULT_MSG_PREFIX, tp->v.TypeVar.name,
                                      QPV_NONDEFAULT_MSG_SUFFIX, 0);
            }
            break;
        case TypeVarTuple_kind:
            if (tp->v.TypeVarTuple.default_value) {
                if (qpv_type_param_bound_or_default(c, tp->v.TypeVarTuple.default_value,
                                                    (void *)tp, 1) < 0)
                    return -1;
                seen_default = 1;
            } else if (seen_default) {
                return qpv_error_name(c, loc, QPV_NONDEFAULT_MSG_PREFIX,
                                      tp->v.TypeVarTuple.name, QPV_NONDEFAULT_MSG_SUFFIX, 0);
            }
            break;
        case ParamSpec_kind:
            if (tp->v.ParamSpec.default_value) {
                if (qpv_type_param_bound_or_default(c, tp->v.ParamSpec.default_value,
                                                    (void *)tp, 0) < 0)
                    return -1;
                seen_default = 1;
            } else if (seen_default) {
                return qpv_error_name(c, loc, QPV_NONDEFAULT_MSG_PREFIX, tp->v.ParamSpec.name,
                                      QPV_NONDEFAULT_MSG_SUFFIX, 0);
            }
            break;
        }
    }
    return 0;
}

/* codegen_argannotation: under `from __future__ import annotations` the
   annotation is only stringified (codegen_visit_annexpr), never compiled. */
static int qpv_argannotation(qpv *c, expr_ty annotation)
{
    if (!annotation || (c->features & CO_FUTURE_ANNOTATIONS))
        return 0;
    if (annotation->kind == Starred_kind) {
        /* *args: *Ts */
        return qpv_visit_expr(c, annotation->v.Starred.value);
    }
    return qpv_visit_expr(c, annotation);
}

static int qpv_argannotations(qpv *c, asdl_arg_seq *args)
{
    Py_ssize_t i;
    for (i = 0; i < asdl_seq_LEN(args); i++) {
        arg_ty arg = asdl_seq_GET(args, i);
        if (qpv_argannotation(c, arg->annotation) < 0)
            return -1;
    }
    return 0;
}

/* codegen_function_annotations + codegen_annotations_in_scope: the
   __annotate__ function, compiled only when the annotation block (keyed by
   `args`) has annotations. */
static int qpv_function_annotations(qpv *c, arguments_ty args, expr_ty returns)
{
    PySTEntryObject *ste;
    qpv_unit u;
    int ret;

    _PySymtable_LookupOptional(c->st, args, &ste);
    if (!ste || !ste->ste_annotations_used)
        return 0;
    qpv_enter_scope_ste(c, &u, QPV_SCOPE_ANNOTATIONS, ste);
    ret = qpv_argannotations(c, args->posonlyargs);
    if (!ret)
        ret = qpv_argannotations(c, args->args);
    if (!ret && args->vararg)
        ret = qpv_argannotation(c, args->vararg->annotation);
    if (!ret)
        ret = qpv_argannotations(c, args->kwonlyargs);
    if (!ret && args->kwarg)
        ret = qpv_argannotation(c, args->kwarg->annotation);
    if (!ret)
        ret = qpv_argannotation(c, returns);
    qpv_exit_scope(c);
    return ret;
}

/* codegen_function_body + codegen_emit_function_body */
static int qpv_function_body(qpv *c, stmt_ty s, int is_async, asdl_stmt_seq *body)
{
    PySTEntryObject *ste;
    qpv_unit u;
    Py_ssize_t i;
    int ret = 0, add_stopiteration_handler;

    if (qpv_enter_scope(c, &u, is_async ? QPV_SCOPE_ASYNC_FUNCTION : QPV_SCOPE_FUNCTION, s) < 0)
        return -1;
    ste = u.ste;
    i = ste->ste_has_docstring ? 1 : 0;
    add_stopiteration_handler = ste->ste_coroutine || ste->ste_generator;
    if (add_stopiteration_handler)
        ret = qpv_push_fblock(c, NO_LOCATION, QPV_FB_STOP_ITERATION, NULL);
    for (; ret == 0 && i < asdl_seq_LEN(body); i++)
        ret = qpv_visit_stmt(c, asdl_seq_GET(body, i));
    qpv_exit_scope(c);
    return ret;
}

/* codegen_function: decorators, defaults, type parameters, annotations,
   body. */
static int qpv_function(qpv *c, stmt_ty s, int is_async)
{
    arguments_ty args;
    expr_ty returns;
    asdl_expr_seq *decos;
    asdl_type_param_seq *type_params;
    asdl_stmt_seq *body;
    qpv_unit tpu;
    int is_generic, ret;

    if (is_async) {
        args = s->v.AsyncFunctionDef.args;
        returns = s->v.AsyncFunctionDef.returns;
        decos = s->v.AsyncFunctionDef.decorator_list;
        type_params = s->v.AsyncFunctionDef.type_params;
        body = s->v.AsyncFunctionDef.body;
    } else {
        args = s->v.FunctionDef.args;
        returns = s->v.FunctionDef.returns;
        decos = s->v.FunctionDef.decorator_list;
        type_params = s->v.FunctionDef.type_params;
        body = s->v.FunctionDef.body;
    }
    if (qpv_visit_exprs(c, decos) < 0)
        return -1;
    if (qpv_default_arguments(c, args) < 0)
        return -1;
    is_generic = asdl_seq_LEN(type_params) > 0;
    if (is_generic) {
        if (qpv_enter_scope(c, &tpu, QPV_SCOPE_ANNOTATIONS, type_params) < 0)
            return -1;
        if (qpv_type_params(c, type_params) < 0) {
            qpv_exit_scope(c);
            return -1;
        }
    }
    ret = qpv_function_annotations(c, args, returns);
    if (ret == 0)
        ret = qpv_function_body(c, s, is_async, body);
    if (is_generic)
        qpv_exit_scope(c);
    return ret;
}

/* codegen_lambda */
static int qpv_lambda(qpv *c, expr_ty e)
{
    qpv_unit u;
    int ret;

    if (qpv_default_arguments(c, e->v.Lambda.args) < 0)
        return -1;
    if (qpv_enter_scope(c, &u, QPV_SCOPE_LAMBDA, e) < 0)
        return -1;
    ret = qpv_visit_expr(c, e->v.Lambda.body);
    qpv_exit_scope(c);
    return ret;
}

/* codegen_process_deferred_annotations: the module's/class's simple-name
   annotations, compiled into __annotate__ (annotation block keyed by
   ste_id + 1) after the body. */
static int qpv_process_deferred_annotations(qpv *c)
{
    qpv_unit *outer = c->u, u;
    Py_ssize_t i;
    int ret = 0;

    if (outer->ndeferred == 0)
        return 0;
    if (qpv_enter_scope(c, &u, QPV_SCOPE_ANNOTATIONS,
                        (void *)((uintptr_t)outer->ste->ste_id + 1)) < 0)
        return -1;
    for (i = 0; ret == 0 && i < outer->ndeferred; i++)
        ret = qpv_visit_expr(c, outer->deferred[i]->v.AnnAssign.annotation);
    qpv_exit_scope(c);
    return ret;
}

/* codegen_body: statements (after the docstring), then the deferred
   annotations unless `from __future__ import annotations`. */
static int qpv_body(qpv *c, asdl_stmt_seq *stmts, int is_interactive)
{
    Py_ssize_t i = 0;

    if (!asdl_seq_LEN(stmts))
        return 0;
    if (!is_interactive && c->u->ste->ste_has_docstring)
        i = 1;
    for (; i < asdl_seq_LEN(stmts); i++) {
        if (qpv_visit_stmt(c, asdl_seq_GET(stmts, i)) < 0)
            return -1;
    }
    if (!(c->features & CO_FUTURE_ANNOTATIONS))
        return qpv_process_deferred_annotations(c);
    return 0;
}

/* codegen_class + codegen_class_body: decorators, type parameters, the
   class body, then bases and keywords. */
static int qpv_class(qpv *c, stmt_ty s)
{
    asdl_type_param_seq *type_params = s->v.ClassDef.type_params;
    int is_generic = asdl_seq_LEN(type_params) > 0;
    qpv_unit tpu, u;
    int ret;

    if (qpv_visit_exprs(c, s->v.ClassDef.decorator_list) < 0)
        return -1;
    if (is_generic) {
        if (qpv_enter_scope(c, &tpu, QPV_SCOPE_ANNOTATIONS, type_params) < 0)
            return -1;
        if (qpv_type_params(c, type_params) < 0) {
            qpv_exit_scope(c);
            return -1;
        }
    }
    ret = qpv_enter_scope(c, &u, QPV_SCOPE_CLASS, s);
    if (ret == 0) {
        ret = qpv_body(c, s->v.ClassDef.body, 0);
        qpv_exit_scope(c);
    }
    if (ret == 0)
        ret = qpv_call_helper(c, s->v.ClassDef.bases, s->v.ClassDef.keywords);
    if (is_generic)
        qpv_exit_scope(c);
    return ret;
}

/* codegen_typealias + codegen_typealias_body */
static int qpv_typealias(qpv *c, stmt_ty s)
{
    asdl_type_param_seq *type_params = s->v.TypeAlias.type_params;
    int is_generic = asdl_seq_LEN(type_params) > 0;
    qpv_unit tpu, u;
    int ret;

    if (is_generic) {
        if (qpv_enter_scope(c, &tpu, QPV_SCOPE_ANNOTATIONS, type_params) < 0)
            return -1;
        if (qpv_type_params(c, type_params) < 0) {
            qpv_exit_scope(c);
            return -1;
        }
    }
    ret = qpv_enter_scope(c, &u, QPV_SCOPE_ANNOTATIONS, s);
    if (ret == 0) {
        ret = qpv_visit_expr(c, s->v.TypeAlias.value);
        qpv_exit_scope(c);
    }
    if (is_generic)
        qpv_exit_scope(c);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Control flow */

static int qpv_unwind_fblock_stack(qpv *c, _Py_SourceLocation *ploc,
                                   int preserve_tos, int *found_loop);

/* codegen_unwind_fblock: only a `finally` body (re-visited here) and the
   location updates matter. */
static int qpv_unwind_fblock(qpv *c, _Py_SourceLocation *ploc,
                             const qpv_fblock *info, int preserve_tos)
{
    switch (info->type) {
    case QPV_FB_FINALLY_TRY:
        if (preserve_tos &&
            qpv_push_fblock(c, *ploc, QPV_FB_POP_VALUE, NULL) < 0)
            return -1;
        /* emit the finally block */
        if (qpv_visit_stmts(c, info->finalbody) < 0)
            return -1;
        if (preserve_tos)
            qpv_pop_fblock(c);
        /* the unwinding instruction becomes artificial */
        *ploc = NO_LOCATION;
        return 0;
    case QPV_FB_WITH:
    case QPV_FB_ASYNC_WITH:
        /* the __exit__ call is emitted at info->loc, then the unwinding
           instruction becomes artificial */
        *ploc = NO_LOCATION;
        return 0;
    default:
        return 0;
    }
}

/* codegen_unwind_fblock_stack: unwind up to the innermost loop (when
   `found_loop` is given) or the whole stack. */
static int qpv_unwind_fblock_stack(qpv *c, _Py_SourceLocation *ploc,
                                   int preserve_tos, int *found_loop)
{
    qpv_unit *u = c->u;
    qpv_fblock *top, copy;

    if (u->nfblocks == 0)
        return 0;
    top = &u->fblock[u->nfblocks - 1];
    if (top->type == QPV_FB_EXCEPTION_GROUP_HANDLER)
        return qpv_error(c, *ploc,
                         "'break', 'continue' and 'return' cannot appear in an except* block");
    if (found_loop && (top->type == QPV_FB_WHILE_LOOP ||
                       top->type == QPV_FB_FOR_LOOP ||
                       top->type == QPV_FB_ASYNC_FOR_LOOP)) {
        *found_loop = 1;
        return 0;
    }
    copy = *top;
    qpv_pop_fblock(c);
    if (qpv_unwind_fblock(c, ploc, &copy, preserve_tos) < 0)
        return -1;
    if (qpv_unwind_fblock_stack(c, ploc, preserve_tos, found_loop) < 0)
        return -1;
    return qpv_push_fblock(c, copy.loc, copy.type, copy.finalbody);
}

/* codegen_return */
static int qpv_return(qpv *c, stmt_ty s)
{
    _Py_SourceLocation loc = QPV_LOC(s);
    expr_ty value = s->v.Return.value;
    PySTEntryObject *ste = c->u->ste;
    int preserve_tos = value != NULL && !qpv_is_folded_constant(c, value);

    if (!_PyST_IsFunctionLike(ste))
        return qpv_error(c, loc, "'return' outside function");
    if (value != NULL && ste->ste_coroutine && ste->ste_generator)
        return qpv_error(c, loc, "'return' with value in async generator");
    if (preserve_tos) {
        if (qpv_visit_expr(c, value) < 0)
            return -1;
    } else if (value != NULL) {
        loc = QPV_LOC(value);
    }
    if (value == NULL || value->lineno != s->lineno)
        loc = QPV_LOC(s);
    return qpv_unwind_fblock_stack(c, &loc, preserve_tos, NULL);
}

/* codegen_break / codegen_continue */
static int qpv_break_continue(qpv *c, stmt_ty s, int is_break)
{
    _Py_SourceLocation loc = QPV_LOC(s), origin_loc = loc;
    int found_loop = 0;

    if (qpv_unwind_fblock_stack(c, &loc, 0, &found_loop) < 0)
        return -1;
    if (!found_loop)
        return qpv_error(c, origin_loc, is_break ? "'break' outside loop"
                                                 : "'continue' not properly in loop");
    /* unwinding the loop block itself (break) emits no code that can fail */
    return 0;
}

/* codegen_for */
static int qpv_for(qpv *c, stmt_ty s)
{
    if (qpv_push_fblock(c, QPV_LOC(s), QPV_FB_FOR_LOOP, NULL) < 0)
        return -1;
    if (qpv_visit_expr(c, s->v.For.iter) < 0 ||
        qpv_visit_expr(c, s->v.For.target) < 0 ||
        qpv_visit_stmts(c, s->v.For.body) < 0)
        return -1;
    qpv_pop_fblock(c);
    return qpv_visit_stmts(c, s->v.For.orelse);
}

/* codegen_async_for */
static int qpv_async_for(qpv *c, stmt_ty s)
{
    if (qpv_visit_expr(c, s->v.AsyncFor.iter) < 0)
        return -1;
    if (qpv_push_fblock(c, QPV_LOC(s), QPV_FB_ASYNC_FOR_LOOP, NULL) < 0)
        return -1;
    if (qpv_visit_expr(c, s->v.AsyncFor.target) < 0 ||
        qpv_visit_stmts(c, s->v.AsyncFor.body) < 0)
        return -1;
    qpv_pop_fblock(c);
    return qpv_visit_stmts(c, s->v.AsyncFor.orelse);
}

/* codegen_while */
static int qpv_while(qpv *c, stmt_ty s)
{
    if (qpv_push_fblock(c, QPV_LOC(s), QPV_FB_WHILE_LOOP, NULL) < 0)
        return -1;
    if (qpv_visit_expr(c, s->v.While.test) < 0 ||
        qpv_visit_stmts(c, s->v.While.body) < 0)
        return -1;
    qpv_pop_fblock(c);
    return qpv_visit_stmts(c, s->v.While.orelse);
}

/* codegen_try_except */
static int qpv_try_except(qpv *c, stmt_ty s)
{
    _Py_SourceLocation loc = QPV_LOC(s);
    asdl_excepthandler_seq *handlers = s->v.Try.handlers;
    Py_ssize_t i, n = asdl_seq_LEN(handlers);

    if (qpv_push_fblock(c, loc, QPV_FB_TRY_EXCEPT, NULL) < 0)
        return -1;
    if (qpv_visit_stmts(c, s->v.Try.body) < 0)
        return -1;
    qpv_pop_fblock(c);
    if (qpv_visit_stmts(c, s->v.Try.orelse) < 0)
        return -1;
    /* the runtime pushes a block for the handlers */
    if (qpv_push_fblock(c, loc, QPV_FB_EXCEPTION_HANDLER, NULL) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        excepthandler_ty handler = asdl_seq_GET(handlers, i);
        _Py_SourceLocation hloc = QPV_LOC(handler);
        if (!handler->v.ExceptHandler.type && i < n - 1)
            return qpv_error(c, hloc, "default 'except:' must be last");
        if (handler->v.ExceptHandler.type &&
            qpv_visit_expr(c, handler->v.ExceptHandler.type) < 0)
            return -1;
        if (qpv_push_fblock(c, hloc, QPV_FB_HANDLER_CLEANUP, NULL) < 0)
            return -1;
        if (qpv_visit_stmts(c, handler->v.ExceptHandler.body) < 0)
            return -1;
        qpv_pop_fblock(c);
    }
    qpv_pop_fblock(c);
    return 0;
}

/* codegen_try_star_except: the else-block comes after the handlers. */
static int qpv_try_star_except(qpv *c, stmt_ty s)
{
    _Py_SourceLocation loc = QPV_LOC(s);
    asdl_excepthandler_seq *handlers = s->v.TryStar.handlers;
    Py_ssize_t i, n = asdl_seq_LEN(handlers);

    if (qpv_push_fblock(c, loc, QPV_FB_TRY_EXCEPT, NULL) < 0)
        return -1;
    if (qpv_visit_stmts(c, s->v.TryStar.body) < 0)
        return -1;
    qpv_pop_fblock(c);
    if (qpv_push_fblock(c, loc, QPV_FB_EXCEPTION_GROUP_HANDLER, NULL) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        excepthandler_ty handler = asdl_seq_GET(handlers, i);
        if (handler->v.ExceptHandler.type &&
            qpv_visit_expr(c, handler->v.ExceptHandler.type) < 0)
            return -1;
        if (qpv_push_fblock(c, QPV_LOC(handler), QPV_FB_HANDLER_CLEANUP, NULL) < 0)
            return -1;
        if (qpv_visit_stmts(c, handler->v.ExceptHandler.body) < 0)
            return -1;
        qpv_pop_fblock(c);
    }
    qpv_pop_fblock(c);
    return qpv_visit_stmts(c, s->v.TryStar.orelse);
}

/* codegen_try_finally / codegen_try_star_finally: the finally body is
   compiled twice (normal and exceptional exit), plus once per unwinding
   statement inside the protected part. */
static int qpv_try_finally(qpv *c, stmt_ty s, int is_star)
{
    asdl_stmt_seq *finalbody = is_star ? s->v.TryStar.finalbody : s->v.Try.finalbody;
    asdl_excepthandler_seq *handlers = is_star ? s->v.TryStar.handlers : s->v.Try.handlers;
    int ret;

    if (qpv_push_fblock(c, QPV_LOC(s), QPV_FB_FINALLY_TRY, finalbody) < 0)
        return -1;
    if (asdl_seq_LEN(handlers))
        ret = is_star ? qpv_try_star_except(c, s) : qpv_try_except(c, s);
    else
        ret = qpv_visit_stmts(c, is_star ? s->v.TryStar.body : s->v.Try.body);
    if (ret < 0)
        return -1;
    qpv_pop_fblock(c);
    if (qpv_visit_stmts(c, finalbody) < 0)
        return -1;
    if (qpv_push_fblock(c, NO_LOCATION, QPV_FB_FINALLY_END, NULL) < 0)
        return -1;
    if (qpv_visit_stmts(c, finalbody) < 0)
        return -1;
    qpv_pop_fblock(c);
    return 0;
}

/* codegen_with_inner / codegen_async_with_inner */
static int qpv_with(qpv *c, stmt_ty s, int pos, int is_async)
{
    asdl_withitem_seq *items = is_async ? s->v.AsyncWith.items : s->v.With.items;
    withitem_ty item = asdl_seq_GET(items, pos);
    _Py_SourceLocation loc;

    if (qpv_visit_expr(c, item->context_expr) < 0)
        return -1;
    loc = QPV_LOC(item->context_expr);
    if (qpv_push_fblock(c, loc, is_async ? QPV_FB_ASYNC_WITH : QPV_FB_WITH, NULL) < 0)
        return -1;
    if (item->optional_vars && qpv_visit_expr(c, item->optional_vars) < 0)
        return -1;
    pos++;
    if (pos == asdl_seq_LEN(items)) {
        if (qpv_visit_stmts(c, is_async ? s->v.AsyncWith.body : s->v.With.body) < 0)
            return -1;
    } else {
        if (qpv_with(c, s, pos, is_async) < 0)
            return -1;
    }
    qpv_pop_fblock(c);
    return 0;
}

/* codegen_validate_lazy_import */
static int qpv_validate_lazy_import(qpv *c, _Py_SourceLocation loc)
{
    if (c->u->scope_type != QPV_SCOPE_MODULE)
        return qpv_error(c, loc, "lazy imports only allowed in module scope");
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Assignments */

/* codegen_augassign */
static int qpv_augassign(qpv *c, stmt_ty s)
{
    expr_ty e = s->v.AugAssign.target;

    switch (e->kind) {
    case Attribute_kind:
        if (qpv_visit_expr(c, e->v.Attribute.value) < 0)
            return -1;
        break;
    case Subscript_kind:
        if (qpv_visit_expr(c, e->v.Subscript.value) < 0 ||
            qpv_visit_expr(c, e->v.Subscript.slice) < 0)
            return -1;
        break;
    default:
        break;
    }
    return qpv_visit_expr(c, s->v.AugAssign.value);
}

/* codegen_check_ann_subscr */
static int qpv_check_ann_subscr(qpv *c, expr_ty e)
{
    Py_ssize_t i;

    switch (e->kind) {
    case Slice_kind:
        if (e->v.Slice.lower && qpv_visit_expr(c, e->v.Slice.lower) < 0)
            return -1;
        if (e->v.Slice.upper && qpv_visit_expr(c, e->v.Slice.upper) < 0)
            return -1;
        if (e->v.Slice.step && qpv_visit_expr(c, e->v.Slice.step) < 0)
            return -1;
        return 0;
    case Tuple_kind:
        for (i = 0; i < asdl_seq_LEN(e->v.Tuple.elts); i++) {
            if (qpv_check_ann_subscr(c, asdl_seq_GET(e->v.Tuple.elts, i)) < 0)
                return -1;
        }
        return 0;
    default:
        return qpv_visit_expr(c, e);
    }
}

/* codegen_annassign: the annotation itself is only compiled for simple
   names in module/class scope (deferred into __annotate__). */
static int qpv_annassign(qpv *c, stmt_ty s)
{
    expr_ty targ = s->v.AnnAssign.target;

    if (s->v.AnnAssign.value) {
        if (qpv_visit_expr(c, s->v.AnnAssign.value) < 0 ||
            qpv_visit_expr(c, targ) < 0)
            return -1;
    }
    switch (targ->kind) {
    case Name_kind:
        if (s->v.AnnAssign.simple &&
            (c->u->scope_type == QPV_SCOPE_MODULE || c->u->scope_type == QPV_SCOPE_CLASS) &&
            !(c->features & CO_FUTURE_ANNOTATIONS))
            return qpv_add_deferred_annotation(c, s);
        return 0;
    case Attribute_kind:
        if (!s->v.AnnAssign.value)
            return qpv_visit_expr(c, targ->v.Attribute.value);
        return 0;
    case Subscript_kind:
        if (!s->v.AnnAssign.value) {
            if (qpv_visit_expr(c, targ->v.Subscript.value) < 0)
                return -1;
            return qpv_check_ann_subscr(c, targ->v.Subscript.slice);
        }
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Pattern matching (PEP 634) */

#define QPV_WILDCARD_CHECK(N) ((N)->kind == MatchAs_kind && !(N)->v.MatchAs.name)
#define QPV_WILDCARD_STAR_CHECK(N) ((N)->kind == MatchStar_kind && !(N)->v.MatchStar.name)

static int qpv_pc_contains(const qpv_pc *pc, JSAtom name)
{
    Py_ssize_t i;
    for (i = 0; i < pc->nstores; i++) {
        if (pc->stores[i] == name)
            return 1;
    }
    return 0;
}

static int qpv_pc_append(qpv *c, qpv_pc *pc, JSAtom name)
{
    if (pc->nstores == pc->cap) {
        Py_ssize_t cap = pc->cap ? pc->cap * 2 : 8;
        JSAtom *s = js_realloc_rt(c->ctx->rt, pc->stores, cap * sizeof(*s));
        if (!s)
            return qpv_no_memory(c);
        pc->stores = s;
        pc->cap = cap;
    }
    pc->stores[pc->nstores++] = name;
    return 0;
}

/* codegen_error_duplicate_store */
static int qpv_error_duplicate_store(qpv *c, _Py_SourceLocation loc, JSAtom name)
{
    return qpv_error_name(c, loc, "multiple assignments to name ", name, " in pattern", 1);
}

/* codegen_pattern_helper_store_name */
static int qpv_pattern_store_name(qpv *c, _Py_SourceLocation loc, JSAtom name, qpv_pc *pc)
{
    if (name == JS_ATOM_NULL)
        return 0;
    if (qpv_pc_contains(pc, name))
        return qpv_error_duplicate_store(c, loc, name);
    return qpv_pc_append(c, pc, name);
}

/* codegen_pattern_subpattern: irrefutable sub-patterns are fine. */
static int qpv_pattern_subpattern(qpv *c, pattern_ty p, qpv_pc *pc)
{
    int allow_irrefutable = pc->allow_irrefutable;
    pc->allow_irrefutable = 1;
    if (qpv_pattern(c, p, pc) < 0)
        return -1;
    pc->allow_irrefutable = allow_irrefutable;
    return 0;
}

/* codegen_pattern_as */
static int qpv_pattern_as(qpv *c, pattern_ty p, qpv_pc *pc)
{
    if (p->v.MatchAs.pattern == NULL) {
        if (!pc->allow_irrefutable) {
            if (p->v.MatchAs.name)
                return qpv_error_name(c, QPV_LOC(p), "name capture ", p->v.MatchAs.name,
                                      " makes remaining patterns unreachable", 1);
            return qpv_error(c, QPV_LOC(p), "wildcard makes remaining patterns unreachable");
        }
        return qpv_pattern_store_name(c, QPV_LOC(p), p->v.MatchAs.name, pc);
    }
    if (qpv_pattern(c, p->v.MatchAs.pattern, pc) < 0)
        return -1;
    return qpv_pattern_store_name(c, QPV_LOC(p), p->v.MatchAs.name, pc);
}

/* codegen_pattern_class + validate_kwd_attrs */
static int qpv_pattern_class(qpv *c, pattern_ty p, qpv_pc *pc)
{
    asdl_pattern_seq *patterns = p->v.MatchClass.patterns;
    asdl_identifier_seq *kwd_attrs = p->v.MatchClass.kwd_attrs;
    asdl_pattern_seq *kwd_patterns = p->v.MatchClass.kwd_patterns;
    Py_ssize_t nargs = asdl_seq_LEN(patterns);
    Py_ssize_t nattrs = asdl_seq_LEN(kwd_attrs);
    Py_ssize_t nkwd_patterns = asdl_seq_LEN(kwd_patterns);
    Py_ssize_t i, j;

    if (nattrs != nkwd_patterns) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "kwd_attrs (%d) / kwd_patterns (%d) length mismatch in class pattern",
                 (int)nattrs, (int)nkwd_patterns);
        return qpv_error(c, QPV_LOC(p), msg);
    }
    /* "too many sub-patterns in class pattern %R" needs > INT_MAX sub-patterns */
    for (i = 0; i < nattrs; i++) {
        JSAtom attr = asdl_seq_GET(kwd_attrs, i);
        for (j = i + 1; j < nattrs; j++) {
            if (asdl_seq_GET(kwd_attrs, j) == attr)
                return qpv_error_name(c, QPV_LOC(asdl_seq_GET(kwd_patterns, j)),
                                      "attribute name repeated in class pattern: ",
                                      attr, "", 0);
        }
    }
    if (qpv_visit_expr(c, p->v.MatchClass.cls) < 0)
        return -1;
    for (i = 0; i < nargs + nattrs; i++) {
        pattern_ty sub = i < nargs ? asdl_seq_GET(patterns, i)
                                   : asdl_seq_GET(kwd_patterns, i - nargs);
        if (QPV_WILDCARD_CHECK(sub))
            continue;
        if (qpv_pattern_subpattern(c, sub, pc) < 0)
            return -1;
    }
    return 0;
}

/* codegen_pattern_mapping + codegen_pattern_mapping_key */
static int qpv_pattern_mapping(qpv *c, pattern_ty p, qpv_pc *pc)
{
    asdl_expr_seq *keys = p->v.MatchMapping.keys;
    asdl_pattern_seq *patterns = p->v.MatchMapping.patterns;
    Py_ssize_t size = asdl_seq_LEN(keys);
    Py_ssize_t npatterns = asdl_seq_LEN(patterns);
    Py_ssize_t i, j, nseen = 0;
    qpv_val *seen = NULL, cur;
    int ret = 0;

    if (size != npatterns) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "keys (%d) / patterns (%d) length mismatch in mapping pattern",
                 (int)size, (int)npatterns);
        return qpv_error(c, QPV_LOC(p), msg);
    }
    if (!size && !p->v.MatchMapping.rest)
        return 0;
    /* "too many sub-patterns in mapping pattern" needs > INT_MAX keys */
    if (size) {
        seen = js_malloc_rt(c->ctx->rt, size * sizeof(*seen));
        if (!seen)
            return qpv_no_memory(c);
    }
    for (i = 0; ret == 0 && i < size; i++) {
        expr_ty key = asdl_seq_GET(keys, i);
        if (key == NULL) {
            ret = qpv_error(c, QPV_LOC(asdl_seq_GET(patterns, i)),
                            "can't use NULL keys in MatchMapping "
                            "(set 'rest' parameter instead)");
            break;
        }
        if (qpv_fold_match_const(c, key, &cur)) {
            for (j = 0; j < nseen; j++) {
                int eq = qpv_val_eq(c, &seen[j], &cur);
                if (eq < 0) {
                    ret = -1;
                    break;
                }
                if (eq) {
                    DynBuf b;
                    dbuf_init(&b);
                    dbuf_putstr(&b, "mapping pattern checks duplicate key (");
                    if (qpv_put_val_repr(c, &b, &cur) < 0) {
                        dbuf_free(&b);
                        ret = -1;
                        break;
                    }
                    dbuf_putc(&b, ')');
                    ret = qpv_error_buf(c, QPV_LOC(p), &b);
                    break;
                }
            }
            if (ret < 0) {
                qpv_val_free(c, &cur);
                break;
            }
            seen[nseen++] = cur;
        } else if (key->kind != Attribute_kind) {
            ret = qpv_error(c, QPV_LOC(p),
                            "mapping pattern keys may only match literals and attribute lookups");
            break;
        }
        ret = qpv_visit_expr(c, key);
    }
    for (j = 0; j < nseen; j++)
        qpv_val_free(c, &seen[j]);
    js_free_rt(c->ctx->rt, seen);
    if (ret < 0)
        return -1;
    for (i = 0; i < size; i++) {
        if (qpv_pattern_subpattern(c, asdl_seq_GET(patterns, i), pc) < 0)
            return -1;
    }
    if (p->v.MatchMapping.rest)
        return qpv_pattern_store_name(c, QPV_LOC(p), p->v.MatchMapping.rest, pc);
    return 0;
}

/* codegen_pattern_or: every alternative must bind the same names; they
   are then added to the enclosing captures. */
static int qpv_pattern_or(qpv *c, pattern_ty p, qpv_pc *pc)
{
    asdl_pattern_seq *alts = p->v.MatchOr.patterns;
    Py_ssize_t size = asdl_seq_LEN(alts), i, k;
    qpv_pc alt = { NULL, 0, 0, 0 }, control = { NULL, 0, 0, 0 };
    int ret = -1;

    for (i = 0; i < size; i++) {
        alt.nstores = 0;
        /* an irrefutable sub-pattern must be last, if it is allowed at all */
        alt.allow_irrefutable = (i == size - 1) && pc->allow_irrefutable;
        if (qpv_pattern(c, asdl_seq_GET(alts, i), &alt) < 0)
            goto done;
        if (i == 0) {
            /* the first alternative's captures are the control */
            control = alt;
            alt.stores = NULL;
            alt.nstores = alt.cap = 0;
        } else if (alt.nstores != control.nstores) {
            goto diff;
        } else {
            for (k = 0; k < control.nstores; k++) {
                if (!qpv_pc_contains(&alt, control.stores[k]))
                    goto diff;
            }
        }
    }
    for (k = 0; k < control.nstores; k++) {
        JSAtom name = control.stores[k];
        if (qpv_pc_contains(pc, name)) {
            qpv_error_duplicate_store(c, QPV_LOC(p), name);
            goto done;
        }
        if (qpv_pc_append(c, pc, name) < 0)
            goto done;
    }
    ret = 0;
    goto done;
 diff:
    qpv_error(c, QPV_LOC(p), "alternative patterns bind different names");
 done:
    js_free_rt(c->ctx->rt, alt.stores);
    js_free_rt(c->ctx->rt, control.stores);
    return ret;
}

/* codegen_pattern_sequence (+ pattern_helper_sequence_subscr /
   pattern_helper_sequence_unpack / codegen_pattern_unpack_helper) */
static int qpv_pattern_sequence(qpv *c, pattern_ty p, qpv_pc *pc)
{
    asdl_pattern_seq *patterns = p->v.MatchSequence.patterns;
    Py_ssize_t size = asdl_seq_LEN(patterns), i, star = -1;
    int only_wildcard = 1, star_wildcard = 0, seen_star;

    for (i = 0; i < size; i++) {
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        if (pattern->kind == MatchStar_kind) {
            if (star >= 0)
                return qpv_error(c, QPV_LOC(p), "multiple starred names in sequence pattern");
            star_wildcard = QPV_WILDCARD_STAR_CHECK(pattern);
            only_wildcard &= star_wildcard;
            star = i;
            continue;
        }
        only_wildcard &= QPV_WILDCARD_CHECK(pattern);
    }
    if (only_wildcard)
        return 0;
    if (star_wildcard) {
        /* subscript each non-wildcard element */
        for (i = 0; i < size; i++) {
            pattern_ty pattern = asdl_seq_GET(patterns, i);
            if (QPV_WILDCARD_CHECK(pattern) || i == star)
                continue;
            if (qpv_pattern_subpattern(c, pattern, pc) < 0)
                return -1;
        }
        return 0;
    }
    /* UNPACK_SEQUENCE / UNPACK_EX */
    seen_star = 0;
    for (i = 0; i < size; i++) {
        pattern_ty elt = asdl_seq_GET(patterns, i);
        if (elt->kind == MatchStar_kind && !seen_star) {
            if ((i >= (1 << 8)) || (size - i - 1 >= (INT_MAX >> 8)))
                return qpv_error(c, QPV_LOC(p),
                                 "too many expressions in star-unpacking sequence pattern");
            seen_star = 1;
        } else if (elt->kind == MatchStar_kind) {
            return qpv_error(c, QPV_LOC(p), "multiple starred expressions in sequence pattern");
        }
    }
    for (i = 0; i < size; i++) {
        if (qpv_pattern_subpattern(c, asdl_seq_GET(patterns, i), pc) < 0)
            return -1;
    }
    return 0;
}

/* codegen_pattern_value: after folding, only constants and dotted names. */
static int qpv_pattern_value(qpv *c, pattern_ty p)
{
    expr_ty value = p->v.MatchValue.value;
    qpv_val v;

    if (!qpv_fold_match_const(c, value, &v) && value->kind != Attribute_kind)
        return qpv_error(c, QPV_LOC(p), "patterns may only match literals and attribute lookups");
    return qpv_visit_expr(c, value);
}

/* codegen_pattern */
static int qpv_pattern(qpv *c, pattern_ty p, qpv_pc *pc)
{
    if (qpv_check_recursion(c) < 0)
        return -1;
    switch (p->kind) {
    case MatchValue_kind:
        return qpv_pattern_value(c, p);
    case MatchSingleton_kind:
        return 0;
    case MatchSequence_kind:
        return qpv_pattern_sequence(c, p, pc);
    case MatchMapping_kind:
        return qpv_pattern_mapping(c, p, pc);
    case MatchClass_kind:
        return qpv_pattern_class(c, p, pc);
    case MatchStar_kind:
        return qpv_pattern_store_name(c, QPV_LOC(p), p->v.MatchStar.name, pc);
    case MatchAs_kind:
        return qpv_pattern_as(c, p, pc);
    case MatchOr_kind:
        return qpv_pattern_or(c, p, pc);
    }
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "invalid match pattern node in AST (kind=%d)", (int)p->kind);
        return qpv_error(c, QPV_LOC(p), msg);
    }
}

/* codegen_match_inner: a trailing `case _:` (with other cases before it)
   is not compiled as a pattern. */
static int qpv_match(qpv *c, stmt_ty s)
{
    asdl_match_case_seq *cases_seq = s->v.Match.cases;
    Py_ssize_t cases = asdl_seq_LEN(cases_seq), i;
    match_case_ty m;
    int has_default, ret;

    if (qpv_visit_expr(c, s->v.Match.subject) < 0)
        return -1;
    m = asdl_seq_GET(cases_seq, cases - 1);
    has_default = QPV_WILDCARD_CHECK(m->pattern) && 1 < cases;
    for (i = 0; i < cases - has_default; i++) {
        qpv_pc pc = { NULL, 0, 0, 0 };
        m = asdl_seq_GET(cases_seq, i);
        /* irrefutable cases must be either guarded, last, or both */
        pc.allow_irrefutable = m->guard != NULL || i == cases - 1;
        ret = qpv_pattern(c, m->pattern, &pc);
        js_free_rt(c->ctx->rt, pc.stores);
        if (ret < 0)
            return -1;
        if (m->guard && qpv_visit_expr(c, m->guard) < 0)
            return -1;
        if (qpv_visit_stmts(c, m->body) < 0)
            return -1;
    }
    if (has_default) {
        m = asdl_seq_GET(cases_seq, cases - 1);
        if (m->guard && qpv_visit_expr(c, m->guard) < 0)
            return -1;
        if (qpv_visit_stmts(c, m->body) < 0)
            return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Statements and expressions */

/* codegen_visit_stmt */
static int qpv_visit_stmt(qpv *c, stmt_ty s)
{
    Py_ssize_t i;

    if (qpv_check_recursion(c) < 0)
        return -1;
    switch (s->kind) {
    case FunctionDef_kind:
        return qpv_function(c, s, 0);
    case AsyncFunctionDef_kind:
        return qpv_function(c, s, 1);
    case ClassDef_kind:
        return qpv_class(c, s);
    case TypeAlias_kind:
        return qpv_typealias(c, s);
    case Return_kind:
        return qpv_return(c, s);
    case Delete_kind:
        return qpv_visit_exprs(c, s->v.Delete.targets);
    case Assign_kind:
        if (qpv_visit_expr(c, s->v.Assign.value) < 0)
            return -1;
        return qpv_visit_exprs(c, s->v.Assign.targets);
    case AugAssign_kind:
        return qpv_augassign(c, s);
    case AnnAssign_kind:
        return qpv_annassign(c, s);
    case For_kind:
        return qpv_for(c, s);
    case AsyncFor_kind:
        return qpv_async_for(c, s);
    case While_kind:
        return qpv_while(c, s);
    case If_kind:
        if (qpv_visit_expr(c, s->v.If.test) < 0 ||
            qpv_visit_stmts(c, s->v.If.body) < 0)
            return -1;
        return qpv_visit_stmts(c, s->v.If.orelse);
    case Match_kind:
        return qpv_match(c, s);
    case Raise_kind:
        if (s->v.Raise.exc) {
            if (qpv_visit_expr(c, s->v.Raise.exc) < 0)
                return -1;
            if (s->v.Raise.cause && qpv_visit_expr(c, s->v.Raise.cause) < 0)
                return -1;
        }
        return 0;
    case Try_kind:
        if (asdl_seq_LEN(s->v.Try.finalbody))
            return qpv_try_finally(c, s, 0);
        return qpv_try_except(c, s);
    case TryStar_kind:
        if (asdl_seq_LEN(s->v.TryStar.finalbody))
            return qpv_try_finally(c, s, 1);
        return qpv_try_star_except(c, s);
    case Assert_kind: {
        /* codegen_assert at optimization level 0.  Always emit a warning
           if the test is a non-zero length tuple. */
        expr_ty test = s->v.Assert.test;
        if ((test->kind == Tuple_kind &&
             asdl_seq_LEN(test->v.Tuple.elts) > 0) ||
            (test->kind == Constant_kind &&
             test->v.Constant.value->kind == QP_CONST_OBJECT &&
             qp_tuple_check(c->ctx, test->v.Constant.value->v) &&
             QP_TUPLE_SIZE(test->v.Constant.value->v) > 0)) {
            if (qpv_warn(c, QPV_LOC(s), "assertion is always true, "
                                        "perhaps remove parentheses?") < 0)
                return -1;
        }
        if (qpv_visit_expr(c, s->v.Assert.test) < 0)
            return -1;
        if (s->v.Assert.msg)
            return qpv_visit_expr(c, s->v.Assert.msg);
        return 0;
    }
    case Import_kind:
        /* codegen_import: checked once per alias */
        for (i = 0; i < asdl_seq_LEN(s->v.Import.names); i++) {
            if (s->v.Import.is_lazy && qpv_validate_lazy_import(c, QPV_LOC(s)) < 0)
                return -1;
        }
        return 0;
    case ImportFrom_kind:
        /* codegen_from_import */
        if (s->v.ImportFrom.is_lazy) {
            alias_ty alias = asdl_seq_GET(s->v.ImportFrom.names, 0);
            if (alias->name == c->atom_star)
                return qpv_error(c, QPV_LOC(s), "cannot lazy import *");
            return qpv_validate_lazy_import(c, QPV_LOC(s));
        }
        return 0;
    case Global_kind:
    case Nonlocal_kind:
    case Pass_kind:
        return 0;
    case Expr_kind:
        /* codegen_stmt_expr: a constant statement compiles to a NOP */
        return qpv_visit_expr(c, s->v.Expr.value);
    case Break_kind:
        return qpv_break_continue(c, s, 1);
    case Continue_kind:
        return qpv_break_continue(c, s, 0);
    case With_kind:
        return qpv_with(c, s, 0, 0);
    case AsyncWith_kind:
        return qpv_with(c, s, 0, 1);
    }
    return 0;
}

/* codegen_visit_expr.  codegen_jump_if (if/while/assert/comprehension
   conditions, match guards) visits the same sub-expressions in the same
   order, so conditions are visited as plain expressions. */
static int qpv_visit_expr(qpv *c, expr_ty e)
{
    _Py_SourceLocation loc;
    Py_ssize_t i;

    if (qpv_check_recursion(c) < 0)
        return -1;
    loc = QPV_LOC(e);
    switch (e->kind) {
    case NamedExpr_kind:
        if (qpv_visit_expr(c, e->v.NamedExpr.value) < 0)
            return -1;
        return qpv_visit_expr(c, e->v.NamedExpr.target);
    case BoolOp_kind:
        return qpv_visit_exprs(c, e->v.BoolOp.values);
    case BinOp_kind:
        if (qpv_visit_expr(c, e->v.BinOp.left) < 0)
            return -1;
        return qpv_visit_expr(c, e->v.BinOp.right);
    case UnaryOp_kind:
        return qpv_visit_expr(c, e->v.UnaryOp.operand);
    case Lambda_kind:
        return qpv_lambda(c, e);
    case IfExp_kind:
        if (qpv_visit_expr(c, e->v.IfExp.test) < 0 ||
            qpv_visit_expr(c, e->v.IfExp.body) < 0)
            return -1;
        return qpv_visit_expr(c, e->v.IfExp.orelse);
    case Dict_kind:
        /* codegen_dict / codegen_subdict keep key/value source order */
        for (i = 0; i < asdl_seq_LEN(e->v.Dict.values); i++) {
            expr_ty key = asdl_seq_GET(e->v.Dict.keys, i);
            if (key && qpv_visit_expr(c, key) < 0)
                return -1;
            if (qpv_visit_expr(c, asdl_seq_GET(e->v.Dict.values, i)) < 0)
                return -1;
        }
        return 0;
    case Set_kind:
        return qpv_starunpack(c, e->v.Set.elts);
    case GeneratorExp_kind:
        return qpv_comprehension(c, e, QPV_COMP_GENEXP, e->v.GeneratorExp.generators,
                                 e->v.GeneratorExp.elt, NULL);
    case ListComp_kind:
        return qpv_comprehension(c, e, QPV_COMP_LISTCOMP, e->v.ListComp.generators,
                                 e->v.ListComp.elt, NULL);
    case SetComp_kind:
        return qpv_comprehension(c, e, QPV_COMP_SETCOMP, e->v.SetComp.generators,
                                 e->v.SetComp.elt, NULL);
    case DictComp_kind:
        return qpv_comprehension(c, e, QPV_COMP_DICTCOMP, e->v.DictComp.generators,
                                 e->v.DictComp.key, e->v.DictComp.value);
    case Yield_kind:
        if (!_PyST_IsFunctionLike(c->u->ste))
            return qpv_error(c, loc, "'yield' outside function");
        if (e->v.Yield.value)
            return qpv_visit_expr(c, e->v.Yield.value);
        return 0;
    case YieldFrom_kind:
        if (!_PyST_IsFunctionLike(c->u->ste))
            return qpv_error(c, loc, "'yield from' outside function");
        if (c->u->scope_type == QPV_SCOPE_ASYNC_FUNCTION)
            return qpv_error(c, loc, "'yield from' inside async function");
        return qpv_visit_expr(c, e->v.YieldFrom.value);
    case Await_kind:
        return qpv_visit_expr(c, e->v.Await.value);
    case Compare_kind:
        /* codegen_compare / codegen_jump_if */
        if (qpv_check_compare(c, e) < 0)
            return -1;
        if (qpv_visit_expr(c, e->v.Compare.left) < 0)
            return -1;
        return qpv_visit_exprs(c, e->v.Compare.comparators);
    case Call_kind:
        return qpv_call(c, e);
    case Constant_kind:
        return 0;
    case JoinedStr_kind:
        return qpv_visit_exprs(c, e->v.JoinedStr.values);
    case TemplateStr_kind:
        /* codegen_template_str: the strings first, then the interpolations */
        for (i = 0; i < asdl_seq_LEN(e->v.TemplateStr.values); i++) {
            expr_ty value = asdl_seq_GET(e->v.TemplateStr.values, i);
            if (value->kind != Interpolation_kind && qpv_visit_expr(c, value) < 0)
                return -1;
        }
        for (i = 0; i < asdl_seq_LEN(e->v.TemplateStr.values); i++) {
            expr_ty value = asdl_seq_GET(e->v.TemplateStr.values, i);
            if (value->kind == Interpolation_kind && qpv_visit_expr(c, value) < 0)
                return -1;
        }
        return 0;
    case FormattedValue_kind:
        if (qpv_visit_expr(c, e->v.FormattedValue.value) < 0)
            return -1;
        if (e->v.FormattedValue.format_spec)
            return qpv_visit_expr(c, e->v.FormattedValue.format_spec);
        return 0;
    case Interpolation_kind:
        if (qpv_visit_expr(c, e->v.Interpolation.value) < 0)
            return -1;
        if (e->v.Interpolation.format_spec)
            return qpv_visit_expr(c, e->v.Interpolation.format_spec);
        return 0;
    case Attribute_kind:
        /* the zero/two-argument super() fast path visits the same
           arguments as compiling the super() call */
        return qpv_visit_expr(c, e->v.Attribute.value);
    case Subscript_kind:
        /* codegen_subscript */
        if (e->v.Subscript.ctx == Load) {
            if (qpv_check_subscripter(c, e->v.Subscript.value) < 0 ||
                qpv_check_index(c, e->v.Subscript.value, e->v.Subscript.slice) < 0)
                return -1;
        }
        if (qpv_visit_expr(c, e->v.Subscript.value) < 0)
            return -1;
        return qpv_visit_expr(c, e->v.Subscript.slice);
    case Starred_kind:
        /* every legitimate Starred is consumed by its List/Tuple/Set/call/
           comprehension parent */
        if (e->v.Starred.ctx == Store)
            return qpv_error(c, loc, "starred assignment target must be in a list or tuple");
        return qpv_error(c, loc, "can't use starred expression here");
    case Slice_kind:
        /* codegen_slice: an all-constant slice is a single constant */
        if (e->v.Slice.lower && qpv_visit_expr(c, e->v.Slice.lower) < 0)
            return -1;
        if (e->v.Slice.upper && qpv_visit_expr(c, e->v.Slice.upper) < 0)
            return -1;
        if (e->v.Slice.step)
            return qpv_visit_expr(c, e->v.Slice.step);
        return 0;
    case Name_kind:
        return 0;
    case List_kind:
        if (e->v.List.ctx == Store)
            return qpv_assignment_helper(c, loc, e->v.List.elts);
        if (e->v.List.ctx == Load)
            return qpv_starunpack(c, e->v.List.elts);
        return qpv_visit_exprs(c, e->v.List.elts);
    case Tuple_kind:
        if (e->v.Tuple.ctx == Store)
            return qpv_assignment_helper(c, loc, e->v.Tuple.elts);
        if (e->v.Tuple.ctx == Load)
            return qpv_starunpack(c, e->v.Tuple.elts);
        return qpv_visit_exprs(c, e->v.Tuple.elts);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Entry point (compiler_codegen) */

static int qp_validate(struct symtable *st, mod_ty mod, int flags, const char *filename,
                       JSValueConst module, qp_error *err)
{
    qpv c;
    qpv_unit u;
    int ret;

    c.st = st;
    c.ctx = st->st_ctx;
    c.err = err;
    c.features = flags;
    c.filename = filename;
    c.module = module;
    c.disable_warning = 0;
    c.u = NULL;
    c.atom_debug = JS_NewAtom(c.ctx, "__debug__");
    c.atom_star = JS_NewAtom(c.ctx, "*");
    if (c.atom_debug == JS_ATOM_NULL || c.atom_star == JS_ATOM_NULL) {
        ret = qpv_js_failure(&c);
        goto done;
    }
    /* _PyCodegen_EnterAnonymousScope */
    ret = qpv_enter_scope(&c, &u, QPV_SCOPE_MODULE, mod);
    if (ret < 0)
        goto done;
    switch (mod->kind) {
    case Module_kind:
        ret = qpv_body(&c, mod->v.Module.body, 0);
        break;
    case Interactive_kind:
        ret = qpv_body(&c, mod->v.Interactive.body, 1);
        break;
    case Expression_kind:
        ret = qpv_visit_expr(&c, mod->v.Expression.body);
        break;
    default:
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "module kind %d should not be possible", (int)mod->kind);
        ret = -1;
        break;
    }
    qpv_exit_scope(&c);
 done:
    JS_FreeAtom(c.ctx, c.atom_debug);
    JS_FreeAtom(c.ctx, c.atom_star);
    return ret < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------------- */
/* ast_preprocess.c: PEP 765 control flow in `finally` (enable_warnings) */

/* ControlFlowInFinallyContext */
typedef struct qpv_cf_ctx {
    BOOL in_finally;
    BOOL in_funcdef;
    BOOL in_loop;
} qpv_cf_ctx;

typedef struct qpv_pp {
    JSContext *ctx;
    qp_error *err;
    const char *filename;
    JSValueConst module;
    const qpv_cf_ctx *top;      /* get_cf_finally_top, NULL when empty */
} qpv_pp;

static int qpv_pp_stmts(qpv_pp *st, asdl_stmt_seq *stmts);

/* push_cf_context / pop_cf_context around `stmts` */
static int qpv_pp_stmts_in(qpv_pp *st, asdl_stmt_seq *stmts, BOOL finally, BOOL funcdef,
                           BOOL loop)
{
    qpv_cf_ctx cf = { finally, funcdef, loop };
    const qpv_cf_ctx *prev = st->top;
    int ret;

    st->top = &cf;
    ret = qpv_pp_stmts(st, stmts);
    st->top = prev;
    return ret;
}

/* control_flow_in_finally_warning */
static int qpv_pp_warning(qpv_pp *st, const char *kw, stmt_ty n)
{
    char msg[64];

    snprintf(msg, sizeof(msg), "'%s' in a 'finally' block", kw);
    return qp_syntax_warn_err(st->ctx, st->err, st->filename, st->module, msg,
                              n->lineno, n->col_offset + 1,
                              n->end_lineno, n->end_col_offset + 1);
}

static int qpv_pp_stmt(qpv_pp *st, stmt_ty s)
{
    Py_ssize_t i;

    if (js_check_stack_overflow(st->ctx->rt, 0)) {
        qp_error_set(st->err, QP_ERR_RECURSION, 0, 0, 0, 0, "%s",
                     "maximum recursion depth exceeded during compilation");
        return -1;
    }
    switch (s->kind) {
    case FunctionDef_kind:
        return qpv_pp_stmts_in(st, s->v.FunctionDef.body, FALSE, TRUE, FALSE);
    case AsyncFunctionDef_kind:
        return qpv_pp_stmts_in(st, s->v.AsyncFunctionDef.body, FALSE, TRUE, FALSE);
    case ClassDef_kind:
        return qpv_pp_stmts(st, s->v.ClassDef.body);
    case Return_kind:
        /* before_return */
        if (st->top && st->top->in_finally && !st->top->in_funcdef)
            return qpv_pp_warning(st, "return", s);
        return 0;
    case For_kind:
        if (qpv_pp_stmts_in(st, s->v.For.body, FALSE, FALSE, TRUE) < 0)
            return -1;
        return qpv_pp_stmts(st, s->v.For.orelse);
    case AsyncFor_kind:
        if (qpv_pp_stmts_in(st, s->v.AsyncFor.body, FALSE, FALSE, TRUE) < 0)
            return -1;
        return qpv_pp_stmts(st, s->v.AsyncFor.orelse);
    case While_kind:
        if (qpv_pp_stmts_in(st, s->v.While.body, FALSE, FALSE, TRUE) < 0)
            return -1;
        return qpv_pp_stmts(st, s->v.While.orelse);
    case If_kind:
        if (qpv_pp_stmts(st, s->v.If.body) < 0)
            return -1;
        return qpv_pp_stmts(st, s->v.If.orelse);
    case With_kind:
        return qpv_pp_stmts(st, s->v.With.body);
    case AsyncWith_kind:
        return qpv_pp_stmts(st, s->v.AsyncWith.body);
    case Try_kind:
    case TryStar_kind: {
        BOOL star = s->kind == TryStar_kind;
        asdl_excepthandler_seq *handlers = star ? s->v.TryStar.handlers : s->v.Try.handlers;
        if (qpv_pp_stmts(st, star ? s->v.TryStar.body : s->v.Try.body) < 0)
            return -1;
        for (i = 0; i < asdl_seq_LEN(handlers); i++) {
            excepthandler_ty h = asdl_seq_GET(handlers, i);
            if (qpv_pp_stmts(st, h->v.ExceptHandler.body) < 0)
                return -1;
        }
        if (qpv_pp_stmts(st, star ? s->v.TryStar.orelse : s->v.Try.orelse) < 0)
            return -1;
        return qpv_pp_stmts_in(st, star ? s->v.TryStar.finalbody : s->v.Try.finalbody,
                               TRUE, FALSE, FALSE);
    }
    case Match_kind:
        for (i = 0; i < asdl_seq_LEN(s->v.Match.cases); i++) {
            match_case_ty m = asdl_seq_GET(s->v.Match.cases, i);
            if (qpv_pp_stmts(st, m->body) < 0)
                return -1;
        }
        return 0;
    case Break_kind:
    case Continue_kind:
        /* before_loop_exit */
        if (st->top && st->top->in_finally && !st->top->in_loop)
            return qpv_pp_warning(st, s->kind == Break_kind ? "break" : "continue", s);
        return 0;
    default:
        return 0;
    }
}

static int qpv_pp_stmts(qpv_pp *st, asdl_stmt_seq *stmts)
{
    Py_ssize_t i;

    for (i = 0; i < asdl_seq_LEN(stmts); i++) {
        if (qpv_pp_stmt(st, asdl_seq_GET(stmts, i)) < 0)
            return -1;
    }
    return 0;
}

static int qp_preprocess_warnings(JSContext *ctx, mod_ty mod, const char *filename,
                                  JSValueConst module, qp_error *err)
{
    qpv_pp st;

    if (JS_IsUndefined(module))
        return 0;
    st.ctx = ctx;
    st.err = err;
    st.filename = filename;
    st.module = module;
    st.top = NULL;
    switch (mod->kind) {
    case Module_kind:
        return qpv_pp_stmts(&st, mod->v.Module.body);
    case Interactive_kind:
        return qpv_pp_stmts(&st, mod->v.Interactive.body);
    default:
        return 0;
    }
}

#undef QPV_WILDCARD_CHECK
#undef QPV_WILDCARD_STAR_CHECK
#undef QPV_NONDEFAULT_MSG_PREFIX
#undef QPV_NONDEFAULT_MSG_SUFFIX
#undef QPV_LOC
#undef QPV_MAXBLOCKS
#undef QPV_INT_MAX_STR_DIGITS
