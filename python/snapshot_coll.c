/*
 * snapshot_coll.c - heap snapshot payloads of the Python container and
 * small value classes (part `coll` of python/snapshot.c, same TU):
 *
 *   PY_COMPLEX     QPComplex                  (complexobject.c)
 *   PY_TUPLE       QPTuple                    (tupleobject.c)
 *   PY_RANGE       QPRange                    (rangeobject.c)
 *   PY_SLICE       QPSlice                    (sliceobject.c)
 *   PY_ITER        QPIterObj                  (iterobject.c)
 *   PY_DICT        QPDict over qp_dictcore    (dictobject.c / dictcore.c)
 *   PY_FROZENDICT  QPDict
 *   PY_SET         QPSet over qp_setcore      (setobject.c / dictcore.c)
 *   PY_FROZENSET   QPSet
 *   PY_DICT_ITER   QPDictIter (dictobject.c) or QPSetIter (setobject.c):
 *                  both payloads start with a `kind` byte and the set one
 *                  has kind == QP_DICTITER_KIND_SET (the test the class
 *                  finalizer/gc_mark use to route to the set functions)
 *   PY_DICT_VIEW   QPDictView (dict_keys/values/items and mappingproxy)
 *
 * Every payload starts with a presence byte (0 = u.opaque was NULL, which
 * every finalizer of these classes accepts).
 *
 * Hash tables
 * -----------
 * dict / frozendict: the keys table is written exactly as it is in memory:
 * table size, kind (unicode or general), usable, nentries, every index slot
 * (EMPTY, DUMMY or an entry index) and every entry up to nentries, deleted
 * ones included.  Iteration order (entry order, holes of deleted entries
 * included) and the positions held by dict iterators therefore survive
 * unchanged.  `uid` and `version` are kept (QPState.next_dict_uid is part of
 * the framework's QPState and keeps uids unique); keys_gen restarts, it only
 * detects table replacement during a single lookup.
 *
 * set / frozenset: the open-addressing table is written slot by slot
 * (unused, dummy or active key + hash), so the iteration order and the slot
 * positions held by set iterators survive unchanged.  fill/used/finger are
 * kept; table_gen restarts (lookup restart detection only).
 *
 * Stored hashes: object addresses change across a snapshot, so the stored
 * hash of a key whose hash depends on identity (object.__hash__, functions,
 * types, tuples containing such objects, ...) or on per-process state is
 * stale after restore.  Each non-empty dict/set registers a deferred
 * re-validation (qp_snap_defer) that recomputes every key hash with
 * qp_hash() (what qp_dict_prepare_key and the set code use; it dispatches
 * str to qp_str_hash).  When every hash is unchanged nothing is touched, so
 * str/int keyed tables keep their exact layout.  When some hash changed:
 *  - dict: the entries are updated in place and only the index table is
 *    rebuilt (entries, iteration order and iterator positions unchanged);
 *    core.version is bumped as for any table rebuild;
 *  - set: the table is rebuilt at the same size inserting the active keys
 *    in their old slot order (set_insert_clean, dummies dropped); the
 *    iteration order then follows the new hashes as it would in CPython,
 *    and a set iterator that was part way through such a set may skip or
 *    repeat keys (its position is a slot index of the old layout).
 * qp_hash may run Python __hash__ code; if that code mutates the table
 * being re-validated the restore fails with RuntimeError instead of
 * re-indexing a table that changed under it.
 *
 * Caches restored empty: tuple hash, frozendict hash, frozenset hash
 * (QPSet.hash and qp_setcore.hash) are -1.
 */

/* Bytes still unread: every element read below takes at least one byte, so
   counts larger than this are invalid input (checked before allocating). */
static size_t qp_snap_coll_left(SnapReader *r)
{
    return (size_t)(r->bc.buf_end - r->bc.ptr);
}

static void qp_snap_coll_put_present(SnapWriter *w, const void *payload)
{
    bc_put_u8(&w->bc, payload != NULL);
}

/* 1 = payload present, 0 = absent (u.opaque stays NULL), -1 = error. */
static int qp_snap_coll_get_present(SnapReader *r)
{
    uint8_t present;
    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (present > 1)
        return snap_error(r, "invalid Python payload flag");
    return present;
}

static int qp_snap_coll_get_ssize(SnapReader *r, Py_ssize_t *pv)
{
    int64_t v;
    if (qp_snap_get_i64(r, &v))
        return -1;
    if (v < PY_SSIZE_T_MIN || v > PY_SSIZE_T_MAX)
        return snap_error(r, "invalid Python size");
    *pv = (Py_ssize_t)v;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* dict / frozendict                                                         */
/* ------------------------------------------------------------------------ */

/* Largest dict table written (1 << 30 slots, int32 indices). */
#define QP_SNAP_COLL_DICT_MAX_LOG2 30

/* dictcore.c #undefs its private macros at its end; these mirror
   QP_DC_EMPTY_KEYS, QP_DC_IS_NULL, QP_DC_IS_STR and QP_DC_USABLE_FRACTION. */
#define QP_SNAP_COLL_EMPTY_KEYS (&qp_dc_empty_keys)

static inline BOOL qp_snap_coll_is_null(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED;
}

static inline Py_ssize_t qp_snap_coll_usable_fraction(uint8_t log2_size)
{
    return (((Py_ssize_t)1 << log2_size) << 1) / 3;
}

static int qp_snap_coll_write_dict(SnapWriter *w, JSObject *p)
{
    QPDict *d = p->u.opaque;
    qp_dictkeys *keys;
    size_t i, size;
    Py_ssize_t n;

    qp_snap_coll_put_present(w, d);
    if (!d)
        return 0;
    keys = d->core.keys;
    if (keys == QP_SNAP_COLL_EMPTY_KEYS) {
        bc_put_u8(&w->bc, 0);
    } else {
        if (keys->log2_size > QP_SNAP_COLL_DICT_MAX_LOG2)
            return qp_snap_reject(w, p, "dict table too large");
        bc_put_u8(&w->bc, keys->log2_size);
        bc_put_u8(&w->bc, keys->unicode);
        if (qp_snap_put_i64(w, keys->usable) ||
            qp_snap_put_i64(w, keys->nentries))
            return -1;
    }
    if (qp_snap_put_i64(w, d->core.used) ||
        qp_snap_put_i64(w, (int64_t)d->core.version) ||
        qp_snap_put_i64(w, (int64_t)d->uid))
        return -1;
    if (keys == QP_SNAP_COLL_EMPTY_KEYS)
        return 0;
    size = (size_t)1 << keys->log2_size;
    for (i = 0; i < size; i++)
        bc_put_sleb128(&w->bc, (int32_t)qp_dc_get_index(keys, i));
    n = keys->nentries;
    for (i = 0; i < (size_t)n; i++) {
        const qp_dictentry *ep = &keys->entries[i];
        /* deleted entries hold QP_NULL key and value */
        if (snap_put_value(w, ep->key) ||
            snap_put_value(w, ep->value) ||
            qp_snap_put_i64(w, ep->hash))
            return -1;
    }
    return 0;
}

/* Deferred: re-validate the stored hashes of a restored dict (see the file
   comment).  Rebuilds only the index table, and only if a hash changed. */
static int qp_snap_coll_dict_rehash(JSContext *ctx, JSObject *p)
{
    QPDict *d = p->u.opaque;
    qp_dictkeys *keys;
    Py_hash_t *hashes;
    Py_ssize_t i, n;
    uint64_t version, keys_gen;
    BOOL changed = FALSE;
    JSValue obj;
    int ret = -1;

    if (!d || d->core.keys == QP_SNAP_COLL_EMPTY_KEYS || d->core.used == 0)
        return 0;
    keys = d->core.keys;
    n = keys->nentries;
    hashes = js_malloc(ctx, sizeof(hashes[0]) * n);
    if (!hashes)
        return -1;
    /* __hash__ code may drop the last reference to the dict */
    obj = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    version = d->core.version;
    keys_gen = d->core.keys_gen;
    for (i = 0; i < n; i++) {
        JSValue key;
        Py_hash_t h;
        if (!QP_DICTENTRY_ACTIVE(&keys->entries[i]))
            continue;
        key = JS_DupValue(ctx, keys->entries[i].key);
        h = qp_hash(ctx, key);
        JS_FreeValue(ctx, key);
        if (h == -1)
            goto done;
        if (d->core.keys != keys || d->core.keys_gen != keys_gen ||
            d->core.version != version) {
            qp_raise(ctx, QP_EXC(ctx, RuntimeError),
                     "dict mutated during snapshot restore");
            goto done;
        }
        hashes[i] = h;
        if (h != keys->entries[i].hash)
            changed = TRUE;
    }
    if (changed) {
        memset(keys->indices, 0xff, (size_t)1 << keys->log2_index_bytes);
        for (i = 0; i < n; i++) {
            qp_dictentry *ep = &keys->entries[i];
            if (!QP_DICTENTRY_ACTIVE(ep))
                continue;
            ep->hash = hashes[i];
            qp_dc_set_index(keys, qp_dc_find_empty_slot(keys, ep->hash), i);
        }
        d->core.version++;
    }
    ret = 0;
 done:
    JS_FreeValue(ctx, obj);
    js_free(ctx, hashes);
    return ret;
}

static int qp_snap_coll_read_dict(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    QPDict *d;
    qp_dictkeys *keys;
    uint8_t log2_size, unicode;
    Py_ssize_t usable, nentries, used, i, nactive, nindexed;
    int64_t version, uid;
    size_t size, slot;
    uint8_t *seen = NULL;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    d = js_malloc(ctx, sizeof(*d));
    if (!d)
        return -1;
    qp_dictcore_init(&d->core);
    d->uid = 0;
    d->hash = -1;
    p->u.opaque = d;
    snap_payload_ready(r);

    if (bc_get_u8(&r->bc, &log2_size))
        return -1;
    usable = nentries = 0;
    unicode = 1;
    if (log2_size != 0) {
        /* qp_dc_calculate_log2_keysize(0) is PyDict_LOG_MINSIZE */
        if (log2_size < qp_dc_calculate_log2_keysize(0) ||
            log2_size > QP_SNAP_COLL_DICT_MAX_LOG2)
            return snap_error(r, "invalid dict table size");
        if (bc_get_u8(&r->bc, &unicode) ||
            qp_snap_coll_get_ssize(r, &usable) ||
            qp_snap_coll_get_ssize(r, &nentries))
            return -1;
        if (unicode > 1 || nentries < 0 || usable < 0 ||
            nentries > qp_snap_coll_usable_fraction(log2_size) ||
            usable > qp_snap_coll_usable_fraction(log2_size) - nentries)
            return snap_error(r, "invalid dict table");
    }
    if (qp_snap_coll_get_ssize(r, &used) ||
        qp_snap_get_i64(r, &version) ||
        qp_snap_get_i64(r, &uid))
        return -1;
    d->core.version = (uint64_t)version;
    d->uid = (uint64_t)uid;
    if (log2_size == 0) {
        if (used != 0)
            return snap_error(r, "invalid dict size");
        return 0;
    }
    if (used < 0 || used > nentries)
        return snap_error(r, "invalid dict size");
    size = (size_t)1 << log2_size;
    if (size + (size_t)nentries > qp_snap_coll_left(r))
        return snap_error(r, "invalid dict table");

    keys = qp_dc_new_keys(ctx, log2_size, unicode);
    if (!keys)
        return -1;
    /* entries are released up to nentries: grow it as entries are read */
    keys->nentries = 0;
    d->core.keys = keys;
    d->core.keys_gen++;

    nindexed = 0;
    for (slot = 0; slot < size; slot++) {
        int32_t ix;
        if (bc_get_sleb128(&r->bc, &ix))
            return -1;
        if (ix < QP_DKIX_DUMMY || ix >= nentries)
            return snap_error(r, "invalid dict index");
        if (ix >= 0)
            nindexed++;
        qp_dc_set_index(keys, slot, ix);
    }
    nactive = 0;
    for (i = 0; i < nentries; i++) {
        qp_dictentry *ep = &keys->entries[i];
        int64_t hash;
        ep->key = JS_UNINITIALIZED;
        ep->value = JS_UNINITIALIZED;
        ep->hash = 0;
        keys->nentries = i + 1;
        if (snap_get_value(r, &ep->key) ||
            snap_get_value(r, &ep->value) ||
            qp_snap_get_i64(r, &hash))
            return -1;
        ep->hash = (Py_hash_t)hash;
        if (qp_snap_coll_is_null(ep->key) != qp_snap_coll_is_null(ep->value))
            return snap_error(r, "invalid dict entry");
        if (QP_DICTENTRY_ACTIVE(ep)) {
            /* the unicode kind compares keys as flat strings */
            if (unicode && JS_VALUE_GET_TAG(ep->key) != JS_TAG_STRING)
                return snap_error(r, "invalid dict entry");
            nactive++;
        }
    }
    keys->usable = usable;
    if (nactive != used || nindexed != used)
        return snap_error(r, "invalid dict table");
    /* every index must name a distinct live entry */
    if (nentries > 0) {
        seen = js_mallocz(ctx, (size_t)nentries);
        if (!seen)
            return -1;
        for (slot = 0; slot < size; slot++) {
            Py_ssize_t ix = qp_dc_get_index(keys, slot);
            if (ix < 0)
                continue;
            if (seen[ix] || !QP_DICTENTRY_ACTIVE(&keys->entries[ix])) {
                js_free(ctx, seen);
                return snap_error(r, "invalid dict index");
            }
            seen[ix] = 1;
        }
        js_free(ctx, seen);
    }
    d->core.used = used;
    if (used > 0)
        return qp_snap_defer(r, p, qp_snap_coll_dict_rehash);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* set / frozenset                                                           */
/* ------------------------------------------------------------------------ */

/* Largest set table written (1 << 30 slots). */
#define QP_SNAP_COLL_SET_MAX_SIZE ((Py_ssize_t)1 << 30)

enum {
    QP_SNAP_COLL_SLOT_UNUSED = 0,
    QP_SNAP_COLL_SLOT_DUMMY = 1,
    QP_SNAP_COLL_SLOT_ACTIVE = 2,
};

static int qp_snap_coll_write_set(SnapWriter *w, JSObject *p)
{
    QPSet *s = p->u.opaque;
    const qp_setcore *sc;
    Py_ssize_t i;

    qp_snap_coll_put_present(w, s);
    if (!s)
        return 0;
    sc = &s->core;
    if (sc->mask + 1 > QP_SNAP_COLL_SET_MAX_SIZE)
        return qp_snap_reject(w, p, "set table too large");
    if (qp_snap_put_i64(w, sc->mask) ||
        qp_snap_put_i64(w, sc->fill) ||
        qp_snap_put_i64(w, sc->used) ||
        qp_snap_put_i64(w, sc->finger))
        return -1;
    for (i = 0; i <= sc->mask; i++) {
        const qp_setentry *e = &sc->table[i];
        if (qp_snap_coll_is_null(e->key)) {
            bc_put_u8(&w->bc, QP_SNAP_COLL_SLOT_UNUSED);
        } else if (!QP_SETENTRY_ACTIVE(e)) {
            bc_put_u8(&w->bc, QP_SNAP_COLL_SLOT_DUMMY);
        } else {
            bc_put_u8(&w->bc, QP_SNAP_COLL_SLOT_ACTIVE);
            if (snap_put_value(w, e->key) || qp_snap_put_i64(w, e->hash))
                return -1;
        }
    }
    return 0;
}

/* Deferred: re-validate the stored hashes of a restored set (see the file
   comment).  Rebuilds the table at the same size only if a hash changed. */
static int qp_snap_coll_set_rehash(JSContext *ctx, JSObject *p)
{
    QPSet *s = p->u.opaque;
    qp_setcore *sc;
    qp_setentry *table, *oldtable, *newtable;
    qp_setentry small_copy[QP_SET_MINSIZE];
    Py_hash_t *hashes;
    Py_ssize_t i, mask, used, fill;
    uint64_t table_gen;
    BOOL changed = FALSE;
    JSValue obj;
    int ret = -1;

    if (!s || s->core.used == 0)
        return 0;
    sc = &s->core;
    table = sc->table;
    mask = sc->mask;
    used = sc->used;
    fill = sc->fill;
    table_gen = sc->table_gen;
    hashes = js_malloc(ctx, sizeof(hashes[0]) * (mask + 1));
    if (!hashes)
        return -1;
    /* __hash__ code may drop the last reference to the set */
    obj = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    for (i = 0; i <= mask; i++) {
        JSValue key;
        Py_hash_t h;
        if (!QP_SETENTRY_ACTIVE(&table[i]))
            continue;
        key = JS_DupValue(ctx, table[i].key);
        h = qp_hash(ctx, key);
        JS_FreeValue(ctx, key);
        if (h == -1)
            goto done;
        if (sc->table != table || sc->table_gen != table_gen ||
            sc->mask != mask || sc->used != used || sc->fill != fill) {
            qp_raise(ctx, QP_EXC(ctx, RuntimeError),
                     "set mutated during snapshot restore");
            goto done;
        }
        hashes[i] = h;
        if (h != table[i].hash)
            changed = TRUE;
    }
    if (changed) {
        if (table == sc->smalltable) {
            /* mask is QP_SET_MINSIZE - 1: rebuild the inline table in place */
            memcpy(small_copy, table, sizeof(small_copy));
            oldtable = small_copy;
            newtable = table;
        } else {
            newtable = js_malloc(ctx, sizeof(newtable[0]) * (mask + 1));
            if (!newtable)
                goto done;
            oldtable = table;
        }
        qp_sc_zero_table(newtable, (size_t)mask + 1);
        for (i = 0; i <= mask; i++) {
            if (QP_SETENTRY_ACTIVE(&oldtable[i]))
                qp_sc_insert_clean(newtable, (size_t)mask, oldtable[i].key,
                                   hashes[i]);
        }
        sc->fill = sc->used;
        if (newtable != table) {
            qp_sc_set_table(sc, newtable);
            js_free(ctx, oldtable);
        } else {
            sc->table_gen++;
        }
    }
    ret = 0;
 done:
    JS_FreeValue(ctx, obj);
    js_free(ctx, hashes);
    return ret;
}

static int qp_snap_coll_read_set(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    QPSet *s;
    qp_setcore *sc;
    Py_ssize_t mask, fill, used, finger, i, nfill;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    s = js_malloc(ctx, sizeof(*s));
    if (!s)
        return -1;
    qp_setcore_init(&s->core);
    s->hash = -1;
    p->u.opaque = s;
    snap_payload_ready(r);
    sc = &s->core;

    if (qp_snap_coll_get_ssize(r, &mask) ||
        qp_snap_coll_get_ssize(r, &fill) ||
        qp_snap_coll_get_ssize(r, &used) ||
        qp_snap_coll_get_ssize(r, &finger))
        return -1;
    /* power-of-two table >= QP_SET_MINSIZE; a failing lookup needs at least
       one unused slot */
    if (mask < QP_SET_MINSIZE - 1 || mask + 1 > QP_SNAP_COLL_SET_MAX_SIZE ||
        ((mask + 1) & mask) != 0 ||
        used < 0 || fill < used || fill > mask ||
        (size_t)mask + 1 > qp_snap_coll_left(r))
        return snap_error(r, "invalid set table");
    if (mask != QP_SET_MINSIZE - 1) {
        qp_setentry *table = js_malloc(ctx, sizeof(table[0]) * (mask + 1));
        if (!table)
            return -1;
        qp_sc_zero_table(table, (size_t)mask + 1);
        sc->table = table;
        sc->mask = mask;
    }
    /* the finalizer releases the first sc->used active slots: count them as
       they are stored */
    nfill = 0;
    for (i = 0; i <= mask; i++) {
        qp_setentry *e = &sc->table[i];
        JSValue key;
        uint8_t tag;
        int64_t hash;
        if (bc_get_u8(&r->bc, &tag))
            return -1;
        switch (tag) {
        case QP_SNAP_COLL_SLOT_UNUSED:
            break;
        case QP_SNAP_COLL_SLOT_DUMMY:
            e->key = QP_SETCORE_DUMMY;
            e->hash = -1;
            nfill++;
            break;
        case QP_SNAP_COLL_SLOT_ACTIVE:
            if (snap_get_value(r, &key))
                return -1;
            if (qp_snap_coll_is_null(key))
                return snap_error(r, "invalid set entry");
            e->key = key;
            sc->used++;
            nfill++;
            if (qp_snap_get_i64(r, &hash))
                return -1;
            e->hash = (Py_hash_t)hash;
            break;
        default:
            return snap_error(r, "invalid set entry");
        }
    }
    if (sc->used != used || nfill != fill)
        return snap_error(r, "invalid set table");
    sc->fill = fill;
    sc->finger = finger;    /* always masked by pop() */
    if (used > 0)
        return qp_snap_defer(r, p, qp_snap_coll_set_rehash);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* dict / set iterators, dict views                                          */
/* ------------------------------------------------------------------------ */

static int qp_snap_coll_write_dictiter(SnapWriter *w, JSObject *p)
{
    void *payload = p->u.opaque;

    qp_snap_coll_put_present(w, payload);
    if (!payload)
        return 0;
    if (*(uint8_t *)payload == QP_DICTITER_KIND_SET) {
        QPSetIter *si = payload;
        bc_put_u8(&w->bc, QP_DICTITER_KIND_SET);
        if (snap_put_value(w, si->set) ||
            qp_snap_put_i64(w, si->st.used) ||
            qp_snap_put_i64(w, si->st.pos) ||
            qp_snap_put_i64(w, si->st.len))
            return -1;
    } else {
        QPDictIter *it = payload;
        bc_put_u8(&w->bc, it->kind);
        bc_put_u8(&w->bc, it->reversed);
        if (snap_put_value(w, it->dict) ||
            qp_snap_put_i64(w, it->st.used) ||
            qp_snap_put_i64(w, it->st.pos) ||
            qp_snap_put_i64(w, it->st.len))
            return -1;
    }
    return 0;
}

static int qp_snap_coll_read_dictiter(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    uint8_t kind, reversed;
    qp_dictcore_iter st;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    if (bc_get_u8(&r->bc, &kind))
        return -1;
    if (kind == QP_DICTITER_KIND_SET) {
        QPSetIter *si = js_malloc(ctx, sizeof(*si));
        if (!si)
            return -1;
        si->kind = QP_DICTITER_KIND_SET;
        si->set = QP_NULL;
        si->st.used = si->st.pos = si->st.len = 0;
        p->u.opaque = si;
        snap_payload_ready(r);
        if (snap_get_value(r, &si->set) ||
            qp_snap_coll_get_ssize(r, &si->st.used) ||
            qp_snap_coll_get_ssize(r, &si->st.pos) ||
            qp_snap_coll_get_ssize(r, &si->st.len))
            return -1;
        if (si->st.pos < 0)
            return snap_error(r, "invalid set iterator");
        return 0;
    }
    if (kind > QP_DV_ITEMS)
        return snap_error(r, "invalid dict iterator");
    if (bc_get_u8(&r->bc, &reversed))
        return -1;
    if (reversed > 1)
        return snap_error(r, "invalid dict iterator");
    {
        QPDictIter *it = js_malloc(ctx, sizeof(*it));
        if (!it)
            return -1;
        it->kind = kind;
        it->reversed = reversed;
        it->dict = JS_UNDEFINED;
        it->st.used = it->st.pos = it->st.len = 0;
        p->u.opaque = it;
        snap_payload_ready(r);
        if (snap_get_value(r, &it->dict) ||
            qp_snap_coll_get_ssize(r, &st.used) ||
            qp_snap_coll_get_ssize(r, &st.pos) ||
            qp_snap_coll_get_ssize(r, &st.len))
            return -1;
        /* the forward scan indexes entries[pos] without a lower bound */
        if (!reversed && st.pos < 0)
            return snap_error(r, "invalid dict iterator");
        it->st = st;
    }
    return 0;
}

static int qp_snap_coll_write_dictview(SnapWriter *w, JSObject *p)
{
    QPDictView *v = p->u.opaque;

    qp_snap_coll_put_present(w, v);
    if (!v)
        return 0;
    bc_put_u8(&w->bc, v->kind);
    return snap_put_value(w, v->mapping);
}

static int qp_snap_coll_read_dictview(SnapReader *r, JSObject *p)
{
    QPDictView *v;
    uint8_t kind;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    if (bc_get_u8(&r->bc, &kind))
        return -1;
    if (kind > QP_DV_PROXY)
        return snap_error(r, "invalid dict view");
    v = js_malloc(r->ctx, sizeof(*v));
    if (!v)
        return -1;
    v->kind = kind;
    v->mapping = JS_UNDEFINED;
    p->u.opaque = v;
    snap_payload_ready(r);
    return snap_get_value(r, &v->mapping);
}

/* ------------------------------------------------------------------------ */
/* tuple, range, slice, sequence iterators, complex                          */
/* ------------------------------------------------------------------------ */

static int qp_snap_coll_write_tuple(SnapWriter *w, JSObject *p)
{
    QPTuple *t = p->u.opaque;
    uint32_t i;

    qp_snap_coll_put_present(w, t);
    if (!t)
        return 0;
    bc_put_leb128(&w->bc, t->len);
    for (i = 0; i < t->len; i++) {
        if (snap_put_value(w, t->items[i]))
            return -1;
    }
    return 0;
}

static int qp_snap_coll_read_tuple(SnapReader *r, JSObject *p)
{
    QPTuple *t;
    uint32_t len, i;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    if (bc_get_leb128(&r->bc, &len))
        return -1;
    if (len > QP_TUPLE_MAX_SIZE || len > qp_snap_coll_left(r))
        return snap_error(r, "invalid tuple size");
    t = js_malloc(r->ctx, offsetof(QPTuple, items) + sizeof(JSValue) * len);
    if (!t)
        return -1;
    t->hash = -1;
    t->len = 0;     /* the finalizer releases items[0 .. len) */
    p->u.opaque = t;
    snap_payload_ready(r);
    for (i = 0; i < len; i++) {
        if (snap_get_value(r, &t->items[i]))
            return -1;
        t->len = i + 1;
    }
    return 0;
}

static int qp_snap_coll_write_range(SnapWriter *w, JSObject *p)
{
    QPRange *rg = p->u.opaque;

    qp_snap_coll_put_present(w, rg);
    if (!rg)
        return 0;
    if (snap_put_value(w, rg->start) ||
        snap_put_value(w, rg->stop) ||
        snap_put_value(w, rg->step) ||
        snap_put_value(w, rg->length))
        return -1;
    return 0;
}

static int qp_snap_coll_read_range(SnapReader *r, JSObject *p)
{
    QPRange *rg;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    rg = js_malloc(r->ctx, sizeof(*rg));
    if (!rg)
        return -1;
    rg->start = rg->stop = rg->step = rg->length = JS_UNDEFINED;
    p->u.opaque = rg;
    snap_payload_ready(r);
    if (snap_get_value(r, &rg->start) ||
        snap_get_value(r, &rg->stop) ||
        snap_get_value(r, &rg->step) ||
        snap_get_value(r, &rg->length))
        return -1;
    return 0;
}

static int qp_snap_coll_write_slice(SnapWriter *w, JSObject *p)
{
    QPSlice *sl = p->u.opaque;

    qp_snap_coll_put_present(w, sl);
    if (!sl)
        return 0;
    if (snap_put_value(w, sl->start) ||
        snap_put_value(w, sl->stop) ||
        snap_put_value(w, sl->step))
        return -1;
    return 0;
}

static int qp_snap_coll_read_slice(SnapReader *r, JSObject *p)
{
    QPSlice *sl;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    sl = js_malloc(r->ctx, sizeof(*sl));
    if (!sl)
        return -1;
    sl->start = sl->stop = sl->step = JS_UNDEFINED;
    p->u.opaque = sl;
    snap_payload_ready(r);
    if (snap_get_value(r, &sl->start) ||
        snap_get_value(r, &sl->stop) ||
        snap_get_value(r, &sl->step))
        return -1;
    return 0;
}

/* Every iterator type of iterobject.c (seqiter, list/tuple/str/bytes/
   bytearray iterators and their reversed forms, callable_iterator,
   range_iterator, longrange_iterator) shares QPIterObj; the Python type is
   the prototype, so writing every field covers all kinds, positions
   included (str iterators: code-point index + UTF-16 offset into the same
   immutable string). */
static int qp_snap_coll_write_iter(SnapWriter *w, JSObject *p)
{
    QPIterObj *it = p->u.opaque;

    qp_snap_coll_put_present(w, it);
    if (!it)
        return 0;
    if (snap_put_value(w, it->seq) ||
        snap_put_value(w, it->aux) ||
        snap_put_value(w, it->aux2) ||
        qp_snap_put_i64(w, it->index) ||
        qp_snap_put_i64(w, it->pos) ||
        qp_snap_put_i64(w, it->start) ||
        qp_snap_put_i64(w, it->step) ||
        qp_snap_put_i64(w, it->len))
        return -1;
    return 0;
}

static int qp_snap_coll_read_iter(SnapReader *r, JSObject *p)
{
    QPIterObj *it;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    it = js_mallocz(r->ctx, sizeof(*it));
    if (!it)
        return -1;
    it->seq = it->aux = it->aux2 = QP_NULL;
    p->u.opaque = it;
    snap_payload_ready(r);
    if (snap_get_value(r, &it->seq) ||
        snap_get_value(r, &it->aux) ||
        snap_get_value(r, &it->aux2) ||
        qp_snap_coll_get_ssize(r, &it->index) ||
        qp_snap_coll_get_ssize(r, &it->pos) ||
        qp_snap_get_i64(r, &it->start) ||
        qp_snap_get_i64(r, &it->step) ||
        qp_snap_get_i64(r, &it->len))
        return -1;
    return 0;
}

static int qp_snap_coll_write_complex(SnapWriter *w, JSObject *p)
{
    QPComplex *c = p->u.opaque;

    qp_snap_coll_put_present(w, c);
    if (!c)
        return 0;
    if (qp_snap_put_f64(w, c->real) || qp_snap_put_f64(w, c->imag))
        return -1;
    return 0;
}

static int qp_snap_coll_read_complex(SnapReader *r, JSObject *p)
{
    QPComplex *c;
    int ret;

    ret = qp_snap_coll_get_present(r);
    if (ret <= 0)
        return ret;
    c = js_mallocz(r->ctx, sizeof(*c));
    if (!c)
        return -1;
    p->u.opaque = c;
    snap_payload_ready(r);
    if (qp_snap_get_f64(r, &c->real) || qp_snap_get_f64(r, &c->imag))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------ */

static int qp_snap_write_coll(SnapWriter *w, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_COMPLEX:
        return qp_snap_coll_write_complex(w, p);
    case JS_CLASS_PY_TUPLE:
        return qp_snap_coll_write_tuple(w, p);
    case JS_CLASS_PY_RANGE:
        return qp_snap_coll_write_range(w, p);
    case JS_CLASS_PY_SLICE:
        return qp_snap_coll_write_slice(w, p);
    case JS_CLASS_PY_ITER:
        return qp_snap_coll_write_iter(w, p);
    case JS_CLASS_PY_DICT:
    case JS_CLASS_PY_FROZENDICT:
        return qp_snap_coll_write_dict(w, p);
    case JS_CLASS_PY_SET:
    case JS_CLASS_PY_FROZENSET:
        return qp_snap_coll_write_set(w, p);
    case JS_CLASS_PY_DICT_ITER:
        return qp_snap_coll_write_dictiter(w, p);
    case JS_CLASS_PY_DICT_VIEW:
        return qp_snap_coll_write_dictview(w, p);
    default:
        return 1;
    }
}

static int qp_snap_read_coll(SnapReader *r, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_COMPLEX:
        return qp_snap_coll_read_complex(r, p);
    case JS_CLASS_PY_TUPLE:
        return qp_snap_coll_read_tuple(r, p);
    case JS_CLASS_PY_RANGE:
        return qp_snap_coll_read_range(r, p);
    case JS_CLASS_PY_SLICE:
        return qp_snap_coll_read_slice(r, p);
    case JS_CLASS_PY_ITER:
        return qp_snap_coll_read_iter(r, p);
    case JS_CLASS_PY_DICT:
    case JS_CLASS_PY_FROZENDICT:
        return qp_snap_coll_read_dict(r, p);
    case JS_CLASS_PY_SET:
    case JS_CLASS_PY_FROZENSET:
        return qp_snap_coll_read_set(r, p);
    case JS_CLASS_PY_DICT_ITER:
        return qp_snap_coll_read_dictiter(r, p);
    case JS_CLASS_PY_DICT_VIEW:
        return qp_snap_coll_read_dictview(r, p);
    default:
        return 1;
    }
}
