/*
 * dquickts Python C API (plan D5, Tier S), unit E2: runtime services.
 *
 * Thread and interpreter state (single-threaded M3 semantics, capi-design.md
 * "GIL and thread state"), PyEval_*, recursion guard, pending calls,
 * pythread.h locks over pthreads, PyMutex / PyEvent / once flags / parking
 * lot, critical sections (GIL build: no-ops), PyImport_*, PySys_*, audit
 * hooks, lifecycle (Py_AtExit, Py_Exit, Py_FatalError, PyStatus), pytime,
 * the internal hash table, contextvars / generator helpers, the CODE mirror
 * kind with the PyCode / PyFrame API, file and OS utilities.
 *
 * Several sections are ports of CPython 3.15.2 (Python/pytime.c,
 * Python/hashtable.c, Python/mystrtoul.c, Python/pystrcmp.c, parts of
 * Python/lock.c, Python/thread.c, Python/sysmodule.c, Python/ceval.c,
 * Python/fileutils.c); PSF-2.0.
 *
 * Included into the quickjs.c translation unit after python/capi.h (see the
 * include block of capi.h).  Exported C API functions are non-static;
 * everything else is static and named qp_capi_*.
 */

#include "../capi/compat/pycore_qp_tstate.h"

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/random.h>      /* getentropy() */
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "modstate.h"

/* TU-static helpers of other python/ *.c files (defined earlier in the
   quickjs.c translation unit; declared here for the unit syntax check). */
static JSValue qp_run_source(JSContext *ctx, const char *src, size_t len, const char *filename,
                             int mode, int flags, JSValueConst globals, JSValueConst locals);
static void qp_run_flush_io(JSContext *ctx);
static int qp_import_get_module(JSContext *ctx, JSValueConst name, JSValue *pmod);
static JSValue qp_import_module_level(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      JSValueConst locals, JSValueConst fromlist, int level);
static JSValue qp_sys_getsizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic);
static QPModState *qp_ctxvar_state(JSContext *ctx);
static QPType *qp_ctxvar_type(QPModState *ms, int which);
static JSValue qp_context_new_from_vars(JSContext *ctx, QPModState *ms, JSValueConst vars);
static JSValue qp_contextvar_new(JSContext *ctx, const char *name, JSValueConst dflt);
static int qp_contextvar_get(JSContext *ctx, JSValueConst var, JSValueConst dflt, JSValue *pval);
static JSValue qp_contextvar_set(JSContext *ctx, JSValueConst var, JSValueConst val);
static int qp_contextvar_reset(JSContext *ctx, JSValueConst var, JSValueConst token);
static JSValue qp_context_copy_current(JSContext *ctx);
static int qp_context_enter(JSContext *ctx, JSValueConst context);
static int qp_context_exit(JSContext *ctx, JSValueConst context);
static void qp_os_before_fork(JSContext *ctx);
static void qp_os_after_fork(JSContext *ctx, int child);
static void (*qp_signal_setsig(int sig, void (*handler)(int)))(int);
static void (*qp_signal_getsig(int sig))(int);
/* python/mod_misc.c: tripped flags of the C signal handler (tentative
   definition, merged with mod_misc.c's) */
static volatile sig_atomic_t qp_signal_tripped[QP_NSIG];
/* python/mod_misc.c QP_CTXVAR_T_*: indices of QPModState.ctx_types */
#define QP_CAPI_CTXVAR_T_CONTEXT 0
#define QP_CAPI_CTXVAR_T_VAR     1
#define QP_CAPI_CTXVAR_T_TOKEN   2

/* ========================================================================= */
/* Common helpers                                                             */
/* ========================================================================= */

/* Pin owner of the borrowed references handed out by this unit
   (PyEval_GetBuiltins/Globals/Locals/Frame, PyImport_GetModuleDict,
   PyImport_AddModule*, PySys_GetObject, PySys_GetXOptions, ...): a
   J-backed dict mirror kept alive for the life of the C API. */
static PyObject *qp_capi_rt_holder;

/* Main thread of the C-API interpreter (PyThread_get_thread_ident()). */
static unsigned long qp_capi_main_thread;
/* Set by Py_Exit before the exit handlers run (Py_IsFinalizing). */
static int qp_capi_finalizing;

/* Pin key of a named borrowed slot: FNV-1a of "tag\0name". */
static uint64_t qp_capi_rt_key(const char *tag, const char *name, size_t name_len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    const unsigned char *p;
    size_t i;

    for (p = (const unsigned char *)tag; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    h ^= 0;
    h *= 0x100000001b3ULL;
    p = (const unsigned char *)name;
    for (i = 0; i < name_len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return QP_CAPI_PIN_HASHED(h);
}

static uint64_t qp_capi_rt_key_str(const char *tag, const char *name)
{
    return qp_capi_rt_key(tag, name, strlen(name));
}

/* Borrowed mirror of v pinned in the holder under key (NULL + exception). */
static PyObject *qp_capi_rt_borrow(uint64_t key, JSValueConst v)
{
    return qp_capi_borrow(qp_capi.ctx, qp_capi_rt_holder, key, v);
}

/* JS value of an optional PyObject* argument: QP_NULL for NULL. */
static JSValue qp_capi_rt_opt(PyObject *op)
{
    if (op == NULL)
        return QP_NULL;
    return qp_capi_value(qp_capi.ctx, op);
}

/* New attribute value of a JS value by C name (NULL + exception). */
static PyObject *qp_capi_rt_getattr(JSContext *ctx, JSValueConst v, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;

    if (a == JS_ATOM_NULL)
        return NULL;
    r = qp_getattr(ctx, v, a);
    JS_FreeAtom(ctx, a);
    return qp_capi_from_js_result(ctx, r);
}

static int qp_capi_rt_is_main_thread(void)
{
    return PyThread_get_thread_ident() == qp_capi_main_thread;
}

/* The PyThreadState bound to the calling OS thread (CPython's gilstate
   TSS): the main thread's, a _thread thread's, or one created by
   PyGILState_Ensure in a foreign thread.  Set by the thread itself
   (QP_TSTATE_RESTORE / START hooks, PyGILState_Ensure). */
static _Thread_local PyThreadState *qp_capi_tls_tstate;

static void _Py_NO_RETURN qp_capi_fatal_error(const char *func, const char *msg);

/* ========================================================================= */
/* Thread state and interpreter state                                         */
/* ========================================================================= */

/* The module-visible _PyThreadStateImpl (capi/compat/pycore_qp_tstate.h)
   MUST stay the first member: PyThreadState* <-> _PyThreadStateImpl*
   <-> QPCapiThreadState* are the same pointer. */
struct QPCapiThreadState {
    _PyThreadStateImpl impl;
    /* M5 GIL (python/ceval.c): the runtime thread state saved by
       PyEval_SaveThread (NULL while attached, or before the first save),
       and the one of a foreign thread created for this tstate */
    struct QPThreadState *qp_ts;
    struct QPThreadState *own_ts;
};

static PyInterpreterState qp_capi_interp_storage;
static uint64_t qp_capi_next_tstate_id = 1;
/* CPython HEAD_LOCK: the thread state list may change without the GIL
   (PyThreadState_New in a foreign thread) */
static pthread_mutex_t qp_capi_head_lock = PTHREAD_MUTEX_INITIALIZER;

static void qp_capi_llist_init(struct llist_node *head)
{
    head->next = head;
    head->prev = head;
}

static unsigned long qp_capi_native_thread_id(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    (void)pthread_threadid_np(NULL, &tid);
    return (unsigned long)tid;
#elif defined(__linux__)
    return (unsigned long)syscall(SYS_gettid);
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

/* New thread state linked at the head of interp's list; NULL on OOM (no
   exception: callers decide). */
static struct QPCapiThreadState *qp_capi_tstate_alloc(PyInterpreterState *interp)
{
    struct QPCapiThreadState *ts = PyMem_RawCalloc(1, sizeof(*ts));
    PyThreadState *t;
    int limit;

    if (!ts)
        return NULL;
    t = &ts->impl.base;
    t->interp = interp;
    t->thread_id = PyThread_get_thread_ident();
    t->native_thread_id = qp_capi_native_thread_id();
    t->_status.initialized = 1;
    t->_status.bound = 1;
    t->_whence = 4;  /* _PyThreadState_WHENCE_C_API */
    limit = Py_GetRecursionLimit();
    t->py_recursion_limit = limit;
    t->py_recursion_remaining = limit;
    qp_capi_llist_init(&ts->impl.asyncio_tasks_head);
    pthread_mutex_lock(&qp_capi_head_lock);
    t->id = qp_capi_next_tstate_id++;
    t->next = interp->threads.head;
    if (t->next)
        t->next->prev = t;
    interp->threads.head = t;
    pthread_mutex_unlock(&qp_capi_head_lock);
    return ts;
}

static void qp_capi_tstate_clear(PyThreadState *t)
{
    _PyThreadStateImpl *impl = (_PyThreadStateImpl *)t;
    Py_CLEAR(t->dict);
    Py_CLEAR(t->async_exc);
    Py_CLEAR(t->context);
    Py_CLEAR(t->async_gen_firstiter);
    Py_CLEAR(t->async_gen_finalizer);
    Py_CLEAR(t->c_profileobj);
    Py_CLEAR(t->c_traceobj);
    t->c_profilefunc = NULL;
    t->c_tracefunc = NULL;
    Py_CLEAR(impl->asyncio_running_loop);
    Py_CLEAR(impl->asyncio_running_task);
    t->_status.cleared = 1;
}

static void qp_capi_tstate_unlink(PyThreadState *t)
{
    PyInterpreterState *interp = t->interp;
    pthread_mutex_lock(&qp_capi_head_lock);
    if (t->prev)
        t->prev->next = t->next;
    else if (interp && interp->threads.head == t)
        interp->threads.head = t->next;
    if (t->next)
        t->next->prev = t->prev;
    t->prev = t->next = NULL;
    pthread_mutex_unlock(&qp_capi_head_lock);
}

#define QP_CAPI_NO_TSTATE_MSG \
    "the function must be called with the GIL held, after Python " \
    "initialization and before Python finalization, but the GIL is " \
    "released (the current Python thread state is NULL)"

#define QP_CAPI_TS(t) ((struct QPCapiThreadState *)(t))

/* Does the calling thread hold the GIL (python/ceval.c)? */
static int qp_capi_holds_gil(void)
{
    QPThreads *thr = qp_capi.ctx ? qp_capi.ctx->rt->py_threads : NULL;
    QPThreadState *cur;
    if (!thr)
        return qp_capi_rt_is_main_thread();
    cur = __atomic_load_n(&thr->current, __ATOMIC_RELAXED);
    return cur != NULL && cur == qp_tstate_tls;
}

/* C-API thread state of a runtime thread state ts (created on first use,
   gilstate_counter 1 like CPython's bound thread states); NULL on memory
   error. */
static PyThreadState *qp_capi_tstate_bind(QPThreadState *ts)
{
    struct QPCapiThreadState *c;
    if (ts->capi_bound)
        return (PyThreadState *)ts->capi_bound;
    c = qp_capi_tstate_alloc(qp_capi.interp);
    if (!c)
        return NULL;
    c->impl.base.thread_id = (unsigned long)ts->ident;
    c->impl.base.gilstate_counter = 1;
    c->impl.base._status.active = 1;
    ts->capi_bound = &c->impl.base;
    return &c->impl.base;
}

/* PyThreadState_Clear + unlink + free (not current) */
static void qp_capi_tstate_free(PyThreadState *t)
{
    if (!t->_status.cleared)
        qp_capi_tstate_clear(t);
    qp_capi_tstate_unlink(t);
    PyMem_RawFree(t);
}

/* python/ceval.c QP_TSTATE_* hook: qp_capi.tstate_current is the GIL
   holder's and moves with the GIL like the rest of the thread state. */
static void qp_capi_thread_hook(QPThreadState *ts, int event)
{
    PyThreadState *t;
    /* the hook is process-wide: the threads of the runtimes the C API is
       not bound to have no C thread state */
    if (!qp_capi_owned_by(ts->thr->rt))
        return;
    switch (event) {
    case QP_TSTATE_SAVE:
        ts->capi_tstate = qp_capi.tstate_current;
        break;
    case QP_TSTATE_RESTORE:
        qp_capi.tstate_current = (PyThreadState *)ts->capi_tstate;
        qp_capi_tls_tstate = (PyThreadState *)ts->capi_bound;
        break;
    case QP_TSTATE_START:
        t = qp_capi_tstate_bind(ts);
        ts->capi_tstate = t;
        qp_capi.tstate_current = t;
        qp_capi_tls_tstate = t;
        break;
    case QP_TSTATE_EXIT:
        t = (PyThreadState *)ts->capi_bound;
        ts->capi_bound = ts->capi_tstate = NULL;
        if (t == NULL || (qp_capi.main_tstate && t == &qp_capi.main_tstate->impl.base))
            break;
        if (qp_capi.tstate_current == t)
            qp_capi.tstate_current = NULL;
        if (qp_capi_tls_tstate == t)
            qp_capi_tls_tstate = NULL;
        qp_capi_tstate_free(t);
        break;
    case QP_TSTATE_FORK_CHILD:
        /* PyOS_AfterFork_Child: the forking thread is the main thread (ts
           NULL: no other runtime thread ever existed, the thread states
           stay) */
        if (ts) {
            qp_capi_main_thread = (unsigned long)ts->ident;
            t = qp_capi_tstate_bind(ts);
            if (t) {
                t->thread_id = qp_capi_main_thread;
                t->native_thread_id = qp_capi_native_thread_id();
                qp_capi.main_tstate = QP_CAPI_TS(t);
            }
        }
        /* reset_asyncio_state: the child runs no loop and no task; like
           CPython the references are dropped without being released (no
           finalizer runs inside fork()) */
        t = qp_capi.tstate_current;
        if (t) {
            _PyThreadStateImpl *impl = (_PyThreadStateImpl *)t;
            qp_capi_llist_init(&impl->asyncio_tasks_head);
            impl->asyncio_running_loop = NULL;
            impl->asyncio_running_task = NULL;
        }
        break;
    }
}

PyThreadState *PyThreadState_GetUnchecked(void)
{
    return qp_capi.tstate_current;
}

PyThreadState *PyThreadState_Get(void)
{
    PyThreadState *t = qp_capi.tstate_current;
    if (t == NULL)
        _Py_FatalErrorFunc("PyThreadState_Get", QP_CAPI_NO_TSTATE_MSG);
    return t;
}

/* GIL build: detach the current thread state (release the GIL), attach
   newts (take the GIL) */
PyThreadState *PyThreadState_Swap(PyThreadState *newts)
{
    PyThreadState *old = qp_capi_holds_gil() ? qp_capi.tstate_current : NULL;
    if (old == newts)
        return old;
    if (old != NULL)
        (void)PyEval_SaveThread();
    if (newts != NULL)
        PyEval_RestoreThread(newts);
    return old;
}

PyThreadState *PyThreadState_New(PyInterpreterState *interp)
{
    struct QPCapiThreadState *ts;
    if (interp == NULL)
        interp = qp_capi.interp;
    if (interp == NULL)
        return NULL;
    ts = qp_capi_tstate_alloc(interp);
    return ts ? &ts->impl.base : NULL;
}

void PyThreadState_Clear(PyThreadState *tstate)
{
    if (tstate == NULL)
        return;
    qp_capi_tstate_clear(tstate);
}

void PyThreadState_Delete(PyThreadState *tstate)
{
    if (tstate == NULL)
        _Py_FatalErrorFunc("PyThreadState_Delete", "NULL tstate");
    if (tstate == qp_capi.tstate_current)
        _Py_FatalErrorFunc("PyThreadState_Delete", "tstate is still current");
    if (qp_capi.main_tstate && tstate == &qp_capi.main_tstate->impl.base)
        _Py_FatalErrorFunc("PyThreadState_Delete", "cannot delete the main thread state");
    {
        QPThreadState *own = QP_CAPI_TS(tstate)->own_ts;
        if (own && own->capi_bound == tstate)
            own->capi_bound = NULL;
    }
    qp_capi_tstate_free(tstate);
}

/* python/mod_thread.c: release the Python-level state of a thread */
static void qp_thread_tstate_clear(JSContext *ctx, QPThreadState *ts);

/* _PyThreadState_DeleteCurrent: delete t and release the GIL */
static void qp_capi_delete_current(PyThreadState *t)
{
    QPThreadState *own = QP_CAPI_TS(t)->own_ts;
    QPThreadState *qts = qp_tstate_tls;

    PyThreadState_Delete(t);
    if (own && own == qts) {
        /* the foreign thread is done with Python */
        if (qp_capi_tls_tstate == t)
            qp_capi_tls_tstate = NULL;
        own->capi_bound = own->capi_tstate = NULL;
        qp_thread_detach_foreign(qp_capi.ctx, own, qp_thread_tstate_clear);
    } else {
        (void)qp_gil_release(qp_capi.ctx);
    }
}

void PyThreadState_DeleteCurrent(void)
{
    PyThreadState *t = qp_capi.tstate_current;
    if (t == NULL)
        _Py_FatalErrorFunc("PyThreadState_DeleteCurrent", QP_CAPI_NO_TSTATE_MSG);
    qp_capi.tstate_current = NULL;
    qp_capi_delete_current(t);
}

PyObject *PyThreadState_GetDict(void)
{
    PyThreadState *t = qp_capi.tstate_current;
    if (t == NULL)
        return NULL;
    if (t->dict == NULL) {
        t->dict = PyDict_New();
        if (t->dict == NULL)
            PyErr_Clear();
    }
    return t->dict;
}

PyInterpreterState *PyThreadState_GetInterpreter(PyThreadState *tstate)
{
    return tstate->interp;
}

uint64_t PyThreadState_GetID(PyThreadState *tstate)
{
    return tstate->id;
}

PyThreadState *PyThreadState_Next(PyThreadState *tstate)
{
    return tstate->next;
}

void PyThreadState_EnterTracing(PyThreadState *tstate)
{
    tstate->tracing++;
}

void PyThreadState_LeaveTracing(PyThreadState *tstate)
{
    tstate->tracing--;
}

PyInterpreterState *PyInterpreterState_Get(void)
{
    PyThreadState *t = qp_capi.tstate_current;
    if (t == NULL)
        _Py_FatalErrorFunc("PyInterpreterState_Get", QP_CAPI_NO_TSTATE_MSG);
    return t->interp;
}

PyInterpreterState *PyInterpreterState_Main(void)
{
    return qp_capi.interp;
}

PyInterpreterState *PyInterpreterState_Head(void)
{
    return qp_capi.interp;
}

PyInterpreterState *PyInterpreterState_Next(PyInterpreterState *interp)
{
    return NULL;
}

PyThreadState *PyInterpreterState_ThreadHead(PyInterpreterState *interp)
{
    return interp->threads.head;
}

int64_t PyInterpreterState_GetID(PyInterpreterState *interp)
{
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "no interpreter provided");
        return -1;
    }
    return 0;
}

PyObject *PyInterpreterState_GetDict(PyInterpreterState *interp)
{
    if (interp->qp_dict == NULL) {
        interp->qp_dict = PyDict_New();
        if (interp->qp_dict == NULL)
            PyErr_Clear();
    }
    /* Returning NULL means no per-interpreter dict is available. */
    return interp->qp_dict;
}

/* Subinterpreters: one C-API interpreter per process (capi-design.md,
   "Deviations"). */
PyInterpreterState *PyInterpreterState_New(void)
{
    PyErr_SetString(PyExc_RuntimeError,
                    "subinterpreters are not supported: the C API has one "
                    "interpreter per process");
    return NULL;
}

void PyInterpreterState_Clear(PyInterpreterState *interp)
{
    if (interp != qp_capi.interp)
        return;
    Py_CLEAR(interp->qp_dict);
}

void PyInterpreterState_Delete(PyInterpreterState *interp)
{
    if (interp == qp_capi.interp)
        _Py_FatalErrorFunc("PyInterpreterState_Delete",
                           "cannot delete the main interpreter");
}

PyThreadState *Py_NewInterpreter(void)
{
    return NULL;
}

/* subinterpreters are not supported (one interpreter per C-API process,
   capi-design.md §0): creation fails like Py_NewInterpreter */
PyStatus Py_NewInterpreterFromConfig(PyThreadState **tstate_p, const PyInterpreterConfig *config)
{
    (void)config;
    *tstate_p = NULL;
    return PyStatus_Error("subinterpreters are not supported");
}

void Py_EndInterpreter(PyThreadState *tstate)
{
    _Py_FatalErrorFunc("Py_EndInterpreter", "not a subinterpreter thread state");
}

/* _PyThread_CurrentFrames: {thread id: innermost frame} (one thread). */
PyObject *_PyThread_CurrentFrames(void)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *result = PyDict_New(), *frame, *key;
    JSValue f;

    if (result == NULL)
        return NULL;
    if (!qp_capi.main_tstate)
        return result;
    f = qp_getframe(ctx, 0);
    if (JS_IsException(f)) {
        if (!PyErr_ExceptionMatches(PyExc_ValueError)) {
            Py_DECREF(result);
            return NULL;
        }
        PyErr_Clear();
        return result;
    }
    frame = qp_capi_steal(ctx, f);
    if (frame == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    key = PyLong_FromUnsignedLong(qp_capi.main_tstate->impl.base.thread_id);
    if (key == NULL || PyDict_SetItem(result, key, frame) < 0) {
        Py_XDECREF(key);
        Py_DECREF(frame);
        Py_DECREF(result);
        return NULL;
    }
    Py_DECREF(key);
    Py_DECREF(frame);
    return result;
}

/* ---- GIL (python/ceval.c "Threads and the GIL") --------------------------- */

/* PyEval_SaveThread: release the GIL; the runtime thread state is kept in
   the C-API one until PyEval_RestoreThread. */
PyThreadState *PyEval_SaveThread(void)
{
    PyThreadState *t = qp_capi.tstate_current;
    if (t == NULL)
        _Py_FatalErrorFunc("PyEval_SaveThread", QP_CAPI_NO_TSTATE_MSG);
    qp_capi.tstate_current = NULL;
    QP_CAPI_TS(t)->qp_ts = qp_gil_release(qp_capi.ctx);
    return t;
}

/* PyEval_RestoreThread: take the GIL (creating the runtime thread state
   of a foreign thread on its first attach) and make tstate current. */
void PyEval_RestoreThread(PyThreadState *tstate)
{
    struct QPCapiThreadState *c = QP_CAPI_TS(tstate);
    QPThreadState *qts;

    if (tstate == NULL)
        _Py_FatalErrorFunc("PyEval_RestoreThread", "NULL tstate");
    qts = c->qp_ts;
    c->qp_ts = NULL;
    if (qts) {
        qp_gil_acquire(qp_capi.ctx, qts);
    } else if (!qp_capi_holds_gil()) {
        qts = qp_thread_attach(qp_capi.ctx);
        if (!qts)
            _Py_FatalErrorFunc("PyEval_RestoreThread", "cannot allocate the thread state");
        if (qts->foreign && !qts->capi_bound) {
            qts->capi_bound = tstate;
            c->own_ts = qts;
            qp_capi_tls_tstate = tstate;
        }
    }
    qp_capi.tstate_current = tstate;
}

void PyEval_AcquireThread(PyThreadState *tstate)
{
    PyEval_RestoreThread(tstate);
}

void PyEval_ReleaseThread(PyThreadState *tstate)
{
    if (tstate == NULL)
        _Py_FatalErrorFunc("PyEval_ReleaseThread", "NULL thread state");
    if (qp_capi.tstate_current != tstate)
        _Py_FatalErrorFunc("PyEval_ReleaseThread", "wrong thread state");
    (void)PyEval_SaveThread();
}

void PyEval_InitThreads(void)
{
    /* Deprecated: the GIL is created by Py_Initialize (CPython does
       nothing here either). */
}

/* PyGILState_Ensure: the calling thread's bound thread state (created for
   a foreign thread, deleted by the matching Release), attached. */
PyGILState_STATE PyGILState_Ensure(void)
{
    PyThreadState *t;
    int has_gil;

    if (qp_capi.main_tstate == NULL)
        _Py_FatalErrorFunc("PyGILState_Ensure", "the C API is not initialized");
    t = qp_capi_tls_tstate;
    if (t == NULL) {
        /* first call in this thread: take the GIL, then bind a thread
           state (the existing one of a runtime thread, else a new one
           deleted by the matching Release) */
        QPThreadState *qts = qp_thread_attach(qp_capi.ctx);
        if (qts == NULL)
            _Py_FatalErrorFunc("PyGILState_Ensure", "cannot allocate the thread state");
        if (qts->capi_bound) {
            t = (PyThreadState *)qts->capi_bound;
        } else {
            t = qp_capi_tstate_bind(qts);
            if (t == NULL)
                _Py_FatalErrorFunc("PyGILState_Ensure", "cannot allocate the thread state");
            t->gilstate_counter = 0;
            if (qts->foreign)
                QP_CAPI_TS(t)->own_ts = qts;
        }
        qts->capi_tstate = t;
        qp_capi.tstate_current = t;
        qp_capi_tls_tstate = t;
        has_gil = 0;
    } else {
        has_gil = qp_capi_holds_gil() && qp_capi.tstate_current == t;
        if (!has_gil)
            PyEval_RestoreThread(t);
    }
    t->gilstate_counter++;
    return has_gil ? PyGILState_LOCKED : PyGILState_UNLOCKED;
}

void PyGILState_Release(PyGILState_STATE oldstate)
{
    PyThreadState *t = qp_capi_tls_tstate;

    if (t == NULL)
        _Py_FatalErrorFunc("PyGILState_Release",
                           "auto-releasing thread-state, but no thread-state for this thread");
    if (!qp_capi_holds_gil() || qp_capi.tstate_current != t)
        _Py_FatalErrorFunc("PyGILState_Release",
                           "thread state must be current when releasing");
    t->gilstate_counter--;
    if (t->gilstate_counter < 0)
        _Py_FatalErrorFunc("PyGILState_Release", "This thread state must be current when releasing");
    if (t->gilstate_counter == 0) {
        /* the thread state created by PyGILState_Ensure */
        t->gilstate_counter++;
        PyThreadState_Clear(t);
        t->gilstate_counter--;
        qp_capi.tstate_current = NULL;
        if (qp_capi_tls_tstate == t)
            qp_capi_tls_tstate = NULL;
        qp_capi_delete_current(t);
    } else if (oldstate == PyGILState_UNLOCKED) {
        (void)PyEval_SaveThread();
    }
}

PyThreadState *PyGILState_GetThisThreadState(void)
{
    if (qp_capi.main_tstate == NULL)
        return NULL;
    return qp_capi_tls_tstate;
}

int PyGILState_Check(void)
{
    if (qp_capi.main_tstate == NULL)
        return 1;
    return qp_capi_holds_gil() && qp_capi.tstate_current != NULL &&
        qp_capi.tstate_current == qp_capi_tls_tstate;
}

/* ---- stop the world (GIL build: nothing to stop) -------------------------- */

void _PyEval_StopTheWorld(PyInterpreterState *interp)
{
    interp->stoptheworld.world_stopped = 1;
}

void _PyEval_StartTheWorld(PyInterpreterState *interp)
{
    interp->stoptheworld.world_stopped = 0;
}

int _Py_IsInterpreterFinalizing(PyInterpreterState *interp)
{
    return qp_capi_finalizing;
}

/* ========================================================================= */
/* ceval: recursion, pending calls, frame getters, slice indices              */
/* ========================================================================= */

int Py_EnterRecursiveCall(const char *where)
{
    if (js_check_stack_overflow(qp_capi.rt, 0)) {
        PyErr_Format(PyExc_RecursionError, "maximum recursion depth exceeded%s",
                     where ? where : "");
        return -1;
    }
    return 0;
}

void Py_LeaveRecursiveCall(void)
{
    /* The C recursion guard is stack based (CPython 3.12+): nothing to undo. */
}

void Py_SetRecursionLimit(int new_limit)
{
    JSContext *ctx = qp_capi.ctx;
    PyThreadState *t;
    if (ctx == NULL)
        return;
    QP_STATE(ctx)->recursion_limit = new_limit;
    for (t = qp_capi.interp ? qp_capi.interp->threads.head : NULL; t; t = t->next)
        t->py_recursion_limit = new_limit;
}

int Py_GetRecursionLimit(void)
{
    JSContext *ctx = qp_capi.ctx;
    if (ctx == NULL || QP_STATE(ctx)->recursion_limit <= 0)
        return 1000;
    return QP_STATE(ctx)->recursion_limit;
}

/* Pending calls (Python/ceval_gil.c): two queues, as in CPython.  The
   main-thread queue of Py_AddPendingCall (MAXPENDINGCALLS_MAIN entries) runs
   on the main thread only; the interpreter queue of _PyEval_AddPendingCall
   (MAXPENDINGCALLS, mainthreadonly = 0; _testinternalcapi.pending_threadfunc)
   runs on any thread of the C-API context.  Both may be filled from any
   thread; the interpreter poll (quickjs.c, qp_capi_pending_check) runs
   them, Py_MakePendingCalls too, not the signal checks of blocking calls
   (CPython's eval breaker vs PyErr_CheckSignals). */
#define QP_CAPI_MAXPENDINGCALLS 300
#define QP_CAPI_MAXPENDINGCALLS_MAIN 32
typedef struct QPCapiPendingQueue {
    struct {
        int (*func)(void *);
        void *arg;
    } calls[QP_CAPI_MAXPENDINGCALLS];
    int first, npending, max;
} QPCapiPendingQueue;
static QPCapiPendingQueue qp_capi_pending_main = { .max = QP_CAPI_MAXPENDINGCALLS_MAIN };
static QPCapiPendingQueue qp_capi_pending_interp = { .max = QP_CAPI_MAXPENDINGCALLS };
static int qp_capi_pending_busy;
static int qp_capi_pending_any;     /* atomic: a call may be queued */
static pthread_mutex_t qp_capi_pending_lock = PTHREAD_MUTEX_INITIALIZER;

/* 0, or -1 when the queue is full */
static int qp_capi_add_pending(QPCapiPendingQueue *q, int (*func)(void *), void *arg)
{
    int result = -1;
    pthread_mutex_lock(&qp_capi_pending_lock);
    if (q->npending < q->max) {
        int i = (q->first + q->npending) % QP_CAPI_MAXPENDINGCALLS;
        q->calls[i].func = func;
        q->calls[i].arg = arg;
        q->npending++;
        __atomic_store_n(&qp_capi_pending_any, 1, __ATOMIC_RELEASE);
        result = 0;
    }
    pthread_mutex_unlock(&qp_capi_pending_lock);
    /* signal_pending_calls: the thread holding the GIL polls at its next
       instruction (racy store, like a signal) */
    if (result == 0 && qp_capi.ctx)
        __atomic_store_n(&qp_capi.ctx->interrupt_counter, 1, __ATOMIC_RELAXED);
    return result;
}

int Py_AddPendingCall(int (*func)(void *), void *arg)
{
    return qp_capi_add_pending(&qp_capi_pending_main, func, arg);
}

static int qp_capi_pop_pending(QPCapiPendingQueue *q, int (**func)(void *), void **arg)
{
    int found = 0;
    pthread_mutex_lock(&qp_capi_pending_lock);
    if (q->npending > 0) {
        *func = q->calls[q->first].func;
        *arg = q->calls[q->first].arg;
        q->first = (q->first + 1) % QP_CAPI_MAXPENDINGCALLS;
        q->npending--;
        found = 1;
    }
    if (qp_capi_pending_main.npending == 0 && qp_capi_pending_interp.npending == 0)
        __atomic_store_n(&qp_capi_pending_any, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&qp_capi_pending_lock);
    return found;
}

/* _make_pending_calls of one queue: at most maxloop calls, so that a call
   that queues another one does not keep this thread busy forever (the
   remaining ones run at the next poll, maybe in another thread).  0, or -1
   with the callback's exception. */
static int qp_capi_run_pending_queue(QPCapiPendingQueue *q, int maxloop)
{
    int (*func)(void *);
    void *arg;
    int i;

    for (i = 0; i < maxloop; i++) {
        if (!qp_capi_pop_pending(q, &func, &arg))
            return 0;
        if (func(arg) != 0) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_SystemError,
                                "pending call failed without setting an exception");
            return -1;
        }
    }
    /* hit maxloop: poll again soon */
    if (__atomic_load_n(&qp_capi_pending_any, __ATOMIC_ACQUIRE))
        __atomic_store_n(&qp_capi.ctx->interrupt_counter, 1, __ATOMIC_RELAXED);
    return 0;
}

/* make_pending_calls: the interpreter queue (MAXPENDINGCALLSLOOP calls),
   then, on the main thread, the main-thread queue (MAXPENDINGCALLSLOOP_MAIN:
   all of them).  0, or -1 with the callback's exception. */
static int qp_capi_run_pending(void)
{
    int r;

    if (qp_capi_pending_busy)
        return 0;
    qp_capi_pending_busy = 1;
    r = qp_capi_run_pending_queue(&qp_capi_pending_interp, 100);
    if (r == 0 && qp_capi_rt_is_main_thread())
        r = qp_capi_run_pending_queue(&qp_capi_pending_main, QP_CAPI_MAXPENDINGCALLS_MAIN);
    qp_capi_pending_busy = 0;
    return r;
}

int Py_MakePendingCalls(void)
{
    JSContext *ctx = qp_capi.ctx;

    if (ctx == NULL || !qp_capi_rt_is_main_thread())
        return 0;
    /* signal handlers first (CPython _PyEval_MakePendingCalls) */
    if (qp_signal_check(ctx) < 0)
        return -1;
    return qp_capi_run_pending();
}

/* object.h hook, from qp_signal_check (the interpreter poll) */
static int qp_capi_pending_check(JSContext *ctx)
{
    if (!__atomic_load_n(&qp_capi_pending_any, __ATOMIC_ACQUIRE) || ctx != qp_capi.ctx ||
        qp_capi.tstate_current == NULL)
        return 0;
    return qp_capi_run_pending();
}

/* The innermost Python frame of the caller (NULL if none). */
static struct JSStackFrame *qp_capi_rt_frame(void)
{
    if (qp_capi.ctx == NULL)
        return NULL;
    return qp_py_frame_at(qp_capi.ctx, 0);
}

static JSValue qp_capi_rt_builtins_value(void)
{
    JSContext *ctx = qp_capi.ctx;
    struct JSStackFrame *sf = qp_capi_rt_frame();
    JSValue b = sf ? qp_frame_builtins(sf) : QP_NULL;
    if (QP_IS_NULL(b))
        b = QP_STATE(ctx)->builtins_dict;
    return b;
}

PyObject *PyEval_GetBuiltins(void)
{
    return qp_capi_rt_borrow(QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_BUILTINS),
                             qp_capi_rt_builtins_value());
}

PyObject *PyEval_GetGlobals(void)
{
    struct JSStackFrame *sf = qp_capi_rt_frame();
    JSValue g = sf ? qp_frame_globals(sf) : QP_NULL;
    if (QP_IS_NULL(g))
        return NULL;
    return qp_capi_rt_borrow(QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_GLOBALS), g);
}

/* locals(): the mapping (module/class code) or a dict snapshot (function). */
static JSValue qp_capi_rt_frame_locals(void)
{
    JSContext *ctx = qp_capi.ctx;
    struct JSStackFrame *sf = qp_capi_rt_frame();
    if (sf == NULL) {
        PyErr_SetString(PyExc_SystemError, "frame does not exist");
        return JS_EXCEPTION;
    }
    return qp_frame_locals(ctx, sf);
}

PyObject *PyEval_GetLocals(void)
{
    JSValue l = qp_capi_rt_frame_locals();
    PyObject *r;
    if (JS_IsException(l))
        return NULL;
    r = qp_capi_rt_borrow(qp_capi_rt_key_str("eval", "locals"), l);
    JS_FreeValue(qp_capi.ctx, l);
    return r;
}

PyObject *PyEval_GetFrameLocals(void)
{
    JSValue l = qp_capi_rt_frame_locals();
    if (JS_IsException(l))
        return NULL;
    return qp_capi_steal(qp_capi.ctx, l);
}

PyObject *PyEval_GetFrameGlobals(void)
{
    struct JSStackFrame *sf = qp_capi_rt_frame();
    JSValue g = sf ? qp_frame_globals(sf) : QP_NULL;
    if (QP_IS_NULL(g))
        return NULL;
    return qp_capi_new_ref(qp_capi.ctx, g);
}

PyObject *PyEval_GetFrameBuiltins(void)
{
    return qp_capi_new_ref(qp_capi.ctx, qp_capi_rt_builtins_value());
}

/* The innermost frame object (new reference), NULL without exception if
   there is no Python frame. */
static PyObject *qp_capi_rt_current_frame(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue f;
    if (ctx == NULL || qp_capi_rt_frame() == NULL)
        return NULL;
    f = qp_getframe(ctx, 0);
    if (JS_IsException(f)) {
        PyErr_Clear();
        return NULL;
    }
    return qp_capi_steal(ctx, f);
}

PyFrameObject *PyEval_GetFrame(void)
{
    PyObject *f = qp_capi_rt_current_frame();
    PyObject *r;
    if (f == NULL)
        return NULL;
    r = qp_capi_rt_borrow(qp_capi_rt_key_str("eval", "frame"), qp_capi_value(qp_capi.ctx, f));
    Py_DECREF(f);
    return (PyFrameObject *)r;
}

PyFrameObject *PyThreadState_GetFrame(PyThreadState *tstate)
{
    if (tstate != qp_capi.tstate_current)
        return NULL;
    return (PyFrameObject *)qp_capi_rt_current_frame();
}

PyObject *_PyEval_GetBuiltin(PyObject *name)
{
    PyObject *attr;
    PyObject *builtins = PyEval_GetBuiltins();
    if (builtins == NULL)
        return NULL;
    if (PyMapping_GetOptionalItem(builtins, name, &attr) == 0)
        PyErr_SetObject(PyExc_AttributeError, name);
    return attr;
}

int PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
    struct JSStackFrame *sf = qp_capi_rt_frame();
    int result = cf->cf_flags != 0;

    if (sf != NULL) {
        struct JSFunctionBytecode *b = qp_code_bytecode(sf->cur_func);
        QPCodeInfo *ci = b ? qp_code_info(b) : NULL;
        if (ci) {
            int compilerflags = (int)ci->co_flags & PyCF_MASK;
            if (compilerflags) {
                result = 1;
                cf->cf_flags |= compilerflags;
            }
        }
    }
    return result;
}

const char *PyEval_GetFuncName(PyObject *func)
{
    PyObject *name;
    const char *s;

    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    if (PyCFunction_Check(func))
        return ((PyCFunctionObject *)func)->m_ml->ml_name;
    if (!PyFunction_Check(func))
        return Py_TYPE(func)->tp_name;
    /* func.__name__, kept alive by a pin of the function's name */
    name = PyObject_GetAttrString(func, "__name__");
    if (name == NULL)
        return NULL;
    s = PyUnicode_AsUTF8(name);
    if (s != NULL) {
        qp_capi_pin_set(qp_capi_rt_holder,
                        qp_capi_rt_key("funcname", (const char *)&func, sizeof(func)), name);
        return s;
    }
    Py_DECREF(name);
    return NULL;
}

const char *PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else
        return " object";
}

/* Extract a slice index from a PyLong or an object with the nb_index slot
   defined, and store in *pi.  Silently reduce values larger than
   PY_SSIZE_T_MAX to PY_SSIZE_T_MAX, and silently boost values less than
   PY_SSIZE_T_MIN to PY_SSIZE_T_MIN.  Return 0 on error, 1 on success. */
int _PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    if (Py_IsNone(v))
        return 1;
    return _PyEval_SliceIndexNotNone(v, pi);
}

int _PyEval_SliceIndexNotNone(PyObject *v, Py_ssize_t *pi)
{
    Py_ssize_t x;
    if (PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && PyErr_Occurred())
            return 0;
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "slice indices must be integers or "
                        "have an __index__ method");
        return 0;
    }
    *pi = x;
    return 1;
}

int _PyEval_UnpackIndices(PyObject *start, PyObject *stop, Py_ssize_t len,
                          Py_ssize_t *istart, Py_ssize_t *istop)
{
    if (len < 0)
        return 0;
    *istart = 0;
    *istop = PY_SSIZE_T_MAX;
    if (!_PyEval_SliceIndex(start, istart))
        return 0;
    if (!_PyEval_SliceIndex(stop, istop))
        return 0;
    PySlice_AdjustIndices(len, istart, istop, 1);
    return 1;
}

/* ---- evaluating code ------------------------------------------------------ */

PyObject *PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue cv, gv, lv;

    if (globals == NULL) {
        PyErr_SetString(PyExc_SystemError, "PyEval_EvalCode: NULL globals");
        return NULL;
    }
    if (!PyDict_Check(globals)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    cv = qp_capi_value(ctx, co);
    gv = qp_capi_value(ctx, globals);
    lv = qp_capi_rt_opt(locals);
    if (JS_IsException(cv) || JS_IsException(gv) || JS_IsException(lv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_eval_code(ctx, cv, gv, lv));
}

PyObject *PyEval_EvalCodeEx(PyObject *co, PyObject *globals, PyObject *locals,
                            PyObject *const *args, int argcount,
                            PyObject *const *kws, int kwcount,
                            PyObject *const *defs, int defcount,
                            PyObject *kwdefs, PyObject *closure)
{
    PyObject *types = NULL, *ftype = NULL, *defaults = NULL, *fargs = NULL;
    PyObject *fkw = NULL, *func = NULL, *callargs = NULL, *callkw = NULL;
    PyObject *result = NULL;
    int i;

    if (argcount == 0 && kwcount == 0 && defcount == 0 && kwdefs == NULL &&
        closure == NULL)
        return PyEval_EvalCode(co, globals, locals);
    if (globals == NULL) {
        PyErr_SetString(PyExc_SystemError, "PyEval_EvalCodeEx: NULL globals");
        return NULL;
    }
    /* kwdefs is read with PyDict_GetItemRef() by the call (SystemError) */
    if (!PyDict_Check(globals) || (kwdefs != NULL && !PyDict_Check(kwdefs))) {
        PyErr_BadInternalCall();
        return NULL;
    }
    /* function = types.FunctionType(co, globals, None, defs, closure,
       kwdefs); function(*args, **kws) */
    types = PyImport_ImportModule("types");
    if (types == NULL)
        goto done;
    ftype = PyObject_GetAttrString(types, "FunctionType");
    if (ftype == NULL)
        goto done;
    if (defcount > 0) {
        defaults = PyTuple_New(defcount);
        if (defaults == NULL)
            goto done;
        for (i = 0; i < defcount; i++)
            PyTuple_SET_ITEM(defaults, i, Py_NewRef(defs[i]));
    }
    fargs = PyTuple_Pack(5, co, globals, Py_None, defaults ? defaults : Py_None,
                         closure ? closure : Py_None);
    if (fargs == NULL)
        goto done;
    if (kwdefs != NULL) {
        fkw = PyDict_New();
        if (fkw == NULL || PyDict_SetItemString(fkw, "kwdefaults", kwdefs) < 0)
            goto done;
    }
    func = PyObject_Call(ftype, fargs, fkw);
    if (func == NULL)
        goto done;
    callargs = PyTuple_New(argcount);
    if (callargs == NULL)
        goto done;
    for (i = 0; i < argcount; i++)
        PyTuple_SET_ITEM(callargs, i, Py_NewRef(args[i]));
    if (kwcount > 0) {
        callkw = PyDict_New();
        if (callkw == NULL)
            goto done;
        for (i = 0; i < kwcount; i++) {
            if (PyDict_SetItem(callkw, kws[2 * i], kws[2 * i + 1]) < 0)
                goto done;
        }
    }
    result = PyObject_Call(func, callargs, callkw);
done:
    Py_XDECREF(types);
    Py_XDECREF(ftype);
    Py_XDECREF(defaults);
    Py_XDECREF(fargs);
    Py_XDECREF(fkw);
    Py_XDECREF(func);
    Py_XDECREF(callargs);
    Py_XDECREF(callkw);
    return result;
}

/* ---- pythonrun.h: running and compiling source ---------------------------- */

#define QP_CAPI_CF_PARSE_FLAGS \
    (PyCF_MASK | PyCF_SOURCE_IS_UTF8 | PyCF_DONT_IMPLY_DEDENT | \
     PyCF_IGNORE_COOKIE | PyCF_TYPE_COMMENTS | PyCF_ALLOW_TOP_LEVEL_AWAIT | \
     PyCF_ALLOW_INCOMPLETE_INPUT)
/* run_eval_code_obj(): globals must be a real dict (SystemError) */
static int qp_capi_check_run_globals(PyObject *globals)
{
    if (globals == NULL || !PyAnyDict_Check(globals)) {
        PyErr_SetString(PyExc_SystemError,
                        "globals must be a real dict or a real frozendict");
        return -1;
    }
    return 0;
}

static int qp_capi_check_start(int start)
{
    if (start == Py_single_input || start == Py_file_input
        || start == Py_eval_input || start == Py_func_type_input)
        return 0;
    PyErr_SetString(PyExc_ValueError, "invalid start argument");
    return -1;
}


PyObject *PyRun_StringFlags(const char *str, int start, PyObject *globals,
                            PyObject *locals, PyCompilerFlags *flags)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue gv, lv;
    int f = flags ? (flags->cf_flags & QP_CAPI_CF_PARSE_FLAGS) : 0;

    if (qp_capi_check_start(start) < 0 || qp_capi_check_run_globals(globals) < 0)
        return NULL;
    gv = qp_capi_rt_opt(globals);
    lv = qp_capi_rt_opt(locals);
    if (JS_IsException(gv) || JS_IsException(lv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_run_source(ctx, str, strlen(str), "<string>",
                                                     start, f, gv, lv));
}

PyObject *(PyRun_String)(const char *str, int s, PyObject *g, PyObject *l)
{
    return PyRun_StringFlags(str, s, g, l, NULL);
}

/* Read a whole C stream (NUL-terminated buffer from PyMem_Malloc). */
static char *qp_capi_read_stream(FILE *fp, size_t *plen)
{
    size_t cap = 8192, len = 0, n;
    char *buf = PyMem_Malloc(cap), *nb;

    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (;;) {
        if (cap - len < 4096) {
            cap *= 2;
            nb = PyMem_Realloc(buf, cap);
            if (nb == NULL) {
                PyMem_Free(buf);
                PyErr_NoMemory();
                return NULL;
            }
            buf = nb;
        }
        n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) {
            if (ferror(fp)) {
                PyMem_Free(buf);
                PyErr_SetFromErrno(PyExc_OSError);
                return NULL;
            }
            break;
        }
    }
    buf[len] = '\0';
    *plen = len;
    return buf;
}

PyObject *PyRun_FileExFlags(FILE *fp, const char *filename, int start, PyObject *globals,
                            PyObject *locals, int closeit, PyCompilerFlags *flags)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue gv, lv;
    size_t len;
    char *src;
    PyObject *r;
    int f = flags ? (flags->cf_flags & QP_CAPI_CF_PARSE_FLAGS) : 0;

    if (qp_capi_check_start(start) < 0)
        return NULL;
    if (qp_capi_check_run_globals(globals) < 0) {
        if (closeit)
            fclose(fp);
        return NULL;
    }
    src = qp_capi_read_stream(fp, &len);
    if (closeit)
        fclose(fp);
    if (src == NULL)
        return NULL;
    gv = qp_capi_rt_opt(globals);
    lv = qp_capi_rt_opt(locals);
    if (JS_IsException(gv) || JS_IsException(lv)) {
        PyMem_Free(src);
        return NULL;
    }
    r = qp_capi_from_js_result(ctx, qp_run_source(ctx, src, len, filename ? filename : "???",
                                                  start, f, gv, lv));
    PyMem_Free(src);
    return r;
}

PyObject *(PyRun_File)(FILE *fp, const char *p, int s, PyObject *g, PyObject *l)
{
    return PyRun_FileExFlags(fp, p, s, g, l, 0, NULL);
}

PyObject *(PyRun_FileEx)(FILE *fp, const char *p, int s, PyObject *g, PyObject *l, int c)
{
    return PyRun_FileExFlags(fp, p, s, g, l, c, NULL);
}

PyObject *(PyRun_FileFlags)(FILE *fp, const char *p, int s, PyObject *g, PyObject *l,
                            PyCompilerFlags *flags)
{
    return PyRun_FileExFlags(fp, p, s, g, l, 0, flags);
}

int PyRun_SimpleStringFlags(const char *command, PyCompilerFlags *flags)
{
    PyObject *m, *d, *v;

    m = PyImport_AddModuleRef("__main__");
    if (m == NULL)
        return -1;
    d = PyModule_GetDict(m);
    v = PyRun_StringFlags(command, Py_file_input, d, d, flags);
    Py_DECREF(m);
    if (v == NULL) {
        PyErr_Print();
        return -1;
    }
    Py_DECREF(v);
    return 0;
}

int (PyRun_SimpleString)(const char *s)
{
    return PyRun_SimpleStringFlags(s, NULL);
}

int PyRun_SimpleFileExFlags(FILE *fp, const char *filename, int closeit, PyCompilerFlags *flags)
{
    PyObject *m, *d, *v, *fn;
    int set_file_name = 0, ret = -1, has;

    m = PyImport_AddModuleRef("__main__");
    if (m == NULL) {
        if (closeit)
            fclose(fp);
        return -1;
    }
    d = PyModule_GetDict(m);
    has = PyDict_ContainsString(d, "__file__");
    if (has < 0)
        goto done;
    if (!has) {
        fn = PyUnicode_DecodeFSDefault(filename);
        if (fn == NULL)
            goto done;
        if (PyDict_SetItemString(d, "__file__", fn) < 0 ||
            PyDict_SetItemString(d, "__cached__", Py_None) < 0) {
            Py_DECREF(fn);
            goto done;
        }
        Py_DECREF(fn);
        set_file_name = 1;
    }
    /* set_main_loader(): when running from stdin, leave __main__.__loader__ alone */
    if (strcmp(filename, "<stdin>") != 0) {
        PyObject *bootstrap, *loader_type, *loader = NULL;
        bootstrap = PyImport_ImportModule("_frozen_importlib_external");
        loader_type = bootstrap ? PyObject_GetAttrString(bootstrap, "SourceFileLoader") : NULL;
        Py_XDECREF(bootstrap);
        fn = loader_type ? PyUnicode_DecodeFSDefault(filename) : NULL;
        if (fn) {
            loader = PyObject_CallFunction(loader_type, "sO", "__main__", fn);
            Py_DECREF(fn);
        }
        Py_XDECREF(loader_type);
        if (loader == NULL || PyDict_SetItemString(d, "__loader__", loader) < 0) {
            Py_XDECREF(loader);
            fprintf(stderr, "python: failed to set __main__.__loader__\n");
            goto done;
        }
        Py_DECREF(loader);
    }
    v = PyRun_FileExFlags(fp, filename, Py_file_input, d, d, closeit, flags);
    closeit = 0;
    qp_run_flush_io(qp_capi.ctx);
    if (v == NULL) {
        Py_CLEAR(m);
        PyErr_Print();
        goto done;
    }
    Py_DECREF(v);
    ret = 0;
done:
    if (closeit)
        fclose(fp);
    if (set_file_name) {
        if (PyDict_PopString(d, "__file__", NULL) < 0)
            PyErr_Print();
        if (PyDict_PopString(d, "__cached__", NULL) < 0)
            PyErr_Print();
    }
    Py_XDECREF(m);
    return ret;
}

int (PyRun_SimpleFile)(FILE *f, const char *p)
{
    return PyRun_SimpleFileExFlags(f, p, 0, NULL);
}

int (PyRun_SimpleFileEx)(FILE *f, const char *p, int c)
{
    return PyRun_SimpleFileExFlags(f, p, c, NULL);
}

/* Py_FdIsInteractive (Python/pylifecycle.c) */
int Py_FdIsInteractive(FILE *fp, const char *filename)
{
    if (isatty(fileno(fp)))
        return 1;
    if (!QP_STATE(qp_capi.ctx)->config.interactive)
        return 0;
    return filename == NULL || strcmp(filename, "<stdin>") == 0 || strcmp(filename, "???") == 0;
}

/* One line of fp (without '\n') appended to line after the prompt
   str(sys.<prompt_name>) on stderr (PyOS_StdioReadline; stdin goes through
   the interactive loop's reader, so the readline module applies).
   1 = line read, 0 = EOF before any character, -1 = exception. */
static int qp_capi_fp_readline(JSContext *ctx, FILE *fp, JSAtom prompt_name, DynBuf *line)
{
    DynBuf prompt;
    int c, got = 0;

    if (fp == stdin)
        return qp_repl_readline(ctx, prompt_name, line);
    dbuf_init(&prompt);
    qp_repl_prompt(ctx, prompt_name, &prompt);
    fwrite(prompt.buf ? (const char *)prompt.buf : "", 1, prompt.size, stderr);
    fflush(stderr);
    dbuf_free(&prompt);
    Py_BEGIN_ALLOW_THREADS
    while ((c = getc(fp)) != EOF) {
        got = 1;
        if (c == '\n')
            break;
        dbuf_putc(line, c);
    }
    if (c == EOF)
        clearerr(fp);
    Py_END_ALLOW_THREADS
    return got;
}

/* _PyRun_InteractiveOne: read one complete interactive input from fp (sys.ps1
   then sys.ps2 prompts, codeop's completeness rule) and run it in __main__
   as Py_single_input.  0, E_EOF at end of input, or -1 with an exception. */
static int qp_capi_interactive_one(FILE *fp, PyObject *filename, PyCompilerFlags *flags)
{
    JSContext *ctx = qp_capi.ctx;
    int f = flags ? (flags->cf_flags & QP_CAPI_CF_PARSE_FLAGS) : 0;
    const char *fname;
    DynBuf src;
    int rc, more = 0;
    PyObject *main_module, *d;
    JSValue dv, res;

    if (!PyUnicode_Check(filename)) {
        PyErr_Format(PyExc_TypeError, "expect str for filename, got %T", filename);
        return -1;
    }
    fname = PyUnicode_AsUTF8(filename);
    if (!fname)
        return -1;
    dbuf_init(&src);
    for (;;) {
        if (more)
            dbuf_putc(&src, '\n');
        rc = qp_capi_fp_readline(ctx, fp, more ? QP_ATOM(ctx, ps2) : QP_ATOM(ctx, ps1), &src);
        dbuf_putc(&src, '\0');
        src.size--;
        if (rc < 0) {
            dbuf_free(&src);
            return -1;
        }
        if (rc == 0) {
            if (!more) {
                dbuf_free(&src);
                return E_EOF;
            }
            break;      /* EOF inside a statement: compile what we have */
        }
        if (!more && qp_repl_is_blank((const char *)src.buf, src.size)) {
            src.size = 0;
            continue;
        }
        if (qp_repl_input_complete(ctx, &src))
            break;
        more = 1;
    }
    dbuf_putc(&src, '\n');
    dbuf_putc(&src, '\0');
    src.size--;
    main_module = PyImport_AddModuleRef("__main__");
    if (!main_module) {
        dbuf_free(&src);
        return -1;
    }
    d = PyModule_GetDict(main_module);
    dv = d ? qp_capi_value(ctx, d) : JS_EXCEPTION;
    res = JS_IsException(dv) ? JS_EXCEPTION :
        qp_run_source_ex(ctx, (const char *)src.buf, src.size, fname, Py_single_input,
                         f, dv, dv, TRUE);
    Py_DECREF(main_module);
    dbuf_free(&src);
    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
    qp_run_flush_io(ctx);
    return 0;
}

int PyRun_InteractiveOneObject(FILE *fp, PyObject *filename, PyCompilerFlags *flags)
{
    int res = qp_capi_interactive_one(fp, filename, flags);
    if (res == -1) {
        PyErr_Print();
        qp_run_flush_io(qp_capi.ctx);
    }
    return res;
}

int PyRun_InteractiveOneFlags(FILE *fp, const char *filename_str, PyCompilerFlags *flags)
{
    PyObject *filename = PyUnicode_DecodeFSDefault(filename_str);
    int res;
    if (!filename) {
        PyErr_Print();
        return -1;
    }
    res = PyRun_InteractiveOneObject(fp, filename, flags);
    Py_DECREF(filename);
    return res;
}

int (PyRun_InteractiveOne)(FILE *f, const char *p)
{
    return PyRun_InteractiveOneFlags(f, p, NULL);
}

/* _PyRun_InteractiveLoop: default sys.ps1/ps2, then one input after the
   other until EOF; errors are printed (SystemExit ends the loop unless
   -i), MemoryError more than 16 times in a row aborts.  0 or -1. */
static int qp_capi_interactive_loop(FILE *fp, PyObject *filename, PyCompilerFlags *flags)
{
    JSContext *ctx = qp_capi.ctx;
    int nomem_count = 0, ret;

    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps1), ">>> ");
    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps2), "... ");
    do {
        ret = qp_capi_interactive_one(fp, filename, flags);
        if (ret == -1 && PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_MemoryError)) {
                if (++nomem_count > 16) {
                    PyErr_Clear();
                    return -1;
                }
            } else {
                nomem_count = 0;
            }
            if (!QP_STATE(ctx)->config.inspect && PyErr_ExceptionMatches(PyExc_SystemExit))
                return -1;
            PyErr_Print();
            qp_run_flush_io(ctx);
        } else {
            nomem_count = 0;
        }
    } while (ret != E_EOF);
    return 0;
}

int PyRun_InteractiveLoopFlags(FILE *fp, const char *filename, PyCompilerFlags *flags)
{
    PyObject *filename_obj = PyUnicode_DecodeFSDefault(filename);
    int r;
    if (!filename_obj) {
        PyErr_Print();
        return -1;
    }
    r = qp_capi_interactive_loop(fp, filename_obj, flags);
    Py_DECREF(filename_obj);
    if (r < 0) {
        PyErr_Print();
        return -1;
    }
    return 0;
}

int (PyRun_InteractiveLoop)(FILE *f, const char *p)
{
    return PyRun_InteractiveLoopFlags(f, p, NULL);
}

/* PyRun_AnyFileExFlags: the interactive loop for a terminal (or "<stdin>"
   under -i), else PyRun_SimpleFileExFlags */
int PyRun_AnyFileExFlags(FILE *fp, const char *filename, int closeit, PyCompilerFlags *flags)
{
    if (filename == NULL)
        filename = "???";
    if (Py_FdIsInteractive(fp, filename)) {
        int r = PyRun_InteractiveLoopFlags(fp, filename, flags);
        if (closeit)
            fclose(fp);
        return r;
    }
    return PyRun_SimpleFileExFlags(fp, filename, closeit, flags);
}

int (PyRun_AnyFile)(FILE *fp, const char *name)
{
    return PyRun_AnyFileExFlags(fp, name, 0, NULL);
}

int (PyRun_AnyFileEx)(FILE *fp, const char *name, int closeit)
{
    return PyRun_AnyFileExFlags(fp, name, closeit, NULL);
}

int (PyRun_AnyFileFlags)(FILE *fp, const char *name, PyCompilerFlags *flags)
{
    return PyRun_AnyFileExFlags(fp, name, 0, flags);
}

PyObject *Py_CompileStringObject(const char *str, PyObject *filename, int start,
                                 PyCompilerFlags *flags, int optimize)
{
    JSContext *ctx = qp_capi.ctx;
    int f = flags ? flags->cf_flags : 0;
    const char *fname, *mode;
    qp_error err;
    JSValue bc;

    if (qp_capi_check_start(start) < 0)
        return NULL;
    fname = PyUnicode_AsUTF8(filename);
    if (fname == NULL)
        return NULL;
    if (f & PyCF_ONLY_AST) {
        /* AST results: builtins.compile(source, filename, mode, flags,
           dont_inherit=True, optimize=optimize) */
        PyObject *builtins, *compile, *src, *r;
        switch (start) {
        case Py_single_input: mode = "single"; break;
        case Py_file_input: mode = "exec"; break;
        case Py_eval_input: mode = "eval"; break;
        case Py_func_type_input: mode = "func_type"; break;
        default:
            PyErr_SetString(PyExc_ValueError,
                            "compile() mode must be 'exec', 'eval', 'single' or 'func_type'");
            return NULL;
        }
        builtins = PyEval_GetBuiltins();
        if (builtins == NULL)
            return NULL;
        if (PyMapping_GetOptionalItemString(builtins, "compile", &compile) <= 0) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_RuntimeError, "lost builtins.compile");
            return NULL;
        }
        if (f & (PyCF_IGNORE_COOKIE | PyCF_SOURCE_IS_UTF8))
            src = PyUnicode_FromString(str);
        else
            src = PyBytes_FromString(str);
        if (src == NULL) {
            Py_DECREF(compile);
            return NULL;
        }
        r = PyObject_CallFunction(compile, "OOsiii", src, filename, mode,
                                  f & ~PyCF_SOURCE_IS_UTF8, 1, optimize);
        Py_DECREF(src);
        Py_DECREF(compile);
        return r;
    }
    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, str, strlen(str), fname, QP_NULL, start,
                           f & QP_CAPI_CF_PARSE_FLAGS, optimize, &err);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, fname);
        qp_error_clear(&err);
        return NULL;
    }
    qp_error_clear(&err);
    {
        JSValue code = qp_code_new(ctx, bc);
        JS_FreeValue(ctx, bc);
        return qp_capi_from_js_result(ctx, code);
    }
}

PyObject *Py_CompileStringExFlags(const char *str, const char *filename_str, int start,
                                  PyCompilerFlags *flags, int optimize)
{
    PyObject *filename, *co;
    filename = PyUnicode_DecodeFSDefault(filename_str);
    if (filename == NULL)
        return NULL;
    co = Py_CompileStringObject(str, filename, start, flags, optimize);
    Py_DECREF(filename);
    return co;
}

PyObject *(Py_CompileString)(const char *str, const char *p, int s)
{
    return Py_CompileStringExFlags(str, p, s, NULL, -1);
}

PyObject *(Py_CompileStringFlags)(const char *str, const char *p, int s, PyCompilerFlags *flags)
{
    return Py_CompileStringExFlags(str, p, s, flags, -1);
}

/* ========================================================================= */
/* Parking lot, PyMutex, PyEvent, once flags (CPython Python/lock.c and        */
/* Python/parking_lot.c semantics over one pthread mutex)                      */
/* ========================================================================= */

/* pycore_lock.h / pycore_parking_lot.h (CPython 3.15.2, layouts unchanged) */
#define QP_CAPI_HAS_PARKED 2
#define QP_CAPI_ONCE_INITIALIZED 4

typedef enum _PyLockFlags {
    _Py_LOCK_DONT_DETACH = 0,
    _PY_LOCK_DETACH = 1,
    _PY_LOCK_HANDLE_SIGNALS = 2,
    _PY_FAIL_IF_INTERRUPTED = 4,
    _PY_LOCK_PYTHONLOCK = 8,
} _PyLockFlags;

typedef struct {
    uint8_t v;
} PyEvent;

typedef struct {
    uintptr_t v;
} _PyRawMutex;

typedef int _Py_once_fn_t(void *arg);

typedef struct {
    PyMutex mutex;
    unsigned long long thread;
    size_t level;
} _PyRecursiveMutex;

enum {
    Py_PARK_OK = 0,
    Py_PARK_AGAIN = -1,
    Py_PARK_TIMEOUT = -2,
    Py_PARK_INTR = -3,
};

typedef void _Py_unpark_fn_t(void *arg, void *park_arg, int has_more_waiters);

PyAPI_FUNC(PyLockStatus) _PyMutex_LockTimed(PyMutex *m, PyTime_t timeout_ns, _PyLockFlags flags);
PyAPI_FUNC(int) _PyMutex_TryUnlock(PyMutex *m);
PyAPI_FUNC(void) _Py_yield(void);
PyAPI_FUNC(int) _PyEvent_IsSet(PyEvent *evt);
PyAPI_FUNC(void) _PyEvent_Notify(PyEvent *evt);
PyAPI_FUNC(void) PyEvent_Wait(PyEvent *evt);
PyAPI_FUNC(int) PyEvent_WaitTimed(PyEvent *evt, PyTime_t timeout_ns, int detach);
PyAPI_FUNC(void) _PyRawMutex_LockSlow(_PyRawMutex *m);
PyAPI_FUNC(void) _PyRawMutex_UnlockSlow(_PyRawMutex *m);
PyAPI_FUNC(int) _PyOnceFlag_CallOnceSlow(_PyOnceFlag *flag, _Py_once_fn_t *fn, void *arg);
PyAPI_FUNC(int) _PyRecursiveMutex_IsLockedByCurrentThread(_PyRecursiveMutex *m);
PyAPI_FUNC(void) _PyRecursiveMutex_Lock(_PyRecursiveMutex *m);
PyAPI_FUNC(PyLockStatus) _PyRecursiveMutex_LockTimed(_PyRecursiveMutex *m, PyTime_t timeout,
                                                     _PyLockFlags flags);
PyAPI_FUNC(void) _PyRecursiveMutex_Unlock(_PyRecursiveMutex *m);
PyAPI_FUNC(int) _PyRecursiveMutex_TryUnlock(_PyRecursiveMutex *m);
PyAPI_FUNC(int) _PyParkingLot_Park(const void *address, const void *expected,
                                   size_t address_size, PyTime_t timeout_ns,
                                   void *park_arg, int detach);
PyAPI_FUNC(void) _PyParkingLot_Unpark(const void *address, _Py_unpark_fn_t *fn, void *arg);
PyAPI_FUNC(void) _PyParkingLot_UnparkAll(const void *address);
PyAPI_FUNC(void) _PyParkingLot_AfterFork(void);
PyAPI_FUNC(PyTime_t) _PyDeadline_Init(PyTime_t timeout);
PyAPI_FUNC(PyTime_t) _PyDeadline_Get(PyTime_t deadline);
PyAPI_FUNC(PyTime_t) _PyTime_Add(PyTime_t t1, PyTime_t t2);
PyAPI_FUNC(PyTime_t) _PyTime_FromMicrosecondsClamp(PyTime_t us);

typedef struct QPCapiParkWaiter {
    struct QPCapiParkWaiter *next, *prev;
    const void *address;
    void *park_arg;
    pthread_cond_t cond;
    int woken;
} QPCapiParkWaiter;

static pthread_mutex_t qp_capi_park_lock = PTHREAD_MUTEX_INITIALIZER;
static QPCapiParkWaiter qp_capi_park_list = {
    &qp_capi_park_list, &qp_capi_park_list, NULL, NULL, PTHREAD_COND_INITIALIZER, 0
};

/* atomic_memcmp of parking_lot.c */
static int qp_capi_park_matches(const void *addr, const void *expected, size_t size)
{
    switch (size) {
    case 1: return __atomic_load_n((const uint8_t *)addr, __ATOMIC_SEQ_CST) == *(const uint8_t *)expected;
    case 2: return __atomic_load_n((const uint16_t *)addr, __ATOMIC_SEQ_CST) == *(const uint16_t *)expected;
    case 4: return __atomic_load_n((const uint32_t *)addr, __ATOMIC_SEQ_CST) == *(const uint32_t *)expected;
    case 8: return __atomic_load_n((const uint64_t *)addr, __ATOMIC_SEQ_CST) == *(const uint64_t *)expected;
    default:
        _Py_FatalErrorFunc("_PyParkingLot_Park", "unexpected address size");
    }
}

static void qp_capi_park_unlink(QPCapiParkWaiter *w)
{
    w->prev->next = w->next;
    w->next->prev = w->prev;
    w->next = w->prev = w;
}

int _PyParkingLot_Park(const void *address, const void *expected, size_t size,
                       PyTime_t timeout_ns, void *park_arg, int detach)
{
    QPCapiParkWaiter w;
    PyThreadState *saved = NULL;
    struct timespec deadline;
    int ret = Py_PARK_OK;

    pthread_mutex_lock(&qp_capi_park_lock);
    if (!qp_capi_park_matches(address, expected, size)) {
        pthread_mutex_unlock(&qp_capi_park_lock);
        return Py_PARK_AGAIN;
    }
    if (timeout_ns == 0) {
        pthread_mutex_unlock(&qp_capi_park_lock);
        return Py_PARK_TIMEOUT;
    }
    w.address = address;
    w.park_arg = park_arg;
    w.woken = 0;
    pthread_cond_init(&w.cond, NULL);
    w.prev = qp_capi_park_list.prev;
    w.next = &qp_capi_park_list;
    w.prev->next = &w;
    qp_capi_park_list.prev = &w;
    if (detach && qp_capi.tstate_current != NULL && qp_capi_holds_gil())
        saved = PyEval_SaveThread();
    if (timeout_ns > 0) {
        struct timespec now;
        PyTime_t ns;
        clock_gettime(CLOCK_REALTIME, &now);
        ns = _PyTime_Add((PyTime_t)now.tv_sec * 1000000000 + now.tv_nsec, timeout_ns);
        deadline.tv_sec = (time_t)(ns / 1000000000);
        deadline.tv_nsec = (long)(ns % 1000000000);
    }
    while (!w.woken) {
        if (timeout_ns < 0) {
            pthread_cond_wait(&w.cond, &qp_capi_park_lock);
        } else if (pthread_cond_timedwait(&w.cond, &qp_capi_park_lock, &deadline) == ETIMEDOUT &&
                   !w.woken) {
            qp_capi_park_unlink(&w);
            ret = Py_PARK_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&qp_capi_park_lock);
    pthread_cond_destroy(&w.cond);
    if (saved)
        PyEval_RestoreThread(saved);
    return ret;
}

void _PyParkingLot_Unpark(const void *address, _Py_unpark_fn_t *fn, void *arg)
{
    QPCapiParkWaiter *w, *x;
    int has_more = 0;

    pthread_mutex_lock(&qp_capi_park_lock);
    for (w = qp_capi_park_list.next; w != &qp_capi_park_list; w = w->next) {
        if (w->address == address)
            break;
    }
    if (w == &qp_capi_park_list) {
        w = NULL;
    } else {
        for (x = w->next; x != &qp_capi_park_list; x = x->next) {
            if (x->address == address) {
                has_more = 1;
                break;
            }
        }
        qp_capi_park_unlink(w);
    }
    fn(arg, w ? w->park_arg : NULL, has_more);
    if (w) {
        w->woken = 1;
        pthread_cond_signal(&w->cond);
    }
    pthread_mutex_unlock(&qp_capi_park_lock);
}

void _PyParkingLot_UnparkAll(const void *address)
{
    QPCapiParkWaiter *w, *next;

    pthread_mutex_lock(&qp_capi_park_lock);
    for (w = qp_capi_park_list.next; w != &qp_capi_park_list; w = next) {
        next = w->next;
        if (w->address == address) {
            qp_capi_park_unlink(w);
            w->woken = 1;
            pthread_cond_signal(&w->cond);
        }
    }
    pthread_mutex_unlock(&qp_capi_park_lock);
}

void _PyParkingLot_AfterFork(void)
{
    /* the child has only the forking thread: drop every waiter */
    pthread_mutex_init(&qp_capi_park_lock, NULL);
    qp_capi_park_list.next = qp_capi_park_list.prev = &qp_capi_park_list;
}

void _Py_yield(void)
{
    sched_yield();
}

static void qp_capi_mutex_unpark(void *arg, void *park_arg, int has_more_waiters)
{
    PyMutex *m = arg;
    _Py_atomic_store_uint8(&m->_bits, has_more_waiters ? QP_CAPI_HAS_PARKED : _Py_UNLOCKED);
}

PyLockStatus _PyMutex_LockTimed(PyMutex *m, PyTime_t timeout, _PyLockFlags flags)
{
    uint8_t v = _Py_atomic_load_uint8_relaxed(&m->_bits);
    PyTime_t endtime = 0;
    int ret;

    if ((v & _Py_LOCKED) == 0) {
        if (_Py_atomic_compare_exchange_uint8(&m->_bits, &v, v | _Py_LOCKED))
            return PY_LOCK_ACQUIRED;
    }
    if (timeout == 0)
        return PY_LOCK_FAILURE;
    if (timeout > 0)
        endtime = _PyDeadline_Init(timeout);
    for (;;) {
        if ((v & _Py_LOCKED) == 0) {
            if (_Py_atomic_compare_exchange_uint8(&m->_bits, &v, v | _Py_LOCKED))
                return PY_LOCK_ACQUIRED;
            continue;
        }
        if (!(v & QP_CAPI_HAS_PARKED)) {
            uint8_t newv = v | QP_CAPI_HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uint8(&m->_bits, &v, newv))
                continue;
            v = newv;
        }
        ret = _PyParkingLot_Park(&m->_bits, &v, sizeof(v), timeout, NULL,
                                 flags & _PY_LOCK_DETACH);
        if (ret == Py_PARK_INTR && (flags & _PY_LOCK_HANDLE_SIGNALS)) {
            if (Py_MakePendingCalls() < 0)
                return PY_LOCK_INTR;
        } else if (ret == Py_PARK_INTR && (flags & _PY_FAIL_IF_INTERRUPTED)) {
            return PY_LOCK_INTR;
        } else if (ret == Py_PARK_TIMEOUT) {
            return PY_LOCK_FAILURE;
        }
        if (timeout > 0) {
            timeout = _PyDeadline_Get(endtime);
            if (timeout <= 0)
                timeout = 0;
        }
        v = _Py_atomic_load_uint8_relaxed(&m->_bits);
    }
}

int _PyMutex_TryUnlock(PyMutex *m)
{
    uint8_t v = _Py_atomic_load_uint8(&m->_bits);
    for (;;) {
        if ((v & _Py_LOCKED) == 0)
            return -1;  /* not locked */
        if (v & QP_CAPI_HAS_PARKED) {
            _PyParkingLot_Unpark(&m->_bits, qp_capi_mutex_unpark, m);
            return 0;
        }
        if (_Py_atomic_compare_exchange_uint8(&m->_bits, &v, _Py_UNLOCKED))
            return 0;
    }
}

/* the public header maps these names to its inline fast paths */
#undef PyMutex_Lock
#undef PyMutex_Unlock
#undef PyMutex_IsLocked

void PyMutex_Lock(PyMutex *m)
{
    _PyMutex_LockTimed(m, -1, _PY_LOCK_DETACH);
}

void PyMutex_Unlock(PyMutex *m)
{
    if (_PyMutex_TryUnlock(m) < 0)
        _Py_FatalErrorFunc("PyMutex_Unlock", "unlocking mutex that is not locked");
}

int PyMutex_IsLocked(PyMutex *m)
{
    return (_Py_atomic_load_uint8(&m->_bits) & _Py_LOCKED) != 0;
}

static void qp_capi_rawmutex_unpark(void *arg, void *park_arg, int has_more_waiters)
{
    _PyRawMutex *m = arg;
    _Py_atomic_store_uintptr(&m->v, has_more_waiters ? QP_CAPI_HAS_PARKED : _Py_UNLOCKED);
}

void _PyRawMutex_LockSlow(_PyRawMutex *m)
{
    for (;;) {
        uintptr_t v = _Py_atomic_load_uintptr(&m->v);
        if ((v & _Py_LOCKED) == 0) {
            if (_Py_atomic_compare_exchange_uintptr(&m->v, &v, v | _Py_LOCKED))
                return;
            continue;
        }
        if (!(v & QP_CAPI_HAS_PARKED)) {
            uintptr_t newv = v | QP_CAPI_HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uintptr(&m->v, &v, newv))
                continue;
            v = newv;
        }
        (void)_PyParkingLot_Park(&m->v, &v, sizeof(v), -1, NULL, 0);
    }
}

void _PyRawMutex_UnlockSlow(_PyRawMutex *m)
{
    uintptr_t v = _Py_atomic_load_uintptr(&m->v);
    for (;;) {
        if ((v & _Py_LOCKED) == 0)
            _Py_FatalErrorFunc("_PyRawMutex_UnlockSlow", "unlocking mutex that is not locked");
        if (v & QP_CAPI_HAS_PARKED) {
            _PyParkingLot_Unpark(&m->v, qp_capi_rawmutex_unpark, m);
            return;
        }
        if (_Py_atomic_compare_exchange_uintptr(&m->v, &v, _Py_UNLOCKED))
            return;
    }
}

int _PyEvent_IsSet(PyEvent *evt)
{
    uint8_t v = _Py_atomic_load_uint8(&evt->v);
    return v == _Py_LOCKED;
}

void _PyEvent_Notify(PyEvent *evt)
{
    uintptr_t v = _Py_atomic_exchange_uint8(&evt->v, _Py_LOCKED);
    if (v == _Py_UNLOCKED || v == _Py_LOCKED)
        return;  /* nobody waits / notified more than once */
    _PyParkingLot_UnparkAll(&evt->v);
}

int PyEvent_WaitTimed(PyEvent *evt, PyTime_t timeout_ns, int detach)
{
    for (;;) {
        uint8_t v = _Py_atomic_load_uint8(&evt->v);
        uint8_t expected;
        if (v == _Py_LOCKED)
            return 1;  /* event already set */
        if (v == _Py_UNLOCKED) {
            if (!_Py_atomic_compare_exchange_uint8(&evt->v, &v, QP_CAPI_HAS_PARKED))
                continue;
        }
        expected = QP_CAPI_HAS_PARKED;
        (void)_PyParkingLot_Park(&evt->v, &expected, sizeof(evt->v), timeout_ns, NULL, detach);
        return _Py_atomic_load_uint8(&evt->v) == _Py_LOCKED;
    }
}

void PyEvent_Wait(PyEvent *evt)
{
    while (!PyEvent_WaitTimed(evt, -1, /*detach=*/1))
        ;
}

static int qp_capi_unlock_once(_PyOnceFlag *o, int res)
{
    /* success (0): initialized; failure (-1): back to unlocked */
    uint8_t new_value, old_value;
    switch (res) {
    case -1:
        new_value = _Py_UNLOCKED;
        break;
    case 0:
        new_value = QP_CAPI_ONCE_INITIALIZED;
        break;
    default:
        _Py_FatalErrorFunc("_PyOnceFlag_CallOnce", "invalid result from _PyOnceFlag_CallOnce");
    }
    old_value = _Py_atomic_exchange_uint8(&o->v, new_value);
    if ((old_value & QP_CAPI_HAS_PARKED) != 0)
        _PyParkingLot_UnparkAll(&o->v);
    return res;
}

int _PyOnceFlag_CallOnceSlow(_PyOnceFlag *flag, _Py_once_fn_t *fn, void *arg)
{
    uint8_t v = _Py_atomic_load_uint8(&flag->v);
    for (;;) {
        if (v == _Py_UNLOCKED) {
            if (!_Py_atomic_compare_exchange_uint8(&flag->v, &v, _Py_LOCKED))
                continue;
            return qp_capi_unlock_once(flag, fn(arg));
        }
        if (v == QP_CAPI_ONCE_INITIALIZED)
            return 0;
        /* initializing in another thread */
        if (!(v & QP_CAPI_HAS_PARKED)) {
            uint8_t new_value = v | QP_CAPI_HAS_PARKED;
            if (!_Py_atomic_compare_exchange_uint8(&flag->v, &v, new_value))
                continue;
            v = new_value;
        }
        (void)_PyParkingLot_Park(&flag->v, &v, sizeof(v), -1, NULL, 1);
        v = _Py_atomic_load_uint8(&flag->v);
    }
}

static unsigned long long qp_capi_thread_ident_ex(void)
{
    return (unsigned long long)PyThread_get_thread_ident();
}

int _PyRecursiveMutex_IsLockedByCurrentThread(_PyRecursiveMutex *m)
{
    return __atomic_load_n(&m->thread, __ATOMIC_RELAXED) == qp_capi_thread_ident_ex();
}

void _PyRecursiveMutex_Lock(_PyRecursiveMutex *m)
{
    unsigned long long thread = qp_capi_thread_ident_ex();
    if (__atomic_load_n(&m->thread, __ATOMIC_RELAXED) == thread) {
        m->level++;
        return;
    }
    PyMutex_Lock(&m->mutex);
    __atomic_store_n(&m->thread, thread, __ATOMIC_RELAXED);
}

PyLockStatus _PyRecursiveMutex_LockTimed(_PyRecursiveMutex *m, PyTime_t timeout,
                                         _PyLockFlags flags)
{
    unsigned long long thread = qp_capi_thread_ident_ex();
    PyLockStatus s;
    if (__atomic_load_n(&m->thread, __ATOMIC_RELAXED) == thread) {
        m->level++;
        return PY_LOCK_ACQUIRED;
    }
    s = _PyMutex_LockTimed(&m->mutex, timeout, flags);
    if (s == PY_LOCK_ACQUIRED)
        __atomic_store_n(&m->thread, thread, __ATOMIC_RELAXED);
    return s;
}

int _PyRecursiveMutex_TryUnlock(_PyRecursiveMutex *m)
{
    if (!_PyRecursiveMutex_IsLockedByCurrentThread(m))
        return -1;
    if (m->level > 0) {
        m->level--;
        return 0;
    }
    __atomic_store_n(&m->thread, 0, __ATOMIC_RELAXED);
    PyMutex_Unlock(&m->mutex);
    return 0;
}

void _PyRecursiveMutex_Unlock(_PyRecursiveMutex *m)
{
    if (_PyRecursiveMutex_TryUnlock(m) < 0)
        _Py_FatalErrorFunc("_PyRecursiveMutex_Unlock",
                           "unlocking a recursive mutex that is not owned by the current thread");
}

/* ---- critical sections: no-ops in the GIL build ---------------------------- */

void PyCriticalSection_Begin(PyCriticalSection *c, PyObject *op)
{
}

void PyCriticalSection_End(PyCriticalSection *c)
{
}

void PyCriticalSection2_Begin(PyCriticalSection2 *c, PyObject *a, PyObject *b)
{
}

void PyCriticalSection2_End(PyCriticalSection2 *c)
{
}

void PyCriticalSection_BeginMutex(PyCriticalSection *c, PyMutex *m)
{
}

void PyCriticalSection2_BeginMutex(PyCriticalSection2 *c, PyMutex *m1, PyMutex *m2)
{
}

/* ========================================================================= */
/* pythread.h (CPython 3.15 Python/thread.c: locks are PyMutex)               */
/* ========================================================================= */

const long long PY_TIMEOUT_MAX = LLONG_MAX / 1000;

/* the thread stack size is _thread's (python/mod_thread.c) */
#define qp_capi_thread_stacksize qp_thread_stacksize

void PyThread_init_thread(void)
{
    /* nothing to initialize: pthreads need no setup */
}

struct qp_capi_thread_boot {
    void (*func)(void *);
    void *arg;
};

static void *qp_capi_thread_run(void *p)
{
    struct qp_capi_thread_boot b = *(struct qp_capi_thread_boot *)p;
    PyMem_RawFree(p);
    b.func(b.arg);
    return NULL;
}

/* A detached OS thread running func(arg) with the _thread stack size; it
   attaches to the interpreter with PyGILState_Ensure (M5). */
unsigned long PyThread_start_new_thread(void (*func)(void *), void *arg)
{
    struct qp_capi_thread_boot *b = PyMem_RawMalloc(sizeof(*b));
    pthread_attr_t attr;
    pthread_t th;
    int rc;

    if (b == NULL)
        return PYTHREAD_INVALID_THREAD_ID;
    b->func = func;
    b->arg = arg;
    rc = pthread_attr_init(&attr);
    if (rc == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_attr_setstacksize(&attr, qp_thread_stacksize ? qp_thread_stacksize
                                                                  : QP_THREAD_DEFAULT_STACK);
        if (rc == 0)
            rc = pthread_create(&th, &attr, qp_capi_thread_run, b);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        PyMem_RawFree(b);
        return PYTHREAD_INVALID_THREAD_ID;
    }
    return (unsigned long)(uintptr_t)th;
}

void _Py_NO_RETURN PyThread_exit_thread(void)
{
    pthread_exit(0);
}

unsigned long PyThread_get_thread_ident(void)
{
    return (unsigned long)(uintptr_t)pthread_self();
}

unsigned long PyThread_get_thread_native_id(void)
{
    return qp_capi_native_thread_id();
}

PyThread_type_lock PyThread_allocate_lock(void)
{
    PyMutex *lock = (PyMutex *)PyMem_RawMalloc(sizeof(PyMutex));
    if (lock)
        *lock = (PyMutex){0};
    return (PyThread_type_lock)lock;
}

void PyThread_free_lock(PyThread_type_lock lock)
{
    PyMem_RawFree(lock);
}

PyLockStatus PyThread_acquire_lock_timed(PyThread_type_lock lock, PY_TIMEOUT_T microseconds,
                                         int intr_flag)
{
    PyTime_t timeout;
    _PyLockFlags flags = _Py_LOCK_DONT_DETACH;

    if (microseconds >= 0)
        timeout = _PyTime_FromMicrosecondsClamp(microseconds);
    else
        timeout = -1;
    if (intr_flag)
        flags |= _PY_FAIL_IF_INTERRUPTED;
    return _PyMutex_LockTimed((PyMutex *)lock, timeout, flags);
}

int PyThread_acquire_lock(PyThread_type_lock lock, int waitflag)
{
    return PyThread_acquire_lock_timed(lock, waitflag ? -1 : 0, /*intr_flag=*/0);
}

void PyThread_release_lock(PyThread_type_lock lock)
{
    PyMutex_Unlock((PyMutex *)lock);
}

size_t PyThread_get_stacksize(void)
{
    return qp_capi_thread_stacksize;
}

int PyThread_set_stacksize(size_t size)
{
    pthread_attr_t attrs;
    int rc;

    if (size == 0) {
        qp_capi_thread_stacksize = 0;
        return 0;
    }
    if (size < 0x8000)  /* THREAD_STACK_MIN */
        return -1;
    if (pthread_attr_init(&attrs) != 0)
        return -1;
    rc = pthread_attr_setstacksize(&attrs, size);
    pthread_attr_destroy(&attrs);
    if (rc != 0)
        return -1;
    qp_capi_thread_stacksize = size;
    return 0;
}

PyObject *PyThread_GetInfo(void)
{
    PyObject *info = PySys_GetAttrString("thread_info");
    return info;
}

Py_tss_t *PyThread_tss_alloc(void)
{
    Py_tss_t *key = (Py_tss_t *)PyMem_RawMalloc(sizeof(Py_tss_t));
    if (key != NULL)
        memset(key, 0, sizeof(*key));
    return key;
}

void PyThread_tss_free(Py_tss_t *key)
{
    if (key != NULL) {
        PyThread_tss_delete(key);
        PyMem_RawFree((void *)key);
    }
}

int PyThread_tss_is_created(Py_tss_t *key)
{
    return key->_is_initialized;
}

int PyThread_tss_create(Py_tss_t *key)
{
    if (key->_is_initialized)
        return 0;
    if (pthread_key_create(&key->_key, NULL) != 0)
        return -1;
    key->_is_initialized = 1;
    return 0;
}

void PyThread_tss_delete(Py_tss_t *key)
{
    if (!key->_is_initialized)
        return;
    pthread_key_delete(key->_key);
    key->_is_initialized = 0;
}

int PyThread_tss_set(Py_tss_t *key, void *value)
{
    return pthread_setspecific(key->_key, value) == 0 ? 0 : -1;
}

void *PyThread_tss_get(Py_tss_t *key)
{
    return pthread_getspecific(key->_key);
}

/* Deprecated int-keyed TLS API (CPython thread_pthread.h). */
int PyThread_create_key(void)
{
    pthread_key_t key;
    if (pthread_key_create(&key, NULL) != 0)
        return -1;
    if (key > INT_MAX) {
        pthread_key_delete(key);
        errno = ENOMEM;
        return -1;
    }
    return (int)key;
}

void PyThread_delete_key(int key)
{
    pthread_key_delete((pthread_key_t)key);
}

void PyThread_delete_key_value(int key)
{
    pthread_setspecific((pthread_key_t)key, NULL);
}

int PyThread_set_key_value(int key, void *value)
{
    return pthread_setspecific((pthread_key_t)key, value) == 0 ? 0 : -1;
}

void *PyThread_get_key_value(int key)
{
    return pthread_getspecific((pthread_key_t)key);
}

void PyThread_ReInitTLS(void)
{
}

/* ========================================================================= */
/* import.h                                                                   */
/* ========================================================================= */

PyObject *PyImport_GetModuleDict(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue modules = QP_STATE(ctx)->modules;
    if (JS_VALUE_GET_TAG(modules) != JS_TAG_OBJECT)
        _Py_FatalErrorFunc("PyImport_GetModuleDict", "no module dictionary");
    return qp_capi_rt_borrow(qp_capi_rt_key_str("sys", "modules"), modules);
}

PyObject *PyImport_GetModule(PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue nv = qp_capi_value(ctx, name), mod;
    int rc;

    if (JS_IsException(nv))
        return NULL;
    rc = qp_import_get_module(ctx, nv, &mod);
    if (rc <= 0)
        return NULL;
    return qp_capi_steal(ctx, mod);
}

/* import_add_module(): sys.modules[name] if it is a module, else a new
   module stored there.  New reference. */
static PyObject *qp_capi_import_add_module(PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue nv = qp_capi_value(ctx, name), mod, modules;
    int rc;

    if (JS_IsException(nv))
        return NULL;
    modules = QP_STATE(ctx)->modules;
    if (JS_VALUE_GET_TAG(modules) != JS_TAG_OBJECT) {
        PyErr_SetString(PyExc_RuntimeError, "no import module dictionary");
        return NULL;
    }
    rc = qp_import_get_module(ctx, nv, &mod);
    if (rc < 0)
        return NULL;
    if (rc > 0) {
        if (qp_module_check(ctx, mod))
            return qp_capi_steal(ctx, mod);
        JS_FreeValue(ctx, mod);
    }
    mod = qp_module_new(ctx, nv);
    if (JS_IsException(mod))
        return NULL;
    if (qp_dict_set_item(ctx, modules, nv, mod) < 0) {
        JS_FreeValue(ctx, mod);
        return NULL;
    }
    return qp_capi_steal(ctx, mod);
}

PyObject *PyImport_AddModuleRef(const char *name)
{
    PyObject *name_obj = PyUnicode_FromString(name), *m;
    if (name_obj == NULL)
        return NULL;
    m = qp_capi_import_add_module(name_obj);
    Py_DECREF(name_obj);
    return m;
}

PyObject *PyImport_AddModuleObject(PyObject *name)
{
    PyObject *mod = qp_capi_import_add_module(name), *r, *key;
    const char *s;
    Py_ssize_t len;

    if (mod == NULL)
        return NULL;
    /* borrowed: the module stays referenced by sys.modules; the pin (one
       per name; any hashable name is accepted, keyed by its repr) keeps the
       pointer valid if it is removed from there */
    key = PyObject_Repr(name);
    s = key ? PyUnicode_AsUTF8AndSize(key, &len) : NULL;
    if (s == NULL) {
        Py_XDECREF(key);
        Py_DECREF(mod);
        return NULL;
    }
    r = qp_capi_rt_borrow(qp_capi_rt_key("addmodule", s, (size_t)len),
                          qp_capi_value(qp_capi.ctx, mod));
    Py_DECREF(key);
    Py_DECREF(mod);
    return r;
}

PyObject *PyImport_AddModule(const char *name)
{
    PyObject *name_obj = PyUnicode_FromString(name), *m;
    if (name_obj == NULL)
        return NULL;
    m = PyImport_AddModuleObject(name_obj);
    Py_DECREF(name_obj);
    return m;
}

PyObject *PyImport_Import(PyObject *module_name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (module_name == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyUnicode_Check(module_name)) {
        PyErr_Format(PyExc_TypeError, "module name must be a string, not %T", module_name);
        return NULL;
    }
    v = qp_capi_value(ctx, module_name);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_import_module_obj(ctx, v));
}

PyObject *PyImport_ImportModule(const char *name)
{
    PyObject *pname, *result;
    pname = PyUnicode_FromString(name);
    if (pname == NULL)
        return NULL;
    result = PyImport_Import(pname);
    Py_DECREF(pname);
    return result;
}

PyObject *PyImport_ImportModuleLevelObject(PyObject *name, PyObject *globals, PyObject *locals,
                                           PyObject *fromlist, int level)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue nv = qp_capi_rt_opt(name), gv = qp_capi_rt_opt(globals);
    JSValue lv = qp_capi_rt_opt(locals), fv = qp_capi_rt_opt(fromlist);

    if (JS_IsException(nv) || JS_IsException(gv) || JS_IsException(lv) || JS_IsException(fv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_import_module_level(ctx, nv, gv, lv, fv, level));
}

PyObject *PyImport_ImportModuleLevel(const char *name, PyObject *globals, PyObject *locals,
                                     PyObject *fromlist, int level)
{
    PyObject *nameobj, *mod;
    nameobj = PyUnicode_FromString(name);
    if (nameobj == NULL)
        return NULL;
    mod = PyImport_ImportModuleLevelObject(nameobj, globals, locals, fromlist, level);
    Py_DECREF(nameobj);
    return mod;
}

PyObject *PyImport_ImportModuleAttr(PyObject *modname, PyObject *attrname)
{
    PyObject *mod = PyImport_Import(modname), *result;
    if (mod == NULL)
        return NULL;
    result = PyObject_GetAttr(mod, attrname);
    Py_DECREF(mod);
    return result;
}

PyObject *PyImport_ImportModuleAttrString(const char *modname, const char *attrname)
{
    PyObject *pmodname, *pattrname, *result;
    pmodname = PyUnicode_FromString(modname);
    if (pmodname == NULL)
        return NULL;
    pattrname = PyUnicode_FromString(attrname);
    if (pattrname == NULL) {
        Py_DECREF(pmodname);
        return NULL;
    }
    result = PyImport_ImportModuleAttr(pmodname, pattrname);
    Py_DECREF(pattrname);
    Py_DECREF(pmodname);
    return result;
}

PyObject *PyImport_ReloadModule(PyObject *m)
{
    PyObject *reload = PyImport_ImportModuleAttrString("importlib", "reload"), *res;
    if (reload == NULL)
        return NULL;
    res = PyObject_CallOneArg(reload, m);
    Py_DECREF(reload);
    return res;
}

PyAPI_FUNC(int) _PyImport_SetModule(PyObject *name, PyObject *m);
PyAPI_FUNC(int) _PyImport_SetModuleString(const char *name, PyObject *m);

int _PyImport_SetModule(PyObject *name, PyObject *m)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue nv = qp_capi_value(ctx, name), mv = qp_capi_value(ctx, m);
    JSValue modules = QP_STATE(ctx)->modules;
    if (JS_IsException(nv) || JS_IsException(mv))
        return -1;
    if (JS_VALUE_GET_TAG(modules) != JS_TAG_OBJECT) {
        PyErr_SetString(PyExc_RuntimeError, "no import module dictionary");
        return -1;
    }
    return qp_dict_set_item(ctx, modules, nv, mv);
}

int _PyImport_SetModuleString(const char *name, PyObject *m)
{
    PyObject *nameobj = PyUnicode_FromString(name);
    int r;
    if (nameobj == NULL)
        return -1;
    r = _PyImport_SetModule(nameobj, m);
    Py_DECREF(nameobj);
    return r;
}

/* PEP 810 lazy imports: QPState.lazy_imports_mode (0 normal, 1 all, the
   PyImport_LazyImportsMode values) and QPState.lazy_imports_filter, the
   state of sys.set_lazy_imports() / sys.set_lazy_imports_filter(). */
int PyImport_SetLazyImportsMode(PyImport_LazyImportsMode mode)
{
    QP_STATE(qp_capi.ctx)->lazy_imports_mode = mode;
    return 0;
}

PyImport_LazyImportsMode PyImport_GetLazyImportsMode(void)
{
    return (PyImport_LazyImportsMode)QP_STATE(qp_capi.ctx)->lazy_imports_mode;
}

int PyImport_SetLazyImportsFilter(PyObject *filter)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = JS_UNDEFINED;

    if (filter != NULL && filter != Py_None) {
        v = qp_capi_value(ctx, filter);
        if (JS_IsException(v))
            return -1;
    }
    return qp_sys_lazy_imports_filter_set(ctx, v);
}

/* New reference, or NULL without an exception when no filter is set. */
PyObject *PyImport_GetLazyImportsFilter(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValueConst filter = QP_STATE(ctx)->lazy_imports_filter;

    if (JS_IsUndefined(filter))
        return NULL;
    return qp_capi_new_ref(ctx, filter);
}

/* ========================================================================= */
/* sysmodule.h, audit.h                                                       */
/* ========================================================================= */

/* sys dict (borrowed JS value) or QP_NULL with RuntimeError "no sys module". */
static JSValue qp_capi_sysdict(void)
{
    JSValue d = QP_STATE(qp_capi.ctx)->sys_dict;
    if (JS_VALUE_GET_TAG(d) != JS_TAG_OBJECT) {
        PyErr_SetString(PyExc_RuntimeError, "no sys module");
        return QP_NULL;
    }
    return d;
}

/* PyDict_GetItemRef on the sys dict: 1 found (*value new), 0 missing, -1. */
static int qp_capi_sys_lookup(PyObject *name, PyObject **value)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d = qp_capi_sysdict(), nv, v;
    int rc;

    *value = NULL;
    if (QP_IS_NULL(d))
        return -1;
    nv = qp_capi_value(ctx, name);
    if (JS_IsException(nv))
        return -1;
    rc = qp_dict_get_item(ctx, d, nv, &v);
    if (rc <= 0)
        return rc;
    *value = qp_capi_steal(ctx, v);
    return *value ? 1 : -1;
}

PyObject *PySys_GetAttr(PyObject *name)
{
    PyObject *value;
    int rc;
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%T'", name);
        return NULL;
    }
    rc = qp_capi_sys_lookup(name, &value);
    if (rc == 0)
        PyErr_Format(PyExc_RuntimeError, "lost sys.%U", name);
    return value;
}

PyObject *PySys_GetAttrString(const char *name)
{
    PyObject *key = PyUnicode_FromString(name), *value;
    int rc;
    if (key == NULL)
        return NULL;
    rc = qp_capi_sys_lookup(key, &value);
    Py_DECREF(key);
    if (rc == 0)
        PyErr_Format(PyExc_RuntimeError, "lost sys.%s", name);
    return value;
}

int PySys_GetOptionalAttr(PyObject *name, PyObject **value)
{
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%T'", name);
        *value = NULL;
        return -1;
    }
    if (JS_VALUE_GET_TAG(QP_STATE(qp_capi.ctx)->sys_dict) != JS_TAG_OBJECT) {
        *value = NULL;
        return 0;
    }
    return qp_capi_sys_lookup(name, value);
}

int PySys_GetOptionalAttrString(const char *name, PyObject **value)
{
    PyObject *key;
    int rc;
    if (JS_VALUE_GET_TAG(QP_STATE(qp_capi.ctx)->sys_dict) != JS_TAG_OBJECT) {
        *value = NULL;
        return 0;
    }
    key = PyUnicode_FromString(name);
    if (key == NULL) {
        *value = NULL;
        return -1;
    }
    rc = qp_capi_sys_lookup(key, value);
    Py_DECREF(key);
    return rc;
}

PyObject *PySys_GetObject(const char *name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d = QP_STATE(ctx)->sys_dict, key, v = QP_NULL;
    PyObject *exc, *result = NULL;
    int rc;

    if (JS_VALUE_GET_TAG(d) != JS_TAG_OBJECT)
        return NULL;
    exc = PyErr_GetRaisedException();
    key = qp_str_from_cstr(ctx, name);
    rc = JS_IsException(key) ? -1 : qp_dict_get_item(ctx, d, key, &v);
    JS_FreeValue(ctx, key);
    if (rc > 0) {
        result = qp_capi_rt_borrow(qp_capi_rt_key_str("sys", name), v);
        JS_FreeValue(ctx, v);
    }
    /* suppress a new exception and restore the old one */
    if (PyErr_Occurred())
        PyErr_FormatUnraisable("Exception ignored in PySys_GetObject()");
    PyErr_SetRaisedException(exc);
    return result;
}

static int qp_capi_sys_set_object(PyObject *key, PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, kv, vv;
    if (key == NULL)
        return -1;
    d = qp_capi_sysdict();
    if (QP_IS_NULL(d))
        return -1;
    kv = qp_capi_value(ctx, key);
    if (JS_IsException(kv))
        return -1;
    if (v == NULL) {
        JSValue old;
        if (qp_dict_pop(ctx, d, kv, &old) < 0)
            return -1;
        JS_FreeValue(ctx, old);
        return 0;
    }
    vv = qp_capi_value(ctx, v);
    if (JS_IsException(vv))
        return -1;
    return qp_dict_set_item(ctx, d, kv, vv);
}

int PySys_SetObject(const char *name, PyObject *v)
{
    PyObject *key = v ? PyUnicode_InternFromString(name) : PyUnicode_FromString(name);
    int r = qp_capi_sys_set_object(key, v);
    Py_XDECREF(key);
    return r;
}

PyObject *PySys_GetXOptions(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d = QP_STATE(ctx)->sys_dict, key, v = QP_NULL, nd;
    PyObject *result = NULL;
    int rc;

    if (JS_VALUE_GET_TAG(d) != JS_TAG_OBJECT)
        return NULL;
    key = qp_str_from_cstr(ctx, "_xoptions");
    if (JS_IsException(key))
        return NULL;
    rc = qp_dict_get_item(ctx, d, key, &v);
    if (rc == 0 || (rc > 0 && !qp_dict_check(ctx, v))) {
        /* missing or replaced: a fresh dict, as CPython get_xoptions() */
        JS_FreeValue(ctx, v);
        nd = qp_dict_new(ctx);
        if (JS_IsException(nd) || qp_dict_set_item(ctx, d, key, nd) < 0) {
            JS_FreeValue(ctx, nd);
            JS_FreeValue(ctx, key);
            return NULL;
        }
        v = nd;
        rc = 1;
    }
    JS_FreeValue(ctx, key);
    if (rc > 0) {
        result = qp_capi_rt_borrow(qp_capi_rt_key_str("sys", "_xoptions"), v);
        JS_FreeValue(ctx, v);
    }
    return result;
}

/* sys_pyfile_write_unicode */
static int qp_capi_sys_pyfile_write_unicode(PyObject *unicode, PyObject *file)
{
    PyObject *result;
    if (file == NULL)
        return -1;
    result = PyObject_CallMethod(file, "write", "O", unicode);
    if (result == NULL)
        return -1;
    Py_DECREF(result);
    return 0;
}

static int qp_capi_sys_pyfile_write(const char *text, PyObject *file)
{
    PyObject *unicode;
    int err;
    if (file == NULL)
        return -1;
    unicode = PyUnicode_FromString(text);
    if (unicode == NULL)
        return -1;
    err = qp_capi_sys_pyfile_write_unicode(unicode, file);
    Py_DECREF(unicode);
    return err;
}

static void qp_capi_sys_write(const char *key, FILE *fp, const char *format, va_list va)
{
    PyObject *file, *exc;
    char buffer[1001];
    int written;

    exc = PyErr_GetRaisedException();
    written = PyOS_vsnprintf(buffer, sizeof(buffer), format, va);
    file = PySys_GetAttrString(key);
    if (qp_capi_sys_pyfile_write(buffer, file) != 0) {
        PyErr_Clear();
        fputs(buffer, fp);
    }
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        const char *truncated = "... truncated";
        if (qp_capi_sys_pyfile_write(truncated, file) != 0) {
            PyErr_Clear();
            fputs(truncated, fp);
        }
    }
    Py_XDECREF(file);
    PyErr_SetRaisedException(exc);
}

void PySys_WriteStdout(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    qp_capi_sys_write("stdout", stdout, format, va);
    va_end(va);
}

void PySys_WriteStderr(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    qp_capi_sys_write("stderr", stderr, format, va);
    va_end(va);
}

static void qp_capi_sys_format(const char *key, FILE *fp, const char *format, va_list va)
{
    PyObject *file, *message, *exc;
    const char *utf8;

    exc = PyErr_GetRaisedException();
    message = PyUnicode_FromFormatV(format, va);
    if (message != NULL) {
        file = PySys_GetAttrString(key);
        if (qp_capi_sys_pyfile_write_unicode(message, file) != 0) {
            PyErr_Clear();
            utf8 = PyUnicode_AsUTF8(message);
            if (utf8 != NULL)
                fputs(utf8, fp);
        }
        Py_XDECREF(file);
        Py_DECREF(message);
    }
    PyErr_SetRaisedException(exc);
}

void PySys_FormatStdout(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    qp_capi_sys_format("stdout", stdout, format, va);
    va_end(va);
}

void PySys_FormatStderr(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    qp_capi_sys_format("stderr", stderr, format, va);
    va_end(va);
}

PyAPI_FUNC(size_t) _PySys_GetSizeOf(PyObject *o);

size_t _PySys_GetSizeOf(PyObject *o)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, o), r;
    JSValueConst args[1];
    PyObject *res;
    Py_ssize_t size;

    if (JS_IsException(v))
        return (size_t)-1;
    args[0] = v;
    /* sys.getsizeof(o) without a default: __sizeof__ + the pre-header */
    r = qp_sys_getsizeof(ctx, JS_UNDEFINED, args, 1, QP_NULL, 0);
    res = qp_capi_from_js_result(ctx, r);
    if (res == NULL)
        return (size_t)-1;
    size = PyLong_AsSsize_t(res);
    Py_DECREF(res);
    if (size == -1 && PyErr_Occurred())
        return (size_t)-1;
    return (size_t)size;
}

/* C audit hooks (process-wide, may be added before the C API is bound). */
typedef struct QPCapiAuditHook {
    struct QPCapiAuditHook *next;
    Py_AuditHookFunction hook;
    void *user_data;
} QPCapiAuditHook;
static QPCapiAuditHook *qp_capi_audit_hooks;
static pthread_mutex_t qp_capi_audit_lock = PTHREAD_MUTEX_INITIALIZER;

static int qp_capi_should_audit(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue hooks;
    if (qp_capi_audit_hooks)
        return 1;
    if (ctx == NULL)
        return 0;
    hooks = QP_STATE(ctx)->audit_hooks;
    return JS_VALUE_GET_TAG(hooks) == JS_TAG_OBJECT && qp_list_size(hooks) > 0;
}

/* sys_audit_tstate: eventArgs consumed */
static int qp_capi_sys_audit_args(const char *event, PyObject *eventArgs)
{
    JSContext *ctx = qp_capi.ctx;
    PyThreadState *ts = qp_capi.tstate_current;
    PyObject *exc, *eventName = NULL, *hooks = NULL, *hook = NULL, *o;
    QPCapiAuditHook *e;
    int res = -1, canTrace;

    exc = PyErr_GetRaisedException();
    if (!eventArgs)
        goto exit;
    /* global (C) hooks */
    for (e = qp_capi_audit_hooks; e; e = e->next) {
        if (e->hook(event, eventArgs, e->user_data) < 0)
            goto exit;
    }
    /* interpreter (Python) hooks */
    if (JS_VALUE_GET_TAG(QP_STATE(ctx)->audit_hooks) == JS_TAG_OBJECT &&
        qp_list_size(QP_STATE(ctx)->audit_hooks) > 0) {
        PyObject *list = qp_capi_new_ref(ctx, QP_STATE(ctx)->audit_hooks);
        if (list == NULL)
            goto exit;
        eventName = PyUnicode_FromString(event);
        if (!eventName) {
            Py_DECREF(list);
            goto exit;
        }
        hooks = PyObject_GetIter(list);
        Py_DECREF(list);
        if (!hooks)
            goto exit;
        /* disallow tracing in hooks unless explicitly enabled */
        PyThreadState_EnterTracing(ts);
        while ((hook = PyIter_Next(hooks)) != NULL) {
            PyObject *args[2];
            canTrace = PyObject_GetOptionalAttrString(hook, "__cantrace__", &o);
            if (o) {
                canTrace = PyObject_IsTrue(o);
                Py_DECREF(o);
            }
            if (canTrace < 0)
                break;
            if (canTrace)
                PyThreadState_LeaveTracing(ts);
            args[0] = eventName;
            args[1] = eventArgs;
            o = PyObject_Vectorcall(hook, args, 2, NULL);
            if (canTrace)
                PyThreadState_EnterTracing(ts);
            if (!o)
                break;
            Py_DECREF(o);
            Py_CLEAR(hook);
        }
        PyThreadState_LeaveTracing(ts);
        if (PyErr_Occurred())
            goto exit;
    }
    res = 0;
exit:
    Py_XDECREF(hook);
    Py_XDECREF(hooks);
    Py_XDECREF(eventName);
    Py_XDECREF(eventArgs);
    if (!res)
        PyErr_SetRaisedException(exc);
    else
        Py_XDECREF(exc);
    return res;
}

static int qp_capi_sys_audit(const char *event, const char *argFormat, va_list vargs)
{
    PyObject *eventArgs;

    if (qp_capi.tstate_current == NULL)
        return 0;  /* audit hooks cannot be called without a thread state */
    if (!qp_capi_should_audit())
        return 0;
    if (argFormat && argFormat[0]) {
        PyObject *exc = PyErr_GetRaisedException();
        eventArgs = Py_VaBuildValue(argFormat, vargs);
        if (eventArgs && !PyTuple_Check(eventArgs)) {
            PyObject *argTuple = PyTuple_Pack(1, eventArgs);
            Py_SETREF(eventArgs, argTuple);
        }
        if (eventArgs == NULL) {
            Py_XDECREF(exc);
            return -1;
        }
        PyErr_SetRaisedException(exc);
    } else {
        eventArgs = PyTuple_New(0);
        if (eventArgs == NULL)
            return -1;
    }
    return qp_capi_sys_audit_args(event, eventArgs);
}

int PySys_Audit(const char *event, const char *argFormat, ...)
{
    va_list vargs;
    int res;
    va_start(vargs, argFormat);
    res = qp_capi_sys_audit(event, argFormat, vargs);
    va_end(vargs);
    return res;
}

int PySys_AuditTuple(const char *event, PyObject *args)
{
    if (args == NULL)
        return PySys_Audit(event, NULL);
    if (!PyTuple_Check(args)) {
        PyErr_Format(PyExc_TypeError, "args must be tuple, got %s", Py_TYPE(args)->tp_name);
        return -1;
    }
    return PySys_Audit(event, "O", args);
}

int PySys_AddAuditHook(Py_AuditHookFunction hook, void *userData)
{
    QPCapiAuditHook *e, **pp;

    /* invoke existing hooks first; they may abort the addition */
    if (qp_capi.tstate_current != NULL) {
        if (PySys_Audit("sys.addaudithook", NULL) < 0) {
            if (PyErr_ExceptionMatches(PyExc_RuntimeError)) {
                /* errors derived from RuntimeError are not reported */
                PyErr_Clear();
                return 0;
            }
            return -1;
        }
    }
    e = PyMem_RawMalloc(sizeof(*e));
    if (!e) {
        if (qp_capi.tstate_current != NULL)
            PyErr_NoMemory();
        return -1;
    }
    e->next = NULL;
    e->hook = hook;
    e->user_data = userData;
    pthread_mutex_lock(&qp_capi_audit_lock);
    for (pp = &qp_capi_audit_hooks; *pp; pp = &(*pp)->next)
        ;
    *pp = e;
    pthread_mutex_unlock(&qp_capi_audit_lock);
    return 0;
}

/* ========================================================================= */
/* PEP 741 runtime configuration (CPython 3.15.2 Python/initconfig.c          */
/* PYCONFIG_SPEC + PYPRECONFIG_SPEC, config_get, PyConfig_Set) over QPConfig  */
/* ========================================================================= */

typedef enum {
    QP_CFG_INT, QP_CFG_UINT, QP_CFG_ULONG, QP_CFG_BOOL,
    QP_CFG_WSTR, QP_CFG_WSTR_OPT, QP_CFG_WSTR_LIST,
} QPConfigType;

/* PyConfig_MEMBER_READ_ONLY / PUBLIC (the INIT_ONLY members are not
   reachable by name: config_generic_find_spec skips them) */
typedef enum { QP_CFG_READ_ONLY, QP_CFG_PUBLIC } QPConfigVisibility;

/* Members read from the state (QP_CFGM_NONE: the value is the sys
   attribute of the spec) */
typedef enum {
    QP_CFGM_NONE,
    /* public */
    QP_CFGM_bytes_warning, QP_CFGM_cpu_count, QP_CFGM_lazy_imports, QP_CFGM_inspect,
    QP_CFGM_int_max_str_digits, QP_CFGM_interactive, QP_CFGM_optimization_level,
    QP_CFGM_parser_debug, QP_CFGM_quiet, QP_CFGM_use_environment, QP_CFGM_verbose,
    QP_CFGM_write_bytecode,
    /* read-only */
    QP_CFGM_buffered_stdio, QP_CFGM_check_hash_pycs_mode, QP_CFGM_code_debug_ranges,
    QP_CFGM_dev_mode, QP_CFGM_filesystem_encoding, QP_CFGM_filesystem_errors,
    QP_CFGM_hash_seed, QP_CFGM_home, QP_CFGM_import_time, QP_CFGM_isolated,
    QP_CFGM_parse_argv, QP_CFGM_program_name, QP_CFGM_run_command, QP_CFGM_run_filename,
    QP_CFGM_run_module, QP_CFGM_safe_path, QP_CFGM_site_import,
    QP_CFGM_skip_source_first_line, QP_CFGM_stdio_encoding, QP_CFGM_stdio_errors,
    QP_CFGM_tracemalloc, QP_CFGM_use_frozen_modules, QP_CFGM_use_hash_seed,
    QP_CFGM_user_site_directory, QP_CFGM_warn_default_encoding,
    QP_CFGM_configure_locale, QP_CFGM_utf8_mode,
    /* features this build does not have: their fixed value is spec->value */
    QP_CFGM_CONST,
} QPConfigMember;

typedef struct {
    const char *name;
    unsigned char type;         /* QPConfigType */
    unsigned char visibility;   /* QPConfigVisibility */
    unsigned char member;       /* QPConfigMember */
    unsigned char sys_flag;     /* PyConfig_Set updates sys.flags (qp_sys_make_flags reads the member) */
    const char *sys_attr;       /* the value is this sys attribute (get and set) */
    int *global_var;            /* deprecated global configuration variable (Py_VerboseFlag...) */
    unsigned char global_not;   /* the global variable is the negation */
    signed char value;          /* QP_CFGM_CONST value */
} QPConfigSpec;

#define QP_CFG_SYS(name, type, vis, attr) \
    { #name, QP_CFG_##type, QP_CFG_##vis, QP_CFGM_NONE, 0, attr, NULL, 0, 0 }
#define QP_CFG_MEMBER(name, type, vis) \
    { #name, QP_CFG_##type, QP_CFG_##vis, QP_CFGM_##name, 0, NULL, NULL, 0, 0 }
#define QP_CFG_FLAG(name, type, global, not) \
    { #name, QP_CFG_##type, QP_CFG_PUBLIC, QP_CFGM_##name, 1, NULL, global, not, 0 }
#define QP_CFG_CONST(name, type, value) \
    { #name, QP_CFG_##type, QP_CFG_READ_ONLY, QP_CFGM_CONST, 0, NULL, NULL, 0, value }

_Py_COMP_DIAG_PUSH
_Py_COMP_DIAG_IGNORE_DEPR_DECLS
static const QPConfigSpec qp_config_spec[] = {
    /* --- public options (PYCONFIG_SPEC) --- */
    QP_CFG_SYS(argv, WSTR_LIST, PUBLIC, "argv"),
    QP_CFG_SYS(base_exec_prefix, WSTR_OPT, PUBLIC, "base_exec_prefix"),
    QP_CFG_SYS(base_executable, WSTR_OPT, PUBLIC, "_base_executable"),
    QP_CFG_SYS(base_prefix, WSTR_OPT, PUBLIC, "base_prefix"),
    QP_CFG_FLAG(bytes_warning, UINT, &Py_BytesWarningFlag, 0),
    QP_CFG_MEMBER(cpu_count, INT, PUBLIC),
    QP_CFG_MEMBER(lazy_imports, INT, PUBLIC),
    QP_CFG_SYS(exec_prefix, WSTR_OPT, PUBLIC, "exec_prefix"),
    QP_CFG_SYS(executable, WSTR_OPT, PUBLIC, "executable"),
    QP_CFG_FLAG(inspect, BOOL, &Py_InspectFlag, 0),
    QP_CFG_MEMBER(int_max_str_digits, UINT, PUBLIC),
    QP_CFG_FLAG(interactive, BOOL, &Py_InteractiveFlag, 0),
    QP_CFG_SYS(module_search_paths, WSTR_LIST, PUBLIC, "path"),
    QP_CFG_FLAG(optimization_level, UINT, &Py_OptimizeFlag, 0),
    QP_CFG_FLAG(parser_debug, BOOL, &Py_DebugFlag, 0),
    QP_CFG_SYS(platlibdir, WSTR, PUBLIC, "platlibdir"),
    QP_CFG_SYS(prefix, WSTR_OPT, PUBLIC, "prefix"),
    QP_CFG_SYS(pycache_prefix, WSTR_OPT, PUBLIC, "pycache_prefix"),
    QP_CFG_FLAG(quiet, BOOL, &Py_QuietFlag, 0),
    QP_CFG_SYS(stdlib_dir, WSTR_OPT, PUBLIC, "_stdlib_dir"),
    QP_CFG_FLAG(use_environment, BOOL, &Py_IgnoreEnvironmentFlag, 1),
    QP_CFG_FLAG(verbose, UINT, &Py_VerboseFlag, 0),
    QP_CFG_SYS(warnoptions, WSTR_LIST, PUBLIC, "warnoptions"),
    QP_CFG_FLAG(write_bytecode, BOOL, &Py_DontWriteBytecodeFlag, 1),
    QP_CFG_SYS(xoptions, WSTR_LIST, PUBLIC, "_xoptions"),

    /* --- read-only options --- */
    QP_CFG_MEMBER(buffered_stdio, BOOL, READ_ONLY),
    QP_CFG_MEMBER(check_hash_pycs_mode, WSTR, READ_ONLY),
    QP_CFG_MEMBER(code_debug_ranges, BOOL, READ_ONLY),
    QP_CFG_CONST(configure_c_stdio, BOOL, 0),       /* the C streams keep their libc setup */
    QP_CFG_MEMBER(dev_mode, BOOL, READ_ONLY),
    QP_CFG_CONST(dump_refs, BOOL, 0),               /* no Py_TRACE_REFS */
    QP_CFG_CONST(dump_refs_file, WSTR_OPT, 0),
    QP_CFG_CONST(faulthandler, BOOL, 0),            /* no faulthandler module */
    QP_CFG_MEMBER(filesystem_encoding, WSTR, READ_ONLY),
    QP_CFG_MEMBER(filesystem_errors, WSTR, READ_ONLY),
    QP_CFG_MEMBER(hash_seed, ULONG, READ_ONLY),
    QP_CFG_MEMBER(home, WSTR_OPT, READ_ONLY),
    QP_CFG_CONST(thread_inherit_context, INT, 0),   /* sys.flags.thread_inherit_context */
    QP_CFG_CONST(context_aware_warnings, INT, 0),   /* sys.flags.context_aware_warnings */
    QP_CFG_MEMBER(import_time, UINT, READ_ONLY),
    QP_CFG_CONST(install_signal_handlers, BOOL, 1), /* the importlib boot imports _signal */
    QP_CFG_MEMBER(isolated, BOOL, READ_ONLY),
    QP_CFG_CONST(malloc_stats, BOOL, 0),            /* no pymalloc */
    QP_CFG_CONST(pymalloc_hugepages, BOOL, 0),
    QP_CFG_SYS(orig_argv, WSTR_LIST, READ_ONLY, "orig_argv"),
    QP_CFG_MEMBER(parse_argv, BOOL, READ_ONLY),
    QP_CFG_CONST(pathconfig_warnings, BOOL, 1),     /* !Py_FrozenFlag */
    QP_CFG_CONST(perf_profiling, UINT, 0),          /* no perf trampoline */
    QP_CFG_CONST(remote_debug, BOOL, 0),            /* no sys.remote_exec */
    QP_CFG_MEMBER(program_name, WSTR, READ_ONLY),
    QP_CFG_MEMBER(run_command, WSTR_OPT, READ_ONLY),
    QP_CFG_MEMBER(run_filename, WSTR_OPT, READ_ONLY),
    QP_CFG_MEMBER(run_module, WSTR_OPT, READ_ONLY),
    QP_CFG_MEMBER(safe_path, BOOL, READ_ONLY),
    QP_CFG_CONST(show_ref_count, BOOL, 0),          /* no Py_REF_DEBUG */
    QP_CFG_MEMBER(site_import, BOOL, READ_ONLY),
    QP_CFG_MEMBER(skip_source_first_line, BOOL, READ_ONLY),
    QP_CFG_MEMBER(stdio_encoding, WSTR, READ_ONLY),
    QP_CFG_MEMBER(stdio_errors, WSTR, READ_ONLY),
    QP_CFG_MEMBER(tracemalloc, UINT, READ_ONLY),
    QP_CFG_MEMBER(use_frozen_modules, BOOL, READ_ONLY),
    QP_CFG_MEMBER(use_hash_seed, BOOL, READ_ONLY),
#ifdef __APPLE__
    QP_CFG_CONST(use_system_logger, BOOL, 0),       /* stdout/stderr are not copied to the system log */
#endif
    QP_CFG_MEMBER(user_site_directory, BOOL, READ_ONLY),
    QP_CFG_MEMBER(warn_default_encoding, BOOL, READ_ONLY),

    /* --- read-only options of PYPRECONFIG_SPEC --- */
    QP_CFG_CONST(allocator, INT, 0),                /* PYMEM_ALLOCATOR_NOT_SET: no PYTHONMALLOC */
    QP_CFG_CONST(coerce_c_locale, BOOL, 0),         /* no PEP 538 locale coercion */
    QP_CFG_CONST(coerce_c_locale_warn, BOOL, 0),
    QP_CFG_MEMBER(configure_locale, BOOL, READ_ONLY),
    QP_CFG_MEMBER(utf8_mode, BOOL, READ_ONLY),
    { NULL },
};
_Py_COMP_DIAG_POP

#undef QP_CFG_SYS
#undef QP_CFG_MEMBER
#undef QP_CFG_FLAG
#undef QP_CFG_CONST

static const QPConfigSpec *qp_config_find_spec(const char *name)
{
    const QPConfigSpec *spec;

    for (spec = qp_config_spec; spec->name; spec++) {
        if (strcmp(spec->name, name) == 0)
            return spec;
    }
    return NULL;
}

static void qp_config_unknown_name_error(const char *name)
{
    PyErr_Format(PyExc_ValueError, "unknown config option name: %s", name);
}

/* PYTHONHASHSEED value: 0 when unset or "random" */
static unsigned long qp_config_hash_seed(const QPConfig *c)
{
    if (c->hash_seed == NULL || strcmp(c->hash_seed, "random") == 0)
        return 0;
    return strtoul(c->hash_seed, NULL, 10);
}

/* config_get(config, spec, use_sys=1) */
static PyObject *qp_config_get(const QPConfigSpec *spec)
{
    JSContext *ctx = qp_capi.ctx;
    QPState *st = QP_STATE(ctx);
    const QPConfig *c = &st->config;
    char encoding_buf[QP_IO_ENCODING_BUF], errors_buf[QP_IO_ERRORS_BUF];
    const char *s = NULL, *encoding, *errors;
    PyObject *attr;
    long v = 0;
    int is_true;

    if (spec->sys_attr)
        return PySys_GetAttrString(spec->sys_attr);
    switch ((QPConfigMember)spec->member) {
    case QP_CFGM_bytes_warning: v = c->bytes_warning; break;
    case QP_CFGM_cpu_count: v = c->cpu_count > 0 ? c->cpu_count : -1; break;
    case QP_CFGM_lazy_imports: v = c->lazy_imports; break;
    case QP_CFGM_inspect: v = c->inspect; break;
    case QP_CFGM_int_max_str_digits: v = st->int_max_str_digits; break;
    case QP_CFGM_interactive: v = c->interactive; break;
    case QP_CFGM_optimization_level: v = c->optimize; break;
    case QP_CFGM_parser_debug: v = c->parser_debug; break;
    case QP_CFGM_quiet: v = c->quiet; break;
    case QP_CFGM_use_environment: v = !c->ignore_environment; break;
    case QP_CFGM_verbose: v = c->verbose; break;
    case QP_CFGM_write_bytecode:
        /* config_get_sys_write_bytecode: not sys.dont_write_bytecode */
        attr = PySys_GetAttrString("dont_write_bytecode");
        if (attr == NULL)
            return NULL;
        is_true = PyObject_IsTrue(attr);
        Py_DECREF(attr);
        if (is_true < 0)
            return NULL;
        v = !is_true;
        break;
    case QP_CFGM_buffered_stdio: v = !c->unbuffered; break;
    case QP_CFGM_check_hash_pycs_mode:
        s = c->check_hash_based_pycs ? c->check_hash_based_pycs : "default";
        break;
    case QP_CFGM_code_debug_ranges: v = !c->no_debug_ranges; break;
    case QP_CFGM_dev_mode: v = c->dev_mode; break;
    /* sys.getfilesystemencoding() / getfilesystemencodeerrors() */
    case QP_CFGM_filesystem_encoding: s = "utf-8"; break;
    case QP_CFGM_filesystem_errors: s = "surrogateescape"; break;
    case QP_CFGM_hash_seed: return PyLong_FromUnsignedLong(qp_config_hash_seed(c));
    case QP_CFGM_home: s = c->home; break;
    case QP_CFGM_import_time: v = c->import_time; break;
    case QP_CFGM_isolated: v = c->isolated; break;
    case QP_CFGM_parse_argv: v = c->parse_argv; break;
    case QP_CFGM_program_name:
        /* config_init_program_name: argv[0], else "python3" */
        s = c->orig_argc > 0 && c->orig_argv[0] && c->orig_argv[0][0] ? c->orig_argv[0]
                                                                       : "python3";
        break;
    case QP_CFGM_run_command: s = c->run_command; break;  /* the -c text and "\n" */
    case QP_CFGM_run_filename: s = c->run_filename; break;
    case QP_CFGM_run_module: s = c->run_module; break;
    case QP_CFGM_safe_path: v = c->safe_path; break;
    case QP_CFGM_site_import: v = !c->no_site; break;
    case QP_CFGM_skip_source_first_line: v = c->skip_source_first_line; break;
    case QP_CFGM_stdio_encoding:
    case QP_CFGM_stdio_errors:
        if (qp_io_stdio_config(ctx, c, encoding_buf, errors_buf, &encoding, &errors) < 0)
            return NULL;
        return PyUnicode_FromString(spec->member == QP_CFGM_stdio_encoding ? encoding : errors);
    case QP_CFGM_tracemalloc: v = c->tracemalloc; break;
    case QP_CFGM_use_frozen_modules: v = c->use_frozen_modules; break;
    case QP_CFGM_use_hash_seed:
        v = c->hash_seed != NULL && strcmp(c->hash_seed, "random") != 0;
        break;
    case QP_CFGM_user_site_directory: v = !c->no_user_site; break;
    case QP_CFGM_warn_default_encoding: v = c->warn_default_encoding; break;
    case QP_CFGM_configure_locale: v = qp_preconfig_configure_locale; break;
    case QP_CFGM_utf8_mode: v = c->utf8_mode > 0; break;
    case QP_CFGM_CONST: v = spec->value; break;
    case QP_CFGM_NONE: break;
    }
    switch ((QPConfigType)spec->type) {
    case QP_CFG_BOOL:
        return PyBool_FromLong(v != 0);
    case QP_CFG_WSTR:
    case QP_CFG_WSTR_OPT:
        if (s == NULL)
            Py_RETURN_NONE;
        return PyUnicode_DecodeFSDefault(s);
    default:
        return PyLong_FromLong(v);
    }
}

PyObject *PyConfig_Get(const char *name)
{
    const QPConfigSpec *spec = qp_config_find_spec(name);

    if (spec == NULL) {
        qp_config_unknown_name_error(name);
        return NULL;
    }
    return qp_config_get(spec);
}

int PyConfig_GetInt(const char *name, int *value)
{
    PyObject *obj = PyConfig_Get(name);
    int as_int;

    if (obj == NULL)
        return -1;
    if (!PyLong_Check(obj)) {
        Py_DECREF(obj);
        PyErr_Format(PyExc_TypeError, "config option %s is not an int", name);
        return -1;
    }
    as_int = PyLong_AsInt(obj);
    Py_DECREF(obj);
    if (as_int == -1 && PyErr_Occurred()) {
        PyErr_Format(PyExc_OverflowError,
                     "config option %s value does not fit into a C int", name);
        return -1;
    }
    *value = as_int;
    return 0;
}

PyObject *PyConfig_Names(void)
{
    const QPConfigSpec *spec;
    PyObject *names = PyList_New(0), *name, *frozen;
    int res;

    if (names == NULL)
        return NULL;
    for (spec = qp_config_spec; spec->name; spec++) {
        name = PyUnicode_FromString(spec->name);
        if (name == NULL)
            goto error;
        res = PyList_Append(names, name);
        Py_DECREF(name);
        if (res < 0)
            goto error;
    }
    frozen = PyFrozenSet_New(names);
    Py_DECREF(names);
    return frozen;
 error:
    Py_DECREF(names);
    return NULL;
}

/* Set the QPConfig field of an int member (config_set_int_attr and the
   member half of config_set_sys_flag). */
static void qp_config_set_member(const QPConfigSpec *spec, int value)
{
    QPConfig *c = &QP_STATE(qp_capi.ctx)->config;

    switch ((QPConfigMember)spec->member) {
    case QP_CFGM_bytes_warning: c->bytes_warning = value; break;
    case QP_CFGM_cpu_count: c->cpu_count = value; break;
    case QP_CFGM_lazy_imports: c->lazy_imports = value; break;
    case QP_CFGM_inspect: c->inspect = value; break;
    case QP_CFGM_interactive: c->interactive = value; break;
    case QP_CFGM_optimization_level: c->optimize = value; break;
    case QP_CFGM_parser_debug: c->parser_debug = value; break;
    case QP_CFGM_quiet: c->quiet = value; break;
    case QP_CFGM_use_environment: c->ignore_environment = !value; break;
    case QP_CFGM_verbose: c->verbose = value; break;
    case QP_CFGM_write_bytecode: c->dont_write_bytecode = !value; break;
    default: Py_UNREACHABLE();
    }
}

/* config_set_sys_flag: the member, then sys.flags (rebuilt from the
   configuration like _PySys_SetFlagObj replaces it) and, for
   write_bytecode, sys.dont_write_bytecode. */
static int qp_config_set_sys_flag(const QPConfigSpec *spec, int int_value)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *value;
    int res;

    if (spec->type == QP_CFG_BOOL && int_value != 0)
        int_value = 1;  /* convert values < 0 and values > 1 to 1 */
    qp_config_set_member(spec, int_value);
    if (qp_sys_update_flags(ctx) < 0)
        return -1;
    if (spec->member != QP_CFGM_write_bytecode)
        return 0;
    /* the value of sys.flags.dont_write_bytecode (config_sys_flag_not) */
    value = PyLong_FromLong(!int_value);
    if (value == NULL)
        return -1;
    res = PySys_SetObject("dont_write_bytecode", value);
    Py_DECREF(value);
    return res;
}

int PyConfig_Set(const char *name, PyObject *value)
{
    const QPConfigSpec *spec;
    PyObject *key, *item;
    Py_ssize_t i, pos;
    int int_value = 0, has_int_value = 0;

    if (PySys_Audit("cpython.PyConfig_Set", "sO", name, value) < 0)
        return -1;
    spec = qp_config_find_spec(name);
    if (spec == NULL) {
        qp_config_unknown_name_error(name);
        return -1;
    }
    if (spec->visibility != QP_CFG_PUBLIC) {
        PyErr_Format(PyExc_ValueError, "cannot set read-only option %s", name);
        return -1;
    }

    switch ((QPConfigType)spec->type) {
    case QP_CFG_INT:
    case QP_CFG_UINT:
    case QP_CFG_BOOL:
        if (!PyLong_Check(value)) {
            PyErr_Format(PyExc_TypeError, "expected int or bool, got %T", value);
            return -1;
        }
        int_value = PyLong_AsInt(value);
        if (int_value == -1 && PyErr_Occurred())
            return -1;
        if (int_value < 0 && spec->type != QP_CFG_INT) {
            PyErr_Format(PyExc_ValueError, "value must be >= 0");
            return -1;
        }
        has_int_value = 1;
        break;
    case QP_CFG_ULONG:
        /* not implemented: only hash_seed uses this type, and it's read-only */
        goto cannot_set;
    case QP_CFG_WSTR:
        if (!PyUnicode_CheckExact(value)) {
            PyErr_Format(PyExc_TypeError, "expected str, got %T", value);
            return -1;
        }
        break;
    case QP_CFG_WSTR_OPT:
        if (value != Py_None && !PyUnicode_CheckExact(value)) {
            PyErr_Format(PyExc_TypeError, "expected str or None, got %T", value);
            return -1;
        }
        break;
    case QP_CFG_WSTR_LIST:
        if (strcmp(spec->name, "xoptions") != 0) {
            if (!PyList_Check(value)) {
                PyErr_Format(PyExc_TypeError, "expected list[str], got %T", value);
                return -1;
            }
            for (i = 0; i < PyList_GET_SIZE(value); i++) {
                item = PyList_GET_ITEM(value, i);
                if (!PyUnicode_Check(item)) {
                    PyErr_Format(PyExc_TypeError, "expected str, list item %zd has type %T",
                                 i, item);
                    return -1;
                }
            }
        } else {
            /* xoptions type is dict[str, str] */
            if (!PyDict_Check(value)) {
                PyErr_Format(PyExc_TypeError, "expected dict[str, str | bool], got %T", value);
                return -1;
            }
            pos = 0;
            while (PyDict_Next(value, &pos, &key, &item)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_Format(PyExc_TypeError, "expected str, got dict key type %T", key);
                    return -1;
                }
                if (!PyUnicode_Check(item) && !PyBool_Check(item)) {
                    /* CPython reports the type of the key here */
                    PyErr_Format(PyExc_TypeError, "expected str or bool, got dict value type %T",
                                 key);
                    return -1;
                }
            }
        }
        break;
    }

    /* Set the global variable */
    if (spec->global_var != NULL)
        *spec->global_var = spec->global_not ? !int_value : int_value;  /* deprecated */

    if (spec->sys_attr != NULL) {
        /* Set the sys attribute (like CPython, not the configuration) */
        return PySys_SetObject(spec->sys_attr, value);
    }
    if (has_int_value) {
        if (spec->sys_flag)
            return qp_config_set_sys_flag(spec, int_value);
        if (spec->member == QP_CFGM_int_max_str_digits)
            return qp_sys_int_max_str_digits_set(qp_capi.ctx, int_value);
        qp_config_set_member(spec, int_value);
        return 0;
    }
 cannot_set:
    PyErr_Format(PyExc_ValueError, "cannot set option %s", name);
    return -1;
}

/* ========================================================================= */
/* pylifecycle.h, PyStatus, fatal errors                                      */
/* ========================================================================= */

const unsigned long Py_Version = PY_VERSION_HEX;

int Py_IsInitialized(void)
{
    return qp_capi.initialized == 2;
}

int Py_IsFinalizing(void)
{
    return qp_capi_finalizing;
}

#define QP_CAPI_NEXITFUNCS 32
static void (*qp_capi_exitfuncs[QP_CAPI_NEXITFUNCS])(void);
static int qp_capi_nexitfuncs;
static int qp_capi_exitfuncs_registered;

/* call_ll_exitfuncs: the Py_AtExit functions, last registered first, after
   the interpreter shut down (C atexit). */
static void qp_capi_call_ll_exitfuncs(void)
{
    while (qp_capi_nexitfuncs > 0) {
        void (*exitfunc)(void) = qp_capi_exitfuncs[--qp_capi_nexitfuncs];
        qp_capi_exitfuncs[qp_capi_nexitfuncs] = NULL;
        exitfunc();
    }
    fflush(stdout);
    fflush(stderr);
}

/* PyUnstable_AtExit (Modules/atexitmodule.c): low-level callbacks of the
   interpreter, last registered first, run by qp_atexit_run after the
   Python atexit functions (_PyAtExit_Call) */
typedef struct QPCapiAtExitCallback {
    atexit_datacallbackfunc func;
    void *data;
    struct QPCapiAtExitCallback *next;
} QPCapiAtExitCallback;
static QPCapiAtExitCallback *qp_capi_ll_callbacks;

int PyUnstable_AtExit(PyInterpreterState *interp, atexit_datacallbackfunc func, void *data)
{
    QPCapiAtExitCallback *cb;

    if (interp != PyInterpreterState_Get()) {
        PyErr_SetString(PyExc_SystemError, "PyUnstable_AtExit: not the current interpreter");
        return -1;
    }
    cb = PyMem_RawMalloc(sizeof(*cb));
    if (!cb) {
        PyErr_NoMemory();
        return -1;
    }
    cb->func = func;
    cb->data = data;
    pthread_mutex_lock(&qp_capi_pending_lock);
    cb->next = qp_capi_ll_callbacks;
    qp_capi_ll_callbacks = cb;
    pthread_mutex_unlock(&qp_capi_pending_lock);
    return 0;
}

static void qp_capi_atexit_ll_run(JSContext *ctx)
{
    QPCapiAtExitCallback *cb;

    if (ctx != qp_capi.ctx)
        return;
    for (;;) {
        pthread_mutex_lock(&qp_capi_pending_lock);
        cb = qp_capi_ll_callbacks;
        if (cb)
            qp_capi_ll_callbacks = cb->next;
        pthread_mutex_unlock(&qp_capi_pending_lock);
        if (!cb)
            break;
        cb->func(cb->data);
        PyMem_RawFree(cb);
    }
}

int Py_AtExit(void (*func)(void))
{
    if (qp_capi_nexitfuncs >= QP_CAPI_NEXITFUNCS)
        return -1;
    if (!qp_capi_exitfuncs_registered) {
        if (atexit(qp_capi_call_ll_exitfuncs) != 0)
            return -1;
        qp_capi_exitfuncs_registered = 1;
    }
    qp_capi_exitfuncs[qp_capi_nexitfuncs++] = func;
    return 0;
}

void _Py_NO_RETURN Py_Exit(int sts)
{
    JSContext *ctx = qp_capi.ctx;
    if (ctx != NULL && qp_capi.tstate_current != NULL && qp_capi_rt_is_main_thread()) {
        qp_capi_finalizing = 1;
        qp_atexit_run(ctx);
        qp_run_flush_io(ctx);
    }
    exit(sts);
}

/* fatal_error (python/mod_faulthandler.c) */
static void _Py_NO_RETURN qp_capi_fatal_error(const char *func, const char *msg)
{
    qp_fatal_error(qp_capi.ctx, func, msg);
}

void _Py_NO_RETURN _Py_FatalErrorFunc(const char *func, const char *message)
{
    qp_capi_fatal_error(func, message);
}

void _Py_NO_RETURN (Py_FatalError)(const char *msg)
{
    qp_capi_fatal_error(NULL, msg);
}

PyAPI_FUNC(void) _Py_NO_RETURN _Py_FatalErrorFormat(const char *func, const char *format, ...);

void _Py_NO_RETURN _Py_FatalErrorFormat(const char *func, const char *format, ...)
{
    char buf[1024];
    va_list vargs;
    va_start(vargs, format);
    vsnprintf(buf, sizeof(buf), format, vargs);
    va_end(vargs);
    qp_capi_fatal_error(func, buf);
}

PyStatus PyStatus_Ok(void)
{
    return (PyStatus){ ._type = _PyStatus_TYPE_OK };
}

PyStatus PyStatus_Error(const char *err_msg)
{
    return (PyStatus){ ._type = _PyStatus_TYPE_ERROR, .err_msg = err_msg };
}

PyStatus PyStatus_NoMemory(void)
{
    return PyStatus_Error("memory allocation failed");
}

PyStatus PyStatus_Exit(int exitcode)
{
    return (PyStatus){ ._type = _PyStatus_TYPE_EXIT, .exitcode = exitcode };
}

int PyStatus_IsError(PyStatus status)
{
    return status._type == _PyStatus_TYPE_ERROR;
}

int PyStatus_IsExit(PyStatus status)
{
    return status._type == _PyStatus_TYPE_EXIT;
}

int PyStatus_Exception(PyStatus status)
{
    return status._type != _PyStatus_TYPE_OK;
}

void _Py_NO_RETURN Py_ExitStatusException(PyStatus status)
{
    if (status._type == _PyStatus_TYPE_EXIT)
        exit(status.exitcode);
    else if (status._type == _PyStatus_TYPE_ERROR)
        qp_capi_fatal_error(status.func, status.err_msg);
    else
        _Py_FatalErrorFunc("Py_ExitStatusException",
                           "Py_ExitStatusException() must not be called on success");
}

/* ========================================================================= */
/* contextvars (over our _contextvars module, python/mod_misc.c)              */
/* ========================================================================= */

PyTypeObject PyContext_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_contextvars.Context",
    .tp_basicsize = sizeof(PyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

PyTypeObject PyContextVar_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_contextvars.ContextVar",
    .tp_basicsize = sizeof(PyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

PyTypeObject PyContextToken_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_contextvars.Token",
    .tp_basicsize = sizeof(PyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

PyObject *PyContext_New(void)
{
    JSContext *ctx = qp_capi.ctx;
    QPModState *ms = qp_ctxvar_state(ctx);
    if (!ms)
        return NULL;
    return qp_capi_from_js_result(ctx, qp_context_new_from_vars(ctx, ms, QP_NULL));
}

PyObject *PyContext_Copy(PyObject *octx)
{
    if (!PyContext_CheckExact(octx)) {
        PyErr_SetString(PyExc_TypeError, "an instance of Context was expected");
        return NULL;
    }
    return PyObject_CallMethod(octx, "copy", NULL);
}

PyObject *PyContext_CopyCurrent(void)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_context_copy_current(ctx));
}

int PyContext_Enter(PyObject *octx)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, octx);
    if (JS_IsException(v))
        return -1;
    return qp_context_enter(ctx, v);
}

int PyContext_Exit(PyObject *octx)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, octx);
    if (JS_IsException(v))
        return -1;
    return qp_context_exit(ctx, v);
}

PyAPI_FUNC(int) _PyContext_Enter(PyThreadState *ts, PyObject *octx);
PyAPI_FUNC(int) _PyContext_Exit(PyThreadState *ts, PyObject *octx);

int _PyContext_Enter(PyThreadState *ts, PyObject *octx)
{
    return PyContext_Enter(octx);
}

int _PyContext_Exit(PyThreadState *ts, PyObject *octx)
{
    return PyContext_Exit(octx);
}

PyObject *PyContextVar_New(const char *name, PyObject *def)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue dv = qp_capi_rt_opt(def);
    if (JS_IsException(dv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_contextvar_new(ctx, name, dv));
}

int PyContextVar_Get(PyObject *ovar, PyObject *def, PyObject **val)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue var = qp_capi_value(ctx, ovar), dv = qp_capi_rt_opt(def), r;

    *val = NULL;
    if (JS_IsException(var) || JS_IsException(dv))
        return -1;
    if (qp_contextvar_get(ctx, var, dv, &r) < 0)
        return -1;
    if (QP_IS_NULL(r))
        return 0;
    *val = qp_capi_steal(ctx, r);
    return *val ? 0 : -1;
}

PyObject *PyContextVar_Set(PyObject *ovar, PyObject *val)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue var = qp_capi_value(ctx, ovar), vv = qp_capi_value(ctx, val);
    if (JS_IsException(var) || JS_IsException(vv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_contextvar_set(ctx, var, vv));
}

int PyContextVar_Reset(PyObject *ovar, PyObject *otok)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue var = qp_capi_value(ctx, ovar), tok = qp_capi_value(ctx, otok);
    if (JS_IsException(var) || JS_IsException(tok))
        return -1;
    return qp_contextvar_reset(ctx, var, tok);
}

/* ---- context watchers (Python/context.c) -------------------------------- */

#define QP_CAPI_CONTEXT_MAX_WATCHERS 8      /* CONTEXT_MAX_WATCHERS */
static PyContext_WatchCallback qp_capi_context_watchers[QP_CAPI_CONTEXT_MAX_WATCHERS];

static const char *qp_capi_context_event_name(PyContextEvent event)
{
    switch (event) {
    case Py_CONTEXT_SWITCHED:
        return "Py_CONTEXT_SWITCHED";
    default:
        return "?";
    }
}

/* notify_context_watchers(ts, Py_CONTEXT_SWITCHED, ts->context), called
   by _PyContext_Enter / _PyContext_Exit (mod_misc.c) */
static void qp_capi_context_switched(JSContext *ctx, JSValueConst current)
{
    unsigned bits = qp_capi_context_watchers_set;
    PyObject *co;
    int i;

    if (!bits || qp_capi_dead)
        return;
    /* None after exiting the last context of the stack (no base context
       was created) */
    co = QP_IS_NULL(current) ? Py_NewRef(Py_None) : qp_capi_new_ref(ctx, current);
    if (!co) {
        PyErr_FormatUnraisable("Exception ignored in %s watcher callback",
                               qp_capi_context_event_name(Py_CONTEXT_SWITCHED));
        return;
    }
    for (i = 0; bits; i++, bits >>= 1) {
        PyContext_WatchCallback cb;
        if (!(bits & 1))
            continue;
        /* A previous callback may have cleared this watcher. */
        cb = qp_capi_context_watchers[i];
        if (cb && cb(Py_CONTEXT_SWITCHED, co) < 0)
            PyErr_FormatUnraisable("Exception ignored in %s watcher callback for %R",
                                   qp_capi_context_event_name(Py_CONTEXT_SWITCHED), co);
    }
    Py_DECREF(co);
}

int PyContext_AddWatcher(PyContext_WatchCallback callback)
{
    int i;
    for (i = 0; i < QP_CAPI_CONTEXT_MAX_WATCHERS; i++) {
        if (!qp_capi_context_watchers[i]) {
            qp_capi_context_watchers[i] = callback;
            qp_capi_context_watchers_set |= (uint8_t)(1 << i);
            return i;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "no more context watcher IDs available");
    return -1;
}

int PyContext_ClearWatcher(int watcher_id)
{
    if (watcher_id < 0 || watcher_id >= QP_CAPI_CONTEXT_MAX_WATCHERS) {
        PyErr_Format(PyExc_ValueError, "Invalid context watcher ID %d", watcher_id);
        return -1;
    }
    if (!qp_capi_context_watchers[watcher_id]) {
        PyErr_Format(PyExc_ValueError, "No context watcher set for ID %d", watcher_id);
        return -1;
    }
    qp_capi_context_watchers[watcher_id] = NULL;
    qp_capi_context_watchers_set &= (uint8_t)~(1 << watcher_id);
    return 0;
}

/* pycore_context.h (dquickts): _testcapi's clear_context_stack over our
   context stack (QPModState.ctx_current, CPython tstate->context): 1 if a
   non-base context is entered (the current one has a previous context;
   nothing changes), else 0 after dropping the current context, so that the
   next context access creates a fresh base context. */
PyAPI_FUNC(int) _PyQp_ClearContextStack(void);
int _PyQp_ClearContextStack(void)
{
    JSContext *ctx = qp_capi.ctx;
    QPModState *ms = qp_ctxvar_state(ctx);
    JSValue cur;

    if (!ms)
        return -1;
    cur = ms->ctx_current;
    if (QP_IS_NULL(cur))
        return 0;
    if (!QP_IS_NULL(qp_instance_slots(cur)[QP_CONTEXT_SLOT_PREV]))
        return 1;
    ms->ctx_current = QP_NULL;
    JS_FreeValue(ctx, cur);
    return 0;
}

/* ========================================================================= */
/* generators (pycore_genobject.h helpers, genobject.h)                       */
/* ========================================================================= */

PyAPI_FUNC(int) _PyGen_SetStopIterationValue(PyObject *value);
PyAPI_FUNC(int) _PyGen_FetchStopIterationValue(PyObject **pvalue);

int _PyGen_SetStopIterationValue(PyObject *value)
{
    /* an explicit instance: PyErr_SetObject(StopIteration, tuple) would
       unpack a tuple value */
    PyObject *exc = value == NULL ? PyObject_CallNoArgs(PyExc_StopIteration)
                                  : PyObject_CallOneArg(PyExc_StopIteration, value);
    if (exc == NULL)
        return -1;
    PyErr_SetRaisedException(exc);
    return 0;
}

int _PyGen_FetchStopIterationValue(PyObject **pvalue)
{
    PyObject *value = NULL;
    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        JSContext *ctx = qp_capi.ctx;
        PyObject *exc = PyErr_GetRaisedException();
        JSValue ev = qp_capi_value(ctx, exc);
        if (JS_IsException(ev)) {
            Py_DECREF(exc);
            return -1;
        }
        value = qp_capi_steal(ctx, qp_exc_stopiteration_value(ctx, ev));
        Py_DECREF(exc);
        if (value == NULL)
            return -1;
    } else if (PyErr_Occurred()) {
        return -1;
    }
    if (value == NULL)
        value = Py_NewRef(Py_None);
    *pvalue = value;
    return 0;
}

PyCodeObject *PyGen_GetCode(PyGenObject *gen)
{
    return (PyCodeObject *)PyObject_GetAttrString((PyObject *)gen, "gi_code");
}

/* ========================================================================= */
/* code objects: kind CODE, code.h                                            */
/* ========================================================================= */

/* pycore_code.h local kinds */
#define QP_CAPI_CO_FAST_ARG_POS 0x02
#define QP_CAPI_CO_FAST_ARG_KW  0x04
#define QP_CAPI_CO_FAST_ARG_VAR 0x08
#define QP_CAPI_CO_FAST_LOCAL   0x20
#define QP_CAPI_CO_FAST_CELL    0x40
#define QP_CAPI_CO_FAST_FREE    0x80

static size_t qp_capi_code_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyCodeObject) - sizeof(PyObject);
}

static int qp_capi_code_int(JSContext *ctx, JSValueConst v, const char *name, int *out)
{
    PyObject *o = qp_capi_rt_getattr(ctx, v, name);
    long x;
    if (o == NULL)
        return -1;
    x = PyLong_AsLong(o);
    Py_DECREF(o);
    if (x == -1 && PyErr_Occurred())
        return -1;
    *out = (int)x;
    return 0;
}

/* PEP 523 co_extra (Objects/codeobject.c): co_extra of the code mirror
   holds a QPCapiCodeExtra; the mirror is PERSIST once an extra is set, so
   it lives as long as the code object, and the free functions run when it
   is released (code_dealloc). */
#define QP_CAPI_MAX_CO_EXTRA_USERS 255
typedef struct {
    Py_ssize_t ce_size;
    void *ce_extras[1];
} QPCapiCodeExtra;
static freefunc qp_capi_co_extra_freefuncs[QP_CAPI_MAX_CO_EXTRA_USERS];
static Py_ssize_t qp_capi_co_extra_user_count;

static void qp_capi_code_free_extras(PyCodeObject *co)
{
    QPCapiCodeExtra *ce = co->co_extra;
    Py_ssize_t i;
    if (!ce)
        return;
    co->co_extra = NULL;
    for (i = 0; i < ce->ce_size; i++) {
        freefunc free_extra = qp_capi_co_extra_freefuncs[i];
        if (free_extra && ce->ce_extras[i])
            free_extra(ce->ce_extras[i]);
    }
    PyMem_RawFree(ce);
}

/* PyUnstable_GC_VisitObjects (Python/gc.c): every object gc.get_objects()
   lists (a snapshot, so the callback may allocate), then the tracked
   extension instances that have no Python identity yet; stops when the
   callback returns 0 */
void PyUnstable_GC_VisitObjects(gcvisitobjects_t callback, void *arg)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue *items = NULL;
    Py_ssize_t n, i;
    int go_on = 1;
    struct list_head *el;

    n = qp_gc_snapshot(ctx, &items);
    if (n < 0) {
        PyErr_Clear();
        return;
    }
    for (i = 0; i < n; i++) {
        PyObject *op = go_on ? qp_capi_new_ref(ctx, items[i]) : NULL;
        JS_FreeValue(ctx, items[i]);
        if (!op) {
            if (go_on)
                PyErr_Clear();
            continue;
        }
        go_on = callback(op, arg);
        Py_DECREF(op);
    }
    qp_sys_free(items);
    if (!go_on)
        return;
    list_for_each(el, &qp_capi.live_list) {
        QPCapiHead *h = list_entry(el, QPCapiHead, live_link);
        if ((h->flags & (QP_CAPI_F_NATIVE | QP_CAPI_F_TRACKED)) == (QP_CAPI_F_NATIVE | QP_CAPI_F_TRACKED) &&
            QP_IS_NULL(h->v) && !callback(QP_CAPI_OBJ(h), arg))
            return;
    }
}

Py_ssize_t PyUnstable_Eval_RequestCodeExtraIndex(freefunc free)
{
    Py_ssize_t new_index;
    if (qp_capi_co_extra_user_count >= QP_CAPI_MAX_CO_EXTRA_USERS - 1)
        return -1;
    new_index = qp_capi_co_extra_user_count++;
    qp_capi_co_extra_freefuncs[new_index] = free;
    return new_index;
}

int PyUnstable_Code_GetExtra(PyObject *code, Py_ssize_t index, void **extra)
{
    QPCapiCodeExtra *ce;
    if (!PyCode_Check(code)) {
        PyErr_BadInternalCall();
        return -1;
    }
    ce = ((PyCodeObject *)code)->co_extra;
    *extra = (ce && index >= 0 && index < ce->ce_size) ? ce->ce_extras[index] : NULL;
    return 0;
}

int PyUnstable_Code_SetExtra(PyObject *code, Py_ssize_t index, void *extra)
{
    PyCodeObject *co = (PyCodeObject *)code;
    QPCapiCodeExtra *ce;

    if (!PyCode_Check(code) || index < 0 || index >= qp_capi_co_extra_user_count) {
        PyErr_BadInternalCall();
        return -1;
    }
    ce = co->co_extra;
    if (!ce || ce->ce_size <= index) {
        Py_ssize_t old = ce ? ce->ce_size : 0, i;
        ce = PyMem_RawRealloc(ce, sizeof(QPCapiCodeExtra) + (size_t)qp_capi_co_extra_user_count * sizeof(void *));
        if (!ce) {
            PyErr_NoMemory();
            return -1;
        }
        for (i = old; i < qp_capi_co_extra_user_count; i++)
            ce->ce_extras[i] = NULL;
        ce->ce_size = qp_capi_co_extra_user_count;
        co->co_extra = ce;
        if (QP_CAPI_HAS_HEAD(co))
            QP_CAPI_HEAD(co)->flags |= QP_CAPI_F_PERSIST;
    }
    if (ce->ce_extras[index] && qp_capi_co_extra_freefuncs[index])
        qp_capi_co_extra_freefuncs[index](ce->ce_extras[index]);
    ce->ce_extras[index] = extra;
    return 0;
}

static void qp_capi_code_clear(JSRuntime *rt, PyObject *op)
{
    PyCodeObject *co = (PyCodeObject *)op;
    qp_capi_code_free_extras(co);
    Py_CLEAR(co->co_consts);
    Py_CLEAR(co->co_names);
    Py_CLEAR(co->co_exceptiontable);
    Py_CLEAR(co->co_localsplusnames);
    Py_CLEAR(co->co_localspluskinds);
    Py_CLEAR(co->co_filename);
    Py_CLEAR(co->co_name);
    Py_CLEAR(co->co_qualname);
    Py_CLEAR(co->co_linetable);
}

static int qp_capi_code_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyCodeObject *co = (PyCodeObject *)op;
    Py_VISIT(co->co_consts);
    Py_VISIT(co->co_names);
    Py_VISIT(co->co_exceptiontable);
    Py_VISIT(co->co_localsplusnames);
    Py_VISIT(co->co_localspluskinds);
    Py_VISIT(co->co_filename);
    Py_VISIT(co->co_name);
    Py_VISIT(co->co_qualname);
    Py_VISIT(co->co_linetable);
    return 0;
}

/* co_localsplusnames / co_localspluskinds and the counts derived from
   co_varnames, co_cellvars, co_freevars (CPython _PyCode_Validate layout:
   locals first, then cells that are not arguments/locals, then frees). */
static int qp_capi_code_localsplus(PyCodeObject *co, PyObject *varnames, PyObject *cellvars,
                                   PyObject *freevars)
{
    Py_ssize_t nv = PyTuple_GET_SIZE(varnames), nc = PyTuple_GET_SIZE(cellvars);
    Py_ssize_t nf = PyTuple_GET_SIZE(freevars), n = 0, i, j, total = nv + nc + nf;
    PyObject *names = PyTuple_New(total), *kinds;
    char *k;
    int nargs, flags = co->co_flags;

    if (names == NULL)
        return -1;
    kinds = PyBytes_FromStringAndSize(NULL, total);
    if (kinds == NULL) {
        Py_DECREF(names);
        return -1;
    }
    k = PyBytes_AS_STRING(kinds);
    nargs = co->co_argcount + co->co_kwonlyargcount +
        ((flags & CO_VARARGS) != 0) + ((flags & CO_VARKEYWORDS) != 0);
    for (i = 0; i < nv; i++) {
        PyObject *name = PyTuple_GET_ITEM(varnames, i);
        int kind = QP_CAPI_CO_FAST_LOCAL;
        if (i < nargs) {
            if (i < co->co_argcount)
                kind |= QP_CAPI_CO_FAST_ARG_POS | (i >= co->co_posonlyargcount ? QP_CAPI_CO_FAST_ARG_KW : 0);
            else if (i < co->co_argcount + co->co_kwonlyargcount)
                kind |= QP_CAPI_CO_FAST_ARG_KW;
            else
                kind |= QP_CAPI_CO_FAST_ARG_VAR |
                    ((flags & CO_VARARGS) && i == co->co_argcount + co->co_kwonlyargcount
                     ? QP_CAPI_CO_FAST_ARG_POS : QP_CAPI_CO_FAST_ARG_KW);
        }
        for (j = 0; j < nc; j++) {
            int eq = PyUnicode_Compare(name, PyTuple_GET_ITEM(cellvars, j));
            if (eq == -1 && PyErr_Occurred())
                goto error;
            if (eq == 0) {
                kind |= QP_CAPI_CO_FAST_CELL;
                break;
            }
        }
        PyTuple_SET_ITEM(names, n, Py_NewRef(name));
        k[n++] = (char)kind;
    }
    for (j = 0; j < nc; j++) {
        PyObject *name = PyTuple_GET_ITEM(cellvars, j);
        int found = 0;
        for (i = 0; i < nv; i++) {
            int eq = PyUnicode_Compare(name, PyTuple_GET_ITEM(varnames, i));
            if (eq == -1 && PyErr_Occurred())
                goto error;
            if (eq == 0) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;
        PyTuple_SET_ITEM(names, n, Py_NewRef(name));
        k[n++] = (char)QP_CAPI_CO_FAST_CELL;
    }
    for (j = 0; j < nf; j++) {
        PyTuple_SET_ITEM(names, n, Py_NewRef(PyTuple_GET_ITEM(freevars, j)));
        k[n++] = (char)QP_CAPI_CO_FAST_FREE;
    }
    if (n < total) {
        if (_PyTuple_Resize(&names, n) < 0)
            goto error;
        if (_PyBytes_Resize(&kinds, n) < 0) {
            Py_DECREF(names);
            return -1;
        }
    }
    co->co_localsplusnames = names;
    co->co_localspluskinds = kinds;
    co->co_nlocalsplus = (int)n;
    co->co_nlocals = (int)nv;
    co->co_ncellvars = (int)nc;
    co->co_nfreevars = (int)nf;
    co->co_framesize = (int)n + co->co_stacksize;
    return 0;
error:
    Py_XDECREF(names);
    Py_DECREF(kinds);
    return -1;
}

/* kind ops init: materialize the scalar co_* fields and the immutable
   object fields from the code object's attributes (owned references). */
static int qp_capi_code_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyCodeObject *co = (PyCodeObject *)op;
    PyObject *varnames = NULL, *cellvars = NULL, *freevars = NULL;
    QPCapiTupleFillSave fill;
    int r = -1;

    /* the name tuples are read below: fill them now even when this code
       object is materialized while filling a tuple (dis.Instruction) */
    qp_capi_tuple_fill_suspend(&fill);
    if (qp_capi_code_int(ctx, v, "co_flags", &co->co_flags) < 0 ||
        qp_capi_code_int(ctx, v, "co_argcount", &co->co_argcount) < 0 ||
        qp_capi_code_int(ctx, v, "co_posonlyargcount", &co->co_posonlyargcount) < 0 ||
        qp_capi_code_int(ctx, v, "co_kwonlyargcount", &co->co_kwonlyargcount) < 0 ||
        qp_capi_code_int(ctx, v, "co_stacksize", &co->co_stacksize) < 0 ||
        qp_capi_code_int(ctx, v, "co_firstlineno", &co->co_firstlineno) < 0)
        goto done;
    if (!(co->co_consts = qp_capi_rt_getattr(ctx, v, "co_consts")) ||
        !(co->co_names = qp_capi_rt_getattr(ctx, v, "co_names")) ||
        !(co->co_exceptiontable = qp_capi_rt_getattr(ctx, v, "co_exceptiontable")) ||
        !(co->co_filename = qp_capi_rt_getattr(ctx, v, "co_filename")) ||
        !(co->co_name = qp_capi_rt_getattr(ctx, v, "co_name")) ||
        !(co->co_qualname = qp_capi_rt_getattr(ctx, v, "co_qualname")) ||
        !(co->co_linetable = qp_capi_rt_getattr(ctx, v, "co_linetable")) ||
        !(varnames = qp_capi_rt_getattr(ctx, v, "co_varnames")) ||
        !(cellvars = qp_capi_rt_getattr(ctx, v, "co_cellvars")) ||
        !(freevars = qp_capi_rt_getattr(ctx, v, "co_freevars")))
        goto done;
    if (!PyTuple_Check(varnames) || !PyTuple_Check(cellvars) || !PyTuple_Check(freevars)) {
        PyErr_SetString(PyExc_SystemError, "code object: co_varnames/co_cellvars/co_freevars "
                        "must be tuples");
        goto done;
    }
    if (qp_capi_code_localsplus(co, varnames, cellvars, freevars) < 0)
        goto done;
    co->co_version = 0;
    co->_co_firsttraceable = 0;
    r = 0;
done:
    qp_capi_tuple_fill_resume(&fill);
    Py_XDECREF(varnames);
    Py_XDECREF(cellvars);
    Py_XDECREF(freevars);
    if (r < 0)
        qp_capi_code_clear(qp_capi.rt, op);
    return r;
}

static const QPCapiKindOps qp_capi_code_kind_ops = {
    .name = "code",
    .size = qp_capi_code_size,
    .init = qp_capi_code_init,
    .traverse = qp_capi_code_traverse,
    .clear = qp_capi_code_clear,
};

/* Bytecode of a code object mirror, NULL + TypeError. */
static struct JSFunctionBytecode *qp_capi_code_bc(PyCodeObject *co)
{
    JSValue v = qp_capi_value(qp_capi.ctx, (PyObject *)co);
    struct JSFunctionBytecode *b;
    if (JS_IsException(v))
        return NULL;
    b = qp_code_bytecode(v);
    if (b == NULL)
        PyErr_SetString(PyExc_TypeError, "expected a code object");
    return b;
}

PyCodeObject *PyCode_NewEmpty(const char *filename, const char *funcname, int firstlineno)
{
    JSContext *ctx = qp_capi.ctx;
    qp_error err;
    JSValue bc, code, name;
    QPCodeObject tmpl;

    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, "", 0, filename, QP_NULL, Py_file_input, 0, -1, &err);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        return NULL;
    }
    qp_error_clear(&err);
    name = qp_str_from_cstr(ctx, funcname);
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, bc);
        return NULL;
    }
    /* one code object, as code.replace(co_name=funcname,
       co_qualname=funcname, co_firstlineno=firstlineno) would make it (a
       code watcher sees a single PY_CODE_EVENT_CREATE) */
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.name = name;
    tmpl.qualname = name;
    tmpl.filename = QP_NULL;
    tmpl.firstlineno = firstlineno;
    tmpl.has_firstlineno = TRUE;
    tmpl.fields = QP_NULL;
    code = qp_co_alloc(ctx, bc, &tmpl);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, bc);
    return (PyCodeObject *)qp_capi_from_js_result(ctx, code);
}

/* ---- code watchers (Objects/codeobject.c) ------------------------------- */

/* PY_CODE_EVENT_CREATE is reported when a code object is made
   (qp_co_alloc: compile(), func.__code__, marshal, replace(), ...);
   PY_CODE_EVENT_DESTROY from the finalizer queue, to which the free of
   every code object is deferred while a code watcher is set
   (qp_capi_dealloc_watched), as CPython's code_dealloc resurrects it for
   the callbacks. */
#define QP_CAPI_CODE_MAX_WATCHERS 8         /* CODE_MAX_WATCHERS */
static PyCode_WatchCallback qp_capi_code_watchers[QP_CAPI_CODE_MAX_WATCHERS];

static const char *qp_capi_code_event_name(PyCodeEvent event)
{
    switch (event) {
#define QP_CAPI_CODE_EVENT_CASE(op) case PY_CODE_EVENT_##op: return "PY_CODE_EVENT_" #op;
    PY_FOREACH_CODE_EVENT(QP_CAPI_CODE_EVENT_CASE)
#undef QP_CAPI_CODE_EVENT_CASE
    }
    return "?";
}

/* notify_code_watchers */
static void qp_capi_code_notify(JSContext *ctx, JSValueConst code, PyCodeEvent event)
{
    unsigned bits = qp_capi_code_watchers_set;
    PyObject *co;
    int i;

    if (!bits || qp_capi_dead)
        return;
    co = qp_capi_new_ref(ctx, code);
    if (!co) {
        PyErr_FormatUnraisable("Exception ignored in %s watcher callback",
                               qp_capi_code_event_name(event));
        return;
    }
    for (i = 0; bits; i++, bits >>= 1) {
        PyCode_WatchCallback cb;
        if (!(bits & 1))
            continue;
        cb = qp_capi_code_watchers[i];
        if (cb && cb(event, (PyCodeObject *)co) < 0)
            PyErr_FormatUnraisable("Exception ignored in %s watcher callback for %R",
                                   qp_capi_code_event_name(event), co);
    }
    Py_DECREF(co);
}

static void qp_capi_code_created(JSContext *ctx, JSValueConst code)
{
    qp_capi_code_notify(ctx, code, PY_CODE_EVENT_CREATE);
}

int PyCode_AddWatcher(PyCode_WatchCallback callback)
{
    int i;
    for (i = 0; i < QP_CAPI_CODE_MAX_WATCHERS; i++) {
        if (!qp_capi_code_watchers[i]) {
            qp_capi_code_watchers[i] = callback;
            qp_capi_code_watchers_set |= (uint8_t)(1 << i);
            return i;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "no more code watcher IDs available");
    return -1;
}

int PyCode_ClearWatcher(int watcher_id)
{
    if (watcher_id < 0 || watcher_id >= QP_CAPI_CODE_MAX_WATCHERS) {
        PyErr_Format(PyExc_ValueError, "Invalid code watcher ID %d", watcher_id);
        return -1;
    }
    if (!qp_capi_code_watchers[watcher_id]) {
        PyErr_Format(PyExc_ValueError, "No code watcher set for ID %d", watcher_id);
        return -1;
    }
    qp_capi_code_watchers[watcher_id] = NULL;
    qp_capi_code_watchers_set &= (uint8_t)~(1 << watcher_id);
    return 0;
}

/* object.h hooks of qp_del_context / qp_run_pending_finalizers (type,
   function, dict and code deallocation events). */
static JSContext *qp_capi_dealloc_watched(JSObject *p)
{
    BOOL watched;

    if (qp_capi_dead || qp_capi.initialized != 2)
        return NULL;
    switch (p->class_id) {
    case JS_CLASS_PY_DICT: {
        QPDict *d = p->u.opaque;
        watched = d && (d->core.version & qp_capi_dict_watchers_set) != 0;
        break;
    }
    case JS_CLASS_PY_CODE:
        watched = qp_capi_code_watchers_set != 0;
        break;
    case JS_CLASS_PY_TYPE:
        watched = qp_capi_type_dealloc_watched(p);
        break;
    case JS_CLASS_BYTECODE_FUNCTION:
        watched = qp_capi_func_watchers_set && qp_function_data(JS_MKPTR(JS_TAG_OBJECT, p));
        break;
    default:
        watched = FALSE;
        break;
    }
    return watched ? qp_capi.ctx : NULL;
}

static void qp_capi_dealloc_notify(JSContext *ctx, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    switch (p->class_id) {
    case JS_CLASS_PY_DICT:
        qp_capi_dict_dealloc_notify(ctx, p);
        break;
    case JS_CLASS_PY_CODE:
        qp_capi_code_notify(ctx, obj, PY_CODE_EVENT_DESTROY);
        break;
    case JS_CLASS_PY_TYPE:
        qp_capi_type_dealloc_notify(ctx, obj);
        break;
    case JS_CLASS_BYTECODE_FUNCTION:
        qp_capi_func_event(ctx, PyFunction_EVENT_DESTROY, obj, QP_NULL);
        break;
    default:
        break;
    }
}

int PyCode_Addr2Line(PyCodeObject *co, int addrq)
{
    struct JSFunctionBytecode *b;
    QPLoc loc;
    int pc;
    if (addrq < 0)
        return co->co_firstlineno;
    b = qp_capi_code_bc(co);
    if (b == NULL) {
        PyErr_Clear();
        return -1;
    }
    /* addrq is a co_code offset (f_lasti) */
    pc = qp_code_lasti2pc(b, addrq);
    if (pc < 0 || qp_code_addr2loc(b, (uint32_t)pc, &loc) < 0)
        return -1;
    return loc.lineno;
}

int PyCode_Addr2Location(PyCodeObject *co, int addrq, int *start_line, int *start_column,
                         int *end_line, int *end_column)
{
    struct JSFunctionBytecode *b;
    QPLoc loc;
    int pc;
    if (addrq < 0) {
        *start_line = *end_line = co->co_firstlineno;
        *start_column = *end_column = 0;
        return 1;
    }
    b = qp_capi_code_bc(co);
    if (b == NULL) {
        PyErr_Clear();
        loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
    } else if ((pc = qp_code_lasti2pc(b, addrq)) < 0 ||
               qp_code_addr2loc(b, (uint32_t)pc, &loc) < 0) {
        loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
    }
    *start_line = loc.lineno;
    *start_column = loc.col;
    *end_line = loc.end_lineno;
    *end_column = loc.end_col;
    return 1;
}

PyObject *PyCode_GetCode(PyCodeObject *co)
{
    return PyObject_GetAttrString((PyObject *)co, "co_code");
}

PyObject *PyCode_GetVarnames(PyCodeObject *co)
{
    return PyObject_GetAttrString((PyObject *)co, "co_varnames");
}

PyObject *PyCode_GetCellvars(PyCodeObject *co)
{
    return PyObject_GetAttrString((PyObject *)co, "co_cellvars");
}

PyObject *PyCode_GetFreevars(PyCodeObject *co)
{
    return PyObject_GetAttrString((PyObject *)co, "co_freevars");
}

/* ========================================================================= */
/* frames (frameobject.h, pyframe.h): facades of python/frameobject.c         */
/* ========================================================================= */

/* frame attribute, None -> NULL without exception when none_is_null */
static PyObject *qp_capi_frame_attr(PyFrameObject *frame, const char *name, int none_is_null)
{
    PyObject *r = PyObject_GetAttrString((PyObject *)frame, name);
    if (r != NULL && none_is_null && Py_IsNone(r)) {
        Py_DECREF(r);
        return NULL;
    }
    return r;
}

static int qp_capi_frame_int(PyFrameObject *frame, const char *name)
{
    PyObject *r = qp_capi_frame_attr(frame, name, 1);
    long x;
    if (r == NULL) {
        PyErr_Clear();
        return -1;
    }
    x = PyLong_AsLong(r);
    Py_DECREF(r);
    if (x == -1 && PyErr_Occurred())
        PyErr_Clear();
    return (int)x;
}

int PyFrame_GetLineNumber(PyFrameObject *frame)
{
    return qp_capi_frame_int(frame, "f_lineno");
}

/* Python/pylifecycle.c (readline): the locale of the environment for
   category (not Android: setlocale(category, "")) */
PyAPI_FUNC(char *) _Py_SetLocaleFromEnv(int category);
char *_Py_SetLocaleFromEnv(int category)
{
    return setlocale(category, "");
}

/* ---- uid_t / gid_t (Modules/posixmodule.c: grp, pwd) --------------------
   The converters of the native posix module (python/mod_posix.c). */

PyObject *_PyLong_FromUid(uid_t uid)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_os_from_id(ctx, (uint32_t)uid));
}

PyObject *_PyLong_FromGid(gid_t gid)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_os_from_id(ctx, (uint32_t)gid));
}

static int qp_capi_id_converter(PyObject *obj, uint32_t *p, const char *what)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, obj);

    if (JS_IsException(v))
        return 0;
    return qp_os_id_conv(ctx, v, p, what) < 0 ? 0 : 1;
}

int _Py_Uid_Converter(PyObject *obj, uid_t *p)
{
    uint32_t v;
    if (!qp_capi_id_converter(obj, &v, "uid"))
        return 0;
    *p = (uid_t)v;
    return 1;
}

int _Py_Gid_Converter(PyObject *obj, gid_t *p)
{
    uint32_t v;
    if (!qp_capi_id_converter(obj, &v, "gid"))
        return 0;
    *p = (gid_t)v;
    return 1;
}

/* ---- PyOS_Readline (Parser/myreadline.c) ----------------------------------
   The readline module installs PyOS_ReadlineFunctionPointer; input() on the
   console (python/bltinmodule.c) and the basic REPL (python/pythonrun.c)
   read their lines through it once it is installed. */

PyThreadState *_PyOS_ReadlineTState = NULL;
static PyMutex qp_capi_readline_lock = {0};

static char *qp_capi_stdio_readline(FILE *sys_stdin, FILE *sys_stdout, const char *prompt);
char *(*PyOS_ReadlineFunctionPointer)(FILE *, FILE *, const char *) = NULL;

/* fgets() with the signal handling of my_fgets: 0 = line, -1 = EOF,
   1 = interrupt (exception set), -2 = error */
static int qp_capi_my_fgets(PyThreadState *tstate, char *buf, int len, FILE *fp)
{
    for (;;) {
        if (PyOS_InputHook != NULL)
            (void)(PyOS_InputHook)();
        errno = 0;
        clearerr(fp);
        if (fgets(buf, len, fp) != NULL)
            return 0;
        int err = errno;
        if (feof(fp)) {
            clearerr(fp);
            return -1;
        }
        if (err == EINTR) {
            PyEval_RestoreThread(tstate);
            int s = PyErr_CheckSignals();
            PyEval_SaveThread();
            if (s < 0)
                return 1;
            continue;
        }
        return -2;
    }
}

/* PyOS_StdioReadline: the prompt on stderr, one line from sys_stdin
   (PyMem_RawMalloc'ed, with its '\n'; "" at EOF; NULL after an
   interrupt).  Called without the GIL. */
static char *qp_capi_stdio_readline(FILE *sys_stdin, FILE *sys_stdout, const char *prompt)
{
    size_t n = 0;
    char *p = NULL, *pr;
    PyThreadState *tstate = _PyOS_ReadlineTState;

    fflush(sys_stdout);
    if (prompt)
        fprintf(stderr, "%s", prompt);
    fflush(stderr);
    do {
        size_t incr = (n > 0) ? n + 2 : 100;
        if (incr > INT_MAX) {
            PyMem_RawFree(p);
            PyEval_RestoreThread(tstate);
            PyErr_SetString(PyExc_OverflowError, "input line too long");
            PyEval_SaveThread();
            return NULL;
        }
        pr = PyMem_RawRealloc(p, n + incr);
        if (pr == NULL) {
            PyMem_RawFree(p);
            PyEval_RestoreThread(tstate);
            PyErr_NoMemory();
            PyEval_SaveThread();
            return NULL;
        }
        p = pr;
        int err = qp_capi_my_fgets(tstate, p + n, (int)incr, sys_stdin);
        if (err == 1) {
            PyMem_RawFree(p);
            return NULL;
        } else if (err != 0) {
            p[n] = '\0';
            break;
        }
        n += strlen(p + n);
    } while (n == 0 || p[n - 1] != '\n');
    pr = PyMem_RawRealloc(p, n + 1);
    if (pr == NULL) {
        PyMem_RawFree(p);
        PyEval_RestoreThread(tstate);
        PyErr_NoMemory();
        PyEval_SaveThread();
        return NULL;
    }
    return pr;
}

char *PyOS_Readline(FILE *sys_stdin, FILE *sys_stdout, const char *prompt)
{
    char *rv, *res;
    size_t len;
    PyThreadState *tstate = PyThreadState_Get();

    if (_PyOS_ReadlineTState == tstate) {
        PyErr_SetString(PyExc_RuntimeError, "can't re-enter readline");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    PyMutex_Lock(&qp_capi_readline_lock);
    _PyOS_ReadlineTState = tstate;
    if (PyOS_ReadlineFunctionPointer == NULL)
        PyOS_ReadlineFunctionPointer = qp_capi_stdio_readline;
    /* `python -i < file`: interactive mode without a console */
    if (!isatty(fileno(sys_stdin)) || !isatty(fileno(sys_stdout)))
        rv = qp_capi_stdio_readline(sys_stdin, sys_stdout, prompt);
    else
        rv = (*PyOS_ReadlineFunctionPointer)(sys_stdin, sys_stdout, prompt);
    _PyOS_ReadlineTState = NULL;
    PyMutex_Unlock(&qp_capi_readline_lock);
    Py_END_ALLOW_THREADS
    if (rv == NULL)
        return NULL;
    len = strlen(rv) + 1;
    res = PyMem_Malloc(len);
    if (res != NULL)
        memcpy(res, rv, len);
    else
        PyErr_NoMemory();
    PyMem_RawFree(rv);
    return res;
}

/* A line-editing hook (the readline module) is installed and the C API is
   bound to ctx: the console reads go through PyOS_Readline. */
static BOOL qp_capi_readline_hooked(JSContext *ctx)
{
    return PyOS_ReadlineFunctionPointer != NULL &&
        PyOS_ReadlineFunctionPointer != qp_capi_stdio_readline &&
        qp_capi.ctx == ctx;
}

/* The basic REPL's read of one line (python/pythonrun.c) through the hook:
   the line without its '\n' appended to line.  1 = line, 0 = EOF,
   -1 = exception. */
static int qp_capi_repl_readline(JSContext *ctx, const char *prompt, DynBuf *line)
{
    char *s = PyOS_Readline(stdin, stdout, prompt);
    size_t len;

    (void)ctx;
    if (s == NULL) {
        PyErr_CheckSignals();
        if (!PyErr_Occurred())
            PyErr_SetNone(PyExc_KeyboardInterrupt);
        return -1;
    }
    len = strlen(s);
    if (len == 0) {
        PyMem_Free(s);
        return 0;
    }
    if (s[len - 1] == '\n')
        len--;
    dbuf_put(line, (const uint8_t *)s, len);
    PyMem_Free(s);
    return 1;
}

/* input() on the console with the hook installed (CPython builtin_input_impl,
   the tty branch).  *handled = 0 when fin/fout are not the console: the
   caller then reads sys.stdin. */
static JSValue qp_capi_input_tty(JSContext *ctx, JSValueConst fin_v, JSValueConst fout_v,
                                 JSValueConst prompt_v, int *handled)
{
    PyObject *fin = NULL, *fout = NULL, *prompt = NULL, *tmp;
    PyObject *stdin_encoding = NULL, *stdin_errors = NULL;
    PyObject *stdout_encoding = NULL, *stdout_errors = NULL, *po = NULL;
    PyObject *result = NULL;
    const char *stdin_encoding_str, *stdin_errors_str, *promptstr;
    char *s = NULL;
    size_t len;
    long fd;
    int tty;

    *handled = 0;
    fin = qp_capi_new_ref(ctx, fin_v);
    fout = qp_capi_new_ref(ctx, fout_v);
    if (!fin || !fout)
        goto error;
    if (!QP_IS_NULL(prompt_v)) {
        prompt = qp_capi_new_ref(ctx, prompt_v);
        if (!prompt)
            goto error;
    }
    /* readline only for Python's sys.stdin / sys.stdout = C's stdin/stdout */
    tmp = PyObject_CallMethod(fin, "fileno", NULL);
    if (tmp == NULL) {
        PyErr_Clear();
        tty = 0;
    } else {
        fd = PyLong_AsLong(tmp);
        Py_DECREF(tmp);
        if (fd < 0 && PyErr_Occurred())
            goto error;
        tty = fd == fileno(stdin) && isatty(fd);
    }
    if (tty) {
        tmp = PyObject_CallMethod(fout, "fileno", NULL);
        if (tmp == NULL) {
            PyErr_Clear();
            tty = 0;
        } else {
            fd = PyLong_AsLong(tmp);
            Py_DECREF(tmp);
            if (fd < 0 && PyErr_Occurred())
                goto error;
            tty = fd == fileno(stdout) && isatty(fd);
        }
    }
    if (!tty)
        goto not_tty;
    stdin_encoding = PyObject_GetAttrString(fin, "encoding");
    stdin_errors = stdin_encoding ? PyObject_GetAttrString(fin, "errors") : NULL;
    if (!stdin_errors || !PyUnicode_Check(stdin_encoding) || !PyUnicode_Check(stdin_errors))
        goto not_tty_clear;
    stdin_encoding_str = PyUnicode_AsUTF8(stdin_encoding);
    stdin_errors_str = stdin_encoding_str ? PyUnicode_AsUTF8(stdin_errors) : NULL;
    if (!stdin_errors_str)
        goto error;
    tmp = PyObject_CallMethod(fout, "flush", NULL);
    if (tmp)
        Py_DECREF(tmp);
    else
        PyErr_Clear();
    if (prompt != NULL) {
        const char *stdout_encoding_str, *stdout_errors_str;
        PyObject *stringpo;
        stdout_encoding = PyObject_GetAttrString(fout, "encoding");
        stdout_errors = stdout_encoding ? PyObject_GetAttrString(fout, "errors") : NULL;
        if (!stdout_errors || !PyUnicode_Check(stdout_encoding) ||
            !PyUnicode_Check(stdout_errors))
            goto not_tty_clear;
        stdout_encoding_str = PyUnicode_AsUTF8(stdout_encoding);
        stdout_errors_str = stdout_encoding_str ? PyUnicode_AsUTF8(stdout_errors) : NULL;
        if (!stdout_errors_str)
            goto error;
        stringpo = PyObject_Str(prompt);
        if (stringpo == NULL)
            goto error;
        po = PyUnicode_AsEncodedString(stringpo, stdout_encoding_str, stdout_errors_str);
        Py_DECREF(stringpo);
        if (po == NULL)
            goto error;
        promptstr = PyBytes_AS_STRING(po);
        if ((Py_ssize_t)strlen(promptstr) != PyBytes_GET_SIZE(po)) {
            PyErr_SetString(PyExc_ValueError,
                            "input: prompt string cannot contain null characters");
            goto error;
        }
    } else {
        promptstr = "";
    }
    *handled = 1;
    s = PyOS_Readline(stdin, stdout, promptstr);
    if (s == NULL) {
        PyErr_CheckSignals();
        if (!PyErr_Occurred())
            PyErr_SetNone(PyExc_KeyboardInterrupt);
        goto error;
    }
    len = strlen(s);
    if (len == 0) {
        PyErr_SetNone(PyExc_EOFError);
        goto error;
    }
    if (len > PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "input: input too long");
        goto error;
    }
    len--;   /* strip trailing '\n' */
    if (len != 0 && s[len - 1] == '\r')
        len--;   /* strip trailing '\r' */
    result = PyUnicode_Decode(s, len, stdin_encoding_str, stdin_errors_str);
    goto done;
not_tty_clear:
    PyErr_Clear();
not_tty:
    *handled = 0;
    goto done;
error:
    *handled = 1;
done:
    PyMem_Free(s);
    Py_XDECREF(po);
    Py_XDECREF(stdout_encoding);
    Py_XDECREF(stdout_errors);
    Py_XDECREF(stdin_encoding);
    Py_XDECREF(stdin_errors);
    Py_XDECREF(prompt);
    Py_XDECREF(fin);
    Py_XDECREF(fout);
    if (!*handled)
        return JS_UNDEFINED;
    if (!result)
        return JS_EXCEPTION;
    JSValue r = qp_capi_value(ctx, result);
    r = JS_IsException(r) ? r : JS_DupValue(ctx, r);
    Py_DECREF(result);
    return r;
}

/* ---- C profile and trace functions (PyEval_SetProfile / PyEval_SetTrace) --
   The interpreter reports events to the Python-level sys.setprofile /
   sys.settrace callables (python/frameobject.c qp_trace_emit).  A C hook is
   installed as such a callable: a builtin whose self is a capsule holding
   (func, obj); it maps the event name to its PyTrace_* code and calls
   func(obj, frame, what, arg).  On 'call' a trace hook returns a builtin of
   the same capsule as the local trace function, so the frame also reports
   its 'line', 'return' and 'exception' events to it.  Deviation: sys.getprofile()/gettrace() return
   the hook builtin (CPython: obj). */

typedef struct {
    Py_tracefunc func;
    PyObject *obj;
    int is_trace;
} QPCapiTraceHook;

#define QP_CAPI_TRACE_HOOK "qp.trace_hook"

static void qp_capi_trace_hook_free(PyObject *capsule)
{
    QPCapiTraceHook *h = PyCapsule_GetPointer(capsule, QP_CAPI_TRACE_HOOK);

    if (h) {
        Py_XDECREF(h->obj);
        PyMem_RawFree(h);
    }
}

static PyMethodDef qp_capi_trace_hook_def;

static PyObject *qp_capi_trace_hook_call(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    static const char *const names[] = {
        "call", "exception", "line", "return", "c_call", "c_exception", "c_return", "opcode",
    };
    QPCapiTraceHook *h = PyCapsule_GetPointer(self, QP_CAPI_TRACE_HOOK);
    const char *event;
    int what;

    if (!h)
        return NULL;
    if (nargs != 3 || !PyUnicode_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError, "trace hook expects (frame, event, arg)");
        return NULL;
    }
    event = PyUnicode_AsUTF8(args[1]);
    if (!event)
        return NULL;
    for (what = 0; what < (int)countof(names); what++) {
        if (!strcmp(event, names[what]))
            break;
    }
    if (what == (int)countof(names)) {
        PyErr_Format(PyExc_ValueError, "unknown trace event '%s'", event);
        return NULL;
    }
    if (h->func(h->obj, (PyFrameObject *)args[0], what, args[2]) != 0)
        return NULL;
    /* trace_trampoline: the result of 'call' is the frame's f_trace, None
       keeps it; the profiler's result is ignored */
    if (h->is_trace && what == PyTrace_CALL)
        return PyCFunction_New(&qp_capi_trace_hook_def, self);
    Py_RETURN_NONE;
}

static PyMethodDef qp_capi_trace_hook_def = {
    "c_trace_hook", (PyCFunction)(void (*)(void))qp_capi_trace_hook_call, METH_FASTCALL,
    "C-level profile or trace function (PyEval_SetProfile, PyEval_SetTrace)."
};

/* which: 1 profile (else trace) | 2 all threads (*AllThreads) */
static int qp_capi_set_trace_hook(int which, Py_tracefunc func, PyObject *obj)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiTraceHook *h;
    PyObject *capsule, *hook;
    JSValue v;
    int r, is_profile = which & 1;

    if (qp_sys_audit_event(ctx, is_profile ? "sys.setprofile" : "sys.settrace") < 0)
        return -1;
    if (!func)
        return (which & 2) ? qp_trace_set_all(ctx, is_profile, QP_NULL) :
            qp_trace_set(ctx, is_profile, QP_NULL);
    h = PyMem_RawCalloc(1, sizeof(*h));
    if (!h) {
        PyErr_NoMemory();
        return -1;
    }
    capsule = PyCapsule_New(h, QP_CAPI_TRACE_HOOK, qp_capi_trace_hook_free);
    if (!capsule) {
        PyMem_RawFree(h);
        return -1;
    }
    h->func = func;
    h->obj = Py_XNewRef(obj);
    h->is_trace = !is_profile;
    hook = PyCFunction_New(&qp_capi_trace_hook_def, capsule);
    Py_DECREF(capsule);
    if (!hook)
        return -1;
    v = qp_capi_value(ctx, hook);
    r = JS_IsException(v) ? -1 :
        (which & 2) ? qp_trace_set_all(ctx, is_profile, v) : qp_trace_set(ctx, is_profile, v);
    Py_DECREF(hook);
    return r;
}

/* Include/internal/pycore_ceval.h (_lsprof) */
int _PyEval_SetProfile(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    (void)tstate;
    return qp_capi_set_trace_hook(1, func, arg);
}

int _PyEval_SetTrace(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    (void)tstate;
    return qp_capi_set_trace_hook(0, func, arg);
}

void PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
    if (qp_capi_set_trace_hook(1, func, arg) < 0)
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetProfile");
}

void PyEval_SetProfileAllThreads(Py_tracefunc func, PyObject *arg)
{
    if (qp_capi_set_trace_hook(3, func, arg) < 0)
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetProfileAllThreads");
}

void PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
    if (qp_capi_set_trace_hook(0, func, arg) < 0)
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetTrace");
}

void PyEval_SetTraceAllThreads(Py_tracefunc func, PyObject *arg)
{
    if (qp_capi_set_trace_hook(2, func, arg) < 0)
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetTraceAllThreads");
}

int PyFrame_GetLasti(PyFrameObject *frame)
{
    return qp_capi_frame_int(frame, "f_lasti");
}

PyCodeObject *PyFrame_GetCode(PyFrameObject *frame)
{
    return (PyCodeObject *)qp_capi_frame_attr(frame, "f_code", 0);
}

PyFrameObject *PyFrame_GetBack(PyFrameObject *frame)
{
    PyObject *exc = PyErr_GetRaisedException();
    PyObject *r = qp_capi_frame_attr(frame, "f_back", 1);
    if (r == NULL)
        PyErr_Clear();
    PyErr_SetRaisedException(exc);
    return (PyFrameObject *)r;
}

PyObject *PyFrame_GetLocals(PyFrameObject *frame)
{
    return qp_capi_frame_attr(frame, "f_locals", 0);
}

PyObject *PyFrame_GetGlobals(PyFrameObject *frame)
{
    return qp_capi_frame_attr(frame, "f_globals", 0);
}

PyObject *PyFrame_GetBuiltins(PyFrameObject *frame)
{
    return qp_capi_frame_attr(frame, "f_builtins", 0);
}

PyObject *PyFrame_GetGenerator(PyFrameObject *frame)
{
    PyObject *r = qp_capi_frame_attr(frame, "f_generator", 1);
    if (r == NULL)
        PyErr_Clear();
    return r;
}

PyObject *PyFrame_GetVar(PyFrameObject *frame, PyObject *name)
{
    PyObject *locals, *value;
    int rc;
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "name must be str, not %s", Py_TYPE(name)->tp_name);
        return NULL;
    }
    locals = PyFrame_GetLocals(frame);
    if (locals == NULL)
        return NULL;
    rc = PyMapping_GetOptionalItem(locals, name, &value);
    Py_DECREF(locals);
    if (rc == 0)
        PyErr_Format(PyExc_NameError, "variable %R does not exist", name);
    return rc > 0 ? value : NULL;
}

PyObject *PyFrame_GetVarString(PyFrameObject *frame, const char *name)
{
    PyObject *name_obj = PyUnicode_FromString(name), *value;
    if (name_obj == NULL)
        return NULL;
    value = PyFrame_GetVar(frame, name_obj);
    Py_DECREF(name_obj);
    return value;
}

PyFrameObject *PyFrame_New(PyThreadState *tstate, PyCodeObject *code, PyObject *globals,
                           PyObject *locals)
{
    JSContext *ctx = qp_capi.ctx;
    struct JSFunctionBytecode *b;
    JSValue gv, lv, func, frame;

    b = qp_capi_code_bc(code);
    if (b == NULL)
        return NULL;
    gv = qp_capi_value(ctx, globals);
    if (JS_IsException(gv))
        return NULL;
    if (locals == NULL && !(code->co_flags & CO_NEWLOCALS))
        lv = gv;
    else
        lv = qp_capi_rt_opt(locals);
    if (JS_IsException(lv))
        return NULL;
    func = qp_function_new(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b), gv);
    if (JS_IsException(func))
        return NULL;
    frame = qp_frame_new(ctx, func, 0, QP_NULL, lv);
    JS_FreeValue(ctx, func);
    return (PyFrameObject *)qp_capi_from_js_result(ctx, frame);
}

int PyFrame_FastToLocalsWithError(PyFrameObject *f)
{
    if (f == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    /* PEP 667: f_locals is a live view; nothing to copy */
    return 0;
}

void PyFrame_FastToLocals(PyFrameObject *frame)
{
}

void PyFrame_LocalsToFast(PyFrameObject *frame, int clear)
{
}
/* ========================================================================= */
/* PEP 669 monitoring (cpython/monitoring.h, CPython Python/instrumentation.c) */
/* ========================================================================= */

/* Over sys.monitoring (python/instrumentation.c qp_mon_capi_*).  A
   PyMonitoringState holds the tools that monitor one event of a code-like
   object; PyMonitoring_EnterScope refreshes the states from the global
   event sets when the global monitoring version moved, and a callback that
   returns DISABLE removes its tool from the state of a location event. */

int PyMonitoring_EnterScope(PyMonitoringState *state_array, uint64_t *version,
                            const uint8_t *event_types, Py_ssize_t length)
{
    JSContext *ctx = qp_capi.ctx;
    uint64_t v = qp_mon_capi_version(ctx);
    Py_ssize_t i;

    if (*version == v)
        return 0;
    for (i = 0; i < length; i++)
        state_array[i].active = (uint8_t)qp_mon_capi_tools(ctx, event_types[i]);
    *version = v;
    return 0;
}

int PyMonitoring_ExitScope(void)
{
    return 0;
}

/* capi_call_instrumentation: the callbacks get (codelike, second,
   extra...), second = offset (the line for LINE). */
static int qp_capi_mon_call(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                            int event, int32_t second, PyObject *const *extra, int nextra)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue argv[4];
    int i;

    if (offset < 0) {
        PyErr_SetString(PyExc_ValueError, "offset must be non-negative");
        return -1;
    }
    argv[0] = qp_capi_value(ctx, codelike);
    if (JS_IsException(argv[0]))
        return -1;
    argv[1] = JS_NewInt32(ctx, second);
    for (i = 0; i < nextra; i++) {
        argv[2 + i] = qp_capi_value(ctx, extra[i]);
        if (JS_IsException(argv[2 + i]))
            return -1;
    }
    return qp_mon_capi_fire(ctx, &state->active, event, (JSValueConst *)argv, 2 + nextra);
}

static int qp_capi_mon_fire(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                            int event, PyObject *arg)
{
    return qp_capi_mon_call(state, codelike, offset, event, offset, &arg, arg ? 1 : 0);
}

/* Exception events: the raised exception is the extra argument and stays
   raised unless a callback fails (exception_event_setup/teardown). */
static int qp_capi_mon_fire_exc(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                int event)
{
    PyObject *exc = PyErr_GetRaisedException();
    int err;

    if (exc == NULL) {
        PyErr_Format(PyExc_ValueError, "Firing event %d with no exception set", event);
        return -1;
    }
    err = qp_capi_mon_fire(state, codelike, offset, event, exc);
    if (err == 0)
        PyErr_SetRaisedException(exc);
    else
        Py_DECREF(exc);
    return err;
}

int _PyMonitoring_FirePyStartEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_PY_START, NULL);
}

int _PyMonitoring_FirePyResumeEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_PY_RESUME, NULL);
}

int _PyMonitoring_FirePyReturnEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                    PyObject *retval)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_PY_RETURN, retval);
}

int _PyMonitoring_FirePyYieldEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                   PyObject *retval)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_PY_YIELD, retval);
}

int _PyMonitoring_FireCallEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                PyObject *callable, PyObject *arg0)
{
    PyObject *extra[2] = { callable, arg0 };
    return qp_capi_mon_call(state, codelike, offset, PY_MONITORING_EVENT_CALL, offset, extra, 2);
}

int _PyMonitoring_FireLineEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                int lineno)
{
    return qp_capi_mon_call(state, codelike, offset, PY_MONITORING_EVENT_LINE, lineno, NULL, 0);
}

int _PyMonitoring_FireJumpEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                PyObject *target_offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_JUMP, target_offset);
}

int _PyMonitoring_FireBranchEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                  PyObject *target_offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_BRANCH_RIGHT,
                            target_offset);
}

int _PyMonitoring_FireBranchRightEvent(PyMonitoringState *state, PyObject *codelike,
                                       int32_t offset, PyObject *target_offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_BRANCH_RIGHT,
                            target_offset);
}

int _PyMonitoring_FireBranchLeftEvent(PyMonitoringState *state, PyObject *codelike,
                                      int32_t offset, PyObject *target_offset)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_BRANCH_LEFT,
                            target_offset);
}

int _PyMonitoring_FireCReturnEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset,
                                   PyObject *retval)
{
    return qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_C_RETURN, retval);
}

int _PyMonitoring_FirePyThrowEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_PY_THROW);
}

int _PyMonitoring_FireRaiseEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_RAISE);
}

int _PyMonitoring_FireCRaiseEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_C_RAISE);
}

int _PyMonitoring_FireReraiseEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_RERAISE);
}

int _PyMonitoring_FireExceptionHandledEvent(PyMonitoringState *state, PyObject *codelike,
                                            int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_EXCEPTION_HANDLED);
}

int _PyMonitoring_FirePyUnwindEvent(PyMonitoringState *state, PyObject *codelike, int32_t offset)
{
    return qp_capi_mon_fire_exc(state, codelike, offset, PY_MONITORING_EVENT_PY_UNWIND);
}

/* The StopIteration(value) is raised for the callbacks only. */
int _PyMonitoring_FireStopIterationEvent(PyMonitoringState *state, PyObject *codelike,
                                         int32_t offset, PyObject *value)
{
    PyObject *exc;
    int err;

    PyErr_SetObject(PyExc_StopIteration, value);
    exc = PyErr_GetRaisedException();
    if (exc == NULL) {
        PyErr_Format(PyExc_ValueError, "Firing event %d with no exception set",
                     PY_MONITORING_EVENT_STOP_ITERATION);
        return -1;
    }
    err = qp_capi_mon_fire(state, codelike, offset, PY_MONITORING_EVENT_STOP_ITERATION, exc);
    Py_DECREF(exc);
    return err;
}


/* ========================================================================= */
/* fileutils.h, pycore_fileutils.h                                            */
/* ========================================================================= */

PyAPI_FUNC(int) _Py_get_inheritable(int fd);
PyAPI_FUNC(int) _Py_set_inheritable(int fd, int inheritable, int *atomic_flag_works);
PyAPI_FUNC(int) _Py_set_inheritable_async_safe(int fd, int inheritable, int *atomic_flag_works);
PyAPI_FUNC(int) _Py_dup(int fd);
PyAPI_FUNC(int) _Py_IsValidFD(int fd);

static int qp_capi_get_inheritable(int fd, int raise)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1) {
        if (raise)
            PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return !(flags & FD_CLOEXEC);
}

static int qp_capi_set_inheritable(int fd, int inheritable, int raise, int *atomic_flag_works)
{
    int flags, new_flags, res;

    if (atomic_flag_works != NULL && !inheritable) {
        if (*atomic_flag_works == -1) {
            int is_inheritable = qp_capi_get_inheritable(fd, raise);
            if (is_inheritable == -1)
                return -1;
            *atomic_flag_works = !is_inheritable;
        }
        if (*atomic_flag_works)
            return 0;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        if (raise)
            PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    if (inheritable)
        new_flags = flags & ~FD_CLOEXEC;
    else
        new_flags = flags | FD_CLOEXEC;
    if (new_flags == flags)
        return 0;
    res = fcntl(fd, F_SETFD, new_flags);
    if (res < 0) {
        if (raise)
            PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return 0;
}

int _Py_get_inheritable(int fd)
{
    return qp_capi_get_inheritable(fd, 1);
}

int _Py_set_inheritable(int fd, int inheritable, int *atomic_flag_works)
{
    return qp_capi_set_inheritable(fd, inheritable, 1, atomic_flag_works);
}

int _Py_set_inheritable_async_safe(int fd, int inheritable, int *atomic_flag_works)
{
    return qp_capi_set_inheritable(fd, inheritable, 0, atomic_flag_works);
}

/* Python/fileutils.c (POSIX): F_GETFD fails with EBADF on a closed fd */
int _Py_IsValidFD(int fd)
{
    if (fd < 0)
        return 0;
    return fcntl(fd, F_GETFD) >= 0;
}

int _Py_dup(int fd)
{
    Py_BEGIN_ALLOW_THREADS
    fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    Py_END_ALLOW_THREADS
    if (fd < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return fd;
}

FILE *Py_fopen(PyObject *path, const char *mode)
{
    FILE *f;
    int async_err = 0, saved_errno;
    PyObject *bytes;
    const char *path_bytes;

    if (PySys_Audit("open", "Osi", path, mode, 0) < 0)
        return NULL;
    if (!PyUnicode_FSConverter(path, &bytes))
        return NULL;
    path_bytes = PyBytes_AS_STRING(bytes);
    do {
        Py_BEGIN_ALLOW_THREADS
        f = fopen(path_bytes, mode);
        Py_END_ALLOW_THREADS
    } while (f == NULL && errno == EINTR && !(async_err = PyErr_CheckSignals()));
    saved_errno = errno;
    Py_DECREF(bytes);
    if (async_err)
        return NULL;
    if (f == NULL) {
        errno = saved_errno;
        PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, path);
        return NULL;
    }
    if (qp_capi_set_inheritable(fileno(f), 0, 1, NULL) < 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

int Py_fclose(FILE *file)
{
    return fclose(file);
}

/* ========================================================================= */
/* PyOS_* utilities                                                           */
/* ========================================================================= */

int PyOS_snprintf(char *str, size_t size, const char *format, ...)
{
    int rc;
    va_list va;
    va_start(va, format);
    rc = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return rc;
}

int PyOS_vsnprintf(char *str, size_t size, const char *format, va_list va)
{
    int len;
    if (size > INT_MAX - 1) {
        len = -666;
        goto done;
    }
    len = vsnprintf(str, size, format, va);
done:
    if (size > 0)
        str[size - 1] = '\0';
    return len;
}

PyObject *PyOS_FSPath(PyObject *path)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue pv, func;
    PyObject *path_repr;

    if (PyUnicode_Check(path) || PyBytes_Check(path))
        return Py_NewRef(path);
    pv = qp_capi_value(ctx, path);
    if (JS_IsException(pv))
        return NULL;
    func = qp_lookup_special(ctx, pv, QP_ATOM(ctx, __fspath__));
    if (JS_IsException(func))
        return NULL;
    if (QP_IS_NULL(func) || JS_IsUndefined(func) || JS_IsNull(func)) {
        if (JS_HasException(ctx))
            return NULL;
        return PyErr_Format(PyExc_TypeError,
                            "expected str, bytes or os.PathLike object, not %T", path);
    }
    path_repr = qp_capi_from_js_result(ctx, qp_call0(ctx, func));
    JS_FreeValue(ctx, func);
    if (path_repr == NULL)
        return NULL;
    if (!(PyUnicode_Check(path_repr) || PyBytes_Check(path_repr))) {
        PyErr_Format(PyExc_TypeError,
                     "expected %T.__fspath__() to return str or bytes, not %T",
                     path, path_repr);
        Py_DECREF(path_repr);
        return NULL;
    }
    return path_repr;
}

int PyOS_InterruptOccurred(void)
{
    if (!qp_capi_rt_is_main_thread())
        return 0;
    if (!qp_signal_tripped[SIGINT])
        return 0;
    qp_signal_tripped[SIGINT] = 0;
    return 1;
}

void PyOS_BeforeFork(void)
{
    if (qp_capi.ctx && qp_capi.tstate_current) {
        qp_os_before_fork(qp_capi.ctx);
        qp_rmutex_lock(qp_capi.ctx, QP_RMUTEX_IMPORT);
    }
}

void PyOS_AfterFork_Parent(void)
{
    if (qp_capi.ctx && qp_capi.tstate_current) {
        qp_rmutex_unlock(qp_capi.ctx, QP_RMUTEX_IMPORT);
        qp_os_after_fork(qp_capi.ctx, 0);
    }
}

void PyOS_AfterFork_Child(void)
{
    _PyParkingLot_AfterFork();
    pthread_mutex_init(&qp_capi_pending_lock, NULL);
    pthread_mutex_init(&qp_capi_audit_lock, NULL);
    pthread_mutex_init(&qp_capi_head_lock, NULL);
    qp_capi_main_thread = PyThread_get_thread_ident();
    if (qp_capi.ctx && qp_capi.tstate_current) {
        /* only this thread exists: it becomes the main thread (the
           QP_TSTATE_FORK_CHILD hook rebinds qp_capi.main_tstate) */
        qp_thread_after_fork_child(qp_capi.ctx);
        qp_signal_after_fork_child();
        qp_rmutex_unlock(qp_capi.ctx, QP_RMUTEX_IMPORT);
    }
    if (qp_capi.main_tstate) {
        PyThreadState *t = &qp_capi.main_tstate->impl.base;
        t->thread_id = qp_capi_main_thread;
        t->native_thread_id = qp_capi_native_thread_id();
    }
    if (qp_capi.ctx && qp_capi.tstate_current)
        qp_os_after_fork(qp_capi.ctx, 1);
}

void PyOS_AfterFork(void)
{
    PyOS_AfterFork_Child();
}

PyOS_sighandler_t PyOS_getsig(int sig)
{
    return qp_signal_getsig(sig);
}

PyOS_sighandler_t PyOS_setsig(int sig, PyOS_sighandler_t handler)
{
    return qp_signal_setsig(sig, handler);
}

PyAPI_FUNC(int) _PyOS_URandom(void *buffer, Py_ssize_t size);
PyAPI_FUNC(int) _PyOS_URandomNonblock(void *buffer, Py_ssize_t size);

static int qp_capi_urandom(void *buffer, Py_ssize_t size)
{
    unsigned char *p = buffer;
    if (size < 0) {
        PyErr_Format(PyExc_ValueError, "negative argument not allowed");
        return -1;
    }
    while (size > 0) {
        size_t chunk = size > 256 ? 256 : (size_t)size;
        if (getentropy(p, chunk) < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return -1;
        }
        p += chunk;
        size -= (Py_ssize_t)chunk;
    }
    return 0;
}

int _PyOS_URandom(void *buffer, Py_ssize_t size)
{
    return qp_capi_urandom(buffer, size);
}

int _PyOS_URandomNonblock(void *buffer, Py_ssize_t size)
{
    return qp_capi_urandom(buffer, size);
}

/* Python/mystrtoul.c digit values (_PyLong_DigitValue): 0-35, 37 = invalid */
static const unsigned char qp_capi_digit_value[256] = {
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  37, 37, 37, 37, 37, 37,
    37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 37, 37, 37, 37,
    37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
};

/* Static overflow check values for bases 2 through 36.
 * qp_capi_strtoul_smallmax[base] is the largest unsigned long i such that
 * i * base doesn't overflow unsigned long.
 */
static const unsigned long qp_capi_strtoul_smallmax[] = {
    0, /* bases 0 and 1 are invalid */
    0,
    ULONG_MAX / 2,
    ULONG_MAX / 3,
    ULONG_MAX / 4,
    ULONG_MAX / 5,
    ULONG_MAX / 6,
    ULONG_MAX / 7,
    ULONG_MAX / 8,
    ULONG_MAX / 9,
    ULONG_MAX / 10,
    ULONG_MAX / 11,
    ULONG_MAX / 12,
    ULONG_MAX / 13,
    ULONG_MAX / 14,
    ULONG_MAX / 15,
    ULONG_MAX / 16,
    ULONG_MAX / 17,
    ULONG_MAX / 18,
    ULONG_MAX / 19,
    ULONG_MAX / 20,
    ULONG_MAX / 21,
    ULONG_MAX / 22,
    ULONG_MAX / 23,
    ULONG_MAX / 24,
    ULONG_MAX / 25,
    ULONG_MAX / 26,
    ULONG_MAX / 27,
    ULONG_MAX / 28,
    ULONG_MAX / 29,
    ULONG_MAX / 30,
    ULONG_MAX / 31,
    ULONG_MAX / 32,
    ULONG_MAX / 33,
    ULONG_MAX / 34,
    ULONG_MAX / 35,
    ULONG_MAX / 36,
};

/* maximum digits that can't ever overflow for bases 2 through 36,
 * calculated by [int(math.floor(math.log(2**32, i))) for i in range(2, 37)].
 * Note that this is pessimistic if sizeof(long) > 4.
 */
/* [int(math.floor(math.log(2**64, i))) for i in range(2, 37)] */
static const int qp_capi_strtoul_digitlimit[] = {
         0,   0, 64, 40, 32, 27, 24, 22, 21, 20,  /*  0 -  9 */
    19,  18, 17, 17, 16, 16, 16, 15, 15, 15,  /* 10 - 19 */
    14,  14, 14, 14, 13, 13, 13, 13, 13, 13,  /* 20 - 29 */
    13,  12, 12, 12, 12, 12, 12};             /* 30 - 36 */

/*
**      strtoul
**              This is a general purpose routine for converting
**              an ascii string to an integer in an arbitrary base.
**              Leading white space is ignored.  If 'base' is zero
**              it looks for a leading 0b, 0o or 0x to tell which
**              base.  If these are absent it defaults to 10.
**              Base must be 0 or between 2 and 36 (inclusive).
**              If 'ptr' is non-NULL it will contain a pointer to
**              the end of the scan.
**              Errors due to bad pointers will probably result in
**              exceptions - we don't check for them.
*/
unsigned long
PyOS_strtoul(const char *str, char **ptr, int base)
{
    unsigned long result = 0; /* return value of the function */
    int c;             /* current input character */
    int ovlimit;       /* required digits to overflow */

    /* skip leading white space */
    while (*str && Py_ISSPACE(*str))
        ++str;

    /* check for leading 0b, 0o or 0x for auto-base or base 16 */
    switch (base) {
    case 0:             /* look for leading 0b, 0o or 0x */
        if (*str == '0') {
            ++str;
            if (*str == 'x' || *str == 'X') {
                /* there must be at least one digit after 0x */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 16) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
                base = 16;
            } else if (*str == 'o' || *str == 'O') {
                /* there must be at least one digit after 0o */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 8) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
                base = 8;
            } else if (*str == 'b' || *str == 'B') {
                /* there must be at least one digit after 0b */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 2) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
                base = 2;
            } else {
                /* skip all zeroes... */
                while (*str == '0')
                    ++str;
                while (Py_ISSPACE(*str))
                    ++str;
                if (ptr)
                    *ptr = (char *)str;
                return 0;
            }
        }
        else
            base = 10;
        break;

    /* even with explicit base, skip leading 0? prefix */
    case 16:
        if (*str == '0') {
            ++str;
            if (*str == 'x' || *str == 'X') {
                /* there must be at least one digit after 0x */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 16) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
            }
        }
        break;
    case 8:
        if (*str == '0') {
            ++str;
            if (*str == 'o' || *str == 'O') {
                /* there must be at least one digit after 0o */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 8) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
            }
        }
        break;
    case 2:
        if(*str == '0') {
            ++str;
            if (*str == 'b' || *str == 'B') {
                /* there must be at least one digit after 0b */
                if (qp_capi_digit_value[Py_CHARMASK(str[1])] >= 2) {
                    if (ptr)
                        *ptr = (char *)str;
                    return 0;
                }
                ++str;
            }
        }
        break;
    }

    /* catch silly bases */
    if (base < 2 || base > 36) {
        if (ptr)
            *ptr = (char *)str;
        return 0;
    }

    /* skip leading zeroes */
    while (*str == '0')
        ++str;

    /* base is guaranteed to be in [2, 36] at this point */
    ovlimit = qp_capi_strtoul_digitlimit[base];

    /* do the conversion until non-digit character encountered */
    while ((c = qp_capi_digit_value[Py_CHARMASK(*str)]) < base) {
        if (ovlimit > 0) /* no overflow check required */
            result = result * base + c;
        else { /* requires overflow check */
            unsigned long temp_result;

            if (ovlimit < 0) /* guaranteed overflow */
                goto overflowed;

            /* there could be an overflow */
            /* check overflow just from shifting */
            if (result > qp_capi_strtoul_smallmax[base])
                goto overflowed;

            result *= base;

            /* check overflow from the digit's value */
            temp_result = result + c;
            if (temp_result < result)
                goto overflowed;

            result = temp_result;
        }

        ++str;
        --ovlimit;
    }

    /* set pointer to point to the last character scanned */
    if (ptr)
        *ptr = (char *)str;

    return result;

overflowed:
    if (ptr) {
        /* spool through remaining digit characters */
        while (qp_capi_digit_value[Py_CHARMASK(*str)] < base)
            ++str;
        *ptr = (char *)str;
    }
    errno = ERANGE;
    return (unsigned long)-1;
}

/* Checking for overflow in PyOS_strtol is a PITA; see comments
 * about QP_CAPI_RT_ABS_LONG_MIN in longobject.c.
 */
#define QP_CAPI_RT_ABS_LONG_MIN         (0-(unsigned long)LONG_MIN)

long
PyOS_strtol(const char *str, char **ptr, int base)
{
    long result;
    unsigned long uresult;
    char sign;

    while (*str && Py_ISSPACE(*str))
        str++;

    sign = *str;
    if (sign == '+' || sign == '-')
        str++;

    uresult = PyOS_strtoul(str, ptr, base);

    if (uresult <= (unsigned long)LONG_MAX) {
        result = (long)uresult;
        if (sign == '-')
            result = -result;
    }
    else if (sign == '-' && uresult == QP_CAPI_RT_ABS_LONG_MIN) {
        result = LONG_MIN;
    }
    else {
        errno = ERANGE;
        result = LONG_MAX;
    }
    return result;
}

#undef QP_CAPI_RT_ABS_LONG_MIN

int
PyOS_mystrnicmp(const char *s1, const char *s2, Py_ssize_t size)
{
    const unsigned char *p1, *p2;
    if (size == 0)
        return 0;
    p1 = (const unsigned char *)s1;
    p2 = (const unsigned char *)s2;
    for (; (--size > 0) && *p1 && *p2 && (Py_TOLOWER(*p1) == Py_TOLOWER(*p2));
         p1++, p2++) {
        ;
    }
    return Py_TOLOWER(*p1) - Py_TOLOWER(*p2);
}

int
PyOS_mystricmp(const char *s1, const char *s2)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (; *p1 && *p2 && (Py_TOLOWER(*p1) == Py_TOLOWER(*p2)); p1++, p2++) {
        ;
    }
    return (Py_TOLOWER(*p1) - Py_TOLOWER(*p2));
}


/* ========================================================================= */
/* pytime (port of CPython 3.15.2 Python/pytime.c, POSIX clocks)              */
/* ========================================================================= */

/* pycore_time.h */
typedef enum {
    _PyTime_ROUND_FLOOR = 0,
    _PyTime_ROUND_CEILING = 1,
    _PyTime_ROUND_HALF_EVEN = 2,
    _PyTime_ROUND_UP = 3,
    _PyTime_ROUND_TIMEOUT = _PyTime_ROUND_UP
} _PyTime_round_t;

typedef struct {
    const char *implementation;
    int monotonic;
    int adjustable;
    double resolution;
} _Py_clock_info_t;

PyAPI_FUNC(PyObject *) _PyLong_FromTime_t(time_t sec);
PyAPI_FUNC(time_t) _PyLong_AsTime_t(PyObject *obj);
PyAPI_FUNC(int) _PyTime_ObjectToTime_t(PyObject *obj, time_t *sec, _PyTime_round_t);
PyAPI_FUNC(int) _PyTime_ObjectToTimeval(PyObject *obj, time_t *sec, long *usec, _PyTime_round_t);
PyAPI_FUNC(int) _PyTime_ObjectToTimespec(PyObject *obj, time_t *sec, long *nsec, _PyTime_round_t);
PyAPI_FUNC(PyTime_t) _PyTime_FromSeconds(int seconds);
PyAPI_FUNC(int) _PyTime_FromSecondsDouble(double seconds, _PyTime_round_t round, PyTime_t *result);
PyAPI_FUNC(int) _PyTime_FromSecondsObject(PyTime_t *t, PyObject *obj, _PyTime_round_t round);
PyAPI_FUNC(int) _PyTime_FromMillisecondsObject(PyTime_t *t, PyObject *obj, _PyTime_round_t round);
PyAPI_FUNC(PyTime_t) _PyTime_AsMilliseconds(PyTime_t t, _PyTime_round_t round);
PyAPI_FUNC(PyTime_t) _PyTime_AsMicroseconds(PyTime_t t, _PyTime_round_t round);
PyAPI_FUNC(int) _PyTime_FromTimeval(PyTime_t *tp, struct timeval *tv);
PyAPI_FUNC(int) _PyTime_AsTimeval(PyTime_t t, struct timeval *tv, _PyTime_round_t round);
PyAPI_FUNC(void) _PyTime_AsTimeval_clamp(PyTime_t t, struct timeval *tv, _PyTime_round_t round);
PyAPI_FUNC(int) _PyTime_AsTimevalTime_t(PyTime_t t, time_t *secs, int *us, _PyTime_round_t round);
PyAPI_FUNC(int) _PyTime_FromTimespec(PyTime_t *tp, const struct timespec *ts);
PyAPI_FUNC(int) _PyTime_AsTimespec(PyTime_t t, struct timespec *ts);
PyAPI_FUNC(void) _PyTime_AsTimespec_clamp(PyTime_t t, struct timespec *ts);
PyAPI_FUNC(int) _PyTime_TimeWithInfo(PyTime_t *t, _Py_clock_info_t *info);
PyAPI_FUNC(int) _PyTime_MonotonicWithInfo(PyTime_t *t, _Py_clock_info_t *info);
PyAPI_FUNC(int) _PyTime_PerfCounterWithInfo(PyTime_t *t, _Py_clock_info_t *info);
PyAPI_FUNC(int) _PyTime_localtime(time_t t, struct tm *tm);
PyAPI_FUNC(int) _PyTime_gmtime(time_t t, struct tm *tm);

#define QP_CAPI_SEC_TO_MS 1000
#define QP_CAPI_MS_TO_US 1000
#define QP_CAPI_SEC_TO_US (QP_CAPI_SEC_TO_MS * QP_CAPI_MS_TO_US)
#define QP_CAPI_US_TO_NS 1000
#define QP_CAPI_MS_TO_NS (QP_CAPI_MS_TO_US * QP_CAPI_US_TO_NS)
#define QP_CAPI_SEC_TO_NS (QP_CAPI_SEC_TO_MS * QP_CAPI_MS_TO_NS)
#define QP_CAPI_NS_TO_MS (1000 * 1000)
#define QP_CAPI_NS_TO_US (1000)
#define QP_CAPI_NS_TO_100NS (100)
#define QP_CAPI_TIME_T_MAX LLONG_MAX
#define QP_CAPI_TIME_T_MIN LLONG_MIN

static void
qp_capi_pytime_time_t_overflow(void)
{
    PyErr_SetString(PyExc_OverflowError,
                    "timestamp out of range for platform time_t");
}


static void
qp_capi_pytime_overflow(void)
{
    PyErr_SetString(PyExc_OverflowError,
                    "timestamp out of range for C PyTime_t");
}


// Compute t1 + t2. Clamp to [PyTime_MIN; PyTime_MAX] on overflow.
static inline int
qp_capi_pytime_add(PyTime_t *t1, PyTime_t t2)
{
    if (t2 > 0 && *t1 > PyTime_MAX - t2) {
        *t1 = PyTime_MAX;
        return -1;
    }
    else if (t2 < 0 && *t1 < PyTime_MIN - t2) {
        *t1 = PyTime_MIN;
        return -1;
    }
    else {
        *t1 += t2;
        return 0;
    }
}


PyTime_t
_PyTime_Add(PyTime_t t1, PyTime_t t2)
{
    (void)qp_capi_pytime_add(&t1, t2);
    return t1;
}


static inline int
qp_capi_pytime_mul_check_overflow(PyTime_t a, PyTime_t b)
{
    if (b != 0) {
        assert(b > 0);
        return ((a < PyTime_MIN / b) || (PyTime_MAX / b < a));
    }
    else {
        return 0;
    }
}


// Compute t * k. Clamp to [PyTime_MIN; PyTime_MAX] on overflow.
static inline int
qp_capi_pytime_mul(PyTime_t *t, PyTime_t k)
{
    assert(k >= 0);
    if (qp_capi_pytime_mul_check_overflow(*t, k)) {
        *t = (*t >= 0) ? PyTime_MAX : PyTime_MIN;
        return -1;
    }
    else {
        *t *= k;
        return 0;
    }
}


// Compute t * k. Clamp to [PyTime_MIN; PyTime_MAX] on overflow.
static inline PyTime_t
qp_capi_pytime_mul2(PyTime_t t, PyTime_t k)
{
    (void)qp_capi_pytime_mul(&t, k);
    return t;
}




time_t
_PyLong_AsTime_t(PyObject *obj)
{
    long long val = PyLong_AsLongLong(obj);
    if (val == -1 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            qp_capi_pytime_time_t_overflow();
        }
        return -1;
    }
    return (time_t)val;
}


PyObject *
_PyLong_FromTime_t(time_t t)
{
    return PyLong_FromLongLong((long long)t);
}


// Convert PyTime_t to time_t.
// Return 0 on success. Return -1 and clamp the value on overflow.
static int
qp_capi_pytime_as_time_t(PyTime_t t, time_t *t2)
{
    *t2 = (time_t)t;
    return 0;
}




/* Round to nearest with ties going to nearest even integer
   (_PyTime_ROUND_HALF_EVEN) */
static double
qp_capi_pytime_round_half_even(double x)
{
    double rounded = round(x);
    if (fabs(x-rounded) == 0.5) {
        /* halfway case: round to even */
        rounded = 2.0 * round(x / 2.0);
    }
    return rounded;
}


static double
qp_capi_pytime_round(double x, _PyTime_round_t round)
{
    /* volatile avoids optimization changing how numbers are rounded */
    volatile double d;

    d = x;
    if (round == _PyTime_ROUND_HALF_EVEN) {
        d = qp_capi_pytime_round_half_even(d);
    }
    else if (round == _PyTime_ROUND_CEILING) {
        d = ceil(d);
    }
    else if (round == _PyTime_ROUND_FLOOR) {
        d = floor(d);
    }
    else {
        assert(round == _PyTime_ROUND_UP);
        d = (d >= 0.0) ? ceil(d) : floor(d);
    }
    return d;
}


static int
qp_capi_pytime_double_to_denominator(double d, time_t *sec, long *numerator,
                             long idenominator, _PyTime_round_t round)
{
    double denominator = (double)idenominator;
    double intpart;
    /* volatile avoids optimization changing how numbers are rounded */
    volatile double floatpart;

    floatpart = modf(d, &intpart);

    floatpart *= denominator;
    floatpart = qp_capi_pytime_round(floatpart, round);
    if (floatpart >= denominator) {
        floatpart -= denominator;
        intpart += 1.0;
    }
    else if (floatpart < 0) {
        floatpart += denominator;
        intpart -= 1.0;
    }
    assert(0.0 <= floatpart && floatpart < denominator);

    /*
       Conversion of an out-of-range value to time_t gives undefined behaviour
       (C99 §6.3.1.4p1), so we must guard against it. However, checking that
       `intpart` is in range is delicate: the obvious expression `intpart <=
       QP_CAPI_TIME_T_MAX` will first convert the value `QP_CAPI_TIME_T_MAX` to a double,
       potentially changing its value and leading to us failing to catch some
       UB-inducing values. The code below works correctly under the mild
       assumption that time_t is a two's complement integer type with no trap
       representation, and that `QP_CAPI_TIME_T_MIN` is within the representable
       range of a C double.

       Note: we want the `if` condition below to be true for NaNs; therefore,
       resist any temptation to simplify by applying De Morgan's laws.
    */
    if (!((double)QP_CAPI_TIME_T_MIN <= intpart && intpart < -(double)QP_CAPI_TIME_T_MIN)) {
        qp_capi_pytime_time_t_overflow();
        return -1;
    }
    *sec = (time_t)intpart;
    *numerator = (long)floatpart;
    assert(0 <= *numerator && *numerator < idenominator);
    return 0;
}


static int
qp_capi_pytime_object_to_denominator(PyObject *obj, time_t *sec, long *numerator,
                             long denominator, _PyTime_round_t round)
{
    assert(denominator >= 1);

    if (PyIndex_Check(obj)) {
        *sec = _PyLong_AsTime_t(obj);
        *numerator = 0;
        if (*sec == (time_t)-1 && PyErr_Occurred()) {
            return -1;
        }
        return 0;
    }
    else {
        double d = PyFloat_AsDouble(obj);
        if (d == -1 && PyErr_Occurred()) {
            *numerator = 0;
            return -1;
        }
        if (isnan(d)) {
            *numerator = 0;
            PyErr_SetString(PyExc_ValueError, "Invalid value NaN (not a number)");
            return -1;
        }
        return qp_capi_pytime_double_to_denominator(d, sec, numerator,
                                            denominator, round);
    }
}


int
_PyTime_ObjectToTime_t(PyObject *obj, time_t *sec, _PyTime_round_t round)
{
    if (PyIndex_Check(obj)) {
        *sec = _PyLong_AsTime_t(obj);
        if (*sec == (time_t)-1 && PyErr_Occurred()) {
            return -1;
        }
        return 0;
    }
    else {
        double intpart;
        /* volatile avoids optimization changing how numbers are rounded */
        volatile double d;

        d = PyFloat_AsDouble(obj);
        if (d == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (isnan(d)) {
            PyErr_SetString(PyExc_ValueError, "Invalid value NaN (not a number)");
            return -1;
        }

        d = qp_capi_pytime_round(d, round);
        (void)modf(d, &intpart);

        /* See comments in qp_capi_pytime_double_to_denominator */
        if (!((double)QP_CAPI_TIME_T_MIN <= intpart && intpart < -(double)QP_CAPI_TIME_T_MIN)) {
            qp_capi_pytime_time_t_overflow();
            return -1;
        }
        *sec = (time_t)intpart;
        return 0;
    }
}


int
_PyTime_ObjectToTimespec(PyObject *obj, time_t *sec, long *nsec,
                         _PyTime_round_t round)
{
    return qp_capi_pytime_object_to_denominator(obj, sec, nsec, QP_CAPI_SEC_TO_NS, round);
}


int
_PyTime_ObjectToTimeval(PyObject *obj, time_t *sec, long *usec,
                        _PyTime_round_t round)
{
    return qp_capi_pytime_object_to_denominator(obj, sec, usec, QP_CAPI_SEC_TO_US, round);
}


PyTime_t
_PyTime_FromSeconds(int seconds)
{
    /* ensure that integer overflow cannot happen, int type should have 32
       bits, whereas PyTime_t type has at least 64 bits (QP_CAPI_SEC_TO_NS takes 30
       bits). */
    static_assert(INT_MAX <= PyTime_MAX / QP_CAPI_SEC_TO_NS, "PyTime_t overflow");
    static_assert(INT_MIN >= PyTime_MIN / QP_CAPI_SEC_TO_NS, "PyTime_t underflow");

    PyTime_t t = (PyTime_t)seconds;
    assert((t >= 0 && t <= PyTime_MAX / QP_CAPI_SEC_TO_NS)
           || (t < 0 && t >= PyTime_MIN / QP_CAPI_SEC_TO_NS));
    t *= QP_CAPI_SEC_TO_NS;
    return t;
}


PyTime_t
_PyTime_FromMicrosecondsClamp(PyTime_t us)
{
    PyTime_t ns = qp_capi_pytime_mul2(us, QP_CAPI_US_TO_NS);
    return ns;
}


static int
qp_capi_pytime_fromtimespec(PyTime_t *tp, const struct timespec *ts, int raise_exc)
{
    PyTime_t t, tv_nsec;

    static_assert(sizeof(ts->tv_sec) <= sizeof(PyTime_t),
                  "timespec.tv_sec is larger than PyTime_t");
    t = (PyTime_t)ts->tv_sec;

    int res1 = qp_capi_pytime_mul(&t, QP_CAPI_SEC_TO_NS);

    tv_nsec = ts->tv_nsec;
    int res2 = qp_capi_pytime_add(&t, tv_nsec);

    *tp = t;

    if (raise_exc && (res1 < 0 || res2 < 0)) {
        qp_capi_pytime_overflow();
        return -1;
    }
    return 0;
}

int
_PyTime_FromTimespec(PyTime_t *tp, const struct timespec *ts)
{
    return qp_capi_pytime_fromtimespec(tp, ts, 1);
}


static int
qp_capi_pytime_fromtimeval(PyTime_t *tp, struct timeval *tv, int raise_exc)
{
    static_assert(sizeof(tv->tv_sec) <= sizeof(PyTime_t),
                  "timeval.tv_sec is larger than PyTime_t");
    PyTime_t t = (PyTime_t)tv->tv_sec;

    int res1 = qp_capi_pytime_mul(&t, QP_CAPI_SEC_TO_NS);

    PyTime_t usec = (PyTime_t)tv->tv_usec * QP_CAPI_US_TO_NS;
    int res2 = qp_capi_pytime_add(&t, usec);

    *tp = t;

    if (raise_exc && (res1 < 0 || res2 < 0)) {
        qp_capi_pytime_overflow();
        return -1;
    }
    return 0;
}


int
_PyTime_FromTimeval(PyTime_t *tp, struct timeval *tv)
{
    return qp_capi_pytime_fromtimeval(tp, tv, 1);
}


static int
qp_capi_pytime_from_double(PyTime_t *tp, double value, _PyTime_round_t round,
                   long unit_to_ns)
{
    /* volatile avoids optimization changing how numbers are rounded */
    volatile double d;

    /* convert to a number of nanoseconds */
    d = value;
    d *= (double)unit_to_ns;
    d = qp_capi_pytime_round(d, round);

    /* See comments in qp_capi_pytime_double_to_denominator */
    if (!((double)PyTime_MIN <= d && d < -(double)PyTime_MIN)) {
        qp_capi_pytime_overflow();
        *tp = 0;
        return -1;
    }
    PyTime_t ns = (PyTime_t)d;

    *tp = ns;
    return 0;
}


static int
qp_capi_pytime_from_object(PyTime_t *tp, PyObject *obj, _PyTime_round_t round,
                   long unit_to_ns)
{
    if (PyIndex_Check(obj)) {
        long long sec = PyLong_AsLongLong(obj);
        if (sec == -1 && PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
                qp_capi_pytime_overflow();
            }
            return -1;
        }

        static_assert(sizeof(long long) <= sizeof(PyTime_t),
                    "PyTime_t is smaller than long long");
        PyTime_t ns = (PyTime_t)sec;
        if (qp_capi_pytime_mul(&ns, unit_to_ns) < 0) {
            qp_capi_pytime_overflow();
            return -1;
        }

        *tp = ns;
        return 0;
    }
    else {
        double d;
        d = PyFloat_AsDouble(obj);
        if (d == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (isnan(d)) {
            PyErr_SetString(PyExc_ValueError, "Invalid value NaN (not a number)");
            return -1;
        }
        return qp_capi_pytime_from_double(tp, d, round, unit_to_ns);
    }
}


int
_PyTime_FromSecondsObject(PyTime_t *tp, PyObject *obj, _PyTime_round_t round)
{
    return qp_capi_pytime_from_object(tp, obj, round, QP_CAPI_SEC_TO_NS);
}


int
_PyTime_FromMillisecondsObject(PyTime_t *tp, PyObject *obj, _PyTime_round_t round)
{
    return qp_capi_pytime_from_object(tp, obj, round, QP_CAPI_MS_TO_NS);
}


double
PyTime_AsSecondsDouble(PyTime_t ns)
{
    /* volatile avoids optimization changing how numbers are rounded */
    volatile double d;

    if (ns % QP_CAPI_SEC_TO_NS == 0) {
        /* Divide using integers to avoid rounding issues on the integer part.
           1e-9 cannot be stored exactly in IEEE 64-bit. */
        PyTime_t secs = ns / QP_CAPI_SEC_TO_NS;
        d = (double)secs;
    }
    else {
        d = (double)ns;
        d /= 1e9;
    }
    return d;
}


int
_PyTime_FromSecondsDouble(double seconds, _PyTime_round_t round, PyTime_t *result)
{
    return qp_capi_pytime_from_double(result, seconds, round, QP_CAPI_SEC_TO_NS);
}


static PyTime_t
qp_capi_pytime_divide_round_up(const PyTime_t t, const PyTime_t k)
{
    assert(k > 1);
    if (t >= 0) {
        // Don't use (t + k - 1) / k to avoid integer overflow
        // if t is equal to PyTime_MAX
        PyTime_t q = t / k;
        if (t % k) {
            q += 1;
        }
        return q;
    }
    else {
        // Don't use (t - (k - 1)) / k to avoid integer overflow
        // if t is equals to PyTime_MIN.
        PyTime_t q = t / k;
        if (t % k) {
            q -= 1;
        }
        return q;
    }
}


static PyTime_t
qp_capi_pytime_divide(const PyTime_t t, const PyTime_t k,
              const _PyTime_round_t round)
{
    assert(k > 1);
    if (round == _PyTime_ROUND_HALF_EVEN) {
        PyTime_t x = t / k;
        PyTime_t r = t % k;
        PyTime_t abs_r = Py_ABS(r);
        if (abs_r > k / 2 || (abs_r == k / 2 && (Py_ABS(x) & 1))) {
            if (t >= 0) {
                x++;
            }
            else {
                x--;
            }
        }
        return x;
    }
    else if (round == _PyTime_ROUND_CEILING) {
        if (t >= 0) {
            return qp_capi_pytime_divide_round_up(t, k);
        }
        else {
            return t / k;
        }
    }
    else if (round == _PyTime_ROUND_FLOOR){
        if (t >= 0) {
            return t / k;
        }
        else {
            return qp_capi_pytime_divide_round_up(t, k);
        }
    }
    else {
        assert(round == _PyTime_ROUND_UP);
        return qp_capi_pytime_divide_round_up(t, k);
    }
}


// Compute (t / k, t % k) in (pq, pr).
// Make sure that 0 <= pr < k.
// Return 0 on success.
// Return -1 on underflow and store (PyTime_MIN, 0) in (pq, pr).
static int
qp_capi_pytime_divmod(const PyTime_t t, const PyTime_t k,
              PyTime_t *pq, PyTime_t *pr)
{
    assert(k > 1);
    PyTime_t q = t / k;
    PyTime_t r = t % k;
    if (r < 0) {
        if (q == PyTime_MIN) {
            *pq = PyTime_MIN;
            *pr = 0;
            return -1;
        }
        r += k;
        q -= 1;
    }
    assert(0 <= r && r < k);

    *pq = q;
    *pr = r;
    return 0;
}




PyTime_t
_PyTime_AsMicroseconds(PyTime_t ns, _PyTime_round_t round)
{
    return qp_capi_pytime_divide(ns, QP_CAPI_NS_TO_US, round);
}


PyTime_t
_PyTime_AsMilliseconds(PyTime_t ns, _PyTime_round_t round)
{
    return qp_capi_pytime_divide(ns, QP_CAPI_NS_TO_MS, round);
}


static int
qp_capi_pytime_as_timeval(PyTime_t ns, PyTime_t *ptv_sec, int *ptv_usec,
                  _PyTime_round_t round)
{
    PyTime_t us = qp_capi_pytime_divide(ns, QP_CAPI_US_TO_NS, round);

    PyTime_t tv_sec, tv_usec;
    int res = qp_capi_pytime_divmod(us, QP_CAPI_SEC_TO_US, &tv_sec, &tv_usec);
    *ptv_sec = tv_sec;
    *ptv_usec = (int)tv_usec;
    return res;
}


static int
qp_capi_pytime_as_timeval_struct(PyTime_t t, struct timeval *tv,
                         _PyTime_round_t round, int raise_exc)
{
    PyTime_t tv_sec;
    int tv_usec;
    int res = qp_capi_pytime_as_timeval(t, &tv_sec, &tv_usec, round);
    int res2;
    res2 = qp_capi_pytime_as_time_t(tv_sec, &tv->tv_sec);
    if (res2 < 0) {
        tv_usec = 0;
    }
    tv->tv_usec = tv_usec;

    if (raise_exc && (res < 0 || res2 < 0)) {
        qp_capi_pytime_time_t_overflow();
        return -1;
    }
    return 0;
}


int
_PyTime_AsTimeval(PyTime_t t, struct timeval *tv, _PyTime_round_t round)
{
    return qp_capi_pytime_as_timeval_struct(t, tv, round, 1);
}


void
_PyTime_AsTimeval_clamp(PyTime_t t, struct timeval *tv, _PyTime_round_t round)
{
    (void)qp_capi_pytime_as_timeval_struct(t, tv, round, 0);
}


int
_PyTime_AsTimevalTime_t(PyTime_t t, time_t *p_secs, int *us,
                        _PyTime_round_t round)
{
    PyTime_t secs;
    if (qp_capi_pytime_as_timeval(t, &secs, us, round) < 0) {
        qp_capi_pytime_time_t_overflow();
        return -1;
    }

    if (qp_capi_pytime_as_time_t(secs, p_secs) < 0) {
        qp_capi_pytime_time_t_overflow();
        return -1;
    }
    return 0;
}


static int
qp_capi_pytime_as_timespec(PyTime_t ns, struct timespec *ts, int raise_exc)
{
    PyTime_t tv_sec, tv_nsec;
    int res = qp_capi_pytime_divmod(ns, QP_CAPI_SEC_TO_NS, &tv_sec, &tv_nsec);

    int res2 = qp_capi_pytime_as_time_t(tv_sec, &ts->tv_sec);
    if (res2 < 0) {
        tv_nsec = 0;
    }
    ts->tv_nsec = tv_nsec;

    if (raise_exc && (res < 0 || res2 < 0)) {
        qp_capi_pytime_time_t_overflow();
        return -1;
    }
    return 0;
}

void
_PyTime_AsTimespec_clamp(PyTime_t t, struct timespec *ts)
{
    (void)qp_capi_pytime_as_timespec(t, ts, 0);
}

int
_PyTime_AsTimespec(PyTime_t t, struct timespec *ts)
{
    return qp_capi_pytime_as_timespec(t, ts, 1);
}


// N.B. If raise_exc=0, this may be called without a thread state.
static int
qp_capi_pytime_get_system_clock(PyTime_t *tp, _Py_clock_info_t *info, int raise_exc)
{
    assert(info == NULL || raise_exc);
    if (raise_exc) {
        // raise_exc requires to hold a thread state
    }

    int err;
    struct timespec ts;




    err = clock_gettime(CLOCK_REALTIME, &ts);
    if (err) {
        if (raise_exc) {
            PyErr_SetFromErrno(PyExc_OSError);
        }
        return -1;
    }
    if (qp_capi_pytime_fromtimespec(tp, &ts, raise_exc) < 0) {
        return -1;
    }

    if (info) {
        struct timespec res;
        info->implementation = "clock_gettime(CLOCK_REALTIME)";
        info->monotonic = 0;
        info->adjustable = 1;
        if (clock_getres(CLOCK_REALTIME, &res) == 0) {
            info->resolution = (double)res.tv_sec + (double)res.tv_nsec * 1e-9;
        }
        else {
            info->resolution = 1e-9;
        }
    }



    return 0;
}


int
PyTime_Time(PyTime_t *result)
{
    if (qp_capi_pytime_get_system_clock(result, NULL, 1) < 0) {
        *result = 0;
        return -1;
    }
    return 0;
}


int
PyTime_TimeRaw(PyTime_t *result)
{
    if (qp_capi_pytime_get_system_clock(result, NULL, 0) < 0) {
        *result = 0;
        return -1;
    }
    return 0;
}


int
_PyTime_TimeWithInfo(PyTime_t *t, _Py_clock_info_t *info)
{
    return qp_capi_pytime_get_system_clock(t, info, 1);
}






// N.B. If raise_exc=0, this may be called without a thread state.
static int
qp_capi_pytime_get_monotonic_clock(PyTime_t *tp, _Py_clock_info_t *info, int raise_exc)
{
    /* the clock of time.monotonic() (python/mod_time.c: mach_absolute_time()
       on macOS, clock_gettime(CLOCK_MONOTONIC/CLOCK_HIGHRES) elsewhere) */
    QPTimeClockInfo qinfo;
    int64_t t;

    assert(info == NULL || raise_exc);
    if (qp_time_monotonic_clock(raise_exc ? qp_capi.ctx : NULL, &t,
                                info ? &qinfo : NULL) < 0)
        return -1;
    *tp = (PyTime_t)t;
    if (info) {
        info->implementation = qinfo.implementation;
        info->monotonic = qinfo.monotonic;
        info->adjustable = qinfo.adjustable;
        info->resolution = qinfo.resolution;
    }
    return 0;
}


int
PyTime_Monotonic(PyTime_t *result)
{
    if (qp_capi_pytime_get_monotonic_clock(result, NULL, 1) < 0) {
        *result = 0;
        return -1;
    }
    return 0;
}


int
PyTime_MonotonicRaw(PyTime_t *result)
{
    if (qp_capi_pytime_get_monotonic_clock(result, NULL, 0) < 0) {
        *result = 0;
        return -1;
    }
    return 0;
}


int
_PyTime_MonotonicWithInfo(PyTime_t *tp, _Py_clock_info_t *info)
{
    return qp_capi_pytime_get_monotonic_clock(tp, info, 1);
}


int
_PyTime_PerfCounterWithInfo(PyTime_t *t, _Py_clock_info_t *info)
{
    return _PyTime_MonotonicWithInfo(t, info);
}


int
PyTime_PerfCounter(PyTime_t *result)
{
    return PyTime_Monotonic(result);
}


int
PyTime_PerfCounterRaw(PyTime_t *result)
{
    return PyTime_MonotonicRaw(result);
}


int
_PyTime_localtime(time_t t, struct tm *tm)
{


    errno = 0;
    if (localtime_r(&t, tm) == NULL) {
        if (errno == 0) {
            errno = EINVAL;
        }
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return 0;
}


int
_PyTime_gmtime(time_t t, struct tm *tm)
{
    if (gmtime_r(&t, tm) == NULL) {
        if (errno == 0) {
            errno = EINVAL;
        }
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return 0;
}


PyTime_t
_PyDeadline_Init(PyTime_t timeout)
{
    PyTime_t now;
    // silently ignore error: cannot report error to the caller
    (void)PyTime_MonotonicRaw(&now);
    return _PyTime_Add(now, timeout);
}


PyTime_t
_PyDeadline_Get(PyTime_t deadline)
{
    PyTime_t now;
    // silently ignore error: cannot report error to the caller
    (void)PyTime_MonotonicRaw(&now);
    return deadline - now;
}


#undef QP_CAPI_SEC_TO_MS
#undef QP_CAPI_MS_TO_US
#undef QP_CAPI_SEC_TO_US
#undef QP_CAPI_US_TO_NS
#undef QP_CAPI_MS_TO_NS
#undef QP_CAPI_SEC_TO_NS
#undef QP_CAPI_NS_TO_MS
#undef QP_CAPI_NS_TO_US
#undef QP_CAPI_NS_TO_100NS
#undef QP_CAPI_TIME_T_MAX
#undef QP_CAPI_TIME_T_MIN

/* ========================================================================= */
/* _Py_hashtable (port of CPython 3.15.2 Python/hashtable.c)                  */
/* ========================================================================= */

/* Single linked list */

typedef struct _Py_slist_item_s {
    struct _Py_slist_item_s *next;
} _Py_slist_item_t;

typedef struct {
    _Py_slist_item_t *head;
} _Py_slist_t;

#define QP_CAPI_SLIST_ITEM_NEXT(ITEM) (((_Py_slist_item_t *)(ITEM))->next)

#define QP_CAPI_SLIST_HEAD(SLIST) (((_Py_slist_t *)(SLIST))->head)


/* _Py_hashtable: table entry */

typedef struct {
    /* used by _Py_hashtable_t.buckets to link entries */
    _Py_slist_item_t _Py_slist_item;

    Py_uhash_t key_hash;
    void *key;
    void *value;
} _Py_hashtable_entry_t;


/* _Py_hashtable: prototypes */

/* Forward declaration */
struct _Py_hashtable_t;
typedef struct _Py_hashtable_t _Py_hashtable_t;

typedef Py_uhash_t (*_Py_hashtable_hash_func) (const void *key);
typedef int (*_Py_hashtable_compare_func) (const void *key1, const void *key2);
typedef void (*_Py_hashtable_destroy_func) (void *key);
typedef _Py_hashtable_entry_t* (*_Py_hashtable_get_entry_func)(_Py_hashtable_t *ht,
                                                               const void *key);

typedef struct {
    // Allocate a memory block
    void* (*malloc) (size_t size);

    // Release a memory block
    void (*free) (void *ptr);
} _Py_hashtable_allocator_t;


/* _Py_hashtable: table */
struct _Py_hashtable_t {
    size_t nentries; // Total number of entries in the table
    size_t nbuckets;
    _Py_slist_t *buckets;

    _Py_hashtable_get_entry_func get_entry_func;
    _Py_hashtable_hash_func hash_func;
    _Py_hashtable_compare_func compare_func;
    _Py_hashtable_destroy_func key_destroy_func;
    _Py_hashtable_destroy_func value_destroy_func;
    _Py_hashtable_allocator_t alloc;
};

// Export _Py_hashtable functions for '_testinternalcapi' shared extension
PyAPI_FUNC(_Py_hashtable_t *) _Py_hashtable_new(
    _Py_hashtable_hash_func hash_func,
    _Py_hashtable_compare_func compare_func);

/* Hash a pointer (void*) */
PyAPI_FUNC(Py_uhash_t) _Py_hashtable_hash_ptr(const void *key);

/* Comparison using memcmp() */
PyAPI_FUNC(int) _Py_hashtable_compare_direct(
    const void *key1,
    const void *key2);

PyAPI_FUNC(_Py_hashtable_t *) _Py_hashtable_new_full(
    _Py_hashtable_hash_func hash_func,
    _Py_hashtable_compare_func compare_func,
    _Py_hashtable_destroy_func key_destroy_func,
    _Py_hashtable_destroy_func value_destroy_func,
    _Py_hashtable_allocator_t *allocator);

PyAPI_FUNC(void) _Py_hashtable_destroy(_Py_hashtable_t *ht);

PyAPI_FUNC(void) _Py_hashtable_clear(_Py_hashtable_t *ht);

typedef int (*_Py_hashtable_foreach_func) (_Py_hashtable_t *ht,
                                           const void *key, const void *value,
                                           void *user_data);

/* Call func() on each entry of the hashtable.
   Iteration stops if func() result is non-zero, in this case it's the result
   of the call. Otherwise, the function returns 0. */
PyAPI_FUNC(int) _Py_hashtable_foreach(
    _Py_hashtable_t *ht,
    _Py_hashtable_foreach_func func,
    void *user_data);

PyAPI_FUNC(size_t) _Py_hashtable_size(const _Py_hashtable_t *ht);
PyAPI_FUNC(size_t) _Py_hashtable_len(const _Py_hashtable_t *ht);

/* Add a new entry to the hash. The key must not be present in the hash table.
   Return 0 on success, -1 on memory error. */
PyAPI_FUNC(int) _Py_hashtable_set(
    _Py_hashtable_t *ht,
    const void *key,
    void *value);


/* Get an entry.
   Return NULL if the key does not exist. */
static inline _Py_hashtable_entry_t *
_Py_hashtable_get_entry(_Py_hashtable_t *ht, const void *key)
{
    return ht->get_entry_func(ht, key);
}


/* Get value from an entry.
   Return NULL if the entry is not found.

   Use _Py_hashtable_get_entry() to distinguish entry value equal to NULL
   and entry not found. */
PyAPI_FUNC(void*) _Py_hashtable_get(_Py_hashtable_t *ht, const void *key);


/* Remove a key and its associated value without calling key and value destroy
   functions.

   Return the removed value if the key was found.
   Return NULL if the key was not found. */
PyAPI_FUNC(void*) _Py_hashtable_steal(
    _Py_hashtable_t *ht,
    const void *key);


#define QP_CAPI_HT_MIN_SIZE 16
#define QP_CAPI_HT_HIGH 0.50
#define QP_CAPI_HT_LOW 0.10
#define QP_CAPI_HT_REHASH_FACTOR 2.0 / (QP_CAPI_HT_LOW + QP_CAPI_HT_HIGH)

#define QP_CAPI_HT_BUCKETS_HEAD(SLIST) \
        ((_Py_hashtable_entry_t *)QP_CAPI_SLIST_HEAD(&(SLIST)))
#define QP_CAPI_HT_TABLE_HEAD(HT, BUCKET) \
        ((_Py_hashtable_entry_t *)QP_CAPI_SLIST_HEAD(&(HT)->buckets[BUCKET]))
#define QP_CAPI_HT_ENTRY_NEXT(ENTRY) \
        ((_Py_hashtable_entry_t *)QP_CAPI_SLIST_ITEM_NEXT(ENTRY))

/* Forward declaration */
static int qp_capi_ht_rehash(_Py_hashtable_t *ht);

static void
qp_capi_slist_init(_Py_slist_t *list)
{
    list->head = NULL;
}


static void
qp_capi_slist_prepend(_Py_slist_t *list, _Py_slist_item_t *item)
{
    item->next = list->head;
    list->head = item;
}


static void
qp_capi_slist_remove(_Py_slist_t *list, _Py_slist_item_t *previous,
                 _Py_slist_item_t *item)
{
    if (previous != NULL)
        previous->next = item->next;
    else
        list->head = item->next;
}


Py_uhash_t
_Py_hashtable_hash_ptr(const void *key)
{
    return (Py_uhash_t)Py_HashPointer(key);
}


int
_Py_hashtable_compare_direct(const void *key1, const void *key2)
{
    return (key1 == key2);
}


/* makes sure the real size of the buckets array is a power of 2 */
static size_t
qp_capi_ht_round_size(size_t s)
{
    size_t i;
    if (s < QP_CAPI_HT_MIN_SIZE)
        return QP_CAPI_HT_MIN_SIZE;
    i = 1;
    while (i < s)
        i <<= 1;
    return i;
}


size_t
_Py_hashtable_size(const _Py_hashtable_t *ht)
{
    size_t size = sizeof(_Py_hashtable_t);
    /* buckets */
    size += ht->nbuckets * sizeof(_Py_hashtable_entry_t *);
    /* entries */
    size += ht->nentries * sizeof(_Py_hashtable_entry_t);
    return size;
}


size_t
_Py_hashtable_len(const _Py_hashtable_t *ht)
{
    return ht->nentries;
}


_Py_hashtable_entry_t *
_Py_hashtable_get_entry_generic(_Py_hashtable_t *ht, const void *key)
{
    Py_uhash_t key_hash = ht->hash_func(key);
    size_t index = key_hash & (ht->nbuckets - 1);
    _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, index);
    while (1) {
        if (entry == NULL) {
            return NULL;
        }
        if (entry->key_hash == key_hash && ht->compare_func(key, entry->key)) {
            break;
        }
        entry = QP_CAPI_HT_ENTRY_NEXT(entry);
    }
    return entry;
}


// Specialized for:
// hash_func == _Py_hashtable_hash_ptr
// compare_func == _Py_hashtable_compare_direct
static _Py_hashtable_entry_t *
qp_capi_ht_get_entry_ptr(_Py_hashtable_t *ht, const void *key)
{
    Py_uhash_t key_hash = _Py_hashtable_hash_ptr(key);
    size_t index = key_hash & (ht->nbuckets - 1);
    _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, index);
    while (1) {
        if (entry == NULL) {
            return NULL;
        }
        // Compare directly keys (ignore entry->key_hash)
        if (entry->key == key) {
            break;
        }
        entry = QP_CAPI_HT_ENTRY_NEXT(entry);
    }
    return entry;
}


void*
_Py_hashtable_steal(_Py_hashtable_t *ht, const void *key)
{
    Py_uhash_t key_hash = ht->hash_func(key);
    size_t index = key_hash & (ht->nbuckets - 1);

    _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, index);
    _Py_hashtable_entry_t *previous = NULL;
    while (1) {
        if (entry == NULL) {
            // not found
            return NULL;
        }
        if (entry->key_hash == key_hash && ht->compare_func(key, entry->key)) {
            break;
        }
        previous = entry;
        entry = QP_CAPI_HT_ENTRY_NEXT(entry);
    }

    qp_capi_slist_remove(&ht->buckets[index], (_Py_slist_item_t *)previous,
                     (_Py_slist_item_t *)entry);
    ht->nentries--;

    void *value = entry->value;
    (ht->alloc.free)(entry);

    if ((float)ht->nentries / (float)ht->nbuckets < QP_CAPI_HT_LOW) {
        // Ignore failure: error cannot be reported to the caller
        qp_capi_ht_rehash(ht);
    }
    return value;
}


int
_Py_hashtable_set(_Py_hashtable_t *ht, const void *key, void *value)
{
    _Py_hashtable_entry_t *entry;

#ifndef NDEBUG
    /* Don't write the assertion on a single line because it is interesting
       to know the duplicated entry if the assertion failed. The entry can
       be read using a debugger. */
    entry = ht->get_entry_func(ht, key);
    assert(entry == NULL);
#endif

    entry = (ht->alloc.malloc)(sizeof(_Py_hashtable_entry_t));
    if (entry == NULL) {
        /* memory allocation failed */
        return -1;
    }

    entry->key_hash = ht->hash_func(key);
    entry->key = (void *)key;
    entry->value = value;

    ht->nentries++;
    if ((float)ht->nentries / (float)ht->nbuckets > QP_CAPI_HT_HIGH) {
        if (qp_capi_ht_rehash(ht) < 0) {
            ht->nentries--;
            (ht->alloc.free)(entry);
            return -1;
        }
    }

    size_t index = entry->key_hash & (ht->nbuckets - 1);
    qp_capi_slist_prepend(&ht->buckets[index], (_Py_slist_item_t*)entry);
    return 0;
}


void*
_Py_hashtable_get(_Py_hashtable_t *ht, const void *key)
{
    _Py_hashtable_entry_t *entry = ht->get_entry_func(ht, key);
    if (entry != NULL) {
        return entry->value;
    }
    else {
        return NULL;
    }
}


int
_Py_hashtable_foreach(_Py_hashtable_t *ht,
                      _Py_hashtable_foreach_func func,
                      void *user_data)
{
    for (size_t hv = 0; hv < ht->nbuckets; hv++) {
        _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, hv);
        while (entry != NULL) {
            int res = func(ht, entry->key, entry->value, user_data);
            if (res) {
                return res;
            }
            entry = QP_CAPI_HT_ENTRY_NEXT(entry);
        }
    }
    return 0;
}


static int
qp_capi_ht_rehash(_Py_hashtable_t *ht)
{
    size_t new_size = qp_capi_ht_round_size((size_t)(ht->nentries * QP_CAPI_HT_REHASH_FACTOR));
    if (new_size == ht->nbuckets) {
        return 0;
    }

    size_t buckets_size = new_size * sizeof(ht->buckets[0]);
    _Py_slist_t *new_buckets = (ht->alloc.malloc)(buckets_size);
    if (new_buckets == NULL) {
        /* memory allocation failed */
        return -1;
    }
    memset(new_buckets, 0, buckets_size);

    for (size_t bucket = 0; bucket < ht->nbuckets; bucket++) {
        _Py_hashtable_entry_t *entry = QP_CAPI_HT_BUCKETS_HEAD(ht->buckets[bucket]);
        while (entry != NULL) {
            assert(ht->hash_func(entry->key) == entry->key_hash);
            _Py_hashtable_entry_t *next = QP_CAPI_HT_ENTRY_NEXT(entry);
            size_t entry_index = entry->key_hash & (new_size - 1);

            qp_capi_slist_prepend(&new_buckets[entry_index], (_Py_slist_item_t*)entry);

            entry = next;
        }
    }

    (ht->alloc.free)(ht->buckets);
    ht->nbuckets = new_size;
    ht->buckets = new_buckets;
    return 0;
}


_Py_hashtable_t *
_Py_hashtable_new_full(_Py_hashtable_hash_func hash_func,
                       _Py_hashtable_compare_func compare_func,
                       _Py_hashtable_destroy_func key_destroy_func,
                       _Py_hashtable_destroy_func value_destroy_func,
                       _Py_hashtable_allocator_t *allocator)
{
    _Py_hashtable_allocator_t alloc;
    if (allocator == NULL) {
        alloc.malloc = PyMem_Malloc;
        alloc.free = PyMem_Free;
    }
    else {
        alloc = *allocator;
    }

    _Py_hashtable_t *ht = (_Py_hashtable_t *)(alloc.malloc)(sizeof(_Py_hashtable_t));
    if (ht == NULL) {
        return ht;
    }

    ht->nbuckets = QP_CAPI_HT_MIN_SIZE;
    ht->nentries = 0;

    size_t buckets_size = ht->nbuckets * sizeof(ht->buckets[0]);
    ht->buckets = (alloc.malloc)(buckets_size);
    if (ht->buckets == NULL) {
        (alloc.free)(ht);
        return NULL;
    }
    memset(ht->buckets, 0, buckets_size);

    ht->get_entry_func = _Py_hashtable_get_entry_generic;
    ht->hash_func = hash_func;
    ht->compare_func = compare_func;
    ht->key_destroy_func = key_destroy_func;
    ht->value_destroy_func = value_destroy_func;
    ht->alloc = alloc;
    if (ht->hash_func == _Py_hashtable_hash_ptr
        && ht->compare_func == _Py_hashtable_compare_direct)
    {
        ht->get_entry_func = qp_capi_ht_get_entry_ptr;
    }
    return ht;
}


_Py_hashtable_t *
_Py_hashtable_new(_Py_hashtable_hash_func hash_func,
                  _Py_hashtable_compare_func compare_func)
{
    return _Py_hashtable_new_full(hash_func, compare_func,
                                  NULL, NULL, NULL);
}


static void
qp_capi_ht_destroy_entry(_Py_hashtable_t *ht, _Py_hashtable_entry_t *entry)
{
    if (ht->key_destroy_func) {
        ht->key_destroy_func(entry->key);
    }
    if (ht->value_destroy_func) {
        ht->value_destroy_func(entry->value);
    }
    (ht->alloc.free)(entry);
}


void
_Py_hashtable_clear(_Py_hashtable_t *ht)
{
    for (size_t i=0; i < ht->nbuckets; i++) {
        _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, i);
        while (entry != NULL) {
            _Py_hashtable_entry_t *next = QP_CAPI_HT_ENTRY_NEXT(entry);
            qp_capi_ht_destroy_entry(ht, entry);
            entry = next;
        }
        qp_capi_slist_init(&ht->buckets[i]);
    }
    ht->nentries = 0;
    // Ignore failure: clear function is not expected to fail
    // because of a memory allocation failure.
    (void)qp_capi_ht_rehash(ht);
}


void
_Py_hashtable_destroy(_Py_hashtable_t *ht)
{
    for (size_t i = 0; i < ht->nbuckets; i++) {
        _Py_hashtable_entry_t *entry = QP_CAPI_HT_TABLE_HEAD(ht, i);
        while (entry) {
            _Py_hashtable_entry_t *entry_next = QP_CAPI_HT_ENTRY_NEXT(entry);
            qp_capi_ht_destroy_entry(ht, entry);
            entry = entry_next;
        }
    }

    (ht->alloc.free)(ht->buckets);
    (ht->alloc.free)(ht);
}


#undef QP_CAPI_HT_MIN_SIZE
#undef QP_CAPI_HT_HIGH
#undef QP_CAPI_HT_LOW
#undef QP_CAPI_HT_REHASH_FACTOR
#undef QP_CAPI_HT_BUCKETS_HEAD
#undef QP_CAPI_HT_TABLE_HEAD
#undef QP_CAPI_HT_ENTRY_NEXT
#undef QP_CAPI_SLIST_HEAD
#undef QP_CAPI_SLIST_ITEM_NEXT

/* ========================================================================= */
/* Init (E2 step of qp_capi_init)                                             */
/* ========================================================================= */

static int qp_capi_init_runtime(JSContext *ctx)
{
    PyInterpreterState *interp = &qp_capi_interp_storage;
    struct QPCapiThreadState *ts;
    QPModState *ms;
    QPThreads *thr;

    if (qp_capi.main_tstate != NULL)
        return 0;
    qp_capi_llist_init(&interp->asyncio_tasks_head);
    interp->threads.head = NULL;
    interp->stoptheworld.world_stopped = 0;
    /* the GIL exists whenever the C API is used: a foreign thread may call
       PyGILState_Ensure while the main thread is in a blocking call */
    thr = qp_threads_get(ctx);
    if (thr == NULL)
        return -1;
    qp_capi_main_thread = (unsigned long)thr->main_ts->ident;
    qp_capi.interp = interp;
    ts = qp_capi_tstate_alloc(interp);
    if (ts == NULL) {
        qp_capi.interp = NULL;
        qp_raise_memory_error(ctx);
        return -1;
    }
    ts->impl.base.thread_id = qp_capi_main_thread;
    ts->impl.base.gilstate_counter = 1;
    qp_capi.main_tstate = ts;
    ts->impl.base._status.active = 1;
    /* bind every runtime thread (the others get theirs when they resume)
       and move qp_capi.tstate_current with the GIL from now on */
    thr->main_ts->capi_bound = &ts->impl.base;
    thr->main_ts->capi_tstate = &ts->impl.base;
    {
        struct list_head *el;
        list_for_each(el, &thr->tstates) {
            QPThreadState *o = list_entry(el, QPThreadState, link);
            if (o != thr->main_ts && qp_capi_tstate_bind(o) != NULL)
                o->capi_tstate = o->capi_bound;
        }
    }
    qp_capi.tstate_current = (PyThreadState *)thr->current->capi_bound;
    qp_capi_tls_tstate = qp_capi.tstate_current;
    qp_tstate_capi_hook = qp_capi_thread_hook;

    qp_capi.kind_ops[QP_CAPI_K_CODE] = qp_capi_code_kind_ops;

    if (qp_capi_rt_holder == NULL) {
        qp_capi_rt_holder = PyDict_New();
        if (qp_capi_rt_holder == NULL)
            goto fail;
    }

    /* the contextvars types of our _contextvars (created lazily by
       mod_misc.c without importing the module) */
    ms = qp_ctxvar_state(ctx);
    if (ms == NULL ||
        qp_capi_bind_static_type(ctx, &PyContext_Type, qp_ctxvar_type(ms, QP_CAPI_CTXVAR_T_CONTEXT)) < 0 ||
        qp_capi_bind_static_type(ctx, &PyContextVar_Type, qp_ctxvar_type(ms, QP_CAPI_CTXVAR_T_VAR)) < 0 ||
        qp_capi_bind_static_type(ctx, &PyContextToken_Type, qp_ctxvar_type(ms, QP_CAPI_CTXVAR_T_TOKEN)) < 0)
        goto fail;
    return 0;
fail:
    Py_CLEAR(qp_capi_rt_holder);
    qp_tstate_capi_hook = NULL;
    {
        struct list_head *el;
        list_for_each(el, &thr->tstates) {
            QPThreadState *o = list_entry(el, QPThreadState, link);
            if (o->capi_bound && o->capi_bound != &ts->impl.base)
                qp_capi_tstate_free((PyThreadState *)o->capi_bound);
            o->capi_bound = o->capi_tstate = NULL;
        }
    }
    qp_capi_tls_tstate = NULL;
    qp_capi_tstate_unlink(&ts->impl.base);
    PyMem_RawFree(ts);
    qp_capi.main_tstate = NULL;
    qp_capi.tstate_current = NULL;
    qp_capi.interp = NULL;
    return -1;
}

/* qp_capi_context_free, first, while the objects work (CPython
   PyInterpreterState_Clear / PyThreadState_Clear): the objects the
   interpreter and thread states hold. */
static void qp_capi_runtime_clear(void)
{
    PyInterpreterState *interp = qp_capi.interp;
    PyThreadState *t;

    if (!interp)
        return;
    for (t = interp->threads.head; t; t = t->next)
        qp_capi_tstate_clear(t);
    Py_CLEAR(interp->qp_dict);
    Py_CLEAR(qp_capi_rt_holder);
    /* the watchers are per interpreter (CPython interpreter_clear) */
    qp_capi_dict_watchers_clear();
    memset(qp_capi_code_watchers, 0, sizeof(qp_capi_code_watchers));
    qp_capi_code_watchers_set = 0;
    memset(qp_capi_context_watchers, 0, sizeof(qp_capi_context_watchers));
    qp_capi_context_watchers_set = 0;
}

/* qp_capi_context_free, last: the thread states and the interpreter state
   go with the runtime, so that the C API of a later runtime starts again
   from qp_capi_init_runtime. */
static void qp_capi_runtime_reset(JSRuntime *rt)
{
    PyInterpreterState *interp = qp_capi.interp;
    QPThreads *thr = rt->py_threads;
    PyThreadState *t;

    qp_tstate_capi_hook = NULL;
    if (thr) {
        struct list_head *el;
        list_for_each(el, &thr->tstates) {
            QPThreadState *o = list_entry(el, QPThreadState, link);
            o->capi_bound = o->capi_tstate = NULL;
        }
    }
    if (interp) {
        /* the main thread state, those of the runtime's threads and of the
           foreign threads (PyGILState_Ensure) */
        while ((t = interp->threads.head) != NULL)
            qp_capi_tstate_free(t);
        memset(interp, 0, sizeof(*interp));
    }
    qp_capi.main_tstate = NULL;
    qp_capi.tstate_current = NULL;
    qp_capi.interp = NULL;
    qp_capi_tls_tstate = NULL;
    qp_capi_finalizing = 0;
}

#undef QP_CAPI_CTXVAR_T_CONTEXT
#undef QP_CAPI_CTXVAR_T_VAR
#undef QP_CAPI_CTXVAR_T_TOKEN
#undef QP_CAPI_NO_TSTATE_MSG
#undef QP_CAPI_NPENDINGCALLS
#undef QP_CAPI_CF_PARSE_FLAGS
#undef QP_CAPI_HAS_PARKED
#undef QP_CAPI_ONCE_INITIALIZED
#undef QP_CAPI_NEXITFUNCS
#undef QP_CAPI_CO_FAST_ARG_POS
#undef QP_CAPI_CO_FAST_ARG_KW
#undef QP_CAPI_CO_FAST_ARG_VAR
#undef QP_CAPI_CO_FAST_LOCAL
#undef QP_CAPI_CO_FAST_CELL
#undef QP_CAPI_CO_FAST_FREE
