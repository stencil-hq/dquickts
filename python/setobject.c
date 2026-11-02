/*
 * dquickts Python: set and frozenset objects (object.h section (f)).
 *
 * Derived from CPython Objects/setobject.c (PSF-2.0).
 *
 * The hash table itself (probing, resizing, ordering, mutation-during-compare
 * handling, frozenset hash) lives in python/dictcore.h (qp_setcore_*); this
 * file is the object glue: types, constructors, methods, operators,
 * comparisons, iteration and repr, ported from setobject.c.
 *
 * Payload: PY_SET / PY_FROZENSET objects hold a js_malloc'd QPSet in
 * u.opaque (never moved: the small table lives inside it).  The frozenset
 * hash cache is qp_setcore.hash (QPSet.hash is kept at -1).  Set iterators
 * are JS_CLASS_PY_DICT_ITER objects whose payload starts with
 * kind == QP_DICTITER_KIND_SET; dictobject.c routes that class's finalizer
 * and gc_mark to qp_setiter_finalizer / qp_setiter_mark below.
 */

#ifndef QP_DICTITER_KIND_SET
#define QP_DICTITER_KIND_SET 0xFF
#endif

/* Size of CPython's PySetObject on 64-bit builds (set.__sizeof__). */
#define QP_SET_BASICSIZE 200
#define QP_SET_ENTRYSIZE 16

#define QP_SC(v) (&QP_SET(v)->core)

/* Set iterator payload (CPython setiterobject).  `set` is QP_NULL once the
   iterator is exhausted (CPython drops si_set). */
typedef struct QPSetIter {
    uint8_t kind;               /* QP_DICTITER_KIND_SET, must be first */
    JSValue set;
    qp_setcore_iter st;
} QPSetIter;

static JSValue qp_set_intersection(JSContext *ctx, JSValueConst so, JSValueConst other);
static JSValue qp_set_copy_impl(JSContext *ctx, JSValueConst so);

/* ------------------------------------------------------------------------ */
/* Type checks                                                               */
/* ------------------------------------------------------------------------ */

static inline int qp_set_class_id(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return -1;
    return JS_VALUE_GET_OBJ(v)->class_id;
}

static BOOL qp_set_check(JSContext *ctx, JSValueConst v)
{
    return qp_set_class_id(v) == JS_CLASS_PY_SET;
}

static BOOL qp_frozenset_check(JSContext *ctx, JSValueConst v)
{
    return qp_set_class_id(v) == JS_CLASS_PY_FROZENSET;
}

static BOOL qp_anyset_check(JSContext *ctx, JSValueConst v)
{
    int id = qp_set_class_id(v);
    return id == JS_CLASS_PY_SET || id == JS_CLASS_PY_FROZENSET;
}

static BOOL qp_set_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_set_check(ctx, v) && qp_type_of(ctx, v) == QP_TYPE(ctx, set);
}

static BOOL qp_frozenset_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_frozenset_check(ctx, v) && qp_type_of(ctx, v) == QP_TYPE(ctx, frozenset);
}

static BOOL qp_anyset_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_set_check_exact(ctx, v) || qp_frozenset_check_exact(ctx, v);
}

/* PyAnyDict_CheckExact: exact dict or exact frozendict. */
static BOOL qp_set_anydict_check_exact(JSContext *ctx, JSValueConst v)
{
    QPType *t;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    t = qp_type_of(ctx, v);
    return t == QP_TYPE(ctx, dict) || t == QP_TYPE(ctx, frozendict);
}

static Py_ssize_t qp_set_size(JSValueConst set)
{
    return QP_SC(set)->used;
}

/* ------------------------------------------------------------------------ */
/* Entry-level helpers                                                       */
/* ------------------------------------------------------------------------ */

/* Equality callback for the core: PyObject_RichCompareBool(a, b, Py_EQ). */
static int qp_set_eq_cb(JSContext *ctx, JSValueConst a, JSValueConst b, void *opaque)
{
    return qp_richcompare_bool(ctx, a, b, QP_CMP_EQ);
}

/* set_unhashable_type: rewrap a pending exact TypeError as
   "cannot use '%T' as a set element (%S)". */
static void qp_set_unhashable_type(JSContext *ctx, JSValueConst key)
{
    JSValue exc = qp_exc_fetch(ctx);
    if (qp_type_of(ctx, exc) != QP_EXC(ctx, TypeError)) {
        JS_Throw(ctx, exc);
        return;
    }
    qp_raise(ctx, QP_EXC(ctx, TypeError), "cannot use '%T' as a set element (%S)",
             key, exc);
    JS_FreeValue(ctx, exc);
}

/* set_add_entry: stored str keys are flattened (dictcore requires flat
   strings for its exact-str fast path). */
static int qp_set_add_entry(JSContext *ctx, JSValueConst so, JSValueConst key,
                            Py_hash_t hash)
{
    if (JS_VALUE_GET_TAG(key) == JS_TAG_STRING_ROPE) {
        JSValue flat = js_linearize_string_rope(ctx, JS_DupValue(ctx, key));
        if (JS_IsException(flat))
            return -1;
        return qp_setcore_add_take(ctx, QP_SC(so), flat, hash, qp_set_eq_cb, NULL);
    }
    return qp_setcore_add(ctx, QP_SC(so), key, hash, qp_set_eq_cb, NULL);
}

static int qp_set_contains_entry(JSContext *ctx, JSValueConst so, JSValueConst key,
                                 Py_hash_t hash)
{
    return qp_setcore_contains(ctx, QP_SC(so), key, hash, qp_set_eq_cb, NULL);
}

static int qp_set_discard_entry(JSContext *ctx, JSValueConst so, JSValueConst key,
                                Py_hash_t hash)
{
    return qp_setcore_discard(ctx, QP_SC(so), key, hash, qp_set_eq_cb, NULL);
}

static int qp_set_add_key(JSContext *ctx, JSValueConst so, JSValueConst key)
{
    Py_hash_t hash = qp_hash(ctx, key);
    if (hash == -1) {
        qp_set_unhashable_type(ctx, key);
        return -1;
    }
    return qp_set_add_entry(ctx, so, key, hash);
}

static int qp_set_contains_key(JSContext *ctx, JSValueConst so, JSValueConst key)
{
    Py_hash_t hash = qp_hash(ctx, key);
    if (hash == -1) {
        qp_set_unhashable_type(ctx, key);
        return -1;
    }
    return qp_set_contains_entry(ctx, so, key, hash);
}

static int qp_set_discard_key(JSContext *ctx, JSValueConst so, JSValueConst key)
{
    Py_hash_t hash = qp_hash(ctx, key);
    if (hash == -1) {
        qp_set_unhashable_type(ctx, key);
        return -1;
    }
    return qp_set_discard_entry(ctx, so, key, hash);
}

/* Hash of `key` for the membership-style operations that accept an
   unhashable set as key (looked up as the equivalent frozenset): -1 with
   the CPython error otherwise. */
static Py_hash_t qp_set_hash_or_frozen(JSContext *ctx, JSValueConst key)
{
    Py_hash_t hash = qp_hash(ctx, key);
    if (hash != -1)
        return hash;
    if (!qp_set_check(ctx, key) || !qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
        qp_set_unhashable_type(ctx, key);
        return -1;
    }
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
    return qp_setcore_hash_impl(QP_SC(key));
}

/* _PySet_Contains (`key in so`, set.__contains__, frozenset.__contains__). */
static int qp_set_contains_impl(JSContext *ctx, JSValueConst so, JSValueConst key)
{
    Py_hash_t hash = qp_set_hash_or_frozen(ctx, key);
    if (hash == -1)
        return -1;
    return qp_set_contains_entry(ctx, so, key, hash);
}

/* ------------------------------------------------------------------------ */
/* Allocation, finalization, GC                                              */
/* ------------------------------------------------------------------------ */

/* Empty set/frozenset instance of type t (make_new_set without iterable). */
static JSValue qp_set_alloc(JSContext *ctx, QPType *t)
{
    QPSet *s;
    JSValue obj;
    int class_id = t->instance_class_id;

    if (class_id != JS_CLASS_PY_SET && class_id != JS_CLASS_PY_FROZENSET)
        class_id = qp_type_is_subtype(t, QP_TYPE(ctx, frozenset))
            ? JS_CLASS_PY_FROZENSET : JS_CLASS_PY_SET;
    s = js_malloc(ctx, sizeof(*s));
    if (!s)
        return JS_EXCEPTION;
    qp_setcore_init(&s->core);
    s->hash = -1;
    if (t == QP_TYPE(ctx, set) || t == QP_TYPE(ctx, frozenset))
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), class_id);
    else
        obj = qp_object_new_instance(ctx, t, class_id);
    if (JS_IsException(obj)) {
        js_free(ctx, s);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = s;
    return obj;
}

static void qp_set_finalizer(JSRuntime *rt, JSValue val)
{
    QPSet *s = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (s) {
        JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
        qp_setcore_free(rt, &s->core);
        js_free_rt(rt, s);
    }
}

static void qp_set_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPSet *s = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (s)
        qp_setcore_mark(rt, &s->core, mark_func);
}

/* ------------------------------------------------------------------------ */
/* Bulk update                                                               */
/* ------------------------------------------------------------------------ */

static int qp_set_update_dict(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    return qp_setcore_update_dict(ctx, QP_SC(so), &QP_DICT(other)->core,
                                  qp_set_eq_cb, NULL);
}

static int qp_set_update_iterable(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    JSValue it, key;

    it = qp_iter(ctx, other);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, it);
            return -1;
        }
        if (QP_IS_NULL(key))
            break;
        if (qp_set_add_key(ctx, so, key)) {
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, it);
            return -1;
        }
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, it);
    return 0;
}

/* set_update_internal (also set_update_local / set_update_lock_held). */
static int qp_set_update_internal(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    if (qp_anyset_check(ctx, other)) {
        if (qp_is(so, other))
            return 0;
        return qp_setcore_merge(ctx, QP_SC(so), QP_SC(other), qp_set_eq_cb, NULL);
    }
    if (qp_set_anydict_check_exact(ctx, other))
        return qp_set_update_dict(ctx, so, other);
    return qp_set_update_iterable(ctx, so, other);
}

static int qp_set_update(JSContext *ctx, JSValueConst set, JSValueConst iterable)
{
    return qp_set_update_internal(ctx, set, iterable);
}

/* make_new_set(type, iterable); iterable QP_NULL = none. */
static JSValue qp_make_new_set(JSContext *ctx, QPType *t, JSValueConst iterable)
{
    JSValue so = qp_set_alloc(ctx, t);
    if (JS_IsException(so))
        return so;
    if (!QP_IS_NULL(iterable) && qp_set_update_internal(ctx, so, iterable)) {
        JS_FreeValue(ctx, so);
        return JS_EXCEPTION;
    }
    return so;
}

/* make_new_set_basetype: results of operations on subclasses are plain
   set / frozenset. */
static JSValue qp_make_new_set_basetype(JSContext *ctx, QPType *t, JSValueConst iterable)
{
    if (t != QP_TYPE(ctx, set) && t != QP_TYPE(ctx, frozenset)) {
        if (qp_type_is_subtype(t, QP_TYPE(ctx, set)))
            t = QP_TYPE(ctx, set);
        else
            t = QP_TYPE(ctx, frozenset);
    }
    return qp_make_new_set(ctx, t, iterable);
}

/* make_new_frozenset: frozenset(f) is idempotent for an exact frozenset. */
static JSValue qp_make_new_frozenset(JSContext *ctx, QPType *t, JSValueConst iterable)
{
    if (t != QP_TYPE(ctx, frozenset))
        return qp_make_new_set(ctx, t, iterable);
    if (!QP_IS_NULL(iterable) && qp_frozenset_check_exact(ctx, iterable))
        return JS_DupValue(ctx, iterable);
    return qp_make_new_set(ctx, t, iterable);
}

static JSValue qp_set_new(JSContext *ctx, JSValueConst iterable)
{
    return qp_make_new_set(ctx, QP_TYPE(ctx, set), iterable);
}

/* PyFrozenSet_New: always a new object. */
static JSValue qp_frozenset_new(JSContext *ctx, JSValueConst iterable)
{
    return qp_make_new_set(ctx, QP_TYPE(ctx, frozenset), iterable);
}

/* BUILD_SET: add items in order; the first failure stops adding. */
static JSValue qp_set_from_array(JSContext *ctx, int n, JSValueConst *items)
{
    JSValue so = qp_set_alloc(ctx, QP_TYPE(ctx, set));
    int i;
    if (JS_IsException(so))
        return so;
    for (i = 0; i < n; i++) {
        if (qp_set_add_key(ctx, so, items[i])) {
            JS_FreeValue(ctx, so);
            return JS_EXCEPTION;
        }
    }
    return so;
}

/* PySet_Add (also allowed on a frozenset being built). */
static int qp_set_add(JSContext *ctx, JSValueConst set, JSValueConst key)
{
    return qp_set_add_key(ctx, set, key);
}

/* `key in set` semantics (_PySet_Contains: a set key is looked up as the
   equivalent frozenset). */
static int qp_set_contains(JSContext *ctx, JSValueConst set, JSValueConst key)
{
    return qp_set_contains_impl(ctx, set, key);
}

/* PySet_Discard: 1 found, 0 absent, -1 error. */
static int qp_set_discard(JSContext *ctx, JSValueConst set, JSValueConst key)
{
    return qp_set_discard_key(ctx, set, key);
}

/* _PySet_NextEntry. */
static int qp_set_next(JSValueConst set, Py_ssize_t *ppos, JSValue *pkey, Py_hash_t *phash)
{
    qp_setentry *entry;
    if (!qp_setcore_next(QP_SC(set), ppos, &entry))
        return 0;
    *pkey = entry->key;
    if (phash)
        *phash = entry->hash;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Copy, intersection, difference, symmetric difference                      */
/* ------------------------------------------------------------------------ */

/* set_copy_impl: plain set/frozenset copy (basetype of type(so)). */
static JSValue qp_set_copy_impl(JSContext *ctx, JSValueConst so)
{
    JSValue copy = qp_make_new_set_basetype(ctx, qp_type_of(ctx, so), QP_NULL);
    if (JS_IsException(copy))
        return copy;
    if (qp_setcore_merge(ctx, QP_SC(copy), QP_SC(so), qp_set_eq_cb, NULL) < 0) {
        JS_FreeValue(ctx, copy);
        return JS_EXCEPTION;
    }
    return copy;
}

/* set_intersection */
static JSValue qp_set_intersection(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    JSValue result, it, key;
    Py_hash_t hash;
    int rv;

    if (qp_is(so, other))
        return qp_set_copy_impl(ctx, so);

    result = qp_make_new_set_basetype(ctx, qp_type_of(ctx, so), QP_NULL);
    if (JS_IsException(result))
        return result;

    if (qp_anyset_check(ctx, other)) {
        if (qp_setcore_intersection(ctx, QP_SC(result), QP_SC(so), QP_SC(other),
                                    qp_set_eq_cb, NULL) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        return result;
    }

    it = qp_iter(ctx, other);
    if (JS_IsException(it)) {
        JS_FreeValue(ctx, result);
        return it;
    }
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key))
            goto fail;
        if (QP_IS_NULL(key))
            break;
        hash = qp_hash(ctx, key);
        if (hash == -1)
            goto error;
        rv = qp_set_contains_entry(ctx, so, key, hash);
        if (rv < 0)
            goto error;
        if (rv) {
            if (qp_set_add_entry(ctx, result, key, hash))
                goto error;
            if (qp_set_size(result) >= qp_set_size(so)) {
                JS_FreeValue(ctx, key);
                break;
            }
        }
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, it);
    return result;
 error:
    JS_FreeValue(ctx, key);
 fail:
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

/* set_intersection_multi_impl */
static JSValue qp_set_intersection_multi(JSContext *ctx, JSValueConst so,
                                         JSValueConst *others, int n)
{
    JSValue result, newresult;
    int i;

    if (n == 0)
        return qp_set_copy_impl(ctx, so);
    result = JS_DupValue(ctx, so);
    for (i = 0; i < n; i++) {
        newresult = qp_set_intersection(ctx, result, others[i]);
        JS_FreeValue(ctx, result);
        if (JS_IsException(newresult))
            return newresult;
        result = newresult;
    }
    return result;
}

static int qp_set_both_frozen(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return qp_frozenset_check(ctx, a) && qp_frozenset_check(ctx, b);
}

/* Replace the contents of so by those of tmp (consumes tmp). */
static void qp_set_swap_take(JSContext *ctx, JSValueConst so, JSValue tmp)
{
    qp_setcore_swap_bodies(QP_SC(so), QP_SC(tmp), qp_set_both_frozen(ctx, so, tmp));
    JS_FreeValue(ctx, tmp);
}

/* set_difference_update_internal */
static int qp_set_difference_update_internal(JSContext *ctx, JSValueConst so,
                                             JSValueConst other)
{
    JSValue it, key;

    if (qp_is(so, other)) {
        qp_setcore_clear(ctx, QP_SC(so));
        return 0;
    }
    if (qp_anyset_check(ctx, other))
        return qp_setcore_difference_update_set(ctx, QP_SC(so), QP_SC(other),
                                                qp_set_eq_cb, NULL);
    it = qp_iter(ctx, other);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, it);
            return -1;
        }
        if (QP_IS_NULL(key))
            break;
        if (qp_set_discard_key(ctx, so, key) < 0) {
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, it);
            return -1;
        }
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, it);
    return qp_setcore_purge_dummies(ctx, QP_SC(so));
}

static JSValue qp_set_copy_and_difference(JSContext *ctx, JSValueConst so,
                                          JSValueConst other)
{
    JSValue result = qp_set_copy_impl(ctx, so);
    if (JS_IsException(result))
        return result;
    if (qp_set_difference_update_internal(ctx, result, other) == 0)
        return result;
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

/* set_difference_untracked */
static JSValue qp_set_difference(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    JSValue result;
    Py_ssize_t other_size;
    BOOL is_dict = FALSE;
    int rv;

    if (qp_anyset_check(ctx, other)) {
        other_size = qp_set_size(other);
    } else if (qp_set_anydict_check_exact(ctx, other)) {
        other_size = QP_DICT(other)->core.used;
        is_dict = TRUE;
    } else {
        return qp_set_copy_and_difference(ctx, so, other);
    }

    /* If len(so) much more than len(other), it's more efficient to simply
       copy so and then iterate other looking for common elements. */
    if ((qp_set_size(so) >> 2) > other_size)
        return qp_set_copy_and_difference(ctx, so, other);

    result = qp_make_new_set_basetype(ctx, qp_type_of(ctx, so), QP_NULL);
    if (JS_IsException(result))
        return result;
    if (is_dict)
        rv = qp_setcore_difference_dict(ctx, QP_SC(result), QP_SC(so),
                                        &QP_DICT(other)->core, qp_set_eq_cb, NULL);
    else
        rv = qp_setcore_difference_set(ctx, QP_SC(result), QP_SC(so), QP_SC(other),
                                       qp_set_eq_cb, NULL);
    if (rv < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* set_difference_multi_impl */
static JSValue qp_set_difference_multi(JSContext *ctx, JSValueConst so,
                                       JSValueConst *others, int n)
{
    JSValue result;
    int i;

    if (n == 0)
        return qp_set_copy_impl(ctx, so);
    result = qp_set_difference(ctx, so, others[0]);
    if (JS_IsException(result))
        return result;
    for (i = 1; i < n; i++) {
        if (qp_set_difference_update_internal(ctx, result, others[i])) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

/* set_symmetric_difference_update_impl body: 0 / -1. */
static int qp_set_symmetric_difference_update_internal(JSContext *ctx, JSValueConst so,
                                                       JSValueConst other)
{
    JSValue otherset;
    int rv;

    if (qp_is(so, other)) {
        qp_setcore_clear(ctx, QP_SC(so));
        return 0;
    }
    if (qp_set_anydict_check_exact(ctx, other))
        return qp_setcore_symmetric_difference_update_dict(ctx, QP_SC(so),
                                                           &QP_DICT(other)->core,
                                                           qp_set_eq_cb, NULL);
    if (qp_anyset_check(ctx, other))
        return qp_setcore_symmetric_difference_update_set(ctx, QP_SC(so), QP_SC(other),
                                                          qp_set_eq_cb, NULL);
    otherset = qp_make_new_set_basetype(ctx, qp_type_of(ctx, so), other);
    if (JS_IsException(otherset))
        return -1;
    rv = qp_setcore_symmetric_difference_update_set(ctx, QP_SC(so), QP_SC(otherset),
                                                    qp_set_eq_cb, NULL);
    JS_FreeValue(ctx, otherset);
    return rv;
}

/* set_symmetric_difference_impl */
static JSValue qp_set_symmetric_difference(JSContext *ctx, JSValueConst so,
                                           JSValueConst other)
{
    JSValue result = qp_make_new_set_basetype(ctx, qp_type_of(ctx, so), QP_NULL);
    if (JS_IsException(result))
        return result;
    if (qp_set_update_internal(ctx, result, other) < 0 ||
        qp_setcore_symmetric_difference_update_set(ctx, QP_SC(result), QP_SC(so),
                                                   qp_set_eq_cb, NULL) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* ------------------------------------------------------------------------ */
/* Subset relations                                                          */
/* ------------------------------------------------------------------------ */

/* set_issubset_impl: 1 / 0 / -1. */
static int qp_set_issubset(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    if (!qp_anyset_check(ctx, other)) {
        JSValue tmp = qp_set_intersection(ctx, so, other);
        int result;
        if (JS_IsException(tmp))
            return -1;
        result = qp_set_size(tmp) == qp_set_size(so);
        JS_FreeValue(ctx, tmp);
        return result;
    }
    return qp_setcore_issubset(ctx, QP_SC(so), QP_SC(other), qp_set_eq_cb, NULL);
}

/* set_issuperset_impl: 1 / 0 / -1. */
static int qp_set_issuperset(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    JSValue it, key;
    int rv;

    if (qp_anyset_check(ctx, other))
        return qp_set_issubset(ctx, other, so);
    it = qp_iter(ctx, other);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key)) {
            rv = -1;
            break;
        }
        if (QP_IS_NULL(key)) {
            rv = 1;
            break;
        }
        rv = qp_set_contains_key(ctx, so, key);
        JS_FreeValue(ctx, key);
        if (rv < 0)
            break;
        if (!rv)
            break;
    }
    JS_FreeValue(ctx, it);
    return rv;
}

/* set_isdisjoint_impl: 1 / 0 / -1. */
static int qp_set_isdisjoint(JSContext *ctx, JSValueConst so, JSValueConst other)
{
    JSValue it, key;
    int rv;

    if (qp_is(so, other))
        return qp_set_size(so) == 0;
    if (qp_anyset_check_exact(ctx, other))
        return qp_setcore_isdisjoint(ctx, QP_SC(so), QP_SC(other), qp_set_eq_cb, NULL);
    it = qp_iter(ctx, other);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        key = qp_next(ctx, it);
        if (JS_IsException(key)) {
            rv = -1;
            break;
        }
        if (QP_IS_NULL(key)) {
            rv = 1;
            break;
        }
        rv = qp_set_contains_key(ctx, so, key);
        JS_FreeValue(ctx, key);
        if (rv < 0)
            break;
        if (rv) {
            rv = 0;
            break;
        }
    }
    JS_FreeValue(ctx, it);
    return rv;
}

static JSValue qp_set_bool_result(int r)
{
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

/* set_richcompare */
static JSValue qp_set_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    Py_hash_t v_hash, w_hash;
    int r;

    if (!qp_anyset_check(ctx, w))
        return qp_not_implemented(ctx);

    switch (op) {
    case QP_CMP_EQ:
    case QP_CMP_NE:
        if (qp_set_size(v) != qp_set_size(w)) {
            r = 0;
        } else {
            v_hash = QP_SC(v)->hash;
            w_hash = QP_SC(w)->hash;
            if (v_hash != -1 && w_hash != -1 && v_hash != w_hash)
                r = 0;
            else
                r = qp_set_issubset(ctx, v, w);
        }
        if (r < 0)
            return JS_EXCEPTION;
        return qp_bool_new(op == QP_CMP_EQ ? r : !r);
    case QP_CMP_LE:
        return qp_set_bool_result(qp_set_issubset(ctx, v, w));
    case QP_CMP_GE:
        return qp_set_bool_result(qp_set_issuperset(ctx, v, w));
    case QP_CMP_LT:
        if (qp_set_size(v) >= qp_set_size(w))
            return qp_bool_new(0);
        return qp_set_bool_result(qp_set_issubset(ctx, v, w));
    case QP_CMP_GT:
        if (qp_set_size(v) <= qp_set_size(w))
            return qp_bool_new(0);
        return qp_set_bool_result(qp_set_issuperset(ctx, v, w));
    }
    return qp_not_implemented(ctx);
}

/* ------------------------------------------------------------------------ */
/* Number protocol                                                           */
/* ------------------------------------------------------------------------ */

static JSValue qp_set_or(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue result;

    if (!qp_anyset_check(ctx, self) || !qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    result = qp_set_copy_impl(ctx, self);
    if (JS_IsException(result))
        return result;
    if (qp_is(self, other))
        return result;
    if (qp_set_update_internal(ctx, result, other)) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

static JSValue qp_set_ior(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    if (qp_set_update_internal(ctx, self, other))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

static JSValue qp_set_and(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, self) || !qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    return qp_set_intersection(ctx, self, other);
}

static JSValue qp_set_iand(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue tmp;

    if (!qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    tmp = qp_set_intersection(ctx, self, other);
    if (JS_IsException(tmp))
        return tmp;
    qp_set_swap_take(ctx, self, tmp);
    return JS_DupValue(ctx, self);
}

static JSValue qp_set_sub(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, self) || !qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    return qp_set_difference(ctx, self, other);
}

static JSValue qp_set_isub(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    if (qp_set_difference_update_internal(ctx, self, other) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

static JSValue qp_set_xor(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, self) || !qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    return qp_set_symmetric_difference(ctx, self, other);
}

static JSValue qp_set_ixor(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (!qp_anyset_check(ctx, other))
        return qp_not_implemented(ctx);
    if (qp_set_symmetric_difference_update_internal(ctx, self, other) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

/* ------------------------------------------------------------------------ */
/* Sequence protocol, hash, repr                                             */
/* ------------------------------------------------------------------------ */

static Py_ssize_t qp_set_len(JSContext *ctx, JSValueConst self)
{
    return qp_set_size(self);
}

static int qp_set_sq_contains(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    return qp_set_contains_impl(ctx, self, key);
}

/* frozenset_hash (cached in the core). */
static Py_hash_t qp_frozenset_hash(JSContext *ctx, JSValueConst self)
{
    return qp_setcore_frozenset_hash(QP_SC(self));
}

/* Py_TYPE(so)->tp_name as a str. */
static JSValue qp_set_type_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* set_repr: "set()", "{1, 2}", "frozenset({1})", "Sub({1})", "set(...)". */
static JSValue qp_set_repr(JSContext *ctx, JSValueConst so)
{
    QPType *t = qp_type_of(ctx, so);
    JSValue name, sep, listrepr, result = JS_EXCEPTION;
    JSValue *items;
    Py_ssize_t n, i, pos;
    qp_setentry *entry;
    int status;

    status = qp_repr_enter(ctx, so);
    if (status != 0) {
        if (status < 0)
            return JS_EXCEPTION;
        name = qp_set_type_name(ctx, t);
        if (JS_IsException(name))
            return name;
        result = qp_str_from_format(ctx, "%U(...)", name);
        JS_FreeValue(ctx, name);
        return result;
    }

    n = qp_set_size(so);
    if (n == 0) {
        qp_repr_leave(ctx, so);
        name = qp_set_type_name(ctx, t);
        if (JS_IsException(name))
            return name;
        result = qp_str_from_format(ctx, "%U()", name);
        JS_FreeValue(ctx, name);
        return result;
    }

    /* Snapshot the keys first: the element reprs may mutate the set. */
    items = js_malloc(ctx, sizeof(JSValue) * n);
    if (!items)
        goto done;
    pos = 0;
    i = 0;
    while (i < n && qp_setcore_next(QP_SC(so), &pos, &entry))
        items[i++] = JS_DupValue(ctx, entry->key);
    n = i;
    for (i = 0; i < n; i++) {
        JSValue r = qp_repr(ctx, items[i]);
        JS_FreeValue(ctx, items[i]);
        items[i] = r;
        if (JS_IsException(r)) {
            Py_ssize_t j;
            for (j = i + 1; j < n; j++)
                JS_FreeValue(ctx, items[j]);
            n = i;
            goto free_items;
        }
    }
    sep = qp_str_from_cstr(ctx, ", ");
    if (JS_IsException(sep))
        goto free_items;
    listrepr = qp_str_join_array(ctx, sep, (int)n, (JSValueConst *)items);
    JS_FreeValue(ctx, sep);
    if (JS_IsException(listrepr))
        goto free_items;
    if (t != QP_TYPE(ctx, set)) {
        name = qp_set_type_name(ctx, t);
        if (!JS_IsException(name)) {
            result = qp_str_from_format(ctx, "%U({%U})", name, listrepr);
            JS_FreeValue(ctx, name);
        }
    } else {
        result = qp_str_from_format(ctx, "{%U}", listrepr);
    }
    JS_FreeValue(ctx, listrepr);
 free_items:
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, items[i]);
    js_free(ctx, items);
 done:
    qp_repr_leave(ctx, so);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Iterator                                                                  */
/* ------------------------------------------------------------------------ */

static void qp_setiter_finalizer(JSRuntime *rt, JSValueConst val)
{
    QPSetIter *si = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (si) {
        JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
        JS_FreeValueRT(rt, si->set);
        js_free_rt(rt, si);
    }
}

static void qp_setiter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPSetIter *si = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (si)
        JS_MarkValue(rt, si->set, mark_func);
}

/* set_iter */
static JSValue qp_set_iter(JSContext *ctx, JSValueConst so)
{
    QPSetIter *si;
    JSValue obj;

    si = js_malloc(ctx, sizeof(*si));
    if (!si)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, set_iterator)),
                                 JS_CLASS_PY_DICT_ITER);
    if (JS_IsException(obj)) {
        js_free(ctx, si);
        return obj;
    }
    si->kind = QP_DICTITER_KIND_SET;
    si->set = JS_DupValue(ctx, so);
    qp_setcore_iter_init(QP_SC(so), &si->st);
    JS_VALUE_GET_OBJ(obj)->u.opaque = si;
    return obj;
}

static QPSetIter *qp_setiter_payload(JSValueConst it)
{
    return JS_VALUE_GET_OBJ(it)->u.opaque;
}

/* setiter_iternext */
static JSValue qp_setiter_next(JSContext *ctx, JSValueConst it)
{
    QPSetIter *si = qp_setiter_payload(it);
    JSValue key;
    int r;

    if (QP_IS_NULL(si->set))
        return QP_NULL;
    r = qp_setcore_iter_next(QP_SC(si->set), &si->st, &key);
    if (r == QP_SETITER_CHANGED_SIZE)
        return qp_raise_runtime_error(ctx, "Set changed size during iteration");
    if (r == 0) {
        JSValue set = si->set;
        si->set = QP_NULL;
        JS_FreeValue(ctx, set);
        return QP_NULL;
    }
    return JS_DupValue(ctx, key);
}

static JSValue qp_setiter_self(JSContext *ctx, JSValueConst it)
{
    return JS_DupValue(ctx, it);
}

/* setiter_len */
static JSValue qp_setiter_length_hint(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPSetIter *si = qp_setiter_payload(self);
    Py_ssize_t len = 0;
    if (!QP_IS_NULL(si->set))
        len = qp_setcore_iter_length_hint(QP_SC(si->set), &si->st);
    return qp_long_from_int64(ctx, len);
}

/* setiter_reduce: (iter, (list_of_remaining_items,)) */
static JSValue qp_setiter_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPSetIter *si = qp_setiter_payload(self);
    qp_setcore_iter tmp = si->st;
    JSValue set, list, key, iter_fn, targs, result;
    int r;

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    if (!QP_IS_NULL(si->set)) {
        set = JS_DupValue(ctx, si->set);
        for (;;) {
            r = qp_setcore_iter_next(QP_SC(set), &tmp, &key);
            if (r == 0)
                break;
            if (r == QP_SETITER_CHANGED_SIZE) {
                JS_FreeValue(ctx, set);
                JS_FreeValue(ctx, list);
                return qp_raise_runtime_error(ctx, "Set changed size during iteration");
            }
            if (qp_list_append(ctx, list, key)) {
                JS_FreeValue(ctx, set);
                JS_FreeValue(ctx, list);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, set);
    }
    /* _PyEval_GetBuiltin(&_Py_ID(iter)) */
    r = qp_dict_get_item_atom(ctx, QP_STATE(ctx)->builtins_dict, QP_ATOM(ctx, iter),
                              &iter_fn);
    if (r <= 0) {
        JS_FreeValue(ctx, list);
        if (r == 0)
            return qp_raise(ctx, QP_EXC(ctx, AttributeError), "iter");
        return JS_EXCEPTION;
    }
    targs = qp_tuple_pack(ctx, 1, (JSValueConst *)&list);
    JS_FreeValue(ctx, list);
    if (JS_IsException(targs)) {
        JS_FreeValue(ctx, iter_fn);
        return targs;
    }
    {
        JSValueConst items[2] = { iter_fn, targs };
        result = qp_tuple_pack(ctx, 2, items);
    }
    JS_FreeValue(ctx, iter_fn);
    JS_FreeValue(ctx, targs);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Constructors                                                              */
/* ------------------------------------------------------------------------ */

/* PyArg_UnpackTuple(args, Py_TYPE(self)->tp_name, 0, 1, ...) count check. */
static int qp_set_check_nargs(JSContext *ctx, QPType *t, int nargs)
{
    const char *name;
    int r;

    if (nargs <= 1)
        return 0;
    if (t->tp_name)
        return qp_arg_check_positional(ctx, t->tp_name, nargs, 0, 1);
    name = JS_AtomToCString(ctx, t->name);
    if (!name)
        return -1;
    r = qp_arg_check_positional(ctx, name, nargs, 0, 1);
    JS_FreeCString(ctx, name);
    return r;
}

/* Generic type_call for subtypes reached through the exact type's
   vectorcall: tp_new, then tp_init when the result is an instance. */
static JSValue qp_set_type_call(JSContext *ctx, QPType *t, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    JSValue obj = t->slots.tp_new(ctx, t, args, nargsf, kwnames);
    if (JS_IsException(obj))
        return obj;
    if (!qp_typecheck(ctx, obj, t) || !t->slots.tp_init)
        return obj;
    if (t->slots.tp_init(ctx, obj, args, nargsf, kwnames) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* set_new: ignores its arguments (set_init consumes them). */
static JSValue qp_set_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    return qp_make_new_set(ctx, subtype, QP_NULL);
}

/* set_init */
static int qp_set_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);

    if (qp_arg_no_keywords(ctx, "set", kwnames))
        return -1;
    if (qp_set_check_nargs(ctx, qp_type_of(ctx, self), nargs))
        return -1;
    if (QP_SC(self)->fill)
        qp_setcore_clear(ctx, QP_SC(self));
    QP_SC(self)->hash = -1;
    if (nargs == 0)
        return 0;
    return qp_set_update_internal(ctx, self, args[0]);
}

/* set_vectorcall */
static JSValue qp_set_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    QPType *t = qp_type_from_value(callable);
    Py_ssize_t nargs = qp_nargs(nargsf);

    if (t != QP_TYPE(ctx, set))
        return qp_set_type_call(ctx, t, args, nargsf, kwnames);
    if (qp_arg_no_keywords(ctx, "set", kwnames))
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "set", (int)nargs, 0, 1))
        return JS_EXCEPTION;
    return qp_make_new_set(ctx, t, nargs ? args[0] : QP_NULL);
}

/* frozenset_new */
static JSValue qp_frozenset_tp_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    QPType *fs = QP_TYPE(ctx, frozenset);

    if ((type == fs || type->slots.tp_init == fs->slots.tp_init) &&
        qp_arg_no_keywords(ctx, "frozenset", kwnames))
        return JS_EXCEPTION;
    if (qp_set_check_nargs(ctx, type, nargs))
        return JS_EXCEPTION;
    return qp_make_new_frozenset(ctx, type, nargs ? args[0] : QP_NULL);
}

/* frozenset_vectorcall */
static JSValue qp_frozenset_vectorcall(JSContext *ctx, JSValueConst callable,
                                       JSValueConst *args, size_t nargsf,
                                       JSValueConst kwnames)
{
    QPType *t = qp_type_from_value(callable);
    Py_ssize_t nargs = qp_nargs(nargsf);

    if (t != QP_TYPE(ctx, frozenset))
        return qp_set_type_call(ctx, t, args, nargsf, kwnames);
    if (qp_arg_no_keywords(ctx, "frozenset", kwnames))
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "frozenset", (int)nargs, 0, 1))
        return JS_EXCEPTION;
    return qp_make_new_frozenset(ctx, t, nargs ? args[0] : QP_NULL);
}

/* ------------------------------------------------------------------------ */
/* Methods                                                                   */
/* ------------------------------------------------------------------------ */

#define QP_SET_METH(fn) \
    static JSValue fn(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                      int nargs, JSValueConst kwnames, int magic)

QP_SET_METH(qp_set_m_add)
{
    if (qp_set_add_key(ctx, self, args[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

QP_SET_METH(qp_set_m_clear)
{
    qp_setcore_clear(ctx, QP_SC(self));
    return JS_UNDEFINED;
}

QP_SET_METH(qp_set_m_contains)
{
    return qp_set_bool_result(qp_set_contains_impl(ctx, self, args[0]));
}

QP_SET_METH(qp_set_m_copy)
{
    return qp_set_copy_impl(ctx, self);
}

QP_SET_METH(qp_frozenset_m_copy)
{
    if (qp_frozenset_check_exact(ctx, self))
        return JS_DupValue(ctx, self);
    return qp_set_copy_impl(ctx, self);
}

/* set.remove / set.discard (magic 1 = remove: KeyError when absent). */
QP_SET_METH(qp_set_m_discard)
{
    JSValueConst key = args[0];
    int rv = qp_set_discard_key(ctx, self, key);

    if (rv < 0) {
        Py_hash_t hash;
        if (!qp_set_check(ctx, key) || !qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            return JS_EXCEPTION;
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        hash = qp_setcore_hash_impl(QP_SC(key));
        rv = qp_set_discard_entry(ctx, self, key, hash);
        if (rv < 0)
            return JS_EXCEPTION;
    }
    if (magic && rv == 0)
        return qp_raise_key_error(ctx, key);
    return JS_UNDEFINED;
}

QP_SET_METH(qp_set_m_difference)
{
    return qp_set_difference_multi(ctx, self, args, nargs);
}

QP_SET_METH(qp_set_m_difference_update)
{
    int i;
    for (i = 0; i < nargs; i++) {
        if (qp_set_difference_update_internal(ctx, self, args[i]))
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

QP_SET_METH(qp_set_m_intersection)
{
    return qp_set_intersection_multi(ctx, self, args, nargs);
}

QP_SET_METH(qp_set_m_intersection_update)
{
    JSValue tmp = qp_set_intersection_multi(ctx, self, args, nargs);
    if (JS_IsException(tmp))
        return tmp;
    qp_set_swap_take(ctx, self, tmp);
    return JS_UNDEFINED;
}

QP_SET_METH(qp_set_m_isdisjoint)
{
    return qp_set_bool_result(qp_set_isdisjoint(ctx, self, args[0]));
}

QP_SET_METH(qp_set_m_issubset)
{
    return qp_set_bool_result(qp_set_issubset(ctx, self, args[0]));
}

QP_SET_METH(qp_set_m_issuperset)
{
    return qp_set_bool_result(qp_set_issuperset(ctx, self, args[0]));
}

QP_SET_METH(qp_set_m_pop)
{
    JSValue key;
    if (!qp_setcore_pop(QP_SC(self), &key))
        return qp_raise(ctx, QP_EXC(ctx, KeyError), "pop from an empty set");
    return key;
}

/* set___reduce___impl: (type(so), (list(so),), so.__getstate__()) */
QP_SET_METH(qp_set_m_reduce)
{
    JSValue keys, targs, state, result;
    JSValueConst items[3];

    keys = qp_list_from_iterable(ctx, self);
    if (JS_IsException(keys))
        return keys;
    targs = qp_tuple_pack(ctx, 1, (JSValueConst *)&keys);
    JS_FreeValue(ctx, keys);
    if (JS_IsException(targs))
        return targs;
    state = qp_call_method(ctx, self, QP_ATOM(ctx, __getstate__), NULL, 0, QP_NULL);
    if (JS_IsException(state)) {
        JS_FreeValue(ctx, targs);
        return state;
    }
    items[0] = QP_TYPE_VALUE(qp_type_of(ctx, self));
    items[1] = targs;
    items[2] = state;
    result = qp_tuple_pack(ctx, 3, items);
    JS_FreeValue(ctx, targs);
    JS_FreeValue(ctx, state);
    return result;
}

QP_SET_METH(qp_set_m_sizeof)
{
    const qp_setcore *s = QP_SC(self);
    int64_t res = QP_SET_BASICSIZE;
    if (s->table != s->smalltable)
        res += ((int64_t)s->mask + 1) * QP_SET_ENTRYSIZE;
    return qp_long_from_int64(ctx, res);
}

QP_SET_METH(qp_set_m_symmetric_difference)
{
    return qp_set_symmetric_difference(ctx, self, args[0]);
}

QP_SET_METH(qp_set_m_symmetric_difference_update)
{
    if (qp_set_symmetric_difference_update_internal(ctx, self, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* set_union_impl */
QP_SET_METH(qp_set_m_union)
{
    JSValue result = qp_make_new_set_basetype(ctx, qp_type_of(ctx, self), self);
    int i;

    if (JS_IsException(result))
        return result;
    for (i = 0; i < nargs; i++) {
        if (qp_is(self, args[i]))
            continue;
        if (qp_set_update_internal(ctx, result, args[i])) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

QP_SET_METH(qp_set_m_update)
{
    int i;
    for (i = 0; i < nargs; i++) {
        if (qp_set_update_internal(ctx, self, args[i]))
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* __class_getitem__ = Py_GenericAlias (self is the class). */
QP_SET_METH(qp_set_m_class_getitem)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

#undef QP_SET_METH

static const QPMethodDef qp_set_methods[] = {
    { "add", qp_set_m_add, QP_METH_O, 0,
      "add($self, object, /)\n--\n\nAdd an element to a set.\n\n"
      "This has no effect if the element is already present." },
    { "clear", qp_set_m_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nRemove all elements from this set." },
    { "__contains__", qp_set_m_contains, QP_METH_O | QP_METH_COEXIST, 0,
      "__contains__($self, object, /)\n--\n\nx.__contains__(y) <==> y in x." },
    { "copy", qp_set_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a shallow copy of a set." },
    { "discard", qp_set_m_discard, QP_METH_O, 0,
      "discard($self, object, /)\n--\n\nRemove an element from a set if it is a member.\n\n"
      "Unlike set.remove(), the discard() method does not raise\n"
      "an exception when an element is missing from the set." },
    { "difference", qp_set_m_difference, QP_METH_FASTCALL, 0,
      "difference($self, /, *others)\n--\n\n"
      "Return a new set with elements in the set that are not in the others." },
    { "difference_update", qp_set_m_difference_update, QP_METH_FASTCALL, 0,
      "difference_update($self, /, *others)\n--\n\n"
      "Update the set, removing elements found in others." },
    { "intersection", qp_set_m_intersection, QP_METH_FASTCALL, 0,
      "intersection($self, /, *others)\n--\n\n"
      "Return a new set with elements common to the set and all others." },
    { "intersection_update", qp_set_m_intersection_update, QP_METH_FASTCALL, 0,
      "intersection_update($self, /, *others)\n--\n\n"
      "Update the set, keeping only elements found in it and all others." },
    { "isdisjoint", qp_set_m_isdisjoint, QP_METH_O, 0,
      "isdisjoint($self, other, /)\n--\n\nReturn True if two sets have a null intersection." },
    { "issubset", qp_set_m_issubset, QP_METH_O, 0,
      "issubset($self, other, /)\n--\n\nReport whether another set contains this set." },
    { "issuperset", qp_set_m_issuperset, QP_METH_O, 0,
      "issuperset($self, other, /)\n--\n\nReport whether this set contains another set." },
    { "pop", qp_set_m_pop, QP_METH_NOARGS, 0,
      "pop($self, /)\n--\n\nRemove and return an arbitrary set element.\n\n"
      "Raises KeyError if the set is empty." },
    { "__reduce__", qp_set_m_reduce, QP_METH_NOARGS, 0,
      "__reduce__($self, /)\n--\n\nReturn state information for pickling." },
    { "remove", qp_set_m_discard, QP_METH_O, 1,
      "remove($self, object, /)\n--\n\nRemove an element from a set; it must be a member.\n\n"
      "If the element is not a member, raise a KeyError." },
    { "__sizeof__", qp_set_m_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nS.__sizeof__() -> size of S in memory, in bytes." },
    { "symmetric_difference", qp_set_m_symmetric_difference, QP_METH_O, 0,
      "symmetric_difference($self, other, /)\n--\n\n"
      "Return a new set with elements in either the set or other but not both." },
    { "symmetric_difference_update", qp_set_m_symmetric_difference_update, QP_METH_O, 0,
      "symmetric_difference_update($self, other, /)\n--\n\n"
      "Update the set, keeping only elements found in either set, but not in both." },
    { "union", qp_set_m_union, QP_METH_FASTCALL, 0,
      "union($self, /, *others)\n--\n\n"
      "Return a new set with elements from the set and all others." },
    { "update", qp_set_m_update, QP_METH_FASTCALL, 0,
      "update($self, /, *others)\n--\n\nUpdate the set, adding elements from all others." },
    { "__class_getitem__", qp_set_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "sets are generic over the type of their elements" },
    { NULL }
};

static const QPMethodDef qp_frozenset_methods[] = {
    { "__contains__", qp_set_m_contains, QP_METH_O | QP_METH_COEXIST, 0,
      "__contains__($self, object, /)\n--\n\nx.__contains__(y) <==> y in x." },
    { "copy", qp_frozenset_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nReturn a shallow copy of a set." },
    { "difference", qp_set_m_difference, QP_METH_FASTCALL, 0,
      "difference($self, /, *others)\n--\n\n"
      "Return a new set with elements in the set that are not in the others." },
    { "intersection", qp_set_m_intersection, QP_METH_FASTCALL, 0,
      "intersection($self, /, *others)\n--\n\n"
      "Return a new set with elements common to the set and all others." },
    { "isdisjoint", qp_set_m_isdisjoint, QP_METH_O, 0,
      "isdisjoint($self, other, /)\n--\n\nReturn True if two sets have a null intersection." },
    { "issubset", qp_set_m_issubset, QP_METH_O, 0,
      "issubset($self, other, /)\n--\n\nReport whether another set contains this set." },
    { "issuperset", qp_set_m_issuperset, QP_METH_O, 0,
      "issuperset($self, other, /)\n--\n\nReport whether this set contains another set." },
    { "__reduce__", qp_set_m_reduce, QP_METH_NOARGS, 0,
      "__reduce__($self, /)\n--\n\nReturn state information for pickling." },
    { "__sizeof__", qp_set_m_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nS.__sizeof__() -> size of S in memory, in bytes." },
    { "symmetric_difference", qp_set_m_symmetric_difference, QP_METH_O, 0,
      "symmetric_difference($self, other, /)\n--\n\n"
      "Return a new set with elements in either the set or other but not both." },
    { "union", qp_set_m_union, QP_METH_FASTCALL, 0,
      "union($self, /, *others)\n--\n\n"
      "Return a new set with elements from the set and all others." },
    { "__class_getitem__", qp_set_m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "frozensets are generic over the type of their elements" },
    { NULL }
};

static const QPMethodDef qp_setiter_methods[] = {
    { "__length_hint__", qp_setiter_length_hint, QP_METH_NOARGS, 0,
      "Private method returning an estimate of len(list(it))." },
    { "__reduce__", qp_setiter_reduce, QP_METH_NOARGS, 0,
      "Return state information for pickling." },
    { NULL }
};

/* ------------------------------------------------------------------------ */
/* Type specs and init                                                       */
/* ------------------------------------------------------------------------ */

static const QPTypeSpec qp_set_spec = {
    .name = "set",
    .doc = "set(iterable=(), /)\n--\n\nBuild an unordered collection of unique elements.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_SET,
    .slots = {
        .tp_repr = qp_set_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_vectorcall = qp_set_vectorcall,
        .tp_richcompare = qp_set_richcompare,
        .tp_iter = qp_set_iter,
        .tp_init = qp_set_tp_init,
        .tp_new = qp_set_tp_new,
        .nb = {
            .nb_subtract = qp_set_sub,
            .nb_and = qp_set_and,
            .nb_xor = qp_set_xor,
            .nb_or = qp_set_or,
            .nb_inplace_subtract = qp_set_isub,
            .nb_inplace_and = qp_set_iand,
            .nb_inplace_xor = qp_set_ixor,
            .nb_inplace_or = qp_set_ior,
        },
        .sq = {
            .sq_length = qp_set_len,
            .sq_contains = qp_set_sq_contains,
        },
    },
    .methods = qp_set_methods,
};

static const QPTypeSpec qp_frozenset_spec = {
    .name = "frozenset",
    .doc = "frozenset(iterable=(), /)\n--\n\n"
           "Build an immutable unordered collection of unique elements.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MATCH_SELF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_FROZENSET,
    .slots = {
        .tp_repr = qp_set_repr,
        .tp_hash = qp_frozenset_hash,
        .tp_vectorcall = qp_frozenset_vectorcall,
        .tp_richcompare = qp_set_richcompare,
        .tp_iter = qp_set_iter,
        .tp_new = qp_frozenset_tp_new,
        .nb = {
            .nb_subtract = qp_set_sub,
            .nb_and = qp_set_and,
            .nb_xor = qp_set_xor,
            .nb_or = qp_set_or,
        },
        .sq = {
            .sq_length = qp_set_len,
            .sq_contains = qp_set_sq_contains,
        },
    },
    .methods = qp_frozenset_methods,
};

static const QPTypeSpec qp_setiter_spec = {
    .name = "set_iterator",
    .flags = QP_TPFLAGS_DEFAULT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT_ITER,
    .slots = {
        .tp_iter = qp_setiter_self,
        .tp_iternext = qp_setiter_next,
    },
    .methods = qp_setiter_methods,
};

/* Called by qp_init_dicts (dictobject.c): ready set, frozenset and
   set_iterator; create QPState.empty_frozenset.  0 / -1. */
static int qp_init_sets(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSValue fs;

    if (qp_type_ready(ctx, QP_TYPE(ctx, set), &qp_set_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, frozenset), &qp_frozenset_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, set_iterator), &qp_setiter_spec) < 0)
        return -1;
    fs = qp_set_alloc(ctx, QP_TYPE(ctx, frozenset));
    if (JS_IsException(fs))
        return -1;
    st->empty_frozenset = fs;
    return 0;
}
