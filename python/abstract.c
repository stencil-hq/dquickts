/*
 * dquickts Python: abstract object protocol (object.h section (h),
 * abstract.c half): number, sequence, mapping, iterator protocols,
 * isinstance/issubclass, format(), and the match_class / match_keys
 * helpers.
 *
 * Derived from CPython 3.15 Objects/abstract.c (PSF-2.0) and the
 * _PyEval_GetIter / _PyForIter_VirtualIteratorNext / _PyEval_MatchClass /
 * _PyEval_MatchKeys helpers of Python/ceval.c (PSF-2.0).
 */

/* object.c helpers */
static const char *qp_obj_tpname(JSContext *ctx, QPType *t, char *buf, int size);
static void qp_obj_clear_exc(JSContext *ctx);
static int qp_obj_enter_recursive(JSContext *ctx);
/* genericaliasobject.c (unit k): borrowed args tuple of a types.UnionType */
static JSValue qp_union_args(JSValueConst self);

#define QP_ABS_NAMEBUF 256

/* type_error(): TypeError with the tp_name of obj's type as the only %s. */
static JSValue qp_abs_type_error(JSContext *ctx, const char *fmt, JSValueConst obj)
{
    char buf[QP_ABS_NAMEBUF];
    return qp_raise_type_error(ctx, fmt,
                               qp_obj_tpname(ctx, qp_type_of(ctx, obj), buf, sizeof(buf)));
}

static BOOL qp_abs_is_int_tag(JSValueConst v)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return TRUE;
    default:
        return FALSE;
    }
}

/* _PyIndex_Check */
static BOOL qp_abs_index_check(JSContext *ctx, JSValueConst v)
{
    if (qp_abs_is_int_tag(v))
        return TRUE;
    return qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
}

/* tp_as_sequence != NULL (our slot tables are embedded: any slot set;
   heap types always have one in CPython) */
static BOOL qp_abs_has_sq(const QPType *t)
{
    const QPSequenceMethods *m = &t->slots.sq;
    return (t->flags & QP_TPFLAGS_HEAPTYPE) ||
        m->sq_length || m->sq_concat || m->sq_repeat || m->sq_item ||
        m->sq_ass_item || m->sq_contains || m->sq_inplace_concat ||
        m->sq_inplace_repeat;
}

/* ------------------------------------------------------------------------ */
/* Number protocol                                                           */
/* ------------------------------------------------------------------------ */

#define QP_NB_OFF(x) ((uint16_t)offsetof(QPNumberMethods, x))
#define QP_NB_BINOP(t, off) (*(QPBinaryFunc **)((char *)&(t)->slots.nb + (off)))
#define QP_NB_TERNOP(t, off) (*(QPTernaryFunc **)((char *)&(t)->slots.nb + (off)))

typedef struct QPBinopInfo {
    uint16_t slot, islot;
    const char *name, *iname;
} QPBinopInfo;

static const QPBinopInfo qp_binop_info[QP_NB_INPLACE_ADD] = {
    [QP_NB_ADD] = { QP_NB_OFF(nb_add), QP_NB_OFF(nb_inplace_add), "+", "+=" },
    [QP_NB_AND] = { QP_NB_OFF(nb_and), QP_NB_OFF(nb_inplace_and), "&", "&=" },
    [QP_NB_FLOOR_DIVIDE] = { QP_NB_OFF(nb_floor_divide), QP_NB_OFF(nb_inplace_floor_divide), "//", "//=" },
    [QP_NB_LSHIFT] = { QP_NB_OFF(nb_lshift), QP_NB_OFF(nb_inplace_lshift), "<<", "<<=" },
    [QP_NB_MATRIX_MULTIPLY] = { QP_NB_OFF(nb_matrix_multiply), QP_NB_OFF(nb_inplace_matrix_multiply), "@", "@=" },
    [QP_NB_MULTIPLY] = { QP_NB_OFF(nb_multiply), QP_NB_OFF(nb_inplace_multiply), "*", "*=" },
    [QP_NB_REMAINDER] = { QP_NB_OFF(nb_remainder), QP_NB_OFF(nb_inplace_remainder), "%", "%=" },
    [QP_NB_OR] = { QP_NB_OFF(nb_or), QP_NB_OFF(nb_inplace_or), "|", "|=" },
    [QP_NB_POWER] = { QP_NB_OFF(nb_power), QP_NB_OFF(nb_inplace_power), "** or pow()", "**=" },
    [QP_NB_RSHIFT] = { QP_NB_OFF(nb_rshift), QP_NB_OFF(nb_inplace_rshift), ">>", ">>=" },
    [QP_NB_SUBTRACT] = { QP_NB_OFF(nb_subtract), QP_NB_OFF(nb_inplace_subtract), "-", "-=" },
    [QP_NB_TRUE_DIVIDE] = { QP_NB_OFF(nb_true_divide), QP_NB_OFF(nb_inplace_true_divide), "/", "/=" },
    [QP_NB_XOR] = { QP_NB_OFF(nb_xor), QP_NB_OFF(nb_inplace_xor), "^", "^=" },
};

/* binary_op1: w.op(v,w) first when type(w) is a strict subclass of
   type(v), then v.op(v,w), then w.op(v,w).  NotImplemented if none
   applies (new reference). */
static JSValue qp_abs_binary_op1(JSContext *ctx, JSValueConst v, JSValueConst w, int off)
{
    QPType *tv = qp_type_of(ctx, v), *tw = qp_type_of(ctx, w);
    QPBinaryFunc *slotv = QP_NB_BINOP(tv, off), *slotw = NULL;
    JSValue x;

    if (tw != tv) {
        slotw = QP_NB_BINOP(tw, off);
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

static JSValue qp_abs_binop_type_error(JSContext *ctx, JSValueConst v, JSValueConst w,
                                       const char *op_name)
{
    char b1[QP_ABS_NAMEBUF], b2[QP_ABS_NAMEBUF];
    return qp_raise_type_error(ctx, "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
                               op_name,
                               qp_obj_tpname(ctx, qp_type_of(ctx, v), b1, sizeof(b1)),
                               qp_obj_tpname(ctx, qp_type_of(ctx, w), b2, sizeof(b2)));
}

/* ternary_op: v.op(v,w,z), w.op(v,w,z), z.op(v,w,z). */
static JSValue qp_abs_ternary_op(JSContext *ctx, JSValueConst v, JSValueConst w, JSValueConst z,
                                 int off, const char *op_name)
{
    QPType *tv = qp_type_of(ctx, v), *tw = qp_type_of(ctx, w), *tz;
    QPTernaryFunc *slotv = QP_NB_TERNOP(tv, off), *slotw = NULL, *slotz;
    JSValue x;
    char b1[QP_ABS_NAMEBUF], b2[QP_ABS_NAMEBUF], b3[QP_ABS_NAMEBUF];

    if (tw != tv) {
        slotw = QP_NB_TERNOP(tw, off);
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && qp_type_is_subtype(tw, tv)) {
            x = slotw(ctx, v, w, z);
            if (!qp_is_notimpl(ctx, x))
                return x;
            JS_FreeValue(ctx, x);
            slotw = NULL;
        }
        x = slotv(ctx, v, w, z);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    if (slotw) {
        x = slotw(ctx, v, w, z);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    tz = qp_type_of(ctx, z);
    slotz = QP_NB_TERNOP(tz, off);
    if (slotz == slotv || slotz == slotw)
        slotz = NULL;
    if (slotz) {
        x = slotz(ctx, v, w, z);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    if (JS_IsUndefined(z) || JS_IsNull(z))
        return qp_raise_type_error(ctx, "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
                                   op_name, qp_obj_tpname(ctx, tv, b1, sizeof(b1)),
                                   qp_obj_tpname(ctx, tw, b2, sizeof(b2)));
    return qp_raise_type_error(ctx, "unsupported operand type(s) for %.100s: '%.100s', '%.100s', '%.100s'",
                               op_name, qp_obj_tpname(ctx, tv, b1, sizeof(b1)),
                               qp_obj_tpname(ctx, tw, b2, sizeof(b2)),
                               qp_obj_tpname(ctx, tz, b3, sizeof(b3)));
}

/* sequence_repeat: seq * n with n an index ("can't multiply sequence by
   non-int of type '%.200s'"). */
static JSValue qp_abs_sequence_repeat(JSContext *ctx, QPSsizeArgFunc *repeatfunc,
                                      JSValueConst seq, JSValueConst n)
{
    Py_ssize_t count;
    if (!qp_abs_index_check(ctx, n))
        return qp_abs_type_error(ctx, "can't multiply sequence by non-int of type '%.200s'", n);
    if (qp_as_ssize(ctx, n, QP_EXC(ctx, OverflowError), &count) < 0)
        return JS_EXCEPTION;
    return repeatfunc(ctx, seq, count);
}

/* PyNumber_Add / Multiply sequence fallbacks after NotImplemented. */
static JSValue qp_abs_add(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    JSValue r = qp_abs_binary_op1(ctx, v, w, QP_NB_OFF(nb_add));
    QPType *tv;
    if (!qp_is_notimpl(ctx, r))
        return r;
    JS_FreeValue(ctx, r);
    tv = qp_type_of(ctx, v);
    if (tv->slots.sq.sq_concat)
        return tv->slots.sq.sq_concat(ctx, v, w);
    return qp_abs_binop_type_error(ctx, v, w, "+");
}

static JSValue qp_abs_multiply(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    JSValue r = qp_abs_binary_op1(ctx, v, w, QP_NB_OFF(nb_multiply));
    QPType *tv, *tw;
    if (!qp_is_notimpl(ctx, r))
        return r;
    JS_FreeValue(ctx, r);
    tv = qp_type_of(ctx, v);
    tw = qp_type_of(ctx, w);
    if (tv->slots.sq.sq_repeat)
        return qp_abs_sequence_repeat(ctx, tv->slots.sq.sq_repeat, v, w);
    if (tw->slots.sq.sq_repeat)
        return qp_abs_sequence_repeat(ctx, tw->slots.sq.sq_repeat, w, v);
    return qp_abs_binop_type_error(ctx, v, w, "*");
}

/* binary_iop1: v.__iop__ first, then the binary operation. */
static JSValue qp_abs_binary_iop1(JSContext *ctx, JSValueConst v, JSValueConst w,
                                  int ioff, int off)
{
    QPType *tv = qp_type_of(ctx, v);
    QPBinaryFunc *slot = QP_NB_BINOP(tv, ioff);
    if (slot) {
        JSValue x = slot(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    return qp_abs_binary_op1(ctx, v, w, off);
}

static JSValue qp_abs_inplace_add(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    JSValue r = qp_abs_binary_iop1(ctx, v, w, QP_NB_OFF(nb_inplace_add), QP_NB_OFF(nb_add));
    QPType *tv;
    QPBinaryFunc *func;
    if (!qp_is_notimpl(ctx, r))
        return r;
    JS_FreeValue(ctx, r);
    tv = qp_type_of(ctx, v);
    func = tv->slots.sq.sq_inplace_concat;
    if (func == NULL)
        func = tv->slots.sq.sq_concat;
    if (func)
        return func(ctx, v, w);
    return qp_abs_binop_type_error(ctx, v, w, "+=");
}

static JSValue qp_abs_inplace_multiply(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    JSValue r = qp_abs_binary_iop1(ctx, v, w, QP_NB_OFF(nb_inplace_multiply),
                                   QP_NB_OFF(nb_multiply));
    QPType *tv, *tw;
    QPSsizeArgFunc *f;
    if (!qp_is_notimpl(ctx, r))
        return r;
    JS_FreeValue(ctx, r);
    tv = qp_type_of(ctx, v);
    tw = qp_type_of(ctx, w);
    if (qp_abs_has_sq(tv)) {
        f = tv->slots.sq.sq_inplace_repeat;
        if (f == NULL)
            f = tv->slots.sq.sq_repeat;
        if (f)
            return qp_abs_sequence_repeat(ctx, f, v, w);
    } else if (qp_abs_has_sq(tw)) {
        /* the right operand must not be mutated: no sq_inplace_repeat */
        if (tw->slots.sq.sq_repeat)
            return qp_abs_sequence_repeat(ctx, tw->slots.sq.sq_repeat, w, v);
    }
    return qp_abs_binop_type_error(ctx, v, w, "*=");
}

/* int32 fast paths of +, -, *, &, |, ^ (also for the in-place forms:
   ints are immutable).  Returns QP_NULL when not applicable. */
static JSValue qp_abs_int_fast(JSContext *ctx, int32_t x, int32_t y, int nb)
{
    switch (nb) {
    case QP_NB_ADD:
        return qp_long_from_int64(ctx, (int64_t)x + y);
    case QP_NB_SUBTRACT:
        return qp_long_from_int64(ctx, (int64_t)x - y);
    case QP_NB_MULTIPLY:
        return qp_long_from_int64(ctx, (int64_t)x * y);
    case QP_NB_AND:
        return JS_NewInt32(ctx, x & y);
    case QP_NB_OR:
        return JS_NewInt32(ctx, x | y);
    case QP_NB_XOR:
        return JS_NewInt32(ctx, x ^ y);
    default:
        return QP_NULL;
    }
}

static JSValue qp_binop(JSContext *ctx, JSValueConst a, JSValueConst b, int nb)
{
    const QPBinopInfo *info;
    JSValue r;
    int base;

    if (nb == QP_NB_SUBSCR)
        return qp_getitem(ctx, a, b);
    base = nb >= QP_NB_INPLACE_ADD ? nb - QP_NB_INPLACE_OFFSET : nb;
    if ((unsigned)base >= QP_NB_INPLACE_ADD)
        return qp_raise_system_error(ctx, "invalid binary operator %d", nb);
    if (JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_TAG(b) == JS_TAG_INT) {
        r = qp_abs_int_fast(ctx, JS_VALUE_GET_INT(a), JS_VALUE_GET_INT(b), base);
        if (!QP_IS_NULL(r))
            return r;
    }
    info = &qp_binop_info[base];
    if (nb < QP_NB_INPLACE_ADD) {
        switch (nb) {
        case QP_NB_ADD:
            if (qp_str_check_exact(a) && qp_str_check_exact(b))
                return qp_str_concat(ctx, a, b);
            return qp_abs_add(ctx, a, b);
        case QP_NB_MULTIPLY:
            return qp_abs_multiply(ctx, a, b);
        case QP_NB_POWER:
            return qp_abs_ternary_op(ctx, a, b, JS_UNDEFINED, info->slot, info->name);
        default:
            r = qp_abs_binary_op1(ctx, a, b, info->slot);
            if (qp_is_notimpl(ctx, r)) {
                JS_FreeValue(ctx, r);
                return qp_abs_binop_type_error(ctx, a, b, info->name);
            }
            return r;
        }
    }
    switch (base) {
    case QP_NB_ADD:
        if (qp_str_check_exact(a) && qp_str_check_exact(b))
            return qp_str_concat(ctx, a, b);
        return qp_abs_inplace_add(ctx, a, b);
    case QP_NB_MULTIPLY:
        return qp_abs_inplace_multiply(ctx, a, b);
    case QP_NB_POWER: {
        /* ternary_iop */
        QPTernaryFunc *slot = QP_NB_TERNOP(qp_type_of(ctx, a), info->islot);
        if (slot) {
            r = slot(ctx, a, b, JS_UNDEFINED);
            if (!qp_is_notimpl(ctx, r))
                return r;
            JS_FreeValue(ctx, r);
        }
        return qp_abs_ternary_op(ctx, a, b, JS_UNDEFINED, info->slot, info->iname);
    }
    default:
        r = qp_abs_binary_iop1(ctx, a, b, info->islot, info->slot);
        if (qp_is_notimpl(ctx, r)) {
            JS_FreeValue(ctx, r);
            return qp_abs_binop_type_error(ctx, a, b, info->iname);
        }
        return r;
    }
}

/* PyNumber_Power */
static JSValue qp_power(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst mod)
{
    return qp_abs_ternary_op(ctx, a, b, mod, QP_NB_OFF(nb_power), "** or pow()");
}

static JSValue qp_unop(JSContext *ctx, JSValueConst a, int unop)
{
    QPType *t = qp_type_of(ctx, a);
    QPUnaryFunc *f;
    const char *descr;

    switch (unop) {
    case QP_UNOP_NEG:
        f = t->slots.nb.nb_negative;
        descr = "unary -";
        break;
    case QP_UNOP_INVERT:
        f = t->slots.nb.nb_invert;
        descr = "unary ~";
        break;
    case QP_UNOP_POS:
        f = t->slots.nb.nb_positive;
        descr = "unary +";
        break;
    default:
        return qp_raise_system_error(ctx, "invalid unary operator %d", unop);
    }
    if (f)
        return f(ctx, a);
    {
        char buf[QP_ABS_NAMEBUF];
        return qp_raise_type_error(ctx, "bad operand type for %s: '%.200s'", descr,
                                   qp_obj_tpname(ctx, t, buf, sizeof(buf)));
    }
}

/* PyNumber_Index: exact int. */
static JSValue qp_index(JSContext *ctx, JSValueConst v)
{
    QPType *t;
    JSValue result;

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return JS_DupValue(ctx, v);
    case JS_TAG_BOOL:
        return JS_NewInt32(ctx, JS_VALUE_GET_BOOL(v) != 0);
    default:
        break;
    }
    if (qp_long_check(ctx, v))
        return qp_long_value(ctx, v);
    t = qp_type_of(ctx, v);
    if (t->slots.nb.nb_index == NULL)
        return qp_abs_type_error(ctx, "'%.200s' object cannot be interpreted as an integer", v);
    result = t->slots.nb.nb_index(ctx, v);
    if (JS_IsException(result) || qp_long_check_exact(result))
        return result;
    if (!qp_long_check(ctx, result)) {
        qp_raise_type_error(ctx, "%T.__index__() must return an int, not %T", v, result);
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    /* Issue #17576: strict int subclass result is deprecated */
    if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                "%T.__index__() must return an int, not %T.  The ability to return an "
                "instance of a strict subclass of int is deprecated, and may be removed "
                "in a future version of Python.", v, result) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    {
        JSValue plain = qp_long_value(ctx, result);
        JS_FreeValue(ctx, result);
        return plain;
    }
}

/* PyNumber_AsSsize_t: exc_type NULL clamps on overflow, else raises
   exc_type("cannot fit '%.200s' into an index-sized integer"). */
static int qp_as_ssize(JSContext *ctx, JSValueConst v, QPType *exc_type, Py_ssize_t *out)
{
    JSValue value;
    int64_t x;
    int overflow = 0;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    value = qp_index(ctx, v);
    if (JS_IsException(value))
        return -1;
    if (qp_long_as_int64_overflow(value, &x, &overflow) < 0) {
        JS_FreeValue(ctx, value);
        return -1;
    }
    if (!overflow && (x > PY_SSIZE_T_MAX || x < PY_SSIZE_T_MIN))
        overflow = x < 0 ? -1 : 1;
    if (overflow) {
        if (!exc_type) {
            *out = qp_long_sign(value) < 0 ? PY_SSIZE_T_MIN : PY_SSIZE_T_MAX;
        } else {
            char buf[QP_ABS_NAMEBUF];
            JS_FreeValue(ctx, value);
            qp_raise(ctx, exc_type, "cannot fit '%.200s' into an index-sized integer",
                     qp_obj_tpname(ctx, qp_type_of(ctx, v), buf, sizeof(buf)));
            return -1;
        }
    } else {
        *out = (Py_ssize_t)x;
    }
    JS_FreeValue(ctx, value);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Items, slices, length                                                     */
/* ------------------------------------------------------------------------ */

/* Negative index adjustment of PySequence_GetItem & co. */
static int qp_abs_adjust_index(JSContext *ctx, QPType *t, JSValueConst s, Py_ssize_t *pi)
{
    if (*pi < 0 && t->slots.sq.sq_length) {
        Py_ssize_t l = t->slots.sq.sq_length(ctx, s);
        if (l < 0)
            return -1;
        *pi += l;
    }
    return 0;
}

/* PySequence_GetItem */
static JSValue qp_abs_sequence_getitem(JSContext *ctx, JSValueConst s, Py_ssize_t i)
{
    QPType *t = qp_type_of(ctx, s);
    if (t->slots.sq.sq_item) {
        if (qp_abs_adjust_index(ctx, t, s, &i) < 0)
            return JS_EXCEPTION;
        return t->slots.sq.sq_item(ctx, s, i);
    }
    if (t->slots.mp.mp_subscript)
        return qp_abs_type_error(ctx, "%.200s is not a sequence", s);
    return qp_abs_type_error(ctx, "'%.200s' object does not support indexing", s);
}

static JSValue qp_getitem(JSContext *ctx, JSValueConst c, JSValueConst key)
{
    QPType *t = qp_type_of(ctx, c), *ct;

    if (t->slots.mp.mp_subscript)
        return t->slots.mp.mp_subscript(ctx, c, key);
    if (t->slots.sq.sq_item) {
        Py_ssize_t i;
        if (!qp_abs_index_check(ctx, key))
            return qp_abs_type_error(ctx, "sequence index must be integer, not '%.200s'", key);
        if (qp_as_ssize(ctx, key, QP_EXC(ctx, IndexError), &i) < 0)
            return JS_EXCEPTION;
        return qp_abs_sequence_getitem(ctx, c, i);
    }
    ct = qp_type_from_value(c);
    if (ct) {
        JSValue meth, result;
        char buf[QP_ABS_NAMEBUF];
        /* type[int] is special-cased; str[int] etc. go through
           __class_getitem__ */
        if (ct == QP_TYPE(ctx, type))
            return qp_generic_alias_new(ctx, c, key);
        if (qp_getattr_opt(ctx, c, QP_ATOM(ctx, __class_getitem__), &meth) < 0)
            return JS_EXCEPTION;
        if (!QP_IS_NULL(meth) && !JS_IsUndefined(meth) && !JS_IsNull(meth)) {
            result = qp_call1(ctx, meth, key);
            JS_FreeValue(ctx, meth);
            return result;
        }
        JS_FreeValue(ctx, meth);
        return qp_raise_type_error(ctx, "type '%.200s' is not subscriptable",
                                   qp_obj_tpname(ctx, ct, buf, sizeof(buf)));
    }
    return qp_abs_type_error(ctx, "'%.200s' object is not subscriptable", c);
}

/* PyObject_SetItem / PyObject_DelItem (v == QP_NULL deletes). */
static int qp_abs_ass_item(JSContext *ctx, JSValueConst c, JSValueConst key, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, c);
    BOOL del = QP_IS_NULL(v);

    if (t->slots.mp.mp_ass_subscript)
        return t->slots.mp.mp_ass_subscript(ctx, c, key, v);
    if (qp_abs_has_sq(t)) {
        if (qp_abs_index_check(ctx, key)) {
            Py_ssize_t i;
            if (qp_as_ssize(ctx, key, QP_EXC(ctx, IndexError), &i) < 0)
                return -1;
            /* PySequence_SetItem / PySequence_DelItem */
            if (t->slots.sq.sq_ass_item) {
                if (qp_abs_adjust_index(ctx, t, c, &i) < 0)
                    return -1;
                return t->slots.sq.sq_ass_item(ctx, c, i, v);
            }
            if (t->slots.mp.mp_ass_subscript) {
                qp_abs_type_error(ctx, "%.200s is not a sequence", c);
                return -1;
            }
            qp_abs_type_error(ctx, del ? "'%.200s' object doesn't support item deletion"
                                       : "'%.200s' object does not support item assignment", c);
            return -1;
        } else if (t->slots.sq.sq_ass_item) {
            qp_abs_type_error(ctx, "sequence index must be integer, not '%.200s'", key);
            return -1;
        }
    }
    qp_abs_type_error(ctx, del ? "'%.200s' object does not support item deletion"
                               : "'%.200s' object does not support item assignment", c);
    return -1;
}

static int qp_setitem(JSContext *ctx, JSValueConst c, JSValueConst key, JSValueConst v)
{
    return qp_abs_ass_item(ctx, c, key, v);
}

static int qp_delitem(JSContext *ctx, JSValueConst c, JSValueConst key)
{
    return qp_abs_ass_item(ctx, c, key, QP_NULL);
}

/* BINARY_SLICE: c[start:stop] */
static JSValue qp_getslice(JSContext *ctx, JSValueConst c, JSValueConst start, JSValueConst stop)
{
    JSValue slice = qp_slice_new(ctx, start, stop, JS_UNDEFINED), res;
    if (JS_IsException(slice))
        return slice;
    res = qp_getitem(ctx, c, slice);
    JS_FreeValue(ctx, slice);
    return res;
}

/* STORE_SLICE: c[start:stop] = v (v == QP_NULL deletes). */
static int qp_setslice(JSContext *ctx, JSValueConst c, JSValueConst start, JSValueConst stop,
                       JSValueConst v)
{
    JSValue slice = qp_slice_new(ctx, start, stop, JS_UNDEFINED);
    int res;
    if (JS_IsException(slice))
        return -1;
    res = qp_abs_ass_item(ctx, c, slice, v);
    JS_FreeValue(ctx, slice);
    return res;
}

/* PyObject_Size */
static Py_ssize_t qp_len(JSContext *ctx, JSValueConst v)
{
    QPType *t;

    if (qp_str_check_exact(v))
        return qp_str_len(ctx, v);
    t = qp_type_of(ctx, v);
    if (t->slots.sq.sq_length)
        return t->slots.sq.sq_length(ctx, v);
    if (t->slots.mp.mp_length)
        return t->slots.mp.mp_length(ctx, v);
    qp_abs_type_error(ctx, "object of type '%.200s' has no len()", v);
    return -1;
}

/* PyObject_LengthHint */
static Py_ssize_t qp_length_hint(JSContext *ctx, JSValueConst v, Py_ssize_t dflt)
{
    QPType *t = qp_type_of(ctx, v);
    JSValue hint, result, value;
    Py_ssize_t res;

    if (t->slots.sq.sq_length || t->slots.mp.mp_length) {
        res = qp_len(ctx, v);
        if (res >= 0)
            return res;
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            return -1;
        qp_obj_clear_exc(ctx);
    }
    hint = qp_lookup_special(ctx, v, QP_ATOM(ctx, __length_hint__));
    if (JS_IsException(hint))
        return -1;
    if (QP_IS_NULL(hint))
        return dflt;
    result = qp_call0(ctx, hint);
    JS_FreeValue(ctx, hint);
    if (JS_IsException(result)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            qp_obj_clear_exc(ctx);
            return dflt;
        }
        return -1;
    }
    if (qp_is_notimpl(ctx, result)) {
        JS_FreeValue(ctx, result);
        return dflt;
    }
    if (!qp_long_check(ctx, result)) {
        qp_raise_type_error(ctx, "%T.__length_hint__() must return an int, not %T", v, result);
        JS_FreeValue(ctx, result);
        return -1;
    }
    value = qp_long_value(ctx, result);
    JS_FreeValue(ctx, result);
    if (JS_IsException(value))
        return -1;
    if (qp_long_as_ssize(ctx, value, &res) < 0) {
        JS_FreeValue(ctx, value);
        return -1;
    }
    JS_FreeValue(ctx, value);
    if (res < 0) {
        qp_raise_value_error(ctx, "%T.__length_hint__() must return a non-negative int", v);
        return -1;
    }
    return res;
}

/* PySequence_Contains: sq_contains, else iteration with == (identity
   first). */
static int qp_contains(JSContext *ctx, JSValueConst c, JSValueConst key)
{
    QPType *t = qp_type_of(ctx, c);
    JSValue it, item;
    int cmp;

    if (t->slots.sq.sq_contains)
        return t->slots.sq.sq_contains(ctx, c, key);
    it = qp_iter(ctx, c);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            qp_obj_clear_exc(ctx);
            qp_abs_type_error(ctx, "argument of type '%.200s' is not a container or iterable", c);
        }
        return -1;
    }
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item)) {
            cmp = -1;
            break;
        }
        if (QP_IS_NULL(item)) {
            cmp = 0;
            break;
        }
        cmp = qp_richcompare_bool(ctx, item, key, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (cmp != 0)
            break;
    }
    JS_FreeValue(ctx, it);
    return cmp < 0 ? -1 : cmp;
}

/* ------------------------------------------------------------------------ */
/* Iteration                                                                 */
/* ------------------------------------------------------------------------ */

/* PySequence_Check */
static BOOL qp_abs_sequence_check(JSContext *ctx, JSValueConst o)
{
    if (qp_dict_check(ctx, o))
        return FALSE;
    return qp_type_of(ctx, o)->slots.sq.sq_item != NULL;
}

/* PyObject_GetIter */
static JSValue qp_iter(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    JSValue res;

    if (t->slots.tp_iter == NULL) {
        if (qp_abs_sequence_check(ctx, v))
            return qp_seqiter_new(ctx, v);
        return qp_abs_type_error(ctx, "'%.200s' object is not iterable", v);
    }
    res = t->slots.tp_iter(ctx, v);
    if (JS_IsException(res))
        return res;
    if (qp_type_of(ctx, res)->slots.tp_iternext == NULL) {
        qp_raise_type_error(ctx, "%T.__iter__() must return an iterator, not %T", v, res);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* iternext(): value, QP_NULL when exhausted (a StopIteration is
   consumed), JS_EXCEPTION on other errors. */
static JSValue qp_next(JSContext *ctx, JSValueConst it)
{
    QPType *t = qp_type_of(ctx, it);
    JSValue item;

    if (t->slots.tp_iternext == NULL)
        return qp_abs_type_error(ctx, "'%.200s' object is not an iterator", it);
    item = t->slots.tp_iternext(ctx, it);
    if (JS_IsException(item)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
            qp_obj_clear_exc(ctx);
            return QP_NULL;
        }
    }
    return item;
}

/* _PyEval_GetIter.  Types with tp_iteritem are iterated virtually: the
   iterable itself stays on the stack with index 0 (range only when its
   length fits an int32 index).  yield-from kinds pass coroutines
   through (or reject them in non-coroutine generators). */
static int qp_get_iter_virtual(JSContext *ctx, JSValueConst obj, int kind,
                               JSValue *piter, JSValue *pidx)
{
    QPType *t = qp_type_of(ctx, obj);
    JSValue it;

    *piter = QP_NULL;
    *pidx = QP_NULL;
    if (t->slots.tp_iteritem) {
        BOOL ok = TRUE;
        if (t == QP_TYPE(ctx, range)) {
            Py_ssize_t n = qp_len(ctx, obj);
            if (n < 0) {
                qp_obj_clear_exc(ctx);  /* OverflowError: use the iterator */
                ok = FALSE;
            } else if (n > INT32_MAX) {
                ok = FALSE;
            }
        }
        if (ok) {
            *piter = JS_DupValue(ctx, obj);
            *pidx = JS_NewInt32(ctx, 0);
            return 0;
        }
    }
    if (kind != QP_GETITER_NORMAL && qp_coro_check(obj)) {
        if (kind == QP_GETITER_YIELD_FROM_CORO_CHECK) {
            qp_raise_type_error(ctx, "cannot 'yield from' a coroutine object "
                                "in a non-coroutine generator");
            return -1;
        }
        *piter = JS_DupValue(ctx, obj);
        return 0;
    }
    it = qp_iter(ctx, obj);
    if (JS_IsException(it))
        return -1;
    *piter = it;
    return 0;
}

/* _PyForIter_VirtualIteratorNext: 1 item (*pitem new ref), 0 exhausted,
   -1 error.  *pidx is the JS int index of virtual iteration (advanced)
   or QP_NULL for a real iterator. */
static int qp_iter_next_virtual(JSContext *ctx, JSValueConst iter, JSValue *pidx, JSValue *pitem)
{
    JSValue item;

    if (JS_VALUE_GET_TAG(*pidx) == JS_TAG_INT) {
        int32_t i = JS_VALUE_GET_INT(*pidx);
        int r = qp_type_of(ctx, iter)->slots.tp_iteritem(ctx, iter, i, pitem);
        if (r > 0)
            *pidx = JS_NewInt32(ctx, i + 1);
        else
            *pitem = QP_NULL;
        return r;
    }
    item = qp_next(ctx, iter);
    if (JS_IsException(item)) {
        *pitem = QP_NULL;
        return -1;
    }
    *pitem = item;
    return QP_IS_NULL(item) ? 0 : 1;
}

/* ------------------------------------------------------------------------ */
/* isinstance / issubclass                                                   */
/* ------------------------------------------------------------------------ */

static BOOL qp_abs_is_union(JSContext *ctx, JSValueConst v)
{
    return JS_IsObject(v) && qp_type_of(ctx, v) == QP_TYPE(ctx, Union);
}

/* abstract_get_bases: new tuple, or QP_NULL (no __bases__ / not a tuple,
   or an error other than AttributeError: check JS_HasException). */
static JSValue qp_abs_get_bases(JSContext *ctx, JSValueConst cls, int *perr)
{
    JSValue bases;
    *perr = 0;
    if (qp_getattr_opt(ctx, cls, QP_ATOM(ctx, __bases__), &bases) < 0) {
        *perr = 1;
        return QP_NULL;
    }
    if (!QP_IS_NULL(bases) && !qp_tuple_check(ctx, bases)) {
        JS_FreeValue(ctx, bases);
        return QP_NULL;
    }
    return bases;
}

static int qp_abs_abstract_issubclass(JSContext *ctx, JSValueConst derived, JSValueConst cls)
{
    JSValue bases = QP_NULL, nb;
    JSValueConst cur = derived;
    Py_ssize_t i, n;
    int r = 0, err;

    for (;;) {
        if (qp_is(cur, cls)) {
            JS_FreeValue(ctx, bases);
            return 1;
        }
        /* keep the previous bases alive until cur is no longer used */
        nb = qp_abs_get_bases(ctx, cur, &err);
        JS_FreeValue(ctx, bases);
        bases = nb;
        if (QP_IS_NULL(bases))
            return err ? -1 : 0;
        n = QP_TUPLE_SIZE(bases);
        if (n == 0) {
            JS_FreeValue(ctx, bases);
            return 0;
        }
        if (n == 1) {
            cur = QP_TUPLE_ITEMS(bases)[0];
            continue;
        }
        break;
    }
    if (qp_obj_enter_recursive(ctx) < 0) {
        JS_FreeValue(ctx, bases);
        return -1;
    }
    for (i = 0; i < n; i++) {
        r = qp_abs_abstract_issubclass(ctx, QP_TUPLE_ITEMS(bases)[i], cls);
        if (r != 0)
            break;
    }
    JS_FreeValue(ctx, bases);
    return r;
}

/* check_class: 1 when cls has a tuple __bases__, else 0 with TypeError
   (or the lookup error) pending. */
static int qp_abs_check_class(JSContext *ctx, JSValueConst cls, const char *error)
{
    int err;
    JSValue bases = qp_abs_get_bases(ctx, cls, &err);
    if (QP_IS_NULL(bases)) {
        if (!err)
            qp_raise_type_error(ctx, "%s", error);
        return 0;
    }
    JS_FreeValue(ctx, bases);
    return 1;
}

/* object_isinstance (_PyObject_RealIsInstance) */
static int qp_object_real_isinstance(JSContext *ctx, JSValueConst inst, JSValueConst cls)
{
    QPType *t = qp_type_from_value(cls), *it;
    JSValue icls;
    int retval;

    if (t) {
        if (qp_typecheck(ctx, inst, t))
            return 1;
        retval = qp_getattr_opt(ctx, inst, QP_ATOM(ctx, __class__), &icls);
        if (retval < 0)
            return -1;
        if (retval == 0)
            return 0;
        it = qp_type_from_value(icls);
        retval = 0;
        if (it && it != qp_type_of(ctx, inst))
            retval = qp_type_is_subtype(it, t);
        JS_FreeValue(ctx, icls);
        return retval;
    }
    if (!qp_abs_check_class(ctx, cls,
                            "isinstance() arg 2 must be a type, a tuple of types, or a union"))
        return -1;
    retval = qp_getattr_opt(ctx, inst, QP_ATOM(ctx, __class__), &icls);
    if (retval <= 0)
        return retval;
    retval = qp_abs_abstract_issubclass(ctx, icls, cls);
    JS_FreeValue(ctx, icls);
    return retval;
}

/* object_recursive_isinstance */
static int qp_isinstance(JSContext *ctx, JSValueConst inst, JSValueConst cls)
{
    QPType *t = qp_type_from_value(cls);
    JSValue checker, res;
    int ok;

    if (t && qp_type_of(ctx, inst) == t)
        return 1;
    if (t && qp_type_of(ctx, cls) == QP_TYPE(ctx, type))
        return qp_object_real_isinstance(ctx, inst, cls);
    if (qp_abs_is_union(ctx, cls))
        cls = qp_union_args(cls);
    if (qp_tuple_check(ctx, cls)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(cls);
        int r = 0;
        if (qp_obj_enter_recursive(ctx) < 0)
            return -1;
        for (i = 0; i < n; i++) {
            r = qp_isinstance(ctx, inst, QP_TUPLE_ITEMS(cls)[i]);
            if (r != 0)
                break;
        }
        return r;
    }
    checker = qp_lookup_special(ctx, cls, QP_ATOM(ctx, __instancecheck__));
    if (JS_IsException(checker))
        return -1;
    if (!QP_IS_NULL(checker)) {
        if (qp_obj_enter_recursive(ctx) < 0) {
            JS_FreeValue(ctx, checker);
            return -1;
        }
        res = qp_call1(ctx, checker, inst);
        JS_FreeValue(ctx, checker);
        if (JS_IsException(res))
            return -1;
        ok = qp_bool(ctx, res);
        JS_FreeValue(ctx, res);
        return ok;
    }
    return qp_object_real_isinstance(ctx, inst, cls);
}

/* recursive_issubclass (_PyObject_RealIsSubclass) */
static int qp_object_real_issubclass(JSContext *ctx, JSValueConst derived, JSValueConst cls)
{
    QPType *tc = qp_type_from_value(cls), *td = qp_type_from_value(derived);

    if (tc && td)
        return qp_type_is_subtype(td, tc);
    if (!qp_abs_check_class(ctx, derived, "issubclass() arg 1 must be a class"))
        return -1;
    if (!qp_abs_is_union(ctx, cls) &&
        !qp_abs_check_class(ctx, cls,
                            "issubclass() arg 2 must be a class, a tuple of classes, or a union"))
        return -1;
    return qp_abs_abstract_issubclass(ctx, derived, cls);
}

/* object_issubclass */
static int qp_issubclass(JSContext *ctx, JSValueConst derived, JSValueConst cls)
{
    JSValue checker, res;
    int ok;

    if (qp_type_from_value(cls) && qp_type_of(ctx, cls) == QP_TYPE(ctx, type)) {
        if (qp_is(derived, cls))
            return 1;
        return qp_object_real_issubclass(ctx, derived, cls);
    }
    if (qp_abs_is_union(ctx, cls))
        cls = qp_union_args(cls);
    if (qp_tuple_check(ctx, cls)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(cls);
        int r = 0;
        if (qp_obj_enter_recursive(ctx) < 0)
            return -1;
        for (i = 0; i < n; i++) {
            r = qp_issubclass(ctx, derived, QP_TUPLE_ITEMS(cls)[i]);
            if (r != 0)
                break;
        }
        return r;
    }
    checker = qp_lookup_special(ctx, cls, QP_ATOM(ctx, __subclasscheck__));
    if (JS_IsException(checker))
        return -1;
    if (!QP_IS_NULL(checker)) {
        if (qp_obj_enter_recursive(ctx) < 0) {
            JS_FreeValue(ctx, checker);
            return -1;
        }
        res = qp_call1(ctx, checker, derived);
        JS_FreeValue(ctx, checker);
        if (JS_IsException(res))
            return -1;
        ok = qp_bool(ctx, res);
        JS_FreeValue(ctx, res);
        return ok;
    }
    /* can be reached when infinite recursion happens */
    return qp_object_real_issubclass(ctx, derived, cls);
}

/* ------------------------------------------------------------------------ */
/* format()                                                                  */
/* ------------------------------------------------------------------------ */

static JSValue qp_format(JSContext *ctx, JSValueConst obj, JSValueConst spec)
{
    JSValue meth, result, empty = JS_UNDEFINED;
    JSValueConst fs = spec;

    if (!QP_IS_NULL(spec) && !qp_str_check(ctx, spec)) {
        char buf[QP_ABS_NAMEBUF];
        return qp_raise_system_error(ctx, "Format specifier must be a string, not %.200s",
                                     qp_obj_tpname(ctx, qp_type_of(ctx, spec), buf, sizeof(buf)));
    }
    if (QP_IS_NULL(spec) || (qp_str_check_exact(spec) && qp_str_len(ctx, spec) == 0)) {
        if (qp_str_check_exact(obj))
            return JS_DupValue(ctx, obj);
        if (qp_long_check_exact(obj))
            return qp_str(ctx, obj);
    }
    if (QP_IS_NULL(spec)) {
        empty = JS_AtomToString(ctx, JS_ATOM_empty_string);
        fs = empty;
    }
    meth = qp_lookup_special(ctx, obj, QP_ATOM(ctx, __format__));
    if (JS_IsException(meth)) {
        JS_FreeValue(ctx, empty);
        return meth;
    }
    if (QP_IS_NULL(meth)) {
        char buf[QP_ABS_NAMEBUF];
        JS_FreeValue(ctx, empty);
        return qp_raise_type_error(ctx, "Type %.100s doesn't define __format__",
                                   qp_obj_tpname(ctx, qp_type_of(ctx, obj), buf, sizeof(buf)));
    }
    result = qp_call1(ctx, meth, fs);
    JS_FreeValue(ctx, meth);
    JS_FreeValue(ctx, empty);
    if (!JS_IsException(result) && !qp_str_check(ctx, result)) {
        qp_raise_type_error(ctx, "%T.__format__() must return a str, not %T", obj, result);
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* ------------------------------------------------------------------------ */
/* Structural pattern matching helpers                                       */
/* ------------------------------------------------------------------------ */

/* match_class_attr: subject.<name> with the duplicate check; 1 found,
   0 missing (no error), -1 error. */
static int qp_abs_match_class_attr(JSContext *ctx, JSValueConst subject, QPType *type,
                                   JSValueConst name, JSValueConst seen, JSValue *pattr)
{
    JSAtom atom;
    int r;

    *pattr = QP_NULL;
    if (!QP_IS_NULL(seen)) {
        r = qp_set_contains(ctx, seen, name);
        if (r == 0)
            r = qp_set_add(ctx, seen, name) < 0 ? -1 : 0;
        if (r != 0) {
            if (r > 0) {
                char buf[QP_ABS_NAMEBUF];
                qp_raise_type_error(ctx, "%s() got multiple sub-patterns for attribute %R",
                                    qp_obj_tpname(ctx, type, buf, sizeof(buf)), name);
            }
            return -1;
        }
    }
    atom = qp_str_to_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_getattr_opt(ctx, subject, atom, pattr);
    JS_FreeAtom(ctx, atom);
    return r;
}

/* _PyEval_MatchClass: tuple of extracted attributes, None when the
   pattern does not match, JS_EXCEPTION on error. */
static JSValue qp_match_class(JSContext *ctx, JSValueConst subject, JSValueConst cls,
                              int nargs, JSValueConst kwnames)
{
    QPType *type = qp_type_from_value(cls);
    Py_ssize_t nkwargs, nattrs, i, filled = 0;
    JSValue seen = QP_NULL, match_args = QP_NULL, *attrs = NULL, res;
    char buf[QP_ABS_NAMEBUF], buf2[QP_ABS_NAMEBUF];
    int r, failed_error = 0;

    if (!type)
        return qp_raise_type_error(ctx, "class pattern must refer to a class");
    r = qp_isinstance(ctx, subject, cls);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    nkwargs = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames);
    nattrs = nargs + nkwargs;
    if (!nattrs)
        return qp_tuple_new(ctx, 0);
    if (nargs > 0 && nattrs > 1) {
        seen = qp_set_new(ctx, QP_NULL);
        if (JS_IsException(seen))
            return seen;
    }
    attrs = js_mallocz(ctx, sizeof(JSValue) * nattrs);
    if (!attrs) {
        JS_FreeValue(ctx, seen);
        return JS_EXCEPTION;
    }
    if (nargs) {
        int match_self = 0;
        Py_ssize_t allowed;
        if (qp_getattr_opt(ctx, cls, QP_ATOM(ctx, __match_args__), &match_args) < 0)
            goto error;
        if (!QP_IS_NULL(match_args)) {
            if (!qp_tuple_check_exact(ctx, match_args)) {
                qp_raise_type_error(ctx, "%s.__match_args__ must be a tuple (got %s)",
                                    qp_obj_tpname(ctx, type, buf, sizeof(buf)),
                                    qp_obj_tpname(ctx, qp_type_of(ctx, match_args), buf2, sizeof(buf2)));
                goto error;
            }
            allowed = QP_TUPLE_SIZE(match_args);
        } else {
            /* _Py_TPFLAGS_MATCH_SELF only counts when __match_args__ is
               not defined */
            match_self = (type->flags & QP_TPFLAGS_MATCH_SELF) != 0;
            allowed = match_self ? 1 : 0;
        }
        if (allowed < nargs) {
            qp_raise_type_error(ctx, "%s() accepts %zd positional sub-pattern%s (%zd given)",
                                qp_obj_tpname(ctx, type, buf, sizeof(buf)),
                                allowed, allowed == 1 ? "" : "s", (Py_ssize_t)nargs);
            goto error;
        }
        if (match_self) {
            attrs[filled++] = JS_DupValue(ctx, subject);
        } else {
            for (i = 0; i < nargs; i++) {
                JSValueConst name = QP_TUPLE_ITEMS(match_args)[i];
                JSValue attr;
                if (!qp_str_check_exact(name)) {
                    qp_raise_type_error(ctx, "__match_args__ elements must be strings (got %s)",
                                        qp_obj_tpname(ctx, qp_type_of(ctx, name), buf, sizeof(buf)));
                    goto error;
                }
                r = qp_abs_match_class_attr(ctx, subject, type, name, seen, &attr);
                if (r < 0)
                    goto error;
                if (r == 0)
                    goto nomatch;
                attrs[filled++] = attr;
            }
        }
        JS_FreeValue(ctx, match_args);
        match_args = QP_NULL;
    }
    for (i = 0; i < nkwargs; i++) {
        JSValue attr;
        r = qp_abs_match_class_attr(ctx, subject, type, QP_TUPLE_ITEMS(kwnames)[i], seen, &attr);
        if (r < 0)
            goto error;
        if (r == 0)
            goto nomatch;
        attrs[filled++] = attr;
    }
    JS_FreeValue(ctx, seen);
    res = qp_tuple_new_steal(ctx, nattrs, attrs);
    js_free(ctx, attrs);
    return res;
 error:
    failed_error = 1;
 nomatch:
    JS_FreeValue(ctx, match_args);
    JS_FreeValue(ctx, seen);
    for (i = 0; i < filled; i++)
        JS_FreeValue(ctx, attrs[i]);
    js_free(ctx, attrs);
    return failed_error ? JS_EXCEPTION : JS_UNDEFINED;
}

/* _PyEval_MatchKeys: tuple of values for `keys` (exact tuple) using the
   two-argument map.get(key, dummy); None if a key is missing. */
static JSValue qp_match_keys(JSContext *ctx, JSValueConst subject, JSValueConst keys)
{
    Py_ssize_t nkeys = QP_TUPLE_SIZE(keys), i, filled = 0;
    JSValue meth = QP_NULL, self = QP_NULL, seen = QP_NULL, dummy = QP_NULL;
    JSValue *values = NULL, res = JS_EXCEPTION;
    int r;

    if (!nkeys)
        return qp_tuple_new(ctx, 0);
    if (qp_load_method(ctx, subject, QP_ATOM(ctx, get), &meth, &self) < 0)
        return JS_EXCEPTION;
    seen = qp_set_new(ctx, QP_NULL);
    if (JS_IsException(seen)) {
        seen = QP_NULL;
        goto done;
    }
    dummy = qp_call0(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, object)));
    if (JS_IsException(dummy)) {
        dummy = QP_NULL;
        goto done;
    }
    values = js_mallocz(ctx, sizeof(JSValue) * nkeys);
    if (!values)
        goto done;
    for (i = 0; i < nkeys; i++) {
        JSValueConst key = QP_TUPLE_ITEMS(keys)[i];
        JSValueConst args[4];
        JSValue value;
        r = qp_set_contains(ctx, seen, key);
        if (r == 0)
            r = qp_set_add(ctx, seen, key) < 0 ? -1 : 0;
        if (r != 0) {
            if (r > 0)
                qp_raise_value_error(ctx, "mapping pattern checks duplicate key (%R)", key);
            goto done;
        }
        args[0] = JS_UNDEFINED;     /* scratch slot */
        args[1] = self;
        args[2] = key;
        args[3] = dummy;
        if (!QP_IS_NULL(self))
            value = qp_vectorcall(ctx, meth, args + 1, 3 | QP_VECTORCALL_ARGUMENTS_OFFSET, QP_NULL);
        else
            value = qp_vectorcall(ctx, meth, args + 2, 2, QP_NULL);
        if (JS_IsException(value))
            goto done;
        if (qp_is(value, dummy)) {
            /* key not in map */
            JS_FreeValue(ctx, value);
            res = JS_UNDEFINED;
            goto done;
        }
        values[filled++] = value;
    }
    res = qp_tuple_new_steal(ctx, nkeys, values);
    filled = 0;
 done:
    for (i = 0; i < filled; i++)
        JS_FreeValue(ctx, values[i]);
    if (values)
        js_free(ctx, values);
    JS_FreeValue(ctx, meth);
    JS_FreeValue(ctx, self);
    JS_FreeValue(ctx, seen);
    JS_FreeValue(ctx, dummy);
    return res;
}
