/*
 * dquickts Python C API (plan D5, unit C): int, bool, float, complex.
 *
 * Mirror kinds LONG / FLOAT / COMPLEX (python/capi.h), the static small
 * ints and the bool singletons, longobject.h / cpython/longobject.h /
 * cpython/longintrepr.h / pycore_long.h, floatobject.h (+ Pack/Unpack),
 * complexobject.h (+ _Py_c_*), boolobject.h and pystrtod.h.
 *
 * Derived from CPython 3.15.2 Objects/longobject.c, Objects/floatobject.c,
 * Objects/complexobject.c, Objects/boolobject.c and Python/pystrtod.c
 * (PSF-2.0).
 *
 * Representation.  A LONG mirror carries CPython's PyLongObject layout
 * (30-bit digits, lv_tag) filled when the mirror is created, so the
 * inline helpers of the headers (_PyLong_IsCompact, _PyLong_CompactValue,
 * the pycore_long.h tag tests) and every digit-based routine below work on
 * it directly.  Ints made by C (PyLong_From*, _PyLong_FromByteArray,
 * PyLongWriter) are C-primary: they get a JS value only when they cross
 * (kind_ops.to_js).  Values -5..1024 are the static immortal
 * _PyQp_SmallInts (CPython's _PyLong_SMALL_INTS).  FLOAT mirrors carry
 * ob_fval (C-primary allowed), COMPLEX mirrors carry cval of their
 * JS_CLASS_PY_COMPLEX object.  Arithmetic that is not digit shuffling
 * (GCD, DivmodNear, shifts, formatting, parsing) runs on the JS ints of
 * the M2 runtime.
 */

#include <float.h>

/* ------------------------------------------------------------------------ */
/* Constants of the CPython 3.15 representation                              */
/* ------------------------------------------------------------------------ */

#define QP_CAPI_NSMALLNEGINTS 5     /* _PY_NSMALLNEGINTS (3.15) */
#define QP_CAPI_NSMALLPOSINTS 1025  /* _PY_NSMALLPOSINTS (3.15) */
#if defined(_PY_NSMALLNEGINTS) && (_PY_NSMALLNEGINTS != QP_CAPI_NSMALLNEGINTS || \
                                   _PY_NSMALLPOSINTS != QP_CAPI_NSMALLPOSINTS)
#error "capi_number.c: small int range differs from the pycore shim"
#endif

/* pycore_long.h tag layout */
#define QP_CAPI_LSIGN_MASK      3
#define QP_CAPI_LSIGN_ZERO      1
#define QP_CAPI_LSIGN_NEGATIVE  2
#define QP_CAPI_LNON_SIZE_BITS  3
#define QP_CAPI_LIMMORTAL_BIT   (1 << 2)
#define QP_CAPI_LTAG(sign, size) \
    ((uintptr_t)(1 - (sign)) | ((uintptr_t)(size) << QP_CAPI_LNON_SIZE_BITS))

/* object size of an int with n digits (zero keeps one digit, as CPython) */
#define QP_CAPI_LONG_SIZE(n) \
    (offsetof(PyLongObject, long_value.ob_digit) + (size_t)((n) > 0 ? (n) : 1) * sizeof(digit))
#define QP_CAPI_LONG_MAX_DIGITS \
    ((PY_SSIZE_T_MAX - offsetof(PyLongObject, long_value.ob_digit)) / sizeof(digit))

#define QP_CAPI_ABS_LONG_MIN  (0 - (unsigned long)LONG_MIN)
#define QP_CAPI_ABS_SSIZE_MIN (0 - (size_t)PY_SSIZE_T_MIN)
#define QP_CAPI_ABS_LLONG_MIN (0 - (unsigned long long)LLONG_MIN)

#if JS_LIMB_BITS == 64
typedef js_dlimb_t qp_capi_acc_t;
#else
typedef uint64_t qp_capi_acc_t;
#endif

static inline Py_ssize_t qp_capi_long_ndigits(const PyLongObject *op)
{
    return (Py_ssize_t)(op->long_value.lv_tag >> QP_CAPI_LNON_SIZE_BITS);
}

static inline int qp_capi_long_is_negative(const PyLongObject *op)
{
    return (op->long_value.lv_tag & QP_CAPI_LSIGN_MASK) == QP_CAPI_LSIGN_NEGATIVE;
}

static inline int qp_capi_long_is_zero(const PyLongObject *op)
{
    return (op->long_value.lv_tag & QP_CAPI_LSIGN_MASK) == QP_CAPI_LSIGN_ZERO;
}

/* -1, 0, 1 */
static inline int qp_capi_long_sign(const PyLongObject *op)
{
    return 1 - (int)(op->long_value.lv_tag & QP_CAPI_LSIGN_MASK);
}

static inline int qp_capi_long_is_compact(const PyLongObject *op)
{
    return op->long_value.lv_tag < (2 << QP_CAPI_LNON_SIZE_BITS);
}

static inline int qp_capi_long_is_nonneg_compact(const PyLongObject *op)
{
    return (op->long_value.lv_tag & ~(uintptr_t)QP_CAPI_LIMMORTAL_BIT) <= (1 << QP_CAPI_LNON_SIZE_BITS);
}

static inline Py_ssize_t qp_capi_long_compact_value(const PyLongObject *op)
{
    return qp_capi_long_sign(op) * (Py_ssize_t)op->long_value.ob_digit[0];
}

static inline void qp_capi_long_set_tag(PyLongObject *op, int sign, Py_ssize_t size)
{
    op->long_value.lv_tag = QP_CAPI_LTAG(sign, size);
}

static inline void qp_capi_long_flip_sign(PyLongObject *op)
{
    uintptr_t flipped = 2 - (op->long_value.lv_tag & QP_CAPI_LSIGN_MASK);
    op->long_value.lv_tag = (op->long_value.lv_tag & ~(uintptr_t)((1 << QP_CAPI_LNON_SIZE_BITS) - 1)) | flipped;
}

static inline int qp_capi_num_bit_length(unsigned long x)
{
    return x ? (int)(8 * sizeof(unsigned long)) - __builtin_clzl(x) : 0;
}

/* defined in capi_abstract.c (pycore_abstract.h) */
PyAPI_FUNC(PyObject *) _PyNumber_Index(PyObject *item);

/* CPython null_error() */
static void *qp_capi_num_null_error(void)
{
    if (!PyErr_Occurred())
        PyErr_SetString(PyExc_SystemError, "null argument to internal routine");
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Static objects: small ints, bools                                         */
/* ------------------------------------------------------------------------ */

PyAPI_DATA(PyLongObject) _PyQp_SmallInts[QP_CAPI_NSMALLNEGINTS + QP_CAPI_NSMALLPOSINTS];
PyLongObject _PyQp_SmallInts[QP_CAPI_NSMALLNEGINTS + QP_CAPI_NSMALLPOSINTS];
static int qp_capi_small_ints_ready;

PyLongObject _Py_FalseStruct = {
    PyObject_HEAD_INIT(&PyBool_Type)
    { QP_CAPI_LTAG(0, 0), { 0 } }
};

PyLongObject _Py_TrueStruct = {
    PyObject_HEAD_INIT(&PyBool_Type)
    { QP_CAPI_LTAG(1, 1), { 1 } }
};

static void qp_capi_small_ints_init(void)
{
    static const PyLongObject tmpl = {
        PyObject_HEAD_INIT(&PyLong_Type)
        { 0, { 0 } }
    };
    int i;

    if (qp_capi_small_ints_ready)
        return;
    for (i = 0; i < QP_CAPI_NSMALLNEGINTS + QP_CAPI_NSMALLPOSINTS; i++) {
        PyLongObject *op = &_PyQp_SmallInts[i];
        long ival = i - QP_CAPI_NSMALLNEGINTS;
        memcpy(op, &tmpl, sizeof(tmpl));
        op->long_value.lv_tag =
            QP_CAPI_LTAG(ival == 0 ? 0 : (ival < 0 ? -1 : 1), ival == 0 ? 0 : 1) | QP_CAPI_LIMMORTAL_BIT;
        op->long_value.ob_digit[0] = (digit)(ival < 0 ? -ival : ival);
    }
    qp_capi_small_ints_ready = 1;
}

static PyObject *qp_capi_small_int(long v)
{
    if (v < -QP_CAPI_NSMALLNEGINTS || v >= QP_CAPI_NSMALLPOSINTS)
        return NULL;
    qp_capi_small_ints_init();
    return (PyObject *)&_PyQp_SmallInts[v + QP_CAPI_NSMALLNEGINTS];
}

/* ------------------------------------------------------------------------ */
/* Allocation of C-primary ints                                              */
/* ------------------------------------------------------------------------ */

/* long_alloc(): positive (or zero) int with room for ndigits digits. */
static PyLongObject *qp_capi_long_alloc(Py_ssize_t ndigits)
{
    PyLongObject *v;

    if ((size_t)ndigits > QP_CAPI_LONG_MAX_DIGITS) {
        PyErr_SetString(PyExc_OverflowError, "too many digits in integer");
        return NULL;
    }
    v = (PyLongObject *)qp_capi_alloc(&PyLong_Type, QP_CAPI_LONG_SIZE(ndigits), QP_CAPI_K_LONG);
    if (!v)
        return NULL;
    qp_capi_long_set_tag(v, ndigits != 0, ndigits);
    return v;
}

/* long_normalize(): drop leading zero digits. */
static PyLongObject *qp_capi_long_normalize(PyLongObject *v)
{
    Py_ssize_t j = qp_capi_long_ndigits(v), i = j;

    while (i > 0 && v->long_value.ob_digit[i - 1] == 0)
        --i;
    if (i != j) {
        if (i == 0) {
            qp_capi_long_set_tag(v, 0, 0);
            v->long_value.ob_digit[0] = 0;
        } else {
            v->long_value.lv_tag = ((uintptr_t)i << QP_CAPI_LNON_SIZE_BITS) |
                                   (v->long_value.lv_tag & QP_CAPI_LSIGN_MASK);
        }
    }
    return v;
}

/* maybe_small_long(): consumes v, returns the small int singleton when the
   value is in range. */
static PyObject *qp_capi_long_maybe_small(PyLongObject *v)
{
    if (v && qp_capi_long_is_compact(v)) {
        Py_ssize_t ival = qp_capi_long_compact_value(v);
        PyObject *s = qp_capi_small_int((long)ival);
        if (s) {
            Py_DECREF(v);
            return s;
        }
    }
    return (PyObject *)v;
}

/* int from a magnitude and a sign (PYLONG_FROM_INT / PYLONG_FROM_UINT). */
static PyObject *qp_capi_long_from_u64(uint64_t m, int negative)
{
    PyLongObject *v;
    Py_ssize_t ndigits = 0;
    uint64_t t;
    digit *p;

    if (!negative && m < QP_CAPI_NSMALLPOSINTS)
        return qp_capi_small_int((long)m);
    if (negative && m <= QP_CAPI_NSMALLNEGINTS)
        return qp_capi_small_int(-(long)m);
    for (t = m; t; t >>= PyLong_SHIFT)
        ndigits++;
    v = qp_capi_long_alloc(ndigits);
    if (!v)
        return NULL;
    p = v->long_value.ob_digit;
    while (m) {
        *p++ = (digit)(m & PyLong_MASK);
        m >>= PyLong_SHIFT;
    }
    if (negative)
        qp_capi_long_flip_sign(v);
    return (PyObject *)v;
}

static PyObject *qp_capi_long_from_i64(int64_t x)
{
    return x < 0 ? qp_capi_long_from_u64(0 - (uint64_t)x, 1) : qp_capi_long_from_u64((uint64_t)x, 0);
}

/* ------------------------------------------------------------------------ */
/* JS int <-> digits                                                         */
/* ------------------------------------------------------------------------ */

/* Plain int (int tag, short bigint, bigint or bool) of an int, bool or
   int-subclass instance (borrowed); QP_NULL if v is not an int. */
static JSValueConst qp_capi_long_plain(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);

    if (tag == JS_TAG_INT || tag == JS_TAG_SHORT_BIG_INT || tag == JS_TAG_BIG_INT ||
        tag == JS_TAG_BOOL)
        return v;
    if (tag == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (p->class_id == JS_CLASS_PY_BOXED) {
            int t = JS_VALUE_GET_TAG(p->u.object_data);
            if (t == JS_TAG_INT || t == JS_TAG_SHORT_BIG_INT || t == JS_TAG_BIG_INT)
                return p->u.object_data;
        }
    }
    return QP_NULL;
}

/* Upper bound of the digit count of a plain int. */
static Py_ssize_t qp_capi_long_digits_bound(JSValueConst u)
{
    if (JS_VALUE_GET_TAG(u) == JS_TAG_BIG_INT) {
        JSBigInt *p = JS_VALUE_GET_PTR(u);
        return ((Py_ssize_t)p->len * JS_LIMB_BITS + PyLong_SHIFT - 1) / PyLong_SHIFT;
    }
    return (64 + PyLong_SHIFT - 1) / PyLong_SHIFT;
}

/* Fill lv_tag and the digits of op (room for qp_capi_long_digits_bound(u)
   digits) from the plain int u. */
static void qp_capi_long_fill(PyLongObject *op, JSValueConst u)
{
    digit *d = op->long_value.ob_digit;
    Py_ssize_t n = 0;
    int negative;

    switch (JS_VALUE_GET_TAG(u)) {
    case JS_TAG_BIG_INT: {
        JSBigInt *p = JS_VALUE_GET_PTR(u);
        qp_capi_acc_t acc = 0;
        unsigned accbits = 0;
        js_limb_t carry;
        uint32_t i;

        negative = (int)(p->tab[p->len - 1] >> (JS_LIMB_BITS - 1));
        carry = negative;
        for (i = 0; i < p->len; i++) {
            js_limb_t limb = p->tab[i];
            if (negative) {
                limb = ~limb + carry;
                carry = (carry && limb == 0);
            }
            acc |= (qp_capi_acc_t)limb << accbits;
            accbits += JS_LIMB_BITS;
            while (accbits >= PyLong_SHIFT) {
                d[n++] = (digit)(acc & PyLong_MASK);
                acc >>= PyLong_SHIFT;
                accbits -= PyLong_SHIFT;
            }
        }
        while (acc) {
            d[n++] = (digit)(acc & PyLong_MASK);
            acc >>= PyLong_SHIFT;
        }
        while (n > 0 && d[n - 1] == 0)
            n--;
        break;
    }
    default: {
        int64_t x;
        uint64_t m;
        switch (JS_VALUE_GET_TAG(u)) {
        case JS_TAG_SHORT_BIG_INT:
            x = JS_VALUE_GET_SHORT_BIG_INT(u);
            break;
        case JS_TAG_BOOL:
            x = JS_VALUE_GET_BOOL(u) != 0;
            break;
        default:
            x = JS_VALUE_GET_INT(u);
            break;
        }
        negative = x < 0;
        m = negative ? 0 - (uint64_t)x : (uint64_t)x;
        while (m) {
            d[n++] = (digit)(m & PyLong_MASK);
            m >>= PyLong_SHIFT;
        }
        break;
    }
    }
    if (n == 0) {
        d[0] = 0;
        qp_capi_long_set_tag(op, 0, 0);
    } else {
        qp_capi_long_set_tag(op, negative ? -1 : 1, n);
    }
}

/* A JSBigInt allocation failed: keep uncatchable exceptions, else
   MemoryError (as the int unit does). */
static JSValue qp_capi_long_bigerr(JSContext *ctx)
{
    if (ctx->rt->current_exception_is_uncatchable)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return qp_raise_memory_error(ctx);
}

/* Canonical JS int of the digits of v (new reference). */
static JSValue qp_capi_long_to_js_value(JSContext *ctx, const PyLongObject *v)
{
    Py_ssize_t n = qp_capi_long_ndigits(v), i;
    const digit *d = v->long_value.ob_digit;
    int negative = qp_capi_long_is_negative(v);
    uint32_t nl, k = 0;
    JSBigInt *r;
    qp_capi_acc_t acc = 0;
    unsigned accbits = 0;

    if (n <= 2) {
        int64_t m = n == 0 ? 0 : (n == 1 ? (int64_t)d[0] : ((int64_t)d[1] << PyLong_SHIFT) | d[0]);
        return qp_long_from_int64(ctx, negative ? -m : m);
    }
    if ((uint64_t)n * PyLong_SHIFT / JS_LIMB_BITS + 2 > (uint64_t)QP_LONG_MAX_LIMBS)
        return qp_raise_memory_error(ctx);
    nl = (uint32_t)(((uint64_t)n * PyLong_SHIFT + JS_LIMB_BITS - 1) / JS_LIMB_BITS + 1);
    QP_BIG_CALL(ctx, r, js_bigint_new(ctx, (int)nl));
    if (!r)
        return qp_capi_long_bigerr(ctx);
    for (i = 0; i < n; i++) {
        acc |= (qp_capi_acc_t)d[i] << accbits;
        accbits += PyLong_SHIFT;
        if (accbits >= JS_LIMB_BITS) {
            r->tab[k++] = (js_limb_t)acc;
            acc >>= JS_LIMB_BITS;
            accbits -= JS_LIMB_BITS;
        }
    }
    while (k < nl) {
        r->tab[k++] = (js_limb_t)acc;
        acc = 0;
    }
    if (negative) {
        js_limb_t carry = 1;
        for (k = 0; k < nl; k++) {
            r->tab[k] = ~r->tab[k] + carry;
            carry = (carry && r->tab[k] == 0);
        }
    }
    r = js_bigint_normalize(ctx, r);
    return qp_long_normalize(ctx, JS_MKPTR(JS_TAG_BIG_INT, r));
}

/* Plain JS int of an int mirror (new reference; TypeError for non-ints). */
static JSValue qp_capi_long_js(JSContext *ctx, PyObject *op)
{
    JSValue v;

    if (PyLong_Check(op) && (!QP_CAPI_HAS_HEAD(op) || QP_CAPI_HEAD(op)->kind == QP_CAPI_K_LONG))
        return qp_capi_long_to_js_value(ctx, (PyLongObject *)op);
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v))
        return v;
    return qp_long_value(ctx, v);
}

/* ---- kind ops ---- */

static size_t qp_capi_long_kind_size(JSContext *ctx, JSValueConst v)
{
    JSValueConst u = qp_capi_long_plain(v);
    Py_ssize_t n = QP_IS_NULL(u) ? 0 : qp_capi_long_digits_bound(u);
    return QP_CAPI_LONG_SIZE(n) - sizeof(PyObject);
}

static int qp_capi_long_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    JSValueConst u = qp_capi_long_plain(v);

    if (QP_IS_NULL(u)) {
        qp_raise_system_error(ctx, "int mirror of a non-int value");
        return -1;
    }
    qp_capi_long_fill((PyLongObject *)op, u);
    return 0;
}

static JSValue qp_capi_long_kind_to_js(JSContext *ctx, PyObject *op)
{
    return qp_capi_long_to_js_value(ctx, (PyLongObject *)op);
}

static int qp_capi_float_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    JSValueConst u = v;

    if (JS_VALUE_GET_TAG(u) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(u)->class_id == JS_CLASS_PY_BOXED)
        u = JS_VALUE_GET_OBJ(u)->u.object_data;
    if (JS_VALUE_GET_TAG(u) != JS_TAG_FLOAT64) {
        qp_raise_system_error(ctx, "float mirror of a non-float value");
        return -1;
    }
    ((PyFloatObject *)op)->ob_fval = JS_VALUE_GET_FLOAT64(u);
    return 0;
}

/* C-primary float at its first crossing; a subtype instance made from C
   (numpy.float64 from PyType_GenericAlloc / PyObject_Init) becomes an
   instance of the subtype with the value C stored */
static JSValue qp_capi_float_kind_to_js(JSContext *ctx, PyObject *op)
{
    QPType *sub;

    if (Py_IS_TYPE(op, &PyFloat_Type))
        return qp_float_result(ctx, ((PyFloatObject *)op)->ob_fval);
    sub = qp_capi_type_qp(Py_TYPE(op));
    if (!sub)
        return JS_EXCEPTION;
    return qp_float_new_type(ctx, sub, ((PyFloatObject *)op)->ob_fval);
}

static int qp_capi_complex_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    QPComplex *c;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_COMPLEX ||
        !(c = JS_VALUE_GET_OBJ(v)->u.opaque)) {
        qp_raise_system_error(ctx, "complex mirror of a non-complex value");
        return -1;
    }
    ((PyComplexObject *)op)->cval.real = c->real;
    ((PyComplexObject *)op)->cval.imag = c->imag;
    return 0;
}

/* C-primary complex (a subtype instance made from C, numpy.complex128) */
static JSValue qp_capi_complex_kind_to_js(JSContext *ctx, PyObject *op)
{
    Py_complex c = ((PyComplexObject *)op)->cval;
    QPType *sub;

    if (Py_IS_TYPE(op, &PyComplex_Type))
        return qp_complex_new(ctx, c.real, c.imag);
    sub = qp_capi_type_qp(Py_TYPE(op));
    if (!sub)
        return JS_EXCEPTION;
    return qp_complex_new_type(ctx, sub, c.real, c.imag);
}

/* Init step of unit C (qp_capi_init order: A, B, C, ...).  Idempotent. */
/* tp_methods of the static int and float types.  The runtime types own the
   methods; these entries exist for C code that takes a PyCFunction out of
   the table and calls it directly (_decimal: cfunc_noargs() of
   float.as_integer_ratio and int.bit_length).  They run the int / float
   method, not an override of a subclass of self. */
static PyObject *qp_capi_long_bit_length(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallMethod((PyObject *)&PyLong_Type, "bit_length", "O", self);
}

static PyObject *qp_capi_float_as_integer_ratio(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallMethod((PyObject *)&PyFloat_Type, "as_integer_ratio", "O", self);
}

static PyMethodDef qp_capi_long_methods[] = {
    { "bit_length", qp_capi_long_bit_length, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL }
};

static PyMethodDef qp_capi_float_methods[] = {
    { "as_integer_ratio", qp_capi_float_as_integer_ratio, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL }
};

static int qp_capi_init_numbers(JSContext *ctx)
{
    qp_capi_small_ints_init();
    PyLong_Type.tp_methods = qp_capi_long_methods;
    PyFloat_Type.tp_methods = qp_capi_float_methods;
    qp_capi.kind_ops[QP_CAPI_K_LONG] = (QPCapiKindOps){
        .name = "int",
        .size = qp_capi_long_kind_size,
        .init = qp_capi_long_kind_init,
        .to_js = qp_capi_long_kind_to_js,
    };
    qp_capi.kind_ops[QP_CAPI_K_FLOAT] = (QPCapiKindOps){
        .name = "float",
        .init = qp_capi_float_kind_init,
        .to_js = qp_capi_float_kind_to_js,
    };
    qp_capi.kind_ops[QP_CAPI_K_COMPLEX] = (QPCapiKindOps){
        .name = "complex",
        .init = qp_capi_complex_kind_init,
        .to_js = qp_capi_complex_kind_to_js,
    };
    return 0;
}

/* ------------------------------------------------------------------------ */
/* int construction                                                          */
/* ------------------------------------------------------------------------ */

PyObject *PyLong_FromLong(long ival)
{
    return qp_capi_long_from_i64(ival);
}

PyObject *PyLong_FromUnsignedLong(unsigned long ival)
{
    return qp_capi_long_from_u64(ival, 0);
}

PyObject *PyLong_FromLongLong(long long ival)
{
    return qp_capi_long_from_i64(ival);
}

PyObject *PyLong_FromUnsignedLongLong(unsigned long long ival)
{
    return qp_capi_long_from_u64(ival, 0);
}

PyObject *PyLong_FromSize_t(size_t ival)
{
    return qp_capi_long_from_u64(ival, 0);
}

PyObject *PyLong_FromSsize_t(Py_ssize_t ival)
{
    return qp_capi_long_from_i64(ival);
}

PyObject *PyLong_FromInt32(int32_t value)
{
    return qp_capi_long_from_i64(value);
}

PyObject *PyLong_FromUInt32(uint32_t value)
{
    return qp_capi_long_from_u64(value, 0);
}

PyObject *PyLong_FromInt64(int64_t value)
{
    return qp_capi_long_from_i64(value);
}

PyObject *PyLong_FromUInt64(uint64_t value)
{
    return qp_capi_long_from_u64(value, 0);
}

PyObject *PyLong_FromVoidPtr(void *p)
{
    return qp_capi_long_from_u64((uint64_t)(uintptr_t)p, 0);
}

PyObject *PyLong_FromDouble(double dval)
{
    const double int_max = (unsigned long)LONG_MAX + 1;
    PyLongObject *v;
    double frac;
    int i, ndig, expo, neg = 0;

    if (-int_max < dval && dval < int_max)
        return PyLong_FromLong((long)dval);
    if (isinf(dval)) {
        PyErr_SetString(PyExc_OverflowError, "cannot convert float infinity to integer");
        return NULL;
    }
    if (isnan(dval)) {
        PyErr_SetString(PyExc_ValueError, "cannot convert float NaN to integer");
        return NULL;
    }
    if (dval < 0.0) {
        neg = 1;
        dval = -dval;
    }
    frac = frexp(dval, &expo); /* dval = frac*2**expo; 0.0 <= frac < 1.0 */
    ndig = (expo - 1) / PyLong_SHIFT + 1;
    v = qp_capi_long_alloc(ndig);
    if (!v)
        return NULL;
    frac = ldexp(frac, (expo - 1) % PyLong_SHIFT + 1);
    for (i = ndig; --i >= 0; ) {
        digit bits = (digit)frac;
        v->long_value.ob_digit[i] = bits;
        frac = frac - (double)bits;
        frac = ldexp(frac, PyLong_SHIFT);
    }
    if (neg)
        qp_capi_long_flip_sign(v);
    return (PyObject *)v;
}

PyLongObject *_PyLong_New(Py_ssize_t size)
{
    return qp_capi_long_alloc(size);
}

PyObject *_PyLong_Copy(PyLongObject *src)
{
    PyLongObject *result;
    Py_ssize_t size;

    if (qp_capi_long_is_compact(src)) {
        PyObject *s = qp_capi_small_int((long)qp_capi_long_compact_value(src));
        if (s)
            return s;
    }
    size = qp_capi_long_ndigits(src);
    result = qp_capi_long_alloc(size);
    if (!result)
        return NULL;
    result->long_value.lv_tag = src->long_value.lv_tag & ~(uintptr_t)QP_CAPI_LIMMORTAL_BIT;
    memcpy(result->long_value.ob_digit, src->long_value.ob_digit, (size > 0 ? size : 1) * sizeof(digit));
    return (PyObject *)result;
}

PyLongObject *_PyLong_FromDigits(int negative, Py_ssize_t digit_count, digit *digits)
{
    PyLongObject *result;

    if (digit_count == 0)
        return (PyLongObject *)qp_capi_small_int(0);
    result = qp_capi_long_alloc(digit_count);
    if (!result)
        return NULL;
    qp_capi_long_set_tag(result, negative ? -1 : 1, digit_count);
    memcpy(result->long_value.ob_digit, digits, digit_count * sizeof(digit));
    return result;
}

PyAPI_FUNC(void) _PyLong_ExactDealloc(PyObject *self);
void _PyLong_ExactDealloc(PyObject *self)
{
    _Py_Dealloc(self);
}

/* ------------------------------------------------------------------------ */
/* int -> C integers                                                         */
/* ------------------------------------------------------------------------ */

/* The int of vv (borrowed) or of vv.__index__() (new reference, *pdecref
   set); NULL on error. */
static PyLongObject *qp_capi_long_or_index(PyObject *vv, int *pdecref)
{
    *pdecref = 0;
    if (PyLong_Check(vv)) {
        JSValue v;
        if (!QP_CAPI_HAS_HEAD(vv) || QP_CAPI_HEAD(vv)->kind == QP_CAPI_K_LONG)
            return (PyLongObject *)vv;
        /* an int whose mirror has no digits (foreign layout): use its
           plain int value */
        v = qp_capi_long_js(qp_capi.ctx, vv);
        if (JS_IsException(v))
            return NULL;
        *pdecref = 1;
        return (PyLongObject *)qp_capi_steal(qp_capi.ctx, v);
    }
    *pdecref = 1;
    return (PyLongObject *)_PyNumber_Index(vv);
}

/* Magnitude of a non-compact int into an unsigned 64-bit value: 0 or 1
   when it does not fit. */
static int qp_capi_long_mag_u64(const PyLongObject *v, uint64_t *out)
{
    Py_ssize_t i = qp_capi_long_ndigits(v);
    uint64_t x = 0;

    while (--i >= 0) {
        if (x > (UINT64_MAX >> PyLong_SHIFT))
            return 1;
        x = (x << PyLong_SHIFT) | v->long_value.ob_digit[i];
    }
    *out = x;
    return 0;
}

long PyLong_AsLongAndOverflow(PyObject *vv, int *overflow)
{
    PyLongObject *v;
    long res;
    int do_decref;

    *overflow = 0;
    if (vv == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    v = qp_capi_long_or_index(vv, &do_decref);
    if (!v)
        return -1;
    if (qp_capi_long_is_compact(v)) {
        res = (long)qp_capi_long_compact_value(v);
    } else {
        uint64_t x;
        int sign = qp_capi_long_sign(v);
        res = -1;
        if (qp_capi_long_mag_u64(v, &x))
            *overflow = sign;
        else if (x <= (unsigned long)LONG_MAX)
            res = (long)x * sign;
        else if (sign < 0 && x == QP_CAPI_ABS_LONG_MIN)
            res = LONG_MIN;
        else
            *overflow = sign;
    }
    if (do_decref)
        Py_DECREF(v);
    return res;
}

long PyLong_AsLong(PyObject *obj)
{
    int overflow;
    long result = PyLong_AsLongAndOverflow(obj, &overflow);
    if (overflow)
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C long");
    return result;
}

int PyLong_AsInt(PyObject *obj)
{
    int overflow;
    long result = PyLong_AsLongAndOverflow(obj, &overflow);
    if (overflow || result > INT_MAX || result < INT_MIN) {
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int");
        return -1;
    }
    return (int)result;
}

/* long is 64-bit on every C-API host (capi object.h rejects 32-bit) */
long long PyLong_AsLongLongAndOverflow(PyObject *vv, int *overflow)
{
    return PyLong_AsLongAndOverflow(vv, overflow);
}

Py_ssize_t PyLong_AsSsize_t(PyObject *vv)
{
    PyLongObject *v;
    uint64_t x;
    int sign;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyLong_Check(vv)) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return -1;
    }
    v = (PyLongObject *)vv;
    if (qp_capi_long_is_compact(v))
        return qp_capi_long_compact_value(v);
    sign = qp_capi_long_sign(v);
    if (!qp_capi_long_mag_u64(v, &x)) {
        if (x <= (size_t)PY_SSIZE_T_MAX)
            return (Py_ssize_t)x * sign;
        if (sign < 0 && x == QP_CAPI_ABS_SSIZE_MIN)
            return PY_SSIZE_T_MIN;
    }
    PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C ssize_t");
    return -1;
}

unsigned long PyLong_AsUnsignedLong(PyObject *vv)
{
    PyLongObject *v;
    uint64_t x;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return (unsigned long)-1;
    }
    if (!PyLong_Check(vv)) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return (unsigned long)-1;
    }
    v = (PyLongObject *)vv;
    if (qp_capi_long_is_nonneg_compact(v))
        return (unsigned long)(size_t)qp_capi_long_compact_value(v);
    if (qp_capi_long_is_negative(v)) {
        PyErr_SetString(PyExc_OverflowError, "can't convert negative value to unsigned int");
        return (unsigned long)-1;
    }
    if (qp_capi_long_mag_u64(v, &x) || x > ULONG_MAX) {
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C unsigned long");
        return (unsigned long)-1;
    }
    return (unsigned long)x;
}

size_t PyLong_AsSize_t(PyObject *vv)
{
    PyLongObject *v;
    uint64_t x;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return (size_t)-1;
    }
    if (!PyLong_Check(vv)) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return (size_t)-1;
    }
    v = (PyLongObject *)vv;
    if (qp_capi_long_is_nonneg_compact(v))
        return (size_t)qp_capi_long_compact_value(v);
    if (qp_capi_long_is_negative(v)) {
        PyErr_SetString(PyExc_OverflowError, "can't convert negative value to size_t");
        return (size_t)-1;
    }
    if (qp_capi_long_mag_u64(v, &x) || x > SIZE_MAX) {
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C size_t");
        return (size_t)-1;
    }
    return (size_t)x;
}

/* low 64 bits of the two's complement value of an int */
static uint64_t qp_capi_long_mask_u64(const PyLongObject *v)
{
    Py_ssize_t i;
    uint64_t x = 0;

    if (qp_capi_long_is_compact(v))
        return (uint64_t)(int64_t)qp_capi_long_compact_value(v);
    i = qp_capi_long_ndigits(v);
    while (--i >= 0)
        x = (x << PyLong_SHIFT) | v->long_value.ob_digit[i];
    return qp_capi_long_is_negative(v) ? 0 - x : x;
}

static uint64_t qp_capi_long_as_mask(PyObject *op)
{
    PyLongObject *lo;
    uint64_t val;

    if (op == NULL) {
        PyErr_BadInternalCall();
        return (uint64_t)-1;
    }
    if (PyLong_Check(op))
        return qp_capi_long_mask_u64((PyLongObject *)op);
    lo = (PyLongObject *)_PyNumber_Index(op);
    if (!lo)
        return (uint64_t)-1;
    val = qp_capi_long_mask_u64(lo);
    Py_DECREF(lo);
    return val;
}

unsigned long PyLong_AsUnsignedLongMask(PyObject *op)
{
    return (unsigned long)qp_capi_long_as_mask(op);
}

unsigned long long PyLong_AsUnsignedLongLongMask(PyObject *op)
{
    return (unsigned long long)qp_capi_long_as_mask(op);
}

long long PyLong_AsLongLong(PyObject *vv)
{
    PyLongObject *v;
    long long bytes;
    int res, do_decref;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    v = qp_capi_long_or_index(vv, &do_decref);
    if (!v)
        return -1;
    if (qp_capi_long_is_compact(v)) {
        res = 0;
        bytes = qp_capi_long_compact_value(v);
    } else {
        res = _PyLong_AsByteArray(v, (unsigned char *)&bytes, sizeof(bytes), PY_LITTLE_ENDIAN, 1, 1);
    }
    if (do_decref)
        Py_DECREF(v);
    return res < 0 ? (long long)-1 : bytes;
}

unsigned long long PyLong_AsUnsignedLongLong(PyObject *vv)
{
    PyLongObject *v;
    unsigned long long bytes;
    int res;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return (unsigned long long)-1;
    }
    if (!PyLong_Check(vv)) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return (unsigned long long)-1;
    }
    v = (PyLongObject *)vv;
    if (qp_capi_long_is_nonneg_compact(v)) {
        res = 0;
        bytes = (unsigned long long)(size_t)qp_capi_long_compact_value(v);
    } else {
        res = _PyLong_AsByteArray(v, (unsigned char *)&bytes, sizeof(bytes), PY_LITTLE_ENDIAN, 0, 1);
    }
    return res < 0 ? (unsigned long long)res : bytes;
}

void *PyLong_AsVoidPtr(PyObject *vv)
{
    long long x;

    if (vv && PyLong_Check(vv) && qp_capi_long_is_negative((PyLongObject *)vv))
        x = PyLong_AsLongLong(vv);
    else
        x = (long long)PyLong_AsUnsignedLongLong(vv);
    if (x == -1 && PyErr_Occurred())
        return NULL;
    return (void *)(intptr_t)x;
}

int PyLong_IsPositive(PyObject *obj)
{
    if (!PyLong_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected int, got %T", obj);
        return -1;
    }
    return (((PyLongObject *)obj)->long_value.lv_tag & QP_CAPI_LSIGN_MASK) == 0;
}

int PyLong_IsNegative(PyObject *obj)
{
    if (!PyLong_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected int, got %T", obj);
        return -1;
    }
    return qp_capi_long_is_negative((PyLongObject *)obj);
}

int PyLong_IsZero(PyObject *obj)
{
    if (!PyLong_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "expected int, got %T", obj);
        return -1;
    }
    return qp_capi_long_is_zero((PyLongObject *)obj);
}

int _PyLong_Sign(PyObject *vv)
{
    return qp_capi_long_sign((PyLongObject *)vv);
}

int PyLong_GetSign(PyObject *vv, int *sign)
{
    if (!PyLong_Check(vv)) {
        PyErr_Format(PyExc_TypeError, "expect int, got %T", vv);
        return -1;
    }
    *sign = qp_capi_long_sign((PyLongObject *)vv);
    return 0;
}

/* longintrepr.h maps these names to the inline versions; the exported
   functions are for the stable ABI (CPython does the same). */
#undef PyUnstable_Long_IsCompact
#undef PyUnstable_Long_CompactValue
int PyUnstable_Long_IsCompact(const PyLongObject *op)
{
    return qp_capi_long_is_compact(op);
}

Py_ssize_t PyUnstable_Long_CompactValue(const PyLongObject *op)
{
    return qp_capi_long_compact_value(op);
}
#define PyUnstable_Long_IsCompact _PyLong_IsCompact
#define PyUnstable_Long_CompactValue _PyLong_CompactValue

int64_t _PyLong_NumBits(PyObject *vv)
{
    PyLongObject *v = (PyLongObject *)vv;
    Py_ssize_t ndigits = qp_capi_long_ndigits(v);
    int64_t result = 0;

    if (ndigits > 0) {
        digit msd = v->long_value.ob_digit[ndigits - 1];
        result = (int64_t)(ndigits - 1) * PyLong_SHIFT + qp_capi_num_bit_length(msd);
    }
    return result;
}

/* ------------------------------------------------------------------------ */
/* Byte arrays (int.from_bytes / to_bytes, PyLong_*NativeBytes)              */
/* ------------------------------------------------------------------------ */

PyObject *_PyLong_FromByteArray(const unsigned char *bytes, size_t n,
                                int little_endian, int is_signed)
{
    const unsigned char *pstartbyte, *pendbyte;
    int incr;
    size_t numsignificantbytes;
    Py_ssize_t ndigits, idigit = 0;
    PyLongObject *v;

    if (n == 0)
        return PyLong_FromLong(0L);
    if (little_endian) {
        pstartbyte = bytes;
        pendbyte = bytes + n - 1;
        incr = 1;
    } else {
        pstartbyte = bytes + n - 1;
        pendbyte = bytes;
        incr = -1;
    }
    if (is_signed)
        is_signed = *pendbyte >= 0x80;
    {
        size_t i;
        const unsigned char *p = pendbyte;
        const int pincr = -incr;
        const unsigned char insignificant = is_signed ? 0xff : 0x00;
        for (i = 0; i < n; ++i, p += pincr) {
            if (*p != insignificant)
                break;
        }
        numsignificantbytes = n - i;
        if (is_signed && numsignificantbytes < n)
            ++numsignificantbytes;
    }
    ndigits = numsignificantbytes / PyLong_SHIFT * 8
        + (numsignificantbytes % PyLong_SHIFT * 8 + PyLong_SHIFT - 1) / PyLong_SHIFT;
    v = qp_capi_long_alloc(ndigits);
    if (!v)
        return NULL;
    {
        size_t i;
        twodigits carry = 1, accum = 0;
        unsigned int accumbits = 0;
        const unsigned char *p = pstartbyte;
        for (i = 0; i < numsignificantbytes; ++i, p += incr) {
            twodigits thisbyte = *p;
            if (is_signed) {
                thisbyte = (0xff ^ thisbyte) + carry;
                carry = thisbyte >> 8;
                thisbyte &= 0xff;
            }
            accum |= thisbyte << accumbits;
            accumbits += 8;
            if (accumbits >= PyLong_SHIFT) {
                v->long_value.ob_digit[idigit++] = (digit)(accum & PyLong_MASK);
                accum >>= PyLong_SHIFT;
                accumbits -= PyLong_SHIFT;
            }
        }
        if (accumbits)
            v->long_value.ob_digit[idigit++] = (digit)accum;
    }
    if (idigit == 0) {
        v->long_value.ob_digit[0] = 0;
        qp_capi_long_set_tag(v, 0, 0);
    } else {
        qp_capi_long_set_tag(v, is_signed ? -1 : 1, idigit);
    }
    return qp_capi_long_maybe_small(qp_capi_long_normalize(v));
}

int _PyLong_AsByteArray(PyLongObject *v, unsigned char *bytes, size_t n,
                        int little_endian, int is_signed, int with_exceptions)
{
    Py_ssize_t i, ndigits;
    twodigits accum;
    unsigned int accumbits;
    int do_twos_comp;
    digit carry;
    size_t j;
    unsigned char *p;
    int pincr;

    ndigits = qp_capi_long_ndigits(v);
    if (qp_capi_long_is_negative(v)) {
        if (!is_signed) {
            if (with_exceptions)
                PyErr_SetString(PyExc_OverflowError, "can't convert negative int to unsigned");
            return -1;
        }
        do_twos_comp = 1;
    } else {
        do_twos_comp = 0;
    }
    if (little_endian) {
        p = bytes;
        pincr = 1;
    } else {
        p = bytes + n - 1;
        pincr = -1;
    }
    j = 0;
    accum = 0;
    accumbits = 0;
    carry = do_twos_comp ? 1 : 0;
    for (i = 0; i < ndigits; ++i) {
        digit thisdigit = v->long_value.ob_digit[i];
        if (do_twos_comp) {
            thisdigit = (thisdigit ^ PyLong_MASK) + carry;
            carry = thisdigit >> PyLong_SHIFT;
            thisdigit &= PyLong_MASK;
        }
        accum |= (twodigits)thisdigit << accumbits;
        if (i == ndigits - 1) {
            digit s = do_twos_comp ? thisdigit ^ PyLong_MASK : thisdigit;
            while (s != 0) {
                s >>= 1;
                accumbits++;
            }
        } else {
            accumbits += PyLong_SHIFT;
        }
        while (accumbits >= 8) {
            if (j >= n)
                goto Overflow;
            ++j;
            *p = (unsigned char)(accum & 0xff);
            p += pincr;
            accumbits -= 8;
            accum >>= 8;
        }
    }
    if (accumbits > 0) {
        if (j >= n)
            goto Overflow;
        ++j;
        if (do_twos_comp)
            accum |= (~(twodigits)0) << accumbits;
        *p = (unsigned char)(accum & 0xff);
        p += pincr;
    } else if (j == n && is_signed) {
        int sign_bit_set = n > 0 ? *(p - pincr) >= 0x80 : 0;
        if (sign_bit_set == do_twos_comp)
            return 0;
        goto Overflow;
    }
    {
        unsigned char signbyte = do_twos_comp ? 0xffU : 0U;
        for (; j < n; ++j, p += pincr)
            *p = signbyte;
    }
    return 0;

Overflow:
    if (with_exceptions)
        PyErr_SetString(PyExc_OverflowError, "int too big to convert");
    return -1;
}

static inline int qp_capi_fits_in_n_bits(Py_ssize_t v, Py_ssize_t n)
{
    Py_ssize_t v_extended;
    if (n >= (Py_ssize_t)sizeof(Py_ssize_t) * 8)
        return 1;
    v_extended = v >> ((int)n - 1);
    return v_extended == 0 || v_extended == -1;
}

static inline int qp_capi_resolve_endianness(int endianness)
{
    if (endianness == -1 || (endianness & 2))
        return PY_LITTLE_ENDIAN;
    return endianness & 1;
}

Py_ssize_t PyLong_AsNativeBytes(PyObject *vv, void *buffer, Py_ssize_t n, int flags)
{
    PyLongObject *v;
    union {
        Py_ssize_t v;
        unsigned char b[sizeof(Py_ssize_t)];
    } cv;
    int do_decref = 0, little_endian;
    Py_ssize_t res = 0;

    if (vv == NULL || n < 0) {
        PyErr_BadInternalCall();
        return -1;
    }
    little_endian = qp_capi_resolve_endianness(flags);
    if (PyLong_Check(vv)) {
        v = (PyLongObject *)vv;
    } else if (flags != -1 && (flags & Py_ASNATIVEBYTES_ALLOW_INDEX)) {
        v = (PyLongObject *)_PyNumber_Index(vv);
        if (!v)
            return -1;
        do_decref = 1;
    } else {
        PyErr_Format(PyExc_TypeError, "expect int, got %T", vv);
        return -1;
    }
    if ((flags != -1 && (flags & Py_ASNATIVEBYTES_REJECT_NEGATIVE)) && qp_capi_long_is_negative(v)) {
        PyErr_SetString(PyExc_ValueError, "Cannot convert negative int");
        if (do_decref)
            Py_DECREF(v);
        return -1;
    }
    if (qp_capi_long_is_compact(v)) {
        cv.v = qp_capi_long_compact_value(v);
        res = sizeof(cv.b);
        if (n <= 0) {
            /* nothing to do */
        } else if (n <= (Py_ssize_t)sizeof(cv.b)) {
#if PY_LITTLE_ENDIAN
            if (little_endian) {
                memcpy(buffer, cv.b, n);
            } else {
                for (Py_ssize_t i = 0; i < n; ++i)
                    ((unsigned char *)buffer)[n - i - 1] = cv.b[i];
            }
#else
            if (little_endian) {
                for (Py_ssize_t i = 0; i < n; ++i)
                    ((unsigned char *)buffer)[i] = cv.b[sizeof(cv.b) - i - 1];
            } else {
                memcpy(buffer, &cv.b[sizeof(cv.b) - n], n);
            }
#endif
            if (qp_capi_fits_in_n_bits(cv.v, n * 8)) {
                res = n;
            } else if (cv.v > 0 && qp_capi_fits_in_n_bits(cv.v, n * 8 + 1)) {
                if (flags == -1 || (flags & Py_ASNATIVEBYTES_UNSIGNED_BUFFER))
                    res = n;
                else
                    res = n + 1;
            }
        } else {
            unsigned char fill = cv.v < 0 ? 0xFF : 0x00;
#if PY_LITTLE_ENDIAN
            if (little_endian) {
                memcpy(buffer, cv.b, sizeof(cv.b));
                memset((char *)buffer + sizeof(cv.b), fill, n - sizeof(cv.b));
            } else {
                unsigned char *b = (unsigned char *)buffer;
                for (Py_ssize_t i = 0; i < n - (int)sizeof(cv.b); ++i)
                    *b++ = fill;
                for (Py_ssize_t i = sizeof(cv.b); i > 0; --i)
                    *b++ = cv.b[i - 1];
            }
#else
            if (little_endian) {
                unsigned char *b = (unsigned char *)buffer;
                for (Py_ssize_t i = sizeof(cv.b); i > 0; --i)
                    *b++ = cv.b[i - 1];
                for (Py_ssize_t i = 0; i < n - (int)sizeof(cv.b); ++i)
                    *b++ = fill;
            } else {
                memset(buffer, fill, n - sizeof(cv.b));
                memcpy((char *)buffer + n - sizeof(cv.b), cv.b, sizeof(cv.b));
            }
#endif
        }
    } else {
        int64_t nb;
        if (n > 0)
            _PyLong_AsByteArray(v, buffer, (size_t)n, little_endian, 1, 0);
        nb = _PyLong_NumBits((PyObject *)v);
        res = (Py_ssize_t)(nb / 8) + 1;
        if (n > 0 && res == n + 1 && nb % 8 == 0) {
            if (qp_capi_long_is_negative(v)) {
                int is_edge_case = 1;
                unsigned char *b = (unsigned char *)buffer;
                for (Py_ssize_t i = 0; i < n && is_edge_case; ++i, ++b) {
                    if (i == 0)
                        is_edge_case = (*b == (little_endian ? 0 : 0x80));
                    else if (i < n - 1)
                        is_edge_case = (*b == 0);
                    else
                        is_edge_case = (*b == (little_endian ? 0x80 : 0));
                }
                if (is_edge_case)
                    res = n;
            } else {
                unsigned char *b = (unsigned char *)buffer;
                if (b[little_endian ? n - 1 : 0] & 0x80) {
                    if (flags == -1 || (flags & Py_ASNATIVEBYTES_UNSIGNED_BUFFER))
                        res = n;
                    else
                        res = n + 1;
                }
            }
        }
    }
    if (do_decref)
        Py_DECREF(v);
    return res;
}

PyObject *PyLong_FromNativeBytes(const void *buffer, size_t n, int flags)
{
    if (!buffer) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyLong_FromByteArray((const unsigned char *)buffer, n, qp_capi_resolve_endianness(flags),
                                 (flags == -1 || !(flags & Py_ASNATIVEBYTES_UNSIGNED_BUFFER)) ? 1 : 0);
}

PyObject *PyLong_FromUnsignedNativeBytes(const void *buffer, size_t n, int flags)
{
    if (!buffer) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyLong_FromByteArray((const unsigned char *)buffer, n, qp_capi_resolve_endianness(flags), 0);
}

#define QP_CAPI_LONG_TO_INT(type, obj, result, uflags)                          \
    do {                                                                      \
        type value;                                                           \
        int flags = Py_ASNATIVEBYTES_NATIVE_ENDIAN | Py_ASNATIVEBYTES_ALLOW_INDEX | (uflags); \
        Py_ssize_t bytes = PyLong_AsNativeBytes(obj, &value, sizeof(value), flags); \
        if (bytes < 0)                                                        \
            return -1;                                                        \
        if ((size_t)bytes > sizeof(value)) {                                  \
            PyErr_SetString(PyExc_OverflowError,                              \
                            "Python int too large to convert to C " #type);   \
            return -1;                                                        \
        }                                                                     \
        *result = value;                                                      \
        return 0;                                                             \
    } while (0)
#define QP_CAPI_UFLAGS (Py_ASNATIVEBYTES_UNSIGNED_BUFFER | Py_ASNATIVEBYTES_REJECT_NEGATIVE)

int PyLong_AsInt32(PyObject *obj, int32_t *result)
{
    QP_CAPI_LONG_TO_INT(int32_t, obj, result, 0);
}

int PyLong_AsInt64(PyObject *obj, int64_t *result)
{
    QP_CAPI_LONG_TO_INT(int64_t, obj, result, 0);
}

int PyLong_AsUInt32(PyObject *obj, uint32_t *result)
{
    QP_CAPI_LONG_TO_INT(uint32_t, obj, result, QP_CAPI_UFLAGS);
}

int PyLong_AsUInt64(PyObject *obj, uint64_t *result)
{
    QP_CAPI_LONG_TO_INT(uint64_t, obj, result, QP_CAPI_UFLAGS);
}
#undef QP_CAPI_LONG_TO_INT

/* Argument Clinic converters (pycore_long.h) */
#define QP_CAPI_UNSIGNED_INT_CONVERTER(NAME, TYPE)                            \
    PyAPI_FUNC(int) _PyLong_##NAME##_Converter(PyObject *obj, void *ptr);     \
    int _PyLong_##NAME##_Converter(PyObject *obj, void *ptr)                  \
    {                                                                         \
        Py_ssize_t bytes = PyLong_AsNativeBytes(obj, ptr, sizeof(TYPE),       \
                Py_ASNATIVEBYTES_NATIVE_ENDIAN | Py_ASNATIVEBYTES_ALLOW_INDEX | \
                QP_CAPI_UFLAGS);                                              \
        if (bytes < 0)                                                        \
            return 0;                                                         \
        if ((size_t)bytes > sizeof(TYPE)) {                                   \
            PyErr_SetString(PyExc_OverflowError,                              \
                            "Python int too large for C " #TYPE);             \
            return 0;                                                         \
        }                                                                     \
        return 1;                                                             \
    }

QP_CAPI_UNSIGNED_INT_CONVERTER(UnsignedShort, unsigned short)
QP_CAPI_UNSIGNED_INT_CONVERTER(UnsignedInt, unsigned int)
QP_CAPI_UNSIGNED_INT_CONVERTER(UnsignedLong, unsigned long)
QP_CAPI_UNSIGNED_INT_CONVERTER(UnsignedLongLong, unsigned long long)
QP_CAPI_UNSIGNED_INT_CONVERTER(Size_t, size_t)
QP_CAPI_UNSIGNED_INT_CONVERTER(UInt8, uint8_t)
QP_CAPI_UNSIGNED_INT_CONVERTER(UInt16, uint16_t)
QP_CAPI_UNSIGNED_INT_CONVERTER(UInt32, uint32_t)
QP_CAPI_UNSIGNED_INT_CONVERTER(UInt64, uint64_t)
#undef QP_CAPI_UNSIGNED_INT_CONVERTER
#undef QP_CAPI_UFLAGS

/* ------------------------------------------------------------------------ */
/* int <-> double                                                            */
/* ------------------------------------------------------------------------ */

/* x[0:m] << d (0 <= d < PyLong_SHIFT); returns the carry. */
static digit qp_capi_v_lshift(digit *z, const digit *a, Py_ssize_t m, int d)
{
    Py_ssize_t i;
    digit carry = 0;
    for (i = 0; i < m; i++) {
        twodigits acc = (twodigits)a[i] << d | carry;
        z[i] = (digit)acc & PyLong_MASK;
        carry = (digit)(acc >> PyLong_SHIFT);
    }
    return carry;
}

/* x[0:m] >> d (0 <= d < PyLong_SHIFT); returns the bits shifted out. */
static digit qp_capi_v_rshift(digit *z, const digit *a, Py_ssize_t m, int d)
{
    Py_ssize_t i;
    digit carry = 0;
    digit mask = ((digit)1 << d) - 1U;
    for (i = m; i-- > 0;) {
        twodigits acc = (twodigits)carry << PyLong_SHIFT | a[i];
        carry = (digit)acc & mask;
        z[i] = (digit)(acc >> d);
    }
    return carry;
}

PyAPI_FUNC(double) _PyLong_Frexp(PyLongObject *a, int64_t *e);
double _PyLong_Frexp(PyLongObject *a, int64_t *e)
{
    Py_ssize_t a_size, shift_digits, x_size;
    int shift_bits;
    int64_t a_bits;
    digit rem;
    digit x_digits[2 + (DBL_MANT_DIG + 1) / PyLong_SHIFT] = {0,};
    double dx;
    static const int half_even_correction[8] = {0, -1, -2, 1, 0, -1, 2, 1};

    a_size = qp_capi_long_ndigits(a);
    if (a_size == 0) {
        *e = 0;
        return 0.0;
    }
    a_bits = _PyLong_NumBits((PyObject *)a);
    if (a_bits <= DBL_MANT_DIG + 2) {
        shift_digits = (DBL_MANT_DIG + 2 - (Py_ssize_t)a_bits) / PyLong_SHIFT;
        shift_bits = (DBL_MANT_DIG + 2 - (int)a_bits) % PyLong_SHIFT;
        x_size = shift_digits;
        rem = qp_capi_v_lshift(x_digits + x_size, a->long_value.ob_digit, a_size, shift_bits);
        x_size += a_size;
        x_digits[x_size++] = rem;
    } else {
        shift_digits = (Py_ssize_t)((a_bits - DBL_MANT_DIG - 2) / PyLong_SHIFT);
        shift_bits = (int)((a_bits - DBL_MANT_DIG - 2) % PyLong_SHIFT);
        rem = qp_capi_v_rshift(x_digits, a->long_value.ob_digit + shift_digits,
                               a_size - shift_digits, shift_bits);
        x_size = a_size - shift_digits;
        if (rem) {
            x_digits[0] |= 1;
        } else {
            while (shift_digits > 0) {
                if (a->long_value.ob_digit[--shift_digits]) {
                    x_digits[0] |= 1;
                    break;
                }
            }
        }
    }
    x_digits[0] += half_even_correction[x_digits[0] & 7];
    dx = x_digits[--x_size];
    while (x_size > 0)
        dx = dx * PyLong_BASE + x_digits[--x_size];
    dx /= 4.0 * (double)((uint64_t)1 << DBL_MANT_DIG);
    if (dx == 1.0) {
        dx = 0.5;
        a_bits += 1;
    }
    *e = a_bits;
    return qp_capi_long_is_negative(a) ? -dx : dx;
}

double PyLong_AsDouble(PyObject *v)
{
    int64_t exponent;
    double x;

    if (v == NULL) {
        PyErr_BadInternalCall();
        return -1.0;
    }
    if (!PyLong_Check(v)) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return -1.0;
    }
    if (qp_capi_long_is_compact((PyLongObject *)v))
        return (double)qp_capi_long_compact_value((PyLongObject *)v);
    x = _PyLong_Frexp((PyLongObject *)v, &exponent);
    if (exponent > DBL_MAX_EXP) {
        PyErr_SetString(PyExc_OverflowError, "int too large to convert to float");
        return -1.0;
    }
    return ldexp(x, (int)exponent);
}

/* ------------------------------------------------------------------------ */
/* Parsing and formatting                                                    */
/* ------------------------------------------------------------------------ */

PyAPI_DATA(unsigned char) _PyLong_DigitValue[256];
unsigned char _PyLong_DigitValue[256] = {
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  37, 37, 37, 37, 37, 37,
    37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 37, 37, 37, 37,
    37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
};

/* Py_ISSPACE (ASCII whitespace of pyctype) */
static inline int qp_capi_num_isspace(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* long_from_string_base() validation: *str is advanced to the end of the
   digits (or to the offending character).  1 = valid with digits in
   [start, *pend), 0 = syntax error. */
static int qp_capi_long_scan_digits(const char **str, int base, const char **pstart, const char **pend)
{
    const char *start, *p;
    char prev = 0;

    start = p = *str;
    if (*start == '_')
        return 0;
    while (_PyLong_DigitValue[(unsigned char)*p] < base || *p == '_') {
        if (*p == '_') {
            if (prev == '_') {
                *str = p - 1;
                return 0;
            }
        }
        prev = *p;
        ++p;
    }
    if (prev == '_') {
        *str = p - 1;
        return 0;
    }
    *str = p;
    if (start == p)
        return 0;
    *pstart = start;
    *pend = p;
    while (*p && qp_capi_num_isspace((unsigned char)*p))
        p++;
    *str = p;
    return *p == '\0';
}

PyObject *PyLong_FromString(const char *str, char **pend, int base)
{
    JSContext *ctx = qp_capi.ctx;
    int sign = 1, error_if_nonzero = 0;
    const char *orig_str = str, *dstart, *dend;
    JSValue s, z;
    PyObject *strobj, *res;
    Py_ssize_t slen;

    if ((base != 0 && base < 2) || base > 36) {
        PyErr_SetString(PyExc_ValueError, "int() arg 2 must be >= 2 and <= 36");
        return NULL;
    }
    while (*str != '\0' && qp_capi_num_isspace((unsigned char)*str))
        ++str;
    if (*str == '+') {
        ++str;
    } else if (*str == '-') {
        ++str;
        sign = -1;
    }
    if (base == 0) {
        if (str[0] != '0')
            base = 10;
        else if (str[1] == 'x' || str[1] == 'X')
            base = 16;
        else if (str[1] == 'o' || str[1] == 'O')
            base = 8;
        else if (str[1] == 'b' || str[1] == 'B')
            base = 2;
        else {
            /* "old" (C-style) octal literal, now invalid; might still be zero */
            error_if_nonzero = 1;
            base = 10;
        }
    }
    if (str[0] == '0' &&
        ((base == 16 && (str[1] == 'x' || str[1] == 'X')) ||
         (base == 8 && (str[1] == 'o' || str[1] == 'O')) ||
         (base == 2 && (str[1] == 'b' || str[1] == 'B')))) {
        str += 2;
        if (*str == '_')
            ++str;
    }
    if (!qp_capi_long_scan_digits(&str, base, &dstart, &dend))
        goto onError;

    /* the validated digits/underscores are converted by int(str, base) */
    s = qp_str_from_utf8(ctx, dstart, dend - dstart);
    if (JS_IsException(s))
        return NULL;
    z = qp_long_from_str(ctx, s, base);
    JS_FreeValue(ctx, s);
    if (JS_IsException(z))
        return NULL;
    if (error_if_nonzero) {
        base = 0;
        if (qp_long_sign(z) != 0) {
            JS_FreeValue(ctx, z);
            goto onError;
        }
    }
    if (sign < 0) {
        JSValue t = qp_unop(ctx, z, QP_UNOP_NEG);
        JS_FreeValue(ctx, z);
        z = t;
        if (JS_IsException(z))
            return NULL;
    }
    res = qp_capi_steal(ctx, z);
    if (res && pend != NULL)
        *pend = (char *)str;
    return res;

onError:
    if (pend != NULL)
        *pend = (char *)str;
    slen = strlen(orig_str) < 200 ? (Py_ssize_t)strlen(orig_str) : 200;
    strobj = PyUnicode_FromStringAndSize(orig_str, slen);
    if (strobj == NULL)
        return NULL;
    PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %.200R", base, strobj);
    Py_DECREF(strobj);
    return NULL;
}

PyAPI_FUNC(PyObject *) _PyLong_FromBytes(const char *s, Py_ssize_t len, int base);
PyObject *_PyLong_FromBytes(const char *s, Py_ssize_t len, int base)
{
    PyObject *result, *strobj;
    char *end = NULL;

    result = PyLong_FromString(s, &end, base);
    if (end == NULL || (result != NULL && end == s + len))
        return result;
    Py_XDECREF(result);
    strobj = PyBytes_FromStringAndSize(s, Py_MIN(len, 200));
    if (strobj != NULL) {
        PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %.200R", base, strobj);
        Py_DECREF(strobj);
    }
    return NULL;
}

PyObject *PyLong_FromUnicodeObject(PyObject *u, int base)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (!u)
        return qp_capi_num_null_error();
    v = qp_capi_value(ctx, u);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_long_from_str(ctx, v, base));
}

PyAPI_FUNC(PyObject *) _PyLong_Format(PyObject *obj, int base);
PyObject *_PyLong_Format(PyObject *obj, int base)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, r;

    v = qp_capi_long_js(ctx, obj);
    if (JS_IsException(v))
        return NULL;
    r = qp_long_to_str(ctx, v, base, 1);
    JS_FreeValue(ctx, v);
    return qp_capi_from_js_result(ctx, r);
}

/* ------------------------------------------------------------------------ */
/* Arithmetic helpers of pycore_long.h (on the JS ints)                      */
/* ------------------------------------------------------------------------ */

/* JS-level binop on plain ints; consumes nothing. */
static JSValue qp_capi_li_binop(JSContext *ctx, JSValueConst a, JSValueConst b, int nb)
{
    return qp_binop(ctx, a, b, nb);
}

PyAPI_FUNC(PyObject *) _PyLong_Rshift(PyObject *a, int64_t shiftby);
PyObject *_PyLong_Rshift(PyObject *a, int64_t shiftby)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue va, vs, r;

    va = qp_capi_long_js(ctx, a);
    if (JS_IsException(va))
        return NULL;
    vs = qp_long_from_int64(ctx, shiftby);
    if (JS_IsException(vs)) {
        JS_FreeValue(ctx, va);
        return NULL;
    }
    r = qp_capi_li_binop(ctx, va, vs, QP_NB_RSHIFT);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vs);
    return qp_capi_from_js_result(ctx, r);
}

PyAPI_FUNC(PyObject *) _PyLong_Lshift(PyObject *a, int64_t shiftby);
PyObject *_PyLong_Lshift(PyObject *a, int64_t shiftby)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue va, vs, r;

    va = qp_capi_long_js(ctx, a);
    if (JS_IsException(va))
        return NULL;
    vs = qp_long_from_int64(ctx, shiftby);
    if (JS_IsException(vs)) {
        JS_FreeValue(ctx, va);
        return NULL;
    }
    r = qp_capi_li_binop(ctx, va, vs, QP_NB_LSHIFT);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vs);
    return qp_capi_from_js_result(ctx, r);
}

/* |v| of a plain int (consumes v). */
static JSValue qp_capi_li_abs(JSContext *ctx, JSValue v)
{
    JSValue r;
    if (JS_IsException(v) || qp_long_sign(v) >= 0)
        return v;
    r = qp_unop(ctx, v, QP_UNOP_NEG);
    JS_FreeValue(ctx, v);
    return r;
}

PyObject *_PyLong_GCD(PyObject *aarg, PyObject *barg)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue a, b, t;

    a = qp_capi_li_abs(ctx, qp_capi_long_js(ctx, aarg));
    if (JS_IsException(a))
        return NULL;
    b = qp_capi_li_abs(ctx, qp_capi_long_js(ctx, barg));
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return NULL;
    }
    /* Euclid on the runtime's ints */
    while (qp_long_sign(b) != 0) {
        t = qp_capi_li_binop(ctx, a, b, QP_NB_REMAINDER);
        JS_FreeValue(ctx, a);
        if (JS_IsException(t)) {
            JS_FreeValue(ctx, b);
            return NULL;
        }
        a = b;
        b = t;
    }
    JS_FreeValue(ctx, b);
    return qp_capi_steal(ctx, a);
}

PyAPI_FUNC(PyObject *) _PyLong_DivmodNear(PyObject *a, PyObject *b);
PyObject *_PyLong_DivmodNear(PyObject *a, PyObject *b)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue va = JS_UNDEFINED, vb = JS_UNDEFINED, q = JS_UNDEFINED, r = JS_UNDEFINED;
    JSValue twice_r = JS_UNDEFINED, one, t, items[2];
    int cmp, b_neg, q_odd;

    if (!PyLong_Check(a) || !PyLong_Check(b)) {
        PyErr_SetString(PyExc_TypeError, "non-integer arguments in division");
        return NULL;
    }
    va = qp_capi_long_js(ctx, a);
    if (JS_IsException(va))
        goto error;
    vb = qp_capi_long_js(ctx, b);
    if (JS_IsException(vb))
        goto error;
    /* q, r = divmod(a, b) */
    q = qp_capi_li_binop(ctx, va, vb, QP_NB_FLOOR_DIVIDE);
    if (JS_IsException(q))
        goto error;
    r = qp_capi_li_binop(ctx, va, vb, QP_NB_REMAINDER);
    if (JS_IsException(r))
        goto error;
    one = JS_NewInt32(ctx, 1);
    twice_r = qp_capi_li_binop(ctx, r, one, QP_NB_LSHIFT);
    if (JS_IsException(twice_r))
        goto error;
    cmp = qp_long_compare(twice_r, vb);
    b_neg = qp_long_sign(vb) < 0;
    t = qp_capi_li_binop(ctx, q, one, QP_NB_AND);
    if (JS_IsException(t))
        goto error;
    q_odd = qp_long_sign(t) != 0;
    JS_FreeValue(ctx, t);
    /* round up if r / b > 0.5, or r / b == 0.5 and q is odd */
    if ((b_neg ? cmp < 0 : cmp > 0) || (cmp == 0 && q_odd)) {
        t = qp_capi_li_binop(ctx, q, one, QP_NB_ADD);
        if (JS_IsException(t))
            goto error;
        JS_FreeValue(ctx, q);
        q = t;
        t = qp_capi_li_binop(ctx, r, vb, QP_NB_SUBTRACT);
        if (JS_IsException(t))
            goto error;
        JS_FreeValue(ctx, r);
        r = t;
    }
    JS_FreeValue(ctx, twice_r);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    items[0] = q;
    items[1] = r;
    return qp_capi_from_js_result(ctx, qp_tuple_new_steal(ctx, 2, items));

error:
    JS_FreeValue(ctx, twice_r);
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    return NULL;
}

PyObject *PyLong_GetInfo(void)
{
    return PySys_GetAttrString("int_info");
}

/* ------------------------------------------------------------------------ */
/* PEP 757: import / export                                                  */
/* ------------------------------------------------------------------------ */

static const PyLongLayout qp_capi_long_layout = {
    .bits_per_digit = PyLong_SHIFT,
    .digit_size = sizeof(digit),
    .digits_order = -1,             /* least significant first */
    .digit_endianness = PY_LITTLE_ENDIAN ? -1 : 1,
};

const PyLongLayout *PyLong_GetNativeLayout(void)
{
    return &qp_capi_long_layout;
}

int PyLong_Export(PyObject *obj, PyLongExport *export_long)
{
    int overflow;
    long value;

    if (!PyLong_Check(obj)) {
        memset(export_long, 0, sizeof(*export_long));
        PyErr_Format(PyExc_TypeError, "expect int, got %T", obj);
        return -1;
    }
    value = PyLong_AsLongAndOverflow(obj, &overflow);
    if (!overflow) {
        export_long->value = value;
        export_long->negative = 0;
        export_long->ndigits = 0;
        export_long->digits = NULL;
        export_long->_reserved = 0;
    } else {
        PyLongObject *self = (PyLongObject *)obj;
        export_long->value = 0;
        export_long->negative = qp_capi_long_is_negative(self);
        export_long->ndigits = qp_capi_long_ndigits(self);
        if (export_long->ndigits == 0)
            export_long->ndigits = 1;
        export_long->digits = self->long_value.ob_digit;
        export_long->_reserved = (Py_uintptr_t)Py_NewRef(obj);
    }
    return 0;
}

void PyLong_FreeExport(PyLongExport *export_long)
{
    PyObject *obj = (PyObject *)export_long->_reserved;
    if (obj) {
        export_long->_reserved = 0;
        Py_DECREF(obj);
    }
}

PyLongWriter *PyLongWriter_Create(int negative, Py_ssize_t ndigits, void **digits)
{
    PyLongObject *obj;

    if (ndigits <= 0) {
        PyErr_SetString(PyExc_ValueError, "ndigits must be positive");
        goto error;
    }
    obj = qp_capi_long_alloc(ndigits);
    if (obj == NULL)
        goto error;
    if (negative)
        qp_capi_long_flip_sign(obj);
    *digits = obj->long_value.ob_digit;
    return (PyLongWriter *)obj;

error:
    *digits = NULL;
    return NULL;
}

void PyLongWriter_Discard(PyLongWriter *writer)
{
    if (writer == NULL)
        return;
    Py_DECREF((PyObject *)writer);
}

PyObject *PyLongWriter_Finish(PyLongWriter *writer)
{
    return qp_capi_long_maybe_small(qp_capi_long_normalize((PyLongObject *)writer));
}

/* ------------------------------------------------------------------------ */
/* bool                                                                      */
/* ------------------------------------------------------------------------ */

PyObject *PyBool_FromLong(long ok)
{
    return ok ? Py_True : Py_False;
}

int (Py_IsTrue)(PyObject *x)
{
    return Py_Is(x, Py_True);
}

int (Py_IsFalse)(PyObject *x)
{
    return Py_Is(x, Py_False);
}

/* ------------------------------------------------------------------------ */
/* float                                                                     */
/* ------------------------------------------------------------------------ */

PyObject *PyFloat_FromDouble(double fval)
{
    PyFloatObject *op = (PyFloatObject *)qp_capi_alloc(&PyFloat_Type, sizeof(PyFloatObject), QP_CAPI_K_FLOAT);
    if (!op)
        return NULL;
    op->ob_fval = fval;
    return (PyObject *)op;
}

double PyFloat_AsDouble(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;
    double d;

    if (op == NULL) {
        PyErr_BadArgument();
        return -1;
    }
    if (PyFloat_Check(op) && (!QP_CAPI_HAS_HEAD(op) || QP_CAPI_HEAD(op)->kind == QP_CAPI_K_FLOAT))
        return PyFloat_AS_DOUBLE(op);
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v))
        return -1;
    if (qp_float_as_double(ctx, v, &d) < 0)
        return -1;
    return d;
}

PyObject *PyFloat_FromString(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv;

    if (!v)
        return qp_capi_num_null_error();
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_float_from_str(ctx, jv));
}

double PyFloat_GetMax(void)
{
    return DBL_MAX;
}

double PyFloat_GetMin(void)
{
    return DBL_MIN;
}

PyObject *PyFloat_GetInfo(void)
{
    return PySys_GetAttrString("float_info");
}

#define QP_CAPI_FLOAT_LITTLE_ENDIAN PY_LITTLE_ENDIAN
#define QP_CAPI_FLOAT_BIG_ENDIAN    PY_BIG_ENDIAN

int PyFloat_Pack2(double x, char *data, int le)
{
    unsigned char *p = (unsigned char *)data;
    unsigned char sign;
    int e;
    double f;
    unsigned short bits;
    int incr = 1;

    if (x == 0.0) {
        sign = (copysign(1.0, x) == -1.0);
        e = 0;
        bits = 0;
    } else if (isinf(x)) {
        sign = (x < 0.0);
        e = 0x1f;
        bits = 0;
    } else if (isnan(x)) {
        uint64_t v;
        sign = (copysign(1.0, x) == -1.0);
        e = 0x1f;
        memcpy(&v, &x, sizeof(v));
        v &= 0xffc0000000000ULL;
        bits = (unsigned short)(v >> 42); /* NaN's type & payload */
        if (!bits)
            bits |= (1 << 9);             /* set qNaN if no payload */
    } else {
        sign = (x < 0.0);
        if (sign)
            x = -x;
        f = frexp(x, &e);
        if (f < 0.5 || f >= 1.0) {
            PyErr_SetString(PyExc_SystemError, "frexp() result out of range");
            return -1;
        }
        /* Normalize f to be in the range [1.0, 2.0) */
        f *= 2.0;
        e--;
        if (e >= 16) {
            goto Overflow;
        } else if (e < -25) {
            /* |x| < 2**-25. Underflow to zero. */
            f = 0.0;
            e = 0;
        } else if (e < -14) {
            /* |x| < 2**-14. Gradual underflow */
            f = ldexp(f, 14 + e);
            e = 0;
        } else {
            e += 15;
            f -= 1.0; /* Get rid of leading 1 */
        }
        f *= 1024.0; /* 2**10 */
        /* Round to even */
        bits = (unsigned short)f; /* Note the truncation */
        if ((f - bits > 0.5) || ((f - bits == 0.5) && (bits % 2 == 1))) {
            ++bits;
            if (bits == 1024) {
                /* The carry propagated out of a string of 10 1 bits. */
                bits = 0;
                ++e;
                if (e == 31)
                    goto Overflow;
            }
        }
    }
    bits |= (e << 10) | (sign << 15);
    if (le) {
        p += 1;
        incr = -1;
    }
    *p = (unsigned char)((bits >> 8) & 0xFF);
    p += incr;
    *p = (unsigned char)(bits & 0xFF);
    return 0;

Overflow:
    PyErr_SetString(PyExc_OverflowError, "float too large to pack with e format");
    return -1;
}

int PyFloat_Pack4(double x, char *data, int le)
{
    unsigned char *p = (unsigned char *)data;
    float y = (float)x;
    int i, incr = 1;
    unsigned char s[sizeof(float)];

    if (isinf(y) && !isinf(x)) {
        PyErr_SetString(PyExc_OverflowError, "float too large to pack with f format");
        return -1;
    }
    /* correct y if x was a sNaN, transformed to qNaN by conversion */
    if (isnan(x)) {
        uint64_t v;
        memcpy(&v, &x, 8);
#ifndef __riscv
        if ((v & (1ULL << 51)) == 0) {
            uint32_t u32;
            memcpy(&u32, &y, 4);
            if (u32 & 0x3fffff)        /* if have payload, make sNaN */
                u32 &= ~(1 << 22);
            memcpy(&y, &u32, 4);
        }
#else
        {
            uint32_t u32;
            memcpy(&u32, &y, 4);
            if (v & (1ULL << 63))
                u32 |= (1U << 31);
            u32 -= (u32 & 0x3fffff);
            u32 += (uint32_t)((v & 0x7ffffffffffffULL) >> 29);
            if ((v & (1ULL << 51)) == 0 && (u32 & 0x3fffff))
                u32 &= ~(1 << 22);
            memcpy(&y, &u32, 4);
        }
#endif
    }
    memcpy(s, &y, sizeof(float));
    if ((QP_CAPI_FLOAT_LITTLE_ENDIAN && !le) || (QP_CAPI_FLOAT_BIG_ENDIAN && le)) {
        p += 3;
        incr = -1;
    }
    for (i = 0; i < 4; i++) {
        *p = s[i];
        p += incr;
    }
    return 0;
}

int PyFloat_Pack8(double x, char *data, int le)
{
    unsigned char *p = (unsigned char *)data;
    unsigned char as_bytes[8];
    const unsigned char *s = as_bytes;
    int i, incr = 1;

    memcpy(as_bytes, &x, 8);
    if ((QP_CAPI_FLOAT_LITTLE_ENDIAN && !le) || (QP_CAPI_FLOAT_BIG_ENDIAN && le)) {
        p += 7;
        incr = -1;
    }
    for (i = 0; i < 8; i++) {
        *p = *s++;
        p += incr;
    }
    return 0;
}

double PyFloat_Unpack2(const char *data, int le)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned char sign;
    int e;
    unsigned int f;
    double x;
    int incr = 1;

    if (le) {
        p += 1;
        incr = -1;
    }
    sign = (*p >> 7) & 1;
    e = (*p & 0x7C) >> 2;
    f = (*p & 0x03) << 8;
    p += incr;
    f |= *p;
    if (e == 0x1f) {
        if (f == 0)
            return sign ? -INFINITY : INFINITY;
        {
            uint64_t v = sign ? 0xfff0000000000000ULL : 0x7ff0000000000000ULL;
            v += (uint64_t)f << 42; /* add NaN's type & payload */
            memcpy(&x, &v, sizeof(v));
            return x;
        }
    }
    x = (double)f / 1024.0;
    if (e == 0) {
        e = -14;
    } else {
        x += 1.0;
        e -= 15;
    }
    x = ldexp(x, e);
    if (sign)
        x = -x;
    return x;
}

double PyFloat_Unpack4(const char *data, int le)
{
    const unsigned char *p = (const unsigned char *)data;
    float x;

    if ((QP_CAPI_FLOAT_LITTLE_ENDIAN && !le) || (QP_CAPI_FLOAT_BIG_ENDIAN && le)) {
        char buf[4];
        char *d = &buf[3];
        int i;
        for (i = 0; i < 4; i++)
            *d-- = *p++;
        memcpy(&x, buf, 4);
    } else {
        memcpy(&x, p, 4);
    }
    /* return sNaN double if x was sNaN float */
    if (isnan(x)) {
        uint32_t v;
        memcpy(&v, &x, 4);
#ifndef __riscv
        if ((v & (1 << 22)) == 0) {
            double y = x; /* will make qNaN double */
            uint64_t u64;
            memcpy(&u64, &y, 8);
            u64 &= ~(1ULL << 51); /* make sNaN */
            memcpy(&y, &u64, 8);
            return y;
        }
#else
        {
            double y = x;
            uint64_t u64;
            memcpy(&u64, &y, 8);
            if ((v & (1 << 22)) == 0)
                u64 &= ~(1ULL << 51);
            if (v & (1U << 31))
                u64 |= (1ULL << 63);
            u64 -= (u64 & 0x7ffffffffffffULL);
            u64 += ((v & 0x3fffffULL) << 29);
            memcpy(&y, &u64, 8);
            return y;
        }
#endif
    }
    return x;
}

double PyFloat_Unpack8(const char *data, int le)
{
    const unsigned char *p = (const unsigned char *)data;
    double x;

    if ((QP_CAPI_FLOAT_LITTLE_ENDIAN && !le) || (QP_CAPI_FLOAT_BIG_ENDIAN && le)) {
        char buf[8];
        char *d = &buf[7];
        int i;
        for (i = 0; i < 8; i++)
            *d-- = *p++;
        memcpy(&x, buf, 8);
    } else {
        memcpy(&x, p, 8);
    }
    return x;
}
#undef QP_CAPI_FLOAT_LITTLE_ENDIAN
#undef QP_CAPI_FLOAT_BIG_ENDIAN

/* ------------------------------------------------------------------------ */
/* complex                                                                   */
/* ------------------------------------------------------------------------ */

Py_complex _Py_c_sum(Py_complex a, Py_complex b)
{
    Py_complex r;
    r.real = a.real + b.real;
    r.imag = a.imag + b.imag;
    return r;
}

Py_complex _Py_c_diff(Py_complex a, Py_complex b)
{
    Py_complex r;
    r.real = a.real - b.real;
    r.imag = a.imag - b.imag;
    return r;
}

/* pycore_complexobject.h: mixed complex/real operands */
Py_complex _Py_cr_sum(Py_complex a, double b)
{
    Py_complex r = a;
    r.real += b;
    return r;
}

Py_complex _Py_cr_diff(Py_complex a, double b)
{
    Py_complex r = a;
    r.real -= b;
    return r;
}

Py_complex _Py_rc_diff(double a, Py_complex b)
{
    Py_complex r;
    r.real = a - b.real;
    r.imag = -b.imag;
    return r;
}

Py_complex _Py_cr_prod(Py_complex a, double b)
{
    Py_complex r = a;
    r.real *= b;
    r.imag *= b;
    return r;
}

Py_complex _Py_cr_quot(Py_complex a, double b)
{
    Py_complex r = a;
    if (b) {
        r.real /= b;
        r.imag /= b;
    } else {
        errno = EDOM;
        r.real = r.imag = 0.0;
    }
    return r;
}

/* _Py_c_quot() with a real dividend */
Py_complex _Py_rc_quot(double a, Py_complex b)
{
    Py_complex r;
    const double abs_breal = b.real < 0 ? -b.real : b.real;
    const double abs_bimag = b.imag < 0 ? -b.imag : b.imag;

    if (abs_breal >= abs_bimag) {
        if (abs_breal == 0.0) {
            errno = EDOM;
            r.real = r.imag = 0.0;
        } else {
            const double ratio = b.imag / b.real;
            const double denom = b.real + b.imag * ratio;
            r.real = a / denom;
            r.imag = (-a * ratio) / denom;
        }
    } else if (abs_bimag >= abs_breal) {
        const double ratio = b.real / b.imag;
        const double denom = b.real * ratio + b.imag;
        r.real = (a * ratio) / denom;
        r.imag = (-a) / denom;
    } else {
        r.real = r.imag = Py_NAN;
    }
    if (isnan(r.real) && isnan(r.imag) && isfinite(a) && (isinf(abs_breal) || isinf(abs_bimag))) {
        const double x = copysign(isinf(b.real) ? 1.0 : 0.0, b.real);
        const double y = copysign(isinf(b.imag) ? 1.0 : 0.0, b.imag);
        r.real = 0.0 * (a * x);
        r.imag = 0.0 * (-a * y);
    }
    return r;
}

Py_complex _Py_c_neg(Py_complex a)
{
    Py_complex r;
    r.real = -a.real;
    r.imag = -a.imag;
    return r;
}

Py_complex _Py_c_prod(Py_complex z, Py_complex w)
{
    double a = z.real, b = z.imag, c = w.real, d = w.imag;
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    Py_complex r = {ac - bd, ad + bc};

    /* Recover infinities that computed as nan+nanj (C11 Annex G.5.1). */
    if (isnan(r.real) && isnan(r.imag)) {
        int recalc = 0;
        if (isinf(a) || isinf(b)) {
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            if (isnan(c))
                c = copysign(0.0, c);
            if (isnan(d))
                d = copysign(0.0, d);
            recalc = 1;
        }
        if (isinf(c) || isinf(d)) {
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            if (isnan(a))
                a = copysign(0.0, a);
            if (isnan(b))
                b = copysign(0.0, b);
            recalc = 1;
        }
        if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            if (isnan(a))
                a = copysign(0.0, a);
            if (isnan(b))
                b = copysign(0.0, b);
            if (isnan(c))
                c = copysign(0.0, c);
            if (isnan(d))
                d = copysign(0.0, d);
            recalc = 1;
        }
        if (recalc) {
            r.real = INFINITY * (a * c - b * d);
            r.imag = INFINITY * (a * d + b * c);
        }
    }
    return r;
}

Py_complex _Py_c_quot(Py_complex a, Py_complex b)
{
    Py_complex r;
    const double abs_breal = b.real < 0 ? -b.real : b.real;
    const double abs_bimag = b.imag < 0 ? -b.imag : b.imag;

    if (abs_breal >= abs_bimag) {
        /* divide tops and bottom by b.real */
        if (abs_breal == 0.0) {
            errno = EDOM;
            r.real = r.imag = 0.0;
        } else {
            const double ratio = b.imag / b.real;
            const double denom = b.real + b.imag * ratio;
            r.real = (a.real + a.imag * ratio) / denom;
            r.imag = (a.imag - a.real * ratio) / denom;
        }
    } else if (abs_bimag >= abs_breal) {
        /* divide tops and bottom by b.imag */
        const double ratio = b.real / b.imag;
        const double denom = b.real * ratio + b.imag;
        r.real = (a.real * ratio + a.imag) / denom;
        r.imag = (a.imag * ratio - a.real) / denom;
    } else {
        /* At least one of b.real or b.imag is a NaN */
        r.real = r.imag = Py_NAN;
    }
    /* Recover infinities and zeros that computed as nan+nanj (C11 G.5.2). */
    if (isnan(r.real) && isnan(r.imag)) {
        if ((isinf(a.real) || isinf(a.imag)) && isfinite(b.real) && isfinite(b.imag)) {
            const double x = copysign(isinf(a.real) ? 1.0 : 0.0, a.real);
            const double y = copysign(isinf(a.imag) ? 1.0 : 0.0, a.imag);
            r.real = INFINITY * (x * b.real + y * b.imag);
            r.imag = INFINITY * (y * b.real - x * b.imag);
        } else if ((isinf(abs_breal) || isinf(abs_bimag)) && isfinite(a.real) && isfinite(a.imag)) {
            const double x = copysign(isinf(b.real) ? 1.0 : 0.0, b.real);
            const double y = copysign(isinf(b.imag) ? 1.0 : 0.0, b.imag);
            r.real = 0.0 * (a.real * x + a.imag * y);
            r.imag = 0.0 * (a.imag * x - a.real * y);
        }
    }
    return r;
}

Py_complex _Py_c_pow(Py_complex a, Py_complex b)
{
    Py_complex r;
    double vabs, len, at, phase;

    if (b.real == 0. && b.imag == 0.) {
        r.real = 1.;
        r.imag = 0.;
    } else if (a.real == 0. && a.imag == 0.) {
        if (b.imag != 0. || b.real < 0.)
            errno = EDOM;
        r.real = 0.;
        r.imag = 0.;
    } else {
        vabs = hypot(a.real, a.imag);
        len = pow(vabs, b.real);
        at = atan2(a.imag, a.real);
        phase = at * b.real;
        if (b.imag != 0.0) {
            len *= exp(-at * b.imag);
            phase += b.imag * log(vabs);
        }
        r.real = len * cos(phase);
        r.imag = len * sin(phase);
        /* _Py_ADJUST_ERANGE2 */
        if (r.real == INFINITY || r.real == -INFINITY || r.imag == INFINITY || r.imag == -INFINITY) {
            if (errno == 0)
                errno = ERANGE;
        } else if (errno == ERANGE) {
            errno = 0;
        }
    }
    return r;
}

double _Py_c_abs(Py_complex z)
{
    /* sets errno = ERANGE on overflow;  otherwise errno = 0 */
    double result;

    if (!isfinite(z.real) || !isfinite(z.imag)) {
        /* C99 rules: an infinite part gives infinity, even with a NaN */
        if (isinf(z.real)) {
            errno = 0;
            return fabs(z.real);
        }
        if (isinf(z.imag)) {
            errno = 0;
            return fabs(z.imag);
        }
        return Py_NAN;
    }
    result = hypot(z.real, z.imag);
    errno = isfinite(result) ? 0 : ERANGE;
    return result;
}

PyObject *PyComplex_FromCComplex(Py_complex cval)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_complex_new(ctx, cval.real, cval.imag));
}

PyObject *PyComplex_FromDoubles(double real, double imag)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_complex_new(ctx, real, imag));
}

/* complex value of op: the mirror field for complex instances, else
   __complex__ / __float__ / __index__ (PyComplex_AsCComplex). */
static int qp_capi_complex_value(PyObject *op, Py_complex *out)
{
    JSContext *ctx = qp_capi.ctx;
    QPComplex c;
    JSValue v;

    if (PyComplex_Check(op) && (!QP_CAPI_HAS_HEAD(op) || QP_CAPI_HEAD(op)->kind == QP_CAPI_K_COMPLEX)) {
        *out = ((PyComplexObject *)op)->cval;
        return 0;
    }
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v))
        return -1;
    if (qp_complex_as_c(ctx, v, &c) < 0)
        return -1;
    out->real = c.real;
    out->imag = c.imag;
    return 0;
}

double PyComplex_RealAsDouble(PyObject *op)
{
    Py_complex c;
    if (qp_capi_complex_value(op, &c) < 0)
        return -1.0;
    return c.real;
}

double PyComplex_ImagAsDouble(PyObject *op)
{
    Py_complex c;
    if (qp_capi_complex_value(op, &c) < 0)
        return -1.0;
    return c.imag;
}

Py_complex PyComplex_AsCComplex(PyObject *op)
{
    Py_complex cv;
    if (qp_capi_complex_value(op, &cv) < 0) {
        cv.real = -1.;
        cv.imag = 0.;
    }
    return cv;
}

/* ------------------------------------------------------------------------ */
/* pystrtod.h                                                                */
/* ------------------------------------------------------------------------ */

char *PyOS_double_to_string(double val, char format_code, int precision, int flags, int *type)
{
    qp_error err;
    char *s, *r;
    size_t len;

    switch (format_code) {
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
        break;
    case 'r':
        if (precision == 0)
            break;
        /* fall through */
    default:
        PyErr_BadInternalCall();
        return NULL;
    }
    memset(&err, 0, sizeof(err));
    s = qp_double_to_string(val, format_code, precision, flags, type, &err);
    if (!s) {
        if (err.kind == QP_ERR_NONE || err.kind == QP_ERR_MEMORY)
            PyErr_NoMemory();
        else
            qp_raise_qp_error(qp_capi.ctx, &err, NULL);
        qp_error_clear(&err);
        return NULL;
    }
    /* the caller releases the result with PyMem_Free */
    len = strlen(s);
    r = PyMem_Malloc(len + 1);
    if (r)
        memcpy(r, s, len + 1);
    else
        PyErr_NoMemory();
    qp_sys_free(s);
    return r;
}

double PyOS_string_to_double(const char *s, char **endptr, PyObject *overflow_exception)
{
    qp_error err;
    double x;

    memset(&err, 0, sizeof(err));
    x = qp_string_to_double(s, endptr, overflow_exception ? QP_ERR_OVERFLOW : QP_ERR_NONE, &err);
    if (err.kind != QP_ERR_NONE) {
        if (err.kind == QP_ERR_OVERFLOW && overflow_exception)
            PyErr_SetString(overflow_exception, err.msg ? err.msg : "");
        else if (err.kind == QP_ERR_MEMORY)
            PyErr_NoMemory();
        else
            qp_raise_qp_error(qp_capi.ctx, &err, NULL);
        qp_error_clear(&err);
        return -1.0;
    }
    return x;
}
