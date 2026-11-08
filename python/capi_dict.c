/*
 * dquickts C API (D5, unit E1): dict, set, capsule and weak reference
 * objects, and the containers init step (qp_capi_init_containers).
 *
 * Semantics ported from CPython 3.15.2 Objects/dictobject.c,
 * Objects/setobject.c, Objects/capsule.c and Objects/weakrefobject.c
 * (PSF-2.0).  Part of the quickjs.c translation unit, after capi_tuple.c.
 *
 * Layouts (capi-design.md §1):
 * - DICT / SET: always the JS dict/set; PyDictObject / PySetObject are
 *   handles (sizes are calls).  Borrowed values of PyDict_GetItem* and the
 *   key/value pairs of PyDict_Next are pinned by the dict mirror under hashed
 *   keys (value pins: hash bit 61 clear, key pins: bit 61 set).
 * - PyCapsule_Type: a static extension type (instances are natives).
 * - WEAKREF: the weakref objects of the runtime's _weakref module; the
 *   three public weakref types are static PyTypeObjects bound to them.
 */

#include "modstate.h"

/* Internal exports (CPython pycore_dict.h, pycore_setobject.h,
   pycore_capsule.h). */
PyAPI_FUNC(int) _PyDict_GetItemRef_KnownHash(PyDictObject *op, PyObject *key, Py_hash_t hash,
                                             PyObject **result);
PyAPI_FUNC(int) _PyDict_GetItemRef_KnownHash_LockHeld(PyDictObject *op, PyObject *key,
                                                      Py_hash_t hash, PyObject **result);
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key, PyObject *item,
                                          Py_hash_t hash);
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash_LockHeld(PyDictObject *mp, PyObject *key,
                                                   PyObject *value, Py_hash_t hash);
PyAPI_FUNC(int) _PyDict_DelItem_KnownHash(PyObject *mp, PyObject *key, Py_hash_t hash);
PyAPI_FUNC(int) _PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey,
                             PyObject **pvalue, Py_hash_t *phash);
PyAPI_FUNC(int) _PyDict_Pop_KnownHash(PyDictObject *mp, PyObject *key, Py_hash_t hash,
                                      PyObject **result);
PyAPI_FUNC(void) _PyDict_Clear_LockHeld(PyObject *op);
PyAPI_FUNC(int) _PySet_NextEntryRef(PyObject *set, Py_ssize_t *pos, PyObject **key,
                                    Py_hash_t *hash);
PyAPI_FUNC(int) _PySet_Update(PyObject *set, PyObject *iterable);
PyAPI_FUNC(int) _PySet_NextEntry(PyObject *set, Py_ssize_t *pos, PyObject **key,
                                 Py_hash_t *hash);
PyAPI_FUNC(int) _PyCapsule_SetTraverse(PyObject *op, traverseproc traverse_func,
                                       inquiry clear_func);
/* Removed from the 3.15 headers, still part of the stable ABI (abi3). */
PyAPI_FUNC(PyObject *) PyWeakref_GetObject(PyObject *ref);

/* Private helpers of the M2/M3 units (same translation unit). */
/* dictobject.c (unit f) */
static int qp_dict_eq_cb(JSContext *ctx, JSValueConst a, JSValueConst b, void *opaque);
static int qp_dict_prepare_key(JSContext *ctx, JSValueConst d, JSValueConst key,
                               JSValue *pkey, Py_hash_t *phash);
static int qp_dict_pop_impl(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *presult);
static int qp_dict_merge_from_seq2(JSContext *ctx, JSValueConst d, JSValueConst seq2,
                                   int override);
/* mod_weakref.c (M3: _weakref); wr_types[] order: ReferenceType,
   ProxyType, CallableProxyType */
static QPModState *qp_wr_state(JSContext *ctx);
static QPWeakRec *qp_wr_rec(JSContext *ctx, JSValueConst v);
static void qp_wr_unlink(JSRuntime *rt, QPWeakRec *rec);

/* ========================================================================= */
/* DICT                                                                       */
/* ========================================================================= */

#define QP_CAPI_DICT_HMASK (((uint64_t)1 << 61) - 1)
#define QP_CAPI_DICT_VPIN(h) QP_CAPI_PIN_HASHED((uint64_t)(h) & QP_CAPI_DICT_HMASK)
#define QP_CAPI_DICT_KPIN(h) \
    QP_CAPI_PIN_HASHED(((uint64_t)(h) & QP_CAPI_DICT_HMASK) | ((uint64_t)1 << 61))

/* frozendict_does_not_support(what) / PyErr_BadInternalCall for the
   mutation API: 0 if op is a (mutable) dict. */
static int qp_capi_dict_mutable(PyObject *op, const char *what)
{
    if (PyDict_Check(op))
        return 0;
    if (op && PyFrozenDict_Check(op))
        PyErr_Format(PyExc_TypeError, "frozendict object does not support item %s", what);
    else
        PyErr_BadInternalCall();
    return -1;
}

/* Key of a lookup with a known hash: ropes flattened (dictcore compares
   flat strings only).  New reference or JS_EXCEPTION. */
static JSValue qp_capi_dict_flat_key(JSContext *ctx, JSValueConst key)
{
    if (JS_VALUE_GET_TAG(key) == JS_TAG_STRING_ROPE)
        return js_linearize_string_rope(ctx, JS_DupValue(ctx, key));
    return JS_DupValue(ctx, key);
}

/* Prepared key + hash (hash computed unless `known`). */
static int qp_capi_dict_prep(JSContext *ctx, JSValueConst d, JSValueConst key,
                             const Py_hash_t *known, JSValue *pk, Py_hash_t *ph)
{
    if (known) {
        *pk = qp_capi_dict_flat_key(ctx, key);
        if (JS_IsException(*pk))
            return -1;
        *ph = *known;
        return 0;
    }
    return qp_dict_prepare_key(ctx, d, key, pk, ph);
}

/* d[key] without KeyError: 1 (*pvalue new ref), 0 absent, -1 error;
   *phash receives the key hash. */
static int qp_capi_dict_lookup(JSContext *ctx, JSValueConst d, JSValueConst key,
                               const Py_hash_t *known, JSValue *pvalue, Py_hash_t *phash)
{
    JSValue k, v;
    Py_hash_t h;
    Py_ssize_t ix;

    *pvalue = QP_NULL;
    if (qp_capi_dict_prep(ctx, d, key, known, &k, &h) < 0)
        return -1;
    ix = qp_dictcore_lookup(ctx, &QP_DICT(d)->core, k, h, qp_dict_eq_cb, NULL, &v);
    if (phash)
        *phash = h;
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

/* Lookup with C arguments: borrowed result pinned by the dict (NULL when
   absent, or on error with the exception pending). */
static PyObject *qp_capi_dict_get_borrowed(JSContext *ctx, PyObject *op, PyObject *key,
                                           const Py_hash_t *known, int *perr)
{
    JSValue d, k, v;
    Py_hash_t h;
    PyObject *res;
    int r;

    *perr = 0;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        goto error;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        goto error;
    r = qp_capi_dict_lookup(ctx, d, k, known, &v, &h);
    if (r < 0)
        goto error;
    if (r == 0)
        return NULL;
    res = qp_capi_borrow(ctx, op, QP_CAPI_DICT_VPIN(h), v);
    JS_FreeValue(ctx, v);
    if (!res)
        goto error;
    return res;
 error:
    *perr = 1;
    return NULL;
}

/* Lookup with C arguments, new reference: 1 / 0 / -1. */
static int qp_capi_dict_get_ref(JSContext *ctx, PyObject *op, PyObject *key,
                                const Py_hash_t *known, PyObject **result)
{
    JSValue d, k, v;
    int r;

    *result = NULL;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    r = qp_capi_dict_lookup(ctx, d, k, known, &v, NULL);
    if (r <= 0)
        return r;
    *result = qp_capi_steal(ctx, v);
    return *result ? 1 : -1;
}

/* dict_getitem: errors are reported as unraisable (except KeyError) and
   the previously pending exception is preserved. */
static PyObject *qp_capi_dict_getitem_quiet(JSContext *ctx, PyObject *op, PyObject *key,
                                            const char *warnmsg)
{
    QPCapiErrSave saved;
    PyObject *res = NULL;
    JSValue k, fk;
    Py_hash_t h = -1;
    int err = 1;

    if (!PyAnyDict_Check(op))
        return NULL;
    qp_capi_err_save(ctx, &saved);
    /* _PyObject_HashFast: a hash error is reported as is (no
       dict_unhashable_type rewrap). */
    k = qp_capi_value(ctx, key);
    if (!JS_IsException(k)) {
        fk = qp_capi_dict_flat_key(ctx, k);
        if (!JS_IsException(fk)) {
            h = JS_VALUE_GET_TAG(fk) == JS_TAG_STRING ? qp_str_hash(ctx, fk) : qp_hash(ctx, fk);
            JS_FreeValue(ctx, fk);
        }
    }
    if (h != -1)
        res = qp_capi_dict_get_borrowed(ctx, op, key, &h, &err);
    if (err) {
        if (PyErr_ExceptionMatches(PyExc_KeyError))
            qp_err_clear(ctx);
        else
            PyErr_FormatUnraisable("%s", warnmsg);
    }
    qp_capi_err_restore(ctx, &saved);
    return res;
}

PyObject *PyDict_New(void)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_dict_new(ctx));
}

PyObject *_PyDict_NewPresized(Py_ssize_t minused)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_dict_new_presized(ctx, minused));
}

PyObject *PyFrozenDict_New(PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst t = QP_TYPE_VALUE(QP_TYPE(ctx, frozendict));
    JSValue it;

    if (!iterable)
        return qp_capi_from_js_result(ctx, qp_call0(ctx, t));
    it = qp_capi_value(ctx, iterable);
    if (JS_IsException(it))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_call1(ctx, t, it));
}

PyObject *PyDict_GetItem(PyObject *op, PyObject *key)
{
    return qp_capi_dict_getitem_quiet(qp_capi.ctx, op, key,
                                      "Exception ignored in PyDict_GetItem(); consider using "
                                      "PyDict_GetItemRef() or PyDict_GetItemWithError()");
}

PyObject *PyDict_GetItemString(PyObject *v, const char *key)
{
    JSContext *ctx = qp_capi.ctx;
    static const char msg[] = "Exception ignored in PyDict_GetItemString(); consider using "
                              "PyDict_GetItemStringRef()";
    PyObject *kv, *rv;

    kv = PyUnicode_FromString(key);
    if (kv == NULL) {
        PyErr_FormatUnraisable("%s", msg);
        return NULL;
    }
    rv = qp_capi_dict_getitem_quiet(ctx, v, kv, msg);
    Py_DECREF(kv);
    return rv;
}

PyObject *PyDict_GetItemWithError(PyObject *op, PyObject *key)
{
    int err;
    if (!PyAnyDict_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qp_capi_dict_get_borrowed(qp_capi.ctx, op, key, NULL, &err);
}

PyObject *_PyDict_GetItem_KnownHash(PyObject *op, PyObject *key, Py_hash_t hash)
{
    int err;
    if (!PyAnyDict_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qp_capi_dict_get_borrowed(qp_capi.ctx, op, key, &hash, &err);
}

PyObject *_PyDict_GetItemStringWithError(PyObject *v, const char *key)
{
    PyObject *kv, *rv;
    kv = PyUnicode_FromString(key);
    if (kv == NULL)
        return NULL;
    rv = PyDict_GetItemWithError(v, kv);
    Py_DECREF(kv);
    return rv;
}

int PyDict_GetItemRef(PyObject *op, PyObject *key, PyObject **result)
{
    if (!PyAnyDict_Check(op)) {
        PyErr_BadInternalCall();
        *result = NULL;
        return -1;
    }
    return qp_capi_dict_get_ref(qp_capi.ctx, op, key, NULL, result);
}

int PyDict_GetItemStringRef(PyObject *v, const char *key, PyObject **result)
{
    PyObject *key_obj = PyUnicode_FromString(key);
    int res;
    if (key_obj == NULL) {
        *result = NULL;
        return -1;
    }
    res = PyDict_GetItemRef(v, key_obj, result);
    Py_DECREF(key_obj);
    return res;
}

int _PyDict_GetItemRef_KnownHash(PyDictObject *op, PyObject *key, Py_hash_t hash,
                                 PyObject **result)
{
    if (!PyAnyDict_Check((PyObject *)op)) {
        PyErr_BadInternalCall();
        *result = NULL;
        return -1;
    }
    return qp_capi_dict_get_ref(qp_capi.ctx, (PyObject *)op, key, &hash, result);
}

int _PyDict_GetItemRef_KnownHash_LockHeld(PyDictObject *op, PyObject *key, Py_hash_t hash,
                                          PyObject **result)
{
    return _PyDict_GetItemRef_KnownHash(op, key, hash, result);
}

int PyDict_SetItem(PyObject *op, PyObject *key, PyObject *value)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, k, v;

    if (qp_capi_dict_mutable(op, "assignment") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    v = qp_capi_value(ctx, value);
    if (JS_IsException(v))
        return -1;
    return qp_dict_set_item(ctx, d, k, v);
}

int PyDict_SetItemString(PyObject *v, const char *key, PyObject *item)
{
    PyObject *kv = PyUnicode_FromString(key);
    int err;
    if (kv == NULL)
        return -1;
    err = PyDict_SetItem(v, kv, item);
    Py_DECREF(kv);
    return err;
}

int _PyDict_SetItem_KnownHash(PyObject *op, PyObject *key, PyObject *value, Py_hash_t hash)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, k, v, fk;

    if (qp_capi_dict_mutable(op, "assignment") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    v = qp_capi_value(ctx, value);
    if (JS_IsException(v))
        return -1;
    fk = qp_capi_dict_flat_key(ctx, k);
    if (JS_IsException(fk))
        return -1;
    return qp_dictcore_setitem_take(ctx, &QP_DICT(d)->core, fk, hash, JS_DupValue(ctx, v),
                                    qp_dict_eq_cb, NULL);
}

int _PyDict_SetItem_KnownHash_LockHeld(PyDictObject *mp, PyObject *key, PyObject *value,
                                       Py_hash_t hash)
{
    return _PyDict_SetItem_KnownHash((PyObject *)mp, key, value, hash);
}

/* delete key: 0, or -1 (KeyError when absent). */
static int qp_capi_dict_del(JSContext *ctx, PyObject *op, PyObject *key, const Py_hash_t *known)
{
    JSValue d, k, fk;
    Py_hash_t h;
    int r;

    if (qp_capi_dict_mutable(op, "deletion") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    if (qp_capi_dict_prep(ctx, d, k, known, &fk, &h) < 0)
        return -1;
    r = qp_dictcore_delitem(ctx, &QP_DICT(d)->core, fk, h, qp_dict_eq_cb, NULL);
    JS_FreeValue(ctx, fk);
    if (r < 0)
        return -1;
    if (r == 0) {
        qp_raise_key_error(ctx, k);
        return -1;
    }
    return 0;
}

int PyDict_DelItem(PyObject *op, PyObject *key)
{
    return qp_capi_dict_del(qp_capi.ctx, op, key, NULL);
}

int _PyDict_DelItem_KnownHash(PyObject *op, PyObject *key, Py_hash_t hash)
{
    return qp_capi_dict_del(qp_capi.ctx, op, key, &hash);
}

int PyDict_DelItemString(PyObject *v, const char *key)
{
    PyObject *kv = PyUnicode_FromString(key);
    int err;
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

void PyDict_Clear(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d;

    if (!PyDict_Check(op))
        return;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d)) {
        qp_err_clear(ctx);
        return;
    }
    qp_dictcore_clear(ctx, &QP_DICT(d)->core);
}

void _PyDict_Clear_LockHeld(PyObject *op)
{
    PyDict_Clear(op);
}

/* PyDict_Next core: borrowed key/value pinned by the dict mirror. */
static int qp_capi_dict_next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey,
                             PyObject **pvalue, Py_hash_t *phash)
{
    JSContext *ctx = qp_capi.ctx;
    qp_dictcore *core;
    qp_dictentry *ep;
    Py_ssize_t ix;
    JSValue d;
    PyObject *k = NULL, *v = NULL;

    if (!PyAnyDict_Check(op))
        return 0;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return 0;
    core = &QP_DICT(d)->core;
    if (!qp_dictcore_next(core, ppos, &ix))
        return 0;
    ep = &core->keys->entries[ix];
    if (pkey) {
        k = qp_capi_borrow(ctx, op, QP_CAPI_DICT_KPIN(ep->hash), ep->key);
        if (!k)
            return 0;   /* out of memory: the exception stays pending */
        ep = &core->keys->entries[ix];
    }
    if (pvalue) {
        v = qp_capi_borrow(ctx, op, QP_CAPI_DICT_VPIN(ep->hash), ep->value);
        if (!v)
            return 0;
        ep = &core->keys->entries[ix];
    }
    if (pkey)
        *pkey = k;
    if (pvalue)
        *pvalue = v;
    if (phash)
        *phash = ep->hash;
    return 1;
}

int PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
    return qp_capi_dict_next(op, ppos, pkey, pvalue, NULL);
}

int _PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue,
                 Py_hash_t *phash)
{
    return qp_capi_dict_next(op, ppos, pkey, pvalue, phash);
}

/* PyDict_Keys / Values / Items */
static PyObject *qp_capi_dict_list(PyObject *op, int which)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d;

    if (op == NULL || !PyAnyDict_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return NULL;
    switch (which) {
    case 0:
        return qp_capi_from_js_result(ctx, qp_dict_keys_list(ctx, d));
    case 1:
        return qp_capi_from_js_result(ctx, qp_dict_values_list(ctx, d));
    default:
        return qp_capi_from_js_result(ctx, qp_dict_items_list(ctx, d));
    }
}

PyObject *PyDict_Keys(PyObject *op)
{
    return qp_capi_dict_list(op, 0);
}

PyObject *PyDict_Values(PyObject *op)
{
    return qp_capi_dict_list(op, 1);
}

PyObject *PyDict_Items(PyObject *op)
{
    return qp_capi_dict_list(op, 2);
}

Py_ssize_t PyDict_Size(PyObject *mp)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d;

    if (mp == NULL || !PyAnyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return -1;
    }
    d = qp_capi_e1_jv(ctx, mp);
    if (JS_IsException(d))
        return -1;
    return qp_dict_size(d);
}

PyObject *PyDict_Copy(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d;

    if (o == NULL || !PyDict_Check(o)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    d = qp_capi_e1_jv(ctx, o);
    if (JS_IsException(d))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_dict_copy(ctx, d));
}

/* Include/internal/pycore_dict.h (_elementtree): a new exact dict with the
   items of the dict or frozendict o. */
PyAPI_FUNC(PyObject *) _PyDict_CopyAsDict(PyObject *o);
PyObject *_PyDict_CopyAsDict(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d;

    assert(PyAnyDict_Check(o));
    d = qp_capi_e1_jv(ctx, o);
    if (JS_IsException(d))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_anydict_copy_as(ctx, d, 0));
}

int PyDict_Contains(PyObject *op, PyObject *key)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, k;

    if (!PyAnyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    return qp_dict_contains(ctx, d, k);
}

int PyDict_ContainsString(PyObject *op, const char *key)
{
    PyObject *key_obj = PyUnicode_FromString(key);
    int res;
    if (key_obj == NULL)
        return -1;
    res = PyDict_Contains(op, key_obj);
    Py_DECREF(key_obj);
    return res;
}

int PyDict_Merge(PyObject *a, PyObject *b, int override)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, o;

    /* dict_merge_api: a frozendict `a` is reported before a NULL `b` */
    if (a == NULL || (b == NULL && PyDict_Check(a))) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (qp_capi_dict_mutable(a, "assignment") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, a);
    if (JS_IsException(d))
        return -1;
    o = qp_capi_value(ctx, b);
    if (JS_IsException(o))
        return -1;
    return qp_dict_merge(ctx, d, o, override);
}

int PyDict_Update(PyObject *a, PyObject *b)
{
    return PyDict_Merge(a, b, 1);
}

int PyDict_MergeFromSeq2(PyObject *a, PyObject *seq2, int override)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, s;

    if (a == NULL || seq2 == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (qp_capi_dict_mutable(a, "assignment") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, a);
    if (JS_IsException(d))
        return -1;
    s = qp_capi_value(ctx, seq2);
    if (JS_IsException(s))
        return -1;
    return qp_dict_merge_from_seq2(ctx, d, s, override != 0);
}

/* dict_setdefault_ref: 0 inserted, 1 present, -1 error; *pres borrowed
   stored value (pinned) when pres != NULL. */
static int qp_capi_dict_setdefault(JSContext *ctx, PyObject *op, PyObject *key,
                                   PyObject *dflt, PyObject **pres, BOOL incref)
{
    JSValue d, k, v, fk, stored;
    Py_hash_t h;
    PyObject *res;
    int r;

    if (pres)
        *pres = NULL;
    if (qp_capi_dict_mutable(op, "assignment") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    v = qp_capi_value(ctx, dflt);
    if (JS_IsException(v))
        return -1;
    if (qp_dict_prepare_key(ctx, d, k, &fk, &h) < 0)
        return -1;
    r = qp_dictcore_setdefault(ctx, &QP_DICT(d)->core, fk, h, v, qp_dict_eq_cb, NULL, &stored);
    if (r < 0) {
        JS_FreeValue(ctx, fk);
        return -1;
    }
    if (pres) {
        stored = JS_DupValue(ctx, stored);
        JS_FreeValue(ctx, fk);
        if (incref) {
            res = qp_capi_steal(ctx, stored);
        } else {
            res = qp_capi_borrow(ctx, op, QP_CAPI_DICT_VPIN(h), stored);
            JS_FreeValue(ctx, stored);
        }
        if (!res)
            return -1;
        *pres = res;
    } else {
        JS_FreeValue(ctx, fk);
    }
    return r;
}

PyObject *PyDict_SetDefault(PyObject *d, PyObject *key, PyObject *defaultobj)
{
    PyObject *res;
    if (qp_capi_dict_setdefault(qp_capi.ctx, d, key, defaultobj, &res, FALSE) < 0)
        return NULL;
    return res;     /* borrowed */
}

int PyDict_SetDefaultRef(PyObject *d, PyObject *key, PyObject *default_value,
                         PyObject **result)
{
    return qp_capi_dict_setdefault(qp_capi.ctx, d, key, default_value, result, TRUE);
}

/* pop: 1 (*result owned), 0 absent, -1 error. */
static int qp_capi_dict_pop(JSContext *ctx, PyObject *op, PyObject *key, const Py_hash_t *known,
                            PyObject **result)
{
    JSValue d, k, fk, v;
    Py_hash_t h;
    int r;

    if (result)
        *result = NULL;
    if (qp_capi_dict_mutable(op, "deletion") < 0)
        return -1;
    d = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(d))
        return -1;
    /* An empty dict answers 0 before the key is looked at. */
    if (QP_DICT(d)->core.used == 0)
        return 0;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    if (!known) {
        r = qp_dict_pop_impl(ctx, d, k, &v);
    } else {
        if (qp_capi_dict_prep(ctx, d, k, known, &fk, &h) < 0)
            return -1;
        r = qp_dictcore_pop(ctx, &QP_DICT(d)->core, fk, h, qp_dict_eq_cb, NULL, &v);
        JS_FreeValue(ctx, fk);
    }
    if (r <= 0)
        return r;
    if (!result) {
        JS_FreeValue(ctx, v);
        return 1;
    }
    *result = qp_capi_steal(ctx, v);
    return *result ? 1 : -1;
}

int PyDict_Pop(PyObject *op, PyObject *key, PyObject **result)
{
    return qp_capi_dict_pop(qp_capi.ctx, op, key, NULL, result);
}

int _PyDict_Pop_KnownHash(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **result)
{
    return qp_capi_dict_pop(qp_capi.ctx, (PyObject *)mp, key, &hash, result);
}

int PyDict_PopString(PyObject *op, const char *key, PyObject **result)
{
    PyObject *key_obj = PyUnicode_FromString(key);
    int res;
    if (key_obj == NULL) {
        if (result != NULL)
            *result = NULL;
        return -1;
    }
    res = PyDict_Pop(op, key_obj, result);
    Py_DECREF(key_obj);
    return res;
}

PyObject *_PyDict_Pop(PyObject *dict, PyObject *key, PyObject *default_value)
{
    PyObject *result;
    if (PyDict_Pop(dict, key, &result) == 0) {
        if (default_value != NULL)
            return Py_NewRef(default_value);
        qp_raise_key_error(qp_capi.ctx, qp_capi_value(qp_capi.ctx, key));
        return NULL;
    }
    return result;
}

/* ---- dict watchers (Objects/dictobject.c) ------------------------------- */

/* The watcher bits of a dict are the low bits of its qp_dictcore.version
   (CPython _ma_watcher_tag), so mutations made by Python code reach
   qp_capi_dict_notify from the table code (dictcore.c).  A watched dict is
   found from its core through qp_capi_dict_watched (core -> JSObject*,
   entries added by PyDict_Watch and dropped by PyDict_Unwatch of the last
   bit or by the dict's finalizer).  PyDict_EVENT_DEALLOCATED is reported
   from the finalizer queue: the free of a watched dict is deferred there
   (qp_capi_dealloc_watched), as CPython's dict_dealloc resurrects it for
   the callbacks. */
#define QP_CAPI_DICT_MAX_WATCHERS 8         /* DICT_MAX_WATCHERS */
#define QP_CAPI_DICT_FIRST_WATCHER 3        /* FIRST_AVAILABLE_WATCHER: 0-2 are CPython's */

static_assert(PyDict_EVENT_ADDED == QP_DC_EVENT_ADDED &&
              PyDict_EVENT_MODIFIED == QP_DC_EVENT_MODIFIED &&
              PyDict_EVENT_DELETED == QP_DC_EVENT_DELETED &&
              PyDict_EVENT_CLONED == QP_DC_EVENT_CLONED &&
              PyDict_EVENT_CLEARED == QP_DC_EVENT_CLEARED &&
              PyDict_EVENT_DEALLOCATED == QP_DC_EVENT_DEALLOCATED,
              "dictcore watcher events");
static_assert((1 << QP_CAPI_DICT_MAX_WATCHERS) - 1 == QP_DC_WATCHER_MASK,
              "dict watcher bits");

static PyDict_WatchCallback qp_capi_dict_watchers[QP_CAPI_DICT_MAX_WATCHERS];
static QPCapiMap *qp_capi_dict_watched;

static const char *qp_capi_dict_event_name(int event)
{
    switch (event) {
#define QP_CAPI_DICT_EVENT_CASE(op) case PyDict_EVENT_##op: return "PyDict_EVENT_" #op;
    PY_FOREACH_DICT_EVENT(QP_CAPI_DICT_EVENT_CASE)
#undef QP_CAPI_DICT_EVENT_CASE
    }
    return "?";
}

/* _PyDict_SendEvent */
static void qp_capi_dict_notify(JSContext *ctx, qp_dictcore *d, int event,
                                JSValueConst key, JSValueConst value)
{
    unsigned bits = (unsigned)(d->version & QP_DC_WATCHER_MASK) & qp_capi_dict_watchers_set;
    JSObject *p;
    PyObject *mp, *k = NULL, *v = NULL;
    int i;

    if (!bits || qp_capi_dead)
        return;
    p = qp_capi_map_get(qp_capi_dict_watched, (uintptr_t)d);
    if (!p)
        return;
    mp = qp_capi_new_ref(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    if (mp && JS_VALUE_GET_TAG(key) != JS_TAG_UNINITIALIZED && !(k = qp_capi_new_ref(ctx, key)))
        Py_CLEAR(mp);
    if (mp && JS_VALUE_GET_TAG(value) != JS_TAG_UNINITIALIZED &&
        !(v = qp_capi_new_ref(ctx, value)))
        Py_CLEAR(mp);
    if (!mp) {
        Py_XDECREF(k);
        PyErr_FormatUnraisable("Exception ignored in %s watcher callback for <dict at %p>",
                               qp_capi_dict_event_name(event), (void *)p);
        return;
    }
    for (i = 0; bits; i++, bits >>= 1) {
        PyDict_WatchCallback cb;
        if (!(bits & 1))
            continue;
        cb = qp_capi_dict_watchers[i];
        if (cb && cb((PyDict_WatchEvent)event, mp, k, v) < 0) {
            /* CPython does not pass the dict as the context (its repr can
               run arbitrary code) */
            PyErr_FormatUnraisable("Exception ignored in %s watcher callback for <dict at %p>",
                                   qp_capi_dict_event_name(event), (void *)mp);
        }
    }
    Py_XDECREF(v);
    Py_XDECREF(k);
    Py_DECREF(mp);
}

static void qp_capi_dict_forget(qp_dictcore *d)
{
    qp_capi_map_del(qp_capi_dict_watched, (uintptr_t)d);
}

/* dict_dealloc's PyDict_EVENT_DEALLOCATED (finalizer queue, p alive). */
static void qp_capi_dict_dealloc_notify(JSContext *ctx, JSObject *p)
{
    QPDict *d = p->u.opaque;
    if (d && (d->core.version & QP_DC_WATCHER_MASK))
        qp_capi_dict_notify(ctx, &d->core, QP_DC_EVENT_DEALLOCATED,
                            JS_UNINITIALIZED, JS_UNINITIALIZED);
}

static int qp_capi_dict_validate_watcher(int watcher_id)
{
    if (watcher_id < 0 || watcher_id >= QP_CAPI_DICT_MAX_WATCHERS) {
        PyErr_Format(PyExc_ValueError, "Invalid dict watcher ID %d", watcher_id);
        return -1;
    }
    if (!qp_capi_dict_watchers[watcher_id]) {
        PyErr_Format(PyExc_ValueError, "No dict watcher set for ID %d", watcher_id);
        return -1;
    }
    return 0;
}

/* The dict core of a watch / unwatch target, NULL + ValueError. */
static QPDict *qp_capi_dict_watch_target(JSContext *ctx, PyObject *dict, int watcher_id,
                                         JSObject **pp)
{
    JSValue v;
    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_ValueError, "Cannot watch non-dictionary");
        return NULL;
    }
    if (qp_capi_dict_validate_watcher(watcher_id) < 0)
        return NULL;
    v = qp_capi_value(ctx, dict);
    if (JS_IsException(v))
        return NULL;
    if (!qp_dict_check(ctx, v) || !QP_DICT(v)) {
        PyErr_SetString(PyExc_ValueError, "Cannot watch non-dictionary");
        return NULL;
    }
    *pp = JS_VALUE_GET_OBJ(v);
    return QP_DICT(v);
}

int PyDict_Watch(int watcher_id, PyObject *dict)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;
    QPDict *d = qp_capi_dict_watch_target(ctx, dict, watcher_id, &p);

    if (!d)
        return -1;
    if (!qp_capi_dict_watched && !(qp_capi_dict_watched = qp_capi_map_new())) {
        PyErr_NoMemory();
        return -1;
    }
    if (qp_capi_map_put(qp_capi_dict_watched, (uintptr_t)&d->core, p) < 0) {
        PyErr_NoMemory();
        return -1;
    }
    d->core.version |= (uint64_t)1 << watcher_id;
    return 0;
}

int PyDict_Unwatch(int watcher_id, PyObject *dict)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;
    QPDict *d = qp_capi_dict_watch_target(ctx, dict, watcher_id, &p);

    if (!d)
        return -1;
    d->core.version &= ~((uint64_t)1 << watcher_id);
    if (!(d->core.version & QP_DC_WATCHER_MASK))
        qp_capi_dict_forget(&d->core);
    return 0;
}

int PyDict_AddWatcher(PyDict_WatchCallback callback)
{
    int i;
    /* Some watchers are reserved for CPython, start at the first available one */
    for (i = QP_CAPI_DICT_FIRST_WATCHER; i < QP_CAPI_DICT_MAX_WATCHERS; i++) {
        if (!qp_capi_dict_watchers[i]) {
            qp_capi_dict_watchers[i] = callback;
            qp_capi_dict_watchers_set |= (uint8_t)(1 << i);
            return i;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "no more dict watcher IDs available");
    return -1;
}

/* The dicts keep their watcher bits (CPython); events of a cleared ID are
   no longer delivered. */
int PyDict_ClearWatcher(int watcher_id)
{
    if (qp_capi_dict_validate_watcher(watcher_id) < 0)
        return -1;
    qp_capi_dict_watchers[watcher_id] = NULL;
    qp_capi_dict_watchers_set &= (uint8_t)~(1 << watcher_id);
    return 0;
}

/* qp_capi_runtime_clear: the watchers are per interpreter. */
static void qp_capi_dict_watchers_clear(void)
{
    memset(qp_capi_dict_watchers, 0, sizeof(qp_capi_dict_watchers));
    qp_capi_dict_watchers_set = 0;
    qp_capi_map_free(qp_capi_dict_watched);
    qp_capi_dict_watched = NULL;
}

/* ========================================================================= */
/* SET                                                                        */
/* ========================================================================= */

PyObject *PySet_New(PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue it = qp_capi_e1_opt(ctx, iterable);
    if (JS_IsException(it))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_set_new(ctx, it));
}

PyObject *PyFrozenSet_New(PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue it = qp_capi_e1_opt(ctx, iterable);
    if (JS_IsException(it))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_frozenset_new(ctx, it));
}

Py_ssize_t PySet_Size(PyObject *anyset)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s;

    if (!PyAnySet_Check(anyset)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_e1_jv(ctx, anyset);
    if (JS_IsException(s))
        return -1;
    return qp_set_size(s);
}

int PySet_Clear(PyObject *set)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s;

    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return -1;
    qp_setcore_clear(ctx, &QP_SET(s)->core);
    return 0;
}

/* set operation on (set, key): 0 contains, 1 discard, 2 add */
static int qp_capi_set_key_op(PyObject *set, PyObject *key, int op)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, k;

    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    switch (op) {
    case 0:
        return qp_set_contains(ctx, s, k);
    case 1:
        return qp_set_discard(ctx, s, k);
    default:
        return qp_set_add(ctx, s, k);
    }
}

int PySet_Contains(PyObject *anyset, PyObject *key)
{
    if (!PyAnySet_Check(anyset)) {
        PyErr_BadInternalCall();
        return -1;
    }
    /* set.__contains__ looks an (unhashable) set key up as a frozenset;
       PySet_Contains does not: set_unhashable_type */
    if (PySet_Check(key) && PyObject_Hash(key) == -1) {
        PyObject *exc = PyErr_GetRaisedException();
        if (!Py_IS_TYPE(exc, (PyTypeObject *)PyExc_TypeError)) {
            PyErr_SetRaisedException(exc);
            return -1;
        }
        PyErr_Format(PyExc_TypeError, "cannot use '%T' as a set element (%S)", key, exc);
        Py_DECREF(exc);
        return -1;
    }
    return qp_capi_set_key_op(anyset, key, 0);
}

int PySet_Discard(PyObject *set, PyObject *key)
{
    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return qp_capi_set_key_op(set, key, 1);
}

int PySet_Add(PyObject *anyset, PyObject *key)
{
    if (!PySet_Check(anyset) &&
        (!PyFrozenSet_Check(anyset) || Py_REFCNT(anyset) != 1)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return qp_capi_set_key_op(anyset, key, 2);
}

PyObject *PySet_Pop(PyObject *set)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, k;

    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return NULL;
    if (!qp_setcore_pop(&QP_SET(s)->core, &k)) {
        PyErr_SetString(PyExc_KeyError, "pop from an empty set");
        return NULL;
    }
    return qp_capi_steal(ctx, k);
}

int _PySet_NextEntryRef(PyObject *set, Py_ssize_t *pos, PyObject **key, Py_hash_t *hash)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, k;
    Py_hash_t h;

    if (!PyAnySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return -1;
    if (!qp_set_next(s, pos, &k, &h))
        return 0;
    *key = qp_capi_new_ref(ctx, k);
    if (!*key)
        return -1;
    *hash = h;
    return 1;
}

/* Borrowed key, pinned by the set mirror (hashed slot of the key). */
int _PySet_NextEntry(PyObject *set, Py_ssize_t *pos, PyObject **key, Py_hash_t *hash)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, k;
    Py_hash_t h;

    if (!PyAnySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return -1;
    if (!qp_set_next(s, pos, &k, &h))
        return 0;
    *key = qp_capi_borrow(ctx, set, QP_CAPI_PIN_HASHED(h), k);
    if (!*key)
        return -1;
    *hash = h;
    return 1;
}

int _PySet_Update(PyObject *set, PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, it;

    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_e1_jv(ctx, set);
    if (JS_IsException(s))
        return -1;
    it = qp_capi_value(ctx, iterable);
    if (JS_IsException(it))
        return -1;
    return qp_set_update(ctx, s, it);
}

/* ========================================================================= */
/* PyCapsule (port of Objects/capsule.c)                                      */
/* ========================================================================= */

typedef struct {
    PyObject_HEAD
    void *pointer;
    const char *name;
    void *context;
    PyCapsule_Destructor destructor;
    traverseproc traverse_func;
    inquiry clear_func;
} QPCapiCapsule;

static int qp_capi_capsule_legal(PyObject *op, const char *invalid_capsule)
{
    if (!op || !PyCapsule_CheckExact(op))
        goto error;
    if (((QPCapiCapsule *)op)->pointer == NULL)
        goto error;
    return 1;
 error:
    PyErr_SetString(PyExc_ValueError, invalid_capsule);
    return 0;
}

#define QP_CAPI_CAPSULE_LEGAL(capsule, name) \
    (qp_capi_capsule_legal(capsule, name " called with invalid PyCapsule object"))

static int qp_capi_capsule_name_matches(const char *name1, const char *name2)
{
    /* if either is NULL, they're only the same if they're both NULL */
    if (!name1 || !name2)
        return name1 == name2;
    return !strcmp(name1, name2);
}

PyObject *PyCapsule_New(void *pointer, const char *name, PyCapsule_Destructor destructor)
{
    QPCapiCapsule *capsule;

    if (!pointer) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_New called with null pointer");
        return NULL;
    }
    capsule = PyObject_GC_New(QPCapiCapsule, &PyCapsule_Type);
    if (capsule == NULL)
        return NULL;
    capsule->pointer = pointer;
    capsule->name = name;
    capsule->context = NULL;
    capsule->destructor = destructor;
    capsule->traverse_func = NULL;
    capsule->clear_func = NULL;
    /* only tracked by the GC when _PyCapsule_SetTraverse() is called */
    return (PyObject *)capsule;
}

int PyCapsule_IsValid(PyObject *op, const char *name)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)op;
    return capsule != NULL && PyCapsule_CheckExact(op) && capsule->pointer != NULL &&
        qp_capi_capsule_name_matches(capsule->name, name);
}

void *PyCapsule_GetPointer(PyObject *op, const char *name)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_GetPointer"))
        return NULL;
    if (!qp_capi_capsule_name_matches(name, ((QPCapiCapsule *)op)->name)) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_GetPointer called with incorrect name");
        return NULL;
    }
    return ((QPCapiCapsule *)op)->pointer;
}

const char *PyCapsule_GetName(PyObject *op)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_GetName"))
        return NULL;
    return ((QPCapiCapsule *)op)->name;
}

PyCapsule_Destructor PyCapsule_GetDestructor(PyObject *op)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_GetDestructor"))
        return NULL;
    return ((QPCapiCapsule *)op)->destructor;
}

void *PyCapsule_GetContext(PyObject *op)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_GetContext"))
        return NULL;
    return ((QPCapiCapsule *)op)->context;
}

int PyCapsule_SetPointer(PyObject *op, void *pointer)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_SetPointer"))
        return -1;
    if (!pointer) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_SetPointer called with null pointer");
        return -1;
    }
    ((QPCapiCapsule *)op)->pointer = pointer;
    return 0;
}

int PyCapsule_SetName(PyObject *op, const char *name)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_SetName"))
        return -1;
    ((QPCapiCapsule *)op)->name = name;
    return 0;
}

int PyCapsule_SetDestructor(PyObject *op, PyCapsule_Destructor destructor)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_SetDestructor"))
        return -1;
    ((QPCapiCapsule *)op)->destructor = destructor;
    return 0;
}

int PyCapsule_SetContext(PyObject *op, void *context)
{
    if (!QP_CAPI_CAPSULE_LEGAL(op, "PyCapsule_SetContext"))
        return -1;
    ((QPCapiCapsule *)op)->context = context;
    return 0;
}

int _PyCapsule_SetTraverse(PyObject *op, traverseproc traverse_func, inquiry clear_func)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)op;

    if (!QP_CAPI_CAPSULE_LEGAL(op, "_PyCapsule_SetTraverse"))
        return -1;
    if (traverse_func == NULL || clear_func == NULL) {
        PyErr_SetString(PyExc_ValueError, "_PyCapsule_SetTraverse() called with NULL callback");
        return -1;
    }
    if (!PyObject_GC_IsTracked(op))
        PyObject_GC_Track(op);
    capsule->traverse_func = traverse_func;
    capsule->clear_func = clear_func;
    return 0;
}

void *PyCapsule_Import(const char *name, int no_block)
{
    PyObject *object = NULL;
    void *return_value = NULL;
    char *trace;
    size_t name_length = (strlen(name) + 1) * sizeof(char);
    char *name_dup = (char *)PyMem_Malloc(name_length);

    if (!name_dup)
        return PyErr_NoMemory();
    memcpy(name_dup, name, name_length);
    trace = name_dup;
    while (trace) {
        char *dot = strchr(trace, '.');
        if (dot)
            *dot++ = '\0';
        if (object == NULL) {
            object = PyImport_ImportModule(trace);
            if (!object)
                PyErr_Format(PyExc_ImportError,
                             "PyCapsule_Import could not import module \"%s\"", trace);
        } else {
            PyObject *object2 = PyObject_GetAttrString(object, trace);
            Py_SETREF(object, object2);
        }
        if (!object)
            goto EXIT;
        trace = dot;
    }
    /* compare attribute name to module.name by hand */
    if (PyCapsule_IsValid(object, name))
        return_value = ((QPCapiCapsule *)object)->pointer;
    else
        PyErr_Format(PyExc_AttributeError, "PyCapsule_Import \"%s\" is not valid", name);
 EXIT:
    Py_XDECREF(object);
    PyMem_Free(name_dup);
    return return_value;
}

static void qp_capi_capsule_dealloc(PyObject *op)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)op;
    PyObject_GC_UnTrack(op);
    if (capsule->destructor)
        capsule->destructor(op);
    PyObject_GC_Del(op);
}

static PyObject *qp_capi_capsule_repr(PyObject *o)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)o;
    const char *name, *quote;

    if (capsule->name) {
        quote = "\"";
        name = capsule->name;
    } else {
        quote = "";
        name = "NULL";
    }
    return PyUnicode_FromFormat("<capsule object %s%s%s at %p>", quote, name, quote, capsule);
}

static int qp_capi_capsule_traverse(PyObject *self, visitproc visit, void *arg)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)self;
    if (capsule->traverse_func != NULL)
        return capsule->traverse_func(self, visit, arg);
    return 0;
}

static int qp_capi_capsule_clear(PyObject *self)
{
    QPCapiCapsule *capsule = (QPCapiCapsule *)self;
    if (capsule->clear_func != NULL)
        return capsule->clear_func(self);
    return 0;
}

PyTypeObject PyCapsule_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "PyCapsule",
    .tp_basicsize = sizeof(QPCapiCapsule),
    .tp_dealloc = qp_capi_capsule_dealloc,
    .tp_repr = qp_capi_capsule_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = PyDoc_STR(
        "Capsule objects let you wrap a C \"void *\" pointer in a Python\n"
        "object.  They're a way of passing data through the Python interpreter\n"
        "without creating your own custom type.\n"
        "\n"
        "Capsules are used for communication between extension modules.\n"
        "They provide a way for an extension module to export a C interface\n"
        "to other extension modules, so that extension modules can use the\n"
        "Python import mechanism to link to one another.\n"),
    .tp_traverse = qp_capi_capsule_traverse,
    .tp_clear = qp_capi_capsule_clear,
};

/* ========================================================================= */
/* Weak references                                                            */
/* ========================================================================= */

PyTypeObject _PyWeakref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.ReferenceType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
};

PyTypeObject _PyWeakref_ProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.ProxyType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
};

PyTypeObject _PyWeakref_CallableProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.CallableProxyType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
};

/* Record of a weakref mirror, NULL + exception when ref is not one. */
static QPWeakRec *qp_capi_weakref_rec(JSContext *ctx, PyObject *ref, BOOL type_error)
{
    JSValue v;
    QPWeakRec *rec;

    if (ref == NULL || !PyWeakref_Check(ref))
        goto bad;
    v = qp_capi_e1_jv(ctx, ref);
    if (JS_IsException(v))
        return NULL;
    rec = qp_wr_rec(ctx, v);
    if (rec)
        return rec;
 bad:
    if (type_error)
        PyErr_SetString(PyExc_TypeError, "expected a weakref");
    else
        PyErr_BadInternalCall();
    return NULL;
}

static PyObject *qp_capi_weakref_make(PyObject *ob, PyObject *callback, BOOL proxy)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue o, cb = QP_NULL;

    o = qp_capi_value(ctx, ob);
    if (JS_IsException(o))
        return NULL;
    if (callback && callback != Py_None) {
        cb = qp_capi_value(ctx, callback);
        if (JS_IsException(cb))
            return NULL;
    }
    return qp_capi_from_js_result(ctx, proxy ? qp_weakref_new_proxy(ctx, o, cb)
                                    : qp_weakref_new(ctx, o, cb));
}

PyObject *PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    return qp_capi_weakref_make(ob, callback, FALSE);
}

PyObject *PyWeakref_NewProxy(PyObject *ob, PyObject *callback)
{
    return qp_capi_weakref_make(ob, callback, TRUE);
}

int PyWeakref_GetRef(PyObject *ref, PyObject **pobj)
{
    JSContext *ctx = qp_capi.ctx;
    QPWeakRec *rec;
    JSValue v;

    *pobj = NULL;
    if (ref == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    rec = qp_capi_weakref_rec(ctx, ref, TRUE);
    if (!rec)
        return -1;
    if (!rec->target)
        return 0;
    v = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, rec->target));
    *pobj = qp_capi_steal(ctx, v);
    return *pobj ? 1 : -1;
}

/* Borrowed referent (None when dead), pinned by the weakref mirror. */
PyObject *PyWeakref_GetObject(PyObject *ref)
{
    JSContext *ctx = qp_capi.ctx;
    QPWeakRec *rec = qp_capi_weakref_rec(ctx, ref, FALSE);
    uint64_t key = QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_WEAKREF);

    if (!rec)
        return NULL;
    if (!rec->target) {
        qp_capi_pin_drop(ref, key);
        return Py_None;
    }
    {
        /* the referent is held by C already (ob_refcnt >= 1): borrow that
           reference, as CPython does, without counting one more */
        QPCapiHead *h = qp_capi_head_of_jobj(rec->target);
        if (h && (h->flags & QP_CAPI_F_LIVE) && !(h->flags & QP_CAPI_F_ORPHAN)) {
            PyObject *pinned = qp_capi_pin_get(ref, key);
            if (pinned == QP_CAPI_OBJ(h))
                return pinned;
            if (pinned)
                qp_capi_pin_drop(ref, key);
            return QP_CAPI_OBJ(h);
        }
    }
    return qp_capi_borrow(ctx, ref, key, JS_MKPTR(JS_TAG_OBJECT, rec->target));
}

int PyWeakref_IsDead(PyObject *ref)
{
    JSContext *ctx = qp_capi.ctx;
    QPWeakRec *rec;

    if (ref == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyWeakref_Check(ref)) {
        PyErr_Format(PyExc_TypeError, "expected a weakref, got %T", ref);
        return -1;
    }
    rec = qp_capi_weakref_rec(ctx, ref, TRUE);
    if (!rec)
        return -1;
    return rec->target == NULL;
}

/* Clear the reference without calling the callback (the callback stays
   set, as in CPython). */
void _PyWeakref_ClearRef(PyWeakReference *self)
{
    JSContext *ctx = qp_capi.ctx;
    QPWeakRec *rec = qp_capi_weakref_rec(ctx, (PyObject *)self, TRUE);

    if (!rec) {
        qp_err_clear(ctx);
        return;
    }
    qp_wr_unlink(ctx->rt, rec);
}

/* ========================================================================= */
/* Init step of the containers (capi.h (E1))                                  */
/* ========================================================================= */

static int qp_capi_init_containers(JSContext *ctx)
{
    static PyTypeObject *const wr_types[3] = {
        &_PyWeakref_RefType, &_PyWeakref_ProxyType, &_PyWeakref_CallableProxyType,
    };
    QPModState *ms;
    int i;

    qp_capi_init_sequence_kinds();
    qp_capi.kind_ops[QP_CAPI_K_DICT] = (QPCapiKindOps){ .name = "dict" };
    qp_capi.kind_ops[QP_CAPI_K_SET] = (QPCapiKindOps){ .name = "set" };
    qp_capi.kind_ops[QP_CAPI_K_WEAKREF] = (QPCapiKindOps){ .name = "weakref" };

    if (PyType_Ready(&PyCapsule_Type) < 0)
        return -1;

    ms = qp_wr_state(ctx);
    if (!ms)
        return -1;
    for (i = 0; i < 3; i++) {
        QPType *t = qp_type_from_value(ms->wr_types[i]);
        if (!t) {
            qp_raise_system_error(ctx, "_weakref types are not initialized");
            return -1;
        }
        if (!wr_types[i]->tp_base)
            wr_types[i]->tp_base = &PyBaseObject_Type;
        if (qp_capi_bind_static_type(ctx, wr_types[i], t) < 0)
            return -1;
    }
    return 0;
}
