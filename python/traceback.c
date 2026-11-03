/*
 * dquickts Python: traceback objects and the display of exceptions.
 *
 * Traceback objects are built lazily from the (function, pc) pairs the
 * interpreter reports while unwinding (qp_exc_frame_unwind); the frame
 * facade of tb_frame is created on first access.
 *
 * Exceptions are displayed exactly like CPython 3.15's default
 * sys.excepthook, i.e. traceback._print_exception_bltin() (Lib/traceback.py:
 * TracebackException, StackSummary.format_frame_summary with the caret /
 * anchor logic, chained exceptions, exception groups, notes, SyntaxError
 * formatting, "Did you mean" suggestions), without colour.
 *
 * Derived from CPython Python/traceback.c, Lib/traceback.py,
 * Lib/linecache.py, Lib/textwrap.py (dedent/indent) and
 * Lib/tokenize.py (detect_encoding) (PSF-2.0).
 *
 * Part of the quickjs.c translation unit: everything is static.
 */

/* ---- exceptions.c payload accessors (unit i, M2Exc) --------------------- */
static JSValue qp_exc_context(JSValueConst exc);
static JSValue qp_exc_cause(JSValueConst exc);
static BOOL qp_exc_suppress_context(JSValueConst exc);
static JSValue qp_exc_traceback(JSValueConst exc);
static void qp_exc_set_traceback_raw(JSContext *ctx, JSValueConst exc, JSValue tb);

/* ---- errors.c ------------------------------------------------------------ */
static void qp_exc_chain_context(JSContext *ctx, JSValueConst value);
static JSValue qp_calculate_suggestions(JSContext *ctx, JSValueConst dir_list, JSValueConst name);
static int qp_file_write_str(JSContext *ctx, JSValueConst file, JSValueConst str);
static void qp_file_flush(JSContext *ctx, JSValueConst file);
static JSValue qp_err_private_dict(JSContext *ctx, const char *key);
/* ---- mod_codecs.c --------------------------------------------------------- */
static JSValue qp_codec_decode(JSContext *ctx, JSValueConst object, const char *encoding,
                               const char *errors);


/* ---- this file ----------------------------------------------------------- */
static int qp_init_traceback(JSContext *ctx);
static JSValue qp_tb_from_current_frame(JSContext *ctx);
static JSValue qp_tb_source_line_stripped(JSContext *ctx, JSValueConst filename, int lineno);

#ifndef QP_SUGG_MOVE_COST
#define QP_SUGG_MAX_CANDIDATE_ITEMS 750
#define QP_SUGG_MAX_STRING_SIZE 40
#define QP_SUGG_MOVE_COST 2
#define QP_SUGG_CASE_COST 1
#endif
#define QP_TB_NONE INT_MIN          /* None for int fields */
#define QP_TB_NONE_OR_NULL(v) (JS_IsUndefined(v) || QP_IS_NULL(v))

static void qp_tb_clear_exc(JSContext *ctx)
{
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

static inline BOOL qp_tb_same(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* ========================================================================= */
/* Traceback objects                                                          */
/* ========================================================================= */

typedef struct QPTraceback {
    JSValue next;       /* traceback object or None */
    JSValue func;       /* Python function of the frame; QP_NULL for traceback(...) objects */
    JSValue frame;      /* frame object of the activation (PyTraceBack_Here); QP_NULL
                           until tb_frame is read when it was not running */
    int lasti;          /* byte offset inside the instruction (func set), else
                           the Python offset given to traceback(...) */
    int lineno;         /* -1: computed from lasti */
} QPTraceback;

static inline BOOL qp_tb_check(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_TRACEBACK &&
        JS_VALUE_GET_OBJ(v)->u.opaque != NULL;
}

static inline QPTraceback *qp_tb_data(JSValueConst v)
{
    return (QPTraceback *)JS_VALUE_GET_OBJ(v)->u.opaque;
}

static void qp_tb_finalizer(JSRuntime *rt, JSValue val)
{
    QPTraceback *tb = (QPTraceback *)JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!tb)
        return;
    JS_FreeValueRT(rt, tb->next);
    JS_FreeValueRT(rt, tb->func);
    JS_FreeValueRT(rt, tb->frame);
    js_free_rt(rt, tb);
}

static void qp_tb_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPTraceback *tb = (QPTraceback *)JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!tb)
        return;
    JS_MarkValue(rt, tb->next, mark_func);
    JS_MarkValue(rt, tb->func, mark_func);
    JS_MarkValue(rt, tb->frame, mark_func);
}

/* New traceback object (all arguments borrowed). */
static JSValue qp_tb_alloc(JSContext *ctx, JSValueConst next, JSValueConst func,
                           JSValueConst frame, int lasti, int lineno)
{
    JSValue obj;
    QPTraceback *tb;

    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, traceback)),
                                 JS_CLASS_PY_TRACEBACK);
    if (JS_IsException(obj))
        return obj;
    tb = js_malloc(ctx, sizeof(*tb));
    if (!tb) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    tb->next = JS_DupValue(ctx, QP_IS_NULL(next) ? JS_UNDEFINED : next);
    tb->func = JS_DupValue(ctx, func);
    tb->frame = JS_DupValue(ctx, frame);
    tb->lasti = lasti;
    tb->lineno = lineno;
    JS_VALUE_GET_OBJ(obj)->u.opaque = tb;
    return obj;
}

/* _PyTraceBack_FromFrame(NULL, frame) for the innermost running Python
   frame at its current instruction: a new traceback, JS_UNDEFINED when no
   Python code runs, JS_EXCEPTION on error. */
static JSValue qp_tb_from_current_frame(JSContext *ctx)
{
    JSStackFrame *sf = qp_py_frame_at(ctx, 0);
    JSFunctionBytecode *b;
    JSValue frame;

    if (!sf)
        return JS_UNDEFINED;
    b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    frame = qp_frame_of(ctx, sf);
    if (JS_IsException(frame))
        return frame;
    return qp_tb_alloc(ctx, JS_UNDEFINED, sf->cur_func, frame,
                       (int)qp_fr_sf_pc(sf, b), -1);
}

/* Bytecode of the code a traceback entry refers to (NULL if unknown). */
static JSFunctionBytecode *qp_tb_bytecode(JSContext *ctx, QPTraceback *tb)
{
    JSFunctionBytecode *b = NULL;
    if (!QP_IS_NULL(tb->func)) {
        b = qp_code_bytecode(tb->func);
    } else if (!QP_IS_NULL(tb->frame)) {
        JSAtom a = JS_NewAtom(ctx, "f_code");
        JSValue code = a != JS_ATOM_NULL ? qp_getattr(ctx, tb->frame, a) : JS_EXCEPTION;
        if (a != JS_ATOM_NULL)
            JS_FreeAtom(ctx, a);
        if (JS_IsException(code)) {
            qp_tb_clear_exc(ctx);
            return NULL;
        }
        b = qp_code_bytecode(code);
        JS_FreeValue(ctx, code);
    }
    return b;
}

/* Byte offset of tb's instruction in b (its bytecode), -1 if unknown. */
static int qp_tb_pc(QPTraceback *tb, JSFunctionBytecode *b)
{
    if (!QP_IS_NULL(tb->func))
        return tb->lasti;
    return qp_code_lasti2pc(b, tb->lasti);
}

/* tb_lineno: the stored line, else PyCode_Addr2Line(code, lasti); QP_TB_NONE
   when unknown. */
static int qp_tb_lineno(JSContext *ctx, QPTraceback *tb)
{
    JSFunctionBytecode *b;
    QPLoc loc;
    int pc;
    if (tb->lineno != -1)
        return tb->lineno;
    b = qp_tb_bytecode(ctx, tb);
    if (b && (pc = qp_tb_pc(tb, b)) >= 0 && qp_code_addr2loc(b, (uint32_t)pc, &loc) == 0 &&
        loc.lineno >= 0)
        return loc.lineno;
    return QP_TB_NONE;
}

/* At the exception: label of a Python frame: convert a JS value, chain its
   __context__, and prepend a traceback entry (PyTraceBack_Here). */
static void qp_exc_frame_unwind(JSContext *ctx, JSValueConst func, uint32_t pc)
{
    JSRuntime *rt = ctx->rt;
    JSValue cur, tb, frame = QP_NULL;
    JSStackFrame *sf;
    JSFunctionBytecode *b;

    if (rt->current_exception_is_uncatchable)
        return;
    /* an error return without a pending exception (QP_NULL) is normalized
       to SystemError like any other non-Python thrown value */
    if (!JS_HasException(ctx) || !qp_exc_check(ctx, rt->current_exception)) {
        JSValue e = qp_exc_normalize(ctx, JS_GetException(ctx));
        BOOL ok = qp_exc_check(ctx, e);
        if (ok)
            qp_exc_chain_context(ctx, e);
        JS_Throw(ctx, e);
        if (!ok)
            return;
    }
    cur = JS_DupValue(ctx, rt->current_exception);

    /* the frame object of the activation (CPython PyTraceBack_Here takes
       _PyFrame_GetFrameObject): it keeps f_locals and f_back once the
       frame ends (qp_frame_detach) */
    sf = rt->current_stack_frame;
    b = qp_code_bytecode(func);
    if (sf && b && qp_tb_same(sf->cur_func, func) && b->py_code) {
        frame = qp_frame_of(ctx, sf);
        if (JS_IsException(frame)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            frame = QP_NULL;
        }
    }
    tb = qp_tb_alloc(ctx, qp_exc_traceback(cur), func, frame, (int)pc, -1);
    if (JS_IsException(tb)) {
        /* out of memory: keep the original exception pending */
        JS_Throw(ctx, cur);
        return;
    }
    qp_exc_set_traceback_raw(ctx, cur, tb);
    JS_FreeValue(ctx, cur);
}

static JSValue qp_tb_get_next(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, qp_tb_data(self)->next);
}

static int qp_tb_set_next(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPTraceback *tb = qp_tb_data(self);
    JSValueConst cursor;
    char buf[256];
    JSValue old;

    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "can't delete tb_next attribute");
        return -1;
    }
    if (!JS_IsUndefined(value) && !JS_IsNull(value) && !qp_tb_check(value)) {
        QPType *t = qp_type_of(ctx, value);
        qp_raise_type_error(ctx, "expected traceback object, got '%s'",
                            t->tp_name ? t->tp_name : JS_AtomGetStr(ctx, buf, sizeof(buf), t->name));
        return -1;
    }
    for (cursor = value; qp_tb_check(cursor); cursor = qp_tb_data(cursor)->next) {
        if (qp_tb_same(cursor, self)) {
            qp_raise_value_error(ctx, "traceback loop detected");
            return -1;
        }
    }
    old = tb->next;
    tb->next = JS_DupValue(ctx, JS_IsNull(value) ? JS_UNDEFINED : value);
    JS_FreeValue(ctx, old);
    return 0;
}

/* tb_frame, created on first use for an entry recorded without its frame
   object.  Borrowed; JS_EXCEPTION on error. */
static JSValue qp_tb_frame(JSContext *ctx, QPTraceback *tb)
{
    if (QP_IS_NULL(tb->frame)) {
        JSValue f = qp_frame_new(ctx, tb->func, (uint32_t)tb->lasti, JS_UNDEFINED, QP_NULL);
        if (JS_IsException(f))
            return f;
        tb->frame = f;
    }
    return tb->frame;
}

static JSValue qp_tb_get_frame(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, qp_tb_frame(ctx, qp_tb_data(self)));
}

static JSValue qp_tb_get_lasti(JSContext *ctx, JSValueConst self, int magic)
{
    QPTraceback *tb = qp_tb_data(self);
    JSFunctionBytecode *b;

    if (QP_IS_NULL(tb->func) || tb->lasti < 0 || !(b = qp_code_bytecode(tb->func)))
        return JS_NewInt32(ctx, tb->lasti);
    return JS_NewInt32(ctx, qp_code_pc2lasti(b, (uint32_t)tb->lasti));
}

static JSValue qp_tb_get_lineno(JSContext *ctx, JSValueConst self, int magic)
{
    int l = qp_tb_lineno(ctx, qp_tb_data(self));
    if (l == QP_TB_NONE)
        return JS_UNDEFINED;
    return JS_NewInt32(ctx, l);
}

static JSValue qp_tb_dir(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    static const char *const names[] = { "tb_frame", "tb_next", "tb_lasti", "tb_lineno" };
    JSValue items[4];
    int i;
    for (i = 0; i < 4; i++) {
        items[i] = qp_str_from_cstr(ctx, names[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    return qp_list_new_steal(ctx, 4, items);
}

/* traceback(tb_next, tb_frame, tb_lasti, tb_lineno) */
static JSValue qp_tb_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                         size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "tb_next", "tb_frame", "tb_lasti", "tb_lineno", NULL };
    static const QPArgSpec spec = { "traceback", kw, 4, 4, 0 };
    JSValueConst a[4];
    int lasti, lineno;
    char buf[256];

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_typecheck(ctx, a[1], QP_TYPE(ctx, frame))) {
        QPType *t = qp_type_of(ctx, a[1]);
        return qp_raise_type_error(ctx, "traceback() argument 'tb_frame' must be frame, not %s",
                                   t->tp_name ? t->tp_name :
                                   JS_AtomGetStr(ctx, buf, sizeof(buf), t->name));
    }
    if (qp_arg_int(ctx, a[2], &lasti) < 0 || qp_arg_int(ctx, a[3], &lineno) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(a[0]) && !JS_IsNull(a[0]) && !qp_tb_check(a[0])) {
        QPType *t = qp_type_of(ctx, a[0]);
        return qp_raise_type_error(ctx, "expected traceback object or None, got '%s'",
                                   t->tp_name ? t->tp_name :
                                   JS_AtomGetStr(ctx, buf, sizeof(buf), t->name));
    }
    return qp_tb_alloc(ctx, JS_IsNull(a[0]) ? JS_UNDEFINED : a[0], QP_NULL, a[1],
                       lasti, lineno);
}

static const QPMethodDef qp_tb_methods[] = {
    { "__dir__", qp_tb_dir, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPGetSetDef qp_tb_getsets[] = {
    { "tb_next", qp_tb_get_next, qp_tb_set_next, 0, NULL },
    { "tb_frame", qp_tb_get_frame, NULL, 0, NULL, QP_GETSET_MEMBER },
    { "tb_lasti", qp_tb_get_lasti, NULL, 0, NULL, QP_GETSET_MEMBER },
    { "tb_lineno", qp_tb_get_lineno, NULL, 0, NULL },
    { NULL },
};

static const QPTypeSpec qp_tb_spec = {
    .name = "traceback",
    .doc = "traceback(tb_next, tb_frame, tb_lasti, tb_lineno)\n--\n\n"
           "Create a new traceback object.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_TRACEBACK,
    .nslots = 0,
    .slots = { .tp_new = qp_tb_new },
    .methods = qp_tb_methods,
    .getsets = qp_tb_getsets,
};

/* Called by qp_init_exceptions. */
static int qp_init_traceback(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, traceback), &qp_tb_spec);
}

/* ========================================================================= */
/* Code point text buffers                                                    */
/* ========================================================================= */

typedef struct QPText {
    JSContext *ctx;
    uint32_t *buf;
    size_t len, size;
    BOOL oom;
} QPText;

/* A line of a text: [start, end) without its terminator. */
typedef struct QPLine {
    size_t start, end;
} QPLine;

static void qp_text_init(QPText *t, JSContext *ctx)
{
    t->ctx = ctx;
    t->buf = NULL;
    t->len = t->size = 0;
    t->oom = FALSE;
}

static void qp_text_free(QPText *t)
{
    js_free(t->ctx, t->buf);
    t->buf = NULL;
    t->len = t->size = 0;
}

static BOOL qp_text_reserve(QPText *t, size_t extra)
{
    size_t ns;
    uint32_t *nb;
    if (t->oom)
        return FALSE;
    if (t->len + extra <= t->size)
        return TRUE;
    ns = max_int(64, t->size * 3 / 2);
    if (ns < t->len + extra)
        ns = t->len + extra;
    nb = js_realloc(t->ctx, t->buf, ns * sizeof(uint32_t));
    if (!nb) {
        qp_tb_clear_exc(t->ctx);
        t->oom = TRUE;
        return FALSE;
    }
    t->buf = nb;
    t->size = ns;
    return TRUE;
}

static void qp_text_putc(QPText *t, uint32_t c)
{
    if (qp_text_reserve(t, 1))
        t->buf[t->len++] = c;
}

static void qp_text_put(QPText *t, const uint32_t *s, size_t n)
{
    if (n && qp_text_reserve(t, n)) {
        memcpy(t->buf + t->len, s, n * sizeof(uint32_t));
        t->len += n;
    }
}

static void qp_text_fill(QPText *t, uint32_t c, Py_ssize_t n)
{
    while (n-- > 0)
        qp_text_putc(t, c);
}

/* UTF-8 C string (our literals are ASCII). */
static void qp_text_puts(QPText *t, const char *s)
{
    const uint8_t *p = (const uint8_t *)s, *p_next;
    while (*p) {
        if (*p < 0x80) {
            qp_text_putc(t, *p++);
        } else {
            int c = unicode_from_utf8(p, 6, &p_next);
            if (c < 0) {
                qp_text_putc(t, 0xFFFD);
                p++;
            } else {
                qp_text_putc(t, c);
                p = p_next;
            }
        }
    }
}

static void qp_text_printf(QPText *t, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qp_text_puts(t, buf);
}

/* Append a str; 0 or -1 with a pending exception. */
static int qp_text_put_str(QPText *t, JSValueConst str)
{
    uint32_t *b;
    int n = JS_ToUTF32String(t->ctx, &b, str);
    if (n < 0)
        return -1;
    qp_text_put(t, b, n);
    js_free(t->ctx, b);
    return 0;
}

/* Append str(v) (or the placeholder when str() fails, _safe_string). */
static void qp_text_put_safe_str(QPText *t, JSValueConst v, BOOL use_repr, const char *what)
{
    JSValue s = use_repr ? qp_repr(t->ctx, v) : qp_str(t->ctx, v);
    if (JS_IsException(s) || qp_text_put_str(t, s) < 0) {
        qp_tb_clear_exc(t->ctx);
        qp_text_printf(t, "<%s %s() failed>", what, use_repr ? "repr" : "str");
    }
    JS_FreeValue(t->ctx, s);
}

static JSValue qp_text_to_str(QPText *t)
{
    if (t->oom)
        return JS_ThrowOutOfMemory(t->ctx);
    return JS_NewUTF32String(t->ctx, t->buf, (int)t->len);
}

static inline BOOL qp_tb_isspace(uint32_t c)
{
    return _PyUnicode_IsWhitespace(c) != 0;
}

/* str.splitlines(): boundaries are _PyUnicode_IsLinebreak characters, \r\n
   counts once.  *pcount lines; returns NULL (count 0) for an empty text or
   on allocation failure (t->oom set when t != NULL). */
static QPLine *qp_text_splitlines(JSContext *ctx, const uint32_t *s, size_t n, size_t *pcount,
                                  size_t **pnext)
{
    QPLine *lines = NULL;
    size_t *nexts = NULL;
    size_t count = 0, size = 0, i = 0, start;

    *pcount = 0;
    if (pnext)
        *pnext = NULL;
    while (i < n) {
        size_t eol;
        start = i;
        while (i < n && !_PyUnicode_IsLinebreak(s[i]))
            i++;
        eol = i;
        if (i < n) {
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
                i += 2;
            else
                i++;
        }
        if (count == size) {
            size_t ns = size ? size * 2 : 8;
            QPLine *nl = js_realloc(ctx, lines, ns * sizeof(QPLine));
            size_t *nn = pnext ? js_realloc(ctx, nexts, ns * sizeof(size_t)) : NULL;
            if (!nl || (pnext && !nn)) {
                qp_tb_clear_exc(ctx);
                js_free(ctx, nl ? nl : lines);
                js_free(ctx, nn ? nn : nexts);
                return NULL;
            }
            lines = nl;
            nexts = nn;
            size = ns;
        }
        lines[count].start = start;
        lines[count].end = eol;
        if (pnext)
            nexts[count] = i;
        count++;
    }
    *pcount = count;
    if (pnext)
        *pnext = nexts;
    return lines;
}

/* textwrap.indent(text, prefix, lambda line: True) appended to out. */
static void qp_text_indent_all(QPText *out, const uint32_t *s, size_t n, const uint32_t *prefix,
                               size_t prefix_len)
{
    size_t count, i, *nexts;
    QPLine *lines = qp_text_splitlines(out->ctx, s, n, &count, &nexts);
    if (n && !lines) {
        out->oom = TRUE;
        return;
    }
    for (i = 0; i < count; i++) {
        qp_text_put(out, prefix, prefix_len);
        qp_text_put(out, s + lines[i].start, nexts[i] - lines[i].start);
    }
    js_free(out->ctx, lines);
    js_free(out->ctx, nexts);
}

static int qp_tb_ucmp(const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    size_t i, m = an < bn ? an : bn;
    for (i = 0; i < m; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return an < bn ? -1 : an > bn ? 1 : 0;
}

static BOOL qp_tb_all_space(const uint32_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!qp_tb_isspace(s[i]))
            return FALSE;
    }
    return TRUE;
}

/* textwrap.dedent(text) appended to out (lines split on '\n' only). */
static void qp_text_dedent(QPText *out, const uint32_t *s, size_t n)
{
    const uint32_t *l1 = NULL, *l2 = NULL;
    size_t l1n = 0, l2n = 0, margin = 0, i, start;

    /* min / max of the non-blank lines */
    for (start = 0; ; ) {
        size_t e = start;
        while (e < n && s[e] != '\n')
            e++;
        if (e > start && !qp_tb_all_space(s + start, e - start)) {
            if (!l1 || qp_tb_ucmp(s + start, e - start, l1, l1n) < 0) {
                l1 = s + start;
                l1n = e - start;
            }
            if (!l2 || qp_tb_ucmp(s + start, e - start, l2, l2n) > 0) {
                l2 = s + start;
                l2n = e - start;
            }
        }
        if (e >= n)
            break;
        start = e + 1;
    }
    if (l1) {
        for (i = 0; i < l1n; i++) {
            margin = i;
            if (i >= l2n || l1[i] != l2[i] || (l1[i] != ' ' && l1[i] != '\t'))
                break;
        }
    }
    for (start = 0; ; ) {
        size_t e = start;
        while (e < n && s[e] != '\n')
            e++;
        if (e > start && !qp_tb_all_space(s + start, e - start)) {
            size_t m = margin < e - start ? margin : e - start;
            qp_text_put(out, s + start + m, e - start - m);
        }
        if (e >= n)
            break;
        qp_text_putc(out, '\n');
        start = e + 1;
    }
}

/* ========================================================================= */
/* Display width (traceback._display_width / _wlen)                          */
/* ========================================================================= */

/* Code points whose _str_width() is not 1 (combining / Cf -> 0, East Asian
   W/F -> 2), generated from CPython 3.15's unicodedata (Unicode 17.0.0). */
static const struct { uint32_t lo, hi; uint8_t w; } qp_tb_width_table[] = {
    {0x300,0x34E,0}, {0x350,0x36F,0}, {0x483,0x487,0}, {0x591,0x5BD,0},
    {0x5BF,0x5BF,0}, {0x5C1,0x5C2,0}, {0x5C4,0x5C5,0}, {0x5C7,0x5C7,0},
    {0x600,0x605,0}, {0x610,0x61A,0}, {0x61C,0x61C,0}, {0x64B,0x65F,0},
    {0x670,0x670,0}, {0x6D6,0x6DD,0}, {0x6DF,0x6E4,0}, {0x6E7,0x6E8,0},
    {0x6EA,0x6ED,0}, {0x70F,0x70F,0}, {0x711,0x711,0}, {0x730,0x74A,0},
    {0x7EB,0x7F3,0}, {0x7FD,0x7FD,0}, {0x816,0x819,0}, {0x81B,0x823,0},
    {0x825,0x827,0}, {0x829,0x82D,0}, {0x859,0x85B,0}, {0x890,0x891,0},
    {0x897,0x89F,0}, {0x8CA,0x8FF,0}, {0x93C,0x93C,0}, {0x94D,0x94D,0},
    {0x951,0x954,0}, {0x9BC,0x9BC,0}, {0x9CD,0x9CD,0}, {0x9FE,0x9FE,0},
    {0xA3C,0xA3C,0}, {0xA4D,0xA4D,0}, {0xABC,0xABC,0}, {0xACD,0xACD,0},
    {0xB3C,0xB3C,0}, {0xB4D,0xB4D,0}, {0xBCD,0xBCD,0}, {0xC3C,0xC3C,0},
    {0xC4D,0xC4D,0}, {0xC55,0xC56,0}, {0xCBC,0xCBC,0}, {0xCCD,0xCCD,0},
    {0xD3B,0xD3C,0}, {0xD4D,0xD4D,0}, {0xDCA,0xDCA,0}, {0xE38,0xE3A,0},
    {0xE48,0xE4B,0}, {0xEB8,0xEBA,0}, {0xEC8,0xECB,0}, {0xF18,0xF19,0},
    {0xF35,0xF35,0}, {0xF37,0xF37,0}, {0xF39,0xF39,0}, {0xF71,0xF72,0},
    {0xF74,0xF74,0}, {0xF7A,0xF7D,0}, {0xF80,0xF80,0}, {0xF82,0xF84,0},
    {0xF86,0xF87,0}, {0xFC6,0xFC6,0}, {0x1037,0x1037,0}, {0x1039,0x103A,0},
    {0x108D,0x108D,0}, {0x1100,0x115F,2}, {0x135D,0x135F,0},
    {0x1714,0x1715,0}, {0x1734,0x1734,0}, {0x17D2,0x17D2,0},
    {0x17DD,0x17DD,0}, {0x180E,0x180E,0}, {0x18A9,0x18A9,0},
    {0x1939,0x193B,0}, {0x1A17,0x1A18,0}, {0x1A60,0x1A60,0},
    {0x1A75,0x1A7C,0}, {0x1A7F,0x1A7F,0}, {0x1AB0,0x1ABD,0},
    {0x1ABF,0x1ADD,0}, {0x1AE0,0x1AEB,0}, {0x1B34,0x1B34,0},
    {0x1B44,0x1B44,0}, {0x1B6B,0x1B73,0}, {0x1BAA,0x1BAB,0},
    {0x1BE6,0x1BE6,0}, {0x1BF2,0x1BF3,0}, {0x1C37,0x1C37,0},
    {0x1CD0,0x1CD2,0}, {0x1CD4,0x1CE0,0}, {0x1CE2,0x1CE8,0},
    {0x1CED,0x1CED,0}, {0x1CF4,0x1CF4,0}, {0x1CF8,0x1CF9,0},
    {0x1DC0,0x1DFF,0}, {0x200B,0x200F,0}, {0x202A,0x202E,0},
    {0x2060,0x2064,0}, {0x2066,0x206F,0}, {0x20D0,0x20DC,0},
    {0x20E1,0x20E1,0}, {0x20E5,0x20F0,0}, {0x231A,0x231B,2},
    {0x2329,0x232A,2}, {0x23E9,0x23EC,2}, {0x23F0,0x23F0,2},
    {0x23F3,0x23F3,2}, {0x25FD,0x25FE,2}, {0x2614,0x2615,2},
    {0x2630,0x2637,2}, {0x2648,0x2653,2}, {0x267F,0x267F,2},
    {0x268A,0x268F,2}, {0x2693,0x2693,2}, {0x26A1,0x26A1,2},
    {0x26AA,0x26AB,2}, {0x26BD,0x26BE,2}, {0x26C4,0x26C5,2},
    {0x26CE,0x26CE,2}, {0x26D4,0x26D4,2}, {0x26EA,0x26EA,2},
    {0x26F2,0x26F3,2}, {0x26F5,0x26F5,2}, {0x26FA,0x26FA,2},
    {0x26FD,0x26FD,2}, {0x2705,0x2705,2}, {0x270A,0x270B,2},
    {0x2728,0x2728,2}, {0x274C,0x274C,2}, {0x274E,0x274E,2},
    {0x2753,0x2755,2}, {0x2757,0x2757,2}, {0x2795,0x2797,2},
    {0x27B0,0x27B0,2}, {0x27BF,0x27BF,2}, {0x2B1B,0x2B1C,2},
    {0x2B50,0x2B50,2}, {0x2B55,0x2B55,2}, {0x2CEF,0x2CF1,0},
    {0x2D7F,0x2D7F,0}, {0x2DE0,0x2DFF,0}, {0x2E80,0x2E99,2},
    {0x2E9B,0x2EF3,2}, {0x2F00,0x2FD5,2}, {0x2FF0,0x3029,2},
    {0x302A,0x302F,0}, {0x3030,0x303E,2}, {0x3041,0x3096,2},
    {0x3099,0x309A,0}, {0x309B,0x30FF,2}, {0x3105,0x312F,2},
    {0x3131,0x318E,2}, {0x3190,0x31E5,2}, {0x31EF,0x321E,2},
    {0x3220,0x3247,2}, {0x3250,0xA48C,2}, {0xA490,0xA4C6,2},
    {0xA66F,0xA66F,0}, {0xA674,0xA67D,0}, {0xA69E,0xA69F,0},
    {0xA6F0,0xA6F1,0}, {0xA806,0xA806,0}, {0xA82C,0xA82C,0},
    {0xA8C4,0xA8C4,0}, {0xA8E0,0xA8F1,0}, {0xA92B,0xA92D,0},
    {0xA953,0xA953,0}, {0xA960,0xA97C,2}, {0xA9B3,0xA9B3,0},
    {0xA9C0,0xA9C0,0}, {0xAAB0,0xAAB0,0}, {0xAAB2,0xAAB4,0},
    {0xAAB7,0xAAB8,0}, {0xAABE,0xAABF,0}, {0xAAC1,0xAAC1,0},
    {0xAAF6,0xAAF6,0}, {0xABED,0xABED,0}, {0xAC00,0xD7A3,2},
    {0xF900,0xFAFF,2}, {0xFB1E,0xFB1E,0}, {0xFE10,0xFE19,2},
    {0xFE20,0xFE2F,0}, {0xFE30,0xFE52,2}, {0xFE54,0xFE66,2},
    {0xFE68,0xFE6B,2}, {0xFEFF,0xFEFF,0}, {0xFF01,0xFF60,2},
    {0xFFE0,0xFFE6,2}, {0xFFF9,0xFFFB,0}, {0x101FD,0x101FD,0},
    {0x102E0,0x102E0,0}, {0x10376,0x1037A,0}, {0x10A0D,0x10A0D,0},
    {0x10A0F,0x10A0F,0}, {0x10A38,0x10A3A,0}, {0x10A3F,0x10A3F,0},
    {0x10AE5,0x10AE6,0}, {0x10D24,0x10D27,0}, {0x10D69,0x10D6D,0},
    {0x10EAB,0x10EAC,0}, {0x10EFA,0x10EFB,0}, {0x10EFD,0x10EFF,0},
    {0x10F46,0x10F50,0}, {0x10F82,0x10F85,0}, {0x11046,0x11046,0},
    {0x11070,0x11070,0}, {0x1107F,0x1107F,0}, {0x110B9,0x110BA,0},
    {0x110BD,0x110BD,0}, {0x110CD,0x110CD,0}, {0x11100,0x11102,0},
    {0x11133,0x11134,0}, {0x11173,0x11173,0}, {0x111C0,0x111C0,0},
    {0x111CA,0x111CA,0}, {0x11235,0x11236,0}, {0x112E9,0x112EA,0},
    {0x1133B,0x1133C,0}, {0x1134D,0x1134D,0}, {0x11366,0x1136C,0},
    {0x11370,0x11374,0}, {0x113CE,0x113D0,0}, {0x11442,0x11442,0},
    {0x11446,0x11446,0}, {0x1145E,0x1145E,0}, {0x114C2,0x114C3,0},
    {0x115BF,0x115C0,0}, {0x1163F,0x1163F,0}, {0x116B6,0x116B7,0},
    {0x1172B,0x1172B,0}, {0x11839,0x1183A,0}, {0x1193D,0x1193E,0},
    {0x11943,0x11943,0}, {0x119E0,0x119E0,0}, {0x11A34,0x11A34,0},
    {0x11A47,0x11A47,0}, {0x11A99,0x11A99,0}, {0x11C3F,0x11C3F,0},
    {0x11D42,0x11D42,0}, {0x11D44,0x11D45,0}, {0x11D97,0x11D97,0},
    {0x11F41,0x11F42,0}, {0x13430,0x1343F,0}, {0x1612F,0x1612F,0},
    {0x16AF0,0x16AF4,0}, {0x16B30,0x16B36,0}, {0x16FE0,0x16FE4,2},
    {0x16FF0,0x16FF1,0}, {0x16FF2,0x16FF6,2}, {0x17000,0x18CD5,2},
    {0x18CFF,0x18D1E,2}, {0x18D80,0x18DF2,2}, {0x1AFF0,0x1AFF3,2},
    {0x1AFF5,0x1AFFB,2}, {0x1AFFD,0x1AFFE,2}, {0x1B000,0x1B122,2},
    {0x1B132,0x1B132,2}, {0x1B150,0x1B152,2}, {0x1B155,0x1B155,2},
    {0x1B164,0x1B167,2}, {0x1B170,0x1B2FB,2}, {0x1BC9E,0x1BC9E,0},
    {0x1BCA0,0x1BCA3,0}, {0x1D165,0x1D169,0}, {0x1D16D,0x1D182,0},
    {0x1D185,0x1D18B,0}, {0x1D1AA,0x1D1AD,0}, {0x1D242,0x1D244,0},
    {0x1D300,0x1D356,2}, {0x1D360,0x1D376,2}, {0x1E000,0x1E006,0},
    {0x1E008,0x1E018,0}, {0x1E01B,0x1E021,0}, {0x1E023,0x1E024,0},
    {0x1E026,0x1E02A,0}, {0x1E08F,0x1E08F,0}, {0x1E130,0x1E136,0},
    {0x1E2AE,0x1E2AE,0}, {0x1E2EC,0x1E2EF,0}, {0x1E4EC,0x1E4EF,0},
    {0x1E5EE,0x1E5EF,0}, {0x1E6E3,0x1E6E3,0}, {0x1E6E6,0x1E6E6,0},
    {0x1E6EE,0x1E6EF,0}, {0x1E6F5,0x1E6F5,0}, {0x1E8D0,0x1E8D6,0},
    {0x1E944,0x1E94A,0}, {0x1F004,0x1F004,2}, {0x1F0CF,0x1F0CF,2},
    {0x1F18E,0x1F18E,2}, {0x1F191,0x1F19A,2}, {0x1F200,0x1F202,2},
    {0x1F210,0x1F23B,2}, {0x1F240,0x1F248,2}, {0x1F250,0x1F251,2},
    {0x1F260,0x1F265,2}, {0x1F300,0x1F320,2}, {0x1F32D,0x1F335,2},
    {0x1F337,0x1F37C,2}, {0x1F37E,0x1F393,2}, {0x1F3A0,0x1F3CA,2},
    {0x1F3CF,0x1F3D3,2}, {0x1F3E0,0x1F3F0,2}, {0x1F3F4,0x1F3F4,2},
    {0x1F3F8,0x1F43E,2}, {0x1F440,0x1F440,2}, {0x1F442,0x1F4FC,2},
    {0x1F4FF,0x1F53D,2}, {0x1F54B,0x1F54E,2}, {0x1F550,0x1F567,2},
    {0x1F57A,0x1F57A,2}, {0x1F595,0x1F596,2}, {0x1F5A4,0x1F5A4,2},
    {0x1F5FB,0x1F64F,2}, {0x1F680,0x1F6C5,2}, {0x1F6CC,0x1F6CC,2},
    {0x1F6D0,0x1F6D2,2}, {0x1F6D5,0x1F6D8,2}, {0x1F6DC,0x1F6DF,2},
    {0x1F6EB,0x1F6EC,2}, {0x1F6F4,0x1F6FC,2}, {0x1F7E0,0x1F7EB,2},
    {0x1F7F0,0x1F7F0,2}, {0x1F90C,0x1F93A,2}, {0x1F93C,0x1F945,2},
    {0x1F947,0x1F9FF,2}, {0x1FA70,0x1FA7C,2}, {0x1FA80,0x1FA8A,2},
    {0x1FA8E,0x1FAC6,2}, {0x1FAC8,0x1FAC8,2}, {0x1FACD,0x1FADC,2},
    {0x1FADF,0x1FAEA,2}, {0x1FAEF,0x1FAF8,2}, {0x20000,0x2FFFD,2},
    {0x30000,0x3FFFD,2}, {0xE0001,0xE0001,0}, {0xE0020,0xE007F,0},
};

static int qp_tb_char_width(uint32_t c)
{
    int lo = 0, hi = countof(qp_tb_width_table) - 1;
    if (c < 128)
        return 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < qp_tb_width_table[mid].lo)
            hi = mid - 1;
        else if (c > qp_tb_width_table[mid].hi)
            lo = mid + 1;
        else
            return qp_tb_width_table[mid].w;
    }
    return 1;
}

/* _wlen(s): widths minus ANSI escape sequences plus one per \x1a. */
static Py_ssize_t qp_tb_wlen(const uint32_t *s, size_t n)
{
    Py_ssize_t w = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        w += qp_tb_char_width(s[i]);
        if (s[i] == 0x1a)
            w++;
    }
    for (i = 0; i + 1 < n; ) {
        if (s[i] == 0x1b && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && s[j] >= 0x20 && s[j] <= 0x40)
                j++;
            if (j < n && s[j] >= 0x41 && s[j] <= 0x7e) {
                w -= (Py_ssize_t)(j + 1 - i);
                i = j + 1;
                continue;
            }
        }
        i++;
    }
    return w;
}

/* _display_width(line, offset): _wlen(line[:offset]) (offset >= 0) */
static Py_ssize_t qp_tb_display_width(const uint32_t *s, size_t n, Py_ssize_t offset)
{
    if (offset < 0)
        offset = (Py_ssize_t)n + offset < 0 ? 0 : (Py_ssize_t)n + offset;
    if ((size_t)offset > n)
        offset = n;
    return qp_tb_wlen(s, offset);
}

/* _byte_offset_to_character_offset(str, offset): number of characters of
   line.encode()[:offset].decode(errors="replace"). */
static Py_ssize_t qp_tb_byte_to_char(const uint32_t *s, size_t n, Py_ssize_t offset)
{
    Py_ssize_t bytes = 0, chars = 0;
    size_t i;
    if (offset <= 0)
        return 0;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        int l = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (bytes + l > offset) {
            chars++;   /* truncated sequence -> one U+FFFD */
            return chars;
        }
        bytes += l;
        chars++;
        if (bytes == offset)
            break;
    }
    return chars;
}

/* UTF-8 encoding of code points (lone surrogates: -1, like str.encode()). */
static char *qp_tb_to_utf8(JSContext *ctx, const uint32_t *s, size_t n, size_t *plen)
{
    char *buf = js_malloc(ctx, n * 4 + 1);
    size_t i, l = 0;
    if (!buf) {
        qp_tb_clear_exc(ctx);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (s[i] >= 0xD800 && s[i] <= 0xDFFF) {
            js_free(ctx, buf);
            return NULL;
        }
        l += unicode_to_utf8((uint8_t *)buf + l, s[i]);
    }
    buf[l] = '\0';
    *plen = l;
    return buf;
}

/* ========================================================================= */
/* Source lines (linecache)                                                   */
/* ========================================================================= */

typedef struct QPSrcFile {
    struct QPSrcFile *next;
    JSValue filename;
    uint32_t *text;         /* decoded source */
    size_t len;
    QPLine *lines;          /* linecache lines (without terminator) */
    size_t nlines;
} QPSrcFile;

/* Strict UTF-8 decoding; -1 on invalid input. */
static int qp_tb_decode_utf8(const uint8_t *p, size_t n, uint32_t *out, size_t *pout)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint32_t c = p[i];
        int l;
        uint32_t min;
        if (c < 0x80) {
            out[o++] = c;
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            l = 1; c &= 0x1F; min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            l = 2; c &= 0x0F; min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            l = 3; c &= 0x07; min = 0x10000;
        } else {
            return -1;
        }
        {
            int k;
            if (i + (size_t)l >= n)
                return -1;
            for (k = 1; k <= l; k++) {
                if ((p[i + k] & 0xC0) != 0x80)
                    return -1;
                c = (c << 6) | (p[i + k] & 0x3F);
            }
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            return -1;
        out[o++] = c;
        i += l + 1;
    }
    *pout = o;
    return 0;
}

/* tokenize.detect_encoding: coding cookie of the first two lines.  Returns
   0 utf-8, 1 latin-1, 2 ascii, 3 another codec (its normalized name in
   other[QP_TB_ENC_NAME]), -1 invalid. */
#define QP_TB_ENC_NAME 64
static int qp_tb_detect_encoding(const uint8_t *p, size_t n, BOOL bom, char *other)
{
    size_t pos = 0;
    int line;
    for (line = 0; line < 2 && pos < n; line++) {
        size_t e = pos, i;
        BOOL blank;
        while (e < n && p[e] != '\n' && p[e] != '\r')
            e++;
        i = pos;
        while (i < e && (p[i] == ' ' || p[i] == '\t' || p[i] == '\f'))
            i++;
        blank = (i >= e || p[i] == '#');
        if (i < e && p[i] == '#') {
            /* ^[ \t\f]*#.*?coding[:=][ \t]*([-\w.]+) */
            size_t k;
            for (k = i + 1; k + 7 <= e; k++) {
                if (!memcmp(p + k, "coding", 6) && (p[k + 6] == ':' || p[k + 6] == '=')) {
                    size_t s = k + 7, t;
                    char name[QP_TB_ENC_NAME];
                    size_t nl = 0;
                    while (s < e && (p[s] == ' ' || p[s] == '\t'))
                        s++;
                    t = s;
                    while (t < e && (p[t] == '-' || p[t] == '_' || p[t] == '.' ||
                                     (p[t] >= '0' && p[t] <= '9') ||
                                     ((p[t] | 0x20) >= 'a' && (p[t] | 0x20) <= 'z')))
                        t++;
                    if (t == s)
                        continue;
                    for (; s < t && nl + 1 < sizeof(name); s++) {
                        char c = p[s];
                        name[nl++] = (c == '_') ? '-' : (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                    }
                    name[nl] = '\0';
                    if (!strcmp(name, "utf-8") || !strncmp(name, "utf-8-", 6) ||
                        !strcmp(name, "utf8") || !strcmp(name, "u8") ||
                        !strcmp(name, "utf") || !strcmp(name, "cp65001"))
                        return 0;
                    if (bom)
                        return -1;
                    if (!strcmp(name, "latin-1") || !strncmp(name, "latin-1-", 8) ||
                        !strcmp(name, "iso-8859-1") || !strncmp(name, "iso-8859-1-", 11) ||
                        !strcmp(name, "iso-latin-1") || !strncmp(name, "iso-latin-1-", 12) ||
                        !strcmp(name, "latin1") || !strcmp(name, "l1") ||
                        !strcmp(name, "iso8859-1") || !strcmp(name, "8859") ||
                        !strcmp(name, "cp819") || !strcmp(name, "latin"))
                        return 1;
                    if (!strcmp(name, "ascii") || !strcmp(name, "us-ascii") ||
                        !strcmp(name, "646"))
                        return 2;
                    memcpy(other, name, nl + 1);
                    return 3;
                }
            }
        }
        if (!blank)
            break;
        pos = e;
        if (pos < n && p[pos] == '\r')
            pos++;
        if (pos < n && p[pos] == '\n')
            pos++;
    }
    return 0;
}

/* Read and decode a source file; universal newlines.  0 or -1. */
static int qp_tb_read_file(JSContext *ctx, const char *path, QPSrcFile *sf)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    size_t n = 0, size = 0, r, i, o;
    uint32_t *text;
    int enc;
    BOOL bom = FALSE;
    char enc_name[QP_TB_ENC_NAME];

    if (!f)
        return -1;
    for (;;) {
        if (n == size) {
            size_t ns = size ? size * 2 : 8192;
            uint8_t *nd = js_realloc(ctx, data, ns);
            if (!nd) {
                qp_tb_clear_exc(ctx);
                js_free(ctx, data);
                fclose(f);
                return -1;
            }
            data = nd;
            size = ns;
        }
        r = fread(data + n, 1, size - n, f);
        if (r == 0)
            break;
        n += r;
    }
    fclose(f);
    i = 0;
    if (n >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        bom = TRUE;
        i = 3;
    }
    enc = qp_tb_detect_encoding(data + i, n - i, bom, enc_name);
    text = js_malloc(ctx, (n - i + 1) * sizeof(uint32_t));
    if (!text || enc < 0) {
        qp_tb_clear_exc(ctx);
        js_free(ctx, text);
        js_free(ctx, data);
        return -1;
    }
    if (enc == 0) {
        if (qp_tb_decode_utf8(data + i, n - i, text, &o) < 0)
            goto fail;
    } else if (enc == 3) {
        /* the codec registry (tokenizer: PyUnicode_Decode) */
        JSValue b = qp_bytes_new(ctx, data + i, n - i), s;
        uint32_t *u;
        int un;
        if (JS_IsException(b))
            goto fail_exc;
        s = qp_codec_decode(ctx, b, enc_name, "strict");
        JS_FreeValue(ctx, b);
        if (JS_IsException(s))
            goto fail_exc;
        if (!qp_str_check(ctx, s)) {
            JS_FreeValue(ctx, s);
            goto fail;
        }
        un = JS_ToUTF32String(ctx, &u, s);
        JS_FreeValue(ctx, s);
        if (un < 0)
            goto fail_exc;
        js_free(ctx, text);
        text = u;
        o = un;
    } else {
        for (o = 0; i < n; i++) {
            if (enc == 2 && data[i] >= 0x80)
                goto fail;
            text[o++] = data[i];
        }
    }
    js_free(ctx, data);
    /* universal newlines: \r\n and \r -> \n */
    {
        size_t k, w = 0;
        for (k = 0; k < o; k++) {
            if (text[k] == '\r') {
                text[w++] = '\n';
                if (k + 1 < o && text[k + 1] == '\n')
                    k++;
            } else {
                text[w++] = text[k];
            }
        }
        o = w;
    }
    sf->text = text;
    sf->len = o;
    return 0;
 fail_exc:
    qp_tb_clear_exc(ctx);
 fail:
    js_free(ctx, text);
    js_free(ctx, data);
    return -1;
}

/* Split a decoded file into linecache lines ('\n' terminated). */
static void qp_tb_split_file_lines(JSContext *ctx, QPSrcFile *sf)
{
    size_t i, start = 0, count = 0, size = 0;
    for (i = 0; i <= sf->len; i++) {
        if (i == sf->len && start == sf->len)
            break;
        if (i == sf->len || sf->text[i] == '\n') {
            if (count == size) {
                size_t ns = size ? size * 2 : 64;
                QPLine *nl = js_realloc(ctx, sf->lines, ns * sizeof(QPLine));
                if (!nl) {
                    qp_tb_clear_exc(ctx);
                    break;
                }
                sf->lines = nl;
                size = ns;
            }
            sf->lines[count].start = start;
            sf->lines[count].end = i;
            count++;
            start = i + 1;
        }
    }
    sf->nlines = count;
}

/* Load the source lines of `filename` (linecache.getlines semantics:
   "<...>" pseudo files only from registered sources; relative names are
   also searched along sys.path). */
static void qp_tb_load_source(JSContext *ctx, QPSrcFile *sf)
{
    size_t plen;
    const char *path;
    JSValue reg, text;
    int r;

    if (!qp_str_check(ctx, sf->filename))
        return;
    path = qp_str_utf8(ctx, sf->filename, &plen);
    if (!path) {
        qp_tb_clear_exc(ctx);
        return;
    }
    if (path[0] == '<') {
        /* linecache._getline_from_code: sources registered by the launcher */
        reg = qp_err_private_dict(ctx, "\x01qp_linecache");
        if (!JS_IsException(reg)) {
            r = qp_dict_get_item(ctx, reg, sf->filename, &text);
            if (r > 0) {
                uint32_t *b;
                int n = JS_ToUTF32String(ctx, &b, text);
                JS_FreeValue(ctx, text);
                if (n >= 0) {
                    size_t cnt, i;
                    QPLine *ls = qp_text_splitlines(ctx, b, n, &cnt, NULL);
                    sf->text = b;
                    sf->len = n;
                    sf->lines = ls;
                    sf->nlines = ls ? cnt : 0;
                    (void)i;
                }
            }
            JS_FreeValue(ctx, reg);
        }
        qp_tb_clear_exc(ctx);
        JS_FreeCString(ctx, path);
        return;
    }
    if (plen == 0 || plen != strlen(path)) {
        JS_FreeCString(ctx, path);
        return;
    }
    if (qp_tb_read_file(ctx, path, sf) < 0 && path[0] != '/') {
        /* updatecache(): try every sys.path entry */
        JSAtom a = JS_NewAtom(ctx, "path");
        JSValue sys_path = a != JS_ATOM_NULL ? qp_sys_get(ctx, a) : QP_NULL;
        Py_ssize_t n, i;
        JSValue *items;
        if (a != JS_ATOM_NULL)
            JS_FreeAtom(ctx, a);
        if (!JS_IsException(sys_path) && !QP_IS_NULL(sys_path) &&
            (items = qp_list_items(sys_path, &n)) != NULL) {
            for (i = 0; i < n; i++) {
                const char *dir;
                size_t dlen;
                char *full;
                if (!qp_str_check_exact(items[i]))
                    continue;
                dir = qp_str_utf8(ctx, items[i], &dlen);
                if (!dir) {
                    qp_tb_clear_exc(ctx);
                    continue;
                }
                full = js_malloc(ctx, dlen + plen + 2);
                if (full) {
                    if (dlen == 0)
                        memcpy(full, path, plen + 1);
                    else
                        snprintf(full, dlen + plen + 2, "%s%s%s", dir,
                                 dir[dlen - 1] == '/' ? "" : "/", path);
                    r = qp_tb_read_file(ctx, full, sf);
                    js_free(ctx, full);
                } else {
                    r = -1;
                }
                JS_FreeCString(ctx, dir);
                if (r == 0)
                    break;
                /* the entries may have moved: re-read the list */
                items = qp_list_items(sys_path, &n);
                if (!items)
                    break;
            }
        }
        JS_FreeValue(ctx, sys_path);
        qp_tb_clear_exc(ctx);
    }
    JS_FreeCString(ctx, path);
    if (sf->text)
        qp_tb_split_file_lines(ctx, sf);
}

static QPSrcFile *qp_tb_get_source(JSContext *ctx, QPSrcFile **cache, JSValueConst filename)
{
    QPSrcFile *sf;
    for (sf = *cache; sf; sf = sf->next) {
        int eq = qp_str_check(ctx, filename) && qp_str_check(ctx, sf->filename) ?
            qp_str_eq(ctx, filename, sf->filename) : 0;
        if (eq > 0)
            return sf;
        if (eq < 0)
            qp_tb_clear_exc(ctx);
    }
    sf = js_mallocz(ctx, sizeof(*sf));
    if (!sf) {
        qp_tb_clear_exc(ctx);
        return NULL;
    }
    sf->filename = JS_DupValue(ctx, filename);
    qp_tb_load_source(ctx, sf);
    sf->next = *cache;
    *cache = sf;
    return sf;
}

static void qp_tb_free_sources(JSContext *ctx, QPSrcFile *sf)
{
    while (sf) {
        QPSrcFile *n = sf->next;
        JS_FreeValue(ctx, sf->filename);
        js_free(ctx, sf->text);
        js_free(ctx, sf->lines);
        js_free(ctx, sf);
        sf = n;
    }
}

/* linecache.getline(filename, lineno).rstrip(): pointer/length into the
   cached text (length 0 when absent). */
static const uint32_t *qp_tb_getline(JSContext *ctx, QPSrcFile **cache, JSValueConst filename,
                                     int lineno, size_t *plen)
{
    QPSrcFile *sf = qp_tb_get_source(ctx, cache, filename);
    const uint32_t *p;
    size_t n;
    *plen = 0;
    if (!sf || lineno < 1 || (size_t)lineno > sf->nlines)
        return NULL;
    p = sf->text + sf->lines[lineno - 1].start;
    n = sf->lines[lineno - 1].end - sf->lines[lineno - 1].start;
    while (n > 0 && qp_tb_isspace(p[n - 1]))
        n--;
    *plen = n;
    return p;
}

/* The source line `lineno` of `filename` with surrounding whitespace
   stripped (warnings display), QP_NULL when unavailable. */
static JSValue qp_tb_source_line_stripped(JSContext *ctx, JSValueConst filename, int lineno)
{
    QPSrcFile *cache = NULL;
    size_t n;
    const uint32_t *p = qp_tb_getline(ctx, &cache, filename, lineno, &n);
    JSValue r = QP_NULL;
    while (n > 0 && qp_tb_isspace(*p)) {
        p++;
        n--;
    }
    if (p && n > 0)
        r = JS_NewUTF32String(ctx, p, (int)n);
    qp_tb_free_sources(ctx, cache);
    if (JS_IsException(r)) {
        qp_tb_clear_exc(ctx);
        r = QP_NULL;
    }
    return r;
}

/* ========================================================================= */
/* Caret anchors (traceback._extract_caret_anchors_from_line_segment)         */
/* ========================================================================= */

typedef struct QPTBParse {
    qp_error err;
    qp_arena *arena;
    mod_ty mod;
} QPTBParse;

/* ast.parse(src) (exec mode); FALSE on any failure (no exception left). */
static BOOL qp_tb_parse(JSContext *ctx, QPTBParse *ps, const uint32_t *s, size_t n,
                        const char *prefix, const char *suffix)
{
    size_t len, pl = strlen(prefix), sl = strlen(suffix);
    char *body, *src;

    memset(&ps->err, 0, sizeof(ps->err));
    ps->arena = NULL;
    ps->mod = NULL;
    body = qp_tb_to_utf8(ctx, s, n, &len);
    if (!body)
        return FALSE;
    src = js_malloc(ctx, pl + len + sl + 1);
    if (!src) {
        qp_tb_clear_exc(ctx);
        js_free(ctx, body);
        return FALSE;
    }
    memcpy(src, prefix, pl);
    memcpy(src + pl, body, len);
    memcpy(src + pl + len, suffix, sl + 1);
    js_free(ctx, body);
    len += pl + sl;
    if (memchr(src, '\0', len)) {
        js_free(ctx, src);
        return FALSE;
    }
    ps->arena = qp_arena_new(ctx, &ps->err);
    if (ps->arena)
        ps->mod = qp_parse_string(ctx, ps->arena, src, len, "<unknown>", JS_UNDEFINED,
                                  QP_INPUT_FILE, QP_PF_SOURCE_IS_UTF8, &ps->err);
    js_free(ctx, src);
    qp_tb_clear_exc(ctx);
    return ps->mod != NULL && ps->mod->kind == Module_kind;
}

static void qp_tb_parse_free(QPTBParse *ps)
{
    if (ps->arena)
        qp_arena_free(ps->arena);
    qp_error_clear(&ps->err);
}

typedef struct QPAnchors {
    int left_end_lineno;
    Py_ssize_t left_end_offset;
    int right_start_lineno;
    Py_ssize_t right_start_offset;
    uint32_t primary_char, secondary_char;
} QPAnchors;

/* segment.splitlines() view */
typedef struct QPSegLines {
    const uint32_t *s;
    QPLine *lines;
    size_t n;
} QPSegLines;

#define QP_SEG_LEN(sl, i) ((sl)->lines[i].end - (sl)->lines[i].start)
#define QP_SEG_CH(sl, i, c) ((sl)->s[(sl)->lines[i].start + (c)])

static int qp_seg_normalize(QPSegLines *sl, int lineno, Py_ssize_t offset, Py_ssize_t *pcol)
{
    if (lineno < 0 || (size_t)lineno >= sl->n)
        return -1;
    *pcol = qp_tb_byte_to_char(sl->s + sl->lines[lineno].start, QP_SEG_LEN(sl, lineno), offset);
    return 0;
}

static int qp_seg_next_valid(QPSegLines *sl, int *plineno, Py_ssize_t *pcol)
{
    while ((size_t)*plineno < sl->n && *pcol >= (Py_ssize_t)QP_SEG_LEN(sl, *plineno)) {
        *pcol = 0;
        (*plineno)++;
    }
    if (!((size_t)*plineno < sl->n && *pcol < (Py_ssize_t)QP_SEG_LEN(sl, *plineno)))
        return -1;      /* AssertionError */
    return 0;
}

/* increment_until(): stop 0 = non-space and not ')', 1 = '[', 2 = '(' */
static int qp_seg_increment_until(QPSegLines *sl, int *plineno, Py_ssize_t *pcol, int stop)
{
    for (;;) {
        uint32_t ch;
        BOOL done;
        if (*plineno < 0 || (size_t)*plineno >= sl->n ||
            *pcol < 0 || *pcol >= (Py_ssize_t)QP_SEG_LEN(sl, *plineno))
            return -1;
        ch = QP_SEG_CH(sl, *plineno, *pcol);
        if (ch == '\\' || ch == '#') {
            *pcol = 0;
            (*plineno)++;
            if (qp_seg_next_valid(sl, plineno, pcol) < 0)
                return -1;
            continue;
        }
        if (stop == 0)
            done = !qp_tb_isspace(ch) && ch != ')';
        else
            done = ch == (stop == 1 ? '[' : '(');
        if (done)
            return 0;
        (*pcol)++;
        if (qp_seg_next_valid(sl, plineno, pcol) < 0)
            return -1;
    }
}

static int qp_seg_setup_positions(QPSegLines *sl, expr_ty e, BOOL force_valid,
                                  int *plineno, Py_ssize_t *pcol)
{
    *plineno = e->end_lineno - 2;
    if (qp_seg_normalize(sl, *plineno, e->end_col_offset, pcol) < 0)
        return -1;
    if (force_valid)
        return qp_seg_next_valid(sl, plineno, pcol);
    return 0;
}

/* 1 anchors found, 0 none */
static int qp_tb_extract_anchors(JSContext *ctx, const uint32_t *seg, size_t seg_len,
                                 QPAnchors *an)
{
    QPTBParse ps;
    QPSegLines sl;
    stmt_ty st;
    expr_ty e;
    int ret = 0, lineno, rl;
    Py_ssize_t col, rc;

    sl.s = seg;
    sl.lines = NULL;
    sl.n = 0;
    an->primary_char = '~';
    an->secondary_char = '^';
    if (!qp_tb_parse(ctx, &ps, seg, seg_len, "(\n", "\n)"))
        goto done;
    if (asdl_seq_LEN(ps.mod->v.Module.body) != 1)
        goto done;
    sl.lines = qp_text_splitlines(ctx, seg, seg_len, &sl.n, NULL);
    st = asdl_seq_GET(ps.mod->v.Module.body, 0);
    if (st->kind != Expr_kind)
        goto done;
    e = st->v.Expr.value;
    switch (e->kind) {
    case BinOp_kind: {
        expr_ty right = e->v.BinOp.right;
        Py_ssize_t right_col;
        if (qp_seg_setup_positions(&sl, e->v.BinOp.left, TRUE, &lineno, &col) < 0 ||
            qp_seg_increment_until(&sl, &lineno, &col, 0) < 0)
            goto done;
        right_col = col + 1;
        if (right_col < (Py_ssize_t)QP_SEG_LEN(&sl, lineno)) {
            BOOL before_right;
            if (right->lineno - 2 > lineno) {
                before_right = TRUE;
            } else {
                if (qp_seg_normalize(&sl, right->lineno - 2, right->col_offset, &rc) < 0)
                    goto done;
                before_right = right_col < rc;
            }
            if (before_right) {
                uint32_t ch = QP_SEG_CH(&sl, lineno, right_col);
                if (!qp_tb_isspace(ch) && ch != '\\' && ch != '#')
                    right_col++;
            }
        }
        an->left_end_lineno = lineno;
        an->left_end_offset = col;
        an->right_start_lineno = lineno;
        an->right_start_offset = right_col;
        ret = 1;
        break;
    }
    case Subscript_kind:
    case Call_kind: {
        expr_ty inner = e->kind == Subscript_kind ? e->v.Subscript.value : e->v.Call.func;
        if (qp_seg_setup_positions(&sl, inner, TRUE, &lineno, &col) < 0 ||
            qp_seg_increment_until(&sl, &lineno, &col, e->kind == Subscript_kind ? 1 : 2) < 0 ||
            qp_seg_setup_positions(&sl, e, FALSE, &rl, &rc) < 0)
            goto done;
        an->left_end_lineno = lineno;
        an->left_end_offset = col;
        an->right_start_lineno = rl;
        an->right_start_offset = rc;
        ret = 1;
        break;
    }
    default:
        break;
    }
 done:
    js_free(ctx, sl.lines);
    qp_tb_parse_free(&ps);
    return ret;
}

/* StackSummary._should_show_carets */
static BOOL qp_tb_should_show_carets(JSContext *ctx, Py_ssize_t start_offset,
                                     Py_ssize_t end_offset, const uint32_t *text,
                                     QPLine *all_lines, size_t n, BOOL has_anchors)
{
    QPText joined;
    QPTBParse ps;
    size_t i;
    BOOL parsed;
    const uint32_t *l0, *ll;
    size_t l0n, lln;
    Py_ssize_t k;

    qp_text_init(&joined, ctx);
    for (i = 0; i < n; i++) {
        if (i)
            qp_text_putc(&joined, '\n');
        qp_text_put(&joined, text + all_lines[i].start, all_lines[i].end - all_lines[i].start);
    }
    parsed = !joined.oom && qp_tb_parse(ctx, &ps, joined.buf, joined.len, "", "");
    qp_text_free(&joined);
    if (parsed) {
        asdl_stmt_seq *body = ps.mod->v.Module.body;
        expr_ty value = NULL;
        if (asdl_seq_LEN(body) == 0) {
            qp_tb_parse_free(&ps);
            return FALSE;
        } else {
            stmt_ty st = asdl_seq_GET(body, 0);
            if (st->kind == Return_kind && st->v.Return.value &&
                st->v.Return.value->kind == Call_kind &&
                st->v.Return.value->v.Call.func->kind == Name_kind) {
                value = st->v.Return.value;
            } else if (st->kind == Assign_kind && st->v.Assign.value->kind == Call_kind &&
                       asdl_seq_LEN(st->v.Assign.targets) == 1 &&
                       asdl_seq_GET(st->v.Assign.targets, 0)->kind == Name_kind) {
                value = st->v.Assign.value;
            }
            if (value && value->lineno == 1 && value->end_lineno == (int)n &&
                value->col_offset == start_offset && value->end_col_offset == end_offset) {
                qp_tb_parse_free(&ps);
                return FALSE;
            }
        }
    }
    qp_tb_parse_free(&ps);
    if (has_anchors)
        return TRUE;
    if (n == 0)
        return FALSE;
    l0 = text + all_lines[0].start;
    l0n = all_lines[0].end - all_lines[0].start;
    ll = text + all_lines[n - 1].start;
    lln = all_lines[n - 1].end - all_lines[n - 1].start;
    /* all_lines[0][:start_offset].lstrip() or all_lines[-1][end_offset:].rstrip() */
    for (k = 0; k < start_offset && (size_t)k < l0n; k++) {
        if (!qp_tb_isspace(l0[k]))
            return TRUE;
    }
    for (k = end_offset < 0 ? 0 : end_offset; (size_t)k < lln; k++) {
        if (!qp_tb_isspace(ll[k]))
            return TRUE;
    }
    return FALSE;
}

/* ========================================================================= */
/* Frame summaries and their formatting                                       */
/* ========================================================================= */

typedef struct QPFrameSum {
    JSValue filename;   /* co_filename */
    JSValue name;       /* co_name */
    int lineno, end_lineno, colno, end_colno;   /* QP_TB_NONE = None */
} QPFrameSum;

typedef struct QPTE QPTE;

/* _colorize.Traceback theme fields */
enum {
    QP_TH_TYPE, QP_TH_MESSAGE, QP_TH_NOTE, QP_TH_FILENAME, QP_TH_LINE_NO, QP_TH_FRAME,
    QP_TH_ERROR_HIGHLIGHT, QP_TH_ERROR_RANGE, QP_TH_RESET, QP_TH__COUNT
};

typedef struct QPTBState {
    JSContext *ctx;
    QPSrcFile *sources;
    QPText out;
    int group_depth;            /* _ExceptionPrintContext.exception_group_depth */
    BOOL need_close;
    JSObject **seen;            /* _seen (identity) */
    int nseen, seen_size;
    QPTE **tes;                 /* every TracebackException node, for freeing */
    int ntes, tes_size;
    int max_group_width, max_group_depth;
    BOOL colorize;              /* _print_exception_bltin: _colorize.can_colorize(file=file) */
    JSValue theme[QP_TH__COUNT];    /* get_theme(force_color=True).traceback (str), when colorize */
} QPTBState;

/* theme.<field> of a colorized output; the no-colour theme is all "" */
static void qp_tb_th(QPText *t, const QPTBState *st, int field)
{
    if (st->colorize && qp_text_put_str(t, st->theme[field]) < 0)
        qp_tb_clear_exc(st->ctx);
}

/* One element of _zip_display_width(line, carets): the next character of
   line (a code point with the zero-width ones after it, the grapheme
   approximation) at *pi, and the number of carets it takes (its display
   width, fewer when they run out) from *pc.  Returns its length. */
static size_t qp_tb_zip_next(const uint32_t *line, size_t n, size_t *pi, Py_ssize_t ncarets,
                             Py_ssize_t *pc, Py_ssize_t *ptake)
{
    size_t i = *pi, e = i + 1;
    Py_ssize_t w = qp_tb_wlen(line + i, 1);

    while (e < n && qp_tb_char_width(line[e]) == 0 && line[e] != '\n')
        e++;
    if (w > ncarets - *pc)
        w = ncarets - *pc > 0 ? ncarets - *pc : 0;
    *ptake = w;
    *pc += w;
    *pi = e;
    return e - i;
}

/* output_line() with colorize: the line, then the carets, with the runs of
   characters under '^' / '~' carets in error_highlight / error_range
   (itertools.groupby over _zip_display_width(line + "\n", carets)). */
static void qp_tb_output_colored(QPText *res, const QPTBState *st, const uint32_t *line,
                                 size_t len, const uint32_t *carets, Py_ssize_t ncarets)
{
    QPText ln, cr;
    uint32_t *full;
    size_t n = len + 1, i = 0;
    Py_ssize_t c = 0;

    full = js_malloc(st->ctx, n * sizeof(uint32_t));
    if (!full) {
        qp_tb_clear_exc(st->ctx);
        res->oom = TRUE;
        return;
    }
    memcpy(full, line, len * sizeof(uint32_t));
    full[len] = '\n';
    qp_text_init(&ln, st->ctx);
    qp_text_init(&cr, st->ctx);
    while (i < n) {
        /* one group: consecutive characters with equal caret strings */
        size_t gs = i, k;
        Py_ssize_t cs = c, take, gtake;
        BOOL hl = FALSE, rng = FALSE;
        qp_tb_zip_next(full, n, &i, ncarets, &c, &take);
        gtake = take;
        for (;;) {
            size_t save_i = i;
            Py_ssize_t save_c = c, t2;
            if (i >= n)
                break;
            qp_tb_zip_next(full, n, &i, ncarets, &c, &t2);
            if (t2 != take || memcmp(carets + save_c, carets + cs, t2 * sizeof(uint32_t))) {
                i = save_i;
                c = save_c;
                break;
            }
            gtake += t2;
        }
        for (k = 0; k < (size_t)take; k++) {
            if (carets[cs + k] == '^')
                hl = TRUE;
            else if (carets[cs + k] == '~')
                rng = TRUE;
        }
        if (hl || rng) {
            int field = hl ? QP_TH_ERROR_HIGHLIGHT : QP_TH_ERROR_RANGE;
            qp_tb_th(&ln, st, field);
            qp_text_put(&ln, full + gs, i - gs);
            qp_tb_th(&ln, st, QP_TH_RESET);
            qp_tb_th(&cr, st, field);
            qp_text_put(&cr, carets + cs, gtake);
            qp_tb_th(&cr, st, QP_TH_RESET);
        } else {
            qp_text_put(&ln, full + gs, i - gs);
            qp_text_put(&cr, carets + cs, gtake);
        }
    }
    qp_text_put(res, ln.buf, ln.len);
    qp_text_put(res, cr.buf, cr.len);
    qp_text_putc(res, '\n');
    if (ln.oom || cr.oom)
        res->oom = TRUE;
    qp_text_free(&ln);
    qp_text_free(&cr);
    js_free(st->ctx, full);
}

static void qp_tb_put_lines_joined(QPText *t, const uint32_t *text, QPLine *lines, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (i)
            qp_text_putc(t, '\n');
        qp_text_put(t, text + lines[i].start, lines[i].end - lines[i].start);
    }
}

/* One caret line of output_line(). */
static void qp_tb_output_line(QPText *res, const QPTBState *st, const uint32_t *text,
                              QPLine *all_lines, size_t n,
                              size_t lineno, BOOL show_carets, Py_ssize_t dp_start_offset,
                              Py_ssize_t dp_end_offset, const QPAnchors *an,
                              Py_ssize_t a_left, Py_ssize_t a_right)
{
    const uint32_t *line = text + all_lines[lineno].start;
    size_t len = all_lines[lineno].end - all_lines[lineno].start, sp = 0;
    Py_ssize_t num_carets, col;
    QPText carets;

    if (!show_carets) {
        qp_text_put(res, line, len);
        qp_text_putc(res, '\n');
        return;
    }
    while (sp < len && qp_tb_isspace(line[sp]))
        sp++;
    num_carets = lineno == n - 1 ? dp_end_offset : qp_tb_wlen(line, len);
    qp_text_init(&carets, st->ctx);
    for (col = 0; col < num_carets; col++) {
        if (col < (Py_ssize_t)sp || (lineno == 0 && col < dp_start_offset)) {
            qp_text_putc(&carets, ' ');
        } else if (an &&
                   ((int)lineno > an->left_end_lineno ||
                    ((int)lineno == an->left_end_lineno && col >= a_left)) &&
                   ((int)lineno < an->right_start_lineno ||
                    ((int)lineno == an->right_start_lineno && col < a_right))) {
            qp_text_putc(&carets, an->secondary_char);
        } else {
            qp_text_putc(&carets, an ? an->primary_char : '^');
        }
    }
    if (carets.oom) {
        res->oom = TRUE;
    } else if (st->colorize) {
        qp_tb_output_colored(res, st, line, len, carets.buf, carets.len);
    } else {
        qp_text_put(res, line, len);
        qp_text_putc(res, '\n');
        qp_text_put(res, carets.buf, carets.len);
        qp_text_putc(res, '\n');
    }
    qp_text_free(&carets);
}

/* StackSummary.format_frame_summary (no locals) */
static void qp_tb_format_frame(QPTBState *st, QPText *row, QPFrameSum *f)
{
    JSContext *ctx = st->ctx;
    QPText orig, ded, seg, res, ded_res;
    QPLine *orig_lines = NULL, *all_lines = NULL;
    size_t n_orig = 0, n_all = 0, i;
    int ln;
    static const uint32_t four_spaces[4] = { ' ', ' ', ' ', ' ' };

    qp_text_init(&orig, ctx);
    qp_text_init(&ded, ctx);
    qp_text_init(&seg, ctx);
    qp_text_init(&res, ctx);
    qp_text_init(&ded_res, ctx);

    /* '  File {}"{}"{}, line {}{}{}, in {}{}{}\n' */
    qp_text_puts(row, "  File ");
    qp_tb_th(row, st, QP_TH_FILENAME);
    qp_text_putc(row, '"');
    if (!qp_str_check(ctx, f->filename)) {
        qp_text_put_safe_str(row, f->filename, FALSE, "filename");
    } else {
        uint32_t *fb;
        int fn = JS_ToUTF32String(ctx, &fb, f->filename);
        if (fn < 0) {
            qp_tb_clear_exc(ctx);
        } else {
            static const uint32_t stdin_pfx[7] = { '<', 's', 't', 'd', 'i', 'n', '-' };
            if (fn >= 8 && !memcmp(fb, stdin_pfx, sizeof(stdin_pfx)) && fb[fn - 1] == '>')
                qp_text_puts(row, "<stdin>");
            else
                qp_text_put(row, fb, fn);
            js_free(ctx, fb);
        }
    }
    qp_text_putc(row, '"');
    qp_tb_th(row, st, QP_TH_RESET);
    qp_text_puts(row, ", line ");
    qp_tb_th(row, st, QP_TH_LINE_NO);
    if (f->lineno == QP_TB_NONE)
        qp_text_puts(row, "None");
    else
        qp_text_printf(row, "%d", f->lineno);
    qp_tb_th(row, st, QP_TH_RESET);
    qp_text_puts(row, ", in ");
    qp_tb_th(row, st, QP_TH_FRAME);
    qp_text_put_safe_str(row, f->name, FALSE, "name");
    qp_tb_th(row, st, QP_TH_RESET);
    qp_text_putc(row, '\n');

    if (f->lineno == QP_TB_NONE || f->end_lineno == QP_TB_NONE)
        goto done;
    /* _set_lines: lines lineno..end_lineno, rstripped, "\n".join + "\n" */
    for (ln = f->lineno; ln <= f->end_lineno; ln++) {
        size_t l;
        const uint32_t *p = qp_tb_getline(ctx, &st->sources, f->filename, ln, &l);
        if (ln != f->lineno)
            qp_text_putc(&orig, '\n');
        if (p)
            qp_text_put(&orig, p, l);
    }
    qp_text_putc(&orig, '\n');
    qp_text_dedent(&ded, orig.buf, orig.len);
    if (orig.oom || ded.oom || qp_tb_all_space(ded.buf, ded.len))
        goto done;

    if (f->colno == QP_TB_NONE || f->end_colno == QP_TB_NONE) {
        /* only the first line, stripped */
        size_t s = 0, e = 0;
        while (e < orig.len && orig.buf[e] != '\n')
            e++;
        while (s < e && qp_tb_isspace(orig.buf[s]))
            s++;
        while (e > s && qp_tb_isspace(orig.buf[e - 1]))
            e--;
        if (e > s) {
            qp_text_puts(row, "    ");
            qp_text_put(row, orig.buf + s, e - s);
        }
        qp_text_putc(row, '\n');
        goto done;
    }

    orig_lines = qp_text_splitlines(ctx, orig.buf, orig.len, &n_orig, NULL);
    all_lines = qp_text_splitlines(ctx, ded.buf, ded.len, &n_all, NULL);
    if (!orig_lines || !all_lines || (size_t)(f->end_lineno - f->lineno) >= n_orig)
        goto done;
    if (n_all > (size_t)(f->end_lineno - f->lineno + 1))
        n_all = f->end_lineno - f->lineno + 1;
    {
        const uint32_t *first = orig.buf + orig_lines[0].start;
        size_t first_n = orig_lines[0].end - orig_lines[0].start;
        QPLine *ll = &orig_lines[f->end_lineno - f->lineno];
        Py_ssize_t start_offset, end_offset, dedent_chars, dp_start, dp_end;
        Py_ssize_t seg_total, seg_stop, last_len;
        const uint32_t *a0 = ded.buf + all_lines[0].start;
        size_t a0n = all_lines[0].end - all_lines[0].start;
        const uint32_t *al = ded.buf + all_lines[n_all - 1].start;
        size_t aln = all_lines[n_all - 1].end - all_lines[n_all - 1].start;
        QPAnchors an;
        BOOL has_anchors, show_carets;
        Py_ssize_t a_left = 0, a_right = 0;
        BOOL *sig;
        size_t prev;
        BOOL have_prev = FALSE;

        start_offset = qp_tb_byte_to_char(first, first_n, f->colno);
        end_offset = qp_tb_byte_to_char(orig.buf + ll->start, ll->end - ll->start, f->end_colno);
        dedent_chars = (Py_ssize_t)first_n - (Py_ssize_t)a0n;
        start_offset = max_int64(0, start_offset - dedent_chars);
        end_offset = max_int64(0, end_offset - dedent_chars);
        dp_start = qp_tb_display_width(a0, a0n, start_offset);
        dp_end = qp_tb_display_width(al, aln, end_offset);

        /* segment = "\n".join(all_lines)[start_offset:len - (len(last) - end_offset)] */
        qp_tb_put_lines_joined(&seg, ded.buf, all_lines, n_all);
        if (seg.oom)
            goto done;
        seg_total = seg.len;
        last_len = aln;
        seg_stop = seg_total - (last_len - end_offset);
        if (seg_stop > seg_total)
            seg_stop = seg_total;
        if (seg_stop < 0)
            seg_stop = 0;
        {
            Py_ssize_t s0 = start_offset > seg_total ? seg_total : start_offset;
            Py_ssize_t slen = seg_stop > s0 ? seg_stop - s0 : 0;
            has_anchors = qp_tb_extract_anchors(ctx, seg.buf + s0, slen, &an) == 1;
        }
        show_carets = qp_tb_should_show_carets(ctx, start_offset, end_offset, ded.buf,
                                               all_lines, n_all, has_anchors);

        sig = js_mallocz(ctx, n_all * sizeof(BOOL));
        if (!sig) {
            qp_tb_clear_exc(ctx);
            goto done;
        }
        sig[0] = TRUE;
        sig[n_all - 1] = TRUE;
        if (has_anchors) {
            int k;
            a_left = an.left_end_offset;
            a_right = an.right_start_offset;
            if (an.left_end_lineno == 0)
                a_left += start_offset;
            if (an.right_start_lineno == 0)
                a_right += start_offset;
            if (an.left_end_lineno >= 0 && (size_t)an.left_end_lineno < n_all) {
                QPLine *l = &all_lines[an.left_end_lineno];
                a_left = qp_tb_display_width(ded.buf + l->start, l->end - l->start, a_left);
            }
            if (an.right_start_lineno >= 0 && (size_t)an.right_start_lineno < n_all) {
                QPLine *l = &all_lines[an.right_start_lineno];
                a_right = qp_tb_display_width(ded.buf + l->start, l->end - l->start, a_right);
            }
            for (k = an.left_end_lineno - 1; k < an.left_end_lineno + 2; k++) {
                if (k >= 0 && (size_t)k < n_all)
                    sig[k] = TRUE;
            }
            for (k = an.right_start_lineno - 1; k < an.right_start_lineno + 2; k++) {
                if (k >= 0 && (size_t)k < n_all)
                    sig[k] = TRUE;
            }
        }
        prev = 0;
        for (i = 0; i < n_all; i++) {
            if (!sig[i])
                continue;
            if (have_prev) {
                size_t diff = i - prev;
                if (diff == 2)
                    qp_tb_output_line(&res, st, ded.buf, all_lines, n_all, i - 1, show_carets,
                                      dp_start, dp_end, has_anchors ? &an : NULL, a_left, a_right);
                else if (diff > 2)
                    qp_text_printf(&res, "...<%d lines>...\n", (int)(diff - 1));
            }
            qp_tb_output_line(&res, st, ded.buf, all_lines, n_all, i, show_carets,
                              dp_start, dp_end, has_anchors ? &an : NULL, a_left, a_right);
            prev = i;
            have_prev = TRUE;
        }
        js_free(ctx, sig);
        qp_text_dedent(&ded_res, res.buf, res.len);
        qp_text_indent_all(row, ded_res.buf, ded_res.len, four_spaces, 4);
    }
 done:
    js_free(ctx, orig_lines);
    js_free(ctx, all_lines);
    qp_text_free(&orig);
    qp_text_free(&ded);
    qp_text_free(&seg);
    qp_text_free(&res);
    qp_text_free(&ded_res);
}

static void qp_tb_free_frames(JSContext *ctx, QPFrameSum *frames, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        JS_FreeValue(ctx, frames[i].filename);
        JS_FreeValue(ctx, frames[i].name);
    }
    js_free(ctx, frames);
}

/* sys.tracebacklimit for BUILTIN_EXCEPTION_LIMIT: -1 = no limit. */
static int qp_tb_builtin_limit(JSContext *ctx)
{
    JSAtom a = JS_NewAtom(ctx, "tracebacklimit");
    JSValue v;
    int64_t x;
    int limit = -1;
    if (a == JS_ATOM_NULL) {
        qp_tb_clear_exc(ctx);
        return -1;
    }
    v = qp_sys_get(ctx, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(v)) {
        qp_tb_clear_exc(ctx);
        return -1;
    }
    if (!QP_IS_NULL(v) && !JS_IsUndefined(v) && qp_long_check(ctx, v)) {
        JSValue iv = qp_long_value(ctx, v);
        int ov = 0;
        if (!JS_IsException(iv) && qp_long_as_int64_overflow(iv, &x, &ov) == 0) {
            if (ov > 0)
                limit = INT_MAX;
            else if (ov < 0 || x < 0)
                limit = 0;
            else
                limit = x > INT_MAX ? INT_MAX : (int)x;
        }
        JS_FreeValue(ctx, iv);
        qp_tb_clear_exc(ctx);
    }
    JS_FreeValue(ctx, v);
    return limit;
}

/* StackSummary._extract_from_extended_frame_gen(_walk_tb_with_full_positions(tb),
   limit=BUILTIN_EXCEPTION_LIMIT) */
static int qp_tb_extract(QPTBState *st, JSValueConst tb, QPFrameSum **pframes)
{
    JSContext *ctx = st->ctx;
    JSValueConst cur;
    int count = 0, skip, limit, i = 0;
    QPFrameSum *frames;

    *pframes = NULL;
    for (cur = tb; qp_tb_check(cur); cur = qp_tb_data(cur)->next)
        count++;
    if (count == 0)
        return 0;
    limit = qp_tb_builtin_limit(ctx);
    skip = (limit >= 0 && limit < count) ? count - limit : 0;
    frames = js_mallocz(ctx, sizeof(QPFrameSum) * (count - skip + 1));
    if (!frames) {
        qp_tb_clear_exc(ctx);
        return 0;
    }
    for (cur = tb; qp_tb_check(cur); cur = qp_tb_data(cur)->next) {
        QPTraceback *t = qp_tb_data(cur);
        JSFunctionBytecode *b;
        QPLoc loc = { -1, -1, -1, -1 };
        QPFrameSum *f;

        if (skip > 0) {
            skip--;
            continue;
        }
        f = &frames[i];
        b = qp_tb_bytecode(ctx, t);
        if (b) {
            f->filename = qp_str_from_atom(ctx, b->has_debug ? b->debug.filename
                                                             : JS_ATOM_empty_string);
            f->name = qp_str_from_atom(ctx, b->func_name);
            int pc = qp_tb_pc(t, b);
            if (pc >= 0 && qp_code_addr2loc(b, (uint32_t)pc, &loc) < 0) {
                loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
            }
        } else {
            /* frame without Python bytecode: f_code.co_filename / co_name */
            JSAtom a = JS_NewAtom(ctx, "f_code");
            JSValue code = QP_IS_NULL(t->frame) || a == JS_ATOM_NULL ? JS_UNDEFINED :
                qp_getattr(ctx, t->frame, a);
            JSAtom fa = JS_NewAtom(ctx, "co_filename"), na = JS_NewAtom(ctx, "co_name");
            f->filename = JS_IsException(code) || JS_IsUndefined(code) ? JS_UNDEFINED :
                qp_getattr(ctx, code, fa);
            f->name = JS_IsException(code) || JS_IsUndefined(code) ? JS_UNDEFINED :
                qp_getattr(ctx, code, na);
            JS_FreeValue(ctx, code);
            JS_FreeAtom(ctx, a);
            JS_FreeAtom(ctx, fa);
            JS_FreeAtom(ctx, na);
        }
        if (JS_IsException(f->filename) || JS_IsException(f->name)) {
            qp_tb_clear_exc(ctx);
            if (JS_IsException(f->filename))
                f->filename = JS_UNDEFINED;
            if (JS_IsException(f->name))
                f->name = JS_UNDEFINED;
        }
        f->lineno = loc.lineno >= 0 ? loc.lineno : qp_tb_lineno(ctx, t);
        f->end_lineno = loc.end_lineno >= 0 ? loc.end_lineno : f->lineno;
        f->colno = loc.col >= 0 ? loc.col : QP_TB_NONE;
        f->end_colno = loc.end_col >= 0 ? loc.end_col : QP_TB_NONE;
        i++;
    }
    *pframes = frames;
    return i;
}

/* ========================================================================= */
/* TracebackException                                                         */
/* ========================================================================= */

struct QPTE {
    JSValue exc;
    JSValue type_str;           /* exc_type_str */
    JSValue str;                /* _str */
    JSValue notes;              /* __notes__ (QP_NULL = None) */
    BOOL notes_failed;          /* notes is the "Ignored error getting __notes__" str */
    BOOL suppress_context;
    BOOL is_syntax_error;
    JSValue se_filename, se_lineno, se_end_lineno, se_text, se_offset, se_end_offset, se_msg;
    QPFrameSum *frames;
    int nframes;
    QPTE *cause, *context;
    QPTE **excs;
    int nexcs;
    BOOL has_excs;
};

static BOOL qp_te_seen(QPTBState *st, JSValueConst v)
{
    int i;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    for (i = 0; i < st->nseen; i++) {
        if (st->seen[i] == JS_VALUE_GET_OBJ(v))
            return TRUE;
    }
    return FALSE;
}

static void qp_te_seen_add(QPTBState *st, JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || qp_te_seen(st, v))
        return;
    if (st->nseen == st->seen_size) {
        int ns = st->seen_size ? st->seen_size * 2 : 16;
        JSObject **n = js_realloc(st->ctx, st->seen, ns * sizeof(JSObject *));
        if (!n) {
            qp_tb_clear_exc(st->ctx);
            return;
        }
        st->seen = n;
        st->seen_size = ns;
    }
    st->seen[st->nseen++] = JS_VALUE_GET_OBJ(v);
}

/* getattr(obj, name) with a C name: new ref, QP_NULL when the attribute is
   missing (AttributeError), JS_EXCEPTION on other errors. */
static JSValue qp_te_getattr_opt(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue v;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr_opt(ctx, obj, a, &v);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    return r ? v : QP_NULL;
}

/* getattr(obj, name, None): errors become None. */
static JSValue qp_te_getattr_none(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue v = qp_te_getattr_opt(ctx, obj, name);
    if (JS_IsException(v)) {
        qp_tb_clear_exc(ctx);
        return JS_UNDEFINED;
    }
    return QP_IS_NULL(v) ? JS_UNDEFINED : v;
}

static BOOL qp_te_str_isascii(JSContext *ctx, JSValueConst s)
{
    uint32_t *b;
    int i, n = JS_ToUTF32String(ctx, &b, s);
    BOOL r = TRUE;
    if (n < 0) {
        qp_tb_clear_exc(ctx);
        return TRUE;
    }
    for (i = 0; i < n; i++) {
        if (b[i] >= 0x80) {
            r = FALSE;
            break;
        }
    }
    js_free(ctx, b);
    return r;
}

/* `s in seq_of_str` by str equality (-1 error) */
static int qp_te_str_in_list(JSContext *ctx, JSValueConst list, JSValueConst s)
{
    Py_ssize_t n, i;
    JSValue *items = qp_list_items(list, &n);
    if (!items)
        return qp_contains(ctx, list, s);
    for (i = 0; i < n; i++) {
        if (qp_str_check(ctx, items[i])) {
            int eq = qp_str_eq(ctx, items[i], s);
            if (eq != 0)
                return eq;
        }
    }
    return 0;
}

/* Append the str keys / items of an iterable to list (non-str kept, the
   caller filters); 0 or -1. */
static int qp_te_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable)
{
    if (QP_TB_NONE_OR_NULL(iterable))
        return 0;
    if (qp_dict_check(ctx, iterable)) {
        JSValue keys = qp_dict_keys_list(ctx, iterable);
        int r;
        if (JS_IsException(keys))
            return -1;
        r = qp_list_extend(ctx, list, keys);
        JS_FreeValue(ctx, keys);
        return r;
    }
    return qp_list_extend(ctx, list, iterable);
}

/* list keeping only str items */
static JSValue qp_te_filter_str(JSContext *ctx, JSValueConst list, BOOL hide_underscored,
                                JSValueConst lazy_obj)
{
    JSValue out = qp_list_new(ctx, 0), it, item;
    if (JS_IsException(out))
        return out;
    it = qp_iter(ctx, list);
    if (JS_IsException(it))
        goto fail;
    for (;;) {
        item = qp_next(ctx, it);
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, it);
            goto fail;
        }
        if (qp_str_check(ctx, item)) {
            BOOL keep = TRUE;
            if (hide_underscored) {
                uint32_t *b;
                int n = JS_ToUTF32String(ctx, &b, item);
                if (n < 0) {
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, it);
                    goto fail;
                }
                keep = !(n > 0 && b[0] == '_');
                js_free(ctx, b);
            }
            if (keep && !QP_IS_NULL(lazy_obj) && qp_module_check(ctx, lazy_obj)) {
                /* _is_lazy_import(obj, x) */
                JSValue d = qp_module_get_dict(lazy_obj), v;
                if (JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT) {
                    int r = qp_dict_get_item(ctx, d, item, &v);
                    if (r < 0)
                        qp_tb_clear_exc(ctx);
                    if (r > 0) {
                        if (qp_typecheck(ctx, v, QP_TYPE(ctx, lazy_import)))
                            keep = FALSE;
                        JS_FreeValue(ctx, v);
                    }
                }
            }
            if (keep && qp_list_append(ctx, out, item) < 0) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, it);
                goto fail;
            }
        }
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, it);
    return out;
 fail:
    JS_FreeValue(ctx, out);
    return JS_EXCEPTION;
}

/* _get_safe___dir__(obj) (sorted str names), JS_EXCEPTION on error */
static JSValue qp_te_safe_dir(JSContext *ctx, JSValueConst obj, BOOL hide_underscored)
{
    JSValue d, out;
    d = qp_call_method(ctx, obj, QP_ATOM(ctx, __dir__), NULL, 0, QP_NULL);
    if (JS_IsException(d)) {
        JSValue m;
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            return JS_EXCEPTION;
        qp_tb_clear_exc(ctx);
        m = qp_getattr(ctx, QP_TYPE_VALUE(qp_type_of(ctx, obj)), QP_ATOM(ctx, __dir__));
        if (JS_IsException(m))
            return m;
        d = qp_call1(ctx, m, obj);
        JS_FreeValue(ctx, m);
        if (JS_IsException(d))
            return d;
    }
    out = qp_te_filter_str(ctx, d, hide_underscored, obj);
    JS_FreeValue(ctx, d);
    if (JS_IsException(out))
        return out;
    if (qp_list_sort(ctx, out, QP_NULL, 0) < 0) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

/* Python-level _levenshtein_distance over code points */
static BOOL qp_te_lower_eq(uint32_t a, uint32_t b)
{
    Py_UCS4 la[3], lb[3];
    int na = _PyUnicode_ToLowerFull(a, la), nb = _PyUnicode_ToLowerFull(b, lb);
    return na == nb && !memcmp(la, lb, na * sizeof(Py_UCS4));
}

static Py_ssize_t qp_te_levenshtein(const uint32_t *a, size_t an, const uint32_t *b, size_t bn,
                                    Py_ssize_t max_cost)
{
    size_t row[QP_SUGG_MAX_STRING_SIZE], i, bi;
    Py_ssize_t result = 0;

    if (an == bn && !memcmp(a, b, an * sizeof(uint32_t)))
        return 0;
    while (an && bn && a[0] == b[0]) {
        a++; an--;
        b++; bn--;
    }
    while (an && bn && a[an - 1] == b[bn - 1]) {
        an--;
        bn--;
    }
    if (!an || !bn)
        return QP_SUGG_MOVE_COST * (Py_ssize_t)(an + bn);
    if (an > QP_SUGG_MAX_STRING_SIZE || bn > QP_SUGG_MAX_STRING_SIZE)
        return max_cost + 1;
    if (bn < an) {
        const uint32_t *t = a; a = b; b = t;
        size_t tn = an; an = bn; bn = tn;
    }
    if ((Py_ssize_t)(bn - an) * QP_SUGG_MOVE_COST > max_cost)
        return max_cost + 1;
    for (i = 0; i < an; i++)
        row[i] = QP_SUGG_MOVE_COST * (i + 1);
    for (bi = 0; bi < bn; bi++) {
        uint32_t bchar = b[bi];
        Py_ssize_t distance = result = bi * QP_SUGG_MOVE_COST;
        Py_ssize_t minimum = PY_SSIZE_T_MAX;
        for (i = 0; i < an; i++) {
            Py_ssize_t cost = bchar == a[i] ? 0 :
                qp_te_lower_eq(bchar, a[i]) ? QP_SUGG_CASE_COST : QP_SUGG_MOVE_COST;
            Py_ssize_t substitute = distance + cost, insert_delete;
            distance = row[i];
            insert_delete = (result < distance ? result : distance) + QP_SUGG_MOVE_COST;
            result = insert_delete < substitute ? insert_delete : substitute;
            row[i] = result;
            if (result < minimum)
                minimum = result;
        }
        if (minimum > max_cost)
            return max_cost + 1;
    }
    return result;
}

/* getattr_static(obj, name) is not _sentinel (approximation without
   inspect: instance __dict__, then the MRO of type(obj) / of obj itself
   for classes). */
static BOOL qp_te_has_static_attr(JSContext *ctx, JSValueConst obj, JSValueConst name)
{
    JSAtom a = qp_str_to_atom(ctx, name);
    QPType *t;
    BOOL found = FALSE;
    if (a == JS_ATOM_NULL) {
        qp_tb_clear_exc(ctx);
        return FALSE;
    }
    t = qp_type_from_value(obj);
    if (t && !QP_IS_NULL(qp_type_lookup(ctx, t, a)))
        found = TRUE;
    if (!found && !t) {
        JSValue d;
        int r = qp_getattr_opt(ctx, obj, QP_ATOM(ctx, __dict__), &d);
        if (r > 0) {
            if (qp_anydict_check(ctx, d)) {
                JSValue v;
                int rr = qp_dict_get_item(ctx, d, name, &v);
                if (rr > 0) {
                    found = TRUE;
                    JS_FreeValue(ctx, v);
                }
            }
            JS_FreeValue(ctx, d);
        }
        qp_tb_clear_exc(ctx);
    }
    if (!found && !QP_IS_NULL(qp_type_lookup(ctx, qp_type_of(ctx, obj), a)))
        found = TRUE;
    JS_FreeAtom(ctx, a);
    return found;
}

/* _check_for_nested_attribute(obj, wrong_name, attrs) */
static JSValue qp_te_nested_attribute(JSContext *ctx, JSValueConst obj, JSValueConst wrong_name,
                                      JSValueConst attrs)
{
    Py_ssize_t n, i;
    JSValue *items = qp_list_items(attrs, &n);
    int checked = 0;
    if (!items)
        return QP_NULL;
    for (i = 0; i < n && checked < 20; i++) {
        JSValue attr_name = JS_DupValue(ctx, items[i]), attr_obj, cls_attr, res;
        uint32_t *b;
        int len = JS_ToUTF32String(ctx, &b, attr_name);
        JSAtom a;
        if (len < 0) {
            qp_tb_clear_exc(ctx);
            JS_FreeValue(ctx, attr_name);
            continue;
        }
        if (len > 0 && b[0] == '_') {
            js_free(ctx, b);
            JS_FreeValue(ctx, attr_name);
            continue;
        }
        js_free(ctx, b);
        checked++;
        a = qp_str_to_atom(ctx, attr_name);
        if (a == JS_ATOM_NULL) {
            qp_tb_clear_exc(ctx);
            JS_FreeValue(ctx, attr_name);
            continue;
        }
        /* skip descriptors */
        cls_attr = qp_type_lookup(ctx, qp_type_of(ctx, obj), a);
        if (!QP_IS_NULL(cls_attr) &&
            !QP_IS_NULL(qp_type_lookup(ctx, qp_type_of(ctx, cls_attr), QP_ATOM(ctx, __get__)))) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, attr_name);
            continue;
        }
        /* skip lazy imports */
        if (qp_module_check(ctx, obj)) {
            JSValue d = qp_module_get_dict(obj), v;
            int r = JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT ? qp_dict_get_item(ctx, d, attr_name, &v) : 0;
            BOOL lazy = FALSE;
            if (r > 0) {
                lazy = qp_typecheck(ctx, v, QP_TYPE(ctx, lazy_import));
                JS_FreeValue(ctx, v);
            }
            qp_tb_clear_exc(ctx);
            if (lazy) {
                JS_FreeAtom(ctx, a);
                JS_FreeValue(ctx, attr_name);
                continue;
            }
        }
        attr_obj = qp_getattr(ctx, obj, a);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(attr_obj)) {
            qp_tb_clear_exc(ctx);
            JS_FreeValue(ctx, attr_name);
            continue;
        }
        if (qp_module_check(ctx, attr_obj)) {
            JSValue d = qp_module_get_dict(attr_obj), v;
            int r = JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT ? qp_dict_get_item(ctx, d, wrong_name, &v) : 0;
            BOOL lazy = FALSE;
            if (r > 0) {
                lazy = qp_typecheck(ctx, v, QP_TYPE(ctx, lazy_import));
                JS_FreeValue(ctx, v);
            }
            qp_tb_clear_exc(ctx);
            if (lazy) {
                JS_FreeValue(ctx, attr_obj);
                JS_FreeValue(ctx, attr_name);
                continue;
            }
        }
        if (qp_te_has_static_attr(ctx, attr_obj, wrong_name)) {
            res = qp_str_from_format(ctx, "%U.%U", attr_name, wrong_name);
            JS_FreeValue(ctx, attr_obj);
            JS_FreeValue(ctx, attr_name);
            if (JS_IsException(res)) {
                qp_tb_clear_exc(ctx);
                return QP_NULL;
            }
            return res;
        }
        JS_FreeValue(ctx, attr_obj);
        JS_FreeValue(ctx, attr_name);
    }
    return QP_NULL;
}

/* The innermost traceback entry (or NULL). */
static QPTraceback *qp_te_last_tb(JSValueConst tb)
{
    QPTraceback *t = NULL;
    while (qp_tb_check(tb)) {
        t = qp_tb_data(tb);
        tb = t->next;
    }
    return t;
}

/* frame.f_locals['self'] of a traceback entry, QP_NULL if absent. */
static JSValue qp_te_frame_self(JSContext *ctx, QPTraceback *t)
{
    JSValue frame = qp_tb_frame(ctx, t), loc, key, v = QP_NULL;

    if (JS_IsException(frame)) {
        qp_tb_clear_exc(ctx);
        return QP_NULL;
    }
    loc = qp_te_getattr_none(ctx, frame, "f_locals");
    key = qp_str_from_cstr(ctx, "self");
    if (!JS_IsException(key) && !JS_IsUndefined(loc)) {
        v = qp_getitem(ctx, loc, key);
        if (JS_IsException(v))
            v = QP_NULL;
    }
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, loc);
    qp_tb_clear_exc(ctx);
    return v;
}

/* list(f_locals) + list(f_globals) + list(f_builtins) of a traceback entry */
static int qp_te_frame_names(JSContext *ctx, QPTraceback *t, JSValueConst list)
{
    static const char *const names[3] = { "f_locals", "f_globals", "f_builtins" };
    JSValue frame = qp_tb_frame(ctx, t);
    int i;

    if (JS_IsException(frame))
        return -1;
    for (i = 0; i < 3; i++) {
        JSValue m = qp_te_getattr_opt(ctx, frame, names[i]);
        int r;
        if (JS_IsException(m))
            return -1;
        r = QP_IS_NULL(m) ? 0 : qp_te_extend(ctx, list, m);
        JS_FreeValue(ctx, m);
        if (r < 0)
            return -1;
    }
    return 0;
}

enum { QP_SUGG_ATTR = 1, QP_SUGG_MODNF, QP_SUGG_IMPORT, QP_SUGG_NAME };

/* traceback._compute_suggestion_error: new str or QP_NULL (never raises) */
static JSValue qp_te_compute_suggestion(JSContext *ctx, JSValueConst exc, JSValueConst tb,
                                        JSValueConst wrong_name_in, int kind)
{
    JSValue wrong_name, d = QP_NULL, tmp, suggestion = QP_NULL;
    BOOL not_normalized = FALSE;
    uint32_t *wb = NULL;
    int wn = 0, r;

    if (!qp_str_check(ctx, wrong_name_in))
        return QP_NULL;
    wrong_name = JS_DupValue(ctx, wrong_name_in);
    if (!qp_te_str_isascii(ctx, wrong_name)) {
        uint32_t *nb;
        int nn = js_string_normalize1(ctx, &nb, wrong_name, UNICODE_NFKC);
        if (nn < 0)
            goto fail;
        tmp = JS_NewUTF32String(ctx, nb, nn);
        js_free(ctx, nb);
        if (JS_IsException(tmp))
            goto fail;
        r = qp_str_eq(ctx, tmp, wrong_name);
        if (r == 0)
            not_normalized = TRUE;
        JS_FreeValue(ctx, wrong_name);
        wrong_name = tmp;
    }
    if (kind == QP_SUGG_ATTR) {
        JSValue obj = qp_te_getattr_none(ctx, exc, "obj");
        BOOL hide = TRUE;
        wn = JS_ToUTF32String(ctx, &wb, wrong_name);
        if (wn < 0) {
            JS_FreeValue(ctx, obj);
            goto fail;
        }
        hide = !(wn > 0 && wb[0] == '_');
        if (hide && !JS_IsUndefined(tb)) {
            QPTraceback *t = qp_te_last_tb(tb);
            if (t) {
                JSValue self = qp_te_frame_self(ctx, t);
                if (qp_tb_same(self, obj) || (!QP_IS_NULL(self) && qp_is(self, obj)))
                    hide = FALSE;
                JS_FreeValue(ctx, self);
            }
        }
        d = qp_te_safe_dir(ctx, obj, hide);
        if (JS_IsException(d)) {
            JS_FreeValue(ctx, obj);
            goto fail;
        }
        JS_FreeValue(ctx, obj);
    } else if (kind == QP_SUGG_MODNF) {
        /* names discovered by the sys.meta_path finders */
        JSAtom ma = JS_NewAtom(ctx, "meta_path");
        JSValue mp = ma != JS_ATOM_NULL ? qp_sys_get(ctx, ma) : QP_NULL, it, finder;
        uint32_t *nb;
        int nn, k;
        BOOL dotted = FALSE;
        JS_FreeAtom(ctx, ma);
        nn = JS_ToUTF32String(ctx, &nb, wrong_name);
        if (nn < 0) {
            JS_FreeValue(ctx, mp);
            goto fail;
        }
        for (k = 0; k < nn; k++)
            dotted |= nb[k] == '.';
        js_free(ctx, nb);
        /* a dotted name needs importlib.util.find_spec(parent), which is
           part of the import machinery of M3: like an exception there,
           no suggestion */
        if (dotted || JS_IsException(mp) || QP_IS_NULL(mp)) {
            JS_FreeValue(ctx, mp);
            goto fail;
        }
        d = qp_list_new(ctx, 0);
        it = JS_IsException(d) ? JS_EXCEPTION : qp_iter(ctx, mp);
        JS_FreeValue(ctx, mp);
        if (JS_IsException(it))
            goto fail;
        while (!QP_IS_NULL(finder = qp_next(ctx, it))) {
            JSValue disc, specs, sit, spec, nm;
            if (JS_IsException(finder)) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
            disc = qp_te_getattr_none(ctx, finder, "discover");
            JS_FreeValue(ctx, finder);
            if (JS_IsUndefined(disc) || qp_bool(ctx, disc) <= 0) {
                qp_tb_clear_exc(ctx);
                JS_FreeValue(ctx, disc);
                continue;
            }
            specs = qp_call1(ctx, disc, JS_UNDEFINED);
            JS_FreeValue(ctx, disc);
            sit = JS_IsException(specs) ? JS_EXCEPTION : qp_iter(ctx, specs);
            JS_FreeValue(ctx, specs);
            if (JS_IsException(sit)) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
            while (!QP_IS_NULL(spec = qp_next(ctx, sit))) {
                if (JS_IsException(spec))
                    break;
                nm = qp_getattr(ctx, spec, QP_ATOM(ctx, name));
                JS_FreeValue(ctx, spec);
                if (JS_IsException(nm) || qp_list_append(ctx, d, nm) < 0) {
                    JS_FreeValue(ctx, nm);
                    spec = JS_EXCEPTION;
                    break;
                }
                JS_FreeValue(ctx, nm);
            }
            JS_FreeValue(ctx, sit);
            if (JS_IsException(spec)) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
        }
        JS_FreeValue(ctx, it);
    } else if (kind == QP_SUGG_IMPORT) {
        /* mod = __import__(exc_value.name) */
        JSValue name = qp_te_getattr_none(ctx, exc, "name"), imp, mod;
        int rr = qp_dict_get_item_atom(ctx, QP_STATE(ctx)->builtins_dict,
                                       QP_ATOM(ctx, __import__), &imp);
        if (rr <= 0) {
            JS_FreeValue(ctx, name);
            goto fail;
        }
        mod = qp_call1(ctx, imp, name);
        JS_FreeValue(ctx, imp);
        JS_FreeValue(ctx, name);
        if (JS_IsException(mod))
            goto fail;
        wn = JS_ToUTF32String(ctx, &wb, wrong_name);
        if (wn < 0) {
            JS_FreeValue(ctx, mod);
            goto fail;
        }
        d = qp_te_safe_dir(ctx, mod, !(wn > 0 && wb[0] == '_'));
        JS_FreeValue(ctx, mod);
        if (JS_IsException(d))
            goto fail;
    } else {
        /* NameError: names of the innermost frame */
        QPTraceback *t = qp_te_last_tb(tb);
        JSValue all, self;
        if (!t)
            goto fail;
        all = qp_list_new(ctx, 0);
        if (JS_IsException(all))
            goto fail;
        if (qp_te_frame_names(ctx, t, all) < 0) {
            JS_FreeValue(ctx, all);
            goto fail;
        }
        d = qp_te_filter_str(ctx, all, FALSE, QP_NULL);
        JS_FreeValue(ctx, all);
        if (JS_IsException(d))
            goto fail;
        if (not_normalized) {
            r = qp_te_str_in_list(ctx, d, wrong_name);
            if (r < 0)
                goto fail;
            if (r) {
                suggestion = JS_DupValue(ctx, wrong_name);
                goto done;
            }
        }
        self = qp_te_frame_self(ctx, t);
        if (!QP_IS_NULL(self)) {
            JSAtom a = qp_str_to_atom(ctx, wrong_name);
            JSValue v;
            int has = 0;
            if (a != JS_ATOM_NULL) {
                has = qp_getattr_opt(ctx, self, a, &v);
                if (has > 0)
                    JS_FreeValue(ctx, v);
                JS_FreeAtom(ctx, a);
            }
            qp_tb_clear_exc(ctx);
            JS_FreeValue(ctx, self);
            if (has > 0) {
                suggestion = qp_str_from_format(ctx, "self.%U", wrong_name);
                if (JS_IsException(suggestion))
                    goto fail;
                goto done;
            }
        }
    }

    if (not_normalized) {
        r = qp_te_str_in_list(ctx, d, wrong_name);
        if (r < 0)
            goto fail;
        if (r) {
            suggestion = JS_DupValue(ctx, wrong_name);
            goto done;
        }
    }
    /* _suggestions._generate_suggestions(d, wrong_name) */
    suggestion = qp_calculate_suggestions(ctx, d, wrong_name);
    if (JS_IsException(suggestion)) {
        qp_tb_clear_exc(ctx);
        suggestion = QP_NULL;
    }
    if (!QP_IS_NULL(suggestion))
        goto done;
    /* pure Python closest match */
    {
        Py_ssize_t n, i, best;
        JSValue *items = qp_list_items(d, &n);
        if (!wb) {
            wn = JS_ToUTF32String(ctx, &wb, wrong_name);
            if (wn < 0)
                goto fail;
        }
        if (items && n <= QP_SUGG_MAX_CANDIDATE_ITEMS && wn <= QP_SUGG_MAX_STRING_SIZE) {
            best = wn;
            for (i = 0; i < n; i++) {
                uint32_t *pb;
                int pn = JS_ToUTF32String(ctx, &pb, items[i]);
                Py_ssize_t max_distance, cur;
                if (pn < 0) {
                    qp_tb_clear_exc(ctx);
                    break;
                }
                if (pn == wn && !memcmp(pb, wb, wn * sizeof(uint32_t))) {
                    js_free(ctx, pb);
                    continue;
                }
                max_distance = (pn + wn + 3) * QP_SUGG_MOVE_COST / 6;
                if (max_distance > best - 1)
                    max_distance = best - 1;
                cur = qp_te_levenshtein(wb, wn, pb, pn, max_distance);
                js_free(ctx, pb);
                if (cur > max_distance)
                    continue;
                if (QP_IS_NULL(suggestion) || cur < best) {
                    JS_FreeValue(ctx, suggestion);
                    suggestion = JS_DupValue(ctx, items[i]);
                    best = cur;
                }
                items = qp_list_items(d, &n);
                if (!items)
                    break;
            }
        }
    }
    if (QP_IS_NULL(suggestion) && kind == QP_SUGG_ATTR) {
        JSValue obj = qp_te_getattr_none(ctx, exc, "obj");
        suggestion = qp_te_nested_attribute(ctx, obj, wrong_name, d);
        JS_FreeValue(ctx, obj);
    }
 done:
    js_free(ctx, wb);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, wrong_name);
    return suggestion;
 fail:
    qp_tb_clear_exc(ctx);
    js_free(ctx, wb);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, wrong_name);
    JS_FreeValue(ctx, suggestion);
    return QP_NULL;
}

/* _get_cross_language_hint(obj, wrong_name) */
static JSValue qp_te_cross_language_hint(JSContext *ctx, JSValueConst obj, JSValueConst wrong_name)
{
    static const char bitwise[] = "Did you mean to use an 'int' object? Bitwise operators "
                                  "are not supported by 'float'.";
    static const struct {
        const char *name;
        int type;               /* QPTypeId */
        const char *hint;
        BOOL raw;
    } hints[] = {
        { "push", QP_T_list, "append", FALSE },
        { "concat", QP_T_list, "extend", FALSE },
        { "addAll", QP_T_list, "extend", FALSE },
        { "contains", QP_T_list, "Use 'x in list'.", TRUE },
        { "add", QP_T_list, "Did you mean to use a 'set' object?", TRUE },
        { "add", QP_T_frozenset, "Did you mean to use a 'set' object?", TRUE },
        { "toUpperCase", QP_T_str, "upper", FALSE },
        { "toLowerCase", QP_T_str, "lower", FALSE },
        { "trimStart", QP_T_str, "lstrip", FALSE },
        { "trimEnd", QP_T_str, "rstrip", FALSE },
        { "keySet", QP_T_dict, "keys", FALSE },
        { "entrySet", QP_T_dict, "items", FALSE },
        { "entries", QP_T_dict, "items", FALSE },
        { "putAll", QP_T_dict, "update", FALSE },
        { "put", QP_T_dict, "Use d[k] = v.", TRUE },
        { "append", QP_T_tuple, "Did you mean to use a 'list' object?", TRUE },
        { "extend", QP_T_tuple, "Did you mean to use a 'list' object?", TRUE },
        { "insert", QP_T_tuple, "Did you mean to use a 'list' object?", TRUE },
        { "remove", QP_T_tuple, "Did you mean to use a 'list' object?", TRUE },
        { "remove", QP_T_frozenset, "Did you mean to use a 'set' object?", TRUE },
        { "discard", QP_T_frozenset, "Did you mean to use a 'set' object?", TRUE },
        { "update", QP_T_frozenset, "Did you mean to use a 'set' object?", TRUE },
        { "update", QP_T_frozendict, "Did you mean to use a 'dict' object?", TRUE },
        { "__or__", QP_T_float, bitwise, TRUE },
        { "__and__", QP_T_float, bitwise, TRUE },
        { "__xor__", QP_T_float, bitwise, TRUE },
        { "__lshift__", QP_T_float, bitwise, TRUE },
        { "__rshift__", QP_T_float, bitwise, TRUE },
    };
    const char *w;
    size_t i;
    JSValue res = QP_NULL;

    if (!qp_str_check(ctx, wrong_name))
        return QP_NULL;
    w = JS_ToCString(ctx, wrong_name);
    if (!w) {
        qp_tb_clear_exc(ctx);
        return QP_NULL;
    }
    for (i = 0; i < countof(hints); i++) {
        if (strcmp(w, hints[i].name))
            continue;
        if (!qp_typecheck(ctx, obj, QP_STATE(ctx)->types[hints[i].type]))
            continue;
        if (hints[i].raw)
            res = qp_str_from_cstr(ctx, hints[i].hint);
        else
            res = qp_str_from_format(ctx, "Did you mean '.%s'?", hints[i].hint);
        break;
    }
    JS_FreeCString(ctx, w);
    if (JS_IsException(res)) {
        qp_tb_clear_exc(ctx);
        res = QP_NULL;
    }
    return res;
}

/* name in sys.stdlib_module_names */
static BOOL qp_te_in_stdlib_names(JSContext *ctx, JSValueConst name)
{
    JSAtom a = JS_NewAtom(ctx, "stdlib_module_names");
    JSValue names;
    int r = 0;
    if (a == JS_ATOM_NULL) {
        qp_tb_clear_exc(ctx);
        return FALSE;
    }
    names = qp_sys_get(ctx, a);
    JS_FreeAtom(ctx, a);
    if (!JS_IsException(names) && !QP_IS_NULL(names) && !JS_IsUndefined(names))
        r = qp_contains(ctx, names, name);
    JS_FreeValue(ctx, names);
    qp_tb_clear_exc(ctx);
    return r > 0;
}

/* _missing_stdlib_info: generated by CPython's
   Tools/build/check_extension_modules.py for the modules of
   sys.stdlib_module_names a build cannot import; for this platform, the
   Windows-only ones. */
static int qp_missing_stdlib_info_exec(JSContext *ctx, JSValueConst mod)
{
    static const char *const windows_modules[] = {
        "_overlapped", "_testconsole", "_winapi", "_wmi", "msvcrt", "nt", "winreg", "winsound",
    };
    JSValue d = qp_dict_new(ctx), msg, key;
    JSAtom a;
    size_t i;
    int r;

    if (JS_IsException(d))
        return -1;
    for (i = 0; i < countof(windows_modules); i++) {
        msg = qp_str_from_format(ctx, "Unsupported platform for Windows-only standard library "
                                 "module '%s'", windows_modules[i]);
        if (JS_IsException(msg))
            goto fail;
        key = qp_str_from_cstr(ctx, windows_modules[i]);
        r = JS_IsException(key) ? -1 : qp_dict_set_item(ctx, d, key, msg);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, msg);
        if (r < 0)
            goto fail;
    }
    a = JS_NewAtom(ctx, "_MISSING_STDLIB_MODULE_MESSAGES");
    if (a == JS_ATOM_NULL)
        goto fail;
    r = qp_setattr(ctx, mod, a, d);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, d);
    return r;
 fail:
    JS_FreeValue(ctx, d);
    return -1;
}

static const QPModuleDef qp_moddef__missing_stdlib_info = {
    "_missing_stdlib_info",
    NULL,
    NULL,
    qp_missing_stdlib_info_exec,
};

/* sys.modules[name], new reference or QP_NULL (never raises) */
static JSValue qp_tb_loaded_module(JSContext *ctx, const char *name)
{
    JSValue key, mod = QP_NULL;
    int r;

    if (!qp_dict_check(ctx, QP_STATE(ctx)->modules))
        return QP_NULL;
    key = qp_str_from_cstr(ctx, name);
    if (JS_IsException(key)) {
        qp_tb_clear_exc(ctx);
        return QP_NULL;
    }
    r = qp_dict_get_item(ctx, QP_STATE(ctx)->modules, key, &mod);
    JS_FreeValue(ctx, key);
    if (r <= 0) {
        qp_tb_clear_exc(ctx);
        return QP_NULL;
    }
    return mod;
}

/* traceback._MISSING_STDLIB_MODULE_MESSAGES.get(name, "Standard library
   module %r was not found"): the traceback module's mapping once it is
   imported, else the one of _missing_stdlib_info. */
static JSValue qp_te_missing_stdlib_message(JSContext *ctx, JSValueConst name)
{
    JSValue mod = qp_tb_loaded_module(ctx, "traceback"), msgs = QP_NULL, m = QP_NULL;

    if (QP_IS_NULL(mod)) {
        mod = qp_import_module(ctx, "_missing_stdlib_info");
        if (JS_IsException(mod)) {
            qp_tb_clear_exc(ctx);
            mod = QP_NULL;
        }
    }
    if (!QP_IS_NULL(mod)) {
        msgs = qp_te_getattr_opt(ctx, mod, "_MISSING_STDLIB_MODULE_MESSAGES");
        JS_FreeValue(ctx, mod);
        if (JS_IsException(msgs))
            msgs = QP_NULL;
    }
    if (!QP_IS_NULL(msgs)) {
        JSValue dflt = qp_str_from_format(ctx, "Standard library module %R was not found", name);
        JSValueConst args[2] = { name, dflt };
        JSAtom a = JS_NewAtom(ctx, "get");
        if (a != JS_ATOM_NULL && !JS_IsException(dflt))
            m = qp_call_method(ctx, msgs, a, args, 2, QP_NULL);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, dflt);
        JS_FreeValue(ctx, msgs);
        if (JS_IsException(m) || !qp_str_check(ctx, m)) {
            JS_FreeValue(ctx, m);
            m = QP_NULL;
        }
        qp_tb_clear_exc(ctx);
    }
    if (QP_IS_NULL(m))
        m = qp_str_from_format(ctx, "Standard library module %R was not found", name);
    return m;
}

/* traceback._find_incompatible_extension_module(name): new str or QP_NULL
   (never raises) */
static JSValue qp_te_incompatible_extension(JSContext *ctx, JSValueConst name)
{
    JSValue mod, res = QP_NULL;
    JSAtom a;

    if (!qp_str_check(ctx, name))
        return QP_NULL;
    mod = qp_import_module(ctx, "traceback");
    if (JS_IsException(mod)) {
        qp_tb_clear_exc(ctx);
        return QP_NULL;
    }
    a = JS_NewAtom(ctx, "_find_incompatible_extension_module");
    if (a != JS_ATOM_NULL)
        res = qp_call_method(ctx, mod, a, &name, 1, QP_NULL);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(res) || !qp_str_check(ctx, res) || !qp_bool(ctx, res)) {
        JS_FreeValue(ctx, res);
        res = QP_NULL;
    }
    qp_tb_clear_exc(ctx);
    return res;
}

/* self._str += <formatted suffix> */
static void qp_te_append_str(JSContext *ctx, QPTE *te, JSValue suffix)
{
    JSValue n;
    if (JS_IsException(suffix)) {
        qp_tb_clear_exc(ctx);
        return;
    }
    n = qp_str_concat(ctx, te->str, suffix);
    JS_FreeValue(ctx, suffix);
    if (JS_IsException(n)) {
        qp_tb_clear_exc(ctx);
        return;
    }
    JS_FreeValue(ctx, te->str);
    te->str = n;
}

/* ". Did you mean: 's'?" or with ascii() for non-ASCII suggestions */
static JSValue qp_te_did_you_mean(JSContext *ctx, JSValueConst s)
{
    if (qp_te_str_isascii(ctx, s))
        return qp_str_from_format(ctx, ". Did you mean: '%U'?", s);
    return qp_str_from_format(ctx, ". Did you mean: '%U' (%A)?", s, s);
}

/* TracebackException.__init__ for one exception (no chain processing). */
static QPTE *qp_te_new(QPTBState *st, JSValueConst exc)
{
    JSContext *ctx = st->ctx;
    QPTE *te;
    QPType *t;
    JSValue tb, v;

    if (st->ntes == st->tes_size) {
        int ns = st->tes_size ? st->tes_size * 2 : 8;
        QPTE **n = js_realloc(ctx, st->tes, ns * sizeof(QPTE *));
        if (!n) {
            qp_tb_clear_exc(ctx);
            return NULL;
        }
        st->tes = n;
        st->tes_size = ns;
    }
    te = js_mallocz(ctx, sizeof(*te));
    if (!te) {
        qp_tb_clear_exc(ctx);
        return NULL;
    }
    st->tes[st->ntes++] = te;
    te->exc = JS_DupValue(ctx, exc);
    te->notes = te->type_str = te->str = QP_NULL;
    te->se_filename = te->se_lineno = te->se_end_lineno = te->se_text = JS_UNDEFINED;
    te->se_offset = te->se_end_offset = te->se_msg = JS_UNDEFINED;
    qp_te_seen_add(st, exc);

    tb = qp_exc_check(ctx, exc) ? JS_DupValue(ctx, qp_exc_traceback(exc)) : JS_UNDEFINED;
    te->nframes = qp_tb_extract(st, tb, &te->frames);

    /* _str = _safe_string(exc_value, 'exception') */
    te->str = qp_str(ctx, exc);
    if (JS_IsException(te->str)) {
        qp_tb_clear_exc(ctx);
        te->str = qp_str_from_cstr(ctx, "<exception str() failed>");
        if (JS_IsException(te->str)) {
            qp_tb_clear_exc(ctx);
            te->str = JS_AtomToString(ctx, JS_ATOM_empty_string);
        }
    }
    /* __notes__ */
    v = qp_te_getattr_opt(ctx, exc, "__notes__");
    if (JS_IsException(v)) {
        JSValue e = qp_exc_fetch(ctx), r;
        r = qp_repr(ctx, e);
        if (JS_IsException(r)) {
            qp_tb_clear_exc(ctx);
            r = qp_str_from_cstr(ctx, "<__notes__ repr() failed>");
        }
        v = JS_IsException(r) ? JS_EXCEPTION :
            qp_str_from_format(ctx, "Ignored error getting __notes__: %U", r);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
        if (JS_IsException(v)) {
            qp_tb_clear_exc(ctx);
            v = QP_NULL;
        } else {
            te->notes_failed = TRUE;
        }
    }
    te->notes = (JS_IsUndefined(v)) ? QP_NULL : v;

    /* exc_type_str */
    t = qp_type_of(ctx, exc);
    {
        JSValue qn = qp_getattr(ctx, QP_TYPE_VALUE(t), QP_ATOM(ctx, __qualname__));
        JSValue mod = JS_IsException(qn) ? JS_EXCEPTION :
            qp_getattr(ctx, QP_TYPE_VALUE(t), QP_ATOM(ctx, __module__));
        if (JS_IsException(qn) || JS_IsException(mod)) {
            qp_tb_clear_exc(ctx);
            JS_FreeValue(ctx, qn);
            te->type_str = qp_str_from_atom(ctx, t->name);
        } else {
            BOOL plain = FALSE;
            if (qp_str_check(ctx, mod)) {
                const char *m = JS_ToCString(ctx, mod);
                if (m) {
                    plain = !strcmp(m, "__main__") || !strcmp(m, "builtins");
                    JS_FreeCString(ctx, m);
                }
                if (plain)
                    te->type_str = qp_str(ctx, qn);
                else
                    te->type_str = qp_str_from_format(ctx, "%U.%S", mod, qn);
            } else {
                te->type_str = qp_str_from_format(ctx, "<unknown>.%S", qn);
            }
            JS_FreeValue(ctx, qn);
            JS_FreeValue(ctx, mod);
        }
        if (JS_IsException(te->type_str)) {
            qp_tb_clear_exc(ctx);
            te->type_str = qp_str_from_cstr(ctx, "<unknown>");
            if (JS_IsException(te->type_str)) {
                qp_tb_clear_exc(ctx);
                te->type_str = JS_AtomToString(ctx, JS_ATOM_empty_string);
            }
        }
    }

    if (qp_type_is_subtype(t, QP_EXC(ctx, SyntaxError))) {
        JSValue lno;
        te->is_syntax_error = TRUE;
        te->se_filename = qp_te_getattr_none(ctx, exc, "filename");
        lno = qp_te_getattr_none(ctx, exc, "lineno");
        te->se_lineno = JS_IsUndefined(lno) ? JS_UNDEFINED : qp_str(ctx, lno);
        JS_FreeValue(ctx, lno);
        lno = qp_te_getattr_none(ctx, exc, "end_lineno");
        te->se_end_lineno = JS_IsUndefined(lno) ? JS_UNDEFINED : qp_str(ctx, lno);
        JS_FreeValue(ctx, lno);
        if (JS_IsException(te->se_lineno) || JS_IsException(te->se_end_lineno)) {
            qp_tb_clear_exc(ctx);
            if (JS_IsException(te->se_lineno))
                te->se_lineno = JS_UNDEFINED;
            if (JS_IsException(te->se_end_lineno))
                te->se_end_lineno = JS_UNDEFINED;
        }
        te->se_text = qp_te_getattr_none(ctx, exc, "text");
        te->se_offset = qp_te_getattr_none(ctx, exc, "offset");
        te->se_end_offset = qp_te_getattr_none(ctx, exc, "end_offset");
        te->se_msg = qp_te_getattr_none(ctx, exc, "msg");
    } else if (qp_type_is_subtype(t, QP_EXC(ctx, ImportError)) &&
               !JS_IsUndefined(v = qp_te_getattr_none(ctx, exc, "name_from"))) {
        JSValue s = qp_te_compute_suggestion(ctx, exc, tb, v, QP_SUGG_IMPORT);
        if (!QP_IS_NULL(s)) {
            qp_te_append_str(ctx, te, qp_te_did_you_mean(ctx, s));
            JS_FreeValue(ctx, s);
        }
        JS_FreeValue(ctx, v);
    } else if (qp_type_is_subtype(t, QP_EXC(ctx, ModuleNotFoundError))) {
        JSValue name = qp_te_getattr_none(ctx, exc, "name");
        if (qp_te_in_stdlib_names(ctx, name)) {
            JSValue m = qp_te_missing_stdlib_message(ctx, name);
            if (JS_IsException(m)) {
                qp_tb_clear_exc(ctx);
            } else {
                JS_FreeValue(ctx, te->str);
                te->str = m;
            }
        } else {
            JSAtom fa = JS_NewAtom(ctx, "flags");
            JSValue flags = fa != JS_ATOM_NULL ? qp_sys_get(ctx, fa) : QP_NULL;
            JSValue ns = (JS_IsException(flags) || QP_IS_NULL(flags)) ? JS_UNDEFINED :
                qp_te_getattr_none(ctx, flags, "no_site");
            int no_site = JS_IsUndefined(ns) ? 0 : qp_bool(ctx, ns);
            if (fa != JS_ATOM_NULL)
                JS_FreeAtom(ctx, fa);
            JS_FreeValue(ctx, flags);
            JS_FreeValue(ctx, ns);
            qp_tb_clear_exc(ctx);
            if (no_site > 0) {
                qp_te_append_str(ctx, te, qp_str_from_cstr(ctx,
                    ". Site initialization is disabled, did you forget to "
                    "add the site-packages directory to sys.path "
                    "or to enable your virtual environment?"));
            } else {
                JSValue abi_tag = qp_te_incompatible_extension(ctx, name), s;
                if (!QP_IS_NULL(abi_tag)) {
                    qp_te_append_str(ctx, te, qp_str_from_format(ctx,
                        ". Although a module with this name was found for a "
                        "different Python version (%U).", abi_tag));
                    JS_FreeValue(ctx, abi_tag);
                } else if (!QP_IS_NULL(s = qp_te_compute_suggestion(ctx, exc, tb, name,
                                                                    QP_SUGG_MODNF))) {
                    qp_te_append_str(ctx, te, qp_str_from_format(ctx, ". Did you mean: '%U'?", s));
                    JS_FreeValue(ctx, s);
                }
            }
        }
        JS_FreeValue(ctx, name);
    } else if (qp_type_is_subtype(t, QP_EXC(ctx, AttributeError)) &&
               !JS_IsUndefined(v = qp_te_getattr_none(ctx, exc, "name"))) {
        JSValue obj = qp_te_getattr_opt(ctx, exc, "obj"), hint = QP_NULL;
        if (JS_IsException(obj)) {
            qp_tb_clear_exc(ctx);
            obj = QP_NULL;
        }
        if (!QP_IS_NULL(obj))
            hint = qp_te_cross_language_hint(ctx, obj, v);
        JS_FreeValue(ctx, obj);
        if (!QP_IS_NULL(hint)) {
            qp_te_append_str(ctx, te, qp_str_from_format(ctx, ". %U", hint));
            JS_FreeValue(ctx, hint);
        } else {
            JSValue s = qp_te_compute_suggestion(ctx, exc, tb, v, QP_SUGG_ATTR);
            if (!QP_IS_NULL(s)) {
                if (qp_te_str_isascii(ctx, s))
                    qp_te_append_str(ctx, te, qp_str_from_format(ctx,
                        ". Did you mean '.%U' instead of '.%S'?", s, v));
                else
                    qp_te_append_str(ctx, te, qp_str_from_format(ctx,
                        ". Did you mean '.%U' (%A) instead of '.%S' (%A)?", s, s, v, v));
                JS_FreeValue(ctx, s);
            }
        }
        JS_FreeValue(ctx, v);
    } else if (qp_type_is_subtype(t, QP_EXC(ctx, NameError)) &&
               !JS_IsUndefined(v = qp_te_getattr_none(ctx, exc, "name"))) {
        JSValue s = qp_te_compute_suggestion(ctx, exc, tb, v, QP_SUGG_NAME);
        if (!QP_IS_NULL(s))
            qp_te_append_str(ctx, te, qp_te_did_you_mean(ctx, s));
        if (qp_str_check(ctx, v) && qp_te_in_stdlib_names(ctx, v)) {
            if (!QP_IS_NULL(s))
                qp_te_append_str(ctx, te, qp_str_from_format(ctx,
                    " Or did you forget to import '%U'?", v));
            else
                qp_te_append_str(ctx, te, qp_str_from_format(ctx,
                    ". Did you forget to import '%U'?", v));
        }
        JS_FreeValue(ctx, s);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, tb);
    te->suppress_context = qp_exc_check(ctx, exc) ? qp_exc_suppress_context(exc) : FALSE;
    return te;
}

/* TracebackException(type(exc), exc, exc.__traceback__, compact=True) with
   the queue that converts __cause__ / __context__ / group members. */
static QPTE *qp_te_build(QPTBState *st, JSValueConst exc)
{
    JSContext *ctx = st->ctx;
    QPTE *top = qp_te_new(st, exc), **queue = NULL;
    int qlen = 0, qsize = 0;

    if (!top)
        return NULL;
#define QP_TE_PUSH(x) do {                                                \
        if (qlen == qsize) {                                              \
            int ns_ = qsize ? qsize * 2 : 16;                             \
            QPTE **nq_ = js_realloc(ctx, queue, ns_ * sizeof(QPTE *));    \
            if (!nq_) { qp_tb_clear_exc(ctx); goto out; }                 \
            queue = nq_; qsize = ns_;                                     \
        }                                                                 \
        queue[qlen++] = (x);                                              \
    } while (0)
    QP_TE_PUSH(top);
    while (qlen > 0) {
        QPTE *te = queue[--qlen];
        JSValueConst e = te->exc;
        JSValueConst c;
        BOOL is_exc = qp_exc_check(ctx, e);

        te->cause = NULL;
        te->context = NULL;
        if (is_exc && !QP_TB_NONE_OR_NULL(c = qp_exc_cause(e)) && !qp_te_seen(st, c))
            te->cause = qp_te_new(st, c);
        /* compact: context only when there is no cause and it is not suppressed */
        if (is_exc && te->cause == NULL && !qp_exc_suppress_context(e) &&
            !QP_TB_NONE_OR_NULL(c = qp_exc_context(e)) && !qp_te_seen(st, c))
            te->context = qp_te_new(st, c);
        if (is_exc && qp_typecheck(ctx, e, QP_EXC(ctx, BaseExceptionGroup))) {
            JSValue excs = qp_getattr(ctx, e, QP_ATOM(ctx, exceptions));
            te->has_excs = TRUE;
            if (JS_IsException(excs)) {
                qp_tb_clear_exc(ctx);
            } else {
                JSValue tup = qp_tuple_check(ctx, excs) ? JS_DupValue(ctx, excs) :
                    qp_tuple_from_iterable(ctx, excs);
                JS_FreeValue(ctx, excs);
                if (JS_IsException(tup)) {
                    qp_tb_clear_exc(ctx);
                } else {
                    Py_ssize_t k, n = QP_TUPLE_SIZE(tup);
                    te->excs = js_mallocz(ctx, sizeof(QPTE *) * (n + 1));
                    if (!te->excs) {
                        qp_tb_clear_exc(ctx);
                    } else {
                        for (k = 0; k < n; k++) {
                            QPTE *x = qp_te_new(st, QP_TUPLE_ITEMS(tup)[k]);
                            if (!x)
                                break;
                            te->excs[te->nexcs++] = x;
                        }
                    }
                    JS_FreeValue(ctx, tup);
                }
            }
        }
        if (te->cause)
            QP_TE_PUSH(te->cause);
        if (te->context)
            QP_TE_PUSH(te->context);
        {
            int k;
            for (k = 0; k < te->nexcs; k++)
                QP_TE_PUSH(te->excs[k]);
        }
    }
#undef QP_TE_PUSH
 out:
    js_free(ctx, queue);
    return top;
}

static void qp_te_free_all(QPTBState *st)
{
    JSContext *ctx = st->ctx;
    int i;
    for (i = 0; i < st->ntes; i++) {
        QPTE *te = st->tes[i];
        JS_FreeValue(ctx, te->exc);
        JS_FreeValue(ctx, te->type_str);
        JS_FreeValue(ctx, te->str);
        JS_FreeValue(ctx, te->notes);
        JS_FreeValue(ctx, te->se_filename);
        JS_FreeValue(ctx, te->se_lineno);
        JS_FreeValue(ctx, te->se_end_lineno);
        JS_FreeValue(ctx, te->se_text);
        JS_FreeValue(ctx, te->se_offset);
        JS_FreeValue(ctx, te->se_end_offset);
        JS_FreeValue(ctx, te->se_msg);
        qp_tb_free_frames(ctx, te->frames, te->nframes);
        js_free(ctx, te->excs);
        js_free(ctx, te);
    }
    js_free(ctx, st->tes);
    st->tes = NULL;
    st->ntes = 0;
}

/* ---- formatting ---------------------------------------------------------- */

/* _ExceptionPrintContext.emit(text, margin_char) for one chunk */
static void qp_tb_emit(QPTBState *st, const uint32_t *s, size_t n, uint32_t margin)
{
    uint32_t prefix[256];
    size_t pl = 0;
    int i;
    if (st->group_depth == 0) {
        qp_text_put(&st->out, s, n);
        return;
    }
    for (i = 0; i < 2 * st->group_depth && pl < countof(prefix) - 2; i++)
        prefix[pl++] = ' ';
    prefix[pl++] = margin;
    prefix[pl++] = ' ';
    qp_text_indent_all(&st->out, s, n, prefix, pl);
}

static void qp_tb_emit_text(QPTBState *st, QPText *chunk, uint32_t margin)
{
    if (chunk->oom)
        st->out.oom = TRUE;
    qp_tb_emit(st, chunk->buf, chunk->len, margin);
    chunk->len = 0;
}

static void qp_tb_emit_cstr(QPTBState *st, const char *s, uint32_t margin)
{
    QPText t;
    qp_text_init(&t, st->ctx);
    qp_text_puts(&t, s);
    qp_tb_emit_text(st, &t, margin);
    qp_text_free(&t);
}

static BOOL qp_tb_value_eq_str(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int r;
    if (!qp_str_check(ctx, a) || !qp_str_check(ctx, b))
        return FALSE;
    r = qp_str_eq(ctx, a, b);
    if (r < 0)
        qp_tb_clear_exc(ctx);
    return r > 0;
}

/* StackSummary.format(): every formatted frame / repetition line is one
   emitted chunk */
static void qp_tb_emit_stack(QPTBState *st, QPTE *te)
{
    JSContext *ctx = st->ctx;
    QPText chunk;
    int i, count = 0;
    QPFrameSum *last = NULL;

    qp_text_init(&chunk, ctx);
    for (i = 0; i < te->nframes; i++) {
        QPFrameSum *f = &te->frames[i];
        if (!last || !qp_tb_value_eq_str(ctx, last->filename, f->filename) ||
            last->lineno == QP_TB_NONE || last->lineno != f->lineno ||
            !qp_tb_value_eq_str(ctx, last->name, f->name)) {
            if (count > 3) {
                count -= 3;
                qp_text_printf(&chunk, "  [Previous line repeated %d more time%s]\n",
                               count, count > 1 ? "s" : "");
                qp_tb_emit_text(st, &chunk, '|');
            }
            last = f;
            count = 0;
        }
        count++;
        if (count > 3)
            continue;
        qp_tb_format_frame(st, &chunk, f);
        qp_tb_emit_text(st, &chunk, '|');
    }
    if (count > 3) {
        count -= 3;
        qp_text_printf(&chunk, "  [Previous line repeated %d more time%s]\n",
                       count, count > 1 ? "s" : "");
        qp_tb_emit_text(st, &chunk, '|');
    }
    qp_text_free(&chunk);
}

/* int value of a Python int (bool included); FALSE if not an int */
static BOOL qp_tb_as_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    JSValue iv;
    int64_t x;
    int ov = 0;
    if (QP_IS_NULL(v) || !qp_long_check(ctx, v))
        return FALSE;
    iv = qp_long_value(ctx, v);
    if (JS_IsException(iv)) {
        qp_tb_clear_exc(ctx);
        return FALSE;
    }
    if (qp_long_as_int64_overflow(iv, &x, &ov) < 0) {
        qp_tb_clear_exc(ctx);
        x = 0;
    }
    JS_FreeValue(ctx, iv);
    if (ov)
        x = ov > 0 ? INT32_MAX : INT32_MIN;
    if (x > INT32_MAX)
        x = INT32_MAX;
    if (x < INT32_MIN)
        x = INT32_MIN;
    *out = (Py_ssize_t)x;
    return TRUE;
}

/* TracebackException._find_keyword_typos (it tokenizes the code and
   recompiles candidate fixes with codeop and difflib): run by the
   traceback module on a TracebackException of the error, whose updated
   location, text and message replace ours when it found a keyword.  Errors
   are suppressed, as in _format_syntax_error. */
static void qp_te_find_keyword_typos(JSContext *ctx, QPTE *te)
{
    static const char *const fields[6] = { "text", "offset", "end_offset", "lineno",
                                           "end_lineno", "msg" };
    JSValue meta, mod = JS_UNDEFINED, cls = JS_UNDEFINED, pyte = JS_UNDEFINED, r, msg;
    JSValue args[3], vals[6];
    const char *m;
    BOOL wanted;
    JSAtom a;
    int i, eq;

    if (!qp_str_check(ctx, te->se_msg))
        return;
    m = qp_str_utf8(ctx, te->se_msg, NULL);
    if (!m) {
        qp_tb_clear_exc(ctx);
        return;
    }
    wanted = !strcmp(m, "invalid syntax") || strstr(m, "Perhaps you forgot a comma") != NULL;
    JS_FreeCString(ctx, m);
    if (!wanted)
        return;
    meta = qp_te_getattr_none(ctx, te->exc, "_metadata");
    i = qp_bool(ctx, meta);
    JS_FreeValue(ctx, meta);
    if (i <= 0)
        goto done;
    mod = qp_import_module(ctx, "traceback");
    if (JS_IsException(mod))
        goto done;
    cls = qp_te_getattr_opt(ctx, mod, "TracebackException");
    if (JS_IsException(cls) || QP_IS_NULL(cls))
        goto done;
    args[0] = QP_TYPE_VALUE(qp_type_of(ctx, te->exc));
    args[1] = te->exc;
    args[2] = JS_UNDEFINED;
    pyte = qp_vectorcall(ctx, cls, (JSValueConst *)args, 3, QP_NULL);
    if (JS_IsException(pyte))
        goto done;
    a = JS_NewAtom(ctx, "_find_keyword_typos");
    if (a == JS_ATOM_NULL)
        goto done;
    r = qp_call_method(ctx, pyte, a, NULL, 0, QP_NULL);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);
    msg = qp_te_getattr_none(ctx, pyte, "msg");
    eq = qp_str_check(ctx, msg) ? qp_str_eq(ctx, msg, te->se_msg) : 1;
    JS_FreeValue(ctx, msg);
    if (eq != 0)
        goto done;
    for (i = 0; i < 6; i++)
        vals[i] = qp_te_getattr_none(ctx, pyte, fields[i]);
    JS_FreeValue(ctx, te->se_text);
    te->se_text = vals[0];
    JS_FreeValue(ctx, te->se_offset);
    te->se_offset = vals[1];
    JS_FreeValue(ctx, te->se_end_offset);
    te->se_end_offset = vals[2];
    JS_FreeValue(ctx, te->se_lineno);
    te->se_lineno = vals[3];
    JS_FreeValue(ctx, te->se_end_lineno);
    te->se_end_lineno = vals[4];
    JS_FreeValue(ctx, te->se_msg);
    te->se_msg = vals[5];
 done:
    JS_FreeValue(ctx, pyte);
    JS_FreeValue(ctx, cls);
    JS_FreeValue(ctx, mod);
    qp_tb_clear_exc(ctx);
}

/* TracebackException._format_syntax_error: each yielded line is emitted */
static void qp_tb_emit_syntax_error(QPTBState *st, QPTE *te)
{
    JSContext *ctx = st->ctx;
    QPText chunk;
    BOOL has_suffix = FALSE;
    uint32_t *text = NULL;
    int tn;

    qp_text_init(&chunk, ctx);
    if (!JS_IsUndefined(te->se_lineno)) {
        int truth = qp_bool(ctx, te->se_filename);
        qp_text_puts(&chunk, "  File ");
        qp_tb_th(&chunk, st, QP_TH_FILENAME);
        qp_text_putc(&chunk, '"');
        if (truth > 0) {
            qp_text_put_safe_str(&chunk, te->se_filename, FALSE, "filename");
        } else {
            qp_tb_clear_exc(ctx);
            qp_text_puts(&chunk, "<string>");
        }
        qp_text_putc(&chunk, '"');
        qp_tb_th(&chunk, st, QP_TH_RESET);
        qp_text_puts(&chunk, ", line ");
        qp_tb_th(&chunk, st, QP_TH_LINE_NO);
        if (qp_text_put_str(&chunk, te->se_lineno) < 0)
            qp_tb_clear_exc(ctx);
        qp_tb_th(&chunk, st, QP_TH_RESET);
        qp_text_putc(&chunk, '\n');
        qp_tb_emit_text(st, &chunk, '|');
    } else if (!JS_IsUndefined(te->se_filename)) {
        has_suffix = TRUE;
    }
    if (qp_str_check(ctx, te->se_text))
        qp_te_find_keyword_typos(ctx, te);
    if (qp_str_check(ctx, te->se_text) &&
        (tn = JS_ToUTF32String(ctx, &text, te->se_text)) >= 0) {
        Py_ssize_t rlen = tn, ls = 0, spaces, offset, end_offset, colno, end_colno;
        const uint32_t *ltext;
        Py_ssize_t llen;
        while (rlen > 0 && text[rlen - 1] == '\n')
            rlen--;
        while (ls < rlen && (text[ls] == ' ' || text[ls] == '\n' || text[ls] == '\f'))
            ls++;
        ltext = text + ls;
        llen = rlen - ls;
        spaces = ls;
        if (JS_IsUndefined(te->se_offset)) {
            qp_text_puts(&chunk, "    ");
            qp_text_put(&chunk, ltext, llen);
            qp_text_putc(&chunk, '\n');
            qp_tb_emit_text(st, &chunk, '|');
        } else if (qp_tb_as_ssize(ctx, te->se_offset, &offset)) {
            BOOL same_line;
            if (JS_IsUndefined(te->se_lineno) || JS_IsUndefined(te->se_end_lineno))
                same_line = JS_IsUndefined(te->se_lineno) && JS_IsUndefined(te->se_end_lineno);
            else {
                /* str after TracebackException.__init__, int once
                   _find_keyword_typos relocated the error */
                Py_ssize_t l1, l2;
                same_line = qp_tb_value_eq_str(ctx, te->se_lineno, te->se_end_lineno) ||
                    (qp_tb_as_ssize(ctx, te->se_lineno, &l1) &&
                     qp_tb_as_ssize(ctx, te->se_end_lineno, &l2) && l1 == l2);
            }
            if (same_line) {
                Py_ssize_t eo;
                if (qp_tb_as_ssize(ctx, te->se_end_offset, &eo) && eo != 0)
                    end_offset = eo;
                else
                    end_offset = offset;
            } else {
                end_offset = rlen + 1;
            }
            if (tn > 0 && offset > tn)
                offset = rlen + 1;
            if (tn > 0 && end_offset > tn)
                end_offset = rlen + 1;
            if (offset >= end_offset || end_offset < 0)
                end_offset = offset + 1;
            colno = offset - 1 - spaces;
            end_colno = end_offset - 1 - spaces;
            if (colno >= 0) {
                Py_ssize_t dp_colno = qp_tb_display_width(ltext, llen, colno);
                Py_ssize_t hs = colno > llen ? llen : colno;
                Py_ssize_t he = end_colno > llen ? llen : end_colno;
                Py_ssize_t caret_count = he > hs ? qp_tb_wlen(ltext + hs, he - hs)
                                                 : end_colno - colno;
                qp_text_puts(&chunk, "    ");
                if (st->colorize) {
                    /* ltext[:colno] + error_highlight + ltext[colno:end_colno]
                       + reset + ltext[end_colno:] */
                    if (he < hs)
                        he = hs;
                    qp_text_put(&chunk, ltext, hs);
                    qp_tb_th(&chunk, st, QP_TH_ERROR_HIGHLIGHT);
                    qp_text_put(&chunk, ltext + hs, he - hs);
                    qp_tb_th(&chunk, st, QP_TH_RESET);
                    qp_text_put(&chunk, ltext + he, llen - he);
                } else {
                    qp_text_put(&chunk, ltext, llen);
                }
                qp_text_putc(&chunk, '\n');
                qp_tb_emit_text(st, &chunk, '|');
                qp_text_puts(&chunk, "    ");
                qp_text_fill(&chunk, ' ', dp_colno);
                qp_tb_th(&chunk, st, QP_TH_ERROR_HIGHLIGHT);
                qp_text_fill(&chunk, '^', caret_count);
                qp_tb_th(&chunk, st, QP_TH_RESET);
                qp_text_putc(&chunk, '\n');
                qp_tb_emit_text(st, &chunk, '|');
            } else {
                qp_text_puts(&chunk, "    ");
                qp_text_put(&chunk, ltext, llen);
                qp_text_putc(&chunk, '\n');
                qp_tb_emit_text(st, &chunk, '|');
            }
        }
        js_free(ctx, text);
    } else {
        qp_tb_clear_exc(ctx);
    }
    /* "{type}{stype}{reset}: {message}{msg}{reset}{filename_suffix}\n" */
    qp_tb_th(&chunk, st, QP_TH_TYPE);
    if (qp_text_put_str(&chunk, te->type_str) < 0)
        qp_tb_clear_exc(ctx);
    qp_tb_th(&chunk, st, QP_TH_RESET);
    qp_text_puts(&chunk, ": ");
    qp_tb_th(&chunk, st, QP_TH_MESSAGE);
    {
        int truth = qp_bool(ctx, te->se_msg);
        if (truth == 0) {
            qp_text_puts(&chunk, "<no detail available>");
        } else {
            if (truth < 0)
                qp_tb_clear_exc(ctx);   /* TypeError from bool(): msg kept */
            qp_text_put_safe_str(&chunk, te->se_msg, FALSE, "msg");
        }
    }
    qp_tb_th(&chunk, st, QP_TH_RESET);
    if (has_suffix) {
        qp_text_puts(&chunk, " (");
        qp_text_put_safe_str(&chunk, te->se_filename, FALSE, "filename");
        qp_text_putc(&chunk, ')');
    }
    qp_text_putc(&chunk, '\n');
    qp_tb_emit_text(st, &chunk, '|');
    qp_text_free(&chunk);
}

/* _format_note: one chunk per "\n"-separated line */
static void qp_tb_emit_note(QPTBState *st, QPText *note)
{
    QPText chunk;
    size_t start = 0, i;
    qp_text_init(&chunk, st->ctx);
    for (i = 0; i <= note->len; i++) {
        if (i == note->len || note->buf[i] == '\n') {
            qp_tb_th(&chunk, st, QP_TH_NOTE);
            qp_text_put(&chunk, note->buf + start, i - start);
            qp_tb_th(&chunk, st, QP_TH_RESET);
            qp_text_putc(&chunk, '\n');
            qp_tb_emit_text(st, &chunk, '|');
            start = i + 1;
        }
    }
    qp_text_free(&chunk);
}

/* collections.abc.Sequence check for __notes__ (builtin sequences) */
static BOOL qp_tb_is_sequence(JSContext *ctx, JSValueConst v)
{
    return qp_list_check(ctx, v) || qp_tuple_check(ctx, v) ||
        qp_typecheck(ctx, v, QP_TYPE(ctx, range)) ||
        qp_typecheck(ctx, v, QP_TYPE(ctx, memoryview)) || qp_bytearray_check(ctx, v);
}

/* TracebackException.format_exception_only() (depth 0, show_group False) */
static void qp_tb_emit_exception_only(QPTBState *st, QPTE *te)
{
    JSContext *ctx = st->ctx;
    QPText chunk;

    qp_text_init(&chunk, ctx);
    if (!te->is_syntax_error) {
        /* _format_final_exc_line */
        qp_tb_th(&chunk, st, QP_TH_TYPE);
        if (qp_text_put_str(&chunk, te->type_str) < 0)
            qp_tb_clear_exc(ctx);
        qp_tb_th(&chunk, st, QP_TH_RESET);
        if (qp_str_check(ctx, te->str) && qp_str_len(ctx, te->str) > 0) {
            qp_text_puts(&chunk, ": ");
            qp_tb_th(&chunk, st, QP_TH_MESSAGE);
            if (qp_text_put_str(&chunk, te->str) < 0)
                qp_tb_clear_exc(ctx);
            qp_tb_th(&chunk, st, QP_TH_RESET);
        }
        qp_text_putc(&chunk, '\n');
        qp_tb_emit_text(st, &chunk, '|');
    } else {
        qp_tb_emit_syntax_error(st, te);
    }
    if (te->notes_failed) {
        if (qp_text_put_str(&chunk, te->notes) < 0)
            qp_tb_clear_exc(ctx);
        qp_tb_emit_note(st, &chunk);
        chunk.len = 0;
    } else if (!QP_IS_NULL(te->notes)) {
        if (qp_tb_is_sequence(ctx, te->notes) && !qp_str_check(ctx, te->notes) &&
            !qp_bytes_check(ctx, te->notes)) {
            JSValue it = qp_iter(ctx, te->notes), note;
            if (JS_IsException(it)) {
                qp_tb_clear_exc(ctx);
            } else {
                while (!QP_IS_NULL(note = qp_next(ctx, it))) {
                    if (JS_IsException(note)) {
                        qp_tb_clear_exc(ctx);
                        break;
                    }
                    qp_text_put_safe_str(&chunk, note, FALSE, "note");
                    JS_FreeValue(ctx, note);
                    qp_tb_emit_note(st, &chunk);
                    chunk.len = 0;
                }
                JS_FreeValue(ctx, it);
            }
        } else {
            qp_text_put_safe_str(&chunk, te->notes, TRUE, "__notes__");
            qp_tb_emit_note(st, &chunk);
            chunk.len = 0;
        }
    }
    qp_text_free(&chunk);
}

static const char qp_tb_cause_message[] =
    "\nThe above exception was the direct cause of the following exception:\n\n";
static const char qp_tb_context_message[] =
    "\nDuring handling of the above exception, another exception occurred:\n\n";

/* TracebackException.format(chain=True) */
static void qp_te_format(QPTBState *st, QPTE *self)
{
    JSContext *ctx = st->ctx;
    struct { const char *msg; QPTE *te; } *output = NULL;
    int n = 0, size = 0, k;
    QPTE *exc = self;

    while (exc) {
        const char *msg = NULL;
        QPTE *next = NULL;
        if (exc->cause) {
            msg = qp_tb_cause_message;
            next = exc->cause;
        } else if (exc->context && !exc->suppress_context) {
            msg = qp_tb_context_message;
            next = exc->context;
        }
        if (n == size) {
            int ns = size ? size * 2 : 8;
            void *no = js_realloc(ctx, output, ns * sizeof(*output));
            if (!no) {
                qp_tb_clear_exc(ctx);
                break;
            }
            output = no;
            size = ns;
        }
        output[n].msg = msg;
        output[n].te = exc;
        n++;
        exc = next;
    }
    for (k = n - 1; k >= 0; k--) {
        QPTE *te = output[k].te;
        if (output[k].msg)
            qp_tb_emit_cstr(st, output[k].msg, '|');
        if (!te->has_excs) {
            if (te->nframes) {
                qp_tb_emit_cstr(st, "Traceback (most recent call last):\n", '|');
                qp_tb_emit_stack(st, te);
            }
            qp_tb_emit_exception_only(st, te);
        } else if (st->group_depth > st->max_group_depth) {
            char buf[64];
            snprintf(buf, sizeof(buf), "... (max_group_depth is %d)\n", st->max_group_depth);
            qp_tb_emit_cstr(st, buf, '|');
        } else {
            BOOL is_toplevel = st->group_depth == 0;
            int i, cnt;
            if (is_toplevel)
                st->group_depth++;
            if (te->nframes) {
                qp_tb_emit_cstr(st, "Exception Group Traceback (most recent call last):\n",
                                is_toplevel ? '+' : '|');
                qp_tb_emit_stack(st, te);
            }
            qp_tb_emit_exception_only(st, te);
            cnt = te->nexcs <= st->max_group_width ? te->nexcs : st->max_group_width + 1;
            st->need_close = FALSE;
            for (i = 0; i < cnt; i++) {
                BOOL last_exc = i == cnt - 1;
                BOOL truncated = i >= st->max_group_width;
                char title[32];
                if (last_exc)
                    st->need_close = TRUE;
                if (truncated)
                    snprintf(title, sizeof(title), "...");
                else
                    snprintf(title, sizeof(title), "%d", i + 1);
                qp_text_fill(&st->out, ' ', 2 * st->group_depth);
                qp_text_puts(&st->out, i == 0 ? "+-" : "  ");
                qp_text_printf(&st->out, "+---------------- %s ----------------\n", title);
                st->group_depth++;
                if (!truncated) {
                    qp_te_format(st, te->excs[i]);
                } else {
                    int remaining = te->nexcs - st->max_group_width;
                    char buf[64];
                    snprintf(buf, sizeof(buf), "and %d more exception%s\n", remaining,
                             remaining > 1 ? "s" : "");
                    qp_tb_emit_cstr(st, buf, '|');
                }
                if (last_exc && st->need_close) {
                    qp_text_fill(&st->out, ' ', 2 * st->group_depth);
                    qp_text_puts(&st->out, "+------------------------------------\n");
                    st->need_close = FALSE;
                }
                st->group_depth--;
            }
            if (is_toplevel)
                st->group_depth = 0;
        }
    }
    js_free(ctx, output);
}

/* ========================================================================= */
/* Display                                                                    */
/* ========================================================================= */

/* traceback._safe_can_colorize(file=file): _colorize.can_colorize once
   _colorize is imported (it may be patched, or _colorize.COLORIZE
   cleared), else the same rules here.  An error means no colour (the
   fallback printer of PyErr_Display). */
static BOOL qp_tb_can_colorize(JSContext *ctx, JSValueConst file)
{
    JSValue mod = qp_tb_loaded_module(ctx, "_colorize"), fn, kw, res, fd;
    const char *e;
    Py_ssize_t v;
    int r;

    if (!QP_IS_NULL(mod)) {
        fn = qp_te_getattr_opt(ctx, mod, "can_colorize");
        JS_FreeValue(ctx, mod);
        if (JS_IsException(fn) || QP_IS_NULL(fn)) {
            qp_tb_clear_exc(ctx);
            return FALSE;
        }
        kw = qp_str_from_cstr(ctx, "file");
        if (!JS_IsException(kw)) {
            JSValue kwnames = qp_tuple_pack(ctx, 1, (JSValueConst *)&kw);
            JS_FreeValue(ctx, kw);
            res = JS_IsException(kwnames) ? JS_EXCEPTION :
                qp_vectorcall(ctx, fn, &file, 0, kwnames);
            JS_FreeValue(ctx, kwnames);
        } else {
            res = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, fn);
        r = JS_IsException(res) ? 0 : qp_bool(ctx, res);
        JS_FreeValue(ctx, res);
        qp_tb_clear_exc(ctx);
        return r > 0;
    }
    /* _colorize.can_colorize */
    if (!QP_STATE(ctx)->config.ignore_environment && (e = getenv("PYTHON_COLORS"))) {
        if (!strcmp(e, "0"))
            return FALSE;
        if (!strcmp(e, "1"))
            return TRUE;
    }
    if ((e = getenv("NO_COLOR")) && *e)
        return FALSE;
    if ((e = getenv("FORCE_COLOR")) && *e)
        return TRUE;
    if ((e = getenv("TERM")) && !strcmp(e, "dumb"))
        return FALSE;
    if (JS_IsUndefined(file))
        return FALSE;
    /* os.isatty(file.fileno()) */
    fd = qp_call_method(ctx, file, QP_ATOM(ctx, fileno), NULL, 0, QP_NULL);
    if (JS_IsException(fd)) {
        qp_tb_clear_exc(ctx);
        return FALSE;
    }
    r = qp_tb_as_ssize(ctx, fd, &v) && v >= 0 && v < INT32_MAX && isatty((int)v);
    JS_FreeValue(ctx, fd);
    return r;
}

/* _colorize.get_theme(force_color=True).traceback: the theme of _colorize
   once it is imported (set_theme), else the default one.  0 or -1. */
static int qp_tb_load_theme(JSContext *ctx, QPTBState *st)
{
    static const char *const names[QP_TH__COUNT] = {
        "type", "message", "note", "filename", "line_no", "frame",
        "error_highlight", "error_range", "reset",
    };
    static const char *const defaults[QP_TH__COUNT] = {
        "\x1b[1;35m", "\x1b[35m", "\x1b[36m", "\x1b[35m", "\x1b[35m", "\x1b[35m",
        "\x1b[1;31m", "\x1b[31m", "\x1b[0m",
    };
    JSValue mod = qp_tb_loaded_module(ctx, "_colorize"), tb = QP_NULL;
    int i;

    if (!QP_IS_NULL(mod)) {
        JSValue fn = qp_te_getattr_opt(ctx, mod, "get_theme"), kw, kwnames, arg = JS_TRUE, theme;
        JS_FreeValue(ctx, mod);
        if (JS_IsException(fn) || QP_IS_NULL(fn))
            goto fail;
        kw = qp_str_from_cstr(ctx, "force_color");
        kwnames = JS_IsException(kw) ? JS_EXCEPTION : qp_tuple_pack(ctx, 1, (JSValueConst *)&kw);
        JS_FreeValue(ctx, kw);
        theme = JS_IsException(kwnames) ? JS_EXCEPTION :
            qp_vectorcall(ctx, fn, (JSValueConst *)&arg, 0, kwnames);
        JS_FreeValue(ctx, kwnames);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(theme))
            goto fail;
        tb = qp_te_getattr_opt(ctx, theme, "traceback");
        JS_FreeValue(ctx, theme);
        if (JS_IsException(tb) || QP_IS_NULL(tb))
            goto fail;
    }
    for (i = 0; i < QP_TH__COUNT; i++) {
        JSValue v;
        if (QP_IS_NULL(tb)) {
            v = qp_str_from_cstr(ctx, defaults[i]);
        } else {
            v = qp_te_getattr_opt(ctx, tb, names[i]);
            if (!JS_IsException(v) && !qp_str_check(ctx, v)) {
                JSValue s = QP_IS_NULL(v) ? JS_EXCEPTION : qp_str(ctx, v);
                JS_FreeValue(ctx, v);
                v = s;
            }
        }
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, tb);
            goto fail;
        }
        st->theme[i] = v;
    }
    JS_FreeValue(ctx, tb);
    return 0;
 fail:
    qp_tb_clear_exc(ctx);
    for (i = 0; i < QP_TH__COUNT; i++) {
        JS_FreeValue(ctx, st->theme[i]);
        st->theme[i] = JS_UNDEFINED;
    }
    return -1;
}

/* traceback._print_exception_bltin(exc, file) / PyErr_Display.  file QP_NULL
   = sys.stderr (sys.__stderr__ when None, C stderr when unusable). */
static int qp_exc_display(JSContext *ctx, JSValueConst exc, JSValueConst file_in)
{
    QPTBState st;
    JSValue file, saved, text;
    int ret = -1, i;

    saved = JS_HasException(ctx) ? JS_GetException(ctx) : QP_NULL;
    if (QP_IS_NULL(file_in)) {
        file = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
        if (JS_IsException(file)) {
            qp_tb_clear_exc(ctx);
            file = QP_NULL;
        }
        if (QP_TB_NONE_OR_NULL(file)) {
            JSAtom a = JS_NewAtom(ctx, "__stderr__");
            JS_FreeValue(ctx, file);
            file = a != JS_ATOM_NULL ? qp_sys_get(ctx, a) : QP_NULL;
            if (a != JS_ATOM_NULL)
                JS_FreeAtom(ctx, a);
            if (JS_IsException(file)) {
                qp_tb_clear_exc(ctx);
                file = QP_NULL;
            }
        }
        if (QP_IS_NULL(file))
            file = JS_UNDEFINED;
    } else {
        file = JS_DupValue(ctx, file_in);
    }

    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    st.max_group_width = 15;
    st.max_group_depth = 10;
    for (i = 0; i < QP_TH__COUNT; i++)
        st.theme[i] = JS_UNDEFINED;
    st.colorize = qp_tb_can_colorize(ctx, file) && qp_tb_load_theme(ctx, &st) == 0;
    qp_text_init(&st.out, ctx);
    if (!qp_exc_check(ctx, exc)) {
        /* print_exception_invalid_type */
        char buf[256];
        QPType *t = qp_type_of(ctx, exc);
        qp_text_puts(&st.out, "TypeError: print_exception(): Exception expected for value, ");
        qp_text_puts(&st.out, t->tp_name ? t->tp_name : JS_AtomGetStr(ctx, buf, sizeof(buf), t->name));
        qp_text_puts(&st.out, " found\n");
    } else {
        QPTE *te = qp_te_build(&st, exc);
        if (te)
            qp_te_format(&st, te);
    }
    text = qp_text_to_str(&st.out);
    if (!JS_IsException(text)) {
        ret = qp_file_write_str(ctx, file, text);
        JS_FreeValue(ctx, text);
        if (ret == 0)
            qp_file_flush(ctx, file);
    }
    qp_te_free_all(&st);
    qp_tb_free_sources(ctx, st.sources);
    js_free(ctx, st.seen);
    qp_text_free(&st.out);
    for (i = 0; i < QP_TH__COUNT; i++)
        JS_FreeValue(ctx, st.theme[i]);
    JS_FreeValue(ctx, file);
    qp_tb_clear_exc(ctx);
    if (!QP_IS_NULL(saved))
        JS_Throw(ctx, saved);
    return ret;
}
