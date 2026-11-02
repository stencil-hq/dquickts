/*
 * dquickts Python: the float type (object.h section (c)).
 *
 * Derived from CPython 3.15.2 Objects/floatobject.c and the float parts of
 * Objects/abstract.c (PyNumber_Float) (PSF-2.0).
 *
 * A float is a JS_TAG_FLOAT64 value created with qp_float_new (never
 * JS_NewFloat64, which would fold integral values into ints).  Instances of
 * float subclasses are JS_CLASS_PY_BOXED objects whose u.object_data holds
 * the JS_TAG_FLOAT64 value (int/str subclasses box their own tags, so the
 * tag of object_data identifies a float subclass instance).
 *
 * Unboxed floats have no address: NaN identity is the 64-bit pattern
 * (qp_float_result gives every new NaN a unique payload); boxed instances
 * hash nan by object address.
 */

#include <float.h>

static JSValue qp_complex_nb_power(JSContext *ctx, JSValueConst v, JSValueConst w, JSValueConst z);

/* New float.  CPython creates a distinct object for every NaN result and
   `is`/containment compare NaNs by identity; qp_is compares unboxed floats
   by their 64-bit pattern, so every NaN produced here gets a fresh payload
   (sign kept), and hash(nan) is derived from that pattern. */
static JSValue qp_float_result(JSContext *ctx, double d)
{
    if (unlikely(isnan(d))) {
        static uint64_t qp_nan_counter;
        union { double d; uint64_t u; } x;
        x.d = d;
        qp_nan_counter++;
        x.u = (x.u & 0x8000000000000000ULL) | 0x7ff8000000000000ULL |
              (qp_nan_counter & 0x0007ffffffffffffULL);
        d = x.d;
    }
    return qp_float_new(ctx, d);
}

/* Float value of a float or float-subclass instance; FALSE otherwise. */
static inline BOOL qp_float_get(JSValueConst v, double *d)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64) {
        *d = JS_VALUE_GET_FLOAT64(v);
        return TRUE;
    }
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (p->class_id == JS_CLASS_PY_BOXED &&
            JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_FLOAT64) {
            *d = JS_VALUE_GET_FLOAT64(p->u.object_data);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL qp_float_check(JSContext *ctx, JSValueConst v)
{
    double d;
    return qp_float_get(v, &d);
}

/* tp_name of type(v) as a C string into buf (heap types: __name__). */
static const char *qp_float_type_name(JSContext *ctx, QPType *t, char *buf, int size)
{
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* New float of type t (float itself or a subclass). */
static JSValue qp_float_new_type(JSContext *ctx, QPType *t, double d)
{
    JSValue obj;
    if (t == QP_TYPE(ctx, float))
        return qp_float_result(ctx, d);
    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_BOXED);
    if (JS_IsException(obj))
        return obj;
    JS_VALUE_GET_OBJ(obj)->u.object_data = qp_float_result(ctx, d);
    return obj;
}

/* OverflowError/ValueError from errno (PyErr_SetFromErrno). */
static JSValue qp_float_raise_errno(JSContext *ctx, int err)
{
    JSValue args[2], tup;
    QPType *t = err == ERANGE ? QP_EXC(ctx, OverflowError) : QP_EXC(ctx, ValueError);
    args[0] = JS_NewInt32(ctx, err);
    args[1] = qp_str_from_cstr(ctx, strerror(err));
    if (JS_IsException(args[1]))
        return JS_EXCEPTION;
    tup = qp_tuple_new_steal(ctx, 2, args);
    if (JS_IsException(tup))
        return tup;
    qp_raise_value(ctx, t, tup);
    JS_FreeValue(ctx, tup);
    return JS_EXCEPTION;
}

/* Unboxed floats: NaN identity = bit pattern (see qp_float_result). */
static Py_hash_t qp_float_hash(double d)
{
    union { double d; uint64_t u; } x;
    x.d = d;
    return qp_hash_double(d, (uintptr_t)x.u);
}

static JSValue qp_float_to_repr(JSContext *ctx, double d)
{
    char buf[QP_FLOAT_REPR_MAX];
    int n = qp_float_repr(d, buf);
    if (n < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewStringLen(ctx, buf, n);
}

/* CONVERT_TO_DOUBLE: 1 = *out set, 0 = not a float/int (NotImplemented),
   -1 = error (int too large to convert to float). */
static int qp_float_convert(JSContext *ctx, JSValueConst v, double *out)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_FLOAT64:
        *out = JS_VALUE_GET_FLOAT64(v);
        return 1;
    case JS_TAG_INT:
        *out = JS_VALUE_GET_INT(v);
        return 1;
    case JS_TAG_BOOL:
        *out = JS_VALUE_GET_BOOL(v) ? 1.0 : 0.0;
        return 1;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return qp_long_as_double(ctx, v, out) < 0 ? -1 : 1;
    case JS_TAG_OBJECT:
        if (qp_float_get(v, out))
            return 1;
        if (qp_long_check(ctx, v)) {
            JSValue iv = qp_long_value(ctx, v);
            int r;
            if (JS_IsException(iv))
                return -1;
            r = qp_long_as_double(ctx, iv, out);
            JS_FreeValue(ctx, iv);
            return r < 0 ? -1 : 1;
        }
        return 0;
    default:
        return 0;
    }
}

#define QP_FLOAT_CONVERT(v, d)                                          \
    do {                                                                \
        int r_ = qp_float_convert(ctx, (v), &(d));                      \
        if (r_ <= 0)                                                    \
            return r_ < 0 ? JS_EXCEPTION : qp_not_implemented(ctx);     \
    } while (0)

/* PyFloat_AsDouble */
static int qp_float_as_double(JSContext *ctx, JSValueConst v, double *out)
{
    QPType *t;
    JSValue res;
    double d;

    if (qp_float_get(v, out))
        return 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    t = qp_type_of(ctx, v);
    if (!t->slots.nb.nb_float) {
        if (t->slots.nb.nb_index) {
            JSValue iv = qp_index(ctx, v);
            int r;
            if (JS_IsException(iv))
                return -1;
            r = qp_long_as_double(ctx, iv, out);
            JS_FreeValue(ctx, iv);
            return r;
        }
        {
            char buf[ATOM_GET_STR_BUF_SIZE];
            qp_raise_type_error(ctx, "must be real number, not %.50s",
                                qp_float_type_name(ctx, t, buf, sizeof(buf)));
        }
        return -1;
    }
    res = t->slots.nb.nb_float(ctx, v);
    if (JS_IsException(res))
        return -1;
    if (JS_VALUE_GET_TAG(res) != JS_TAG_FLOAT64) {
        if (!qp_float_get(res, &d)) {
            qp_raise_type_error(ctx, "%T.__float__() must return a float, not %T", v, res);
            JS_FreeValue(ctx, res);
            return -1;
        }
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "%T.__float__() must return a float, not %T.  "
                    "The ability to return an instance of a strict subclass of float "
                    "is deprecated, and may be removed in a future version of Python.",
                    v, res) < 0) {
            JS_FreeValue(ctx, res);
            return -1;
        }
    }
    qp_float_get(res, out);
    JS_FreeValue(ctx, res);
    return 0;
}

/* _PyUnicode_TransformDecimalAndSpaceToASCII into a NUL-terminated
   js_malloc'd buffer of *plen bytes. */
static char *qp_float_str_to_ascii(JSContext *ctx, JSValueConst s, size_t *plen)
{
    qp_strview view;
    Py_ssize_t i;
    char *buf;

    if (qp_str_view_acquire(ctx, s, &view) < 0)
        return NULL;
    buf = js_malloc(ctx, view.len + 1);
    if (buf) {
        for (i = 0; i < view.len; i++) {
            Py_UCS4 ch = QP_SL_READ(view.kind, view.data, i);
            if (ch < 128) {
                buf[i] = (char)ch;
            } else if (_PyUnicode_IsWhitespace(ch)) {
                buf[i] = ' ';
            } else {
                int dec = _PyUnicode_ToDecimalDigit(ch);
                buf[i] = dec < 0 ? '?' : (char)('0' + dec);
            }
        }
        buf[view.len] = '\0';
        *plen = view.len;
    }
    qp_str_view_release(ctx, &view);
    return buf;
}

/* PyFloat_FromString: str, bytes, bytearray or buffer exporter. */
static JSValue qp_float_from_str(JSContext *ctx, JSValueConst v)
{
    char *s = NULL;
    size_t len = 0;
    double x;
    int r;

    if (qp_str_check(ctx, v)) {
        JSValue sv = qp_str_value(ctx, v);
        if (JS_IsException(sv))
            return sv;
        s = qp_float_str_to_ascii(ctx, sv, &len);
        JS_FreeValue(ctx, sv);
        if (!s)
            return JS_EXCEPTION;
    } else if (qp_bytes_check(ctx, v) || qp_bytearray_check(ctx, v)) {
        Py_ssize_t n;
        const uint8_t *data = qp_bytes_data(v, &n);
        s = js_malloc(ctx, n + 1);
        if (!s)
            return JS_EXCEPTION;
        memcpy(s, data, n);
        s[n] = '\0';
        len = n;
    } else if (qp_type_of(ctx, v)->slots.bf.bf_getbuffer) {
        QPBuffer view;
        if (qp_buffer_get(ctx, v, &view, QP_BUF_SIMPLE) < 0)
            return JS_EXCEPTION;
        s = js_malloc(ctx, view.len + 1);
        if (s) {
            memcpy(s, view.buf, view.len);
            s[view.len] = '\0';
            len = view.len;
        }
        qp_buffer_release(ctx, &view);
        if (!s)
            return JS_EXCEPTION;
    } else {
        char buf[ATOM_GET_STR_BUF_SIZE];
        return qp_raise_type_error(ctx,
            "float() argument must be a string or a real number, not '%.200s'",
            qp_float_type_name(ctx, qp_type_of(ctx, v), buf, sizeof(buf)));
    }
    r = qp_float_from_string(s, len, &x);
    js_free(ctx, s);
    if (r < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (r > 0)
        return qp_raise_value_error(ctx, "could not convert string to float: %R", v);
    return qp_float_result(ctx, x);
}

/* PyNumber_Float */
static JSValue qp_number_float(JSContext *ctx, JSValueConst v)
{
    QPType *t;
    double d;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)
        return JS_DupValue(ctx, v);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return qp_float_result(ctx, JS_VALUE_GET_INT(v));
    t = qp_type_of(ctx, v);
    if (t->slots.nb.nb_float) {
        JSValue res = t->slots.nb.nb_float(ctx, v);
        if (JS_IsException(res) || JS_VALUE_GET_TAG(res) == JS_TAG_FLOAT64)
            return res;
        if (!qp_float_get(res, &d)) {
            qp_raise_type_error(ctx, "%T.__float__() must return a float, not %T", v, res);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "%T.__float__() must return a float, not %T.  "
                    "The ability to return an instance of a strict subclass of float "
                    "is deprecated, and may be removed in a future version of Python.",
                    v, res) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, res);
        return qp_float_result(ctx, d);
    }
    if (t->slots.nb.nb_index) {
        JSValue iv = qp_index(ctx, v);
        int r;
        if (JS_IsException(iv))
            return iv;
        r = qp_long_as_double(ctx, iv, &d);
        JS_FreeValue(ctx, iv);
        if (r < 0)
            return JS_EXCEPTION;
        return qp_float_result(ctx, d);
    }
    if (qp_float_get(v, &d))
        return qp_float_result(ctx, d);
    return qp_float_from_str(ctx, v);
}

/* ---- slots -------------------------------------------------------------- */

static JSValue qp_float_repr_slot(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    return qp_float_to_repr(ctx, d);
}

static Py_hash_t qp_float_hash_slot(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT)
        return qp_hash_double(d, (uintptr_t)JS_VALUE_GET_PTR(v));
    return qp_float_hash(d);
}

/* float_richcompare: exact comparison with ints of any size. */
static JSValue qp_float_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    double i, j;
    int c;

    if (!qp_float_get(v, &i))
        return qp_not_implemented(ctx);
    if (qp_float_get(w, &j)) {
        /* plain double comparison below */
    } else if (!isfinite(i) && qp_long_check(ctx, w)) {
        /* nan compares false with everything, inf by its sign */
        j = 0.0;
    } else if (JS_VALUE_GET_TAG(w) == JS_TAG_INT) {
        j = JS_VALUE_GET_INT(w);
    } else if (JS_VALUE_GET_TAG(w) == JS_TAG_BOOL) {
        j = JS_VALUE_GET_BOOL(w);
    } else if (qp_long_check(ctx, w)) {
        JSValue iw = qp_long_value(ctx, w), fl;
        int64_t i64;
        int overflow = 0;
        double f;
        if (JS_IsException(iw))
            return iw;
        if (qp_long_as_int64_overflow(iw, &i64, &overflow) == 0 && !overflow &&
            i64 <= ((int64_t)1 << 53) && i64 >= -((int64_t)1 << 53)) {
            JS_FreeValue(ctx, iw);
            j = (double)i64;
            goto compare;
        }
        /* exact: compare floor(i) as an int with w */
        f = floor(i);
        fl = qp_long_from_double(ctx, f);
        if (JS_IsException(fl)) {
            JS_FreeValue(ctx, iw);
            return fl;
        }
        c = qp_long_compare(fl, iw);
        JS_FreeValue(ctx, fl);
        JS_FreeValue(ctx, iw);
        if (c == 0 && f != i)
            c = 1;
        goto result;
    } else {
        return qp_not_implemented(ctx);
    }
 compare:
    if (isnan(i) || isnan(j))
        return qp_bool_new(op == QP_CMP_NE);
    c = i < j ? -1 : i > j ? 1 : 0;
 result:
    switch (op) {
    case QP_CMP_LT: return qp_bool_new(c < 0);
    case QP_CMP_LE: return qp_bool_new(c <= 0);
    case QP_CMP_EQ: return qp_bool_new(c == 0);
    case QP_CMP_NE: return qp_bool_new(c != 0);
    case QP_CMP_GT: return qp_bool_new(c > 0);
    default:        return qp_bool_new(c >= 0);
    }
}

static JSValue qp_float_add(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double a, b;
    QP_FLOAT_CONVERT(v, a);
    QP_FLOAT_CONVERT(w, b);
    return qp_float_result(ctx, a + b);
}

static JSValue qp_float_sub(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double a, b;
    QP_FLOAT_CONVERT(v, a);
    QP_FLOAT_CONVERT(w, b);
    return qp_float_result(ctx, a - b);
}

static JSValue qp_float_mul(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double a, b;
    QP_FLOAT_CONVERT(v, a);
    QP_FLOAT_CONVERT(w, b);
    return qp_float_result(ctx, a * b);
}

static JSValue qp_float_div(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double a, b;
    QP_FLOAT_CONVERT(v, a);
    QP_FLOAT_CONVERT(w, b);
    if (b == 0.0)
        return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "division by zero");
    return qp_float_result(ctx, a / b);
}

static JSValue qp_float_rem(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double vx, wx, mod;
    QP_FLOAT_CONVERT(v, vx);
    QP_FLOAT_CONVERT(w, wx);
    if (wx == 0.0)
        return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "division by zero");
    mod = fmod(vx, wx);
    if (mod) {
        /* ensure the remainder has the same sign as the denominator */
        if ((wx < 0) != (mod < 0))
            mod += wx;
    } else {
        /* signed zero: same sign as the denominator */
        mod = copysign(0.0, wx);
    }
    return qp_float_result(ctx, mod);
}

static void qp_float_div_mod(double vx, double wx, double *floordiv, double *mod)
{
    double div;
    *mod = fmod(vx, wx);
    /* vx - mod is mathematically an exact multiple of wx; div may be
       slightly off an integral value, snapped below */
    div = (vx - *mod) / wx;
    if (*mod) {
        if ((wx < 0) != (*mod < 0)) {
            *mod += wx;
            div -= 1.0;
        }
    } else {
        *mod = copysign(0.0, wx);
    }
    if (div) {
        *floordiv = floor(div);
        if (div - *floordiv > 0.5)
            *floordiv += 1.0;
    } else {
        /* zero with the sign of the true quotient */
        *floordiv = copysign(0.0, vx / wx);
    }
}

static JSValue qp_float_divmod(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double vx, wx, mod, floordiv;
    JSValue items[2];
    QP_FLOAT_CONVERT(v, vx);
    QP_FLOAT_CONVERT(w, wx);
    if (wx == 0.0)
        return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "division by zero");
    qp_float_div_mod(vx, wx, &floordiv, &mod);
    items[0] = qp_float_result(ctx, floordiv);
    items[1] = qp_float_result(ctx, mod);
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_float_floor_div(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    double vx, wx, mod, floordiv;
    QP_FLOAT_CONVERT(v, vx);
    QP_FLOAT_CONVERT(w, wx);
    if (wx == 0.0)
        return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "division by zero");
    qp_float_div_mod(vx, wx, &floordiv, &mod);
    return qp_float_result(ctx, floordiv);
}

/* x is not an infinity or nan */
#define QP_DOUBLE_IS_ODD_INTEGER(x) (fmod(fabs(x), 2.0) == 1.0)

static JSValue qp_float_pow(JSContext *ctx, JSValueConst v, JSValueConst w, JSValueConst z)
{
    double iv, iw, ix;
    int negate_result = 0;

    if (!JS_IsUndefined(z) && !JS_IsNull(z))
        return qp_raise_type_error(ctx, "pow() 3rd argument not "
                                   "allowed unless all arguments are integers");
    QP_FLOAT_CONVERT(v, iv);
    QP_FLOAT_CONVERT(w, iw);

    if (iw == 0)                /* v**0 is 1, even 0**0 */
        return qp_float_result(ctx, 1.0);
    if (isnan(iv))              /* nan**w = nan, unless w == 0 */
        return qp_float_result(ctx, iv);
    if (isnan(iw))              /* v**nan = nan, unless v == 1; 1**nan = 1 */
        return qp_float_result(ctx, iv == 1.0 ? 1.0 : iw);
    if (isinf(iw)) {
        /* v**inf: 0.0 if |v| < 1; 1.0 if |v| == 1; inf if |v| > 1;
           v**-inf: inf if |v| < 1; 1.0 if |v| == 1; 0.0 if |v| > 1 */
        iv = fabs(iv);
        if (iv == 1.0)
            return qp_float_result(ctx, 1.0);
        else if ((iw > 0.0) == (iv > 1.0))
            return qp_float_result(ctx, fabs(iw));
        else
            return qp_float_result(ctx, 0.0);
    }
    if (isinf(iv)) {
        /* (+-inf)**w: inf for w positive, 0 for w negative, with the sign
           of iv if w is an odd integer */
        int iw_is_odd = QP_DOUBLE_IS_ODD_INTEGER(iw);
        if (iw > 0.0)
            return qp_float_result(ctx, iw_is_odd ? iv : fabs(iv));
        else
            return qp_float_result(ctx, iw_is_odd ? copysign(0.0, iv) : 0.0);
    }
    if (iv == 0.0) {
        int iw_is_odd = QP_DOUBLE_IS_ODD_INTEGER(iw);
        if (iw < 0.0)
            return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "zero to a negative power");
        return qp_float_result(ctx, iw_is_odd ? iv : 0.0);
    }
    if (iv < 0.0) {
        if (iw != floor(iw)) {
            /* negative numbers raised to fractional powers become complex */
            return qp_complex_nb_power(ctx, v, w, z);
        }
        iv = -iv;
        negate_result = QP_DOUBLE_IS_ODD_INTEGER(iw);
    }
    if (iv == 1.0)   /* 1**w is 1, even 1**inf and 1**nan; (-1)**big_int */
        return qp_float_result(ctx, negate_result ? -1.0 : 1.0);

    errno = 0;
    ix = pow(iv, iw);
    /* _Py_ADJUST_ERANGE1 */
    if (errno == 0) {
        if (ix == INFINITY || ix == -INFINITY)
            errno = ERANGE;
    } else if (errno == ERANGE && ix == 0.0) {
        errno = 0;
    }
    if (negate_result)
        ix = -ix;
    if (errno != 0)
        return qp_float_raise_errno(ctx, errno);
    return qp_float_result(ctx, ix);
}

#undef QP_DOUBLE_IS_ODD_INTEGER

static JSValue qp_float_neg(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    return qp_float_result(ctx, -d);
}

static JSValue qp_float_abs(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    return qp_float_result(ctx, fabs(d));
}

static int qp_float_bool(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    return d != 0.0;
}

/* float_float: exact float of the same value */
static JSValue qp_float_float(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)
        return JS_DupValue(ctx, v);
    qp_float_get(v, &d);
    return qp_float_result(ctx, d);
}

static JSValue qp_float_trunc_slot(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    qp_float_get(v, &d);
    return qp_long_from_double(ctx, d);
}

/* ---- methods ------------------------------------------------------------ */

static double qp_float_self(JSValueConst self)
{
    double d = 0;
    qp_float_get(self, &d);
    return d;
}

static JSValue qp_float_m_is_integer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    double x = qp_float_self(self);
    if (!isfinite(x))
        return JS_FALSE;
    return qp_bool_new(floor(x) == x);
}

static JSValue qp_float_m_trunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_double(ctx, qp_float_self(self));
}

static JSValue qp_float_m_floor(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_double(ctx, floor(qp_float_self(self)));
}

static JSValue qp_float_m_ceil(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_double(ctx, ceil(qp_float_self(self)));
}

/* float.__round__(ndigits=None) */
static JSValue qp_float_m_round(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    double x = qp_float_self(self), rounded;
    Py_ssize_t ndigits;
    qp_error err;

    if (qp_arg_check_positional(ctx, "__round__", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0 || JS_IsUndefined(args[0]) || JS_IsNull(args[0])) {
        /* round to nearest integer, halfway cases to even */
        return qp_long_from_double(ctx, qp_double_round_half_even(x));
    }
    /* clips on overflow */
    if (qp_as_ssize(ctx, args[0], NULL, &ndigits) < 0)
        return JS_EXCEPTION;
    if (!isfinite(x))
        return qp_float_result(ctx, x);
    /* NDIGITS_MAX = 323, NDIGITS_MIN = -308 (DBL_* * 0.30103) */
    if (ndigits > (int)((DBL_MANT_DIG - DBL_MIN_EXP) * 0.30103))
        return qp_float_result(ctx, x);
    if (ndigits < -(int)((DBL_MAX_EXP + 1) * 0.30103))
        return qp_float_result(ctx, 0.0 * x);
    memset(&err, 0, sizeof(err));
    if (qp_double_round(x, ndigits, &rounded, &err) < 0) {
        qp_raise_qp_error(ctx, &err, NULL);
        qp_error_clear(&err);
        return JS_EXCEPTION;
    }
    return qp_float_result(ctx, rounded);
}

static JSValue qp_float_m_conjugate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_float_float(ctx, self);
}

/* TOHEX_NBITS is DBL_MANT_DIG rounded up to the next integer of the form 4k+1 */
#define QP_TOHEX_NBITS (DBL_MANT_DIG + 3 - (DBL_MANT_DIG + 2) % 4)

static JSValue qp_float_m_hex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    static const char hexdigits[] = "0123456789abcdef";
    double x = qp_float_self(self), m;
    int e, shift, i, si, esign;
    char s[(QP_TOHEX_NBITS - 1) / 4 + 3];
    char out[64];

    if (isnan(x) || isinf(x))
        return qp_float_to_repr(ctx, x);
    if (x == 0.0)
        return qp_str_from_cstr(ctx, copysign(1.0, x) == -1.0 ? "-0x0.0p+0" : "0x0.0p+0");

    m = frexp(fabs(x), &e);
    shift = 1 - max_int(DBL_MIN_EXP - e, 0);
    m = ldexp(m, shift);
    e -= shift;

    si = 0;
    s[si++] = hexdigits[(int)m];
    m -= (int)m;
    s[si++] = '.';
    for (i = 0; i < (QP_TOHEX_NBITS - 1) / 4; i++) {
        m *= 16.0;
        s[si++] = hexdigits[(int)m];
        m -= (int)m;
    }
    s[si] = '\0';

    if (e < 0) {
        esign = '-';
        e = -e;
    } else {
        esign = '+';
    }
    snprintf(out, sizeof(out), "%s0x%sp%c%d", x < 0.0 ? "-" : "", s, esign, e);
    return qp_str_from_cstr(ctx, out);
}

#define QP_LMIN(a, b) ((long)(a) < (long)(b) ? (long)(a) : (long)(b))

static int qp_hex_from_char(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline int qp_float_isspace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* float.fromhex(string) (classmethod) */
static JSValue qp_float_m_fromhex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPType *type = qp_type_from_value(self);
    JSValue sv, result;
    double x;
    long exp, top_exp, lsb, key_digit;
    const char *s, *coeff_start, *s_store, *coeff_end, *exp_start, *s_end, *s0;
    int half_eps, digit, round_up, negate = 0;
    Py_ssize_t ndigits, fdigits, i;
    size_t length;
    JSValueConst string = args[0];

    if (!qp_str_check(ctx, string))
        return qp_raise_type_error(ctx, "bad argument type for built-in operation");
    sv = qp_str_value(ctx, string);
    if (JS_IsException(sv))
        return sv;
    s0 = s = qp_str_utf8(ctx, sv, &length);
    JS_FreeValue(ctx, sv);
    if (!s)
        return JS_EXCEPTION;
    s_end = s + length;

    /* leading whitespace */
    while (qp_float_isspace(*s))
        s++;

    /* infinities and nans */
    x = _Py_parse_inf_or_nan(s, (char **)&coeff_end);
    if (coeff_end != s) {
        s = coeff_end;
        goto finished;
    }

    /* optional sign */
    if (*s == '-') {
        s++;
        negate = 1;
    } else if (*s == '+') {
        s++;
    }

    /* [0x] */
    s_store = s;
    if (*s == '0') {
        s++;
        if (*s == 'x' || *s == 'X')
            s++;
        else
            s = s_store;
    }

    /* coefficient: <integer> [. <fraction>] */
    coeff_start = s;
    while (qp_hex_from_char(*s) >= 0)
        s++;
    s_store = s;
    if (*s == '.') {
        s++;
        while (qp_hex_from_char(*s) >= 0)
            s++;
        coeff_end = s - 1;
    } else {
        coeff_end = s;
    }

    /* ndigits = total # of hex digits; fdigits = # after point */
    ndigits = coeff_end - coeff_start;
    fdigits = coeff_end - s_store;
    if (ndigits == 0)
        goto parse_error;
    if (ndigits > QP_LMIN(DBL_MIN_EXP - DBL_MANT_DIG - LONG_MIN / 2,
                          LONG_MAX / 2 + 1 - DBL_MAX_EXP) / 4)
        goto insane_length_error;

    /* [p <exponent>] */
    if (*s == 'p' || *s == 'P') {
        s++;
        exp_start = s;
        if (*s == '-' || *s == '+')
            s++;
        if (!('0' <= *s && *s <= '9'))
            goto parse_error;
        s++;
        while ('0' <= *s && *s <= '9')
            s++;
        exp = strtol(exp_start, NULL, 10);
    } else {
        exp = 0;
    }

/* for 0 <= j < ndigits, HEX_DIGIT(j) gives the jth most significant digit */
#define HEX_DIGIT(j) qp_hex_from_char(*((j) < fdigits ? coeff_end - (j) : coeff_end - 1 - (j)))

    /* Discard leading zeros, and catch extreme overflow and underflow */
    while (ndigits > 0 && HEX_DIGIT(ndigits - 1) == 0)
        ndigits--;
    if (ndigits == 0 || exp < LONG_MIN / 2) {
        x = 0.0;
        goto finished;
    }
    if (exp > LONG_MAX / 2)
        goto overflow_error;

    /* Adjust exponent for fractional part. */
    exp = exp - 4 * ((long)fdigits);

    /* top_exp = 1 more than exponent of most sig. bit of coefficient */
    top_exp = exp + 4 * ((long)ndigits - 1);
    for (digit = HEX_DIGIT(ndigits - 1); digit != 0; digit /= 2)
        top_exp++;

    /* catch almost all nonextreme cases of overflow and underflow here */
    if (top_exp < DBL_MIN_EXP - DBL_MANT_DIG) {
        x = 0.0;
        goto finished;
    }
    if (top_exp > DBL_MAX_EXP)
        goto overflow_error;

    /* lsb = exponent of least significant bit of the *rounded* value */
    lsb = (top_exp > (long)DBL_MIN_EXP ? top_exp : (long)DBL_MIN_EXP) - DBL_MANT_DIG;

    x = 0.0;
    if (exp >= lsb) {
        /* no rounding required */
        for (i = ndigits - 1; i >= 0; i--)
            x = 16.0 * x + HEX_DIGIT(i);
        x = ldexp(x, (int)(exp));
        goto finished;
    }
    /* rounding required.  key_digit is the index of the hex digit
       containing the first bit to be rounded away. */
    half_eps = 1 << (int)((lsb - exp - 1) % 4);
    key_digit = (lsb - exp - 1) / 4;
    for (i = ndigits - 1; i > key_digit; i--)
        x = 16.0 * x + HEX_DIGIT(i);
    digit = HEX_DIGIT(key_digit);
    x = 16.0 * x + (double)(digit & (16 - 2 * half_eps));

    /* round-half-even: round up if bit lsb-1 is 1 and at least one of
       bits lsb, lsb-2, lsb-3, lsb-4, ... is 1. */
    if ((digit & half_eps) != 0) {
        round_up = 0;
        if ((digit & (3 * half_eps - 1)) != 0 || (half_eps == 8 &&
                key_digit + 1 < ndigits && (HEX_DIGIT(key_digit + 1) & 1) != 0))
            round_up = 1;
        else
            for (i = key_digit - 1; i >= 0; i--)
                if (HEX_DIGIT(i) != 0) {
                    round_up = 1;
                    break;
                }
        if (round_up) {
            x += 2 * half_eps;
            if (top_exp == DBL_MAX_EXP &&
                x == ldexp((double)(2 * half_eps), DBL_MANT_DIG))
                /* overflow corner case: pre-rounded value <
                   2**DBL_MAX_EXP; rounded=2**DBL_MAX_EXP. */
                goto overflow_error;
        }
    }
    x = ldexp(x, (int)(exp + 4 * key_digit));
#undef HEX_DIGIT

 finished:
    /* optional trailing whitespace leading to the end of the string */
    while (qp_float_isspace(*s))
        s++;
    if (s != s_end)
        goto parse_error;
    JS_FreeCString(ctx, s0);
    result = qp_float_result(ctx, negate ? -x : x);
    if (type && type != QP_TYPE(ctx, float)) {
        JSValue r2 = qp_vectorcall(ctx, QP_TYPE_VALUE(type), (JSValueConst *)&result, 1, QP_NULL);
        JS_FreeValue(ctx, result);
        return r2;
    }
    return result;

 overflow_error:
    JS_FreeCString(ctx, s0);
    return qp_raise(ctx, QP_EXC(ctx, OverflowError),
                    "hexadecimal value too large to represent as a float");
 parse_error:
    JS_FreeCString(ctx, s0);
    return qp_raise_value_error(ctx, "invalid hexadecimal floating-point string");
 insane_length_error:
    JS_FreeCString(ctx, s0);
    return qp_raise_value_error(ctx, "hexadecimal string too long to convert");
}

static JSValue qp_float_m_as_integer_ratio(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                           int nargs, JSValueConst kwnames, int magic)
{
    double x = qp_float_self(self), float_part;
    int exponent, i;
    JSValue items[2];

    if (isinf(x))
        return qp_raise(ctx, QP_EXC(ctx, OverflowError), "cannot convert Infinity to integer ratio");
    if (isnan(x))
        return qp_raise_value_error(ctx, "cannot convert NaN to integer ratio");

    float_part = frexp(x, &exponent);   /* x == float_part * 2**exponent exactly */
    for (i = 0; i < 300 && float_part != floor(float_part); i++) {
        float_part *= 2.0;
        exponent--;
    }
    if (exponent > 0) {
        /* x itself is integral: numerator = x, denominator = 1 */
        items[0] = qp_long_from_double(ctx, x);
        if (JS_IsException(items[0]))
            return items[0];
        items[1] = JS_NewInt32(ctx, 1);
    } else {
        JSValue sh;
        items[0] = qp_long_from_double(ctx, float_part);
        if (JS_IsException(items[0]))
            return items[0];
        sh = JS_NewInt32(ctx, -exponent);
        items[1] = qp_binop(ctx, JS_NewInt32(ctx, 1), sh, QP_NB_LSHIFT);
        if (JS_IsException(items[1])) {
            JS_FreeValue(ctx, items[0]);
            return JS_EXCEPTION;
        }
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_float_m_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    JSValue item = qp_float_result(ctx, qp_float_self(self));
    return qp_tuple_new_steal(ctx, 1, &item);
}

/* float.__getformat__(typestr) (classmethod) */
static JSValue qp_float_m_getformat(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    const char *s;
    size_t len;
    int ok;
    JSValue sv;

    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "__getformat__() argument must be str, not %T", args[0]);
    sv = qp_str_value(ctx, args[0]);
    if (JS_IsException(sv))
        return sv;
    s = qp_str_utf8(ctx, sv, &len);
    JS_FreeValue(ctx, sv);
    if (!s)
        return JS_EXCEPTION;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        return qp_raise_value_error(ctx, "embedded null character");
    }
    ok = strcmp(s, "double") == 0 || strcmp(s, "float") == 0;
    JS_FreeCString(ctx, s);
    if (!ok)
        return qp_raise_value_error(ctx, "__getformat__() argument 1 must be "
                                    "'double' or 'float'");
#if defined(WORDS_BIGENDIAN) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return qp_str_from_cstr(ctx, "IEEE, big-endian");
#else
    return qp_str_from_cstr(ctx, "IEEE, little-endian");
#endif
}

/* Shared by float.__format__ and complex.__format__: str spec checks,
   empty spec -> str(self), else fmt(view) into a DynBuf. */
typedef int QPFormatNumFunc(DynBuf *out, double re, double im, const void *spec, int kind,
                            size_t len, const char *type_name, qp_error *err);

static JSValue qp_number_format(JSContext *ctx, JSValueConst self, JSValueConst spec,
                                double re, double im, QPFormatNumFunc *fn)
{
    JSValue sv, res;
    qp_strview view;
    DynBuf b;
    qp_error err;
    char nbuf[ATOM_GET_STR_BUF_SIZE];
    const char *tname;
    int r;

    if (!qp_str_check(ctx, spec))
        return qp_raise_type_error(ctx, "__format__() argument must be str, not %T", spec);
    sv = qp_str_value(ctx, spec);
    if (JS_IsException(sv))
        return sv;
    if (qp_str_view_acquire(ctx, sv, &view) < 0) {
        JS_FreeValue(ctx, sv);
        return JS_EXCEPTION;
    }
    if (view.len == 0) {
        qp_str_view_release(ctx, &view);
        JS_FreeValue(ctx, sv);
        return qp_str(ctx, self);
    }
    tname = qp_float_type_name(ctx, qp_type_of(ctx, self), nbuf, sizeof(nbuf));
    js_dbuf_init(ctx, &b);
    memset(&err, 0, sizeof(err));
    r = fn(&b, re, im, view.data, view.kind, view.len, tname, &err);
    qp_str_view_release(ctx, &view);
    JS_FreeValue(ctx, sv);
    if (r < 0) {
        dbuf_free(&b);
        if (err.kind == QP_ERR_MEMORY || err.kind == QP_ERR_NONE) {
            qp_error_clear(&err);
            return JS_ThrowOutOfMemory(ctx);
        }
        qp_raise_qp_error(ctx, &err, NULL);
        qp_error_clear(&err);
        return JS_EXCEPTION;
    }
    res = qp_str_from_utf8(ctx, (const char *)b.buf, b.size);
    dbuf_free(&b);
    return res;
}

static int qp_float_format_fn(DynBuf *out, double re, double im, const void *spec, int kind,
                              size_t len, const char *type_name, qp_error *err)
{
    return qp_format_float(out, re, spec, kind, len, type_name, err);
}

static JSValue qp_float_m_format(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_number_format(ctx, self, args[0], qp_float_self(self), 0.0, qp_float_format_fn);
}

/* float.from_number(number) (classmethod) */
static JSValue qp_float_m_from_number(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPType *type = qp_type_from_value(self);
    double x;
    JSValue result;

    if (JS_VALUE_GET_TAG(args[0]) == JS_TAG_FLOAT64 && type == QP_TYPE(ctx, float))
        return JS_DupValue(ctx, args[0]);
    if (qp_float_as_double(ctx, args[0], &x) < 0)
        return JS_EXCEPTION;
    result = qp_float_result(ctx, x);
    if (type && type != QP_TYPE(ctx, float)) {
        JSValue r2 = qp_vectorcall(ctx, QP_TYPE_VALUE(type), (JSValueConst *)&result, 1, QP_NULL);
        JS_FreeValue(ctx, result);
        return r2;
    }
    return result;
}

static JSValue qp_float_get_real(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_float_float(ctx, self);
}

static JSValue qp_float_get_imag(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_float_result(ctx, 0.0);
}

/* float(x=0, /) */
static JSValue qp_float_tp_new(JSContext *ctx, QPType *type, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *ft = QP_TYPE(ctx, float);
    JSValue tmp, res;
    double d;

    if ((type == ft || type->slots.tp_init == ft->slots.tp_init) &&
        qp_arg_no_keywords(ctx, "float", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "float", (int)nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0)
        tmp = qp_float_result(ctx, 0.0);
    else if (qp_str_check(ctx, args[0]))
        tmp = qp_float_from_str(ctx, args[0]);
    else
        tmp = qp_number_float(ctx, args[0]);
    if (type == ft || JS_IsException(tmp))
        return tmp;
    /* float_subtype_new */
    d = 0;
    qp_float_get(tmp, &d);
    JS_FreeValue(ctx, tmp);
    res = qp_float_new_type(ctx, type, d);
    return res;
}

static const QPMethodDef qp_float_methods[] = {
    { "from_number", qp_float_m_from_number, QP_METH_O | QP_METH_CLASS, 0,
      "from_number($type, number, /)\n--\n\nConvert real number to a floating-point number." },
    { "conjugate", qp_float_m_conjugate, QP_METH_NOARGS, 0,
      "conjugate($self, /)\n--\n\nReturn self, the complex conjugate of any float." },
    { "__trunc__", qp_float_m_trunc, QP_METH_NOARGS, 0,
      "__trunc__($self, /)\n--\n\nReturn the Integral closest to x between 0 and x." },
    { "__floor__", qp_float_m_floor, QP_METH_NOARGS, 0,
      "__floor__($self, /)\n--\n\nReturn the floor as an Integral." },
    { "__ceil__", qp_float_m_ceil, QP_METH_NOARGS, 0,
      "__ceil__($self, /)\n--\n\nReturn the ceiling as an Integral." },
    { "__round__", qp_float_m_round, QP_METH_FASTCALL, 0,
      "__round__($self, ndigits=None, /)\n--\n\nReturn the Integral closest to x, rounding half toward even.\n\n"
      "When an argument is passed, work like built-in round(x, ndigits)." },
    { "as_integer_ratio", qp_float_m_as_integer_ratio, QP_METH_NOARGS, 0,
      "as_integer_ratio($self, /)\n--\n\nReturn a pair of integers, whose ratio is exactly equal to the original float.\n\n"
      "The ratio is in lowest terms and has a positive denominator.  Raise\n"
      "OverflowError on infinities and a ValueError on NaNs.\n\n"
      ">>> (10.0).as_integer_ratio()\n(10, 1)\n>>> (0.0).as_integer_ratio()\n(0, 1)\n"
      ">>> (-.25).as_integer_ratio()\n(-1, 4)" },
    { "fromhex", qp_float_m_fromhex, QP_METH_O | QP_METH_CLASS, 0,
      "fromhex($type, string, /)\n--\n\nCreate a floating-point number from a hexadecimal string.\n\n"
      ">>> float.fromhex('0x1.ffffp10')\n2047.984375\n>>> float.fromhex('-0x1p-1074')\n-5e-324" },
    { "hex", qp_float_m_hex, QP_METH_NOARGS, 0,
      "hex($self, /)\n--\n\nReturn a hexadecimal representation of a floating-point number.\n\n"
      ">>> (-0.1).hex()\n'-0x1.999999999999ap-4'\n>>> 3.14159.hex()\n'0x1.921f9f01b866ep+1'" },
    { "is_integer", qp_float_m_is_integer, QP_METH_NOARGS, 0,
      "is_integer($self, /)\n--\n\nReturn True if the float is an integer." },
    { "__getnewargs__", qp_float_m_getnewargs, QP_METH_NOARGS, 0,
      "__getnewargs__($self, /)\n--\n\n" },
    { "__getformat__", qp_float_m_getformat, QP_METH_O | QP_METH_CLASS, 0,
      "__getformat__($type, typestr, /)\n--\n\nYou probably don't want to use this function.\n\n"
      "  typestr\n    Must be 'double' or 'float'.\n\n"
      "It exists mainly to be used in Python's test suite.\n\n"
      "This function returns whichever of 'IEEE, big-endian' or 'IEEE,\n"
      "little-endian' best describes the format of floating-point numbers\n"
      "used by the C type named by typestr." },
    { "__format__", qp_float_m_format, QP_METH_O, 0,
      "__format__($self, format_spec, /)\n--\n\nFormats the float according to format_spec." },
    { NULL }
};

static const QPGetSetDef qp_float_getsets[] = {
    { "real", qp_float_get_real, NULL, 0, "the real part of a complex number" },
    { "imag", qp_float_get_imag, NULL, 0, "the imaginary part of a complex number" },
    { NULL }
};

static const QPTypeSpec qp_float_spec = {
    .name = "float",
    .doc = "float(x=0, /)\n--\n\nConvert a string or number to a floating-point number, if possible.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF |
             QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BOXED,
    .slots = {
        .tp_repr = qp_float_repr_slot,
        .tp_hash = qp_float_hash_slot,
        .tp_richcompare = qp_float_richcompare,
        .tp_new = qp_float_tp_new,
        .nb = {
            .nb_add = qp_float_add,
            .nb_subtract = qp_float_sub,
            .nb_multiply = qp_float_mul,
            .nb_remainder = qp_float_rem,
            .nb_divmod = qp_float_divmod,
            .nb_power = qp_float_pow,
            .nb_negative = qp_float_neg,
            .nb_positive = qp_float_float,
            .nb_absolute = qp_float_abs,
            .nb_bool = qp_float_bool,
            .nb_int = qp_float_trunc_slot,
            .nb_float = qp_float_float,
            .nb_floor_divide = qp_float_floor_div,
            .nb_true_divide = qp_float_div,
        },
    },
    .methods = qp_float_methods,
    .getsets = qp_float_getsets,
};

/* Called from qp_init_numbers (longobject.c) after int and bool. */
static int qp_init_float(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, float), &qp_float_spec);
}
