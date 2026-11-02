/*
 * dquickts Python: the `str` type over QuickJS strings.
 *
 * Derived from CPython Objects/unicodeobject.c, Objects/stringlib/codecs.h,
 * Objects/stringlib/repr.h and Python/codecs.c (PSF-2.0), v3.15.2.
 *
 * Representation (object.h, D3): a Python str is a JSString (8-bit Latin-1
 * or 16-bit UTF-16 code units) or a JS string rope.  Python semantics are
 * per CODE POINT: in a 16-bit string a high surrogate followed by a low
 * surrogate is one code point, any other surrogate is a lone code point of
 * its own.  Instances of str subclasses are JS_CLASS_PY_BOXED objects whose
 * u.object_data is the exact str value.
 *
 * Code-point views (qp_strview, stringlib.h): 8-bit strings are viewed as
 * kind 1 and 16-bit strings without surrogate pairs as kind 2, directly over
 * the JSString data (ropes are flattened in place first).  16-bit strings
 * WITH surrogate pairs get a kind 4 view over a reference-counted UCS4 side
 * buffer (QPUcs4Buf).  Side buffers, code-point lengths and hashes of long
 * or interned strings are memoized in QPState.str_cache, a direct-mapped
 * table keyed by the flat JSString; each entry holds a counted reference to
 * its string, so a key pointer can never be reused for another string while
 * the entry exists.
 *
 * Inherent limitation of the UTF-16 representation: a str made of two lone
 * surrogates that happen to form a valid pair (e.g. "\ud83d\ude00", or the
 * concatenation "\ud83d" + "\ude00") is indistinguishable from the single
 * astral code point, and behaves as such.
 */

/* Defined in other units (not in object.h): python/iterobject.c and
   python/unicode_format.c / templateobject.c, python/unicodename.c. */
static JSValue qp_str_iter_new(JSContext *ctx, JSValueConst str);
static JSValue qp_str_format_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_format_map_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_format_dunder(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic);
static JSValue qp_str_remainder(JSContext *ctx, JSValueConst a, JSValueConst b);
static int qp_init_templatelib(JSContext *ctx);
static int qp_unicode_get_name(uint32_t code, char *buffer, int buflen);
/* python/mod_codecs.c: codec registry (_PyCodec_EncodeText /
   _PyCodec_DecodeText with PyUnicode_AsEncodedString / PyUnicode_Decode
   result checks), used for codecs and error handlers not built in here. */
static JSValue qp_codec_encode_text(JSContext *ctx, JSValueConst s, const char *encoding,
                                    const char *errors);
static JSValue qp_codec_decode_text(JSContext *ctx, JSValueConst obj, const char *encoding,
                                    const char *errors);
static int qp_codec_check_encoding_errors(JSContext *ctx, const char *encoding, const char *errors);
static void qp_str_state_free(JSContext *ctx);

#define QP_MAX_UNICODE 0x10FFFF
#define NAME_MAXLEN_QP 256   /* unicodename_db.h NAME_MAXLEN */
#define QP_IS_HI_SURR(c) (((c) & 0xFC00) == 0xD800)
#define QP_IS_LO_SURR(c) (((c) & 0xFC00) == 0xDC00)
#define QP_IS_SURR(c)    (((c) & 0xF800) == 0xD800)
#define QP_FROM_SURR(hi, lo) ((((Py_UCS4)(hi) - 0xD800) << 10) + ((Py_UCS4)(lo) - 0xDC00) + 0x10000)

static const char qp_hexdigits[] = "0123456789abcdef";

/* ========================================================================= */
/* Cache of code-point information (QPState.str_cache)                       */
/* ========================================================================= */

/* Reference-counted UCS4 copy of a 16-bit string with surrogate pairs. */
typedef struct QPUcs4Buf {
    int ref_count;
    Py_ssize_t len;
    uint32_t data[];
} QPUcs4Buf;

#define QP_STR_CACHE_BITS 9
#define QP_STR_CACHE_SIZE (1 << QP_STR_CACHE_BITS)
/* wide strings shorter than this are scanned directly (no cache entry) */
#define QP_STR_CACHE_MIN_LEN 32
/* non-atom strings longer than this never get their hash cached, so the
   cache does not keep huge strings alive just for a hash */
#define QP_STR_HASH_CACHE_MAX_LEN 4096

typedef struct QPStrCacheEntry {
    JSString *str;          /* counted reference; NULL = empty slot */
    Py_hash_t hash;         /* -1 = not computed */
    Py_ssize_t cplen;       /* code points, -1 = not computed */
    QPUcs4Buf *ucs4;        /* side buffer (one reference) if the string has pairs */
} QPStrCacheEntry;

struct QPStrCache {
    QPStrCacheEntry e[QP_STR_CACHE_SIZE];
};

static void qp_ucs4buf_unref(JSRuntime *rt, QPUcs4Buf *b)
{
    if (b && --b->ref_count == 0)
        js_free_rt(rt, b);
}

static void qp_str_cache_clear_entry(JSRuntime *rt, QPStrCacheEntry *e)
{
    if (e->str) {
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, e->str));
        e->str = NULL;
    }
    qp_ucs4buf_unref(rt, e->ucs4);
    e->ucs4 = NULL;
    e->hash = -1;
    e->cplen = -1;
}

/* Release QPState.str_cache (called by qp_state_free, unit a). */
static void qp_str_state_free(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    struct QPStrCache *c;
    int i;

    if (!st || !st->str_cache)
        return;
    c = st->str_cache;
    for (i = 0; i < QP_STR_CACHE_SIZE; i++)
        qp_str_cache_clear_entry(ctx->rt, &c->e[i]);
    js_free(ctx, c);
    st->str_cache = NULL;
}

/* Entry for the flat string p (created, evicting the previous occupant, when
   `create`); NULL when absent and !create or when there is no cache. */
static QPStrCacheEntry *qp_str_cache_get(JSContext *ctx, JSString *p, BOOL create)
{
    QPState *st = QP_STATE(ctx);
    struct QPStrCache *c;
    QPStrCacheEntry *e;
    uintptr_t h;

    if (!st || !(c = st->str_cache))
        return NULL;
    h = (uintptr_t)p;
    h = (h >> 4) ^ (h >> (4 + QP_STR_CACHE_BITS));
    e = &c->e[h & (QP_STR_CACHE_SIZE - 1)];
    if (e->str == p)
        return e;
    if (!create)
        return NULL;
    qp_str_cache_clear_entry(ctx->rt, e);
    js_rc(p)->ref_count++;   /* counted reference (JS_DupValue on the string) */
    e->str = p;
    return e;
}

/* ========================================================================= */
/* Low-level helpers                                                          */
/* ========================================================================= */

/* The exact str inside v: v itself, or the value of a boxed str subclass
   instance (borrowed). */
static inline JSValueConst qp_str_unbox(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (p->class_id == JS_CLASS_PY_BOXED)
            return p->u.object_data;
    }
    return v;
}

static BOOL qp_str_check_exact(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_STRING || tag == JS_TAG_STRING_ROPE;
}

static BOOL qp_str_check(JSContext *ctx, JSValueConst v)
{
    return qp_str_check_exact(qp_str_unbox(v));
}

static JSValue qp_str_value(JSContext *ctx, JSValueConst v)
{
    return JS_DupValue(ctx, qp_str_unbox(v));
}

/* Length in UTF-16/Latin-1 code units of an exact str (string or rope). */
static inline Py_ssize_t qp_str_units(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE)
        return JS_VALUE_GET_STRING_ROPE(v)->len;
    return JS_VALUE_GET_STRING(v)->len;
}

/* Flat JSString of an exact str (ropes are linearized in place, the flat
   string stays alive as long as v).  NULL with a pending exception. */
static JSString *qp_str_flat(JSContext *ctx, JSValueConst v)
{
    size_t len;
    JS_BOOL wide;
    const void *d;

    if (likely(JS_VALUE_GET_TAG(v) == JS_TAG_STRING))
        return JS_VALUE_GET_STRING(v);
    d = JS_GetStringData(ctx, v, &len, &wide);
    if (!d)
        return NULL;
    return (JSString *)((uint8_t *)d - offsetof(JSString, u));
}

static inline JSValue qp_str_empty(JSContext *ctx)
{
    return JS_AtomToString(ctx, JS_ATOM_empty_string);
}

/* Name of the type of v as CPython's Py_TYPE(v)->tp_name. */
static const char *qp_str_tpname(JSContext *ctx, JSValueConst v, char *buf, int size)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* Number of surrogate pairs in s[0:n]. */
static Py_ssize_t qp_u16_count_pairs(const uint16_t *s, Py_ssize_t n)
{
    Py_ssize_t i, k = 0;
    for (i = 0; i + 1 < n; i++) {
        if (QP_IS_HI_SURR(s[i]) && QP_IS_LO_SURR(s[i + 1])) {
            k++;
            i++;
        }
    }
    return k;
}

static QPUcs4Buf *qp_ucs4buf_from_u16(JSContext *ctx, const uint16_t *s, Py_ssize_t n,
                                      Py_ssize_t cplen)
{
    QPUcs4Buf *b;
    Py_ssize_t i, k = 0;

    b = js_malloc(ctx, sizeof(QPUcs4Buf) + sizeof(uint32_t) * (cplen ? cplen : 1));
    if (!b)
        return NULL;
    b->ref_count = 1;
    b->len = cplen;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (QP_IS_HI_SURR(c) && i + 1 < n && QP_IS_LO_SURR(s[i + 1])) {
            c = QP_FROM_SURR(c, s[i + 1]);
            i++;
        }
        b->data[k++] = c;
    }
    return b;
}

/* For a flat 16-bit string: *pb = new reference to its UCS4 side buffer if
   it contains surrogate pairs, else NULL; *pcplen = code points.  -1 on
   memory error (exception set). */
static int qp_str_wide_info(JSContext *ctx, JSString *p, QPUcs4Buf **pb, Py_ssize_t *pcplen)
{
    QPStrCacheEntry *e = NULL;
    Py_ssize_t npairs;
    QPUcs4Buf *b;

    *pb = NULL;
    if (p->len >= QP_STR_CACHE_MIN_LEN) {
        e = qp_str_cache_get(ctx, p, TRUE);
        if (e && e->cplen >= 0) {
            if (e->ucs4) {
                e->ucs4->ref_count++;
                *pb = e->ucs4;
            }
            *pcplen = e->cplen;
            return 0;
        }
    }
    npairs = qp_u16_count_pairs(p->u.str16, p->len);
    *pcplen = p->len - npairs;
    if (npairs == 0) {
        if (e)
            e->cplen = p->len;
        return 0;
    }
    b = qp_ucs4buf_from_u16(ctx, p->u.str16, p->len, p->len - npairs);
    if (!b)
        return -1;
    if (e) {
        e->cplen = b->len;
        e->ucs4 = b;
        b->ref_count++;
    }
    *pb = b;
    return 0;
}

/* ========================================================================= */
/* Views and lengths                                                          */
/* ========================================================================= */

static int qp_str_view_acquire(JSContext *ctx, JSValueConst s, qp_strview *view)
{
    JSString *p = qp_str_flat(ctx, qp_str_unbox(s));
    QPUcs4Buf *b;
    Py_ssize_t cplen;

    if (!p) {
        *view = qp_strview_make(1, "", 0);
        return -1;
    }
    if (!p->is_wide_char) {
        *view = qp_strview_make(1, p->u.str8, p->len);
        return 0;
    }
    if (qp_str_wide_info(ctx, p, &b, &cplen) < 0) {
        *view = qp_strview_make(1, "", 0);
        return -1;
    }
    if (b)
        *view = qp_strview_make(4, b->data, b->len);
    else
        *view = qp_strview_make(2, p->u.str16, p->len);
    return 0;
}

static void qp_str_view_release(JSContext *ctx, qp_strview *view)
{
    if (view->kind == 4 && view->data) {
        QPUcs4Buf *b = (QPUcs4Buf *)((uint8_t *)view->data - offsetof(QPUcs4Buf, data));
        qp_ucs4buf_unref(ctx->rt, b);
    }
    view->data = NULL;
    view->len = 0;
    view->kind = 1;
}

static Py_ssize_t qp_str_len(JSContext *ctx, JSValueConst s)
{
    JSValueConst v = qp_str_unbox(s);
    JSString *p;
    QPUcs4Buf *b;
    Py_ssize_t cplen;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING) {
        p = JS_VALUE_GET_STRING(v);
        if (!p->is_wide_char)
            return p->len;
    } else {
        JSStringRope *r = JS_VALUE_GET_STRING_ROPE(v);
        if (!r->is_wide_char)
            return r->len;
        p = qp_str_flat(ctx, v);
        if (!p)
            return -1;
    }
    if (p->len < QP_STR_CACHE_MIN_LEN)
        return p->len - qp_u16_count_pairs(p->u.str16, p->len);
    if (qp_str_wide_info(ctx, p, &b, &cplen) < 0)
        return -1;
    qp_ucs4buf_unref(ctx->rt, b);
    return cplen;
}

/* A str argument held during a method call: exact value, flat string and
   code-point view. */
typedef struct QPStrArg {
    JSValueConst v;         /* exact str (borrowed from the caller's value) */
    JSString *p;            /* flat string of v */
    qp_strview view;
} QPStrArg;

static int qp_sarg_get(JSContext *ctx, JSValueConst s, QPStrArg *a)
{
    a->v = qp_str_unbox(s);
    a->p = qp_str_flat(ctx, a->v);
    if (!a->p) {
        a->view = qp_strview_make(1, "", 0);
        return -1;
    }
    return qp_str_view_acquire(ctx, a->v, &a->view);
}

static void qp_sarg_free(JSContext *ctx, QPStrArg *a)
{
    qp_str_view_release(ctx, &a->view);
}

/* ========================================================================= */
/* Construction                                                               */
/* ========================================================================= */

static JSValue qp_str_too_long(JSContext *ctx)
{
    /* the JS string length limit (2**30 - 1 code units) is far below
       CPython's; exceeding it is reported as an allocation failure */
    return qp_raise_memory_error(ctx);
}

/* New str from code points data[0:len] of the given kind (1, 2 or 4).  The
   result is canonical: 8-bit when every code point is < 0x100. */
static JSValue qp_str_new_ucs(JSContext *ctx, int kind, const void *data, Py_ssize_t len)
{
    JSString *str;
    Py_ssize_t i, n16;
    Py_UCS4 maxc = 0;

    if (len <= 0)
        return qp_str_empty(ctx);
    if (kind == 1) {
        if (len > JS_STRING_LEN_MAX)
            return qp_str_too_long(ctx);
        return js_new_string8_len(ctx, data, (int)len);
    }
    if (kind == 2) {
        const uint16_t *s = data;
        for (i = 0; i < len; i++)
            maxc |= s[i];
        if (len > JS_STRING_LEN_MAX)
            return qp_str_too_long(ctx);
        if (maxc < 0x100) {
            str = js_alloc_string(ctx, (int)len, 0);
            if (!str)
                return JS_EXCEPTION;
            for (i = 0; i < len; i++)
                str->u.str8[i] = (uint8_t)s[i];
            str->u.str8[len] = '\0';
            return JS_MKPTR(JS_TAG_STRING, str);
        }
        return js_new_string16_len(ctx, s, (int)len);
    } else {
        const uint32_t *s = data;
        n16 = len;
        for (i = 0; i < len; i++) {
            if (s[i] > maxc)
                maxc = s[i];
            if (s[i] >= 0x10000)
                n16++;
        }
        if (n16 > JS_STRING_LEN_MAX)
            return qp_str_too_long(ctx);
        if (maxc < 0x100) {
            str = js_alloc_string(ctx, (int)len, 0);
            if (!str)
                return JS_EXCEPTION;
            for (i = 0; i < len; i++)
                str->u.str8[i] = (uint8_t)s[i];
            str->u.str8[len] = '\0';
        } else {
            Py_ssize_t k = 0;
            str = js_alloc_string(ctx, (int)n16, 1);
            if (!str)
                return JS_EXCEPTION;
            for (i = 0; i < len; i++) {
                uint32_t c = s[i];
                if (c >= 0x10000) {
                    c -= 0x10000;
                    str->u.str16[k++] = 0xD800 + (c >> 10);
                    str->u.str16[k++] = 0xDC00 + (c & 0x3FF);
                } else {
                    str->u.str16[k++] = c;
                }
            }
        }
        return JS_MKPTR(JS_TAG_STRING, str);
    }
}

static JSValue qp_str_from_view(JSContext *ctx, qp_strview v)
{
    return qp_str_new_ucs(ctx, v.kind, v.data, v.len);
}

static JSValue qp_str_from_ucs4(JSContext *ctx, const uint32_t *s, size_t len)
{
    return qp_str_new_ucs(ctx, 4, s, (Py_ssize_t)len);
}

static JSValue qp_str_from_char(JSContext *ctx, uint32_t cp)
{
    return qp_str_new_ucs(ctx, 4, &cp, 1);
}

/* Map a stringlib status to the Python exception (always JS_EXCEPTION). */
static JSValue qp_sl_raise(JSContext *ctx, int status, const char *overflow_msg)
{
    if (status == QP_SL_OVERFLOW)
        return qp_raise_overflow_error(ctx, "%s", overflow_msg);
    if (status == QP_SL_EMPTY_SEP)
        return qp_raise_value_error(ctx, "empty separator");
    return qp_raise_memory_error(ctx);
}

/* str from a builder (freed). */
static JSValue qp_str_from_builder(JSContext *ctx, qp_ucsbuilder *b)
{
    qp_strview v = qp_ucsbuilder_view(b);
    JSValue r = qp_str_new_ucs(ctx, v.kind, v.data, v.len);
    qp_ucsbuilder_free(b);
    return r;
}

/* Substring a[start:end] (code points, 0 <= start <= end <= len); the
   whole string gives the exact str itself. */
static JSValue qp_sarg_sub(JSContext *ctx, QPStrArg *a, Py_ssize_t start, Py_ssize_t end)
{
    if (start <= 0 && end >= a->view.len)
        return JS_DupValue(ctx, a->v);
    if (end <= start)
        return qp_str_empty(ctx);
    if (a->view.kind != 4)
        return js_sub_string(ctx, a->p, (int)start, (int)end);
    return qp_str_new_ucs(ctx, 4, (const uint32_t *)a->view.data + start, end - start);
}

static JSValue qp_str_from_utf8(JSContext *ctx, const char *s, size_t len)
{
    qp_ucsbuilder b;
    qp_codec_error err;
    size_t i;
    int r;

    for (i = 0; i < len; i++) {
        if ((uint8_t)s[i] >= 0x80)
            break;
    }
    if (i == len) {
        if (len > JS_STRING_LEN_MAX)
            return qp_str_too_long(ctx);
        return js_new_string8_len(ctx, s, (int)len);
    }
    qp_ucsbuilder_init(&b, ctx->rt);
    r = qp_sl_utf8_decode(s, (Py_ssize_t)len, NULL, &b, &err);
    if (r < 0) {
        qp_ucsbuilder_free(&b);
        if (r == QP_SL_CODEC_ERROR)
            return qp_raise_unicode_decode(ctx, "utf-8", s, (Py_ssize_t)len,
                                           err.start, err.end, err.reason);
        return qp_raise_memory_error(ctx);
    }
    return qp_str_from_builder(ctx, &b);
}

static JSValue qp_str_from_cstr(JSContext *ctx, const char *s)
{
    return qp_str_from_utf8(ctx, s, strlen(s));
}

static JSValue qp_str_from_atom(JSContext *ctx, JSAtom atom)
{
    return JS_AtomToString(ctx, atom);
}

static JSAtom qp_str_to_atom(JSContext *ctx, JSValueConst s)
{
    JSString *p = qp_str_flat(ctx, qp_str_unbox(s));
    if (!p)
        return JS_ATOM_NULL;
    js_rc(p)->ref_count++;          /* JS_NewAtomStr consumes a reference */
    return JS_NewAtomStr(ctx, p);
}

/* Index of the first lone surrogate of a flat string at or after i (code
   unit index), or -1. */
static int qp_u16_find_lone_surrogate(const uint16_t *s, int n, int i)
{
    for (; i < n; i++) {
        uint16_t c = s[i];
        if (QP_IS_SURR(c)) {
            if (QP_IS_HI_SURR(c) && i + 1 < n && QP_IS_LO_SURR(s[i + 1])) {
                i++;
                continue;
            }
            return i;
        }
    }
    return -1;
}

/* Code-point index of code unit u in s (pairs before it count once). */
static Py_ssize_t qp_u16_cp_index(const uint16_t *s, Py_ssize_t u)
{
    return u - qp_u16_count_pairs(s, u);
}

static const char *qp_str_utf8(JSContext *ctx, JSValueConst s, size_t *plen)
{
    JSValueConst v = qp_str_unbox(s);
    JSString *p = qp_str_flat(ctx, v);
    int i;

    if (!p)
        return NULL;
    if (p->is_wide_char) {
        i = qp_u16_find_lone_surrogate(p->u.str16, p->len, 0);
        if (i >= 0) {
            /* UnicodeEncodeError over the run of lone surrogates */
            int j = i + 1;
            while (j < (int)p->len && QP_IS_SURR(p->u.str16[j]) &&
                   qp_u16_find_lone_surrogate(p->u.str16, p->len, j) == j)
                j++;
            qp_raise_unicode_encode(ctx, "utf-8", v, qp_u16_cp_index(p->u.str16, i),
                                    qp_u16_cp_index(p->u.str16, j), "surrogates not allowed");
            return NULL;
        }
    }
    return JS_ToCStringLen(ctx, plen, JS_MKPTR(JS_TAG_STRING, p));
}

/* ========================================================================= */
/* Concatenation, join, comparison, hash                                      */
/* ========================================================================= */

static JSValue qp_str_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValueConst va = qp_str_unbox(a), vb = qp_str_unbox(b);
    if (qp_str_units(va) + qp_str_units(vb) > JS_STRING_LEN_MAX)
        return qp_str_too_long(ctx);
    return JS_ConcatString(ctx, JS_DupValue(ctx, va), JS_DupValue(ctx, vb));
}

static JSValue qp_str_join_array(JSContext *ctx, JSValueConst sep, int n, JSValueConst *items)
{
    StringBuffer sb;
    JSValueConst vsep = qp_str_unbox(sep);
    Py_ssize_t total, seplen;
    int i, wide;

    if (n == 0)
        return qp_str_empty(ctx);
    if (n == 1 && qp_str_check_exact(items[0]))
        return JS_DupValue(ctx, items[0]);
    seplen = qp_str_units(vsep);
    total = seplen * (n - 1);
    wide = JS_VALUE_GET_TAG(vsep) == JS_TAG_STRING ? JS_VALUE_GET_STRING(vsep)->is_wide_char
                                                   : JS_VALUE_GET_STRING_ROPE(vsep)->is_wide_char;
    for (i = 0; i < n; i++) {
        JSValueConst it = qp_str_unbox(items[i]);
        total += qp_str_units(it);
        if (JS_VALUE_GET_TAG(it) == JS_TAG_STRING ? JS_VALUE_GET_STRING(it)->is_wide_char
                                                  : JS_VALUE_GET_STRING_ROPE(it)->is_wide_char)
            wide = 1;
        if (total > JS_STRING_LEN_MAX)
            return qp_str_too_long(ctx);
    }
    if (total == 0)
        return qp_str_empty(ctx);
    if (string_buffer_init2(ctx, &sb, (int)total, wide))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        if (i > 0 && seplen > 0)
            string_buffer_concat_value(&sb, vsep);
        string_buffer_concat_value(&sb, qp_str_unbox(items[i]));
    }
    return string_buffer_end(&sb);
}

static int qp_str_eq(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValueConst va = qp_str_unbox(a), vb = qp_str_unbox(b);
    JSString *pa, *pb;

    if (JS_VALUE_GET_PTR(va) == JS_VALUE_GET_PTR(vb))
        return 1;
    if (qp_str_units(va) != qp_str_units(vb))
        return 0;
    pa = qp_str_flat(ctx, va);
    if (!pa)
        return -1;
    pb = qp_str_flat(ctx, vb);
    if (!pb)
        return -1;
    return js_string_eq(ctx, pa, pb);
}

static inline uint32_t qp_jsstr_unit(const JSString *p, uint32_t i)
{
    return p->is_wide_char ? p->u.str16[i] : p->u.str8[i];
}

/* Code point starting at code unit i of p. */
static uint32_t qp_jsstr_cp_at(const JSString *p, uint32_t i)
{
    uint32_t c = qp_jsstr_unit(p, i);
    if (QP_IS_HI_SURR(c) && i + 1 < p->len) {
        uint32_t c2 = qp_jsstr_unit(p, i + 1);
        if (QP_IS_LO_SURR(c2))
            return QP_FROM_SURR(c, c2);
    }
    return c;
}

/* Compare two flat strings in code point order (unicode_compare). */
static int qp_jsstr_compare(const JSString *a, const JSString *b)
{
    uint32_t la = a->len, lb = b->len, n = la < lb ? la : lb, i;

    if (!a->is_wide_char && !b->is_wide_char) {
        int r = memcmp(a->u.str8, b->u.str8, n);
        if (r)
            return r < 0 ? -1 : 1;
    } else {
        for (i = 0; i < n; i++) {
            if (qp_jsstr_unit(a, i) != qp_jsstr_unit(b, i))
                break;
        }
        if (i < n) {
            /* first differing unit: compare the code points that contain
               it (a pair that started at i - 1 in one string only is
               compared with the lone high surrogate of the other) */
            uint32_t pa = i, pb = i, pos, ca, cb;
            if (i > 0 && QP_IS_HI_SURR(qp_jsstr_unit(a, i - 1)) && QP_IS_LO_SURR(qp_jsstr_unit(a, i)))
                pa = i - 1;
            if (i > 0 && QP_IS_HI_SURR(qp_jsstr_unit(b, i - 1)) && QP_IS_LO_SURR(qp_jsstr_unit(b, i)))
                pb = i - 1;
            pos = pa < pb ? pa : pb;
            ca = qp_jsstr_cp_at(a, pos);
            cb = qp_jsstr_cp_at(b, pos);
            if (ca != cb)
                return ca < cb ? -1 : 1;
            /* not reached for distinct units; fall back to unit order */
            return qp_jsstr_unit(a, i) < qp_jsstr_unit(b, i) ? -1 : 1;
        }
    }
    return la < lb ? -1 : la > lb ? 1 : 0;
}

static int qp_str_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSString *pa = qp_str_flat(ctx, qp_str_unbox(a)), *pb;
    if (!pa)
        return INT_MIN;
    pb = qp_str_flat(ctx, qp_str_unbox(b));
    if (!pb)
        return INT_MIN;
    if (pa == pb)
        return 0;
    return qp_jsstr_compare(pa, pb);
}

static Py_hash_t qp_jsstr_hash(JSContext *ctx, JSString *p)
{
    const uint8_t *key = QP_STATE(ctx)->hash_key;
    if (p->len == 0)
        return 0;
    if (!p->is_wide_char)
        return (Py_hash_t)qp_hash_str_latin1(p->u.str8, p->len, key);
    return (Py_hash_t)qp_hash_str_utf16(p->u.str16, p->len, key);
}

static Py_hash_t qp_str_hash(JSContext *ctx, JSValueConst s)
{
    JSString *p = qp_str_flat(ctx, qp_str_unbox(s));
    QPStrCacheEntry *e;
    Py_hash_t h;

    if (!p)
        return -1;
    if (p->len == 0)
        return 0;
    if (p->atom_type == 0 && (p->len < QP_STR_CACHE_MIN_LEN || p->len > QP_STR_HASH_CACHE_MAX_LEN))
        return qp_jsstr_hash(ctx, p);
    e = qp_str_cache_get(ctx, p, TRUE);
    if (e && e->hash != -1)
        return e->hash;
    h = qp_jsstr_hash(ctx, p);
    if (e)
        e->hash = h;
    return h;
}

static JSValue qp_str_substring(JSContext *ctx, JSValueConst s, Py_ssize_t start, Py_ssize_t end)
{
    QPStrArg a;
    JSValue r;

    if (qp_sarg_get(ctx, s, &a) < 0)
        return JS_EXCEPTION;
    if (start < 0)
        start = 0;
    if (end > a.view.len)
        end = a.view.len;
    r = qp_sarg_sub(ctx, &a, start, end);
    qp_sarg_free(ctx, &a);
    return r;
}

/* ========================================================================= */
/* repr / ascii                                                               */
/* ========================================================================= */

/* Append the backslash escape of a non-ASCII or non-printable code point. */
static void qp_sb_put_escape(StringBuffer *sb, Py_UCS4 ch)
{
    string_buffer_putc8(sb, '\\');
    if (ch <= 0xff) {
        string_buffer_putc8(sb, 'x');
        string_buffer_putc8(sb, qp_hexdigits[(ch >> 4) & 0xF]);
        string_buffer_putc8(sb, qp_hexdigits[ch & 0xF]);
    } else if (ch <= 0xffff) {
        string_buffer_putc8(sb, 'u');
        string_buffer_putc8(sb, qp_hexdigits[(ch >> 12) & 0xF]);
        string_buffer_putc8(sb, qp_hexdigits[(ch >> 8) & 0xF]);
        string_buffer_putc8(sb, qp_hexdigits[(ch >> 4) & 0xF]);
        string_buffer_putc8(sb, qp_hexdigits[ch & 0xF]);
    } else {
        int sh;
        string_buffer_putc8(sb, 'U');
        for (sh = 28; sh >= 0; sh -= 4)
            string_buffer_putc8(sb, qp_hexdigits[(ch >> sh) & 0xF]);
    }
}

/* unicode_repr + stringlib repr(). */
static JSValue qp_str_repr(JSContext *ctx, JSValueConst s)
{
    qp_strview v;
    StringBuffer sb;
    Py_ssize_t i, squote = 0, dquote = 0, osize = 2;
    Py_UCS4 quote = '\'';
    int wide = 0;

    if (qp_str_view_acquire(ctx, s, &v) < 0)
        return JS_EXCEPTION;
    for (i = 0; i < v.len; i++) {
        Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
        if (ch == '\'')
            squote++;
        else if (ch == '"')
            dquote++;
        else if (ch >= 0x100 && _PyUnicode_IsPrintable(ch))
            wide = 1;
    }
    if (squote && !dquote)
        quote = '"';
    osize += v.len;
    if (osize > JS_STRING_LEN_MAX)
        osize = JS_STRING_LEN_MAX;
    if (string_buffer_init2(ctx, &sb, (int)osize, wide)) {
        qp_str_view_release(ctx, &v);
        return JS_EXCEPTION;
    }
    string_buffer_putc8(&sb, quote);
    for (i = 0; i < v.len; i++) {
        Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
        if (ch == quote || ch == '\\') {
            string_buffer_putc8(&sb, '\\');
            string_buffer_putc8(&sb, ch);
        } else if (ch == '\t') {
            string_buffer_putc8(&sb, '\\');
            string_buffer_putc8(&sb, 't');
        } else if (ch == '\n') {
            string_buffer_putc8(&sb, '\\');
            string_buffer_putc8(&sb, 'n');
        } else if (ch == '\r') {
            string_buffer_putc8(&sb, '\\');
            string_buffer_putc8(&sb, 'r');
        } else if (ch < ' ' || ch == 0x7F) {
            qp_sb_put_escape(&sb, ch);
        } else if (ch < 0x7F) {
            string_buffer_putc8(&sb, ch);
        } else if (!_PyUnicode_IsPrintable(ch)) {
            qp_sb_put_escape(&sb, ch);
        } else {
            string_buffer_putc(&sb, ch);
        }
    }
    string_buffer_putc8(&sb, quote);
    qp_str_view_release(ctx, &v);
    return string_buffer_end(&sb);
}

/* PyObject_ASCII tail: escape every non-ASCII code point of repr_str with
   \xNN, \uNNNN or \UNNNNNNNN (the 'backslashreplace' forms). */
static JSValue qp_str_ascii_escape(JSContext *ctx, JSValueConst repr_str)
{
    qp_strview v;
    StringBuffer sb;
    Py_ssize_t i;

    if (qp_str_view_acquire(ctx, repr_str, &v) < 0)
        return JS_EXCEPTION;
    for (i = 0; i < v.len; i++) {
        if (QP_SL_READ(v.kind, v.data, i) >= 0x80)
            break;
    }
    if (i == v.len) {
        qp_str_view_release(ctx, &v);
        return qp_str_value(ctx, repr_str);
    }
    if (string_buffer_init(ctx, &sb, (int)(v.len < JS_STRING_LEN_MAX ? v.len : JS_STRING_LEN_MAX))) {
        qp_str_view_release(ctx, &v);
        return JS_EXCEPTION;
    }
    for (i = 0; i < v.len; i++) {
        Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
        if (ch < 0x80)
            string_buffer_putc8(&sb, ch);
        else
            qp_sb_put_escape(&sb, ch);
    }
    qp_str_view_release(ctx, &v);
    return string_buffer_end(&sb);
}

/* ========================================================================= */
/* Slots                                                                      */
/* ========================================================================= */

static Py_ssize_t qp_str_sq_length(JSContext *ctx, JSValueConst a)
{
    return qp_str_len(ctx, a);
}

/* unicode_getitem: i already adjusted for negative values. */
static JSValue qp_str_sq_item(JSContext *ctx, JSValueConst a, Py_ssize_t i)
{
    qp_strview v;
    Py_UCS4 ch;

    if (qp_str_view_acquire(ctx, a, &v) < 0)
        return JS_EXCEPTION;
    if (i < 0 || i >= v.len) {
        qp_str_view_release(ctx, &v);
        return qp_raise_index_error(ctx, "string index out of range");
    }
    ch = QP_SL_READ(v.kind, v.data, i);
    qp_str_view_release(ctx, &v);
    return qp_str_from_char(ctx, ch);
}

/* Virtual iteration (unicode_iteritem). */
static int qp_str_iteritem(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem)
{
    qp_strview v;
    Py_UCS4 ch;

    *pitem = QP_NULL;
    if (qp_str_view_acquire(ctx, seq, &v) < 0)
        return -1;
    if (index >= v.len) {
        qp_str_view_release(ctx, &v);
        return 0;
    }
    ch = QP_SL_READ(v.kind, v.data, index);
    qp_str_view_release(ctx, &v);
    *pitem = qp_str_from_char(ctx, ch);
    return JS_IsException(*pitem) ? -1 : 1;
}

/* PyUnicode_Concat as sq_concat. */
static JSValue qp_str_sq_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    if (!qp_str_check(ctx, a))
        return qp_raise_type_error(ctx, "must be str, not %.100s", qp_str_tpname(ctx, a, buf, sizeof(buf)));
    if (!qp_str_check(ctx, b))
        return qp_raise_type_error(ctx, "can only concatenate str (not \"%.200s\") to str",
                                   qp_str_tpname(ctx, b, buf, sizeof(buf)));
    return qp_str_concat(ctx, a, b);
}

/* unicode_repeat. */
static JSValue qp_str_sq_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t n)
{
    JSValueConst v = qp_str_unbox(a);
    JSString *p, *str;
    Py_ssize_t ulen, total, done, nbytes, unit;

    if (n < 1)
        return qp_str_empty(ctx);
    if (n == 1)
        return JS_DupValue(ctx, v);
    ulen = qp_str_units(v);
    if (ulen == 0)
        return qp_str_empty(ctx);
    {
        Py_ssize_t cplen = qp_str_len(ctx, v);
        if (cplen < 0)
            return JS_EXCEPTION;
        if (cplen > PY_SSIZE_T_MAX / n)
            return qp_raise_overflow_error(ctx, "repeated string is too long");
    }
    if (ulen > JS_STRING_LEN_MAX / n)
        return qp_str_too_long(ctx);
    p = qp_str_flat(ctx, v);
    if (!p)
        return JS_EXCEPTION;
    total = ulen * n;
    str = js_alloc_string(ctx, (int)total, p->is_wide_char);
    if (!str)
        return JS_EXCEPTION;
    unit = p->is_wide_char ? 2 : 1;
    nbytes = total * unit;
    memcpy(str->u.str8, p->u.str8, ulen * unit);
    done = ulen * unit;
    while (done < nbytes) {
        Py_ssize_t chunk = done <= nbytes - done ? done : nbytes - done;
        memcpy(str->u.str8 + done, str->u.str8, chunk);
        done += chunk;
    }
    if (!p->is_wide_char)
        str->u.str8[total] = '\0';
    return JS_MKPTR(JS_TAG_STRING, str);
}

/* PyUnicode_Contains. */
static int qp_str_sq_contains(JSContext *ctx, JSValueConst a, JSValueConst key)
{
    QPStrArg s, sub;
    Py_ssize_t r;

    if (!qp_str_check(ctx, key)) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        qp_raise_type_error(ctx, "'in <string>' requires string as left operand, not %.100s",
                            qp_str_tpname(ctx, key, buf, sizeof(buf)));
        return -1;
    }
    if (qp_sarg_get(ctx, a, &s) < 0)
        return -1;
    if (qp_sarg_get(ctx, key, &sub) < 0) {
        qp_sarg_free(ctx, &s);
        return -1;
    }
    r = qp_sl_find(ctx->rt, s.view, sub.view, 0, PY_SSIZE_T_MAX, 1);
    qp_sarg_free(ctx, &sub);
    qp_sarg_free(ctx, &s);
    if (r == QP_SL_NOMEM) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    return r >= 0;
}

static BOOL qp_str_has_index(JSContext *ctx, JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT || tag == JS_TAG_BOOL || tag == JS_TAG_SHORT_BIG_INT || tag == JS_TAG_BIG_INT)
        return TRUE;
    return qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
}

/* unicode_subscript. */
static JSValue qp_str_mp_subscript(JSContext *ctx, JSValueConst a, JSValueConst item)
{
    if (qp_str_has_index(ctx, item)) {
        Py_ssize_t i, len;
        if (JS_VALUE_GET_TAG(item) == JS_TAG_INT) {
            i = JS_VALUE_GET_INT(item);
        } else if (qp_as_ssize(ctx, item, QP_EXC(ctx, IndexError), &i) < 0) {
            return JS_EXCEPTION;
        }
        if (i < 0) {
            len = qp_str_len(ctx, a);
            if (len < 0)
                return JS_EXCEPTION;
            i += len;
        }
        return qp_str_sq_item(ctx, a, i);
    }
    if (qp_slice_check(item)) {
        Py_ssize_t start, stop, step, slen, i, cur;
        QPStrArg s;
        JSValue r;

        if (qp_slice_unpack(ctx, item, &start, &stop, &step) < 0)
            return JS_EXCEPTION;
        if (qp_sarg_get(ctx, a, &s) < 0)
            return JS_EXCEPTION;
        slen = qp_slice_adjust_indices(s.view.len, &start, &stop, step);
        if (slen <= 0) {
            r = qp_str_empty(ctx);
        } else if (start == 0 && step == 1 && slen == s.view.len) {
            r = JS_DupValue(ctx, s.v);
        } else if (step == 1) {
            r = qp_sarg_sub(ctx, &s, start, start + slen);
        } else {
            uint32_t *buf = js_malloc(ctx, sizeof(uint32_t) * slen);
            if (!buf) {
                r = JS_EXCEPTION;
            } else {
                for (cur = start, i = 0; i < slen; cur += step, i++)
                    buf[i] = QP_SL_READ(s.view.kind, s.view.data, cur);
                r = qp_str_new_ucs(ctx, 4, buf, slen);
                js_free(ctx, buf);
            }
        }
        qp_sarg_free(ctx, &s);
        return r;
    }
    {
        char buf[ATOM_GET_STR_BUF_SIZE];
        return qp_raise_type_error(ctx, "string indices must be integers, not '%.200s'",
                                   qp_str_tpname(ctx, item, buf, sizeof(buf)));
    }
}

/* PyUnicode_RichCompare. */
static JSValue qp_str_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    int r;

    if (!qp_str_check(ctx, a) || !qp_str_check(ctx, b))
        return qp_not_implemented(ctx);
    if (op == QP_CMP_EQ || op == QP_CMP_NE) {
        r = qp_str_eq(ctx, a, b);
        if (r < 0)
            return JS_EXCEPTION;
        return qp_bool_new(op == QP_CMP_EQ ? r : !r);
    }
    r = qp_str_compare(ctx, a, b);
    if (r == INT_MIN)
        return JS_EXCEPTION;
    switch (op) {
    case QP_CMP_LT: return qp_bool_new(r < 0);
    case QP_CMP_LE: return qp_bool_new(r <= 0);
    case QP_CMP_GT: return qp_bool_new(r > 0);
    default:        return qp_bool_new(r >= 0);
    }
}

/* tp_str (unicode_str): the exact str. */
static JSValue qp_str_tp_str(JSContext *ctx, JSValueConst a)
{
    return qp_str_value(ctx, a);
}

/* ========================================================================= */
/* Argument helpers                                                           */
/* ========================================================================= */

/* _PyArg_BadArgument: "%s() %s must be %s, not %s". */
static JSValue qp_str_bad_arg(JSContext *ctx, const char *fname, const char *displayname,
                              const char *expected, JSValueConst arg)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *tn = JS_IsUndefined(arg) || JS_IsNull(arg) ? "None"
                                                           : qp_str_tpname(ctx, arg, buf, sizeof(buf));
    return qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50s",
                               fname, displayname, expected, tn);
}

/* _PyEval_SliceIndex: None leaves *out unchanged. */
static int qp_str_slice_index(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    if (qp_str_has_index(ctx, v))
        return qp_as_ssize(ctx, v, NULL, out);
    qp_raise_type_error(ctx, "slice indices must be integers or None or have an __index__ method");
    return -1;
}

/* PyLong_AsInt-style conversion (Argument Clinic `int`). */
static int qp_str_arg_int(JSContext *ctx, JSValueConst v, int *out)
{
    JSValue iv;
    int64_t x;
    int overflow = 0;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    iv = qp_index(ctx, v);
    if (JS_IsException(iv))
        return -1;
    qp_long_as_int64_overflow(iv, &x, &overflow);
    JS_FreeValue(ctx, iv);
    if (overflow || x > INT_MAX || x < INT_MIN) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C int");
        return -1;
    }
    *out = (int)x;
    return 0;
}

/* convert_uc: one-character fill argument. */
static int qp_str_convert_uc(JSContext *ctx, JSValueConst obj, Py_UCS4 *out)
{
    qp_strview v;
    if (!qp_str_check(ctx, obj)) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        qp_raise_type_error(ctx, "The fill character must be a unicode character, not %.100s",
                            qp_str_tpname(ctx, obj, buf, sizeof(buf)));
        return -1;
    }
    if (qp_str_view_acquire(ctx, obj, &v) < 0)
        return -1;
    if (v.len != 1) {
        qp_str_view_release(ctx, &v);
        qp_raise_type_error(ctx, "The fill character must be exactly one character long");
        return -1;
    }
    *out = QP_SL_READ(v.kind, v.data, 0);
    qp_str_view_release(ctx, &v);
    return 0;
}

/* Result of a builder-producing stringlib call: UNCHANGED gives the exact
   str of the receiver (unicode_result_unchanged). */
static JSValue qp_str_build_result(JSContext *ctx, QPStrArg *s, int status, qp_ucsbuilder *b,
                                   const char *overflow_msg)
{
    if (status == QP_SL_UNCHANGED) {
        qp_ucsbuilder_free(b);
        return JS_DupValue(ctx, s->v);
    }
    if (status < 0) {
        qp_ucsbuilder_free(b);
        return qp_sl_raise(ctx, status, overflow_msg);
    }
    return qp_str_from_builder(ctx, b);
}

/* list of the substrings named by the spans (split results). */
static JSValue qp_str_spans_to_list(JSContext *ctx, QPStrArg *s, qp_spanvec *sv)
{
    JSValue list, *items;
    Py_ssize_t i;

    items = js_malloc(ctx, sizeof(JSValue) * (sv->len ? sv->len : 1));
    if (!items)
        return JS_EXCEPTION;
    for (i = 0; i < sv->len; i++) {
        items[i] = qp_sarg_sub(ctx, s, sv->items[i].start, sv->items[i].end);
        if (JS_IsException(items[i])) {
            while (i-- > 0)
                JS_FreeValue(ctx, items[i]);
            js_free(ctx, items);
            return JS_EXCEPTION;
        }
    }
    list = qp_list_new_steal(ctx, sv->len, items);
    js_free(ctx, items);
    return list;
}

/* ========================================================================= */
/* Methods                                                                    */
/* ========================================================================= */

#define QP_STR_METH(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                        int nargs, JSValueConst kwnames, int magic)

/* ---- case conversion (case_operation) ---------------------------------- */

static Py_UCS4 qp_handle_capital_sigma(qp_strview v, Py_ssize_t i)
{
    Py_ssize_t j;
    int final_sigma;
    Py_UCS4 c = 0;

    for (j = i - 1; j >= 0; j--) {
        c = QP_SL_READ(v.kind, v.data, j);
        if (!_PyUnicode_IsCaseIgnorable(c))
            break;
    }
    final_sigma = j >= 0 && _PyUnicode_IsCased(c);
    if (final_sigma) {
        for (j = i + 1; j < v.len; j++) {
            c = QP_SL_READ(v.kind, v.data, j);
            if (!_PyUnicode_IsCaseIgnorable(c))
                break;
        }
        final_sigma = j == v.len || !_PyUnicode_IsCased(c);
    }
    return final_sigma ? 0x3C2 : 0x3C3;
}

static int qp_lower_ucs4(qp_strview v, Py_ssize_t i, Py_UCS4 c, Py_UCS4 *mapped)
{
    if (c == 0x3A3) {
        mapped[0] = qp_handle_capital_sigma(v, i);
        return 1;
    }
    return _PyUnicode_ToLowerFull(c, mapped);
}

enum { QP_CASE_LOWER, QP_CASE_UPPER, QP_CASE_CAPITALIZE, QP_CASE_CASEFOLD,
       QP_CASE_SWAPCASE, QP_CASE_TITLE };

static Py_ssize_t qp_do_case(qp_strview v, Py_UCS4 *res, int op)
{
    Py_ssize_t i, k = 0;
    int previous_is_cased = 0;

    for (i = 0; i < v.len; i++) {
        Py_UCS4 c = QP_SL_READ(v.kind, v.data, i), mapped[3];
        int n, j;
        switch (op) {
        case QP_CASE_LOWER:
            n = qp_lower_ucs4(v, i, c, mapped);
            break;
        case QP_CASE_UPPER:
            n = _PyUnicode_ToUpperFull(c, mapped);
            break;
        case QP_CASE_CAPITALIZE:
            n = i == 0 ? _PyUnicode_ToTitleFull(c, mapped) : qp_lower_ucs4(v, i, c, mapped);
            break;
        case QP_CASE_CASEFOLD:
            n = _PyUnicode_ToFoldedFull(c, mapped);
            break;
        case QP_CASE_SWAPCASE:
            if (_PyUnicode_IsUppercase(c)) {
                n = qp_lower_ucs4(v, i, c, mapped);
            } else if (_PyUnicode_IsLowercase(c)) {
                n = _PyUnicode_ToUpperFull(c, mapped);
            } else {
                n = 1;
                mapped[0] = c;
            }
            break;
        default: /* title */
            if (previous_is_cased)
                n = qp_lower_ucs4(v, i, c, mapped);
            else
                n = _PyUnicode_ToTitleFull(c, mapped);
            previous_is_cased = _PyUnicode_IsCased(c);
            break;
        }
        for (j = 0; j < n; j++)
            res[k++] = mapped[j];
    }
    return k;
}

QP_STR_METH(qp_str_m_case)
{
    QPStrArg s;
    Py_ssize_t i, k;
    Py_UCS4 *tmp;
    JSValue r;

    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (s.view.len == 0) {
        r = JS_DupValue(ctx, s.v);
        goto done;
    }
    /* ASCII fast path for lower/upper (ascii_upper_or_lower) */
    if ((magic == QP_CASE_LOWER || magic == QP_CASE_UPPER) && s.view.kind == 1) {
        const uint8_t *d = s.view.data;
        for (i = 0; i < s.view.len; i++) {
            if (d[i] >= 0x80)
                break;
        }
        if (i == s.view.len) {
            JSString *str = js_alloc_string(ctx, (int)s.view.len, 0);
            if (!str) {
                r = JS_EXCEPTION;
                goto done;
            }
            for (i = 0; i < s.view.len; i++)
                str->u.str8[i] = magic == QP_CASE_LOWER ? QP_TOLOWER(d[i]) : QP_TOUPPER(d[i]);
            str->u.str8[s.view.len] = '\0';
            r = JS_MKPTR(JS_TAG_STRING, str);
            goto done;
        }
    }
    if ((size_t)s.view.len > PY_SSIZE_T_MAX / (3 * sizeof(Py_UCS4))) {
        r = qp_raise_overflow_error(ctx, "string is too long");
        goto done;
    }
    tmp = js_malloc(ctx, sizeof(Py_UCS4) * 3 * s.view.len);
    if (!tmp) {
        r = JS_EXCEPTION;
        goto done;
    }
    k = qp_do_case(s.view, tmp, magic);
    r = qp_str_new_ucs(ctx, 4, tmp, k);
    js_free(ctx, tmp);
done:
    qp_sarg_free(ctx, &s);
    return r;
}

/* ---- predicates --------------------------------------------------------- */

enum { QP_PRED_ISASCII, QP_PRED_ISLOWER, QP_PRED_ISUPPER, QP_PRED_ISTITLE, QP_PRED_ISSPACE,
       QP_PRED_ISDECIMAL, QP_PRED_ISDIGIT, QP_PRED_ISNUMERIC, QP_PRED_ISALPHA, QP_PRED_ISALNUM,
       QP_PRED_ISIDENTIFIER, QP_PRED_ISPRINTABLE };

static int qp_str_predicate(qp_strview v, int op)
{
    Py_ssize_t i;
    int cased, previous_is_cased;

    switch (op) {
    case QP_PRED_ISASCII:
        for (i = 0; i < v.len; i++) {
            if (QP_SL_READ(v.kind, v.data, i) >= 0x80)
                return 0;
        }
        return 1;
    case QP_PRED_ISPRINTABLE:
        for (i = 0; i < v.len; i++) {
            if (!_PyUnicode_IsPrintable(QP_SL_READ(v.kind, v.data, i)))
                return 0;
        }
        return 1;
    case QP_PRED_ISLOWER:
    case QP_PRED_ISUPPER:
        cased = 0;
        for (i = 0; i < v.len; i++) {
            Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
            if (op == QP_PRED_ISLOWER) {
                if (_PyUnicode_IsUppercase(ch) || _PyUnicode_IsTitlecase(ch))
                    return 0;
                if (!cased && _PyUnicode_IsLowercase(ch))
                    cased = 1;
            } else {
                if (_PyUnicode_IsLowercase(ch) || _PyUnicode_IsTitlecase(ch))
                    return 0;
                if (!cased && _PyUnicode_IsUppercase(ch))
                    cased = 1;
            }
        }
        return cased;
    case QP_PRED_ISTITLE:
        cased = 0;
        previous_is_cased = 0;
        for (i = 0; i < v.len; i++) {
            Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
            if (_PyUnicode_IsUppercase(ch) || _PyUnicode_IsTitlecase(ch)) {
                if (previous_is_cased)
                    return 0;
                previous_is_cased = 1;
                cased = 1;
            } else if (_PyUnicode_IsLowercase(ch)) {
                if (!previous_is_cased)
                    return 0;
                previous_is_cased = 1;
                cased = 1;
            } else {
                previous_is_cased = 0;
            }
        }
        return cased;
    case QP_PRED_ISIDENTIFIER:
        if (v.len == 0)
            return 0;
        {
            Py_UCS4 ch = QP_SL_READ(v.kind, v.data, 0);
            if (!_PyUnicode_IsXidStart(ch) && ch != 0x5F)
                return 0;
        }
        for (i = 1; i < v.len; i++) {
            if (!_PyUnicode_IsXidContinue(QP_SL_READ(v.kind, v.data, i)))
                return 0;
        }
        return 1;
    default:
        if (v.len == 0)
            return 0;
        for (i = 0; i < v.len; i++) {
            Py_UCS4 ch = QP_SL_READ(v.kind, v.data, i);
            int ok;
            switch (op) {
            case QP_PRED_ISSPACE:   ok = qp_sl_unicode_isspace(ch); break;
            case QP_PRED_ISDECIMAL: ok = _PyUnicode_IsDecimalDigit(ch); break;
            case QP_PRED_ISDIGIT:   ok = _PyUnicode_IsDigit(ch); break;
            case QP_PRED_ISNUMERIC: ok = _PyUnicode_IsNumeric(ch); break;
            case QP_PRED_ISALPHA:   ok = _PyUnicode_IsAlpha(ch); break;
            default: /* isalnum */
                ok = _PyUnicode_IsAlpha(ch) || _PyUnicode_IsDecimalDigit(ch) ||
                     _PyUnicode_IsDigit(ch) || _PyUnicode_IsNumeric(ch);
                break;
            }
            if (!ok)
                return 0;
        }
        return 1;
    }
}

QP_STR_METH(qp_str_m_pred)
{
    qp_strview v;
    int r;

    if (qp_str_view_acquire(ctx, self, &v) < 0)
        return JS_EXCEPTION;
    r = qp_str_predicate(v, magic);
    qp_str_view_release(ctx, &v);
    return qp_bool_new(r);
}

/* ---- searching ------------------------------------------------------------ */

enum { QP_FIND_FIND, QP_FIND_RFIND, QP_FIND_INDEX, QP_FIND_RINDEX, QP_FIND_COUNT };
static const char *const qp_find_names[] = { "find", "rfind", "index", "rindex", "count" };

/* find/rfind/index/rindex/count(sub[, start[, end]]) */
QP_STR_METH(qp_str_m_find)
{
    const char *fname = qp_find_names[magic];
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX, r;
    QPStrArg s, sub;

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[0]))
        return qp_str_bad_arg(ctx, fname, "argument 1", "str", args[0]);
    if (nargs >= 2 && qp_str_slice_index(ctx, args[1], &start) < 0)
        return JS_EXCEPTION;
    if (nargs >= 3 && qp_str_slice_index(ctx, args[2], &end) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, args[0], &sub) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    if (magic == QP_FIND_COUNT)
        r = qp_sl_count(ctx->rt, s.view, sub.view, start, end);
    else
        r = qp_sl_find(ctx->rt, s.view, sub.view, start, end,
                       (magic == QP_FIND_FIND || magic == QP_FIND_INDEX) ? 1 : -1);
    qp_sarg_free(ctx, &sub);
    qp_sarg_free(ctx, &s);
    if (r == QP_SL_NOMEM)
        return qp_raise_memory_error(ctx);
    if (r < 0 && (magic == QP_FIND_INDEX || magic == QP_FIND_RINDEX))
        return qp_raise_value_error(ctx, "substring not found");
    return JS_NewInt64(ctx, r);
}

/* startswith/endswith(prefix[, start[, end]]); magic = direction */
QP_STR_METH(qp_str_m_tailmatch)
{
    const char *fname = magic < 0 ? "startswith" : "endswith";
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX, i, n;
    JSValueConst sub;
    QPStrArg s, a;
    int r = 0;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    sub = args[0];
    if (nargs >= 2 && qp_str_slice_index(ctx, args[1], &start) < 0)
        return JS_EXCEPTION;
    if (nargs >= 3 && qp_str_slice_index(ctx, args[2], &end) < 0)
        return JS_EXCEPTION;
    if (qp_tuple_check(ctx, sub)) {
        n = QP_TUPLE_SIZE(sub);
        for (i = 0; i < n; i++) {
            JSValueConst it = QP_TUPLE_ITEMS(sub)[i];
            if (!qp_str_check(ctx, it))
                return qp_raise_type_error(ctx, "tuple for %s must only contain str, not %.100s",
                                           fname, qp_str_tpname(ctx, it, buf, sizeof(buf)));
            if (qp_sarg_get(ctx, self, &s) < 0)
                return JS_EXCEPTION;
            if (qp_sarg_get(ctx, it, &a) < 0) {
                qp_sarg_free(ctx, &s);
                return JS_EXCEPTION;
            }
            r = qp_sl_tailmatch(s.view, a.view, start, end, magic);
            qp_sarg_free(ctx, &a);
            qp_sarg_free(ctx, &s);
            if (r)
                return JS_TRUE;
        }
        return JS_FALSE;
    }
    if (!qp_str_check(ctx, sub))
        return qp_raise_type_error(ctx, "%s first arg must be str or a tuple of str, not %.100s",
                                   fname, qp_str_tpname(ctx, sub, buf, sizeof(buf)));
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, sub, &a) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    r = qp_sl_tailmatch(s.view, a.view, start, end, magic);
    qp_sarg_free(ctx, &a);
    qp_sarg_free(ctx, &s);
    return qp_bool_new(r);
}

/* ---- splitting ------------------------------------------------------------ */

/* split / rsplit(sep=None, maxsplit=-1); magic 1 = rsplit */
QP_STR_METH(qp_str_m_split)
{
    static const char *const kw[] = { "sep", "maxsplit", NULL };
    QPArgSpec spec = { magic ? "rsplit" : "split", kw, 0, 2, 0 };
    JSValueConst a[2];
    JSValueConst sep = JS_UNDEFINED;
    Py_ssize_t maxsplit = -1;
    QPStrArg s, sp;
    qp_spanvec sv;
    int r, has_sep;
    JSValue res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]))
        sep = a[0];
    if (!QP_IS_NULL(a[1]) && qp_arg_ssize(ctx, a[1], &maxsplit) < 0)
        return JS_EXCEPTION;
    has_sep = !(JS_IsUndefined(sep) || JS_IsNull(sep));
    if (has_sep && !qp_str_check(ctx, sep)) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        return qp_raise_type_error(ctx, "must be str or None, not %.100s",
                                   qp_str_tpname(ctx, sep, buf, sizeof(buf)));
    }
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (has_sep && qp_sarg_get(ctx, sep, &sp) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    qp_spanvec_init(&sv, ctx->rt);
    if (magic)
        r = qp_sl_rsplit(ctx->rt, s.view, has_sep ? &sp.view : NULL, maxsplit, QP_SL_STR, &sv);
    else
        r = qp_sl_split(ctx->rt, s.view, has_sep ? &sp.view : NULL, maxsplit, QP_SL_STR, &sv);
    if (r < 0)
        res = qp_sl_raise(ctx, r, "");
    else
        res = qp_str_spans_to_list(ctx, &s, &sv);
    qp_spanvec_free(&sv);
    if (has_sep)
        qp_sarg_free(ctx, &sp);
    qp_sarg_free(ctx, &s);
    return res;
}

QP_STR_METH(qp_str_m_splitlines)
{
    static const char *const kw[] = { "keepends", NULL };
    QPArgSpec spec = { "splitlines", kw, 0, 1, 0 };
    JSValueConst a[1];
    int keepends = 0, r;
    QPStrArg s;
    qp_spanvec sv;
    JSValue res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0])) {
        keepends = qp_bool(ctx, a[0]);
        if (keepends < 0)
            return JS_EXCEPTION;
    }
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    qp_spanvec_init(&sv, ctx->rt);
    r = qp_sl_splitlines(s.view, keepends, QP_SL_STR, &sv);
    if (r < 0)
        res = qp_sl_raise(ctx, r, "");
    else
        res = qp_str_spans_to_list(ctx, &s, &sv);
    qp_spanvec_free(&sv);
    qp_sarg_free(ctx, &s);
    return res;
}

/* partition / rpartition(sep); magic = direction */
QP_STR_METH(qp_str_m_partition)
{
    JSValueConst sep = args[0];
    QPStrArg s, sp;
    qp_span out[3];
    JSValue items[3];
    int r, i;

    if (!qp_str_check(ctx, sep)) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        return qp_raise_type_error(ctx, "must be str, not %.100s",
                                   qp_str_tpname(ctx, sep, buf, sizeof(buf)));
    }
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, sep, &sp) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    r = qp_sl_partition(ctx->rt, s.view, sp.view, magic, out);
    qp_sarg_free(ctx, &sp);
    if (r < 0) {
        qp_sarg_free(ctx, &s);
        return qp_sl_raise(ctx, r, "");
    }
    if (r == 0) {
        JSValue e1 = qp_str_empty(ctx), e2 = qp_str_empty(ctx);
        if (magic > 0) {
            items[0] = JS_DupValue(ctx, self);
            items[1] = e1;
            items[2] = e2;
        } else {
            items[0] = e1;
            items[1] = e2;
            items[2] = JS_DupValue(ctx, self);
        }
    } else {
        for (i = 0; i < 3; i++) {
            if (i == 1)
                items[i] = JS_DupValue(ctx, sep);
            else
                items[i] = qp_sarg_sub(ctx, &s, out[i].start, out[i].end);
            if (JS_IsException(items[i])) {
                while (i-- > 0)
                    JS_FreeValue(ctx, items[i]);
                qp_sarg_free(ctx, &s);
                return JS_EXCEPTION;
            }
        }
    }
    qp_sarg_free(ctx, &s);
    return qp_tuple_new_steal(ctx, 3, items);
}

/* ---- join ------------------------------------------------------------------ */

/* PyUnicode_Join */
QP_STR_METH(qp_str_m_join)
{
    JSValueConst iterable = args[0];
    JSValue seq, res;
    JSValueConst *items;
    Py_ssize_t n, i;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (qp_list_check_exact(ctx, iterable) || qp_tuple_check_exact(ctx, iterable)) {
        seq = JS_DupValue(ctx, iterable);
    } else {
        JSValue it = qp_iter(ctx, iterable);
        if (JS_IsException(it)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
                return qp_raise_type_error(ctx, "can only join an iterable");
            }
            return JS_EXCEPTION;
        }
        seq = qp_list_from_iterable(ctx, it);
        JS_FreeValue(ctx, it);
        if (JS_IsException(seq))
            return JS_EXCEPTION;
    }
    if (qp_tuple_check_exact(ctx, seq)) {
        n = QP_TUPLE_SIZE(seq);
        items = (JSValueConst *)QP_TUPLE_ITEMS(seq);
    } else {
        items = (JSValueConst *)qp_list_items(seq, &n);
        if (!items) {
            JSValue t = qp_list_as_tuple(ctx, seq);
            JS_FreeValue(ctx, seq);
            if (JS_IsException(t))
                return JS_EXCEPTION;
            seq = t;
            n = QP_TUPLE_SIZE(seq);
            items = (JSValueConst *)QP_TUPLE_ITEMS(seq);
        }
    }
    for (i = 0; i < n; i++) {
        if (!qp_str_check(ctx, items[i])) {
            res = qp_raise_type_error(ctx, "sequence item %zd: expected str instance, %.80s found",
                                      i, qp_str_tpname(ctx, items[i], buf, sizeof(buf)));
            JS_FreeValue(ctx, seq);
            return res;
        }
    }
    if (n > INT_MAX) {
        JS_FreeValue(ctx, seq);
        return qp_str_too_long(ctx);
    }
    if (n == 1) {
        res = qp_str_value(ctx, items[0]);
    } else {
        res = qp_str_join_array(ctx, self, (int)n, items);
    }
    JS_FreeValue(ctx, seq);
    return res;
}

/* ---- replace / strip / pad ---------------------------------------------- */

QP_STR_METH(qp_str_m_replace)
{
    static const char *const kw[] = { "", "", "count", NULL };
    QPArgSpec spec = { "replace", kw, 2, 3, 0 };
    JSValueConst a[3];
    Py_ssize_t count = -1;
    QPStrArg s, o, nw;
    qp_ucsbuilder b;
    JSValue res;
    int r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_str_bad_arg(ctx, "replace", "argument 1", "str", a[0]);
    if (!qp_str_check(ctx, a[1]))
        return qp_str_bad_arg(ctx, "replace", "argument 2", "str", a[1]);
    if (!QP_IS_NULL(a[2]) && qp_arg_ssize(ctx, a[2], &count) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, a[0], &o) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    if (qp_sarg_get(ctx, a[1], &nw) < 0) {
        qp_sarg_free(ctx, &o);
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    qp_ucsbuilder_init(&b, ctx->rt);
    r = qp_sl_replace(s.view, o.view, nw.view, count, &b);
    res = qp_str_build_result(ctx, &s, r, &b, "replace string is too long");
    qp_sarg_free(ctx, &nw);
    qp_sarg_free(ctx, &o);
    qp_sarg_free(ctx, &s);
    return res;
}

/* strip / lstrip / rstrip(chars=None); magic = QP_SL_*STRIP */
QP_STR_METH(qp_str_m_strip)
{
    static const char *const names[] = { "lstrip", "rstrip", "strip" };
    const char *fname = names[magic];
    QPStrArg s, c;
    qp_span sp;
    JSValue res;

    if (qp_arg_check_positional(ctx, fname, nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 1 && !JS_IsUndefined(args[0]) && !JS_IsNull(args[0]) && !qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "%s arg must be None or str", fname);
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (nargs == 1 && qp_str_check(ctx, args[0])) {
        if (qp_sarg_get(ctx, args[0], &c) < 0) {
            qp_sarg_free(ctx, &s);
            return JS_EXCEPTION;
        }
        qp_sl_strip_chars(s.view, c.view, magic, &sp);
        qp_sarg_free(ctx, &c);
    } else {
        qp_sl_strip_ws(s.view, magic, QP_SL_STR, &sp);
    }
    res = qp_sarg_sub(ctx, &s, sp.start, sp.end);
    qp_sarg_free(ctx, &s);
    return res;
}

enum { QP_PAD_CENTER, QP_PAD_LJUST, QP_PAD_RJUST };

/* center / ljust / rjust(width, fillchar=' ') */
QP_STR_METH(qp_str_m_pad)
{
    static const char *const names[] = { "center", "ljust", "rjust" };
    Py_ssize_t width;
    Py_UCS4 fill = ' ';
    QPStrArg s;
    qp_ucsbuilder b;
    JSValue res;
    int r;

    if (qp_arg_check_positional(ctx, names[magic], nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_arg_ssize(ctx, args[0], &width) < 0)
        return JS_EXCEPTION;
    if (nargs == 2 && qp_str_convert_uc(ctx, args[1], &fill) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    qp_ucsbuilder_init(&b, ctx->rt);
    if (magic == QP_PAD_CENTER)
        r = qp_sl_center(s.view, width, fill, &b);
    else if (magic == QP_PAD_LJUST)
        r = qp_sl_ljust(s.view, width, fill, &b);
    else
        r = qp_sl_rjust(s.view, width, fill, &b);
    res = qp_str_build_result(ctx, &s, r, &b, "padded string is too long");
    qp_sarg_free(ctx, &s);
    return res;
}

QP_STR_METH(qp_str_m_zfill)
{
    Py_ssize_t width;
    QPStrArg s;
    qp_ucsbuilder b;
    JSValue res;
    int r;

    if (qp_arg_ssize(ctx, args[0], &width) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    qp_ucsbuilder_init(&b, ctx->rt);
    r = qp_sl_zfill(s.view, width, &b);
    res = qp_str_build_result(ctx, &s, r, &b, "padded string is too long");
    qp_sarg_free(ctx, &s);
    return res;
}

QP_STR_METH(qp_str_m_expandtabs)
{
    static const char *const kw[] = { "tabsize", NULL };
    QPArgSpec spec = { "expandtabs", kw, 0, 1, 0 };
    JSValueConst a[1];
    int tabsize = 8, r;
    QPStrArg s;
    qp_ucsbuilder b;
    JSValue res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_str_arg_int(ctx, a[0], &tabsize) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    qp_ucsbuilder_init(&b, ctx->rt);
    r = qp_sl_expandtabs(s.view, tabsize, &b);
    res = qp_str_build_result(ctx, &s, r, &b, "new string is too long");
    qp_sarg_free(ctx, &s);
    return res;
}

/* removeprefix / removesuffix(affix); magic = direction (-1 prefix) */
QP_STR_METH(qp_str_m_removeaffix)
{
    const char *fname = magic < 0 ? "removeprefix" : "removesuffix";
    QPStrArg s, a;
    JSValue res;
    int match;

    if (!qp_str_check(ctx, args[0]))
        return qp_str_bad_arg(ctx, fname, "argument", "str", args[0]);
    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (qp_sarg_get(ctx, args[0], &a) < 0) {
        qp_sarg_free(ctx, &s);
        return JS_EXCEPTION;
    }
    match = qp_sl_tailmatch(s.view, a.view, 0, PY_SSIZE_T_MAX, magic);
    if (!match)
        res = JS_DupValue(ctx, s.v);
    else if (magic < 0)
        res = qp_sarg_sub(ctx, &s, a.view.len, s.view.len);
    else
        res = qp_sarg_sub(ctx, &s, 0, s.view.len - a.view.len);
    qp_sarg_free(ctx, &a);
    qp_sarg_free(ctx, &s);
    return res;
}

/* ---- maketrans / translate -------------------------------------------- */

/* str.maketrans(x[, y[, z]]) (static method) */
QP_STR_METH(qp_str_m_maketrans)
{
    JSValueConst x;
    JSValue nd, key, value;
    Py_ssize_t i;
    int res;

    if (qp_arg_check_positional(ctx, "maketrans", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    x = args[0];
    if (nargs >= 2 && !qp_str_check(ctx, args[1]))
        return qp_str_bad_arg(ctx, "maketrans", "argument 2", "str", args[1]);
    if (nargs >= 3 && !qp_str_check(ctx, args[2]))
        return qp_str_bad_arg(ctx, "maketrans", "argument 3", "str", args[2]);
    nd = qp_dict_new(ctx);
    if (JS_IsException(nd))
        return JS_EXCEPTION;
    if (nargs >= 2) {
        qp_strview vx, vy, vz;
        if (!qp_str_check(ctx, x)) {
            qp_raise_type_error(ctx, "first maketrans argument must be a string if there is a second argument");
            goto err;
        }
        if (qp_str_view_acquire(ctx, x, &vx) < 0)
            goto err;
        if (qp_str_view_acquire(ctx, args[1], &vy) < 0) {
            qp_str_view_release(ctx, &vx);
            goto err;
        }
        if (vx.len != vy.len) {
            qp_str_view_release(ctx, &vy);
            qp_str_view_release(ctx, &vx);
            qp_raise_value_error(ctx, "the first two maketrans arguments must have equal length");
            goto err;
        }
        res = 0;
        for (i = 0; i < vx.len && res >= 0; i++) {
            key = JS_NewInt32(ctx, QP_SL_READ(vx.kind, vx.data, i));
            value = JS_NewInt32(ctx, QP_SL_READ(vy.kind, vy.data, i));
            res = qp_dict_set_item(ctx, nd, key, value);
        }
        qp_str_view_release(ctx, &vy);
        qp_str_view_release(ctx, &vx);
        if (res < 0)
            goto err;
        if (nargs >= 3) {
            if (qp_str_view_acquire(ctx, args[2], &vz) < 0)
                goto err;
            for (i = 0; i < vz.len && res >= 0; i++) {
                key = JS_NewInt32(ctx, QP_SL_READ(vz.kind, vz.data, i));
                res = qp_dict_set_item(ctx, nd, key, JS_UNDEFINED);
            }
            qp_str_view_release(ctx, &vz);
            if (res < 0)
                goto err;
        }
    } else {
        QPType *t = qp_type_of(ctx, x);
        Py_ssize_t pos = 0;
        JSValue k, v;
        if (t != QP_TYPE(ctx, dict) && t != QP_TYPE(ctx, frozendict)) {
            qp_raise_type_error(ctx, "if you give only one argument to maketrans it must be a dict");
            goto err;
        }
        while (qp_dict_next(x, &pos, &k, &v)) {
            if (qp_str_check(ctx, k)) {
                qp_strview vk;
                Py_UCS4 ch;
                if (qp_str_view_acquire(ctx, k, &vk) < 0)
                    goto err;
                if (vk.len != 1) {
                    qp_str_view_release(ctx, &vk);
                    /* CPython's message lacks the space ("translate" "table") */
                    qp_raise_value_error(ctx, "string keys in translatetable must be of length 1");
                    goto err;
                }
                ch = QP_SL_READ(vk.kind, vk.data, 0);
                qp_str_view_release(ctx, &vk);
                if (qp_dict_set_item(ctx, nd, JS_NewInt32(ctx, ch), v) < 0)
                    goto err;
            } else if (qp_long_check(ctx, k)) {
                if (qp_dict_set_item(ctx, nd, k, v) < 0)
                    goto err;
            } else {
                qp_raise_type_error(ctx, "keys in translate table mustbe strings or integers");
                goto err;
            }
        }
    }
    return nd;
err:
    JS_FreeValue(ctx, nd);
    return JS_EXCEPTION;
}

/* charmaptranslate_lookup: *result = QP_NULL (no mapping), None, int (with
   *replace) or str (new refs).  0 / -1. */
static int qp_charmap_lookup(JSContext *ctx, Py_UCS4 c, JSValueConst mapping, JSValue *result,
                             Py_UCS4 *replace)
{
    JSValue w = JS_NewInt32(ctx, (int32_t)c), x;
    int64_t value;
    int overflow = 0;

    *result = QP_NULL;
    if (qp_dict_check_exact(ctx, mapping)) {
        int rc = qp_dict_get_item(ctx, mapping, w, &x);
        if (rc < 0)
            return -1;
        if (rc == 0)
            return 0;
    } else {
        x = qp_getitem(ctx, mapping, w);
        if (JS_IsException(x)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, LookupError))) {
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
                return 0;
            }
            return -1;
        }
    }
    if (JS_IsUndefined(x) || JS_IsNull(x)) {
        *result = JS_UNDEFINED;
        return 0;
    }
    if (qp_long_check(ctx, x)) {
        JSValue iv = qp_long_value(ctx, x);
        if (JS_IsException(iv)) {
            JS_FreeValue(ctx, x);
            return -1;
        }
        qp_long_as_int64_overflow(iv, &value, &overflow);
        JS_FreeValue(ctx, iv);
        if (overflow || value < LONG_MIN || value > LONG_MAX) {
            JS_FreeValue(ctx, x);
            qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
            return -1;
        }
        if (value < 0 || value > QP_MAX_UNICODE) {
            JS_FreeValue(ctx, x);
            qp_raise_value_error(ctx, "character mapping must be in range(0x%lx)",
                                 (unsigned long)QP_MAX_UNICODE + 1);
            return -1;
        }
        *result = x;
        *replace = (Py_UCS4)value;
        return 0;
    }
    if (qp_str_check(ctx, x)) {
        *result = x;
        return 0;
    }
    JS_FreeValue(ctx, x);
    qp_raise_type_error(ctx, "character mapping must return integer, None or str");
    return -1;
}

/* _PyUnicode_TranslateCharmap(self, table, "ignore") with the ASCII fast
   path (unicode_fast_translate): each distinct ASCII character is looked up
   once while every mapping found is ASCII-compatible. */
QP_STR_METH(qp_str_m_translate)
{
    JSValueConst mapping = args[0];
    QPStrArg s;
    qp_ucsbuilder b;
    Py_ssize_t i = 0;
    JSValue item;
    Py_UCS4 replace = 0;

    if (qp_sarg_get(ctx, self, &s) < 0)
        return JS_EXCEPTION;
    if (s.view.len == 0) {
        qp_sarg_free(ctx, &s);
        return qp_str_empty(ctx);
    }
    qp_ucsbuilder_init(&b, ctx->rt);
    if (s.view.kind == 1 && qp_str_predicate(s.view, QP_PRED_ISASCII)) {
        uint8_t table[128];
        const uint8_t *in = s.view.data;   /* ASCII views are kind 1 */
        memset(table, 0xff, sizeof(table));
        for (i = 0; i < s.view.len; i++) {
            uint8_t ch = in[i], ch2 = table[ch];
            if (ch2 == 0xff) {
                int fast = 0;
                if (qp_charmap_lookup(ctx, ch, mapping, &item, &replace) < 0)
                    goto fail;
                if (QP_IS_NULL(item)) {
                    table[ch] = ch;
                    fast = 1;
                } else if (JS_IsUndefined(item)) {
                    table[ch] = 0xfe;
                    fast = 1;
                } else if (!qp_str_check(ctx, item)) {
                    if (replace <= 127) {
                        table[ch] = (uint8_t)replace;
                        fast = 1;
                    }
                } else {
                    qp_strview iv;
                    if (qp_str_view_acquire(ctx, item, &iv) < 0) {
                        JS_FreeValue(ctx, item);
                        goto fail;
                    }
                    if (iv.len == 1 && QP_SL_READ(iv.kind, iv.data, 0) <= 127) {
                        table[ch] = (uint8_t)QP_SL_READ(iv.kind, iv.data, 0);
                        fast = 1;
                    }
                    qp_str_view_release(ctx, &iv);
                }
                JS_FreeValue(ctx, item);
                if (!fast)
                    break;
                ch2 = table[ch];
            }
            if (ch2 == 0xfe)
                continue;
            if (qp_ucsbuilder_putc(&b, ch2) < 0)
                goto nomem;
        }
    }
    for (; i < s.view.len; i++) {
        Py_UCS4 ch = QP_SL_READ(s.view.kind, s.view.data, i);
        if (qp_charmap_lookup(ctx, ch, mapping, &item, &replace) < 0)
            goto fail;
        if (QP_IS_NULL(item)) {
            if (qp_ucsbuilder_putc(&b, ch) < 0)
                goto nomem;
        } else if (JS_IsUndefined(item)) {
            /* deleted ('ignore') */
        } else if (!qp_str_check(ctx, item)) {
            if (qp_ucsbuilder_putc(&b, replace) < 0)
                goto nomem;
        } else {
            qp_strview iv;
            int r;
            if (qp_str_view_acquire(ctx, item, &iv) < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            r = qp_ucsbuilder_write(&b, iv, 0, iv.len);
            qp_str_view_release(ctx, &iv);
            if (r < 0) {
                JS_FreeValue(ctx, item);
                goto nomem;
            }
        }
        JS_FreeValue(ctx, item);
    }
    qp_sarg_free(ctx, &s);
    return qp_str_from_builder(ctx, &b);
nomem:
    qp_raise_memory_error(ctx);
fail:
    qp_ucsbuilder_free(&b);
    qp_sarg_free(ctx, &s);
    return JS_EXCEPTION;
}

/* ---- misc ----------------------------------------------------------------- */

/* __sizeof__: CPython's compact-object sizes on 64-bit hosts. */
QP_STR_METH(qp_str_m_sizeof)
{
    qp_strview v;
    Py_UCS4 maxc;
    Py_ssize_t size;
    int kind;

    if (qp_str_view_acquire(ctx, self, &v) < 0)
        return JS_EXCEPTION;
    maxc = 0;
    {
        Py_ssize_t i;
        for (i = 0; i < v.len; i++) {
            Py_UCS4 c = QP_SL_READ(v.kind, v.data, i);
            if (c > maxc)
                maxc = c;
        }
    }
    kind = maxc < 0x100 ? 1 : maxc < 0x10000 ? 2 : 4;
    if (!qp_str_check_exact(self))
        size = 64 + (v.len + 1) * kind;         /* PyUnicodeObject + data */
    else if (maxc < 0x80)
        size = 40 + v.len + 1;                  /* PyASCIIObject */
    else
        size = 56 + (v.len + 1) * kind;         /* PyCompactUnicodeObject */
    qp_str_view_release(ctx, &v);
    return JS_NewInt64(ctx, size);
}

QP_STR_METH(qp_str_m_getnewargs)
{
    JSValue v = qp_str_value(ctx, self);
    return qp_tuple_new_steal(ctx, 1, &v);
}

/* ========================================================================= */
/* Codecs (utf-8, utf-8-sig, utf-16*, utf-32*, latin-1, ascii)                */
/* ========================================================================= */

/* Built-in fast paths (CPython PyUnicode_AsEncodedString/PyUnicode_Decode
   shortcuts, extended to all aliases of these codecs).  Any other encoding,
   or an error handler other than the 8 builtin ones, is delegated to the
   codec registry (python/mod_codecs.c), which raises LookupError("unknown
   encoding: %s") / ("unknown error handler name '%s'") as CPython does. */
enum {
    QP_ENC_UTF8, QP_ENC_UTF8_SIG, QP_ENC_UTF16, QP_ENC_UTF16_LE, QP_ENC_UTF16_BE,
    QP_ENC_UTF32, QP_ENC_UTF32_LE, QP_ENC_UTF32_BE, QP_ENC_LATIN1, QP_ENC_ASCII,
};

/* encodings.aliases entries for these codecs, plus the module names */
static const struct { const char *name; uint8_t codec; } qp_str_codec_aliases[] = {
    { "646", QP_ENC_ASCII }, { "ansi_x3.4_1968", QP_ENC_ASCII }, { "ansi_x3_4_1968", QP_ENC_ASCII },
    { "ansi_x3.4_1986", QP_ENC_ASCII }, { "cp367", QP_ENC_ASCII }, { "csascii", QP_ENC_ASCII },
    { "ibm367", QP_ENC_ASCII }, { "iso646_us", QP_ENC_ASCII }, { "iso_646.irv_1991", QP_ENC_ASCII },
    { "iso_ir_6", QP_ENC_ASCII }, { "us", QP_ENC_ASCII }, { "us_ascii", QP_ENC_ASCII },
    { "ascii", QP_ENC_ASCII },
    { "8859", QP_ENC_LATIN1 }, { "cp819", QP_ENC_LATIN1 }, { "csisolatin1", QP_ENC_LATIN1 },
    { "ibm819", QP_ENC_LATIN1 }, { "iso8859", QP_ENC_LATIN1 }, { "iso8859_1", QP_ENC_LATIN1 },
    { "iso_8859_1", QP_ENC_LATIN1 }, { "iso_8859_1_1987", QP_ENC_LATIN1 },
    { "iso_ir_100", QP_ENC_LATIN1 }, { "l1", QP_ENC_LATIN1 }, { "latin", QP_ENC_LATIN1 },
    { "latin1", QP_ENC_LATIN1 }, { "latin_1", QP_ENC_LATIN1 },
    { "csutf16", QP_ENC_UTF16 }, { "u16", QP_ENC_UTF16 }, { "utf16", QP_ENC_UTF16 },
    { "utf_16", QP_ENC_UTF16 },
    { "csutf16be", QP_ENC_UTF16_BE }, { "unicodebigunmarked", QP_ENC_UTF16_BE },
    { "utf_16be", QP_ENC_UTF16_BE }, { "utf_16_be", QP_ENC_UTF16_BE },
    { "csutf16le", QP_ENC_UTF16_LE }, { "unicodelittleunmarked", QP_ENC_UTF16_LE },
    { "utf_16le", QP_ENC_UTF16_LE }, { "utf_16_le", QP_ENC_UTF16_LE },
    { "csutf32", QP_ENC_UTF32 }, { "u32", QP_ENC_UTF32 }, { "utf32", QP_ENC_UTF32 },
    { "utf_32", QP_ENC_UTF32 },
    { "csutf32be", QP_ENC_UTF32_BE }, { "utf_32be", QP_ENC_UTF32_BE }, { "utf_32_be", QP_ENC_UTF32_BE },
    { "csutf32le", QP_ENC_UTF32_LE }, { "utf_32le", QP_ENC_UTF32_LE }, { "utf_32_le", QP_ENC_UTF32_LE },
    { "csutf8", QP_ENC_UTF8 }, { "u8", QP_ENC_UTF8 }, { "utf", QP_ENC_UTF8 }, { "utf8", QP_ENC_UTF8 },
    { "utf8_ucs2", QP_ENC_UTF8 }, { "utf8_ucs4", QP_ENC_UTF8 }, { "cp65001", QP_ENC_UTF8 },
    { "utf_8", QP_ENC_UTF8 },
    { "utf8_sig", QP_ENC_UTF8_SIG }, { "utf_8_sig", QP_ENC_UTF8_SIG },
};

static int qp_str_codec_find(const char *norm)
{
    size_t i;
    for (i = 0; i < countof(qp_str_codec_aliases); i++) {
        if (!strcmp(qp_str_codec_aliases[i].name, norm))
            return qp_str_codec_aliases[i].codec;
    }
    return -1;
}

/* Fast path of codecs.lookup(encoding) for the codecs implemented here:
   normalizestring() then encodings.normalize_encoding() and the alias /
   module search.  Returns QP_ENC_* or -1 (no exception) when the codec must
   come from the codec registry (python/mod_codecs.c). */
static int qp_str_codec_lookup(const char *encoding)
{
    char norm[64], alt[64];
    size_t n = 0, i;
    int punct = 0, c;

    for (i = 0; encoding[i]; i++) {
        unsigned char ch = encoding[i];
        if (ch == ' ')
            ch = '-';
        else
            ch = QP_TOLOWER(ch);
        if (QP_ISALNUM(ch) || ch == '.') {
            if (punct && n > 0) {
                if (n >= sizeof(norm) - 1)
                    goto unknown;
                norm[n++] = '_';
            }
            punct = 0;
            if (n >= sizeof(norm) - 1)
                goto unknown;
            norm[n++] = ch;
        } else {
            punct = 1;
        }
    }
    norm[n] = '\0';
    c = qp_str_codec_find(norm);
    if (c >= 0)
        return c;
    memcpy(alt, norm, n + 1);
    for (i = 0; i < n; i++) {
        if (alt[i] == '.')
            alt[i] = '_';
    }
    return qp_str_codec_find(alt);
unknown:
    return -1;
}

enum {
    QP_EH_STRICT, QP_EH_IGNORE, QP_EH_REPLACE, QP_EH_BACKSLASHREPLACE,
    QP_EH_XMLCHARREFREPLACE, QP_EH_NAMEREPLACE, QP_EH_SURROGATEESCAPE, QP_EH_SURROGATEPASS,
};

/* QP_EH_* of a builtin error handler name (NULL = strict), or -1.  Other
   names are resolved by the codec registry (python/mod_codecs.c), which
   the encode/decode entry points delegate to. */
static int qp_error_handler_builtin(const char *errors)
{
    static const char *const names[] = {
        "strict", "ignore", "replace", "backslashreplace", "xmlcharrefreplace",
        "namereplace", "surrogateescape", "surrogatepass",
    };
    int i;
    if (!errors)
        return QP_EH_STRICT;
    for (i = 0; i < (int)countof(names); i++) {
        if (!strcmp(errors, names[i]))
            return i;
    }
    return -1;
}

/* ---- encoding ------------------------------------------------------------- */

typedef struct QPEncoder {
    JSContext *ctx;
    int codec;              /* QP_ENC_* */
    const char *encoding;   /* name used in UnicodeEncodeError */
    const char *errors;
    int eh;                 /* QP_EH_* */
    JSValueConst str;       /* exact str being encoded */
    qp_strview v;
    int le;                 /* utf-16/32 byte order */
    DynBuf out;
} QPEncoder;

static void qp_enc_put_cp(QPEncoder *e, Py_UCS4 ch)
{
    DynBuf *b = &e->out;
    uint8_t t[4];

    switch (e->codec) {
    case QP_ENC_UTF8:
    case QP_ENC_UTF8_SIG:
        if (ch < 0x80) {
            dbuf_putc(b, ch);
        } else if (ch < 0x800) {
            t[0] = 0xC0 | (ch >> 6);
            t[1] = 0x80 | (ch & 0x3F);
            dbuf_put(b, t, 2);
        } else if (ch < 0x10000) {
            t[0] = 0xE0 | (ch >> 12);
            t[1] = 0x80 | ((ch >> 6) & 0x3F);
            t[2] = 0x80 | (ch & 0x3F);
            dbuf_put(b, t, 3);
        } else {
            t[0] = 0xF0 | (ch >> 18);
            t[1] = 0x80 | ((ch >> 12) & 0x3F);
            t[2] = 0x80 | ((ch >> 6) & 0x3F);
            t[3] = 0x80 | (ch & 0x3F);
            dbuf_put(b, t, 4);
        }
        break;
    case QP_ENC_UTF16:
    case QP_ENC_UTF16_LE:
    case QP_ENC_UTF16_BE:
        if (ch >= 0x10000) {
            Py_UCS4 c = ch - 0x10000;
            {
                uint16_t u1 = 0xD800 + (c >> 10), u2 = 0xDC00 + (c & 0x3FF);
                if (e->le) {
                    t[0] = u1 & 0xFF; t[1] = u1 >> 8; t[2] = u2 & 0xFF; t[3] = u2 >> 8;
                } else {
                    t[0] = u1 >> 8; t[1] = u1 & 0xFF; t[2] = u2 >> 8; t[3] = u2 & 0xFF;
                }
                dbuf_put(b, t, 4);
            }
        } else {
            if (e->le) {
                t[0] = ch & 0xFF; t[1] = ch >> 8;
            } else {
                t[0] = ch >> 8; t[1] = ch & 0xFF;
            }
            dbuf_put(b, t, 2);
        }
        break;
    case QP_ENC_UTF32:
    case QP_ENC_UTF32_LE:
    case QP_ENC_UTF32_BE:
        if (e->le) {
            t[0] = ch & 0xFF; t[1] = (ch >> 8) & 0xFF; t[2] = (ch >> 16) & 0xFF; t[3] = ch >> 24;
        } else {
            t[0] = ch >> 24; t[1] = (ch >> 16) & 0xFF; t[2] = (ch >> 8) & 0xFF; t[3] = ch & 0xFF;
        }
        dbuf_put(b, t, 4);
        break;
    default:
        dbuf_putc(b, (uint8_t)ch);
        break;
    }
}

static void qp_enc_put_ascii(QPEncoder *e, const char *s)
{
    while (*s)
        qp_enc_put_cp(e, (uint8_t)*s++);
}

static BOOL qp_enc_encodable(QPEncoder *e, Py_UCS4 ch)
{
    switch (e->codec) {
    case QP_ENC_LATIN1: return ch < 0x100;
    case QP_ENC_ASCII:  return ch < 0x80;
    default:            return !QP_IS_SURR(ch);
    }
}

static int qp_enc_raise(QPEncoder *e, Py_ssize_t start, Py_ssize_t end, const char *reason)
{
    qp_raise_unicode_encode(e->ctx, e->encoding, e->str, start, end, reason);
    return -1;
}

/* Error handler for the unencodable run v[start:end]; *newpos = where to
   continue. */
static int qp_enc_error(QPEncoder *e, Py_ssize_t start, Py_ssize_t end, const char *reason,
                        Py_ssize_t *newpos)
{
    Py_ssize_t i;
    char buf[32];

    *newpos = end;
    switch (e->eh) {
    case QP_EH_STRICT:
        return qp_enc_raise(e, start, end, reason);
    case QP_EH_IGNORE:
        return 0;
    case QP_EH_REPLACE:
        for (i = start; i < end; i++)
            qp_enc_put_cp(e, '?');
        return 0;
    case QP_EH_BACKSLASHREPLACE:
    case QP_EH_NAMEREPLACE:
        for (i = start; i < end; i++) {
            Py_UCS4 ch = QP_SL_READ(e->v.kind, e->v.data, i);
            if (e->eh == QP_EH_NAMEREPLACE) {
                char name[NAME_MAXLEN_QP];
                if (qp_unicode_get_name(ch, name, sizeof(name))) {
                    qp_enc_put_ascii(e, "\\N{");
                    qp_enc_put_ascii(e, name);
                    qp_enc_put_ascii(e, "}");
                    continue;
                }
            }
            if (ch < 0x100)
                snprintf(buf, sizeof(buf), "\\x%02x", (unsigned)ch);
            else if (ch < 0x10000)
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)ch);
            else
                snprintf(buf, sizeof(buf), "\\U%08x", (unsigned)ch);
            qp_enc_put_ascii(e, buf);
        }
        return 0;
    case QP_EH_XMLCHARREFREPLACE:
        for (i = start; i < end; i++) {
            snprintf(buf, sizeof(buf), "&#%u;", (unsigned)QP_SL_READ(e->v.kind, e->v.data, i));
            qp_enc_put_ascii(e, buf);
        }
        return 0;
    case QP_EH_SURROGATEESCAPE:
        if (e->codec >= QP_ENC_UTF16 && e->codec <= QP_ENC_UTF32_BE) {
            /* the handler returns bytes of length 1 per character, which
               the UTF-16/32 encoders only accept in whole code units */
            int unit = e->codec <= QP_ENC_UTF16_BE ? 2 : 4;
            for (i = start; i < end; i++) {
                Py_UCS4 ch = QP_SL_READ(e->v.kind, e->v.data, i);
                if (ch < 0xDC80 || ch > 0xDCFF)
                    return qp_enc_raise(e, start, end, reason);
            }
            if ((end - start) % unit)
                return qp_enc_raise(e, start, end, "surrogates not allowed");
            for (i = start; i < end; i++)
                dbuf_putc(&e->out, (uint8_t)(QP_SL_READ(e->v.kind, e->v.data, i) - 0xDC00));
            return 0;
        }
        for (i = start; i < end; i++) {
            Py_UCS4 ch = QP_SL_READ(e->v.kind, e->v.data, i);
            if (ch < 0xDC80 || ch > 0xDCFF)
                return qp_enc_raise(e, i, end, reason);
            dbuf_putc(&e->out, (uint8_t)(ch - 0xDC00));
        }
        return 0;
    default: /* surrogatepass */
        if (e->codec == QP_ENC_LATIN1 || e->codec == QP_ENC_ASCII)
            return qp_enc_raise(e, start, end, reason);
        for (i = start; i < end; i++) {
            if (!QP_IS_SURR(QP_SL_READ(e->v.kind, e->v.data, i)))
                return qp_enc_raise(e, start, end, reason);
        }
        for (i = start; i < end; i++)
            qp_enc_put_cp(e, QP_SL_READ(e->v.kind, e->v.data, i));
        return 0;
    }
}

static JSValue qp_str_encode(JSContext *ctx, JSValueConst s, const char *encoding, const char *errors)
{
    QPEncoder e;
    Py_ssize_t pos, end, newpos;
    const char *reason;
    JSValue res;
    int codec, eh;

    if (qp_codec_check_encoding_errors(ctx, encoding, errors) < 0)
        return JS_EXCEPTION;
    codec = encoding ? qp_str_codec_lookup(encoding) : QP_ENC_UTF8;
    eh = qp_error_handler_builtin(errors);
    if (codec < 0 || eh < 0) {
        /* PyUnicode_AsEncodedString: other codecs / error handlers go
           through the codec registry */
        return qp_codec_encode_text(ctx, qp_str_unbox(s), encoding ? encoding : "utf-8", errors);
    }
    memset(&e, 0, sizeof(e));
    e.ctx = ctx;
    e.codec = codec;
    e.errors = errors;
    e.eh = eh;
    e.str = qp_str_unbox(s);
    e.le = 1;
    switch (codec) {
    case QP_ENC_UTF8: case QP_ENC_UTF8_SIG: e.encoding = "utf-8"; break;
    case QP_ENC_UTF16: e.encoding = "utf-16"; break;
    case QP_ENC_UTF16_LE: e.encoding = "utf-16-le"; break;
    case QP_ENC_UTF16_BE: e.encoding = "utf-16-be"; e.le = 0; break;
    case QP_ENC_UTF32: e.encoding = "utf-32"; break;
    case QP_ENC_UTF32_LE: e.encoding = "utf-32-le"; break;
    case QP_ENC_UTF32_BE: e.encoding = "utf-32-be"; e.le = 0; break;
    case QP_ENC_LATIN1: e.encoding = "latin-1"; break;
    default: e.encoding = "ascii"; break;
    }
    reason = codec == QP_ENC_LATIN1 ? "ordinal not in range(256)"
           : codec == QP_ENC_ASCII ? "ordinal not in range(128)" : "surrogates not allowed";
    if (qp_str_view_acquire(ctx, e.str, &e.v) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &e.out);
    if (codec == QP_ENC_UTF8_SIG)
        dbuf_put(&e.out, (const uint8_t *)"\xEF\xBB\xBF", 3);
    else if (codec == QP_ENC_UTF16)
        dbuf_put(&e.out, (const uint8_t *)"\xFF\xFE", 2);
    else if (codec == QP_ENC_UTF32)
        dbuf_put(&e.out, (const uint8_t *)"\xFF\xFE\x00\x00", 4);
    if (e.v.kind == 1 && (codec == QP_ENC_LATIN1 ||
                          ((codec == QP_ENC_UTF8 || codec == QP_ENC_UTF8_SIG || codec == QP_ENC_ASCII) &&
                           qp_str_predicate(e.v, QP_PRED_ISASCII)))) {
        dbuf_put(&e.out, e.v.data, e.v.len);
    } else {
        for (pos = 0; pos < e.v.len;) {
            Py_UCS4 ch = QP_SL_READ(e.v.kind, e.v.data, pos);
            if (qp_enc_encodable(&e, ch)) {
                qp_enc_put_cp(&e, ch);
                pos++;
                continue;
            }
            end = pos + 1;
            while (end < e.v.len && !qp_enc_encodable(&e, QP_SL_READ(e.v.kind, e.v.data, end)))
                end++;
            if (qp_enc_error(&e, pos, end, reason, &newpos) < 0)
                goto fail;
            pos = newpos;
        }
    }
    if (e.out.error) {
        qp_raise_memory_error(ctx);
        goto fail;
    }
    res = qp_bytes_new(ctx, e.out.buf, e.out.size);
    dbuf_free(&e.out);
    qp_str_view_release(ctx, &e.v);
    return res;
fail:
    dbuf_free(&e.out);
    qp_str_view_release(ctx, &e.v);
    return JS_EXCEPTION;
}

/* ---- decoding ------------------------------------------------------------- */

typedef struct QPDecoder {
    JSContext *ctx;
    int codec;
    const char *encoding;   /* name used in UnicodeDecodeError */
    const char *errors;
    int eh;
    const uint8_t *data;    /* the object of the exception */
    Py_ssize_t len;
    int le;
    qp_ucsbuilder out;
} QPDecoder;

static int qp_dec_raise(QPDecoder *d, Py_ssize_t start, Py_ssize_t end, const char *reason)
{
    qp_raise_unicode_decode(d->ctx, d->encoding, (const char *)d->data, d->len, start, end, reason);
    return -1;
}

static int qp_dec_putc(QPDecoder *d, Py_UCS4 ch)
{
    if (qp_ucsbuilder_putc(&d->out, ch) < 0) {
        qp_raise_memory_error(d->ctx);
        return -1;
    }
    return 0;
}

/* unicode_decode_call_errorhandler_writer with the builtin handlers. */
static int qp_dec_error(QPDecoder *d, Py_ssize_t start, Py_ssize_t end, const char *reason,
                        Py_ssize_t *newpos)
{
    Py_ssize_t i;
    const uint8_t *p = d->data;

    *newpos = end;
    switch (d->eh) {
    case QP_EH_STRICT:
        return qp_dec_raise(d, start, end, reason);
    case QP_EH_IGNORE:
        return 0;
    case QP_EH_REPLACE:
        return qp_dec_putc(d, 0xFFFD);
    case QP_EH_BACKSLASHREPLACE:
        for (i = start; i < end; i++) {
            if (qp_dec_putc(d, '\\') || qp_dec_putc(d, 'x') ||
                qp_dec_putc(d, qp_hexdigits[p[i] >> 4]) || qp_dec_putc(d, qp_hexdigits[p[i] & 0xF]))
                return -1;
        }
        return 0;
    case QP_EH_XMLCHARREFREPLACE:
    case QP_EH_NAMEREPLACE:
        qp_raise_type_error(d->ctx, "don't know how to handle UnicodeDecodeError in error callback");
        return -1;
    case QP_EH_SURROGATEESCAPE: {
        Py_ssize_t consumed = 0;
        while (consumed < 4 && consumed < end - start) {
            uint8_t c = p[start + consumed];
            if (c < 128)
                break;
            if (qp_dec_putc(d, 0xDC00 + c))
                return -1;
            consumed++;
        }
        if (consumed == 0)
            return qp_dec_raise(d, start, end, reason);
        *newpos = start + consumed;
        return 0;
    }
    default: { /* surrogatepass */
        Py_UCS4 ch = 0;
        int bytelength = 0;
        switch (d->codec) {
        case QP_ENC_UTF8:
        case QP_ENC_UTF8_SIG:
            if (start + 3 <= d->len && (p[start] & 0xf0) == 0xe0 &&
                (p[start + 1] & 0xc0) == 0x80 && (p[start + 2] & 0xc0) == 0x80) {
                ch = ((p[start] & 0x0f) << 12) + ((p[start + 1] & 0x3f) << 6) + (p[start + 2] & 0x3f);
                bytelength = 3;
            }
            break;
        case QP_ENC_UTF16: case QP_ENC_UTF16_LE: case QP_ENC_UTF16_BE:
            if (start + 2 <= d->len) {
                ch = d->le ? p[start] | (p[start + 1] << 8) : (p[start] << 8) | p[start + 1];
                bytelength = 2;
            }
            break;
        case QP_ENC_UTF32: case QP_ENC_UTF32_LE: case QP_ENC_UTF32_BE:
            if (start + 4 <= d->len) {
                ch = d->le ? p[start] | (p[start + 1] << 8) | (p[start + 2] << 16) | ((Py_UCS4)p[start + 3] << 24)
                           : ((Py_UCS4)p[start] << 24) | (p[start + 1] << 16) | (p[start + 2] << 8) | p[start + 3];
                bytelength = 4;
            }
            break;
        default:
            break;
        }
        if (!bytelength || !QP_IS_SURR(ch))
            return qp_dec_raise(d, start, end, reason);
        *newpos = start + bytelength;
        return qp_dec_putc(d, ch);
    }
    }
}

static int qp_decode_utf8(QPDecoder *d)
{
    Py_ssize_t pos = 0, newpos;
    qp_codec_error err;
    int r;

    while (pos < d->len) {
        r = qp_sl_utf8_decode((const char *)d->data + pos, d->len - pos, NULL, &d->out, &err);
        if (r == 0)
            break;
        if (r != QP_SL_CODEC_ERROR) {
            qp_raise_memory_error(d->ctx);
            return -1;
        }
        if (qp_dec_error(d, pos + err.start, pos + err.end, err.reason, &newpos) < 0)
            return -1;
        pos = newpos;
    }
    return 0;
}

static int qp_decode_ucs1(QPDecoder *d, Py_UCS4 limit)
{
    Py_ssize_t i = 0, newpos;
    if (qp_ucsbuilder_prepare(&d->out, d->len, 0x7F) < 0) {
        qp_raise_memory_error(d->ctx);
        return -1;
    }
    while (i < d->len) {
        Py_UCS4 c = d->data[i];
        if (c < limit) {
            if (qp_dec_putc(d, c))
                return -1;
            i++;
            continue;
        }
        if (qp_dec_error(d, i, i + 1, "ordinal not in range(128)", &newpos) < 0)
            return -1;
        i = newpos;
    }
    return 0;
}

/* PyUnicode_DecodeUTF16Stateful (final). */
static int qp_decode_utf16(QPDecoder *d)
{
    const uint8_t *p = d->data;
    Py_ssize_t q = 0, e = d->len, newpos, start, end;
    const char *errmsg;

    if (d->codec == QP_ENC_UTF16 && e >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            q = 2;
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            q = 2;
            d->le = 0;
        }
    }
    d->encoding = d->le ? "utf-16-le" : "utf-16-be";
#define QP_RD16(i) (d->le ? (Py_UCS4)p[i] | ((Py_UCS4)p[(i) + 1] << 8) : ((Py_UCS4)p[i] << 8) | p[(i) + 1])
    while (q < e) {
        if (e - q < 2) {
            errmsg = "truncated data";
            start = q;
            end = e;
        } else {
            Py_UCS4 u = QP_RD16(q);
            if (!QP_IS_SURR(u)) {
                if (qp_dec_putc(d, u))
                    return -1;
                q += 2;
                continue;
            }
            if (QP_IS_LO_SURR(u)) {
                errmsg = "illegal encoding";
                start = q;
                end = q + 2;
            } else if (e - q < 4) {
                errmsg = "unexpected end of data";
                start = q;
                end = e;
            } else {
                Py_UCS4 u2 = QP_RD16(q + 2);
                if (QP_IS_LO_SURR(u2)) {
                    if (qp_dec_putc(d, QP_FROM_SURR(u, u2)))
                        return -1;
                    q += 4;
                    continue;
                }
                errmsg = "illegal UTF-16 surrogate";
                start = q;
                end = q + 2;
            }
        }
        if (qp_dec_error(d, start, end, errmsg, &newpos) < 0)
            return -1;
        q = newpos;
    }
#undef QP_RD16
    return 0;
}

/* PyUnicode_DecodeUTF32Stateful (final). */
static int qp_decode_utf32(QPDecoder *d)
{
    const uint8_t *p = d->data;
    Py_ssize_t q = 0, e = d->len, newpos, start, end;
    const char *errmsg;

    if (d->codec == QP_ENC_UTF32 && e >= 4) {
        if (p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) {
            q = 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) {
            q = 4;
            d->le = 0;
        }
    }
    d->encoding = d->le ? "utf-32-le" : "utf-32-be";
    while (q < e) {
        if (e - q < 4) {
            errmsg = "truncated data";
            start = q;
            end = e;
        } else {
            Py_UCS4 ch = d->le ? p[q] | (p[q + 1] << 8) | (p[q + 2] << 16) | ((Py_UCS4)p[q + 3] << 24)
                               : ((Py_UCS4)p[q] << 24) | (p[q + 1] << 16) | (p[q + 2] << 8) | p[q + 3];
            if (ch < 0x110000 && !QP_IS_SURR(ch)) {
                if (qp_dec_putc(d, ch))
                    return -1;
                q += 4;
                continue;
            }
            errmsg = ch < 0x110000 ? "code point in surrogate code point range(0xd800, 0xe000)"
                                   : "code point not in range(0x110000)";
            start = q;
            end = q + 4;
        }
        if (qp_dec_error(d, start, end, errmsg, &newpos) < 0)
            return -1;
        q = newpos;
    }
    return 0;
}

static JSValue qp_str_decode(JSContext *ctx, const char *data, Py_ssize_t len,
                             const char *encoding, const char *errors)
{
    QPDecoder d;
    int codec, eh, r;

    if (qp_codec_check_encoding_errors(ctx, encoding, errors) < 0)
        return JS_EXCEPTION;
    if (len == 0)
        return qp_str_empty(ctx);
    codec = encoding ? qp_str_codec_lookup(encoding) : QP_ENC_UTF8;
    eh = qp_error_handler_builtin(errors);
    if (codec < 0 || eh < 0) {
        /* PyUnicode_Decode: decode via the codec registry */
        JSValue buf = qp_bytes_new(ctx, data, len), res;
        if (JS_IsException(buf))
            return buf;
        res = qp_codec_decode_text(ctx, buf, encoding ? encoding : "utf-8", errors);
        JS_FreeValue(ctx, buf);
        return res;
    }
    memset(&d, 0, sizeof(d));
    d.ctx = ctx;
    d.codec = codec;
    d.errors = errors;
    d.eh = eh;
    d.data = (const uint8_t *)data;
    d.len = len;
    d.le = !(codec == QP_ENC_UTF16_BE || codec == QP_ENC_UTF32_BE);
    qp_ucsbuilder_init(&d.out, ctx->rt);
    switch (codec) {
    case QP_ENC_UTF8_SIG:
        /* codecs.utf_8_sig: the BOM is dropped before utf_8_decode sees
           the data */
        if (len >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) {
            d.data += 3;
            d.len -= 3;
        }
        /* fall through */
    case QP_ENC_UTF8:
        d.encoding = "utf-8";
        r = qp_decode_utf8(&d);
        break;
    case QP_ENC_LATIN1:
        if (len > JS_STRING_LEN_MAX) {
            qp_ucsbuilder_free(&d.out);
            return qp_str_too_long(ctx);
        }
        qp_ucsbuilder_free(&d.out);
        return js_new_string8_len(ctx, data, (int)len);
    case QP_ENC_ASCII:
        d.encoding = "ascii";
        r = qp_decode_ucs1(&d, 0x80);
        break;
    case QP_ENC_UTF16: case QP_ENC_UTF16_LE: case QP_ENC_UTF16_BE:
        r = qp_decode_utf16(&d);
        break;
    default:
        r = qp_decode_utf32(&d);
        break;
    }
    if (r < 0) {
        qp_ucsbuilder_free(&d.out);
        return JS_EXCEPTION;
    }
    return qp_str_from_builder(ctx, &d.out);
}

/* encoding / errors argument as a NUL-free C string (JS_FreeCString). */
static const char *qp_str_arg_cstr(JSContext *ctx, JSValueConst v, const char *fname,
                                   const char *argname)
{
    size_t len;
    const char *s;

    if (!qp_str_check(ctx, v)) {
        char disp[64];
        snprintf(disp, sizeof(disp), "argument '%s'", argname);
        qp_str_bad_arg(ctx, fname, disp, "str", v);
        return NULL;
    }
    s = qp_str_utf8(ctx, v, &len);
    if (!s)
        return NULL;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        qp_raise_value_error(ctx, "embedded null character");
        return NULL;
    }
    return s;
}

QP_STR_METH(qp_str_m_encode)
{
    static const char *const kw[] = { "encoding", "errors", NULL };
    QPArgSpec spec = { "encode", kw, 0, 2, 0 };
    JSValueConst a[2];
    const char *enc = NULL, *err = NULL;
    JSValue res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && !(enc = qp_str_arg_cstr(ctx, a[0], "encode", "encoding")))
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && !(err = qp_str_arg_cstr(ctx, a[1], "encode", "errors"))) {
        JS_FreeCString(ctx, enc);
        return JS_EXCEPTION;
    }
    res = qp_str_encode(ctx, self, enc, err);
    JS_FreeCString(ctx, enc);
    JS_FreeCString(ctx, err);
    return res;
}

/* PyUnicode_FromEncodedObject. */
static JSValue qp_str_from_encoded(JSContext *ctx, JSValueConst obj, const char *encoding,
                                   const char *errors)
{
    QPBuffer buf;
    JSValue res;

    if (qp_str_check(ctx, obj))
        return qp_raise_type_error(ctx, "decoding str is not supported");
    if (qp_buffer_get(ctx, obj, &buf, QP_BUF_SIMPLE) < 0) {
        char tn[ATOM_GET_STR_BUF_SIZE];
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return qp_raise_type_error(ctx, "decoding to str: need a bytes-like object, %.80s found",
                                   qp_str_tpname(ctx, obj, tn, sizeof(tn)));
    }
    res = qp_str_decode(ctx, (const char *)buf.buf, buf.len, encoding, errors);
    qp_buffer_release(ctx, &buf);
    return res;
}

/* ========================================================================= */
/* Constructor                                                                */
/* ========================================================================= */

/* unicode_subtype_new: instance of the str subclass `type` holding u's
   value (consumes u). */
static JSValue qp_str_subtype_new(JSContext *ctx, QPType *type, JSValue u)
{
    JSValue obj, val;
    JSObject *p;

    if (JS_IsException(u))
        return u;
    val = qp_str_value(ctx, u);
    JS_FreeValue(ctx, u);
    obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_BOXED);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, val);
        return obj;
    }
    p = JS_VALUE_GET_OBJ(obj);
    JS_FreeValue(ctx, p->u.object_data);
    p->u.object_data = val;
    return obj;
}

/* str(object, encoding, errors) for the given encoding/errors values
   (QP_NULL = absent). */
static JSValue qp_str_new_impl(JSContext *ctx, JSValueConst x, JSValueConst encoding,
                               JSValueConst errors)
{
    const char *enc = NULL, *err = NULL;
    JSValue res;

    if (QP_IS_NULL(x))
        return qp_str_empty(ctx);
    if (QP_IS_NULL(encoding) && QP_IS_NULL(errors))
        return qp_str(ctx, x);
    if (!QP_IS_NULL(encoding) && !(enc = qp_str_arg_cstr(ctx, encoding, "str", "encoding")))
        return JS_EXCEPTION;
    if (!QP_IS_NULL(errors) && !(err = qp_str_arg_cstr(ctx, errors, "str", "errors"))) {
        JS_FreeCString(ctx, enc);
        return JS_EXCEPTION;
    }
    res = qp_str_from_encoded(ctx, x, enc, err);
    JS_FreeCString(ctx, enc);
    JS_FreeCString(ctx, err);
    return res;
}

/* tp_new (unicode_new). */
static JSValue qp_str_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "object", "encoding", "errors", NULL };
    QPArgSpec spec = { "str", kw, 0, 3, 0 };
    JSValueConst a[3];
    JSValue u;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    u = qp_str_new_impl(ctx, a[0], a[1], a[2]);
    if (!JS_IsException(u) && subtype != QP_TYPE(ctx, str))
        return qp_str_subtype_new(ctx, subtype, u);
    return u;
}

/* tp_vectorcall of the str type (unicode_vectorcall). */
static JSValue qp_str_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t = qp_type_from_value(callable);

    if (!t)
        t = QP_TYPE(ctx, str);
    if ((!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) != 0) || t != QP_TYPE(ctx, str))
        return qp_str_tp_new(ctx, t, args, nargsf, kwnames);
    if (qp_arg_check_positional(ctx, "str", (int)nargs, 0, 3) < 0)
        return JS_EXCEPTION;
    return qp_str_new_impl(ctx, nargs > 0 ? args[0] : QP_NULL, nargs > 1 ? args[1] : QP_NULL,
                           nargs > 2 ? args[2] : QP_NULL);
}

/* ========================================================================= */
/* Type                                                                       */
/* ========================================================================= */

#define QP_SIG(sig, text) sig "\n--\n\n" text

static const QPMethodDef qp_str_methods[] = {
    { "encode", qp_str_m_encode, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_SIG("encode($self, /, encoding='utf-8', errors='strict')",
             "Encode the string using the codec registered for encoding.\n\n"
             "  encoding\n    The encoding in which to encode the string.\n"
             "  errors\n    The error handling scheme to use for encoding errors.\n"
             "    The default is 'strict' meaning that encoding errors raise a\n"
             "    UnicodeEncodeError.  Other possible values are 'ignore', 'replace'\n"
             "    and 'xmlcharrefreplace' as well as any other name registered with\n"
             "    codecs.register_error that can handle UnicodeEncodeErrors.") },
    { "replace", qp_str_m_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_SIG("replace($self, old, new, /, count=-1)",
             "Return a copy with all occurrences of substring old replaced by new.\n\n"
             "  count\n    Maximum number of occurrences to replace.\n"
             "    -1 (the default value) means replace all occurrences.\n\n"
             "If the optional argument count is given, only the first count occurrences are\n"
             "replaced.") },
    { "split", qp_str_m_split, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_SIG("split($self, /, sep=None, maxsplit=-1)",
             "Return a list of the substrings in the string, using sep as the separator string.\n\n"
             "  sep\n    The separator used to split the string.\n\n"
             "    When set to None (the default value), will split on any whitespace\n"
             "    character (including \\n \\r \\t \\f and spaces) and will discard\n"
             "    empty strings from the result.\n"
             "  maxsplit\n    Maximum number of splits.\n"
             "    -1 (the default value) means no limit.\n\n"
             "Splitting starts at the front of the string and works to the end.\n\n"
             "Note, str.split() is mainly useful for data that has been intentionally\n"
             "delimited.  With natural text that includes punctuation, consider using\n"
             "the regular expression module.") },
    { "rsplit", qp_str_m_split, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      QP_SIG("rsplit($self, /, sep=None, maxsplit=-1)",
             "Return a list of the substrings in the string, using sep as the separator string.\n\n"
             "  sep\n    The separator used to split the string.\n\n"
             "    When set to None (the default value), will split on any whitespace\n"
             "    character (including \\n \\r \\t \\f and spaces) and will discard\n"
             "    empty strings from the result.\n"
             "  maxsplit\n    Maximum number of splits.\n"
             "    -1 (the default value) means no limit.\n\n"
             "Splitting starts at the end of the string and works to the front.") },
    { "join", qp_str_m_join, QP_METH_O, 0,
      QP_SIG("join($self, iterable, /)",
             "Concatenate any number of strings.\n\n"
             "The string whose method is called is inserted in between each given string.\n"
             "The result is returned as a new string.\n\n"
             "Example: '.'.join(['ab', 'pq', 'rs']) -> 'ab.pq.rs'") },
    { "capitalize", qp_str_m_case, QP_METH_NOARGS, QP_CASE_CAPITALIZE,
      QP_SIG("capitalize($self, /)",
             "Return a capitalized version of the string.\n\n"
             "More specifically, make the first character have upper case and the rest lower\n"
             "case.") },
    { "casefold", qp_str_m_case, QP_METH_NOARGS, QP_CASE_CASEFOLD,
      QP_SIG("casefold($self, /)", "Return a version of the string suitable for caseless comparisons.") },
    { "title", qp_str_m_case, QP_METH_NOARGS, QP_CASE_TITLE,
      QP_SIG("title($self, /)",
             "Return a version of the string where each word is titlecased.\n\n"
             "More specifically, words start with uppercased characters and all remaining\n"
             "cased characters have lower case.") },
    { "center", qp_str_m_pad, QP_METH_FASTCALL, QP_PAD_CENTER,
      QP_SIG("center($self, width, fillchar=' ', /)",
             "Return a centered string of length width.\n\n"
             "Padding is done using the specified fill character (default is a space).") },
    { "count", qp_str_m_find, QP_METH_FASTCALL, QP_FIND_COUNT,
      QP_SIG("count($self, sub[, start[, end]], /)",
             "Return the number of non-overlapping occurrences of substring sub in string S[start:end].\n\n"
             "Optional arguments start and end are interpreted as in slice\nnotation.") },
    { "expandtabs", qp_str_m_expandtabs, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_SIG("expandtabs($self, /, tabsize=8)",
             "Return a copy where all tab characters are expanded using spaces.\n\n"
             "If tabsize is not given, a tab size of 8 characters is assumed.") },
    { "find", qp_str_m_find, QP_METH_FASTCALL, QP_FIND_FIND,
      QP_SIG("find($self, sub[, start[, end]], /)",
             "Return the lowest index in S where substring sub is found, such that sub is contained within S[start:end].\n\n"
             "Optional arguments start and end are interpreted as in slice\nnotation.  Return -1 on failure.") },
    { "partition", qp_str_m_partition, QP_METH_O, 1,
      QP_SIG("partition($self, sep, /)",
             "Partition the string into three parts using the given separator.\n\n"
             "This will search for the separator in the string.  If the separator\n"
             "is found, returns a 3-tuple containing the part before the\n"
             "separator, the separator itself, and the part after it.\n\n"
             "If the separator is not found, returns a 3-tuple containing\n"
             "the original string and two empty strings.") },
    { "index", qp_str_m_find, QP_METH_FASTCALL, QP_FIND_INDEX,
      QP_SIG("index($self, sub[, start[, end]], /)",
             "Return the lowest index in S where substring sub is found, such that sub is contained within S[start:end].\n\n"
             "Optional arguments start and end are interpreted as in slice\nnotation.  Raises ValueError when the substring is not found.") },
    { "ljust", qp_str_m_pad, QP_METH_FASTCALL, QP_PAD_LJUST,
      QP_SIG("ljust($self, width, fillchar=' ', /)",
             "Return a left-justified string of length width.\n\n"
             "Padding is done using the specified fill character (default is a space).") },
    { "lower", qp_str_m_case, QP_METH_NOARGS, QP_CASE_LOWER,
      QP_SIG("lower($self, /)", "Return a copy of the string converted to lowercase.") },
    { "lstrip", qp_str_m_strip, QP_METH_FASTCALL, QP_SL_LEFTSTRIP,
      QP_SIG("lstrip($self, chars=None, /)",
             "Return a copy of the string with leading whitespace removed.\n\n"
             "If chars is given and not None, remove characters in chars instead.") },
    { "rfind", qp_str_m_find, QP_METH_FASTCALL, QP_FIND_RFIND,
      QP_SIG("rfind($self, sub[, start[, end]], /)",
             "Return the highest index in S where substring sub is found, such that sub is contained within S[start:end].\n\n"
             "Optional arguments start and end are interpreted as in slice\nnotation.  Return -1 on failure.") },
    { "rindex", qp_str_m_find, QP_METH_FASTCALL, QP_FIND_RINDEX,
      QP_SIG("rindex($self, sub[, start[, end]], /)",
             "Return the highest index in S where substring sub is found, such that sub is contained within S[start:end].\n\n"
             "Optional arguments start and end are interpreted as in slice\nnotation.  Raises ValueError when the substring is not found.") },
    { "rjust", qp_str_m_pad, QP_METH_FASTCALL, QP_PAD_RJUST,
      QP_SIG("rjust($self, width, fillchar=' ', /)",
             "Return a right-justified string of length width.\n\n"
             "Padding is done using the specified fill character (default is a space).") },
    { "rstrip", qp_str_m_strip, QP_METH_FASTCALL, QP_SL_RIGHTSTRIP,
      QP_SIG("rstrip($self, chars=None, /)",
             "Return a copy of the string with trailing whitespace removed.\n\n"
             "If chars is given and not None, remove characters in chars instead.") },
    { "rpartition", qp_str_m_partition, QP_METH_O, -1,
      QP_SIG("rpartition($self, sep, /)",
             "Partition the string into three parts using the given separator.\n\n"
             "This will search for the separator in the string, starting at the end. If\n"
             "the separator is found, returns a 3-tuple containing the part before the\n"
             "separator, the separator itself, and the part after it.\n\n"
             "If the separator is not found, returns a 3-tuple containing\n"
             "two empty strings and the original string.") },
    { "splitlines", qp_str_m_splitlines, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_SIG("splitlines($self, /, keepends=False)",
             "Return a list of the lines in the string, breaking at line boundaries.\n\n"
             "Line breaks are not included in the resulting list unless keepends is given and\ntrue.") },
    { "strip", qp_str_m_strip, QP_METH_FASTCALL, QP_SL_BOTHSTRIP,
      QP_SIG("strip($self, chars=None, /)",
             "Return a copy of the string with leading and trailing whitespace removed.\n\n"
             "If chars is given and not None, remove characters in chars instead.") },
    { "swapcase", qp_str_m_case, QP_METH_NOARGS, QP_CASE_SWAPCASE,
      QP_SIG("swapcase($self, /)",
             "Convert uppercase characters to lowercase and lowercase characters to uppercase.") },
    { "translate", qp_str_m_translate, QP_METH_O, 0,
      QP_SIG("translate($self, table, /)",
             "Replace each character in the string using the given translation table.\n\n"
             "  table\n    Translation table, which must be a mapping of Unicode ordinals\n"
             "    to Unicode ordinals, strings, or None.\n\n"
             "The table must implement lookup/indexing via __getitem__, for\n"
             "instance a dictionary or list.  If this operation raises\n"
             "LookupError, the character is left untouched.  Characters mapped to\n"
             "None are deleted.") },
    { "upper", qp_str_m_case, QP_METH_NOARGS, QP_CASE_UPPER,
      QP_SIG("upper($self, /)", "Return a copy of the string converted to uppercase.") },
    { "startswith", qp_str_m_tailmatch, QP_METH_FASTCALL, -1,
      QP_SIG("startswith($self, prefix[, start[, end]], /)",
             "Return True if the string starts with the specified prefix, False otherwise.\n\n"
             "  prefix\n    A string or a tuple of strings to try.\n"
             "  start\n    Optional start position. Default: start of the string.\n"
             "  end\n    Optional stop position. Default: end of the string.") },
    { "endswith", qp_str_m_tailmatch, QP_METH_FASTCALL, 1,
      QP_SIG("endswith($self, suffix[, start[, end]], /)",
             "Return True if the string ends with the specified suffix, False otherwise.\n\n"
             "  suffix\n    A string or a tuple of strings to try.\n"
             "  start\n    Optional start position. Default: start of the string.\n"
             "  end\n    Optional stop position. Default: end of the string.") },
    { "removeprefix", qp_str_m_removeaffix, QP_METH_O, -1,
      QP_SIG("removeprefix($self, prefix, /)",
             "Return a str with the given prefix string removed if present.\n\n"
             "If the string starts with the prefix string, return string[len(prefix):].\n"
             "Otherwise, return a copy of the original string.") },
    { "removesuffix", qp_str_m_removeaffix, QP_METH_O, 1,
      QP_SIG("removesuffix($self, suffix, /)",
             "Return a str with the given suffix string removed if present.\n\n"
             "If the string ends with the suffix string and that suffix is not empty,\n"
             "return string[:-len(suffix)]. Otherwise, return a copy of the original\nstring.") },
    { "isascii", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISASCII,
      QP_SIG("isascii($self, /)",
             "Return True if all characters in the string are ASCII, False otherwise.\n\n"
             "ASCII characters have code points in the range U+0000-U+007F.\n"
             "Empty string is ASCII too.") },
    { "islower", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISLOWER,
      QP_SIG("islower($self, /)",
             "Return True if the string is a lowercase string, False otherwise.\n\n"
             "A string is lowercase if all cased characters in the string are lowercase and\n"
             "there is at least one cased character in the string.") },
    { "isupper", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISUPPER,
      QP_SIG("isupper($self, /)",
             "Return True if the string is an uppercase string, False otherwise.\n\n"
             "A string is uppercase if all cased characters in the string are uppercase and\n"
             "there is at least one cased character in the string.") },
    { "istitle", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISTITLE,
      QP_SIG("istitle($self, /)",
             "Return True if the string is a title-cased string, False otherwise.\n\n"
             "In a title-cased string, upper- and title-case characters may only\n"
             "follow uncased characters and lowercase characters only cased ones.") },
    { "isspace", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISSPACE,
      QP_SIG("isspace($self, /)",
             "Return True if the string is a whitespace string, False otherwise.\n\n"
             "A string is whitespace if all characters in the string are whitespace and there\n"
             "is at least one character in the string.") },
    { "isdecimal", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISDECIMAL,
      QP_SIG("isdecimal($self, /)",
             "Return True if the string is a decimal string, False otherwise.\n\n"
             "A string is a decimal string if all characters in the string are decimal and\n"
             "there is at least one character in the string.") },
    { "isdigit", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISDIGIT,
      QP_SIG("isdigit($self, /)",
             "Return True if the string is a digit string, False otherwise.\n\n"
             "A string is a digit string if all characters in the string are digits and there\n"
             "is at least one character in the string.") },
    { "isnumeric", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISNUMERIC,
      QP_SIG("isnumeric($self, /)",
             "Return True if the string is a numeric string, False otherwise.\n\n"
             "A string is numeric if all characters in the string are numeric and there is at\n"
             "least one character in the string.") },
    { "isalpha", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISALPHA,
      QP_SIG("isalpha($self, /)",
             "Return True if the string is an alphabetic string, False otherwise.\n\n"
             "A string is alphabetic if all characters in the string are alphabetic and there\n"
             "is at least one character in the string.") },
    { "isalnum", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISALNUM,
      QP_SIG("isalnum($self, /)",
             "Return True if the string is an alpha-numeric string, False otherwise.\n\n"
             "A string is alpha-numeric if all characters in the string are alpha-numeric and\n"
             "there is at least one character in the string.") },
    { "isidentifier", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISIDENTIFIER,
      QP_SIG("isidentifier($self, /)",
             "Return True if the string is a valid Python identifier, False otherwise.\n\n"
             "Call keyword.iskeyword(s) to test whether string s is a reserved identifier,\n"
             "such as \"def\" or \"class\".") },
    { "isprintable", qp_str_m_pred, QP_METH_NOARGS, QP_PRED_ISPRINTABLE,
      QP_SIG("isprintable($self, /)",
             "Return True if all characters in the string are printable, False otherwise.\n\n"
             "A character is printable if repr() may use it in its output.") },
    { "zfill", qp_str_m_zfill, QP_METH_O, 0,
      QP_SIG("zfill($self, width, /)",
             "Pad a numeric string with zeros on the left, to fill a field of the given width.\n\n"
             "The string is never truncated.") },
    { "format", qp_str_format_meth, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "format($self, /, *args, **kwargs)\n--\n\n"
      "Return a formatted version of the string, using substitutions from args and kwargs.\n"
      "The substitutions are identified by braces ('{' and '}')." },
    { "format_map", qp_str_format_map_meth, QP_METH_O, 0,
      "format_map($self, mapping, /)\n--\n\n"
      "Return a formatted version of the string, using substitutions from mapping.\n"
      "The substitutions are identified by braces ('{' and '}')." },
    { "__format__", qp_str_format_dunder, QP_METH_O, 0,
      QP_SIG("__format__($self, format_spec, /)", "Return a formatted version of the string as described by format_spec.") },
    { "maketrans", qp_str_m_maketrans, QP_METH_FASTCALL | QP_METH_STATIC, 0,
      QP_SIG("maketrans(x, y=<unrepresentable>, z=<unrepresentable>, /)",
             "Return a translation table usable for str.translate().\n\n"
             "If there is only one argument, it must be a dictionary mapping Unicode\n"
             "ordinals (integers) or characters to Unicode ordinals, strings or None.\n"
             "Character keys will be then converted to ordinals.\n"
             "If there are two arguments, they must be strings of equal length, and\n"
             "in the resulting dictionary, each character in x will be mapped to the\n"
             "character at the same position in y. If there is a third argument, it\n"
             "must be a string, whose characters will be mapped to None in the result.") },
    { "__sizeof__", qp_str_m_sizeof, QP_METH_NOARGS, 0,
      QP_SIG("__sizeof__($self, /)", "Return the size of the string in memory, in bytes.") },
    { "__getnewargs__", qp_str_m_getnewargs, QP_METH_NOARGS, 0, NULL },
    { NULL, NULL, 0, 0, NULL }
};

static const QPTypeSpec qp_str_spec = {
    .name = "str",
    .doc = "str(object='') -> str\n"
           "str(bytes_or_buffer[, encoding[, errors]]) -> str\n\n"
           "Create a new string object from the given object. If encoding or\n"
           "errors is specified, then the object must expose a data buffer\n"
           "that will be decoded using the given encoding and error handler.\n"
           "Otherwise, returns the result of object.__str__() (if defined)\n"
           "or repr(object).\n"
           "encoding defaults to 'utf-8'.\n"
           "errors defaults to 'strict'.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_UNICODE_SUBCLASS |
             QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BOXED,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_str_repr,
        .tp_str = qp_str_tp_str,
        .tp_hash = qp_str_hash,
        .tp_vectorcall = qp_str_vectorcall,
        .tp_richcompare = qp_str_richcompare,
        .tp_iter = qp_str_iter_new,
        .tp_iteritem = qp_str_iteritem,
        .tp_new = qp_str_tp_new,
        .nb = { .nb_remainder = qp_str_remainder },
        .sq = {
            .sq_length = qp_str_sq_length,
            .sq_concat = qp_str_sq_concat,
            .sq_repeat = qp_str_sq_repeat,
            .sq_item = qp_str_sq_item,
            .sq_contains = qp_str_sq_contains,
        },
        .mp = {
            .mp_length = qp_str_sq_length,
            .mp_subscript = qp_str_mp_subscript,
        },
    },
    .methods = qp_str_methods,
};

/* Unit d init (m2-design §7 step 5): str cache, str type, then
   string.templatelib types.  QPState.hash_key is seeded earlier by
   qp_state_init (unit a), before any str is hashed. */
static int qp_init_str(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);

    if (!st->str_cache) {
        struct QPStrCache *c = js_malloc(ctx, sizeof(*c));
        int i;
        if (!c)
            return -1;
        for (i = 0; i < QP_STR_CACHE_SIZE; i++) {
            c->e[i].str = NULL;
            c->e[i].hash = -1;
            c->e[i].cplen = -1;
            c->e[i].ucs4 = NULL;
        }
        st->str_cache = c;
    }
    if (qp_type_ready(ctx, QP_TYPE(ctx, str), &qp_str_spec) < 0)
        return -1;
    return qp_init_templatelib(ctx);
}
