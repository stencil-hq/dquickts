/*
 * _weakref module and the weak reference types (port of CPython
 * Modules/_weakref.c and Objects/weakrefobject.c, PSF-2.0).
 *
 * Storage.  A Python weakref/proxy object is a JS_CLASS_PY_OBJECT instance
 * whose QPInstance slot 0 holds a hidden "holder" object (a private JS
 * class) owning a QPWeakRec (python/modstate.h).  The record keeps a raw
 * pointer to the referent and increments the referent's
 * JSObject.weakref_count (the QuickJS weak reference count, as WeakRef and
 * WeakMap do), so free_object() calls qp_weakref_object_freed() when the
 * referent dies: the records are unlinked and cleared at once (CPython
 * PyObject_ClearWeakRefs), so ref() returns None immediately.  Records are
 * found from the referent through a per-context pointer hash table
 * (CPython keeps the list head in the object, tp_weaklistoffset).
 *
 * Callbacks.  free_object() cannot run Python code, so callbacks are
 * queued (QPModState.wr_pending) in CPython's call order and run by
 * qp_weakref_run_pending(), which qp_signal_check() calls from the
 * interpreter's periodic poll; the poll is requested immediately by
 * setting ctx->interrupt_counter, so a callback runs before the next call
 * or backward jump of the running Python code, and gc.collect() drains
 * the queue before returning.  As in CPython, a weakref that is itself
 * garbage does not get its callback called, and errors are reported with
 * "Exception ignored while calling weakref callback %R" ("... on calling
 * ..." for callbacks queued by the cycle collector, as gc.c reports them).
 *
 * The record also keeps a raw pointer to the weakref object (`owner`) and
 * holds a weakref_count on it, so the owner's JSObject structure outlives
 * the record and liveness can be tested from the free hook
 * (free_mark / GC mark) without touching freed memory.
 */
#include "modstate.h"

/* errors.c (unit i) */
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);

static JSClassID qp_wr_holder_class_id;

enum { QP_WR_T_REF, QP_WR_T_PROXY, QP_WR_T_CALLPROXY };

/* ---- holder class --------------------------------------------------------- */

static void qp_wr_table_remove(QPModState *ms, JSObject *target);
static QPWeakBucket *qp_wr_table_find(QPModState *ms, JSObject *target);

/* Drop the record's count on a JSObject structure, freeing the structure if
   the object is already dead (js_weakref_free). */
static void qp_wr_release_struct(JSRuntime *rt, JSObject *p)
{
    assert(p->weakref_count >= 1);
    p->weakref_count--;
    if (p->weakref_count == 0 && js_rc(p)->ref_count == 0 && js_rc(p)->mark == 0)
        js_free_rt(rt, p);
}

/* Unlink rec from its referent's list (CPython clear_weakref_lock_held
   without the callback part). */
static void qp_wr_unlink(JSRuntime *rt, QPWeakRec *rec)
{
    JSObject *target = rec->target;
    QPModState *ms;
    QPWeakBucket *b;

    if (!target)
        return;
    ms = (rec->ctx && rec->ctx->py) ? rec->ctx->py->mod_state : NULL;
    if (ms) {
        b = qp_wr_table_find(ms, target);
        if (b && b->head == rec)
            b->head = rec->next;
        if (rec->prev)
            rec->prev->next = rec->next;
        if (rec->next)
            rec->next->prev = rec->prev;
        if (b && !b->head)
            qp_wr_table_remove(ms, target);
    }
    rec->prev = rec->next = NULL;
    rec->target = NULL;
    qp_wr_release_struct(rt, target);
}

/* The weakref object died (its slot released the holder). */
static void qp_wr_holder_finalizer(JSRuntime *rt, JSValue val)
{
    QPWeakRec *rec = JS_GetOpaque(val, qp_wr_holder_class_id);
    JSObject *owner;

    if (!rec)
        return;
    qp_wr_unlink(rt, rec);
    owner = rec->owner;
    rec->owner = NULL;
    if (owner)
        qp_wr_release_struct(rt, owner);
    if (rec->pending) {
        /* still queued: the drain sees owner == NULL and frees it */
        return;
    }
    if (!QP_IS_NULL(rec->callback))
        JS_FreeValueRT(rt, rec->callback);
    if (!QP_IS_NULL(rec->pending_cb))
        JS_FreeValueRT(rt, rec->pending_cb);
    js_free_rt(rt, rec);
}

static void qp_wr_holder_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPWeakRec *rec = JS_GetOpaque(val, qp_wr_holder_class_id);

    if (!rec)
        return;
    if (!QP_IS_NULL(rec->callback))
        JS_MarkValue(rt, rec->callback, mark_func);
    /* not rec->pending_cb: a queued callback belongs to the pending queue
       of the context (marked by qp_modstate_mark), not to the holder. The record outlives
       a holder that dies while it is queued (qp_wr_holder_finalizer), so a
       cycle collection freeing the holder must not free the callback the
       drain still releases or calls. */
}

static const JSClassDef qp_wr_holder_class = {
    "WeakRefHolder",
    .finalizer = qp_wr_holder_finalizer,
    .gc_mark = qp_wr_holder_mark,
};

/* ---- referent -> list table (pointer hash, linear probing) ------------- */

#define QP_WR_DELETED ((JSObject *)1)

static uint32_t qp_wr_hash_ptr(JSObject *p)
{
    uintptr_t h = (uintptr_t)p;
    h ^= h >> 17;
    h *= (uintptr_t)0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return (uint32_t)h;
}

static QPWeakBucket *qp_wr_table_find(QPModState *ms, JSObject *target)
{
    uint32_t mask, i;

    if (!ms->wr_size)
        return NULL;
    mask = ms->wr_size - 1;
    for (i = qp_wr_hash_ptr(target) & mask;; i = (i + 1) & mask) {
        QPWeakBucket *b = &ms->wr_table[i];
        if (b->target == target)
            return b;
        if (b->target == NULL)
            return NULL;
    }
}

static void qp_wr_table_remove(QPModState *ms, JSObject *target)
{
    QPWeakBucket *b = qp_wr_table_find(ms, target);
    if (b) {
        b->target = QP_WR_DELETED;
        b->head = NULL;
        ms->wr_count--;
    }
}

static int qp_wr_table_resize(JSContext *ctx, QPModState *ms, uint32_t new_size)
{
    QPWeakBucket *nt, *old = ms->wr_table;
    uint32_t i, old_size = ms->wr_size, mask = new_size - 1;

    nt = js_mallocz(ctx, sizeof(*nt) * new_size);
    if (!nt)
        return -1;
    for (i = 0; i < old_size; i++) {
        QPWeakBucket *b = &old[i];
        uint32_t j;
        if (b->target == NULL || b->target == QP_WR_DELETED)
            continue;
        for (j = qp_wr_hash_ptr(b->target) & mask; nt[j].target; j = (j + 1) & mask)
            ;
        nt[j] = *b;
    }
    js_free(ctx, old);
    ms->wr_table = nt;
    ms->wr_size = new_size;
    ms->wr_used = ms->wr_count;
    return 0;
}

/* Bucket of target, created empty if absent (NULL + exception on OOM). */
static QPWeakBucket *qp_wr_table_insert(JSContext *ctx, QPModState *ms, JSObject *target)
{
    QPWeakBucket *b = qp_wr_table_find(ms, target);
    uint32_t mask, i;

    if (b)
        return b;
    if ((ms->wr_used + 1) * 4 >= ms->wr_size * 3) {
        uint32_t ns = ms->wr_size ? ms->wr_size : 16;
        while ((ms->wr_count + 1) * 2 >= ns)
            ns *= 2;
        if (qp_wr_table_resize(ctx, ms, ns) < 0) {
            qp_raise_memory_error(ctx);
            return NULL;
        }
    }
    mask = ms->wr_size - 1;
    for (i = qp_wr_hash_ptr(target) & mask;; i = (i + 1) & mask) {
        b = &ms->wr_table[i];
        if (b->target == NULL || b->target == QP_WR_DELETED) {
            if (b->target == NULL)
                ms->wr_used++;
            b->target = target;
            b->head = NULL;
            ms->wr_count++;
            return b;
        }
    }
}

/* ---- types ---------------------------------------------------------------- */

static const QPTypeSpec qp_wr_ref_spec;
static const QPTypeSpec qp_wr_proxy_spec;
static const QPTypeSpec qp_wr_callproxy_spec;

/* Module state with the three weakref types created (NULL + exception). */
static QPModState *qp_wr_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    JSRuntime *rt = ctx->rt;
    static const QPTypeSpec *const specs[3] = {
        &qp_wr_ref_spec, &qp_wr_proxy_spec, &qp_wr_callproxy_spec,
    };
    int i;

    if (!ms)
        return NULL;
    if (!QP_IS_NULL(ms->wr_types[QP_WR_T_CALLPROXY]))
        return ms;
    if (qp_wr_holder_class_id == 0)
        JS_NewClassID(&qp_wr_holder_class_id);
    if (!JS_IsRegisteredClass(rt, qp_wr_holder_class_id) &&
        JS_NewClass(rt, qp_wr_holder_class_id, &qp_wr_holder_class) < 0) {
        qp_raise_memory_error(ctx);
        return NULL;
    }
    for (i = 0; i < 3; i++) {
        if (QP_IS_NULL(ms->wr_types[i])) {
            JSValue t = qp_type_from_spec(ctx, specs[i]);
            if (JS_IsException(t))
                return NULL;
            ms->wr_types[i] = t;
        }
    }
    return ms;
}

static QPType *qp_wr_type(QPModState *ms, int which)
{
    return qp_type_from_value(ms->wr_types[which]);
}

/* Record of a weakref/proxy object, NULL if v is not one (no exception). */
static QPWeakRec *qp_wr_rec(JSContext *ctx, JSValueConst v)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    JSValue *slots;
    QPType *t;

    if (!ms || QP_IS_NULL(ms->wr_types[QP_WR_T_CALLPROXY]) ||
        JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_OBJECT)
        return NULL;
    t = qp_type_of(ctx, v);
    if (!qp_type_is_subtype(t, qp_wr_type(ms, QP_WR_T_REF)) &&
        t != qp_wr_type(ms, QP_WR_T_PROXY) && t != qp_wr_type(ms, QP_WR_T_CALLPROXY))
        return NULL;
    slots = qp_instance_slots(v);
    if (!slots || JS_VALUE_GET_TAG(slots[0]) != JS_TAG_OBJECT)
        return NULL;
    return JS_GetOpaque(slots[0], qp_wr_holder_class_id);
}

static BOOL qp_wr_is_proxy(QPWeakRec *rec)
{
    return rec->kind != QP_WR_KIND_REF;
}

/* Referent of rec: new reference, or QP_NULL if dead. */
static JSValue qp_wr_get_ref(QPWeakRec *rec)
{
    JSObject *p = rec->target;
    if (!p)
        return QP_NULL;
    return JS_DupValueRT(rec->ctx->rt, JS_MKPTR(JS_TAG_OBJECT, p));
}

/* CPython _PyType_SUPPORTS_WEAKREFS */
static BOOL qp_wr_supports(JSContext *ctx, JSValueConst obj)
{
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return FALSE;
    return (qp_type_of(ctx, obj)->flags & QP_TPFLAGS_WEAKREF) != 0;
}

/* tp_name of a type as str (CPython "'%s'" with tp_name). */
static JSValue qp_wr_tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

static BOOL qp_wr_is_basic_ref(JSContext *ctx, QPModState *ms, QPWeakRec *rec)
{
    return QP_IS_NULL(rec->callback) && rec->kind == QP_WR_KIND_REF && rec->owner &&
        qp_type_of(ctx, JS_MKPTR(JS_TAG_OBJECT, rec->owner)) == qp_wr_type(ms, QP_WR_T_REF);
}

static BOOL qp_wr_is_basic_proxy(QPWeakRec *rec)
{
    return QP_IS_NULL(rec->callback) && rec->kind != QP_WR_KIND_REF;
}

/* CPython get_basic_refs */
static void qp_wr_get_basic_refs(JSContext *ctx, QPModState *ms, QPWeakRec *head,
                                 QPWeakRec **refp, QPWeakRec **proxyp)
{
    *refp = NULL;
    *proxyp = NULL;
    if (head && QP_IS_NULL(head->callback)) {
        if (qp_wr_is_basic_ref(ctx, ms, head)) {
            *refp = head;
            head = head->next;
        }
        if (head && QP_IS_NULL(head->callback) && head->kind != QP_WR_KIND_REF)
            *proxyp = head;
    }
}

/* CPython insert_weakref */
static void qp_wr_insert(JSContext *ctx, QPModState *ms, QPWeakBucket *b, QPWeakRec *newref)
{
    QPWeakRec *ref, *proxy, *prev;

    qp_wr_get_basic_refs(ctx, ms, b->head, &ref, &proxy);
    if (qp_wr_is_basic_ref(ctx, ms, newref))
        prev = NULL;
    else if (qp_wr_is_basic_proxy(newref))
        prev = ref;
    else
        prev = proxy ? proxy : ref;
    if (!prev) {
        newref->prev = NULL;
        newref->next = b->head;
        if (b->head)
            b->head->prev = newref;
        b->head = newref;
    } else {
        newref->prev = prev;
        newref->next = prev->next;
        if (prev->next)
            prev->next->prev = newref;
        prev->next = newref;
    }
}

/* CPython get_or_create_weakref: type = ref type or subclass (kind REF) or
   one of the proxy types. */
static JSValue qp_wr_get_or_create(JSContext *ctx, QPType *type, JSValueConst obj,
                                   JSValueConst callback)
{
    QPModState *ms = qp_wr_state(ctx);
    QPWeakBucket *b;
    QPWeakRec *rec, *ref, *proxy;
    JSValue self, holder, *slots;
    JSObject *target;
    int kind;

    if (!ms)
        return JS_EXCEPTION;
    if (!qp_wr_supports(ctx, obj)) {
        JSValue name = qp_wr_tp_name(ctx, qp_type_of(ctx, obj));
        if (JS_IsException(name))
            return JS_EXCEPTION;
        qp_raise_type_error(ctx, "cannot create weak reference to '%U' object", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(callback) && JS_IsUndefined(callback))
        callback = QP_NULL;
    if (type == qp_wr_type(ms, QP_WR_T_PROXY))
        kind = QP_WR_KIND_PROXY;
    else if (type == qp_wr_type(ms, QP_WR_T_CALLPROXY))
        kind = QP_WR_KIND_CALLPROXY;
    else
        kind = QP_WR_KIND_REF;
    target = JS_VALUE_GET_OBJ(obj);

    /* try_reuse_basic_ref */
    b = qp_wr_table_find(ms, target);
    if (b && QP_IS_NULL(callback)) {
        QPWeakRec *cand = NULL;
        qp_wr_get_basic_refs(ctx, ms, b->head, &ref, &proxy);
        if (type == qp_wr_type(ms, QP_WR_T_REF))
            cand = ref;
        else if (kind != QP_WR_KIND_REF)
            cand = proxy;
        if (cand && cand->owner && !cand->owner->free_mark)
            return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, cand->owner));
    }

    self = qp_object_new_instance(ctx, type, JS_CLASS_PY_OBJECT);
    if (JS_IsException(self))
        return JS_EXCEPTION;
    slots = qp_instance_slots(self);
    if (!slots) {
        JS_FreeValue(ctx, self);
        return qp_raise_system_error(ctx, "weakref instance without slots");
    }
    rec = js_mallocz(ctx, sizeof(*rec));
    if (!rec) {
        JS_FreeValue(ctx, self);
        return JS_EXCEPTION;
    }
    rec->ctx = ctx;
    rec->kind = kind;
    rec->hash = -1;
    rec->callback = QP_NULL;
    rec->pending_cb = QP_NULL;
    holder = JS_NewObjectProtoClass(ctx, JS_NULL, qp_wr_holder_class_id);
    if (JS_IsException(holder)) {
        js_free(ctx, rec);
        JS_FreeValue(ctx, self);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(holder, rec);
    /* the holder now owns rec */
    rec->owner = JS_VALUE_GET_OBJ(self);
    rec->owner->weakref_count++;
    JS_FreeValue(ctx, slots[0]);
    slots[0] = holder;
    if (!QP_IS_NULL(callback))
        rec->callback = JS_DupValue(ctx, callback);

    b = qp_wr_table_insert(ctx, ms, target);
    if (!b) {
        JS_FreeValue(ctx, self);
        return JS_EXCEPTION;
    }
    rec->target = target;
    target->weakref_count++;
    qp_wr_insert(ctx, ms, b, rec);
    return self;
}

/* ---- public C API (same translation unit) ----------------------------------
   static JSValue qp_weakref_new(JSContext *ctx, JSValueConst obj, JSValueConst callback);
       PyWeakref_NewRef (callback QP_NULL or None = none)
   static JSValue qp_weakref_new_proxy(JSContext *ctx, JSValueConst obj, JSValueConst callback);
   static BOOL qp_weakref_check(JSContext *ctx, JSValueConst v);
       PyWeakref_Check (ref, subclasses, proxies)
   static JSValue qp_weakref_get(JSContext *ctx, JSValueConst ref);
       referent of ref (new reference), None if dead
   static JSValue qp_weakref_get_first(JSContext *ctx, JSValueConst obj);
       head of obj's weakref list (subtype __weakref__ getter) or None
   static void qp_weakref_run_pending(JSContext *ctx);
       run queued callbacks (called by qp_signal_check and gc.collect) */

static JSValue qp_weakref_new(JSContext *ctx, JSValueConst obj, JSValueConst callback)
{
    QPModState *ms = qp_wr_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return qp_wr_get_or_create(ctx, qp_wr_type(ms, QP_WR_T_REF), obj, callback);
}

static JSValue qp_weakref_new_proxy(JSContext *ctx, JSValueConst obj, JSValueConst callback)
{
    QPModState *ms = qp_wr_state(ctx);
    int which;
    if (!ms)
        return JS_EXCEPTION;
    which = JS_IsFunction(ctx, obj) ? QP_WR_T_CALLPROXY : QP_WR_T_PROXY;
    if (which == QP_WR_T_PROXY && qp_type_of(ctx, obj)->slots.tp_call)
        which = QP_WR_T_CALLPROXY;
    return qp_wr_get_or_create(ctx, qp_wr_type(ms, which), obj, callback);
}

static BOOL qp_weakref_check(JSContext *ctx, JSValueConst v)
{
    return qp_wr_rec(ctx, v) != NULL;
}

static JSValue qp_weakref_get(JSContext *ctx, JSValueConst ref)
{
    QPWeakRec *rec = qp_wr_rec(ctx, ref);
    JSValue v;

    if (!rec)
        return qp_raise_type_error(ctx, "expected a weakref");
    v = qp_wr_get_ref(rec);
    return QP_IS_NULL(v) ? JS_UNDEFINED : v;
}

static JSValue qp_weakref_get_first(JSContext *ctx, JSValueConst obj)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    QPWeakBucket *b;
    QPWeakRec *r;

    if (!ms || JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    b = qp_wr_table_find(ms, JS_VALUE_GET_OBJ(obj));
    if (!b)
        return JS_UNDEFINED;
    for (r = b->head; r; r = r->next) {
        if (r->owner && !r->owner->free_mark)
            return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, r->owner));
    }
    return JS_UNDEFINED;
}

/* ---- referent death -------------------------------------------------------- */

/* The weakref object is dying or dead (cycle collection in progress, or
   queued in a free cascade). */
static BOOL qp_wr_owner_dying(JSObject *owner)
{
    return owner == NULL || owner->free_mark || js_rc(owner)->mark != 0;
}

/* rec was cleared: queue its callback unless the weakref is garbage too.
   Returns TRUE if queued. */
static BOOL qp_wr_queue_callback(JSRuntime *rt, QPModState *ms, QPWeakRec *rec)
{
    if (QP_IS_NULL(rec->callback))
        return FALSE;
    if (qp_wr_owner_dying(rec->owner) || rec->pending) {
        /* the weakref is garbage too: no callback (CPython
           handle_weakref_callbacks); its holder releases the callback */
        return FALSE;
    }
    rec->pending_cb = rec->callback;
    rec->callback = QP_NULL;
    rec->pending = 1;
    rec->pending_from_gc = rt->gc_phase == JS_GC_PHASE_REMOVE_CYCLES;
    rec->pending_next = NULL;
    if (ms->wr_pending_tail)
        ms->wr_pending_tail->pending_next = rec;
    else
        ms->wr_pending_head = rec;
    ms->wr_pending_tail = rec;
    return TRUE;
}

/* gc.c handle_weakref_callbacks: before the finalizers of the cyclic
   garbage run, the weakrefs with a callback to the unreachable object p
   are cleared and the callbacks of those that are not garbage too are
   queued.  The weakrefs without callback stay usable by the finalizers;
   qp_weakref_object_freed() clears them. */
static void qp_weakref_clear_callback_refs(JSRuntime *rt, JSObject *p)
{
    struct list_head *el;

    list_for_each(el, &rt->context_list) {
        JSContext *ctx = list_entry(el, JSContext, link);
        QPModState *ms;
        QPWeakBucket *b;
        QPWeakRec *rec, *next;
        BOOL queued = FALSE;

        if (!ctx->py || !(ms = ctx->py->mod_state) || !ms->wr_size)
            continue;
        b = qp_wr_table_find(ms, p);
        if (!b)
            continue;
        for (rec = b->head; rec; rec = next) {
            next = rec->next;
            if (QP_IS_NULL(rec->callback))
                continue;
            qp_wr_unlink(rt, rec);
            if (qp_wr_queue_callback(rt, ms, rec))
                queued = TRUE;
        }
        if (queued)
            ctx->interrupt_counter = 1;   /* prompt qp_signal_check() */
    }
}

static void qp_weakref_object_freed(JSRuntime *rt, JSObject *p)
{
    struct list_head *el;

    list_for_each(el, &rt->context_list) {
        JSContext *ctx = list_entry(el, JSContext, link);
        QPModState *ms;
        QPWeakBucket *b;
        QPWeakRec *rec, *next;
        BOOL queued = FALSE;

        if (!ctx->py || !(ms = ctx->py->mod_state) || !ms->wr_size)
            continue;
        b = qp_wr_table_find(ms, p);
        if (!b)
            continue;
        rec = b->head;
        qp_wr_table_remove(ms, p);
        /* PyObject_ClearWeakRefs: clear every reference in list order and
           take its callback; callbacks of live weakrefs are queued in the
           same order. */
        for (; rec; rec = next) {
            next = rec->next;
            rec->prev = rec->next = NULL;
            rec->target = NULL;
            assert(p->weakref_count >= 1);
            p->weakref_count--;
            if (qp_wr_queue_callback(rt, ms, rec))
                queued = TRUE;
        }
        if (queued)
            ctx->interrupt_counter = 1;   /* prompt qp_signal_check() */
    }
}

/* _PyWeakref_ClearWeakRefsNoCallbacks (PyUnstable_Object_ClearWeakRefsNoCallbacks):
   clear every weak reference to the live object p and drop the callbacks
   without calling them. */
static void qp_weakref_clear_no_callbacks(JSRuntime *rt, JSObject *p)
{
    struct list_head *el;

    list_for_each(el, &rt->context_list) {
        JSContext *ctx = list_entry(el, JSContext, link);
        QPModState *ms;
        QPWeakBucket *b;
        QPWeakRec *rec, *next;

        if (!ctx->py || !(ms = ctx->py->mod_state) || !ms->wr_size)
            continue;
        b = qp_wr_table_find(ms, p);
        if (!b)
            continue;
        rec = b->head;
        qp_wr_table_remove(ms, p);
        for (; rec; rec = next) {
            JSValue cb = rec->callback;
            next = rec->next;
            rec->prev = rec->next = NULL;
            rec->target = NULL;
            assert(p->weakref_count >= 1);
            p->weakref_count--;
            rec->callback = QP_NULL;
            if (!QP_IS_NULL(cb))
                JS_FreeValueRT(rt, cb);
        }
    }
}

/* CPython handle_callback for every queued record. */
static void qp_weakref_run_pending(JSContext *ctx)
{
    QPModState *ms = ctx->py ? ctx->py->mod_state : NULL;
    QPWeakRec *rec;

    if (!ms)
        return;
    while ((rec = ms->wr_pending_head) != NULL) {
        JSValue cb, self, res, saved = QP_NULL;
        BOOL from_gc;

        ms->wr_pending_head = rec->pending_next;
        if (!ms->wr_pending_head)
            ms->wr_pending_tail = NULL;
        rec->pending_next = NULL;
        rec->pending = 0;
        cb = rec->pending_cb;
        rec->pending_cb = QP_NULL;
        if (!rec->owner) {
            /* the weakref died while queued */
            if (!QP_IS_NULL(cb))
                JS_FreeValue(ctx, cb);
            if (!QP_IS_NULL(rec->callback))
                JS_FreeValue(ctx, rec->callback);
            js_free(ctx, rec);
            continue;
        }
        if (QP_IS_NULL(cb))
            continue;
        self = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, rec->owner));
        if (JS_HasException(ctx))
            saved = JS_GetException(ctx);
        from_gc = rec->pending_from_gc;
        res = qp_call1(ctx, cb, self);
        if (JS_IsException(res)) {
            /* gc.c handle_weakrefs vs weakrefobject.c handle_callback */
            qp_format_unraisable(ctx, from_gc ?
                                 "Exception ignored on calling weakref callback %R" :
                                 "Exception ignored while calling weakref callback %R", cb);
        } else {
            JS_FreeValue(ctx, res);
        }
        if (!QP_IS_NULL(saved))
            JS_Throw(ctx, saved);
        JS_FreeValue(ctx, self);
        JS_FreeValue(ctx, cb);
    }
}

/* ---- ReferenceType --------------------------------------------------------- */

static JSValue qp_wr_ref_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    /* PyArg_UnpackTuple(args, "__new__", 1, 2): keywords are ignored here
       (tp_init rejects them) */
    if (qp_arg_check_positional(ctx, "__new__", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    return qp_wr_get_or_create(ctx, subtype, args[0], nargs > 1 ? args[1] : QP_NULL);
}

static int qp_wr_ref_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    if (!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) > 0) {
        qp_raise_type_error(ctx, "ref() takes no keyword arguments");
        return -1;
    }
    return qp_arg_check_positional(ctx, "__init__", nargs, 1, 2);
}

static JSValue qp_wr_ref_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    QPWeakRec *rec = qp_wr_rec(ctx, self);
    JSValue v;

    if (!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) > 0)
        return qp_raise_type_error(ctx, "weakref() takes no keyword arguments");
    if (qp_arg_check_positional(ctx, "weakref", (int)qp_nargs(nargsf), 0, 0) < 0)
        return JS_EXCEPTION;
    if (!rec)
        return JS_UNDEFINED;
    v = qp_wr_get_ref(rec);
    return QP_IS_NULL(v) ? JS_UNDEFINED : v;
}

static Py_hash_t qp_wr_ref_hash(JSContext *ctx, JSValueConst self)
{
    QPWeakRec *rec = qp_wr_rec(ctx, self);
    JSValue obj;
    Py_hash_t h;

    if (!rec) {
        qp_raise_type_error(ctx, "weak object has gone away");
        return -1;
    }
    if (rec->hash != -1)
        return rec->hash;
    obj = qp_wr_get_ref(rec);
    if (QP_IS_NULL(obj)) {
        qp_raise_type_error(ctx, "weak object has gone away");
        return -1;
    }
    h = qp_hash(ctx, obj);
    JS_FreeValue(ctx, obj);
    rec->hash = h;
    return h;
}

static JSValue qp_wr_ref_repr(JSContext *ctx, JSValueConst self)
{
    QPWeakRec *rec = qp_wr_rec(ctx, self);
    JSValue obj = rec ? qp_wr_get_ref(rec) : QP_NULL;
    JSValue name, repr;

    if (QP_IS_NULL(obj))
        return qp_str_from_format(ctx, "<weakref at %p; dead>", JS_VALUE_GET_PTR(self));
    name = qp_lookup_special(ctx, obj, QP_ATOM(ctx, __name__));
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (QP_IS_NULL(name) || !qp_str_check(ctx, name))
        repr = qp_str_from_format(ctx, "<weakref at %p; to '%T' at %p>",
                                  JS_VALUE_GET_PTR(self), obj, JS_VALUE_GET_PTR(obj));
    else
        repr = qp_str_from_format(ctx, "<weakref at %p; to '%T' at %p (%U)>",
                                  JS_VALUE_GET_PTR(self), obj, JS_VALUE_GET_PTR(obj), name);
    JS_FreeValue(ctx, obj);
    if (!QP_IS_NULL(name))
        JS_FreeValue(ctx, name);
    return repr;
}

/* Weak references only support equality, not ordering.  Two weak
   references are equal if the underlying objects are equal.  If the
   underlying object has gone away, they are equal if they are identical. */
static JSValue qp_wr_ref_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other, int op)
{
    QPWeakRec *ra, *rb;
    JSValue a, b, res;

    if (op != QP_CMP_EQ && op != QP_CMP_NE)
        return qp_not_implemented(ctx);
    ra = qp_wr_rec(ctx, self);
    rb = qp_wr_rec(ctx, other);
    if (!ra || !rb)
        return qp_not_implemented(ctx);
    a = qp_wr_get_ref(ra);
    b = qp_wr_get_ref(rb);
    if (QP_IS_NULL(a) || QP_IS_NULL(b)) {
        BOOL r = JS_VALUE_GET_PTR(self) == JS_VALUE_GET_PTR(other);
        if (!QP_IS_NULL(a))
            JS_FreeValue(ctx, a);
        if (!QP_IS_NULL(b))
            JS_FreeValue(ctx, b);
        if (op == QP_CMP_NE)
            r = !r;
        return qp_bool_new(r);
    }
    res = qp_richcompare(ctx, a, b, op);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return res;
}

static JSValue qp_wr_get_callback(JSContext *ctx, JSValueConst self, int magic)
{
    QPWeakRec *rec = qp_wr_rec(ctx, self);
    if (!rec || QP_IS_NULL(rec->callback))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, rec->callback);
}

static JSValue qp_wr_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_wr_ref_methods[] = {
    { "__class_getitem__", qp_wr_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Weakrefs are generic over the type of the referenced object." },
    { NULL }
};

static const QPGetSetDef qp_wr_ref_getsets[] = {
    { "__callback__", qp_wr_get_callback, NULL, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_wr_ref_spec = {
    .name = "weakref.ReferenceType",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_repr = qp_wr_ref_repr,
        .tp_hash = qp_wr_ref_hash,
        .tp_call = qp_wr_ref_call,
        .tp_richcompare = qp_wr_ref_richcompare,
        .tp_init = qp_wr_ref_init,
        .tp_new = qp_wr_ref_new,
    },
    .methods = qp_wr_ref_methods,
    .getsets = qp_wr_ref_getsets,
};

/* ---- ProxyType / CallableProxyType ---------------------------------------- */

/* Referent of a live proxy (new ref), ReferenceError for a dead one. */
static JSValue qp_wr_proxy_ref(JSContext *ctx, JSValueConst proxy)
{
    QPWeakRec *rec = qp_wr_rec(ctx, proxy);
    JSValue v = rec ? qp_wr_get_ref(rec) : QP_NULL;
    if (QP_IS_NULL(v))
        return qp_raise(ctx, QP_EXC(ctx, ReferenceError),
                        "weakly-referenced object no longer exists");
    return v;
}

/* CPython proxy_unwrap: new ref to a live proxy's referent, or to the
   object itself if it is not a proxy. */
static JSValue qp_wr_unwrap(JSContext *ctx, JSValueConst obj)
{
    QPWeakRec *rec = qp_wr_rec(ctx, obj);
    if (rec && qp_wr_is_proxy(rec))
        return qp_wr_proxy_ref(ctx, obj);
    return JS_DupValue(ctx, obj);
}

static JSValue qp_wr_proxy_getattr(JSContext *ctx, JSValueConst proxy, JSAtom name)
{
    JSValue o = qp_wr_unwrap(ctx, proxy), res;
    if (JS_IsException(o))
        return o;
    res = qp_getattr(ctx, o, name);
    JS_FreeValue(ctx, o);
    return res;
}

static int qp_wr_proxy_setattr(JSContext *ctx, JSValueConst proxy, JSAtom name, JSValueConst v)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy);
    int res;
    if (JS_IsException(o))
        return -1;
    res = QP_IS_NULL(v) ? qp_delattr(ctx, o, name) : qp_setattr(ctx, o, name, v);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_repr(JSContext *ctx, JSValueConst proxy)
{
    QPWeakRec *rec = qp_wr_rec(ctx, proxy);
    JSValue obj = rec ? qp_wr_get_ref(rec) : QP_NULL, repr;
    if (QP_IS_NULL(obj))
        return qp_str_from_format(ctx, "<weakproxy at %p; dead>", JS_VALUE_GET_PTR(proxy));
    repr = qp_str_from_format(ctx, "<weakproxy at %p; to '%T' at %p>",
                              JS_VALUE_GET_PTR(proxy), obj, JS_VALUE_GET_PTR(obj));
    JS_FreeValue(ctx, obj);
    return repr;
}

static JSValue qp_wr_proxy_str(JSContext *ctx, JSValueConst proxy)
{
    JSValue o = qp_wr_unwrap(ctx, proxy), res;
    if (JS_IsException(o))
        return o;
    res = qp_str(ctx, o);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_call(JSContext *ctx, JSValueConst proxy, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    JSValue o = qp_wr_unwrap(ctx, proxy), res;
    if (JS_IsException(o))
        return o;
    res = qp_vectorcall(ctx, o, args, qp_nargs(nargsf), kwnames);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_richcompare(JSContext *ctx, JSValueConst proxy, JSValueConst v, int op)
{
    JSValue a, b, res;
    a = qp_wr_unwrap(ctx, proxy);
    if (JS_IsException(a))
        return a;
    b = qp_wr_unwrap(ctx, v);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    res = qp_richcompare(ctx, a, b, op);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return res;
}

/* binary number slots through qp_binop */
static JSValue qp_wr_proxy_binop(JSContext *ctx, JSValueConst x, JSValueConst y, int nb)
{
    JSValue a, b, res;
    a = qp_wr_unwrap(ctx, x);
    if (JS_IsException(a))
        return a;
    b = qp_wr_unwrap(ctx, y);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    res = qp_binop(ctx, a, b, nb);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return res;
}

#define QP_WR_BINARY(fname, nb)                                              \
    static JSValue fname(JSContext *ctx, JSValueConst x, JSValueConst y)     \
    {                                                                        \
        return qp_wr_proxy_binop(ctx, x, y, nb);                             \
    }

QP_WR_BINARY(qp_wr_proxy_add, QP_NB_ADD)
QP_WR_BINARY(qp_wr_proxy_sub, QP_NB_SUBTRACT)
QP_WR_BINARY(qp_wr_proxy_mul, QP_NB_MULTIPLY)
QP_WR_BINARY(qp_wr_proxy_floor_div, QP_NB_FLOOR_DIVIDE)
QP_WR_BINARY(qp_wr_proxy_true_div, QP_NB_TRUE_DIVIDE)
QP_WR_BINARY(qp_wr_proxy_mod, QP_NB_REMAINDER)
QP_WR_BINARY(qp_wr_proxy_lshift, QP_NB_LSHIFT)
QP_WR_BINARY(qp_wr_proxy_rshift, QP_NB_RSHIFT)
QP_WR_BINARY(qp_wr_proxy_and, QP_NB_AND)
QP_WR_BINARY(qp_wr_proxy_xor, QP_NB_XOR)
QP_WR_BINARY(qp_wr_proxy_or, QP_NB_OR)
QP_WR_BINARY(qp_wr_proxy_matmul, QP_NB_MATRIX_MULTIPLY)
QP_WR_BINARY(qp_wr_proxy_iadd, QP_NB_INPLACE_ADD)
QP_WR_BINARY(qp_wr_proxy_isub, QP_NB_INPLACE_SUBTRACT)
QP_WR_BINARY(qp_wr_proxy_imul, QP_NB_INPLACE_MULTIPLY)
QP_WR_BINARY(qp_wr_proxy_ifloor_div, QP_NB_INPLACE_FLOOR_DIVIDE)
QP_WR_BINARY(qp_wr_proxy_itrue_div, QP_NB_INPLACE_TRUE_DIVIDE)
QP_WR_BINARY(qp_wr_proxy_imod, QP_NB_INPLACE_REMAINDER)
QP_WR_BINARY(qp_wr_proxy_ilshift, QP_NB_INPLACE_LSHIFT)
QP_WR_BINARY(qp_wr_proxy_irshift, QP_NB_INPLACE_RSHIFT)
QP_WR_BINARY(qp_wr_proxy_iand, QP_NB_INPLACE_AND)
QP_WR_BINARY(qp_wr_proxy_ixor, QP_NB_INPLACE_XOR)
QP_WR_BINARY(qp_wr_proxy_ior, QP_NB_INPLACE_OR)
QP_WR_BINARY(qp_wr_proxy_imatmul, QP_NB_INPLACE_MATRIX_MULTIPLY)

/* Call builtins.<name>(args...) (PyNumber_Divmod / PyNumber_Absolute
   semantics, which the builtins implement exactly). */
static JSValue qp_wr_call_builtin(JSContext *ctx, const char *name, int argc, JSValueConst *argv)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue fn, res;
    int r;

    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, QP_STATE(ctx)->builtins_dict, atom, &fn);
    JS_FreeAtom(ctx, atom);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_system_error(ctx, "builtins.%s is missing", name);
    res = qp_vectorcall(ctx, fn, argv, argc, QP_NULL);
    JS_FreeValue(ctx, fn);
    return res;
}

static JSValue qp_wr_proxy_divmod(JSContext *ctx, JSValueConst x, JSValueConst y)
{
    JSValue v[2], res;
    v[0] = qp_wr_unwrap(ctx, x);
    if (JS_IsException(v[0]))
        return v[0];
    v[1] = qp_wr_unwrap(ctx, y);
    if (JS_IsException(v[1])) {
        JS_FreeValue(ctx, v[0]);
        return v[1];
    }
    res = qp_wr_call_builtin(ctx, "divmod", 2, (JSValueConst *)v);
    JS_FreeValue(ctx, v[0]);
    JS_FreeValue(ctx, v[1]);
    return res;
}

static JSValue qp_wr_proxy_ternary(JSContext *ctx, JSValueConst proxy, JSValueConst v,
                                   JSValueConst w, BOOL inplace)
{
    JSValue a, b, c = QP_NULL, res;
    a = qp_wr_unwrap(ctx, proxy);
    if (JS_IsException(a))
        return a;
    b = qp_wr_unwrap(ctx, v);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    if (!QP_IS_NULL(w)) {
        c = qp_wr_unwrap(ctx, w);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
            return c;
        }
    }
    if (inplace && (QP_IS_NULL(c) || JS_IsUndefined(c)))
        res = qp_binop(ctx, a, b, QP_NB_INPLACE_POWER);
    else
        res = qp_power(ctx, a, b, QP_IS_NULL(c) ? JS_UNDEFINED : c);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    if (!QP_IS_NULL(c))
        JS_FreeValue(ctx, c);
    return res;
}

static JSValue qp_wr_proxy_pow(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c)
{
    return qp_wr_proxy_ternary(ctx, a, b, c, FALSE);
}

static JSValue qp_wr_proxy_ipow(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c)
{
    return qp_wr_proxy_ternary(ctx, a, b, c, TRUE);
}

static JSValue qp_wr_proxy_unop(JSContext *ctx, JSValueConst proxy, int which)
{
    JSValue o = qp_wr_unwrap(ctx, proxy), res;
    if (JS_IsException(o))
        return o;
    switch (which) {
    case 0: res = qp_unop(ctx, o, QP_UNOP_NEG); break;
    case 1: res = qp_unop(ctx, o, QP_UNOP_POS); break;
    case 2: res = qp_unop(ctx, o, QP_UNOP_INVERT); break;
    case 3: res = qp_wr_call_builtin(ctx, "abs", 1, (JSValueConst *)&o); break;
    case 4: res = qp_number_long(ctx, o); break;
    case 5: res = qp_number_float(ctx, o); break;
    default: res = qp_index(ctx, o); break;
    }
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_neg(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 0); }
static JSValue qp_wr_proxy_pos(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 1); }
static JSValue qp_wr_proxy_invert(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 2); }
static JSValue qp_wr_proxy_abs(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 3); }
static JSValue qp_wr_proxy_int(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 4); }
static JSValue qp_wr_proxy_float(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 5); }
static JSValue qp_wr_proxy_index(JSContext *ctx, JSValueConst a) { return qp_wr_proxy_unop(ctx, a, 6); }

static int qp_wr_proxy_bool(JSContext *ctx, JSValueConst proxy)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy);
    int res;
    if (JS_IsException(o))
        return -1;
    res = qp_bool(ctx, o);
    JS_FreeValue(ctx, o);
    return res;
}

static int qp_wr_proxy_contains(JSContext *ctx, JSValueConst proxy, JSValueConst value)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy);
    int res;
    if (JS_IsException(o))
        return -1;
    res = qp_contains(ctx, o, value);
    JS_FreeValue(ctx, o);
    return res;
}

static Py_ssize_t qp_wr_proxy_length(JSContext *ctx, JSValueConst proxy)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy);
    Py_ssize_t res;
    if (JS_IsException(o))
        return -1;
    res = qp_len(ctx, o);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_getitem(JSContext *ctx, JSValueConst proxy, JSValueConst key)
{
    JSValue a, b, res;
    a = qp_wr_unwrap(ctx, proxy);
    if (JS_IsException(a))
        return a;
    b = qp_wr_unwrap(ctx, key);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    res = qp_getitem(ctx, a, b);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return res;
}

static int qp_wr_proxy_setitem(JSContext *ctx, JSValueConst proxy, JSValueConst key, JSValueConst value)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy);
    int res;
    if (JS_IsException(o))
        return -1;
    res = QP_IS_NULL(value) ? qp_delitem(ctx, o, key) : qp_setitem(ctx, o, key, value);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_iter(JSContext *ctx, JSValueConst proxy)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy), res;
    if (JS_IsException(o))
        return o;
    res = qp_iter(ctx, o);
    JS_FreeValue(ctx, o);
    return res;
}

static JSValue qp_wr_proxy_iternext(JSContext *ctx, JSValueConst proxy)
{
    JSValue o = qp_wr_proxy_ref(ctx, proxy), res;
    QPType *t;
    if (JS_IsException(o))
        return o;
    t = qp_type_of(ctx, o);
    if (!t->slots.tp_iternext) {
        JSValue name = qp_wr_tp_name(ctx, t);
        JS_FreeValue(ctx, o);
        if (JS_IsException(name))
            return JS_EXCEPTION;
        qp_raise_type_error(ctx, "Weakref proxy referenced a non-iterator '%.200U' object", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    res = qp_next(ctx, o);
    JS_FreeValue(ctx, o);
    return res;
}

/* WRAP_METHOD: __bytes__ / __reversed__ */
static JSValue qp_wr_proxy_method(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue o = qp_wr_unwrap(ctx, self), res;
    JSAtom name = magic ? QP_ATOM(ctx, __reversed__) : QP_ATOM(ctx, __bytes__);
    if (JS_IsException(o))
        return o;
    res = qp_call_method(ctx, o, name, NULL, 0, QP_NULL);
    JS_FreeValue(ctx, o);
    return res;
}

static const QPMethodDef qp_wr_proxy_methods[] = {
    { "__bytes__", qp_wr_proxy_method, QP_METH_NOARGS, 0, NULL },
    { "__reversed__", qp_wr_proxy_method, QP_METH_NOARGS, 1, NULL },
    { NULL }
};

#define QP_WR_PROXY_SLOTS                                                    \
        .tp_getattro = qp_wr_proxy_getattr,                                  \
        .tp_setattro = qp_wr_proxy_setattr,                                  \
        .tp_repr = qp_wr_proxy_repr,                                         \
        .tp_str = qp_wr_proxy_str,                                           \
        .tp_richcompare = qp_wr_proxy_richcompare,                           \
        .tp_iter = qp_wr_proxy_iter,                                         \
        .tp_iternext = qp_wr_proxy_iternext,                                 \
        .nb = {                                                              \
            .nb_add = qp_wr_proxy_add,                                       \
            .nb_subtract = qp_wr_proxy_sub,                                  \
            .nb_multiply = qp_wr_proxy_mul,                                  \
            .nb_remainder = qp_wr_proxy_mod,                                 \
            .nb_divmod = qp_wr_proxy_divmod,                                 \
            .nb_power = qp_wr_proxy_pow,                                     \
            .nb_negative = qp_wr_proxy_neg,                                  \
            .nb_positive = qp_wr_proxy_pos,                                  \
            .nb_absolute = qp_wr_proxy_abs,                                  \
            .nb_bool = qp_wr_proxy_bool,                                     \
            .nb_invert = qp_wr_proxy_invert,                                 \
            .nb_lshift = qp_wr_proxy_lshift,                                 \
            .nb_rshift = qp_wr_proxy_rshift,                                 \
            .nb_and = qp_wr_proxy_and,                                       \
            .nb_xor = qp_wr_proxy_xor,                                       \
            .nb_or = qp_wr_proxy_or,                                         \
            .nb_int = qp_wr_proxy_int,                                       \
            .nb_float = qp_wr_proxy_float,                                   \
            .nb_inplace_add = qp_wr_proxy_iadd,                              \
            .nb_inplace_subtract = qp_wr_proxy_isub,                         \
            .nb_inplace_multiply = qp_wr_proxy_imul,                         \
            .nb_inplace_remainder = qp_wr_proxy_imod,                        \
            .nb_inplace_power = qp_wr_proxy_ipow,                            \
            .nb_inplace_lshift = qp_wr_proxy_ilshift,                        \
            .nb_inplace_rshift = qp_wr_proxy_irshift,                        \
            .nb_inplace_and = qp_wr_proxy_iand,                              \
            .nb_inplace_xor = qp_wr_proxy_ixor,                              \
            .nb_inplace_or = qp_wr_proxy_ior,                                \
            .nb_floor_divide = qp_wr_proxy_floor_div,                        \
            .nb_true_divide = qp_wr_proxy_true_div,                          \
            .nb_inplace_floor_divide = qp_wr_proxy_ifloor_div,               \
            .nb_inplace_true_divide = qp_wr_proxy_itrue_div,                 \
            .nb_index = qp_wr_proxy_index,                                   \
            .nb_matrix_multiply = qp_wr_proxy_matmul,                        \
            .nb_inplace_matrix_multiply = qp_wr_proxy_imatmul,               \
        },                                                                   \
        .sq = {                                                              \
            .sq_contains = qp_wr_proxy_contains,                             \
        },                                                                   \
        .mp = {                                                              \
            .mp_length = qp_wr_proxy_length,                                 \
            .mp_subscript = qp_wr_proxy_getitem,                             \
            .mp_ass_subscript = qp_wr_proxy_setitem,                         \
        },

/* tp_hash is intentionally omitted: proxies are "mutable" (when the
   reference dies), so the inherited object hash is dropped because
   tp_richcompare is defined. */
static const QPTypeSpec qp_wr_proxy_spec = {
    .name = "weakref.ProxyType",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        QP_WR_PROXY_SLOTS
    },
    .methods = qp_wr_proxy_methods,
};

static const QPTypeSpec qp_wr_callproxy_spec = {
    .name = "weakref.CallableProxyType",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        QP_WR_PROXY_SLOTS
        .tp_call = qp_wr_proxy_call,
    },
    .methods = qp_wr_proxy_methods,
};

/* ---- _weakref module -------------------------------------------------------- */

static JSValue qp_wr_getweakrefcount(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    QPWeakBucket *b;
    QPWeakRec *r;
    int64_t count = 0;

    if (ms && qp_wr_supports(ctx, args[0])) {
        b = qp_wr_table_find(ms, JS_VALUE_GET_OBJ(args[0]));
        if (b) {
            for (r = b->head; r; r = r->next)
                count++;
        }
    }
    return qp_long_from_int64(ctx, count);
}

static JSValue qp_wr_getweakrefs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    JSValue result = qp_list_new(ctx, 0);
    QPWeakBucket *b;
    QPWeakRec *r;

    if (JS_IsException(result) || !ms || !qp_wr_supports(ctx, args[0]))
        return result;
    b = qp_wr_table_find(ms, JS_VALUE_GET_OBJ(args[0]));
    if (!b)
        return result;
    for (r = b->head; r; r = r->next) {
        if (qp_wr_owner_dying(r->owner))
            continue;
        if (qp_list_append(ctx, result, JS_MKPTR(JS_TAG_OBJECT, r->owner)) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

static JSValue qp_wr_remove_dead_weakref(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    JSValue value;
    QPWeakRec *rec;
    int r;

    if (qp_arg_check_positional(ctx, "_remove_dead_weakref", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_dict_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "_remove_dead_weakref() argument 1 must be dict, not %T",
                                   args[0]);
    r = qp_dict_get_item(ctx, args[0], args[1], &value);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    rec = qp_wr_rec(ctx, value);
    JS_FreeValue(ctx, value);
    if (!rec)
        return qp_raise_type_error(ctx, "not a weakref");
    if (!rec->target && qp_dict_del_item(ctx, args[0], args[1]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wr_proxy_func(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms;
    int which;

    if (qp_arg_check_positional(ctx, "proxy", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    ms = qp_wr_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    /* PyWeakref_NewProxy: callable referents get CallableProxyType */
    which = qp_type_of(ctx, args[0])->slots.tp_call || JS_IsFunction(ctx, args[0])
        ? QP_WR_T_CALLPROXY : QP_WR_T_PROXY;
    return qp_wr_get_or_create(ctx, qp_wr_type(ms, which), args[0],
                               nargs > 1 ? args[1] : QP_NULL);
}

static const QPMethodDef qp_wr_module_methods[] = {
    { "getweakrefcount", qp_wr_getweakrefcount, QP_METH_O, 0,
      "getweakrefcount($module, object, /)\n--\n\n"
      "Return the number of weak references to 'object'." },
    { "_remove_dead_weakref", qp_wr_remove_dead_weakref, QP_METH_FASTCALL, 0,
      "_remove_dead_weakref($module, dct, key, /)\n--\n\n"
      "Atomically remove key from dict if it points to a dead weakref." },
    { "getweakrefs", qp_wr_getweakrefs, QP_METH_O, 0,
      "getweakrefs($module, object, /)\n--\n\n"
      "Return a list of all weak reference objects pointing to 'object'." },
    { "proxy", qp_wr_proxy_func, QP_METH_FASTCALL, 0,
      "proxy($module, object, callback=None, /)\n--\n\n"
      "Create a proxy object that weakly references 'object'.\n\n"
      "'callback', if given, is called with a reference to the\n"
      "proxy when 'object' is about to be finalized." },
    { NULL }
};

static int qp_wr_add(JSContext *ctx, JSValueConst module, const char *name, JSValueConst v)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    int r;
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), atom, v);
    JS_FreeAtom(ctx, atom);
    return r;
}

static int qp_wr_exec(JSContext *ctx, JSValueConst module)
{
    QPModState *ms = qp_wr_state(ctx);
    if (!ms)
        return -1;
    if (qp_wr_add(ctx, module, "ref", ms->wr_types[QP_WR_T_REF]) < 0 ||
        qp_wr_add(ctx, module, "ReferenceType", ms->wr_types[QP_WR_T_REF]) < 0 ||
        qp_wr_add(ctx, module, "ProxyType", ms->wr_types[QP_WR_T_PROXY]) < 0 ||
        qp_wr_add(ctx, module, "CallableProxyType", ms->wr_types[QP_WR_T_CALLPROXY]) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef__weakref = {
    "_weakref",
    "Weak-reference support module.",
    qp_wr_module_methods,
    qp_wr_exec,
};
