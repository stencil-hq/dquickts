/*
 * dictcore.c - CPython's dict and set hash-table algorithms over JSValues.
 *
 * Derived from CPython Objects/dictobject.c and Objects/setobject.c
 * (PSF-2.0).  See dictcore.h for the contract (ownership, callbacks, error
 * codes).  Names in comments refer to the CPython 3.15.2 functions each
 * routine ports; the free-threading (Py_GIL_DISABLED) variants, split tables
 * and dict watchers are omitted because they never affect results or order.
 *
 * All macros defined here are #undef'd at the end of the file.
 */

/* ========================================================================= */
/* Shared helpers                                                            */
/* ========================================================================= */

#define QP_DC_IS_NULL(v) (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED)
#define QP_DC_IS_STR(v) (JS_VALUE_GET_TAG(v) == JS_TAG_STRING)

/* _PyDict_NotifyEvent: dict watchers (C API), one test of the tag bits
 * when the dict is not watched. */
#ifdef CONFIG_PYTHON_CAPI
#define QP_DC_WATCHED(d) unlikely((d)->version & QP_DC_WATCHER_MASK)
#define QP_DC_NOTIFY(ctx, d, ev, k, v) do {                                 \
        if (QP_DC_WATCHED(d))                                               \
            qp_capi_dict_notify(ctx, d, ev, k, v);                          \
    } while (0)
#else
#define QP_DC_WATCHED(d) 0
#define QP_DC_NOTIFY(ctx, d, ev, k, v) ((void)0)
#endif

/* Python identity (`a is b`) on JSValues, used where CPython compares
 * PyObject pointers: same normalized tag and same payload. */
static inline BOOL qp_dc_same(JSValueConst a, JSValueConst b)
{
    int ta = JS_VALUE_GET_NORM_TAG(a);
    if (ta != JS_VALUE_GET_NORM_TAG(b))
        return FALSE;
    switch (ta) {
    case JS_TAG_FLOAT64: {
        double da = JS_VALUE_GET_FLOAT64(a), db = JS_VALUE_GET_FLOAT64(b);
        uint64_t ua, ub;
        memcpy(&ua, &da, sizeof(ua));
        memcpy(&ub, &db, sizeof(ub));
        return ua == ub;
    }
    case JS_TAG_SHORT_BIG_INT:
        return JS_VALUE_GET_SHORT_BIG_INT(a) == JS_VALUE_GET_SHORT_BIG_INT(b);
    default:
        if (JS_VALUE_HAS_REF_COUNT(a))
            return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
        return JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    }
}

/* unicode_eq for two exact strs (both JS_TAG_STRING): same length and same
 * code units.  Never runs user code. */
static inline BOOL qp_dc_str_eq(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return js_string_eq(ctx, JS_VALUE_GET_STRING(a), JS_VALUE_GET_STRING(b));
}

/* _Py_bit_length for size_t. */
static inline uint8_t qp_dc_bit_length(size_t x)
{
    uint8_t n = 0;
    while (x != 0) {
        n++;
        x >>= 1;
    }
    return n;
}

/* ========================================================================= */
/* dict                                                                      */
/* ========================================================================= */

#define QP_DC_LOG_MINSIZE 3                 /* PyDict_LOG_MINSIZE */
#define QP_DC_MINSIZE 8                     /* PyDict_MINSIZE */
#define QP_DC_PERTURB_SHIFT 5
#define QP_DC_USABLE_FRACTION(n) (((n) << 1)/3)
#define QP_DC_GROWTH_RATE(d) ((d)->used*3)
#define QP_DC_SIZE(dk) ((size_t)1 << (dk)->log2_size)
#define QP_DC_MASK(dk) (QP_DC_SIZE(dk) - 1)
#define QP_DKIX_KEY_CHANGED (-4)            /* internal: restart the lookup */
/* Byte offset of the index table inside a keys block (8-byte aligned so the
 * entries that follow the power-of-two-sized index table are aligned too). */
#define QP_DC_HDR_SIZE ((sizeof(qp_dictkeys) + 7) & ~(size_t)7)

/* Py_EMPTY_KEYS: immutable, shared by every empty dict.  log2_size 0 (one
 * EMPTY slot), usable 0 so the first insertion always allocates, unicode kind
 * (as CPython's DICT_KEYS_UNICODE empty_keys_struct).  Never written. */
static int8_t qp_dc_empty_indices[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static qp_dictkeys qp_dc_empty_keys = {
    .log2_size = 0,
    .log2_index_bytes = 3,
    .unicode = 1,
    .usable = 0,
    .nentries = 0,
    .indices = qp_dc_empty_indices,
    .entries = NULL,
};
#define QP_DC_EMPTY_KEYS (&qp_dc_empty_keys)

/* dictkeys_get_index: DKIX_EMPTY, DKIX_DUMMY or an entry index. */
static inline Py_ssize_t qp_dc_get_index(const qp_dictkeys *keys, size_t i)
{
    int log2size = keys->log2_size;
    if (log2size < 8)
        return ((const int8_t *)keys->indices)[i];
    else if (log2size < 16)
        return ((const int16_t *)keys->indices)[i];
#if INTPTR_MAX > INT32_MAX
    else if (log2size >= 32)
        return (Py_ssize_t)((const int64_t *)keys->indices)[i];
#endif
    else
        return ((const int32_t *)keys->indices)[i];
}

/* dictkeys_set_index. */
static inline void qp_dc_set_index(qp_dictkeys *keys, size_t i, Py_ssize_t ix)
{
    int log2size = keys->log2_size;
    if (log2size < 8)
        ((int8_t *)keys->indices)[i] = (int8_t)ix;
    else if (log2size < 16)
        ((int16_t *)keys->indices)[i] = (int16_t)ix;
#if INTPTR_MAX > INT32_MAX
    else if (log2size >= 32)
        ((int64_t *)keys->indices)[i] = (int64_t)ix;
#endif
    else
        ((int32_t *)keys->indices)[i] = (int32_t)ix;
}

/* calculate_log2_keysize: smallest log2 size >= minsize (at least 8). */
static inline uint8_t qp_dc_calculate_log2_keysize(Py_ssize_t minsize)
{
    if (minsize < QP_DC_MINSIZE)
        minsize = QP_DC_MINSIZE;
    return qp_dc_bit_length((size_t)minsize - 1);
}

/* estimate_log2_keysize: inverse of USABLE_FRACTION, the table size that
 * holds n entries without resizing. */
static inline uint8_t qp_dc_estimate_log2_keysize(Py_ssize_t n)
{
    return qp_dc_calculate_log2_keysize((n*3 + 1) / 2);
}

/* log2 of the index table byte size for a table of 1 << log2_size slots
 * (1-byte indices below 256 slots, 2 below 2**16, 8 from 2**32, else 4). */
static inline int qp_dc_log2_index_bytes(uint8_t log2_size)
{
    if (log2_size < 8)
        return log2_size;
    else if (log2_size < 16)
        return log2_size + 1;
#if INTPTR_MAX > INT32_MAX
    else if (log2_size >= 32)
        return log2_size + 3;
#endif
    else
        return log2_size + 2;
}

/* new_keys_object: allocate an empty keys table of 1 << log2_size slots, all
 * indices EMPTY, USABLE_FRACTION(size) usable entries.  NULL + OOM thrown on
 * failure (also when the size cannot be represented). */
static qp_dictkeys *qp_dc_new_keys(JSContext *ctx, uint8_t log2_size,
                                   int unicode)
{
    int log2_bytes = qp_dc_log2_index_bytes(log2_size);
    size_t index_bytes, usable;
    qp_dictkeys *dk;

    if (log2_bytes >= (int)(sizeof(size_t) * 8 - 1))
        goto oom;
    index_bytes = (size_t)1 << log2_bytes;
    usable = QP_DC_USABLE_FRACTION((size_t)1 << log2_size);
    if (usable > (SIZE_MAX - QP_DC_HDR_SIZE - index_bytes) / sizeof(qp_dictentry))
        goto oom;
    dk = js_malloc(ctx, QP_DC_HDR_SIZE + index_bytes
                   + usable * sizeof(qp_dictentry));
    if (!dk)
        return NULL;
    dk->log2_size = log2_size;
    dk->log2_index_bytes = (uint8_t)log2_bytes;
    dk->unicode = unicode != 0;
    dk->usable = (Py_ssize_t)usable;
    dk->nentries = 0;
    dk->indices = (char *)dk + QP_DC_HDR_SIZE;
    dk->entries = (qp_dictentry *)((char *)dk->indices + index_bytes);
    /* All indices DKIX_EMPTY.  Entries past nentries are never read, so they
     * are left uninitialized. */
    memset(dk->indices, 0xff, index_bytes);
    return dk;
 oom:
    JS_ThrowOutOfMemory(ctx);
    return NULL;
}

/* Free a keys block (not its references).  The empty table is static. */
static void qp_dc_free_keys_block(JSRuntime *rt, qp_dictkeys *dk)
{
    if (dk != QP_DC_EMPTY_KEYS)
        js_free_rt(rt, dk);
}

/* dictkeys_decref reaching zero: release the references of every entry (key
 * then value, in entry order) and free the block.  Callers detach the table
 * from its dict first, because releasing can run finalizers. */
static void qp_dc_keys_release(JSRuntime *rt, qp_dictkeys *dk)
{
    Py_ssize_t i, n;
    if (dk == QP_DC_EMPTY_KEYS)
        return;
    for (i = 0, n = dk->nentries; i < n; i++) {
        qp_dictentry *ep = &dk->entries[i];
        JS_FreeValueRT(rt, ep->key);
        JS_FreeValueRT(rt, ep->value);
    }
    qp_dc_free_keys_block(rt, dk);
}

/* Install a new keys table in `d` (set_keys); bumps the generation used by
 * lookups to detect a replaced table. */
static inline void qp_dc_set_keys(qp_dictcore *d, qp_dictkeys *keys)
{
    d->keys = keys;
    d->keys_gen++;
    d->version += QP_DC_VERSION_INC;
}

static void qp_dictcore_init(qp_dictcore *d)
{
    d->keys = QP_DC_EMPTY_KEYS;
    d->used = 0;
    d->version = 0;
    d->keys_gen = 0;
}

/* dict_new_presized. */
static int qp_dictcore_init_presized(JSContext *ctx, qp_dictcore *d,
                                     Py_ssize_t minused, int unicode)
{
    const uint8_t log2_max_presize = 17;
    const Py_ssize_t max_presize = ((Py_ssize_t)1) << log2_max_presize;
    uint8_t log2_newsize;
    qp_dictkeys *new_keys;

    qp_dictcore_init(d);
    if (minused <= QP_DC_USABLE_FRACTION(QP_DC_MINSIZE))
        return 0;
    /* There are no strict guarantee that returned dict can contain minused
     * items without resize.  So we create medium size dict instead of very
     * large dict or MemoryError. */
    if (minused > QP_DC_USABLE_FRACTION(max_presize))
        log2_newsize = log2_max_presize;
    else
        log2_newsize = qp_dc_estimate_log2_keysize(minused);
    new_keys = qp_dc_new_keys(ctx, log2_newsize, unicode);
    if (!new_keys)
        return -1;
    d->keys = new_keys;
    d->keys_gen++;
    return 0;
}

static void qp_dictcore_free(JSRuntime *rt, qp_dictcore *d)
{
    qp_dictkeys *keys = d->keys;
    d->keys = QP_DC_EMPTY_KEYS;
    d->used = 0;
    d->keys_gen++;
    d->version += QP_DC_VERSION_INC;
    qp_dc_keys_release(rt, keys);
}

static void qp_dictcore_mark(JSRuntime *rt, const qp_dictcore *d,
                             JS_MarkFunc *mark_func)
{
    const qp_dictkeys *keys = d->keys;
    Py_ssize_t i, n = keys->nentries;
    for (i = 0; i < n; i++) {
        const qp_dictentry *ep = &keys->entries[i];
        if (QP_DICTENTRY_ACTIVE(ep)) {
            JS_MarkValue(rt, ep->value, mark_func);
            JS_MarkValue(rt, ep->key, mark_func);
        }
    }
}

/* lookdict_index: the index-table slot that holds entry `index`, found by
 * replaying the probe sequence of `hash`.  DKIX_EMPTY if absent. */
static Py_ssize_t qp_dc_lookdict_index(const qp_dictkeys *k, Py_hash_t hash,
                                       Py_ssize_t index)
{
    size_t mask = QP_DC_MASK(k);
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;

    for (;;) {
        Py_ssize_t ix = qp_dc_get_index(k, i);
        if (ix == index)
            return (Py_ssize_t)i;
        if (ix == QP_DKIX_EMPTY)
            return QP_DKIX_EMPTY;
        perturb >>= QP_DC_PERTURB_SHIFT;
        i = mask & (i*5 + perturb + 1);
    }
}

/* compare_unicode_unicode: exact-str key against an all-str table; never
 * calls back. */
static inline int qp_dc_compare_unicode(JSContext *ctx, const qp_dictkeys *dk,
                                        Py_ssize_t ix, JSValueConst key,
                                        Py_hash_t hash)
{
    const qp_dictentry *ep = &dk->entries[ix];
    if (qp_dc_same(ep->key, key) ||
        (ep->hash == hash && qp_dc_str_eq(ctx, ep->key, key)))
        return 1;
    return 0;
}

/* compare_generic / compare_unicode_generic: identity, then (equal hashes)
 * str content or the eq callback.  After a callback the result only counts if
 * the dict still has the same keys table (generation `gen`) and the entry
 * still holds the compared key; otherwise QP_DKIX_KEY_CHANGED (restart).
 * The temporary reference to the stored key is dropped before that check, as
 * CPython does (Py_DECREF(startkey) precedes it). */
static int qp_dc_compare_generic(JSContext *ctx, qp_dictcore *d,
                                 qp_dictkeys *dk, uint64_t gen, Py_ssize_t ix,
                                 JSValueConst key, Py_hash_t hash,
                                 qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_dictentry *ep = &dk->entries[ix];
    if (qp_dc_same(ep->key, key))
        return 1;
    if (ep->hash == hash) {
        JSValue startkey = ep->key;
        int cmp;
        /* PyObject_RichCompareBool on two exact strs: no user code. */
        if (QP_DC_IS_STR(startkey) && QP_DC_IS_STR(key))
            return qp_dc_str_eq(ctx, startkey, key);
        JS_DupValue(ctx, startkey);
        cmp = eq(ctx, startkey, key, opaque);
        JS_FreeValue(ctx, startkey);
        if (cmp < 0)
            return QP_DKIX_ERROR;
        if (d->keys_gen == gen && qp_dc_same(dk->entries[ix].key, startkey))
            return cmp > 0;
        /* The dict was mutated, restart */
        return QP_DKIX_KEY_CHANGED;
    }
    return 0;
}

/* do_lookup: probe `dk` (which is d->keys) for key.  Returns the entry index,
 * DKIX_EMPTY, DKIX_ERROR or DKIX_KEY_CHANGED. */
static Py_ssize_t qp_dc_do_lookup(JSContext *ctx, qp_dictcore *d,
                                  qp_dictkeys *dk, JSValueConst key,
                                  Py_hash_t hash, int unicode_unicode,
                                  qp_dictcore_eq_fn *eq, void *opaque)
{
    size_t mask = QP_DC_MASK(dk);
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;
    uint64_t gen = d->keys_gen;
    Py_ssize_t ix;

    for (;;) {
        ix = qp_dc_get_index(dk, i);
        if (ix >= 0) {
            int cmp;
            if (unicode_unicode)
                cmp = qp_dc_compare_unicode(ctx, dk, ix, key, hash);
            else
                cmp = qp_dc_compare_generic(ctx, d, dk, gen, ix, key, hash,
                                            eq, opaque);
            if (cmp < 0)
                return cmp;
            else if (cmp)
                return ix;
        } else if (ix == QP_DKIX_EMPTY) {
            return QP_DKIX_EMPTY;
        }
        perturb >>= QP_DC_PERTURB_SHIFT;
        i = mask & (i*5 + perturb + 1);
    }
}

/* _Py_dict_lookup. */
static Py_ssize_t qp_dictcore_lookup(JSContext *ctx, qp_dictcore *d,
                                     JSValueConst key, Py_hash_t hash,
                                     qp_dictcore_eq_fn *eq, void *opaque,
                                     JSValue *pvalue)
{
    qp_dictkeys *dk;
    Py_ssize_t ix;

 start:
    dk = d->keys;
    if (dk->unicode && QP_DC_IS_STR(key)) {
        ix = qp_dc_do_lookup(ctx, d, dk, key, hash, 1, eq, opaque);
    } else {
        ix = qp_dc_do_lookup(ctx, d, dk, key, hash, 0, eq, opaque);
        if (ix == QP_DKIX_KEY_CHANGED)
            goto start;
    }
    if (pvalue) {
        if (ix >= 0)
            *pvalue = dk->entries[ix].value;
        else
            *pvalue = JS_UNINITIALIZED;
    }
    return ix;
}

static int qp_dictcore_contains(JSContext *ctx, qp_dictcore *d,
                                JSValueConst key, Py_hash_t hash,
                                qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t ix = qp_dictcore_lookup(ctx, d, key, hash, eq, opaque, NULL);
    if (ix == QP_DKIX_ERROR)
        return -1;
    return ix >= 0;
}

/* find_empty_slot: first EMPTY or DUMMY slot on key's probe sequence (the key
 * is known to be absent). */
static size_t qp_dc_find_empty_slot(const qp_dictkeys *keys, Py_hash_t hash)
{
    const size_t mask = QP_DC_MASK(keys);
    size_t i = (size_t)hash & mask;
    Py_ssize_t ix = qp_dc_get_index(keys, i);
    size_t perturb;
    for (perturb = (size_t)hash; ix >= 0;) {
        perturb >>= QP_DC_PERTURB_SHIFT;
        i = (i*5 + perturb + 1) & mask;
        ix = qp_dc_get_index(keys, i);
    }
    return i;
}

/* build_indices_generic / build_indices_unicode: index the first n entries
 * (all live) of a fresh table. */
static void qp_dc_build_indices(qp_dictkeys *keys, const qp_dictentry *ep,
                                Py_ssize_t n)
{
    size_t mask = QP_DC_MASK(keys);
    Py_ssize_t ix;
    for (ix = 0; ix != n; ix++, ep++) {
        Py_hash_t hash = ep->hash;
        size_t i = (size_t)hash & mask;
        size_t perturb;
        for (perturb = (size_t)hash; qp_dc_get_index(keys, i) != QP_DKIX_EMPTY;) {
            perturb >>= QP_DC_PERTURB_SHIFT;
            i = mask & (i*5 + perturb + 1);
        }
        qp_dc_set_index(keys, i, ix);
    }
}

/* dictresize: move the live entries (compacted, order kept) into a new table
 * of 1 << log2_newsize slots.  The table stays/becomes unicode only if it was
 * unicode and `unicode` is set.  0 or -1 (OOM thrown). */
static int qp_dc_resize(JSContext *ctx, qp_dictcore *d, uint8_t log2_newsize,
                        int unicode)
{
    qp_dictkeys *oldkeys, *newkeys;
    Py_ssize_t numentries;

    if (log2_newsize >= sizeof(size_t) * 8) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    oldkeys = d->keys;
    if (!oldkeys->unicode)
        unicode = 0;

    /* Allocate a new table (never reusing the old one). */
    newkeys = qp_dc_new_keys(ctx, log2_newsize, unicode);
    if (!newkeys)
        return -1;
    numentries = d->used;
    if (numentries > 0) {
        qp_dictentry *oldentries = oldkeys->entries;
        qp_dictentry *newentries = newkeys->entries;
        if (oldkeys->nentries == numentries) {
            memcpy(newentries, oldentries, numentries * sizeof(qp_dictentry));
        } else {
            qp_dictentry *ep = oldentries;
            Py_ssize_t i;
            for (i = 0; i < numentries; i++) {
                while (QP_DC_IS_NULL(ep->value))
                    ep++;
                newentries[i] = *ep++;
            }
        }
        qp_dc_build_indices(newkeys, newentries, numentries);
    }
    qp_dc_set_keys(d, newkeys);
    /* References moved to the new table: free only the old block. */
    qp_dc_free_keys_block(ctx->rt, oldkeys);
    newkeys->usable -= numentries;
    newkeys->nentries = numentries;
    return 0;
}

/* insertion_resize. */
static int qp_dc_insertion_resize(JSContext *ctx, qp_dictcore *d, int unicode)
{
    return qp_dc_resize(ctx, d, qp_dc_calculate_log2_keysize(QP_DC_GROWTH_RATE(d)),
                        unicode);
}

/* insert_combined_dict: append a new entry for a key known to be absent.
 * Takes ownership of key and value only on success (0); -1 on OOM. */
static int qp_dc_insert_combined(JSContext *ctx, qp_dictcore *d,
                                 Py_hash_t hash, JSValue key, JSValue value)
{
    qp_dictkeys *keys;
    qp_dictentry *ep;
    size_t hashpos;

    /* gh-140551: If dict was cleared in the lookup, we have to resize one
     * more time to force general key kind. */
    if (d->keys->unicode && !QP_DC_IS_STR(key)) {
        if (qp_dc_insertion_resize(ctx, d, 0) < 0)
            return -1;
    }
    if (d->keys->usable <= 0) {
        /* Need to resize. */
        if (qp_dc_insertion_resize(ctx, d, 1) < 0)
            return -1;
    }
    if (!QP_DC_IS_STR(key))
        d->version &= ~(uint64_t)QP_DC_TAG_SPLIT;   /* now a combined table */
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_ADDED, key, value);
    keys = d->keys;
    hashpos = qp_dc_find_empty_slot(keys, hash);
    qp_dc_set_index(keys, hashpos, keys->nentries);
    ep = &keys->entries[keys->nentries];
    ep->key = key;
    ep->value = value;
    ep->hash = hash;
    keys->usable--;
    keys->nentries++;
    return 0;
}

/* replace_value: store `value` (owned) at entry ix, release the old value. */
static void qp_dc_replace_value(JSContext *ctx, qp_dictcore *d, Py_ssize_t ix,
                                JSValue old_value, JSValue value)
{
    qp_dictentry *ep = &d->keys->entries[ix];
    if (!qp_dc_same(old_value, value))
        d->version += QP_DC_VERSION_INC;
    ep->value = value;
    JS_FreeValue(ctx, old_value);   /* which **CAN** re-enter (see gh-66843) */
}

/* insertdict: consumes key and value. */
static int qp_dc_insertdict(JSContext *ctx, qp_dictcore *d, JSValue key,
                            Py_hash_t hash, JSValue value,
                            qp_dictcore_eq_fn *eq, void *opaque)
{
    JSValue old_value;
    Py_ssize_t ix;

    ix = qp_dictcore_lookup(ctx, d, key, hash, eq, opaque, &old_value);
    if (ix == QP_DKIX_ERROR)
        goto fail;
    if (ix < 0) {
        /* qp_dc_insert_combined() converts a unicode table into a general
         * one if key is not an exact str. */
        if (qp_dc_insert_combined(ctx, d, hash, key, value) < 0)
            goto fail;
        d->used++;
        d->version += QP_DC_VERSION_INC;
        return 0;
    }
    /* a split table (instance __dict__) reports every store */
    if (QP_DC_WATCHED(d) &&
        (!qp_dc_same(old_value, value) || (d->version & QP_DC_TAG_SPLIT)))
        QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_MODIFIED, key, value);
    qp_dc_replace_value(ctx, d, ix, old_value, value);
    JS_FreeValue(ctx, key);
    return 0;
 fail:
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, key);
    return -1;
}

/* insert_to_emptydict: first insertion into a dict using the empty keys.
 * Consumes key and value. */
static int qp_dc_insert_to_emptydict(JSContext *ctx, qp_dictcore *d,
                                     JSValue key, Py_hash_t hash,
                                     JSValue value)
{
    int unicode = QP_DC_IS_STR(key);
    qp_dictkeys *newkeys = qp_dc_new_keys(ctx, QP_DC_LOG_MINSIZE, unicode);
    size_t hashpos;
    qp_dictentry *ep;

    if (!newkeys) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        return -1;
    }
    if (!unicode)
        d->version &= ~(uint64_t)QP_DC_TAG_SPLIT;   /* now a combined table */
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_ADDED, key, value);
    hashpos = (size_t)hash & (QP_DC_MINSIZE - 1);
    qp_dc_set_index(newkeys, hashpos, 0);
    ep = &newkeys->entries[0];
    ep->key = key;
    ep->value = value;
    ep->hash = hash;
    d->used++;
    newkeys->usable--;
    newkeys->nentries++;
    qp_dc_set_keys(d, newkeys);
    return 0;
}

/* setitem_take2_lock_held_known_hash. */
static int qp_dictcore_setitem_take(JSContext *ctx, qp_dictcore *d,
                                    JSValue key, Py_hash_t hash, JSValue value,
                                    qp_dictcore_eq_fn *eq, void *opaque)
{
    if (d->keys == QP_DC_EMPTY_KEYS)
        return qp_dc_insert_to_emptydict(ctx, d, key, hash, value);
    /* insertdict() handles any resizing that might be necessary */
    return qp_dc_insertdict(ctx, d, key, hash, value, eq, opaque);
}

static int qp_dictcore_setitem(JSContext *ctx, qp_dictcore *d,
                               JSValueConst key, Py_hash_t hash,
                               JSValueConst value,
                               qp_dictcore_eq_fn *eq, void *opaque)
{
    return qp_dictcore_setitem_take(ctx, d, JS_DupValue(ctx, key), hash,
                                    JS_DupValue(ctx, value), eq, opaque);
}

/* dict_setdefault_ref_lock_held (combined tables). */
static int qp_dictcore_setdefault(JSContext *ctx, qp_dictcore *d,
                                  JSValueConst key, Py_hash_t hash,
                                  JSValueConst default_value,
                                  qp_dictcore_eq_fn *eq, void *opaque,
                                  JSValue *presult)
{
    JSValue value;
    Py_ssize_t ix;

    if (d->keys == QP_DC_EMPTY_KEYS) {
        if (qp_dc_insert_to_emptydict(ctx, d, JS_DupValue(ctx, key), hash,
                                      JS_DupValue(ctx, default_value)) < 0) {
            if (presult)
                *presult = JS_UNINITIALIZED;
            return -1;
        }
        if (presult)
            *presult = d->keys->entries[0].value;
        return 0;
    }

    ix = qp_dictcore_lookup(ctx, d, key, hash, eq, opaque, &value);
    if (ix == QP_DKIX_ERROR) {
        if (presult)
            *presult = JS_UNINITIALIZED;
        return -1;
    }
    if (ix == QP_DKIX_EMPTY) {
        JSValue k = JS_DupValue(ctx, key);
        JSValue v = JS_DupValue(ctx, default_value);
        if (qp_dc_insert_combined(ctx, d, hash, k, v) < 0) {
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            if (presult)
                *presult = JS_UNINITIALIZED;
            return -1;
        }
        d->used++;
        d->version += QP_DC_VERSION_INC;
        if (presult)
            *presult = d->keys->entries[d->keys->nentries - 1].value;
        return 0;
    }
    if (presult)
        *presult = value;
    return 1;
}

/* delitem_common.  The index slot is found from the entry's stored hash (the
 * probe sequence it was inserted with). */
static void qp_dictcore_delitem_ix(JSContext *ctx, qp_dictcore *d,
                                   Py_ssize_t ix)
{
    qp_dictkeys *keys = d->keys;
    qp_dictentry *ep = &keys->entries[ix];
    JSValue old_key, old_value;
    Py_ssize_t hashpos = qp_dc_lookdict_index(keys, ep->hash, ix);

    d->used--;
    d->version += QP_DC_VERSION_INC;
    qp_dc_set_index(keys, (size_t)hashpos, QP_DKIX_DUMMY);
    old_key = ep->key;
    old_value = ep->value;
    ep->key = JS_UNINITIALIZED;
    ep->value = JS_UNINITIALIZED;
    ep->hash = 0;
    JS_FreeValue(ctx, old_key);
    JS_FreeValue(ctx, old_value);
}

/* _PyDict_DelItem_KnownHash_LockHeld minus the KeyError. */
static int qp_dictcore_delitem(JSContext *ctx, qp_dictcore *d,
                               JSValueConst key, Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t ix = qp_dictcore_lookup(ctx, d, key, hash, eq, opaque, NULL);
    if (ix == QP_DKIX_ERROR)
        return -1;
    if (ix == QP_DKIX_EMPTY)
        return 0;
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_DELETED, key, JS_UNINITIALIZED);
    qp_dictcore_delitem_ix(ctx, d, ix);
    return 1;
}

/* _PyDict_Pop_KnownHash. */
static int qp_dictcore_pop(JSContext *ctx, qp_dictcore *d, JSValueConst key,
                           Py_hash_t hash, qp_dictcore_eq_fn *eq, void *opaque,
                           JSValue *presult)
{
    JSValue old_value;
    Py_ssize_t ix;

    if (presult)
        *presult = JS_UNINITIALIZED;
    if (d->used == 0)
        return 0;
    ix = qp_dictcore_lookup(ctx, d, key, hash, eq, opaque, &old_value);
    if (ix == QP_DKIX_ERROR)
        return -1;
    if (ix == QP_DKIX_EMPTY)
        return 0;
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_DELETED, key, JS_UNINITIALIZED);
    /* Keep a reference for the result: delitem releases the table's one. */
    JS_DupValue(ctx, old_value);
    qp_dictcore_delitem_ix(ctx, d, ix);
    if (presult)
        *presult = old_value;
    else
        JS_FreeValue(ctx, old_value);
    return 1;
}

/* dict_popitem_impl: pop the last live entry; nentries shrinks to it, the
 * index slot becomes DUMMY and usable is not given back. */
static int qp_dictcore_popitem(JSContext *ctx, qp_dictcore *d,
                               JSValue *pkey, JSValue *pvalue)
{
    qp_dictkeys *keys;
    qp_dictentry *ep0;
    Py_ssize_t i, j;
    Py_hash_t hash;

    if (d->used == 0)
        return 0;
    keys = d->keys;
    ep0 = keys->entries;
    i = keys->nentries - 1;
    while (i >= 0 && QP_DC_IS_NULL(ep0[i].value))
        i--;
    d->version &= ~(uint64_t)QP_DC_TAG_SPLIT;   /* CPython makes it combined first */
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_DELETED, ep0[i].key, JS_UNINITIALIZED);
    *pkey = ep0[i].key;
    *pvalue = ep0[i].value;
    hash = ep0[i].hash;
    ep0[i].key = JS_UNINITIALIZED;
    ep0[i].value = JS_UNINITIALIZED;
    ep0[i].hash = -1;

    j = qp_dc_lookdict_index(keys, hash, i);
    qp_dc_set_index(keys, (size_t)j, QP_DKIX_DUMMY);
    /* We can't dk_usable++ since there is DKIX_DUMMY in indices */
    keys->nentries = i;
    d->used--;
    d->version += QP_DC_VERSION_INC;
    return 1;
}

/* clear_lock_held: detach the table first (releasing can run finalizers that
 * touch the dict), then release it. */
static void qp_dictcore_clear(JSContext *ctx, qp_dictcore *d)
{
    qp_dictkeys *oldkeys = d->keys;
    if (oldkeys == QP_DC_EMPTY_KEYS)
        return;
    QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_CLEARED, JS_UNINITIALIZED, JS_UNINITIALIZED);
    d->version &= ~(uint64_t)QP_DC_TAG_SPLIT;   /* the empty keys are combined */
    oldkeys = d->keys;
    d->used = 0;
    qp_dc_set_keys(d, QP_DC_EMPTY_KEYS);
    qp_dc_keys_release(ctx->rt, oldkeys);
}

/* _PyDict_Next. */
static int qp_dictcore_next(const qp_dictcore *d, Py_ssize_t *ppos,
                            Py_ssize_t *pix)
{
    const qp_dictkeys *keys = d->keys;
    Py_ssize_t i = *ppos, n = keys->nentries;

    if (i < 0 || i >= n)
        return 0;
    while (i < n && QP_DC_IS_NULL(keys->entries[i].value))
        i++;
    if (i >= n)
        return 0;
    *ppos = i + 1;
    *pix = i;
    return 1;
}

/* dictiter_new. */
static void qp_dictcore_iter_init(const qp_dictcore *d, qp_dictcore_iter *it,
                                  int reversed)
{
    it->used = d->used;
    it->len = d->used;
    it->pos = reversed ? d->keys->nentries - 1 : 0;
}

/* dictiter_iternextkey_lock_held (and the value/item variants, which share
 * the same scan) / dictreviter_iter_lock_held. */
static int qp_dictcore_iter_next(const qp_dictcore *d, qp_dictcore_iter *it,
                                 int reversed, Py_ssize_t *pix)
{
    const qp_dictkeys *k = d->keys;
    Py_ssize_t i;

    if (it->used != d->used) {
        it->used = -1; /* Make this state sticky */
        return QP_DICTITER_CHANGED_SIZE;
    }
    i = it->pos;
    if (!reversed) {
        Py_ssize_t n = k->nentries;
        while (i < n && QP_DC_IS_NULL(k->entries[i].value))
            i++;
        if (i >= n)
            return 0;
    } else {
        if (i < 0 || i >= k->nentries)
            return 0;
        while (QP_DC_IS_NULL(k->entries[i].value)) {
            if (--i < 0)
                return 0;
        }
    }
    /* We found an element, but did not expect it */
    if (it->len == 0)
        return QP_DICTITER_KEYS_CHANGED;
    it->pos = reversed ? i - 1 : i + 1;
    it->len--;
    *pix = i;
    return 1;
}

/* dictiter_len. */
static Py_ssize_t qp_dictcore_iter_length_hint(const qp_dictcore *d,
                                               const qp_dictcore_iter *it)
{
    if (it->used == d->used)
        return it->len;
    return 0;
}

/* clone_combined_dict_keys: a copy of orig's table with new references to
 * every live key and value. */
static qp_dictkeys *qp_dc_clone_keys(JSContext *ctx, const qp_dictcore *orig)
{
    const qp_dictkeys *ok = orig->keys;
    size_t index_bytes = (size_t)1 << ok->log2_index_bytes;
    size_t usable = QP_DC_USABLE_FRACTION(QP_DC_SIZE(ok));
    qp_dictkeys *keys;
    Py_ssize_t i, n;

    keys = js_malloc(ctx, QP_DC_HDR_SIZE + index_bytes
                     + usable * sizeof(qp_dictentry));
    if (!keys)
        return NULL;
    *keys = *ok;
    keys->indices = (char *)keys + QP_DC_HDR_SIZE;
    keys->entries = (qp_dictentry *)((char *)keys->indices + index_bytes);
    memcpy(keys->indices, ok->indices, index_bytes);
    n = ok->nentries;
    if (n > 0)
        memcpy(keys->entries, ok->entries, n * sizeof(qp_dictentry));
    for (i = 0; i < n; i++) {
        qp_dictentry *ep = &keys->entries[i];
        if (!QP_DC_IS_NULL(ep->value)) {
            JS_DupValue(ctx, ep->value);
            JS_DupValue(ctx, ep->key);
        }
    }
    return keys;
}

/* dict_dict_merge. */
static int qp_dictcore_merge(JSContext *ctx, qp_dictcore *d,
                             qp_dictcore *other, JSValueConst other_obj,
                             int override, JSValue *dupkey,
                             qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t orig_size, pos, ix;

    if (other == d || other->used == 0)
        /* a.update(a) or a.update({}); nothing to do */
        return 0;
    if (d->used == 0) {
        /* Since the target dict is empty, contains() always returns 0.
         * Setting override to 1 skips the unnecessary test. */
        const qp_dictkeys *okeys = other->keys;
        override = 1;
        /* If other is clean, combined, and just allocated, just clone it. */
        if (!(d->version & QP_DC_TAG_SPLIT) && !(other->version & QP_DC_TAG_SPLIT) &&
            other->used == okeys->nentries &&
            (okeys->log2_size == QP_DC_LOG_MINSIZE ||
             (Py_ssize_t)QP_DC_USABLE_FRACTION(QP_DC_SIZE(okeys)/2) < other->used)) {
            qp_dictkeys *keys;
            qp_dictkeys *oldkeys;
            QP_DC_NOTIFY(ctx, d, QP_DC_EVENT_CLONED, other_obj, JS_UNINITIALIZED);
            keys = qp_dc_clone_keys(ctx, other);
            if (!keys)
                return -1;
            oldkeys = d->keys;
            qp_dc_set_keys(d, keys);
            d->used = other->used;
            qp_dc_keys_release(ctx->rt, oldkeys);
            return 0;
        }
    }
    /* Do one big resize at the start, rather than incrementally resizing as
     * we insert new items.  Expect that there will be no (or few)
     * overlapping keys. */
    if ((Py_ssize_t)QP_DC_USABLE_FRACTION(QP_DC_SIZE(d->keys)) < other->used) {
        int unicode = other->keys->unicode;
        if (qp_dc_resize(ctx, d,
                         qp_dc_estimate_log2_keysize(d->used + other->used),
                         unicode))
            return -1;
    }

    orig_size = other->used;
    pos = 0;
    while (qp_dictcore_next(other, &pos, &ix)) {
        qp_dictentry *ep = &other->keys->entries[ix];
        Py_hash_t hash = ep->hash;
        JSValue key = JS_DupValue(ctx, ep->key);
        JSValue value = JS_DupValue(ctx, ep->value);
        int err = 0;
        if (override == 1) {
            err = qp_dc_insertdict(ctx, d, JS_DupValue(ctx, key), hash,
                                   JS_DupValue(ctx, value), eq, opaque);
        } else {
            err = qp_dictcore_contains(ctx, d, key, hash, eq, opaque);
            if (err == 0) {
                err = qp_dc_insertdict(ctx, d, JS_DupValue(ctx, key), hash,
                                       JS_DupValue(ctx, value), eq, opaque);
            } else if (err > 0) {
                if (dupkey != NULL) {
                    *dupkey = key;
                    JS_FreeValue(ctx, value);
                    return QP_DICT_MERGE_DUP;
                }
                err = 0;
            }
        }
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, key);
        if (err != 0)
            return -1;
        if (orig_size != other->used)
            return QP_DICT_MERGE_MUTATED;
    }
    return 0;
}

/* copy_lock_held_untracked (combined dict, type without its own __iter__). */
static int qp_dictcore_copy(JSContext *ctx, qp_dictcore *dst, qp_dictcore *src,
                            qp_dictcore_eq_fn *eq, void *opaque)
{
    if (src->used == 0)
        /* The dict is empty; just return a new dict. */
        return 0;
    if (src->used >= (src->keys->nentries * 2) / 3) {
        /* Use fast-copy if 'src' has at most 1/3 non-used keys (a dict
         * almost emptied by del/pop is copied by merging instead, which
         * compacts it). */
        qp_dictkeys *keys = qp_dc_clone_keys(ctx, src);
        qp_dictkeys *oldkeys;
        if (!keys)
            return -1;
        oldkeys = dst->keys;
        qp_dc_set_keys(dst, keys);
        dst->used = src->used;
        qp_dc_keys_release(ctx->rt, oldkeys);
        return 0;
    }
    return qp_dictcore_merge(ctx, dst, src, JS_UNDEFINED, 1, NULL, eq, opaque);
}

/* The dictresize() call of dict_dict_fromkeys / dict_set_fromkeys. */
static int qp_dictcore_fromkeys_resize(JSContext *ctx, qp_dictcore *d,
                                       Py_ssize_t n, int unicode)
{
    uint8_t new_size = qp_dc_estimate_log2_keysize(n);
    if (new_size < d->keys->log2_size)
        new_size = d->keys->log2_size;
    return qp_dc_resize(ctx, d, new_size, unicode);
}

/* dict_equal_lock_held. */
static int qp_dictcore_equal(JSContext *ctx, qp_dictcore *a, qp_dictcore *b,
                             qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t i;

    if (a->used != b->used)
        /* can't be equal if # of entries differ */
        return 0;
    /* Same # of entries -- check all of 'em.  Exit early on any diff. */
    for (i = 0; i < a->keys->nentries; i++) {
        qp_dictentry *ep = &a->keys->entries[i];
        JSValue key, aval, bval;
        Py_hash_t hash;
        Py_ssize_t ix;
        int cmp;

        if (QP_DC_IS_NULL(ep->value))
            continue;
        /* temporarily keep aval and key alive until we're done with them */
        key = JS_DupValue(ctx, ep->key);
        aval = JS_DupValue(ctx, ep->value);
        hash = ep->hash;
        /* reuse the known hash value */
        ix = qp_dictcore_lookup(ctx, b, key, hash, eq, opaque, &bval);
        if (ix < 0) {
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, aval);
            return ix == QP_DKIX_ERROR ? -1 : 0;
        }
        bval = JS_DupValue(ctx, bval);
        cmp = eq(ctx, aval, bval, opaque);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, aval);
        JS_FreeValue(ctx, bval);
        if (cmp <= 0)  /* error or not equal */
            return cmp < 0 ? -1 : 0;
    }
    return 1;
}

/* Constants of tuple_hash (xxHash-based), 64- or 32-bit as in
 * Include/internal/pycore_tuple.h. */
#if INTPTR_MAX > INT32_MAX
#define QP_DC_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#define QP_DC_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#define QP_DC_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#define QP_DC_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#define QP_DC_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#define QP_DC_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#define QP_DC_XXPRIME_5 ((Py_uhash_t)374761393UL)
#define QP_DC_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif

/* _shuffle_bits (setobject.c / dictobject.c). */
static inline Py_uhash_t qp_dc_shuffle_bits(Py_uhash_t h)
{
    return ((h ^ (Py_uhash_t)89869747UL) ^ (h << 16)) * (Py_uhash_t)3644798167UL;
}

/* frozendict_pair_hash: hash((key, value)) with the key hash known. */
static Py_hash_t qp_dc_frozendict_pair_hash(JSContext *ctx, Py_hash_t key_hash,
                                            JSValueConst value,
                                            qp_dictcore_hash_fn *value_hash,
                                            void *opaque)
{
    const Py_ssize_t len = 2;
    Py_uhash_t acc = QP_DC_XXPRIME_5;
    Py_uhash_t lane = (Py_uhash_t)key_hash;

    acc += lane * QP_DC_XXPRIME_2;
    acc = QP_DC_XXROTATE(acc);
    acc *= QP_DC_XXPRIME_1;

    lane = (Py_uhash_t)value_hash(ctx, value, opaque);
    if (lane == (Py_uhash_t)-1)
        return -1;
    acc += lane * QP_DC_XXPRIME_2;
    acc = QP_DC_XXROTATE(acc);
    acc *= QP_DC_XXPRIME_1;

    /* Add input length, mangled to keep the historical value of hash(()). */
    acc += (Py_uhash_t)len ^ (QP_DC_XXPRIME_5 ^ (Py_uhash_t)3527539UL);

    if (acc == (Py_uhash_t)-1)
        acc = 1546275796;
    return (Py_hash_t)acc;
}

/* frozendict_hash (body). */
static Py_hash_t qp_dictcore_frozendict_hash(JSContext *ctx, qp_dictcore *d,
                                             qp_dictcore_hash_fn *value_hash,
                                             void *opaque)
{
    Py_uhash_t hash = 0;
    Py_ssize_t pos = 0, ix;

    while (qp_dictcore_next(d, &pos, &ix)) {
        qp_dictentry *ep = &d->keys->entries[ix];
        JSValue value = JS_DupValue(ctx, ep->value);
        Py_hash_t pair_hash = qp_dc_frozendict_pair_hash(ctx, ep->hash, value,
                                                         value_hash, opaque);
        JS_FreeValue(ctx, value);
        if (pair_hash == -1)
            return -1;
        hash ^= qp_dc_shuffle_bits((Py_uhash_t)pair_hash);
    }
    /* Factor in the number of active entries */
    hash ^= ((Py_uhash_t)d->used + 1) * (Py_uhash_t)1927868237UL;
    /* Disperse patterns arising in nested frozendicts */
    hash ^= (hash >> 11) ^ (hash >> 25);
    hash = hash * 69069U + (Py_uhash_t)907133923UL;
    /* -1 is reserved as an error code */
    if (hash == (Py_uhash_t)-1)
        hash = 590923713UL;
    return (Py_hash_t)hash;
}

/* ========================================================================= */
/* set                                                                       */
/* ========================================================================= */

#define QP_SC_LINEAR_PROBES 9
#define QP_SC_PERTURB_SHIFT 5

#define QP_SC_LOOKKEY_FOUND 1
#define QP_SC_LOOKKEY_NO_MATCH 0
#define QP_SC_LOOKKEY_ERROR (-1)
#define QP_SC_LOOKKEY_CHANGED (-2)
#define QP_SC_LOOKKEY_EMPTY (-3)

/* set_zero_table: every slot unused (key NULL, hash 0). */
static void qp_sc_zero_table(qp_setentry *table, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        table[i].key = JS_UNINITIALIZED;
        table[i].hash = 0;
    }
}

/* Point so->table at `table`, bumping the generation when it changes (the
 * set counterpart of CPython's `table != so->table` restart test, immune to
 * a freed table being reallocated at the same address). */
static inline void qp_sc_set_table(qp_setcore *so, qp_setentry *table)
{
    if (so->table != table)
        so->table_gen++;
    so->table = table;
}

static void qp_setcore_init(qp_setcore *s)
{
    s->fill = 0;
    s->used = 0;
    s->mask = QP_SET_MINSIZE - 1;
    s->table = s->smalltable;
    s->table_gen = 0;
    s->hash = -1;
    s->finger = 0;
    qp_sc_zero_table(s->smalltable, QP_SET_MINSIZE);
}

/* set_dealloc (minus the object): release active keys, free the table. */
static void qp_setcore_free(JSRuntime *rt, qp_setcore *s)
{
    qp_setentry *entry;
    Py_ssize_t used = s->used;
    uint64_t gen = s->table_gen;

    for (entry = s->table; used > 0; entry++) {
        if (QP_SETENTRY_ACTIVE(entry)) {
            used--;
            JS_FreeValueRT(rt, entry->key);
        }
    }
    if (s->table != s->smalltable)
        js_free_rt(rt, s->table);
    qp_setcore_init(s);
    s->table_gen = gen + 1;
}

static void qp_setcore_mark(JSRuntime *rt, const qp_setcore *s,
                            JS_MarkFunc *mark_func)
{
    const qp_setentry *entry;
    Py_ssize_t used = s->used;
    for (entry = s->table; used > 0; entry++) {
        if (QP_SETENTRY_ACTIVE(entry)) {
            used--;
            JS_MarkValue(rt, entry->key, mark_func);
        }
    }
}

/* set_compare_entry_lock_held (the frozenset variant gives the same results
 * since a frozenset cannot be mutated by the callback). */
static inline int qp_sc_compare_entry(JSContext *ctx, qp_setcore *so,
                                      qp_setentry *entry, JSValueConst key,
                                      Py_hash_t hash,
                                      qp_dictcore_eq_fn *eq, void *opaque)
{
    if (entry->hash == 0 && QP_DC_IS_NULL(entry->key))
        return QP_SC_LOOKKEY_EMPTY;
    if (entry->hash == hash) {
        JSValue startkey = entry->key;
        qp_setentry *table;
        uint64_t gen;
        int cmp;
        if (qp_dc_same(startkey, key))
            return QP_SC_LOOKKEY_FOUND;
        if (QP_DC_IS_STR(startkey) && QP_DC_IS_STR(key) &&
            qp_dc_str_eq(ctx, startkey, key))
            return QP_SC_LOOKKEY_FOUND;
        table = so->table;
        gen = so->table_gen;
        JS_DupValue(ctx, startkey);
        cmp = eq(ctx, startkey, key, opaque);
        JS_FreeValue(ctx, startkey);
        if (cmp < 0)
            return QP_SC_LOOKKEY_ERROR;
        if (table != so->table || gen != so->table_gen ||
            !qp_dc_same(entry->key, startkey))
            return QP_SC_LOOKKEY_CHANGED;
        if (cmp > 0)
            return QP_SC_LOOKKEY_FOUND;
    }
    return QP_SC_LOOKKEY_NO_MATCH;
}

/* set_do_lookup: linear probing in runs of LINEAR_PROBES + 1 slots, then
 * perturbation.  FOUND (*epp set), NO_MATCH, ERROR or CHANGED. */
static int qp_sc_do_lookup(JSContext *ctx, qp_setcore *so, qp_setentry *table,
                           size_t mask, JSValueConst key, Py_hash_t hash,
                           qp_setentry **epp,
                           qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_setentry *entry;
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask; /* Unsigned for defined overflow behavior */
    int probes;
    int status;

    for (;;) {
        entry = &table[i];
        probes = (i + QP_SC_LINEAR_PROBES <= mask) ? QP_SC_LINEAR_PROBES : 0;
        do {
            status = qp_sc_compare_entry(ctx, so, entry, key, hash, eq, opaque);
            if (status != QP_SC_LOOKKEY_NO_MATCH) {
                if (status == QP_SC_LOOKKEY_EMPTY)
                    return QP_SC_LOOKKEY_NO_MATCH;
                *epp = entry;
                return status;
            }
            entry++;
        } while (probes--);
        perturb >>= QP_SC_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

/* set_lookkey: restart while the table changes under the comparison. */
static int qp_sc_lookkey(JSContext *ctx, qp_setcore *so, JSValueConst key,
                         Py_hash_t hash, qp_setentry **epp,
                         qp_dictcore_eq_fn *eq, void *opaque)
{
    int status;
    do {
        status = qp_sc_do_lookup(ctx, so, so->table, (size_t)so->mask, key,
                                 hash, epp, eq, opaque);
    } while (status == QP_SC_LOOKKEY_CHANGED);
    return status;
}

/* set_insert_clean: insert a key known to be absent into a table without
 * dummies; the caller updates fill/used and owns the reference transfer. */
static void qp_sc_insert_clean(qp_setentry *table, size_t mask, JSValue key,
                               Py_hash_t hash)
{
    qp_setentry *entry;
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;
    size_t j;

    for (;;) {
        entry = &table[i];
        if (QP_DC_IS_NULL(entry->key))
            goto found_null;
        if (i + QP_SC_LINEAR_PROBES <= mask) {
            for (j = 0; j < QP_SC_LINEAR_PROBES; j++) {
                entry++;
                if (QP_DC_IS_NULL(entry->key))
                    goto found_null;
            }
        }
        perturb >>= QP_SC_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
 found_null:
    entry->hash = hash;
    entry->key = key;
}

/* set_table_resize: rebuild into the smallest power-of-two table (>= 8)
 * larger than minused, dropping dummies.  A small table that only needs
 * purging is rebuilt in place (its address does not change).  No callbacks
 * run here. */
static int qp_setcore_table_resize(JSContext *ctx, qp_setcore *so,
                                   Py_ssize_t minused)
{
    qp_setentry *oldtable, *newtable, *entry;
    Py_ssize_t oldmask = so->mask;
    size_t newmask;
    int is_oldtable_malloced;
    qp_setentry small_copy[QP_SET_MINSIZE];
    size_t newsize = QP_SET_MINSIZE;

    /* Find the smallest table size > minused. */
    while (newsize <= (size_t)minused)
        newsize <<= 1; /* The largest possible value is PY_SSIZE_T_MAX + 1. */

    /* Get space for a new table. */
    oldtable = so->table;
    is_oldtable_malloced = oldtable != so->smalltable;

    if (newsize == QP_SET_MINSIZE) {
        /* A large table is shrinking, or we can't get any smaller. */
        newtable = so->smalltable;
        if (newtable == oldtable) {
            if (so->fill == so->used) {
                /* No dummies, so no point doing anything. */
                return 0;
            }
            /* We're not going to resize it, but rebuild the table anyway to
             * purge old dummy entries (necessary if fill == size, since a
             * failing search needs at least one virgin slot). */
            memcpy(small_copy, oldtable, sizeof(small_copy));
            oldtable = small_copy;
        }
    } else {
        if (newsize > SIZE_MAX / sizeof(qp_setentry)) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        newtable = js_malloc(ctx, newsize * sizeof(qp_setentry));
        if (!newtable)
            return -1;
    }

    /* Make the set empty, using the new table. */
    qp_sc_zero_table(newtable, newsize);
    so->mask = (Py_ssize_t)(newsize - 1);

    /* Copy the data over; this is refcount-neutral for active entries;
     * dummy entries aren't copied over, of course */
    newmask = (size_t)so->mask;
    if (so->fill == so->used) {
        for (entry = oldtable; entry <= oldtable + oldmask; entry++) {
            if (!QP_DC_IS_NULL(entry->key))
                qp_sc_insert_clean(newtable, newmask, entry->key, entry->hash);
        }
    } else {
        so->fill = so->used;
        for (entry = oldtable; entry <= oldtable + oldmask; entry++) {
            if (QP_SETENTRY_ACTIVE(entry))
                qp_sc_insert_clean(newtable, newmask, entry->key, entry->hash);
        }
    }
    qp_sc_set_table(so, newtable);
    if (is_oldtable_malloced)
        js_free(ctx, oldtable);
    return 0;
}

/* set_add_entry_takeref. */
static int qp_setcore_add_take(JSContext *ctx, qp_setcore *so, JSValue key,
                               Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_setentry *table;
    qp_setentry *freeslot;
    qp_setentry *entry;
    size_t perturb;
    size_t mask;
    size_t i;                       /* Unsigned for defined overflow behavior */
    uint64_t gen;
    int probes;
    int cmp;

 restart:
    mask = (size_t)so->mask;
    i = (size_t)hash & mask;
    freeslot = NULL;
    perturb = (size_t)hash;

    for (;;) {
        entry = &so->table[i];
        probes = (i + QP_SC_LINEAR_PROBES <= mask) ? QP_SC_LINEAR_PROBES : 0;
        do {
            if (entry->hash == 0 && QP_DC_IS_NULL(entry->key))
                goto found_unused_or_dummy;
            if (entry->hash == hash) {
                JSValue startkey = entry->key;
                if (qp_dc_same(startkey, key))
                    goto found_active;
                if (QP_DC_IS_STR(startkey) && QP_DC_IS_STR(key) &&
                    qp_dc_str_eq(ctx, startkey, key))
                    goto found_active;
                table = so->table;
                gen = so->table_gen;
                JS_DupValue(ctx, startkey);
                cmp = eq(ctx, startkey, key, opaque);
                JS_FreeValue(ctx, startkey);
                if (cmp > 0)
                    goto found_active;
                if (cmp < 0)
                    goto comparison_error;
                if (table != so->table || gen != so->table_gen ||
                    !qp_dc_same(entry->key, startkey))
                    goto restart;
                mask = (size_t)so->mask;
            } else if (entry->hash == -1) {
                freeslot = entry;
            }
            entry++;
        } while (probes--);
        perturb >>= QP_SC_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }

 found_unused_or_dummy:
    if (freeslot == NULL)
        goto found_unused;
    if (freeslot->hash != -1)
        goto restart;
    so->used++;
    freeslot->hash = hash;
    freeslot->key = key;
    return 0;

 found_unused:
    so->fill++;
    so->used++;
    entry->hash = hash;
    entry->key = key;
    if ((size_t)so->fill*5 < mask*3)
        return 0;
    return qp_setcore_table_resize(ctx, so,
                                   so->used > 50000 ? so->used*2 : so->used*4);

 found_active:
    JS_FreeValue(ctx, key);
    return 0;

 comparison_error:
    JS_FreeValue(ctx, key);
    return -1;
}

/* set_add_entry. */
static int qp_setcore_add(JSContext *ctx, qp_setcore *s, JSValueConst key,
                          Py_hash_t hash, qp_dictcore_eq_fn *eq, void *opaque)
{
    return qp_setcore_add_take(ctx, s, JS_DupValue(ctx, key), hash, eq, opaque);
}

/* set_contains_entry. */
static int qp_setcore_contains(JSContext *ctx, qp_setcore *s, JSValueConst key,
                               Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_setentry *entry; /* unused */
    return qp_sc_lookkey(ctx, s, key, hash, &entry, eq, opaque);
}

/* set_discard_entry. */
static int qp_setcore_discard(JSContext *ctx, qp_setcore *so, JSValueConst key,
                              Py_hash_t hash,
                              qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_setentry *entry;
    JSValue old_key;
    int status = qp_sc_lookkey(ctx, so, key, hash, &entry, eq, opaque);
    if (status < 0)
        return -1;
    if (status == QP_SC_LOOKKEY_NO_MATCH)
        return 0;
    old_key = entry->key;
    entry->hash = -1;
    so->used--;
    entry->key = QP_SETCORE_DUMMY;
    JS_FreeValue(ctx, old_key);
    return 1;
}

/* set_empty_to_minsize. */
static void qp_sc_empty_to_minsize(qp_setcore *so)
{
    qp_sc_zero_table(so->smalltable, QP_SET_MINSIZE);
    so->fill = 0;
    so->used = 0;
    so->mask = QP_SET_MINSIZE - 1;
    so->hash = -1;
    qp_sc_set_table(so, so->smalltable);
}

/* set_clear_internal: empty the set before releasing anything, since
 * releasing can run code that mutates it; a small table is copied aside. */
static void qp_setcore_clear(JSContext *ctx, qp_setcore *so)
{
    qp_setentry *entry;
    qp_setentry *table = so->table;
    Py_ssize_t fill = so->fill;
    Py_ssize_t used = so->used;
    int table_is_malloced = table != so->smalltable;
    qp_setentry small_copy[QP_SET_MINSIZE];

    if (table_is_malloced) {
        qp_sc_empty_to_minsize(so);
    } else if (fill > 0) {
        /* It's a small table with something that needs to be cleared. */
        memcpy(small_copy, table, sizeof(small_copy));
        table = small_copy;
        qp_sc_empty_to_minsize(so);
    }
    /* else it's a small table that's already empty */

    for (entry = table; used > 0; entry++) {
        if (QP_SETENTRY_ACTIVE(entry)) {
            used--;
            JS_FreeValue(ctx, entry->key);
        }
    }
    if (table_is_malloced)
        js_free(ctx, table);
}

/* set_next. */
static int qp_setcore_next(const qp_setcore *so, Py_ssize_t *pos_ptr,
                           qp_setentry **entry_ptr)
{
    Py_ssize_t i = *pos_ptr;
    Py_ssize_t mask = so->mask;
    qp_setentry *entry = &so->table[i];

    while (i <= mask && !QP_SETENTRY_ACTIVE(entry)) {
        i++;
        entry++;
    }
    *pos_ptr = i + 1;
    if (i > mask)
        return 0;
    *entry_ptr = entry;
    return 1;
}

/* set_pop_impl: scan from the finger (wrapping), leave a dummy. */
static int qp_setcore_pop(qp_setcore *so, JSValue *pkey)
{
    /* Make sure the search finger is in bounds */
    qp_setentry *entry = so->table + (so->finger & so->mask);
    qp_setentry *limit = so->table + so->mask;

    if (so->used == 0)
        return 0;
    while (!QP_SETENTRY_ACTIVE(entry)) {
        entry++;
        if (entry > limit)
            entry = so->table;
    }
    entry->hash = -1;
    so->used--;
    *pkey = entry->key;
    entry->key = QP_SETCORE_DUMMY;
    so->finger = entry - so->table + 1;   /* next place to start */
    return 1;
}

static void qp_setcore_iter_init(const qp_setcore *s, qp_setcore_iter *it)
{
    it->used = s->used;
    it->pos = 0;
    it->len = s->used;
}

/* setiter_iternext. */
static int qp_setcore_iter_next(const qp_setcore *so, qp_setcore_iter *it,
                                JSValue *pkey)
{
    Py_ssize_t i, mask;
    const qp_setentry *entry;

    if (it->used != so->used) {
        it->used = -1; /* Make this state sticky */
        return QP_SETITER_CHANGED_SIZE;
    }
    i = it->pos;
    entry = so->table;
    mask = so->mask;
    while (i <= mask && !QP_SETENTRY_ACTIVE(&entry[i]))
        i++;
    it->pos = i + 1;
    if (i > mask)
        return 0;
    it->len--;
    *pkey = entry[i].key;
    return 1;
}

/* setiter_len. */
static Py_ssize_t qp_setcore_iter_length_hint(const qp_setcore *s,
                                              const qp_setcore_iter *it)
{
    if (it->used == s->used)
        return it->len;
    return 0;
}

/* set_merge_lock_held. */
static int qp_setcore_merge(JSContext *ctx, qp_setcore *so, qp_setcore *other,
                            qp_dictcore_eq_fn *eq, void *opaque)
{
    JSValue key;
    Py_ssize_t i;
    qp_setentry *so_entry;
    qp_setentry *other_entry;

    if (other == so || other->used == 0)
        /* a.update(a) or a.update(set()); nothing to do */
        return 0;
    /* Do one big resize at the start, rather than incrementally resizing as
     * we insert new keys.  Expect that there will be no (or few) overlapping
     * keys. */
    if ((so->fill + other->used)*5 >= so->mask*3) {
        if (qp_setcore_table_resize(ctx, so, (so->used + other->used)*2) != 0)
            return -1;
    }
    so_entry = so->table;
    other_entry = other->table;

    /* If our table is empty, and both tables have the same size, and there
     * are no dummies to eliminate, then just copy the pointers. */
    if (so->fill == 0 && so->mask == other->mask && other->fill == other->used) {
        for (i = 0; i <= other->mask; i++, so_entry++, other_entry++) {
            key = other_entry->key;
            if (!QP_DC_IS_NULL(key)) {
                so_entry->hash = other_entry->hash;
                so_entry->key = JS_DupValue(ctx, key);
            }
        }
        so->fill = other->fill;
        so->used = other->used;
        return 0;
    }

    /* If our table is empty, we can use set_insert_clean() */
    if (so->fill == 0) {
        qp_setentry *newtable = so->table;
        size_t newmask = (size_t)so->mask;
        so->fill = other->used;
        so->used = other->used;
        for (i = other->mask + 1; i > 0; i--, other_entry++) {
            key = other_entry->key;
            if (QP_SETENTRY_ACTIVE(other_entry))
                qp_sc_insert_clean(newtable, newmask, JS_DupValue(ctx, key),
                                   other_entry->hash);
        }
        return 0;
    }

    /* We can't assure there are no duplicates, so do normal insertions */
    for (i = 0; i <= other->mask; i++) {
        other_entry = &other->table[i];
        if (QP_SETENTRY_ACTIVE(other_entry)) {
            if (qp_setcore_add(ctx, so, other_entry->key, other_entry->hash,
                               eq, opaque))
                return -1;
        }
    }
    return 0;
}

/* set_update_dict_lock_held. */
static int qp_setcore_update_dict(JSContext *ctx, qp_setcore *so,
                                  qp_dictcore *other,
                                  qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t dictsize = other->used;
    Py_ssize_t pos = 0, ix;

    /* Do one big resize at the start, rather than incrementally resizing as
     * we insert new keys.  Expect that there will be no (or few) overlapping
     * keys. */
    if ((so->fill + dictsize)*5 >= so->mask*3) {
        if (qp_setcore_table_resize(ctx, so, (so->used + dictsize)*2) != 0)
            return -1;
    }
    while (qp_dictcore_next(other, &pos, &ix)) {
        qp_dictentry *ep = &other->keys->entries[ix];
        if (qp_setcore_add(ctx, so, ep->key, ep->hash, eq, opaque))
            return -1;
    }
    return 0;
}

/* set_swap_bodies: exchange contents (tables, smalltables when in use,
 * fill/used/mask; hashes as documented). */
static void qp_setcore_swap_bodies(qp_setcore *a, qp_setcore *b,
                                   int both_frozen)
{
    Py_ssize_t t;
    qp_setentry *u;
    qp_setentry tab[QP_SET_MINSIZE];
    Py_hash_t h;
    qp_setentry *a_table = a->table;
    qp_setentry *b_table = b->table;

    t = a->fill;     a->fill   = b->fill;        b->fill  = t;
    t = a->used;     a->used   = b->used;        b->used  = t;
    t = a->mask;     a->mask   = b->mask;        b->mask  = t;

    u = a_table;
    if (a_table == a->smalltable)
        u = b->smalltable;
    a_table = b_table;
    if (b_table == b->smalltable)
        a_table = a->smalltable;
    b_table = u;

    if (a_table == a->smalltable || b_table == b->smalltable) {
        memcpy(tab, a->smalltable, sizeof(tab));
        memcpy(a->smalltable, b->smalltable, sizeof(tab));
        memcpy(b->smalltable, tab, sizeof(tab));
    }

    if (both_frozen) {
        h = a->hash;
        a->hash = b->hash;
        b->hash = h;
    } else {
        a->hash = -1;
        b->hash = -1;
    }
    /* Both tables changed identity from the point of view of a comparison in
     * progress. */
    a->table = a_table;
    b->table = b_table;
    a->table_gen++;
    b->table_gen++;
}

/* set_intersection, set/frozenset `other` branch (plus so == other). */
static int qp_setcore_intersection(JSContext *ctx, qp_setcore *result,
                                   qp_setcore *so, qp_setcore *other,
                                   qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t pos = 0;
    qp_setentry *entry;

    if (so == other)
        return qp_setcore_merge(ctx, result, so, eq, opaque);  /* set_copy_impl */

    if (other->used > so->used) {
        qp_setcore *tmp = so;
        so = other;
        other = tmp;
    }
    while (qp_setcore_next(other, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        Py_hash_t hash = entry->hash;
        int rv = qp_setcore_contains(ctx, so, key, hash, eq, opaque);
        if (rv < 0) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        if (rv) {
            if (qp_setcore_add(ctx, result, key, hash, eq, opaque)) {
                JS_FreeValue(ctx, key);
                return -1;
            }
        }
        JS_FreeValue(ctx, key);
    }
    return 0;
}

/* set_isdisjoint_impl, so == other and exact set/frozenset branches. */
static int qp_setcore_isdisjoint(JSContext *ctx, qp_setcore *so,
                                 qp_setcore *other,
                                 qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t pos = 0;
    qp_setentry *entry;

    if (so == other)
        return so->used == 0;
    if (other->used > so->used) {
        qp_setcore *tmp = so;
        so = other;
        other = tmp;
    }
    while (qp_setcore_next(other, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        int rv = qp_setcore_contains(ctx, so, key, entry->hash, eq, opaque);
        JS_FreeValue(ctx, key);
        if (rv < 0)
            return -1;
        if (rv)
            return 0;
    }
    return 1;
}

/* Tail of set_difference_update_internal. */
static int qp_setcore_purge_dummies(JSContext *ctx, qp_setcore *so)
{
    /* If more than 1/4th are dummies, then resize them away. */
    if ((size_t)(so->fill - so->used) <= (size_t)so->mask / 4)
        return 0;
    return qp_setcore_table_resize(ctx, so,
                                   so->used > 50000 ? so->used*2 : so->used*4);
}

/* set_difference_update_internal, set/frozenset `other` branch. */
static int qp_setcore_difference_update_set(JSContext *ctx, qp_setcore *so,
                                            qp_setcore *other,
                                            qp_dictcore_eq_fn *eq,
                                            void *opaque)
{
    qp_setentry *entry;
    Py_ssize_t pos = 0;
    qp_setcore tmp;
    int use_tmp = 0;

    if (so == other) {
        qp_setcore_clear(ctx, so);
        return 0;
    }
    /* Optimization:  When the other set is more than 8 times larger than the
     * base set, replace the other set with intersection of the two sets. */
    if ((other->used >> 3) > so->used) {
        qp_setcore_init(&tmp);
        use_tmp = 1;
        if (qp_setcore_intersection(ctx, &tmp, so, other, eq, opaque) < 0) {
            qp_setcore_free(ctx->rt, &tmp);
            return -1;
        }
        other = &tmp;
    }
    while (qp_setcore_next(other, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        if (qp_setcore_discard(ctx, so, key, entry->hash, eq, opaque) < 0) {
            JS_FreeValue(ctx, key);
            if (use_tmp)
                qp_setcore_free(ctx->rt, &tmp);
            return -1;
        }
        JS_FreeValue(ctx, key);
    }
    if (use_tmp)
        qp_setcore_free(ctx->rt, &tmp);
    return qp_setcore_purge_dummies(ctx, so);
}

/* set_difference_untracked, set/frozenset `other` loop. */
static int qp_setcore_difference_set(JSContext *ctx, qp_setcore *result,
                                     qp_setcore *so, qp_setcore *other,
                                     qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t pos = 0;
    qp_setentry *entry;

    /* Iterate over so, checking for common elements in other. */
    while (qp_setcore_next(so, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        Py_hash_t hash = entry->hash;
        int rv = qp_setcore_contains(ctx, other, key, hash, eq, opaque);
        if (rv < 0) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        if (!rv) {
            if (qp_setcore_add(ctx, result, key, hash, eq, opaque)) {
                JS_FreeValue(ctx, key);
                return -1;
            }
        }
        JS_FreeValue(ctx, key);
    }
    return 0;
}

/* set_difference_untracked, exact dict/frozendict `other` loop. */
static int qp_setcore_difference_dict(JSContext *ctx, qp_setcore *result,
                                      qp_setcore *so, qp_dictcore *other,
                                      qp_dictcore_eq_fn *eq, void *opaque)
{
    Py_ssize_t pos = 0;
    qp_setentry *entry;

    while (qp_setcore_next(so, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        Py_hash_t hash = entry->hash;
        int rv = qp_dictcore_contains(ctx, other, key, hash, eq, opaque);
        if (rv < 0) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        if (!rv) {
            if (qp_setcore_add(ctx, result, key, hash, eq, opaque)) {
                JS_FreeValue(ctx, key);
                return -1;
            }
        }
        JS_FreeValue(ctx, key);
    }
    return 0;
}

/* set_symmetric_difference_update_set. */
static int qp_setcore_symmetric_difference_update_set(JSContext *ctx,
                                                      qp_setcore *so,
                                                      qp_setcore *other,
                                                      qp_dictcore_eq_fn *eq,
                                                      void *opaque)
{
    Py_ssize_t pos = 0;
    qp_setentry *entry;

    while (qp_setcore_next(other, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        Py_hash_t hash = entry->hash;
        int rv = qp_setcore_discard(ctx, so, key, hash, eq, opaque);
        if (rv < 0) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        if (rv == 0) {
            if (qp_setcore_add(ctx, so, key, hash, eq, opaque)) {
                JS_FreeValue(ctx, key);
                return -1;
            }
        }
        JS_FreeValue(ctx, key);
    }
    return 0;
}

/* set_symmetric_difference_update_dict. */
static int qp_setcore_symmetric_difference_update_dict(JSContext *ctx,
                                                       qp_setcore *so,
                                                       qp_dictcore *other,
                                                       qp_dictcore_eq_fn *eq,
                                                       void *opaque)
{
    Py_ssize_t pos = 0, ix;

    while (qp_dictcore_next(other, &pos, &ix)) {
        qp_dictentry *ep = &other->keys->entries[ix];
        JSValue key = JS_DupValue(ctx, ep->key);
        Py_hash_t hash = ep->hash;
        int rv = qp_setcore_discard(ctx, so, key, hash, eq, opaque);
        if (rv < 0) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        if (rv == 0) {
            if (qp_setcore_add(ctx, so, key, hash, eq, opaque)) {
                JS_FreeValue(ctx, key);
                return -1;
            }
        }
        JS_FreeValue(ctx, key);
    }
    return 0;
}

/* set_issubset_impl, set/frozenset `other` branch. */
static int qp_setcore_issubset(JSContext *ctx, qp_setcore *so,
                               qp_setcore *other,
                               qp_dictcore_eq_fn *eq, void *opaque)
{
    qp_setentry *entry;
    Py_ssize_t pos = 0;

    if (so->used > other->used)
        return 0;
    while (qp_setcore_next(so, &pos, &entry)) {
        JSValue key = JS_DupValue(ctx, entry->key);
        int rv = qp_setcore_contains(ctx, other, key, entry->hash, eq, opaque);
        JS_FreeValue(ctx, key);
        if (rv < 0)
            return -1;
        if (!rv)
            return 0;
    }
    return 1;
}

/* frozenset_hash_impl: xor of shuffled entry hashes over the whole table
 * (unused slots hash 0, dummies -1, their effect removed afterwards), mixed
 * with the size. */
static Py_hash_t qp_setcore_hash_impl(const qp_setcore *so)
{
    Py_uhash_t hash = 0;
    const qp_setentry *entry;

    for (entry = so->table; entry <= &so->table[so->mask]; entry++)
        hash ^= qp_dc_shuffle_bits((Py_uhash_t)entry->hash);

    /* Remove the effect of an odd number of NULL entries */
    if ((so->mask + 1 - so->fill) & 1)
        hash ^= qp_dc_shuffle_bits(0);

    /* Remove the effect of an odd number of dummy entries */
    if ((so->fill - so->used) & 1)
        hash ^= qp_dc_shuffle_bits((Py_uhash_t)-1);

    /* Factor in the number of active entries */
    hash ^= ((Py_uhash_t)so->used + 1) * (Py_uhash_t)1927868237UL;

    /* Disperse patterns arising in nested frozensets */
    hash ^= (hash >> 11) ^ (hash >> 25);
    hash = hash * 69069U + (Py_uhash_t)907133923UL;

    /* -1 is reserved as an error code */
    if (hash == (Py_uhash_t)-1)
        hash = 590923713UL;

    return (Py_hash_t)hash;
}

/* frozenset_hash. */
static Py_hash_t qp_setcore_frozenset_hash(qp_setcore *so)
{
    if (so->hash != -1)
        return so->hash;
    so->hash = qp_setcore_hash_impl(so);
    return so->hash;
}

#undef QP_DC_IS_NULL
#undef QP_DC_IS_STR
#undef QP_DC_WATCHED
#undef QP_DC_NOTIFY
#undef QP_DC_LOG_MINSIZE
#undef QP_DC_MINSIZE
#undef QP_DC_PERTURB_SHIFT
#undef QP_DC_USABLE_FRACTION
#undef QP_DC_GROWTH_RATE
#undef QP_DC_SIZE
#undef QP_DC_MASK
#undef QP_DKIX_KEY_CHANGED
#undef QP_DC_HDR_SIZE
#undef QP_DC_EMPTY_KEYS
#undef QP_DC_XXPRIME_1
#undef QP_DC_XXPRIME_2
#undef QP_DC_XXPRIME_5
#undef QP_DC_XXROTATE
#undef QP_SC_LINEAR_PROBES
#undef QP_SC_PERTURB_SHIFT
#undef QP_SC_LOOKKEY_FOUND
#undef QP_SC_LOOKKEY_NO_MATCH
#undef QP_SC_LOOKKEY_ERROR
#undef QP_SC_LOOKKEY_CHANGED
#undef QP_SC_LOOKKEY_EMPTY
