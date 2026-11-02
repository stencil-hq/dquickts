/*
 * dictcore.h - CPython's dict and set hash-table algorithms over JSValues.
 *
 * Derived from CPython Objects/dictobject.c and Objects/setobject.c
 * (PSF-2.0).
 *
 * This file is part of the single quickjs.c translation unit (included through
 * python/quickpy.c after python/compat.h), so every function is `static`.  It
 * only needs <quickjs.h>/quickjs.c internals (JSValue, JSString, js_malloc,
 * JS_MarkValue) and Py_ssize_t from python/compat.h.  It does NOT depend on the
 * Python object model: the caller supplies every hash (computed once, before
 * the call) and an equality callback.  The structs below are meant to be
 * embedded in the PY_DICT / PY_SET / PY_FROZENSET objects.
 *
 * What is ported (same table sizes, growth policy, probe sequences, entry
 * layout decisions and mutation-during-compare behaviour as CPython 3.15.2,
 * so that iteration order is identical for the same operations and hashes):
 *
 *  dict (qp_dictcore): compact ordered dict: an index table of 1/2/4/8-byte
 *    signed indices (DKIX_EMPTY / DKIX_DUMMY) plus an append-only entries
 *    array; USABLE_FRACTION 2/3, GROWTH_RATE used*3, PyDict_MINSIZE 8,
 *    perturbation probing, the "unicode keys" table kind (every key an exact
 *    str) and its conversion to the general kind, the shared immutable empty
 *    keys table, dictresize, insert_to_emptydict, insertdict, delitem (dummy
 *    index, entry cleared), popitem (LIFO, nentries shrinks), clear, copy
 *    (clone fast path), dict_dict_merge, setdefault, pop, fromkeys presizing,
 *    dict equality, frozendict hash, and iteration by entry index.
 *    Split tables (instance __dict__ sharing keys) are not ported: they never
 *    change iteration order, only memory use.
 *
 *  set (qp_setcore): open addressing with LINEAR_PROBES 9 + perturbation,
 *    dummy entries, the 8-entry inline smalltable, fill/used accounting,
 *    set_table_resize growth (used*4, or used*2 above 50000), set_add_entry,
 *    set_discard_entry, set_merge and the dict/set fast paths of update,
 *    intersection, difference, symmetric difference, issubset, isdisjoint,
 *    pop (search finger), clear, swap_bodies and frozenset_hash.
 *
 * ---------------------------------------------------------------------------
 * Keys, identity and equality
 *
 *  - "exact str" means a flat JSString value (JS_TAG_STRING).  Callers must
 *    flatten ropes (JS_TAG_STRING_ROPE) before using a string as a key; a rope
 *    would be treated as a non-str object (compared through the callback and
 *    switching a dict to the general kind).
 *  - Identity (CPython's `ep->me_key == key`) is "same JSValue": same
 *    normalized tag and same payload (same pointer for reference-counted
 *    values, same bits for float64, same integer for int/bool/short bigint/
 *    null/undefined).  Two NaN floats with the same bits are therefore
 *    identical keys.
 *  - Two exact strs are compared by content (hash first, then characters)
 *    without calling back, as CPython's unicode_eq fast paths do.  This can
 *    never run user code.
 *  - Everything else goes to the equality callback, called as
 *    eq(ctx, stored_key, lookup_key, opaque) exactly where CPython calls
 *    PyObject_RichCompareBool(startkey, key, Py_EQ).  It must implement
 *    PyObject_RichCompareBool(a, b, Py_EQ) (including its identity shortcut)
 *    and return 1 (equal), 0 (not equal) or -1 (exception pending on ctx).
 *    The callback may run arbitrary code, including code that mutates or
 *    clears the very table being searched; the core handles that exactly like
 *    CPython (dict: restart the lookup if the keys table was replaced or the
 *    compared entry changed; set: restart if the table was reallocated or the
 *    entry changed).
 *  - Hashes are Py_hash_t values that are never -1 (CPython reserves -1 for
 *    errors; callers map -1 to -2 as CPython's hash functions do).
 *
 * Lifetime rule: every function that takes an `eq` callback may run arbitrary
 * code.  The caller must keep the object that embeds the qp_dictcore /
 * qp_setcore alive (hold a reference) for the duration of the call, exactly as
 * CPython callers hold a reference to the dict/set.  The core keeps its own
 * temporary references to the keys it compares, so a callback that deletes
 * entries is safe.
 *
 * Reference counting: the core owns one reference to every stored key and
 * value.  Functions named *_take consume the references passed to them (also
 * on failure); all other functions borrow their JSValue arguments and
 * JS_DupValue what they store.  Values handed back through `JSValue *` out
 * parameters are BORROWED (valid until the next mutation of the table or any
 * call that can run user code) unless the comment says "owned".
 *
 * Errors: -1 means an exception is pending on ctx (thrown by the callback, or
 * JS_ThrowOutOfMemory from js_malloc).  Python-level errors that CPython raises
 * itself (KeyError, RuntimeError "... changed size during iteration", "dict
 * mutated during update", ...) are NOT thrown here; they are reported through
 * distinct return codes documented per function, and the caller raises them
 * with CPython's message.
 */
#ifndef QP_DICTCORE_H
#define QP_DICTCORE_H

#ifndef QP_PY_HASH_T_DEFINED
#define QP_PY_HASH_T_DEFINED
typedef intptr_t Py_hash_t;
typedef uintptr_t Py_uhash_t;
#endif

/* Equality: 1 equal, 0 not equal, -1 exception pending.  See header comment
 * for the call convention (stored key first). */
typedef int qp_dictcore_eq_fn(JSContext *ctx, JSValueConst a, JSValueConst b,
                              void *opaque);
/* Hash of a value (frozendict hash only): the hash, or -1 with an exception
 * pending. */
typedef Py_hash_t qp_dictcore_hash_fn(JSContext *ctx, JSValueConst v,
                                      void *opaque);

/* ========================================================================= */
/* dict                                                                      */
/* ========================================================================= */

/* Index table values (CPython DKIX_*) and lookup results. */
#define QP_DKIX_EMPTY (-1)   /* slot never used / key not found */
#define QP_DKIX_DUMMY (-2)   /* slot of a deleted entry */
#define QP_DKIX_ERROR (-3)   /* comparison raised (exception pending) */

/* One entry of the entries array.  A deleted entry (CPython: me_key ==
 * me_value == NULL) holds JS_UNINITIALIZED in both key and value; Python
 * values are never JS_UNINITIALIZED. */
typedef struct qp_dictentry {
    JSValue key;
    JSValue value;
    Py_hash_t hash;     /* the key's hash (stored for both table kinds) */
} qp_dictentry;

#define QP_DICTENTRY_ACTIVE(ep) \
    (JS_VALUE_GET_TAG((ep)->value) != JS_TAG_UNINITIALIZED)

/* CPython PyDictKeysObject (combined tables only).  One js_malloc block:
 * header, then 1 << log2_index_bytes bytes of indices, then
 * USABLE_FRACTION(size) entries.  The shared empty table (log2_size 0) is a
 * static object that is never written. */
typedef struct qp_dictkeys {
    uint8_t log2_size;          /* table size is 1 << log2_size */
    uint8_t log2_index_bytes;   /* index table byte size is 1 << this */
    uint8_t unicode;            /* 1 = DICT_KEYS_UNICODE: all keys exact str */
    Py_ssize_t usable;          /* entries still insertable without resize */
    Py_ssize_t nentries;        /* entries used so far (incl. deleted ones) */
    void *indices;              /* int8/16/32/64_t[1 << log2_size] */
    qp_dictentry *entries;      /* qp_dictentry[USABLE_FRACTION(size)] */
} qp_dictkeys;

/* The dict proper (CPython PyDictObject minus the object header). */
typedef struct qp_dictcore {
    qp_dictkeys *keys;          /* never NULL; the shared empty table if empty */
    Py_ssize_t used;            /* number of live items (len(d)) */
    /* Incremented by every mutation that changes a key, a value, the set of
     * entries or entry positions (insert, value replacement by a different
     * value, delete, pop, popitem, clear, resize/compaction, copy/merge).
     * Never decremented; 0 only for a never-mutated dict.  Caches keyed on
     * (dict, version) stay valid while it is unchanged. */
    uint64_t version;
    /* Incremented whenever `keys` is replaced (lookup restart detection). */
    uint64_t keys_gen;
} qp_dictcore;

/* Iterator state (CPython dictiterobject minus the dict pointer/result). */
typedef struct qp_dictcore_iter {
    Py_ssize_t used;    /* d->used when the iterator was created (-1: sticky error) */
    Py_ssize_t pos;     /* next entry index (reversed: current index, < 0 = done) */
    Py_ssize_t len;     /* items still expected */
} qp_dictcore_iter;

/* qp_dictcore_iter_next results besides 1 (item) / 0 (exhausted). */
#define QP_DICTITER_CHANGED_SIZE (-1) /* RuntimeError("dictionary changed size during iteration");
                                         the state is now sticky; CPython keeps the dict reference */
#define QP_DICTITER_KEYS_CHANGED (-2) /* RuntimeError("dictionary keys changed during iteration");
                                         CPython drops the dict reference (iterator exhausted) */

/* qp_dictcore_merge / qp_dictcore_copy results besides 0 / -1. */
#define QP_DICT_MERGE_DUP     (-2)  /* duplicate key with override == 2; *dupkey set (owned) */
#define QP_DICT_MERGE_MUTATED (-3)  /* RuntimeError("dict mutated during update"); nothing thrown */

/* Initialize an empty dict (shared empty keys table, no allocation). */
static __maybe_unused void qp_dictcore_init(qp_dictcore *d);
/* CPython dict_new_presized(minused, unicode): empty dict whose table can hold
 * about `minused` items (capped at 2**17 slots).  0 or -1 (OOM). */
static __maybe_unused int qp_dictcore_init_presized(JSContext *ctx, qp_dictcore *d,
                                     Py_ssize_t minused, int unicode);
/* Release every key/value reference and the table (dict_dealloc); leaves an
 * empty dict.  For finalizers. */
static __maybe_unused void qp_dictcore_free(JSRuntime *rt, qp_dictcore *d);
/* GC: mark every live key and value (dict_traverse). */
static __maybe_unused void qp_dictcore_mark(JSRuntime *rt, const qp_dictcore *d,
                             JS_MarkFunc *mark_func);

/* CPython _Py_dict_lookup.  Returns the entry index (>= 0) and the borrowed
 * value in *pvalue (if pvalue != NULL; JS_UNINITIALIZED when not found),
 * QP_DKIX_EMPTY if absent, QP_DKIX_ERROR (-3) if eq raised. */
static __maybe_unused Py_ssize_t qp_dictcore_lookup(JSContext *ctx, qp_dictcore *d,
                                     JSValueConst key, Py_hash_t hash,
                                     qp_dictcore_eq_fn *eq, void *opaque,
                                     JSValue *pvalue);
/* 1 if present, 0 if absent, -1 on error (_PyDict_Contains_KnownHash). */
static __maybe_unused int qp_dictcore_contains(JSContext *ctx, qp_dictcore *d,
                                JSValueConst key, Py_hash_t hash,
                                qp_dictcore_eq_fn *eq, void *opaque);
/* d[key] = value (setitem_take2_lock_held_known_hash).  Consumes key and
 * value (also on failure).  When the key exists the stored key is kept and
 * only the value is replaced.  0 or -1. */
static __maybe_unused int qp_dictcore_setitem_take(JSContext *ctx, qp_dictcore *d,
                                    JSValue key, Py_hash_t hash, JSValue value,
                                    qp_dictcore_eq_fn *eq, void *opaque);
/* Same, borrowing key and value. */
static __maybe_unused int qp_dictcore_setitem(JSContext *ctx, qp_dictcore *d,
                               JSValueConst key, Py_hash_t hash,
                               JSValueConst value,
                               qp_dictcore_eq_fn *eq, void *opaque);
/* dict.setdefault (dict_setdefault_ref_lock_held): borrows key and
 * default_value.  Returns 0 if inserted, 1 if already present, -1 on error;
 * *presult (if non-NULL) receives the borrowed stored value. */
static __maybe_unused int qp_dictcore_setdefault(JSContext *ctx, qp_dictcore *d,
                                  JSValueConst key, Py_hash_t hash,
                                  JSValueConst default_value,
                                  qp_dictcore_eq_fn *eq, void *opaque,
                                  JSValue *presult);
/* del d[key] (_PyDict_DelItem_KnownHash): 1 deleted, 0 not found (caller
 * raises KeyError(key)), -1 on error. */
static __maybe_unused int qp_dictcore_delitem(JSContext *ctx, qp_dictcore *d,
                               JSValueConst key, Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque);
/* Delete the live entry `ix` (delitem_common): index slot becomes DUMMY,
 * entry cleared, references released (may run finalizers). */
static __maybe_unused void qp_dictcore_delitem_ix(JSContext *ctx, qp_dictcore *d,
                                   Py_ssize_t ix);
/* dict.pop core (_PyDict_Pop_KnownHash): 1 found (value removed, *presult
 * OWNED), 0 not found (no comparison at all when the dict is empty), -1 on
 * error.  presult may be NULL. */
static __maybe_unused int qp_dictcore_pop(JSContext *ctx, qp_dictcore *d, JSValueConst key,
                           Py_hash_t hash, qp_dictcore_eq_fn *eq, void *opaque,
                           JSValue *presult);
/* dict.popitem: removes the last item (LIFO).  1 with *pkey and *pvalue OWNED,
 * 0 if empty (caller raises KeyError("popitem(): dictionary is empty")). */
static __maybe_unused int qp_dictcore_popitem(JSContext *ctx, qp_dictcore *d,
                               JSValue *pkey, JSValue *pvalue);
/* dict.clear (cannot fail; releases references after detaching the table). */
static __maybe_unused void qp_dictcore_clear(JSContext *ctx, qp_dictcore *d);

/* PyDict_Next: advance *ppos (start at 0) to the next live entry; returns 1
 * and its index in *pix, or 0 at the end.  Re-reads the table every call. */
static __maybe_unused int qp_dictcore_next(const qp_dictcore *d, Py_ssize_t *ppos,
                            Py_ssize_t *pix);
/* dictiter_new state (reversed != 0 for reversed(d)). */
static __maybe_unused void qp_dictcore_iter_init(const qp_dictcore *d, qp_dictcore_iter *it,
                                  int reversed);
/* dictiter_iternext* / dictreviter_iter: 1 with the entry index in *pix,
 * 0 exhausted (CPython drops the dict reference), or QP_DICTITER_* codes. */
static __maybe_unused int qp_dictcore_iter_next(const qp_dictcore *d, qp_dictcore_iter *it,
                                 int reversed, Py_ssize_t *pix);
/* __length_hint__ (dictiter_len) for an iterator whose dict is still set. */
static __maybe_unused Py_ssize_t qp_dictcore_iter_length_hint(const qp_dictcore *d,
                                               const qp_dictcore_iter *it);

/* dict_dict_merge(mp, other, override, dupkey): merge all items of `other`
 * into `d`.  override 1: d[k] = v; 0: keep existing keys; 2: duplicate key
 * is an error: returns QP_DICT_MERGE_DUP with *dupkey OWNED (dupkey must be
 * non-NULL iff override == 2).  Returns 0, -1 (exception), QP_DICT_MERGE_DUP
 * or QP_DICT_MERGE_MUTATED (other changed size while merging; caller raises
 * RuntimeError("dict mutated during update")).  The caller handles the
 * non-dict / dict-subclass-with-__iter__ paths of dict_merge. */
static __maybe_unused int qp_dictcore_merge(JSContext *ctx, qp_dictcore *d,
                             qp_dictcore *other, int override,
                             JSValue *dupkey,
                             qp_dictcore_eq_fn *eq, void *opaque);
/* dict.copy core (copy_lock_held_untracked for combined dicts whose type does
 * not override __iter__): `dst` must be a freshly initialized empty dict.
 * Clones the table when it is compact enough, else merges (compacting).
 * Returns 0, -1 or QP_DICT_MERGE_MUTATED. */
static __maybe_unused int qp_dictcore_copy(JSContext *ctx, qp_dictcore *dst, qp_dictcore *src,
                            qp_dictcore_eq_fn *eq, void *opaque);
/* dict_dict_fromkeys / dict_set_fromkeys presizing: resize `d` to hold `n`
 * keys (never shrinking below the current table).  unicode: the source dict's
 * table kind (keys->unicode) for a dict source, 0 for a set source.  The
 * caller then inserts each key with qp_dictcore_setitem.  0 or -1. */
static __maybe_unused int qp_dictcore_fromkeys_resize(JSContext *ctx, qp_dictcore *d,
                                       Py_ssize_t n, int unicode);
/* dict_equal: 1 equal, 0 not equal, -1 on error.  Values are compared with
 * eq(a_value, b_value). */
static __maybe_unused int qp_dictcore_equal(JSContext *ctx, qp_dictcore *a, qp_dictcore *b,
                             qp_dictcore_eq_fn *eq, void *opaque);
/* frozendict_hash without the cache and the recursion guard (the caller
 * caches in its object and wraps the call in its recursion check).  Returns
 * the hash or -1 if value_hash failed. */
static __maybe_unused Py_hash_t qp_dictcore_frozendict_hash(JSContext *ctx, qp_dictcore *d,
                                             qp_dictcore_hash_fn *value_hash,
                                             void *opaque);

static inline Py_ssize_t qp_dictcore_size(const qp_dictcore *d)
{
    return d->used;
}

/* ========================================================================= */
/* set / frozenset                                                           */
/* ========================================================================= */

#define QP_SET_MINSIZE 8    /* PySet_MINSIZE */

/* One table slot.  Unused: key JS_UNINITIALIZED, hash 0.  Dummy (deleted):
 * key QP_SETCORE_DUMMY, hash -1.  Active: any other key. */
typedef struct qp_setentry {
    JSValue key;
    Py_hash_t hash;
} qp_setentry;

/* Never a Python value: an internal QuickJS stack tag. */
#define QP_SETCORE_DUMMY JS_MKVAL(JS_TAG_CATCH_OFFSET, 0)
#define QP_SETENTRY_ACTIVE(e) \
    (JS_VALUE_GET_TAG((e)->key) != JS_TAG_UNINITIALIZED && \
     JS_VALUE_GET_TAG((e)->key) != JS_TAG_CATCH_OFFSET)

/* CPython PySetObject minus the object header and weakref list.  `table`
 * points into the struct itself (smalltable) for small sets, so a qp_setcore
 * must never be copied bitwise: use qp_setcore_swap_bodies. */
typedef struct qp_setcore {
    Py_ssize_t fill;            /* active + dummy slots */
    Py_ssize_t used;            /* active slots (len(s)) */
    Py_ssize_t mask;            /* table size - 1 */
    qp_setentry *table;         /* smalltable or a js_malloc'd block */
    uint64_t table_gen;         /* incremented whenever `table` changes */
    Py_hash_t hash;             /* frozenset hash cache, -1 if not computed */
    Py_ssize_t finger;          /* pop() search finger */
    qp_setentry smalltable[QP_SET_MINSIZE];
} qp_setcore;

/* Iterator state (CPython setiterobject minus the set pointer). */
typedef struct qp_setcore_iter {
    Py_ssize_t used;    /* s->used at creation (-1: sticky error) */
    Py_ssize_t pos;
    Py_ssize_t len;
} qp_setcore_iter;

/* qp_setcore_iter_next result besides 1 / 0: RuntimeError("Set changed size
 * during iteration"); the state is sticky, CPython keeps the set reference. */
#define QP_SETITER_CHANGED_SIZE (-1)

/* Empty set with the small table (make_new_set). */
static __maybe_unused void qp_setcore_init(qp_setcore *s);
/* Release references and the table (set_dealloc); leaves an empty set. */
static __maybe_unused void qp_setcore_free(JSRuntime *rt, qp_setcore *s);
/* GC: mark every active key (set_traverse). */
static __maybe_unused void qp_setcore_mark(JSRuntime *rt, const qp_setcore *s,
                            JS_MarkFunc *mark_func);

/* set_add_entry_takeref: consumes key (also on failure).  Adding an already
 * present key keeps the stored key.  0 or -1 (on -1 after the key was stored,
 * the resize that followed failed, as in CPython). */
static __maybe_unused int qp_setcore_add_take(JSContext *ctx, qp_setcore *s, JSValue key,
                               Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque);
/* set_add_entry: borrows key. */
static __maybe_unused int qp_setcore_add(JSContext *ctx, qp_setcore *s, JSValueConst key,
                          Py_hash_t hash, qp_dictcore_eq_fn *eq, void *opaque);
/* set_contains_entry: 1 / 0 / -1. */
static __maybe_unused int qp_setcore_contains(JSContext *ctx, qp_setcore *s, JSValueConst key,
                               Py_hash_t hash,
                               qp_dictcore_eq_fn *eq, void *opaque);
/* set_discard_entry: 1 found and removed (slot becomes dummy), 0 not found
 * (set.remove raises KeyError), -1 on error. */
static __maybe_unused int qp_setcore_discard(JSContext *ctx, qp_setcore *s, JSValueConst key,
                              Py_hash_t hash,
                              qp_dictcore_eq_fn *eq, void *opaque);
/* set_clear_internal (cannot fail). */
static __maybe_unused void qp_setcore_clear(JSContext *ctx, qp_setcore *s);
/* set.pop: 1 with *pkey OWNED, 0 if empty (KeyError("pop from an empty set")). */
static __maybe_unused int qp_setcore_pop(qp_setcore *s, JSValue *pkey);
/* set_next: advance *ppos (start at 0); 1 with the borrowed slot in *pentry,
 * 0 at the end. */
static __maybe_unused int qp_setcore_next(const qp_setcore *s, Py_ssize_t *ppos,
                           qp_setentry **pentry);
static __maybe_unused void qp_setcore_iter_init(const qp_setcore *s, qp_setcore_iter *it);
/* setiter_iternext: 1 with the borrowed key in *pkey, 0 exhausted (CPython
 * drops the set reference), QP_SETITER_CHANGED_SIZE. */
static __maybe_unused int qp_setcore_iter_next(const qp_setcore *s, qp_setcore_iter *it,
                                JSValue *pkey);
static __maybe_unused Py_ssize_t qp_setcore_iter_length_hint(const qp_setcore *s,
                                              const qp_setcore_iter *it);
/* set_table_resize(so, minused): rebuild into the smallest table with
 * size > minused (purging dummies).  0 or -1 (OOM). */
static __maybe_unused int qp_setcore_table_resize(JSContext *ctx, qp_setcore *s,
                                   Py_ssize_t minused);

/* set_merge_lock_held: s |= other (other is a set or frozenset). 0 / -1. */
static __maybe_unused int qp_setcore_merge(JSContext *ctx, qp_setcore *s, qp_setcore *other,
                            qp_dictcore_eq_fn *eq, void *opaque);
/* set_update_dict_lock_held: add every key of an exact dict/frozendict, using
 * the stored hashes. 0 / -1. */
static __maybe_unused int qp_setcore_update_dict(JSContext *ctx, qp_setcore *s,
                                  qp_dictcore *other,
                                  qp_dictcore_eq_fn *eq, void *opaque);
/* set_swap_bodies.  both_frozen: both objects are frozenset (sub)types (then
 * the cached hashes are swapped, else both reset to -1).  The pop finger is
 * not swapped (as in CPython). */
static __maybe_unused void qp_setcore_swap_bodies(qp_setcore *a, qp_setcore *b,
                                   int both_frozen);

/* set_intersection when `other` is a set/frozenset: fills `result` (a fresh
 * empty set).  so == other copies.  0 / -1 (result then holds a partial
 * result the caller frees). */
static __maybe_unused int qp_setcore_intersection(JSContext *ctx, qp_setcore *result,
                                   qp_setcore *so, qp_setcore *other,
                                   qp_dictcore_eq_fn *eq, void *opaque);
/* set_isdisjoint when `other` is an EXACT set/frozenset: 1 / 0 / -1. */
static __maybe_unused int qp_setcore_isdisjoint(JSContext *ctx, qp_setcore *so,
                                 qp_setcore *other,
                                 qp_dictcore_eq_fn *eq, void *opaque);
/* set_difference_update_internal when `other` is a set/frozenset (including
 * so == other -> clear, the "other 8x larger" intersection shortcut and the
 * final dummy purge).  0 / -1.  For any other iterable the caller discards
 * each key (computing its hash) and then calls qp_setcore_purge_dummies. */
static __maybe_unused int qp_setcore_difference_update_set(JSContext *ctx, qp_setcore *so,
                                            qp_setcore *other,
                                            qp_dictcore_eq_fn *eq,
                                            void *opaque);
/* Tail of set_difference_update_internal: resize away dummies if more than a
 * quarter of the table is dummies.  0 / -1. */
static __maybe_unused int qp_setcore_purge_dummies(JSContext *ctx, qp_setcore *so);
/* The two loops of set_difference_untracked: result (fresh, empty) gets every
 * key of `so` not in `other`.  The caller does the dispatch exactly as
 * CPython: other set -> size_other = len(other), exact dict/frozendict ->
 * size_other = len(other), else copy + difference_update; and if
 * (len(so) >> 2) > size_other: copy(so) + difference_update(other) instead
 * (qp_setcore_merge + qp_setcore_difference_update_set for a set other; the
 * generic iterable path for a dict other).  0 / -1. */
static __maybe_unused int qp_setcore_difference_set(JSContext *ctx, qp_setcore *result,
                                     qp_setcore *so, qp_setcore *other,
                                     qp_dictcore_eq_fn *eq, void *opaque);
static __maybe_unused int qp_setcore_difference_dict(JSContext *ctx, qp_setcore *result,
                                      qp_setcore *so, qp_dictcore *other,
                                      qp_dictcore_eq_fn *eq, void *opaque);
/* set_symmetric_difference_update_set / _dict (so != other; the caller
 * clears for so == other and builds a temporary set for other iterables;
 * set ^ other = new set updated from other, then _update_set(result, so)).
 * 0 / -1. */
static __maybe_unused int qp_setcore_symmetric_difference_update_set(JSContext *ctx,
                                                      qp_setcore *so,
                                                      qp_setcore *other,
                                                      qp_dictcore_eq_fn *eq,
                                                      void *opaque);
static __maybe_unused int qp_setcore_symmetric_difference_update_dict(JSContext *ctx,
                                                       qp_setcore *so,
                                                       qp_dictcore *other,
                                                       qp_dictcore_eq_fn *eq,
                                                       void *opaque);
/* set_issubset when `other` is a set/frozenset: 1 / 0 / -1.  (issuperset with
 * a set other is issubset(other, so); set equality is len check + this.) */
static __maybe_unused int qp_setcore_issubset(JSContext *ctx, qp_setcore *so,
                               qp_setcore *other,
                               qp_dictcore_eq_fn *eq, void *opaque);
/* frozenset_hash_impl (no caching): the hash of the equivalent frozenset. */
static __maybe_unused Py_hash_t qp_setcore_hash_impl(const qp_setcore *s);
/* frozenset_hash: cached in s->hash. */
static __maybe_unused Py_hash_t qp_setcore_frozenset_hash(qp_setcore *s);

static inline Py_ssize_t qp_setcore_size(const qp_setcore *s)
{
    return s->used;
}

#endif /* QP_DICTCORE_H */
