/*
 * dquickts Python, milestone M4: heap snapshots of the Python runtime.
 *
 * JS_WriteSnapshot() / JS_ReadSnapshot() (quickjs-snapshot.c) serialise
 * the whole runtime, Python included (D3: one runtime).  quickjs-snapshot.c
 * is included before the Python units, so it only calls the hooks defined
 * here (declared at its top, under QP_SNAPSHOT):
 *
 *   qp_snap_add_natives     registers the static Python tables (specs,
 *                           method/getset/member tables, module defs,
 *                           slotdefs and every C slot function they hold)
 *                           as snapshot natives and hashes their layout
 *                           into the build signature
 *   qp_snap_init_runtime    registers the Python JS classes (and the
 *                           weakref holder host class) on the reader runtime
 *   qp_snap_write_payload / qp_snap_read_payload
 *                           payload of every JS_CLASS_PY_* object and of
 *                           the weakref holder class (1 = not Python)
 *   qp_snap_write_state / qp_snap_read_state
 *                           JSContext.py (QPState and its QPModState)
 *   qp_snap_write_code / qp_snap_read_code
 *                           JSFunctionBytecode.is_py / py_code (QPCodeInfo)
 *   qp_snap_finish          deferred work once the heap is complete
 *   qp_snap_reader_free     reader state
 *
 * Identity: builtin types, exception types, singletons and interned atoms
 * are ordinary cells of the snapshot referenced from the restored QPState,
 * so `None is None`, `type(1) is int` and QP_ATOM() hold after a restore.
 * C pointers are never written: static data and code go through the native
 * registry (index of the registered pointer), QPType* fields through the
 * reference of their type object.
 *
 * Address-dependent state: identity hashes change with the object
 * addresses, so the parts that store hashes of arbitrary keys re-validate
 * them in deferred functions (qp_snap_defer), which may run Python code
 * (__hash__).  Caches (method cache, inline caches, str cache, hash caches)
 * are restored empty.
 *
 * C-extension state (D5, python/capi_snapshot.c): C modules are saved as
 * module objects and executed again on read, C types and the classes a C
 * module makes are saved by reference (the re-executed module re-creates
 * them into the saved type objects), C functions and descriptors keep
 * their objects with a reference (owner, name) to their C definition, and
 * C-extension instances are saved as their __reduce_ex__() value
 * (records, written instead of the object, read into a JS_CLASS_PY_NATIVE
 * placeholder that becomes the reconstructed object).
 *
 * Host state is rejected (TypeError "snapshot: cannot save Python object
  * ...): OS threads other than the main thread (what names the main thread
 * is bound to the main thread of the reader, qp_snap_thread_bound),
 * directory streams, poll and kqueue objects, file
 * objects on descriptors other than the standard streams, sockets, processes,
 * C-extension objects that cannot be pickled, running Python code.
 * Process-level state that Python keeps in the heap is re-applied on read:
 * signal handlers (sigaction) and a disabled gc.
 *
 * The payloads of the other classes are in the part files included below
 * (python/snapshot_<part>.c); local://qp-snap-api.md describes the helper
 * API they use.
 */

#include "snapshot_tables.h"

/* ========================================================================= */
/* Helpers shared with the part files                                        */
/* ========================================================================= */

static int qp_snap_put_i64(SnapWriter *w, int64_t v)
{
    bc_put_u64(&w->bc, (uint64_t)v);
    return 0;
}

static int qp_snap_get_i64(SnapReader *r, int64_t *pv)
{
    uint64_t u;
    if (bc_get_u64(&r->bc, &u))
        return -1;
    *pv = (int64_t)u;
    return 0;
}

static int qp_snap_put_f64(SnapWriter *w, double d)
{
    JSFloat64Union u;
    u.d = d;
    bc_put_u64(&w->bc, u.u64);
    return 0;
}

static int qp_snap_get_f64(SnapReader *r, double *pd)
{
    JSFloat64Union u;
    if (bc_get_u64(&r->bc, &u.u64))
        return -1;
    *pd = u.d;
    return 0;
}

static int qp_snap_get_int(SnapReader *r, int *pv)
{
    int32_t v;
    if (bc_get_sleb128(&r->bc, &v))
        return -1;
    *pv = v;
    return 0;
}

static int qp_snap_put_type(SnapWriter *w, QPType *t)
{
    return snap_put_obj(w, t ? t->obj : NULL);
}

/* QPType of the type object p, allocated empty (finalizer-safe, like
   qp_type_alloc) when its body is not read yet: references to a type can
   be resolved before the type itself is read. */
static QPType *qp_snap_type_ensure(SnapReader *r, JSObject *p)
{
    QPTypeExt *e;
    QPType *t;

    if (p->u.opaque)
        return p->u.opaque;
    e = js_mallocz(r->ctx, sizeof(*e));
    if (!e)
        return NULL;
    t = &e->t;
    t->obj = p;
    t->name = JS_ATOM_NULL;
    t->qualname = JS_UNDEFINED;
    t->bases = JS_UNDEFINED;
    t->mro = JS_UNDEFINED;
    t->dict = JS_UNDEFINED;
    t->slot_names = JS_UNDEFINED;
    t->instance_class_id = JS_CLASS_PY_OBJECT;
    e->doc = QP_NULL;
    p->u.opaque = t;
    return t;
}

static int qp_snap_get_type(SnapReader *r, QPType **pt)
{
    JSObject *p;

    *pt = NULL;
    if (snap_get_obj(r, &p))
        return -1;
    if (!p)
        return 0;
    if (p->class_id != JS_CLASS_PY_TYPE)
        return snap_error(r, "invalid Python type reference");
    *pt = qp_snap_type_ensure(r, p);
    return *pt ? 0 : -1;
}

/* name of the Python type of p for error messages (no Python code runs) */
static const char *qp_snap_type_name(JSRuntime *rt, JSObject *p, char *buf, int buf_size)
{
    JSObject *proto = p->shape ? p->shape->proto : NULL;
    if (proto && proto->class_id == JS_CLASS_PY_TYPE && proto->u.opaque) {
        QPType *t = proto->u.opaque;
        if (t->tp_name)
            return t->tp_name;
        if (t->name != JS_ATOM_NULL)
            return JS_AtomGetStrRT(rt, buf, buf_size, t->name);
    }
    return JS_AtomGetStrRT(rt, buf, buf_size, rt->class_array[p->class_id].class_name);
}

static int qp_snap_reject(SnapWriter *w, JSObject *p, const char *reason)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    JS_ThrowTypeError(w->ctx, "snapshot: cannot save Python object '%s': %s",
                      qp_snap_type_name(w->rt, p, buf, sizeof(buf)), reason);
    return -1;
}

/* ---- deferred work (reader) --------------------------------------------- */

typedef int QPSnapDeferFunc(JSContext *ctx, JSObject *p);

typedef struct QPSnapDefer {
    QPSnapDeferFunc *func;
    JSObject *obj;              /* borrowed: the reader keeps every cell alive */
} QPSnapDefer;

typedef struct QPSnapRead {
    JSContext *py_ctx;          /* first restored context with Python state */
    QPSnapDefer *early, *late;
    int early_count, early_size;
    int late_count, late_size;
    struct QPCapiSnapRead *capi; /* C-extension state (capi_snapshot.c) */
    JSObject **thread_bound;    /* _thread objects bound to the main thread */
    int thread_bound_count, thread_bound_size;
    BOOL thread_rebound;        /* the main _ThreadHandle was rebound: */
    uint64_t thread_old_ident;  /* the ident of the writer's main thread */
} QPSnapRead;

/* ---- C-extension state (python/capi_snapshot.c, included later) -------- */

/* kinds of C definitions referenced by functions and descriptors */
enum {
    QP_SNAP_DEF_METHOD,         /* const QPMethodDef * */
    QP_SNAP_DEF_GETSET,         /* const QPGetSetDef * */
    QP_SNAP_DEF_WRAPPED,        /* wrapped C slot function */
};

#ifdef CONFIG_PYTHON_CAPI
static int qp_capi_snap_add_natives(JSRuntime *rt, SnapNatives *n);
static int qp_capi_snap_write_record(SnapWriter *w, JSObject *p);
static int qp_capi_snap_read_record(SnapReader *r, JSObject *p);
static int qp_capi_snap_write_type_ref(SnapWriter *w, JSObject *p);
static int qp_capi_snap_read_type_ref(SnapReader *r, JSObject *p);
static int qp_capi_snap_put_def(SnapWriter *w, const void *def, int kind);
static BOOL qp_capi_snap_is_capi_def(SnapWriter *w, const void *def, int kind);
static int qp_capi_snap_type_skip_sub(SnapWriter *w, QPType *sub);
static int qp_capi_snap_get_def(SnapReader *r, const void **pdef, int kind);
struct QPModule;
static int qp_capi_snap_module_write(SnapWriter *w, struct QPModule *m);
static int qp_capi_snap_module_read(SnapReader *r, JSObject *p);
static int qp_capi_snap_finish(SnapReader *r, JSContext *ctx);
static void qp_capi_snap_writer_free(SnapWriter *w);
static void qp_capi_snap_reader_free(SnapReader *r);
#endif

/* Reference to static C data or code of a function/descriptor payload:
   u8 0 + native index, or (C API) u8 1 + C definition reference. */
static int qp_snap_put_def(SnapWriter *w, const void *def, int kind)
{
#ifdef CONFIG_PYTHON_CAPI
    if (def && !snap_map_find(&w->natives.map, def) && qp_capi_snap_is_capi_def(w, def, kind)) {
        bc_put_u8(&w->bc, 1);
        return qp_capi_snap_put_def(w, def, kind);
    }
#endif
    bc_put_u8(&w->bc, 0);
    return snap_put_native(w, def);
}

/* *pdef: the definition, or NULL until the C modules are restored (C
   definition reference); *pref tells which. */
static int qp_snap_get_def(SnapReader *r, const void **pdef, int kind, BOOL *pref)
{
    uint8_t tag;
    void *ptr;

    *pref = FALSE;
    if (bc_get_u8(&r->bc, &tag))
        return -1;
    if (tag == 0) {
        if (snap_get_native(r, &ptr))
            return -1;
        *pdef = ptr;
        return 0;
    }
#ifdef CONFIG_PYTHON_CAPI
    if (tag == 1) {
        *pref = TRUE;
        *pdef = NULL;
        return qp_capi_snap_get_def(r, pdef, kind);
    }
#endif
    return snap_error(r, "invalid C definition reference");
}

/* snap_write_object() hook: C-extension objects are written as records
   (1 = not one).  A cell keeps its form in every snapshot of a series. */
static int qp_snap_write_record(SnapWriter *w, JSObject *p)
{
    int ret = 1;
#ifdef CONFIG_PYTHON_CAPI
    ret = qp_capi_snap_write_record(w, p);
#endif
    if (ret == 1 && w->cur_idx < w->base_cell_count &&
        snap_writer_cell(w, w->cur_idx)->py_record) {
        JS_ThrowTypeError(w->ctx, "snapshot: a C extension object of the base snapshot "
                          "changed its class; write a full snapshot");
        return -1;
    }
    return ret;
}

static void qp_snap_writer_free(SnapWriter *w)
{
#ifdef CONFIG_PYTHON_CAPI
    qp_capi_snap_writer_free(w);
#endif
}

static QPSnapRead *qp_snap_read_state_get(SnapReader *r)
{
    if (!r->py) {
        r->py = js_mallocz_rt(r->rt, sizeof(QPSnapRead));
        if (!r->py)
            JS_ThrowOutOfMemory(r->ctx);
    }
    return r->py;
}

static int qp_snap_defer_add(SnapReader *r, BOOL early, JSObject *p, QPSnapDeferFunc *fn)
{
    QPSnapRead *s = qp_snap_read_state_get(r);
    QPSnapDefer **ptab;
    int *pcount, *psize;

    if (!s)
        return -1;
    ptab = early ? &s->early : &s->late;
    pcount = early ? &s->early_count : &s->late_count;
    psize = early ? &s->early_size : &s->late_size;
    if (snap_resize(r->rt, (void **)ptab, sizeof(**ptab), psize, *pcount + 1)) {
        JS_ThrowOutOfMemory(r->ctx);
        return -1;
    }
    (*ptab)[*pcount].func = fn;
    (*ptab)[*pcount].obj = p;
    (*pcount)++;
    return 0;
}

/* fn(ctx, p) runs once every body is read and the object flags are
   restored, in registration order; it may run Python code. */
static int qp_snap_defer(SnapReader *r, JSObject *p, QPSnapDeferFunc *fn)
{
    return qp_snap_defer_add(r, FALSE, p, fn);
}

/* Same, but every early function runs before the first qp_snap_defer one
   (pointer fix-ups that hashing or comparisons may depend on). */
static int qp_snap_defer_early(SnapReader *r, JSObject *p, QPSnapDeferFunc *fn)
{
    return qp_snap_defer_add(r, TRUE, p, fn);
}

static void qp_snap_reader_free(SnapReader *r)
{
    QPSnapRead *s = r->py;
    if (!s)
        return;
#ifdef CONFIG_PYTHON_CAPI
    qp_capi_snap_reader_free(r);
#endif
    js_free_rt(r->rt, s->early);
    js_free_rt(r->rt, s->late);
    js_free_rt(r->rt, s->thread_bound);
    js_free_rt(r->rt, s);
    r->py = NULL;
}

/* ========================================================================= */
/* Part files                                                                */
/* ========================================================================= */

#include "snapshot_func.c"
#include "snapshot_coll.c"
#include "snapshot_buf.c"
#include "snapshot_descr.c"
#include "snapshot_gen.c"

/* ========================================================================= */
/* Native registry and build signature                                       */
/* ========================================================================= */

/* C slot functions stored in QPType.slots outside the specs and slotdefs */
static const void * const qp_snap_extra_natives[] = {
    (const void *)qp_hash_not_implemented,      /* typeobject.c: __hash__ = None */
    (const void *)qp_slot_tp_getattro,          /* typeobject_slots.h: __getattr__ */
};

/* JS functions made by the interop state (jsinterop.c): the fallback
   methods of Python objects seen from JS, Function.prototype.callKw, and
   the promise reactions and iterator steps it creates */
static const void * const qp_snap_jsi_natives[] = {
    (const void *)qp_jsi_fn_iterator,
    (const void *)qp_jsi_fn_tostring,
    (const void *)qp_jsi_fn_tojson,
    (const void *)qp_jsi_fn_valueof,
    (const void *)qp_jsi_fn_then,
    (const void *)qp_jsi_fn_callkw,
    (const void *)qp_jsi_fn_hasinstance,
    (const void *)qp_jsi_fn_tojs,
    (const void *)qp_jsi_task_resume,
    (const void *)qp_jsi_iter_next,
    (const void *)qp_jsi_return_data,
};

static int qp_snap_add_entries(JSRuntime *rt, SnapNatives *n, const char *type_name,
                               const char *table_name, const void *tab, size_t count,
                               size_t elem_size)
{
    size_t i;

    snap_hash_str(&n->signature, type_name);
    snap_hash_str(&n->signature, table_name);
    snap_hash_u32(&n->signature, (uint32_t)count);
    for (i = 0; i < count; i++) {
        const void *e = (const uint8_t *)tab + i * elem_size;
        if (snap_natives_push(rt, n, e))
            return -1;
    }
    return 0;
}

/* the C slot functions of a spec (QPTypeSlots is made of function
   pointers only) */
static int qp_snap_add_spec_slots(JSRuntime *rt, SnapNatives *n, const QPTypeSpec *spec)
{
    QPTypeSlots slots = spec->slots;
    void *fn[sizeof(QPTypeSlots) / sizeof(void *)];
    size_t i;

    memcpy(fn, &slots, sizeof(fn));
    for (i = 0; i < countof(fn); i++) {
        if (fn[i] && snap_natives_push(rt, n, fn[i]))
            return -1;
    }
    return 0;
}

static int qp_snap_add_natives(JSRuntime *rt, SnapNatives *n)
{
    int i;

    /* layout of the Python state that the snapshot format depends on */
    snap_hash_str(&n->signature, "python");
    snap_hash_u32(&n->signature, JS_CLASS_PY_LAST - JS_CLASS_PY_FIRST + 1);
    snap_hash_u32(&n->signature, QP_T__COUNT);
    snap_hash_u32(&n->signature, QP_EXC__COUNT);
    snap_hash_u32(&n->signature, QP_ATOM__COUNT);
    snap_hash_u32(&n->signature, QP_NSIG);
    snap_hash_u32(&n->signature, sizeof(QPTypeSlots) / sizeof(void *));
    snap_hash_u32(&n->signature, QP_NSLOTDEFS);

#define QP_SNAP_A(T, x)                                                      \
    if (qp_snap_add_entries(rt, n, #T, #x, x, countof(x), sizeof((x)[0])))   \
        return -1;
#define QP_SNAP_S(T, x)                                                      \
    if (qp_snap_add_entries(rt, n, #T, #x, &(x), 1, sizeof(x)))              \
        return -1;
    QP_SNAP_TABLES(QP_SNAP_A, QP_SNAP_S)
    QP_SNAP_TABLES_HOST(QP_SNAP_A, QP_SNAP_S)
#undef QP_SNAP_A
#undef QP_SNAP_S

    /* slot functions of every spec */
#define QP_SNAP_A(T, x)                                                      \
    if (__builtin_types_compatible_p(T, QPTypeSpec)) {                       \
        size_t k_;                                                           \
        for (k_ = 0; k_ < countof(x); k_++) {                                \
            if (qp_snap_add_spec_slots(rt, n, (const QPTypeSpec *)&(x)[k_])) \
                return -1;                                                   \
        }                                                                    \
    }
#define QP_SNAP_S(T, x)                                                      \
    if (__builtin_types_compatible_p(T, QPTypeSpec) &&                       \
        qp_snap_add_spec_slots(rt, n, (const QPTypeSpec *)&(x)))             \
        return -1;
    QP_SNAP_TABLES(QP_SNAP_A, QP_SNAP_S)
    QP_SNAP_TABLES_HOST(QP_SNAP_A, QP_SNAP_S)
#undef QP_SNAP_A
#undef QP_SNAP_S

    /* slotdefs: the entries (wrapper descriptors) and the generic slot_*
       dispatchers installed on heap types */
    for (i = 0; i < QP_NSLOTDEFS; i++) {
        snap_hash_str(&n->signature, qp_slotdefs[i].name);
        if (snap_natives_push(rt, n, &qp_slotdefs[i]) ||
            (qp_slotdefs[i].function && snap_natives_push(rt, n, qp_slotdefs[i].function)) ||
            (qp_slotdefs[i].wrapper && snap_natives_push(rt, n, (const void *)qp_slotdefs[i].wrapper)))
            return -1;
    }
    for (i = 0; i < countof(qp_snap_extra_natives); i++) {
        if (snap_natives_push(rt, n, qp_snap_extra_natives[i]))
            return -1;
    }
    for (i = 0; i < countof(qp_snap_jsi_natives); i++) {
        if (snap_natives_push(rt, n, qp_snap_jsi_natives[i]))
            return -1;
    }
#ifdef CONFIG_PYTHON_CAPI
    if (qp_capi_snap_add_natives(rt, n))
        return -1;
#endif
    return 0;
}

/* The JS classes that Python objects use, on a reader runtime (registered
   by qp_state_init on the writer side). */
static int qp_snap_init_runtime(JSRuntime *rt)
{
    if (!JS_IsRegisteredClass(rt, JS_CLASS_PY_FIRST) &&
        init_class_range(rt, qp_class_def, JS_CLASS_PY_FIRST, countof(qp_class_def)))
        return -1;
    /* the weakref holder is a host class created on first use: register it
       with the same (process-wide) id if the writer process had one */
    if (qp_wr_holder_class_id == 0)
        JS_NewClassID(&qp_wr_holder_class_id);
    if (!JS_IsRegisteredClass(rt, qp_wr_holder_class_id) &&
        JS_NewClass(rt, qp_wr_holder_class_id, &qp_wr_holder_class) < 0)
        return -1;
    return 0;
}

/* ========================================================================= */
/* Type objects (JS_CLASS_PY_TYPE)                                           */
/* ========================================================================= */

#define QP_SNAP_NSLOTFN (sizeof(QPTypeSlots) / sizeof(void *))

static int qp_snap_write_type(SnapWriter *w, JSObject *p)
{
    QPType *t = p->u.opaque;
    QPTypeExt *e;
    void *fn[QP_SNAP_NSLOTFN];
    int i;

    if (!t) {
        bc_put_u8(&w->bc, 0);
        return 0;
    }
#ifdef CONFIG_PYTHON_CAPI
    {
        /* u8 2 + key: type made by C, saved by reference */
        int ret = qp_capi_snap_write_type_ref(w, p);
        if (ret <= 0)
            return ret;
    }
#endif
    e = QP_TEXT(t);
    if (t->flags & QP_TPFLAGS_READYING)
        return qp_snap_reject(w, p, "type is being created");
    /* tp_name comes from the spec (typeobject.c qp_type_ready) or is owned
       by the type (QP_TPFLAGS_PRIV_OWNED_NAME, saved as text) */
    if (t->tp_name && !(t->flags & QP_TPFLAGS_PRIV_OWNED_NAME) &&
        (!t->spec || t->tp_name != t->spec->name))
        return qp_snap_reject(w, p, "type name not owned by its spec");
    bc_put_u8(&w->bc, 1);
    if (snap_put_ref(w, SNAP_CELL_CONTEXT, t->ctx) ||
        snap_put_native(w, t->spec))
        return -1;
    if (t->tp_name && (t->flags & QP_TPFLAGS_PRIV_OWNED_NAME)) {
        size_t n = strlen(t->tp_name);
        bc_put_u8(&w->bc, 2);
        bc_put_leb128(&w->bc, (uint32_t)n);
        dbuf_put(&w->bc.dbuf, (const uint8_t *)t->tp_name, n);
    } else {
        bc_put_u8(&w->bc, t->tp_name != NULL);
    }
    if (bc_put_atom(&w->bc, t->name) ||
        snap_put_value(w, t->qualname))
        return -1;
    bc_put_u64(&w->bc, t->flags);
    bc_put_leb128(&w->bc, t->version_tag);
    bc_put_leb128(&w->bc, t->instance_class_id);
    bc_put_leb128(&w->bc, t->nslots);
    if (qp_snap_put_type(w, t->base) ||
        snap_put_value(w, t->bases) ||
        snap_put_value(w, t->mro) ||
        snap_put_value(w, t->dict) ||
        snap_put_value(w, t->slot_names) ||
        snap_put_value(w, e->doc))
        return -1;
    /* raw links: bases (a NULL entry is a base that died in the same GC
       cycle) and the subclasses in registration order (__subclasses__()) */
    bc_put_leb128(&w->bc, e->nbases_raw);
    for (i = 0; i < e->nbases_raw; i++) {
        if (qp_snap_put_type(w, e->bases_raw[i]))
            return -1;
    }
#ifdef CONFIG_PYTHON_CAPI
    {
        /* types saved by reference link themselves again when their C
           module runs (and an older copy from a failed module exec is
           not saved) */
        uint8_t skip_buf[64], *skip = skip_buf;
        int n = 0, ret = 0;
        if (t->subclass_count > (int)sizeof(skip_buf)) {
            skip = js_malloc(w->ctx, t->subclass_count);
            if (!skip)
                return -1;
        }
        for (i = 0; i < t->subclass_count && ret >= 0; i++) {
            ret = qp_capi_snap_type_skip_sub(w, t->subclasses[i]);
            skip[i] = ret > 0;
            n += ret == 0;
        }
        if (ret >= 0) {
            bc_put_leb128(&w->bc, n);
            for (i = 0; i < t->subclass_count && ret >= 0; i++) {
                if (!skip[i] && qp_snap_put_type(w, t->subclasses[i]))
                    ret = -1;
            }
        }
        if (skip != skip_buf)
            js_free(w->ctx, skip);
        if (ret < 0)
            return -1;
    }
#else
    bc_put_leb128(&w->bc, t->subclass_count);
    for (i = 0; i < t->subclass_count; i++) {
        if (qp_snap_put_type(w, t->subclasses[i]))
            return -1;
    }
#endif
    memcpy(fn, &t->slots, sizeof(fn));
    for (i = 0; i < (int)QP_SNAP_NSLOTFN; i++) {
        if (snap_put_native(w, fn[i]))
            return -1;
    }
    return 0;
}

static int qp_snap_read_type(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    QPType *t;
    QPTypeExt *e;
    void *fn[QP_SNAP_NSLOTFN];
    void *spec;
    uint8_t present, has_tp_name;
    uint32_t v, i, n;
    uint64_t flags;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
#ifdef CONFIG_PYTHON_CAPI
    if (present == 2)
        return qp_capi_snap_read_type_ref(r, p);
#endif
    if (present != 1)
        return snap_error(r, "invalid Python type payload");
    t = qp_snap_type_ensure(r, p);
    if (!t)
        return -1;
    e = QP_TEXT(t);
    snap_payload_ready(r);
    if (snap_get_ref(r, SNAP_CELL_CONTEXT, (void **)&t->ctx) ||
        snap_get_native(r, &spec) ||
        bc_get_u8(&r->bc, &has_tp_name))
        return -1;
    t->spec = spec;
    if (has_tp_name == 2) {
        uint32_t n;
        char *tn;
        if (bc_get_leb128(&r->bc, &n))
            return -1;
        tn = js_malloc_rt(r->rt, (size_t)n + 1);
        if (!tn)
            return snap_error(r, "out of memory");
        if (bc_get_buf(&r->bc, (uint8_t *)tn, n)) {
            js_free_rt(r->rt, tn);
            return -1;
        }
        tn[n] = '\0';
        t->tp_name = tn;
        /* the saved flags carry it too */
        t->flags |= QP_TPFLAGS_PRIV_OWNED_NAME;
    } else if (has_tp_name) {
        if (!t->spec)
            return snap_error(r, "invalid Python type name");
        t->tp_name = t->spec->name;
    }
    if (bc_get_atom(&r->bc, &t->name) ||
        snap_get_value(r, &t->qualname) ||
        bc_get_u64(&r->bc, &flags) ||
        bc_get_leb128(&r->bc, &t->version_tag))
        return -1;
    t->flags = flags;
    if (bc_get_leb128(&r->bc, &v))
        return -1;
    if (v >= r->rt->class_count)
        return snap_error(r, "invalid Python instance class");
    t->instance_class_id = v;
    if (bc_get_leb128(&r->bc, &v))
        return -1;
    if (v > UINT16_MAX)
        return snap_error(r, "invalid Python slot count");
    t->nslots = v;
    if (qp_snap_get_type(r, &t->base) ||
        snap_get_value(r, &t->bases) ||
        snap_get_value(r, &t->mro) ||
        snap_get_value(r, &t->dict) ||
        snap_get_value(r, &t->slot_names) ||
        snap_get_value(r, &e->doc))
        return -1;

    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid Python base count");
    if (n) {
        e->bases_raw = js_mallocz(ctx, sizeof(e->bases_raw[0]) * n);
        if (!e->bases_raw)
            return -1;
        for (i = 0; i < n; i++) {
            if (qp_snap_get_type(r, &e->bases_raw[i]))
                return -1;
            e->nbases_raw = i + 1;
        }
    }
    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid Python subclass count");
    if (n) {
        t->subclasses = js_mallocz(ctx, sizeof(t->subclasses[0]) * n);
        if (!t->subclasses)
            return -1;
        t->subclass_size = n;
        for (i = 0; i < n; i++) {
            QPType *sub;
            if (qp_snap_get_type(r, &sub))
                return -1;
            if (!sub)
                return snap_error(r, "invalid Python subclass");
            t->subclasses[t->subclass_count++] = sub;
        }
    }
    for (i = 0; i < QP_SNAP_NSLOTFN; i++) {
        if (snap_get_native(r, &fn[i]))
            return -1;
    }
    memcpy(&t->slots, fn, sizeof(fn));
    return 0;
}

/* ========================================================================= */
/* Instances (JS_CLASS_PY_OBJECT), boxed values, C-extension companions      */
/* ========================================================================= */

/* Builtin types whose instances hold host resources in their slots. */
static const struct {
    const QPTypeSpec *spec;
    const char *reason;
} qp_snap_host_specs[] = {
    { &qp_si_spec, "directory stream (os.scandir iterator)" },     /* DIR* in a slot */
    { &qp_poll_spec, "poll object on file descriptors" },
#ifdef QP_HAVE_KQUEUE
    { &qp_kqueue_spec, "kqueue object (file descriptor)" },
#endif
    { &qp_tokiter_spec, "tokenizer state (_tokenize.TokenizerIter)" },
};

/* _thread objects that name an OS thread by its ident (_ThreadHandle of a
   started thread, RLock held by a thread).  The main thread is the one
   thread a restored runtime has again (the main thread of the reading
   process): what is bound to it is saved by reference and bound to the
   new main thread on read (qp_snap_thread_rebind).  Locks and RLocks
   that no thread holds and the handles of threads not started or done
   are plain values; a handle of another live thread is host state.
   Returns 1 (bound to the main thread), 0 (a value) or -1 with *preason. */
static int qp_snap_thread_bound(JSContext *ctx, JSObject *p, QPType *t, const char **preason)
{
    JSValue obj = JS_MKPTR(JS_TAG_OBJECT, p);
    QPInstance *in = p->u.opaque;
    QPType *b;
    int state;

    if (!in || in->nslots < 2)
        return 0;
    for (b = t; b; b = b->base) {
        if (b->spec == &qp_thread_handle_spec) {
            state = qp_thread_slot_int(obj, 1);
            if (state != QP_TH_STARTING && state != QP_TH_RUNNING)
                return 0;
            if (state == QP_TH_RUNNING &&
                qp_thread_slot_u64(ctx, obj, 0) == qp_thread_main_ident(ctx))
                return 1;
            *preason = "operating system thread (only the main thread is restored)";
            return -1;
        }
        if (b->spec == &qp_thread_rlock_spec)
            return qp_thread_slot_int(obj, 1) > 0 &&
                qp_thread_slot_u64(ctx, obj, 0) == qp_thread_main_ident(ctx);
    }
    return 0;
}

/* Python classes (any module spelling) whose instances own a file
   descriptor, a socket or a child process.  FileIO objects on the standard
   streams (fd 0-2, e.g. behind sys.stdout) are accepted: every process
   has them. */
static const struct {
    const char *module, *qualname, *reason;
} qp_snap_host_classes[] = {
    { "_io", "FileIO", "file" },
    { "_pyio", "FileIO", "file" },
    { "io", "FileIO", "file" },
    { "_socket", "socket", "socket" },
    { "socket", "socket", "socket" },
    { "subprocess", "Popen", "child process" },
};

/* str value of a str/atom pair comparison without running Python code */
static BOOL qp_snap_str_equals(JSContext *ctx, JSValueConst v, const char *s)
{
    const char *p;
    BOOL ret;
    if (!JS_IsString(v))
        return FALSE;
    p = JS_ToCString(ctx, v);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return FALSE;
    }
    ret = strcmp(p, s) == 0;
    JS_FreeCString(ctx, p);
    return ret;
}

/* own attribute `name` of instance p (shape property or materialized
   __dict__), QP_NULL if absent; borrowed.  Pure: no Python code runs. */
static JSValue qp_snap_peek_attr(JSContext *ctx, JSObject *p, const char *name)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    JSAtom atom;
    JSValue ret = QP_NULL;

    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return QP_NULL;
    }
    if (p->py_dict_mode) {
        JSValue d = qp_obj_dict_get(ctx, p), v;
        if (!QP_IS_NULL(d) && JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT &&
            JS_VALUE_GET_OBJ(d)->class_id == JS_CLASS_PY_DICT) {
            int res = qp_dict_get_item_atom(ctx, d, atom, &v);
            if (res < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            } else if (res > 0) {
                ret = v;
                JS_FreeValue(ctx, v); /* still owned by the dict */
            }
        }
    } else {
        prs = find_own_property(&pr, p, atom);
        if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL)
            ret = pr->u.value;
    }
    JS_FreeAtom(ctx, atom);
    return ret;
}

/* reason why instance p of type t cannot be saved, NULL if it can */
static const char *qp_snap_host_reason(JSContext *ctx, JSObject *p, QPType *t)
{
    QPType *b;
    int i;

    for (b = t; b; b = b->base) {
        for (i = 0; i < (int)countof(qp_snap_host_specs); i++) {
            if (b->spec == qp_snap_host_specs[i].spec)
                return qp_snap_host_specs[i].reason;
        }
    }
    if (!ctx || !ctx->py || JS_VALUE_GET_TAG(t->mro) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(t->mro)->class_id != JS_CLASS_PY_TUPLE)
        return NULL;
    {
        QPTuple *mro = QP_TUPLE(t->mro);
        uint32_t k;
        for (k = 0; k < mro->len; k++) {
            QPType *m;
            JSValue mod = QP_NULL;
            if (JS_VALUE_GET_TAG(mro->items[k]) != JS_TAG_OBJECT ||
                JS_VALUE_GET_OBJ(mro->items[k])->class_id != JS_CLASS_PY_TYPE)
                continue;
            m = JS_VALUE_GET_OBJ(mro->items[k])->u.opaque;
            if (!m || !(m->flags & QP_TPFLAGS_HEAPTYPE))
                continue;
            for (i = 0; i < (int)countof(qp_snap_host_classes); i++) {
                int res;
                if (!qp_snap_str_equals(ctx, m->qualname, qp_snap_host_classes[i].qualname))
                    continue;
                res = qp_dict_get_item_atom(ctx, m->dict, QP_ATOM(ctx, __module__), &mod);
                if (res < 0) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    continue;
                }
                res = res > 0 && qp_snap_str_equals(ctx, mod, qp_snap_host_classes[i].module);
                if (!QP_IS_NULL(mod))
                    JS_FreeValue(ctx, mod);
                if (!res)
                    continue;
                if (!strcmp(qp_snap_host_classes[i].reason, "file")) {
                    JSValue fd = qp_snap_peek_attr(ctx, p, "_fd");
                    if (JS_VALUE_GET_TAG(fd) == JS_TAG_INT &&
                        ((JS_VALUE_GET_INT(fd) >= 0 && JS_VALUE_GET_INT(fd) <= 2) ||
                         JS_VALUE_GET_INT(fd) < 0 /* closed */))
                        return NULL;
                    /* closed (_pyio.FileIO.close() keeps _fd; IOBase.close()
                       sets the private __closed) */
                    if (JS_VALUE_GET_TAG(fd = qp_snap_peek_attr(ctx, p, "_IOBase__closed")) ==
                        JS_TAG_BOOL && JS_VALUE_GET_BOOL(fd))
                        return NULL;
                }
                return qp_snap_host_classes[i].reason;
            }
        }
    }
    return NULL;
}

static int qp_snap_write_instance(SnapWriter *w, JSObject *p)
{
    JSObject *proto = p->shape->proto;
    QPInstance *in = p->u.opaque;
    uint32_t i;
    int bound = 0;

    if (proto && proto->class_id == JS_CLASS_PY_TYPE && proto->u.opaque) {
        QPType *t = proto->u.opaque;
        const char *reason = qp_snap_host_reason(t->ctx, p, t);
        if (!reason)
            bound = qp_snap_thread_bound(t->ctx, p, t, &reason);
        if (reason)
            return qp_snap_reject(w, p, reason);
    }
    if (!in) {
        bc_put_u8(&w->bc, 0);
        return 0;
    }
    /* 1: slots, 2: slots of an object bound to the main thread */
    bc_put_u8(&w->bc, bound ? 2 : 1);
    bc_put_leb128(&w->bc, in->nslots);
    for (i = 0; i < in->nslots; i++) {
        if (snap_put_value(w, in->slots[i]))
            return -1;
    }
    return 0;
}

static int qp_snap_read_instance(SnapReader *r, JSObject *p)
{
    QPInstance *in;
    uint8_t present;
    uint32_t i, n;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
    if (present > 2)
        return snap_error(r, "invalid Python instance payload");
    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr || (present == 2 && n < 2))
        return snap_error(r, "invalid Python instance slot count");
    if (present == 2) {
        QPSnapRead *s = qp_snap_read_state_get(r);
        if (!s)
            return -1;
        if (snap_resize(r->rt, (void **)&s->thread_bound, sizeof(*s->thread_bound),
                        &s->thread_bound_size, s->thread_bound_count + 1)) {
            JS_ThrowOutOfMemory(r->ctx);
            return -1;
        }
        s->thread_bound[s->thread_bound_count++] = p;
    }
    in = js_malloc(r->ctx, sizeof(*in) + sizeof(in->slots[0]) * n);
    if (!in)
        return -1;
    in->nslots = n;
    for (i = 0; i < n; i++)
        in->slots[i] = QP_NULL;
    p->u.opaque = in;
    snap_payload_ready(r);
    for (i = 0; i < n; i++) {
        if (snap_get_value(r, &in->slots[i]))
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Weak references (mod_weakref.c holder class)                              */
/* ========================================================================= */

/* The weakref object keeps its QPWeakRec in a holder host object (slot 0).
   The referent is written as a normal reference (like the engine's weak
   references): if nothing else keeps it, the restored runtime frees it as
   soon as the reader drops its references, which clears the weakref and
   queues its callback.  The per-referent lists of the module state are
   rebuilt by qp_wr_insert (basic references first, as when created). */
static int qp_snap_write_wr_holder(SnapWriter *w, JSObject *p)
{
    QPWeakRec *rec = p->u.opaque;

    if (!rec) {
        bc_put_u8(&w->bc, 0);
        return 0;
    }
    if (rec->pending || !QP_IS_NULL(rec->pending_cb)) {
        JS_ThrowTypeError(w->ctx, "snapshot: a Python weak reference callback is pending");
        return -1;
    }
    bc_put_u8(&w->bc, 1);
    if (snap_put_ref(w, SNAP_CELL_CONTEXT, rec->ctx))
        return -1;
    bc_put_u8(&w->bc, rec->kind);
    /* the hash of a live referent may be an identity hash: recomputed */
    qp_snap_put_i64(w, rec->target ? -1 : rec->hash);
    if (snap_put_value(w, rec->callback) ||
        snap_put_obj(w, rec->owner) ||
        snap_put_obj(w, rec->target))
        return -1;
    return 0;
}

static int qp_snap_wr_link(JSContext *ctx, JSObject *p)
{
    QPWeakRec *rec = p->u.opaque;
    QPModState *ms;
    QPWeakBucket *b;

    if (!rec || !rec->target)
        return 0;
    ms = qp_modstate(rec->ctx);
    if (!ms)
        return -1;
    b = qp_wr_table_insert(rec->ctx, ms, rec->target);
    if (!b)
        return -1;
    qp_wr_insert(rec->ctx, ms, b, rec);
    return 0;
}

static int qp_snap_read_wr_holder(SnapReader *r, JSObject *p)
{
    QPWeakRec *rec;
    JSObject *owner, *target;
    uint8_t present, kind;
    int64_t hash;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
    rec = js_mallocz(r->ctx, sizeof(*rec));
    if (!rec)
        return -1;
    rec->callback = QP_NULL;
    rec->pending_cb = QP_NULL;
    rec->hash = -1;
    p->u.opaque = rec;
    snap_payload_ready(r);
    if (snap_get_ref(r, SNAP_CELL_CONTEXT, (void **)&rec->ctx) ||
        bc_get_u8(&r->bc, &kind) ||
        qp_snap_get_i64(r, &hash) ||
        snap_get_value(r, &rec->callback) ||
        snap_get_obj(r, &owner) ||
        snap_get_obj(r, &target))
        return -1;
    if (!rec->ctx || kind > QP_WR_KIND_CALLPROXY)
        return snap_error(r, "invalid Python weak reference");
    rec->kind = kind;
    rec->hash = hash;
    if (owner) {
        owner->weakref_count++;
        rec->owner = owner;
    }
    if (target) {
        target->weakref_count++;
        rec->target = target;
    }
    return qp_snap_defer(r, p, qp_snap_wr_link);
}

/* ========================================================================= */
/* Payload dispatch                                                          */
/* ========================================================================= */

static int qp_snap_write_payload(SnapWriter *w, JSObject *p)
{
    int ret;

    if (qp_wr_holder_class_id != 0 && p->class_id == qp_wr_holder_class_id)
        return qp_snap_write_wr_holder(w, p);
    if (!QP_IS_PY_CLASS(p->class_id))
        return 1;
    switch (p->class_id) {
    case JS_CLASS_PY_TYPE:
        return qp_snap_write_type(w, p);
    case JS_CLASS_PY_OBJECT:
        return qp_snap_write_instance(w, p);
    case JS_CLASS_PY_BOXED:
        return snap_put_value(w, p->u.object_data);
    case JS_CLASS_PY_NATIVE:
        /* D5 companion of a C-extension object: its state is foreign C
           memory with C callbacks, nothing that can be re-derived */
        return qp_snap_reject(w, p, "C extension object");
    default:
        break;
    }
    if ((ret = qp_snap_write_func(w, p)) != 1 ||
        (ret = qp_snap_write_coll(w, p)) != 1 ||
        (ret = qp_snap_write_buf(w, p)) != 1 ||
        (ret = qp_snap_write_descr(w, p)) != 1 ||
        (ret = qp_snap_write_gen(w, p)) != 1)
        return ret;
    JS_ThrowInternalError(w->ctx, "snapshot: Python class %d has no serializer", p->class_id);
    return -1;
}

static int qp_snap_read_payload(SnapReader *r, JSObject *p)
{
    int ret;

    if (qp_wr_holder_class_id != 0 && p->class_id == qp_wr_holder_class_id)
        return qp_snap_read_wr_holder(r, p);
    if (!QP_IS_PY_CLASS(p->class_id))
        return 1;
    switch (p->class_id) {
    case JS_CLASS_PY_TYPE:
        return qp_snap_read_type(r, p);
    case JS_CLASS_PY_OBJECT:
        return qp_snap_read_instance(r, p);
    case JS_CLASS_PY_BOXED:
        p->u.object_data = JS_UNDEFINED;
        snap_payload_ready(r);
        return snap_get_value(r, &p->u.object_data);
    case JS_CLASS_PY_NATIVE:
#ifdef CONFIG_PYTHON_CAPI
        return qp_capi_snap_read_record(r, p);
#else
        return snap_error(r, "unsupported Python C extension object");
#endif
    default:
        break;
    }
    if ((ret = qp_snap_read_func(r, p)) != 1 ||
        (ret = qp_snap_read_coll(r, p)) != 1 ||
        (ret = qp_snap_read_buf(r, p)) != 1 ||
        (ret = qp_snap_read_descr(r, p)) != 1 ||
        (ret = qp_snap_read_gen(r, p)) != 1)
        return ret;
    return snap_error(r, "unsupported Python object class %d", p->class_id);
}

/* ========================================================================= */
/* Code metadata (JSFunctionBytecode.py_code)                                */
/* ========================================================================= */

/* python/codeobject.c qp_bc_write_code / qp_bc_read_code (the format the
   bytecode writer uses for marshal and .pyc files too) */
static int qp_snap_write_code(SnapWriter *w, JSFunctionBytecode *b)
{
    return qp_bc_write_code(&w->bc, b);
}

static int qp_snap_read_code(SnapReader *r, JSFunctionBytecode *b)
{
    return qp_bc_read_code(&r->bc, b);
}

/* ========================================================================= */
/* Per-context state (JSContext.py)                                          */
/* ========================================================================= */

/* JSValue fields of QPState, in format order */
#define QP_SNAP_STATE_VALUES(X)                                              \
    X(not_implemented) X(ellipsis) X(empty_tuple) X(empty_frozenset)         \
    X(empty_bytes) X(builtins_module) X(builtins_dict) X(sys_module)         \
    X(sys_dict) X(typing_nodefault) X(audit_hooks) X(sys_structseq_types)    \
    X(modules) X(import_func) X(importlib) X(repr_stack) X(pending_del)      \
    X(asyncgen_firstiter) X(asyncgen_finalizer) X(exc_info_base.exc_value)   \
    X(lazy_imports_filter) X(lazy_modules)

/* int fields of QPState (config: the launcher's options; its string
   pointers belong to the launcher process and are not restored) */
#define QP_SNAP_STATE_INTS(X)                                                \
    X(config.optimize) X(config.verbose) X(config.quiet) X(config.isolated)  \
    X(config.ignore_environment) X(config.safe_path) X(config.unbuffered)    \
    X(config.inspect) X(config.interactive) X(config.dont_write_bytecode)    \
    X(config.int_max_str_digits) X(config.lazy_imports)                      \
    X(config.recursion_limit) X(config.no_user_site) X(config.no_site)       \
    X(config.bytes_warning) X(config.utf8_mode) X(config.dev_mode)           \
    X(config.warn_default_encoding) X(config.parser_debug)                   \
    X(config.cpu_count)                                                      \
    X(recursion_limit) X(int_max_str_digits) X(lazy_imports_mode)           \
    X(import_lock_level) X(override_frozen_modules) X(dlopenflags)

/* QPModState JSValue fields and arrays, in format order */
#define QP_SNAP_MS_VALUES(X)                                                 \
    X(atexit_callbacks) X(sig_default_int_handler) X(sig_dfl) X(sig_ign)     \
    X(sig_itimer_error) X(sig_siginfo_type) X(gc_callbacks) X(gc_garbage) X(warn_filters)        \
    X(warn_once_registry) X(warn_default_action) X(warn_context)             \
    X(ctx_current) X(thread_shutdown_handles) X(codec_search_path)           \
    X(codec_search_cache) X(codec_error_registry) X(codec_encoding_map_type) \
    X(ast_state) X(mon_disable) X(mon_missing) X(async_exc)
#define QP_SNAP_MS_ARRAYS(X)                                                 \
    X(sig_handlers) X(ctx_types) X(thread_types) X(wr_types)                 \
    X(codec_builtin_handlers) X(mon_tool_names) X(mon_callbacks)
#define QP_SNAP_MS_INTS(X)                                                   \
    X(sig_wakeup_warn_on_full_buffer) X(gc_enabled) X(gc_debug)              \
    X(gc_threshold[0]) X(gc_threshold[1]) X(gc_threshold[2])                 \
    X(warn_lock_count)
#define QP_SNAP_MS_I64(X)                                                    \
    X(gc_frozen) X(gc_collections[0]) X(gc_collections[1])                   \
    X(gc_collections[2]) X(gc_collected[0]) X(gc_collected[1])               \
    X(gc_collected[2]) X(gc_uncollectable[0]) X(gc_uncollectable[1])        \
    X(gc_uncollectable[2]) X(gc_candidates[0]) X(gc_candidates[1])          \
    X(gc_candidates[2]) X(gc_baseline) X(warn_filters_version)               \
    X(mon_events[0]) X(mon_events[1]) X(mon_events[2]) X(mon_events[3])      \
    X(mon_events[4]) X(mon_events[5])

static int qp_snap_write_modstate(SnapWriter *w, QPModState *ms)
{
    int i;

    bc_put_u8(&w->bc, ms != NULL);
    if (!ms)
        return 0;
    if (ms->gc_collecting) {
        JS_ThrowTypeError(w->ctx, "snapshot: Python gc collection in progress");
        return -1;
    }
    if (ms->wr_pending_head) {
        JS_ThrowTypeError(w->ctx, "snapshot: Python weak reference callbacks are pending");
        return -1;
    }
#define X(f) if (snap_put_value(w, ms->f)) return -1;
    QP_SNAP_MS_VALUES(X)
#undef X
#define X(f)                                                                 \
    for (i = 0; i < (int)countof(ms->f); i++) {                             \
        if (snap_put_value(w, ms->f[i]))                                     \
            return -1;                                                       \
    }
    QP_SNAP_MS_ARRAYS(X)
#undef X
#define X(f) bc_put_sleb128(&w->bc, ms->f);
    QP_SNAP_MS_INTS(X)
#undef X
#define X(f) qp_snap_put_i64(w, ms->f);
    QP_SNAP_MS_I64(X)
#undef X
    for (i = 0; i < 3; i++)
        qp_snap_put_f64(w, ms->gc_duration[i]);
    return 0;
}

static int qp_snap_read_modstate(SnapReader *r, QPState *s)
{
    QPModState *ms;
    uint8_t present;
    int i;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    ms = qp_modstate(s->ctx); /* initialised with QP_NULL values */
    if (!ms)
        return -1;
#define X(f) if (snap_get_value(r, &ms->f)) return -1;
    QP_SNAP_MS_VALUES(X)
#undef X
#define X(f)                                                                 \
    for (i = 0; i < (int)countof(ms->f); i++) {                             \
        if (snap_get_value(r, &ms->f[i]))                                    \
            return -1;                                                       \
    }
    QP_SNAP_MS_ARRAYS(X)
#undef X
#define X(f) if (qp_snap_get_int(r, &ms->f)) return -1;
    QP_SNAP_MS_INTS(X)
#undef X
#define X(f) { int64_t v_; if (qp_snap_get_i64(r, &v_)) return -1; ms->f = v_; }
    QP_SNAP_MS_I64(X)
#undef X
    for (i = 0; i < 3; i++) {
        if (qp_snap_get_f64(r, &ms->gc_duration[i]))
            return -1;
    }
    /* the wakeup fd (signal.set_wakeup_fd) is a descriptor of the writer
       process */
    ms->sig_wakeup_fd = -1;
    return 0;
}

static int qp_snap_write_state(SnapWriter *w, JSContext *ctx)
{
    QPState *s = ctx->py;
    int i;

    bc_put_u8(&w->bc, s != NULL);
    if (!s)
        return 0;
    if (s->recursion_depth != 0 || s->exc_info != &s->exc_info_base) {
        JS_ThrowTypeError(w->ctx, "snapshot: Python code is running");
        return -1;
    }
    if (qp_thread_others_exist(w->ctx)) {
        JS_ThrowTypeError(w->ctx, "snapshot: other Python threads are running");
        return -1;
    }
    for (i = 0; i < QP_T__COUNT; i++) {
        if (qp_snap_put_type(w, s->types[i]))
            return -1;
    }
    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (qp_snap_put_type(w, s->exc_types[i]))
            return -1;
    }
    for (i = 0; i < QP_ATOM__COUNT; i++) {
        if (bc_put_atom(&w->bc, s->atoms[i]))
            return -1;
    }
    if (bc_put_atom(&w->bc, s->atom_dict_slot))
        return -1;
#define X(f) if (snap_put_value(w, s->f)) return -1;
    QP_SNAP_STATE_VALUES(X)
#undef X
    for (i = 0; i < (int)countof(s->bytes_chars); i++) {
        if (snap_put_value(w, s->bytes_chars[i]))
            return -1;
    }
#define X(f) bc_put_sleb128(&w->bc, s->f);
    QP_SNAP_STATE_INTS(X)
#undef X
    bc_put_leb128(&w->bc, s->next_type_version);
    bc_put_u64(&w->bc, s->next_dict_uid);
    dbuf_put(&w->bc.dbuf, s->hash_key, sizeof(s->hash_key));
    if (qp_snap_write_modstate(w, s->mod_state))
        return -1;
    /* the interop state is rebuilt on read (qp_snap_jsi_apply) */
    bc_put_u8(&w->bc, ctx->py_js != NULL);
    return 0;
}

/* the interop state of a session that used it: exotic handlers of the
   Python classes (and the is_exotic bit of the restored objects),
   Function.prototype.callKw and the fallback methods */
static int qp_snap_jsi_apply(JSContext *ctx, JSObject *unused)
{
    return qp_jsi_state(ctx) ? 0 : -1;
}

/* process state described by the restored module state */
static int qp_snap_state_apply(JSContext *ctx, JSObject *unused)
{
    QPModState *ms = ctx->py->mod_state;
    int i;

    if (!ms)
        return 0;
    /* signal handlers installed by signal.signal() (and the default
       SIGINT handler): reinstall the C handler in this process */
    for (i = 1; i < QP_NSIG; i++) {
        JSValueConst h = ms->sig_handlers[i];
        if (QP_IS_NULL(h))
            continue;
        if (qp_signal_compare_handler(h, ms->sig_ign))
            qp_signal_setsig(i, SIG_IGN);
        else if (!qp_signal_compare_handler(h, ms->sig_dfl) && qp_signal_callable(ctx, h))
            qp_signal_setsig(i, qp_signal_handler);
    }
    /* gc.disable(): the automatic collection of this runtime is off */
    if (!ms->gc_enabled) {
        ms->gc_saved_threshold = JS_GetGCThreshold(ctx->rt);
        JS_SetGCThreshold(ctx->rt, (size_t)-1);
    }
    return 0;
}

static int qp_snap_read_state(SnapReader *r, JSContext *ctx)
{
    QPSnapRead *rs;
    QPState *s;
    struct QPStrCache *c;
    uint8_t present, interop;
    uint64_t uid;
    int i;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    rs = qp_snap_read_state_get(r);
    if (!rs)
        return -1;
#ifdef CONFIG_PYTHON_CAPI
    /* as qp_state_init: the restored session imports the C modules too */
    if (qp_capi_register_core_modules() < 0)
        return -1;
#endif
    /* same initial values as qp_state_init (finalizer-safe) */
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return -1;
    ctx->py = s;
    s->ctx = ctx;
#define X(f) s->f = JS_UNDEFINED;
    QP_SNAP_STATE_VALUES(X)
#undef X
    for (i = 0; i < (int)countof(s->bytes_chars); i++)
        s->bytes_chars[i] = JS_UNDEFINED;
    s->exc_info = &s->exc_info_base;
    qp_mon_state_init(s);
    if (!rs->py_ctx)
        rs->py_ctx = ctx;
    c = js_malloc(ctx, sizeof(*c));
    if (!c)
        return -1;
    for (i = 0; i < QP_STR_CACHE_SIZE; i++) {
        c->e[i].str = NULL;
        c->e[i].hash = -1;
        c->e[i].cplen = -1;
        c->e[i].ucs4 = NULL;
    }
    s->str_cache = c;

    for (i = 0; i < QP_T__COUNT; i++) {
        if (qp_snap_get_type(r, &s->types[i]))
            return -1;
        if (s->types[i])
            JS_DupValue(ctx, QP_TYPE_VALUE(s->types[i]));
    }
    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (qp_snap_get_type(r, &s->exc_types[i]))
            return -1;
        if (s->exc_types[i])
            JS_DupValue(ctx, QP_TYPE_VALUE(s->exc_types[i]));
    }
    for (i = 0; i < QP_ATOM__COUNT; i++) {
        if (bc_get_atom(&r->bc, &s->atoms[i]))
            return -1;
    }
    if (bc_get_atom(&r->bc, &s->atom_dict_slot))
        return -1;
#define X(f) if (snap_get_value(r, &s->f)) return -1;
    QP_SNAP_STATE_VALUES(X)
#undef X
    for (i = 0; i < (int)countof(s->bytes_chars); i++) {
        if (snap_get_value(r, &s->bytes_chars[i]))
            return -1;
    }
#define X(f) if (qp_snap_get_int(r, &s->f)) return -1;
    QP_SNAP_STATE_INTS(X)
#undef X
    if (bc_get_leb128(&r->bc, &s->next_type_version) ||
        bc_get_u64(&r->bc, &uid) ||
        bc_get_buf(&r->bc, s->hash_key, sizeof(s->hash_key)))
        return -1;
    s->next_dict_uid = uid;
    /* qp_main: the hash key of this process (the deferred re-validation
       of the hash tables then rehashes their keys) */
    if (r->rt->py_snap_hash_key)
        memcpy(s->hash_key, r->rt->py_snap_hash_key, sizeof(s->hash_key));
    if (qp_snap_read_modstate(r, s) || bc_get_u8(&r->bc, &interop))
        return -1;
    /* the object slot carries the context (see qp_snap_run_deferred) */
    if (interop && qp_snap_defer(r, (JSObject *)ctx, qp_snap_jsi_apply))
        return -1;
    return qp_snap_defer(r, (JSObject *)ctx, qp_snap_state_apply);
}

/* ========================================================================= */
/* End of the read                                                           */
/* ========================================================================= */

static int qp_snap_run_deferred(SnapReader *r, QPSnapDefer *tab, int count)
{
    QPSnapRead *s = r->py;
    int i;

    for (i = 0; i < count; i++) {
        /* the state hook passes its context as the object (see
           qp_snap_read_state); other functions run in the first Python
           context (they only use it for allocation and Python calls) */
        JSContext *ctx = s->py_ctx;
        JSObject *obj = tab[i].obj;
        if (tab[i].func == qp_snap_state_apply) {
            ctx = (JSContext *)obj;
            obj = NULL;
        }
        if (!ctx)
            return snap_error(r, "Python object without Python state");
        if (tab[i].func(ctx, obj) < 0) {
            JSValue exc = JS_GetException(ctx);
            const char *msg = NULL;
            if (!JS_IsNull(exc) && !JS_IsUndefined(exc))
                msg = JS_ToCString(ctx, exc);
            if (!msg)
                JS_FreeValue(ctx, JS_GetException(ctx));
            snap_error(r, "restoring a Python object failed: %s", msg ? msg : "error");
            if (msg)
                JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
            return -1;
        }
    }
    return 0;
}

#ifdef CONFIG_PYTHON_CAPI
static BOOL qp_capi_snap_is_c_slot(const void *fn);

/* key whose hash runs no Python code and needs no C-extension state */
static BOOL qp_snap_hash_pure(JSContext *ctx, JSValueConst k, int depth)
{
    JSObject *p;
    QPType *t;

    if (JS_VALUE_GET_TAG(k) != JS_TAG_OBJECT)
        return TRUE;
    if (depth > 8)
        return FALSE;
    p = JS_VALUE_GET_OBJ(k);
    /* C-extension objects (records) and definitions not restored yet */
    if (p->class_id == JS_CLASS_PY_NATIVE || p->class_id == JS_CLASS_PY_CFUNCTION ||
        p->class_id == JS_CLASS_PY_DESCR)
        return FALSE;
    if (!p->shape->proto || p->shape->proto->class_id != JS_CLASS_PY_TYPE)
        return FALSE;
    t = p->shape->proto->u.opaque;
    /* types saved by reference have no slots until their module ran */
    if (!t || !t->slots.tp_hash || t->slots.tp_hash == qp_slot_tp_hash ||
        qp_capi_snap_is_c_slot((const void *)t->slots.tp_hash))
        return FALSE;
    switch (p->class_id) {
    case JS_CLASS_PY_TUPLE:
        {
            QPTuple *tu = p->u.opaque;
            uint32_t i;
            for (i = 0; tu && i < tu->len; i++) {
                if (!qp_snap_hash_pure(ctx, tu->items[i], depth + 1))
                    return FALSE;
            }
            return TRUE;
        }
    case JS_CLASS_PY_FROZENSET:
        {
            qp_setcore *sc;
            Py_ssize_t i;
            if (!p->u.opaque)
                return FALSE;
            sc = &((QPSet *)p->u.opaque)->core;
            for (i = 0; i <= sc->mask; i++) {
                if (QP_SETENTRY_ACTIVE(&sc->table[i]) &&
                    !qp_snap_hash_pure(ctx, sc->table[i].key, depth + 1))
                    return FALSE;
            }
            return TRUE;
        }
    default:
        return TRUE;
    }
}

/* Before C modules run again (Python code): the dicts and sets whose keys
   are pure get their identity hashes now, so that the module code finds
   e.g. type-keyed entries; the late pass rehashes everything again. */
static void qp_snap_prerehash(QPSnapRead *s)
{
    JSContext *ctx = s->py_ctx;
    int i;

    for (i = 0; i < s->late_count; i++) {
        JSObject *p = s->late[i].obj;
        BOOL pure = TRUE;
        if (s->late[i].func == qp_snap_coll_dict_rehash) {
            QPDict *d = p->u.opaque;
            qp_dictkeys *keys;
            Py_ssize_t j;
            if (!d || d->core.keys == QP_SNAP_COLL_EMPTY_KEYS)
                continue;
            keys = d->core.keys;
            for (j = 0; pure && j < keys->nentries; j++) {
                if (QP_DICTENTRY_ACTIVE(&keys->entries[j]))
                    pure = qp_snap_hash_pure(ctx, keys->entries[j].key, 0);
            }
        } else if (s->late[i].func == qp_snap_coll_set_rehash) {
            QPSet *st = p->u.opaque;
            Py_ssize_t j;
            if (!st)
                continue;
            for (j = 0; pure && j <= st->core.mask; j++) {
                if (QP_SETENTRY_ACTIVE(&st->core.table[j]))
                    pure = qp_snap_hash_pure(ctx, st->core.table[j].key, 0);
            }
        } else {
            continue;
        }
        if (pure && s->late[i].func(ctx, p) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
}
#endif

/* The _thread objects bound to the writer's main thread (see
   qp_snap_thread_bound) name the main thread of this process: the ident
   slot (slot 0 of _ThreadHandle and RLock) gets its ident.  Before any
   Python code runs: a held RLock is held by the running thread. */
static int qp_snap_thread_rebind(SnapReader *r, QPSnapRead *s)
{
    JSContext *ctx = s->py_ctx;
    int i;

    for (i = 0; i < s->thread_bound_count; i++) {
        JSObject *p = s->thread_bound[i];
        JSValue obj = JS_MKPTR(JS_TAG_OBJECT, p), ident;
        QPType *t = p->shape->proto && p->shape->proto->class_id == JS_CLASS_PY_TYPE ?
            p->shape->proto->u.opaque : NULL;
        if (!ctx || !t || !p->u.opaque)
            return snap_error(r, "invalid Python object bound to the main thread");
        for (; t && t->spec != &qp_thread_handle_spec && t->spec != &qp_thread_rlock_spec;
             t = t->base)
            continue;
        if (!t)
            return snap_error(r, "invalid Python object bound to the main thread");
        if (t->spec == &qp_thread_handle_spec && !s->thread_rebound) {
            s->thread_rebound = TRUE;
            s->thread_old_ident = qp_thread_slot_u64(ctx, obj, 0);
        }
        ident = qp_long_from_uint64(ctx, qp_thread_main_ident(ctx));
        if (JS_IsException(ident))
            return snap_error(r, "out of memory");
        qp_thread_set_slot(ctx, obj, 0, ident);
    }
    return 0;
}

/* threading.py keeps the ident of its main Thread (Thread._ident, the key
   of threading._active) and its native id: the main thread of this
   process, like threading._after_fork() does for the thread that forked */
static const char qp_snap_threading_src[] =
    "def rebind(threading, old, new):\n"
    "    main = getattr(threading, '_main_thread', None)\n"
    "    if main is None or getattr(main, '_ident', None) != old:\n"
    "        return\n"
    "    active = getattr(threading, '_active', None)\n"
    "    if type(active) is dict and active.get(old) is main:\n"
    "        del active[old]\n"
    "        active[new] = main\n"
    "    main._ident = new\n"
    "    if getattr(threading, '_HAVE_THREAD_NATIVE_ID', False):\n"
    "        main._set_native_id()\n";

static int qp_snap_threading_rebind(SnapReader *r, QPSnapRead *s)
{
    JSContext *ctx = s->py_ctx;
    JSValue mod = QP_NULL, ns = QP_NULL, f = QP_NULL, res, exc;
    JSValueConst args[3];
    const char *msg;
    JSAtom a;
    int ret;

    if (!s->thread_rebound)
        return 0;
    a = JS_NewAtom(ctx, "threading");
    if (a == JS_ATOM_NULL)
        goto fail;
    ret = qp_dict_get_item_atom(ctx, QP_STATE(ctx)->modules, a, &mod);
    JS_FreeAtom(ctx, a);
    if (ret <= 0) {
        if (ret < 0)
            goto fail;
        return 0;
    }
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns)) {
        ns = QP_NULL;
        goto fail;
    }
    if (qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __builtins__),
                              QP_STATE(ctx)->builtins_module) < 0)
        goto fail;
    res = qp_run_string(ctx, qp_snap_threading_src, sizeof(qp_snap_threading_src) - 1,
                        "<snapshot>", QP_INPUT_FILE, ns, ns);
    if (JS_IsException(res))
        goto fail;
    JS_FreeValue(ctx, res);
    a = JS_NewAtom(ctx, "rebind");
    if (a == JS_ATOM_NULL)
        goto fail;
    ret = qp_dict_get_item_atom(ctx, ns, a, &f);
    JS_FreeAtom(ctx, a);
    if (ret <= 0) {
        if (ret == 0)
            qp_raise_system_error(ctx, "snapshot helper rebind missing");
        goto fail;
    }
    args[0] = mod;
    args[1] = qp_long_from_uint64(ctx, s->thread_old_ident);
    if (JS_IsException(args[1]))
        goto fail;
    args[2] = qp_long_from_uint64(ctx, qp_thread_main_ident(ctx));
    if (JS_IsException(args[2])) {
        JS_FreeValue(ctx, (JSValue)args[1]);
        goto fail;
    }
    res = qp_vectorcall(ctx, f, args, 3, QP_NULL);
    JS_FreeValue(ctx, (JSValue)args[1]);
    JS_FreeValue(ctx, (JSValue)args[2]);
    if (JS_IsException(res))
        goto fail;
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, mod);
    return 0;
 fail:
    exc = JS_GetException(ctx);
    msg = JS_IsNull(exc) || JS_IsUndefined(exc) ? NULL : JS_ToCString(ctx, exc);
    if (!msg)
        JS_FreeValue(ctx, JS_GetException(ctx));
    snap_error(r, "restoring the threading module failed: %s", msg ? msg : "error");
    if (msg)
        JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
    if (!QP_IS_NULL(f))
        JS_FreeValue(ctx, f);
    if (!QP_IS_NULL(ns))
        JS_FreeValue(ctx, ns);
    if (!QP_IS_NULL(mod))
        JS_FreeValue(ctx, mod);
    return -1;
}

/* Called by JS_ReadSnapshot() once every body is read and the object flags
   are restored (the garbage collector stays off until it returns). */
static int qp_snap_finish(SnapReader *r)
{
    QPSnapRead *s = r->py;

    if (!s)
        return 0;
    if (qp_snap_thread_rebind(r, s))
        return -1;
    if (qp_snap_run_deferred(r, s->early, s->early_count))
        return -1;
#ifdef CONFIG_PYTHON_CAPI
    /* C modules, C types and functions, then C-extension instances: runs
       Python code (module execs, reconstructors) */
    if (s->capi) {
        if (!s->py_ctx)
            return snap_error(r, "C extension state without Python state");
        qp_snap_prerehash(s);
        if (qp_capi_snap_finish(r, s->py_ctx))
            return -1;
    }
#endif
    if (qp_snap_run_deferred(r, s->late, s->late_count))
        return -1;
    return qp_snap_threading_rebind(r, s);
}

/* ========================================================================= */
/* Embedding entry points (test hosts)                                       */
/* ========================================================================= */

/* Initialise Python on ctx like the qpy launcher with default options
   (state, importlib boot, __main__).  Returns 0, or -1 after printing the
   error. */
int qp_embed_init(JSContext *ctx)
{
    JSValue m;

    if (qp_state_init(ctx, NULL) < 0) {
        fprintf(stderr, "Python state initialization failed\n");
        return -1;
    }
#ifdef QP_IMPORTLIB_BOOT
    if (qp_importlib_boot(ctx) < 0) {
        qp_handle_uncaught(ctx);
        return -1;
    }
#endif
    m = qp_run_main_module(ctx);
    if (JS_IsException(m)) {
        qp_handle_uncaught(ctx);
        return -1;
    }
    JS_FreeValue(ctx, m);
    return 0;
}

/* Run Python source in the __main__ namespace.  Returns 0, or the exit
   status (SystemExit) / 1 after printing an uncaught exception. */
int qp_embed_exec(JSContext *ctx, const char *src, const char *filename)
{
    JSValue m, d, res;

    m = qp_run_main_module(ctx);
    if (JS_IsException(m))
        return qp_handle_uncaught(ctx);
    d = qp_run_main_dict(ctx, m);
    JS_FreeValue(ctx, m);
    if (JS_IsException(d))
        return qp_handle_uncaught(ctx);
    res = qp_run_string(ctx, src, strlen(src), filename, QP_INPUT_FILE, d, d);
    JS_FreeValue(ctx, d);
    /* the queued __del__ methods and weakref callbacks (see qp_drepl_exec) */
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    if (JS_IsException(res))
        return qp_handle_uncaught(ctx);
    JS_FreeValue(ctx, res);
    return 0;
}
