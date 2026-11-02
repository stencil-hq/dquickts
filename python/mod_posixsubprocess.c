/*
 * _posixsubprocess module (port of CPython Modules/_posixsubprocess.c,
 * PSF-2.0): fork_exec() for subprocess.Popen.
 *
 * Differences forced by the runtime: the child is always created with
 * fork() (CPython may use vfork() on Linux when no preexec_fn/uid/gid/
 * groups are requested; the observable behaviour is the same), and there is
 * no cyclic GC to disable around preexec_fn (QuickJS reference counting).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <grp.h>
#include <signal.h>
#include <limits.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#if defined(__FreeBSD__) || (defined(__APPLE__) && defined(__MACH__)) || defined(__DragonFly__)
# define QP_PSP_FD_DIR "/dev/fd"
#else
# define QP_PSP_FD_DIR "/proc/self/fd"
#endif

#ifdef NGROUPS_MAX
#define QP_PSP_MAX_GROUPS NGROUPS_MAX
#else
#define QP_PSP_MAX_GROUPS 64
#endif

#define QP_PSP_POSIX_CALL(call)   do { if ((call) == -1) goto error; } while (0)

/* posixmodule.c helpers (same translation unit) */
static JSValue qp_os_fsconvert(JSContext *ctx, JSValueConst o);
static int qp_os_id_conv(JSContext *ctx, JSValueConst o, uint32_t *out, const char *what);
static void qp_os_before_fork(JSContext *ctx);
static void qp_os_after_fork(JSContext *ctx, int child);

/* Convert ASCII to a positive int, no libc call. no overflow. -1 on error. */
static int qp_psp_pos_int_from_ascii(const char *name)
{
    int num = 0;
    while (*name >= '0' && *name <= '9') {
        num = num * 10 + (*name - '0');
        ++name;
    }
    if (*name)
        return -1;  /* Non digit found, not a number. */
    return num;
}

/* Returns 1 if there is a problem with fd_sequence, 0 otherwise. */
static int qp_psp_sanity_check_fd_sequence(JSContext *ctx, JSValueConst fd_sequence)
{
    Py_ssize_t i;
    int64_t prev_fd = -1;
    for (i = 0; i < QP_TUPLE_SIZE(fd_sequence); ++i) {
        JSValueConst py_fd = QP_TUPLE_ITEMS(fd_sequence)[i];
        int64_t iter_fd;
        int overflow = 0;
        if (!qp_long_check(ctx, py_fd))
            return 1;
        if (qp_long_as_int64_overflow(py_fd, &iter_fd, &overflow) < 0 || overflow)
            return 1;
        if (iter_fd < 0 || iter_fd <= prev_fd || iter_fd > INT_MAX) {
            /* Negative, overflow, unsorted, too big for a fd. */
            return 1;
        }
        prev_fd = iter_fd;
    }
    return 0;
}

/* Is fd found in the sorted Python Sequence? */
static int qp_psp_is_fd_in_sorted_fd_sequence(int fd, int *fd_sequence, Py_ssize_t fd_sequence_len)
{
    /* Binary search. */
    Py_ssize_t search_min = 0;
    Py_ssize_t search_max = fd_sequence_len - 1;
    if (search_max < 0)
        return 0;
    do {
        long middle = (search_min + search_max) / 2;
        long middle_fd = fd_sequence[middle];
        if (fd == middle_fd)
            return 1;
        if (fd > middle_fd)
            search_min = middle + 1;
        else
            search_max = middle - 1;
    } while (search_min <= search_max);
    return 0;
}

static void qp_psp_free_charp_array(JSContext *ctx, char **array)
{
    Py_ssize_t i;
    if (!array)
        return;
    for (i = 0; array[i] != NULL; ++i)
        js_free(ctx, array[i]);
    js_free(ctx, array);
}

/* _PySequence_BytesToCharpArray: sequence of bytes -> NULL-terminated array */
static char **qp_psp_bytes_to_charp_array(JSContext *ctx, JSValueConst seq_obj)
{
    JSValue seq = qp_tuple_from_iterable(ctx, seq_obj);
    Py_ssize_t i, argc;
    char **array;
    if (JS_IsException(seq))
        return NULL;
    argc = QP_TUPLE_SIZE(seq);
    array = js_mallocz(ctx, (argc + 1) * sizeof(char *));
    if (!array) {
        JS_FreeValue(ctx, seq);
        return NULL;
    }
    for (i = 0; i < argc; ++i) {
        JSValueConst item = QP_TUPLE_ITEMS(seq)[i];
        const uint8_t *data;
        Py_ssize_t size;
        if (!qp_bytes_check(ctx, item)) {
            qp_raise_type_error(ctx, "expected bytes, %T found", item);
            goto fail;
        }
        data = qp_bytes_data(item, &size);
        /* PyBytes_AsStringAndSize: check for embedded null bytes */
        if (memchr(data, 0, size)) {
            qp_raise_value_error(ctx, "embedded null byte");
            goto fail;
        }
        array[i] = js_malloc(ctx, size + 1);
        if (!array[i])
            goto fail;
        memcpy(array[i], data, size);
        array[i][size] = '\0';
    }
    array[argc] = NULL;
    JS_FreeValue(ctx, seq);
    return array;
fail:
    JS_FreeValue(ctx, seq);
    qp_psp_free_charp_array(ctx, array);
    return NULL;
}

static int qp_psp_convert_fds_to_keep(JSContext *ctx, JSValueConst py_fds_to_keep, int *c_fds_to_keep)
{
    Py_ssize_t i, len = QP_TUPLE_SIZE(py_fds_to_keep);
    for (i = 0; i < len; ++i) {
        int64_t fd;
        if (qp_long_as_int64(ctx, QP_TUPLE_ITEMS(py_fds_to_keep)[i], &fd) < 0)
            return -1;
        if (fd < 0 || fd > INT_MAX) {
            qp_raise_value_error(ctx, "fd out of range in fds_to_keep.");
            return -1;
        }
        c_fds_to_keep[i] = (int)fd;
    }
    return 0;
}

/* _Py_set_inheritable_async_safe */
static int qp_psp_set_inheritable(int fd, int inheritable)
{
    int flags = fcntl(fd, F_GETFD), nf;
    if (flags < 0)
        return -1;
    nf = inheritable ? (flags & ~FD_CLOEXEC) : (flags | FD_CLOEXEC);
    if (nf == flags)
        return 0;
    return fcntl(fd, F_SETFD, nf);
}

/* This function must be async-signal-safe as it is called from child_exec()
 * after fork(). */
static int qp_psp_make_inheritable(int *c_fds_to_keep, Py_ssize_t len, int errpipe_write)
{
    Py_ssize_t i;
    for (i = 0; i < len; ++i) {
        int fd = c_fds_to_keep[i];
        if (fd == errpipe_write) {
            /* errpipe_write is part of fds_to_keep. It must be closed at
               exec(), but kept open in the child process until exec() is
               called. */
            continue;
        }
        if (qp_psp_set_inheritable(fd, 1) < 0)
            return -1;
    }
    return 0;
}

/* Get the maximum file descriptor that could be opened by this process.
 * This function is async signal safe for use between fork() and exec(). */
static long qp_psp_safe_get_max_fd(void)
{
    long local_max_fd;
#ifdef _SC_OPEN_MAX
    local_max_fd = sysconf(_SC_OPEN_MAX);
    if (local_max_fd == -1)
#endif
        local_max_fd = 256;  /* Matches legacy Lib/subprocess.py behavior. */
    return local_max_fd;
}

/* Close all file descriptors in the given range except for those in
 * fds_to_keep by invoking closer on each subrange. */
static int qp_psp_close_range_except(int start_fd, int end_fd, int *fds_to_keep,
                                     Py_ssize_t fds_to_keep_len, int (*closer)(int, int))
{
    Py_ssize_t keep_seq_idx;
    if (end_fd == -1) {
        long m = qp_psp_safe_get_max_fd();
        end_fd = m > INT_MAX ? INT_MAX : (int)m;
    }
    /* As fds_to_keep is sorted we can loop through the list closing
     * fds in between any in the keep list falling within our range. */
    for (keep_seq_idx = 0; keep_seq_idx < fds_to_keep_len; ++keep_seq_idx) {
        int keep_fd = fds_to_keep[keep_seq_idx];
        if (keep_fd < start_fd)
            continue;
        if (closer(start_fd, keep_fd - 1) != 0)
            return -1;
        start_fd = keep_fd + 1;
    }
    if (start_fd <= end_fd) {
        if (closer(start_fd, end_fd) != 0)
            return -1;
    }
    return 0;
}

static int qp_psp_brute_force_closer(int first, int last)
{
    int i;
    for (i = first; i <= last; i++) {
        /* Ignore errors */
        (void)close(i);
    }
    return 0;
}

#if defined(__linux__) && defined(SYS_getdents64)
/* struct linux_dirent64 (stable kernel ABI) */
struct qp_psp_linux_dirent64 {
   unsigned long long d_ino;
   long long d_off;
   unsigned short d_reclen;     /* Length of this linux_dirent */
   unsigned char  d_type;
   char           d_name[256];  /* Filename (null-terminated) */
};

/* Close all open file descriptors in the range from start_fd and higher
 * except those in the sorted fds_to_keep list; async signal safe (no
 * malloc, direct getdents64 syscall). */
static void qp_psp_close_open_fds_fallback(int start_fd, int *fds_to_keep, Py_ssize_t fds_to_keep_len)
{
    int fd_dir_fd;
    do {
        fd_dir_fd = open(QP_PSP_FD_DIR, O_RDONLY | O_CLOEXEC);
    } while (fd_dir_fd == -1 && errno == EINTR);
    if (fd_dir_fd == -1) {
        /* No way to get a list of open fds. */
        qp_psp_close_range_except(start_fd, -1, fds_to_keep, fds_to_keep_len,
                                  qp_psp_brute_force_closer);
        return;
    } else {
        char buffer[sizeof(struct qp_psp_linux_dirent64)];
        int bytes;
        while ((bytes = syscall(SYS_getdents64, fd_dir_fd,
                                (struct qp_psp_linux_dirent64 *)buffer,
                                sizeof(buffer))) > 0) {
            struct qp_psp_linux_dirent64 *entry;
            int offset;
            for (offset = 0; offset < bytes; offset += entry->d_reclen) {
                int fd;
                entry = (struct qp_psp_linux_dirent64 *)(buffer + offset);
                if ((fd = qp_psp_pos_int_from_ascii(entry->d_name)) < 0)
                    continue;  /* Not a number. */
                if (fd != fd_dir_fd && fd >= start_fd &&
                    !qp_psp_is_fd_in_sorted_fd_sequence(fd, fds_to_keep, fds_to_keep_len)) {
                    close(fd);
                }
            }
        }
        close(fd_dir_fd);
    }
}
#else
static int qp_psp_unsafe_closer(int first, int last)
{
    /* _Py_closerange */
    return qp_psp_brute_force_closer(first, last);
}

/* Close all open file descriptors from start_fd and higher except those in
 * the sorted fds_to_keep.  Calls opendir()/readdir()/closedir() like
 * CPython's _close_open_fds_maybe_unsafe. */
static void qp_psp_close_open_fds_fallback(int start_fd, int *fds_to_keep,
                                           Py_ssize_t fds_to_keep_len)
{
    DIR *proc_fd_dir = opendir(QP_PSP_FD_DIR);
    if (!proc_fd_dir) {
        /* No way to get a list of open fds. */
        qp_psp_close_range_except(start_fd, -1, fds_to_keep, fds_to_keep_len,
                                  qp_psp_unsafe_closer);
    } else {
        struct dirent *dir_entry;
        int fd_used_by_opendir = dirfd(proc_fd_dir);
        errno = 0;
        while ((dir_entry = readdir(proc_fd_dir))) {
            int fd;
            if ((fd = qp_psp_pos_int_from_ascii(dir_entry->d_name)) < 0)
                continue;  /* Not a number. */
            if (fd != fd_used_by_opendir && fd >= start_fd &&
                !qp_psp_is_fd_in_sorted_fd_sequence(fd, fds_to_keep, fds_to_keep_len)) {
                close(fd);
            }
            errno = 0;
        }
        if (errno) {
            /* readdir error, revert behavior. Highly Unlikely. */
            qp_psp_close_range_except(start_fd, -1, fds_to_keep, fds_to_keep_len,
                                      qp_psp_unsafe_closer);
        }
        closedir(proc_fd_dir);
    }
}
#endif

#if defined(__linux__) && defined(SYS_close_range)
/* close_range() is async-signal-safe on Linux (thin syscall wrapper) */
static int qp_psp_close_range_closer(int first, int last)
{
    return (int)syscall(SYS_close_range, (unsigned int)first, (unsigned int)last, 0);
}
#endif

static void qp_psp_close_open_fds(int start_fd, int *fds_to_keep, Py_ssize_t fds_to_keep_len)
{
#if defined(__linux__) && defined(SYS_close_range)
    if (qp_psp_close_range_except(start_fd, INT_MAX, fds_to_keep, fds_to_keep_len,
                                  qp_psp_close_range_closer) == 0) {
        return;
    }
#endif
    qp_psp_close_open_fds_fallback(start_fd, fds_to_keep, fds_to_keep_len);
}

/* _Py_RestoreSignals: SIGPIPE, SIGXFZ, SIGXFSZ back to SIG_DFL */
static void qp_psp_restore_signals(void)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_DFL);
#endif
#ifdef SIGXFZ
    signal(SIGXFZ, SIG_DFL);
#endif
#ifdef SIGXFSZ
    signal(SIGXFSZ, SIG_DFL);
#endif
}

/* _Py_write_noraise */
static void qp_psp_write_noraise(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= n;
    }
}

/* Code executed in the child process immediately after fork to set things
 * up and call exec().  Only async-signal-safe functions, except for the
 * preexec_fn callback, exactly as CPython. */
static void qp_psp_child_exec(JSContext *ctx,
                              char *const exec_array[], char *const argv[], char *const envp[],
                              const char *cwd,
                              int p2cread, int p2cwrite, int c2pread, int c2pwrite,
                              int errread, int errwrite, int errpipe_read, int errpipe_write,
                              int close_fds, int restore_signals, int call_setsid, pid_t pgid_to_set,
                              gid_t gid, Py_ssize_t extra_group_size, const gid_t *extra_groups,
                              uid_t uid, int child_umask,
                              int *fds_to_keep, Py_ssize_t fds_to_keep_len,
                              JSValueConst preexec_fn)
{
    int i, saved_errno;
    /* Indicate to the parent that the error happened before exec(). */
    const char *err_msg = "noexec";
    /* Buffer large enough to hold a hex integer.  We can't malloc. */
    char hex_errno[sizeof(saved_errno) * 2 + 1];

    if (qp_psp_make_inheritable(fds_to_keep, fds_to_keep_len, errpipe_write) < 0)
        goto error;

    /* Close parent's pipe ends. */
    if (p2cwrite != -1)
        QP_PSP_POSIX_CALL(close(p2cwrite));
    if (c2pread != -1)
        QP_PSP_POSIX_CALL(close(c2pread));
    if (errread != -1)
        QP_PSP_POSIX_CALL(close(errread));
    QP_PSP_POSIX_CALL(close(errpipe_read));

    /* When duping fds, if there arises a situation where one of the fds is
       either 0, 1 or 2, it is possible that it is overwritten (#12607). */
    if (c2pwrite == 0) {
        QP_PSP_POSIX_CALL(c2pwrite = dup(c2pwrite));
        /* issue32270 */
        if (qp_psp_set_inheritable(c2pwrite, 0) < 0)
            goto error;
    }
    while (errwrite == 0 || errwrite == 1) {
        QP_PSP_POSIX_CALL(errwrite = dup(errwrite));
        /* issue32270 */
        if (qp_psp_set_inheritable(errwrite, 0) < 0)
            goto error;
    }

    /* Dup fds for child.
       dup2() removes the CLOEXEC flag but we must do it ourselves if dup2()
       would be a no-op (issue #10806). */
    if (p2cread == 0) {
        if (qp_psp_set_inheritable(p2cread, 1) < 0)
            goto error;
    } else if (p2cread != -1)
        QP_PSP_POSIX_CALL(dup2(p2cread, 0));  /* stdin */

    if (c2pwrite == 1) {
        if (qp_psp_set_inheritable(c2pwrite, 1) < 0)
            goto error;
    } else if (c2pwrite != -1)
        QP_PSP_POSIX_CALL(dup2(c2pwrite, 1));  /* stdout */

    if (errwrite == 2) {
        if (qp_psp_set_inheritable(errwrite, 1) < 0)
            goto error;
    } else if (errwrite != -1)
        QP_PSP_POSIX_CALL(dup2(errwrite, 2));  /* stderr */

    /* We no longer manually close p2cread, c2pwrite, and errwrite here as
     * _close_open_fds takes care when it is not already non-inheritable. */

    if (cwd) {
        if (chdir(cwd) == -1) {
            err_msg = "noexec:chdir";
            goto error;
        }
    }

    if (child_umask >= 0)
        umask(child_umask);  /* umask() always succeeds. */

    if (restore_signals)
        qp_psp_restore_signals();

    if (call_setsid)
        QP_PSP_POSIX_CALL(setsid());

    if (pgid_to_set >= 0)
        QP_PSP_POSIX_CALL(setpgid(0, pgid_to_set));

    if (extra_group_size >= 0)
        QP_PSP_POSIX_CALL(setgroups((int)extra_group_size, extra_groups));

    if (gid != (gid_t)-1)
        QP_PSP_POSIX_CALL(setregid(gid, gid));

    if (uid != (uid_t)-1)
        QP_PSP_POSIX_CALL(setreuid(uid, uid));

    err_msg = "";
    if (!JS_IsUndefined(preexec_fn) && !JS_IsNull(preexec_fn)) {
        /* This is where the user has asked us to deadlock their program. */
        JSValue result = qp_call0(ctx, preexec_fn);
        if (JS_IsException(result)) {
            /* Stringifying the exception or traceback would involve
             * memory allocation and thus potential for deadlock. */
            err_msg = "Exception occurred in preexec_fn.";
            errno = 0;  /* We don't want to report an OSError. */
            goto error;
        }
        /* We're about to exec so why bother freeing result. */
    }

    /* close FDs after executing preexec_fn, which might open FDs */
    if (close_fds)
        qp_psp_close_open_fds(3, fds_to_keep, fds_to_keep_len);

    /* This loop matches the Lib/os.py _execvpe()'s PATH search when */
    /* given the executable_list generated by Lib/subprocess.py.     */
    saved_errno = 0;
    for (i = 0; exec_array[i] != NULL; ++i) {
        const char *executable = exec_array[i];
        if (envp)
            execve(executable, argv, envp);
        else
            execv(executable, argv);
        if (errno != ENOENT && errno != ENOTDIR && saved_errno == 0)
            saved_errno = errno;
    }
    /* Report the first exec error, not the last. */
    if (saved_errno)
        errno = saved_errno;

error:
    saved_errno = errno;
    /* Report the posix error to our parent process. */
    if (saved_errno) {
        static const char hexdigits[] = "0123456789abcdef";
        char *cur;
        qp_psp_write_noraise(errpipe_write, "OSError:", 8);
        cur = hex_errno + sizeof(hex_errno);
        while (saved_errno != 0 && cur != hex_errno) {
            *--cur = hexdigits[saved_errno % 16];
            saved_errno /= 16;
        }
        qp_psp_write_noraise(errpipe_write, cur, hex_errno + sizeof(hex_errno) - cur);
        qp_psp_write_noraise(errpipe_write, ":", 1);
        /* We can't call strerror(saved_errno).  It is not async signal safe.
         * The parent process will look the error message up. */
    } else {
        qp_psp_write_noraise(errpipe_write, "SubprocessError:0:", 18);
    }
    qp_psp_write_noraise(errpipe_write, err_msg, strlen(err_msg));
}

static JSValue qp_psp_fork_exec(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst process_args, executable_list, py_fds_to_keep, cwd_obj, env_list;
    JSValueConst gid_object, extra_groups_packed, uid_object, preexec_fn;
    int close_fds, p2cread, p2cwrite, c2pread, c2pwrite, errread, errwrite;
    int errpipe_read, errpipe_write, restore_signals, call_setsid, pgid_to_set, child_umask;
    char **exec_array = NULL, **argv = NULL, **envp = NULL;
    JSValue cwd_obj2 = JS_UNDEFINED, converted_args = JS_UNDEFINED;
    const char *cwd = NULL;
    gid_t *extra_groups = NULL;
    Py_ssize_t extra_group_size = -2, fds_to_keep_len;
    gid_t gid = (gid_t)-1;
    uid_t uid = (uid_t)-1;
    int *c_fds_to_keep = NULL;
    int need_after_fork = 0, saved_errno = 0;
    pid_t pid = -1;
    BOOL has_preexec;

    if (qp_arg_check_positional(ctx, "fork_exec", nargs, 23, 23) < 0)
        return JS_EXCEPTION;
    process_args = args[0];
    executable_list = args[1];
    py_fds_to_keep = args[3];
    cwd_obj = args[4];
    env_list = args[5];
    gid_object = args[17];
    extra_groups_packed = args[18];
    uid_object = args[19];
    preexec_fn = args[21];
    if (qp_arg_bool(ctx, args[2], &close_fds) < 0)
        return JS_EXCEPTION;
    if (!qp_tuple_check(ctx, py_fds_to_keep))
        return qp_raise_type_error(ctx, "fork_exec() argument 4 must be tuple, not %T", py_fds_to_keep);
    if (qp_arg_int(ctx, args[6], &p2cread) < 0 || qp_arg_int(ctx, args[7], &p2cwrite) < 0 ||
        qp_arg_int(ctx, args[8], &c2pread) < 0 || qp_arg_int(ctx, args[9], &c2pwrite) < 0 ||
        qp_arg_int(ctx, args[10], &errread) < 0 || qp_arg_int(ctx, args[11], &errwrite) < 0 ||
        qp_arg_int(ctx, args[12], &errpipe_read) < 0 || qp_arg_int(ctx, args[13], &errpipe_write) < 0 ||
        qp_arg_bool(ctx, args[14], &restore_signals) < 0 || qp_arg_bool(ctx, args[15], &call_setsid) < 0 ||
        qp_arg_int(ctx, args[16], &pgid_to_set) < 0 || qp_arg_int(ctx, args[20], &child_umask) < 0)
        return JS_EXCEPTION;
    has_preexec = !JS_IsUndefined(preexec_fn) && !JS_IsNull(preexec_fn);
    fds_to_keep_len = QP_TUPLE_SIZE(py_fds_to_keep);

    if (close_fds && errpipe_write < 3)  /* precondition */
        return qp_raise_value_error(ctx, "errpipe_write must be >= 3");
    if (qp_psp_sanity_check_fd_sequence(ctx, py_fds_to_keep))
        return qp_raise_value_error(ctx, "bad value(s) in fds_to_keep");

    exec_array = qp_psp_bytes_to_charp_array(ctx, executable_list);
    if (!exec_array)
        goto cleanup;

    /* Convert args and env into appropriate arguments for exec() in the
       parent process to avoid allocating or freeing memory in the child. */
    if (!JS_IsUndefined(process_args) && !JS_IsNull(process_args)) {
        JSValue fast;
        Py_ssize_t num_args, i;
        if (qp_list_check(ctx, process_args) || qp_tuple_check(ctx, process_args)) {
            fast = qp_tuple_from_iterable(ctx, process_args);
        } else {
            JSValue it = qp_iter(ctx, process_args);
            if (JS_IsException(it)) {
                if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    qp_raise_type_error(ctx, "argv must be a tuple");
                }
                goto cleanup;
            }
            JS_FreeValue(ctx, it);
            fast = qp_tuple_from_iterable(ctx, process_args);
        }
        if (JS_IsException(fast))
            goto cleanup;
        num_args = QP_TUPLE_SIZE(fast);
        converted_args = qp_list_new(ctx, 0);
        if (JS_IsException(converted_args)) {
            JS_FreeValue(ctx, fast);
            goto cleanup;
        }
        for (i = 0; i < num_args; ++i) {
            JSValue c = qp_os_fsconvert(ctx, QP_TUPLE_ITEMS(fast)[i]);
            if (JS_IsException(c) || qp_list_append(ctx, converted_args, c) < 0) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, fast);
                goto cleanup;
            }
            JS_FreeValue(ctx, c);
        }
        JS_FreeValue(ctx, fast);
        argv = qp_psp_bytes_to_charp_array(ctx, converted_args);
        JS_FreeValue(ctx, converted_args);
        converted_args = JS_UNDEFINED;
        if (!argv)
            goto cleanup;
    }

    if (!JS_IsUndefined(env_list) && !JS_IsNull(env_list)) {
        envp = qp_psp_bytes_to_charp_array(ctx, env_list);
        if (!envp)
            goto cleanup;
    }

    if (!JS_IsUndefined(cwd_obj) && !JS_IsNull(cwd_obj)) {
        Py_ssize_t len;
        cwd_obj2 = qp_os_fsconvert(ctx, cwd_obj);
        if (JS_IsException(cwd_obj2)) {
            cwd_obj2 = JS_UNDEFINED;
            goto cleanup;
        }
        /* NUL-terminated copy for chdir() */
        {
            const uint8_t *d = qp_bytes_data(cwd_obj2, &len);
            char *c = js_malloc(ctx, len + 1);
            if (!c)
                goto cleanup;
            memcpy(c, d, len);
            c[len] = '\0';
            cwd = c;
        }
    }

    /* extra_group_size == -2: subprocess API called with extra_groups=None
       (no setgroups call at all). */
    if (!JS_IsUndefined(extra_groups_packed) && !JS_IsNull(extra_groups_packed)) {
        Py_ssize_t i;
        JSValue *items;
        if (!qp_list_check(ctx, extra_groups_packed)) {
            qp_raise_type_error(ctx, "setgroups argument must be a list");
            goto cleanup;
        }
        items = qp_list_items(extra_groups_packed, &extra_group_size);
        if (extra_group_size > QP_PSP_MAX_GROUPS) {
            qp_raise_value_error(ctx, "too many extra_groups");
            goto cleanup;
        }
        /* Deliberately keep extra_groups == NULL for extra_group_size == 0 */
        if (extra_group_size > 0) {
            extra_groups = js_malloc(ctx, extra_group_size * sizeof(gid_t));
            if (!extra_groups) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise(ctx, QP_EXC(ctx, MemoryError), "failed to allocate memory for group list");
                goto cleanup;
            }
        }
        for (i = 0; i < extra_group_size; i++) {
            uint32_t g;
            items = qp_list_items(extra_groups_packed, &extra_group_size);
            if (i >= extra_group_size)
                break;
            if (!qp_long_check(ctx, items[i])) {
                qp_raise_type_error(ctx, "extra_groups must be integers");
                goto cleanup;
            }
            if (qp_os_id_conv(ctx, items[i], &g, "gid") < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_value_error(ctx, "invalid group id");
                goto cleanup;
            }
            extra_groups[i] = (gid_t)g;
        }
    }

    if (!JS_IsUndefined(gid_object) && !JS_IsNull(gid_object)) {
        uint32_t g;
        if (qp_os_id_conv(ctx, gid_object, &g, "gid") < 0)
            goto cleanup;
        gid = (gid_t)g;
    }
    if (!JS_IsUndefined(uid_object) && !JS_IsNull(uid_object)) {
        uint32_t u;
        if (qp_os_id_conv(ctx, uid_object, &u, "uid") < 0)
            goto cleanup;
        uid = (uid_t)u;
    }

    c_fds_to_keep = js_malloc(ctx, (fds_to_keep_len ? fds_to_keep_len : 1) * sizeof(int));
    if (!c_fds_to_keep) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        qp_raise(ctx, QP_EXC(ctx, MemoryError), "failed to malloc c_fds_to_keep");
        goto cleanup;
    }
    if (qp_psp_convert_fds_to_keep(ctx, py_fds_to_keep, c_fds_to_keep) < 0)
        goto cleanup;

    /* This must be the last thing done before fork() because we do not
     * want to call the before-fork hooks if there is any chance of another
     * error leading to the cleanup: code without calling fork(). */
    if (has_preexec) {
        qp_os_before_fork(ctx);
        need_after_fork = 1;
    }

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        /* Child process. */
        if (has_preexec) {
            /* We'll be calling back into Python later so we need to do
               this (PyOS_AfterFork_Child). */
            qp_os_after_fork(ctx, 1);
        }
        qp_psp_child_exec(ctx, exec_array, argv, envp, cwd,
                          p2cread, p2cwrite, c2pread, c2pwrite,
                          errread, errwrite, errpipe_read, errpipe_write,
                          close_fds, restore_signals, call_setsid, (pid_t)pgid_to_set,
                          gid, extra_group_size, extra_groups,
                          uid, child_umask,
                          c_fds_to_keep, fds_to_keep_len, preexec_fn);
        _exit(255);
    }

    /* Parent (original) process */
    if (pid == (pid_t)-1) {
        /* Capture errno for the exception. */
        saved_errno = errno;
    }
    if (need_after_fork)
        qp_os_after_fork(ctx, 0);

cleanup:
    js_free(ctx, c_fds_to_keep);
    if (saved_errno != 0) {
        /* We can't call this above as the after-fork hooks call back into
           Python code which would see the unreturned error. */
        qp_raise_os_error(ctx, saved_errno, QP_NULL);
    }
    js_free(ctx, extra_groups);
    js_free(ctx, (void *)cwd);
    JS_FreeValue(ctx, cwd_obj2);
    JS_FreeValue(ctx, converted_args);
    qp_psp_free_charp_array(ctx, envp);
    qp_psp_free_charp_array(ctx, argv);
    qp_psp_free_charp_array(ctx, exec_array);
    if (pid == -1)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int)pid);
}

static const QPMethodDef qp_posixsubprocess_methods[] = {
    { "fork_exec", qp_psp_fork_exec, QP_METH_FASTCALL, 0,
      "fork_exec($module, args, executable_list, close_fds, pass_fds, cwd,\n"
      "          env, p2cread, p2cwrite, c2pread, c2pwrite, errread, errwrite,\n"
      "          errpipe_read, errpipe_write, restore_signals, call_setsid,\n"
      "          pgid_to_set, gid, extra_groups, uid, child_umask, preexec_fn,\n"
      "          /)\n"
      "--\n"
      "\n"
      "Spawn a fresh new child process.\n"
      "\n"
      "Fork a child process, close parent file descriptors as appropriate in\n"
      "the child and duplicate the few that are needed before calling exec() in\n"
      "the child process.\n"
      "\n"
      "If close_fds is True, close file descriptors 3 and higher, except those\n"
      "listed in the sorted tuple pass_fds.\n"
      "\n"
      "The preexec_fn, if supplied, will be called immediately before closing\n"
      "file descriptors and exec.\n"
      "\n"
      "WARNING: preexec_fn is NOT SAFE if your application uses threads.\n"
      "         It may trigger infrequent, difficult to debug deadlocks.\n"
      "\n"
      "If an error occurs in the child process before the exec, it is\n"
      "serialized and written to the errpipe_write fd per subprocess.py.\n"
      "\n"
      "Returns: the child process\'s PID.\n"
      "\n"
      "Raises: Only on an error in the parent process." },
    { NULL }
};

static const QPModuleDef qp_moddef__posixsubprocess = {
    "_posixsubprocess",
    "A POSIX helper for the subprocess module.",
    qp_posixsubprocess_methods,
    NULL,
};
