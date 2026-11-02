/*
 * dquickts Python: slice and ellipsis types, slice index helpers (unit e).
 *
 * Derived from CPython 3.15 Objects/sliceobject.c and the slice-index
 * helpers of Python/ceval.c (_PyEval_SliceIndex) (PSF-2.0).
 */

/* ---- cross-file helpers of unit e (rangeobject.c / iterobject.c) -------- */
static BOOL qp_rg_i64(JSValueConst v, int64_t *out);
static JSValue qp_rg_add(JSContext *ctx, JSValueConst a, JSValueConst b);
static int qp_rg_cmp(JSValueConst a, JSValueConst b);
static int qp_rg_sign(JSValueConst a);
static BOOL qp_seq_index_check(JSContext *ctx, JSValueConst v);

typedef struct QPSlice {
    JSValue start, stop, step;   /* any objects, None when absent; owned */
} QPSlice;

static QPSlice *qp_slice_get(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_SLICE)
        return NULL;
    return p->u.opaque;
}

static BOOL qp_slice_check(JSValueConst v)
{
    return qp_slice_get(v) != NULL;
}

static void qp_slice_finalizer(JSRuntime *rt, JSValue val)
{
    QPSlice *s = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (s) {
        JS_FreeValueRT(rt, s->start);
        JS_FreeValueRT(rt, s->stop);
        JS_FreeValueRT(rt, s->step);
        js_free_rt(rt, s);
    }
}

static void qp_slice_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPSlice *s = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (s) {
        JS_MarkValue(rt, s->start, mark_func);
        JS_MarkValue(rt, s->stop, mark_func);
        JS_MarkValue(rt, s->step, mark_func);
    }
}

/* PySlice_New: QP_NULL arguments mean None. */
static JSValue qp_slice_new(JSContext *ctx, JSValueConst start, JSValueConst stop, JSValueConst step)
{
    JSValue obj;
    QPSlice *s;

    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, slice)), JS_CLASS_PY_SLICE);
    if (JS_IsException(obj))
        return obj;
    s = js_malloc(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->start = QP_IS_NULL(start) ? JS_UNDEFINED : JS_DupValue(ctx, start);
    s->stop = QP_IS_NULL(stop) ? JS_UNDEFINED : JS_DupValue(ctx, stop);
    s->step = QP_IS_NULL(step) ? JS_UNDEFINED : JS_DupValue(ctx, step);
    JS_VALUE_GET_OBJ(obj)->u.opaque = s;
    return obj;
}

static BOOL qp_slice_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

/* _PyEval_SliceIndexNotNone: int or __index__, clamped to Py_ssize_t.
   0 or -1. */
static int qp_slice_index(JSContext *ctx, JSValueConst v, Py_ssize_t *pi)
{
    int64_t x;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *pi = JS_VALUE_GET_INT(v);
        return 0;
    }
    if ((JS_VALUE_GET_TAG(v) == JS_TAG_SHORT_BIG_INT || JS_VALUE_GET_TAG(v) == JS_TAG_BOOL) &&
        qp_rg_i64(v, &x)) {
        *pi = (Py_ssize_t)x;
        return 0;
    }
    if (qp_seq_index_check(ctx, v))
        return qp_as_ssize(ctx, v, NULL, pi);
    qp_raise_type_error(ctx, "slice indices must be integers or have an __index__ method");
    return -1;
}

/* PySlice_Unpack */
static int qp_slice_unpack(JSContext *ctx, JSValueConst slice, Py_ssize_t *start,
                           Py_ssize_t *stop, Py_ssize_t *step)
{
    QPSlice *r = qp_slice_get(slice);

    if (qp_slice_is_none(r->step)) {
        *step = 1;
    } else {
        if (qp_slice_index(ctx, r->step, step) < 0)
            return -1;
        if (*step == 0) {
            qp_raise_value_error(ctx, "slice step cannot be zero");
            return -1;
        }
        /* guard against "step = -step" overflow later on */
        if (*step < -PY_SSIZE_T_MAX)
            *step = -PY_SSIZE_T_MAX;
    }
    if (qp_slice_is_none(r->start)) {
        *start = *step < 0 ? PY_SSIZE_T_MAX : 0;
    } else {
        if (qp_slice_index(ctx, r->start, start) < 0)
            return -1;
    }
    if (qp_slice_is_none(r->stop)) {
        *stop = *step < 0 ? PY_SSIZE_T_MIN : PY_SSIZE_T_MAX;
    } else {
        if (qp_slice_index(ctx, r->stop, stop) < 0)
            return -1;
    }
    return 0;
}

/* PySlice_AdjustIndices: clamp to [0, length] and return the slice length. */
static Py_ssize_t qp_slice_adjust_indices(Py_ssize_t length, Py_ssize_t *start,
                                          Py_ssize_t *stop, Py_ssize_t step)
{
    if (*start < 0) {
        *start += length;
        if (*start < 0)
            *start = (step < 0) ? -1 : 0;
    } else if (*start >= length) {
        *start = (step < 0) ? length - 1 : length;
    }
    if (*stop < 0) {
        *stop += length;
        if (*stop < 0)
            *stop = (step < 0) ? -1 : 0;
    } else if (*stop >= length) {
        *stop = (step < 0) ? length - 1 : length;
    }
    if (step < 0) {
        if (*stop < *start)
            return (*start - *stop - 1) / (-step) + 1;
    } else {
        if (*start < *stop)
            return (*stop - *start - 1) / step + 1;
    }
    return 0;
}

/* evaluate_slice_index: exact int via __index__ (new ref). */
static JSValue qp_slice_evaluate_index(JSContext *ctx, JSValueConst v)
{
    if (qp_seq_index_check(ctx, v))
        return qp_index(ctx, v);
    return qp_raise_type_error(ctx, "slice indices must be integers or None or have an "
                               "__index__ method");
}

/* Clamp *pv (owned exact int) into [lower, upper] the way
   _PySlice_GetLongIndices does for start and stop. */
static int qp_slice_clamp_long(JSContext *ctx, JSValue *pv, JSValueConst length,
                               JSValueConst lower, JSValueConst upper)
{
    JSValue v = *pv, tmp;
    if (qp_rg_sign(v) < 0) {
        tmp = qp_rg_add(ctx, v, length);
        JS_FreeValue(ctx, v);
        *pv = tmp;
        if (JS_IsException(tmp))
            return -1;
        if (qp_rg_cmp(tmp, lower) < 0) {
            JS_FreeValue(ctx, tmp);
            *pv = JS_DupValue(ctx, lower);
        }
    } else if (qp_rg_cmp(v, upper) > 0) {
        JS_FreeValue(ctx, v);
        *pv = JS_DupValue(ctx, upper);
    }
    return 0;
}

/* _PySlice_GetLongIndices: start/stop/step of `slice` for a sequence of
   `length` (non-negative exact int) as exact ints (new refs).  Used by
   slice.indices() and range slicing.  0 or -1 (all outputs undefined). */
static int qp_slice_get_long_indices(JSContext *ctx, JSValueConst slice, JSValueConst length,
                                     JSValue *pstart, JSValue *pstop, JSValue *pstep)
{
    QPSlice *self = qp_slice_get(slice);
    JSValue start = JS_UNDEFINED, stop = JS_UNDEFINED, step = JS_UNDEFINED;
    JSValue lower = JS_UNDEFINED, upper = JS_UNDEFINED;
    int step_is_negative;

    /* Convert step to an integer; raise for zero step. */
    if (qp_slice_is_none(self->step)) {
        step = JS_NewInt32(ctx, 1);
        step_is_negative = 0;
    } else {
        step = qp_slice_evaluate_index(ctx, self->step);
        if (JS_IsException(step))
            goto error;
        if (qp_rg_sign(step) == 0) {
            qp_raise_value_error(ctx, "slice step cannot be zero");
            goto error;
        }
        step_is_negative = qp_rg_sign(step) < 0;
    }

    /* Find lower and upper bounds for start and stop. */
    if (step_is_negative) {
        lower = JS_NewInt32(ctx, -1);
        upper = qp_rg_add(ctx, length, lower);
        if (JS_IsException(upper))
            goto error;
    } else {
        lower = JS_NewInt32(ctx, 0);
        upper = JS_DupValue(ctx, length);
    }

    /* Compute start. */
    if (qp_slice_is_none(self->start)) {
        start = JS_DupValue(ctx, step_is_negative ? upper : lower);
    } else {
        start = qp_slice_evaluate_index(ctx, self->start);
        if (JS_IsException(start))
            goto error;
        if (qp_slice_clamp_long(ctx, &start, length, lower, upper) < 0)
            goto error;
    }

    /* Compute stop. */
    if (qp_slice_is_none(self->stop)) {
        stop = JS_DupValue(ctx, step_is_negative ? lower : upper);
    } else {
        stop = qp_slice_evaluate_index(ctx, self->stop);
        if (JS_IsException(stop))
            goto error;
        if (qp_slice_clamp_long(ctx, &stop, length, lower, upper) < 0)
            goto error;
    }

    *pstart = start;
    *pstop = stop;
    *pstep = step;
    JS_FreeValue(ctx, upper);
    JS_FreeValue(ctx, lower);
    return 0;
 error:
    *pstart = *pstop = *pstep = JS_UNDEFINED;
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, stop);
    JS_FreeValue(ctx, step);
    JS_FreeValue(ctx, upper);
    JS_FreeValue(ctx, lower);
    return -1;
}

/* ---- slice type ---------------------------------------------------------- */

static JSValue qp_slice_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);

    if (qp_arg_no_keywords(ctx, "slice", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "slice", (int)nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    /* slice(stop) swaps start and stop to mirror range() */
    if (nargs == 1)
        return qp_slice_new(ctx, QP_NULL, args[0], QP_NULL);
    return qp_slice_new(ctx, args[0], args[1], nargs == 3 ? args[2] : QP_NULL);
}

static JSValue qp_slice_repr(JSContext *ctx, JSValueConst self)
{
    QPSlice *s = qp_slice_get(self);
    return qp_str_from_format(ctx, "slice(%R, %R, %R)", s->start, s->stop, s->step);
}

/* Slices compare like (start, stop, step) tuples. */
static JSValue qp_slice_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPSlice *a = qp_slice_get(v), *b = qp_slice_get(w);
    JSValueConst items[3];
    JSValue t1, t2, res;

    if (!a || !b)
        return qp_not_implemented(ctx);
    if (a == b) {
        switch (op) {
        case QP_CMP_EQ:
        case QP_CMP_LE:
        case QP_CMP_GE:
            return JS_TRUE;
        default:
            return JS_FALSE;
        }
    }
    items[0] = a->start;
    items[1] = a->stop;
    items[2] = a->step;
    t1 = qp_tuple_pack(ctx, 3, items);
    if (JS_IsException(t1))
        return t1;
    items[0] = b->start;
    items[1] = b->stop;
    items[2] = b->step;
    t2 = qp_tuple_pack(ctx, 3, items);
    if (JS_IsException(t2)) {
        JS_FreeValue(ctx, t1);
        return t2;
    }
    res = qp_richcompare(ctx, t1, t2, op);
    JS_FreeValue(ctx, t1);
    JS_FreeValue(ctx, t2);
    return res;
}

/* xxHash-based combination as in CPython's slice_hash / tuplehash. */
#define QP_SLICE_XXPRIME_1 ((uint64_t)11400714785074694791ULL)
#define QP_SLICE_XXPRIME_2 ((uint64_t)14029467366897019727ULL)
#define QP_SLICE_XXPRIME_5 ((uint64_t)2870177450012600261ULL)
#define QP_SLICE_XXROTATE(x) (((x) << 31) | ((x) >> 33))

static Py_hash_t qp_slice_hash(JSContext *ctx, JSValueConst self)
{
    QPSlice *s = qp_slice_get(self);
    JSValueConst parts[3] = { s->start, s->stop, s->step };
    uint64_t acc = QP_SLICE_XXPRIME_5, lane;
    int i;

    for (i = 0; i < 3; i++) {
        Py_hash_t h = qp_hash(ctx, parts[i]);
        if (h == -1)
            return -1;
        lane = (uint64_t)h;
        acc += lane * QP_SLICE_XXPRIME_2;
        acc = QP_SLICE_XXROTATE(acc);
        acc *= QP_SLICE_XXPRIME_1;
    }
    if (acc == (uint64_t)-1)
        return 1546275796;
    return (Py_hash_t)acc;
}

/* slice.indices(len) -> (start, stop, stride) */
static JSValue qp_slice_m_indices(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue length, items[3];

    length = qp_index(ctx, args[0]);
    if (JS_IsException(length))
        return length;
    if (qp_rg_sign(length) < 0) {
        JS_FreeValue(ctx, length);
        return qp_raise_value_error(ctx, "length should not be negative");
    }
    if (qp_slice_get_long_indices(ctx, self, length, &items[0], &items[1], &items[2]) < 0) {
        JS_FreeValue(ctx, length);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, length);
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_slice_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPSlice *s = qp_slice_get(self);
    JSValueConst items[3] = { s->start, s->stop, s->step };
    JSValueConst pair[2];
    JSValue a, res;

    a = qp_tuple_pack(ctx, 3, items);
    if (JS_IsException(a))
        return a;
    pair[0] = QP_TYPE_VALUE(qp_type_of(ctx, self));
    pair[1] = a;
    res = qp_tuple_pack(ctx, 2, pair);
    JS_FreeValue(ctx, a);
    return res;
}

static JSValue qp_slice_m_class_getitem(JSContext *ctx, JSValueConst cls, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, cls, args[0]);
}

static JSValue qp_slice_get_member(JSContext *ctx, JSValueConst self, int magic)
{
    QPSlice *s = qp_slice_get(self);
    switch (magic) {
    case 0: return JS_DupValue(ctx, s->start);
    case 1: return JS_DupValue(ctx, s->stop);
    default: return JS_DupValue(ctx, s->step);
    }
}

/* Members are Py_READONLY: both assignment and deletion fail. */
static int qp_slice_set_member(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    qp_raise(ctx, QP_EXC(ctx, AttributeError), "readonly attribute");
    return -1;
}

static const QPMethodDef qp_slice_methods[] = {
    { "indices", qp_slice_m_indices, QP_METH_O, 0,
      "S.indices(len) -> (start, stop, stride)\n\n"
      "Assuming a sequence of length len, calculate the start and stop\n"
      "indices, and the stride length of the extended slice described by\n"
      "S. Out of bounds indices are clipped in a manner consistent with the\n"
      "handling of normal slices." },
    { "__reduce__", qp_slice_m_reduce, QP_METH_NOARGS, 0,
      "Return state information for pickling." },
    { "__class_getitem__", qp_slice_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "slices are generic over the types of their start, end, and step values" },
    { NULL }
};

static const QPGetSetDef qp_slice_getsets[] = {
    { "start", qp_slice_get_member, qp_slice_set_member, 0, NULL },
    { "stop", qp_slice_get_member, qp_slice_set_member, 1, NULL },
    { "step", qp_slice_get_member, qp_slice_set_member, 2, NULL },
    { NULL }
};

static const QPTypeSpec qp_slice_spec = {
    .name = "slice",
    .doc = "slice(stop)\nslice(start, stop[, step])\n\n"
           "Create a slice object.\n\n"
           "This is used for extended slicing (e.g. a[0:10:2]).",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_SLICE,
    .slots = {
        .tp_repr = qp_slice_repr,
        .tp_hash = qp_slice_hash,
        .tp_richcompare = qp_slice_richcompare,
        .tp_new = qp_slice_tp_new,
    },
    .methods = qp_slice_methods,
    .getsets = qp_slice_getsets,
};

/* ---- ellipsis ------------------------------------------------------------ */

static JSValue qp_ellipsis_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    if (qp_nargs(nargsf) != 0 ||
        (!QP_IS_NULL(kwnames) && !JS_IsUndefined(kwnames) && QP_TUPLE_SIZE(kwnames) != 0))
        return qp_raise_type_error(ctx, "EllipsisType takes no arguments");
    return JS_DupValue(ctx, QP_STATE(ctx)->ellipsis);
}

static JSValue qp_ellipsis_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_cstr(ctx, "Ellipsis");
}

static JSValue qp_ellipsis_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_str_from_cstr(ctx, "Ellipsis");
}

static const QPMethodDef qp_ellipsis_methods[] = {
    { "__reduce__", qp_ellipsis_m_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_ellipsis_spec = {
    .name = "ellipsis",
    .doc = "ellipsis()\n--\n\nThe type of the Ellipsis singleton.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .slots = {
        .tp_repr = qp_ellipsis_repr,
        .tp_new = qp_ellipsis_tp_new,
    },
    .methods = qp_ellipsis_methods,
};

/* Called by qp_init_sequences (iterobject.c): slice and ellipsis types and
   the Ellipsis singleton (QPState.ellipsis). */
static int qp_init_slice(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    JSValue e;

    if (qp_type_ready(ctx, QP_TYPE(ctx, slice), &qp_slice_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, ellipsis), &qp_ellipsis_spec) < 0)
        return -1;
    e = qp_object_new_instance(ctx, QP_TYPE(ctx, ellipsis), JS_CLASS_PY_OBJECT);
    if (JS_IsException(e))
        return -1;
    s->ellipsis = e;
    return 0;
}
