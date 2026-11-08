/*
 * memoryview type and managed buffers.
 *
 * Derived from CPython 3.15 Objects/memoryobject.c (PSF-2.0).
 *
 * Every memoryview registers with a managed buffer object (QPManagedBuffer,
 * CPython _PyManagedBufferObject) that holds the single buffer export taken
 * from the underlying object; slices, casts, toreadonly() and
 * memoryview(memoryview) share it.  The export is released when the last
 * registered view is released (release(), the context manager, or the view
 * being freed).  Views keep their own buf/shape/strides/format like a
 * Py_buffer.  Formats are the native single-item struct formats (with an
 * optional '@'), as produced by every exporter of the runtime; there is no
 * struct module fallback in M2 (other formats raise NotImplementedError
 * "memoryview: unsupported format ..." like CPython does for non-native
 * formats it cannot unpack).
 *
 * JS typed arrays have type memoryview (qp_type_of): methods and slots
 * accept them as `self` through a temporary descriptor (qp_mv_get).
 */
#include "bytesobject_internal.h"

static BOOL qp_memoryview_check(JSValueConst v)
{
    return qp_bk_kind(v) == QP_BK_MEMORYVIEW;
}

static BOOL qp_mv_is_typed_array(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        qp_bm_is_typed_array_class(JS_VALUE_GET_OBJ(v)->class_id);
}

static void qp_mv_set_dims_inline(QPMemoryView *mv)
{
    mv->shape = &mv->dims1[0];
    mv->strides = &mv->dims1[1];
}

/* init_flags */
static void qp_mv_init_flags(QPMemoryView *mv)
{
    int flags = mv->flags & QP_MV_RELEASED;
    int i;
    Py_ssize_t sd;
    switch (mv->ndim) {
    case 0:
        flags |= QP_MV_SCALAR | QP_MV_C | QP_MV_FORTRAN;
        break;
    case 1:
        if (mv->shape[0] == 1 || mv->strides[0] == mv->itemsize)
            flags |= QP_MV_C | QP_MV_FORTRAN;
        break;
    default:
        if (mv->len == 0) {
            flags |= QP_MV_C | QP_MV_FORTRAN;
            break;
        }
        sd = mv->itemsize;
        for (i = mv->ndim - 1; i >= 0; i--) {
            if (mv->shape[i] > 1 && mv->strides[i] != sd)
                break;
            sd *= mv->shape[i];
        }
        if (i < 0)
            flags |= QP_MV_C;
        sd = mv->itemsize;
        for (i = 0; i < mv->ndim; i++) {
            if (mv->shape[i] > 1 && mv->strides[i] != sd)
                break;
            sd *= mv->shape[i];
        }
        if (i == mv->ndim)
            flags |= QP_MV_FORTRAN;
        break;
    }
    mv->flags = flags;
}

/* init_len */
static void qp_mv_init_len(QPMemoryView *mv)
{
    Py_ssize_t i, len = 1;
    for (i = 0; i < mv->ndim; i++)
        len *= mv->shape[i];
    mv->len = len * mv->itemsize;
}

static void qp_mv_free_format(JSRuntime *rt, QPMemoryView *mv)
{
    if (mv->format && mv->format != mv->format_inline)
        js_free_rt(rt, (char *)mv->format);
    mv->format = NULL;
}

/* Owned copy of fmt as the format of a heap memoryview.  -1 on OOM
   (exception set, previous format kept). */
static int qp_mv_set_format(JSContext *ctx, QPMemoryView *mv, const char *fmt)
{
    size_t n = strlen(fmt) + 1;
    char *p = mv->format_inline;
    if (n > sizeof(mv->format_inline) || fmt == mv->format_inline) {
        p = js_malloc(ctx, n);
        if (!p)
            return -1;
    }
    memcpy(p, fmt, n);
    qp_mv_free_format(ctx->rt, mv);
    mv->format = p;
    return 0;
}

/* Format of a stack descriptor: borrowed from the exporter, which outlives
   the descriptor. */
static void qp_mv_borrow_format(QPMemoryView *mv, const char *fmt)
{
    mv->format = fmt;
}

/* Fill a temporary descriptor for a JS typed array / DataView. */
static void qp_mv_from_typed_array(JSValueConst obj, QPMemoryView *tmp)
{
    const char *fmt;
    memset(tmp, 0, sizeof(*tmp));
    tmp->kind = QP_BK_MEMORYVIEW;
    tmp->mbuf = JS_UNDEFINED;
    tmp->hash = -1;
    qp_bm_js_buffer_info(JS_VALUE_GET_OBJ(obj), &tmp->buf, &tmp->len, &tmp->readonly,
                         &tmp->itemsize, &fmt);
    if (!tmp->buf)
        tmp->buf = (uint8_t *)qp_bytes_empty_data;
    qp_mv_borrow_format(tmp, fmt);
    tmp->ndim = 1;
    qp_mv_set_dims_inline(tmp);
    tmp->shape[0] = tmp->len / tmp->itemsize;
    tmp->strides[0] = tmp->itemsize;
    qp_mv_init_flags(tmp);
}

/* The view of `self` (a memoryview, or a typed array described in tmp). */
static QPMemoryView *qp_mv_get(JSContext *ctx, JSValueConst self, QPMemoryView *tmp)
{
    if (qp_memoryview_check(self))
        return QP_MV_OBJ(self);
    if (qp_mv_is_typed_array(self)) {
        qp_mv_from_typed_array(self, tmp);
        return tmp;
    }
    qp_raise_type_error(ctx, "descriptor requires a 'memoryview' object but received '%T'", self);
    return NULL;
}

static int qp_mv_check_released(JSContext *ctx, QPMemoryView *mv)
{
    if (mv->flags & QP_MV_RELEASED) {
        qp_raise_value_error(ctx, "operation forbidden on released memoryview object");
        return -1;
    }
    return 0;
}

#define QP_MV_GET(self, mv, tmp, fail)                  \
    do {                                                \
        (mv) = qp_mv_get(ctx, (self), &(tmp));          \
        if (!(mv) || qp_mv_check_released(ctx, (mv)) < 0) \
            return fail;                                \
    } while (0)

/* ------------------------------------------------------------------------ */
/* managed buffers and view creation                                         */
/* ------------------------------------------------------------------------ */

static void qp_mbuf_release(JSContext *ctx, QPManagedBuffer *mb)
{
    if (mb->released)
        return;
    mb->released = 1;
    qp_buffer_release(ctx, &mb->master);
}

static void qp_mbuf_finalize(JSRuntime *rt, QPManagedBuffer *mb)
{
    if (!mb->released) {
        mb->released = 1;
        qp_buffer_release_rt(rt, &mb->master);
    }
    js_free_rt(rt, mb);
}

static void qp_mbuf_mark(JSRuntime *rt, QPManagedBuffer *mb, JS_MarkFunc *mark_func)
{
    if (!mb->released)
        JS_MarkValue(rt, mb->master.obj, mark_func);
}

/* _PyManagedBuffer_FromObject */
static JSValue qp_mbuf_new(JSContext *ctx, JSValueConst base, int flags)
{
    QPManagedBuffer *mb;
    JSValue obj;
    mb = js_mallocz(ctx, sizeof(*mb));
    if (!mb)
        return JS_EXCEPTION;
    mb->kind = QP_BK_MBUF;
    mb->master.obj = JS_UNDEFINED;
    if (qp_buffer_get(ctx, base, &mb->master, flags) < 0) {
        js_free(ctx, mb);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_PY_BYTEARRAY);
    if (JS_IsException(obj)) {
        qp_buffer_release(ctx, &mb->master);
        js_free(ctx, mb);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = mb;
    return obj;
}

/* New managed buffer over raw memory (no exporter object; PyMemoryView_
   FromMemory, PyMemoryView_FromBuffer), which must outlive it. */
static JSValue qp_mbuf_raw(JSContext *ctx, void *buf, Py_ssize_t len, int readonly,
                           Py_ssize_t itemsize, const char *format)
{
    QPManagedBuffer *mb;
    JSValue obj;
    mb = js_mallocz(ctx, sizeof(*mb));
    if (!mb)
        return JS_EXCEPTION;
    mb->kind = QP_BK_MBUF;
    mb->master.obj = JS_UNDEFINED;
    mb->master.buf = buf;
    mb->master.len = len;
    mb->master.readonly = readonly;
    mb->master.itemsize = itemsize > 0 ? itemsize : 1;
    mb->master.format = format ? format : "B";
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_PY_BYTEARRAY);
    if (JS_IsException(obj)) {
        js_free(ctx, mb);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = mb;
    return obj;
}

/* New memoryview registered with mbuf (consumed), with room for ndim
   dimensions; the caller fills the view. */
static JSValue qp_mv_alloc(JSContext *ctx, JSValue mbuf, int ndim, QPMemoryView **pmv)
{
    QPMemoryView *mv;
    JSValue obj;
    mv = js_mallocz(ctx, sizeof(*mv));
    if (!mv) {
        JS_FreeValue(ctx, mbuf);
        return JS_EXCEPTION;
    }
    mv->kind = QP_BK_MEMORYVIEW;
    mv->hash = -1;
    mv->mbuf = JS_UNDEFINED;
    if (ndim > 1) {
        mv->shape = js_malloc(ctx, sizeof(Py_ssize_t) * 2 * ndim);
        if (!mv->shape) {
            js_free(ctx, mv);
            JS_FreeValue(ctx, mbuf);
            return JS_EXCEPTION;
        }
        mv->strides = mv->shape + ndim;
    } else {
        qp_mv_set_dims_inline(mv);
    }
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, memoryview)), JS_CLASS_PY_BYTEARRAY);
    if (JS_IsException(obj)) {
        if (mv->shape != &mv->dims1[0])
            js_free(ctx, mv->shape);
        js_free(ctx, mv);
        JS_FreeValue(ctx, mbuf);
        return obj;
    }
    mv->mbuf = mbuf;
    QP_MBUF_OBJ(mbuf)->exports++;
    JS_VALUE_GET_OBJ(obj)->u.opaque = mv;
    *pmv = mv;
    return obj;
}

#ifdef CONFIG_PYTHON_CAPI
/* python/capi_bytes.c: ndim, shape and strides (NULL: C-contiguous) of a
   buffer exported by a C extension (ndarrays, ctypes arrays and scalars,
   ...), -1 for other exporters */
static int qp_capi_buffer_shape(JSContext *ctx, const QPBuffer *view, const Py_ssize_t **pshape,
                                const Py_ssize_t **pstrides, Py_ssize_t *pitemsize);
static JSValue qp_capi_buffer_base(JSContext *ctx, const QPBuffer *view);
#endif

/* mbuf_add_view(mbuf, NULL): view of the whole master buffer */
static JSValue qp_mv_from_mbuf(JSContext *ctx, JSValue mbuf)
{
    QPMemoryView *mv;
    QPBuffer *m = &QP_MBUF_OBJ(mbuf)->master;
    const Py_ssize_t *cshape = NULL, *cstrides = NULL;
    int ndim = 1, i;
    Py_ssize_t sd, citemsize = 0;
    JSValue obj;
#ifdef CONFIG_PYTHON_CAPI
    i = qp_capi_buffer_shape(ctx, m, &cshape, &cstrides, &citemsize);
    if (i >= 0) {
        ndim = i;
    } else {
        cshape = NULL;
        cstrides = NULL;
    }
#endif
    obj = qp_mv_alloc(ctx, mbuf, ndim, &mv);
    if (JS_IsException(obj))
        return obj;
    m = &QP_MBUF_OBJ(mv->mbuf)->master;
    mv->buf = m->buf;
    mv->len = m->len;
    mv->itemsize = m->itemsize > 0 ? m->itemsize : 1;
    mv->readonly = m->readonly;
    if (qp_mv_set_format(ctx, mv, m->format ? m->format : "B") < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (cshape || ndim != 1) {
        /* the exporter's strides, or C-contiguous ones of its shape */
        mv->itemsize = citemsize;
        mv->ndim = ndim;
        sd = mv->itemsize;
        for (i = ndim - 1; i >= 0; i--) {
            mv->shape[i] = cshape[i];
            mv->strides[i] = cstrides ? cstrides[i] : sd;
            sd *= cshape[i];
        }
    } else {
        mv->ndim = 1;
        mv->shape[0] = mv->len / mv->itemsize;
        mv->strides[0] = mv->itemsize;
    }
    qp_mv_init_flags(mv);
    return obj;
}

/* Managed buffer of a view (new reference); typed-array descriptors get one
   made on demand. */
static JSValue qp_mv_mbuf_ref(JSContext *ctx, JSValueConst self, QPMemoryView *mv)
{
    if (!JS_IsUndefined(mv->mbuf))
        return JS_DupValue(ctx, mv->mbuf);
    return qp_mbuf_new(ctx, self, QP_BUF_SIMPLE);
}

/* mbuf_add_incomplete_view / mbuf_add_view: copy of src's view with room
   for ndim dimensions (ndim == src->ndim: complete copy). */
static JSValue qp_mv_copy_view(JSContext *ctx, JSValueConst self, QPMemoryView *src, int ndim,
                               QPMemoryView **pmv)
{
    QPMemoryView *mv;
    JSValue mbuf = qp_mv_mbuf_ref(ctx, self, src);
    JSValue obj;
    int i;
    if (JS_IsException(mbuf))
        return mbuf;
    obj = qp_mv_alloc(ctx, mbuf, ndim, &mv);
    if (JS_IsException(obj))
        return obj;
    mv->buf = src->buf;
    mv->len = src->len;
    mv->itemsize = src->itemsize;
    mv->readonly = src->readonly;
    if (qp_mv_set_format(ctx, mv, src->format) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    mv->ndim = ndim;
    if (ndim == src->ndim) {
        for (i = 0; i < ndim; i++) {
            mv->shape[i] = src->shape[i];
            mv->strides[i] = src->strides[i];
        }
    } else {
        /* incomplete view: 1-D placeholder over the whole buffer */
        mv->ndim = src->ndim < ndim ? src->ndim : ndim;
        for (i = 0; i < mv->ndim; i++) {
            mv->shape[i] = src->shape[i];
            mv->strides[i] = src->strides[i];
        }
        mv->ndim = src->ndim;
    }
    mv->flags = src->flags & ~QP_MV_RELEASED;
    *pmv = mv;
    return obj;
}

/* PyMemoryView_FromObject */
static JSValue qp_memoryview_from_object(JSContext *ctx, JSValueConst v, int flags)
{
    QPMemoryView *mv;
    JSValue mbuf;
    if (qp_memoryview_check(v)) {
        QPMemoryView *src = QP_MV_OBJ(v);
        if (qp_mv_check_released(ctx, src) < 0)
            return JS_EXCEPTION;
        if ((flags & QP_BUF_WRITABLE) && src->readonly) {
            qp_raise(ctx, QP_EXC(ctx, BufferError), "memoryview: underlying buffer is not writable");
            return JS_EXCEPTION;
        }
        return qp_mv_copy_view(ctx, v, src, src->ndim, &mv);
    }
    if (qp_buffer_check(ctx, v)) {
        mbuf = qp_mbuf_new(ctx, v, flags | QP_BUF_STRIDED);
        if (JS_IsException(mbuf))
            return mbuf;
        return qp_mv_from_mbuf(ctx, mbuf);
    }
    return qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                              "memoryview: a bytes-like object is required, not '%U'", v);
}

/* _memory_release */
static void qp_mv_release_view(JSContext *ctx, QPMemoryView *mv)
{
    QPManagedBuffer *mb;
    if (mv->flags & QP_MV_RELEASED)
        return;
    mv->flags |= QP_MV_RELEASED;
    if (JS_IsUndefined(mv->mbuf))
        return;
    mb = QP_MBUF_OBJ(mv->mbuf);
    if (mb && --mb->exports == 0)
        qp_mbuf_release(ctx, mb);
}

static void qp_memoryview_finalize(JSRuntime *rt, QPMemoryView *mv)
{
    if (!(mv->flags & QP_MV_RELEASED) && JS_VALUE_GET_TAG(mv->mbuf) == JS_TAG_OBJECT) {
        QPManagedBuffer *mb = QP_MBUF_OBJ(mv->mbuf);
        mv->flags |= QP_MV_RELEASED;
        if (mb && --mb->exports == 0 && !mb->released) {
            mb->released = 1;
            qp_buffer_release_rt(rt, &mb->master);
        }
    }
    JS_FreeValueRT(rt, mv->mbuf);
    if (mv->shape && mv->shape != &mv->dims1[0])
        js_free_rt(rt, mv->shape);
    qp_mv_free_format(rt, mv);
    js_free_rt(rt, mv);
}

static void qp_memoryview_mark(JSRuntime *rt, QPMemoryView *mv, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, mv->mbuf, mark_func);
}

/* ------------------------------------------------------------------------ */
/* buffer export of memoryviews and typed arrays                             */
/* ------------------------------------------------------------------------ */

static int qp_memoryview_getbuffer(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags)
{
    QPMemoryView tmp, *mv;
    mv = qp_mv_get(ctx, obj, &tmp);
    if (!mv || qp_mv_check_released(ctx, mv) < 0)
        return -1;
    if ((flags & QP_BUF_WRITABLE) && mv->readonly) {
        qp_raise(ctx, QP_EXC(ctx, BufferError), "memoryview: underlying buffer is not writable");
        return -1;
    }
    if (!(mv->flags & QP_MV_C)) {
        qp_raise(ctx, QP_EXC(ctx, BufferError), "memoryview: underlying buffer is not C-contiguous");
        return -1;
    }
    view->obj = JS_DupValue(ctx, obj);
    view->buf = mv->buf;
    view->len = mv->len;
    view->readonly = mv->readonly;
    view->itemsize = mv->itemsize;
    if (mv == &tmp) {
        /* static format strings of the typed-array table */
        Py_ssize_t isz;
        uint8_t *b;
        Py_ssize_t l;
        int ro;
        qp_bm_js_buffer_info(JS_VALUE_GET_OBJ(obj), &b, &l, &ro, &isz, &view->format);
    } else {
        view->format = mv->format;
        mv->exports++;
    }
    return 0;
}

static void qp_memoryview_releasebuffer(JSContext *ctx, QPBuffer *view)
{
    if (qp_memoryview_check(view->obj)) {
        QPMemoryView *mv = QP_MV_OBJ(view->obj);
        if (mv->exports > 0)
            mv->exports--;
    }
}

/* ------------------------------------------------------------------------ */
/* formats, pack / unpack                                                    */
/* ------------------------------------------------------------------------ */

#define QP_IS_BYTE_FORMAT(f) \
    (strcmp((f), "b") == 0 || strcmp((f), "B") == 0 || strcmp((f), "c") == 0)

/* get_native_fmtchar: item size of a native format ('@' optional), *result
   = the format without '@'; -1 if not native. */
static Py_ssize_t qp_mv_native_fmtchar(const char **result, const char *fmt)
{
    Py_ssize_t size = -1;
    if (fmt[0] == '@')
        fmt++;
    switch (fmt[0]) {
    case 'c': case 'b': case 'B': size = 1; break;
    case 'h': case 'H': size = sizeof(short); break;
    case 'i': case 'I': size = sizeof(int); break;
    case 'l': case 'L': size = sizeof(long); break;
    case 'q': case 'Q': size = sizeof(long long); break;
    case 'n': case 'N': size = sizeof(Py_ssize_t); break;
    case 'f': size = sizeof(float); break;
    case 'd': size = sizeof(double); break;
    case 'e': size = 2; break;
    case '?': size = 1; break;
    case 'P': size = sizeof(void *); break;
    case 'Z':
        switch (fmt[1]) {
        case 'f': size = 2 * sizeof(float); break;
        case 'd': size = 2 * sizeof(double); break;
        }
        if (size > 0 && fmt[2] == '\0') {
            *result = fmt;
            return size;
        }
        return -1;
    }
    if (size > 0 && fmt[1] == '\0') {
        *result = fmt;
        return size;
    }
    return -1;
}

/* adjust_fmt */
static const char *qp_mv_adjust_fmt(JSContext *ctx, QPMemoryView *mv)
{
    const char *fmt = mv->format[0] == '@' ? mv->format + 1 : mv->format;
    if (fmt[0] == 'Z' && fmt[1] && fmt[2] == '\0')
        return fmt;
    if (fmt[0] && fmt[1] == '\0')
        return fmt;
    qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "memoryview: unsupported format %s", mv->format);
    return NULL;
}

/* unpack_single */
static JSValue qp_mv_unpack(JSContext *ctx, QPMemoryView *mv, const uint8_t *ptr, const char *fmt)
{
    if (mv->flags & QP_MV_RELEASED)
        return qp_raise_value_error(ctx, "operation forbidden on released memoryview object");
    switch (fmt[0]) {
    case 'B': return JS_NewInt32(ctx, *ptr);
    case 'b': return JS_NewInt32(ctx, *(const int8_t *)ptr);
    case 'h': { short x; memcpy(&x, ptr, sizeof x); return JS_NewInt32(ctx, x); }
    case 'H': { unsigned short x; memcpy(&x, ptr, sizeof x); return JS_NewInt32(ctx, x); }
    case 'i': { int x; memcpy(&x, ptr, sizeof x); return JS_NewInt32(ctx, x); }
    case 'I': { unsigned int x; memcpy(&x, ptr, sizeof x); return qp_long_from_int64(ctx, x); }
    case 'l': { long x; memcpy(&x, ptr, sizeof x); return qp_long_from_int64(ctx, x); }
    case 'L': { unsigned long x; memcpy(&x, ptr, sizeof x); return qp_long_from_uint64(ctx, x); }
    case 'q': { long long x; memcpy(&x, ptr, sizeof x); return qp_long_from_int64(ctx, x); }
    case 'Q': { unsigned long long x; memcpy(&x, ptr, sizeof x); return qp_long_from_uint64(ctx, x); }
    case 'n': { Py_ssize_t x; memcpy(&x, ptr, sizeof x); return qp_long_from_int64(ctx, x); }
    case 'N': { size_t x; memcpy(&x, ptr, sizeof x); return qp_long_from_uint64(ctx, x); }
    case 'f': { float x; memcpy(&x, ptr, sizeof x); return qp_float_new(ctx, x); }
    case 'd': { double x; memcpy(&x, ptr, sizeof x); return qp_float_new(ctx, x); }
    case 'e': { uint16_t x; memcpy(&x, ptr, sizeof x); return qp_float_new(ctx, fromfp16(x)); }
    case '?': return JS_NewBool(ctx, *ptr != 0);
    case 'c': return qp_bytes_new(ctx, ptr, 1);
    case 'P': { void *p; memcpy(&p, ptr, sizeof p); return qp_long_from_uint64(ctx, (uintptr_t)p); }
    case 'Z':
        if (fmt[1] == 'f') {
            float x[2];
            memcpy(x, ptr, sizeof x);
            return qp_complex_new(ctx, x[0], x[1]);
        }
        if (fmt[1] == 'd') {
            double x[2];
            memcpy(x, ptr, sizeof x);
            return qp_complex_new(ctx, x[0], x[1]);
        }
        break;
    }
    return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "memoryview: format %s not supported", fmt);
}

static int qp_mv_type_error_int(JSContext *ctx, const char *fmt)
{
    qp_raise_type_error(ctx, "memoryview: invalid type for format '%s'", fmt);
    return -1;
}

static int qp_mv_value_error_int(JSContext *ctx, const char *fmt)
{
    qp_raise_value_error(ctx, "memoryview: invalid value for format '%s'", fmt);
    return -1;
}

/* fix_error_int */
static int qp_mv_fix_error_int(JSContext *ctx, const char *fmt)
{
    if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
        return qp_mv_type_error_int(ctx, fmt);
    if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, OverflowError)) ||
        qp_bm_exc_clear_if(ctx, QP_EXC(ctx, ValueError)))
        return qp_mv_value_error_int(ctx, fmt);
    return -1;
}

/* integer operand of pack_single: 0 ok (*v or *uv for values > INT64_MAX
   with *big set), -1 error (already mapped). */
static int qp_mv_pack_int(JSContext *ctx, JSValueConst item, const char *fmt, int64_t *v,
                          uint64_t *uv, int *big)
{
    JSValue iv = qp_index(ctx, item);
    int overflow;
    *big = 0;
    if (JS_IsException(iv))
        return qp_mv_fix_error_int(ctx, fmt);
    qp_long_as_int64_overflow(iv, v, &overflow);
    if (overflow) {
        if (qp_long_sign(iv) > 0) {
            JSValue max = qp_long_from_uint64(ctx, UINT64_MAX);
            int cmp;
            if (JS_IsException(max)) {
                JS_FreeValue(ctx, iv);
                return -1;
            }
            cmp = qp_long_compare(iv, max);
            JS_FreeValue(ctx, max);
            if (cmp <= 0 && JS_ToBigUint64(ctx, uv, iv) == 0) {
                JS_FreeValue(ctx, iv);
                *big = 1;
                return 0;
            }
        }
        JS_FreeValue(ctx, iv);
        return qp_mv_value_error_int(ctx, fmt);
    }
    JS_FreeValue(ctx, iv);
    return 0;
}

/* pack_single */
static int qp_mv_pack(JSContext *ctx, QPMemoryView *mv, uint8_t *ptr, JSValueConst item,
                      const char *fmt)
{
    int64_t ld;
    uint64_t lu;
    int big;

#define QP_MV_RELEASED_AGAIN()                                                \
    do {                                                                      \
        if (mv->flags & QP_MV_RELEASED) {                                     \
            qp_raise_value_error(ctx, "operation forbidden on released memoryview object"); \
            return -1;                                                        \
        }                                                                     \
    } while (0)

    switch (fmt[0]) {
    case 'b': case 'h': case 'i': case 'l': case 'q': case 'n':
        if (qp_mv_pack_int(ctx, item, fmt, &ld, &lu, &big) < 0)
            return -1;
        if (big)
            return qp_mv_value_error_int(ctx, fmt);
        QP_MV_RELEASED_AGAIN();
        switch (fmt[0]) {
        case 'b':
            if (ld < SCHAR_MIN || ld > SCHAR_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            *(int8_t *)ptr = (int8_t)ld;
            break;
        case 'h': {
            short x;
            if (ld < SHRT_MIN || ld > SHRT_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (short)ld;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'i': {
            int x;
            if (ld < INT_MIN || ld > INT_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (int)ld;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'l': {
            long x;
            if (ld < LONG_MIN || ld > LONG_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (long)ld;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'q': {
            long long x = ld;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        default: {
            Py_ssize_t x = (Py_ssize_t)ld;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        }
        break;
    case 'B': case 'H': case 'I': case 'L': case 'Q': case 'N': case 'P':
        if (qp_mv_pack_int(ctx, item, fmt, &ld, &lu, &big) < 0)
            return -1;
        if (!big) {
            if (ld < 0)
                return qp_mv_value_error_int(ctx, fmt);
            lu = (uint64_t)ld;
        }
        QP_MV_RELEASED_AGAIN();
        switch (fmt[0]) {
        case 'B':
            if (lu > UCHAR_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            *ptr = (uint8_t)lu;
            break;
        case 'H': {
            unsigned short x;
            if (lu > USHRT_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (unsigned short)lu;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'I': {
            unsigned int x;
            if (lu > UINT_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (unsigned int)lu;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'L': {
            unsigned long x;
            if (lu > ULONG_MAX)
                return qp_mv_value_error_int(ctx, fmt);
            x = (unsigned long)lu;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        case 'P': {
            void *x = (void *)(uintptr_t)lu;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        default: {
            uint64_t x = lu;
            memcpy(ptr, &x, sizeof x);
            break;
        }
        }
        break;
    case 'f': case 'd': case 'e': {
        double d;
        if (qp_float_as_double(ctx, item, &d) < 0)
            return qp_mv_fix_error_int(ctx, fmt);
        QP_MV_RELEASED_AGAIN();
        if (fmt[0] == 'f') {
            float x = (float)d;
            memcpy(ptr, &x, sizeof x);
        } else if (fmt[0] == 'd') {
            memcpy(ptr, &d, sizeof d);
        } else {
            uint16_t x = tofp16(d);
            /* PyFloat_Pack2: OverflowError "float too large to pack with e format" */
            if (isfinite(d) && !isfinite(fromfp16(x)))
                return qp_mv_value_error_int(ctx, fmt);
            memcpy(ptr, &x, sizeof x);
        }
        break;
    }
    case 'Z':
        if (fmt[1] == 'f' || fmt[1] == 'd') {
            QPComplex c;
            if (qp_complex_as_c(ctx, item, &c) < 0)
                return qp_mv_fix_error_int(ctx, fmt);
            QP_MV_RELEASED_AGAIN();
            if (fmt[1] == 'd') {
                double x[2] = { c.real, c.imag };
                memcpy(ptr, x, sizeof x);
            } else {
                float x[2] = { (float)c.real, (float)c.imag };
                memcpy(ptr, x, sizeof x);
            }
            break;
        }
        goto err_format;
    case '?': {
        int b = qp_bool(ctx, item);
        if (b < 0)
            return -1;
        QP_MV_RELEASED_AGAIN();
        *ptr = (uint8_t)(b != 0);
        break;
    }
    case 'c':
        if (!qp_bytes_check(ctx, item))
            return qp_mv_type_error_int(ctx, fmt);
        if (QP_BYTES_OBJ(item)->len != 1)
            return qp_mv_value_error_int(ctx, fmt);
        *ptr = QP_BYTES_OBJ(item)->data[0];
        break;
    default:
        goto err_format;
    }
    return 0;
err_format:
    qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "memoryview: format %s not supported", fmt);
    return -1;
#undef QP_MV_RELEASED_AGAIN
}

/* ------------------------------------------------------------------------ */
/* contiguous copies                                                         */
/* ------------------------------------------------------------------------ */

/* Copy every item of mv to dest in C (fortran = 0) or Fortran order. */
static int qp_mv_to_contiguous(JSContext *ctx, QPMemoryView *mv, uint8_t *dest, int fortran)
{
    Py_ssize_t *idx;
    Py_ssize_t total, n;
    int d, nd = mv->ndim;

    if (mv->len == 0)
        return 0;
    if (nd == 0 || ((mv->flags & QP_MV_C) && !fortran) || ((mv->flags & QP_MV_FORTRAN) && fortran)) {
        memcpy(dest, mv->buf, mv->len);
        return 0;
    }
    idx = js_mallocz(ctx, sizeof(Py_ssize_t) * nd);
    if (!idx)
        return -1;
    total = mv->len / mv->itemsize;
    for (n = 0; n < total; n++) {
        const uint8_t *p = mv->buf;
        for (d = 0; d < nd; d++)
            p += idx[d] * mv->strides[d];
        memcpy(dest, p, mv->itemsize);
        dest += mv->itemsize;
        if (fortran) {
            for (d = 0; d < nd; d++) {
                if (++idx[d] < mv->shape[d])
                    break;
                idx[d] = 0;
            }
        } else {
            for (d = nd - 1; d >= 0; d--) {
                if (++idx[d] < mv->shape[d])
                    break;
                idx[d] = 0;
            }
        }
    }
    js_free(ctx, idx);
    return 0;
}

/* bytes(mv) / bytearray(mv) for possibly non-contiguous views */
static JSValue qp_memoryview_to_bytes(JSContext *ctx, JSValueConst v, int bytearray)
{
    QPMemoryView tmp, *mv;
    JSValue r;
    QP_MV_GET(v, mv, tmp, JS_EXCEPTION);
    r = qp_bm_new(ctx, bytearray, NULL, mv->len);
    if (JS_IsException(r) || mv->len == 0)
        return r;
    if (qp_mv_to_contiguous(ctx, mv, QP_BYTES_OBJ(r)->data, 0) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* indexing                                                                  */
/* ------------------------------------------------------------------------ */

static uint8_t *qp_mv_lookup_dimension(JSContext *ctx, QPMemoryView *mv, uint8_t *ptr, int dim,
                                       Py_ssize_t index)
{
    Py_ssize_t nitems = mv->shape[dim];
    if (index < 0)
        index += nitems;
    if (index < 0 || index >= nitems) {
        qp_raise_index_error(ctx, "index out of bounds on dimension %d", dim + 1);
        return NULL;
    }
    return ptr + mv->strides[dim] * index;
}

static uint8_t *qp_mv_ptr_from_tuple(JSContext *ctx, QPMemoryView *mv, JSValueConst tup)
{
    uint8_t *ptr = mv->buf;
    Py_ssize_t dim, nindices = QP_TUPLE_SIZE(tup);
    if (nindices > mv->ndim) {
        qp_raise_type_error(ctx, "cannot index %d-dimension view with %zd-element tuple",
                            mv->ndim, nindices);
        return NULL;
    }
    for (dim = 0; dim < nindices; dim++) {
        Py_ssize_t index;
        if (qp_as_ssize(ctx, QP_TUPLE_ITEMS(tup)[dim], QP_EXC(ctx, IndexError), &index) < 0)
            return NULL;
        ptr = qp_mv_lookup_dimension(ctx, mv, ptr, (int)dim, index);
        if (!ptr)
            return NULL;
    }
    return ptr;
}

/* memory_item */
static JSValue qp_mv_item(JSContext *ctx, QPMemoryView *mv, Py_ssize_t index)
{
    const char *fmt;
    if (qp_mv_check_released(ctx, mv) < 0)
        return JS_EXCEPTION;
    fmt = qp_mv_adjust_fmt(ctx, mv);
    if (!fmt)
        return JS_EXCEPTION;
    if (mv->ndim == 0)
        return qp_raise_type_error(ctx, "invalid indexing of 0-dim memory");
    if (mv->ndim == 1) {
        uint8_t *ptr = qp_mv_lookup_dimension(ctx, mv, mv->buf, 0, index);
        if (!ptr)
            return JS_EXCEPTION;
        return qp_mv_unpack(ctx, mv, ptr, fmt);
    }
    return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "multi-dimensional sub-views are not implemented");
}

static JSValue qp_memoryview_sq_item(JSContext *ctx, JSValueConst self, Py_ssize_t index)
{
    QPMemoryView tmp, *mv = qp_mv_get(ctx, self, &tmp);
    if (!mv)
        return JS_EXCEPTION;
    return qp_mv_item(ctx, mv, index);
}

static BOOL qp_mv_is_multiindex(JSContext *ctx, JSValueConst key)
{
    Py_ssize_t i, n;
    if (!qp_tuple_check(ctx, key))
        return FALSE;
    n = QP_TUPLE_SIZE(key);
    for (i = 0; i < n; i++)
        if (!qp_index_check(ctx, QP_TUPLE_ITEMS(key)[i]))
            return FALSE;
    return TRUE;
}

static BOOL qp_mv_is_multislice(JSContext *ctx, JSValueConst key)
{
    Py_ssize_t i, n;
    if (!qp_tuple_check(ctx, key))
        return FALSE;
    n = QP_TUPLE_SIZE(key);
    for (i = 0; i < n; i++)
        if (!qp_slice_check(QP_TUPLE_ITEMS(key)[i]))
            return FALSE;
    return TRUE;
}

/* init_slice on dimension 0 */
static int qp_mv_init_slice(JSContext *ctx, QPMemoryView *mv, JSValueConst key)
{
    Py_ssize_t start, stop, step, slicelength;
    if (qp_slice_unpack(ctx, key, &start, &stop, &step) < 0)
        return -1;
    slicelength = qp_slice_adjust_indices(mv->shape[0], &start, &stop, step);
    mv->buf += mv->strides[0] * start;
    mv->shape[0] = slicelength;
    mv->strides[0] *= step;
    return 0;
}

static BOOL qp_mv_is_ellipsis(JSContext *ctx, JSValueConst key)
{
    return qp_is(key, QP_STATE(ctx)->ellipsis);
}

static JSValue qp_memoryview_mp_subscript(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    QPMemoryView tmp, *mv;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);

    if (mv->ndim == 0) {
        if (qp_tuple_check(ctx, key) && QP_TUPLE_SIZE(key) == 0) {
            const char *fmt = qp_mv_adjust_fmt(ctx, mv);
            if (!fmt)
                return JS_EXCEPTION;
            return qp_mv_unpack(ctx, mv, mv->buf, fmt);
        }
        if (qp_mv_is_ellipsis(ctx, key))
            return JS_DupValue(ctx, self);
        return qp_raise_type_error(ctx, "invalid indexing of 0-dim memory");
    }
    if (qp_index_check(ctx, key)) {
        Py_ssize_t index;
        if (qp_as_ssize(ctx, key, QP_EXC(ctx, IndexError), &index) < 0)
            return JS_EXCEPTION;
        return qp_mv_item(ctx, mv, index);
    }
    if (qp_slice_check(key)) {
        QPMemoryView *sliced;
        JSValue r = qp_mv_copy_view(ctx, self, mv, mv->ndim, &sliced);
        if (JS_IsException(r))
            return r;
        if (qp_mv_init_slice(ctx, sliced, key) < 0) {
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
        qp_mv_init_len(sliced);
        qp_mv_init_flags(sliced);
        return r;
    }
    if (qp_mv_is_multiindex(ctx, key)) {
        const char *fmt;
        uint8_t *ptr;
        fmt = qp_mv_adjust_fmt(ctx, mv);
        if (!fmt)
            return JS_EXCEPTION;
        if (QP_TUPLE_SIZE(key) < mv->ndim)
            return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "sub-views are not implemented");
        ptr = qp_mv_ptr_from_tuple(ctx, mv, key);
        if (!ptr)
            return JS_EXCEPTION;
        return qp_mv_unpack(ctx, mv, ptr, fmt);
    }
    if (qp_mv_is_multislice(ctx, key))
        return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "multi-dimensional slicing is not implemented");
    return qp_raise_type_error(ctx, "memoryview: invalid slice key");
}

/* Flat descriptor of a QPBuffer (1-D, C-contiguous). */
static void qp_mv_from_qpbuffer(const QPBuffer *b, QPMemoryView *d)
{
    memset(d, 0, sizeof(*d));
    d->kind = QP_BK_MEMORYVIEW;
    d->mbuf = JS_UNDEFINED;
    d->buf = b->buf;
    d->len = b->len;
    d->itemsize = b->itemsize > 0 ? b->itemsize : 1;
    d->readonly = b->readonly;
    qp_mv_borrow_format(d, b->format ? b->format : "B");
    d->ndim = 1;
    qp_mv_set_dims_inline(d);
    d->shape[0] = d->len / d->itemsize;
    d->strides[0] = d->itemsize;
    qp_mv_init_flags(d);
}

/* equiv_shape */
static int qp_mv_equiv_shape(const QPMemoryView *a, const QPMemoryView *b)
{
    int i;
    if (a->ndim != b->ndim)
        return 0;
    for (i = 0; i < a->ndim; i++) {
        if (a->shape[i] != b->shape[i])
            return 0;
        if (a->shape[i] == 0)
            break;
    }
    return 1;
}

/* copy_single: copy src items into the 1-D view dest (overlap-safe). */
static int qp_mv_copy_single(JSContext *ctx, QPMemoryView *dest, QPMemoryView *src)
{
    const char *dfmt = dest->format[0] == '@' ? dest->format + 1 : dest->format;
    const char *sfmt = src->format[0] == '@' ? src->format + 1 : src->format;
    uint8_t *tmp;
    Py_ssize_t i;
    if (strcmp(dfmt, sfmt) != 0 || dest->itemsize != src->itemsize ||
        !qp_mv_equiv_shape(dest, src)) {
        qp_raise_value_error(ctx, "memoryview assignment: lvalue and rvalue have different structures");
        return -1;
    }
    if (src->len == 0)
        return 0;
    tmp = js_malloc(ctx, src->len);
    if (!tmp)
        return -1;
    if (qp_mv_to_contiguous(ctx, src, tmp, 0) < 0) {
        js_free(ctx, tmp);
        return -1;
    }
    for (i = 0; i < dest->shape[0]; i++)
        memcpy(dest->buf + i * dest->strides[0], tmp + i * dest->itemsize, dest->itemsize);
    js_free(ctx, tmp);
    return 0;
}

static int qp_memoryview_mp_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst key,
                                          JSValueConst value)
{
    QPMemoryView tmp, *mv;
    const char *fmt;
    uint8_t *ptr;

    QP_MV_GET(self, mv, tmp, -1);
    fmt = qp_mv_adjust_fmt(ctx, mv);
    if (!fmt)
        return -1;
    if (mv->readonly) {
        qp_raise_type_error(ctx, "cannot modify read-only memory");
        return -1;
    }
    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "cannot delete memory");
        return -1;
    }
    if (mv->ndim == 0) {
        if (qp_mv_is_ellipsis(ctx, key) || (qp_tuple_check(ctx, key) && QP_TUPLE_SIZE(key) == 0))
            return qp_mv_pack(ctx, mv, mv->buf, value, fmt);
        qp_raise_type_error(ctx, "invalid indexing of 0-dim memory");
        return -1;
    }
    if (qp_index_check(ctx, key)) {
        Py_ssize_t index;
        if (mv->ndim > 1) {
            qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "sub-views are not implemented");
            return -1;
        }
        if (qp_as_ssize(ctx, key, QP_EXC(ctx, IndexError), &index) < 0)
            return -1;
        ptr = qp_mv_lookup_dimension(ctx, mv, mv->buf, 0, index);
        if (!ptr)
            return -1;
        return qp_mv_pack(ctx, mv, ptr, value, fmt);
    }
    if (qp_slice_check(key) && mv->ndim == 1) {
        QPMemoryView dest, stmp, *src;
        QPBuffer sbuf;
        int ret;
        sbuf.obj = JS_UNDEFINED;
        if (qp_memoryview_check(value) || qp_mv_is_typed_array(value)) {
            src = qp_mv_get(ctx, value, &stmp);
            if (!src || qp_mv_check_released(ctx, src) < 0)
                return -1;
        } else {
            if (qp_buffer_get(ctx, value, &sbuf, QP_BUF_SIMPLE) < 0)
                return -1;
            qp_mv_from_qpbuffer(&sbuf, &stmp);
            src = &stmp;
        }
        dest = *mv;
        qp_mv_set_dims_inline(&dest);
        dest.shape[0] = mv->shape[0];
        dest.strides[0] = mv->strides[0];
        ret = qp_mv_init_slice(ctx, &dest, key);
        if (ret == 0) {
            dest.len = dest.shape[0] * dest.itemsize;
            ret = qp_mv_copy_single(ctx, &dest, src);
        }
        qp_buffer_release(ctx, &sbuf);
        return ret;
    }
    if (qp_mv_is_multiindex(ctx, key)) {
        if (QP_TUPLE_SIZE(key) < mv->ndim) {
            qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "sub-views are not implemented");
            return -1;
        }
        ptr = qp_mv_ptr_from_tuple(ctx, mv, key);
        if (!ptr)
            return -1;
        return qp_mv_pack(ctx, mv, ptr, value, fmt);
    }
    if (qp_slice_check(key) || qp_mv_is_multislice(ctx, key)) {
        qp_raise(ctx, QP_EXC(ctx, NotImplementedError),
                 "memoryview slice assignments are currently restricted to ndim = 1");
        return -1;
    }
    qp_raise_type_error(ctx, "memoryview: invalid slice key");
    return -1;
}

static Py_ssize_t qp_memoryview_sq_length(JSContext *ctx, JSValueConst self)
{
    QPMemoryView tmp, *mv;
    QP_MV_GET(self, mv, tmp, -1);
    if (mv->ndim == 0) {
        qp_raise_type_error(ctx, "0-dim memory has no length");
        return -1;
    }
    return mv->shape[0];
}

/* ------------------------------------------------------------------------ */
/* comparison, hash, repr, iteration                                         */
/* ------------------------------------------------------------------------ */

/* unpack_cmp for identical native formats: 1 equal, 0 not */
static int qp_mv_unpack_cmp(const uint8_t *p, const uint8_t *q, const char *fmt, Py_ssize_t itemsize)
{
    switch (fmt[0]) {
    case 'f': { float x, y; memcpy(&x, p, sizeof x); memcpy(&y, q, sizeof y); return x == y; }
    case 'd': { double x, y; memcpy(&x, p, sizeof x); memcpy(&y, q, sizeof y); return x == y; }
    case 'e': { uint16_t x, y; memcpy(&x, p, 2); memcpy(&y, q, 2); return fromfp16(x) == fromfp16(y); }
    case 'Z':
        if (fmt[1] == 'f') {
            float x[2], y[2];
            memcpy(x, p, sizeof x);
            memcpy(y, q, sizeof y);
            return x[0] == y[0] && x[1] == y[1];
        } else {
            double x[2], y[2];
            memcpy(x, p, sizeof x);
            memcpy(y, q, sizeof y);
            return x[0] == y[0] && x[1] == y[1];
        }
    case '?': return (*p != 0) == (*q != 0);
    default: return memcmp(p, q, itemsize) == 0;
    }
}

/* Compare item by item; fmt NULL = unpack both sides to objects ("_"). */
static int qp_mv_cmp_items(JSContext *ctx, QPMemoryView *v, QPMemoryView *w, const char *fmt)
{
    Py_ssize_t total, n, *idx = NULL;
    int d, nd = v->ndim, equal = 1;
    const char *vfmt = NULL, *wfmt = NULL;

    if (!fmt) {
        /* struct-style unpacking: native formats only in this runtime */
        if (qp_mv_native_fmtchar(&vfmt, v->format) < 0 || qp_mv_native_fmtchar(&wfmt, w->format) < 0)
            return 0; /* struct.error -> not equal */
    }
    if (nd == 0) {
        total = 1;
    } else {
        total = 1;
        for (d = 0; d < nd; d++)
            total *= v->shape[d];
        idx = js_mallocz(ctx, sizeof(Py_ssize_t) * nd);
        if (!idx)
            return -1;
    }
    for (n = 0; n < total && equal == 1; n++) {
        const uint8_t *p = v->buf, *q = w->buf;
        for (d = 0; d < nd; d++) {
            p += idx[d] * v->strides[d];
            q += idx[d] * w->strides[d];
        }
        if (fmt) {
            equal = qp_mv_unpack_cmp(p, q, fmt, v->itemsize);
        } else {
            JSValue a = qp_mv_unpack(ctx, v, p, vfmt), b;
            if (JS_IsException(a)) {
                equal = -1;
                break;
            }
            b = qp_mv_unpack(ctx, w, q, wfmt);
            if (JS_IsException(b)) {
                JS_FreeValue(ctx, a);
                equal = -1;
                break;
            }
            equal = qp_richcompare_bool(ctx, a, b, QP_CMP_EQ);
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
        }
        for (d = nd - 1; d >= 0; d--) {
            if (++idx[d] < v->shape[d])
                break;
            idx[d] = 0;
        }
    }
    if (idx)
        js_free(ctx, idx);
    return equal;
}

static JSValue qp_memoryview_tp_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPMemoryView vtmp, wtmp, *vv, *ww;
    QPBuffer wbuf;
    const char *vfmt, *wfmt;
    int equal;

    if (op != QP_CMP_EQ && op != QP_CMP_NE)
        return qp_not_implemented(ctx);
    vv = qp_mv_get(ctx, v, &vtmp);
    if (!vv)
        return JS_EXCEPTION;
    wbuf.obj = JS_UNDEFINED;
    if (vv->flags & QP_MV_RELEASED) {
        equal = qp_is(v, w);
        goto result;
    }
    if (qp_is(v, w)) {
        const char *f = vv->format[0] == '@' ? vv->format + 1 : vv->format;
        if (f[0] != 0 && strchr("bBchHiIlLnNqQ?", f[0]) != NULL && f[1] == 0) {
            equal = 1;
            goto result;
        }
    }
    if (qp_memoryview_check(w) || qp_mv_is_typed_array(w)) {
        ww = qp_mv_get(ctx, w, &wtmp);
        if (!ww)
            return JS_EXCEPTION;
        if (ww->flags & QP_MV_RELEASED) {
            equal = qp_is(v, w);
            goto result;
        }
    } else {
        if (qp_buffer_get(ctx, w, &wbuf, QP_BUF_SIMPLE) < 0) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return qp_not_implemented(ctx);
        }
        qp_mv_from_qpbuffer(&wbuf, &wtmp);
        ww = &wtmp;
    }
    if (!qp_mv_equiv_shape(vv, ww)) {
        equal = 0;
        goto result;
    }
    if (qp_mv_native_fmtchar(&vfmt, vv->format) < 0)
        vfmt = "_";
    if (qp_mv_native_fmtchar(&wfmt, ww->format) < 0)
        wfmt = "_";
    if (strcmp(vfmt, "_") == 0 || strcmp(wfmt, "_") == 0 || strcmp(vfmt, wfmt) != 0)
        equal = qp_mv_cmp_items(ctx, vv, ww, NULL);
    else
        equal = qp_mv_cmp_items(ctx, vv, ww, vfmt);
result:
    qp_buffer_release(ctx, &wbuf);
    if (equal < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, (equal && op == QP_CMP_EQ) || (!equal && op == QP_CMP_NE));
}

static Py_hash_t qp_memoryview_tp_hash(JSContext *ctx, JSValueConst self)
{
    QPMemoryView tmp, *mv;
    const char *fmt;
    QP_MV_GET(self, mv, tmp, -1);
    if (mv->hash == -1) {
        uint8_t *mem = mv->buf;
        Py_hash_t h;
        if (!mv->readonly) {
            qp_raise_value_error(ctx, "cannot hash writable memoryview object");
            return -1;
        }
        if (qp_mv_native_fmtchar(&fmt, mv->format) < 0 || !QP_IS_BYTE_FORMAT(fmt)) {
            qp_raise_value_error(ctx, "memoryview: hashing is restricted to formats 'B', 'b' or 'c'");
            return -1;
        }
        if (!JS_IsUndefined(mv->mbuf)) {
            QPManagedBuffer *mb = QP_MBUF_OBJ(mv->mbuf);
            JSValue obj = JS_DupValue(ctx, mb->master.obj);
            mv->exports++;
            h = qp_hash(ctx, obj);
            mv->exports--;
            JS_FreeValue(ctx, obj);
            if (h == -1)
                return -1;
        }
        if (!(mv->flags & QP_MV_C)) {
            mem = js_malloc(ctx, mv->len ? mv->len : 1);
            if (!mem)
                return -1;
            if (qp_mv_to_contiguous(ctx, mv, mem, 0) < 0) {
                js_free(ctx, mem);
                return -1;
            }
        }
        mv->hash = qp_hash_buffer(mem, mv->len, QP_STATE(ctx)->hash_key);
        if (mem != mv->buf)
            js_free(ctx, mem);
    }
    return mv->hash;
}

static JSValue qp_memoryview_tp_repr(JSContext *ctx, JSValueConst self)
{
    QPMemoryView tmp, *mv = qp_mv_get(ctx, self, &tmp);
    if (!mv)
        return JS_EXCEPTION;
    if (mv->flags & QP_MV_RELEASED)
        return qp_str_from_format(ctx, "<released memory at %p>", (void *)JS_VALUE_GET_PTR(self));
    return qp_str_from_format(ctx, "<memory at %p>", (void *)JS_VALUE_GET_PTR(self));
}

static JSValue qp_memoryview_tp_iter(JSContext *ctx, JSValueConst self)
{
    QPMemoryView tmp, *mv;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    if (mv->ndim == 0)
        return qp_raise_type_error(ctx, "invalid indexing of 0-dim memory");
    if (mv->ndim != 1)
        return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "multi-dimensional sub-views are not implemented");
    if (!qp_mv_adjust_fmt(ctx, mv))
        return JS_EXCEPTION;
    return qp_seqiter_new(ctx, self);
}

static JSValue qp_memoryview_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "object", NULL };
    static const QPArgSpec spec = { "memoryview", kw, 1, 1, 0 };
    JSValueConst a[1];
    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    return qp_memoryview_from_object(ctx, a[0], QP_BUF_SIMPLE);
}

/* ------------------------------------------------------------------------ */
/* methods                                                                   */
/* ------------------------------------------------------------------------ */

static JSValue qp_mv_release_meth(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                  JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv = qp_mv_get(ctx, self, &tmp);
    if (!mv)
        return JS_EXCEPTION;
    if (mv == &tmp)
        return JS_UNDEFINED;
    if (mv->exports == 0) {
        qp_mv_release_view(ctx, mv);
        return JS_UNDEFINED;
    }
    if (mv->exports > 0)
        return qp_raise(ctx, QP_EXC(ctx, BufferError), "memoryview has %zd exported buffer%s",
                        mv->exports, mv->exports == 1 ? "" : "s");
    return qp_raise_system_error(ctx, "memoryview: negative export count");
}

static JSValue qp_mv_enter(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    return JS_DupValue(ctx, self);
}

static JSValue qp_mv_exit(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                          JSValueConst kwnames, int magic)
{
    return qp_mv_release_meth(ctx, self, NULL, 0, QP_NULL, 0);
}

/* cast(format, shape=<unrepresentable>) */
static JSValue qp_mv_cast(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                          JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "format", "shape", NULL };
    static const QPArgSpec spec = { "cast", kw, 1, 2, 0 };
    JSValueConst a[2];
    QPMemoryView tmp, *self_mv, *mv;
    Py_ssize_t ndim = 1, itemsize, i;
    JSValueConst shape;
    JSValue r, ascii, fmtstr, shapeseq = JS_UNDEFINED;
    const char *destfmt, *srcfmt;
    char fmtbuf[QP_MV_FMT_INLINE];  /* native formats are at most "@x" */
    Py_ssize_t fmtlen;
    const uint8_t *fd;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_bm_bad_argument(ctx, "cast", "argument 'format'", "str", a[0]);
    shape = a[1];
    QP_MV_GET(self, self_mv, tmp, JS_EXCEPTION);
    if (!(self_mv->flags & QP_MV_C))
        return qp_raise_type_error(ctx, "memoryview: casts are restricted to C-contiguous views");
    if (!QP_IS_NULL(shape) || self_mv->ndim != 1) {
        for (i = 0; i < self_mv->ndim; i++) {
            if (self_mv->shape[i] == 0)
                return qp_raise_type_error(ctx, "memoryview: cannot cast view with zeros in shape or strides");
        }
    }
    if (!QP_IS_NULL(shape)) {
        if (!qp_list_check(ctx, shape) && !qp_tuple_check(ctx, shape))
            return qp_raise_type_error(ctx, "shape must be a list or a tuple");
        shapeseq = qp_tuple_from_iterable(ctx, shape);
        if (JS_IsException(shapeseq))
            return shapeseq;
        ndim = QP_TUPLE_SIZE(shapeseq);
        if (ndim > QP_MV_MAX_NDIM) {
            JS_FreeValue(ctx, shapeseq);
            return qp_raise_value_error(ctx, "memoryview: number of dimensions must not exceed 64");
        }
        if (self_mv->ndim != 1 && ndim != 1) {
            JS_FreeValue(ctx, shapeseq);
            return qp_raise_type_error(ctx, "memoryview: cast must be 1D -> ND or ND -> 1D");
        }
    }

    /* format as ASCII (UnicodeEncodeError otherwise) */
    fmtstr = qp_str_value(ctx, a[0]);
    if (JS_IsException(fmtstr)) {
        JS_FreeValue(ctx, shapeseq);
        return fmtstr;
    }
    ascii = qp_str_encode(ctx, fmtstr, "ascii", "strict");
    JS_FreeValue(ctx, fmtstr);
    if (JS_IsException(ascii)) {
        JS_FreeValue(ctx, shapeseq);
        return ascii;
    }
    fd = qp_bytes_data(ascii, &fmtlen);
    if (fmtlen >= (Py_ssize_t)sizeof(fmtbuf) || memchr(fd, 0, fmtlen))
        fmtlen = -1;
    else {
        memcpy(fmtbuf, fd, fmtlen);
        fmtbuf[fmtlen] = '\0';
    }
    JS_FreeValue(ctx, ascii);

    r = qp_mv_copy_view(ctx, self, self_mv, ndim == 0 ? 1 : (int)ndim, &mv);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, shapeseq);
        return r;
    }
    /* cast_to_1D */
    itemsize = fmtlen < 0 ? -1 : qp_mv_native_fmtchar(&destfmt, fmtbuf);
    if (itemsize < 0) {
        qp_raise_value_error(ctx, "memoryview: destination format must be a native format prefixed with an optional '@'");
        goto error;
    }
    if ((qp_mv_native_fmtchar(&srcfmt, mv->format) < 0 || !QP_IS_BYTE_FORMAT(srcfmt)) &&
        !QP_IS_BYTE_FORMAT(destfmt)) {
        qp_raise_type_error(ctx, "memoryview: cannot cast between two non-byte formats");
        goto error;
    }
    if (mv->len % itemsize) {
        qp_raise_type_error(ctx, "memoryview: length is not a multiple of itemsize");
        goto error;
    }
    if (qp_mv_set_format(ctx, mv, fmtbuf) < 0)
        goto error;
    mv->itemsize = itemsize;
    mv->ndim = 1;
    mv->shape[0] = mv->len / mv->itemsize;
    mv->strides[0] = mv->itemsize;
    qp_mv_init_flags(mv);

    if (!JS_IsUndefined(shapeseq)) {
        /* cast_to_ND */
        Py_ssize_t len;
        mv->ndim = (int)ndim;
        if (ndim == 0) {
            len = mv->itemsize;
        } else {
            len = mv->itemsize;
            for (i = 0; i < ndim; i++) {
                JSValueConst t = QP_TUPLE_ITEMS(shapeseq)[i];
                JSValue iv;
                Py_ssize_t x;
                int rc;
                if (!qp_long_check(ctx, t)) {
                    qp_raise_type_error(ctx, "memoryview.cast(): elements of shape must be integers");
                    goto error;
                }
                iv = qp_long_value(ctx, t);
                if (JS_IsException(iv))
                    goto error;
                rc = qp_long_as_ssize(ctx, iv, &x);
                JS_FreeValue(ctx, iv);
                if (rc < 0)
                    goto error;
                if (x <= 0) {
                    qp_raise_value_error(ctx, "memoryview.cast(): elements of shape must be integers > 0");
                    goto error;
                }
                if (x > PY_SSIZE_T_MAX / len) {
                    qp_raise_value_error(ctx, "memoryview.cast(): product(shape) > SSIZE_MAX");
                    goto error;
                }
                len *= x;
                mv->shape[i] = x;
            }
            /* init_strides_from_shape (C order) */
            mv->strides[ndim - 1] = mv->itemsize;
            for (i = ndim - 2; i >= 0; i--)
                mv->strides[i] = mv->strides[i + 1] * mv->shape[i + 1];
        }
        if (mv->len != len) {
            qp_raise_type_error(ctx, "memoryview: product(shape) * itemsize != buffer size");
            goto error;
        }
        qp_mv_init_flags(mv);
        JS_FreeValue(ctx, shapeseq);
    }
    return r;
error:
    JS_FreeValue(ctx, shapeseq);
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

static JSValue qp_mv_toreadonly(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *src, *mv;
    JSValue r;
    QP_MV_GET(self, src, tmp, JS_EXCEPTION);
    r = qp_mv_copy_view(ctx, self, src, src->ndim, &mv);
    if (JS_IsException(r))
        return r;
    mv->readonly = 1;
    return r;
}

/* tolist_rec over dimensions [dim, ndim) starting at ptr */
static JSValue qp_mv_tolist_rec(JSContext *ctx, QPMemoryView *mv, const uint8_t *ptr, int dim,
                                const char *fmt)
{
    Py_ssize_t i;
    JSValue lst = qp_list_new(ctx, 0);
    if (JS_IsException(lst))
        return lst;
    for (i = 0; i < mv->shape[dim]; i++) {
        const uint8_t *p = ptr + i * mv->strides[dim];
        JSValue item;
        int rc;
        if (dim == mv->ndim - 1)
            item = qp_mv_unpack(ctx, mv, p, fmt);
        else
            item = qp_mv_tolist_rec(ctx, mv, p, dim + 1, fmt);
        if (JS_IsException(item))
            goto fail;
        rc = qp_list_append(ctx, lst, item);
        JS_FreeValue(ctx, item);
        if (rc < 0)
            goto fail;
    }
    return lst;
fail:
    JS_FreeValue(ctx, lst);
    return JS_EXCEPTION;
}

static JSValue qp_mv_tolist(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                            JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv;
    const char *fmt;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    fmt = qp_mv_adjust_fmt(ctx, mv);
    if (!fmt)
        return JS_EXCEPTION;
    if (mv->ndim == 0)
        return qp_mv_unpack(ctx, mv, mv->buf, fmt);
    return qp_mv_tolist_rec(ctx, mv, mv->buf, 0, fmt);
}

/* tobytes(order='C') */
static JSValue qp_mv_tobytes(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                             JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "order", NULL };
    static const QPArgSpec spec = { "tobytes", kw, 0, 1, 0 };
    JSValueConst a[1];
    QPMemoryView tmp, *mv;
    int fortran = 0;
    JSValue r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && !JS_IsUndefined(a[0]) && !JS_IsNull(a[0]) && !qp_str_check(ctx, a[0]))
        return qp_bm_bad_argument(ctx, "tobytes", "argument 'order'", "str or None", a[0]);
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    if (!QP_IS_NULL(a[0]) && !JS_IsUndefined(a[0]) && !JS_IsNull(a[0])) {
        const char *order = qp_bm_arg_cstr(ctx, "tobytes", "order", a[0]);
        if (!order)
            return JS_EXCEPTION;
        if (strcmp(order, "F") == 0) {
            fortran = 1;
        } else if (strcmp(order, "A") == 0) {
            fortran = (mv->flags & QP_MV_FORTRAN) && !(mv->flags & QP_MV_C);
        } else if (strcmp(order, "C") != 0) {
            JS_FreeCString(ctx, order);
            return qp_raise_value_error(ctx, "order must be 'C', 'F' or 'A'");
        }
        JS_FreeCString(ctx, order);
    }
    r = qp_bytes_new(ctx, NULL, mv->len);
    if (JS_IsException(r) || mv->len == 0)
        return r;
    if (qp_mv_to_contiguous(ctx, mv, QP_BYTES_OBJ(r)->data, fortran) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

static JSValue qp_mv_hex(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                         JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv;
    JSValueConst sep;
    Py_ssize_t bps;
    int sep_char;
    JSValue r;
    uint8_t *mem;

    if (qp_bm_parse_hex_args(ctx, "hex", args, nargs, kwnames, &sep, &bps) < 0)
        return JS_EXCEPTION;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    if (mv != &tmp)
        mv->exports++;
    if (qp_bm_hex_sep(ctx, sep, &sep_char) < 0) {
        if (mv != &tmp)
            mv->exports--;
        return JS_EXCEPTION;
    }
    if (mv != &tmp)
        mv->exports--;
    if (mv->flags & QP_MV_C)
        return qp_bm_strhex_build(ctx, mv->buf, mv->len, sep_char, bps);
    mem = js_malloc(ctx, mv->len ? mv->len : 1);
    if (!mem)
        return JS_EXCEPTION;
    if (qp_mv_to_contiguous(ctx, mv, mem, 0) < 0) {
        js_free(ctx, mem);
        return JS_EXCEPTION;
    }
    r = qp_bm_strhex_build(ctx, mem, mv->len, sep_char, bps);
    js_free(ctx, mem);
    return r;
}

/* count(value, /) */
static JSValue qp_mv_count(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv;
    JSValue it;
    Py_ssize_t count = 0, i, n;
    it = qp_memoryview_tp_iter(ctx, self);
    if (JS_IsException(it))
        return it;
    JS_FreeValue(ctx, it);
    mv = qp_mv_get(ctx, self, &tmp);
    if (!mv)
        return JS_EXCEPTION;
    n = mv->shape[0];
    for (i = 0; i < n; i++) {
        JSValue item = qp_mv_item(ctx, mv, i);
        int eq;
        if (JS_IsException(item))
            return item;
        eq = qp_richcompare_bool(ctx, item, args[0], QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (eq < 0)
            return JS_EXCEPTION;
        count += eq;
        if (mv != &tmp && (mv->flags & QP_MV_RELEASED))
            return qp_raise_value_error(ctx, "operation forbidden on released memoryview object");
    }
    return qp_long_from_int64(ctx, count);
}

/* index(value, start=0, stop=sys.maxsize, /) */
static JSValue qp_mv_index(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                           JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv;
    Py_ssize_t start = 0, stop = PY_SSIZE_T_MAX, n, index;
    if (qp_arg_check_positional(ctx, "index", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (nargs > 1 && qp_arg_ssize(ctx, args[1], &start) < 0)
        return JS_EXCEPTION;
    if (nargs > 2 && qp_arg_ssize(ctx, args[2], &stop) < 0)
        return JS_EXCEPTION;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    if (mv->ndim == 0)
        return qp_raise_type_error(ctx, "invalid lookup on 0-dim memory");
    if (mv->ndim != 1)
        return qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "multi-dimensional lookup is not implemented");
    n = mv->shape[0];
    if (start < 0)
        start = start + n > 0 ? start + n : 0;
    if (stop < 0)
        stop = stop + n > 0 ? stop + n : 0;
    if (stop > n)
        stop = n;
    if (start > stop)
        start = stop;
    for (index = start; index < stop; index++) {
        JSValue item = qp_mv_item(ctx, mv, index);
        int eq;
        if (JS_IsException(item))
            return item;
        eq = qp_richcompare_bool(ctx, item, args[0], QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (eq > 0)
            return qp_long_from_int64(ctx, index);
        if (eq < 0)
            return JS_EXCEPTION;
    }
    return qp_raise_value_error(ctx, "memoryview.index(x): x not found");
}

/* memoryview._from_flags(object, flags) (classmethod) */
static JSValue qp_mv_from_flags(JSContext *ctx, JSValueConst type, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "object", "flags", NULL };
    static const QPArgSpec spec = { "_from_flags", kw, 2, 2, 0 };
    JSValueConst a[2];
    int flags;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_arg_int(ctx, a[1], &flags) < 0)
        return JS_EXCEPTION;
    return qp_memoryview_from_object(ctx, a[0], flags & QP_BUF_WRITABLE);
}

static JSValue qp_mv_class_getitem(JSContext *ctx, JSValueConst type, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, type, args[0]);
}

static JSValue qp_mv_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                            JSValueConst kwnames, int magic)
{
    QPMemoryView tmp, *mv = qp_mv_get(ctx, self, &tmp);
    if (!mv)
        return JS_EXCEPTION;
    /* sizeof(PyMemoryViewObject) minus ob_array + 3 * ndim entries */
    return qp_long_from_int64(ctx, 200 + (int64_t)mv->ndim * 3 * (int64_t)sizeof(Py_ssize_t));
}

/* ------------------------------------------------------------------------ */
/* getters                                                                   */
/* ------------------------------------------------------------------------ */

static JSValue qp_mv_ssize_tuple(JSContext *ctx, int n, const Py_ssize_t *v)
{
    JSValue items[QP_MV_MAX_NDIM];
    int i;
    if (n == 0 || !v)
        return JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    for (i = 0; i < n; i++)
        items[i] = qp_long_from_int64(ctx, v[i]);
    return qp_tuple_new_steal(ctx, n, items);
}

/* magic: 0 obj, 1 nbytes, 2 readonly, 3 itemsize, 4 format, 5 ndim,
   6 shape, 7 strides, 8 suboffsets, 9 c_contiguous, 10 f_contiguous,
   11 contiguous */
static JSValue qp_mv_get_attr(JSContext *ctx, JSValueConst self, int magic)
{
    QPMemoryView tmp, *mv;
    QP_MV_GET(self, mv, tmp, JS_EXCEPTION);
    switch (magic) {
    case 0:
        if (mv == &tmp)
            return JS_DupValue(ctx, self);
#ifdef CONFIG_PYTHON_CAPI
        {
            JSValue base = qp_capi_buffer_base(ctx, &QP_MBUF_OBJ(mv->mbuf)->master);
            if (!JS_IsUndefined(base))
                return base;
        }
#endif
        return JS_DupValue(ctx, QP_MBUF_OBJ(mv->mbuf)->master.obj);
    case 1: return qp_long_from_int64(ctx, mv->len);
    case 2: return JS_NewBool(ctx, mv->readonly);
    case 3: return qp_long_from_int64(ctx, mv->itemsize);
    case 4: return qp_str_from_cstr(ctx, mv->format);
    case 5: return JS_NewInt32(ctx, mv->ndim);
    case 6: return qp_mv_ssize_tuple(ctx, mv->ndim, mv->shape);
    case 7: return qp_mv_ssize_tuple(ctx, mv->ndim, mv->strides);
    case 8: return JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    case 9: return JS_NewBool(ctx, (mv->flags & QP_MV_C) != 0);
    case 10: return JS_NewBool(ctx, (mv->flags & QP_MV_FORTRAN) != 0);
    default: return JS_NewBool(ctx, (mv->flags & (QP_MV_C | QP_MV_FORTRAN)) != 0);
    }
}

static const QPGetSetDef qp_memoryview_getsets[] = {
    { "obj", qp_mv_get_attr, NULL, 0, "The underlying object of the memoryview." },
    { "nbytes", qp_mv_get_attr, NULL, 1,
      "The amount of space in bytes that the array would use in\n a contiguous representation." },
    { "readonly", qp_mv_get_attr, NULL, 2, "A bool indicating whether the memory is read only." },
    { "itemsize", qp_mv_get_attr, NULL, 3, "The size in bytes of each element of the memoryview." },
    { "format", qp_mv_get_attr, NULL, 4,
      "A string containing the format (in struct module style)\n for each element in the view." },
    { "ndim", qp_mv_get_attr, NULL, 5,
      "An integer indicating how many dimensions of a multi-dimensional\n array the memory represents." },
    { "shape", qp_mv_get_attr, NULL, 6,
      "A tuple of ndim integers giving the shape of the memory\n as an N-dimensional array." },
    { "strides", qp_mv_get_attr, NULL, 7,
      "A tuple of ndim integers giving the size in bytes to access\n each element for each dimension of the array." },
    { "suboffsets", qp_mv_get_attr, NULL, 8, "A tuple of integers used internally for PIL-style arrays." },
    { "c_contiguous", qp_mv_get_attr, NULL, 9, "A bool indicating whether the memory is C contiguous." },
    { "f_contiguous", qp_mv_get_attr, NULL, 10, "A bool indicating whether the memory is Fortran contiguous." },
    { "contiguous", qp_mv_get_attr, NULL, 11, "A bool indicating whether the memory is contiguous." },
    { NULL, NULL, NULL, 0, NULL },
};

static const QPMethodDef qp_memoryview_methods[] = {
    { "release", qp_mv_release_meth, QP_METH_NOARGS, 0,
      "release($self, /)\n--\n\nRelease the underlying buffer exposed by the memoryview object." },
    { "tobytes", qp_mv_tobytes, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "tobytes($self, /, order='C')\n--\n\nReturn the data in the buffer as a byte string.\n\n"
      "Order can be {'C', 'F', 'A'}. When order is 'C' or 'F', the data of the\n"
      "original array is converted to C or Fortran order. For contiguous views,\n"
      "'A' returns an exact copy of the physical memory. In particular, in-memory\n"
      "Fortran order is preserved. For non-contiguous views, the data is converted\n"
      "to C first. order=None is the same as order='C'." },
    { "hex", qp_mv_hex, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_HEX_DOC("Return the data in the buffer as a str of hexadecimal numbers.",
                 "memoryview(b'\\xb9\\x01\\xef')") },
    { "tolist", qp_mv_tolist, QP_METH_NOARGS, 0,
      "tolist($self, /)\n--\n\nReturn the data in the buffer as a list of elements." },
    { "cast", qp_mv_cast, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "cast($self, /, format, shape=<unrepresentable>)\n--\n\nCast a memoryview to a new format or shape." },
    { "toreadonly", qp_mv_toreadonly, QP_METH_NOARGS, 0,
      "toreadonly($self, /)\n--\n\nReturn a readonly version of the memoryview." },
    { "_from_flags", qp_mv_from_flags, QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 0,
      "_from_flags($type, /, object, flags)\n--\n\nCreate a new memoryview object which references the given object." },
    { "count", qp_mv_count, QP_METH_O, 0,
      "count($self, value, /)\n--\n\nCount the number of occurrences of a value." },
    { "index", qp_mv_index, QP_METH_FASTCALL, 0,
      "index($self, value, start=0, stop=sys.maxsize, /)\n--\n\nReturn the index of the first occurrence of a value.\n\n"
      "Raises ValueError if the value is not present." },
    { "__enter__", qp_mv_enter, QP_METH_NOARGS, 0, NULL },
    { "__exit__", qp_mv_exit, QP_METH_FASTCALL, 0, NULL },
    { "__sizeof__", qp_mv_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nSize of object in memory, in bytes." },
    { "__class_getitem__", qp_mv_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "See PEP 585" },
    { NULL, NULL, 0, 0, NULL },
};

static const QPTypeSpec qp_memoryview_spec = {
    .name = "memoryview",
    .doc = "memoryview(object)\n--\n\nCreate a new memoryview object which references the given object.",
    .flags = QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BYTEARRAY,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_memoryview_tp_repr,
        .tp_hash = qp_memoryview_tp_hash,
        .tp_richcompare = qp_memoryview_tp_richcompare,
        .tp_iter = qp_memoryview_tp_iter,
        .tp_new = qp_memoryview_tp_new,
        .sq = {
            .sq_length = qp_memoryview_sq_length,
            .sq_item = qp_memoryview_sq_item,
        },
        .mp = {
            .mp_length = qp_memoryview_sq_length,
            .mp_subscript = qp_memoryview_mp_subscript,
            .mp_ass_subscript = qp_memoryview_mp_ass_subscript,
        },
        .bf = {
            .bf_getbuffer = qp_memoryview_getbuffer,
            .bf_releasebuffer = qp_memoryview_releasebuffer,
        },
    },
    .methods = qp_memoryview_methods,
    .getsets = qp_memoryview_getsets,
};

static int qp_init_memoryview_type(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, memoryview), &qp_memoryview_spec);
}
