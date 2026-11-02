/*
 * Per-context state of the M3 native modules that CPython keeps in the
 * interpreter state (PyInterpreterState): atexit callbacks, Python signal
 * handlers, gc configuration, the _warnings filters, the current
 * contextvars Context, the codec registry and the weak reference
 * registry.  Included by python/mod_misc.c, python/mod_weakref.c,
 * python/mod_thread.c, python/mod_codecs.c (one translation unit with
 * quickjs.c; the guard makes repeated inclusion a no-op).
 *
 * QPState.mod_state is NULL until a module first needs it
 * (qp_modstate()).  qp_modstate_mark / qp_modstate_free are called by
 * qp_state_mark / qp_state_free (object.h).
 */
#ifndef QP_MODSTATE_H
#define QP_MODSTATE_H

#include <signal.h>

#ifndef NSIG
#  if defined(_NSIG)
#    define NSIG _NSIG
#  elif defined(_SIGMAX)
#    define NSIG (_SIGMAX + 1)
#  elif defined(SIGMAX)
#    define NSIG (SIGMAX + 1)
#  else
#    define NSIG 64
#  endif
#endif
#define QP_NSIG NSIG

/* ---- weak references (python/mod_weakref.c) ------------------------------
   One record per Python weakref/proxy object (CPython PyWeakReference
   minus the object header).  The record is owned by a hidden holder
   object stored in the weakref's instance slot 0; the holder's finalizer
   (the weakref object dying) unlinks and frees it.  `target` is a raw
   pointer whose JSObject.weakref_count was incremented, so free_object
   calls qp_weakref_object_freed() when it dies (CPython
   PyObject_ClearWeakRefs). */
typedef struct QPWeakRec {
    JSContext *ctx;
    JSObject *target;           /* referent, NULL once cleared */
    JSObject *owner;            /* the Python weakref object (not counted), NULL once it died */
    struct QPWeakRec *prev, *next;   /* per-target list (CPython wr_prev/wr_next) */
    struct QPWeakRec *pending_next;  /* callback queue link */
    uint8_t pending;            /* queued in QPModState.wr_pending */
    uint8_t kind;               /* QP_WR_KIND_* */
    Py_hash_t hash;             /* cached hash of the referent, -1 = not computed */
    JSValue callback;           /* __callback__ (owned, marked by the holder), QP_NULL = none */
    JSValue pending_cb;         /* callback taken when the referent died, waiting in the queue */
} QPWeakRec;

#define QP_WR_KIND_REF       0
#define QP_WR_KIND_PROXY     1
#define QP_WR_KIND_CALLPROXY 2

/* hash bucket: per-target list head */
typedef struct QPWeakBucket {
    JSObject *target;           /* NULL = empty, (JSObject *)1 = deleted */
    QPWeakRec *head;
} QPWeakBucket;

typedef struct QPModState {
    /* atexit (mod_misc.c): list of (func, args tuple, kwargs dict or None) tuples */
    JSValue atexit_callbacks;

    /* _signal (mod_misc.c): Handlers[i].func (QP_NULL = never initialised) */
    JSValue sig_handlers[QP_NSIG];
    JSValue sig_default_int_handler;    /* _signal.default_int_handler */
    JSValue sig_dfl, sig_ign;           /* Handlers sentinels: int 0 / int 1 objects */
    JSValue sig_itimer_error;           /* _signal.ItimerError */
    JSValue sig_siginfo_type;           /* _signal.struct_siginfo (sigwaitinfo platforms), QP_NULL otherwise */
    int sig_wakeup_fd;                  /* -1 = none */
    int sig_wakeup_warn_on_full_buffer;

    /* gc (mod_misc.c) */
    int gc_enabled;
    int gc_debug;
    int gc_threshold[3];
    int gc_collecting;
    Py_ssize_t gc_frozen;
    int64_t gc_collections[3];
    int64_t gc_collected[3];
    int64_t gc_uncollectable[3];
    int64_t gc_candidates[3];
    double gc_duration[3];
    int64_t gc_baseline;                /* tracked-object count after the last collection (get_count) */
    size_t gc_saved_threshold;          /* QuickJS malloc_gc_threshold while gc is disabled */
    JSValue gc_callbacks;               /* gc.callbacks list */
    JSValue gc_garbage;                 /* gc.garbage list */

    /* _warnings (mod_misc.c) */
    JSValue warn_filters;               /* list */
    JSValue warn_once_registry;         /* dict */
    JSValue warn_default_action;        /* str */
    JSValue warn_context;               /* ContextVar for catch_warnings (3.14 _warnings_context) or QP_NULL */
    int64_t warn_filters_version;
    int warn_lock_count;                /* _acquire_lock/_release_lock recursion */

    /* _contextvars (mod_misc.c) */
    JSValue ctx_current;                /* current Context object, QP_NULL = none yet */
    JSValue ctx_types[3];               /* Context, ContextVar, Token type objects (QP_NULL until _contextvars is first used) */

    /* _thread (mod_thread.c) */
    JSValue thread_shutdown_handles;    /* list of non-daemon _ThreadHandle objects (_shutdown) */
    JSValue thread_types[5];            /* lock, RLock, _local, _ThreadHandle, _ExceptHookArgs type objects */

    /* weak references (mod_weakref.c) */
    QPWeakBucket *wr_table;
    uint32_t wr_size;                   /* power of 2, 0 = no table */
    uint32_t wr_count, wr_used;         /* live / live+deleted buckets */
    QPWeakRec *wr_pending_head, *wr_pending_tail;
    JSValue wr_types[3];                /* ReferenceType, ProxyType, CallableProxyType */

    /* _codecs (mod_codecs.c; CPython interp->codecs) */
    JSValue codec_search_path;          /* list of search functions */
    JSValue codec_search_cache;         /* dict normalized name -> CodecInfo */
    JSValue codec_error_registry;       /* dict name -> handler */
    JSValue codec_builtin_handlers[8];  /* strict, ignore, replace, xmlcharrefreplace, backslashreplace, namereplace, surrogatepass, surrogateescape function objects */
    JSValue codec_encoding_map_type;    /* EncodingMap type object */

    /* _ast (mod_ast.c; CPython interp->ast): tuple of the node classes and
       operator singletons, QP_NULL until first used */
    JSValue ast_state;

    /* sys.monitoring (instrumentation.c; CPython interp->monitoring_*) */
    JSValue mon_tool_names[6];          /* str, QP_NULL = tool id free */
    JSValue mon_callbacks;              /* dict (tool_id << 8 | event_id) -> callable */
    JSValue mon_local_events;           /* dict (code, tool_id) -> event set */
    uint32_t mon_events[6];             /* global event set per tool */

    /* asynchronous exception type raised at the next poll (CPython
       tstate->async_exc, _PY_ASYNC_EXCEPTION_BIT), QP_NULL = none */
    JSValue async_exc;
} QPModState;

/* State of ctx, created on first use; NULL with MemoryError pending. */
static QPModState *qp_modstate(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    QPModState *ms = s->mod_state;
    int i;

    if (ms)
        return ms;
    ms = js_mallocz(ctx, sizeof(*ms));
    if (!ms) {
        qp_raise_memory_error(ctx);
        return NULL;
    }
    ms->atexit_callbacks = QP_NULL;
    for (i = 0; i < QP_NSIG; i++)
        ms->sig_handlers[i] = QP_NULL;
    ms->sig_default_int_handler = QP_NULL;
    ms->sig_dfl = QP_NULL;
    ms->sig_ign = QP_NULL;
    ms->sig_itimer_error = QP_NULL;
    ms->sig_siginfo_type = QP_NULL;
    ms->sig_wakeup_fd = -1;
    ms->sig_wakeup_warn_on_full_buffer = 1;
    ms->gc_enabled = 1;
    ms->gc_threshold[0] = 2000;   /* CPython 3.14+ defaults (gc.get_threshold()) */
    ms->gc_threshold[1] = 10;
    ms->gc_threshold[2] = 0;
    ms->gc_callbacks = QP_NULL;
    ms->gc_garbage = QP_NULL;
    ms->warn_filters = QP_NULL;
    ms->warn_once_registry = QP_NULL;
    ms->warn_default_action = QP_NULL;
    ms->warn_context = QP_NULL;
    ms->ctx_current = QP_NULL;
    for (i = 0; i < (int)countof(ms->ctx_types); i++)
        ms->ctx_types[i] = QP_NULL;
    ms->thread_shutdown_handles = QP_NULL;
    for (i = 0; i < (int)countof(ms->thread_types); i++)
        ms->thread_types[i] = QP_NULL;
    for (i = 0; i < (int)countof(ms->wr_types); i++)
        ms->wr_types[i] = QP_NULL;
    ms->codec_search_path = QP_NULL;
    ms->codec_search_cache = QP_NULL;
    ms->codec_error_registry = QP_NULL;
    for (i = 0; i < 8; i++)
        ms->codec_builtin_handlers[i] = QP_NULL;
    ms->codec_encoding_map_type = QP_NULL;
    ms->ast_state = QP_NULL;
    for (i = 0; i < (int)countof(ms->mon_tool_names); i++)
        ms->mon_tool_names[i] = QP_NULL;
    ms->mon_callbacks = QP_NULL;
    ms->mon_local_events = QP_NULL;
    ms->async_exc = QP_NULL;
    s->mod_state = ms;
    return ms;
}

static void qp_modstate_mark_value(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark_func)
{
    if (!QP_IS_NULL(v))
        JS_MarkValue(rt, v, mark_func);
}

static void qp_modstate_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func)
{
    QPModState *ms = s->mod_state;
    QPWeakRec *r;
    int i;

    if (!ms)
        return;
    /* the callbacks waiting in the queue belong to the context: a context
       that dies before they run (no Python code ran after their referents
       died) must not be kept alive by them */
    for (r = ms->wr_pending_head; r; r = r->pending_next)
        qp_modstate_mark_value(rt, r->pending_cb, mark_func);
    qp_modstate_mark_value(rt, ms->atexit_callbacks, mark_func);
    for (i = 0; i < QP_NSIG; i++)
        qp_modstate_mark_value(rt, ms->sig_handlers[i], mark_func);
    qp_modstate_mark_value(rt, ms->sig_default_int_handler, mark_func);
    qp_modstate_mark_value(rt, ms->sig_dfl, mark_func);
    qp_modstate_mark_value(rt, ms->sig_ign, mark_func);
    qp_modstate_mark_value(rt, ms->sig_itimer_error, mark_func);
    qp_modstate_mark_value(rt, ms->sig_siginfo_type, mark_func);
    qp_modstate_mark_value(rt, ms->gc_callbacks, mark_func);
    qp_modstate_mark_value(rt, ms->gc_garbage, mark_func);
    qp_modstate_mark_value(rt, ms->warn_filters, mark_func);
    qp_modstate_mark_value(rt, ms->warn_once_registry, mark_func);
    qp_modstate_mark_value(rt, ms->warn_default_action, mark_func);
    qp_modstate_mark_value(rt, ms->warn_context, mark_func);
    qp_modstate_mark_value(rt, ms->ctx_current, mark_func);
    for (i = 0; i < (int)countof(ms->ctx_types); i++)
        qp_modstate_mark_value(rt, ms->ctx_types[i], mark_func);
    qp_modstate_mark_value(rt, ms->thread_shutdown_handles, mark_func);
    for (i = 0; i < (int)countof(ms->thread_types); i++)
        qp_modstate_mark_value(rt, ms->thread_types[i], mark_func);
    for (i = 0; i < (int)countof(ms->wr_types); i++)
        qp_modstate_mark_value(rt, ms->wr_types[i], mark_func);
    qp_modstate_mark_value(rt, ms->codec_search_path, mark_func);
    qp_modstate_mark_value(rt, ms->codec_search_cache, mark_func);
    qp_modstate_mark_value(rt, ms->codec_error_registry, mark_func);
    for (i = 0; i < 8; i++)
        qp_modstate_mark_value(rt, ms->codec_builtin_handlers[i], mark_func);
    qp_modstate_mark_value(rt, ms->codec_encoding_map_type, mark_func);
    qp_modstate_mark_value(rt, ms->ast_state, mark_func);
    for (i = 0; i < (int)countof(ms->mon_tool_names); i++)
        qp_modstate_mark_value(rt, ms->mon_tool_names[i], mark_func);
    qp_modstate_mark_value(rt, ms->mon_callbacks, mark_func);
    qp_modstate_mark_value(rt, ms->mon_local_events, mark_func);
    qp_modstate_mark_value(rt, ms->async_exc, mark_func);
}

static void qp_modstate_free_value(JSContext *ctx, JSValue *pv)
{
    JSValue v = *pv;
    *pv = QP_NULL;
    if (!QP_IS_NULL(v))
        JS_FreeValue(ctx, v);
}

/* Release the state.  Weak records still attached to live targets give
   back their weakref_count (the targets die after the state, when nothing
   would clear them) and are detached from the state; their holders free
   them.  Orphan records waiting in the callback queue are freed here. */
static void qp_modstate_free(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    QPModState *ms = s->mod_state;
    uint32_t i;
    QPWeakRec *r, *next;

    if (!ms)
        return;
    for (i = 0; i < ms->wr_size; i++) {
        QPWeakBucket *b = &ms->wr_table[i];
        if (b->target == NULL || b->target == (JSObject *)1)
            continue;
        for (r = b->head; r; r = next) {
            next = r->next;
            r->prev = r->next = NULL;
            if (r->target) {
                r->target->weakref_count--;
                r->target = NULL;
            }
        }
    }
    js_free(ctx, ms->wr_table);
    ms->wr_table = NULL;
    ms->wr_size = 0;
    for (r = ms->wr_pending_head; r; r = next) {
        next = r->pending_next;
        r->pending_next = NULL;
        r->pending = 0;
        if (!r->owner) {
            /* orphan: its holder already died */
            if (!QP_IS_NULL(r->callback))
                JS_FreeValue(ctx, r->callback);
            if (!QP_IS_NULL(r->pending_cb))
                JS_FreeValue(ctx, r->pending_cb);
            js_free_rt(ctx->rt, r);
        } else if (!QP_IS_NULL(r->pending_cb)) {
            /* the callback will not run: released with the queue (the
               state marked it) */
            JSValue cb = r->pending_cb;
            r->pending_cb = QP_NULL;
            JS_FreeValue(ctx, cb);
        }
    }
    ms->wr_pending_head = ms->wr_pending_tail = NULL;

    qp_modstate_free_value(ctx, &ms->atexit_callbacks);
    for (i = 0; i < QP_NSIG; i++)
        qp_modstate_free_value(ctx, &ms->sig_handlers[i]);
    qp_modstate_free_value(ctx, &ms->sig_default_int_handler);
    qp_modstate_free_value(ctx, &ms->sig_dfl);
    qp_modstate_free_value(ctx, &ms->sig_ign);
    qp_modstate_free_value(ctx, &ms->sig_itimer_error);
    qp_modstate_free_value(ctx, &ms->sig_siginfo_type);
    qp_modstate_free_value(ctx, &ms->gc_callbacks);
    qp_modstate_free_value(ctx, &ms->gc_garbage);
    qp_modstate_free_value(ctx, &ms->warn_filters);
    qp_modstate_free_value(ctx, &ms->warn_once_registry);
    qp_modstate_free_value(ctx, &ms->warn_default_action);
    qp_modstate_free_value(ctx, &ms->warn_context);
    qp_modstate_free_value(ctx, &ms->ctx_current);
    for (i = 0; i < countof(ms->ctx_types); i++)
        qp_modstate_free_value(ctx, &ms->ctx_types[i]);
    qp_modstate_free_value(ctx, &ms->thread_shutdown_handles);
    for (i = 0; i < countof(ms->thread_types); i++)
        qp_modstate_free_value(ctx, &ms->thread_types[i]);
    for (i = 0; i < countof(ms->wr_types); i++)
        qp_modstate_free_value(ctx, &ms->wr_types[i]);
    qp_modstate_free_value(ctx, &ms->codec_search_path);
    qp_modstate_free_value(ctx, &ms->codec_search_cache);
    qp_modstate_free_value(ctx, &ms->codec_error_registry);
    for (i = 0; i < 8; i++)
        qp_modstate_free_value(ctx, &ms->codec_builtin_handlers[i]);
    qp_modstate_free_value(ctx, &ms->codec_encoding_map_type);
    qp_modstate_free_value(ctx, &ms->ast_state);
    for (i = 0; i < countof(ms->mon_tool_names); i++)
        qp_modstate_free_value(ctx, &ms->mon_tool_names[i]);
    qp_modstate_free_value(ctx, &ms->mon_callbacks);
    qp_modstate_free_value(ctx, &ms->mon_local_events);
    qp_modstate_free_value(ctx, &ms->async_exc);
    s->mod_state = NULL;
    js_free(ctx, ms);
}

#endif /* QP_MODSTATE_H */
