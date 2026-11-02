/*
 * Small native modules whose state CPython keeps in the interpreter state
 * (python/modstate.h): gc (Modules/gcmodule.c), atexit
 * (Modules/atexitmodule.c), _signal (Modules/signalmodule.c),
 * _contextvars (Python/context.c, Python/_contextvars.c), marshal
 * (Python/marshal.c) and _warnings (Python/_warnings.c).  Ports of the
 * CPython 3.15 sources (PSF-2.0).
 *
 * Runtime hooks defined here (object.h "M3 NATIVE-MODULE RUNTIME HOOKS"):
 * qp_atexit_run (runner at exit) and qp_signal_check (interpreter poll).
 */
#include "modstate.h"
#include <time.h>

/* python/mod_weakref.c */
static void qp_weakref_run_pending(JSContext *ctx);
/* python/errors.c (unit i): PyErr_FormatUnraisable with a C message */
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);

/* PyErr_FormatUnraisable(fmt, obj) for the pending exception: message is
   fmt with %R of obj. */
static void qp_misc_unraisable(JSContext *ctx, const char *fmt, JSValueConst obj)
{
    JSValue exc = qp_exc_fetch(ctx);
    JSValue msg = QP_IS_NULL(obj) ? qp_str_from_cstr(ctx, fmt) : qp_str_from_format(ctx, fmt, obj);
    const char *cmsg = NULL;

    if (!JS_IsException(msg))
        cmsg = qp_str_utf8(ctx, msg, NULL);
    if (!cmsg)
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    qp_exc_write_unraisable_msg(ctx, exc, cmsg ? cmsg : "Exception ignored", QP_NULL);
    if (cmsg)
        JS_FreeCString(ctx, cmsg);
    if (!JS_IsException(msg))
        JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, exc);
}

static int qp_misc_add_value(JSContext *ctx, JSValueConst module, const char *name, JSValueConst v)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    int r;
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), atom, v);
    JS_FreeAtom(ctx, atom);
    return r;
}

static int qp_misc_add_int(JSContext *ctx, JSValueConst module, const char *name, int64_t v)
{
    JSValue iv = qp_long_from_int64(ctx, v);
    int r;
    if (JS_IsException(iv))
        return -1;
    r = qp_misc_add_value(ctx, module, name, iv);
    JS_FreeValue(ctx, iv);
    return r;
}

/* PyCallable_Check */
static BOOL qp_misc_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* ========================================================================= */
/* gc (Modules/gcmodule.c)                                                    */
/* ========================================================================= */
/*
 * The collector is QuickJS's: reference counting frees acyclic garbage at
 * once and JS_RunGC() frees unreachable cycles.  There are no generations:
 * collect(generation) always runs a full collection (CPython's result for
 * generation 2), get_count() reports the number of tracked objects created
 * since the last collection as the generation-0 count, get_objects(0) /
 * get_objects(None) list every tracked Python object and generations 1/2
 * are empty.  get_referents/get_referrers follow the QuickJS gc_mark
 * edges (plus the items of lists, tuples, dicts and sets, which include
 * untracked values such as ints and strs, as CPython's tp_traverse does),
 * looking through internal objects (function data, closure variables,
 * weakref holders, generator frames, whose locals are visited even while
 * they run) to the Python objects behind them.  DEBUG_SAVEALL
 * cannot keep cycles alive (QuickJS frees them inside JS_RunGC), so
 * gc.garbage stays empty; the cycle collector never refuses to free a
 * cycle (cycles with __del__ are freed without calling it, m2-design §6),
 * so the uncollectable count is always 0.  Audit events (gc.get_objects,
 * gc.get_referrers, gc.get_referents) are not raised: native modules have
 * no C-level audit entry point.
 */

#define QP_GC_DEBUG_STATS         (1 << 0)
#define QP_GC_DEBUG_COLLECTABLE   (1 << 1)
#define QP_GC_DEBUG_UNCOLLECTABLE (1 << 2)
#define QP_GC_DEBUG_SAVEALL       (1 << 5)
#define QP_GC_DEBUG_LEAK          (QP_GC_DEBUG_COLLECTABLE | QP_GC_DEBUG_UNCOLLECTABLE | \
                                   QP_GC_DEBUG_SAVEALL)
#define QP_GC_NUM_GENERATIONS 3

static QPModState *qp_gc_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->gc_callbacks)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return NULL;
        ms->gc_callbacks = l;
    }
    if (QP_IS_NULL(ms->gc_garbage)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return NULL;
        ms->gc_garbage = l;
    }
    return ms;
}

/* A Python object the collector tracks (CPython: a live GC-tracked
   container); internal companions are not Python objects. */
static BOOL qp_gc_visible(JSObject *p)
{
    if (p->free_mark || p->class_id == 0)
        return FALSE;
    if (QP_IS_PY_CLASS(p->class_id))
        return p->class_id != JS_CLASS_PY_FUNC_DATA && p->class_id != JS_CLASS_PY_NATIVE;
    if (p->class_id == JS_CLASS_ARRAY)
        return TRUE;
    if (p->class_id == JS_CLASS_BYTECODE_FUNCTION)
        return p->u.func.function_bytecode && p->u.func.function_bytecode->is_py;
    return FALSE;
}

static int64_t qp_gc_count_tracked(JSRuntime *rt)
{
    struct list_head *el;
    int64_t n = 0;

    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (js_rc(gp)->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT && qp_gc_visible((JSObject *)gp))
            n++;
    }
    return n;
}

/* Snapshot of the tracked objects (new references), allocated with
   qp_sys_malloc so that taking it cannot trigger a collection.  -1 with
   MemoryError. */
static Py_ssize_t qp_gc_snapshot(JSContext *ctx, JSValue **pitems)
{
    JSRuntime *rt = ctx->rt;
    struct list_head *el;
    JSValue *items = NULL;
    Py_ssize_t n = 0, size = 0;

    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (js_rc(gp)->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT || !qp_gc_visible((JSObject *)gp))
            continue;
        if (n == size) {
            Py_ssize_t ns = size ? size * 2 : 256;
            JSValue *ni = qp_sys_realloc(items, sizeof(JSValue) * ns);
            if (!ni) {
                while (n > 0)
                    JS_FreeValue(ctx, items[--n]);
                qp_sys_free(items);
                qp_raise_memory_error(ctx);
                return -1;
            }
            items = ni;
            size = ns;
        }
        items[n++] = JS_DupValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, gp));
    }
    *pitems = items;
    return n;
}

static void qp_gc_snapshot_free(JSContext *ctx, JSValue *items, Py_ssize_t n)
{
    while (n > 0)
        JS_FreeValue(ctx, items[--n]);
    qp_sys_free(items);
}

/* ---- referents ---------------------------------------------------------- */

/* GC headers reached from one object through mark_children (QuickJS
   JS_MarkFunc has no opaque argument: the collector is a static of the
   single-threaded runtime). */
typedef struct QPGcHdrVec {
    JSGCObjectHeader **v;
    int n, size;
    BOOL oom;
    BOOL self_ref;              /* v[0] reached again: the object refers to itself */
} QPGcHdrVec;

static QPGcHdrVec *qp_gc_cur_vec;

static void qp_gc_mark_collect(JSRuntime *rt, JSGCObjectHeader *gp)
{
    QPGcHdrVec *vec = qp_gc_cur_vec;
    int i;

    if (vec->oom)
        return;
    for (i = 0; i < vec->n; i++) {
        if (vec->v[i] == gp) {
            if (i == 0)
                vec->self_ref = TRUE;
            return;
        }
    }
    if (vec->n == vec->size) {
        int ns = vec->size ? vec->size * 2 : 32;
        JSGCObjectHeader **nv = qp_sys_realloc(vec->v, sizeof(*nv) * ns);
        if (!nv) {
            vec->oom = TRUE;
            return;
        }
        vec->v = nv;
        vec->size = ns;
    }
    vec->v[vec->n++] = gp;
}

typedef int QPGcVisitFunc(JSContext *ctx, JSValueConst v, void *opaque); /* 1 = stop, -1 = error */

/* Items of the containers whose tp_traverse visits untracked values. */
static int qp_gc_visit_items(JSContext *ctx, JSValueConst obj, QPGcVisitFunc *fn, void *opaque,
                             BOOL *handled)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    int r = 0;

    *handled = TRUE;
    if (p->class_id == JS_CLASS_PY_TUPLE) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(obj);
        for (i = 0; i < n && r == 0; i++)
            r = fn(ctx, QP_TUPLE_ITEMS(obj)[i], opaque);
    } else if (p->class_id == JS_CLASS_PY_DICT || p->class_id == JS_CLASS_PY_FROZENDICT) {
        Py_ssize_t pos = 0;
        JSValue k, v;
        while (r == 0 && qp_dict_next(obj, &pos, &k, &v)) {
            r = fn(ctx, k, opaque);
            if (r == 0)
                r = fn(ctx, v, opaque);
        }
    } else if (p->class_id == JS_CLASS_PY_SET || p->class_id == JS_CLASS_PY_FROZENSET) {
        Py_ssize_t pos = 0;
        JSValue k;
        Py_hash_t h;
        while (r == 0 && qp_set_next(obj, &pos, &k, &h))
            r = fn(ctx, k, opaque);
    } else if (p->class_id == JS_CLASS_ARRAY) {
        Py_ssize_t i, n;
        JSValue *items = qp_list_items(obj, &n);
        if (!items) {
            *handled = FALSE;
            return 0;
        }
        for (i = 0; i < n && r == 0; i++) {
            JSValue v = JS_DupValue(ctx, items[i]);   /* fn may run code */
            r = fn(ctx, v, opaque);
            JS_FreeValue(ctx, v);
            items = qp_list_items(obj, &n);
            if (!items)
                break;
        }
    } else {
        *handled = FALSE;
    }
    return r;
}

/* The locals of a running generator frame, which mark_children skips (a
   running frame cannot be part of a removable cycle); CPython's
   gen_traverse visits them in every state. */
static void qp_gc_mark_running_locals(JSRuntime *rt, JSAsyncFunctionState *s)
{
    JSStackFrame *sf = &s->frame;
    JSFunctionBytecode *b;
    int i;

    if (s->is_completed || sf->cur_sp || JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return;
    b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    if (!b)
        return;
    for (i = 0; i < sf->arg_count; i++)
        JS_MarkValue(rt, sf->arg_buf[i], qp_gc_mark_collect);
    for (i = 0; i < b->var_count; i++)
        JS_MarkValue(rt, sf->var_buf[i], qp_gc_mark_collect);
}

/* Call fn for every object obj refers to (CPython tp_traverse).  0, 1
   (stopped) or -1. */
static int qp_gc_foreach_referent(JSContext *ctx, JSValueConst obj, QPGcVisitFunc *fn, void *opaque)
{
    JSRuntime *rt = ctx->rt;
    QPGcHdrVec vec = { NULL, 0, 0, FALSE, FALSE }, *saved;
    BOOL handled;
    int i, r;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return 0;
    r = qp_gc_visit_items(ctx, obj, fn, opaque, &handled);
    if (handled || r != 0)
        return r;
    saved = qp_gc_cur_vec;
    qp_gc_cur_vec = &vec;
    qp_gc_mark_collect(rt, &JS_VALUE_GET_OBJ(obj)->header);
    /* breadth-first through internal objects; vec.v[0] is obj itself */
    for (i = 0; i < vec.n && !vec.oom; i++) {
        JSGCObjectHeader *gp = vec.v[i];
        int type = js_rc(gp)->gc_obj_type;
        if (i > 0 && type == JS_GC_OBJ_TYPE_JS_OBJECT && qp_gc_visible((JSObject *)gp))
            continue;
        if (i > 0 && type != JS_GC_OBJ_TYPE_JS_OBJECT && type != JS_GC_OBJ_TYPE_VAR_REF &&
            type != JS_GC_OBJ_TYPE_ASYNC_FUNCTION)
            continue;
        if (vec.n > 100000)
            break;
        if (type == JS_GC_OBJ_TYPE_VAR_REF) {
            /* a cell refers to its value (an open var ref of a heap
               frame marks the whole frame) */
            JS_MarkValue(rt, *((JSVarRef *)gp)->pvalue, qp_gc_mark_collect);
            continue;
        }
        mark_children(rt, gp, qp_gc_mark_collect);
        if (type == JS_GC_OBJ_TYPE_ASYNC_FUNCTION)
            qp_gc_mark_running_locals(rt, (JSAsyncFunctionState *)gp);
    }
    qp_gc_cur_vec = saved;
    if (vec.oom) {
        qp_sys_free(vec.v);
        qp_raise_memory_error(ctx);
        return -1;
    }
    r = 0;
    /* subtype_traverse visits the type of heap-type instances (reached by
       QuickJS through the shape prototype, which is not followed here) */
    {
        QPType *t = qp_type_of(ctx, obj);
        if ((t->flags & QP_TPFLAGS_HEAPTYPE) && JS_VALUE_GET_OBJ(obj)->class_id != JS_CLASS_PY_TYPE)
            r = fn(ctx, QP_TYPE_VALUE(t), opaque);
    }
    if (vec.self_ref && r == 0)
        r = fn(ctx, obj, opaque);
    for (i = 1; i < vec.n && r == 0; i++) {
        JSGCObjectHeader *gp = vec.v[i];
        JSValue v;
        if (js_rc(gp)->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT || !qp_gc_visible((JSObject *)gp))
            continue;
        if (gp == &qp_type_of(ctx, obj)->obj->header)
            continue;
        v = JS_DupValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, gp));
        r = fn(ctx, v, opaque);
        JS_FreeValue(ctx, v);
    }
    qp_sys_free(vec.v);
    return r;
}

static int qp_gc_append_visit(JSContext *ctx, JSValueConst v, void *opaque)
{
    return qp_list_append(ctx, *(JSValue *)opaque, v) < 0 ? -1 : 0;
}

typedef struct QPGcFind {
    JSValueConst *objs;
    int nobjs;
} QPGcFind;

static int qp_gc_find_visit(JSContext *ctx, JSValueConst v, void *opaque)
{
    QPGcFind *f = opaque;
    int i;
    for (i = 0; i < f->nobjs; i++) {
        if (qp_is(v, f->objs[i]))
            return 1;
    }
    return 0;
}

/* ---- callbacks and collection ------------------------------------------- */

static double qp_gc_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static JSValue qp_gc_stats_dict(JSContext *ctx, int generation, BOOL with_generation,
                                int64_t collections, int64_t collected, int64_t uncollectable,
                                int64_t candidates, double duration)
{
    JSValue d = qp_dict_new(ctx), v;
    struct { const char *name; int64_t val; } ints[5];
    int i, n = 0;

    if (JS_IsException(d))
        return d;
    if (with_generation) {
        ints[n].name = "generation"; ints[n++].val = generation;
    } else {
        ints[n].name = "collections"; ints[n++].val = collections;
    }
    ints[n].name = "collected"; ints[n++].val = collected;
    ints[n].name = "uncollectable"; ints[n++].val = uncollectable;
    ints[n].name = "candidates"; ints[n++].val = candidates;
    for (i = 0; i < n; i++) {
        JSAtom a = JS_NewAtom(ctx, ints[i].name);
        int r;
        v = qp_long_from_int64(ctx, ints[i].val);
        r = (a == JS_ATOM_NULL || JS_IsException(v)) ? -1 : qp_dict_set_item_atom(ctx, d, a, v);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    {
        JSAtom a = JS_NewAtom(ctx, "duration");
        int r;
        v = qp_float_new(ctx, duration);
        r = a == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, d, a, v);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    return d;
 fail:
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

/* invoke_gc_callback */
static void qp_gc_invoke_callbacks(JSContext *ctx, QPModState *ms, const char *phase, int generation,
                                   int64_t collected, int64_t uncollectable, int64_t candidates,
                                   double duration)
{
    JSValue info = QP_NULL, phase_obj, cbs = ms->gc_callbacks;
    Py_ssize_t i;

    if (QP_IS_NULL(cbs) || !qp_list_check(ctx, cbs))
        return;
    if (qp_list_size(cbs) != 0) {
        info = qp_gc_stats_dict(ctx, generation, TRUE, 0, collected, uncollectable, candidates,
                                duration);
        if (JS_IsException(info)) {
            qp_misc_unraisable(ctx, "Exception ignored on invoking gc callbacks", QP_NULL);
            return;
        }
    }
    phase_obj = qp_str_from_cstr(ctx, phase);
    if (JS_IsException(phase_obj)) {
        if (!QP_IS_NULL(info))
            JS_FreeValue(ctx, info);
        qp_misc_unraisable(ctx, "Exception ignored on invoking gc callbacks", QP_NULL);
        return;
    }
    JS_DupValue(ctx, cbs);
    for (i = 0; i < qp_list_size(cbs); i++) {
        JSValue cb = qp_getitem(ctx, cbs, JS_NewInt32(ctx, (int32_t)i)), r;
        if (JS_IsException(cb)) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            break;
        }
        r = qp_call2(ctx, cb, phase_obj, QP_IS_NULL(info) ? JS_UNDEFINED : info);
        if (JS_IsException(r))
            qp_misc_unraisable(ctx, "Exception ignored while calling GC callback %R", cb);
        else
            JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, cb);
    }
    JS_FreeValue(ctx, cbs);
    JS_FreeValue(ctx, phase_obj);
    if (!QP_IS_NULL(info))
        JS_FreeValue(ctx, info);
}

/* gc_collect_main: a full QuickJS collection, then the queued __del__
   methods and weakref callbacks.  Returns the number of unreachable
   objects freed. */
static Py_ssize_t qp_gc_collect_main(JSContext *ctx, QPModState *ms, int generation)
{
    int64_t before, after, collected;
    double t0, duration;

    if (ms->gc_collecting)
        return 0;
    ms->gc_collecting = 1;
    ms->gc_collections[generation]++;
    qp_gc_invoke_callbacks(ctx, ms, "start", generation, 0, 0, 0, 0.0);
    t0 = qp_gc_now();
    before = qp_gc_count_tracked(ctx->rt);
    if (ms->gc_debug & QP_GC_DEBUG_STATS) {
        qp_sys_write_stderr(ctx, "gc: collecting generation %d...\n", generation);
        qp_sys_write_stderr(ctx, "gc: objects in each generation: %lld 0 0\n"
                            "gc: objects in permanent generation: %lld\n",
                            (long long)before, (long long)ms->gc_frozen);
    }
    JS_RunGC(ctx->rt);
    after = qp_gc_count_tracked(ctx->rt);
    collected = before > after ? before - after : 0;
    duration = qp_gc_now() - t0;
    if (ms->gc_debug & QP_GC_DEBUG_STATS)
        qp_sys_write_stderr(ctx, "gc: done, %lld unreachable, %lld uncollectable, %.4fs elapsed\n",
                            (long long)collected, 0LL, duration);
    ms->gc_collected[generation] += collected;
    ms->gc_candidates[generation] += before;
    ms->gc_duration[generation] += duration;
    /* finalizers and weakref callbacks of the freed objects */
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    ms->gc_baseline = qp_gc_count_tracked(ctx->rt);
    qp_gc_invoke_callbacks(ctx, ms, "stop", generation, collected, 0, before, duration);
    ms->gc_collecting = 0;
    return (Py_ssize_t)collected;
}

/* PyGC_Collect() of Py_FinalizeEx: when the collector is enabled, a full
   collection (with gc.callbacks) before the modules are destroyed, so
   that the garbage is finalized while they still work */
static void qp_gc_collect_at_exit(JSContext *ctx)
{
    QPModState *ms = qp_gc_state(ctx);

    if (!ms) {
        qp_tw_clear_exc(ctx);
        return;
    }
    if (ms->gc_enabled)
        qp_gc_collect_main(ctx, ms, QP_GC_NUM_GENERATIONS - 1);
}

static JSValue qp_gc_enable(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    JSRuntime *rt = ctx->rt;

    if (!ms)
        return JS_EXCEPTION;
    if (magic) {
        /* enable: restore QuickJS's automatic collection */
        if (!ms->gc_enabled) {
            size_t sz = rt->malloc_ctx.malloc_state.malloc_size;
            size_t th = ms->gc_saved_threshold;
            if (th < sz + (sz >> 1))
                th = sz + (sz >> 1);
            JS_SetGCThreshold(rt, th);
        }
        ms->gc_enabled = 1;
    } else {
        if (ms->gc_enabled) {
            ms->gc_saved_threshold = JS_GetGCThreshold(rt);
            JS_SetGCThreshold(rt, (size_t)-1);
        }
        ms->gc_enabled = 0;
    }
    return JS_UNDEFINED;
}

static JSValue qp_gc_isenabled(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return qp_bool_new(ms->gc_enabled);
}

static JSValue qp_gc_collect(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "generation", NULL };
    static const QPArgSpec spec = { "collect", kw, 0, 1, 0 };
    JSValueConst a[1];
    QPModState *ms;
    int generation = QP_GC_NUM_GENERATIONS - 1;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_int(ctx, a[0], &generation) < 0)
        return JS_EXCEPTION;
    ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (generation < 0 || generation >= QP_GC_NUM_GENERATIONS)
        return qp_raise_value_error(ctx, "invalid generation");
    return qp_long_from_int64(ctx, qp_gc_collect_main(ctx, ms, generation));
}

static JSValue qp_gc_set_debug(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    int flags;

    if (!ms || qp_arg_int(ctx, args[0], &flags) < 0)
        return JS_EXCEPTION;
    ms->gc_debug = flags;
    return JS_UNDEFINED;
}

static JSValue qp_gc_get_debug(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, ms->gc_debug);
}

static JSValue qp_gc_int_tuple3(JSContext *ctx, int64_t a, int64_t b, int64_t c)
{
    JSValue items[3];
    items[0] = qp_long_from_int64(ctx, a);
    items[1] = qp_long_from_int64(ctx, b);
    items[2] = qp_long_from_int64(ctx, c);
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_gc_set_threshold(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    int th[3], i;

    if (!ms)
        return JS_EXCEPTION;
    if (nargs < 1 || nargs > 3)
        return qp_raise_type_error(ctx, "gc.set_threshold requires 1 to 3 arguments");
    for (i = 0; i < nargs; i++) {
        if (qp_arg_int(ctx, args[i], &th[i]) < 0)
            return JS_EXCEPTION;
    }
    for (i = 0; i < nargs; i++)
        ms->gc_threshold[i] = th[i];
    return JS_UNDEFINED;
}

static JSValue qp_gc_get_threshold(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return qp_gc_int_tuple3(ctx, ms->gc_threshold[0], ms->gc_threshold[1], ms->gc_threshold[2]);
}

static JSValue qp_gc_get_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    int64_t n;
    if (!ms)
        return JS_EXCEPTION;
    n = qp_gc_count_tracked(ctx->rt) - ms->gc_baseline;
    if (n < 0) {
        /* objects died since the last collection: count from here */
        ms->gc_baseline += n;
        n = 0;
    }
    return qp_gc_int_tuple3(ctx, n, 0, 0);
}

static JSValue qp_gc_get_referrers(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    JSValue *items = NULL, result;
    Py_ssize_t n, i;
    QPGcFind f;

    n = qp_gc_snapshot(ctx, &items);
    if (n < 0)
        return JS_EXCEPTION;
    result = qp_list_new(ctx, 0);
    if (JS_IsException(result)) {
        qp_gc_snapshot_free(ctx, items, n);
        return result;
    }
    f.objs = args;
    f.nobjs = nargs;
    for (i = 0; i < n; i++) {
        int r = qp_gc_foreach_referent(ctx, items[i], qp_gc_find_visit, &f);
        if (r < 0 || (r == 1 && qp_list_append(ctx, result, items[i]) < 0)) {
            JS_FreeValue(ctx, result);
            qp_gc_snapshot_free(ctx, items, n);
            return JS_EXCEPTION;
        }
    }
    qp_gc_snapshot_free(ctx, items, n);
    return result;
}

static JSValue qp_gc_get_referents(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    JSValue result = qp_list_new(ctx, 0);
    int i;

    if (JS_IsException(result))
        return result;
    for (i = 0; i < nargs; i++) {
        /* only tracked objects have referents (CPython _PyObject_IS_GC) */
        if (JS_VALUE_GET_TAG(args[i]) != JS_TAG_OBJECT ||
            !qp_gc_visible(JS_VALUE_GET_OBJ(args[i])))
            continue;
        if (qp_gc_foreach_referent(ctx, args[i], qp_gc_append_visit, &result) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

static JSValue qp_gc_get_objects(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "generation", NULL };
    static const QPArgSpec spec = { "get_objects", kw, 0, 1, 0 };
    JSValueConst a[1];
    Py_ssize_t generation = -1, n;
    JSValue *items = NULL, result;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && !JS_IsUndefined(a[0])) {
        if (qp_type_of(ctx, a[0])->slots.nb.nb_index == NULL && !qp_long_check(ctx, a[0]))
            return qp_raise_type_error(ctx, "argument should be integer or None, not '%T'", a[0]);
        if (qp_arg_ssize(ctx, a[0], &generation) < 0)
            return JS_EXCEPTION;
    }
    if (generation >= QP_GC_NUM_GENERATIONS)
        return qp_raise_value_error(ctx, "generation parameter must be less than the number of "
                                    "available generations (%i)", QP_GC_NUM_GENERATIONS);
    if (generation < -1)
        return qp_raise_value_error(ctx, "generation parameter cannot be negative");
    if (generation > 0)
        return qp_list_new(ctx, 0);   /* everything is young (see the comment above) */
    n = qp_gc_snapshot(ctx, &items);
    if (n < 0)
        return JS_EXCEPTION;
    result = qp_list_new_steal(ctx, n, items);
    qp_sys_free(items);
    return result;
}

static JSValue qp_gc_get_stats(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    JSValue result, items[QP_GC_NUM_GENERATIONS];
    int i;

    if (!ms)
        return JS_EXCEPTION;
    for (i = 0; i < QP_GC_NUM_GENERATIONS; i++) {
        items[i] = qp_gc_stats_dict(ctx, i, FALSE, ms->gc_collections[i], ms->gc_collected[i],
                                    ms->gc_uncollectable[i], ms->gc_candidates[i],
                                    ms->gc_duration[i]);
        if (JS_IsException(items[i])) {
            while (i > 0)
                JS_FreeValue(ctx, items[--i]);
            return JS_EXCEPTION;
        }
    }
    result = qp_list_new_steal(ctx, QP_GC_NUM_GENERATIONS, items);
    return result;
}

/* PyObject_GC_IsTracked: containers are tracked; atomic values (None,
   bool, int, float, str, bytes, complex), bytearray, the empty tuple and
   static builtin types are not. */
static JSValue qp_gc_is_tracked(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst obj = args[0];
    JSObject *p;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return qp_bool_new(FALSE);
    p = JS_VALUE_GET_OBJ(obj);
    switch (p->class_id) {
    case JS_CLASS_PY_COMPLEX:
    case JS_CLASS_PY_BYTES:
    case JS_CLASS_PY_BYTEARRAY:
    case JS_CLASS_PY_RANGE:
        return qp_bool_new(FALSE);
    case JS_CLASS_PY_TUPLE:
        return qp_bool_new(QP_TUPLE_SIZE(obj) != 0);
    case JS_CLASS_PY_TYPE: {
        QPType *t = qp_type_from_value(obj);
        return qp_bool_new(t && (t->flags & QP_TPFLAGS_HEAPTYPE));
    }
    default:
        return qp_bool_new(qp_gc_visible(p));
    }
}

static JSValue qp_gc_is_finalized(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst obj = args[0];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return qp_bool_new(FALSE);
    return qp_bool_new(JS_VALUE_GET_OBJ(obj)->py_finalized);
}

static JSValue qp_gc_freeze(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (magic) {
        /* freeze: every tracked object moves to the permanent generation */
        ms->gc_frozen += qp_gc_count_tracked(ctx->rt) - ms->gc_frozen;
        ms->gc_baseline = qp_gc_count_tracked(ctx->rt);
    } else {
        ms->gc_frozen = 0;
    }
    return JS_UNDEFINED;
}

static JSValue qp_gc_get_freeze_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, ms->gc_frozen);
}

static const QPMethodDef qp_gc_methods[] = {
    { "enable", qp_gc_enable, QP_METH_NOARGS, 1,
      "enable($module, /)\n--\n\nEnable automatic garbage collection." },
    { "disable", qp_gc_enable, QP_METH_NOARGS, 0,
      "disable($module, /)\n--\n\nDisable automatic garbage collection." },
    { "isenabled", qp_gc_isenabled, QP_METH_NOARGS, 0,
      "isenabled($module, /)\n--\n\nReturns true if automatic garbage collection is enabled." },
    { "set_debug", qp_gc_set_debug, QP_METH_O, 0,
      "set_debug($module, flags, /)\n--\n\n"
      "Set the garbage collection debugging flags.\n\n"
      "  flags\n"
      "    An integer that can have the following bits turned on:\n"
      "      DEBUG_STATS - Print statistics during collection.\n"
      "      DEBUG_COLLECTABLE - Print collectable objects found.\n"
      "      DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects\n"
      "        found.\n"
      "      DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.\n"
      "      DEBUG_LEAK - Debug leaking programs (everything but STATS).\n\n"
      "Debugging information is written to sys.stderr." },
    { "get_debug", qp_gc_get_debug, QP_METH_NOARGS, 0,
      "get_debug($module, /)\n--\n\nGet the garbage collection debugging flags." },
    { "get_count", qp_gc_get_count, QP_METH_NOARGS, 0,
      "get_count($module, /)\n--\n\nReturn a three-tuple of the current collection counts." },
    { "set_threshold", qp_gc_set_threshold, QP_METH_FASTCALL, 0,
      "set_threshold(threshold0, [threshold1, [threshold2]])\n"
      "Set the collection thresholds (the collection frequency).\n\n"
      "Setting 'threshold0' to zero disables collection." },
    { "get_threshold", qp_gc_get_threshold, QP_METH_NOARGS, 0,
      "get_threshold($module, /)\n--\n\nReturn the current collection thresholds." },
    { "collect", qp_gc_collect, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "collect($module, /, generation=2)\n--\n\n"
      "Run the garbage collector.\n\n"
      "With no arguments, run a full collection.  The optional argument\n"
      "may be an integer specifying which generation to collect.  A ValueError\n"
      "is raised if the generation number is invalid.\n\n"
      "The number of unreachable objects is returned." },
    { "get_objects", qp_gc_get_objects, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "get_objects($module, /, generation=None)\n--\n\n"
      "Return a list of objects tracked by the collector (excluding the list returned).\n\n"
      "  generation\n"
      "    Generation to extract the objects from.\n\n"
      "If generation is not None, return only the objects tracked by the\n"
      "collector that are in that generation." },
    { "get_stats", qp_gc_get_stats, QP_METH_NOARGS, 0,
      "get_stats($module, /)\n--\n\n"
      "Return a list of dictionaries containing per-generation statistics." },
    { "is_tracked", qp_gc_is_tracked, QP_METH_O, 0,
      "is_tracked($module, obj, /)\n--\n\n"
      "Returns true if the object is tracked by the garbage collector.\n\n"
      "Simple atomic objects will return false." },
    { "is_finalized", qp_gc_is_finalized, QP_METH_O, 0,
      "is_finalized($module, obj, /)\n--\n\n"
      "Returns true if the object has been already finalized by the GC." },
    { "get_referrers", qp_gc_get_referrers, QP_METH_FASTCALL, 0,
      "get_referrers($module, /, *objs)\n--\n\n"
      "Return the list of objects that directly refer to any of 'objs'." },
    { "get_referents", qp_gc_get_referents, QP_METH_FASTCALL, 0,
      "get_referents($module, /, *objs)\n--\n\n"
      "Return the list of objects that are directly referred to by 'objs'." },
    { "freeze", qp_gc_freeze, QP_METH_NOARGS, 1,
      "freeze($module, /)\n--\n\n"
      "Freeze all current tracked objects and ignore them for future collections.\n\n"
      "This can be used before a POSIX fork() call to make the gc copy-on-write\n"
      "friendly.\n"
      "Note: collection before a POSIX fork() call may free pages for future\n"
      "allocation which can cause copy-on-write." },
    { "unfreeze", qp_gc_freeze, QP_METH_NOARGS, 0,
      "unfreeze($module, /)\n--\n\n"
      "Unfreeze all objects in the permanent generation.\n\n"
      "Put all objects in the permanent generation back into oldest generation." },
    { "get_freeze_count", qp_gc_get_freeze_count, QP_METH_NOARGS, 0,
      "get_freeze_count($module, /)\n--\n\n"
      "Return the number of objects in the permanent generation." },
    { NULL }
};

static int qp_gc_exec(JSContext *ctx, JSValueConst module)
{
    QPModState *ms = qp_gc_state(ctx);
    if (!ms)
        return -1;
    if (qp_misc_add_value(ctx, module, "garbage", ms->gc_garbage) < 0 ||
        qp_misc_add_value(ctx, module, "callbacks", ms->gc_callbacks) < 0 ||
        qp_misc_add_int(ctx, module, "DEBUG_STATS", QP_GC_DEBUG_STATS) < 0 ||
        qp_misc_add_int(ctx, module, "DEBUG_COLLECTABLE", QP_GC_DEBUG_COLLECTABLE) < 0 ||
        qp_misc_add_int(ctx, module, "DEBUG_UNCOLLECTABLE", QP_GC_DEBUG_UNCOLLECTABLE) < 0 ||
        qp_misc_add_int(ctx, module, "DEBUG_SAVEALL", QP_GC_DEBUG_SAVEALL) < 0 ||
        qp_misc_add_int(ctx, module, "DEBUG_LEAK", QP_GC_DEBUG_LEAK) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef_gc = {
    "gc",
    "This module provides access to the garbage collector for reference cycles.\n"
    "\n"
    "enable() -- Enable automatic garbage collection.\n"
    "disable() -- Disable automatic garbage collection.\n"
    "isenabled() -- Returns true if automatic collection is enabled.\n"
    "collect() -- Do a full collection right now.\n"
    "get_count() -- Return the current collection counts.\n"
    "get_stats() -- Return list of dictionaries containing per-generation stats.\n"
    "set_debug() -- Set debugging flags.\n"
    "get_debug() -- Get debugging flags.\n"
    "set_threshold() -- Set the collection thresholds.\n"
    "get_threshold() -- Return the current collection thresholds.\n"
    "get_objects() -- Return a list of all objects tracked by the collector.\n"
    "is_tracked() -- Returns true if a given object is tracked.\n"
    "is_finalized() -- Returns true if a given object has been already finalized.\n"
    "get_referrers() -- Return the list of objects that refer to an object.\n"
    "get_referents() -- Return the list of objects that an object refers to.\n"
    "freeze() -- Freeze all tracked objects and ignore them for future collections.\n"
    "unfreeze() -- Unfreeze all objects in the permanent generation.\n"
    "get_freeze_count() -- Return the number of objects in the permanent generation.\n",
    qp_gc_methods,
    qp_gc_exec,
};

/* ========================================================================= */
/* atexit (Modules/atexitmodule.c)                                            */
/* ========================================================================= */

/* callbacks: list of (func, args tuple, kwargs dict or None), LIFO order
   (register inserts at index 0). */
static QPModState *qp_atexit_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->atexit_callbacks)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return NULL;
        ms->atexit_callbacks = l;
    }
    return ms;
}

/* atexit_cleanup: clear the list without calling the callbacks */
static int qp_atexit_cleanup(JSContext *ctx, QPModState *ms)
{
    JSValue empty = qp_list_new(ctx, 0);
    int r;
    if (JS_IsException(empty))
        return -1;
    r = qp_setslice(ctx, ms->atexit_callbacks, JS_UNDEFINED, JS_UNDEFINED, empty);
    JS_FreeValue(ctx, empty);
    return r;
}

/* atexit_callfuncs */
static void qp_atexit_callfuncs(JSContext *ctx, QPModState *ms)
{
    JSValue copy, saved = QP_NULL;
    Py_ssize_t i, n;

    if (JS_HasException(ctx))
        saved = JS_GetException(ctx);
    copy = qp_getslice(ctx, ms->atexit_callbacks, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(copy)) {
        qp_misc_unraisable(ctx, "Exception ignored while copying atexit callbacks", QP_NULL);
        if (qp_atexit_cleanup(ctx, ms) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        goto done;
    }
    n = qp_list_size(copy);
    for (i = 0; i < n; i++) {
        JSValue tuple = qp_getitem(ctx, copy, JS_NewInt32(ctx, (int32_t)i)), res;
        JSValueConst func, fargs, fkwargs;
        if (JS_IsException(tuple)) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            continue;
        }
        func = QP_TUPLE_ITEMS(tuple)[0];
        fargs = QP_TUPLE_ITEMS(tuple)[1];
        fkwargs = QP_TUPLE_ITEMS(tuple)[2];
        res = qp_call_tuple(ctx, func, fargs, JS_IsUndefined(fkwargs) ? QP_NULL : fkwargs);
        if (JS_IsException(res))
            qp_misc_unraisable(ctx, "Exception ignored in atexit callback %R", func);
        else
            JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, tuple);
    }
    JS_FreeValue(ctx, copy);
    if (qp_atexit_cleanup(ctx, ms) < 0)
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
 done:
    if (!QP_IS_NULL(saved))
        JS_Throw(ctx, saved);
}

/* _PyAtExit_Call: run at interpreter exit (object.h hook). */
static void qp_atexit_run(JSContext *ctx)
{
    QPModState *ms = QP_STATE(ctx) ? QP_STATE(ctx)->mod_state : NULL;
    if (!ms || QP_IS_NULL(ms->atexit_callbacks))
        return;
    qp_atexit_callfuncs(ctx, ms);
}

static JSValue qp_atexit_register(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms;
    JSValue fargs, fkwargs, items[3], callback;
    Py_ssize_t nkw = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames), i;

    if (nargs == 0)
        return qp_raise_type_error(ctx, "register() takes at least 1 argument (0 given)");
    if (!qp_misc_callable(ctx, args[0]))
        return qp_raise_type_error(ctx, "the first argument must be callable");
    ms = qp_atexit_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    fargs = qp_tuple_pack(ctx, nargs - 1, args + 1);
    if (JS_IsException(fargs))
        return JS_EXCEPTION;
    if (nkw) {
        fkwargs = qp_dict_new(ctx);
        if (JS_IsException(fkwargs)) {
            JS_FreeValue(ctx, fargs);
            return JS_EXCEPTION;
        }
        for (i = 0; i < nkw; i++) {
            if (qp_dict_set_item(ctx, fkwargs, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0) {
                JS_FreeValue(ctx, fargs);
                JS_FreeValue(ctx, fkwargs);
                return JS_EXCEPTION;
            }
        }
    } else {
        fkwargs = JS_UNDEFINED;
    }
    items[0] = JS_DupValue(ctx, args[0]);
    items[1] = fargs;
    items[2] = fkwargs;
    callback = qp_tuple_new_steal(ctx, 3, items);
    if (JS_IsException(callback))
        return JS_EXCEPTION;
    /* atexit callbacks go in a LIFO order */
    if (qp_list_insert(ctx, ms->atexit_callbacks, 0, callback) < 0) {
        JS_FreeValue(ctx, callback);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, callback);
    return JS_DupValue(ctx, args[0]);
}

static JSValue qp_atexit_run_exitfuncs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_atexit_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    qp_atexit_callfuncs(ctx, ms);
    return JS_UNDEFINED;
}

static JSValue qp_atexit_clear(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_atexit_state(ctx);
    if (!ms || qp_atexit_cleanup(ctx, ms) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_atexit_ncallbacks(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_atexit_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, qp_list_size(ms->atexit_callbacks));
}

/* atexit_unregister_locked */
static JSValue qp_atexit_unregister(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_atexit_state(ctx);
    JSValueConst func = args[0];
    JSValue callbacks;
    Py_ssize_t i, j;

    if (!ms)
        return JS_EXCEPTION;
    callbacks = ms->atexit_callbacks;
    for (i = qp_list_size(callbacks) - 1; i >= 0; --i) {
        JSValue tuple = qp_getitem(ctx, callbacks, JS_NewInt32(ctx, (int32_t)i));
        int cmp;
        if (JS_IsException(tuple))
            return JS_EXCEPTION;
        cmp = qp_richcompare_bool(ctx, func, QP_TUPLE_ITEMS(tuple)[0], QP_CMP_EQ);
        if (cmp < 0) {
            JS_FreeValue(ctx, tuple);
            return JS_EXCEPTION;
        }
        if (cmp == 1) {
            /* its index could have changed if callbacks were unregistered
               during the comparison */
            j = qp_list_size(callbacks) - 1;
            if (j > i)
                j = i;
            for (; j >= 0; --j) {
                JSValue it = qp_getitem(ctx, callbacks, JS_NewInt32(ctx, (int32_t)j));
                BOOL same;
                if (JS_IsException(it)) {
                    JS_FreeValue(ctx, tuple);
                    return JS_EXCEPTION;
                }
                same = qp_is(it, tuple);
                JS_FreeValue(ctx, it);
                if (same) {
                    JSValue idx = JS_NewInt32(ctx, (int32_t)j);
                    if (qp_delitem(ctx, callbacks, idx) < 0) {
                        JS_FreeValue(ctx, tuple);
                        return JS_EXCEPTION;
                    }
                    i = j;
                    break;
                }
            }
        }
        JS_FreeValue(ctx, tuple);
        if (i >= qp_list_size(callbacks))
            i = qp_list_size(callbacks);
    }
    return JS_UNDEFINED;
}

static const QPMethodDef qp_atexit_methods[] = {
    { "register", qp_atexit_register, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "register($module, func, /, *args, **kwargs)\n--\n\n"
      "Register a function to be executed upon normal program termination\n\n"
      "    func - function to be called at exit\n"
      "    args - optional arguments to pass to func\n"
      "    kwargs - optional keyword arguments to pass to func\n\n"
      "    func is returned to facilitate usage as a decorator." },
    { "_clear", qp_atexit_clear, QP_METH_NOARGS, 0,
      "_clear($module, /)\n--\n\n"
      "Clear the list of previously registered exit functions." },
    { "unregister", qp_atexit_unregister, QP_METH_O, 0,
      "unregister($module, func, /)\n--\n\n"
      "Unregister an exit function which was previously registered using\n"
      "atexit.register\n\n"
      "    func - function to be unregistered" },
    { "_run_exitfuncs", qp_atexit_run_exitfuncs, QP_METH_NOARGS, 0,
      "_run_exitfuncs($module, /)\n--\n\n"
      "Run all registered exit functions.\n\n"
      "If a callback raises an exception, it is logged with sys.unraisablehook." },
    { "_ncallbacks", qp_atexit_ncallbacks, QP_METH_NOARGS, 0,
      "_ncallbacks($module, /)\n--\n\n"
      "Return the number of registered exit functions." },
    { NULL }
};

static int qp_atexit_exec(JSContext *ctx, JSValueConst module)
{
    return qp_atexit_state(ctx) ? 0 : -1;
}

static const QPModuleDef qp_moddef_atexit = {
    "atexit",
    "allow programmer to define multiple exit functions to be executed\n"
    "upon normal program termination.\n"
    "\n"
    "Two public functions, register and unregister, are defined.\n",
    qp_atexit_methods,
    qp_atexit_exec,
};

/*
 * _contextvars: port of CPython 3.15 Python/context.c and
 * Python/_contextvars.c (PSF-2.0).  Fragment of python/mod_misc.c.
 *
 * Representation.  CPython keeps the variables of a Context in a HAMT (an
 * immutable persistent mapping).  Here a Context holds a PY_DICT that is
 * copied on write: a dict referenced from more than one place (several
 * Context objects after Context.copy()/copy_context(), a live iterator
 * from iter(ctx), a comparison in progress) has a reference count above
 * one and is copied before the first mutation, so every snapshot stays
 * isolated exactly as with the HAMT.  Only the iteration order differs
 * (insertion order instead of hash order), which CPython does not
 * specify.  Context.keys()/values()/items() return lists (the HAMT views
 * of CPython are plain iterables with a length).
 *
 * The current context is QPModState.ctx_current (CPython
 * PyThreadState.context; one per context until real OS threads exist in
 * M5, where it becomes per thread).  Context watchers
 * (PyContext_AddWatcher) and the per-variable lookup cache are C-API /
 * performance features without observable Python behaviour and are not
 * ported.
 *
 * C API for the other modules of the translation unit (the types are
 * created lazily on first use, so these work before `_contextvars` is
 * imported):
 *
 *   PyContextVar_New: new ContextVar named `name`; dflt QP_NULL = no default.
 *     static JSValue qp_contextvar_new(JSContext *ctx, const char *name, JSValueConst dflt);
 *   PyContextVar_Get: 0 with *pval a new reference (or QP_NULL when the
 *   variable is unset and has no default and dflt is QP_NULL), -1 on error.
 *     static int qp_contextvar_get(JSContext *ctx, JSValueConst var, JSValueConst dflt, JSValue *pval);
 *   PyContextVar_Set: returns the Token.
 *     static JSValue qp_contextvar_set(JSContext *ctx, JSValueConst var, JSValueConst val);
 *   PyContextVar_Reset: 0 or -1.
 *     static int qp_contextvar_reset(JSContext *ctx, JSValueConst var, JSValueConst token);
 *   PyContext_CopyCurrent:
 *     static JSValue qp_context_copy_current(JSContext *ctx);
 *   PyContext_Enter / PyContext_Exit: 0 or -1.
 *     static int qp_context_enter(JSContext *ctx, JSValueConst context);
 *     static int qp_context_exit(JSContext *ctx, JSValueConst context);
 */

#include "modstate.h"

/* indices of QPModState.ctx_types */
enum {
    QP_CTXVAR_T_CONTEXT = 0,
    QP_CTXVAR_T_VAR = 1,
    QP_CTXVAR_T_TOKEN = 2,
};

/* Context instance slots */
enum {
    QP_CONTEXT_SLOT_VARS = 0,       /* PY_DICT {ContextVar: value}, copy on write */
    QP_CONTEXT_SLOT_PREV = 1,       /* context current before enter (owned), QP_NULL */
    QP_CONTEXT_SLOT_ENTERED = 2,    /* int 0/1 */
    QP_CONTEXT_NSLOTS = 3,
};

/* ContextVar instance slots */
enum {
    QP_CTXVAR_SLOT_NAME = 0,        /* str */
    QP_CTXVAR_SLOT_DEFAULT = 1,     /* default value, QP_NULL = none */
    QP_CTXVAR_SLOT_HASH_LO = 2,     /* precomputed hash, low 32 bits (int32) */
    QP_CTXVAR_SLOT_HASH_HI = 3,     /* high 32 bits */
    QP_CTXVAR_NSLOTS = 4,
};

/* Token instance slots */
enum {
    QP_CTXTOK_SLOT_CTX = 0,         /* Context the token was created in */
    QP_CTXTOK_SLOT_VAR = 1,         /* ContextVar */
    QP_CTXTOK_SLOT_OLD = 2,         /* previous value, QP_NULL = Token.MISSING */
    QP_CTXTOK_SLOT_USED = 3,        /* int 0/1 */
    QP_CTXTOK_NSLOTS = 4,
};

static int qp_ctxvar_init_types(JSContext *ctx, QPModState *ms);

/* Module state with the three types created (NULL + exception on failure). */
static QPModState *qp_ctxvar_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->ctx_types[QP_CTXVAR_T_CONTEXT]) && qp_ctxvar_init_types(ctx, ms) < 0)
        return NULL;
    return ms;
}

static QPType *qp_ctxvar_type(QPModState *ms, int which)
{
    return qp_type_from_value(ms->ctx_types[which]);
}

/* Py*_CheckExact (none of the three types is subclassable). */
static BOOL qp_ctxvar_is(JSContext *ctx, QPModState *ms, JSValueConst v, int which)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        qp_type_of(ctx, v) == qp_ctxvar_type(ms, which);
}

static BOOL qp_ctxvar_same(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* ---- Context internals --------------------------------------------------- */

/* context_new_from_vars / context_new_empty (vars QP_NULL). */
static JSValue qp_context_new_from_vars(JSContext *ctx, QPModState *ms, JSValueConst vars)
{
    JSValue obj, d;
    JSValue *slots;

    if (QP_IS_NULL(vars)) {
        d = qp_dict_new(ctx);
        if (JS_IsException(d))
            return d;
    } else {
        d = JS_DupValue(ctx, vars);
    }
    obj = qp_object_new_instance(ctx, qp_ctxvar_type(ms, QP_CTXVAR_T_CONTEXT), JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, d);
        return obj;
    }
    slots = qp_instance_slots(obj);
    slots[QP_CONTEXT_SLOT_VARS] = d;
    slots[QP_CONTEXT_SLOT_PREV] = QP_NULL;
    slots[QP_CONTEXT_SLOT_ENTERED] = JS_NewInt32(ctx, 0);
    return obj;
}

/* context_get: the current context (borrowed), created empty on first use. */
static JSValue qp_context_get(JSContext *ctx, QPModState *ms)
{
    if (QP_IS_NULL(ms->ctx_current)) {
        JSValue c = qp_context_new_from_vars(ctx, ms, QP_NULL);
        if (JS_IsException(c))
            return c;
        ms->ctx_current = c;
    }
    return ms->ctx_current;
}

/* Variables of a context (borrowed). */
static JSValue qp_context_vars(JSValueConst context)
{
    return qp_instance_slots(context)[QP_CONTEXT_SLOT_VARS];
}

/* Variables of a context ready for in-place mutation (borrowed): a dict
   that anything else references is replaced by a private copy first
   (the HAMT's persistence). */
static JSValue qp_context_vars_for_write(JSContext *ctx, JSValueConst context)
{
    JSValue *slots = qp_instance_slots(context);
    JSValue vars = slots[QP_CONTEXT_SLOT_VARS];

    if (js_rc(JS_VALUE_GET_OBJ(vars))->ref_count > 1) {
        JSValue copy = qp_dict_copy(ctx, vars);
        if (JS_IsException(copy))
            return copy;
        slots[QP_CONTEXT_SLOT_VARS] = copy;
        JS_FreeValue(ctx, vars);
        vars = copy;
    }
    return vars;
}

/* context_check_key_type */
static int qp_context_check_key(JSContext *ctx, QPModState *ms, JSValueConst key)
{
    if (!qp_ctxvar_is(ctx, ms, key, QP_CTXVAR_T_VAR)) {
        qp_raise_type_error(ctx, "a ContextVar key was expected, got %R", key);
        return -1;
    }
    return 0;
}

/* _PyContext_Enter */
static int qp_context_enter_ms(JSContext *ctx, QPModState *ms, JSValueConst context)
{
    JSValue *slots;

    if (!qp_ctxvar_is(ctx, ms, context, QP_CTXVAR_T_CONTEXT)) {
        qp_raise_type_error(ctx, "an instance of Context was expected");
        return -1;
    }
    slots = qp_instance_slots(context);
    if (JS_VALUE_GET_INT(slots[QP_CONTEXT_SLOT_ENTERED])) {
        qp_raise_runtime_error(ctx, "cannot enter context: %R is already entered", context);
        return -1;
    }
    slots[QP_CONTEXT_SLOT_ENTERED] = JS_NewInt32(ctx, 1);
    /* the reference of the thread state moves to ctx_prev (CPython keeps
       it the same way: ts->context is overwritten without a decref) */
    slots[QP_CONTEXT_SLOT_PREV] = ms->ctx_current;
    ms->ctx_current = JS_DupValue(ctx, context);
    return 0;
}

/* _PyContext_Exit */
static int qp_context_exit_ms(JSContext *ctx, QPModState *ms, JSValueConst context)
{
    JSValue *slots;
    JSValue old;

    if (!qp_ctxvar_is(ctx, ms, context, QP_CTXVAR_T_CONTEXT)) {
        qp_raise_type_error(ctx, "an instance of Context was expected");
        return -1;
    }
    slots = qp_instance_slots(context);
    if (!JS_VALUE_GET_INT(slots[QP_CONTEXT_SLOT_ENTERED])) {
        qp_raise_runtime_error(ctx, "cannot exit context: %R has not been entered", context);
        return -1;
    }
    if (!qp_ctxvar_same(ms->ctx_current, context)) {
        /* Can only happen if someone misuses the C API */
        qp_raise_runtime_error(ctx, "cannot exit context: thread state references "
                               "a different context object");
        return -1;
    }
    old = ms->ctx_current;
    ms->ctx_current = slots[QP_CONTEXT_SLOT_PREV];
    slots[QP_CONTEXT_SLOT_PREV] = QP_NULL;
    slots[QP_CONTEXT_SLOT_ENTERED] = JS_NewInt32(ctx, 0);
    JS_FreeValue(ctx, old);
    return 0;
}

static int qp_context_enter(JSContext *ctx, JSValueConst context)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    if (!ms)
        return -1;
    return qp_context_enter_ms(ctx, ms, context);
}

static int qp_context_exit(JSContext *ctx, JSValueConst context)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    if (!ms)
        return -1;
    return qp_context_exit_ms(ctx, ms, context);
}

/* ---- ContextVar / Token internals ---------------------------------------- */

/* contextvar_new */
static JSValue qp_ctxvar_new_ms(JSContext *ctx, QPModState *ms, JSValueConst name,
                                JSValueConst dflt)
{
    JSValue obj;
    JSValue *slots;
    Py_hash_t name_hash, h;

    if (!qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "context variable name must be a str");
    obj = qp_object_new_instance(ctx, qp_ctxvar_type(ms, QP_CTXVAR_T_VAR), JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    slots = qp_instance_slots(obj);
    slots[QP_CTXVAR_SLOT_NAME] = JS_DupValue(ctx, name);
    slots[QP_CTXVAR_SLOT_DEFAULT] = QP_IS_NULL(dflt) ? QP_NULL : JS_DupValue(ctx, dflt);

    /* contextvar_generate_hash: hash(name) XOR Py_HashPointer(self), with
       a unique number instead of the address, so that the hash (stored,
       see the HAMT of the contexts) is the same in every process for the
       same program: a heap snapshot base is reproducible */
    name_hash = qp_hash(ctx, name);
    if (name_hash == -1) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    h = (Py_hash_t)qp_hash_pointer((uintptr_t)(++QP_STATE(ctx)->next_dict_uid << 4)) ^ name_hash;
    if (h == -1)
        h = -2;
    slots[QP_CTXVAR_SLOT_HASH_LO] = JS_NewInt32(ctx, (int32_t)(uint32_t)(uint64_t)h);
    slots[QP_CTXVAR_SLOT_HASH_HI] = JS_NewInt32(ctx, (int32_t)(uint32_t)((uint64_t)h >> 32));
    return obj;
}

/* token_new; old QP_NULL = the variable had no value */
static JSValue qp_ctxvar_token_new(JSContext *ctx, QPModState *ms, JSValueConst context,
                                   JSValueConst var, JSValueConst old)
{
    JSValue obj;
    JSValue *slots;

    obj = qp_object_new_instance(ctx, qp_ctxvar_type(ms, QP_CTXVAR_T_TOKEN), JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    slots = qp_instance_slots(obj);
    slots[QP_CTXTOK_SLOT_CTX] = JS_DupValue(ctx, context);
    slots[QP_CTXTOK_SLOT_VAR] = JS_DupValue(ctx, var);
    slots[QP_CTXTOK_SLOT_OLD] = QP_IS_NULL(old) ? QP_NULL : JS_DupValue(ctx, old);
    slots[QP_CTXTOK_SLOT_USED] = JS_NewInt32(ctx, 0);
    return obj;
}

/* contextvar_set: var = val in the current context. */
static int qp_ctxvar_store(JSContext *ctx, QPModState *ms, JSValueConst var, JSValueConst val)
{
    JSValue context = qp_context_get(ctx, ms);
    JSValue vars;

    if (JS_IsException(context))
        return -1;
    vars = qp_context_vars_for_write(ctx, context);
    if (JS_IsException(vars))
        return -1;
    return qp_dict_set_item(ctx, vars, var, val);
}

/* contextvar_del: LookupError(var) if the variable has no value. */
static int qp_ctxvar_delete(JSContext *ctx, QPModState *ms, JSValueConst var)
{
    JSValue context = qp_context_get(ctx, ms);
    JSValue vars;
    int r;

    if (JS_IsException(context))
        return -1;
    r = qp_dict_contains(ctx, qp_context_vars(context), var);
    if (r < 0)
        return -1;
    if (r == 0) {
        qp_raise_value(ctx, QP_EXC(ctx, LookupError), var);
        return -1;
    }
    vars = qp_context_vars_for_write(ctx, context);
    if (JS_IsException(vars))
        return -1;
    return qp_dict_del_item(ctx, vars, var) < 0 ? -1 : 0;
}

/* Token.MISSING singleton (borrowed; it lives in Token.__dict__). */
static JSValue qp_ctxvar_token_missing(JSContext *ctx, QPModState *ms)
{
    JSAtom name = JS_NewAtom(ctx, "MISSING");
    JSValue v;

    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    v = qp_type_lookup(ctx, qp_ctxvar_type(ms, QP_CTXVAR_T_TOKEN), name);
    JS_FreeAtom(ctx, name);
    if (QP_IS_NULL(v))
        return qp_raise_system_error(ctx, "Token.MISSING is missing");
    return v;
}

/* ---- public C API --------------------------------------------------------- */

static JSValue qp_contextvar_new(JSContext *ctx, const char *name, JSValueConst dflt)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue pyname, var;

    if (!ms)
        return JS_EXCEPTION;
    pyname = qp_str_from_cstr(ctx, name);
    if (JS_IsException(pyname))
        return pyname;
    var = qp_ctxvar_new_ms(ctx, ms, pyname, dflt);
    JS_FreeValue(ctx, pyname);
    return var;
}

static int qp_contextvar_get(JSContext *ctx, JSValueConst var, JSValueConst dflt, JSValue *pval)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue dd;

    *pval = QP_NULL;
    if (!ms)
        return -1;
    if (!qp_ctxvar_is(ctx, ms, var, QP_CTXVAR_T_VAR)) {
        qp_raise_type_error(ctx, "an instance of ContextVar was expected");
        return -1;
    }
    if (!QP_IS_NULL(ms->ctx_current)) {
        JSValue found;
        int r = qp_dict_get_item(ctx, qp_context_vars(ms->ctx_current), var, &found);
        if (r < 0)
            return -1;
        if (r == 1) {
            *pval = found;
            return 0;
        }
    }
    /* not found */
    if (QP_IS_NULL(dflt)) {
        dd = qp_instance_slots(var)[QP_CTXVAR_SLOT_DEFAULT];
        if (!QP_IS_NULL(dd))
            *pval = JS_DupValue(ctx, dd);
    } else {
        *pval = JS_DupValue(ctx, dflt);
    }
    return 0;
}

static JSValue qp_contextvar_set(JSContext *ctx, JSValueConst var, JSValueConst val)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue context, old, tok;

    if (!ms)
        return JS_EXCEPTION;
    if (!qp_ctxvar_is(ctx, ms, var, QP_CTXVAR_T_VAR))
        return qp_raise_type_error(ctx, "an instance of ContextVar was expected");
    context = qp_context_get(ctx, ms);
    if (JS_IsException(context))
        return context;
    if (qp_dict_get_item(ctx, qp_context_vars(context), var, &old) < 0)
        return JS_EXCEPTION;
    tok = qp_ctxvar_token_new(ctx, ms, context, var, old);
    JS_FreeValue(ctx, old);
    if (JS_IsException(tok))
        return tok;
    if (qp_ctxvar_store(ctx, ms, var, val) < 0) {
        JS_FreeValue(ctx, tok);
        return JS_EXCEPTION;
    }
    return tok;
}

static int qp_contextvar_reset(JSContext *ctx, JSValueConst var, JSValueConst token)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue *tslots;
    JSValue context, old;

    if (!ms)
        return -1;
    if (!qp_ctxvar_is(ctx, ms, var, QP_CTXVAR_T_VAR)) {
        qp_raise_type_error(ctx, "an instance of ContextVar was expected");
        return -1;
    }
    if (!qp_ctxvar_is(ctx, ms, token, QP_CTXVAR_T_TOKEN)) {
        qp_raise_type_error(ctx, "an instance of Token was expected");
        return -1;
    }
    tslots = qp_instance_slots(token);
    if (JS_VALUE_GET_INT(tslots[QP_CTXTOK_SLOT_USED])) {
        qp_raise_runtime_error(ctx, "%R has already been used once", token);
        return -1;
    }
    if (!qp_ctxvar_same(var, tslots[QP_CTXTOK_SLOT_VAR])) {
        qp_raise_value_error(ctx, "%R was created by a different ContextVar", token);
        return -1;
    }
    context = qp_context_get(ctx, ms);
    if (JS_IsException(context))
        return -1;
    if (!qp_ctxvar_same(context, tslots[QP_CTXTOK_SLOT_CTX])) {
        qp_raise_value_error(ctx, "%R was created in a different Context", token);
        return -1;
    }
    tslots[QP_CTXTOK_SLOT_USED] = JS_NewInt32(ctx, 1);
    old = tslots[QP_CTXTOK_SLOT_OLD];
    if (QP_IS_NULL(old))
        return qp_ctxvar_delete(ctx, ms, var);
    return qp_ctxvar_store(ctx, ms, var, old);
}

static JSValue qp_context_copy_current(JSContext *ctx)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue context;

    if (!ms)
        return JS_EXCEPTION;
    context = qp_context_get(ctx, ms);
    if (JS_IsException(context))
        return context;
    return qp_context_new_from_vars(ctx, ms, qp_context_vars(context));
}

/* ---- Context type --------------------------------------------------------- */

static JSValue qp_context_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    QPModState *ms = qp_ctxvar_state(ctx);

    if (!ms)
        return JS_EXCEPTION;
    if (qp_nargs(nargsf) != 0 || (!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) != 0))
        return qp_raise_type_error(ctx, "Context() does not accept any arguments");
    return qp_context_new_from_vars(ctx, ms, QP_NULL);
}

static JSValue qp_context_tp_iter(JSContext *ctx, JSValueConst self)
{
    /* the dict iterator keeps a reference to the variables, so the
       context copies them before any later mutation: the iterator walks
       a snapshot like the HAMT iterator */
    return qp_iter(ctx, qp_context_vars(self));
}

/* _PyHamt_Eq */
static JSValue qp_context_tp_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue vv, wv, key, val, other;
    Py_ssize_t pos = 0;
    int res = 1, r;

    if (!ms)
        return JS_EXCEPTION;
    if (!qp_ctxvar_is(ctx, ms, v, QP_CTXVAR_T_CONTEXT) ||
        !qp_ctxvar_is(ctx, ms, w, QP_CTXVAR_T_CONTEXT) ||
        (op != QP_CMP_EQ && op != QP_CMP_NE))
        return qp_not_implemented(ctx);

    /* held for the whole walk: value __eq__ methods that set variables
       then copy instead of mutating the dicts being compared */
    vv = JS_DupValue(ctx, qp_context_vars(v));
    wv = JS_DupValue(ctx, qp_context_vars(w));
    if (qp_ctxvar_same(vv, wv)) {
        res = 1;
    } else if (qp_dict_size(vv) != qp_dict_size(wv)) {
        res = 0;
    } else {
        while (qp_dict_next(vv, &pos, &key, &val)) {
            key = JS_DupValue(ctx, key);
            val = JS_DupValue(ctx, val);
            r = qp_dict_get_item(ctx, wv, key, &other);
            if (r == 1) {
                r = qp_richcompare_bool(ctx, val, other, QP_CMP_EQ);
                JS_FreeValue(ctx, other);
            }
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, val);
            if (r < 0) {
                JS_FreeValue(ctx, vv);
                JS_FreeValue(ctx, wv);
                return JS_EXCEPTION;
            }
            if (r == 0) {
                res = 0;
                break;
            }
        }
    }
    JS_FreeValue(ctx, vv);
    JS_FreeValue(ctx, wv);
    if (op == QP_CMP_NE)
        res = !res;
    return JS_NewBool(ctx, res);
}

static Py_ssize_t qp_context_tp_len(JSContext *ctx, JSValueConst self)
{
    return qp_dict_size(qp_context_vars(self));
}

static JSValue qp_context_tp_subscript(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue val;
    int r;

    if (!ms || qp_context_check_key(ctx, ms, key) < 0)
        return JS_EXCEPTION;
    r = qp_dict_get_item(ctx, qp_context_vars(self), key, &val);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_key_error(ctx, key);
    return val;
}

static int qp_context_tp_contains(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    QPModState *ms = qp_ctxvar_state(ctx);

    if (!ms || qp_context_check_key(ctx, ms, key) < 0)
        return -1;
    return qp_dict_contains(ctx, qp_context_vars(self), key);
}

static JSValue qp_context_m_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue val;
    int r;

    if (!ms)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "get", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_context_check_key(ctx, ms, args[0]) < 0)
        return JS_EXCEPTION;
    r = qp_dict_get_item(ctx, qp_context_vars(self), args[0], &val);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return nargs > 1 ? JS_DupValue(ctx, args[1]) : JS_UNDEFINED;
    return val;
}

static JSValue qp_context_m_items(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    return qp_dict_items_list(ctx, qp_context_vars(self));
}

static JSValue qp_context_m_keys(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_dict_keys_list(ctx, qp_context_vars(self));
}

static JSValue qp_context_m_values(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_dict_values_list(ctx, qp_context_vars(self));
}

static JSValue qp_context_m_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_ctxvar_state(ctx);

    if (!ms)
        return JS_EXCEPTION;
    return qp_context_new_from_vars(ctx, ms, qp_context_vars(self));
}

/* context_run */
static JSValue qp_context_m_run(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue res;

    if (!ms)
        return JS_EXCEPTION;
    if (nargs < 1)
        return qp_raise_type_error(ctx, "run() missing 1 required positional argument");
    if (qp_context_enter_ms(ctx, ms, self) < 0)
        return JS_EXCEPTION;
    /* positional args[1..nargs) followed by the keyword values */
    res = qp_vectorcall(ctx, args[0], args + 1, (size_t)(nargs - 1), kwnames);
    if (qp_context_exit_ms(ctx, ms, self) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static const QPMethodDef qp_context_methods[] = {
    { "get", qp_context_m_get, QP_METH_FASTCALL, 0,
      "get($self, key, default=None, /)\n"
      "--\n"
      "\n"
      "Return the value for `key` if `key` has the value in the context object.\n"
      "\n"
      "If `key` does not exist, return `default`.  If `default` is not\n"
      "given, return None." },
    { "items", qp_context_m_items, QP_METH_NOARGS, 0,
      "items($self, /)\n"
      "--\n"
      "\n"
      "Return all variables and their values in the context object.\n"
      "\n"
      "The result is returned as a list of 2-tuples (variable, value)." },
    { "keys", qp_context_m_keys, QP_METH_NOARGS, 0,
      "keys($self, /)\n"
      "--\n"
      "\n"
      "Return a list of all variables in the context object." },
    { "values", qp_context_m_values, QP_METH_NOARGS, 0,
      "values($self, /)\n"
      "--\n"
      "\n"
      "Return a list of all variables' values in the context object." },
    { "copy", qp_context_m_copy, QP_METH_NOARGS, 0,
      "copy($self, /)\n"
      "--\n"
      "\n"
      "Return a shallow copy of the context object." },
    { "run", qp_context_m_run, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_context_spec = {
    .name = "_contextvars.Context",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_CONTEXT_NSLOTS,
    .slots = {
        .tp_hash = qp_hash_not_implemented,
        .tp_richcompare = qp_context_tp_richcompare,
        .tp_iter = qp_context_tp_iter,
        .tp_new = qp_context_tp_new,
        .sq = { .sq_contains = qp_context_tp_contains },
        .mp = { .mp_length = qp_context_tp_len, .mp_subscript = qp_context_tp_subscript },
    },
    .methods = qp_context_methods,
};

/* ---- ContextVar type ------------------------------------------------------ */

static JSValue qp_ctxvar_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "", "default", NULL };
    static const QPArgSpec spec = { "ContextVar", kwlist, 1, 1, 0 };
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValueConst out[2];

    if (!ms)
        return JS_EXCEPTION;
    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, out) < 0)
        return JS_EXCEPTION;
    return qp_ctxvar_new_ms(ctx, ms, out[0], out[1]);
}

static Py_hash_t qp_ctxvar_tp_hash(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    uint64_t lo = (uint32_t)JS_VALUE_GET_INT(slots[QP_CTXVAR_SLOT_HASH_LO]);
    uint64_t hi = (uint32_t)JS_VALUE_GET_INT(slots[QP_CTXVAR_SLOT_HASH_HI]);
    return (Py_hash_t)((hi << 32) | lo);
}

static JSValue qp_ctxvar_tp_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);

    if (QP_IS_NULL(slots[QP_CTXVAR_SLOT_DEFAULT]))
        return qp_str_from_format(ctx, "<ContextVar name=%R at %p>",
                                  slots[QP_CTXVAR_SLOT_NAME], JS_VALUE_GET_PTR(self));
    return qp_str_from_format(ctx, "<ContextVar name=%R default=%R at %p>",
                              slots[QP_CTXVAR_SLOT_NAME], slots[QP_CTXVAR_SLOT_DEFAULT],
                              JS_VALUE_GET_PTR(self));
}

static JSValue qp_ctxvar_m_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValue val;

    if (qp_arg_check_positional(ctx, "get", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (qp_contextvar_get(ctx, self, nargs > 0 ? args[0] : QP_NULL, &val) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(val))
        return qp_raise_value(ctx, QP_EXC(ctx, LookupError), self);
    return val;
}

static JSValue qp_ctxvar_m_set(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    return qp_contextvar_set(ctx, self, args[0]);
}

static JSValue qp_ctxvar_m_reset(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_ctxvar_state(ctx);

    if (!ms)
        return JS_EXCEPTION;
    if (!qp_ctxvar_is(ctx, ms, args[0], QP_CTXVAR_T_TOKEN))
        return qp_raise_type_error(ctx, "expected an instance of Token, got %R", args[0]);
    if (qp_contextvar_reset(ctx, self, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* Py_GenericAlias as a METH_O | METH_CLASS method */
static JSValue qp_ctxvar_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_ctxvar_methods[] = {
    { "get", qp_ctxvar_m_get, QP_METH_FASTCALL, 0,
      "get($self, default=<unrepresentable>, /)\n"
      "--\n"
      "\n"
      "Return a value for the context variable for the current context.\n"
      "\n"
      "If there is no value for the variable in the current context, the\n"
      "method will:\n"
      " * return the value of the default argument of the method, if\n"
      "   provided; or\n"
      " * return the default value for the context variable, if it was\n"
      "   created with one; or\n"
      " * raise a LookupError." },
    { "set", qp_ctxvar_m_set, QP_METH_O, 0,
      "set($self, value, /)\n"
      "--\n"
      "\n"
      "Call to set a new value for the context variable in the current context.\n"
      "\n"
      "The required value argument is the new value for the context\n"
      "variable.\n"
      "\n"
      "Returns a Token object that can be used to restore the variable to\n"
      "its previous value via the `ContextVar.reset()` method." },
    { "reset", qp_ctxvar_m_reset, QP_METH_O, 0,
      "reset($self, token, /)\n"
      "--\n"
      "\n"
      "Reset the context variable.\n"
      "\n"
      "The variable is reset to the value it had before the\n"
      "`ContextVar.set()` that created the token was used." },
    { "__class_getitem__", qp_ctxvar_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "ContextVars are generic over the type of their contained values" },
    { NULL }
};

static const QPMemberDef qp_ctxvar_members[] = {
    { "name", QP_CTXVAR_SLOT_NAME, QP_MEMBER_READONLY, NULL },
    { NULL }
};

static const QPTypeSpec qp_ctxvar_spec = {
    .name = "_contextvars.ContextVar",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_CTXVAR_NSLOTS,
    .slots = {
        .tp_repr = qp_ctxvar_tp_repr,
        .tp_hash = qp_ctxvar_tp_hash,
        .tp_new = qp_ctxvar_tp_new,
    },
    .methods = qp_ctxvar_methods,
    .members = qp_ctxvar_members,
};

/* ---- Token type ----------------------------------------------------------- */

static JSValue qp_ctxtok_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    return qp_raise_runtime_error(ctx, "Tokens can only be created by ContextVars");
}

static JSValue qp_ctxtok_tp_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);

    return qp_str_from_format(ctx, "<Token%s var=%R at %p>",
                              JS_VALUE_GET_INT(slots[QP_CTXTOK_SLOT_USED]) ? " used" : "",
                              slots[QP_CTXTOK_SLOT_VAR], JS_VALUE_GET_PTR(self));
}

static JSValue qp_ctxtok_get_var(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, qp_instance_slots(self)[QP_CTXTOK_SLOT_VAR]);
}

static JSValue qp_ctxtok_get_old_value(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue old = qp_instance_slots(self)[QP_CTXTOK_SLOT_OLD];
    QPModState *ms;
    JSValue missing;

    if (!QP_IS_NULL(old))
        return JS_DupValue(ctx, old);
    ms = qp_ctxvar_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    missing = qp_ctxvar_token_missing(ctx, ms);
    if (JS_IsException(missing))
        return missing;
    return JS_DupValue(ctx, missing);
}

static JSValue qp_ctxtok_m_enter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, self);
}

static JSValue qp_ctxtok_m_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "__exit__", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    if (qp_contextvar_reset(ctx, qp_instance_slots(self)[QP_CTXTOK_SLOT_VAR], self) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const QPGetSetDef qp_ctxtok_getsets[] = {
    { "var", qp_ctxtok_get_var, NULL, 0, NULL },
    { "old_value", qp_ctxtok_get_old_value, NULL, 0, NULL },
    { NULL }
};

static const QPMethodDef qp_ctxtok_methods[] = {
    { "__class_getitem__", qp_ctxvar_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Tokens are generic over the same type as the ContextVar which created them." },
    { "__enter__", qp_ctxtok_m_enter, QP_METH_NOARGS, 0,
      "__enter__($self, /)\n"
      "--\n"
      "\n"
      "Enter into Token context manager." },
    { "__exit__", qp_ctxtok_m_exit, QP_METH_FASTCALL, 0,
      "__exit__($self, type, val, tb, /)\n"
      "--\n"
      "\n"
      "Exit from Token context manager, restore the linked ContextVar." },
    { NULL }
};

static const QPTypeSpec qp_ctxtok_spec = {
    .name = "_contextvars.Token",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_CTXTOK_NSLOTS,
    .slots = {
        .tp_repr = qp_ctxtok_tp_repr,
        .tp_hash = qp_hash_not_implemented,
        .tp_new = qp_ctxtok_tp_new,
    },
    .methods = qp_ctxtok_methods,
    .getsets = qp_ctxtok_getsets,
};

/* ---- Token.MISSING -------------------------------------------------------- */

static JSValue qp_ctxtok_missing_tp_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_cstr(ctx, "<Token.MISSING>");
}

/* _PyContextTokenMissing_Type: a static type without tp_new, which
   PyType_Ready marks DISALLOW_INSTANTIATION ("cannot create
   'Token.MISSING' instances"). */
static const QPTypeSpec qp_ctxtok_missing_spec = {
    .name = "Token.MISSING",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_ctxtok_missing_tp_repr,
    },
};

/* Create Context, ContextVar, Token (with Token.MISSING, _PyContext_Init)
   into ms->ctx_types; all or nothing. */
static int qp_ctxvar_init_types(JSContext *ctx, QPModState *ms)
{
    JSValue types[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };
    JSValue mtype = JS_UNDEFINED, missing = JS_UNDEFINED;
    QPType *tok;
    JSAtom name;
    int i, r;

    types[QP_CTXVAR_T_CONTEXT] = qp_type_from_spec(ctx, &qp_context_spec);
    if (JS_IsException(types[QP_CTXVAR_T_CONTEXT]))
        goto fail;
    types[QP_CTXVAR_T_VAR] = qp_type_from_spec(ctx, &qp_ctxvar_spec);
    if (JS_IsException(types[QP_CTXVAR_T_VAR]))
        goto fail;
    types[QP_CTXVAR_T_TOKEN] = qp_type_from_spec(ctx, &qp_ctxtok_spec);
    if (JS_IsException(types[QP_CTXVAR_T_TOKEN]))
        goto fail;
    mtype = qp_type_from_spec(ctx, &qp_ctxtok_missing_spec);
    if (JS_IsException(mtype))
        goto fail;
    missing = qp_object_new_instance(ctx, qp_type_from_value(mtype), JS_CLASS_PY_OBJECT);
    if (JS_IsException(missing))
        goto fail;

    /* Token.MISSING */
    tok = qp_type_from_value(types[QP_CTXVAR_T_TOKEN]);
    name = JS_NewAtom(ctx, "MISSING");
    if (name == JS_ATOM_NULL)
        goto fail;
    r = qp_dict_set_item_atom(ctx, tok->dict, name, missing);
    JS_FreeAtom(ctx, name);
    if (r < 0)
        goto fail;
    qp_type_modified(ctx, tok);
    JS_FreeValue(ctx, missing);
    JS_FreeValue(ctx, mtype);

    for (i = 0; i < 3; i++)
        ms->ctx_types[i] = types[i];
    return 0;

fail:
    for (i = 0; i < 3; i++)
        JS_FreeValue(ctx, types[i]);
    JS_FreeValue(ctx, mtype);
    JS_FreeValue(ctx, missing);
    return -1;
}

/* ---- module --------------------------------------------------------------- */

static JSValue qp_ctxvar_copy_context(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_context_copy_current(ctx);
}

static const QPMethodDef qp_ctxvar_module_methods[] = {
    { "copy_context", qp_ctxvar_copy_context, QP_METH_NOARGS, 0,
      "copy_context($module, /)\n"
      "--\n"
      "\n" },
    { NULL }
};

static int qp_ctxvar_exec(JSContext *ctx, JSValueConst module)
{
    static const char *const names[3] = { "Context", "ContextVar", "Token" };
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue dict;
    JSAtom name;
    int i, r;

    if (!ms)
        return -1;
    dict = qp_module_get_dict(module);
    for (i = 0; i < 3; i++) {
        name = JS_NewAtom(ctx, names[i]);
        if (name == JS_ATOM_NULL)
            return -1;
        r = qp_dict_set_item_atom(ctx, dict, name, ms->ctx_types[i]);
        JS_FreeAtom(ctx, name);
        if (r < 0)
            return -1;
    }
    return 0;
}

static const QPModuleDef qp_moddef__contextvars = {
    "_contextvars",
    "Context Variables",
    qp_ctxvar_module_methods,
    qp_ctxvar_exec,
};

/*
 * _warnings: port of CPython 3.15 Python/_warnings.c (PSF-2.0).
 *
 * The interpreter-level warnings state (filters, once registry, default
 * action, the _warnings_context ContextVar, filters version, lock) lives in
 * QPModState.warn_*; the module attributes `filters`, `_onceregistry`,
 * `_defaultaction` and `_warnings_context` ARE those objects, and the C code
 * re-reads the Python `warnings` module attributes like get_warnings_attr().
 *
 * Locking: CPython's recursive mutex is the interpreter recursive mutex
 * QP_RMUTEX_WARNINGS (python/ceval.c): warn_lock_count is its level, the
 * owning thread is kept by the GIL code; releasing a lock the thread does
 * not hold raises RuntimeError like _PyRecursiveMutex_TryUnlock failing.
 *
 * Finalization: CPython skips importing `warnings` once the interpreter is
 * finalizing; this runtime has no finalizing phase in which Python code may
 * still run, so the import is attempted whenever sys.modules exists.
 */
#include "modstate.h"

/* python/mod_misc_contextvars (same TU) */
static JSValue qp_contextvar_new(JSContext *ctx, const char *name, JSValueConst dflt);
static int qp_contextvar_get(JSContext *ctx, JSValueConst var, JSValueConst dflt, JSValue *pval);

static int qp_warnings_warn(JSContext *ctx, QPType *category, JSValueConst message,
                            Py_ssize_t stack_level, JSValueConst source);
static int qp_warnings_warn_explicit(JSContext *ctx, QPType *category, JSValueConst message,
                                     JSValueConst filename, int lineno, JSValueConst module,
                                     JSValueConst registry, JSValueConst source);

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */

static void qp_warnings_clear_error(JSContext *ctx)
{
    JS_FreeValue(ctx, JS_GetException(ctx));
}

/* PyObject_GetOptionalAttr with a C-string name. */
static int qp_warnings_getattr_opt(JSContext *ctx, JSValueConst obj, const char *name,
                                   JSValue *pres)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;
    if (a == JS_ATOM_NULL) {
        *pres = QP_NULL;
        return -1;
    }
    r = qp_getattr_opt(ctx, obj, a, pres);
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue qp_warnings_getattr(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue qp_warnings_call_method1(JSContext *ctx, JSValueConst obj, const char *name,
                                        JSValueConst arg)
{
    JSValue m = qp_warnings_getattr(ctx, obj, name), r;
    if (JS_IsException(m))
        return m;
    r = QP_IS_NULL(arg) ? qp_call0(ctx, m) : qp_call1(ctx, m, arg);
    JS_FreeValue(ctx, m);
    return r;
}

/* Py_TYPE(v)->tp_name as a str. */
static JSValue qp_warnings_tp_name(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* PyCallable_Check */
static BOOL qp_warnings_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* ASCII action name comparison (_PyUnicode_EqualToASCIIString). */
static BOOL qp_warnings_action_is(JSContext *ctx, JSValueConst action, const char *s)
{
    size_t len;
    const char *u = qp_str_utf8(ctx, action, &len);
    BOOL r;
    if (!u) {
        qp_warnings_clear_error(ctx);
        return FALSE;
    }
    r = len == strlen(s) && memcmp(u, s, len) == 0;
    JS_FreeCString(ctx, u);
    return r;
}

/* ---------------------------------------------------------------------- */
/* state                                                                   */

static JSValue qp_warnings_create_filter(JSContext *ctx, QPType *category, const char *action,
                                         const char *modname)
{
    JSValue items[5];
    items[0] = qp_str_from_cstr(ctx, action);
    items[1] = JS_UNDEFINED;
    items[2] = JS_DupValue(ctx, QP_TYPE_VALUE(category));
    items[3] = modname ? qp_str_from_cstr(ctx, modname) : JS_UNDEFINED;
    items[4] = JS_NewInt32(ctx, 0);
    if (JS_IsException(items[0]) || JS_IsException(items[3])) {
        int i;
        for (i = 0; i < 5; i++)
            if (!JS_IsException(items[i]))
                JS_FreeValue(ctx, items[i]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 5, items);
}

/* init_filters of a release (non-Py_DEBUG) build. */
static JSValue qp_warnings_init_filters(JSContext *ctx)
{
    JSValue items[5];
    int n = 0, i;
#define QP_WARNINGS_ADD(TYPE, ACTION, MODNAME)                                       \
    do {                                                                             \
        items[n] = qp_warnings_create_filter(ctx, QP_EXC(ctx, TYPE), ACTION, MODNAME); \
        if (JS_IsException(items[n]))                                                \
            goto fail;                                                               \
        n++;                                                                         \
    } while (0)
    QP_WARNINGS_ADD(DeprecationWarning, "default", "__main__");
    QP_WARNINGS_ADD(DeprecationWarning, "ignore", NULL);
    QP_WARNINGS_ADD(PendingDeprecationWarning, "ignore", NULL);
    QP_WARNINGS_ADD(ImportWarning, "ignore", NULL);
    QP_WARNINGS_ADD(ResourceWarning, "ignore", NULL);
#undef QP_WARNINGS_ADD
    return qp_list_new_steal(ctx, n, items);
 fail:
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, items[i]);
    return JS_EXCEPTION;
}

/* _PyWarnings_InitState: NULL + exception on failure. */
static QPModState *qp_warnings_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    JSValue v;
    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->warn_filters)) {
        v = qp_warnings_init_filters(ctx);
        if (JS_IsException(v))
            return NULL;
        ms->warn_filters = v;
        ms->warn_filters_version = 0;
    }
    if (QP_IS_NULL(ms->warn_once_registry)) {
        v = qp_dict_new(ctx);
        if (JS_IsException(v))
            return NULL;
        ms->warn_once_registry = v;
    }
    if (QP_IS_NULL(ms->warn_default_action)) {
        v = qp_str_from_cstr(ctx, "default");
        if (JS_IsException(v))
            return NULL;
        ms->warn_default_action = v;
    }
    if (QP_IS_NULL(ms->warn_context)) {
        v = qp_contextvar_new(ctx, "_warnings_context", QP_NULL);
        if (JS_IsException(v))
            return NULL;
        ms->warn_context = v;
    }
    return ms;
}

/* the lock is the recursive mutex QP_RMUTEX_WARNINGS of python/ceval.c
   (level: ms->warn_lock_count) */
static void qp_warnings_lock(JSContext *ctx, QPModState *ms)
{
    (void)ms;
    qp_rmutex_lock(ctx, QP_RMUTEX_WARNINGS);
}

static int qp_warnings_unlock(JSContext *ctx, QPModState *ms)
{
    (void)ms;
    return qp_rmutex_unlock(ctx, QP_RMUTEX_WARNINGS);
}

/* ---------------------------------------------------------------------- */

static int qp_warnings_check_matched(JSContext *ctx, JSValueConst obj, JSValueConst arg,
                                     JSValueConst arg2)
{
    JSValue result;
    int rc;

    /* A 'None' filter always matches */
    if (JS_IsUndefined(obj) || JS_IsNull(obj))
        return 1;

    /* An internal plain text default filter must match exactly */
    if (qp_str_check_exact(obj)) {
        int cmp;
        if (QP_IS_NULL(arg))
            return 0;
        if (!qp_str_check(ctx, arg)) {
            qp_raise_type_error(ctx, "Can't compare %T and %T", obj, arg);
            return -1;
        }
        cmp = qp_str_compare(ctx, obj, arg);
        if (cmp == INT_MIN)
            return -1;
        return cmp == 0;
    }

    /* Otherwise assume a regex filter and call its match() method */
    if (!QP_IS_NULL(arg)) {
        result = qp_warnings_call_method1(ctx, obj, "match", arg);
    } else {
        JSValue mod = qp_import_module(ctx, "_py_warnings"), match;
        if (JS_IsException(mod))
            return -1;
        match = qp_warnings_getattr(ctx, mod, "_match_filename");
        JS_FreeValue(ctx, mod);
        if (JS_IsException(match))
            return -1;
        result = qp_call2(ctx, match, obj, QP_IS_NULL(arg2) ? JS_UNDEFINED : arg2);
        JS_FreeValue(ctx, match);
    }
    if (JS_IsException(result))
        return -1;
    rc = qp_bool(ctx, result);
    JS_FreeValue(ctx, result);
    return rc;
}

/* get_warnings_attr: new ref, or QP_NULL meaning "absent" (no exception)
   or error (exception pending; callers test with JS_HasException). */
static JSValue qp_warnings_get_warnings_attr(JSContext *ctx, const char *attr, int try_import)
{
    JSValue warnings_module, obj;
    QPState *s = QP_STATE(ctx);

    if (try_import) {
        warnings_module = qp_import_module(ctx, "warnings");
        if (JS_IsException(warnings_module)) {
            /* Fallback to the C implementation if we cannot get
               the Python implementation */
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, ImportError)))
                qp_warnings_clear_error(ctx);
            return QP_NULL;
        }
    } else {
        int r;
        JSValue key;
        if (QP_IS_NULL(s->modules) || !qp_dict_check(ctx, s->modules))
            return QP_NULL;
        key = qp_str_from_cstr(ctx, "warnings");
        if (JS_IsException(key))
            return QP_NULL;
        r = qp_dict_get_item(ctx, s->modules, key, &warnings_module);
        JS_FreeValue(ctx, key);
        if (r <= 0)
            return QP_NULL;
        /* PyImport_GetModule: a None entry means "not imported" */
        if (JS_IsUndefined(warnings_module) || JS_IsNull(warnings_module)) {
            JS_FreeValue(ctx, warnings_module);
            return QP_NULL;
        }
    }

    (void)qp_warnings_getattr_opt(ctx, warnings_module, attr, &obj);
    JS_FreeValue(ctx, warnings_module);
    return obj;
}

static JSValue qp_warnings_get_context(JSContext *ctx, QPModState *ms)
{
    JSValue v;
    if (qp_contextvar_get(ctx, ms->warn_context, QP_NULL, &v) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(v))
        return JS_UNDEFINED;
    return v;
}

static JSValue qp_warnings_get_context_filters(JSContext *ctx, QPModState *ms)
{
    JSValue c = qp_warnings_get_context(ctx, ms), filters;
    if (JS_IsException(c))
        return c;
    if (JS_IsUndefined(c) || JS_IsNull(c))
        return JS_UNDEFINED;
    filters = qp_warnings_getattr(ctx, c, "_filters");
    JS_FreeValue(ctx, c);
    if (JS_IsException(filters))
        return filters;
    if (!qp_list_check(ctx, filters)) {
        JS_FreeValue(ctx, filters);
        return qp_raise_value_error(ctx, "_filters of warnings._warnings_context must be a list");
    }
    return filters;
}

/* Borrowed reference to the filters list (or JS_EXCEPTION). */
static JSValue qp_warnings_get_filters(JSContext *ctx, QPModState *ms)
{
    JSValue f = qp_warnings_get_warnings_attr(ctx, "filters", 0);
    if (QP_IS_NULL(f)) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
    } else {
        JS_FreeValue(ctx, ms->warn_filters);
        ms->warn_filters = f;
    }
    if (QP_IS_NULL(ms->warn_filters) || !qp_list_check(ctx, ms->warn_filters))
        return qp_raise_value_error(ctx, "_warnings.filters must be a list");
    return ms->warn_filters;
}

/* Borrowed. */
static JSValue qp_warnings_get_once_registry(JSContext *ctx, QPModState *ms)
{
    JSValue r = qp_warnings_get_warnings_attr(ctx, "onceregistry", 0), name;
    if (QP_IS_NULL(r)) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return ms->warn_once_registry;
    }
    if (!qp_dict_check(ctx, r)) {
        name = qp_warnings_tp_name(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "_warnings.onceregistry must be a dict, not '%.200U'", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ms->warn_once_registry);
    ms->warn_once_registry = r;
    return r;
}

/* Borrowed. */
static JSValue qp_warnings_get_default_action(JSContext *ctx, QPModState *ms)
{
    JSValue a = qp_warnings_get_warnings_attr(ctx, "defaultaction", 0), name;
    if (QP_IS_NULL(a)) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return ms->warn_default_action;
    }
    if (!qp_str_check(ctx, a)) {
        name = qp_warnings_tp_name(ctx, a);
        JS_FreeValue(ctx, a);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "_warnings.defaultaction must be a string, not '%.200U'", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ms->warn_default_action);
    ms->warn_default_action = a;
    return a;
}

/* filter_search: -1 error; 0 ok.  On a match *item is a new ref to the
   filter tuple and *action a new ref to its action, else both QP_NULL. */
static int qp_warnings_filter_search(JSContext *ctx, JSValueConst category, JSValueConst text,
                                     Py_ssize_t lineno, JSValueConst module,
                                     JSValueConst filename, const char *list_name,
                                     JSValueConst filters, JSValue *item, JSValue *action)
{
    Py_ssize_t i;
    *action = QP_NULL;
    for (i = 0; i < qp_list_size(filters); i++) {
        Py_ssize_t len, ln;
        JSValue *items = qp_list_items(filters, &len);
        JSValue tmp;
        JSValueConst act, msg, cat, mod, ln_obj;
        int good_msg, good_mod, is_subclass;

        if (i >= len)
            break;
        tmp = items[i];
        if (!qp_tuple_check(ctx, tmp) || QP_TUPLE_SIZE(tmp) != 5) {
            qp_raise_value_error(ctx, "warnings.%s item %zd isn't a 5-tuple", list_name, i);
            return -1;
        }
        tmp = JS_DupValue(ctx, tmp);
        act = QP_TUPLE_ITEMS(tmp)[0];
        msg = QP_TUPLE_ITEMS(tmp)[1];
        cat = QP_TUPLE_ITEMS(tmp)[2];
        mod = QP_TUPLE_ITEMS(tmp)[3];
        ln_obj = QP_TUPLE_ITEMS(tmp)[4];

        if (!qp_str_check(ctx, act)) {
            JSValue name = qp_warnings_tp_name(ctx, act);
            if (!JS_IsException(name)) {
                qp_raise_type_error(ctx, "action must be a string, not '%.200U'", name);
                JS_FreeValue(ctx, name);
            }
            JS_FreeValue(ctx, tmp);
            return -1;
        }
        good_msg = qp_warnings_check_matched(ctx, msg, text, QP_NULL);
        if (good_msg < 0)
            goto fail;
        good_mod = qp_warnings_check_matched(ctx, mod, module, filename);
        if (good_mod < 0)
            goto fail;
        is_subclass = qp_issubclass(ctx, category, cat);
        if (is_subclass < 0)
            goto fail;
        /* PyLong_AsSsize_t: int required (no __index__) */
        if (!qp_long_check(ctx, ln_obj)) {
            qp_raise_type_error(ctx, "'%T' object cannot be interpreted as an integer", ln_obj);
            goto fail;
        }
        if (qp_as_ssize(ctx, ln_obj, QP_EXC(ctx, OverflowError), &ln) < 0)
            goto fail;
        if (good_msg && is_subclass && good_mod && (ln == 0 || lineno == ln)) {
            *item = tmp;
            *action = JS_DupValue(ctx, act);
            return 0;
        }
        JS_FreeValue(ctx, tmp);
        continue;
    fail:
        JS_FreeValue(ctx, tmp);
        return -1;
    }
    return 0;
}

/* get_filter: new ref to the action (JS_EXCEPTION on error); *item new ref. */
static JSValue qp_warnings_get_filter(JSContext *ctx, QPModState *ms, JSValueConst category,
                                      JSValueConst text, Py_ssize_t lineno,
                                      JSValueConst module, JSValueConst filename, JSValue *item)
{
    JSValue context_filters, action;
    BOOL use_global_filters = FALSE;

    /* check _warning_context _filters list */
    context_filters = qp_warnings_get_context_filters(ctx, ms);
    if (JS_IsException(context_filters))
        return JS_EXCEPTION;
    if (JS_IsUndefined(context_filters)) {
        use_global_filters = TRUE;
    } else {
        int r = qp_warnings_filter_search(ctx, category, text, lineno, module, filename,
                                          "_warnings_context _filters", context_filters,
                                          item, &action);
        JS_FreeValue(ctx, context_filters);
        if (r < 0)
            return JS_EXCEPTION;
        if (!QP_IS_NULL(action))
            return action;
    }

    if (use_global_filters) {
        /* check warnings.filters list */
        JSValue filters = qp_warnings_get_filters(ctx, ms);
        int r;
        if (JS_IsException(filters))
            return JS_EXCEPTION;
        filters = JS_DupValue(ctx, filters);
        r = qp_warnings_filter_search(ctx, category, text, lineno, module, filename,
                                      "filters", filters, item, &action);
        JS_FreeValue(ctx, filters);
        if (r < 0)
            return JS_EXCEPTION;
        if (!QP_IS_NULL(action))
            return action;
    }

    action = qp_warnings_get_default_action(ctx, ms);
    if (JS_IsException(action))
        return action;
    *item = JS_UNDEFINED;
    return JS_DupValue(ctx, action);
}

static int qp_warnings_dict_clear(JSContext *ctx, JSValueConst d)
{
    JSValue keys = qp_dict_keys_list(ctx, d);
    Py_ssize_t i, n;
    JSValue *items;
    if (JS_IsException(keys))
        return -1;
    items = qp_list_items(keys, &n);
    for (i = 0; i < n; i++) {
        if (qp_dict_del_item(ctx, d, items[i]) < 0) {
            JS_FreeValue(ctx, keys);
            return -1;
        }
    }
    JS_FreeValue(ctx, keys);
    return 0;
}

static int qp_warnings_already_warned(JSContext *ctx, QPModState *ms, JSValueConst registry,
                                      JSValueConst key, int should_set)
{
    JSValue version_obj, already, vkey;
    BOOL should_update_version;
    int r;

    vkey = qp_str_from_cstr(ctx, "version");
    if (JS_IsException(vkey))
        return -1;
    r = qp_dict_get_item(ctx, registry, vkey, &version_obj);
    if (r < 0)
        goto fail;
    should_update_version = TRUE;
    if (r > 0 && qp_long_check_exact(version_obj)) {
        int64_t v;
        int overflow = 0;
        if (qp_long_as_int64_overflow(version_obj, &v, &overflow) == 0 && !overflow &&
            v == ms->warn_filters_version)
            should_update_version = FALSE;
    }
    if (r > 0)
        JS_FreeValue(ctx, version_obj);
    if (should_update_version) {
        if (qp_warnings_dict_clear(ctx, registry) < 0)
            goto fail;
        version_obj = qp_long_from_int64(ctx, ms->warn_filters_version);
        if (JS_IsException(version_obj))
            goto fail;
        r = qp_dict_set_item(ctx, registry, vkey, version_obj);
        JS_FreeValue(ctx, version_obj);
        if (r < 0)
            goto fail;
    } else {
        r = qp_dict_get_item(ctx, registry, key, &already);
        if (r < 0)
            goto fail;
        if (r > 0) {
            int rc = qp_bool(ctx, already);
            JS_FreeValue(ctx, already);
            if (rc != 0) {
                JS_FreeValue(ctx, vkey);
                return rc;
            }
        }
    }
    JS_FreeValue(ctx, vkey);
    /* This warning wasn't found in the registry, set it. */
    if (should_set)
        return qp_dict_set_item(ctx, registry, key, JS_TRUE);
    return 0;
 fail:
    JS_FreeValue(ctx, vkey);
    return -1;
}

static int qp_warnings_update_registry(JSContext *ctx, QPModState *ms, JSValueConst registry,
                                       JSValueConst text, JSValueConst category, int add_zero)
{
    JSValueConst items[3];
    JSValue altkey;
    int rc;
    items[0] = text;
    items[1] = category;
    items[2] = JS_NewInt32(ctx, 0);
    altkey = qp_tuple_pack(ctx, add_zero ? 3 : 2, items);
    if (JS_IsException(altkey))
        return -1;
    rc = qp_warnings_already_warned(ctx, ms, registry, altkey, 1);
    JS_FreeValue(ctx, altkey);
    return rc;
}

/* PyFile_WriteObject(obj, f, Py_PRINT_RAW) */
static int qp_warnings_file_write_obj(JSContext *ctx, JSValueConst f, JSValueConst obj)
{
    JSValue s = qp_str(ctx, obj), r;
    if (JS_IsException(s))
        return -1;
    r = qp_warnings_call_method1(ctx, f, "write", s);
    JS_FreeValue(ctx, s);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_warnings_file_write_str(JSContext *ctx, JSValueConst f, const char *text)
{
    JSValue s = qp_str_from_cstr(ctx, text);
    int r;
    if (JS_IsException(s))
        return -1;
    r = qp_warnings_file_write_obj(ctx, f, s);
    JS_FreeValue(ctx, s);
    return r;
}

/* python/traceback.c (M2ErrTb): linecache.getline(filename, lineno).strip()
   as a new str, QP_NULL if unavailable. */
static JSValue qp_tb_source_line_stripped(JSContext *ctx, JSValueConst filename, int lineno);

/* _Py_DisplaySourceLine(f, filename, lineno, indent=2): leading whitespace
   stripped, indented by two spaces.  Errors are cleared by the caller. */
static void qp_warnings_display_source_line(JSContext *ctx, JSValueConst f,
                                            JSValueConst filename, int lineno)
{
    JSValue stripped = qp_tb_source_line_stripped(ctx, filename, lineno);

    if (QP_IS_NULL(stripped) || JS_IsException(stripped))
        return;
    if (qp_str_len(ctx, stripped) > 0 &&
        qp_warnings_file_write_str(ctx, f, "  ") == 0 &&
        qp_warnings_file_write_obj(ctx, f, stripped) == 0)
        qp_warnings_file_write_str(ctx, f, "\n");
    JS_FreeValue(ctx, stripped);
}

static void qp_warnings_show_warning(JSContext *ctx, JSValueConst filename, int lineno,
                                     JSValueConst text, JSValueConst category,
                                     JSValueConst sourceline)
{
    JSValue f_stderr = QP_NULL, name;
    char lineno_str[128];

    snprintf(lineno_str, sizeof(lineno_str), ":%d: ", lineno);

    name = qp_getattr(ctx, category, QP_ATOM(ctx, __name__));
    if (JS_IsException(name))
        goto error;

    f_stderr = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
    if (QP_IS_NULL(f_stderr) || JS_IsUndefined(f_stderr)) {
        fprintf(stderr, "lost sys.stderr\n");
        goto error;
    }

    /* Print "filename:lineno: category: text\n" */
    if (qp_warnings_file_write_obj(ctx, f_stderr, filename) < 0 ||
        qp_warnings_file_write_str(ctx, f_stderr, lineno_str) < 0 ||
        qp_warnings_file_write_obj(ctx, f_stderr, name) < 0 ||
        qp_warnings_file_write_str(ctx, f_stderr, ": ") < 0 ||
        qp_warnings_file_write_obj(ctx, f_stderr, text) < 0 ||
        qp_warnings_file_write_str(ctx, f_stderr, "\n") < 0)
        goto error;

    /* Print "  source_line\n" */
    if (!QP_IS_NULL(sourceline)) {
        /* CPython computes the stripped line but writes the original one. */
        qp_warnings_file_write_obj(ctx, f_stderr, sourceline);
        qp_warnings_file_write_str(ctx, f_stderr, "\n");
    } else {
        qp_warnings_display_source_line(ctx, f_stderr, filename, lineno);
    }

 error:
    if (!JS_IsException(name))
        JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, f_stderr);
    if (JS_HasException(ctx))
        qp_warnings_clear_error(ctx);
}

static int qp_warnings_call_show_warning(JSContext *ctx, JSValueConst category,
                                         JSValueConst text, JSValueConst message,
                                         JSValueConst filename, int lineno,
                                         JSValueConst lineno_obj, JSValueConst sourceline,
                                         JSValueConst source, JSValueConst module)
{
    JSValue show_fn, warnmsg_cls, msg, res;
    JSValueConst args[8];
    int nargs;

    /* The Python implementation is able to log the traceback where the source
       was allocated, whereas the C implementation doesn't. */
    show_fn = qp_warnings_get_warnings_attr(ctx, "_showwarnmsg", 1);
    if (QP_IS_NULL(show_fn)) {
        if (JS_HasException(ctx))
            return -1;
        qp_warnings_show_warning(ctx, filename, lineno, text, category, sourceline);
        return 0;
    }

    if (!qp_warnings_callable(ctx, show_fn)) {
        qp_raise_type_error(ctx, "warnings._showwarnmsg() must be set to a callable");
        goto error;
    }

    warnmsg_cls = qp_warnings_get_warnings_attr(ctx, "WarningMessage", 0);
    if (QP_IS_NULL(warnmsg_cls)) {
        if (!JS_HasException(ctx))
            qp_raise_runtime_error(ctx, "unable to get warnings.WarningMessage");
        goto error;
    }

    args[0] = message;
    args[1] = category;
    args[2] = filename;
    args[3] = lineno_obj;
    args[4] = JS_UNDEFINED;
    args[5] = QP_IS_NULL(sourceline) ? JS_UNDEFINED : sourceline;
    args[6] = QP_IS_NULL(source) ? JS_UNDEFINED : source;
    args[7] = module;
    /* PyObject_CallFunctionObjArgs stops at a NULL module */
    nargs = QP_IS_NULL(module) ? 7 : 8;
    msg = qp_vectorcall(ctx, warnmsg_cls, args, nargs, QP_NULL);
    JS_FreeValue(ctx, warnmsg_cls);
    if (JS_IsException(msg))
        goto error;

    res = qp_call1(ctx, show_fn, msg);
    JS_FreeValue(ctx, show_fn);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
    return 0;

 error:
    JS_FreeValue(ctx, show_fn);
    return -1;
}

/* PyErr_SetObject(category, message) */
static void qp_warnings_raise(JSContext *ctx, JSValueConst category, JSValueConst message)
{
    QPType *t = qp_type_from_value(category);
    if (!t || !qp_exc_type_check(category)) {
        qp_raise_system_error(ctx, "_PyErr_SetObject: exception %R is not a BaseException subclass",
                              category);
        return;
    }
    qp_raise_value(ctx, t, message);
}

/* warn_explicit: 0 (None) or -1.  Caller holds the lock.  module and
   registry and sourceline and source may be QP_NULL. */
static int qp_warnings_do_warn_explicit(JSContext *ctx, QPModState *ms, JSValueConst category,
                                        JSValueConst message_in, JSValueConst filename,
                                        int lineno, JSValueConst module, JSValueConst registry,
                                        JSValueConst sourceline, JSValueConst source)
{
    JSValue key = JS_UNDEFINED, text = JS_UNDEFINED, lineno_obj, item = JS_UNDEFINED;
    JSValue message, action = JS_UNDEFINED;
    JSValueConst cat = category;
    BOOL has_registry;
    int rc, ret = -1;

    /* module can be None if a warning is emitted late during Python shutdown.
       In this case, the Python warnings module was probably unloaded, filters
       are no more available to choose as action. It is safer to ignore the
       warning and do nothing. */
    if (!QP_IS_NULL(module) && JS_IsUndefined(module))
        return 0;

    has_registry = !QP_IS_NULL(registry) && !JS_IsUndefined(registry) && !JS_IsNull(registry);
    if (has_registry && !qp_dict_check(ctx, registry)) {
        qp_raise_type_error(ctx, "'registry' must be a dict or None");
        return -1;
    }

    /* Normalize message. */
    if (qp_typecheck(ctx, message_in, QP_EXC(ctx, Warning))) {
        text = qp_str(ctx, message_in);
        if (JS_IsException(text))
            return -1;
        message = JS_DupValue(ctx, message_in);
        cat = QP_TYPE_VALUE(qp_type_of(ctx, message_in));
    } else {
        text = JS_DupValue(ctx, message_in);
        message = qp_call1(ctx, category, message_in);
        if (JS_IsException(message)) {
            JS_FreeValue(ctx, text);
            return -1;
        }
    }

    lineno_obj = JS_NewInt32(ctx, lineno);
    if (!QP_IS_NULL(source) && (JS_IsUndefined(source) || JS_IsNull(source)))
        source = QP_NULL;

    /* Create key. */
    {
        JSValueConst items[3] = { text, cat, lineno_obj };
        key = qp_tuple_pack(ctx, 3, items);
        if (JS_IsException(key)) {
            key = JS_UNDEFINED;
            goto cleanup;
        }
    }

    if (has_registry) {
        rc = qp_warnings_already_warned(ctx, ms, registry, key, 0);
        if (rc < 0)
            goto cleanup;
        if (rc == 1)
            goto return_none;
        /* Else this warning hasn't been generated before. */
    }

    action = qp_warnings_get_filter(ctx, ms, cat, text, lineno, module, filename, &item);
    if (JS_IsException(action)) {
        action = JS_UNDEFINED;
        goto cleanup;
    }

    if (qp_warnings_action_is(ctx, action, "error")) {
        qp_warnings_raise(ctx, cat, message);
        goto cleanup;
    }
    if (qp_warnings_action_is(ctx, action, "ignore"))
        goto return_none;

    /* Store in the registry that we've been here, *except* when the action
       is "always" or "all". */
    rc = 0;
    if (!qp_warnings_action_is(ctx, action, "always") &&
        !qp_warnings_action_is(ctx, action, "all")) {
        if (has_registry && qp_dict_set_item(ctx, registry, key, JS_TRUE) < 0)
            goto cleanup;

        if (qp_warnings_action_is(ctx, action, "once")) {
            JSValueConst reg = registry;
            if (!has_registry) {
                reg = qp_warnings_get_once_registry(ctx, ms);
                if (JS_IsException(reg))
                    goto cleanup;
            }
            /* WarningsState.once_registry[(text, category)] = 1 */
            reg = JS_DupValue(ctx, reg);
            rc = qp_warnings_update_registry(ctx, ms, reg, text, cat, 0);
            JS_FreeValue(ctx, (JSValue)reg);
        } else if (qp_warnings_action_is(ctx, action, "module")) {
            /* registry[(text, category, 0)] = 1 */
            if (has_registry)
                rc = qp_warnings_update_registry(ctx, ms, registry, text, cat, 0);
        } else if (!qp_warnings_action_is(ctx, action, "default")) {
            qp_raise_runtime_error(ctx, "Unrecognized action (%R) in warnings.filters:\n %R",
                                   action, item);
            goto cleanup;
        }
    }

    if (rc == 1)  /* Already warned for this module. */
        goto return_none;
    if (rc < 0)
        goto cleanup;
    if (qp_warnings_call_show_warning(ctx, cat, text, message, filename, lineno, lineno_obj,
                                      sourceline, source, module) < 0)
        goto cleanup;

 return_none:
    ret = 0;
 cleanup:
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, action);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, text);
    JS_FreeValue(ctx, message);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* frames                                                                  */

static JSFunctionBytecode *qp_warnings_frame_bytecode(struct JSStackFrame *sf)
{
    JSObject *p;
    if (!sf || JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION || !p->u.func.function_bytecode->is_py)
        return NULL;
    return p->u.func.function_bytecode;
}

/* PyFrame_GetBack: next Python frame further out, NULL if none. */
static struct JSStackFrame *qp_warnings_frame_back(struct JSStackFrame *sf)
{
    for (sf = sf->prev_frame; sf; sf = sf->prev_frame) {
        if (qp_warnings_frame_bytecode(sf))
            return sf;
    }
    return NULL;
}

/* co_filename (new ref). */
static JSValue qp_warnings_frame_filename(JSContext *ctx, struct JSStackFrame *sf)
{
    JSFunctionBytecode *b = qp_warnings_frame_bytecode(sf);
    return qp_str_from_atom(ctx, b->has_debug ? b->debug.filename : JS_ATOM_empty_string);
}

static int qp_warnings_frame_lineno(struct JSStackFrame *sf)
{
    JSFunctionBytecode *b = qp_warnings_frame_bytecode(sf);
    QPLoc loc;
    if (sf->cur_pc &&
        qp_code_addr2loc(b, (uint32_t)(sf->cur_pc - b->byte_code_buf - 1), &loc) == 0 &&
        loc.lineno >= 0)
        return loc.lineno;
    if (b->py_code)
        return b->py_code->firstlineno;
    return 0;
}

/* PyUnicode_Contains for ASCII needles; errors -> false. */
static BOOL qp_warnings_str_contains(JSContext *ctx, const char *s, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= len; i++)
        if (memcmp(s + i, needle, n) == 0)
            return TRUE;
    return FALSE;
}

static BOOL qp_warnings_is_internal_filename(JSContext *ctx, JSValueConst filename)
{
    const char *s;
    size_t len;
    BOOL r;
    if (!qp_str_check(ctx, filename))
        return FALSE;
    s = qp_str_utf8(ctx, filename, &len);
    if (!s) {
        qp_warnings_clear_error(ctx);
        return FALSE;
    }
    r = qp_warnings_str_contains(ctx, s, len, "importlib") &&
        qp_warnings_str_contains(ctx, s, len, "_bootstrap");
    JS_FreeCString(ctx, s);
    return r;
}

static BOOL qp_warnings_is_filename_to_skip(JSContext *ctx, JSValueConst filename,
                                            JSValueConst skip_file_prefixes)
{
    const char *s;
    size_t len;
    Py_ssize_t idx;
    BOOL r = FALSE;
    if (QP_IS_NULL(skip_file_prefixes) || !qp_str_check(ctx, filename))
        return FALSE;
    s = qp_str_utf8(ctx, filename, &len);
    if (!s) {
        qp_warnings_clear_error(ctx);
        return FALSE;
    }
    /* UTF-8 preserves code point prefixes, so a byte prefix test is
       PyUnicode_Tailmatch(filename, prefix, 0, MAX, -1). */
    for (idx = 0; idx < QP_TUPLE_SIZE(skip_file_prefixes); idx++) {
        size_t plen;
        const char *p = qp_str_utf8(ctx, QP_TUPLE_ITEMS(skip_file_prefixes)[idx], &plen);
        if (!p) {
            qp_warnings_clear_error(ctx);
            break;
        }
        r = plen <= len && memcmp(s, p, plen) == 0;
        JS_FreeCString(ctx, p);
        if (r)
            break;
    }
    JS_FreeCString(ctx, s);
    return r;
}

static BOOL qp_warnings_is_internal_frame(JSContext *ctx, struct JSStackFrame *sf)
{
    JSValue filename;
    BOOL r;
    if (!sf)
        return FALSE;
    filename = qp_warnings_frame_filename(ctx, sf);
    if (JS_IsException(filename)) {
        qp_warnings_clear_error(ctx);
        return FALSE;
    }
    r = qp_warnings_is_internal_filename(ctx, filename);
    JS_FreeValue(ctx, filename);
    return r;
}

static struct JSStackFrame *qp_warnings_next_external_frame(JSContext *ctx,
                                                            struct JSStackFrame *sf,
                                                            JSValueConst skip_file_prefixes)
{
    for (;;) {
        JSValue fn;
        BOOL skip;
        sf = qp_warnings_frame_back(sf);
        if (!sf)
            return NULL;
        fn = qp_warnings_frame_filename(ctx, sf);
        if (JS_IsException(fn)) {
            qp_warnings_clear_error(ctx);
            return sf;
        }
        skip = qp_warnings_is_internal_filename(ctx, fn) ||
               qp_warnings_is_filename_to_skip(ctx, fn, skip_file_prefixes);
        JS_FreeValue(ctx, fn);
        if (!skip)
            return sf;
    }
}

/* setup_context: 0 ok (new refs in *filename, *module, *registry), -1 error.
   skip_file_prefixes: QP_NULL or a tuple. */
static int qp_warnings_setup_context(JSContext *ctx, Py_ssize_t stack_level,
                                     JSValueConst skip_file_prefixes, JSValue *filename,
                                     int *lineno, JSValue *module, JSValue *registry)
{
    struct JSStackFrame *f;
    JSValueConst globals;
    JSAtom ra;
    int rc;

    *filename = *module = *registry = QP_NULL;
    if (!QP_IS_NULL(skip_file_prefixes)) {
        /* Type check our data structure up front. Later code that uses it
         * isn't structured to report errors. */
        Py_ssize_t idx;
        for (idx = 0; idx < QP_TUPLE_SIZE(skip_file_prefixes); idx++) {
            JSValueConst prefix = QP_TUPLE_ITEMS(skip_file_prefixes)[idx];
            if (!qp_str_check(ctx, prefix)) {
                JSValue name = qp_warnings_tp_name(ctx, prefix);
                if (!JS_IsException(name)) {
                    qp_raise_type_error(ctx, "Found non-str '%U' in skip_file_prefixes.", name);
                    JS_FreeValue(ctx, name);
                }
                return -1;
            }
        }
    }
    f = qp_py_frame_at(ctx, 0);
    // Stack level comparisons to Python code is off by one as there is no
    // warnings-related stack level to avoid.
    if (stack_level <= 0 || qp_warnings_is_internal_frame(ctx, f)) {
        while (--stack_level > 0 && f != NULL)
            f = qp_warnings_frame_back(f);
    } else {
        while (--stack_level > 0 && f != NULL)
            f = qp_warnings_next_external_frame(ctx, f, skip_file_prefixes);
    }

    globals = f ? qp_frame_globals(f) : QP_NULL;
    if (f == NULL || QP_IS_NULL(globals)) {
        globals = QP_STATE(ctx)->sys_dict;
        *filename = qp_str_from_cstr(ctx, "<sys>");
        *lineno = 0;
    } else {
        *filename = qp_warnings_frame_filename(ctx, f);
        *lineno = qp_warnings_frame_lineno(f);
    }
    if (JS_IsException(*filename)) {
        *filename = QP_NULL;
        return -1;
    }

    /* Setup registry. */
    ra = JS_NewAtom(ctx, "__warningregistry__");
    if (ra == JS_ATOM_NULL)
        goto handle_error;
    rc = qp_dict_get_item_atom(ctx, globals, ra, registry);
    if (rc == 0) {
        *registry = qp_dict_new(ctx);
        if (JS_IsException(*registry)) {
            *registry = QP_NULL;
            rc = -1;
        } else {
            rc = qp_dict_set_item_atom(ctx, globals, ra, *registry);
        }
    }
    JS_FreeAtom(ctx, ra);
    if (rc < 0)
        goto handle_error;

    /* Setup module. */
    rc = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __name__), module);
    if (rc < 0)
        goto handle_error;
    if (rc > 0) {
        if (JS_IsUndefined(*module) || JS_IsNull(*module) || qp_str_check(ctx, *module))
            return 0;
        JS_FreeValue(ctx, *module);
    }
    *module = qp_str_from_cstr(ctx, "<string>");
    if (JS_IsException(*module)) {
        *module = QP_NULL;
        goto handle_error;
    }
    return 0;

 handle_error:
    JS_FreeValue(ctx, *registry);
    JS_FreeValue(ctx, *module);
    JS_FreeValue(ctx, *filename);
    *filename = *module = *registry = QP_NULL;
    return -1;
}

/* get_category: borrowed type value or JS_EXCEPTION. */
static JSValueConst qp_warnings_get_category(JSContext *ctx, JSValueConst message,
                                        JSValueConst category)
{
    QPType *t;
    if (qp_typecheck(ctx, message, QP_EXC(ctx, Warning)))
        /* Ignore the category argument. */
        return QP_TYPE_VALUE(qp_type_of(ctx, message));
    if (QP_IS_NULL(category) || JS_IsUndefined(category) || JS_IsNull(category))
        return QP_TYPE_VALUE(QP_EXC(ctx, UserWarning));

    /* Validate category. */
    t = qp_type_from_value(category);
    if (!t)
        return qp_raise_type_error(ctx, "category must be a Warning subclass, not '%T'",
                                   category);
    if (!qp_type_is_subtype(t, QP_EXC(ctx, Warning)))
        return qp_raise_type_error(ctx, "category must be a Warning subclass, not class '%N'",
                                   t);
    return category;
}

static int qp_warnings_do_warn(JSContext *ctx, JSValueConst message, JSValueConst category,
                               Py_ssize_t stack_level, JSValueConst source,
                               JSValueConst skip_file_prefixes)
{
    JSValue filename, module, registry;
    QPModState *ms;
    int lineno, res;

    ms = qp_warnings_state(ctx);
    if (!ms)
        return -1;
    if (qp_warnings_setup_context(ctx, stack_level, skip_file_prefixes, &filename, &lineno,
                                  &module, &registry) < 0)
        return -1;
    qp_warnings_lock(ctx, ms);
    res = qp_warnings_do_warn_explicit(ctx, ms, category, message, filename, lineno, module,
                                       registry, QP_NULL, source);
    qp_warnings_unlock(ctx, ms);
    JS_FreeValue(ctx, filename);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, module);
    return res;
}

/* get_source_line: new ref, or QP_NULL (exception pending or not). */
static JSValue qp_warnings_get_source_line(JSContext *ctx, JSValueConst module_globals,
                                           int lineno)
{
    JSValue importlib = QP_STATE(ctx)->importlib, external, loader, spec, module_name;
    JSValue get_source, source, source_list, line;
    Py_ssize_t n;
    JSValue *items;
    int rc;

    /* stolen from import.c (_PyImport_BlessMyLoader) */
    if (QP_IS_NULL(importlib) || JS_IsUndefined(importlib))
        return QP_NULL;
    external = qp_warnings_getattr(ctx, importlib, "_bootstrap_external");
    if (JS_IsException(external))
        return QP_NULL;
    loader = qp_warnings_call_method1(ctx, external, "_bless_my_loader", module_globals);
    JS_FreeValue(ctx, external);
    if (JS_IsException(loader))
        return QP_NULL;

    /* Prefer __spec__.name: __name__ is "__main__" for the module executed
       as a script, but the loader can only handle its own module name. */
    rc = qp_dict_get_item_atom(ctx, module_globals, QP_ATOM(ctx, __spec__), &spec);
    if (rc < 0) {
        JS_FreeValue(ctx, loader);
        return QP_NULL;
    }
    module_name = QP_NULL;
    if (rc > 0) {
        rc = qp_getattr_opt(ctx, spec, QP_ATOM(ctx, name), &module_name);
        JS_FreeValue(ctx, spec);
        if (rc < 0) {
            JS_FreeValue(ctx, loader);
            return QP_NULL;
        }
        if (JS_IsUndefined(module_name) || JS_IsNull(module_name)) {
            JS_FreeValue(ctx, module_name);
            module_name = QP_NULL;
        }
    }
    if (QP_IS_NULL(module_name)) {
        rc = qp_dict_get_item_atom(ctx, module_globals, QP_ATOM(ctx, __name__), &module_name);
        if (rc <= 0) {  // not found or error
            JS_FreeValue(ctx, loader);
            return QP_NULL;
        }
    }

    /* Make sure the loader implements the optional get_source() method. */
    (void)qp_warnings_getattr_opt(ctx, loader, "get_source", &get_source);
    JS_FreeValue(ctx, loader);
    if (QP_IS_NULL(get_source)) {
        JS_FreeValue(ctx, module_name);
        return QP_NULL;
    }
    /* Call get_source() to get the source code. */
    source = qp_call1(ctx, get_source, module_name);
    JS_FreeValue(ctx, get_source);
    JS_FreeValue(ctx, module_name);
    if (JS_IsException(source)) {
        /* The source line is optional: the loader can be unable to provide
           the source of the module, for example if it is not its loader. */
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, ImportError)))
            qp_warnings_clear_error(ctx);
        return QP_NULL;
    }
    if (JS_IsUndefined(source) || JS_IsNull(source))
        return QP_NULL;

    /* Split the source into lines. */
    if (!qp_str_check(ctx, source)) {
        qp_raise_type_error(ctx, "must be str, not %T", source);
        JS_FreeValue(ctx, source);
        return QP_NULL;
    }
    source_list = qp_warnings_call_method1(ctx, source, "splitlines", QP_NULL);
    JS_FreeValue(ctx, source);
    if (JS_IsException(source_list))
        return QP_NULL;
    if (!qp_list_check(ctx, source_list)) {
        JS_FreeValue(ctx, source_list);
        return QP_NULL;
    }

    /* Get the source line. */
    items = qp_list_items(source_list, &n);
    if (lineno < 1 || lineno > n) {
        JS_FreeValue(ctx, source_list);
        return QP_NULL;
    }
    line = JS_DupValue(ctx, items[lineno - 1]);
    JS_FreeValue(ctx, source_list);
    return line;
}

/* ---------------------------------------------------------------------- */
/* exported C API                                                          */

/* PyErr_WarnEx / warn_unicode: category NULL = RuntimeWarning. */
static int qp_warnings_warn(JSContext *ctx, QPType *category, JSValueConst message,
                            Py_ssize_t stack_level, JSValueConst source)
{
    if (!category)
        category = QP_EXC(ctx, RuntimeWarning);
    return qp_warnings_do_warn(ctx, message, QP_TYPE_VALUE(category), stack_level, source,
                               QP_NULL);
}

/* PyErr_WarnExplicitObject (+ source): category NULL = RuntimeWarning. */
static int qp_warnings_warn_explicit(JSContext *ctx, QPType *category, JSValueConst message,
                                     JSValueConst filename, int lineno, JSValueConst module,
                                     JSValueConst registry, JSValueConst source)
{
    QPModState *ms = qp_warnings_state(ctx);
    int res;
    if (!ms)
        return -1;
    if (!category)
        category = QP_EXC(ctx, RuntimeWarning);
    qp_warnings_lock(ctx, ms);
    res = qp_warnings_do_warn_explicit(ctx, ms, QP_TYPE_VALUE(category), message, filename,
                                       lineno, module, registry, QP_NULL, source);
    qp_warnings_unlock(ctx, ms);
    return res;
}

/* ---------------------------------------------------------------------- */
/* module functions                                                        */

static JSValue qp_warnings_m_warn(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "message", "category", "stacklevel", "source",
                                      "skip_file_prefixes", NULL };
    static const QPArgSpec spec = { "warn", kw, 1, 4, 0 };
    JSValueConst a[5];
    JSValueConst category, source, skip = QP_NULL;
    Py_ssize_t stacklevel = 1;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2]) && qp_arg_ssize(ctx, a[2], &stacklevel) < 0)
        return JS_EXCEPTION;
    source = QP_IS_NULL(a[3]) ? JS_UNDEFINED : a[3];
    if (!QP_IS_NULL(a[4])) {
        if (!qp_tuple_check(ctx, a[4])) {
            JSValue name = JS_IsUndefined(a[4]) ? qp_str_from_cstr(ctx, "None")
                                                : qp_warnings_tp_name(ctx, a[4]);
            if (JS_IsException(name))
                return name;
            qp_raise_type_error(ctx, "warn() argument 'skip_file_prefixes' must be tuple, not %.50U",
                                name);
            JS_FreeValue(ctx, name);
            return JS_EXCEPTION;
        }
        skip = a[4];
    }
    category = qp_warnings_get_category(ctx, a[0], QP_IS_NULL(a[1]) ? JS_UNDEFINED : a[1]);
    if (JS_IsException(category))
        return JS_EXCEPTION;
    if (!QP_IS_NULL(skip)) {
        if (QP_TUPLE_SIZE(skip) > 0) {
            if (stacklevel < 2)
                stacklevel = 2;
        } else {
            skip = QP_NULL;
        }
    }
    if (qp_warnings_do_warn(ctx, a[0], category, stacklevel, source, skip) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_warnings_m_warn_explicit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                           int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "message", "category", "filename", "lineno", "module",
                                      "registry", "module_globals", "source", NULL };
    static const QPArgSpec spec = { "warn_explicit", kw, 4, 8, 0 };
    JSValueConst a[8];
    JSValueConst registry, module_globals, sourceobj;
    JSValue source_line = QP_NULL;
    QPModState *ms;
    int lineno, res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[2])) {
        JSValue name = qp_warnings_tp_name(ctx, a[2]);
        if (JS_IsException(name))
            return name;
        if (JS_IsUndefined(a[2]))
            qp_raise_type_error(ctx, "warn_explicit() argument 'filename' must be str, not None");
        else
            qp_raise_type_error(ctx, "warn_explicit() argument 'filename' must be str, not %.50U",
                                name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    if (qp_arg_int(ctx, a[3], &lineno) < 0)
        return JS_EXCEPTION;
    registry = QP_IS_NULL(a[5]) ? JS_UNDEFINED : a[5];
    module_globals = QP_IS_NULL(a[6]) ? JS_UNDEFINED : a[6];
    sourceobj = QP_IS_NULL(a[7]) ? JS_UNDEFINED : a[7];

    ms = qp_warnings_state(ctx);
    if (!ms)
        return JS_EXCEPTION;

    if (!JS_IsUndefined(module_globals) && !JS_IsNull(module_globals)) {
        if (!qp_dict_check(ctx, module_globals) &&
            !qp_typecheck(ctx, module_globals, QP_TYPE(ctx, frozendict)))
            return qp_raise_type_error(ctx, "module_globals must be a dict or a frozendict, "
                                       "not %T", module_globals);
        source_line = qp_warnings_get_source_line(ctx, module_globals, lineno);
        if (QP_IS_NULL(source_line) && JS_HasException(ctx))
            return JS_EXCEPTION;
    }

    qp_warnings_lock(ctx, ms);
    res = qp_warnings_do_warn_explicit(ctx, ms, a[1], a[0], a[2], lineno, a[4], registry,
                                       source_line, sourceobj);
    qp_warnings_unlock(ctx, ms);
    JS_FreeValue(ctx, source_line);
    if (res < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_warnings_m_filters_mutated_lock_held(JSContext *ctx, JSValueConst self,
                                                       JSValueConst *args, int nargs,
                                                       JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_warnings_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    // Note that the lock must be held by the caller.
    if (ms->warn_lock_count <= 0)
        return qp_raise_runtime_error(ctx, "warnings lock is not held");
    ms->warn_filters_version++;
    return JS_UNDEFINED;
}

static JSValue qp_warnings_m_acquire_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_warnings_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    qp_warnings_lock(ctx, ms);
    return JS_UNDEFINED;
}

static JSValue qp_warnings_m_release_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_warnings_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (qp_warnings_unlock(ctx, ms) < 0)
        return qp_raise_runtime_error(ctx, "cannot release un-acquired lock");
    return JS_UNDEFINED;
}

static const QPMethodDef qp_warnings_methods[] = {
    { "warn", qp_warnings_m_warn, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "warn($module, /, message, category=None, stacklevel=1, source=None, *,\n"
      "     skip_file_prefixes=())\n"
      "--\n"
      "\n"
      "Issue a warning, or maybe ignore it or raise an exception.\n"
      "\n"
      "  message\n"
      "    Text of the warning message.\n"
      "  category\n"
      "    The Warning category subclass. Defaults to UserWarning.\n"
      "  stacklevel\n"
      "    How far up the call stack to make this warning appear. A value of 2 for\n"
      "    example attributes the warning to the caller of the code calling warn().\n"
      "  source\n"
      "    If supplied, the destroyed object which emitted a ResourceWarning\n"
      "  skip_file_prefixes\n"
      "    An optional tuple of module filename prefixes indicating frames to skip\n"
      "    during stacklevel computations for stack frame attribution." },
    { "warn_explicit", qp_warnings_m_warn_explicit, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "warn_explicit($module, /, message, category, filename, lineno,\n"
      "              module=<unrepresentable>, registry=None,\n"
      "              module_globals=None, source=None)\n"
      "--\n"
      "\n"
      "Issue a warning, or maybe ignore it or raise an exception." },
    { "_filters_mutated_lock_held", qp_warnings_m_filters_mutated_lock_held, QP_METH_NOARGS, 0,
      "_filters_mutated_lock_held($module, /)\n--\n\n" },
    { "_acquire_lock", qp_warnings_m_acquire_lock, QP_METH_NOARGS, 0,
      "_acquire_lock($module, /)\n--\n\n" },
    { "_release_lock", qp_warnings_m_release_lock, QP_METH_NOARGS, 0,
      "_release_lock($module, /)\n--\n\n" },
    { NULL, NULL, 0, 0, NULL },
};

static int qp_warnings_set_module_attr(JSContext *ctx, JSValueConst module, const char *name,
                                       JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;
    if (a == JS_ATOM_NULL)
        return -1;
    r = qp_setattr(ctx, module, a, v);
    JS_FreeAtom(ctx, a);
    return r;
}

static int qp_warnings_exec(JSContext *ctx, JSValueConst module)
{
    QPModState *ms = qp_warnings_state(ctx);
    if (!ms)
        return -1;
    if (qp_warnings_set_module_attr(ctx, module, "filters", ms->warn_filters) < 0 ||
        qp_warnings_set_module_attr(ctx, module, "_onceregistry", ms->warn_once_registry) < 0 ||
        qp_warnings_set_module_attr(ctx, module, "_defaultaction", ms->warn_default_action) < 0 ||
        qp_warnings_set_module_attr(ctx, module, "_warnings_context", ms->warn_context) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef__warnings = {
    "_warnings",
    "_warnings provides basic warning filtering support.\n"
    "It is a helper module to speed up interpreter start-up.",
    qp_warnings_methods,
    qp_warnings_exec,
};

/*
 * _signal module: port of CPython Modules/signalmodule.c (POSIX part,
 * PSF-2.0).
 *
 * Python-level handlers live in QPModState.sig_handlers[] (CPython
 * Handlers[i].func); SIG_DFL / SIG_IGN are the int objects 0 / 1 stored in
 * ms->sig_dfl / ms->sig_ign (CPython's default_handler / ignore_handler).
 * The C handler only sets sig_atomic_t flags, writes the wakeup fd byte and
 * asks the interpreter of the main context for a prompt poll
 * (interrupt_counter = 1); the poll calls qp_signal_check(), which runs the
 * Python handlers like _PyErr_CheckSignalsTstate and then the queued
 * weakref callbacks.
 *
 * Threads: the Python handlers run in the main thread only (the thread
 * that ran Python code when the first thread was started, see
 * qp_thread_is_main_rt()); signal.signal() and set_wakeup_fd() raise
 * CPython's "only works in main thread of the main interpreter"
 * ValueError in other threads.  A signal arriving while another thread
 * holds the GIL makes that thread poll; the main thread runs the handlers
 * once it gets the GIL back (qp_gil_acquire() makes it poll at once).
 * sys.audit hooks are not available to native modules, so the
 * "signal.pthread_kill" audit event is not raised.
 */
#include "modstate.h"
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__linux__) && defined(__NR_pidfd_send_signal) && \
    !(defined(__ANDROID__) && __ANDROID_API__ < 31)
#define QP_SIGNAL_HAVE_PIDFD 1
#endif
/* sigwaitinfo / sigtimedwait: not provided by macOS */
#if !defined(__APPLE__)
#define QP_SIGNAL_HAVE_SIGWAITINFO 1
#endif

static void qp_weakref_run_pending(JSContext *ctx);
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);

/* async-signal-safe state (CPython Handlers[].tripped, is_tripped, wakeup) */
static volatile sig_atomic_t qp_signal_tripped[QP_NSIG];
static volatile sig_atomic_t qp_signal_is_tripped;
static volatile sig_atomic_t qp_signal_wakeup_fd = -1;
static volatile sig_atomic_t qp_signal_wakeup_warn = 1;
/* errno of a failed wakeup fd write, reported at the next poll (CPython
   report_wakeup_write_error pending call); 0 = none */
static volatile sig_atomic_t qp_signal_wakeup_errno;
static JSContext *volatile qp_signal_main_ctx;
#ifdef QP_SIGNAL_HAVE_SIGWAITINFO
#endif

static void qp_signal_request_poll(void)
{
    JSContext *ctx = qp_signal_main_ctx;
    if (ctx)
        ctx->interrupt_counter = 1;
}

/* trip_signal() */
static void qp_signal_trip_c(int sig_num)
{
    int fd;
    qp_signal_tripped[sig_num] = 1;
    qp_signal_is_tripped = 1;
    qp_signal_request_poll();
    fd = qp_signal_wakeup_fd;
    if (fd != -1) {
        unsigned char byte = (unsigned char)sig_num;
        ssize_t rc;
        do {
            rc = write(fd, &byte, 1);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            if (qp_signal_wakeup_warn ||
                (errno != EWOULDBLOCK && errno != EAGAIN)) {
                qp_signal_wakeup_errno = errno;
                qp_signal_is_tripped = 1;
            }
        }
    }
}

/* signal_handler() */
static void qp_signal_handler(int sig_num)
{
    int save_errno = errno;
    qp_signal_trip_c(sig_num);
    errno = save_errno;
}

/* PyOS_setsig */
static void (*qp_signal_setsig(int sig, void (*handler)(int)))(int)
{
    struct sigaction context, ocontext;
    context.sa_handler = handler;
    sigemptyset(&context.sa_mask);
    context.sa_flags = SA_ONSTACK;
    if (sigaction(sig, &context, &ocontext) == -1)
        return SIG_ERR;
    return ocontext.sa_handler;
}

/* PyOS_getsig */
static void (*qp_signal_getsig(int sig))(int)
{
    struct sigaction context;
    if (sigaction(sig, NULL, &context) == -1)
        return SIG_ERR;
    return context.sa_handler;
}

/* compare_handler(): func is an exact int equal to the sentinel */
static BOOL qp_signal_compare_handler(JSValueConst func, JSValueConst dfl_ign)
{
    int64_t a, b;
    int ov;
    if (QP_IS_NULL(func) || QP_IS_NULL(dfl_ign) || !qp_long_check_exact(func))
        return FALSE;
    if (qp_long_as_int64_overflow(func, &a, &ov) < 0 || ov)
        return FALSE;
    if (qp_long_as_int64_overflow(dfl_ign, &b, &ov) < 0 || ov)
        return FALSE;
    return a == b;
}

static BOOL qp_signal_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

static void qp_signal_set_handler(JSContext *ctx, QPModState *ms, int signum, JSValue func)
{
    JSValue old = ms->sig_handlers[signum];
    ms->sig_handlers[signum] = func;
    if (!QP_IS_NULL(old))
        JS_FreeValue(ctx, old);
}

/* report_wakeup_write_error() */
static void qp_signal_report_wakeup_error(JSContext *ctx, int err)
{
    JSValue exc;
    qp_raise_os_error(ctx, err, QP_NULL);
    exc = qp_exc_fetch(ctx);
    qp_exc_write_unraisable_msg(ctx, exc, "Exception ignored while "
                                "trying to write to the signal wakeup fd", QP_NULL);
    JS_FreeValue(ctx, exc);
}

/* _PyErr_CheckSignalsTstate */
static int qp_signal_check_signals(JSContext *ctx)
{
    QPModState *ms;
    int i, err;

    if (!qp_signal_is_tripped)
        return 0;
    /* _Py_ThreadCanHandleSignals: only the main thread runs the handlers */
    if (!qp_thread_is_main_rt(ctx->rt))
        return 0;
    qp_signal_is_tripped = 0;
    err = qp_signal_wakeup_errno;
    if (err) {
        qp_signal_wakeup_errno = 0;
        qp_signal_report_wakeup_error(ctx, err);
    }
    ms = qp_modstate(ctx);
    if (!ms) {
        qp_signal_is_tripped = 1;
        return -1;
    }
    for (i = 1; i < QP_NSIG; i++) {
        JSValue func, frame, args[2], result;
        if (!qp_signal_tripped[i])
            continue;
        qp_signal_tripped[i] = 0;
        func = ms->sig_handlers[i];
        if (QP_IS_NULL(func) || JS_IsUndefined(func) ||
            qp_signal_compare_handler(func, ms->sig_ign) ||
            qp_signal_compare_handler(func, ms->sig_dfl)) {
            JSValue exc;
            qp_raise(ctx, QP_EXC(ctx, OSError),
                     "Signal %d ignored due to race condition", i);
            exc = qp_exc_fetch(ctx);
            qp_exc_write_unraisable_msg(ctx, exc, "Exception ignored while "
                                        "calling signal handler", QP_NULL);
            JS_FreeValue(ctx, exc);
            continue;
        }
        if (qp_py_frame_at(ctx, 0) == NULL) {
            frame = JS_UNDEFINED;
        } else {
            frame = qp_getframe(ctx, 0);
            if (JS_IsException(frame)) {
                qp_signal_is_tripped = 1;
                return -1;
            }
        }
        func = JS_DupValue(ctx, func);
        args[0] = JS_NewInt32(ctx, i);
        args[1] = frame;
        result = qp_vectorcall(ctx, func, (JSValueConst *)args, 2, QP_NULL);
        JS_FreeValue(ctx, func);
        JS_FreeValue(ctx, frame);
        if (JS_IsException(result)) {
            /* On error, re-schedule a call to the check */
            qp_signal_is_tripped = 1;
            return -1;
        }
        JS_FreeValue(ctx, result);
    }
    return 0;
}

/* _Py_HandlePending: Python signal handlers, then __del__ of the objects
   freed since the last poll and weakref callbacks */
static int qp_signal_check(JSContext *ctx)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;

    if (ms && !QP_IS_NULL(ms->async_exc)) {
        /* _PY_ASYNC_EXCEPTION_BIT: raise the pending asynchronous exception */
        JSValue exc = ms->async_exc;
        ms->async_exc = QP_NULL;
        qp_raise_value(ctx, qp_type_from_value(exc), JS_UNDEFINED);
        JS_FreeValue(ctx, exc);
        return -1;
    }
    if (qp_signal_is_tripped && ctx == qp_signal_main_ctx &&
        qp_thread_is_main_rt(ctx->rt)) {
        if (qp_signal_check_signals(ctx) < 0)
            return -1;
    }
#ifdef CONFIG_PYTHON_CAPI
    /* PyThreadState_SetAsyncExc (python/capi_stable.c) */
    if (qp_capi_async_exc_check(ctx) < 0)
        return -1;
#endif
    /* objects freed since the last poll: __del__, then weakref callbacks */
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    return 0;
}

/* _PySignal_AfterFork: signals that arrived just before os.fork() are
   handled by the parent only */
static void qp_signal_after_fork_child(void)
{
    int i;
    if (!qp_signal_is_tripped)
        return;
    for (i = 1; i < QP_NSIG; i++)
        qp_signal_tripped[i] = 0;
    qp_signal_is_tripped = 0;
}

/* PyErr_SetInterruptEx (+ the ValueError of _thread.interrupt_main) */
static int qp_signal_trip(JSContext *ctx, int signum)
{
    QPModState *ms;
    JSValue func;
    if (signum < 1 || signum >= QP_NSIG) {
        qp_raise_value_error(ctx, "signal number out of range");
        return -1;
    }
    ms = qp_modstate(ctx);
    if (!ms)
        return -1;
    func = ms->sig_handlers[signum];
    if (!qp_signal_compare_handler(func, ms->sig_ign) &&
        !qp_signal_compare_handler(func, ms->sig_dfl)) {
        if (!qp_signal_main_ctx)
            qp_signal_main_ctx = ctx;
        qp_signal_trip_c(signum);
    }
    return 0;
}

/* qjs --drepl FILE --python: make ctx (the session) the context that
   handles the signals and give SIGINT the C handler that its Python-level
   handler needs (the line editor of qjs owns SIGINT between the inputs).
   Returns the previous C handler. */
static void (*qp_signal_attach(JSContext *ctx))(int)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    void (*handler)(int) = SIG_DFL;
    JSValueConst h;

    qp_signal_main_ctx = ctx;
    if (ms && !QP_IS_NULL(h = ms->sig_handlers[SIGINT])) {
        if (qp_signal_compare_handler(h, ms->sig_ign))
            handler = SIG_IGN;
        else if (!qp_signal_compare_handler(h, ms->sig_dfl) && qp_signal_callable(ctx, h))
            handler = qp_signal_handler;
    }
    return qp_signal_setsig(SIGINT, handler);
}

/* ctx goes away (qp_state_free): the signal handlers no longer poll it */
static void qp_signal_forget_ctx(JSContext *ctx)
{
    if (qp_signal_main_ctx == ctx)
        qp_signal_main_ctx = NULL;
}

/* qpy started from its base snapshot (pythonrun.c): the signal state was
   recorded in a process where no signal was ignored, so the signals this
   process inherited ignored ('ignored[signum]') are recorded ignored and
   stay ignored, SIGINT included, as signal_get_set_handlers() and the
   SIGINT installation of a normal start do.  The C handler of SIGINT
   (qp_snap_state_apply installed it) is then SIG_IGN again. */
static void qp_signal_apply_inherited(JSContext *ctx, const uint8_t *ignored, int nsig)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    int signum;

    if (!ms || QP_IS_NULL(ms->sig_ign))
        return;
    for (signum = 1; signum < QP_NSIG && signum < nsig; signum++) {
        if (!ignored[signum] || qp_signal_compare_handler(ms->sig_handlers[signum], ms->sig_ign))
            continue;
        qp_signal_set_handler(ctx, ms, signum, JS_DupValue(ctx, ms->sig_ign));
        qp_signal_setsig(signum, SIG_IGN);
    }
}

/* ---- argument helpers ---- */

static int qp_signal_int_args(JSContext *ctx, const char *fname, JSValueConst *args,
                              int nargs, int n, int *out)
{
    int i;
    if (qp_arg_check_positional(ctx, fname, nargs, n, n) < 0)
        return -1;
    for (i = 0; i < n; i++)
        if (qp_arg_int(ctx, args[i], &out[i]) < 0)
            return -1;
    return 0;
}

static JSValue qp_signal_none_or_exc(JSContext *ctx, int r)
{
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* _PyTime_FromSecondsObject(ROUND_CEILING) in nanoseconds */
static int qp_signal_seconds_ns(JSContext *ctx, JSValueConst obj, int64_t *out)
{
    if (qp_float_check(ctx, obj)) {
        double d;
        if (qp_float_as_double(ctx, obj, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
        d = ceil(d * 1e9);
        if (!(d >= (double)INT64_MIN && d < -(double)INT64_MIN)) {
            qp_raise_overflow_error(ctx, "timestamp too large to convert to C PyTime_t");
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    } else {
        JSValue v = qp_index(ctx, obj);
        int64_t s;
        int ov;
        if (JS_IsException(v))
            return -1;
        if (qp_long_as_int64_overflow(v, &s, &ov) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        if (ov || s > INT64_MAX / 1000000000 || s < INT64_MIN / 1000000000) {
            qp_raise_overflow_error(ctx, "timestamp too large to convert to C PyTime_t");
            return -1;
        }
        *out = s * 1000000000;
        return 0;
    }
}

/* _Py_Sigset_Converter */
static int qp_signal_sigset_conv(JSContext *ctx, JSValueConst obj, sigset_t *mask)
{
    JSValue it, item, v;
    int64_t signum;
    int ov;

    if (sigemptyset(mask) < 0) {
        qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    it = qp_iter(ctx, obj);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        item = qp_next(ctx, it);
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item))
            goto error;
        v = qp_index(ctx, item);
        JS_FreeValue(ctx, item);
        if (JS_IsException(v))
            goto error;
        ov = 0;
        if (qp_long_as_int64_overflow(v, &signum, &ov) < 0) {
            JS_FreeValue(ctx, v);
            goto error;
        }
        JS_FreeValue(ctx, v);
        if (ov)
            signum = -1;
        if (signum <= 0 || signum >= QP_NSIG) {
            qp_raise_value_error(ctx, "signal number %lld out of range [1; %d]",
                                 (long long)signum, QP_NSIG - 1);
            goto error;
        }
        if (sigaddset(mask, (int)signum)) {
            if (errno != EINVAL) {
                qp_raise_os_error(ctx, errno, QP_NULL);
                goto error;
            }
            if (qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1,
                        "invalid signal number %lld, please use valid_signals()",
                        (long long)signum) < 0)
                goto error;
        }
    }
    JS_FreeValue(ctx, it);
    return 0;
error:
    JS_FreeValue(ctx, it);
    return -1;
}

static JSValue qp_signal_sigset_to_set(JSContext *ctx, sigset_t *mask)
{
    JSValue result = qp_set_new(ctx, QP_NULL);
    int sig;
    if (JS_IsException(result))
        return JS_EXCEPTION;
    for (sig = 1; sig < QP_NSIG; sig++) {
        if (sigismember(mask, sig) != 1)
            continue;
        if (qp_set_add(ctx, result, JS_NewInt32(ctx, sig)) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

/* ---- module functions ---- */

static JSValue qp_signal_default_int_handler(JSContext *ctx, JSValueConst self,
                                             JSValueConst *args, int nargs,
                                             JSValueConst kwnames, int magic)
{
    int signalnum;
    JSValue exc;
    if (qp_arg_check_positional(ctx, "default_int_handler", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &signalnum) < 0)
        return JS_EXCEPTION;
    exc = qp_exc_new(ctx, QP_EXC(ctx, KeyboardInterrupt), 0, NULL);
    if (JS_IsException(exc))
        return JS_EXCEPTION;
    return qp_raise_object(ctx, exc);
}

static JSValue qp_signal_alarm(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    int seconds;
    if (qp_arg_int(ctx, args[0], &seconds) < 0)
        return JS_EXCEPTION;
    /* alarm() returns the number of seconds remaining */
    return qp_long_from_int64(ctx, (int64_t)alarm((unsigned int)seconds));
}

static JSValue qp_signal_pause(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    (void)pause();
    /* make sure that any exceptions that got raised are propagated
       back into Python */
    return qp_signal_none_or_exc(ctx, qp_signal_check_signals(ctx));
}

static JSValue qp_signal_raise_signal(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    int signalnum;
    if (qp_arg_int(ctx, args[0], &signalnum) < 0)
        return JS_EXCEPTION;
    if (raise(signalnum))
        return qp_raise_os_error(ctx, errno, QP_NULL);
    /* the current thread can handle signals: handle the raised one now */
    return qp_signal_none_or_exc(ctx, qp_signal_check_signals(ctx));
}

static JSValue qp_signal_signal(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms;
    int signalnum;
    JSValueConst handler;
    void (*func)(int);
    JSValue old;

    if (qp_arg_check_positional(ctx, "signal", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &signalnum) < 0)
        return JS_EXCEPTION;
    handler = args[1];
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (!qp_thread_is_main_rt(ctx->rt))
        return qp_raise_value_error(ctx, "signal only works in main thread "
                                    "of the main interpreter");
    if (signalnum < 1 || signalnum >= QP_NSIG)
        return qp_raise_value_error(ctx, "signal number out of range");
    if (qp_signal_callable(ctx, handler)) {
        func = qp_signal_handler;
    } else if (qp_signal_compare_handler(handler, ms->sig_ign)) {
        func = SIG_IGN;
    } else if (qp_signal_compare_handler(handler, ms->sig_dfl)) {
        func = SIG_DFL;
    } else {
        return qp_raise_type_error(ctx, "signal handler must be signal.SIG_IGN, "
                                   "signal.SIG_DFL, or a callable object");
    }
    /* Check for pending signals before changing signal handler */
    if (qp_signal_check_signals(ctx) < 0)
        return JS_EXCEPTION;
    if (qp_signal_setsig(signalnum, func) == SIG_ERR)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    old = ms->sig_handlers[signalnum];
    ms->sig_handlers[signalnum] = JS_DupValue(ctx, handler);
    if (!QP_IS_NULL(old))
        return old;
    return JS_UNDEFINED;
}

static JSValue qp_signal_getsignal(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms;
    int signalnum;
    JSValue h;
    if (qp_arg_int(ctx, args[0], &signalnum) < 0)
        return JS_EXCEPTION;
    if (signalnum < 1 || signalnum >= QP_NSIG)
        return qp_raise_value_error(ctx, "signal number out of range");
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    h = ms->sig_handlers[signalnum];
    if (!QP_IS_NULL(h))
        return JS_DupValue(ctx, h);
    return JS_UNDEFINED;
}

static JSValue qp_signal_strsignal(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    int signalnum;
    const char *res;
    if (qp_arg_int(ctx, args[0], &signalnum) < 0)
        return JS_EXCEPTION;
    if (signalnum < 1 || signalnum >= QP_NSIG)
        return qp_raise_value_error(ctx, "signal number out of range");
    errno = 0;
    res = strsignal(signalnum);
    if (errno || res == NULL || strstr(res, "Unknown signal") != NULL)
        return JS_UNDEFINED;
    return qp_str_from_cstr(ctx, res);
}

static JSValue qp_signal_siginterrupt(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    int a[2];
    struct sigaction act;
    if (qp_signal_int_args(ctx, "siginterrupt", args, nargs, 2, a) < 0)
        return JS_EXCEPTION;
    if (a[0] < 1 || a[0] >= QP_NSIG)
        return qp_raise_value_error(ctx, "signal number out of range");
    (void)sigaction(a[0], NULL, &act);
    if (a[1])
        act.sa_flags &= ~SA_RESTART;
    else
        act.sa_flags |= SA_RESTART;
    if (sigaction(a[0], &act, NULL) < 0)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return JS_UNDEFINED;
}

static JSValue qp_signal_set_wakeup_fd(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "", "warn_on_full_buffer", NULL };
    static const QPArgSpec spec = { "set_wakeup_fd", kwlist, 1, 1, 0 };
    JSValueConst a[2];
    int fd, warn = 1, old_fd;
    QPModState *ms;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1])) {
        warn = qp_bool(ctx, a[1]);
        if (warn < 0)
            return JS_EXCEPTION;
    }
    if (qp_arg_int(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    if (!qp_thread_is_main_rt(ctx->rt))
        return qp_raise_value_error(ctx, "set_wakeup_fd only works in main thread "
                                    "of the main interpreter");
    if (fd != -1) {
        struct stat st;
        int flags;
        if (fstat(fd, &st) != 0)
            return qp_raise_os_error(ctx, errno, QP_NULL);
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            return qp_raise_os_error(ctx, errno, QP_NULL);
        if (!(flags & O_NONBLOCK))
            return qp_raise_value_error(ctx, "the fd %d must be in non-blocking mode", fd);
    }
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    old_fd = qp_signal_wakeup_fd;
    qp_signal_wakeup_fd = fd;
    qp_signal_wakeup_warn = warn;
    ms->sig_wakeup_fd = fd;
    ms->sig_wakeup_warn_on_full_buffer = warn;
    return JS_NewInt32(ctx, old_fd);
}

static int qp_signal_timeval(JSContext *ctx, JSValueConst obj, struct timeval *tv)
{
    int64_t ns, us;
    if (QP_IS_NULL(obj)) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
        return 0;
    }
    if (qp_signal_seconds_ns(ctx, obj, &ns) < 0)
        return -1;
    /* _PyTime_AsTimeval(ROUND_CEILING) */
    us = ns / 1000;
    if (ns % 1000 > 0)
        us++;
    tv->tv_sec = (time_t)(us / 1000000);
    us %= 1000000;
    if (us < 0) {
        us += 1000000;
        tv->tv_sec--;
    }
    tv->tv_usec = (suseconds_t)us;
    return 0;
}

static JSValue qp_signal_itimer_retval(JSContext *ctx, struct itimerval *iv)
{
    JSValue items[2];
    items[0] = qp_float_new(ctx, iv->it_value.tv_sec +
                            (double)(iv->it_value.tv_usec / 1000000.0));
    items[1] = qp_float_new(ctx, iv->it_interval.tv_sec +
                            (double)(iv->it_interval.tv_usec / 1000000.0));
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_signal_raise_itimer_error(JSContext *ctx, int err)
{
    QPModState *ms = qp_modstate(ctx);
    JSValue exc, eno, msg, a[2];
    QPType *t;
    if (!ms)
        return JS_EXCEPTION;
    t = qp_type_from_value(ms->sig_itimer_error);
    if (!t)
        return qp_raise_os_error(ctx, err, QP_NULL);
    eno = JS_NewInt32(ctx, err);
    msg = qp_str_from_cstr(ctx, strerror(err));
    if (JS_IsException(msg))
        return JS_EXCEPTION;
    a[0] = eno;
    a[1] = msg;
    exc = qp_exc_new(ctx, t, 2, (JSValueConst *)a);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return JS_EXCEPTION;
    return qp_raise_object(ctx, exc);
}

static JSValue qp_signal_setitimer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    int which;
    struct itimerval nv, old;
    if (qp_arg_check_positional(ctx, "setitimer", nargs, 2, 3) < 0 ||
        qp_arg_int(ctx, args[0], &which) < 0)
        return JS_EXCEPTION;
    if (qp_signal_timeval(ctx, args[1], &nv.it_value) < 0 ||
        qp_signal_timeval(ctx, nargs > 2 ? args[2] : QP_NULL, &nv.it_interval) < 0)
        return JS_EXCEPTION;
    /* Let OS check "which" value */
    if (setitimer(which, &nv, &old) != 0)
        return qp_signal_raise_itimer_error(ctx, errno);
    return qp_signal_itimer_retval(ctx, &old);
}

static JSValue qp_signal_getitimer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    int which;
    struct itimerval old;
    if (qp_arg_int(ctx, args[0], &which) < 0)
        return JS_EXCEPTION;
    if (getitimer(which, &old) != 0)
        return qp_signal_raise_itimer_error(ctx, errno);
    return qp_signal_itimer_retval(ctx, &old);
}

static JSValue qp_signal_pthread_sigmask(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    int how, err;
    sigset_t mask, previous;
    if (qp_arg_check_positional(ctx, "pthread_sigmask", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &how) < 0 ||
        qp_signal_sigset_conv(ctx, args[1], &mask) < 0)
        return JS_EXCEPTION;
    err = pthread_sigmask(how, &mask, &previous);
    if (err != 0)
        return qp_raise_os_error(ctx, err, QP_NULL);
    /* if signals was unblocked, signal handlers have been called */
    if (qp_signal_check_signals(ctx) < 0)
        return JS_EXCEPTION;
    return qp_signal_sigset_to_set(ctx, &previous);
}

static JSValue qp_signal_sigpending(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    sigset_t mask;
    if (sigpending(&mask))
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return qp_signal_sigset_to_set(ctx, &mask);
}

static JSValue qp_signal_sigwait(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    sigset_t set;
    int err, signum;
    if (qp_signal_sigset_conv(ctx, args[0], &set) < 0)
        return JS_EXCEPTION;
    err = sigwait(&set, &signum);
    if (err)
        return qp_raise_os_error(ctx, err, QP_NULL);
    return JS_NewInt32(ctx, signum);
}

static JSValue qp_signal_valid_signals(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    sigset_t mask;
    if (sigemptyset(&mask) || sigfillset(&mask))
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return qp_signal_sigset_to_set(ctx, &mask);
}

#ifdef QP_SIGNAL_HAVE_SIGWAITINFO
static const char *const qp_signal_siginfo_fields[] = {
    "si_signo", "si_code", "si_errno", "si_pid", "si_uid", "si_status", "si_band", NULL
};
static const char *const qp_signal_siginfo_docs[] = {
    "signal number", "signal code", "errno associated with this signal",
    "sending process ID", "real user ID of sending process",
    "exit value or signal", "band event for SIGPOLL", NULL
};
static const QPStructSeqDesc qp_signal_siginfo_desc = {
    "signal.struct_siginfo",
    "struct_siginfo: Result from sigwaitinfo or sigtimedwait.\n\n"
    "This object may be accessed either as a tuple of\n"
    "(si_signo, si_code, si_errno, si_pid, si_uid, si_status, si_band),\n"
    "or via the attributes si_signo, si_code, and so on.",
    qp_signal_siginfo_fields, qp_signal_siginfo_docs, 7, 0
};

static JSValue qp_signal_fill_siginfo(JSContext *ctx, siginfo_t *si)
{
    JSValue items[7];
    int i;
    items[0] = JS_NewInt32(ctx, si->si_signo);
    items[1] = JS_NewInt32(ctx, si->si_code);
    items[2] = JS_NewInt32(ctx, si->si_errno);
    items[3] = qp_long_from_int64(ctx, (int64_t)si->si_pid);
    items[4] = qp_long_from_uint64(ctx, (uint64_t)si->si_uid);
    items[5] = JS_NewInt32(ctx, si->si_status);
    items[6] = qp_long_from_int64(ctx, (int64_t)si->si_band);
    for (i = 0; i < 7; i++) {
        if (JS_IsException(items[i])) {
            int j;
            for (j = 0; j < 7; j++)
                if (!JS_IsException(items[j]))
                    JS_FreeValue(ctx, items[j]);
            return JS_EXCEPTION;
        }
    }
    {
        QPModState *ms = QP_STATE(ctx)->mod_state;
        QPType *t = ms ? qp_type_from_value(ms->sig_siginfo_type) : NULL;
        if (!t) {
            for (i = 0; i < 7; i++)
                JS_FreeValue(ctx, items[i]);
            return qp_raise_system_error(ctx, "signal.struct_siginfo is not initialized");
        }
        return qp_structseq_new(ctx, t, items);
    }
}

static JSValue qp_signal_sigwaitinfo(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    sigset_t set;
    siginfo_t si;
    int err, async_err = 0;
    if (qp_signal_sigset_conv(ctx, args[0], &set) < 0)
        return JS_EXCEPTION;
    do {
        err = sigwaitinfo(&set, &si);
    } while (err == -1 && errno == EINTR &&
             !(async_err = (qp_signal_check_signals(ctx) < 0)));
    if (err == -1)
        return !async_err ? qp_raise_os_error(ctx, errno, QP_NULL) : JS_EXCEPTION;
    return qp_signal_fill_siginfo(ctx, &si);
}

static JSValue qp_signal_sigtimedwait(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    sigset_t set;
    siginfo_t si;
    int64_t timeout, deadline;
    struct timespec now;
    if (qp_arg_check_positional(ctx, "sigtimedwait", nargs, 2, 2) < 0 ||
        qp_signal_sigset_conv(ctx, args[0], &set) < 0 ||
        qp_signal_seconds_ns(ctx, args[1], &timeout) < 0)
        return JS_EXCEPTION;
    if (timeout < 0)
        return qp_raise_value_error(ctx, "timeout must be non-negative");
    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + timeout;
    for (;;) {
        struct timespec ts;
        ts.tv_sec = (time_t)(timeout / 1000000000);
        ts.tv_nsec = (long)(timeout % 1000000000);
        if (sigtimedwait(&set, &si, &ts) != -1)
            break;
        if (errno != EINTR) {
            if (errno == EAGAIN)
                return JS_UNDEFINED;
            return qp_raise_os_error(ctx, errno, QP_NULL);
        }
        /* sigtimedwait() was interrupted by a signal (EINTR) */
        if (qp_signal_check_signals(ctx) < 0)
            return JS_EXCEPTION;
        clock_gettime(CLOCK_MONOTONIC, &now);
        timeout = deadline - ((int64_t)now.tv_sec * 1000000000 + now.tv_nsec);
        if (timeout < 0)
            break;
    }
    return qp_signal_fill_siginfo(ctx, &si);
}
#endif /* QP_SIGNAL_HAVE_SIGWAITINFO */

static JSValue qp_signal_pthread_kill(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    JSValue v, lo, hi, m;
    int64_t low, high;
    int ov, signalnum, err;
    uint64_t thread_id;

    if (qp_arg_check_positional(ctx, "pthread_kill", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_long_check(ctx, args[0]) &&
        qp_type_of(ctx, args[0])->slots.nb.nb_index == NULL)
        return qp_raise_type_error(ctx, "pthread_kill() argument 1 must be int, not %T", args[0]);
    /* unsigned_long(bitwise=True): PyLong_AsUnsignedLongMask */
    v = qp_index(ctx, args[0]);
    if (JS_IsException(v))
        return JS_EXCEPTION;
    m = qp_long_from_int64(ctx, INT64_MAX);
    lo = qp_binop(ctx, v, m, QP_NB_AND);
    JS_FreeValue(ctx, m);
    m = JS_NewInt32(ctx, 63);
    hi = JS_IsException(lo) ? JS_EXCEPTION : qp_binop(ctx, v, m, QP_NB_RSHIFT);
    JS_FreeValue(ctx, v);
    if (JS_IsException(hi)) {
        JS_FreeValue(ctx, lo);
        return JS_EXCEPTION;
    }
    m = JS_NewInt32(ctx, 1);
    v = qp_binop(ctx, hi, m, QP_NB_AND);
    JS_FreeValue(ctx, hi);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, lo);
        return JS_EXCEPTION;
    }
    low = high = 0;
    if (qp_long_as_int64_overflow(lo, &low, &ov) < 0 ||
        qp_long_as_int64_overflow(v, &high, &ov) < 0) {
        JS_FreeValue(ctx, lo);
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lo);
    JS_FreeValue(ctx, v);
    thread_id = (uint64_t)low | ((uint64_t)high << 63);
    if (qp_arg_int(ctx, args[1], &signalnum) < 0)
        return JS_EXCEPTION;
    err = pthread_kill((pthread_t)(uintptr_t)thread_id, signalnum);
    if (err != 0)
        return qp_raise_os_error(ctx, err, QP_NULL);
    /* the signal may have been send to the current thread */
    return qp_signal_none_or_exc(ctx, qp_signal_check_signals(ctx));
}

#ifdef QP_SIGNAL_HAVE_PIDFD
static JSValue qp_signal_pidfd_send_signal(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                           int nargs, JSValueConst kwnames, int magic)
{
    int pidfd, signalnum, flags = 0;
    if (qp_arg_check_positional(ctx, "pidfd_send_signal", nargs, 2, 4) < 0 ||
        qp_arg_int(ctx, args[0], &pidfd) < 0 ||
        qp_arg_int(ctx, args[1], &signalnum) < 0)
        return JS_EXCEPTION;
    if (nargs > 3 && qp_arg_int(ctx, args[3], &flags) < 0)
        return JS_EXCEPTION;
    if (nargs > 2 && !JS_IsUndefined(args[2]))
        return qp_raise_type_error(ctx, "siginfo must be None");
    if (syscall(__NR_pidfd_send_signal, pidfd, signalnum, NULL, flags) < 0)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return JS_UNDEFINED;
}
#endif

static const QPMethodDef qp_signal_methods[] = {
    { "default_int_handler", qp_signal_default_int_handler, QP_METH_FASTCALL, 0,
      "default_int_handler($module, signalnum, frame, /)\n--\n\n"
      "The default handler for SIGINT installed by Python.\n\n"
      "It raises KeyboardInterrupt." },
    { "alarm", qp_signal_alarm, QP_METH_O, 0,
      "alarm($module, seconds, /)\n--\n\n"
      "Arrange for SIGALRM to arrive after the given number of seconds." },
    { "setitimer", qp_signal_setitimer, QP_METH_FASTCALL, 0,
      "setitimer($module, which, seconds, interval=0.0, /)\n--\n\n"
      "Sets given itimer (one of ITIMER_REAL, ITIMER_VIRTUAL or ITIMER_PROF).\n\n"
      "The timer will fire after value seconds and after that every interval\n"
      "seconds.  The itimer can be cleared by setting seconds to zero.\n\n"
      "Returns old values as a tuple: (delay, interval)." },
    { "getitimer", qp_signal_getitimer, QP_METH_O, 0,
      "getitimer($module, which, /)\n--\n\n"
      "Returns current value of given itimer." },
    { "signal", qp_signal_signal, QP_METH_FASTCALL, 0,
      "signal($module, signalnum, handler, /)\n--\n\n"
      "Set the action for the given signal.\n\n"
      "The action can be SIG_DFL, SIG_IGN, or a callable Python object.\n"
      "The previous action is returned.  See getsignal() for possible return\n"
      "values.\n\n"
      "*** IMPORTANT NOTICE ***\n"
      "A signal handler function is called with two arguments: the first is\n"
      "the signal number, the second is the interrupted stack frame." },
    { "raise_signal", qp_signal_raise_signal, QP_METH_O, 0,
      "raise_signal($module, signalnum, /)\n--\n\n"
      "Send a signal to the executing process." },
    { "strsignal", qp_signal_strsignal, QP_METH_O, 0,
      "strsignal($module, signalnum, /)\n--\n\n"
      "Return the system description of the given signal.\n\n"
      "Returns the description of signal *signalnum*, such as \"Interrupt\"\n"
      "for :const:`SIGINT`. Returns :const:`None` if *signalnum* has no\n"
      "description. Raises :exc:`ValueError` if *signalnum* is invalid." },
    { "getsignal", qp_signal_getsignal, QP_METH_O, 0,
      "getsignal($module, signalnum, /)\n--\n\n"
      "Return the current action for the given signal.\n\n"
      "The return value can be:\n"
      "  SIG_IGN -- if the signal is being ignored\n"
      "  SIG_DFL -- if the default action for the signal is in effect\n"
      "  None    -- if an unknown handler is in effect\n"
      "  anything else -- the callable Python object used as a handler" },
    { "set_wakeup_fd", qp_signal_set_wakeup_fd, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_wakeup_fd($module, fd, /, *, warn_on_full_buffer=True)\n--\n\n"
      "Sets the fd to be written to (with the signal number) when a signal comes in.\n\n"
      "A library can use this to wakeup select or poll.\n"
      "The previous fd or -1 is returned.\n\n"
      "The fd must be non-blocking." },
    { "siginterrupt", qp_signal_siginterrupt, QP_METH_FASTCALL, 0,
      "siginterrupt($module, signalnum, flag, /)\n--\n\n"
      "Change system call restart behaviour.\n\n"
      "If flag is False, system calls will be restarted when interrupted by\n"
      "signal sig, else system calls will be interrupted." },
    { "pause", qp_signal_pause, QP_METH_NOARGS, 0,
      "pause($module, /)\n--\n\n"
      "Wait until a signal arrives." },
#ifdef QP_SIGNAL_HAVE_PIDFD
    { "pidfd_send_signal", qp_signal_pidfd_send_signal, QP_METH_FASTCALL, 0,
      "pidfd_send_signal($module, pidfd, signalnum, siginfo=None, flags=0, /)\n--\n\n"
      "Send a signal to a process referred to by a pid file descriptor." },
#endif
    { "pthread_kill", qp_signal_pthread_kill, QP_METH_FASTCALL, 0,
      "pthread_kill($module, thread_id, signalnum, /)\n--\n\n"
      "Send a signal to a thread." },
    { "pthread_sigmask", qp_signal_pthread_sigmask, QP_METH_FASTCALL, 0,
      "pthread_sigmask($module, how, mask, /)\n--\n\n"
      "Fetch and/or change the signal mask of the calling thread." },
    { "sigpending", qp_signal_sigpending, QP_METH_NOARGS, 0,
      "sigpending($module, /)\n--\n\n"
      "Examine pending signals.\n\n"
      "Returns a set of signal numbers that are pending for delivery to\n"
      "the calling thread." },
    { "sigwait", qp_signal_sigwait, QP_METH_O, 0,
      "sigwait($module, sigset, /)\n--\n\n"
      "Wait for a signal.\n\n"
      "Suspend execution of the calling thread until the delivery of one of the\n"
      "signals specified in the signal set sigset.  The function accepts the\n"
      "signal and returns the signal number." },
#ifdef QP_SIGNAL_HAVE_SIGWAITINFO
    { "sigwaitinfo", qp_signal_sigwaitinfo, QP_METH_O, 0,
      "sigwaitinfo($module, sigset, /)\n--\n\n"
      "Wait synchronously until one of the signals in *sigset* is delivered.\n\n"
      "Returns a struct_siginfo containing information about the signal." },
    { "sigtimedwait", qp_signal_sigtimedwait, QP_METH_FASTCALL, 0,
      "sigtimedwait($module, sigset, timeout, /)\n--\n\n"
      "Like sigwaitinfo(), but with a timeout.\n\n"
      "The timeout is specified in seconds, rounded up to nanoseconds." },
#endif
    { "valid_signals", qp_signal_valid_signals, QP_METH_NOARGS, 0,
      "valid_signals($module, /)\n--\n\n"
      "Return a set of valid signal numbers on this platform.\n\n"
      "The signal numbers returned by this function can be safely passed to\n"
      "functions like `pthread_sigmask`." },
    { NULL }
};

static int qp_signal_add(JSContext *ctx, JSValueConst module, const char *name, JSValue v)
{
    JSAtom at;
    int r;
    if (JS_IsException(v))
        return -1;
    at = JS_NewAtom(ctx, name);
    if (at == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), at, v);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, v);
    return r;
}

/* signal_add_constants() */
static int qp_signal_add_constants(JSContext *ctx, JSValueConst module)
{
#define QP_SIG_INT(m) \
    do { if (qp_signal_add(ctx, module, #m, JS_NewInt32(ctx, (m))) < 0) return -1; } while (0)
    if (qp_signal_add(ctx, module, "NSIG", JS_NewInt32(ctx, QP_NSIG)) < 0)
        return -1;
#ifdef SIG_BLOCK
    QP_SIG_INT(SIG_BLOCK);
#endif
#ifdef SIG_UNBLOCK
    QP_SIG_INT(SIG_UNBLOCK);
#endif
#ifdef SIG_SETMASK
    QP_SIG_INT(SIG_SETMASK);
#endif
#ifdef SIGHUP
    QP_SIG_INT(SIGHUP);
#endif
#ifdef SIGINT
    QP_SIG_INT(SIGINT);
#endif
#ifdef SIGBREAK
    QP_SIG_INT(SIGBREAK);
#endif
#ifdef SIGQUIT
    QP_SIG_INT(SIGQUIT);
#endif
#ifdef SIGILL
    QP_SIG_INT(SIGILL);
#endif
#ifdef SIGTRAP
    QP_SIG_INT(SIGTRAP);
#endif
#ifdef SIGIOT
    QP_SIG_INT(SIGIOT);
#endif
#ifdef SIGABRT
    QP_SIG_INT(SIGABRT);
#endif
#ifdef SIGEMT
    QP_SIG_INT(SIGEMT);
#endif
#ifdef SIGFPE
    QP_SIG_INT(SIGFPE);
#endif
#ifdef SIGKILL
    QP_SIG_INT(SIGKILL);
#endif
#ifdef SIGBUS
    QP_SIG_INT(SIGBUS);
#endif
#ifdef SIGSEGV
    QP_SIG_INT(SIGSEGV);
#endif
#ifdef SIGSYS
    QP_SIG_INT(SIGSYS);
#endif
#ifdef SIGPIPE
    QP_SIG_INT(SIGPIPE);
#endif
#ifdef SIGALRM
    QP_SIG_INT(SIGALRM);
#endif
#ifdef SIGTERM
    QP_SIG_INT(SIGTERM);
#endif
#ifdef SIGUSR1
    QP_SIG_INT(SIGUSR1);
#endif
#ifdef SIGUSR2
    QP_SIG_INT(SIGUSR2);
#endif
#ifdef SIGCLD
    QP_SIG_INT(SIGCLD);
#endif
#ifdef SIGCHLD
    QP_SIG_INT(SIGCHLD);
#endif
#ifdef SIGPWR
    QP_SIG_INT(SIGPWR);
#endif
#ifdef SIGIO
    QP_SIG_INT(SIGIO);
#endif
#ifdef SIGURG
    QP_SIG_INT(SIGURG);
#endif
#ifdef SIGWINCH
    QP_SIG_INT(SIGWINCH);
#endif
#ifdef SIGPOLL
    QP_SIG_INT(SIGPOLL);
#endif
#ifdef SIGSTOP
    QP_SIG_INT(SIGSTOP);
#endif
#ifdef SIGTSTP
    QP_SIG_INT(SIGTSTP);
#endif
#ifdef SIGCONT
    QP_SIG_INT(SIGCONT);
#endif
#ifdef SIGTTIN
    QP_SIG_INT(SIGTTIN);
#endif
#ifdef SIGTTOU
    QP_SIG_INT(SIGTTOU);
#endif
#ifdef SIGVTALRM
    QP_SIG_INT(SIGVTALRM);
#endif
#ifdef SIGPROF
    QP_SIG_INT(SIGPROF);
#endif
#ifdef SIGXCPU
    QP_SIG_INT(SIGXCPU);
#endif
#ifdef SIGXFSZ
    QP_SIG_INT(SIGXFSZ);
#endif
#ifdef SIGRTMIN
    QP_SIG_INT(SIGRTMIN);
#endif
#ifdef SIGRTMAX
    QP_SIG_INT(SIGRTMAX);
#endif
#ifdef SIGINFO
    QP_SIG_INT(SIGINFO);
#endif
#ifdef SIGSTKFLT
    QP_SIG_INT(SIGSTKFLT);
#endif
#ifdef ITIMER_REAL
    QP_SIG_INT(ITIMER_REAL);
#endif
#ifdef ITIMER_VIRTUAL
    QP_SIG_INT(ITIMER_VIRTUAL);
#endif
#ifdef ITIMER_PROF
    QP_SIG_INT(ITIMER_PROF);
#endif
#undef QP_SIG_INT
    return 0;
}

/* PyErr_NewException("signal.ItimerError", PyExc_OSError, NULL) */
static JSValue qp_signal_new_itimer_error(JSContext *ctx)
{
    JSValue bases, ns, mod, name, cls, b[1];
    b[0] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, OSError)));
    bases = qp_tuple_new_steal(ctx, 1, b);
    if (JS_IsException(bases))
        return JS_EXCEPTION;
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns)) {
        JS_FreeValue(ctx, bases);
        return JS_EXCEPTION;
    }
    mod = qp_str_from_cstr(ctx, "signal");
    name = qp_str_from_cstr(ctx, "ItimerError");
    if (JS_IsException(mod) || JS_IsException(name) ||
        qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __module__), mod) < 0)
        cls = JS_EXCEPTION;
    else
        cls = qp_type_new(ctx, QP_TYPE(ctx, type), name, bases, ns, QP_NULL);
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, bases);
    return cls;
}

/* signal_module_exec() + signal_get_set_handlers() */
static int qp_signal_exec(JSContext *ctx, JSValueConst module)
{
    QPModState *ms = qp_modstate(ctx);
    JSValue v, sigint_func;
    int signum;

    if (!ms)
        return -1;
    if (QP_IS_NULL(ms->sig_dfl))
        ms->sig_dfl = JS_NewInt32(ctx, (int)(intptr_t)SIG_DFL);
    if (QP_IS_NULL(ms->sig_ign))
        ms->sig_ign = JS_NewInt32(ctx, (int)(intptr_t)SIG_IGN);
    if (QP_IS_NULL(ms->sig_itimer_error)) {
        v = qp_signal_new_itimer_error(ctx);
        if (JS_IsException(v))
            return -1;
        ms->sig_itimer_error = v;
    }
    if (qp_signal_add_constants(ctx, module) < 0)
        return -1;
    if (qp_signal_add(ctx, module, "SIG_DFL", JS_DupValue(ctx, ms->sig_dfl)) < 0 ||
        qp_signal_add(ctx, module, "SIG_IGN", JS_DupValue(ctx, ms->sig_ign)) < 0 ||
        qp_signal_add(ctx, module, "ItimerError", JS_DupValue(ctx, ms->sig_itimer_error)) < 0)
        return -1;
#ifdef QP_SIGNAL_HAVE_SIGWAITINFO
    v = qp_structseq_new_type(ctx, &qp_signal_siginfo_desc);
    if (JS_IsException(v))
        return -1;
    qp_modstate_free_value(ctx, &ms->sig_siginfo_type);
    ms->sig_siginfo_type = JS_DupValue(ctx, v);
    if (qp_signal_add(ctx, module, "struct_siginfo", v) < 0)
        return -1;
#endif

    /* only the main interpreter's context handles signals */
    if (qp_signal_main_ctx && qp_signal_main_ctx != ctx)
        return 0;
    qp_signal_main_ctx = ctx;
    qp_signal_wakeup_fd = ms->sig_wakeup_fd;
    qp_signal_wakeup_warn = ms->sig_wakeup_warn_on_full_buffer;

    for (signum = 1; signum < QP_NSIG; signum++) {
        void (*c_handler)(int) = qp_signal_getsig(signum);
        JSValue func;
        if (c_handler == SIG_DFL)
            func = JS_DupValue(ctx, ms->sig_dfl);
        else if (c_handler == SIG_IGN)
            func = JS_DupValue(ctx, ms->sig_ign);
        else
            func = JS_UNDEFINED;   /* None of our business */
        qp_signal_set_handler(ctx, ms, signum, func);
    }
    /* Install Python SIGINT handler which raises KeyboardInterrupt */
    sigint_func = ms->sig_handlers[SIGINT];
    if (qp_signal_compare_handler(sigint_func, ms->sig_dfl)) {
        JSAtom at = JS_NewAtom(ctx, "default_int_handler");
        JSValue h;
        if (at == JS_ATOM_NULL)
            return -1;
        h = qp_getattr(ctx, module, at);
        JS_FreeAtom(ctx, at);
        if (JS_IsException(h))
            return -1;
        qp_signal_set_handler(ctx, ms, SIGINT, h);
        qp_signal_setsig(SIGINT, qp_signal_handler);
    }
    return 0;
}

static const QPModuleDef qp_moddef__signal = {
    "_signal",
    "This module provides mechanisms to use signal handlers in Python.\n\n"
    "Functions:\n\n"
    "alarm() -- cause SIGALRM after a specified time [Unix only]\n"
    "setitimer() -- cause a signal (described below) after a specified\n"
    "               float time and the timer may restart then [Unix only]\n"
    "getitimer() -- get current value of timer [Unix only]\n"
    "signal() -- set the action for a given signal\n"
    "getsignal() -- get the signal action for a given signal\n"
    "pause() -- wait until a signal arrives [Unix only]\n"
    "default_int_handler() -- default SIGINT handler\n\n"
    "signal constants:\n"
    "SIG_DFL -- used to refer to the system default handler\n"
    "SIG_IGN -- used to ignore the signal\n"
    "NSIG -- number of defined signals\n"
    "SIGINT, SIGTERM, etc. -- signal numbers\n\n"
    "itimer constants:\n"
    "ITIMER_REAL -- decrements in real time, and delivers SIGALRM upon\n"
    "               expiration\n"
    "ITIMER_VIRTUAL -- decrements only when the process is executing,\n"
    "               and delivers SIGVTALRM upon expiration\n"
    "ITIMER_PROF -- decrements both when the process is executing and\n"
    "               when the system is executing on behalf of the process.\n"
    "               Coupled with ITIMER_VIRTUAL, this timer is usually\n"
    "               used to profile the time spent by the application\n"
    "               in user and kernel space. SIGPROF is delivered upon\n"
    "               expiration.\n\n\n"
    "*** IMPORTANT NOTICE ***\n"
    "A signal handler function is called with two arguments:\n"
    "the first is the signal number, the second is the interrupted stack frame.",
    qp_signal_methods,
    qp_signal_exec,
};

/*
 * marshal module: port of CPython 3.15 Python/marshal.c (PSF-2.0).
 *
 * Same wire format and byte output as CPython (Py_MARSHAL_VERSION 6).
 * Differences that follow from the object model:
 *  - "Uniquely referenced" (w_ref/w_complete) is the JS reference count of
 *    heap values; immediate values (int32 ints, short bigints, floats) have
 *    no count and are treated like CPython's shared/immortal objects, with
 *    identity = (tag, value bits).
 *  - "Interned" strings are atom strings plus the empty string and the
 *    1-character latin-1 strings (statically interned in CPython).
 *    Interned strings read back are made atoms.
 *  - Code objects (TYPE_CODE) hold QuickJS bytecode, not CPython's code
 *    fields: the format hash of the build (qp_pyc_format_hash, two longs),
 *    the size and bytes of JS_WriteObject(bytecode, JS_WRITE_OBJ_BYTECODE)
 *    (the Python metadata and constants included, see
 *    qp_bc_write_code / qp_bc_write_pyobj), then a byte telling whether
 *    the replace() overrides of the code object follow (name, qualname and
 *    filename objects, None if not overridden, then the firstlineno
 *    override flag and value and the added co_flags as longs).  loads()
 *    of code written by another build raises ValueError("bad marshal data
 *    (code object of another build)"); .pyc files of another build are
 *    rejected before by their magic number (_imp.pyc_magic_number_token).
 *  - sys.audit events (marshal.dumps/loads/load) are not raised: there is
 *    no C-level audit entry point in the core yet.
 *  - load() passes a bytearray (instead of a memoryview over the internal
 *    buffer) of exactly the requested size to file.readinto().
 */

#define QP_MARSHAL_VERSION 6

#define QP_MARSHAL_MAX_STACK_DEPTH 2000

#define QP_MARSHAL_TYPE_NULL               '0'
#define QP_MARSHAL_TYPE_NONE               'N'
#define QP_MARSHAL_TYPE_FALSE              'F'
#define QP_MARSHAL_TYPE_TRUE               'T'
#define QP_MARSHAL_TYPE_STOPITER           'S'
#define QP_MARSHAL_TYPE_ELLIPSIS           '.'
#define QP_MARSHAL_TYPE_BINARY_FLOAT       'g'
#define QP_MARSHAL_TYPE_BINARY_COMPLEX     'y'
#define QP_MARSHAL_TYPE_LONG               'l'
#define QP_MARSHAL_TYPE_STRING             's'
#define QP_MARSHAL_TYPE_TUPLE              '('
#define QP_MARSHAL_TYPE_LIST               '['
#define QP_MARSHAL_TYPE_DICT               '{'
#define QP_MARSHAL_TYPE_FROZENDICT         '}'
#define QP_MARSHAL_TYPE_CODE               'c'
#define QP_MARSHAL_TYPE_UNICODE            'u'
#define QP_MARSHAL_TYPE_UNKNOWN            '?'
#define QP_MARSHAL_TYPE_SET                '<'
#define QP_MARSHAL_TYPE_FROZENSET          '>'
#define QP_MARSHAL_TYPE_SLICE              ':'
#define QP_MARSHAL_TYPE_INTERNED           't'
#define QP_MARSHAL_TYPE_ASCII              'a'
#define QP_MARSHAL_TYPE_ASCII_INTERNED     'A'
#define QP_MARSHAL_TYPE_SHORT_ASCII        'z'
#define QP_MARSHAL_TYPE_SHORT_ASCII_INTERNED 'Z'
#define QP_MARSHAL_TYPE_INT                'i'
#define QP_MARSHAL_TYPE_SMALL_TUPLE        ')'
#define QP_MARSHAL_TYPE_COMPLEX            'x'
#define QP_MARSHAL_TYPE_FLOAT              'f'
#define QP_MARSHAL_TYPE_INT64              'I'
#define QP_MARSHAL_TYPE_REF                'r'
#define QP_MARSHAL_FLAG_REF                0x80

#define QP_MARSHAL_WFERR_OK 0
#define QP_MARSHAL_WFERR_UNMARSHALLABLE 1
#define QP_MARSHAL_WFERR_NESTEDTOODEEP 2
#define QP_MARSHAL_WFERR_NOMEMORY 3
#define QP_MARSHAL_WFERR_CODE_NOT_ALLOWED 4
#define QP_MARSHAL_WFERR_SET_CHANGED_SIZE 5

#define QP_MARSHAL_SHIFT 15
#define QP_MARSHAL_BASE (1 << QP_MARSHAL_SHIFT)
#define QP_MARSHAL_MASK (QP_MARSHAL_BASE - 1)
#define QP_MARSHAL_SIZE32_MAX 0x7FFFFFFF

/* ---- identity table (CPython _Py_hashtable keyed by pointer) ----------- */

typedef struct qp_marshal_refent {
    JSValue v;          /* owned reference (CPython Py_NewRef in the table); JS_UNINITIALIZED = empty */
    uint64_t key;
    uint32_t w;
} qp_marshal_refent;

typedef struct qp_marshal_WFILE {
    JSContext *ctx;
    int error;
    int depth;
    uint8_t *buf;
    size_t len, cap;
    qp_marshal_refent *tab;     /* NULL: not writing references */
    size_t tab_cap, nentries;
    int version;
    int allow_code;
} qp_marshal_WFILE;

static void qp_marshal_w_object(JSValueConst v, qp_marshal_WFILE *p);
static void qp_marshal_w_code(JSValueConst v, int flag, qp_marshal_WFILE *p);

static uint64_t qp_marshal_idkey(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (JS_VALUE_HAS_REF_COUNT(v))
        return (uint64_t)(uintptr_t)JS_VALUE_GET_PTR(v);
    if (tag == JS_TAG_FLOAT64) {
        double d = JS_VALUE_GET_FLOAT64(v);
        uint64_t u;
        memcpy(&u, &d, 8);
        return u;
    }
    if (tag == JS_TAG_SHORT_BIG_INT)
        return (uint64_t)JS_VALUE_GET_SHORT_BIG_INT(v);
    return (uint64_t)(int64_t)JS_VALUE_GET_INT(v);
}

static size_t qp_marshal_idhash(JSValueConst v, uint64_t key)
{
    uint64_t h = key ^ ((uint64_t)(uint32_t)JS_VALUE_GET_TAG(v) << 56);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h;
}

static qp_marshal_refent *qp_marshal_tab_find(qp_marshal_WFILE *p, JSValueConst v, int *found)
{
    uint64_t key = qp_marshal_idkey(v);
    size_t mask = p->tab_cap - 1, i = qp_marshal_idhash(v, key) & mask;
    int tag = JS_VALUE_GET_TAG(v);
    for (;;) {
        qp_marshal_refent *e = &p->tab[i];
        if (QP_IS_NULL(e->v)) {
            *found = 0;
            return e;
        }
        if (e->key == key && JS_VALUE_GET_TAG(e->v) == tag) {
            *found = 1;
            return e;
        }
        i = (i + 1) & mask;
    }
}

static int qp_marshal_tab_grow(qp_marshal_WFILE *p)
{
    size_t old_cap = p->tab_cap, i, ncap = old_cap ? old_cap * 2 : 64;
    qp_marshal_refent *old = p->tab, *nt;
    nt = js_malloc(p->ctx, ncap * sizeof(*nt));
    if (!nt)
        return -1;
    for (i = 0; i < ncap; i++)
        nt[i].v = QP_NULL;
    p->tab = nt;
    p->tab_cap = ncap;
    for (i = 0; i < old_cap; i++) {
        if (!QP_IS_NULL(old[i].v)) {
            int found;
            qp_marshal_refent *e = qp_marshal_tab_find(p, old[i].v, &found);
            *e = old[i];
        }
    }
    js_free(p->ctx, old);
    return 0;
}

static int qp_marshal_w_init_refs(qp_marshal_WFILE *wf, int version)
{
    if (version >= 3) {
        if (qp_marshal_tab_grow(wf) < 0) {
            qp_raise_memory_error(wf->ctx);
            return -1;
        }
    }
    return 0;
}

static void qp_marshal_w_clear_refs(qp_marshal_WFILE *wf)
{
    size_t i;
    if (!wf->tab)
        return;
    for (i = 0; i < wf->tab_cap; i++) {
        if (!QP_IS_NULL(wf->tab[i].v))
            JS_FreeValue(wf->ctx, wf->tab[i].v);
    }
    js_free(wf->ctx, wf->tab);
    wf->tab = NULL;
}

/* ---- output primitives ---------------------------------------------------- */

static int qp_marshal_w_reserve(qp_marshal_WFILE *p, size_t needed)
{
    size_t ncap;
    uint8_t *nb;
    if (p->buf == NULL && p->cap != 0)
        return 0;   /* already failed */
    if (p->cap - p->len >= needed)
        return 1;
    ncap = p->cap ? p->cap : 50;
    while (ncap - p->len < needed) {
        if (ncap > SIZE_MAX / 2) {
            ncap = 0;
            break;
        }
        ncap += ncap > 16384 ? 16384 : ncap;   /* CPython grows by min(size, 16 KiB) */
    }
    if (ncap == 0 || ncap > (size_t)PY_SSIZE_T_MAX) {
        js_free(p->ctx, p->buf);
        p->buf = NULL;
        p->len = 0;
        p->cap = 1;
        p->error = QP_MARSHAL_WFERR_NOMEMORY;
        return 0;
    }
    nb = js_realloc(p->ctx, p->buf, ncap);
    if (!nb) {
        js_free(p->ctx, p->buf);
        p->buf = NULL;
        p->len = 0;
        p->cap = 1;
        p->error = QP_MARSHAL_WFERR_NOMEMORY;
        return 0;
    }
    p->buf = nb;
    p->cap = ncap;
    return 1;
}

static void qp_marshal_w_byte(int c, qp_marshal_WFILE *p)
{
    if (qp_marshal_w_reserve(p, 1))
        p->buf[p->len++] = (uint8_t)c;
}

static void qp_marshal_w_string(const void *s, size_t n, qp_marshal_WFILE *p)
{
    if (n == 0)
        return;
    if (qp_marshal_w_reserve(p, n)) {
        memcpy(p->buf + p->len, s, n);
        p->len += n;
    }
}

static void qp_marshal_w_short(int x, qp_marshal_WFILE *p)
{
    qp_marshal_w_byte(x & 0xff, p);
    qp_marshal_w_byte((x >> 8) & 0xff, p);
}

static void qp_marshal_w_long(long x, qp_marshal_WFILE *p)
{
    qp_marshal_w_byte((int)(x & 0xff), p);
    qp_marshal_w_byte((int)((x >> 8) & 0xff), p);
    qp_marshal_w_byte((int)((x >> 16) & 0xff), p);
    qp_marshal_w_byte((int)((x >> 24) & 0xff), p);
}

/* W_SIZE */
static int qp_marshal_w_size(Py_ssize_t n, qp_marshal_WFILE *p)
{
    if (n > QP_MARSHAL_SIZE32_MAX) {
        p->depth--;
        p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
        return -1;
    }
    qp_marshal_w_long((long)n, p);
    return 0;
}

static void qp_marshal_w_pstring(const void *s, Py_ssize_t n, qp_marshal_WFILE *p)
{
    if (qp_marshal_w_size(n, p) < 0)
        return;
    qp_marshal_w_string(s, n, p);
}

static void qp_marshal_w_short_pstring(const void *s, Py_ssize_t n, qp_marshal_WFILE *p)
{
    qp_marshal_w_byte((uint8_t)n, p);
    qp_marshal_w_string(s, n, p);
}

#define QP_MARSHAL_W_TYPE(t, p) qp_marshal_w_byte((t) | flag, (p))

/* w_PyLong: TYPE_LONG with base 2**15 digits, least significant first,
   signed digit count. */
static void qp_marshal_w_pylong(JSValueConst v, int flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    int64_t sv;
    int ovf = 0;

    QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_LONG, p);
    if (JS_VALUE_GET_TAG(v) != JS_TAG_BIG_INT) {
        uint64_t d, abs_value;
        long l = 0;
        int sign;
        qp_long_as_int64_overflow(v, &sv, &ovf);
        if (sv == 0) {
            qp_marshal_w_long(0, p);
            return;
        }
        sign = sv < 0 ? -1 : 1;
        abs_value = sv < 0 ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
        d = abs_value;
        do {
            d >>= QP_MARSHAL_SHIFT;
            l += sign;
        } while (d);
        qp_marshal_w_long(l, p);
        d = abs_value;
        do {
            qp_marshal_w_short((int)(d & QP_MARSHAL_MASK), p);
            d >>= QP_MARSHAL_SHIFT;
        } while (d);
        return;
    }
    {
        JSBigInt *b = JS_VALUE_GET_PTR(v);
        uint32_t n = b->len, i;
        int neg = js_bigint_sign(b);
        js_limb_t *mag, carry;
        uint64_t bitlen, ndig, k;

        mag = js_malloc(ctx, n * sizeof(js_limb_t));
        if (!mag) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->depth--;
            p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
            return;
        }
        memcpy(mag, b->tab, n * sizeof(js_limb_t));
        if (neg) {
            carry = 1;
            for (i = 0; i < n; i++) {
                js_limb_t t = ~mag[i] + carry;
                carry = carry && t == 0;
                mag[i] = t;
            }
        }
        while (n > 1 && mag[n - 1] == 0)
            n--;
        bitlen = (uint64_t)(n - 1) * JS_LIMB_BITS;
        {
            js_limb_t top = mag[n - 1];
            while (top) {
                bitlen++;
                top >>= 1;
            }
        }
        ndig = (bitlen + QP_MARSHAL_SHIFT - 1) / QP_MARSHAL_SHIFT;
        if (ndig > QP_MARSHAL_SIZE32_MAX) {
            js_free(ctx, mag);
            p->depth--;
            p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
            return;
        }
        qp_marshal_w_long(neg ? -(long)ndig : (long)ndig, p);
        for (k = 0; k < ndig; k++) {
            uint64_t bit = k * QP_MARSHAL_SHIFT;
            uint64_t li = bit / JS_LIMB_BITS;
            unsigned sh = (unsigned)(bit % JS_LIMB_BITS);
            uint32_t dg = (uint32_t)(mag[li] >> sh);
            if (sh + QP_MARSHAL_SHIFT > JS_LIMB_BITS && li + 1 < n)
                dg |= (uint32_t)(mag[li + 1] << (JS_LIMB_BITS - sh));
            qp_marshal_w_short((int)(dg & QP_MARSHAL_MASK), p);
        }
        js_free(ctx, mag);
    }
}

static void qp_marshal_w_float_bin(double v, qp_marshal_WFILE *p)
{
    uint8_t buf[8];
    uint64_t u;
    int i;
    memcpy(&u, &v, 8);
    for (i = 0; i < 8; i++)
        buf[i] = (uint8_t)(u >> (8 * i));   /* PyFloat_Pack8(v, buf, 1): little endian */
    qp_marshal_w_string(buf, 8, p);
}

static void qp_marshal_w_float_str(double v, qp_marshal_WFILE *p)
{
    qp_error err = { 0 };
    char *buf = qp_double_to_string(v, 'g', 17, 0, NULL, &err);
    if (!buf) {
        qp_error_clear(&err);
        p->error = QP_MARSHAL_WFERR_NOMEMORY;
        return;
    }
    qp_marshal_w_short_pstring(buf, strlen(buf), p);
    qp_sys_free(buf);
}

/* PyUnicode_CHECK_INTERNED for exact str values. */
static int qp_marshal_str_interned(JSValueConst v)
{
    JSString *s;
    uint32_t c;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_STRING)
        return 0;
    s = JS_VALUE_GET_STRING(v);
    if (s->atom_type)
        return 1;
    if (s->len == 0)
        return 1;
    if (s->len == 1) {
        c = s->is_wide_char ? s->u.str16[0] : s->u.str8[0];
        return c < 256;
    }
    return 0;
}

/* _PyObject_IsUniquelyReferenced (interned strings count as immortal). */
static int qp_marshal_unique(JSValueConst v)
{
    if (!JS_VALUE_HAS_REF_COUNT(v))
        return 0;
    if (qp_marshal_str_interned(v))
        return 0;
    return js_rc(JS_VALUE_GET_PTR(v))->ref_count == 1;
}

static int qp_marshal_is_type(JSContext *ctx, JSValueConst v, QPType *t)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && qp_type_of(ctx, v) == t;
}

static int qp_marshal_w_ref(JSValueConst v, int *flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    qp_marshal_refent *e;
    int found;

    if (p->version < 3 || p->tab == NULL)
        return 0;
    if (qp_marshal_unique(v) && !(qp_str_check_exact(v) && qp_marshal_str_interned(v)))
        return 0;
    e = qp_marshal_tab_find(p, v, &found);
    if (found) {
        uint32_t w = e->w;
        if (w & 0x80000000U) {
            qp_raise_value_error(ctx, "cannot marshal recursion %T objects", v);
            goto err;
        }
        qp_marshal_w_byte(QP_MARSHAL_TYPE_REF, p);
        qp_marshal_w_long((long)(int)w, p);
        return 1;
    } else {
        size_t w = p->nentries;
        if (w >= 0x7fffffff) {
            qp_raise_value_error(ctx, "too many objects");
            goto err;
        }
        if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, code)) || qp_slice_check(v) ||
            qp_marshal_is_type(ctx, v, QP_TYPE(ctx, frozendict)))
            w |= 0x80000000U;
        e->v = JS_DupValue(ctx, v);
        e->key = qp_marshal_idkey(v);
        e->w = (uint32_t)w;
        p->nentries++;
        if (p->nentries * 2 > p->tab_cap && qp_marshal_tab_grow(p) < 0)
            goto err;
        *flag |= QP_MARSHAL_FLAG_REF;
        return 0;
    }
err:
    p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
    return 1;
}

static void qp_marshal_w_complete(JSValueConst v, qp_marshal_WFILE *p)
{
    qp_marshal_refent *e;
    int found;
    if (p->version < 3 || p->tab == NULL)
        return;
    if (qp_marshal_unique(v))
        return;
    e = qp_marshal_tab_find(p, v, &found);
    if (!found)
        return;
    e->w &= ~0x80000000U;
}

/* One element of a set being serialised: sort key = its own dump, then the
   iteration index (sorted(v, key=marshal.dumps), bpo-37596). */
typedef struct qp_marshal_setitem {
    uint8_t *dump;
    size_t dump_len;
    size_t index;
    JSValue value;
} qp_marshal_setitem;

static int qp_marshal_setitem_cmp(const void *a, const void *b)
{
    const qp_marshal_setitem *x = a, *y = b;
    size_t n = x->dump_len < y->dump_len ? x->dump_len : y->dump_len;
    int c = n ? memcmp(x->dump, y->dump, n) : 0;
    if (c)
        return c;
    if (x->dump_len != y->dump_len)
        return x->dump_len < y->dump_len ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static int qp_marshal_write_buf(JSContext *ctx, JSValueConst x, int version, int allow_code,
                                qp_marshal_WFILE *wf);

static void qp_marshal_w_set(JSValueConst v, int flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    Py_ssize_t n, i = 0, pos = 0, k;
    qp_marshal_setitem *items;
    JSValue key;
    Py_hash_t hash;
    int changed = 0;

    if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, frozenset)))
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_FROZENSET, p);
    else
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_SET, p);
    n = qp_set_size(v);
    if (qp_marshal_w_size(n, p) < 0)
        return;
    items = js_mallocz(ctx, (n ? n : 1) * sizeof(*items));
    if (!items) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        p->error = QP_MARSHAL_WFERR_NOMEMORY;
        return;
    }
    /* Take references first (CPython's _PySet_NextEntryRef), so no element
       looks uniquely referenced to the nested writers, as in CPython where
       the (dump, index, element) tuples hold them. */
    while (qp_set_next(v, &pos, &key, &hash)) {
        if (i == n) {
            changed = 1;
            break;
        }
        items[i].value = JS_DupValue(ctx, key);
        items[i].index = i;
        i++;
    }
    if (!changed && i == n) {
        for (k = 0; k < n; k++) {
            qp_marshal_WFILE sub;
            if (qp_marshal_write_buf(ctx, items[k].value, p->version, p->allow_code, &sub) != 0) {
                js_free(ctx, sub.buf);
                JS_FreeValue(ctx, JS_GetException(ctx));
                p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
                break;
            }
            items[k].dump = sub.buf;
            items[k].dump_len = sub.len;
        }
        if (p->error == QP_MARSHAL_WFERR_OK) {
            qsort(items, n, sizeof(*items), qp_marshal_setitem_cmp);
            for (k = 0; k < n; k++)
                qp_marshal_w_object(items[k].value, p);
        }
    } else {
        p->error = QP_MARSHAL_WFERR_SET_CHANGED_SIZE;
    }
    for (k = 0; k < i; k++) {
        js_free(ctx, items[k].dump);
        JS_FreeValue(ctx, items[k].value);
    }
    js_free(ctx, items);
}

static void qp_marshal_w_str(JSValueConst v, int flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    qp_strview view;
    int interned = qp_marshal_str_interned(v);

    if (p->version >= 4) {
        Py_ssize_t i;
        int ascii;
        if (qp_str_view_acquire(ctx, v, &view) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->depth--;
            p->error = QP_MARSHAL_WFERR_NOMEMORY;
            return;
        }
        ascii = view.kind == 1;
        for (i = 0; ascii && i < view.len; i++) {
            if (((const uint8_t *)view.data)[i] >= 0x80)
                ascii = 0;
        }
        if (ascii) {
            if (view.len < 256) {
                QP_MARSHAL_W_TYPE(interned ? QP_MARSHAL_TYPE_SHORT_ASCII_INTERNED
                                           : QP_MARSHAL_TYPE_SHORT_ASCII, p);
                qp_marshal_w_short_pstring(view.data, view.len, p);
            } else {
                QP_MARSHAL_W_TYPE(interned ? QP_MARSHAL_TYPE_ASCII_INTERNED
                                           : QP_MARSHAL_TYPE_ASCII, p);
                qp_marshal_w_pstring(view.data, view.len, p);
            }
            qp_str_view_release(ctx, &view);
            return;
        }
        qp_str_view_release(ctx, &view);
    }
    {
        JSValue utf8 = qp_str_encode(ctx, v, "utf8", "surrogatepass");
        const uint8_t *data;
        Py_ssize_t len;
        if (JS_IsException(utf8)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->depth--;
            p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
            return;
        }
        if (p->version >= 3 && interned)
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_INTERNED, p);
        else
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_UNICODE, p);
        data = qp_bytes_data(utf8, &len);
        qp_marshal_w_pstring(data, len, p);
        JS_FreeValue(ctx, utf8);
    }
}

static int qp_marshal_get_attr(JSContext *ctx, JSValueConst obj, const char *name, JSValue *out)
{
    JSAtom a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL)
        return -1;
    *out = qp_getattr(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return JS_IsException(*out) ? -1 : 0;
}

static void qp_marshal_w_complex_object(JSValueConst v, int flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    Py_ssize_t i, n;
    int tag = JS_VALUE_GET_TAG(v);

    if (qp_long_check_exact(v)) {
        int64_t x;
        int overflow = 0;
        qp_long_as_int64_overflow(v, &x, &overflow);
        if (overflow || x < INT32_MIN || x > INT32_MAX) {
            qp_marshal_w_pylong(v, flag, p);
        } else {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_INT, p);
            qp_marshal_w_long((long)x, p);
        }
    }
    else if (tag == JS_TAG_FLOAT64) {
        double d = JS_VALUE_GET_FLOAT64(v);
        if (p->version > 1) {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_BINARY_FLOAT, p);
            qp_marshal_w_float_bin(d, p);
        } else {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_FLOAT, p);
            qp_marshal_w_float_str(d, p);
        }
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, complex))) {
        QPComplex c;
        if (qp_complex_as_c(ctx, v, &c) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
            return;
        }
        if (p->version > 1) {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_BINARY_COMPLEX, p);
            qp_marshal_w_float_bin(c.real, p);
            qp_marshal_w_float_bin(c.imag, p);
        } else {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_COMPLEX, p);
            qp_marshal_w_float_str(c.real, p);
            qp_marshal_w_float_str(c.imag, p);
        }
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, bytes))) {
        Py_ssize_t len;
        const uint8_t *data = qp_bytes_data(v, &len);
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_STRING, p);
        qp_marshal_w_pstring(data, len, p);
    }
    else if (qp_str_check_exact(v)) {
        qp_marshal_w_str(v, flag, p);
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, tuple))) {
        n = QP_TUPLE_SIZE(v);
        if (p->version >= 4 && n < 256) {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_SMALL_TUPLE, p);
            qp_marshal_w_byte((int)n, p);
        } else {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_TUPLE, p);
            if (qp_marshal_w_size(n, p) < 0)
                return;
        }
        for (i = 0; i < n; i++)
            qp_marshal_w_object(QP_TUPLE_ITEMS(v)[i], p);
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, list))) {
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_LIST, p);
        n = qp_list_size(v);
        if (qp_marshal_w_size(n, p) < 0)
            return;
        for (i = 0; i < n; i++) {
            Py_ssize_t len;
            JSValue *items = qp_list_items(v, &len);
            /* PyList_GET_ITEM on a list a buffer exporter may have shrunk */
            qp_marshal_w_object(i < len ? items[i] : JS_UNDEFINED, p);
        }
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, dict)) ||
             qp_marshal_is_type(ctx, v, QP_TYPE(ctx, frozendict))) {
        Py_ssize_t pos = 0;
        JSValue key, value;
        int frozen = qp_marshal_is_type(ctx, v, QP_TYPE(ctx, frozendict));
        if (frozen) {
            if (p->version < 6) {
                qp_marshal_w_byte(QP_MARSHAL_TYPE_UNKNOWN, p);
                p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
                return;
            }
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_FROZENDICT, p);
        } else {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_DICT, p);
        }
        /* This one is NULL object terminated! */
        while (qp_dict_next(v, &pos, &key, &value)) {
            qp_marshal_w_object(key, p);
            qp_marshal_w_object(value, p);
        }
        qp_marshal_w_object(QP_NULL, p);
        if (frozen)
            qp_marshal_w_complete(v, p);
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, set)) ||
             qp_marshal_is_type(ctx, v, QP_TYPE(ctx, frozenset))) {
        qp_marshal_w_set(v, flag, p);
    }
    else if (qp_marshal_is_type(ctx, v, QP_TYPE(ctx, code))) {
        if (!p->allow_code) {
            p->error = QP_MARSHAL_WFERR_CODE_NOT_ALLOWED;
            return;
        }
        qp_marshal_w_code(v, flag, p);
    }
    else if (qp_slice_check(v)) {
        JSValue start, stop, step;
        if (p->version < 5) {
            qp_marshal_w_byte(QP_MARSHAL_TYPE_UNKNOWN, p);
            p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
            return;
        }
        if (qp_marshal_get_attr(ctx, v, "start", &start) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->error = QP_MARSHAL_WFERR_NOMEMORY;
            return;
        }
        if (qp_marshal_get_attr(ctx, v, "stop", &stop) < 0) {
            JS_FreeValue(ctx, start);
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->error = QP_MARSHAL_WFERR_NOMEMORY;
            return;
        }
        if (qp_marshal_get_attr(ctx, v, "step", &step) < 0) {
            JS_FreeValue(ctx, start);
            JS_FreeValue(ctx, stop);
            JS_FreeValue(ctx, JS_GetException(ctx));
            p->error = QP_MARSHAL_WFERR_NOMEMORY;
            return;
        }
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_SLICE, p);
        qp_marshal_w_object(start, p);
        qp_marshal_w_object(stop, p);
        qp_marshal_w_object(step, p);
        JS_FreeValue(ctx, start);
        JS_FreeValue(ctx, stop);
        JS_FreeValue(ctx, step);
        qp_marshal_w_complete(v, p);
    }
    else {
        /* PyObject_CheckBuffer: write unknown bytes-like objects (bytearray,
           memoryview, bytes subclasses, ...) as a bytes object. */
        QPBuffer view;
        if (qp_buffer_get(ctx, v, &view, QP_BUF_SIMPLE) == 0) {
            QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_STRING, p);
            qp_marshal_w_pstring(view.buf, view.len, p);
            qp_buffer_release(ctx, &view);
            return;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_UNKNOWN, p);
        p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
    }
}

/* TYPE_CODE payload (see the file header) */
static void qp_marshal_w_code(JSValueConst v, int flag, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    QPCodeObject *c = qp_co_data(v);
    uint64_t h = qp_pyc_format_hash();
    uint8_t *buf;
    size_t len;
    BOOL overrides;

    if (!c) {
        p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
        return;
    }
    buf = JS_WriteObject(ctx, &len, c->bytecode, JS_WRITE_OBJ_BYTECODE);
    if (!buf) {
        p->error = QP_MARSHAL_WFERR_UNMARSHALLABLE;
        return;
    }
    QP_MARSHAL_W_TYPE(QP_MARSHAL_TYPE_CODE, p);
    qp_marshal_w_long((long)(int32_t)(uint32_t)h, p);
    qp_marshal_w_long((long)(int32_t)(uint32_t)(h >> 32), p);
    if (qp_marshal_w_size((Py_ssize_t)len, p) == 0)
        qp_marshal_w_string(buf, len, p);
    js_free(ctx, buf);
    overrides = !QP_IS_NULL(c->name) || !QP_IS_NULL(c->qualname) ||
        !QP_IS_NULL(c->filename) || c->has_firstlineno || c->flags_add;
    qp_marshal_w_byte(overrides, p);
    if (overrides) {
        qp_marshal_w_object(QP_IS_NULL(c->name) ? JS_UNDEFINED : c->name, p);
        qp_marshal_w_object(QP_IS_NULL(c->qualname) ? JS_UNDEFINED : c->qualname, p);
        qp_marshal_w_object(QP_IS_NULL(c->filename) ? JS_UNDEFINED : c->filename, p);
        qp_marshal_w_long(c->has_firstlineno, p);
        qp_marshal_w_long(c->firstlineno, p);
        qp_marshal_w_long((long)(int32_t)c->flags_add, p);
    }
}

static void qp_marshal_w_object(JSValueConst v, qp_marshal_WFILE *p)
{
    JSContext *ctx = p->ctx;
    int flag = 0;
    int tag;

    if (p->error != QP_MARSHAL_WFERR_OK)
        return;
    p->depth++;
    tag = JS_VALUE_GET_TAG(v);
    if (p->depth > QP_MARSHAL_MAX_STACK_DEPTH) {
        p->error = QP_MARSHAL_WFERR_NESTEDTOODEEP;
    } else if (QP_IS_NULL(v)) {
        qp_marshal_w_byte(QP_MARSHAL_TYPE_NULL, p);
    } else if (tag == JS_TAG_UNDEFINED || tag == JS_TAG_NULL) {
        qp_marshal_w_byte(QP_MARSHAL_TYPE_NONE, p);
    } else if (tag == JS_TAG_OBJECT &&
               JS_VALUE_GET_OBJ(v) == QP_EXC(ctx, StopIteration)->obj) {
        qp_marshal_w_byte(QP_MARSHAL_TYPE_STOPITER, p);
    } else if (tag == JS_TAG_OBJECT &&
               JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(QP_STATE(ctx)->ellipsis)) {
        qp_marshal_w_byte(QP_MARSHAL_TYPE_ELLIPSIS, p);
    } else if (tag == JS_TAG_BOOL) {
        qp_marshal_w_byte(JS_VALUE_GET_BOOL(v) ? QP_MARSHAL_TYPE_TRUE : QP_MARSHAL_TYPE_FALSE, p);
    } else if (!qp_marshal_w_ref(v, &flag, p)) {
        qp_marshal_w_complex_object(v, flag, p);
    }
    p->depth--;
}

/* Run the writer; returns the WFERR code.  wf->buf/wf->len hold the output
   (free wf->buf with js_free).  Exceptions raised on the way are left to
   the caller (qp_marshal_write_object overrides them like CPython). */
static int qp_marshal_write_buf(JSContext *ctx, JSValueConst x, int version, int allow_code,
                                qp_marshal_WFILE *wf)
{
    memset(wf, 0, sizeof(*wf));
    wf->ctx = ctx;
    wf->version = version;
    wf->allow_code = allow_code;
    if (qp_marshal_w_init_refs(wf, version) < 0)
        return QP_MARSHAL_WFERR_NOMEMORY;
    if (!qp_marshal_w_reserve(wf, 1))
        return wf->error;
    qp_marshal_w_object(x, wf);
    qp_marshal_w_clear_refs(wf);
    return wf->error;
}

/* _PyMarshal_WriteObjectToString */
static JSValue qp_marshal_write_object(JSContext *ctx, JSValueConst x, int version, int allow_code)
{
    qp_marshal_WFILE wf;
    int error = qp_marshal_write_buf(ctx, x, version, allow_code, &wf);
    JSValue res;

    if (error != QP_MARSHAL_WFERR_OK) {
        js_free(ctx, wf.buf);
        qp_marshal_w_clear_refs(&wf);
        JS_FreeValue(ctx, JS_GetException(ctx));
        switch (error) {
        case QP_MARSHAL_WFERR_NOMEMORY:
            return qp_raise_memory_error(ctx);
        case QP_MARSHAL_WFERR_NESTEDTOODEEP:
            return qp_raise_value_error(ctx, "object too deeply nested to marshal");
        case QP_MARSHAL_WFERR_CODE_NOT_ALLOWED:
            return qp_raise_value_error(ctx, "marshalling code objects is disallowed");
        case QP_MARSHAL_WFERR_SET_CHANGED_SIZE:
            return qp_raise_runtime_error(ctx, "set changed size during iteration");
        default:
            return qp_raise_value_error(ctx, "unmarshallable object");
        }
    }
    res = qp_bytes_new(ctx, wf.buf, (Py_ssize_t)wf.len);
    js_free(ctx, wf.buf);
    return res;
}

/* ========================================================================= */
/* Reader                                                                     */
/* ========================================================================= */

typedef struct qp_marshal_RFILE {
    JSContext *ctx;
    int depth;
    JSValueConst readable;      /* QP_NULL for loads() */
    const uint8_t *ptr;
    const uint8_t *end;
    uint8_t *buf;
    Py_ssize_t buf_size;
    JSValue *refs;              /* reference list; QP_NULL = reserved slot (CPython's None) */
    Py_ssize_t nrefs, refs_cap;
    int allow_code;
} qp_marshal_RFILE;

static const uint8_t *qp_marshal_r_string(Py_ssize_t n, qp_marshal_RFILE *p)
{
    JSContext *ctx = p->ctx;
    Py_ssize_t read = -1;

    if (p->ptr != NULL) {
        /* Fast path for loads() */
        const uint8_t *res = p->ptr;
        Py_ssize_t left = p->end - p->ptr;
        if (left < n) {
            qp_raise(ctx, QP_EXC(ctx, EOFError), "marshal data too short");
            return NULL;
        }
        p->ptr += n;
        return res;
    }
    if (p->buf == NULL || p->buf_size < n) {
        uint8_t *tmp = js_realloc(ctx, p->buf, n ? n : 1);
        if (!tmp) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            qp_raise_memory_error(ctx);
            return NULL;
        }
        p->buf = tmp;
        p->buf_size = n;
    }
    {
        JSValue ba, res;
        JSAtom a;
        ba = qp_bytearray_new(ctx, p->buf, n);
        if (JS_IsException(ba))
            return NULL;
        a = JS_NewAtom(ctx, "readinto");
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, ba);
            return NULL;
        }
        res = qp_call_method(ctx, p->readable, a, (JSValueConst *)&ba, 1, QP_NULL);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, ba);
            return NULL;
        }
        /* PyNumber_AsSsize_t(res, PyExc_ValueError) */
        {
            JSValue idx = qp_index(ctx, res);
            int64_t r64;
            int overflow = 0;
            JS_FreeValue(ctx, res);
            if (JS_IsException(idx)) {
                JS_FreeValue(ctx, ba);
                return NULL;
            }
            qp_long_as_int64_overflow(idx, &r64, &overflow);
            if (overflow || r64 > PY_SSIZE_T_MAX || r64 < -PY_SSIZE_T_MAX - 1) {
                qp_raise_value_error(ctx, "cannot fit '%T' into an index-sized integer", idx);
                JS_FreeValue(ctx, idx);
                JS_FreeValue(ctx, ba);
                return NULL;
            }
            JS_FreeValue(ctx, idx);
            read = (Py_ssize_t)r64;
        }
        if (read == n) {
            Py_ssize_t blen;
            const uint8_t *data = qp_bytes_data(ba, &blen);
            memcpy(p->buf, data, blen < n ? blen : n);
        }
        JS_FreeValue(ctx, ba);
    }
    if (read != n) {
        if (read > n)
            qp_raise_value_error(ctx, "read() returned too much data: "
                                 "%zd bytes requested, %zd returned", n, read);
        else
            qp_raise(ctx, QP_EXC(ctx, EOFError), "EOF read where not expected");
        return NULL;
    }
    return p->buf;
}

/* Returns the byte or -1 (EOF) with an exception set. */
static int qp_marshal_r_byte(qp_marshal_RFILE *p)
{
    if (p->ptr != NULL) {
        if (p->ptr < p->end)
            return *p->ptr++;
    } else {
        const uint8_t *ptr = qp_marshal_r_string(1, p);
        if (ptr != NULL)
            return *ptr;
        return -1;
    }
    qp_raise(p->ctx, QP_EXC(p->ctx, EOFError), "EOF read where not expected");
    return -1;
}

/* Returns -1 with an exception set on error (check err). */
static int qp_marshal_r_short(qp_marshal_RFILE *p, int *err)
{
    int x = -1;
    const uint8_t *buffer = qp_marshal_r_string(2, p);
    *err = buffer == NULL;
    if (buffer != NULL) {
        x = buffer[0] | (buffer[1] << 8);
        x = (int16_t)x;     /* sign extension */
    }
    return x;
}

static long qp_marshal_r_long(qp_marshal_RFILE *p, int *err)
{
    long x = -1;
    const uint8_t *buffer = qp_marshal_r_string(4, p);
    *err = buffer == NULL;
    if (buffer != NULL) {
        uint32_t u = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
                     ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
        x = (long)(int32_t)u;
    }
    return x;
}

/* TYPE_INT64 */
static JSValue qp_marshal_r_long64(qp_marshal_RFILE *p)
{
    const uint8_t *b = qp_marshal_r_string(8, p);
    uint64_t u = 0;
    int i;
    if (b == NULL)
        return JS_EXCEPTION;
    for (i = 7; i >= 0; i--)
        u = (u << 8) | b[i];
    return qp_long_from_int64(p->ctx, (int64_t)u);
}

static JSValue qp_marshal_r_pylong(qp_marshal_RFILE *p)
{
    JSContext *ctx = p->ctx;
    int err;
    long n = qp_marshal_r_long(p, &err);
    Py_ssize_t size, j;
    size_t nl;
    JSBigInt *r;
    int neg;

    if (err)
        return JS_EXCEPTION;
    if (n < -QP_MARSHAL_SIZE32_MAX || n > QP_MARSHAL_SIZE32_MAX)
        return qp_raise_value_error(ctx, "bad marshal data (long size out of range)");
    if (n == 0)
        return JS_NewInt32(ctx, 0);   /* (CPython: writer of size 1 with digit 0) */
    neg = n < 0;
    size = neg ? -n : n;
    nl = ((size_t)size * QP_MARSHAL_SHIFT) / JS_LIMB_BITS + 2;
    if (nl > QP_LONG_MAX_LIMBS)
        return qp_raise_memory_error(ctx);
    QP_BIG_CALL(ctx, r, js_bigint_new(ctx, (int)nl));
    if (!r) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qp_raise_memory_error(ctx);
    }
    memset(r->tab, 0, nl * sizeof(js_limb_t));
    for (j = 0; j < size; j++) {
        int md = qp_marshal_r_short(p, &err);
        uint64_t bit;
        size_t li;
        unsigned sh;
        if (err) {
            js_free(ctx, r);
            return JS_EXCEPTION;
        }
        if (md < 0 || md > QP_MARSHAL_BASE) {
            js_free(ctx, r);
            return qp_raise_value_error(ctx, "bad marshal data (digit out of range in long)");
        }
        if (md == 0 && j == size - 1) {
            js_free(ctx, r);
            return qp_raise_value_error(ctx, "bad marshal data (unnormalized long data)");
        }
        bit = (uint64_t)j * QP_MARSHAL_SHIFT;
        li = bit / JS_LIMB_BITS;
        sh = (unsigned)(bit % JS_LIMB_BITS);
        r->tab[li] |= (js_limb_t)md << sh;
        if (sh + QP_MARSHAL_SHIFT + 1 > JS_LIMB_BITS)
            r->tab[li + 1] |= (js_limb_t)md >> (JS_LIMB_BITS - sh);
    }
    if (neg) {
        js_limb_t carry = 1;
        size_t i;
        for (i = 0; i < nl; i++) {
            js_limb_t t = ~r->tab[i] + carry;
            carry = carry && t == 0;
            r->tab[i] = t;
        }
    }
    r = js_bigint_normalize(ctx, r);
    return qp_long_normalize(ctx, JS_CompactBigInt(ctx, r));
}

static double qp_marshal_r_float_bin(qp_marshal_RFILE *p, int *err)
{
    const uint8_t *b = qp_marshal_r_string(8, p);
    uint64_t u = 0;
    double d;
    int i;
    *err = b == NULL;
    if (b == NULL)
        return -1;
    for (i = 7; i >= 0; i--)
        u = (u << 8) | b[i];
    memcpy(&d, &u, 8);
    return d;
}

static double qp_marshal_r_float_str(qp_marshal_RFILE *p, int *err)
{
    int n;
    char buf[256];
    const uint8_t *ptr;
    qp_error e = { 0 };
    double d;

    *err = 1;
    n = qp_marshal_r_byte(p);
    if (n < 0)
        return -1;
    ptr = qp_marshal_r_string(n, p);
    if (ptr == NULL)
        return -1;
    memcpy(buf, ptr, n);
    buf[n] = '\0';
    d = qp_string_to_double(buf, NULL, QP_ERR_NONE, &e);
    if (e.kind != QP_ERR_NONE) {
        qp_raise_qp_error(p->ctx, &e, NULL);
        qp_error_clear(&e);
        return -1;
    }
    *err = 0;
    return d;
}

static int qp_marshal_refs_append(qp_marshal_RFILE *p, JSValue v /* consumed */)
{
    if (p->nrefs == p->refs_cap) {
        Py_ssize_t ncap = p->refs_cap ? p->refs_cap * 2 : 16;
        JSValue *nr = js_realloc(p->ctx, p->refs, ncap * sizeof(JSValue));
        if (!nr) {
            JS_FreeValue(p->ctx, v);
            JS_FreeValue(p->ctx, JS_GetException(p->ctx));
            qp_raise_memory_error(p->ctx);
            return -1;
        }
        p->refs = nr;
        p->refs_cap = ncap;
    }
    p->refs[p->nrefs++] = v;
    return 0;
}

/* allocate the reflist index for a new object. Return -1 on failure */
static Py_ssize_t qp_marshal_r_ref_reserve(int flag, qp_marshal_RFILE *p)
{
    if (flag) {
        Py_ssize_t idx = p->nrefs;
        if (idx >= 0x7ffffffe) {
            qp_raise_value_error(p->ctx, "bad marshal data (index list too large)");
            return -1;
        }
        if (qp_marshal_refs_append(p, QP_NULL) < 0)
            return -1;
        return idx;
    }
    return 0;
}

/* Store o at a reserved index; o may be an exception (nothing done). */
static JSValue qp_marshal_r_ref_insert(JSValue o, Py_ssize_t idx, int flag, qp_marshal_RFILE *p)
{
    if (!JS_IsException(o) && flag) {
        JSValue tmp = p->refs[idx];
        p->refs[idx] = JS_DupValue(p->ctx, o);
        JS_FreeValue(p->ctx, tmp);
    }
    return o;
}

static JSValue qp_marshal_r_ref(JSValue o, int flag, qp_marshal_RFILE *p)
{
    if (JS_IsException(o))
        return o;
    if (qp_marshal_refs_append(p, JS_DupValue(p->ctx, o)) < 0) {
        JS_FreeValue(p->ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue qp_marshal_r_object(qp_marshal_RFILE *p);
static JSValue qp_marshal_r_code(qp_marshal_RFILE *p);

/* r_object for a container element: QP_NULL (TYPE_NULL) becomes
   TypeError "NULL object in marshal data for <what>". */
static JSValue qp_marshal_r_item(qp_marshal_RFILE *p, const char *what)
{
    JSValue v = qp_marshal_r_object(p);
    if (QP_IS_NULL(v))
        return qp_raise_type_error(p->ctx, "NULL object in marshal data for %s", what);
    return v;
}

static JSValue qp_marshal_r_str_ascii(qp_marshal_RFILE *p, Py_ssize_t n, int is_interned)
{
    JSContext *ctx = p->ctx;
    const uint8_t *ptr = qp_marshal_r_string(n, p);
    JSValue v;
    if (ptr == NULL)
        return JS_EXCEPTION;
    /* PyUnicode_FromKindAndData(PyUnicode_1BYTE_KIND, ...): latin-1 */
    v = qp_str_from_view(ctx, qp_strview_make(1, ptr, n));
    if (JS_IsException(v) || !is_interned)
        return v;
    {
        JSAtom a = qp_str_to_atom(ctx, v);
        JS_FreeValue(ctx, v);
        if (a == JS_ATOM_NULL)
            return JS_EXCEPTION;
        v = qp_str_from_atom(ctx, a);
        JS_FreeAtom(ctx, a);
        return v;
    }
}

static JSValue qp_marshal_r_object(qp_marshal_RFILE *p)
{
    JSContext *ctx = p->ctx;
    JSValue v, v2, retval = QP_NULL;
    Py_ssize_t idx = 0;
    long i, n;
    int type, code = qp_marshal_r_byte(p);
    int flag, is_interned = 0, err;

    if (code < 0) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, EOFError))) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            qp_raise(ctx, QP_EXC(ctx, EOFError), "EOF read where object expected");
        }
        return JS_EXCEPTION;
    }
    p->depth++;
    if (p->depth > QP_MARSHAL_MAX_STACK_DEPTH) {
        p->depth--;
        return qp_raise_value_error(ctx, "recursion limit exceeded");
    }
    flag = code & QP_MARSHAL_FLAG_REF;
    type = code & ~QP_MARSHAL_FLAG_REF;

#define QP_MARSHAL_R_REF(O) do { if (flag) O = qp_marshal_r_ref(O, flag, p); } while (0)

    switch (type) {
    case QP_MARSHAL_TYPE_NULL:
        break;
    case QP_MARSHAL_TYPE_NONE:
        retval = JS_UNDEFINED;
        break;
    case QP_MARSHAL_TYPE_STOPITER:
        retval = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, StopIteration)));
        break;
    case QP_MARSHAL_TYPE_ELLIPSIS:
        retval = JS_DupValue(ctx, QP_STATE(ctx)->ellipsis);
        break;
    case QP_MARSHAL_TYPE_FALSE:
        retval = JS_FALSE;
        break;
    case QP_MARSHAL_TYPE_TRUE:
        retval = JS_TRUE;
        break;
    case QP_MARSHAL_TYPE_INT:
        n = qp_marshal_r_long(p, &err);
        if (err) {
            retval = JS_EXCEPTION;
            break;
        }
        retval = JS_NewInt32(ctx, (int32_t)n);
        QP_MARSHAL_R_REF(retval);
        break;
    case QP_MARSHAL_TYPE_INT64:
        retval = qp_marshal_r_long64(p);
        QP_MARSHAL_R_REF(retval);
        break;
    case QP_MARSHAL_TYPE_LONG:
        retval = qp_marshal_r_pylong(p);
        QP_MARSHAL_R_REF(retval);
        break;
    case QP_MARSHAL_TYPE_FLOAT:
    case QP_MARSHAL_TYPE_BINARY_FLOAT: {
        double x = type == QP_MARSHAL_TYPE_FLOAT ? qp_marshal_r_float_str(p, &err)
                                                 : qp_marshal_r_float_bin(p, &err);
        if (err) {
            retval = JS_EXCEPTION;
            break;
        }
        retval = qp_float_new(ctx, x);
        QP_MARSHAL_R_REF(retval);
        break;
    }
    case QP_MARSHAL_TYPE_COMPLEX:
    case QP_MARSHAL_TYPE_BINARY_COMPLEX: {
        double re, im;
        re = type == QP_MARSHAL_TYPE_COMPLEX ? qp_marshal_r_float_str(p, &err)
                                             : qp_marshal_r_float_bin(p, &err);
        if (err) {
            retval = JS_EXCEPTION;
            break;
        }
        im = type == QP_MARSHAL_TYPE_COMPLEX ? qp_marshal_r_float_str(p, &err)
                                             : qp_marshal_r_float_bin(p, &err);
        if (err) {
            retval = JS_EXCEPTION;
            break;
        }
        retval = qp_complex_new(ctx, re, im);
        QP_MARSHAL_R_REF(retval);
        break;
    }
    case QP_MARSHAL_TYPE_STRING: {
        const uint8_t *ptr;
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (bytes object size out of range)");
            retval = JS_EXCEPTION;
            break;
        }
        ptr = qp_marshal_r_string(n, p);
        if (ptr == NULL) {
            retval = JS_EXCEPTION;
            break;
        }
        retval = qp_bytes_new(ctx, ptr, n);
        QP_MARSHAL_R_REF(retval);
        break;
    }
    case QP_MARSHAL_TYPE_ASCII_INTERNED:
        is_interned = 1;
        /* fall through */
    case QP_MARSHAL_TYPE_ASCII:
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (string size out of range)");
            retval = JS_EXCEPTION;
            break;
        }
        retval = qp_marshal_r_str_ascii(p, n, is_interned);
        QP_MARSHAL_R_REF(retval);
        break;
    case QP_MARSHAL_TYPE_SHORT_ASCII_INTERNED:
        is_interned = 1;
        /* fall through */
    case QP_MARSHAL_TYPE_SHORT_ASCII:
        n = qp_marshal_r_byte(p);
        if (n < 0) {
            retval = JS_EXCEPTION;
            break;
        }
        retval = qp_marshal_r_str_ascii(p, n, is_interned);
        QP_MARSHAL_R_REF(retval);
        break;
    case QP_MARSHAL_TYPE_INTERNED:
        is_interned = 1;
        /* fall through */
    case QP_MARSHAL_TYPE_UNICODE: {
        const uint8_t *buffer;
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (string size out of range)");
            retval = JS_EXCEPTION;
            break;
        }
        if (n != 0) {
            buffer = qp_marshal_r_string(n, p);
            if (buffer == NULL) {
                retval = JS_EXCEPTION;
                break;
            }
            v = qp_str_decode(ctx, (const char *)buffer, n, "utf-8", "surrogatepass");
        } else {
            v = qp_str_from_cstr(ctx, "");
        }
        if (JS_IsException(v)) {
            retval = v;
            break;
        }
        if (is_interned) {
            JSAtom a = qp_str_to_atom(ctx, v);
            JS_FreeValue(ctx, v);
            if (a == JS_ATOM_NULL) {
                retval = JS_EXCEPTION;
                break;
            }
            v = qp_str_from_atom(ctx, a);
            JS_FreeAtom(ctx, a);
        }
        retval = v;
        QP_MARSHAL_R_REF(retval);
        break;
    }
    case QP_MARSHAL_TYPE_SMALL_TUPLE:
    case QP_MARSHAL_TYPE_TUPLE:
        if (type == QP_MARSHAL_TYPE_SMALL_TUPLE) {
            n = qp_marshal_r_byte(p);
            if (n < 0) {
                retval = JS_EXCEPTION;
                break;
            }
        } else {
            n = qp_marshal_r_long(p, &err);
            if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
                if (!err)
                    qp_raise_value_error(ctx, "bad marshal data (tuple size out of range)");
                retval = JS_EXCEPTION;
                break;
            }
        }
        v = qp_tuple_new(ctx, n);
        QP_MARSHAL_R_REF(v);
        if (JS_IsException(v)) {
            retval = v;
            break;
        }
        for (i = 0; i < n; i++) {
            v2 = qp_marshal_r_item(p, "tuple");
            if (JS_IsException(v2)) {
                JS_FreeValue(ctx, v);
                v = JS_EXCEPTION;
                break;
            }
            JS_FreeValue(ctx, QP_TUPLE_ITEMS(v)[i]);
            QP_TUPLE_ITEMS(v)[i] = v2;
        }
        retval = v;
        break;
    case QP_MARSHAL_TYPE_LIST:
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (list size out of range)");
            retval = JS_EXCEPTION;
            break;
        }
        v = qp_list_new(ctx, n);
        QP_MARSHAL_R_REF(v);
        if (JS_IsException(v)) {
            retval = v;
            break;
        }
        for (i = 0; i < n; i++) {
            Py_ssize_t len;
            JSValue *items;
            v2 = qp_marshal_r_item(p, "list");
            if (JS_IsException(v2)) {
                JS_FreeValue(ctx, v);
                v = JS_EXCEPTION;
                break;
            }
            items = qp_list_items(v, &len);
            if (i < len) {
                JS_FreeValue(ctx, items[i]);
                items[i] = v2;
            } else {
                JS_FreeValue(ctx, v2);
            }
        }
        retval = v;
        break;
    case QP_MARSHAL_TYPE_DICT:
    case QP_MARSHAL_TYPE_FROZENDICT:
        v = qp_dict_new(ctx);
        if (JS_IsException(v)) {
            retval = v;
            break;
        }
        if (type == QP_MARSHAL_TYPE_DICT) {
            QP_MARSHAL_R_REF(v);
            if (JS_IsException(v)) {
                retval = v;
                break;
            }
        } else {
            idx = qp_marshal_r_ref_reserve(flag, p);
            if (idx < 0) {
                JS_FreeValue(ctx, v);
                retval = JS_EXCEPTION;
                break;
            }
        }
        err = 0;
        for (;;) {
            JSValue key, val;
            key = qp_marshal_r_object(p);
            if (JS_IsException(key)) {
                err = 1;
                break;
            }
            if (QP_IS_NULL(key))
                break;
            val = qp_marshal_r_object(p);
            if (JS_IsException(val) || QP_IS_NULL(val)) {
                /* NULL value: CPython stops without an error here. */
                err = JS_IsException(val);
                JS_FreeValue(ctx, key);
                break;
            }
            if (qp_dict_set_item(ctx, v, key, val) < 0) {
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, val);
                err = 1;
                break;
            }
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, val);
        }
        if (err) {
            JS_FreeValue(ctx, v);
            v = JS_EXCEPTION;
        }
        if (type == QP_MARSHAL_TYPE_FROZENDICT && !JS_IsException(v)) {
            JSValue fd = qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, frozendict)), v);
            JS_FreeValue(ctx, v);
            v = qp_marshal_r_ref_insert(fd, idx, flag, p);
        }
        retval = v;
        break;
    case QP_MARSHAL_TYPE_SET:
    case QP_MARSHAL_TYPE_FROZENSET:
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n > QP_MARSHAL_SIZE32_MAX) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (set size out of range)");
            retval = JS_EXCEPTION;
            break;
        }
        if (n == 0 && type == QP_MARSHAL_TYPE_FROZENSET) {
            /* call frozenset() to get the empty frozenset singleton */
            v = qp_call0(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, frozenset)));
            if (JS_IsException(v)) {
                retval = v;
                break;
            }
            QP_MARSHAL_R_REF(v);
            retval = v;
        } else {
            v = type == QP_MARSHAL_TYPE_SET ? qp_set_new(ctx, QP_NULL)
                                            : qp_frozenset_new(ctx, QP_NULL);
            if (type == QP_MARSHAL_TYPE_SET) {
                QP_MARSHAL_R_REF(v);
            } else if (!JS_IsException(v)) {
                /* delayed registration of frozensets (they must be complete
                   before anything can reference them) */
                idx = qp_marshal_r_ref_reserve(flag, p);
                if (idx < 0) {
                    JS_FreeValue(ctx, v);
                    v = JS_EXCEPTION;
                }
            }
            if (JS_IsException(v)) {
                retval = v;
                break;
            }
            for (i = 0; i < n; i++) {
                v2 = qp_marshal_r_item(p, "set");
                if (JS_IsException(v2)) {
                    JS_FreeValue(ctx, v);
                    v = JS_EXCEPTION;
                    break;
                }
                if (qp_set_add(ctx, v, v2) < 0) {
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, v2);
                    v = JS_EXCEPTION;
                    break;
                }
                JS_FreeValue(ctx, v2);
            }
            if (type != QP_MARSHAL_TYPE_SET)
                v = qp_marshal_r_ref_insert(v, idx, flag, p);
            retval = v;
        }
        break;
    case QP_MARSHAL_TYPE_CODE: {
        Py_ssize_t cidx;
        if (!p->allow_code) {
            retval = qp_raise_value_error(ctx, "unmarshalling code objects is disallowed");
            break;
        }
        cidx = qp_marshal_r_ref_reserve(flag, p);
        if (cidx < 0) {
            retval = JS_EXCEPTION;
            break;
        }
        v = qp_marshal_r_code(p);
        if (!JS_IsException(v))
            v = qp_marshal_r_ref_insert(v, cidx, flag, p);
        retval = v;
        break;
    }
    case QP_MARSHAL_TYPE_REF:
        n = qp_marshal_r_long(p, &err);
        if (err || n < 0 || n >= p->nrefs) {
            if (!err)
                qp_raise_value_error(ctx, "bad marshal data (invalid reference)");
            retval = JS_EXCEPTION;
            break;
        }
        v = p->refs[n];
        if (QP_IS_NULL(v)) {
            retval = qp_raise_value_error(ctx, "bad marshal data (invalid reference)");
            break;
        }
        retval = JS_DupValue(ctx, v);
        break;
    case QP_MARSHAL_TYPE_SLICE: {
        JSValue start = QP_NULL, stop = QP_NULL, step = QP_NULL;
        Py_ssize_t sidx = qp_marshal_r_ref_reserve(flag, p);
        retval = JS_EXCEPTION;
        if (sidx < 0)
            break;
        start = qp_marshal_r_object(p);
        if (JS_IsException(start) || QP_IS_NULL(start))
            goto slice_cleanup;
        stop = qp_marshal_r_object(p);
        if (JS_IsException(stop) || QP_IS_NULL(stop))
            goto slice_cleanup;
        step = qp_marshal_r_object(p);
        if (JS_IsException(step) || QP_IS_NULL(step))
            goto slice_cleanup;
        retval = qp_slice_new(ctx, start, stop, step);
        qp_marshal_r_ref_insert(retval, sidx, flag, p);
    slice_cleanup:
        /* a NULL component yields NULL without an exception, like CPython */
        if (JS_IsException(retval) &&
            (QP_IS_NULL(start) || QP_IS_NULL(stop) || QP_IS_NULL(step)) &&
            !JS_IsException(start) && !JS_IsException(stop) && !JS_IsException(step))
            retval = QP_NULL;
        if (!JS_IsException(start))
            JS_FreeValue(ctx, start);
        if (!JS_IsException(stop))
            JS_FreeValue(ctx, stop);
        if (!JS_IsException(step))
            JS_FreeValue(ctx, step);
        break;
    }
    default:
        retval = qp_raise_value_error(ctx, "bad marshal data (unknown type code)");
        break;
    }
#undef QP_MARSHAL_R_REF
    p->depth--;
    return retval;
}

/* TYPE_CODE payload (see the file header) */
static JSValue qp_marshal_r_code(qp_marshal_RFILE *p)
{
    JSContext *ctx = p->ctx;
    uint64_t h = qp_pyc_format_hash();
    JSValue bc, code, ov[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };
    const uint8_t *data;
    long lo, hi, n, has_first, first, flags_add;
    int err = 0, overrides, i;
    QPCodeObject *c;

    lo = qp_marshal_r_long(p, &err);
    if (!err)
        hi = qp_marshal_r_long(p, &err);
    if (!err)
        n = qp_marshal_r_long(p, &err);
    if (err)
        return JS_EXCEPTION;
    if ((uint32_t)lo != (uint32_t)h || (uint32_t)hi != (uint32_t)(h >> 32))
        return qp_raise_value_error(ctx, "bad marshal data (code object of another build)");
    if (n < 0)
        return qp_raise_value_error(ctx, "bad marshal data (code size out of range)");
    data = qp_marshal_r_string(n, p);
    if (!data)
        return JS_EXCEPTION;
    bc = JS_ReadObject(ctx, data, (size_t)n, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(bc)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        return qp_raise_value_error(ctx, "bad marshal data (invalid code object)");
    }
    if (JS_VALUE_GET_TAG(bc) != JS_TAG_FUNCTION_BYTECODE ||
        !((JSFunctionBytecode *)JS_VALUE_GET_PTR(bc))->is_py) {
        JS_FreeValue(ctx, bc);
        return qp_raise_value_error(ctx, "bad marshal data (invalid code object)");
    }
    code = qp_code_new(ctx, bc);
    JS_FreeValue(ctx, bc);
    if (JS_IsException(code))
        return code;
    overrides = qp_marshal_r_byte(p);
    if (overrides < 0) {
        JS_FreeValue(ctx, code);
        return JS_EXCEPTION;
    }
    if (!overrides)
        return code;
    for (i = 0; i < 3; i++) {
        ov[i] = qp_marshal_r_item(p, "code");
        if (JS_IsException(ov[i]))
            goto fail;
        if (!JS_IsUndefined(ov[i]) && !qp_str_check(ctx, ov[i])) {
            qp_raise_value_error(ctx, "bad marshal data (invalid code object)");
            goto fail;
        }
    }
    has_first = qp_marshal_r_long(p, &err);
    if (!err)
        first = qp_marshal_r_long(p, &err);
    if (!err)
        flags_add = qp_marshal_r_long(p, &err);
    if (err)
        goto fail;
    c = qp_co_data(code);
    if (!JS_IsUndefined(ov[0])) {
        c->name = ov[0];
        ov[0] = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(ov[1])) {
        c->qualname = ov[1];
        ov[1] = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(ov[2])) {
        c->filename = ov[2];
        ov[2] = JS_UNDEFINED;
    }
    c->has_firstlineno = has_first != 0;
    c->firstlineno = (int)first;
    c->flags_add = (uint32_t)flags_add;
    return code;
 fail:
    for (i = 0; i < 3; i++) {
        if (!JS_IsException(ov[i]))
            JS_FreeValue(ctx, ov[i]);
    }
    JS_FreeValue(ctx, code);
    return JS_EXCEPTION;
}

/* read_object */
static JSValue qp_marshal_read_object(qp_marshal_RFILE *p)
{
    JSValue v = qp_marshal_r_object(p);
    if (QP_IS_NULL(v))
        return qp_raise_type_error(p->ctx, "NULL object in marshal data for object");
    return v;
}

static void qp_marshal_rfile_free(qp_marshal_RFILE *p)
{
    Py_ssize_t i;
    for (i = 0; i < p->nrefs; i++)
        JS_FreeValue(p->ctx, p->refs[i]);
    js_free(p->ctx, p->refs);
    js_free(p->ctx, p->buf);
}

/* ========================================================================= */
/* Module functions                                                           */
/* ========================================================================= */

static const char *const qp_marshal_kw_dump[] = { "", "", "", "allow_code", NULL };
static const char *const qp_marshal_kw_dumps[] = { "", "", "allow_code", NULL };
static const char *const qp_marshal_kw_load[] = { "", "allow_code", NULL };

/* dump(value, file, version=version, /, *, allow_code=True) */
static JSValue qp_marshal_dump(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "dump", qp_marshal_kw_dump, 2, 3, 0 };
    JSValueConst a[4];
    int version = QP_MARSHAL_VERSION, allow_code = 1;
    JSValue s, res;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2]) && qp_arg_int(ctx, a[2], &version) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[3]) && qp_arg_bool(ctx, a[3], &allow_code) < 0)
        return JS_EXCEPTION;
    s = qp_marshal_write_object(ctx, a[0], version, allow_code);
    if (JS_IsException(s))
        return s;
    res = qp_call_method(ctx, a[1], QP_ATOM(ctx, write), (JSValueConst *)&s, 1, QP_NULL);
    JS_FreeValue(ctx, s);
    return res;
}

/* load(file, /, *, allow_code=True) */
static JSValue qp_marshal_load(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "load", qp_marshal_kw_load, 1, 1, 0 };
    JSValueConst a[2];
    int allow_code = 1;
    JSValue data, result, zero = JS_NewInt32(ctx, 0);
    JSAtom read_atom;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_bool(ctx, a[1], &allow_code) < 0)
        return JS_EXCEPTION;
    /* Call read(0) to check that the object has a read method returning
       bytes (good error handling for r_string). */
    read_atom = JS_NewAtom(ctx, "read");
    if (read_atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    data = qp_call_method(ctx, a[0], read_atom, (JSValueConst *)&zero, 1, QP_NULL);
    JS_FreeAtom(ctx, read_atom);
    if (JS_IsException(data))
        return data;
    if (!qp_bytes_check(ctx, data)) {
        QPType *t = qp_type_of(ctx, data);
        char buf[ATOM_GET_STR_BUF_SIZE];
        const char *tp_name = t->tp_name ? t->tp_name : JS_AtomGetStr(ctx, buf, sizeof(buf), t->name);
        result = qp_raise_type_error(ctx, "file.read() returned not bytes but %.100s", tp_name);
    } else {
        qp_marshal_RFILE rf;
        memset(&rf, 0, sizeof(rf));
        rf.ctx = ctx;
        rf.allow_code = allow_code;
        rf.readable = a[0];
        result = qp_marshal_read_object(&rf);
        qp_marshal_rfile_free(&rf);
    }
    JS_FreeValue(ctx, data);
    return result;
}

/* dumps(value, version=version, /, *, allow_code=True) */
static JSValue qp_marshal_dumps(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "dumps", qp_marshal_kw_dumps, 1, 2, 0 };
    JSValueConst a[3];
    int version = QP_MARSHAL_VERSION, allow_code = 1;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_int(ctx, a[1], &version) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2]) && qp_arg_bool(ctx, a[2], &allow_code) < 0)
        return JS_EXCEPTION;
    return qp_marshal_write_object(ctx, a[0], version, allow_code);
}

/* loads(bytes, /, *, allow_code=True) */
static JSValue qp_marshal_loads(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "loads", qp_marshal_kw_load, 1, 1, 0 };
    JSValueConst a[2];
    int allow_code = 1;
    QPBuffer view;
    qp_marshal_RFILE rf;
    JSValue result;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, a[0], &view, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_bool(ctx, a[1], &allow_code) < 0) {
        qp_buffer_release(ctx, &view);
        return JS_EXCEPTION;
    }
    memset(&rf, 0, sizeof(rf));
    rf.ctx = ctx;
    rf.allow_code = allow_code;
    rf.readable = QP_NULL;
    /* a non-NULL ptr selects the in-memory path even for empty input */
    rf.ptr = view.buf ? view.buf : (const uint8_t *)"";
    rf.end = rf.ptr + view.len;
    result = qp_marshal_read_object(&rf);
    qp_marshal_rfile_free(&rf);
    qp_buffer_release(ctx, &view);
    return result;
}

/* Python constants in the constant pool of bytecode written by
   JS_WriteObject(JS_WRITE_OBJ_BYTECODE) (tuples, frozensets, bytes,
   complex, slices, Ellipsis...): BC_TAG_PY_OBJECT, the size and the
   marshal data of the value. */
static int qp_bc_write_pyobj(BCWriterState *s, JSValueConst obj)
{
    JSContext *ctx = s->ctx;
    qp_marshal_WFILE wf;
    int error = qp_marshal_write_buf(ctx, obj, QP_MARSHAL_VERSION, 1, &wf);

    if (error != QP_MARSHAL_WFERR_OK) {
        js_free(ctx, wf.buf);
        qp_marshal_w_clear_refs(&wf);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "unsupported Python constant");
        return -1;
    }
    bc_put_u8(s, BC_TAG_PY_OBJECT);
    bc_put_leb128(s, (uint32_t)wf.len);
    dbuf_put(&s->dbuf, wf.buf, wf.len);
    js_free(ctx, wf.buf);
    return 0;
}

static JSValue qp_bc_read_pyobj(BCReaderState *s)
{
    qp_marshal_RFILE rf;
    JSValue v;
    uint32_t len;

    if (bc_get_leb128(s, &len))
        return JS_EXCEPTION;
    if (len > (uint32_t)(s->buf_end - s->ptr)) {
        bc_read_error_end(s);
        return JS_EXCEPTION;
    }
    memset(&rf, 0, sizeof(rf));
    rf.ctx = s->ctx;
    rf.allow_code = 1;
    rf.readable = QP_NULL;
    rf.ptr = s->ptr;
    rf.end = s->ptr + len;
    v = qp_marshal_read_object(&rf);
    qp_marshal_rfile_free(&rf);
    if (!JS_IsException(v) && rf.ptr != rf.end) {
        JS_FreeValue(s->ctx, v);
        v = JS_ThrowSyntaxError(s->ctx, "invalid Python constant");
    }
    s->ptr += len;
    return v;
}

static const QPMethodDef qp_marshal_methods[] = {
    { "dump", qp_marshal_dump, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dump($module, value, file, version=version, /, *, allow_code=True)\n"
      "--\n"
      "\n"
      "Write the value on the open file.\n"
      "\n"
      "  value\n"
      "    Must be a supported type.\n"
      "  file\n"
      "    Must be a writeable binary file.\n"
      "  version\n"
      "    Indicates the data format that dump should use.\n"
      "  allow_code\n"
      "    Allow to write code objects.\n"
      "\n"
      "If the value has (or contains an object that has) an unsupported type, a\n"
      "ValueError exception is raised - but garbage data will also be written\n"
      "to the file. The object will not be properly read back by load()." },
    { "load", qp_marshal_load, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "load($module, file, /, *, allow_code=True)\n"
      "--\n"
      "\n"
      "Read one value from the open file and return it.\n"
      "\n"
      "  file\n"
      "    Must be readable binary file.\n"
      "  allow_code\n"
      "    Allow to load code objects.\n"
      "\n"
      "If no valid value is read (e.g. because the data has a different Python\n"
      "version's incompatible marshal format), raise EOFError, ValueError or\n"
      "TypeError.\n"
      "\n"
      "Note: If an object containing an unsupported type was marshalled with\n"
      "dump(), load() will substitute None for the unmarshallable type." },
    { "dumps", qp_marshal_dumps, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dumps($module, value, version=version, /, *, allow_code=True)\n"
      "--\n"
      "\n"
      "Return the bytes object that would be written to a file by dump(value, file).\n"
      "\n"
      "  value\n"
      "    Must be a supported type.\n"
      "  version\n"
      "    Indicates the data format that dumps should use.\n"
      "  allow_code\n"
      "    Allow to write code objects.\n"
      "\n"
      "Raise a ValueError exception if value has (or contains an object that\n"
      "has) an unsupported type." },
    { "loads", qp_marshal_loads, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "loads($module, bytes, /, *, allow_code=True)\n"
      "--\n"
      "\n"
      "Convert the bytes-like object to a value.\n"
      "\n"
      "  allow_code\n"
      "    Allow to load code objects.\n"
      "\n"
      "If no valid value is found, raise EOFError, ValueError or TypeError.\n"
      "Extra bytes in the input are ignored." },
    { NULL }
};

static int qp_marshal_exec(JSContext *ctx, JSValueConst module)
{
    JSValue key = qp_str_from_cstr(ctx, "version");
    int r;
    if (JS_IsException(key))
        return -1;
    r = qp_dict_set_item(ctx, qp_module_get_dict(module), key, JS_NewInt32(ctx, QP_MARSHAL_VERSION));
    JS_FreeValue(ctx, key);
    return r;
}

static const QPModuleDef qp_moddef_marshal = {
    "marshal",
    "This module contains functions that can read and write Python values in\n"
    "a binary format. The format is specific to Python, but independent of\n"
    "machine architecture issues.\n"
    "\n"
    "Not all Python object types are supported; in general, only objects\n"
    "whose value is independent from a particular invocation of Python can be\n"
    "written and read by this module. The following types are supported:\n"
    "None, integers, floating-point numbers, strings, bytes, bytearrays,\n"
    "tuples, lists, sets, dictionaries, and code objects, where it\n"
    "should be understood that tuples, lists and dictionaries are only\n"
    "supported as long as the values contained therein are themselves\n"
    "supported; and recursive lists and dictionaries should not be written\n"
    "(they will cause infinite loops).\n"
    "\n"
    "Variables:\n"
    "\n"
    "version -- indicates the format that the module uses. Version 0 is the\n"
    "    historical format, version 1 shares interned strings and version 2\n"
    "    uses a binary format for floating-point numbers.\n"
    "    Version 3 shares common object references (New in version 3.4).\n"
    "\n"
    "Functions:\n"
    "\n"
    "dump() -- write value to a file\n"
    "load() -- read value from a file\n"
    "dumps() -- marshal value as a bytes object\n"
    "loads() -- read value from a bytes-like object",
    qp_marshal_methods,
    qp_marshal_exec,
};
