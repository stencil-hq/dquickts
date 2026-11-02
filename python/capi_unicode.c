/*
 * dquickts Python C API (plan D5, Tier S), unit D part 1: str.
 *
 * Contract: python/capi.h, design local://capi-design.md (sections 1.2
 * "str mirrors", 6 unit D, 10 "Owner D").  Ports of CPython 3.15
 * Objects/unicodeobject.c, Objects/unicode_writer.c, Python/codecs.c
 * (PSF-2.0) where the code depends only on the PEP 393 layout.
 *
 * Layout of str objects seen by C code (CPython's, so PyUnicode_KIND/DATA/
 * READ/GET_LENGTH/IS_ASCII/MAX_CHAR_VALUE stay CPython's inline code):
 *  - J-backed mirrors of JS strings (kind STR, crossing JS -> C): the
 *    non-compact PyUnicodeObject.  data.any points INTO the flat JSString
 *    for 8-bit strings (kind 1) and for 16-bit strings without surrogate
 *    pairs whose max char is >= 256 (kind 2).  Otherwise (16-bit strings
 *    holding only Latin-1, or with surrogate pairs = one code point, D3)
 *    the canonical kind 1 / 4 copy is stored inline right after the
 *    PyUnicodeObject (kind_ops.size depends on the string).
 *  - C-primary strings (PyUnicode_New): CPython's compact layouts; the JS
 *    string is built at the first crossing (kind_ops.to_js).
 * The utf8 cache (PyUnicode_AsUTF8AndSize) is a js_malloc_rt block owned
 * by the mirror, except for ASCII strings whose data is NUL-terminated
 * (shared, as in CPython).
 *
 * Errors are the pending exception of qp_capi.ctx: the CPython ports raise
 * with the exported PyErr_* functions, the JS paths with qp_raise*.
 */

#include <wchar.h>

_Py_COMP_DIAG_PUSH
_Py_COMP_DIAG_IGNORE_DEPR_DECLS

#define QPD_MAX_UNICODE 0x10FFFF

static int qp_capi_init_bytes(JSContext *ctx);     /* python/capi_bytes.c */

/* TU-static functions of python/unicodeobject.c, unicode_format.c and
   mod_codecs.c (defined earlier in the quickjs.c translation unit; repeated
   here so that build/check/unit.sh, which sees only the headers, checks
   this file too). */
static JSValueConst qp_str_unbox(JSValueConst v);
static JSString *qp_str_flat(JSContext *ctx, JSValueConst v);
static JSValue qp_str_new_ucs(JSContext *ctx, int kind, const void *data, Py_ssize_t len);
static JSValue qp_str_from_encoded(JSContext *ctx, JSValueConst obj, const char *encoding,
                                   const char *errors);
static JSValue qp_str_remainder(JSContext *ctx, JSValueConst a, JSValueConst b);
static int qp_codec_register(JSContext *ctx, JSValueConst search_function);
static int qp_codec_unregister(JSContext *ctx, JSValueConst search_function);
static JSValue qp_codec_lookup(JSContext *ctx, const char *encoding);
static JSValue qp_cd_codec_getitem(JSContext *ctx, const char *encoding, int index);
static JSValue qp_codec_encode(JSContext *ctx, JSValueConst object, const char *encoding,
                               const char *errors);
static JSValue qp_codec_decode(JSContext *ctx, JSValueConst object, const char *encoding,
                               const char *errors);
static int qp_codec_register_error(JSContext *ctx, const char *name, JSValueConst error);
static JSValue qp_codec_lookup_error(JSContext *ctx, const char *name);
static JSValue qp_cd_build_encoding_map(JSContext *ctx, JSValueConst string);
static JSValue qp_cd_decode_utf7(JSContext *ctx, const void *data, Py_ssize_t size,
                                 const char *errors, Py_ssize_t *consumed);
static JSValue qp_cd_decode_utf8(JSContext *ctx, const void *s, Py_ssize_t size,
                                 const char *errors, Py_ssize_t *consumed);
static JSValue qp_cd_decode_utf16(JSContext *ctx, const void *data, Py_ssize_t size,
                                  const char *errors, int *byteorder, Py_ssize_t *consumed);
static JSValue qp_cd_decode_utf32(JSContext *ctx, const void *data, Py_ssize_t size,
                                  const char *errors, int *byteorder, Py_ssize_t *consumed);
static JSValue qp_cd_encode_utf16_32(JSContext *ctx, JSValueConst s, const char *errors,
                                     int byteorder, int width);
static JSValue qp_cd_decode_unicode_escape(JSContext *ctx, const void *data, Py_ssize_t size,
                                           const char *errors, Py_ssize_t *consumed);
static JSValue qp_cd_encode_unicode_escape(JSContext *ctx, JSValueConst str);
static JSValue qp_cd_decode_raw_unicode_escape(JSContext *ctx, const void *data, Py_ssize_t size,
                                               const char *errors, Py_ssize_t *consumed);
static JSValue qp_cd_encode_raw_unicode_escape(JSContext *ctx, JSValueConst str);
static JSValue qp_cd_encode_ucs(JSContext *ctx, JSValueConst s, const char *errors, Py_UCS4 limit);
static JSValue qp_cd_decode_ascii(JSContext *ctx, const void *s, Py_ssize_t size, const char *errors);
static JSValue qp_cd_decode_charmap(JSContext *ctx, const void *data, Py_ssize_t size,
                                    JSValueConst mapping, const char *errors);
static JSValue qp_cd_encode_charmap(JSContext *ctx, JSValueConst str, JSValueConst mapping,
                                    const char *errors);

/* pycore_unicodeobject.h / pycore_symtable.h / pycore_unicodectype.h
   exports (internal headers are compat shims outside this TU). */
PyAPI_FUNC(PyObject *) _PyUnicode_Copy(PyObject *unicode);
PyAPI_FUNC(PyObject *) _PyUnicode_JoinArray(PyObject *separator, PyObject *const *items,
                                            Py_ssize_t seqlen);
PyAPI_FUNC(int) _PyUnicode_Equal(PyObject *, PyObject *);
PyAPI_FUNC(int) _PyUnicode_EqualToASCIIString(PyObject *left, const char *right);
PyAPI_FUNC(void) _PyUnicode_InternMortal(PyInterpreterState *interp, PyObject **);
PyAPI_FUNC(const char *) _PyUnicode_AsUTF8NoNUL(PyObject *);
PyAPI_FUNC(int) _PyUnicode_CheckConsistency(PyObject *op, int check_content);
PyAPI_FUNC(int) _PyUnicode_IsXidStart(Py_UCS4 ch);
PyAPI_FUNC(int) _PyUnicode_IsXidContinue(Py_UCS4 ch);
PyAPI_FUNC(PyObject *) _Py_Mangle(PyObject *privateobj, PyObject *ident);
PyAPI_FUNC(int) _Py_IsPrivateName(PyObject *ident);

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

static inline qp_strview qpd_view(PyObject *u)
{
    return qp_strview_make(PyUnicode_KIND(u), PyUnicode_DATA(u), PyUnicode_GET_LENGTH(u));
}

static PyObject *qpd_empty(void)
{
    return Py_NewRef(_PyQp_Singleton(QP_CAPI_SINGLETON_STR_EMPTY));
}

/* JS result (consumed) -> new reference / NULL + pending exception. */
static PyObject *qpd_from_js(JSValue r)
{
    return qp_capi_from_js_result(qp_capi.ctx, r);
}

/* Borrowed exact JS str of a str object (binds C-primary strings);
   JS_EXCEPTION on failure. */
static JSValueConst qpd_js(PyObject *u)
{
    JSValueConst v = qp_capi_value(qp_capi.ctx, u);
    if (JS_IsException(v))
        return v;
    return qp_str_unbox(v);
}

static int qpd_ensure_unicode(PyObject *obj)
{
    if (!PyUnicode_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "must be str, not %.100s", Py_TYPE(obj)->tp_name);
        return -1;
    }
    return 0;
}

/* Exact max char of data[0:n] (exact for kind 4, kind-exact otherwise). */
static Py_UCS4 qpd_max_char(int kind, const void *data, Py_ssize_t n)
{
    Py_ssize_t i;
    Py_UCS4 m = 0;
    if (kind == PyUnicode_1BYTE_KIND) {
        const Py_UCS1 *s = data;
        for (i = 0; i < n; i++)
            m |= s[i];
    } else if (kind == PyUnicode_2BYTE_KIND) {
        const Py_UCS2 *s = data;
        for (i = 0; i < n; i++)
            m |= s[i];
    } else {
        const Py_UCS4 *s = data;
        for (i = 0; i < n; i++) {
            if (s[i] > m)
                m = s[i];
        }
    }
    return m;
}

/* to[to_start:to_start+n] = from[from_start:from_start+n], converting
   between kinds (the target kind can hold every copied character). */
static void qpd_copy(int to_kind, void *to, Py_ssize_t to_start,
                     int from_kind, const void *from, Py_ssize_t from_start, Py_ssize_t n)
{
    Py_ssize_t i;
    if (n <= 0)
        return;
    if (to_kind == from_kind) {
        memmove((char *)to + to_start * to_kind, (const char *)from + from_start * from_kind,
                (size_t)n * to_kind);
        return;
    }
    for (i = 0; i < n; i++)
        PyUnicode_WRITE(to_kind, to, to_start + i, PyUnicode_READ(from_kind, from, from_start + i));
}

/* Canonical new str of the code points data[0:n] (CPython _PyUnicode_FromUCS1/
   2/4: empty and Latin-1 singletons shared). */
static PyObject *qpd_from_kind(int kind, const void *data, Py_ssize_t n)
{
    PyObject *u;
    Py_UCS4 maxchar;

    if (n == 0)
        return qpd_empty();
    if (n == 1) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, 0);
        if (ch < 256)
            return Py_NewRef(_PyQp_Latin1Char((int)ch));
    }
    maxchar = qpd_max_char(kind, data, n);
    u = PyUnicode_New(n, maxchar);
    if (!u)
        return NULL;
    qpd_copy(PyUnicode_KIND(u), PyUnicode_DATA(u), 0, kind, data, 0, n);
    return u;
}

static PyObject *qpd_from_builder(qp_ucsbuilder *b)
{
    PyObject *r = qpd_from_kind(b->kind, b->data, b->len);
    qp_ucsbuilder_free(b);
    return r;
}

/* Raise the exception of a stringlib status code. */
static void qpd_sl_error(Py_ssize_t r, const char *overflow_msg)
{
    if (r == QP_SL_OVERFLOW)
        PyErr_SetString(PyExc_OverflowError, overflow_msg);
    else if (r == QP_SL_EMPTY_SEP)
        PyErr_SetString(PyExc_ValueError, "empty separator");
    else
        PyErr_NoMemory();
}

/* unicode_result_unchanged */
static PyObject *qpd_result_unchanged(PyObject *u)
{
    if (PyUnicode_CheckExact(u))
        return Py_NewRef(u);
    return _PyUnicode_Copy(u);
}

/* s[start:end] of a str (canonical; the object itself for the whole
   string when it is exact). */
static PyObject *qpd_substring(PyObject *s, Py_ssize_t start, Py_ssize_t end)
{
    int kind = PyUnicode_KIND(s);
    if (start == 0 && end == PyUnicode_GET_LENGTH(s))
        return qpd_result_unchanged(s);
    return qpd_from_kind(kind, (const char *)PyUnicode_DATA(s) + start * kind, end - start);
}

/* _PyUnicode_IsModifiable: a fresh compact C-primary string with one
   reference whose hash was never computed. */
static int qpd_modifiable(PyObject *u)
{
    if (Py_REFCNT(u) != 1)
        return 0;
    if (!QP_CAPI_HAS_HEAD(u) || (QP_CAPI_HEAD(u)->flags & _Py_QP_BOUND_FLAG))
        return 0;
    if (!PyUnicode_IS_COMPACT(u))
        return 0;
    if (_PyASCIIObject_CAST(u)->hash != -1)
        return 0;
    if (PyUnicode_CHECK_INTERNED(u))
        return 0;
    if (!PyUnicode_CheckExact(u))
        return 0;
    return 1;
}

static int qpd_check_modifiable(PyObject *u)
{
    if (!qpd_modifiable(u)) {
        PyErr_SetString(PyExc_SystemError, "Cannot modify a string currently used");
        return -1;
    }
    return 0;
}

/* unicode_compare: -1 / 0 / 1 in code point order. */
static int qpd_compare(PyObject *a, PyObject *b)
{
    int ka = PyUnicode_KIND(a), kb = PyUnicode_KIND(b);
    const void *da = PyUnicode_DATA(a), *db = PyUnicode_DATA(b);
    Py_ssize_t la = PyUnicode_GET_LENGTH(a), lb = PyUnicode_GET_LENGTH(b);
    Py_ssize_t i, n = la < lb ? la : lb;

    if (ka == 1 && kb == 1) {
        int c = n ? memcmp(da, db, (size_t)n) : 0;
        if (c)
            return c < 0 ? -1 : 1;
    } else {
        for (i = 0; i < n; i++) {
            Py_UCS4 ca = PyUnicode_READ(ka, da, i), cb = PyUnicode_READ(kb, db, i);
            if (ca != cb)
                return ca < cb ? -1 : 1;
        }
    }
    return la < lb ? -1 : la > lb ? 1 : 0;
}

/* unicode_eq (kinds need not be canonical). */
static int qpd_eq(PyObject *a, PyObject *b)
{
    Py_ssize_t len = PyUnicode_GET_LENGTH(a), i;
    int ka, kb;
    const void *da, *db;
    if (a == b)
        return 1;
    if (len != PyUnicode_GET_LENGTH(b))
        return 0;
    if (len == 0)
        return 1;
    ka = PyUnicode_KIND(a);
    kb = PyUnicode_KIND(b);
    da = PyUnicode_DATA(a);
    db = PyUnicode_DATA(b);
    if (ka == kb)
        return memcmp(da, db, (size_t)len * ka) == 0;
    for (i = 0; i < len; i++) {
        if (PyUnicode_READ(ka, da, i) != PyUnicode_READ(kb, db, i))
            return 0;
    }
    return 1;
}

/* UnicodeEncodeError(encoding, u, start, end, reason). */
static void qpd_raise_encode_error(PyObject *u, const char *encoding, Py_ssize_t start,
                                   Py_ssize_t end, const char *reason)
{
    JSValueConst v = qpd_js(u);
    if (JS_IsException(v))
        return;
    qp_raise_unicode_encode(qp_capi.ctx, encoding, v, start, end, reason);
}

/* UTF-8 of u ('strict') in a NUL-terminated js_malloc_rt block. */
static int qpd_utf8_encode(PyObject *u, char **pbuf, Py_ssize_t *plen)
{
    JSRuntime *rt = qp_capi.rt;
    qp_codec_error err;
    char *data = NULL, *nd;
    Py_ssize_t len = 0;
    int r;

    r = qp_sl_utf8_encode(rt, qpd_view(u), &data, &len, &err);
    if (r == QP_SL_CODEC_ERROR) {
        qpd_raise_encode_error(u, "utf-8", err.start, err.end, err.reason);
        return -1;
    }
    if (r < 0) {
        PyErr_NoMemory();
        return -1;
    }
    nd = data ? js_realloc_rt(rt, data, (size_t)len + 1) : js_malloc_rt(rt, 1);
    if (!nd) {
        if (data)
            js_free_rt(rt, data);
        PyErr_NoMemory();
        return -1;
    }
    nd[len] = '\0';
    *pbuf = nd;
    *plen = len;
    return 0;
}

/* Strict UTF-8 decoding into a canonical str (consumed != NULL: stateful,
   stops before an incomplete trailing sequence). */
static PyObject *qpd_decode_utf8(const char *s, Py_ssize_t size, Py_ssize_t *consumed)
{
    qp_ucsbuilder b;
    qp_codec_error err;
    Py_ssize_t i;
    int r;

    for (i = 0; i < size; i++) {
        if ((unsigned char)s[i] & 0x80)
            break;
    }
    if (i == size) {
        if (consumed)
            *consumed = size;
        return qpd_from_kind(PyUnicode_1BYTE_KIND, s, size);
    }
    qp_ucsbuilder_init(&b, qp_capi.rt);
    r = qp_sl_utf8_decode(s, size, consumed, &b, &err);
    if (r == QP_SL_CODEC_ERROR) {
        qp_ucsbuilder_free(&b);
        qp_raise_unicode_decode(qp_capi.ctx, "utf-8", s, size, err.start, err.end, err.reason);
        return NULL;
    }
    if (r < 0) {
        qp_ucsbuilder_free(&b);
        PyErr_NoMemory();
        return NULL;
    }
    return qpd_from_builder(&b);
}

static int qpd_is_strict(const char *errors)
{
    return errors == NULL || strcmp(errors, "strict") == 0;
}

/* encoding NULL or a spelling of UTF-8 (_Py_normalize_encoding + the
   "utf_8"/"utf8" fast-path test of PyUnicode_Decode/AsEncodedString). */
static int qpd_is_utf8(const char *encoding)
{
    char buf[8];
    size_t i;
    if (encoding == NULL)
        return 1;
    for (i = 0; encoding[i] && i < sizeof(buf) - 1; i++) {
        char c = encoding[i];
        buf[i] = (c == '-' || c == ' ') ? '_' : (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    if (encoding[i])
        return 0;
    buf[i] = '\0';
    return !strcmp(buf, "utf_8") || !strcmp(buf, "utf8");
}

/* ========================================================================= */
/* Kind STR (mirror ops)                                                      */
/* ========================================================================= */

typedef struct QPDStrPlan {
    JSString *p;        /* flat string */
    int kind;           /* canonical kind */
    int copy;           /* canonical data stored inline after the struct */
    Py_ssize_t len;     /* code points */
} QPDStrPlan;

/* Representation of the str value of v (a JS string or a boxed str
   subclass instance). */
static int qpd_str_plan(JSContext *ctx, JSValueConst v, QPDStrPlan *pl)
{
    JSValueConst s = qp_str_unbox(v);
    JSString *p;
    Py_ssize_t i, n, pairs = 0;
    Py_UCS4 maxc = 0;

    if (!qp_str_check_exact(s)) {
        qp_raise_system_error(ctx, "str mirror of a non-str value");
        return -1;
    }
    p = qp_str_flat(ctx, s);
    if (!p)
        return -1;
    pl->p = p;
    n = p->len;
    if (!p->is_wide_char) {
        pl->kind = PyUnicode_1BYTE_KIND;
        pl->copy = 0;
        pl->len = n;
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint16_t c = p->u.str16[i];
        maxc |= c;
        if ((c & 0xFC00) == 0xD800 && i + 1 < n && (p->u.str16[i + 1] & 0xFC00) == 0xDC00) {
            pairs++;
            i++;
        }
    }
    if (pairs) {
        pl->kind = PyUnicode_4BYTE_KIND;
        pl->copy = 1;
        pl->len = n - pairs;
    } else if (maxc < 256) {
        pl->kind = PyUnicode_1BYTE_KIND;
        pl->copy = 1;
        pl->len = n;
    } else {
        pl->kind = PyUnicode_2BYTE_KIND;
        pl->copy = 0;
        pl->len = n;
    }
    return 0;
}

static size_t qpd_str_size(JSContext *ctx, JSValueConst v)
{
    QPDStrPlan pl;
    size_t base = sizeof(PyUnicodeObject) - sizeof(PyObject);
    if (qpd_str_plan(ctx, v, &pl) < 0) {
        /* init fails the same way and reports the error */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return base;
    }
    if (!pl.copy)
        return base;
    return base + (size_t)(pl.len + 1) * pl.kind;
}

static int qpd_str_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyUnicodeObject *u = (PyUnicodeObject *)op;
    PyASCIIObject *a = &u->_base._base;
    QPDStrPlan pl;
    JSString *p;
    Py_ssize_t i;
    int ascii = 0;

    if (qpd_str_plan(ctx, v, &pl) < 0)
        return -1;
    p = pl.p;
    if (!pl.copy) {
        if (pl.kind == PyUnicode_1BYTE_KIND) {
            u->data.any = p->u.str8;
            ascii = 1;
            for (i = 0; i < pl.len; i++) {
                if (p->u.str8[i] & 0x80) {
                    ascii = 0;
                    break;
                }
            }
        } else {
            u->data.any = p->u.str16;
        }
    } else {
        void *d = (uint8_t *)op + sizeof(PyUnicodeObject);
        Py_ssize_t k = 0, n = p->len;
        u->data.any = d;
        if (pl.kind == PyUnicode_1BYTE_KIND) {
            Py_UCS1 *o = d;
            ascii = 1;
            for (i = 0; i < n; i++) {
                o[i] = (Py_UCS1)p->u.str16[i];
                if (o[i] & 0x80)
                    ascii = 0;
            }
            o[n] = 0;
        } else {
            Py_UCS4 *o = d;
            for (i = 0; i < n; i++) {
                Py_UCS4 c = p->u.str16[i];
                if ((c & 0xFC00) == 0xD800 && i + 1 < n && (p->u.str16[i + 1] & 0xFC00) == 0xDC00) {
                    c = 0x10000 + (((c & 0x3FF) << 10) | (p->u.str16[i + 1] & 0x3FF));
                    i++;
                }
                o[k++] = c;
            }
            o[k] = 0;
        }
    }
    a->length = pl.len;
    a->hash = -1;
    a->state.interned = p->atom_type ? SSTATE_INTERNED_MORTAL : SSTATE_NOT_INTERNED;
    a->state.kind = pl.kind;
    a->state.compact = 0;
    a->state.ascii = ascii;
    a->state.statically_allocated = 0;
    u->_base.utf8 = NULL;
    u->_base.utf8_length = 0;
    if (p->atom_type) {
        /* atoms cache their Python hash (qp_str_hash) */
        Py_hash_t h = qp_str_hash(ctx, JS_MKPTR(JS_TAG_STRING, p));
        if (h == -1)
            return -1;
        a->hash = h;
    }
    return 0;
}

static JSValue qpd_str_to_js(JSContext *ctx, PyObject *op)
{
    return qp_str_new_ucs(ctx, PyUnicode_KIND(op), PyUnicode_DATA(op), PyUnicode_GET_LENGTH(op));
}

/* Data pointer of a str for "is the utf8 cache shared" tests (no
   dereference of a dead JS value). */
static const void *qpd_raw_data(PyObject *op)
{
    if (PyUnicode_IS_COMPACT(op))
        return _PyUnicode_COMPACT_DATA(op);
    return ((PyUnicodeObject *)op)->data.any;
}

static void qpd_str_clear(JSRuntime *rt, PyObject *op)
{
    PyCompactUnicodeObject *c;
    if (PyUnicode_IS_COMPACT_ASCII(op))
        return;
    c = (PyCompactUnicodeObject *)op;
    if (c->utf8 && (const void *)c->utf8 != qpd_raw_data(op))
        js_free_rt(rt, c->utf8);
    c->utf8 = NULL;
    c->utf8_length = 0;
}

/* (D, capi.h) immortal interned str mirror for an identifier. */
static PyObject *qp_capi_str_immortal(JSContext *ctx, const char *utf8)
{
    JSValue s;
    JSAtom atom;
    PyObject *op;

    s = qp_str_from_utf8(ctx, utf8, strlen(utf8));
    if (JS_IsException(s))
        return NULL;
    atom = qp_str_to_atom(ctx, s);
    JS_FreeValue(ctx, s);
    if (atom == JS_ATOM_NULL)
        return NULL;
    s = JS_AtomToString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    op = qp_capi_steal(ctx, s);
    if (!op)
        return NULL;
    if (!(op->ob_flags & _Py_STATICALLY_ALLOCATED_FLAG)) {
        op->ob_refcnt = (Py_ssize_t)_Py_IMMORTAL_INITIAL_REFCNT;
        QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    }
    _PyASCIIObject_CAST(op)->state.interned = SSTATE_INTERNED_IMMORTAL;
    return op;
}

/* ========================================================================= */
/* Construction and representation                                           */
/* ========================================================================= */

PyObject *PyUnicode_New(Py_ssize_t size, Py_UCS4 maxchar)
{
    PyObject *obj;
    PyCompactUnicodeObject *unicode;
    void *data;
    int kind, is_ascii = 0;
    Py_ssize_t char_size, struct_size;

    if (size == 0)
        return qpd_empty();
    struct_size = sizeof(PyCompactUnicodeObject);
    if (maxchar < 128) {
        kind = PyUnicode_1BYTE_KIND;
        char_size = 1;
        is_ascii = 1;
        struct_size = sizeof(PyASCIIObject);
    } else if (maxchar < 256) {
        kind = PyUnicode_1BYTE_KIND;
        char_size = 1;
    } else if (maxchar < 65536) {
        kind = PyUnicode_2BYTE_KIND;
        char_size = 2;
    } else {
        if (maxchar > QPD_MAX_UNICODE) {
            PyErr_SetString(PyExc_SystemError, "invalid maximum character passed to PyUnicode_New");
            return NULL;
        }
        kind = PyUnicode_4BYTE_KIND;
        char_size = 4;
    }
    if (size < 0) {
        PyErr_SetString(PyExc_SystemError, "Negative size passed to PyUnicode_New");
        return NULL;
    }
    if (size > ((PY_SSIZE_T_MAX - struct_size) / char_size - 1))
        return PyErr_NoMemory();
    obj = qp_capi_alloc(&PyUnicode_Type, (size_t)(struct_size + (size + 1) * char_size),
                        QP_CAPI_K_STR);
    if (!obj)
        return NULL;
    unicode = (PyCompactUnicodeObject *)obj;
    unicode->_base.length = size;
    unicode->_base.hash = -1;
    unicode->_base.state.interned = 0;
    unicode->_base.state.kind = kind;
    unicode->_base.state.compact = 1;
    unicode->_base.state.ascii = is_ascii;
    unicode->_base.state.statically_allocated = 0;
    if (is_ascii) {
        data = ((PyASCIIObject *)obj) + 1;
        ((char *)data)[size] = 0;
    } else {
        data = unicode + 1;
        unicode->utf8 = NULL;
        unicode->utf8_length = 0;
        if (kind == PyUnicode_1BYTE_KIND)
            ((Py_UCS1 *)data)[size] = 0;
        else if (kind == PyUnicode_2BYTE_KIND)
            ((Py_UCS2 *)data)[size] = 0;
        else
            ((Py_UCS4 *)data)[size] = 0;
    }
    return obj;
}

int (PyUnicode_KIND)(PyObject *op)
{
    return PyUnicode_KIND(op);
}

void *(PyUnicode_DATA)(PyObject *op)
{
    return _PyUnicode_DATA(op);
}

PyObject *PyUnicode_FromKindAndData(int kind, const void *buffer, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return NULL;
    }
    switch (kind) {
    case PyUnicode_1BYTE_KIND:
    case PyUnicode_2BYTE_KIND:
        return qpd_from_kind(kind, buffer, size);
    case PyUnicode_4BYTE_KIND:
        if (qpd_max_char(kind, buffer, size) > QPD_MAX_UNICODE) {
            PyErr_SetString(PyExc_SystemError, "invalid maximum character passed to PyUnicode_New");
            return NULL;
        }
        return qpd_from_kind(kind, buffer, size);
    default:
        PyErr_SetString(PyExc_SystemError, "invalid kind");
        return NULL;
    }
}

PyObject *PyUnicode_FromStringAndSize(const char *u, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_SetString(PyExc_SystemError, "Negative size passed to PyUnicode_FromStringAndSize");
        return NULL;
    }
    if (u != NULL)
        return qpd_decode_utf8(u, size, NULL);
    if (size > 0) {
        PyErr_SetString(PyExc_SystemError,
                        "NULL string with positive size with NULL passed to PyUnicode_FromStringAndSize");
        return NULL;
    }
    return qpd_empty();
}

PyObject *PyUnicode_FromString(const char *u)
{
    size_t size = strlen(u);
    if (size > PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "input too long");
        return NULL;
    }
    return qpd_decode_utf8(u, (Py_ssize_t)size, NULL);
}

PyObject *PyUnicode_FromOrdinal(int ordinal)
{
    PyObject *u;
    if (ordinal < 0 || ordinal > QPD_MAX_UNICODE) {
        PyErr_SetString(PyExc_ValueError, "chr() arg not in range(0x110000)");
        return NULL;
    }
    if (ordinal < 256)
        return Py_NewRef(_PyQp_Latin1Char(ordinal));
    u = PyUnicode_New(1, (Py_UCS4)ordinal);
    if (!u)
        return NULL;
    PyUnicode_WRITE(PyUnicode_KIND(u), PyUnicode_DATA(u), 0, (Py_UCS4)ordinal);
    return u;
}

PyObject *PyUnicode_FromWideChar(const wchar_t *w, Py_ssize_t size)
{
    Py_ssize_t i;
    _Static_assert(sizeof(wchar_t) == 4, "4-byte wchar_t expected");
    if (w == NULL && size != 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (size == -1)
        size = (Py_ssize_t)wcslen(w);
    for (i = 0; i < size; i++) {
        if ((Py_UCS4)w[i] > QPD_MAX_UNICODE) {
            PyErr_Format(PyExc_ValueError, "character U+%x is not in range [U+0000; U+10ffff]",
                         (unsigned int)(Py_UCS4)w[i]);
            return NULL;
        }
    }
    return qpd_from_kind(PyUnicode_4BYTE_KIND, w, size);
}

PyObject *PyUnicode_Substring(PyObject *str, Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t length;
    if (qpd_ensure_unicode(str) < 0)
        return NULL;
    length = PyUnicode_GET_LENGTH(str);
    if (end > length)
        end = length;
    if (start == 0 && end == length)
        return qpd_result_unchanged(str);
    if (start < 0 || end < 0) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return NULL;
    }
    if (start >= length || end < start)
        return qpd_empty();
    return qpd_substring(str, start, end);
}

PyObject *_PyUnicode_Copy(PyObject *unicode)
{
    Py_ssize_t length;
    PyObject *copy;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    length = PyUnicode_GET_LENGTH(unicode);
    copy = PyUnicode_New(length, PyUnicode_MAX_CHAR_VALUE(unicode));
    if (!copy)
        return NULL;
    qpd_copy(PyUnicode_KIND(copy), PyUnicode_DATA(copy), 0,
             PyUnicode_KIND(unicode), PyUnicode_DATA(unicode), 0, length);
    return copy;
}

PyObject *PyUnicode_FromObject(PyObject *obj)
{
    if (PyUnicode_CheckExact(obj))
        return Py_NewRef(obj);
    if (PyUnicode_Check(obj))
        return _PyUnicode_Copy(obj);
    PyErr_Format(PyExc_TypeError, "Can't convert '%.100s' object to str implicitly",
                 Py_TYPE(obj)->tp_name);
    return NULL;
}

PyObject *PyUnicode_FromEncodedObject(PyObject *obj, const char *encoding, const char *errors)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst v;

    if (obj == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (PyBytes_Check(obj)) {
        if (PyBytes_GET_SIZE(obj) == 0)
            return qpd_empty();
        return PyUnicode_Decode(PyBytes_AS_STRING(obj), PyBytes_GET_SIZE(obj), encoding, errors);
    }
    v = qp_capi_value(ctx, obj);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_str_from_encoded(ctx, v, encoding, errors));
}

Py_ssize_t PyUnicode_GetLength(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return -1;
    }
    return PyUnicode_GET_LENGTH(unicode);
}

Py_UCS4 PyUnicode_ReadChar(PyObject *unicode, Py_ssize_t index)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return (Py_UCS4)-1;
    }
    if (index < 0 || index >= PyUnicode_GET_LENGTH(unicode)) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return (Py_UCS4)-1;
    }
    return PyUnicode_READ_CHAR(unicode, index);
}

int PyUnicode_WriteChar(PyObject *unicode, Py_ssize_t index, Py_UCS4 ch)
{
    if (!PyUnicode_Check(unicode) || !PyUnicode_IS_COMPACT(unicode)) {
        PyErr_BadArgument();
        return -1;
    }
    if (index < 0 || index >= PyUnicode_GET_LENGTH(unicode)) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -1;
    }
    if (qpd_check_modifiable(unicode))
        return -1;
    if (ch > PyUnicode_MAX_CHAR_VALUE(unicode)) {
        PyErr_SetString(PyExc_ValueError, "character out of range");
        return -1;
    }
    PyUnicode_WRITE(PyUnicode_KIND(unicode), PyUnicode_DATA(unicode), index, ch);
    return 0;
}

int PyUnicode_Resize(PyObject **p_unicode, Py_ssize_t length)
{
    PyObject *unicode, *res;
    Py_ssize_t old_length;

    if (p_unicode == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    unicode = *p_unicode;
    if (unicode == NULL || !PyUnicode_Check(unicode) || length < 0) {
        PyErr_BadInternalCall();
        return -1;
    }
    old_length = PyUnicode_GET_LENGTH(unicode);
    if (old_length == length)
        return 0;
    if (length == 0) {
        Py_SETREF(*p_unicode, qpd_empty());
        return 0;
    }
    /* mirror blocks are never reallocated in place: resize_copy */
    res = PyUnicode_New(length, PyUnicode_MAX_CHAR_VALUE(unicode));
    if (!res)
        return -1;
    qpd_copy(PyUnicode_KIND(res), PyUnicode_DATA(res), 0, PyUnicode_KIND(unicode),
             PyUnicode_DATA(unicode), 0, old_length < length ? old_length : length);
    if (length > old_length)
        memset((char *)PyUnicode_DATA(res) + old_length * PyUnicode_KIND(res), 0,
               (size_t)(length - old_length) * PyUnicode_KIND(res));
    Py_SETREF(*p_unicode, res);
    return 0;
}

Py_ssize_t PyUnicode_CopyCharacters(PyObject *to, Py_ssize_t to_start, PyObject *from,
                                    Py_ssize_t from_start, Py_ssize_t how_many)
{
    int from_kind, to_kind;
    const void *from_data;
    Py_ssize_t i;

    if (!PyUnicode_Check(from) || !PyUnicode_Check(to)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if ((size_t)from_start > (size_t)PyUnicode_GET_LENGTH(from)) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -1;
    }
    if ((size_t)to_start > (size_t)PyUnicode_GET_LENGTH(to)) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -1;
    }
    if (how_many < 0) {
        PyErr_SetString(PyExc_SystemError, "how_many cannot be negative");
        return -1;
    }
    how_many = Py_MIN(PyUnicode_GET_LENGTH(from) - from_start, how_many);
    if (to_start + how_many > PyUnicode_GET_LENGTH(to)) {
        PyErr_Format(PyExc_SystemError,
                     "Cannot write %zi characters at %zi in a string of %zi characters",
                     how_many, to_start, PyUnicode_GET_LENGTH(to));
        return -1;
    }
    if (how_many == 0)
        return 0;
    if (qpd_check_modifiable(to))
        return -1;
    from_kind = PyUnicode_KIND(from);
    to_kind = PyUnicode_KIND(to);
    from_data = PyUnicode_DATA(from);
    if (from_kind > to_kind) {
        Py_UCS4 to_max = PyUnicode_MAX_CHAR_VALUE(to);
        for (i = 0; i < how_many; i++) {
            if (PyUnicode_READ(from_kind, from_data, from_start + i) > to_max) {
                static const char *const names[] = { "", "UCS1", "UCS2", "", "UCS4" };
                PyErr_Format(PyExc_SystemError,
                             "Cannot copy %s characters into a string of %s characters",
                             names[from_kind], names[to_kind]);
                return -1;
            }
        }
    }
    qpd_copy(to_kind, PyUnicode_DATA(to), to_start, from_kind, from_data, from_start, how_many);
    return how_many;
}

Py_ssize_t PyUnicode_Fill(PyObject *unicode, Py_ssize_t start, Py_ssize_t length, Py_UCS4 fill_char)
{
    Py_ssize_t maxlen, i;
    int kind;
    void *data;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (qpd_check_modifiable(unicode))
        return -1;
    if (start < 0) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -1;
    }
    if (fill_char > PyUnicode_MAX_CHAR_VALUE(unicode)) {
        PyErr_SetString(PyExc_ValueError, "fill character is bigger than the string maximum character");
        return -1;
    }
    maxlen = PyUnicode_GET_LENGTH(unicode) - start;
    length = Py_MIN(maxlen, length);
    if (length <= 0)
        return 0;
    kind = PyUnicode_KIND(unicode);
    data = PyUnicode_DATA(unicode);
    for (i = 0; i < length; i++)
        PyUnicode_WRITE(kind, data, start + i, fill_char);
    return length;
}

/* as_ucs4 */
static Py_UCS4 *qpd_as_ucs4(PyObject *string, Py_UCS4 *target, Py_ssize_t targetsize, int copy_null)
{
    int kind;
    const void *data;
    Py_ssize_t len, targetlen;

    if (!PyUnicode_Check(string)) {
        PyErr_BadArgument();
        return NULL;
    }
    kind = PyUnicode_KIND(string);
    data = PyUnicode_DATA(string);
    len = PyUnicode_GET_LENGTH(string);
    targetlen = len;
    if (copy_null)
        targetlen++;
    if (!target) {
        target = PyMem_New(Py_UCS4, targetlen);
        if (!target) {
            PyErr_NoMemory();
            return NULL;
        }
    } else if (targetsize < targetlen) {
        PyErr_Format(PyExc_SystemError, "string is longer than the buffer");
        if (copy_null && 0 < targetsize)
            target[0] = 0;
        return NULL;
    }
    qpd_copy(PyUnicode_4BYTE_KIND, target, 0, kind, data, 0, len);
    if (copy_null)
        target[len] = 0;
    return target;
}

Py_UCS4 *PyUnicode_AsUCS4(PyObject *string, Py_UCS4 *target, Py_ssize_t targetsize, int copy_null)
{
    if (target == NULL || targetsize < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qpd_as_ucs4(string, target, targetsize, copy_null);
}

Py_UCS4 *PyUnicode_AsUCS4Copy(PyObject *string)
{
    return qpd_as_ucs4(string, NULL, 0, 1);
}

/* unicode_copy_as_widechar (4-byte wchar_t): `size` units, the last one
   being the terminating NUL when size > length. */
static void qpd_copy_as_widechar(PyObject *unicode, wchar_t *w, Py_ssize_t size)
{
    Py_ssize_t len = PyUnicode_GET_LENGTH(unicode), n = size < len ? size : len, i;
    int kind = PyUnicode_KIND(unicode);
    const void *data = PyUnicode_DATA(unicode);
    for (i = 0; i < n; i++)
        w[i] = (wchar_t)PyUnicode_READ(kind, data, i);
    if (size > len)
        w[len] = 0;
}

Py_ssize_t PyUnicode_AsWideChar(PyObject *unicode, wchar_t *w, Py_ssize_t size)
{
    Py_ssize_t res;
    if (unicode == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return -1;
    }
    res = PyUnicode_GET_LENGTH(unicode);
    if (w == NULL)
        return res + 1;
    if (size > res)
        size = res + 1;
    else
        res = size;
    qpd_copy_as_widechar(unicode, w, size);
    return res;
}

wchar_t *PyUnicode_AsWideCharString(PyObject *unicode, Py_ssize_t *size)
{
    wchar_t *buffer;
    Py_ssize_t buflen;

    if (unicode == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    buflen = PyUnicode_GET_LENGTH(unicode);
    buffer = PyMem_New(wchar_t, buflen + 1);
    if (buffer == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    qpd_copy_as_widechar(unicode, buffer, buflen + 1);
    if (size != NULL) {
        *size = buflen;
    } else if (wcslen(buffer) != (size_t)buflen) {
        PyMem_Free(buffer);
        PyErr_SetString(PyExc_ValueError, "embedded null character");
        return NULL;
    }
    return buffer;
}

const char *PyUnicode_AsUTF8AndSize(PyObject *unicode, Py_ssize_t *psize)
{
    PyCompactUnicodeObject *c;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        if (psize)
            *psize = -1;
        return NULL;
    }
    if (PyUnicode_IS_COMPACT_ASCII(unicode)) {
        if (psize)
            *psize = PyUnicode_GET_LENGTH(unicode);
        return (const char *)_PyUnicode_COMPACT_DATA(unicode);
    }
    c = (PyCompactUnicodeObject *)unicode;
    if (c->utf8 == NULL) {
        const Py_UCS1 *d = PyUnicode_DATA(unicode);
        Py_ssize_t len = PyUnicode_GET_LENGTH(unicode);
        if (PyUnicode_IS_ASCII(unicode) && d[len] == 0) {
            /* shared with the (NUL-terminated) ASCII data */
            c->utf8 = (char *)d;
            c->utf8_length = len;
        } else {
            char *buf;
            Py_ssize_t blen;
            if (qpd_utf8_encode(unicode, &buf, &blen) < 0) {
                if (psize)
                    *psize = -1;
                return NULL;
            }
            c->utf8 = buf;
            c->utf8_length = blen;
        }
    }
    if (psize)
        *psize = c->utf8_length;
    return c->utf8;
}

const char *PyUnicode_AsUTF8(PyObject *unicode)
{
    return PyUnicode_AsUTF8AndSize(unicode, NULL);
}

const char *_PyUnicode_AsUTF8NoNUL(PyObject *unicode)
{
    Py_ssize_t size;
    const char *s = PyUnicode_AsUTF8AndSize(unicode, &size);
    if (s && strlen(s) != (size_t)size) {
        PyErr_SetString(PyExc_ValueError, "embedded null character");
        return NULL;
    }
    return s;
}

int _PyUnicode_CheckConsistency(PyObject *op, int check_content)
{
    PyASCIIObject *a;
    int kind;

    if (!PyUnicode_Check(op))
        Py_FatalError("_PyUnicode_CheckConsistency: not a str object");
    a = _PyASCIIObject_CAST(op);
    kind = a->state.kind;
    if (kind != PyUnicode_1BYTE_KIND && kind != PyUnicode_2BYTE_KIND && kind != PyUnicode_4BYTE_KIND)
        Py_FatalError("_PyUnicode_CheckConsistency: invalid kind");
    if (a->state.ascii && kind != PyUnicode_1BYTE_KIND)
        Py_FatalError("_PyUnicode_CheckConsistency: ASCII string of a wide kind");
    if (!a->state.compact && ((PyUnicodeObject *)op)->data.any == NULL)
        Py_FatalError("_PyUnicode_CheckConsistency: no data");
    if (check_content) {
        Py_UCS4 maxchar = qpd_max_char(kind, PyUnicode_DATA(op), a->length);
        if (a->state.ascii ? maxchar > 0x7F : kind == PyUnicode_1BYTE_KIND ? maxchar > 0xFF
                                             : kind == PyUnicode_2BYTE_KIND ? maxchar > 0xFFFF
                                                                            : maxchar > QPD_MAX_UNICODE)
            Py_FatalError("_PyUnicode_CheckConsistency: character out of the kind's range");
    }
    return 1;
}

/* ========================================================================= */
/* PyUnicode_FromFormat (CPython format codes over qp_str_from_format)        */
/* ========================================================================= */

#define QPD_SPEC_MAX 64

static int qpd_spec_put(char *spec, int *n, char c)
{
    if (*n >= QPD_SPEC_MAX - 1)
        return -1;
    spec[(*n)++] = c;
    spec[*n] = '\0';
    return 0;
}

static int qpd_spec_put_int(char *spec, int *n, int v)
{
    char buf[16];
    int i, len = snprintf(buf, sizeof(buf), "%d", v);
    for (i = 0; i < len; i++) {
        if (qpd_spec_put(spec, n, buf[i]) < 0)
            return -1;
    }
    return 0;
}

/* One conversion: formats the argument(s) of `spec` (one directive, with
   '*' already substituted) into a str. */
static JSValue qpd_format_arg(JSContext *ctx, const char *spec, char conv, int sizemod,
                              va_list *vargs)
{
    enum { SM_NONE, SM_L, SM_LL, SM_Z, SM_T, SM_J };
    switch (conv) {
    case 'c':
        return qp_str_from_format(ctx, spec, va_arg(*vargs, int));
    case 'd': case 'i':
        switch (sizemod) {
        case SM_L: return qp_str_from_format(ctx, spec, va_arg(*vargs, long));
        case SM_LL: return qp_str_from_format(ctx, spec, va_arg(*vargs, long long));
        case SM_Z: return qp_str_from_format(ctx, spec, va_arg(*vargs, Py_ssize_t));
        case SM_T: return qp_str_from_format(ctx, spec, va_arg(*vargs, ptrdiff_t));
        case SM_J: return qp_str_from_format(ctx, spec, va_arg(*vargs, intmax_t));
        default: return qp_str_from_format(ctx, spec, va_arg(*vargs, int));
        }
    case 'o': case 'u': case 'x': case 'X':
        switch (sizemod) {
        case SM_L: return qp_str_from_format(ctx, spec, va_arg(*vargs, unsigned long));
        case SM_LL: return qp_str_from_format(ctx, spec, va_arg(*vargs, unsigned long long));
        case SM_Z: return qp_str_from_format(ctx, spec, va_arg(*vargs, size_t));
        case SM_T: return qp_str_from_format(ctx, spec, va_arg(*vargs, ptrdiff_t));
        case SM_J: return qp_str_from_format(ctx, spec, va_arg(*vargs, uintmax_t));
        default: return qp_str_from_format(ctx, spec, va_arg(*vargs, unsigned int));
        }
    case 'p':
        return qp_str_from_format(ctx, spec, va_arg(*vargs, void *));
    case 's':
        if (sizemod)
            return qp_str_from_format(ctx, spec, va_arg(*vargs, const wchar_t *));
        return qp_str_from_format(ctx, spec, va_arg(*vargs, const char *));
    case 'V': {
        PyObject *obj = va_arg(*vargs, PyObject *);
        const void *str = sizemod ? (const void *)va_arg(*vargs, const wchar_t *)
                                  : (const void *)va_arg(*vargs, const char *);
        JSValue v = QP_NULL;
        if (obj) {
            v = qp_capi_value(ctx, obj);
            if (JS_IsException(v))
                return v;
        }
        return qp_str_from_format(ctx, spec, v, str);
    }
    case 'U': case 'S': case 'R': case 'A': case 'T': {
        PyObject *obj = va_arg(*vargs, PyObject *);
        JSValue v;
        if (!obj)
            return qp_raise_system_error(ctx, "PyUnicode_FromFormat: NULL object for %%%c", conv);
        v = qp_capi_value(ctx, obj);
        if (JS_IsException(v))
            return v;
        if (conv == 'U' && !qp_str_check(ctx, v))
            return qp_raise_system_error(ctx, "PyUnicode_FromFormat: %%U argument is not a str");
        return qp_str_from_format(ctx, spec, v);
    }
    case 'N': {
        PyTypeObject *tp = va_arg(*vargs, PyTypeObject *);
        QPType *t;
        if (!tp || !PyType_Check((PyObject *)tp))
            return qp_raise_type_error(ctx, "%%N argument must be a type");
        t = qp_capi_type_qp(tp);
        if (!t)
            return JS_EXCEPTION;
        return qp_str_from_format(ctx, spec, t);
    }
    default:
        return qp_raise_system_error(ctx, "invalid format string: %s", spec);
    }
}

static JSValue qpd_from_format_js(JSContext *ctx, const char *format, va_list *vargs)
{
    enum { SM_NONE, SM_L, SM_LL, SM_Z, SM_T, SM_J };
    StringBuffer b;
    const char *f, *start;
    char spec[QPD_SPEC_MAX];
    int n, sizemod;
    size_t i;

    for (i = 0; format[i]; i++) {
        if ((unsigned char)format[i] > 127) {
            return qp_raise_value_error(ctx, "PyUnicode_FromFormatV() expects an ASCII-encoded "
                                        "format string, got a non-ASCII byte: 0x%02x",
                                        (unsigned)(unsigned char)format[i]);
        }
    }
    string_buffer_init(ctx, &b, (int)(i + 16));
    for (f = format; *f; ) {
        JSValue piece;
        if (*f != '%') {
            const char *q = strchr(f, '%');
            size_t len = q ? (size_t)(q - f) : strlen(f);
            if (string_buffer_write8(&b, (const uint8_t *)f, (int)len))
                goto fail;
            f += len;
            continue;
        }
        start = f++;
        if (*f == '%') {
            if (string_buffer_putc8(&b, '%'))
                goto fail;
            f++;
            continue;
        }
        n = 0;
        spec[0] = '\0';
        if (qpd_spec_put(spec, &n, '%') < 0)
            goto invalid;
        while (*f == '-' || *f == '0' || *f == '#') {
            if (qpd_spec_put(spec, &n, *f++) < 0)
                goto invalid;
        }
        if (*f == '*') {
            if (qpd_spec_put_int(spec, &n, va_arg(*vargs, int)) < 0)
                goto invalid;
            f++;
        } else {
            while (*f >= '0' && *f <= '9') {
                if (qpd_spec_put(spec, &n, *f++) < 0)
                    goto invalid;
            }
        }
        if (*f == '.') {
            f++;
            if (*f == '*') {
                int prec = va_arg(*vargs, int);
                f++;
                if (prec >= 0 && (qpd_spec_put(spec, &n, '.') < 0 ||
                                  qpd_spec_put_int(spec, &n, prec) < 0))
                    goto invalid;
            } else {
                if (qpd_spec_put(spec, &n, '.') < 0)
                    goto invalid;
                while (*f >= '0' && *f <= '9') {
                    if (qpd_spec_put(spec, &n, *f++) < 0)
                        goto invalid;
                }
            }
        }
        sizemod = SM_NONE;
        if (*f == 'l') {
            if (f[1] == 'l') {
                sizemod = SM_LL;
                if (qpd_spec_put(spec, &n, *f++) < 0)
                    goto invalid;
            } else {
                sizemod = SM_L;
            }
            if (qpd_spec_put(spec, &n, *f++) < 0)
                goto invalid;
        } else if (*f == 'z' || *f == 't' || *f == 'j') {
            sizemod = *f == 'z' ? SM_Z : *f == 't' ? SM_T : SM_J;
            if (qpd_spec_put(spec, &n, *f++) < 0)
                goto invalid;
        }
        if (!*f)
            goto invalid_at;
        if (qpd_spec_put(spec, &n, *f) < 0)
            goto invalid;
        if ((*f == 's' || *f == 'V') && sizemod != SM_NONE && sizemod != SM_L)
            goto invalid_at;
        piece = qpd_format_arg(ctx, spec, *f, sizemod, vargs);
        f++;
        if (JS_IsException(piece))
            goto fail;
        if (string_buffer_concat_value_free(&b, piece))
            goto fail;
    }
    return string_buffer_end(&b);

invalid:
invalid_at:
    qp_raise_system_error(ctx, "invalid format string: %s", start);
fail:
    string_buffer_free(&b);
    return JS_EXCEPTION;
}

PyObject *PyUnicode_FromFormatV(const char *format, va_list vargs)
{
    va_list ap;
    JSValue r;
    va_copy(ap, vargs);
    r = qpd_from_format_js(qp_capi.ctx, format, &ap);
    va_end(ap);
    return qpd_from_js(r);
}

PyObject *PyUnicode_FromFormat(const char *format, ...)
{
    va_list ap;
    JSValue r;
    va_start(ap, format);
    r = qpd_from_format_js(qp_capi.ctx, format, &ap);
    va_end(ap);
    return qpd_from_js(r);
}

/* ========================================================================= */
/* Interning                                                                  */
/* ========================================================================= */

void _PyUnicode_InternMortal(PyInterpreterState *interp, PyObject **p)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *s = *p, *r;
    JSValueConst v;
    JSAtom atom;
    (void)interp;

    if (s == NULL || !PyUnicode_CheckExact(s) || PyUnicode_CHECK_INTERNED(s))
        return;
    v = qpd_js(s);
    if (JS_IsException(v)) {
        PyErr_Clear();
        return;
    }
    atom = qp_str_to_atom(ctx, v);
    if (atom == JS_ATOM_NULL) {
        PyErr_Clear();
        return;
    }
    r = qp_capi_steal(ctx, JS_AtomToString(ctx, atom));
    JS_FreeAtom(ctx, atom);
    if (!r) {
        PyErr_Clear();
        return;
    }
    if (!PyUnicode_CHECK_INTERNED(r))
        _PyASCIIObject_CAST(r)->state.interned = SSTATE_INTERNED_MORTAL;
    Py_SETREF(*p, r);
}

/* _PyUnicode_InternImmortal: the identifier table of unit A for names
   without NUL, mortal atom interning otherwise. */
static void qpd_intern_immortal(PyObject **p)
{
    PyObject *s = *p, *r;
    const char *utf8;
    Py_ssize_t len;

    if (s == NULL || !PyUnicode_CheckExact(s))
        return;
    if (PyUnicode_CHECK_INTERNED(s) >= SSTATE_INTERNED_IMMORTAL)
        return;
    utf8 = PyUnicode_AsUTF8AndSize(s, &len);
    if (!utf8 || strlen(utf8) != (size_t)len) {
        if (!utf8)
            PyErr_Clear();
        _PyUnicode_InternMortal(NULL, p);
        return;
    }
    r = qp_capi_intern_id(utf8);
    if (!r) {
        PyErr_Clear();
        return;
    }
    Py_SETREF(*p, Py_NewRef(r));
}

void PyUnicode_InternInPlace(PyObject **p)
{
    qpd_intern_immortal(p);
}

PyObject *PyUnicode_InternFromString(const char *cp)
{
    PyObject *r = qp_capi_intern_id(cp);
    return r ? Py_NewRef(r) : NULL;
}

PyObject *_PyUnicode_FromId(_Py_Identifier *id)
{
    return qp_capi_intern_id(id->string);
}

/* ========================================================================= */
/* Codecs                                                                     */
/* ========================================================================= */

const char *PyUnicode_GetDefaultEncoding(void)
{
    return "utf-8";
}

PyObject *PyUnicode_Decode(const char *s, Py_ssize_t size, const char *encoding, const char *errors)
{
    if (size < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (qpd_is_utf8(encoding) && qpd_is_strict(errors))
        return qpd_decode_utf8(s, size, NULL);
    return qpd_from_js(qp_str_decode(qp_capi.ctx, s, size, encoding, errors));
}

PyObject *PyUnicode_AsEncodedString(PyObject *unicode, const char *encoding, const char *errors)
{
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (qpd_is_utf8(encoding) && qpd_is_strict(errors))
        return PyUnicode_AsUTF8String(unicode);
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_str_encode(qp_capi.ctx, v, encoding, errors));
}

PyObject *PyUnicode_BuildEncodingMap(PyObject *string)
{
    JSValueConst v;
    if (!PyUnicode_Check(string)) {
        PyErr_BadArgument();
        return NULL;
    }
    v = qpd_js(string);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_cd_build_encoding_map(qp_capi.ctx, v));
}

PyObject *PyUnicode_DecodeUTF7Stateful(const char *string, Py_ssize_t length, const char *errors,
                                       Py_ssize_t *consumed)
{
    return qpd_from_js(qp_cd_decode_utf7(qp_capi.ctx, string, length, errors, consumed));
}

PyObject *PyUnicode_DecodeUTF7(const char *string, Py_ssize_t length, const char *errors)
{
    return PyUnicode_DecodeUTF7Stateful(string, length, errors, NULL);
}

PyObject *PyUnicode_DecodeUTF8Stateful(const char *string, Py_ssize_t length, const char *errors,
                                       Py_ssize_t *consumed)
{
    if (qpd_is_strict(errors))
        return qpd_decode_utf8(string, length, consumed);
    return qpd_from_js(qp_cd_decode_utf8(qp_capi.ctx, string, length, errors, consumed));
}

PyObject *PyUnicode_DecodeUTF8(const char *string, Py_ssize_t length, const char *errors)
{
    return PyUnicode_DecodeUTF8Stateful(string, length, errors, NULL);
}

PyObject *PyUnicode_AsUTF8String(PyObject *unicode)
{
    char *buf;
    Py_ssize_t len;
    PyObject *r;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyUnicode_IS_ASCII(unicode))
        return PyBytes_FromStringAndSize(PyUnicode_DATA(unicode), PyUnicode_GET_LENGTH(unicode));
    if (((PyCompactUnicodeObject *)unicode)->utf8)
        return PyBytes_FromStringAndSize(((PyCompactUnicodeObject *)unicode)->utf8,
                                         ((PyCompactUnicodeObject *)unicode)->utf8_length);
    if (qpd_utf8_encode(unicode, &buf, &len) < 0)
        return NULL;
    r = PyBytes_FromStringAndSize(buf, len);
    js_free_rt(qp_capi.rt, buf);
    return r;
}

PyObject *PyUnicode_DecodeUTF32Stateful(const char *string, Py_ssize_t length, const char *errors,
                                        int *byteorder, Py_ssize_t *consumed)
{
    return qpd_from_js(qp_cd_decode_utf32(qp_capi.ctx, string, length, errors, byteorder, consumed));
}

PyObject *PyUnicode_DecodeUTF32(const char *string, Py_ssize_t length, const char *errors,
                                int *byteorder)
{
    return PyUnicode_DecodeUTF32Stateful(string, length, errors, byteorder, NULL);
}

PyObject *PyUnicode_DecodeUTF16Stateful(const char *string, Py_ssize_t length, const char *errors,
                                        int *byteorder, Py_ssize_t *consumed)
{
    return qpd_from_js(qp_cd_decode_utf16(qp_capi.ctx, string, length, errors, byteorder, consumed));
}

PyObject *PyUnicode_DecodeUTF16(const char *string, Py_ssize_t length, const char *errors,
                                int *byteorder)
{
    return PyUnicode_DecodeUTF16Stateful(string, length, errors, byteorder, NULL);
}

/* Encoders of _codecs taking the exact JS str. */
static PyObject *qpd_encode_with(PyObject *unicode,
                                 JSValue (*enc)(JSContext *ctx, JSValueConst s))
{
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(enc(qp_capi.ctx, v));
}

static PyObject *qpd_encode_utfn(PyObject *unicode, int width)
{
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_cd_encode_utf16_32(qp_capi.ctx, v, NULL, 0, width));
}

PyObject *PyUnicode_AsUTF32String(PyObject *unicode)
{
    return qpd_encode_utfn(unicode, 4);
}

PyObject *PyUnicode_AsUTF16String(PyObject *unicode)
{
    return qpd_encode_utfn(unicode, 2);
}

PyObject *PyUnicode_DecodeUnicodeEscape(const char *string, Py_ssize_t length, const char *errors)
{
    return qpd_from_js(qp_cd_decode_unicode_escape(qp_capi.ctx, string, length, errors, NULL));
}

PyObject *PyUnicode_AsUnicodeEscapeString(PyObject *unicode)
{
    return qpd_encode_with(unicode, qp_cd_encode_unicode_escape);
}

PyObject *PyUnicode_DecodeRawUnicodeEscape(const char *string, Py_ssize_t length, const char *errors)
{
    return qpd_from_js(qp_cd_decode_raw_unicode_escape(qp_capi.ctx, string, length, errors, NULL));
}

PyObject *PyUnicode_AsRawUnicodeEscapeString(PyObject *unicode)
{
    return qpd_encode_with(unicode, qp_cd_encode_raw_unicode_escape);
}

PyObject *PyUnicode_DecodeLatin1(const char *string, Py_ssize_t length, const char *errors)
{
    (void)errors;
    if (length < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qpd_from_kind(PyUnicode_1BYTE_KIND, string, length);
}

static PyObject *qpd_encode_ucs(PyObject *unicode, const char *errors, Py_UCS4 limit)
{
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyUnicode_IS_ASCII(unicode))
        return PyBytes_FromStringAndSize(PyUnicode_DATA(unicode), PyUnicode_GET_LENGTH(unicode));
    if (limit == 256 && PyUnicode_KIND(unicode) == PyUnicode_1BYTE_KIND)
        return PyBytes_FromStringAndSize(PyUnicode_DATA(unicode), PyUnicode_GET_LENGTH(unicode));
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_cd_encode_ucs(qp_capi.ctx, v, errors, limit));
}

PyObject *PyUnicode_AsLatin1String(PyObject *unicode)
{
    return qpd_encode_ucs(unicode, NULL, 256);
}

PyObject *PyUnicode_DecodeASCII(const char *string, Py_ssize_t length, const char *errors)
{
    return qpd_from_js(qp_cd_decode_ascii(qp_capi.ctx, string, length, errors));
}

PyObject *PyUnicode_AsASCIIString(PyObject *unicode)
{
    return qpd_encode_ucs(unicode, NULL, 128);
}

PyObject *PyUnicode_DecodeCharmap(const char *string, Py_ssize_t length, PyObject *mapping,
                                  const char *errors)
{
    JSValueConst m;
    if (mapping == NULL)
        return PyUnicode_DecodeLatin1(string, length, errors);
    m = qp_capi_value(qp_capi.ctx, mapping);
    if (JS_IsException(m))
        return NULL;
    return qpd_from_js(qp_cd_decode_charmap(qp_capi.ctx, string, length, m, errors));
}

PyObject *PyUnicode_AsCharmapString(PyObject *unicode, PyObject *mapping)
{
    JSValueConst v, m;
    if (!PyUnicode_Check(unicode) || mapping == NULL) {
        PyErr_BadArgument();
        return NULL;
    }
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    m = qp_capi_value(qp_capi.ctx, mapping);
    if (JS_IsException(m))
        return NULL;
    return qpd_from_js(qp_cd_encode_charmap(qp_capi.ctx, v, m, NULL));
}

/* The locale codec is UTF-8 (python/mod_locale.c); CPython's wide-char
   error handlers. */
static const char *qpd_locale_errors(const char *errors)
{
    if (errors == NULL || strcmp(errors, "strict") == 0)
        return "strict";
    if (strcmp(errors, "surrogateescape") == 0 || strcmp(errors, "surrogatepass") == 0)
        return errors;
    PyErr_Format(PyExc_ValueError, "unsupported error handler");
    return NULL;
}

PyObject *PyUnicode_DecodeLocaleAndSize(const char *str, Py_ssize_t len, const char *errors)
{
    const char *eh;
    if (len < 0 || str[len] != '\0' || (size_t)len != strlen(str)) {
        PyErr_SetString(PyExc_ValueError, "embedded null byte");
        return NULL;
    }
    eh = qpd_locale_errors(errors);
    if (!eh)
        return NULL;
    if (!strcmp(eh, "strict"))
        return qpd_decode_utf8(str, len, NULL);
    return qpd_from_js(qp_str_decode(qp_capi.ctx, str, len, "utf-8", eh));
}

PyObject *PyUnicode_DecodeLocale(const char *str, const char *errors)
{
    size_t size = strlen(str);
    return PyUnicode_DecodeLocaleAndSize(str, (Py_ssize_t)size, errors);
}

PyObject *PyUnicode_EncodeLocale(PyObject *unicode, const char *errors)
{
    const char *eh;
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    eh = qpd_locale_errors(errors);
    if (!eh)
        return NULL;
    if (PyUnicode_FindChar(unicode, 0, 0, PyUnicode_GET_LENGTH(unicode), 1) >= 0) {
        PyErr_SetString(PyExc_ValueError, "embedded null character");
        return NULL;
    }
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_str_encode(qp_capi.ctx, v, "utf-8", eh));
}

PyObject *PyUnicode_DecodeFSDefaultAndSize(const char *s, Py_ssize_t size)
{
    return qpd_from_js(qp_str_decode(qp_capi.ctx, s, size, "utf-8", "surrogateescape"));
}

PyObject *PyUnicode_DecodeFSDefault(const char *s)
{
    return PyUnicode_DecodeFSDefaultAndSize(s, (Py_ssize_t)strlen(s));
}

PyObject *PyUnicode_EncodeFSDefault(PyObject *unicode)
{
    JSValueConst v;
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    v = qpd_js(unicode);
    if (JS_IsException(v))
        return NULL;
    return qpd_from_js(qp_str_encode(qp_capi.ctx, v, "utf-8", "surrogateescape"));
}

/* PyOS_FSPath core: str/bytes unchanged, os.PathLike through __fspath__. */
static PyObject *qpd_fspath(PyObject *path)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst v;
    JSValue func, r;
    PyObject *res;

    if (PyUnicode_Check(path) || PyBytes_Check(path))
        return Py_NewRef(path);
    v = qp_capi_value(ctx, path);
    if (JS_IsException(v))
        return NULL;
    func = qp_lookup_special(ctx, v, QP_ATOM(ctx, __fspath__));
    if (QP_IS_NULL(func)) {
        PyErr_Format(PyExc_TypeError, "expected str, bytes or os.PathLike object, not %.200s",
                     Py_TYPE(path)->tp_name);
        return NULL;
    }
    r = qp_call0(ctx, func);
    JS_FreeValue(ctx, func);
    res = qpd_from_js(r);
    if (!res)
        return NULL;
    if (!(PyUnicode_Check(res) || PyBytes_Check(res))) {
        PyErr_Format(PyExc_TypeError, "expected %.200s.__fspath__() to return str or bytes, not %.200s",
                     Py_TYPE(path)->tp_name, Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

int PyUnicode_FSConverter(PyObject *arg, void *addr)
{
    PyObject *path, *output;
    Py_ssize_t size;
    const char *data;

    if (arg == NULL) {
        Py_DECREF(*(PyObject **)addr);
        *(PyObject **)addr = NULL;
        return 1;
    }
    path = qpd_fspath(arg);
    if (path == NULL)
        return 0;
    if (PyBytes_Check(path)) {
        output = path;
    } else {
        output = PyUnicode_EncodeFSDefault(path);
        Py_DECREF(path);
        if (!output)
            return 0;
    }
    size = PyBytes_GET_SIZE(output);
    data = PyBytes_AS_STRING(output);
    if ((size_t)size != strlen(data)) {
        PyErr_SetString(PyExc_ValueError, "embedded null byte");
        Py_DECREF(output);
        return 0;
    }
    *(PyObject **)addr = output;
    return Py_CLEANUP_SUPPORTED;
}

int PyUnicode_FSDecoder(PyObject *arg, void *addr)
{
    PyObject *path, *output;

    if (arg == NULL) {
        Py_DECREF(*(PyObject **)addr);
        *(PyObject **)addr = NULL;
        return 1;
    }
    path = qpd_fspath(arg);
    if (path == NULL)
        return 0;
    if (PyUnicode_Check(path)) {
        output = path;
    } else {
        output = PyUnicode_DecodeFSDefaultAndSize(PyBytes_AS_STRING(path), PyBytes_GET_SIZE(path));
        Py_DECREF(path);
        if (!output)
            return 0;
    }
    if (PyUnicode_FindChar(output, 0, 0, PyUnicode_GET_LENGTH(output), 1) >= 0) {
        PyErr_SetString(PyExc_ValueError, "embedded null character");
        Py_DECREF(output);
        return 0;
    }
    *(PyObject **)addr = output;
    return Py_CLEANUP_SUPPORTED;
}

/* ========================================================================= */
/* Operations                                                                 */
/* ========================================================================= */

PyObject *PyUnicode_Concat(PyObject *left, PyObject *right)
{
    Py_ssize_t llen, rlen;
    Py_UCS4 maxchar;
    PyObject *result;

    if (qpd_ensure_unicode(left) < 0)
        return NULL;
    if (!PyUnicode_Check(right)) {
        PyErr_Format(PyExc_TypeError, "can only concatenate str (not \"%.200s\") to str",
                     Py_TYPE(right)->tp_name);
        return NULL;
    }
    llen = PyUnicode_GET_LENGTH(left);
    rlen = PyUnicode_GET_LENGTH(right);
    if (llen == 0)
        return PyUnicode_FromObject(right);
    if (rlen == 0)
        return PyUnicode_FromObject(left);
    if (llen > PY_SSIZE_T_MAX - rlen) {
        PyErr_SetString(PyExc_OverflowError, "strings are too large to concat");
        return NULL;
    }
    maxchar = Py_MAX(PyUnicode_MAX_CHAR_VALUE(left), PyUnicode_MAX_CHAR_VALUE(right));
    result = PyUnicode_New(llen + rlen, maxchar);
    if (!result)
        return NULL;
    qpd_copy(PyUnicode_KIND(result), PyUnicode_DATA(result), 0, PyUnicode_KIND(left),
             PyUnicode_DATA(left), 0, llen);
    qpd_copy(PyUnicode_KIND(result), PyUnicode_DATA(result), llen, PyUnicode_KIND(right),
             PyUnicode_DATA(right), 0, rlen);
    return result;
}

void PyUnicode_Append(PyObject **p_left, PyObject *right)
{
    PyObject *left, *res;

    if (p_left == NULL) {
        if (!PyErr_Occurred())
            PyErr_BadInternalCall();
        return;
    }
    left = *p_left;
    if (right == NULL || left == NULL || !PyUnicode_Check(left) || !PyUnicode_Check(right)) {
        if (!PyErr_Occurred())
            PyErr_BadInternalCall();
        goto error;
    }
    res = PyUnicode_Concat(left, right);
    if (res == NULL)
        goto error;
    Py_SETREF(*p_left, res);
    return;

error:
    Py_CLEAR(*p_left);
}

void PyUnicode_AppendAndDel(PyObject **pleft, PyObject *right)
{
    PyUnicode_Append(pleft, right);
    Py_XDECREF(right);
}

/* List of the pieces of s described by the spans. */
static PyObject *qpd_spans_to_list(PyObject *s, qp_spanvec *sv)
{
    PyObject *list = PyList_New(sv->len), *item;
    Py_ssize_t i;
    if (!list)
        return NULL;
    for (i = 0; i < sv->len; i++) {
        item = qpd_substring(s, sv->items[i].start, sv->items[i].end);
        if (!item) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, item);
    }
    return list;
}

static PyObject *qpd_split(PyObject *s, PyObject *sep, Py_ssize_t maxsplit, int right)
{
    qp_spanvec sv;
    qp_strview sepv;
    PyObject *res;
    int r;

    if (qpd_ensure_unicode(s) < 0)
        return NULL;
    if (sep != NULL) {
        if (qpd_ensure_unicode(sep) < 0)
            return NULL;
        sepv = qpd_view(sep);
    }
    qp_spanvec_init(&sv, qp_capi.rt);
    if (right)
        r = qp_sl_rsplit(qp_capi.rt, qpd_view(s), sep ? &sepv : NULL, maxsplit, QP_SL_STR, &sv);
    else
        r = qp_sl_split(qp_capi.rt, qpd_view(s), sep ? &sepv : NULL, maxsplit, QP_SL_STR, &sv);
    if (r < 0) {
        qp_spanvec_free(&sv);
        qpd_sl_error(r, "");
        return NULL;
    }
    res = qpd_spans_to_list(s, &sv);
    qp_spanvec_free(&sv);
    return res;
}

PyObject *PyUnicode_Split(PyObject *s, PyObject *sep, Py_ssize_t maxsplit)
{
    return qpd_split(s, sep, maxsplit, 0);
}

PyObject *PyUnicode_RSplit(PyObject *s, PyObject *sep, Py_ssize_t maxsplit)
{
    return qpd_split(s, sep, maxsplit, 1);
}

PyObject *PyUnicode_Splitlines(PyObject *s, int keepends)
{
    qp_spanvec sv;
    PyObject *res;
    int r;

    if (qpd_ensure_unicode(s) < 0)
        return NULL;
    qp_spanvec_init(&sv, qp_capi.rt);
    r = qp_sl_splitlines(qpd_view(s), keepends, QP_SL_STR, &sv);
    if (r < 0) {
        qp_spanvec_free(&sv);
        qpd_sl_error(r, "");
        return NULL;
    }
    res = qpd_spans_to_list(s, &sv);
    qp_spanvec_free(&sv);
    return res;
}

static PyObject *qpd_partition(PyObject *s, PyObject *sep, int direction)
{
    qp_span out[3];
    PyObject *a, *b, *c;
    int r;

    if (qpd_ensure_unicode(s) < 0 || qpd_ensure_unicode(sep) < 0)
        return NULL;
    r = qp_sl_partition(qp_capi.rt, qpd_view(s), qpd_view(sep), direction, out);
    if (r < 0) {
        qpd_sl_error(r, "");
        return NULL;
    }
    if (r == 0) {
        PyObject *e1 = qpd_empty(), *e2 = qpd_empty(), *res;
        res = direction > 0 ? PyTuple_Pack(3, s, e1, e2) : PyTuple_Pack(3, e1, e2, s);
        Py_DECREF(e1);
        Py_DECREF(e2);
        return res;
    }
    a = qpd_substring(s, out[0].start, out[0].end);
    c = a ? qpd_substring(s, out[2].start, out[2].end) : NULL;
    if (!c) {
        Py_XDECREF(a);
        return NULL;
    }
    b = PyTuple_Pack(3, a, sep, c);
    Py_DECREF(a);
    Py_DECREF(c);
    return b;
}

PyObject *PyUnicode_Partition(PyObject *s, PyObject *sep)
{
    return qpd_partition(s, sep, 1);
}

PyObject *PyUnicode_RPartition(PyObject *s, PyObject *sep)
{
    return qpd_partition(s, sep, -1);
}

PyObject *PyUnicode_Translate(PyObject *str, PyObject *table, const char *errors)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst v, t;
    JSValue tv;
    JSAtom name;
    JSValue r;
    (void)errors;

    if (qpd_ensure_unicode(str) < 0)
        return NULL;
    v = qpd_js(str);
    if (JS_IsException(v))
        return NULL;
    t = qp_capi_value(ctx, table);
    if (JS_IsException(t))
        return NULL;
    name = JS_NewAtom(ctx, "translate");
    if (name == JS_ATOM_NULL)
        return NULL;
    tv = JS_DupValue(ctx, t);
    r = qp_call_method(ctx, v, name, (JSValueConst *)&tv, 1, QP_NULL);
    JS_FreeValue(ctx, tv);
    JS_FreeAtom(ctx, name);
    return qpd_from_js(r);
}

PyObject *_PyUnicode_JoinArray(PyObject *separator, PyObject *const *items, Py_ssize_t seqlen)
{
    qp_strview sepv, small[16], *views;
    qp_ucsbuilder b;
    Py_ssize_t i;
    int r;

    if (seqlen == 0)
        return qpd_empty();
    if (seqlen == 1 && PyUnicode_CheckExact(items[0]))
        return Py_NewRef(items[0]);
    if (separator != NULL) {
        if (!PyUnicode_Check(separator)) {
            PyErr_Format(PyExc_TypeError, "separator: expected str instance, %.80s found",
                         Py_TYPE(separator)->tp_name);
            return NULL;
        }
        sepv = qpd_view(separator);
    }
    views = small;
    if (seqlen > (Py_ssize_t)(sizeof(small) / sizeof(small[0]))) {
        views = PyMem_New(qp_strview, seqlen);
        if (!views)
            return PyErr_NoMemory();
    }
    for (i = 0; i < seqlen; i++) {
        if (!PyUnicode_Check(items[i])) {
            PyErr_Format(PyExc_TypeError, "sequence item %zd: expected str instance, %.80s found",
                         i, Py_TYPE(items[i])->tp_name);
            if (views != small)
                PyMem_Free(views);
            return NULL;
        }
        views[i] = qpd_view(items[i]);
    }
    qp_ucsbuilder_init(&b, qp_capi.rt);
    r = qp_sl_join(separator ? &sepv : NULL, views, seqlen, &b);
    if (views != small)
        PyMem_Free(views);
    if (r != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        qpd_sl_error(r, "join() result is too long for a Python string");
        return NULL;
    }
    return qpd_from_builder(&b);
}

PyObject *PyUnicode_Join(PyObject *separator, PyObject *seq)
{
    PyObject *fseq, *res;
    fseq = PySequence_Fast(seq, "can only join an iterable");
    if (fseq == NULL)
        return NULL;
    res = _PyUnicode_JoinArray(separator, PySequence_Fast_ITEMS(fseq), PySequence_Fast_GET_SIZE(fseq));
    Py_DECREF(fseq);
    return res;
}

Py_ssize_t PyUnicode_Tailmatch(PyObject *str, PyObject *substr, Py_ssize_t start, Py_ssize_t end,
                               int direction)
{
    if (qpd_ensure_unicode(str) < 0 || qpd_ensure_unicode(substr) < 0)
        return -1;
    return qp_sl_tailmatch(qpd_view(str), qpd_view(substr), start, end, direction);
}

Py_ssize_t PyUnicode_Find(PyObject *str, PyObject *substr, Py_ssize_t start, Py_ssize_t end,
                          int direction)
{
    Py_ssize_t r;
    if (qpd_ensure_unicode(str) < 0 || qpd_ensure_unicode(substr) < 0)
        return -2;
    r = qp_sl_find(qp_capi.rt, qpd_view(str), qpd_view(substr), start, end, direction);
    if (r == QP_SL_NOMEM) {
        PyErr_NoMemory();
        return -2;
    }
    return r;
}

Py_ssize_t PyUnicode_FindChar(PyObject *str, Py_UCS4 ch, Py_ssize_t start, Py_ssize_t end,
                              int direction)
{
    if (qpd_ensure_unicode(str) < 0)
        return -2;
    return qp_sl_find_char(qpd_view(str), ch, start, end, direction);
}

Py_ssize_t PyUnicode_Count(PyObject *str, PyObject *substr, Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t r;
    if (qpd_ensure_unicode(str) < 0 || qpd_ensure_unicode(substr) < 0)
        return -1;
    r = qp_sl_count(qp_capi.rt, qpd_view(str), qpd_view(substr), start, end);
    if (r < 0) {
        PyErr_NoMemory();
        return -1;
    }
    return r;
}

PyObject *PyUnicode_Replace(PyObject *str, PyObject *substr, PyObject *replstr, Py_ssize_t maxcount)
{
    qp_ucsbuilder b;
    int r;
    if (qpd_ensure_unicode(str) < 0 || qpd_ensure_unicode(substr) < 0 ||
        qpd_ensure_unicode(replstr) < 0)
        return NULL;
    qp_ucsbuilder_init(&b, qp_capi.rt);
    r = qp_sl_replace(qpd_view(str), qpd_view(substr), qpd_view(replstr), maxcount, &b);
    if (r == QP_SL_UNCHANGED) {
        qp_ucsbuilder_free(&b);
        return qpd_result_unchanged(str);
    }
    if (r != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        qpd_sl_error(r, "replace string is too long");
        return NULL;
    }
    return qpd_from_builder(&b);
}

int PyUnicode_Compare(PyObject *left, PyObject *right)
{
    if (PyUnicode_Check(left) && PyUnicode_Check(right)) {
        if (left == right)
            return 0;
        return qpd_compare(left, right);
    }
    PyErr_Format(PyExc_TypeError, "Can't compare %.100s and %.100s",
                 Py_TYPE(left)->tp_name, Py_TYPE(right)->tp_name);
    return -1;
}

int PyUnicode_CompareWithASCIIString(PyObject *uni, const char *str)
{
    int kind = PyUnicode_KIND(uni);
    const void *data = PyUnicode_DATA(uni);
    Py_ssize_t len1 = PyUnicode_GET_LENGTH(uni), i;

    if (kind == PyUnicode_1BYTE_KIND) {
        size_t len2 = strlen(str), len = Py_MIN((size_t)len1, len2);
        int cmp = len ? memcmp(data, str, len) : 0;
        if (cmp != 0)
            return cmp < 0 ? -1 : 1;
        if ((size_t)len1 > len2)
            return 1;
        if ((size_t)len1 < len2)
            return -1;
        return 0;
    }
    for (i = 0; i < len1 && str[i]; i++) {
        Py_UCS4 chr = PyUnicode_READ(kind, data, i);
        if (chr != (unsigned char)str[i])
            return (chr < (unsigned char)str[i]) ? -1 : 1;
    }
    if (i < len1)
        return 1;
    if (str[i])
        return -1;
    return 0;
}

int PyUnicode_EqualToUTF8AndSize(PyObject *unicode, const char *str, Py_ssize_t size)
{
    const unsigned char *s = (const unsigned char *)str, *end;
    int kind;
    const void *data;
    Py_ssize_t len, i;

    if (!PyUnicode_Check(unicode))
        return 0;
    if (size < 0)
        return 0;
    len = PyUnicode_GET_LENGTH(unicode);
    kind = PyUnicode_KIND(unicode);
    data = PyUnicode_DATA(unicode);
    if (PyUnicode_IS_ASCII(unicode))
        return size == len && memcmp(data, str, (size_t)len) == 0;
    end = s + size;
    for (i = 0; i < len; i++) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i), d;
        int n;
        if (s >= end)
            return 0;
        if (*s < 0x80) {
            d = *s;
            n = 1;
        } else if ((*s & 0xE0) == 0xC0) {
            d = *s & 0x1F;
            n = 2;
        } else if ((*s & 0xF0) == 0xE0) {
            d = *s & 0x0F;
            n = 3;
        } else if ((*s & 0xF8) == 0xF0) {
            d = *s & 0x07;
            n = 4;
        } else {
            return 0;
        }
        if (end - s < n)
            return 0;
        for (int k = 1; k < n; k++) {
            if ((s[k] & 0xC0) != 0x80)
                return 0;
            d = (d << 6) | (s[k] & 0x3F);
        }
        /* reject overlong forms and surrogates: they never compare equal
           (the UTF-8 encoding of a str with surrogates does not exist) */
        if ((n == 2 && d < 0x80) || (n == 3 && d < 0x800) || (n == 4 && (d < 0x10000 || d > 0x10FFFF)) ||
            (d >= 0xD800 && d <= 0xDFFF))
            return 0;
        if (d != ch)
            return 0;
        s += n;
    }
    return s == end;
}

int PyUnicode_EqualToUTF8(PyObject *unicode, const char *str)
{
    return PyUnicode_EqualToUTF8AndSize(unicode, str, (Py_ssize_t)strlen(str));
}

int _PyUnicode_EqualToASCIIString(PyObject *unicode, const char *str)
{
    size_t len;
    if (!PyUnicode_IS_ASCII(unicode))
        return 0;
    len = (size_t)PyUnicode_GET_LENGTH(unicode);
    return strlen(str) == len && memcmp(PyUnicode_DATA(unicode), str, len) == 0;
}

int _PyUnicode_Equal(PyObject *str1, PyObject *str2)
{
    return qpd_eq(str1, str2);
}

int PyUnicode_Equal(PyObject *str1, PyObject *str2)
{
    if (!PyUnicode_Check(str1)) {
        PyErr_Format(PyExc_TypeError, "first argument must be str, not %s", Py_TYPE(str1)->tp_name);
        return -1;
    }
    if (!PyUnicode_Check(str2)) {
        PyErr_Format(PyExc_TypeError, "second argument must be str, not %s", Py_TYPE(str2)->tp_name);
        return -1;
    }
    return qpd_eq(str1, str2);
}

PyObject *PyUnicode_RichCompare(PyObject *left, PyObject *right, int op)
{
    int result;

    if (!PyUnicode_Check(left) || !PyUnicode_Check(right))
        Py_RETURN_NOTIMPLEMENTED;
    if (left == right) {
        switch (op) {
        case Py_EQ: case Py_LE: case Py_GE:
            Py_RETURN_TRUE;
        case Py_NE: case Py_LT: case Py_GT:
            Py_RETURN_FALSE;
        default:
            PyErr_BadArgument();
            return NULL;
        }
    }
    if (op == Py_EQ || op == Py_NE) {
        result = qpd_eq(left, right);
        result ^= (op == Py_NE);
        return PyBool_FromLong(result);
    }
    result = qpd_compare(left, right);
    Py_RETURN_RICHCOMPARE(result, 0, op);
}

PyObject *PyUnicode_Format(PyObject *format, PyObject *args)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst f, a;
    if (format == NULL || args == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (qpd_ensure_unicode(format) < 0)
        return NULL;
    f = qpd_js(format);
    if (JS_IsException(f))
        return NULL;
    a = qp_capi_value(ctx, args);
    if (JS_IsException(a))
        return NULL;
    return qpd_from_js(qp_str_remainder(ctx, f, a));
}

int PyUnicode_Contains(PyObject *str, PyObject *substr)
{
    Py_ssize_t r;
    if (!PyUnicode_Check(substr)) {
        PyErr_Format(PyExc_TypeError, "'in <string>' requires string as left operand, not %.100s",
                     Py_TYPE(substr)->tp_name);
        return -1;
    }
    if (qpd_ensure_unicode(str) < 0)
        return -1;
    r = qp_sl_find(qp_capi.rt, qpd_view(str), qpd_view(substr), 0, PY_SSIZE_T_MAX, 1);
    if (r == QP_SL_NOMEM) {
        PyErr_NoMemory();
        return -1;
    }
    return r >= 0;
}

int PyUnicode_IsIdentifier(PyObject *self)
{
    Py_ssize_t i, len = PyUnicode_GET_LENGTH(self);
    int kind = PyUnicode_KIND(self);
    const void *data = PyUnicode_DATA(self);
    Py_UCS4 ch;

    if (len == 0)
        return 0;
    ch = PyUnicode_READ(kind, data, 0);
    if (!qp_unicode_is_xid_start(ch) && ch != 0x5F)
        return 0;
    for (i = 1; i < len; i++) {
        if (!qp_unicode_is_xid_continue(PyUnicode_READ(kind, data, i)))
            return 0;
    }
    return 1;
}

/* ========================================================================= */
/* _PyUnicodeWriter (Objects/unicode_writer.c) and PyUnicodeWriter            */
/* ========================================================================= */

#define QPD_OVERALLOCATE_FACTOR 4

/* PyUnicode_MAX_CHAR_VALUE of the canonical str whose max char is m. */
static Py_UCS4 qpd_canonical_maxchar(Py_UCS4 m)
{
    return m < 0x80 ? 0x7f : m < 0x100 ? 0xff : m < 0x10000 ? 0xffff : 0x10ffff;
}
static void qpd_writer_update(_PyUnicodeWriter *writer)
{
    writer->maxchar = PyUnicode_MAX_CHAR_VALUE(writer->buffer);
    writer->data = PyUnicode_DATA(writer->buffer);
    if (!writer->readonly) {
        writer->kind = PyUnicode_KIND(writer->buffer);
        writer->size = PyUnicode_GET_LENGTH(writer->buffer);
    } else {
        writer->kind = 0;
        writer->size = 0;
    }
}

void _PyUnicodeWriter_Init(_PyUnicodeWriter *writer)
{
    memset(writer, 0, sizeof(*writer));
    writer->min_char = 127;
}

/* resize + keep kind (mirror blocks are never reallocated in place) */
static PyObject *qpd_resize_compact(PyObject *buffer, Py_ssize_t newlen, Py_ssize_t used)
{
    PyObject *nb = PyUnicode_New(newlen, PyUnicode_MAX_CHAR_VALUE(buffer));
    if (!nb)
        return NULL;
    qpd_copy(PyUnicode_KIND(nb), PyUnicode_DATA(nb), 0, PyUnicode_KIND(buffer),
             PyUnicode_DATA(buffer), 0, used < newlen ? used : newlen);
    Py_DECREF(buffer);
    return nb;
}

int _PyUnicodeWriter_PrepareInternal(_PyUnicodeWriter *writer, Py_ssize_t length, Py_UCS4 maxchar)
{
    Py_ssize_t newlen;
    PyObject *newbuffer;

    if (length > PY_SSIZE_T_MAX - writer->pos) {
        PyErr_NoMemory();
        return -1;
    }
    newlen = writer->pos + length;
    maxchar = Py_MAX(maxchar, writer->min_char);

    if (writer->buffer == NULL) {
        if (writer->overallocate && newlen <= (PY_SSIZE_T_MAX - newlen / QPD_OVERALLOCATE_FACTOR))
            newlen += newlen / QPD_OVERALLOCATE_FACTOR;
        if (newlen < writer->min_length)
            newlen = writer->min_length;
        writer->buffer = PyUnicode_New(newlen, maxchar);
        if (writer->buffer == NULL)
            return -1;
    } else if (newlen > writer->size) {
        if (writer->overallocate && newlen <= (PY_SSIZE_T_MAX - newlen / QPD_OVERALLOCATE_FACTOR))
            newlen += newlen / QPD_OVERALLOCATE_FACTOR;
        if (newlen < writer->min_length)
            newlen = writer->min_length;
        if (maxchar > writer->maxchar || writer->readonly) {
            maxchar = Py_MAX(maxchar, writer->maxchar);
            newbuffer = PyUnicode_New(newlen, maxchar);
            if (newbuffer == NULL)
                return -1;
            qpd_copy(PyUnicode_KIND(newbuffer), PyUnicode_DATA(newbuffer), 0,
                     PyUnicode_KIND(writer->buffer), PyUnicode_DATA(writer->buffer), 0, writer->pos);
            Py_DECREF(writer->buffer);
            writer->readonly = 0;
        } else {
            newbuffer = qpd_resize_compact(writer->buffer, newlen, writer->pos);
            if (newbuffer == NULL) {
                writer->buffer = NULL;
                return -1;
            }
        }
        writer->buffer = newbuffer;
    } else if (maxchar > writer->maxchar) {
        newbuffer = PyUnicode_New(writer->size, maxchar);
        if (newbuffer == NULL)
            return -1;
        qpd_copy(PyUnicode_KIND(newbuffer), PyUnicode_DATA(newbuffer), 0,
                 PyUnicode_KIND(writer->buffer), PyUnicode_DATA(writer->buffer), 0, writer->pos);
        Py_SETREF(writer->buffer, newbuffer);
    }
    qpd_writer_update(writer);
    return 0;
}

static inline int qpd_writer_prepare(_PyUnicodeWriter *w, Py_ssize_t length, Py_UCS4 maxchar)
{
    if (maxchar <= w->maxchar && length <= w->size - w->pos)
        return 0;
    if (length == 0)
        return 0;
    return _PyUnicodeWriter_PrepareInternal(w, length, maxchar);
}

int _PyUnicodeWriter_PrepareKindInternal(_PyUnicodeWriter *writer, int kind)
{
    Py_UCS4 maxchar;
    switch (kind) {
    case PyUnicode_1BYTE_KIND: maxchar = 0xff; break;
    case PyUnicode_2BYTE_KIND: maxchar = 0xffff; break;
    case PyUnicode_4BYTE_KIND: maxchar = QPD_MAX_UNICODE; break;
    default:
        PyErr_SetString(PyExc_SystemError, "invalid kind");
        return -1;
    }
    return _PyUnicodeWriter_PrepareInternal(writer, 0, maxchar);
}

int _PyUnicodeWriter_WriteChar(_PyUnicodeWriter *writer, Py_UCS4 ch)
{
    if (qpd_writer_prepare(writer, 1, ch) < 0)
        return -1;
    PyUnicode_WRITE(writer->kind, writer->data, writer->pos, ch);
    writer->pos++;
    return 0;
}

/* Append data[0:len] of the given kind (exact max char maxchar). */
static int qpd_writer_write_kind(_PyUnicodeWriter *writer, int kind, const void *data,
                                 Py_ssize_t len, Py_UCS4 maxchar)
{
    if (len == 0)
        return 0;
    if (qpd_writer_prepare(writer, len, maxchar) < 0)
        return -1;
    qpd_copy(writer->kind, writer->data, writer->pos, kind, data, 0, len);
    writer->pos += len;
    return 0;
}

int _PyUnicodeWriter_WriteStr(_PyUnicodeWriter *writer, PyObject *str)
{
    Py_UCS4 maxchar;
    Py_ssize_t len = PyUnicode_GET_LENGTH(str);

    if (len == 0)
        return 0;
    maxchar = PyUnicode_MAX_CHAR_VALUE(str);
    if (maxchar > writer->maxchar || len > writer->size - writer->pos) {
        if (writer->buffer == NULL && !writer->overallocate) {
            writer->readonly = 1;
            writer->buffer = Py_NewRef(str);
            qpd_writer_update(writer);
            writer->pos += len;
            return 0;
        }
        if (_PyUnicodeWriter_PrepareInternal(writer, len, maxchar) == -1)
            return -1;
    }
    qpd_copy(writer->kind, writer->data, writer->pos, PyUnicode_KIND(str), PyUnicode_DATA(str), 0, len);
    writer->pos += len;
    return 0;
}

int _PyUnicodeWriter_WriteSubstring(_PyUnicodeWriter *writer, PyObject *str, Py_ssize_t start,
                                    Py_ssize_t end)
{
    Py_ssize_t len;
    Py_UCS4 maxchar;
    int kind;
    const char *data;

    if (start == 0 && end == PyUnicode_GET_LENGTH(str))
        return _PyUnicodeWriter_WriteStr(writer, str);
    len = end - start;
    if (len <= 0)
        return 0;
    kind = PyUnicode_KIND(str);
    data = (const char *)PyUnicode_DATA(str) + start * kind;
    if (PyUnicode_MAX_CHAR_VALUE(str) > writer->maxchar)
        maxchar = qpd_max_char(kind, data, len);
    else
        maxchar = writer->maxchar;
    return qpd_writer_write_kind(writer, kind, data, len, maxchar);
}

int _PyUnicodeWriter_WriteASCIIString(_PyUnicodeWriter *writer, const char *ascii, Py_ssize_t len)
{
    if (len == -1)
        len = (Py_ssize_t)strlen(ascii);
    if (len == 0)
        return 0;
    if (writer->buffer == NULL && !writer->overallocate) {
        PyObject *str = qpd_from_kind(PyUnicode_1BYTE_KIND, ascii, len);
        if (str == NULL)
            return -1;
        writer->readonly = 1;
        writer->buffer = str;
        qpd_writer_update(writer);
        writer->pos += len;
        return 0;
    }
    return qpd_writer_write_kind(writer, PyUnicode_1BYTE_KIND, ascii, len, 127);
}

int _PyUnicodeWriter_WriteLatin1String(_PyUnicodeWriter *writer, const char *str, Py_ssize_t len)
{
    if (len == 0)
        return 0;
    return qpd_writer_write_kind(writer, PyUnicode_1BYTE_KIND, str, len,
                                 qpd_max_char(PyUnicode_1BYTE_KIND, str, len));
}

PyObject *_PyUnicodeWriter_Finish(_PyUnicodeWriter *writer)
{
    PyObject *str;

    if (writer->pos == 0) {
        Py_CLEAR(writer->buffer);
        return qpd_empty();
    }
    str = writer->buffer;
    writer->buffer = NULL;
    if (writer->readonly)
        return str;
    if (PyUnicode_GET_LENGTH(str) != writer->pos ||
        PyUnicode_MAX_CHAR_VALUE(str) !=
            qpd_canonical_maxchar(qpd_max_char(PyUnicode_KIND(str), PyUnicode_DATA(str), writer->pos))) {
        /* shrink, and narrow to the canonical kind (_PyUnicode_Result) */
        PyObject *r = qpd_from_kind(PyUnicode_KIND(str), PyUnicode_DATA(str), writer->pos);
        Py_DECREF(str);
        return r;
    }
    return str;
}

void _PyUnicodeWriter_Dealloc(_PyUnicodeWriter *writer)
{
    Py_CLEAR(writer->buffer);
}

/* Append the code points of a JS str (consumed). */
static int qpd_writer_write_js(_PyUnicodeWriter *writer, JSValue s)
{
    JSContext *ctx = qp_capi.ctx;
    qp_strview v;
    int r;
    if (JS_IsException(s))
        return -1;
    if (qp_str_view_acquire(ctx, s, &v) < 0) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    r = qpd_writer_write_kind(writer, v.kind, v.data, v.len, qpd_max_char(v.kind, v.data, v.len));
    qp_str_view_release(ctx, &v);
    JS_FreeValue(ctx, s);
    return r;
}

PyUnicodeWriter *PyUnicodeWriter_Create(Py_ssize_t length)
{
    _PyUnicodeWriter *writer;
    if (length < 0) {
        PyErr_SetString(PyExc_ValueError, "length must be positive");
        return NULL;
    }
    writer = PyMem_Malloc(sizeof(_PyUnicodeWriter));
    if (writer == NULL)
        return (PyUnicodeWriter *)PyErr_NoMemory();
    _PyUnicodeWriter_Init(writer);
    if (qpd_writer_prepare(writer, length, 127) < 0) {
        PyUnicodeWriter_Discard((PyUnicodeWriter *)writer);
        return NULL;
    }
    writer->overallocate = 1;
    return (PyUnicodeWriter *)writer;
}

void PyUnicodeWriter_Discard(PyUnicodeWriter *writer)
{
    if (writer == NULL)
        return;
    _PyUnicodeWriter_Dealloc((_PyUnicodeWriter *)writer);
    PyMem_Free(writer);
}

PyObject *PyUnicodeWriter_Finish(PyUnicodeWriter *writer)
{
    PyObject *str = _PyUnicodeWriter_Finish((_PyUnicodeWriter *)writer);
    PyMem_Free(writer);
    return str;
}

int PyUnicodeWriter_WriteChar(PyUnicodeWriter *writer, Py_UCS4 ch)
{
    if (ch > QPD_MAX_UNICODE) {
        PyErr_SetString(PyExc_ValueError, "character must be in range(0x110000)");
        return -1;
    }
    return _PyUnicodeWriter_WriteChar((_PyUnicodeWriter *)writer, ch);
}

int PyUnicodeWriter_WriteStr(PyUnicodeWriter *writer, PyObject *obj)
{
    PyObject *str;
    int res;
    if (Py_TYPE(obj) == &PyUnicode_Type)
        return _PyUnicodeWriter_WriteStr((_PyUnicodeWriter *)writer, obj);
    str = PyObject_Str(obj);
    if (str == NULL)
        return -1;
    res = _PyUnicodeWriter_WriteStr((_PyUnicodeWriter *)writer, str);
    Py_DECREF(str);
    return res;
}

int PyUnicodeWriter_WriteRepr(PyUnicodeWriter *writer, PyObject *obj)
{
    PyObject *repr;
    int res;
    if (obj == NULL)
        return _PyUnicodeWriter_WriteASCIIString((_PyUnicodeWriter *)writer, "<NULL>", 6);
    repr = PyObject_Repr(obj);
    if (repr == NULL)
        return -1;
    res = _PyUnicodeWriter_WriteStr((_PyUnicodeWriter *)writer, repr);
    Py_DECREF(repr);
    return res;
}

int PyUnicodeWriter_WriteSubstring(PyUnicodeWriter *writer, PyObject *str, Py_ssize_t start,
                                   Py_ssize_t end)
{
    if (!PyUnicode_Check(str)) {
        PyErr_Format(PyExc_TypeError, "expect str, not %.100s", Py_TYPE(str)->tp_name);
        return -1;
    }
    if (start < 0 || start > end) {
        PyErr_Format(PyExc_ValueError, "invalid start argument");
        return -1;
    }
    if (end > PyUnicode_GET_LENGTH(str)) {
        PyErr_Format(PyExc_ValueError, "invalid end argument");
        return -1;
    }
    return _PyUnicodeWriter_WriteSubstring((_PyUnicodeWriter *)writer, str, start, end);
}

int PyUnicodeWriter_WriteASCII(PyUnicodeWriter *writer, const char *str, Py_ssize_t size)
{
    return _PyUnicodeWriter_WriteASCIIString((_PyUnicodeWriter *)writer, str, size);
}

int PyUnicodeWriter_WriteUTF8(PyUnicodeWriter *writer, const char *str, Py_ssize_t size)
{
    _PyUnicodeWriter *w = (_PyUnicodeWriter *)writer;
    PyObject *u;
    int r;
    if (size < 0)
        size = (Py_ssize_t)strlen(str);
    u = qpd_decode_utf8(str, size, NULL);
    if (!u)
        return -1;
    r = qpd_writer_write_kind(w, PyUnicode_KIND(u), PyUnicode_DATA(u), PyUnicode_GET_LENGTH(u),
                              PyUnicode_MAX_CHAR_VALUE(u));
    Py_DECREF(u);
    return r;
}

int PyUnicodeWriter_DecodeUTF8Stateful(PyUnicodeWriter *writer, const char *string, Py_ssize_t length,
                                       const char *errors, Py_ssize_t *consumed)
{
    _PyUnicodeWriter *w = (_PyUnicodeWriter *)writer;
    Py_ssize_t old_pos = w->pos;
    int r;
    if (length < 0)
        length = (Py_ssize_t)strlen(string);
    if (qpd_is_strict(errors)) {
        PyObject *u = qpd_decode_utf8(string, length, consumed);
        if (!u) {
            r = -1;
        } else {
            r = qpd_writer_write_kind(w, PyUnicode_KIND(u), PyUnicode_DATA(u),
                                      PyUnicode_GET_LENGTH(u), PyUnicode_MAX_CHAR_VALUE(u));
            Py_DECREF(u);
        }
    } else {
        r = qpd_writer_write_js(w, qp_cd_decode_utf8(qp_capi.ctx, string, length, errors, consumed));
    }
    if (r < 0) {
        w->pos = old_pos;
        if (consumed)
            *consumed = 0;
    }
    return r;
}

int PyUnicodeWriter_WriteWideChar(PyUnicodeWriter *writer, const wchar_t *str, Py_ssize_t size)
{
    PyObject *u;
    int r;
    if (size < 0)
        size = (Py_ssize_t)wcslen(str);
    if (size == 0)
        return 0;
    u = PyUnicode_FromWideChar(str, size);
    if (!u)
        return -1;
    r = _PyUnicodeWriter_WriteStr((_PyUnicodeWriter *)writer, u);
    Py_DECREF(u);
    return r;
}

int PyUnicodeWriter_WriteUCS4(PyUnicodeWriter *writer, const Py_UCS4 *str, Py_ssize_t size)
{
    Py_UCS4 maxchar;
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return -1;
    }
    if (size == 0)
        return 0;
    maxchar = qpd_max_char(PyUnicode_4BYTE_KIND, str, size);
    if (maxchar > QPD_MAX_UNICODE) {
        PyErr_SetString(PyExc_ValueError, "character must be in range(0x110000)");
        return -1;
    }
    return qpd_writer_write_kind((_PyUnicodeWriter *)writer, PyUnicode_4BYTE_KIND, str, size, maxchar);
}

int PyUnicodeWriter_Format(PyUnicodeWriter *writer, const char *format, ...)
{
    va_list ap;
    JSValue r;
    va_start(ap, format);
    r = qpd_from_format_js(qp_capi.ctx, format, &ap);
    va_end(ap);
    return qpd_writer_write_js((_PyUnicodeWriter *)writer, r);
}

/* ========================================================================= */
/* Character database exports (unicodetype.h, P1 renames)                    */
/* ========================================================================= */

int _PyUnicode_IsLowercase(Py_UCS4 ch) { return qp_unicode_is_lowercase(ch); }
int _PyUnicode_IsUppercase(Py_UCS4 ch) { return qp_unicode_is_uppercase(ch); }
int _PyUnicode_IsTitlecase(Py_UCS4 ch) { return qp_unicode_is_titlecase(ch); }
int _PyUnicode_IsWhitespace(const Py_UCS4 ch) { return qp_unicode_is_whitespace(ch); }
int _PyUnicode_IsLinebreak(const Py_UCS4 ch) { return qp_unicode_is_linebreak(ch); }
Py_UCS4 _PyUnicode_ToLowercase(Py_UCS4 ch) { return qp_unicode_to_lowercase(ch); }
Py_UCS4 _PyUnicode_ToUppercase(Py_UCS4 ch) { return qp_unicode_to_uppercase(ch); }
Py_UCS4 _PyUnicode_ToTitlecase(Py_UCS4 ch) { return qp_unicode_to_titlecase(ch); }
int _PyUnicode_ToDecimalDigit(Py_UCS4 ch) { return qp_unicode_to_decimal_digit(ch); }
int _PyUnicode_ToDigit(Py_UCS4 ch) { return qp_unicode_to_digit(ch); }
double _PyUnicode_ToNumeric(Py_UCS4 ch) { return qp_unicode_to_numeric(ch); }
int _PyUnicode_IsDecimalDigit(Py_UCS4 ch) { return qp_unicode_is_decimal_digit(ch); }
int _PyUnicode_IsDigit(Py_UCS4 ch) { return qp_unicode_is_digit(ch); }
int _PyUnicode_IsNumeric(Py_UCS4 ch) { return qp_unicode_is_numeric(ch); }
int _PyUnicode_IsPrintable(Py_UCS4 ch) { return qp_unicode_is_printable(ch); }
int _PyUnicode_IsAlpha(Py_UCS4 ch) { return qp_unicode_is_alpha(ch); }
/* pycore_unicodectype.h */
int _PyUnicode_IsXidStart(Py_UCS4 ch) { return qp_unicode_is_xid_start(ch); }
int _PyUnicode_IsXidContinue(Py_UCS4 ch) { return qp_unicode_is_xid_continue(ch); }

/* cpython/pyctype.h tables: CPython 3.15 Python/pyctype.c (PSF-2.0). */
const unsigned int _Py_ctype_table[256] = {
    0, /* 0x0 '\x00' */
    0, /* 0x1 '\x01' */
    0, /* 0x2 '\x02' */
    0, /* 0x3 '\x03' */
    0, /* 0x4 '\x04' */
    0, /* 0x5 '\x05' */
    0, /* 0x6 '\x06' */
    0, /* 0x7 '\x07' */
    0, /* 0x8 '\x08' */
    PY_CTF_SPACE, /* 0x9 '\t' */
    PY_CTF_SPACE, /* 0xa '\n' */
    PY_CTF_SPACE, /* 0xb '\v' */
    PY_CTF_SPACE, /* 0xc '\f' */
    PY_CTF_SPACE, /* 0xd '\r' */
    0, /* 0xe '\x0e' */
    0, /* 0xf '\x0f' */
    0, /* 0x10 '\x10' */
    0, /* 0x11 '\x11' */
    0, /* 0x12 '\x12' */
    0, /* 0x13 '\x13' */
    0, /* 0x14 '\x14' */
    0, /* 0x15 '\x15' */
    0, /* 0x16 '\x16' */
    0, /* 0x17 '\x17' */
    0, /* 0x18 '\x18' */
    0, /* 0x19 '\x19' */
    0, /* 0x1a '\x1a' */
    0, /* 0x1b '\x1b' */
    0, /* 0x1c '\x1c' */
    0, /* 0x1d '\x1d' */
    0, /* 0x1e '\x1e' */
    0, /* 0x1f '\x1f' */
    PY_CTF_SPACE, /* 0x20 ' ' */
    0, /* 0x21 '!' */
    0, /* 0x22 '"' */
    0, /* 0x23 '#' */
    0, /* 0x24 '$' */
    0, /* 0x25 '%' */
    0, /* 0x26 '&' */
    0, /* 0x27 "'" */
    0, /* 0x28 '(' */
    0, /* 0x29 ')' */
    0, /* 0x2a '*' */
    0, /* 0x2b '+' */
    0, /* 0x2c ',' */
    0, /* 0x2d '-' */
    0, /* 0x2e '.' */
    0, /* 0x2f '/' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x30 '0' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x31 '1' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x32 '2' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x33 '3' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x34 '4' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x35 '5' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x36 '6' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x37 '7' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x38 '8' */
    PY_CTF_DIGIT|PY_CTF_XDIGIT, /* 0x39 '9' */
    0, /* 0x3a ':' */
    0, /* 0x3b ';' */
    0, /* 0x3c '<' */
    0, /* 0x3d '=' */
    0, /* 0x3e '>' */
    0, /* 0x3f '?' */
    0, /* 0x40 '@' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x41 'A' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x42 'B' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x43 'C' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x44 'D' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x45 'E' */
    PY_CTF_UPPER|PY_CTF_XDIGIT, /* 0x46 'F' */
    PY_CTF_UPPER, /* 0x47 'G' */
    PY_CTF_UPPER, /* 0x48 'H' */
    PY_CTF_UPPER, /* 0x49 'I' */
    PY_CTF_UPPER, /* 0x4a 'J' */
    PY_CTF_UPPER, /* 0x4b 'K' */
    PY_CTF_UPPER, /* 0x4c 'L' */
    PY_CTF_UPPER, /* 0x4d 'M' */
    PY_CTF_UPPER, /* 0x4e 'N' */
    PY_CTF_UPPER, /* 0x4f 'O' */
    PY_CTF_UPPER, /* 0x50 'P' */
    PY_CTF_UPPER, /* 0x51 'Q' */
    PY_CTF_UPPER, /* 0x52 'R' */
    PY_CTF_UPPER, /* 0x53 'S' */
    PY_CTF_UPPER, /* 0x54 'T' */
    PY_CTF_UPPER, /* 0x55 'U' */
    PY_CTF_UPPER, /* 0x56 'V' */
    PY_CTF_UPPER, /* 0x57 'W' */
    PY_CTF_UPPER, /* 0x58 'X' */
    PY_CTF_UPPER, /* 0x59 'Y' */
    PY_CTF_UPPER, /* 0x5a 'Z' */
    0, /* 0x5b '[' */
    0, /* 0x5c '\\' */
    0, /* 0x5d ']' */
    0, /* 0x5e '^' */
    0, /* 0x5f '_' */
    0, /* 0x60 '`' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x61 'a' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x62 'b' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x63 'c' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x64 'd' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x65 'e' */
    PY_CTF_LOWER|PY_CTF_XDIGIT, /* 0x66 'f' */
    PY_CTF_LOWER, /* 0x67 'g' */
    PY_CTF_LOWER, /* 0x68 'h' */
    PY_CTF_LOWER, /* 0x69 'i' */
    PY_CTF_LOWER, /* 0x6a 'j' */
    PY_CTF_LOWER, /* 0x6b 'k' */
    PY_CTF_LOWER, /* 0x6c 'l' */
    PY_CTF_LOWER, /* 0x6d 'm' */
    PY_CTF_LOWER, /* 0x6e 'n' */
    PY_CTF_LOWER, /* 0x6f 'o' */
    PY_CTF_LOWER, /* 0x70 'p' */
    PY_CTF_LOWER, /* 0x71 'q' */
    PY_CTF_LOWER, /* 0x72 'r' */
    PY_CTF_LOWER, /* 0x73 's' */
    PY_CTF_LOWER, /* 0x74 't' */
    PY_CTF_LOWER, /* 0x75 'u' */
    PY_CTF_LOWER, /* 0x76 'v' */
    PY_CTF_LOWER, /* 0x77 'w' */
    PY_CTF_LOWER, /* 0x78 'x' */
    PY_CTF_LOWER, /* 0x79 'y' */
    PY_CTF_LOWER, /* 0x7a 'z' */
    0, /* 0x7b '{' */
    0, /* 0x7c '|' */
    0, /* 0x7d '}' */
    0, /* 0x7e '~' */
    0, /* 0x7f '\x7f' */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};


const unsigned char _Py_ctype_tolower[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

const unsigned char _Py_ctype_toupper[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

/* Py_UNICODE_ISSPACE fast path: CPython 3.15 Objects/unicodeobject.c (PSF-2.0). */
const unsigned char _Py_ascii_whitespace[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
/*     case 0x0009: * CHARACTER TABULATION */
/*     case 0x000A: * LINE FEED */
/*     case 0x000B: * LINE TABULATION */
/*     case 0x000C: * FORM FEED */
/*     case 0x000D: * CARRIAGE RETURN */
    0, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
/*     case 0x001C: * FILE SEPARATOR */
/*     case 0x001D: * GROUP SEPARATOR */
/*     case 0x001E: * RECORD SEPARATOR */
/*     case 0x001F: * UNIT SEPARATOR */
    0, 0, 0, 0, 1, 1, 1, 1,
/*     case 0x0020: * SPACE */
    1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,

    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* ========================================================================= */
/* Name mangling exports (pycore_symtable.h over python/symtable.c, P1)       */
/* ========================================================================= */

/* New atom of an exact str (JS_ATOM_NULL + exception). */
static JSAtom qpd_atom(PyObject *u)
{
    JSValueConst v = qpd_js(u);
    if (JS_IsException(v))
        return JS_ATOM_NULL;
    return qp_str_to_atom(qp_capi.ctx, v);
}

PyObject *_Py_Mangle(PyObject *privateobj, PyObject *ident)
{
    JSContext *ctx = qp_capi.ctx;
    JSAtom pa, ia, ra;
    qp_error err;

    if (privateobj == NULL || !PyUnicode_Check(privateobj) || !PyUnicode_Check(ident) ||
        PyUnicode_GET_LENGTH(ident) < 2 || PyUnicode_READ_CHAR(ident, 0) != '_' ||
        PyUnicode_READ_CHAR(ident, 1) != '_')
        return Py_NewRef(ident);
    pa = qpd_atom(privateobj);
    if (pa == JS_ATOM_NULL)
        return NULL;
    ia = qpd_atom(ident);
    if (ia == JS_ATOM_NULL) {
        JS_FreeAtom(ctx, pa);
        return NULL;
    }
    memset(&err, 0, sizeof(err));
    ra = qp_symtable_mangle(pa, ia, ctx, &err);
    JS_FreeAtom(ctx, pa);
    JS_FreeAtom(ctx, ia);
    if (ra == JS_ATOM_NULL) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, NULL);
        else if (!JS_HasException(ctx))
            qp_raise_memory_error(ctx);
        qp_error_clear(&err);
        return NULL;
    }
    {
        JSValue s = JS_AtomToString(ctx, ra);
        JS_FreeAtom(ctx, ra);
        return qp_capi_steal(ctx, s);
    }
}

int _Py_IsPrivateName(PyObject *ident)
{
    JSContext *ctx = qp_capi.ctx;
    JSAtom ia;
    int r;
    if (!PyUnicode_Check(ident))
        return 0;
    ia = qpd_atom(ident);
    if (ia == JS_ATOM_NULL) {
        PyErr_Clear();
        return 0;
    }
    r = qp_symtable_is_private_name(ctx, ia);
    JS_FreeAtom(ctx, ia);
    return r;
}

/* ========================================================================= */
/* codecs.h: PyCodec_* over the _codecs registry (python/mod_codecs.c)        */
/* ========================================================================= */

int PyCodec_Register(PyObject *search_function)
{
    JSValueConst v = qp_capi_value(qp_capi.ctx, search_function);
    if (JS_IsException(v))
        return -1;
    return qp_codec_register(qp_capi.ctx, v);
}

int PyCodec_Unregister(PyObject *search_function)
{
    JSValueConst v = qp_capi_value(qp_capi.ctx, search_function);
    if (JS_IsException(v))
        return -1;
    return qp_codec_unregister(qp_capi.ctx, v);
}

int PyCodec_KnownEncoding(const char *encoding)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue codecs = qp_codec_lookup(ctx, encoding);
    if (JS_IsException(codecs)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_FreeValue(ctx, codecs);
    return 1;
}

static PyObject *qpd_codec_coder(PyObject *object, const char *encoding, const char *errors,
                                 int decode)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst v = qp_capi_value(ctx, object);
    if (JS_IsException(v))
        return NULL;
    if (decode)
        return qpd_from_js(qp_codec_decode(ctx, v, encoding, errors));
    return qpd_from_js(qp_codec_encode(ctx, v, encoding, errors));
}

PyObject *PyCodec_Encode(PyObject *object, const char *encoding, const char *errors)
{
    return qpd_codec_coder(object, encoding, errors, 0);
}

PyObject *PyCodec_Decode(PyObject *object, const char *encoding, const char *errors)
{
    return qpd_codec_coder(object, encoding, errors, 1);
}

PyObject *PyCodec_Encoder(const char *encoding)
{
    return qpd_from_js(qp_cd_codec_getitem(qp_capi.ctx, encoding, 0));
}

PyObject *PyCodec_Decoder(const char *encoding)
{
    return qpd_from_js(qp_cd_codec_getitem(qp_capi.ctx, encoding, 1));
}

/* codec_getincrementalcodec */
static PyObject *qpd_codec_incremental(const char *encoding, const char *errors, const char *attrname)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue info, inccodec, r, e;
    JSAtom name;

    info = qp_codec_lookup(ctx, encoding);
    if (JS_IsException(info))
        return NULL;
    name = JS_NewAtom(ctx, attrname);
    if (name == JS_ATOM_NULL) {
        JS_FreeValue(ctx, info);
        return NULL;
    }
    inccodec = qp_getattr(ctx, info, name);
    JS_FreeAtom(ctx, name);
    JS_FreeValue(ctx, info);
    if (JS_IsException(inccodec))
        return NULL;
    if (errors) {
        e = qp_str_from_cstr(ctx, errors);
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, inccodec);
            return NULL;
        }
        r = qp_call1(ctx, inccodec, e);
        JS_FreeValue(ctx, e);
    } else {
        r = qp_call0(ctx, inccodec);
    }
    JS_FreeValue(ctx, inccodec);
    return qpd_from_js(r);
}

PyObject *PyCodec_IncrementalEncoder(const char *encoding, const char *errors)
{
    return qpd_codec_incremental(encoding, errors, "incrementalencoder");
}

PyObject *PyCodec_IncrementalDecoder(const char *encoding, const char *errors)
{
    return qpd_codec_incremental(encoding, errors, "incrementaldecoder");
}

/* codec_getstreamcodec: CodecInfo item 2 (reader) / 3 (writer). */
static PyObject *qpd_codec_stream(const char *encoding, PyObject *stream, const char *errors, int index)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst sv;
    JSValue cls, r, e;

    sv = qp_capi_value(ctx, stream);
    if (JS_IsException(sv))
        return NULL;
    cls = qp_cd_codec_getitem(ctx, encoding, index);
    if (JS_IsException(cls))
        return NULL;
    if (errors) {
        e = qp_str_from_cstr(ctx, errors);
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, cls);
            return NULL;
        }
        r = qp_call2(ctx, cls, sv, e);
        JS_FreeValue(ctx, e);
    } else {
        r = qp_call1(ctx, cls, sv);
    }
    JS_FreeValue(ctx, cls);
    return qpd_from_js(r);
}

PyObject *PyCodec_StreamReader(const char *encoding, PyObject *stream, const char *errors)
{
    return qpd_codec_stream(encoding, stream, errors, 2);
}

PyObject *PyCodec_StreamWriter(const char *encoding, PyObject *stream, const char *errors)
{
    return qpd_codec_stream(encoding, stream, errors, 3);
}

int PyCodec_RegisterError(const char *name, PyObject *error)
{
    JSValueConst v = qp_capi_value(qp_capi.ctx, error);
    if (JS_IsException(v))
        return -1;
    return qp_codec_register_error(qp_capi.ctx, name, v);
}

PyObject *PyCodec_LookupError(const char *name)
{
    return qpd_from_js(qp_codec_lookup_error(qp_capi.ctx, name));
}

/* The builtin handlers (Python/codecs.c), called through the registry
   entries the _codecs module registers for them. */
static PyObject *qpd_codec_builtin_handler(const char *name, PyObject *exc)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst ev;
    JSValue h, r;

    ev = qp_capi_value(ctx, exc);
    if (JS_IsException(ev))
        return NULL;
    h = qp_codec_lookup_error(ctx, name);
    if (JS_IsException(h))
        return NULL;
    r = qp_call1(ctx, h, ev);
    JS_FreeValue(ctx, h);
    return qpd_from_js(r);
}

PyObject *PyCodec_StrictErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("strict", exc);
}

PyObject *PyCodec_IgnoreErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("ignore", exc);
}

PyObject *PyCodec_ReplaceErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("replace", exc);
}

PyObject *PyCodec_XMLCharRefReplaceErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("xmlcharrefreplace", exc);
}

PyObject *PyCodec_BackslashReplaceErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("backslashreplace", exc);
}

PyObject *PyCodec_NameReplaceErrors(PyObject *exc)
{
    return qpd_codec_builtin_handler("namereplace", exc);
}

/* ========================================================================= */
/* Unit D init                                                                */
/* ========================================================================= */

/* (D, capi.h) kind ops STR/BYTES/BYTEARRAY/MEMORYVIEW and the C-API-only
   types of unit D.  The codec registry is the per-context one of _codecs,
   so qp_capi.codecs stays NULL.  Idempotent. */
static int qp_capi_init_str(JSContext *ctx)
{
    QPCapiKindOps *k = &qp_capi.kind_ops[QP_CAPI_K_STR];
    k->name = "str";
    k->size = qpd_str_size;
    k->init = qpd_str_init;
    k->to_js = qpd_str_to_js;
    k->refresh = NULL;
    k->traverse = NULL;
    k->clear = qpd_str_clear;
    return qp_capi_init_bytes(ctx);
}

_Py_COMP_DIAG_POP
