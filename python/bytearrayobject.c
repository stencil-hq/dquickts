/*
 * bytearray type.
 *
 * Derived from CPython 3.15 Objects/bytearrayobject.c (PSF-2.0).  The data
 * is a plain js_malloc'd block (CPython 3.15 keeps it in a hidden bytes
 * object for take_bytes(); the observable behaviour, including the
 * allocation policy reported by __alloc__(), is the same).  Methods shared
 * with bytes are the qp_bm_* functions of bytesobject.c.
 */
#include "bytesobject_internal.h"

static void qp_bytearray_free_payload(JSRuntime *rt, QPBytesObject *b)
{
    if (b->data)
        js_free_rt(rt, b->data);
    js_free_rt(rt, b);
}

/* Empty bytearray instance of subtype (NULL = exact). */
static JSValue qp_bytearray_alloc(JSContext *ctx, QPType *subtype)
{
    QPBytesObject *b;
    JSValue obj;
    QPType *bt = QP_TYPE(ctx, bytearray);

    b = js_malloc(ctx, sizeof(QPBytesObject));
    if (!b)
        return JS_EXCEPTION;
    b->kind = QP_BK_BYTEARRAY;
    b->len = 0;
    b->data = NULL;
    b->hash = -1;
    b->alloc = 0;
    b->exports = 0;
    if (subtype == NULL || subtype == bt)
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(bt), JS_CLASS_PY_BYTEARRAY);
    else
        obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_BYTEARRAY);
    if (JS_IsException(obj)) {
        js_free(ctx, b);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = b;
    return obj;
}

/* PyByteArray_FromStringAndSize (data NULL: zero-filled). */
static JSValue qp_bytearray_new(JSContext *ctx, const void *data, Py_ssize_t len)
{
    JSValue obj;
    QPBytesObject *b;
    if (len < 0)
        return qp_raise_system_error(ctx, "Negative size passed to PyByteArray_FromStringAndSize");
    if (len >= PY_SSIZE_T_MAX)
        return qp_raise_memory_error(ctx);
    obj = qp_bytearray_alloc(ctx, NULL);
    if (JS_IsException(obj) || len == 0)
        return obj;
    b = QP_BYTES_OBJ(obj);
    b->data = js_malloc(ctx, len + 1);
    if (!b->data) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (data)
        memcpy(b->data, data, len);
    else
        memset(b->data, 0, len);
    b->data[len] = '\0';
    b->len = len;
    b->alloc = len;
    return obj;
}

static int qp_bytearray_canresize(JSContext *ctx, QPBytesObject *b)
{
    if (b->exports > 0) {
        qp_raise(ctx, QP_EXC(ctx, BufferError), "Existing exports of data: object cannot be re-sized");
        return 0;
    }
    return 1;
}

/* bytearray_resize_lock_held: CPython's allocation policy. */
static int qp_bytearray_resize(JSContext *ctx, JSValueConst self, Py_ssize_t requested_size)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    size_t alloc = (size_t)b->alloc;
    size_t size = (size_t)requested_size;
    uint8_t *p;

    if (requested_size < 0) {
        qp_raise_value_error(ctx, "Can only resize to positive sizes, got %zd", requested_size);
        return -1;
    }
    if (requested_size == b->len)
        return 0;
    if (!qp_bytearray_canresize(ctx, b))
        return -1;
    if (requested_size == 0) {
        if (b->data)
            js_free(ctx, b->data);
        b->data = NULL;
        b->alloc = 0;
        b->len = 0;
        return 0;
    }
    if (size <= alloc) {
        if (size < alloc / 2) {
            alloc = size;
        } else {
            b->len = requested_size;
            b->data[size] = '\0';
            return 0;
        }
    } else {
        if ((double)size <= (double)alloc * 1.125)
            alloc = size + (size >> 3) + (size < 9 ? 3 : 6);
        else
            alloc = size;
    }
    if (alloc >= (size_t)PY_SSIZE_T_MAX) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    p = b->data ? js_realloc(ctx, b->data, alloc + 1) : js_malloc(ctx, alloc + 1);
    if (!p)
        return -1;
    b->data = p;
    b->alloc = (Py_ssize_t)alloc;
    b->len = requested_size;
    p[size] = '\0';
    return 0;
}

/* ------------------------------------------------------------------------ */
/* buffer protocol                                                           */
/* ------------------------------------------------------------------------ */

static int qp_bytearray_getbuffer(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags)
{
    QPBytesObject *b = QP_BYTES_OBJ(obj);
    view->obj = JS_DupValue(ctx, obj);
    view->buf = b->data ? b->data : (uint8_t *)qp_bytes_empty_data;
    view->len = b->len;
    view->readonly = 0;
    view->itemsize = 1;
    view->format = "B";
    b->exports++;
    return 0;
}

static void qp_bytearray_releasebuffer(JSContext *ctx, QPBuffer *view)
{
    if (qp_bk_kind(view->obj) == QP_BK_BYTEARRAY) {
        QPBytesObject *b = QP_BYTES_OBJ(view->obj);
        if (b->exports > 0)
            b->exports--;
    }
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* _getbytevalue: "byte must be in range(0, 256)" */
static int qp_bytearray_getbytevalue(JSContext *ctx, JSValueConst arg, int *value)
{
    JSValue iv = qp_index(ctx, arg);
    int64_t v;
    int overflow;
    if (JS_IsException(iv))
        return -1;
    qp_long_as_int64_overflow(iv, &v, &overflow);
    JS_FreeValue(ctx, iv);
    if (overflow || v < 0 || v >= 256) {
        qp_raise_value_error(ctx, "byte must be in range(0, 256)");
        return -1;
    }
    *value = (int)v;
    return 0;
}

/* bytearray_setslice_linear: replace [lo, hi) by bytes[0..bytes_len). */
static int qp_bytearray_setslice_linear(JSContext *ctx, JSValueConst self, Py_ssize_t lo,
                                        Py_ssize_t hi, const uint8_t *bytes, Py_ssize_t bytes_len)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t avail = hi - lo, growth = bytes_len - avail, size = b->len;
    int res = 0;

    if (growth < 0) {
        if (!qp_bytearray_canresize(ctx, b))
            return -1;
        memmove(b->data + lo + bytes_len, b->data + hi, size - hi);
        if (qp_bytearray_resize(ctx, self, size + growth) < 0) {
            b->len = size + growth;
            b->data[b->len] = '\0';
            res = -1;
        }
    } else if (growth > 0) {
        if (size > PY_SSIZE_T_MAX - 1 - growth) {
            qp_raise_memory_error(ctx);
            return -1;
        }
        if (qp_bytearray_resize(ctx, self, size + growth) < 0)
            return -1;
        memmove(b->data + lo + bytes_len, b->data + hi, size - hi);
    }
    if (bytes_len > 0)
        memmove(b->data + lo, bytes, bytes_len);
    return res;
}

/* bytearray_setslice (values QP_NULL = delete) */
static int qp_bytearray_setslice(JSContext *ctx, JSValueConst self, Py_ssize_t lo, Py_ssize_t hi,
                                 JSValueConst values)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    QPBuffer vbytes;
    const uint8_t *bytes = NULL;
    Py_ssize_t needed = 0;
    int res;

    vbytes.obj = JS_UNDEFINED;
    if (!QP_IS_NULL(values) && JS_VALUE_GET_TAG(values) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(values) == JS_VALUE_GET_PTR(self)) {
        JSValue copy = qp_bytearray_new(ctx, b->data, b->len);
        if (JS_IsException(copy))
            return -1;
        res = qp_bytearray_setslice(ctx, self, lo, hi, copy);
        JS_FreeValue(ctx, copy);
        return res;
    }
    if (!QP_IS_NULL(values)) {
        if (qp_buffer_get(ctx, values, &vbytes, QP_BUF_SIMPLE) < 0) {
            JSValue n;
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            n = qp_bm_tpname(ctx, values);
            if (!JS_IsException(n)) {
                qp_raise_type_error(ctx, "can't set bytearray slice from %U", n);
                JS_FreeValue(ctx, n);
            }
            return -1;
        }
        needed = vbytes.len;
        bytes = vbytes.buf;
    }
    if (lo < 0)
        lo = 0;
    else if (lo > b->len)
        lo = b->len;
    if (hi < lo)
        hi = lo;
    if (hi > b->len)
        hi = b->len;
    res = qp_bytearray_setslice_linear(ctx, self, lo, hi, bytes, needed);
    qp_buffer_release(ctx, &vbytes);
    return res;
}

/* ------------------------------------------------------------------------ */
/* slots                                                                     */
/* ------------------------------------------------------------------------ */

static JSValue qp_bytearray_tp_repr(JSContext *ctx, JSValueConst self)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    QPType *t = qp_type_of(ctx, self);
    JSValue r, br, cls;
    const char *dot;

    br = qp_bytes_repr_cls(ctx, b->data ? b->data : qp_bytes_empty_data, b->len, 1, "bytearray");
    if (JS_IsException(br))
        return br;
    /* _PyType_Name: tp_name after the last dot */
    if (t->tp_name) {
        dot = strrchr(t->tp_name, '.');
        cls = qp_str_from_cstr(ctx, dot ? dot + 1 : t->tp_name);
    } else {
        cls = qp_str_from_atom(ctx, t->name);
    }
    if (JS_IsException(cls)) {
        JS_FreeValue(ctx, br);
        return cls;
    }
    r = qp_str_from_format(ctx, "%U(%U)", cls, br);
    JS_FreeValue(ctx, cls);
    JS_FreeValue(ctx, br);
    return r;
}

static JSValue qp_bytearray_tp_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other, int op)
{
    QPBuffer sv, ov;
    Py_ssize_t ssize, osize, minsize;
    int cmp;
    JSValue r;

    if (!qp_buffer_check(ctx, self) || !qp_buffer_check(ctx, other))
        return qp_not_implemented(ctx);
    if (qp_buffer_get(ctx, self, &sv, QP_BUF_SIMPLE) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return qp_not_implemented(ctx);
    }
    if (qp_buffer_get(ctx, other, &ov, QP_BUF_SIMPLE) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        qp_buffer_release(ctx, &sv);
        return qp_not_implemented(ctx);
    }
    ssize = sv.len;
    osize = ov.len;
    if (ssize != osize && (op == QP_CMP_EQ || op == QP_CMP_NE)) {
        r = JS_NewBool(ctx, op == QP_CMP_NE);
    } else {
        minsize = ssize < osize ? ssize : osize;
        cmp = minsize ? memcmp(sv.buf, ov.buf, minsize) : 0;
        if (cmp != 0)
            r = qp_bm_cmp_result(cmp, op);
        else
            r = qp_bm_cmp_result(ssize < osize ? -1 : ssize > osize, op);
    }
    qp_buffer_release(ctx, &sv);
    qp_buffer_release(ctx, &ov);
    return r;
}

static Py_ssize_t qp_bytearray_sq_length(JSContext *ctx, JSValueConst self)
{
    return QP_BYTES_OBJ(self)->len;
}

static JSValue qp_bytearray_sq_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (i < 0 || i >= b->len)
        return qp_raise_index_error(ctx, "bytearray index out of range");
    return JS_NewInt32(ctx, b->data[i]);
}

static JSValue qp_bytearray_sq_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return qp_bm_concat(ctx, a, b, 1);
}

static JSValue qp_bytearray_sq_repeat(JSContext *ctx, JSValueConst self, Py_ssize_t count)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t mysize = b->len, size;
    JSValue r;
    if (count < 0)
        count = 0;
    if (count > 0 && mysize > (PY_SSIZE_T_MAX - 1) / count)
        return qp_raise_memory_error(ctx);
    size = mysize * count;
    r = qp_bytearray_new(ctx, NULL, size);
    if (JS_IsException(r) || size == 0)
        return r;
    qp_bm_repeat_fill(QP_BYTES_OBJ(r)->data, size, b->data, mysize);
    return r;
}

static JSValue qp_bytearray_sq_inplace_concat(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    QPBuffer vo;
    Py_ssize_t size;
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (qp_buffer_get(ctx, other, &vo, QP_BUF_SIMPLE) < 0) {
        JSValue no, ns;
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        no = qp_bm_tpname(ctx, other);
        ns = qp_bm_tpname(ctx, self);
        if (!JS_IsException(no) && !JS_IsException(ns))
            qp_raise_type_error(ctx, "can't concat %U to %U", no, ns);
        JS_FreeValue(ctx, no);
        JS_FreeValue(ctx, ns);
        return JS_EXCEPTION;
    }
    size = b->len;
    if (size > PY_SSIZE_T_MAX - 1 - vo.len) {
        qp_buffer_release(ctx, &vo);
        return qp_raise_memory_error(ctx);
    }
    if (qp_bytearray_resize(ctx, self, size + vo.len) < 0) {
        qp_buffer_release(ctx, &vo);
        return JS_EXCEPTION;
    }
    if (vo.len)
        memmove(b->data + size, vo.buf, vo.len);
    qp_buffer_release(ctx, &vo);
    return JS_DupValue(ctx, self);
}

static JSValue qp_bytearray_sq_inplace_repeat(JSContext *ctx, JSValueConst self, Py_ssize_t count)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t mysize, size;
    if (count < 0)
        count = 0;
    else if (count == 1)
        return JS_DupValue(ctx, self);
    mysize = b->len;
    if (count > 0 && mysize > (PY_SSIZE_T_MAX - 1) / count)
        return qp_raise_memory_error(ctx);
    size = mysize * count;
    if (qp_bytearray_resize(ctx, self, size) < 0)
        return JS_EXCEPTION;
    if (size)
        qp_bm_repeat_fill(b->data, size, b->data, mysize);
    return JS_DupValue(ctx, self);
}

static int qp_bytearray_sq_ass_item(JSContext *ctx, JSValueConst self, Py_ssize_t i, JSValueConst value)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    int ival = -1;
    if (!QP_IS_NULL(value) && qp_bytearray_getbytevalue(ctx, value, &ival) < 0)
        return -1;
    if (i < 0)
        i += b->len;
    if (i < 0 || i >= b->len) {
        qp_raise_index_error(ctx, "bytearray index out of range");
        return -1;
    }
    if (QP_IS_NULL(value))
        return qp_bytearray_setslice(ctx, self, i, i + 1, QP_NULL);
    b->data[i] = (uint8_t)ival;
    return 0;
}

static JSValue qp_bytearray_mp_subscript(JSContext *ctx, JSValueConst self, JSValueConst index)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    if (qp_index_check(ctx, index)) {
        Py_ssize_t i;
        if (qp_as_ssize(ctx, index, QP_EXC(ctx, IndexError), &i) < 0)
            return JS_EXCEPTION;
        if (i < 0)
            i += b->len;
        if (i < 0 || i >= b->len)
            return qp_raise_index_error(ctx, "bytearray index out of range");
        return JS_NewInt32(ctx, b->data[i]);
    }
    if (qp_slice_check(index)) {
        Py_ssize_t start, stop, step, slicelength;
        if (qp_slice_unpack(ctx, index, &start, &stop, &step) < 0)
            return JS_EXCEPTION;
        slicelength = qp_slice_adjust_indices(b->len, &start, &stop, step);
        return qp_bm_slice(ctx, 1, b->data, start, step, slicelength);
    }
    return qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                              "bytearray indices must be integers or slices, not %U", index);
}

/* bytearray_ass_subscript (values QP_NULL = delete) */
static int qp_bytearray_mp_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst index,
                                         JSValueConst values)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t start, stop, step, slicelen, needed;
    const uint8_t *bytes;

    if (qp_index_check(ctx, index)) {
        Py_ssize_t i;
        int ival = -1;
        if (qp_as_ssize(ctx, index, QP_EXC(ctx, IndexError), &i) < 0)
            return -1;
        if (!QP_IS_NULL(values) && qp_bytearray_getbytevalue(ctx, values, &ival) < 0)
            return -1;
        if (i < 0)
            i += b->len;
        if (i < 0 || i >= b->len) {
            qp_raise_index_error(ctx, "bytearray index out of range");
            return -1;
        }
        if (QP_IS_NULL(values)) {
            start = i;
            stop = i + 1;
            step = 1;
            slicelen = 1;
        } else {
            b->data[i] = (uint8_t)ival;
            return 0;
        }
    } else if (qp_slice_check(index)) {
        if (qp_slice_unpack(ctx, index, &start, &stop, &step) < 0)
            return -1;
        slicelen = qp_slice_adjust_indices(b->len, &start, &stop, step);
    } else {
        qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                           "bytearray indices must be integers or slices, not %U", index);
        return -1;
    }

    if (QP_IS_NULL(values)) {
        bytes = NULL;
        needed = 0;
    } else if ((JS_VALUE_GET_TAG(values) == JS_TAG_OBJECT &&
                JS_VALUE_GET_PTR(values) == JS_VALUE_GET_PTR(self)) ||
               qp_bk_kind(values) != QP_BK_BYTEARRAY) {
        JSValue copy;
        int err;
        if (qp_bm_number_check(ctx, values) || qp_str_check(ctx, values)) {
            qp_raise_type_error(ctx, "can assign only bytes, buffers, or iterables of ints in range(0, 256)");
            return -1;
        }
        copy = qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, bytearray)), values);
        if (JS_IsException(copy))
            return -1;
        err = qp_bytearray_mp_ass_subscript(ctx, self, index, copy);
        JS_FreeValue(ctx, copy);
        return err;
    } else {
        QPBytesObject *vb = QP_BYTES_OBJ(values);
        bytes = vb->data;
        needed = vb->len;
    }

    if ((step < 0 && start < stop) || (step > 0 && start > stop))
        stop = start;

    if (step == 1)
        return qp_bytearray_setslice_linear(ctx, self, start, stop, bytes, needed);
    if (needed == 0) {
        size_t cur;
        Py_ssize_t i;
        uint8_t *buf = b->data;
        if (!qp_bytearray_canresize(ctx, b))
            return -1;
        if (slicelen == 0)
            return 0;
        if (step < 0) {
            stop = start + 1;
            start = stop + step * (slicelen - 1) - 1;
            step = -step;
        }
        for (cur = start, i = 0; i < slicelen; cur += step, i++) {
            Py_ssize_t lim = step - 1;
            if (cur + step >= (size_t)b->len)
                lim = b->len - cur - 1;
            memmove(buf + cur - i, buf + cur + 1, lim);
        }
        cur = start + (size_t)slicelen * step;
        if (cur < (size_t)b->len)
            memmove(buf + cur - slicelen, buf + cur, b->len - cur);
        return qp_bytearray_resize(ctx, self, b->len - slicelen);
    } else {
        size_t cur;
        Py_ssize_t i;
        if (needed != slicelen) {
            qp_raise_value_error(ctx, "attempt to assign bytes of size %zd to extended slice of size %zd",
                                 needed, slicelen);
            return -1;
        }
        for (cur = start, i = 0; i < slicelen; cur += step, i++)
            b->data[cur] = bytes[i];
        return 0;
    }
}

static JSValue qp_bytearray_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    return qp_bytearray_alloc(ctx, subtype);
}

/* bytearray.__init__(source=b'', encoding=None, errors=None) */
static int qp_bytearray_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "source", "encoding", "errors", NULL };
    static const QPArgSpec spec = { "bytearray", kw, 0, 3, 0 };
    JSValueConst a[3];
    JSValueConst arg;
    const char *encoding = NULL, *errors = NULL;
    QPBytesObject *b;
    JSValue it;
    int ret = -1;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return -1;
    arg = a[0];
    if (!QP_IS_NULL(a[1])) {
        encoding = qp_bm_arg_cstr(ctx, "bytearray", "encoding", a[1]);
        if (!encoding)
            return -1;
    }
    if (!QP_IS_NULL(a[2])) {
        errors = qp_bm_arg_cstr(ctx, "bytearray", "errors", a[2]);
        if (!errors)
            goto done;
    }
    b = QP_BYTES_OBJ(self);
    if (!qp_bytearray_canresize(ctx, b))
        goto done;
    if (qp_bytearray_resize(ctx, self, 0) < 0)
        goto done;

    if (QP_IS_NULL(arg)) {
        if (encoding != NULL || errors != NULL) {
            qp_raise_type_error(ctx, encoding != NULL ? "encoding without a string argument" :
                                "errors without a string argument");
            goto done;
        }
        ret = 0;
        goto done;
    }

    if (qp_str_check(ctx, arg)) {
        JSValue s, encoded, r;
        if (encoding == NULL) {
            qp_raise_type_error(ctx, "string argument without an encoding");
            goto done;
        }
        s = qp_str_value(ctx, arg);
        if (JS_IsException(s))
            goto done;
        encoded = qp_str_encode(ctx, s, encoding, errors);
        JS_FreeValue(ctx, s);
        if (JS_IsException(encoded))
            goto done;
        r = qp_bytearray_sq_inplace_concat(ctx, self, encoded);
        JS_FreeValue(ctx, encoded);
        if (JS_IsException(r))
            goto done;
        JS_FreeValue(ctx, r);
        ret = 0;
        goto done;
    }

    if (encoding != NULL || errors != NULL) {
        qp_raise_type_error(ctx, encoding != NULL ? "encoding without a string argument" :
                            "errors without a string argument");
        goto done;
    }

    if (qp_index_check(ctx, arg)) {
        Py_ssize_t count;
        if (qp_as_ssize(ctx, arg, QP_EXC(ctx, OverflowError), &count) < 0) {
            if (!qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
                goto done;
        } else {
            if (count < 0) {
                qp_raise_value_error(ctx, "negative count");
                goto done;
            }
            if (count > 0) {
                if (qp_bytearray_resize(ctx, self, count) < 0)
                    goto done;
                memset(b->data, 0, count);
            }
            ret = 0;
            goto done;
        }
    }

    if (qp_buffer_check(ctx, arg)) {
        JSValue tmp;
        Py_ssize_t len;
        const uint8_t *d;
        if (qp_memoryview_check(arg) ||
            (JS_VALUE_GET_TAG(arg) == JS_TAG_OBJECT &&
             qp_bm_is_typed_array_class(JS_VALUE_GET_OBJ(arg)->class_id))) {
            tmp = qp_memoryview_to_bytes(ctx, arg, 0);
            if (JS_IsException(tmp))
                goto done;
        } else {
            QPBuffer view;
            if (qp_buffer_get(ctx, arg, &view, QP_BUF_SIMPLE) < 0)
                goto done;
            if (qp_bytearray_resize(ctx, self, view.len) < 0) {
                qp_buffer_release(ctx, &view);
                goto done;
            }
            if (view.len)
                memmove(b->data, view.buf, view.len);
            qp_buffer_release(ctx, &view);
            ret = 0;
            goto done;
        }
        d = qp_bytes_data(tmp, &len);
        if (qp_bytearray_resize(ctx, self, len) < 0) {
            JS_FreeValue(ctx, tmp);
            goto done;
        }
        if (len)
            memcpy(b->data, d, len);
        JS_FreeValue(ctx, tmp);
        ret = 0;
        goto done;
    }

    if (qp_list_check_exact(ctx, arg) || qp_tuple_check_exact(ctx, arg)) {
        Py_ssize_t size, i, n;
        JSValue *items;
        int is_tuple = qp_tuple_check_exact(ctx, arg), slow = 0;
        if (is_tuple) {
            items = QP_TUPLE_ITEMS(arg);
            size = QP_TUPLE_SIZE(arg);
        } else {
            items = qp_list_items(arg, &size);
            if (!items)
                slow = 1;
        }
        if (!slow) {
            for (i = 0; i < size; i++) {
                if (!qp_long_check_exact(items[i])) {
                    slow = 1;
                    break;
                }
            }
        }
        if (!slow) {
            if (qp_bytearray_resize(ctx, self, size) < 0)
                goto done;
            for (i = 0; i < size; i++) {
                int value;
                if (!is_tuple) {
                    items = qp_list_items(arg, &n);
                    if (!items || i >= n)
                        break;
                }
                if (qp_bytearray_getbytevalue(ctx, items[i], &value) < 0) {
                    qp_bytearray_resize(ctx, self, 0);
                    goto done;
                }
                b->data[i] = (uint8_t)value;
            }
            ret = 0;
            goto done;
        }
    }

    it = qp_iter(ctx, arg);
    if (JS_IsException(it)) {
        if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
            qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError),
                               "cannot convert '%U' object to bytearray", arg);
        goto done;
    }
    for (;;) {
        JSValue item = qp_next(ctx, it);
        int value, rc;
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, it);
            goto done;
        }
        rc = qp_bytearray_getbytevalue(ctx, item, &value);
        JS_FreeValue(ctx, item);
        if (rc < 0 || qp_bytearray_resize(ctx, self, b->len + 1) < 0) {
            JS_FreeValue(ctx, it);
            goto done;
        }
        b->data[b->len - 1] = (uint8_t)value;
    }
    JS_FreeValue(ctx, it);
    ret = 0;
done:
    if (encoding)
        JS_FreeCString(ctx, encoding);
    if (errors)
        JS_FreeCString(ctx, errors);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* methods                                                                   */
/* ------------------------------------------------------------------------ */

static JSValue qp_bytearray_append(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    int item;
    Py_ssize_t n;
    if (qp_bytearray_getbytevalue(ctx, args[0], &item) < 0)
        return JS_EXCEPTION;
    n = QP_BYTES_OBJ(self)->len;
    if (qp_bytearray_resize(ctx, self, n + 1) < 0)
        return JS_EXCEPTION;
    QP_BYTES_OBJ(self)->data[n] = (uint8_t)item;
    return JS_UNDEFINED;
}

static JSValue qp_bytearray_insert(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    Py_ssize_t index, n;
    int item;
    uint8_t *buf;
    if (qp_arg_check_positional(ctx, "insert", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_arg_ssize(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (qp_bytearray_getbytevalue(ctx, args[1], &item) < 0)
        return JS_EXCEPTION;
    n = QP_BYTES_OBJ(self)->len;
    if (qp_bytearray_resize(ctx, self, n + 1) < 0)
        return JS_EXCEPTION;
    buf = QP_BYTES_OBJ(self)->data;
    if (index < 0) {
        index += n;
        if (index < 0)
            index = 0;
    }
    if (index > n)
        index = n;
    memmove(buf + index + 1, buf + index, n - index);
    buf[index] = (uint8_t)item;
    return JS_UNDEFINED;
}

/* extend(iterable_of_ints, /) */
static JSValue qp_bytearray_extend(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    JSValueConst iterable = args[0];
    JSValue it, tmp;
    Py_ssize_t buf_size, len = 0;
    int rc;

    if (qp_buffer_check(ctx, iterable)) {
        Py_ssize_t n = QP_BYTES_OBJ(self)->len;
        if (qp_bytearray_setslice(ctx, self, n, n, iterable) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    it = qp_iter(ctx, iterable);
    if (JS_IsException(it)) {
        if (qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
            qp_bm_raise_tpname(ctx, QP_EXC(ctx, TypeError), "can't extend bytearray with %U", iterable);
        return JS_EXCEPTION;
    }
    buf_size = qp_length_hint(ctx, iterable, 32);
    if (buf_size < 0) {
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    tmp = qp_bytearray_new(ctx, NULL, buf_size);
    if (JS_IsException(tmp)) {
        JS_FreeValue(ctx, it);
        return tmp;
    }
    for (;;) {
        JSValue item = qp_next(ctx, it);
        int value;
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item))
            goto fail;
        if (qp_bytearray_getbytevalue(ctx, item, &value) < 0) {
            if (qp_str_check(ctx, iterable) && qp_bm_exc_clear_if(ctx, QP_EXC(ctx, TypeError)))
                qp_raise_type_error(ctx, "expected iterable of integers; got: 'str'");
            JS_FreeValue(ctx, item);
            goto fail;
        }
        JS_FreeValue(ctx, item);
        if (len >= buf_size) {
            Py_ssize_t addition;
            if (len >= PY_SSIZE_T_MAX - 1) {
                qp_raise_memory_error(ctx);
                goto fail;
            }
            addition = len ? len >> 1 : 1;
            if (addition > PY_SSIZE_T_MAX - 1 - len)
                buf_size = PY_SSIZE_T_MAX - 1;
            else
                buf_size = len + addition;
            if (qp_bytearray_resize(ctx, tmp, buf_size) < 0)
                goto fail;
        }
        QP_BYTES_OBJ(tmp)->data[len++] = (uint8_t)value;
    }
    JS_FreeValue(ctx, it);
    it = JS_UNDEFINED;
    if (qp_bytearray_resize(ctx, tmp, len) < 0)
        goto fail;
    {
        Py_ssize_t n = QP_BYTES_OBJ(self)->len;
        rc = qp_bytearray_setslice(ctx, self, n, n, tmp);
    }
    JS_FreeValue(ctx, tmp);
    if (rc < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, tmp);
    return JS_EXCEPTION;
}

static JSValue qp_bytearray_pop(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    Py_ssize_t index = -1, n;
    QPBytesObject *b;
    int value;
    if (qp_arg_check_positional(ctx, "pop", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs > 0 && qp_arg_ssize(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    b = QP_BYTES_OBJ(self);
    n = b->len;
    if (n == 0)
        return qp_raise_index_error(ctx, "pop from empty bytearray");
    if (index < 0)
        index += n;
    if (index < 0 || index >= n)
        return qp_raise_index_error(ctx, "pop index out of range");
    if (!qp_bytearray_canresize(ctx, b))
        return JS_EXCEPTION;
    value = b->data[index];
    memmove(b->data + index, b->data + index + 1, n - index - 1);
    if (qp_bytearray_resize(ctx, self, n - 1) < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, value);
}

static JSValue qp_bytearray_remove(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                   JSValueConst kwnames, int magic)
{
    int value;
    QPBytesObject *b;
    Py_ssize_t n;
    const uint8_t *p;
    if (qp_bytearray_getbytevalue(ctx, args[0], &value) < 0)
        return JS_EXCEPTION;
    b = QP_BYTES_OBJ(self);
    n = b->len;
    p = n ? memchr(b->data, value, n) : NULL;
    if (!p)
        return qp_raise_value_error(ctx, "value not found in bytearray");
    if (!qp_bytearray_canresize(ctx, b))
        return JS_EXCEPTION;
    memmove(b->data + (p - b->data), p + 1, n - (p - b->data) - 1);
    if (qp_bytearray_resize(ctx, self, n - 1) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_bytearray_clear(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                  JSValueConst kwnames, int magic)
{
    if (qp_bytearray_resize(ctx, self, 0) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_bytearray_copy(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                 JSValueConst kwnames, int magic)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    return qp_bytearray_new(ctx, b->data, b->len);
}

static JSValue qp_bytearray_reverse(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                    JSValueConst kwnames, int magic)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t i, j;
    for (i = 0, j = b->len - 1; i < j; i++, j--) {
        uint8_t t = b->data[i];
        b->data[i] = b->data[j];
        b->data[j] = t;
    }
    return JS_UNDEFINED;
}

static JSValue qp_bytearray_resize_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t size, start;
    if (qp_arg_ssize(ctx, args[0], &size) < 0)
        return JS_EXCEPTION;
    start = QP_BYTES_OBJ(self)->len;
    if (qp_bytearray_resize(ctx, self, size) < 0)
        return JS_EXCEPTION;
    if (size > start)
        memset(QP_BYTES_OBJ(self)->data + start, 0, size - start);
    return JS_UNDEFINED;
}

/* take_bytes(n=None, /) */
static JSValue qp_bytearray_take_bytes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPBytesObject *b = QP_BYTES_OBJ(self);
    Py_ssize_t to_take, size;
    JSValue ret;

    if (qp_arg_check_positional(ctx, "take_bytes", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    size = b->len;
    if (nargs == 0 || JS_IsUndefined(args[0]) || JS_IsNull(args[0])) {
        to_take = size;
    } else if (qp_index_check(ctx, args[0])) {
        if (qp_as_ssize(ctx, args[0], QP_EXC(ctx, IndexError), &to_take) < 0)
            return JS_EXCEPTION;
        size = b->len;
        if (to_take < 0)
            to_take += size;
    } else {
        return qp_raise_type_error(ctx, "n must be an integer or None");
    }
    if (to_take < 0 || to_take > size)
        return qp_raise_index_error(ctx, "can't take %zd bytes outside size %zd", to_take, size);
    if (!qp_bytearray_canresize(ctx, b))
        return JS_EXCEPTION;
    if (to_take == 0 || size == 0)
        return qp_bytes_new(ctx, NULL, 0);
    ret = qp_bytes_new(ctx, b->data, to_take);
    if (JS_IsException(ret))
        return ret;
    if (to_take == size) {
        js_free(ctx, b->data);
        b->data = NULL;
        b->len = 0;
        b->alloc = 0;
    } else if (to_take < size - to_take) {
        /* CPython advances ob_start: the allocation is unchanged */
        Py_ssize_t remaining = size - to_take;
        memmove(b->data, b->data + to_take, remaining);
        b->data[remaining] = '\0';
        b->len = remaining;
    } else {
        /* CPython keeps the remaining bytes in a new exact-size buffer */
        Py_ssize_t remaining = size - to_take;
        uint8_t *p = js_malloc(ctx, remaining + 1);
        if (!p) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        memcpy(p, b->data + to_take, remaining);
        p[remaining] = '\0';
        js_free(ctx, b->data);
        b->data = p;
        b->len = remaining;
        b->alloc = remaining;
    }
    return ret;
}

static JSValue qp_bytearray_alloc_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t alloc = QP_BYTES_OBJ(self)->alloc;
    if (alloc > 0)
        alloc += 33; /* _PyBytesObject_SIZE */
    return qp_long_from_int64(ctx, alloc);
}

static JSValue qp_bytearray_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t alloc = QP_BYTES_OBJ(self)->alloc;
    Py_ssize_t res = 64; /* sizeof(PyByteArrayObject) */
    if (alloc > 0)
        res += 33 + alloc;
    return qp_long_from_int64(ctx, res);
}

/* _common_reduce */
static JSValue qp_bytearray_common_reduce(JSContext *ctx, JSValueConst self, int proto)
{
    JSValue state, args, items[3], t;
    QPBytesObject *b;

    state = qp_call_method(ctx, self, QP_ATOM(ctx, __getstate__), NULL, 0, QP_NULL);
    if (JS_IsException(state))
        return state;
    b = QP_BYTES_OBJ(self);
    if (b->len == 0) {
        args = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    } else if (proto < 3) {
        JSValue pair[2];
        pair[0] = qp_str_decode(ctx, (const char *)b->data, b->len, "latin-1", "strict");
        if (JS_IsException(pair[0])) {
            JS_FreeValue(ctx, state);
            return JS_EXCEPTION;
        }
        pair[1] = qp_str_from_cstr(ctx, "latin-1");
        if (JS_IsException(pair[1])) {
            JS_FreeValue(ctx, pair[0]);
            JS_FreeValue(ctx, state);
            return JS_EXCEPTION;
        }
        args = qp_tuple_new_steal(ctx, 2, pair);
    } else {
        JSValue by = qp_bytes_new(ctx, b->data, b->len);
        if (JS_IsException(by)) {
            JS_FreeValue(ctx, state);
            return by;
        }
        args = qp_tuple_new_steal(ctx, 1, &by);
    }
    if (JS_IsException(args)) {
        JS_FreeValue(ctx, state);
        return args;
    }
    t = QP_TYPE_VALUE(qp_type_of(ctx, self));
    items[0] = JS_DupValue(ctx, t);
    items[1] = args;
    items[2] = state;
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_bytearray_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_bytearray_common_reduce(ctx, self, 2);
}

static JSValue qp_bytearray_reduce_ex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    int proto = 0;
    if (qp_arg_check_positional(ctx, "__reduce_ex__", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs > 0 && qp_arg_int(ctx, args[0], &proto) < 0)
        return JS_EXCEPTION;
    return qp_bytearray_common_reduce(ctx, self, proto);
}

static const QPMethodDef qp_bytearray_methods[] = {
    { "__alloc__", qp_bytearray_alloc_meth, QP_METH_NOARGS, 0,
      "__alloc__($self, /)\n--\n\nB.__alloc__() -> int\n\nReturn the number of bytes actually allocated." },
    { "__reduce__", qp_bytearray_reduce, QP_METH_NOARGS, 0,
      "__reduce__($self, /)\n--\n\nReturn state information for pickling." },
    { "__reduce_ex__", qp_bytearray_reduce_ex, QP_METH_FASTCALL, 0,
      "__reduce_ex__($self, proto=0, /)\n--\n\nReturn state information for pickling." },
    { "__sizeof__", qp_bytearray_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nReturns the size of the bytearray object in memory, in bytes." },
    { "append", qp_bytearray_append, QP_METH_O, 0,
      "append($self, item, /)\n--\n\nAppend a single item to the end of the bytearray.\n\n  item\n    The item to be appended." },
    { "clear", qp_bytearray_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nRemove all items from the bytearray." },
    { "copy", qp_bytearray_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a copy of B." },
    { "extend", qp_bytearray_extend, QP_METH_O, 0,
      "extend($self, iterable_of_ints, /)\n--\n\nAppend all the items from the iterator or sequence to the end of the bytearray.\n\n  iterable_of_ints\n    The iterable of items to append." },
    { "fromhex", qp_bm_fromhex, QP_METH_O | QP_METH_CLASS, 1,
      "fromhex($type, string, /)\n--\n\nCreate a bytearray object from a string of hexadecimal numbers.\n\n"
      "Spaces between two numbers are accepted.\nExample: bytearray.fromhex('B9 01EF') -> bytearray(b'\\\\xb9\\\\x01\\\\xef')" },
    { "insert", qp_bytearray_insert, QP_METH_FASTCALL, 0,
      "insert($self, index, item, /)\n--\n\nInsert a single item into the bytearray before the given index.\n\n  index\n    The index where the value is to be inserted.\n  item\n    The item to be inserted." },
    { "pop", qp_bytearray_pop, QP_METH_FASTCALL, 0,
      "pop($self, index=-1, /)\n--\n\nRemove and return a single item from B.\n\n  index\n    The index from where to remove the item.\n    -1 (the default value) means remove the last item.\n\nIf no index argument is given, will pop the last item." },
    { "remove", qp_bytearray_remove, QP_METH_O, 0,
      "remove($self, value, /)\n--\n\nRemove the first occurrence of a value in the bytearray.\n\n  value\n    The value to remove." },
    { "resize", qp_bytearray_resize_meth, QP_METH_O, 0,
      "resize($self, size, /)\n--\n\nResize the internal buffer of bytearray to len.\n\n  size\n    New size to resize to.." },
    { "reverse", qp_bytearray_reverse, QP_METH_NOARGS, 0,
      "reverse($self, /)\n--\n\nReverse the order of the values in B in place." },
    { "take_bytes", qp_bytearray_take_bytes, QP_METH_FASTCALL, 0,
      "take_bytes($self, n=None, /)\n--\n\nTake *n* bytes from the bytearray and return them as a bytes object.\n\n  n\n    Bytes to take, negative indexes from end. None indicates all bytes." },
    QP_BM_SHARED_METHODS,
    { NULL, NULL, 0, 0, NULL },
};

static const QPTypeSpec qp_bytearray_spec = {
    .name = "bytearray",
    .doc = "bytearray(iterable_of_ints) -> bytearray\n"
           "bytearray(string, encoding[, errors]) -> bytearray\n"
           "bytearray(bytes_or_buffer) -> mutable copy of bytes_or_buffer\n"
           "bytearray(int) -> bytes array of size given by the parameter initialized with null bytes\n"
           "bytearray() -> empty bytes array\n\n"
           "Construct a mutable bytearray object from:\n"
           "  - an iterable yielding integers in range(256)\n"
           "  - a text string encoded using the specified encoding\n"
           "  - a bytes or a buffer object\n"
           "  - any object implementing the buffer API.\n"
           "  - an integer",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BYTEARRAY,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_bytearray_tp_repr,
        .tp_str = qp_bytearray_tp_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_richcompare = qp_bytearray_tp_richcompare,
        .tp_iter = qp_bytes_tp_iter,
        .tp_iteritem = qp_bytes_tp_iteritem,
        .tp_init = qp_bytearray_tp_init,
        .tp_new = qp_bytearray_tp_new,
        .nb = { .nb_remainder = qp_bm_mod },
        .sq = {
            .sq_length = qp_bytearray_sq_length,
            .sq_concat = qp_bytearray_sq_concat,
            .sq_repeat = qp_bytearray_sq_repeat,
            .sq_item = qp_bytearray_sq_item,
            .sq_ass_item = qp_bytearray_sq_ass_item,
            .sq_contains = qp_bm_contains,
            .sq_inplace_concat = qp_bytearray_sq_inplace_concat,
            .sq_inplace_repeat = qp_bytearray_sq_inplace_repeat,
        },
        .mp = {
            .mp_length = qp_bytearray_sq_length,
            .mp_subscript = qp_bytearray_mp_subscript,
            .mp_ass_subscript = qp_bytearray_mp_ass_subscript,
        },
        .bf = {
            .bf_getbuffer = qp_bytearray_getbuffer,
            .bf_releasebuffer = qp_bytearray_releasebuffer,
        },
    },
    .methods = qp_bytearray_methods,
};

static int qp_init_bytearray_type(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, bytearray), &qp_bytearray_spec);
}
