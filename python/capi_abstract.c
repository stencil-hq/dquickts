/*
 * dquickts Python C API (plan D5, unit C): the number, sequence, mapping
 * and iterator protocols of abstract.h (PyNumber_*, PySequence_*,
 * PyMapping_*, PyIter_*, PyAIter_Check), pycore_abstract.h
 * (_PyNumber_Index, _Py_convert_optional_to_ssize_t) and iterobject.h
 * (PySeqIter_New, PyCallIter_New).
 *
 * Derived from CPython 3.15.2 Objects/abstract.c (PSF-2.0).
 *
 * The protocols run on the JS values of the arguments and the QPTypeSlots
 * of their types (the same slots the interpreter uses; C extension types
 * reach their C slots through B's forward bridges), so semantics and
 * messages are those of M2's abstract.c.  The generic PyObject_* entries
 * of abstract.h (GetItem, Size, GetIter, ...) belong to unit A.
 */

/* QPType of op: from Py_TYPE() for unbound (C-primary) objects, without
   giving them a JS identity; else the type of the JS value (which follows
   __class__ assignment).  NULL with an exception if op cannot be bound. */
static QPType *qp_capi_abs_qptype(JSContext *ctx, PyObject *op)
{
    JSValue v;

    if (QP_CAPI_HAS_HEAD(op) && QP_IS_NULL(QP_CAPI_HEAD(op)->v)) {
        QPType *t = qp_capi_type_qp(Py_TYPE(op));
        if (t)
            return t;
    }
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v))
        return NULL;
    return qp_type_of(ctx, v);
}

/* JS value of an optional argument: QP_NULL for NULL. */
static int qp_capi_abs_value_opt(JSContext *ctx, PyObject *op, JSValue *pv)
{
    if (!op) {
        *pv = QP_NULL;
        return 0;
    }
    *pv = qp_capi_value(ctx, op);
    return JS_IsException(*pv) ? -1 : 0;
}

/* New reference result of a QP slot (QP_NULL is a caller bug here). */
static PyObject *qp_capi_abs_slot_result(JSContext *ctx, JSValue r)
{
    if (QP_IS_NULL(r)) {
        PyErr_SetString(PyExc_SystemError, "slot returned NULL without setting an exception");
        return NULL;
    }
    return qp_capi_from_js_result(ctx, r);
}

/* ------------------------------------------------------------------------ */
/* Number protocol                                                           */
/* ------------------------------------------------------------------------ */

#define QP_CAPI_NB_BINARY(m, off) (*(QPBinaryFunc **)((char *)&(m) + (off)))
#define QP_CAPI_NB_TERNARY(m, off) (*(QPTernaryFunc **)((char *)&(m) + (off)))
#define QP_CAPI_NB_SLOT(name) offsetof(QPNumberMethods, name)

/* binary_op1(): v op w over the binary slot at `off` of QPNumberMethods;
   may return NotImplemented (new reference). */
static JSValue qp_capi_binary_op1(JSContext *ctx, JSValueConst v, JSValueConst w, size_t off)
{
    QPType *tv = qp_type_of(ctx, v), *tw = qp_type_of(ctx, w);
    QPBinaryFunc *slotv = QP_CAPI_NB_BINARY(tv->slots.nb, off), *slotw = NULL;
    JSValue x;

    if (tw != tv) {
        slotw = QP_CAPI_NB_BINARY(tw->slots.nb, off);
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && qp_type_is_subtype(tw, tv)) {
            x = slotw(ctx, v, w);
            if (!qp_is_notimpl(ctx, x))
                return x;
            JS_FreeValue(ctx, x);
            slotw = NULL;
        }
        x = slotv(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    if (slotw) {
        x = slotw(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    return qp_not_implemented(ctx);
}

/* binary_iop1(): in-place slot first, then binary_op1. */
static JSValue qp_capi_binary_iop1(JSContext *ctx, JSValueConst v, JSValueConst w,
                                   size_t ioff, size_t off)
{
    QPBinaryFunc *slot = QP_CAPI_NB_BINARY(qp_type_of(ctx, v)->slots.nb, ioff);

    if (slot) {
        JSValue x = slot(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    return qp_capi_binary_op1(ctx, v, w, off);
}

/* ternary_op() over the ternary slot at `off` (z may be None). */
static PyObject *qp_capi_ternary_op(PyObject *v, PyObject *w, PyObject *z, size_t off,
                                    const char *op_name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv, jw, jz, x;
    QPType *tv, *tw, *tz;
    QPTernaryFunc *slotv, *slotw = NULL, *slotz;

    if (!v || !w || !z)
        return qp_capi_num_null_error();
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    jw = qp_capi_value(ctx, w);
    if (JS_IsException(jw))
        return NULL;
    jz = qp_capi_value(ctx, z);
    if (JS_IsException(jz))
        return NULL;
    tv = qp_type_of(ctx, jv);
    tw = qp_type_of(ctx, jw);
    slotv = QP_CAPI_NB_TERNARY(tv->slots.nb, off);
    if (tw != tv) {
        slotw = QP_CAPI_NB_TERNARY(tw->slots.nb, off);
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && qp_type_is_subtype(tw, tv)) {
            x = slotw(ctx, jv, jw, jz);
            if (!qp_is_notimpl(ctx, x))
                return qp_capi_abs_slot_result(ctx, x);
            JS_FreeValue(ctx, x);
            slotw = NULL;
        }
        x = slotv(ctx, jv, jw, jz);
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    if (slotw) {
        x = slotw(ctx, jv, jw, jz);
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    tz = qp_type_of(ctx, jz);
    slotz = QP_CAPI_NB_TERNARY(tz->slots.nb, off);
    if (slotz == slotv || slotz == slotw)
        slotz = NULL;
    if (slotz) {
        x = slotz(ctx, jv, jw, jz);
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: '%.100s', '%.100s', '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name, Py_TYPE(z)->tp_name);
    return NULL;
}

/* v op w with the interpreter's BINARY_OP semantics (QP_NB_*). */
static PyObject *qp_capi_abs_binop(PyObject *v, PyObject *w, int nb)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv, jw;

    if (!v || !w)
        return qp_capi_num_null_error();
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    jw = qp_capi_value(ctx, w);
    if (JS_IsException(jw))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_binop(ctx, jv, jw, nb));
}

static PyObject *qp_capi_abs_unop(PyObject *o, int unop)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (!o)
        return qp_capi_num_null_error();
    v = qp_capi_value(ctx, o);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_unop(ctx, v, unop));
}

int PyNumber_Check(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t;

    if (o == NULL)
        return 0;
    t = qp_capi_abs_qptype(ctx, o);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    return t->slots.nb.nb_index || t->slots.nb.nb_int || t->slots.nb.nb_float || PyComplex_Check(o);
}

PyObject *PyNumber_Add(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_ADD); }
PyObject *PyNumber_Subtract(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_SUBTRACT); }
PyObject *PyNumber_Multiply(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_MULTIPLY); }
PyObject *PyNumber_MatrixMultiply(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_MATRIX_MULTIPLY); }
PyObject *PyNumber_FloorDivide(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_FLOOR_DIVIDE); }
PyObject *PyNumber_TrueDivide(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_TRUE_DIVIDE); }
PyObject *PyNumber_Remainder(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_REMAINDER); }
PyObject *PyNumber_Lshift(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_LSHIFT); }
PyObject *PyNumber_Rshift(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_RSHIFT); }
PyObject *PyNumber_And(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_AND); }
PyObject *PyNumber_Xor(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_XOR); }
PyObject *PyNumber_Or(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_OR); }

PyObject *PyNumber_InPlaceAdd(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_ADD); }
PyObject *PyNumber_InPlaceSubtract(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_SUBTRACT); }
PyObject *PyNumber_InPlaceMultiply(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_MULTIPLY); }
PyObject *PyNumber_InPlaceMatrixMultiply(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_MATRIX_MULTIPLY); }
PyObject *PyNumber_InPlaceFloorDivide(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_FLOOR_DIVIDE); }
PyObject *PyNumber_InPlaceTrueDivide(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_TRUE_DIVIDE); }
PyObject *PyNumber_InPlaceRemainder(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_REMAINDER); }
PyObject *PyNumber_InPlaceLshift(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_LSHIFT); }
PyObject *PyNumber_InPlaceRshift(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_RSHIFT); }
PyObject *PyNumber_InPlaceAnd(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_AND); }
PyObject *PyNumber_InPlaceXor(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_XOR); }
PyObject *PyNumber_InPlaceOr(PyObject *v, PyObject *w) { return qp_capi_abs_binop(v, w, QP_NB_INPLACE_OR); }

PyObject *PyNumber_Divmod(PyObject *v, PyObject *w)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv, jw, x;

    if (!v || !w)
        return qp_capi_num_null_error();
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    jw = qp_capi_value(ctx, w);
    if (JS_IsException(jw))
        return NULL;
    x = qp_capi_binary_op1(ctx, jv, jw, QP_CAPI_NB_SLOT(nb_divmod));
    if (qp_is_notimpl(ctx, x)) {
        JS_FreeValue(ctx, x);
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
                     "divmod()", Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
        return NULL;
    }
    return qp_capi_abs_slot_result(ctx, x);
}

PyObject *PyNumber_Power(PyObject *v, PyObject *w, PyObject *z)
{
    return qp_capi_ternary_op(v, w, z, QP_CAPI_NB_SLOT(nb_power), "** or pow()");
}

PyObject *PyNumber_InPlacePower(PyObject *v, PyObject *w, PyObject *z)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv, jw, jz, x;
    QPType *t;

    if (!v || !w || !z)
        return qp_capi_num_null_error();
    if (z == Py_None)
        return qp_capi_abs_binop(v, w, QP_NB_INPLACE_POWER);
    /* ternary_iop(): nb_inplace_power, then ternary_op(nb_power) */
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    jw = qp_capi_value(ctx, w);
    if (JS_IsException(jw))
        return NULL;
    jz = qp_capi_value(ctx, z);
    if (JS_IsException(jz))
        return NULL;
    t = qp_type_of(ctx, jv);
    if (t->slots.nb.nb_inplace_power) {
        x = t->slots.nb.nb_inplace_power(ctx, jv, jw, jz);
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    return qp_capi_ternary_op(v, w, z, QP_CAPI_NB_SLOT(nb_power), "**=");
}

PyObject *PyNumber_Negative(PyObject *o) { return qp_capi_abs_unop(o, QP_UNOP_NEG); }
PyObject *PyNumber_Positive(PyObject *o) { return qp_capi_abs_unop(o, QP_UNOP_POS); }
PyObject *PyNumber_Invert(PyObject *o) { return qp_capi_abs_unop(o, QP_UNOP_INVERT); }

PyObject *PyNumber_Absolute(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;
    QPType *t;

    if (!o)
        return qp_capi_num_null_error();
    v = qp_capi_value(ctx, o);
    if (JS_IsException(v))
        return NULL;
    t = qp_type_of(ctx, v);
    if (t->slots.nb.nb_absolute)
        return qp_capi_abs_slot_result(ctx, t->slots.nb.nb_absolute(ctx, v));
    PyErr_Format(PyExc_TypeError, "bad operand type for abs(): '%.200s'", Py_TYPE(o)->tp_name);
    return NULL;
}

/* PyNumber_Index without the exact-int conversion: an int (subclass
   included) is returned as is. */
PyAPI_FUNC(PyObject *) _PyNumber_Index(PyObject *item);
PyObject *_PyNumber_Index(PyObject *item)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (item == NULL)
        return qp_capi_num_null_error();
    if (PyLong_Check(item))
        return Py_NewRef(item);
    v = qp_capi_value(ctx, item);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_index(ctx, v));
}

PyObject *PyNumber_Index(PyObject *item)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (item == NULL)
        return qp_capi_num_null_error();
    if (PyLong_CheckExact(item))
        return Py_NewRef(item);
    v = qp_capi_value(ctx, item);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_index(ctx, v));
}

Py_ssize_t PyNumber_AsSsize_t(PyObject *item, PyObject *err)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *exc_type = NULL;
    Py_ssize_t out;
    JSValue v;

    if (item == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (err) {
        exc_type = qp_capi_type_qp((PyTypeObject *)err);
        if (!exc_type) {
            PyErr_BadInternalCall();
            return -1;
        }
    }
    v = qp_capi_value(ctx, item);
    if (JS_IsException(v))
        return -1;
    if (qp_as_ssize(ctx, v, exc_type, &out) < 0)
        return -1;
    return out;
}

PyObject *PyNumber_Long(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (o == NULL)
        return qp_capi_num_null_error();
    if (PyLong_CheckExact(o))
        return Py_NewRef(o);
    v = qp_capi_value(ctx, o);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_number_long(ctx, v));
}

PyObject *PyNumber_Float(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (o == NULL)
        return qp_capi_num_null_error();
    if (PyFloat_CheckExact(o))
        return Py_NewRef(o);
    v = qp_capi_value(ctx, o);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_number_float(ctx, v));
}

PyObject *PyNumber_ToBase(PyObject *n, int base)
{
    PyObject *index, *res;

    if (!(base == 2 || base == 8 || base == 10 || base == 16)) {
        PyErr_SetString(PyExc_SystemError, "PyNumber_ToBase: base must be 2, 8, 10 or 16");
        return NULL;
    }
    index = PyNumber_Index(n);
    if (!index)
        return NULL;
    res = _PyLong_Format(index, base);
    Py_DECREF(index);
    return res;
}

PyAPI_FUNC(int) _Py_convert_optional_to_ssize_t(PyObject *obj, void *result);
int _Py_convert_optional_to_ssize_t(PyObject *obj, void *result)
{
    JSContext *ctx = qp_capi.ctx;
    Py_ssize_t limit;
    QPType *t;

    if (obj == Py_None)
        return 1;
    t = qp_capi_abs_qptype(ctx, obj);
    if (!t)
        return 0;
    if (t->slots.nb.nb_index) {
        limit = PyNumber_AsSsize_t(obj, PyExc_OverflowError);
        if (limit == -1 && PyErr_Occurred())
            return 0;
    } else {
        PyErr_Format(PyExc_TypeError, "argument should be integer or None, not '%.200s'",
                     Py_TYPE(obj)->tp_name);
        return 0;
    }
    *((Py_ssize_t *)result) = limit;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Sequence protocol                                                         */
/* ------------------------------------------------------------------------ */

/* type of s and its JS value; 0 or -1 */
static int qp_capi_abs_typed_value(JSContext *ctx, PyObject *s, JSValue *pv, QPType **pt)
{
    *pv = qp_capi_value(ctx, s);
    if (JS_IsException(*pv))
        return -1;
    *pt = qp_type_of(ctx, *pv);
    return 0;
}

static int qp_capi_sequence_check_qp(QPType *t, PyObject *s)
{
    if (PyDict_Check(s))
        return 0;
    return t->slots.sq.sq_item != NULL;
}

int PySequence_Check(PyObject *s)
{
    QPType *t;

    if (s == NULL)
        return 0;
    t = qp_capi_abs_qptype(qp_capi.ctx, s);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    return qp_capi_sequence_check_qp(t, s);
}

Py_ssize_t PySequence_Size(PyObject *s)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;
    QPType *t;

    if (s == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (qp_capi_abs_typed_value(ctx, s, &v, &t) < 0)
        return -1;
    if (t->slots.sq.sq_length)
        return t->slots.sq.sq_length(ctx, v);
    if (t->slots.mp.mp_length)
        PyErr_Format(PyExc_TypeError, "%.200s is not a sequence", Py_TYPE(s)->tp_name);
    else
        PyErr_Format(PyExc_TypeError, "object of type '%.200s' has no len()", Py_TYPE(s)->tp_name);
    return -1;
}

#undef PySequence_Length
Py_ssize_t PySequence_Length(PyObject *s)
{
    return PySequence_Size(s);
}
#define PySequence_Length PySequence_Size

/* concat / in-place concat (inplace selects sq_inplace_concat and the
   nb_inplace_add fallback). */
static PyObject *qp_capi_sequence_concat(PyObject *s, PyObject *o, int inplace)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue vs, vo, x;
    QPType *ts, *to;

    if (s == NULL || o == NULL)
        return qp_capi_num_null_error();
    if (qp_capi_abs_typed_value(ctx, s, &vs, &ts) < 0 || qp_capi_abs_typed_value(ctx, o, &vo, &to) < 0)
        return NULL;
    if (inplace && ts->slots.sq.sq_inplace_concat)
        return qp_capi_abs_slot_result(ctx, ts->slots.sq.sq_inplace_concat(ctx, vs, vo));
    if (ts->slots.sq.sq_concat)
        return qp_capi_abs_slot_result(ctx, ts->slots.sq.sq_concat(ctx, vs, vo));
    /* Instances of user classes defining __add__() only have nb_add: fall
       back to it if both arguments appear to be sequences. */
    if (qp_capi_sequence_check_qp(ts, s) && qp_capi_sequence_check_qp(to, o)) {
        if (inplace)
            x = qp_capi_binary_iop1(ctx, vs, vo, QP_CAPI_NB_SLOT(nb_inplace_add), QP_CAPI_NB_SLOT(nb_add));
        else
            x = qp_capi_binary_op1(ctx, vs, vo, QP_CAPI_NB_SLOT(nb_add));
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    PyErr_Format(PyExc_TypeError, "'%.200s' object can't be concatenated", Py_TYPE(s)->tp_name);
    return NULL;
}

PyObject *PySequence_Concat(PyObject *s, PyObject *o)
{
    return qp_capi_sequence_concat(s, o, 0);
}

PyObject *PySequence_InPlaceConcat(PyObject *s, PyObject *o)
{
    return qp_capi_sequence_concat(s, o, 1);
}

static PyObject *qp_capi_sequence_repeat(PyObject *o, Py_ssize_t count, int inplace)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue vo, n, x;
    QPType *t;

    if (o == NULL)
        return qp_capi_num_null_error();
    if (qp_capi_abs_typed_value(ctx, o, &vo, &t) < 0)
        return NULL;
    if (inplace && t->slots.sq.sq_inplace_repeat)
        return qp_capi_abs_slot_result(ctx, t->slots.sq.sq_inplace_repeat(ctx, vo, count));
    if (t->slots.sq.sq_repeat)
        return qp_capi_abs_slot_result(ctx, t->slots.sq.sq_repeat(ctx, vo, count));
    /* user classes defining __mul__() only have nb_multiply */
    if (qp_capi_sequence_check_qp(t, o)) {
        n = qp_long_from_int64(ctx, count);
        if (JS_IsException(n))
            return NULL;
        if (inplace)
            x = qp_capi_binary_iop1(ctx, vo, n, QP_CAPI_NB_SLOT(nb_inplace_multiply),
                                    QP_CAPI_NB_SLOT(nb_multiply));
        else
            x = qp_capi_binary_op1(ctx, vo, n, QP_CAPI_NB_SLOT(nb_multiply));
        JS_FreeValue(ctx, n);
        if (!qp_is_notimpl(ctx, x))
            return qp_capi_abs_slot_result(ctx, x);
        JS_FreeValue(ctx, x);
    }
    PyErr_Format(PyExc_TypeError, "'%.200s' object can't be repeated", Py_TYPE(o)->tp_name);
    return NULL;
}

PyObject *PySequence_Repeat(PyObject *o, Py_ssize_t count)
{
    return qp_capi_sequence_repeat(o, count, 0);
}

PyObject *PySequence_InPlaceRepeat(PyObject *o, Py_ssize_t count)
{
    return qp_capi_sequence_repeat(o, count, 1);
}

/* negative index adjustment through sq_length */
static int qp_capi_sequence_adjust(JSContext *ctx, QPType *t, JSValueConst v, Py_ssize_t *pi)
{
    if (*pi < 0 && t->slots.sq.sq_length) {
        Py_ssize_t l = t->slots.sq.sq_length(ctx, v);
        if (l < 0)
            return -1;
        *pi += l;
    }
    return 0;
}

PyObject *PySequence_GetItem(PyObject *s, Py_ssize_t i)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;
    QPType *t;

    if (s == NULL)
        return qp_capi_num_null_error();
    if (qp_capi_abs_typed_value(ctx, s, &v, &t) < 0)
        return NULL;
    if (t->slots.sq.sq_item) {
        if (qp_capi_sequence_adjust(ctx, t, v, &i) < 0)
            return NULL;
        return qp_capi_abs_slot_result(ctx, t->slots.sq.sq_item(ctx, v, i));
    }
    if (t->slots.mp.mp_subscript)
        PyErr_Format(PyExc_TypeError, "%.200s is not a sequence", Py_TYPE(s)->tp_name);
    else
        PyErr_Format(PyExc_TypeError, "'%.200s' object does not support indexing", Py_TYPE(s)->tp_name);
    return NULL;
}

/* _PySlice_FromIndices(i1, i2) as a JS slice */
static JSValue qp_capi_slice_from_indices(JSContext *ctx, Py_ssize_t i1, Py_ssize_t i2)
{
    JSValue a, b, r;

    a = qp_long_from_int64(ctx, i1);
    if (JS_IsException(a))
        return a;
    b = qp_long_from_int64(ctx, i2);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    r = qp_slice_new(ctx, a, b, JS_UNDEFINED);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return r;
}

PyObject *PySequence_GetSlice(PyObject *s, Py_ssize_t i1, Py_ssize_t i2)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, slice, r;
    QPType *t;

    if (!s)
        return qp_capi_num_null_error();
    if (qp_capi_abs_typed_value(ctx, s, &v, &t) < 0)
        return NULL;
    if (t->slots.mp.mp_subscript) {
        slice = qp_capi_slice_from_indices(ctx, i1, i2);
        if (JS_IsException(slice))
            return NULL;
        r = t->slots.mp.mp_subscript(ctx, v, slice);
        JS_FreeValue(ctx, slice);
        return qp_capi_abs_slot_result(ctx, r);
    }
    PyErr_Format(PyExc_TypeError, "'%.200s' object is unsliceable", Py_TYPE(s)->tp_name);
    return NULL;
}

/* sq_ass_item with value o (NULL deletes) */
static int qp_capi_sequence_ass_item(PyObject *s, Py_ssize_t i, PyObject *o, const char *what)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, vo;
    QPType *t;

    if (s == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (qp_capi_abs_typed_value(ctx, s, &v, &t) < 0 || qp_capi_abs_value_opt(ctx, o, &vo) < 0)
        return -1;
    if (t->slots.sq.sq_ass_item) {
        if (qp_capi_sequence_adjust(ctx, t, v, &i) < 0)
            return -1;
        return t->slots.sq.sq_ass_item(ctx, v, i, vo);
    }
    if (t->slots.mp.mp_ass_subscript)
        PyErr_Format(PyExc_TypeError, "%.200s is not a sequence", Py_TYPE(s)->tp_name);
    else
        PyErr_Format(PyExc_TypeError, what, Py_TYPE(s)->tp_name);
    return -1;
}

int PySequence_SetItem(PyObject *s, Py_ssize_t i, PyObject *o)
{
    return qp_capi_sequence_ass_item(s, i, o, "'%.200s' object does not support item assignment");
}

int PySequence_DelItem(PyObject *s, Py_ssize_t i)
{
    return qp_capi_sequence_ass_item(s, i, NULL, "'%.200s' object doesn't support item deletion");
}

static int qp_capi_sequence_ass_slice(PyObject *s, Py_ssize_t i1, Py_ssize_t i2, PyObject *o,
                                      const char *what)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, vo, slice;
    QPType *t;
    int res;

    if (s == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (qp_capi_abs_typed_value(ctx, s, &v, &t) < 0 || qp_capi_abs_value_opt(ctx, o, &vo) < 0)
        return -1;
    if (t->slots.mp.mp_ass_subscript) {
        slice = qp_capi_slice_from_indices(ctx, i1, i2);
        if (JS_IsException(slice))
            return -1;
        res = t->slots.mp.mp_ass_subscript(ctx, v, slice, vo);
        JS_FreeValue(ctx, slice);
        return res;
    }
    PyErr_Format(PyExc_TypeError, what, Py_TYPE(s)->tp_name);
    return -1;
}

int PySequence_SetSlice(PyObject *s, Py_ssize_t i1, Py_ssize_t i2, PyObject *o)
{
    return qp_capi_sequence_ass_slice(s, i1, i2, o, "'%.200s' object doesn't support slice assignment");
}

int PySequence_DelSlice(PyObject *s, Py_ssize_t i1, Py_ssize_t i2)
{
    return qp_capi_sequence_ass_slice(s, i1, i2, NULL, "'%.200s' object doesn't support slice deletion");
}

PyObject *PySequence_Tuple(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv;

    if (v == NULL)
        return qp_capi_num_null_error();
    if (PyTuple_CheckExact(v))
        return Py_NewRef(v);
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_tuple_from_iterable(ctx, jv));
}

PyObject *PySequence_List(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv;

    if (v == NULL)
        return qp_capi_num_null_error();
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_list_from_iterable(ctx, jv));
}

/* Replace a pending TypeError by TypeError(msg) (PySequence_Fast,
   _PySequence_IterSearch). */
static void qp_capi_retype_type_error(const char *fmt, PyObject *o)
{
    if (PyErr_ExceptionMatches(PyExc_TypeError)) {
        PyErr_Clear();
        if (o)
            PyErr_Format(PyExc_TypeError, fmt, Py_TYPE(o)->tp_name);
        else
            PyErr_SetString(PyExc_TypeError, fmt);
    }
}

PyObject *PySequence_Fast(PyObject *v, const char *m)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jv, it, list;

    if (v == NULL)
        return qp_capi_num_null_error();
    if (PyList_CheckExact(v) || PyTuple_CheckExact(v))
        return Py_NewRef(v);
    jv = qp_capi_value(ctx, v);
    if (JS_IsException(jv))
        return NULL;
    it = qp_iter(ctx, jv);
    if (JS_IsException(it)) {
        qp_capi_retype_type_error(m, NULL);
        return NULL;
    }
    list = qp_list_from_iterable(ctx, it);
    JS_FreeValue(ctx, it);
    return qp_capi_from_js_result(ctx, list);
}

#define QP_CAPI_ITERSEARCH_COUNT    1
#define QP_CAPI_ITERSEARCH_INDEX    2
#define QP_CAPI_ITERSEARCH_CONTAINS 3

/* _PySequence_IterSearch() */
static Py_ssize_t qp_capi_iter_search(PyObject *seq, PyObject *obj, int operation)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jseq, jobj, it, item;
    Py_ssize_t n = 0;
    int wrapped = 0, cmp;

    if (seq == NULL || obj == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    jseq = qp_capi_value(ctx, seq);
    if (JS_IsException(jseq))
        return -1;
    jobj = qp_capi_value(ctx, obj);
    if (JS_IsException(jobj))
        return -1;
    it = qp_iter(ctx, jseq);
    if (JS_IsException(it)) {
        if (operation == QP_CAPI_ITERSEARCH_CONTAINS)
            qp_capi_retype_type_error("argument of type '%.200s' is not a container or iterable", seq);
        else
            qp_capi_retype_type_error("argument of type '%.200s' is not iterable", seq);
        return -1;
    }
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item))
            goto Fail;
        if (QP_IS_NULL(item))
            break;
        cmp = qp_richcompare_bool(ctx, item, jobj, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (cmp < 0)
            goto Fail;
        if (cmp > 0) {
            switch (operation) {
            case QP_CAPI_ITERSEARCH_COUNT:
                if (n == PY_SSIZE_T_MAX) {
                    PyErr_SetString(PyExc_OverflowError, "count exceeds C integer size");
                    goto Fail;
                }
                ++n;
                break;
            case QP_CAPI_ITERSEARCH_INDEX:
                if (wrapped) {
                    PyErr_SetString(PyExc_OverflowError, "index exceeds C integer size");
                    goto Fail;
                }
                goto Done;
            default: /* QP_CAPI_ITERSEARCH_CONTAINS */
                n = 1;
                goto Done;
            }
        }
        if (operation == QP_CAPI_ITERSEARCH_INDEX) {
            if (n == PY_SSIZE_T_MAX)
                wrapped = 1;
            ++n;
        }
    }
    if (operation != QP_CAPI_ITERSEARCH_INDEX)
        goto Done;
    PyErr_SetString(PyExc_ValueError, "sequence.index(x): x not in sequence");
Fail:
    n = -1;
Done:
    JS_FreeValue(ctx, it);
    return n;
}

Py_ssize_t PySequence_Count(PyObject *s, PyObject *o)
{
    return qp_capi_iter_search(s, o, QP_CAPI_ITERSEARCH_COUNT);
}

Py_ssize_t PySequence_Index(PyObject *s, PyObject *o)
{
    return qp_capi_iter_search(s, o, QP_CAPI_ITERSEARCH_INDEX);
}

int PySequence_Contains(PyObject *seq, PyObject *ob)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue vs, vo;
    QPType *t;

    if (seq == NULL || ob == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (qp_capi_abs_typed_value(ctx, seq, &vs, &t) < 0)
        return -1;
    if (t->slots.sq.sq_contains) {
        vo = qp_capi_value(ctx, ob);
        if (JS_IsException(vo))
            return -1;
        return t->slots.sq.sq_contains(ctx, vs, vo);
    }
    return (int)qp_capi_iter_search(seq, ob, QP_CAPI_ITERSEARCH_CONTAINS);
}

#undef PySequence_In
int PySequence_In(PyObject *w, PyObject *v)
{
    return PySequence_Contains(w, v);
}
#define PySequence_In PySequence_Contains

/* ------------------------------------------------------------------------ */
/* Mapping protocol                                                          */
/* ------------------------------------------------------------------------ */

int PyMapping_Check(PyObject *o)
{
    QPType *t;

    if (o == NULL)
        return 0;
    t = qp_capi_abs_qptype(qp_capi.ctx, o);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    return t->slots.mp.mp_subscript != NULL;
}

Py_ssize_t PyMapping_Size(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;
    QPType *t;

    if (o == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    if (qp_capi_abs_typed_value(ctx, o, &v, &t) < 0)
        return -1;
    if (t->slots.mp.mp_length)
        return t->slots.mp.mp_length(ctx, v);
    if (t->slots.sq.sq_length)
        PyErr_Format(PyExc_TypeError, "%.200s is not a mapping", Py_TYPE(o)->tp_name);
    else
        PyErr_Format(PyExc_TypeError, "object of type '%.200s' has no len()", Py_TYPE(o)->tp_name);
    return -1;
}

#undef PyMapping_Length
Py_ssize_t PyMapping_Length(PyObject *o)
{
    return PyMapping_Size(o);
}
#define PyMapping_Length PyMapping_Size

PyObject *PyMapping_GetItemString(PyObject *o, const char *key)
{
    PyObject *okey, *r;

    if (key == NULL)
        return qp_capi_num_null_error();
    okey = PyUnicode_FromString(key);
    if (okey == NULL)
        return NULL;
    r = PyObject_GetItem(o, okey);
    Py_DECREF(okey);
    return r;
}

int PyMapping_GetOptionalItem(PyObject *obj, PyObject *key, PyObject **result)
{
    if (PyAnyDict_CheckExact(obj))
        return PyDict_GetItemRef(obj, key, result);
    *result = PyObject_GetItem(obj, key);
    if (*result)
        return 1;
    if (!PyErr_ExceptionMatches(PyExc_KeyError))
        return -1;
    PyErr_Clear();
    return 0;
}

int PyMapping_GetOptionalItemString(PyObject *obj, const char *key, PyObject **result)
{
    PyObject *okey;
    int rc;

    if (key == NULL) {
        *result = NULL;
        qp_capi_num_null_error();
        return -1;
    }
    okey = PyUnicode_FromString(key);
    if (okey == NULL) {
        *result = NULL;
        return -1;
    }
    rc = PyMapping_GetOptionalItem(obj, okey, result);
    Py_DECREF(okey);
    return rc;
}

int PyMapping_SetItemString(PyObject *o, const char *key, PyObject *value)
{
    PyObject *okey;
    int r;

    if (key == NULL) {
        qp_capi_num_null_error();
        return -1;
    }
    okey = PyUnicode_FromString(key);
    if (okey == NULL)
        return -1;
    r = PyObject_SetItem(o, okey, value);
    Py_DECREF(okey);
    return r;
}

int PyMapping_HasKeyStringWithError(PyObject *obj, const char *key)
{
    PyObject *res;
    int rc = PyMapping_GetOptionalItemString(obj, key, &res);
    Py_XDECREF(res);
    return rc;
}

int PyMapping_HasKeyWithError(PyObject *obj, PyObject *key)
{
    PyObject *res;
    int rc = PyMapping_GetOptionalItem(obj, key, &res);
    Py_XDECREF(res);
    return rc;
}

int PyMapping_HasKeyString(PyObject *obj, const char *key)
{
    PyObject *value = NULL;
    int rc;

    if (obj == NULL) {
        qp_capi_num_null_error();
        rc = -1;
    } else {
        rc = PyMapping_GetOptionalItemString(obj, key, &value);
    }
    if (rc < 0) {
        PyErr_FormatUnraisable(
            "Exception ignored in PyMapping_HasKeyString(); consider using "
            "PyMapping_HasKeyStringWithError(), "
            "PyMapping_GetOptionalItemString() or PyMapping_GetItemString()");
        return 0;
    }
    Py_XDECREF(value);
    return rc;
}

int PyMapping_HasKey(PyObject *obj, PyObject *key)
{
    PyObject *value = NULL;
    int rc;

    if (obj == NULL || key == NULL) {
        qp_capi_num_null_error();
        rc = -1;
    } else {
        rc = PyMapping_GetOptionalItem(obj, key, &value);
    }
    if (rc < 0) {
        PyErr_FormatUnraisable(
            "Exception ignored in PyMapping_HasKey(); consider using "
            "PyMapping_HasKeyWithError(), "
            "PyMapping_GetOptionalItem() or PyObject_GetItem()");
        return 0;
    }
    Py_XDECREF(value);
    return rc;
}

/* method_output_as_list(): list(o.<meth>()) with CPython's message for a
   non-iterable result. */
static PyObject *qp_capi_method_output_as_list(PyObject *o, JSAtom meth, const char *meth_name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, out, it, list;

    v = qp_capi_value(ctx, o);
    if (JS_IsException(v))
        return NULL;
    out = qp_call_method(ctx, v, meth, NULL, 0, QP_NULL);
    if (JS_IsException(out))
        return NULL;
    if (qp_list_check_exact(ctx, out))
        return qp_capi_steal(ctx, out);
    it = qp_iter(ctx, out);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            qp_raise_type_error(ctx, "%T.%s() must return an iterable, not %T", v, meth_name, out);
        }
        JS_FreeValue(ctx, out);
        return NULL;
    }
    JS_FreeValue(ctx, out);
    list = qp_list_from_iterable(ctx, it);
    JS_FreeValue(ctx, it);
    return qp_capi_from_js_result(ctx, list);
}

PyObject *PyMapping_Keys(PyObject *o)
{
    if (o == NULL)
        return qp_capi_num_null_error();
    if (PyAnyDict_CheckExact(o))
        return PyDict_Keys(o);
    return qp_capi_method_output_as_list(o, QP_ATOM(qp_capi.ctx, keys), "keys");
}

PyObject *PyMapping_Items(PyObject *o)
{
    if (o == NULL)
        return qp_capi_num_null_error();
    if (PyAnyDict_CheckExact(o))
        return PyDict_Items(o);
    return qp_capi_method_output_as_list(o, QP_ATOM(qp_capi.ctx, items), "items");
}

PyObject *PyMapping_Values(PyObject *o)
{
    if (o == NULL)
        return qp_capi_num_null_error();
    if (PyAnyDict_CheckExact(o))
        return PyDict_Values(o);
    return qp_capi_method_output_as_list(o, QP_ATOM(qp_capi.ctx, values), "values");
}

/* ------------------------------------------------------------------------ */
/* Iterator protocol                                                         */
/* ------------------------------------------------------------------------ */

int PyIter_Check(PyObject *obj)
{
    QPType *t = qp_capi_abs_qptype(qp_capi.ctx, obj);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    return t->slots.tp_iternext != NULL;
}

int PyAIter_Check(PyObject *obj)
{
    QPType *t = qp_capi_abs_qptype(qp_capi.ctx, obj);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    return t->slots.am.am_anext != NULL;
}

/* An extension instance whose C type implements tp_iternext: called
   directly (no JS identity needed), as CPython's iternext() does. */
static int qp_capi_iter_native(PyObject *iter)
{
    return QP_CAPI_HAS_HEAD(iter) && (QP_CAPI_HEAD(iter)->flags & QP_CAPI_F_NATIVE) &&
           Py_TYPE(iter)->tp_iternext != NULL;
}

/* iternext(): 1 item, 0 exhausted (StopIteration cleared), -1 error. */
static int qp_capi_abs_iternext(PyObject *iter, PyObject **item)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, r;

    if (qp_capi_iter_native(iter)) {
        if ((*item = Py_TYPE(iter)->tp_iternext(iter)))
            return 1;
        if (!PyErr_Occurred())
            return 0;
        if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    *item = NULL;
    v = qp_capi_value(ctx, iter);
    if (JS_IsException(v))
        return -1;
    r = qp_next(ctx, v);
    if (JS_IsException(r))
        return -1;
    if (QP_IS_NULL(r))
        return 0;
    *item = qp_capi_steal(ctx, r);
    return *item ? 1 : -1;
}

int PyIter_NextItem(PyObject *iter, PyObject **item)
{
    QPType *t = qp_capi_abs_qptype(qp_capi.ctx, iter);

    if (!t) {
        *item = NULL;
        return -1;
    }
    if (t->slots.tp_iternext == NULL) {
        *item = NULL;
        PyErr_Format(PyExc_TypeError, "expected an iterator, got '%T'", iter);
        return -1;
    }
    return qp_capi_abs_iternext(iter, item);
}

PyObject *PyIter_Next(PyObject *iter)
{
    PyObject *item;
    (void)qp_capi_abs_iternext(iter, &item);
    return item;
}

/* _PyGen_FetchStopIterationValue() on a pending exception: 0 with the
   StopIteration value (None if none), -1 if another error is pending. */
static int qp_capi_fetch_stop_iteration_value(JSContext *ctx, PyObject **pvalue)
{
    JSValue exc, val;

    *pvalue = NULL;
    if (JS_HasException(ctx)) {
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration)))
            return -1;
        exc = qp_exc_fetch(ctx);
        val = qp_exc_stopiteration_value(ctx, exc);
        JS_FreeValue(ctx, exc);
        if (JS_IsException(val))
            return -1;
        *pvalue = qp_capi_steal(ctx, val);
        return *pvalue ? 0 : -1;
    }
    *pvalue = Py_None;
    return 0;
}

PySendResult PyIter_Send(PyObject *iter, PyObject *arg, PyObject **result)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue vi, va, r;
    QPType *t;
    int res;

    *result = NULL;
    if (qp_capi_abs_typed_value(ctx, iter, &vi, &t) < 0)
        return PYGEN_ERROR;
    va = qp_capi_value(ctx, arg);
    if (JS_IsException(va))
        return PYGEN_ERROR;
    if (t->slots.am.am_send) {
        r = JS_UNDEFINED;
        res = t->slots.am.am_send(ctx, vi, va, &r);
        if (res == QP_SEND_ERROR)
            return PYGEN_ERROR;
        *result = qp_capi_steal(ctx, r);
        if (!*result)
            return PYGEN_ERROR;
        return res == QP_SEND_NEXT ? PYGEN_NEXT : PYGEN_RETURN;
    }
    if (arg == Py_None && t->slots.tp_iternext)
        r = t->slots.tp_iternext(ctx, vi);
    else
        r = qp_call_method(ctx, vi, QP_ATOM(ctx, send), (JSValueConst *)&va, 1, QP_NULL);
    if (!JS_IsException(r) && !QP_IS_NULL(r)) {
        *result = qp_capi_steal(ctx, r);
        return *result ? PYGEN_NEXT : PYGEN_ERROR;
    }
    if (qp_capi_fetch_stop_iteration_value(ctx, result) == 0)
        return PYGEN_RETURN;
    return PYGEN_ERROR;
}

PyObject *PySeqIter_New(PyObject *seq)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (!seq)
        return qp_capi_num_null_error();
    v = qp_capi_value(ctx, seq);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_seqiter_new(ctx, v));
}

PyObject *PyCallIter_New(PyObject *callable, PyObject *sentinel)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c, s;

    if (!callable || !sentinel)
        return qp_capi_num_null_error();
    c = qp_capi_value(ctx, callable);
    if (JS_IsException(c))
        return NULL;
    s = qp_capi_value(ctx, sentinel);
    if (JS_IsException(s))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_calliter_new(ctx, c, s));
}

#undef QP_CAPI_NB_BINARY
#undef QP_CAPI_NB_TERNARY
#undef QP_CAPI_NB_SLOT
#undef QP_CAPI_ITERSEARCH_COUNT
#undef QP_CAPI_ITERSEARCH_INDEX
#undef QP_CAPI_ITERSEARCH_CONTAINS
