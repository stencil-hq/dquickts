/*
 * stringlib.c - buffer-level string algorithms for the Python runtime.
 *
 * Derived from CPython Objects/stringlib/ and Objects/unicodeobject.c,
 * Objects/stringlib/transmogrify.h, Objects/bytes_methods.c (PSF-2.0),
 * v3.15.2.  See python/stringlib.h for the API and conventions.
 *
 * The search/split/partition/count/replace/find_max_char/UTF-8 kernels are
 * CPython's stringlib templates (python/stringlib/ *.h), instantiated for
 * UCS1, UCS2 and UCS4 exactly as unicodeobject.c does (plus a bytes flavour
 * of the whitespace/line-break functions).  The kind dispatchers below are
 * ports of the unicodeobject.c drivers.  The only systematic difference:
 * CPython's str objects are canonical (kind = smallest that fits), so it may
 * answer "not found" as soon as the needle's kind is wider than the
 * haystack's; our views may be wider than necessary, so the needle is
 * narrowed when its characters fit (qp_sl_needle_askind) and the shortcut is
 * taken only when they really don't.  Results are identical.
 *
 * Everything is static: this file is part of the quickjs.c translation unit.
 */

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* ---- unit-local configuration (pyport.h / pyconfig.h equivalents) ------- */

#define QP_SL_LOCAL_INLINE(type) static inline type
#define QP_SL_MAX(x, y) (((x) > (y)) ? (x) : (y))
#define QP_SL_MIN(x, y) (((x) > (y)) ? (y) : (x))
#define QP_SL_ALIGN_DOWN(p, a) ((void *)((uintptr_t)(p) & ~(uintptr_t)((a) - 1)))
#define QP_SL_IS_ALIGNED(p, a) (!((uintptr_t)(p) & (uintptr_t)((a) - 1)))

#if defined(__SIZEOF_SIZE_T__)
# define QP_SL_SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#elif defined(_WIN64)
# define QP_SL_SIZEOF_SIZE_T 8
#elif defined(_WIN32)
# define QP_SL_SIZEOF_SIZE_T 4
#elif SIZE_MAX > 0xFFFFFFFFu
# define QP_SL_SIZEOF_SIZE_T 8
#else
# define QP_SL_SIZEOF_SIZE_T 4
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
# if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define QP_SL_LITTLE_ENDIAN 1
# else
#  define QP_SL_LITTLE_ENDIAN 0
# endif
#elif defined(_WIN32)
# define QP_SL_LITTLE_ENDIAN 1
#else
# error "python/stringlib.c: cannot determine the byte order"
#endif

/* wmemchr() is used as STRINGLIB_FAST_MEMCHR for the kind whose width
   matches wchar_t (as CPython's ucs2lib.h / ucs4lib.h do). */
#if defined(__SIZEOF_WCHAR_T__)
# define QP_SL_SIZEOF_WCHAR_T __SIZEOF_WCHAR_T__
#elif defined(_WIN32)
# define QP_SL_SIZEOF_WCHAR_T 2
#else
# define QP_SL_SIZEOF_WCHAR_T 0
#endif

#define QP_SL_MAX_UNICODE 0x10ffff
#define QP_SL_IS_SURROGATE(ch) (0xD800 <= (ch) && (ch) <= 0xDFFF)

/* Bloom filter of unicodeobject.c (BLOOM_MASK / BLOOM / make_bloom_mask). */
#if ULONG_MAX > 0xFFFFFFFFUL
# define QP_SL_BLOOM_WIDTH 64
#else
# define QP_SL_BLOOM_WIDTH 32
#endif
#define QP_SL_BLOOM_BIT(ch) (1UL << ((ch) & (QP_SL_BLOOM_WIDTH - 1)))
#define QP_SL_BLOOM(mask, ch) ((mask) & QP_SL_BLOOM_BIT(ch))
/* bloom_linebreak: _PyUnicode_Init() builds it from these code points. */
#define QP_SL_BLOOM_LINEBREAK_MASK                                      \
    (QP_SL_BLOOM_BIT(0x000AUL) | QP_SL_BLOOM_BIT(0x000DUL) |            \
     QP_SL_BLOOM_BIT(0x001CUL) | QP_SL_BLOOM_BIT(0x001DUL) |            \
     QP_SL_BLOOM_BIT(0x001EUL) | QP_SL_BLOOM_BIT(0x0085UL) |            \
     QP_SL_BLOOM_BIT(0x2028UL) | QP_SL_BLOOM_BIT(0x2029UL))

/* _Py_ascii_whitespace (first 128 entries; the rest is 0). */
static const unsigned char qp_sl_ascii_whitespace[128] = {
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
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* ascii_linebreak of unicodeobject.c (first 128 entries). */
static const unsigned char qp_sl_ascii_linebreak[128] = {
    0, 0, 0, 0, 0, 0, 0, 0,
/*         0x000A, * LINE FEED */
/*         0x000B, * LINE TABULATION */
/*         0x000C, * FORM FEED */
/*         0x000D, * CARRIAGE RETURN */
    0, 0, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
/*         0x001C, * FILE SEPARATOR */
/*         0x001D, * GROUP SEPARATOR */
/*         0x001E, * RECORD SEPARATOR */
    0, 0, 0, 0, 1, 1, 1, 0,
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
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* Py_UNICODE_ISSPACE */
#define QP_SL_UNICODE_ISSPACE(ch)                                       \
    ((Py_UCS4)(ch) < 128U ? qp_sl_ascii_whitespace[(ch)] :              \
     _PyUnicode_IsWhitespace(ch))

/* BLOOM_LINEBREAK of unicodeobject.c (= Py_UNICODE_ISLINEBREAK) */
#define QP_SL_BLOOM_LINEBREAK(ch)                                       \
    ((Py_UCS4)(ch) < 128U ? qp_sl_ascii_linebreak[(ch)] :               \
     (QP_SL_BLOOM(QP_SL_BLOOM_LINEBREAK_MASK, (ch)) &&                  \
      _PyUnicode_IsLinebreak(ch)))

/* PyUnicode_WRITE */
#define QP_SL_WRITE(kind, data, i, ch) do {                             \
        if ((kind) == 1)                                                \
            ((Py_UCS1 *)(data))[i] = (Py_UCS1)(ch);                     \
        else if ((kind) == 2)                                           \
            ((Py_UCS2 *)(data))[i] = (Py_UCS2)(ch);                     \
        else                                                            \
            ((Py_UCS4 *)(data))[i] = (Py_UCS4)(ch);                     \
    } while (0)

static int qp_spanvec_push(qp_spanvec *v, Py_ssize_t start, Py_ssize_t end);
static void qp_spanvec_reverse(qp_spanvec *v, Py_ssize_t from);

/* ---- template instantiation (as in unicodeobject.c) ---------------------- */

#include "stringlib/ucs1lib.h"
#include "stringlib/fastsearch.h"
#include "stringlib/partition.h"
#include "stringlib/split.h"
#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/replace.h"
#include "stringlib/find_max_char.h"
#include "stringlib/codecs.h"
#include "stringlib/undef.h"

#include "stringlib/ucs2lib.h"
#include "stringlib/fastsearch.h"
#include "stringlib/partition.h"
#include "stringlib/split.h"
#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/replace.h"
#include "stringlib/find_max_char.h"
#include "stringlib/codecs.h"
#include "stringlib/undef.h"

#include "stringlib/ucs4lib.h"
#include "stringlib/fastsearch.h"
#include "stringlib/partition.h"
#include "stringlib/split.h"
#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/replace.h"
#include "stringlib/find_max_char.h"
#include "stringlib/codecs.h"
#include "stringlib/undef.h"

/* bytes whitespace / line-break flavour (split_whitespace,
   rsplit_whitespace, splitlines only) */
#include "stringlib/byteslib.h"
#include "stringlib/split.h"
#include "stringlib/undef.h"

/* ---- span vector ------------------------------------------------------------ */

#define QP_SL_SPANVEC_MIN 12    /* split.h MAX_PREALLOC */

static void qp_spanvec_init(qp_spanvec *v, JSRuntime *rt)
{
    v->rt = rt;
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static void qp_spanvec_free(qp_spanvec *v)
{
    if (v->items)
        js_free_rt(v->rt, v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

/* Append [start, end), doubling the capacity when full. */
static int qp_spanvec_push(qp_spanvec *v, Py_ssize_t start, Py_ssize_t end)
{
    if (v->len == v->cap) {
        Py_ssize_t new_cap;
        qp_span *p;
        if (v->cap == 0)
            new_cap = QP_SL_SPANVEC_MIN;
        else if (v->cap > PY_SSIZE_T_MAX / 2 / (Py_ssize_t)sizeof(qp_span))
            return QP_SL_NOMEM;
        else
            new_cap = v->cap * 2;
        if (v->items)
            p = js_realloc_rt(v->rt, v->items, (size_t)new_cap * sizeof(qp_span));
        else
            p = js_malloc_rt(v->rt, (size_t)new_cap * sizeof(qp_span));
        if (!p)
            return QP_SL_NOMEM;
        v->items = p;
        v->cap = new_cap;
    }
    v->items[v->len].start = start;
    v->items[v->len].end = end;
    v->len++;
    return 0;
}

static void qp_spanvec_reverse(qp_spanvec *v, Py_ssize_t from)
{
    Py_ssize_t lo = from, hi = v->len - 1;
    while (lo < hi) {
        qp_span t = v->items[lo];
        v->items[lo] = v->items[hi];
        v->items[hi] = t;
        lo++;
        hi--;
    }
}

/* ---- kind helpers ------------------------------------------------------------- */

/* Largest code point a buffer of `kind` can hold. */
static inline Py_UCS4 qp_sl_kind_maxchar(int kind)
{
    return kind == 1 ? 0xFFu : kind == 2 ? 0xFFFFu : QP_SL_MAX_UNICODE;
}

static int qp_sl_kind_for_maxchar(Py_UCS4 maxchar)
{
    if (maxchar < 0x100)
        return 1;
    if (maxchar < 0x10000)
        return 2;
    return 4;
}

/* find_max_char over (kind, data)[0:len]. */
static Py_UCS4 qp_sl_find_max_char_kind(int kind, const void *data, Py_ssize_t len)
{
    switch (kind) {
    case 1:
        return ucs1lib_find_max_char((const Py_UCS1 *)data, (const Py_UCS1 *)data + len);
    case 2:
        return ucs2lib_find_max_char((const Py_UCS2 *)data, (const Py_UCS2 *)data + len);
    default:
        return ucs4lib_find_max_char((const Py_UCS4 *)data, (const Py_UCS4 *)data + len);
    }
}

static Py_UCS4 qp_sl_find_max_char(qp_strview s)
{
    return qp_sl_find_max_char_kind(s.kind, s.data, s.len);
}

/* _PyUnicode_CONVERT_BYTES generalised: copy n code units from (from_kind,
   src) to (to_kind, dst).  Narrowing requires every unit to fit.  dst may
   equal src for in-place narrowing (each unit is read before the narrower
   write that can overlap it); otherwise the buffers must not overlap
   unless the kinds are equal. */
static void qp_sl_convert(int to_kind, void *dst, int from_kind, const void *src,
                          Py_ssize_t n)
{
    Py_ssize_t i;

    if (n <= 0)
        return;
    if (to_kind == from_kind) {
        memmove(dst, src, (size_t)n * to_kind);
        return;
    }
#define QP_SL_CONV(TO, FROM) do {                                       \
        TO *d_ = (TO *)dst;                                             \
        const FROM *s_ = (const FROM *)src;                             \
        for (i = 0; i < n; i++)                                         \
            d_[i] = (TO)s_[i];                                          \
    } while (0)
    switch (to_kind * 8 + from_kind) {
    case 2 * 8 + 1: QP_SL_CONV(Py_UCS2, Py_UCS1); break;
    case 4 * 8 + 1: QP_SL_CONV(Py_UCS4, Py_UCS1); break;
    case 4 * 8 + 2: QP_SL_CONV(Py_UCS4, Py_UCS2); break;
    case 1 * 8 + 2: QP_SL_CONV(Py_UCS1, Py_UCS2); break;
    case 1 * 8 + 4: QP_SL_CONV(Py_UCS1, Py_UCS4); break;
    default:        QP_SL_CONV(Py_UCS2, Py_UCS4); break;
    }
#undef QP_SL_CONV
}

/* unicode_askind: new js_malloc_rt'd copy of (kind, data)[0:len] in
   to_kind (narrowing allowed when the characters fit).  NULL on OOM. */
static void *qp_sl_askind(JSRuntime *rt, int kind, const void *data, Py_ssize_t len,
                          int to_kind)
{
    void *buf;

    if (len > PY_SSIZE_T_MAX / to_kind)
        return NULL;
    buf = js_malloc_rt(rt, len > 0 ? (size_t)len * to_kind : 1);
    if (!buf)
        return NULL;
    qp_sl_convert(to_kind, buf, kind, data, len);
    return buf;
}

/* Make `sub` comparable with a haystack of `kind`.  Returns 0 with *pbuf =
   sub's characters in `kind` (*ptofree = the allocated copy, or NULL when
   sub.data is used directly); 1 when sub holds a character that a `kind`
   buffer cannot contain, so it cannot occur (CPython: kind1 < kind2); or
   QP_SL_NOMEM. */
static int qp_sl_needle_askind(JSRuntime *rt, qp_strview sub, int kind,
                               const void **pbuf, void **ptofree)
{
    void *buf;

    *ptofree = NULL;
    *pbuf = sub.data;
    if (sub.kind == kind)
        return 0;
    if (sub.kind > kind && qp_sl_find_max_char(sub) > qp_sl_kind_maxchar(kind))
        return 1;
    buf = qp_sl_askind(rt, sub.kind, sub.data, sub.len, kind);
    if (!buf)
        return QP_SL_NOMEM;
    *pbuf = buf;
    *ptofree = buf;
    return 0;
}

/* ---- builder ------------------------------------------------------------------ */

static void qp_ucsbuilder_init(qp_ucsbuilder *b, JSRuntime *rt)
{
    b->rt = rt;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->kind = 1;
}

static void qp_ucsbuilder_free(qp_ucsbuilder *b)
{
    if (b->data)
        js_free_rt(b->rt, b->data);
    qp_ucsbuilder_init(b, b->rt);
}

/* Reallocate to new_cap units of new_kind (>= b->kind), keeping the
   content.  0 or QP_SL_NOMEM (b unchanged). */
static int qp_ucsbuilder_resize(qp_ucsbuilder *b, Py_ssize_t new_cap, int new_kind)
{
    void *p;
    size_t size;

    if (new_cap > PY_SSIZE_T_MAX / new_kind)
        return QP_SL_NOMEM;
    size = new_cap > 0 ? (size_t)new_cap * new_kind : 1;
    if (new_kind == b->kind && b->data) {
        p = js_realloc_rt(b->rt, b->data, size);
        if (!p)
            return QP_SL_NOMEM;
    }
    else {
        p = js_malloc_rt(b->rt, size);
        if (!p)
            return QP_SL_NOMEM;
        if (b->data) {
            qp_sl_convert(new_kind, p, b->kind, b->data, b->len);
            js_free_rt(b->rt, b->data);
        }
    }
    b->data = p;
    b->cap = new_cap;
    b->kind = new_kind;
    return 0;
}

/* _PyUnicodeWriter_Prepare: room for n more units, kind able to hold
   maxchar.  Growing a non-empty buffer over-allocates by 25% like CPython's
   writer (OVERALLOCATE_FACTOR 4); the first allocation is exact. */
static int qp_ucsbuilder_prepare(qp_ucsbuilder *b, Py_ssize_t n, Py_UCS4 maxchar)
{
    int kind = qp_sl_kind_for_maxchar(maxchar);
    Py_ssize_t need, new_cap;

    if (kind < b->kind)
        kind = b->kind;
    if (n > PY_SSIZE_T_MAX - b->len)
        return QP_SL_NOMEM;
    need = b->len + n;
    if (need <= b->cap && kind == b->kind)
        return 0;
    new_cap = b->cap;
    if (need > new_cap) {
        new_cap = need;
        if (b->cap > 0 && need <= PY_SSIZE_T_MAX - need / 4)
            new_cap = need + need / 4;
    }
    return qp_ucsbuilder_resize(b, new_cap, kind);
}

static int qp_ucsbuilder_putc(qp_ucsbuilder *b, Py_UCS4 ch)
{
    if (qp_ucsbuilder_prepare(b, 1, ch) < 0)
        return QP_SL_NOMEM;
    QP_SL_WRITE(b->kind, b->data, b->len, ch);
    b->len++;
    return 0;
}

/* _PyUnicodeWriter_WriteSubstring: the range's max char is only scanned
   when it is stored wider than the builder. */
static int qp_ucsbuilder_write(qp_ucsbuilder *b, qp_strview v, Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t n = end - start;
    Py_UCS4 maxchar = 0;
    const char *src;

    if (n <= 0)
        return 0;
    src = (const char *)v.data + (size_t)start * v.kind;
    if (v.kind > b->kind)
        maxchar = qp_sl_find_max_char_kind(v.kind, src, n);
    if (qp_ucsbuilder_prepare(b, n, maxchar) < 0)
        return QP_SL_NOMEM;
    qp_sl_convert(b->kind, (char *)b->data + (size_t)b->len * b->kind, v.kind, src, n);
    b->len += n;
    return 0;
}

/* _PyUnicode_Fill: kind-specific fill of data[start:start+length]. */
static void qp_sl_fill(int kind, void *data, Py_UCS4 value, Py_ssize_t start, Py_ssize_t length)
{
    Py_ssize_t i;

    switch (kind) {
    case 1:
        memset((Py_UCS1 *)data + start, (unsigned char)value, (size_t)length);
        break;
    case 2: {
        Py_UCS2 *p = (Py_UCS2 *)data + start;
        for (i = 0; i < length; i++)
            p[i] = (Py_UCS2)value;
        break;
    }
    default: {
        Py_UCS4 *p = (Py_UCS4 *)data + start;
        for (i = 0; i < length; i++)
            p[i] = value;
        break;
    }
    }
}

static int qp_ucsbuilder_fill(qp_ucsbuilder *b, Py_UCS4 ch, Py_ssize_t n)
{
    if (n <= 0)
        return 0;
    if (qp_ucsbuilder_prepare(b, n, ch) < 0)
        return QP_SL_NOMEM;
    qp_sl_fill(b->kind, b->data, ch, b->len, n);
    b->len += n;
    return 0;
}

/* unicode_adjust_maxchar: convert in place to the smallest kind holding the
   content (no allocation; the spare capacity is kept). */
static int qp_ucsbuilder_narrow(qp_ucsbuilder *b)
{
    int kind;

    if (b->data == NULL) {
        b->kind = 1;
        return 0;
    }
    kind = qp_sl_kind_for_maxchar(qp_sl_find_max_char_kind(b->kind, b->data, b->len));
    if (kind < b->kind) {
        qp_sl_convert(kind, b->data, b->kind, b->data, b->len);
        b->cap = (Py_ssize_t)(((size_t)b->cap * b->kind) / kind);
        b->kind = kind;
    }
    return 0;
}

static qp_strview qp_ucsbuilder_view(const qp_ucsbuilder *b)
{
    return qp_strview_make(b->kind, b->data, b->len);
}

static void *qp_ucsbuilder_detach(qp_ucsbuilder *b, int *kind, Py_ssize_t *len)
{
    void *data = b->data;
    *kind = b->kind;
    *len = b->len;
    b->data = NULL;
    qp_ucsbuilder_init(b, b->rt);
    return data;
}

/* PyUnicode_New(n, maxchar) for the bulk paths: an empty builder becomes
   exactly n units of `kind` (kind 1 when n == 0, like the empty string). */
static int qp_ucsbuilder_alloc(qp_ucsbuilder *b, Py_ssize_t n, int kind)
{
    assert(b->len == 0);
    if (n == 0) {
        b->kind = 1;
        return 0;
    }
    if (b->data && b->kind != kind) {
        js_free_rt(b->rt, b->data);
        b->data = NULL;
        b->cap = 0;
    }
    b->kind = kind;
    if (n > b->cap && qp_ucsbuilder_resize(b, n, kind) < 0)
        return QP_SL_NOMEM;
    b->len = n;
    return 0;
}

/* ---- character classes -------------------------------------------------------- */

static int qp_sl_unicode_isspace(Py_UCS4 ch)
{
    return QP_SL_UNICODE_ISSPACE(ch) != 0;
}

static int qp_sl_unicode_islinebreak(Py_UCS4 ch)
{
    return QP_SL_BLOOM_LINEBREAK(ch) != 0;
}

/* ---- searching ---------------------------------------------------------------- */

/* helper macro to fixup start/end slice values (ADJUST_INDICES) */
static void qp_sl_adjust_indices(Py_ssize_t *pstart, Py_ssize_t *pend, Py_ssize_t len)
{
    Py_ssize_t start = *pstart, end = *pend;

    if (end > len) {
        end = len;
    }
    else if (end < 0) {
        end += len;
        if (end < 0) {
            end = 0;
        }
    }
    if (start < 0) {
        start += len;
        if (start < 0) {
            start = 0;
        }
    }
    *pstart = start;
    *pend = end;
}

/* findchar() of unicodeobject.c: first/last index of ch in (kind, s)[0:size]
   or -1. */
static Py_ssize_t qp_sl_findchar(const void *s, int kind, Py_ssize_t size, Py_UCS4 ch,
                                 int direction)
{
    switch (kind) {
    case 1:
        if ((Py_UCS1) ch != ch)
            return -1;
        if (direction > 0)
            return ucs1lib_find_char((const Py_UCS1 *) s, size, (Py_UCS1) ch);
        else
            return ucs1lib_rfind_char((const Py_UCS1 *) s, size, (Py_UCS1) ch);
    case 2:
        if ((Py_UCS2) ch != ch)
            return -1;
        if (direction > 0)
            return ucs2lib_find_char((const Py_UCS2 *) s, size, (Py_UCS2) ch);
        else
            return ucs2lib_rfind_char((const Py_UCS2 *) s, size, (Py_UCS2) ch);
    default:
        if (direction > 0)
            return ucs4lib_find_char((const Py_UCS4 *) s, size, ch);
        else
            return ucs4lib_rfind_char((const Py_UCS4 *) s, size, ch);
    }
}

/* anylib_find: find (kind, buf2)[0:len2] in (kind, buf1)[0:len1]; result
   offset by `offset` (or -1). */
static Py_ssize_t qp_sl_anylib_find(int kind, const void *buf1, Py_ssize_t len1,
                                    const void *buf2, Py_ssize_t len2, Py_ssize_t offset)
{
    switch (kind) {
    case 1:
        return ucs1lib_find(buf1, len1, buf2, len2, offset);
    case 2:
        return ucs2lib_find(buf1, len1, buf2, len2, offset);
    default:
        return ucs4lib_find(buf1, len1, buf2, len2, offset);
    }
}

/* anylib_count */
static Py_ssize_t qp_sl_anylib_count(int kind, const void *sbuf, Py_ssize_t slen,
                                     const void *buf1, Py_ssize_t len1, Py_ssize_t maxcount)
{
    switch (kind) {
    case 1:
        return ucs1lib_count(sbuf, slen, buf1, len1, maxcount);
    case 2:
        return ucs2lib_count(sbuf, slen, buf1, len1, maxcount);
    default:
        return ucs4lib_count(sbuf, slen, buf1, len1, maxcount);
    }
}

/* any_find_slice */
static Py_ssize_t qp_sl_find(JSRuntime *rt, qp_strview s, qp_strview sub,
                             Py_ssize_t start, Py_ssize_t end, int direction)
{
    Py_ssize_t len1 = s.len, len2 = sub.len, result;
    const void *buf1 = s.data, *buf2;
    void *tofree;
    int r;

    qp_sl_adjust_indices(&start, &end, len1);
    if (end - start < len2)
        return -1;

    if (len2 == 1) {
        Py_UCS4 ch = QP_SL_READ(sub.kind, sub.data, 0);
        result = qp_sl_findchar((const char *)buf1 + (size_t)s.kind * start,
                                s.kind, end - start, ch, direction);
        if (result == -1)
            return -1;
        else
            return start + result;
    }

    r = qp_sl_needle_askind(rt, sub, s.kind, &buf2, &tofree);
    if (r != 0)
        return r == 1 ? -1 : r;

    if (direction > 0) {
        switch (s.kind) {
        case 1:
            result = ucs1lib_find_slice(buf1, len1, buf2, len2, start, end);
            break;
        case 2:
            result = ucs2lib_find_slice(buf1, len1, buf2, len2, start, end);
            break;
        default:
            result = ucs4lib_find_slice(buf1, len1, buf2, len2, start, end);
            break;
        }
    }
    else {
        switch (s.kind) {
        case 1:
            result = ucs1lib_rfind_slice(buf1, len1, buf2, len2, start, end);
            break;
        case 2:
            result = ucs2lib_rfind_slice(buf1, len1, buf2, len2, start, end);
            break;
        default:
            result = ucs4lib_rfind_slice(buf1, len1, buf2, len2, start, end);
            break;
        }
    }

    if (tofree)
        js_free_rt(rt, tofree);
    return result;
}

/* PyUnicode_FindChar */
static Py_ssize_t qp_sl_find_char(qp_strview s, Py_UCS4 ch,
                                  Py_ssize_t start, Py_ssize_t end, int direction)
{
    Py_ssize_t result;

    qp_sl_adjust_indices(&start, &end, s.len);
    if (end - start < 1)
        return -1;
    result = qp_sl_findchar((const char *)s.data + (size_t)s.kind * start,
                            s.kind, end - start, ch, direction);
    if (result == -1)
        return -1;
    else
        return start + result;
}

/* unicode_count_impl */
static Py_ssize_t qp_sl_count(JSRuntime *rt, qp_strview s, qp_strview sub,
                              Py_ssize_t start, Py_ssize_t end)
{
    Py_ssize_t result, len1 = s.len, len2 = sub.len;
    const void *buf2;
    void *tofree;
    int r;

    qp_sl_adjust_indices(&start, &end, len1);
    if (end - start < len2)
        return 0;

    r = qp_sl_needle_askind(rt, sub, s.kind, &buf2, &tofree);
    if (r != 0)
        return r == 1 ? 0 : r;

    switch (s.kind) {
    case 1:
        result = ucs1lib_count(((const Py_UCS1*)s.data) + start, end - start,
                               buf2, len2, PY_SSIZE_T_MAX);
        break;
    case 2:
        result = ucs2lib_count(((const Py_UCS2*)s.data) + start, end - start,
                               buf2, len2, PY_SSIZE_T_MAX);
        break;
    default:
        result = ucs4lib_count(((const Py_UCS4*)s.data) + start, end - start,
                               buf2, len2, PY_SSIZE_T_MAX);
        break;
    }

    if (tofree)
        js_free_rt(rt, tofree);
    return result;
}

/* tailmatch (unicodeobject.c); _Py_bytes_tailmatch gives the same results
   for bytes. */
static int qp_sl_tailmatch(qp_strview self, qp_strview substring,
                           Py_ssize_t start, Py_ssize_t end, int direction)
{
    int kind_self, kind_sub;
    const void *data_self, *data_sub;
    Py_ssize_t offset, i, end_sub;

    qp_sl_adjust_indices(&start, &end, self.len);
    end -= substring.len;
    if (end < start)
        return 0;

    if (substring.len == 0)
        return 1;

    kind_self = self.kind;
    data_self = self.data;
    kind_sub = substring.kind;
    data_sub = substring.data;
    end_sub = substring.len - 1;

    if (direction > 0)
        offset = end;
    else
        offset = start;

    if (QP_SL_READ(kind_self, data_self, offset) ==
        QP_SL_READ(kind_sub, data_sub, 0) &&
        QP_SL_READ(kind_self, data_self, offset + end_sub) ==
        QP_SL_READ(kind_sub, data_sub, end_sub)) {
        /* If both are of the same kind, memcmp is sufficient */
        if (kind_self == kind_sub) {
            return ! memcmp((const char *)data_self +
                                ((size_t)offset * kind_sub),
                            data_sub,
                            (size_t)substring.len * kind_sub);
        }
        /* otherwise we have to compare each character by first accessing it */
        else {
            /* We do not need to compare 0 and len(substring)-1 because
               the if statement above ensured already that they are equal
               when we end up here. */
            for (i = 1; i < end_sub; ++i) {
                if (QP_SL_READ(kind_self, data_self, offset + i) !=
                    QP_SL_READ(kind_sub, data_sub, i))
                    return 0;
            }
            return 1;
        }
    }

    return 0;
}

/* ---- splitting ---------------------------------------------------------------- */

/* split() / rsplit() of unicodeobject.c (and bytes split/rsplit). */
static int qp_sl_split_impl(JSRuntime *rt, qp_strview s, const qp_strview *sep,
                            Py_ssize_t maxcount, int flags, qp_spanvec *out, int right)
{
    Py_ssize_t len1 = s.len, len2;
    const void *buf1 = s.data, *buf2;
    void *tofree;
    int r;

    if (sep == NULL) {
        if (maxcount < 0) {
            maxcount = (len1 - 1) / 2 + 1;
        }
        if (flags & QP_SL_BYTES) {
            assert(s.kind == 1);
            return right ? byteslib_rsplit_whitespace(buf1, len1, maxcount, out)
                         : byteslib_split_whitespace(buf1, len1, maxcount, out);
        }
        switch (s.kind) {
        case 1:
            return right ? ucs1lib_rsplit_whitespace(buf1, len1, maxcount, out)
                         : ucs1lib_split_whitespace(buf1, len1, maxcount, out);
        case 2:
            return right ? ucs2lib_rsplit_whitespace(buf1, len1, maxcount, out)
                         : ucs2lib_split_whitespace(buf1, len1, maxcount, out);
        default:
            return right ? ucs4lib_rsplit_whitespace(buf1, len1, maxcount, out)
                         : ucs4lib_split_whitespace(buf1, len1, maxcount, out);
        }
    }

    len2 = sep->len;
    if (maxcount < 0) {
        // if len2 == 0, it will raise ValueError.
        maxcount = len2 == 0 ? 0 : (len1 / len2) + 1;
        // handle expected overflow case: (Py_SSIZE_T_MAX / 1) + 1
        maxcount = maxcount < 0 ? len1 : maxcount;
    }
    if (len1 < len2)
        return qp_spanvec_push(out, 0, len1);
    r = qp_sl_needle_askind(rt, *sep, s.kind, &buf2, &tofree);
    if (r == 1)     /* CPython: kind1 < kind2 -> [self] */
        return qp_spanvec_push(out, 0, len1);
    if (r < 0)
        return r;

    switch (s.kind) {
    case 1:
        r = right ? ucs1lib_rsplit(buf1, len1, buf2, len2, maxcount, out)
                  : ucs1lib_split(buf1, len1, buf2, len2, maxcount, out);
        break;
    case 2:
        r = right ? ucs2lib_rsplit(buf1, len1, buf2, len2, maxcount, out)
                  : ucs2lib_split(buf1, len1, buf2, len2, maxcount, out);
        break;
    default:
        r = right ? ucs4lib_rsplit(buf1, len1, buf2, len2, maxcount, out)
                  : ucs4lib_split(buf1, len1, buf2, len2, maxcount, out);
        break;
    }
    if (tofree)
        js_free_rt(rt, tofree);
    return r;
}

static int qp_sl_split(JSRuntime *rt, qp_strview s, const qp_strview *sep,
                       Py_ssize_t maxsplit, int flags, qp_spanvec *out)
{
    return qp_sl_split_impl(rt, s, sep, maxsplit, flags, out, 0);
}

static int qp_sl_rsplit(JSRuntime *rt, qp_strview s, const qp_strview *sep,
                        Py_ssize_t maxsplit, int flags, qp_spanvec *out)
{
    return qp_sl_split_impl(rt, s, sep, maxsplit, flags, out, 1);
}

/* PyUnicode_Splitlines / bytes splitlines */
static int qp_sl_splitlines(qp_strview s, int keepends, int flags, qp_spanvec *out)
{
    if (flags & QP_SL_BYTES) {
        assert(s.kind == 1);
        return byteslib_splitlines(s.data, s.len, keepends, out);
    }
    switch (s.kind) {
    case 1:
        return ucs1lib_splitlines(s.data, s.len, keepends, out);
    case 2:
        return ucs2lib_splitlines(s.data, s.len, keepends, out);
    default:
        return ucs4lib_splitlines(s.data, s.len, keepends, out);
    }
}

/* PyUnicode_Partition / PyUnicode_RPartition (and bytes). */
static int qp_sl_partition(JSRuntime *rt, qp_strview s, qp_strview sep,
                           int direction, qp_span out[3])
{
    Py_ssize_t len1 = s.len, len2 = sep.len;
    const void *buf2;
    void *tofree;
    int r;

    if (len1 < len2)
        goto not_found;
    r = qp_sl_needle_askind(rt, sep, s.kind, &buf2, &tofree);
    if (r == 1)     /* CPython: kind1 < kind2 */
        goto not_found;
    if (r < 0)
        return r;

    switch (s.kind) {
    case 1:
        r = direction > 0 ? ucs1lib_partition(s.data, len1, buf2, len2, out)
                          : ucs1lib_rpartition(s.data, len1, buf2, len2, out);
        break;
    case 2:
        r = direction > 0 ? ucs2lib_partition(s.data, len1, buf2, len2, out)
                          : ucs2lib_rpartition(s.data, len1, buf2, len2, out);
        break;
    default:
        r = direction > 0 ? ucs4lib_partition(s.data, len1, buf2, len2, out)
                          : ucs4lib_rpartition(s.data, len1, buf2, len2, out);
        break;
    }
    if (tofree)
        js_free_rt(rt, tofree);
    return r;

 not_found:
    if (direction > 0) {
        out[0].start = 0;    out[0].end = len1;
        out[1].start = len1; out[1].end = len1;
        out[2].start = len1; out[2].end = len1;
    }
    else {
        out[0].start = 0; out[0].end = 0;
        out[1].start = 0; out[1].end = 0;
        out[2].start = 0; out[2].end = len1;
    }
    return 0;
}

/* ---- stripping ---------------------------------------------------------------- */

/* do_strip (str) / do_strip (bytes, Py_ISSPACE) */
static void qp_sl_strip_ws(qp_strview s, int striptype, int flags, qp_span *out)
{
    Py_ssize_t len = s.len, i, j;
    int kind = s.kind;
    const void *data = s.data;
    int bytes = (flags & QP_SL_BYTES) != 0;

#define QP_SL_STRIP_ISSPACE(ch) \
    (bytes ? QP_ISSPACE(ch) != 0 : QP_SL_UNICODE_ISSPACE(ch) != 0)

    i = 0;
    if (striptype != QP_SL_RIGHTSTRIP) {
        while (i < len) {
            Py_UCS4 ch = QP_SL_READ(kind, data, i);
            if (!QP_SL_STRIP_ISSPACE(ch))
                break;
            i++;
        }
    }

    j = len;
    if (striptype != QP_SL_LEFTSTRIP) {
        j--;
        while (j >= i) {
            Py_UCS4 ch = QP_SL_READ(kind, data, j);
            if (!QP_SL_STRIP_ISSPACE(ch))
                break;
            j--;
        }
        j++;
    }
#undef QP_SL_STRIP_ISSPACE

    out->start = i;
    out->end = j;
}

/* make_bloom_mask */
static unsigned long qp_sl_make_bloom_mask(int kind, const void *ptr, Py_ssize_t len)
{
    unsigned long mask = 0;
    Py_ssize_t i;

    for (i = 0; i < len; i++)
        mask |= QP_SL_BLOOM_BIT(QP_SL_READ(kind, ptr, i));
    return mask;
}

/* _PyUnicode_XStrip (bytes do_xstrip gives the same spans) */
static void qp_sl_strip_chars(qp_strview s, qp_strview sepobj, int striptype, qp_span *out)
{
    const void *data = s.data;
    int kind = s.kind;
    Py_ssize_t i, j, len = s.len;
    unsigned long sepmask;
    Py_ssize_t seplen = sepobj.len;

    sepmask = qp_sl_make_bloom_mask(sepobj.kind, sepobj.data, seplen);

    i = 0;
    if (striptype != QP_SL_RIGHTSTRIP) {
        while (i < len) {
            Py_UCS4 ch = QP_SL_READ(kind, data, i);
            if (!QP_SL_BLOOM(sepmask, ch))
                break;
            if (qp_sl_find_char(sepobj, ch, 0, seplen, 1) < 0)
                break;
            i++;
        }
    }

    j = len;
    if (striptype != QP_SL_LEFTSTRIP) {
        j--;
        while (j >= i) {
            Py_UCS4 ch = QP_SL_READ(kind, data, j);
            if (!QP_SL_BLOOM(sepmask, ch))
                break;
            if (qp_sl_find_char(sepobj, ch, 0, seplen, 1) < 0)
                break;
            j--;
        }

        j++;
    }

    out->start = i;
    out->end = j;
}

/* ---- replace ------------------------------------------------------------------- */

/* replace_1char_inplace (unicodeobject.c) on the builder's buffer. */
static void qp_sl_replace_1char_inplace(qp_ucsbuilder *u, Py_ssize_t pos,
                                        Py_UCS4 u1, Py_UCS4 u2, Py_ssize_t maxcount)
{
    int kind = u->kind;
    void *data = u->data;
    Py_ssize_t len = u->len;
    if (kind == 1) {
        ucs1lib_replace_1char_inplace((Py_UCS1 *)data + pos,
                                      (Py_UCS1 *)data + len,
                                      u1, u2, maxcount);
    }
    else if (kind == 2) {
        ucs2lib_replace_1char_inplace((Py_UCS2 *)data + pos,
                                      (Py_UCS2 *)data + len,
                                      u1, u2, maxcount);
    }
    else {
        assert(kind == 4);
        ucs4lib_replace_1char_inplace((Py_UCS4 *)data + pos,
                                      (Py_UCS4 *)data + len,
                                      u1, u2, maxcount);
    }
}

/* replace() of unicodeobject.c.  Before CPython's algorithm runs, old and
   new are brought to their minimal kind (they are usually short), so the
   kind bookkeeping below is CPython's; `self` keeps its storage kind (its
   max char value is taken as that kind's maximum). */
static int qp_sl_replace(qp_strview self, qp_strview str1, qp_strview str2,
                         Py_ssize_t maxcount, qp_ucsbuilder *u)
{
    JSRuntime *rt = u->rt;
    const char *sbuf = self.data;
    const void *buf1 = str1.data;
    const void *buf2 = str2.data;
    void *stmp = NULL;              /* widened self (CPython srelease) */
    void *tmp1 = NULL, *tmp2 = NULL; /* widened str1/str2 (release1/2) */
    void *norm1 = NULL, *norm2 = NULL; /* narrowed str1/str2 */
    int skind = self.kind;
    int kind1 = str1.kind;
    int kind2 = str2.kind;
    Py_ssize_t slen = self.len;
    Py_ssize_t len1 = str1.len;
    Py_ssize_t len2 = str2.len;
    int mayshrink;
    Py_UCS4 maxchar, maxchar_str1, maxchar_str2;
    int ret = QP_SL_BUILT;

    if (slen < len1)
        goto nothing;

    if (maxcount < 0)
        maxcount = PY_SSIZE_T_MAX;
    else if (maxcount == 0)
        goto nothing;

    /* str1 == str2: the same object */
    if (str1.data == str2.data && kind1 == kind2 && len1 == len2)
        goto nothing;

    maxchar = qp_sl_kind_maxchar(skind);
    maxchar_str1 = qp_sl_find_max_char(str1);
    if (maxchar < maxchar_str1)
        /* substring too wide to be present */
        goto nothing;
    maxchar_str2 = qp_sl_find_max_char(str2);

    /* minimal kinds for str1 / str2 */
    if (kind1 > qp_sl_kind_for_maxchar(maxchar_str1)) {
        int k = qp_sl_kind_for_maxchar(maxchar_str1);
        norm1 = qp_sl_askind(rt, kind1, buf1, len1, k);
        if (!norm1) goto error;
        buf1 = norm1;
        kind1 = k;
    }
    if (kind2 > qp_sl_kind_for_maxchar(maxchar_str2)) {
        int k = qp_sl_kind_for_maxchar(maxchar_str2);
        norm2 = qp_sl_askind(rt, kind2, buf2, len2, k);
        if (!norm2) goto error;
        buf2 = norm2;
        kind2 = k;
    }

    /* Replacing str1 with str2 may cause a maxchar reduction in the
       result string. */
    mayshrink = (kind2 < kind1) && (skind == kind1);
    maxchar = QP_SL_MAX(maxchar, maxchar_str2);

    if (len1 == len2) {
        /* same length */
        if (len1 == 0)
            goto nothing;
        if (len1 == 1) {
            /* replace characters */
            Py_UCS4 u1, u2;
            Py_ssize_t pos;

            u1 = QP_SL_READ(kind1, buf1, 0);
            pos = qp_sl_findchar(sbuf, skind, slen, u1, 1);
            if (pos < 0)
                goto nothing;
            u2 = QP_SL_READ(kind2, buf2, 0);
            if (qp_ucsbuilder_alloc(u, slen, qp_sl_kind_for_maxchar(maxchar)) < 0)
                goto error;

            qp_sl_convert(u->kind, u->data, skind, sbuf, slen);
            qp_sl_replace_1char_inplace(u, pos, u1, u2, maxcount);
        }
        else {
            int rkind = skind;
            char *res;
            Py_ssize_t i;

            if (kind1 < rkind) {
                /* widen substring */
                tmp1 = qp_sl_askind(rt, kind1, buf1, len1, rkind);
                if (!tmp1) goto error;
                buf1 = tmp1;
            }
            i = qp_sl_anylib_find(rkind, sbuf, slen, buf1, len1, 0);
            if (i < 0)
                goto nothing;
            if (rkind > kind2) {
                /* widen replacement */
                tmp2 = qp_sl_askind(rt, kind2, buf2, len2, rkind);
                if (!tmp2) goto error;
                buf2 = tmp2;
            }
            else if (rkind < kind2) {
                /* widen self and buf1 */
                rkind = kind2;
                if (tmp1) {
                    js_free_rt(rt, tmp1);
                    tmp1 = NULL;
                    buf1 = norm1 ? norm1 : str1.data;
                }
                stmp = qp_sl_askind(rt, skind, sbuf, slen, rkind);
                if (!stmp) goto error;
                sbuf = stmp;
                tmp1 = qp_sl_askind(rt, kind1, buf1, len1, rkind);
                if (!tmp1) goto error;
                buf1 = tmp1;
            }
            if (qp_ucsbuilder_alloc(u, slen, qp_sl_kind_for_maxchar(maxchar)) < 0)
                goto error;
            assert(u->kind == rkind);
            res = u->data;

            memcpy(res, sbuf, (size_t)rkind * slen);
            /* change everything in-place, starting with this one */
            memcpy(res + (size_t)rkind * i,
                   buf2,
                   (size_t)rkind * len2);
            i += len1;

            while ( --maxcount > 0) {
                i = qp_sl_anylib_find(rkind,
                                      sbuf+(size_t)rkind*i, slen-i,
                                      buf1, len1, i);
                if (i == -1)
                    break;
                memcpy(res + (size_t)rkind * i,
                       buf2,
                       (size_t)rkind * len2);
                i += len1;
            }
        }
    }
    else {
        Py_ssize_t n, i, j, ires;
        Py_ssize_t new_size;
        int rkind = skind;
        char *res;

        if (kind1 < rkind) {
            /* widen substring */
            tmp1 = qp_sl_askind(rt, kind1, buf1, len1, rkind);
            if (!tmp1) goto error;
            buf1 = tmp1;
        }
        n = qp_sl_anylib_count(rkind, sbuf, slen, buf1, len1, maxcount);
        if (n == 0)
            goto nothing;
        if (kind2 < rkind) {
            /* widen replacement */
            tmp2 = qp_sl_askind(rt, kind2, buf2, len2, rkind);
            if (!tmp2) goto error;
            buf2 = tmp2;
        }
        else if (kind2 > rkind) {
            /* widen self and buf1 */
            rkind = kind2;
            stmp = qp_sl_askind(rt, skind, sbuf, slen, rkind);
            if (!stmp) goto error;
            sbuf = stmp;
            if (tmp1) {
                js_free_rt(rt, tmp1);
                tmp1 = NULL;
                buf1 = norm1 ? norm1 : str1.data;
            }
            tmp1 = qp_sl_askind(rt, kind1, buf1, len1, rkind);
            if (!tmp1) goto error;
            buf1 = tmp1;
        }
        /* new_size = PyUnicode_GET_LENGTH(self) + n * (PyUnicode_GET_LENGTH(str2) -
           PyUnicode_GET_LENGTH(str1)); */
        if (len1 < len2 && len2 - len1 > (PY_SSIZE_T_MAX - slen) / n) {
            ret = QP_SL_OVERFLOW;       /* "replace string is too long" */
            goto done;
        }
        new_size = slen + n * (len2 - len1);
        if (new_size == 0) {
            qp_ucsbuilder_alloc(u, 0, 1);   /* the empty string */
            goto done;
        }
        if (new_size > (PY_SSIZE_T_MAX / rkind)) {
            ret = QP_SL_OVERFLOW;       /* "replace string is too long" */
            goto done;
        }
        if (qp_ucsbuilder_alloc(u, new_size, qp_sl_kind_for_maxchar(maxchar)) < 0)
            goto error;
        assert(u->kind == rkind);
        res = u->data;
        ires = i = 0;
        if (len1 > 0) {
            while (n-- > 0) {
                /* look for next match */
                j = qp_sl_anylib_find(rkind,
                                      sbuf + (size_t)rkind * i, slen-i,
                                      buf1, len1, i);
                if (j == -1)
                    break;
                else if (j > i) {
                    /* copy unchanged part [i:j] */
                    memcpy(res + (size_t)rkind * ires,
                           sbuf + (size_t)rkind * i,
                           (size_t)rkind * (j-i));
                    ires += j - i;
                }
                /* copy substitution string */
                if (len2 > 0) {
                    memcpy(res + (size_t)rkind * ires,
                           buf2,
                           (size_t)rkind * len2);
                    ires += len2;
                }
                i = j + len1;
            }
            if (i < slen)
                /* copy tail [i:] */
                memcpy(res + (size_t)rkind * ires,
                       sbuf + (size_t)rkind * i,
                       (size_t)rkind * (slen-i));
        }
        else {
            /* interleave */
            while (n > 0) {
                memcpy(res + (size_t)rkind * ires,
                       buf2,
                       (size_t)rkind * len2);
                ires += len2;
                if (--n <= 0)
                    break;
                memcpy(res + (size_t)rkind * ires,
                       sbuf + (size_t)rkind * i,
                       rkind);
                ires++;
                i++;
            }
            memcpy(res + (size_t)rkind * ires,
                   sbuf + (size_t)rkind * i,
                   (size_t)rkind * (slen-i));
        }
    }

    if (mayshrink)
        qp_ucsbuilder_narrow(u);
    goto done;

  nothing:
    /* nothing to replace; return original string (when possible) */
    ret = QP_SL_UNCHANGED;
    goto done;

  error:
    ret = QP_SL_NOMEM;
  done:
    if (stmp)
        js_free_rt(rt, stmp);
    if (tmp1)
        js_free_rt(rt, tmp1);
    if (tmp2)
        js_free_rt(rt, tmp2);
    if (norm1)
        js_free_rt(rt, norm1);
    if (norm2)
        js_free_rt(rt, norm2);
    if (ret != QP_SL_BUILT)
        qp_ucsbuilder_free(u);
    return ret;
}

/* ---- expandtabs / pad / join ---------------------------------------------------- */

/* unicode_expandtabs_impl (bytes: stringlib_expandtabs_impl, same output) */
static int qp_sl_expandtabs(qp_strview self, int tabsize, qp_ucsbuilder *u)
{
    Py_ssize_t i, j, line_pos, src_len, incr;
    Py_UCS4 ch;
    const void *src_data;
    void *dest_data;
    int kind;
    int found;

    /* First pass: determine size of output string */
    src_len = self.len;
    i = j = line_pos = 0;
    kind = self.kind;
    src_data = self.data;
    found = 0;
    for (; i < src_len; i++) {
        ch = QP_SL_READ(kind, src_data, i);
        if (ch == '\t') {
            found = 1;
            if (tabsize > 0) {
                incr = tabsize - (line_pos % tabsize); /* cannot overflow */
                if (j > PY_SSIZE_T_MAX - incr)
                    goto overflow;
                line_pos += incr;
                j += incr;
            }
        }
        else {
            if (j > PY_SSIZE_T_MAX - 1)
                goto overflow;
            line_pos++;
            j++;
            if (ch == '\n' || ch == '\r')
                line_pos = 0;
        }
    }
    if (!found)
        return QP_SL_UNCHANGED;

    /* Second pass: create output string and fill it */
    if (qp_ucsbuilder_alloc(u, j, kind) < 0)
        return QP_SL_NOMEM;
    dest_data = u->data;

    i = j = line_pos = 0;

    for (; i < src_len; i++) {
        ch = QP_SL_READ(kind, src_data, i);
        if (ch == '\t') {
            if (tabsize > 0) {
                incr = tabsize - (line_pos % tabsize);
                line_pos += incr;
                qp_sl_fill(kind, dest_data, ' ', j, incr);
                j += incr;
            }
        }
        else {
            line_pos++;
            QP_SL_WRITE(kind, dest_data, j, ch);
            j++;
            if (ch == '\n' || ch == '\r')
                line_pos = 0;
        }
    }
    assert (j == u->len);
    return QP_SL_BUILT;

  overflow:
    /* str: "new string is too long"; bytes: "result too long" */
    return QP_SL_OVERFLOW;
}

/* pad() of unicodeobject.c (transmogrify.h pad() for bytes) */
static int qp_sl_pad(qp_strview self, Py_ssize_t left, Py_ssize_t right,
                     Py_UCS4 fill, qp_ucsbuilder *u)
{
    Py_UCS4 maxchar;
    int kind;
    void *data;

    if (left < 0)
        left = 0;
    if (right < 0)
        right = 0;

    if (left == 0 && right == 0)
        return QP_SL_UNCHANGED;

    if (left > PY_SSIZE_T_MAX - self.len ||
        right > PY_SSIZE_T_MAX - (left + self.len)) {
        /* "padded string is too long" */
        return QP_SL_OVERFLOW;
    }
    maxchar = qp_sl_kind_maxchar(self.kind);
    maxchar = QP_SL_MAX(maxchar, fill);
    if (qp_ucsbuilder_alloc(u, left + self.len + right,
                            qp_sl_kind_for_maxchar(maxchar)) < 0)
        return QP_SL_NOMEM;

    kind = u->kind;
    data = u->data;
    if (left)
        qp_sl_fill(kind, data, fill, 0, left);
    if (right)
        qp_sl_fill(kind, data, fill, left + self.len, right);
    qp_sl_convert(kind, (char *)data + (size_t)left * kind, self.kind, self.data, self.len);
    return QP_SL_BUILT;
}

/* unicode_center_impl */
static int qp_sl_center(qp_strview self, Py_ssize_t width, Py_UCS4 fillchar, qp_ucsbuilder *u)
{
    Py_ssize_t marg, left;

    if (self.len >= width)
        return QP_SL_UNCHANGED;

    marg = width - self.len;
    left = marg / 2 + (marg & width & 1);

    return qp_sl_pad(self, left, marg - left, fillchar, u);
}

/* unicode_ljust_impl */
static int qp_sl_ljust(qp_strview self, Py_ssize_t width, Py_UCS4 fillchar, qp_ucsbuilder *u)
{
    if (self.len >= width)
        return QP_SL_UNCHANGED;

    return qp_sl_pad(self, 0, width - self.len, fillchar, u);
}

/* unicode_rjust_impl */
static int qp_sl_rjust(qp_strview self, Py_ssize_t width, Py_UCS4 fillchar, qp_ucsbuilder *u)
{
    if (self.len >= width)
        return QP_SL_UNCHANGED;

    return qp_sl_pad(self, width - self.len, 0, fillchar, u);
}

/* unicode_zfill_impl */
static int qp_sl_zfill(qp_strview self, Py_ssize_t width, qp_ucsbuilder *u)
{
    Py_ssize_t fill;
    int kind, ret;
    void *data;
    Py_UCS4 chr;

    if (self.len >= width)
        return QP_SL_UNCHANGED;

    fill = width - self.len;

    ret = qp_sl_pad(self, fill, 0, '0', u);

    if (ret != QP_SL_BUILT)
        return ret;

    kind = u->kind;
    data = u->data;
    /* CPython reads the terminating NUL when self is empty */
    if (fill < u->len) {
        chr = QP_SL_READ(kind, data, fill);

        if (chr == '+' || chr == '-') {
            /* move sign to beginning of string */
            QP_SL_WRITE(kind, data, 0, chr);
            QP_SL_WRITE(kind, data, fill, '0');
        }
    }

    return QP_SL_BUILT;
}

/* _PyUnicode_JoinArray, from the length pre-pass on (bytes: the same layout
   as stringlib/join.h) */
static int qp_sl_join(const qp_strview *separator, const qp_strview *items,
                      Py_ssize_t seqlen, qp_ucsbuilder *u)
{
    static const Py_UCS1 space = ' ';
    qp_strview sep;
    Py_ssize_t seplen, sz, i, res_offset;
    Py_UCS4 maxchar, item_maxchar;
    int kind;
    char *res_data;

    sep = qp_strview_make(1, &space, 0);
    if (seqlen == 1) {
        seplen = 0;
        maxchar = 0;
    }
    else {
        /* Set up sep and seplen */
        if (separator == NULL) {
            /* fall back to a blank space separator */
            sep = qp_strview_make(1, &space, 1);
            seplen = 1;
            maxchar = 32;
        }
        else {
            sep = *separator;
            seplen = sep.len;
            maxchar = qp_sl_kind_maxchar(sep.kind);
        }
    }

    /* Do a pre-pass to figure out the total amount of space we'll need (sz) */
    sz = 0;
    for (i = 0; i < seqlen; i++) {
        size_t add_sz;
        add_sz = items[i].len;
        item_maxchar = qp_sl_kind_maxchar(items[i].kind);
        maxchar = QP_SL_MAX(maxchar, item_maxchar);
        if (i != 0) {
            add_sz += seplen;
        }
        if (add_sz > (size_t)(PY_SSIZE_T_MAX - sz)) {
            /* "join() result is too long for a Python string" */
            return QP_SL_OVERFLOW;
        }
        sz += add_sz;
    }

    if (qp_ucsbuilder_alloc(u, sz, qp_sl_kind_for_maxchar(maxchar)) < 0)
        return QP_SL_NOMEM;

    /* Catenate everything. */
    kind = u->kind;
    res_data = u->data;
    for (i = 0, res_offset = 0; i < seqlen; ++i) {
        Py_ssize_t itemlen;

        /* Copy item, and maybe the separator. */
        if (i && seplen != 0) {
            qp_sl_convert(kind, res_data + (size_t)res_offset * kind,
                          sep.kind, sep.data, seplen);
            res_offset += seplen;
        }

        itemlen = items[i].len;
        if (itemlen != 0) {
            qp_sl_convert(kind, res_data + (size_t)res_offset * kind,
                          items[i].kind, items[i].data, itemlen);
            res_offset += itemlen;
        }
    }
    assert(res_offset == u->len);
    return QP_SL_BUILT;
}

/* ---- UTF-8 ------------------------------------------------------------------------ */

/* unicode_decode_utf8 / unicode_decode_utf8_impl with the 'strict' error
   handler. */
static int qp_sl_utf8_decode(const char *s, Py_ssize_t size, Py_ssize_t *consumed,
                             qp_ucsbuilder *writer, qp_codec_error *err)
{
    const char *starts = s;
    const char *end = s + size;
    Py_ssize_t startinpos, endinpos;
    const char *errmsg = "";

    if (size == 0) {
        if (consumed)
            *consumed = 0;
        return 0;
    }

    /* Each code point takes at least one byte: reserve size units once (the
       decoders write without bounds checks). */
    if (qp_ucsbuilder_prepare(writer, size, 0) < 0)
        return QP_SL_NOMEM;

    while (s < end) {
        Py_UCS4 ch;
        int kind = writer->kind;

        if (kind == 1) {
            ch = ucs1lib_utf8_decode(&s, end, writer->data, &writer->len);
        } else if (kind == 2) {
            ch = ucs2lib_utf8_decode(&s, end, writer->data, &writer->len);
        } else {
            assert(kind == 4);
            ch = ucs4lib_utf8_decode(&s, end, writer->data, &writer->len);
        }

        switch (ch) {
        case 0:
            if (s == end || consumed)
                goto End;
            errmsg = "unexpected end of data";
            startinpos = s - starts;
            endinpos = end - starts;
            break;
        case 1:
            errmsg = "invalid start byte";
            startinpos = s - starts;
            endinpos = startinpos + 1;
            break;
        case 2:
            if (consumed && (unsigned char)s[0] == 0xED && end - s == 2
                && (unsigned char)s[1] >= 0xA0 && (unsigned char)s[1] <= 0xBF)
            {
                /* Truncated surrogate code in range D800-DFFF */
                goto End;
            }
            /* fall through */
        case 3:
        case 4:
            errmsg = "invalid continuation byte";
            startinpos = s - starts;
            endinpos = startinpos + ch - 1;
            break;
        default:
            // ch doesn't fit into kind, so change the buffer kind to write
            // the character (room for the rest of the input is kept)
            if (qp_ucsbuilder_prepare(writer, 1 + (end - s), ch) < 0)
                return QP_SL_NOMEM;
            QP_SL_WRITE(writer->kind, writer->data, writer->len, ch);
            writer->len++;
            continue;
        }

        /* 'strict' */
        err->start = startinpos;
        err->end = endinpos;
        err->reason = errmsg;
        return QP_SL_CODEC_ERROR;
    }

End:
    if (consumed)
        *consumed = s - starts;
    return 0;
}

/* unicode_encode_utf8 with the 'strict' error handler. */
static int qp_sl_utf8_encode(JSRuntime *rt, qp_strview s, char **pdata, Py_ssize_t *plen,
                             qp_codec_error *err)
{
    Py_ssize_t size = s.len, max_char_size, err_start = -1, err_end = -1;
    char *buf, *p, *shrunk;

    *pdata = NULL;
    *plen = 0;
    if (size == 0)
        return 0;
    max_char_size = s.kind == 1 ? 2 : s.kind == 2 ? 3 : 4;
    if (size > PY_SSIZE_T_MAX / max_char_size) {
        /* integer overflow */
        return QP_SL_NOMEM;
    }
    buf = js_malloc_rt(rt, (size_t)size * max_char_size);
    if (!buf)
        return QP_SL_NOMEM;

    switch (s.kind) {
    case 1:
        p = ucs1lib_utf8_encoder(s.data, size, buf, &err_start, &err_end);
        break;
    case 2:
        p = ucs2lib_utf8_encoder(s.data, size, buf, &err_start, &err_end);
        break;
    default:
        p = ucs4lib_utf8_encoder(s.data, size, buf, &err_start, &err_end);
        break;
    }
    if (p == NULL) {
        js_free_rt(rt, buf);
        err->start = err_start;
        err->end = err_end;
        err->reason = "surrogates not allowed";
        return QP_SL_CODEC_ERROR;
    }

    *plen = p - buf;
    if (*plen == 0) {
        js_free_rt(rt, buf);
        return 0;
    }
    /* give back the over-allocation (keep the buffer if that fails) */
    shrunk = js_realloc_rt(rt, buf, (size_t)*plen);
    *pdata = shrunk ? shrunk : buf;
    return 0;
}

/* ---- end of the unit-local configuration -------------------------------------- */

#undef QP_SL_LOCAL_INLINE
#undef QP_SL_MAX
#undef QP_SL_MIN
#undef QP_SL_ALIGN_DOWN
#undef QP_SL_IS_ALIGNED
#undef QP_SL_SIZEOF_SIZE_T
#undef QP_SL_LITTLE_ENDIAN
#undef QP_SL_SIZEOF_WCHAR_T
#undef QP_SL_MAX_UNICODE
#undef QP_SL_IS_SURROGATE
#undef QP_SL_BLOOM_WIDTH
#undef QP_SL_BLOOM_BIT
#undef QP_SL_BLOOM
#undef QP_SL_BLOOM_LINEBREAK_MASK
#undef QP_SL_UNICODE_ISSPACE
#undef QP_SL_BLOOM_LINEBREAK
#undef QP_SL_WRITE
#undef QP_SL_SPANVEC_MIN
#undef STRINGLIB_FASTSEARCH_H
#undef FAST_COUNT
#undef FAST_SEARCH
#undef FAST_RSEARCH
#undef STRINGLIB_BLOOM_WIDTH
#undef STRINGLIB_BLOOM_ADD
#undef STRINGLIB_BLOOM
#undef MAX_SHIFT
#undef UCS1_ASCII_CHAR_MASK
#undef ASCII_CHAR_MASK
#undef IS_CONTINUATION_BYTE
