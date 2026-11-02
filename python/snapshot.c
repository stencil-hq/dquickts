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
 * Host state is rejected (TypeError "snapshot: cannot save Python object
 * ..."): OS threads, directory streams, poll objects, file objects on
 * descriptors other than the standard streams, sockets, processes,
 * C-extension companions (JS_CLASS_PY_NATIVE), running Python code.
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
} QPSnapRead;

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
    js_free_rt(r->rt, s->early);
    js_free_rt(r->rt, s->late);
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
    e = QP_TEXT(t);
    if (t->flags & QP_TPFLAGS_READYING)
        return qp_snap_reject(w, p, "type is being created");
    /* tp_name always comes from the spec (typeobject.c qp_type_ready) */
    if (t->tp_name && (!t->spec || t->tp_name != t->spec->name))
        return qp_snap_reject(w, p, "type name not owned by its spec");
    bc_put_u8(&w->bc, 1);
    if (snap_put_ref(w, SNAP_CELL_CONTEXT, t->ctx) ||
        snap_put_native(w, t->spec))
        return -1;
    bc_put_u8(&w->bc, t->tp_name != NULL);
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
    bc_put_leb128(&w->bc, t->subclass_count);
    for (i = 0; i < t->subclass_count; i++) {
        if (qp_snap_put_type(w, t->subclasses[i]))
            return -1;
    }
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
    if (has_tp_name) {
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
    { &qp_thread_handle_spec, "operating system thread" },
};

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

    if (proto && proto->class_id == JS_CLASS_PY_TYPE && proto->u.opaque) {
        QPType *t = proto->u.opaque;
        const char *reason = qp_snap_host_reason(t->ctx, p, t);
        if (reason)
            return qp_snap_reject(w, p, reason);
    }
    if (!in) {
        bc_put_u8(&w->bc, 0);
        return 0;
    }
    bc_put_u8(&w->bc, 1);
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
    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid Python instance slot count");
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
        return snap_error(r, "unsupported Python C extension object");
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

static int qp_snap_write_code(SnapWriter *w, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    uint32_t i;

    bc_put_u8(&w->bc, b->is_py | ((ci != NULL) << 1));
    if (!ci)
        return 0;
    bc_put_leb128(&w->bc, ci->co_flags);
    bc_put_leb128(&w->bc, ci->argcount);
    bc_put_leb128(&w->bc, ci->posonlyargcount);
    bc_put_leb128(&w->bc, ci->kwonlyargcount);
    bc_put_sleb128(&w->bc, ci->firstlineno);
    if (bc_put_atom(&w->bc, ci->qualname))
        return -1;
    bc_put_sleb128(&w->bc, ci->doc_cpool_idx);
    bc_put_leb128(&w->bc, ci->loc_count);
    for (i = 0; i < ci->loc_count; i++) {
        bc_put_sleb128(&w->bc, ci->locs[i].lineno);
        bc_put_sleb128(&w->bc, ci->locs[i].end_lineno);
        bc_put_sleb128(&w->bc, ci->locs[i].col);
        bc_put_sleb128(&w->bc, ci->locs[i].end_col);
    }
    bc_put_leb128(&w->bc, ci->pcloc_count);
    for (i = 0; i < ci->pcloc_count; i++) {
        bc_put_leb128(&w->bc, ci->pcloc[i].pc);
        bc_put_leb128(&w->bc, ci->pcloc[i].loc);
    }
    /* inline caches: only their count (restored empty) */
    bc_put_leb128(&w->bc, ci->ic_count);
    return 0;
}

static int qp_snap_read_code(SnapReader *r, JSFunctionBytecode *b)
{
    JSContext *ctx = r->ctx;
    QPCodeInfo *ci;
    uint8_t bits;
    uint32_t v, i, n;

    if (bc_get_u8(&r->bc, &bits))
        return -1;
    b->is_py = bits & 1;
    if (!(bits & 2))
        return 0;
    ci = qp_code_info_new(ctx);
    if (!ci)
        return -1;
    b->py_code = ci; /* freed with the bytecode on error */
    if (bc_get_leb128(&r->bc, &ci->co_flags))
        return -1;
    if (bc_get_leb128(&r->bc, &v)) return -1;
    ci->argcount = v;
    if (bc_get_leb128(&r->bc, &v)) return -1;
    ci->posonlyargcount = v;
    if (bc_get_leb128(&r->bc, &v)) return -1;
    ci->kwonlyargcount = v;
    if (qp_snap_get_int(r, &ci->firstlineno) ||
        bc_get_atom(&r->bc, &ci->qualname) ||
        qp_snap_get_int(r, &ci->doc_cpool_idx) ||
        snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid Python location table");
    if (n) {
        ci->locs = js_malloc(ctx, sizeof(ci->locs[0]) * n);
        if (!ci->locs)
            return -1;
        ci->loc_size = n;
        for (i = 0; i < n; i++) {
            QPLoc *l = &ci->locs[i];
            if (qp_snap_get_int(r, &l->lineno) || qp_snap_get_int(r, &l->end_lineno) ||
                qp_snap_get_int(r, &l->col) || qp_snap_get_int(r, &l->end_col))
                return -1;
            ci->loc_count = i + 1;
        }
    }
    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid Python location map");
    if (n) {
        ci->pcloc = js_malloc(ctx, sizeof(ci->pcloc[0]) * n);
        if (!ci->pcloc)
            return -1;
        for (i = 0; i < n; i++) {
            if (bc_get_leb128(&r->bc, &ci->pcloc[i].pc) ||
                bc_get_leb128(&r->bc, &ci->pcloc[i].loc))
                return -1;
            if (ci->pcloc[i].loc >= ci->loc_count)
                return snap_error(r, "invalid Python location map");
            ci->pcloc_count = i + 1;
        }
    }
    if (bc_get_leb128(&r->bc, &v))
        return -1;
    if (v > UINT16_MAX)
        return snap_error(r, "invalid Python inline cache count");
    ci->ic_count = v;
    /* inline caches restored empty, kinds from the bytecode */
    return qp_code_info_setup_ic(ctx, b);
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
    X(asyncgen_firstiter) X(asyncgen_finalizer) X(exc_info_base.exc_value)

/* int fields of QPState (config: the launcher's options; its string
   pointers belong to the launcher process and are not restored) */
#define QP_SNAP_STATE_INTS(X)                                                \
    X(config.optimize) X(config.verbose) X(config.quiet) X(config.isolated)  \
    X(config.ignore_environment) X(config.safe_path) X(config.unbuffered)    \
    X(config.inspect) X(config.interactive) X(config.dont_write_bytecode)    \
    X(config.int_max_str_digits) X(config.lazy_imports)                      \
    X(config.recursion_limit) X(config.no_user_site) X(config.no_site)       \
    X(config.bytes_warning) X(config.utf8_mode) X(config.dev_mode)           \
    X(config.warn_default_encoding)                                          \
    X(recursion_limit) X(int_max_str_digits) X(lazy_imports_mode)           \
    X(import_lock_level) X(override_frozen_modules)

/* QPModState JSValue fields and arrays, in format order */
#define QP_SNAP_MS_VALUES(X)                                                 \
    X(atexit_callbacks) X(sig_default_int_handler) X(sig_dfl) X(sig_ign)     \
    X(sig_itimer_error) X(gc_callbacks) X(gc_garbage) X(warn_filters)        \
    X(warn_once_registry) X(warn_default_action) X(warn_context)             \
    X(ctx_current) X(thread_shutdown_handles) X(codec_search_path)           \
    X(codec_search_cache) X(codec_error_registry) X(codec_encoding_map_type)
#define QP_SNAP_MS_ARRAYS(X)                                                 \
    X(sig_handlers) X(ctx_types) X(thread_types) X(wr_types)                 \
    X(codec_builtin_handlers)
#define QP_SNAP_MS_INTS(X)                                                   \
    X(sig_wakeup_warn_on_full_buffer) X(gc_enabled) X(gc_debug)              \
    X(gc_threshold[0]) X(gc_threshold[1]) X(gc_threshold[2])                 \
    X(warn_lock_count)
#define QP_SNAP_MS_I64(X)                                                    \
    X(gc_frozen) X(gc_collections[0]) X(gc_collections[1])                   \
    X(gc_collections[2]) X(gc_collected[0]) X(gc_collected[1])               \
    X(gc_collected[2]) X(gc_uncollectable[0]) X(gc_uncollectable[1])        \
    X(gc_uncollectable[2]) X(gc_candidates[0]) X(gc_candidates[1])          \
    X(gc_candidates[2]) X(gc_baseline) X(warn_filters_version)

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
#define X(f) bc_put_sleb128(&w->bc, s->f);
    QP_SNAP_STATE_INTS(X)
#undef X
    bc_put_leb128(&w->bc, s->next_type_version);
    bc_put_u64(&w->bc, s->next_dict_uid);
    dbuf_put(&w->bc.dbuf, s->hash_key, sizeof(s->hash_key));
    return qp_snap_write_modstate(w, s->mod_state);
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
    uint8_t present;
    uint64_t uid;
    int i;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    rs = qp_snap_read_state_get(r);
    if (!rs)
        return -1;
    /* same initial values as qp_state_init (finalizer-safe) */
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return -1;
    ctx->py = s;
    s->ctx = ctx;
#define X(f) s->f = JS_UNDEFINED;
    QP_SNAP_STATE_VALUES(X)
#undef X
    s->exc_info = &s->exc_info_base;
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
#define X(f) if (qp_snap_get_int(r, &s->f)) return -1;
    QP_SNAP_STATE_INTS(X)
#undef X
    if (bc_get_leb128(&r->bc, &s->next_type_version) ||
        bc_get_u64(&r->bc, &uid) ||
        bc_get_buf(&r->bc, s->hash_key, sizeof(s->hash_key)))
        return -1;
    s->next_dict_uid = uid;
    if (qp_snap_read_modstate(r, s))
        return -1;
    /* the object slot carries the context (see qp_snap_run_deferred) */
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

/* Called by JS_ReadSnapshot() once every body is read and the object flags
   are restored (the garbage collector stays off until it returns). */
static int qp_snap_finish(SnapReader *r)
{
    QPSnapRead *s = r->py;

    if (!s)
        return 0;
    if (qp_snap_run_deferred(r, s->early, s->early_count))
        return -1;
    return qp_snap_run_deferred(r, s->late, s->late_count);
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
    if (JS_IsException(res))
        return qp_handle_uncaught(ctx);
    JS_FreeValue(ctx, res);
    return 0;
}
