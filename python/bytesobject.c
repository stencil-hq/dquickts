/*
 * bytes type, shared bytes/bytearray methods, buffer protocol, bytes %
 * formatting (PEP 461) and unit-g initialization.
 *
 * Derived from CPython 3.15 Objects/bytesobject.c, Objects/bytes_methods.c,
 * Objects/stringlib/{join,partition,split,transmogrify}.h and
 * Python/pystrhex.c (PSF-2.0).  The string algorithms are the shared
 * stringlib.h ports over kind-1 views (flag QP_SL_BYTES).
 *
 * Methods shared by bytes and bytearray (qp_bm_*) look at the payload kind
 * of `self` to build results of the right type, like CPython's per-type
 * copies of the same stringlib code.  For bytearray `self`, the data
 * pointer is re-read after every argument conversion that may run Python
 * code (__index__, __buffer__), and buffer arguments are held as exports.
 */
#include "bytesobject_internal.h"

/* ------------------------------------------------------------------------ */
/* small helpers                                                             */
/* ------------------------------------------------------------------------ */

#define QPB_ISSPACE(c) ((c) == ' ' || ((c) >= '\t' && (c) <= '\r'))
#define QPB_ISLOWER(c) ((c) >= 'a' && (c) <= 'z')
#define QPB_ISUPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define QPB_ISALPHA(c) (QPB_ISLOWER(c) || QPB_ISUPPER(c))
#define QPB_ISDIGIT(c) ((c) >= '0' && (c) <= '9')
#define QPB_ISALNUM(c) (QPB_ISALPHA(c) || QPB_ISDIGIT(c))
#define QPB_TOLOWER(c) (QPB_ISUPPER(c) ? (c) + 32 : (c))
#define QPB_TOUPPER(c) (QPB_ISLOWER(c) ? (c) - 32 : (c))

/* qp_hexdigits ("0123456789abcdef") is defined by unicodeobject.c (same TU) */

/* Py_TYPE(v)->tp_name as a str: the spec name of builtin types, __name__ of
   heap types. */
static JSValue qp_bm_tpname(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* raise type(fmt % tpname(v)): fmt contains exactly one %U. */
static JSValue qp_bm_raise_tpname(JSContext *ctx, QPType *type, const char *fmt, JSValueConst v)
{
    JSValue n = qp_bm_tpname(ctx, v);
    if (JS_IsException(n))
        return n;
    qp_raise(ctx, type, fmt, n);
    JS_FreeValue(ctx, n);
    return JS_EXCEPTION;
}

/* _PyArg_BadArgument: "%s() %s must be %s, not %s". */
static JSValue qp_bm_bad_argument(JSContext *ctx, const char *fname, const char *argdesc,
                                  const char *expected, JSValueConst v)
{
    JSValue n;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        n = qp_str_from_cstr(ctx, "None");
    else
        n = qp_bm_tpname(ctx, v);
    if (JS_IsException(n))
        return n;
    qp_raise_type_error(ctx, "%s() %s must be %s, not %U", fname, argdesc, expected, n);
    JS_FreeValue(ctx, n);
    return JS_EXCEPTION;
}

/* If the pending exception is an instance of `type`, discard it and return
   1; else leave it pending and return 0. */
static int qp_bm_exc_clear_if(JSContext *ctx, QPType *type)
{
    if (!qp_exc_pending_matches(ctx, type))
        return 0;
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
    return 1;
}

static void qp_bm_exc_clear(JSContext *ctx)
{
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
}

/* _PyIndex_Check */
static BOOL qp_index_check(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
}

/* PyNumber_Check */
static BOOL qp_bm_number_check(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    return t->slots.nb.nb_index || t->slots.nb.nb_int || t->slots.nb.nb_float ||
        qp_complex_check(ctx, v);
}

static BOOL qp_bm_is_typed_array_class(int class_id)
{
    return (class_id >= JS_CLASS_UINT8C_ARRAY && class_id <= JS_CLASS_FLOAT64_ARRAY) ||
        class_id == JS_CLASS_DATAVIEW;
}

/* PyObject_CheckBuffer */
static BOOL qp_buffer_check(JSContext *ctx, JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    {
        int cid = JS_VALUE_GET_OBJ(v)->class_id;
        if (qp_bm_is_typed_array_class(cid) || cid == JS_CLASS_ARRAY_BUFFER ||
            cid == JS_CLASS_SHARED_ARRAY_BUFFER)
            return TRUE;
    }
    return qp_type_of(ctx, v)->slots.bf.bf_getbuffer != NULL;
}

/* _PyEval_SliceIndex: None leaves *pi unchanged. */
static int qp_bm_slice_index(JSContext *ctx, JSValueConst v, Py_ssize_t *pi)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!qp_index_check(ctx, v)) {
        qp_raise_type_error(ctx, "slice indices must be integers or None or have an __index__ method");
        return -1;
    }
    return qp_as_ssize(ctx, v, NULL, pi);
}

#define QP_ADJUST_INDICES(start, end, len) \
    do {                                   \
        if ((end) > (len))                 \
            (end) = (len);                 \
        else if ((end) < 0) {              \
            (end) += (len);                \
            if ((end) < 0)                 \
                (end) = 0;                 \
        }                                  \
        if ((start) < 0) {                 \
            (start) += (len);              \
            if ((start) < 0)               \
                (start) = 0;               \
        }                                  \
    } while (0)

/* Convert a stringlib status to an exception. */
static JSValue qp_bm_sl_error(JSContext *ctx, int st, const char *overflow_msg)
{
    switch (st) {
    case QP_SL_OVERFLOW:
        return qp_raise_overflow_error(ctx, "%s", overflow_msg);
    case QP_SL_EMPTY_SEP:
        return qp_raise_value_error(ctx, "empty separator");
    default:
        return qp_raise_memory_error(ctx);
    }
}

/* ------------------------------------------------------------------------ */
/* object creation                                                           */
/* ------------------------------------------------------------------------ */

static BOOL qp_bytes_check(JSContext *ctx, JSValueConst v)
{
    return qp_bk_kind(v) == QP_BK_BYTES;
}

static BOOL qp_bytearray_check(JSContext *ctx, JSValueConst v)
{
    return qp_bk_kind(v) == QP_BK_BYTEARRAY;
}

static BOOL qp_bytes_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_bk_kind(v) == QP_BK_BYTES &&
        JS_VALUE_GET_OBJ(v)->shape->proto == QP_TYPE(ctx, bytes)->obj;
}

static const uint8_t *qp_bytes_data(JSValueConst v, Py_ssize_t *plen)
{
    int k = qp_bk_kind(v);
    QPBytesObject *b;
    if (k != QP_BK_BYTES && k != QP_BK_BYTEARRAY) {
        *plen = 0;
        return NULL;
    }
    b = QP_BYTES_OBJ(v);
    *plen = b->len;
    return b->data ? b->data : qp_bytes_empty_data;
}

/* New bytes object of `subtype` (NULL or bytes = exact) holding a copy of
   data[0..len) (data may be NULL: zero-filled). */
static JSValue qp_bytes_new_type(JSContext *ctx, QPType *subtype, const void *data, Py_ssize_t len)
{
    QPBytesObject *b;
    JSValue obj, *pchar = NULL;
    QPType *bt = QP_TYPE(ctx, bytes);

    if (len < 0 || (size_t)len > (size_t)PY_SSIZE_T_MAX - sizeof(QPBytesObject) - 1)
        return qp_raise_overflow_error(ctx, "byte string is too large");
    if (subtype == NULL || subtype == bt) {
        subtype = NULL;
        if (len == 0 && JS_VALUE_GET_TAG(QP_STATE(ctx)->empty_bytes) == JS_TAG_OBJECT)
            return JS_DupValue(ctx, QP_STATE(ctx)->empty_bytes);
        /* PyBytes_FromStringAndSize(str, 1): the singleton of the byte
           (not for data == NULL: the caller fills the new object) */
        if (len == 1 && data) {
            pchar = &QP_STATE(ctx)->bytes_chars[*(const uint8_t *)data];
            if (JS_VALUE_GET_TAG(*pchar) == JS_TAG_OBJECT)
                return JS_DupValue(ctx, *pchar);
        }
    }
    b = js_malloc(ctx, sizeof(QPBytesObject) + len + 1);
    if (!b)
        return JS_EXCEPTION;
    b->kind = QP_BK_BYTES;
    b->len = len;
    b->data = b->inline_data;
    b->hash = -1;
    b->alloc = 0;
    b->exports = 0;
    if (len > 0) {
        if (data)
            memcpy(b->inline_data, data, len);
        else
            memset(b->inline_data, 0, len);
    }
    b->inline_data[len] = '\0';
    if (subtype == NULL)
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(bt), JS_CLASS_PY_BYTES);
    else
        obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_BYTES);
    if (JS_IsException(obj)) {
        js_free(ctx, b);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = b;
    if (pchar)
        *pchar = JS_DupValue(ctx, obj);
    return obj;
}

static JSValue qp_bytes_new(JSContext *ctx, const void *data, Py_ssize_t len)
{
    return qp_bytes_new_type(ctx, NULL, data, len);
}

/* bytes or bytearray (STRINGLIB_NEW of the type of self) */
static JSValue qp_bm_new(JSContext *ctx, int bytearray, const void *data, Py_ssize_t len)
{
    if (bytearray)
        return qp_bytearray_new(ctx, data, len);
    return qp_bytes_new(ctx, data, len);
}

#define QP_BM_IS_BA(self) (qp_bk_kind(self) == QP_BK_BYTEARRAY)

/* bytearray self: hold an export while a method converts arguments that
   may run Python code (__index__, __buffer__, __len__, __repr__), so that
   resizing self raises BufferError (CPython's ob_exports++ in
   _bytearray_with_buffer, bytearray_contains, split, hex, mod, ...).  No-op
   for bytes. */
static void qp_bm_export(JSValueConst self)
{
    if (QP_BM_IS_BA(self))
        QP_BYTES_OBJ(self)->exports++;
}

static void qp_bm_unexport(JSValueConst self)
{
    if (QP_BM_IS_BA(self))
        QP_BYTES_OBJ(self)->exports--;
}

/* stringlib return_self(): exact bytes -> self, else a new object of the
   base type with the same content. */
static JSValue qp_bm_return_self(JSContext *ctx, JSValueConst self)
{
    Py_ssize_t len;
    const uint8_t *d;
    if (qp_bytes_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    d = qp_bytes_data(self, &len);
    return qp_bm_new(ctx, QP_BM_IS_BA(self), d, len);
}

/* Result of a builder (kind 1) as bytes/bytearray; frees the builder. */
static JSValue qp_bm_from_builder(JSContext *ctx, int bytearray, qp_ucsbuilder *b)
{
    JSValue r = qp_bm_new(ctx, bytearray, b->data, b->len);
    qp_ucsbuilder_free(b);
    return r;
}

/* ------------------------------------------------------------------------ */
/* buffer protocol                                                           */
/* ------------------------------------------------------------------------ */

static int qp_bytes_getbuffer(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags)
{
    QPBytesObject *b = QP_BYTES_OBJ(obj);
    if (flags & QP_BUF_WRITABLE) {
        qp_raise(ctx, QP_EXC(ctx, BufferError), "Object is not writable.");
        return -1;
    }
    view->obj = JS_DupValue(ctx, obj);
    view->buf = b->data;
    view->len = b->len;
    view->readonly = 1;
    view->itemsize = 1;
    view->format = "B";
    return 0;
}

static const char *qp_bm_typed_array_format(int class_id, Py_ssize_t *itemsize)
{
    switch (class_id) {
    case JS_CLASS_INT8_ARRAY: *itemsize = 1; return "b";
    case JS_CLASS_INT16_ARRAY: *itemsize = 2; return "h";
    case JS_CLASS_UINT16_ARRAY: *itemsize = 2; return "H";
    case JS_CLASS_INT32_ARRAY: *itemsize = 4; return "i";
    case JS_CLASS_UINT32_ARRAY: *itemsize = 4; return "I";
    case JS_CLASS_BIG_INT64_ARRAY: *itemsize = 8; return "q";
    case JS_CLASS_BIG_UINT64_ARRAY: *itemsize = 8; return "Q";
    case JS_CLASS_FLOAT16_ARRAY: *itemsize = 2; return "e";
    case JS_CLASS_FLOAT32_ARRAY: *itemsize = 4; return "f";
    case JS_CLASS_FLOAT64_ARRAY: *itemsize = 8; return "d";
    default: *itemsize = 1; return "B";
    }
}

/* Raw view of a JS ArrayBuffer / typed array / DataView. */
static void qp_bm_js_buffer_info(JSObject *p, uint8_t **pbuf, Py_ssize_t *plen,
                                 int *preadonly, Py_ssize_t *pitemsize, const char **pformat)
{
    *pformat = "B";
    *pitemsize = 1;
    if (p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
        JSArrayBuffer *abuf = p->u.array_buffer;
        *pbuf = abuf->detached ? NULL : abuf->data;
        *plen = abuf->detached ? 0 : abuf->byte_length;
        *preadonly = abuf->immutable;
        return;
    }
    {
        JSTypedArray *ta = p->u.typed_array;
        JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
        *preadonly = abuf->immutable;
        if (p->class_id == JS_CLASS_DATAVIEW) {
            if (abuf->detached) {
                *pbuf = NULL;
                *plen = 0;
            } else {
                Py_ssize_t l = ta->track_rab ? (Py_ssize_t)abuf->byte_length - ta->offset : ta->length;
                if (l < 0 || ta->offset + (Py_ssize_t)l > abuf->byte_length)
                    l = 0;
                *pbuf = abuf->data + ta->offset;
                *plen = l;
            }
            return;
        }
        *pformat = qp_bm_typed_array_format(p->class_id, pitemsize);
        *pbuf = p->u.array.u.ptr;
        *plen = (Py_ssize_t)p->u.array.count * *pitemsize;
    }
}

static int qp_buffer_get(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags)
{
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        QPType *t;
        if (qp_bm_is_typed_array_class(p->class_id))
            return qp_memoryview_getbuffer(ctx, obj, view, flags);
        if (p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
            qp_bm_js_buffer_info(p, &view->buf, &view->len, &view->readonly,
                                 &view->itemsize, &view->format);
            if ((flags & QP_BUF_WRITABLE) && view->readonly) {
                qp_raise(ctx, QP_EXC(ctx, BufferError), "Object is not writable.");
                return -1;
            }
            if (!view->buf)
                view->buf = (uint8_t *)qp_bytes_empty_data;
            view->obj = JS_DupValue(ctx, obj);
            return 0;
        }
        t = qp_type_of(ctx, obj);
        if (t->slots.bf.bf_getbuffer)
            return t->slots.bf.bf_getbuffer(ctx, obj, view, flags);
    }
    qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                       "a bytes-like object is required, not '%U'", obj);
    return -1;
}

static void qp_buffer_release(JSContext *ctx, QPBuffer *view)
{
    JSValue obj = view->obj;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return;
    {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (!qp_bm_is_typed_array_class(p->class_id) && p->class_id != JS_CLASS_ARRAY_BUFFER &&
            p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) {
            QPType *t = qp_type_of(ctx, obj);
            if (t->slots.bf.bf_releasebuffer)
                t->slots.bf.bf_releasebuffer(ctx, view);
        }
    }
    view->obj = JS_UNDEFINED;
    JS_FreeValue(ctx, obj);
}

/* Release from a finalizer (no context): only the exporters of this unit
   keep export counts; a Python-level __release_buffer__ is not called
   while the GC frees objects. */
static void qp_buffer_release_rt(JSRuntime *rt, QPBuffer *view)
{
    JSValue obj = view->obj;
    int k;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return;
    k = qp_bk_kind(obj);
    if (k == QP_BK_BYTEARRAY) {
        QPBytesObject *b = QP_BYTES_OBJ(obj);
        if (b->exports > 0)
            b->exports--;
    } else if (k == QP_BK_MEMORYVIEW) {
        QPMemoryView *mv = QP_MV_OBJ(obj);
        if (mv->exports > 0)
            mv->exports--;
    }
#ifdef CONFIG_PYTHON_CAPI
    /* D5 (CapiD): C-extension exporters release through their C slot */
    else if (JS_VALUE_GET_OBJ(obj)->class_id == JS_CLASS_PY_NATIVE) {
        qp_capi_bf_release_rt(rt, view);
    }
#endif
    view->obj = JS_UNDEFINED;
    JS_FreeValueRT(rt, obj);
}

/* ------------------------------------------------------------------------ */
/* repr, hash, compare                                                       */
/* ------------------------------------------------------------------------ */

/* _Py_bytes_repr */
static JSValue qp_bytes_repr_cls(JSContext *ctx, const uint8_t *data, Py_ssize_t length,
                                 int smartquotes, const char *classname)
{
    Py_ssize_t i, newsize, squotes = 0, dquotes = 0;
    uint8_t quote;
    char *buf, *p;
    JSValue v;

    newsize = 3;
    for (i = 0; i < length; i++) {
        uint8_t c = data[i];
        Py_ssize_t incr = 1;
        switch (c) {
        case '\'': squotes++; break;
        case '"': dquotes++; break;
        case '\\': case '\t': case '\n': case '\r':
            incr = 2; break;
        default:
            if (c < ' ' || c >= 0x7f)
                incr = 4;
        }
        if (newsize > PY_SSIZE_T_MAX - incr)
            goto overflow;
        newsize += incr;
    }
    quote = '\'';
    if (smartquotes && squotes && !dquotes)
        quote = '"';
    if (squotes && quote == '\'') {
        if (newsize > PY_SSIZE_T_MAX - squotes)
            goto overflow;
        newsize += squotes;
    }
    if (newsize > JS_STRING_LEN_MAX)
        goto overflow;
    buf = js_malloc(ctx, newsize);
    if (!buf)
        return JS_EXCEPTION;
    p = buf;
    *p++ = 'b';
    *p++ = quote;
    for (i = 0; i < length; i++) {
        uint8_t c = data[i];
        if (c == quote || c == '\\') {
            *p++ = '\\';
            *p++ = c;
        } else if (c == '\t') {
            *p++ = '\\';
            *p++ = 't';
        } else if (c == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        } else if (c == '\r') {
            *p++ = '\\';
            *p++ = 'r';
        } else if (c < ' ' || c >= 0x7f) {
            *p++ = '\\';
            *p++ = 'x';
            *p++ = qp_hexdigits[(c & 0xf0) >> 4];
            *p++ = qp_hexdigits[c & 0xf];
        } else {
            *p++ = c;
        }
    }
    *p++ = quote;
    v = JS_NewStringLen(ctx, buf, p - buf);
    js_free(ctx, buf);
    return v;
overflow:
    return qp_raise_overflow_error(ctx, "%s object is too large to make repr", classname);
}

static JSValue qp_bytes_repr(JSContext *ctx, const uint8_t *data, Py_ssize_t len, int smartquotes)
{
    return qp_bytes_repr_cls(ctx, data, len, smartquotes, "bytes");
}

static JSValue qp_bytes_tp_repr(JSContext *ctx, JSValueConst self)
{
    Py_ssize_t len;
    const uint8_t *d = qp_bytes_data(self, &len);
    return qp_bytes_repr(ctx, d, len, 1);
}

static Py_hash_t qp_bytes_tp_hash(JSContext *ctx, JSValueConst self)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (b->hash == -1)
        b->hash = qp_hash_buffer(b->data, b->len, QP_STATE(ctx)->hash_key);
    return b->hash;
}

static JSValue qp_bm_cmp_result(Py_ssize_t c, int op)
{
    int r;
    switch (op) {
    case QP_CMP_LT: r = c < 0; break;
    case QP_CMP_LE: r = c <= 0; break;
    case QP_CMP_EQ: r = c == 0; break;
    case QP_CMP_NE: r = c != 0; break;
    case QP_CMP_GT: r = c > 0; break;
    default: r = c >= 0; break;
    }
    return JS_NewBool(NULL, r);
}

static JSValue qp_bytes_tp_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    QPBytesObject *x, *y;
    Py_ssize_t min_len;
    int c;

    if (!qp_bytes_check(ctx, a) || !qp_bytes_check(ctx, b))
        return qp_not_implemented(ctx);
    x = QP_BYTES_OBJ(a);
    y = QP_BYTES_OBJ(b);
    if (x == y)
        return JS_NewBool(ctx, op == QP_CMP_EQ || op == QP_CMP_LE || op == QP_CMP_GE);
    if (op == QP_CMP_EQ || op == QP_CMP_NE) {
        int eq = x->len == y->len && (x->len == 0 || memcmp(x->data, y->data, x->len) == 0);
        return JS_NewBool(ctx, eq ^ (op == QP_CMP_NE));
    }
    min_len = x->len < y->len ? x->len : y->len;
    c = min_len > 0 ? memcmp(x->data, y->data, min_len) : 0;
    if (c != 0)
        return qp_bm_cmp_result(c, op);
    return qp_bm_cmp_result(x->len < y->len ? -1 : x->len > y->len, op);
}

/* ------------------------------------------------------------------------ */
/* sequence / mapping slots of bytes                                         */
/* ------------------------------------------------------------------------ */

static Py_ssize_t qp_bytes_sq_length(JSContext *ctx, JSValueConst self)
{
    return QP_BYTES_OBJ(self)->len;
}

static JSValue qp_bytes_sq_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (i < 0 || i >= b->len)
        return qp_raise_index_error(ctx, "index out of range");
    return JS_NewInt32(ctx, b->data[i]);
}

static int qp_bytes_tp_iteritem(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem)
{
    Py_ssize_t len;
    const uint8_t *d = qp_bytes_data(seq, &len);
    if (index < 0 || index >= len)
        return 0;
    *pitem = JS_NewInt32(ctx, d[index]);
    return 1;
}

static JSValue qp_bytes_tp_iter(JSContext *ctx, JSValueConst self)
{
    return qp_bytes_iter_new(ctx, self);
}

/* bytes_concat / PyByteArray_Concat (result type given by `bytearray`) */
static JSValue qp_bm_concat(JSContext *ctx, JSValueConst a, JSValueConst b, int bytearray)
{
    QPBuffer va, vb;
    JSValue r;
    uint8_t *d;

    va.obj = vb.obj = JS_UNDEFINED;
    if (qp_buffer_get(ctx, a, &va, QP_BUF_SIMPLE) < 0 ||
        qp_buffer_get(ctx, b, &vb, QP_BUF_SIMPLE) < 0) {
        JSValue na, nb;
        qp_bm_exc_clear(ctx);
        qp_buffer_release(ctx, &va);
        na = qp_bm_tpname(ctx, a);
        nb = qp_bm_tpname(ctx, b);
        if (!JS_IsException(na) && !JS_IsException(nb))
            qp_raise_type_error(ctx, "can't concat %U to %U", nb, na);
        JS_FreeValue(ctx, na);
        JS_FreeValue(ctx, nb);
        return JS_EXCEPTION;
    }
    if (!bytearray && va.len == 0 && qp_bytes_check_exact(ctx, b)) {
        r = JS_DupValue(ctx, b);
        goto done;
    }
    if (!bytearray && vb.len == 0 && qp_bytes_check_exact(ctx, a)) {
        r = JS_DupValue(ctx, a);
        goto done;
    }
    if (va.len > PY_SSIZE_T_MAX - vb.len) {
        r = qp_raise_memory_error(ctx);
        goto done;
    }
    r = qp_bm_new(ctx, bytearray, NULL, va.len + vb.len);
    if (!JS_IsException(r)) {
        d = QP_BYTES_OBJ(r)->data;
        if (va.len)
            memcpy(d, va.buf, va.len);
        if (vb.len)
            memcpy(d + va.len, vb.buf, vb.len);
    }
done:
    qp_buffer_release(ctx, &va);
    qp_buffer_release(ctx, &vb);
    return r;
}

static JSValue qp_bytes_sq_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return qp_bm_concat(ctx, a, b, 0);
}

/* _PyBytes_Repeat: fill dest (len_dest) with copies of src (len_src). */
static void qp_bm_repeat_fill(uint8_t *dest, Py_ssize_t len_dest, const uint8_t *src, Py_ssize_t len_src)
{
    Py_ssize_t copied;
    if (len_dest == 0)
        return;
    if (len_src == 1) {
        memset(dest, src[0], len_dest);
        return;
    }
    if (src != dest)
        memcpy(dest, src, len_src);
    copied = len_src;
    while (copied < len_dest) {
        Py_ssize_t bytes_to_copy = copied < len_dest - copied ? copied : len_dest - copied;
        memcpy(dest + copied, dest, bytes_to_copy);
        copied += bytes_to_copy;
    }
}

static JSValue qp_bytes_sq_repeat(JSContext *ctx, JSValueConst self, Py_ssize_t n)
{
    QPBytesObject *a = QP_BYTES_OBJ(self);
    Py_ssize_t size;
    JSValue r;
    if (n < 0)
        n = 0;
    if (n > 0 && a->len > PY_SSIZE_T_MAX / n)
        return qp_raise_overflow_error(ctx, "repeated bytes are too long");
    size = a->len * n;
    if (size == a->len && qp_bytes_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    r = qp_bytes_new(ctx, NULL, size);
    if (JS_IsException(r) || size == 0)
        return r;
    qp_bm_repeat_fill(QP_BYTES_OBJ(r)->data, size, a->data, a->len);
    return r;
}

/* _Py_bytes_contains */
static int qp_bm_contains_held(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    Py_ssize_t ival, len;
    const uint8_t *d;
    if (qp_as_ssize(ctx, arg, NULL, &ival) < 0) {
        QPBuffer varg;
        Py_ssize_t pos;
        qp_bm_exc_clear(ctx);
        if (qp_buffer_get(ctx, arg, &varg, QP_BUF_SIMPLE) < 0)
            return -1;
        d = qp_bytes_data(self, &len);
        pos = qp_sl_find(ctx->rt, qp_strview_make(1, d, len),
                         qp_strview_make(1, varg.buf, varg.len), 0, len, 1);
        qp_buffer_release(ctx, &varg);
        if (pos == QP_SL_NOMEM) {
            qp_raise_memory_error(ctx);
            return -1;
        }
        return pos >= 0;
    }
    if (ival < 0 || ival >= 256) {
        qp_raise_value_error(ctx, "byte must be in range(0, 256)");
        return -1;
    }
    d = qp_bytes_data(self, &len);
    return len > 0 && memchr(d, (int)ival, len) != NULL;
}

static int qp_bm_contains(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    int r;
    qp_bm_export(self);
    r = qp_bm_contains_held(ctx, self, arg);
    qp_bm_unexport(self);
    return r;
}

/* Slice of data with (start, step, slicelength) as a new object. */
static JSValue qp_bm_slice(JSContext *ctx, int bytearray, const uint8_t *src,
                           Py_ssize_t start, Py_ssize_t step, Py_ssize_t slicelength)
{
    JSValue r;
    uint8_t *d;
    Py_ssize_t i;
    size_t cur;
    if (slicelength <= 0)
        return qp_bm_new(ctx, bytearray, NULL, 0);
    if (step == 1)
        return qp_bm_new(ctx, bytearray, src + start, slicelength);
    r = qp_bm_new(ctx, bytearray, NULL, slicelength);
    if (JS_IsException(r))
        return r;
    d = QP_BYTES_OBJ(r)->data;
    for (cur = start, i = 0; i < slicelength; cur += step, i++)
        d[i] = src[cur];
    return r;
}

static JSValue qp_bytes_mp_subscript(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (qp_index_check(ctx, item)) {
        Py_ssize_t i;
        if (qp_as_ssize(ctx, item, QP_EXC(ctx, IndexError), &i) < 0)
            return JS_EXCEPTION;
        if (i < 0)
            i += b->len;
        if (i < 0 || i >= b->len)
            return qp_raise_index_error(ctx, "index out of range");
        return JS_NewInt32(ctx, b->data[i]);
    }
    if (qp_slice_check(item)) {
        Py_ssize_t start, stop, step, slicelength;
        if (qp_slice_unpack(ctx, item, &start, &stop, &step) < 0)
            return JS_EXCEPTION;
        slicelength = qp_slice_adjust_indices(b->len, &start, &stop, step);
        if (slicelength > 0 && start == 0 && step == 1 && slicelength == b->len &&
            qp_bytes_check_exact(ctx, self))
            return JS_DupValue(ctx, self);
        return qp_bm_slice(ctx, 0, b->data, start, step, slicelength);
    }
    return qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                              "byte indices must be integers or slices, not %U", item);
}

/* ------------------------------------------------------------------------ */
/* constructor                                                               */
/* ------------------------------------------------------------------------ */

/* str argument of a clinic `str` parameter as UTF-8 (JS_FreeCString). */
static const char *qp_bm_arg_cstr(JSContext *ctx, const char *fname, const char *argname,
                                  JSValueConst v)
{
    JSValue s;
    const char *r;
    size_t len;
    char desc[64];
    if (!qp_str_check(ctx, v)) {
        snprintf(desc, sizeof(desc), "argument '%s'", argname);
        qp_bm_bad_argument(ctx, fname, desc, "str", v);
        return NULL;
    }
    s = qp_str_value(ctx, v);
    if (JS_IsException(s))
        return NULL;
    r = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (!r)
        return NULL;
    if (strlen(r) != len) {
        JS_FreeCString(ctx, r);
        qp_raise_value_error(ctx, "embedded null character");
        return NULL;
    }
    return r;
}

/* Bytes from an iterable of ints (CPython _PyBytes_FromIterator /
   _PyBytes_FromList / _PyBytes_FromTuple: "bytes must be in range(0, 256)").
   kind 1: exact list, 2: exact tuple, 0: `it` = iter(x) (consumed). */
static JSValue qp_bytes_from_ints(JSContext *ctx, JSValueConst x, int kind, JSValue it)
{
    DynBuf db;
    JSValue item, r;
    Py_ssize_t value, i;

    js_dbuf_init(ctx, &db);
    if (kind == 1) {
        /* exact list: items re-read each step (__index__ may mutate it) */
        for (i = 0; i < qp_list_size(x); i++) {
            item = qp_getitem(ctx, x, JS_NewInt32(ctx, (int32_t)i));
            if (JS_IsException(item))
                goto fail;
            if (qp_as_ssize(ctx, item, NULL, &value) < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            JS_FreeValue(ctx, item);
            if (value < 0 || value >= 256) {
                qp_raise_value_error(ctx, "bytes must be in range(0, 256)");
                goto fail;
            }
            if (dbuf_putc(&db, (uint8_t)value))
                goto oom;
        }
    } else if (kind == 2) {
        Py_ssize_t n = QP_TUPLE_SIZE(x);
        for (i = 0; i < n; i++) {
            if (qp_as_ssize(ctx, QP_TUPLE_ITEMS(x)[i], NULL, &value) < 0)
                goto fail;
            if (value < 0 || value >= 256) {
                qp_raise_value_error(ctx, "bytes must be in range(0, 256)");
                goto fail;
            }
            if (dbuf_putc(&db, (uint8_t)value))
                goto oom;
        }
    } else {
        Py_ssize_t hint = qp_length_hint(ctx, x, 64);
        if (hint < 0)
            goto fail;
        if (hint > 0 && hint < (1 << 20) && dbuf_claim(&db, hint))
            goto oom;
        for (;;) {
            item = qp_next(ctx, it);
            if (QP_IS_NULL(item))
                break;
            if (JS_IsException(item))
                goto fail;
            if (qp_as_ssize(ctx, item, NULL, &value) < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            JS_FreeValue(ctx, item);
            if (value < 0 || value >= 256) {
                qp_raise_value_error(ctx, "bytes must be in range(0, 256)");
                goto fail;
            }
            if (dbuf_putc(&db, (uint8_t)value))
                goto oom;
        }
        JS_FreeValue(ctx, it);
        it = JS_UNDEFINED;
    }
    r = qp_bytes_new(ctx, db.buf, db.size);
    dbuf_free(&db);
    return r;
oom:
    qp_raise_memory_error(ctx);
fail:
    JS_FreeValue(ctx, it);
    dbuf_free(&db);
    return JS_EXCEPTION;
}

/* _PyBytes_FromBuffer (any exporter, non-contiguous memoryviews too) */
static JSValue qp_bytes_from_buffer(JSContext *ctx, JSValueConst x, int bytearray)
{
    QPBuffer view;
    JSValue r;
    if (qp_memoryview_check(x) ||
        (JS_VALUE_GET_TAG(x) == JS_TAG_OBJECT &&
         qp_bm_is_typed_array_class(JS_VALUE_GET_OBJ(x)->class_id)))
        return qp_memoryview_to_bytes(ctx, x, bytearray);
    if (qp_buffer_get(ctx, x, &view, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    r = qp_bm_new(ctx, bytearray, view.buf, view.len);
    qp_buffer_release(ctx, &view);
    return r;
}

/* PyBytes_FromObject */
static JSValue qp_bytes_from_object_plain(JSContext *ctx, JSValueConst x)
{
    JSValue it;
    if (qp_bytes_check_exact(ctx, x))
        return JS_DupValue(ctx, x);
    if (qp_buffer_check(ctx, x))
        return qp_bytes_from_buffer(ctx, x, 0);
    if (qp_list_check_exact(ctx, x))
        return qp_bytes_from_ints(ctx, x, 1, JS_UNDEFINED);
    if (qp_tuple_check_exact(ctx, x))
        return qp_bytes_from_ints(ctx, x, 2, JS_UNDEFINED);
    if (!qp_str_check(ctx, x)) {
        it = qp_iter(ctx, x);
        if (!JS_IsException(it))
            return qp_bytes_from_ints(ctx, x, 0, it);
        if (!qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
            return JS_EXCEPTION;
    }
    return qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                              "cannot convert '%U' object to bytes", x);
}

/* bytes_new_impl for an exact bytes result (type handled by the caller).
   x may be QP_NULL; encoding/errors NULL when absent. */
static JSValue qp_bytes_new_impl(JSContext *ctx, JSValueConst x, const char *encoding,
                                 const char *errors)
{
    JSValue func, bytes;
    Py_ssize_t size;

    if (QP_IS_NULL(x)) {
        if (encoding != NULL || errors != NULL)
            return qp_raise_type_error(ctx, encoding != NULL ?
                                       "encoding without a string argument" :
                                       "errors without a string argument");
        return qp_bytes_new(ctx, NULL, 0);
    }
    if (encoding != NULL) {
        JSValue s;
        if (!qp_str_check(ctx, x))
            return qp_raise_type_error(ctx, "encoding without a string argument");
        s = qp_str_value(ctx, x);
        if (JS_IsException(s))
            return s;
        bytes = qp_str_encode(ctx, s, encoding, errors);
        JS_FreeValue(ctx, s);
        return bytes;
    }
    if (errors != NULL)
        return qp_raise_type_error(ctx, qp_str_check(ctx, x) ?
                                   "string argument without an encoding" :
                                   "errors without a string argument");
    func = qp_lookup_special(ctx, x, QP_ATOM(ctx, __bytes__));
    if (JS_IsException(func))
        return func;
    if (!QP_IS_NULL(func)) {
        bytes = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(bytes))
            return bytes;
        if (!qp_bytes_check(ctx, bytes)) {
            qp_raise_type_error(ctx, "%T.__bytes__() must return a bytes, not %T", x, bytes);
            JS_FreeValue(ctx, bytes);
            return JS_EXCEPTION;
        }
        return bytes;
    }
    if (qp_str_check(ctx, x))
        return qp_raise_type_error(ctx, "string argument without an encoding");
    if (qp_index_check(ctx, x)) {
        if (qp_as_ssize(ctx, x, QP_EXC(ctx, OverflowError), &size) < 0) {
            if (!qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
                return JS_EXCEPTION;
            return qp_bytes_from_object_plain(ctx, x);
        }
        if (size < 0)
            return qp_raise_value_error(ctx, "negative count");
        return qp_bytes_new(ctx, NULL, size);
    }
    return qp_bytes_from_object_plain(ctx, x);
}

static JSValue qp_bytes_from_object(JSContext *ctx, JSValueConst v)
{
    return qp_bytes_new_impl(ctx, v, NULL, NULL);
}

static JSValue qp_bytes_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "source", "encoding", "errors", NULL };
    static const QPArgSpec spec = { "bytes", kw, 0, 3, 0 };
    JSValueConst a[3];
    const char *encoding = NULL, *errors = NULL;
    JSValue r;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1])) {
        encoding = qp_bm_arg_cstr(ctx, "bytes", "encoding", a[1]);
        if (!encoding)
            return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(a[2])) {
        errors = qp_bm_arg_cstr(ctx, "bytes", "errors", a[2]);
        if (!errors) {
            if (encoding)
                JS_FreeCString(ctx, encoding);
            return JS_EXCEPTION;
        }
    }
    r = qp_bytes_new_impl(ctx, a[0], encoding, errors);
    if (encoding)
        JS_FreeCString(ctx, encoding);
    if (errors)
        JS_FreeCString(ctx, errors);
    if (!JS_IsException(r) && subtype != QP_TYPE(ctx, bytes)) {
        /* bytes_subtype_new */
        Py_ssize_t len;
        const uint8_t *d = qp_bytes_data(r, &len);
        JSValue s = qp_bytes_new_type(ctx, subtype, d, len);
        JS_FreeValue(ctx, r);
        r = s;
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* hex / fromhex                                                             */
/* ------------------------------------------------------------------------ */

/* Validate a hex() separator (pystrhex.c); *psep = -1 when absent. */
static int qp_bm_hex_sep(JSContext *ctx, JSValueConst sep, int *psep)
{
    Py_ssize_t seplen;
    int ch;
    *psep = -1;
    if (QP_IS_NULL(sep))
        return 0;
    seplen = qp_len(ctx, sep);
    if (seplen < 0)
        return -1;
    if (seplen != 1) {
        qp_raise_value_error(ctx, "sep must be length 1.");
        return -1;
    }
    if (qp_str_check(ctx, sep)) {
        JSValue s = qp_str_value(ctx, sep);
        qp_strview v;
        if (JS_IsException(s))
            return -1;
        if (qp_str_view_acquire(ctx, s, &v) < 0) {
            JS_FreeValue(ctx, s);
            return -1;
        }
        ch = (int)QP_SL_READ(v.kind, v.data, 0);
        qp_str_view_release(ctx, &v);
        JS_FreeValue(ctx, s);
    } else if (qp_bytes_check(ctx, sep)) {
        ch = QP_BYTES_OBJ(sep)->data[0];
    } else {
        qp_raise_type_error(ctx, "sep must be str or bytes.");
        return -1;
    }
    if (ch > 127) {
        qp_raise_value_error(ctx, "sep must be ASCII.");
        return -1;
    }
    *psep = ch;
    return 0;
}

/* _Py_strhex_impl (str result); sep_char < 0: no separator. */
static JSValue qp_bm_strhex_build(JSContext *ctx, const uint8_t *argbuf, Py_ssize_t arglen,
                                  int sep_char, Py_ssize_t bytes_per_sep_group)
{
    size_t abs_bytes_per_sep;
    Py_ssize_t resultlen = 0, i, j;
    char *retbuf;
    JSValue r;

    if (sep_char < 0)
        bytes_per_sep_group = 0;
    abs_bytes_per_sep = bytes_per_sep_group < 0 ? -(size_t)bytes_per_sep_group :
        (size_t)bytes_per_sep_group;
    if (bytes_per_sep_group && arglen > 0)
        resultlen = (arglen - 1) / abs_bytes_per_sep;
    if (arglen >= PY_SSIZE_T_MAX / 2 - resultlen)
        return qp_raise_memory_error(ctx);
    resultlen += arglen * 2;
    if (resultlen > JS_STRING_LEN_MAX)
        return qp_raise_memory_error(ctx);
    if (abs_bytes_per_sep >= (size_t)arglen) {
        bytes_per_sep_group = 0;
        abs_bytes_per_sep = 0;
    }
    retbuf = js_malloc(ctx, resultlen + 1);
    if (!retbuf)
        return JS_EXCEPTION;
    if (bytes_per_sep_group == 0) {
        for (i = 0, j = 0; i < arglen; i++) {
            uint8_t c = argbuf[i];
            retbuf[j++] = qp_hexdigits[c >> 4];
            retbuf[j++] = qp_hexdigits[c & 0x0f];
        }
    } else {
        Py_ssize_t chunks = (arglen - 1) / abs_bytes_per_sep, chunk;
        size_t k;
        if (bytes_per_sep_group < 0) {
            i = j = 0;
            for (chunk = 0; chunk < chunks; chunk++) {
                for (k = 0; k < abs_bytes_per_sep; k++) {
                    uint8_t c = argbuf[i++];
                    retbuf[j++] = qp_hexdigits[c >> 4];
                    retbuf[j++] = qp_hexdigits[c & 0x0f];
                }
                retbuf[j++] = (char)sep_char;
            }
            while (i < arglen) {
                uint8_t c = argbuf[i++];
                retbuf[j++] = qp_hexdigits[c >> 4];
                retbuf[j++] = qp_hexdigits[c & 0x0f];
            }
        } else {
            i = arglen - 1;
            j = resultlen - 1;
            for (chunk = 0; chunk < chunks; chunk++) {
                for (k = 0; k < abs_bytes_per_sep; k++) {
                    uint8_t c = argbuf[i--];
                    retbuf[j--] = qp_hexdigits[c & 0x0f];
                    retbuf[j--] = qp_hexdigits[c >> 4];
                }
                retbuf[j--] = (char)sep_char;
            }
            while (i >= 0) {
                uint8_t c = argbuf[i--];
                retbuf[j--] = qp_hexdigits[c & 0x0f];
                retbuf[j--] = qp_hexdigits[c >> 4];
            }
        }
    }
    r = JS_NewStringLen(ctx, retbuf, resultlen);
    js_free(ctx, retbuf);
    return r;
}

/* hex(sep=<unrepresentable>, bytes_per_sep=1) argument parsing */
static int qp_bm_parse_hex_args(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                                JSValueConst kwnames, JSValueConst *psep, Py_ssize_t *pbps)
{
    static const char *const kw[] = { "sep", "bytes_per_sep", NULL };
    QPArgSpec spec = { fname, kw, 0, 2, 0 };
    JSValueConst a[2];
    Py_ssize_t bps = 1;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return -1;
    if (!QP_IS_NULL(a[1]) && qp_arg_ssize(ctx, a[1], &bps) < 0)
        return -1;
    *psep = a[0];
    *pbps = bps;
    return 0;
}

/* Hex of a buffer with a validated sep object (QP_NULL = none). */
static JSValue qp_bytes_strhex(JSContext *ctx, const uint8_t *data, Py_ssize_t len,
                               JSValueConst sep, Py_ssize_t bytes_per_sep)
{
    int sep_char;
    if (qp_bm_hex_sep(ctx, sep, &sep_char) < 0)
        return JS_EXCEPTION;
    return qp_bm_strhex_build(ctx, data, len, sep_char, bytes_per_sep);
}

static JSValue qp_bm_hex(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                         JSValueConst kwnames, int magic)
{
    JSValueConst sep;
    Py_ssize_t bps, len;
    int sep_char, r;
    const uint8_t *d;
    if (qp_bm_parse_hex_args(ctx, "hex", args, nargs, kwnames, &sep, &bps) < 0)
        return JS_EXCEPTION;
    qp_bm_export(self);
    r = qp_bm_hex_sep(ctx, sep, &sep_char);
    qp_bm_unexport(self);
    if (r < 0)
        return JS_EXCEPTION;
    d = qp_bytes_data(self, &len);
    return qp_bm_strhex_build(ctx, d, len, sep_char, bps);
}

static int qp_bm_hexval(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 37;
}

/* _PyBytes_FromHex */
static JSValue qp_bytes_from_hex(JSContext *ctx, JSValueConst string, int use_bytearray)
{
    Py_ssize_t hexlen, invalid_char = -1, i;
    const uint8_t *str, *start, *end;
    uint8_t *tmp = NULL;
    QPBuffer view;
    DynBuf db;
    JSValue r = JS_EXCEPTION;

    view.obj = JS_UNDEFINED;
    if (qp_str_check(ctx, string)) {
        JSValue s = qp_str_value(ctx, string);
        qp_strview sv;
        if (JS_IsException(s))
            return s;
        if (qp_str_view_acquire(ctx, s, &sv) < 0) {
            JS_FreeValue(ctx, s);
            return JS_EXCEPTION;
        }
        hexlen = sv.len;
        for (i = 0; i < hexlen; i++) {
            if (QP_SL_READ(sv.kind, sv.data, i) >= 128)
                break;
        }
        if (i < hexlen) {
            qp_str_view_release(ctx, &sv);
            JS_FreeValue(ctx, s);
            invalid_char = i;
            js_dbuf_init(ctx, &db);
            goto error;
        }
        tmp = js_malloc(ctx, hexlen + 1);
        if (!tmp) {
            qp_str_view_release(ctx, &sv);
            JS_FreeValue(ctx, s);
            return JS_EXCEPTION;
        }
        for (i = 0; i < hexlen; i++)
            tmp[i] = (uint8_t)QP_SL_READ(sv.kind, sv.data, i);
        qp_str_view_release(ctx, &sv);
        JS_FreeValue(ctx, s);
        str = tmp;
    } else if (qp_buffer_check(ctx, string)) {
        if (qp_buffer_get(ctx, string, &view, QP_BUF_SIMPLE) < 0)
            return JS_EXCEPTION;
        hexlen = view.len;
        str = view.buf;
    } else {
        return qp_raise_type_error(ctx, "fromhex() argument must be str or bytes-like, not %T",
                                   string);
    }

    js_dbuf_init(ctx, &db);
    start = str;
    end = str + hexlen;
    while (str < end) {
        int top, bot;
        if (QPB_ISSPACE(*str)) {
            do {
                str++;
                if (str >= end)
                    goto done;
            } while (QPB_ISSPACE(*str));
        }
        top = qp_bm_hexval(*str);
        if (top >= 16) {
            invalid_char = str - start;
            goto error;
        }
        str++;
        if (str >= end) {
            invalid_char = -1;
            goto error;
        }
        bot = qp_bm_hexval(*str);
        if (bot >= 16) {
            invalid_char = str - start;
            goto error;
        }
        str++;
        if (dbuf_putc(&db, (uint8_t)((top << 4) + bot))) {
            qp_raise_memory_error(ctx);
            goto fail;
        }
    }
done:
    r = qp_bm_new(ctx, use_bytearray, db.buf, db.size);
    goto fail;
error:
    if (invalid_char == -1)
        qp_raise_value_error(ctx, "fromhex() arg must contain an even number of hexadecimal digits");
    else
        qp_raise_value_error(ctx, "non-hexadecimal number found in fromhex() arg at position %zd",
                             invalid_char);
fail:
    dbuf_free(&db);
    if (tmp)
        js_free(ctx, tmp);
    qp_buffer_release(ctx, &view);
    return r;
}

/* bytes.fromhex / bytearray.fromhex (classmethods; magic 1 = bytearray) */
static JSValue qp_bm_fromhex(JSContext *ctx, JSValueConst type, JSValueConst *args, int nargs,
                             JSValueConst kwnames, int magic)
{
    JSValue r = qp_bytes_from_hex(ctx, args[0], magic);
    QPType *t = qp_type_from_value(type);
    QPType *base = magic ? QP_TYPE(ctx, bytearray) : QP_TYPE(ctx, bytes);
    if (!JS_IsException(r) && t != base) {
        JSValue s = qp_call1(ctx, type, r);
        JS_FreeValue(ctx, r);
        r = s;
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* shared methods                                                            */
/* ------------------------------------------------------------------------ */

/* magic: 0 lower, 1 upper, 2 swapcase, 3 title, 4 capitalize */
static JSValue qp_bm_case(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                          JSValueConst kwnames, int magic)
{
    Py_ssize_t len, i;
    const uint8_t *s = qp_bytes_data(self, &len);
    JSValue r = qp_bm_new(ctx, QP_BM_IS_BA(self), NULL, len);
    uint8_t *d;
    int prev = 0;
    if (JS_IsException(r) || len == 0)
        return r;
    d = QP_BYTES_OBJ(r)->data;
    s = qp_bytes_data(self, &len);
    for (i = 0; i < len; i++) {
        int c = s[i];
        switch (magic) {
        case 0: c = QPB_TOLOWER(c); break;
        case 1: c = QPB_TOUPPER(c); break;
        case 2:
            if (QPB_ISLOWER(c))
                c -= 32;
            else if (QPB_ISUPPER(c))
                c += 32;
            break;
        case 3:
            if (QPB_ISLOWER(c)) {
                if (!prev)
                    c -= 32;
                prev = 1;
            } else if (QPB_ISUPPER(c)) {
                if (prev)
                    c += 32;
                prev = 1;
            } else {
                prev = 0;
            }
            break;
        default:
            c = i == 0 ? QPB_TOUPPER(c) : QPB_TOLOWER(c);
            break;
        }
        d[i] = (uint8_t)c;
    }
    return r;
}

/* magic: 0 isalnum, 1 isalpha, 2 isascii, 3 isdigit, 4 islower, 5 isspace,
   6 istitle, 7 isupper */
static JSValue qp_bm_isx(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                         JSValueConst kwnames, int magic)
{
    Py_ssize_t len, i;
    const uint8_t *p = qp_bytes_data(self, &len);
    int cased = 0, prev = 0;

    if (magic == 2) {
        for (i = 0; i < len; i++)
            if (p[i] >= 128)
                return JS_FALSE;
        return JS_TRUE;
    }
    if (len == 0)
        return JS_FALSE;
    switch (magic) {
    case 0:
        for (i = 0; i < len; i++)
            if (!QPB_ISALNUM(p[i]))
                return JS_FALSE;
        return JS_TRUE;
    case 1:
        for (i = 0; i < len; i++)
            if (!QPB_ISALPHA(p[i]))
                return JS_FALSE;
        return JS_TRUE;
    case 3:
        for (i = 0; i < len; i++)
            if (!QPB_ISDIGIT(p[i]))
                return JS_FALSE;
        return JS_TRUE;
    case 5:
        for (i = 0; i < len; i++)
            if (!QPB_ISSPACE(p[i]))
                return JS_FALSE;
        return JS_TRUE;
    case 4:
        for (i = 0; i < len; i++) {
            if (QPB_ISUPPER(p[i]))
                return JS_FALSE;
            if (QPB_ISLOWER(p[i]))
                cased = 1;
        }
        return JS_NewBool(ctx, cased);
    case 7:
        for (i = 0; i < len; i++) {
            if (QPB_ISLOWER(p[i]))
                return JS_FALSE;
            if (QPB_ISUPPER(p[i]))
                cased = 1;
        }
        return JS_NewBool(ctx, cased);
    default: /* istitle */
        for (i = 0; i < len; i++) {
            uint8_t ch = p[i];
            if (QPB_ISUPPER(ch)) {
                if (prev)
                    return JS_FALSE;
                prev = 1;
                cased = 1;
            } else if (QPB_ISLOWER(ch)) {
                if (!prev)
                    return JS_FALSE;
                prev = 1;
                cased = 1;
            } else {
                prev = 0;
            }
        }
        return JS_NewBool(ctx, cased);
    }
}

/* center/ljust/rjust(width, fillchar=b' ', /): magic 0/1/2 */
static JSValue qp_bm_pad(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                         JSValueConst kwnames, int magic)
{
    static const char *const names[] = { "center", "ljust", "rjust" };
    Py_ssize_t width, len;
    int fill = ' ', st;
    const uint8_t *d;
    qp_ucsbuilder b;

    if (qp_arg_check_positional(ctx, names[magic], nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_arg_ssize(ctx, args[0], &width) < 0)
        return JS_EXCEPTION;
    if (nargs > 1) {
        int k = qp_bk_kind(args[1]);
        if ((k == QP_BK_BYTES || k == QP_BK_BYTEARRAY) && QP_BYTES_OBJ(args[1])->len == 1)
            fill = QP_BYTES_OBJ(args[1])->data[0];
        else
            return qp_bm_bad_argument(ctx, names[magic], "argument 2",
                                      "a byte string of length 1", args[1]);
    }
    d = qp_bytes_data(self, &len);
    qp_ucsbuilder_init(&b, ctx->rt);
    if (magic == 0)
        st = qp_sl_center(qp_strview_make(1, d, len), width, fill, &b);
    else if (magic == 1)
        st = qp_sl_ljust(qp_strview_make(1, d, len), width, fill, &b);
    else
        st = qp_sl_rjust(qp_strview_make(1, d, len), width, fill, &b);
    if (st == QP_SL_UNCHANGED)
        return qp_bm_return_self(ctx, self);
    if (st != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        return qp_raise_memory_error(ctx);
    }
    return qp_bm_from_builder(ctx, QP_BM_IS_BA(self), &b);
}

static JSValue qp_bm_zfill(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    Py_ssize_t width, len;
    const uint8_t *d;
    qp_ucsbuilder b;
    int st;
    if (qp_arg_ssize(ctx, args[0], &width) < 0)
        return JS_EXCEPTION;
    d = qp_bytes_data(self, &len);
    qp_ucsbuilder_init(&b, ctx->rt);
    st = qp_sl_zfill(qp_strview_make(1, d, len), width, &b);
    if (st == QP_SL_UNCHANGED)
        return qp_bm_return_self(ctx, self);
    if (st != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        return qp_raise_memory_error(ctx);
    }
    return qp_bm_from_builder(ctx, QP_BM_IS_BA(self), &b);
}

static JSValue qp_bm_expandtabs(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "tabsize", NULL };
    static const QPArgSpec spec = { "expandtabs", kw, 0, 1, 0 };
    JSValueConst a[1];
    int tabsize = 8, st;
    Py_ssize_t len;
    const uint8_t *d;
    qp_ucsbuilder b;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_int(ctx, a[0], &tabsize) < 0)
        return JS_EXCEPTION;
    d = qp_bytes_data(self, &len);
    qp_ucsbuilder_init(&b, ctx->rt);
    st = qp_sl_expandtabs(qp_strview_make(1, d, len), tabsize, &b);
    if (st == QP_SL_UNCHANGED)
        return qp_bm_new(ctx, QP_BM_IS_BA(self), d, len);
    if (st != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        return qp_bm_sl_error(ctx, st, "result too long");
    }
    return qp_bm_from_builder(ctx, QP_BM_IS_BA(self), &b);
}

/* Shared parse of (sub[, start[, end]]) with the int-or-buffer first
   argument (parse_args_finds_byte).  On success the needle is in
   (*psub, *psublen), backed by *pbuf (release it) or *pbyte, and self holds
   an export (qp_bm_unexport) taken after start and end were converted. */
static int qp_bm_parse_finds(JSContext *ctx, JSValueConst self, const char *fname,
                             JSValueConst *args, int nargs,
                             Py_ssize_t *pstart, Py_ssize_t *pend, QPBuffer *pbuf,
                             uint8_t *pbyte, const uint8_t **psub, Py_ssize_t *psublen)
{
    JSValueConst subobj;
    pbuf->obj = JS_UNDEFINED;
    *pstart = 0;
    *pend = PY_SSIZE_T_MAX;
    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return -1;
    subobj = args[0];
    if (nargs > 1 && qp_bm_slice_index(ctx, args[1], pstart) < 0)
        return -1;
    if (nargs > 2 && qp_bm_slice_index(ctx, args[2], pend) < 0)
        return -1;
    qp_bm_export(self);
    if (qp_buffer_check(ctx, subobj)) {
        if (qp_buffer_get(ctx, subobj, pbuf, QP_BUF_SIMPLE) < 0)
            goto fail;
        *psub = pbuf->buf;
        *psublen = pbuf->len;
        return 0;
    }
    if (!qp_index_check(ctx, subobj)) {
        qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                           "argument should be integer or bytes-like object, not '%U'", subobj);
        goto fail;
    }
    {
        Py_ssize_t ival;
        if (qp_as_ssize(ctx, subobj, NULL, &ival) < 0)
            goto fail;
        if (ival < 0 || ival > 255) {
            qp_raise_value_error(ctx, "byte must be in range(0, 256)");
            goto fail;
        }
        *pbyte = (uint8_t)ival;
        *psub = pbyte;
        *psublen = 1;
    }
    return 0;
fail:
    qp_bm_unexport(self);
    return -1;
}

/* find_internal: index or -1; -2 on error.  magic: 0 find, 1 index,
   2 rfind, 3 rindex */
static JSValue qp_bm_find(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                          JSValueConst kwnames, int magic)
{
    static const char *const names[] = { "find", "index", "rfind", "rindex" };
    Py_ssize_t start, end, sublen, len, res;
    QPBuffer buf;
    uint8_t byte;
    const uint8_t *sub, *str;
    int dir = magic < 2 ? 1 : -1;

    if (qp_bm_parse_finds(ctx, self, names[magic], args, nargs, &start, &end, &buf, &byte,
                          &sub, &sublen) < 0)
        return JS_EXCEPTION;
    str = qp_bytes_data(self, &len);
    QP_ADJUST_INDICES(start, end, len);
    if (end - start < sublen) {
        res = -1;
    } else if (sublen == 1) {
        Py_ssize_t i;
        res = -1;
        if (dir > 0) {
            const uint8_t *p = memchr(str + start, sub[0], end - start);
            if (p)
                res = p - str;
        } else {
            for (i = end - 1; i >= start; i--) {
                if (str[i] == sub[0]) {
                    res = i;
                    break;
                }
            }
        }
    } else {
        res = qp_sl_find(ctx->rt, qp_strview_make(1, str, len), qp_strview_make(1, sub, sublen),
                         start, end, dir);
        if (res == QP_SL_NOMEM) {
            qp_buffer_release(ctx, &buf);
            qp_bm_unexport(self);
            return qp_raise_memory_error(ctx);
        }
    }
    qp_buffer_release(ctx, &buf);
    qp_bm_unexport(self);
    if (res < 0 && (magic & 1))
        return qp_raise_value_error(ctx, "subsection not found");
    return qp_long_from_int64(ctx, res);
}

static JSValue qp_bm_count(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    Py_ssize_t start, end, sublen, len, res;
    QPBuffer buf;
    uint8_t byte;
    const uint8_t *sub, *str;

    if (qp_bm_parse_finds(ctx, self, "count", args, nargs, &start, &end, &buf, &byte, &sub,
                          &sublen) < 0)
        return JS_EXCEPTION;
    str = qp_bytes_data(self, &len);
    res = qp_sl_count(ctx->rt, qp_strview_make(1, str, len), qp_strview_make(1, sub, sublen),
                      start, end);
    qp_buffer_release(ctx, &buf);
    qp_bm_unexport(self);
    if (res < 0)
        return qp_raise_memory_error(ctx);
    return qp_long_from_int64(ctx, res);
}

/* tailmatch(): 1 found, 0 not, -1 error */
static int qp_bm_tailmatch1(JSContext *ctx, JSValueConst self, JSValueConst substr,
                            Py_ssize_t start, Py_ssize_t end, int direction)
{
    QPBuffer v;
    Py_ssize_t len, slen;
    const uint8_t *str;
    int r = 0;
    if (qp_buffer_get(ctx, substr, &v, QP_BUF_SIMPLE) < 0)
        return -1;
    slen = v.len;
    str = qp_bytes_data(self, &len);
    QP_ADJUST_INDICES(start, end, len);
    if (direction < 0) {
        if (start > len - slen)
            goto done;
    } else {
        if (end - start < slen || start > len)
            goto done;
        if (end - slen > start)
            start = end - slen;
    }
    if (end - start < slen)
        goto done;
    if (slen && memcmp(str + start, v.buf, slen) != 0)
        goto done;
    r = 1;
done:
    qp_buffer_release(ctx, &v);
    return r;
}

/* _Py_bytes_tailmatch after the start and end conversions */
static JSValue qp_bm_tailmatch_held(JSContext *ctx, JSValueConst self, const char *fname,
                                    JSValueConst subobj, Py_ssize_t start, Py_ssize_t end,
                                    int direction)
{
    int r;

    if (qp_tuple_check(ctx, subobj)) {
        Py_ssize_t i;
        for (i = 0; i < QP_TUPLE_SIZE(subobj); i++) {
            r = qp_bm_tailmatch1(ctx, self, QP_TUPLE_ITEMS(subobj)[i], start, end, direction);
            if (r < 0)
                return JS_EXCEPTION;
            if (r)
                return JS_TRUE;
        }
        return JS_FALSE;
    }
    r = qp_bm_tailmatch1(ctx, self, subobj, start, end, direction);
    if (r < 0) {
        if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError))) {
            JSValue n = qp_bm_tpname(ctx, subobj);
            if (JS_IsException(n))
                return n;
            qp_raise_type_error(ctx, "%s first arg must be bytes or a tuple of bytes, not %U",
                                fname, n);
            JS_FreeValue(ctx, n);
        }
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, r);
}

/* startswith / endswith (magic 0 / 1) */
static JSValue qp_bm_tailmatch(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, int magic)
{
    const char *fname = magic ? "endswith" : "startswith";
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    JSValue r;

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (nargs > 1 && qp_bm_slice_index(ctx, args[1], &start) < 0)
        return JS_EXCEPTION;
    if (nargs > 2 && qp_bm_slice_index(ctx, args[2], &end) < 0)
        return JS_EXCEPTION;
    qp_bm_export(self);
    r = qp_bm_tailmatch_held(ctx, self, fname, args[0], start, end, magic ? 1 : -1);
    qp_bm_unexport(self);
    return r;
}

static JSValue qp_bm_decode(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                            JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "encoding", "errors", NULL };
    static const QPArgSpec spec = { "decode", kw, 0, 2, 0 };
    JSValueConst a[2];
    const char *encoding = NULL, *errors = NULL;
    Py_ssize_t len;
    const uint8_t *d;
    JSValue r = JS_EXCEPTION;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0])) {
        encoding = qp_bm_arg_cstr(ctx, "decode", "encoding", a[0]);
        if (!encoding)
            return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(a[1])) {
        errors = qp_bm_arg_cstr(ctx, "decode", "errors", a[1]);
        if (!errors)
            goto done;
    }
    d = qp_bytes_data(self, &len);
    r = qp_str_decode(ctx, (const char *)d, len, encoding ? encoding : "utf-8",
                      errors ? errors : "strict");
done:
    if (encoding)
        JS_FreeCString(ctx, encoding);
    if (errors)
        JS_FreeCString(ctx, errors);
    return r;
}

/* stringlib bytes_join */
static JSValue qp_bm_join_impl(JSContext *ctx, JSValueConst self, JSValueConst iterable)
{
    JSValue seq, r = JS_EXCEPTION;
    JSValue *items;
    Py_ssize_t seqlen, i, nbufs = 0, sz = 0, seplen;
    QPBuffer *buffers = NULL;
    int bytearray = QP_BM_IS_BA(self);
    const uint8_t *sep;
    uint8_t *p;

    if (qp_list_check_exact(ctx, iterable) || qp_tuple_check_exact(ctx, iterable)) {
        seq = JS_DupValue(ctx, iterable);
    } else {
        JSValue it = qp_iter(ctx, iterable);
        if (JS_IsException(it)) {
            if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
                qp_raise_type_error(ctx, "can only join an iterable");
            return JS_EXCEPTION;
        }
        seq = qp_list_from_iterable(ctx, it);
        JS_FreeValue(ctx, it);
        if (JS_IsException(seq))
            return seq;
    }
    if (qp_tuple_check_exact(ctx, seq)) {
        seqlen = QP_TUPLE_SIZE(seq);
    } else {
        seqlen = qp_list_size(seq);
    }
    if (seqlen == 0) {
        r = qp_bm_new(ctx, bytearray, NULL, 0);
        goto out;
    }
    buffers = js_mallocz(ctx, sizeof(QPBuffer) * seqlen);
    if (!buffers)
        goto out;
    for (i = 0; i < seqlen; i++)
        buffers[i].obj = JS_UNDEFINED;
    for (i = 0; i < seqlen; i++) {
        JSValue item;
        if (qp_tuple_check_exact(ctx, seq)) {
            item = JS_DupValue(ctx, QP_TUPLE_ITEMS(seq)[i]);
        } else {
            Py_ssize_t n;
            items = qp_list_items(seq, &n);
            if (items && i < n)
                item = JS_DupValue(ctx, items[i]);
            else
                item = qp_getitem(ctx, seq, JS_NewInt32(ctx, (int32_t)i));
            if (JS_IsException(item))
                goto fail;
        }
        if (seqlen == 1 && !bytearray && qp_bytes_check_exact(ctx, item)) {
            r = item;
            goto fail;
        }
        if (qp_buffer_get(ctx, item, &buffers[i], QP_BUF_SIMPLE) < 0) {
            JSValue n;
            qp_bm_exc_clear(ctx);
            n = qp_bm_tpname(ctx, item);
            JS_FreeValue(ctx, item);
            if (!JS_IsException(n)) {
                qp_raise_type_error(ctx, "sequence item %zd: expected a bytes-like object, %U found",
                                    i, n);
                JS_FreeValue(ctx, n);
            }
            goto fail;
        }
        JS_FreeValue(ctx, item);
        nbufs = i + 1;
        if (buffers[i].len > PY_SSIZE_T_MAX - sz) {
            qp_raise_overflow_error(ctx, "join() result is too long");
            goto fail;
        }
        sz += buffers[i].len;
        /* an item's __buffer__() may have mutated the list */
        if (!qp_tuple_check_exact(ctx, seq) && qp_list_size(seq) != seqlen) {
            qp_raise(ctx, QP_EXC(ctx, RuntimeError), "sequence changed size during iteration");
            goto fail;
        }
    }
    sep = qp_bytes_data(self, &seplen);
    if (seplen) {
        if (seplen > (PY_SSIZE_T_MAX - sz) / (seqlen - 1 > 0 ? seqlen - 1 : 1)) {
            qp_raise_overflow_error(ctx, "join() result is too long");
            goto fail;
        }
        sz += seplen * (seqlen - 1);
    }
    r = qp_bm_new(ctx, bytearray, NULL, sz);
    if (JS_IsException(r))
        goto fail;
    p = QP_BYTES_OBJ(r)->data;
    sep = qp_bytes_data(self, &seplen);
    for (i = 0; i < nbufs; i++) {
        if (i > 0 && seplen) {
            memcpy(p, sep, seplen);
            p += seplen;
        }
        if (buffers[i].len) {
            memcpy(p, buffers[i].buf, buffers[i].len);
            p += buffers[i].len;
        }
    }
fail:
    if (buffers) {
        for (i = 0; i < nbufs; i++)
            qp_buffer_release(ctx, &buffers[i]);
        js_free(ctx, buffers);
    }
out:
    JS_FreeValue(ctx, seq);
    return r;
}

static JSValue qp_bm_join(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                          JSValueConst kwnames, int magic)
{
    JSValue r;
    if (!QP_BM_IS_BA(self))
        return qp_bm_join_impl(ctx, self, args[0]);
    /* bytearray_join_impl: exported while a custom iterator runs, so that
       it cannot clear/resize self */
    QP_BYTES_OBJ(self)->exports++;
    r = qp_bm_join_impl(ctx, self, args[0]);
    QP_BYTES_OBJ(self)->exports--;
    return r;
}

/* strip/lstrip/rstrip(bytes=None, /): magic = QP_SL_*STRIP */
static JSValue qp_bm_strip(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    static const char *const names[] = { "lstrip", "rstrip", "strip" };
    Py_ssize_t len;
    const uint8_t *d;
    qp_span sp;

    if (qp_arg_check_positional(ctx, names[magic], nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0 || JS_IsUndefined(args[0]) || JS_IsNull(args[0])) {
        d = qp_bytes_data(self, &len);
        qp_sl_strip_ws(qp_strview_make(1, d, len), magic, QP_SL_BYTES, &sp);
    } else {
        QPBuffer v;
        if (qp_buffer_get(ctx, args[0], &v, QP_BUF_SIMPLE) < 0)
            return JS_EXCEPTION;
        d = qp_bytes_data(self, &len);
        qp_sl_strip_chars(qp_strview_make(1, d, len), qp_strview_make(1, v.buf, v.len), magic, &sp);
        qp_buffer_release(ctx, &v);
        d = qp_bytes_data(self, &len);
    }
    if (sp.start == 0 && sp.end == len)
        return qp_bm_return_self(ctx, self);
    return qp_bm_new(ctx, QP_BM_IS_BA(self), d + sp.start, sp.end - sp.start);
}

/* bytes.maketrans(frm, to, /) (static) */
static JSValue qp_bm_maketrans(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, int magic)
{
    QPBuffer frm, to;
    uint8_t table[256];
    Py_ssize_t i;
    if (qp_arg_check_positional(ctx, "maketrans", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, args[0], &frm, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, args[1], &to, QP_BUF_SIMPLE) < 0) {
        qp_buffer_release(ctx, &frm);
        return JS_EXCEPTION;
    }
    if (frm.len != to.len) {
        qp_buffer_release(ctx, &frm);
        qp_buffer_release(ctx, &to);
        return qp_raise_value_error(ctx, "maketrans arguments must have same length");
    }
    for (i = 0; i < 256; i++)
        table[i] = (uint8_t)i;
    for (i = 0; i < frm.len; i++)
        table[frm.buf[i]] = to.buf[i];
    qp_buffer_release(ctx, &frm);
    qp_buffer_release(ctx, &to);
    return qp_bytes_new(ctx, table, 256);
}

/* partition / rpartition (magic 0 / 1) */
static JSValue qp_bm_partition(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, int magic)
{
    QPBuffer v;
    qp_span sp[3];
    Py_ssize_t len;
    const uint8_t *d;
    int st, bytearray = QP_BM_IS_BA(self);
    JSValue items[3];
    JSValue sepobj;

    if (qp_buffer_get(ctx, args[0], &v, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    if (bytearray) {
        sepobj = qp_bytearray_new(ctx, v.buf, v.len);
    } else {
        sepobj = JS_DupValue(ctx, args[0]);
    }
    if (JS_IsException(sepobj)) {
        qp_buffer_release(ctx, &v);
        return sepobj;
    }
    d = qp_bytes_data(self, &len);
    st = qp_sl_partition(ctx->rt, qp_strview_make(1, d, len), qp_strview_make(1, v.buf, v.len),
                         magic ? -1 : 1, sp);
    qp_buffer_release(ctx, &v);
    if (st < 0) {
        JS_FreeValue(ctx, sepobj);
        return qp_bm_sl_error(ctx, st, "");
    }
    if (st == 0) {
        JS_FreeValue(ctx, sepobj);
        if (bytearray) {
            items[0] = qp_bm_new(ctx, 1, magic ? NULL : d, magic ? 0 : len);
            items[1] = qp_bm_new(ctx, 1, NULL, 0);
            items[2] = qp_bm_new(ctx, 1, magic ? d : NULL, magic ? len : 0);
        } else {
            JSValue empty = qp_bytes_new(ctx, NULL, 0);
            if (magic) {
                items[0] = JS_DupValue(ctx, empty);
                items[1] = empty;
                items[2] = JS_DupValue(ctx, self);
            } else {
                items[0] = JS_DupValue(ctx, self);
                items[1] = JS_DupValue(ctx, empty);
                items[2] = empty;
            }
        }
    } else {
        items[0] = qp_bm_new(ctx, bytearray, d + sp[0].start, sp[0].end - sp[0].start);
        items[1] = sepobj;
        items[2] = qp_bm_new(ctx, bytearray, d + sp[2].start, sp[2].end - sp[2].start);
    }
    if (JS_IsException(items[0]) || JS_IsException(items[1]) || JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[2]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 3, items);
}

/* replace(old, new, /, count=-1) */
static JSValue qp_bm_replace(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                             JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "", "count", NULL };
    static const QPArgSpec spec = { "replace", kw, 2, 3, 0 };
    JSValueConst a[3];
    Py_ssize_t count = -1, len;
    QPBuffer vold, vnew;
    const uint8_t *d;
    qp_ucsbuilder b;
    int st;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, a[0], &vold, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, a[1], &vnew, QP_BUF_SIMPLE) < 0) {
        qp_buffer_release(ctx, &vold);
        return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(a[2]) && qp_arg_ssize(ctx, a[2], &count) < 0) {
        qp_buffer_release(ctx, &vold);
        qp_buffer_release(ctx, &vnew);
        return JS_EXCEPTION;
    }
    d = qp_bytes_data(self, &len);
    qp_ucsbuilder_init(&b, ctx->rt);
    st = qp_sl_replace(qp_strview_make(1, d, len), qp_strview_make(1, vold.buf, vold.len),
                       qp_strview_make(1, vnew.buf, vnew.len), count, &b);
    qp_buffer_release(ctx, &vold);
    qp_buffer_release(ctx, &vnew);
    if (st == QP_SL_UNCHANGED)
        return qp_bm_return_self(ctx, self);
    if (st != QP_SL_BUILT) {
        qp_ucsbuilder_free(&b);
        return qp_bm_sl_error(ctx, st, "replace bytes is too long");
    }
    return qp_bm_from_builder(ctx, QP_BM_IS_BA(self), &b);
}

/* removeprefix / removesuffix (magic 0 / 1) */
static JSValue qp_bm_removeaffix(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                 JSValueConst kwnames, int magic)
{
    QPBuffer v;
    Py_ssize_t len;
    const uint8_t *d;
    JSValue r;
    if (qp_buffer_get(ctx, args[0], &v, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    d = qp_bytes_data(self, &len);
    if (len >= v.len && v.len > 0) {
        if (magic == 0 && memcmp(d, v.buf, v.len) == 0) {
            r = qp_bm_new(ctx, QP_BM_IS_BA(self), d + v.len, len - v.len);
            goto done;
        }
        if (magic == 1 && memcmp(d + len - v.len, v.buf, v.len) == 0) {
            r = qp_bm_new(ctx, QP_BM_IS_BA(self), d, len - v.len);
            goto done;
        }
    }
    r = qp_bm_return_self(ctx, self);
done:
    qp_buffer_release(ctx, &v);
    return r;
}

/* List of pieces given by spans; a single whole span is return_self. */
static JSValue qp_bm_spans_to_list(JSContext *ctx, JSValueConst self, qp_spanvec *vec)
{
    Py_ssize_t i, len;
    const uint8_t *d = qp_bytes_data(self, &len);
    int bytearray = QP_BM_IS_BA(self);
    JSValue list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    for (i = 0; i < vec->len; i++) {
        JSValue piece;
        int rc;
        if (vec->len == 1 && vec->items[0].start == 0 && vec->items[0].end == len)
            piece = qp_bm_return_self(ctx, self);
        else
            piece = qp_bm_new(ctx, bytearray, d + vec->items[i].start,
                              vec->items[i].end - vec->items[i].start);
        if (JS_IsException(piece))
            goto fail;
        rc = qp_list_append(ctx, list, piece);
        JS_FreeValue(ctx, piece);
        if (rc < 0)
            goto fail;
    }
    return list;
fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* split / rsplit(sep=None, maxsplit=-1): magic 0 / 1 */
static JSValue qp_bm_split(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "sep", "maxsplit", NULL };
    QPArgSpec spec = { magic ? "rsplit" : "split", kw, 0, 2, 0 };
    JSValueConst a[2];
    Py_ssize_t maxsplit = -1, len;
    QPBuffer v;
    qp_spanvec vec;
    qp_strview sepv;
    const uint8_t *d;
    int st, has_sep = 0;
    JSValue r;

    v.obj = JS_UNDEFINED;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_ssize(ctx, a[1], &maxsplit) < 0)
        return JS_EXCEPTION;
    qp_bm_export(self);
    if (!QP_IS_NULL(a[0]) && !JS_IsUndefined(a[0]) && !JS_IsNull(a[0])) {
        if (qp_buffer_get(ctx, a[0], &v, QP_BUF_SIMPLE) < 0) {
            qp_bm_unexport(self);
            return JS_EXCEPTION;
        }
        sepv = qp_strview_make(1, v.buf, v.len);
        has_sep = 1;
    }
    d = qp_bytes_data(self, &len);
    qp_spanvec_init(&vec, ctx->rt);
    if (magic)
        st = qp_sl_rsplit(ctx->rt, qp_strview_make(1, d, len), has_sep ? &sepv : NULL,
                          maxsplit, QP_SL_BYTES, &vec);
    else
        st = qp_sl_split(ctx->rt, qp_strview_make(1, d, len), has_sep ? &sepv : NULL,
                         maxsplit, QP_SL_BYTES, &vec);
    qp_buffer_release(ctx, &v);
    qp_bm_unexport(self);
    if (st < 0) {
        qp_spanvec_free(&vec);
        return qp_bm_sl_error(ctx, st, "");
    }
    r = qp_bm_spans_to_list(ctx, self, &vec);
    qp_spanvec_free(&vec);
    return r;
}

static JSValue qp_bm_splitlines(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "keepends", NULL };
    static const QPArgSpec spec = { "splitlines", kw, 0, 1, 0 };
    JSValueConst a[1];
    int keepends = 0, st;
    Py_ssize_t len;
    const uint8_t *d;
    qp_spanvec vec;
    JSValue r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_bool(ctx, a[0], &keepends) < 0)
        return JS_EXCEPTION;
    d = qp_bytes_data(self, &len);
    qp_spanvec_init(&vec, ctx->rt);
    st = qp_sl_splitlines(qp_strview_make(1, d, len), keepends, QP_SL_BYTES, &vec);
    if (st < 0) {
        qp_spanvec_free(&vec);
        return qp_raise_memory_error(ctx);
    }
    r = qp_bm_spans_to_list(ctx, self, &vec);
    qp_spanvec_free(&vec);
    return r;
}

/* translate(table, /, delete=b'') */
static JSValue qp_bm_translate(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "delete", NULL };
    static const QPArgSpec spec = { "translate", kw, 1, 2, 0 };
    JSValueConst a[2];
    QPBuffer tv, dv;
    const uint8_t *table_chars = NULL, *input;
    Py_ssize_t tablen = 256, dellen = 0, inlen, i;
    int trans_table[256], changed = 0, bytearray = QP_BM_IS_BA(self);
    uint8_t *output, *out0;
    JSValue result;

    tv.obj = dv.obj = JS_UNDEFINED;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(a[0]) && !JS_IsNull(a[0])) {
        if (qp_buffer_get(ctx, a[0], &tv, QP_BUF_SIMPLE) < 0)
            return JS_EXCEPTION;
        table_chars = tv.buf;
        tablen = tv.len;
    }
    if (tablen != 256) {
        qp_buffer_release(ctx, &tv);
        return qp_raise_value_error(ctx, "translation table must be 256 characters long");
    }
    if (!QP_IS_NULL(a[1])) {
        if (qp_buffer_get(ctx, a[1], &dv, QP_BUF_SIMPLE) < 0) {
            qp_buffer_release(ctx, &tv);
            return JS_EXCEPTION;
        }
        dellen = dv.len;
    }
    input = qp_bytes_data(self, &inlen);
    out0 = output = js_malloc(ctx, inlen + 1);
    if (!output) {
        result = JS_EXCEPTION;
        goto done;
    }
    if (dellen == 0 && table_chars != NULL) {
        for (i = 0; i < inlen; i++)
            output[i] = table_chars[input[i]];
        if (!bytearray && qp_bytes_check_exact(ctx, self) &&
            (inlen == 0 || memcmp(input, output, inlen) == 0))
            result = JS_DupValue(ctx, self);
        else
            result = qp_bm_new(ctx, bytearray, output, inlen);
        goto done;
    }
    for (i = 0; i < 256; i++)
        trans_table[i] = table_chars ? table_chars[i] : (int)i;
    for (i = 0; i < dellen; i++)
        trans_table[dv.buf[i]] = -1;
    for (i = 0; i < inlen; i++) {
        int c = input[i];
        if (trans_table[c] != -1) {
            *output++ = (uint8_t)trans_table[c];
            if (trans_table[c] == c)
                continue;
        }
        changed = 1;
    }
    if (!changed && !bytearray && qp_bytes_check_exact(ctx, self))
        result = JS_DupValue(ctx, self);
    else
        result = qp_bm_new(ctx, bytearray, out0, output - out0);
done:
    if (out0)
        js_free(ctx, out0);
    qp_buffer_release(ctx, &tv);
    qp_buffer_release(ctx, &dv);
    return result;
}

/* ------------------------------------------------------------------------ */
/* bytes-only methods                                                        */
/* ------------------------------------------------------------------------ */

static JSValue qp_bytes_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    Py_ssize_t len;
    const uint8_t *d = qp_bytes_data(self, &len);
    JSValue b = qp_bytes_new(ctx, d, len);
    if (JS_IsException(b))
        return b;
    return qp_tuple_new_steal(ctx, 1, &b);
}

static JSValue qp_bytes_dunder_bytes(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                     JSValueConst kwnames, int magic)
{
    Py_ssize_t len;
    const uint8_t *d;
    if (qp_bytes_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    d = qp_bytes_data(self, &len);
    return qp_bytes_new(ctx, d, len);
}

static JSValue qp_bytes_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, int magic)
{
    /* PyBytesObject_SIZE (33 on 64-bit hosts) + len */
    return qp_long_from_int64(ctx, 33 + QP_BYTES_OBJ(self)->len);
}

/* ------------------------------------------------------------------------ */
/* PEP 461 formatting (_PyBytes_FormatEx)                                    */
/* ------------------------------------------------------------------------ */

#define QP_F_LJUST (1 << 0)
#define QP_F_SIGN  (1 << 1)
#define QP_F_BLANK (1 << 2)
#define QP_F_ALT   (1 << 3)
#define QP_F_ZERO  (1 << 4)

/* FORMAT_ERROR prefix + message (message is formatted by the caller into
   a str; we prepend the argument designation). */
static void qp_bm_format_error(JSContext *ctx, QPType *exc, JSValueConst key, Py_ssize_t argidx,
                               JSValue msg)
{
    if (JS_IsException(msg))
        return;
    if (!QP_IS_NULL(key))
        qp_raise(ctx, exc, "format argument %R: %U", key, msg);
    else if (argidx >= 0)
        qp_raise(ctx, exc, "format argument %zd: %U", argidx, msg);
    else
        qp_raise(ctx, exc, "format argument: %U", msg);
    JS_FreeValue(ctx, msg);
}

static JSValueConst qp_bm_getnextarg(JSContext *ctx, JSValueConst args, Py_ssize_t arglen,
                                Py_ssize_t *p_argidx, int allowone)
{
    Py_ssize_t argidx = *p_argidx;
    if (argidx < arglen) {
        (*p_argidx)++;
        if (arglen >= 0)
            return QP_TUPLE_ITEMS(args)[argidx];
        if (allowone)
            return args;
    }
    qp_raise_type_error(ctx, "not enough arguments for format string (got %zd)",
                        arglen < 0 ? (Py_ssize_t)1 : arglen);
    return JS_EXCEPTION;
}

/* _PyUnicode_FormatLong as a malloc'd ASCII buffer (js_free). */
static char *qp_bm_format_long(JSContext *ctx, JSValueConst val, int alt, int prec, int type,
                               Py_ssize_t *plen)
{
    JSValue v, s;
    const char *buf;
    size_t llen;
    char *res, *b1;
    int base, sign, numnondigits = 0, numdigits, len;
    Py_ssize_t i;

    if (prec > INT_MAX - 3) {
        qp_raise_overflow_error(ctx, "precision too large");
        return NULL;
    }
    base = (type == 'o') ? 8 : (type == 'x' || type == 'X') ? 16 : 10;
    if (base != 10)
        numnondigits = 2;
    v = qp_long_value(ctx, val);
    if (JS_IsException(v))
        return NULL;
    s = qp_long_to_str(ctx, v, base, base != 10);
    JS_FreeValue(ctx, v);
    if (JS_IsException(s))
        return NULL;
    buf = qp_str_utf8(ctx, s, &llen);
    JS_FreeValue(ctx, s);
    if (!buf)
        return NULL;
    if (llen > INT_MAX) {
        JS_FreeCString(ctx, buf);
        qp_raise_value_error(ctx, "string too large in _PyUnicode_FormatLong");
        return NULL;
    }
    len = (int)llen;
    sign = buf[0] == '-';
    numnondigits += sign;
    numdigits = len - numnondigits;
    res = js_malloc(ctx, (size_t)numnondigits + (prec > numdigits ? prec : numdigits) + 1);
    if (!res) {
        JS_FreeCString(ctx, buf);
        return NULL;
    }
    b1 = res;
    /* sign, then the base marker when alt */
    if (sign)
        *b1++ = '-';
    if (base != 10) {
        if (alt) {
            *b1++ = '0';
            *b1++ = (char)type;
        } else {
            numnondigits -= 2;
        }
    }
    for (i = prec - numdigits; i > 0; i--)
        *b1++ = '0';
    memcpy(b1, buf + len - numdigits, numdigits);
    b1 += numdigits;
    JS_FreeCString(ctx, buf);
    if (type == 'X') {
        char *q;
        for (q = res; q < b1; q++)
            if (*q >= 'a' && *q <= 'z')
                *q -= 32;
    }
    *plen = b1 - res;
    return res;
}

/* format_obj: %b / %s operand as a bytes-like object.  *ptemp receives a
   new reference keeping (*pbuf, *plen) alive. */
static int qp_bm_format_obj(JSContext *ctx, JSValueConst v, Py_ssize_t argidx, JSValueConst key,
                            const uint8_t **pbuf, Py_ssize_t *plen, JSValue *ptemp)
{
    JSValue func, result;
    int k = qp_bk_kind(v);
    if (k == QP_BK_BYTES || k == QP_BK_BYTEARRAY) {
        *pbuf = qp_bytes_data(v, plen);
        *ptemp = JS_DupValue(ctx, v);
        return 0;
    }
    func = qp_lookup_special(ctx, v, QP_ATOM(ctx, __bytes__));
    if (JS_IsException(func))
        return -1;
    if (!QP_IS_NULL(func)) {
        result = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(result))
            return -1;
        if (!qp_bytes_check(ctx, result)) {
            qp_raise_type_error(ctx, "%T.__bytes__() must return a bytes, not %T", v, result);
            JS_FreeValue(ctx, result);
            return -1;
        }
        *pbuf = qp_bytes_data(result, plen);
        *ptemp = result;
        return 0;
    }
    if (qp_buffer_check(ctx, v)) {
        result = qp_bytes_from_buffer(ctx, v, 0);
        if (JS_IsException(result))
            return -1;
        *pbuf = qp_bytes_data(result, plen);
        *ptemp = result;
        return 0;
    }
    qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                       qp_str_from_format(ctx, "%%b requires a bytes-like object, "
                                          "or an object that implements __bytes__, not %T", v));
    return -1;
}

static int qp_bm_byte_converter(JSContext *ctx, JSValueConst arg, Py_ssize_t argidx,
                                JSValueConst key, uint8_t *p)
{
    int k = qp_bk_kind(arg);
    if (k == QP_BK_BYTES || k == QP_BK_BYTEARRAY) {
        QPBytesObject *b = QP_BYTES_OBJ(arg);
        if (b->len != 1) {
            qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                               qp_str_from_format(ctx, "%%c requires an integer in range(256) or "
                                                  "a single byte, not a %s object of length %zd",
                                                  k == QP_BK_BYTES ? "bytes" : "bytearray",
                                                  b->len));
            return -1;
        }
        *p = b->data[0];
        return 0;
    }
    if (qp_index_check(ctx, arg)) {
        JSValue iv = qp_index(ctx, arg);
        int64_t ival;
        int overflow;
        if (JS_IsException(iv))
            return -1;
        qp_long_as_int64_overflow(iv, &ival, &overflow);
        JS_FreeValue(ctx, iv);
        if (overflow || !(0 <= ival && ival <= 255)) {
            qp_bm_format_error(ctx, QP_EXC(ctx, OverflowError), key, argidx,
                               qp_str_from_cstr(ctx, "%c argument not in range(256)"));
            return -1;
        }
        *p = (uint8_t)ival;
        return 0;
    }
    qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                       qp_str_from_format(ctx, "%%c requires an integer in range(256) or "
                                          "a single byte, not %T", arg));
    return -1;
}

static JSValue qp_bytes_format(JSContext *ctx, const uint8_t *format, Py_ssize_t format_len,
                               JSValueConst args_in, int use_bytearray)
{
    const uint8_t *fmt = format;
    Py_ssize_t fmtcnt = format_len, arglen, argidx;
    JSValue args = JS_DupValue(ctx, args_in);
    JSValue key = QP_NULL;
    int dict = 0;
    DynBuf db;
    JSValue r;
    uint8_t *fmtcopy;

    /* the format may live in a bytearray mutated by __index__ & co. */
    fmtcopy = js_malloc(ctx, format_len + 1);
    if (!fmtcopy) {
        JS_FreeValue(ctx, args);
        return JS_EXCEPTION;
    }
    if (format_len)
        memcpy(fmtcopy, format, format_len);
    fmt = format = fmtcopy;

    js_dbuf_init(ctx, &db);
    if (qp_tuple_check(ctx, args)) {
        arglen = QP_TUPLE_SIZE(args);
        argidx = 0;
    } else {
        arglen = -1;
        argidx = -2;
    }
    {
        QPType *t = qp_type_of(ctx, args);
        if (t->slots.mp.mp_subscript && !qp_tuple_check(ctx, args) &&
            !qp_bytes_check(ctx, args) && !qp_str_check(ctx, args) &&
            !qp_bytearray_check(ctx, args))
            dict = 1;
    }

    while (--fmtcnt >= 0) {
        if (*fmt != '%') {
            Py_ssize_t len;
            const uint8_t *pos = memchr(fmt + 1, '%', fmtcnt);
            if (pos != NULL)
                len = pos - fmt;
            else
                len = fmtcnt + 1;
            if (dbuf_put(&db, fmt, len))
                goto nomem;
            fmt += len;
            fmtcnt -= (len - 1);
        } else {
            int flags = 0;
            Py_ssize_t width = -1;
            int prec = -1;
            int c = '\0';
            int fill, sign;
            JSValueConst v = JS_UNDEFINED;
            JSValue temp = JS_UNDEFINED;
            char *tempbuf = NULL;
            const uint8_t *pbuf = NULL;
            Py_ssize_t len = 0;
            uint8_t onechar;
            const uint8_t *fmtstart;

            fmt++;
            if (*fmt == '%') {
                if (dbuf_putc(&db, '%'))
                    goto nomem;
                fmt++;
                fmtcnt--;
                continue;
            }
            JS_FreeValue(ctx, key);
            key = QP_NULL;
            fmtstart = fmt;
            if (*fmt == '(') {
                const uint8_t *keystart;
                Py_ssize_t keylen;
                int pcount = 1;
                JSValue nargs;

                if (!dict) {
                    qp_raise_type_error(ctx, "format requires a mapping, not %T", args);
                    goto error;
                }
                ++fmt;
                --fmtcnt;
                keystart = fmt;
                while (pcount > 0 && --fmtcnt >= 0) {
                    if (*fmt == ')')
                        --pcount;
                    else if (*fmt == '(')
                        ++pcount;
                    fmt++;
                }
                keylen = fmt - keystart - 1;
                if (fmtcnt < 0 || pcount > 0) {
                    qp_raise_value_error(ctx, "stray %% or incomplete format key at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1));
                    goto error;
                }
                key = qp_bytes_new(ctx, keystart, keylen);
                if (JS_IsException(key)) {
                    key = QP_NULL;
                    goto error;
                }
                nargs = qp_getitem(ctx, args, key);
                if (JS_IsException(nargs))
                    goto error;
                JS_FreeValue(ctx, args);
                args = nargs;
                arglen = -3;
                argidx = -4;
            } else {
                if (arglen < -1) {
                    qp_raise_value_error(ctx, "format requires a parenthesised mapping key at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1));
                    goto error;
                }
            }

            while (--fmtcnt >= 0) {
                switch (c = *fmt++) {
                case '-': flags |= QP_F_LJUST; continue;
                case '+': flags |= QP_F_SIGN; continue;
                case ' ': flags |= QP_F_BLANK; continue;
                case '#': flags |= QP_F_ALT; continue;
                case '0': flags |= QP_F_ZERO; continue;
                }
                break;
            }

            if (c == '*') {
                if (arglen < -1) {
                    qp_raise_value_error(ctx, "* cannot be used with a parenthesised mapping key at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1));
                    goto error;
                }
                v = qp_bm_getnextarg(ctx, args, arglen, &argidx, 0);
                if (JS_IsException(v))
                    goto error;
                if (!qp_long_check(ctx, v)) {
                    qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                                       qp_str_from_format(ctx, "* requires int, not %T", v));
                    goto error;
                }
                {
                    JSValue iv = qp_long_value(ctx, v);
                    int rc;
                    if (JS_IsException(iv))
                        goto error;
                    rc = qp_long_as_ssize(ctx, iv, &width);
                    JS_FreeValue(ctx, iv);
                    if (rc < 0) {
                        if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, OverflowError)))
                            qp_bm_format_error(ctx, QP_EXC(ctx, OverflowError), key, argidx,
                                               qp_str_from_cstr(ctx, "too big for width"));
                        goto error;
                    }
                }
                if (width < 0) {
                    if (width == PY_SSIZE_T_MIN) {
                        qp_bm_format_error(ctx, QP_EXC(ctx, OverflowError), key, argidx,
                                           qp_str_from_cstr(ctx, "too big for width"));
                        goto error;
                    }
                    flags |= QP_F_LJUST;
                    width = -width;
                }
                if (--fmtcnt >= 0)
                    c = *fmt++;
            } else if (QPB_ISDIGIT(c)) {
                width = c - '0';
                while (--fmtcnt >= 0) {
                    c = *fmt++;
                    if (!QPB_ISDIGIT(c))
                        break;
                    if (width > (PY_SSIZE_T_MAX - (c - '0')) / 10) {
                        qp_raise_value_error(ctx, "width too big at position %zd",
                                             (Py_ssize_t)(fmtstart - format - 1));
                        goto error;
                    }
                    width = width * 10 + (c - '0');
                }
            }

            if (c == '.') {
                prec = 0;
                if (--fmtcnt >= 0)
                    c = *fmt++;
                if (c == '*') {
                    if (arglen < -1) {
                        qp_raise_value_error(ctx, "* cannot be used with a parenthesised mapping key at position %zd",
                                             (Py_ssize_t)(fmtstart - format - 1));
                        goto error;
                    }
                    v = qp_bm_getnextarg(ctx, args, arglen, &argidx, 0);
                    if (JS_IsException(v))
                        goto error;
                    if (!qp_long_check(ctx, v)) {
                        qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                                           qp_str_from_format(ctx, "* requires int, not %T", v));
                        goto error;
                    }
                    {
                        JSValue iv = qp_long_value(ctx, v);
                        int64_t pv;
                        int ovf;
                        if (JS_IsException(iv))
                            goto error;
                        qp_long_as_int64_overflow(iv, &pv, &ovf);
                        JS_FreeValue(ctx, iv);
                        if (ovf || pv > INT_MAX || pv < INT_MIN) {
                            qp_bm_format_error(ctx, QP_EXC(ctx, OverflowError), key, argidx,
                                               qp_str_from_cstr(ctx, "too big for precision"));
                            goto error;
                        }
                        prec = (int)pv;
                    }
                    if (prec < 0)
                        prec = 0;
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                } else if (QPB_ISDIGIT(c)) {
                    prec = c - '0';
                    while (--fmtcnt >= 0) {
                        c = *fmt++;
                        if (!QPB_ISDIGIT(c))
                            break;
                        if (prec > (INT_MAX - (c - '0')) / 10) {
                            qp_raise_value_error(ctx, "precision too big at position %zd",
                                                 (Py_ssize_t)(fmtstart - format - 1));
                            goto error;
                        }
                        prec = prec * 10 + (c - '0');
                    }
                }
            }
            if (fmtcnt >= 0) {
                if (c == 'h' || c == 'l' || c == 'L') {
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                }
            }
            if (fmtcnt < 0) {
                qp_raise_value_error(ctx, "stray %% at position %zd",
                                     (Py_ssize_t)(fmtstart - format - 1));
                goto error;
            }
            v = qp_bm_getnextarg(ctx, args, arglen, &argidx, 1);
            if (JS_IsException(v))
                goto error;

            sign = 0;
            fill = ' ';
            switch (c) {
            case 'r':
            case 'a': {
                JSValue a = qp_ascii(ctx, v);
                size_t alen;
                const char *as;
                if (JS_IsException(a))
                    goto error;
                as = qp_str_utf8(ctx, a, &alen);
                JS_FreeValue(ctx, a);
                if (!as)
                    goto error;
                tempbuf = js_malloc(ctx, alen + 1);
                if (!tempbuf) {
                    JS_FreeCString(ctx, as);
                    goto error;
                }
                memcpy(tempbuf, as, alen);
                JS_FreeCString(ctx, as);
                pbuf = (const uint8_t *)tempbuf;
                len = alen;
                if (prec >= 0 && len > prec)
                    len = prec;
                break;
            }
            case 's':
            case 'b':
                if (qp_bm_format_obj(ctx, v, argidx, key, &pbuf, &len, &temp) < 0)
                    goto error;
                if (prec >= 0 && len > prec)
                    len = prec;
                break;
            case 'i':
            case 'd':
            case 'u':
            case 'o':
            case 'x':
            case 'X':
                if (qp_long_check_exact(v) && width == -1 && prec == -1 &&
                    !(flags & (QP_F_SIGN | QP_F_BLANK)) && c != 'X') {
                    int base = (c == 'o') ? 8 : (c == 'x') ? 16 : 10;
                    JSValue s = qp_long_to_str(ctx, v, base, (flags & QP_F_ALT) != 0);
                    size_t slen;
                    const char *ss;
                    int rc;
                    if (JS_IsException(s))
                        goto error;
                    ss = qp_str_utf8(ctx, s, &slen);
                    JS_FreeValue(ctx, s);
                    if (!ss)
                        goto error;
                    rc = dbuf_put(&db, (const uint8_t *)ss, slen);
                    JS_FreeCString(ctx, ss);
                    if (rc)
                        goto nomem;
                    continue;
                }
                if (qp_long_check(ctx, v)) {
                    tempbuf = qp_bm_format_long(ctx, v, flags & QP_F_ALT, prec, c, &len);
                    if (!tempbuf)
                        goto error;
                } else {
                    int ok = 0;
                    if (qp_bm_number_check(ctx, v)) {
                        JSValue iobj;
                        if (c == 'o' || c == 'x' || c == 'X')
                            iobj = qp_index(ctx, v);
                        else
                            iobj = qp_number_long(ctx, v);
                        if (!JS_IsException(iobj)) {
                            tempbuf = qp_bm_format_long(ctx, iobj, flags & QP_F_ALT, prec, c, &len);
                            JS_FreeValue(ctx, iobj);
                            if (!tempbuf)
                                goto error;
                            ok = 1;
                        } else if (!qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError))) {
                            goto error;
                        }
                    }
                    if (!ok) {
                        qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                                           qp_str_from_format(ctx, "%%%c requires %s, not %T", c,
                                                              (c == 'o' || c == 'x' || c == 'X') ?
                                                              "an integer" : "a real number", v));
                        goto error;
                    }
                }
                pbuf = (const uint8_t *)tempbuf;
                sign = 1;
                if (flags & QP_F_ZERO)
                    fill = '0';
                break;
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G': {
                double x;
                char *p;
                int dtype;
                qp_error err;
                if (qp_float_as_double(ctx, v, &x) < 0) {
                    if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
                        qp_bm_format_error(ctx, QP_EXC(ctx, TypeError), key, argidx,
                                           qp_str_from_format(ctx, "%%%c requires a real number, not %T",
                                                              c, v));
                    goto error;
                }
                memset(&err, 0, sizeof(err));
                p = qp_double_to_string(x, (char)c, prec < 0 ? 6 : prec,
                                        (flags & QP_F_ALT) ? QP_DTSF_ALT : 0, &dtype, &err);
                if (!p) {
                    if (err.kind == QP_ERR_MEMORY || !err.msg)
                        qp_raise_memory_error(ctx);
                    else
                        qp_raise_value_error(ctx, "%s", err.msg);
                    qp_error_clear(&err);
                    goto error;
                }
                len = strlen(p);
                tempbuf = js_malloc(ctx, len + 1);
                if (!tempbuf) {
                    qp_sys_free(p);
                    goto error;
                }
                memcpy(tempbuf, p, len + 1);
                qp_sys_free(p);
                pbuf = (const uint8_t *)tempbuf;
                if (width == -1 && prec == -1 && !(flags & (QP_F_SIGN | QP_F_BLANK))) {
                    int rc = dbuf_put(&db, pbuf, len);
                    js_free(ctx, tempbuf);
                    if (rc)
                        goto nomem;
                    continue;
                }
                sign = 1;
                if (flags & QP_F_ZERO)
                    fill = '0';
                break;
            }
            case 'c':
                if (qp_bm_byte_converter(ctx, v, argidx, key, &onechar) < 0)
                    goto error;
                pbuf = &onechar;
                len = 1;
                if (width == -1) {
                    if (dbuf_putc(&db, onechar))
                        goto nomem;
                    continue;
                }
                break;
            default:
                if (QPB_ISALPHA(c)) {
                    qp_raise_value_error(ctx, "unsupported format %%%c at position %zd",
                                         c, (Py_ssize_t)(fmtstart - format - 1));
                } else if (c == '\'') {
                    qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                         "format character \"'\" at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1),
                                         (Py_ssize_t)(fmt - format - 1));
                } else if (c >= 32 && c < 127) {
                    qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                         "format character '%c' at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1), c,
                                         (Py_ssize_t)(fmt - format - 1));
                } else {
                    qp_raise_value_error(ctx, "stray %% at position %zd or unexpected "
                                         "format character with code 0x%02x at position %zd",
                                         (Py_ssize_t)(fmtstart - format - 1), c & 0xff,
                                         (Py_ssize_t)(fmt - format - 1));
                }
                goto error;
            }

            if (sign) {
                if (*pbuf == '-' || *pbuf == '+') {
                    sign = *pbuf++;
                    len--;
                } else if (flags & QP_F_SIGN) {
                    sign = '+';
                } else if (flags & QP_F_BLANK) {
                    sign = ' ';
                } else {
                    sign = 0;
                }
            }
            if (width < len)
                width = len;
            /* the whole field at once (CPython _PyBytesWriter_Prepare): a
               width that cannot be allocated is a MemoryError, not a
               byte-by-byte padding loop */
            if (dbuf_claim(&db, (size_t)width + 1))
                goto nomem_item;
            if (sign) {
                if (fill != ' ' && dbuf_putc(&db, (uint8_t)sign))
                    goto nomem_item;
                if (width > len)
                    width--;
            }
            if ((flags & QP_F_ALT) && (c == 'o' || c == 'x' || c == 'X')) {
                if (fill != ' ') {
                    if (dbuf_put(&db, pbuf, 2))
                        goto nomem_item;
                    pbuf += 2;
                }
                width -= 2;
                if (width < 0)
                    width = 0;
                len -= 2;
            }
            if (width > len && !(flags & QP_F_LJUST)) {
                Py_ssize_t n;
                for (n = width - len; n > 0; n--)
                    if (dbuf_putc(&db, (uint8_t)fill))
                        goto nomem_item;
                width = len;
            }
            if (fill == ' ') {
                if (sign && dbuf_putc(&db, (uint8_t)sign))
                    goto nomem_item;
                if ((flags & QP_F_ALT) && (c == 'o' || c == 'x' || c == 'X')) {
                    if (dbuf_put(&db, pbuf, 2))
                        goto nomem_item;
                    pbuf += 2;
                }
            }
            if (len > 0 && dbuf_put(&db, pbuf, len))
                goto nomem_item;
            if (width > len) {
                Py_ssize_t n;
                for (n = width - len; n > 0; n--)
                    if (dbuf_putc(&db, ' '))
                        goto nomem_item;
            }
            if (dict && (argidx < arglen)) {
                qp_raise_type_error(ctx, "not all arguments converted during bytes formatting");
                JS_FreeValue(ctx, temp);
                if (tempbuf)
                    js_free(ctx, tempbuf);
                goto error;
            }
            JS_FreeValue(ctx, temp);
            if (tempbuf)
                js_free(ctx, tempbuf);
            continue;
        nomem_item:
            JS_FreeValue(ctx, temp);
            if (tempbuf)
                js_free(ctx, tempbuf);
            goto nomem;
        }
    }

    if (argidx < arglen && !dict) {
        qp_raise_type_error(ctx, "not all arguments converted during bytes formatting "
                            "(required %zd, got %zd)",
                            arglen < 0 ? (Py_ssize_t)0 : argidx,
                            arglen < 0 ? (Py_ssize_t)1 : arglen);
        goto error;
    }
    r = qp_bm_new(ctx, use_bytearray, db.buf, db.size);
    dbuf_free(&db);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, args);
    js_free(ctx, fmtcopy);
    return r;
nomem:
    qp_raise_memory_error(ctx);
error:
    dbuf_free(&db);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, args);
    js_free(ctx, fmtcopy);
    return JS_EXCEPTION;
}

/* nb_remainder of bytes and bytearray (magic-free slot) */
static JSValue qp_bm_mod(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    int k = qp_bk_kind(self);
    Py_ssize_t len;
    const uint8_t *d;
    JSValue r;
    if (k != QP_BK_BYTES && k != QP_BK_BYTEARRAY)
        return qp_not_implemented(ctx);
    /* the format runs __repr__/__format__...: d stays valid */
    qp_bm_export(self);
    d = qp_bytes_data(self, &len);
    r = qp_bytes_format(ctx, d, len, arg, k == QP_BK_BYTEARRAY);
    qp_bm_unexport(self);
    return r;
}

/* ------------------------------------------------------------------------ */
/* GC                                                                        */
/* ------------------------------------------------------------------------ */

static void qp_bytes_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    void *op = p->u.opaque;
    if (!op)
        return;
    p->u.opaque = NULL;
    switch (*(uint8_t *)op) {
    case QP_BK_BYTES:
        js_free_rt(rt, op);
        break;
    case QP_BK_BYTEARRAY:
        qp_bytearray_free_payload(rt, op);
        break;
    case QP_BK_MEMORYVIEW:
        qp_memoryview_finalize(rt, op);
        break;
    case QP_BK_MBUF:
        qp_mbuf_finalize(rt, op);
        break;
    }
}

static void qp_bytes_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    void *op = p->u.opaque;
    if (!op)
        return;
    switch (*(uint8_t *)op) {
    case QP_BK_MEMORYVIEW:
        qp_memoryview_mark(rt, op, mark_func);
        break;
    case QP_BK_MBUF:
        qp_mbuf_mark(rt, op, mark_func);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* type                                                                      */
/* ------------------------------------------------------------------------ */

static const QPMethodDef qp_bytes_methods[] = {
    { "__getnewargs__", qp_bytes_getnewargs, QP_METH_NOARGS, 0, NULL },
    { "__bytes__", qp_bytes_dunder_bytes, QP_METH_NOARGS, 0,
      "__bytes__($self, /)\n--\n\nConvert this value to exact type bytes." },
    { "__sizeof__", qp_bytes_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nSize of object in memory, in bytes." },
    { "fromhex", qp_bm_fromhex, QP_METH_O | QP_METH_CLASS, 0,
      "fromhex($type, string, /)\n--\n\nCreate a bytes object from a string of hexadecimal numbers.\n\n"
      "Spaces between two numbers are accepted.\nExample: bytes.fromhex('B9 01EF') -> b'\\\\xb9\\\\x01\\\\xef'." },
    { "hex", qp_bm_hex, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_HEX_DOC("Create a string of hexadecimal numbers from a bytes object.",
                 "b'\\xb9\\x01\\xef'") },
    QP_BM_SHARED_METHODS,
    { NULL, NULL, 0, 0, NULL },
};

static const QPTypeSpec qp_bytes_spec = {
    .name = "bytes",
    .doc = "bytes(iterable_of_ints) -> bytes\n"
           "bytes(string, encoding[, errors]) -> bytes\n"
           "bytes(bytes_or_buffer) -> immutable copy of bytes_or_buffer\n"
           "bytes(int) -> bytes object of size given by the parameter initialized with null bytes\n"
           "bytes() -> empty bytes object\n\n"
           "Construct an immutable array of bytes from:\n"
           "  - an iterable yielding integers in range(256)\n"
           "  - a text string encoded using the specified encoding\n"
           "  - any object implementing the buffer API.\n"
           "  - an integer",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_BYTES_SUBCLASS | QP_TPFLAGS_MATCH_SELF |
             QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_PRIV_VARSIZE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BYTES,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_bytes_tp_repr,
        .tp_str = qp_bytes_tp_repr,
        .tp_hash = qp_bytes_tp_hash,
        .tp_richcompare = qp_bytes_tp_richcompare,
        .tp_iter = qp_bytes_tp_iter,
        .tp_iteritem = qp_bytes_tp_iteritem,
        .tp_new = qp_bytes_tp_new,
        .nb = { .nb_remainder = qp_bm_mod },
        .sq = {
            .sq_length = qp_bytes_sq_length,
            .sq_concat = qp_bytes_sq_concat,
            .sq_repeat = qp_bytes_sq_repeat,
            .sq_item = qp_bytes_sq_item,
            .sq_contains = qp_bm_contains,
        },
        .mp = {
            .mp_length = qp_bytes_sq_length,
            .mp_subscript = qp_bytes_mp_subscript,
        },
        .bf = { .bf_getbuffer = qp_bytes_getbuffer },
    },
    .methods = qp_bytes_methods,
};

/* Init of unit g: bytes (with the empty-bytes singleton), bytearray and
   memoryview. */
static int qp_init_bytes(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSValue e = qp_bytes_new_type(ctx, NULL, NULL, 0);
    if (JS_IsException(e))
        return -1;
    st->empty_bytes = e;
    if (qp_type_ready(ctx, QP_TYPE(ctx, bytes), &qp_bytes_spec) < 0)
        return -1;
    if (qp_init_bytearray_type(ctx) < 0)
        return -1;
    if (qp_init_memoryview_type(ctx) < 0)
        return -1;
    return 0;
}
