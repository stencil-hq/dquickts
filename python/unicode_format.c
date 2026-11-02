/*
 * str formatting: str % args (PyUnicode_Format), str.format() /
 * str.format_map() (PEP 3101), str.__format__, and the _string module
 * helpers formatter_parser() / formatter_field_name_split() used by
 * string.Formatter.
 *
 * Derived from CPython 3.15.2 Objects/unicode_format.c,
 * Objects/stringlib/unicode_format.h and Objects/unicodeobject.c
 * (unicode___format___impl, unicode_mod) (PSF-2.0).
 *
 * Output is accumulated in a QuickJS StringBuffer (UTF-16; code points
 * >= 0x10000 become surrogate pairs).  Format strings are read through
 * code-point views (qp_str_view_acquire), so every index and width below
 * counts code points exactly like CPython.
 */

/* Interface used by unicodeobject.c (str type spec) and templateobject.c. */
static JSValue qp_str_format_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_format_map_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_format_dunder(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_remainder(JSContext *ctx, JSValueConst a, JSValueConst b);
static int qp_init_str_formatter(JSContext *ctx);

/* ======================================================================= */
/* Shared helpers                                                          */
/* ======================================================================= */

/* Append v[start:end) (code points). */
static int qpf_put_view(StringBuffer *b, const qp_strview *v, Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t i;

    if (end <= start)
        return 0;
    switch (v->kind) {
    case 1:
        return string_buffer_write8(b, (const uint8_t *)v->data + start, (int)(end - start));
    case 2:
        return string_buffer_write16(b, (const uint16_t *)v->data + start, (int)(end - start));
    default:
        for (i = start; i < end; i++) {
            if (string_buffer_putc(b, ((const uint32_t *)v->data)[i]))
                return -1;
        }
        return 0;
    }
}

/* Append n copies of code point ch. */
static int qpf_fill(StringBuffer *b, Py_UCS4 ch, Py_ssize_t n)
{
    if (n <= 0)
        return 0;
    if (ch < 0x10000)
        return string_buffer_fill(b, (int)ch, (int)n);
    while (n-- > 0) {
        if (string_buffer_putc(b, ch))
            return -1;
    }
    return 0;
}

/* Append a str (exact or str-subclass instance). */
static int qpf_put_str(JSContext *ctx, StringBuffer *b, JSValueConst s)
{
    JSValue v;
    int r;

    if (qp_str_check_exact(s))
        return string_buffer_concat_value(b, s);
    v = qp_str_value(ctx, s);
    if (JS_IsException(v))
        return -1;
    r = string_buffer_concat_value(b, v);
    JS_FreeValue(ctx, v);
    return r;
}

/* Append WTF-8 text (numfmt.h output: lone surrogates as 3-byte forms). */
static int qpf_put_wtf8(StringBuffer *b, const uint8_t *p, size_t len)
{
    const uint8_t *end = p + len, *next;
    int c;

    while (p < end) {
        if (*p < 0x80) {
            if (string_buffer_putc8(b, *p++))
                return -1;
            continue;
        }
        c = unicode_from_utf8(p, (int)(end - p), &next);
        if (c < 0) {            /* not produced by numfmt; keep the byte */
            c = *p;
            next = p + 1;
        }
        if (string_buffer_putc(b, (uint32_t)c))
            return -1;
        p = next;
    }
    return 0;
}

static JSValue qpf_str_from_wtf8(JSContext *ctx, const uint8_t *p, size_t len)
{
    StringBuffer b;

    string_buffer_init(ctx, &b, (int)len);
    if (qpf_put_wtf8(&b, p, len)) {
        string_buffer_free(&b);
        return JS_EXCEPTION;
    }
    return string_buffer_end(&b);
}

/* Raise the exception described by a numfmt.h error and clear it. */
static JSValue qpf_raise_numfmt(JSContext *ctx, qp_error *err)
{
    const char *msg = err->msg ? err->msg : "";

    switch (err->kind) {
    case QP_ERR_OVERFLOW:
        qp_raise_overflow_error(ctx, "%s", msg);
        break;
    case QP_ERR_MEMORY:
        qp_raise_memory_error(ctx);
        break;
    default:
        qp_raise_value_error(ctx, "%s", msg);
        break;
    }
    qp_error_clear(err);
    return JS_EXCEPTION;
}

/* Py_TYPE(v)->tp_name (UTF-8): the spec name of builtin types, __name__ of
   heap types. */
static const char *qpf_tp_name(JSContext *ctx, JSValueConst v, char *buf, int size)
{
    QPType *t = qp_type_of(ctx, v);

    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* _PyArg_BadArgument: "%.200s() %.200s must be %.50s, not %.50s". */
static JSValue qpf_bad_argument(JSContext *ctx, const char *fname, const char *displayname,
                                const char *expected, JSValueConst arg)
{
    char buf[ATOM_GET_STR_BUF_SIZE];

    return qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50s",
                               fname, displayname, expected,
                               JS_IsUndefined(arg) || JS_IsNull(arg) ? "None"
                               : qpf_tp_name(ctx, arg, buf, sizeof(buf)));
}

/* Drop the pending exception (PyErr_Format clears it before raising). */
static void qpf_clear_exception(JSContext *ctx)
{
    JS_FreeValue(ctx, JS_GetException(ctx));
}

/* Plain str value + code-point view of a str (or str subclass instance).
   On success *pstr holds a new reference to release with qpf_view_end. */
static int qpf_view_begin(JSContext *ctx, JSValueConst s, JSValue *pstr, qp_strview *view)
{
    JSValue v = qp_str_value(ctx, s);

    if (JS_IsException(v))
        return -1;
    if (qp_str_view_acquire(ctx, v, view) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    *pstr = v;
    return 0;
}

static void qpf_view_end(JSContext *ctx, JSValue str, qp_strview *view)
{
    qp_str_view_release(ctx, view);
    JS_FreeValue(ctx, str);
}

/* Length in code points of a str or str subclass instance (-1 on error). */
static Py_ssize_t qpf_str_len_any(JSContext *ctx, JSValueConst s)
{
    JSValue v;
    Py_ssize_t len;

    if (qp_str_check_exact(s))
        return qp_str_len(ctx, s);
    v = qp_str_value(ctx, s);
    if (JS_IsException(v))
        return -1;
    len = qp_str_len(ctx, v);
    JS_FreeValue(ctx, v);
    return len;
}

/* PyNumber_Check: __index__, __int__, __float__ or complex. */
static BOOL qpf_number_check(JSContext *ctx, JSValueConst v)
{
    QPType *t;

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_FLOAT64:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return TRUE;
    default:
        break;
    }
    t = qp_type_of(ctx, v);
    return t->slots.nb.nb_index || t->slots.nb.nb_int || t->slots.nb.nb_float ||
        qp_complex_check(ctx, v);
}

/* PyMapping_Check: the type has mp_subscript. */
static BOOL qpf_mapping_check(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.mp.mp_subscript != NULL;
}

/* ======================================================================= */
/* str.__format__                                                          */
/* ======================================================================= */

/* _PyUnicode_FormatAdvancedWriter for a str `obj` and a str `spec`. */
static JSValue qpf_format_str_spec(JSContext *ctx, JSValueConst obj, JSValueConst spec)
{
    JSValue spec_str, obj_str, res;
    qp_strview spec_view, obj_view;
    qp_error err = { 0 };
    DynBuf db;
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *tp_name;
    int r;

    if (qpf_view_begin(ctx, spec, &spec_str, &spec_view))
        return JS_EXCEPTION;
    if (spec_view.len == 0) {
        /* empty spec: str(obj) */
        qpf_view_end(ctx, spec_str, &spec_view);
        if (qp_str_check_exact(obj))
            return JS_DupValue(ctx, obj);
        return qp_str(ctx, obj);
    }
    if (qpf_view_begin(ctx, obj, &obj_str, &obj_view)) {
        qpf_view_end(ctx, spec_str, &spec_view);
        return JS_EXCEPTION;
    }
    tp_name = qpf_tp_name(ctx, obj, buf, sizeof(buf));
    js_dbuf_init(ctx, &db);
    r = qp_format_str(&db, obj_view.data, obj_view.kind, obj_view.len,
                      spec_view.data, spec_view.kind, spec_view.len, tp_name, &err);
    qpf_view_end(ctx, obj_str, &obj_view);
    qpf_view_end(ctx, spec_str, &spec_view);
    if (r < 0) {
        dbuf_free(&db);
        return qpf_raise_numfmt(ctx, &err);
    }
    if (db.error) {
        dbuf_free(&db);
        return qp_raise_memory_error(ctx);
    }
    res = qpf_str_from_wtf8(ctx, db.buf, db.size);
    dbuf_free(&db);
    return res;
}

/* str.__format__(format_spec, /) */
static JSValue qp_str_format_dunder(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    if (!qp_str_check(ctx, args[0]))
        return qpf_bad_argument(ctx, "__format__", "argument", "str", args[0]);
    return qpf_format_str_spec(ctx, self, args[0]);
}

/* ======================================================================= */
/* str % args (PyUnicode_Format)                                           */
/* ======================================================================= */

#define QPF_F_LJUST (1 << 0)
#define QPF_F_SIGN  (1 << 1)
#define QPF_F_BLANK (1 << 2)
#define QPF_F_ALT   (1 << 3)
#define QPF_F_ZERO  (1 << 4)

#define QPF_MAX_UNICODE 0x10FFFF

typedef struct QPFmtState {
    JSContext *ctx;
    JSValue args;               /* borrowed, or owned when args_owned */
    int args_owned;
    Py_ssize_t arglen, argidx;
    JSValueConst dict;          /* QP_NULL: not a mapping */
    qp_strview fmt;
    JSValueConst fmtstr;        /* plain str of the format */
    Py_ssize_t fmtcnt, fmtpos;
    StringBuffer out;
} QPFmtState;

typedef struct QPFmtArg {
    Py_UCS4 ch;
    int flags;
    Py_ssize_t width;
    int prec;
    int sign;
    Py_ssize_t fmtstart;
    JSValue key;                /* QP_NULL unless %(key) */
} QPFmtArg;

/* PyUnicode_READ of the format; the NUL terminator CPython reads at the
   end of the string is 0 here. */
static inline Py_UCS4 qpf_fmt_read(const QPFmtState *st, Py_ssize_t pos)
{
    if (pos >= st->fmt.len)
        return 0;
    return QP_SL_READ(st->fmt.kind, st->fmt.data, pos);
}

/* FORMAT_ERROR(): "format argument %R: ", "format argument %zd: " or
   "format argument: " followed by the formatted text.  Clears any pending
   exception first (PyErr_Format semantics). */
static void qpf_format_error(QPFmtState *st, QPFmtArg *arg, QPType *exc, const char *fmt, ...)
{
    JSContext *ctx = st->ctx;
    JSValue tail;
    va_list ap;

    qpf_clear_exception(ctx);
    va_start(ap, fmt);
    tail = qp_str_from_format_v(ctx, fmt, ap);
    va_end(ap);
    if (JS_IsException(tail))
        return;
    if (!QP_IS_NULL(arg->key))
        qp_raise(ctx, exc, "format argument %R: %U", arg->key, tail);
    else if (st->argidx >= 0)
        qp_raise(ctx, exc, "format argument %zd: %U", st->argidx, tail);
    else
        qp_raise(ctx, exc, "format argument: %U", tail);
    JS_FreeValue(ctx, tail);
}

/* Items of a tuple (or tuple subclass instance). */
static JSValue *qpf_tuple_items(JSValueConst t)
{
    return QP_TUPLE_ITEMS(t);
}

/* unicode_format_getnextarg: borrowed argument or JS_EXCEPTION. */
static JSValue qpf_getnextarg(QPFmtState *st, int allowone)
{
    Py_ssize_t argidx = st->argidx;

    if (argidx < st->arglen && (allowone || st->arglen >= 0)) {
        st->argidx++;
        if (st->arglen >= 0)
            return qpf_tuple_items(st->args)[argidx];
        else if (allowone)
            return st->args;
    }
    qp_raise_type_error(st->ctx, "not enough arguments for format string (got %zd)",
                        st->arglen < 0 ? (Py_ssize_t)1 : st->arglen);
    return JS_EXCEPTION;
}

/* formatfloat(): new str, or JS_EXCEPTION. */
static JSValue qpf_formatfloat(QPFmtState *st, QPFmtArg *arg, JSValueConst v)
{
    JSContext *ctx = st->ctx;
    double x;
    int prec, dtoa_flags = 0;
    char *p;
    qp_error err = { 0 };
    JSValue res;

    if (qp_float_as_double(ctx, v, &x) < 0) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            qpf_format_error(st, arg, QP_EXC(ctx, TypeError),
                             "%%%c requires a real number, not %T", (int)arg->ch, v);
        return JS_EXCEPTION;
    }
    prec = arg->prec;
    if (prec < 0)
        prec = 6;
    if (arg->flags & QPF_F_ALT)
        dtoa_flags |= QP_DTSF_ALT;
    p = qp_double_to_string(x, (char)arg->ch, prec, dtoa_flags, NULL, &err);
    if (!p)
        return qpf_raise_numfmt(ctx, &err);
    res = JS_NewStringLen(ctx, p, strlen(p));
    qp_sys_free(p);
    return res;
}

/* _PyUnicode_FormatLong(): "-"? ("0x" | "0X" | "0o")? digit+ with at least
   prec digits; val is an int (bool and subclasses included). */
static JSValue qpf_format_long(JSContext *ctx, JSValueConst val, int alt, int prec, int type)
{
    JSValue plain, digits_str, res;
    const char *s;
    size_t slen, i, numdigits, numnondigits, pos;
    int base, sign;
    char *buf;

    if (prec > INT_MAX - 3)
        return qp_raise_overflow_error(ctx, "precision too large");
    switch (type) {
    case 'o':
        base = 8;
        break;
    case 'x':
    case 'X':
        base = 16;
        break;
    default:
        base = 10;
        break;
    }
    plain = qp_long_value(ctx, val);
    if (JS_IsException(plain))
        return JS_EXCEPTION;
    digits_str = qp_long_to_str(ctx, plain, base, base != 10);
    JS_FreeValue(ctx, plain);
    if (JS_IsException(digits_str))
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &slen, digits_str);
    JS_FreeValue(ctx, digits_str);
    if (!s)
        return JS_EXCEPTION;
    if (slen > INT_MAX) {
        JS_FreeCString(ctx, s);
        return qp_raise_value_error(ctx, "string too large in _PyUnicode_FormatLong");
    }
    sign = s[0] == '-';
    numnondigits = sign + (base != 10 ? 2 : 0);
    numdigits = slen - numnondigits;
    buf = js_malloc(ctx, numnondigits + (prec > (int)numdigits ? (size_t)prec : numdigits) + 1);
    if (!buf) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    pos = 0;
    if (sign)
        buf[pos++] = '-';
    /* base marker only with the alternate form */
    if (base != 10 && alt) {
        buf[pos++] = s[sign];
        buf[pos++] = s[sign + 1];
    }
    /* zero fill up to prec digits */
    for (i = numdigits; (int)i < prec; i++)
        buf[pos++] = '0';
    memcpy(buf + pos, s + numnondigits, numdigits);
    pos += numdigits;
    JS_FreeCString(ctx, s);
    if (type == 'X') {
        for (i = 0; i < pos; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'x')
                buf[i] -= 'a' - 'A';
        }
    }
    res = JS_NewStringLen(ctx, buf, pos);
    js_free(ctx, buf);
    return res;
}

/* mainformatlong(): 1 written to the output, 0 *p_output set, -1 error. */
static int qpf_mainformatlong(QPFmtState *st, QPFmtArg *arg, JSValueConst v, JSValue *p_output)
{
    JSContext *ctx = st->ctx;
    JSValue iobj, res;
    int type = (int)arg->ch;

    if (!qpf_number_check(ctx, v))
        goto wrongtype;

    /* make sure number is a type of integer for o, x, and X */
    if (!qp_long_check(ctx, v)) {
        if (type == 'o' || type == 'x' || type == 'X')
            iobj = qp_index(ctx, v);
        else
            iobj = qp_number_long(ctx, v);
        if (JS_IsException(iobj)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
                goto wrongtype;
            return -1;
        }
    } else {
        iobj = JS_DupValue(ctx, v);
    }

    if (qp_long_check_exact(v) && arg->width == -1 && arg->prec == -1 &&
        !(arg->flags & (QPF_F_SIGN | QPF_F_BLANK)) && type != 'X') {
        /* Fast path */
        int base = type == 'o' ? 8 : type == 'x' ? 16 : 10;
        JSValue s = qp_long_to_str(ctx, v, base, arg->flags & QPF_F_ALT);
        int r;

        JS_FreeValue(ctx, iobj);
        if (JS_IsException(s))
            return -1;
        r = string_buffer_concat_value(&st->out, s);
        JS_FreeValue(ctx, s);
        return r < 0 ? -1 : 1;
    }

    res = qpf_format_long(ctx, iobj, arg->flags & QPF_F_ALT, arg->prec, type);
    JS_FreeValue(ctx, iobj);
    if (JS_IsException(res))
        return -1;
    *p_output = res;
    return 0;

wrongtype:
    switch (type) {
    case 'o':
    case 'x':
    case 'X':
        qpf_format_error(st, arg, QP_EXC(ctx, TypeError),
                         "%%%c requires an integer, not %T", type, v);
        break;
    default:
        qpf_format_error(st, arg, QP_EXC(ctx, TypeError),
                         "%%%c requires a real number, not %T", type, v);
        break;
    }
    return -1;
}

/* formatchar(): code point, or (Py_UCS4)-1 on error. */
static Py_UCS4 qpf_formatchar(QPFmtState *st, QPFmtArg *arg, JSValueConst v)
{
    JSContext *ctx = st->ctx;

    if (qp_str_check(ctx, v)) {
        Py_ssize_t len = qpf_str_len_any(ctx, v);
        JSValue s;
        qp_strview view;
        Py_UCS4 ch;

        if (len < 0)
            return (Py_UCS4)-1;
        if (len == 1) {
            if (qpf_view_begin(ctx, v, &s, &view))
                return (Py_UCS4)-1;
            ch = QP_SL_READ(view.kind, view.data, 0);
            qpf_view_end(ctx, s, &view);
            return ch;
        }
        qpf_format_error(st, arg, QP_EXC(ctx, TypeError),
                         "%%c requires an integer or a unicode character, "
                         "not a string of length %zd", len);
        return (Py_UCS4)-1;
    } else {
        JSValue iv;
        int64_t x;
        int overflow = 0;

        /* PyLong_AsLongAndOverflow: __index__ for non-ints */
        if (qp_long_check(ctx, v))
            iv = qp_long_value(ctx, v);
        else
            iv = qp_index(ctx, v);
        if (JS_IsException(iv)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
                qpf_format_error(st, arg, QP_EXC(ctx, TypeError),
                                 "%%c requires an integer or a unicode character, not %T", v);
            return (Py_UCS4)-1;
        }
        qp_long_as_int64_overflow(iv, &x, &overflow);
        JS_FreeValue(ctx, iv);
        if (overflow || x < 0 || x > QPF_MAX_UNICODE) {
            /* this includes an overflow in converting to C long */
            qpf_format_error(st, arg, QP_EXC(ctx, OverflowError),
                             "%%c argument not in range(0x110000)%s", "");
            return (Py_UCS4)-1;
        }
        return (Py_UCS4)x;
    }
}

/* unicode_format_arg_parse(): mapping key, flags, width, precision. */
static int qpf_arg_parse(QPFmtState *st, QPFmtArg *arg)
{
    JSContext *ctx = st->ctx;
    JSValue v;

    if (arg->ch == '(') {
        /* Get argument value from a dictionary. Example: "%(name)s". */
        Py_ssize_t keystart, keylen;
        int pcount = 1;

        if (QP_IS_NULL(st->dict)) {
            qp_raise_type_error(ctx, "format requires a mapping, not %T", st->args);
            return -1;
        }
        ++st->fmtpos;
        --st->fmtcnt;
        keystart = st->fmtpos;
        /* Skip over balanced parentheses */
        while (pcount > 0 && --st->fmtcnt >= 0) {
            arg->ch = qpf_fmt_read(st, st->fmtpos);
            if (arg->ch == ')')
                --pcount;
            else if (arg->ch == '(')
                ++pcount;
            st->fmtpos++;
        }
        keylen = st->fmtpos - keystart - 1;
        if (st->fmtcnt < 0 || pcount > 0) {
            qp_raise_value_error(ctx, "stray %% or incomplete format key at position %zd",
                                 arg->fmtstart);
            return -1;
        }
        arg->key = qp_str_substring(ctx, st->fmtstr, keystart, keystart + keylen);
        if (JS_IsException(arg->key)) {
            arg->key = QP_NULL;
            return -1;
        }
        if (st->args_owned) {
            st->args_owned = 0;
            JS_FreeValue(ctx, st->args);
            st->args = JS_UNDEFINED;
        }
        v = qp_getitem(ctx, st->dict, arg->key);
        if (JS_IsException(v))
            return -1;
        st->args = v;
        st->args_owned = 1;
        st->arglen = -3;
        st->argidx = -4;
    } else {
        if (st->arglen < -1) {
            qp_raise_value_error(ctx, "format requires a parenthesised mapping key "
                                 "at position %zd", arg->fmtstart);
            return -1;
        }
    }

    /* Parse flags. Example: "%+i" => flags=F_SIGN. */
    while (--st->fmtcnt >= 0) {
        arg->ch = qpf_fmt_read(st, st->fmtpos);
        st->fmtpos++;
        switch (arg->ch) {
        case '-': arg->flags |= QPF_F_LJUST; continue;
        case '+': arg->flags |= QPF_F_SIGN; continue;
        case ' ': arg->flags |= QPF_F_BLANK; continue;
        case '#': arg->flags |= QPF_F_ALT; continue;
        case '0': arg->flags |= QPF_F_ZERO; continue;
        }
        break;
    }

    /* Parse width. Example: "%10s" => width=10 */
    if (arg->ch == '*') {
        if (st->arglen < -1) {
            qp_raise_value_error(ctx, "* cannot be used with a parenthesised mapping key "
                                 "at position %zd", arg->fmtstart);
            return -1;
        }
        v = qpf_getnextarg(st, 0);
        if (JS_IsException(v))
            return -1;
        if (!qp_long_check(ctx, v)) {
            qpf_format_error(st, arg, QP_EXC(ctx, TypeError), "* requires int, not %T", v);
            return -1;
        } else {
            JSValue iv = qp_long_value(ctx, v);
            int r;

            if (JS_IsException(iv))
                return -1;
            r = qp_long_as_ssize(ctx, iv, &arg->width);
            JS_FreeValue(ctx, iv);
            if (r < 0) {
                if (qp_exc_pending_matches(ctx, QP_EXC(ctx, OverflowError)))
                    qpf_format_error(st, arg, QP_EXC(ctx, OverflowError),
                                     "too big for width%s", "");
                return -1;
            }
        }
        if (arg->width < 0) {
            if (arg->width == PY_SSIZE_T_MIN) {
                qpf_format_error(st, arg, QP_EXC(ctx, OverflowError), "too big for width%s", "");
                return -1;
            }
            arg->flags |= QPF_F_LJUST;
            arg->width = -arg->width;
        }
        if (--st->fmtcnt >= 0) {
            arg->ch = qpf_fmt_read(st, st->fmtpos);
            st->fmtpos++;
        }
    } else if (arg->ch >= '0' && arg->ch <= '9') {
        arg->width = arg->ch - '0';
        while (--st->fmtcnt >= 0) {
            arg->ch = qpf_fmt_read(st, st->fmtpos);
            st->fmtpos++;
            if (arg->ch < '0' || arg->ch > '9')
                break;
            if (arg->width > (PY_SSIZE_T_MAX - ((int)arg->ch - '0')) / 10) {
                qp_raise_value_error(ctx, "width too big at position %zd", arg->fmtstart);
                return -1;
            }
            arg->width = arg->width * 10 + (arg->ch - '0');
        }
    }

    /* Parse precision. Example: "%.3f" => prec=3 */
    if (arg->ch == '.') {
        arg->prec = 0;
        if (--st->fmtcnt >= 0) {
            arg->ch = qpf_fmt_read(st, st->fmtpos);
            st->fmtpos++;
        }
        if (arg->ch == '*') {
            if (st->arglen < -1) {
                qp_raise_value_error(ctx, "* cannot be used with a parenthesised mapping key "
                                     "at position %zd", arg->fmtstart);
                return -1;
            }
            v = qpf_getnextarg(st, 0);
            if (JS_IsException(v))
                return -1;
            if (!qp_long_check(ctx, v)) {
                qpf_format_error(st, arg, QP_EXC(ctx, TypeError), "* requires int, not %T", v);
                return -1;
            } else {
                /* PyLong_AsInt */
                JSValue iv = qp_long_value(ctx, v);
                int64_t p;
                int overflow = 0;

                if (JS_IsException(iv))
                    return -1;
                qp_long_as_int64_overflow(iv, &p, &overflow);
                JS_FreeValue(ctx, iv);
                if (overflow || p > INT_MAX || p < INT_MIN) {
                    qpf_format_error(st, arg, QP_EXC(ctx, OverflowError),
                                     "too big for precision%s", "");
                    return -1;
                }
                arg->prec = (int)p;
            }
            if (arg->prec < 0)
                arg->prec = 0;
            if (--st->fmtcnt >= 0) {
                arg->ch = qpf_fmt_read(st, st->fmtpos);
                st->fmtpos++;
            }
        } else if (arg->ch >= '0' && arg->ch <= '9') {
            arg->prec = arg->ch - '0';
            while (--st->fmtcnt >= 0) {
                arg->ch = qpf_fmt_read(st, st->fmtpos);
                st->fmtpos++;
                if (arg->ch < '0' || arg->ch > '9')
                    break;
                if (arg->prec > (INT_MAX - ((int)arg->ch - '0')) / 10) {
                    qp_raise_value_error(ctx, "precision too big at position %zd",
                                         arg->fmtstart);
                    return -1;
                }
                arg->prec = arg->prec * 10 + (arg->ch - '0');
            }
        }
    }

    /* Ignore "h", "l" and "L" format prefix (ex: "%hi" or "%ls") */
    if (st->fmtcnt >= 0) {
        if (arg->ch == 'h' || arg->ch == 'l' || arg->ch == 'L') {
            if (--st->fmtcnt >= 0) {
                arg->ch = qpf_fmt_read(st, st->fmtpos);
                st->fmtpos++;
            }
        }
    }
    if (st->fmtcnt < 0) {
        qp_raise_value_error(ctx, "stray %% at position %zd", arg->fmtstart);
        return -1;
    }
    return 0;
}

/* unicode_format_arg_format(): 1 written to the output, 0 *p_str set, -1
   error. */
static int qpf_arg_format(QPFmtState *st, QPFmtArg *arg, JSValue *p_str)
{
    JSContext *ctx = st->ctx;
    JSValue v;

    v = qpf_getnextarg(st, 1);
    if (JS_IsException(v))
        return -1;

    switch (arg->ch) {
    case 's':
    case 'r':
    case 'a':
        if (qp_long_check_exact(v) && arg->width == -1 && arg->prec == -1) {
            /* Fast path */
            JSValue s = qp_long_to_str(ctx, v, 10, arg->flags & QPF_F_ALT);
            int r;

            if (JS_IsException(s))
                return -1;
            r = string_buffer_concat_value(&st->out, s);
            JS_FreeValue(ctx, s);
            return r < 0 ? -1 : 1;
        }
        if (qp_str_check_exact(v) && arg->ch == 's')
            *p_str = JS_DupValue(ctx, v);
        else if (arg->ch == 's')
            *p_str = qp_str(ctx, v);
        else if (arg->ch == 'r')
            *p_str = qp_repr(ctx, v);
        else
            *p_str = qp_ascii(ctx, v);
        break;

    case 'i':
    case 'd':
    case 'u':
    case 'o':
    case 'x':
    case 'X': {
        int ret = qpf_mainformatlong(st, arg, v, p_str);
        if (ret != 0)
            return ret;
        arg->sign = 1;
        break;
    }

    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
        if (arg->width == -1 && arg->prec == -1 &&
            !(arg->flags & (QPF_F_SIGN | QPF_F_BLANK))) {
            /* Fast path */
            JSValue s = qpf_formatfloat(st, arg, v);
            int r;

            if (JS_IsException(s))
                return -1;
            r = string_buffer_concat_value(&st->out, s);
            JS_FreeValue(ctx, s);
            return r < 0 ? -1 : 1;
        }
        arg->sign = 1;
        *p_str = qpf_formatfloat(st, arg, v);
        break;

    case 'c': {
        Py_UCS4 ch = qpf_formatchar(st, arg, v);
        if (ch == (Py_UCS4)-1)
            return -1;
        if (arg->width == -1 && arg->prec == -1) {
            /* Fast path */
            if (string_buffer_putc(&st->out, ch))
                return -1;
            return 1;
        }
        *p_str = qp_str_from_char(ctx, ch);
        break;
    }

    default:
        if (arg->ch < 128 && QP_ISALPHA(arg->ch)) {
            qp_raise_value_error(ctx, "unsupported format %%%c at position %zd",
                                 (int)arg->ch, arg->fmtstart);
        } else if (arg->ch == '\'') {
            qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                 "format character \"'\" at position %zd",
                                 arg->fmtstart, st->fmtpos - 1);
        } else if (arg->ch >= 32 && arg->ch < 127) {
            qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                 "format character '%c' at position %zd",
                                 arg->fmtstart, (int)arg->ch, st->fmtpos - 1);
        } else {
            char hex[16];

            snprintf(hex, sizeof(hex), "U+%04X", (unsigned)arg->ch);
            if (_PyUnicode_IsPrintable(arg->ch))
                qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                     "format character '%c' (%s) at position %zd",
                                     arg->fmtstart, (int)arg->ch, hex, st->fmtpos - 1);
            else
                qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                     "format character %s at position %zd",
                                     arg->fmtstart, hex, st->fmtpos - 1);
        }
        return -1;
    }
    if (JS_IsException(*p_str)) {
        *p_str = QP_NULL;
        return -1;
    }
    return 0;
}

/* unicode_format_arg_output(): sign, numeric prefix, padding, truncation. */
static int qpf_arg_output(QPFmtState *st, QPFmtArg *arg, JSValueConst str)
{
    JSContext *ctx = st->ctx;
    StringBuffer *b = &st->out;
    JSValue plain;
    qp_strview view;
    Py_ssize_t len, pindex;
    Py_UCS4 signchar, fill;
    int ret = -1;

    fill = ' ';
    if (arg->sign && (arg->flags & QPF_F_ZERO))
        fill = '0';

    if (qpf_view_begin(ctx, str, &plain, &view))
        return -1;
    len = view.len;
    if ((arg->width == -1 || arg->width <= len) &&
        (arg->prec == -1 || arg->prec >= len) &&
        !(arg->flags & (QPF_F_SIGN | QPF_F_BLANK))) {
        /* Fast path */
        ret = qpf_put_view(b, &view, 0, len);
        goto done;
    }

    /* Truncate the string for "s", "r" and "a" formats if the precision is
       set */
    if (arg->ch == 's' || arg->ch == 'r' || arg->ch == 'a') {
        if (arg->prec >= 0 && len > arg->prec)
            len = arg->prec;
    }

    /* Adjust sign and width */
    pindex = 0;
    signchar = '\0';
    if (arg->sign) {
        Py_UCS4 ch = len > 0 ? QP_SL_READ(view.kind, view.data, pindex) : 0;
        if (ch == '-' || ch == '+') {
            signchar = ch;
            len--;
            pindex++;
        } else if (arg->flags & QPF_F_SIGN) {
            signchar = '+';
        } else if (arg->flags & QPF_F_BLANK) {
            signchar = ' ';
        } else {
            arg->sign = 0;
        }
    }
    if (arg->width < len)
        arg->width = len;

    /* Write the sign if needed */
    if (arg->sign) {
        if (fill != ' ') {
            if (string_buffer_putc(b, signchar))
                goto done;
        }
        if (arg->width > len)
            arg->width--;
    }

    /* Write the numeric prefix for "x", "X" and "o" formats if the
       alternate form is used. */
    if ((arg->flags & QPF_F_ALT) && (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
        if (fill != ' ') {
            if (string_buffer_putc(b, '0') || string_buffer_putc(b, arg->ch))
                goto done;
            pindex += 2;
        }
        arg->width -= 2;
        if (arg->width < 0)
            arg->width = 0;
        len -= 2;
    }

    /* Pad left with the fill character if needed */
    if (arg->width > len && !(arg->flags & QPF_F_LJUST)) {
        if (qpf_fill(b, fill, arg->width - len))
            goto done;
        arg->width = len;
    }

    /* If padding with spaces: write sign if needed and/or numeric prefix if
       the alternate form is used */
    if (fill == ' ') {
        if (arg->sign) {
            if (string_buffer_putc(b, signchar))
                goto done;
        }
        if ((arg->flags & QPF_F_ALT) && (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
            if (string_buffer_putc(b, '0') || string_buffer_putc(b, arg->ch))
                goto done;
            pindex += 2;
        }
    }

    /* Write characters */
    if (len > 0) {
        if (qpf_put_view(b, &view, pindex, pindex + len))
            goto done;
    }

    /* Pad right with the fill character if needed */
    if (arg->width > len) {
        if (qpf_fill(b, ' ', arg->width - len))
            goto done;
    }
    ret = 0;
done:
    qpf_view_end(ctx, plain, &view);
    return ret;
}

/* unicode_format_arg(): one '%' conversion (fmtpos is past the '%'). */
static int qpf_format_arg(QPFmtState *st)
{
    JSContext *ctx = st->ctx;
    QPFmtArg arg;
    JSValue str = QP_NULL;
    int ret;

    arg.ch = qpf_fmt_read(st, st->fmtpos);
    if (arg.ch == '%') {
        st->fmtpos++;
        st->fmtcnt--;
        return string_buffer_putc8(&st->out, '%');
    }
    arg.flags = 0;
    arg.width = -1;
    arg.prec = -1;
    arg.sign = 0;
    arg.fmtstart = st->fmtpos - 1;
    arg.key = QP_NULL;

    ret = qpf_arg_parse(st, &arg);
    if (ret == -1)
        goto onError;

    ret = qpf_arg_format(st, &arg, &str);
    if (ret == -1)
        goto onError;

    if (ret != 1) {
        ret = qpf_arg_output(st, &arg, str);
        JS_FreeValue(ctx, str);
        if (ret == -1)
            goto onError;
    }

    if (!QP_IS_NULL(st->dict) && st->argidx < st->arglen) {
        qp_raise_type_error(ctx, "not all arguments converted during string formatting");
        goto onError;
    }
    JS_FreeValue(ctx, arg.key);
    return 0;

onError:
    JS_FreeValue(ctx, arg.key);
    return -1;
}

/* PyUnicode_Format(format, args); `format` is a str or str subclass. */
static JSValue qpf_unicode_format(JSContext *ctx, JSValueConst format, JSValueConst args)
{
    QPFmtState st;
    JSValue fmtstr;
    BOOL is_tuple;

    if (qpf_view_begin(ctx, format, &fmtstr, &st.fmt))
        return JS_EXCEPTION;
    st.ctx = ctx;
    st.fmtstr = fmtstr;
    st.fmtcnt = st.fmt.len;
    st.fmtpos = 0;
    string_buffer_init(ctx, &st.out, (int)min_int64(st.fmt.len + 100, JS_STRING_LEN_MAX));

    is_tuple = qp_tuple_check(ctx, args);
    if (is_tuple) {
        st.arglen = QP_TUPLE_SIZE(args);
        st.argidx = 0;
    } else {
        st.arglen = -1;
        st.argidx = -2;
    }
    st.args_owned = 0;
    if (!is_tuple && !qp_str_check(ctx, args) && qpf_mapping_check(ctx, args))
        st.dict = args;
    else
        st.dict = QP_NULL;
    st.args = (JSValue)args;

    while (--st.fmtcnt >= 0) {
        if (qpf_fmt_read(&st, st.fmtpos) != '%') {
            Py_ssize_t nonfmtpos = st.fmtpos++;

            while (st.fmtcnt >= 0 && qpf_fmt_read(&st, st.fmtpos) != '%') {
                st.fmtpos++;
                st.fmtcnt--;
            }
            if (st.fmtcnt < 0)
                st.fmtpos--;
            if (qpf_put_view(&st.out, &st.fmt, nonfmtpos, st.fmtpos))
                goto onError;
        } else {
            st.fmtpos++;
            if (qpf_format_arg(&st) == -1)
                goto onError;
        }
    }

    if (st.argidx < st.arglen && QP_IS_NULL(st.dict)) {
        qp_raise_type_error(ctx, "not all arguments converted during string formatting "
                            "(required %zd, got %zd)",
                            st.arglen < 0 ? (Py_ssize_t)0 : st.argidx,
                            st.arglen < 0 ? (Py_ssize_t)1 : st.arglen);
        goto onError;
    }

    if (st.args_owned)
        JS_FreeValue(ctx, st.args);
    qpf_view_end(ctx, fmtstr, &st.fmt);
    return string_buffer_end(&st.out);

onError:
    string_buffer_free(&st.out);
    if (st.args_owned)
        JS_FreeValue(ctx, st.args);
    qpf_view_end(ctx, fmtstr, &st.fmt);
    return JS_EXCEPTION;
}

/* str.__mod__ (unicode_mod): nb_remainder slot of str. */
static JSValue qp_str_remainder(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    if (!qp_str_check(ctx, a))
        return qp_not_implemented(ctx);
    return qpf_unicode_format(ctx, a, b);
}

/* ======================================================================= */
/* str.format / str.format_map (Objects/stringlib/unicode_format.h)        */
/* ======================================================================= */

/* Characters [start, end) of a str; str == QP_NULL is CPython's NULL. */
typedef struct QPSubString {
    JSValueConst str;           /* plain str, borrowed */
    const qp_strview *view;     /* code points of str */
    Py_ssize_t start, end;
} QPSubString;

typedef enum {
    QPF_ANS_INIT,
    QPF_ANS_AUTO,
    QPF_ANS_MANUAL
} QPAutoNumberState;

typedef struct QPAutoNumber {
    QPAutoNumberState an_state;
    Py_ssize_t an_field_number;
} QPAutoNumber;

/* Arguments of str.format (positional + keywords) or str.format_map. */
typedef struct QPFormatArgs {
    JSValueConst *pos;          /* positional args (may be NULL when npos == 0) */
    Py_ssize_t npos;
    int has_pos;                /* 0: format_map (positional fields not allowed) */
    JSValueConst *kwvalues;     /* keyword values in kwnames order */
    JSValueConst kwnames;       /* tuple of str or QP_NULL */
    JSValueConst mapping;       /* format_map mapping, else QP_NULL */
} QPFormatArgs;

static void qpf_substr_init(QPSubString *s, JSValueConst str, const qp_strview *view,
                            Py_ssize_t start, Py_ssize_t end)
{
    s->str = str;
    s->view = view;
    s->start = start;
    s->end = end;
}

static inline Py_UCS4 qpf_sub_read(const QPSubString *s, Py_ssize_t i)
{
    return QP_SL_READ(s->view->kind, s->view->data, i);
}

/* SubString_new_object: None when str is NULL. */
static JSValue qpf_substr_new(JSContext *ctx, const QPSubString *s)
{
    if (QP_IS_NULL(s->str))
        return JS_UNDEFINED;
    return qp_str_substring(ctx, s->str, s->start, s->end);
}

/* SubString_new_object_or_empty */
static JSValue qpf_substr_new_or_empty(JSContext *ctx, const QPSubString *s)
{
    if (QP_IS_NULL(s->str))
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    return qpf_substr_new(ctx, s);
}

/* 1 (ValueError set) on an automatic/manual numbering conflict. */
static int qpf_autonumber_state_error(JSContext *ctx, QPAutoNumberState state,
                                      int field_name_is_empty)
{
    if (state == QPF_ANS_MANUAL) {
        if (field_name_is_empty) {
            qp_raise_value_error(ctx, "cannot switch from manual field specification "
                                 "to automatic field numbering");
            return 1;
        }
    } else {
        if (!field_name_is_empty) {
            qp_raise_value_error(ctx, "cannot switch from automatic field numbering "
                                 "to manual field specification");
            return 1;
        }
    }
    return 0;
}

/* get_integer(): decimal value of the substring, -1 if it is not a number
   (or empty); -2 with ValueError on overflow. */
static Py_ssize_t qpf_get_integer(JSContext *ctx, const QPSubString *str)
{
    Py_ssize_t accumulator = 0, digitval, i;

    /* empty string is an error */
    if (str->start >= str->end)
        return -1;
    for (i = str->start; i < str->end; i++) {
        digitval = _PyUnicode_ToDecimalDigit(qpf_sub_read(str, i));
        if (digitval < 0)
            return -1;
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            qp_raise_value_error(ctx, "Too many decimal digits in format string");
            return -2;
        }
        accumulator = accumulator * 10 + digitval;
    }
    return accumulator;
}

typedef struct QPFieldNameIterator {
    QPSubString str;            /* the whole field name */
    Py_ssize_t index;           /* current position inside it */
} QPFieldNameIterator;

static void qpf_fieldname_iter_init(QPFieldNameIterator *it, JSValueConst s,
                                    const qp_strview *view, Py_ssize_t start, Py_ssize_t end)
{
    qpf_substr_init(&it->str, s, view, start, end);
    it->index = start;
}

static void qpf_fieldname_attr(QPFieldNameIterator *it, QPSubString *name)
{
    Py_UCS4 c;

    name->str = it->str.str;
    name->view = it->str.view;
    name->start = it->index;
    /* return everything until '.' or '[' */
    while (it->index < it->str.end) {
        c = qpf_sub_read(&it->str, it->index++);
        if (c == '[' || c == '.') {
            /* backup so that this character will be seen next time */
            it->index--;
            break;
        }
    }
    /* end of string is okay */
    name->end = it->index;
}

static int qpf_fieldname_item(JSContext *ctx, QPFieldNameIterator *it, QPSubString *name)
{
    int bracket_seen = 0;

    name->str = it->str.str;
    name->view = it->str.view;
    name->start = it->index;
    /* return everything until ']' */
    while (it->index < it->str.end) {
        if (qpf_sub_read(&it->str, it->index++) == ']') {
            bracket_seen = 1;
            break;
        }
    }
    /* make sure we ended with a ']' */
    if (!bracket_seen) {
        qp_raise_value_error(ctx, "Missing ']' in format string");
        return 0;
    }
    /* don't include the ']' */
    name->end = it->index - 1;
    return 1;
}

/* FieldNameIterator_next: 0 error, 1 end, 2 value returned. */
static int qpf_fieldname_next(JSContext *ctx, QPFieldNameIterator *it, int *is_attribute,
                              Py_ssize_t *name_idx, QPSubString *name)
{
    /* check at end of input */
    if (it->index >= it->str.end)
        return 1;

    switch (qpf_sub_read(&it->str, it->index++)) {
    case '.':
        *is_attribute = 1;
        qpf_fieldname_attr(it, name);
        *name_idx = -1;
        break;
    case '[':
        *is_attribute = 0;
        if (!qpf_fieldname_item(ctx, it, name))
            return 0;
        *name_idx = qpf_get_integer(ctx, name);
        if (*name_idx == -2)
            return 0;
        break;
    default:
        /* Invalid character follows ']' */
        qp_raise_value_error(ctx, "Only '.' or '[' may follow ']' in format field specifier");
        return 0;
    }

    /* empty string is an error */
    if (name->start == name->end) {
        qp_raise_value_error(ctx, "Empty attribute in format string");
        return 0;
    }
    return 2;
}

/* field_name_split(): 'first' = part before the first '.' or '[',
   *first_idx its integer value or -1, 'rest' iterates over the remainder.
   auto_number NULL: no automatic numbering (string.Formatter).  0 on
   error. */
static int qpf_field_name_split(JSContext *ctx, JSValueConst str, const qp_strview *view,
                                Py_ssize_t start, Py_ssize_t end, QPSubString *first,
                                Py_ssize_t *first_idx, QPFieldNameIterator *rest,
                                QPAutoNumber *auto_number)
{
    Py_UCS4 c;
    Py_ssize_t i = start;
    int field_name_is_empty, using_numeric_index;

    /* find the part up until the first '.' or '[' */
    while (i < end) {
        c = QP_SL_READ(view->kind, view->data, i);
        i++;
        if (c == '[' || c == '.') {
            i--;
            break;
        }
    }

    qpf_substr_init(first, str, view, start, i);
    qpf_fieldname_iter_init(rest, str, view, i, end);

    /* see if "first" is an integer, in which case it's used as an index */
    *first_idx = qpf_get_integer(ctx, first);
    if (*first_idx == -2)
        return 0;

    field_name_is_empty = first->start >= first->end;

    /* If the field name is omitted or if we have a numeric index specified,
       then we're doing numeric indexing into args. */
    using_numeric_index = field_name_is_empty || *first_idx != -1;

    if (auto_number) {
        if (auto_number->an_state == QPF_ANS_INIT && using_numeric_index)
            auto_number->an_state = field_name_is_empty ? QPF_ANS_AUTO : QPF_ANS_MANUAL;
        if (using_numeric_index &&
            qpf_autonumber_state_error(ctx, auto_number->an_state, field_name_is_empty))
            return 0;
        /* Zero length field means we want to do auto-numbering of the
           fields. */
        if (field_name_is_empty)
            *first_idx = (auto_number->an_field_number)++;
    }
    return 1;
}

/* Keyword argument lookup for str.format (kwargs dict semantics). */
static JSValue qpf_kwargs_lookup(JSContext *ctx, const QPFormatArgs *fa, JSValueConst key)
{
    if (!QP_IS_NULL(fa->mapping))
        return qp_getitem(ctx, fa->mapping, key);
    if (!QP_IS_NULL(fa->kwnames)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(fa->kwnames);
        JSValue *names = QP_TUPLE_ITEMS(fa->kwnames);

        for (i = 0; i < n; i++) {
            int r = qp_str_eq(ctx, names[i], key);
            if (r < 0)
                return JS_EXCEPTION;
            if (r)
                return JS_DupValue(ctx, fa->kwvalues[i]);
        }
    }
    return qp_raise_key_error(ctx, key);
}

/* get_field_object(): the object named by the field (before the spec). */
static JSValue qpf_get_field_object(JSContext *ctx, QPSubString *input, const QPFormatArgs *fa,
                                    QPAutoNumber *auto_number)
{
    JSValue obj = JS_UNDEFINED, tmp, name_obj;
    int ok, is_attribute;
    QPSubString name, first;
    Py_ssize_t index;
    QPFieldNameIterator rest;

    if (!qpf_field_name_split(ctx, input->str, input->view, input->start, input->end,
                              &first, &index, &rest, auto_number))
        return JS_EXCEPTION;

    if (index == -1) {
        /* look up in kwargs */
        JSValue key = qpf_substr_new(ctx, &first);
        if (JS_IsException(key))
            return JS_EXCEPTION;
        obj = qpf_kwargs_lookup(ctx, fa, key);
        JS_FreeValue(ctx, key);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
    } else {
        /* format_map(): positional fields are not allowed */
        if (!fa->has_pos) {
            return qp_raise_value_error(ctx, "Format string contains positional fields");
        }
        /* look up in args */
        if (index >= fa->npos) {
            return qp_raise_index_error(ctx, "Replacement index %zd out of range for "
                                        "positional args tuple", index);
        }
        obj = JS_DupValue(ctx, fa->pos[index]);
    }

    /* iterate over the rest of the field_name */
    while ((ok = qpf_fieldname_next(ctx, &rest, &is_attribute, &index, &name)) == 2) {
        if (is_attribute) {
            /* getattr lookup "." */
            JSAtom atom;

            name_obj = qpf_substr_new(ctx, &name);
            if (JS_IsException(name_obj))
                goto error;
            atom = qp_str_to_atom(ctx, name_obj);
            JS_FreeValue(ctx, name_obj);
            if (atom == JS_ATOM_NULL)
                goto error;
            tmp = qp_getattr(ctx, obj, atom);
            JS_FreeAtom(ctx, atom);
        } else if (index == -1) {
            /* getitem lookup "[]" with a str key */
            name_obj = qpf_substr_new(ctx, &name);
            if (JS_IsException(name_obj))
                goto error;
            tmp = qp_getitem(ctx, obj, name_obj);
            JS_FreeValue(ctx, name_obj);
        } else {
            /* integer key: PySequence_GetItem for sequences and
               PyObject_GetItem otherwise, both obj[index] here */
            JSValue idx = qp_long_from_int64(ctx, index);
            if (JS_IsException(idx))
                goto error;
            tmp = qp_getitem(ctx, obj, idx);
            JS_FreeValue(ctx, idx);
        }
        if (JS_IsException(tmp))
            goto error;
        JS_FreeValue(ctx, obj);
        obj = tmp;
    }
    /* end of iterator, this is the non-error case */
    if (ok == 1)
        return obj;
error:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* parse_field(): split "{field_name!conversion:format_spec}" (str starts
   after the '{').  1 ok, 0 error. */
static int qpf_parse_field(JSContext *ctx, QPSubString *str, QPSubString *field_name,
                           QPSubString *format_spec, int *format_spec_needs_expanding,
                           Py_UCS4 *conversion)
{
    Py_UCS4 c = 0;

    *conversion = '\0';
    qpf_substr_init(format_spec, QP_NULL, str->view, 0, 0);

    /* Search for the field name.  it's terminated by the end of the
       string, or a ':' or '!' */
    field_name->str = str->str;
    field_name->view = str->view;
    field_name->start = str->start;
    while (str->start < str->end) {
        switch ((c = qpf_sub_read(str, str->start++))) {
        case '{':
            qp_raise_value_error(ctx, "unexpected '{' in field name");
            return 0;
        case '[':
            for (; str->start < str->end; str->start++) {
                if (qpf_sub_read(str, str->start) == ']')
                    break;
            }
            continue;
        case '}':
        case ':':
        case '!':
            break;
        default:
            continue;
        }
        break;
    }

    field_name->end = str->start - 1;
    if (c == '!' || c == ':') {
        Py_ssize_t count;

        /* see if there's a conversion specifier */
        if (c == '!') {
            /* there must be another character present */
            if (str->start >= str->end) {
                qp_raise_value_error(ctx, "end of string while looking for conversion "
                                     "specifier");
                return 0;
            }
            *conversion = qpf_sub_read(str, str->start++);

            if (str->start < str->end) {
                c = qpf_sub_read(str, str->start++);
                if (c == '}')
                    return 1;
                if (c != ':') {
                    qp_raise_value_error(ctx, "expected ':' after conversion specifier");
                    return 0;
                }
            }
        }
        format_spec->str = str->str;
        format_spec->view = str->view;
        format_spec->start = str->start;
        count = 1;
        while (str->start < str->end) {
            switch ((c = qpf_sub_read(str, str->start++))) {
            case '{':
                *format_spec_needs_expanding = 1;
                count++;
                break;
            case '}':
                count--;
                if (count == 0) {
                    format_spec->end = str->start - 1;
                    return 1;
                }
                break;
            default:
                break;
            }
        }
        qp_raise_value_error(ctx, "unmatched '{' in format spec");
        return 0;
    } else if (c != '}') {
        qp_raise_value_error(ctx, "expected '}' before end of string");
        return 0;
    }
    return 1;
}

typedef struct QPMarkupIterator {
    QPSubString str;
} QPMarkupIterator;

/* MarkupIterator_next(): 0 error, 1 end, 2 literal and/or field. */
static int qpf_markup_next(JSContext *ctx, QPMarkupIterator *it, QPSubString *literal,
                           int *field_present, QPSubString *field_name,
                           QPSubString *format_spec, Py_UCS4 *conversion,
                           int *format_spec_needs_expanding)
{
    int at_end, markup_follows = 0;
    Py_UCS4 c = 0;
    Py_ssize_t start, len;

    qpf_substr_init(literal, QP_NULL, it->str.view, 0, 0);
    qpf_substr_init(field_name, QP_NULL, it->str.view, 0, 0);
    qpf_substr_init(format_spec, QP_NULL, it->str.view, 0, 0);
    *conversion = '\0';
    *format_spec_needs_expanding = 0;
    *field_present = 0;

    /* No more input, end of iterator. */
    if (it->str.start >= it->str.end)
        return 1;

    start = it->str.start;

    /* Read literal text until the end, an escaped '{' or '}', or an
       unescaped '{'. */
    while (it->str.start < it->str.end) {
        c = qpf_sub_read(&it->str, it->str.start++);
        if (c == '{' || c == '}') {
            markup_follows = 1;
            break;
        }
    }

    at_end = it->str.start >= it->str.end;
    len = it->str.start - start;

    if (c == '}' && (at_end || c != qpf_sub_read(&it->str, it->str.start))) {
        qp_raise_value_error(ctx, "Single '}' encountered in format string");
        return 0;
    }
    if (at_end && c == '{') {
        qp_raise_value_error(ctx, "Single '{' encountered in format string");
        return 0;
    }
    if (!at_end) {
        if (c == qpf_sub_read(&it->str, it->str.start)) {
            /* escaped } or {, skip it in the input */
            it->str.start++;
            markup_follows = 0;
        } else {
            len--;
        }
    }

    /* record the literal text */
    literal->str = it->str.str;
    literal->start = start;
    literal->end = start + len;

    if (!markup_follows)
        return 2;

    /* this is markup; parse the field */
    *field_present = 1;
    if (!qpf_parse_field(ctx, &it->str, field_name, format_spec,
                         format_spec_needs_expanding, conversion))
        return 0;
    return 2;
}

/* do_conversion(): !r, !s, !a */
static JSValue qpf_do_conversion(JSContext *ctx, JSValueConst obj, Py_UCS4 conversion)
{
    switch (conversion) {
    case 'r':
        return qp_repr(ctx, obj);
    case 's':
        return qp_str(ctx, obj);
    case 'a':
        return qp_ascii(ctx, obj);
    default:
        if (conversion > 32 && conversion < 127)
            return qp_raise_value_error(ctx, "Unknown conversion specifier %c", (int)conversion);
        return qp_raise_value_error(ctx, "Unknown conversion specifier \\x%x",
                                    (unsigned int)conversion);
    }
}

static JSValue qpf_build_string(JSContext *ctx, QPSubString *input, const QPFormatArgs *fa,
                                int recursion_depth, QPAutoNumber *auto_number);

/* output_markup(): render one replacement field.  1 ok, 0 error. */
static int qpf_output_markup(JSContext *ctx, QPSubString *field_name, QPSubString *format_spec,
                             int format_spec_needs_expanding, Py_UCS4 conversion,
                             StringBuffer *out, const QPFormatArgs *fa,
                             int recursion_depth, QPAutoNumber *auto_number)
{
    JSValue fieldobj, tmp, spec, result;
    int ok;

    fieldobj = qpf_get_field_object(ctx, field_name, fa, auto_number);
    if (JS_IsException(fieldobj))
        return 0;

    if (conversion != '\0') {
        tmp = qpf_do_conversion(ctx, fieldobj, conversion);
        JS_FreeValue(ctx, fieldobj);
        if (JS_IsException(tmp))
            return 0;
        fieldobj = tmp;
    }

    /* if needed, recursively compute the format_spec */
    if (format_spec_needs_expanding)
        spec = qpf_build_string(ctx, format_spec, fa, recursion_depth - 1, auto_number);
    else
        spec = qpf_substr_new_or_empty(ctx, format_spec);
    if (JS_IsException(spec)) {
        JS_FreeValue(ctx, fieldobj);
        return 0;
    }

    /* render_field(): fieldobj.__format__(spec) */
    result = qp_format(ctx, fieldobj, spec);
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, fieldobj);
    if (JS_IsException(result))
        return 0;
    ok = qpf_put_str(ctx, out, result) == 0;
    JS_FreeValue(ctx, result);
    return ok;
}

/* do_markup(): top-level loop of str.format. 1 ok, 0 error. */
static int qpf_do_markup(JSContext *ctx, QPSubString *input, const QPFormatArgs *fa,
                         StringBuffer *out, int recursion_depth, QPAutoNumber *auto_number)
{
    QPMarkupIterator iter;
    int format_spec_needs_expanding, result, field_present;
    QPSubString literal, field_name, format_spec;
    Py_UCS4 conversion;

    iter.str = *input;
    while ((result = qpf_markup_next(ctx, &iter, &literal, &field_present, &field_name,
                                     &format_spec, &conversion,
                                     &format_spec_needs_expanding)) == 2) {
        if (literal.end != literal.start) {
            if (qpf_put_view(out, literal.view, literal.start, literal.end))
                return 0;
        }
        if (field_present) {
            if (!qpf_output_markup(ctx, &field_name, &format_spec,
                                   format_spec_needs_expanding, conversion, out, fa,
                                   recursion_depth, auto_number))
                return 0;
        }
    }
    return result;
}

/* build_string(): format input with the arguments; new str. */
static JSValue qpf_build_string(JSContext *ctx, QPSubString *input, const QPFormatArgs *fa,
                                int recursion_depth, QPAutoNumber *auto_number)
{
    StringBuffer out;

    /* check the recursion level */
    if (recursion_depth <= 0)
        return qp_raise_value_error(ctx, "Max string recursion exceeded");

    string_buffer_init(ctx, &out, (int)min_int64(input->view->len + 100, JS_STRING_LEN_MAX));
    if (!qpf_do_markup(ctx, input, fa, &out, recursion_depth, auto_number)) {
        string_buffer_free(&out);
        return JS_EXCEPTION;
    }
    return string_buffer_end(&out);
}

/* do_string_format() */
static JSValue qpf_do_string_format(JSContext *ctx, JSValueConst self, const QPFormatArgs *fa)
{
    JSValue str, res;
    qp_strview view;
    QPSubString input;
    QPAutoNumber auto_number;

    if (qpf_view_begin(ctx, self, &str, &view))
        return JS_EXCEPTION;
    auto_number.an_state = QPF_ANS_INIT;
    auto_number.an_field_number = 0;
    qpf_substr_init(&input, str, &view, 0, view.len);
    /* PEP 3101 says only 2 levels */
    res = qpf_build_string(ctx, &input, fa, 2, &auto_number);
    qpf_view_end(ctx, str, &view);
    return res;
}

/* str.format(*args, **kwargs) */
static JSValue qp_str_format_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPFormatArgs fa;

    fa.pos = args;
    fa.npos = nargs;
    fa.has_pos = 1;
    fa.kwvalues = args + nargs;
    fa.kwnames = (!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) > 0) ? kwnames : QP_NULL;
    fa.mapping = QP_NULL;
    return qpf_do_string_format(ctx, self, &fa);
}

/* str.format_map(mapping, /) */
static JSValue qp_str_format_map_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPFormatArgs fa;

    fa.pos = NULL;
    fa.npos = 0;
    fa.has_pos = 0;
    fa.kwvalues = NULL;
    fa.kwnames = QP_NULL;
    fa.mapping = args[0];
    return qpf_do_string_format(ctx, self, &fa);
}

/* ======================================================================= */
/* _string.formatter_parser / _string.formatter_field_name_split           */
/* ======================================================================= */

/* Instance slots of formatteriterator: the str and the position. */
#define QPF_FMTITER_STR   0
#define QPF_FMTITER_POS   1
#define QPF_FMTITER_NSLOTS 2
/* Instance slots of fieldnameiterator: the str and the position. */
#define QPF_FNITER_STR    0
#define QPF_FNITER_POS    1
#define QPF_FNITER_NSLOTS 2

static JSValue qpf_iter_self(JSContext *ctx, JSValueConst it)
{
    return JS_DupValue(ctx, it);
}

/* formatteriter_next(): (literal, field_name, format_spec, conversion). */
static JSValue qpf_formatteriter_next(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSValueConst str = slots[QPF_FMTITER_STR];
    qp_strview view;
    QPMarkupIterator it;
    QPSubString literal, field_name, format_spec;
    Py_UCS4 conversion;
    int field_present, format_spec_needs_expanding, result;
    JSValue items[4];
    int i;

    if (qp_str_view_acquire(ctx, str, &view) < 0)
        return JS_EXCEPTION;
    qpf_substr_init(&it.str, str, &view, JS_VALUE_GET_INT(slots[QPF_FMTITER_POS]), view.len);
    result = qpf_markup_next(ctx, &it, &literal, &field_present, &field_name, &format_spec,
                             &conversion, &format_spec_needs_expanding);
    slots[QPF_FMTITER_POS] = JS_NewInt32(ctx, (int32_t)it.str.start);
    if (result == 0 || result == 1) {
        qp_str_view_release(ctx, &view);
        return result == 0 ? JS_EXCEPTION : QP_NULL;
    }
    items[0] = qpf_substr_new(ctx, &literal);
    items[1] = qpf_substr_new(ctx, &field_name);
    /* format_spec: a str (possibly empty) when a field is present, else
       None */
    items[2] = field_present ? qpf_substr_new_or_empty(ctx, &format_spec)
                             : qpf_substr_new(ctx, &format_spec);
    items[3] = conversion == '\0' ? JS_UNDEFINED : qp_str_from_char(ctx, conversion);
    qp_str_view_release(ctx, &view);
    for (i = 0; i < 4; i++) {
        if (JS_IsException(items[i])) {
            int j;
            for (j = 0; j < 4; j++) {
                if (!JS_IsException(items[j]))
                    JS_FreeValue(ctx, items[j]);
            }
            return JS_EXCEPTION;
        }
    }
    return qp_tuple_new_steal(ctx, 4, items);
}

/* fieldnameiter_next(): (is_attr, int or str). */
static JSValue qpf_fieldnameiter_next(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSValueConst str = slots[QPF_FNITER_STR];
    qp_strview view;
    QPFieldNameIterator it;
    QPSubString name;
    int is_attr = 0, result;
    Py_ssize_t idx = -1;
    JSValue items[2];

    if (qp_str_view_acquire(ctx, str, &view) < 0)
        return JS_EXCEPTION;
    qpf_fieldname_iter_init(&it, str, &view, 0, view.len);
    it.index = JS_VALUE_GET_INT(slots[QPF_FNITER_POS]);
    result = qpf_fieldname_next(ctx, &it, &is_attr, &idx, &name);
    slots[QPF_FNITER_POS] = JS_NewInt32(ctx, (int32_t)it.index);
    if (result == 0 || result == 1) {
        qp_str_view_release(ctx, &view);
        return result == 0 ? JS_EXCEPTION : QP_NULL;
    }
    items[0] = qp_bool_new(is_attr);
    /* either an integer or a string */
    if (idx != -1)
        items[1] = qp_long_from_int64(ctx, idx);
    else
        items[1] = qpf_substr_new(ctx, &name);
    qp_str_view_release(ctx, &view);
    if (JS_IsException(items[1]))
        return JS_EXCEPTION;
    return qp_tuple_new_steal(ctx, 2, items);
}

/* _string.formatter_parser(str) */
static JSValue qpf_formatter_parser(JSContext *ctx, JSValueConst module, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst self = args[0];
    JSValue it, *slots;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (!qp_str_check(ctx, self))
        return qp_raise_type_error(ctx, "expected str, got %s", qpf_tp_name(ctx, self, buf, sizeof(buf)));
    it = qp_object_new_instance(ctx, QP_TYPE(ctx, formatteriterator), JS_CLASS_PY_OBJECT);
    if (JS_IsException(it))
        return JS_EXCEPTION;
    slots = qp_instance_slots(it);
    slots[QPF_FMTITER_STR] = qp_str_value(ctx, self);
    if (JS_IsException(slots[QPF_FMTITER_STR])) {
        slots[QPF_FMTITER_STR] = QP_NULL;
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    slots[QPF_FMTITER_POS] = JS_NewInt32(ctx, 0);
    return it;
}

/* _string.formatter_field_name_split(str) -> (first, rest iterator) */
static JSValue qpf_formatter_field_name_split(JSContext *ctx, JSValueConst module,
                                              JSValueConst *args, int nargs,
                                              JSValueConst kwnames, int magic)
{
    JSValueConst self = args[0];
    JSValue str, it, *slots, items[2];
    qp_strview view;
    QPSubString first;
    Py_ssize_t first_idx;
    QPFieldNameIterator rest;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (!qp_str_check(ctx, self))
        return qp_raise_type_error(ctx, "expected str, got %s", qpf_tp_name(ctx, self, buf, sizeof(buf)));
    if (qpf_view_begin(ctx, self, &str, &view))
        return JS_EXCEPTION;
    if (!qpf_field_name_split(ctx, str, &view, 0, view.len, &first, &first_idx, &rest, NULL)) {
        qpf_view_end(ctx, str, &view);
        return JS_EXCEPTION;
    }
    /* first becomes an integer, if possible; else a string */
    if (first_idx != -1)
        items[0] = qp_long_from_int64(ctx, first_idx);
    else
        items[0] = qpf_substr_new(ctx, &first);
    qp_str_view_release(ctx, &view);
    if (JS_IsException(items[0])) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    it = qp_object_new_instance(ctx, QP_TYPE(ctx, fieldnameiterator), JS_CLASS_PY_OBJECT);
    if (JS_IsException(it)) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    slots = qp_instance_slots(it);
    slots[QPF_FNITER_STR] = str;   /* ownership moves to the iterator */
    slots[QPF_FNITER_POS] = JS_NewInt32(ctx, (int32_t)rest.index);
    items[1] = it;
    return qp_tuple_new_steal(ctx, 2, items);
}

/* Functions of the builtin _string module (registered by the module
   table of unit l; string.Formatter imports them). */
static const QPMethodDef qp_string_module_methods[] __maybe_unused = {
    { "formatter_field_name_split", qpf_formatter_field_name_split, QP_METH_O, 0,
      "split the argument as a field name" },
    { "formatter_parser", qpf_formatter_parser, QP_METH_O, 0,
      "parse the argument as a format string" },
    { NULL, NULL, 0, 0, NULL },
};

static const QPTypeSpec qpf_formatteriter_spec = {
    .name = "formatteriterator",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QPF_FMTITER_NSLOTS,
    .slots = {
        .tp_iter = qpf_iter_self,
        .tp_iternext = qpf_formatteriter_next,
    },
};

static const QPTypeSpec qpf_fieldnameiter_spec = {
    .name = "fieldnameiterator",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QPF_FNITER_NSLOTS,
    .slots = {
        .tp_iter = qpf_iter_self,
        .tp_iternext = qpf_fieldnameiter_next,
    },
};

/* Ready formatteriterator and fieldnameiterator (called by
   qp_init_templatelib). */
static int qp_init_str_formatter(JSContext *ctx)
{
    if (qp_type_ready(ctx, QP_TYPE(ctx, formatteriterator), &qpf_formatteriter_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, fieldnameiterator), &qpf_fieldnameiter_spec) < 0)
        return -1;
    return 0;
}

/* ======================================================================= */
/* PyUnicode_FromFormat (qp_str_from_format)                               */
/* Derived from CPython 3.15.2 Objects/unicodeobject.c                     */
/* unicode_fromformat_arg() and helpers (PSF-2.0).                         */
/* ======================================================================= */

#define QPFF_LJUST 0x01
#define QPFF_ZERO  0x02
#define QPFF_ALT   0x04

#define QPFF_LONG     1
#define QPFF_LONGLONG 2
#define QPFF_SIZE     3
#define QPFF_PTRDIFF  4
#define QPFF_INTMAX   5

/* maximum number of characters required for output of %jo or %jd or %p */
#define QPFF_MAX_INTMAX_CHARS (5 + (sizeof(intmax_t) * 8 - 1) / 3)

/* unicode_fromformat_write_str() over a code-point view. */
static int qpff_write_view(StringBuffer *b, const qp_strview *v, Py_ssize_t width,
                           Py_ssize_t precision, int flags)
{
    Py_ssize_t length = v->len, fill;

    if ((precision == -1 || precision >= length) && width <= length)
        return qpf_put_view(b, v, 0, length);
    if (precision != -1)
        length = precision < length ? precision : length;
    fill = width - length > 0 ? width - length : 0;
    if (fill && !(flags & QPFF_LJUST)) {
        if (qpf_fill(b, ' ', fill))
            return -1;
    }
    if (qpf_put_view(b, v, 0, length))
        return -1;
    if (fill && (flags & QPFF_LJUST)) {
        if (qpf_fill(b, ' ', fill))
            return -1;
    }
    return 0;
}

/* unicode_fromformat_write_str() for a str (or str subclass instance). */
static int qpff_write_str(JSContext *ctx, StringBuffer *b, JSValueConst s, Py_ssize_t width,
                          Py_ssize_t precision, int flags)
{
    JSValue plain;
    qp_strview view;
    int r;

    if (precision == -1 && width <= 0 && qp_str_check_exact(s))
        return string_buffer_concat_value(b, s);
    if (qpf_view_begin(ctx, s, &plain, &view))
        return -1;
    r = qpff_write_view(b, &view, width, precision, flags);
    qpf_view_end(ctx, plain, &view);
    return r;
}

/* Same as qpff_write_str, consuming s. */
static int qpff_write_str_free(JSContext *ctx, StringBuffer *b, JSValue s, Py_ssize_t width,
                               Py_ssize_t precision, int flags)
{
    int r;

    if (JS_IsException(s))
        return -1;
    r = qpff_write_str(ctx, b, s, width, precision, flags);
    JS_FreeValue(ctx, s);
    return r;
}

/* unicode_fromformat_write_utf8(): decode with the "replace" error handler;
   with a precision, at most `precision` bytes are read and an incomplete
   trailing sequence is dropped (stateful decoding). */
static int qpff_write_utf8(JSContext *ctx, StringBuffer *b, const char *str, Py_ssize_t width,
                           Py_ssize_t precision, int flags)
{
    qp_ucsbuilder ub;
    qp_codec_error cerr;
    qp_strview view;
    Py_ssize_t length, pos = 0, consumed;
    int stateful = 0, r;

    if (precision == -1) {
        length = strlen(str);
    } else {
        length = 0;
        while (length < precision && str[length])
            length++;
        if (length == precision)
            stateful = 1;
    }

    qp_ucsbuilder_init(&ub, ctx->rt);
    while (pos < length) {
        consumed = 0;
        r = qp_sl_utf8_decode(str + pos, length - pos, stateful ? &consumed : NULL, &ub, &cerr);
        if (r == 0)
            break;
        if (r != QP_SL_CODEC_ERROR)
            goto oom;
        if (qp_ucsbuilder_putc(&ub, 0xFFFD))
            goto oom;
        pos += cerr.end > cerr.start ? cerr.end : cerr.start + 1;
    }
    view = qp_ucsbuilder_view(&ub);
    r = qpff_write_view(b, &view, width, -1, flags);
    qp_ucsbuilder_free(&ub);
    return r;

oom:
    qp_ucsbuilder_free(&ub);
    JS_ThrowOutOfMemory(ctx);
    return -1;
}

/* unicode_fromformat_write_wcstr() (wchar_t is UTF-32 on our hosts; lone
   surrogate values are kept). */
static int qpff_write_wcstr(JSContext *ctx, StringBuffer *b, const wchar_t *str,
                            Py_ssize_t width, Py_ssize_t precision, int flags)
{
    qp_ucsbuilder ub;
    qp_strview view;
    Py_ssize_t length, i;
    int r;

    if (precision == -1) {
        length = 0;
        while (str[length])
            length++;
    } else {
        length = 0;
        while (length < precision && str[length])
            length++;
    }
    qp_ucsbuilder_init(&ub, ctx->rt);
    for (i = 0; i < length; i++) {
        uint32_t c = (uint32_t)str[i];
        if (c > 0x10FFFF) {
            qp_ucsbuilder_free(&ub);
            qp_raise_value_error(ctx, "character U+%x is not in range [U+0000; U+10ffff]",
                                 (unsigned)c);
            return -1;
        }
        if (qp_ucsbuilder_putc(&ub, c)) {
            qp_ucsbuilder_free(&ub);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    view = qp_ucsbuilder_view(&ub);
    r = qpff_write_view(b, &view, width, -1, flags);
    qp_ucsbuilder_free(&ub);
    return r;
}

/* _PyType_GetFullyQualifiedName(type, sep): tp_name for static types,
   "module<sep>qualname" for heap types (just qualname when the module is
   builtins or __main__, or not a str). */
static JSValue qpff_type_fullname(JSContext *ctx, QPType *t, char sep)
{
    JSValue qualname, module, res;
    int r;

    if (!(t->flags & QP_TPFLAGS_HEAPTYPE)) {
        if (t->tp_name)
            return qp_str_from_cstr(ctx, t->tp_name);
        return qp_str_from_atom(ctx, t->name);
    }
    if (JS_IsString(t->qualname) || JS_VALUE_GET_TAG(t->qualname) == JS_TAG_STRING_ROPE)
        qualname = JS_DupValue(ctx, t->qualname);
    else
        qualname = qp_str_from_atom(ctx, t->name);
    if (JS_IsException(qualname))
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), &module);
    if (r <= 0) {
        JS_FreeValue(ctx, qualname);
        if (r == 0)
            qp_raise(ctx, QP_EXC(ctx, AttributeError), "__module__");
        return JS_EXCEPTION;
    }
    res = qualname;
    if (qp_str_check(ctx, module)) {
        BOOL use_module;
        JSValue plain = qp_str_value(ctx, module);
        const char *m;

        if (JS_IsException(plain)) {
            JS_FreeValue(ctx, module);
            JS_FreeValue(ctx, qualname);
            return JS_EXCEPTION;
        }
        m = JS_ToCString(ctx, plain);
        if (!m) {
            JS_FreeValue(ctx, plain);
            JS_FreeValue(ctx, module);
            JS_FreeValue(ctx, qualname);
            return JS_EXCEPTION;
        }
        use_module = strcmp(m, "builtins") != 0 && strcmp(m, "__main__") != 0;
        JS_FreeCString(ctx, m);
        if (use_module) {
            res = qp_str_from_format(ctx, "%U%c%U", plain, (int)sep, qualname);
            JS_FreeValue(ctx, qualname);
        }
        JS_FreeValue(ctx, plain);
    }
    JS_FreeValue(ctx, module);
    return res;
}

/* unicode_fromformat_arg(): one conversion starting at f ('%'); returns
   the position after it, or NULL with an exception. */
static const char *qpff_arg(JSContext *ctx, StringBuffer *b, const char *f, va_list *vargs)
{
    const char *p = f;
    int flags = 0, sizemod = 0, len;
    Py_ssize_t width, precision;

    f++;
    if (*f == '%') {
        if (string_buffer_putc8(b, '%'))
            return NULL;
        return f + 1;
    }

    /* Parse flags. */
    for (;;) {
        switch (*f++) {
        case '-': flags |= QPFF_LJUST; continue;
        case '0': flags |= QPFF_ZERO; continue;
        case '#': flags |= QPFF_ALT; continue;
        }
        f--;
        break;
    }

    /* parse the width.precision part */
    width = -1;
    if (*f == '*') {
        width = va_arg(*vargs, int);
        if (width < 0) {
            flags |= QPFF_LJUST;
            width = -width;
        }
        f++;
    } else if (*f >= '0' && *f <= '9') {
        width = *f - '0';
        f++;
        while (*f >= '0' && *f <= '9') {
            if (width > (PY_SSIZE_T_MAX - ((int)*f - '0')) / 10) {
                qp_raise_value_error(ctx, "width too big");
                return NULL;
            }
            width = width * 10 + (*f - '0');
            f++;
        }
    }
    precision = -1;
    if (*f == '.') {
        f++;
        if (*f == '*') {
            precision = va_arg(*vargs, int);
            if (precision < 0)
                precision = -2;
            f++;
        } else if (*f >= '0' && *f <= '9') {
            precision = *f - '0';
            f++;
            while (*f >= '0' && *f <= '9') {
                if (precision > (PY_SSIZE_T_MAX - ((int)*f - '0')) / 10) {
                    qp_raise_value_error(ctx, "precision too big");
                    return NULL;
                }
                precision = precision * 10 + (*f - '0');
                f++;
            }
        }
    }

    if (*f == 'l') {
        if (f[1] == 'l') {
            sizemod = QPFF_LONGLONG;
            f += 2;
        } else {
            sizemod = QPFF_LONG;
            ++f;
        }
    } else if (*f == 'z') {
        sizemod = QPFF_SIZE;
        ++f;
    } else if (*f == 't') {
        sizemod = QPFF_PTRDIFF;
        ++f;
    } else if (*f == 'j') {
        sizemod = QPFF_INTMAX;
        ++f;
    }

    switch (*f) {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
        break;
    case 'c': case 'p':
        if (sizemod || width >= 0 || precision >= 0)
            goto invalid_format;
        break;
    case 's':
    case 'V':
        if (sizemod && sizemod != QPFF_LONG)
            goto invalid_format;
        break;
    default:
        if (sizemod)
            goto invalid_format;
        break;
    }

    switch (*f) {
    case 'c': {
        int ordinal = va_arg(*vargs, int);
        if (ordinal < 0 || ordinal > 0x10FFFF) {
            qp_raise_overflow_error(ctx, "character argument not in range(0x110000)");
            return NULL;
        }
        if (string_buffer_putc(b, (uint32_t)ordinal))
            return NULL;
        break;
    }

    case 'd': case 'i':
    case 'o': case 'u': case 'x': case 'X': {
        char buffer[QPFF_MAX_INTMAX_CHARS];
        int sign;
        Py_ssize_t spacepad, zeropad, ilen;

#define QPFF_SPRINT(SIZE_SPEC, FMT_CHAR, TYPE) \
        snprintf(buffer, sizeof(buffer), "%" SIZE_SPEC FMT_CHAR, va_arg(*vargs, TYPE))
#define QPFF_DO_SPRINTS(SIZE_SPEC, SIGNED_TYPE, UNSIGNED_TYPE)               \
        switch (*f) {                                                        \
        case 'o': len = QPFF_SPRINT(SIZE_SPEC, "o", UNSIGNED_TYPE); break;   \
        case 'u': len = QPFF_SPRINT(SIZE_SPEC, "u", UNSIGNED_TYPE); break;   \
        case 'x': len = QPFF_SPRINT(SIZE_SPEC, "x", UNSIGNED_TYPE); break;   \
        case 'X': len = QPFF_SPRINT(SIZE_SPEC, "X", UNSIGNED_TYPE); break;   \
        default:  len = QPFF_SPRINT(SIZE_SPEC, "d", SIGNED_TYPE); break;     \
        }

        switch (sizemod) {
        case QPFF_LONG:     QPFF_DO_SPRINTS("l", long, unsigned long); break;
        case QPFF_LONGLONG: QPFF_DO_SPRINTS("ll", long long, unsigned long long); break;
        case QPFF_SIZE:     QPFF_DO_SPRINTS("z", ssize_t, size_t); break;
        case QPFF_PTRDIFF:  QPFF_DO_SPRINTS("t", ptrdiff_t, ptrdiff_t); break;
        case QPFF_INTMAX:   QPFF_DO_SPRINTS("j", intmax_t, uintmax_t); break;
        default:            QPFF_DO_SPRINTS("", int, unsigned int); break;
        }
#undef QPFF_SPRINT
#undef QPFF_DO_SPRINTS

        sign = buffer[0] == '-';
        ilen = len - sign;
        precision = precision > ilen ? precision : ilen;
        width = width > precision + sign ? width : precision + sign;
        if ((flags & QPFF_ZERO) && !(flags & QPFF_LJUST))
            precision = width - sign;
        spacepad = width - precision - sign > 0 ? width - precision - sign : 0;
        zeropad = precision - ilen > 0 ? precision - ilen : 0;

        if (spacepad && !(flags & QPFF_LJUST)) {
            if (qpf_fill(b, ' ', spacepad))
                return NULL;
        }
        if (sign) {
            if (string_buffer_putc8(b, '-'))
                return NULL;
        }
        if (zeropad) {
            if (qpf_fill(b, '0', zeropad))
                return NULL;
        }
        if (string_buffer_write8(b, (const uint8_t *)buffer + sign, (int)ilen))
            return NULL;
        if (spacepad && (flags & QPFF_LJUST)) {
            if (qpf_fill(b, ' ', spacepad))
                return NULL;
        }
        break;
    }

    case 'p': {
        char number[QPFF_MAX_INTMAX_CHARS + 2];

        len = snprintf(number, sizeof(number), "%p", va_arg(*vargs, void *));
        /* %p is ill-defined: ensure leading 0x. */
        if (number[1] == 'X') {
            number[1] = 'x';
        } else if (number[1] != 'x') {
            memmove(number + 2, number, strlen(number) + 1);
            number[0] = '0';
            number[1] = 'x';
            len += 2;
        }
        if (string_buffer_write8(b, (const uint8_t *)number, len))
            return NULL;
        break;
    }

    case 's':
        if (sizemod) {
            const wchar_t *s = va_arg(*vargs, const wchar_t *);
            if (qpff_write_wcstr(ctx, b, s, width, precision, flags) < 0)
                return NULL;
        } else {
            const char *s = va_arg(*vargs, const char *);
            if (qpff_write_utf8(ctx, b, s, width, precision, flags) < 0)
                return NULL;
        }
        break;

    case 'U': {
        JSValue obj = va_arg(*vargs, JSValue);
        if (qpff_write_str(ctx, b, obj, width, precision, flags) < 0)
            return NULL;
        break;
    }

    case 'V': {
        /* str object, or the C string when the object is QP_NULL */
        JSValue obj = va_arg(*vargs, JSValue);
        const char *str = NULL;
        const wchar_t *wstr = NULL;

        if (sizemod)
            wstr = va_arg(*vargs, const wchar_t *);
        else
            str = va_arg(*vargs, const char *);
        if (!QP_IS_NULL(obj) && JS_VALUE_GET_TAG(obj) != JS_TAG_NULL) {
            if (qpff_write_str(ctx, b, obj, width, precision, flags) < 0)
                return NULL;
        } else if (sizemod) {
            if (qpff_write_wcstr(ctx, b, wstr, width, precision, flags) < 0)
                return NULL;
        } else {
            if (qpff_write_utf8(ctx, b, str, width, precision, flags) < 0)
                return NULL;
        }
        break;
    }

    case 'S': {
        JSValue obj = va_arg(*vargs, JSValue);
        if (qpff_write_str_free(ctx, b, qp_str(ctx, obj), width, precision, flags) < 0)
            return NULL;
        break;
    }

    case 'R': {
        JSValue obj = va_arg(*vargs, JSValue);
        if (qpff_write_str_free(ctx, b, qp_repr(ctx, obj), width, precision, flags) < 0)
            return NULL;
        break;
    }

    case 'A': {
        JSValue obj = va_arg(*vargs, JSValue);
        if (qpff_write_str_free(ctx, b, qp_ascii(ctx, obj), width, precision, flags) < 0)
            return NULL;
        break;
    }

    case 'T': {
        JSValue obj = va_arg(*vargs, JSValue);
        QPType *type = qp_type_of(ctx, obj);
        if (qpff_write_str_free(ctx, b, qpff_type_fullname(ctx, type, (flags & QPFF_ALT) ? ':' : '.'),
                                width, precision, flags) < 0)
            return NULL;
        break;
    }

    case 'N': {
        /* object.h contract: the argument is a QPType * */
        QPType *type = va_arg(*vargs, QPType *);
        if (!type) {
            qp_raise_type_error(ctx, "%%N argument must be a type");
            return NULL;
        }
        if (qpff_write_str_free(ctx, b, qpff_type_fullname(ctx, type, (flags & QPFF_ALT) ? ':' : '.'),
                                width, precision, flags) < 0)
            return NULL;
        break;
    }

    default:
    invalid_format:
        qp_raise_system_error(ctx, "invalid format string: %s", p);
        return NULL;
    }
    return f + 1;
}

/* PyUnicode_FromFormatV */
static JSValue qp_str_from_format_v(JSContext *ctx, const char *format, va_list vargs)
{
    StringBuffer b;
    va_list vargs2;
    size_t len = strlen(format), i;
    const char *f;

    for (i = 0; i < len; i++) {
        if ((unsigned char)format[i] > 127) {
            return qp_raise_value_error(ctx, "PyUnicode_FromFormatV() expects an ASCII-encoded "
                                        "format string, got a non-ASCII byte: 0x%02x",
                                        (unsigned)(unsigned char)format[i]);
        }
    }
    string_buffer_init(ctx, &b, (int)(len + 100));
    va_copy(vargs2, vargs);
    for (f = format; *f; ) {
        if (*f == '%') {
            f = qpff_arg(ctx, &b, f, &vargs2);
            if (!f)
                goto fail;
        } else {
            const char *q = strchr(f, '%');
            size_t n = q ? (size_t)(q - f) : strlen(f);

            if (string_buffer_write8(&b, (const uint8_t *)f, (int)n))
                goto fail;
            f += n;
        }
    }
    va_end(vargs2);
    return string_buffer_end(&b);

fail:
    va_end(vargs2);
    string_buffer_free(&b);
    return JS_EXCEPTION;
}

/* PyUnicode_FromFormat */
static JSValue qp_str_from_format(JSContext *ctx, const char *format, ...)
{
    JSValue res;
    va_list ap;

    va_start(ap, format);
    res = qp_str_from_format_v(ctx, format, ap);
    va_end(ap);
    return res;
}
