/*
 * dictobject.c - dict, frozendict (PEP 814), dict views, dict iterators and
 * mappingproxy for dquickts Python (object.h section (f)).
 *
 * Derived from CPython Objects/dictobject.c and Objects/descrobject.c
 * (mappingproxy) (PSF-2.0).
 *
 * The hash tables themselves are python/dictcore.h (insertion order,
 * probing, resize, LIFO popitem, version tags, iteration with mutation
 * detection).  This file is the CPython object glue: hashing keys with
 * qp_hash (flat strs hashed with qp_str_hash; ropes are flattened first
 * because dictcore needs flat strings), equality through
 * qp_richcompare_bool(EQ) (identity shortcut included), the Python-level
 * methods, the constructors, views and iterators, and the namespace helpers
 * (atom-keyed fast paths) used by the interpreter.
 *
 * Every instance of dict (or a dict subclass) is a JS object of class
 * JS_CLASS_PY_DICT whose u.opaque is a QPDict; frozendict uses
 * JS_CLASS_PY_FROZENDICT with the same payload.  Views and mappingproxy are
 * JS_CLASS_PY_DICT_VIEW (QPDictView), dict iterators JS_CLASS_PY_DICT_ITER
 * (QPDictIter, whose first byte is shared with setobject.c's set iterator:
 * kind == QP_DICTITER_KIND_SET routes finalization/marking there).
 */

#ifndef QP_DICTITER_KIND_SET
#define QP_DICTITER_KIND_SET 0xFF
#endif

/* setobject.c (same unit (f)) */
static int qp_init_sets(JSContext *ctx);
static void qp_setiter_finalizer(JSRuntime *rt, JSValue val);
static void qp_setiter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);

/* View / iterator kinds */
enum {
    QP_DV_KEYS = 0,
    QP_DV_VALUES = 1,
    QP_DV_ITEMS = 2,
    QP_DV_PROXY = 3,    /* mappingproxy (QPDictView.mapping = any mapping) */
};

/* Payload of JS_CLASS_PY_DICT_ITER for dict iterators. */
typedef struct QPDictIter {
    uint8_t kind;               /* QP_DV_KEYS/VALUES/ITEMS; must be first (see QP_DICTITER_KIND_SET) */
    uint8_t reversed;
    JSValue dict;               /* the dict; JS_UNDEFINED once exhausted (CPython di_dict = NULL) */
    qp_dictcore_iter st;
} QPDictIter;

/* Payload of JS_CLASS_PY_DICT_VIEW: dict_keys / dict_values / dict_items
   (mapping is a dict or frozendict) and mappingproxy (any mapping). */
typedef struct QPDictView {
    uint8_t kind;               /* QP_DV_* */
    JSValue mapping;
} QPDictView;

#define QP_DICTITER(v) ((QPDictIter *)JS_VALUE_GET_OBJ(v)->u.opaque)
#define QP_DICTVIEW(v) ((QPDictView *)JS_VALUE_GET_OBJ(v)->u.opaque)

/* PyDictObject / PyFrozenDictObject basic sizes on 64-bit CPython (for
   __sizeof__), and the entry sizes of the two key-table kinds. */
#define QP_DICT_BASICSIZE       48
#define QP_FROZENDICT_BASICSIZE 56
#define QP_DICTKEYS_HDRSIZE     32
#define QP_DICT_ENTRY_GENERAL   24
#define QP_DICT_ENTRY_UNICODE   16

static JSValue qp_dict_tp_iter(JSContext *ctx, JSValueConst self);
static JSValue qp_dictiter_new(JSContext *ctx, JSValueConst dict, int kind, int reversed);
static JSValue qp_dictview_new(JSContext *ctx, JSValueConst mapping, int kind);
static int qp_dict_merge_impl(JSContext *ctx, JSValueConst a, JSValueConst b,
                              int override, JSValue *dupkey);

/* ========================================================================= */
/* Callbacks for dictcore                                                    */
/* ========================================================================= */

static int qp_dict_eq_cb(JSContext *ctx, JSValueConst a, JSValueConst b, void *opaque)
{
    return qp_richcompare_bool(ctx, a, b, QP_CMP_EQ);
}

static Py_hash_t qp_dict_value_hash_cb(JSContext *ctx, JSValueConst v, void *opaque)
{
    return qp_hash(ctx, v);
}

#define QP_DICT_EQ qp_dict_eq_cb, NULL

/* ========================================================================= */
/* Type checks                                                               */
/* ========================================================================= */

static inline BOOL qp_dict_has_class(JSValueConst v, JSClassID id)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v)->class_id == id;
}

static inline BOOL qp_dict_proto_is(JSValueConst v, QPType *t)
{
    return JS_VALUE_GET_OBJ(v)->shape->proto == t->obj;
}

static BOOL qp_dict_check(JSContext *ctx, JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_DICT);
}

static BOOL qp_dict_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_DICT) && qp_dict_proto_is(v, QP_TYPE(ctx, dict));
}

static BOOL qp_anydict_check(JSContext *ctx, JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_DICT) || qp_dict_has_class(v, JS_CLASS_PY_FROZENDICT);
}

static inline BOOL qp_frozendict_check(JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_FROZENDICT);
}

static inline BOOL qp_frozendict_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_FROZENDICT) &&
        qp_dict_proto_is(v, QP_TYPE(ctx, frozendict));
}

static inline BOOL qp_anydict_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_dict_check_exact(ctx, v) || qp_frozendict_check_exact(ctx, v);
}

/* PyDictViewSet_Check: dict_keys or dict_items. */
static inline BOOL qp_dictviewset_check(JSValueConst v)
{
    return qp_dict_has_class(v, JS_CLASS_PY_DICT_VIEW) &&
        (QP_DICTVIEW(v)->kind == QP_DV_KEYS || QP_DICTVIEW(v)->kind == QP_DV_ITEMS);
}

static inline BOOL qp_dictview_kind_is(JSValueConst v, int kind)
{
    return qp_dict_has_class(v, JS_CLASS_PY_DICT_VIEW) && QP_DICTVIEW(v)->kind == kind;
}

/* Whether iterating `v` is plain dict iteration (Py_TYPE(v)->tp_iter == dict_iter). */
static inline BOOL qp_dict_has_dict_iter(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.tp_iter == qp_dict_tp_iter;
}

static inline Py_ssize_t qp_dict_nkw(JSValueConst kwnames)
{
    if (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames))
        return 0;
    return QP_TUPLE_SIZE(kwnames);
}

/* Py_TYPE(v)->tp_name as a str (heap types: __name__). */
static JSValue qp_dict_tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return JS_AtomToString(ctx, t->name);
}

/* obj.<name>(*args) with a C-string method name. */
static JSValue qp_dict_call_method_cstr(JSContext *ctx, JSValueConst obj, const char *name,
                                        JSValueConst *args, int nargs)
{
    JSValue res;
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    res = qp_call_method(ctx, obj, atom, args, nargs, QP_NULL);
    JS_FreeAtom(ctx, atom);
    return res;
}

/* ========================================================================= */
/* Allocation, GC                                                            */
/* ========================================================================= */

/* New empty instance of t (dict, frozendict or a subclass), with a table
   presized for `presize` items when presize > 0. */
static JSValue qp_dict_alloc(JSContext *ctx, QPType *t, Py_ssize_t presize, int unicode)
{
    QPDict *d;
    JSValue obj;

    d = js_malloc(ctx, sizeof(QPDict));
    if (!d)
        return JS_EXCEPTION;
    if (presize > 0) {
        if (qp_dictcore_init_presized(ctx, &d->core, presize, unicode) < 0) {
            js_free(ctx, d);
            return JS_EXCEPTION;
        }
    } else {
        qp_dictcore_init(&d->core);
    }
    d->uid = ++QP_STATE(ctx)->next_dict_uid;
    d->hash = -1;
    /* the class id is fixed for dict/frozendict: type dicts are built
       before qp_init_dicts readies those types */
    if (t == QP_TYPE(ctx, dict))
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), JS_CLASS_PY_DICT);
    else if (t == QP_TYPE(ctx, frozendict))
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), JS_CLASS_PY_FROZENDICT);
    else
        obj = qp_object_new_instance(ctx, t, t->instance_class_id);
    if (JS_IsException(obj)) {
        qp_dictcore_free(ctx->rt, &d->core);
        js_free(ctx, d);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = d;
    return obj;
}

static JSValue qp_dict_new(JSContext *ctx)
{
    return qp_dict_alloc(ctx, QP_TYPE(ctx, dict), 0, 0);
}

static JSValue qp_dict_new_presized(JSContext *ctx, Py_ssize_t n)
{
    return qp_dict_alloc(ctx, QP_TYPE(ctx, dict), n, 0);
}

static void qp_dict_finalizer(JSRuntime *rt, JSValue val)
{
    QPDict *d = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!d)
        return;
    JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
    qp_dictcore_free(rt, &d->core);
    js_free_rt(rt, d);
}

static void qp_dict_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPDict *d = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (d)
        qp_dictcore_mark(rt, &d->core, mark_func);
}

static void qp_dictiter_finalizer(JSRuntime *rt, JSValue val)
{
    QPDictIter *it = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!it)
        return;
    if (it->kind == QP_DICTITER_KIND_SET) {
        qp_setiter_finalizer(rt, val);
        return;
    }
    JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
    JS_FreeValueRT(rt, it->dict);
    js_free_rt(rt, it);
}

static void qp_dictiter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPDictIter *it = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!it)
        return;
    if (it->kind == QP_DICTITER_KIND_SET) {
        qp_setiter_mark(rt, val, mark_func);
        return;
    }
    JS_MarkValue(rt, it->dict, mark_func);
}

static void qp_dictview_finalizer(JSRuntime *rt, JSValue val)
{
    QPDictView *v = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!v)
        return;
    JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
    JS_FreeValueRT(rt, v->mapping);
    js_free_rt(rt, v);
}

static void qp_dictview_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPDictView *v = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (v)
        JS_MarkValue(rt, v->mapping, mark_func);
}

/* ========================================================================= */
/* Key hashing and the basic operations                                      */
/* ========================================================================= */

/* dict_unhashable_type: rewrap an exact TypeError raised while hashing
   `key` as "cannot use '%T' as a dict key (%S)". */
static void qp_dict_unhashable(JSContext *ctx, JSValueConst d, JSValueConst key)
{
    JSValue exc = qp_exc_fetch(ctx);
    if (qp_type_of(ctx, exc) != QP_EXC(ctx, TypeError)) {
        JS_Throw(ctx, exc);
        return;
    }
    if (qp_frozendict_check(d))
        qp_raise_type_error(ctx, "cannot use '%T' as a frozendict key (%S)", key, exc);
    else
        qp_raise_type_error(ctx, "cannot use '%T' as a dict key (%S)", key, exc);
    JS_FreeValue(ctx, exc);
}

/* Key preparation for dictcore: *pkey = the key as a new reference (ropes
   flattened: dictcore compares strs only when flat), *phash its hash.
   Hash errors are rewrapped like dict_unhashable_type.  0 / -1. */
static int qp_dict_prepare_key(JSContext *ctx, JSValueConst d, JSValueConst key,
                               JSValue *pkey, Py_hash_t *phash)
{
    JSValue k;
    Py_hash_t h;

    if (JS_VALUE_GET_TAG(key) == JS_TAG_STRING_ROPE) {
        k = js_linearize_string_rope(ctx, JS_DupValue(ctx, key));
        if (JS_IsException(k))
            return -1;
    } else {
        k = JS_DupValue(ctx, key);
    }
    if (JS_VALUE_GET_TAG(k) == JS_TAG_STRING)
        h = qp_str_hash(ctx, k);
    else
        h = qp_hash(ctx, k);
    if (h == -1) {
        qp_dict_unhashable(ctx, d, k);
        JS_FreeValue(ctx, k);
        return -1;
    }
    *pkey = k;
    *phash = h;
    return 0;
}

/* Lookup without KeyError: 1 (*pvalue new ref), 0 absent (*pvalue =
   QP_NULL), -1 error. */
static int qp_dict_lookup(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *pvalue)
{
    JSValue k, v;
    Py_hash_t h;
    Py_ssize_t ix;

    *pvalue = QP_NULL;
    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return -1;
    ix = qp_dictcore_lookup(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ, &v);
    if (ix == QP_DKIX_ERROR) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    if (ix < 0 || QP_IS_NULL(v)) {
        JS_FreeValue(ctx, k);
        return 0;
    }
    *pvalue = JS_DupValue(ctx, v);
    JS_FreeValue(ctx, k);
    return 1;
}

static int qp_dict_contains_impl(JSContext *ctx, JSValueConst d, JSValueConst key)
{
    JSValue k;
    Py_hash_t h;
    int r;

    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return -1;
    r = qp_dictcore_contains(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ);
    JS_FreeValue(ctx, k);
    return r;
}

/* d[key] = value on a dict or a frozendict under construction. */
static int qp_dict_setitem_impl(JSContext *ctx, JSValueConst d, JSValueConst key, JSValueConst value)
{
    JSValue k;
    Py_hash_t h;

    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return -1;
    return qp_dictcore_setitem_take(ctx, &QP_DICT(d)->core, k, h,
                                    JS_DupValue(ctx, value), QP_DICT_EQ);
}

/* 1 deleted, 0 absent, -1 error. */
static int qp_dict_delitem_impl(JSContext *ctx, JSValueConst d, JSValueConst key)
{
    JSValue k;
    Py_hash_t h;
    int r;

    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return -1;
    r = qp_dictcore_delitem(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ);
    JS_FreeValue(ctx, k);
    return r;
}

/* pop_lock_held: 1 (*presult owned), 0 absent, -1 error.  An empty dict
   answers 0 before hashing the key (as CPython). */
static int qp_dict_pop_impl(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *presult)
{
    JSValue k;
    Py_hash_t h;
    int r;

    *presult = QP_NULL;
    if (QP_DICT(d)->core.used == 0)
        return 0;
    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return -1;
    r = qp_dictcore_pop(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ, presult);
    JS_FreeValue(ctx, k);
    if (r <= 0)
        *presult = QP_NULL;
    return r;
}

/* dict_setdefault_ref_lock_held: new ref of the stored value. */
static JSValue qp_dict_setdefault_impl(JSContext *ctx, JSValueConst d, JSValueConst key,
                                       JSValueConst dflt)
{
    JSValue k, res;
    Py_hash_t h;
    int r;

    if (qp_dict_prepare_key(ctx, d, key, &k, &h) < 0)
        return JS_EXCEPTION;
    r = qp_dictcore_setdefault(ctx, &QP_DICT(d)->core, k, h, dflt, QP_DICT_EQ, &res);
    if (r < 0) {
        JS_FreeValue(ctx, k);
        return JS_EXCEPTION;
    }
    res = JS_DupValue(ctx, res);
    JS_FreeValue(ctx, k);
    return res;
}

/* ---- public API (object.h section (f)) ---------------------------------- */

/* frozendict_does_not_support(WHAT): the public mutation API (PyDict_SetItem,
   PyDict_DelItem, PyDict_Merge, ...) refuses frozendicts, e.g. exec() with
   frozendict globals storing a name.  -1 if d is a frozendict. */
static int qp_dict_check_mutable(JSContext *ctx, JSValueConst d, const char *what)
{
    if (qp_frozendict_check(d)) {
        qp_raise_type_error(ctx, "frozendict object does not support item %s", what);
        return -1;
    }
    return 0;
}

static int qp_dict_get_item(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *pvalue)
{
    return qp_dict_lookup(ctx, d, key, pvalue);
}

static int qp_dict_get_item_atom(JSContext *ctx, JSValueConst d, JSAtom key, JSValue *pvalue)
{
    JSValue k, v;
    Py_hash_t h;
    Py_ssize_t ix;

    *pvalue = QP_NULL;
    k = JS_AtomToString(ctx, key);
    if (JS_IsException(k))
        return -1;
    h = qp_str_hash(ctx, k);
    if (h == -1) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    ix = qp_dictcore_lookup(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ, &v);
    if (ix == QP_DKIX_ERROR) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    if (ix < 0 || QP_IS_NULL(v)) {
        JS_FreeValue(ctx, k);
        return 0;
    }
    *pvalue = JS_DupValue(ctx, v);
    JS_FreeValue(ctx, k);
    return 1;
}

static int qp_dict_set_item(JSContext *ctx, JSValueConst d, JSValueConst key, JSValueConst value)
{
    if (qp_dict_check_mutable(ctx, d, "assignment") < 0)
        return -1;
    return qp_dict_setitem_impl(ctx, d, key, value);
}

static int qp_dict_set_item_atom(JSContext *ctx, JSValueConst d, JSAtom key, JSValueConst value)
{
    JSValue k;
    Py_hash_t h;

    if (qp_dict_check_mutable(ctx, d, "assignment") < 0)
        return -1;
    k = JS_AtomToString(ctx, key);
    if (JS_IsException(k))
        return -1;
    h = qp_str_hash(ctx, k);
    if (h == -1) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    return qp_dictcore_setitem_take(ctx, &QP_DICT(d)->core, k, h,
                                    JS_DupValue(ctx, value), QP_DICT_EQ);
}

static int qp_dict_del_item(JSContext *ctx, JSValueConst d, JSValueConst key)
{
    if (qp_dict_check_mutable(ctx, d, "deletion") < 0)
        return -1;
    return qp_dict_delitem_impl(ctx, d, key);
}

static int qp_dict_del_item_atom(JSContext *ctx, JSValueConst d, JSAtom key)
{
    JSValue k;
    Py_hash_t h;
    int r;

    if (qp_dict_check_mutable(ctx, d, "deletion") < 0)
        return -1;
    k = JS_AtomToString(ctx, key);
    if (JS_IsException(k))
        return -1;
    h = qp_str_hash(ctx, k);
    if (h == -1) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    r = qp_dictcore_delitem(ctx, &QP_DICT(d)->core, k, h, QP_DICT_EQ);
    JS_FreeValue(ctx, k);
    return r;
}

static int qp_dict_contains(JSContext *ctx, JSValueConst d, JSValueConst key)
{
    return qp_dict_contains_impl(ctx, d, key);
}

static Py_ssize_t qp_dict_size(JSValueConst d)
{
    return QP_DICT(d)->core.used;
}

static int qp_dict_next(JSValueConst d, Py_ssize_t *ppos, JSValue *pkey, JSValue *pvalue)
{
    qp_dictcore *core = &QP_DICT(d)->core;
    Py_ssize_t ix;
    qp_dictentry *ep;

    if (!qp_dictcore_next(core, ppos, &ix))
        return 0;
    ep = &core->keys->entries[ix];
    if (pkey)
        *pkey = ep->key;
    if (pvalue)
        *pvalue = ep->value;
    return 1;
}

static JSValue qp_dict_setdefault(JSContext *ctx, JSValueConst d, JSValueConst key, JSValueConst dflt)
{
    return qp_dict_setdefault_impl(ctx, d, key, dflt);
}

static int qp_dict_pop(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *pvalue)
{
    return qp_dict_pop_impl(ctx, d, key, pvalue);
}

/* ========================================================================= */
/* Merging, updating, copying                                                */
/* ========================================================================= */

/* PyMapping_Keys(o) as a list (method_output_as_list for non-dicts). */
static JSValue qp_dict_mapping_keys(JSContext *ctx, JSValueConst o)
{
    JSValue out, it, res;

    if (qp_dict_check_exact(ctx, o))
        return qp_dict_keys_list(ctx, o);
    out = qp_call_method(ctx, o, QP_ATOM(ctx, keys), NULL, 0, QP_NULL);
    if (JS_IsException(out) || qp_list_check_exact(ctx, out))
        return out;
    it = qp_iter(ctx, out);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JSValue on = qp_dict_tp_name(ctx, qp_type_of(ctx, o));
            JSValue rn = JS_IsException(on) ? JS_EXCEPTION : qp_dict_tp_name(ctx, qp_type_of(ctx, out));
            if (!JS_IsException(rn)) {
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
                qp_raise_type_error(ctx, "%U.keys() returned a non-iterable (type %U)",
                                    on, rn);
            }
            JS_FreeValue(ctx, on);
            JS_FreeValue(ctx, rn);
        }
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, out);
    res = qp_list_from_iterable(ctx, it);
    JS_FreeValue(ctx, it);
    return res;
}

/* dict_merge(a, b, override, dupkey): 0, -1 (exception), or -2 when
   override == 2 found a duplicate key (*dupkey set, owned; dupkey must be
   non-NULL when override == 2). */
static int qp_dict_merge_impl(JSContext *ctx, JSValueConst a, JSValueConst b,
                              int override, JSValue *dupkey)
{
    JSValue keys, iter, key, value;
    int res = 0, status;

    if (qp_anydict_check(ctx, b) && qp_dict_has_dict_iter(ctx, b)) {
        JSValue dk = QP_NULL;
        int r = qp_dictcore_merge(ctx, &QP_DICT(a)->core, &QP_DICT(b)->core, override,
                                  override == 2 ? &dk : NULL, QP_DICT_EQ);
        if (r == QP_DICT_MERGE_DUP) {
            *dupkey = dk;
            return -2;
        }
        if (r == QP_DICT_MERGE_MUTATED) {
            qp_raise_runtime_error(ctx, "dict mutated during update");
            return -1;
        }
        return r < 0 ? -1 : 0;
    }

    /* the generic, slower way: keys() + __getitem__ */
    keys = qp_dict_mapping_keys(ctx, b);
    if (JS_IsException(keys))
        return -1;
    iter = qp_iter(ctx, keys);
    JS_FreeValue(ctx, keys);
    if (JS_IsException(iter))
        return -1;
    for (;;) {
        key = qp_next(ctx, iter);
        if (JS_IsException(key)) {
            res = -1;
            break;
        }
        if (QP_IS_NULL(key))
            break;
        if (override != 1) {
            status = qp_dict_contains_impl(ctx, a, key);
            if (status != 0) {
                if (status > 0) {
                    if (dupkey == NULL) {
                        JS_FreeValue(ctx, key);
                        continue;
                    }
                    *dupkey = key;
                    res = -2;
                } else {
                    JS_FreeValue(ctx, key);
                    res = -1;
                }
                break;
            }
        }
        value = qp_getitem(ctx, b, key);
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, key);
            res = -1;
            break;
        }
        status = qp_dict_setitem_impl(ctx, a, key, value);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        if (status < 0) {
            res = -1;
            break;
        }
    }
    JS_FreeValue(ctx, iter);
    return res;
}

/* _PyErr_FormatNote: add a note to the pending exception. */
static void qp_dict_add_note(JSContext *ctx, const char *fmt, Py_ssize_t i)
{
    JSValue exc, note, r;

    exc = qp_exc_fetch(ctx);
    note = qp_str_from_format(ctx, fmt, i);
    if (JS_IsException(note)) {
        JS_FreeValue(ctx, exc);
        return;
    }
    r = qp_call_method(ctx, exc, QP_ATOM(ctx, add_note), (JSValueConst *)&note, 1, QP_NULL);
    JS_FreeValue(ctx, note);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, exc);
        return;
    }
    JS_FreeValue(ctx, r);
    JS_Throw(ctx, exc);
}

/* merge_from_seq2_lock_held: d.update(iterable of pairs). */
static int qp_dict_merge_from_seq2(JSContext *ctx, JSValueConst d, JSValueConst seq2, int override)
{
    JSValue it, item, fast, key, value;
    Py_ssize_t i, n;
    int res = 0;

    it = qp_iter(ctx, seq2);
    if (JS_IsException(it))
        return -1;
    for (i = 0; ; i++) {
        item = qp_next(ctx, it);
        if (JS_IsException(item)) {
            res = -1;
            break;
        }
        if (QP_IS_NULL(item))
            break;

        /* PySequence_Fast(item, "object is not iterable") */
        if (qp_tuple_check_exact(ctx, item) || qp_list_check_exact(ctx, item)) {
            fast = JS_DupValue(ctx, item);
        } else {
            JSValue it2 = qp_iter(ctx, item);
            if (JS_IsException(it2)) {
                if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                    JS_FreeValue(ctx, qp_exc_fetch(ctx));
                    qp_raise_type_error(ctx, "object is not iterable");
                }
                fast = JS_EXCEPTION;
            } else {
                fast = qp_list_from_iterable(ctx, it2);
                JS_FreeValue(ctx, it2);
            }
        }
        if (JS_IsException(fast)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
                qp_dict_add_note(ctx, "Cannot convert dictionary update "
                                 "sequence element #%zd to a sequence", i);
            JS_FreeValue(ctx, item);
            res = -1;
            break;
        }
        key = value = QP_NULL;
        if (qp_tuple_check_exact(ctx, fast)) {
            n = QP_TUPLE_SIZE(fast);
            if (n == 2) {
                key = JS_DupValue(ctx, QP_TUPLE_ITEMS(fast)[0]);
                value = JS_DupValue(ctx, QP_TUPLE_ITEMS(fast)[1]);
            }
        } else {
            JSValue *items = qp_list_items(fast, &n);
            if (items) {
                if (n == 2) {
                    key = JS_DupValue(ctx, items[0]);
                    value = JS_DupValue(ctx, items[1]);
                }
            } else {
                JSValue idx0, idx1;
                n = qp_list_size(fast);
                if (n == 2) {
                    idx0 = JS_NewInt32(ctx, 0);
                    idx1 = JS_NewInt32(ctx, 1);
                    key = qp_getitem(ctx, fast, idx0);
                    if (!JS_IsException(key))
                        value = qp_getitem(ctx, fast, idx1);
                    if (JS_IsException(key) || JS_IsException(value)) {
                        if (!JS_IsException(key))
                            JS_FreeValue(ctx, key);
                        JS_FreeValue(ctx, fast);
                        JS_FreeValue(ctx, item);
                        res = -1;
                        break;
                    }
                }
            }
        }
        JS_FreeValue(ctx, fast);
        if (n != 2) {
            qp_raise_value_error(ctx, "dictionary update sequence element #%zd "
                                 "has length %zd; 2 is required", i, n);
            JS_FreeValue(ctx, item);
            res = -1;
            break;
        }
        if (override) {
            res = qp_dict_setitem_impl(ctx, d, key, value);
        } else {
            JSValue r = qp_dict_setdefault_impl(ctx, d, key, value);
            res = JS_IsException(r) ? -1 : 0;
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, item);
        if (res < 0)
            break;
    }
    JS_FreeValue(ctx, it);
    return res;
}

/* dict_update_arg: mapping (has keys()) or iterable of pairs. */
static int qp_dict_update_arg(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    JSValue keys;
    int r;

    if (qp_anydict_check_exact(ctx, arg))
        return qp_dict_merge_impl(ctx, self, arg, 1, NULL);
    r = qp_getattr_opt(ctx, arg, QP_ATOM(ctx, keys), &keys);
    if (r < 0)
        return -1;
    if (r > 0) {
        JS_FreeValue(ctx, keys);
        return qp_dict_merge_impl(ctx, self, arg, 1, NULL);
    }
    return qp_dict_merge_from_seq2(ctx, self, arg, 1);
}

/* dict_update_common: (positional arg)? then keyword arguments. */
static int qp_dict_update_common(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, const char *methname)
{
    Py_ssize_t i, nkw;

    if (qp_arg_check_positional(ctx, methname, nargs, 0, 1) < 0)
        return -1;
    if (nargs == 1 && qp_dict_update_arg(ctx, self, args[0]) < 0)
        return -1;
    nkw = qp_dict_nkw(kwnames);
    for (i = 0; i < nkw; i++) {
        if (qp_dict_setitem_impl(ctx, self, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0)
            return -1;
    }
    return 0;
}

static int qp_dict_merge(JSContext *ctx, JSValueConst d, JSValueConst other, int override)
{
    JSValue dupkey = QP_NULL;
    int r;

    if (qp_dict_check_mutable(ctx, d, "assignment") < 0)
        return -1;
    r = qp_dict_merge_impl(ctx, d, other, override, override == 2 ? &dupkey : NULL);
    if (r == -2) {
        qp_raise_key_error(ctx, dupkey);
        JS_FreeValue(ctx, dupkey);
        return -1;
    }
    return r;
}

static int qp_dict_update(JSContext *ctx, JSValueConst d, JSValueConst other)
{
    if (qp_dict_check_mutable(ctx, d, "assignment") < 0)
        return -1;
    return qp_dict_update_arg(ctx, d, other);
}

/* _PyObject_FunctionStr: "module.qualname()" / "qualname()" / str(x). */
static JSValue qp_dict_function_str(JSContext *ctx, JSValueConst x)
{
    JSValue qualname, module = QP_NULL, result = JS_EXCEPTION, builtins;
    int ret;

    ret = qp_getattr_opt(ctx, x, QP_ATOM(ctx, __qualname__), &qualname);
    if (ret <= 0) {
        if (ret < 0)
            return JS_EXCEPTION;
        return qp_str(ctx, x);
    }
    ret = qp_getattr_opt(ctx, x, QP_ATOM(ctx, __module__), &module);
    if (ret > 0 && !JS_IsUndefined(module) && !JS_IsNull(module)) {
        builtins = JS_NewString(ctx, "builtins");
        if (JS_IsException(builtins))
            goto done;
        ret = qp_richcompare_bool(ctx, module, builtins, QP_CMP_NE);
        JS_FreeValue(ctx, builtins);
        if (ret < 0)
            goto done;
        if (ret > 0) {
            result = qp_str_from_format(ctx, "%S.%S()", module, qualname);
            goto done;
        }
    } else if (ret < 0) {
        goto done;
    }
    result = qp_str_from_format(ctx, "%S()", qualname);
done:
    JS_FreeValue(ctx, qualname);
    if (!QP_IS_NULL(module))
        JS_FreeValue(ctx, module);
    return result;
}

/* DICT_MERGE (_PyDict_MergeUniq + _PyEval_FormatKwargsError). */
static int qp_dict_merge_kwargs(JSContext *ctx, JSValueConst d, JSValueConst other,
                                JSValueConst callable)
{
    JSValue dupkey = QP_NULL;
    int r = qp_dict_merge_impl(ctx, d, other, 2, &dupkey);

    if (r == 0)
        return 0;
    if (r == -2) {
        JSValue funcstr = qp_dict_function_str(ctx, callable);
        if (JS_IsException(funcstr)) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            qp_raise_type_error(ctx, "%s got multiple values for keyword argument '%S'",
                                "function", dupkey);
        } else {
            qp_raise_type_error(ctx, "%U got multiple values for keyword argument '%S'",
                                funcstr, dupkey);
            JS_FreeValue(ctx, funcstr);
        }
        JS_FreeValue(ctx, dupkey);
        return -1;
    }
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
        JSValue exc = qp_exc_fetch(ctx), keys;
        int has_keys = qp_getattr_opt(ctx, other, QP_ATOM(ctx, keys), &keys);
        if (has_keys == 0) {
            qp_raise_type_error(ctx, "Value after ** must be a mapping, not %T", other);
            JS_FreeValue(ctx, exc);
        } else if (has_keys > 0) {
            JS_FreeValue(ctx, keys);
            JS_Throw(ctx, exc);
        } else {
            JS_FreeValue(ctx, exc);
        }
    }
    return -1;
}

/* copy_lock_held_untracked(o, as_frozendict): a new exact dict (or exact
   frozendict) with the items of the dict/frozendict o. */
static JSValue qp_anydict_copy_as(JSContext *ctx, JSValueConst o, int as_frozendict)
{
    JSValue copy;
    int r;

    copy = qp_dict_alloc(ctx, as_frozendict ? QP_TYPE(ctx, frozendict) : QP_TYPE(ctx, dict), 0, 0);
    if (JS_IsException(copy))
        return copy;
    if (QP_DICT(o)->core.used == 0)
        return copy;
    if (qp_dict_has_dict_iter(ctx, o)) {
        r = qp_dictcore_copy(ctx, &QP_DICT(copy)->core, &QP_DICT(o)->core, QP_DICT_EQ);
        if (r == QP_DICT_MERGE_MUTATED) {
            qp_raise_runtime_error(ctx, "dict mutated during update");
            r = -1;
        }
    } else {
        r = qp_dict_merge_impl(ctx, copy, o, 1, NULL);
    }
    if (r < 0) {
        JS_FreeValue(ctx, copy);
        return JS_EXCEPTION;
    }
    return copy;
}

static JSValue qp_dict_copy(JSContext *ctx, JSValueConst d)
{
    return qp_anydict_copy_as(ctx, d, 0);
}

/* Contents as a list: kind QP_DV_KEYS / VALUES / ITEMS. */
static JSValue qp_dict_list_impl(JSContext *ctx, JSValueConst dv, int kind)
{
    qp_dictcore *core = &QP_DICT(dv)->core;
    Py_ssize_t n = core->used, i = 0, pos = 0, ix;
    JSValue *items, res;

    items = js_malloc(ctx, sizeof(JSValue) * (n > 0 ? n : 1));
    if (!items)
        return JS_EXCEPTION;
    while (i < n && qp_dictcore_next(core, &pos, &ix)) {
        qp_dictentry *ep = &core->keys->entries[ix];
        JSValue v;
        if (kind == QP_DV_KEYS) {
            v = JS_DupValue(ctx, ep->key);
        } else if (kind == QP_DV_VALUES) {
            v = JS_DupValue(ctx, ep->value);
        } else {
            JSValueConst kv[2] = { ep->key, ep->value };
            v = qp_tuple_pack(ctx, 2, kv);
            if (JS_IsException(v)) {
                while (i > 0)
                    JS_FreeValue(ctx, items[--i]);
                js_free(ctx, items);
                return JS_EXCEPTION;
            }
        }
        items[i++] = v;
    }
    res = qp_list_new_steal(ctx, i, items);
    js_free(ctx, items);
    return res;
}

static JSValue qp_dict_keys_list(JSContext *ctx, JSValueConst d)
{
    return qp_dict_list_impl(ctx, d, QP_DV_KEYS);
}

static JSValue qp_dict_values_list(JSContext *ctx, JSValueConst d)
{
    return qp_dict_list_impl(ctx, d, QP_DV_VALUES);
}

static JSValue qp_dict_items_list(JSContext *ctx, JSValueConst d)
{
    return qp_dict_list_impl(ctx, d, QP_DV_ITEMS);
}

/* BUILD_MAP (_PyDict_FromItems): presized, unicode table when every key is
   an exact str. */
static JSValue qp_dict_from_pairs(JSContext *ctx, int n, JSValueConst *kv)
{
    JSValue d;
    int i, unicode = 1;

    for (i = 0; i < n; i++) {
        int tag = JS_VALUE_GET_TAG(kv[2 * i]);
        if (tag != JS_TAG_STRING && tag != JS_TAG_STRING_ROPE) {
            unicode = 0;
            break;
        }
    }
    d = qp_dict_alloc(ctx, QP_TYPE(ctx, dict), n, unicode);
    if (JS_IsException(d))
        return d;
    for (i = 0; i < n; i++) {
        if (qp_dict_setitem_impl(ctx, d, kv[2 * i], kv[2 * i + 1]) < 0) {
            JS_FreeValue(ctx, d);
            return JS_EXCEPTION;
        }
    }
    return d;
}

/* ========================================================================= */
/* fromkeys                                                                  */
/* ========================================================================= */

/* dict_dict_fromkeys: d is a fresh exact dict/frozendict, iterable an exact
   dict/frozendict. */
static int qp_dict_dict_fromkeys(JSContext *ctx, JSValueConst d, JSValueConst iterable,
                                 JSValueConst value)
{
    qp_dictcore *core = &QP_DICT(d)->core, *other = &QP_DICT(iterable)->core;
    Py_ssize_t pos = 0, ix;

    if (qp_dictcore_fromkeys_resize(ctx, core, other->used, other->keys->unicode) < 0)
        return -1;
    while (qp_dictcore_next(other, &pos, &ix)) {
        qp_dictentry *ep = &other->keys->entries[ix];
        JSValue key = JS_DupValue(ctx, ep->key);
        int r = qp_dictcore_setitem(ctx, core, key, ep->hash, value, QP_DICT_EQ);
        JS_FreeValue(ctx, key);
        if (r < 0)
            return -1;
    }
    return 0;
}

/* dict_set_fromkeys */
static int qp_dict_set_fromkeys(JSContext *ctx, JSValueConst d, JSValueConst iterable,
                                JSValueConst value)
{
    qp_dictcore *core = &QP_DICT(d)->core;
    Py_ssize_t pos = 0;
    JSValue key;
    Py_hash_t hash;

    if (qp_dictcore_fromkeys_resize(ctx, core, qp_set_size(iterable), 0) < 0)
        return -1;
    while (qp_set_next(iterable, &pos, &key, &hash)) {
        int r;
        key = JS_DupValue(ctx, key);
        r = qp_dictcore_setitem(ctx, core, key, hash, value, QP_DICT_EQ);
        JS_FreeValue(ctx, key);
        if (r < 0)
            return -1;
    }
    return 0;
}

/* _PyDict_FromKeys(cls, iterable, value) */
static JSValue qp_dict_fromkeys_impl(JSContext *ctx, JSValueConst cls, JSValueConst iterable,
                                     JSValueConst value)
{
    QPType *fd_type = QP_TYPE(ctx, frozendict);
    JSValue d, it, key, res;
    int need_copy = 0, status = 0;

    if (qp_type_from_value(cls) == fd_type)
        d = qp_dict_alloc(ctx, fd_type, 0, 0);
    else
        d = qp_call0(ctx, cls);
    if (JS_IsException(d))
        return d;

    /* A frozendict returned by a subclass constructor is filled through a
       private exact copy, then passed to cls() at the end. */
    if (qp_frozendict_check(d) && qp_type_from_value(cls) != fd_type) {
        JSValue copy = qp_dict_alloc(ctx, fd_type, 0, 0);
        need_copy = 1;
        if (JS_IsException(copy))
            goto fail;
        if (qp_dict_merge_impl(ctx, copy, d, 1, NULL) < 0) {
            JS_FreeValue(ctx, copy);
            goto fail;
        }
        JS_FreeValue(ctx, d);
        d = copy;
    }

    if (qp_dict_check_exact(ctx, d) || qp_frozendict_check_exact(ctx, d)) {
        if (qp_anydict_check_exact(ctx, iterable)) {
            if (qp_dict_dict_fromkeys(ctx, d, iterable, value) < 0)
                goto fail;
            goto done;
        }
        if (qp_anyset_check(ctx, iterable) &&
            (qp_type_of(ctx, iterable) == QP_TYPE(ctx, set) ||
             qp_type_of(ctx, iterable) == QP_TYPE(ctx, frozenset))) {
            if (qp_dict_set_fromkeys(ctx, d, iterable, value) < 0)
                goto fail;
            goto done;
        }
    }

    it = qp_iter(ctx, iterable);
    if (JS_IsException(it))
        goto fail;
    if (qp_anydict_check(ctx, d) && (qp_dict_check_exact(ctx, d) || qp_frozendict_check(d))) {
        for (;;) {
            key = qp_next(ctx, it);
            if (JS_IsException(key)) {
                status = -1;
                break;
            }
            if (QP_IS_NULL(key))
                break;
            status = qp_dict_setitem_impl(ctx, d, key, value);
            JS_FreeValue(ctx, key);
            if (status < 0)
                break;
        }
    } else {
        for (;;) {
            key = qp_next(ctx, it);
            if (JS_IsException(key)) {
                status = -1;
                break;
            }
            if (QP_IS_NULL(key))
                break;
            status = qp_setitem(ctx, d, key, value);
            JS_FreeValue(ctx, key);
            if (status < 0)
                break;
        }
    }
    JS_FreeValue(ctx, it);
    if (status < 0)
        goto fail;
done:
    if (need_copy) {
        res = qp_call1(ctx, cls, d);
        JS_FreeValue(ctx, d);
        return res;
    }
    return d;
fail:
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* dict / frozendict slots                                                   */
/* ========================================================================= */

/* anydict_repr_impl: "{k: v, ...}" with recursion guard "{...}". */
static JSValue qp_anydict_repr_impl(JSContext *ctx, JSValueConst self)
{
    QPDict *d = QP_DICT(self);
    StringBuffer b_s, *b = &b_s;
    Py_ssize_t pos = 0, ix;
    int first = 1, r;
    JSValue s;

    r = qp_repr_enter(ctx, self);
    if (r != 0)
        return r > 0 ? JS_NewString(ctx, "{...}") : JS_EXCEPTION;
    if (d->core.used == 0) {
        qp_repr_leave(ctx, self);
        return JS_NewString(ctx, "{}");
    }
    if (string_buffer_init(ctx, b, 1 + 4 + 6 * (int)min_int64(d->core.used - 1, 1000) + 1))
        goto fail;
    string_buffer_putc8(b, '{');
    /* repr() of a key or value may mutate the dict: qp_dictcore_next
       re-reads the table at every step. */
    while (qp_dictcore_next(&d->core, &pos, &ix)) {
        qp_dictentry *ep = &d->core.keys->entries[ix];
        JSValue key = JS_DupValue(ctx, ep->key);
        JSValue value = JS_DupValue(ctx, ep->value);
        if (!first)
            string_buffer_puts8(b, ", ");
        first = 0;
        s = qp_repr(ctx, key);
        JS_FreeValue(ctx, key);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, value);
            goto fail;
        }
        string_buffer_concat_value(b, s);
        JS_FreeValue(ctx, s);
        string_buffer_puts8(b, ": ");
        s = qp_repr(ctx, value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(s))
            goto fail;
        string_buffer_concat_value(b, s);
        JS_FreeValue(ctx, s);
    }
    string_buffer_putc8(b, '}');
    qp_repr_leave(ctx, self);
    return string_buffer_end(b);
fail:
    qp_repr_leave(ctx, self);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue qp_dict_repr(JSContext *ctx, JSValueConst self)
{
    return qp_anydict_repr_impl(ctx, self);
}

static JSValue qp_frozendict_repr(JSContext *ctx, JSValueConst self)
{
    JSValue name, repr, res;

    name = qp_dict_tp_name(ctx, qp_type_of(ctx, self));
    if (JS_IsException(name))
        return name;
    if (QP_DICT(self)->core.used == 0) {
        res = qp_str_from_format(ctx, "%U()", name);
        JS_FreeValue(ctx, name);
        return res;
    }
    repr = qp_anydict_repr_impl(ctx, self);
    if (JS_IsException(repr)) {
        JS_FreeValue(ctx, name);
        return repr;
    }
    res = qp_str_from_format(ctx, "%U(%U)", name, repr);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, repr);
    return res;
}

static Py_ssize_t qp_dict_length(JSContext *ctx, JSValueConst self)
{
    return QP_DICT(self)->core.used;
}

/* _PyDict_Subscript: KeyError(key), or type(self).__missing__(self, key)
   for subclasses. */
static JSValue qp_dict_subscript(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    JSValue v, missing, res;
    int r;

    r = qp_dict_lookup(ctx, self, key, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return v;
    if (!qp_anydict_check_exact(ctx, self)) {
        missing = qp_lookup_special(ctx, self, QP_ATOM(ctx, __missing__));
        if (JS_IsException(missing))
            return missing;
        if (!QP_IS_NULL(missing)) {
            res = qp_call1(ctx, missing, key);
            JS_FreeValue(ctx, missing);
            return res;
        }
    }
    return qp_raise_key_error(ctx, key);
}

/* _PyDict_StoreSubscript: v == QP_NULL deletes (KeyError if absent). */
static int qp_dict_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst key, JSValueConst v)
{
    int r;

    if (!QP_IS_NULL(v))
        return qp_dict_setitem_impl(ctx, self, key, v);
    r = qp_dict_delitem_impl(ctx, self, key);
    if (r == 0) {
        qp_raise_key_error(ctx, key);
        return -1;
    }
    return r < 0 ? -1 : 0;
}

static int qp_dict_sq_contains(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    return qp_dict_contains_impl(ctx, self, key);
}

static JSValue qp_dict_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    int cmp;

    if (!qp_anydict_check(ctx, v) || !qp_anydict_check(ctx, w) ||
        (op != QP_CMP_EQ && op != QP_CMP_NE))
        return qp_not_implemented(ctx);
    cmp = qp_dictcore_equal(ctx, &QP_DICT(v)->core, &QP_DICT(w)->core, QP_DICT_EQ);
    if (cmp < 0)
        return JS_EXCEPTION;
    return qp_bool_new(cmp == (op == QP_CMP_EQ));
}

static JSValue qp_dict_tp_iter(JSContext *ctx, JSValueConst self)
{
    return qp_dictiter_new(ctx, self, QP_DV_KEYS, 0);
}

/* _PyDict_Or */
static JSValue qp_dict_or(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue res;

    if (!qp_anydict_check(ctx, self) || !qp_anydict_check(ctx, other))
        return qp_not_implemented(ctx);
    res = qp_anydict_copy_as(ctx, self, qp_frozendict_check(self));
    if (JS_IsException(res))
        return res;
    if (qp_dict_update_arg(ctx, res, other) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* _PyDict_IOr */
static JSValue qp_dict_ior(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (qp_dict_update_arg(ctx, self, other) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

static JSValue qp_frozendict_or(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (qp_frozendict_check_exact(ctx, self)) {
        /* frozendict() | frozendict(...) => frozendict(...) */
        if (QP_DICT(self)->core.used == 0 && qp_frozendict_check_exact(ctx, other))
            return JS_DupValue(ctx, other);
        /* frozendict(...) | frozendict() => frozendict(...) */
        if (qp_anydict_check_exact(ctx, other) && QP_DICT(other)->core.used == 0)
            return JS_DupValue(ctx, self);
    }
    return qp_dict_or(ctx, self, other);
}

/* frozendict_hash (cached; frozenset-like combination of the (key, value)
   pair hashes, see dictcore). */
static Py_hash_t qp_frozendict_hash(JSContext *ctx, JSValueConst self)
{
    QPDict *d = QP_DICT(self);
    Py_hash_t h;

    if (d->hash != -1)
        return d->hash;
    if (js_check_stack_overflow(ctx->rt, 0)) {
        qp_raise(ctx, QP_EXC(ctx, RecursionError),
                 "maximum recursion depth exceeded while hashing a frozendict");
        return -1;
    }
    h = qp_dictcore_frozendict_hash(ctx, &d->core, qp_dict_value_hash_cb, NULL);
    if (h == -1)
        return -1;
    d->hash = h;
    return h;
}

/* Generic type_call for a subtype reaching a builtin vectorcall. */
static JSValue qp_dict_type_call(JSContext *ctx, QPType *t, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    JSValue obj;

    obj = t->slots.tp_new(ctx, t, args, nargsf, kwnames);
    if (JS_IsException(obj) || !qp_typecheck(ctx, obj, t) || !t->slots.tp_init)
        return obj;
    if (t->slots.tp_init(ctx, obj, args, nargsf, kwnames) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue qp_dict_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    /* args/kwds are consumed by tp_init */
    return qp_dict_alloc(ctx, subtype, 0, 0);
}

static int qp_dict_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           size_t nargsf, JSValueConst kwnames)
{
    return qp_dict_update_common(ctx, self, args, (int)qp_nargs(nargsf), kwnames, "dict");
}

static JSValue qp_dict_vectorcall(JSContext *ctx, JSValueConst type, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    QPType *t = qp_type_from_value(type);
    int nargs = (int)qp_nargs(nargsf);
    JSValue self;

    if (t != QP_TYPE(ctx, dict))
        return qp_dict_type_call(ctx, t, args, nargsf, kwnames);
    if (qp_arg_check_positional(ctx, "dict", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    self = qp_dict_alloc(ctx, t, 0, 0);
    if (JS_IsException(self))
        return self;
    if (qp_dict_update_common(ctx, self, args, nargs, kwnames, "dict") < 0) {
        JS_FreeValue(ctx, self);
        return JS_EXCEPTION;
    }
    return self;
}

static JSValue qp_frozendict_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    JSValue d = qp_dict_alloc(ctx, subtype, 0, 0);
    if (JS_IsException(d))
        return d;
    if (qp_dict_update_common(ctx, d, args, (int)qp_nargs(nargsf), kwnames, "frozendict") < 0) {
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    return d;
}

static JSValue qp_frozendict_vectorcall(JSContext *ctx, JSValueConst type, JSValueConst *args,
                                        size_t nargsf, JSValueConst kwnames)
{
    QPType *t = qp_type_from_value(type);
    int nargs = (int)qp_nargs(nargsf);

    if (t != QP_TYPE(ctx, frozendict))
        return qp_dict_type_call(ctx, t, args, nargsf, kwnames);
    if (qp_arg_check_positional(ctx, "frozendict", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    /* frozendict(frozendict) returns the same object unmodified */
    if (nargs == 1 && qp_dict_nkw(kwnames) == 0 && qp_frozendict_check_exact(ctx, args[0]))
        return JS_DupValue(ctx, args[0]);
    return qp_frozendict_tp_new(ctx, t, args, nargsf, kwnames);
}

/* ---- methods ------------------------------------------------------------ */

static JSValue qp_dict_m_contains(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    int r = qp_dict_contains_impl(ctx, self, args[0]);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

static JSValue qp_dict_m_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_dict_subscript(ctx, self, args[0]);
}

static Py_ssize_t qp_dict_sizeof_impl(JSContext *ctx, JSValueConst self)
{
    const qp_dictkeys *k = QP_DICT(self)->core.keys;
    Py_ssize_t res = qp_frozendict_check(self) ? QP_FROZENDICT_BASICSIZE : QP_DICT_BASICSIZE;

    if (k->log2_size != 0) {    /* not the shared empty table */
        Py_ssize_t usable = (((Py_ssize_t)1 << k->log2_size) << 1) / 3;
        res += QP_DICTKEYS_HDRSIZE + ((Py_ssize_t)1 << k->log2_index_bytes) +
            usable * (k->unicode ? QP_DICT_ENTRY_UNICODE : QP_DICT_ENTRY_GENERAL);
    }
    return res;
}

static JSValue qp_dict_m_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_int64(ctx, qp_dict_sizeof_impl(ctx, self));
}

static JSValue qp_dict_m_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue v;
    int r;

    if (qp_arg_check_positional(ctx, "get", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    r = qp_dict_lookup(ctx, self, args[0], &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return v;
    return nargs > 1 ? JS_DupValue(ctx, args[1]) : JS_UNDEFINED;
}

static JSValue qp_dict_m_setdefault(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "setdefault", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    return qp_dict_setdefault_impl(ctx, self, args[0], nargs > 1 ? args[1] : JS_UNDEFINED);
}

/* dict_pop_default */
static JSValue qp_dict_m_pop(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue res;
    int r;

    if (qp_arg_check_positional(ctx, "pop", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    r = qp_dict_pop_impl(ctx, self, args[0], &res);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return res;
    if (nargs > 1)
        return JS_DupValue(ctx, args[1]);
    return qp_raise_key_error(ctx, args[0]);
}

static JSValue qp_dict_m_popitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    JSValue kv[2];

    if (!qp_dictcore_popitem(ctx, &QP_DICT(self)->core, &kv[0], &kv[1]))
        return qp_raise(ctx, QP_EXC(ctx, KeyError), "popitem(): dictionary is empty");
    return qp_tuple_new_steal(ctx, 2, kv);
}

static JSValue qp_dict_m_keys(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    return qp_dictview_new(ctx, self, magic);
}

static JSValue qp_dict_m_update(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (qp_dict_update_common(ctx, self, args, nargs, kwnames, "update") < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_dict_m_fromkeys(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "fromkeys", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    return qp_dict_fromkeys_impl(ctx, self, args[0], nargs > 1 ? args[1] : JS_UNDEFINED);
}

static JSValue qp_dict_m_clear(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    qp_dictcore_clear(ctx, &QP_DICT(self)->core);
    return JS_UNDEFINED;
}

static JSValue qp_dict_m_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    return qp_anydict_copy_as(ctx, self, 0);
}

static JSValue qp_frozendict_m_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    if (qp_frozendict_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    return qp_anydict_copy_as(ctx, self, 1);
}

static JSValue qp_dict_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    return qp_dictiter_new(ctx, self, QP_DV_KEYS, 1);
}

static JSValue qp_dict_m_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

/* frozendict.__getnewargs__: (dict(self),) */
static JSValue qp_frozendict_m_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    JSValue d, res;

    d = qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, dict)), self);
    if (JS_IsException(d))
        return d;
    res = qp_tuple_new_steal(ctx, 1, &d);
    return res;
}

#define QP_DICT_GETITEM_DOC "__getitem__($self, key, /)\n--\n\nReturn self[key]."
#define QP_DICT_CONTAINS_DOC "__contains__($self, key, /)\n--\n\n" \
    "True if the dictionary has the specified key, else False."
#define QP_DICT_SIZEOF_DOC "__sizeof__($self, /)\n--\n\nReturn the size of the dict in memory, in bytes."
#define QP_DICT_GET_DOC "get($self, key, default=None, /)\n--\n\n" \
    "Return the value for key if key is in the dictionary, else default."
#define QP_DICT_KEYS_DOC "keys($self, /)\n--\n\n" \
    "Return a set-like object providing a view on the dict's keys."
#define QP_DICT_ITEMS_DOC "items($self, /)\n--\n\n" \
    "Return a set-like object providing a view on the dict's items."
#define QP_DICT_VALUES_DOC "values($self, /)\n--\n\n" \
    "Return an object providing a view on the dict's values."
#define QP_DICT_FROMKEYS_DOC "fromkeys($type, iterable, value=None, /)\n--\n\n" \
    "Create a new dictionary with keys from iterable and values set to value."
#define QP_DICT_REVERSED_DOC "__reversed__($self, /)\n--\n\n" \
    "Return a reverse iterator over the dict keys."

static const QPMethodDef qp_dict_methods[] = {
    { "__contains__", qp_dict_m_contains, QP_METH_O | QP_METH_COEXIST, 0, QP_DICT_CONTAINS_DOC },
    { "__getitem__", qp_dict_m_getitem, QP_METH_O | QP_METH_COEXIST, 0, QP_DICT_GETITEM_DOC },
    { "__sizeof__", qp_dict_m_sizeof, QP_METH_NOARGS, 0, QP_DICT_SIZEOF_DOC },
    { "get", qp_dict_m_get, QP_METH_FASTCALL, 0, QP_DICT_GET_DOC },
    { "setdefault", qp_dict_m_setdefault, QP_METH_FASTCALL, 0,
      "setdefault($self, key, default=None, /)\n--\n\n"
      "Insert key with a value of default if key is not in the dictionary.\n\n"
      "Return the value for key if key is in the dictionary, else default." },
    { "pop", qp_dict_m_pop, QP_METH_FASTCALL, 0,
      "pop($self, key, default=<unrepresentable>, /)\n--\n\n"
      "D.pop(k[,d]) -> v, remove specified key and return the corresponding value.\n\n"
      "If the key is not found, return the default if given; otherwise,\n"
      "raise a KeyError." },
    { "popitem", qp_dict_m_popitem, QP_METH_NOARGS, 0,
      "popitem($self, /)\n--\n\n"
      "Remove and return a (key, value) pair as a 2-tuple.\n\n"
      "Pairs are returned in LIFO (last-in, first-out) order.\n"
      "Raises KeyError if the dict is empty." },
    { "keys", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_KEYS, QP_DICT_KEYS_DOC },
    { "items", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_ITEMS, QP_DICT_ITEMS_DOC },
    { "values", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_VALUES, QP_DICT_VALUES_DOC },
    { "update", qp_dict_m_update, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "D.update([E, ]**F) -> None.  Update D from mapping/iterable E and F.\n"
      "If E is present and has a .keys() method, then does:  for k in E.keys(): D[k] = E[k]\n"
      "If E is present and lacks a .keys() method, then does:  for k, v in E: D[k] = v\n"
      "In either case, this is followed by: for k in F:  D[k] = F[k]" },
    { "fromkeys", qp_dict_m_fromkeys, QP_METH_FASTCALL | QP_METH_CLASS, 0, QP_DICT_FROMKEYS_DOC },
    { "clear", qp_dict_m_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nRemove all items from the dict." },
    { "copy", qp_dict_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a shallow copy of the dict." },
    { "__reversed__", qp_dict_m_reversed, QP_METH_NOARGS, 0, QP_DICT_REVERSED_DOC },
    { "__class_getitem__", qp_dict_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "dicts are generic over two types, signifying (respectively) the types of their keys and values" },
    { NULL },
};

static const QPMethodDef qp_frozendict_methods[] = {
    { "__contains__", qp_dict_m_contains, QP_METH_O | QP_METH_COEXIST, 0, QP_DICT_CONTAINS_DOC },
    { "__getitem__", qp_dict_m_getitem, QP_METH_O | QP_METH_COEXIST, 0, QP_DICT_GETITEM_DOC },
    { "__sizeof__", qp_dict_m_sizeof, QP_METH_NOARGS, 0, QP_DICT_SIZEOF_DOC },
    { "get", qp_dict_m_get, QP_METH_FASTCALL, 0, QP_DICT_GET_DOC },
    { "keys", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_KEYS, QP_DICT_KEYS_DOC },
    { "items", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_ITEMS, QP_DICT_ITEMS_DOC },
    { "values", qp_dict_m_keys, QP_METH_NOARGS, QP_DV_VALUES, QP_DICT_VALUES_DOC },
    { "fromkeys", qp_dict_m_fromkeys, QP_METH_FASTCALL | QP_METH_CLASS, 0, QP_DICT_FROMKEYS_DOC },
    { "copy", qp_frozendict_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a shallow copy of the frozendict." },
    { "__reversed__", qp_dict_m_reversed, QP_METH_NOARGS, 0, QP_DICT_REVERSED_DOC },
    { "__class_getitem__", qp_dict_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "frozendicts are generic over two types, signifying (respectively) the types of the frozendict's keys and values" },
    { "__getnewargs__", qp_frozendict_m_getnewargs, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPTypeSpec qp_dict_spec = {
    .name = "dict",
    .doc = "dict() -> new empty dictionary\n"
           "dict(mapping) -> new dictionary initialized from a mapping object's\n"
           "    (key, value) pairs\n"
           "dict(iterable) -> new dictionary initialized as if via:\n"
           "    d = {}\n"
           "    for k, v in iterable:\n"
           "        d[k] = v\n"
           "dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
           "    in the keyword argument list.  For example:  dict(one=1, two=2)",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_DICT_SUBCLASS |
             QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_MAPPING | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT,
    .slots = {
        .tp_repr = qp_dict_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_vectorcall = qp_dict_vectorcall,
        .tp_richcompare = qp_dict_richcompare,
        .tp_iter = qp_dict_tp_iter,
        .tp_init = qp_dict_tp_init,
        .tp_new = qp_dict_tp_new,
        .nb = {
            .nb_or = qp_dict_or,
            .nb_inplace_or = qp_dict_ior,
        },
        .sq = {
            .sq_contains = qp_dict_sq_contains,
        },
        .mp = {
            .mp_length = qp_dict_length,
            .mp_subscript = qp_dict_subscript,
            .mp_ass_subscript = qp_dict_ass_subscript,
        },
    },
    .methods = qp_dict_methods,
};

static const QPTypeSpec qp_frozendict_spec = {
    .name = "frozendict",
    .doc = "frozendict() -> new empty immutable dictionary\n"
           "frozendict(mapping) -> new immutable dictionary initialized from a mapping\n"
           "    object's (key, value) pairs\n"
           "frozendict(iterable) -> new immutable dictionary initialized as if via:\n"
           "    d = {}\n"
           "    for k, v in iterable:\n"
           "        d[k] = v\n"
           "    d = frozendict(d)\n"
           "frozendict(**kwargs) -> new immutable dictionary initialized with the name=value\n"
           "    pairs in the keyword argument list.  For example:  frozendict(one=1, two=2)",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF |
             QP_TPFLAGS_MAPPING | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_FROZENDICT,
    .slots = {
        .tp_repr = qp_frozendict_repr,
        .tp_hash = qp_frozendict_hash,
        .tp_vectorcall = qp_frozendict_vectorcall,
        .tp_richcompare = qp_dict_richcompare,
        .tp_iter = qp_dict_tp_iter,
        .tp_new = qp_frozendict_tp_new,
        .nb = {
            .nb_or = qp_frozendict_or,
        },
        .sq = {
            .sq_contains = qp_dict_sq_contains,
        },
        .mp = {
            .mp_length = qp_dict_length,
            .mp_subscript = qp_dict_subscript,
        },
    },
    .methods = qp_frozendict_methods,
};

/* ========================================================================= */
/* Dict iterators                                                            */
/* ========================================================================= */

static QPType *qp_dictiter_type(JSContext *ctx, int kind, int reversed)
{
    switch (kind) {
    case QP_DV_KEYS:
        return reversed ? QP_TYPE(ctx, dict_reversekeyiterator) : QP_TYPE(ctx, dict_keyiterator);
    case QP_DV_VALUES:
        return reversed ? QP_TYPE(ctx, dict_reversevalueiterator) : QP_TYPE(ctx, dict_valueiterator);
    default:
        return reversed ? QP_TYPE(ctx, dict_reverseitemiterator) : QP_TYPE(ctx, dict_itemiterator);
    }
}

/* dictiter_new */
static JSValue qp_dictiter_new(JSContext *ctx, JSValueConst dict, int kind, int reversed)
{
    QPDictIter *it;
    JSValue obj;

    it = js_malloc(ctx, sizeof(QPDictIter));
    if (!it)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(qp_dictiter_type(ctx, kind, reversed)),
                                 JS_CLASS_PY_DICT_ITER);
    if (JS_IsException(obj)) {
        js_free(ctx, it);
        return obj;
    }
    it->kind = kind;
    it->reversed = reversed;
    it->dict = JS_DupValue(ctx, dict);
    qp_dictcore_iter_init(&QP_DICT(dict)->core, &it->st, reversed);
    JS_VALUE_GET_OBJ(obj)->u.opaque = it;
    return obj;
}

static JSValue qp_dictiter_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

/* The item of entry `ix` for an iterator kind (new ref). */
static JSValue qp_dictiter_item(JSContext *ctx, const qp_dictentry *ep, int kind)
{
    JSValueConst kv[2];

    if (kind == QP_DV_KEYS)
        return JS_DupValue(ctx, ep->key);
    if (kind == QP_DV_VALUES)
        return JS_DupValue(ctx, ep->value);
    kv[0] = ep->key;
    kv[1] = ep->value;
    return qp_tuple_pack(ctx, 2, kv);
}

/* dictiter_iternext* / dictreviter_iternext */
static JSValue qp_dictiter_next(JSContext *ctx, JSValueConst self)
{
    QPDictIter *it = QP_DICTITER(self);
    QPDict *d;
    Py_ssize_t ix;
    JSValue dict;
    int r;

    if (JS_IsUndefined(it->dict))
        return QP_NULL;
    d = QP_DICT(it->dict);
    r = qp_dictcore_iter_next(&d->core, &it->st, it->reversed, &ix);
    if (r == 1)
        return qp_dictiter_item(ctx, &d->core.keys->entries[ix], it->kind);
    if (r == QP_DICTITER_CHANGED_SIZE)
        return qp_raise_runtime_error(ctx, "dictionary changed size during iteration");
    dict = it->dict;
    it->dict = JS_UNDEFINED;
    JS_FreeValue(ctx, dict);
    if (r == QP_DICTITER_KEYS_CHANGED)
        return qp_raise_runtime_error(ctx, "dictionary keys changed during iteration");
    return QP_NULL;
}

static JSValue qp_dictiter_m_length_hint(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    QPDictIter *it = QP_DICTITER(self);
    Py_ssize_t len = 0;

    if (!JS_IsUndefined(it->dict))
        len = qp_dictcore_iter_length_hint(&QP_DICT(it->dict)->core, &it->st);
    return qp_long_from_int64(ctx, len);
}

/* dictiter_reduce: (iter, (list of the remaining items,)) without
   consuming the iterator. */
static JSValue qp_dictiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPDictIter *it = QP_DICTITER(self);
    JSValue list, iter_fn, tup, res;
    JSValueConst pair[2];

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    if (!JS_IsUndefined(it->dict)) {
        JSValue dict = JS_DupValue(ctx, it->dict);
        qp_dictcore *core = &QP_DICT(dict)->core;
        qp_dictcore_iter st = it->st;
        Py_ssize_t ix;
        for (;;) {
            JSValue item;
            int r = qp_dictcore_iter_next(core, &st, it->reversed, &ix);
            if (r == 0)
                break;
            if (r < 0) {
                JS_FreeValue(ctx, dict);
                JS_FreeValue(ctx, list);
                return qp_raise_runtime_error(ctx, r == QP_DICTITER_CHANGED_SIZE ?
                                              "dictionary changed size during iteration" :
                                              "dictionary keys changed during iteration");
            }
            item = qp_dictiter_item(ctx, &core->keys->entries[ix], it->kind);
            if (JS_IsException(item) || qp_list_append(ctx, list, item) < 0) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, dict);
                JS_FreeValue(ctx, list);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, dict);
    }
    iter_fn = qp_eval_get_builtin(ctx, QP_ATOM(ctx, iter));
    if (JS_IsException(iter_fn)) {
        JS_FreeValue(ctx, list);
        return JS_EXCEPTION;
    }
    tup = qp_tuple_new_steal(ctx, 1, &list);
    if (JS_IsException(tup)) {
        JS_FreeValue(ctx, iter_fn);
        return tup;
    }
    pair[0] = iter_fn;
    pair[1] = tup;
    res = qp_tuple_pack(ctx, 2, pair);
    JS_FreeValue(ctx, iter_fn);
    JS_FreeValue(ctx, tup);
    return res;
}

static const QPMethodDef qp_dictiter_methods[] = {
    { "__length_hint__", qp_dictiter_m_length_hint, QP_METH_NOARGS, 0,
      "Private method returning an estimate of len(list(it))." },
    { "__reduce__", qp_dictiter_m_reduce, QP_METH_NOARGS, 0,
      "Return state information for pickling." },
    { NULL },
};

#define QP_DICTITER_SPEC(cname, tpname)                                       \
static const QPTypeSpec cname = {                                             \
    .name = tpname,                                                           \
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION |         \
             QP_TPFLAGS_IMMUTABLETYPE,                                        \
    .base = -1,                                                               \
    .exc_base = -1,                                                           \
    .instance_class_id = JS_CLASS_PY_DICT_ITER,                               \
    .slots = {                                                                \
        .tp_iter = qp_dictiter_iter,                                          \
        .tp_iternext = qp_dictiter_next,                                      \
    },                                                                        \
    .methods = qp_dictiter_methods,                                           \
};

QP_DICTITER_SPEC(qp_dict_keyiterator_spec, "dict_keyiterator")
QP_DICTITER_SPEC(qp_dict_valueiterator_spec, "dict_valueiterator")
QP_DICTITER_SPEC(qp_dict_itemiterator_spec, "dict_itemiterator")
QP_DICTITER_SPEC(qp_dict_reversekeyiterator_spec, "dict_reversekeyiterator")
QP_DICTITER_SPEC(qp_dict_reversevalueiterator_spec, "dict_reversevalueiterator")
QP_DICTITER_SPEC(qp_dict_reverseitemiterator_spec, "dict_reverseitemiterator")

/* ========================================================================= */
/* Dict views                                                                */
/* ========================================================================= */

static JSValue qp_dictview_alloc(JSContext *ctx, QPType *t, JSValueConst mapping, int kind)
{
    QPDictView *v;
    JSValue obj;

    v = js_malloc(ctx, sizeof(QPDictView));
    if (!v)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), JS_CLASS_PY_DICT_VIEW);
    if (JS_IsException(obj)) {
        js_free(ctx, v);
        return obj;
    }
    v->kind = kind;
    v->mapping = JS_DupValue(ctx, mapping);
    JS_VALUE_GET_OBJ(obj)->u.opaque = v;
    return obj;
}

/* _PyDictView_New */
static JSValue qp_dictview_new(JSContext *ctx, JSValueConst mapping, int kind)
{
    QPType *t = kind == QP_DV_KEYS ? QP_TYPE(ctx, dict_keys) :
        kind == QP_DV_VALUES ? QP_TYPE(ctx, dict_values) : QP_TYPE(ctx, dict_items);

    if (!qp_anydict_check(ctx, mapping)) {
        JSValue tn = qp_dict_tp_name(ctx, qp_type_of(ctx, mapping));
        if (JS_IsException(tn))
            return tn;
        qp_raise_type_error(ctx, "%s() requires a dict argument, not '%U'", t->tp_name, tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    return qp_dictview_alloc(ctx, t, mapping, kind);
}

static Py_ssize_t qp_dictview_len(JSContext *ctx, JSValueConst self)
{
    return QP_DICT(QP_DICTVIEW(self)->mapping)->core.used;
}

static JSValue qp_dictview_iter(JSContext *ctx, JSValueConst self)
{
    QPDictView *v = QP_DICTVIEW(self);
    return qp_dictiter_new(ctx, v->mapping, v->kind, 0);
}

static JSValue qp_dictview_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPDictView *v = QP_DICTVIEW(self);
    return qp_dictiter_new(ctx, v->mapping, v->kind, 1);
}

static JSValue qp_dictview_get_mapping(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_mappingproxy_new(ctx, QP_DICTVIEW(self)->mapping);
}

static int qp_dictkeys_contains(JSContext *ctx, JSValueConst self, JSValueConst obj)
{
    return qp_dict_contains_impl(ctx, QP_DICTVIEW(self)->mapping, obj);
}

static int qp_dictitems_contains(JSContext *ctx, JSValueConst self, JSValueConst obj)
{
    JSValue key, value, found;
    int r;

    if (!qp_tuple_check(ctx, obj) || QP_TUPLE_SIZE(obj) != 2)
        return 0;
    key = JS_DupValue(ctx, QP_TUPLE_ITEMS(obj)[0]);
    value = JS_DupValue(ctx, QP_TUPLE_ITEMS(obj)[1]);
    r = qp_dict_lookup(ctx, QP_DICTVIEW(self)->mapping, key, &found);
    if (r == 1) {
        r = qp_richcompare_bool(ctx, found, value, QP_CMP_EQ);
        JS_FreeValue(ctx, found);
    }
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, value);
    return r;
}

/* all_contained_in: every item of self is in other (iterating self). */
static int qp_dictview_all_contained_in(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue iter, next;
    int ok = 1;

    iter = qp_iter(ctx, self);
    if (JS_IsException(iter))
        return -1;
    for (;;) {
        next = qp_next(ctx, iter);
        if (JS_IsException(next)) {
            ok = -1;
            break;
        }
        if (QP_IS_NULL(next))
            break;
        ok = qp_contains(ctx, other, next);
        JS_FreeValue(ctx, next);
        if (ok <= 0)
            break;
    }
    JS_FreeValue(ctx, iter);
    return ok;
}

static JSValue qp_dictview_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other, int op)
{
    Py_ssize_t len_self, len_other;
    int ok;

    if (!qp_anyset_check(ctx, other) && !qp_dictviewset_check(other))
        return qp_not_implemented(ctx);
    len_self = qp_len(ctx, self);
    if (len_self < 0)
        return JS_EXCEPTION;
    len_other = qp_len(ctx, other);
    if (len_other < 0)
        return JS_EXCEPTION;
    ok = 0;
    switch (op) {
    case QP_CMP_NE:
    case QP_CMP_EQ:
        if (len_self == len_other)
            ok = qp_dictview_all_contained_in(ctx, self, other);
        if (op == QP_CMP_NE && ok >= 0)
            ok = !ok;
        break;
    case QP_CMP_LT:
        if (len_self < len_other)
            ok = qp_dictview_all_contained_in(ctx, self, other);
        break;
    case QP_CMP_LE:
        if (len_self <= len_other)
            ok = qp_dictview_all_contained_in(ctx, self, other);
        break;
    case QP_CMP_GT:
        if (len_self > len_other)
            ok = qp_dictview_all_contained_in(ctx, other, self);
        break;
    case QP_CMP_GE:
        if (len_self >= len_other)
            ok = qp_dictview_all_contained_in(ctx, other, self);
        break;
    }
    if (ok < 0)
        return JS_EXCEPTION;
    return qp_bool_new(ok);
}

/* dictview_repr: "dict_keys([...])", "..." when recursive. */
static JSValue qp_dictview_repr(JSContext *ctx, JSValueConst self)
{
    QPType *t = qp_type_of(ctx, self);
    JSValue seq, res;
    int rc;

    rc = qp_repr_enter(ctx, self);
    if (rc != 0)
        return rc > 0 ? JS_NewString(ctx, "...") : JS_EXCEPTION;
    seq = qp_list_from_iterable(ctx, self);
    if (JS_IsException(seq)) {
        qp_repr_leave(ctx, self);
        return seq;
    }
    res = qp_str_from_format(ctx, "%s(%R)", t->tp_name ? t->tp_name : "dict_view", seq);
    JS_FreeValue(ctx, seq);
    qp_repr_leave(ctx, self);
    return res;
}

/* dictviews_to_set: set(self), using the dict itself for keys views of
   exact dicts. */
static JSValue qp_dictviews_to_set(JSContext *ctx, JSValueConst self)
{
    JSValueConst left = self;

    if (qp_dictview_kind_is(self, QP_DV_KEYS)) {
        JSValueConst dict = QP_DICTVIEW(self)->mapping;
        if (qp_anydict_check_exact(ctx, dict))
            left = dict;
    }
    return qp_set_new(ctx, left);
}

static JSValue qp_dictviews_sub(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue result, tmp;

    result = qp_dictviews_to_set(ctx, self);
    if (JS_IsException(result))
        return result;
    tmp = qp_dict_call_method_cstr(ctx, result, "difference_update", &other, 1);
    if (JS_IsException(tmp)) {
        JS_FreeValue(ctx, result);
        return tmp;
    }
    JS_FreeValue(ctx, tmp);
    return result;
}

/* _PyDictView_Intersect */
static JSValue qp_dictviews_and(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValueConst tmp;
    JSValue result, it, key;
    Py_ssize_t len_self;
    int rv, is_keys;

    /* the interpreter swaps parameters when the dict view is on the right */
    if (!qp_dictviewset_check(self)) {
        tmp = other;
        other = self;
        self = tmp;
    }
    len_self = qp_dictview_len(ctx, self);

    /* if other is a set and self is smaller than other, reuse set
       intersection logic */
    if (qp_type_of(ctx, other) == QP_TYPE(ctx, set)) {
        Py_ssize_t len_other = qp_len(ctx, other);
        if (len_other < 0)
            return JS_EXCEPTION;
        if (len_self <= len_other)
            return qp_dict_call_method_cstr(ctx, other, "intersection", &self, 1);
    }

    /* if other is another dict view, and it is bigger than self, swap them */
    if (qp_dictviewset_check(other)) {
        Py_ssize_t len_other = qp_dictview_len(ctx, other);
        if (len_other > len_self) {
            tmp = other;
            other = self;
            self = tmp;
        }
    }

    result = qp_set_new(ctx, QP_NULL);
    if (JS_IsException(result))
        return result;
    it = qp_iter(ctx, other);
    if (JS_IsException(it)) {
        JS_FreeValue(ctx, result);
        return it;
    }
    is_keys = qp_dictview_kind_is(self, QP_DV_KEYS);
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key))
            goto error;
        if (QP_IS_NULL(key))
            break;
        rv = is_keys ? qp_dictkeys_contains(ctx, self, key) : qp_dictitems_contains(ctx, self, key);
        if (rv > 0)
            rv = qp_set_add(ctx, result, key) < 0 ? -1 : 1;
        JS_FreeValue(ctx, key);
        if (rv < 0)
            goto error;
    }
    JS_FreeValue(ctx, it);
    return result;
error:
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

static JSValue qp_dictviews_or(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue result = qp_dictviews_to_set(ctx, self);
    if (JS_IsException(result))
        return result;
    if (qp_set_update(ctx, result, other) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* dictitems_xor_lock_held: symmetric difference of two items views. */
static JSValue qp_dictitems_xor(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValueConst d2 = QP_DICTVIEW(other)->mapping;
    qp_dictcore *c2 = &QP_DICT(d2)->core;
    JSValue temp_dict, result_set, remaining;
    Py_ssize_t pos = 0, ix;

    temp_dict = qp_anydict_copy_as(ctx, QP_DICTVIEW(self)->mapping, 0);
    if (JS_IsException(temp_dict))
        return temp_dict;
    result_set = qp_set_new(ctx, QP_NULL);
    if (JS_IsException(result_set)) {
        JS_FreeValue(ctx, temp_dict);
        return result_set;
    }
    while (qp_dictcore_next(c2, &pos, &ix)) {
        qp_dictentry *ep = &c2->keys->entries[ix];
        JSValue key = JS_DupValue(ctx, ep->key);
        JSValue val2 = JS_DupValue(ctx, ep->value);
        JSValue val1;
        Py_hash_t hash = ep->hash;
        Py_ssize_t lix;
        int to_delete;

        lix = qp_dictcore_lookup(ctx, &QP_DICT(temp_dict)->core, key, hash, QP_DICT_EQ, &val1);
        if (lix == QP_DKIX_ERROR) {
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, val2);
            goto error;
        }
        if (lix < 0 || QP_IS_NULL(val1)) {
            to_delete = 0;
        } else {
            val1 = JS_DupValue(ctx, val1);
            to_delete = qp_richcompare_bool(ctx, val1, val2, QP_CMP_EQ);
            JS_FreeValue(ctx, val1);
            if (to_delete < 0) {
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, val2);
                goto error;
            }
        }
        if (to_delete) {
            int r;
            JS_FreeValue(ctx, val2);
            r = qp_dictcore_delitem(ctx, &QP_DICT(temp_dict)->core, key, hash, QP_DICT_EQ);
            JS_FreeValue(ctx, key);
            if (r < 0)
                goto error;
        } else {
            JSValue kv[2] = { key, val2 };
            JSValue pair = qp_tuple_new_steal(ctx, 2, kv);
            int r;
            if (JS_IsException(pair))
                goto error;
            r = qp_set_add(ctx, result_set, pair);
            JS_FreeValue(ctx, pair);
            if (r < 0)
                goto error;
        }
    }
    remaining = qp_dictview_new(ctx, temp_dict, QP_DV_ITEMS);
    if (JS_IsException(remaining))
        goto error;
    if (qp_set_update(ctx, result_set, remaining) < 0) {
        JS_FreeValue(ctx, remaining);
        goto error;
    }
    JS_FreeValue(ctx, remaining);
    JS_FreeValue(ctx, temp_dict);
    return result_set;
error:
    JS_FreeValue(ctx, temp_dict);
    JS_FreeValue(ctx, result_set);
    return JS_EXCEPTION;
}

static JSValue qp_dictviews_xor(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue result, tmp;

    if (qp_dictview_kind_is(self, QP_DV_ITEMS) && qp_dictview_kind_is(other, QP_DV_ITEMS))
        return qp_dictitems_xor(ctx, self, other);
    result = qp_dictviews_to_set(ctx, self);
    if (JS_IsException(result))
        return result;
    tmp = qp_dict_call_method_cstr(ctx, result, "symmetric_difference_update", &other, 1);
    if (JS_IsException(tmp)) {
        JS_FreeValue(ctx, result);
        return tmp;
    }
    JS_FreeValue(ctx, tmp);
    return result;
}

/* dictviews_isdisjoint */
static JSValue qp_dictviews_m_isdisjoint(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst other = args[0], tmp;
    JSValue it, item;
    int contains;

    if (JS_VALUE_GET_TAG(other) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(other) == JS_VALUE_GET_PTR(self))
        return qp_bool_new(qp_dictview_len(ctx, self) == 0);

    /* Iterate over the shorter object (only if other is a set, because
       PySequence_Contains may be expensive otherwise) */
    if (qp_anyset_check(ctx, other) || qp_dictviewset_check(other)) {
        Py_ssize_t len_self = qp_dictview_len(ctx, self);
        Py_ssize_t len_other = qp_len(ctx, other);
        if (len_other == -1)
            return JS_EXCEPTION;
        if (len_other > len_self) {
            tmp = other;
            other = self;
            self = tmp;
        }
    }
    it = qp_iter(ctx, other);
    if (JS_IsException(it))
        return it;
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, it);
            return item;
        }
        if (QP_IS_NULL(item))
            break;
        contains = qp_contains(ctx, self, item);
        JS_FreeValue(ctx, item);
        if (contains == -1) {
            JS_FreeValue(ctx, it);
            return JS_EXCEPTION;
        }
        if (contains) {
            JS_FreeValue(ctx, it);
            return qp_bool_new(0);
        }
    }
    JS_FreeValue(ctx, it);
    return qp_bool_new(1);
}

static const QPMethodDef qp_dictkeys_methods[] = {
    { "isdisjoint", qp_dictviews_m_isdisjoint, QP_METH_O, 0,
      "Return True if the view and the given iterable have a null intersection." },
    { "__reversed__", qp_dictview_m_reversed, QP_METH_NOARGS, 0,
      "Return a reverse iterator over the dict keys." },
    { NULL },
};

static const QPMethodDef qp_dictitems_methods[] = {
    { "isdisjoint", qp_dictviews_m_isdisjoint, QP_METH_O, 0,
      "Return True if the view and the given iterable have a null intersection." },
    { "__reversed__", qp_dictview_m_reversed, QP_METH_NOARGS, 0,
      "Return a reverse iterator over the dict items." },
    { NULL },
};

static const QPMethodDef qp_dictvalues_methods[] = {
    { "__reversed__", qp_dictview_m_reversed, QP_METH_NOARGS, 0,
      "Return a reverse iterator over the dict values." },
    { NULL },
};

static const QPGetSetDef qp_dictview_getsets[] = {
    { "mapping", qp_dictview_get_mapping, NULL, 0, "dictionary that this view refers to" },
    { NULL },
};

static const QPTypeSpec qp_dict_keys_spec = {
    .name = "dict_keys",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT_VIEW,
    .slots = {
        .tp_repr = qp_dictview_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_richcompare = qp_dictview_richcompare,
        .tp_iter = qp_dictview_iter,
        .nb = {
            .nb_subtract = qp_dictviews_sub,
            .nb_and = qp_dictviews_and,
            .nb_xor = qp_dictviews_xor,
            .nb_or = qp_dictviews_or,
        },
        .sq = {
            .sq_length = qp_dictview_len,
            .sq_contains = qp_dictkeys_contains,
        },
    },
    .methods = qp_dictkeys_methods,
    .getsets = qp_dictview_getsets,
};

static const QPTypeSpec qp_dict_items_spec = {
    .name = "dict_items",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT_VIEW,
    .slots = {
        .tp_repr = qp_dictview_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_richcompare = qp_dictview_richcompare,
        .tp_iter = qp_dictview_iter,
        .nb = {
            .nb_subtract = qp_dictviews_sub,
            .nb_and = qp_dictviews_and,
            .nb_xor = qp_dictviews_xor,
            .nb_or = qp_dictviews_or,
        },
        .sq = {
            .sq_length = qp_dictview_len,
            .sq_contains = qp_dictitems_contains,
        },
    },
    .methods = qp_dictitems_methods,
    .getsets = qp_dictview_getsets,
};

static const QPTypeSpec qp_dict_values_spec = {
    .name = "dict_values",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT_VIEW,
    .slots = {
        .tp_repr = qp_dictview_repr,
        .tp_iter = qp_dictview_iter,
        .sq = {
            .sq_length = qp_dictview_len,
        },
    },
    .methods = qp_dictvalues_methods,
    .getsets = qp_dictview_getsets,
};

/* ========================================================================= */
/* mappingproxy (CPython Objects/descrobject.c)                              */
/* ========================================================================= */

#define QP_PROXY_MAPPING(v) (QP_DICTVIEW(v)->mapping)

static inline BOOL qp_mappingproxy_check(JSValueConst v)
{
    return qp_dictview_kind_is(v, QP_DV_PROXY);
}

/* mappingproxy_check_mapping */
static int qp_mappingproxy_check_mapping(JSContext *ctx, JSValueConst mapping)
{
    QPType *t = qp_type_of(ctx, mapping);
    if (!t->slots.mp.mp_subscript || qp_list_check(ctx, mapping) || qp_tuple_check(ctx, mapping)) {
        JSValue tn = qp_dict_tp_name(ctx, t);
        if (JS_IsException(tn))
            return -1;
        qp_raise_type_error(ctx, "mappingproxy() argument must be a mapping, not %U", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    return 0;
}

static JSValue qp_mappingproxy_new(JSContext *ctx, JSValueConst mapping)
{
    if (qp_mappingproxy_check_mapping(ctx, mapping) < 0)
        return JS_EXCEPTION;
    return qp_dictview_alloc(ctx, QP_TYPE(ctx, mappingproxy), mapping, QP_DV_PROXY);
}

static JSValue qp_mappingproxy_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                      size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "mapping", NULL };
    static const QPArgSpec spec = { "mappingproxy", kwlist, 1, 1, 0 };
    JSValueConst mapping;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, &mapping) < 0)
        return JS_EXCEPTION;
    return qp_mappingproxy_new(ctx, mapping);
}

static Py_ssize_t qp_mappingproxy_len(JSContext *ctx, JSValueConst self)
{
    return qp_len(ctx, QP_PROXY_MAPPING(self));
}

static JSValue qp_mappingproxy_getitem(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    return qp_getitem(ctx, QP_PROXY_MAPPING(self), key);
}

static JSValue qp_mappingproxy_or(JSContext *ctx, JSValueConst left, JSValueConst right)
{
    if (qp_mappingproxy_check(left))
        left = QP_PROXY_MAPPING(left);
    if (qp_mappingproxy_check(right))
        right = QP_PROXY_MAPPING(right);
    return qp_binop(ctx, left, right, QP_NB_OR);
}

static JSValue qp_mappingproxy_ior(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    return qp_raise_type_error(ctx, "'|=' is not supported by %s; use '|' instead",
                               "mappingproxy");
}

static int qp_mappingproxy_contains(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    JSValueConst m = QP_PROXY_MAPPING(self);
    if (qp_dict_check_exact(ctx, m))
        return qp_dict_contains_impl(ctx, m, key);
    return qp_contains(ctx, m, key);
}

static JSValue qp_mappingproxy_m_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst newargs[2];

    if (qp_arg_check_positional(ctx, "get", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    newargs[0] = args[0];
    newargs[1] = nargs > 1 ? args[1] : JS_UNDEFINED;
    return qp_call_method(ctx, QP_PROXY_MAPPING(self), QP_ATOM(ctx, get), newargs, 2, QP_NULL);
}

/* keys() / values() / items() / copy() / __reversed__(): forwarded. */
static JSValue qp_mappingproxy_m_forward(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    JSAtom name;
    switch (magic) {
    case 0: name = QP_ATOM(ctx, keys); break;
    case 1: name = QP_ATOM(ctx, values); break;
    case 2: name = QP_ATOM(ctx, items); break;
    case 3: name = QP_ATOM(ctx, copy); break;
    default: name = QP_ATOM(ctx, __reversed__); break;
    }
    return qp_call_method(ctx, QP_PROXY_MAPPING(self), name, NULL, 0, QP_NULL);
}

static JSValue qp_mappingproxy_iter(JSContext *ctx, JSValueConst self)
{
    return qp_iter(ctx, QP_PROXY_MAPPING(self));
}

static Py_hash_t qp_mappingproxy_hash(JSContext *ctx, JSValueConst self)
{
    return qp_hash(ctx, QP_PROXY_MAPPING(self));
}

static JSValue qp_mappingproxy_str(JSContext *ctx, JSValueConst self)
{
    return qp_str(ctx, QP_PROXY_MAPPING(self));
}

static JSValue qp_mappingproxy_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_format(ctx, "mappingproxy(%R)", QP_PROXY_MAPPING(self));
}

/* mappingproxy_richcompare: a mutable exact dict is never handed to the
   other operand's __eq__; a copy is compared instead (gh-152405).
   OrderedDict (exempted by CPython) is not a builtin type here: a dict
   copy compares the same. */
static JSValue qp_mappingproxy_richcompare(JSContext *ctx, JSValueConst self, JSValueConst w, int op)
{
    JSValueConst m = QP_PROXY_MAPPING(self);

    if (op != QP_CMP_EQ && op != QP_CMP_NE)
        return qp_not_implemented(ctx);
    if (qp_dict_check_exact(ctx, m) &&
        !(qp_anydict_check_exact(ctx, w) ||
          (qp_mappingproxy_check(w) && qp_type_of(ctx, w) == QP_TYPE(ctx, mappingproxy)))) {
        JSValue copy, res;
        copy = qp_anydict_copy_as(ctx, m, 0);
        if (JS_IsException(copy))
            return copy;
        res = qp_richcompare(ctx, copy, w, op);
        JS_FreeValue(ctx, copy);
        return res;
    }
    return qp_richcompare(ctx, m, w, op);
}

static const QPMethodDef qp_mappingproxy_methods[] = {
    { "get", qp_mappingproxy_m_get, QP_METH_FASTCALL, 0,
      "get($self, key, default=None, /)\n--\n\n"
      "Return the value for key if key is in the mapping, else default." },
    { "keys", qp_mappingproxy_m_forward, QP_METH_NOARGS, 0,
      "D.keys() -> a set-like object providing a view on D's keys" },
    { "values", qp_mappingproxy_m_forward, QP_METH_NOARGS, 1,
      "D.values() -> an object providing a view on D's values" },
    { "items", qp_mappingproxy_m_forward, QP_METH_NOARGS, 2,
      "D.items() -> a set-like object providing a view on D's items" },
    { "copy", qp_mappingproxy_m_forward, QP_METH_NOARGS, 3,
      "D.copy() -> a shallow copy of D" },
    { "__class_getitem__", qp_dict_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "mappingproxy objects are generic over two types, signifying (respectively) the types of their keys and values" },
    { "__reversed__", qp_mappingproxy_m_forward, QP_METH_NOARGS, 4,
      "D.__reversed__() -> reverse iterator" },
    { NULL },
};

static const QPTypeSpec qp_mappingproxy_spec = {
    .name = "mappingproxy",
    .doc = "mappingproxy(mapping)\n--\n\nRead-only proxy of a mapping.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_MAPPING | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT_VIEW,
    .slots = {
        .tp_repr = qp_mappingproxy_repr,
        .tp_str = qp_mappingproxy_str,
        .tp_hash = qp_mappingproxy_hash,
        .tp_richcompare = qp_mappingproxy_richcompare,
        .tp_iter = qp_mappingproxy_iter,
        .tp_new = qp_mappingproxy_tp_new,
        .nb = {
            .nb_or = qp_mappingproxy_or,
            .nb_inplace_or = qp_mappingproxy_ior,
        },
        .sq = {
            .sq_contains = qp_mappingproxy_contains,
        },
        .mp = {
            .mp_length = qp_mappingproxy_len,
            .mp_subscript = qp_mappingproxy_getitem,
        },
    },
    .methods = qp_mappingproxy_methods,
};

/* ========================================================================= */
/* Init                                                                      */
/* ========================================================================= */

static int qp_init_dicts(JSContext *ctx)
{
    static const struct {
        int id;
        const QPTypeSpec *spec;
    } types[] = {
        { QP_T_dict, &qp_dict_spec },
        { QP_T_frozendict, &qp_frozendict_spec },
        { QP_T_dict_keys, &qp_dict_keys_spec },
        { QP_T_dict_values, &qp_dict_values_spec },
        { QP_T_dict_items, &qp_dict_items_spec },
        { QP_T_dict_keyiterator, &qp_dict_keyiterator_spec },
        { QP_T_dict_valueiterator, &qp_dict_valueiterator_spec },
        { QP_T_dict_itemiterator, &qp_dict_itemiterator_spec },
        { QP_T_dict_reversekeyiterator, &qp_dict_reversekeyiterator_spec },
        { QP_T_dict_reversevalueiterator, &qp_dict_reversevalueiterator_spec },
        { QP_T_dict_reverseitemiterator, &qp_dict_reverseitemiterator_spec },
        { QP_T_mappingproxy, &qp_mappingproxy_spec },
    };
    size_t i;

    for (i = 0; i < countof(types); i++) {
        if (qp_type_ready(ctx, QP_STATE(ctx)->types[types[i].id], types[i].spec) < 0)
            return -1;
    }
    return qp_init_sets(ctx);
}
