/*
 * select module (port of CPython Modules/selectmodule.c, PSF-2.0): select()
 * and poll objects with the POLL* constants.  epoll/kqueue/devpoll are not
 * provided (as in a CPython build without them); selectors.py then picks
 * PollSelector, which is what subprocess.communicate uses on POSIX.
 */
#include <sys/select.h>
#include <poll.h>
#include <limits.h>
#include <math.h>
#include <time.h>

static int qp_object_as_fd(JSContext *ctx, JSValueConst o, int *pfd);

static double qp_sel_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* _PyTime_FromSecondsObject / _PyTime_FromMillisecondsObject: converts an
   int or float to a double count of units; TypeError for other types. */
static int qp_sel_time_object(JSContext *ctx, JSValueConst obj, double *out)
{
    double d;
    if (qp_float_check(ctx, obj)) {
        if (qp_float_as_double(ctx, obj, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
    } else {
        JSValue v = qp_index(ctx, obj);
        if (JS_IsException(v))
            return -1;
        if (qp_long_as_double(ctx, v, &d) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
    }
    if (fabs(d) > 9.2e9) {
        qp_raise_overflow_error(ctx, "timestamp too large to convert to C PyTime_t");
        return -1;
    }
    *out = d;
    return 0;
}

typedef struct QPSelEntry {
    int fd;
    JSValue obj;    /* borrowed from the sequence held in QPSelSet.seq */
} QPSelEntry;

typedef struct QPSelSet {
    JSValue seq;    /* list (PySequence_Fast result) */
    QPSelEntry *e;
    int n;
} QPSelSet;

/* seq2set */
static int qp_sel_seq2set(JSContext *ctx, JSValueConst seq, fd_set *set, QPSelSet *s, int *pmax)
{
    Py_ssize_t i, n;
    JSValue *items;
    FD_ZERO(set);
    s->e = NULL;
    s->n = 0;
    if (qp_list_check(ctx, seq)) {
        s->seq = JS_DupValue(ctx, seq);
    } else if (qp_tuple_check(ctx, seq)) {
        s->seq = qp_list_new_steal(ctx, 0, NULL);
        if (JS_IsException(s->seq) || qp_list_extend(ctx, s->seq, seq) < 0)
            return -1;
    } else {
        JSValue it = qp_iter(ctx, seq);
        if (JS_IsException(it)) {
            s->seq = JS_UNDEFINED;
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "arguments 1-3 must be sequences");
            }
            return -1;
        }
        JS_FreeValue(ctx, it);
        s->seq = qp_list_from_iterable(ctx, seq);
        if (JS_IsException(s->seq))
            return -1;
    }
    items = qp_list_items(s->seq, &n);
    if (n > FD_SETSIZE) {
        qp_raise_value_error(ctx, "too many file descriptors in select()");
        return -1;
    }
    s->e = js_malloc(ctx, sizeof(QPSelEntry) * (n ? n : 1));
    if (!s->e)
        return -1;
    for (i = 0; i < n; i++) {
        int v;
        items = qp_list_items(s->seq, &n);
        if (i >= n)
            break;
        if (qp_object_as_fd(ctx, items[i], &v) < 0)
            return -1;
        if (v < 0 || v >= FD_SETSIZE) {
            qp_raise_value_error(ctx, "filedescriptor out of range in select()");
            return -1;
        }
        if (v > *pmax)
            *pmax = v;
        FD_SET(v, set);
        items = qp_list_items(s->seq, &n);
        s->e[s->n].fd = v;
        s->e[s->n].obj = items[i];
        s->n++;
    }
    return 0;
}

static void qp_sel_set_free(JSContext *ctx, QPSelSet *s)
{
    js_free(ctx, s->e);
    JS_FreeValue(ctx, s->seq);
}

/* set2list */
static JSValue qp_sel_set2list(JSContext *ctx, fd_set *set, QPSelSet *s)
{
    JSValue list = qp_list_new(ctx, 0);
    int i;
    if (JS_IsException(list))
        return list;
    for (i = 0; i < s->n; i++) {
        if (FD_ISSET(s->e[i].fd, set)) {
            if (qp_list_append(ctx, list, s->e[i].obj) < 0) {
                JS_FreeValue(ctx, list);
                return JS_EXCEPTION;
            }
        }
    }
    return list;
}

static JSValue qp_sel_select(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst timeout_obj;
    QPSelSet rs = { JS_UNDEFINED }, ws = { JS_UNDEFINED }, es = { JS_UNDEFINED };
    fd_set ifdset, ofdset, efdset;
    struct timeval tv, *tvp = NULL;
    double timeout = 0, deadline = 0;
    int max = -1, n, err;
    JSValue ret = JS_EXCEPTION, items[3];

    if (qp_arg_check_positional(ctx, "select", nargs, 3, 4) < 0)
        return JS_EXCEPTION;
    timeout_obj = nargs > 3 ? args[3] : JS_UNDEFINED;
    if (!JS_IsUndefined(timeout_obj) && !JS_IsNull(timeout_obj)) {
        if (qp_sel_time_object(ctx, timeout_obj, &timeout) < 0) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "timeout must be a real number or None, not %T", timeout_obj);
            }
            return JS_EXCEPTION;
        }
        if (timeout < 0) {
            qp_raise_value_error(ctx, "timeout must be non-negative");
            return JS_EXCEPTION;
        }
        tvp = &tv;
        deadline = qp_sel_monotonic() + timeout;
    }
    if (qp_sel_seq2set(ctx, args[0], &ifdset, &rs, &max) < 0 ||
        qp_sel_seq2set(ctx, args[1], &ofdset, &ws, &max) < 0 ||
        qp_sel_seq2set(ctx, args[2], &efdset, &es, &max) < 0)
        goto done;
    for (;;) {
        if (tvp) {
            /* _PyTime_ROUND_TIMEOUT: round up to the microsecond */
            double us = ceil(timeout * 1e6);
            tv.tv_sec = (time_t)(us / 1e6);
            tv.tv_usec = (suseconds_t)(us - (double)tv.tv_sec * 1e6);
        }
        errno = 0;
        n = select(max + 1, &ifdset, &ofdset, &efdset, tvp);
        err = errno;
        if (n >= 0 || err != EINTR)
            break;
        /* PEP 475: retry with the recomputed timeout and fresh sets */
        {
            int i;
            FD_ZERO(&ifdset); FD_ZERO(&ofdset); FD_ZERO(&efdset);
            for (i = 0; i < rs.n; i++) FD_SET(rs.e[i].fd, &ifdset);
            for (i = 0; i < ws.n; i++) FD_SET(ws.e[i].fd, &ofdset);
            for (i = 0; i < es.n; i++) FD_SET(es.e[i].fd, &efdset);
        }
        if (tvp) {
            timeout = deadline - qp_sel_monotonic();
            if (timeout < 0) {
                n = 0;
                FD_ZERO(&ifdset); FD_ZERO(&ofdset); FD_ZERO(&efdset);
                break;
            }
        }
    }
    if (n < 0) {
        qp_raise_os_error(ctx, err, QP_NULL);
        goto done;
    }
    items[0] = qp_sel_set2list(ctx, &ifdset, &rs);
    items[1] = qp_sel_set2list(ctx, &ofdset, &ws);
    items[2] = qp_sel_set2list(ctx, &efdset, &es);
    if (JS_IsException(items[0]) || JS_IsException(items[1]) || JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[2]);
        goto done;
    }
    ret = qp_tuple_new_steal(ctx, 3, items);
done:
    qp_sel_set_free(ctx, &rs);
    qp_sel_set_free(ctx, &ws);
    qp_sel_set_free(ctx, &es);
    return ret;
}

/* ---- poll objects ---------------------------------------------------- */

enum { QP_POLL_SLOT_DICT, QP_POLL_SLOT_RUNNING, QP_POLL_NSLOTS };
#define QP_SEL_POLL_TYPE_PROP "\x01qp_select_poll_type"

/* _PyLong_UnsignedShort_Converter */
static int qp_sel_ushort(JSContext *ctx, JSValueConst v, unsigned short *out)
{
    JSValue i = qp_index(ctx, v);
    int64_t x;
    int overflow = 0;
    if (JS_IsException(i))
        return -1;
    if (qp_long_as_int64_overflow(i, &x, &overflow) < 0 || overflow) {
        int neg = qp_long_sign(i) < 0;
        JS_FreeValue(ctx, i);
        if (neg)
            qp_raise_value_error(ctx, "value must be positive");
        else
            qp_raise_overflow_error(ctx, "Python int too large for C unsigned short");
        return -1;
    }
    JS_FreeValue(ctx, i);
    if (x < 0) {
        qp_raise_value_error(ctx, "value must be positive");
        return -1;
    }
    if (x > USHRT_MAX) {
        qp_raise_overflow_error(ctx, "Python int too large for C unsigned short");
        return -1;
    }
    *out = (unsigned short)x;
    return 0;
}

static JSValue qp_poll_dict(JSValueConst self)
{
    return qp_instance_slots(self)[QP_POLL_SLOT_DICT];
}

static JSValue qp_poll_register(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    int fd;
    unsigned short mask = POLLIN | POLLPRI | POLLOUT;
    JSValue key, val;
    int r;
    if (qp_arg_check_positional(ctx, magic ? "modify" : "register", nargs, magic ? 2 : 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    if (nargs > 1 && qp_sel_ushort(ctx, args[1], &mask) < 0)
        return JS_EXCEPTION;
    key = JS_NewInt32(ctx, fd);
    if (magic) {
        /* modify: the fd must be registered */
        r = qp_dict_contains(ctx, qp_poll_dict(self), key);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0) {
            return qp_raise_os_error(ctx, ENOENT, QP_NULL);
        }
    }
    val = JS_NewInt32(ctx, mask);
    if (qp_dict_set_item(ctx, qp_poll_dict(self), key, val) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_poll_unregister(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    int fd, r;
    JSValue key, val;
    if (qp_object_as_fd(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    key = JS_NewInt32(ctx, fd);
    r = qp_dict_pop(ctx, qp_poll_dict(self), key, &val);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_key_error(ctx, key);
    JS_FreeValue(ctx, val);
    return JS_UNDEFINED;
}

static JSValue qp_poll_poll(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue *slots = qp_instance_slots(self);
    JSValueConst dict = slots[QP_POLL_SLOT_DICT];
    double ms = -1, deadline = 0;
    int timeout_ms = -1, n, err, i, nfds;
    struct pollfd *ufds;
    Py_ssize_t pos = 0;
    JSValue k, v, result;

    if (qp_arg_check_positional(ctx, "poll", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs > 0 && !JS_IsUndefined(args[0]) && !JS_IsNull(args[0])) {
        if (qp_sel_time_object(ctx, args[0], &ms) < 0) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "timeout must be a real number or None, not %T", args[0]);
            }
            return JS_EXCEPTION;
        }
        /* _PyTime_ROUND_TIMEOUT to milliseconds */
        ms = ceil(ms);
        if (ms >= 0) {
            if (ms > INT_MAX) {
                qp_raise_overflow_error(ctx, "timeout is too large");
                return JS_EXCEPTION;
            }
            timeout_ms = (int)ms;
            deadline = qp_sel_monotonic() + ms / 1000.0;
        } else {
            ms = -1;
        }
    }
    if (JS_ToBool(ctx, slots[QP_POLL_SLOT_RUNNING])) {
        return qp_raise_runtime_error(ctx, "concurrent poll() invocation");
    }
    nfds = (int)qp_dict_size(dict);
    ufds = js_malloc(ctx, sizeof(struct pollfd) * (nfds ? nfds : 1));
    if (!ufds)
        return JS_EXCEPTION;
    i = 0;
    while (i < nfds && qp_dict_next(dict, &pos, &k, &v)) {
        ufds[i].fd = JS_VALUE_GET_INT(k);
        ufds[i].events = (short)JS_VALUE_GET_INT(v);
        ufds[i].revents = 0;
        i++;
    }
    nfds = i;
    slots[QP_POLL_SLOT_RUNNING] = JS_TRUE;
    for (;;) {
        errno = 0;
        n = poll(ufds, nfds, timeout_ms);
        err = errno;
        if (n >= 0 || err != EINTR)
            break;
        /* PEP 475: retry with the recomputed timeout */
        if (ms >= 0) {
            double rem = (deadline - qp_sel_monotonic()) * 1000.0;
            if (rem < 0) {
                n = 0;
                break;
            }
            timeout_ms = (int)ceil(rem);
        }
    }
    slots[QP_POLL_SLOT_RUNNING] = JS_FALSE;
    if (n < 0) {
        js_free(ctx, ufds);
        return qp_raise_os_error(ctx, err, QP_NULL);
    }
    result = qp_list_new(ctx, 0);
    if (JS_IsException(result))
        goto fail;
    for (i = 0; i < nfds && n > 0; i++) {
        JSValue pair[2], t;
        int r;
        if (ufds[i].revents == 0)
            continue;
        pair[0] = JS_NewInt32(ctx, ufds[i].fd);
        pair[1] = JS_NewInt32(ctx, ufds[i].revents & 0xffff);
        t = qp_tuple_new_steal(ctx, 2, pair);
        if (JS_IsException(t))
            goto fail;
        r = qp_list_append(ctx, result, t);
        JS_FreeValue(ctx, t);
        if (r < 0)
            goto fail;
        n--;
    }
    js_free(ctx, ufds);
    return result;
fail:
    js_free(ctx, ufds);
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

static const QPMethodDef qp_poll_methods[] = {
    { "register", qp_poll_register, QP_METH_FASTCALL, 0, "register($self, fd,\n"
"         eventmask=select.POLLIN | select.POLLPRI | select.POLLOUT, /)\n"
"--\n"
"\n"
"Register a file descriptor with the polling object.\n"
"\n"
"  fd\n"
"    either an integer, or an object with a fileno() method returning an int\n"
"  eventmask\n"
"    an optional bitmask describing the type of events to check for" },
    { "modify", qp_poll_register, QP_METH_FASTCALL, 1, "modify($self, fd, eventmask, /)\n"
"--\n"
"\n"
"Modify an already registered file descriptor.\n"
"\n"
"  fd\n"
"    either an integer, or an object with a fileno() method returning\n"
"    an int\n"
"  eventmask\n"
"    a bitmask describing the type of events to check for" },
    { "unregister", qp_poll_unregister, QP_METH_O, 0, "unregister($self, fd, /)\n"
"--\n"
"\n"
"Remove a file descriptor being tracked by the polling object." },
    { "poll", qp_poll_poll, QP_METH_FASTCALL, 0, "poll($self, timeout=None, /)\n"
"--\n"
"\n"
"Polls the set of registered file descriptors.\n"
"\n"
"  timeout\n"
"    The maximum time to wait in milliseconds, or else None (or a negative\n"
"    value) to wait indefinitely.\n"
"\n"
"Returns a list containing any descriptors that have events or errors\n"
"to report, as a list of (fd, event) 2-tuples." },
    { NULL }
};

static const QPTypeSpec qp_poll_spec = {
    .name = "select.poll",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_POLL_NSLOTS,
    .methods = qp_poll_methods,
};

static JSValue qp_sel_poll(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue tv = JS_GetPropertyStr(ctx, self, QP_SEL_POLL_TYPE_PROP);
    QPType *t;
    JSValue obj, d;
    if (JS_IsException(tv))
        return tv;
    t = qp_type_from_value(tv);
    JS_FreeValue(ctx, tv);
    if (!t)
        return qp_raise_system_error(ctx, "select.poll type missing");
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return d;
    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, d);
        return obj;
    }
    qp_instance_slots(obj)[QP_POLL_SLOT_DICT] = d;
    qp_instance_slots(obj)[QP_POLL_SLOT_RUNNING] = JS_FALSE;
    return obj;
}

static const QPMethodDef qp_select_methods[] = {
    { "select", qp_sel_select, QP_METH_FASTCALL, 0, "select($module, rlist, wlist, xlist, timeout=None, /)\n"
"--\n"
"\n"
"Wait until one or more file descriptors are ready for some kind of I/O.\n"
"\n"
"The first three arguments are iterables of file descriptors to be waited\n"
"for:\n"
"rlist -- wait until ready for reading\n"
"wlist -- wait until ready for writing\n"
"xlist -- wait for an \"exceptional condition\"\n"
"If only one kind of condition is required, pass [] for the other lists.\n"
"\n"
"A file descriptor is either a socket or file object, or a small integer\n"
"gotten from a fileno() method call on one of those.\n"
"\n"
"The optional 4th argument specifies a timeout in seconds; it may be\n"
"a non-integer to specify fractions of seconds.  If it is absent\n"
"or None, the call will never time out.\n"
"\n"
"The return value is a tuple of three lists corresponding to the first\n"
"three arguments; each contains the subset of the corresponding file\n"
"descriptors that are ready.\n"
"\n"
"*** IMPORTANT NOTICE ***\n"
"On Windows, only sockets are supported; on Unix, all file\n"
"descriptors can be used." },
    { "poll", qp_sel_poll, QP_METH_NOARGS, 0, "poll($module, /)\n"
"--\n"
"\n"
"Returns a polling object.\n"
"\n"
"This object supports registering and unregistering file descriptors, and\n"
"then polling them for I/O events." },
    { NULL }
};

static int qp_sel_add(JSContext *ctx, JSValueConst module, const char *name, JSValue v)
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

static int qp_select_exec(JSContext *ctx, JSValueConst module)
{
    JSValue pt;
#define QP_SEL_INT(VAL) \
    do { if (qp_sel_add(ctx, module, #VAL, JS_NewInt32(ctx, (VAL))) < 0) return -1; } while (0)
    if (qp_sel_add(ctx, module, "error", JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, OSError)))) < 0)
        return -1;
#ifdef PIPE_BUF
    QP_SEL_INT(PIPE_BUF);
#endif
    pt = qp_type_from_spec(ctx, &qp_poll_spec);
    if (JS_IsException(pt))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, module, QP_SEL_POLL_TYPE_PROP, pt, 0) < 0)
        return -1;
    QP_SEL_INT(POLLIN);
    QP_SEL_INT(POLLPRI);
    QP_SEL_INT(POLLOUT);
    QP_SEL_INT(POLLERR);
    QP_SEL_INT(POLLHUP);
    QP_SEL_INT(POLLNVAL);
#ifdef POLLRDNORM
    QP_SEL_INT(POLLRDNORM);
#endif
#ifdef POLLRDBAND
    QP_SEL_INT(POLLRDBAND);
#endif
#ifdef POLLWRNORM
    QP_SEL_INT(POLLWRNORM);
#endif
#ifdef POLLWRBAND
    QP_SEL_INT(POLLWRBAND);
#endif
#ifdef POLLMSG
    QP_SEL_INT(POLLMSG);
#endif
#ifdef POLLRDHUP
    /* Kernel 2.6.17+ */
    QP_SEL_INT(POLLRDHUP);
#endif
#undef QP_SEL_INT
    return 0;
}

static const QPModuleDef qp_moddef_select = {
    "select",
    "This module supports asynchronous I/O on multiple file descriptors.\n\
\n\
*** IMPORTANT NOTICE ***\n\
On Windows, only sockets are supported; on Unix, all file descriptors.",
    qp_select_methods,
    qp_select_exec,
};
