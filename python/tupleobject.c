/*
 * tupleobject.c - Python `tuple` (object.h (e)).
 *
 * Derived from CPython Objects/tupleobject.c (PSF-2.0).
 *
 * A tuple is a JS_CLASS_PY_TUPLE object whose u.opaque is a QPTuple (cached
 * hash + inline items, object.h).  Exact tuples have the `tuple` type
 * object as JS prototype; subclass instances are made by
 * qp_object_new_instance with their class as prototype.  The empty tuple is
 * a per-context singleton (QPState.empty_tuple), created on first use since
 * type readying (unit h) builds tuples before qp_init_sequences runs.
 */

/* iterobject.c */
static JSValue qp_tuple_iter_new(JSContext *ctx, JSValueConst tuple);
/* listobject.c */
static const char *qp_lst_tp_name(JSContext *ctx, JSValueConst v, char *buf, int size);
static BOOL qp_lst_index_check(JSContext *ctx, JSValueConst v);
static int qp_lst_slice_index(JSContext *ctx, JSValueConst v, Py_ssize_t *pi);
static JSValue qp_lst_compare_sizes(Py_ssize_t a, Py_ssize_t b, int op);

#define QP_TUPLE_MAX_SIZE INT32_MAX

/* xxHash-based tuple hash constants (CPython Include/internal/pycore_tuple.h) */
#define QP_TUPLE_HASH_XXPRIME_1 ((uint64_t)11400714785074694791ULL)
#define QP_TUPLE_HASH_XXPRIME_2 ((uint64_t)14029467366897019727ULL)
#define QP_TUPLE_HASH_XXPRIME_5 ((uint64_t)2870177450012600261ULL)
#define QP_TUPLE_HASH_XXROTATE(x) (((x) << 31) | ((x) >> 33))

static void qp_tuple_finalizer(JSRuntime *rt, JSValue val)
{
    QPTuple *t = JS_VALUE_GET_OBJ(val)->u.opaque;
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < t->len; i++)
        JS_FreeValueRT(rt, t->items[i]);
    js_free_rt(rt, t);
}

static void qp_tuple_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPTuple *t = JS_VALUE_GET_OBJ(val)->u.opaque;
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < t->len; i++)
        JS_MarkValue(rt, t->items[i], mark_func);
}

/* tuple_alloc: new tuple of type `type` (NULL = tuple) with n None items. */
static JSValue qp_tup_alloc(JSContext *ctx, QPType *type, Py_ssize_t n)
{
    QPTuple *t;
    JSValue obj;
    Py_ssize_t i;

    if (n < 0 || n > QP_TUPLE_MAX_SIZE)
        return qp_raise_memory_error(ctx);
    t = js_malloc(ctx, offsetof(QPTuple, items) + sizeof(JSValue) * n);
    if (!t)
        return JS_EXCEPTION;
    t->hash = -1;
    t->len = (uint32_t)n;
    for (i = 0; i < n; i++)
        t->items[i] = JS_UNDEFINED;
    if (!type || type == QP_TYPE(ctx, tuple))
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, tuple)), JS_CLASS_PY_TUPLE);
    else
        obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_TUPLE);
    if (JS_IsException(obj)) {
        js_free(ctx, t);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = t;
    return obj;
}

/* tuple_get_empty */
static JSValue qp_tup_empty(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    if (JS_VALUE_GET_TAG(s->empty_tuple) != JS_TAG_OBJECT) {
        JSValue t = qp_tup_alloc(ctx, NULL, 0);
        if (JS_IsException(t))
            return t;
        s->empty_tuple = t;
    }
    return JS_DupValue(ctx, s->empty_tuple);
}

/* ---------------------------------------------------------------------- */
/* Exported API (object.h (e))                                             */
/* ---------------------------------------------------------------------- */

static BOOL qp_tuple_check(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_TUPLE;
}

static BOOL qp_tuple_check_exact(JSContext *ctx, JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(v);
    return p->class_id == JS_CLASS_PY_TUPLE && p->shape->proto == QP_TYPE(ctx, tuple)->obj;
}

static JSValue qp_tuple_new(JSContext *ctx, Py_ssize_t n)
{
    if (n == 0)
        return qp_tup_empty(ctx);
    return qp_tup_alloc(ctx, NULL, n);
}

static JSValue qp_tuple_new_steal(JSContext *ctx, Py_ssize_t n, JSValue *items)
{
    JSValue t;
    Py_ssize_t i;
    if (n == 0)
        return qp_tup_empty(ctx);
    t = qp_tup_alloc(ctx, NULL, n);
    if (JS_IsException(t)) {
        for (i = 0; i < n; i++)
            JS_FreeValue(ctx, items[i]);
        return t;
    }
    memcpy(QP_TUPLE_ITEMS(t), items, sizeof(JSValue) * n);
    return t;
}

/* PyTuple_FromArray */
static JSValue qp_tuple_pack(JSContext *ctx, Py_ssize_t n, JSValueConst *items)
{
    JSValue t;
    JSValue *dst;
    Py_ssize_t i;
    if (n == 0)
        return qp_tup_empty(ctx);
    t = qp_tup_alloc(ctx, NULL, n);
    if (JS_IsException(t))
        return t;
    dst = QP_TUPLE_ITEMS(t);
    for (i = 0; i < n; i++)
        dst[i] = JS_DupValue(ctx, items[i]);
    return t;
}

/* PySequence_Tuple */
static JSValue qp_tuple_from_iterable(JSContext *ctx, JSValueConst v)
{
    JSValue it, item, *buf = NULL, *nbuf;
    Py_ssize_t n = 0, size = 0;

    /* Special-case the common tuple and list cases, for efficiency.
       Tuple subclass instances are not returned as-is. */
    if (qp_tuple_check_exact(ctx, v))
        return JS_DupValue(ctx, v);
    if (qp_list_check_exact(ctx, v))
        return qp_list_as_tuple(ctx, v);
    it = qp_iter(ctx, v);
    if (JS_IsException(it))
        return it;
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item))
            goto fail;
        if (QP_IS_NULL(item))
            break;
        if (n >= size) {
            Py_ssize_t nsize = size ? size + (size >> 1) : 8;
            if (nsize > QP_TUPLE_MAX_SIZE) {
                nsize = QP_TUPLE_MAX_SIZE;
                if (n >= nsize) {
                    JS_FreeValue(ctx, item);
                    qp_raise_memory_error(ctx);
                    goto fail;
                }
            }
            nbuf = js_realloc(ctx, buf, sizeof(JSValue) * nsize);
            if (!nbuf) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            buf = nbuf;
            size = nsize;
        }
        buf[n++] = item;
    }
    JS_FreeValue(ctx, it);
    item = qp_tuple_new_steal(ctx, n, buf);
    js_free(ctx, buf);
    return item;
 fail:
    while (n-- > 0)
        JS_FreeValue(ctx, buf[n]);
    js_free(ctx, buf);
    JS_FreeValue(ctx, it);
    return JS_EXCEPTION;
}

/* ---------------------------------------------------------------------- */
/* Slots                                                                   */
/* ---------------------------------------------------------------------- */

/* tuple_repr */
static JSValue qp_tup_repr(JSContext *ctx, JSValueConst self)
{
    QPTuple *t = QP_TUPLE(self);
    StringBuffer b;
    JSValue r;
    uint32_t i, n = t->len;
    int res;

    if (n == 0)
        return JS_NewString(ctx, "()");
    /* While not mutable, it is still possible to end up with a cycle in a
       tuple through an object that stores itself within a tuple (and thus
       infinitely asks for the repr of itself). */
    res = qp_repr_enter(ctx, self);
    if (res != 0)
        return res > 0 ? JS_NewString(ctx, "(...)") : JS_EXCEPTION;
    string_buffer_init(ctx, &b, n == 1 ? 4 : 16);
    string_buffer_putc8(&b, '(');
    for (i = 0; i < n; i++) {
        if (i > 0)
            string_buffer_puts8(&b, ", ");
        /* an item C code has not set yet (PyTuple_New) */
        if (QP_IS_NULL(t->items[i])) {
            string_buffer_puts8(&b, "<NULL>");
            continue;
        }
        r = qp_repr(ctx, t->items[i]);
        if (JS_IsException(r))
            goto error;
        string_buffer_concat_value(&b, r);
        JS_FreeValue(ctx, r);
    }
    if (n == 1)
        string_buffer_putc8(&b, ',');
    string_buffer_putc8(&b, ')');
    qp_repr_leave(ctx, self);
    return string_buffer_end(&b);
 error:
    string_buffer_free(&b);
    qp_repr_leave(ctx, self);
    return JS_EXCEPTION;
}

/* tuple_hash: xxHash-based, cached (CPython 3.14+). */
static Py_hash_t qp_tup_hash(JSContext *ctx, JSValueConst self)
{
    QPTuple *t = QP_TUPLE(self);
    uint64_t acc;
    Py_hash_t lane;
    uint32_t i;

    if (t->hash != -1)
        return t->hash;
    /* Hashing an item recurses back into tuple_hash() when that item is
       itself a tuple, so guard the descent to raise RecursionError rather
       than overflow the C stack on a deeply nested tuple. */
    if (js_check_stack_overflow(ctx->rt, 0)) {
        qp_raise(ctx, QP_EXC(ctx, RecursionError),
                 "maximum recursion depth exceeded while hashing a tuple");
        return -1;
    }
    acc = QP_TUPLE_HASH_XXPRIME_5;
    for (i = 0; i < t->len; i++) {
        lane = qp_hash(ctx, t->items[i]);
        if (lane == -1)
            return -1;
        acc += (uint64_t)lane * QP_TUPLE_HASH_XXPRIME_2;
        acc = QP_TUPLE_HASH_XXROTATE(acc);
        acc *= QP_TUPLE_HASH_XXPRIME_1;
    }
    /* Add input length, mangled to keep the historical value of hash(()). */
    acc += (uint64_t)t->len ^ (QP_TUPLE_HASH_XXPRIME_5 ^ 3527539UL);
    if (acc == (uint64_t)-1)
        acc = 1546275796;
    t->hash = (Py_hash_t)acc;
    return t->hash;
}

static Py_ssize_t qp_tup_length(JSContext *ctx, JSValueConst self)
{
    return QP_TUPLE_SIZE(self);
}

static int qp_tup_contains(JSContext *ctx, JSValueConst self, JSValueConst el)
{
    QPTuple *t = QP_TUPLE(self);
    uint32_t i;
    int cmp = 0;
    for (i = 0; cmp == 0 && i < t->len; i++)
        cmp = qp_richcompare_bool(ctx, t->items[i], el, QP_CMP_EQ);
    return cmp;
}

static JSValue qp_tup_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    QPTuple *t = QP_TUPLE(self);
    if (i < 0 || i >= t->len)
        return qp_raise_index_error(ctx, "tuple index out of range");
    return JS_DupValue(ctx, t->items[i]);
}

/* tuple_slice: self[ilow:ihigh] with clamping. */
static JSValue qp_tup_slice(JSContext *ctx, JSValueConst self, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    QPTuple *t = QP_TUPLE(self);
    if (ilow < 0)
        ilow = 0;
    if (ihigh > t->len)
        ihigh = t->len;
    if (ihigh < ilow)
        ihigh = ilow;
    if (ilow == 0 && ihigh == t->len && qp_tuple_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    return qp_tuple_pack(ctx, ihigh - ilow, (JSValueConst *)t->items + ilow);
}

/* _PyTuple_Concat */
static JSValue qp_tup_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QPTuple *ta = QP_TUPLE(a), *tb;
    Py_ssize_t size, i;
    JSValue np;
    JSValue *dest;
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (ta->len == 0 && qp_tuple_check_exact(ctx, b))
        return JS_DupValue(ctx, b);
    if (!qp_tuple_check(ctx, b))
        return qp_raise_type_error(ctx, "can only concatenate tuple (not \"%.200s\") to tuple",
                                   qp_lst_tp_name(ctx, b, buf, sizeof(buf)));
    tb = QP_TUPLE(b);
    if (tb->len == 0 && qp_tuple_check_exact(ctx, a))
        return JS_DupValue(ctx, a);
    size = (Py_ssize_t)ta->len + tb->len;
    if (size == 0)
        return qp_tup_empty(ctx);
    np = qp_tup_alloc(ctx, NULL, size);
    if (JS_IsException(np))
        return np;
    dest = QP_TUPLE_ITEMS(np);
    for (i = 0; i < ta->len; i++)
        dest[i] = JS_DupValue(ctx, ta->items[i]);
    for (i = 0; i < tb->len; i++)
        dest[ta->len + i] = JS_DupValue(ctx, tb->items[i]);
    return np;
}

/* tuple_repeat */
static JSValue qp_tup_repeat(JSContext *ctx, JSValueConst self, Py_ssize_t n)
{
    QPTuple *t = QP_TUPLE(self);
    Py_ssize_t input_size = t->len, output_size, i, j;
    JSValue np;
    JSValue *dest;

    if (input_size == 0 || n == 1) {
        if (qp_tuple_check_exact(ctx, self))
            return JS_DupValue(ctx, self);
    }
    if (input_size == 0 || n <= 0)
        return qp_tup_empty(ctx);
    if (input_size > QP_TUPLE_MAX_SIZE / n)
        return qp_raise_memory_error(ctx);
    output_size = input_size * n;
    np = qp_tup_alloc(ctx, NULL, output_size);
    if (JS_IsException(np))
        return np;
    dest = QP_TUPLE_ITEMS(np);
    for (j = 0; j < n; j++) {
        for (i = 0; i < input_size; i++)
            dest[j * input_size + i] = JS_DupValue(ctx, t->items[i]);
    }
    return np;
}

/* tuple_subscript */
static JSValue qp_tup_subscript(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    QPTuple *t = QP_TUPLE(self);
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (qp_lst_index_check(ctx, item)) {
        Py_ssize_t i;
        if (JS_VALUE_GET_TAG(item) == JS_TAG_INT)
            i = JS_VALUE_GET_INT(item);
        else if (qp_as_ssize(ctx, item, QP_EXC(ctx, IndexError), &i) < 0)
            return JS_EXCEPTION;
        if (i < 0)
            i += t->len;
        return qp_tup_item(ctx, self, i);
    }
    if (qp_slice_check(item)) {
        Py_ssize_t start, stop, step, slicelength, i;
        size_t cur;
        JSValue np;
        JSValue *dest;

        if (qp_slice_unpack(ctx, item, &start, &stop, &step) < 0)
            return JS_EXCEPTION;
        slicelength = qp_slice_adjust_indices(t->len, &start, &stop, step);
        if (slicelength <= 0)
            return qp_tup_empty(ctx);
        if (start == 0 && step == 1 && slicelength == t->len &&
            qp_tuple_check_exact(ctx, self))
            return JS_DupValue(ctx, self);
        np = qp_tup_alloc(ctx, NULL, slicelength);
        if (JS_IsException(np))
            return np;
        dest = QP_TUPLE_ITEMS(np);
        for (cur = start, i = 0; i < slicelength; cur += step, i++)
            dest[i] = JS_DupValue(ctx, t->items[cur]);
        return np;
    }
    return qp_raise_type_error(ctx, "tuple indices must be integers or slices, not %.200s",
                               qp_lst_tp_name(ctx, item, buf, sizeof(buf)));
}

/* tuple_richcompare */
static JSValue qp_tup_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPTuple *vt, *wt;
    Py_ssize_t i, vlen, wlen;
    int k;

    if (!qp_tuple_check(ctx, v) || !qp_tuple_check(ctx, w))
        return qp_not_implemented(ctx);
    vt = QP_TUPLE(v);
    wt = QP_TUPLE(w);
    vlen = vt->len;
    wlen = wt->len;
    /* Search for the first index where items are different.
       Note that because tuples are immutable, it's safe to reuse vlen and
       wlen across the comparison calls. */
    for (i = 0; i < vlen && i < wlen; i++) {
        k = qp_richcompare_bool(ctx, vt->items[i], wt->items[i], QP_CMP_EQ);
        if (k < 0)
            return JS_EXCEPTION;
        if (!k)
            break;
    }
    if (i >= vlen || i >= wlen) {
        /* No more items to compare -- compare sizes */
        return qp_lst_compare_sizes(vlen, wlen, op);
    }
    /* We have an item that differs -- shortcuts for EQ/NE */
    if (op == QP_CMP_EQ)
        return JS_FALSE;
    if (op == QP_CMP_NE)
        return JS_TRUE;
    /* Compare the final item again using the proper operator */
    return qp_richcompare(ctx, vt->items[i], wt->items[i], op);
}

static JSValue qp_tup_iter(JSContext *ctx, JSValueConst self)
{
    return qp_tuple_iter_new(ctx, self);
}

/* tuple_iteritem (virtual iteration) */
static int qp_tup_iteritem(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem)
{
    QPTuple *t = QP_TUPLE(seq);
    if (index < 0 || index >= t->len)
        return 0;
    *pitem = JS_DupValue(ctx, t->items[index]);
    return 1;
}

/* tuple_subtype_new */
static JSValue qp_tup_subtype_new(JSContext *ctx, QPType *type, JSValueConst iterable)
{
    JSValue tmp, newobj;
    JSValue *dst;
    QPTuple *src;
    uint32_t i;

    tmp = QP_IS_NULL(iterable) ? qp_tup_empty(ctx) : qp_tuple_from_iterable(ctx, iterable);
    if (JS_IsException(tmp))
        return tmp;
    src = QP_TUPLE(tmp);
    /* This may allocate an empty tuple that is not the global one. */
    newobj = qp_tup_alloc(ctx, type, src->len);
    if (!JS_IsException(newobj)) {
        dst = QP_TUPLE_ITEMS(newobj);
        for (i = 0; i < src->len; i++)
            dst[i] = JS_DupValue(ctx, src->items[i]);
    }
    JS_FreeValue(ctx, tmp);
    return newobj;
}

/* tuple_new_impl */
static JSValue qp_tup_new_impl(JSContext *ctx, QPType *type, JSValueConst iterable)
{
    if (type != QP_TYPE(ctx, tuple))
        return qp_tup_subtype_new(ctx, type, iterable);
    if (QP_IS_NULL(iterable))
        return qp_tup_empty(ctx);
    return qp_tuple_from_iterable(ctx, iterable);
}

/* tuple.__new__ (Argument Clinic wrapper) */
static JSValue qp_tup_new(JSContext *ctx, QPType *type, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *tuple_type = QP_TYPE(ctx, tuple);

    if ((type == tuple_type || type->slots.tp_init == tuple_type->slots.tp_init) &&
        qp_arg_no_keywords(ctx, "tuple", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "tuple", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    return qp_tup_new_impl(ctx, type, nargs ? args[0] : QP_NULL);
}

/* tuple_vectorcall: tuple(...) */
static JSValue qp_tup_vectorcall(JSContext *ctx, JSValueConst type, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t;

    if (qp_arg_no_keywords(ctx, "tuple", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "tuple", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    t = qp_type_from_value(type);
    if (!t)
        t = QP_TYPE(ctx, tuple);
    return qp_tup_new_impl(ctx, t, nargs ? args[0] : QP_NULL);
}

/* ---------------------------------------------------------------------- */
/* Methods                                                                 */
/* ---------------------------------------------------------------------- */

static JSValue qp_tup_m_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    JSValue s = qp_tup_slice(ctx, self, 0, QP_TUPLE_SIZE(self));
    if (JS_IsException(s))
        return s;
    return qp_tuple_new_steal(ctx, 1, &s);
}

/* tuple_index_impl */
static JSValue qp_tup_m_index(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    QPTuple *t = QP_TUPLE(self);
    Py_ssize_t start = 0, stop = PY_SSIZE_T_MAX, i;
    int cmp;

    if (qp_arg_check_positional(ctx, "index", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (nargs >= 2 && qp_lst_slice_index(ctx, args[1], &start))
        return JS_EXCEPTION;
    if (nargs >= 3 && qp_lst_slice_index(ctx, args[2], &stop))
        return JS_EXCEPTION;
    if (start < 0) {
        start += t->len;
        if (start < 0)
            start = 0;
    }
    if (stop < 0)
        stop += t->len;
    else if (stop > t->len)
        stop = t->len;
    for (i = start; i < stop; i++) {
        cmp = qp_richcompare_bool(ctx, t->items[i], args[0], QP_CMP_EQ);
        if (cmp > 0)
            return qp_long_from_int64(ctx, i);
        if (cmp < 0)
            return JS_EXCEPTION;
    }
    return qp_raise_value_error(ctx, "tuple.index(x): x not in tuple");
}

/* tuple_count_impl */
static JSValue qp_tup_m_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    QPTuple *t = QP_TUPLE(self);
    Py_ssize_t count = 0;
    uint32_t i;
    int cmp;

    for (i = 0; i < t->len; i++) {
        cmp = qp_richcompare_bool(ctx, t->items[i], args[0], QP_CMP_EQ);
        if (cmp > 0)
            count++;
        else if (cmp < 0)
            return JS_EXCEPTION;
    }
    return qp_long_from_int64(ctx, count);
}

/* Py_GenericAlias */
static JSValue qp_tup_m_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_tuple_methods[] = {
    { "__getnewargs__", qp_tup_m_getnewargs, QP_METH_NOARGS, 0,
      "__getnewargs__($self, /)\n--\n\n" },
    { "index", qp_tup_m_index, QP_METH_FASTCALL, 0,
      "index($self, value, start=0, stop=sys.maxsize, /)\n--\n\nReturn first index of value.\n\n"
      "Raises ValueError if the value is not present." },
    { "count", qp_tup_m_count, QP_METH_O, 0,
      "count($self, value, /)\n--\n\nReturn number of occurrences of value." },
    { "__class_getitem__", qp_tup_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Tuples are generic over the types of their contents.\n\n"
      "For example, use ``tuple[int, str]`` for a pair whose first element\n"
      "is an int and second element is a string.\n\n"
      "Tuples also support the form ``tuple[T, ...]`` to indicate\n"
      "an arbitrary length tuple of elements of type T." },
    { NULL },
};

static const QPTypeSpec qp_tuple_spec = {
    .name = "tuple",
    .doc = "tuple(iterable=(), /)\n--\n\nBuilt-in immutable sequence.\n\n"
           "If no argument is given, the constructor returns an empty tuple.\n"
           "If iterable is specified the tuple is initialized from iterable's items.\n\n"
           "If the argument is a tuple, the return value is the same object.",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_TUPLE_SUBCLASS |
             QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_IMMUTABLETYPE |
             QP_TPFLAGS_PRIV_VARSIZE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_TUPLE,
    .slots = {
        .tp_repr = qp_tup_repr,
        .tp_hash = qp_tup_hash,
        .tp_vectorcall = qp_tup_vectorcall,
        .tp_richcompare = qp_tup_richcompare,
        .tp_iter = qp_tup_iter,
        .tp_iteritem = qp_tup_iteritem,
        .tp_new = qp_tup_new,
        .sq = {
            .sq_length = qp_tup_length,
            .sq_concat = qp_tup_concat,
            .sq_repeat = qp_tup_repeat,
            .sq_item = qp_tup_item,
            .sq_contains = qp_tup_contains,
        },
        .mp = {
            .mp_length = qp_tup_length,
            .mp_subscript = qp_tup_subscript,
        },
    },
    .methods = qp_tuple_methods,
};

/* Called by qp_init_sequences (iterobject.c), before qp_init_list. */
static int qp_init_tuple(JSContext *ctx)
{
    JSValue e;
    if (qp_type_ready(ctx, QP_TYPE(ctx, tuple), &qp_tuple_spec) < 0)
        return -1;
    e = qp_tup_empty(ctx);
    if (JS_IsException(e))
        return -1;
    JS_FreeValue(ctx, e);
    return 0;
}
