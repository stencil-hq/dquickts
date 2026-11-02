/*
 * dquickts Python: the range type (unit e).
 *
 * Derived from CPython 3.15 Objects/rangeobject.c (PSF-2.0).
 *
 * start/stop/step/length are exact Python ints of any size.  Arithmetic on
 * them goes through the qp_rg_* helpers below: an int64 fast path with
 * overflow checks, falling back to the int type's number slots (qp_binop)
 * for big values.  The range iterators (range_iterator and
 * longrange_iterator) live with the other PY_ITER types in iterobject.c.
 */

/* ---- cross-file helpers of unit e (defined in sliceobject.c / iterobject.c) */
static int qp_slice_get_long_indices(JSContext *ctx, JSValueConst slice, JSValueConst length,
                                     JSValue *pstart, JSValue *pstop, JSValue *pstep);
static JSValue qp_rangeiter_new(JSContext *ctx, JSValueConst start, JSValueConst stop,
                                JSValueConst step, JSValueConst length, BOOL reverse);
static JSValue qp_seq_type_name(JSContext *ctx, JSValueConst v);
static BOOL qp_seq_index_check(JSContext *ctx, JSValueConst v);
static Py_ssize_t qp_seq_iter_search(JSContext *ctx, JSValueConst seq, JSValueConst obj, int op);
#define QP_ITERSEARCH_COUNT    1
#define QP_ITERSEARCH_INDEX    2
#define QP_ITERSEARCH_CONTAINS 3

typedef struct QPRange {
    JSValue start, stop, step, length;   /* exact ints, owned */
} QPRange;

static QPRange *qp_range_get(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_RANGE)
        return NULL;
    return p->u.opaque;
}

static void qp_range_finalizer(JSRuntime *rt, JSValue val)
{
    QPRange *r = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (r) {
        JS_FreeValueRT(rt, r->start);
        JS_FreeValueRT(rt, r->stop);
        JS_FreeValueRT(rt, r->step);
        JS_FreeValueRT(rt, r->length);
        js_free_rt(rt, r);
    }
}

static void qp_range_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPRange *r = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (r) {
        JS_MarkValue(rt, r->start, mark_func);
        JS_MarkValue(rt, r->stop, mark_func);
        JS_MarkValue(rt, r->step, mark_func);
        JS_MarkValue(rt, r->length, mark_func);
    }
}

/* ---- exact-int arithmetic helpers (shared with slice and iterators) ---- */

/* TRUE with *out if the exact int (or bool) v fits int64. */
static BOOL qp_rg_i64(JSValueConst v, int64_t *out)
{
    int overflow = 0;
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
        *out = JS_VALUE_GET_INT(v);
        return TRUE;
    case JS_TAG_BOOL:
        *out = JS_VALUE_GET_BOOL(v);
        return TRUE;
    default:
        if (qp_long_as_int64_overflow(v, out, &overflow) < 0)
            return FALSE;
        return !overflow;
    }
}

static JSValue qp_rg_from_i64(JSContext *ctx, int64_t v)
{
    if (v >= INT32_MIN && v <= INT32_MAX)
        return JS_NewInt32(ctx, (int32_t)v);
    return qp_long_from_int64(ctx, v);
}

static JSValue qp_rg_add(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    if (qp_rg_i64(a, &x) && qp_rg_i64(b, &y) && !__builtin_add_overflow(x, y, &r))
        return qp_rg_from_i64(ctx, r);
    return qp_binop(ctx, a, b, QP_NB_ADD);
}

static JSValue qp_rg_sub(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    if (qp_rg_i64(a, &x) && qp_rg_i64(b, &y) && !__builtin_sub_overflow(x, y, &r))
        return qp_rg_from_i64(ctx, r);
    return qp_binop(ctx, a, b, QP_NB_SUBTRACT);
}

static JSValue qp_rg_mul(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    if (qp_rg_i64(a, &x) && qp_rg_i64(b, &y) && !__builtin_mul_overflow(x, y, &r))
        return qp_rg_from_i64(ctx, r);
    return qp_binop(ctx, a, b, QP_NB_MULTIPLY);
}

/* a // b with Python flooring (b != 0). */
static JSValue qp_rg_floordiv(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, q;
    if (qp_rg_i64(a, &x) && qp_rg_i64(b, &y) && y != 0 && !(x == INT64_MIN && y == -1)) {
        q = x / y;
        if ((x % y) != 0 && ((x < 0) != (y < 0)))
            q--;
        return qp_rg_from_i64(ctx, q);
    }
    return qp_binop(ctx, a, b, QP_NB_FLOOR_DIVIDE);
}

/* a % b with Python sign rules (b != 0). */
static JSValue qp_rg_mod(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, m;
    if (qp_rg_i64(a, &x) && qp_rg_i64(b, &y) && y != 0 && !(x == INT64_MIN && y == -1)) {
        m = x % y;
        if (m != 0 && ((m < 0) != (y < 0)))
            m += y;
        return qp_rg_from_i64(ctx, m);
    }
    return qp_binop(ctx, a, b, QP_NB_REMAINDER);
}

static JSValue qp_rg_neg(JSContext *ctx, JSValueConst a)
{
    int64_t x;
    if (qp_rg_i64(a, &x) && x != INT64_MIN)
        return qp_rg_from_i64(ctx, -x);
    return qp_unop(ctx, a, QP_UNOP_NEG);
}

/* Comparison of exact ints/bools: -1/0/1 (cannot fail). */
static int qp_rg_cmp(JSValueConst a, JSValueConst b)
{
    int64_t x, y;
    if (JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_TAG(b) == JS_TAG_INT) {
        x = JS_VALUE_GET_INT(a);
        y = JS_VALUE_GET_INT(b);
        return (x > y) - (x < y);
    }
    if (JS_VALUE_GET_TAG(a) == JS_TAG_BOOL || JS_VALUE_GET_TAG(b) == JS_TAG_BOOL) {
        JSValueConst a1 = JS_VALUE_GET_TAG(a) == JS_TAG_BOOL ? JS_NewInt32(NULL, JS_VALUE_GET_BOOL(a)) : a;
        JSValueConst b1 = JS_VALUE_GET_TAG(b) == JS_TAG_BOOL ? JS_NewInt32(NULL, JS_VALUE_GET_BOOL(b)) : b;
        return qp_long_compare(a1, b1);
    }
    return qp_long_compare(a, b);
}

static int qp_rg_sign(JSValueConst a)
{
    switch (JS_VALUE_GET_TAG(a)) {
    case JS_TAG_INT:
        return (JS_VALUE_GET_INT(a) > 0) - (JS_VALUE_GET_INT(a) < 0);
    case JS_TAG_BOOL:
        return JS_VALUE_GET_BOOL(a) != 0;
    default:
        return qp_long_sign(a);
    }
}

static BOOL qp_rg_is_one(JSValueConst a)
{
    return JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_INT(a) == 1;
}

/* ---- construction -------------------------------------------------------- */

/* Number of items of range(lo, hi, step) for int64 bounds (step != 0); the
   result always fits uint64 (CPython get_len_of_range). */
static uint64_t qp_range_len_i64(int64_t lo, int64_t hi, int64_t step)
{
    if (step > 0 && lo < hi)
        return 1 + ((uint64_t)hi - 1 - (uint64_t)lo) / (uint64_t)step;
    else if (step < 0 && lo > hi)
        return 1 + ((uint64_t)lo - 1 - (uint64_t)hi) / (0 - (uint64_t)step);
    else
        return 0;
}

/* Length of range(start, stop, step) as an exact int (compute_range_length). */
static JSValue qp_range_compute_length(JSContext *ctx, JSValueConst start, JSValueConst stop,
                                       JSValueConst step)
{
    int64_t lstart, lstop, lstep;
    uint64_t ulen;
    JSValueConst lo, hi;
    JSValue nstep, diff, tmp, res;

    if (qp_rg_i64(start, &lstart) && qp_rg_i64(stop, &lstop) && qp_rg_i64(step, &lstep)) {
        ulen = qp_range_len_i64(lstart, lstop, lstep);
        if (ulen <= (uint64_t)INT64_MAX)
            return qp_rg_from_i64(ctx, (int64_t)ulen);
    }
    if (qp_rg_sign(step) > 0) {
        lo = start;
        hi = stop;
        nstep = JS_DupValue(ctx, step);
    } else {
        lo = stop;
        hi = start;
        nstep = qp_rg_neg(ctx, step);
        if (JS_IsException(nstep))
            return nstep;
    }
    if (qp_rg_cmp(lo, hi) >= 0) {
        JS_FreeValue(ctx, nstep);
        return JS_NewInt32(ctx, 0);
    }
    /* (hi - lo - 1) // step + 1 */
    diff = qp_rg_sub(ctx, hi, lo);
    if (JS_IsException(diff))
        goto fail;
    tmp = qp_rg_sub(ctx, diff, JS_NewInt32(ctx, 1));
    JS_FreeValue(ctx, diff);
    if (JS_IsException(tmp))
        goto fail;
    diff = qp_rg_floordiv(ctx, tmp, nstep);
    JS_FreeValue(ctx, tmp);
    if (JS_IsException(diff))
        goto fail;
    res = qp_rg_add(ctx, diff, JS_NewInt32(ctx, 1));
    JS_FreeValue(ctx, diff);
    JS_FreeValue(ctx, nstep);
    return res;
 fail:
    JS_FreeValue(ctx, nstep);
    return JS_EXCEPTION;
}

/* make_range_object: consumes start, stop and step (exact ints, step != 0). */
static JSValue qp_range_make(JSContext *ctx, JSValue start, JSValue stop, JSValue step)
{
    JSValue length, obj;
    QPRange *r;

    length = qp_range_compute_length(ctx, start, stop, step);
    if (JS_IsException(length))
        goto fail;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, range)), JS_CLASS_PY_RANGE);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, length);
        goto fail;
    }
    r = js_malloc(ctx, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, length);
        goto fail;
    }
    r->start = start;
    r->stop = stop;
    r->step = step;
    r->length = length;
    JS_VALUE_GET_OBJ(obj)->u.opaque = r;
    return obj;
 fail:
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, stop);
    JS_FreeValue(ctx, step);
    return JS_EXCEPTION;
}

/* validate_step: step QP_NULL means 1; "range() arg 3 must not be zero". */
static JSValue qp_range_validate_step(JSContext *ctx, JSValueConst step)
{
    JSValue v;
    if (QP_IS_NULL(step))
        return JS_NewInt32(ctx, 1);
    v = qp_index(ctx, step);
    if (JS_IsException(v))
        return v;
    if (qp_rg_sign(v) == 0) {
        JS_FreeValue(ctx, v);
        return qp_raise_value_error(ctx, "range() arg 3 must not be zero");
    }
    return v;
}

/* range_from_array */
static JSValue qp_range_from_array(JSContext *ctx, JSValueConst *args, Py_ssize_t nargs)
{
    JSValue start, stop, step;

    switch (nargs) {
    case 3:
    case 2:
        start = qp_index(ctx, args[0]);
        if (JS_IsException(start))
            return start;
        stop = qp_index(ctx, args[1]);
        if (JS_IsException(stop)) {
            JS_FreeValue(ctx, start);
            return stop;
        }
        step = qp_range_validate_step(ctx, nargs == 3 ? args[2] : QP_NULL);
        if (JS_IsException(step)) {
            JS_FreeValue(ctx, start);
            JS_FreeValue(ctx, stop);
            return step;
        }
        break;
    case 1:
        stop = qp_index(ctx, args[0]);
        if (JS_IsException(stop))
            return stop;
        start = JS_NewInt32(ctx, 0);
        step = JS_NewInt32(ctx, 1);
        break;
    case 0:
        return qp_raise_type_error(ctx, "range expected at least 1 argument, got 0");
    default:
        return qp_raise_type_error(ctx, "range expected at most 3 arguments, got %zd", nargs);
    }
    return qp_range_make(ctx, start, stop, step);
}

/* Public constructor: range(start, stop, step) with the builtin's argument
   rules; start QP_NULL means range(stop), step QP_NULL means 1. */
static JSValue qp_range_new(JSContext *ctx, JSValueConst start, JSValueConst stop,
                            JSValueConst step)
{
    JSValueConst args[3];
    if (QP_IS_NULL(start))
        return qp_range_from_array(ctx, &stop, 1);
    args[0] = start;
    args[1] = stop;
    args[2] = step;
    return qp_range_from_array(ctx, args, QP_IS_NULL(step) ? 2 : 3);
}

static JSValue qp_range_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    if (qp_arg_no_keywords(ctx, "range", kwnames) < 0)
        return JS_EXCEPTION;
    return qp_range_from_array(ctx, args, qp_nargs(nargsf));
}

static JSValue qp_range_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    if (qp_arg_no_keywords(ctx, "range", kwnames) < 0)
        return JS_EXCEPTION;
    return qp_range_from_array(ctx, args, qp_nargs(nargsf));
}

/* ---- sequence protocol --------------------------------------------------- */

static Py_ssize_t qp_range_length(JSContext *ctx, JSValueConst self)
{
    QPRange *r = qp_range_get(self);
    Py_ssize_t n;
    if (JS_VALUE_GET_TAG(r->length) == JS_TAG_INT)
        return JS_VALUE_GET_INT(r->length);
    if (qp_long_as_ssize(ctx, r->length, &n) < 0)
        return -1;
    return n;
}

/* start + i * step */
static JSValue qp_range_compute_item(JSContext *ctx, QPRange *r, JSValueConst i)
{
    JSValue incr, res;
    if (qp_rg_is_one(r->step))
        return qp_rg_add(ctx, r->start, i);
    incr = qp_rg_mul(ctx, i, r->step);
    if (JS_IsException(incr))
        return incr;
    res = qp_rg_add(ctx, r->start, incr);
    JS_FreeValue(ctx, incr);
    return res;
}

/* r[arg] for an exact int arg (negative indices count from the end). */
static JSValue qp_range_compute_range_item(JSContext *ctx, QPRange *r, JSValueConst arg)
{
    JSValue i, res;
    if (qp_rg_sign(arg) < 0) {
        i = qp_rg_add(ctx, r->length, arg);
        if (JS_IsException(i))
            return i;
    } else {
        i = JS_DupValue(ctx, arg);
    }
    if (qp_rg_sign(i) < 0 || qp_rg_cmp(i, r->length) >= 0) {
        JS_FreeValue(ctx, i);
        return qp_raise_index_error(ctx, "range object index out of range");
    }
    res = qp_range_compute_item(ctx, r, i);
    JS_FreeValue(ctx, i);
    return res;
}

static JSValue qp_range_sq_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    QPRange *r = qp_range_get(self);
    JSValue arg, res;
    arg = qp_rg_from_i64(ctx, i);
    if (JS_IsException(arg))
        return arg;
    res = qp_range_compute_range_item(ctx, r, arg);
    JS_FreeValue(ctx, arg);
    return res;
}

/* Virtual iteration (get_iter/for_iter): item `index` (>= 0). */
static int qp_range_iteritem(JSContext *ctx, JSValueConst self, Py_ssize_t index, JSValue *pitem)
{
    QPRange *r = qp_range_get(self);
    int64_t len, start, step, v;
    JSValue i;

    if (qp_rg_i64(r->length, &len) && qp_rg_i64(r->start, &start) && qp_rg_i64(r->step, &step)) {
        if (index >= len)
            return 0;
        if (!__builtin_mul_overflow((int64_t)index, step, &v) &&
            !__builtin_add_overflow(v, start, &v)) {
            *pitem = qp_rg_from_i64(ctx, v);
            return JS_IsException(*pitem) ? -1 : 1;
        }
    }
    i = qp_rg_from_i64(ctx, index);
    if (JS_IsException(i))
        return -1;
    if (qp_rg_cmp(i, r->length) >= 0) {
        JS_FreeValue(ctx, i);
        return 0;
    }
    *pitem = qp_range_compute_item(ctx, r, i);
    JS_FreeValue(ctx, i);
    return JS_IsException(*pitem) ? -1 : 1;
}

/* r[slice] -> range (compute_slice) */
static JSValue qp_range_compute_slice(JSContext *ctx, QPRange *r, JSValueConst slice)
{
    JSValue start, stop, step, substart = JS_UNDEFINED, substop = JS_UNDEFINED, substep;

    if (qp_slice_get_long_indices(ctx, slice, r->length, &start, &stop, &step) < 0)
        return JS_EXCEPTION;
    substep = qp_rg_mul(ctx, r->step, step);
    if (JS_IsException(substep))
        goto fail;
    substart = qp_range_compute_item(ctx, r, start);
    if (JS_IsException(substart))
        goto fail;
    substop = qp_range_compute_item(ctx, r, stop);
    if (JS_IsException(substop))
        goto fail;
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, stop);
    JS_FreeValue(ctx, step);
    return qp_range_make(ctx, substart, substop, substep);
 fail:
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, stop);
    JS_FreeValue(ctx, step);
    JS_FreeValue(ctx, substart);
    JS_FreeValue(ctx, substop);
    JS_FreeValue(ctx, substep);
    return JS_EXCEPTION;
}

static JSValue qp_range_subscript(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    QPRange *r = qp_range_get(self);
    JSValue i, res, name;

    if (qp_seq_index_check(ctx, item)) {
        i = qp_index(ctx, item);
        if (JS_IsException(i))
            return i;
        res = qp_range_compute_range_item(ctx, r, i);
        JS_FreeValue(ctx, i);
        return res;
    }
    if (qp_slice_check(item))
        return qp_range_compute_slice(ctx, r, item);
    name = qp_seq_type_name(ctx, item);
    if (JS_IsException(name))
        return name;
    qp_raise_type_error(ctx, "range indices must be integers or slices, not %U", name);
    JS_FreeValue(ctx, name);
    return JS_EXCEPTION;
}

/* ob (exact int or bool) in r, without iterating (range_contains_long). */
static int qp_range_contains_long(JSContext *ctx, QPRange *r, JSValueConst ob0)
{
    JSValueConst ob;
    JSValue tmp1, tmp2;
    int res;

    ob = JS_VALUE_GET_TAG(ob0) == JS_TAG_BOOL ? JS_NewInt32(ctx, JS_VALUE_GET_BOOL(ob0)) : ob0;
    if (qp_rg_sign(r->step) > 0) {
        /* positive steps: start <= ob < stop */
        if (!(qp_rg_cmp(r->start, ob) <= 0 && qp_rg_cmp(ob, r->stop) < 0))
            return 0;
    } else {
        /* negative steps: stop < ob <= start */
        if (!(qp_rg_cmp(ob, r->start) <= 0 && qp_rg_cmp(r->stop, ob) < 0))
            return 0;
    }
    /* ((ob - start) % step) == 0 */
    tmp1 = qp_rg_sub(ctx, ob, r->start);
    if (JS_IsException(tmp1))
        return -1;
    tmp2 = qp_rg_mod(ctx, tmp1, r->step);
    JS_FreeValue(ctx, tmp1);
    if (JS_IsException(tmp2))
        return -1;
    res = qp_rg_sign(tmp2) == 0;
    JS_FreeValue(ctx, tmp2);
    return res;
}

static BOOL qp_range_is_long(JSValueConst ob)
{
    int tag = JS_VALUE_GET_TAG(ob);
    return tag == JS_TAG_INT || tag == JS_TAG_BOOL || tag == JS_TAG_SHORT_BIG_INT ||
        tag == JS_TAG_BIG_INT;
}

static int qp_range_contains(JSContext *ctx, JSValueConst self, JSValueConst ob)
{
    QPRange *r = qp_range_get(self);
    if (qp_range_is_long(ob))
        return qp_range_contains_long(ctx, r, ob);
    return (int)qp_seq_iter_search(ctx, self, ob, QP_ITERSEARCH_CONTAINS);
}

static int qp_range_bool(JSContext *ctx, JSValueConst self)
{
    return qp_rg_sign(qp_range_get(self)->length) != 0;
}

/* ---- comparison, hash, repr --------------------------------------------- */

/* Ranges compare as sequences: equal lengths, then (if non-empty) equal
   starts, then (if longer than 1) equal steps. */
static int qp_range_equals(QPRange *r0, QPRange *r1)
{
    if (r0 == r1)
        return 1;
    if (qp_rg_cmp(r0->length, r1->length) != 0)
        return 0;
    if (qp_rg_sign(r0->length) == 0)
        return 1;
    if (qp_rg_cmp(r0->start, r1->start) != 0)
        return 0;
    if (qp_rg_is_one(r0->length))
        return 1;
    return qp_rg_cmp(r0->step, r1->step) == 0;
}

static JSValue qp_range_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other, int op)
{
    QPRange *r0 = qp_range_get(self), *r1 = qp_range_get(other);
    int res;

    if (!r0 || !r1)
        return qp_not_implemented(ctx);
    switch (op) {
    case QP_CMP_EQ:
    case QP_CMP_NE:
        res = qp_range_equals(r0, r1);
        if (op == QP_CMP_NE)
            res = !res;
        return JS_NewBool(ctx, res);
    default:
        return qp_not_implemented(ctx);
    }
}

/* hash((len, None, None)) / hash((len, start, None)) / hash((len, start, step)) */
static Py_hash_t qp_range_hash(JSContext *ctx, JSValueConst self)
{
    QPRange *r = qp_range_get(self);
    JSValueConst items[3];
    JSValue t;
    Py_hash_t h;

    items[0] = r->length;
    if (qp_rg_sign(r->length) == 0) {
        items[1] = JS_UNDEFINED;
        items[2] = JS_UNDEFINED;
    } else {
        items[1] = r->start;
        items[2] = qp_rg_is_one(r->length) ? JS_UNDEFINED : r->step;
    }
    t = qp_tuple_pack(ctx, 3, items);
    if (JS_IsException(t))
        return -1;
    h = qp_hash(ctx, t);
    JS_FreeValue(ctx, t);
    return h;
}

static JSValue qp_range_repr(JSContext *ctx, JSValueConst self)
{
    QPRange *r = qp_range_get(self);
    if (qp_rg_is_one(r->step))
        return qp_str_from_format(ctx, "range(%R, %R)", r->start, r->stop);
    return qp_str_from_format(ctx, "range(%R, %R, %R)", r->start, r->stop, r->step);
}

static JSValue qp_range_iter(JSContext *ctx, JSValueConst self)
{
    QPRange *r = qp_range_get(self);
    return qp_rangeiter_new(ctx, r->start, r->stop, r->step, r->length, FALSE);
}

/* ---- methods ------------------------------------------------------------- */

static JSValue qp_range_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPRange *r = qp_range_get(self);
    return qp_rangeiter_new(ctx, r->start, r->stop, r->step, r->length, TRUE);
}

static JSValue qp_range_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPRange *r = qp_range_get(self);
    JSValueConst items[3] = { r->start, r->stop, r->step };
    JSValue a, res;
    JSValueConst pair[2];

    a = qp_tuple_pack(ctx, 3, items);
    if (JS_IsException(a))
        return a;
    pair[0] = QP_TYPE_VALUE(qp_type_of(ctx, self));
    pair[1] = a;
    res = qp_tuple_pack(ctx, 2, pair);
    JS_FreeValue(ctx, a);
    return res;
}

static JSValue qp_range_m_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPRange *r = qp_range_get(self);
    Py_ssize_t n;
    if (qp_range_is_long(args[0])) {
        int res = qp_range_contains_long(ctx, r, args[0]);
        if (res < 0)
            return JS_EXCEPTION;
        return JS_NewInt32(ctx, res);
    }
    n = qp_seq_iter_search(ctx, self, args[0], QP_ITERSEARCH_COUNT);
    if (n < 0)
        return JS_EXCEPTION;
    return qp_rg_from_i64(ctx, n);
}

static JSValue qp_range_m_index(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPRange *r = qp_range_get(self);
    JSValueConst ob = args[0];
    JSValue idx, sidx;
    Py_ssize_t n;
    int contains;

    if (!qp_range_is_long(ob)) {
        n = qp_seq_iter_search(ctx, self, ob, QP_ITERSEARCH_INDEX);
        if (n < 0)
            return JS_EXCEPTION;
        return qp_rg_from_i64(ctx, n);
    }
    contains = qp_range_contains_long(ctx, r, ob);
    if (contains < 0)
        return JS_EXCEPTION;
    if (!contains)
        return qp_raise_value_error(ctx, "range.index(x): x not in range");
    idx = qp_rg_sub(ctx, ob, r->start);
    if (JS_IsException(idx) || qp_rg_is_one(r->step))
        return idx;
    sidx = qp_rg_floordiv(ctx, idx, r->step);
    JS_FreeValue(ctx, idx);
    return sidx;
}

/* start / stop / step: read-only members (Py_READONLY). */
static JSValue qp_range_get_member(JSContext *ctx, JSValueConst self, int magic)
{
    QPRange *r = qp_range_get(self);
    switch (magic) {
    case 0: return JS_DupValue(ctx, r->start);
    case 1: return JS_DupValue(ctx, r->stop);
    default: return JS_DupValue(ctx, r->step);
    }
}

static int qp_range_set_member(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    qp_raise(ctx, QP_EXC(ctx, AttributeError), "readonly attribute");
    return -1;
}

static const QPMethodDef qp_range_methods[] = {
    { "__reversed__", qp_range_m_reversed, QP_METH_NOARGS, 0, "Return a reverse iterator." },
    { "__reduce__", qp_range_m_reduce, QP_METH_NOARGS, 0, NULL },
    { "count", qp_range_m_count, QP_METH_O, 0,
      "rangeobject.count(value) -> integer -- return number of occurrences of value" },
    { "index", qp_range_m_index, QP_METH_O, 0,
      "rangeobject.index(value) -> integer -- return index of value.\n"
      "Raise ValueError if the value is not present." },
    { NULL }
};

static const QPGetSetDef qp_range_getsets[] = {
    { "start", qp_range_get_member, qp_range_set_member, 0, NULL },
    { "stop", qp_range_get_member, qp_range_set_member, 1, NULL },
    { "step", qp_range_get_member, qp_range_set_member, 2, NULL },
    { NULL }
};

static const QPTypeSpec qp_range_spec = {
    .name = "range",
    .doc = "range(stop) -> range object\n"
           "range(start, stop[, step]) -> range object\n\n"
           "Return an object that produces a sequence of integers from start (inclusive)\n"
           "to stop (exclusive) by step.  range(i, j) produces i, i+1, i+2, ..., j-1.\n"
           "start defaults to 0, and stop is omitted!  range(4) produces 0, 1, 2, 3.\n"
           "These are exactly the valid indices for a list of 4 elements.\n"
           "When step is given, it specifies the increment (or decrement).",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_RANGE,
    .slots = {
        .tp_repr = qp_range_repr,
        .tp_hash = qp_range_hash,
        .tp_richcompare = qp_range_richcompare,
        .tp_iter = qp_range_iter,
        .tp_iteritem = qp_range_iteritem,
        .tp_new = qp_range_tp_new,
        .tp_vectorcall = qp_range_vectorcall,
        .nb = { .nb_bool = qp_range_bool },
        .sq = {
            .sq_length = qp_range_length,
            .sq_item = qp_range_sq_item,
            .sq_contains = qp_range_contains,
        },
        .mp = {
            .mp_length = qp_range_length,
            .mp_subscript = qp_range_subscript,
        },
    },
    .methods = qp_range_methods,
    .getsets = qp_range_getsets,
};

/* Called by qp_init_sequences (iterobject.c). */
static int qp_init_range(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, range), &qp_range_spec);
}
