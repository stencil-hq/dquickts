/*
 * dquickts Python, milestone M2: private declarations of python/ceval.c
 * used by the OP_py handlers (python/ceval_ops.h, included inside
 * JS_CallInternal) and by the quickjs.c integration.  Included by
 * quickjs.c right after python/object.h.
 */
#ifndef QP_CEVAL_H
#define QP_CEVAL_H

struct JSStackFrame;
struct JSFunctionBytecode;

/* allocate the inline caches of a new Python bytecode (kinds from the
   IC operands of its subops); 0 or -1 (exception pending) */
static int qp_code_info_setup_ic(JSContext *ctx, struct JSFunctionBytecode *b);

static JSValue qp_load_common_const(JSContext *ctx, struct JSStackFrame *sf, int id);
static JSValue qp_ceval_load_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                    QPGlobalCache *ic);
static int qp_ceval_store_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                 JSValueConst v);
static int qp_ceval_delete_global(JSContext *ctx, JSValueConst func, JSAtom atom);
/* mapping[str(atom)] without KeyError: 1 (*pv new ref) / 0 / -1 */
static int qp_ceval_mapping_get(JSContext *ctx, JSValueConst mapping, JSAtom atom,
                                JSValue *pv);
static JSValue qp_ceval_load_name(JSContext *ctx, JSValueConst func, JSValueConst locals,
                                  JSAtom atom);
static int qp_ceval_store_name(JSContext *ctx, JSValueConst locals, JSAtom atom,
                               JSValueConst v);
static int qp_ceval_delete_name(JSContext *ctx, JSValueConst locals, JSAtom atom);
static JSValue qp_ceval_call_ex(JSContext *ctx, JSValueConst callable, JSValueConst self,
                                JSValueConst args, JSValueConst kwargs);
static int qp_ceval_list_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable);
static int qp_ceval_dict_update(JSContext *ctx, JSValueConst dict, JSValueConst update);
static JSValue qp_ceval_with_except_start(JSContext *ctx, JSValueConst exit_func,
                                          JSValueConst self, JSValueConst exc);
static JSValue qp_ceval_load_build_class(JSContext *ctx, JSValueConst func);
static int qp_ceval_setup_annotations(JSContext *ctx, JSValueConst locals);
static JSValue qp_ceval_import_name(JSContext *ctx, JSValueConst func,
                                    struct JSFunctionBytecode *b, JSValue *arg_buf,
                                    JSAtom name, JSValueConst level,
                                    JSValueConst fromlist, int mode);
static JSValue qp_ceval_intrinsic1(JSContext *ctx, struct JSStackFrame *sf,
                                   struct JSFunctionBytecode *b, JSValue *arg_buf,
                                   int id, JSValueConst v);
static JSValue qp_ceval_intrinsic2(JSContext *ctx, int id, JSValueConst a, JSValueConst b);

/* python/unicodeobject.c (unit d): releases QPState.str_cache */
static void qp_str_state_free(JSContext *ctx);
/* M3 (python/ modules state, signals, weakrefs; owner M3Misc) */
static void qp_modstate_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func);
static void qp_modstate_free(JSContext *ctx);
static void qp_weakref_object_freed(JSRuntime *rt, struct JSObject *p);
static int qp_signal_check(JSContext *ctx);

/* ---- python/ceval.c "Threads and the GIL" (M5) ---------------------------
   Types shared by ceval.c, mod_thread.c, sysmodule.c and the C API
   (python/capi_runtime.c); JSThreadExecState is defined by quickjs.c. */
#include <pthread.h>

#define QP_GIL_DEFAULT_INTERVAL_US 5000

/* sys.setswitchinterval (microseconds, CPython gil->interval); process-wide */
static unsigned long qp_gil_interval_us = QP_GIL_DEFAULT_INTERVAL_US;

typedef struct QPThreadState {
    struct list_head link;          /* QPThreads.tstates */
    struct QPThreads *thr;
    uint64_t ident;                 /* (uintptr_t)pthread_t: _thread.get_ident() */
    uint64_t serial;                /* unique, never reused (0: first main thread) */
    /* execution state, valid while another thread holds the GIL */
    JSThreadExecState js;
    int recursion_depth;
    uint8_t reraise;
    QPExcStackItem exc_info_base;
    QPExcStackItem *exc_info;
    JSValue repr_stack;
    JSValue asyncgen_firstiter, asyncgen_finalizer;
    int coro_origin_depth;
    JSValue context;                /* QPModState.ctx_current */
    JSValue trace_func, profile_func;   /* sys.settrace / sys.setprofile (per thread, like CPython) */
    uint8_t trace_active, tracing;
    JSValue async_exc;              /* QPModState.async_exc (tstate->async_exc) */
    /* owned by the thread state */
    JSValue local_refs;             /* list of weakrefs to the _thread._local objects holding a dict of this thread, QP_NULL */
    /* thread started by _thread (mod_thread.c), released once it ran */
    JSValue func, args, kwargs;     /* kwargs QP_NULL: none */
    JSValue handle;                 /* its _ThreadHandle, QP_NULL: none */
    /* C API (python/capi_runtime.c): the thread's own PyThreadState and
       the one current while it holds the GIL (NULL: detached) */
    void *capi_bound;
    void *capi_tstate;
    BOOL foreign;                   /* created by PyGILState_Ensure (libc memory) */
} QPThreadState;

/* C API thread states (python/capi_runtime.c sets the hook when the C API
   is initialized; NULL otherwise) */
enum {
    QP_TSTATE_SAVE,                 /* ts releases the GIL */
    QP_TSTATE_RESTORE,              /* ts took the GIL (called on ts's thread) */
    QP_TSTATE_START,                /* thread started by _thread, first run */
    QP_TSTATE_EXIT,                 /* ts is deleted (exit, or dead after fork) */
    QP_TSTATE_FORK_CHILD,           /* ts is the only (main) thread of a forked child
                                       (NULL: no thread was ever started) */
};
static void (*qp_tstate_capi_hook)(struct QPThreadState *ts, int event);

/* the thread state of the calling OS thread, once it held the GIL */
static _Thread_local struct QPThreadState *qp_tstate_tls;

/* the interpreter's recursive mutexes (CPython _PyRecursiveMutex) */
enum {
    QP_RMUTEX_IMPORT,               /* _imp.acquire_lock (level: QPState.import_lock_level) */
    QP_RMUTEX_WARNINGS,             /* _warnings lock (level: QPModState.warn_lock_count) */
    QP_RMUTEX__COUNT
};

typedef struct QPThreads {
    JSRuntime *rt;
    JSContext *ctx;                 /* the Python context of the threads */
    QPThreadState *main_ts;         /* main thread (signal handlers, suspension) */
    QPThreadState *current;         /* thread holding the GIL, NULL while released */
    struct list_head tstates;       /* every thread state, main included */
    int count;                      /* running threads started by _thread (_count()) */
    uint64_t next_serial;
    BOOL finalizing;                /* qp_threads_finalize() called */
    QPThreadState *finalizing_ts;
    /* owners of the recursive mutexes, NULL while unlocked or locked by
       the main thread before the threads existed */
    QPThreadState *rmutex_owner[QP_RMUTEX__COUNT];

    /* the GIL (CPython struct _gil_runtime_state) */
    pthread_mutex_t mutex;
    pthread_cond_t cond;            /* signalled when the GIL is released */
    pthread_mutex_t switch_mutex;
    pthread_cond_t switch_cond;     /* signalled when the GIL changes hands */
    int locked;
    unsigned long switch_number;
    QPThreadState *last_holder;
    int drop_request;               /* atomic */

    /* waits of lock objects and handles (qp_thread_wait) */
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_cond;
    uint64_t wait_gen;              /* changed under the GIL and wait_mutex */
    int nwaiters;                   /* under the GIL */
} QPThreads;

/* stack of the threads started by _thread / PyThread_start_new_thread:
   _thread.stack_size() (python/mod_thread.c), 0: QP_THREAD_DEFAULT_STACK
   (CPython's macOS THREAD_STACK_SIZE, used on every platform: the pthread
   default is 512 KiB on macOS) */
static size_t qp_thread_stacksize;
#define QP_THREAD_DEFAULT_STACK ((size_t)16 * 1024 * 1024)

/* periodic poll while JSRuntime.py_threads is set: hand the GIL over on
   a drop request */
static void qp_gil_poll(JSContext *ctx);
/* the running thread is the main thread (TRUE with a single thread) */
static BOOL qp_thread_is_main_rt(JSRuntime *rt);
/* Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS around a blocking call:
   the code in between must not touch the runtime (no JSValue, no
   js_malloc).  No-ops while the runtime has a single Python thread. */
static QPThreadState *qp_gil_release(JSContext *ctx);
static void qp_gil_acquire(JSContext *ctx, QPThreadState *ts);
#define QP_BEGIN_ALLOW_THREADS(ctx) \
    { QPThreadState *qp_gil_saved_ = qp_gil_release(ctx);
#define QP_END_ALLOW_THREADS(ctx) \
      qp_gil_acquire((ctx), qp_gil_saved_); }
static uint64_t qp_thread_self_ident(void);   /* (uintptr_t)pthread_self() */
static QPThreads *qp_threads_get(JSContext *ctx);
static void qp_threads_free(JSContext *ctx);
static int qp_threads_finalize(JSContext *ctx);
static QPThreadState *qp_tstate_new(QPThreads *thr, uintptr_t stack_size);
static void qp_thread_sync_init(QPThreads *thr);
static void qp_gil_take(QPThreads *thr, QPThreadState *ts);
static void qp_gil_drop(QPThreads *thr, QPThreadState *ts);
static void qp_gil_switch_in(QPThreads *thr, QPThreadState *ts);
static QPThreadState *qp_thread_current(JSContext *ctx);
static uint64_t qp_thread_serial(JSContext *ctx);
static uint64_t qp_thread_main_ident(JSContext *ctx);
static BOOL qp_thread_others_exist(JSContext *ctx);
/* waits on objects changed under the GIL: 1 ready, 0 timed out, -1
   exception (see ceval.c) */
typedef BOOL QPThreadReadyFunc(JSContext *ctx, JSValueConst obj);
static int qp_thread_wait(JSContext *ctx, QPThreadReadyFunc *ready, JSValueConst obj,
                          int64_t timeout_ns, BOOL handle_signals);
static void qp_thread_notify(JSContext *ctx);
static int *qp_rmutex_level_ptr(JSContext *ctx, int which);
static QPThreadState *qp_thread_attach(JSContext *ctx);
static void qp_thread_detach_foreign(JSContext *ctx, QPThreadState *ts,
                                     void (*clear)(JSContext *ctx, QPThreadState *ts));
static void qp_rmutex_lock(JSContext *ctx, int which);
static int qp_rmutex_unlock(JSContext *ctx, int which);
/* python/mod_thread.c, python/mod_misc.c: os.fork() child */
static void qp_thread_after_fork_child(JSContext *ctx);
static void qp_signal_after_fork_child(void);

/* python/getargs.c (unit k): _Py_CalculateSuggestions over C strings */
static int qp_suggest_keyword(const char *name, const char *const *cands, int ncands);

#endif /* QP_CEVAL_H */
