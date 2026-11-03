/*
 * faulthandler: dump the Python tracebacks on a fault, after a timeout or
 * on a user signal.
 *
 * Derived from CPython Modules/faulthandler.c and the signal-safe dump
 * functions of Python/traceback.c and Python/pylifecycle.c
 * (_Py_DumpExtensionModules, fatal_error) (PSF-2.0), v3.15.
 *
 * Runtime mapping:
 * - A thread's Python frames are the frames of its JSStackFrame chain that
 *   run Python bytecode (qp_fr_sf_func): the chain of the GIL holder is
 *   JSRuntime.current_stack_frame, the one of a thread waiting for the GIL
 *   is saved in its QPThreadState (QPThreadState.js.current_stack_frame).
 *   While the runtime has a single Python thread (JSRuntime.py_threads
 *   NULL), the thread that last called this module is the Python thread.
 * - The frames, code metadata and strings are read without allocation or
 *   locking, so that the dumps run in signal handlers and in the watchdog
 *   thread while the other threads run (like CPython, the reads of another
 *   thread's running frames are racy).
 * - The state is process-wide like CPython's _PyRuntime.faulthandler; each
 *   configuration records the Python context it reports on, and
 *   qp_faulthandler_fini() (Py_FinalizeEx, release of the state) disables
 *   the ones of a context.
 * - Py_FatalError (qp_fatal_error) is implemented here as well: the C API
 *   (python/capi_runtime.c) and _fatal_error_c_thread() use it.
 */

#include <signal.h>
#include <sys/resource.h>
#include <dlfcn.h>
#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define QP_FH_CAN_C_BACKTRACE 1
#endif
#if defined(__linux__) && defined(_GNU_SOURCE) && defined(__GLIBC__)
#include <link.h>
#define QP_FH_HAVE_DLADDR1 1
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#endif

#define QP_FH_MAX_STRING_LENGTH 500
#define QP_FH_MAX_FRAME_DEPTH 100
#define QP_FH_DEFAULT_MAX_NTHREADS 100
/* allocate at most 100 MiB of the stack to raise the stack overflow */
#define QP_FH_STACK_OVERFLOW_MAX_SIZE (100 * 1024 * 1024)

/* ------------------------------------------------------------------------ */
/* Signal-safe output                                                        */
/* ------------------------------------------------------------------------ */

/* _Py_write_noraise: write() retried on EINTR and short writes */
static void qp_fh_write(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            return;
        p += n;
        len -= (size_t)n;
    }
}

static void qp_fh_puts(int fd, const char *s)
{
    qp_fh_write(fd, s, strlen(s));
}

/* _Py_DumpDecimal */
static void qp_fh_dump_decimal(int fd, size_t value)
{
    char buf[3 * sizeof(size_t) + 1], *p = buf + sizeof(buf);
    do {
        *--p = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    qp_fh_write(fd, p, (size_t)(buf + sizeof(buf) - p));
}

/* dump_hexadecimal: at least width digits */
static void qp_fh_dump_hex(int fd, uintptr_t value, int width, BOOL strip_zeros)
{
    static const char digits[] = "0123456789abcdef";
    char buf[sizeof(uintptr_t) * 2], *end = buf + sizeof(buf), *p = end;
    if (width > (int)sizeof(buf))
        width = (int)sizeof(buf);
    do {
        *--p = digits[value & 15];
        value >>= 4;
    } while ((end - p) < width || value);
    if (strip_zeros) {
        while (*p == '0' && end - p >= 2)
            p++;
    }
    qp_fh_write(fd, p, (size_t)(end - p));
}

/* _Py_DumpASCII of a flat string (code units; surrogate pairs are joined) */
static void qp_fh_dump_string(int fd, const JSString *str)
{
    uint32_t i, size = str->len;
    BOOL truncated = FALSE;
    char out[64];
    int n = 0;

    if (size > QP_FH_MAX_STRING_LENGTH) {
        size = QP_FH_MAX_STRING_LENGTH;
        truncated = TRUE;
    }
    if (!str->is_wide_char) {
        for (i = 0; i < size; i++) {
            uint8_t c = str->u.str8[i];
            if (c < ' ' || c > 126)
                break;
        }
        if (i == size) {
            qp_fh_write(fd, str->u.str8, size);
            goto done;
        }
    }
    for (i = 0; i < size; i++) {
        uint32_t c = str->is_wide_char ? str->u.str16[i] : str->u.str8[i];
        int width;
        if (str->is_wide_char && c >= 0xd800 && c < 0xdc00 && i + 1 < str->len &&
            str->u.str16[i + 1] >= 0xdc00 && str->u.str16[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (str->u.str16[i + 1] - 0xdc00);
            i++;
        }
        if (n > (int)sizeof(out) - 10) {
            qp_fh_write(fd, out, (size_t)n);
            n = 0;
        }
        if (c >= ' ' && c <= 126) {
            out[n++] = (char)c;
            continue;
        }
        out[n++] = '\\';
        if (c <= 0xff) {
            out[n++] = 'x';
            width = 2;
        } else if (c <= 0xffff) {
            out[n++] = 'u';
            width = 4;
        } else {
            out[n++] = 'U';
            width = 8;
        }
        while (width-- > 0)
            out[n++] = "0123456789abcdef"[(c >> (4 * width)) & 15];
    }
    if (n)
        qp_fh_write(fd, out, (size_t)n);
 done:
    if (truncated)
        qp_fh_puts(fd, "...");
}

/* An atom as text; FALSE when it cannot be read safely */
static BOOL qp_fh_dump_atom(int fd, JSRuntime *rt, JSAtom atom)
{
    JSAtomStruct *p;
    if (atom == JS_ATOM_NULL)
        return FALSE;
    if (__JS_AtomIsTaggedInt(atom)) {
        qp_fh_dump_decimal(fd, __JS_AtomToUInt32(atom));
        return TRUE;
    }
    if (atom >= (JSAtom)rt->atom_size)
        return FALSE;
    p = rt->atom_array[atom];
    if (!p || atom_is_free(p))
        return FALSE;
    qp_fh_dump_string(fd, p);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Tracebacks                                                                */
/* ------------------------------------------------------------------------ */

/* The thread that runs Python while the runtime has a single Python thread
   (JSRuntime.py_threads NULL): the last caller of this module, of
   Py_FatalError or of the dump functions of the C API. */
static uint64_t qp_fh_single_ident;
static JSRuntime *qp_fh_single_rt;

static void qp_fh_note_thread(JSContext *ctx)
{
    if (!ctx->rt->py_threads) {
        qp_fh_single_rt = ctx->rt;
        qp_fh_single_ident = qp_thread_self_ident();
    }
}

/* The thread state of the calling thread in ctx's runtime, NULL if it has
   none (single Python thread, or a thread that never ran Python code). */
static QPThreadState *qp_fh_self_tstate(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *ts;
    if (!thr)
        return NULL;
    ts = qp_tstate_tls;
    return ts && ts->thr == thr ? ts : NULL;
}

/* Is the calling thread the Python thread of a single-threaded runtime? */
static BOOL qp_fh_self_is_single(JSContext *ctx)
{
    return !ctx->rt->py_threads && qp_fh_single_rt == ctx->rt &&
        qp_fh_single_ident == qp_thread_self_ident();
}

/* The frame chain of the thread ts (NULL: the single Python thread) */
static JSStackFrame *qp_fh_thread_frames(JSContext *ctx, QPThreadState *ts)
{
    QPThreads *thr = ctx->rt->py_threads;
    if (!ts || !thr)
        return ctx->rt->current_stack_frame;
    if (ts == __atomic_load_n(&thr->current, __ATOMIC_RELAXED))
        return ctx->rt->current_stack_frame;
    return ts->js.current_stack_frame;
}

/* dump_frame: "  File "xxx", line xxx in xxx" */
static void qp_fh_dump_frame(int fd, JSRuntime *rt, JSStackFrame *sf, JSFunctionBytecode *b)
{
    QPLoc loc;
    int lineno = -1;

    qp_fh_puts(fd, "  File ");
    if (b->has_debug && b->debug.filename != JS_ATOM_NULL) {
        qp_fh_puts(fd, "\"");
        if (!qp_fh_dump_atom(fd, rt, b->debug.filename))
            qp_fh_puts(fd, "???");
        qp_fh_puts(fd, "\"");
    } else {
        qp_fh_puts(fd, "???");
    }
    qp_fh_puts(fd, ", line ");
    if (sf->cur_pc && qp_code_addr2loc(b, qp_fr_sf_pc(sf, b), &loc) == 0)
        lineno = loc.lineno;
    if (lineno >= 0)
        qp_fh_dump_decimal(fd, (size_t)lineno);
    else
        qp_fh_puts(fd, "???");
    qp_fh_puts(fd, " in ");
    if (!qp_fh_dump_atom(fd, rt, b->func_name))
        qp_fh_puts(fd, "???");
    qp_fh_puts(fd, "\n");
}

/* dump_traceback */
static void qp_fh_dump_frames(int fd, JSContext *ctx, JSStackFrame *sf, BOOL write_header)
{
    unsigned int depth = 0;

    if (write_header)
        qp_fh_puts(fd, "Stack (most recent call first):\n");
    for (; sf; sf = sf->prev_frame) {
        JSObject *p = qp_fr_sf_func(sf);
        if (!p)
            continue;
        if (depth >= QP_FH_MAX_FRAME_DEPTH)
            break;
        qp_fh_dump_frame(fd, ctx->rt, sf, p->u.func.function_bytecode);
        depth++;
    }
    if (depth == 0)
        qp_fh_puts(fd, "  <no Python frame>\n");
}

/* PyUnstable_DumpTraceback of the calling thread (nothing if it has no
   Python thread state) */
static void qp_fh_dump_traceback_self(int fd, JSContext *ctx)
{
    QPThreadState *ts = qp_fh_self_tstate(ctx);
    if (ts)
        qp_fh_dump_frames(fd, ctx, qp_fh_thread_frames(ctx, ts), TRUE);
    else if (qp_fh_self_is_single(ctx))
        qp_fh_dump_frames(fd, ctx, ctx->rt->current_stack_frame, TRUE);
}

/* PyUnstable_DumpTraceback of the thread whose _thread.get_ident() is
   ident.  NULL or an error message. */
static const char *qp_fh_dump_traceback_ident(int fd, JSContext *ctx, uint64_t ident)
{
    QPThreads *thr = ctx->rt->py_threads;
    struct list_head *el;

    if (!thr) {
        qp_fh_dump_frames(fd, ctx, ctx->rt->current_stack_frame, TRUE);
        return NULL;
    }
    list_for_each(el, &thr->tstates) {
        QPThreadState *ts = list_entry(el, QPThreadState, link);
        if (ts->ident == ident) {
            qp_fh_dump_frames(fd, ctx, qp_fh_thread_frames(ctx, ts), TRUE);
            return NULL;
        }
    }
    return "unable to get the thread state";
}

/* write_thread_id: "Current thread 0xHHHH [name] (most recent call first):" */
static void qp_fh_write_thread_id(int fd, uint64_t ident, BOOL is_current)
{
    qp_fh_puts(fd, is_current ? "Current thread 0x" : "Thread 0x");
    qp_fh_dump_hex(fd, (uintptr_t)ident, (int)sizeof(unsigned long) * 2, FALSE);
#if defined(__APPLE__) || defined(__GLIBC__)
    {
        char name[100];
        if (pthread_getname_np((pthread_t)(uintptr_t)ident, name, sizeof(name)) == 0) {
            size_t len = strnlen(name, sizeof(name));
            if (len) {
                qp_fh_puts(fd, " [");
                qp_fh_write(fd, name, len);
                qp_fh_puts(fd, "]");
            }
        }
    }
#endif
    qp_fh_puts(fd, " (most recent call first):\n");
}

static BOOL qp_fh_gc_collecting(JSContext *ctx)
{
    QPState *s = ctx->py;
    return s && s->mod_state && s->mod_state->gc_collecting;
}

/* One thread of PyUnstable_DumpTracebackThreads */
static void qp_fh_dump_thread(int fd, JSContext *ctx, uint64_t ident, JSStackFrame *sf,
                              BOOL is_current)
{
    qp_fh_write_thread_id(fd, ident, is_current);
    if (is_current && qp_fh_gc_collecting(ctx))
        qp_fh_puts(fd, "  Garbage-collecting\n");
    qp_fh_dump_frames(fd, ctx, sf, FALSE);
}

/* PyUnstable_DumpTracebackThreads: the threads of ctx's runtime, newest
   first; the calling thread is the current one when self_known.  NULL or
   an error message. */
static const char *qp_fh_dump_threads(int fd, JSContext *ctx, BOOL self_known,
                                      Py_ssize_t max_threads)
{
    QPThreads *thr;
    struct list_head *el;
    QPThreadState *self;
    Py_ssize_t nthreads = 0;

    if (!ctx || !ctx->py)
        return "unable to get the interpreter state";
    if (max_threads == 0)
        max_threads = QP_FH_DEFAULT_MAX_NTHREADS;
    thr = ctx->rt->py_threads;
    if (!thr) {
        uint64_t ident;
        if (qp_fh_single_rt == ctx->rt)
            ident = qp_fh_single_ident;
        else if (self_known)
            ident = qp_thread_self_ident();     /* the C API, never noted */
        else
            return "unable to get the thread head state";
        if (max_threads <= 0) {
            qp_fh_puts(fd, "...\n");
            return NULL;
        }
        qp_fh_dump_thread(fd, ctx, ident, ctx->rt->current_stack_frame,
                          self_known && ident == qp_thread_self_ident());
        return NULL;
    }
    self = self_known ? qp_fh_self_tstate(ctx) : NULL;
    for (el = thr->tstates.prev; el != &thr->tstates; el = el->prev) {
        QPThreadState *ts = list_entry(el, QPThreadState, link);
        if (nthreads != 0)
            qp_fh_puts(fd, "\n");
        if (nthreads >= max_threads) {
            qp_fh_puts(fd, "...\n");
            break;
        }
        qp_fh_dump_thread(fd, ctx, ts->ident, qp_fh_thread_frames(ctx, ts), ts == self);
        nthreads++;
    }
    return NULL;
}

/* faulthandler_dump_traceback: from a signal handler */
static void qp_fh_dump_traceback_signal(int fd, JSContext *ctx, BOOL all_threads,
                                        Py_ssize_t max_threads)
{
    static volatile int reentrant = 0;

    if (reentrant || !ctx)
        return;
    reentrant = 1;
    if (all_threads)
        (void)qp_fh_dump_threads(fd, ctx, TRUE, max_threads);
    else
        qp_fh_dump_traceback_self(fd, ctx);
    reentrant = 0;
}

/* ------------------------------------------------------------------------ */
/* C stack (_Py_DumpStack)                                                   */
/* ------------------------------------------------------------------------ */

#ifdef QP_FH_CAN_C_BACKTRACE
static void qp_fh_dump_pointer(int fd, const void *ptr)
{
    qp_fh_puts(fd, "0x");
    qp_fh_dump_hex(fd, (uintptr_t)ptr, (int)sizeof(void *), TRUE);
}

/* _Py_backtrace_symbols_fd */
static void qp_fh_backtrace_symbols(int fd, void *const *array, int size)
{
    int i;
    for (i = 0; i < size; i++) {
        Dl_info info;
        int status;
        memset(&info, 0, sizeof(info));
#ifdef QP_FH_HAVE_DLADDR1
        {
            struct link_map *map = NULL;
            status = dladdr1(array[i], &info, (void **)&map, RTLD_DL_LINKMAP);
            if (status != 0 && info.dli_fname && info.dli_fname[0] && map)
                info.dli_fbase = (void *)map->l_addr;
        }
#else
        status = dladdr(array[i], &info);
#endif
        if (status == 0 || !info.dli_fname || !info.dli_fname[0]) {
            qp_fh_puts(fd, "  Binary file '<unknown>' [");
            qp_fh_dump_pointer(fd, array[i]);
            qp_fh_puts(fd, "]\n");
            continue;
        }
        if (!info.dli_sname)
            info.dli_saddr = info.dli_fbase;
        if (!info.dli_sname && !info.dli_saddr) {
            qp_fh_puts(fd, "  Binary file \"");
            qp_fh_puts(fd, info.dli_fname);
            qp_fh_puts(fd, "\" [");
        } else {
            char sign;
            uintptr_t offset;
            if ((uintptr_t)array[i] >= (uintptr_t)info.dli_saddr) {
                sign = '+';
                offset = (uintptr_t)array[i] - (uintptr_t)info.dli_saddr;
            } else {
                sign = '-';
                offset = (uintptr_t)info.dli_saddr - (uintptr_t)array[i];
            }
            qp_fh_puts(fd, "  Binary file \"");
            qp_fh_puts(fd, info.dli_fname);
            qp_fh_puts(fd, "\", at ");
            qp_fh_puts(fd, info.dli_sname ? info.dli_sname : "");
            qp_fh_write(fd, &sign, 1);
            qp_fh_puts(fd, "0x");
            qp_fh_dump_hex(fd, offset, (int)sizeof(offset), TRUE);
            qp_fh_puts(fd, " [");
        }
        qp_fh_dump_pointer(fd, array[i]);
        qp_fh_puts(fd, "]\n");
    }
}
#endif

static void qp_fh_dump_stack(int fd)
{
#ifdef QP_FH_CAN_C_BACKTRACE
#define QP_FH_BACKTRACE_SIZE 32
    void *callstack[QP_FH_BACKTRACE_SIZE];
    int frames;

    qp_fh_puts(fd, "Current thread's C stack trace (most recent call first):\n");
    frames = backtrace(callstack, QP_FH_BACKTRACE_SIZE);
    if (frames == 0) {
        qp_fh_puts(fd, "  <system returned no stack trace>\n");
        return;
    }
    qp_fh_backtrace_symbols(fd, callstack, frames);
    if (frames == QP_FH_BACKTRACE_SIZE)
        qp_fh_puts(fd, "  <truncated rest of calls>\n");
#undef QP_FH_BACKTRACE_SIZE
#else
    qp_fh_puts(fd, "Current thread's C stack trace (most recent call first):\n");
    qp_fh_puts(fd, "  <cannot get C stack on this system>\n");
#endif
}

/* _Py_InitDumpStack: load what backtrace() needs outside of a signal handler */
static void qp_fh_init_dump_stack(void)
{
#ifdef QP_FH_CAN_C_BACKTRACE
    void *callstack[1];
    (void)backtrace(callstack, 1);
#endif
}

/* ------------------------------------------------------------------------ */
/* Extension modules (_Py_DumpExtensionModules)                              */
/* ------------------------------------------------------------------------ */

/* the flat string of a str key, NULL otherwise */
static JSString *qp_fh_str(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING ? JS_VALUE_GET_STRING(v) : NULL;
}

static BOOL qp_fh_str_eq_ascii(const JSString *s, const char *a)
{
    uint32_t i, n = (uint32_t)strlen(a);
    if (s->len != n)
        return FALSE;
    for (i = 0; i < n; i++) {
        uint32_t c = s->is_wide_char ? s->u.str16[i] : s->u.str8[i];
        if (c != (uint8_t)a[i])
            return FALSE;
    }
    return TRUE;
}

static uint32_t qp_fh_str_char(const JSString *s, uint32_t i)
{
    return s->is_wide_char ? s->u.str16[i] : s->u.str8[i];
}

/* key == item or key starts with item + "." */
static BOOL qp_fh_is_stdlib_name(const JSString *key, const JSString *item)
{
    uint32_t i;
    if (item->len > key->len)
        return FALSE;
    for (i = 0; i < item->len; i++) {
        if (qp_fh_str_char(key, i) != qp_fh_str_char(item, i))
            return FALSE;
    }
    return key->len == item->len || qp_fh_str_char(key, item->len) == '.';
}

/* _PyModule_IsExtension: a builtin module (native or C API) or a C
   extension module */
static BOOL qp_fh_module_is_extension(JSValueConst v)
{
    QPModule *m;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_MODULE)
        return FALSE;
    m = JS_VALUE_GET_OBJ(v)->u.opaque;
    return m && (m->builtin || m->capi_seq != 0 || m->c_ext);
}

static BOOL qp_fh_is_dict(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_DICT && QP_DICT(v);
}

static void qp_fh_dump_extension_modules(int fd, JSContext *ctx)
{
    QPState *s;
    JSValue stdlib_names = QP_NULL, key, value;
    Py_ssize_t pos, count = 0;
    BOOL header = TRUE;

    if (!ctx || !(s = ctx->py) || s->finalized || !qp_fh_is_dict(s->modules))
        return;
    if (qp_fh_is_dict(s->sys_dict)) {
        pos = 0;
        while (qp_dict_next(s->sys_dict, &pos, &key, &value)) {
            JSString *k = qp_fh_str(key);
            if (k && qp_fh_str_eq_ascii(k, "stdlib_module_names")) {
                stdlib_names = value;
                break;
            }
        }
    }
    /* not a frozenset: stdlib modules are not excluded */
    if (!QP_IS_NULL(stdlib_names) &&
        !(JS_VALUE_GET_TAG(stdlib_names) == JS_TAG_OBJECT &&
          JS_VALUE_GET_OBJ(stdlib_names)->class_id == JS_CLASS_PY_FROZENSET))
        stdlib_names = QP_NULL;
    pos = 0;
    while (qp_dict_next(s->modules, &pos, &key, &value)) {
        JSString *k = qp_fh_str(key);
        if (!k || !qp_fh_module_is_extension(value))
            continue;
        if (!QP_IS_NULL(stdlib_names)) {
            Py_ssize_t i = 0;
            JSValue item;
            BOOL is_stdlib = FALSE;
            while (qp_set_next(stdlib_names, &i, &item, NULL)) {
                JSString *it = qp_fh_str(item);
                if (it && qp_fh_is_stdlib_name(k, it)) {
                    is_stdlib = TRUE;
                    break;
                }
            }
            if (is_stdlib)
                continue;
        }
        qp_fh_puts(fd, header ? "\nExtension modules: " : ", ");
        header = FALSE;
        qp_fh_dump_string(fd, k);
        count++;
    }
    if (count) {
        qp_fh_puts(fd, " (total: ");
        qp_fh_dump_decimal(fd, (size_t)count);
        qp_fh_puts(fd, ")\n");
    }
}

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct QPFaultHandler {
    int signum;
    int enabled;
    const char *name;
    struct sigaction previous;
} QPFaultHandler;

static QPFaultHandler qp_fh_handlers[] = {
#ifdef SIGBUS
    { SIGBUS, 0, "Bus error" },
#endif
#ifdef SIGILL
    { SIGILL, 0, "Illegal instruction" },
#endif
    { SIGFPE, 0, "Floating-point exception" },
    { SIGABRT, 0, "Aborted" },
    /* last: the default choice if the signal is not found */
    { SIGSEGV, 0, "Segmentation fault" },
};
#define QP_FH_NSIGNALS ((int)countof(qp_fh_handlers))

/* fatal_error */
static struct {
    int enabled;
    int fd;
    JSValue file;                   /* QP_NULL: none (a file descriptor was given) */
    int all_threads;
    int c_stack;
    Py_ssize_t max_threads;
    JSContext *ctx;
} qp_fh_fatal = { 0, -1, QP_NULL };

/* user signals (register) */
typedef struct QPFaultUser {
    int enabled;
    int fd;
    JSValue file;
    int all_threads;
    int chain;
    Py_ssize_t max_threads;
    JSContext *ctx;
    struct sigaction previous;
} QPFaultUser;
static QPFaultUser qp_fh_user[QP_NSIG];

/* the dump_traceback_later() watchdog */
static struct {
    BOOL running;                   /* the thread exists */
    BOOL sync_init;
    pthread_t thread;
    pid_t pid;                      /* of the process that started it */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    BOOL cancel;                    /* under mutex */
    int fd;
    JSValue file;
    int64_t timeout_us;
    int repeat;
    int exit;
    char header[100];
    size_t header_len;
    Py_ssize_t max_threads;
    JSContext *ctx;
} qp_fh_watchdog = { .file = QP_NULL };

/* the alternate signal stack of the thread that enabled the handlers */
static stack_t qp_fh_stack, qp_fh_old_stack;

static void qp_fh_clear_file(JSContext *ctx, JSValue *pfile)
{
    JSValue f = *pfile;
    *pfile = QP_NULL;
    if (!QP_IS_NULL(f))
        JS_FreeValue(ctx, f);
}

/* ------------------------------------------------------------------------ */
/* Fatal signals                                                             */
/* ------------------------------------------------------------------------ */

static void qp_fh_disable_fatal_handler(QPFaultHandler *h)
{
    if (!h->enabled)
        return;
    h->enabled = 0;
    (void)sigaction(h->signum, &h->previous, NULL);
}

/* faulthandler_dump_c_stack */
static void qp_fh_dump_c_stack_signal(int fd)
{
    static volatile int reentrant = 0;
    if (reentrant)
        return;
    reentrant = 1;
    if (qp_fh_fatal.c_stack) {
        qp_fh_puts(fd, "\n");
        qp_fh_dump_stack(fd);
    }
    reentrant = 0;
}

/* faulthandler_fatal_error: SIGSEGV, SIGFPE, SIGABRT, SIGBUS and SIGILL.
   Display the tracebacks, restore the previous handler and call it. */
static void qp_fh_fatal_handler(int signum)
{
    const int fd = qp_fh_fatal.fd;
    QPFaultHandler *h = NULL;
    int save_errno = errno, i;
    BOOL found = FALSE;

    if (!qp_fh_fatal.enabled)
        return;
    for (i = 0; i < QP_FH_NSIGNALS; i++) {
        h = &qp_fh_handlers[i];
        if (h->signum == signum) {
            found = TRUE;
            break;
        }
    }
    /* restore the previous handler */
    qp_fh_disable_fatal_handler(h);
    if (found) {
        qp_fh_puts(fd, "Fatal Python error: ");
        qp_fh_puts(fd, h->name);
        qp_fh_puts(fd, "\n\n");
    } else {
        qp_fh_puts(fd, "Fatal Python error from unexpected signum: ");
        qp_fh_dump_decimal(fd, (size_t)signum);
        qp_fh_puts(fd, "\n\n");
    }
    qp_fh_dump_traceback_signal(fd, qp_fh_fatal.ctx, qp_fh_fatal.all_threads,
                                qp_fh_fatal.max_threads);
    qp_fh_dump_c_stack_signal(fd);
    qp_fh_dump_extension_modules(fd, qp_fh_fatal.ctx);
    errno = save_errno;
    /* call the previous handler: immediately thanks to SA_NODEFER */
    raise(signum);
}

/* faulthandler_allocate_stack: the alternate stack of the calling thread,
   so that the handlers run on a stack overflow */
static int qp_fh_allocate_stack(JSContext *ctx)
{
    if (qp_fh_stack.ss_sp)
        return 0;
    qp_fh_stack.ss_size = SIGSTKSZ * 2;
#if defined(__linux__) && defined(AT_MINSIGSTKSZ)
    {
        unsigned long min_size = getauxval(AT_MINSIGSTKSZ);
        if (min_size != 0)
            qp_fh_stack.ss_size = SIGSTKSZ + min_size;
    }
#endif
    qp_fh_stack.ss_flags = 0;
    qp_fh_stack.ss_sp = qp_sys_malloc(qp_fh_stack.ss_size);
    if (!qp_fh_stack.ss_sp) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    if (sigaltstack(&qp_fh_stack, &qp_fh_old_stack) != 0) {
        int err = errno;
        qp_sys_free(qp_fh_stack.ss_sp);
        qp_fh_stack.ss_sp = NULL;
        qp_raise_os_error(ctx, err, QP_NULL);
        return -1;
    }
    return 0;
}

static void qp_fh_free_stack(void)
{
    stack_t cur;
    if (!qp_fh_stack.ss_sp)
        return;
    memset(&cur, 0, sizeof(cur));
    /* restore the previous stack if ours is the current one */
    if (sigaltstack(NULL, &cur) == 0 && cur.ss_sp == qp_fh_stack.ss_sp)
        sigaltstack(&qp_fh_old_stack, NULL);
    qp_sys_free(qp_fh_stack.ss_sp);
    qp_fh_stack.ss_sp = NULL;
}

/* RuntimeError from errno (PyErr_SetFromErrno(PyExc_RuntimeError)) */
static void qp_fh_raise_errno(JSContext *ctx, QPType *type, int err)
{
    JSValue args[2], exc;
    args[0] = JS_NewInt32(ctx, err);
    args[1] = qp_str_from_cstr(ctx, strerror(err));
    if (JS_IsException(args[1]))
        return;
    exc = qp_exc_new(ctx, type, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    if (!JS_IsException(exc))
        qp_raise_object(ctx, exc);
}

/* faulthandler_enable */
static int qp_fh_enable(JSContext *ctx)
{
    int i;

    if (qp_fh_fatal.enabled)
        return 0;
    qp_fh_fatal.enabled = 1;
    if (qp_fh_allocate_stack(ctx) < 0)
        return -1;
    qp_fh_init_dump_stack();
    for (i = 0; i < QP_FH_NSIGNALS; i++) {
        QPFaultHandler *h = &qp_fh_handlers[i];
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = qp_fh_fatal_handler;
        sigemptyset(&action.sa_mask);
        /* do not prevent the signal from being received from within its
           own handler; run on the alternate stack */
        action.sa_flags = SA_NODEFER | SA_ONSTACK;
        if (sigaction(h->signum, &action, &h->previous) != 0) {
            qp_fh_raise_errno(ctx, QP_EXC(ctx, RuntimeError), errno);
            return -1;
        }
        h->enabled = 1;
    }
    return 0;
}

/* faulthandler_disable */
static void qp_fh_disable(JSContext *ctx)
{
    int i;
    if (qp_fh_fatal.enabled) {
        qp_fh_fatal.enabled = 0;
        for (i = 0; i < QP_FH_NSIGNALS; i++)
            qp_fh_disable_fatal_handler(&qp_fh_handlers[i]);
    }
    qp_fh_clear_file(ctx, &qp_fh_fatal.file);
}

/* ------------------------------------------------------------------------ */
/* User signals                                                              */
/* ------------------------------------------------------------------------ */

static void qp_fh_user_handler(int signum);

static int qp_fh_register_signal(int signum, int chain, struct sigaction *previous)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = qp_fh_user_handler;
    sigemptyset(&action.sa_mask);
    /* restart the interrupted system calls; with chain, do not prevent
       the signal from being received from within its own handler */
    action.sa_flags = chain ? SA_NODEFER : SA_RESTART;
    action.sa_flags |= SA_ONSTACK;
    return sigaction(signum, &action, previous);
}

/* faulthandler_user: dump the tracebacks, then call the previous handler
   if chain */
static void qp_fh_user_handler(int signum)
{
    QPFaultUser *u;
    int save_errno = errno;

    if (signum <= 0 || signum >= QP_NSIG)
        return;
    u = &qp_fh_user[signum];
    if (!u->enabled)
        return;
    qp_fh_dump_traceback_signal(u->fd, u->ctx, u->all_threads, u->max_threads);
    if (u->chain) {
        (void)sigaction(signum, &u->previous, NULL);
        errno = save_errno;
        raise(signum);
        save_errno = errno;
        (void)qp_fh_register_signal(signum, u->chain, NULL);
    }
    errno = save_errno;
}

static int qp_fh_unregister(JSContext *ctx, QPFaultUser *u, int signum)
{
    if (!u->enabled)
        return 0;
    u->enabled = 0;
    (void)sigaction(signum, &u->previous, NULL);
    qp_fh_clear_file(ctx, &u->file);
    u->fd = -1;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Watchdog                                                                  */
/* ------------------------------------------------------------------------ */

static void *qp_fh_watchdog_main(void *arg)
{
    sigset_t set;
    BOOL ok = TRUE;

    /* this thread receives no signal */
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
    do {
        int64_t deadline = (int64_t)qp_thread_monotonic_ns() +
            qp_fh_watchdog.timeout_us * 1000;
        BOOL cancelled;
        pthread_mutex_lock(&qp_fh_watchdog.mutex);
        for (;;) {
            int64_t left;
            if (qp_fh_watchdog.cancel)
                break;
            left = deadline - (int64_t)qp_thread_monotonic_ns();
            if (left <= 0)
                break;
            qp_cond_timedwait(&qp_fh_watchdog.cond, &qp_fh_watchdog.mutex, left);
        }
        cancelled = qp_fh_watchdog.cancel;
        pthread_mutex_unlock(&qp_fh_watchdog.mutex);
        if (cancelled)
            break;
        /* timeout: dump the tracebacks */
        qp_fh_write(qp_fh_watchdog.fd, qp_fh_watchdog.header, qp_fh_watchdog.header_len);
        ok = qp_fh_dump_threads(qp_fh_watchdog.fd, qp_fh_watchdog.ctx, FALSE,
                                qp_fh_watchdog.max_threads) == NULL;
        if (qp_fh_watchdog.exit)
            _exit(1);
    } while (ok && qp_fh_watchdog.repeat);
    return NULL;
}

/* cancel_dump_traceback_later */
static void qp_fh_cancel_watchdog(JSContext *ctx)
{
    if (!qp_fh_watchdog.running)
        return;
    if (qp_fh_watchdog.pid == getpid()) {
        pthread_mutex_lock(&qp_fh_watchdog.mutex);
        qp_fh_watchdog.cancel = TRUE;
        pthread_cond_signal(&qp_fh_watchdog.cond);
        pthread_mutex_unlock(&qp_fh_watchdog.mutex);
        pthread_join(qp_fh_watchdog.thread, NULL);
    }
    /* else: a forked child, where the thread does not exist */
    qp_fh_watchdog.running = FALSE;
    qp_fh_watchdog.cancel = FALSE;
    qp_fh_clear_file(ctx, &qp_fh_watchdog.file);
}

/* format_timeout */
static void qp_fh_format_timeout(int64_t us, char *buf, size_t size)
{
    unsigned long sec, min, hour;
    sec = (unsigned long)(us / 1000000);
    us %= 1000000;
    min = sec / 60;
    sec %= 60;
    hour = min / 60;
    min %= 60;
    if (us != 0)
        snprintf(buf, size, "Timeout (%lu:%02lu:%02lu.%06u)!\n", hour, min, sec, (unsigned)us);
    else
        snprintf(buf, size, "Timeout (%lu:%02lu:%02lu)!\n", hour, min, sec);
}

/* ------------------------------------------------------------------------ */
/* Py_FatalError                                                             */
/* ------------------------------------------------------------------------ */

static void qp_faulthandler_fini(JSContext *ctx);

/* Does the calling thread run Python code of ctx (hold the GIL)? */
static BOOL qp_fh_holds_gil(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *ts;
    if (!thr)
        return qp_fh_single_rt != ctx->rt || qp_fh_single_ident == qp_thread_self_ident();
    ts = qp_fh_self_tstate(ctx);
    return ts && ts == __atomic_load_n(&thr->current, __ATOMIC_RELAXED);
}

/* _Py_FatalError_PrintExc: TRUE if the pending exception was printed with
   a traceback */
static BOOL qp_fh_fatal_print_exc(JSContext *ctx)
{
    JSValue exc, ferr, tb;
    BOOL has_tb;

    if (!JS_HasException(ctx))
        return FALSE;
    exc = qp_exc_fetch(ctx);
    ferr = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
    if (QP_IS_NULL(ferr) || JS_IsNull(ferr) || JS_IsUndefined(ferr) || JS_IsException(ferr)) {
        if (JS_IsException(ferr))
            qp_err_clear(ctx);
        JS_FreeValue(ctx, exc);
        return FALSE;
    }
    if (qp_exc_display(ctx, exc, QP_NULL) < 0)
        qp_err_clear(ctx);
    tb = qp_exc_get_traceback(ctx, exc);
    has_tb = !JS_IsException(tb) && !QP_IS_NULL(tb) && !JS_IsNull(tb) && !JS_IsUndefined(tb);
    if (JS_IsException(tb))
        qp_err_clear(ctx);
    else
        JS_FreeValue(ctx, tb);
    JS_FreeValue(ctx, exc);
    {
        JSValue r = qp_call_method(ctx, ferr, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
        if (JS_IsException(r))
            qp_err_clear(ctx);
        else
            JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, ferr);
    return has_tb;
}

/* fatal_error (Py_FatalError, _Py_FatalErrorFunc): write the message, the
   runtime state, the exception or the tracebacks of the threads and the
   extension modules to stderr, then abort().  ctx may be NULL. */
static void __attribute__((noreturn)) qp_fatal_error(JSContext *ctx, const char *func,
                                                     const char *msg)
{
    static int reentrant = 0;
    int fd = fileno(stderr);
    BOOL has_gil;

    if (reentrant)
        abort();    /* Py_FatalError() caused a second fatal error */
    reentrant = 1;
    fflush(stdout);
    fflush(stderr);
    qp_fh_puts(fd, "Fatal Python error: ");
    if (func) {
        qp_fh_puts(fd, func);
        qp_fh_puts(fd, ": ");
    }
    qp_fh_puts(fd, msg ? msg : "<message not set>");
    qp_fh_puts(fd, "\n");
    qp_fh_puts(fd, "Python runtime state: ");
    if (ctx && ctx->py && ctx->py->finalized) {
        QPThreads *thr = ctx->rt->py_threads;
        qp_fh_puts(fd, "finalizing (tstate=0x");
        qp_fh_dump_hex(fd, thr && thr->finalizing_ts ? (uintptr_t)thr->finalizing_ts
                       : (uintptr_t)ctx->py, (int)sizeof(void *) * 2, FALSE);
        qp_fh_puts(fd, ")");
    } else if (ctx && ctx->py) {
        qp_fh_puts(fd, "initialized");
    } else {
        qp_fh_puts(fd, "unknown");
    }
    qp_fh_puts(fd, "\n");
    has_gil = ctx && ctx->py && qp_fh_holds_gil(ctx);
    if (!has_gil || !qp_fh_fatal_print_exc(ctx)) {
        /* _Py_FatalError_DumpTracebacks */
        qp_fh_puts(fd, "\n");
        if (ctx && ctx->py)
            (void)qp_fh_dump_threads(fd, ctx, TRUE, 0);
    }
    qp_fh_dump_extension_modules(fd, ctx);
    if (has_gil) {
        /* the traceback was displayed: no second one on abort() */
        qp_faulthandler_fini(ctx);
        if (qp_sys_flush_std_files(ctx) < 0)
            qp_err_clear(ctx);
    }
    abort();
}

/* ------------------------------------------------------------------------ */
/* Module functions                                                          */
/* ------------------------------------------------------------------------ */

/* faulthandler_get_fileno: the file descriptor of file (sys.stderr when
   absent or None); *pfile gets a new reference to the file object (QP_NULL
   for a file descriptor).  -1 with an exception. */
static int qp_fh_get_fileno(JSContext *ctx, JSValueConst file, JSValue *pfile)
{
    JSValue f, r;
    int64_t v;
    int fd = -1, overflow = 0;

    *pfile = QP_NULL;
    if (QP_IS_NULL(file) || JS_IsNull(file) || JS_IsUndefined(file)) {
        f = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
        if (JS_IsException(f))
            return -1;
        if (QP_IS_NULL(f)) {
            qp_raise_runtime_error(ctx, "lost sys.stderr");
            return -1;
        }
        if (JS_IsNull(f) || JS_IsUndefined(f)) {
            JS_FreeValue(ctx, f);
            qp_raise_runtime_error(ctx, "sys.stderr is None");
            return -1;
        }
    } else if (qp_long_check(ctx, file)) {
        if (JS_IsBool(file) &&
            qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1, "bool is used as a file descriptor") < 0)
            return -1;
        if (qp_arg_int(ctx, file, &fd) < 0)
            return -1;
        if (fd < 0) {
            qp_raise_value_error(ctx, "file is not a valid file descriptor");
            return -1;
        }
        return fd;
    } else {
        f = JS_DupValue(ctx, file);
    }
    r = qp_call_method(ctx, f, QP_ATOM(ctx, fileno), NULL, 0, QP_NULL);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, f);
        return -1;
    }
    fd = -1;
    if (qp_long_check(ctx, r) && qp_long_as_int64_overflow(r, &v, &overflow) == 0 &&
        !overflow && v >= 0 && v < INT_MAX)
        fd = (int)v;
    JS_FreeValue(ctx, r);
    if (fd == -1) {
        if (JS_HasException(ctx))
            qp_err_clear(ctx);
        qp_raise_runtime_error(ctx, "file.fileno() is not a valid file descriptor");
        JS_FreeValue(ctx, f);
        return -1;
    }
    /* _PyFile_Flush: errors are ignored */
    r = qp_call_method(ctx, f, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
    if (JS_IsException(r))
        qp_err_clear(ctx);
    else
        JS_FreeValue(ctx, r);
    *pfile = f;
    return fd;
}

static int qp_fh_arg_bool(JSContext *ctx, JSValueConst v, int *out)
{
    return QP_IS_NULL(v) ? 0 : qp_arg_bool(ctx, v, out);
}

static int qp_fh_arg_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    return QP_IS_NULL(v) ? 0 : qp_arg_ssize(ctx, v, out);
}

/* dump_traceback(file=sys.stderr, all_threads=True, *, max_threads=100) */
static JSValue qp_fh_py_dump_traceback(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "file", "all_threads", "max_threads", NULL };
    QPArgSpec spec = { "dump_traceback", kw, 0, 2, 0 };
    JSValueConst a[3];
    JSValue file;
    int fd, all_threads = 1;
    Py_ssize_t max_threads = 100;
    const char *errmsg = NULL;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0 ||
        qp_fh_arg_bool(ctx, a[1], &all_threads) < 0 ||
        qp_fh_arg_ssize(ctx, a[2], &max_threads) < 0)
        return JS_EXCEPTION;
    fd = qp_fh_get_fileno(ctx, a[0], &file);
    if (fd < 0)
        return JS_EXCEPTION;
    qp_fh_note_thread(ctx);
    /* the other threads wait for the GIL: their frames do not change */
    if (all_threads)
        errmsg = qp_fh_dump_threads(fd, ctx, TRUE, max_threads);
    else
        qp_fh_dump_traceback_self(fd, ctx);
    qp_fh_clear_file(ctx, &file);
    if (errmsg)
        return qp_raise_runtime_error(ctx, "%s", errmsg);
    if (qp_signal_check(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* dump_c_stack(file=sys.stderr) */
static JSValue qp_fh_py_dump_c_stack(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "file", NULL };
    QPArgSpec spec = { "dump_c_stack", kw, 0, 1, 0 };
    JSValueConst a[1];
    JSValue file;
    int fd;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    fd = qp_fh_get_fileno(ctx, a[0], &file);
    if (fd < 0)
        return JS_EXCEPTION;
    qp_fh_dump_stack(fd);
    qp_fh_clear_file(ctx, &file);
    if (qp_signal_check(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* enable(file=sys.stderr, all_threads=True, c_stack=True, *, max_threads=100) */
static JSValue qp_fh_py_enable(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "file", "all_threads", "c_stack", "max_threads", NULL };
    QPArgSpec spec = { "enable", kw, 0, 3, 0 };
    JSValueConst a[4];
    JSValue file;
    int fd, all_threads = 1, c_stack = 1;
    Py_ssize_t max_threads = 100;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0 ||
        qp_fh_arg_bool(ctx, a[1], &all_threads) < 0 ||
        qp_fh_arg_bool(ctx, a[2], &c_stack) < 0 ||
        qp_fh_arg_ssize(ctx, a[3], &max_threads) < 0)
        return JS_EXCEPTION;
    fd = qp_fh_get_fileno(ctx, a[0], &file);
    if (fd < 0)
        return JS_EXCEPTION;
    qp_fh_note_thread(ctx);
    qp_fh_clear_file(ctx, &qp_fh_fatal.file);
    qp_fh_fatal.file = file;
    qp_fh_fatal.fd = fd;
    qp_fh_fatal.all_threads = all_threads;
    qp_fh_fatal.ctx = ctx;
    qp_fh_fatal.c_stack = c_stack;
    qp_fh_fatal.max_threads = max_threads;
    if (qp_fh_enable(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_fh_py_disable(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (!qp_fh_fatal.enabled)
        return JS_FALSE;
    qp_fh_disable(ctx);
    return JS_TRUE;
}

static JSValue qp_fh_py_is_enabled(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_fh_fatal.enabled);
}

/* dump_traceback_later(timeout, repeat=False, file=sys.stderr, exit=False,
   *, max_threads=100) */
static JSValue qp_fh_py_dump_traceback_later(JSContext *ctx, JSValueConst self,
                                             JSValueConst *args, int nargs,
                                             JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "timeout", "repeat", "file", "exit", "max_threads", NULL };
    QPArgSpec spec = { "dump_traceback_later", kw, 1, 4, 0 };
    JSValueConst a[5];
    JSValue file;
    int fd, repeat = 0, exit_ = 0, err;
    Py_ssize_t max_threads = 100;
    int64_t timeout_ns, timeout_us;
    pthread_attr_t attr;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_time_from_seconds_object(ctx, a[0], &timeout_ns, QP_TIME_ROUND_TIMEOUT) < 0)
        return JS_EXCEPTION;
    if (qp_fh_arg_bool(ctx, a[1], &repeat) < 0 ||
        qp_fh_arg_bool(ctx, a[3], &exit_) < 0 ||
        qp_fh_arg_ssize(ctx, a[4], &max_threads) < 0)
        return JS_EXCEPTION;
    /* _PyTime_AsMicroseconds(ROUND_TIMEOUT): away from zero */
    timeout_us = timeout_ns / 1000;
    if (timeout_ns % 1000 > 0)
        timeout_us++;
    else if (timeout_ns % 1000 < 0)
        timeout_us--;
    if (timeout_us <= 0)
        return qp_raise_value_error(ctx, "timeout must be greater than 0");
    /* PY_TIMEOUT_MAX, and LONG_MAX seconds for format_timeout() */
    if (timeout_us > INT64_MAX / 1000)
        return qp_raise_overflow_error(ctx, "timeout value is too large");
    fd = qp_fh_get_fileno(ctx, a[2], &file);
    if (fd < 0)
        return JS_EXCEPTION;
    qp_fh_note_thread(ctx);
    /* cancel the previous thread, if running */
    qp_fh_cancel_watchdog(ctx);
    if (!qp_fh_watchdog.sync_init) {
        pthread_mutex_init(&qp_fh_watchdog.mutex, NULL);
        qp_cond_init(&qp_fh_watchdog.cond);
        qp_fh_watchdog.sync_init = TRUE;
    }
    qp_fh_format_timeout(timeout_us, qp_fh_watchdog.header, sizeof(qp_fh_watchdog.header));
    qp_fh_watchdog.header_len = strlen(qp_fh_watchdog.header);
    qp_fh_watchdog.file = file;
    qp_fh_watchdog.fd = fd;
    qp_fh_watchdog.timeout_us = timeout_us;
    qp_fh_watchdog.repeat = repeat;
    qp_fh_watchdog.exit = exit_;
    qp_fh_watchdog.max_threads = max_threads;
    qp_fh_watchdog.ctx = ctx;
    qp_fh_watchdog.cancel = FALSE;
    qp_fh_watchdog.pid = getpid();
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    err = pthread_create(&qp_fh_watchdog.thread, &attr, qp_fh_watchdog_main, NULL);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        qp_fh_clear_file(ctx, &qp_fh_watchdog.file);
        return qp_raise_runtime_error(ctx, "unable to start watchdog thread");
    }
    qp_fh_watchdog.running = TRUE;
    return JS_UNDEFINED;
}

static JSValue qp_fh_py_cancel_dump_traceback_later(JSContext *ctx, JSValueConst self,
                                                    JSValueConst *args, int nargs,
                                                    JSValueConst kwnames, int magic)
{
    qp_fh_cancel_watchdog(ctx);
    return JS_UNDEFINED;
}

/* check_signum */
static int qp_fh_check_signum(JSContext *ctx, int signum)
{
    int i;
    for (i = 0; i < QP_FH_NSIGNALS; i++) {
        if (qp_fh_handlers[i].signum == signum) {
            qp_raise_runtime_error(ctx, "signal %d cannot be registered, use enable() instead",
                                   signum);
            return -1;
        }
    }
    if (signum < 1 || signum >= QP_NSIG) {
        qp_raise_value_error(ctx, "signal number out of range");
        return -1;
    }
    return 0;
}

/* register(signum, file=sys.stderr, all_threads=True, chain=False, *,
   max_threads=100) */
static JSValue qp_fh_py_register(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "signum", "file", "all_threads", "chain", "max_threads",
                                      NULL };
    QPArgSpec spec = { "register", kw, 1, 4, 0 };
    JSValueConst a[5];
    JSValue file;
    int signum, fd, all_threads = 1, chain = 0;
    Py_ssize_t max_threads = 100;
    QPFaultUser *u;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0 ||
        qp_arg_int(ctx, a[0], &signum) < 0 ||
        qp_fh_arg_bool(ctx, a[2], &all_threads) < 0 ||
        qp_fh_arg_bool(ctx, a[3], &chain) < 0 ||
        qp_fh_arg_ssize(ctx, a[4], &max_threads) < 0)
        return JS_EXCEPTION;
    if (qp_fh_check_signum(ctx, signum) < 0)
        return JS_EXCEPTION;
    fd = qp_fh_get_fileno(ctx, a[1], &file);
    if (fd < 0)
        return JS_EXCEPTION;
    qp_fh_note_thread(ctx);
    u = &qp_fh_user[signum];
    if (!u->enabled) {
        struct sigaction previous;
        if (qp_fh_allocate_stack(ctx) < 0) {
            qp_fh_clear_file(ctx, &file);
            return JS_EXCEPTION;
        }
        if (qp_fh_register_signal(signum, chain, &previous) != 0) {
            qp_raise_os_error(ctx, errno, QP_NULL);
            qp_fh_clear_file(ctx, &file);
            return JS_EXCEPTION;
        }
        u->previous = previous;
    }
    qp_fh_clear_file(ctx, &u->file);
    u->file = file;
    u->fd = fd;
    u->all_threads = all_threads;
    u->chain = chain;
    u->ctx = ctx;
    u->max_threads = max_threads;
    u->enabled = 1;
    return JS_UNDEFINED;
}

/* unregister(signum, /) */
static JSValue qp_fh_py_unregister(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    int signum;
    if (qp_arg_int(ctx, args[0], &signum) < 0 || qp_fh_check_signum(ctx, signum) < 0)
        return JS_EXCEPTION;
    return qp_bool_new(qp_fh_unregister(ctx, &qp_fh_user[signum], signum));
}

/* faulthandler_suppress_crash_report */
static void qp_fh_suppress_crash_report(void)
{
    struct rlimit rl;
    /* no core dump */
    if (getrlimit(RLIMIT_CORE, &rl) == 0) {
        rl.rlim_cur = 0;
        setrlimit(RLIMIT_CORE, &rl);
    }
}

/* _sigsegv(release_gil=False, /) */
static JSValue qp_fh_py_sigsegv(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    int release_gil = 0;
    if (qp_arg_check_positional(ctx, "_sigsegv", nargs, 0, 1) < 0 ||
        (nargs > 0 && qp_arg_bool(ctx, args[0], &release_gil) < 0))
        return JS_EXCEPTION;
    if (release_gil) {
        QP_BEGIN_ALLOW_THREADS(ctx);
        qp_fh_suppress_crash_report();
        raise(SIGSEGV);
        QP_END_ALLOW_THREADS(ctx);
    } else {
        qp_fh_suppress_crash_report();
        raise(SIGSEGV);
    }
    return JS_UNDEFINED;
}

static void *qp_fh_fatal_error_thread(void *arg)
{
    qp_fatal_error(arg, "faulthandler_fatal_error_thread", "in new thread");
}

/* _fatal_error_c_thread(): Py_FatalError() in a new C thread, waited for
   with the GIL held */
static JSValue qp_fh_py_fatal_error_c_thread(JSContext *ctx, JSValueConst self,
                                             JSValueConst *args, int nargs,
                                             JSValueConst kwnames, int magic)
{
    pthread_t t;
    qp_fh_suppress_crash_report();
    qp_fh_note_thread(ctx);
    if (pthread_create(&t, NULL, qp_fh_fatal_error_thread, ctx) != 0)
        return qp_raise_runtime_error(ctx, "unable to start the thread");
    /* never returns: Py_FatalError() exits the process */
    pthread_join(t, NULL);
    return JS_UNDEFINED;
}

static JSValue qp_fh_py_sigfpe(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    qp_fh_suppress_crash_report();
    raise(SIGFPE);
    abort();    /* Py_UNREACHABLE */
}

static JSValue qp_fh_py_sigabrt(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    qp_fh_suppress_crash_report();
    abort();
}

/* stack_overflow: at least 4096 bytes of stack per call (volatile: no tail
   call) */
static __attribute__((noinline)) uintptr_t qp_fh_stack_overflow(uintptr_t min_sp,
                                                                uintptr_t max_sp,
                                                                size_t *depth)
{
    volatile unsigned char buffer[4096];
    uintptr_t sp = (uintptr_t)&buffer;
    *depth += 1;
    if (sp < min_sp || max_sp < sp)
        return sp;
    buffer[0] = 1;
    buffer[4095] = 0;
    return qp_fh_stack_overflow(min_sp, max_sp, depth) + buffer[0] - 1;
}

static JSValue qp_fh_py_stack_overflow(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    size_t depth = 0, size;
    uintptr_t sp = (uintptr_t)&depth, stop, lower, upper;

    qp_fh_suppress_crash_report();
    lower = sp >= QP_FH_STACK_OVERFLOW_MAX_SIZE ? sp - QP_FH_STACK_OVERFLOW_MAX_SIZE : 0;
    upper = UINTPTR_MAX - QP_FH_STACK_OVERFLOW_MAX_SIZE >= sp ?
        sp + QP_FH_STACK_OVERFLOW_MAX_SIZE : UINTPTR_MAX;
    stop = qp_fh_stack_overflow(lower, upper, &depth);
    size = sp < stop ? stop - sp : sp - stop;
    return qp_raise_runtime_error(ctx, "unable to raise a stack overflow (allocated %zu bytes "
                                  "on the stack, %zu recursive calls)", size, depth);
}

/* Disable what ctx configured (_PyFaulthandler_Fini): Py_FinalizeEx and the
   release of the Python state. */
static void qp_faulthandler_fini(JSContext *ctx)
{
    int signum;
    BOOL any = FALSE;

    if (qp_fh_watchdog.running && qp_fh_watchdog.ctx == ctx)
        qp_fh_cancel_watchdog(ctx);
    for (signum = 1; signum < QP_NSIG; signum++) {
        QPFaultUser *u = &qp_fh_user[signum];
        if (u->ctx == ctx) {
            qp_fh_unregister(ctx, u, signum);
            u->ctx = NULL;
        }
        any |= u->enabled;
    }
    if (qp_fh_fatal.ctx == ctx) {
        qp_fh_disable(ctx);
        qp_fh_fatal.ctx = NULL;
    }
    if (!any && !qp_fh_fatal.enabled)
        qp_fh_free_stack();
    if (qp_fh_single_rt == ctx->rt)
        qp_fh_single_rt = NULL;
}

/* -X faulthandler / PYTHONFAULTHANDLER (_PyFaulthandler_Init):
   faulthandler.enable().  -1 with the exception pending. */
static int qp_faulthandler_init_enable(JSContext *ctx)
{
    JSValue mod, enable, r;
    JSAtom a;

    mod = qp_import_module(ctx, "faulthandler");
    if (JS_IsException(mod))
        return -1;
    a = JS_NewAtom(ctx, "enable");
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, mod);
        return -1;
    }
    enable = qp_getattr(ctx, mod, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(enable))
        return -1;
    r = qp_call0(ctx, enable);
    JS_FreeValue(ctx, enable);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_fh_exec(JSContext *ctx, JSValueConst mod)
{
    qp_fh_note_thread(ctx);
    return 0;
}

static const QPMethodDef qp_fh_methods[] = {
    { "enable", qp_fh_py_enable, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "enable($module, /, file=sys.stderr, all_threads=True, c_stack=True, *,\n"
      "       max_threads=100)\n"
      "--\n"
      "\n"
      "Enable the fault handler." },
    { "disable", qp_fh_py_disable, QP_METH_NOARGS, 0,
      "disable($module, /)\n"
      "--\n"
      "\n"
      "Disable the fault handler." },
    { "is_enabled", qp_fh_py_is_enabled, QP_METH_NOARGS, 0,
      "is_enabled($module, /)\n"
      "--\n"
      "\n"
      "Check if the handler is enabled." },
    { "dump_traceback", qp_fh_py_dump_traceback, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dump_traceback($module, /, file=sys.stderr, all_threads=True, *,\n"
      "               max_threads=100)\n"
      "--\n"
      "\n"
      "Dump the traceback of the current thread into file.\n"
      "\n"
      "Dump the traceback of all threads if all_threads is true. max_threads\n"
      "caps the number of threads dumped." },
    { "dump_c_stack", qp_fh_py_dump_c_stack, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dump_c_stack($module, /, file=sys.stderr)\n"
      "--\n"
      "\n"
      "Dump the C stack of the current thread." },
    { "dump_traceback_later", qp_fh_py_dump_traceback_later,
      QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dump_traceback_later($module, /, timeout, repeat=False,\n"
      "                     file=sys.stderr, exit=False, *, max_threads=100)\n"
      "--\n"
      "\n"
      "Dump the traceback of all threads in timeout seconds.\n"
      "\n"
      "If repeat is true, the tracebacks of all threads are dumped every\n"
      "timeout seconds.  If exit is true, call _exit(1) which is not safe.\n"
      "max_threads caps the number of threads dumped." },
    { "cancel_dump_traceback_later", qp_fh_py_cancel_dump_traceback_later, QP_METH_NOARGS, 0,
      "cancel_dump_traceback_later($module, /)\n"
      "--\n"
      "\n"
      "Cancel the previous call to dump_traceback_later()." },
    { "register", qp_fh_py_register, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "register($module, /, signum, file=sys.stderr, all_threads=True,\n"
      "         chain=False, *, max_threads=100)\n"
      "--\n"
      "\n"
      "Register a handler for the signal 'signum'.\n"
      "\n"
      "Dump the traceback of the current thread, or of all threads if\n"
      "all_threads is True, into file. max_threads caps the number of threads\n"
      "dumped." },
    { "unregister", qp_fh_py_unregister, QP_METH_O, 0,
      "unregister($module, signum, /)\n"
      "--\n"
      "\n"
      "Unregister the handler of the signal 'signum' registered by register()." },
    { "_sigsegv", qp_fh_py_sigsegv, QP_METH_FASTCALL, 0,
      "_sigsegv($module, release_gil=False, /)\n"
      "--\n"
      "\n"
      "Raise a SIGSEGV signal." },
    { "_fatal_error_c_thread", qp_fh_py_fatal_error_c_thread, QP_METH_NOARGS, 0,
      "_fatal_error_c_thread($module, /)\n"
      "--\n"
      "\n"
      "Call Py_FatalError() in a new C thread." },
    { "_sigabrt", qp_fh_py_sigabrt, QP_METH_NOARGS, 0,
      "_sigabrt($module, /)\n"
      "--\n"
      "\n"
      "Raise a SIGABRT signal." },
    { "_sigfpe", qp_fh_py_sigfpe, QP_METH_NOARGS, 0,
      "_sigfpe($module, /)\n"
      "--\n"
      "\n"
      "Raise a SIGFPE signal." },
    { "_stack_overflow", qp_fh_py_stack_overflow, QP_METH_NOARGS, 0,
      "_stack_overflow($module, /)\n"
      "--\n"
      "\n"
      "Recursive call to raise a stack overflow." },
    { NULL }
};

static const QPModuleDef qp_moddef_faulthandler = {
    "faulthandler",
    "faulthandler module.",
    qp_fh_methods,
    qp_fh_exec,
};
