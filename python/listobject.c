/*
 * listobject.c - Python `list` over QuickJS fast arrays (object.h (e)).
 *
 * Derived from CPython Objects/listobject.c (PSF-2.0).
 *
 * A Python list is a JS_CLASS_ARRAY object (plain JS arrays are exact lists,
 * list subclass instances are arrays whose prototype is the subclass).  All
 * Python operations work on the fast-array storage p->u.array (values,
 * count, u1.size = capacity) and keep the JS `length` property
 * (p->prop[0]) equal to count.  Arrays that JS code left in another state
 * (sparse/slow arrays, or fast arrays whose length exceeds count) are first
 * normalized back to that canonical form by qp_lst_normalize(); holes read
 * as None (undefined).  Every function re-validates the storage after
 * anything that can run user code, since CPython's semantics for lists
 * mutated during an operation (repr, compare, sort, extend, ...) are kept.
 *
 * JS-only states with no CPython equivalent (non-extensible arrays,
 * read-only `length`, frozen elements) make the mutating operations raise
 * TypeError instead of breaking the JS invariants.
 */

/* iterobject.c */
static JSValue qp_list_iter_new(JSContext *ctx, JSValueConst list);
static JSValue qp_list_reviter_new(JSContext *ctx, JSValueConst list);

#define QP_LIST_MAX_SIZE INT32_MAX

#define QP_LST_OBJ(v) JS_VALUE_GET_OBJ(v)

/* ---------------------------------------------------------------------- */
/* Helpers shared with tupleobject.c                                       */
/* ---------------------------------------------------------------------- */

/* tp_name of type(v), as CPython's "%.200s" messages print it (heap types:
   __name__ only). */
static const char *qp_lst_tp_name(JSContext *ctx, JSValueConst v, char *buf, int size)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* _PyIndex_Check */
static BOOL qp_lst_index_check(JSContext *ctx, JSValueConst v)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return TRUE;
    case JS_TAG_OBJECT:
        return qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
    default:
        return FALSE;
    }
}

/* _PyEval_SliceIndexNotNone (Argument Clinic slice_index(accept={int})) */
static int qp_lst_slice_index(JSContext *ctx, JSValueConst v, Py_ssize_t *pi)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *pi = JS_VALUE_GET_INT(v);
        return 0;
    }
    if (!qp_lst_index_check(ctx, v)) {
        qp_raise_type_error(ctx, "slice indices must be integers or have an __index__ method");
        return -1;
    }
    return qp_as_ssize(ctx, v, NULL, pi) < 0 ? -1 : 0;
}

/* Py_RETURN_RICHCOMPARE(a, b, op) */
static JSValue qp_lst_compare_sizes(Py_ssize_t a, Py_ssize_t b, int op)
{
    BOOL r;
    switch (op) {
    case QP_CMP_LT: r = a < b; break;
    case QP_CMP_LE: r = a <= b; break;
    case QP_CMP_EQ: r = a == b; break;
    case QP_CMP_NE: r = a != b; break;
    case QP_CMP_GT: r = a > b; break;
    default:        r = a >= b; break;
    }
    return qp_bool_new(r);
}

/* ---------------------------------------------------------------------- */
/* Storage                                                                 */
/* ---------------------------------------------------------------------- */

static inline BOOL qp_lst_is_canonical(JSObject *p)
{
    JSValue len = p->prop[0].u.value;
    return p->fast_array && JS_VALUE_GET_TAG(len) == JS_TAG_INT &&
        (uint32_t)JS_VALUE_GET_INT(len) == p->u.array.count;
}

static inline void qp_lst_set_count(JSObject *p, uint32_t n)
{
    p->u.array.count = n;
    p->prop[0].u.value = JS_MKVAL(JS_TAG_INT, n);
}

static Py_ssize_t qp_list_size(JSValueConst list)
{
    JSValue len = QP_LST_OBJ(list)->prop[0].u.value;
    if (likely(JS_VALUE_GET_TAG(len) == JS_TAG_INT))
        return (uint32_t)JS_VALUE_GET_INT(len);
    if (JS_VALUE_GET_TAG(len) == JS_TAG_FLOAT64)
        return (Py_ssize_t)(uint32_t)JS_VALUE_GET_FLOAT64(len);
    return 0;
}

/* Turn any JS array into the canonical fast form (count == length). */
static no_inline int qp_lst_normalize(JSContext *ctx, JSObject *p)
{
    JSValue obj = JS_MKPTR(JS_TAG_OBJECT, p);
    Py_ssize_t n = qp_list_size(obj);
    JSValue *tab;
    uint32_t len, i;

    if (n > QP_LIST_MAX_SIZE) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    len = (uint32_t)n;
    if (p->fast_array) {
        /* length > count: trailing holes read as None */
        if (len > p->u.array.u1.size && expand_fast_array(ctx, p, len))
            return -1;
        for (i = p->u.array.count; i < len; i++)
            p->u.array.u.values[i] = JS_UNDEFINED;
        qp_lst_set_count(p, len);
        return 0;
    }
    /* slow array: read the elements, drop the index properties, then
       switch back to fast storage */
    if (!p->extensible) {
        qp_raise_type_error(ctx, "cannot use a non-extensible sparse JS array as a list");
        return -1;
    }
    tab = js_malloc(ctx, sizeof(JSValue) * (len ? len : 1));
    if (!tab)
        return -1;
    for (i = 0; i < len; i++) {
        tab[i] = JS_GetPropertyUint32(ctx, obj, i);
        if (JS_IsException(tab[i]))
            goto fail;
    }
    if (p->fast_array) {
        /* a getter cannot make it fast again, but stay safe */
        qp_raise_runtime_error(ctx, "JS array changed while converting it to a list");
        goto fail;
    }
    if (set_array_length(ctx, p, JS_MKVAL(JS_TAG_INT, 0), JS_PROP_THROW) < 0)
        goto fail;
    if (qp_list_size(obj) != 0) {
        qp_raise_type_error(ctx, "cannot use a sealed JS array as a list");
        goto fail;
    }
    p->fast_array = 1;
    p->u.array.u.values = NULL;
    p->u.array.count = 0;
    p->u.array.u1.size = 0;
    if (len > 0 && expand_fast_array(ctx, p, len)) {
        qp_lst_set_count(p, 0);
        goto fail;
    }
    if (len > 0)
        memcpy(p->u.array.u.values, tab, sizeof(JSValue) * len);
    qp_lst_set_count(p, len);
    js_free(ctx, tab);
    return 0;
 fail:
    while (i-- > 0)
        JS_FreeValue(ctx, tab[i]);
    js_free(ctx, tab);
    return -1;
}

/* Storage readable as values[0..count): 0 or -1. */
static inline int qp_lst_fast(JSContext *ctx, JSObject *p)
{
    if (likely(qp_lst_is_canonical(p)))
        return 0;
    return qp_lst_normalize(ctx, p);
}

/* Same, before a mutation. */
static int qp_lst_mfast(JSContext *ctx, JSObject *p)
{
    if (qp_lst_fast(ctx, p))
        return -1;
    if (unlikely(!(get_shape_prop(p->shape)[0].flags & JS_PROP_WRITABLE))) {
        qp_raise_type_error(ctx, "cannot modify a JS array with a read-only length");
        return -1;
    }
    return 0;
}

/* Capacity for n elements (n > count: the array grows). */
static int qp_lst_reserve(JSContext *ctx, JSObject *p, Py_ssize_t n)
{
    if (n > p->u.array.count && !p->extensible) {
        qp_raise_type_error(ctx, "cannot add elements to a non-extensible JS array");
        return -1;
    }
    if (n <= p->u.array.u1.size)
        return 0;
    if (n > QP_LIST_MAX_SIZE) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    return expand_fast_array(ctx, p, (uint32_t)n);
}

/* items[i] as a new reference: 1, 0 if i is out of range, -1 error. */
static int qp_lst_getref(JSContext *ctx, JSObject *p, Py_ssize_t i, JSValue *pv)
{
    if (qp_lst_fast(ctx, p))
        return -1;
    if (i < 0 || (size_t)i >= p->u.array.count)
        return 0;
    *pv = JS_DupValue(ctx, p->u.array.u.values[i]);
    return 1;
}

/* Append (consumes v). */
static int qp_lst_push(JSContext *ctx, JSObject *p, JSValue v)
{
    uint32_t n;
    if (qp_lst_mfast(ctx, p))
        goto fail;
    n = p->u.array.count;
    if (unlikely(n >= p->u.array.u1.size || !p->extensible)) {
        if (qp_lst_reserve(ctx, p, (Py_ssize_t)n + 1))
            goto fail;
    }
    p->u.array.u.values[n] = v;
    qp_lst_set_count(p, n + 1);
    return 0;
 fail:
    JS_FreeValue(ctx, v);
    return -1;
}

/* list_clear: empty the list and release its storage. */
static int qp_lst_clear(JSContext *ctx, JSObject *p)
{
    JSValue *items;
    uint32_t i;
    if (qp_lst_mfast(ctx, p))
        return -1;
    items = p->u.array.u.values;
    i = p->u.array.count;
    p->u.array.u.values = NULL;
    p->u.array.u1.size = 0;
    qp_lst_set_count(p, 0);
    /* the list is consistent before any value is released */
    while (i-- > 0)
        JS_FreeValue(ctx, items[i]);
    js_free(ctx, items);
    return 0;
}

static JSValue qp_lst_new_from(JSContext *ctx, const JSValue *src, Py_ssize_t n)
{
    JSValue np = JS_NewArray(ctx);
    JSObject *q;
    Py_ssize_t i;
    if (JS_IsException(np) || n <= 0)
        return np;
    q = QP_LST_OBJ(np);
    if (qp_lst_reserve(ctx, q, n)) {
        JS_FreeValue(ctx, np);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        q->u.array.u.values[i] = JS_DupValue(ctx, src[i]);
    qp_lst_set_count(q, n);
    return np;
}

/* list_slice: new exact list of items[ilow:ihigh] (p canonical, bounds
   valid). */
static JSValue qp_lst_slice(JSContext *ctx, JSObject *p, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    if (ihigh - ilow <= 0)
        return JS_NewArray(ctx);
    return qp_lst_new_from(ctx, p->u.array.u.values + ilow, ihigh - ilow);
}

/* ---------------------------------------------------------------------- */
/* Exported API (object.h (e))                                             */
/* ---------------------------------------------------------------------- */

static BOOL qp_list_check(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_ARRAY;
}

static BOOL qp_list_check_exact(JSContext *ctx, JSValueConst v)
{
    JSObject *p, *proto;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_ARRAY)
        return FALSE;
    proto = p->shape->proto;
    return !proto || proto->class_id != JS_CLASS_PY_TYPE ||
        proto == QP_TYPE(ctx, list)->obj;
}

static JSValue qp_list_new(JSContext *ctx, Py_ssize_t n)
{
    if (n > QP_LIST_MAX_SIZE)
        return qp_raise_memory_error(ctx);
    return js_allocate_fast_array(ctx, n < 0 ? 0 : n);
}

static JSValue qp_list_new_steal(JSContext *ctx, Py_ssize_t n, JSValue *items)
{
    JSValue np = JS_NewArray(ctx);
    JSObject *q;
    Py_ssize_t i;
    if (JS_IsException(np))
        goto fail;
    if (n > 0) {
        q = QP_LST_OBJ(np);
        if (qp_lst_reserve(ctx, q, n)) {
            JS_FreeValue(ctx, np);
            goto fail;
        }
        memcpy(q->u.array.u.values, items, sizeof(JSValue) * n);
        qp_lst_set_count(q, n);
    }
    return np;
 fail:
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, items[i]);
    return JS_EXCEPTION;
}

static JSValue *qp_list_items(JSValueConst list, Py_ssize_t *plen)
{
    JSObject *p = QP_LST_OBJ(list);
    if (plen)
        *plen = qp_list_size(list);
    if (!qp_lst_is_canonical(p))
        return NULL;
    return p->u.array.u.values;
}

static int qp_list_append(JSContext *ctx, JSValueConst list, JSValueConst v)
{
    return qp_lst_push(ctx, QP_LST_OBJ(list), JS_DupValue(ctx, v));
}

/* ins1 */
static int qp_list_insert(JSContext *ctx, JSValueConst list, Py_ssize_t where, JSValueConst v)
{
    JSObject *p = QP_LST_OBJ(list);
    JSValue *items;
    Py_ssize_t n;

    if (qp_lst_mfast(ctx, p))
        return -1;
    n = p->u.array.count;
    if (qp_lst_reserve(ctx, p, n + 1))
        return -1;
    if (where < 0) {
        where += n;
        if (where < 0)
            where = 0;
    }
    if (where > n)
        where = n;
    items = p->u.array.u.values;
    memmove(items + where + 1, items + where, sizeof(JSValue) * (n - where));
    items[where] = JS_DupValue(ctx, v);
    qp_lst_set_count(p, n + 1);
    return 0;
}

/* Inplace repeat of a list (list_inplace_repeat_lock_held). */
static int qp_lst_inplace_repeat_int(JSContext *ctx, JSObject *p, Py_ssize_t n)
{
    Py_ssize_t input_size, output_size, j, i;
    JSValue *items;

    if (qp_lst_mfast(ctx, p))
        return -1;
    input_size = p->u.array.count;
    if (input_size == 0 || n == 1)
        return 0;
    if (n < 1)
        return qp_lst_clear(ctx, p);
    if (input_size > QP_LIST_MAX_SIZE / n) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    output_size = input_size * n;
    if (qp_lst_reserve(ctx, p, output_size))
        return -1;
    items = p->u.array.u.values;
    for (j = 1; j < n; j++) {
        for (i = 0; i < input_size; i++)
            items[j * input_size + i] = JS_DupValue(ctx, items[i]);
    }
    qp_lst_set_count(p, output_size);
    return 0;
}

/* list_extend_fast over an exact list or tuple `seq` (not p itself). */
static int qp_lst_extend_fast(JSContext *ctx, JSObject *p, JSValueConst seq)
{
    const JSValue *src;
    Py_ssize_t n, m, i;
    JSValue *dest;

    if (qp_lst_mfast(ctx, p))
        return -1;
    if (JS_VALUE_GET_OBJ(seq)->class_id == JS_CLASS_ARRAY) {
        JSObject *q = JS_VALUE_GET_OBJ(seq);
        if (qp_lst_fast(ctx, q) || qp_lst_mfast(ctx, p))
            return -1;
        src = q->u.array.u.values;
        n = q->u.array.count;
    } else {
        src = QP_TUPLE_ITEMS(seq);
        n = QP_TUPLE_SIZE(seq);
    }
    if (n == 0)
        return 0;
    m = p->u.array.count;
    if (qp_lst_reserve(ctx, p, m + n))
        return -1;
    dest = p->u.array.u.values + m;
    for (i = 0; i < n; i++)
        dest[i] = JS_DupValue(ctx, src[i]);
    qp_lst_set_count(p, m + n);
    return 0;
}

/* list_extend_iter_lock_held */
static int qp_lst_extend_iter(JSContext *ctx, JSObject *p, JSValueConst iterable)
{
    JSValue it, item;
    Py_ssize_t n, m;

    it = qp_iter(ctx, iterable);
    if (JS_IsException(it))
        return -1;
    /* Guess a result list size. */
    n = qp_length_hint(ctx, iterable, 8);
    if (n < 0)
        goto error;
    if (qp_lst_mfast(ctx, p))
        goto error;
    m = p->u.array.count;
    /* a hint beyond the possible size is ignored, like an overflowing one */
    if (n > 0 && m <= QP_LIST_MAX_SIZE - n && p->extensible) {
        if (qp_lst_reserve(ctx, p, m + n))
            goto error;
    }
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item))
            goto error;
        if (QP_IS_NULL(item))
            break;
        if (qp_lst_push(ctx, p, item))
            goto error;
    }
    JS_FreeValue(ctx, it);
    return 0;
 error:
    JS_FreeValue(ctx, it);
    return -1;
}

/* _list_extend */
static int qp_list_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable)
{
    JSObject *p = QP_LST_OBJ(list);
    if (JS_VALUE_GET_TAG(iterable) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(iterable) == p)
        return qp_lst_inplace_repeat_int(ctx, p, 2);
    if (qp_list_check_exact(ctx, iterable) || qp_tuple_check_exact(ctx, iterable))
        return qp_lst_extend_fast(ctx, p, iterable);
    return qp_lst_extend_iter(ctx, p, iterable);
}

static JSValue qp_list_from_iterable(JSContext *ctx, JSValueConst iterable)
{
    JSValue list = JS_NewArray(ctx);
    if (JS_IsException(list))
        return list;
    if (qp_list_extend(ctx, list, iterable)) {
        JS_FreeValue(ctx, list);
        return JS_EXCEPTION;
    }
    return list;
}

static JSValue qp_list_as_tuple(JSContext *ctx, JSValueConst list)
{
    JSObject *p = QP_LST_OBJ(list);
    if (qp_lst_fast(ctx, p))
        return JS_EXCEPTION;
    return qp_tuple_pack(ctx, p->u.array.count, (JSValueConst *)p->u.array.u.values);
}

/* PySequence_Fast(v, msg): an exact list or tuple (new reference); lists
   are canonical on return. */
static JSValue qp_lst_sequence_fast(JSContext *ctx, JSValueConst v, const char *msg)
{
    JSValue it, res;
    if (qp_list_check_exact(ctx, v)) {
        if (qp_lst_fast(ctx, QP_LST_OBJ(v)))
            return JS_EXCEPTION;
        return JS_DupValue(ctx, v);
    }
    if (qp_tuple_check_exact(ctx, v))
        return JS_DupValue(ctx, v);
    it = qp_iter(ctx, v);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            qp_raise_type_error(ctx, "%s", msg);
        }
        return JS_EXCEPTION;
    }
    res = qp_list_from_iterable(ctx, it);
    JS_FreeValue(ctx, it);
    return res;
}

/* Items of a qp_lst_sequence_fast result (borrowed, valid until user code
   runs; *pitems may be NULL when *pn is 0): 0 or -1. */
static int qp_lst_seq_items(JSContext *ctx, JSValueConst seq, const JSValue **pitems,
                            Py_ssize_t *pn)
{
    JSObject *q = JS_VALUE_GET_OBJ(seq);
    if (q->class_id == JS_CLASS_ARRAY) {
        if (qp_lst_fast(ctx, q))
            return -1;
        *pn = q->u.array.count;
        *pitems = q->u.array.u.values;
        return 0;
    }
    *pn = QP_TUPLE_SIZE(seq);
    *pitems = QP_TUPLE_ITEMS(seq);
    return 0;
}

/* list_ass_slice: a[ilow:ihigh] = v (v == QP_NULL deletes). */
static int qp_lst_ass_slice(JSContext *ctx, JSValueConst self, Py_ssize_t ilow,
                            Py_ssize_t ihigh, JSValueConst v)
{
    JSObject *p = QP_LST_OBJ(self);
    JSValue recycle_on_stack[8];
    JSValue *recycle = recycle_on_stack;
    JSValue seq = QP_NULL, *item;
    const JSValue *vitem = NULL;
    Py_ssize_t n = 0, norig, d, k, size;
    int result = -1;

    if (!QP_IS_NULL(v)) {
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v) == p) {
            /* protect against a[i:j] = a */
            if (qp_lst_fast(ctx, p))
                return -1;
            seq = qp_lst_slice(ctx, p, 0, p->u.array.count);
        } else {
            seq = qp_lst_sequence_fast(ctx, v, "can only assign an iterable");
        }
        if (JS_IsException(seq))
            return -1;
    }
    if (qp_lst_mfast(ctx, p))
        goto done;
    if (!QP_IS_NULL(seq)) {
        if (qp_lst_seq_items(ctx, seq, &vitem, &n) || qp_lst_mfast(ctx, p))
            goto done;
    }
    size = p->u.array.count;
    if (ilow < 0)
        ilow = 0;
    else if (ilow > size)
        ilow = size;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > size)
        ihigh = size;

    norig = ihigh - ilow;
    d = n - norig;
    if (size + d == 0) {
        result = qp_lst_clear(ctx, p);
        goto done;
    }
    /* recycle the items that we are about to remove */
    if (norig > (Py_ssize_t)countof(recycle_on_stack)) {
        recycle = js_malloc(ctx, sizeof(JSValue) * norig);
        if (!recycle) {
            recycle = recycle_on_stack;
            goto done;
        }
    }
    if (d > 0 && qp_lst_reserve(ctx, p, size + d))
        goto done;
    item = p->u.array.u.values;
    if (norig)
        memcpy(recycle, &item[ilow], sizeof(JSValue) * norig);
    if (d != 0) /* delete -d items or make room for d items */
        memmove(&item[ihigh + d], &item[ihigh], sizeof(JSValue) * (size - ihigh));
    for (k = 0; k < n; k++, ilow++)
        item[ilow] = JS_DupValue(ctx, vitem[k]);
    qp_lst_set_count(p, size + d);
    for (k = norig - 1; k >= 0; --k)
        JS_FreeValue(ctx, recycle[k]);
    result = 0;
 done:
    if (recycle != recycle_on_stack)
        js_free(ctx, recycle);
    JS_FreeValue(ctx, seq);
    return result;
}

/* ---------------------------------------------------------------------- */
/* Sorting                                                                 */
/* ---------------------------------------------------------------------- */

static int qp_lst_sort_lt(JSContext *ctx, JSValueConst a, JSValueConst b, void *opaque)
{
    return qp_richcompare_bool(ctx, a, b, QP_CMP_LT);
}

/* list_sort_impl.  The storage is detached for the duration of the sort
   (the list looks empty to the key and comparison functions); a list
   found non-empty or with storage afterwards was modified. */
static int qp_list_sort(JSContext *ctx, JSValueConst list, JSValueConst key, int reverse)
{
    JSObject *p = QP_LST_OBJ(list);
    JSValue *saved, *keys = NULL, *final_items;
    uint32_t saved_n, saved_size, final_n, i;
    qp_sort_lt_fn *lt;
    int ret = 0;

    if (qp_lst_mfast(ctx, p))
        return -1;
    saved = p->u.array.u.values;
    saved_n = p->u.array.count;
    saved_size = p->u.array.u1.size;
    p->u.array.u.values = NULL;
    p->u.array.u1.size = 0;
    qp_lst_set_count(p, 0);

    if (!QP_IS_NULL(key) && saved_n > 0) {
        keys = js_malloc(ctx, sizeof(JSValue) * saved_n);
        if (!keys) {
            ret = -1;
            goto keyfunc_fail;
        }
        for (i = 0; i < saved_n; i++) {
            keys[i] = qp_call1(ctx, key, saved[i]);
            if (JS_IsException(keys[i])) {
                while (i-- > 0)
                    JS_FreeValue(ctx, keys[i]);
                js_free(ctx, keys);
                ret = -1;
                goto keyfunc_fail;
            }
        }
    }

    if (saved_n > 1) {
        /* Reverse sort stability achieved by initially reversing the list,
           applying a stable forward sort, then reversing the final result. */
        if (reverse) {
            if (keys)
                qp_sort_reverse(keys, saved_n);
            qp_sort_reverse(saved, saved_n);
        }
        lt = qp_sort_presort_check(keys ? keys : saved, saved_n);
        if (!lt)
            lt = qp_lst_sort_lt;
        ret = qp_timsort(ctx, keys ? keys : saved, keys ? saved : NULL, saved_n, lt, NULL);
    }
    if (keys) {
        for (i = 0; i < saved_n; i++)
            JS_FreeValue(ctx, keys[i]);
        js_free(ctx, keys);
    }
    if (ret == 0 && !(p->fast_array && p->u.array.u.values == NULL &&
                      p->u.array.count == 0 &&
                      JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT &&
                      JS_VALUE_GET_INT(p->prop[0].u.value) == 0)) {
        /* The user mucked with the list during the sort. */
        qp_raise_value_error(ctx, "list modified during sort");
        ret = -1;
    }
    if (reverse && saved_n > 1)
        qp_sort_reverse(saved, saved_n);

 keyfunc_fail:
    /* drop what the list holds now and put the sorted items back */
    if (!p->fast_array) {
        if (set_array_length(ctx, p, JS_MKVAL(JS_TAG_INT, 0), JS_PROP_THROW) < 0 ||
            qp_list_size(list) != 0) {
            if (!JS_HasException(ctx))
                qp_raise_type_error(ctx, "cannot use a sealed JS array as a list");
            for (i = 0; i < saved_n; i++)
                JS_FreeValue(ctx, saved[i]);
            js_free(ctx, saved);
            return -1;
        }
        p->fast_array = 1;
        p->u.array.u.values = NULL;
        p->u.array.count = 0;
        p->u.array.u1.size = 0;
    }
    final_items = p->u.array.u.values;
    final_n = p->u.array.count;
    p->u.array.u.values = saved;
    p->u.array.u1.size = saved_size;
    qp_lst_set_count(p, saved_n);
    if (final_items) {
        while (final_n-- > 0)
            JS_FreeValue(ctx, final_items[final_n]);
        js_free(ctx, final_items);
    }
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Slots                                                                   */
/* ---------------------------------------------------------------------- */

static Py_ssize_t qp_lst_length(JSContext *ctx, JSValueConst self)
{
    return qp_list_size(self);
}

static JSValue qp_lst_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    JSValue v;
    int k = qp_lst_getref(ctx, QP_LST_OBJ(self), i, &v);
    if (k < 0)
        return JS_EXCEPTION;
    if (k == 0)
        return qp_raise_index_error(ctx, "list index out of range");
    return v;
}

/* list_ass_item (v == QP_NULL deletes) */
static int qp_lst_ass_item(JSContext *ctx, JSValueConst self, Py_ssize_t i, JSValueConst v)
{
    JSObject *p = QP_LST_OBJ(self);
    JSValue old;
    if (qp_lst_mfast(ctx, p))
        return -1;
    if (i < 0 || (size_t)i >= p->u.array.count) {
        qp_raise_index_error(ctx, "list assignment index out of range");
        return -1;
    }
    if (QP_IS_NULL(v))
        return qp_lst_ass_slice(ctx, self, i, i + 1, QP_NULL);
    old = p->u.array.u.values[i];
    p->u.array.u.values[i] = JS_DupValue(ctx, v);
    JS_FreeValue(ctx, old);
    return 0;
}

static int qp_lst_contains(JSContext *ctx, JSValueConst self, JSValueConst el)
{
    JSObject *p = QP_LST_OBJ(self);
    JSValue item;
    Py_ssize_t i;
    int k, cmp;
    for (i = 0; ; i++) {
        k = qp_lst_getref(ctx, p, i, &item);
        if (k <= 0)
            return k;
        cmp = qp_richcompare_bool(ctx, item, el, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (cmp != 0)
            return cmp;
    }
}

/* list_concat */
static JSValue qp_lst_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSObject *pa = QP_LST_OBJ(a), *pb, *q;
    Py_ssize_t na, nb, i;
    JSValue np;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (!qp_list_check(ctx, b))
        return qp_raise_type_error(ctx, "can only concatenate list (not \"%.200s\") to list",
                                   qp_lst_tp_name(ctx, b, buf, sizeof(buf)));
    pb = QP_LST_OBJ(b);
    if (qp_lst_fast(ctx, pa) || qp_lst_fast(ctx, pb))
        return JS_EXCEPTION;
    na = pa->u.array.count;
    nb = pb->u.array.count;
    if (na + nb == 0)
        return JS_NewArray(ctx);
    if (na + nb > QP_LIST_MAX_SIZE)
        return qp_raise_memory_error(ctx);
    np = JS_NewArray(ctx);
    if (JS_IsException(np))
        return np;
    q = QP_LST_OBJ(np);
    if (qp_lst_reserve(ctx, q, na + nb)) {
        JS_FreeValue(ctx, np);
        return JS_EXCEPTION;
    }
    for (i = 0; i < na; i++)
        q->u.array.u.values[i] = JS_DupValue(ctx, pa->u.array.u.values[i]);
    for (i = 0; i < nb; i++)
        q->u.array.u.values[na + i] = JS_DupValue(ctx, pb->u.array.u.values[i]);
    qp_lst_set_count(q, na + nb);
    return np;
}

/* list_repeat */
static JSValue qp_lst_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t n)
{
    JSObject *p = QP_LST_OBJ(a), *q;
    Py_ssize_t input_size, output_size, i, j;
    JSValue np;

    if (qp_lst_fast(ctx, p))
        return JS_EXCEPTION;
    input_size = p->u.array.count;
    if (input_size == 0 || n <= 0)
        return JS_NewArray(ctx);
    if (input_size > QP_LIST_MAX_SIZE / n)
        return qp_raise_memory_error(ctx);
    output_size = input_size * n;
    np = JS_NewArray(ctx);
    if (JS_IsException(np))
        return np;
    q = QP_LST_OBJ(np);
    if (qp_lst_reserve(ctx, q, output_size)) {
        JS_FreeValue(ctx, np);
        return JS_EXCEPTION;
    }
    for (j = 0; j < n; j++) {
        for (i = 0; i < input_size; i++)
            q->u.array.u.values[j * input_size + i] = JS_DupValue(ctx, p->u.array.u.values[i]);
    }
    qp_lst_set_count(q, output_size);
    return np;
}

/* list_inplace_concat */
static JSValue qp_lst_inplace_concat(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (qp_list_extend(ctx, self, other))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

/* list_inplace_repeat */
static JSValue qp_lst_inplace_repeat(JSContext *ctx, JSValueConst self, Py_ssize_t n)
{
    if (qp_lst_inplace_repeat_int(ctx, QP_LST_OBJ(self), n))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

/* adjust_slice_indexes */
static Py_ssize_t qp_lst_adjust_slice(Py_ssize_t size, Py_ssize_t *start, Py_ssize_t *stop,
                                      Py_ssize_t step)
{
    Py_ssize_t slicelength = qp_slice_adjust_indices(size, start, stop, step);
    /* Make sure s[5:2] = [..] inserts at the right place:
       before 5, not before 2. */
    if ((step < 0 && *start < *stop) || (step > 0 && *start > *stop))
        *stop = *start;
    return slicelength;
}

/* list_subscript */
static JSValue qp_lst_subscript(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    JSObject *p = QP_LST_OBJ(self);
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (qp_lst_index_check(ctx, item)) {
        Py_ssize_t i;
        if (JS_VALUE_GET_TAG(item) == JS_TAG_INT)
            i = JS_VALUE_GET_INT(item);
        else if (qp_as_ssize(ctx, item, QP_EXC(ctx, IndexError), &i) < 0)
            return JS_EXCEPTION;
        if (i < 0)
            i += qp_list_size(self);
        return qp_lst_item(ctx, self, i);
    }
    if (qp_slice_check(item)) {
        Py_ssize_t start, stop, step, len, i;
        size_t cur;
        JSValue np;
        JSObject *q;

        if (qp_slice_unpack(ctx, item, &start, &stop, &step) < 0)
            return JS_EXCEPTION;
        if (qp_lst_fast(ctx, p))
            return JS_EXCEPTION;
        len = qp_slice_adjust_indices(p->u.array.count, &start, &stop, step);
        if (len <= 0)
            return JS_NewArray(ctx);
        if (step == 1)
            return qp_lst_slice(ctx, p, start, stop);
        np = JS_NewArray(ctx);
        if (JS_IsException(np))
            return np;
        q = QP_LST_OBJ(np);
        if (qp_lst_reserve(ctx, q, len)) {
            JS_FreeValue(ctx, np);
            return JS_EXCEPTION;
        }
        for (cur = start, i = 0; i < len; cur += (size_t)step, i++)
            q->u.array.u.values[i] = JS_DupValue(ctx, p->u.array.u.values[cur]);
        qp_lst_set_count(q, len);
        return np;
    }
    return qp_raise_type_error(ctx, "list indices must be integers or slices, not %.200s",
                               qp_lst_tp_name(ctx, item, buf, sizeof(buf)));
}

/* list_ass_subscript (value == QP_NULL deletes) */
static int qp_lst_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst item,
                                JSValueConst value)
{
    JSObject *p = QP_LST_OBJ(self);
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (qp_lst_index_check(ctx, item)) {
        Py_ssize_t i;
        if (JS_VALUE_GET_TAG(item) == JS_TAG_INT)
            i = JS_VALUE_GET_INT(item);
        else if (qp_as_ssize(ctx, item, QP_EXC(ctx, IndexError), &i) < 0)
            return -1;
        if (i < 0)
            i += qp_list_size(self);
        return qp_lst_ass_item(ctx, self, i, value);
    }
    if (qp_slice_check(item)) {
        Py_ssize_t start, stop, step, slicelength, i, size, nseq;
        size_t cur;
        JSValue *garbage, *items, seq;
        const JSValue *seqitems;

        if (qp_slice_unpack(ctx, item, &start, &stop, &step) < 0)
            return -1;
        if (QP_IS_NULL(value)) {
            /* delete slice */
            if (qp_lst_mfast(ctx, p))
                return -1;
            size = p->u.array.count;
            slicelength = qp_lst_adjust_slice(size, &start, &stop, step);
            if (step == 1)
                return qp_lst_ass_slice(ctx, self, start, stop, QP_NULL);
            if (slicelength <= 0)
                return 0;
            if (step < 0) {
                stop = start + 1;
                start = stop + step * (slicelength - 1) - 1;
                step = -step;
            }
            garbage = js_malloc(ctx, sizeof(JSValue) * slicelength);
            if (!garbage)
                return -1;
            items = p->u.array.u.values;
            /* memmove the parts of the list that are *not* part of the
               slice: step-1 items for each item that is part of the slice,
               and then the tail end of the list */
            for (cur = start, i = 0; cur < (size_t)stop; cur += step, i++) {
                Py_ssize_t lim = step - 1;
                garbage[i] = items[cur];
                if (cur + step >= (size_t)size)
                    lim = size - cur - 1;
                memmove(items + cur - i, items + cur + 1, sizeof(JSValue) * lim);
            }
            cur = start + (size_t)slicelength * step;
            if (cur < (size_t)size)
                memmove(items + cur - slicelength, items + cur, sizeof(JSValue) * (size - cur));
            qp_lst_set_count(p, size - slicelength);
            for (i = 0; i < slicelength; i++)
                JS_FreeValue(ctx, garbage[i]);
            js_free(ctx, garbage);
            return 0;
        }
        /* assign slice; protect against a[::-1] = a */
        if (JS_VALUE_GET_TAG(value) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(value) == p) {
            if (qp_lst_fast(ctx, p))
                return -1;
            seq = qp_lst_slice(ctx, p, 0, p->u.array.count);
        } else {
            seq = qp_lst_sequence_fast(ctx, value, "must assign iterable to extended slice");
        }
        if (JS_IsException(seq))
            return -1;
        if (qp_lst_mfast(ctx, p))
            goto fail;
        slicelength = qp_lst_adjust_slice(p->u.array.count, &start, &stop, step);
        if (step == 1) {
            int res = qp_lst_ass_slice(ctx, self, start, stop, seq);
            JS_FreeValue(ctx, seq);
            return res;
        }
        if (qp_lst_seq_items(ctx, seq, &seqitems, &nseq) || qp_lst_mfast(ctx, p))
            goto fail;
        if (nseq != slicelength) {
            qp_raise_value_error(ctx, "attempt to assign sequence of size %zd to extended slice of size %zd",
                                 nseq, slicelength);
            goto fail;
        }
        if (!slicelength) {
            JS_FreeValue(ctx, seq);
            return 0;
        }
        garbage = js_malloc(ctx, sizeof(JSValue) * slicelength);
        if (!garbage)
            goto fail;
        items = p->u.array.u.values;
        for (cur = start, i = 0; i < slicelength; cur += (size_t)step, i++) {
            garbage[i] = items[cur];
            items[cur] = JS_DupValue(ctx, seqitems[i]);
        }
        for (i = 0; i < slicelength; i++)
            JS_FreeValue(ctx, garbage[i]);
        js_free(ctx, garbage);
        JS_FreeValue(ctx, seq);
        return 0;
    fail:
        JS_FreeValue(ctx, seq);
        return -1;
    }
    qp_raise_type_error(ctx, "list indices must be integers or slices, not %.200s",
                        qp_lst_tp_name(ctx, item, buf, sizeof(buf)));
    return -1;
}

/* list_richcompare_impl */
static JSValue qp_lst_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    JSObject *pv, *pw;
    JSValue a, b, res;
    Py_ssize_t i;
    int k;

    if (!qp_list_check(ctx, v) || !qp_list_check(ctx, w))
        return qp_not_implemented(ctx);
    pv = QP_LST_OBJ(v);
    pw = QP_LST_OBJ(w);
    if (qp_lst_fast(ctx, pv) || qp_lst_fast(ctx, pw))
        return JS_EXCEPTION;
    if (pv->u.array.count != pw->u.array.count && (op == QP_CMP_EQ || op == QP_CMP_NE)) {
        /* Shortcut: if the lengths differ, the lists differ */
        return qp_bool_new(op == QP_CMP_NE);
    }
    /* Search for the first index where items are different */
    for (i = 0; ; i++) {
        if (qp_lst_fast(ctx, pv) || qp_lst_fast(ctx, pw))
            return JS_EXCEPTION;
        if (i >= pv->u.array.count || i >= pw->u.array.count)
            break;
        a = JS_DupValue(ctx, pv->u.array.u.values[i]);
        b = JS_DupValue(ctx, pw->u.array.u.values[i]);
        k = qp_richcompare_bool(ctx, a, b, QP_CMP_EQ);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (k < 0)
            return JS_EXCEPTION;
        if (!k)
            break;
    }
    if (qp_lst_fast(ctx, pv) || qp_lst_fast(ctx, pw))
        return JS_EXCEPTION;
    if (i >= pv->u.array.count || i >= pw->u.array.count) {
        /* No more items to compare -- compare sizes */
        return qp_lst_compare_sizes(pv->u.array.count, pw->u.array.count, op);
    }
    /* We have an item that differs -- shortcuts for EQ/NE */
    if (op == QP_CMP_EQ)
        return JS_FALSE;
    if (op == QP_CMP_NE)
        return JS_TRUE;
    /* Compare the final item again using the proper operator */
    a = JS_DupValue(ctx, pv->u.array.u.values[i]);
    b = JS_DupValue(ctx, pw->u.array.u.values[i]);
    res = qp_richcompare(ctx, a, b, op);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return res;
}

/* list_repr */
static JSValue qp_lst_repr(JSContext *ctx, JSValueConst self)
{
    JSObject *p = QP_LST_OBJ(self);
    StringBuffer b;
    JSValue item, r;
    Py_ssize_t i;
    int res, k;

    if (qp_list_size(self) == 0)
        return JS_NewString(ctx, "[]");
    res = qp_repr_enter(ctx, self);
    if (res != 0)
        return res > 0 ? JS_NewString(ctx, "[...]") : JS_EXCEPTION;
    string_buffer_init(ctx, &b, 16);
    string_buffer_putc8(&b, '[');
    /* Do repr() on each element.  Note that this may mutate the list,
       so the size is refetched on each iteration. */
    for (i = 0; ; i++) {
        k = qp_lst_getref(ctx, p, i, &item);
        if (k < 0)
            goto error;
        if (k == 0)
            break;
        if (i > 0)
            string_buffer_puts8(&b, ", ");
        r = qp_repr(ctx, item);
        JS_FreeValue(ctx, item);
        if (JS_IsException(r))
            goto error;
        string_buffer_concat_value(&b, r);
        JS_FreeValue(ctx, r);
    }
    string_buffer_putc8(&b, ']');
    qp_repr_leave(ctx, self);
    return string_buffer_end(&b);
 error:
    string_buffer_free(&b);
    qp_repr_leave(ctx, self);
    return JS_EXCEPTION;
}

static JSValue qp_lst_iter(JSContext *ctx, JSValueConst self)
{
    return qp_list_iter_new(ctx, self);
}

/* list_iteritem (virtual iteration) */
static int qp_lst_iteritem(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem)
{
    return qp_lst_getref(ctx, QP_LST_OBJ(seq), index, pitem);
}

/* PyType_GenericNew */
static JSValue qp_lst_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    if (subtype == QP_TYPE(ctx, list))
        return JS_NewArray(ctx);
    return qp_object_new_instance(ctx, subtype, JS_CLASS_ARRAY);
}

/* list___init___impl */
static int qp_lst_init_impl(JSContext *ctx, JSValueConst self, JSValueConst iterable)
{
    JSObject *p = QP_LST_OBJ(self);
    /* Empty previous contents */
    if (qp_list_size(self) != 0 || (p->fast_array && p->u.array.u.values)) {
        if (qp_lst_clear(ctx, p))
            return -1;
    }
    if (!QP_IS_NULL(iterable))
        return qp_list_extend(ctx, self, iterable);
    return 0;
}

/* list.__init__ (Argument Clinic wrapper) */
static int qp_lst_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                       size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t = qp_type_of(ctx, self);

    if ((t == QP_TYPE(ctx, list) || t->slots.tp_new == qp_lst_new) &&
        qp_arg_no_keywords(ctx, "list", kwnames) < 0)
        return -1;
    if (qp_arg_check_positional(ctx, "list", nargs, 0, 1) < 0)
        return -1;
    return qp_lst_init_impl(ctx, self, nargs ? args[0] : QP_NULL);
}

/* list_vectorcall: list(...) */
static JSValue qp_lst_vectorcall(JSContext *ctx, JSValueConst type, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t;
    JSValue list;

    if (qp_arg_no_keywords(ctx, "list", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "list", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    t = qp_type_from_value(type);
    if (!t || t == QP_TYPE(ctx, list))
        list = JS_NewArray(ctx);
    else
        list = qp_object_new_instance(ctx, t, JS_CLASS_ARRAY);
    if (JS_IsException(list))
        return list;
    if (nargs && qp_lst_init_impl(ctx, list, args[0])) {
        JS_FreeValue(ctx, list);
        return JS_EXCEPTION;
    }
    return list;
}

/* ---------------------------------------------------------------------- */
/* Methods                                                                 */
/* ---------------------------------------------------------------------- */

static JSValue qp_lst_m_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp_lst_subscript(ctx, self, args[0]);
}

static JSValue qp_lst_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_list_reviter_new(ctx, self);
}

static JSValue qp_lst_m_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    /* sizeof(PyListObject) + allocated * sizeof(PyObject *) */
    int64_t res = 40;
    if (p->fast_array)
        res += (int64_t)p->u.array.u1.size * 8;
    return qp_long_from_int64(ctx, res);
}

static JSValue qp_lst_m_clear(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    if (qp_lst_clear(ctx, QP_LST_OBJ(self)))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_lst_m_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    if (qp_lst_fast(ctx, p))
        return JS_EXCEPTION;
    return qp_lst_slice(ctx, p, 0, p->u.array.count);
}

static JSValue qp_lst_m_append(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    if (qp_list_append(ctx, self, args[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_lst_m_insert(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t index;
    if (qp_arg_check_positional(ctx, "insert", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_arg_ssize(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (qp_list_insert(ctx, self, index, args[1]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_lst_m_extend(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    if (qp_list_extend(ctx, self, args[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* list_pop_impl */
static JSValue qp_lst_m_pop(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    Py_ssize_t index = -1, n;
    JSValue v, *items;

    if (qp_arg_check_positional(ctx, "pop", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs >= 1 && qp_arg_ssize(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (qp_lst_mfast(ctx, p))
        return JS_EXCEPTION;
    n = p->u.array.count;
    if (n == 0)
        return qp_raise_index_error(ctx, "pop from empty list");
    if (index < 0)
        index += n;
    if (index < 0 || index >= n)
        return qp_raise_index_error(ctx, "pop index out of range");
    items = p->u.array.u.values;
    v = items[index];
    memmove(items + index, items + index + 1, sizeof(JSValue) * (n - index - 1));
    qp_lst_set_count(p, n - 1);
    return v;
}

static JSValue qp_lst_m_remove(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    JSValue obj;
    Py_ssize_t i;
    int k, cmp;

    for (i = 0; ; i++) {
        k = qp_lst_getref(ctx, p, i, &obj);
        if (k < 0)
            return JS_EXCEPTION;
        if (k == 0)
            break;
        cmp = qp_richcompare_bool(ctx, obj, args[0], QP_CMP_EQ);
        JS_FreeValue(ctx, obj);
        if (cmp > 0) {
            if (qp_lst_ass_slice(ctx, self, i, i + 1, QP_NULL))
                return JS_EXCEPTION;
            return JS_UNDEFINED;
        }
        if (cmp < 0)
            return JS_EXCEPTION;
    }
    return qp_raise_value_error(ctx, "list.remove(x): x not in list");
}

/* list_index_impl */
static JSValue qp_lst_m_index(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    Py_ssize_t start = 0, stop = PY_SSIZE_T_MAX, i, size;
    JSValue obj;
    int k, cmp;

    if (qp_arg_check_positional(ctx, "index", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (nargs >= 2 && qp_lst_slice_index(ctx, args[1], &start))
        return JS_EXCEPTION;
    if (nargs >= 3 && qp_lst_slice_index(ctx, args[2], &stop))
        return JS_EXCEPTION;
    size = qp_list_size(self);
    if (start < 0) {
        start += size;
        if (start < 0)
            start = 0;
    }
    if (stop < 0) {
        stop += size;
        if (stop < 0)
            stop = 0;
    }
    for (i = start; i < stop; i++) {
        k = qp_lst_getref(ctx, p, i, &obj);
        if (k < 0)
            return JS_EXCEPTION;
        if (k == 0)
            break;
        cmp = qp_richcompare_bool(ctx, obj, args[0], QP_CMP_EQ);
        JS_FreeValue(ctx, obj);
        if (cmp > 0)
            return qp_long_from_int64(ctx, i);
        if (cmp < 0)
            return JS_EXCEPTION;
    }
    return qp_raise_value_error(ctx, "list.index(x): x not in list");
}

static JSValue qp_lst_m_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    Py_ssize_t count = 0, i;
    JSValue obj;
    int k, cmp;

    for (i = 0; ; i++) {
        k = qp_lst_getref(ctx, p, i, &obj);
        if (k < 0)
            return JS_EXCEPTION;
        if (k == 0)
            break;
        cmp = qp_richcompare_bool(ctx, obj, args[0], QP_CMP_EQ);
        JS_FreeValue(ctx, obj);
        if (cmp > 0)
            count++;
        else if (cmp < 0)
            return JS_EXCEPTION;
    }
    return qp_long_from_int64(ctx, count);
}

static JSValue qp_lst_m_reverse(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSObject *p = QP_LST_OBJ(self);
    if (qp_lst_mfast(ctx, p))
        return JS_EXCEPTION;
    if (p->u.array.count > 1)
        qp_sort_reverse(p->u.array.u.values, p->u.array.count);
    return JS_UNDEFINED;
}

/* list.sort(*, key=None, reverse=False) */
static JSValue qp_lst_m_sort(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    static const char *const keywords[] = { "key", "reverse", NULL };
    static const QPArgSpec spec = { "sort", keywords, 0, 0, 0 };
    JSValueConst out[2];
    JSValueConst key;
    int reverse = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    key = out[0];
    if (JS_IsUndefined(key) || JS_IsNull(key))
        key = QP_NULL;
    if (!QP_IS_NULL(out[1]) && qp_arg_bool(ctx, out[1], &reverse) < 0)
        return JS_EXCEPTION;
    if (qp_list_sort(ctx, self, key, reverse))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* Py_GenericAlias */
static JSValue qp_lst_m_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_list_methods[] = {
    { "__getitem__", qp_lst_m_getitem, QP_METH_O | QP_METH_COEXIST, 0,
      "__getitem__($self, index, /)\n--\n\nReturn self[index]." },
    { "__reversed__", qp_lst_m_reversed, QP_METH_NOARGS, 0,
      "__reversed__($self, /)\n--\n\nReturn a reverse iterator over the list." },
    { "__sizeof__", qp_lst_m_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nReturn the size of the list in memory, in bytes." },
    { "clear", qp_lst_m_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nRemove all items from list." },
    { "copy", qp_lst_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a shallow copy of the list." },
    { "append", qp_lst_m_append, QP_METH_O, 0,
      "append($self, object, /)\n--\n\nAppend object to the end of the list." },
    { "insert", qp_lst_m_insert, QP_METH_FASTCALL, 0,
      "insert($self, index, object, /)\n--\n\nInsert object before index." },
    { "extend", qp_lst_m_extend, QP_METH_O, 0,
      "extend($self, iterable, /)\n--\n\nExtend list by appending elements from the iterable." },
    { "pop", qp_lst_m_pop, QP_METH_FASTCALL, 0,
      "pop($self, index=-1, /)\n--\n\nRemove and return item at index (default last).\n\n"
      "Raises IndexError if list is empty or index is out of range." },
    { "remove", qp_lst_m_remove, QP_METH_O, 0,
      "remove($self, value, /)\n--\n\nRemove first occurrence of value.\n\n"
      "Raises ValueError if the value is not present." },
    { "index", qp_lst_m_index, QP_METH_FASTCALL, 0,
      "index($self, value, start=0, stop=sys.maxsize, /)\n--\n\nReturn first index of value.\n\n"
      "Raises ValueError if the value is not present." },
    { "count", qp_lst_m_count, QP_METH_O, 0,
      "count($self, value, /)\n--\n\nReturn number of occurrences of value." },
    { "reverse", qp_lst_m_reverse, QP_METH_NOARGS, 0,
      "reverse($self, /)\n--\n\nReverse *IN PLACE*." },
    { "sort", qp_lst_m_sort, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "sort($self, /, *, key=None, reverse=False)\n--\n\n"
      "Sort the list in ascending order and return None.\n\n"
      "The sort is in-place (i.e. the list itself is modified) and stable\n"
      "(i.e. the order of two equal elements is maintained).\n\n"
      "If a key function is given, apply it once to each list item and sort\n"
      "them, ascending or descending, according to their function values.\n\n"
      "The reverse flag can be set to sort in descending order." },
    { "__class_getitem__", qp_lst_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "lists are generic over the type of their contents" },
    { NULL },
};

static const QPTypeSpec qp_list_spec = {
    .name = "list",
    .doc = "list(iterable=(), /)\n--\n\nBuilt-in mutable sequence.\n\n"
           "If no argument is given, the constructor creates a new empty list.\n"
           "The argument must be an iterable if specified.",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_LIST_SUBCLASS |
             QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_ARRAY,
    .slots = {
        .tp_repr = qp_lst_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_vectorcall = qp_lst_vectorcall,
        .tp_richcompare = qp_lst_richcompare,
        .tp_iter = qp_lst_iter,
        .tp_iteritem = qp_lst_iteritem,
        .tp_init = qp_lst_init,
        .tp_new = qp_lst_new,
        .sq = {
            .sq_length = qp_lst_length,
            .sq_concat = qp_lst_concat,
            .sq_repeat = qp_lst_repeat,
            .sq_item = qp_lst_item,
            .sq_ass_item = qp_lst_ass_item,
            .sq_contains = qp_lst_contains,
            .sq_inplace_concat = qp_lst_inplace_concat,
            .sq_inplace_repeat = qp_lst_inplace_repeat,
        },
        .mp = {
            .mp_length = qp_lst_length,
            .mp_subscript = qp_lst_subscript,
            .mp_ass_subscript = qp_lst_ass_subscript,
        },
    },
    .methods = qp_list_methods,
};

/* Called by qp_init_sequences (iterobject.c). */
static int qp_init_list(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, list), &qp_list_spec);
}
