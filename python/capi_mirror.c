/*
 * dquickts Python C API, unit A (mirror core): mirror heads, allocation,
 * maps, pins, crossings JS value <-> PyObject*, reference transitions,
 * quickjs.c GC hooks, teardown, interned identifiers, singletons, hashing
 * and the PyMem_* / PyObject_Malloc families.
 *
 * Model: local://capi-design.md sections 1 and 4; contract: python/capi.h.
 * Included into the quickjs.c translation unit by python/quickpy.c after
 * capi.h (CONFIG_PYTHON_CAPI only).
 */

/* private head flags (not part of the cross-unit contract) */
#define QP_CAPI_F_LEAKED    0x8000  /* node unlinked by qp_capi_leak_node: never visited again */

#include "bytesobject_internal.h"

/* internal CPython names (capi/compat shims declare them for the modules) */
PyAPI_DATA(PyTypeObject) _PyNone_Type;
PyAPI_DATA(PyTypeObject) _PyNotImplemented_Type;
PyAPI_FUNC(Py_hash_t) _Py_HashBytes(const void *src, Py_ssize_t len);
PyAPI_FUNC(char *) _PyMem_Strdup(const char *str);
PyAPI_FUNC(char *) _PyMem_RawStrdup(const char *str);

/* python/mod_weakref.c */
static BOOL qp_weakref_check(JSContext *ctx, JSValueConst v);

/* forward declarations of helpers defined by later units of the TU */
static Py_ssize_t qp_capi_list_size(PyObject *op);                       /* E1 */
static void qp_capi_list_set_size(PyObject *op, Py_ssize_t n);           /* E1 */
static PyObject **qp_capi_list_items(PyObject *op);                      /* E1 */
static Py_ssize_t qp_capi_bytearray_live_size(PyObject *op);             /* D */
static void qp_capi_bytearray_set_live_size(PyObject *op, Py_ssize_t n); /* D */
static PyObject *qp_capi_str_immortal(JSContext *ctx, const char *utf8); /* D */
static void qp_capi_types_teardown(JSContext *ctx);                      /* B */
static void qp_capi_ids_reset(void);
static void qp_capi_modules_by_index_clear(void);                       /* stable */
static void qp_capi_ext_cache_clear(void);                              /* dynload */
PyAPI_FUNC(void) _PyArg_Fini(void);                                     /* getargs */
static void qp_capi_runtime_clear(void);                                /* E2 */
static void qp_capi_runtime_reset(JSRuntime *rt);                       /* E2 */

/* set once qp_capi_context_free ran, until a later runtime binds the C API
   again (qp_capi_init_mirror): the JS side is gone, C objects that are
   still referenced are leaked */
static int qp_capi_dead;

/* A block leaked on purpose (qp_capi_context_free) and what it references
   are not reported by LeakSanitizer. */
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#define qp_capi_lsan_ignore(h) __lsan_ignore_object(h)
#else
#define qp_capi_lsan_ignore(h) ((void)(h))
#endif

/* The C API binds to one runtime at a time: qp_capi_owner is the runtime
   of the context being bound or bound (qp_capi.ctx), set by qp_capi_init
   and cleared at the end of qp_capi_context_free, under
   qp_capi_bind_lock.  Runtimes running in other threads meanwhile get
   ImportError for the C modules; once the owner is freed, the next
   runtime binds the C API again (qp_capi_context_free resets the
   interpreter-level C state). */
static pthread_mutex_t qp_capi_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static JSRuntime *qp_capi_owner;

/* TRUE when the C API is bound (or being bound) to rt (callable from any
   thread: the hooks run by every runtime check it first) */
static BOOL qp_capi_owned_by(JSRuntime *rt)
{
    return rt && __atomic_load_n(&qp_capi_owner, __ATOMIC_ACQUIRE) == rt;
}

/* ========================================================================= */
/* Pointer-keyed hash map                                                     */
/* ========================================================================= */

#define QP_CAPI_MAP_EMPTY ((uintptr_t)0)
#define QP_CAPI_MAP_TOMB  (~(uintptr_t)0)

typedef struct QPCapiMap {
    size_t cap;         /* power of two */
    size_t count;       /* live entries */
    size_t used;        /* live + tombstones */
    uintptr_t *keys;
    void **vals;
} QPCapiMap;

static inline size_t qp_capi_map_hash(uintptr_t k, size_t cap)
{
    uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return (size_t)h & (cap - 1);
}

static QPCapiMap *qp_capi_map_new(void)
{
    QPCapiMap *m = qp_sys_calloc(1, sizeof(*m));
    return m;
}

static void qp_capi_map_free(QPCapiMap *m)
{
    if (!m)
        return;
    qp_sys_free(m->keys);
    qp_sys_free(m->vals);
    qp_sys_free(m);
}

static int qp_capi_map_resize(QPCapiMap *m, size_t ncap)
{
    uintptr_t *okeys = m->keys;
    void **ovals = m->vals;
    size_t ocap = m->cap, i;
    uintptr_t *keys = qp_sys_calloc(ncap, sizeof(*keys));
    void **vals = qp_sys_calloc(ncap, sizeof(*vals));
    if (!keys || !vals) {
        qp_sys_free(keys);
        qp_sys_free(vals);
        return -1;
    }
    for (i = 0; i < ocap; i++) {
        uintptr_t k = okeys[i];
        size_t j;
        if (k == QP_CAPI_MAP_EMPTY || k == QP_CAPI_MAP_TOMB)
            continue;
        j = qp_capi_map_hash(k, ncap);
        while (keys[j] != QP_CAPI_MAP_EMPTY)
            j = (j + 1) & (ncap - 1);
        keys[j] = k;
        vals[j] = ovals[i];
    }
    qp_sys_free(okeys);
    qp_sys_free(ovals);
    m->keys = keys;
    m->vals = vals;
    m->cap = ncap;
    m->used = m->count;
    return 0;
}

static void *qp_capi_map_get(const QPCapiMap *m, uintptr_t k)
{
    size_t j;
    if (!m || m->count == 0)
        return NULL;
    j = qp_capi_map_hash(k, m->cap);
    for (;;) {
        uintptr_t kk = m->keys[j];
        if (kk == k)
            return m->vals[j];
        if (kk == QP_CAPI_MAP_EMPTY)
            return NULL;
        j = (j + 1) & (m->cap - 1);
    }
}

/* insert or replace; 0 or -1 (out of memory, no exception set) */
static int qp_capi_map_put(QPCapiMap *m, uintptr_t k, void *v)
{
    size_t j, tomb = (size_t)-1;
    if ((m->used + 1) * 4 >= m->cap * 3) {
        size_t ncap = m->cap ? m->cap : 16;
        while ((m->count + 1) * 2 >= ncap)
            ncap *= 2;
        if (qp_capi_map_resize(m, ncap) < 0)
            return -1;
    }
    j = qp_capi_map_hash(k, m->cap);
    for (;;) {
        uintptr_t kk = m->keys[j];
        if (kk == k) {
            m->vals[j] = v;
            return 0;
        }
        if (kk == QP_CAPI_MAP_TOMB && tomb == (size_t)-1)
            tomb = j;
        if (kk == QP_CAPI_MAP_EMPTY)
            break;
        j = (j + 1) & (m->cap - 1);
    }
    if (tomb != (size_t)-1) {
        j = tomb;
    } else {
        m->used++;
    }
    m->keys[j] = k;
    m->vals[j] = v;
    m->count++;
    return 0;
}

static void qp_capi_map_del(QPCapiMap *m, uintptr_t k)
{
    size_t j;
    if (!m || m->count == 0)
        return;
    j = qp_capi_map_hash(k, m->cap);
    for (;;) {
        uintptr_t kk = m->keys[j];
        if (kk == k) {
            m->keys[j] = QP_CAPI_MAP_TOMB;
            m->vals[j] = NULL;
            m->count--;
            return;
        }
        if (kk == QP_CAPI_MAP_EMPTY)
            return;
        j = (j + 1) & (m->cap - 1);
    }
}

/* ========================================================================= */
/* Pins                                                                       */
/* ========================================================================= */

/* Entries are appended; the map gives the newest entry of a key, `prev`
   chains older entries of the same key (only HASHED keys keep several:
   two dict keys with the same hash). */
#define QP_CAPI_PINS_HASHED_MAX 8

typedef struct QPCapiPin {
    uint64_t key;
    PyObject *obj;      /* owned; NULL = dropped */
    int32_t prev;       /* older entry with the same key, -1 = none */
} QPCapiPin;

struct QPCapiPins {
    uint32_t count, cap, live;
    QPCapiPin *e;
    QPCapiMap *index;   /* key + 1 -> newest entry index + 1 */
};

/* pins of statically allocated owners (no head): owner -> QPCapiPins* */
static QPCapiMap *qp_capi_static_pins;

static inline int qp_capi_pin_is_hashed(uint64_t key)
{
    return (key >> 62) == 1;
}

static int qp_capi_pins_reindex(QPCapiPins *p)
{
    uint32_t i, n = 0;
    /* compact: keep live entries in order */
    for (i = 0; i < p->count; i++) {
        if (p->e[i].obj)
            p->e[n++] = p->e[i];
    }
    p->count = n;
    p->live = n;
    qp_capi_map_free(p->index);
    p->index = qp_capi_map_new();
    if (!p->index)
        return -1;
    for (i = 0; i < n; i++) {
        uintptr_t k = (uintptr_t)p->e[i].key + 1;
        void *old = qp_capi_map_get(p->index, k);
        p->e[i].prev = old ? (int32_t)((uintptr_t)old - 1) : -1;
        if (qp_capi_map_put(p->index, k, (void *)(uintptr_t)(i + 1)) < 0)
            return -1;
    }
    return 0;
}

static int32_t qp_capi_pins_newest(QPCapiPins *p, uint64_t key)
{
    void *r = qp_capi_map_get(p->index, (uintptr_t)key + 1);
    return r ? (int32_t)((uintptr_t)r - 1) : -1;
}

static QPCapiPins **qp_capi_pins_slot(PyObject *owner, int create)
{
    if (QP_CAPI_HAS_HEAD(owner))
        return &QP_CAPI_HEAD(owner)->pins;
    {
        void *r;
        if (!qp_capi_static_pins) {
            if (!create)
                return NULL;
            qp_capi_static_pins = qp_capi_map_new();
            if (!qp_capi_static_pins)
                return NULL;
        }
        r = qp_capi_map_get(qp_capi_static_pins, (uintptr_t)owner);
        if (!r) {
            QPCapiPins *np;
            if (!create)
                return NULL;
            /* a heap cell holding the pointer, so a stable address can be
               returned like for heads */
            np = NULL;
            r = qp_sys_calloc(1, sizeof(QPCapiPins *));
            if (!r)
                return NULL;
            *(QPCapiPins **)r = np;
            if (qp_capi_map_put(qp_capi_static_pins, (uintptr_t)owner, r) < 0) {
                qp_sys_free(r);
                return NULL;
            }
        }
        return (QPCapiPins **)r;
    }
}

static PyObject *qp_capi_pin_get(PyObject *owner, uint64_t key)
{
    QPCapiPins **slot = qp_capi_pins_slot(owner, 0);
    QPCapiPins *p;
    int32_t i;
    if (!slot || !(p = *slot))
        return NULL;
    for (i = qp_capi_pins_newest(p, key); i >= 0; i = p->e[i].prev) {
        if (p->e[i].obj)
            return p->e[i].obj;
    }
    return NULL;
}

static void qp_capi_pin_drop(PyObject *owner, uint64_t key)
{
    QPCapiPins **slot = qp_capi_pins_slot(owner, 0);
    QPCapiPins *p;
    int32_t i;
    if (!slot || !(p = *slot))
        return;
    for (i = qp_capi_pins_newest(p, key); i >= 0; i = p->e[i].prev) {
        PyObject *o = p->e[i].obj;
        if (o) {
            p->e[i].obj = NULL;
            p->live--;
            Py_DECREF(o);
        }
    }
    qp_capi_map_del(p->index, (uintptr_t)key + 1);
}

static void qp_capi_mark_persist(PyObject *op)
{
    if (QP_CAPI_HAS_HEAD(op))
        QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
}

/* steals obj */
static void qp_capi_pin_set(PyObject *owner, uint64_t key, PyObject *obj)
{
    QPCapiPins **slot;
    QPCapiPins *p;
    PyObject *old = NULL;
    int32_t i, n_same = 0, oldest = -1;

    if (!obj)
        return;
    slot = qp_capi_pins_slot(owner, 1);
    if (!slot)
        goto oom;
    p = *slot;
    if (!p) {
        p = qp_sys_calloc(1, sizeof(*p));
        if (!p)
            goto oom;
        p->index = qp_capi_map_new();
        if (!p->index) {
            qp_sys_free(p);
            goto oom;
        }
        *slot = p;
    }
    qp_capi_mark_persist(owner);
    for (i = qp_capi_pins_newest(p, key); i >= 0; i = p->e[i].prev) {
        if (p->e[i].obj == obj) {
            /* already pinned: the reference is redundant */
            Py_DECREF(obj);
            return;
        }
        if (p->e[i].obj) {
            n_same++;
            oldest = i;
        }
    }
    if (!qp_capi_pin_is_hashed(key)) {
        /* replace: drop every older value of the slot (released after the
           new pin is in place) */
        int32_t j = qp_capi_pins_newest(p, key);
        if (j >= 0 && p->e[j].obj) {
            old = p->e[j].obj;
            p->e[j].obj = obj;
            p->e[j].prev = -1;
            Py_DECREF(old);
            return;
        }
    } else if (n_same >= QP_CAPI_PINS_HASHED_MAX && oldest >= 0) {
        old = p->e[oldest].obj;
        p->e[oldest].obj = NULL;
        p->live--;
    }
    if (p->count == p->cap) {
        if (p->live * 2 < p->count && p->count >= 16) {
            if (qp_capi_pins_reindex(p) < 0)
                goto oom_obj;
        }
        if (p->count == p->cap) {
            uint32_t ncap = p->cap ? p->cap * 2 : 4;
            QPCapiPin *ne = qp_sys_realloc(p->e, (size_t)ncap * sizeof(*ne));
            if (!ne)
                goto oom_obj;
            p->e = ne;
            p->cap = ncap;
        }
    }
    i = (int32_t)p->count++;
    p->e[i].key = key;
    p->e[i].obj = obj;
    p->e[i].prev = qp_capi_pins_newest(p, key);
    if (qp_capi_map_put(p->index, (uintptr_t)key + 1, (void *)(uintptr_t)(i + 1)) < 0) {
        p->e[i].obj = NULL;
        p->count--;
        goto oom_obj;
    }
    p->live++;
    Py_XDECREF(old);
    return;
 oom_obj:
    Py_XDECREF(old);
 oom:
    /* out of memory: the pin cannot be recorded.  Keep the object alive
       for the rest of the process (a leak is the only safe outcome for a
       borrowed reference already handed out). */
    (void)obj;
    return;
}

static void qp_capi_pins_visit(QPCapiPins *pins, visitproc visit, void *arg)
{
    uint32_t i;
    if (!pins)
        return;
    for (i = 0; i < pins->count; i++) {
        if (pins->e[i].obj)
            visit(pins->e[i].obj, arg);
    }
}

static void qp_capi_pins_free(JSRuntime *rt, QPCapiPins *pins)
{
    uint32_t i;
    (void)rt;
    if (!pins)
        return;
    for (i = 0; i < pins->count; i++) {
        PyObject *o = pins->e[i].obj;
        pins->e[i].obj = NULL;
        Py_XDECREF(o);
    }
    qp_capi_map_free(pins->index);
    qp_sys_free(pins->e);
    qp_sys_free(pins);
}

/* detach and release the pins of a head */
static void qp_capi_head_drop_pins(QPCapiHead *h)
{
    QPCapiPins *p = h->pins;
    if (p) {
        h->pins = NULL;
        qp_capi_pins_free(qp_capi.rt, p);
    }
}

/* ========================================================================= */
/* Heads: lists, maps, binding                                                */
/* ========================================================================= */

static inline BOOL qp_capi_v_is_object(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT;
}

static inline BOOL qp_capi_v_is_string(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING;
}

/* leave live_list / weak_list (live_link stays self-linked when unlisted) */
static void qp_capi_unlist(QPCapiHead *h)
{
    list_del(&h->live_link);
    init_list_head(&h->live_link);
}

static void qp_capi_node_link(QPCapiHead *h)
{
    qp_capi_unlist(h);
    add_gc_object(qp_capi.rt, &h->gc, JS_GC_OBJ_TYPE_PY_CAPI);
    list_add_tail(&h->live_link, &qp_capi.live_list);
}

static void qp_capi_node_unlink(QPCapiHead *h)
{
    /* list_del is valid in whichever GC list the node currently is, and on
       a leaked node (self-linked) */
    list_del(&h->gc.link);
    qp_capi_unlist(h);
    h->flags &= ~(QP_CAPI_F_LIVE | QP_CAPI_F_LEAKED);
}

/* LIVE -> WEAK (ob_refcnt reached 0, J keeps the mirror): list the head so
   that the sync pass can revive it, see qp_capi_revive */
static void qp_capi_weak_link(QPCapiHead *h)
{
    qp_capi_unlist(h);
    list_add_tail(&h->live_link, &qp_capi.weak_list);
}

/* JS object of a bound head (NULL if v is not a live object) */
static inline JSObject *qp_capi_head_jobj(QPCapiHead *h)
{
    if ((h->flags & QP_CAPI_F_ORPHAN) || !qp_capi_v_is_object(h->v))
        return NULL;
    return JS_VALUE_GET_OBJ(h->v);
}

/* register the identity of a bound head: obj_map / u.opaque / str_map */
static int qp_capi_register(QPCapiHead *h)
{
    PyObject *op = QP_CAPI_OBJ(h);
    if (qp_capi_v_is_object(h->v)) {
        JSObject *p = JS_VALUE_GET_OBJ(h->v);
        h->flags |= _Py_QP_BOUND_FLAG;
        if (p->class_id == JS_CLASS_PY_NATIVE) {
            p->u.opaque = op;
        } else if (qp_capi_map_put(qp_capi.obj_map, (uintptr_t)p, h) < 0) {
            return -1;
        }
        p->py_has_mirror = 1;
    } else if (qp_capi_v_is_string(h->v)) {
        JSString *s = JS_VALUE_GET_STRING(h->v);
        if (!qp_capi_map_get(qp_capi.str_map, (uintptr_t)s)) {
            if (qp_capi_map_put(qp_capi.str_map, (uintptr_t)s, h) < 0)
                return -1;
            h->flags |= QP_CAPI_F_INTERNED;
        }
    }
    return 0;
}

/* remove the identity entries of h (v unchanged) */
static void qp_capi_unregister(QPCapiHead *h)
{
    PyObject *op = QP_CAPI_OBJ(h);
    h->flags &= ~_Py_QP_BOUND_FLAG;
    if (qp_capi_v_is_object(h->v) && !(h->flags & QP_CAPI_F_ORPHAN)) {
        JSObject *p = JS_VALUE_GET_OBJ(h->v);
        if (p->class_id == JS_CLASS_PY_NATIVE) {
            if (p->u.opaque == op) {
                p->u.opaque = NULL;
                p->py_has_mirror = 0;
            }
        } else if (qp_capi_map_get(qp_capi.obj_map, (uintptr_t)p) == h) {
            qp_capi_map_del(qp_capi.obj_map, (uintptr_t)p);
            p->py_has_mirror = 0;
        }
    }
    if (h->flags & QP_CAPI_F_INTERNED) {
        JSString *s = JS_VALUE_GET_STRING(h->v);
        if (qp_capi_map_get(qp_capi.str_map, (uintptr_t)s) == h)
            qp_capi_map_del(qp_capi.str_map, (uintptr_t)s);
        h->flags &= ~QP_CAPI_F_INTERNED;
    }
}

/* head of the mirror of JS object p, or NULL */
static QPCapiHead *qp_capi_head_of_jobj(JSObject *p)
{
    if (!p->py_has_mirror)
        return NULL;
    if (p->class_id == JS_CLASS_PY_NATIVE) {
        PyObject *op = p->u.opaque;
        return (op && QP_CAPI_HAS_HEAD(op)) ? QP_CAPI_HEAD(op) : NULL;
    }
    return qp_capi_map_get(qp_capi.obj_map, (uintptr_t)p);
}

/* 0 -> 1 transition of a weak mirror: link the node, take the JS reference
   and refresh the mirrored facts */
static void qp_capi_link_live(QPCapiHead *h)
{
    h->flags |= QP_CAPI_F_LIVE;
    qp_capi_node_link(h);
    if (!QP_IS_NULL(h->v) && !(h->flags & (QP_CAPI_F_ORPHAN | QP_CAPI_F_OWNS_V))) {
        JS_DupValueRT(qp_capi.rt, h->v);
        h->flags |= QP_CAPI_F_OWNS_V;
    }
}

static void qp_capi_make_live(QPCapiHead *h)
{
    PyObject *op = QP_CAPI_OBJ(h);
    qp_capi_link_live(h);
    if (h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].refresh && qp_capi.ctx)
        qp_capi.kind_ops[h->kind].refresh(qp_capi.ctx, op);
}

/* A weak mirror can get ob_refcnt back above 0 without qp_capi_incref:
   C code that keeps a pointer it borrowed from a container (CPython's idiom
   "new reference, Py_DECREF, rely on the container", e.g. _decimal's
   CURRENT_CONTEXT, then Py_NewRef) takes its reference with the inline
   Py_INCREF.  Such a mirror is LIVE again: it must own J and be a GC node,
   or J (and a native's payload with it) could die under the C reference.
   Checked at every crossing (qp_capi_incref, qp_capi_value) and for the
   whole weak_list by the sync pass of each collection. */
static inline BOOL qp_capi_is_revived(QPCapiHead *h)
{
    return !(h->flags & (QP_CAPI_F_LIVE | QP_CAPI_F_DEALLOC)) &&
           h->tag == QP_CAPI_TAG_OBJECT && QP_CAPI_OBJ(h)->ob_refcnt > 0 &&
           !qp_capi_dead;
}

/* ========================================================================= */
/* Allocation                                                                 */
/* ========================================================================= */

static QPCapiHead *qp_capi_block_alloc(size_t objsize)
{
    QPCapiHead *h;
    if (!qp_capi.rt)
        return NULL;
    if (objsize > (size_t)PY_SSIZE_T_MAX - sizeof(QPCapiHead))
        return NULL;
    h = js_malloc_rt(qp_capi.rt, sizeof(QPCapiHead) + objsize);
    if (!h)
        return NULL;
    memset(h, 0, sizeof(QPCapiHead) + objsize);
    h->v = QP_NULL;
    init_list_head(&h->gc.link);
    init_list_head(&h->live_link);
    return h;
}

static void qp_capi_no_memory(void)
{
    if (qp_capi.ctx)
        JS_ThrowOutOfMemory(qp_capi.ctx);
}

static PyObject *qp_capi_alloc(PyTypeObject *type, size_t size, QPCapiKind kind)
{
    QPCapiHead *h;
    PyObject *op;
    if (size < sizeof(PyObject))
        size = sizeof(PyObject);
    h = qp_capi_block_alloc(size);
    if (!h) {
        qp_capi_no_memory();
        return NULL;
    }
    h->tag = QP_CAPI_TAG_OBJECT;
    h->kind = (uint16_t)kind;
    if (kind == QP_CAPI_K_NATIVE)
        h->flags |= QP_CAPI_F_NATIVE;
    op = QP_CAPI_OBJ(h);
    op->ob_refcnt = 1;
    op->ob_type = type;
    if (type && QP_CAPI_HAS_HEAD(type))
        Py_INCREF(type);
    h->flags |= QP_CAPI_F_LIVE;
    qp_capi_node_link(h);
    return op;
}

/* debug hooks on the allocators (PyMem section, below) */
static int qp_capi_memdbg_obj_on;
static void qp_capi_memdbg_poison(JSRuntime *rt, QPCapiHead *h);

static void qp_capi_release(JSRuntime *rt, QPCapiHead *h)
{
    JSValue v = JS_UNDEFINED;
    BOOL free_v = FALSE;
    if (h->flags & QP_CAPI_F_LIVE)
        qp_capi_node_unlink(h);
    else
        qp_capi_unlist(h);
    if (!QP_IS_NULL(h->v)) {
        qp_capi_unregister(h);
        if (h->flags & QP_CAPI_F_OWNS_V) {
            v = h->v;
            free_v = TRUE;
        }
    }
    h->v = QP_NULL;
    h->flags &= ~QP_CAPI_F_OWNS_V;
    qp_capi_head_drop_pins(h);
    h->tag = QP_CAPI_TAG_FREED;
    if (unlikely(qp_capi_memdbg_obj_on))
        qp_capi_memdbg_poison(rt, h);
    js_free_rt(rt, h);
    /* last: may free the JS object and run hooks */
    if (free_v)
        JS_FreeValueRT(rt, v);
}

/* ========================================================================= */
/* Kinds of JS values                                                         */
/* ========================================================================= */

static QPCapiKind qp_capi_kind_of(JSContext *ctx, JSValueConst v)
{
    JSObject *p;
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return QP_CAPI_K_LONG;
    case JS_TAG_FLOAT64:
        return QP_CAPI_K_FLOAT;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return QP_CAPI_K_STR;
    case JS_TAG_OBJECT:
        break;
    default:
        return QP_CAPI_K_OBJECT;
    }
    p = JS_VALUE_GET_OBJ(v);
    switch (p->class_id) {
    case JS_CLASS_PY_TYPE:
        return QP_CAPI_K_TYPE;
    case JS_CLASS_PY_NATIVE:
        return QP_CAPI_K_NATIVE;
    case JS_CLASS_PY_TUPLE:
        return QP_CAPI_K_TUPLE;
    case JS_CLASS_ARRAY:
        return QP_CAPI_K_LIST;
    case JS_CLASS_PY_DICT:
    case JS_CLASS_PY_FROZENDICT:
        return QP_CAPI_K_DICT;
    case JS_CLASS_PY_SET:
    case JS_CLASS_PY_FROZENSET:
        return QP_CAPI_K_SET;
    case JS_CLASS_PY_BYTES:
        return QP_CAPI_K_BYTES;
    case JS_CLASS_PY_BYTEARRAY:
        switch (qp_bk_kind(v)) {
        case QP_BK_BYTEARRAY:
            return QP_CAPI_K_BYTEARRAY;
        case QP_BK_MEMORYVIEW:
            return QP_CAPI_K_MEMORYVIEW;
        default:
            return QP_CAPI_K_OBJECT;
        }
    case JS_CLASS_PY_COMPLEX:
        return QP_CAPI_K_COMPLEX;
    case JS_CLASS_PY_MODULE:
        return QP_CAPI_K_MODULE;
    case JS_CLASS_PY_CFUNCTION:
        return QP_CAPI_K_CFUNCTION;
    case JS_CLASS_PY_METHOD:
        return QP_CAPI_K_METHOD;
    case JS_CLASS_PY_DESCR:
        return QP_CAPI_K_DESCR;
    case JS_CLASS_PY_SLICE:
        return QP_CAPI_K_SLICE;
    case JS_CLASS_PY_EXCEPTION:
        return QP_CAPI_K_EXCEPTION;
    case JS_CLASS_PY_CELL:
        return QP_CAPI_K_CELL;
    case JS_CLASS_PY_CODE:
        return QP_CAPI_K_CODE;
    case JS_CLASS_PY_BOXED:
        switch (JS_VALUE_GET_TAG(p->u.object_data)) {
        case JS_TAG_INT:
        case JS_TAG_SHORT_BIG_INT:
        case JS_TAG_BIG_INT:
        case JS_TAG_BOOL:
            return QP_CAPI_K_LONG;
        case JS_TAG_FLOAT64:
            return QP_CAPI_K_FLOAT;
        case JS_TAG_STRING:
        case JS_TAG_STRING_ROPE:
            return QP_CAPI_K_STR;
        default:
            return QP_CAPI_K_OBJECT;
        }
    case JS_CLASS_PY_OBJECT:
        if (qp_weakref_check(ctx, v))
            return QP_CAPI_K_WEAKREF;
        return QP_CAPI_K_OBJECT;
    default:
        if (qp_bm_is_typed_array_class(p->class_id))
            return QP_CAPI_K_MEMORYVIEW;
        if (qp_is_py_function(v))
            return QP_CAPI_K_FUNCTION;
        return QP_CAPI_K_OBJECT;
    }
}

/* ========================================================================= */
/* Crossings JS -> C                                                          */
/* ========================================================================= */

/* counted reference with the 0 -> 1 transition */
static void qp_capi_incref(PyObject *op)
{
    QPCapiHead *h;
    if (!QP_CAPI_HAS_HEAD(op)) {
        Py_INCREF(op);
        return;
    }
    h = QP_CAPI_HEAD(op);
    if (op->ob_refcnt == 0 && !(h->flags & QP_CAPI_F_DEALLOC)) {
        op->ob_refcnt = 1;
        qp_capi_make_live(h);
        return;
    }
    Py_INCREF(op);
    if (qp_capi_is_revived(h))
        qp_capi_make_live(h);
}

/* size of the public struct of each kind, used when the owner registers no
   size hook (variable-size kinds always register one) */
static const size_t qp_capi_kind_default_size[QP_CAPI_K__COUNT] = {
    [QP_CAPI_K_RAW] = sizeof(PyObject),
    [QP_CAPI_K_NATIVE] = sizeof(PyObject),
    [QP_CAPI_K_OBJECT] = sizeof(PyObject),
    [QP_CAPI_K_TYPE] = sizeof(PyHeapTypeObject),
    [QP_CAPI_K_MODULE] = sizeof(PyObject),
    [QP_CAPI_K_CFUNCTION] = sizeof(PyCMethodObject),
    [QP_CAPI_K_METHOD] = sizeof(PyMethodObject),
    [QP_CAPI_K_FUNCTION] = sizeof(PyFunctionObject),
    [QP_CAPI_K_DESCR] = sizeof(PyWrapperDescrObject),
    [QP_CAPI_K_LONG] = sizeof(PyLongObject),
    [QP_CAPI_K_FLOAT] = sizeof(PyFloatObject),
    [QP_CAPI_K_COMPLEX] = sizeof(PyComplexObject),
    [QP_CAPI_K_STR] = sizeof(PyUnicodeObject),
    [QP_CAPI_K_BYTES] = sizeof(PyBytesObject),
    [QP_CAPI_K_BYTEARRAY] = sizeof(PyByteArrayObject),
    [QP_CAPI_K_MEMORYVIEW] = sizeof(PyMemoryViewObject),
    [QP_CAPI_K_TUPLE] = sizeof(PyTupleObject),
    [QP_CAPI_K_LIST] = sizeof(PyListObject),
    [QP_CAPI_K_DICT] = sizeof(PyDictObject),
    [QP_CAPI_K_SET] = sizeof(PySetObject),
    [QP_CAPI_K_SLICE] = sizeof(PySliceObject),
    [QP_CAPI_K_EXCEPTION] = sizeof(PyBaseExceptionObject),
    [QP_CAPI_K_CELL] = sizeof(PyCellObject),
    [QP_CAPI_K_WEAKREF] = sizeof(PyWeakReference),
    [QP_CAPI_K_CODE] = sizeof(PyCodeObject),
};

/* fill a new mirror: head bound to v (owned), ob_type counted, kind init */
static PyObject *qp_capi_new_mirror_sized(JSContext *ctx, JSValueConst v, QPCapiKind kind,
                                          PyTypeObject *ob_type, size_t extra, BOOL register_id)
{
    QPCapiKindOps *ops = &qp_capi.kind_ops[kind];
    size_t size = sizeof(PyObject) + extra;
    QPCapiHead *h;
    PyObject *op;
    BOOL native_ext = FALSE, type_ext = FALSE;

    if (ops->size)
        size = sizeof(PyObject) + ops->size(ctx, v);
    else if (qp_capi_kind_default_size[kind] > size)
        size = qp_capi_kind_default_size[kind];
    /* a C type extending a builtin (defaultdict over dict): the C fields
       follow the builtin layout, zeroed (design 1.5) */
    if (ob_type && kind != QP_CAPI_K_NATIVE && kind != QP_CAPI_K_OBJECT &&
        ob_type->tp_itemsize == 0 &&
        ob_type->tp_basicsize > 0 && (size_t)ob_type->tp_basicsize > size) {
        size = (size_t)ob_type->tp_basicsize;
        native_ext = TRUE;
    }
    /* a type whose metaclass is a C type with PEP 697 type data (ctypes'
       PyCStructType & co: StgInfo after the PyHeapTypeObject): the data
       follows the type layout, zeroed, and the metaclass's tp_dealloc /
       tp_traverse / tp_clear own it, as for any extension instance */
    if (ob_type && kind == QP_CAPI_K_TYPE && (ob_type->tp_flags & Py_TPFLAGS_ITEMS_AT_END) &&
        ob_type->tp_basicsize > 0 && (size_t)ob_type->tp_basicsize > size) {
        size = (size_t)ob_type->tp_basicsize;
        native_ext = TRUE;
        type_ext = TRUE;
    }
    op = qp_capi_alloc(ob_type, size, kind);
    if (!op)
        return NULL;
    h = QP_CAPI_HEAD(op);
    if (native_ext)
        h->flags |= QP_CAPI_F_NATIVE | QP_CAPI_F_PERSIST;
    h->v = JS_DupValue(ctx, v);
    h->flags |= QP_CAPI_F_OWNS_V;
    if (register_id && qp_capi_register(h) < 0) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (ops->init && ops->init(ctx, op, v) < 0)
        goto fail;
    if (type_ext && (ob_type->tp_flags & Py_TPFLAGS_HAVE_GC))
        PyObject_GC_Track(op);
    return op;
 fail:
    /* undo: the mirror never escaped */
    if (ops->clear)
        ops->clear(qp_capi.rt, op);
    if (ob_type && QP_CAPI_HAS_HEAD(ob_type))
        Py_DECREF(ob_type);
    op->ob_refcnt = 0;
    qp_capi_release(qp_capi.rt, h);
    return NULL;
}

static PyObject *qp_capi_new_mirror(JSContext *ctx, JSValueConst v, QPCapiKind kind,
                                    PyTypeObject *ob_type)
{
    return qp_capi_new_mirror_sized(ctx, v, kind, ob_type, 0, TRUE);
}

static PyObject *qp_capi_lookup(JSValueConst v)
{
    QPCapiHead *h = NULL;
    if (qp_capi_v_is_object(v))
        h = qp_capi_head_of_jobj(JS_VALUE_GET_OBJ(v));
    else if (qp_capi_v_is_string(v))
        h = qp_capi_map_get(qp_capi.str_map, (uintptr_t)JS_VALUE_GET_STRING(v));
    return h ? QP_CAPI_OBJ(h) : NULL;
}

/* extension instance (J of class PY_NATIVE): its payload, allocated lazily
   when J was created from Python (object.__new__) */
static PyObject *qp_capi_native_of(JSContext *ctx, JSValueConst v)
{
    JSObject *p = JS_VALUE_GET_OBJ(v);
    PyObject *op = p->u.opaque;
    PyTypeObject *tp;
    QPCapiHead *h;
    size_t size = sizeof(PyObject);
    QPCapiKindOps *ops = &qp_capi.kind_ops[QP_CAPI_K_NATIVE];

    if (op) {
        qp_capi_incref(op);
        return op;
    }
    tp = qp_capi_type_of(ctx, v);
    if (!tp)
        return NULL;
    if (ops->size)
        size = sizeof(PyObject) + ops->size(ctx, v);
    else if (tp->tp_basicsize > 0)
        size = (size_t)tp->tp_basicsize;
    op = qp_capi_alloc(tp, size, QP_CAPI_K_NATIVE);
    if (!op)
        return NULL;
    h = QP_CAPI_HEAD(op);
    h->flags |= QP_CAPI_F_PERSIST;
    h->v = JS_DupValue(ctx, v);
    h->flags |= QP_CAPI_F_OWNS_V;
    qp_capi_register(h);
    if (ops->init && ops->init(ctx, op, v) < 0) {
        /* keep the (zeroed) payload bound: J owns it now */
        Py_DECREF(op);
        return NULL;
    }
    return op;
}

static PyObject *qp_capi_small_int_of(JSValueConst v)
{
    int64_t n;
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
        n = JS_VALUE_GET_INT(v);
        break;
    case JS_TAG_SHORT_BIG_INT:
        n = JS_VALUE_GET_SHORT_BIG_INT(v);
        break;
    default:
        return NULL;
    }
    if (n < LONG_MIN || n > LONG_MAX)
        return NULL;
    return qp_capi_small_int((long)n);
}

static PyObject *qp_capi_new_ref(JSContext *ctx, JSValueConst v)
{
    PyObject *op;
    QPCapiKind kind;
    PyTypeObject *tp;

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL:
        return Py_None;
    case JS_TAG_BOOL:
        return JS_VALUE_GET_BOOL(v) ? Py_True : Py_False;
    case JS_TAG_EXCEPTION:
        return NULL;
    case JS_TAG_UNINITIALIZED:
        qp_raise_system_error(ctx, "NULL object crossing into the C API");
        return NULL;
    case JS_TAG_INT:
    case JS_TAG_SHORT_BIG_INT:
        op = qp_capi_small_int_of(v);
        if (op)
            return op;
        break;
    case JS_TAG_STRING_ROPE:
        {
            JSValue s = js_linearize_string_rope(ctx, JS_DupValue(ctx, v));
            if (JS_IsException(s))
                return NULL;
            op = qp_capi_new_ref(ctx, s);
            JS_FreeValue(ctx, s);
            return op;
        }
    case JS_TAG_STRING:
        op = qp_capi_lookup(v);
        if (op) {
            qp_capi_incref(op);
            return op;
        }
        break;
    case JS_TAG_OBJECT:
        {
            JSObject *p = JS_VALUE_GET_OBJ(v);
            QPState *qs = QP_STATE(ctx);
            if (qs) {
                if (qp_is_notimpl(ctx, v))
                    return Py_NotImplemented;
                if (JS_VALUE_GET_TAG(qs->ellipsis) == JS_TAG_OBJECT &&
                    JS_VALUE_GET_OBJ(qs->ellipsis) == p)
                    return Py_Ellipsis;
            }
            if (p->class_id == JS_CLASS_PY_TYPE) {
                QPType *t = qp_type_from_value(v);
                PyTypeObject *ct;
                if (!t)
                    break;
                ct = qp_capi_type_from_qp(ctx, t);
                if (!ct)
                    return NULL;
                qp_capi_incref((PyObject *)ct);
                return (PyObject *)ct;
            }
            if (p->class_id == JS_CLASS_PY_NATIVE)
                return qp_capi_native_of(ctx, v);
            {
                QPCapiHead *h = qp_capi_head_of_jobj(p);
                if (h) {
                    op = QP_CAPI_OBJ(h);
                    qp_capi_incref(op);
                    return op;
                }
            }
        }
        break;
    default:
        break;
    }
    kind = qp_capi_kind_of(ctx, v);
    tp = qp_capi_type_of(ctx, v);
    if (!tp)
        return NULL;
    /* ints and floats: a fresh mirror each time, not registered (no
       identity); strings and objects: registered */
    return qp_capi_new_mirror_sized(ctx, v, kind, tp, 0,
                                    kind != QP_CAPI_K_LONG && kind != QP_CAPI_K_FLOAT &&
                                    (qp_capi_v_is_object(v) || qp_capi_v_is_string(v)));
}

static PyObject *qp_capi_steal(JSContext *ctx, JSValue v)
{
    PyObject *op = qp_capi_new_ref(ctx, v);
    JS_FreeValue(ctx, v);
    return op;
}

static PyObject *qp_capi_from_js_result(JSContext *ctx, JSValue r)
{
    if (JS_IsException(r))
        return NULL;
    return qp_capi_steal(ctx, r);
}

static BOOL qp_capi_same_value(JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_VALUE_GET_TAG(b))
        return FALSE;
    switch (JS_VALUE_GET_TAG(a)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
        return JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    case JS_TAG_SHORT_BIG_INT:
        return JS_VALUE_GET_SHORT_BIG_INT(a) == JS_VALUE_GET_SHORT_BIG_INT(b);
    case JS_TAG_FLOAT64:
        {
            double x = JS_VALUE_GET_FLOAT64(a), y = JS_VALUE_GET_FLOAT64(b);
            return memcmp(&x, &y, sizeof(double)) == 0;
        }
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL:
        return TRUE;
    default:
        if (JS_VALUE_HAS_REF_COUNT(a))
            return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
        return FALSE;
    }
}

static PyObject *qp_capi_borrow(JSContext *ctx, PyObject *owner, uint64_t key, JSValueConst v)
{
    PyObject *old = qp_capi_pin_get(owner, key);
    PyObject *op;
    if (old && QP_CAPI_HAS_HEAD(old)) {
        QPCapiHead *oh = QP_CAPI_HEAD(old);
        if (!QP_IS_NULL(oh->v) && !(oh->flags & QP_CAPI_F_ORPHAN) &&
            qp_capi_same_value(oh->v, v))
            return old;
    }
    if (qp_capi_pin_is_hashed(key)) {
        /* another entry of the same hash may hold v already */
        QPCapiPins **slot = qp_capi_pins_slot(owner, 0);
        if (slot && *slot) {
            QPCapiPins *p = *slot;
            int32_t i;
            for (i = qp_capi_pins_newest(p, key); i >= 0; i = p->e[i].prev) {
                PyObject *o = p->e[i].obj;
                if (o && QP_CAPI_HAS_HEAD(o)) {
                    QPCapiHead *oh = QP_CAPI_HEAD(o);
                    if (!QP_IS_NULL(oh->v) && !(oh->flags & QP_CAPI_F_ORPHAN) &&
                        qp_capi_same_value(oh->v, v))
                        return o;
                }
            }
        }
    }
    op = qp_capi_new_ref(ctx, v);
    if (!op)
        return NULL;
    if (!QP_CAPI_HAS_HEAD(op) || _Py_IsImmortal(op)) {
        /* static / immortal objects need no pin */
        Py_DECREF(op);
        return op;
    }
    if (op == old) {
        Py_DECREF(op);
        return op;
    }
    qp_capi_pin_set(owner, key, op);
    return op;
}

/* ========================================================================= */
/* Crossings C -> JS                                                          */
/* ========================================================================= */

static JSValue qp_capi_value_static(JSContext *ctx, PyObject *op)
{
    if (op == Py_None)
        return JS_UNDEFINED;
    if (op == Py_True)
        return JS_TRUE;
    if (op == Py_False)
        return JS_FALSE;
    if (op == Py_NotImplemented)
        return QP_STATE(ctx)->not_implemented;
    if (op == Py_Ellipsis)
        return QP_STATE(ctx)->ellipsis;
    if (PyType_Check(op)) {
        QPType *t = qp_capi_type_qp((PyTypeObject *)op);
        if (!t) {
            /* static extension type not readied yet */
            if (PyType_Ready((PyTypeObject *)op) < 0)
                return JS_EXCEPTION;
            t = qp_capi_type_qp((PyTypeObject *)op);
            if (!t) {
                qp_raise_system_error(ctx, "type '%s' has no Python type object",
                                      ((PyTypeObject *)op)->tp_name);
                return JS_EXCEPTION;
            }
        }
        return QP_TYPE_VALUE(t);
    }
    if (Py_IS_TYPE(op, &PyLong_Type) && qp_capi.kind_ops[QP_CAPI_K_LONG].to_js) {
        /* static small ints: JS ints are not reference counted, so the new
           value is also a valid borrowed one */
        return qp_capi.kind_ops[QP_CAPI_K_LONG].to_js(ctx, op);
    }
    {
        /* a static instance of an extension type (_datetime's utc_timezone):
           immortal, so its JS object lives until the teardown; it is found
           back through u.opaque and has no head (py_has_mirror stays 0) */
        QPType *t = Py_TYPE(op) ? qp_capi_type_qp(Py_TYPE(op)) : NULL;
        JSObject *p = qp_capi_map_get(qp_capi.static_map, (uintptr_t)op);
        JSValue v;
        if (p)
            return JS_MKPTR(JS_TAG_OBJECT, p);
        if (t && t->instance_class_id == JS_CLASS_PY_NATIVE &&
            qp_capi.kind_ops[QP_CAPI_K_NATIVE].to_js) {
            v = qp_capi.kind_ops[QP_CAPI_K_NATIVE].to_js(ctx, op);
            if (JS_IsException(v))
                return v;
            if (qp_capi_map_put(qp_capi.static_map, (uintptr_t)op, JS_VALUE_GET_OBJ(v)) < 0) {
                JS_VALUE_GET_OBJ(v)->u.opaque = NULL;
                JS_FreeValue(ctx, v);
                return JS_ThrowOutOfMemory(ctx);
            }
            return v;
        }
    }
    qp_raise_system_error(ctx, "statically allocated '%s' object cannot cross into Python",
                          Py_TYPE(op)->tp_name);
    return JS_EXCEPTION;
}

static JSValue qp_capi_value(JSContext *ctx, PyObject *op)
{
    QPCapiHead *h;
    QPCapiKindOps *ops;
    JSValue v;

    if (!op) {
        if (!JS_HasException(ctx))
            qp_raise_system_error(ctx, "NULL object passed to the C API");
        return JS_EXCEPTION;
    }
    if (!QP_CAPI_HAS_HEAD(op))
        return qp_capi_value_static(ctx, op);
    h = QP_CAPI_HEAD(op);
    if (qp_capi_is_revived(h))
        qp_capi_make_live(h);
    if (h->flags & QP_CAPI_F_ORPHAN) {
        qp_raise_system_error(ctx, "'%s' object was collected by the cycle collector",
                              Py_TYPE(op)->tp_name);
        return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(h->v))
        return h->v;
    if (h->tag != QP_CAPI_TAG_OBJECT || h->kind >= QP_CAPI_K__COUNT) {
        qp_raise_system_error(ctx, "bad object passed to the C API");
        return JS_EXCEPTION;
    }
    ops = &qp_capi.kind_ops[h->kind];
    if (!ops->to_js) {
        qp_raise_system_error(ctx, "'%s' object cannot cross into Python",
                              Py_TYPE(op)->tp_name);
        return JS_EXCEPTION;
    }
    v = ops->to_js(ctx, op);
    if (JS_IsException(v))
        return v;
    if (!QP_IS_NULL(h->v)) {
        /* to_js bound the mirror itself */
        JS_FreeValue(ctx, v);
        return h->v;
    }
    h->v = v;
    if (h->flags & QP_CAPI_F_LIVE)
        h->flags |= QP_CAPI_F_OWNS_V;
    if (h->kind == QP_CAPI_K_NATIVE || h->kind == QP_CAPI_K_TYPE || (h->flags & QP_CAPI_F_NATIVE))
        h->flags |= QP_CAPI_F_PERSIST;
    if (qp_capi_register(h) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    if (!(h->flags & QP_CAPI_F_LIVE)) {
        /* called on an object at ob_refcnt 0 (inside its dealloc): the
           value is owned by nobody, keep it with the mirror */
        h->flags |= QP_CAPI_F_OWNS_V;
    }
    return h->v;
}

static JSValue qp_capi_to_js(JSContext *ctx, PyObject *op)
{
    JSValue v = qp_capi_value(ctx, op);
    return JS_DupValue(ctx, v);
}

static int qp_capi_to_js_array(JSContext *ctx, PyObject *const *objs, Py_ssize_t n, JSValue *out)
{
    Py_ssize_t i, j;
    for (i = 0; i < n; i++) {
        out[i] = qp_capi_to_js(ctx, objs[i]);
        if (JS_IsException(out[i])) {
            for (j = 0; j < i; j++) {
                JS_FreeValue(ctx, out[j]);
                out[j] = JS_UNDEFINED;
            }
            for (j = i; j < n; j++)
                out[j] = JS_UNDEFINED;
            return -1;
        }
    }
    return 0;
}

static void qp_capi_unbind(PyObject *op)
{
    QPCapiHead *h;
    JSValue v;
    BOOL owned;
    if (!QP_CAPI_HAS_HEAD(op))
        return;
    h = QP_CAPI_HEAD(op);
    if (QP_IS_NULL(h->v))
        return;
    qp_capi_unregister(h);
    v = h->v;
    owned = (h->flags & QP_CAPI_F_OWNS_V) && !(h->flags & QP_CAPI_F_ORPHAN);
    h->v = QP_NULL;
    h->flags &= ~(QP_CAPI_F_OWNS_V | QP_CAPI_F_ORPHAN);
    if (owned)
        JS_FreeValueRT(qp_capi.rt, v);
}

/* ========================================================================= */
/* Reference transitions                                                      */
/* ========================================================================= */

/* run tp_dealloc of an extension instance whose JS side is gone or absent */
static void qp_capi_native_dealloc(PyObject *op)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
    if (h->flags & QP_CAPI_F_LIVE)
        qp_capi_node_unlink(h);
    h->flags |= QP_CAPI_F_DEALLOC;
    if (dealloc) {
        dealloc(op);
    } else {
        /* no tp_dealloc: free the block (CPython would crash) */
        PyObject_Free(op);
    }
}

/* release a non-native mirror whose last C reference is gone */
static void qp_capi_mirror_free(PyObject *op)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    PyTypeObject *tp = Py_TYPE(op);
    if (h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].clear)
        qp_capi.kind_ops[h->kind].clear(qp_capi.rt, op);
    op->ob_type = NULL;
    qp_capi_release(qp_capi.rt, h);
    if (tp && QP_CAPI_HAS_HEAD(tp))
        Py_DECREF(tp);
}

void _Py_Dealloc(PyObject *op)
{
    QPCapiHead *h;
    JSValue v;

    if (!QP_CAPI_HAS_HEAD(op))
        return;                 /* static objects are immortal */
    h = QP_CAPI_HEAD(op);
    if (h->tag != QP_CAPI_TAG_OBJECT)
        return;
    if (h->flags & QP_CAPI_F_DEALLOC)
        return;                 /* tp_dealloc in progress owns the object */
    if (qp_capi_dead) {
        /* after teardown: the JS side is gone, leak */
        return;
    }
    if (h->flags & QP_CAPI_F_NATIVE) {
        if (QP_IS_NULL(h->v) || (h->flags & QP_CAPI_F_ORPHAN)) {
            qp_capi_native_dealloc(op);
            return;
        }
        /* bound native: the payload stays with J (WEAK) */
        if (h->flags & QP_CAPI_F_LIVE)
            qp_capi_node_unlink(h);
        qp_capi_weak_link(h);
        h->flags |= QP_CAPI_F_PERSIST;
        if (h->flags & QP_CAPI_F_OWNS_V) {
            v = h->v;
            h->flags &= ~QP_CAPI_F_OWNS_V;
            JS_FreeValueRT(qp_capi.rt, v);     /* may run qp_capi_object_freed */
        }
        return;
    }
    if (!QP_IS_NULL(h->v) && !(h->flags & QP_CAPI_F_ORPHAN) && qp_capi_v_is_object(h->v) &&
        ((h->flags & QP_CAPI_F_PERSIST) ||
         js_rc(JS_VALUE_GET_OBJ(h->v))->ref_count > ((h->flags & QP_CAPI_F_OWNS_V) ? 1 : 0))) {
        /* WEAK: kept while J lives, owns nothing of J.  A mirror whose J
           is still referenced elsewhere (a container or an attribute C
           stored it in) is kept as well: CPython's object stays at its
           address, and C code keeps using the pointer it stored and
           released (PyDict_SetItem(d, k, v); Py_DECREF(v); use v). */
        h->flags |= QP_CAPI_F_PERSIST;
        if (h->flags & QP_CAPI_F_LIVE)
            qp_capi_node_unlink(h);
        qp_capi_weak_link(h);
        if (h->flags & QP_CAPI_F_OWNS_V) {
            v = h->v;
            h->flags &= ~QP_CAPI_F_OWNS_V;
            JS_FreeValueRT(qp_capi.rt, v);     /* last: may release the mirror */
        }
        return;
    }
    qp_capi_mirror_free(op);
}

void _Py_IncRef(PyObject *o)
{
    Py_INCREF(o);
}

void _Py_DecRef(PyObject *o)
{
    Py_DECREF(o);
}

void Py_IncRef(PyObject *o)
{
    Py_XINCREF(o);
}

void Py_DecRef(PyObject *o)
{
    Py_XDECREF(o);
}

PyObject *(Py_NewRef)(PyObject *obj)
{
    return _Py_NewRef(obj);
}

PyObject *(Py_XNewRef)(PyObject *obj)
{
    return _Py_XNewRef(obj);
}

/* Innermost C method call of this thread (qp_capi_meth_call). */
static _Thread_local QPCapiCall *qp_capi_calls;

/* A non-captured local of the Python frame sf holds v. */
static BOOL qp_capi_frame_local_holds(JSStackFrame *sf, JSValueConst v)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    int i;
    if (!b->vardefs)
        return FALSE;
    for (i = 0; i < sf->arg_count && i < b->arg_count; i++) {
        if (!b->vardefs[i].is_captured && JS_VALUE_GET_TAG(sf->arg_buf[i]) == JS_VALUE_GET_TAG(v) &&
            JS_VALUE_GET_PTR(sf->arg_buf[i]) == JS_VALUE_GET_PTR(v))
            return TRUE;
    }
    for (i = 0; i < b->var_count; i++) {
        if (!b->vardefs[b->arg_count + i].is_captured &&
            JS_VALUE_GET_TAG(sf->var_buf[i]) == JS_VALUE_GET_TAG(v) &&
            JS_VALUE_GET_PTR(sf->var_buf[i]) == JS_VALUE_GET_PTR(v))
            return TRUE;
    }
    return FALSE;
}

/* References to ob that CPython 3.14 borrows in the C method calls in
   progress: the bridge's own reference to self and to each argument
   (vectorcall arguments are borrowed), and the caller's operand stack copy
   of a value that a local of the calling frame holds (LOAD_FAST_BORROW).
   An operand stack copy of a temporary stays counted, as CPython's owned
   stack reference. */
static Py_ssize_t qp_capi_calls_borrowed(PyObject *ob)
{
    const QPCapiCall *c;
    Py_ssize_t d = 0;
    int i;
    for (c = qp_capi_calls; c; c = c->prev) {
        if (c->self == ob)
            d += 1 + (c->self_slot && qp_capi_frame_local_holds(c->sf, c->slots[-1]));
        for (i = 0; i < c->n; i++) {
            if (c->args[i] == ob)
                d += 1 + (c->sf && qp_capi_frame_local_holds(c->sf, c->slots[i]));
        }
    }
    return d;
}

static void qp_capi_call_enter(JSContext *ctx, QPCapiCall *c, PyObject *self, PyObject *const *args,
                               JSValueConst self_v, JSValueConst *slots, int n)
{
    JSStackFrame *sf;
    c->self = self;
    c->args = args;
    c->n = n;
    c->sf = NULL;
    c->slots = slots;
    c->self_slot = FALSE;
    /* self and the arguments are slots of the calling Python frame's
       operand stack when the bytecode called the method directly */
    for (sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        if (qp_fr_sf_func(sf)) {
            JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
            const JSValue *lo = sf->var_buf + b->var_count, *hi = lo + b->stack_size;
            const JSValue *sl = (const JSValue *)slots;
            if (sl >= lo + 1 && sl + n <= hi) {
                c->sf = sf;
                c->self_slot = self && JS_VALUE_GET_TAG(slots[-1]) == JS_VALUE_GET_TAG(self_v) &&
                    JS_VALUE_GET_PTR(slots[-1]) == JS_VALUE_GET_PTR(self_v);
            } else if (n > 0 && sl >= lo && sl + n <= hi) {
                c->sf = sf;
            }
            break;
        }
    }
    c->prev = qp_capi_calls;
    qp_capi_calls = c;
}

static void qp_capi_call_leave(QPCapiCall *c)
{
    qp_capi_calls = c->prev;
}

Py_ssize_t _PyQp_RefcntBound(PyObject *ob)
{
    QPCapiHead *h;
    JSObject *p;
    Py_ssize_t n = ob->ob_refcnt;
    if (!QP_CAPI_HAS_HEAD(ob))
        return n;
    h = QP_CAPI_HEAD(ob);
    p = qp_capi_head_jobj(h);
    if (!p)
        return n;
    n += js_rc(p)->ref_count;
    if (h->flags & QP_CAPI_F_OWNS_V)
        n--;
    if (qp_capi_calls) {
        n -= qp_capi_calls_borrowed(ob);
        if (n < 1)
            n = 1;
    }
    return n;
}

static Py_ssize_t qp_capi_c_refs(JSContext *ctx, JSValueConst v)
{
    PyObject *ob;
    Py_ssize_t n;
    if (!qp_capi_owned_by(ctx->rt))
        return 0;       /* no mirror in this runtime */
    ob = qp_capi_lookup(v);
    if (!ob || !QP_CAPI_HAS_HEAD(ob))
        return 0;
    n = ob->ob_refcnt;
    if (QP_CAPI_HEAD(ob)->flags & QP_CAPI_F_OWNS_V)
        n--;
    return n;
}

Py_ssize_t (Py_REFCNT)(PyObject *ob)
{
    return _Py_REFCNT(ob);
}

void _Py_SetRefcnt(PyObject *ob, Py_ssize_t refcnt)
{
    if (refcnt < 0 || _Py_IsImmortal(ob))
        return;
    ob->ob_refcnt = refcnt;
    if (refcnt == 0)
        _Py_Dealloc(ob);
}

void _Py_NegativeRefcount(const char *filename, int lineno, PyObject *op)
{
    fprintf(stderr, "%s:%d: object at %p has negative ref count %zd\n",
            filename, lineno, (void *)op, (Py_ssize_t)op->ob_refcnt);
    abort();
}

/* ========================================================================= */
/* Sizes of live-size mirrors                                                 */
/* ========================================================================= */

Py_ssize_t _PyQp_GetLiveSize(PyObject *ob)
{
    if (QP_CAPI_HAS_HEAD(ob)) {
        switch (QP_CAPI_HEAD(ob)->kind) {
        case QP_CAPI_K_LIST:
            return qp_capi_list_size(ob);
        case QP_CAPI_K_BYTEARRAY:
            return qp_capi_bytearray_live_size(ob);
        default:
            break;
        }
    }
    return ((PyVarObject *)ob)->ob_size;
}

void _PyQp_SetLiveSize(PyVarObject *ob, Py_ssize_t size)
{
    PyObject *op = (PyObject *)ob;
    if (QP_CAPI_HAS_HEAD(op)) {
        switch (QP_CAPI_HEAD(op)->kind) {
        case QP_CAPI_K_LIST:
            qp_capi_list_set_size(op, size);
            return;
        case QP_CAPI_K_BYTEARRAY:
            qp_capi_bytearray_set_live_size(op, size);
            return;
        default:
            break;
        }
    }
    ob->ob_size = size;
}

PyObject **_PyQp_ListItems(PyObject *list)
{
    return qp_capi_list_items(list);
}

Py_ssize_t (Py_SIZE)(PyObject *ob)
{
    return _Py_SIZE_impl(ob);
}

void (Py_SET_SIZE)(PyVarObject *ob, Py_ssize_t size)
{
    _Py_SET_SIZE_impl(ob, size);
}

PyTypeObject *(Py_TYPE)(PyObject *ob)
{
    return _Py_TYPE_impl(ob);
}

int (Py_IS_TYPE)(PyObject *ob, PyTypeObject *type)
{
    return _Py_TYPE_impl(ob) == type;
}

int (Py_Is)(PyObject *x, PyObject *y)
{
    return x == y;
}

int (Py_IsNone)(PyObject *x)
{
    return x == &_Py_NoneStruct;
}

/* ========================================================================= */
/* GC hooks (quickjs.c, capi-design R7)                                       */
/* ========================================================================= */

typedef struct QPCapiMarkArg {
    JSRuntime *rt;
    JS_MarkFunc *mark_func;
} QPCapiMarkArg;

/* visitproc: one edge to a child object (counted in its ob_refcnt) */
static int qp_capi_mark_visit(PyObject *op, void *arg)
{
    QPCapiMarkArg *a = arg;
    QPCapiHead *h;
    if (!op || !QP_CAPI_HAS_HEAD(op) || _Py_IsImmortal(op))
        return 0;
    h = QP_CAPI_HEAD(op);
    if (h->tag != QP_CAPI_TAG_OBJECT ||
        (h->flags & (QP_CAPI_F_LIVE | QP_CAPI_F_LEAKED)) != QP_CAPI_F_LIVE)
        return 0;
    a->mark_func(a->rt, &h->gc);
    return 0;
}

/* the C children of a mirror: pins, kind-owned references, tp_traverse */
static void qp_capi_mark_children(QPCapiHead *h, QPCapiMarkArg *a)
{
    PyObject *op = QP_CAPI_OBJ(h);
    traverseproc tr = NULL;
    qp_capi_pins_visit(h->pins, qp_capi_mark_visit, a);
    if ((h->flags & (QP_CAPI_F_NATIVE | QP_CAPI_F_TRACKED | QP_CAPI_F_DEALLOC)) ==
        (QP_CAPI_F_NATIVE | QP_CAPI_F_TRACKED) && Py_TYPE(op))
        tr = Py_TYPE(op)->tp_traverse;
    /* a native's tp_traverse chains to its builtin base's tp_traverse
       (qp_capi_builtin_traverse), which visits the kind-owned references:
       visiting them here too would count those edges twice */
    if (tr)
        tr(op, qp_capi_mark_visit, a);
    else if (h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].traverse)
        qp_capi.kind_ops[h->kind].traverse(op, qp_capi_mark_visit, a);
    /* the reference of a J-backed mirror to its type (qp_capi_alloc), as
       CPython's subtype_traverse visits the heap type of an instance: else
       the type of a Python object handed to C would be a root, and with it
       the globals of its methods (natives: their tp_traverse, design 1.6) */
    if (!(h->flags & QP_CAPI_F_NATIVE) && Py_TYPE(op))
        qp_capi_mark_visit((PyObject *)Py_TYPE(op), a);
}

static void qp_capi_gc_sync(JSRuntime *rt)
{
    struct list_head *el, *el1;
    if (!qp_capi_owned_by(rt) || rt != qp_capi.rt)
        return;
    /* weak mirrors that C references again become nodes (no refresh: no
       JS code may run here) */
    list_for_each_safe(el, el1, &qp_capi.weak_list) {
        QPCapiHead *h = list_entry(el, QPCapiHead, live_link);
        if (qp_capi_is_revived(h))
            qp_capi_link_live(h);
    }
    list_for_each(el, &qp_capi.live_list) {
        QPCapiHead *h = list_entry(el, QPCapiHead, live_link);
        PyObject *op = QP_CAPI_OBJ(h);
        Py_ssize_t n = op->ob_refcnt;
        if (h->flags & QP_CAPI_F_LEAKED)
            continue;
        if (n > INT_MAX / 4)
            n = INT_MAX / 4;
        if ((h->flags & QP_CAPI_F_OWNS_V) && qp_capi_head_jobj(h))
            n++;
        js_rc(&h->gc)->ref_count = (int)n;
    }
}

static void qp_capi_mark_node(JSRuntime *rt, JSGCObjectHeader *gp, JS_MarkFunc *mark_func)
{
    QPCapiHead *h = container_of(gp, QPCapiHead, gc);
    QPCapiMarkArg a = { rt, mark_func };
    if (h->flags & QP_CAPI_F_OWNS_V)
        JS_MarkValue(rt, h->v, mark_func);
    qp_capi_mark_children(h, &a);
}

static void qp_capi_mark_object(JSRuntime *rt, JSObject *p, JS_MarkFunc *mark_func)
{
    QPCapiHead *h;
    QPCapiMarkArg a = { rt, mark_func };
    if (qp_capi_dead)
        return;
    h = qp_capi_head_of_jobj(p);
    if (!h || (h->flags & QP_CAPI_F_LEAKED))
        return;
    if (h->flags & QP_CAPI_F_LIVE) {
        /* the +1 of qp_capi_gc_sync, only when the node owns J */
        if (h->flags & QP_CAPI_F_OWNS_V)
            mark_func(rt, &h->gc);
        else
            qp_capi_mark_children(h, &a);
    } else {
        /* weak mirror: J holds its children directly */
        qp_capi_mark_children(h, &a);
    }
}

/* A weak mirror revived by a bare Py_INCREF (qp_capi_is_revived) whose J
   loses its last JS reference before any crossing or collection relinked
   it: numpy's PyArray_FromInterface drops its reference to
   __array_interface__["data"] and takes the array's base reference with
   Py_INCREF while the dict still holds it; the dict dies next.  The
   mirror is made LIVE, which takes a reference to J, so J is not freed and
   the C reference keeps both. */
static BOOL qp_capi_object_revived(JSRuntime *rt, JSObject *p)
{
    QPCapiHead *h;
    (void)rt;
    if (qp_capi_dead)
        return FALSE;
    h = qp_capi_head_of_jobj(p);
    if (!h || !qp_capi_is_revived(h) || QP_IS_NULL(h->v) || JS_VALUE_GET_OBJ(h->v) != p)
        return FALSE;
    qp_capi_make_live(h);
    return TRUE;
}

static void qp_capi_free_cycle_node(JSRuntime *rt, JSGCObjectHeader *gp)
{
    QPCapiHead *h = container_of(gp, QPCapiHead, gc);
    PyObject *op = QP_CAPI_OBJ(h);

    /* leave tmp_obj_list; a node still in gc_zero_ref_count_list at the end
       of the phase is leaked by qp_capi_leak_node */
    list_del(&gp->link);
    list_add_tail(&gp->link, &rt->gc_zero_ref_count_list);

    Py_INCREF(op);
    if ((h->flags & QP_CAPI_F_NATIVE) && (h->flags & QP_CAPI_F_TRACKED) &&
        !(h->flags & QP_CAPI_F_DEALLOC)) {
        inquiry clear = Py_TYPE(op)->tp_clear;
        if (clear)
            clear(op);
    }
    qp_capi_head_drop_pins(h);
    /* natives too: the tp_clear of a builtin base (qp_capi_builtin_clear)
       only empties containers; the kind-owned references of a heap type
       (tp_dict, tp_mro, ...) or a C tuple subclass are released here, as
       CPython's type_clear / tuple dealloc would */
    if (h->kind < QP_CAPI_K__COUNT && !(h->flags & QP_CAPI_F_DEALLOC)) {
        const QPCapiKindOps *ops = &qp_capi.kind_ops[h->kind];
        if (ops->gc_clear)
            ops->gc_clear(rt, op);
        else if (ops->clear)
            ops->clear(rt, op);
    }
    Py_DECREF(op);
}

static void qp_capi_leak_node(JSRuntime *rt, JSGCObjectHeader *gp)
{
    QPCapiHead *h = container_of(gp, QPCapiHead, gc);
    (void)rt;
    /* no list_del: gc_free_cycles walks gc_zero_ref_count_list freeing the
       JS objects without unlinking them and then drops the whole list, so
       the neighbours of the node may already be freed. Self-linked, the
       node stays valid for the list_del of qp_capi_node_unlink. */
    init_list_head(&gp->link);
    h->flags |= QP_CAPI_F_LEAKED;
#ifdef QP_CAPI_DEBUG
    fprintf(stderr, "C API: '%s' object at %p survived cycle collection "
            "(tp_traverse does not match its references); leaked\n",
            Py_TYPE(QP_CAPI_OBJ(h)) ? Py_TYPE(QP_CAPI_OBJ(h))->tp_name : "?",
            (void *)QP_CAPI_OBJ(h));
#endif
}

static void qp_capi_object_freed(JSRuntime *rt, JSObject *p)
{
    QPCapiHead *h;
    PyObject *op;
    (void)rt;
    if (qp_capi_dead) {
        /* after teardown: abandon the payload */
        if (p->class_id == JS_CLASS_PY_NATIVE)
            p->u.opaque = NULL;
        p->py_has_mirror = 0;
        return;
    }
    h = qp_capi_head_of_jobj(p);
    if (!h) {
        p->py_has_mirror = 0;
        return;
    }
    op = QP_CAPI_OBJ(h);
    /* forget the identity: J is dying */
    if (p->class_id == JS_CLASS_PY_NATIVE)
        p->u.opaque = NULL;
    else
        qp_capi_map_del(qp_capi.obj_map, (uintptr_t)p);
    p->py_has_mirror = 0;
    h->flags &= ~_Py_QP_BOUND_FLAG;

    if (h->flags & QP_CAPI_F_LIVE) {
        /* freed by cycle collection while C still holds the mirror */
        h->flags |= QP_CAPI_F_ORPHAN;
        h->flags &= ~QP_CAPI_F_OWNS_V;
        h->v = QP_NULL;
        return;
    }
    h->flags &= ~QP_CAPI_F_OWNS_V;
    h->v = QP_NULL;
    if (h->flags & QP_CAPI_F_DEALLOC)
        return;
    if (op->ob_refcnt > 0) {
        /* a weak mirror revived by C (qp_capi_is_revived) that was not
           relinked yet: C still holds it.  A native keeps its payload and
           becomes C-primary again (unbound); any other mirror is ORPHAN.
           It stays on weak_list until relinked or released. */
        if (!(h->flags & QP_CAPI_F_NATIVE))
            h->flags |= QP_CAPI_F_ORPHAN;
        return;
    }
    if (h->flags & QP_CAPI_F_NATIVE) {
        qp_capi_native_dealloc(op);
        return;
    }
    qp_capi_mirror_free(op);
}

static void qp_capi_runtime_free(JSRuntime *rt)
{
    if (qp_capi_owned_by(rt) && qp_capi.ctx && qp_capi.rt == rt)
        qp_capi_context_free(qp_capi.ctx);
}

static void qp_capi_context_free(JSContext *ctx)
{
    JSRuntime *rt;
    struct list_head *el, *el1;
    size_t i;

    if (!qp_capi_owned_by(ctx->rt) || !qp_capi.ctx || ctx != qp_capi.ctx)
        return;
    rt = qp_capi.rt;
    qp_capi_tm_context_free(ctx);
    /* the interpreter-level C state (CPython finalize_interp_clear) */
    qp_capi_runtime_clear();
    qp_capi_modules_by_index_clear();
    qp_capi_ext_cache_clear();
    _PyArg_Fini();
    qp_capi_types_teardown(ctx);

    /* forget every JS identity (the JS objects die with the context) */
    if (qp_capi.obj_map) {
        for (i = 0; i < qp_capi.obj_map->cap; i++) {
            uintptr_t k = qp_capi.obj_map->keys[i];
            if (k != QP_CAPI_MAP_EMPTY && k != QP_CAPI_MAP_TOMB)
                ((JSObject *)k)->py_has_mirror = 0;
        }
    }
    /* from here on the types are gone: extension instances whose J dies
       while the live nodes release their JS references are abandoned
       (qp_capi_object_freed) instead of running tp_dealloc, which may call
       back into the C API */
    qp_capi_dead = 1;
    /* drop the JS references of the live nodes and unlink them */
    list_for_each_safe(el, el1, &qp_capi.live_list) {
        QPCapiHead *h = list_entry(el, QPCapiHead, live_link);
        JSValue v = h->v;
        BOOL owned = (h->flags & QP_CAPI_F_OWNS_V) != 0;
        if (qp_capi_v_is_object(v)) {
            JSObject *p = JS_VALUE_GET_OBJ(v);
            if (!(h->flags & QP_CAPI_F_ORPHAN)) {
                if (p->class_id == JS_CLASS_PY_NATIVE && p->u.opaque == QP_CAPI_OBJ(h))
                    p->u.opaque = NULL;
                p->py_has_mirror = 0;
            }
        }
        h->flags &= ~(QP_CAPI_F_OWNS_V | QP_CAPI_F_INTERNED | _Py_QP_BOUND_FLAG);
        h->flags |= QP_CAPI_F_ORPHAN;
        h->v = QP_NULL;
        list_del(&h->gc.link);
        init_list_head(&h->gc.link);
        list_del(&h->live_link);
        init_list_head(&h->live_link);
        h->flags |= QP_CAPI_F_LEAKED;
        qp_capi_lsan_ignore(h);
        if (owned)
            JS_FreeValueRT(rt, v);
    }
    /* weak heads own nothing: just forget the list */
    list_for_each_safe(el, el1, &qp_capi.weak_list) {
        QPCapiHead *h = list_entry(el, QPCapiHead, live_link);
        init_list_head(&h->live_link);
        qp_capi_lsan_ignore(h);
    }
    init_list_head(&qp_capi.weak_list);
    /* weak mirrors keep no JS reference; native payloads of J objects are
       abandoned with them */
    if (qp_capi.id_map) {
        for (i = 0; i < qp_capi.id_map->cap; i++) {
            uintptr_t k = qp_capi.id_map->keys[i];
            if (k != QP_CAPI_MAP_EMPTY && k != QP_CAPI_MAP_TOMB)
                JS_FreeAtomRT(rt, (JSAtom)(k - 1));
        }
    }
    /* the JS objects of static extension instances: the payload is static */
    if (qp_capi.static_map) {
        for (i = 0; i < qp_capi.static_map->cap; i++) {
            uintptr_t k = qp_capi.static_map->keys[i];
            if (k != QP_CAPI_MAP_EMPTY && k != QP_CAPI_MAP_TOMB) {
                JSObject *p = qp_capi.static_map->vals[i];
                p->u.opaque = NULL;
                JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, p));
            }
        }
    }
    qp_capi_map_free(qp_capi.obj_map);
    qp_capi_map_free(qp_capi.str_map);
    qp_capi_map_free(qp_capi.id_map);
    qp_capi_map_free(qp_capi.static_map);
    qp_capi.obj_map = qp_capi.str_map = qp_capi.id_map = qp_capi.static_map = NULL;
    qp_capi_ids_reset();
    qp_capi_runtime_reset(rt);
    init_list_head(&qp_capi.live_list);
    pthread_mutex_lock(&qp_capi_bind_lock);
    qp_capi.ctx = NULL;
    qp_capi.initialized = 0;
    __atomic_store_n(&qp_capi_owner, NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&qp_capi_bind_lock);
}

/* ========================================================================= */
/* Interned identifiers and singletons                                        */
/* ========================================================================= */

/* literal text pointer (_Py_ID / _Py_STR) -> immortal str */
static QPCapiMap *qp_capi_literal_map;
static PyObject *qp_capi_latin1[256];
static PyObject *qp_capi_singletons[QP_CAPI_SINGLETON__COUNT];

static void qp_capi_fatal(const char *msg)
{
    fprintf(stderr, "Fatal Python error: %s\n", msg);
    fflush(stderr);
    abort();
}

static JSContext *qp_capi_ctx_or_fatal(const char *where)
{
    if (!qp_capi.ctx) {
        fprintf(stderr, "Fatal Python error: %s: the C API is not initialized\n", where);
        abort();
    }
    return qp_capi.ctx;
}

/* make a mirror immortal (never released; LIVE node forever) */
static void qp_capi_immortalize(PyObject *op)
{
    if (!QP_CAPI_HAS_HEAD(op) || _Py_IsImmortal(op))
        return;
    op->ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT;
    QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    if (PyUnicode_Check(op))
        _PyASCIIObject_CAST(op)->state.interned = SSTATE_INTERNED_IMMORTAL;
}

static PyObject *qp_capi_intern_id(const char *name)
{
    JSContext *ctx = qp_capi_ctx_or_fatal("_Py_ID");
    JSAtom atom;
    PyObject *op;
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return NULL;
    op = qp_capi_map_get(qp_capi.id_map, (uintptr_t)atom + 1);
    if (op) {
        JS_FreeAtom(ctx, atom);
        return op;
    }
    op = qp_capi_str_immortal(ctx, name);
    if (!op) {
        JS_FreeAtom(ctx, atom);
        return NULL;
    }
    qp_capi_immortalize(op);
    if (qp_capi_map_put(qp_capi.id_map, (uintptr_t)atom + 1, op) < 0) {
        JS_FreeAtom(ctx, atom);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return op;     /* the atom stays referenced by id_map */
}

static PyObject *qp_capi_intern_literal(const char *text, const char *where)
{
    PyObject *op = qp_capi_map_get(qp_capi_literal_map, (uintptr_t)text);
    if (op)
        return op;
    op = qp_capi_intern_id(text);
    if (!op)
        qp_capi_fatal(where);
    if (!qp_capi_literal_map)
        qp_capi_literal_map = qp_capi_map_new();
    if (qp_capi_literal_map)
        qp_capi_map_put(qp_capi_literal_map, (uintptr_t)text, op);
    return op;
}

PyObject *_PyQp_InternId(const char *name)
{
    return qp_capi_intern_literal(name, "_Py_ID: cannot create the identifier");
}

PyObject *_PyQp_InternStr(const char *literal)
{
    return qp_capi_intern_literal(literal, "_Py_STR: cannot create the string");
}

PyObject *_PyQp_Latin1Char(int ch)
{
    JSContext *ctx;
    PyObject *op;
    JSValue s;
    ch &= 0xff;
    op = qp_capi_latin1[ch];
    if (op)
        return op;
    ctx = qp_capi_ctx_or_fatal("_Py_LATIN1_CHR");
    s = qp_str_from_char(ctx, (uint32_t)ch);
    if (JS_IsException(s))
        qp_capi_fatal("_Py_LATIN1_CHR: cannot create the string");
    op = qp_capi_steal(ctx, s);
    if (!op)
        qp_capi_fatal("_Py_LATIN1_CHR: cannot create the string");
    qp_capi_immortalize(op);
    qp_capi_latin1[ch] = op;
    return op;
}

PyObject *_PyQp_Singleton(int which)
{
    JSContext *ctx;
    PyObject *op;
    JSValue v;
    if (which < 0 || which >= QP_CAPI_SINGLETON__COUNT)
        qp_capi_fatal("_Py_SINGLETON: unknown singleton");
    op = qp_capi_singletons[which];
    if (op)
        return op;
    ctx = qp_capi_ctx_or_fatal("_Py_SINGLETON");
    switch (which) {
    case QP_CAPI_SINGLETON_TUPLE_EMPTY:
        v = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
        break;
    case QP_CAPI_SINGLETON_BYTES_EMPTY:
        v = JS_DupValue(ctx, QP_STATE(ctx)->empty_bytes);
        break;
    default:
        v = JS_AtomToString(ctx, JS_ATOM_empty_string);
        break;
    }
    if (JS_IsException(v))
        qp_capi_fatal("_Py_SINGLETON: cannot create the object");
    op = qp_capi_steal(ctx, v);
    if (!op)
        qp_capi_fatal("_Py_SINGLETON: cannot create the object");
    qp_capi_immortalize(op);
    qp_capi_singletons[which] = op;
    return op;
}

/* qp_capi_context_free: the interned strings and singletons were mirrors
   of the runtime that goes away; the next runtime creates its own */
static void qp_capi_ids_reset(void)
{
    qp_capi_map_free(qp_capi_literal_map);
    qp_capi_literal_map = NULL;
    memset(qp_capi_latin1, 0, sizeof(qp_capi_latin1));
    memset(qp_capi_singletons, 0, sizeof(qp_capi_singletons));
}

/* None, NotImplemented, Ellipsis (their types are B's static types) */
PyObject _Py_NoneStruct = { { _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &_PyNone_Type };
PyObject _Py_NotImplementedStruct = { { _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &_PyNotImplemented_Type };
PyObject _Py_EllipsisObject = { { _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &PyEllipsis_Type };

PyObject *Py_GetConstantBorrowed(unsigned int constant_id)
{
    switch (constant_id) {
    case Py_CONSTANT_NONE:
        return &_Py_NoneStruct;
    case Py_CONSTANT_FALSE:
        return Py_False;
    case Py_CONSTANT_TRUE:
        return Py_True;
    case Py_CONSTANT_ELLIPSIS:
        return &_Py_EllipsisObject;
    case Py_CONSTANT_NOT_IMPLEMENTED:
        return &_Py_NotImplementedStruct;
    case Py_CONSTANT_ZERO:
        return qp_capi_small_int(0);
    case Py_CONSTANT_ONE:
        return qp_capi_small_int(1);
    case Py_CONSTANT_EMPTY_STR:
        return _PyQp_Singleton(QP_CAPI_SINGLETON_STR_EMPTY);
    case Py_CONSTANT_EMPTY_BYTES:
        return _PyQp_Singleton(QP_CAPI_SINGLETON_BYTES_EMPTY);
    case Py_CONSTANT_EMPTY_TUPLE:
        return _PyQp_Singleton(QP_CAPI_SINGLETON_TUPLE_EMPTY);
    default:
        if (qp_capi.ctx)
            qp_raise_system_error(qp_capi.ctx, "bad argument to internal function");
        return NULL;
    }
}

PyObject *Py_GetConstant(unsigned int constant_id)
{
    PyObject *op = Py_GetConstantBorrowed(constant_id);
    return Py_XNewRef(op);
}

/* ========================================================================= */
/* Hashing                                                                    */
/* ========================================================================= */

/* layout of CPython's _Py_HashSecret_t (internal/pycore_pyhash.h) */
typedef union QPCapiHashSecret {
    unsigned char uc[24];
    struct { Py_hash_t prefix; Py_hash_t suffix; } fnv;
    struct { uint64_t k0; uint64_t k1; } siphash;
    struct { unsigned char padding[16]; Py_hash_t suffix; } djbx33a;
    struct { unsigned char padding[16]; Py_hash_t hashsalt; } expat;
} QPCapiHashSecret;
Py_EXPORTED_SYMBOL QPCapiHashSecret _Py_HashSecret;

Py_hash_t Py_HashPointer(const void *ptr)
{
    return (Py_hash_t)qp_hash_pointer((uintptr_t)ptr);
}

Py_hash_t _Py_HashDouble(PyObject *inst, double v)
{
    return (Py_hash_t)qp_hash_double(v, (uintptr_t)inst);
}

Py_hash_t Py_HashBuffer(const void *ptr, Py_ssize_t len)
{
    return (Py_hash_t)qp_hash_buffer(ptr, (size_t)len, _Py_HashSecret.uc);
}

Py_hash_t _Py_HashBytes(const void *src, Py_ssize_t len)
{
    return Py_HashBuffer(src, len);
}

/* object.__hash__ of the object (typeobject.c): the identity of a Python
   value is the address of its JS object, not of the C mirror */
Py_hash_t PyObject_GenericHash(PyObject *obj)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, obj);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return Py_HashPointer(obj);
    }
    return (Py_hash_t)qp_hash_pointer((uintptr_t)qp_obj_addr(v));
}

static PyHash_FuncDef qp_capi_hash_funcdef = { NULL, "siphash13", 64, 128 };

PyHash_FuncDef *PyHash_GetFuncDef(void)
{
    return &qp_capi_hash_funcdef;
}

/* ========================================================================= */
/* PyMem_* (context-free) and PyObject_Malloc (head blocks)                   */
/* ========================================================================= */

/* The allocators of the three domains (CPython _PyMem_Raw, _PyMem and
   _PyObject) are function tables: PyMem_SetAllocator replaces them and
   PyMem_SetupDebugHooks (PYTHONMALLOC=debug) wraps them.  The default RAW
   and MEM allocator is the system malloc (tracemalloc's while tracing,
   qp_capi_tm_tracing); the default OBJ allocator hands out head blocks
   (QPCapiHead + object, qp_capi_obj_*, below), so a hook of the OBJ
   domain passes the blocks of the allocator it wraps through, as CPython's
   hooks do. */

static void *qp_capi_raw_malloc(void *actx, size_t size)
{
    (void)actx;
    if (unlikely(__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED)))
        return qp_capi_tm_raw_alloc(1, size, FALSE);
    return qp_sys_malloc(size ? size : 1);
}

static void *qp_capi_raw_calloc(void *actx, size_t nelem, size_t elsize)
{
    (void)actx;
    if (unlikely(__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED)))
        return qp_capi_tm_raw_alloc(nelem, elsize, TRUE);
    if (nelem == 0 || elsize == 0) {
        nelem = 1;
        elsize = 1;
    }
    return qp_sys_calloc(nelem, elsize);
}

static void *qp_capi_raw_realloc(void *actx, void *ptr, size_t new_size)
{
    (void)actx;
    if (unlikely(__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED)))
        return qp_capi_tm_raw_realloc(ptr, new_size);
    return qp_sys_realloc(ptr, new_size ? new_size : 1);
}

static void qp_capi_raw_free(void *actx, void *ptr)
{
    (void)actx;
    if (unlikely(__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED))) {
        qp_capi_tm_raw_free(ptr);
        return;
    }
    qp_sys_free(ptr);
}

/* malloc() and co. are macros in this translation unit: the members of a
   PyMemAllocatorEx are called through these */
#define QP_CAPI_A_MALLOC(a, n) (((a)->malloc)((a)->ctx, (n)))
#define QP_CAPI_A_CALLOC(a, m, n) (((a)->calloc)((a)->ctx, (m), (n)))
#define QP_CAPI_A_REALLOC(a, p, n) (((a)->realloc)((a)->ctx, (p), (n)))
#define QP_CAPI_A_FREE(a, p) (((a)->free)((a)->ctx, (p)))

static void *qp_capi_obj_malloc(void *actx, size_t size);
static void *qp_capi_obj_calloc(void *actx, size_t nelem, size_t elsize);
static void *qp_capi_obj_realloc(void *actx, void *ptr, size_t new_size);
static void qp_capi_obj_free(void *actx, void *ptr);

static PyMemAllocatorEx qp_capi_allocators[3] = {
    [PYMEM_DOMAIN_RAW] = { NULL, qp_capi_raw_malloc, qp_capi_raw_calloc, qp_capi_raw_realloc,
                           qp_capi_raw_free },
    [PYMEM_DOMAIN_MEM] = { NULL, qp_capi_raw_malloc, qp_capi_raw_calloc, qp_capi_raw_realloc,
                           qp_capi_raw_free },
    [PYMEM_DOMAIN_OBJ] = { NULL, qp_capi_obj_malloc, qp_capi_obj_calloc, qp_capi_obj_realloc,
                           qp_capi_obj_free },
};

/* ---- debug hooks: CPython Objects/obmalloc.c "A x-platform debugging
   allocator" (without PYMEM_DEBUG_SERIALNO).  A RAW or MEM block is
   [SSSS IFFF CCCC...CCCC FFFF]: the requested size (big-endian size_t),
   the API id ('r' / 'm') and SST-1 FORBIDDENBYTEs, the data (CLEANBYTE
   when allocated, DEADBYTE when freed) and SST FORBIDDENBYTEs; a bad pad
   or API id is a fatal error after a dump of the block.  An OBJ block is
   a head block: no prefix, the data is CLEANBYTE when allocated and SST
   FORBIDDENBYTEs follow it; freed objects are poisoned with DEADBYTE
   (qp_capi_memdbg_poison).  The MEM and OBJ hooks check that the caller
   holds the GIL. */

#define QP_MEMDBG_SST ((int)sizeof(size_t))
#define QP_MEMDBG_EXTRA (3 * sizeof(size_t))    /* PYMEM_DEBUG_EXTRA_BYTES */
#define QP_MEMDBG_CLEANBYTE 0xCD                /* PYMEM_CLEANBYTE */
#define QP_MEMDBG_DEADBYTE 0xDD                 /* PYMEM_DEADBYTE */
#define QP_MEMDBG_FORBIDDENBYTE 0xFD            /* PYMEM_FORBIDDENBYTE */

typedef struct QPCapiMemDbgApi {
    char api_id;
    PyMemAllocatorEx alloc;     /* the wrapped allocator */
} QPCapiMemDbgApi;

static QPCapiMemDbgApi qp_capi_memdbg_raw = { 'r' };
static QPCapiMemDbgApi qp_capi_memdbg_mem = { 'm' };
static QPCapiMemDbgApi qp_capi_memdbg_obj = { 'o' };

PyAPI_FUNC(void) _Py_NO_RETURN _Py_FatalErrorFormat(const char *func, const char *format, ...);

/* Read sizeof(size_t) bytes at p as a big-endian size_t. */
static size_t qp_capi_memdbg_read_size(const uint8_t *q)
{
    size_t result = *q++;
    int i;
    for (i = QP_MEMDBG_SST; --i > 0; ++q)
        result = (result << 8) | *q;
    return result;
}

/* Write n as a big-endian size_t at p. */
static void qp_capi_memdbg_write_size(uint8_t *p, size_t n)
{
    uint8_t *q = p + QP_MEMDBG_SST - 1;
    int i;
    for (i = QP_MEMDBG_SST; --i >= 0; --q) {
        *q = (uint8_t)(n & 0xff);
        n >>= 8;
    }
}

/* _PyObject_DebugDumpAddress: display info to stderr about the block at
   p, then the traceback of its allocation (_PyMem_DumpTraceback; the
   blocks are traced at their start, p - 2*SST). */
static void qp_capi_memdbg_dump(const void *p)
{
    const uint8_t *q = (const uint8_t *)p;
    const uint8_t *tail;
    size_t nbytes;
    int i, ok;
    char id;

    fprintf(stderr, "Debug memory block at address p=%p:", p);
    if (p == NULL) {
        fprintf(stderr, "\n");
        return;
    }
    id = (char)q[-QP_MEMDBG_SST];
    fprintf(stderr, " API '%c'\n", id);

    nbytes = qp_capi_memdbg_read_size(q - 2 * QP_MEMDBG_SST);
    fprintf(stderr, "    %zu bytes originally requested\n", nbytes);

    /* In case this is nuts, check the leading pad bytes first. */
    fprintf(stderr, "    The %d pad bytes at p-%d are ", QP_MEMDBG_SST - 1, QP_MEMDBG_SST - 1);
    ok = 1;
    for (i = 1; i <= QP_MEMDBG_SST - 1; ++i) {
        if (*(q - i) != QP_MEMDBG_FORBIDDENBYTE) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        fputs("FORBIDDENBYTE, as expected.\n", stderr);
    } else {
        fprintf(stderr, "not all FORBIDDENBYTE (0x%02x):\n", QP_MEMDBG_FORBIDDENBYTE);
        for (i = QP_MEMDBG_SST - 1; i >= 1; --i) {
            const uint8_t byte = *(q - i);
            fprintf(stderr, "        at p-%d: 0x%02x", i, byte);
            if (byte != QP_MEMDBG_FORBIDDENBYTE)
                fputs(" *** OUCH", stderr);
            fputc('\n', stderr);
        }
        fputs("    Because memory is corrupted at the start, the "
              "count of bytes requested\n"
              "       may be bogus, and checking the trailing pad "
              "bytes may segfault.\n", stderr);
    }

    tail = q + nbytes;
    fprintf(stderr, "    The %d pad bytes at tail=%p are ", QP_MEMDBG_SST, (const void *)tail);
    ok = 1;
    for (i = 0; i < QP_MEMDBG_SST; ++i) {
        if (tail[i] != QP_MEMDBG_FORBIDDENBYTE) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        fputs("FORBIDDENBYTE, as expected.\n", stderr);
    } else {
        fprintf(stderr, "not all FORBIDDENBYTE (0x%02x):\n", QP_MEMDBG_FORBIDDENBYTE);
        for (i = 0; i < QP_MEMDBG_SST; ++i) {
            const uint8_t byte = tail[i];
            fprintf(stderr, "        at tail+%d: 0x%02x", i, byte);
            if (byte != QP_MEMDBG_FORBIDDENBYTE)
                fputs(" *** OUCH", stderr);
            fputc('\n', stderr);
        }
    }

    if (nbytes > 0) {
        i = 0;
        fputs("    Data at p:", stderr);
        /* print up to 8 bytes at the start */
        while (q < tail && i < 8) {
            fprintf(stderr, " %02x", *q);
            ++i;
            ++q;
        }
        /* and up to 8 at the end */
        if (q < tail) {
            if (tail - q > 8) {
                fputs(" ...", stderr);
                q = tail - 8;
            }
            while (q < tail) {
                fprintf(stderr, " %02x", *q);
                ++q;
            }
        }
        fputc('\n', stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
    qp_capi_tm_dump_traceback(fileno(stderr), (const uint8_t *)p - 2 * QP_MEMDBG_SST);
}

/* _PyMem_DebugCheckAddress: the forbidden bytes on both ends of p and its
   API id, else a dump and a fatal error. */
static void qp_capi_memdbg_check(const char *func, char api, const void *p)
{
    const uint8_t *q = (const uint8_t *)p;
    const uint8_t *tail;
    size_t nbytes;
    int i;
    char id;

    id = (char)q[-QP_MEMDBG_SST];
    if (id != api) {
        qp_capi_memdbg_dump(p);
        _Py_FatalErrorFormat(func, "bad ID: Allocated using API '%c', verified using API '%c'",
                             id, api);
    }
    /* the start first: with an underwrite, the size may be nuts */
    for (i = QP_MEMDBG_SST - 1; i >= 1; --i) {
        if (*(q - i) != QP_MEMDBG_FORBIDDENBYTE) {
            qp_capi_memdbg_dump(p);
            _Py_FatalErrorFunc(func, "bad leading pad byte");
        }
    }
    nbytes = qp_capi_memdbg_read_size(q - 2 * QP_MEMDBG_SST);
    tail = q + nbytes;
    for (i = 0; i < QP_MEMDBG_SST; ++i) {
        if (tail[i] != QP_MEMDBG_FORBIDDENBYTE) {
            qp_capi_memdbg_dump(p);
            _Py_FatalErrorFunc(func, "bad trailing pad byte");
        }
    }
}

/* _PyMem_DebugRawAlloc */
static void *qp_capi_memdbg_raw_alloc(int use_calloc, void *actx, size_t nbytes)
{
    QPCapiMemDbgApi *api = actx;
    uint8_t *p, *data, *tail;
    size_t total;

    if (nbytes > (size_t)PY_SSIZE_T_MAX - QP_MEMDBG_EXTRA)
        return NULL;
    total = nbytes + QP_MEMDBG_EXTRA;
    if (use_calloc)
        p = QP_CAPI_A_CALLOC(&api->alloc, 1, total);
    else
        p = QP_CAPI_A_MALLOC(&api->alloc, total);
    if (p == NULL)
        return NULL;
    data = p + 2 * QP_MEMDBG_SST;
    qp_capi_memdbg_write_size(p, nbytes);
    p[QP_MEMDBG_SST] = (uint8_t)api->api_id;
    memset(p + QP_MEMDBG_SST + 1, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST - 1);
    if (nbytes > 0 && !use_calloc)
        memset(data, QP_MEMDBG_CLEANBYTE, nbytes);
    tail = data + nbytes;
    memset(tail, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST);
    return data;
}

static void *qp_capi_memdbg_raw_malloc(void *actx, size_t nbytes)
{
    return qp_capi_memdbg_raw_alloc(0, actx, nbytes);
}

static void *qp_capi_memdbg_raw_calloc(void *actx, size_t nelem, size_t elsize)
{
    return qp_capi_memdbg_raw_alloc(1, actx, nelem * elsize);
}

/* _PyMem_DebugRawFree: checks the pads, fills the block with DEADBYTE,
   then frees it */
static void qp_capi_memdbg_raw_free_named(const char *func, void *actx, void *p)
{
    QPCapiMemDbgApi *api = actx;
    uint8_t *q;
    size_t nbytes;

    if (p == NULL)
        return;
    q = (uint8_t *)p - 2 * QP_MEMDBG_SST;
    qp_capi_memdbg_check(func, api->api_id, p);
    nbytes = qp_capi_memdbg_read_size(q);
    nbytes += QP_MEMDBG_EXTRA - 2 * QP_MEMDBG_SST;
    memset(q, QP_MEMDBG_DEADBYTE, 2 * QP_MEMDBG_SST);
    memset(p, QP_MEMDBG_DEADBYTE, nbytes);
    QP_CAPI_A_FREE(&api->alloc, q);
}

static void qp_capi_memdbg_raw_free(void *actx, void *p)
{
    qp_capi_memdbg_raw_free_named("_PyMem_DebugRawFree", actx, p);
}

/* _PyMem_DebugRawRealloc */
static void *qp_capi_memdbg_raw_realloc(void *actx, void *p, size_t nbytes)
{
    enum { ERASED_SIZE = 64 };
    QPCapiMemDbgApi *api = actx;
    uint8_t *head, *data, *r, *tail;
    uint8_t save[2 * ERASED_SIZE];  /* a copy of the erased bytes */
    size_t total, original_nbytes;

    if (p == NULL)
        return qp_capi_memdbg_raw_alloc(0, actx, nbytes);
    qp_capi_memdbg_check("_PyMem_DebugRawRealloc", api->api_id, p);
    data = (uint8_t *)p;
    head = data - 2 * QP_MEMDBG_SST;
    original_nbytes = qp_capi_memdbg_read_size(head);
    if (nbytes > (size_t)PY_SSIZE_T_MAX - QP_MEMDBG_EXTRA)
        return NULL;
    total = nbytes + QP_MEMDBG_EXTRA;
    tail = data + original_nbytes;
    /* Mark the header, the trailer, ERASED_SIZE bytes at the begin and
       ERASED_SIZE bytes at the end as dead and save the copy of erased
       bytes. */
    if (original_nbytes <= sizeof(save)) {
        memcpy(save, data, original_nbytes);
        memset(data - 2 * QP_MEMDBG_SST, QP_MEMDBG_DEADBYTE, original_nbytes + QP_MEMDBG_EXTRA);
    } else {
        memcpy(save, data, ERASED_SIZE);
        memset(head, QP_MEMDBG_DEADBYTE, ERASED_SIZE + 2 * QP_MEMDBG_SST);
        memcpy(&save[ERASED_SIZE], tail - ERASED_SIZE, ERASED_SIZE);
        memset(tail - ERASED_SIZE, QP_MEMDBG_DEADBYTE,
               ERASED_SIZE + QP_MEMDBG_EXTRA - 2 * QP_MEMDBG_SST);
    }
    /* Resize and add decorations. */
    r = QP_CAPI_A_REALLOC(&api->alloc, head, total);
    if (r == NULL) {
        /* rewrite the header and footer which have just been erased */
        nbytes = original_nbytes;
    } else {
        head = r;
    }
    data = head + 2 * QP_MEMDBG_SST;
    qp_capi_memdbg_write_size(head, nbytes);
    head[QP_MEMDBG_SST] = (uint8_t)api->api_id;
    memset(head + QP_MEMDBG_SST + 1, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST - 1);
    tail = data + nbytes;
    memset(tail, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST);
    /* Restore saved bytes. */
    if (original_nbytes <= sizeof(save)) {
        memcpy(data, save, Py_MIN(nbytes, original_nbytes));
    } else {
        size_t i = original_nbytes - ERASED_SIZE;
        memcpy(data, save, Py_MIN(nbytes, (size_t)ERASED_SIZE));
        if (nbytes > i)
            memcpy(data + i, &save[ERASED_SIZE], Py_MIN(nbytes - i, (size_t)ERASED_SIZE));
    }
    if (r == NULL)
        return NULL;
    if (nbytes > original_nbytes) {
        /* growing: mark new extra memory clean */
        memset(data + original_nbytes, QP_MEMDBG_CLEANBYTE, nbytes - original_nbytes);
    }
    return data;
}

/* _PyMem_DebugCheckGIL: the caller holds the GIL (before the C API is
   initialized and after it is finalized there is no thread state to hold) */
static void qp_capi_memdbg_check_gil(const char *func)
{
    if (!PyGILState_Check())
        _Py_FatalErrorFunc(func, "Python memory allocator called without holding the GIL");
}

/* _PyMem_DebugMalloc / Calloc / Realloc / Free (MEM domain) */
static void *qp_capi_memdbg_mem_malloc(void *actx, size_t nbytes)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugMalloc");
    return qp_capi_memdbg_raw_alloc(0, actx, nbytes);
}

static void *qp_capi_memdbg_mem_calloc(void *actx, size_t nelem, size_t elsize)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugCalloc");
    return qp_capi_memdbg_raw_alloc(1, actx, nelem * elsize);
}

static void *qp_capi_memdbg_mem_realloc(void *actx, void *p, size_t nbytes)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugRealloc");
    return qp_capi_memdbg_raw_realloc(actx, p, nbytes);
}

static void qp_capi_memdbg_mem_free(void *actx, void *p)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugFree");
    qp_capi_memdbg_raw_free_named("_PyMem_DebugRawFree", actx, p);
}

/* The OBJ domain hooks (head blocks, see above). */
static void *qp_capi_memdbg_obj_alloc(int use_calloc, void *actx, size_t nbytes)
{
    QPCapiMemDbgApi *api = actx;
    uint8_t *data;

    if (nbytes > (size_t)PY_SSIZE_T_MAX - QP_MEMDBG_SST)
        return NULL;
    if (use_calloc)
        data = QP_CAPI_A_CALLOC(&api->alloc, 1, nbytes + QP_MEMDBG_SST);
    else
        data = QP_CAPI_A_MALLOC(&api->alloc, nbytes + QP_MEMDBG_SST);
    if (data == NULL)
        return NULL;
    if (!use_calloc)
        memset(data, QP_MEMDBG_CLEANBYTE, nbytes);
    memset(data + nbytes, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST);
    return data;
}

static void *qp_capi_memdbg_obj_malloc(void *actx, size_t nbytes)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugMalloc");
    return qp_capi_memdbg_obj_alloc(0, actx, nbytes);
}

static void *qp_capi_memdbg_obj_calloc(void *actx, size_t nelem, size_t elsize)
{
    qp_capi_memdbg_check_gil("_PyMem_DebugCalloc");
    return qp_capi_memdbg_obj_alloc(1, actx, nelem * elsize);
}

static void *qp_capi_memdbg_obj_realloc(void *actx, void *p, size_t nbytes)
{
    QPCapiMemDbgApi *api = actx;
    uint8_t *data;

    qp_capi_memdbg_check_gil("_PyMem_DebugRealloc");
    if (p == NULL)
        return qp_capi_memdbg_obj_alloc(0, actx, nbytes);
    if (nbytes > (size_t)PY_SSIZE_T_MAX - QP_MEMDBG_SST)
        return NULL;
    data = QP_CAPI_A_REALLOC(&api->alloc, p, nbytes + QP_MEMDBG_SST);
    if (data)
        memset(data + nbytes, QP_MEMDBG_FORBIDDENBYTE, QP_MEMDBG_SST);
    return data;
}

static void qp_capi_memdbg_obj_free(void *actx, void *p)
{
    QPCapiMemDbgApi *api = actx;
    qp_capi_memdbg_check_gil("_PyMem_DebugFree");
    QP_CAPI_A_FREE(&api->alloc, p);
}

/* A head block is freed while the OBJ hook is installed: the object part
   is filled with DEADBYTE (_PyMem_DebugRawFree), so that _PyObject_IsFreed
   sees it. */
static void qp_capi_memdbg_poison(JSRuntime *rt, QPCapiHead *h)
{
    size_t n = js_malloc_usable_size_rt(rt, h);
    if (n > sizeof(QPCapiHead))
        memset(QP_CAPI_OBJ(h), QP_MEMDBG_DEADBYTE, n - sizeof(QPCapiHead));
}

/* PYTHONMALLOC (ignored with -E and -I) is read before the first block of
   any domain is allocated: "debug" and the "<allocator>_debug" names
   install the debug hooks; there is no pymalloc or mimalloc here, the
   other names keep the system malloc. */
static int qp_capi_mem_configured;

static void qp_capi_mem_configure(void)
{
    JSContext *ctx = qp_capi.ctx;
    const char *s = NULL;
    size_t n;

    qp_capi_mem_configured = 1;
    if (!(ctx && ctx->py && (QP_STATE(ctx)->config.ignore_environment ||
                             QP_STATE(ctx)->config.isolated)))
        s = getenv("PYTHONMALLOC");
    if (!s)
        return;
    n = strlen(s);
    if (strcmp(s, "debug") == 0 || (n > 6 && strcmp(s + n - 6, "_debug") == 0))
        PyMem_SetupDebugHooks();
}

#define QP_CAPI_MEM_CONFIGURE()                   \
    do {                                          \
        if (unlikely(!qp_capi_mem_configured))    \
            qp_capi_mem_configure();              \
    } while (0)

void *PyMem_RawMalloc(size_t size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_MALLOC(&qp_capi_allocators[PYMEM_DOMAIN_RAW], size);
}

void *PyMem_RawCalloc(size_t nelem, size_t elsize)
{
    QP_CAPI_MEM_CONFIGURE();
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return QP_CAPI_A_CALLOC(&qp_capi_allocators[PYMEM_DOMAIN_RAW], nelem, elsize);
}

void *PyMem_RawRealloc(void *ptr, size_t new_size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_REALLOC(&qp_capi_allocators[PYMEM_DOMAIN_RAW], ptr, new_size);
}

void PyMem_RawFree(void *ptr)
{
    QP_CAPI_MEM_CONFIGURE();
    QP_CAPI_A_FREE(&qp_capi_allocators[PYMEM_DOMAIN_RAW], ptr);
}

void *PyMem_Malloc(size_t size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_MALLOC(&qp_capi_allocators[PYMEM_DOMAIN_MEM], size);
}

void *PyMem_Calloc(size_t nelem, size_t elsize)
{
    QP_CAPI_MEM_CONFIGURE();
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return QP_CAPI_A_CALLOC(&qp_capi_allocators[PYMEM_DOMAIN_MEM], nelem, elsize);
}

void *PyMem_Realloc(void *ptr, size_t new_size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_REALLOC(&qp_capi_allocators[PYMEM_DOMAIN_MEM], ptr, new_size);
}

void PyMem_Free(void *ptr)
{
    QP_CAPI_MEM_CONFIGURE();
    QP_CAPI_A_FREE(&qp_capi_allocators[PYMEM_DOMAIN_MEM], ptr);
}

char *_PyMem_Strdup(const char *str)
{
    size_t n = strlen(str) + 1;
    char *copy = PyMem_Malloc(n);
    if (!copy)
        return NULL;
    memcpy(copy, str, n);
    return copy;
}

char *_PyMem_RawStrdup(const char *str)
{
    size_t n = strlen(str) + 1;
    char *copy = PyMem_RawMalloc(n);
    if (!copy)
        return NULL;
    memcpy(copy, str, n);
    return copy;
}

void PyMem_GetAllocator(PyMemAllocatorDomain domain, PyMemAllocatorEx *allocator)
{
    QP_CAPI_MEM_CONFIGURE();
    if ((unsigned)domain <= PYMEM_DOMAIN_OBJ)
        *allocator = qp_capi_allocators[domain];
    else
        memset(allocator, 0, sizeof(*allocator));
}

void PyMem_SetAllocator(PyMemAllocatorDomain domain, PyMemAllocatorEx *allocator)
{
    QP_CAPI_MEM_CONFIGURE();
    if ((unsigned)domain <= PYMEM_DOMAIN_OBJ)
        qp_capi_allocators[domain] = *allocator;
}

/* PyMem_SetupDebugHooks: wrap the allocator of each domain (once) */
void PyMem_SetupDebugHooks(void)
{
    PyMemAllocatorEx *a;

    qp_capi_mem_configured = 1;
    a = &qp_capi_allocators[PYMEM_DOMAIN_RAW];
    if (a->malloc != qp_capi_memdbg_raw_malloc) {
        qp_capi_memdbg_raw.alloc = *a;
        *a = (PyMemAllocatorEx){ &qp_capi_memdbg_raw, qp_capi_memdbg_raw_malloc,
                                 qp_capi_memdbg_raw_calloc, qp_capi_memdbg_raw_realloc,
                                 qp_capi_memdbg_raw_free };
    }
    a = &qp_capi_allocators[PYMEM_DOMAIN_MEM];
    if (a->malloc != qp_capi_memdbg_mem_malloc) {
        qp_capi_memdbg_mem.alloc = *a;
        *a = (PyMemAllocatorEx){ &qp_capi_memdbg_mem, qp_capi_memdbg_mem_malloc,
                                 qp_capi_memdbg_mem_calloc, qp_capi_memdbg_mem_realloc,
                                 qp_capi_memdbg_mem_free };
    }
    a = &qp_capi_allocators[PYMEM_DOMAIN_OBJ];
    if (a->malloc != qp_capi_memdbg_obj_malloc) {
        qp_capi_memdbg_obj.alloc = *a;
        *a = (PyMemAllocatorEx){ &qp_capi_memdbg_obj, qp_capi_memdbg_obj_malloc,
                                 qp_capi_memdbg_obj_calloc, qp_capi_memdbg_obj_realloc,
                                 qp_capi_memdbg_obj_free };
        qp_capi_memdbg_obj_on = 1;
    }
}

/* _PyMem_IsPtrFreed (pycore_pymem.h): NULL or a debug hook fill pattern */
static inline int qp_capi_mem_is_ptr_freed(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
#if UINTPTR_MAX > 0xFFFFFFFFu
    return (value == 0 || value == (uintptr_t)0xCDCDCDCDCDCDCDCDull ||
            value == (uintptr_t)0xDDDDDDDDDDDDDDDDull || value == (uintptr_t)0xFDFDFDFDFDFDFDFDull ||
            value == (uintptr_t)0xFFFFFFFFFFFFFFFFull);
#else
    return (value == 0 || value == 0xCDCDCDCDu || value == 0xDDDDDDDDu ||
            value == 0xFDFDFDFDu || value == 0xFFFFFFFFu);
#endif
}

/* Objects/object.c (pycore_object.h): the object memory looks freed */
PyAPI_FUNC(int) _PyObject_IsFreed(PyObject *op);
int _PyObject_IsFreed(PyObject *op)
{
    if (qp_capi_mem_is_ptr_freed(op) || qp_capi_mem_is_ptr_freed(Py_TYPE(op)))
        return 1;
    return 0;
}

/* _testinternalcapi.check_pyobject_{null,uninitialized,forbidden_bytes,
   freed}_is_freed (which 0..3, mod_testinternalcapi.c): objects that the
   debug hooks make look freed (the blocks are leaked on purpose, the
   caller exits at once). */
static JSValue qp_capi_tic_check_pyobject_is_freed(JSContext *ctx, int which)
{
    PyObject *op = NULL;

    if (qp_capi_init(ctx) < 0)
        return JS_EXCEPTION;
    switch (which) {
    case 0:
        break;
    case 1:
        /* object fields like ob_type are uninitialized */
        op = PyObject_Malloc(sizeof(PyObject));
        if (op == NULL)
            return qp_raise_memory_error(ctx);
        op->ob_refcnt = 1;
        break;
    case 2:
        /* ob_type is after the block: part of the forbidden bytes */
        op = PyObject_Malloc(offsetof(PyObject, ob_type));
        if (op == NULL)
            return qp_raise_memory_error(ctx);
        op->ob_refcnt = 1;
        break;
    default:
        op = PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
        if (op == NULL)
            return JS_EXCEPTION;
        Py_TYPE(op)->tp_dealloc(op);
        /* the object memory is freed */
        op->ob_refcnt = 1;
        break;
    }
    if (!_PyObject_IsFreed(op))
        return qp_raise(ctx, QP_EXC(ctx, AssertionError), "object is not seen as freed");
    return JS_UNDEFINED;
}

/* the default OBJ allocator: head blocks (zeroed) */
static void *qp_capi_obj_malloc(void *actx, size_t size)
{
    QPCapiHead *h = qp_capi_block_alloc(size ? size : 1);
    (void)actx;
    if (!h)
        return NULL;
    h->tag = QP_CAPI_TAG_RAW;
    h->kind = QP_CAPI_K_RAW;
    return QP_CAPI_OBJ(h);
}

static void *qp_capi_obj_calloc(void *actx, size_t nelem, size_t elsize)
{
    return qp_capi_obj_malloc(actx, nelem * elsize);    /* blocks are zeroed */
}

static void *qp_capi_obj_realloc(void *actx, void *ptr, size_t new_size)
{
    QPCapiHead *h, *nh;
    size_t old_total;
    BOOL live, weak, mapped;
    if (!ptr)
        return qp_capi_obj_malloc(actx, new_size);
    if (!new_size)
        new_size = 1;
    if (new_size > (size_t)PY_SSIZE_T_MAX - sizeof(QPCapiHead))
        return NULL;
    h = QP_CAPI_HEAD(ptr);
    old_total = js_malloc_usable_size_rt(qp_capi.rt, h);
    live = (h->flags & QP_CAPI_F_LIVE) != 0;
    weak = !live && !list_empty(&h->live_link);
    mapped = h->tag == QP_CAPI_TAG_OBJECT && !QP_IS_NULL(h->v);
    /* the head is moved: detach it from the lists and maps first */
    if (live)
        qp_capi_node_unlink(h);
    else if (weak)
        qp_capi_unlist(h);
    if (mapped)
        qp_capi_unregister(h);
    nh = js_realloc_rt(qp_capi.rt, h, sizeof(QPCapiHead) + new_size);
    if (!nh) {
        /* the old block is unchanged: restore it */
        init_list_head(&h->gc.link);
        init_list_head(&h->live_link);
        if (live) {
            h->flags |= QP_CAPI_F_LIVE;
            qp_capi_node_link(h);
        } else if (weak) {
            qp_capi_weak_link(h);
        }
        if (mapped)
            qp_capi_register(h);
        return NULL;
    }
    if (sizeof(QPCapiHead) + new_size > old_total)
        memset((uint8_t *)nh + old_total, 0, sizeof(QPCapiHead) + new_size - old_total);
    init_list_head(&nh->gc.link);
    init_list_head(&nh->live_link);
    if (live) {
        nh->flags |= QP_CAPI_F_LIVE;
        qp_capi_node_link(nh);
    } else if (weak) {
        qp_capi_weak_link(nh);
    }
    if (mapped)
        qp_capi_register(nh);
    return QP_CAPI_OBJ(nh);
}

static void qp_capi_obj_free(void *actx, void *ptr)
{
    QPCapiHead *h;
    (void)actx;
    if (!ptr)
        return;
    h = QP_CAPI_HEAD(ptr);
    if (qp_capi_dead || !qp_capi.rt)
        return;             /* runtime gone: leaked */
    if (h->tag == QP_CAPI_TAG_RAW) {
        h->tag = QP_CAPI_TAG_FREED;
        if (unlikely(qp_capi_memdbg_obj_on))
            qp_capi_memdbg_poison(qp_capi.rt, h);
        js_free_rt(qp_capi.rt, h);
        return;
    }
    if (h->tag != QP_CAPI_TAG_OBJECT)
        return;             /* double free: ignored */
    qp_capi_release(qp_capi.rt, h);
}

void *PyObject_Malloc(size_t size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_MALLOC(&qp_capi_allocators[PYMEM_DOMAIN_OBJ], size);
}

void *PyObject_Calloc(size_t nelem, size_t elsize)
{
    QP_CAPI_MEM_CONFIGURE();
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return QP_CAPI_A_CALLOC(&qp_capi_allocators[PYMEM_DOMAIN_OBJ], nelem, elsize);
}

void *PyObject_Realloc(void *ptr, size_t new_size)
{
    QP_CAPI_MEM_CONFIGURE();
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return QP_CAPI_A_REALLOC(&qp_capi_allocators[PYMEM_DOMAIN_OBJ], ptr, new_size);
}

void PyObject_Free(void *ptr)
{
    QP_CAPI_MEM_CONFIGURE();
    QP_CAPI_A_FREE(&qp_capi_allocators[PYMEM_DOMAIN_OBJ], ptr);
}

/* ========================================================================= */
/* Object initialization and GC tracking (objimpl.h)                          */
/* ========================================================================= */

static QPCapiKind qp_capi_c_primary_kind(PyTypeObject *type);   /* capi_type.c */

PyObject *PyObject_Init(PyObject *op, PyTypeObject *tp)
{
    QPCapiHead *h;
    if (!op)
        return PyErr_NoMemory();
    h = QP_CAPI_HEAD(op);
    if (h->tag == QP_CAPI_TAG_RAW) {
        h->tag = QP_CAPI_TAG_OBJECT;
        h->kind = qp_capi_c_primary_kind(tp);
        /* float / complex subtypes (numpy's gentype_alloc) are C-primary
           mirrors of their builtin kind, NATIVE only with extra C fields */
        if (h->kind == QP_CAPI_K_NATIVE ||
            (size_t)tp->tp_basicsize > qp_capi_kind_default_size[h->kind])
            h->flags |= QP_CAPI_F_NATIVE;
    }
    op->ob_flags = 0;
    op->ob_type = tp;
    if (QP_CAPI_HAS_HEAD(tp))
        Py_INCREF(tp);
    op->ob_refcnt = 1;
    if (!(h->flags & QP_CAPI_F_LIVE)) {
        h->flags |= QP_CAPI_F_LIVE;
        qp_capi_node_link(h);
    }
    return op;
}

PyVarObject *PyObject_InitVar(PyVarObject *op, PyTypeObject *tp, Py_ssize_t size)
{
    if (!op)
        return (PyVarObject *)PyErr_NoMemory();
    PyObject_Init((PyObject *)op, tp);
    op->ob_size = size;
    return op;
}

PyObject *_PyObject_New(PyTypeObject *tp)
{
    PyObject *op = PyObject_Malloc(_PyObject_SIZE(tp));
    if (!op)
        return PyErr_NoMemory();
    return PyObject_Init(op, tp);
}

PyVarObject *_PyObject_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    PyVarObject *op;
    size_t size = _PyObject_VAR_SIZE(tp, nitems);
    op = PyObject_Malloc(size);
    if (!op)
        return (PyVarObject *)PyErr_NoMemory();
    return PyObject_InitVar(op, tp, nitems);
}

PyObject *_PyObject_GC_New(PyTypeObject *tp)
{
    return _PyObject_New(tp);
}

PyVarObject *_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyObject_NewVar(tp, nitems);
}

PyObject *PyUnstable_Object_GC_NewWithExtraData(PyTypeObject *tp, size_t extra_size)
{
    PyObject *op = PyObject_Malloc(_PyObject_SIZE(tp) + extra_size);
    if (!op)
        return PyErr_NoMemory();
    return PyObject_Init(op, tp);
}

PyVarObject *_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    size_t size = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    PyVarObject *nop;
    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    nop = PyObject_Realloc(op, size);
    if (!nop)
        return (PyVarObject *)PyErr_NoMemory();
    nop->ob_size = nitems;
    return nop;
}

void PyObject_GC_Del(void *op)
{
    PyObject_Free(op);
}

void PyObject_GC_Track(void *op)
{
    PyObject *o = op;
    if (!QP_CAPI_HAS_HEAD(o))
        return;
    QP_CAPI_HEAD(o)->flags |= QP_CAPI_F_TRACKED;
}

void PyObject_GC_UnTrack(void *op)
{
    PyObject *o = op;
    if (!QP_CAPI_HAS_HEAD(o))
        return;
    QP_CAPI_HEAD(o)->flags &= ~QP_CAPI_F_TRACKED;
}

int PyObject_GC_IsTracked(PyObject *op)
{
    if (!QP_CAPI_HAS_HEAD(op))
        return 0;
    if (QP_CAPI_HEAD(op)->flags & QP_CAPI_F_TRACKED)
        return 1;
    /* J-backed mirrors are tracked by the JS collector */
    return !QP_IS_NULL(QP_CAPI_HEAD(op)->v) && qp_capi_v_is_object(QP_CAPI_HEAD(op)->v);
}

int PyObject_GC_IsFinalized(PyObject *op)
{
    if (!QP_CAPI_HAS_HEAD(op))
        return 0;
    return (QP_CAPI_HEAD(op)->flags & QP_CAPI_F_FINALIZED) != 0;
}

int (PyObject_IS_GC)(PyObject *obj)
{
    return PyType_IS_GC(Py_TYPE(obj)) &&
        (Py_TYPE(obj)->tp_is_gc == NULL || Py_TYPE(obj)->tp_is_gc(obj));
}

static int qp_capi_gc_enabled = 1;

Py_ssize_t PyGC_Collect(void)
{
    if (!qp_capi.rt)
        return 0;
    JS_RunGC(qp_capi.rt);
    return 0;
}

int PyGC_Enable(void)
{
    int old = qp_capi_gc_enabled;
    qp_capi_gc_enabled = 1;
    return old;
}

int PyGC_Disable(void)
{
    int old = qp_capi_gc_enabled;
    qp_capi_gc_enabled = 0;
    return old;
}

int PyGC_IsEnabled(void)
{
    return qp_capi_gc_enabled;
}

/* ========================================================================= */
/* Init (step A of qp_capi_init) and qp_capi_init itself                     */
/* ========================================================================= */

static int qp_capi_init_mirror(JSContext *ctx)
{
    QPState *qs = QP_STATE(ctx);
    if (qp_capi.obj_map)
        return 0;       /* idempotent */
    qp_capi.ctx = ctx;
    qp_capi.rt = JS_GetRuntime(ctx);
    qp_capi_dead = 0;
    init_list_head(&qp_capi.live_list);
    init_list_head(&qp_capi.weak_list);
    qp_capi.obj_map = qp_capi_map_new();
    qp_capi.str_map = qp_capi_map_new();
    qp_capi.id_map = qp_capi_map_new();
    qp_capi.static_map = qp_capi_map_new();
    if (!qp_capi.obj_map || !qp_capi.str_map || !qp_capi.id_map || !qp_capi.static_map) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    qp_capi.kind_ops[QP_CAPI_K_RAW].name = "raw";
    qp_capi.kind_ops[QP_CAPI_K_OBJECT].name = "object";
    /* _Py_HashSecret: the first 16 bytes are the SipHash key of str/bytes
       hashes; the expat salt is derived from it */
    memset(&_Py_HashSecret, 0, sizeof(_Py_HashSecret));
    memcpy(_Py_HashSecret.uc, qs->hash_key, 16);
    _Py_HashSecret.expat.hashsalt =
        (Py_hash_t)qp_keyed_hash(0x6578706174ull, qs->hash_key, 16);
    return 0;
}

static int qp_capi_init(JSContext *ctx)
{
    int state;

    pthread_mutex_lock(&qp_capi_bind_lock);
    if ((qp_capi_owner && qp_capi_owner != ctx->rt) ||
        (qp_capi.ctx && ctx != qp_capi.ctx)) {
        /* another runtime (maybe running in another thread) or another
           context of this runtime has the C API */
        pthread_mutex_unlock(&qp_capi_bind_lock);
        qp_raise(ctx, QP_EXC(ctx, ImportError),
                 "C extension modules are bound to another interpreter");
        return -1;
    }
    state = qp_capi.initialized;
    if (state == 0) {
        __atomic_store_n(&qp_capi_owner, ctx->rt, __ATOMIC_RELEASE);
        qp_capi.initialized = 1;
    }
    pthread_mutex_unlock(&qp_capi_bind_lock);
    if (state != 0)
        return 0;       /* ready, or being initialized by this context */
    if (qp_capi_init_mirror(ctx) < 0 ||
        qp_capi_init_types(ctx) < 0 ||
        qp_capi_init_numbers(ctx) < 0 ||
        qp_capi_init_str(ctx) < 0 ||
        qp_capi_init_containers(ctx) < 0 ||
        qp_capi_init_errors(ctx) < 0 ||
        qp_capi_init_runtime(ctx) < 0 ||
        qp_capi_init_stable(ctx) < 0) {
        pthread_mutex_lock(&qp_capi_bind_lock);
        qp_capi.initialized = 0;
        /* still bound once the mirror state exists: qp_capi_context_free
           releases it */
        if (!qp_capi.ctx)
            __atomic_store_n(&qp_capi_owner, NULL, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&qp_capi_bind_lock);
        return -1;
    }
    qp_capi.initialized = 2;
    return 0;
}
