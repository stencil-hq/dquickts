/*
 * dquickts Python: the complex type (object.h section (c)).
 *
 * Derived from CPython 3.15.2 Objects/complexobject.c (PSF-2.0).
 *
 * complex instances (and complex-subclass instances) are objects of class
 * JS_CLASS_PY_COMPLEX whose u.opaque is a js_malloc'd QPComplex.
 */

/* _Py_ADJUST_ERANGE2 (Include/internal/pycore_pymath.h) */
static inline void qp_adjust_erange2(double x, double y)
{
    if (x == INFINITY || x == -INFINITY || y == INFINITY || y == -INFINITY) {
        if (errno == 0)
            errno = ERANGE;
    }
    else if (errno == ERANGE) {
        errno = 0;
    }
}


static QPComplex qp_c_one = {1., 0.};

static QPComplex
qp_c_sum(QPComplex a, QPComplex b)
{
    QPComplex r;
    r.real = a.real + b.real;
    r.imag = a.imag + b.imag;
    return r;
}

static QPComplex
qp_cr_sum(QPComplex a, double b)
{
    QPComplex r = a;
    r.real += b;
    return r;
}

static inline QPComplex
qp_rc_sum(double a, QPComplex b)
{
    return qp_cr_sum(b, a);
}

static QPComplex
qp_c_diff(QPComplex a, QPComplex b)
{
    QPComplex r;
    r.real = a.real - b.real;
    r.imag = a.imag - b.imag;
    return r;
}

static QPComplex
qp_cr_diff(QPComplex a, double b)
{
    QPComplex r = a;
    r.real -= b;
    return r;
}

static QPComplex
qp_rc_diff(double a, QPComplex b)
{
    QPComplex r;
    r.real = a - b.real;
    r.imag = -b.imag;
    return r;
}

static QPComplex
qp_c_prod(QPComplex z, QPComplex w)
{
    double a = z.real, b = z.imag, c = w.real, d = w.imag;
    double ac = a*c, bd = b*d, ad = a*d, bc = b*c;
    QPComplex r = {ac - bd, ad + bc};

    /* Recover infinities that computed as nan+nanj.  See e.g. the C11,
       Annex G.5.1, routine _Cmultd(). */
    if (isnan(r.real) && isnan(r.imag)) {
        int recalc = 0;

        if (isinf(a) || isinf(b)) {  /* z is infinite */
            /* "Box" the infinity and change nans in the other factor to 0 */
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            if (isnan(c)) {
                c = copysign(0.0, c);
            }
            if (isnan(d)) {
                d = copysign(0.0, d);
            }
            recalc = 1;
        }
        if (isinf(c) || isinf(d)) {  /* w is infinite */
            /* "Box" the infinity and change nans in the other factor to 0 */
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            if (isnan(a)) {
                a = copysign(0.0, a);
            }
            if (isnan(b)) {
                b = copysign(0.0, b);
            }
            recalc = 1;
        }
        if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            /* Recover infinities from overflow by changing nans to 0 */
            if (isnan(a)) {
                a = copysign(0.0, a);
            }
            if (isnan(b)) {
                b = copysign(0.0, b);
            }
            if (isnan(c)) {
                c = copysign(0.0, c);
            }
            if (isnan(d)) {
                d = copysign(0.0, d);
            }
            recalc = 1;
        }
        if (recalc) {
            r.real = INFINITY*(a*c - b*d);
            r.imag = INFINITY*(a*d + b*c);
        }
    }

    return r;
}

static QPComplex
qp_cr_prod(QPComplex a, double b)
{
    QPComplex r = a;
    r.real *= b;
    r.imag *= b;
    return r;
}

static inline QPComplex
qp_rc_prod(double a, QPComplex b)
{
    return qp_cr_prod(b, a);
}

/* Avoid bad optimization on Windows ARM64 until the compiler is fixed */
static QPComplex
qp_c_quot(QPComplex a, QPComplex b)
{
    /******************************************************************
    This was the original algorithm.  It's grossly prone to spurious
    overflow and underflow errors.  It also merrily divides by 0 despite
    checking for that(!).  The code still serves a doc purpose here, as
    the algorithm following is a simple by-cases transformation of this
    one:

    QPComplex r;
    double d = b.real*b.real + b.imag*b.imag;
    if (d == 0.)
        errno = EDOM;
    r.real = (a.real*b.real + a.imag*b.imag)/d;
    r.imag = (a.imag*b.real - a.real*b.imag)/d;
    return r;
    ******************************************************************/

    /* This algorithm is better, and is pretty obvious:  first divide the
     * numerators and denominator by whichever of {b.real, b.imag} has
     * larger magnitude.  The earliest reference I found was to CACM
     * Algorithm 116 (Complex Division, Robert L. Smith, Stanford
     * University).
     */
     QPComplex r;      /* the result */
     const double abs_breal = b.real < 0 ? -b.real : b.real;
     const double abs_bimag = b.imag < 0 ? -b.imag : b.imag;

    if (abs_breal >= abs_bimag) {
        /* divide tops and bottom by b.real */
        if (abs_breal == 0.0) {
            errno = EDOM;
            r.real = r.imag = 0.0;
        }
        else {
            const double ratio = b.imag / b.real;
            const double denom = b.real + b.imag * ratio;
            r.real = (a.real + a.imag * ratio) / denom;
            r.imag = (a.imag - a.real * ratio) / denom;
        }
    }
    else if (abs_bimag >= abs_breal) {
        /* divide tops and bottom by b.imag */
        const double ratio = b.real / b.imag;
        const double denom = b.real * ratio + b.imag;
        assert(b.imag != 0.0);
        r.real = (a.real * ratio + a.imag) / denom;
        r.imag = (a.imag * ratio - a.real) / denom;
    }
    else {
        /* At least one of b.real or b.imag is a NaN */
        r.real = r.imag = NAN;
    }

    /* Recover infinities and zeros that computed as nan+nanj.  See e.g.
       the C11, Annex G.5.2, routine _Cdivd(). */
    if (isnan(r.real) && isnan(r.imag)) {
        if ((isinf(a.real) || isinf(a.imag))
            && isfinite(b.real) && isfinite(b.imag))
        {
            const double x = copysign(isinf(a.real) ? 1.0 : 0.0, a.real);
            const double y = copysign(isinf(a.imag) ? 1.0 : 0.0, a.imag);
            r.real = INFINITY * (x*b.real + y*b.imag);
            r.imag = INFINITY * (y*b.real - x*b.imag);
        }
        else if ((isinf(abs_breal) || isinf(abs_bimag))
                 && isfinite(a.real) && isfinite(a.imag))
        {
            const double x = copysign(isinf(b.real) ? 1.0 : 0.0, b.real);
            const double y = copysign(isinf(b.imag) ? 1.0 : 0.0, b.imag);
            r.real = 0.0 * (a.real*x + a.imag*y);
            r.imag = 0.0 * (a.imag*x - a.real*y);
        }
    }

    return r;
}

static QPComplex
qp_cr_quot(QPComplex a, double b)
{
    QPComplex r = a;
    if (b) {
        r.real /= b;
        r.imag /= b;
    }
    else {
        errno = EDOM;
        r.real = r.imag = 0.0;
    }
    return r;
}

/* an equivalent of qp_c_quot() function, when 1st argument is real */
static QPComplex
qp_rc_quot(double a, QPComplex b)
{
    QPComplex r;
    const double abs_breal = b.real < 0 ? -b.real : b.real;
    const double abs_bimag = b.imag < 0 ? -b.imag : b.imag;

    if (abs_breal >= abs_bimag) {
        if (abs_breal == 0.0) {
            errno = EDOM;
            r.real = r.imag = 0.0;
        }
        else {
            const double ratio = b.imag / b.real;
            const double denom = b.real + b.imag * ratio;
            r.real = a / denom;
            r.imag = (-a * ratio) / denom;
        }
    }
    else if (abs_bimag >= abs_breal) {
        const double ratio = b.real / b.imag;
        const double denom = b.real * ratio + b.imag;
        assert(b.imag != 0.0);
        r.real = (a * ratio) / denom;
        r.imag = (-a) / denom;
    }
    else {
        r.real = r.imag = NAN;
    }

    if (isnan(r.real) && isnan(r.imag) && isfinite(a)
        && (isinf(abs_breal) || isinf(abs_bimag)))
    {
        const double x = copysign(isinf(b.real) ? 1.0 : 0.0, b.real);
        const double y = copysign(isinf(b.imag) ? 1.0 : 0.0, b.imag);
        r.real = 0.0 * (a*x);
        r.imag = 0.0 * (-a*y);
    }

    return r;
}

static QPComplex
qp_c_pow(QPComplex a, QPComplex b)
{
    QPComplex r;
    double vabs,len,at,phase;
    if (b.real == 0. && b.imag == 0.) {
        r.real = 1.;
        r.imag = 0.;
    }
    else if (a.real == 0. && a.imag == 0.) {
        if (b.imag != 0. || b.real < 0.)
            errno = EDOM;
        r.real = 0.;
        r.imag = 0.;
    }
    else {
        vabs = hypot(a.real,a.imag);
        len = pow(vabs,b.real);
        at = atan2(a.imag, a.real);
        phase = at*b.real;
        if (b.imag != 0.0) {
            len *= exp(-at*b.imag);
            phase += b.imag*log(vabs);
        }
        r.real = len*cos(phase);
        r.imag = len*sin(phase);

        qp_adjust_erange2(r.real, r.imag);
    }
    return r;
}

static QPComplex
qp_c_powu(QPComplex x, long n)
{
    QPComplex r, p;
    long mask = 1;
    r = qp_c_one;
    p = x;
    while (mask > 0 && n >= mask) {
        if (n & mask)
            r = qp_c_prod(r,p);
        mask <<= 1;
        p = qp_c_prod(p,p);
    }
    return r;
}

static QPComplex
qp_c_powi(QPComplex x, long n)
{
    if (n > 0)
        return qp_c_powu(x,n);
    else
        return qp_c_quot(qp_c_one, qp_c_powu(x,-n));

}

static double
qp_c_abs(QPComplex z)
{
    /* sets errno = ERANGE on overflow;  otherwise errno = 0 */
    double result;

    if (!isfinite(z.real) || !isfinite(z.imag)) {
        /* C99 rules: if either the real or the imaginary part is an
           infinity, return infinity, even if the other part is a
           NaN. */
        if (isinf(z.real)) {
            result = fabs(z.real);
            errno = 0;
            return result;
        }
        if (isinf(z.imag)) {
            result = fabs(z.imag);
            errno = 0;
            return result;
        }
        /* either the real or imaginary part is a NaN,
           and neither is infinite. Result should be NaN. */
        return NAN;
    }
    result = hypot(z.real, z.imag);
    if (!isfinite(result))
        errno = ERANGE;
    else
        errno = 0;
    return result;
}


/* ======================================================================== */
/* complex object                                                           */
/* ======================================================================== */

/* Shared with floatobject.c (same translation unit). */
static int qp_float_convert(JSContext *ctx, JSValueConst v, double *out);
static char *qp_float_str_to_ascii(JSContext *ctx, JSValueConst s, size_t *plen);
static JSValue qp_float_result(JSContext *ctx, double d);
static JSValue qp_number_format(JSContext *ctx, JSValueConst self, JSValueConst spec,
                                double re, double im,
                                int (*fn)(DynBuf *out, double re, double im, const void *spec,
                                          int kind, size_t len, const char *type_name,
                                          qp_error *err));

static void qp_complex_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    if (p->u.opaque) {
        js_free_rt(rt, p->u.opaque);
        p->u.opaque = NULL;
    }
}

static inline BOOL qp_complex_get(JSValueConst v, QPComplex *c)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (p->class_id == JS_CLASS_PY_COMPLEX && p->u.opaque) {
            *c = *(QPComplex *)p->u.opaque;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL qp_complex_check(JSContext *ctx, JSValueConst v)
{
    QPComplex c;
    return qp_complex_get(v, &c);
}

static BOOL qp_complex_check_exact(JSContext *ctx, JSValueConst v)
{
    QPComplex c;
    return qp_complex_get(v, &c) && qp_type_of(ctx, v) == QP_TYPE(ctx, complex);
}

/* complex_subtype_from_doubles: instance of t (complex or a subclass). */
static JSValue qp_complex_new_type(JSContext *ctx, QPType *t, double real, double imag)
{
    JSValue obj;
    QPComplex *c = js_malloc(ctx, sizeof(*c));
    if (!c)
        return JS_EXCEPTION;
    c->real = real;
    c->imag = imag;
    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_COMPLEX);
    if (JS_IsException(obj)) {
        js_free(ctx, c);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = c;
    return obj;
}

static JSValue qp_complex_new(JSContext *ctx, double real, double imag)
{
    return qp_complex_new_type(ctx, QP_TYPE(ctx, complex), real, imag);
}

static JSValue qp_complex_from_c(JSContext *ctx, QPComplex c)
{
    return qp_complex_new(ctx, c.real, c.imag);
}

/* try_complex_special_method: __complex__() result (exact complex or a
   subclass instance after a DeprecationWarning), QP_NULL when there is no
   __complex__, JS_EXCEPTION on error. */
static JSValue qp_complex_try_special(JSContext *ctx, JSValueConst op)
{
    JSValue f, res;

    f = qp_lookup_special(ctx, op, QP_ATOM(ctx, __complex__));
    if (JS_IsUninitialized(f))
        return QP_NULL;
    if (JS_IsException(f))
        return f;
    res = qp_vectorcall(ctx, f, NULL, 0, QP_NULL);
    JS_FreeValue(ctx, f);
    if (JS_IsException(res) || qp_complex_check_exact(ctx, res))
        return res;
    if (!qp_complex_check(ctx, res)) {
        qp_raise_type_error(ctx, "%T.__complex__() must return a complex, not %T", op, res);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    /* Issue #29894: warn if 'res' not of exact type complex. */
    if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                "%T.__complex__() must return a complex, not %T.  "
                "The ability to return an instance of a strict subclass of complex "
                "is deprecated, and may be removed in a future version of Python.",
                op, res) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* PyComplex_AsCComplex */
static int qp_complex_as_c(JSContext *ctx, JSValueConst v, QPComplex *out)
{
    JSValue newop;

    if (qp_complex_get(v, out))
        return 0;
    newop = qp_complex_try_special(ctx, v);
    if (JS_IsException(newop))
        return -1;
    if (!JS_IsUninitialized(newop)) {
        qp_complex_get(newop, out);
        JS_FreeValue(ctx, newop);
        return 0;
    }
    out->imag = 0.0;
    return qp_float_as_double(ctx, v, &out->real);
}

static JSValue qp_complex_repr_slot(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    DynBuf b;
    JSValue res;

    qp_complex_get(v, &c);
    js_dbuf_init(ctx, &b);
    if (qp_complex_repr(c.real, c.imag, &b) < 0) {
        dbuf_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    res = JS_NewStringLen(ctx, (const char *)b.buf, b.size);
    dbuf_free(&b);
    return res;
}

static Py_hash_t qp_complex_hash_slot(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(v, &c);
    return qp_hash_complex(c.real, c.imag, (uintptr_t)JS_VALUE_GET_PTR(v));
}

/* TO_COMPLEX / real_to_complex: 1 = *c set, 0 = NotImplemented, -1 = error. */
static int qp_complex_to_c(JSContext *ctx, JSValueConst v, QPComplex *c)
{
    if (qp_complex_get(v, c))
        return 1;
    c->imag = 0.0;
    return qp_float_convert(ctx, v, &c->real);
}

/* Complex arithmetic rules implement special mixed-mode case where combining
   a pure-real (float or int) value and a complex value is performed directly
   without first coercing the real value to a complex value. */
#define QP_COMPLEX_BINOP(NAME, FUNC)                                        \
    static JSValue qp_complex_##NAME(JSContext *ctx, JSValueConst v, JSValueConst w) \
    {                                                                       \
        QPComplex a, b;                                                     \
        int r_;                                                             \
        errno = 0;                                                          \
        if (qp_complex_get(w, &b)) {                                        \
            if (qp_complex_get(v, &a)) {                                    \
                a = qp_c_##FUNC(a, b);                                      \
            } else if ((r_ = qp_float_convert(ctx, v, &a.real)) <= 0) {     \
                return r_ < 0 ? JS_EXCEPTION : qp_not_implemented(ctx);     \
            } else {                                                        \
                a = qp_rc_##FUNC(a.real, b);                                \
            }                                                               \
        } else if (!qp_complex_get(v, &a)) {                                \
            return qp_not_implemented(ctx);                                 \
        } else {                                                            \
            double bd;                                                      \
            if ((r_ = qp_float_convert(ctx, w, &bd)) <= 0)                  \
                return r_ < 0 ? JS_EXCEPTION : qp_not_implemented(ctx);     \
            a = qp_cr_##FUNC(a, bd);                                        \
        }                                                                   \
        if (errno == EDOM)                                                  \
            return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError), "division by zero"); \
        return qp_complex_from_c(ctx, a);                                   \
    }

QP_COMPLEX_BINOP(add, sum)
QP_COMPLEX_BINOP(mul, prod)
QP_COMPLEX_BINOP(sub, diff)
QP_COMPLEX_BINOP(div, quot)

/* complex_pow; also the target of float_pow for negative ** fractional. */
static JSValue qp_complex_nb_power(JSContext *ctx, JSValueConst v, JSValueConst w, JSValueConst z)
{
    QPComplex p, a, b;
    int r;

    if ((r = qp_complex_to_c(ctx, v, &a)) <= 0)
        return r < 0 ? JS_EXCEPTION : qp_not_implemented(ctx);
    if ((r = qp_complex_to_c(ctx, w, &b)) <= 0)
        return r < 0 ? JS_EXCEPTION : qp_not_implemented(ctx);
    if (!JS_IsUndefined(z) && !JS_IsNull(z))
        return qp_raise_value_error(ctx, "complex modulo");
    errno = 0;
    /* small integral exponent: faster and more accurate algorithm */
    if (b.imag == 0.0 && b.real == floor(b.real) && fabs(b.real) <= 100.0) {
        p = qp_c_powi(a, (long)b.real);
        qp_adjust_erange2(p.real, p.imag);
    } else {
        p = qp_c_pow(a, b);
    }
    if (errno == EDOM)
        return qp_raise(ctx, QP_EXC(ctx, ZeroDivisionError),
                        "zero to a negative or complex power");
    else if (errno == ERANGE)
        return qp_raise(ctx, QP_EXC(ctx, OverflowError), "complex exponentiation");
    return qp_complex_from_c(ctx, p);
}

static JSValue qp_complex_neg(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(v, &c);
    return qp_complex_new(ctx, -c.real, -c.imag);
}

static JSValue qp_complex_pos(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    if (qp_complex_check_exact(ctx, v))
        return JS_DupValue(ctx, v);
    qp_complex_get(v, &c);
    return qp_complex_from_c(ctx, c);
}

static JSValue qp_complex_abs(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    double result;
    qp_complex_get(v, &c);
    errno = 0;
    result = qp_c_abs(c);
    if (errno == ERANGE)
        return qp_raise(ctx, QP_EXC(ctx, OverflowError), "absolute value too large");
    return qp_float_result(ctx, result);
}

static int qp_complex_bool(JSContext *ctx, JSValueConst v)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(v, &c);
    return c.real != 0.0 || c.imag != 0.0;
}

static JSValue qp_complex_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPComplex i, j;
    double d;
    int equal;

    if ((op != QP_CMP_EQ && op != QP_CMP_NE) || !qp_complex_get(v, &i))
        return qp_not_implemented(ctx);
    if (qp_long_check(ctx, w)) {
        /* check for a 0.0 imaginary part first to avoid the rich
           comparison when possible */
        if (i.imag == 0.0) {
            JSValue jv = qp_float_result(ctx, i.real), res;
            res = qp_richcompare(ctx, jv, w, op);
            JS_FreeValue(ctx, jv);
            return res;
        }
        equal = 0;
    } else if (qp_float_check(ctx, w)) {
        if (qp_float_as_double(ctx, w, &d) < 0)
            return JS_EXCEPTION;
        equal = (i.real == d && i.imag == 0.0);
    } else if (qp_complex_get(w, &j)) {
        equal = (i.real == j.real && i.imag == j.imag);
    } else {
        return qp_not_implemented(ctx);
    }
    return qp_bool_new(equal == (op == QP_CMP_EQ));
}

/* ---- methods ------------------------------------------------------------ */

static JSValue qp_complex_m_conjugate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(self, &c);
    return qp_complex_new(ctx, c.real, -c.imag);
}

static JSValue qp_complex_m_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPComplex c = { 0, 0 };
    JSValue items[2];
    qp_complex_get(self, &c);
    items[0] = qp_float_result(ctx, c.real);
    items[1] = qp_float_result(ctx, c.imag);
    return qp_tuple_new_steal(ctx, 2, items);
}

static int qp_complex_format_fn(DynBuf *out, double re, double im, const void *spec, int kind,
                                size_t len, const char *type_name, qp_error *err)
{
    return qp_format_complex(out, re, im, spec, kind, len, type_name, err);
}

static JSValue qp_complex_m_format(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(self, &c);
    return qp_number_format(ctx, self, args[0], c.real, c.imag, qp_complex_format_fn);
}

static JSValue qp_complex_m_complex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_complex_pos(ctx, self);
}

/* complex.from_number(number) (classmethod) */
static JSValue qp_complex_m_from_number(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    QPType *type = qp_type_from_value(self);
    QPComplex cv;
    JSValue result;

    if (type == QP_TYPE(ctx, complex) && qp_complex_check_exact(ctx, args[0]))
        return JS_DupValue(ctx, args[0]);
    if (qp_complex_as_c(ctx, args[0], &cv) < 0)
        return JS_EXCEPTION;
    result = qp_complex_from_c(ctx, cv);
    if (type && type != QP_TYPE(ctx, complex) && !JS_IsException(result)) {
        JSValue r2 = qp_vectorcall(ctx, QP_TYPE_VALUE(type), (JSValueConst *)&result, 1, QP_NULL);
        JS_FreeValue(ctx, result);
        return r2;
    }
    return result;
}

static JSValue qp_complex_get_real(JSContext *ctx, JSValueConst self, int magic)
{
    QPComplex c = { 0, 0 };
    qp_complex_get(self, &c);
    return qp_float_result(ctx, magic ? c.imag : c.real);
}

/* ---- construction -------------------------------------------------------- */

static inline int qp_complex_isspace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* PyOS_string_to_double(s, &end, NULL) where ValueError is cleared:
   returns 0 (value/end set; end == s if nothing parsed) or -1 (MemoryError
   raised). */
static int qp_complex_strtod(JSContext *ctx, const char *s, char **end, double *out)
{
    qp_error err;
    memset(&err, 0, sizeof(err));
    *out = qp_string_to_double(s, end, QP_ERR_NONE, &err);
    if (err.kind != QP_ERR_NONE) {
        int mem = err.kind != QP_ERR_VALUE;
        qp_error_clear(&err);
        if (mem) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        *end = (char *)s;
    }
    return 0;
}

/* complex_from_string_inner over the NUL-terminated ASCII s[0..len) */
static JSValue qp_complex_from_string_inner(JSContext *ctx, const char *s, Py_ssize_t len,
                                            QPType *type)
{
    double x = 0.0, y = 0.0, z;
    int got_bracket = 0;
    const char *start;
    char *end;

    /* position on first nonblank */
    start = s;
    while (qp_complex_isspace(*s))
        s++;
    if (*s == '(') {
        /* Skip over possible bracket from repr(). */
        got_bracket = 1;
        s++;
        while (qp_complex_isspace(*s))
            s++;
    }

    /* forms: <float>, <float>j, <float><signed-float>j, and for backwards
       compatibility <float><sign>j, <sign>j, j */
    if (qp_complex_strtod(ctx, s, &end, &z) < 0)
        return JS_EXCEPTION;
    if (end != s) {
        /* all 4 forms starting with <float> land here */
        s = end;
        if (*s == '+' || *s == '-') {
            /* <float><signed-float>j | <float><sign>j */
            x = z;
            if (qp_complex_strtod(ctx, s, &end, &y) < 0)
                return JS_EXCEPTION;
            if (end != s) {
                /* <float><signed-float>j */
                s = end;
            } else {
                /* <float><sign>j */
                y = *s == '+' ? 1.0 : -1.0;
                s++;
            }
            if (!(*s == 'j' || *s == 'J'))
                goto parse_error;
            s++;
        } else if (*s == 'j' || *s == 'J') {
            /* <float>j */
            s++;
            y = z;
        } else {
            /* <float> */
            x = z;
        }
    } else {
        /* not starting with <float>; must be <sign>j or j */
        if (*s == '+' || *s == '-') {
            /* <sign>j */
            y = *s == '+' ? 1.0 : -1.0;
            s++;
        } else {
            /* j */
            y = 1.0;
        }
        if (!(*s == 'j' || *s == 'J'))
            goto parse_error;
        s++;
    }

    /* trailing whitespace and closing bracket */
    while (qp_complex_isspace(*s))
        s++;
    if (got_bracket) {
        if (*s != ')')
            goto parse_error;
        s++;
        while (qp_complex_isspace(*s))
            s++;
    }

    /* we should now be at the end of the string */
    if (s - start != len)
        goto parse_error;
    return qp_complex_new_type(ctx, type, x, y);

 parse_error:
    return qp_raise_value_error(ctx, "complex() arg is a malformed string");
}

/* complex_subtype_from_string (v is a str or str subclass) */
static JSValue qp_complex_from_string(JSContext *ctx, QPType *type, JSValueConst v)
{
    JSValue sv, res;
    size_t len;
    char *s, *dup;
    Py_ssize_t n;

    sv = qp_str_value(ctx, v);
    if (JS_IsException(sv))
        return sv;
    s = qp_float_str_to_ascii(ctx, sv, &len);
    JS_FreeValue(ctx, sv);
    if (!s)
        return JS_EXCEPTION;
    /* _Py_string_to_number_with_underscores */
    if (!strchr(s, '_')) {
        res = qp_complex_from_string_inner(ctx, s, len, type);
        js_free(ctx, s);
        return res;
    }
    dup = js_malloc(ctx, len + 1);
    if (!dup) {
        js_free(ctx, s);
        return JS_EXCEPTION;
    }
    n = qp_number_strip_underscores(s, len, dup);
    js_free(ctx, s);
    if (n < 0) {
        js_free(ctx, dup);
        return qp_raise_value_error(ctx, "could not convert string to complex: %R", v);
    }
    res = qp_complex_from_string_inner(ctx, dup, n, type);
    js_free(ctx, dup);
    return res;
}

/* type(x) has nb_float or nb_index */
static BOOL qp_complex_has_real_slots(JSContext *ctx, JSValueConst x)
{
    QPType *t = qp_type_of(ctx, x);
    return t->slots.nb.nb_float != NULL || t->slots.nb.nb_index != NULL;
}

/* complex_new_impl: the 'real'/'imag' form (r may be QP_NULL = 0, i QP_NULL). */
static JSValue qp_complex_new_impl(JSContext *ctx, QPType *type, JSValueConst r0, JSValueConst i)
{
    JSValue tmp, own = JS_UNDEFINED;
    JSValueConst r = JS_IsUninitialized(r0) ? JS_NewInt32(ctx, 0) : r0;
    JSValueConst orig_r = r;
    QPComplex cr, ci;
    int cr_is_complex = 0, ci_is_complex = 0;
    double d;

    /* DEPRECATED: the call of try_complex_special_method() for the "real"
       part will be dropped after the end of the deprecation period. */
    tmp = qp_complex_try_special(ctx, r);
    if (JS_IsException(tmp))
        return tmp;
    if (!JS_IsUninitialized(tmp)) {
        own = tmp;
        r = own;
    }
    if (!qp_complex_has_real_slots(ctx, r) && !qp_complex_check(ctx, r)) {
        qp_raise_type_error(ctx, "complex() argument 'real' must be a real number, not %T", r);
        JS_FreeValue(ctx, own);
        return JS_EXCEPTION;
    }
    if (!JS_IsUninitialized(i) &&
        !qp_complex_has_real_slots(ctx, i) && !qp_complex_check(ctx, i)) {
        qp_raise_type_error(ctx, "complex() argument 'imag' must be a real number, not %T", i);
        JS_FreeValue(ctx, own);
        return JS_EXCEPTION;
    }

    if (qp_complex_get(r, &cr)) {
        /* a complex subtype keeps only its real & imag parts here */
        cr_is_complex = 1;
        JS_FreeValue(ctx, own);
        if (!qp_complex_has_real_slots(ctx, orig_r)) {
            if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "complex() argument 'real' must be a real number, not %T",
                        orig_r) < 0)
                return JS_EXCEPTION;
        }
    } else {
        tmp = qp_number_float(ctx, r);
        if (JS_IsException(tmp))
            return tmp;
        if (qp_float_as_double(ctx, tmp, &cr.real) < 0) {
            JS_FreeValue(ctx, tmp);
            return JS_EXCEPTION;
        }
        cr.imag = 0.0;
        JS_FreeValue(ctx, tmp);
    }
    if (JS_IsUninitialized(i)) {
        ci.real = cr.imag;
    } else if (qp_complex_get(i, &ci)) {
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "complex() argument 'imag' must be a real number, not %T", i) < 0)
            return JS_EXCEPTION;
        ci_is_complex = 1;
    } else {
        tmp = qp_number_float(ctx, i);
        if (JS_IsException(tmp))
            return tmp;
        if (qp_float_as_double(ctx, tmp, &d) < 0) {
            JS_FreeValue(ctx, tmp);
            return JS_EXCEPTION;
        }
        ci.real = d;
        JS_FreeValue(ctx, tmp);
    }
    /* correction for non-canonical (complex) real/imag arguments */
    if (ci_is_complex)
        cr.real -= ci.imag;
    if (cr_is_complex && !JS_IsUninitialized(i))
        ci.real += cr.imag;
    return qp_complex_new_type(ctx, type, cr.real, ci.real);
}

/* actual_complex_new: complex(), complex(x) and the real/imag form. */
static JSValue qp_complex_tp_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "real", "imag", NULL };
    static const QPArgSpec spec = { "complex", kwlist, 0, 2, 0 };
    Py_ssize_t nargs = qp_nargs(nargsf);
    Py_ssize_t nkw = JS_IsUninitialized(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames);
    JSValueConst arg, out[2];
    JSValue tmp, res;
    QPComplex c;

    if (nargs > 1 || nkw > 0) {
        if (qp_arg_parse(ctx, &spec, args, (int)nargs, kwnames, out) < 0)
            return JS_EXCEPTION;
        return qp_complex_new_impl(ctx, type, out[0], out[1]);
    }
    if (nargs == 0)
        return qp_complex_new_type(ctx, type, 0, 0);

    arg = args[0];
    /* a single exact complex argument is returned as is */
    if (type == QP_TYPE(ctx, complex) && qp_complex_check_exact(ctx, arg))
        return JS_DupValue(ctx, arg);
    if (qp_str_check(ctx, arg))
        return qp_complex_from_string(ctx, type, arg);
    tmp = qp_complex_try_special(ctx, arg);
    if (JS_IsException(tmp))
        return tmp;
    if (!JS_IsUninitialized(tmp)) {
        qp_complex_get(tmp, &c);
        JS_FreeValue(ctx, tmp);
        return qp_complex_new_type(ctx, type, c.real, c.imag);
    }
    if (qp_complex_get(arg, &c))
        return qp_complex_new_type(ctx, type, c.real, c.imag);
    if (qp_complex_has_real_slots(ctx, arg)) {
        double r;
        if (qp_float_as_double(ctx, arg, &r) < 0)
            return JS_EXCEPTION;
        return qp_complex_new_type(ctx, type, r, 0);
    }
    res = qp_raise_type_error(ctx, "complex() argument must be a string or a number, not %T", arg);
    return res;
}

static const QPMethodDef qp_complex_methods[] = {
    { "from_number", qp_complex_m_from_number, QP_METH_O | QP_METH_CLASS, 0,
      "from_number($type, number, /)\n--\n\nConvert number to a complex floating-point number." },
    { "conjugate", qp_complex_m_conjugate, QP_METH_NOARGS, 0,
      "conjugate($self, /)\n--\n\nReturn the complex conjugate of its argument. (3-4j).conjugate() == 3+4j." },
    { "__complex__", qp_complex_m_complex, QP_METH_NOARGS, 0,
      "__complex__($self, /)\n--\n\nConvert this value to exact type complex." },
    { "__getnewargs__", qp_complex_m_getnewargs, QP_METH_NOARGS, 0,
      "__getnewargs__($self, /)\n--\n\n" },
    { "__format__", qp_complex_m_format, QP_METH_O, 0,
      "__format__($self, format_spec, /)\n--\n\nConvert to a string according to format_spec." },
    { NULL }
};

static const QPGetSetDef qp_complex_getsets[] = {
    { "real", qp_complex_get_real, NULL, 0, "the real part of a complex number" },
    { "imag", qp_complex_get_real, NULL, 1, "the imaginary part of a complex number" },
    { NULL }
};

static const QPTypeSpec qp_complex_spec = {
    .name = "complex",
    .doc = "complex(real=0, imag=0)\n--\n\n"
           "Create a complex number from a string or numbers.\n\n"
           "If a string is given, parse it as a complex number.\n"
           "If a single number is given, convert it to a complex number.\n"
           "If the 'real' or 'imag' arguments are given, create a complex number\n"
           "with the specified real and imaginary components.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF |
             QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_COMPLEX,
    .slots = {
        .tp_repr = qp_complex_repr_slot,
        .tp_hash = qp_complex_hash_slot,
        .tp_richcompare = qp_complex_richcompare,
        .tp_new = qp_complex_tp_new,
        .nb = {
            .nb_add = qp_complex_add,
            .nb_subtract = qp_complex_sub,
            .nb_multiply = qp_complex_mul,
            .nb_power = qp_complex_nb_power,
            .nb_negative = qp_complex_neg,
            .nb_positive = qp_complex_pos,
            .nb_absolute = qp_complex_abs,
            .nb_bool = qp_complex_bool,
            .nb_true_divide = qp_complex_div,
        },
    },
    .methods = qp_complex_methods,
    .getsets = qp_complex_getsets,
};

/* Called from qp_init_numbers (longobject.c) after float. */
static int qp_init_complex(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, complex), &qp_complex_spec);
}
