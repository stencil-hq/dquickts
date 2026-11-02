/*
 * dquickts Python: iterator objects (unit e).
 *
 * Derived from CPython 3.15 Objects/iterobject.c (seqiter, callable
 * iterator), the iterator parts of Objects/listobject.c, tupleobject.c,
 * rangeobject.c, unicodeobject.c, bytesobject.c, bytearrayobject.c,
 * _PySequence_IterSearch of Objects/abstract.c and unpack_iterable of
 * Python/ceval.c (PSF-2.0).
 *
 * Every iterator type of unit e is an instance of the JS class PY_ITER with
 * the QPIterObj payload below; its Python type (the JS prototype) selects
 * tp_iternext and the methods.  qp_init_sequences, the init function of
 * unit e, also lives here.
 */

/* ---- cross-file helpers of unit e ---------------------------------------- */
/* rangeobject.c */
static BOOL qp_rg_i64(JSValueConst v, int64_t *out);
static JSValue qp_rg_from_i64(JSContext *ctx, int64_t v);
static JSValue qp_rg_add(JSContext *ctx, JSValueConst a, JSValueConst b);
static JSValue qp_rg_sub(JSContext *ctx, JSValueConst a, JSValueConst b);
static JSValue qp_rg_mul(JSContext *ctx, JSValueConst a, JSValueConst b);
static JSValue qp_rg_neg(JSContext *ctx, JSValueConst a);
static int qp_rg_cmp(JSValueConst a, JSValueConst b);
static int qp_rg_sign(JSValueConst a);
static uint64_t qp_range_len_i64(int64_t lo, int64_t hi, int64_t step);
static JSValue qp_range_make(JSContext *ctx, JSValue start, JSValue stop, JSValue step);
static int qp_init_range(JSContext *ctx);
/* sliceobject.c */
static int qp_init_slice(JSContext *ctx);
/* listobject.c / tupleobject.c (M2ListTuple) */
static int qp_init_list(JSContext *ctx);
static int qp_init_tuple(JSContext *ctx);

#define QP_ITERSEARCH_COUNT    1
#define QP_ITERSEARCH_INDEX    2
#define QP_ITERSEARCH_CONTAINS 3

/* Payload of every PY_ITER object.  Field use per type:
   iterator (seqiter), list_iterator, list_reverseiterator, tuple_iterator,
   str(_ascii)_iterator, bytes_iterator, bytearray_iterator:
       seq = sequence (QP_NULL once exhausted), index = next index
       (str: code-point index, pos = matching UTF-16 offset);
   callable_iterator: seq = callable, aux = sentinel (both QP_NULL once
       exhausted);
   range_iterator: start/step/len (int64);
   longrange_iterator: seq = start, aux = step, aux2 = len (exact ints). */
typedef struct QPIterObj {
    JSValue seq, aux, aux2;
    Py_ssize_t index;
    Py_ssize_t pos;
    int64_t start, step, len;
} QPIterObj;

static QPIterObj *qp_iterobj(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_ITER)
        return NULL;
    return p->u.opaque;
}

static void qp_iter_finalizer(JSRuntime *rt, JSValue val)
{
    QPIterObj *it = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (it) {
        JS_FreeValueRT(rt, it->seq);
        JS_FreeValueRT(rt, it->aux);
        JS_FreeValueRT(rt, it->aux2);
        js_free_rt(rt, it);
    }
}

static void qp_iter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPIterObj *it = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (it) {
        JS_MarkValue(rt, it->seq, mark_func);
        JS_MarkValue(rt, it->aux, mark_func);
        JS_MarkValue(rt, it->aux2, mark_func);
    }
}

/* New PY_ITER instance of type t; *pit receives the zeroed payload (all
   JSValue fields QP_NULL). */
static JSValue qp_iterobj_new(JSContext *ctx, QPType *t, QPIterObj **pit)
{
    JSValue obj;
    QPIterObj *it;

    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), JS_CLASS_PY_ITER);
    if (JS_IsException(obj))
        return obj;
    it = js_mallocz(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    it->seq = QP_NULL;
    it->aux = QP_NULL;
    it->aux2 = QP_NULL;
    JS_VALUE_GET_OBJ(obj)->u.opaque = it;
    *pit = it;
    return obj;
}

/* Release the sequence of an exhausted iterator. */
static void qp_iterobj_clear(JSContext *ctx, QPIterObj *it)
{
    JSValue seq = it->seq;
    it->seq = QP_NULL;
    JS_FreeValue(ctx, seq);
}

/* ---- shared helpers (also used by rangeobject.c / sliceobject.c) -------- */

/* tp_name of type(v) as str ("int", heap types: __name__). */
static JSValue qp_seq_type_name(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* _PyIndex_Check: int, bool or a type with nb_index. */
static BOOL qp_seq_index_check(JSContext *ctx, JSValueConst v)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return TRUE;
    default:
        return qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
    }
}

/* PyLong_AsSsize_t (no __index__): TypeError "an integer is required". */
static int qp_seq_as_ssize_strict(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    JSValue iv;
    int ret;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    if (!qp_long_check(ctx, v)) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    iv = qp_long_value(ctx, v);
    if (JS_IsException(iv))
        return -1;
    ret = qp_long_as_ssize(ctx, iv, out);
    JS_FreeValue(ctx, iv);
    return ret;
}

/* PyLong_AsLong (no __index__). */
static int qp_seq_as_long_strict(JSContext *ctx, JSValueConst v, int64_t *out)
{
    JSValue iv;
    int ret;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    if (!qp_long_check(ctx, v)) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    iv = qp_long_value(ctx, v);
    if (JS_IsException(iv))
        return -1;
    ret = qp_long_as_int64(ctx, iv, out);
    JS_FreeValue(ctx, iv);
    return ret;
}

/* _PyEval_GetBuiltin: builtins[name] or AttributeError(name). */
static JSValue qp_seq_get_builtin(JSContext *ctx, JSAtom name)
{
    JSValue v, s;
    int r = qp_dict_get_item_atom(ctx, QP_STATE(ctx)->builtins_dict, name, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return v;
    s = qp_str_from_atom(ctx, name);
    if (JS_IsException(s))
        return s;
    qp_raise_value(ctx, QP_EXC(ctx, AttributeError), s);
    JS_FreeValue(ctx, s);
    return JS_EXCEPTION;
}

/* Clear the pending exception if it is an IndexError or StopIteration
   (or only StopIteration when index_error is FALSE); TRUE if cleared. */
static BOOL qp_seq_clear_stop(JSContext *ctx, BOOL index_error)
{
    if ((index_error && qp_exc_pending_matches(ctx, QP_EXC(ctx, IndexError))) ||
        qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return TRUE;
    }
    return FALSE;
}

/* _PySequence_IterSearch: contains (0/1), count, or index of obj in seq;
   -1 on error. */
static Py_ssize_t qp_seq_iter_search(JSContext *ctx, JSValueConst seq, JSValueConst obj, int op)
{
    JSValue it, item;
    Py_ssize_t n = 0;
    BOOL wrapped = FALSE;
    int cmp;

    it = qp_iter(ctx, seq);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JSValue name = qp_seq_type_name(ctx, seq);
            if (JS_IsException(name))
                return -1;
            JS_FreeValue(ctx, JS_GetException(ctx));
            if (op == QP_ITERSEARCH_CONTAINS)
                qp_raise_type_error(ctx, "argument of type '%U' is not a container or iterable", name);
            else
                qp_raise_type_error(ctx, "argument of type '%U' is not iterable", name);
            JS_FreeValue(ctx, name);
        }
        return -1;
    }
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item))
            goto fail;
        if (QP_IS_NULL(item))
            break;
        cmp = qp_richcompare_bool(ctx, item, obj, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (cmp < 0)
            goto fail;
        if (cmp > 0) {
            switch (op) {
            case QP_ITERSEARCH_COUNT:
                if (n == PY_SSIZE_T_MAX) {
                    qp_raise_overflow_error(ctx, "count exceeds C integer size");
                    goto fail;
                }
                ++n;
                break;
            case QP_ITERSEARCH_INDEX:
                if (wrapped) {
                    qp_raise_overflow_error(ctx, "index exceeds C integer size");
                    goto fail;
                }
                goto done;
            default:
                n = 1;
                goto done;
            }
        }
        if (op == QP_ITERSEARCH_INDEX) {
            if (n == PY_SSIZE_T_MAX)
                wrapped = TRUE;
            ++n;
        }
    }
    if (op == QP_ITERSEARCH_INDEX) {
        qp_raise_value_error(ctx, "sequence.index(x): x not in sequence");
        goto fail;
    }
 done:
    JS_FreeValue(ctx, it);
    return n;
 fail:
    JS_FreeValue(ctx, it);
    return -1;
}

/* PyObject_SelfIter */
static JSValue qp_iter_self(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

/* (callable, (arg,), index) / (callable, (arg,)) reduce tuples. */
static JSValue qp_iter_reduce_value(JSContext *ctx, JSValue callable, JSValueConst arg,
                                    BOOL with_index, Py_ssize_t index)
{
    JSValue items[3], args;

    if (JS_IsException(callable))
        return callable;
    args = qp_tuple_pack(ctx, 1, &arg);
    if (JS_IsException(args)) {
        JS_FreeValue(ctx, callable);
        return args;
    }
    items[0] = callable;
    items[1] = args;
    if (!with_index)
        return qp_tuple_new_steal(ctx, 2, items);
    items[2] = qp_rg_from_i64(ctx, index);
    if (JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, items[1]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 3, items);
}

/* (iter, (empty,)) for an exhausted iterator; consumes empty. */
static JSValue qp_iter_reduce_empty(JSContext *ctx, JSValue empty)
{
    JSValue res;
    if (JS_IsException(empty))
        return empty;
    res = qp_iter_reduce_value(ctx, qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter)), empty, FALSE, 0);
    JS_FreeValue(ctx, empty);
    return res;
}

/* ========================================================================= */
/* iterator (seqiter): iteration through __getitem__                          */
/* ========================================================================= */

static JSValue qp_seqiter_new(JSContext *ctx, JSValueConst seq)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, iterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, seq);
    it->index = 0;
    return obj;
}

/* PySequence_GetItem(seq, i) for i >= 0. */
static JSValue qp_seq_getitem_index(JSContext *ctx, JSValueConst seq, Py_ssize_t i)
{
    QPType *t = qp_type_of(ctx, seq);
    JSValue key, res;
    if (t->slots.sq.sq_item)
        return t->slots.sq.sq_item(ctx, seq, i);
    key = qp_rg_from_i64(ctx, i);
    if (JS_IsException(key))
        return key;
    res = qp_getitem(ctx, seq, key);
    JS_FreeValue(ctx, key);
    return res;
}

static JSValue qp_seqiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue seq, result;

    if (QP_IS_NULL(it->seq))
        return QP_NULL;
    if (it->index == PY_SSIZE_T_MAX)
        return qp_raise_overflow_error(ctx, "iter index too large");
    seq = JS_DupValue(ctx, it->seq);
    result = qp_seq_getitem_index(ctx, seq, it->index);
    JS_FreeValue(ctx, seq);
    if (!JS_IsException(result)) {
        it->index++;
        return result;
    }
    if (qp_seq_clear_stop(ctx, TRUE)) {
        qp_iterobj_clear(ctx, it);
        return QP_NULL;
    }
    return JS_EXCEPTION;
}

static JSValue qp_seqiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t seqsize, len;
    QPType *t;

    if (!QP_IS_NULL(it->seq)) {
        t = qp_type_of(ctx, it->seq);
        if (!t->slots.sq.sq_length && !t->slots.mp.mp_length)
            return qp_not_implemented(ctx);
        seqsize = qp_len(ctx, it->seq);
        if (seqsize == -1)
            return JS_EXCEPTION;
        len = seqsize - it->index;
        if (len >= 0)
            return qp_rg_from_i64(ctx, len);
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue qp_seqiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue iter = qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter));
    if (JS_IsException(iter))
        return iter;
    if (!QP_IS_NULL(it->seq))
        return qp_iter_reduce_value(ctx, iter, it->seq, TRUE, it->index);
    JS_FreeValue(ctx, iter);
    return qp_iter_reduce_empty(ctx, qp_tuple_new(ctx, 0));
}

static JSValue qp_seqiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index;
    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq)) {
        if (index < 0)
            index = 0;
        it->index = index;
    }
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* callable_iterator: iter(callable, sentinel)                                */
/* ========================================================================= */

static JSValue qp_calliter_new(JSContext *ctx, JSValueConst callable, JSValueConst sentinel)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, callable_iterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, callable);
    it->aux = JS_DupValue(ctx, sentinel);
    return obj;
}

static void qp_calliter_clear(JSContext *ctx, QPIterObj *it)
{
    JSValue c = it->seq, s = it->aux;
    it->seq = QP_NULL;
    it->aux = QP_NULL;
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, s);
}

static JSValue qp_calliter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue callable, result, sentinel;
    int ok;

    if (QP_IS_NULL(it->seq))
        return QP_NULL;
    callable = JS_DupValue(ctx, it->seq);
    result = qp_call0(ctx, callable);
    JS_FreeValue(ctx, callable);
    if (JS_IsException(result)) {
        if (qp_seq_clear_stop(ctx, FALSE)) {
            qp_calliter_clear(ctx, it);
            return QP_NULL;
        }
        return JS_EXCEPTION;
    }
    if (QP_IS_NULL(it->aux))     /* exhausted during the call */
        goto drop;
    sentinel = JS_DupValue(ctx, it->aux);
    ok = qp_richcompare_bool(ctx, sentinel, result, QP_CMP_EQ);
    JS_FreeValue(ctx, sentinel);
    if (ok == 0)
        return result;
    if (ok > 0)
        qp_calliter_clear(ctx, it);
    JS_FreeValue(ctx, result);
    return ok < 0 ? JS_EXCEPTION : QP_NULL;
 drop:
    JS_FreeValue(ctx, result);
    return QP_NULL;
}

static JSValue qp_calliter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue iter = qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter));
    JSValue items[2], targs;
    JSValueConst pair[2];

    if (JS_IsException(iter))
        return iter;
    if (!QP_IS_NULL(it->seq) && !QP_IS_NULL(it->aux)) {
        pair[0] = it->seq;
        pair[1] = it->aux;
        targs = qp_tuple_pack(ctx, 2, pair);
        if (JS_IsException(targs)) {
            JS_FreeValue(ctx, iter);
            return targs;
        }
        items[0] = iter;
        items[1] = targs;
        return qp_tuple_new_steal(ctx, 2, items);
    }
    JS_FreeValue(ctx, iter);
    return qp_iter_reduce_empty(ctx, qp_tuple_new(ctx, 0));
}

/* ========================================================================= */
/* list_iterator, list_reverseiterator, tuple_iterator                        */
/* ========================================================================= */

/* list[idx] for 0 <= idx: 1 (*pitem), 0 out of range, -1 error.  Holes of
   a non-fast array read as None. */
static int qp_iter_list_item(JSContext *ctx, JSValueConst list, Py_ssize_t idx, JSValue *pitem)
{
    JSValue *items;
    Py_ssize_t len;
    JSValue v;

    items = qp_list_items(list, &len);
    if (items) {
        if (idx < 0 || idx >= len)
            return 0;
        *pitem = JS_DupValue(ctx, items[idx]);
        return 1;
    }
    len = qp_list_size(list);
    if (idx < 0 || idx >= len)
        return 0;
    v = JS_GetPropertyInt64(ctx, list, idx);
    if (JS_IsException(v))
        return -1;
    *pitem = v;
    return 1;
}

static JSValue qp_list_iter_new(JSContext *ctx, JSValueConst list)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, list_iterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, list);
    it->index = 0;
    return obj;
}

static JSValue qp_list_reviter_new(JSContext *ctx, JSValueConst list)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, list_reverseiterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, list);
    it->index = qp_list_size(list) - 1;
    return obj;
}

static JSValue qp_listiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue item;
    int r;

    if (QP_IS_NULL(it->seq) || it->index < 0)
        return QP_NULL;
    r = qp_iter_list_item(ctx, it->seq, it->index, &item);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        it->index = -1;
        qp_iterobj_clear(ctx, it);
        return QP_NULL;
    }
    it->index++;
    return item;
}

static JSValue qp_listiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    if (!QP_IS_NULL(it->seq) && it->index >= 0) {
        Py_ssize_t len = qp_list_size(it->seq) - it->index;
        if (len >= 0)
            return qp_rg_from_i64(ctx, len);
    }
    return JS_NewInt32(ctx, 0);
}

/* listiter_reduce_general */
static JSValue qp_listiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue callable, res;
    JSAtom atom;

    if (magic) {
        atom = JS_NewAtom(ctx, "reversed");
        if (atom == JS_ATOM_NULL)
            return JS_EXCEPTION;
        callable = qp_seq_get_builtin(ctx, atom);
        JS_FreeAtom(ctx, atom);
    } else {
        callable = qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter));
    }
    if (JS_IsException(callable))
        return callable;
    if (!QP_IS_NULL(it->seq) && it->index >= 0)
        return qp_iter_reduce_value(ctx, callable, it->seq, TRUE, it->index);
    /* empty iterator: (callable, ([],)) */
    {
        JSValue empty = qp_list_new(ctx, 0);
        if (JS_IsException(empty)) {
            JS_FreeValue(ctx, callable);
            return empty;
        }
        res = qp_iter_reduce_value(ctx, callable, empty, FALSE, 0);
        JS_FreeValue(ctx, empty);
        return res;
    }
}

static JSValue qp_listiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index, size;
    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq)) {
        size = qp_list_size(it->seq);
        if (index < -1)
            index = -1;
        else if (index > size)
            index = size;   /* iterator exhausted */
        it->index = index;
    }
    return JS_UNDEFINED;
}

static JSValue qp_listreviter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue item;
    int r;

    if (QP_IS_NULL(it->seq) || it->index < 0)
        return QP_NULL;
    r = qp_iter_list_item(ctx, it->seq, it->index, &item);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        it->index--;
        return item;
    }
    it->index = -1;
    qp_iterobj_clear(ctx, it);
    return QP_NULL;
}

static JSValue qp_listreviter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t len = it->index + 1;
    if (QP_IS_NULL(it->seq) || qp_list_size(it->seq) < len)
        len = 0;
    return qp_rg_from_i64(ctx, len);
}

static JSValue qp_listreviter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index, size;
    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq)) {
        size = qp_list_size(it->seq);
        if (index < -1)
            index = -1;
        else if (index > size - 1)
            index = size - 1;
        it->index = index;
    }
    return JS_UNDEFINED;
}

static JSValue qp_tuple_iter_new(JSContext *ctx, JSValueConst tuple)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, tuple_iterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, tuple);
    it->index = 0;
    return obj;
}

static JSValue qp_tupleiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    if (QP_IS_NULL(it->seq))
        return QP_NULL;
    if (it->index < QP_TUPLE_SIZE(it->seq))
        return JS_DupValue(ctx, QP_TUPLE_ITEMS(it->seq)[it->index++]);
    qp_iterobj_clear(ctx, it);
    return QP_NULL;
}

static JSValue qp_tupleiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t len = 0;
    if (!QP_IS_NULL(it->seq))
        len = QP_TUPLE_SIZE(it->seq) - it->index;
    return qp_rg_from_i64(ctx, len);
}

/* Shared by tuple, bytes, bytearray and str iterators: (iter, (seq,), index)
   or, once exhausted, (iter, (empty,)) where magic selects the empty value
   (0: (), 1: ''). */
static JSValue qp_seqidx_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue iter;
    if (!QP_IS_NULL(it->seq)) {
        iter = qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter));
        return qp_iter_reduce_value(ctx, iter, it->seq, TRUE, it->index);
    }
    return qp_iter_reduce_empty(ctx, magic ? qp_str_from_cstr(ctx, "") : qp_tuple_new(ctx, 0));
}

static JSValue qp_tupleiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index;
    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq)) {
        if (index < 0)
            index = 0;
        else if (index > QP_TUPLE_SIZE(it->seq))
            index = QP_TUPLE_SIZE(it->seq);   /* exhausted iterator */
        it->index = index;
    }
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* range_iterator, longrange_iterator                                         */
/* ========================================================================= */

static JSValue qp_fast_range_iter(JSContext *ctx, int64_t start, int64_t step, int64_t len)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, range_iterator), &it);
    if (JS_IsException(obj))
        return obj;
    it->start = start;
    it->step = step;
    it->len = len;
    return obj;
}

/* longrange_iterator; consumes start, step and len. */
static JSValue qp_long_range_iter(JSContext *ctx, JSValue start, JSValue step, JSValue len)
{
    QPIterObj *it;
    JSValue obj = qp_iterobj_new(ctx, QP_TYPE(ctx, longrange_iterator), &it);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, start);
        JS_FreeValue(ctx, step);
        JS_FreeValue(ctx, len);
        return obj;
    }
    it->seq = start;
    it->aux = step;
    it->aux2 = len;
    return obj;
}

/* iter(range) (reverse = FALSE, CPython range_iter) and reversed(range)
   (reverse = TRUE, range_reverse).  The int64 range_iterator is used when
   every intermediate value fits a C long (64-bit), else longrange_iterator. */
static JSValue qp_rangeiter_new(JSContext *ctx, JSValueConst start, JSValueConst stop,
                                JSValueConst step, JSValueConst length, BOOL reverse)
{
    int64_t lstart, lstop, lstep, new_start, new_stop;
    uint64_t ulen;
    JSValue diff, product, sum, nstep;

    if (!qp_rg_i64(start, &lstart) || !qp_rg_i64(stop, &lstop) || !qp_rg_i64(step, &lstep))
        goto long_range;
    if (!reverse) {
        ulen = qp_range_len_i64(lstart, lstop, lstep);
        if (ulen > (uint64_t)INT64_MAX)
            goto long_range;
        /* check for potential overflow of lstart + ulen * lstep */
        if (ulen) {
            if (lstep > 0) {
                if (lstop > INT64_MAX - (lstep - 1))
                    goto long_range;
            } else {
                if (lstop < INT64_MIN + (-1 - lstep))
                    goto long_range;
            }
        }
        return qp_fast_range_iter(ctx, lstart, lstep, (int64_t)ulen);
    }
    /* reversed: range(start + (n-1)*step, start - step, -step) */
    if (lstep == INT64_MIN)
        goto long_range;
    if (lstep > 0) {
        if ((uint64_t)lstart - (uint64_t)INT64_MIN < (uint64_t)lstep)
            goto long_range;
    } else {
        if ((uint64_t)INT64_MAX - (uint64_t)lstart < 0 - (uint64_t)lstep)
            goto long_range;
    }
    ulen = qp_range_len_i64(lstart, lstop, lstep);
    if (ulen > (uint64_t)INT64_MAX)
        goto long_range;
    new_stop = lstart - lstep;
    new_start = (int64_t)((uint64_t)new_stop + ulen * (uint64_t)lstep);
    return qp_fast_range_iter(ctx, new_start, -lstep, (int64_t)ulen);

 long_range:
    if (!reverse)
        return qp_long_range_iter(ctx, JS_DupValue(ctx, start), JS_DupValue(ctx, step),
                                  JS_DupValue(ctx, length));
    diff = qp_rg_sub(ctx, length, JS_NewInt32(ctx, 1));
    if (JS_IsException(diff))
        return diff;
    product = qp_rg_mul(ctx, diff, step);
    JS_FreeValue(ctx, diff);
    if (JS_IsException(product))
        return product;
    sum = qp_rg_add(ctx, start, product);
    JS_FreeValue(ctx, product);
    if (JS_IsException(sum))
        return sum;
    nstep = qp_rg_neg(ctx, step);
    if (JS_IsException(nstep)) {
        JS_FreeValue(ctx, sum);
        return nstep;
    }
    return qp_long_range_iter(ctx, sum, nstep, JS_DupValue(ctx, length));
}

static JSValue qp_rangeiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    int64_t result;
    if (it->len > 0) {
        result = it->start;
        it->start = (int64_t)((uint64_t)result + (uint64_t)it->step);
        it->len--;
        return qp_rg_from_i64(ctx, result);
    }
    return QP_NULL;
}

static JSValue qp_rangeiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    return qp_rg_from_i64(ctx, qp_iterobj(self)->len);
}

/* (iter, (range,), None); consumes range. */
static JSValue qp_rangeiter_reduce_tuple(JSContext *ctx, JSValue range)
{
    JSValue items[3], iter;
    if (JS_IsException(range))
        return range;
    iter = qp_seq_get_builtin(ctx, QP_ATOM(ctx, iter));
    if (JS_IsException(iter)) {
        JS_FreeValue(ctx, range);
        return iter;
    }
    items[1] = qp_tuple_new_steal(ctx, 1, &range);
    if (JS_IsException(items[1])) {
        JS_FreeValue(ctx, iter);
        return JS_EXCEPTION;
    }
    items[0] = iter;
    items[2] = JS_UNDEFINED;
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_rangeiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue start, stop, step;

    start = qp_rg_from_i64(ctx, it->start);
    if (JS_IsException(start))
        return start;
    stop = qp_rg_from_i64(ctx, (int64_t)((uint64_t)it->start + (uint64_t)it->len * (uint64_t)it->step));
    if (JS_IsException(stop)) {
        JS_FreeValue(ctx, start);
        return stop;
    }
    step = qp_rg_from_i64(ctx, it->step);
    if (JS_IsException(step)) {
        JS_FreeValue(ctx, start);
        JS_FreeValue(ctx, stop);
        return step;
    }
    return qp_rangeiter_reduce_tuple(ctx, qp_range_make(ctx, start, stop, step));
}

static JSValue qp_rangeiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    int64_t index;
    if (qp_seq_as_long_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    /* silently clip the index value */
    if (index < 0)
        index = 0;
    else if (index > it->len)
        index = it->len;   /* exhausted iterator */
    it->start = (int64_t)((uint64_t)it->start + (uint64_t)index * (uint64_t)it->step);
    it->len -= index;
    return JS_UNDEFINED;
}

static JSValue qp_longrangeiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue new_start, new_len, result;

    if (qp_rg_sign(it->aux2) <= 0)
        return QP_NULL;
    new_start = qp_rg_add(ctx, it->seq, it->aux);
    if (JS_IsException(new_start))
        return new_start;
    new_len = qp_rg_sub(ctx, it->aux2, JS_NewInt32(ctx, 1));
    if (JS_IsException(new_len)) {
        JS_FreeValue(ctx, new_start);
        return new_len;
    }
    result = it->seq;
    it->seq = new_start;
    JS_FreeValue(ctx, it->aux2);
    it->aux2 = new_len;
    return result;
}

static JSValue qp_longrangeiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, qp_iterobj(self)->aux2);
}

static JSValue qp_longrangeiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValue product, stop;

    product = qp_rg_mul(ctx, it->aux2, it->aux);
    if (JS_IsException(product))
        return product;
    stop = qp_rg_add(ctx, it->seq, product);
    JS_FreeValue(ctx, product);
    if (JS_IsException(stop))
        return stop;
    return qp_rangeiter_reduce_tuple(ctx, qp_range_make(ctx, JS_DupValue(ctx, it->seq), stop,
                                                         JS_DupValue(ctx, it->aux)));
}

static JSValue qp_longrangeiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                           int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    JSValueConst state = args[0];
    JSValue product, new_start, new_len;

    if (!qp_long_check_exact(state))
        return qp_raise_type_error(ctx, "state must be an int, not %T", state);
    if (qp_rg_sign(state) < 0)
        state = JS_NewInt32(ctx, 0);
    else if (qp_rg_cmp(it->aux2, state) < 0)
        state = it->aux2;
    product = qp_rg_mul(ctx, state, it->aux);
    if (JS_IsException(product))
        return product;
    new_start = qp_rg_add(ctx, it->seq, product);
    JS_FreeValue(ctx, product);
    if (JS_IsException(new_start))
        return new_start;
    new_len = qp_rg_sub(ctx, it->aux2, state);
    if (JS_IsException(new_len)) {
        JS_FreeValue(ctx, new_start);
        return new_len;
    }
    JS_FreeValue(ctx, it->seq);
    it->seq = new_start;
    JS_FreeValue(ctx, it->aux2);
    it->aux2 = new_len;
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* str_ascii_iterator, str_iterator                                           */
/* ========================================================================= */

/* Code points are read from the flat JSString: a UTF-16 surrogate pair is
   one code point, lone surrogates are code points of their own (the str
   representation of unit d). */
static uint32_t qp_striter_read(JSString *p, Py_ssize_t *ppos)
{
    Py_ssize_t pos = *ppos;
    uint32_t c, c2;
    if (!p->is_wide_char) {
        *ppos = pos + 1;
        return p->u.str8[pos];
    }
    c = p->u.str16[pos++];
    if (c >= 0xD800 && c < 0xDC00 && pos < (Py_ssize_t)p->len) {
        c2 = p->u.str16[pos];
        if (c2 >= 0xDC00 && c2 < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
            pos++;
        }
    }
    *ppos = pos;
    return c;
}

static JSValue qp_str_iter_new(JSContext *ctx, JSValueConst str)
{
    QPIterObj *it;
    JSValue s, obj;
    JSString *p;
    BOOL ascii = TRUE;
    uint32_t i;

    s = qp_str_value(ctx, str);
    if (JS_IsException(s))
        return s;
    if (JS_VALUE_GET_TAG(s) == JS_TAG_STRING_ROPE) {
        s = js_linearize_string_rope(ctx, s);
        if (JS_IsException(s))
            return s;
    }
    p = JS_VALUE_GET_STRING(s);
    if (p->is_wide_char) {
        ascii = FALSE;
    } else {
        for (i = 0; i < p->len; i++) {
            if (p->u.str8[i] >= 0x80) {
                ascii = FALSE;
                break;
            }
        }
    }
    obj = qp_iterobj_new(ctx, ascii ? QP_TYPE(ctx, str_ascii_iterator) : QP_TYPE(ctx, str_iterator),
                         &it);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, s);
        return obj;
    }
    it->seq = s;
    it->index = 0;
    it->pos = 0;
    return obj;
}

static JSValue qp_striter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    JSString *p;
    uint32_t c;

    if (QP_IS_NULL(it->seq))
        return QP_NULL;
    p = JS_VALUE_GET_STRING(it->seq);
    if (it->pos < (Py_ssize_t)p->len) {
        c = qp_striter_read(p, &it->pos);
        it->index++;
        return qp_str_from_char(ctx, c);
    }
    qp_iterobj_clear(ctx, it);
    return QP_NULL;
}

static JSValue qp_striter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t len = 0, n;
    if (!QP_IS_NULL(it->seq)) {
        n = qp_str_len(ctx, it->seq);
        if (n < 0)
            return JS_EXCEPTION;
        len = n - it->index;
    }
    return qp_rg_from_i64(ctx, len);
}

static JSValue qp_striter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index, n, i, pos;
    JSString *p;

    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq)) {
        n = qp_str_len(ctx, it->seq);
        if (n < 0)
            return JS_EXCEPTION;
        if (index < 0)
            index = 0;
        else if (index > n)
            index = n;   /* iterator truncated */
        p = JS_VALUE_GET_STRING(it->seq);
        pos = 0;
        for (i = 0; i < index; i++)
            qp_striter_read(p, &pos);
        it->index = index;
        it->pos = pos;
    }
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* bytes_iterator, bytearray_iterator                                         */
/* ========================================================================= */

static JSValue qp_bytes_iter_new(JSContext *ctx, JSValueConst b)
{
    QPIterObj *it;
    QPType *t = qp_bytearray_check(ctx, b) ? QP_TYPE(ctx, bytearray_iterator)
                                           : QP_TYPE(ctx, bytes_iterator);
    JSValue obj = qp_iterobj_new(ctx, t, &it);
    if (JS_IsException(obj))
        return obj;
    it->seq = JS_DupValue(ctx, b);
    it->index = 0;
    return obj;
}

static JSValue qp_bytesiter_next(JSContext *ctx, JSValueConst self)
{
    QPIterObj *it = qp_iterobj(self);
    const uint8_t *data;
    Py_ssize_t len;

    if (QP_IS_NULL(it->seq) || it->index < 0)
        return QP_NULL;
    data = qp_bytes_data(it->seq, &len);
    if (it->index < len)
        return JS_NewInt32(ctx, data[it->index++]);
    it->index = -1;
    qp_iterobj_clear(ctx, it);
    return QP_NULL;
}

static JSValue qp_bytesiter_m_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t len = 0, size;
    if (!QP_IS_NULL(it->seq) && it->index >= 0) {
        qp_bytes_data(it->seq, &size);
        len = size - it->index;
        if (len < 0)
            len = 0;
    }
    return qp_rg_from_i64(ctx, len);
}

/* magic 0: bytes (clip to [0, len]); 1: bytearray (clip to [-1, len]). */
static JSValue qp_bytesiter_m_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj *it = qp_iterobj(self);
    Py_ssize_t index, size;
    if (qp_seq_as_ssize_strict(ctx, args[0], &index) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(it->seq) && it->index >= 0) {
        qp_bytes_data(it->seq, &size);
        if (index < (magic ? -1 : 0))
            index = magic ? -1 : 0;
        else if (index > size)
            index = size;
        it->index = index;
    }
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* unpack_seq / unpack_ex                                                     */
/* ========================================================================= */

/* CPython _PyEval_UnpackIterableStackRef.  out[] is filled in stack order:
   with n = before + (after >= 0 ? 1 + after : 0) values, the first item
   goes to out[n - 1] (top of stack) and the last one to out[0]. */
static int qp_unpack_iterable(JSContext *ctx, JSValueConst v, int before, int after, JSValue *out)
{
    int n = before + (after >= 0 ? 1 + after : 0);
    int i = 0, j;
    Py_ssize_t ll, size;
    JSValue it, w, l, *items;
    QPType *t;

    /* fast path: exact tuple/list of the right length */
    if (after < 0) {
        if (qp_tuple_check_exact(ctx, v) && QP_TUPLE_SIZE(v) == before) {
            for (i = 0; i < before; i++)
                out[before - 1 - i] = JS_DupValue(ctx, QP_TUPLE_ITEMS(v)[i]);
            return 0;
        }
        if (qp_list_check_exact(ctx, v) && (items = qp_list_items(v, &size)) != NULL &&
            size == before) {
            for (i = 0; i < before; i++)
                out[before - 1 - i] = JS_DupValue(ctx, items[i]);
            return 0;
        }
    }

    it = qp_iter(ctx, v);
    if (JS_IsException(it)) {
        t = qp_type_of(ctx, v);
        if (!t->slots.tp_iter && !t->slots.sq.sq_item &&
            qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JSValue name = qp_seq_type_name(ctx, v);
            if (!JS_IsException(name)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "cannot unpack non-iterable %U object", name);
                JS_FreeValue(ctx, name);
            }
        }
        return -1;
    }
    for (; i < before; i++) {
        w = qp_next(ctx, it);
        if (JS_IsException(w))
            goto error;
        if (QP_IS_NULL(w)) {
            if (after == -1)
                qp_raise_value_error(ctx, "not enough values to unpack (expected %d, got %d)",
                                     before, i);
            else
                qp_raise_value_error(ctx, "not enough values to unpack "
                                     "(expected at least %d, got %d)", before + after, i);
            goto error;
        }
        out[n - 1 - i] = w;
    }
    if (after == -1) {
        /* We better have exhausted the iterator now. */
        w = qp_next(ctx, it);
        if (JS_IsException(w))
            goto error;
        if (QP_IS_NULL(w)) {
            JS_FreeValue(ctx, it);
            return 0;
        }
        JS_FreeValue(ctx, w);
        if (qp_list_check_exact(ctx, v) || qp_tuple_check_exact(ctx, v) ||
            qp_dict_check_exact(ctx, v)) {
            ll = qp_dict_check_exact(ctx, v) ? qp_dict_size(v)
                : qp_list_check_exact(ctx, v) ? qp_list_size(v) : QP_TUPLE_SIZE(v);
            if (ll > before) {
                qp_raise_value_error(ctx, "too many values to unpack (expected %d, got %zd)",
                                     before, ll);
                goto error;
            }
        }
        qp_raise_value_error(ctx, "too many values to unpack (expected %d)", before);
        goto error;
    }
    l = qp_list_from_iterable(ctx, it);
    if (JS_IsException(l))
        goto error;
    out[n - 1 - i] = l;
    i++;
    ll = qp_list_size(l);
    if (ll < after) {
        qp_raise_value_error(ctx, "not enough values to unpack (expected at least %d, got %zd)",
                             before + after, (Py_ssize_t)before + ll);
        goto error;
    }
    /* Pop the "after-variable" args off the list. */
    for (j = after; j > 0; j--, i++) {
        if (qp_iter_list_item(ctx, l, ll - j, &w) <= 0) {
            if (!JS_HasException(ctx))
                qp_raise_system_error(ctx, "unpack: list changed size");
            goto error;
        }
        out[n - 1 - i] = w;
    }
    /* Resize the list. */
    if (after > 0 &&
        JS_SetProperty(ctx, l, JS_ATOM_length, qp_rg_from_i64(ctx, ll - after)) < 0)
        goto error;
    JS_FreeValue(ctx, it);
    return 0;
 error:
    for (j = 0; j < i; j++) {
        JS_FreeValue(ctx, out[n - 1 - j]);
        out[n - 1 - j] = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, it);
    return -1;
}

/* ========================================================================= */
/* type specs and unit init                                                   */
/* ========================================================================= */

#define QP_LENGTH_HINT_DOC "Private method returning an estimate of len(list(it))."
#define QP_REDUCE_DOC "Return state information for pickling."
#define QP_SETSTATE_DOC "Set state information for unpickling."

static const QPMethodDef qp_seqiter_methods[] = {
    { "__length_hint__", qp_seqiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_seqiter_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_seqiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_calliter_methods[] = {
    { "__reduce__", qp_calliter_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { NULL }
};

static const QPMethodDef qp_listiter_methods[] = {
    { "__length_hint__", qp_listiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_listiter_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_listiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_listreviter_methods[] = {
    { "__length_hint__", qp_listreviter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_listiter_m_reduce, QP_METH_NOARGS, 1, QP_REDUCE_DOC },
    { "__setstate__", qp_listreviter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_tupleiter_methods[] = {
    { "__length_hint__", qp_tupleiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_seqidx_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_tupleiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_rangeiter_methods[] = {
    { "__length_hint__", qp_rangeiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_rangeiter_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_rangeiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_longrangeiter_methods[] = {
    { "__length_hint__", qp_longrangeiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_longrangeiter_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_longrangeiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_striter_methods[] = {
    { "__length_hint__", qp_striter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_seqidx_m_reduce, QP_METH_NOARGS, 1, QP_REDUCE_DOC },
    { "__setstate__", qp_striter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_bytesiter_methods[] = {
    { "__length_hint__", qp_bytesiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_seqidx_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_bytesiter_m_setstate, QP_METH_O, 0, QP_SETSTATE_DOC },
    { NULL }
};

static const QPMethodDef qp_bytearrayiter_methods[] = {
    { "__length_hint__", qp_bytesiter_m_len, QP_METH_NOARGS, 0, QP_LENGTH_HINT_DOC },
    { "__reduce__", qp_seqidx_m_reduce, QP_METH_NOARGS, 0, QP_REDUCE_DOC },
    { "__setstate__", qp_bytesiter_m_setstate, QP_METH_O, 1, QP_SETSTATE_DOC },
    { NULL }
};

#define QP_ITER_SPEC(cname, pyname, next, meths)                              \
    static const QPTypeSpec cname = {                                         \
        .name = pyname,                                                       \
        .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE |              \
                 QP_TPFLAGS_DISALLOW_INSTANTIATION,                           \
        .base = -1,                                                           \
        .exc_base = -1,                                                       \
        .instance_class_id = JS_CLASS_PY_ITER,                                \
        .slots = { .tp_iter = qp_iter_self, .tp_iternext = next },            \
        .methods = meths,                                                     \
    };

QP_ITER_SPEC(qp_seqiter_spec, "iterator", qp_seqiter_next, qp_seqiter_methods)
QP_ITER_SPEC(qp_calliter_spec, "callable_iterator", qp_calliter_next, qp_calliter_methods)
QP_ITER_SPEC(qp_listiter_spec, "list_iterator", qp_listiter_next, qp_listiter_methods)
QP_ITER_SPEC(qp_listreviter_spec, "list_reverseiterator", qp_listreviter_next,
             qp_listreviter_methods)
QP_ITER_SPEC(qp_tupleiter_spec, "tuple_iterator", qp_tupleiter_next, qp_tupleiter_methods)
QP_ITER_SPEC(qp_rangeiter_spec, "range_iterator", qp_rangeiter_next, qp_rangeiter_methods)
QP_ITER_SPEC(qp_longrangeiter_spec, "longrange_iterator", qp_longrangeiter_next,
             qp_longrangeiter_methods)
QP_ITER_SPEC(qp_strasciiiter_spec, "str_ascii_iterator", qp_striter_next, qp_striter_methods)
QP_ITER_SPEC(qp_striter_spec, "str_iterator", qp_striter_next, qp_striter_methods)
QP_ITER_SPEC(qp_bytesiter_spec, "bytes_iterator", qp_bytesiter_next, qp_bytesiter_methods)
QP_ITER_SPEC(qp_bytearrayiter_spec, "bytearray_iterator", qp_bytesiter_next,
             qp_bytearrayiter_methods)

#undef QP_ITER_SPEC

/* Init of unit e: tuple, list, range, slice, ellipsis (+ Ellipsis) and
   every iterator type of the unit. */
static int qp_init_sequences(JSContext *ctx)
{
    static const struct {
        QPTypeId id;
        const QPTypeSpec *spec;
    } iter_types[] = {
        { QP_T_iterator, &qp_seqiter_spec },
        { QP_T_callable_iterator, &qp_calliter_spec },
        { QP_T_list_iterator, &qp_listiter_spec },
        { QP_T_list_reverseiterator, &qp_listreviter_spec },
        { QP_T_tuple_iterator, &qp_tupleiter_spec },
        { QP_T_range_iterator, &qp_rangeiter_spec },
        { QP_T_longrange_iterator, &qp_longrangeiter_spec },
        { QP_T_str_ascii_iterator, &qp_strasciiiter_spec },
        { QP_T_str_iterator, &qp_striter_spec },
        { QP_T_bytes_iterator, &qp_bytesiter_spec },
        { QP_T_bytearray_iterator, &qp_bytearrayiter_spec },
    };
    size_t i;

    if (qp_init_tuple(ctx) < 0 || qp_init_list(ctx) < 0 || qp_init_range(ctx) < 0 ||
        qp_init_slice(ctx) < 0)
        return -1;
    for (i = 0; i < countof(iter_types); i++) {
        if (qp_type_ready(ctx, QP_STATE(ctx)->types[iter_types[i].id], iter_types[i].spec) < 0)
            return -1;
    }
    return 0;
}
