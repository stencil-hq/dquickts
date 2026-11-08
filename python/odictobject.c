/*
 * odictobject.c - collections.OrderedDict, its views (odict_keys,
 * odict_values, odict_items) and odict_iterator for dquickts Python
 * (object.h section (f)).
 *
 * Derived from CPython Objects/odictobject.c (PSF-2.0).
 *
 * An OrderedDict is a dict: a JS_CLASS_PY_DICT object (QPDict payload)
 * whose QPDict.od holds CPython's doubly-linked list of keys (QPODict,
 * object.h).  As in CPython, OrderedDict has no tp_new of its own (dict's
 * is inherited), so the list is created by the first OrderedDict operation
 * on an instance; one created by dict.__new__() or made an OrderedDict by
 * __class__ assignment starts with its keys in dict order.  The plain dict
 * API (dict.__setitem__(od, k, v), PyDict_SetItem, ...) bypasses the list,
 * as in CPython; the OrderedDict operations then report the same
 * inconsistencies (KeyError while iterating).
 *
 * fast_nodes maps a dict entry index (qp_dictcore_lookup) to its node and is
 * rebuilt whenever the dict's keys table was replaced (CPython _odict_resize
 * with od_resize_sentinel).  Lookups run key __eq__ code, which may mutate
 * the OrderedDict: list walks re-check `state` before following a node
 * pointer, and an index is only used against a fast_nodes table built for
 * the table the lookup ran on.
 *
 * Views are QPDictView payloads (JS_CLASS_PY_DICT_VIEW) of subtypes of
 * dict_keys / dict_values / dict_items that replace tp_iter and
 * __reversed__.  odict_iterator is a PY_ITER instance (QPIterObj,
 * iterobject.c): seq = the OrderedDict (CPython di_odict; QP_NULL once
 * released), aux = the next key (di_current), index = QP_ODICT_ITER_* kind,
 * pos = di_size, start = di_state.
 */

#define QP_ODICT_ITER_REVERSED 1
#define QP_ODICT_ITER_KEYS     2
#define QP_ODICT_ITER_VALUES   4
#define QP_ODICT_ITER_ITEMS    (QP_ODICT_ITER_KEYS | QP_ODICT_ITER_VALUES)

/* sizeof(PyODictObject) and sizeof(_ODictNode) on 64-bit CPython (__sizeof__,
   __basicsize__) */
#define QP_ODICT_BASICSIZE 112
#define QP_ODICT_NODESIZE  32

#define QP_ODICT_MUTATED "OrderedDict mutated during iteration"

/* ========================================================================= */
/* The linked list                                                           */
/* ========================================================================= */

static void qp_odict_node_free(JSRuntime *rt, QPODictNode *node)
{
    JS_FreeValueRT(rt, node->key);
    js_free_rt(rt, node);
}

static void qp_odict_state_free(JSRuntime *rt, QPODict *o)
{
    QPODictNode *node = o->first, *next;

    while (node) {
        next = node->next;
        qp_odict_node_free(rt, node);
        node = next;
    }
    js_free_rt(rt, o->fast_nodes);
    js_free_rt(rt, o);
}

/* odict_traverse (the node keys) */
static void qp_odict_state_mark(JSRuntime *rt, const QPODict *o, JS_MarkFunc *mark_func)
{
    const QPODictNode *node;

    for (node = o->first; node; node = node->next)
        JS_MarkValue(rt, node->key, mark_func);
}

static void qp_odict_add_head(QPODict *o, QPODictNode *node)
{
    node->prev = NULL;
    node->next = o->first;
    if (!o->first)
        o->last = node;
    else
        o->first->prev = node;
    o->first = node;
    o->state++;
}

static void qp_odict_add_tail(QPODict *o, QPODictNode *node)
{
    node->prev = o->last;
    node->next = NULL;
    if (!o->last)
        o->first = node;
    else
        o->last->next = node;
    o->last = node;
    o->state++;
}

/* _odict_remove_node (repeated calls on the same node are no-ops) */
static void qp_odict_remove_node(QPODict *o, QPODictNode *node)
{
    if (o->first == node)
        o->first = node->next;
    else if (node->prev)
        node->prev->next = node->next;
    if (o->last == node)
        o->last = node->prev;
    else if (node->next)
        node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    o->state++;
}

/* _odict_clear_nodes */
static void qp_odict_clear_nodes(JSContext *ctx, QPODict *o)
{
    QPODictNode *node, *next;

    js_free(ctx, o->fast_nodes);
    o->fast_nodes = NULL;
    o->fast_nodes_size = 0;
    o->resize_keys = NULL;
    node = o->first;
    o->first = NULL;
    o->last = NULL;
    o->state++;
    while (node) {
        next = node->next;
        qp_odict_node_free(ctx->rt, node);
        node = next;
    }
}

/* The list of an OrderedDict instance, created by its first OrderedDict
   operation with the dict's keys in dict order (see the file comment).
   NULL on memory error. */
static QPODict *qp_odict_get(JSContext *ctx, JSValueConst od)
{
    QPDict *d = QP_DICT(od);
    QPODict *o = d->od;
    Py_ssize_t pos = 0, ix;

    if (o)
        return o;
    o = js_mallocz(ctx, sizeof(*o));
    if (!o)
        return NULL;
    while (qp_dictcore_next(&d->core, &pos, &ix)) {
        const qp_dictentry *ep = &d->core.keys->entries[ix];
        QPODictNode *node = js_malloc(ctx, sizeof(*node));
        if (!node) {
            qp_odict_state_free(ctx->rt, o);
            return NULL;
        }
        node->key = JS_DupValue(ctx, ep->key);
        node->hash = ep->hash;
        qp_odict_add_tail(o, node);
    }
    d->od = o;
    return o;
}

/* PyObject_Hash of a key for the list (no dict-key rewrap of the error);
   *pkey is a new reference with ropes flattened (dictcore compares flat
   strs).  0 / -1. */
static int qp_odict_hash_key(JSContext *ctx, JSValueConst key, JSValue *pkey, Py_hash_t *phash)
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
    h = JS_VALUE_GET_TAG(k) == JS_TAG_STRING ? qp_str_hash(ctx, k) : qp_hash(ctx, k);
    if (h == -1) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    *pkey = k;
    *phash = h;
    return 0;
}

/* _odict_get_index_raw: the entry index of key in the dict table, the index
   a new entry would get when absent, -1 on error. */
static Py_ssize_t qp_odict_index_raw(JSContext *ctx, qp_dictcore *core, JSValueConst key,
                                     Py_hash_t hash)
{
    Py_ssize_t ix = qp_dictcore_lookup(ctx, core, key, hash, QP_DICT_EQ, NULL);

    if (ix == QP_DKIX_EMPTY)
        return core->keys->nentries;
    return ix < 0 ? -1 : ix;
}

/* Whether fast_nodes was built for the dict's current keys table. */
static inline BOOL qp_odict_synced(const QPODict *o, const qp_dictcore *core)
{
    return o->fast_nodes && o->resize_keys == core->keys && o->resize_gen == core->keys_gen &&
        o->fast_nodes_size == ((Py_ssize_t)1 << core->keys->log2_size);
}

/* _odict_resize: a fast_nodes table matching the dict's keys table. */
static int qp_odict_resize(JSContext *ctx, JSValueConst od, QPODict *o)
{
    qp_dictcore *core = &QP_DICT(od)->core;
    const qp_dictkeys *keys = core->keys;
    uint64_t gen = core->keys_gen, state = o->state;
    Py_ssize_t size = (Py_ssize_t)1 << keys->log2_size, i;
    QPODictNode **fast_nodes, *node;

    fast_nodes = js_mallocz(ctx, sizeof(fast_nodes[0]) * size);
    if (!fast_nodes)
        return -1;
    for (node = o->first; node; node = node->next) {
        /* the key's __eq__ may free the node */
        JSValue key = JS_DupValue(ctx, node->key);
        i = qp_odict_index_raw(ctx, core, key, node->hash);
        JS_FreeValue(ctx, key);
        if (i < 0) {
            js_free(ctx, fast_nodes);
            return -1;
        }
        /* a key's __eq__ may have changed the list or the dict table */
        if (o->state != state || core->keys != keys || core->keys_gen != gen) {
            js_free(ctx, fast_nodes);
            qp_raise_runtime_error(ctx, QP_ODICT_MUTATED);
            return -1;
        }
        fast_nodes[i] = node;
    }
    js_free(ctx, o->fast_nodes);
    o->fast_nodes = fast_nodes;
    o->fast_nodes_size = size;
    o->resize_keys = keys;
    o->resize_gen = gen;
    return 0;
}

/* _odict_get_index: the fast_nodes index of key (fast_nodes synced with the
   table the lookup ran on), -1 on error. */
static Py_ssize_t qp_odict_get_index(JSContext *ctx, JSValueConst od, QPODict *o,
                                     JSValueConst key, Py_hash_t hash)
{
    qp_dictcore *core = &QP_DICT(od)->core;
    Py_ssize_t ix;

    for (;;) {
        if (!qp_odict_synced(o, core) && qp_odict_resize(ctx, od, o) < 0)
            return -1;
        ix = qp_odict_index_raw(ctx, core, key, hash);
        if (ix < 0)
            return -1;
        /* else a key's __eq__ replaced the dict table: look up again */
        if (qp_odict_synced(o, core))
            return ix;
    }
}

/* _odict_find_node_hash: 1 with *pnode, 0 absent, -1 error. */
static int qp_odict_find_node_hash(JSContext *ctx, JSValueConst od, QPODict *o,
                                   JSValueConst key, Py_hash_t hash, QPODictNode **pnode)
{
    Py_ssize_t ix;

    *pnode = NULL;
    if (!o->first)
        return 0;
    ix = qp_odict_get_index(ctx, od, o, key, hash);
    if (ix < 0)
        return -1;
    *pnode = o->fast_nodes[ix];
    return *pnode != NULL;
}

/* _odict_find_node: same, hashing key (not hashed when the list is empty). */
static int qp_odict_find_node(JSContext *ctx, JSValueConst od, QPODict *o,
                              JSValueConst key, QPODictNode **pnode)
{
    JSValue k;
    Py_hash_t hash;
    int r;

    *pnode = NULL;
    if (!o->first)
        return 0;
    if (qp_odict_hash_key(ctx, key, &k, &hash) < 0)
        return -1;
    r = qp_odict_find_node_hash(ctx, od, o, k, hash, pnode);
    JS_FreeValue(ctx, k);
    return r;
}

/* _odict_add_new_node: append a node for key unless it has one. */
static int qp_odict_add_new_node(JSContext *ctx, JSValueConst od, QPODict *o,
                                 JSValueConst key, Py_hash_t hash)
{
    QPODictNode *node;
    Py_ssize_t ix;

    ix = qp_odict_get_index(ctx, od, o, key, hash);
    if (ix < 0)
        return -1;
    if (o->fast_nodes[ix])
        return 0;
    node = js_malloc(ctx, sizeof(*node));
    if (!node)
        return -1;
    node->key = JS_DupValue(ctx, key);
    node->hash = hash;
    qp_odict_add_tail(o, node);
    o->fast_nodes[ix] = node;
    return 0;
}

/* _odict_clear_node: unlink and free the node of key, if any, before the
   key leaves the dict (see CPython's comment on colliding keys). */
static int qp_odict_clear_node(JSContext *ctx, JSValueConst od, QPODict *o,
                               JSValueConst key, Py_hash_t hash)
{
    QPODictNode *node;
    Py_ssize_t ix;

    if (!o->first)
        return 0;
    ix = qp_odict_get_index(ctx, od, o, key, hash);
    if (ix < 0)
        return -1;
    node = o->fast_nodes[ix];
    if (!node)
        return 0;
    o->fast_nodes[ix] = NULL;
    qp_odict_remove_node(o, node);
    qp_odict_node_free(ctx->rt, node);
    return 0;
}

/* _odict_keys_equal: 1 same keys in the same order, 0 not, -1 error. */
static int qp_odict_keys_equal(JSContext *ctx, QPODict *a, QPODict *b)
{
    const uint64_t state_a = a->state, state_b = b->state;
    QPODictNode *node_a = a->first, *node_b = b->first;

    for (;;) {
        JSValue key_a, key_b;
        int r;

        if (!node_a && !node_b)
            return 1;
        if (!node_a || !node_b)
            return 0;
        key_a = JS_DupValue(ctx, node_a->key);
        key_b = JS_DupValue(ctx, node_b->key);
        r = qp_richcompare_bool(ctx, key_a, key_b, QP_CMP_EQ);
        JS_FreeValue(ctx, key_a);
        JS_FreeValue(ctx, key_b);
        if (r < 0)
            return -1;
        if (a->state != state_a || b->state != state_b) {
            qp_raise_runtime_error(ctx, QP_ODICT_MUTATED);
            return -1;
        }
        if (r == 0)
            return 0;
        node_a = node_a->next;
        node_b = node_b->next;
    }
}

/* ========================================================================= */
/* Item API (PyODict_*)                                                      */
/* ========================================================================= */

static BOOL qp_odict_check(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_DICT &&
        qp_type_is_subtype(qp_type_of(ctx, v), QP_TYPE(ctx, OrderedDict));
}

static inline BOOL qp_odict_check_exact(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v) == QP_TYPE(ctx, OrderedDict);
}

/* PyODict_New: PyDict_Type.tp_new(&PyODict_Type) */
static JSValue qp_odict_new(JSContext *ctx)
{
    return qp_dict_alloc(ctx, QP_TYPE(ctx, OrderedDict), 0, 0);
}

/* _PyODict_SetItem_KnownHash_LockHeld (key already prepared) */
static int qp_odict_setitem_hash(JSContext *ctx, JSValueConst od, JSValueConst key,
                                 Py_hash_t hash, JSValueConst value)
{
    qp_dictcore *core = &QP_DICT(od)->core;
    QPODict *o = qp_odict_get(ctx, od);

    if (!o)
        return -1;
    if (qp_dictcore_setitem(ctx, core, key, hash, value, QP_DICT_EQ) < 0)
        return -1;
    if (qp_odict_add_new_node(ctx, od, o, key, hash) < 0) {
        /* revert setting the value on the dict */
        JSValue exc = qp_exc_fetch(ctx);
        if (qp_dictcore_delitem(ctx, core, key, hash, QP_DICT_EQ) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        JS_Throw(ctx, exc);
        return -1;
    }
    return 0;
}

static int qp_odict_set_item(JSContext *ctx, JSValueConst od, JSValueConst key, JSValueConst value)
{
    JSValue k;
    Py_hash_t hash;
    int r;

    if (qp_odict_hash_key(ctx, key, &k, &hash) < 0)
        return -1;
    r = qp_odict_setitem_hash(ctx, od, k, hash, value);
    JS_FreeValue(ctx, k);
    return r;
}

/* PyODict_DelItem: KeyError(key) when absent. */
static int qp_odict_del_item(JSContext *ctx, JSValueConst od, JSValueConst key)
{
    QPODict *o = qp_odict_get(ctx, od);
    JSValue k;
    Py_hash_t hash;
    int r;

    if (!o || qp_odict_hash_key(ctx, key, &k, &hash) < 0)
        return -1;
    r = qp_odict_clear_node(ctx, od, o, k, hash);
    if (r == 0) {
        r = qp_dictcore_delitem(ctx, &QP_DICT(od)->core, k, hash, QP_DICT_EQ);
        if (r == 0)
            qp_raise_key_error(ctx, k);
        r = r > 0 ? 0 : -1;
    }
    JS_FreeValue(ctx, k);
    return r;
}

/* _odict_popkey_hash: the value (owned), `failobj` (QP_NULL: KeyError)
   when absent, or JS_EXCEPTION. */
static JSValue qp_odict_popkey_hash(JSContext *ctx, JSValueConst od, JSValueConst key,
                                    JSValueConst failobj, Py_hash_t hash)
{
    QPODict *o = qp_odict_get(ctx, od);
    QPODictNode *node;
    JSValue value;
    int r;

    if (!o)
        return JS_EXCEPTION;
    r = qp_odict_find_node_hash(ctx, od, o, key, hash, &node);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        /* pop the node first to avoid a possible dict resize (due to eval
           loop reentrancy) and complications due to hash collision
           resolution; the node is looked up again (__eq__ code may have
           changed the list meanwhile) */
        if (qp_odict_clear_node(ctx, od, o, key, hash) < 0)
            return JS_EXCEPTION;
        r = qp_dictcore_pop(ctx, &QP_DICT(od)->core, key, hash, QP_DICT_EQ, &value);
        if (r < 0)
            return JS_EXCEPTION;
        if (r > 0)
            return value;
    }
    if (!QP_IS_NULL(failobj))
        return JS_DupValue(ctx, failobj);
    return qp_raise_key_error(ctx, key);
}

/* ========================================================================= */
/* MutableMapping.update (mutablemapping_*)                                  */
/* ========================================================================= */

/* mutablemapping_add_pairs: self[k] = v for every (k, v) of pairs. */
static int qp_odict_add_pairs(JSContext *ctx, JSValueConst self, JSValueConst pairs)
{
    JSValue it, pair, pit, key = QP_NULL, value = QP_NULL, extra;
    int r = 0;

    it = qp_iter(ctx, pairs);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        pair = qp_next(ctx, it);
        if (QP_IS_NULL(pair))
            break;
        if (JS_IsException(pair))
            goto fail;
        pit = qp_iter(ctx, pair);
        JS_FreeValue(ctx, pair);
        if (JS_IsException(pit))
            goto fail;
        key = value = QP_NULL;
        key = qp_next(ctx, pit);
        if (QP_IS_NULL(key)) {
            qp_raise_value_error(ctx, "need more than 0 values to unpack");
            goto bad_pair;
        }
        if (JS_IsException(key))
            goto bad_pair;
        value = qp_next(ctx, pit);
        if (QP_IS_NULL(value)) {
            qp_raise_value_error(ctx, "need more than 1 value to unpack");
            goto bad_pair;
        }
        if (JS_IsException(value))
            goto bad_pair;
        extra = qp_next(ctx, pit);
        if (!QP_IS_NULL(extra)) {
            if (!JS_IsException(extra)) {
                JS_FreeValue(ctx, extra);
                qp_raise_value_error(ctx, "too many values to unpack (expected 2)");
            }
            goto bad_pair;
        }
        JS_FreeValue(ctx, pit);
        r = qp_setitem(ctx, self, key, value);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        if (r < 0)
            goto fail;
    }
    JS_FreeValue(ctx, it);
    return 0;
bad_pair:
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, pit);
fail:
    JS_FreeValue(ctx, it);
    return -1;
}

/* mutablemapping_update_arg */
static int qp_odict_update_arg(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    JSValue func, res, it, key, value;
    int r;

    if (qp_anydict_check_exact(ctx, arg)) {
        JSValue items = qp_dict_items_list(ctx, arg);
        if (JS_IsException(items))
            return -1;
        r = qp_odict_add_pairs(ctx, self, items);
        JS_FreeValue(ctx, items);
        return r;
    }
    r = qp_getattr_opt(ctx, arg, QP_ATOM(ctx, keys), &func);
    if (r < 0)
        return -1;
    if (r > 0) {
        res = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(res))
            return -1;
        it = qp_iter(ctx, res);
        JS_FreeValue(ctx, res);
        if (JS_IsException(it))
            return -1;
        r = 0;
        for (;;) {
            key = qp_next(ctx, it);
            if (JS_IsException(key)) {
                r = -1;
                break;
            }
            if (QP_IS_NULL(key))
                break;
            value = qp_getitem(ctx, arg, key);
            if (JS_IsException(value)) {
                r = -1;
            } else {
                r = qp_setitem(ctx, self, key, value);
                JS_FreeValue(ctx, value);
            }
            JS_FreeValue(ctx, key);
            if (r < 0)
                break;
        }
        JS_FreeValue(ctx, it);
        return r;
    }
    r = qp_getattr_opt(ctx, arg, QP_ATOM(ctx, items), &func);
    if (r < 0)
        return -1;
    if (r > 0) {
        res = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(res))
            return -1;
        r = qp_odict_add_pairs(ctx, self, res);
        JS_FreeValue(ctx, res);
        return r;
    }
    return qp_odict_add_pairs(ctx, self, arg);
}

/* mutablemapping_update(self, args, kwargs) with vectorcall arguments. */
static int qp_odict_update_common(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames)
{
    Py_ssize_t i, nkw;

    if (nargs > 1) {
        qp_raise_type_error(ctx, "update() takes at most 1 positional argument (%d given)", nargs);
        return -1;
    }
    if (nargs == 1 && qp_odict_update_arg(ctx, self, args[0]) < 0)
        return -1;
    nkw = qp_dict_nkw(kwnames);
    for (i = 0; i < nkw; i++) {
        if (qp_setitem(ctx, self, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0)
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/* OrderedDict slots                                                         */
/* ========================================================================= */

/* odict_mp_ass_sub */
static int qp_odict_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst key,
                                  JSValueConst v)
{
    if (QP_IS_NULL(v))
        return qp_odict_del_item(ctx, self, key);
    return qp_odict_set_item(ctx, self, key, v);
}

static JSValue qp_odictiter_new(JSContext *ctx, JSValueConst od, int kind);

static JSValue qp_odict_tp_iter(JSContext *ctx, JSValueConst self)
{
    return qp_odictiter_new(ctx, self, QP_ODICT_ITER_KEYS);
}

/* odict_init */
static int qp_odict_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);

    if (nargs > 1) {
        qp_raise_type_error(ctx, "expected at most 1 argument, got %d", nargs);
        return -1;
    }
    /* __init__() triggering update() is just the way things are! */
    return qp_odict_update_common(ctx, self, args, nargs, kwnames);
}

/* odict_repr: "OrderedDict({...})" with the items in list order */
static JSValue qp_odict_repr(JSContext *ctx, JSValueConst self)
{
    JSValue name, dcopy, res;
    int r;

    name = JS_AtomToString(ctx, qp_type_of(ctx, self)->name);
    if (JS_IsException(name))
        return name;
    if (QP_DICT(self)->core.used == 0) {
        res = qp_str_from_format(ctx, "%U()", name);
        JS_FreeValue(ctx, name);
        return res;
    }
    r = qp_repr_enter(ctx, self);
    if (r != 0) {
        JS_FreeValue(ctx, name);
        return r > 0 ? JS_NewString(ctx, "...") : JS_EXCEPTION;
    }
    /* PyDict_Copy iterates keys(): the list order */
    dcopy = qp_anydict_copy_as(ctx, self, 0);
    if (JS_IsException(dcopy))
        res = dcopy;
    else
        res = qp_str_from_format(ctx, "%U(%R)", name, dcopy);
    JS_FreeValue(ctx, dcopy);
    JS_FreeValue(ctx, name);
    qp_repr_leave(ctx, self);
    return res;
}

/* odict_richcompare: dict equality, then the key order of two OrderedDicts */
static JSValue qp_odict_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPODict *a, *b;
    JSValue cmp;
    int eq;

    if (!qp_odict_check(ctx, v) || !qp_anydict_check(ctx, w) ||
        (op != QP_CMP_EQ && op != QP_CMP_NE))
        return qp_not_implemented(ctx);
    cmp = qp_dict_richcompare(ctx, v, w, op);
    if (JS_IsException(cmp) || !qp_odict_check(ctx, w))
        return cmp;
    if (JS_VALUE_GET_BOOL(cmp) == (op == QP_CMP_NE))
        return cmp;
    a = qp_odict_get(ctx, v);
    b = a ? qp_odict_get(ctx, w) : NULL;
    if (!b)
        return JS_EXCEPTION;
    eq = qp_odict_keys_equal(ctx, a, b);
    if (eq < 0)
        return JS_EXCEPTION;
    return qp_bool_new(eq == (op == QP_CMP_EQ));
}

/* odict_or: type(od)(left) updated with right */
static JSValue qp_odict_or(JSContext *ctx, JSValueConst left, JSValueConst right)
{
    JSValueConst other;
    QPType *t;
    JSValue res;

    if (qp_odict_check(ctx, left)) {
        t = qp_type_of(ctx, left);
        other = right;
    } else {
        t = qp_type_of(ctx, right);
        other = left;
    }
    if (!qp_anydict_check(ctx, other))
        return qp_not_implemented(ctx);
    res = qp_call1(ctx, QP_TYPE_VALUE(t), left);
    if (JS_IsException(res))
        return res;
    if (qp_odict_update_arg(ctx, res, right) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue qp_odict_ior(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    if (qp_odict_update_arg(ctx, self, other) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

/* ========================================================================= */
/* OrderedDict methods                                                       */
/* ========================================================================= */

static JSValue qp_odict_m_fromkeys(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "iterable", "value", NULL };
    static const QPArgSpec spec = { "fromkeys", kwlist, 1, 2, 0 };
    JSValueConst a[2];

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    return qp_dict_fromkeys_impl(ctx, self, a[0], QP_IS_NULL(a[1]) ? JS_UNDEFINED : a[1]);
}

static JSValue qp_odict_m_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    const QPODict *o = QP_DICT(self)->od;
    Py_ssize_t res = qp_dict_sizeof_impl(ctx, self) - QP_DICT_BASICSIZE + QP_ODICT_BASICSIZE;

    if (o) {
        res += (Py_ssize_t)sizeof(QPODictNode *) * o->fast_nodes_size;
        if (o->first)
            res += QP_ODICT_NODESIZE * QP_DICT(self)->core.used;
    }
    return qp_long_from_int64(ctx, res);
}

/* OrderedDict.__reduce__: (type(od), (), state, None, iter(od.items())) */
static JSValue qp_odict_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    JSValue items[5], tmp;

    items[2] = qp_object_getstate(ctx, self, 0);
    if (JS_IsException(items[2]))
        return items[2];
    items[1] = qp_tuple_new_steal(ctx, 0, NULL);
    if (JS_IsException(items[1])) {
        JS_FreeValue(ctx, items[2]);
        return JS_EXCEPTION;
    }
    tmp = qp_call_method(ctx, self, QP_ATOM(ctx, items), NULL, 0, QP_NULL);
    items[4] = JS_IsException(tmp) ? tmp : qp_iter(ctx, tmp);
    JS_FreeValue(ctx, tmp);
    if (JS_IsException(items[4])) {
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[2]);
        return JS_EXCEPTION;
    }
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[3] = JS_UNDEFINED;
    return qp_tuple_new_steal(ctx, 5, items);
}

/* OrderedDict.setdefault: skips __missing__() calls */
static JSValue qp_odict_m_setdefault(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "key", "default", NULL };
    static const QPArgSpec spec = { "setdefault", kwlist, 1, 2, 0 };
    JSValueConst a[2], dflt;
    JSValue res;
    int r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    dflt = QP_IS_NULL(a[1]) ? JS_UNDEFINED : a[1];
    if (qp_odict_check_exact(ctx, self)) {
        r = qp_dict_lookup(ctx, self, a[0], &res);
        if (r < 0)
            return JS_EXCEPTION;
        if (r > 0)
            return res;
        if (qp_odict_set_item(ctx, self, a[0], dflt) < 0)
            return JS_EXCEPTION;
        return JS_DupValue(ctx, dflt);
    }
    r = qp_contains(ctx, self, a[0]);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return qp_getitem(ctx, self, a[0]);
    if (qp_setitem(ctx, self, a[0], dflt) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, dflt);
}

/* OrderedDict.pop: skips __missing__() calls */
static JSValue qp_odict_m_pop(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "key", "default", NULL };
    static const QPArgSpec spec = { "pop", kwlist, 1, 2, 0 };
    JSValueConst a[2];
    JSValue k, res;
    Py_hash_t hash;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_odict_hash_key(ctx, a[0], &k, &hash) < 0)
        return JS_EXCEPTION;
    res = qp_odict_popkey_hash(ctx, self, k, a[1], hash);
    JS_FreeValue(ctx, k);
    return res;
}

static JSValue qp_odict_m_popitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "last", NULL };
    static const QPArgSpec spec = { "popitem", kwlist, 0, 1, 0 };
    JSValueConst a[1];
    JSValue kv[2];
    QPODict *o;
    QPODictNode *node;
    int last = 1;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_bool(ctx, a[0], &last) < 0)
        return JS_EXCEPTION;
    o = qp_odict_get(ctx, self);
    if (!o)
        return JS_EXCEPTION;
    if (!o->first)
        return qp_raise(ctx, QP_EXC(ctx, KeyError), "dictionary is empty");
    node = last ? o->last : o->first;
    kv[0] = JS_DupValue(ctx, node->key);
    kv[1] = qp_odict_popkey_hash(ctx, self, kv[0], QP_NULL, node->hash);
    if (JS_IsException(kv[1])) {
        JS_FreeValue(ctx, kv[0]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 2, kv);
}

/* keys() / values() / items() (magic: QP_DV_*) */
static JSValue qp_odict_m_view(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = magic == QP_DV_KEYS ? QP_TYPE(ctx, odict_keys) :
        magic == QP_DV_VALUES ? QP_TYPE(ctx, odict_values) : QP_TYPE(ctx, odict_items);
    return qp_dictview_alloc(ctx, t, self, magic);
}

static JSValue qp_odict_m_update(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    if (qp_odict_update_common(ctx, self, args, nargs, kwnames) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_odict_m_clear(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPDict *d = QP_DICT(self);

    qp_dictcore_clear(ctx, &d->core);
    if (d->od)
        qp_odict_clear_nodes(ctx, d->od);
    return JS_UNDEFINED;
}

/* OrderedDict.copy: type(od)() filled in list order */
static JSValue qp_odict_m_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    BOOL exact = qp_odict_check_exact(ctx, self);
    QPODict *o = qp_odict_get(ctx, self);
    QPODictNode *node;
    JSValue od_copy, key, value;
    uint64_t state;
    int r;

    if (!o)
        return JS_EXCEPTION;
    od_copy = exact ? qp_odict_new(ctx) : qp_call0(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    if (JS_IsException(od_copy))
        return od_copy;
    /* the loop body may run Python code which could mutate self and free
       its nodes (gh-148660); detect that the same way __eq__ does */
    state = o->state;
    for (node = o->first; node; node = node->next) {
        key = JS_DupValue(ctx, node->key);
        if (exact) {
            Py_hash_t hash = node->hash;
            r = qp_dict_lookup(ctx, self, key, &value);
            if (r == 0)
                qp_raise_key_error(ctx, key);
            if (r <= 0) {
                JS_FreeValue(ctx, key);
                goto fail;
            }
            r = qp_odict_setitem_hash(ctx, od_copy, key, hash, value);
        } else {
            value = qp_getitem(ctx, self, key);
            if (JS_IsException(value)) {
                JS_FreeValue(ctx, key);
                goto fail;
            }
            r = qp_setitem(ctx, od_copy, key, value);
        }
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, key);
        if (r < 0)
            goto fail;
        if (o->state != state) {
            qp_raise_runtime_error(ctx, QP_ODICT_MUTATED);
            goto fail;
        }
    }
    return od_copy;
fail:
    JS_FreeValue(ctx, od_copy);
    return JS_EXCEPTION;
}

static JSValue qp_odict_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_odictiter_new(ctx, self, QP_ODICT_ITER_KEYS | QP_ODICT_ITER_REVERSED);
}

static JSValue qp_odict_m_move_to_end(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "key", "last", NULL };
    static const QPArgSpec spec = { "move_to_end", kwlist, 1, 2, 0 };
    JSValueConst a[2];
    QPODict *o;
    QPODictNode *node;
    int last = 1, r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_bool(ctx, a[1], &last) < 0)
        return JS_EXCEPTION;
    o = qp_odict_get(ctx, self);
    if (!o)
        return JS_EXCEPTION;
    if (!o->first)
        return qp_raise_key_error(ctx, a[0]);
    node = last ? o->last : o->first;
    if (qp_is(a[0], node->key))
        return JS_UNDEFINED;
    r = qp_odict_find_node(ctx, self, o, a[0], &node);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_key_error(ctx, a[0]);
    if (last) {
        /* only move if not already the last one */
        if (node != o->last) {
            qp_odict_remove_node(o, node);
            qp_odict_add_tail(o, node);
        }
    } else {
        /* only move if not already the first one */
        if (node != o->first) {
            qp_odict_remove_node(o, node);
            qp_odict_add_head(o, node);
        }
    }
    return JS_UNDEFINED;
}

static const QPMethodDef qp_odict_methods[] = {
    /* overridden dict methods */
    { "fromkeys", qp_odict_m_fromkeys, QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 0,
      "fromkeys($type, /, iterable, value=None)\n--\n\n"
      "Create a new ordered dictionary with keys from iterable and values set to value." },
    { "__sizeof__", qp_odict_m_sizeof, QP_METH_NOARGS, 0, "__sizeof__($self, /)\n--\n\n" },
    { "__reduce__", qp_odict_m_reduce, QP_METH_NOARGS, 0,
      "__reduce__($self, /)\n--\n\nReturn state information for pickling" },
    { "setdefault", qp_odict_m_setdefault, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "setdefault($self, /, key, default=None)\n--\n\n"
      "Insert key with a value of default if key is not in the dictionary.\n\n"
      "Return the value for key if key is in the dictionary, else default." },
    { "pop", qp_odict_m_pop, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "pop($self, /, key, default=<unrepresentable>)\n--\n\n"
      "od.pop(key[,default]) -> v, remove specified key and return the corresponding value.\n\n"
      "If the key is not found, return the default if given; otherwise,\n"
      "raise a KeyError." },
    { "popitem", qp_odict_m_popitem, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "popitem($self, /, last=True)\n--\n\n"
      "Remove and return a (key, value) pair from the dictionary.\n\n"
      "Pairs are returned in LIFO order if last is true or FIFO order if\n"
      "false." },
    { "keys", qp_odict_m_view, QP_METH_NOARGS, QP_DV_KEYS, "" },
    { "values", qp_odict_m_view, QP_METH_NOARGS, QP_DV_VALUES, "" },
    { "items", qp_odict_m_view, QP_METH_NOARGS, QP_DV_ITEMS, "" },
    { "update", qp_odict_m_update, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0, "" },
    { "clear", qp_odict_m_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nRemove all items from ordered dict." },
    { "copy", qp_odict_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n--\n\nA shallow copy of ordered dict." },
    /* new methods */
    { "__reversed__", qp_odict_m_reversed, QP_METH_NOARGS, 0,
      "od.__reversed__() <==> reversed(od)" },
    { "move_to_end", qp_odict_m_move_to_end, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "move_to_end($self, /, key, last=True)\n--\n\n"
      "Move an existing element to the end (or beginning if last is false).\n\n"
      "Raise KeyError if the element does not exist." },
    { NULL },
};

static const QPGetSetDef qp_odict_getsets[] = {
    { "__dict__", qp_subtype_dict_get, qp_subtype_dict_set, 0, NULL },
    { NULL },
};

static const QPTypeSpec qp_odict_spec = {
    .name = "collections.OrderedDict",
    .doc = "Dictionary that remembers insertion order",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_DICT_SUBCLASS |
             QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_MAPPING | QP_TPFLAGS_IMMUTABLETYPE |
             QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF,
    .base = QP_T_dict,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DICT,
    .basicsize = QP_ODICT_BASICSIZE,
    .slots = {
        .tp_repr = qp_odict_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_richcompare = qp_odict_richcompare,
        .tp_iter = qp_odict_tp_iter,
        .tp_init = qp_odict_tp_init,
        .nb = {
            .nb_or = qp_odict_or,
            .nb_inplace_or = qp_odict_ior,
        },
        .mp = {
            .mp_ass_subscript = qp_odict_ass_subscript,
        },
    },
    .methods = qp_odict_methods,
    .getsets = qp_odict_getsets,
};

/* ========================================================================= */
/* odict_iterator                                                            */
/* ========================================================================= */

static JSValue qp_odictiter_new(JSContext *ctx, JSValueConst od, int kind)
{
    QPODict *o = qp_odict_get(ctx, od);
    QPODictNode *node;
    QPIterObj *it;
    JSValue obj;

    if (!o)
        return JS_EXCEPTION;
    obj = qp_iterobj_new(ctx, QP_TYPE(ctx, odict_iterator), &it);
    if (JS_IsException(obj))
        return obj;
    node = (kind & QP_ODICT_ITER_REVERSED) ? o->last : o->first;
    it->seq = JS_DupValue(ctx, od);
    it->aux = node ? JS_DupValue(ctx, node->key) : QP_NULL;
    it->index = kind;
    it->pos = QP_DICT(od)->core.used;
    it->start = (int64_t)o->state;
    return obj;
}

/* odictiter_nextkey: the key (owned) and *pod the OrderedDict (owned), or
   QP_NULL when exhausted, or JS_EXCEPTION.  The current key is tracked
   rather than the node, against changes during iteration. */
static JSValue qp_odictiter_nextkey(JSContext *ctx, QPIterObj *it, JSValue *pod)
{
    JSValue od, key, cur;
    QPODict *o;
    QPODictNode *node;
    int r;

    if (QP_IS_NULL(it->seq))
        return QP_NULL;
    if (QP_IS_NULL(it->aux)) {
        qp_iterobj_clear(ctx, it);  /* we're already done */
        return QP_NULL;
    }
    od = it->seq;
    o = qp_odict_get(ctx, od);
    if (!o)
        return JS_EXCEPTION;
    /* check for unsupported changes */
    if ((int64_t)o->state != it->start) {
        qp_raise_runtime_error(ctx, QP_ODICT_MUTATED);
        qp_iterobj_clear(ctx, it);
        return JS_EXCEPTION;
    }
    if (it->pos != QP_DICT(od)->core.used) {
        qp_raise_runtime_error(ctx, "OrderedDict changed size during iteration");
        it->pos = -1;   /* make this state sticky */
        return JS_EXCEPTION;
    }
    /* get the key (__eq__ code may re-enter the iterator) */
    od = JS_DupValue(ctx, od);
    cur = JS_DupValue(ctx, it->aux);
    r = qp_odict_find_node(ctx, od, o, cur, &node);
    if (r <= 0) {
        if (r == 0)
            qp_raise_key_error(ctx, cur);   /* must have been deleted */
        key = it->aux;
        it->aux = QP_NULL;
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, cur);
        JS_FreeValue(ctx, od);
        return JS_EXCEPTION;
    }
    /* advance to the next key */
    node = (it->index & QP_ODICT_ITER_REVERSED) ? node->prev : node->next;
    key = it->aux;
    it->aux = node ? JS_DupValue(ctx, node->key) : QP_NULL;
    JS_FreeValue(ctx, key);
    *pod = od;
    return cur;
}

/* odictiter_iternext */
static JSValue qp_odictiter_next_impl(JSContext *ctx, QPIterObj *it)
{
    JSValue key, value, od, kv[2];
    int r;

    key = qp_odictiter_nextkey(ctx, it, &od);
    if (QP_IS_NULL(key) || JS_IsException(key))
        return key;
    if (!(it->index & QP_ODICT_ITER_VALUES)) {
        JS_FreeValue(ctx, od);
        return key;
    }
    r = qp_dict_lookup(ctx, od, key, &value);
    JS_FreeValue(ctx, od);
    if (r <= 0) {
        if (r == 0)
            qp_raise_key_error(ctx, key);
        JS_FreeValue(ctx, key);
        od = it->aux;
        it->aux = QP_NULL;
        JS_FreeValue(ctx, od);
        qp_iterobj_clear(ctx, it);
        return JS_EXCEPTION;
    }
    if (!(it->index & QP_ODICT_ITER_KEYS)) {
        JS_FreeValue(ctx, key);
        return value;
    }
    kv[0] = key;
    kv[1] = value;
    return qp_tuple_new_steal(ctx, 2, kv);
}

static JSValue qp_odictiter_next(JSContext *ctx, JSValueConst self)
{
    return qp_odictiter_next_impl(ctx, JS_VALUE_GET_OBJ(self)->u.opaque);
}

/* odictiter_reduce: (iter, (list of the remaining items,)) without
   consuming the iterator */
static JSValue qp_odictiter_m_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPIterObj tmp = *(QPIterObj *)JS_VALUE_GET_OBJ(self)->u.opaque;
    JSValue list, item, res;

    tmp.seq = JS_DupValue(ctx, tmp.seq);
    tmp.aux = JS_DupValue(ctx, tmp.aux);
    tmp.aux2 = QP_NULL;
    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        goto done;
    for (;;) {
        item = qp_odictiter_next_impl(ctx, &tmp);
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item) || qp_list_append(ctx, list, item) < 0) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
            break;
        }
        JS_FreeValue(ctx, item);
    }
done:
    JS_FreeValue(ctx, tmp.seq);
    JS_FreeValue(ctx, tmp.aux);
    if (JS_IsException(list))
        return list;
    res = qp_iter_reduce_value(ctx, qp_eval_get_builtin(ctx, QP_ATOM(ctx, iter)), list, FALSE, 0);
    JS_FreeValue(ctx, list);
    return res;
}

static const QPMethodDef qp_odictiter_methods[] = {
    { "__reduce__", qp_odictiter_m_reduce, QP_METH_NOARGS, 0,
      "Return state information for pickling" },
    { NULL },
};

static const QPTypeSpec qp_odict_iterator_spec = {
    .name = "odict_iterator",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_ITER,
    .slots = {
        .tp_iter = qp_iter_self,
        .tp_iternext = qp_odictiter_next,
    },
    .methods = qp_odictiter_methods,
};

/* ========================================================================= */
/* Views                                                                     */
/* ========================================================================= */

static int qp_odictview_iter_kind(JSValueConst self)
{
    switch (QP_DICTVIEW(self)->kind) {
    case QP_DV_KEYS:
        return QP_ODICT_ITER_KEYS;
    case QP_DV_VALUES:
        return QP_ODICT_ITER_VALUES;
    default:
        return QP_ODICT_ITER_ITEMS;
    }
}

/* odictkeys_iter / odictvalues_iter / odictitems_iter */
static JSValue qp_odictview_iter(JSContext *ctx, JSValueConst self)
{
    return qp_odictiter_new(ctx, QP_DICTVIEW(self)->mapping, qp_odictview_iter_kind(self));
}

static JSValue qp_odictview_m_reversed(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_odictiter_new(ctx, QP_DICTVIEW(self)->mapping,
                            qp_odictview_iter_kind(self) | QP_ODICT_ITER_REVERSED);
}

static const QPMethodDef qp_odictview_methods[] = {
    { "__reversed__", qp_odictview_m_reversed, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

#define QP_ODICTVIEW_SPEC(cname, tpname, base_id)                             \
static const QPTypeSpec cname = {                                             \
    .name = tpname,                                                           \
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION |         \
             QP_TPFLAGS_IMMUTABLETYPE,                                        \
    .base = base_id,                                                          \
    .exc_base = -1,                                                           \
    .instance_class_id = JS_CLASS_PY_DICT_VIEW,                               \
    .slots = {                                                                \
        .tp_iter = qp_odictview_iter,                                         \
    },                                                                        \
    .methods = qp_odictview_methods,                                          \
};

QP_ODICTVIEW_SPEC(qp_odict_keys_spec, "odict_keys", QP_T_dict_keys)
QP_ODICTVIEW_SPEC(qp_odict_values_spec, "odict_values", QP_T_dict_values)
QP_ODICTVIEW_SPEC(qp_odict_items_spec, "odict_items", QP_T_dict_items)

/* ========================================================================= */
/* Init                                                                      */
/* ========================================================================= */

/* Called by qp_init_dicts (dictobject.c) once dict and its views are ready. */
static int qp_init_odicts(JSContext *ctx)
{
    static const struct {
        int id;
        const QPTypeSpec *spec;
    } types[] = {
        { QP_T_OrderedDict, &qp_odict_spec },
        { QP_T_odict_keys, &qp_odict_keys_spec },
        { QP_T_odict_values, &qp_odict_values_spec },
        { QP_T_odict_items, &qp_odict_items_spec },
        { QP_T_odict_iterator, &qp_odict_iterator_spec },
    };
    size_t i;

    for (i = 0; i < countof(types); i++) {
        if (qp_type_ready(ctx, QP_STATE(ctx)->types[types[i].id], types[i].spec) < 0)
            return -1;
    }
    return 0;
}
