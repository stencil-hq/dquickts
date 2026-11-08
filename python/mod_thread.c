/*
 * _thread module: port of CPython Modules/_threadmodule.c (PSF-2.0).
 *
 * Threads are OS threads sharing the runtime under the GIL (python/ceval.c,
 * "Threads and the GIL").  The first start_new_thread() /
 * start_joinable_thread() creates the runtime's QPThreads, the calling
 * thread becoming the main thread.  A new thread (qp_thread_entry) takes
 * the GIL, calls the function and, when it returns, releases its thread
 * state (thread-local dicts, handled exception, contextvars Context),
 * marks its _ThreadHandle done and drops the GIL for good, like CPython's
 * thread_run().  Threads are created detached: joining waits for the
 * handle to be done.
 *
 *   - The state of lock, RLock and _ThreadHandle objects is kept in their
 *     QPInstance slots (JS ints) and only changed under the GIL.  A
 *     blocking acquire or join waits in qp_thread_wait(), the GIL released;
 *     releasing a lock or finishing a thread calls qp_thread_notify().  The
 *     main thread runs the Python signal handlers while it waits
 *     (_PY_LOCK_HANDLE_SIGNALS); with a single thread only a signal handler
 *     can release a held lock.
 *   - _local keeps one dict per thread in slot 0 (thread serial -> dict);
 *     the running thread's dict is installed as the instance __dict__ before
 *     every attribute access (qp_thread_local_switch), creating it and
 *     re-running __init__ on the first access from a new thread.  A thread
 *     exiting removes its dicts from the _local objects it used.
 *   - get_ident() is (unsigned long)pthread_self() of the calling thread.
 *   - _shutdown() joins the non-daemon handles; the launcher calls
 *     threading._shutdown() before the atexit callbacks.
 *   - os.fork() in a threaded process: qp_thread_after_fork_child() (called
 *     by posix.fork in the child) keeps only the forking thread, which
 *     becomes the main thread (CPython PyOS_AfterFork_Child).
 */
#include "modstate.h"
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#if defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#endif

/* python/mod_misc.c (_signal): PyErr_SetInterruptEx; 0, or -1 with
   ValueError "signal number out of range". */
static int qp_signal_trip(JSContext *ctx, int signum);
/* python/typeobject_attr.h: obj.__dict__ = value (subtype_setdict) */
static int qp_subtype_dict_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic);
/* python/mod_weakref.c */
static JSValue qp_weakref_new(JSContext *ctx, JSValueConst obj, JSValueConst callback);
static JSValue qp_weakref_get(JSContext *ctx, JSValueConst ref);
/* python/errors.c */
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);

enum {
    QP_THREAD_T_LOCK = 0,
    QP_THREAD_T_RLOCK = 1,
    QP_THREAD_T_LOCAL = 2,
    QP_THREAD_T_HANDLE = 3,
    QP_THREAD_T_EXCEPTHOOK = 4,
};

/* PY_TIMEOUT_MAX of thread_pthread (microseconds) */
#define QP_THREAD_TIMEOUT_MAX_US (LLONG_MAX / 1000)
#define QP_THREAD_UNSET_TIMEOUT ((int64_t)-1000000000)   /* _PyTime_FromSeconds(-1) */
/* _PyOS_MIN_STACK_SIZE + SYSTEM_PAGE_SIZE (release build) */
#define QP_THREAD_MIN_STACK ((Py_ssize_t)((1 << 11) * sizeof(void *) * 3 + 4096))
#define QP_THREAD_STACK_MIN 0x8000      /* thread_pthread.h THREAD_STACK_MIN */
#if defined(__APPLE__)
#define QP_THREAD_NAME_MAXLEN 63
#elif defined(__linux__) || defined(__NetBSD__)
#define QP_THREAD_NAME_MAXLEN 15
#endif

/* qp_thread_stacksize (python/ceval.h): stack size of new threads
   (PyThread_set_stacksize; process-wide) */

/* ThreadHandle states */
enum { QP_TH_NOT_STARTED, QP_TH_STARTING, QP_TH_RUNNING, QP_TH_DONE };

static uint64_t qp_thread_ident(void)
{
    return (uint64_t)(uintptr_t)pthread_self();
}

static QPType *qp_thread_type(JSContext *ctx, int which)
{
    QPModState *ms = qp_modstate(ctx);
    if (!ms)
        return NULL;
    return qp_type_from_value(ms->thread_types[which]);
}

static int qp_thread_slot_int(JSValueConst obj, int i)
{
    JSValue v = qp_instance_slots(obj)[i];
    return JS_VALUE_GET_TAG(v) == JS_TAG_INT ? JS_VALUE_GET_INT(v) : 0;
}

static void qp_thread_set_slot(JSContext *ctx, JSValueConst obj, int i, JSValue v)
{
    JSValue *s = qp_instance_slots(obj);
    JSValue old = s[i];
    s[i] = v;
    if (!QP_IS_NULL(old))
        JS_FreeValue(ctx, old);
}

/* unsigned 64-bit value of an ident slot (0 when unset) */
static uint64_t qp_thread_slot_u64(JSContext *ctx, JSValueConst obj, int i)
{
    JSValue v = qp_instance_slots(obj)[i];
    int64_t r = 0;
    int overflow = 0;
    if (QP_IS_NULL(v))
        return 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return (uint64_t)(uint32_t)JS_VALUE_GET_INT(v);
    if (qp_long_as_int64_overflow(v, &r, &overflow) < 0 || overflow) {
        double d = 0;
        if (qp_long_as_double(ctx, v, &d) < 0) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return 0;
        }
        return (uint64_t)d;
    }
    return (uint64_t)r;
}

/* Type name for reprs: tp_name of builtin types, __name__ of heap types
   (CPython heap type tp_name).  Free with JS_FreeCString when *pfree. */
static const char *qp_thread_tp_name(JSContext *ctx, JSValueConst self, BOOL *pfree)
{
    QPType *t = qp_type_of(ctx, self);
    *pfree = FALSE;
    if (t->tp_name)
        return t->tp_name;
    {
        const char *s = JS_AtomToCString(ctx, t->name);
        if (s)
            *pfree = TRUE;
        return s;
    }
}

/* _PyTime_FromSecondsObject(..., _PyTime_ROUND_TIMEOUT) into nanoseconds */
static int qp_thread_time_from_object(JSContext *ctx, JSValueConst obj, int64_t *out)
{
    if (qp_float_check(ctx, obj)) {
        double d;
        if (qp_float_as_double(ctx, obj, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
        d *= 1e9;
        d = d >= 0 ? ceil(d) : floor(d);    /* ROUND_UP: away from zero */
        if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
            qp_raise_overflow_error(ctx, "timestamp too large to convert to C PyTime_t");
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    } else {
        int64_t secs;
        int overflow = 0;
        JSValue v = qp_index(ctx, obj);
        if (JS_IsException(v))
            return -1;
        if (qp_long_as_int64_overflow(v, &secs, &overflow) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        if (overflow || secs > INT64_MAX / 1000000000 || secs < INT64_MIN / 1000000000) {
            qp_raise_overflow_error(ctx, "timestamp too large to convert to C PyTime_t");
            return -1;
        }
        *out = secs * 1000000000;
        return 0;
    }
}

/* lock_acquire_parse_timeout: *timeout = -1 (forever) or >= 0 ns */
static int qp_thread_parse_timeout(JSContext *ctx, JSValueConst timeout_obj, int blocking,
                                   int64_t *timeout)
{
    *timeout = QP_THREAD_UNSET_TIMEOUT;
    if (!QP_IS_NULL(timeout_obj) && qp_thread_time_from_object(ctx, timeout_obj, timeout) < 0)
        return -1;
    if (!blocking && *timeout != QP_THREAD_UNSET_TIMEOUT) {
        qp_raise_value_error(ctx, "can't specify a timeout for a non-blocking call");
        return -1;
    }
    if (*timeout < 0 && *timeout != QP_THREAD_UNSET_TIMEOUT) {
        qp_raise_value_error(ctx, "timeout value must be a non-negative number");
        return -1;
    }
    if (!blocking) {
        *timeout = 0;
    } else if (*timeout != QP_THREAD_UNSET_TIMEOUT) {
        int64_t us = *timeout / 1000 + (*timeout % 1000 != 0);   /* ROUND_TIMEOUT */
        if (us > QP_THREAD_TIMEOUT_MAX_US) {
            qp_raise_overflow_error(ctx, "timeout value is too large");
            return -1;
        }
    } else {
        *timeout = -1;
    }
    return 0;
}

/* __new__ of lock / RLock: clinic checks for the exact type only */
static int qp_thread_no_args(JSContext *ctx, QPType *subtype, QPType *t, const char *name,
                             size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)(nargsf & ~QP_VECTORCALL_ARGUMENTS_OFFSET);
    if (subtype != t)
        return 0;
    if (nargs > 0) {
        qp_raise_type_error(ctx, "%s() takes no positional arguments", name);
        return -1;
    }
    if (!QP_IS_NULL(kwnames) && !JS_IsUndefined(kwnames) && QP_TUPLE_SIZE(kwnames) > 0) {
        qp_raise_type_error(ctx, "%s() takes no keyword arguments", name);
        return -1;
    }
    return 0;
}

static const char *const qp_thread_acquire_kw[] = { "blocking", "timeout", NULL };

static int qp_thread_acquire_args(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                                  JSValueConst kwnames, int64_t *timeout)
{
    QPArgSpec spec = { fname, qp_thread_acquire_kw, 0, 2, 0 };
    JSValueConst a[2];
    int blocking = 1;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return -1;
    if (!QP_IS_NULL(a[0]) && qp_arg_bool(ctx, a[0], &blocking) < 0)
        return -1;
    return qp_thread_parse_timeout(ctx, a[1], blocking, timeout);
}

/* ======================= _ThreadHandle ================================== */
/* slots: 0 ident (int), 1 state (JS int), 2 qp_thread_fork_gen when made
   by _make_thread_handle() (a thread that this module did not start: the
   main thread, a _DummyThread), else unset */

/* number of os.fork() calls between the start of the process and the
   current process, and the thread that called the last one: the only
   thread of the process then (qp_thread_after_fork_child) */
static uint32_t qp_thread_fork_gen;
static uint64_t qp_thread_fork_ident;

static JSValue qp_thread_handle_alloc(JSContext *ctx, QPType *t)
{
    JSValue obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    qp_thread_set_slot(ctx, obj, 0, JS_NewInt32(ctx, 0));
    qp_thread_set_slot(ctx, obj, 1, JS_NewInt32(ctx, QP_TH_NOT_STARTED));
    return obj;
}

static JSValue qp_thread_handle_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    return qp_thread_handle_alloc(ctx, subtype);
}

static JSValue qp_thread_handle_repr(JSContext *ctx, JSValueConst self)
{
    BOOL fr;
    const char *tn = qp_thread_tp_name(ctx, self, &fr);
    char buf[300];
    if (!tn)
        return JS_EXCEPTION;
    snprintf(buf, sizeof buf, "<%s object: ident=%llu>", tn,
             (unsigned long long)qp_thread_slot_u64(ctx, self, 0));
    if (fr)
        JS_FreeCString(ctx, tn);
    return qp_str_from_cstr(ctx, buf);
}

static JSValue qp_thread_handle_get_ident(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_long_from_uint64(ctx, qp_thread_slot_u64(ctx, self, 0));
}

static BOOL qp_thread_handle_is_exiting(JSContext *ctx, JSValueConst self)
{
    JSValue gen;
    int state = qp_thread_slot_int(self, 1);
    if (state != QP_TH_RUNNING)
        return state == QP_TH_DONE;
    /* _PyThread_AfterFork: in the child of a fork, the handles of the
       threads but the forking one are done.  The handles of the threads
       started here are marked by qp_thread_after_fork_child(); the made
       ones are recognized here: made before the last fork, for another
       thread than the forking one. */
    if (((QPInstance *)JS_VALUE_GET_OBJ(self)->u.opaque)->nslots < 3)
        return FALSE;       /* restored from a snapshot of an older build */
    gen = qp_instance_slots(self)[2];
    if (JS_VALUE_GET_TAG(gen) != JS_TAG_INT ||
        (uint32_t)JS_VALUE_GET_INT(gen) >= qp_thread_fork_gen ||
        qp_thread_slot_u64(ctx, self, 0) == qp_thread_fork_ident)
        return FALSE;
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, QP_TH_DONE));
    return TRUE;
}

static int qp_thread_handle_check_started(JSContext *ctx, JSValueConst self)
{
    if (qp_thread_slot_int(self, 1) < QP_TH_RUNNING) {
        qp_raise_runtime_error(ctx, "thread not started");
        return -1;
    }
    return 0;
}

/* ThreadHandle_join */
static int qp_thread_handle_join_impl(JSContext *ctx, JSValueConst self, int64_t timeout_ns)
{
    int r;
    if (qp_thread_handle_check_started(ctx, self) < 0)
        return -1;
    if (!qp_thread_handle_is_exiting(ctx, self)) {
        QPThreads *thr = ctx->rt->py_threads;
        if (qp_thread_slot_u64(ctx, self, 0) == qp_thread_ident()) {
            qp_raise_runtime_error(ctx, "Cannot join current thread");
            return -1;
        }
        if (thr && thr->finalizing) {
            qp_raise(ctx, QP_EXC(ctx, PythonFinalizationError),
                     "cannot join thread at interpreter shutdown");
            return -1;
        }
    }
    r = qp_thread_wait(ctx, qp_thread_handle_is_exiting, self, timeout_ns < 0 ? -1 : timeout_ns,
                       TRUE);
    return r < 0 ? -1 : 0;
}

static JSValue qp_thread_handle_join(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", NULL };
    QPArgSpec spec = { "join", kw, 0, 1, 0 };
    JSValueConst a[1];
    int64_t timeout_ns = -1;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && !JS_IsUndefined(a[0]) && !JS_IsNull(a[0])) {
        if (qp_thread_time_from_object(ctx, a[0], &timeout_ns) < 0)
            return JS_EXCEPTION;
    }
    if (qp_thread_handle_join_impl(ctx, self, timeout_ns) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_thread_handle_is_done(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_thread_handle_is_exiting(ctx, self));
}

static JSValue qp_thread_handle_set_done(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    if (qp_thread_handle_check_started(ctx, self) < 0)
        return JS_EXCEPTION;
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, QP_TH_DONE));
    qp_thread_notify(ctx);
    return JS_UNDEFINED;
}

static const QPGetSetDef qp_thread_handle_getsets[] = {
    { "ident", qp_thread_handle_get_ident, NULL, 0, NULL },
    { NULL }
};

static const QPMethodDef qp_thread_handle_methods[] = {
    { "join", qp_thread_handle_join, QP_METH_FASTCALL, 0, NULL },
    { "_set_done", qp_thread_handle_set_done, QP_METH_NOARGS, 0, NULL },
    { "is_done", qp_thread_handle_is_done, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_thread_handle_spec = {
    .name = "_thread._ThreadHandle",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 3,
    .slots = {
        .tp_repr = qp_thread_handle_repr,
        .tp_new = qp_thread_handle_new,
    },
    .methods = qp_thread_handle_methods,
    .getsets = qp_thread_handle_getsets,
};

/* ======================= lock ============================================ */
/* slot 0: locked (JS int 0/1) */

static BOOL qp_thread_lock_is_free(JSContext *ctx, JSValueConst self)
{
    return qp_thread_slot_int(self, 0) == 0;
}

static JSValue qp_thread_lock_alloc(JSContext *ctx, QPType *t)
{
    JSValue obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    qp_thread_set_slot(ctx, obj, 0, JS_NewInt32(ctx, 0));
    return obj;
}

static JSValue qp_thread_lock_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    if (qp_thread_no_args(ctx, subtype, qp_thread_type(ctx, QP_THREAD_T_LOCK), "lock",
                          nargsf, kwnames) < 0)
        return JS_EXCEPTION;
    return qp_thread_lock_alloc(ctx, subtype);
}

/* _PyMutex_LockTimed with _PY_LOCK_HANDLE_SIGNALS */
static JSValue qp_thread_lock_acquire_impl(JSContext *ctx, JSValueConst self, int64_t timeout)
{
    int r = qp_thread_wait(ctx, qp_thread_lock_is_free, self, timeout, TRUE);
    if (r < 0)
        return JS_EXCEPTION;
    if (r)
        qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 1));
    return qp_bool_new(r);
}

static JSValue qp_thread_lock_acquire(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    int64_t timeout;
    if (qp_thread_acquire_args(ctx, magic ? "acquire_lock" : "acquire", args, nargs, kwnames,
                               &timeout) < 0)
        return JS_EXCEPTION;
    return qp_thread_lock_acquire_impl(ctx, self, timeout);
}

static JSValue qp_thread_lock_enter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_thread_lock_acquire_impl(ctx, self, -1);
}

static JSValue qp_thread_lock_release(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    if (qp_thread_slot_int(self, 0) == 0)
        return qp_raise_runtime_error(ctx, "release unlocked lock");
    qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 0));
    qp_thread_notify(ctx);
    return JS_UNDEFINED;
}

static JSValue qp_thread_lock_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "__exit__", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    return qp_thread_lock_release(ctx, self, args, 0, QP_NULL, 0);
}

static JSValue qp_thread_lock_locked(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_thread_slot_int(self, 0) != 0);
}

static JSValue qp_thread_lock_at_fork_reinit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                             int nargs, JSValueConst kwnames, int magic)
{
    qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 0));
    return JS_UNDEFINED;
}

static JSValue qp_thread_lock_repr(JSContext *ctx, JSValueConst self)
{
    BOOL fr;
    const char *tn = qp_thread_tp_name(ctx, self, &fr);
    char buf[300];
    if (!tn)
        return JS_EXCEPTION;
    snprintf(buf, sizeof buf, "<%s %s object at %p>",
             qp_thread_slot_int(self, 0) ? "locked" : "unlocked", tn, JS_VALUE_GET_PTR(self));
    if (fr)
        JS_FreeCString(ctx, tn);
    return qp_str_from_cstr(ctx, buf);
}

#define QP_THREAD_LOCK_ACQUIRE_DOC \
    "($self, /, blocking=True, timeout=-1)\n--\n\n"
static const QPMethodDef qp_thread_lock_methods[] = {
    { "acquire_lock", qp_thread_lock_acquire, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "acquire_lock" QP_THREAD_LOCK_ACQUIRE_DOC "An obsolete synonym of acquire()." },
    { "acquire", qp_thread_lock_acquire, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "acquire" QP_THREAD_LOCK_ACQUIRE_DOC
      "Lock the lock.\n\n"
      "Without argument, this blocks if the lock is already locked\n"
      "(even by the same thread), waiting for another thread to release\n"
      "the lock, and return True once the lock is acquired.\n"
      "With an argument, this will only block if the argument is true,\n"
      "and the return value reflects whether the lock is acquired.\n"
      "The blocking operation is interruptible." },
    { "release_lock", qp_thread_lock_release, QP_METH_NOARGS, 0,
      "release_lock($self, /)\n--\n\nAn obsolete synonym of release()." },
    { "release", qp_thread_lock_release, QP_METH_NOARGS, 0,
      "release($self, /)\n--\n\n"
      "Release the lock.\n\n"
      "Allows another thread that is blocked waiting for\n"
      "the lock to acquire the lock.  The lock must be in the locked state,\n"
      "but it needn't be locked by the same thread that unlocks it." },
    { "locked_lock", qp_thread_lock_locked, QP_METH_NOARGS, 0,
      "locked_lock($self, /)\n--\n\nAn obsolete synonym of locked()." },
    { "locked", qp_thread_lock_locked, QP_METH_NOARGS, 0,
      "locked($self, /)\n--\n\nReturn whether the lock is in the locked state." },
    { "__enter__", qp_thread_lock_enter, QP_METH_NOARGS, 0,
      "__enter__($self, /)\n--\n\nLock the lock." },
    { "__exit__", qp_thread_lock_exit, QP_METH_FASTCALL, 0,
      "__exit__($self, exc_type, exc_value, exc_tb, /)\n--\n\nRelease the lock." },
    { "_at_fork_reinit", qp_thread_lock_at_fork_reinit, QP_METH_NOARGS, 0,
      "_at_fork_reinit($self, /)\n--\n\n" },
    { NULL }
};

static const QPTypeSpec qp_thread_lock_spec = {
    .name = "_thread.lock",
    .doc = "lock()\n--\n\n"
           "A lock object is a synchronization primitive.  To create a lock,\n"
           "call threading.Lock().  Methods are:\n\n"
           "acquire() -- lock the lock, possibly blocking until it can be obtained\n"
           "release() -- unlock of the lock\n"
           "locked() -- test whether the lock is currently locked\n\n"
           "A lock is not owned by the thread that locked it; another thread may\n"
           "unlock it.  A thread attempting to lock a lock that it has already locked\n"
           "will block until another thread unlocks it.  Deadlocks may ensue.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_repr = qp_thread_lock_repr,
        .tp_new = qp_thread_lock_new,
    },
    .methods = qp_thread_lock_methods,
};

/* ======================= RLock =========================================== */
/* slots: 0 owner ident (int), 1 recursion level (JS int, 0 = unlocked) */

static BOOL qp_thread_rlock_is_free(JSContext *ctx, JSValueConst self)
{
    return qp_thread_slot_int(self, 1) == 0;
}

static BOOL qp_thread_rlock_is_mine(JSContext *ctx, JSValueConst self)
{
    return qp_thread_slot_int(self, 1) > 0 && qp_thread_slot_u64(ctx, self, 0) == qp_thread_ident();
}

static JSValue qp_thread_rlock_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    JSValue obj;
    if (qp_thread_no_args(ctx, subtype, qp_thread_type(ctx, QP_THREAD_T_RLOCK), "RLock",
                          nargsf, kwnames) < 0)
        return JS_EXCEPTION;
    obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    qp_thread_set_slot(ctx, obj, 0, JS_NewInt32(ctx, 0));
    qp_thread_set_slot(ctx, obj, 1, JS_NewInt32(ctx, 0));
    return obj;
}

static int qp_thread_rlock_take(JSContext *ctx, JSValueConst self, uint64_t owner, int32_t count)
{
    JSValue o = qp_long_from_uint64(ctx, owner);
    if (JS_IsException(o))
        return -1;
    qp_thread_set_slot(ctx, self, 0, o);
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, count));
    return 0;
}

static JSValue qp_thread_rlock_acquire_impl(JSContext *ctx, JSValueConst self, int64_t timeout)
{
    int r;
    if (qp_thread_rlock_is_mine(ctx, self)) {
        int level = qp_thread_slot_int(self, 1);
        if (level == INT32_MAX)
            return qp_raise_overflow_error(ctx, "Internal lock count overflowed");
        qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, level + 1));
        return qp_bool_new(1);
    }
    r = qp_thread_wait(ctx, qp_thread_rlock_is_free, self, timeout, TRUE);
    if (r < 0)
        return JS_EXCEPTION;
    if (r && qp_thread_rlock_take(ctx, self, qp_thread_ident(), 1) < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

static JSValue qp_thread_rlock_acquire(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    int64_t timeout;
    if (qp_thread_acquire_args(ctx, "acquire", args, nargs, kwnames, &timeout) < 0)
        return JS_EXCEPTION;
    return qp_thread_rlock_acquire_impl(ctx, self, timeout);
}

static JSValue qp_thread_rlock_enter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    return qp_thread_rlock_acquire_impl(ctx, self, -1);
}

static JSValue qp_thread_rlock_release(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    int level;
    if (!qp_thread_rlock_is_mine(ctx, self))
        return qp_raise_runtime_error(ctx, "cannot release un-acquired lock");
    level = qp_thread_slot_int(self, 1) - 1;
    if (level == 0)
        qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 0));
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, level));
    if (level == 0)
        qp_thread_notify(ctx);
    return JS_UNDEFINED;
}

static JSValue qp_thread_rlock_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "__exit__", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    return qp_thread_rlock_release(ctx, self, args, 0, QP_NULL, 0);
}

static JSValue qp_thread_rlock_locked(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_thread_slot_int(self, 1) != 0);
}

static JSValue qp_thread_rlock_acquire_restore(JSContext *ctx, JSValueConst self,
                                               JSValueConst *args, int nargs,
                                               JSValueConst kwnames, int magic)
{
    JSValueConst state = args[0];
    JSValue cnt, own;
    Py_ssize_t count;
    int64_t owner;
    int r;
    /* PyArg_Parse(state, "(nK):_acquire_restore", ...) */
    if (!qp_tuple_check(ctx, state)) {
        return qp_raise_type_error(ctx, "_acquire_restore() argument must be tuple, not %T", state);
    }
    if (QP_TUPLE_SIZE(state) != 2) {
        return qp_raise_type_error(ctx, "_acquire_restore() argument must be sequence of length 2, not %zd",
                                   QP_TUPLE_SIZE(state));
    }
    cnt = qp_getitem(ctx, state, JS_NewInt32(ctx, 0));
    if (JS_IsException(cnt))
        return JS_EXCEPTION;
    r = qp_arg_ssize(ctx, cnt, &count);
    JS_FreeValue(ctx, cnt);
    if (r < 0)
        return JS_EXCEPTION;
    own = qp_getitem(ctx, state, JS_NewInt32(ctx, 1));
    if (JS_IsException(own))
        return JS_EXCEPTION;
    if (!qp_long_check(ctx, own)) {
        JS_FreeValue(ctx, own);
        return qp_raise_type_error(ctx, "int is required (got type %T)", own);
    }
    r = qp_long_as_int64(ctx, own, &owner);
    JS_FreeValue(ctx, own);
    if (r < 0)
        return JS_EXCEPTION;
    if (count < 1 || count > INT32_MAX)
        return qp_raise_overflow_error(ctx, "Internal lock count overflowed");
    /* _PyRecursiveMutex_Lock: wait without timeout */
    r = qp_thread_wait(ctx, qp_thread_rlock_is_free, self, -1, FALSE);
    if (r < 0)
        return JS_EXCEPTION;
    if (qp_thread_rlock_take(ctx, self, (uint64_t)owner, (int32_t)count) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_thread_rlock_release_save(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue items[2];
    uint64_t owner;
    int count;
    if (!qp_thread_rlock_is_mine(ctx, self))
        return qp_raise_runtime_error(ctx, "cannot release un-acquired lock");
    owner = qp_thread_slot_u64(ctx, self, 0);
    count = qp_thread_slot_int(self, 1);
    items[1] = qp_long_from_uint64(ctx, owner);
    if (JS_IsException(items[1]))
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, count);
    qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 0));
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, 0));
    qp_thread_notify(ctx);
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_thread_rlock_recursion_count(JSContext *ctx, JSValueConst self,
                                               JSValueConst *args, int nargs,
                                               JSValueConst kwnames, int magic)
{
    return JS_NewInt32(ctx, qp_thread_rlock_is_mine(ctx, self) ? qp_thread_slot_int(self, 1) : 0);
}

static JSValue qp_thread_rlock_is_owned(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_thread_rlock_is_mine(ctx, self));
}

static JSValue qp_thread_rlock_at_fork_reinit(JSContext *ctx, JSValueConst self,
                                              JSValueConst *args, int nargs,
                                              JSValueConst kwnames, int magic)
{
    qp_thread_set_slot(ctx, self, 0, JS_NewInt32(ctx, 0));
    qp_thread_set_slot(ctx, self, 1, JS_NewInt32(ctx, 0));
    return JS_UNDEFINED;
}

static JSValue qp_thread_rlock_repr(JSContext *ctx, JSValueConst self)
{
    BOOL fr;
    const char *tn = qp_thread_tp_name(ctx, self, &fr);
    int level = qp_thread_slot_int(self, 1);
    char buf[360];
    if (!tn)
        return JS_EXCEPTION;
    snprintf(buf, sizeof buf, "<%s %s object owner=%llu count=%zu at %p>",
             level ? "locked" : "unlocked", tn,
             (unsigned long long)qp_thread_slot_u64(ctx, self, 0),
             (size_t)level, JS_VALUE_GET_PTR(self));
    if (fr)
        JS_FreeCString(ctx, tn);
    return qp_str_from_cstr(ctx, buf);
}

static const QPMethodDef qp_thread_rlock_methods[] = {
    { "acquire", qp_thread_rlock_acquire, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "acquire" QP_THREAD_LOCK_ACQUIRE_DOC
      "Lock the lock.\n\n"
      "`blocking` indicates whether we should wait\n"
      "for the lock to be available or not.  If `blocking` is False\n"
      "and another thread holds the lock, the method will return False\n"
      "immediately.  If `blocking` is True and another thread holds\n"
      "the lock, the method will wait for the lock to be released,\n"
      "take it and then return True.\n"
      "(note: the blocking operation is interruptible.)\n\n"
      "In all other cases, the method will return True immediately.\n"
      "Precisely, if the current thread already holds the lock, its\n"
      "internal counter is simply incremented. If nobody holds the lock,\n"
      "the lock is taken and its internal counter initialized to 1." },
    { "release", qp_thread_rlock_release, QP_METH_NOARGS, 0,
      "release($self, /)\n--\n\n"
      "Release the lock.\n\n"
      "Allows another thread that is blocked waiting for the lock\n"
      "to acquire the lock.  The lock must be in the locked state,\n"
      "and must be locked by the same thread that unlocks it; otherwise a\n"
      "`RuntimeError` is raised.\n\n"
      "Do note that if the lock was acquire()d several times in a row by\n"
      "the current thread, release() needs to be called as many times for\n"
      "the lock to be available for other threads." },
    { "locked", qp_thread_rlock_locked, QP_METH_NOARGS, 0,
      "locked($self, /)\n--\n\n"
      "Return a boolean indicating whether this object is locked right now." },
    { "_is_owned", qp_thread_rlock_is_owned, QP_METH_NOARGS, 0,
      "_is_owned($self, /)\n--\n\nFor internal use by `threading.Condition`." },
    { "_acquire_restore", qp_thread_rlock_acquire_restore, QP_METH_O, 0,
      "_acquire_restore($self, state, /)\n--\n\nFor internal use by `threading.Condition`." },
    { "_release_save", qp_thread_rlock_release_save, QP_METH_NOARGS, 0,
      "_release_save($self, /)\n--\n\nFor internal use by `threading.Condition`." },
    { "_recursion_count", qp_thread_rlock_recursion_count, QP_METH_NOARGS, 0,
      "_recursion_count($self, /)\n--\n\nFor internal use by reentrancy checks." },
    { "__enter__", qp_thread_rlock_enter, QP_METH_NOARGS, 0,
      "__enter__($self, /)\n--\n\nLock the lock." },
    { "__exit__", qp_thread_rlock_exit, QP_METH_FASTCALL, 0,
      "__exit__($self, exc_type, exc_value, exc_tb, /)\n--\n\nRelease the lock." },
    { "_at_fork_reinit", qp_thread_rlock_at_fork_reinit, QP_METH_NOARGS, 0,
      "_at_fork_reinit($self, /)\n--\n\n" },
    { NULL }
};

static const QPTypeSpec qp_thread_rlock_spec = {
    .name = "_thread.RLock",
    .doc = NULL,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_IMMUTABLETYPE |
             QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 2,
    .slots = {
        .tp_repr = qp_thread_rlock_repr,
        .tp_new = qp_thread_rlock_new,
    },
    .methods = qp_thread_rlock_methods,
};

/* ======================= _local ========================================== */
/* slots: dicts (dict: thread serial -> that thread's dict), args (tuple),
   kw (dict or QP_NULL), owner (serial of the dict installed as __dict__,
   QP_NULL: none).  The installed dict is the instance's managed __dict__,
   so the generic attribute code (without inline caches: the type has its
   own tp_getattro/tp_setattro) reads the running thread's dict once
   qp_thread_local_switch() installed it. */
enum { QP_LOCAL_DICTS, QP_LOCAL_ARGS, QP_LOCAL_KW, QP_LOCAL_OWNER, QP_LOCAL__NSLOTS };

/* make d the instance __dict__ of self, owned by the thread `serial` */
static int qp_thread_local_install(JSContext *ctx, JSValueConst self, JSValueConst d,
                                   JSValue serial)
{
    if (qp_subtype_dict_set(ctx, self, d, 0) < 0) {
        JS_FreeValue(ctx, serial);
        return -1;
    }
    qp_thread_set_slot(ctx, self, QP_LOCAL_OWNER, serial);
    return 0;
}

/* Remember that the running thread has a dict in self, so that it is
   removed when the thread exits (the main thread outlives the objects). */
static int qp_thread_local_register(JSContext *ctx, JSValueConst self)
{
    QPThreadState *ts = qp_thread_current(ctx);
    JSValue wr;
    int r;

    if (!ts || ts == ts->thr->main_ts)
        return 0;
    if (QP_IS_NULL(ts->local_refs)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return -1;
        ts->local_refs = l;
    }
    wr = qp_weakref_new(ctx, self, QP_NULL);
    if (JS_IsException(wr))
        return -1;
    r = qp_list_append(ctx, ts->local_refs, wr);
    JS_FreeValue(ctx, wr);
    return r;
}

/* _ldict(): install the running thread's dict, creating it and calling
   __init__(*args, **kw) on the first access from this thread. */
static int qp_thread_local_switch(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    uint64_t serial = qp_thread_serial(ctx);
    JSValue key, d, init, res, args, kw;
    int r;

    if (!QP_IS_NULL(slots[QP_LOCAL_OWNER]) &&
        qp_thread_slot_u64(ctx, self, QP_LOCAL_OWNER) == serial)
        return 0;
    if (QP_IS_NULL(slots[QP_LOCAL_DICTS])) {
        d = qp_dict_new(ctx);
        if (JS_IsException(d))
            return -1;
        qp_thread_set_slot(ctx, self, QP_LOCAL_DICTS, d);
        slots = qp_instance_slots(self);
    }
    if (QP_IS_NULL(slots[QP_LOCAL_ARGS])) {
        JSValue t = qp_tuple_new(ctx, 0);
        if (JS_IsException(t))
            return -1;
        qp_thread_set_slot(ctx, self, QP_LOCAL_ARGS, t);
        slots = qp_instance_slots(self);
    }
    key = qp_long_from_uint64(ctx, serial);
    if (JS_IsException(key))
        return -1;
    r = qp_dict_get_item(ctx, slots[QP_LOCAL_DICTS], key, &d);
    if (r < 0) {
        JS_FreeValue(ctx, key);
        return -1;
    }
    if (r > 0) {
        r = qp_thread_local_install(ctx, self, d, key);
        JS_FreeValue(ctx, d);
        return r;
    }
    /* first access from this thread (_local_create_dummy) */
    d = qp_dict_new(ctx);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, key);
        return -1;
    }
    if (qp_dict_set_item(ctx, slots[QP_LOCAL_DICTS], key, d) < 0 ||
        qp_thread_local_register(ctx, self) < 0 ||
        qp_thread_local_install(ctx, self, d, JS_DupValue(ctx, key)) < 0) {
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, key);
        return -1;
    }
    JS_FreeValue(ctx, d);
    if (qp_type_of(ctx, self)->slots.tp_init != QP_TYPE(ctx, object)->slots.tp_init) {
        init = qp_getattr(ctx, self, QP_ATOM(ctx, __init__));
        if (JS_IsException(init)) {
            res = JS_EXCEPTION;
        } else {
            slots = qp_instance_slots(self);
            args = JS_DupValue(ctx, slots[QP_LOCAL_ARGS]);
            kw = QP_IS_NULL(slots[QP_LOCAL_KW]) ? QP_NULL : JS_DupValue(ctx, slots[QP_LOCAL_KW]);
            res = qp_call_tuple(ctx, init, args, kw);
            JS_FreeValue(ctx, init);
            JS_FreeValue(ctx, args);
            if (!QP_IS_NULL(kw))
                JS_FreeValue(ctx, kw);
        }
        if (JS_IsException(res)) {
            /* forget the dict: the next access creates a new one */
            JSValue exc = qp_exc_fetch(ctx);
            slots = qp_instance_slots(self);
            if (qp_dict_del_item(ctx, slots[QP_LOCAL_DICTS], key) < 0)
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
            qp_thread_set_slot(ctx, self, QP_LOCAL_OWNER, QP_NULL);
            JS_FreeValue(ctx, key);
            qp_raise_object(ctx, exc);
            return -1;
        }
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, key);
    return 0;
}

/* The thread ts exits: drop its dicts from the _local objects it used. */
static void qp_thread_local_release(JSContext *ctx, QPThreadState *ts)
{
    JSValue refs = ts->local_refs, key;
    Py_ssize_t i, n;

    ts->local_refs = QP_NULL;
    if (QP_IS_NULL(refs))
        return;
    key = qp_long_from_uint64(ctx, ts->serial);
    if (JS_IsException(key)) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        JS_FreeValue(ctx, refs);
        return;
    }
    for (i = 0;; i++) {
        JSValue *items = qp_list_items(refs, &n), obj, *slots;
        if (!items || i >= n)
            break;
        obj = qp_weakref_get(ctx, items[i]);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            continue;
        }
        if (JS_IsUndefined(obj))
            continue;
        slots = qp_instance_slots(obj);
        if (slots && !QP_IS_NULL(slots[QP_LOCAL_DICTS])) {
            if (qp_dict_del_item(ctx, slots[QP_LOCAL_DICTS], key) < 0)
                qp_format_unraisable(ctx, "Exception ignored while clearing thread local %R", obj);
            slots = qp_instance_slots(obj);
            if (!QP_IS_NULL(slots[QP_LOCAL_OWNER]) &&
                qp_thread_slot_u64(ctx, obj, QP_LOCAL_OWNER) == ts->serial) {
                /* uninstall the dict of the exiting thread */
                JSValue empty = qp_dict_new(ctx);
                if (JS_IsException(empty) || qp_subtype_dict_set(ctx, obj, empty, 0) < 0)
                    JS_FreeValue(ctx, qp_exc_fetch(ctx));
                if (!JS_IsException(empty))
                    JS_FreeValue(ctx, empty);
                qp_thread_set_slot(ctx, obj, QP_LOCAL_OWNER, QP_NULL);
            }
        }
        JS_FreeValue(ctx, obj);
    }
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, refs);
}

static JSValue qp_thread_local_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)(nargsf & ~QP_VECTORCALL_ARGUMENTS_OFFSET);
    int nkw = (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames)) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    JSValue obj, v, d;
    int i;

    if (subtype->slots.tp_init == QP_TYPE(ctx, object)->slots.tp_init && (nargs > 0 || nkw > 0))
        return qp_raise_type_error(ctx, "Initialization arguments are not supported");
    obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    v = qp_tuple_new(ctx, nargs);
    if (JS_IsException(v))
        goto fail;
    for (i = 0; i < nargs; i++) {
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(v)[i]);
        QP_TUPLE_ITEMS(v)[i] = JS_DupValue(ctx, args[i]);
    }
    qp_thread_set_slot(ctx, obj, QP_LOCAL_ARGS, v);
    if (nkw > 0) {
        v = qp_dict_new(ctx);
        if (JS_IsException(v))
            goto fail;
        qp_thread_set_slot(ctx, obj, QP_LOCAL_KW, v);
        for (i = 0; i < nkw; i++) {
            if (qp_dict_set_item(ctx, v, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0)
                goto fail;
        }
    }
    v = qp_dict_new(ctx);
    if (JS_IsException(v))
        goto fail;
    qp_thread_set_slot(ctx, obj, QP_LOCAL_DICTS, v);
    /* the dict of the creating thread; __init__ runs through the call */
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        goto fail;
    {
        JSValue key = qp_long_from_uint64(ctx, qp_thread_serial(ctx));
        if (JS_IsException(key) ||
            qp_dict_set_item(ctx, v, key, d) < 0 ||
            qp_thread_local_register(ctx, obj) < 0 ||
            qp_thread_local_install(ctx, obj, d, JS_DupValue(ctx, key)) < 0) {
            if (!JS_IsException(key))
                JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, d);
            goto fail;
        }
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, d);
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue qp_thread_local_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    if (qp_thread_local_switch(ctx, obj) < 0)
        return JS_EXCEPTION;
    return qp_generic_getattr(ctx, obj, name);
}

static int qp_thread_local_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    if (name == QP_ATOM(ctx, __dict__)) {
        BOOL fr;
        const char *tn = qp_thread_tp_name(ctx, obj, &fr);
        if (!tn)
            return -1;
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "'%.100s' object attribute '__dict__' is read-only", tn);
        if (fr)
            JS_FreeCString(ctx, tn);
        return -1;
    }
    if (qp_thread_local_switch(ctx, obj) < 0)
        return -1;
    return qp_generic_setattr(ctx, obj, name, v);
}

static JSValue qp_thread_local_get_dict(JSContext *ctx, JSValueConst self, int magic)
{
    if (qp_thread_local_switch(ctx, self) < 0)
        return JS_EXCEPTION;
    return qp_object_get_dict(ctx, self);
}

static const QPGetSetDef qp_thread_local_getsets[] = {
    { "__dict__", qp_thread_local_get_dict, NULL, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_thread_local_spec = {
    .name = "_thread._local",
    .doc = "_local()\n--\n\nThread-local data",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MANAGED_DICT |
             QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_LOCAL__NSLOTS,
    .slots = {
        .tp_getattro = qp_thread_local_getattro,
        .tp_setattro = qp_thread_local_setattro,
        .tp_new = qp_thread_local_new,
    },
    .getsets = qp_thread_local_getsets,
};

/* ======================= _ExceptHookArgs ================================= */

static const char *const qp_thread_excepthook_fields[] = {
    "exc_type", "exc_value", "exc_traceback", "thread", NULL
};
static const char *const qp_thread_excepthook_docs[] = {
    "Exception type", "Exception value", "Exception traceback", "Thread", NULL
};
static const QPStructSeqDesc qp_thread_excepthook_desc = {
    "_thread._ExceptHookArgs",
    "ExceptHookArgs\n\nType used to pass arguments to threading.excepthook.",
    qp_thread_excepthook_fields,
    qp_thread_excepthook_docs,
    4,
    0,
};

/* ======================= module functions ================================ */

static BOOL qp_thread_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

static JSValue qp_thread_daemon_threads_allowed(JSContext *ctx, JSValueConst self,
                                                JSValueConst *args, int nargs,
                                                JSValueConst kwnames, int magic)
{
    return qp_bool_new(1);
}

/* ======================= threads ========================================= */

/* add_to_shutdown_handles */
static int qp_thread_add_shutdown_handle(JSContext *ctx, JSValueConst handle)
{
    QPModState *ms = qp_modstate(ctx);
    if (!ms)
        return -1;
    if (QP_IS_NULL(ms->thread_shutdown_handles)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return -1;
        ms->thread_shutdown_handles = l;
    }
    return qp_list_append(ctx, ms->thread_shutdown_handles, handle);
}

/* remove_from_shutdown_handles (no exception) */
static void qp_thread_remove_shutdown_handle(JSContext *ctx, JSValueConst handle)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    JSValue *items;
    Py_ssize_t n, i;

    if (!ms || QP_IS_NULL(ms->thread_shutdown_handles))
        return;
    items = qp_list_items(ms->thread_shutdown_handles, &n);
    for (i = 0; items && i < n; i++) {
        if (JS_VALUE_GET_PTR(items[i]) == JS_VALUE_GET_PTR(handle)) {
            JSValue idx = qp_long_from_int64(ctx, i);
            if (JS_IsException(idx) || qp_delitem(ctx, ms->thread_shutdown_handles, idx) < 0)
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
            JS_FreeValue(ctx, idx);
            return;
        }
    }
}

/* the handle of a thread that is done (or could not start): done, waiters
   woken, not waited for at shutdown */
static void qp_thread_handle_done(JSContext *ctx, JSValueConst handle)
{
    qp_thread_remove_shutdown_handle(ctx, handle);
    qp_thread_set_slot(ctx, handle, 1, JS_NewInt32(ctx, QP_TH_DONE));
    qp_thread_notify(ctx);
}

/* PyErr_FormatUnraisable("Exception ignored in thread started by %R", func) */
static void qp_thread_unraisable(JSContext *ctx, JSValueConst exc, JSValueConst func)
{
    JSValue r = qp_repr(ctx, func);
    const char *s = JS_IsException(r) ? NULL : qp_str_utf8(ctx, r, NULL);
    char *msg = NULL;
    if (s) {
        size_t len = strlen(s) + 64;
        msg = js_malloc(ctx, len);
        if (msg)
            snprintf(msg, len, "Exception ignored in thread started by %s", s);
        JS_FreeCString(ctx, s);
    }
    if (!msg)
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    qp_exc_write_unraisable_msg(ctx, exc, msg ? msg : "Exception ignored in thread started by",
                                QP_NULL);
    js_free(ctx, msg);
    if (!JS_IsException(r))
        JS_FreeValue(ctx, r);
}

/* Release what the exiting thread ts (the running thread) holds:
   PyThreadState_Clear. */
static void qp_thread_tstate_clear(JSContext *ctx, QPThreadState *ts)
{
    JSRuntime *rt = ctx->rt;
    QPState *s = QP_STATE(ctx);
    QPModState *ms = s->mod_state;
    JSValue v;

    qp_thread_local_release(ctx, ts);
#define QP_THREAD_CLEAR(field, none) \
    v = field; field = none; if (!QP_IS_NULL(v)) JS_FreeValue(ctx, v);
    QP_THREAD_CLEAR(ts->func, QP_NULL)
    QP_THREAD_CLEAR(ts->args, QP_NULL)
    QP_THREAD_CLEAR(ts->kwargs, QP_NULL)
    QP_THREAD_CLEAR(s->repr_stack, JS_UNDEFINED)
    QP_THREAD_CLEAR(s->asyncgen_firstiter, JS_UNDEFINED)
    QP_THREAD_CLEAR(s->asyncgen_finalizer, JS_UNDEFINED)
    QP_THREAD_CLEAR(s->exc_info_base.exc_value, JS_UNDEFINED)
    QP_THREAD_CLEAR(s->trace_func, JS_UNDEFINED)
    QP_THREAD_CLEAR(s->profile_func, JS_UNDEFINED)
    if (s->trace_active) {
        s->trace_active = 0;
        qp_trace_dispatch(0);
    }
    s->tracing = 0;
    if (ms) {
        QP_THREAD_CLEAR(ms->ctx_current, QP_NULL)
        QP_THREAD_CLEAR(ms->async_exc, QP_NULL)
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
#undef QP_THREAD_CLEAR
    s->exc_info = &s->exc_info_base;
    s->recursion_depth = 0;
    s->reraise = 0;
    rt->current_exception = JS_UNINITIALIZED;
}

/* thread_run(): body of a thread started by _thread */
static void *qp_thread_entry(void *arg)
{
    QPThreadState *ts = arg;
    QPThreads *thr = ts->thr;
    JSContext *ctx = thr->ctx;
    JSRuntime *rt = thr->rt;
    JSValue res, handle;
    int i;

    qp_gil_take(thr, ts);
    qp_gil_switch_in(thr, ts);
    JS_UpdateStackTop(rt);
    if (qp_tstate_capi_hook)
        qp_tstate_capi_hook(ts, QP_TSTATE_START);
    thr->count++;

    res = qp_call_tuple(ctx, ts->func, ts->args, ts->kwargs);
    if (JS_IsException(res)) {
        JSValue exc = qp_exc_fetch(ctx);
        /* SystemExit is ignored silently */
        if (!qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, SystemExit))))
            qp_thread_unraisable(ctx, exc, ts->func);
        JS_FreeValue(ctx, exc);
    } else {
        JS_FreeValue(ctx, res);
    }
    thr->count--;
    qp_thread_tstate_clear(ctx, ts);
    handle = ts->handle;
    ts->handle = QP_NULL;
    if (!QP_IS_NULL(handle)) {
        qp_thread_handle_done(ctx, handle);
        JS_FreeValue(ctx, handle);
    }
    /* recursive mutexes left locked by the thread stay locked (CPython);
       their owner becomes the main thread (no dangling owner) */
    for (i = 0; i < QP_RMUTEX__COUNT; i++) {
        if (thr->rmutex_owner[i] == ts)
            thr->rmutex_owner[i] = NULL;
    }
    if (qp_tstate_capi_hook)
        qp_tstate_capi_hook(ts, QP_TSTATE_EXIT);
    list_del(&ts->link);
    thr->current = NULL;
    qp_tstate_tls = NULL;
    js_free_rt(rt, ts);
    qp_gil_drop(thr, NULL);
    return NULL;
}

/* ThreadHandle_start + do_start_new_thread: start func(*args, **kwargs)
   (kwargs QP_NULL: none) in a new OS thread; handle QP_NULL or a
   not-started _ThreadHandle. */
static int qp_thread_start(JSContext *ctx, JSValueConst func, JSValueConst args,
                           JSValueConst kwargs, JSValueConst handle, int daemon)
{
    QPThreads *thr;
    QPThreadState *ts;
    pthread_attr_t attr;
    pthread_t tid;
    size_t stack_size;
    uintptr_t js_stack;
    int rc;

    /* interp->finalizing: qp_threads_finalize() marks the threads, the
       finalization of the modules the state (when no thread was started
       before, there are no QPThreads to mark) */
    thr = NULL;
    if (!QP_STATE(ctx)->finalizing) {
        thr = qp_threads_get(ctx);
        if (!thr)
            return -1;
    }
    if (!thr || thr->finalizing) {
        qp_raise(ctx, QP_EXC(ctx, PythonFinalizationError),
                 "can't create new thread at interpreter shutdown");
        return -1;
    }
    /* the native stack of the thread, and the part the runtime may use
       (the rest is the margin of the stack overflow checks) */
    stack_size = qp_thread_stacksize ? qp_thread_stacksize : QP_THREAD_DEFAULT_STACK;
    js_stack = stack_size > 2 * 1024 * 1024 ? stack_size - 512 * 1024 : stack_size * 3 / 4;
    if (ctx->rt->stack_size == 0)
        js_stack = 0;
    ts = qp_tstate_new(thr, js_stack);
    if (!ts) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    if (!QP_IS_NULL(handle)) {
        if (!daemon && qp_thread_add_shutdown_handle(ctx, handle) < 0) {
            js_free_rt(ctx->rt, ts);
            return -1;
        }
        qp_thread_set_slot(ctx, handle, 1, JS_NewInt32(ctx, QP_TH_STARTING));
        ts->handle = JS_DupValue(ctx, handle);
    }
    ts->func = JS_DupValue(ctx, func);
    ts->args = JS_DupValue(ctx, args);
    ts->kwargs = QP_IS_NULL(kwargs) ? QP_NULL : JS_DupValue(ctx, kwargs);

    rc = pthread_attr_init(&attr);
    if (rc == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_attr_setstacksize(&attr, stack_size);
        if (rc == 0)
            rc = pthread_create(&tid, &attr, qp_thread_entry, ts);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        JS_FreeValue(ctx, ts->func);
        JS_FreeValue(ctx, ts->args);
        if (!QP_IS_NULL(ts->kwargs))
            JS_FreeValue(ctx, ts->kwargs);
        if (!QP_IS_NULL(ts->handle)) {
            /* force_done */
            qp_thread_handle_done(ctx, ts->handle);
            JS_FreeValue(ctx, ts->handle);
        }
        js_free_rt(ctx->rt, ts);
        qp_raise_runtime_error(ctx, "can't start new thread");
        return -1;
    }
    /* the thread waits for the GIL: publish its state first */
    ts->ident = (uint64_t)(uintptr_t)tid;
    list_add_tail(&ts->link, &thr->tstates);
    if (!QP_IS_NULL(handle)) {
        JSValue id = qp_long_from_uint64(ctx, ts->ident);
        if (JS_IsException(id)) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            id = JS_NewInt32(ctx, 0);
        }
        qp_thread_set_slot(ctx, handle, 0, id);
        qp_thread_set_slot(ctx, handle, 1, JS_NewInt32(ctx, QP_TH_RUNNING));
    }
    return 0;
}

/* PyOS_AfterFork_Child, thread part (_PyEval_ReInitThreads,
   _PyInterpreterState_DeleteExceptMain, _PyThread_AfterFork): the forking
   thread is the only thread of the child; it becomes the main thread and
   holds a fresh GIL.  The other thread states are released and their
   handles marked done; the objects referenced from their stack frames are
   leaked (their native stacks do not exist in the child). */
static void qp_thread_after_fork_child(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *cur, *ts;
    struct list_head *el, *el1;
    int i;

    /* the handles of _make_thread_handle(), see qp_thread_handle_is_exiting */
    qp_thread_fork_gen++;
    qp_thread_fork_ident = qp_thread_ident();
    if (!thr || thr->ctx != ctx) {
        if (qp_tstate_capi_hook)
            qp_tstate_capi_hook(NULL, QP_TSTATE_FORK_CHILD);
        return;
    }
    cur = thr->current;
    /* the old mutexes may be held by threads which do not exist here */
    qp_thread_sync_init(thr);
    thr->locked = 1;
    thr->last_holder = cur;
    thr->switch_number = 0;
    thr->drop_request = 0;
    thr->wait_gen = 0;
    thr->nwaiters = 0;
    thr->count = 0;
    for (i = 0; i < QP_RMUTEX__COUNT; i++) {
        int *plevel = qp_rmutex_level_ptr(ctx, i);
        if (!thr->rmutex_owner[i] && plevel && *plevel > 0)
            thr->rmutex_owner[i] = thr->main_ts;    /* explicit owner */
    }
    thr->main_ts = cur;
    cur->ident = qp_thread_self_ident();
    list_for_each_safe(el, el1, &thr->tstates) {
        ts = list_entry(el, QPThreadState, link);
        if (ts == cur)
            continue;
        list_del(&ts->link);
        for (i = 0; i < QP_RMUTEX__COUNT; i++) {
            if (thr->rmutex_owner[i] == ts) {
                /* held by a thread that does not exist: unlock */
                int *plevel = qp_rmutex_level_ptr(ctx, i);
                thr->rmutex_owner[i] = NULL;
                if (plevel)
                    *plevel = 0;
            }
        }
        qp_thread_local_release(ctx, ts);
#define QP_THREAD_FREE(v) if (!QP_IS_NULL(v)) JS_FreeValue(ctx, v)
        QP_THREAD_FREE(ts->func);
        QP_THREAD_FREE(ts->args);
        QP_THREAD_FREE(ts->kwargs);
        QP_THREAD_FREE(ts->repr_stack);
        QP_THREAD_FREE(ts->asyncgen_firstiter);
        QP_THREAD_FREE(ts->asyncgen_finalizer);
        QP_THREAD_FREE(ts->exc_info_base.exc_value);
        QP_THREAD_FREE(ts->context);
        QP_THREAD_FREE(ts->trace_func);
        QP_THREAD_FREE(ts->profile_func);
        QP_THREAD_FREE(ts->async_exc);
        if (ts->trace_active)
            qp_trace_dispatch(0);
        QP_THREAD_FREE(ts->js.current_exception);
        QP_THREAD_FREE(ts->js.resume_value);
#undef QP_THREAD_FREE
        if (!QP_IS_NULL(ts->handle)) {
            qp_thread_handle_done(ctx, ts->handle);
            JS_FreeValue(ctx, ts->handle);
        }
        if (qp_tstate_capi_hook)
            qp_tstate_capi_hook(ts, QP_TSTATE_EXIT);
        if (ts->foreign)
            qp_sys_free(ts);
        else
            js_free_rt(ctx->rt, ts);
    }
    if (qp_tstate_capi_hook)
        qp_tstate_capi_hook(cur, QP_TSTATE_FORK_CHILD);
}

static JSValue qp_thread_start_new_thread(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    const char *fname = "start_new_thread";
    JSValue handle, ident;
    QPType *ht = qp_thread_type(ctx, QP_THREAD_T_HANDLE);
    if (qp_arg_check_positional(ctx, fname, nargs, 2, 3) < 0)
        return JS_EXCEPTION;
    if (!qp_thread_callable(ctx, args[0]))
        return qp_raise_type_error(ctx, "first arg must be callable");
    if (!qp_tuple_check(ctx, args[1]))
        return qp_raise_type_error(ctx, "2nd arg must be a tuple");
    if (nargs > 2 && !qp_dict_check(ctx, args[2]))
        return qp_raise_type_error(ctx, "optional 3rd arg must be a dictionary");
    if (!ht)
        return JS_EXCEPTION;
    handle = qp_thread_handle_alloc(ctx, ht);
    if (JS_IsException(handle))
        return handle;
    if (qp_thread_start(ctx, args[0], args[1], nargs > 2 ? args[2] : QP_NULL, handle, 1) < 0) {
        JS_FreeValue(ctx, handle);
        return JS_EXCEPTION;
    }
    ident = qp_long_from_uint64(ctx, qp_thread_slot_u64(ctx, handle, 0));
    JS_FreeValue(ctx, handle);
    return ident;
}

static JSValue qp_thread_start_joinable_thread(JSContext *ctx, JSValueConst self,
                                               JSValueConst *args, int nargs,
                                               JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "function", "handle", "daemon", NULL };
    QPArgSpec spec = { "start_joinable_thread", kw, 1, 3, 0 };
    JSValueConst a[3];
    JSValue handle, empty;
    int daemon = 1, r;
    QPType *ht = qp_thread_type(ctx, QP_THREAD_T_HANDLE);
    if (!ht)
        return JS_EXCEPTION;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2]) && qp_arg_bool(ctx, a[2], &daemon) < 0)
        return JS_EXCEPTION;
    if (!qp_thread_callable(ctx, a[0]))
        return qp_raise_type_error(ctx, "thread function must be callable");
    if (!QP_IS_NULL(a[1]) && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        if (qp_type_of(ctx, a[1]) != ht)
            return qp_raise_type_error(ctx, "'handle' must be a _ThreadHandle");
        if (qp_thread_slot_int(a[1], 1) != QP_TH_NOT_STARTED)
            return qp_raise_runtime_error(ctx, "thread already started");
        handle = JS_DupValue(ctx, a[1]);
    } else {
        handle = qp_thread_handle_alloc(ctx, ht);
        if (JS_IsException(handle))
            return handle;
    }
    empty = qp_tuple_new(ctx, 0);
    if (JS_IsException(empty)) {
        JS_FreeValue(ctx, handle);
        return empty;
    }
    r = qp_thread_start(ctx, a[0], empty, QP_NULL, handle, daemon);
    JS_FreeValue(ctx, empty);
    if (r < 0) {
        JS_FreeValue(ctx, handle);
        return JS_EXCEPTION;
    }
    return handle;
}

static JSValue qp_thread_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    JSValue exc = qp_exc_new(ctx, QP_EXC(ctx, SystemExit), 0, NULL);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

static JSValue qp_thread_interrupt_main(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", NULL };
    QPArgSpec spec = { "signum", kw, 0, 1, 0 };
    JSValueConst a[1];
    int signum = SIGINT;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_int(ctx, a[0], &signum) < 0)
        return JS_EXCEPTION;
    if (qp_signal_trip(ctx, signum) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_thread_allocate_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_thread_type(ctx, QP_THREAD_T_LOCK);
    if (!t)
        return JS_EXCEPTION;
    return qp_thread_lock_alloc(ctx, t);
}

static JSValue qp_thread_get_ident(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_uint64(ctx, qp_thread_ident());
}

#if defined(__APPLE__) || defined(__linux__)
#define QP_THREAD_HAVE_NATIVE_ID 1
/* PyThread_get_thread_native_id */
static JSValue qp_thread_get_native_id(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
#if defined(__APPLE__)
    uint64_t native_id;
    (void)pthread_threadid_np(NULL, &native_id);
#else
    unsigned long native_id = (unsigned long)syscall(SYS_gettid);
#endif
    return qp_long_from_uint64(ctx, (uint64_t)(unsigned long)native_id);
}
#endif

static JSValue qp_thread_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPThreads *thr = ctx->rt->py_threads;
    return JS_NewInt32(ctx, thr ? thr->count : 0);
}

static JSValue qp_thread_stack_size(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", NULL };
    QPArgSpec spec = { "stack_size", kw, 0, 1, 0 };
    JSValueConst a[1];
    Py_ssize_t new_size = 0;
    size_t old_size;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[0]) && qp_arg_ssize(ctx, a[0], &new_size) < 0)
        return JS_EXCEPTION;
    if (new_size != 0 && new_size < QP_THREAD_MIN_STACK)
        return qp_raise_value_error(ctx, "size must be at least %zi bytes", QP_THREAD_MIN_STACK);
    old_size = qp_thread_stacksize;
    /* _pythread_pthread_set_stacksize */
    if (new_size == 0) {
        qp_thread_stacksize = 0;
    } else {
        int ok = 0;
        if (new_size >= QP_THREAD_STACK_MIN) {
            pthread_attr_t attrs;
            if (pthread_attr_init(&attrs) == 0) {
                ok = pthread_attr_setstacksize(&attrs, (size_t)new_size) == 0;
                pthread_attr_destroy(&attrs);
            }
        }
        if (!ok)
            return qp_raise_value_error(ctx, "size not valid: %zd bytes", new_size);
        qp_thread_stacksize = (size_t)new_size;
    }
    return qp_long_from_int64(ctx, (int64_t)old_size);
}

/* file.write(s); consumes s */
static int qp_thread_file_write(JSContext *ctx, JSValueConst file, JSValue s)
{
    JSValue w, r;
    if (JS_IsException(s))
        return -1;
    w = qp_getattr(ctx, file, QP_ATOM(ctx, write));
    if (JS_IsException(w)) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    r = qp_call1(ctx, w, s);
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, s);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* thread_excepthook_file */
static int qp_thread_excepthook_file(JSContext *ctx, JSValueConst file, JSValueConst exc_value,
                                     JSValueConst thread)
{
    JSValue name = QP_NULL, r;
    if (qp_thread_file_write(ctx, file, qp_str_from_cstr(ctx, "Exception in thread ")) < 0)
        return -1;
    if (!JS_IsUndefined(thread) && !JS_IsNull(thread)) {
        if (qp_getattr_opt(ctx, thread, QP_ATOM(ctx, name), &name) < 0)
            return -1;
    }
    if (!QP_IS_NULL(name)) {
        JSValue s = qp_str(ctx, name);
        JS_FreeValue(ctx, name);
        if (qp_thread_file_write(ctx, file, s) < 0)
            return -1;
    } else {
        char buf[32];
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)qp_thread_ident());
        if (qp_thread_file_write(ctx, file, qp_str_from_cstr(ctx, buf)) < 0)
            return -1;
    }
    if (qp_thread_file_write(ctx, file, qp_str_from_cstr(ctx, ":\n")) < 0)
        return -1;
    /* _PyErr_Display */
    if (qp_exc_display(ctx, exc_value, file) < 0)
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    /* _PyFile_Flush */
    r = qp_getattr(ctx, file, QP_ATOM(ctx, flush));
    if (JS_IsException(r))
        return -1;
    {
        JSValue res = qp_call0(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(res))
            return -1;
        JS_FreeValue(ctx, res);
    }
    return 0;
}

static JSValue qp_thread_excepthook(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst hargs = args[0];
    JSValue items[4], file;
    QPType *et = qp_thread_type(ctx, QP_THREAD_T_EXCEPTHOOK);
    int i, res = 0;
    if (!et)
        return JS_EXCEPTION;
    if (qp_type_of(ctx, hargs) != et)
        return qp_raise_type_error(ctx, "_thread.excepthook argument type must be ExceptHookArgs");
    for (i = 0; i < 4; i++) {
        items[i] = qp_getitem(ctx, hargs, JS_NewInt32(ctx, i));
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    if (qp_is(items[0], QP_TYPE_VALUE(QP_EXC(ctx, SystemExit))))
        goto done;      /* silently ignore SystemExit */
    file = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
    if (QP_IS_NULL(file) || JS_IsUndefined(file) || JS_IsNull(file)) {
        JSAtom at;
        if (!QP_IS_NULL(file))
            JS_FreeValue(ctx, file);
        if (JS_IsUndefined(items[3]) || JS_IsNull(items[3]))
            goto done;
        at = JS_NewAtom(ctx, "_stderr");
        if (at == JS_ATOM_NULL) {
            res = -1;
            goto done;
        }
        file = qp_getattr(ctx, items[3], at);
        JS_FreeAtom(ctx, at);
        if (JS_IsException(file)) {
            res = -1;
            goto done;
        }
        if (JS_IsUndefined(file) || JS_IsNull(file))
            goto done;
    }
    res = qp_thread_excepthook_file(ctx, file, items[1], items[3]);
    JS_FreeValue(ctx, file);
done:
    for (i = 0; i < 4; i++)
        JS_FreeValue(ctx, items[i]);
    return res < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue qp_thread_is_main_interpreter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                             int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(1);
}

/* thread_shutdown: join every registered non-daemon handle of another
   thread (a thread removes its handle when it exits). */
static JSValue qp_thread_shutdown(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_modstate(ctx);
    uint64_t ident = qp_thread_ident();
    if (!ms)
        return JS_EXCEPTION;
    for (;;) {
        JSValue handle = QP_NULL, *items;
        Py_ssize_t n, i;
        if (QP_IS_NULL(ms->thread_shutdown_handles))
            break;
        items = qp_list_items(ms->thread_shutdown_handles, &n);
        for (i = 0; i < n; i++) {
            if (qp_thread_slot_u64(ctx, items[i], 0) != ident) {
                handle = JS_DupValue(ctx, items[i]);
                break;
            }
        }
        if (QP_IS_NULL(handle))
            break;
        if (qp_thread_handle_join_impl(ctx, handle, -1) < 0) {
            /* interrupted (Ctrl-C): report and stop waiting */
            JSValue exc = qp_exc_fetch(ctx);
            qp_exc_write_unraisable_msg(ctx, exc, "Exception ignored while joining a thread "
                                        "in _thread._shutdown()", QP_NULL);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, handle);
            return JS_UNDEFINED;
        }
        /* remove_from_shutdown_handles (done by the exiting thread in CPython) */
        items = qp_list_items(ms->thread_shutdown_handles, &n);
        for (i = 0; i < n; i++) {
            if (JS_VALUE_GET_PTR(items[i]) == JS_VALUE_GET_PTR(handle)) {
                JSValue idx = qp_long_from_int64(ctx, i);
                int r = qp_delitem(ctx, ms->thread_shutdown_handles, idx);
                JS_FreeValue(ctx, idx);
                if (r < 0) {
                    JS_FreeValue(ctx, handle);
                    return JS_EXCEPTION;
                }
                break;
            }
        }
        JS_FreeValue(ctx, handle);
    }
    return JS_UNDEFINED;
}

static JSValue qp_thread_make_thread_handle(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                            int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst identobj = args[0];
    JSValue h, idv;
    QPType *ht = qp_thread_type(ctx, QP_THREAD_T_HANDLE);
    if (!ht)
        return JS_EXCEPTION;
    if (!qp_long_check(ctx, identobj))
        return qp_raise_type_error(ctx, "ident must be an integer");
    idv = qp_long_value(ctx, identobj);
    if (JS_IsException(idv))
        return idv;
    if (qp_long_sign(idv) < 0) {
        JS_FreeValue(ctx, idv);
        return qp_raise_overflow_error(ctx, "can't convert negative int to unsigned");
    }
    h = qp_thread_handle_alloc(ctx, ht);
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, idv);
        return h;
    }
    qp_thread_set_slot(ctx, h, 0, idv);
    qp_thread_set_slot(ctx, h, 1, JS_NewInt32(ctx, QP_TH_RUNNING));
    qp_thread_set_slot(ctx, h, 2, JS_NewInt32(ctx, (int32_t)qp_thread_fork_gen));
    return h;
}

static JSValue qp_thread_get_main_thread_ident(JSContext *ctx, JSValueConst self,
                                               JSValueConst *args, int nargs,
                                               JSValueConst kwnames, int magic)
{
    return qp_long_from_uint64(ctx, qp_thread_main_ident(ctx));
}

#ifdef QP_THREAD_NAME_MAXLEN
static JSValue qp_thread_get_name(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    char name[100];
    int rc = pthread_getname_np(pthread_self(), name, sizeof name);
    if (rc)
        return qp_raise_os_error(ctx, rc, QP_NULL);
    return qp_str_decode(ctx, name, (Py_ssize_t)strlen(name), "utf-8", "surrogateescape");
}

static JSValue qp_thread_set_name(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "name", NULL };
    QPArgSpec spec = { "set_name", kw, 1, 1, 0 };
    JSValueConst a[1];
    JSValue enc;
    const uint8_t *data;
    Py_ssize_t len;
    char buf[QP_THREAD_NAME_MAXLEN + 1];
    int rc;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_raise_type_error(ctx, "set_name() argument 'name' must be str, not %T", a[0]);
    /* filesystem encoding, "replace" error handler */
    enc = qp_str_encode(ctx, a[0], "utf-8", "replace");
    if (JS_IsException(enc))
        return enc;
    data = qp_bytes_data(enc, &len);
    if (len > QP_THREAD_NAME_MAXLEN)
        len = QP_THREAD_NAME_MAXLEN;
    memcpy(buf, data, (size_t)len);
    buf[len] = '\0';
    JS_FreeValue(ctx, enc);
#ifdef __APPLE__
    rc = pthread_setname_np(buf);
#else
    rc = pthread_setname_np(pthread_self(), buf);
#endif
    if (rc)
        return qp_raise_os_error(ctx, rc, QP_NULL);
    return JS_UNDEFINED;
}
#endif

static const QPMethodDef qp_thread_methods[] = {
    { "start_new_thread", qp_thread_start_new_thread, QP_METH_FASTCALL, 0,
      "start_new_thread($module, function, args, kwargs={}, /)\n--\n\n"
      "Start a new thread and return its identifier.\n\n"
      "The thread will call the function with positional arguments from the\n"
      "tuple args and keyword arguments taken from the optional dictionary\n"
      "kwargs.  The thread exits when the function returns; the return value\n"
      "is ignored.  The thread will also exit when the function raises an\n"
      "unhandled exception; a stack trace will be printed unless the exception\n"
      "is SystemExit." },
    { "start_new", qp_thread_start_new_thread, QP_METH_FASTCALL, 0,
      "start_new($module, function, args, kwargs={}, /)\n--\n\n"
      "An obsolete synonym of start_new_thread()." },
    { "start_joinable_thread", qp_thread_start_joinable_thread, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "start_joinable_thread($module, /, function, handle=None, daemon=True)\n--\n\n"
      "*For internal use only*: start a new thread.\n\n"
      "Like start_new_thread(), this starts a new thread calling the given function.\n"
      "Unlike start_new_thread(), this returns a handle object with methods to join\n"
      "or detach the given thread.\n"
      "This function is not for third-party code, please use the\n"
      "`threading` module instead. During finalization the runtime will not wait for\n"
      "the thread to exit if daemon is True. If handle is provided it must be a\n"
      "newly created thread._ThreadHandle instance." },
    { "daemon_threads_allowed", qp_thread_daemon_threads_allowed, QP_METH_NOARGS, 0,
      "daemon_threads_allowed($module, /)\n--\n\n"
      "Return True if daemon threads are allowed in the current interpreter,\n"
      "and False otherwise.\n" },
    { "allocate_lock", qp_thread_allocate_lock, QP_METH_NOARGS, 0,
      "allocate_lock($module, /)\n--\n\n"
      "Create a new lock object. See help(type(threading.Lock())) for\n"
      "information about locks." },
    { "allocate", qp_thread_allocate_lock, QP_METH_NOARGS, 0,
      "allocate($module, /)\n--\n\nAn obsolete synonym of allocate_lock()." },
    { "exit_thread", qp_thread_exit, QP_METH_NOARGS, 0,
      "exit_thread($module, /)\n--\n\nAn obsolete synonym of exit()." },
    { "exit", qp_thread_exit, QP_METH_NOARGS, 0,
      "exit($module, /)\n--\n\n"
      "This is synonymous to ``raise SystemExit''.  It will cause the current\n"
      "thread to exit silently unless the exception is caught." },
    { "interrupt_main", qp_thread_interrupt_main, QP_METH_FASTCALL, 0,
      "interrupt_main($module, signum=signal.SIGINT, /)\n--\n\n"
      "Simulate the arrival of the given signal in the main thread,\n"
      "where the corresponding signal handler will be executed.\n"
      "If *signum* is omitted, SIGINT is assumed.\n"
      "A subthread can use this function to interrupt the main thread.\n\n"
      "Note: the default signal handler for SIGINT raises ``KeyboardInterrupt``." },
    { "get_ident", qp_thread_get_ident, QP_METH_NOARGS, 0,
      "get_ident($module, /)\n--\n\n"
      "Return a non-zero integer that uniquely identifies the current thread\n"
      "amongst other threads that exist simultaneously.\n"
      "This may be used to identify per-thread resources.\n"
      "Even though on some platforms threads identities may appear to be\n"
      "allocated consecutive numbers starting at 1, this behavior should not\n"
      "be relied upon, and the number should be seen purely as a magic cookie.\n"
      "A thread's identity may be reused for another thread after it exits." },
#ifdef QP_THREAD_HAVE_NATIVE_ID
    { "get_native_id", qp_thread_get_native_id, QP_METH_NOARGS, 0,
      "get_native_id($module, /)\n--\n\n"
      "Return a non-negative integer identifying the thread as reported\n"
      "by the OS (kernel). This may be used to uniquely identify a\n"
      "particular thread within a system." },
#endif
    { "_count", qp_thread_count, QP_METH_NOARGS, 0,
      "_count($module, /)\n--\n\n"
      "Return the number of currently running Python threads, excluding\n"
      "the main thread. The returned number comprises all threads created\n"
      "through `start_new_thread()` as well as `threading.Thread`, and not\n"
      "yet finished.\n\n"
      "This function is meant for internal and specialized purposes only.\n"
      "In most applications `threading.enumerate()` should be used instead." },
    { "stack_size", qp_thread_stack_size, QP_METH_FASTCALL, 0,
      "stack_size($module, size=0, /)\n--\n\n"
      "Return the thread stack size used when creating new threads.  The\n"
      "optional size argument specifies the stack size (in bytes) to be used\n"
      "for subsequently created threads, and must be 0 (use platform or\n"
      "configured default) or a positive integer value of at least 32,768 (32k).\n"
      "If changing the thread stack size is unsupported, a ThreadError\n"
      "exception is raised.  If the specified size is invalid, a ValueError\n"
      "exception is raised, and the stack size is unmodified.  32k bytes\n"
      " currently the minimum supported stack size value to guarantee\n"
      "sufficient stack space for the interpreter itself.\n\n"
      "Note that some platforms may have particular restrictions on values for\n"
      "the stack size, such as requiring a minimum stack size larger than 32 KiB or\n"
      "requiring allocation in multiples of the system memory page size\n"
      "- platform documentation should be referred to for more information\n"
      "(4 KiB pages are common; using multiples of 4096 for the stack size is\n"
      "the suggested approach in the absence of more specific information)." },
    { "_excepthook", qp_thread_excepthook, QP_METH_O, 0,
      "_excepthook($module, args, /)\n--\n\nHandle uncaught Thread.run() exception." },
    { "_is_main_interpreter", qp_thread_is_main_interpreter, QP_METH_NOARGS, 0,
      "_is_main_interpreter($module, /)\n--\n\n"
      "Return True if the current interpreter is the main Python interpreter." },
    { "_shutdown", qp_thread_shutdown, QP_METH_NOARGS, 0,
      "_shutdown($module, /)\n--\n\n"
      "Wait for all non-daemon threads (other than the calling thread) to stop." },
    { "_make_thread_handle", qp_thread_make_thread_handle, QP_METH_O, 0,
      "_make_thread_handle($module, ident, /)\n--\n\n"
      "Internal only. Make a thread handle for threads not spawned\n"
      "by the _thread or threading module." },
    { "_get_main_thread_ident", qp_thread_get_main_thread_ident, QP_METH_NOARGS, 0,
      "_get_main_thread_ident($module, /)\n--\n\n"
      "Internal only. Return a non-zero integer that uniquely identifies the main thread\n"
      "of the main interpreter." },
#ifdef QP_THREAD_NAME_MAXLEN
    { "set_name", qp_thread_set_name, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_name($module, /, name)\n--\n\nSet the name of the current thread." },
    { "_get_name", qp_thread_get_name, QP_METH_NOARGS, 0,
      "_get_name($module, /)\n--\n\nGet the name of the current thread." },
#endif
    { NULL }
};

/* module dict[name] = v (consumes v) */
static int qp_thread_add(JSContext *ctx, JSValueConst module, const char *name, JSValue v)
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

static int qp_thread_exec(JSContext *ctx, JSValueConst module)
{
    static const struct { int which; const QPTypeSpec *spec; const char *name; } types[] = {
        { QP_THREAD_T_HANDLE, &qp_thread_handle_spec, "_ThreadHandle" },
        { QP_THREAD_T_LOCK, &qp_thread_lock_spec, "lock" },
        { QP_THREAD_T_RLOCK, &qp_thread_rlock_spec, "RLock" },
        { QP_THREAD_T_LOCAL, &qp_thread_local_spec, "_local" },
    };
    QPModState *ms = qp_modstate(ctx);
    JSValue t;
    size_t i;
    double timeout_max;
    if (!ms)
        return -1;
    for (i = 0; i < countof(types); i++) {
        t = qp_type_from_spec(ctx, types[i].spec);
        if (JS_IsException(t))
            return -1;
        JS_FreeValue(ctx, ms->thread_types[types[i].which]);
        ms->thread_types[types[i].which] = JS_DupValue(ctx, t);
        if (qp_thread_add(ctx, module, types[i].name, t) < 0)
            return -1;
    }
    if (qp_thread_add(ctx, module, "LockType", JS_DupValue(ctx, ms->thread_types[QP_THREAD_T_LOCK])) < 0)
        return -1;
    if (qp_thread_add(ctx, module, "error",
                      JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, RuntimeError)))) < 0)
        return -1;
    t = qp_structseq_new_type(ctx, &qp_thread_excepthook_desc);
    if (JS_IsException(t))
        return -1;
    JS_FreeValue(ctx, ms->thread_types[QP_THREAD_T_EXCEPTHOOK]);
    ms->thread_types[QP_THREAD_T_EXCEPTHOOK] = JS_DupValue(ctx, t);
    if (qp_thread_add(ctx, module, "_ExceptHookArgs", t) < 0)
        return -1;
    /* TIMEOUT_MAX: min(PY_TIMEOUT_MAX us, PyTime_MAX ns) in seconds, floored */
    timeout_max = (double)QP_THREAD_TIMEOUT_MAX_US * 1e-6;
    if ((double)INT64_MAX * 1e-9 < timeout_max)
        timeout_max = (double)INT64_MAX * 1e-9;
    timeout_max = floor(timeout_max);
    if (qp_thread_add(ctx, module, "TIMEOUT_MAX", qp_float_new(ctx, timeout_max)) < 0)
        return -1;
#ifdef QP_THREAD_NAME_MAXLEN
    if (qp_thread_add(ctx, module, "_NAME_MAXLEN", JS_NewInt32(ctx, QP_THREAD_NAME_MAXLEN)) < 0)
        return -1;
#endif
    return 0;
}

static const QPModuleDef qp_moddef__thread = {
    "_thread",
    "This module provides primitive operations to write multi-threaded programs.\n"
    "The 'threading' module provides a more convenient interface.",
    qp_thread_methods,
    qp_thread_exec,
};
