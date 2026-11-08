/*
 * posix module (port of the POSIX parts of CPython Modules/posixmodule.c,
 * PSF-2.0) for macOS and Linux.
 *
 * The availability of each function follows CPython's configure results
 * on these platforms (QP_OS_HAVE_* below stand for pyconfig.h HAVE_*;
 * macOS functions newer than the deployment target are checked at run
 * time like HAVE_*_RUNTIME).  Not provided (absent exactly as on a
 * CPython build without them): the Windows-only functions, xattr, statx,
 * timerfd.  Blocking calls release the GIL; EINTR runs the Python signal
 * handlers and retries (PEP 475).  sys.audit hooks are not available to
 * native modules, so the os.* audit events are not raised.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/times.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <termios.h>
#include <signal.h>
#include <limits.h>
#include <langinfo.h>
#include <grp.h>
#include <math.h>
#include <sched.h>
#include <spawn.h>
#include <sysexits.h>
#include <dlfcn.h>
#include <sys/param.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <util.h>
#include <copyfile.h>
#else
#include <pty.h>
#include <utmp.h>
#endif
#ifdef __APPLE__
#include <crt_externs.h>
#define QP_OS_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define QP_OS_ENVIRON environ
#endif
#include <sys/random.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#endif

#if defined(__GLIBC__)
#define QP_OS_GLIBC_PREREQ(maj, min) \
    (__GLIBC__ > (maj) || (__GLIBC__ == (maj) && __GLIBC_MINOR__ >= (min)))
#else
#define QP_OS_GLIBC_PREREQ(maj, min) 0
#endif

/* pyconfig.h HAVE_* of the functions that are not on every POSIX system */
#ifdef __APPLE__
#define QP_OS_HAVE_CHFLAGS 1
#define QP_OS_HAVE_LCHFLAGS 1
#define QP_OS_HAVE_FCOPYFILE 1
#ifdef __MAC_27_0
#define QP_OS_HAVE_PIPE2 1
#define QP_OS_HAVE_PIPE2_RUNTIME __builtin_available(macOS 27.0, *)
#endif
#define QP_OS_HAVE_MKFIFOAT_RUNTIME __builtin_available(macOS 13.0, *)
#define QP_OS_HAVE_MKNODAT_RUNTIME __builtin_available(macOS 13.0, *)
#define QP_OS_HAVE_PTSNAME_R_RUNTIME __builtin_available(macOS 10.13.4, *)
#define QP_OS_HAVE_PWRITEV_RUNTIME __builtin_available(macOS 11.0, *)
#define QP_OS_HAVE_POSIX_SPAWN_SETSID_RUNTIME __builtin_available(macOS 10.15, *)
#endif /* __APPLE__ */
#ifdef __linux__
#define QP_OS_HAVE_PIPE2 1
#define QP_OS_HAVE_SCHED_SETSCHEDULER 1
#define QP_OS_HAVE_SCHED_SETPARAM 1
#define QP_OS_HAVE_SCHED_RR_GET_INTERVAL 1
#define QP_OS_HAVE_SCHED_SETAFFINITY 1
#define QP_OS_HAVE_SETRESUID 1
#define QP_OS_HAVE_POSIX_FALLOCATE 1
#define QP_OS_HAVE_POSIX_FADVISE 1
#define QP_OS_HAVE_CLEARENV 1
#define QP_OS_HAVE_SETNS 1
#define QP_OS_HAVE_UNSHARE 1
#define QP_OS_HAVE_SPLICE 1
#define QP_OS_HAVE_EVENTFD 1
#define QP_OS_HAVE_FUTIMESAT 1
#if defined(__NR_pidfd_open) && !(defined(__ANDROID__) && __ANDROID_API__ < 31)
#define QP_OS_HAVE_PIDFD_OPEN 1
#endif
#if QP_OS_GLIBC_PREREQ(2, 27) || !defined(__GLIBC__)
#define QP_OS_HAVE_COPY_FILE_RANGE 1
#define QP_OS_HAVE_MEMFD_CREATE 1
#endif
#if QP_OS_GLIBC_PREREQ(2, 26)
#define QP_OS_HAVE_PREADV2 1
#endif
#if QP_OS_GLIBC_PREREQ(2, 34)
#define QP_OS_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP 1
#endif
#endif /* __linux__ */
#ifndef QP_OS_HAVE_PIPE2_RUNTIME
#define QP_OS_HAVE_PIPE2_RUNTIME 1
#endif
#ifndef QP_OS_HAVE_MKFIFOAT_RUNTIME
#define QP_OS_HAVE_MKFIFOAT_RUNTIME 1
#define QP_OS_HAVE_MKNODAT_RUNTIME 1
#define QP_OS_HAVE_PTSNAME_R_RUNTIME 1
#define QP_OS_HAVE_PWRITEV_RUNTIME 1
#endif
#if defined(QP_OS_HAVE_SCHED_SETSCHEDULER) || defined(QP_OS_HAVE_SCHED_SETPARAM) || \
    defined(POSIX_SPAWN_SETSCHEDULER) || defined(POSIX_SPAWN_SETSCHEDPARAM)
#define QP_OS_HAVE_SCHED_PARAM 1
#endif

/* pyconfig.h equivalents */
#define QP_OS_HAVE_ST_BLKSIZE 1
#define QP_OS_HAVE_ST_BLOCKS 1
#define QP_OS_HAVE_ST_RDEV 1
#ifdef __APPLE__
#define QP_OS_HAVE_ST_FLAGS 1
#define QP_OS_HAVE_ST_GEN 1
#define QP_OS_HAVE_ST_BIRTHTIME 1
#define QP_OS_ST_ATIM st_atimespec
#define QP_OS_ST_MTIM st_mtimespec
#define QP_OS_ST_CTIM st_ctimespec
#else
#define QP_OS_ST_ATIM st_atim
#define QP_OS_ST_MTIM st_mtim
#define QP_OS_ST_CTIM st_ctim
#endif

#define QP_OS_DEFAULT_DIR_FD ((int)AT_FDCWD)

#define QP_OS_FN(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                        int nargs, JSValueConst kwnames, int magic)

/* hidden (JS-level, invisible to Python) properties of the posix module */
#define QP_OS_PROP_STAT     "\x01qp_os_stat_result"
#define QP_OS_PROP_STATVFS  "\x01qp_os_statvfs_result"
#define QP_OS_PROP_UNAME    "\x01qp_os_uname_result"
#define QP_OS_PROP_TERMSIZE "\x01qp_os_terminal_size"
#define QP_OS_PROP_TIMES    "\x01qp_os_times_result"
#define QP_OS_PROP_DIRENTRY "\x01qp_os_DirEntry"
#define QP_OS_PROP_SCANDIR  "\x01qp_os_ScandirIterator"
#define QP_OS_PROP_FORK_BEFORE "\x01qp_os_fork_before"
#define QP_OS_PROP_FORK_CHILD  "\x01qp_os_fork_child"
#define QP_OS_PROP_FORK_PARENT "\x01qp_os_fork_parent"

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */

/* _PyType_Name(Py_TYPE(o)) into buf */
static const char *qp_os_tname_buf(JSContext *ctx, JSValueConst o, char *buf, int size)
{
    QPType *t = qp_type_of(ctx, o);
    const char *n = t->tp_name, *dot;
    if (n) {
        dot = strrchr(n, '.');
        return dot ? dot + 1 : n;
    }
    return JS_AtomGetStr(ctx, buf, size, t->name);
}
#define qp_os_tname(ctx, o) qp_os_tname_buf((ctx), (o), (char[128]){0}, 128)

static QPType *qp_os_get_type(JSContext *ctx, JSValueConst module, const char *prop)
{
    JSValue v = JS_GetPropertyStr(ctx, module, prop);
    QPType *t;
    if (JS_IsException(v))
        return NULL;
    t = qp_type_from_value(v);
    JS_FreeValue(ctx, v);
    if (!t)
        qp_raise_system_error(ctx, "posix module state is missing");
    return t;
}

static JSValue qp_os_fsdecode(JSContext *ctx, const char *s, size_t len)
{
    return qp_str_decode(ctx, s, (Py_ssize_t)len, "utf-8", "surrogateescape");
}

/* result string of the same kind as the argument (bytes in, bytes out) */
static JSValue qp_os_name_value(JSContext *ctx, int as_bytes, const char *s, size_t len)
{
    if (as_bytes)
        return qp_bytes_new(ctx, s, (Py_ssize_t)len);
    return qp_os_fsdecode(ctx, s, len);
}

static JSValue qp_os_error(JSContext *ctx, int err)
{
    return qp_raise_os_error(ctx, err, QP_NULL);
}

/* PyErr_SetFromErrnoWithFilenameObjects */
static JSValue qp_os_error2(JSContext *ctx, int err, JSValueConst f1, JSValueConst f2)
{
    JSValueConst items[5];
    JSValue msg, exc;
    if (QP_IS_NULL(f2) || JS_IsUndefined(f2))
        return qp_raise_os_error(ctx, err, f1);
    msg = qp_str_from_cstr(ctx, err ? strerror(err) : "Error");
    if (JS_IsException(msg))
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, err);
    items[1] = msg;
    items[2] = f1;
    items[3] = JS_UNDEFINED;
    items[4] = f2;
    exc = qp_exc_new(ctx, QP_EXC(ctx, OSError), 5, items);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

static int qp_os_set(JSContext *ctx, JSValueConst module, const char *name, JSValue v)
{
    JSAtom at;
    int r;
    if (JS_IsException(v))
        return -1;
    at = JS_NewAtom(ctx, name);
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), at, v);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue qp_os_long(JSContext *ctx, int64_t v)
{
    return qp_long_from_int64(ctx, v);
}

/* PEP 475: after EINTR run signal handlers; -1 if one raised */
static int qp_os_eintr_retry(JSContext *ctx, int err)
{
    if (err != EINTR)
        return 0;
    if (qp_signal_check(ctx) < 0)
        return -1;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* converters                                                              */

/* PyLong_AsInt over __index__: "Python int too large to convert to C int" */
static int qp_os_int(JSContext *ctx, JSValueConst o, int *out)
{
    return qp_arg_int(ctx, o, out);
}

static int qp_os_int64(JSContext *ctx, JSValueConst o, int64_t *out)
{
    JSValue i = qp_index(ctx, o);
    int r;
    if (JS_IsException(i))
        return -1;
    r = qp_long_as_int64(ctx, i, out);
    JS_FreeValue(ctx, i);
    return r;
}

/* Py_off_t_converter */
static int qp_os_off(JSContext *ctx, JSValueConst o, off_t *out)
{
    int64_t v;
    if (qp_os_int64(ctx, o, &v) < 0)
        return -1;
    *out = (off_t)v;
    return 0;
}

/* _fd_converter */
static int qp_os_fd_conv(JSContext *ctx, JSValueConst o, int *p)
{
    JSValue index;
    int64_t v;
    int overflow = 0;
    if (JS_VALUE_GET_TAG(o) == JS_TAG_BOOL) {
        if (qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1, "bool is used as a file descriptor") < 0)
            return -1;
    }
    index = qp_index(ctx, o);
    if (JS_IsException(index))
        return -1;
    if (qp_long_as_int64_overflow(index, &v, &overflow) < 0)
        overflow = qp_long_sign(index);
    JS_FreeValue(ctx, index);
    if (overflow > 0 || (!overflow && v > INT_MAX)) {
        qp_raise_overflow_error(ctx, "fd is greater than maximum");
        return -1;
    }
    if (overflow < 0 || v < INT_MIN) {
        qp_raise_overflow_error(ctx, "fd is less than minimum");
        return -1;
    }
    *p = (int)v;
    return 0;
}

/* PyIndex_Check */
static BOOL qp_os_index_check(JSContext *ctx, JSValueConst o)
{
    JSValue m;
    if (qp_long_check(ctx, o))
        return TRUE;
    if (qp_float_check(ctx, o) || qp_str_check(ctx, o))
        return FALSE;
    m = qp_type_lookup(ctx, qp_type_of(ctx, o), QP_ATOM(ctx, __index__));
    return !QP_IS_NULL(m);
}

/* dir_fd_converter */
static int qp_os_dir_fd_conv(JSContext *ctx, JSValueConst o, int *p)
{
    if (QP_IS_NULL(o) || JS_IsUndefined(o) || JS_IsNull(o)) {
        *p = QP_OS_DEFAULT_DIR_FD;
        return 0;
    }
    if (qp_os_index_check(ctx, o))
        return qp_os_fd_conv(ctx, o, p);
    qp_raise_type_error(ctx, "argument should be integer or None, not %s", qp_os_tname(ctx, o));
    return -1;
}

/* PyObject_AsFileDescriptor (also used by the select module) */
static int qp_object_as_fd(JSContext *ctx, JSValueConst o, int *pfd)
{
    int fd;
    if (qp_long_check(ctx, o)) {
        if (qp_arg_int(ctx, o, &fd) < 0)
            return -1;
    } else {
        JSValue meth, res;
        int r = qp_getattr_opt(ctx, o, QP_ATOM(ctx, fileno), &meth);
        if (r < 0)
            return -1;
        if (r == 0) {
            qp_raise_type_error(ctx, "argument must be an int, or have a fileno() method.");
            return -1;
        }
        res = qp_call0(ctx, meth);
        JS_FreeValue(ctx, meth);
        if (JS_IsException(res))
            return -1;
        if (!qp_long_check(ctx, res)) {
            JS_FreeValue(ctx, res);
            qp_raise_type_error(ctx, "fileno() returned a non-integer");
            return -1;
        }
        r = qp_arg_int(ctx, res, &fd);
        JS_FreeValue(ctx, res);
        if (r < 0)
            return -1;
    }
    if (fd < 0) {
        qp_raise_value_error(ctx, "file descriptor cannot be a negative integer (%d)", fd);
        return -1;
    }
    *pfd = fd;
    return 0;
}

/* _Py_Uid_Converter / _Py_Gid_Converter (uid_t and gid_t are 32-bit
   unsigned on macOS and Linux; -1 means "unchanged") */
static int qp_os_id_conv(JSContext *ctx, JSValueConst o, uint32_t *out, const char *what)
{
    JSValue index;
    int64_t v;
    int overflow = 0;
    if (qp_float_check(ctx, o) || !qp_os_index_check(ctx, o)) {
        qp_raise_type_error(ctx, "%s should be integer, not %s", what, qp_os_tname(ctx, o));
        return -1;
    }
    index = qp_index(ctx, o);
    if (JS_IsException(index))
        return -1;
    if (qp_long_as_int64_overflow(index, &v, &overflow) < 0)
        overflow = qp_long_sign(index);
    JS_FreeValue(ctx, index);
    /* CPython with a 64-bit long: a value that fits a long must be -1 or
       survive the truncation to uid_t (so 2**32-1 is (uid_t)-1), others
       "underflow"; beyond a long (an unsigned long at most) the value
       never fits: "overflow" */
    if (overflow < 0 || (!overflow && (v < -1 || v > (int64_t)UINT32_MAX))) {
        qp_raise_overflow_error(ctx, "%s is less than minimum", what);
        return -1;
    }
    if (overflow > 0) {
        qp_raise_overflow_error(ctx, "%s is greater than maximum", what);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* _PyLong_FromUid: (uid_t)-1 -> -1 */
static JSValue qp_os_from_id(JSContext *ctx, uint32_t id)
{
    if (id == (uint32_t)-1)
        return JS_NewInt32(ctx, -1);
    return qp_long_from_uint64(ctx, id);
}

/* _PyLong_FromDev */
static JSValue qp_os_from_dev(JSContext *ctx, dev_t d)
{
    if (d == (dev_t)-1)
        return JS_NewInt32(ctx, -1);
    return qp_long_from_uint64(ctx, (uint64_t)d);
}

/* path_t */
typedef struct QPOSPath {
    const char *function_name;
    const char *argument_name;
    int nullable;
    int allow_fd;
    int nonstrict;
    /* outputs */
    char *narrow;           /* js_malloc'd NUL-terminated copy, NULL for None/fd */
    Py_ssize_t length;
    int fd;
    int is_fd;
    int is_bytes;           /* path given as bytes (results are bytes) */
    JSValue object;         /* owned: the argument (fspath() result for PathLike) */
} QPOSPath;

#define QP_OS_PATH(fn, arg, nullable, allow_fd) \
    { fn, arg, nullable, allow_fd, 0, NULL, 0, -1, 0, 0, QP_NULL }

static void qp_os_path_clean(JSContext *ctx, QPOSPath *p)
{
    js_free(ctx, p->narrow);
    p->narrow = NULL;
    JS_FreeValue(ctx, p->object);
    p->object = QP_NULL;
}


/* path_converter (narrow variant).  `o` may be QP_NULL (argument absent:
   treated like None). */
static int qp_os_path_conv(JSContext *ctx, JSValueConst o, QPOSPath *path)
{
    JSValue obj, bytes = JS_UNDEFINED;
    int is_unicode, is_bytes, is_index;
    const uint8_t *data;
    Py_ssize_t len;
    const char *fn = path->function_name, *an = path->argument_name ? path->argument_name : "path";

    if (QP_IS_NULL(o))
        o = JS_UNDEFINED;
    if ((JS_IsUndefined(o) || JS_IsNull(o)) && path->nullable) {
        path->narrow = NULL;
        path->fd = -1;
        path->object = JS_UNDEFINED;
        path->length = 0;
        return 0;
    }
    obj = JS_DupValue(ctx, o);
    is_index = path->allow_fd && qp_os_index_check(ctx, obj);
    is_bytes = qp_bytes_check(ctx, obj);
    is_unicode = qp_str_check(ctx, obj);
    if (!is_index && !is_unicode && !is_bytes) {
        /* inline PyOS_FSPath() for better error messages */
        JSValue func = qp_lookup_special(ctx, obj, QP_ATOM(ctx, __fspath__)), res;
        if (JS_IsException(func))
            goto error;
        if (QP_IS_NULL(func) || JS_IsUndefined(func)) {
            JS_FreeValue(ctx, func);
            goto error_format;
        }
        res = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(res))
            goto error;
        if (qp_str_check(ctx, res)) {
            is_unicode = 1;
        } else if (qp_bytes_check(ctx, res)) {
            is_bytes = 1;
        } else {
            qp_raise_type_error(ctx, "expected %s.__fspath__() to return str or bytes, not %s",
                                qp_os_tname(ctx, obj), qp_os_tname(ctx, res));
            JS_FreeValue(ctx, res);
            goto error;
        }
        JS_FreeValue(ctx, obj);
        obj = res;
    }
    if (is_unicode) {
        bytes = qp_str_encode(ctx, obj, "utf-8", "surrogateescape");
        if (JS_IsException(bytes))
            goto error;
    } else if (is_bytes) {
        bytes = JS_DupValue(ctx, obj);
    } else if (is_index) {
        if (qp_os_fd_conv(ctx, obj, &path->fd) < 0)
            goto error;
        path->narrow = NULL;
        path->is_fd = 1;
        path->length = 0;
        path->object = obj;
        return 0;
    } else {
    error_format:
        qp_raise_type_error(ctx, "%s%s%s should be %s, not %s",
                            fn ? fn : "", fn ? ": " : "", an,
                            path->allow_fd && path->nullable ? "string, bytes, os.PathLike, integer or None" :
                            path->allow_fd ? "string, bytes, os.PathLike or integer" :
                            path->nullable ? "string, bytes, os.PathLike or None" :
                            "string, bytes or os.PathLike",
                            qp_os_tname(ctx, obj));
        goto error;
    }
    data = qp_bytes_data(bytes, &len);
    if (!path->nonstrict && memchr(data, 0, len)) {
        qp_raise_value_error(ctx, "%s%sembedded null character in %s", fn ? fn : "", fn ? ": " : "", an);
        goto error;
    }
    path->narrow = js_malloc(ctx, len + 1);
    if (!path->narrow)
        goto error;
    memcpy(path->narrow, data, len);
    path->narrow[len] = '\0';
    JS_FreeValue(ctx, bytes);
    path->length = len;
    path->fd = -1;
    path->is_bytes = is_bytes && !is_unicode;
    path->object = obj;
    return 0;
error:
    JS_FreeValue(ctx, bytes);
    JS_FreeValue(ctx, obj);
    return -1;
}

static JSValue qp_os_path_error(JSContext *ctx, QPOSPath *p, int err)
{
    return qp_raise_os_error(ctx, err, p->object);
}

static JSValue qp_os_path_error2(JSContext *ctx, QPOSPath *p, QPOSPath *p2, int err)
{
    return qp_os_error2(ctx, err, p->object, p2->object);
}

/* PyUnicode_FSConverter: str/bytes/PathLike -> NUL-free bytes (new ref) */
static JSValue qp_os_fsconvert(JSContext *ctx, JSValueConst o)
{
    QPOSPath p = QP_OS_PATH(NULL, NULL, 0, 0);
    JSValue r;
    p.nonstrict = 1;
    if (qp_os_path_conv(ctx, o, &p) < 0) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            qp_raise_type_error(ctx, "expected str, bytes or os.PathLike object, not %s", qp_os_tname(ctx, o));
        }
        return JS_EXCEPTION;
    }
    if (memchr(p.narrow, 0, p.length)) {
        qp_os_path_clean(ctx, &p);
        return qp_raise_value_error(ctx, "embedded null byte");
    }
    r = qp_bytes_new(ctx, p.narrow, p.length);
    qp_os_path_clean(ctx, &p);
    return r;
}

static int qp_os_unavailable(JSContext *ctx, const char *fn, const char *arg)
{
    qp_raise(ctx, QP_EXC(ctx, NotImplementedError), "%s%s%s unavailable on this platform",
             fn ? fn : "", fn ? ": " : "", arg);
    return -1;
}

static int qp_os_path_and_dir_fd_invalid(JSContext *ctx, const char *fn, QPOSPath *p, int dir_fd)
{
    if (dir_fd != QP_OS_DEFAULT_DIR_FD && !p->narrow) {
        qp_raise_value_error(ctx, "%s: can't specify dir_fd without matching path", fn);
        return 1;
    }
    return 0;
}

static int qp_os_dir_fd_and_fd_invalid(JSContext *ctx, const char *fn, int dir_fd, int fd)
{
    if (dir_fd != QP_OS_DEFAULT_DIR_FD && fd != -1) {
        qp_raise_value_error(ctx, "%s: can't specify both dir_fd and fd", fn);
        return 1;
    }
    return 0;
}

static int qp_os_fd_and_follow_invalid(JSContext *ctx, const char *fn, int is_fd, int follow)
{
    if (is_fd && !follow) {
        qp_raise_value_error(ctx, "%s: cannot use fd and follow_symlinks together", fn);
        return 1;
    }
    return 0;
}

static int qp_os_dir_fd_and_follow_invalid(JSContext *ctx, const char *fn, int dir_fd, int follow)
{
    if (dir_fd != QP_OS_DEFAULT_DIR_FD && !follow) {
        qp_raise_value_error(ctx, "%s: cannot use dir_fd and follow_symlinks together", fn);
        return 1;
    }
    return 0;
}

/* optional bool argument (QP_NULL = default) */
static int qp_os_opt_bool(JSContext *ctx, JSValueConst v, int dflt, int *out)
{
    if (QP_IS_NULL(v)) {
        *out = dflt;
        return 0;
    }
    return qp_arg_bool(ctx, v, out);
}

static int qp_os_opt_int(JSContext *ctx, JSValueConst v, int dflt, int *out)
{
    if (QP_IS_NULL(v)) {
        *out = dflt;
        return 0;
    }
    return qp_arg_int(ctx, v, out);
}

/* ---------------------------------------------------------------------- */
/* struct sequences                                                        */

static const char *const qp_os_stat_result_fields[] = {
    "st_mode",
    "st_ino",
    "st_dev",
    "st_nlink",
    "st_uid",
    "st_gid",
    "st_size",
    QP_STRUCTSEQ_UNNAMED,
    QP_STRUCTSEQ_UNNAMED,
    QP_STRUCTSEQ_UNNAMED,
    "st_atime",
    "st_mtime",
    "st_ctime",
    "st_atime_ns",
    "st_mtime_ns",
    "st_ctime_ns",
#ifdef QP_OS_HAVE_ST_BLKSIZE
    "st_blksize",
#endif
#ifdef QP_OS_HAVE_ST_BLOCKS
    "st_blocks",
#endif
#ifdef QP_OS_HAVE_ST_RDEV
    "st_rdev",
#endif
#ifdef QP_OS_HAVE_ST_FLAGS
    "st_flags",
#endif
#ifdef QP_OS_HAVE_ST_GEN
    "st_gen",
#endif
#if defined(QP_OS_HAVE_ST_BIRTHTIME) || defined(MS_WINDOWS)
    "st_birthtime",
#endif
#ifdef MS_WINDOWS
    "st_birthtime_ns",
#endif
#ifdef QP_OS_HAVE_ST_FILE_ATTRIBUTES
    "st_file_attributes",
#endif
#ifdef QP_OS_HAVE_ST_FSTYPE
    "st_fstype",
#endif
#ifdef QP_OS_HAVE_ST_REPARSE_TAG
    "st_reparse_tag",
#endif
    NULL
};
static const char *const qp_os_stat_result_docs[] = {
    "protection bits",
    "inode",
    "device",
    "number of hard links",
    "user ID of owner",
    "group ID of owner",
    "total size, in bytes",
    "integer time of last access",
    "integer time of last modification",
    "integer time of last change",
    "time of last access",
    "time of last modification",
    "time of last change",
    "time of last access in nanoseconds",
    "time of last modification in nanoseconds",
    "time of last change in nanoseconds",
#ifdef QP_OS_HAVE_ST_BLKSIZE
    "blocksize for filesystem I/O",
#endif
#ifdef QP_OS_HAVE_ST_BLOCKS
    "number of blocks allocated",
#endif
#ifdef QP_OS_HAVE_ST_RDEV
    "device type (if inode device)",
#endif
#ifdef QP_OS_HAVE_ST_FLAGS
    "user defined flags for file",
#endif
#ifdef QP_OS_HAVE_ST_GEN
    "generation number",
#endif
#if defined(QP_OS_HAVE_ST_BIRTHTIME) || defined(MS_WINDOWS)
    "time of creation",
#endif
#ifdef MS_WINDOWS
    "time of creation in nanoseconds",
#endif
#ifdef QP_OS_HAVE_ST_FILE_ATTRIBUTES
    "Windows file attribute bits",
#endif
#ifdef QP_OS_HAVE_ST_FSTYPE
    "Type of filesystem",
#endif
#ifdef QP_OS_HAVE_ST_REPARSE_TAG
    "Windows reparse tag",
#endif
    NULL
};

static const char *const qp_os_statvfs_result_fields[] = {
    "f_bsize",
    "f_frsize",
    "f_blocks",
    "f_bfree",
    "f_bavail",
    "f_files",
    "f_ffree",
    "f_favail",
    "f_flag",
    "f_namemax",
    "f_fsid",
    NULL
};
static const char *const qp_os_statvfs_result_docs[] = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static const char *const qp_os_uname_result_fields[] = {
    "sysname",
    "nodename",
    "release",
    "version",
    "machine",
    NULL
};
static const char *const qp_os_uname_result_docs[] = {
    "operating system name",
    "name of machine on network (implementation-defined)",
    "operating system release",
    "operating system version",
    "hardware identifier",
    NULL
};

static const char *const qp_os_terminal_size_fields[] = {
    "columns",
    "lines",
    NULL
};
static const char *const qp_os_terminal_size_docs[] = {
    "width of the terminal window in characters",
    "height of the terminal window in characters",
    NULL
};

static const char *const qp_os_times_result_fields[] = {
    "user",
    "system",
    "children_user",
    "children_system",
    "elapsed",
    NULL
};
static const char *const qp_os_times_result_docs[] = {
    "user time",
    "system time",
    "user time of children",
    "system time of children",
    "elapsed time since an arbitrary point in the past",
    NULL
};


static const QPStructSeqDesc qp_os_stat_result_desc = {
    "os.stat_result", "stat_result: Result from stat, fstat, or lstat.\n\n\
This object may be accessed either as a tuple of\n\
  (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime)\n\
or via the attributes st_mode, st_ino, st_dev, st_nlink, and so on.\n\
\n\
Posix/windows: If your platform supports st_blksize, st_blocks, st_rdev,\n\
or st_flags, they are available as attributes only.\n\
\n\
See os.stat for more information.",
    qp_os_stat_result_fields, qp_os_stat_result_docs, 10, 0
};
static const QPStructSeqDesc qp_os_statvfs_result_desc = {
    "os.statvfs_result", "statvfs_result: Result from statvfs or fstatvfs.\n\n\
This object may be accessed either as a tuple of\n\
  (bsize, frsize, blocks, bfree, bavail, files, ffree, favail, flag, namemax),\n\
or via the attributes f_bsize, f_frsize, f_blocks, f_bfree, and so on.\n\
\n\
See os.statvfs for more information.",
    qp_os_statvfs_result_fields, NULL, 10, 0
};
static const QPStructSeqDesc qp_os_uname_result_desc = {
    "posix.uname_result", "uname_result: Result from os.uname().\n\n\
This object may be accessed either as a tuple of\n\
  (sysname, nodename, release, version, machine),\n\
or via the attributes sysname, nodename, release, version, and machine.\n\
\n\
See os.uname for more information.",
    qp_os_uname_result_fields, qp_os_uname_result_docs, 5, 0
};
static const QPStructSeqDesc qp_os_terminal_size_desc = {
    "os.terminal_size", "A tuple of (columns, lines) for holding terminal window size",
    qp_os_terminal_size_fields, qp_os_terminal_size_docs, 2, 0
};
static const QPStructSeqDesc qp_os_times_result_desc = {
    "posix.times_result", "times_result: Result from os.times().\n\n\
This object may be accessed either as a tuple of\n\
  (user, system, children_user, children_system, elapsed),\n\
or via the attributes user, system, children_user, children_system,\n\
and elapsed.\n\
\n\
See os.times for more information.",
    qp_os_times_result_fields, qp_os_times_result_docs, 5, 0
};

enum {
    QP_OS_ST_BASE = 16 - 1,
#ifdef QP_OS_HAVE_ST_BLKSIZE
    QP_OS_ST_BLKSIZE_IDX,
#endif
#ifdef QP_OS_HAVE_ST_BLOCKS
    QP_OS_ST_BLOCKS_IDX,
#endif
#ifdef QP_OS_HAVE_ST_RDEV
    QP_OS_ST_RDEV_IDX,
#endif
#ifdef QP_OS_HAVE_ST_FLAGS
    QP_OS_ST_FLAGS_IDX,
#endif
#ifdef QP_OS_HAVE_ST_GEN
    QP_OS_ST_GEN_IDX,
#endif
#ifdef QP_OS_HAVE_ST_BIRTHTIME
    QP_OS_ST_BIRTHTIME_IDX,
#endif
    QP_OS_ST_NFIELDS_M1
};
#define QP_OS_ST_NFIELDS (QP_OS_ST_NFIELDS_M1)

/* stat_nanosecond_timestamp: sec * 10**9 + nsec */
static JSValue qp_os_ns(JSContext *ctx, int64_t sec, long nsec)
{
    if (sec > -9200000000LL && sec < 9200000000LL)
        return qp_long_from_int64(ctx, sec * 1000000000LL + nsec);
    {
        JSValue a = qp_long_from_int64(ctx, sec), b, c, r;
        b = qp_long_from_int64(ctx, 1000000000LL);
        c = qp_binop(ctx, a, b, QP_NB_MULTIPLY);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (JS_IsException(c))
            return c;
        a = qp_long_from_int64(ctx, nsec);
        r = qp_binop(ctx, c, a, QP_NB_ADD);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, c);
        return r;
    }
}

/* _pystat_fromstructstat */
static JSValue qp_os_stat_from(JSContext *ctx, JSValueConst module, const struct stat *st)
{
    JSValue items[QP_OS_ST_NFIELDS];
    QPType *t = qp_os_get_type(ctx, module, QP_OS_PROP_STAT);
    int i;
    if (!t)
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, (int32_t)st->st_mode);
    items[1] = qp_long_from_uint64(ctx, (uint64_t)st->st_ino);
    items[2] = qp_os_from_dev(ctx, st->st_dev);
    items[3] = qp_long_from_int64(ctx, (int64_t)st->st_nlink);
    items[4] = qp_os_from_id(ctx, st->st_uid);
    items[5] = qp_os_from_id(ctx, st->st_gid);
    items[6] = qp_long_from_int64(ctx, (int64_t)st->st_size);
    {
        const struct timespec *ts[3] = { &st->QP_OS_ST_ATIM, &st->QP_OS_ST_MTIM, &st->QP_OS_ST_CTIM };
        for (i = 0; i < 3; i++) {
            items[7 + i] = qp_long_from_int64(ctx, (int64_t)ts[i]->tv_sec);
            items[10 + i] = qp_float_new(ctx, (double)ts[i]->tv_sec + 1e-9 * ts[i]->tv_nsec);
            items[13 + i] = qp_os_ns(ctx, (int64_t)ts[i]->tv_sec, ts[i]->tv_nsec);
        }
    }
#ifdef QP_OS_HAVE_ST_BLKSIZE
    items[QP_OS_ST_BLKSIZE_IDX] = qp_long_from_int64(ctx, (int64_t)st->st_blksize);
#endif
#ifdef QP_OS_HAVE_ST_BLOCKS
    items[QP_OS_ST_BLOCKS_IDX] = qp_long_from_int64(ctx, (int64_t)st->st_blocks);
#endif
#ifdef QP_OS_HAVE_ST_RDEV
    items[QP_OS_ST_RDEV_IDX] = qp_os_from_dev(ctx, st->st_rdev);
#endif
#ifdef QP_OS_HAVE_ST_FLAGS
    items[QP_OS_ST_FLAGS_IDX] = qp_long_from_int64(ctx, (int64_t)st->st_flags);
#endif
#ifdef QP_OS_HAVE_ST_GEN
    items[QP_OS_ST_GEN_IDX] = qp_long_from_int64(ctx, (int64_t)st->st_gen);
#endif
#ifdef QP_OS_HAVE_ST_BIRTHTIME
    items[QP_OS_ST_BIRTHTIME_IDX] = qp_float_new(ctx, (double)(long)st->st_birthtimespec.tv_sec +
                                                  st->st_birthtimespec.tv_nsec * 1e-9);
#endif
    for (i = 0; i < QP_OS_ST_NFIELDS; i++) {
        if (JS_IsException(items[i])) {
            for (i = 0; i < QP_OS_ST_NFIELDS; i++)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    return qp_structseq_new(ctx, t, items);
}

/* statresult_new: st_?time default to the integer fields */
QP_OS_FN(qp_os_statresult_new)
{
    JSValue res = qp_ss_tp_new(ctx, self, args, nargs, kwnames, magic);
    int i;
    if (JS_IsException(res))
        return res;
    for (i = 7; i <= 9; i++) {
        JSValue v = qp_ss_get(ctx, res, i + 3);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, res);
            return v;
        }
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            if (qp_ss_set(ctx, res, i + 3, qp_ss_get(ctx, res, i)) < 0) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }
    return res;
}

/* ---------------------------------------------------------------------- */
/* argument parsing                                                        */

#define QP_OS_PARSE(fname, minargs, maxpos, ...)                              \
    static const char *const qp_kw_[] = { __VA_ARGS__, NULL };                \
    static const QPArgSpec qp_spec_ = { fname, qp_kw_, minargs, maxpos, 0 };  \
    JSValueConst a[countof(qp_kw_)];                                          \
    if (qp_arg_parse(ctx, &qp_spec_, args, nargs, kwnames, a) < 0)            \
        return JS_EXCEPTION

/* ---------------------------------------------------------------------- */
/* stat family                                                             */

static JSValue qp_os_do_stat(JSContext *ctx, JSValueConst module, const char *fn,
                             QPOSPath *path, int dir_fd, int follow)
{
    struct stat st;
    int result, err;
    if (qp_os_path_and_dir_fd_invalid(ctx, "stat", path, dir_fd) ||
        qp_os_dir_fd_and_fd_invalid(ctx, "stat", dir_fd, path->fd) ||
        qp_os_fd_and_follow_invalid(ctx, "stat", path->is_fd, follow))
        return JS_EXCEPTION;
    if (path->is_fd)
        result = fstat(path->fd, &st);
    else if (!follow && dir_fd == QP_OS_DEFAULT_DIR_FD)
        result = lstat(path->narrow, &st);
    else if (dir_fd != QP_OS_DEFAULT_DIR_FD || !follow)
        result = fstatat(dir_fd, path->narrow, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = stat(path->narrow, &st);
    err = errno;
    if (result != 0)
        return qp_os_path_error(ctx, path, err);
    return qp_os_stat_from(ctx, module, &st);
}

QP_OS_FN(qp_os_stat)
{
    QPOSPath path = QP_OS_PATH("stat", "path", 0, 1);
    int dir_fd, follow;
    JSValue r;
    QP_OS_PARSE("stat", 1, 1, "path", "dir_fd", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_dir_fd_conv(ctx, a[1], &dir_fd) < 0 || qp_os_opt_bool(ctx, a[2], 1, &follow) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    r = qp_os_do_stat(ctx, self, "stat", &path, dir_fd, follow);
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_lstat)
{
    QPOSPath path = QP_OS_PATH("lstat", "path", 0, 0);
    int dir_fd;
    JSValue r;
    QP_OS_PARSE("lstat", 1, 1, "path", "dir_fd");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_dir_fd_conv(ctx, a[1], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    r = qp_os_do_stat(ctx, self, "lstat", &path, dir_fd, 0);
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_fstat)
{
    struct stat st;
    int fd, res, err;
    QP_OS_PARSE("fstat", 1, 1, "fd");
    if (qp_os_int(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    for (;;) {
        res = fstat(fd, &st);
        err = errno;
        if (res == 0)
            break;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
    return qp_os_stat_from(ctx, self, &st);
}

QP_OS_FN(qp_os_access)
{
    QPOSPath path = QP_OS_PATH("access", "path", 0, 0);
    int mode, dir_fd, effective_ids, follow, result, flags = 0;
    QP_OS_PARSE("access", 2, 2, "path", "mode", "dir_fd", "effective_ids", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_int(ctx, a[1], &mode) < 0 || qp_os_dir_fd_conv(ctx, a[2], &dir_fd) < 0 ||
        qp_os_opt_bool(ctx, a[3], 0, &effective_ids) < 0 || qp_os_opt_bool(ctx, a[4], 1, &follow) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    if (!follow)
        flags |= AT_SYMLINK_NOFOLLOW;
    if (effective_ids)
        flags |= AT_EACCESS;
    if (dir_fd != QP_OS_DEFAULT_DIR_FD || effective_ids || !follow)
        result = faccessat(dir_fd, path.narrow, mode, flags);
    else
        result = access(path.narrow, mode);
    qp_os_path_clean(ctx, &path);
    return qp_bool_new(result == 0);
}

QP_OS_FN(qp_os_chdir)
{
    QPOSPath path = QP_OS_PATH("chdir", "path", 0, 1);
    int result, err;
    QP_OS_PARSE("chdir", 1, 1, "path");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    result = path.is_fd ? fchdir(path.fd) : chdir(path.narrow);
    err = errno;
    if (result) {
        JSValue r = qp_os_path_error(ctx, &path, err);
        qp_os_path_clean(ctx, &path);
        return r;
    }
    qp_os_path_clean(ctx, &path);
    return JS_UNDEFINED;
}

/* fildes converter + call */
QP_OS_FN(qp_os_fchdir)
{
    int fd;
    QP_OS_PARSE("fchdir", 1, 1, "fd");
    if (qp_object_as_fd(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res = fchdir(fd), err = errno;
        if (res == 0)
            return JS_UNDEFINED;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
}

QP_OS_FN(qp_os_chmod)
{
    QPOSPath path = QP_OS_PATH("chmod", "path", 0, 1);
    int mode, dir_fd, follow, result, err = 0;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("chmod", 2, 2, "path", "mode", "dir_fd", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_int(ctx, a[1], &mode) < 0 || qp_os_dir_fd_conv(ctx, a[2], &dir_fd) < 0 ||
        qp_os_opt_bool(ctx, a[3], 1, &follow) < 0) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (qp_os_path_and_dir_fd_invalid(ctx, "chmod", &path, dir_fd) ||
        qp_os_dir_fd_and_fd_invalid(ctx, "chmod", dir_fd, path.fd) ||
        qp_os_fd_and_follow_invalid(ctx, "chmod", path.is_fd, follow)) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (path.is_fd) {
        do {
            result = fchmod(path.fd, (mode_t)mode);
            err = errno;
        } while (result && err == EINTR && qp_signal_check(ctx) == 0);
        if (result && err == EINTR) {
            r = JS_EXCEPTION;
            goto done;
        }
    }
#ifdef __APPLE__
    else if (!follow && dir_fd == QP_OS_DEFAULT_DIR_FD) {
        result = lchmod(path.narrow, (mode_t)mode);
        err = errno;
    }
#endif
    else if (!follow || dir_fd != QP_OS_DEFAULT_DIR_FD) {
        result = fchmodat(dir_fd, path.narrow, (mode_t)mode, follow ? 0 : AT_SYMLINK_NOFOLLOW);
        err = errno;
        /* fchmodat(AT_SYMLINK_NOFOLLOW) is not supported for symlinks
           on Linux (CPython: fchmodat_nofollow_unsupported) */
        if (result && !follow && (err == ENOTSUP || err == EOPNOTSUPP)) {
            if (dir_fd != QP_OS_DEFAULT_DIR_FD)
                qp_os_dir_fd_and_follow_invalid(ctx, "chmod", dir_fd, follow);
            else
                qp_os_unavailable(ctx, "chmod", "follow_symlinks");
            r = JS_EXCEPTION;
            goto done;
        }
    } else {
        result = chmod(path.narrow, (mode_t)mode);
        err = errno;
    }
    if (result)
        r = qp_os_path_error(ctx, &path, err);
done:
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_fchmod)
{
    int fd, mode;
    QP_OS_PARSE("fchmod", 2, 2, "fd", "mode");
    if (qp_os_int(ctx, a[0], &fd) < 0 || qp_os_int(ctx, a[1], &mode) < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res = fchmod(fd, (mode_t)mode), err = errno;
        if (res == 0)
            return JS_UNDEFINED;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
}

#ifdef __APPLE__
QP_OS_FN(qp_os_lchmod)
{
    QPOSPath path = QP_OS_PATH("lchmod", "path", 0, 0);
    int mode, res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("lchmod", 2, 2, "path", "mode");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_int(ctx, a[1], &mode) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    res = lchmod(path.narrow, (mode_t)mode);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}
#endif

QP_OS_FN(qp_os_chown)
{
    QPOSPath path = QP_OS_PATH("chown", "path", 0, 1);
    uint32_t uid, gid;
    int dir_fd, follow, result, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("chown", 3, 3, "path", "uid", "gid", "dir_fd", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_id_conv(ctx, a[1], &uid, "uid") < 0 || qp_os_id_conv(ctx, a[2], &gid, "gid") < 0 ||
        qp_os_dir_fd_conv(ctx, a[3], &dir_fd) < 0 || qp_os_opt_bool(ctx, a[4], 1, &follow) < 0 ||
        qp_os_path_and_dir_fd_invalid(ctx, "chown", &path, dir_fd) ||
        qp_os_dir_fd_and_fd_invalid(ctx, "chown", dir_fd, path.fd) ||
        qp_os_fd_and_follow_invalid(ctx, "chown", path.is_fd, follow)) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (path.is_fd)
        result = fchown(path.fd, uid, gid);
    else if (!follow && dir_fd == QP_OS_DEFAULT_DIR_FD)
        result = lchown(path.narrow, uid, gid);
    else if (dir_fd != QP_OS_DEFAULT_DIR_FD || !follow)
        result = fchownat(dir_fd, path.narrow, uid, gid, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = chown(path.narrow, uid, gid);
    err = errno;
    if (result)
        r = qp_os_path_error(ctx, &path, err);
done:
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_fchown)
{
    int fd;
    uint32_t uid, gid;
    QP_OS_PARSE("fchown", 3, 3, "fd", "uid", "gid");
    if (qp_os_int(ctx, a[0], &fd) < 0 || qp_os_id_conv(ctx, a[1], &uid, "uid") < 0 ||
        qp_os_id_conv(ctx, a[2], &gid, "gid") < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res = fchown(fd, uid, gid), err = errno;
        if (res == 0)
            return JS_UNDEFINED;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
}

QP_OS_FN(qp_os_lchown)
{
    QPOSPath path = QP_OS_PATH("lchown", "path", 0, 0);
    uint32_t uid, gid;
    int res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("lchown", 3, 3, "path", "uid", "gid");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_id_conv(ctx, a[1], &uid, "uid") < 0 || qp_os_id_conv(ctx, a[2], &gid, "gid") < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    res = lchown(path.narrow, uid, gid);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* posix_getcwd: magic 1 = bytes */
QP_OS_FN(qp_os_getcwd)
{
    size_t size = 1024;
    char *buf = NULL, *nb;
    JSValue r;
    for (;;) {
        nb = js_realloc(ctx, buf, size);
        if (!nb) {
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
        buf = nb;
        if (getcwd(buf, size))
            break;
        if (errno != ERANGE) {
            int err = errno;
            js_free(ctx, buf);
            return qp_os_error(ctx, err);
        }
        size *= 2;
    }
    r = qp_os_name_value(ctx, magic, buf, strlen(buf));
    js_free(ctx, buf);
    return r;
}

QP_OS_FN(qp_os_link)
{
    QPOSPath src = QP_OS_PATH("link", "src", 0, 0), dst = QP_OS_PATH("link", "dst", 0, 0);
    int src_dir_fd, dst_dir_fd, follow, res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("link", 2, 2, "src", "dst", "src_dir_fd", "dst_dir_fd", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &src) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[1], &dst) < 0 ||
        qp_os_dir_fd_conv(ctx, a[2], &src_dir_fd) < 0 || qp_os_dir_fd_conv(ctx, a[3], &dst_dir_fd) < 0 ||
        qp_os_opt_bool(ctx, a[4], 1, &follow) < 0) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (src_dir_fd != QP_OS_DEFAULT_DIR_FD || dst_dir_fd != QP_OS_DEFAULT_DIR_FD || !follow)
        res = linkat(src_dir_fd, src.narrow, dst_dir_fd, dst.narrow, follow ? AT_SYMLINK_FOLLOW : 0);
    else
        res = link(src.narrow, dst.narrow);
    err = errno;
    if (res)
        r = qp_os_path_error2(ctx, &src, &dst, err);
done:
    qp_os_path_clean(ctx, &src);
    qp_os_path_clean(ctx, &dst);
    return r;
}

/* _posix_listdir */
static JSValue qp_os_do_listdir(JSContext *ctx, QPOSPath *path)
{
    DIR *dirp;
    struct dirent *ep;
    JSValue list, v;
    int as_bytes = path->is_bytes, fd = -1, err;
    const char *name = path->narrow ? path->narrow : ".";

    if (path->is_fd) {
        fd = dup(path->fd);
        if (fd == -1)
            return qp_os_error(ctx, errno);
        dirp = fdopendir(fd);
    } else {
        dirp = opendir(name);
    }
    if (!dirp) {
        err = errno;
        if (fd != -1)
            close(fd);
        return qp_os_path_error(ctx, path, err);
    }
    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        goto out;
    for (;;) {
        errno = 0;
        ep = readdir(dirp);
        if (!ep) {
            if (errno == 0)
                break;
            err = errno;
            JS_FreeValue(ctx, list);
            list = qp_os_path_error(ctx, path, err);
            goto out;
        }
        if (ep->d_name[0] == '.' &&
            (ep->d_name[1] == '\0' || (ep->d_name[1] == '.' && ep->d_name[2] == '\0')))
            continue;
        v = qp_os_name_value(ctx, as_bytes, ep->d_name, strlen(ep->d_name));
        if (JS_IsException(v) || qp_list_append(ctx, list, v) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
            goto out;
        }
        JS_FreeValue(ctx, v);
    }
out:
    if (fd != -1)
        rewinddir(dirp);
    closedir(dirp);
    return list;
}

QP_OS_FN(qp_os_listdir)
{
    QPOSPath path = QP_OS_PATH("listdir", "path", 1, 1);
    JSValue r;
    QP_OS_PARSE("listdir", 0, 1, "path");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    r = qp_os_do_listdir(ctx, &path);
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_mkdir)
{
    QPOSPath path = QP_OS_PATH("mkdir", "path", 0, 0);
    int mode, dir_fd, res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("mkdir", 1, 2, "path", "mode", "dir_fd");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_opt_int(ctx, a[1], 0777, &mode) < 0 || qp_os_dir_fd_conv(ctx, a[2], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    if (dir_fd != QP_OS_DEFAULT_DIR_FD)
        res = mkdirat(dir_fd, path.narrow, (mode_t)mode);
    else
        res = mkdir(path.narrow, (mode_t)mode);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* rmdir (magic 0), unlink (1), remove (2) */
QP_OS_FN(qp_os_unlink)
{
    static const char *const names[] = { "rmdir", "unlink", "remove" };
    static const char *const kw[] = { "path", "dir_fd", NULL };
    static const QPArgSpec specs[3] = {
        { "rmdir", kw, 1, 1, 0 }, { "unlink", kw, 1, 1, 0 }, { "remove", kw, 1, 1, 0 }
    };
    QPOSPath path = QP_OS_PATH(names[magic], "path", 0, 0);
    JSValueConst a[2];
    int dir_fd, res, err;
    JSValue r = JS_UNDEFINED;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_dir_fd_conv(ctx, a[1], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    if (dir_fd != QP_OS_DEFAULT_DIR_FD)
        res = unlinkat(dir_fd, path.narrow, magic == 0 ? AT_REMOVEDIR : 0);
    else
        res = magic == 0 ? rmdir(path.narrow) : unlink(path.narrow);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* rename (magic 0) / replace (1) */
QP_OS_FN(qp_os_rename)
{
    static const char *const kw[] = { "src", "dst", "src_dir_fd", "dst_dir_fd", NULL };
    static const QPArgSpec specs[2] = { { "rename", kw, 2, 2, 0 }, { "replace", kw, 2, 2, 0 } };
    const char *fn = magic ? "replace" : "rename";
    QPOSPath src = QP_OS_PATH(fn, "src", 0, 0), dst = QP_OS_PATH(fn, "dst", 0, 0);
    JSValueConst a[4];
    int sfd, dfd, res, err;
    JSValue r = JS_UNDEFINED;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[0], &src) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[1], &dst) < 0 ||
        qp_os_dir_fd_conv(ctx, a[2], &sfd) < 0 || qp_os_dir_fd_conv(ctx, a[3], &dfd) < 0) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (sfd != QP_OS_DEFAULT_DIR_FD || dfd != QP_OS_DEFAULT_DIR_FD)
        res = renameat(sfd, src.narrow, dfd, dst.narrow);
    else
        res = rename(src.narrow, dst.narrow);
    err = errno;
    if (res)
        r = qp_os_path_error2(ctx, &src, &dst, err);
done:
    qp_os_path_clean(ctx, &src);
    qp_os_path_clean(ctx, &dst);
    return r;
}

QP_OS_FN(qp_os_symlink)
{
    QPOSPath src = QP_OS_PATH("symlink", "src", 0, 0), dst = QP_OS_PATH("symlink", "dst", 0, 0);
    int target_is_dir, dir_fd, res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("symlink", 2, 3, "src", "dst", "target_is_directory", "dir_fd");
    if (qp_os_path_conv(ctx, a[0], &src) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[1], &dst) < 0 || qp_os_opt_bool(ctx, a[2], 0, &target_is_dir) < 0 ||
        qp_os_dir_fd_conv(ctx, a[3], &dir_fd) < 0) {
        r = JS_EXCEPTION;
        goto done;
    }
    if (dir_fd != QP_OS_DEFAULT_DIR_FD)
        res = symlinkat(src.narrow, dir_fd, dst.narrow);
    else
        res = symlink(src.narrow, dst.narrow);
    err = errno;
    if (res)
        r = qp_os_path_error2(ctx, &src, &dst, err);
done:
    qp_os_path_clean(ctx, &src);
    qp_os_path_clean(ctx, &dst);
    return r;
}

QP_OS_FN(qp_os_readlink)
{
    QPOSPath path = QP_OS_PATH("readlink", "path", 0, 0);
    int dir_fd, err;
    ssize_t n;
    char buf[PATH_MAX + 1];
    JSValue r;
    QP_OS_PARSE("readlink", 1, 1, "path", "dir_fd");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_dir_fd_conv(ctx, a[1], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    if (dir_fd != QP_OS_DEFAULT_DIR_FD)
        n = readlinkat(dir_fd, path.narrow, buf, sizeof(buf));
    else
        n = readlink(path.narrow, buf, sizeof(buf));
    err = errno;
    if (n < 0)
        r = qp_os_path_error(ctx, &path, err);
    else
        r = qp_os_name_value(ctx, path.is_bytes, buf, (size_t)n);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* _PyTime_ObjectToTimespec (ROUND_FLOOR) */
static int qp_os_obj_to_timespec(JSContext *ctx, JSValueConst o, struct timespec *ts)
{
    if (qp_float_check(ctx, o)) {
        double d, intpart, frac;
        if (qp_float_as_double(ctx, o, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
        frac = modf(d, &intpart);
        frac *= 1e9;
        frac = floor(frac);
        if (frac >= 1e9) {
            frac -= 1e9;
            intpart += 1.0;
        } else if (frac < 0) {
            frac += 1e9;
            intpart -= 1.0;
        }
        if (intpart < -9.2e18 || intpart > 9.2e18) {
            qp_raise_overflow_error(ctx, "timestamp out of range for platform time_t");
            return -1;
        }
        ts->tv_sec = (time_t)intpart;
        ts->tv_nsec = (long)frac;
        return 0;
    } else {
        int64_t v;
        if (qp_os_int64(ctx, o, &v) < 0) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, OverflowError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_overflow_error(ctx, "timestamp out of range for platform time_t");
            }
            return -1;
        }
        ts->tv_sec = (time_t)v;
        ts->tv_nsec = 0;
        return 0;
    }
}

/* split_py_long_to_s_and_ns */
static int qp_os_ns_to_timespec(JSContext *ctx, JSValueConst o, struct timespec *ts)
{
    JSValue i = qp_index(ctx, o), billion, q, m;
    int64_t s, ns;
    int r;
    if (JS_IsException(i))
        return -1;
    billion = JS_NewInt32(ctx, 1000000000);
    q = qp_binop(ctx, i, billion, QP_NB_FLOOR_DIVIDE);
    m = qp_binop(ctx, i, billion, QP_NB_REMAINDER);
    JS_FreeValue(ctx, i);
    r = (JS_IsException(q) || JS_IsException(m) ||
         qp_long_as_int64(ctx, q, &s) < 0 || qp_long_as_int64(ctx, m, &ns) < 0) ? -1 : 0;
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, m);
    if (r < 0)
        return -1;
    ts->tv_sec = (time_t)s;
    ts->tv_nsec = (long)ns;
    return 0;
}

QP_OS_FN(qp_os_utime)
{
    QPOSPath path = QP_OS_PATH("utime", "path", 0, 1);
    struct timespec ts[2], *tsp = NULL;
    int dir_fd, follow, res, err;
    JSValueConst times, ns;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("utime", 1, 2, "path", "times", "ns", "dir_fd", "follow_symlinks");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    times = a[1];
    ns = a[2];
    if (qp_os_dir_fd_conv(ctx, a[3], &dir_fd) < 0 || qp_os_opt_bool(ctx, a[4], 1, &follow) < 0)
        goto fail;
    if (!QP_IS_NULL(times) && JS_IsUndefined(times))
        times = QP_NULL;
    if (!QP_IS_NULL(times) && !QP_IS_NULL(ns)) {
        qp_raise_value_error(ctx, "utime: you may specify either 'times' or 'ns' but not both");
        goto fail;
    }
    if (!QP_IS_NULL(times)) {
        if (!qp_tuple_check(ctx, times) || QP_TUPLE_SIZE(times) != 2) {
            qp_raise_type_error(ctx, "utime: 'times' must be either a tuple of two ints or None");
            goto fail;
        }
        if (qp_os_obj_to_timespec(ctx, QP_TUPLE_ITEMS(times)[0], &ts[0]) < 0 ||
            qp_os_obj_to_timespec(ctx, QP_TUPLE_ITEMS(times)[1], &ts[1]) < 0)
            goto fail;
        tsp = ts;
    } else if (!QP_IS_NULL(ns)) {
        if (!qp_tuple_check(ctx, ns) || QP_TUPLE_SIZE(ns) != 2) {
            qp_raise_type_error(ctx, "utime: 'ns' must be a tuple of two ints");
            goto fail;
        }
        if (qp_os_ns_to_timespec(ctx, QP_TUPLE_ITEMS(ns)[0], &ts[0]) < 0 ||
            qp_os_ns_to_timespec(ctx, QP_TUPLE_ITEMS(ns)[1], &ts[1]) < 0)
            goto fail;
        tsp = ts;
    }
    if (qp_os_path_and_dir_fd_invalid(ctx, "utime", &path, dir_fd) ||
        qp_os_dir_fd_and_fd_invalid(ctx, "utime", dir_fd, path.fd) ||
        qp_os_fd_and_follow_invalid(ctx, "utime", path.is_fd, follow))
        goto fail;
    if (path.is_fd)
        res = futimens(path.fd, tsp);
    else
        res = utimensat(dir_fd, path.narrow, tsp, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
fail:
    qp_os_path_clean(ctx, &path);
    return JS_EXCEPTION;
}

QP_OS_FN(qp_os_umask)
{
    int mask;
    if (qp_arg_int(ctx, args[0], &mask) < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int)umask((mode_t)mask));
}

/* ---------------------------------------------------------------------- */
/* file descriptors                                                        */

static int qp_os_set_inheritable(int fd, int inheritable)
{
    int flags = fcntl(fd, F_GETFD);
    int nf;
    if (flags < 0)
        return -1;
    nf = inheritable ? (flags & ~FD_CLOEXEC) : (flags | FD_CLOEXEC);
    if (nf == flags)
        return 0;
    return fcntl(fd, F_SETFD, nf);
}

QP_OS_FN(qp_os_open)
{
    QPOSPath path = QP_OS_PATH("open", "path", 0, 0);
    int flags, mode, dir_fd, fd, err;
    JSValue r;
    QP_OS_PARSE("open", 2, 3, "path", "flags", "mode", "dir_fd");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_int(ctx, a[1], &flags) < 0 || qp_os_opt_int(ctx, a[2], 0777, &mode) < 0 ||
        qp_os_dir_fd_conv(ctx, a[3], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    flags |= O_CLOEXEC;
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        if (dir_fd != QP_OS_DEFAULT_DIR_FD)
            fd = openat(dir_fd, path.narrow, flags, mode);
        else
            fd = open(path.narrow, flags, mode);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (fd >= 0)
            break;
        if (err == EINTR) {
            if (qp_signal_check(ctx) < 0) {
                qp_os_path_clean(ctx, &path);
                return JS_EXCEPTION;
            }
            continue;
        }
        r = qp_os_path_error(ctx, &path, err);
        qp_os_path_clean(ctx, &path);
        return r;
    }
    qp_os_path_clean(ctx, &path);
    return JS_NewInt32(ctx, fd);
}

QP_OS_FN(qp_os_close)
{
    int fd;
    QP_OS_PARSE("close", 1, 1, "fd");
    if (qp_os_int(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    /* PEP 475 does not retry close() */
    {
        int res, err;
        QP_BEGIN_ALLOW_THREADS(ctx)
        res = close(fd);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res < 0)
            return qp_os_error(ctx, err);
    }
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_closerange)
{
    int lo, hi, i;
    if (qp_arg_check_positional(ctx, "closerange", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &lo) < 0 || qp_arg_int(ctx, args[1], &hi) < 0)
        return JS_EXCEPTION;
    /* _Py_closerange: errors ignored */
    if (lo < 0)
        lo = 0;
    for (i = lo; i < hi; i++)
        close(i);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_dup)
{
    int fd, nfd;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    nfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (nfd < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, nfd);
}

QP_OS_FN(qp_os_dup2)
{
    int fd, fd2, inheritable, res;
    QP_OS_PARSE("dup2", 2, 3, "fd", "fd2", "inheritable");
    if (qp_os_int(ctx, a[0], &fd) < 0 || qp_os_int(ctx, a[1], &fd2) < 0 ||
        qp_os_opt_bool(ctx, a[2], 1, &inheritable) < 0)
        return JS_EXCEPTION;
#ifdef __linux__
    if (!inheritable) {
        res = dup3(fd, fd2, O_CLOEXEC);
        if (res < 0)
            return qp_os_error(ctx, errno);
        return JS_NewInt32(ctx, res);
    }
#endif
    res = dup2(fd, fd2);
    if (res < 0)
        return qp_os_error(ctx, errno);
    if (!inheritable && qp_os_set_inheritable(res, 0) < 0) {
        int err = errno;
        close(res);
        return qp_os_error(ctx, err);
    }
    return JS_NewInt32(ctx, res);
}

QP_OS_FN(qp_os_lseek)
{
    int fd, how;
    off_t pos, res;
    if (qp_arg_check_positional(ctx, "lseek", nargs, 3, 3) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_os_off(ctx, args[1], &pos) < 0 ||
        qp_arg_int(ctx, args[2], &how) < 0)
        return JS_EXCEPTION;
    res = lseek(fd, pos, how);
    if (res < 0)
        return qp_os_error(ctx, errno);
    return qp_os_long(ctx, (int64_t)res);
}

/* read (magic 0) / pread (magic 1) */
QP_OS_FN(qp_os_read)
{
    int fd, err;
    Py_ssize_t length;
    off_t offset = 0;
    ssize_t n;
    char *buf;
    JSValue r;
    if (qp_arg_check_positional(ctx, magic ? "pread" : "read", nargs, 2 + magic, 2 + magic) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_arg_ssize(ctx, args[1], &length) < 0 ||
        (magic && qp_os_off(ctx, args[2], &offset) < 0))
        return JS_EXCEPTION;
    if (length < 0)
        return qp_os_error(ctx, EINVAL);
    buf = js_malloc(ctx, length ? length : 1);
    if (!buf)
        return JS_EXCEPTION;
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        n = magic ? pread(fd, buf, length, offset) : read(fd, buf, length);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0)
            break;
        if (err == EINTR) {
            if (qp_signal_check(ctx) < 0) {
                js_free(ctx, buf);
                return JS_EXCEPTION;
            }
            continue;
        }
        js_free(ctx, buf);
        return qp_os_error(ctx, err);
    }
    r = qp_bytes_new(ctx, buf, n);
    js_free(ctx, buf);
    return r;
}

/* readinto(fd, buffer, /) */
QP_OS_FN(qp_os_readinto)
{
    int fd, err;
    ssize_t n;
    QPBuffer view;
    if (qp_arg_check_positional(ctx, "readinto", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    /* Py_buffer(accept={rwbuffer}): _PyArg_BadArgument */
    if (qp_buffer_get(ctx, args[1], &view, QP_BUF_WRITABLE) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qp_raise_type_error(ctx, "readinto() argument 2 must be read-write bytes-like object, not %s",
                                   JS_IsUndefined(args[1]) ? "None" : qp_os_tname(ctx, args[1]));
    }
    if (view.len < 0) {
        qp_buffer_release(ctx, &view);
        return qp_raise_value_error(ctx, "negative buffer length");
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        n = read(fd, view.buf, view.len);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0)
            break;
        if (err == EINTR) {
            if (qp_signal_check(ctx) < 0) {
                qp_buffer_release(ctx, &view);
                return JS_EXCEPTION;
            }
            continue;
        }
        qp_buffer_release(ctx, &view);
        return qp_os_error(ctx, err);
    }
    qp_buffer_release(ctx, &view);
    return qp_os_long(ctx, (int64_t)n);
}

/* write (magic 0) / pwrite (magic 1) */
QP_OS_FN(qp_os_write)
{
    int fd, err;
    off_t offset = 0;
    ssize_t n;
    QPBuffer view;
    if (qp_arg_check_positional(ctx, magic ? "pwrite" : "write", nargs, 2 + magic, 2 + magic) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (qp_buffer_get(ctx, args[1], &view, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    if (magic && qp_os_off(ctx, args[2], &offset) < 0) {
        qp_buffer_release(ctx, &view);
        return JS_EXCEPTION;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        n = magic ? pwrite(fd, view.buf, view.len, offset) : write(fd, view.buf, view.len);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0)
            break;
        if (err == EINTR) {
            if (qp_signal_check(ctx) < 0) {
                qp_buffer_release(ctx, &view);
                return JS_EXCEPTION;
            }
            continue;
        }
        qp_buffer_release(ctx, &view);
        return qp_os_error(ctx, err);
    }
    qp_buffer_release(ctx, &view);
    return qp_os_long(ctx, (int64_t)n);
}

/* iov_setup: readv (magic 0, writable buffers) / writev (magic 1) */
QP_OS_FN(qp_os_readv)
{
    const char *fn = magic ? "writev" : "readv";
    int fd, err, i, cnt;
    JSValue seq;
    JSValue *items;
    Py_ssize_t n;
    QPBuffer *views;
    struct iovec *iov;
    ssize_t res;
    if (qp_arg_check_positional(ctx, fn, nargs, 2, 2) < 0 || qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    {
        JSValue it = qp_iter(ctx, args[1]);
        if (JS_IsException(it)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "%s() arg 2 must be a sequence", fn);
            }
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, it);
    }
    seq = qp_list_from_iterable(ctx, args[1]);
    if (JS_IsException(seq))
        return seq;
    items = qp_list_items(seq, &n);
    if (n > INT_MAX) {
        JS_FreeValue(ctx, seq);
        return qp_raise_overflow_error(ctx, "too many buffers");
    }
    cnt = (int)n;
    views = js_mallocz(ctx, sizeof(QPBuffer) * (cnt ? cnt : 1));
    iov = js_malloc(ctx, sizeof(struct iovec) * (cnt ? cnt : 1));
    if (!views || !iov) {
        js_free(ctx, views);
        js_free(ctx, iov);
        JS_FreeValue(ctx, seq);
        return JS_EXCEPTION;
    }
    for (i = 0; i < cnt; i++) {
        if (qp_buffer_get(ctx, items[i], &views[i], magic ? QP_BUF_SIMPLE : QP_BUF_WRITABLE) < 0) {
            cnt = i;
            res = -2;
            goto done;
        }
        iov[i].iov_base = views[i].buf;
        iov[i].iov_len = views[i].len;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        res = magic ? writev(fd, iov, cnt) : readv(fd, iov, cnt);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0) {
            res = -2;
            break;
        }
    }
done:
    for (i = 0; i < cnt; i++)
        qp_buffer_release(ctx, &views[i]);
    js_free(ctx, views);
    js_free(ctx, iov);
    JS_FreeValue(ctx, seq);
    if (res == -2)
        return JS_EXCEPTION;
    if (res < 0)
        return qp_os_error(ctx, err);
    return qp_os_long(ctx, (int64_t)res);
}

/* fsync (magic 0) / fdatasync (magic 1) */
QP_OS_FN(qp_os_fsync)
{
    static const char *const kw[] = { "fd", NULL };
    static const QPArgSpec specs[2] = { { "fsync", kw, 1, 1, 0 }, { "fdatasync", kw, 1, 1, 0 } };
    JSValueConst a[1];
    int fd;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0 ||
        qp_object_as_fd(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res, err;
        QP_BEGIN_ALLOW_THREADS(ctx)
#ifdef __linux__
        res = magic ? fdatasync(fd) : fsync(fd);
#else
        res = fsync(fd);
#endif
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res == 0)
            return JS_UNDEFINED;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
}

QP_OS_FN(qp_os_ftruncate)
{
    int fd;
    off_t length;
    if (qp_arg_check_positional(ctx, "ftruncate", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_os_off(ctx, args[1], &length) < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res = ftruncate(fd, length), err = errno;
        if (res == 0)
            return JS_UNDEFINED;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
}

QP_OS_FN(qp_os_truncate)
{
    QPOSPath path = QP_OS_PATH("truncate", "path", 0, 1);
    off_t length;
    int res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("truncate", 2, 2, "path", "length");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_off(ctx, a[1], &length) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    res = path.is_fd ? ftruncate(path.fd, length) : truncate(path.narrow, length);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

static JSValue qp_os_fd_pair(JSContext *ctx, int a0, int a1)
{
    JSValue items[2];
    items[0] = JS_NewInt32(ctx, a0);
    items[1] = JS_NewInt32(ctx, a1);
    return qp_tuple_new_steal(ctx, 2, items);
}

QP_OS_FN(qp_os_pipe)
{
    int fds[2];
#ifdef QP_OS_HAVE_PIPE2
    /* pipe2() is available on Linux and macOS 27+ */
    static int pipe2_works = -1;
    if (pipe2_works != 0) {
        if (QP_OS_HAVE_PIPE2_RUNTIME) {
            int res = pipe2(fds, O_CLOEXEC), err = errno;
            if (res == 0)
                return qp_os_fd_pair(ctx, fds[0], fds[1]);
            if (err != ENOSYS)
                return qp_os_error(ctx, err);
        }
        pipe2_works = 0;
    }
#endif
    if (pipe(fds) < 0)
        return qp_os_error(ctx, errno);
    if (qp_os_set_inheritable(fds[0], 0) < 0 || qp_os_set_inheritable(fds[1], 0) < 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        return qp_os_error(ctx, err);
    }
    return qp_os_fd_pair(ctx, fds[0], fds[1]);
}

#ifdef QP_OS_HAVE_PIPE2
QP_OS_FN(qp_os_pipe2)
{
    int fds[2], flags, res;
    if (qp_arg_int(ctx, args[0], &flags) < 0)
        return JS_EXCEPTION;
    if (QP_OS_HAVE_PIPE2_RUNTIME) {
        res = pipe2(fds, flags);
    } else {
        res = -1;
        errno = ENOSYS;
    }
    if (res != 0)
        return qp_os_error(ctx, errno);
    return qp_os_fd_pair(ctx, fds[0], fds[1]);
}
#endif

QP_OS_FN(qp_os_isatty)
{
    int fd;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    return qp_bool_new(isatty(fd));
}

QP_OS_FN(qp_os_get_inheritable)
{
    int fd, flags;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return qp_os_error(ctx, errno);
    return qp_bool_new(!(flags & FD_CLOEXEC));
}

QP_OS_FN(qp_os_set_inheritable_fn)
{
    int fd, inh;
    if (qp_arg_check_positional(ctx, "set_inheritable", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_arg_int(ctx, args[1], &inh) < 0)
        return JS_EXCEPTION;
    if (qp_os_set_inheritable(fd, inh) < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_get_blocking)
{
    int fd, flags;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return qp_os_error(ctx, errno);
    return qp_bool_new(!(flags & O_NONBLOCK));
}

QP_OS_FN(qp_os_set_blocking)
{
    int fd, blocking, flags, nf;
    if (qp_arg_check_positional(ctx, "set_blocking", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_arg_bool(ctx, args[1], &blocking) < 0)
        return JS_EXCEPTION;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return qp_os_error(ctx, errno);
    nf = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (nf != flags && fcntl(fd, F_SETFL, nf) < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* ---------------------------------------------------------------------- */
/* DirEntry / ScandirIterator                                              */

enum {
    QP_DE_NAME, QP_DE_PATH, QP_DE_STAT, QP_DE_LSTAT, QP_DE_DTYPE, QP_DE_INO,
    QP_DE_DIRFD, QP_DE_MODULE, QP_DE_NSLOTS
};

static JSValue *qp_de_slots(JSValueConst self)
{
    return qp_instance_slots(self);
}

/* DirEntry_fetch_stat */
static JSValue qp_de_fetch_stat(JSContext *ctx, JSValueConst self, int follow)
{
    JSValue *s = qp_de_slots(self);
    struct stat st;
    int res, err, dir_fd = JS_VALUE_GET_INT(s[QP_DE_DIRFD]);
    QPOSPath path = QP_OS_PATH("stat", "path", 0, 0);
    JSValue r;
    if (qp_os_path_conv(ctx, dir_fd != QP_OS_DEFAULT_DIR_FD ? s[QP_DE_NAME] : s[QP_DE_PATH], &path) < 0)
        return JS_EXCEPTION;
    if (dir_fd != QP_OS_DEFAULT_DIR_FD)
        res = fstatat(dir_fd, path.narrow, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    else if (follow)
        res = stat(path.narrow, &st);
    else
        res = lstat(path.narrow, &st);
    err = errno;
    qp_os_path_clean(ctx, &path);
    if (res != 0)
        return qp_raise_os_error(ctx, err, s[QP_DE_PATH]);
    r = qp_os_stat_from(ctx, s[QP_DE_MODULE], &st);
    return r;
}

static JSValue qp_de_get_lstat(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_de_slots(self);
    if (QP_IS_NULL(s[QP_DE_LSTAT])) {
        JSValue v = qp_de_fetch_stat(ctx, self, 0);
        if (JS_IsException(v))
            return v;
        s = qp_de_slots(self);
        s[QP_DE_LSTAT] = v;
    }
    return JS_DupValue(ctx, s[QP_DE_LSTAT]);
}

static int qp_de_test_mode(JSContext *ctx, JSValueConst self, int follow, mode_t mode_bits);

static int qp_de_is_symlink_c(JSContext *ctx, JSValueConst self)
{
    int d_type = JS_VALUE_GET_INT(qp_de_slots(self)[QP_DE_DTYPE]);
    if (d_type != DT_UNKNOWN)
        return d_type == DT_LNK;
    return qp_de_test_mode(ctx, self, 0, S_IFLNK);
}

static JSValue qp_de_get_stat(JSContext *ctx, JSValueConst self, int follow)
{
    JSValue *s;
    if (!follow)
        return qp_de_get_lstat(ctx, self);
    s = qp_de_slots(self);
    if (QP_IS_NULL(s[QP_DE_STAT])) {
        int r = qp_de_is_symlink_c(ctx, self);
        JSValue v;
        if (r < 0)
            return JS_EXCEPTION;
        v = r ? qp_de_fetch_stat(ctx, self, 1) : qp_de_get_lstat(ctx, self);
        if (JS_IsException(v))
            return v;
        s = qp_de_slots(self);
        s[QP_DE_STAT] = v;
    }
    return JS_DupValue(ctx, s[QP_DE_STAT]);
}

/* DirEntry_test_mode */
static int qp_de_test_mode(JSContext *ctx, JSValueConst self, int follow, mode_t mode_bits)
{
    int d_type = JS_VALUE_GET_INT(qp_de_slots(self)[QP_DE_DTYPE]);
    int need_stat;
    if (d_type == DT_UNKNOWN) {
        need_stat = 1;
    } else if (follow) {
        int r = qp_de_is_symlink_c(ctx, self);
        if (r < 0)
            return -1;
        need_stat = r;
    } else {
        need_stat = 0;
    }
    if (need_stat) {
        JSValue st = qp_de_get_stat(ctx, self, follow), m;
        int64_t mode;
        if (JS_IsException(st)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, FileNotFoundError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return 0;
            }
            return -1;
        }
        m = qp_ss_get(ctx, st, 0);   /* st_mode */
        JS_FreeValue(ctx, st);
        if (JS_IsException(m))
            return -1;
        if (qp_long_as_int64(ctx, m, &mode) < 0) {
            JS_FreeValue(ctx, m);
            return -1;
        }
        JS_FreeValue(ctx, m);
        return ((mode_t)mode & S_IFMT) == mode_bits;
    }
    if (mode_bits == S_IFDIR)
        return d_type == DT_DIR;
    if (mode_bits == S_IFREG)
        return d_type == DT_REG;
    return d_type == DT_LNK;
}

/* is_dir (magic S_IFDIR) / is_file (magic S_IFREG) */
static JSValue qp_de_is_x(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "follow_symlinks", NULL };
    static const QPArgSpec spec_dir = { "is_dir", kw, 0, 0, 0 };
    static const QPArgSpec spec_file = { "is_file", kw, 0, 0, 0 };
    JSValueConst a[1];
    int follow, r;
    if (qp_arg_parse(ctx, magic == S_IFDIR ? &spec_dir : &spec_file, args, nargs, kwnames, a) < 0 ||
        qp_os_opt_bool(ctx, a[0], 1, &follow) < 0)
        return JS_EXCEPTION;
    r = qp_de_test_mode(ctx, self, follow, (mode_t)magic);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

QP_OS_FN(qp_de_is_symlink)
{
    int r = qp_de_is_symlink_c(ctx, self);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

QP_OS_FN(qp_de_is_junction)
{
    return JS_FALSE;
}

QP_OS_FN(qp_de_stat)
{
    static const char *const kw[] = { "follow_symlinks", NULL };
    static const QPArgSpec spec = { "stat", kw, 0, 0, 0 };
    JSValueConst a[1];
    int follow;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0 ||
        qp_os_opt_bool(ctx, a[0], 1, &follow) < 0)
        return JS_EXCEPTION;
    return qp_de_get_stat(ctx, self, follow);
}

QP_OS_FN(qp_de_inode)
{
    return JS_DupValue(ctx, qp_de_slots(self)[QP_DE_INO]);
}

QP_OS_FN(qp_de_fspath)
{
    return JS_DupValue(ctx, qp_de_slots(self)[QP_DE_PATH]);
}

QP_OS_FN(qp_de_class_getitem)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static JSValue qp_de_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_format(ctx, "<DirEntry %R>", qp_de_slots(self)[QP_DE_NAME]);
}

static const QPMethodDef qp_de_methods[] = {
    { "is_dir", qp_de_is_x, QP_METH_FASTCALL | QP_METH_KEYWORDS, S_IFDIR, "is_dir($self, /, *, follow_symlinks=True)\n"
"--\n"
"\n"
"Return True if the entry is a directory; cached per entry." },
    { "is_file", qp_de_is_x, QP_METH_FASTCALL | QP_METH_KEYWORDS, S_IFREG, "is_file($self, /, *, follow_symlinks=True)\n"
"--\n"
"\n"
"Return True if the entry is a file; cached per entry." },
    { "is_symlink", qp_de_is_symlink, QP_METH_NOARGS, 0, "is_symlink($self, /)\n"
"--\n"
"\n"
"Return True if the entry is a symbolic link; cached per entry." },
    { "is_junction", qp_de_is_junction, QP_METH_NOARGS, 0, "is_junction($self, /)\n"
"--\n"
"\n"
"Return True if the entry is a junction; cached per entry." },
    { "stat", qp_de_stat, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0, "stat($self, /, *, follow_symlinks=True)\n"
"--\n"
"\n"
"Return stat_result object for the entry; cached per entry." },
    { "inode", qp_de_inode, QP_METH_NOARGS, 0, "inode($self, /)\n"
"--\n"
"\n"
"Return inode of the entry; cached per entry." },
    { "__fspath__", qp_de_fspath, QP_METH_NOARGS, 0, "__fspath__($self, /)\n"
"--\n"
"\n"
"Returns the path for the entry." },
    { "__class_getitem__", qp_de_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "See PEP 585" },
    { NULL }
};

static const QPMemberDef qp_de_members[] = {
    { "name", QP_DE_NAME, QP_MEMBER_READONLY,
      "the entry's base filename, relative to scandir() \"path\" argument" },
    { "path", QP_DE_PATH, QP_MEMBER_READONLY,
      "the entry's full path name; equivalent to\nos.path.join(scandir_path, entry.name)" },
    { NULL }
};

static const QPTypeSpec qp_de_spec = {
    .name = "posix.DirEntry",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_DE_NSLOTS,
    .slots = { .tp_repr = qp_de_repr },
    .methods = qp_de_methods,
    .members = qp_de_members,
};

enum {
    QP_SI_DIRP, QP_SI_PATHOBJ, QP_SI_PREFIX, QP_SI_ISBYTES, QP_SI_FD, QP_SI_MODULE, QP_SI_NSLOTS
};

static DIR *qp_si_dirp(JSValueConst self)
{
    JSValue v = qp_instance_slots(self)[QP_SI_DIRP];
    double d;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)
        d = JS_VALUE_GET_FLOAT64(v);
    else if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        d = JS_VALUE_GET_INT(v);
    else
        return NULL;
    return (DIR *)(uintptr_t)d;
}

/* ScandirIterator_closedir */
static void qp_si_closedir(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    DIR *dirp = qp_si_dirp(self);
    if (!dirp)
        return;
    s[QP_SI_DIRP] = JS_UNDEFINED;
    if (JS_VALUE_GET_INT(s[QP_SI_FD]) != -1)
        rewinddir(dirp);
    closedir(dirp);
}

/* DirEntry_from_posix_info */
static JSValue qp_de_new(JSContext *ctx, JSValueConst it, const char *name, size_t name_len,
                         int d_type, uint64_t d_ino)
{
    JSValue *s = qp_instance_slots(it);
    JSValueConst module = s[QP_SI_MODULE];
    QPType *t = qp_os_get_type(ctx, module, QP_OS_PROP_DIRENTRY);
    int as_bytes = JS_ToBool(ctx, s[QP_SI_ISBYTES]);
    int fd = JS_VALUE_GET_INT(s[QP_SI_FD]);
    JSValue entry, nm, path;
    JSValue *es;
    if (!t)
        return JS_EXCEPTION;
    nm = qp_os_name_value(ctx, as_bytes, name, name_len);
    if (JS_IsException(nm))
        return nm;
    if (fd != -1) {
        path = JS_DupValue(ctx, nm);
    } else {
        /* join_path_filename */
        Py_ssize_t plen;
        const uint8_t *p = qp_bytes_data(s[QP_SI_PREFIX], &plen);
        size_t len = plen + 1 + name_len;
        char *buf = js_malloc(ctx, len + 1);
        size_t k = 0;
        if (!buf) {
            JS_FreeValue(ctx, nm);
            return JS_EXCEPTION;
        }
        memcpy(buf, p, plen);
        k = plen;
        if (k > 0 && buf[k - 1] != '/')
            buf[k++] = '/';
        memcpy(buf + k, name, name_len);
        k += name_len;
        path = qp_os_name_value(ctx, as_bytes, buf, k);
        js_free(ctx, buf);
        if (JS_IsException(path)) {
            JS_FreeValue(ctx, nm);
            return path;
        }
    }
    entry = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(entry)) {
        JS_FreeValue(ctx, nm);
        JS_FreeValue(ctx, path);
        return entry;
    }
    es = qp_instance_slots(entry);
    es[QP_DE_NAME] = nm;
    es[QP_DE_PATH] = path;
    es[QP_DE_STAT] = QP_NULL;
    es[QP_DE_LSTAT] = QP_NULL;
    es[QP_DE_DTYPE] = JS_NewInt32(ctx, d_type);
    es[QP_DE_INO] = qp_long_from_uint64(ctx, d_ino);
    es[QP_DE_DIRFD] = JS_NewInt32(ctx, fd != -1 ? fd : QP_OS_DEFAULT_DIR_FD);
    es[QP_DE_MODULE] = JS_DupValue(ctx, module);
    if (JS_IsException(es[QP_DE_INO])) {
        es[QP_DE_INO] = JS_UNDEFINED;
        JS_FreeValue(ctx, entry);
        return JS_EXCEPTION;
    }
    return entry;
}

/* ScandirIterator_iternext */
static JSValue qp_si_next(JSContext *ctx, JSValueConst self)
{
    DIR *dirp = qp_si_dirp(self);
    struct dirent *de;
    if (!dirp)
        return QP_NULL;
    for (;;) {
        errno = 0;
        de = readdir(dirp);
        if (!de) {
            int err = errno;
            if (err != 0) {
                JSValue pobj = JS_DupValue(ctx, qp_instance_slots(self)[QP_SI_PATHOBJ]);
                qp_si_closedir(ctx, self);
                qp_raise_os_error(ctx, err, pobj);
                JS_FreeValue(ctx, pobj);
                return JS_EXCEPTION;
            }
            break;
        }
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        return qp_de_new(ctx, self, de->d_name, strlen(de->d_name), de->d_type, (uint64_t)de->d_ino);
    }
    qp_si_closedir(ctx, self);
    return QP_NULL;
}

static JSValue qp_si_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

QP_OS_FN(qp_si_close)
{
    qp_si_closedir(ctx, self);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_si_enter)
{
    return JS_DupValue(ctx, self);
}

QP_OS_FN(qp_si_exit)
{
    qp_si_closedir(ctx, self);
    return JS_UNDEFINED;
}

/* ScandirIterator_finalize */
QP_OS_FN(qp_si_del)
{
    if (qp_si_dirp(self)) {
        qp_si_closedir(ctx, self);
        if (qp_warn(ctx, QP_EXC(ctx, ResourceWarning), 1, "unclosed scandir iterator %R", self) < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static const QPMethodDef qp_si_methods[] = {
    { "__enter__", qp_si_enter, QP_METH_NOARGS, 0, NULL },
    { "__exit__", qp_si_exit, QP_METH_FASTCALL, 0, NULL },
    { "close", qp_si_close, QP_METH_NOARGS, 0, NULL },
    { "__del__", qp_si_del, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_si_spec = {
    .name = "posix.ScandirIterator",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_HAS_DEL,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_SI_NSLOTS,
    .slots = { .tp_iter = qp_si_iter, .tp_iternext = qp_si_next },
    .methods = qp_si_methods,
};

QP_OS_FN(qp_os_scandir)
{
    QPOSPath path = QP_OS_PATH("scandir", "path", 1, 1);
    QPType *t;
    JSValue it, *s;
    DIR *dirp;
    int fd = -1, err;
    QP_OS_PARSE("scandir", 0, 1, "path");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    t = qp_os_get_type(ctx, self, QP_OS_PROP_SCANDIR);
    if (!t) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    if (path.is_fd) {
        fd = dup(path.fd);
        if (fd == -1) {
            err = errno;
            JSValue r = qp_os_path_error(ctx, &path, err);
            qp_os_path_clean(ctx, &path);
            return r;
        }
        dirp = fdopendir(fd);
    } else {
        dirp = opendir(path.narrow ? path.narrow : ".");
    }
    if (!dirp) {
        JSValue r;
        err = errno;
        if (fd != -1)
            close(fd);
        r = qp_os_path_error(ctx, &path, err);
        qp_os_path_clean(ctx, &path);
        return r;
    }
    it = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(it)) {
        closedir(dirp);
        qp_os_path_clean(ctx, &path);
        return it;
    }
    s = qp_instance_slots(it);
    s[QP_SI_DIRP] = JS_NewFloat64(ctx, (double)(uintptr_t)dirp);
    s[QP_SI_PATHOBJ] = JS_DupValue(ctx, path.object);
    s[QP_SI_PREFIX] = path.is_fd ? JS_UNDEFINED
        : qp_bytes_new(ctx, path.narrow ? path.narrow : ".", path.narrow ? path.length : 1);
    s[QP_SI_ISBYTES] = qp_bool_new(path.is_bytes);
    /* iterator->path.fd: the caller's descriptor (entries stat relative to
       it after the iterator, which owns only the dup, is closed) */
    s[QP_SI_FD] = JS_NewInt32(ctx, path.is_fd ? path.fd : -1);
    s[QP_SI_MODULE] = JS_DupValue(ctx, self);
    qp_os_path_clean(ctx, &path);
    if (JS_IsException(s[QP_SI_PREFIX])) {
        s[QP_SI_PREFIX] = JS_UNDEFINED;
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    return it;
}

/* ---------------------------------------------------------------------- */
/* statvfs                                                                 */

static JSValue qp_os_statvfs_from(JSContext *ctx, JSValueConst module, const struct statvfs *st)
{
    QPType *t = qp_os_get_type(ctx, module, QP_OS_PROP_STATVFS);
    JSValue items[11];
    int i;
    if (!t)
        return JS_EXCEPTION;
    items[0] = qp_long_from_uint64(ctx, (uint64_t)st->f_bsize);
    items[1] = qp_long_from_uint64(ctx, (uint64_t)st->f_frsize);
    items[2] = qp_long_from_uint64(ctx, (uint64_t)st->f_blocks);
    items[3] = qp_long_from_uint64(ctx, (uint64_t)st->f_bfree);
    items[4] = qp_long_from_uint64(ctx, (uint64_t)st->f_bavail);
    items[5] = qp_long_from_uint64(ctx, (uint64_t)st->f_files);
    items[6] = qp_long_from_uint64(ctx, (uint64_t)st->f_ffree);
    items[7] = qp_long_from_uint64(ctx, (uint64_t)st->f_favail);
    items[8] = qp_long_from_uint64(ctx, (uint64_t)st->f_flag);
    items[9] = qp_long_from_uint64(ctx, (uint64_t)st->f_namemax);
    items[10] = qp_long_from_uint64(ctx, (uint64_t)st->f_fsid);
    for (i = 0; i < 11; i++) {
        if (JS_IsException(items[i])) {
            for (i = 0; i < 11; i++)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    return qp_structseq_new(ctx, t, items);
}

QP_OS_FN(qp_os_statvfs)
{
    QPOSPath path = QP_OS_PATH("statvfs", "path", 0, 1);
    struct statvfs st;
    int res, err;
    JSValue r;
    QP_OS_PARSE("statvfs", 1, 1, "path");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    res = path.is_fd ? fstatvfs(path.fd, &st) : statvfs(path.narrow, &st);
    err = errno;
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    else
        r = qp_os_statvfs_from(ctx, self, &st);
    qp_os_path_clean(ctx, &path);
    return r;
}

QP_OS_FN(qp_os_fstatvfs)
{
    struct statvfs st;
    int fd;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    for (;;) {
        int res = fstatvfs(fd, &st), err = errno;
        if (res == 0)
            break;
        if ((res = qp_os_eintr_retry(ctx, err)) < 0)
            return JS_EXCEPTION;
        if (!res)
            return qp_os_error(ctx, err);
    }
    return qp_os_statvfs_from(ctx, self, &st);
}

/* ---------------------------------------------------------------------- */
/* process information                                                     */

/* getpid (0), getppid (1), getuid (2), geteuid (3), getgid (4),
   getegid (5), getpgrp (6), setsid (7) */
QP_OS_FN(qp_os_getid)
{
    switch (magic) {
    case 0: return JS_NewInt32(ctx, (int)getpid());
    case 1: return JS_NewInt32(ctx, (int)getppid());
    case 2: return qp_os_from_id(ctx, getuid());
    case 3: return qp_os_from_id(ctx, geteuid());
    case 4: return qp_os_from_id(ctx, getgid());
    case 5: return qp_os_from_id(ctx, getegid());
    case 6: return JS_NewInt32(ctx, (int)getpgrp());
    default:
        if (setsid() < 0)
            return qp_os_error(ctx, errno);
        return JS_UNDEFINED;
    }
}

QP_OS_FN(qp_os_getpgid)
{
    int pid;
    pid_t r;
    QP_OS_PARSE("getpgid", 1, 1, "pid");
    if (qp_os_int(ctx, a[0], &pid) < 0)
        return JS_EXCEPTION;
    r = getpgid(pid);
    if (r < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, (int)r);
}

QP_OS_FN(qp_os_getsid)
{
    int pid;
    pid_t r;
    if (qp_arg_int(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    r = getsid(pid);
    if (r < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, (int)r);
}

QP_OS_FN(qp_os_setpgid)
{
    int pid, pgrp;
    if (qp_arg_check_positional(ctx, "setpgid", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &pid) < 0 || qp_arg_int(ctx, args[1], &pgrp) < 0)
        return JS_EXCEPTION;
    if (setpgid(pid, pgrp) < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_getgroups)
{
    int n = getgroups(0, NULL), i;
    gid_t *grs;
    JSValue list;
    if (n < 0)
        return qp_os_error(ctx, errno);
    grs = js_malloc(ctx, sizeof(gid_t) * (n ? n : 1));
    if (!grs)
        return JS_EXCEPTION;
    n = getgroups(n, grs);
    if (n < 0) {
        int err = errno;
        js_free(ctx, grs);
        return qp_os_error(ctx, err);
    }
    list = qp_list_new(ctx, 0);
    for (i = 0; i < n && !JS_IsException(list); i++) {
        JSValue v = qp_os_from_id(ctx, grs[i]);
        if (JS_IsException(v) || qp_list_append(ctx, list, v) < 0) {
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    js_free(ctx, grs);
    return list;
}

QP_OS_FN(qp_os_getlogin)
{
    char *name;
    errno = 0;
    name = getlogin();
    if (!name) {
        if (errno)
            return qp_os_error(ctx, errno);
        return qp_raise(ctx, QP_EXC(ctx, OSError), "unable to determine login name");
    }
    return qp_os_fsdecode(ctx, name, strlen(name));
}

/* kill (magic 0) / killpg (magic 1) */
QP_OS_FN(qp_os_kill)
{
    int pid, sig;
    if (qp_arg_check_positional(ctx, magic ? "killpg" : "kill", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &pid) < 0 || qp_arg_int(ctx, args[1], &sig) < 0)
        return JS_EXCEPTION;
    if ((magic ? killpg(pid, sig) : kill(pid, sig)) < 0)
        return qp_os_error(ctx, errno);
    /* os_kill_impl: check immediately if the signal was sent to the
       current process (killpg does not) */
    if (!magic && qp_signal_check_signals(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* waitpid (magic 0) / wait (magic 1) */
QP_OS_FN(qp_os_waitpid)
{
    int pid = -1, options = 0, status = 0;
    pid_t res;
    if (!magic) {
        if (qp_arg_check_positional(ctx, "waitpid", nargs, 2, 2) < 0 ||
            qp_arg_int(ctx, args[0], &pid) < 0 || qp_arg_int(ctx, args[1], &options) < 0)
            return JS_EXCEPTION;
    }
    for (;;) {
        int err;
        QP_BEGIN_ALLOW_THREADS(ctx)
        res = magic ? wait(&status) : waitpid(pid, &status, options);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res >= 0)
            break;
        if (err != EINTR)
            return qp_os_error(ctx, err);
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    {
        JSValue items[2];
        items[0] = JS_NewInt32(ctx, (int)res);
        items[1] = JS_NewInt32(ctx, status);
        return qp_tuple_new_steal(ctx, 2, items);
    }
}

QP_OS_FN(qp_os_waitstatus_to_exitcode)
{
    int status;
    QP_OS_PARSE("waitstatus_to_exitcode", 1, 1, "status");
    if (qp_os_int(ctx, a[0], &status) < 0)
        return JS_EXCEPTION;
    if (WIFEXITED(status))
        return JS_NewInt32(ctx, WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return JS_NewInt32(ctx, -WTERMSIG(status));
    return qp_raise_value_error(ctx, "invalid wait status: %i", status);
}

enum { QP_W_IFEXITED, QP_W_EXITSTATUS, QP_W_IFSIGNALED, QP_W_TERMSIG, QP_W_IFSTOPPED,
       QP_W_STOPSIG, QP_W_COREDUMP, QP_W_IFCONTINUED };

QP_OS_FN(qp_os_wmacro)
{
    static const char *const kw[] = { "status", NULL };
    static const char *const kwpos[] = { "", NULL };
    static const QPArgSpec specs[] = {
        { "WIFEXITED", kw, 1, 1, 0 }, { "WEXITSTATUS", kw, 1, 1, 0 },
        { "WIFSIGNALED", kw, 1, 1, 0 }, { "WTERMSIG", kw, 1, 1, 0 },
        { "WIFSTOPPED", kw, 1, 1, 0 }, { "WSTOPSIG", kw, 1, 1, 0 },
        { "WCOREDUMP", kwpos, 1, 1, 0 }, { "WIFCONTINUED", kw, 1, 1, 0 },
    };
    JSValueConst a[1];
    int st;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0 || qp_os_int(ctx, a[0], &st) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case QP_W_IFEXITED: return qp_bool_new(WIFEXITED(st));
    case QP_W_EXITSTATUS: return JS_NewInt32(ctx, WEXITSTATUS(st));
    case QP_W_IFSIGNALED: return qp_bool_new(WIFSIGNALED(st));
    case QP_W_TERMSIG: return JS_NewInt32(ctx, WTERMSIG(st));
    case QP_W_IFSTOPPED: return qp_bool_new(WIFSTOPPED(st));
    case QP_W_STOPSIG: return JS_NewInt32(ctx, WSTOPSIG(st));
    case QP_W_COREDUMP: return qp_bool_new(WCOREDUMP(st));
    default: return qp_bool_new(WIFCONTINUED(st));
    }
}

/* ---------------------------------------------------------------------- */
/* fork / exec                                                             */

static BOOL qp_os_callable(JSContext *ctx, JSValueConst o)
{
    QPType *t;
    if (JS_IsFunction(ctx, o))
        return TRUE;
    if (JS_VALUE_GET_TAG(o) != JS_TAG_OBJECT)
        return FALSE;
    t = qp_type_of(ctx, o);
    return t->slots.tp_call != NULL || qp_type_from_value(o) != NULL;
}

/* run_at_forkers */
static void qp_os_run_at_forkers(JSContext *ctx, JSValueConst module, const char *prop, int reverse)
{
    JSValue lst = JS_GetPropertyStr(ctx, module, prop), cpy;
    Py_ssize_t n, i;
    JSValue *items;
    if (JS_IsException(lst)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (!qp_list_check(ctx, lst)) {
        JS_FreeValue(ctx, lst);
        return;
    }
    cpy = qp_list_as_tuple(ctx, lst);
    JS_FreeValue(ctx, lst);
    if (JS_IsException(cpy)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    n = QP_TUPLE_SIZE(cpy);
    items = QP_TUPLE_ITEMS(cpy);
    for (i = 0; i < n; i++) {
        JSValueConst f = items[reverse ? n - 1 - i : i];
        JSValue r = qp_call0(ctx, f);
        if (JS_IsException(r)) {
            qp_format_unraisable(ctx, "Exception ignored in atfork callback %R", f);
        } else {
            JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, cpy);
}

QP_OS_FN(qp_os_register_at_fork)
{
    static const char *const names[3] = { "before", "after_in_child", "after_in_parent" };
    static const char *const props[3] = { QP_OS_PROP_FORK_BEFORE, QP_OS_PROP_FORK_CHILD,
                                          QP_OS_PROP_FORK_PARENT };
    int i, any = 0;
    QP_OS_PARSE("register_at_fork", 0, 0, "before", "after_in_child", "after_in_parent");
    for (i = 0; i < 3; i++) {
        if (QP_IS_NULL(a[i]))
            continue;
        any = 1;
        if (!qp_os_callable(ctx, a[i])) {
            qp_raise_type_error(ctx, "'%s' must be callable, not %s", names[i], qp_os_tname(ctx, a[i]));
            return JS_EXCEPTION;
        }
    }
    if (!any)
        return qp_raise_type_error(ctx, "At least one argument is required.");
    for (i = 0; i < 3; i++) {
        JSValue lst;
        int r;
        if (QP_IS_NULL(a[i]))
            continue;
        lst = JS_GetPropertyStr(ctx, self, props[i]);
        if (JS_IsException(lst))
            return lst;
        r = qp_list_append(ctx, lst, a[i]);
        JS_FreeValue(ctx, lst);
        if (r < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* PyOS_BeforeFork / PyOS_AfterFork_Parent / PyOS_AfterFork_Child: the
   at-fork hooks of the posix module (used by _posixsubprocess too). */
static void qp_os_before_fork(JSContext *ctx)
{
    JSValue m = qp_import_module(ctx, "posix");
    if (JS_IsException(m)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    qp_os_run_at_forkers(ctx, m, QP_OS_PROP_FORK_BEFORE, 1);
    JS_FreeValue(ctx, m);
}

static void qp_os_after_fork(JSContext *ctx, int child)
{
    JSValue m = qp_import_module(ctx, "posix");
    if (JS_IsException(m)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    qp_os_run_at_forkers(ctx, m, child ? QP_OS_PROP_FORK_CHILD : QP_OS_PROP_FORK_PARENT, 0);
    JS_FreeValue(ctx, m);
}

/* os.fork() (master_fd NULL) / os.forkpty(): PyOS_BeforeFork, the fork,
   then PyOS_AfterFork_Child or PyOS_AfterFork_Parent */
static JSValue qp_os_do_fork(JSContext *ctx, JSValueConst self, int *master_fd)
{
    pid_t pid;
    int err;
    BOOL threaded;
    qp_os_run_at_forkers(ctx, self, QP_OS_PROP_FORK_BEFORE, 1);
    /* PyOS_BeforeFork: no other thread is in the middle of an import */
    qp_rmutex_lock(ctx, QP_RMUTEX_IMPORT);
    threaded = qp_thread_others_exist(ctx);
    fflush(stdout);
    fflush(stderr);
    pid = master_fd ? forkpty(master_fd, NULL, NULL, NULL) : fork();
    err = errno;
    if (pid == 0) {
        /* PyOS_AfterFork_Child: only this thread exists in the child */
        qp_thread_after_fork_child(ctx);
        qp_signal_after_fork_child();
        qp_rmutex_unlock(ctx, QP_RMUTEX_IMPORT);
        qp_os_run_at_forkers(ctx, self, QP_OS_PROP_FORK_CHILD, 0);
    } else {
        /* warn_about_fork_with_threads, then PyOS_AfterFork_Parent */
        if (pid != -1 && threaded &&
            qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "This process (pid=%d) is multi-threaded, use of %s() may lead "
                    "to deadlocks in the child.", (int)getpid(),
                    master_fd ? "forkpty" : "fork") < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        qp_rmutex_unlock(ctx, QP_RMUTEX_IMPORT);
        qp_os_run_at_forkers(ctx, self, QP_OS_PROP_FORK_PARENT, 0);
        /* forkpty: set O_CLOEXEC on master_fd */
        if (pid != -1 && master_fd && qp_os_set_inheritable(*master_fd, 0) < 0) {
            JSValue exc;
            qp_os_error(ctx, errno);
            exc = qp_exc_fetch(ctx);
            qp_exc_write_unraisable_msg(ctx, exc, "Exception ignored when setting master_fd "
                                        "non-inheritable in forkpty()", QP_NULL);
            JS_FreeValue(ctx, exc);
        }
    }
    if (pid == -1)
        return qp_os_error(ctx, err);
    return JS_NewInt32(ctx, (int)pid);
}

QP_OS_FN(qp_os_fork)
{
    return qp_os_do_fork(ctx, self, NULL);
}

/* forkpty() */
QP_OS_FN(qp_os_forkpty)
{
    int master_fd = -1;
    JSValue items[2];
    items[0] = qp_os_do_fork(ctx, self, &master_fd);
    if (JS_IsException(items[0]))
        return items[0];
    items[1] = JS_NewInt32(ctx, master_fd);
    return qp_tuple_new_steal(ctx, 2, items);
}

static void qp_os_free_strv(JSContext *ctx, char **v, Py_ssize_t n)
{
    Py_ssize_t i;
    if (!v)
        return;
    for (i = 0; i < n; i++)
        js_free(ctx, v[i]);
    js_free(ctx, v);
}

/* fsconvert_strdup */
static char *qp_os_fs_strdup(JSContext *ctx, JSValueConst o)
{
    JSValue b = qp_os_fsconvert(ctx, o);
    const uint8_t *d;
    Py_ssize_t len;
    char *s;
    if (JS_IsException(b))
        return NULL;
    d = qp_bytes_data(b, &len);
    s = js_malloc(ctx, len + 1);
    if (s) {
        memcpy(s, d, len);
        s[len] = '\0';
    }
    JS_FreeValue(ctx, b);
    return s;
}

/* argv list/tuple -> NULL-terminated char*[] (count in *pn) */
static char **qp_os_parse_argv(JSContext *ctx, JSValueConst argv, const char *fn, int execve_style,
                               Py_ssize_t *pn)
{
    JSValue seq;
    JSValue *items;
    Py_ssize_t n, i;
    char **v;
    if (!qp_list_check(ctx, argv) && !qp_tuple_check(ctx, argv)) {
        if (execve_style)
            qp_raise_type_error(ctx, "execve: argv must be a tuple or list");
        else
            qp_raise_type_error(ctx, "%s() arg 2 must be a tuple or list", fn);
        return NULL;
    }
    seq = qp_tuple_check(ctx, argv) ? JS_DupValue(ctx, argv) : qp_list_as_tuple(ctx, argv);
    if (JS_IsException(seq))
        return NULL;
    n = QP_TUPLE_SIZE(seq);
    items = QP_TUPLE_ITEMS(seq);
    if (n < 1) {
        if (execve_style)
            qp_raise_value_error(ctx, "execve: argv must not be empty");
        else
            qp_raise_value_error(ctx, "%s() arg 2 must not be empty", fn);
        JS_FreeValue(ctx, seq);
        return NULL;
    }
    v = js_mallocz(ctx, sizeof(char *) * (n + 1));
    if (!v) {
        JS_FreeValue(ctx, seq);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        v[i] = qp_os_fs_strdup(ctx, items[i]);
        if (!v[i]) {
            qp_os_free_strv(ctx, v, i);
            JS_FreeValue(ctx, seq);
            return NULL;
        }
        if (i == 0 && v[0][0] == '\0') {
            if (execve_style)
                qp_raise_value_error(ctx, "execve: argv first element cannot be empty");
            else
                qp_raise_value_error(ctx, "%s() arg 2 first element cannot be empty", fn);
            qp_os_free_strv(ctx, v, 1);
            JS_FreeValue(ctx, seq);
            return NULL;
        }
    }
    JS_FreeValue(ctx, seq);
    *pn = n;
    return v;
}

/* parse_envlist: mapping -> "k=v" array */
static char **qp_os_parse_envlist(JSContext *ctx, JSValueConst env, Py_ssize_t *pn)
{
    JSValue keys, vals, kt, vt;
    Py_ssize_t n, i, cnt = 0;
    char **e = NULL;
    JSAtom at = JS_NewAtom(ctx, "keys");
    keys = qp_call_method(ctx, env, at, NULL, 0, QP_NULL);
    JS_FreeAtom(ctx, at);
    if (JS_IsException(keys))
        return NULL;
    at = JS_NewAtom(ctx, "values");
    vals = qp_call_method(ctx, env, at, NULL, 0, QP_NULL);
    JS_FreeAtom(ctx, at);
    if (JS_IsException(vals)) {
        JS_FreeValue(ctx, keys);
        return NULL;
    }
    kt = qp_tuple_from_iterable(ctx, keys);
    vt = JS_IsException(kt) ? JS_EXCEPTION : qp_tuple_from_iterable(ctx, vals);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    if (JS_IsException(kt) || JS_IsException(vt)) {
        JS_FreeValue(ctx, kt);
        return NULL;
    }
    n = QP_TUPLE_SIZE(kt);
    if (QP_TUPLE_SIZE(vt) < n)
        n = QP_TUPLE_SIZE(vt);
    e = js_mallocz(ctx, sizeof(char *) * (n + 1));
    if (!e)
        goto fail;
    for (i = 0; i < n; i++) {
        JSValue kb = qp_os_fsconvert(ctx, QP_TUPLE_ITEMS(kt)[i]), vb;
        const uint8_t *kd, *vd;
        Py_ssize_t kl, vl;
        if (JS_IsException(kb))
            goto fail;
        kd = qp_bytes_data(kb, &kl);
        /* Search from index 1 because on Windows starting '=' is allowed for
           defining hidden environment variables. */
        if (kl == 0 || memchr(kd + 1, '=', kl - 1)) {
            JS_FreeValue(ctx, kb);
            qp_raise_value_error(ctx, "illegal environment variable name");
            goto fail;
        }
        vb = qp_os_fsconvert(ctx, QP_TUPLE_ITEMS(vt)[i]);
        if (JS_IsException(vb)) {
            JS_FreeValue(ctx, kb);
            goto fail;
        }
        vd = qp_bytes_data(vb, &vl);
        e[cnt] = js_malloc(ctx, kl + vl + 2);
        if (!e[cnt]) {
            JS_FreeValue(ctx, kb);
            JS_FreeValue(ctx, vb);
            goto fail;
        }
        memcpy(e[cnt], kd, kl);
        e[cnt][kl] = '=';
        memcpy(e[cnt] + kl + 1, vd, vl);
        e[cnt][kl + 1 + vl] = '\0';
        cnt++;
        JS_FreeValue(ctx, kb);
        JS_FreeValue(ctx, vb);
    }
    JS_FreeValue(ctx, kt);
    JS_FreeValue(ctx, vt);
    *pn = cnt;
    return e;
fail:
    qp_os_free_strv(ctx, e, cnt);
    JS_FreeValue(ctx, kt);
    JS_FreeValue(ctx, vt);
    return NULL;
}

/* PyMapping_Check */
static BOOL qp_os_mapping_check(JSContext *ctx, JSValueConst o)
{
    return qp_bi_mapping_check(ctx, o);
}

QP_OS_FN(qp_os_execv)
{
    QPOSPath path = QP_OS_PATH("execv", "path", 0, 0);
    char **argvlist;
    Py_ssize_t argc;
    int err;
    if (qp_arg_check_positional(ctx, "execv", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, args[0], &path) < 0)
        return JS_EXCEPTION;
    argvlist = qp_os_parse_argv(ctx, args[1], "execv", 0, &argc);
    if (!argvlist) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    execv(path.narrow, argvlist);
    err = errno;
    /* If we get here it's definitely an error */
    qp_os_free_strv(ctx, argvlist, argc);
    qp_os_path_clean(ctx, &path);
    return qp_os_error(ctx, err);
}

QP_OS_FN(qp_os_execve)
{
#ifdef __linux__
    QPOSPath path = QP_OS_PATH("execve", "path", 0, 1);
#else
    QPOSPath path = QP_OS_PATH("execve", "path", 0, 0);
#endif
    char **argvlist = NULL, **envlist = NULL;
    Py_ssize_t argc = 0, envc = 0;
    JSValue r;
    QP_OS_PARSE("execve", 3, 3, "path", "argv", "env");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    argvlist = qp_os_parse_argv(ctx, a[1], "execve", 1, &argc);
    if (!argvlist)
        goto fail;
    if (!qp_os_mapping_check(ctx, a[2])) {
        qp_raise_type_error(ctx, "execve: environment must be a mapping object");
        goto fail;
    }
    envlist = qp_os_parse_envlist(ctx, a[2], &envc);
    if (!envlist)
        goto fail;
#ifdef __linux__
    if (path.is_fd)
        fexecve(path.fd, argvlist, envlist);
    else
#endif
        execve(path.narrow, argvlist, envlist);
    /* If we get here it's definitely an error */
    r = qp_os_path_error(ctx, &path, errno);
    qp_os_free_strv(ctx, argvlist, argc);
    qp_os_free_strv(ctx, envlist, envc);
    qp_os_path_clean(ctx, &path);
    return r;
fail:
    qp_os_free_strv(ctx, argvlist, argc);
    qp_os_free_strv(ctx, envlist, envc);
    qp_os_path_clean(ctx, &path);
    return JS_EXCEPTION;
}

QP_OS_FN(qp_os__exit)
{
    int status;
    QP_OS_PARSE("_exit", 1, 1, "status");
    if (qp_os_int(ctx, a[0], &status) < 0)
        return JS_EXCEPTION;
    _exit(status);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_abort)
{
    abort();
    return JS_UNDEFINED;
}

/* ---------------------------------------------------------------------- */
/* misc                                                                    */

QP_OS_FN(qp_os_urandom)
{
    Py_ssize_t size, done = 0;
    char *buf;
    JSValue r;
    if (qp_arg_ssize(ctx, args[0], &size) < 0)
        return JS_EXCEPTION;
    if (size < 0)
        return qp_raise_value_error(ctx, "negative argument not allowed");
    buf = js_malloc(ctx, size ? size : 1);
    if (!buf)
        return JS_EXCEPTION;
    while (done < size) {
        size_t chunk = size - done > 256 ? 256 : (size_t)(size - done);
        if (getentropy(buf + done, chunk) < 0) {
            int err = errno;
            if (err == EINTR) {
                if (qp_signal_check(ctx) < 0) {
                    js_free(ctx, buf);
                    return JS_EXCEPTION;
                }
                continue;
            }
            js_free(ctx, buf);
            return qp_os_error(ctx, err);
        }
        done += chunk;
    }
    r = qp_bytes_new(ctx, buf, size);
    js_free(ctx, buf);
    return r;
}

QP_OS_FN(qp_os_cpu_count)
{
    long n = QP_STATE(ctx)->config.cpu_count;
    if (n > 0)      /* -X cpu_count / PYTHON_CPU_COUNT */
        return qp_os_long(ctx, n);
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return JS_UNDEFINED;
    return qp_os_long(ctx, n);
}

QP_OS_FN(qp_os_uname)
{
    struct utsname u;
    QPType *t = qp_os_get_type(ctx, self, QP_OS_PROP_UNAME);
    JSValue items[5];
    int i;
    if (!t)
        return JS_EXCEPTION;
    if (uname(&u) < 0)
        return qp_os_error(ctx, errno);
    items[0] = qp_os_fsdecode(ctx, u.sysname, strlen(u.sysname));
    items[1] = qp_os_fsdecode(ctx, u.nodename, strlen(u.nodename));
    items[2] = qp_os_fsdecode(ctx, u.release, strlen(u.release));
    items[3] = qp_os_fsdecode(ctx, u.version, strlen(u.version));
    items[4] = qp_os_fsdecode(ctx, u.machine, strlen(u.machine));
    for (i = 0; i < 5; i++) {
        if (JS_IsException(items[i])) {
            for (i = 0; i < 5; i++)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    return qp_structseq_new(ctx, t, items);
}

QP_OS_FN(qp_os_strerror)
{
    int code;
    const char *msg;
    if (qp_arg_int(ctx, args[0], &code) < 0)
        return JS_EXCEPTION;
    msg = strerror(code);
    if (!msg)
        return qp_raise_value_error(ctx, "strerror() argument out of range");
    return qp_str_decode(ctx, msg, (Py_ssize_t)strlen(msg), "utf-8", "surrogateescape");
}

/* PyOS_FSPath */
QP_OS_FN(qp_os_fspath)
{
    JSValue func, res;
    JSValueConst path;
    QP_OS_PARSE("fspath", 1, 1, "path");
    path = a[0];
    if (qp_str_check(ctx, path) || qp_bytes_check(ctx, path))
        return JS_DupValue(ctx, path);
    func = qp_lookup_special(ctx, path, QP_ATOM(ctx, __fspath__));
    if (JS_IsException(func))
        return func;
    if (QP_IS_NULL(func) || JS_IsUndefined(func)) {
        JS_FreeValue(ctx, func);
        return qp_raise_type_error(ctx, "expected str, bytes or os.PathLike object, not %s",
                                   qp_os_tname(ctx, path));
    }
    res = qp_call0(ctx, func);
    JS_FreeValue(ctx, func);
    if (JS_IsException(res))
        return res;
    if (!(qp_str_check(ctx, res) || qp_bytes_check(ctx, res))) {
        qp_raise_type_error(ctx, "expected %s.__fspath__() to return str or bytes, not %s",
                            qp_os_tname(ctx, path), qp_os_tname(ctx, res));
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

QP_OS_FN(qp_os_get_terminal_size)
{
    int fd = STDOUT_FILENO;
    struct winsize w;
    QPType *t;
    JSValue items[2];
    if (qp_arg_check_positional(ctx, "get_terminal_size", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs > 0 && qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    t = qp_os_get_type(ctx, self, QP_OS_PROP_TERMSIZE);
    if (!t)
        return JS_EXCEPTION;
    if (ioctl(fd, TIOCGWINSZ, &w))
        return qp_os_error(ctx, errno);
    items[0] = JS_NewInt32(ctx, w.ws_col);
    items[1] = JS_NewInt32(ctx, w.ws_row);
    return qp_structseq_new(ctx, t, items);
}

QP_OS_FN(qp_os_times)
{
    struct tms t;
    clock_t c;
    double ticks = (double)sysconf(_SC_CLK_TCK);
    QPType *tp = qp_os_get_type(ctx, self, QP_OS_PROP_TIMES);
    JSValue items[5];
    if (!tp)
        return JS_EXCEPTION;
    errno = 0;
    c = times(&t);
    if (c == (clock_t)-1)
        return qp_os_error(ctx, errno);
    items[0] = qp_float_new(ctx, (double)t.tms_utime / ticks);
    items[1] = qp_float_new(ctx, (double)t.tms_stime / ticks);
    items[2] = qp_float_new(ctx, (double)t.tms_cutime / ticks);
    items[3] = qp_float_new(ctx, (double)t.tms_cstime / ticks);
    items[4] = qp_float_new(ctx, (double)c / ticks);
    return qp_structseq_new(ctx, tp, items);
}

/* _Py_device_encoding: UTF-8 mode is the default in 3.15 (PEP 686) */
QP_OS_FN(qp_os_device_encoding)
{
    int fd;
    QP_OS_PARSE("device_encoding", 1, 1, "fd");
    if (qp_os_int(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    if (!isatty(fd))
        return JS_UNDEFINED;
    return qp_str_from_cstr(ctx, "utf-8");
}

QP_OS_FN(qp_os_putenv)
{
    JSValue name, value;
    const uint8_t *nd, *vd;
    Py_ssize_t nl, vl;
    char *n, *v;
    int r, err;
    if (qp_arg_check_positional(ctx, "putenv", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    name = qp_os_fsconvert(ctx, args[0]);
    if (JS_IsException(name))
        return name;
    value = qp_os_fsconvert(ctx, args[1]);
    if (JS_IsException(value)) {
        JS_FreeValue(ctx, name);
        return value;
    }
    nd = qp_bytes_data(name, &nl);
    vd = qp_bytes_data(value, &vl);
    if (nl == 0 || memchr(nd, '=', nl)) {
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, value);
        return qp_raise_value_error(ctx, "illegal environment variable name");
    }
    n = js_malloc(ctx, nl + 1);
    v = js_malloc(ctx, vl + 1);
    if (!n || !v) {
        js_free(ctx, n);
        js_free(ctx, v);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    memcpy(n, nd, nl);
    n[nl] = 0;
    memcpy(v, vd, vl);
    v[vl] = 0;
    r = setenv(n, v, 1);
    err = errno;
    js_free(ctx, n);
    js_free(ctx, v);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, value);
    if (r)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_unsetenv)
{
    JSValue name = qp_os_fsconvert(ctx, args[0]);
    const uint8_t *nd;
    Py_ssize_t nl;
    char *n;
    int r, err;
    if (JS_IsException(name))
        return name;
    nd = qp_bytes_data(name, &nl);
    if (nl == 0 || memchr(nd, '=', nl)) {
        JS_FreeValue(ctx, name);
        return qp_raise_value_error(ctx, "illegal environment variable name");
    }
    n = js_malloc(ctx, nl + 1);
    if (!n) {
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    memcpy(n, nd, nl);
    n[nl] = 0;
    r = unsetenv(n);
    err = errno;
    js_free(ctx, n);
    JS_FreeValue(ctx, name);
    if (r)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_sync)
{
    sync();
    return JS_UNDEFINED;
}

QP_OS_FN(qp_os_ctermid)
{
    char buf[L_ctermid];
    char *r = ctermid(buf);
    if (!r)
        return qp_os_error(ctx, errno);
    return qp_os_fsdecode(ctx, buf, strlen(buf));
}

QP_OS_FN(qp_os_getloadavg)
{
    double l[3];
    JSValue items[3];
    if (getloadavg(l, 3) != 3)
        return qp_raise(ctx, QP_EXC(ctx, OSError), "Load averages are unobtainable");
    items[0] = qp_float_new(ctx, l[0]);
    items[1] = qp_float_new(ctx, l[1]);
    items[2] = qp_float_new(ctx, l[2]);
    return qp_tuple_new_steal(ctx, 3, items);
}

/* ---------------------------------------------------------------------- */
/* sysconf / confstr / pathconf                                            */

typedef struct QPConstDef {
    const char *name;
    int value;
} QPConstDef;

static const QPConstDef qp_os_pathconf_names[] = {
#ifdef _PC_ABI_AIO_XFER_MAX
    {"PC_ABI_AIO_XFER_MAX",     _PC_ABI_AIO_XFER_MAX},
#endif
#ifdef _PC_ABI_ASYNC_IO
    {"PC_ABI_ASYNC_IO", _PC_ABI_ASYNC_IO},
#endif
#ifdef _PC_ASYNC_IO
    {"PC_ASYNC_IO",     _PC_ASYNC_IO},
#endif
#ifdef _PC_CHOWN_RESTRICTED
    {"PC_CHOWN_RESTRICTED",     _PC_CHOWN_RESTRICTED},
#endif
#ifdef _PC_FILESIZEBITS
    {"PC_FILESIZEBITS", _PC_FILESIZEBITS},
#endif
#ifdef _PC_LAST
    {"PC_LAST", _PC_LAST},
#endif
#ifdef _PC_LINK_MAX
    {"PC_LINK_MAX",     _PC_LINK_MAX},
#endif
#ifdef _PC_MAX_CANON
    {"PC_MAX_CANON",    _PC_MAX_CANON},
#endif
#ifdef _PC_MAX_INPUT
    {"PC_MAX_INPUT",    _PC_MAX_INPUT},
#endif
#ifdef _PC_NAME_MAX
    {"PC_NAME_MAX",     _PC_NAME_MAX},
#endif
#ifdef _PC_NO_TRUNC
    {"PC_NO_TRUNC",     _PC_NO_TRUNC},
#endif
#ifdef _PC_PATH_MAX
    {"PC_PATH_MAX",     _PC_PATH_MAX},
#endif
#ifdef _PC_PIPE_BUF
    {"PC_PIPE_BUF",     _PC_PIPE_BUF},
#endif
#ifdef _PC_PRIO_IO
    {"PC_PRIO_IO",      _PC_PRIO_IO},
#endif
#ifdef _PC_SOCK_MAXBUF
    {"PC_SOCK_MAXBUF",  _PC_SOCK_MAXBUF},
#endif
#ifdef _PC_SYNC_IO
    {"PC_SYNC_IO",      _PC_SYNC_IO},
#endif
#ifdef _PC_VDISABLE
    {"PC_VDISABLE",     _PC_VDISABLE},
#endif
#ifdef _PC_ACL_ENABLED
    {"PC_ACL_ENABLED",  _PC_ACL_ENABLED},
#endif
#ifdef _PC_MIN_HOLE_SIZE
    {"PC_MIN_HOLE_SIZE",    _PC_MIN_HOLE_SIZE},
#endif
#ifdef _PC_ALLOC_SIZE_MIN
    {"PC_ALLOC_SIZE_MIN",   _PC_ALLOC_SIZE_MIN},
#endif
#ifdef _PC_REC_INCR_XFER_SIZE
    {"PC_REC_INCR_XFER_SIZE",   _PC_REC_INCR_XFER_SIZE},
#endif
#ifdef _PC_REC_MAX_XFER_SIZE
    {"PC_REC_MAX_XFER_SIZE",    _PC_REC_MAX_XFER_SIZE},
#endif
#ifdef _PC_REC_MIN_XFER_SIZE
    {"PC_REC_MIN_XFER_SIZE",    _PC_REC_MIN_XFER_SIZE},
#endif
#ifdef _PC_REC_XFER_ALIGN
    {"PC_REC_XFER_ALIGN",   _PC_REC_XFER_ALIGN},
#endif
#ifdef _PC_SYMLINK_MAX
    {"PC_SYMLINK_MAX",  _PC_SYMLINK_MAX},
#endif
#ifdef _PC_XATTR_ENABLED
    {"PC_XATTR_ENABLED",    _PC_XATTR_ENABLED},
#endif
#ifdef _PC_XATTR_EXISTS
    {"PC_XATTR_EXISTS", _PC_XATTR_EXISTS},
#endif
#ifdef _PC_TIMESTAMP_RESOLUTION
    {"PC_TIMESTAMP_RESOLUTION", _PC_TIMESTAMP_RESOLUTION},
#endif
    { NULL, 0 }
};

static const QPConstDef qp_os_confstr_names[] = {
#ifdef _CS_ARCHITECTURE
    {"CS_ARCHITECTURE", _CS_ARCHITECTURE},
#endif
#ifdef _CS_GNU_LIBC_VERSION
    {"CS_GNU_LIBC_VERSION",     _CS_GNU_LIBC_VERSION},
#endif
#ifdef _CS_GNU_LIBPTHREAD_VERSION
    {"CS_GNU_LIBPTHREAD_VERSION",       _CS_GNU_LIBPTHREAD_VERSION},
#endif
#ifdef _CS_HOSTNAME
    {"CS_HOSTNAME",     _CS_HOSTNAME},
#endif
#ifdef _CS_HW_PROVIDER
    {"CS_HW_PROVIDER",  _CS_HW_PROVIDER},
#endif
#ifdef _CS_HW_SERIAL
    {"CS_HW_SERIAL",    _CS_HW_SERIAL},
#endif
#ifdef _CS_INITTAB_NAME
    {"CS_INITTAB_NAME", _CS_INITTAB_NAME},
#endif
#ifdef _CS_LFS64_CFLAGS
    {"CS_LFS64_CFLAGS", _CS_LFS64_CFLAGS},
#endif
#ifdef _CS_LFS64_LDFLAGS
    {"CS_LFS64_LDFLAGS",        _CS_LFS64_LDFLAGS},
#endif
#ifdef _CS_LFS64_LIBS
    {"CS_LFS64_LIBS",   _CS_LFS64_LIBS},
#endif
#ifdef _CS_LFS64_LINTFLAGS
    {"CS_LFS64_LINTFLAGS",      _CS_LFS64_LINTFLAGS},
#endif
#ifdef _CS_LFS_CFLAGS
    {"CS_LFS_CFLAGS",   _CS_LFS_CFLAGS},
#endif
#ifdef _CS_LFS_LDFLAGS
    {"CS_LFS_LDFLAGS",  _CS_LFS_LDFLAGS},
#endif
#ifdef _CS_LFS_LIBS
    {"CS_LFS_LIBS",     _CS_LFS_LIBS},
#endif
#ifdef _CS_LFS_LINTFLAGS
    {"CS_LFS_LINTFLAGS",        _CS_LFS_LINTFLAGS},
#endif
#ifdef _CS_MACHINE
    {"CS_MACHINE",      _CS_MACHINE},
#endif
#ifdef _CS_PATH
    {"CS_PATH", _CS_PATH},
#endif
#ifdef _CS_RELEASE
    {"CS_RELEASE",      _CS_RELEASE},
#endif
#ifdef _CS_SRPC_DOMAIN
    {"CS_SRPC_DOMAIN",  _CS_SRPC_DOMAIN},
#endif
#ifdef _CS_SYSNAME
    {"CS_SYSNAME",      _CS_SYSNAME},
#endif
#ifdef _CS_VERSION
    {"CS_VERSION",      _CS_VERSION},
#endif
#ifdef _CS_XBS5_ILP32_OFF32_CFLAGS
    {"CS_XBS5_ILP32_OFF32_CFLAGS",      _CS_XBS5_ILP32_OFF32_CFLAGS},
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LDFLAGS
    {"CS_XBS5_ILP32_OFF32_LDFLAGS",     _CS_XBS5_ILP32_OFF32_LDFLAGS},
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LIBS
    {"CS_XBS5_ILP32_OFF32_LIBS",        _CS_XBS5_ILP32_OFF32_LIBS},
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LINTFLAGS
    {"CS_XBS5_ILP32_OFF32_LINTFLAGS",   _CS_XBS5_ILP32_OFF32_LINTFLAGS},
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_CFLAGS
    {"CS_XBS5_ILP32_OFFBIG_CFLAGS",     _CS_XBS5_ILP32_OFFBIG_CFLAGS},
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LDFLAGS
    {"CS_XBS5_ILP32_OFFBIG_LDFLAGS",    _CS_XBS5_ILP32_OFFBIG_LDFLAGS},
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LIBS
    {"CS_XBS5_ILP32_OFFBIG_LIBS",       _CS_XBS5_ILP32_OFFBIG_LIBS},
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LINTFLAGS
    {"CS_XBS5_ILP32_OFFBIG_LINTFLAGS",  _CS_XBS5_ILP32_OFFBIG_LINTFLAGS},
#endif
#ifdef _CS_XBS5_LP64_OFF64_CFLAGS
    {"CS_XBS5_LP64_OFF64_CFLAGS",       _CS_XBS5_LP64_OFF64_CFLAGS},
#endif
#ifdef _CS_XBS5_LP64_OFF64_LDFLAGS
    {"CS_XBS5_LP64_OFF64_LDFLAGS",      _CS_XBS5_LP64_OFF64_LDFLAGS},
#endif
#ifdef _CS_XBS5_LP64_OFF64_LIBS
    {"CS_XBS5_LP64_OFF64_LIBS", _CS_XBS5_LP64_OFF64_LIBS},
#endif
#ifdef _CS_XBS5_LP64_OFF64_LINTFLAGS
    {"CS_XBS5_LP64_OFF64_LINTFLAGS",    _CS_XBS5_LP64_OFF64_LINTFLAGS},
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_CFLAGS
    {"CS_XBS5_LPBIG_OFFBIG_CFLAGS",     _CS_XBS5_LPBIG_OFFBIG_CFLAGS},
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LDFLAGS
    {"CS_XBS5_LPBIG_OFFBIG_LDFLAGS",    _CS_XBS5_LPBIG_OFFBIG_LDFLAGS},
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LIBS
    {"CS_XBS5_LPBIG_OFFBIG_LIBS",       _CS_XBS5_LPBIG_OFFBIG_LIBS},
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS
    {"CS_XBS5_LPBIG_OFFBIG_LINTFLAGS",  _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS},
#endif
#ifdef _MIPS_CS_AVAIL_PROCESSORS
    {"MIPS_CS_AVAIL_PROCESSORS",        _MIPS_CS_AVAIL_PROCESSORS},
#endif
#ifdef _MIPS_CS_BASE
    {"MIPS_CS_BASE",    _MIPS_CS_BASE},
#endif
#ifdef _MIPS_CS_HOSTID
    {"MIPS_CS_HOSTID",  _MIPS_CS_HOSTID},
#endif
#ifdef _MIPS_CS_HW_NAME
    {"MIPS_CS_HW_NAME", _MIPS_CS_HW_NAME},
#endif
#ifdef _MIPS_CS_NUM_PROCESSORS
    {"MIPS_CS_NUM_PROCESSORS",  _MIPS_CS_NUM_PROCESSORS},
#endif
#ifdef _MIPS_CS_OSREL_MAJ
    {"MIPS_CS_OSREL_MAJ",       _MIPS_CS_OSREL_MAJ},
#endif
#ifdef _MIPS_CS_OSREL_MIN
    {"MIPS_CS_OSREL_MIN",       _MIPS_CS_OSREL_MIN},
#endif
#ifdef _MIPS_CS_OSREL_PATCH
    {"MIPS_CS_OSREL_PATCH",     _MIPS_CS_OSREL_PATCH},
#endif
#ifdef _MIPS_CS_OS_NAME
    {"MIPS_CS_OS_NAME", _MIPS_CS_OS_NAME},
#endif
#ifdef _MIPS_CS_OS_PROVIDER
    {"MIPS_CS_OS_PROVIDER",     _MIPS_CS_OS_PROVIDER},
#endif
#ifdef _MIPS_CS_PROCESSORS
    {"MIPS_CS_PROCESSORS",      _MIPS_CS_PROCESSORS},
#endif
#ifdef _MIPS_CS_SERIAL
    {"MIPS_CS_SERIAL",  _MIPS_CS_SERIAL},
#endif
#ifdef _MIPS_CS_VENDOR
    {"MIPS_CS_VENDOR",  _MIPS_CS_VENDOR},
#endif
    { NULL, 0 }
};

static const QPConstDef qp_os_sysconf_names[] = {
#ifdef _SC_2_CHAR_TERM
    {"SC_2_CHAR_TERM",  _SC_2_CHAR_TERM},
#endif
#ifdef _SC_2_C_BIND
    {"SC_2_C_BIND",     _SC_2_C_BIND},
#endif
#ifdef _SC_2_C_DEV
    {"SC_2_C_DEV",      _SC_2_C_DEV},
#endif
#ifdef _SC_2_C_VERSION
    {"SC_2_C_VERSION",  _SC_2_C_VERSION},
#endif
#ifdef _SC_2_FORT_DEV
    {"SC_2_FORT_DEV",   _SC_2_FORT_DEV},
#endif
#ifdef _SC_2_FORT_RUN
    {"SC_2_FORT_RUN",   _SC_2_FORT_RUN},
#endif
#ifdef _SC_2_LOCALEDEF
    {"SC_2_LOCALEDEF",  _SC_2_LOCALEDEF},
#endif
#ifdef _SC_2_SW_DEV
    {"SC_2_SW_DEV",     _SC_2_SW_DEV},
#endif
#ifdef _SC_2_UPE
    {"SC_2_UPE",        _SC_2_UPE},
#endif
#ifdef _SC_2_VERSION
    {"SC_2_VERSION",    _SC_2_VERSION},
#endif
#ifdef _SC_ABI_ASYNCHRONOUS_IO
    {"SC_ABI_ASYNCHRONOUS_IO",  _SC_ABI_ASYNCHRONOUS_IO},
#endif
#ifdef _SC_ACL
    {"SC_ACL",  _SC_ACL},
#endif
#ifdef _SC_AIO_LISTIO_MAX
    {"SC_AIO_LISTIO_MAX",       _SC_AIO_LISTIO_MAX},
#endif
#ifdef _SC_AIO_MAX
    {"SC_AIO_MAX",      _SC_AIO_MAX},
#endif
#ifdef _SC_AIO_PRIO_DELTA_MAX
    {"SC_AIO_PRIO_DELTA_MAX",   _SC_AIO_PRIO_DELTA_MAX},
#endif
#ifdef _SC_ARG_MAX
    {"SC_ARG_MAX",      _SC_ARG_MAX},
#endif
#ifdef _SC_ASYNCHRONOUS_IO
    {"SC_ASYNCHRONOUS_IO",      _SC_ASYNCHRONOUS_IO},
#endif
#ifdef _SC_ATEXIT_MAX
    {"SC_ATEXIT_MAX",   _SC_ATEXIT_MAX},
#endif
#ifdef _SC_AUDIT
    {"SC_AUDIT",        _SC_AUDIT},
#endif
#ifdef _SC_AVPHYS_PAGES
    {"SC_AVPHYS_PAGES", _SC_AVPHYS_PAGES},
#endif
#ifdef _SC_BC_BASE_MAX
    {"SC_BC_BASE_MAX",  _SC_BC_BASE_MAX},
#endif
#ifdef _SC_BC_DIM_MAX
    {"SC_BC_DIM_MAX",   _SC_BC_DIM_MAX},
#endif
#ifdef _SC_BC_SCALE_MAX
    {"SC_BC_SCALE_MAX", _SC_BC_SCALE_MAX},
#endif
#ifdef _SC_BC_STRING_MAX
    {"SC_BC_STRING_MAX",        _SC_BC_STRING_MAX},
#endif
#ifdef _SC_CAP
    {"SC_CAP",  _SC_CAP},
#endif
#ifdef _SC_CHARCLASS_NAME_MAX
    {"SC_CHARCLASS_NAME_MAX",   _SC_CHARCLASS_NAME_MAX},
#endif
#ifdef _SC_CHAR_BIT
    {"SC_CHAR_BIT",     _SC_CHAR_BIT},
#endif
#ifdef _SC_CHAR_MAX
    {"SC_CHAR_MAX",     _SC_CHAR_MAX},
#endif
#ifdef _SC_CHAR_MIN
    {"SC_CHAR_MIN",     _SC_CHAR_MIN},
#endif
#ifdef _SC_CHILD_MAX
    {"SC_CHILD_MAX",    _SC_CHILD_MAX},
#endif
#ifdef _SC_CLK_TCK
    {"SC_CLK_TCK",      _SC_CLK_TCK},
#endif
#ifdef _SC_COHER_BLKSZ
    {"SC_COHER_BLKSZ",  _SC_COHER_BLKSZ},
#endif
#ifdef _SC_COLL_WEIGHTS_MAX
    {"SC_COLL_WEIGHTS_MAX",     _SC_COLL_WEIGHTS_MAX},
#endif
#ifdef _SC_DCACHE_ASSOC
    {"SC_DCACHE_ASSOC", _SC_DCACHE_ASSOC},
#endif
#ifdef _SC_DCACHE_BLKSZ
    {"SC_DCACHE_BLKSZ", _SC_DCACHE_BLKSZ},
#endif
#ifdef _SC_DCACHE_LINESZ
    {"SC_DCACHE_LINESZ",        _SC_DCACHE_LINESZ},
#endif
#ifdef _SC_DCACHE_SZ
    {"SC_DCACHE_SZ",    _SC_DCACHE_SZ},
#endif
#ifdef _SC_DCACHE_TBLKSZ
    {"SC_DCACHE_TBLKSZ",        _SC_DCACHE_TBLKSZ},
#endif
#ifdef _SC_DELAYTIMER_MAX
    {"SC_DELAYTIMER_MAX",       _SC_DELAYTIMER_MAX},
#endif
#ifdef _SC_EQUIV_CLASS_MAX
    {"SC_EQUIV_CLASS_MAX",      _SC_EQUIV_CLASS_MAX},
#endif
#ifdef _SC_EXPR_NEST_MAX
    {"SC_EXPR_NEST_MAX",        _SC_EXPR_NEST_MAX},
#endif
#ifdef _SC_FSYNC
    {"SC_FSYNC",        _SC_FSYNC},
#endif
#ifdef _SC_GETGR_R_SIZE_MAX
    {"SC_GETGR_R_SIZE_MAX",     _SC_GETGR_R_SIZE_MAX},
#endif
#ifdef _SC_GETPW_R_SIZE_MAX
    {"SC_GETPW_R_SIZE_MAX",     _SC_GETPW_R_SIZE_MAX},
#endif
#ifdef _SC_ICACHE_ASSOC
    {"SC_ICACHE_ASSOC", _SC_ICACHE_ASSOC},
#endif
#ifdef _SC_ICACHE_BLKSZ
    {"SC_ICACHE_BLKSZ", _SC_ICACHE_BLKSZ},
#endif
#ifdef _SC_ICACHE_LINESZ
    {"SC_ICACHE_LINESZ",        _SC_ICACHE_LINESZ},
#endif
#ifdef _SC_ICACHE_SZ
    {"SC_ICACHE_SZ",    _SC_ICACHE_SZ},
#endif
#ifdef _SC_INF
    {"SC_INF",  _SC_INF},
#endif
#ifdef _SC_INT_MAX
    {"SC_INT_MAX",      _SC_INT_MAX},
#endif
#ifdef _SC_INT_MIN
    {"SC_INT_MIN",      _SC_INT_MIN},
#endif
#ifdef _SC_IOV_MAX
    {"SC_IOV_MAX",      _SC_IOV_MAX},
#endif
#ifdef _SC_IP_SECOPTS
    {"SC_IP_SECOPTS",   _SC_IP_SECOPTS},
#endif
#ifdef _SC_JOB_CONTROL
    {"SC_JOB_CONTROL",  _SC_JOB_CONTROL},
#endif
#ifdef _SC_KERN_POINTERS
    {"SC_KERN_POINTERS",        _SC_KERN_POINTERS},
#endif
#ifdef _SC_KERN_SIM
    {"SC_KERN_SIM",     _SC_KERN_SIM},
#endif
#ifdef _SC_LINE_MAX
    {"SC_LINE_MAX",     _SC_LINE_MAX},
#endif
#ifdef _SC_LOGIN_NAME_MAX
    {"SC_LOGIN_NAME_MAX",       _SC_LOGIN_NAME_MAX},
#endif
#ifdef _SC_LOGNAME_MAX
    {"SC_LOGNAME_MAX",  _SC_LOGNAME_MAX},
#endif
#ifdef _SC_LONG_BIT
    {"SC_LONG_BIT",     _SC_LONG_BIT},
#endif
#ifdef _SC_MAC
    {"SC_MAC",  _SC_MAC},
#endif
#ifdef _SC_MAPPED_FILES
    {"SC_MAPPED_FILES", _SC_MAPPED_FILES},
#endif
#ifdef _SC_MAXPID
    {"SC_MAXPID",       _SC_MAXPID},
#endif
#ifdef _SC_MB_LEN_MAX
    {"SC_MB_LEN_MAX",   _SC_MB_LEN_MAX},
#endif
#ifdef _SC_MEMLOCK
    {"SC_MEMLOCK",      _SC_MEMLOCK},
#endif
#ifdef _SC_MEMLOCK_RANGE
    {"SC_MEMLOCK_RANGE",        _SC_MEMLOCK_RANGE},
#endif
#ifdef _SC_MEMORY_PROTECTION
    {"SC_MEMORY_PROTECTION",    _SC_MEMORY_PROTECTION},
#endif
#ifdef _SC_MESSAGE_PASSING
    {"SC_MESSAGE_PASSING",      _SC_MESSAGE_PASSING},
#endif
#ifdef _SC_MMAP_FIXED_ALIGNMENT
    {"SC_MMAP_FIXED_ALIGNMENT", _SC_MMAP_FIXED_ALIGNMENT},
#endif
#ifdef _SC_MQ_OPEN_MAX
    {"SC_MQ_OPEN_MAX",  _SC_MQ_OPEN_MAX},
#endif
#ifdef _SC_MQ_PRIO_MAX
    {"SC_MQ_PRIO_MAX",  _SC_MQ_PRIO_MAX},
#endif
#ifdef _SC_NACLS_MAX
    {"SC_NACLS_MAX",    _SC_NACLS_MAX},
#endif
#ifdef _SC_NGROUPS_MAX
    {"SC_NGROUPS_MAX",  _SC_NGROUPS_MAX},
#endif
#ifdef _SC_NL_ARGMAX
    {"SC_NL_ARGMAX",    _SC_NL_ARGMAX},
#endif
#ifdef _SC_NL_LANGMAX
    {"SC_NL_LANGMAX",   _SC_NL_LANGMAX},
#endif
#ifdef _SC_NL_MSGMAX
    {"SC_NL_MSGMAX",    _SC_NL_MSGMAX},
#endif
#ifdef _SC_NL_NMAX
    {"SC_NL_NMAX",      _SC_NL_NMAX},
#endif
#ifdef _SC_NL_SETMAX
    {"SC_NL_SETMAX",    _SC_NL_SETMAX},
#endif
#ifdef _SC_NL_TEXTMAX
    {"SC_NL_TEXTMAX",   _SC_NL_TEXTMAX},
#endif
#ifdef _SC_NPROCESSORS_CONF
    {"SC_NPROCESSORS_CONF",     _SC_NPROCESSORS_CONF},
#endif
#ifdef _SC_NPROCESSORS_ONLN
    {"SC_NPROCESSORS_ONLN",     _SC_NPROCESSORS_ONLN},
#endif
#ifdef _SC_NPROC_CONF
    {"SC_NPROC_CONF",   _SC_NPROC_CONF},
#endif
#ifdef _SC_NPROC_ONLN
    {"SC_NPROC_ONLN",   _SC_NPROC_ONLN},
#endif
#ifdef _SC_NZERO
    {"SC_NZERO",        _SC_NZERO},
#endif
#ifdef _SC_OPEN_MAX
    {"SC_OPEN_MAX",     _SC_OPEN_MAX},
#endif
#ifdef _SC_PAGESIZE
    {"SC_PAGESIZE",     _SC_PAGESIZE},
#endif
#ifdef _SC_PAGE_SIZE
    {"SC_PAGE_SIZE",    _SC_PAGE_SIZE},
#endif
#ifdef _SC_AIX_REALMEM
    {"SC_AIX_REALMEM", _SC_AIX_REALMEM},
#endif
#ifdef _SC_PASS_MAX
    {"SC_PASS_MAX",     _SC_PASS_MAX},
#endif
#ifdef _SC_PHYS_PAGES
    {"SC_PHYS_PAGES",   _SC_PHYS_PAGES},
#endif
#ifdef _SC_PII
    {"SC_PII",  _SC_PII},
#endif
#ifdef _SC_PII_INTERNET
    {"SC_PII_INTERNET", _SC_PII_INTERNET},
#endif
#ifdef _SC_PII_INTERNET_DGRAM
    {"SC_PII_INTERNET_DGRAM",   _SC_PII_INTERNET_DGRAM},
#endif
#ifdef _SC_PII_INTERNET_STREAM
    {"SC_PII_INTERNET_STREAM",  _SC_PII_INTERNET_STREAM},
#endif
#ifdef _SC_PII_OSI
    {"SC_PII_OSI",      _SC_PII_OSI},
#endif
#ifdef _SC_PII_OSI_CLTS
    {"SC_PII_OSI_CLTS", _SC_PII_OSI_CLTS},
#endif
#ifdef _SC_PII_OSI_COTS
    {"SC_PII_OSI_COTS", _SC_PII_OSI_COTS},
#endif
#ifdef _SC_PII_OSI_M
    {"SC_PII_OSI_M",    _SC_PII_OSI_M},
#endif
#ifdef _SC_PII_SOCKET
    {"SC_PII_SOCKET",   _SC_PII_SOCKET},
#endif
#ifdef _SC_PII_XTI
    {"SC_PII_XTI",      _SC_PII_XTI},
#endif
#ifdef _SC_POLL
    {"SC_POLL", _SC_POLL},
#endif
#ifdef _SC_PRIORITIZED_IO
    {"SC_PRIORITIZED_IO",       _SC_PRIORITIZED_IO},
#endif
#ifdef _SC_PRIORITY_SCHEDULING
    {"SC_PRIORITY_SCHEDULING",  _SC_PRIORITY_SCHEDULING},
#endif
#ifdef _SC_REALTIME_SIGNALS
    {"SC_REALTIME_SIGNALS",     _SC_REALTIME_SIGNALS},
#endif
#ifdef _SC_RE_DUP_MAX
    {"SC_RE_DUP_MAX",   _SC_RE_DUP_MAX},
#endif
#ifdef _SC_RTSIG_MAX
    {"SC_RTSIG_MAX",    _SC_RTSIG_MAX},
#endif
#ifdef _SC_SAVED_IDS
    {"SC_SAVED_IDS",    _SC_SAVED_IDS},
#endif
#ifdef _SC_SCHAR_MAX
    {"SC_SCHAR_MAX",    _SC_SCHAR_MAX},
#endif
#ifdef _SC_SCHAR_MIN
    {"SC_SCHAR_MIN",    _SC_SCHAR_MIN},
#endif
#ifdef _SC_SELECT
    {"SC_SELECT",       _SC_SELECT},
#endif
#ifdef _SC_SEMAPHORES
    {"SC_SEMAPHORES",   _SC_SEMAPHORES},
#endif
#ifdef _SC_SEM_NSEMS_MAX
    {"SC_SEM_NSEMS_MAX",        _SC_SEM_NSEMS_MAX},
#endif
#ifdef _SC_SEM_VALUE_MAX
    {"SC_SEM_VALUE_MAX",        _SC_SEM_VALUE_MAX},
#endif
#ifdef _SC_SHARED_MEMORY_OBJECTS
    {"SC_SHARED_MEMORY_OBJECTS",        _SC_SHARED_MEMORY_OBJECTS},
#endif
#ifdef _SC_SHRT_MAX
    {"SC_SHRT_MAX",     _SC_SHRT_MAX},
#endif
#ifdef _SC_SHRT_MIN
    {"SC_SHRT_MIN",     _SC_SHRT_MIN},
#endif
#ifdef _SC_SIGQUEUE_MAX
    {"SC_SIGQUEUE_MAX", _SC_SIGQUEUE_MAX},
#endif
#ifdef _SC_SIGRT_MAX
    {"SC_SIGRT_MAX",    _SC_SIGRT_MAX},
#endif
#ifdef _SC_SIGRT_MIN
    {"SC_SIGRT_MIN",    _SC_SIGRT_MIN},
#endif
#ifdef _SC_SOFTPOWER
    {"SC_SOFTPOWER",    _SC_SOFTPOWER},
#endif
#ifdef _SC_SPLIT_CACHE
    {"SC_SPLIT_CACHE",  _SC_SPLIT_CACHE},
#endif
#ifdef _SC_SSIZE_MAX
    {"SC_SSIZE_MAX",    _SC_SSIZE_MAX},
#endif
#ifdef _SC_STACK_PROT
    {"SC_STACK_PROT",   _SC_STACK_PROT},
#endif
#ifdef _SC_STREAM_MAX
    {"SC_STREAM_MAX",   _SC_STREAM_MAX},
#endif
#ifdef _SC_SYNCHRONIZED_IO
    {"SC_SYNCHRONIZED_IO",      _SC_SYNCHRONIZED_IO},
#endif
#ifdef _SC_THREADS
    {"SC_THREADS",      _SC_THREADS},
#endif
#ifdef _SC_THREAD_ATTR_STACKADDR
    {"SC_THREAD_ATTR_STACKADDR",        _SC_THREAD_ATTR_STACKADDR},
#endif
#ifdef _SC_THREAD_ATTR_STACKSIZE
    {"SC_THREAD_ATTR_STACKSIZE",        _SC_THREAD_ATTR_STACKSIZE},
#endif
#ifdef _SC_THREAD_DESTRUCTOR_ITERATIONS
    {"SC_THREAD_DESTRUCTOR_ITERATIONS", _SC_THREAD_DESTRUCTOR_ITERATIONS},
#endif
#ifdef _SC_THREAD_KEYS_MAX
    {"SC_THREAD_KEYS_MAX",      _SC_THREAD_KEYS_MAX},
#endif
#ifdef _SC_THREAD_PRIORITY_SCHEDULING
    {"SC_THREAD_PRIORITY_SCHEDULING",   _SC_THREAD_PRIORITY_SCHEDULING},
#endif
#ifdef _SC_THREAD_PRIO_INHERIT
    {"SC_THREAD_PRIO_INHERIT",  _SC_THREAD_PRIO_INHERIT},
#endif
#ifdef _SC_THREAD_PRIO_PROTECT
    {"SC_THREAD_PRIO_PROTECT",  _SC_THREAD_PRIO_PROTECT},
#endif
#ifdef _SC_THREAD_PROCESS_SHARED
    {"SC_THREAD_PROCESS_SHARED",        _SC_THREAD_PROCESS_SHARED},
#endif
#ifdef _SC_THREAD_SAFE_FUNCTIONS
    {"SC_THREAD_SAFE_FUNCTIONS",        _SC_THREAD_SAFE_FUNCTIONS},
#endif
#ifdef _SC_THREAD_STACK_MIN
    {"SC_THREAD_STACK_MIN",     _SC_THREAD_STACK_MIN},
#endif
#ifdef _SC_THREAD_THREADS_MAX
    {"SC_THREAD_THREADS_MAX",   _SC_THREAD_THREADS_MAX},
#endif
#ifdef _SC_TIMERS
    {"SC_TIMERS",       _SC_TIMERS},
#endif
#ifdef _SC_TIMER_MAX
    {"SC_TIMER_MAX",    _SC_TIMER_MAX},
#endif
#ifdef _SC_TTY_NAME_MAX
    {"SC_TTY_NAME_MAX", _SC_TTY_NAME_MAX},
#endif
#ifdef _SC_TZNAME_MAX
    {"SC_TZNAME_MAX",   _SC_TZNAME_MAX},
#endif
#ifdef _SC_T_IOV_MAX
    {"SC_T_IOV_MAX",    _SC_T_IOV_MAX},
#endif
#ifdef _SC_UCHAR_MAX
    {"SC_UCHAR_MAX",    _SC_UCHAR_MAX},
#endif
#ifdef _SC_UINT_MAX
    {"SC_UINT_MAX",     _SC_UINT_MAX},
#endif
#ifdef _SC_UIO_MAXIOV
    {"SC_UIO_MAXIOV",   _SC_UIO_MAXIOV},
#endif
#ifdef _SC_ULONG_MAX
    {"SC_ULONG_MAX",    _SC_ULONG_MAX},
#endif
#ifdef _SC_USHRT_MAX
    {"SC_USHRT_MAX",    _SC_USHRT_MAX},
#endif
#ifdef _SC_VERSION
    {"SC_VERSION",      _SC_VERSION},
#endif
#ifdef _SC_WORD_BIT
    {"SC_WORD_BIT",     _SC_WORD_BIT},
#endif
#ifdef _SC_XBS5_ILP32_OFF32
    {"SC_XBS5_ILP32_OFF32",     _SC_XBS5_ILP32_OFF32},
#endif
#ifdef _SC_XBS5_ILP32_OFFBIG
    {"SC_XBS5_ILP32_OFFBIG",    _SC_XBS5_ILP32_OFFBIG},
#endif
#ifdef _SC_XBS5_LP64_OFF64
    {"SC_XBS5_LP64_OFF64",      _SC_XBS5_LP64_OFF64},
#endif
#ifdef _SC_XBS5_LPBIG_OFFBIG
    {"SC_XBS5_LPBIG_OFFBIG",    _SC_XBS5_LPBIG_OFFBIG},
#endif
#ifdef _SC_XOPEN_CRYPT
    {"SC_XOPEN_CRYPT",  _SC_XOPEN_CRYPT},
#endif
#ifdef _SC_XOPEN_ENH_I18N
    {"SC_XOPEN_ENH_I18N",       _SC_XOPEN_ENH_I18N},
#endif
#ifdef _SC_XOPEN_LEGACY
    {"SC_XOPEN_LEGACY", _SC_XOPEN_LEGACY},
#endif
#ifdef _SC_XOPEN_REALTIME
    {"SC_XOPEN_REALTIME",       _SC_XOPEN_REALTIME},
#endif
#ifdef _SC_XOPEN_REALTIME_THREADS
    {"SC_XOPEN_REALTIME_THREADS",       _SC_XOPEN_REALTIME_THREADS},
#endif
#ifdef _SC_XOPEN_SHM
    {"SC_XOPEN_SHM",    _SC_XOPEN_SHM},
#endif
#ifdef _SC_XOPEN_UNIX
    {"SC_XOPEN_UNIX",   _SC_XOPEN_UNIX},
#endif
#ifdef _SC_XOPEN_VERSION
    {"SC_XOPEN_VERSION",        _SC_XOPEN_VERSION},
#endif
#ifdef _SC_XOPEN_XCU_VERSION
    {"SC_XOPEN_XCU_VERSION",    _SC_XOPEN_XCU_VERSION},
#endif
#ifdef _SC_XOPEN_XPG2
    {"SC_XOPEN_XPG2",   _SC_XOPEN_XPG2},
#endif
#ifdef _SC_XOPEN_XPG3
    {"SC_XOPEN_XPG3",   _SC_XOPEN_XPG3},
#endif
#ifdef _SC_XOPEN_XPG4
    {"SC_XOPEN_XPG4",   _SC_XOPEN_XPG4},
#endif
#ifdef _SC_MINSIGSTKSZ
    {"SC_MINSIGSTKSZ",   _SC_MINSIGSTKSZ},
#endif
    { NULL, 0 }
};

/* conv_confname */
static int qp_os_confname(JSContext *ctx, JSValueConst arg, const QPConstDef *table, int *out)
{
    if (qp_long_check(ctx, arg))
        return qp_arg_int(ctx, arg, out);
    if (qp_str_check(ctx, arg)) {
        const char *s = qp_str_utf8(ctx, arg, NULL);
        int i;
        if (!s)
            return -1;
        for (i = 0; table[i].name; i++) {
            if (!strcmp(table[i].name, s)) {
                *out = table[i].value;
                return 0;
            }
        }
        qp_raise_value_error(ctx, "unrecognized configuration name");
        return -1;
    }
    qp_raise_type_error(ctx, "configuration names must be strings or integers");
    return -1;
}

QP_OS_FN(qp_os_sysconf)
{
    int name;
    long v;
    if (qp_os_confname(ctx, args[0], qp_os_sysconf_names, &name) < 0)
        return JS_EXCEPTION;
    errno = 0;
    v = sysconf(name);
    if (v == -1 && errno != 0)
        return qp_os_error(ctx, errno);
    return qp_os_long(ctx, v);
}

QP_OS_FN(qp_os_confstr)
{
    int name;
    size_t len;
    char buf[256];
    if (qp_os_confname(ctx, args[0], qp_os_confstr_names, &name) < 0)
        return JS_EXCEPTION;
    errno = 0;
    len = confstr(name, buf, sizeof(buf));
    if (len == 0) {
        if (errno)
            return qp_os_error(ctx, errno);
        return JS_UNDEFINED;
    }
    if (len >= sizeof(buf)) {
        char *b = js_malloc(ctx, len);
        JSValue r;
        if (!b)
            return JS_EXCEPTION;
        confstr(name, b, len);
        r = qp_os_fsdecode(ctx, b, len - 1);
        js_free(ctx, b);
        return r;
    }
    return qp_os_fsdecode(ctx, buf, len - 1);
}

QP_OS_FN(qp_os_fpathconf)
{
    int fd, name;
    long v;
    if (qp_arg_check_positional(ctx, "fpathconf", nargs, 2, 2) < 0 ||
        qp_object_as_fd(ctx, args[0], &fd) < 0 ||
        qp_os_confname(ctx, args[1], qp_os_pathconf_names, &name) < 0)
        return JS_EXCEPTION;
    errno = 0;
    v = fpathconf(fd, name);
    if (v == -1 && errno != 0)
        return qp_os_error(ctx, errno);
    return qp_os_long(ctx, v);
}

QP_OS_FN(qp_os_pathconf)
{
    QPOSPath path = QP_OS_PATH("pathconf", "path", 0, 1);
    int name, err;
    long v;
    JSValue r;
    QP_OS_PARSE("pathconf", 2, 2, "path", "name");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_confname(ctx, a[1], qp_os_pathconf_names, &name) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    errno = 0;
    v = path.is_fd ? fpathconf(path.fd, name) : pathconf(path.narrow, name);
    err = errno;
    if (v == -1 && err != 0)
        r = qp_os_path_error(ctx, &path, err);
    else
        r = qp_os_long(ctx, v);
    qp_os_path_clean(ctx, &path);
    return r;
}

static int qp_os_conftable(JSContext *ctx, JSValueConst module, const char *name,
                           const QPConstDef *table)
{
    JSValue d = qp_dict_new(ctx);
    int i;
    if (JS_IsException(d))
        return -1;
    for (i = 0; table[i].name; i++) {
        JSAtom at = JS_NewAtom(ctx, table[i].name);
        int r = qp_dict_set_item_atom(ctx, d, at, JS_NewInt32(ctx, table[i].value));
        JS_FreeAtom(ctx, at);
        if (r < 0) {
            JS_FreeValue(ctx, d);
            return -1;
        }
    }
    return qp_os_set(ctx, module, name, d);
}

/* ---------------------------------------------------------------------- */
/* processes, terminals, scheduling and devices                            */

/* PySequence_Fast(o, msg) as a tuple: the items stay alive while the
   conversions below run Python code */
static JSValue qp_os_seq_tuple(JSContext *ctx, JSValueConst o, const char *msg)
{
    JSValue it, r;
    if (qp_tuple_check_exact(ctx, o))
        return JS_DupValue(ctx, o);
    it = qp_iter(ctx, o);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            qp_raise_type_error(ctx, "%s", msg);
        }
        return JS_EXCEPTION;
    }
    r = qp_tuple_from_iterable(ctx, it);
    JS_FreeValue(ctx, it);
    return r;
}

/* unsigned_int / unsigned_long / unsigned_long_long converters of size
   'size' (PyLong_AsNativeBytes with ALLOW_INDEX | UNSIGNED_BUFFER).
   bitwise (clinic bitwise=True): PyIndex_Check (TypeError naming the
   argument 'arg' of 'fn'), the value modulo 2**(8*size) and a
   DeprecationWarning when it does not fit; otherwise REJECT_NEGATIVE
   (ValueError) and OverflowError "Python int too large for C <ctype>". */
static int qp_os_unsigned(JSContext *ctx, JSValueConst o, int size, int bitwise,
                          const char *fn, const char *arg, const char *ctype, uint64_t *out)
{
    JSValue v;
    JSValueConst u;
    int64_t x;
    int overflow = 0, fits;
    uint64_t r;
    if (bitwise && !qp_os_index_check(ctx, o)) {
        qp_raise_type_error(ctx, "%s() %s must be int, not %s", fn, arg, qp_os_tname(ctx, o));
        return -1;
    }
    v = qp_index(ctx, o);
    if (JS_IsException(v))
        return -1;
    u = qp_long_unwrap(v);
    if (qp_long_as_int64_overflow(v, &x, &overflow) == 0 && !overflow) {
        r = (uint64_t)x;
        if (size >= 8)
            fits = bitwise || x >= 0;
        else if (bitwise)
            fits = x >= -((int64_t)1 << (size * 8 - 1)) && x < ((int64_t)1 << (size * 8));
        else
            fits = x >= 0 && x < ((int64_t)1 << (size * 8));
    } else {
        fits = size >= 8 && overflow > 0 && qp_li_numbits(u) <= 64;
        if (JS_ToBigUint64(ctx, &r, u) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    if (!fits) {
        if (!bitwise) {
            if (overflow < 0 || (!overflow && x < 0))
                qp_raise_value_error(ctx, "Cannot convert negative int");
            else
                qp_raise_overflow_error(ctx, "Python int too large for C %s", ctype);
            return -1;
        }
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1, "integer value out of range") < 0)
            return -1;
    }
    if (size < 8)
        r &= ((uint64_t)1 << (size * 8)) - 1;
    *out = r;
    return 0;
}

/* PyLong_AsPid */
static int qp_os_pid(JSContext *ctx, JSValueConst o, pid_t *out)
{
    int v;
    if (qp_arg_int(ctx, o, &v) < 0)
        return -1;
    *out = (pid_t)v;
    return 0;
}

/* _Py_Dev_Converter */
static int qp_os_dev_conv(JSContext *ctx, JSValueConst o, dev_t *out)
{
    JSValue v = qp_index(ctx, o);
    JSValueConst u;
    int64_t x;
    int overflow = 0;
    uint64_t r;
    if (JS_IsException(v))
        return -1;
    u = qp_long_unwrap(v);
    qp_long_as_int64_overflow(v, &x, &overflow);
#ifdef NODEV
    if (!overflow && x == (int64_t)NODEV) {
        JS_FreeValue(ctx, v);
        *out = NODEV;
        return 0;
    }
#endif
    if (overflow < 0 || (!overflow && x < 0)) {
        JS_FreeValue(ctx, v);
        qp_raise_overflow_error(ctx, "can't convert negative int to unsigned");
        return -1;
    }
    if (!overflow) {
        r = (uint64_t)x;
    } else if (qp_li_numbits(u) > 64) {
        JS_FreeValue(ctx, v);
        qp_raise_overflow_error(ctx, "int too big to convert");
        return -1;
    } else if (JS_ToBigUint64(ctx, &r, u) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if ((uint64_t)(dev_t)r != r) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C dev_t");
        return -1;
    }
    *out = (dev_t)r;
    return 0;
}

/* _PyLong_FromDev */
static JSValue qp_os_long_from_dev(JSContext *ctx, dev_t d)
{
#ifdef NODEV
    if (d == NODEV)
        return qp_long_from_int64(ctx, (int64_t)(long long)d);
#endif
    return qp_long_from_uint64(ctx, (uint64_t)d);
}

/* argument_unavailable_error */
static JSValue qp_os_argument_unavailable(JSContext *ctx, const char *fn, const char *arg)
{
    qp_os_unavailable(ctx, fn, arg);
    return JS_EXCEPTION;
}

/* os.ttyname(fd, /) */
QP_OS_FN(qp_os_ttyname)
{
    int fd, ret;
    long size;
    char *buffer;
    JSValue r;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    size = sysconf(_SC_TTY_NAME_MAX);
    if (size == -1)
        return qp_os_error(ctx, errno);
    buffer = js_malloc(ctx, size);
    if (!buffer)
        return JS_EXCEPTION;
    ret = ttyname_r(fd, buffer, size);
    if (ret != 0) {
        js_free(ctx, buffer);
        return qp_os_error(ctx, ret);
    }
    r = qp_os_fsdecode(ctx, buffer, strlen(buffer));
    js_free(ctx, buffer);
    return r;
}

#ifdef QP_OS_HAVE_CHFLAGS
/* chflags (magic 0) / lchflags (magic 1) */
QP_OS_FN(qp_os_chflags)
{
    static const char *const kw[] = { "path", "flags", "follow_symlinks", NULL };
    static const QPArgSpec specs[2] = { { "chflags", kw, 2, 3, 0 }, { "lchflags", kw, 2, 2, 0 } };
    const char *fn = magic ? "lchflags" : "chflags";
    QPOSPath path = QP_OS_PATH(NULL, "path", 0, 0);
    JSValueConst a[3];
    uint64_t flags;
    int follow = 1, res, err;
    JSValue r = JS_UNDEFINED;
    path.function_name = fn;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_unsigned(ctx, a[1], sizeof(unsigned long), 1, fn, "argument 'flags'",
                       "unsigned long", &flags) < 0 ||
        (!magic && qp_os_opt_bool(ctx, a[2], 1, &follow) < 0)) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    QP_BEGIN_ALLOW_THREADS(ctx)
    if (magic || !follow)
        res = lchflags(path.narrow, (unsigned long)flags);
    else
        res = chflags(path.narrow, (unsigned long)flags);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (res)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}
#endif /* QP_OS_HAVE_CHFLAGS */

/* os.chroot(path) */
QP_OS_FN(qp_os_chroot)
{
    QPOSPath path = QP_OS_PATH("chroot", "path", 0, 0);
    int res, err;
    JSValue r = JS_UNDEFINED;
    QP_OS_PARSE("chroot", 1, 1, "path");
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    res = chroot(path.narrow);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (res < 0)
        r = qp_os_path_error(ctx, &path, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* os.nice(increment, /) */
QP_OS_FN(qp_os_nice)
{
    int increment, value;
    if (qp_arg_int(ctx, args[0], &increment) < 0)
        return JS_EXCEPTION;
    /* nice() returns the new priority on macOS and Linux (glibc); -1 may
       be the new priority, so errno is cleared before the call */
    errno = 0;
    value = nice(increment);
    if (value == -1 && errno != 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, value);
}

/* os.getpriority(which, who) */
QP_OS_FN(qp_os_getpriority)
{
    int which, who, retval;
    QP_OS_PARSE("getpriority", 2, 2, "which", "who");
    if (qp_os_int(ctx, a[0], &which) < 0 || qp_os_int(ctx, a[1], &who) < 0)
        return JS_EXCEPTION;
    errno = 0;
    retval = getpriority(which, who);
    if (errno != 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, retval);
}

/* os.setpriority(which, who, priority) */
QP_OS_FN(qp_os_setpriority)
{
    int which, who, priority;
    QP_OS_PARSE("setpriority", 3, 3, "which", "who", "priority");
    if (qp_os_int(ctx, a[0], &which) < 0 || qp_os_int(ctx, a[1], &who) < 0 ||
        qp_os_int(ctx, a[2], &priority) < 0)
        return JS_EXCEPTION;
    if (setpriority(which, who, priority) == -1)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* os.system(command) -> long (unicode_fs_encoded) */
QP_OS_FN(qp_os_system)
{
    JSValue command;
    const uint8_t *bytes;
    Py_ssize_t len;
    long result;
    QP_OS_PARSE("system", 1, 1, "command");
    command = qp_os_fsconvert(ctx, a[0]);
    if (JS_IsException(command))
        return command;
    bytes = qp_bytes_data(command, &len);
    QP_BEGIN_ALLOW_THREADS(ctx)
    result = system((const char *)bytes);
    QP_END_ALLOW_THREADS(ctx)
    JS_FreeValue(ctx, command);
    return qp_long_from_int64(ctx, result);
}

/* path_t(make_wide=True, nonstrict=True) of the _path_* helpers: the
   argument (or its __fspath__()) as UTF-8 bytes (new reference).  str
   is encoded with surrogatepass so that every code point round-trips;
   the algorithms below only look at the ASCII '/' and '.' bytes, which
   never occur inside a multibyte UTF-8 sequence. */
static JSValue qp_os_path_wide(JSContext *ctx, const char *fn, JSValueConst o, int *is_bytes)
{
    JSValue v, r;
    if (qp_str_check(ctx, o) || qp_bytes_check(ctx, o)) {
        v = JS_DupValue(ctx, o);
    } else {
        JSValue func = qp_lookup_special(ctx, o, QP_ATOM(ctx, __fspath__));
        if (JS_IsException(func))
            return func;
        if (QP_IS_NULL(func) || JS_IsUndefined(func)) {
            JS_FreeValue(ctx, func);
            return qp_raise_type_error(ctx, "%s: path should be string, bytes or os.PathLike, not %s",
                                       fn, qp_os_tname(ctx, o));
        }
        v = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(v))
            return v;
        if (!qp_str_check(ctx, v) && !qp_bytes_check(ctx, v)) {
            qp_raise_type_error(ctx, "expected %s.__fspath__() to return str or bytes, not %s",
                                qp_os_tname(ctx, o), qp_os_tname(ctx, v));
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
    }
    *is_bytes = qp_bytes_check(ctx, v);
    if (*is_bytes)
        return v;
    r = qp_str_encode(ctx, v, "utf-8", "surrogatepass");
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue qp_os_path_wide_result(JSContext *ctx, int is_bytes, const uint8_t *s, Py_ssize_t len)
{
    if (is_bytes)
        return qp_bytes_new(ctx, s, len);
    return qp_str_decode(ctx, (const char *)s, len, "utf-8", "surrogatepass");
}

/* _Py_skiproot (POSIX): no drive; the root is '/' or exactly two
   leading slashes */
static void qp_os_skiproot(const uint8_t *path, Py_ssize_t size, Py_ssize_t *rootsize)
{
    if (size < 1 || path[0] != '/')
        *rootsize = 0;   /* relative path, e.g.: 'foo' */
    else if (size < 2 || path[1] != '/' || (size > 2 && path[2] == '/'))
        *rootsize = 1;   /* absolute path, e.g.: '/foo', '///foo' */
    else
        *rootsize = 2;   /* precisely two leading slashes, e.g.: '//foo' */
}

/* os._path_splitroot_ex(path, /) */
QP_OS_FN(qp_os__path_splitroot_ex)
{
    int is_bytes;
    JSValue b = qp_os_path_wide(ctx, "_path_splitroot_ex", args[0], &is_bytes), items[3];
    const uint8_t *p;
    Py_ssize_t len, rootsize;
    if (JS_IsException(b))
        return b;
    p = qp_bytes_data(b, &len);
    qp_os_skiproot(p, len, &rootsize);
    items[0] = qp_os_path_wide_result(ctx, is_bytes, p, 0);
    items[1] = qp_os_path_wide_result(ctx, is_bytes, p, rootsize);
    items[2] = qp_os_path_wide_result(ctx, is_bytes, p + rootsize, len - rootsize);
    JS_FreeValue(ctx, b);
    if (JS_IsException(items[0]) || JS_IsException(items[1]) || JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[2]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 3, items);
}

/* _Py_normpath_and_size (POSIX): in-place normalisation of path[0:size];
   returns the normalized length */
static Py_ssize_t qp_os_normpath(uint8_t *path, Py_ssize_t size)
{
    uint8_t *pEnd = path + size;
    uint8_t *p1 = path;     /* sequentially scanned address in the path */
    uint8_t *p2 = path;     /* destination of a scanned character to be ljusted */
    uint8_t *minP2 = path;  /* the beginning of the destination range */
    uint8_t lastC = 0;      /* the last ljusted character, p2[-1] in most cases */
    Py_ssize_t rootsize;

#define QP_NP_IS_END(x) ((x) >= pEnd)
#define QP_NP_IS_SEP(x) (!QP_NP_IS_END(x) && *(x) == '/')
#define QP_NP_SEP_OR_END(x) (QP_NP_IS_END(x) || *(x) == '/')
    if (size == 0)
        return 0;
    qp_os_skiproot(path, size, &rootsize);
    if (rootsize) {
        /* Skip past root and update minP2 */
        p1 = &path[rootsize];
        p2 = p1;
        minP2 = p2 - 1;
        lastC = *minP2;
    }
    if (!QP_NP_IS_END(p1) && p1[0] == '.' && QP_NP_SEP_OR_END(&p1[1])) {
        /* Skip leading './' */
        ++p1;
        lastC = QP_NP_IS_END(p1) ? 0 : *p1;
        while (QP_NP_IS_SEP(p1))
            p1++;
    }
    for (; !QP_NP_IS_END(p1); ++p1) {
        uint8_t c = *p1;
        if (lastC == '/') {
            if (c == '.') {
                int sep_at_1 = QP_NP_SEP_OR_END(&p1[1]);
                int sep_at_2 = !sep_at_1 && QP_NP_SEP_OR_END(&p1[2]);
                if (sep_at_2 && p1[1] == '.') {
                    uint8_t *p3 = p2;
                    while (p3 != minP2 && *--p3 == '/') { }
                    while (p3 != minP2 && *(p3 - 1) != '/') { --p3; }
                    if (p2 == minP2 ||
                        (p3[0] == '.' && p3[1] == '.' && p3[2] == '/')) {
                        /* Previous segment is also ../, so append instead.
                           Relative path does not absorb ../ at minP2 as well. */
                        *p2++ = '.';
                        *p2++ = '.';
                        lastC = '.';
                    } else if (p3[0] == '/') {
                        /* Absolute path, so absorb segment */
                        p2 = p3 + 1;
                    } else {
                        p2 = p3;
                    }
                    p1 += 1;
                } else if (sep_at_1) {
                } else {
                    *p2++ = lastC = c;
                }
            } else if (c == '/') {
            } else {
                *p2++ = lastC = c;
            }
        } else {
            *p2++ = lastC = c;
        }
    }
    if (p2 != minP2) {
        while (--p2 != minP2 && *p2 == '/') { }
    } else {
        --p2;
    }
#undef QP_NP_IS_END
#undef QP_NP_IS_SEP
#undef QP_NP_SEP_OR_END
    return p2 - path + 1;
}

/* os._path_normpath(path) */
QP_OS_FN(qp_os__path_normpath)
{
    int is_bytes;
    JSValue b, r;
    const uint8_t *p;
    uint8_t *buf;
    Py_ssize_t len, norm_len;
    QP_OS_PARSE("_path_normpath", 1, 1, "path");
    b = qp_os_path_wide(ctx, "_path_normpath", a[0], &is_bytes);
    if (JS_IsException(b))
        return b;
    p = qp_bytes_data(b, &len);
    buf = js_malloc(ctx, len + 1);
    if (!buf) {
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    memcpy(buf, p, len);
    buf[len] = 0;
    JS_FreeValue(ctx, b);
    norm_len = qp_os_normpath(buf, len);
    if (!norm_len)
        r = qp_os_path_wide_result(ctx, is_bytes, (const uint8_t *)".", 1);
    else
        r = qp_os_path_wide_result(ctx, is_bytes, buf, norm_len);
    js_free(ctx, buf);
    return r;
}

/* ---- posix_spawn ---- */

enum {
    QP_OS_SPAWN_OPEN,
    QP_OS_SPAWN_CLOSE,
    QP_OS_SPAWN_DUP2,
#ifdef QP_OS_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
    QP_OS_SPAWN_CLOSEFROM,
#endif
};

#ifdef QP_OS_HAVE_SCHED_PARAM
static int qp_os_sched_param_conv(JSContext *ctx, JSValueConst module, JSValueConst param,
                                  struct sched_param *res);
#endif

/* parse_arglist: the items of a list or tuple (already checked) as a
   NULL-terminated array of fsencoded strings */
static char **qp_os_parse_arglist(JSContext *ctx, JSValueConst argv, Py_ssize_t *pargc)
{
    JSValue seq = qp_tuple_check(ctx, argv) ? JS_DupValue(ctx, argv) : qp_list_as_tuple(ctx, argv);
    Py_ssize_t n, i;
    char **v;
    if (JS_IsException(seq))
        return NULL;
    n = QP_TUPLE_SIZE(seq);
    v = js_mallocz(ctx, sizeof(char *) * (n + 1));
    if (!v) {
        JS_FreeValue(ctx, seq);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        v[i] = qp_os_fs_strdup(ctx, QP_TUPLE_ITEMS(seq)[i]);
        if (!v[i]) {
            qp_os_free_strv(ctx, v, i);
            JS_FreeValue(ctx, seq);
            return NULL;
        }
    }
    JS_FreeValue(ctx, seq);
    *pargc = n;
    return v;
}

/* parse_posix_spawn_flags: initializes *attrp (destroyed on failure) */
static int qp_os_spawn_flags(JSContext *ctx, JSValueConst module, const char *fn,
                             JSValueConst setpgroup, int resetids, int setsid,
                             JSValueConst setsigmask, JSValueConst setsigdef,
                             JSValueConst scheduler, posix_spawnattr_t *attrp)
{
    long all_flags = 0;
    int err = posix_spawnattr_init(attrp);
    if (err) {
        qp_os_error(ctx, err);
        return -1;
    }
    if (!QP_IS_NULL(setpgroup) && !JS_IsUndefined(setpgroup) && !JS_IsNull(setpgroup)) {
        pid_t pgid;
        if (qp_os_pid(ctx, setpgroup, &pgid) < 0)
            goto fail;
        err = posix_spawnattr_setpgroup(attrp, pgid);
        if (err) {
            qp_os_error(ctx, err);
            goto fail;
        }
        all_flags |= POSIX_SPAWN_SETPGROUP;
    }
    if (resetids)
        all_flags |= POSIX_SPAWN_RESETIDS;
    if (setsid) {
#ifdef QP_OS_HAVE_POSIX_SPAWN_SETSID_RUNTIME
        if (QP_OS_HAVE_POSIX_SPAWN_SETSID_RUNTIME) {
#endif
#ifdef POSIX_SPAWN_SETSID
            all_flags |= POSIX_SPAWN_SETSID;
#elif defined(POSIX_SPAWN_SETSID_NP)
            all_flags |= POSIX_SPAWN_SETSID_NP;
#else
            qp_os_argument_unavailable(ctx, fn, "setsid");
            goto fail;
#endif
#ifdef QP_OS_HAVE_POSIX_SPAWN_SETSID_RUNTIME
        } else {
            qp_os_argument_unavailable(ctx, fn, "setsid");
            goto fail;
        }
#endif
    }
    if (!QP_IS_NULL(setsigmask)) {
        sigset_t set;
        if (qp_signal_sigset_conv(ctx, setsigmask, &set) < 0)
            goto fail;
        err = posix_spawnattr_setsigmask(attrp, &set);
        if (err) {
            qp_os_error(ctx, err);
            goto fail;
        }
        all_flags |= POSIX_SPAWN_SETSIGMASK;
    }
    if (!QP_IS_NULL(setsigdef)) {
        sigset_t set;
        if (qp_signal_sigset_conv(ctx, setsigdef, &set) < 0)
            goto fail;
        err = posix_spawnattr_setsigdefault(attrp, &set);
        if (err) {
            qp_os_error(ctx, err);
            goto fail;
        }
        all_flags |= POSIX_SPAWN_SETSIGDEF;
    }
    if (!QP_IS_NULL(scheduler) && !JS_IsUndefined(scheduler) && !JS_IsNull(scheduler)) {
#ifdef POSIX_SPAWN_SETSCHEDULER
        JSValueConst py_schedpolicy, schedparam_obj;
        struct sched_param schedparam;
        if (QP_TUPLE_SIZE(scheduler) != 2) {
            qp_raise_type_error(ctx, "A scheduler tuple must have two elements");
            goto fail;
        }
        py_schedpolicy = QP_TUPLE_ITEMS(scheduler)[0];
        schedparam_obj = QP_TUPLE_ITEMS(scheduler)[1];
        if (qp_os_sched_param_conv(ctx, module, schedparam_obj, &schedparam) < 0)
            goto fail;
        if (!JS_IsUndefined(py_schedpolicy) && !JS_IsNull(py_schedpolicy)) {
            int schedpolicy;
            if (qp_arg_int(ctx, py_schedpolicy, &schedpolicy) < 0)
                goto fail;
            err = posix_spawnattr_setschedpolicy(attrp, schedpolicy);
            if (err) {
                qp_os_error(ctx, err);
                goto fail;
            }
            all_flags |= POSIX_SPAWN_SETSCHEDULER;
        }
        err = posix_spawnattr_setschedparam(attrp, &schedparam);
        if (err) {
            qp_os_error(ctx, err);
            goto fail;
        }
        all_flags |= POSIX_SPAWN_SETSCHEDPARAM;
#else
        qp_raise(ctx, QP_EXC(ctx, NotImplementedError),
                 "The scheduler option is not supported in this system.");
        goto fail;
#endif
    }
    err = posix_spawnattr_setflags(attrp, all_flags);
    if (err) {
        qp_os_error(ctx, err);
        goto fail;
    }
    return 0;
fail:
    (void)posix_spawnattr_destroy(attrp);
    return -1;
}

/* A file_actions tuple item as an int ("i" of PyArg_ParseTuple) */
static int qp_os_fa_int(JSContext *ctx, JSValueConst t, int i, int *out)
{
    return qp_arg_int(ctx, QP_TUPLE_ITEMS(t)[i], out);
}

/* parse_file_actions: initializes *fap (destroyed on failure); the
   fsencoded paths are kept in temp_buffer until posix_spawn() ran */
static int qp_os_spawn_file_actions(JSContext *ctx, JSValueConst file_actions,
                                    posix_spawn_file_actions_t *fap, JSValueConst temp_buffer)
{
    JSValue seq = qp_os_seq_tuple(ctx, file_actions, "file_actions must be a sequence or None");
    Py_ssize_t i;
    int err;
    if (JS_IsException(seq))
        return -1;
    err = posix_spawn_file_actions_init(fap);
    if (err) {
        qp_os_error(ctx, err);
        JS_FreeValue(ctx, seq);
        return -1;
    }
    for (i = 0; i < QP_TUPLE_SIZE(seq); i++) {
        JSValueConst fa = QP_TUPLE_ITEMS(seq)[i];
        int64_t tag;
        Py_ssize_t n;
        JSValue tagv;
        if (!qp_tuple_check(ctx, fa) || QP_TUPLE_SIZE(fa) == 0) {
            qp_raise_type_error(ctx, "Each file_actions element must be a non-empty tuple");
            goto fail;
        }
        n = QP_TUPLE_SIZE(fa);
        /* PyLong_AsLong */
        tagv = qp_index(ctx, QP_TUPLE_ITEMS(fa)[0]);
        if (JS_IsException(tagv))
            goto fail;
        err = qp_long_as_int64(ctx, tagv, &tag);
        JS_FreeValue(ctx, tagv);
        if (err < 0)
            goto fail;
        switch (tag) {
        case QP_OS_SPAWN_OPEN: {
            int fd, oflag;
            uint64_t mode;
            JSValue path;
            const uint8_t *p;
            Py_ssize_t plen;
            if (n != 5) {
                qp_raise_type_error(ctx, "A open file_action tuple must have 5 elements");
                goto fail;
            }
            if (qp_os_fa_int(ctx, fa, 1, &fd) < 0)
                goto fail;
            path = qp_os_fsconvert(ctx, QP_TUPLE_ITEMS(fa)[2]);
            if (JS_IsException(path))
                goto fail;
            if (qp_os_fa_int(ctx, fa, 3, &oflag) < 0 ||
                qp_os_unsigned(ctx, QP_TUPLE_ITEMS(fa)[4], sizeof(unsigned long), 1, NULL, NULL,
                               "unsigned long", &mode) < 0 ||
                qp_list_append(ctx, temp_buffer, path) < 0) {
                JS_FreeValue(ctx, path);
                goto fail;
            }
            p = qp_bytes_data(path, &plen);
            err = posix_spawn_file_actions_addopen(fap, fd, (const char *)p, oflag, (mode_t)mode);
            JS_FreeValue(ctx, path);
            if (err) {
                qp_os_error(ctx, err);
                goto fail;
            }
            break;
        }
        case QP_OS_SPAWN_CLOSE: {
            int fd;
            if (n != 2) {
                qp_raise_type_error(ctx, "A close file_action tuple must have 2 elements");
                goto fail;
            }
            if (qp_os_fa_int(ctx, fa, 1, &fd) < 0)
                goto fail;
            err = posix_spawn_file_actions_addclose(fap, fd);
            if (err) {
                qp_os_error(ctx, err);
                goto fail;
            }
            break;
        }
        case QP_OS_SPAWN_DUP2: {
            int fd1, fd2;
            if (n != 3) {
                qp_raise_type_error(ctx, "A dup2 file_action tuple must have 3 elements");
                goto fail;
            }
            if (qp_os_fa_int(ctx, fa, 1, &fd1) < 0 || qp_os_fa_int(ctx, fa, 2, &fd2) < 0)
                goto fail;
            err = posix_spawn_file_actions_adddup2(fap, fd1, fd2);
            if (err) {
                qp_os_error(ctx, err);
                goto fail;
            }
            break;
        }
#ifdef QP_OS_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
        case QP_OS_SPAWN_CLOSEFROM: {
            int fd;
            if (n != 2) {
                qp_raise_type_error(ctx, "A closefrom file_action tuple must have 2 elements");
                goto fail;
            }
            if (qp_os_fa_int(ctx, fa, 1, &fd) < 0)
                goto fail;
            err = posix_spawn_file_actions_addclosefrom_np(fap, fd);
            if (err) {
                qp_os_error(ctx, err);
                goto fail;
            }
            break;
        }
#endif
        default:
            qp_raise_type_error(ctx, "Unknown file_actions identifier");
            goto fail;
        }
    }
    JS_FreeValue(ctx, seq);
    return 0;
fail:
    JS_FreeValue(ctx, seq);
    (void)posix_spawn_file_actions_destroy(fap);
    return -1;
}

/* posix_spawn (magic 0) / posix_spawnp (magic 1): py_posix_spawn */
QP_OS_FN(qp_os_posix_spawn)
{
    static const char *const kw[] = { "", "", "", "file_actions", "setpgroup", "resetids",
                                      "setsid", "setsigmask", "setsigdef", "scheduler", NULL };
    static const QPArgSpec specs[2] = { { "posix_spawn", kw, 3, 3, 0 },
                                        { "posix_spawnp", kw, 3, 3, 0 } };
    const char *fn = magic ? "posix_spawnp" : "posix_spawn";
    QPOSPath path = QP_OS_PATH(NULL, "path", 0, 0);
    JSValueConst a[10], argv, env, file_actions, scheduler;
    char **argvlist = NULL, **envlist = NULL;
    Py_ssize_t argc = 0, envc = 0;
    posix_spawn_file_actions_t file_actions_buf, *file_actionsp = NULL;
    posix_spawnattr_t attr, *attrp = NULL;
    JSValue result = JS_EXCEPTION, temp_buffer = JS_UNDEFINED;
    int resetids, setsid, err_code;
    pid_t pid;

    path.function_name = fn;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    argv = a[1];
    env = a[2];
    file_actions = a[3];
    scheduler = a[9];
    if (qp_os_opt_bool(ctx, a[5], 0, &resetids) < 0 || qp_os_opt_bool(ctx, a[6], 0, &setsid) < 0)
        goto exit;
    if (!qp_list_check(ctx, argv) && !qp_tuple_check(ctx, argv)) {
        qp_raise_type_error(ctx, "%s: argv must be a tuple or list", fn);
        goto exit;
    }
    if ((qp_list_check(ctx, argv) ? qp_list_size(argv) : QP_TUPLE_SIZE(argv)) < 1) {
        qp_raise_value_error(ctx, "%s: argv must not be empty", fn);
        goto exit;
    }
    if (!qp_os_mapping_check(ctx, env) && !JS_IsUndefined(env) && !JS_IsNull(env)) {
        qp_raise_type_error(ctx, "%s: environment must be a mapping object or None", fn);
        goto exit;
    }
    if (!QP_IS_NULL(scheduler) && !qp_tuple_check(ctx, scheduler) &&
        !JS_IsUndefined(scheduler) && !JS_IsNull(scheduler)) {
        qp_raise_type_error(ctx, "%s: scheduler must be a tuple or None", fn);
        goto exit;
    }
    argvlist = qp_os_parse_arglist(ctx, argv, &argc);
    if (!argvlist)
        goto exit;
    if (!argvlist[0][0]) {
        qp_raise_value_error(ctx, "%s: argv first element cannot be empty", fn);
        goto exit;
    }
    if (JS_IsUndefined(env) || JS_IsNull(env)) {
        envlist = QP_OS_ENVIRON;
    } else {
        envlist = qp_os_parse_envlist(ctx, env, &envc);
        if (!envlist)
            goto exit;
    }
    if (!QP_IS_NULL(file_actions) && !JS_IsUndefined(file_actions) && !JS_IsNull(file_actions)) {
        /* old glibc posix_spawn_file_actions_addopen() did not copy the
           path (bpo-33630): temp_buffer keeps the strings alive */
        temp_buffer = qp_list_new(ctx, 0);
        if (JS_IsException(temp_buffer))
            goto exit;
        if (qp_os_spawn_file_actions(ctx, file_actions, &file_actions_buf, temp_buffer) < 0)
            goto exit;
        file_actionsp = &file_actions_buf;
    }
    if (qp_os_spawn_flags(ctx, self, fn, a[4], resetids, setsid, a[7], a[8], scheduler, &attr) < 0)
        goto exit;
    attrp = &attr;
    if (magic)
        err_code = posix_spawnp(&pid, path.narrow, file_actionsp, attrp, argvlist, envlist);
    else
        err_code = posix_spawn(&pid, path.narrow, file_actionsp, attrp, argvlist, envlist);
    if (err_code) {
        qp_os_path_error(ctx, &path, err_code);
        goto exit;
    }
    result = JS_NewInt32(ctx, (int)pid);
exit:
    if (file_actionsp)
        (void)posix_spawn_file_actions_destroy(file_actionsp);
    if (attrp)
        (void)posix_spawnattr_destroy(attrp);
    if (envlist && envlist != QP_OS_ENVIRON)
        qp_os_free_strv(ctx, envlist, envc);
    qp_os_free_strv(ctx, argvlist, argc);
    JS_FreeValue(ctx, temp_buffer);
    qp_os_path_clean(ctx, &path);
    return result;
}

/* ---- sched_* ---- */

/* os.sched_get_priority_max (magic 0) / sched_get_priority_min (magic 1) */
QP_OS_FN(qp_os_sched_get_priority)
{
    static const char *const kw[] = { "policy", NULL };
    static const QPArgSpec specs[2] = { { "sched_get_priority_max", kw, 1, 1, 0 },
                                        { "sched_get_priority_min", kw, 1, 1, 0 } };
    JSValueConst a[1];
    int policy, r;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0 ||
        qp_os_int(ctx, a[0], &policy) < 0)
        return JS_EXCEPTION;
    /* make sure that errno is cleared before the call */
    errno = 0;
    r = magic ? sched_get_priority_min(policy) : sched_get_priority_max(policy);
    if (r == -1 && errno)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, r);
}

/* os.sched_yield() */
QP_OS_FN(qp_os_sched_yield)
{
    int result, err;
    QP_BEGIN_ALLOW_THREADS(ctx)
    result = sched_yield();
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (result < 0)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}

#ifdef QP_OS_HAVE_SCHED_PARAM
#define QP_OS_PROP_SCHEDPARAM "\x01qp_os_sched_param"

static const char *const qp_os_sched_param_fields[] = { "sched_priority", NULL };
static const char *const qp_os_sched_param_docs[] = { "the scheduling priority", NULL };
static const QPStructSeqDesc qp_os_sched_param_desc = {
    "posix.sched_param", "sched_param(sched_priority)\n--\n\n"
    "Currently has only one field: sched_priority\n\n"
    "  sched_priority\n"
    "    A scheduling parameter.",
    qp_os_sched_param_fields, qp_os_sched_param_docs, 1, 0
};

/* sched_param.__new__(type, sched_priority) */
QP_OS_FN(qp_os_sched_param_new)
{
    static const char *const kw[] = { "", "sched_priority", NULL };
    static const QPArgSpec spec = { "sched_param", kw, 2, 2, 0 };
    JSValueConst a[2];
    JSValue items[1];
    QPType *t;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    t = qp_type_from_value(a[0]);
    if (!t)
        return qp_raise_type_error(ctx, "sched_param.__new__(X): X is not a type object");
    items[0] = JS_DupValue(ctx, a[1]);
    return qp_structseq_new(ctx, t, items);
}

/* sched_param.__reduce__ */
QP_OS_FN(qp_os_sched_param_reduce)
{
    JSValue items[2], arg[1];
    arg[0] = qp_ss_get(ctx, self, 0);
    if (JS_IsException(arg[0]))
        return JS_EXCEPTION;
    items[1] = qp_tuple_new_steal(ctx, 1, arg);
    if (JS_IsException(items[1]))
        return JS_EXCEPTION;
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    return qp_tuple_new_steal(ctx, 2, items);
}

static const QPMethodDef qp_os_sched_param_new_def = {
    "__new__", qp_os_sched_param_new, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
    "Create and return a new object.  See help(type) for accurate signature."
};
static const QPMethodDef qp_os_sched_param_reduce_def = {
    "__reduce__", qp_os_sched_param_reduce, QP_METH_NOARGS, 0, NULL
};

/* convert_sched_param */
static int qp_os_sched_param_conv(JSContext *ctx, JSValueConst module, JSValueConst param,
                                  struct sched_param *res)
{
    QPType *t = qp_os_get_type(ctx, module, QP_OS_PROP_SCHEDPARAM);
    JSValue v;
    int64_t priority;
    int r;
    if (!t)
        return -1;
    if (qp_type_of(ctx, param) != t) {
        qp_raise_type_error(ctx, "must have a sched_param object");
        return -1;
    }
    v = qp_ss_get(ctx, param, 0);
    if (JS_IsException(v))
        return -1;
    /* PyLong_AsLong */
    {
        JSValue i = qp_index(ctx, v);
        JS_FreeValue(ctx, v);
        if (JS_IsException(i))
            return -1;
        r = qp_long_as_int64(ctx, i, &priority);
        JS_FreeValue(ctx, i);
    }
    if (r < 0)
        return -1;
    if (priority > INT_MAX || priority < INT_MIN) {
        qp_raise_overflow_error(ctx, "sched_priority out of range");
        return -1;
    }
    res->sched_priority = (int)priority;
    return 0;
}
#endif /* QP_OS_HAVE_SCHED_PARAM */

#ifdef QP_OS_HAVE_SCHED_SETSCHEDULER
/* os.sched_getscheduler(pid, /) */
QP_OS_FN(qp_os_sched_getscheduler)
{
    pid_t pid;
    int policy;
    if (qp_os_pid(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    policy = sched_getscheduler(pid);
    if (policy < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, policy);
}

/* os.sched_setscheduler(pid, policy, param, /) */
QP_OS_FN(qp_os_sched_setscheduler)
{
    pid_t pid;
    int policy;
    struct sched_param param;
    if (qp_arg_check_positional(ctx, "sched_setscheduler", nargs, 3, 3) < 0 ||
        qp_os_pid(ctx, args[0], &pid) < 0 || qp_arg_int(ctx, args[1], &policy) < 0 ||
        qp_os_sched_param_conv(ctx, self, args[2], &param) < 0)
        return JS_EXCEPTION;
    /* sched_setscheduler() returns 0 in Linux, but the previous scheduling
       policy under Solaris/Illumos, and others; -1 on error everywhere */
    if (sched_setscheduler(pid, policy, &param) == -1)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}
#endif /* QP_OS_HAVE_SCHED_SETSCHEDULER */

#ifdef QP_OS_HAVE_SCHED_SETPARAM
/* os.sched_getparam(pid, /) */
QP_OS_FN(qp_os_sched_getparam)
{
    pid_t pid;
    struct sched_param param;
    QPType *t;
    JSValue items[1];
    if (qp_os_pid(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    if (sched_getparam(pid, &param))
        return qp_os_error(ctx, errno);
    t = qp_os_get_type(ctx, self, QP_OS_PROP_SCHEDPARAM);
    if (!t)
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, param.sched_priority);
    return qp_structseq_new(ctx, t, items);
}

/* os.sched_setparam(pid, param, /) */
QP_OS_FN(qp_os_sched_setparam)
{
    pid_t pid;
    struct sched_param param;
    if (qp_arg_check_positional(ctx, "sched_setparam", nargs, 2, 2) < 0 ||
        qp_os_pid(ctx, args[0], &pid) < 0 ||
        qp_os_sched_param_conv(ctx, self, args[1], &param) < 0)
        return JS_EXCEPTION;
    if (sched_setparam(pid, &param))
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}
#endif /* QP_OS_HAVE_SCHED_SETPARAM */

#ifdef QP_OS_HAVE_SCHED_RR_GET_INTERVAL
/* os.sched_rr_get_interval(pid, /) -> double */
QP_OS_FN(qp_os_sched_rr_get_interval)
{
    pid_t pid;
    struct timespec interval;
    if (qp_os_pid(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    if (sched_rr_get_interval(pid, &interval))
        return qp_os_error(ctx, errno);
    return qp_float_new(ctx, (double)interval.tv_sec + 1e-9 * interval.tv_nsec);
}
#endif /* QP_OS_HAVE_SCHED_RR_GET_INTERVAL */

#ifdef QP_OS_HAVE_SCHED_SETAFFINITY
/* The minimum number of CPUs allocated in a cpu_set_t */
#define QP_OS_NCPUS_START ((int)(sizeof(unsigned long) * CHAR_BIT))

/* os.sched_setaffinity(pid, mask, /) */
QP_OS_FN(qp_os_sched_setaffinity)
{
    pid_t pid;
    int ncpus;
    size_t setsize;
    cpu_set_t *cpu_set = NULL;
    JSValue iterator = JS_UNDEFINED, item;
    if (qp_arg_check_positional(ctx, "sched_setaffinity", nargs, 2, 2) < 0 ||
        qp_os_pid(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    iterator = qp_iter(ctx, args[1]);
    if (JS_IsException(iterator))
        return iterator;
    ncpus = QP_OS_NCPUS_START;
    setsize = CPU_ALLOC_SIZE(ncpus);
    cpu_set = CPU_ALLOC(ncpus);
    if (!cpu_set) {
        JS_ThrowOutOfMemory(ctx);
        goto error;
    }
    CPU_ZERO_S(setsize, cpu_set);
    for (;;) {
        int64_t cpu;
        JSValue iv;
        int r;
        item = qp_next(ctx, iterator);
        if (QP_IS_NULL(item))
            break;
        if (JS_IsException(item))
            goto error;
        if (!qp_os_index_check(ctx, item)) {
            qp_raise_type_error(ctx, "expected an iterator of ints, but iterator yielded %R",
                                QP_TYPE_VALUE(qp_type_of(ctx, item)));
            JS_FreeValue(ctx, item);
            goto error;
        }
        iv = qp_index(ctx, item);
        JS_FreeValue(ctx, item);
        if (JS_IsException(iv))
            goto error;
        r = qp_long_as_int64(ctx, iv, &cpu);
        JS_FreeValue(ctx, iv);
        if (r < 0)
            goto error;
        if (cpu < 0) {
            qp_raise_value_error(ctx, "negative CPU number");
            goto error;
        }
        if (cpu > INT_MAX - 1) {
            qp_raise_overflow_error(ctx, "CPU number too large");
            goto error;
        }
        if (cpu >= ncpus) {
            /* Grow CPU mask to fit the CPU number */
            int newncpus = ncpus;
            cpu_set_t *newmask;
            size_t newsetsize;
            while (newncpus <= cpu) {
                if (newncpus > INT_MAX / 2)
                    newncpus = (int)cpu + 1;
                else
                    newncpus = newncpus * 2;
            }
            newmask = CPU_ALLOC(newncpus);
            if (!newmask) {
                JS_ThrowOutOfMemory(ctx);
                goto error;
            }
            newsetsize = CPU_ALLOC_SIZE(newncpus);
            CPU_ZERO_S(newsetsize, newmask);
            memcpy(newmask, cpu_set, setsize);
            CPU_FREE(cpu_set);
            setsize = newsetsize;
            cpu_set = newmask;
            ncpus = newncpus;
        }
        CPU_SET_S((int)cpu, setsize, cpu_set);
    }
    JS_FreeValue(ctx, iterator);
    iterator = JS_UNDEFINED;
    if (sched_setaffinity(pid, setsize, cpu_set)) {
        qp_os_error(ctx, errno);
        goto error;
    }
    CPU_FREE(cpu_set);
    return JS_UNDEFINED;
error:
    if (cpu_set)
        CPU_FREE(cpu_set);
    JS_FreeValue(ctx, iterator);
    return JS_EXCEPTION;
}

/* os.sched_getaffinity(pid, /) */
QP_OS_FN(qp_os_sched_getaffinity)
{
    pid_t pid;
    int ncpus = QP_OS_NCPUS_START, cpu, count;
    size_t setsize;
    cpu_set_t *mask;
    JSValue res;
    if (qp_os_pid(ctx, args[0], &pid) < 0)
        return JS_EXCEPTION;
    for (;;) {
        setsize = CPU_ALLOC_SIZE(ncpus);
        mask = CPU_ALLOC(ncpus);
        if (!mask)
            return JS_ThrowOutOfMemory(ctx);
        if (sched_getaffinity(pid, setsize, mask) == 0)
            break;
        CPU_FREE(mask);
        if (errno != EINVAL)
            return qp_os_error(ctx, errno);
        if (ncpus > INT_MAX / 2)
            return qp_raise_overflow_error(ctx, "could not allocate a large enough CPU set");
        ncpus *= 2;
    }
    res = qp_set_new(ctx, QP_NULL);
    if (JS_IsException(res)) {
        CPU_FREE(mask);
        return res;
    }
    count = CPU_COUNT_S(setsize, mask);
    for (cpu = 0; count; cpu++) {
        if (CPU_ISSET_S(cpu, setsize, mask)) {
            JSValue cpu_num = JS_NewInt32(ctx, cpu);
            --count;
            if (qp_set_add(ctx, res, cpu_num) < 0) {
                CPU_FREE(mask);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
    }
    CPU_FREE(mask);
    return res;
}
#endif /* QP_OS_HAVE_SCHED_SETAFFINITY */

/* ---- pseudo-terminals ---- */

/* os.posix_openpt(oflag, /) -> int */
QP_OS_FN(qp_os_posix_openpt)
{
    int oflag, fd;
    if (qp_arg_int(ctx, args[0], &oflag) < 0)
        return JS_EXCEPTION;
#ifdef O_CLOEXEC
    oflag |= O_CLOEXEC;
#endif
    fd = posix_openpt(oflag);
    if (fd == -1)
        return qp_os_error(ctx, errno);
    /* Just in case, likely a no-op given O_CLOEXEC above. */
    if (qp_os_set_inheritable(fd, 0) < 0) {
        int err = errno;
        close(fd);
        return qp_os_error(ctx, err);
    }
    return JS_NewInt32(ctx, fd);
}

/* os.grantpt(fd, /) */
QP_OS_FN(qp_os_grantpt)
{
    int fd, ret, saved_errno = 0;
    void (*sig_saved)(int);
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    sig_saved = qp_signal_setsig(SIGCHLD, SIG_DFL);
    ret = grantpt(fd);
    if (ret == -1)
        saved_errno = errno;
    qp_signal_setsig(SIGCHLD, sig_saved);
    if (ret == -1)
        return qp_os_error(ctx, saved_errno);
    return JS_UNDEFINED;
}

/* os.unlockpt(fd, /) */
QP_OS_FN(qp_os_unlockpt)
{
    int fd;
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (unlockpt(fd) == -1)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* os.ptsname(fd, /): ptsname_r() (HAVE_PTSNAME_R), ptsname() otherwise */
QP_OS_FN(qp_os_ptsname)
{
    int fd, ret;
    char name[MAXPATHLEN + 1];
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (QP_OS_HAVE_PTSNAME_R_RUNTIME) {
        ret = ptsname_r(fd, name, sizeof(name));
    } else {
        /* POSIX: upon failure, ptsname() returns a null pointer and may set
           errno; always initialize errno */
        char *p;
        errno = 0;
        p = ptsname(fd);
        if (p == NULL)
            return qp_os_error(ctx, errno);
        return qp_os_fsdecode(ctx, p, strlen(p));
    }
    if (ret != 0)
        return qp_os_error(ctx, ret);
    return qp_os_fsdecode(ctx, name, strlen(name));
}

/* os.openpty() */
QP_OS_FN(qp_os_openpty)
{
    int master_fd = -1, slave_fd = -1, err;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0)
        return qp_os_error(ctx, errno);
    if (qp_os_set_inheritable(master_fd, 0) < 0 || qp_os_set_inheritable(slave_fd, 0) < 0) {
        err = errno;
        close(master_fd);
        close(slave_fd);
        return qp_os_error(ctx, err);
    }
    return qp_os_fd_pair(ctx, master_fd, slave_fd);
}

/* os.login_tty(fd, /) */
QP_OS_FN(qp_os_login_tty)
{
    int fd;
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (login_tty(fd) == -1)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* ---- users and groups ---- */

#ifdef NGROUPS_MAX
#define QP_OS_MAX_GROUPS NGROUPS_MAX
#else
#define QP_OS_MAX_GROUPS 64
#endif

/* os.getgrouplist(user, group, /): the group is an int on macOS, gid_t
   elsewhere */
QP_OS_FN(qp_os_getgrouplist)
{
    JSValue list, ub;
    const char *user;
    Py_ssize_t ulen;
    int i, ngroups;
#ifdef __APPLE__
    int basegid, *groups;
#else
    uint32_t basegid;
    gid_t *groups;
#endif
    if (qp_arg_check_positional(ctx, "getgrouplist", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "getgrouplist() argument 1 must be str, not %s",
                                   qp_os_tname(ctx, args[0]));
    ub = qp_str_encode(ctx, args[0], "utf-8", "strict");
    if (JS_IsException(ub))
        return ub;
    user = (const char *)qp_bytes_data(ub, &ulen);
    if ((Py_ssize_t)strlen(user) != ulen) {
        JS_FreeValue(ctx, ub);
        return qp_raise_value_error(ctx, "embedded null character");
    }
#ifdef __APPLE__
    if (qp_arg_int(ctx, args[1], &basegid) < 0) {
#else
    if (qp_os_id_conv(ctx, args[1], &basegid, "gid") < 0) {
#endif
        JS_FreeValue(ctx, ub);
        return JS_EXCEPTION;
    }
    /* NGROUPS_MAX is the maximum number of supplemental groups a user can
       belong to; getgrouplist() returns the primary group too */
    ngroups = 1 + QP_OS_MAX_GROUPS;
    for (;;) {
        int old_ngroups;
        groups = js_malloc(ctx, sizeof(*groups) * ngroups);
        if (!groups) {
            JS_FreeValue(ctx, ub);
            return JS_EXCEPTION;
        }
        old_ngroups = ngroups;
        if (getgrouplist(user, basegid, groups, &ngroups) != -1)
            break;
        /* getgrouplist() fails if the group list is too small */
        js_free(ctx, groups);
        if (ngroups <= old_ngroups) {
            /* (glibc sets ngroups to the total number of groups) */
            if (ngroups > INT_MAX / 2) {
                JS_FreeValue(ctx, ub);
                return JS_ThrowOutOfMemory(ctx);
            }
            ngroups *= 2;
        }
    }
    JS_FreeValue(ctx, ub);
    list = qp_list_new(ctx, 0);
    for (i = 0; i < ngroups && !JS_IsException(list); i++) {
#ifdef __APPLE__
        JSValue o = qp_long_from_uint64(ctx, (unsigned long)groups[i]);
#else
        JSValue o = qp_os_from_id(ctx, groups[i]);
#endif
        if (JS_IsException(o) || qp_list_append(ctx, list, o) < 0) {
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, o);
    }
    js_free(ctx, groups);
    return list;
}

/* os.initgroups(username, gid, /): the gid is an int on macOS */
QP_OS_FN(qp_os_initgroups)
{
    JSValue oname;
    const uint8_t *username;
    Py_ssize_t len;
    int r;
#ifdef __APPLE__
    int gid;
#else
    uint32_t gid;
#endif
    if (qp_arg_check_positional(ctx, "initgroups", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    oname = qp_os_fsconvert(ctx, args[0]);
    if (JS_IsException(oname))
        return oname;
#ifdef __APPLE__
    r = qp_arg_int(ctx, args[1], &gid);
#else
    r = qp_os_id_conv(ctx, args[1], &gid, "gid");
#endif
    if (r < 0) {
        JS_FreeValue(ctx, oname);
        return JS_EXCEPTION;
    }
    username = qp_bytes_data(oname, &len);
    r = initgroups((const char *)username, gid);
    JS_FreeValue(ctx, oname);
    if (r == -1)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* os.setpgrp() */
QP_OS_FN(qp_os_setpgrp)
{
    if (setpgrp() < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* setuid (0), seteuid (1), setegid (2), setgid (3): one uid_t/gid_t */
QP_OS_FN(qp_os_setid)
{
    uint32_t id;
    int r;
    if (qp_os_id_conv(ctx, args[0], &id, magic >= 2 ? "gid" : "uid") < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: r = setuid(id); break;
    case 1: r = seteuid(id); break;
    case 2: r = setegid(id); break;
    default: r = setgid(id); break;
    }
    if (r < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* setreuid (0), setregid (1) */
QP_OS_FN(qp_os_setreid)
{
    uint32_t r_id, e_id;
    const char *what = magic ? "gid" : "uid";
    int r;
    if (qp_arg_check_positional(ctx, magic ? "setregid" : "setreuid", nargs, 2, 2) < 0 ||
        qp_os_id_conv(ctx, args[0], &r_id, what) < 0 || qp_os_id_conv(ctx, args[1], &e_id, what) < 0)
        return JS_EXCEPTION;
    r = magic ? setregid(r_id, e_id) : setreuid(r_id, e_id);
    if (r < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* os.setgroups(groups, /) */
QP_OS_FN(qp_os_setgroups)
{
    JSValue seq;
    Py_ssize_t len, i;
    gid_t *grouplist;
    if (!qp_abs_sequence_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "setgroups argument must be a sequence");
    seq = qp_os_seq_tuple(ctx, args[0], "setgroups argument must be a sequence");
    if (JS_IsException(seq))
        return seq;
    len = QP_TUPLE_SIZE(seq);
    if (len > QP_OS_MAX_GROUPS) {
        JS_FreeValue(ctx, seq);
        return qp_raise_value_error(ctx, "too many groups");
    }
    grouplist = js_malloc(ctx, sizeof(gid_t) * (len ? len : 1));
    if (!grouplist) {
        JS_FreeValue(ctx, seq);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        JSValueConst elem = QP_TUPLE_ITEMS(seq)[i];
        uint32_t gid;
        if (!qp_os_index_check(ctx, elem)) {
            qp_raise_type_error(ctx, "groups must be integers");
            goto fail;
        }
        if (qp_os_id_conv(ctx, elem, &gid, "gid") < 0)
            goto fail;
        grouplist[i] = gid;
    }
    JS_FreeValue(ctx, seq);
    if (setgroups((int)len, grouplist) < 0) {
        int err = errno;
        js_free(ctx, grouplist);
        return qp_os_error(ctx, err);
    }
    js_free(ctx, grouplist);
    return JS_UNDEFINED;
fail:
    js_free(ctx, grouplist);
    JS_FreeValue(ctx, seq);
    return JS_EXCEPTION;
}

#ifdef QP_OS_HAVE_SETRESUID
/* setresuid (0) / setresgid (1) */
QP_OS_FN(qp_os_setresid)
{
    uint32_t ids[3];
    const char *what = magic ? "gid" : "uid";
    int i, r;
    if (qp_arg_check_positional(ctx, magic ? "setresgid" : "setresuid", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    for (i = 0; i < 3; i++) {
        if (qp_os_id_conv(ctx, args[i], &ids[i], what) < 0)
            return JS_EXCEPTION;
    }
    r = magic ? setresgid(ids[0], ids[1], ids[2]) : setresuid(ids[0], ids[1], ids[2]);
    if (r < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* getresuid (0) / getresgid (1) */
QP_OS_FN(qp_os_getresid)
{
    JSValue items[3];
    int i;
    if (magic) {
        gid_t g[3];
        if (getresgid(&g[0], &g[1], &g[2]) < 0)
            return qp_os_error(ctx, errno);
        for (i = 0; i < 3; i++)
            items[i] = qp_os_from_id(ctx, g[i]);
    } else {
        uid_t u[3];
        if (getresuid(&u[0], &u[1], &u[2]) < 0)
            return qp_os_error(ctx, errno);
        for (i = 0; i < 3; i++)
            items[i] = qp_os_from_id(ctx, u[i]);
    }
    for (i = 0; i < 3; i++) {
        if (JS_IsException(items[i])) {
            for (i = 0; i < 3; i++)
                JS_FreeValue(ctx, items[i]);
            return JS_EXCEPTION;
        }
    }
    return qp_tuple_new_steal(ctx, 3, items);
}
#endif /* QP_OS_HAVE_SETRESUID */

/* ---- wait3 / wait4 / waitid ---- */

/* wait_helper: (pid, status, resource.struct_rusage) */
static JSValue qp_os_wait_helper(JSContext *ctx, pid_t pid, int status, struct rusage *ru)
{
    JSValue mod, struct_rusage, fields[16], tup, rusage, items[3];
    JSAtom at;
    int i = 0;
    /* If wait succeeded but no child was ready to report status, ru will
       not have been populated. */
    if (pid == 0)
        memset(ru, 0, sizeof(*ru));
    mod = qp_import_module(ctx, "resource");
    if (JS_IsException(mod))
        return mod;
    at = JS_NewAtom(ctx, "struct_rusage");
    struct_rusage = qp_getattr(ctx, mod, at);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(struct_rusage))
        return struct_rusage;
#define QP_OS_DOUBLETIME(TV) ((double)(TV).tv_sec + (TV).tv_usec * 0.000001)
    fields[i++] = qp_float_new(ctx, QP_OS_DOUBLETIME(ru->ru_utime));
    fields[i++] = qp_float_new(ctx, QP_OS_DOUBLETIME(ru->ru_stime));
#undef QP_OS_DOUBLETIME
    fields[i++] = qp_long_from_int64(ctx, ru->ru_maxrss);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_ixrss);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_idrss);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_isrss);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_minflt);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_majflt);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_nswap);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_inblock);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_oublock);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_msgsnd);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_msgrcv);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_nsignals);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_nvcsw);
    fields[i++] = qp_long_from_int64(ctx, ru->ru_nivcsw);
    for (i = 0; i < 16; i++) {
        if (JS_IsException(fields[i])) {
            for (i = 0; i < 16; i++)
                JS_FreeValue(ctx, fields[i]);
            JS_FreeValue(ctx, struct_rusage);
            return JS_EXCEPTION;
        }
    }
    tup = qp_tuple_new_steal(ctx, 16, fields);
    if (JS_IsException(tup)) {
        JS_FreeValue(ctx, struct_rusage);
        return tup;
    }
    rusage = qp_call1(ctx, struct_rusage, tup);
    JS_FreeValue(ctx, tup);
    JS_FreeValue(ctx, struct_rusage);
    if (JS_IsException(rusage))
        return rusage;
    items[0] = JS_NewInt32(ctx, (int)pid);
    items[1] = JS_NewInt32(ctx, status);
    items[2] = rusage;
    return qp_tuple_new_steal(ctx, 3, items);
}

/* os.wait3(options) (magic 0) / os.wait4(pid, options) (magic 1) */
QP_OS_FN(qp_os_wait3)
{
    static const char *const kw3[] = { "options", NULL };
    static const char *const kw4[] = { "pid", "options", NULL };
    static const QPArgSpec specs[2] = { { "wait3", kw3, 1, 1, 0 }, { "wait4", kw4, 2, 2, 0 } };
    JSValueConst a[2];
    pid_t pid = -1, res;
    int options, status = 0, err;
    struct rusage ru;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (magic) {
        if (qp_os_pid(ctx, a[0], &pid) < 0 || qp_os_int(ctx, a[1], &options) < 0)
            return JS_EXCEPTION;
    } else if (qp_os_int(ctx, a[0], &options) < 0) {
        return JS_EXCEPTION;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        res = magic ? wait4(pid, &status, options, &ru) : wait3(&status, options, &ru);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res >= 0)
            break;
        if (err != EINTR)
            return qp_os_error(ctx, err);
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    return qp_os_wait_helper(ctx, res, status, &ru);
}

#define QP_OS_PROP_WAITID "\x01qp_os_waitid_result"

static const char *const qp_os_waitid_result_fields[] = {
    "si_pid", "si_uid", "si_signo", "si_status", "si_code", NULL
};
static const QPStructSeqDesc qp_os_waitid_result_desc = {
    "posix.waitid_result", "waitid_result: Result from waitid.\n\n\
This object may be accessed either as a tuple of\n\
  (si_pid, si_uid, si_signo, si_status, si_code),\n\
or via the attributes si_pid, si_uid, and so on.\n\
\n\
See os.waitid for more information.",
    qp_os_waitid_result_fields, NULL, 5, 0
};

/* os.waitid(idtype, id, options, /) */
QP_OS_FN(qp_os_waitid)
{
    int idtype, id, options, res, err, i;
    siginfo_t si;
    QPType *t;
    JSValue items[5];
    if (qp_arg_check_positional(ctx, "waitid", nargs, 3, 3) < 0 ||
        qp_arg_int(ctx, args[0], &idtype) < 0 || qp_arg_int(ctx, args[1], &id) < 0 ||
        qp_arg_int(ctx, args[2], &options) < 0)
        return JS_EXCEPTION;
    memset(&si, 0, sizeof(si));
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        res = waitid((idtype_t)idtype, (id_t)id, &si, options);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (res >= 0)
            break;
        if (err != EINTR)
            return qp_os_error(ctx, err);
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    if (si.si_pid == 0)
        return JS_UNDEFINED;
    t = qp_os_get_type(ctx, self, QP_OS_PROP_WAITID);
    if (!t)
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, (int)si.si_pid);
    items[1] = qp_os_from_id(ctx, si.si_uid);
    items[2] = JS_NewInt32(ctx, si.si_signo);
    items[3] = JS_NewInt32(ctx, si.si_status);
    items[4] = JS_NewInt32(ctx, si.si_code);
    if (JS_IsException(items[1])) {
        for (i = 0; i < 5; i++)
            JS_FreeValue(ctx, items[i]);
        return JS_EXCEPTION;
    }
    return qp_structseq_new(ctx, t, items);
}

/* ---- Linux process descriptors and namespaces ---- */

#ifdef QP_OS_HAVE_PIDFD_OPEN
/* os.pidfd_open(pid, flags=0) */
QP_OS_FN(qp_os_pidfd_open)
{
    pid_t pid;
    uint64_t flags = 0;
    int fd;
    QP_OS_PARSE("pidfd_open", 1, 2, "pid", "flags");
    if (qp_os_pid(ctx, a[0], &pid) < 0 ||
        (!QP_IS_NULL(a[1]) &&
         qp_os_unsigned(ctx, a[1], sizeof(unsigned int), 0, NULL, NULL, "unsigned int", &flags) < 0))
        return JS_EXCEPTION;
    fd = (int)syscall(__NR_pidfd_open, pid, (unsigned int)flags);
    if (fd < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, fd);
}
#endif

#ifdef QP_OS_HAVE_SETNS
/* os.setns(fd, nstype=0) */
QP_OS_FN(qp_os_setns)
{
    int fd, nstype, res, err;
    QP_OS_PARSE("setns", 1, 2, "fd", "nstype");
    if (qp_object_as_fd(ctx, a[0], &fd) < 0 || qp_os_opt_int(ctx, a[1], 0, &nstype) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    res = setns(fd, nstype);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (res != 0)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}
#endif

#ifdef QP_OS_HAVE_UNSHARE
/* os.unshare(flags) */
QP_OS_FN(qp_os_unshare)
{
    int flags, res, err;
    QP_OS_PARSE("unshare", 1, 1, "flags");
    if (qp_os_int(ctx, a[0], &flags) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    res = unshare(flags);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (res != 0)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}
#endif

/* ---- terminals and file locks ---- */

/* os.tcgetpgrp(fd, /) */
QP_OS_FN(qp_os_tcgetpgrp)
{
    int fd;
    pid_t pgid;
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    pgid = tcgetpgrp(fd);
    if (pgid < 0)
        return qp_os_error(ctx, errno);
    return JS_NewInt32(ctx, (int)pgid);
}

/* os.tcsetpgrp(fd, pgid, /) */
QP_OS_FN(qp_os_tcsetpgrp)
{
    int fd;
    pid_t pgid;
    if (qp_arg_check_positional(ctx, "tcsetpgrp", nargs, 2, 2) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_os_pid(ctx, args[1], &pgid) < 0)
        return JS_EXCEPTION;
    if (tcsetpgrp(fd, pgid) < 0)
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}

/* os.lockf(fd, command, length, /) */
QP_OS_FN(qp_os_lockf)
{
    int fd, command, res, err;
    off_t length;
    if (qp_arg_check_positional(ctx, "lockf", nargs, 3, 3) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_arg_int(ctx, args[1], &command) < 0 ||
        qp_os_off(ctx, args[2], &length) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    res = lockf(fd, command, length);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (res < 0)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}

/* ---- vectored and kernel-side I/O ---- */

/* iov_setup over the items of a tuple: buffers of kind 'type'
   (QP_BUF_WRITABLE / QP_BUF_SIMPLE); qp_os_iov_cleanup() releases them */
static int qp_os_iov_setup(JSContext *ctx, JSValueConst seq, int type, struct iovec **piov,
                           QPBuffer **pviews)
{
    Py_ssize_t cnt = QP_TUPLE_SIZE(seq), i;
    QPBuffer *views = js_mallocz(ctx, sizeof(QPBuffer) * (cnt ? cnt : 1));
    struct iovec *iov = js_malloc(ctx, sizeof(struct iovec) * (cnt ? cnt : 1));
    if (!views || !iov) {
        js_free(ctx, views);
        js_free(ctx, iov);
        return -1;
    }
    for (i = 0; i < cnt; i++) {
        if (qp_buffer_get(ctx, QP_TUPLE_ITEMS(seq)[i], &views[i], type) < 0) {
            while (--i >= 0)
                qp_buffer_release(ctx, &views[i]);
            js_free(ctx, views);
            js_free(ctx, iov);
            return -1;
        }
        iov[i].iov_base = views[i].buf;
        iov[i].iov_len = views[i].len;
    }
    *piov = iov;
    *pviews = views;
    return 0;
}

static void qp_os_iov_cleanup(JSContext *ctx, struct iovec *iov, QPBuffer *views, Py_ssize_t cnt)
{
    Py_ssize_t i;
    for (i = 0; i < cnt; i++)
        qp_buffer_release(ctx, &views[i]);
    js_free(ctx, views);
    js_free(ctx, iov);
}

/* os.preadv(fd, buffers, offset, flags=0, /) (magic 0) /
   os.pwritev(fd, buffers, offset, flags=0, /) (magic 1) -> Py_ssize_t */
QP_OS_FN(qp_os_preadv)
{
    const char *fn = magic ? "pwritev" : "preadv";
    int fd, flags = 0, err = 0, cnt;
    off_t offset;
    JSValue seq;
    struct iovec *iov;
    QPBuffer *views;
    ssize_t n;
    if (qp_arg_check_positional(ctx, fn, nargs, 3, 4) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (!qp_abs_sequence_check(ctx, args[1]))
        return qp_raise_type_error(ctx, magic ? "pwritev() arg 2 must be a sequence"
                                              : "preadv2() arg 2 must be a sequence");
    if (qp_os_off(ctx, args[2], &offset) < 0 || (nargs > 3 && qp_arg_int(ctx, args[3], &flags) < 0))
        return JS_EXCEPTION;
#ifndef QP_OS_HAVE_PREADV2
    if (flags != 0)
        return qp_os_argument_unavailable(ctx, magic ? "pwritev2" : "preadv2", "flags");
#endif
    seq = qp_os_seq_tuple(ctx, args[1], magic ? "pwritev() arg 2 must be a sequence"
                                              : "preadv2() arg 2 must be a sequence");
    if (JS_IsException(seq))
        return seq;
    if (QP_TUPLE_SIZE(seq) > INT_MAX) {
        JS_FreeValue(ctx, seq);
        return qp_raise_overflow_error(ctx, "too many buffers");
    }
    cnt = (int)QP_TUPLE_SIZE(seq);
    if (qp_os_iov_setup(ctx, seq, magic ? QP_BUF_SIMPLE : QP_BUF_WRITABLE, &iov, &views) < 0) {
        JS_FreeValue(ctx, seq);
        return JS_EXCEPTION;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
#ifdef QP_OS_HAVE_PREADV2
        n = magic ? pwritev2(fd, iov, cnt, offset, flags) : preadv2(fd, iov, cnt, offset, flags);
#else
        n = magic ? pwritev(fd, iov, cnt, offset) : preadv(fd, iov, cnt, offset);
#endif
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0) {
            n = -2;
            break;
        }
    }
    qp_os_iov_cleanup(ctx, iov, views, cnt);
    JS_FreeValue(ctx, seq);
    if (n == -2)
        return JS_EXCEPTION;
    if (n < 0)
        return qp_os_error(ctx, err);
    return qp_long_from_int64(ctx, (int64_t)n);
}

#ifdef __APPLE__
/* sendfile() headers / trailers: a sequence of buffers (cnt 0: none) */
static int qp_os_sf_vec(JSContext *ctx, JSValueConst o, const char *what, JSValue *pseq,
                        struct iovec **piov, QPBuffer **pviews, int *pcnt)
{
    char msg[64];
    JSValue seq;
    *pseq = JS_UNDEFINED;
    *pcnt = 0;
    if (QP_IS_NULL(o))
        return 0;
    snprintf(msg, sizeof(msg), "sendfile() %s must be a sequence", what);
    if (!qp_abs_sequence_check(ctx, o)) {
        qp_raise_type_error(ctx, "%s", msg);
        return -1;
    }
    seq = qp_os_seq_tuple(ctx, o, msg);
    if (JS_IsException(seq))
        return -1;
    if (QP_TUPLE_SIZE(seq) > INT_MAX) {
        JS_FreeValue(ctx, seq);
        qp_raise_overflow_error(ctx, "sendfile() %s is too large",
                                what[0] == 'h' ? "header" : "trailer");
        return -1;
    }
    if (QP_TUPLE_SIZE(seq) > 0) {
        if (qp_os_iov_setup(ctx, seq, QP_BUF_SIMPLE, piov, pviews) < 0) {
            JS_FreeValue(ctx, seq);
            return -1;
        }
        *pcnt = (int)QP_TUPLE_SIZE(seq);
    }
    *pseq = seq;
    return 0;
}

/* os.sendfile(out_fd, in_fd, offset, count, headers=(), trailers=(),
   flags=0): macOS sendfile(2), count 0 sends until the end of in_fd */
QP_OS_FN(qp_os_sendfile)
{
    JSValue hseq = JS_UNDEFINED, tseq = JS_UNDEFINED;
    struct iovec *hiov = NULL, *tiov = NULL;
    QPBuffer *hviews = NULL, *tviews = NULL;
    struct sf_hdtr sf;
    int out_fd, in_fd, flags, ret, err = 0, i;
    off_t offset, sbytes;
    QP_OS_PARSE("sendfile", 4, 7, "out_fd", "in_fd", "offset", "count", "headers",
                "trailers", "flags");
    if (qp_os_int(ctx, a[0], &out_fd) < 0 || qp_os_int(ctx, a[1], &in_fd) < 0 ||
        qp_os_off(ctx, a[2], &offset) < 0 || qp_os_off(ctx, a[3], &sbytes) < 0 ||
        qp_os_opt_int(ctx, a[6], 0, &flags) < 0)
        return JS_EXCEPTION;
    if (sbytes < 0)
        return qp_raise_value_error(ctx, "count cannot be negative");
    memset(&sf, 0, sizeof(sf));
    if (qp_os_sf_vec(ctx, a[4], "headers", &hseq, &hiov, &hviews, &sf.hdr_cnt) < 0)
        return JS_EXCEPTION;
    if (sf.hdr_cnt) {
        sf.headers = hiov;
        for (i = 0; i < sf.hdr_cnt; i++) {
            Py_ssize_t blen = hiov[i].iov_len;
            if (sbytes >= 0x7fffffffffffffffLL - blen) {
                qp_raise_overflow_error(ctx, "sendfile() header is too large");
                goto fail;
            }
            sbytes += blen;
        }
    }
    if (qp_os_sf_vec(ctx, a[5], "trailers", &tseq, &tiov, &tviews, &sf.trl_cnt) < 0)
        goto fail;
    if (sf.trl_cnt)
        sf.trailers = tiov;
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        ret = sendfile(in_fd, out_fd, offset, &sbytes, &sf, flags);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (ret >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0)
            goto fail;
    }
    if (sf.hdr_cnt)
        qp_os_iov_cleanup(ctx, hiov, hviews, sf.hdr_cnt);
    if (sf.trl_cnt)
        qp_os_iov_cleanup(ctx, tiov, tviews, sf.trl_cnt);
    JS_FreeValue(ctx, hseq);
    JS_FreeValue(ctx, tseq);
    if (ret < 0) {
        /* EAGAIN / EBUSY after some data was sent is a partial success; the
           caller is supposed to retry when nothing was sent */
        if ((err == EAGAIN || err == EBUSY) && sbytes != 0)
            return qp_long_from_int64(ctx, (int64_t)sbytes);
        return qp_os_error(ctx, err);
    }
    return qp_long_from_int64(ctx, (int64_t)sbytes);
fail:
    if (sf.hdr_cnt)
        qp_os_iov_cleanup(ctx, hiov, hviews, sf.hdr_cnt);
    if (sf.trl_cnt)
        qp_os_iov_cleanup(ctx, tiov, tviews, sf.trl_cnt);
    JS_FreeValue(ctx, hseq);
    JS_FreeValue(ctx, tseq);
    return JS_EXCEPTION;
}
#else /* !__APPLE__ */
/* os.sendfile(out_fd, in_fd, offset, count): Linux sendfile(2), offset
   None reads from the current position of in_fd */
QP_OS_FN(qp_os_sendfile)
{
    int out_fd, in_fd, err;
    Py_ssize_t count;
    off_t offset, *poffset = NULL;
    ssize_t ret;
    QP_OS_PARSE("sendfile", 4, 4, "out_fd", "in_fd", "offset", "count");
    if (qp_os_int(ctx, a[0], &out_fd) < 0 || qp_os_int(ctx, a[1], &in_fd) < 0 ||
        qp_arg_ssize(ctx, a[3], &count) < 0)
        return JS_EXCEPTION;
    if (count < 0)
        return qp_raise_value_error(ctx, "count cannot be negative");
    if (!JS_IsUndefined(a[2]) && !JS_IsNull(a[2])) {
        if (qp_os_off(ctx, a[2], &offset) < 0)
            return JS_EXCEPTION;
        poffset = &offset;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        ret = sendfile(out_fd, in_fd, poffset, count);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (ret >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    if (ret < 0)
        return qp_os_error(ctx, err);
    return qp_long_from_int64(ctx, (int64_t)ret);
}
#endif /* __APPLE__ */

#ifdef QP_OS_HAVE_FCOPYFILE
/* os._fcopyfile(in_fd, out_fd, flags, /) */
QP_OS_FN(qp_os__fcopyfile)
{
    int in_fd, out_fd, flags, ret, err;
    if (qp_arg_check_positional(ctx, "_fcopyfile", nargs, 3, 3) < 0 ||
        qp_arg_int(ctx, args[0], &in_fd) < 0 || qp_arg_int(ctx, args[1], &out_fd) < 0 ||
        qp_arg_int(ctx, args[2], &flags) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    ret = fcopyfile(in_fd, out_fd, NULL, flags);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (ret < 0)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}
#endif

#if defined(QP_OS_HAVE_COPY_FILE_RANGE) || defined(QP_OS_HAVE_SPLICE)
/* os.copy_file_range(src, dst, count, offset_src=None, offset_dst=None)
   (magic 0) / os.splice(src, dst, count, offset_src=None,
   offset_dst=None, flags=0) (magic 1) */
QP_OS_FN(qp_os_copy_file_range)
{
    static const char *const kw[] = { "src", "dst", "count", "offset_src", "offset_dst",
                                      "flags", NULL };
    static const QPArgSpec specs[2] = { { "copy_file_range", kw, 3, 5, 0 },
                                        { "splice", kw, 3, 6, 0 } };
    JSValueConst a[6];
    int src, dst, err;
    Py_ssize_t count;
    off_t offset_src_val, offset_dst_val, *p_offset_src = NULL, *p_offset_dst = NULL;
    uint64_t flags = 0;
    ssize_t ret;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0 ||
        qp_os_int(ctx, a[0], &src) < 0 || qp_os_int(ctx, a[1], &dst) < 0 ||
        qp_arg_ssize(ctx, a[2], &count) < 0)
        return JS_EXCEPTION;
    if (count < 0)
        return qp_raise_value_error(ctx, "count cannot be negative");
    if (!QP_IS_NULL(a[3]) && !JS_IsUndefined(a[3]) && !JS_IsNull(a[3])) {
        if (qp_os_off(ctx, a[3], &offset_src_val) < 0)
            return JS_EXCEPTION;
        p_offset_src = &offset_src_val;
    }
    if (!QP_IS_NULL(a[4]) && !JS_IsUndefined(a[4]) && !JS_IsNull(a[4])) {
        if (qp_os_off(ctx, a[4], &offset_dst_val) < 0)
            return JS_EXCEPTION;
        p_offset_dst = &offset_dst_val;
    }
    if (magic && !QP_IS_NULL(a[5]) &&
        qp_os_unsigned(ctx, a[5], sizeof(unsigned int), 0, NULL, NULL, "unsigned int", &flags) < 0)
        return JS_EXCEPTION;
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        if (magic) {
#ifdef QP_OS_HAVE_SPLICE
            ret = splice(src, p_offset_src, dst, p_offset_dst, count, (unsigned int)flags);
#else
            ret = -1;
            errno = ENOSYS;
#endif
        } else {
#ifdef QP_OS_HAVE_COPY_FILE_RANGE
            /* the flags argument is for future extensions and must be 0 */
            ret = copy_file_range(src, p_offset_src, dst, p_offset_dst, count, 0);
#else
            ret = -1;
            errno = ENOSYS;
#endif
        }
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (ret >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    if (ret < 0)
        return qp_os_error(ctx, err);
    return qp_long_from_int64(ctx, (int64_t)ret);
}
#endif

/* ---- special files and device numbers ---- */

/* os.mkfifo(path, mode=0o666, *, dir_fd=None) (magic 0) /
   os.mknod(path, mode=0o600, device=0, *, dir_fd=None) (magic 1) */
QP_OS_FN(qp_os_mkfifo)
{
    static const char *const kwf[] = { "path", "mode", "dir_fd", NULL };
    static const char *const kwn[] = { "path", "mode", "device", "dir_fd", NULL };
    static const QPArgSpec specs[2] = { { "mkfifo", kwf, 1, 2, 0 }, { "mknod", kwn, 1, 3, 0 } };
    const char *fn = magic ? "mknod" : "mkfifo";
    QPOSPath path = QP_OS_PATH(NULL, "path", 0, 0);
    JSValueConst a[4];
    int mode, dir_fd, result, err = 0, unavailable = 0;
    dev_t device = 0;
    JSValue r = JS_UNDEFINED;
    path.function_name = fn;
    if (qp_arg_parse(ctx, &specs[magic], args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_os_path_conv(ctx, a[0], &path) < 0)
        return JS_EXCEPTION;
    if (qp_os_opt_int(ctx, a[1], magic ? 0600 : 0666, &mode) < 0 ||
        (magic && !QP_IS_NULL(a[2]) && qp_os_dev_conv(ctx, a[2], &device) < 0) ||
        qp_os_dir_fd_conv(ctx, a[magic ? 3 : 2], &dir_fd) < 0) {
        qp_os_path_clean(ctx, &path);
        return JS_EXCEPTION;
    }
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        if (dir_fd != QP_OS_DEFAULT_DIR_FD && magic) {
            if (QP_OS_HAVE_MKNODAT_RUNTIME) {
                result = mknodat(dir_fd, path.narrow, mode, device);
            } else {
                unavailable = 1;
                result = 0;
            }
        } else if (dir_fd != QP_OS_DEFAULT_DIR_FD) {
            if (QP_OS_HAVE_MKFIFOAT_RUNTIME) {
                result = mkfifoat(dir_fd, path.narrow, mode);
            } else {
                unavailable = 1;
                result = 0;
            }
        } else {
            result = magic ? mknod(path.narrow, mode, device) : mkfifo(path.narrow, mode);
        }
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (result == 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0) {
            qp_os_path_clean(ctx, &path);
            return JS_EXCEPTION;
        }
    }
    if (unavailable)
        r = qp_os_argument_unavailable(ctx, NULL, "dir_fd");
    else if (result != 0)
        r = qp_os_error(ctx, err);
    qp_os_path_clean(ctx, &path);
    return r;
}

/* major_minor_conv */
static JSValue qp_os_major_minor_conv(JSContext *ctx, unsigned int value)
{
#ifdef NODEV
    if (value == (unsigned int)NODEV)
        return JS_NewInt32(ctx, (int)NODEV);
#endif
    return qp_long_from_uint64(ctx, value);
}

/* major_minor_check */
static int qp_os_major_minor_check(dev_t value)
{
#ifdef NODEV
    if (value == NODEV)
        return 1;
#endif
    return (dev_t)(unsigned int)value == value;
}

/* os.major(device, /) (magic 0) / os.minor(device, /) (magic 1) */
QP_OS_FN(qp_os_major)
{
    dev_t device;
    if (qp_os_dev_conv(ctx, args[0], &device) < 0)
        return JS_EXCEPTION;
    return qp_os_major_minor_conv(ctx, magic ? (unsigned int)minor(device)
                                             : (unsigned int)major(device));
}

/* os.makedev(major, minor, /) -> dev_t */
QP_OS_FN(qp_os_makedev)
{
    dev_t maj, min;
    if (qp_arg_check_positional(ctx, "makedev", nargs, 2, 2) < 0 ||
        qp_os_dev_conv(ctx, args[0], &maj) < 0 || qp_os_dev_conv(ctx, args[1], &min) < 0)
        return JS_EXCEPTION;
    if (!qp_os_major_minor_check(maj) || !qp_os_major_minor_check(min))
        return qp_raise_overflow_error(ctx, "Python int too large to convert to C unsigned int");
    return qp_os_long_from_dev(ctx, makedev(maj, min));
}

#if defined(QP_OS_HAVE_POSIX_FALLOCATE) || defined(QP_OS_HAVE_POSIX_FADVISE)
/* os.posix_fallocate(fd, offset, length, /) (magic 0) /
   os.posix_fadvise(fd, offset, length, advice, /) (magic 1) */
QP_OS_FN(qp_os_posix_fallocate)
{
    int fd, advice = 0, result;
    off_t offset, length;
    if (qp_arg_check_positional(ctx, magic ? "posix_fadvise" : "posix_fallocate", nargs,
                                3 + magic, 3 + magic) < 0 ||
        qp_arg_int(ctx, args[0], &fd) < 0 || qp_os_off(ctx, args[1], &offset) < 0 ||
        qp_os_off(ctx, args[2], &length) < 0 || (magic && qp_arg_int(ctx, args[3], &advice) < 0))
        return JS_EXCEPTION;
    for (;;) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        result = magic ? posix_fadvise(fd, offset, length, advice)
                       : posix_fallocate(fd, offset, length);
        /* DragonFly BSD returns -1 and sets errno */
        if (result == -1)
            result = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (result != EINTR)
            break;
        if (qp_signal_check(ctx) < 0)
            return JS_EXCEPTION;
    }
    if (result == 0)
        return JS_UNDEFINED;
    return qp_os_error(ctx, result);
}
#endif

#ifdef QP_OS_HAVE_CLEARENV
/* os._clearenv() */
QP_OS_FN(qp_os__clearenv)
{
    errno = 0;
    if (clearenv())
        return qp_os_error(ctx, errno);
    return JS_UNDEFINED;
}
#endif

/* ---- Linux file descriptors ---- */

#ifdef QP_OS_HAVE_MEMFD_CREATE
/* os.memfd_create(name, flags=MFD_CLOEXEC) */
QP_OS_FN(qp_os_memfd_create)
{
    JSValue name;
    const uint8_t *bytes;
    Py_ssize_t len;
    uint64_t flags = MFD_CLOEXEC;
    int fd, err;
    QP_OS_PARSE("memfd_create", 1, 2, "name", "flags");
    name = qp_os_fsconvert(ctx, a[0]);
    if (JS_IsException(name))
        return name;
    if (!QP_IS_NULL(a[1]) &&
        qp_os_unsigned(ctx, a[1], sizeof(unsigned int), 1, "memfd_create", "argument 'flags'",
                       "unsigned int", &flags) < 0) {
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    bytes = qp_bytes_data(name, &len);
    QP_BEGIN_ALLOW_THREADS(ctx)
    fd = memfd_create((const char *)bytes, (unsigned int)flags);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    JS_FreeValue(ctx, name);
    if (fd == -1)
        return qp_os_error(ctx, err);
    return JS_NewInt32(ctx, fd);
}
#endif

#if defined(QP_OS_HAVE_EVENTFD) && defined(EFD_CLOEXEC)
/* os.eventfd(initval, flags=EFD_CLOEXEC) */
QP_OS_FN(qp_os_eventfd)
{
    uint64_t initval;
    int flags, fd, err;
    QP_OS_PARSE("eventfd", 1, 2, "initval", "flags");
    if (qp_os_unsigned(ctx, a[0], sizeof(unsigned int), 0, NULL, NULL, "unsigned int", &initval) < 0 ||
        qp_os_opt_int(ctx, a[1], EFD_CLOEXEC, &flags) < 0)
        return JS_EXCEPTION;
    /* initval is limited to uint32_t, internal counter is uint64_t */
    QP_BEGIN_ALLOW_THREADS(ctx)
    fd = eventfd((unsigned int)initval, flags);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (fd == -1)
        return qp_os_error(ctx, err);
    return JS_NewInt32(ctx, fd);
}

/* os.eventfd_read(fd) */
QP_OS_FN(qp_os_eventfd_read)
{
    eventfd_t value;
    int fd, result, err;
    QP_OS_PARSE("eventfd_read", 1, 1, "fd");
    if (qp_object_as_fd(ctx, a[0], &fd) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    result = eventfd_read(fd, &value);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (result == -1)
        return qp_os_error(ctx, err);
    return qp_long_from_uint64(ctx, value);
}

/* os.eventfd_write(fd, value) */
QP_OS_FN(qp_os_eventfd_write)
{
    uint64_t value;
    int fd, result, err;
    QP_OS_PARSE("eventfd_write", 2, 2, "fd", "value");
    if (qp_object_as_fd(ctx, a[0], &fd) < 0 ||
        qp_os_unsigned(ctx, a[1], sizeof(unsigned long long), 0, NULL, NULL,
                       "unsigned long long", &value) < 0)
        return JS_EXCEPTION;
    QP_BEGIN_ALLOW_THREADS(ctx)
    result = eventfd_write(fd, value);
    err = errno;
    QP_END_ALLOW_THREADS(ctx)
    if (result == -1)
        return qp_os_error(ctx, err);
    return JS_UNDEFINED;
}
#endif /* QP_OS_HAVE_EVENTFD && EFD_CLOEXEC */

#ifdef __linux__
/* os.getrandom(size, flags=0) */
QP_OS_FN(qp_os_getrandom)
{
    Py_ssize_t size, n;
    int flags, err = 0;
    char *buf;
    JSValue r;
    QP_OS_PARSE("getrandom", 1, 2, "size", "flags");
    if (qp_arg_ssize(ctx, a[0], &size) < 0 || qp_os_opt_int(ctx, a[1], 0, &flags) < 0)
        return JS_EXCEPTION;
    if (size < 0)
        return qp_os_error(ctx, EINVAL);
    buf = js_malloc(ctx, size ? size : 1);
    if (!buf)
        return JS_EXCEPTION;
    for (;;) {
        n = getrandom(buf, size, flags);
        err = errno;
        if (n >= 0 || err != EINTR)
            break;
        if (qp_signal_check(ctx) < 0) {
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
    }
    if (n < 0) {
        js_free(ctx, buf);
        return qp_os_error(ctx, err);
    }
    r = qp_bytes_new(ctx, buf, n);
    js_free(ctx, buf);
    return r;
}
#endif

/* ---- interpreter helpers ---- */

#ifdef CONFIG_PYTHON_CAPI
/* python/capi_stable.c (PyAPI_DATA of pythonrun.h) */
extern __attribute__((visibility("default"))) int (*PyOS_InputHook)(void);
#endif

/* os._inputhook(): call PyOS_InputHook (set by extensions like tkinter) */
QP_OS_FN(qp_os__inputhook)
{
    int result = 0;
#ifdef CONFIG_PYTHON_CAPI
    int (*hook)(void) = PyOS_InputHook;
    if (hook) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        result = hook();
        QP_END_ALLOW_THREADS(ctx)
    }
#endif
    return JS_NewInt32(ctx, result);
}

/* os._is_inputhook_installed() */
QP_OS_FN(qp_os__is_inputhook_installed)
{
#ifdef CONFIG_PYTHON_CAPI
    return qp_bool_new(PyOS_InputHook != NULL);
#else
    return qp_bool_new(0);
#endif
}

static int qp_os_environ_fill(JSContext *ctx, JSValueConst d);

/* os._create_environ(): a new posix.environ-like dict of the current
   process environment (os.reload_environ) */
QP_OS_FN(qp_os__create_environ)
{
    JSValue d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return d;
    if (qp_os_environ_fill(ctx, d) < 0) {
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    return d;
}

/* ---------------------------------------------------------------------- */
/* module                                                                  */

static const QPMethodDef qp_posix_methods[] = {
    { "stat", qp_os_stat, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "stat($module, /, path, *, dir_fd=None, follow_symlinks=True)\n"
      "--\n"
      "\n"
      "Perform a stat system call on the given path.\n"
      "\n"
      "  path\n"
      "    Path to be examined; can be string, bytes, a path-like object or\n"
      "    open-file-descriptor int.\n"
      "  dir_fd\n"
      "    If not None, it should be a file descriptor open to a directory,\n"
      "    and path should be a relative string; path will then be relative to\n"
      "    that directory.\n"
      "  follow_symlinks\n"
      "    If False, and the last element of the path is a symbolic link,\n"
      "    stat will examine the symbolic link itself instead of the file\n"
      "    the link points to.\n"
      "\n"
      "dir_fd and follow_symlinks may not be implemented\n"
      "  on your platform.  If they are unavailable, using them will raise a\n"
      "  NotImplementedError.\n"
      "\n"
      "It\'s an error to use dir_fd or follow_symlinks when specifying path as\n"
      "  an open file descriptor." },
    { "lstat", qp_os_lstat, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "lstat($module, /, path, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Perform a stat system call on the given path, without following symbolic links.\n"
      "\n"
      "Like stat(), but do not follow symbolic links.\n"
      "Equivalent to stat(path, follow_symlinks=False)." },
    { "fstat", qp_os_fstat, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fstat($module, /, fd)\n"
      "--\n"
      "\n"
      "Perform a stat system call on the given file descriptor.\n"
      "\n"
      "Like stat(), but for an open file descriptor.\n"
      "Equivalent to os.stat(fd)." },
    { "access", qp_os_access, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "access($module, /, path, mode, *, dir_fd=None, effective_ids=False,\n"
      "       follow_symlinks=True)\n"
      "--\n"
      "\n"
      "Use the real uid/gid to test for access to a path.\n"
      "\n"
      "  path\n"
      "    Path to be tested; can be string, bytes, or a path-like object.\n"
      "  mode\n"
      "    Operating-system mode bitfield.  Can be F_OK to test existence,\n"
      "    or the inclusive-OR of R_OK, W_OK, and X_OK.\n"
      "  dir_fd\n"
      "    If not None, it should be a file descriptor open to a directory,\n"
      "    and path should be relative; path will then be relative to that\n"
      "    directory.\n"
      "  effective_ids\n"
      "    If True, access will use the effective uid/gid instead of\n"
      "    the real uid/gid.\n"
      "  follow_symlinks\n"
      "    If False, and the last element of the path is a symbolic link,\n"
      "    access will examine the symbolic link itself instead of the file\n"
      "    the link points to.\n"
      "\n"
      "dir_fd, effective_ids, and follow_symlinks may not be implemented\n"
      "  on your platform.  If they are unavailable, using them will raise a\n"
      "  NotImplementedError.\n"
      "\n"
      "Note that most operations will use the effective uid/gid, therefore this\n"
      "  routine can be used in a suid/sgid environment to test if the invoking\n"
      "  user has the specified access to the path." },
    { "chdir", qp_os_chdir, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "chdir($module, /, path)\n"
      "--\n"
      "\n"
      "Change the current working directory to the specified path.\n"
      "\n"
      "path may always be specified as a string.  On some platforms, path may\n"
      "also be specified as an open file descriptor.  If this functionality is\n"
      "unavailable, using it raises an exception." },
    { "fchdir", qp_os_fchdir, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fchdir($module, /, fd)\n"
      "--\n"
      "\n"
      "Change to the directory of the given file descriptor.\n"
      "\n"
      "fd must be opened on a directory, not a file.\n"
      "Equivalent to os.chdir(fd)." },
    { "chmod", qp_os_chmod, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "chmod($module, /, path, mode, *, dir_fd=None,\n"
      "      follow_symlinks=(os.name != \'nt\'))\n"
      "--\n"
      "\n"
      "Change the access permissions of a file.\n"
      "\n"
      "  path\n"
      "    Path to be modified.  May always be specified as a str, bytes, or\n"
      "    a path-like object.  On some platforms, path may also be specified\n"
      "    as an open file descriptor.  If this functionality is unavailable,\n"
      "    using it raises an exception.\n"
      "  mode\n"
      "    Operating-system mode bitfield.\n"
      "    Be careful when using number literals for *mode*. The conventional\n"
      "    UNIX notation for numeric modes uses an octal base, which needs to\n"
      "    be indicated with a ``0o`` prefix in Python.\n"
      "  dir_fd\n"
      "    If not None, it should be a file descriptor open to a directory,\n"
      "    and path should be relative; path will then be relative to that\n"
      "    directory.\n"
      "  follow_symlinks\n"
      "    If False, and the last element of the path is a symbolic link,\n"
      "    chmod will modify the symbolic link itself instead of the file\n"
      "    the link points to.\n"
      "\n"
      "It is an error to use dir_fd or follow_symlinks when specifying path as\n"
      "  an open file descriptor.\n"
      "dir_fd and follow_symlinks may not be implemented on your platform.\n"
      "  If they are unavailable, using them will raise a NotImplementedError." },
    { "fchmod", qp_os_fchmod, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fchmod($module, /, fd, mode)\n"
      "--\n"
      "\n"
      "Change the access permissions of the file given by file descriptor fd.\n"
      "\n"
      "  fd\n"
      "    The file descriptor of the file to be modified.\n"
      "  mode\n"
      "    Operating-system mode bitfield.\n"
      "    Be careful when using number literals for *mode*.  The conventional\n"
      "    UNIX notation for numeric modes uses an octal base, which needs to\n"
      "    be indicated with a ``0o`` prefix in Python.\n"
      "\n"
      "Equivalent to os.chmod(fd, mode)." },
#ifdef __APPLE__
    { "lchmod", qp_os_lchmod, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "lchmod($module, /, path, mode)\n"
      "--\n"
      "\n"
      "Change the access permissions of a file, without following symbolic links.\n"
      "\n"
      "If path is a symlink, this affects the link itself rather than the\n"
      "target.  Equivalent to chmod(path, mode, follow_symlinks=False)." },
#endif
    { "chown", qp_os_chown, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "chown($module, /, path, uid, gid, *, dir_fd=None, follow_symlinks=True)\n"
      "--\n"
      "\n"
      "Change the owner and group id of path to the numeric uid and gid.\n"
      "\n"
      "  path\n"
      "    Path to be examined; can be string, bytes, a path-like object, or\n"
      "    open-file-descriptor int.\n"
      "  dir_fd\n"
      "    If not None, it should be a file descriptor open to a directory,\n"
      "    and path should be relative; path will then be relative to that\n"
      "    directory.\n"
      "  follow_symlinks\n"
      "    If False, and the last element of the path is a symbolic link,\n"
      "    stat will examine the symbolic link itself instead of the file\n"
      "    the link points to.\n"
      "\n"
      "path may always be specified as a string.  On some platforms, path may\n"
      "also be specified as an open file descriptor.  If this functionality is\n"
      "unavailable, using it raises an exception.\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "If follow_symlinks is False, and the last element of the path is\n"
      "a symbolic link, chown will modify the symbolic link itself instead of\n"
      "the file the link points to.\n"
      "It is an error to use dir_fd or follow_symlinks when specifying path as\n"
      "an open file descriptor.\n"
      "dir_fd and follow_symlinks may not be implemented on your platform.  If\n"
      "they are unavailable, using them will raise a NotImplementedError." },
    { "fchown", qp_os_fchown, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fchown($module, /, fd, uid, gid)\n"
      "--\n"
      "\n"
      "Change the owner and group id of the file specified by file descriptor.\n"
      "\n"
      "Equivalent to os.chown(fd, uid, gid)." },
    { "lchown", qp_os_lchown, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "lchown($module, /, path, uid, gid)\n"
      "--\n"
      "\n"
      "Change the owner and group id of path to the numeric uid and gid.\n"
      "\n"
      "This function will not follow symbolic links.\n"
      "Equivalent to os.chown(path, uid, gid, follow_symlinks=False)." },
    { "getcwd", qp_os_getcwd, QP_METH_NOARGS, 0,
      "getcwd($module, /)\n"
      "--\n"
      "\n"
      "Return a unicode string representing the current working directory." },
    { "getcwdb", qp_os_getcwd, QP_METH_NOARGS, 1,
      "getcwdb($module, /)\n"
      "--\n"
      "\n"
      "Return a bytes string representing the current working directory." },
    { "link", qp_os_link, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "link($module, /, src, dst, *, src_dir_fd=None, dst_dir_fd=None,\n"
      "     follow_symlinks=(os.name != \'nt\'))\n"
      "--\n"
      "\n"
      "Create a hard link to a file.\n"
      "\n"
      "If either src_dir_fd or dst_dir_fd is not None, it should be a file\n"
      "descriptor open to a directory, and the respective path string (src or\n"
      "dst) should be relative; the path will then be relative to that\n"
      "directory.\n"
      "If follow_symlinks is False, and the last element of src is a symbolic\n"
      "link, link will create a link to the symbolic link itself instead of the\n"
      "file the link points to.\n"
      "src_dir_fd, dst_dir_fd, and follow_symlinks may not be implemented on\n"
      "your platform.  If they are unavailable, using them will raise\n"
      "a NotImplementedError." },
    { "listdir", qp_os_listdir, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "listdir($module, /, path=None)\n"
      "--\n"
      "\n"
      "Return a list containing the names of the files in the directory.\n"
      "\n"
      "path can be specified as either str, bytes, or a path-like object.  If\n"
      "path is bytes, the filenames returned will also be bytes; in all other\n"
      "circumstances the filenames returned will be str.\n"
      "If path is None, uses the path=\'.\'.\n"
      "On some platforms, path may also be specified as an open file\n"
      "descriptor; the file descriptor must refer to a directory.  If this\n"
      "functionality is unavailable, using it raises NotImplementedError.\n"
      "\n"
      "The list is in arbitrary order.  It does not include the special\n"
      "entries \'.\' and \'..\' even if they are present in the directory." },
    { "mkdir", qp_os_mkdir, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "mkdir($module, /, path, mode=511, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Create a directory.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError.\n"
      "\n"
      "The mode argument is ignored on Windows.  Where it is used, the current\n"
      "umask value is first masked out." },
    { "rmdir", qp_os_unlink, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "rmdir($module, /, path, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Remove a directory.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative\n"
      "to that directory.\n"
      "dir_fd may not be implemented on your platform.\n"
      "If it is unavailable, using it will raise a NotImplementedError." },
    { "unlink", qp_os_unlink, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "unlink($module, /, path, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Remove a file (same as remove()).\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "dir_fd may not be implemented on your platform.\n"
      "If it is unavailable, using it will raise a NotImplementedError." },
    { "remove", qp_os_unlink, QP_METH_FASTCALL | QP_METH_KEYWORDS, 2,
      "remove($module, /, path, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Remove a file (same as unlink()).\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative\n"
      "to that directory.\n"
      "dir_fd may not be implemented on your platform.\n"
      "If it is unavailable, using it will raise a NotImplementedError." },
    { "rename", qp_os_rename, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "rename($module, /, src, dst, *, src_dir_fd=None, dst_dir_fd=None)\n"
      "--\n"
      "\n"
      "Rename a file or directory.\n"
      "\n"
      "If either src_dir_fd or dst_dir_fd is not None, it should be a file\n"
      "descriptor open to a directory, and the respective path string (src or\n"
      "dst) should be relative; the path will then be relative to that\n"
      "directory.\n"
      "src_dir_fd and dst_dir_fd, may not be implemented on your platform.\n"
      "If they are unavailable, using them will raise a NotImplementedError." },
    { "replace", qp_os_rename, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "replace($module, /, src, dst, *, src_dir_fd=None, dst_dir_fd=None)\n"
      "--\n"
      "\n"
      "Rename a file or directory, overwriting the destination.\n"
      "\n"
      "If either src_dir_fd or dst_dir_fd is not None, it should be a file\n"
      "descriptor open to a directory, and the respective path string (src or\n"
      "dst) should be relative; the path will then be relative to that\n"
      "directory.\n"
      "src_dir_fd and dst_dir_fd, may not be implemented on your platform.\n"
      "If they are unavailable, using them will raise a NotImplementedError." },
    { "symlink", qp_os_symlink, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "symlink($module, /, src, dst, target_is_directory=False, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Create a symbolic link pointing to src named dst.\n"
      "\n"
      "target_is_directory is required on Windows if the target is to be\n"
      "interpreted as a directory.  (On Windows, symlink requires Windows 6.0\n"
      "or greater, and raises a NotImplementedError otherwise.)\n"
      "target_is_directory is ignored on non-Windows platforms.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative\n"
      "to that directory.\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError." },
    { "readlink", qp_os_readlink, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "readlink($module, /, path, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Return a string representing the path to which the symbolic link points.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError." },
    { "utime", qp_os_utime, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "utime($module, /, path, times=None, *, ns=<unrepresentable>,\n"
      "      dir_fd=None, follow_symlinks=True)\n"
      "--\n"
      "\n"
      "Set the access and modified time of path.\n"
      "\n"
      "path may always be specified as a string.  On some platforms, path may\n"
      "also be specified as an open file descriptor.  If this functionality is\n"
      "unavailable, using it raises an exception.\n"
      "\n"
      "If times is not None, it must be a tuple (atime, mtime);\n"
      "atime and mtime should be expressed as float seconds since the epoch.\n"
      "If ns is specified, it must be a tuple (atime_ns, mtime_ns);\n"
      "atime_ns and mtime_ns should be expressed as integer nanoseconds\n"
      "since the epoch.\n"
      "If times is None and ns is unspecified, utime uses the current time.\n"
      "Specifying tuples for both times and ns is an error.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "If follow_symlinks is False, and the last element of the path is\n"
      "a symbolic link, utime will modify the symbolic link itself instead of\n"
      "the file the link points to.\n"
      "It is an error to use dir_fd or follow_symlinks when specifying path as\n"
      "an open file descriptor.\n"
      "dir_fd and follow_symlinks may not be available on your platform.\n"
      "If they are unavailable, using them will raise a NotImplementedError." },
    { "umask", qp_os_umask, QP_METH_O, 0,
      "umask($module, mask, /)\n"
      "--\n"
      "\n"
      "Set the current numeric umask and return the previous umask." },
    { "open", qp_os_open, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "open($module, /, path, flags, mode=511, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Open a file for low level IO.  Returns a file descriptor (integer).\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError." },
    { "close", qp_os_close, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "close($module, /, fd)\n"
      "--\n"
      "\n"
      "Close a file descriptor." },
    { "closerange", qp_os_closerange, QP_METH_FASTCALL, 0,
      "closerange($module, fd_low, fd_high, /)\n"
      "--\n"
      "\n"
      "Closes all file descriptors in [fd_low, fd_high), ignoring errors." },
    { "dup", qp_os_dup, QP_METH_O, 0,
      "dup($module, fd, /)\n"
      "--\n"
      "\n"
      "Return a duplicate of a file descriptor." },
    { "dup2", qp_os_dup2, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "dup2($module, /, fd, fd2, inheritable=True)\n"
      "--\n"
      "\n"
      "Duplicate file descriptor." },
    { "lseek", qp_os_lseek, QP_METH_FASTCALL, 0,
      "lseek($module, fd, position, whence, /)\n"
      "--\n"
      "\n"
      "Set the position of a file descriptor.  Return the new position.\n"
      "\n"
      "  fd\n"
      "    An open file descriptor, as returned by os.open().\n"
      "  position\n"
      "    Position, interpreted relative to \'whence\'.\n"
      "  whence\n"
      "    The relative position to seek from. Valid values are:\n"
      "    - SEEK_SET: seek from the start of the file.\n"
      "    - SEEK_CUR: seek from the current file position.\n"
      "    - SEEK_END: seek from the end of the file.\n"
      "\n"
      "The return value is the number of bytes relative to the beginning of\n"
      "the file." },
    { "read", qp_os_read, QP_METH_FASTCALL, 0,
      "read($module, fd, length, /)\n"
      "--\n"
      "\n"
      "Read from a file descriptor.  Returns a bytes object." },
    { "readinto", qp_os_readinto, QP_METH_FASTCALL, 0,
      "readinto($module, fd, buffer, /)\n"
      "--\n"
      "\n"
      "Read into a buffer object from a file descriptor.\n"
      "\n"
      "The buffer should be mutable and bytes-like.  On success, returns the\n"
      "number of bytes read.  Less bytes may be read than the size of the\n"
      "buffer.  The underlying system call will be retried when interrupted by\n"
      "a signal, unless the signal handler raises an exception.  Other errors\n"
      "will not be retried and an error will be raised.\n"
      "\n"
      "Returns 0 if *fd* is at end of file or if the provided *buffer* has\n"
      "length 0 (which can be used to check for errors without reading data).\n"
      "Never returns negative." },
    { "pread", qp_os_read, QP_METH_FASTCALL, 1,
      "pread($module, fd, length, offset, /)\n"
      "--\n"
      "\n"
      "Read a number of bytes from a file descriptor starting at a particular offset.\n"
      "\n"
      "Read length bytes from file descriptor fd, starting at offset bytes from\n"
      "the beginning of the file.  The file offset remains unchanged." },
    { "write", qp_os_write, QP_METH_FASTCALL, 0,
      "write($module, fd, data, /)\n"
      "--\n"
      "\n"
      "Write a bytes object to a file descriptor." },
    { "pwrite", qp_os_write, QP_METH_FASTCALL, 1,
      "pwrite($module, fd, buffer, offset, /)\n"
      "--\n"
      "\n"
      "Write bytes to a file descriptor starting at a particular offset.\n"
      "\n"
      "Write buffer to fd, starting at offset bytes from the beginning of\n"
      "the file.  Returns the number of bytes written.  Does not change the\n"
      "current file offset." },
    { "readv", qp_os_readv, QP_METH_FASTCALL, 0,
      "readv($module, fd, buffers, /)\n"
      "--\n"
      "\n"
      "Read from a file descriptor fd into an iterable of buffers.\n"
      "\n"
      "The buffers should be mutable buffers accepting bytes.\n"
      "readv will transfer data into each buffer until it is full\n"
      "and then move on to the next buffer in the sequence to hold\n"
      "the rest of the data.\n"
      "\n"
      "readv returns the total number of bytes read,\n"
      "which may be less than the total capacity of all the buffers." },
    { "writev", qp_os_readv, QP_METH_FASTCALL, 1,
      "writev($module, fd, buffers, /)\n"
      "--\n"
      "\n"
      "Iterate over buffers, and write the contents of each to a file descriptor.\n"
      "\n"
      "Returns the total number of bytes written.\n"
      "buffers must be a sequence of bytes-like objects." },
    { "fsync", qp_os_fsync, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fsync($module, /, fd)\n"
      "--\n"
      "\n"
      "Force write of fd to disk." },
#ifdef __linux__
    { "fdatasync", qp_os_fsync, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "fdatasync($module, /, fd)\n"
      "--\n"
      "\n"
      "Force write of fd to disk without forcing update of metadata." },
#endif
    { "ftruncate", qp_os_ftruncate, QP_METH_FASTCALL, 0,
      "ftruncate($module, fd, length, /)\n"
      "--\n"
      "\n"
      "Truncate a file, specified by file descriptor, to a specific length." },
    { "truncate", qp_os_truncate, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "truncate($module, /, path, length)\n"
      "--\n"
      "\n"
      "Truncate a file, specified by path, to a specific length.\n"
      "\n"
      "On some platforms, path may also be specified as an open file\n"
      "descriptor.  If this functionality is unavailable, using it raises\n"
      "an exception." },
    { "pipe", qp_os_pipe, QP_METH_NOARGS, 0,
      "pipe($module, /)\n"
      "--\n"
      "\n"
      "Create a pipe.\n"
      "\n"
      "Returns a tuple of two file descriptors:\n"
      "  (read_fd, write_fd)" },
#ifdef QP_OS_HAVE_PIPE2
    { "pipe2", qp_os_pipe2, QP_METH_O, 0,
      "pipe2($module, flags, /)\n"
      "--\n"
      "\n"
      "Create a pipe with flags set atomically.\n"
      "\n"
      "Returns a tuple of two file descriptors:\n"
      "  (read_fd, write_fd)\n"
      "\n"
      "flags can be constructed by ORing together one or more of these values:\n"
      "O_NONBLOCK, O_CLOEXEC." },
#endif
    { "isatty", qp_os_isatty, QP_METH_O, 0,
      "isatty($module, fd, /)\n"
      "--\n"
      "\n"
      "Return True if the fd is connected to a terminal.\n"
      "\n"
      "Return True if the file descriptor is an open file descriptor\n"
      "connected to the slave end of a terminal." },
    { "get_inheritable", qp_os_get_inheritable, QP_METH_O, 0,
      "get_inheritable($module, fd, /)\n"
      "--\n"
      "\n"
      "Get the close-on-exe flag of the specified file descriptor." },
    { "set_inheritable", qp_os_set_inheritable_fn, QP_METH_FASTCALL, 0,
      "set_inheritable($module, fd, inheritable, /)\n"
      "--\n"
      "\n"
      "Set the inheritable flag of the specified file descriptor." },
    { "get_blocking", qp_os_get_blocking, QP_METH_O, 0,
      "get_blocking($module, fd, /)\n"
      "--\n"
      "\n"
      "Get the blocking mode of the file descriptor.\n"
      "\n"
      "Return False if the O_NONBLOCK flag is set, True if the flag is cleared." },
    { "set_blocking", qp_os_set_blocking, QP_METH_FASTCALL, 0,
      "set_blocking($module, fd, blocking, /)\n"
      "--\n"
      "\n"
      "Set the blocking mode of the specified file descriptor.\n"
      "\n"
      "Set the O_NONBLOCK flag if blocking is False,\n"
      "clear the O_NONBLOCK flag otherwise." },
    { "scandir", qp_os_scandir, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "scandir($module, /, path=None)\n"
      "--\n"
      "\n"
      "Return an iterator of DirEntry objects for given path.\n"
      "\n"
      "path can be specified as either str, bytes, or a path-like object.  If\n"
      "path is bytes, the names of yielded DirEntry objects will also be bytes;\n"
      "in all other circumstances they will be str.\n"
      "\n"
      "If path is None, uses the path=\'.\'." },
    { "statvfs", qp_os_statvfs, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "statvfs($module, /, path)\n"
      "--\n"
      "\n"
      "Perform a statvfs system call on the given path.\n"
      "\n"
      "path may always be specified as a string.\n"
      "On some platforms, path may also be specified as an open file\n"
      "descriptor.  If this functionality is unavailable, using it raises\n"
      "an exception." },
    { "fstatvfs", qp_os_fstatvfs, QP_METH_O, 0,
      "fstatvfs($module, fd, /)\n"
      "--\n"
      "\n"
      "Perform an fstatvfs system call on the given fd.\n"
      "\n"
      "Equivalent to statvfs(fd)." },
    { "getpid", qp_os_getid, QP_METH_NOARGS, 0,
      "getpid($module, /)\n"
      "--\n"
      "\n"
      "Return the current process id." },
    { "getppid", qp_os_getid, QP_METH_NOARGS, 1,
      "getppid($module, /)\n"
      "--\n"
      "\n"
      "Return the parent\'s process id.\n"
      "\n"
      "If the parent process has already exited, Windows machines will still\n"
      "return its id; others systems will return the id of the \'init\' proces\n"
      "(1)." },
    { "getuid", qp_os_getid, QP_METH_NOARGS, 2,
      "getuid($module, /)\n"
      "--\n"
      "\n"
      "Return the current process\'s user id." },
    { "geteuid", qp_os_getid, QP_METH_NOARGS, 3,
      "geteuid($module, /)\n"
      "--\n"
      "\n"
      "Return the current process\'s effective user id." },
    { "getgid", qp_os_getid, QP_METH_NOARGS, 4,
      "getgid($module, /)\n"
      "--\n"
      "\n"
      "Return the current process\'s group id." },
    { "getegid", qp_os_getid, QP_METH_NOARGS, 5,
      "getegid($module, /)\n"
      "--\n"
      "\n"
      "Return the current process\'s effective group id." },
    { "getpgrp", qp_os_getid, QP_METH_NOARGS, 6,
      "getpgrp($module, /)\n"
      "--\n"
      "\n"
      "Return the current process group id." },
    { "setsid", qp_os_getid, QP_METH_NOARGS, 7,
      "setsid($module, /)\n"
      "--\n"
      "\n"
      "Call the system call setsid()." },
    { "getpgid", qp_os_getpgid, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "getpgid($module, /, pid)\n"
      "--\n"
      "\n"
      "Call the system call getpgid(), and return the result." },
    { "getsid", qp_os_getsid, QP_METH_O, 0,
      "getsid($module, pid, /)\n"
      "--\n"
      "\n"
      "Call the system call getsid(pid) and return the result." },
    { "setpgid", qp_os_setpgid, QP_METH_FASTCALL, 0,
      "setpgid($module, pid, pgrp, /)\n"
      "--\n"
      "\n"
      "Call the system call setpgid(pid, pgrp)." },
    { "getgroups", qp_os_getgroups, QP_METH_NOARGS, 0,
      "getgroups($module, /)\n"
      "--\n"
      "\n"
      "Return list of supplemental group IDs for the process." },
    { "getlogin", qp_os_getlogin, QP_METH_NOARGS, 0,
      "getlogin($module, /)\n"
      "--\n"
      "\n"
      "Return the actual login name." },
    { "kill", qp_os_kill, QP_METH_FASTCALL, 0,
      "kill($module, pid, signal, /)\n"
      "--\n"
      "\n"
      "Kill a process with a signal." },
    { "killpg", qp_os_kill, QP_METH_FASTCALL, 1,
      "killpg($module, pgid, signal, /)\n"
      "--\n"
      "\n"
      "Kill a process group with a signal." },
    { "waitpid", qp_os_waitpid, QP_METH_FASTCALL, 0,
      "waitpid($module, pid, options, /)\n"
      "--\n"
      "\n"
      "Wait for completion of a given child process.\n"
      "\n"
      "Returns a tuple of information regarding the child process:\n"
      "    (pid, status)\n"
      "\n"
      "The options argument is ignored on Windows." },
    { "wait", qp_os_waitpid, QP_METH_NOARGS, 1,
      "wait($module, /)\n"
      "--\n"
      "\n"
      "Wait for completion of a child process.\n"
      "\n"
      "Returns a tuple of information about the child process:\n"
      "    (pid, status)" },
    { "waitstatus_to_exitcode", qp_os_waitstatus_to_exitcode, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "waitstatus_to_exitcode($module, /, status)\n"
      "--\n"
      "\n"
      "Convert a wait status to an exit code.\n"
      "\n"
      "On Unix:\n"
      "\n"
      "* If WIFEXITED(status) is true, return WEXITSTATUS(status).\n"
      "* If WIFSIGNALED(status) is true, return -WTERMSIG(status).\n"
      "* Otherwise, raise a ValueError.\n"
      "\n"
      "On Windows, return status shifted right by 8 bits.\n"
      "\n"
      "On Unix, if the process is being traced or if waitpid() was called with\n"
      "WUNTRACED option, the caller must first check if WIFSTOPPED(status) is\n"
      "true.  This function must not be called if WIFSTOPPED(status) is true." },
    { "WIFEXITED", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_IFEXITED,
      "WIFEXITED($module, /, status)\n"
      "--\n"
      "\n"
      "Return True if the process returning status exited via the exit() system call." },
    { "WEXITSTATUS", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_EXITSTATUS,
      "WEXITSTATUS($module, /, status)\n"
      "--\n"
      "\n"
      "Return the process return code from status." },
    { "WIFSIGNALED", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_IFSIGNALED,
      "WIFSIGNALED($module, /, status)\n"
      "--\n"
      "\n"
      "Return True if the process returning status was terminated by a signal." },
    { "WTERMSIG", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_TERMSIG,
      "WTERMSIG($module, /, status)\n"
      "--\n"
      "\n"
      "Return the signal that terminated the process that provided the status value." },
    { "WIFSTOPPED", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_IFSTOPPED,
      "WIFSTOPPED($module, /, status)\n"
      "--\n"
      "\n"
      "Return True if the process returning status was stopped." },
    { "WSTOPSIG", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_STOPSIG,
      "WSTOPSIG($module, /, status)\n"
      "--\n"
      "\n"
      "Return the signal that stopped the process that provided the status value." },
    { "WCOREDUMP", qp_os_wmacro, QP_METH_FASTCALL, QP_W_COREDUMP,
      "WCOREDUMP($module, status, /)\n"
      "--\n"
      "\n"
      "Return True if the process returning status was dumped to a core file." },
    { "WIFCONTINUED", qp_os_wmacro, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_W_IFCONTINUED,
      "WIFCONTINUED($module, /, status)\n"
      "--\n"
      "\n"
      "Return True if a particular process was continued from a job control stop.\n"
      "\n"
      "Return True if the process returning status was continued from a\n"
      "job control stop." },
    { "register_at_fork", qp_os_register_at_fork, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "register_at_fork($module, /, *, before=<unrepresentable>,\n"
      "                 after_in_child=<unrepresentable>,\n"
      "                 after_in_parent=<unrepresentable>)\n"
      "--\n"
      "\n"
      "Register callables to be called when forking a new process.\n"
      "\n"
      "  before\n"
      "    A callable to be called in the parent before the fork() syscall.\n"
      "  after_in_child\n"
      "    A callable to be called in the child after fork().\n"
      "  after_in_parent\n"
      "    A callable to be called in the parent after fork().\n"
      "\n"
      "\'before\' callbacks are called in reverse order.\n"
      "\'after_in_child\' and \'after_in_parent\' callbacks are called in order." },
    { "fork", qp_os_fork, QP_METH_NOARGS, 0,
      "fork($module, /)\n"
      "--\n"
      "\n"
      "Fork a child process.\n"
      "\n"
      "Return 0 to child process and PID of child to parent process." },
    { "execv", qp_os_execv, QP_METH_FASTCALL, 0,
      "execv($module, path, argv, /)\n"
      "--\n"
      "\n"
      "Execute an executable path with arguments, replacing current process.\n"
      "\n"
      "  path\n"
      "    Path of executable file.\n"
      "  argv\n"
      "    Tuple or list of strings." },
    { "execve", qp_os_execve, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "execve($module, /, path, argv, env)\n"
      "--\n"
      "\n"
      "Execute an executable path with arguments, replacing current process.\n"
      "\n"
      "  path\n"
      "    Path of executable file.\n"
      "  argv\n"
      "    Tuple or list of strings.\n"
      "  env\n"
      "    Dictionary of strings mapping to strings." },
    { "_exit", qp_os__exit, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "_exit($module, /, status)\n"
      "--\n"
      "\n"
      "Exit to the system with specified status, without normal exit processing." },
    { "abort", qp_os_abort, QP_METH_NOARGS, 0,
      "abort($module, /)\n"
      "--\n"
      "\n"
      "Abort the interpreter immediately.\n"
      "\n"
      "This function \'dumps core\' or otherwise fails in the hardest way\n"
      "possible on the hosting operating system.  This function never returns." },
    { "urandom", qp_os_urandom, QP_METH_O, 0,
      "urandom($module, size, /)\n"
      "--\n"
      "\n"
      "Return a bytes object containing random bytes suitable for cryptographic use." },
    { "cpu_count", qp_os_cpu_count, QP_METH_NOARGS, 0,
      "cpu_count($module, /)\n"
      "--\n"
      "\n"
      "Return the number of logical CPUs in the system.\n"
      "\n"
      "Return None if indeterminable." },
    { "uname", qp_os_uname, QP_METH_NOARGS, 0,
      "uname($module, /)\n"
      "--\n"
      "\n"
      "Return an object identifying the current operating system.\n"
      "\n"
      "The object behaves like a named tuple with the following fields:\n"
      "  (sysname, nodename, release, version, machine)" },
    { "strerror", qp_os_strerror, QP_METH_O, 0,
      "strerror($module, code, /)\n"
      "--\n"
      "\n"
      "Translate an error code to a message string." },
    { "fspath", qp_os_fspath, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "fspath($module, /, path)\n"
      "--\n"
      "\n"
      "Return the file system path representation of the object.\n"
      "\n"
      "If the object is str or bytes, then allow it to pass through as-is.  If\n"
      "the object defines __fspath__(), then return the result of that method.\n"
      "All other types raise a TypeError." },
    { "get_terminal_size", qp_os_get_terminal_size, QP_METH_FASTCALL, 0,
      "get_terminal_size($module, fd=<unrepresentable>, /)\n"
      "--\n"
      "\n"
      "Return the size of the terminal window as (columns, lines).\n"
      "\n"
      "The optional argument fd (default standard output) specifies\n"
      "which file descriptor should be queried.\n"
      "\n"
      "If the file descriptor is not connected to a terminal, an OSError\n"
      "is thrown.\n"
      "\n"
      "This function will only be defined if an implementation is\n"
      "available for this system.\n"
      "\n"
      "shutil.get_terminal_size is the high-level function which should\n"
      "normally be used, os.get_terminal_size is the low-level implementation." },
    { "times", qp_os_times, QP_METH_NOARGS, 0,
      "times($module, /)\n"
      "--\n"
      "\n"
      "Return a collection containing process timing information.\n"
      "\n"
      "The object returned behaves like a named tuple with these fields:\n"
      "  (utime, stime, cutime, cstime, elapsed_time)\n"
      "All fields are floating-point numbers." },
    { "device_encoding", qp_os_device_encoding, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "device_encoding($module, /, fd)\n"
      "--\n"
      "\n"
      "Return a string describing the encoding of a terminal\'s file descriptor.\n"
      "\n"
      "The file descriptor must be attached to a terminal.\n"
      "If the device is not a terminal, return None." },
    { "putenv", qp_os_putenv, QP_METH_FASTCALL, 0,
      "putenv($module, name, value, /)\n"
      "--\n"
      "\n"
      "Change or add an environment variable." },
    { "unsetenv", qp_os_unsetenv, QP_METH_O, 0,
      "unsetenv($module, name, /)\n"
      "--\n"
      "\n"
      "Delete an environment variable." },
    { "sync", qp_os_sync, QP_METH_NOARGS, 0,
      "sync($module, /)\n"
      "--\n"
      "\n"
      "Force write of everything to disk." },
    { "ctermid", qp_os_ctermid, QP_METH_NOARGS, 0,
      "ctermid($module, /)\n"
      "--\n"
      "\n"
      "Return the name of the controlling terminal for this process." },
    { "getloadavg", qp_os_getloadavg, QP_METH_NOARGS, 0,
      "getloadavg($module, /)\n"
      "--\n"
      "\n"
      "Return average recent system load information.\n"
      "\n"
      "Return the number of processes in the system run queue averaged over\n"
      "the last 1, 5, and 15 minutes as a tuple of three floats.\n"
      "Raises OSError if the load average was unobtainable." },
    { "sysconf", qp_os_sysconf, QP_METH_O, 0,
      "sysconf($module, name, /)\n"
      "--\n"
      "\n"
      "Return an integer-valued system configuration variable." },
    { "confstr", qp_os_confstr, QP_METH_O, 0,
      "confstr($module, name, /)\n"
      "--\n"
      "\n"
      "Return a string-valued system configuration variable." },
    { "fpathconf", qp_os_fpathconf, QP_METH_FASTCALL, 0,
      "fpathconf($module, fd, name, /)\n"
      "--\n"
      "\n"
      "Return the configuration limit name for the file descriptor fd.\n"
      "\n"
      "If there is no limit, return -1." },
    { "pathconf", qp_os_pathconf, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "pathconf($module, /, path, name)\n"
      "--\n"
      "\n"
      "Return the configuration limit name for the file or directory path.\n"
      "\n"
      "If there is no limit, return -1.\n"
      "On some platforms, path may also be specified as an open file\n"
      "descriptor.  If this functionality is unavailable, using it raises\n"
      "an exception." },
    { "ttyname", qp_os_ttyname, QP_METH_O, 0,
      "ttyname($module, fd, /)\n"
      "--\n"
      "\n"
      "Return the name of the terminal device connected to \'fd\'.\n"
      "\n"
      "  fd\n"
      "    Integer file descriptor handle." },
#ifdef QP_OS_HAVE_CHFLAGS
    { "chflags", qp_os_chflags, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "chflags($module, /, path, flags, follow_symlinks=True)\n"
      "--\n"
      "\n"
      "Set file flags.\n"
      "\n"
      "If follow_symlinks is False, and the last element of the path is\n"
      "a symbolic link, chflags() will change flags on the symbolic link itself\n"
      "instead of the file the link points to.\n"
      "follow_symlinks may not be implemented on your platform.  If it is\n"
      "unavailable, using it will raise a NotImplementedError." },
    { "lchflags", qp_os_chflags, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "lchflags($module, /, path, flags)\n"
      "--\n"
      "\n"
      "Set file flags.\n"
      "\n"
      "This function will not follow symbolic links.\n"
      "Equivalent to chflags(path, flags, follow_symlinks=False)." },
#endif
    { "chroot", qp_os_chroot, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "chroot($module, /, path)\n"
      "--\n"
      "\n"
      "Change root directory to path." },
    { "nice", qp_os_nice, QP_METH_O, 0,
      "nice($module, increment, /)\n"
      "--\n"
      "\n"
      "Add increment to the priority of process and return the new priority." },
    { "getpriority", qp_os_getpriority, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "getpriority($module, /, which, who)\n"
      "--\n"
      "\n"
      "Return program scheduling priority." },
    { "setpriority", qp_os_setpriority, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "setpriority($module, /, which, who, priority)\n"
      "--\n"
      "\n"
      "Set program scheduling priority." },
    { "system", qp_os_system, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "system($module, /, command)\n"
      "--\n"
      "\n"
      "Execute the command in a subshell." },
    { "_path_splitroot_ex", qp_os__path_splitroot_ex, QP_METH_O, 0,
      "_path_splitroot_ex($module, path, /)\n"
      "--\n"
      "\n"
      "Split a pathname into drive, root and tail.\n"
      "\n"
      "The tail contains anything after the root." },
    { "_path_normpath", qp_os__path_normpath, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "_path_normpath($module, /, path)\n"
      "--\n"
      "\n"
      "Normalize path, eliminating double slashes, etc." },
    { "posix_spawn", qp_os_posix_spawn, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "posix_spawn($module, path, argv, env, /, *, file_actions=(),\n"
      "            setpgroup=None, resetids=False, setsid=False,\n"
      "            setsigmask=(), setsigdef=(), scheduler=None)\n"
      "--\n"
      "\n"
      "Execute the program specified by path in a new process.\n"
      "\n"
      "  path\n"
      "    Path of executable file.\n"
      "  argv\n"
      "    Tuple or list of strings.\n"
      "  env\n"
      "    Dictionary of strings mapping to strings.\n"
      "  file_actions\n"
      "    A sequence of file action tuples.\n"
      "  setpgroup\n"
      "    The pgroup to use with the POSIX_SPAWN_SETPGROUP flag.\n"
      "  resetids\n"
      "    If the value is `true` the POSIX_SPAWN_RESETIDS will be activated.\n"
      "  setsid\n"
      "    If the value is `true` the POSIX_SPAWN_SETSID or POSIX_SPAWN_SETSID_NP\n"
      "    will be activated.\n"
      "  setsigmask\n"
      "    The sigmask to use with the POSIX_SPAWN_SETSIGMASK flag.\n"
      "  setsigdef\n"
      "    The sigmask to use with the POSIX_SPAWN_SETSIGDEF flag.\n"
      "  scheduler\n"
      "    A tuple with the scheduler policy (optional) and parameters." },
    { "posix_spawnp", qp_os_posix_spawn, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "posix_spawnp($module, path, argv, env, /, *, file_actions=(),\n"
      "             setpgroup=None, resetids=False, setsid=False,\n"
      "             setsigmask=(), setsigdef=(), scheduler=None)\n"
      "--\n"
      "\n"
      "Execute the program specified by path in a new process.\n"
      "\n"
      "  path\n"
      "    Path of executable file.\n"
      "  argv\n"
      "    Tuple or list of strings.\n"
      "  env\n"
      "    Dictionary of strings mapping to strings.\n"
      "  file_actions\n"
      "    A sequence of file action tuples.\n"
      "  setpgroup\n"
      "    The pgroup to use with the POSIX_SPAWN_SETPGROUP flag.\n"
      "  resetids\n"
      "    If the value is `True` the POSIX_SPAWN_RESETIDS will be activated.\n"
      "  setsid\n"
      "    If the value is `True` the POSIX_SPAWN_SETSID or POSIX_SPAWN_SETSID_NP\n"
      "    will be activated.\n"
      "  setsigmask\n"
      "    The sigmask to use with the POSIX_SPAWN_SETSIGMASK flag.\n"
      "  setsigdef\n"
      "    The sigmask to use with the POSIX_SPAWN_SETSIGDEF flag.\n"
      "  scheduler\n"
      "    A tuple with the scheduler policy (optional) and parameters." },
    { "sched_get_priority_max", qp_os_sched_get_priority, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "sched_get_priority_max($module, /, policy)\n"
      "--\n"
      "\n"
      "Get the maximum scheduling priority for policy." },
    { "sched_get_priority_min", qp_os_sched_get_priority, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "sched_get_priority_min($module, /, policy)\n"
      "--\n"
      "\n"
      "Get the minimum scheduling priority for policy." },
#ifdef QP_OS_HAVE_SCHED_SETSCHEDULER
    { "sched_getscheduler", qp_os_sched_getscheduler, QP_METH_O, 0,
      "sched_getscheduler($module, pid, /)\n"
      "--\n"
      "\n"
      "Get the scheduling policy for the process identified by pid.\n"
      "\n"
      "Passing 0 for pid returns the scheduling policy for the calling process." },
    { "sched_setscheduler", qp_os_sched_setscheduler, QP_METH_FASTCALL, 0,
      "sched_setscheduler($module, pid, policy, param, /)\n"
      "--\n"
      "\n"
      "Set the scheduling policy for the process identified by pid.\n"
      "\n"
      "If pid is 0, the calling process is changed.\n"
      "param is an instance of sched_param." },
#endif
#ifdef QP_OS_HAVE_SCHED_SETPARAM
    { "sched_getparam", qp_os_sched_getparam, QP_METH_O, 0,
      "sched_getparam($module, pid, /)\n"
      "--\n"
      "\n"
      "Returns scheduling parameters for the process identified by pid.\n"
      "\n"
      "If pid is 0, returns parameters for the calling process.\n"
      "Return value is an instance of sched_param." },
    { "sched_setparam", qp_os_sched_setparam, QP_METH_FASTCALL, 0,
      "sched_setparam($module, pid, param, /)\n"
      "--\n"
      "\n"
      "Set scheduling parameters for the process identified by pid.\n"
      "\n"
      "If pid is 0, sets parameters for the calling process.\n"
      "param should be an instance of sched_param." },
#endif
#ifdef QP_OS_HAVE_SCHED_RR_GET_INTERVAL
    { "sched_rr_get_interval", qp_os_sched_rr_get_interval, QP_METH_O, 0,
      "sched_rr_get_interval($module, pid, /)\n"
      "--\n"
      "\n"
      "Return the round-robin quantum for the process identified by pid, in seconds.\n"
      "\n"
      "Value returned is a float." },
#endif
    { "sched_yield", qp_os_sched_yield, QP_METH_NOARGS, 0,
      "sched_yield($module, /)\n"
      "--\n"
      "\n"
      "Voluntarily relinquish the CPU." },
#ifdef QP_OS_HAVE_SCHED_SETAFFINITY
    { "sched_setaffinity", qp_os_sched_setaffinity, QP_METH_FASTCALL, 0,
      "sched_setaffinity($module, pid, mask, /)\n"
      "--\n"
      "\n"
      "Set the CPU affinity of the process identified by pid to mask.\n"
      "\n"
      "mask should be an iterable of integers identifying CPUs." },
    { "sched_getaffinity", qp_os_sched_getaffinity, QP_METH_O, 0,
      "sched_getaffinity($module, pid, /)\n"
      "--\n"
      "\n"
      "Return the affinity of the process identified by pid (or the current process if zero).\n"
      "\n"
      "The affinity is returned as a set of CPU identifiers." },
#endif
    { "posix_openpt", qp_os_posix_openpt, QP_METH_O, 0,
      "posix_openpt($module, oflag, /)\n"
      "--\n"
      "\n"
      "Open and return a file descriptor for a master pseudo-terminal device.\n"
      "\n"
      "Performs a posix_openpt() C function call. The oflag argument is used to\n"
      "set file status flags and file access modes as specified in the manual\n"
      "page of posix_openpt() of your system." },
    { "grantpt", qp_os_grantpt, QP_METH_O, 0,
      "grantpt($module, fd, /)\n"
      "--\n"
      "\n"
      "Grant access to the slave pseudo-terminal device.\n"
      "\n"
      "  fd\n"
      "    File descriptor of a master pseudo-terminal device.\n"
      "\n"
      "Performs a grantpt() C function call." },
    { "unlockpt", qp_os_unlockpt, QP_METH_O, 0,
      "unlockpt($module, fd, /)\n"
      "--\n"
      "\n"
      "Unlock a pseudo-terminal master/slave pair.\n"
      "\n"
      "  fd\n"
      "    File descriptor of a master pseudo-terminal device.\n"
      "\n"
      "Performs an unlockpt() C function call." },
    { "ptsname", qp_os_ptsname, QP_METH_O, 0,
      "ptsname($module, fd, /)\n"
      "--\n"
      "\n"
      "Return the name of the slave pseudo-terminal device.\n"
      "\n"
      "  fd\n"
      "    File descriptor of a master pseudo-terminal device.\n"
      "\n"
      "If the ptsname_r() C function is available, it is called;\n"
      "otherwise, performs a ptsname() C function call." },
    { "openpty", qp_os_openpty, QP_METH_NOARGS, 0,
      "openpty($module, /)\n"
      "--\n"
      "\n"
      "Open a pseudo-terminal.\n"
      "\n"
      "Return a tuple of (master_fd, slave_fd) containing open file descriptors\n"
      "for both the master and slave ends." },
    { "login_tty", qp_os_login_tty, QP_METH_O, 0,
      "login_tty($module, fd, /)\n"
      "--\n"
      "\n"
      "Prepare the tty of which fd is a file descriptor for a new login session.\n"
      "\n"
      "Make the calling process a session leader; make the tty the\n"
      "controlling tty, the stdin, the stdout, and the stderr of the\n"
      "calling process; close fd." },
    { "forkpty", qp_os_forkpty, QP_METH_NOARGS, 0,
      "forkpty($module, /)\n"
      "--\n"
      "\n"
      "Fork a new process with a new pseudo-terminal as controlling tty.\n"
      "\n"
      "Returns a tuple of (pid, master_fd).\n"
      "Like fork(), return pid of 0 to the child process,\n"
      "and pid of child to the parent process.\n"
      "To both, return fd of newly opened pseudo-terminal.\n"
      "The master_fd is non-inheritable." },
    { "getgrouplist", qp_os_getgrouplist, QP_METH_FASTCALL, 0,
      "getgrouplist($module, user, group, /)\n"
      "--\n"
      "\n"
      "Returns a list of groups to which a user belongs.\n"
      "\n"
      "  user\n"
      "    username to lookup\n"
      "  group\n"
      "    base group id of the user" },
    { "initgroups", qp_os_initgroups, QP_METH_FASTCALL, 0,
      "initgroups($module, username, gid, /)\n"
      "--\n"
      "\n"
      "Initialize the group access list.\n"
      "\n"
      "Call the system initgroups() to initialize the group access list with\n"
      "all of the groups of which the specified username is a member, plus the\n"
      "specified group id." },
    { "setpgrp", qp_os_setpgrp, QP_METH_NOARGS, 0,
      "setpgrp($module, /)\n"
      "--\n"
      "\n"
      "Make the current process the leader of its process group." },
    { "setuid", qp_os_setid, QP_METH_O, 0,
      "setuid($module, uid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s user id." },
    { "seteuid", qp_os_setid, QP_METH_O, 1,
      "seteuid($module, euid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s effective user id." },
    { "setegid", qp_os_setid, QP_METH_O, 2,
      "setegid($module, egid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s effective group id." },
    { "setreuid", qp_os_setreid, QP_METH_FASTCALL, 0,
      "setreuid($module, ruid, euid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s real and effective user ids." },
    { "setregid", qp_os_setreid, QP_METH_FASTCALL, 1,
      "setregid($module, rgid, egid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s real and effective group ids." },
    { "setgid", qp_os_setid, QP_METH_O, 3,
      "setgid($module, gid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s group id." },
    { "setgroups", qp_os_setgroups, QP_METH_O, 0,
      "setgroups($module, groups, /)\n"
      "--\n"
      "\n"
      "Set the groups of the current process to list." },
#ifdef QP_OS_HAVE_SETRESUID
    { "setresuid", qp_os_setresid, QP_METH_FASTCALL, 0,
      "setresuid($module, ruid, euid, suid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s real, effective, and saved user ids." },
    { "setresgid", qp_os_setresid, QP_METH_FASTCALL, 1,
      "setresgid($module, rgid, egid, sgid, /)\n"
      "--\n"
      "\n"
      "Set the current process\'s real, effective, and saved group ids." },
    { "getresuid", qp_os_getresid, QP_METH_NOARGS, 0,
      "getresuid($module, /)\n"
      "--\n"
      "\n"
      "Return a tuple of the current process\'s real, effective, and saved user ids." },
    { "getresgid", qp_os_getresid, QP_METH_NOARGS, 1,
      "getresgid($module, /)\n"
      "--\n"
      "\n"
      "Return a tuple of the current process\'s real, effective, and saved group ids." },
#endif
    { "wait3", qp_os_wait3, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "wait3($module, /, options)\n"
      "--\n"
      "\n"
      "Wait for completion of a child process.\n"
      "\n"
      "Returns a tuple of information about the child process:\n"
      "  (pid, status, rusage)" },
    { "wait4", qp_os_wait3, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "wait4($module, /, pid, options)\n"
      "--\n"
      "\n"
      "Wait for completion of a specific child process.\n"
      "\n"
      "Returns a tuple of information about the child process:\n"
      "  (pid, status, rusage)" },
    { "waitid", qp_os_waitid, QP_METH_FASTCALL, 0,
      "waitid($module, idtype, id, options, /)\n"
      "--\n"
      "\n"
      "Returns the result of waiting for a process or processes.\n"
      "\n"
      "  idtype\n"
      "    Must be one of be P_PID, P_PGID or P_ALL.\n"
      "  id\n"
      "    The id to wait on.\n"
      "  options\n"
      "    Constructed from the ORing of one or more of WEXITED, WSTOPPED\n"
      "    or WCONTINUED and additionally may be ORed with WNOHANG or WNOWAIT.\n"
      "\n"
      "Returns either waitid_result or None if WNOHANG is specified and there\n"
      "are no children in a waitable state." },
#ifdef QP_OS_HAVE_PIDFD_OPEN
    { "pidfd_open", qp_os_pidfd_open, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "pidfd_open($module, /, pid, flags=0)\n"
      "--\n"
      "\n"
      "Return a file descriptor referring to the process *pid*.\n"
      "\n"
      "The descriptor can be used to perform process management without races\n"
      "and signals." },
#endif
#ifdef QP_OS_HAVE_SETNS
    { "setns", qp_os_setns, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "setns($module, /, fd, nstype=0)\n"
      "--\n"
      "\n"
      "Move the calling thread into different namespaces.\n"
      "\n"
      "  fd\n"
      "    A file descriptor to a namespace.\n"
      "  nstype\n"
      "    Type of namespace." },
#endif
#ifdef QP_OS_HAVE_UNSHARE
    { "unshare", qp_os_unshare, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "unshare($module, /, flags)\n"
      "--\n"
      "\n"
      "Disassociate parts of a process (or thread) execution context.\n"
      "\n"
      "  flags\n"
      "    Namespaces to be unshared." },
#endif
    { "tcgetpgrp", qp_os_tcgetpgrp, QP_METH_O, 0,
      "tcgetpgrp($module, fd, /)\n"
      "--\n"
      "\n"
      "Return the process group associated with the terminal specified by fd." },
    { "tcsetpgrp", qp_os_tcsetpgrp, QP_METH_FASTCALL, 0,
      "tcsetpgrp($module, fd, pgid, /)\n"
      "--\n"
      "\n"
      "Set the process group associated with the terminal specified by fd." },
    { "lockf", qp_os_lockf, QP_METH_FASTCALL, 0,
      "lockf($module, fd, command, length, /)\n"
      "--\n"
      "\n"
      "Apply, test or remove a POSIX lock on an open file descriptor.\n"
      "\n"
      "  fd\n"
      "    An open file descriptor.\n"
      "  command\n"
      "    One of F_LOCK, F_TLOCK, F_ULOCK or F_TEST.\n"
      "  length\n"
      "    The number of bytes to lock, starting at the current position." },
    { "preadv", qp_os_preadv, QP_METH_FASTCALL, 0,
      "preadv($module, fd, buffers, offset, flags=0, /)\n"
      "--\n"
      "\n"
      "Reads from a file descriptor into a number of mutable bytes-like objects.\n"
      "\n"
      "Combines the functionality of readv() and pread().  As readv(), it will\n"
      "transfer data into each buffer until it is full and then move on to the\n"
      "next buffer in the sequence to hold the rest of the data.  Its fourth\n"
      "argument, specifies the file offset at which the input operation is to\n"
      "be performed.  It will return the total number of bytes read (which can\n"
      "be less than the total capacity of all the objects).\n"
      "\n"
      "The flags argument contains a bitwise OR of zero or more of the\n"
      "following flags:\n"
      "\n"
      "- RWF_HIPRI\n"
      "- RWF_NOWAIT\n"
      "- RWF_DONTCACHE\n"
      "\n"
      "Using non-zero flags requires Linux 4.6 or newer." },
    { "pwritev", qp_os_preadv, QP_METH_FASTCALL, 1,
      "pwritev($module, fd, buffers, offset, flags=0, /)\n"
      "--\n"
      "\n"
      "Writes the contents of bytes-like objects to a file descriptor at a given offset.\n"
      "\n"
      "Combines the functionality of writev() and pwrite(). All buffers must be\n"
      "a sequence of bytes-like objects.  Buffers are processed in array order.\n"
      "Entire contents of first buffer is written before proceeding to second,\n"
      "and so on. The operating system may set a limit (sysconf() value\n"
      "SC_IOV_MAX) on the number of buffers that can be used.\n"
      "This function writes the contents of each object to the file descriptor\n"
      "and returns the total number of bytes written.\n"
      "\n"
      "The flags argument contains a bitwise OR of zero or more of the\n"
      "following flags:\n"
      "\n"
      "- RWF_DSYNC\n"
      "- RWF_SYNC\n"
      "- RWF_APPEND\n"
      "- RWF_DONTCACHE\n"
      "- RWF_ATOMIC\n"
      "\n"
      "Using non-zero flags requires Linux 4.7 or newer." },
#ifdef __APPLE__
    { "sendfile", qp_os_sendfile, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "sendfile($module, /, out_fd, in_fd, offset, count, headers=(),\n"
      "         trailers=(), flags=0)\n"
      "--\n"
      "\n"
      "Copy count bytes from file descriptor in_fd to file descriptor out_fd." },
#else
    { "sendfile", qp_os_sendfile, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "sendfile($module, /, out_fd, in_fd, offset, count)\n"
      "--\n"
      "\n"
      "Copy count bytes from file descriptor in_fd to file descriptor out_fd." },
#endif
#ifdef QP_OS_HAVE_FCOPYFILE
    { "_fcopyfile", qp_os__fcopyfile, QP_METH_FASTCALL, 0,
      "_fcopyfile($module, in_fd, out_fd, flags, /)\n"
      "--\n"
      "\n"
      "Efficiently copy content or metadata of 2 regular file descriptors (macOS)." },
#endif
#ifdef QP_OS_HAVE_COPY_FILE_RANGE
    { "copy_file_range", qp_os_copy_file_range, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "copy_file_range($module, /, src, dst, count, offset_src=None,\n"
      "                offset_dst=None)\n"
      "--\n"
      "\n"
      "Copy count bytes from one file descriptor to another.\n"
      "\n"
      "  src\n"
      "    Source file descriptor.\n"
      "  dst\n"
      "    Destination file descriptor.\n"
      "  count\n"
      "    Number of bytes to copy.\n"
      "  offset_src\n"
      "    Starting offset in src.\n"
      "  offset_dst\n"
      "    Starting offset in dst.\n"
      "\n"
      "If offset_src is None, then src is read from the current position;\n"
      "respectively for offset_dst." },
#endif
#ifdef QP_OS_HAVE_SPLICE
    { "splice", qp_os_copy_file_range, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "splice($module, /, src, dst, count, offset_src=None, offset_dst=None,\n"
      "       flags=0)\n"
      "--\n"
      "\n"
      "Transfer count bytes from one pipe to a descriptor or vice versa.\n"
      "\n"
      "  src\n"
      "    Source file descriptor.\n"
      "  dst\n"
      "    Destination file descriptor.\n"
      "  count\n"
      "    Number of bytes to copy.\n"
      "  offset_src\n"
      "    Starting offset in src.\n"
      "  offset_dst\n"
      "    Starting offset in dst.\n"
      "  flags\n"
      "    Flags to modify the semantics of the call.\n"
      "\n"
      "If offset_src is None, then src is read from the current position;\n"
      "respectively for offset_dst. The offset associated to the file\n"
      "descriptor that refers to a pipe must be None." },
#endif
    { "mkfifo", qp_os_mkfifo, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "mkfifo($module, /, path, mode=438, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Create a \"fifo\" (a POSIX named pipe).\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative to\n"
      "that directory.\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError." },
    { "mknod", qp_os_mkfifo, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "mknod($module, /, path, mode=384, device=0, *, dir_fd=None)\n"
      "--\n"
      "\n"
      "Create a node in the file system.\n"
      "\n"
      "Create a node in the file system (file, device special file or named\n"
      "pipe) at path.  mode specifies both the permissions to use and the\n"
      "type of node to be created, being combined (bitwise OR) with one of\n"
      "S_IFREG, S_IFCHR, S_IFBLK, and S_IFIFO.  If S_IFCHR or S_IFBLK is set\n"
      "on mode, device defines the newly created device special file (probably\n"
      "using os.makedev()).  Otherwise device is ignored.\n"
      "\n"
      "If dir_fd is not None, it should be a file descriptor open to\n"
      "a directory, and path should be relative; path will then be relative\n"
      "to that directory.\n"
      "dir_fd may not be implemented on your platform.  If it is unavailable,\n"
      "using it will raise a NotImplementedError." },
    { "major", qp_os_major, QP_METH_O, 0,
      "major($module, device, /)\n"
      "--\n"
      "\n"
      "Extracts a device major number from a raw device number." },
    { "minor", qp_os_major, QP_METH_O, 1,
      "minor($module, device, /)\n"
      "--\n"
      "\n"
      "Extracts a device minor number from a raw device number." },
    { "makedev", qp_os_makedev, QP_METH_FASTCALL, 0,
      "makedev($module, major, minor, /)\n"
      "--\n"
      "\n"
      "Composes a raw device number from the major and minor device numbers." },
#ifdef QP_OS_HAVE_POSIX_FALLOCATE
    { "posix_fallocate", qp_os_posix_fallocate, QP_METH_FASTCALL, 0,
      "posix_fallocate($module, fd, offset, length, /)\n"
      "--\n"
      "\n"
      "Ensure a file has allocated at least a particular number of bytes on disk.\n"
      "\n"
      "Ensure that the file specified by fd encompasses a range of bytes\n"
      "starting at offset bytes from the beginning and continuing for length\n"
      "bytes." },
#endif
#ifdef QP_OS_HAVE_POSIX_FADVISE
    { "posix_fadvise", qp_os_posix_fallocate, QP_METH_FASTCALL, 1,
      "posix_fadvise($module, fd, offset, length, advice, /)\n"
      "--\n"
      "\n"
      "Announce an intention to access data in a specific pattern.\n"
      "\n"
      "Announce an intention to access data in a specific pattern, thus\n"
      "allowing the kernel to make optimizations.\n"
      "The advice applies to the region of the file specified by fd starting at\n"
      "offset and continuing for length bytes.\n"
      "advice is one of POSIX_FADV_NORMAL, POSIX_FADV_SEQUENTIAL,\n"
      "POSIX_FADV_RANDOM, POSIX_FADV_NOREUSE, POSIX_FADV_WILLNEED, or\n"
      "POSIX_FADV_DONTNEED." },
#endif
#ifdef QP_OS_HAVE_CLEARENV
    { "_clearenv", qp_os__clearenv, QP_METH_NOARGS, 0,
      "_clearenv($module, /)\n"
      "--\n"
      "\n" },
#endif
#ifdef QP_OS_HAVE_MEMFD_CREATE
    { "memfd_create", qp_os_memfd_create, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "memfd_create($module, /, name, flags=MFD_CLOEXEC)\n"
      "--\n"
      "\n" },
#endif
#if defined(QP_OS_HAVE_EVENTFD) && defined(EFD_CLOEXEC)
    { "eventfd", qp_os_eventfd, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "eventfd($module, /, initval, flags=EFD_CLOEXEC)\n"
      "--\n"
      "\n"
      "Creates and returns an event notification file descriptor." },
    { "eventfd_read", qp_os_eventfd_read, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "eventfd_read($module, /, fd)\n"
      "--\n"
      "\n"
      "Read eventfd value" },
    { "eventfd_write", qp_os_eventfd_write, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "eventfd_write($module, /, fd, value)\n"
      "--\n"
      "\n"
      "Write eventfd value." },
#endif
#ifdef __linux__
    { "getrandom", qp_os_getrandom, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "getrandom($module, /, size, flags=0)\n"
      "--\n"
      "\n"
      "Obtain a series of random bytes." },
#endif
    { "_inputhook", qp_os__inputhook, QP_METH_NOARGS, 0,
      "_inputhook($module, /)\n"
      "--\n"
      "\n"
      "Calls PyOS_InputHook dropping the GIL first" },
    { "_is_inputhook_installed", qp_os__is_inputhook_installed, QP_METH_NOARGS, 0,
      "_is_inputhook_installed($module, /)\n"
      "--\n"
      "\n"
      "Checks if PyOS_InputHook is set" },
    { "_create_environ", qp_os__create_environ, QP_METH_NOARGS, 0,
      "_create_environ($module, /)\n"
      "--\n"
      "\n"
      "Create the environment dictionary." },
    { NULL }
};

/* convertenviron: the process environment into the bytes -> bytes dict
   'd' (first occurrence wins) */
static int qp_os_environ_fill(JSContext *ctx, JSValueConst d)
{
    char **e;
    for (e = QP_OS_ENVIRON; e && *e; e++) {
        const char *p = strchr(*e, '=');
        JSValue k, v, old;
        if (!p)
            continue;
        k = qp_bytes_new(ctx, *e, p - *e);
        if (JS_IsException(k))
            return -1;
        v = qp_bytes_new(ctx, p + 1, strlen(p + 1));
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, k);
            return -1;
        }
        old = qp_dict_setdefault(ctx, d, k, v);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        if (JS_IsException(old))
            return -1;
        JS_FreeValue(ctx, old);
    }
    return 0;
}

/* posix.environ: empty with QPConfig.detached (the base of a durable
   REPL session must not depend on the environment, see
   qp_os_drepl_environ()) */
static JSValue qp_os_convertenviron(JSContext *ctx)
{
    JSValue d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return d;
    if (!QP_STATE(ctx)->config.detached && qp_os_environ_fill(ctx, d) < 0) {
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    return d;
}

/* posix.environ of a durable REPL session, emptied in place before the
   session is saved (the session file never holds the values of the
   environment) and filled from the process environment otherwise
   ('fill'). os.environ wraps the same dict. Nothing happens if
   sys.modules['posix'].environ is not a dict. */
static void qp_os_drepl_environ(JSContext *ctx, BOOL fill)
{
    QPState *s = QP_STATE(ctx);
    JSValue m = QP_NULL, d = JS_EXCEPTION;
    JSAtom name;
    int r = 0;

    name = JS_NewAtom(ctx, "posix");
    if (name != JS_ATOM_NULL) {
        r = qp_dict_check(ctx, s->modules) ?
            qp_dict_get_item_atom(ctx, s->modules, name, &m) : 0;
        JS_FreeAtom(ctx, name);
    } else {
        r = -1;
    }
    if (r > 0) {
        name = JS_NewAtom(ctx, "environ");
        if (name != JS_ATOM_NULL) {
            d = qp_getattr(ctx, m, name);
            JS_FreeAtom(ctx, name);
        }
        JS_FreeValue(ctx, m);
        if (JS_IsException(d)) {
            r = -1;
        } else if (qp_dict_check(ctx, d)) {
            qp_dictcore_clear(ctx, &QP_DICT(d)->core);
            if (fill && qp_os_environ_fill(ctx, d) < 0)
                r = -1;
        }
        JS_FreeValue(ctx, d);
    }
    /* out of memory, or an attribute error */
    if (r < 0)
        qp_tw_clear_exc(ctx);
}

/* probes of the functions newer than the macOS deployment target */
static int qp_os_probe_mkfifoat(void)
{
    if (QP_OS_HAVE_MKFIFOAT_RUNTIME)
        return 1;
    return 0;
}

static int qp_os_probe_mknodat(void)
{
    if (QP_OS_HAVE_MKNODAT_RUNTIME)
        return 1;
    return 0;
}

static int qp_os_probe_ptsname_r(void)
{
    if (QP_OS_HAVE_PTSNAME_R_RUNTIME)
        return 1;
    return 0;
}

/* have_functions (CPython order); probe NULL: always present */
static const struct {
    const char *name;
    int (*probe)(void);
} qp_os_have_functions[] = {
#if defined(QP_OS_HAVE_EVENTFD) && defined(EFD_CLOEXEC)
    { "HAVE_EVENTFD", NULL },
#endif
    { "HAVE_FACCESSAT", NULL }, { "HAVE_FCHDIR", NULL }, { "HAVE_FCHMOD", NULL },
    { "HAVE_FCHMODAT", NULL }, { "HAVE_FCHOWN", NULL }, { "HAVE_FCHOWNAT", NULL },
#ifdef __linux__
    { "HAVE_FEXECVE", NULL },
#endif
    { "HAVE_FDOPENDIR", NULL }, { "HAVE_FPATHCONF", NULL }, { "HAVE_FSTATAT", NULL },
    { "HAVE_FSTATVFS", NULL }, { "HAVE_FTRUNCATE", NULL }, { "HAVE_FUTIMENS", NULL },
    { "HAVE_FUTIMES", NULL },
#ifdef QP_OS_HAVE_FUTIMESAT
    { "HAVE_FUTIMESAT", NULL },
#endif
    { "HAVE_LINKAT", NULL },
#ifdef QP_OS_HAVE_LCHFLAGS
    { "HAVE_LCHFLAGS", NULL },
#endif
#ifdef __APPLE__
    { "HAVE_LCHMOD", NULL },
#endif
    { "HAVE_LCHOWN", NULL }, { "HAVE_LSTAT", NULL }, { "HAVE_LUTIMES", NULL },
#ifdef QP_OS_HAVE_MEMFD_CREATE
    { "HAVE_MEMFD_CREATE", NULL },
#endif
    { "HAVE_MKDIRAT", NULL }, { "HAVE_MKFIFOAT", qp_os_probe_mkfifoat },
    { "HAVE_MKNODAT", qp_os_probe_mknodat }, { "HAVE_OPENAT", NULL },
    { "HAVE_READLINKAT", NULL }, { "HAVE_RENAMEAT", NULL }, { "HAVE_SYMLINKAT", NULL },
    { "HAVE_UNLINKAT", NULL }, { "HAVE_UTIMENSAT", NULL },
    { "HAVE_PTSNAME_R", qp_os_probe_ptsname_r },
    { NULL, NULL }
};

static int qp_os_add_type(JSContext *ctx, JSValueConst module, const char *prop,
                          const char *export_name, JSValue type)
{
    if (JS_IsException(type))
        return -1;
    if (export_name && qp_os_set(ctx, module, export_name, JS_DupValue(ctx, type)) < 0) {
        JS_FreeValue(ctx, type);
        return -1;
    }
    return JS_DefinePropertyValueStr(ctx, module, prop, type, 0) < 0 ? -1 : 0;
}

static const QPMethodDef qp_os_statresult_new_def = {
    "__new__", qp_os_statresult_new, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
    "Create and return a new object.  See help(type) for accurate signature."
};

static int qp_posix_exec(JSContext *ctx, JSValueConst module)
{
    JSValue v, st;
    int i;
#define QP_OS_INT(X) \
    do { if (qp_os_set(ctx, module, #X, qp_long_from_int64(ctx, (int64_t)(X))) < 0) return -1; } while (0)
#define QP_OS_INTC(N, X) \
    do { if (qp_os_set(ctx, module, N, qp_long_from_int64(ctx, (int64_t)(X))) < 0) return -1; } while (0)
#ifdef F_OK
    QP_OS_INT(F_OK);
#endif
#ifdef R_OK
    QP_OS_INT(R_OK);
#endif
#ifdef W_OK
    QP_OS_INT(W_OK);
#endif
#ifdef X_OK
    QP_OS_INT(X_OK);
#endif
#ifdef NGROUPS_MAX
    QP_OS_INT(NGROUPS_MAX);
#endif
#ifdef TMP_MAX
    QP_OS_INT(TMP_MAX);
#endif
#ifdef WCONTINUED
    QP_OS_INT(WCONTINUED);
#endif
#ifdef WNOHANG
    QP_OS_INT(WNOHANG);
#endif
#ifdef WUNTRACED
    QP_OS_INT(WUNTRACED);
#endif
#ifdef O_RDONLY
    QP_OS_INT(O_RDONLY);
#endif
#ifdef O_WRONLY
    QP_OS_INT(O_WRONLY);
#endif
#ifdef O_RDWR
    QP_OS_INT(O_RDWR);
#endif
#ifdef O_NDELAY
    QP_OS_INT(O_NDELAY);
#endif
#ifdef O_NONBLOCK
    QP_OS_INT(O_NONBLOCK);
#endif
#ifdef O_APPEND
    QP_OS_INT(O_APPEND);
#endif
#ifdef O_DSYNC
    QP_OS_INT(O_DSYNC);
#endif
#ifdef O_RSYNC
    QP_OS_INT(O_RSYNC);
#endif
#ifdef O_SYNC
    QP_OS_INT(O_SYNC);
#endif
#ifdef O_NOCTTY
    QP_OS_INT(O_NOCTTY);
#endif
#ifdef O_CREAT
    QP_OS_INT(O_CREAT);
#endif
#ifdef O_EXCL
    QP_OS_INT(O_EXCL);
#endif
#ifdef O_TRUNC
    QP_OS_INT(O_TRUNC);
#endif
#ifdef O_BINARY
    QP_OS_INT(O_BINARY);
#endif
#ifdef O_TEXT
    QP_OS_INT(O_TEXT);
#endif
#ifdef O_XATTR
    QP_OS_INT(O_XATTR);
#endif
#ifdef O_LARGEFILE
    QP_OS_INT(O_LARGEFILE);
#endif
#ifndef __GNU__
#ifdef O_SHLOCK
    QP_OS_INT(O_SHLOCK);
#endif
#ifdef O_EXLOCK
    QP_OS_INT(O_EXLOCK);
#endif
#endif
#ifdef O_EXEC
    QP_OS_INT(O_EXEC);
#endif
#ifdef O_SEARCH
    QP_OS_INT(O_SEARCH);
#endif
#ifdef O_PATH
    QP_OS_INT(O_PATH);
#endif
#ifdef O_TTY_INIT
    QP_OS_INT(O_TTY_INIT);
#endif
#ifdef O_TMPFILE
    QP_OS_INT(O_TMPFILE);
#endif
#ifdef O_CLOEXEC
    QP_OS_INT(O_CLOEXEC);
#endif
#ifdef O_ACCMODE
    QP_OS_INT(O_ACCMODE);
#endif
#ifdef O_EVTONLY
    QP_OS_INT(O_EVTONLY);
#endif
#ifdef O_FSYNC
    QP_OS_INT(O_FSYNC);
#endif
#ifdef O_SYMLINK
    QP_OS_INT(O_SYMLINK);
#endif
#ifdef SEEK_HOLE
    QP_OS_INT(SEEK_HOLE);
#endif
#ifdef SEEK_DATA
    QP_OS_INT(SEEK_DATA);
#endif
#ifdef O_NOINHERIT
    QP_OS_INT(O_NOINHERIT);
#endif
#ifdef _O_SHORT_LIVED
    QP_OS_INTC("O_SHORT_LIVED", _O_SHORT_LIVED);
#endif
#ifdef O_TEMPORARY
    QP_OS_INT(O_TEMPORARY);
#endif
#ifdef O_RANDOM
    QP_OS_INT(O_RANDOM);
#endif
#ifdef O_SEQUENTIAL
    QP_OS_INT(O_SEQUENTIAL);
#endif
#ifdef O_ASYNC
    QP_OS_INT(O_ASYNC);
#endif
#ifdef O_DIRECT
    QP_OS_INT(O_DIRECT);
#endif
#ifdef O_DIRECTORY
    QP_OS_INT(O_DIRECTORY);
#endif
#ifdef O_NOFOLLOW
    QP_OS_INT(O_NOFOLLOW);
#endif
#ifdef O_NOFOLLOW_ANY
    QP_OS_INT(O_NOFOLLOW_ANY);
#endif
#ifdef O_NOLINKS
    QP_OS_INT(O_NOLINKS);
#endif
#ifdef O_NOATIME
    QP_OS_INT(O_NOATIME);
#endif
#ifdef EX_OK
    QP_OS_INT(EX_OK);
#endif /* EX_OK */
#ifdef EX_USAGE
    QP_OS_INT(EX_USAGE);
#endif /* EX_USAGE */
#ifdef EX_DATAERR
    QP_OS_INT(EX_DATAERR);
#endif /* EX_DATAERR */
#ifdef EX_NOINPUT
    QP_OS_INT(EX_NOINPUT);
#endif /* EX_NOINPUT */
#ifdef EX_NOUSER
    QP_OS_INT(EX_NOUSER);
#endif /* EX_NOUSER */
#ifdef EX_NOHOST
    QP_OS_INT(EX_NOHOST);
#endif /* EX_NOHOST */
#ifdef EX_UNAVAILABLE
    QP_OS_INT(EX_UNAVAILABLE);
#endif /* EX_UNAVAILABLE */
#ifdef EX_SOFTWARE
    QP_OS_INT(EX_SOFTWARE);
#endif /* EX_SOFTWARE */
#ifdef EX_OSERR
    QP_OS_INT(EX_OSERR);
#endif /* EX_OSERR */
#ifdef EX_OSFILE
    QP_OS_INT(EX_OSFILE);
#endif /* EX_OSFILE */
#ifdef EX_CANTCREAT
    QP_OS_INT(EX_CANTCREAT);
#endif /* EX_CANTCREAT */
#ifdef EX_IOERR
    QP_OS_INT(EX_IOERR);
#endif /* EX_IOERR */
#ifdef EX_TEMPFAIL
    QP_OS_INT(EX_TEMPFAIL);
#endif /* EX_TEMPFAIL */
#ifdef EX_PROTOCOL
    QP_OS_INT(EX_PROTOCOL);
#endif /* EX_PROTOCOL */
#ifdef EX_NOPERM
    QP_OS_INT(EX_NOPERM);
#endif /* EX_NOPERM */
#ifdef EX_CONFIG
    QP_OS_INT(EX_CONFIG);
#endif /* EX_CONFIG */
#ifdef EX_NOTFOUND
    QP_OS_INT(EX_NOTFOUND);
#endif /* EX_NOTFOUND */
#ifdef ST_RDONLY
    QP_OS_INT(ST_RDONLY);
#endif /* ST_RDONLY */
#ifdef ST_NOSUID
    QP_OS_INT(ST_NOSUID);
#endif /* ST_NOSUID */
#ifdef ST_NODEV
    QP_OS_INT(ST_NODEV);
#endif /* ST_NODEV */
#ifdef ST_NOEXEC
    QP_OS_INT(ST_NOEXEC);
#endif /* ST_NOEXEC */
#ifdef ST_SYNCHRONOUS
    QP_OS_INT(ST_SYNCHRONOUS);
#endif /* ST_SYNCHRONOUS */
#ifdef ST_MANDLOCK
    QP_OS_INT(ST_MANDLOCK);
#endif /* ST_MANDLOCK */
#ifdef ST_WRITE
    QP_OS_INT(ST_WRITE);
#endif /* ST_WRITE */
#ifdef ST_APPEND
    QP_OS_INT(ST_APPEND);
#endif /* ST_APPEND */
#ifdef ST_NOATIME
    QP_OS_INT(ST_NOATIME);
#endif /* ST_NOATIME */
#ifdef ST_NODIRATIME
    QP_OS_INT(ST_NODIRATIME);
#endif /* ST_NODIRATIME */
#ifdef ST_RELATIME
    QP_OS_INT(ST_RELATIME);
#endif /* ST_RELATIME */
#ifdef WEXITED
    QP_OS_INT(WEXITED);
#endif
#ifdef WNOWAIT
    QP_OS_INT(WNOWAIT);
#endif
#ifdef WSTOPPED
    QP_OS_INT(WSTOPPED);
#endif
#ifdef PRIO_PROCESS
    QP_OS_INT(PRIO_PROCESS);
#endif
#ifdef PRIO_PGRP
    QP_OS_INT(PRIO_PGRP);
#endif
#ifdef PRIO_USER
    QP_OS_INT(PRIO_USER);
#endif
#ifdef PRIO_DARWIN_THREAD
    QP_OS_INT(PRIO_DARWIN_THREAD);
#endif
#ifdef PRIO_DARWIN_PROCESS
    QP_OS_INT(PRIO_DARWIN_PROCESS);
#endif
#ifdef PRIO_DARWIN_BG
    QP_OS_INT(PRIO_DARWIN_BG);
#endif
#ifdef PRIO_DARWIN_NONUI
    QP_OS_INT(PRIO_DARWIN_NONUI);
#endif
    /* constants for posix_fadvise */
#ifdef POSIX_FADV_NORMAL
    QP_OS_INT(POSIX_FADV_NORMAL);
#endif
#ifdef POSIX_FADV_SEQUENTIAL
    QP_OS_INT(POSIX_FADV_SEQUENTIAL);
#endif
#ifdef POSIX_FADV_RANDOM
    QP_OS_INT(POSIX_FADV_RANDOM);
#endif
#ifdef POSIX_FADV_NOREUSE
    QP_OS_INT(POSIX_FADV_NOREUSE);
#endif
#ifdef POSIX_FADV_WILLNEED
    QP_OS_INT(POSIX_FADV_WILLNEED);
#endif
#ifdef POSIX_FADV_DONTNEED
    QP_OS_INT(POSIX_FADV_DONTNEED);
#endif
    /* constants for waitid (idtype_t enumerators) */
    QP_OS_INT(P_PID);
    QP_OS_INT(P_PGID);
    QP_OS_INT(P_ALL);
#ifdef P_PIDFD
    QP_OS_INT(P_PIDFD);
#endif
#ifdef PIDFD_NONBLOCK
    QP_OS_INT(PIDFD_NONBLOCK);
#endif
#ifdef CLD_EXITED
    QP_OS_INT(CLD_EXITED);
#endif
#ifdef CLD_KILLED
    QP_OS_INT(CLD_KILLED);
#endif
#ifdef CLD_DUMPED
    QP_OS_INT(CLD_DUMPED);
#endif
#ifdef CLD_TRAPPED
    QP_OS_INT(CLD_TRAPPED);
#endif
#ifdef CLD_STOPPED
    QP_OS_INT(CLD_STOPPED);
#endif
#ifdef CLD_CONTINUED
    QP_OS_INT(CLD_CONTINUED);
#endif
    /* constants for lockf */
#ifdef F_LOCK
    QP_OS_INT(F_LOCK);
#endif
#ifdef F_TLOCK
    QP_OS_INT(F_TLOCK);
#endif
#ifdef F_ULOCK
    QP_OS_INT(F_ULOCK);
#endif
#ifdef F_TEST
    QP_OS_INT(F_TEST);
#endif
#ifdef QP_OS_HAVE_PREADV2
#ifdef RWF_DSYNC
    QP_OS_INT(RWF_DSYNC);
#endif
#ifdef RWF_HIPRI
    QP_OS_INT(RWF_HIPRI);
#endif
#ifdef RWF_SYNC
    QP_OS_INT(RWF_SYNC);
#endif
#ifdef RWF_NOWAIT
    QP_OS_INT(RWF_NOWAIT);
#endif
#ifdef RWF_DONTCACHE
    QP_OS_INT(RWF_DONTCACHE);
#endif
#ifdef RWF_ATOMIC
    QP_OS_INT(RWF_ATOMIC);
#endif
#ifdef RWF_APPEND
    QP_OS_INT(RWF_APPEND);
#endif
#endif /* QP_OS_HAVE_PREADV2 */
    /* constants for splice */
#ifdef QP_OS_HAVE_SPLICE
    QP_OS_INT(SPLICE_F_MOVE);
    QP_OS_INT(SPLICE_F_NONBLOCK);
    QP_OS_INT(SPLICE_F_MORE);
#endif
    /* constants for posix_spawn */
    QP_OS_INTC("POSIX_SPAWN_OPEN", QP_OS_SPAWN_OPEN);
    QP_OS_INTC("POSIX_SPAWN_CLOSE", QP_OS_SPAWN_CLOSE);
    QP_OS_INTC("POSIX_SPAWN_DUP2", QP_OS_SPAWN_DUP2);
#ifdef QP_OS_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
    QP_OS_INTC("POSIX_SPAWN_CLOSEFROM", QP_OS_SPAWN_CLOSEFROM);
#endif
#ifdef SCHED_OTHER
    QP_OS_INT(SCHED_OTHER);
#endif
#ifdef SCHED_DEADLINE
    QP_OS_INT(SCHED_DEADLINE);
#endif
#ifdef SCHED_FIFO
    QP_OS_INT(SCHED_FIFO);
#endif
#ifdef SCHED_NORMAL
    QP_OS_INT(SCHED_NORMAL);
#endif
#ifdef SCHED_RR
    QP_OS_INT(SCHED_RR);
#endif
#ifdef SCHED_SPORADIC
    QP_OS_INT(SCHED_SPORADIC);
#endif
#ifdef SCHED_BATCH
    QP_OS_INT(SCHED_BATCH);
#endif
#ifdef SCHED_IDLE
    QP_OS_INT(SCHED_IDLE);
#endif
#ifdef SCHED_RESET_ON_FORK
    QP_OS_INT(SCHED_RESET_ON_FORK);
#endif
#ifdef SCHED_SYS
    QP_OS_INT(SCHED_SYS);
#endif
#ifdef SCHED_IA
    QP_OS_INT(SCHED_IA);
#endif
#ifdef SCHED_FSS
    QP_OS_INT(SCHED_FSS);
#endif
#ifdef SCHED_FX
    QP_OS_INTC("SCHED_FX", SCHED_FSS);
#endif
    /* constants for namespaces */
#if defined(QP_OS_HAVE_SETNS) || defined(QP_OS_HAVE_UNSHARE)
#ifdef CLONE_FS
    QP_OS_INT(CLONE_FS);
#endif
#ifdef CLONE_FILES
    QP_OS_INT(CLONE_FILES);
#endif
#ifdef CLONE_NEWNS
    QP_OS_INT(CLONE_NEWNS);
#endif
#ifdef CLONE_NEWCGROUP
    QP_OS_INT(CLONE_NEWCGROUP);
#endif
#ifdef CLONE_NEWUTS
    QP_OS_INT(CLONE_NEWUTS);
#endif
#ifdef CLONE_NEWIPC
    QP_OS_INT(CLONE_NEWIPC);
#endif
#ifdef CLONE_NEWUSER
    QP_OS_INT(CLONE_NEWUSER);
#endif
#ifdef CLONE_NEWPID
    QP_OS_INT(CLONE_NEWPID);
#endif
#ifdef CLONE_NEWNET
    QP_OS_INT(CLONE_NEWNET);
#endif
#ifdef CLONE_NEWTIME
    QP_OS_INT(CLONE_NEWTIME);
#endif
#ifdef CLONE_SYSVSEM
    QP_OS_INT(CLONE_SYSVSEM);
#endif
#ifdef CLONE_THREAD
    QP_OS_INT(CLONE_THREAD);
#endif
#ifdef CLONE_SIGHAND
    QP_OS_INT(CLONE_SIGHAND);
#endif
#ifdef CLONE_VM
    QP_OS_INT(CLONE_VM);
#endif
#endif
#ifdef RTLD_LAZY
    QP_OS_INT(RTLD_LAZY);
#endif
#ifdef RTLD_NOW
    QP_OS_INT(RTLD_NOW);
#endif
#ifdef RTLD_GLOBAL
    QP_OS_INT(RTLD_GLOBAL);
#endif
#ifdef RTLD_LOCAL
    QP_OS_INT(RTLD_LOCAL);
#endif
#ifdef RTLD_NODELETE
    QP_OS_INT(RTLD_NODELETE);
#endif
#ifdef RTLD_NOLOAD
    QP_OS_INT(RTLD_NOLOAD);
#endif
#ifdef RTLD_DEEPBIND
    QP_OS_INT(RTLD_DEEPBIND);
#endif
#ifdef RTLD_MEMBER
    QP_OS_INT(RTLD_MEMBER);
#endif
#ifdef __linux__
    QP_OS_INT(GRND_RANDOM);
    QP_OS_INT(GRND_NONBLOCK);
#endif
#ifdef QP_OS_HAVE_MEMFD_CREATE
    QP_OS_INT(MFD_CLOEXEC);
    QP_OS_INT(MFD_ALLOW_SEALING);
#ifdef MFD_HUGETLB
    QP_OS_INT(MFD_HUGETLB);
#endif
#ifdef MFD_HUGE_SHIFT
    QP_OS_INT(MFD_HUGE_SHIFT);
#endif
#ifdef MFD_HUGE_MASK
    QP_OS_INT(MFD_HUGE_MASK);
#endif
#ifdef MFD_HUGE_64KB
    QP_OS_INT(MFD_HUGE_64KB);
#endif
#ifdef MFD_HUGE_512KB
    QP_OS_INT(MFD_HUGE_512KB);
#endif
#ifdef MFD_HUGE_1MB
    QP_OS_INT(MFD_HUGE_1MB);
#endif
#ifdef MFD_HUGE_2MB
    QP_OS_INT(MFD_HUGE_2MB);
#endif
#ifdef MFD_HUGE_8MB
    QP_OS_INT(MFD_HUGE_8MB);
#endif
#ifdef MFD_HUGE_16MB
    QP_OS_INT(MFD_HUGE_16MB);
#endif
#ifdef MFD_HUGE_32MB
    QP_OS_INT(MFD_HUGE_32MB);
#endif
#ifdef MFD_HUGE_256MB
    QP_OS_INT(MFD_HUGE_256MB);
#endif
#ifdef MFD_HUGE_512MB
    QP_OS_INT(MFD_HUGE_512MB);
#endif
#ifdef MFD_HUGE_1GB
    QP_OS_INT(MFD_HUGE_1GB);
#endif
#ifdef MFD_HUGE_2GB
    QP_OS_INT(MFD_HUGE_2GB);
#endif
#ifdef MFD_HUGE_16GB
    QP_OS_INT(MFD_HUGE_16GB);
#endif
#endif /* QP_OS_HAVE_MEMFD_CREATE */
#if defined(QP_OS_HAVE_EVENTFD) && defined(EFD_CLOEXEC)
    QP_OS_INT(EFD_CLOEXEC);
#ifdef EFD_NONBLOCK
    QP_OS_INT(EFD_NONBLOCK);
#endif
#ifdef EFD_SEMAPHORE
    QP_OS_INT(EFD_SEMAPHORE);
#endif
#endif
#ifdef NODEV
    if (qp_os_set(ctx, module, "NODEV", qp_os_long_from_dev(ctx, NODEV)) < 0)
        return -1;
#endif
#ifdef AT_NO_AUTOMOUNT
    QP_OS_INT(AT_NO_AUTOMOUNT);
#endif
#ifdef QP_OS_HAVE_FCOPYFILE
    QP_OS_INTC("_COPYFILE_DATA", COPYFILE_DATA);
    QP_OS_INTC("_COPYFILE_STAT", COPYFILE_STAT);
    QP_OS_INTC("_COPYFILE_ACL", COPYFILE_ACL);
    QP_OS_INTC("_COPYFILE_XATTR", COPYFILE_XATTR);
#endif
#undef QP_OS_INT
#undef QP_OS_INTC

    if (qp_os_conftable(ctx, module, "pathconf_names", qp_os_pathconf_names) < 0 ||
        qp_os_conftable(ctx, module, "confstr_names", qp_os_confstr_names) < 0 ||
        qp_os_conftable(ctx, module, "sysconf_names", qp_os_sysconf_names) < 0)
        return -1;
    if (qp_os_set(ctx, module, "environ", qp_os_convertenviron(ctx)) < 0)
        return -1;
    if (qp_os_set(ctx, module, "error", JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, OSError)))) < 0)
        return -1;

    /* stat_result with statresult_new */
    st = qp_structseq_new_type(ctx, &qp_os_stat_result_desc);
    if (JS_IsException(st))
        return -1;
    {
        JSValue nw = qp_cfunction_new(ctx, &qp_os_statresult_new_def, st, JS_UNDEFINED), sm;
        JSAtom at;
        int r;
        if (JS_IsException(nw)) {
            JS_FreeValue(ctx, st);
            return -1;
        }
        sm = qp_staticmethod_new(ctx, nw);
        JS_FreeValue(ctx, nw);
        if (JS_IsException(sm)) {
            JS_FreeValue(ctx, st);
            return -1;
        }
        /* structseq types are created immutable; CPython's posixmodule
           patches tp_new before the type is published. */
        at = JS_NewAtom(ctx, "__new__");
        qp_type_from_value(st)->flags &= ~QP_TPFLAGS_IMMUTABLETYPE;
        r = qp_setattr(ctx, st, at, sm);
        qp_type_from_value(st)->flags |= QP_TPFLAGS_IMMUTABLETYPE;
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, sm);
        if (r < 0) {
            JS_FreeValue(ctx, st);
            return -1;
        }
    }
    if (qp_os_add_type(ctx, module, QP_OS_PROP_STAT, "stat_result", st) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_STATVFS, "statvfs_result",
                       qp_structseq_new_type(ctx, &qp_os_statvfs_result_desc)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_UNAME, "uname_result",
                       qp_structseq_new_type(ctx, &qp_os_uname_result_desc)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_TERMSIZE, "terminal_size",
                       qp_structseq_new_type(ctx, &qp_os_terminal_size_desc)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_TIMES, "times_result",
                       qp_structseq_new_type(ctx, &qp_os_times_result_desc)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_WAITID, "waitid_result",
                       qp_structseq_new_type(ctx, &qp_os_waitid_result_desc)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_DIRENTRY, "DirEntry",
                       qp_type_from_spec(ctx, &qp_de_spec)) < 0 ||
        qp_os_add_type(ctx, module, QP_OS_PROP_SCANDIR, NULL,
                       qp_type_from_spec(ctx, &qp_si_spec)) < 0)
        return -1;

#ifdef QP_OS_HAVE_SCHED_PARAM
    /* sched_param: tp_new = os_sched_param, plus __reduce__ */
    {
        JSValue sp = qp_structseq_new_type(ctx, &qp_os_sched_param_desc), nw, sm, red;
        QPType *spt;
        JSAtom at;
        int r;
        if (JS_IsException(sp))
            return -1;
        spt = qp_type_from_value(sp);
        nw = qp_cfunction_new(ctx, &qp_os_sched_param_new_def, sp, JS_UNDEFINED);
        sm = JS_IsException(nw) ? JS_EXCEPTION : qp_staticmethod_new(ctx, nw);
        JS_FreeValue(ctx, nw);
        red = JS_IsException(sm) ? JS_EXCEPTION : qp_descr_new_method(ctx, spt, &qp_os_sched_param_reduce_def);
        if (JS_IsException(red)) {
            JS_FreeValue(ctx, sm);
            JS_FreeValue(ctx, sp);
            return -1;
        }
        spt->flags &= ~QP_TPFLAGS_IMMUTABLETYPE;
        at = JS_NewAtom(ctx, "__new__");
        r = qp_setattr(ctx, sp, at, sm);
        JS_FreeAtom(ctx, at);
        if (r == 0) {
            at = JS_NewAtom(ctx, "__reduce__");
            r = qp_setattr(ctx, sp, at, red);
            JS_FreeAtom(ctx, at);
        }
        spt->flags |= QP_TPFLAGS_IMMUTABLETYPE;
        JS_FreeValue(ctx, sm);
        JS_FreeValue(ctx, red);
        if (r < 0) {
            JS_FreeValue(ctx, sp);
            return -1;
        }
        if (qp_os_add_type(ctx, module, QP_OS_PROP_SCHEDPARAM, "sched_param", sp) < 0)
            return -1;
    }
#endif
#if defined(QP_OS_HAVE_PIPE2) && defined(__APPLE__)
    if (QP_OS_HAVE_PIPE2_RUNTIME) {
        /* `__builtin_available` doesn't allow `!` */
    } else {
        JSAtom at = JS_NewAtom(ctx, "pipe2");
        int r = qp_dict_del_item_atom(ctx, qp_module_get_dict(module), at);
        JS_FreeAtom(ctx, at);
        if (r < 0)
            return -1;
    }
#endif
    if (JS_DefinePropertyValueStr(ctx, module, QP_OS_PROP_FORK_BEFORE, qp_list_new(ctx, 0), 0) < 0 ||
        JS_DefinePropertyValueStr(ctx, module, QP_OS_PROP_FORK_CHILD, qp_list_new(ctx, 0), 0) < 0 ||
        JS_DefinePropertyValueStr(ctx, module, QP_OS_PROP_FORK_PARENT, qp_list_new(ctx, 0), 0) < 0)
        return -1;

    v = qp_list_new(ctx, 0);
    if (JS_IsException(v))
        return -1;
    for (i = 0; qp_os_have_functions[i].name; i++) {
        JSValue s;
        if (qp_os_have_functions[i].probe && !qp_os_have_functions[i].probe())
            continue;
        s = qp_str_from_cstr(ctx, qp_os_have_functions[i].name);
        if (JS_IsException(s) || qp_list_append(ctx, v, s) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, s);
    }
    return qp_os_set(ctx, module, "_have_functions", v);
}

static const QPModuleDef qp_moddef_posix = {
    "posix",
    "This module provides access to operating system functionality that is\n\
standardized by the C Standard and the POSIX standard (a thinly\n\
disguised Unix interface).  Refer to the library manual and\n\
corresponding Unix manual entries for more information on calls.",
    qp_posix_methods,
    qp_posix_exec,
};
