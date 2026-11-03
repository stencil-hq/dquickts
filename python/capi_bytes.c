/*
 * dquickts Python C API (plan D5, Tier S), unit D part 2: bytes, bytearray,
 * memoryview, the buffer protocol, PyBytesWriter, PickleBuffer, strhex.
 *
 * Contract: python/capi.h, design local://capi-design.md (sections 1.2, 3
 * "cpython/bytesobject.h", 6 unit D, 10 "Owner D").  Ports of CPython 3.15
 * Objects/bytesobject.c, Objects/abstract.c (buffer helpers),
 * Objects/memoryobject.c, Objects/picklebufobject.c, Python/pystrhex.c
 * (PSF-2.0) where the code depends only on public layouts.
 *
 * Representation (python/bytesobject_internal.h payloads):
 *  - bytes: the mirror's _qp_sval points at the inline data of the JS
 *    object's QPBytesObject (no copy, NUL-terminated, stable).  bytes are
 *    always J-backed: PyBytes_FromStringAndSize(NULL, n) creates the JS
 *    object at once and C fills it in place before it is shared;
 *    _PyBytes_Resize reallocates the payload of a uniquely referenced
 *    object (identity kept, like CPython's realloc).
 *  - bytearray: live size (_Py_QP_LIVE_SIZE_FLAG) and data pointer
 *    (_PyQp_ByteArrayData) are read from the payload, which moves on resize.
 *  - memoryview (and JS typed arrays / DataView, whose type is memoryview):
 *    PyMemoryViewObject.view is refreshed from the payload at every
 *    hand-out; shape/strides/format point into the payload.  view.obj is
 *    NULL (PyMemoryView_GET_BASE "may be NULL") and mbuf is NULL.
 * Buffers exported to C by JS-backed objects come from qp_buffer_get (1-D)
 * or, for memoryviews, from the full multi-dimensional view (CPython
 * memory_getbuf); Py_buffer.internal holds the bookkeeping block.
 */

#include "bytesobject_internal.h"

_Py_COMP_DIAG_PUSH
_Py_COMP_DIAG_IGNORE_DEPR_DECLS

/* TU-static functions of python/bytearrayobject.c, memoryobject.c and
   mod_codecs.c (defined earlier in the quickjs.c translation unit; repeated
   so that build/check/unit.sh checks this file too). */
static int qp_bytearray_resize(JSContext *ctx, JSValueConst self, Py_ssize_t requested_size);
static JSValue qp_memoryview_from_object(JSContext *ctx, JSValueConst v, int flags);
static JSValue qp_mbuf_new(JSContext *ctx, JSValueConst base, int flags);
static JSValue qp_mv_alloc(JSContext *ctx, JSValue mbuf, int ndim, QPMemoryView **pmv);
static JSValue qp_mv_from_mbuf(JSContext *ctx, JSValue mbuf);
static int qp_mv_set_format(JSContext *ctx, QPMemoryView *mv, const char *fmt);
static void qp_mv_init_flags(QPMemoryView *mv);
static JSValue qp_cd_decode_escape_bytes(JSContext *ctx, const uint8_t *s, Py_ssize_t len,
                                         const char *errors);

/* Internal-header exports of this file (the compat shims live outside the
   quickjs.c TU). */
PyAPI_FUNC(void) _PyBytes_Repeat(char *dest, Py_ssize_t len_dest, const char *src, Py_ssize_t len_src);
PyAPI_FUNC(PyBytesWriter *) _PyBytesWriter_CreateByteArray(Py_ssize_t size);
PyAPI_FUNC(PyObject *) _Py_strhex(const char *argbuf, Py_ssize_t arglen);
PyAPI_FUNC(PyObject *) _Py_strhex_bytes_with_sep(const char *argbuf, Py_ssize_t arglen,
                                                 PyObject *sep, Py_ssize_t bytes_per_group);

/* ========================================================================= */
/* Kinds BYTES / BYTEARRAY / MEMORYVIEW                                       */
/* ========================================================================= */

static size_t qpd_bytes_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyBytesObject) - sizeof(PyObject);
}

static int qpd_bytes_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyBytesObject *o = (PyBytesObject *)op;
    QPBytesObject *b;
    if (qp_bk_kind(v) != QP_BK_BYTES) {
        qp_raise_system_error(ctx, "bytes mirror of a non-bytes value");
        return -1;
    }
    b = QP_BYTES_OBJ(v);
    o->ob_base.ob_size = b->len;
    o->ob_shash = -1;
    o->_qp_sval = (char *)b->data;
    return 0;
}

static size_t qpd_bytearray_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyByteArrayObject) - sizeof(PyObject);
}

static int qpd_bytearray_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    if (qp_bk_kind(v) != QP_BK_BYTEARRAY) {
        qp_raise_system_error(ctx, "bytearray mirror of a non-bytearray value");
        return -1;
    }
    ((PyVarObject *)op)->ob_size = QP_BYTES_OBJ(v)->len;
    QP_CAPI_HEAD(op)->flags |= _Py_QP_LIVE_SIZE_FLAG;
    return 0;
}

/* Bound JS value of a J-backed mirror (QP_NULL when orphaned). */
static JSValueConst qpd_head_value(PyObject *op)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    if (h->flags & QP_CAPI_F_ORPHAN)
        return QP_NULL;
    return h->v;
}

static Py_ssize_t qp_capi_bytearray_live_size(PyObject *op)
{
    JSValueConst v = qpd_head_value(op);
    if (qp_bk_kind(v) != QP_BK_BYTEARRAY)
        return 0;
    return QP_BYTES_OBJ(v)->len;
}

static void qp_capi_bytearray_set_live_size(PyObject *op, Py_ssize_t n)
{
    JSValueConst v = qpd_head_value(op);
    if (qp_bk_kind(v) != QP_BK_BYTEARRAY) {
        PyErr_BadInternalCall();
        return;
    }
    qp_bytearray_resize(qp_capi.ctx, v, n);
}

static char qpd_bytearray_empty[1];

char *_PyQp_ByteArrayData(PyObject *op)
{
    JSValueConst v = qpd_head_value(op);
    QPBytesObject *b;
    if (qp_bk_kind(v) != QP_BK_BYTEARRAY)
        return qpd_bytearray_empty;
    b = QP_BYTES_OBJ(v);
    if (!b->data) {
        qpd_bytearray_empty[0] = '\0';
        return qpd_bytearray_empty;
    }
    return (char *)b->data;
}

/* PyMemoryViewObject + room for shape[0]/strides[0] of typed arrays. */
static size_t qpd_mv_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyMemoryViewObject) - sizeof(PyObject) + 2 * sizeof(Py_ssize_t);
}

static void qpd_mv_fill(PyObject *op)
{
    PyMemoryViewObject *m = (PyMemoryViewObject *)op;
    JSValueConst v = qpd_head_value(op);
    Py_buffer *view = &m->view;

    m->mbuf = NULL;
    m->weakreflist = NULL;
    view->obj = NULL;
    view->suboffsets = NULL;
    view->internal = NULL;
    ((PyVarObject *)op)->ob_size = 0;
    if (qp_bk_kind(v) == QP_BK_MEMORYVIEW) {
        QPMemoryView *mv = QP_MV_OBJ(v);
        m->flags = mv->flags;       /* QP_MV_* == _Py_MEMORYVIEW_* bits */
        m->hash = mv->hash;
        m->exports = mv->exports;
        view->buf = mv->buf;
        view->len = mv->len;
        view->itemsize = mv->itemsize;
        view->readonly = mv->readonly;
        view->ndim = mv->ndim;
        view->format = (char *)mv->format;   /* Py_buffer.format is char * (read-only by contract) */
        view->shape = mv->shape;
        view->strides = mv->strides;
    } else if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
               qp_bm_is_typed_array_class(JS_VALUE_GET_OBJ(v)->class_id)) {
        uint8_t *buf;
        Py_ssize_t len, itemsize;
        int readonly;
        const char *format;
        Py_ssize_t *dims = (Py_ssize_t *)((uint8_t *)op + sizeof(PyMemoryViewObject));
        qp_bm_js_buffer_info(JS_VALUE_GET_OBJ(v), &buf, &len, &readonly, &itemsize, &format);
        if (itemsize <= 0)
            itemsize = 1;
        m->flags = _Py_MEMORYVIEW_C | _Py_MEMORYVIEW_FORTRAN;
        m->hash = -1;
        m->exports = 0;
        view->buf = buf;
        view->len = len;
        view->itemsize = itemsize;
        view->readonly = readonly;
        view->ndim = 1;
        view->format = (char *)format;
        dims[0] = len / itemsize;
        dims[1] = itemsize;
        view->shape = &dims[0];
        view->strides = &dims[1];
    } else {
        /* orphaned or released payload */
        m->flags = _Py_MEMORYVIEW_RELEASED;
        m->hash = -1;
        m->exports = 0;
        view->buf = NULL;
        view->len = 0;
        view->itemsize = 1;
        view->readonly = 1;
        view->ndim = 1;
        view->format = (char *)"B";
        view->shape = NULL;
        view->strides = NULL;
    }
}

static int qpd_mv_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    qpd_mv_fill(op);
    return 0;
}

static void qpd_mv_refresh(JSContext *ctx, PyObject *op)
{
    qpd_mv_fill(op);
}

/* ========================================================================= */
/* bytes                                                                      */
/* ========================================================================= */

static PyObject *qpd_bytes_empty(void)
{
    return Py_NewRef(_PyQp_Singleton(QP_CAPI_SINGLETON_BYTES_EMPTY));
}

PyObject *PyBytes_FromStringAndSize(const char *str, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_SetString(PyExc_SystemError, "Negative size passed to PyBytes_FromStringAndSize");
        return NULL;
    }
    if (size == 0)
        return qpd_bytes_empty();
    return qp_capi_from_js_result(qp_capi.ctx, qp_bytes_new(qp_capi.ctx, str, size));
}

PyObject *PyBytes_FromString(const char *str)
{
    size_t size = strlen(str);
    if (size > PY_SSIZE_T_MAX - sizeof(QPBytesObject) - 1) {
        PyErr_SetString(PyExc_OverflowError, "byte string is too large");
        return NULL;
    }
    return PyBytes_FromStringAndSize(str, (Py_ssize_t)size);
}

PyObject *PyBytes_FromObject(PyObject *x)
{
    JSValueConst v;
    if (x == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (PyBytes_CheckExact(x))
        return Py_NewRef(x);
    if (!PyObject_CheckBuffer(x) && !PyList_CheckExact(x) && !PyTuple_CheckExact(x)) {
        /* only buffers, lists, tuples and iterables: unlike bytes(), no
           integer size, no __index__ */
        PyObject *it, *r;
        if (PyUnicode_Check(x))
            goto cannot_convert;
        it = PyObject_GetIter(x);
        if (it == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_TypeError))
                return NULL;
            PyErr_Clear();
            goto cannot_convert;
        }
        v = qp_capi_value(qp_capi.ctx, it);
        r = JS_IsException(v) ? NULL :
            qp_capi_from_js_result(qp_capi.ctx, qp_bytes_from_object(qp_capi.ctx, v));
        Py_DECREF(it);
        return r;
    }
    v = qp_capi_value(qp_capi.ctx, x);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(qp_capi.ctx, qp_bytes_from_object_plain(qp_capi.ctx, v));
cannot_convert:
    PyErr_Format(PyExc_TypeError, "cannot convert '%.200s' object to bytes",
                 Py_TYPE(x)->tp_name);
    return NULL;
}

Py_ssize_t PyBytes_Size(PyObject *op)
{
    if (!PyBytes_Check(op)) {
        PyErr_Format(PyExc_TypeError, "expected bytes, %.200s found", Py_TYPE(op)->tp_name);
        return -1;
    }
    return Py_SIZE(op);
}

char *PyBytes_AsString(PyObject *op)
{
    if (!PyBytes_Check(op)) {
        PyErr_Format(PyExc_TypeError, "expected bytes, %.200s found", Py_TYPE(op)->tp_name);
        return NULL;
    }
    return PyBytes_AS_STRING(op);
}

int PyBytes_AsStringAndSize(PyObject *obj, char **s, Py_ssize_t *len)
{
    if (s == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyBytes_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected bytes, %.200s found", Py_TYPE(obj)->tp_name);
        return -1;
    }
    *s = PyBytes_AS_STRING(obj);
    if (len != NULL) {
        *len = PyBytes_GET_SIZE(obj);
    } else if (strlen(*s) != (size_t)PyBytes_GET_SIZE(obj)) {
        PyErr_SetString(PyExc_ValueError, "embedded null byte");
        return -1;
    }
    return 0;
}

PyObject *PyBytes_Repr(PyObject *obj, int smartquotes)
{
    if (!PyBytes_Check(obj)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qp_capi_from_js_result(qp_capi.ctx,
                                  qp_bytes_repr(qp_capi.ctx, (const uint8_t *)PyBytes_AS_STRING(obj),
                                                PyBytes_GET_SIZE(obj), smartquotes));
}

/* Resize keeping *pv on failure (_PyBytes_ResizeKeepOnError). */
static int qpd_bytes_resize_keep(PyObject **pv, Py_ssize_t newsize)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *v = *pv, *nv;
    Py_ssize_t oldsize = PyBytes_GET_SIZE(v);
    JSValueConst jv;
    QPBytesObject *qb, *nb;

    if (oldsize == newsize)
        return 0;
    if (newsize == 0) {
        Py_SETREF(*pv, qpd_bytes_empty());
        return 0;
    }
    jv = JS_UNDEFINED;
    if (oldsize != 0 && PyBytes_CheckExact(v) && _PyQp_HasFlag(v, _Py_QP_BOUND_FLAG))
        jv = qpd_head_value(v);
    /* In place only when uniquely referenced (no other C reference, the JS
       object only held by the mirror: never a cached 1-byte singleton) and
       the hash was not computed yet; else a fresh copy. */
    if (qp_bk_kind(jv) != QP_BK_BYTES || Py_REFCNT(v) != 1 ||
        js_rc(JS_VALUE_GET_OBJ(jv))->ref_count >
            ((QP_CAPI_HEAD(v)->flags & QP_CAPI_F_OWNS_V) ? 1 : 0) ||
        QP_BYTES_OBJ(jv)->hash != -1 || ((PyBytesObject *)v)->ob_shash != -1) {
        nv = PyBytes_FromStringAndSize(NULL, newsize);
        if (!nv)
            return -1;
        if (oldsize > 0)
            memcpy(PyBytes_AS_STRING(nv), PyBytes_AS_STRING(v), (size_t)Py_MIN(oldsize, newsize));
        Py_SETREF(*pv, nv);
        return 0;
    }
    if ((size_t)newsize > (size_t)PY_SSIZE_T_MAX - sizeof(QPBytesObject) - 1) {
        PyErr_NoMemory();
        return -1;
    }
    qb = QP_BYTES_OBJ(jv);
    nb = js_realloc_rt(ctx->rt, qb, sizeof(QPBytesObject) + (size_t)newsize + 1);
    if (!nb) {
        PyErr_NoMemory();
        return -1;
    }
    nb->data = nb->inline_data;
    nb->len = newsize;
    nb->hash = -1;
    nb->inline_data[newsize] = '\0';
    JS_VALUE_GET_OBJ(jv)->u.opaque = nb;
    ((PyVarObject *)v)->ob_size = newsize;
    ((PyBytesObject *)v)->ob_shash = -1;
    ((PyBytesObject *)v)->_qp_sval = (char *)nb->inline_data;
    return 0;
}

int _PyBytes_Resize(PyObject **pv, Py_ssize_t newsize)
{
    PyObject *v = *pv;
    if (v == NULL || !PyBytes_Check(v) || newsize < 0) {
        *pv = NULL;
        Py_XDECREF(v);
        PyErr_BadInternalCall();
        return -1;
    }
    if (qpd_bytes_resize_keep(pv, newsize) < 0) {
        Py_CLEAR(*pv);
        return -1;
    }
    return 0;
}

void PyBytes_Concat(PyObject **pv, PyObject *w)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst a, b;
    PyObject *v;

    if (pv == NULL || *pv == NULL)
        return;
    if (w == NULL) {
        Py_CLEAR(*pv);
        return;
    }
    if (Py_REFCNT(*pv) == 1 && PyBytes_CheckExact(*pv)) {
        Py_ssize_t oldsize;
        Py_buffer wb;
        if (PyObject_GetBuffer(w, &wb, PyBUF_SIMPLE) != 0) {
            PyErr_Format(PyExc_TypeError, "can't concat %.100s to %.100s",
                         Py_TYPE(w)->tp_name, Py_TYPE(*pv)->tp_name);
            Py_CLEAR(*pv);
            return;
        }
        oldsize = PyBytes_GET_SIZE(*pv);
        if (oldsize > PY_SSIZE_T_MAX - wb.len) {
            PyErr_NoMemory();
            goto error;
        }
        if (_PyBytes_Resize(pv, oldsize + wb.len) < 0)
            goto error;
        memcpy(PyBytes_AS_STRING(*pv) + oldsize, wb.buf, (size_t)wb.len);
        PyBuffer_Release(&wb);
        return;
    error:
        PyBuffer_Release(&wb);
        Py_CLEAR(*pv);
        return;
    }
    a = qp_capi_value(ctx, *pv);
    b = JS_IsException(a) ? a : qp_capi_value(ctx, w);
    if (JS_IsException(b)) {
        Py_CLEAR(*pv);
        return;
    }
    v = qp_capi_from_js_result(ctx, qp_bm_concat(ctx, a, b, 0));
    Py_SETREF(*pv, v);
}

void PyBytes_ConcatAndDel(PyObject **pv, PyObject *w)
{
    PyBytes_Concat(pv, w);
    Py_XDECREF(w);
}

PyObject *PyBytes_DecodeEscape(const char *s, Py_ssize_t len, const char *errors,
                               Py_ssize_t unicode, const char *recode_encoding)
{
    (void)unicode;
    (void)recode_encoding;
    /* the result is written into a bytes object of len bytes
       (PyBytesWriter_Create(len) -> _PyBytes_FromSize) */
    if ((size_t)len > (size_t)PY_SSIZE_T_MAX - sizeof(QPBytesObject) - 1) {
        PyErr_SetString(PyExc_OverflowError, "byte string is too large");
        return NULL;
    }
    return qp_capi_from_js_result(qp_capi.ctx,
                                  qp_cd_decode_escape_bytes(qp_capi.ctx, (const uint8_t *)s, len, errors));
}

/* sep.join(iterable) through the type's method. */
static PyObject *qpd_call_join(PyObject *sep, PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst s, it;
    JSValue arg, r;
    JSAtom name;

    s = qp_capi_value(ctx, sep);
    if (JS_IsException(s))
        return NULL;
    it = qp_capi_value(ctx, iterable);
    if (JS_IsException(it))
        return NULL;
    name = JS_NewAtom(ctx, "join");
    if (name == JS_ATOM_NULL)
        return NULL;
    arg = JS_DupValue(ctx, it);
    r = qp_call_method(ctx, s, name, (JSValueConst *)&arg, 1, QP_NULL);
    JS_FreeValue(ctx, arg);
    JS_FreeAtom(ctx, name);
    return qp_capi_from_js_result(ctx, r);
}

PyObject *PyBytes_Join(PyObject *sep, PyObject *iterable)
{
    if (sep == NULL || iterable == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyBytes_Check(sep)) {
        PyErr_Format(PyExc_TypeError, "sep: expected bytes, got %.200s", Py_TYPE(sep)->tp_name);
        return NULL;
    }
    return qpd_call_join(sep, iterable);
}

void _PyBytes_Repeat(char *dest, Py_ssize_t len_dest, const char *src, Py_ssize_t len_src)
{
    if (len_dest == 0)
        return;
    if (len_src == 1) {
        memset(dest, src[0], (size_t)len_dest);
    } else {
        Py_ssize_t copied;
        if (src != dest)
            memcpy(dest, src, (size_t)len_src);
        copied = len_src;
        while (copied < len_dest) {
            Py_ssize_t bytes_to_copy = Py_MIN(copied, len_dest - copied);
            memcpy(dest + copied, dest, (size_t)bytes_to_copy);
            copied += bytes_to_copy;
        }
    }
}

/* ========================================================================= */
/* PyBytesWriter (Objects/bytesobject.c)                                      */
/* ========================================================================= */

/* Layout of CPython's pycore_bytesobject.h (the compat shim declares the
   same struct for the modules' inline helpers). */
struct PyBytesWriter {
    char small_buffer[256];
    PyObject *obj;
    Py_ssize_t size;
    int use_bytearray;
    int overallocate;
};

#define QPD_BW_OVERALLOCATE_FACTOR 4

static inline char *qpd_bw_data(PyBytesWriter *writer)
{
    if (writer->obj == NULL)
        return writer->small_buffer;
    if (writer->use_bytearray)
        return PyByteArray_AS_STRING(writer->obj);
    return PyBytes_AS_STRING(writer->obj);
}

static inline Py_ssize_t qpd_bw_allocated(PyBytesWriter *writer)
{
    if (writer->obj == NULL)
        return sizeof(writer->small_buffer);
    if (writer->use_bytearray)
        return PyByteArray_GET_SIZE(writer->obj);
    return PyBytes_GET_SIZE(writer->obj);
}

static int qpd_bw_resize(PyBytesWriter *writer, Py_ssize_t size, int resize)
{
    Py_ssize_t old_allocated = qpd_bw_allocated(writer);
    if (size <= old_allocated)
        return 0;
    if (resize && writer->overallocate) {
        if (size <= (PY_SSIZE_T_MAX - size / QPD_BW_OVERALLOCATE_FACTOR))
            size += size / QPD_BW_OVERALLOCATE_FACTOR;
    }
    if (writer->obj != NULL) {
        if (writer->use_bytearray) {
            if (PyByteArray_Resize(writer->obj, size))
                return -1;
        } else {
            if (qpd_bytes_resize_keep(&writer->obj, size))
                return -1;
        }
    } else if (writer->use_bytearray) {
        writer->obj = PyByteArray_FromStringAndSize(NULL, size);
        if (writer->obj == NULL)
            return -1;
        if (resize)
            memcpy(PyByteArray_AS_STRING(writer->obj), writer->small_buffer, sizeof(writer->small_buffer));
    } else {
        writer->obj = PyBytes_FromStringAndSize(NULL, size);
        if (writer->obj == NULL)
            return -1;
        if (resize)
            memcpy(PyBytes_AS_STRING(writer->obj), writer->small_buffer, sizeof(writer->small_buffer));
    }
    return 0;
}

static PyBytesWriter *qpd_bw_create(Py_ssize_t size, int use_bytearray)
{
    PyBytesWriter *writer;
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 0");
        return NULL;
    }
    writer = (PyBytesWriter *)PyMem_Malloc(sizeof(PyBytesWriter));
    if (writer == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    writer->obj = NULL;
    writer->size = 0;
    writer->use_bytearray = use_bytearray;
    writer->overallocate = !use_bytearray;
    if (size >= 1) {
        if (qpd_bw_resize(writer, size, 0) < 0) {
            PyBytesWriter_Discard(writer);
            return NULL;
        }
        writer->size = size;
    }
    return writer;
}

PyBytesWriter *PyBytesWriter_Create(Py_ssize_t size)
{
    return qpd_bw_create(size, 0);
}

PyBytesWriter *_PyBytesWriter_CreateByteArray(Py_ssize_t size)
{
    return qpd_bw_create(size, 1);
}

void PyBytesWriter_Discard(PyBytesWriter *writer)
{
    if (writer == NULL)
        return;
    Py_XDECREF(writer->obj);
    PyMem_Free(writer);
}

PyObject *PyBytesWriter_FinishWithSize(PyBytesWriter *writer, Py_ssize_t size)
{
    PyObject *result;

    if (size < 0) {
        PyErr_Format(PyExc_ValueError, "size must be positive");
        goto error;
    }
    if (size > writer->size) {
        PyErr_SetString(PyExc_ValueError, "size larger than allocated size");
        goto error;
    }
    if (size == 0 && !writer->use_bytearray) {
        result = qpd_bytes_empty();
    } else if (writer->obj != NULL) {
        if (writer->use_bytearray) {
            if (size != PyByteArray_GET_SIZE(writer->obj)) {
                if (PyByteArray_Resize(writer->obj, size))
                    goto error;
            }
        } else {
            if (size != PyBytes_GET_SIZE(writer->obj)) {
                if (_PyBytes_Resize(&writer->obj, size))
                    goto error;
            }
        }
        result = writer->obj;
        writer->obj = NULL;
    } else if (writer->use_bytearray) {
        result = PyByteArray_FromStringAndSize(writer->small_buffer, size);
    } else {
        result = PyBytes_FromStringAndSize(writer->small_buffer, size);
    }
    PyBytesWriter_Discard(writer);
    return result;

error:
    PyBytesWriter_Discard(writer);
    return NULL;
}

PyObject *PyBytesWriter_Finish(PyBytesWriter *writer)
{
    return PyBytesWriter_FinishWithSize(writer, writer->size);
}

PyObject *PyBytesWriter_FinishWithPointer(PyBytesWriter *writer, void *buf)
{
    Py_ssize_t size = (char *)buf - qpd_bw_data(writer);
    return PyBytesWriter_FinishWithSize(writer, size);
}

void *PyBytesWriter_GetData(PyBytesWriter *writer)
{
    return qpd_bw_data(writer);
}

Py_ssize_t PyBytesWriter_GetSize(PyBytesWriter *writer)
{
    return writer->size;
}

int PyBytesWriter_Resize(PyBytesWriter *writer, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 0");
        return -1;
    }
    if (writer->size < size) {
        if (qpd_bw_resize(writer, size, 1) < 0)
            return -1;
    }
    writer->size = size;
    return 0;
}

int PyBytesWriter_Grow(PyBytesWriter *writer, Py_ssize_t grow)
{
    Py_ssize_t size;
    if (grow == 0)
        return 0;
    if (grow >= 0) {
        if (grow > PY_SSIZE_T_MAX - writer->size) {
            PyErr_NoMemory();
            return -1;
        }
    } else if (writer->size + grow < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid size");
        return -1;
    }
    size = writer->size + grow;
    if (qpd_bw_resize(writer, size, 1) < 0)
        return -1;
    writer->size = size;
    return 0;
}

void *PyBytesWriter_GrowAndUpdatePointer(PyBytesWriter *writer, Py_ssize_t size, void *buf)
{
    Py_ssize_t pos = (char *)buf - qpd_bw_data(writer);
    if (PyBytesWriter_Grow(writer, size) < 0)
        return NULL;
    return qpd_bw_data(writer) + pos;
}

int PyBytesWriter_WriteBytes(PyBytesWriter *writer, const void *bytes, Py_ssize_t size)
{
    Py_ssize_t pos;
    if (size < 0) {
        size_t len = strlen(bytes);
        if (len > (size_t)PY_SSIZE_T_MAX) {
            PyErr_NoMemory();
            return -1;
        }
        size = (Py_ssize_t)len;
    }
    pos = writer->size;
    if (PyBytesWriter_Grow(writer, size) < 0)
        return -1;
    memcpy(qpd_bw_data(writer) + pos, bytes, (size_t)size);
    return 0;
}

/* bytes_fromformat: PyBytes_FromFormatV formatting into the writer at
   writer_pos; returns the end pointer or NULL. */
static char *qpd_bytes_fromformat(PyBytesWriter *writer, Py_ssize_t writer_pos,
                                  const char *format, va_list *vargs)
{
    const char *f, *p;
    Py_ssize_t prec;
    int longflag, size_tflag;
    char buffer[24];
    char *s = qpd_bw_data(writer) + writer_pos;

#define QPD_WRITE_BYTES_LEN(str, len_expr)                                  \
    do {                                                                    \
        size_t len_ = (len_expr);                                           \
        s = PyBytesWriter_GrowAndUpdatePointer(writer, (Py_ssize_t)len_, s); \
        if (s == NULL)                                                      \
            return NULL;                                                    \
        memcpy(s, (str), len_);                                             \
        s += len_;                                                          \
    } while (0)
#define QPD_WRITE_BYTES(str) QPD_WRITE_BYTES_LEN(str, strlen(str))

    for (f = format; *f; f++) {
        if (*f != '%') {
            *s++ = *f;
            continue;
        }
        p = f++;
        while (Py_ISDIGIT(*f))
            f++;
        prec = 0;
        if (*f == '.') {
            f++;
            for (; Py_ISDIGIT(*f); f++)
                prec = (prec * 10) + (*f - '0');
        }
        while (*f && *f != '%' && !Py_ISALPHA(*f))
            f++;
        longflag = 0;
        if (*f == 'l' && (f[1] == 'd' || f[1] == 'u')) {
            longflag = 1;
            ++f;
        }
        size_tflag = 0;
        if (*f == 'z' && (f[1] == 'd' || f[1] == 'u')) {
            size_tflag = 1;
            ++f;
        }
        switch (*f) {
        case 'c': {
            int c = va_arg(*vargs, int);
            if (c < 0 || c > 255) {
                PyErr_SetString(PyExc_OverflowError,
                                "PyBytes_FromFormatV(): %c format expects an integer in range [0; 255]");
                return NULL;
            }
            *s++ = (char)(unsigned char)c;
            break;
        }
        case 'd':
            if (longflag)
                snprintf(buffer, sizeof(buffer), "%ld", va_arg(*vargs, long));
            else if (size_tflag)
                snprintf(buffer, sizeof(buffer), "%zd", va_arg(*vargs, Py_ssize_t));
            else
                snprintf(buffer, sizeof(buffer), "%d", va_arg(*vargs, int));
            QPD_WRITE_BYTES(buffer);
            break;
        case 'u':
            if (longflag)
                snprintf(buffer, sizeof(buffer), "%lu", va_arg(*vargs, unsigned long));
            else if (size_tflag)
                snprintf(buffer, sizeof(buffer), "%zu", va_arg(*vargs, size_t));
            else
                snprintf(buffer, sizeof(buffer), "%u", va_arg(*vargs, unsigned int));
            QPD_WRITE_BYTES(buffer);
            break;
        case 'i':
            snprintf(buffer, sizeof(buffer), "%i", va_arg(*vargs, int));
            QPD_WRITE_BYTES(buffer);
            break;
        case 'x':
            snprintf(buffer, sizeof(buffer), "%x", va_arg(*vargs, int));
            QPD_WRITE_BYTES(buffer);
            break;
        case 's': {
            Py_ssize_t i;
            p = va_arg(*vargs, const char *);
            if (prec <= 0) {
                i = (Py_ssize_t)strlen(p);
            } else {
                i = 0;
                while (i < prec && p[i])
                    i++;
            }
            QPD_WRITE_BYTES_LEN(p, (size_t)i);
            break;
        }
        case 'p':
            snprintf(buffer, sizeof(buffer), "%p", va_arg(*vargs, void *));
            if (buffer[1] == 'X') {
                buffer[1] = 'x';
            } else if (buffer[1] != 'x') {
                memmove(buffer + 2, buffer, strlen(buffer) + 1);
                buffer[0] = '0';
                buffer[1] = 'x';
            }
            QPD_WRITE_BYTES(buffer);
            break;
        case '%':
            *s++ = '%';
            break;
        default:
            /* invalid format string: copy unformatted string and exit */
            QPD_WRITE_BYTES(p);
            return s;
        }
    }
#undef QPD_WRITE_BYTES
#undef QPD_WRITE_BYTES_LEN
    return s;
}

int PyBytesWriter_Format(PyBytesWriter *writer, const char *format, ...)
{
    Py_ssize_t pos = writer->size, old_pos = pos;
    va_list vargs;
    char *buf;

    if (PyBytesWriter_Grow(writer, (Py_ssize_t)strlen(format)) < 0)
        return -1;
    va_start(vargs, format);
    buf = qpd_bytes_fromformat(writer, pos, format, &vargs);
    va_end(vargs);
    if (buf == NULL) {
        writer->size = old_pos;
        return -1;
    }
    return PyBytesWriter_Resize(writer, buf - qpd_bw_data(writer));
}

PyObject *PyBytes_FromFormatV(const char *format, va_list vargs)
{
    PyBytesWriter *writer = PyBytesWriter_Create((Py_ssize_t)strlen(format));
    va_list ap;
    char *s;
    if (writer == NULL)
        return NULL;
    va_copy(ap, vargs);
    s = qpd_bytes_fromformat(writer, 0, format, &ap);
    va_end(ap);
    if (s == NULL) {
        PyBytesWriter_Discard(writer);
        return NULL;
    }
    return PyBytesWriter_FinishWithPointer(writer, s);
}

PyObject *PyBytes_FromFormat(const char *format, ...)
{
    PyObject *ret;
    va_list vargs;
    va_start(vargs, format);
    ret = PyBytes_FromFormatV(format, vargs);
    va_end(vargs);
    return ret;
}

/* ========================================================================= */
/* bytearray                                                                  */
/* ========================================================================= */

PyObject *PyByteArray_FromObject(PyObject *input)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst v = qp_capi_value(ctx, input);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, bytearray)), v));
}

PyObject *PyByteArray_Concat(PyObject *a, PyObject *b)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst av, bv;
    av = qp_capi_value(ctx, a);
    if (JS_IsException(av))
        return NULL;
    bv = qp_capi_value(ctx, b);
    if (JS_IsException(bv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_bm_concat(ctx, av, bv, 1));
}

PyObject *PyByteArray_FromStringAndSize(const char *bytes, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_SetString(PyExc_SystemError, "Negative size passed to PyByteArray_FromStringAndSize");
        return NULL;
    }
    /* CPython stores the content in a bytes object of that size
       (PyBytes_FromStringAndSize(NULL, size)) */
    if ((size_t)size > (size_t)PY_SSIZE_T_MAX - sizeof(QPBytesObject) - 1) {
        PyErr_SetString(PyExc_OverflowError, "byte string is too large");
        return NULL;
    }
    return qp_capi_from_js_result(qp_capi.ctx, qp_bytearray_new(qp_capi.ctx, bytes, size));
}

Py_ssize_t PyByteArray_Size(PyObject *self)
{
    if (!PyByteArray_Check(self)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return PyByteArray_GET_SIZE(self);
}

char *PyByteArray_AsString(PyObject *self)
{
    if (!PyByteArray_Check(self)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyByteArray_AS_STRING(self);
}

int PyByteArray_Resize(PyObject *self, Py_ssize_t requested_size)
{
    JSValueConst v;
    if (!PyByteArray_Check(self)) {
        PyErr_BadInternalCall();
        return -1;
    }
    v = qpd_head_value(self);
    if (qp_bk_kind(v) != QP_BK_BYTEARRAY) {
        PyErr_BadInternalCall();
        return -1;
    }
    return qp_bytearray_resize(qp_capi.ctx, v, requested_size);
}

/* ========================================================================= */
/* Buffer protocol                                                            */
/* ========================================================================= */

/* Py_buffer.internal of the buffers exported by JS-backed objects. */
typedef struct QPDBufInternal {
    int is_mv;              /* full view of a QP memoryview (exports counted) */
    QPBuffer qb;            /* qp_buffer_get export otherwise */
    Py_ssize_t shape, strides;
} QPDBufInternal;

/* The object exports through a C bf_getbuffer (extension type). */
static int qpd_c_exporter(PyObject *obj)
{
    PyBufferProcs *pb = Py_TYPE(obj)->tp_as_buffer;
    return pb != NULL && pb->bf_getbuffer != NULL && pb->bf_getbuffer != qp_capi_bf_getbuffer_rev;
}

#define QPD_REQ_INDIRECT(flags) (((flags) & PyBUF_INDIRECT) == PyBUF_INDIRECT)
#define QPD_REQ_C_CONTIGUOUS(flags) (((flags) & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS)
#define QPD_REQ_F_CONTIGUOUS(flags) (((flags) & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS)
#define QPD_REQ_ANY_CONTIGUOUS(flags) (((flags) & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS)
#define QPD_REQ_STRIDES(flags) (((flags) & PyBUF_STRIDES) == PyBUF_STRIDES)
#define QPD_REQ_SHAPE(flags) (((flags) & PyBUF_ND) == PyBUF_ND)
#define QPD_REQ_WRITABLE(flags) ((flags) & PyBUF_WRITABLE)
#define QPD_REQ_FORMAT(flags) ((flags) & PyBUF_FORMAT)
#define QPD_MV_C_CONTIGUOUS(flags) ((flags) & (_Py_MEMORYVIEW_SCALAR | _Py_MEMORYVIEW_C))
#define QPD_MV_F_CONTIGUOUS(flags) ((flags) & (_Py_MEMORYVIEW_SCALAR | _Py_MEMORYVIEW_FORTRAN))
#define QPD_MV_ANY_CONTIGUOUS(flags) \
    ((flags) & (_Py_MEMORYVIEW_SCALAR | _Py_MEMORYVIEW_C | _Py_MEMORYVIEW_FORTRAN))

/* memory_getbuf over a QP memoryview payload. */
static int qpd_mv_getbuf(QPMemoryView *mv, Py_buffer *view, int flags)
{
    int baseflags = mv->flags;
    if (baseflags & QP_MV_RELEASED) {
        PyErr_SetString(PyExc_ValueError, "operation forbidden on released memoryview object");
        return -1;
    }
    view->buf = mv->buf;
    view->len = mv->len;
    view->itemsize = mv->itemsize;
    view->readonly = mv->readonly;
    view->ndim = mv->ndim;
    view->format = (char *)mv->format;   /* Py_buffer.format is char * (read-only by contract) */
    view->shape = mv->shape;
    view->strides = mv->strides;
    view->suboffsets = NULL;
    if (QPD_REQ_WRITABLE(flags) && mv->readonly) {
        PyErr_SetString(PyExc_BufferError, "memoryview: underlying buffer is not writable");
        return -1;
    }
    if (!QPD_REQ_FORMAT(flags))
        view->format = NULL;
    if (QPD_REQ_C_CONTIGUOUS(flags) && !QPD_MV_C_CONTIGUOUS(baseflags)) {
        PyErr_SetString(PyExc_BufferError, "memoryview: underlying buffer is not C-contiguous");
        return -1;
    }
    if (QPD_REQ_F_CONTIGUOUS(flags) && !QPD_MV_F_CONTIGUOUS(baseflags)) {
        PyErr_SetString(PyExc_BufferError, "memoryview: underlying buffer is not Fortran contiguous");
        return -1;
    }
    if (QPD_REQ_ANY_CONTIGUOUS(flags) && !QPD_MV_ANY_CONTIGUOUS(baseflags)) {
        PyErr_SetString(PyExc_BufferError, "memoryview: underlying buffer is not contiguous");
        return -1;
    }
    if (!QPD_REQ_STRIDES(flags)) {
        if (!QPD_MV_C_CONTIGUOUS(baseflags)) {
            PyErr_SetString(PyExc_BufferError, "memoryview: underlying buffer is not C-contiguous");
            return -1;
        }
        view->strides = NULL;
    }
    if (!QPD_REQ_SHAPE(flags)) {
        if (view->format != NULL) {
            PyErr_Format(PyExc_BufferError,
                         "memoryview: cannot cast to unsigned bytes if the format flag is present");
            return -1;
        }
        view->ndim = 1;
        view->shape = NULL;
    }
    mv->exports++;
    return 0;
}

/* Buffer export of a JS-backed object (also the reverse bf_getbuffer). */
static int qpd_getbuffer_js(PyObject *obj, Py_buffer *view, int flags)
{
    JSContext *ctx = qp_capi.ctx;
    QPDBufInternal *in;
    JSValueConst v;

    v = qp_capi_value(ctx, obj);
    if (JS_IsException(v))
        return -1;
    in = js_mallocz_rt(ctx->rt, sizeof(*in));
    if (!in) {
        PyErr_NoMemory();
        return -1;
    }
    in->qb.obj = JS_UNDEFINED;
    if (qp_bk_kind(v) == QP_BK_MEMORYVIEW) {
        in->is_mv = 1;
        if (qpd_mv_getbuf(QP_MV_OBJ(v), view, flags) < 0) {
            js_free_rt(ctx->rt, in);
            return -1;
        }
    } else {
        QPBuffer *qb = &in->qb;
        if (qp_buffer_get(ctx, v, qb, QPD_REQ_WRITABLE(flags) ? QP_BUF_WRITABLE : QP_BUF_SIMPLE) < 0) {
            js_free_rt(ctx->rt, in);
            return -1;
        }
        if (QPD_REQ_WRITABLE(flags) && qb->readonly) {
            qp_buffer_release(ctx, qb);
            js_free_rt(ctx->rt, in);
            PyErr_SetString(PyExc_BufferError, "Object is not writable.");
            return -1;
        }
        view->buf = qb->buf;
        view->len = qb->len;
        view->readonly = qb->readonly;
        view->itemsize = qb->itemsize > 0 ? qb->itemsize : 1;
        view->format = QPD_REQ_FORMAT(flags) ? (char *)(qb->format ? qb->format : "B") : NULL;
        view->ndim = 1;
        in->shape = view->len / view->itemsize;
        in->strides = view->itemsize;
        view->shape = QPD_REQ_SHAPE(flags) ? &in->shape : NULL;
        view->strides = QPD_REQ_STRIDES(flags) ? &in->strides : NULL;
        view->suboffsets = NULL;
    }
    view->internal = in;
    view->obj = Py_NewRef(obj);
    return 0;
}

static void qpd_releasebuffer_js(PyObject *obj, Py_buffer *view)
{
    QPDBufInternal *in = view->internal;
    JSContext *ctx = qp_capi.ctx;
    if (!in)
        return;
    view->internal = NULL;
    if (in->is_mv) {
        JSValueConst v = qpd_head_value(obj);
        if (qp_bk_kind(v) == QP_BK_MEMORYVIEW) {
            QPMemoryView *mv = QP_MV_OBJ(v);
            if (mv->exports > 0)
                mv->exports--;
        }
    } else {
        qp_buffer_release(ctx, &in->qb);
    }
    js_free_rt(qp_capi.rt, in);
}

static int qp_capi_bf_getbuffer_rev(PyObject *exporter, Py_buffer *view, int flags)
{
    return qpd_getbuffer_js(exporter, view, flags);
}

static void qp_capi_bf_releasebuffer_rev(PyObject *exporter, Py_buffer *view)
{
    qpd_releasebuffer_js(exporter, view);
}

/* Whether the exporter's buffer must be released to stay consistent, i.e.
   CPython's `tp_as_buffer->bf_releasebuffer != NULL` (getargs rejects such
   exporters for "s#"/"y#"/"z#"/"es#"...).  The reverse bridge installs
   bf_releasebuffer when the runtime type has one (bytes: no, bytearray/
   memoryview: yes), as in CPython; the runtime slot decides for mirrors of
   heap types whose __release_buffer__ changed later. */
static int qp_capi_buffer_needs_release(PyObject *o)
{
    PyBufferProcs *pb = Py_TYPE(o)->tp_as_buffer;
    QPType *t;

    if (!pb || !pb->bf_releasebuffer)
        return 0;
    if (pb->bf_releasebuffer != qp_capi_bf_releasebuffer_rev)
        return 1;
    t = qp_capi_type_qp(Py_TYPE(o));
    if (!t) {
        PyErr_Clear();
        return 1;
    }
    return t->slots.bf.bf_releasebuffer != NULL;
}

/* Forward bridge: the QP side takes a buffer from a C exporter.  The
   exporter fills a Py_buffer that stays in place until the release
   (PyBuffer_FillInfo points shape and strides into it), found again
   through view->format, a copy of the format in a block that leads to it. */
typedef struct QPDFwdBuffer {
    Py_buffer view;
} QPDFwdBuffer;

typedef struct QPDFwdFormat {
    QPDFwdBuffer *blk;
    char format[];
} QPDFwdFormat;

static QPDFwdFormat *qpd_fwd_format(const QPBuffer *view)
{
    return (QPDFwdFormat *)(void *)((char *)view->format - offsetof(QPDFwdFormat, format));
}

static int qp_capi_bf_getbuffer_fwd(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags)
{
    PyObject *x;
    PyBufferProcs *pb;
    Py_buffer *pv;
    QPDFwdBuffer *blk;
    QPDFwdFormat *fb;
    const char *fmt;
    size_t fmtlen;
    /* memoryview: PyBUF_FULL_RO without suboffsets, which QP memoryviews do
       not follow (PyBUF_INDIRECT exporters fail the request themselves) */
    int pyflags = (flags & QP_BUF_STRIDED) ? PyBUF_RECORDS_RO : PyBUF_CONTIG_RO | PyBUF_FORMAT;

    if (flags & QP_BUF_WRITABLE)
        pyflags |= PyBUF_WRITABLE;
    x = qp_capi_new_ref(ctx, obj);
    if (!x)
        return -1;
    pb = Py_TYPE(x)->tp_as_buffer;
    if (pb == NULL || pb->bf_getbuffer == NULL) {
        PyErr_Format(PyExc_TypeError, "a bytes-like object is required, not '%.100s'",
                     Py_TYPE(x)->tp_name);
        Py_DECREF(x);
        return -1;
    }
    blk = js_mallocz_rt(ctx->rt, sizeof(QPDFwdBuffer));
    if (!blk) {
        Py_DECREF(x);
        PyErr_NoMemory();
        return -1;
    }
    pv = &blk->view;
    if (pb->bf_getbuffer(x, pv, pyflags) < 0) {
        Py_DECREF(x);
        js_free_rt(ctx->rt, blk);
        return -1;
    }
    Py_DECREF(x);
    fmt = pv->format ? pv->format : "B";
    fmtlen = strlen(fmt);
    fb = js_malloc_rt(ctx->rt, sizeof(QPDFwdFormat) + fmtlen + 1);
    if (!fb) {
        PyBuffer_Release(pv);
        js_free_rt(ctx->rt, blk);
        PyErr_NoMemory();
        return -1;
    }
    fb->blk = blk;
    memcpy(fb->format, fmt, fmtlen + 1);
    view->obj = JS_DupValue(ctx, obj);
    view->buf = pv->buf;
    view->len = pv->len;
    view->readonly = pv->readonly;
    view->itemsize = pv->itemsize > 0 ? pv->itemsize : 1;
    view->format = fb->format;
    return 0;
}

/* memoryview of a C exporter (python/memoryobject.c qp_mv_from_mbuf): the
   exporter's ndim, shape, strides (NULL: C-contiguous) and item size, as
   CPython's memoryview keeps them; -1 when view does not come from the
   forward bridge or its shape is not usable.  The forward bridge asks for
   strides only for memoryview (QP_BUF_STRIDED), else for a C-contiguous
   buffer.  A zero item size (empty ctypes structures) is kept for scalars
   only: QPBuffer and the 1-D views count items with a nonzero size. */
static int qp_capi_buffer_shape(JSContext *ctx, const QPBuffer *view, const Py_ssize_t **pshape,
                                const Py_ssize_t **pstrides, Py_ssize_t *pitemsize)
{
    const QPDFwdBuffer *blk;
    const Py_buffer *pv;
    QPType *t;
    Py_ssize_t n;
    int i;

    *pshape = NULL;
    *pstrides = NULL;
    if (!view->format || !JS_IsObject(view->obj))
        return -1;
    t = qp_type_of(ctx, view->obj);
    if (!t || t->slots.bf.bf_getbuffer != qp_capi_bf_getbuffer_fwd)
        return -1;
    blk = qpd_fwd_format(view)->blk;
    pv = &blk->view;
    if (pv->ndim < 0 || pv->ndim > QP_MV_MAX_NDIM || (pv->ndim > 0 && !pv->shape))
        return -1;
    n = pv->itemsize;
    for (i = 0; i < pv->ndim; i++) {
        if (pv->shape[i] < 0)
            return -1;
        n *= pv->shape[i];
    }
    if (n != view->len || (pv->itemsize <= 0 && pv->ndim > 0))
        return -1;
    *pshape = pv->shape;
    *pstrides = pv->ndim > 0 ? pv->strides : NULL;
    *pitemsize = pv->itemsize;
    return pv->ndim;
}

/* memoryview.obj of a C exporter's buffer: the Py_buffer.obj the exporter
   set, as CPython's memoryview reports view.obj (PickleBuffer redirects to
   the object it wraps).  New reference, JS_UNDEFINED when view does not
   come from the forward bridge, JS_EXCEPTION on binding failure. */
static JSValue qp_capi_buffer_base(JSContext *ctx, const QPBuffer *view)
{
    QPType *t;
    PyObject *base;

    if (!view->format || !JS_IsObject(view->obj))
        return JS_UNDEFINED;
    t = qp_type_of(ctx, view->obj);
    if (!t || t->slots.bf.bf_getbuffer != qp_capi_bf_getbuffer_fwd)
        return JS_UNDEFINED;
    base = qpd_fwd_format(view)->blk->view.obj;
    if (base == NULL)
        return JS_UNDEFINED;
    return qp_capi_to_js(ctx, base);
}

static void qpd_fwd_release(JSRuntime *rt, QPBuffer *view)
{
    QPDFwdFormat *fb;
    QPDFwdBuffer *blk;
    if (!view->format)
        return;
    fb = qpd_fwd_format(view);
    blk = fb->blk;
    view->format = NULL;
    js_free_rt(rt, fb);
    PyBuffer_Release(&blk->view);
    js_free_rt(rt, blk);
}

static void qp_capi_bf_releasebuffer_fwd(JSContext *ctx, QPBuffer *view)
{
    qpd_fwd_release(ctx->rt, view);
}

/* Finalizer-context release (qp_buffer_release_rt of a managed buffer whose
   exporter is a C extension object): same as the forward release. */
static void qp_capi_bf_release_rt(JSRuntime *rt, QPBuffer *view)
{
    qpd_fwd_release(rt, view);
}

int PyObject_CheckBuffer(PyObject *obj)
{
    PyBufferProcs *pb = Py_TYPE(obj)->tp_as_buffer;
    JSValueConst v;
    if (pb != NULL && pb->bf_getbuffer != NULL)
        return 1;
    if (!QP_CAPI_HAS_HEAD(obj) || (QP_CAPI_HEAD(obj)->flags & QP_CAPI_F_NATIVE))
        return 0;
    v = qp_capi_value(qp_capi.ctx, obj);
    if (JS_IsException(v)) {
        PyErr_Clear();
        return 0;
    }
    return qp_buffer_check(qp_capi.ctx, v);
}

int PyObject_GetBuffer(PyObject *obj, Py_buffer *view, int flags)
{
    if (flags != PyBUF_SIMPLE) {
        if (flags == PyBUF_READ || flags == PyBUF_WRITE) {
            PyErr_BadInternalCall();
            return -1;
        }
    }
    if (qpd_c_exporter(obj))
        return Py_TYPE(obj)->tp_as_buffer->bf_getbuffer(obj, view, flags);
    if (!QP_CAPI_HAS_HEAD(obj) || (QP_CAPI_HEAD(obj)->flags & QP_CAPI_F_NATIVE)) {
        PyErr_Format(PyExc_TypeError, "a bytes-like object is required, not '%.100s'",
                     Py_TYPE(obj)->tp_name);
        return -1;
    }
    return qpd_getbuffer_js(obj, view, flags);
}

void PyBuffer_Release(Py_buffer *view)
{
    PyObject *obj = view->obj;
    if (!obj)
        return;
    if (qpd_c_exporter(obj)) {
        PyBufferProcs *pb = Py_TYPE(obj)->tp_as_buffer;
        if (pb->bf_releasebuffer)
            pb->bf_releasebuffer(obj, view);
    } else if (QP_CAPI_HAS_HEAD(obj) && !(QP_CAPI_HEAD(obj)->flags & QP_CAPI_F_NATIVE)) {
        qpd_releasebuffer_js(obj, view);
    }
    view->obj = NULL;
    Py_DECREF(obj);
}

int PyBuffer_FillInfo(Py_buffer *view, PyObject *obj, void *buf, Py_ssize_t len, int readonly, int flags)
{
    if (view == NULL) {
        PyErr_SetString(PyExc_BufferError, "PyBuffer_FillInfo: view==NULL argument is obsolete");
        return -1;
    }
    if (flags != PyBUF_SIMPLE) {
        if (flags == PyBUF_READ || flags == PyBUF_WRITE) {
            PyErr_BadInternalCall();
            return -1;
        }
    }
    if (((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) && (readonly == 1)) {
        PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        return -1;
    }
    view->obj = Py_XNewRef(obj);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
        view->format = (char *)"B";
    view->ndim = 1;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND)
        view->shape = &(view->len);
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
        view->strides = &(view->itemsize);
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

static int qpd_is_fortran_contiguous(const Py_buffer *view)
{
    Py_ssize_t sd, dim;
    int i;
    if (view->len == 0)
        return 1;
    if (view->strides == NULL) {
        if (view->ndim <= 1)
            return 1;
        sd = 0;
        for (i = 0; i < view->ndim; i++) {
            if (view->shape[i] > 1)
                sd += 1;
        }
        return sd <= 1;
    }
    sd = view->itemsize;
    for (i = 0; i < view->ndim; i++) {
        dim = view->shape[i];
        if (dim > 1 && view->strides[i] != sd)
            return 0;
        sd *= dim;
    }
    return 1;
}

static int qpd_is_c_contiguous(const Py_buffer *view)
{
    Py_ssize_t sd, dim;
    int i;
    if (view->len == 0)
        return 1;
    if (view->strides == NULL)
        return 1;
    sd = view->itemsize;
    for (i = view->ndim - 1; i >= 0; i--) {
        dim = view->shape[i];
        if (dim > 1 && view->strides[i] != sd)
            return 0;
        sd *= dim;
    }
    return 1;
}

int PyBuffer_IsContiguous(const Py_buffer *view, char order)
{
    if (view->suboffsets != NULL)
        return 0;
    if (order == 'C')
        return qpd_is_c_contiguous(view);
    if (order == 'F')
        return qpd_is_fortran_contiguous(view);
    if (order == 'A')
        return qpd_is_c_contiguous(view) || qpd_is_fortran_contiguous(view);
    return 0;
}

void *PyBuffer_GetPointer(const Py_buffer *view, const Py_ssize_t *indices)
{
    char *pointer = (char *)view->buf;
    int i;
    for (i = 0; i < view->ndim; i++) {
        pointer += view->strides[i] * indices[i];
        if ((view->suboffsets != NULL) && (view->suboffsets[i] >= 0))
            pointer = *((char **)pointer) + view->suboffsets[i];
    }
    return (void *)pointer;
}

static void qpd_add_one_to_index_F(int nd, Py_ssize_t *index, const Py_ssize_t *shape)
{
    int k;
    for (k = 0; k < nd; k++) {
        if (index[k] < shape[k] - 1) {
            index[k]++;
            break;
        }
        index[k] = 0;
    }
}

static void qpd_add_one_to_index_C(int nd, Py_ssize_t *index, const Py_ssize_t *shape)
{
    int k;
    for (k = nd - 1; k >= 0; k--) {
        if (index[k] < shape[k] - 1) {
            index[k]++;
            break;
        }
        index[k] = 0;
    }
}

Py_ssize_t PyBuffer_SizeFromFormat(const char *format)
{
    PyObject *calcsize, *fmt = NULL, *res = NULL;
    Py_ssize_t itemsize = -1;

    calcsize = PyImport_ImportModuleAttrString("struct", "calcsize");
    if (calcsize == NULL)
        goto done;
    fmt = PyUnicode_FromString(format);
    if (fmt == NULL)
        goto done;
    res = PyObject_CallFunctionObjArgs(calcsize, fmt, NULL);
    if (res == NULL)
        goto done;
    itemsize = PyLong_AsSsize_t(res);
done:
    Py_XDECREF(calcsize);
    Py_XDECREF(fmt);
    Py_XDECREF(res);
    return itemsize;
}

void PyBuffer_FillContiguousStrides(int nd, Py_ssize_t *shape, Py_ssize_t *strides, int itemsize,
                                    char order)
{
    int k;
    Py_ssize_t sd = itemsize;
    if (order == 'F') {
        for (k = 0; k < nd; k++) {
            strides[k] = sd;
            sd *= shape[k];
        }
    } else {
        for (k = nd - 1; k >= 0; k--) {
            strides[k] = sd;
            sd *= shape[k];
        }
    }
}

/* Full shape/strides of a view (NULL shape: 1-D of len/itemsize; NULL
   strides: C-contiguous).  `store` has room for 2 * ndim entries. */
static void qpd_full_dims(const Py_buffer *src, Py_ssize_t *store, const Py_ssize_t **pshape,
                          const Py_ssize_t **pstrides, int *pndim)
{
    int ndim = src->ndim > 0 ? src->ndim : 1;
    const Py_ssize_t *shape = src->shape;
    if (shape == NULL) {
        store[0] = src->itemsize > 0 ? src->len / src->itemsize : src->len;
        shape = store;
        ndim = 1;
    }
    *pshape = shape;
    if (src->strides) {
        *pstrides = src->strides;
    } else {
        PyBuffer_FillContiguousStrides(ndim, (Py_ssize_t *)shape, store + ndim,
                                       (int)(src->itemsize > 0 ? src->itemsize : 1), 'C');
        *pstrides = store + ndim;
    }
    *pndim = ndim;
}

int PyBuffer_ToContiguous(void *buf, const Py_buffer *src, Py_ssize_t len, char order)
{
    Py_ssize_t *store, *indices, elements, itemsize;
    const Py_ssize_t *shape, *strides;
    Py_buffer full;
    char *dst = buf;
    int ndim, k;

    if (len != src->len) {
        PyErr_SetString(PyExc_ValueError, "PyBuffer_ToContiguous: len != view->len");
        return -1;
    }
    if (PyBuffer_IsContiguous(src, order)) {
        memcpy(buf, src->buf, (size_t)len);
        return 0;
    }
    k = src->ndim > 0 ? src->ndim : 1;
    store = PyMem_Malloc(sizeof(Py_ssize_t) * 3 * (size_t)k);
    if (store == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    qpd_full_dims(src, store, &shape, &strides, &ndim);
    indices = store + 2 * k;
    for (k = 0; k < ndim; k++)
        indices[k] = 0;
    full = *src;
    full.ndim = ndim;
    full.shape = (Py_ssize_t *)shape;
    full.strides = (Py_ssize_t *)strides;
    itemsize = src->itemsize > 0 ? src->itemsize : 1;
    elements = len / itemsize;
    while (elements--) {
        memcpy(dst, PyBuffer_GetPointer(&full, indices), (size_t)itemsize);
        dst += itemsize;
        if (order == 'F')
            qpd_add_one_to_index_F(ndim, indices, shape);
        else
            qpd_add_one_to_index_C(ndim, indices, shape);
    }
    PyMem_Free(store);
    return 0;
}

int PyBuffer_FromContiguous(const Py_buffer *view, const void *buf, Py_ssize_t len, char order)
{
    int k;
    void (*addone)(int, Py_ssize_t *, const Py_ssize_t *);
    Py_ssize_t *indices, elements;
    char *ptr;
    const char *src;

    if (len > view->len)
        len = view->len;
    if (PyBuffer_IsContiguous(view, order)) {
        memcpy(view->buf, buf, (size_t)len);
        return 0;
    }
    indices = (Py_ssize_t *)PyMem_Malloc(sizeof(Py_ssize_t) * (size_t)(view->ndim));
    if (indices == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (k = 0; k < view->ndim; k++)
        indices[k] = 0;
    addone = order == 'F' ? qpd_add_one_to_index_F : qpd_add_one_to_index_C;
    src = buf;
    elements = len / view->itemsize;
    while (elements--) {
        ptr = PyBuffer_GetPointer(view, indices);
        memcpy(ptr, src, (size_t)view->itemsize);
        src += view->itemsize;
        addone(view->ndim, indices, view->shape);
    }
    PyMem_Free(indices);
    return 0;
}

int PyObject_CopyData(PyObject *dest, PyObject *src)
{
    Py_buffer view_dest, view_src;
    int k;
    Py_ssize_t *indices, elements;
    char *dptr, *sptr;

    if (!PyObject_CheckBuffer(dest) || !PyObject_CheckBuffer(src)) {
        PyErr_SetString(PyExc_TypeError, "both destination and source must be bytes-like objects");
        return -1;
    }
    if (PyObject_GetBuffer(dest, &view_dest, PyBUF_FULL) != 0)
        return -1;
    if (PyObject_GetBuffer(src, &view_src, PyBUF_FULL_RO) != 0) {
        PyBuffer_Release(&view_dest);
        return -1;
    }
    if (view_dest.len < view_src.len) {
        PyErr_SetString(PyExc_BufferError, "destination is too small to receive data from source");
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return -1;
    }
    if ((PyBuffer_IsContiguous(&view_dest, 'C') && PyBuffer_IsContiguous(&view_src, 'C')) ||
        (PyBuffer_IsContiguous(&view_dest, 'F') && PyBuffer_IsContiguous(&view_src, 'F'))) {
        memcpy(view_dest.buf, view_src.buf, (size_t)view_src.len);
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return 0;
    }
    indices = (Py_ssize_t *)PyMem_Malloc(sizeof(Py_ssize_t) * (size_t)view_src.ndim);
    if (indices == NULL) {
        PyErr_NoMemory();
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return -1;
    }
    for (k = 0; k < view_src.ndim; k++)
        indices[k] = 0;
    elements = 1;
    for (k = 0; k < view_src.ndim; k++)
        elements *= view_src.shape[k];
    while (elements--) {
        dptr = PyBuffer_GetPointer(&view_dest, indices);
        sptr = PyBuffer_GetPointer(&view_src, indices);
        memcpy(dptr, sptr, (size_t)view_src.itemsize);
        qpd_add_one_to_index_C(view_src.ndim, indices, view_src.shape);
    }
    PyMem_Free(indices);
    PyBuffer_Release(&view_dest);
    PyBuffer_Release(&view_src);
    return 0;
}

/* ========================================================================= */
/* memoryview                                                                 */
/* ========================================================================= */

PyObject *PyMemoryView_FromObject(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_memoryview_from_object(ctx, jv, QP_BUF_SIMPLE));
}

/* memoryview over a managed buffer (consumed) with an explicit structure. */
static JSValue qpd_mv_with_dims(JSContext *ctx, JSValue mbuf, void *buf, Py_ssize_t len,
                                int readonly, Py_ssize_t itemsize, const char *format, int ndim,
                                const Py_ssize_t *shape, const Py_ssize_t *strides)
{
    QPMemoryView *mv;
    JSValue obj;
    int i;

    if (ndim < 0 || ndim > QP_MV_MAX_NDIM) {
        JS_FreeValue(ctx, mbuf);
        return qp_raise_value_error(ctx, "memoryview: number of dimensions must not exceed %d",
                                    QP_MV_MAX_NDIM);
    }
    obj = qp_mv_alloc(ctx, mbuf, ndim, &mv);
    if (JS_IsException(obj))
        return obj;
    mv->buf = buf;
    mv->len = len;
    mv->itemsize = itemsize > 0 ? itemsize : 1;
    mv->readonly = readonly;
    if (qp_mv_set_format(ctx, mv, format ? format : "B") < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    mv->ndim = ndim;
    for (i = 0; i < ndim; i++) {
        mv->shape[i] = shape ? shape[i] : (i == 0 ? len / mv->itemsize : 1);
        mv->strides[i] = strides ? strides[i] : 0;
    }
    if (!strides && ndim > 0) {
        Py_ssize_t sd = mv->itemsize;
        for (i = ndim - 1; i >= 0; i--) {
            mv->strides[i] = sd;
            sd *= mv->shape[i];
        }
    }
    qp_mv_init_flags(mv);
    return obj;
}

PyObject *PyMemoryView_FromMemory(char *mem, Py_ssize_t size, int flags)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue mbuf;
    int readonly;

    if (!(flags == PyBUF_READ || flags == PyBUF_WRITE)) {
        PyErr_SetString(PyExc_SystemError, "PyMemoryView_FromMemory: flags must be PyBUF_READ or PyBUF_WRITE");
        return NULL;
    }
    readonly = (flags == PyBUF_WRITE) ? 0 : 1;
    mbuf = qp_mbuf_raw(ctx, mem, size, readonly, 1, "B");
    if (JS_IsException(mbuf))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_mv_from_mbuf(ctx, mbuf));
}

PyObject *PyMemoryView_FromBuffer(const Py_buffer *info)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue mbuf;

    if (info->buf == NULL) {
        PyErr_SetString(PyExc_ValueError, "PyMemoryView_FromBuffer(): info->buf must not be NULL");
        return NULL;
    }
    mbuf = qp_mbuf_raw(ctx, info->buf, info->len, info->readonly, info->itemsize, info->format);
    if (JS_IsException(mbuf))
        return NULL;
    return qp_capi_from_js_result(ctx, qpd_mv_with_dims(ctx, mbuf, info->buf, info->len, info->readonly,
                                                        info->itemsize, info->format,
                                                        info->shape ? info->ndim : 1,
                                                        info->shape, info->shape ? info->strides : NULL));
}

PyObject *PyMemoryView_GetContiguous(PyObject *obj, int buffertype, char order)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *mv, *bytes;
    Py_buffer *view;
    Py_ssize_t *strides;
    JSValue mbuf;
    JSValueConst bv;
    PyObject *res;

    mv = PyMemoryView_FromObject(obj);
    if (mv == NULL)
        return NULL;
    view = &((PyMemoryViewObject *)mv)->view;
    if (buffertype == PyBUF_WRITE && view->readonly) {
        PyErr_SetString(PyExc_BufferError, "underlying buffer is not writable");
        Py_DECREF(mv);
        return NULL;
    }
    if (PyBuffer_IsContiguous(view, order))
        return mv;
    if (buffertype == PyBUF_WRITE) {
        PyErr_SetString(PyExc_BufferError, "writable contiguous buffer requested for a non-contiguous object.");
        Py_DECREF(mv);
        return NULL;
    }
    /* memory_from_contiguous_copy: copy into a bytes object, then view it
       with the same format and shape and contiguous strides */
    bytes = PyBytes_FromStringAndSize(NULL, view->len);
    if (bytes == NULL) {
        Py_DECREF(mv);
        return NULL;
    }
    if (PyBuffer_ToContiguous(PyBytes_AS_STRING(bytes), view, view->len, order) < 0)
        goto fail;
    strides = PyMem_New(Py_ssize_t, view->ndim > 0 ? view->ndim : 1);
    if (!strides) {
        PyErr_NoMemory();
        goto fail;
    }
    PyBuffer_FillContiguousStrides(view->ndim, view->shape, strides, (int)view->itemsize,
                                   order == 'F' ? 'F' : 'C');
    bv = qp_capi_value(ctx, bytes);
    if (JS_IsException(bv)) {
        PyMem_Free(strides);
        goto fail;
    }
    mbuf = qp_mbuf_new(ctx, bv, QP_BUF_SIMPLE);
    if (JS_IsException(mbuf)) {
        PyMem_Free(strides);
        goto fail;
    }
    res = qp_capi_from_js_result(ctx, qpd_mv_with_dims(ctx, mbuf, PyBytes_AS_STRING(bytes), view->len, 1,
                                                       view->itemsize, view->format, view->ndim,
                                                       view->shape, strides));
    PyMem_Free(strides);
    Py_DECREF(bytes);
    Py_DECREF(mv);
    return res;

fail:
    Py_DECREF(bytes);
    Py_DECREF(mv);
    return NULL;
}

/* ========================================================================= */
/* PickleBuffer (Objects/picklebufobject.c): static extension type            */
/* ========================================================================= */

typedef struct {
    PyObject_HEAD
    Py_buffer view;
    PyObject *weakreflist;
} QPDPickleBufferObject;

PyObject *PyPickleBuffer_FromObject(PyObject *base)
{
    PyTypeObject *type = &PyPickleBuffer_Type;
    QPDPickleBufferObject *self;

    self = (QPDPickleBufferObject *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    self->view.obj = NULL;
    self->weakreflist = NULL;
    if (PyObject_GetBuffer(base, &self->view, PyBUF_FULL_RO) < 0) {
        Py_DECREF(self);
        return NULL;
    }
    return (PyObject *)self;
}

const Py_buffer *PyPickleBuffer_GetBuffer(PyObject *obj)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)obj;
    if (!PyPickleBuffer_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected PickleBuffer, %.200s found", Py_TYPE(obj)->tp_name);
        return NULL;
    }
    if (self->view.obj == NULL) {
        PyErr_SetString(PyExc_ValueError, "operation forbidden on released PickleBuffer object");
        return NULL;
    }
    return &self->view;
}

int PyPickleBuffer_Release(PyObject *obj)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)obj;
    if (!PyPickleBuffer_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected PickleBuffer, %.200s found", Py_TYPE(obj)->tp_name);
        return -1;
    }
    PyBuffer_Release(&self->view);
    return 0;
}

static PyObject *qpd_picklebuf_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    QPDPickleBufferObject *self;
    PyObject *base;
    static char empty[] = "";
    char *keywords[] = { empty, NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:PickleBuffer", keywords, &base))
        return NULL;
    self = (QPDPickleBufferObject *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    self->view.obj = NULL;
    self->weakreflist = NULL;
    if (PyObject_GetBuffer(base, &self->view, PyBUF_FULL_RO) < 0) {
        Py_DECREF(self);
        return NULL;
    }
    return (PyObject *)self;
}

static int qpd_picklebuf_traverse(PyObject *op, visitproc visit, void *arg)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    Py_VISIT(self->view.obj);
    return 0;
}

static int qpd_picklebuf_clear(PyObject *op)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    PyBuffer_Release(&self->view);
    return 0;
}

static void qpd_picklebuf_dealloc(PyObject *op)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    PyObject_GC_UnTrack(self);
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs(op);
    PyBuffer_Release(&self->view);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int qpd_picklebuf_getbuf(PyObject *op, Py_buffer *view, int flags)
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    if (self->view.obj == NULL) {
        PyErr_SetString(PyExc_ValueError, "operation forbidden on released PickleBuffer object");
        return -1;
    }
    return PyObject_GetBuffer(self->view.obj, view, flags);
}

static void qpd_picklebuf_releasebuf(PyObject *self, Py_buffer *view)
{
    /* bf_getbuffer redirects to the original object: never called (it only
       signals non-trivial releasing, as in CPython). */
    (void)self;
    (void)view;
}

static PyBufferProcs qpd_picklebuf_as_buffer = {
    .bf_getbuffer = qpd_picklebuf_getbuf,
    .bf_releasebuffer = qpd_picklebuf_releasebuf,
};

static PyObject *qpd_picklebuf_raw(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    JSContext *ctx = qp_capi.ctx;
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    PyObject *m;
    JSValueConst mv;

    if (self->view.obj == NULL) {
        PyErr_SetString(PyExc_ValueError, "operation forbidden on released PickleBuffer object");
        return NULL;
    }
    if (self->view.suboffsets != NULL || !PyBuffer_IsContiguous(&self->view, 'A')) {
        PyErr_SetString(PyExc_BufferError, "cannot extract raw buffer from non-contiguous buffer");
        return NULL;
    }
    m = PyMemoryView_FromObject(op);
    if (m == NULL)
        return NULL;
    /* make it a "raw" memoryview: format B, 1-D of len bytes (the
       payload of the JS memoryview is authoritative) */
    mv = qpd_head_value(m);
    if (qp_bk_kind(mv) == QP_BK_MEMORYVIEW) {
        QPMemoryView *p = QP_MV_OBJ(mv);
        if (qp_mv_set_format(ctx, p, "B") < 0) {
            Py_DECREF(m);
            return NULL;
        }
        p->ndim = 1;
        p->itemsize = 1;
        p->shape[0] = p->len;
        p->strides[0] = 1;
        p->flags = (p->flags & QP_MV_RELEASED) | QP_MV_C | QP_MV_FORTRAN;
        qpd_mv_fill(m);
    } else {
        (void)ctx;
    }
    return m;
}

static PyObject *qpd_picklebuf_release(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    QPDPickleBufferObject *self = (QPDPickleBufferObject *)op;
    PyBuffer_Release(&self->view);
    Py_RETURN_NONE;
}

static PyMethodDef qpd_picklebuf_methods[] = {
    { "raw", qpd_picklebuf_raw, METH_NOARGS,
      PyDoc_STR("raw($self, /)\n--\n\nReturn a memoryview of the raw memory underlying this buffer.\n"
                "Will raise BufferError is the buffer isn't contiguous.") },
    { "release", qpd_picklebuf_release, METH_NOARGS,
      PyDoc_STR("release($self, /)\n--\n\nRelease the underlying buffer exposed by the PickleBuffer object.") },
    { NULL, NULL, 0, NULL }
};

PyTypeObject PyPickleBuffer_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pickle.PickleBuffer",
    .tp_doc = PyDoc_STR("Wrapper for potentially out-of-band buffers"),
    .tp_basicsize = sizeof(QPDPickleBufferObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = qpd_picklebuf_new,
    .tp_dealloc = qpd_picklebuf_dealloc,
    .tp_traverse = qpd_picklebuf_traverse,
    .tp_clear = qpd_picklebuf_clear,
    .tp_weaklistoffset = offsetof(QPDPickleBufferObject, weakreflist),
    .tp_as_buffer = &qpd_picklebuf_as_buffer,
    .tp_methods = qpd_picklebuf_methods,
};

/* ========================================================================= */
/* strhex (Python/pystrhex.c), Py_hexdigits                                   */
/* ========================================================================= */

const char *Py_hexdigits = "0123456789abcdef";

PyObject *_Py_strhex(const char *argbuf, Py_ssize_t arglen)
{
    PyObject *u;
    Py_UCS1 *d;
    Py_ssize_t i;

    if (arglen > PY_SSIZE_T_MAX / 2)
        return PyErr_NoMemory();
    u = PyUnicode_New(arglen * 2, 127);
    if (!u)
        return NULL;
    d = PyUnicode_1BYTE_DATA(u);
    for (i = 0; i < arglen; i++) {
        unsigned char c = (unsigned char)argbuf[i];
        d[2 * i] = (Py_UCS1)Py_hexdigits[c >> 4];
        d[2 * i + 1] = (Py_UCS1)Py_hexdigits[c & 0xf];
    }
    return u;
}

PyObject *_Py_strhex_bytes_with_sep(const char *argbuf, Py_ssize_t arglen, PyObject *sep,
                                    Py_ssize_t bytes_per_group)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst sv = QP_NULL;
    JSValue s, b;

    if (sep != NULL) {
        sv = qp_capi_value(ctx, sep);
        if (JS_IsException(sv))
            return NULL;
    }
    s = qp_bytes_strhex(ctx, (const uint8_t *)argbuf, arglen, sv, bytes_per_group);
    if (JS_IsException(s))
        return NULL;
    b = qp_str_encode(ctx, s, "ascii", NULL);
    JS_FreeValue(ctx, s);
    return qp_capi_from_js_result(ctx, b);
}

/* ========================================================================= */
/* Init (called by qp_capi_init_str)                                          */
/* ========================================================================= */

static int qp_capi_init_bytes(JSContext *ctx)
{
    QPCapiKindOps *k;

    k = &qp_capi.kind_ops[QP_CAPI_K_BYTES];
    k->name = "bytes";
    k->size = qpd_bytes_size;
    k->init = qpd_bytes_init;
    k->to_js = NULL;
    k->refresh = NULL;
    k->traverse = NULL;
    k->clear = NULL;

    k = &qp_capi.kind_ops[QP_CAPI_K_BYTEARRAY];
    k->name = "bytearray";
    k->size = qpd_bytearray_size;
    k->init = qpd_bytearray_init;
    k->to_js = NULL;
    k->refresh = NULL;
    k->traverse = NULL;
    k->clear = NULL;

    k = &qp_capi.kind_ops[QP_CAPI_K_MEMORYVIEW];
    k->name = "memoryview";
    k->size = qpd_mv_size;
    k->init = qpd_mv_init;
    k->to_js = NULL;
    k->refresh = qpd_mv_refresh;
    k->traverse = NULL;
    k->clear = NULL;

    if (!(PyPickleBuffer_Type.tp_flags & Py_TPFLAGS_READY)) {
        if (PyType_Ready(&PyPickleBuffer_Type) < 0)
            return -1;
    }
    return 0;
}

_Py_COMP_DIAG_POP
