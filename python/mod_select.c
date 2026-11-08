/*
 * select module (port of CPython Modules/selectmodule.c, PSF-2.0): select()
 * poll objects with the POLL* constants, and on macOS/BSD the kqueue and
 * kevent types with the KQ_* constants (HAVE_KQUEUE).  epoll/devpoll are not
 * provided (as in a CPython build without them); selectors.py then picks
 * KqueueSelector on macOS/BSD and PollSelector elsewhere.
 */
#include <sys/select.h>
#include <poll.h>
#include <limits.h>
#include <math.h>
#include <time.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define QP_HAVE_KQUEUE 1
#include <sys/types.h>
#include <sys/event.h>
#include <unistd.h>
#endif

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
        QP_BEGIN_ALLOW_THREADS(ctx)
        errno = 0;
        n = select(max + 1, &ifdset, &ofdset, &efdset, tvp);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0 || err != EINTR)
            break;
        /* select() was interrupted by a signal: run the Python signal
           handlers (PEP 475), then retry with the recomputed timeout and
           fresh sets */
        if (qp_signal_check(ctx) < 0)
            goto done;
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
        QP_BEGIN_ALLOW_THREADS(ctx)
        errno = 0;
        n = poll(ufds, nfds, timeout_ms);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (n >= 0 || err != EINTR)
            break;
        /* poll() was interrupted by a signal: run the Python signal
           handlers (PEP 475), then retry with the recomputed timeout */
        if (qp_signal_check(ctx) < 0) {
            slots[QP_POLL_SLOT_RUNNING] = JS_FALSE;
            js_free(ctx, ufds);
            return JS_EXCEPTION;
        }
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

#ifdef QP_HAVE_KQUEUE
/* ---- kqueue / kevent (kqueue interface for BSD) ------------------------
 * Copyright (c) 2000 Doug White, 2006 James Knight, 2007 Christian Heimes
 * (BSD-2-Clause, see CPython Modules/selectmodule.c). */

/* kevent instances keep their struct kevent in an ArrayBuffer slot. */
enum { QP_KEV_SLOT_DATA, QP_KEV_NSLOTS };
/* kqueue instances: the control fd (-1 = closed) and the pid that owns it
   (CPython invalidates open kqueues in the child after fork). */
enum { QP_KQ_SLOT_FD, QP_KQ_SLOT_PID, QP_KQ_NSLOTS };
#define QP_SEL_KEVENT_TYPE_PROP "\x01qp_select_kevent_type"

enum { QP_KEV_IDENT, QP_KEV_FILTER, QP_KEV_FLAGS, QP_KEV_FFLAGS, QP_KEV_DATA, QP_KEV_UDATA };

static const QPTypeSpec qp_kevent_spec;
static const QPTypeSpec qp_kqueue_spec;

static QPType *qp_kq_find(JSContext *ctx, JSValueConst v, const QPTypeSpec *spec)
{
    QPType *t;
    for (t = qp_type_of(ctx, v); t; t = t->base) {
        if (t->spec == spec)
            return t;
    }
    return NULL;
}

static struct kevent *qp_kev_ptr(JSContext *ctx, JSValueConst self)
{
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, qp_instance_slots(self)[QP_KEV_SLOT_DATA]);
    return (struct kevent *)p;
}

static JSValue qp_kev_alloc(JSContext *ctx, QPType *t, const struct kevent *e)
{
    JSValue obj, buf;
    buf = JS_NewArrayBufferCopy(ctx, (const uint8_t *)e, sizeof(*e));
    if (JS_IsException(buf))
        return buf;
    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, buf);
        return obj;
    }
    qp_instance_slots(obj)[QP_KEV_SLOT_DATA] = buf;
    return obj;
}

/* __index__ of v split for the C converters: *s = value when it fits int64
   (*range 0), else *range = -1 (below INT64_MIN), 1 (fits uint64) or 2
   (larger); *u = low 64 bits of the two's complement value in every case. */
static int qp_kev_int(JSContext *ctx, JSValueConst v, int64_t *s, uint64_t *u, int *range)
{
    JSValue idx = qp_index(ctx, v), lo, hi;
    int64_t x, l;
    int overflow, neg_hi;
    if (JS_IsException(idx))
        return -1;
    qp_long_as_int64_overflow(idx, &x, &overflow);
    if (!overflow) {
        JS_FreeValue(ctx, idx);
        *s = x;
        *u = (uint64_t)x;
        *range = 0;
        return 0;
    }
    {
        JSValue mlo = qp_long_from_int64(ctx, INT64_MAX);
        JSValue mhi = qp_long_from_uint64(ctx, (uint64_t)1 << 63);
        lo = JS_IsException(mlo) ? JS_EXCEPTION : qp_long_and_slot(ctx, idx, mlo);
        hi = JS_IsException(mhi) ? JS_EXCEPTION : qp_long_and_slot(ctx, idx, mhi);
        JS_FreeValue(ctx, mlo);
        JS_FreeValue(ctx, mhi);
    }
    if (JS_IsException(lo) || JS_IsException(hi)) {
        JS_FreeValue(ctx, lo);
        JS_FreeValue(ctx, hi);
        JS_FreeValue(ctx, idx);
        return -1;
    }
    qp_long_as_int64_overflow(lo, &l, &overflow);
    neg_hi = qp_long_sign(hi) != 0;
    JS_FreeValue(ctx, lo);
    JS_FreeValue(ctx, hi);
    *u = (uint64_t)l | (neg_hi ? (uint64_t)1 << 63 : 0);
    *s = -1;
    if (overflow < 0 || qp_long_sign(idx) < 0) {
        *range = -1;
    } else {
        JSValue m = qp_long_from_uint64(ctx, UINT64_MAX);
        *range = qp_long_compare(idx, m) <= 0 ? 1 : 2;
        JS_FreeValue(ctx, m);
    }
    JS_FreeValue(ctx, idx);
    return 0;
}

/* PyLong_AsLong */
static int qp_kev_as_long(JSContext *ctx, JSValueConst v, int64_t *out)
{
    uint64_t u;
    int range;
    if (qp_kev_int(ctx, v, out, &u, &range) < 0)
        return -1;
    if (range) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
        return -1;
    }
    return 0;
}

/* getargs 'H' / 'I' / 'K' (PyLong_AsNativeBytes into an nbytes unsigned
   buffer): DeprecationWarning when the value needs more than nbytes */
static int qp_kev_as_mask(JSContext *ctx, JSValueConst v, int nbytes, uint64_t *out)
{
    int64_t s;
    int range, out_of_range;
    if (qp_kev_int(ctx, v, &s, out, &range) < 0)
        return -1;
    if (nbytes == 8)
        out_of_range = range == -1 || range == 2;
    else
        out_of_range = range != 0 || s < -((int64_t)1 << (nbytes * 8 - 1)) ||
                       s > (int64_t)(((uint64_t)1 << (nbytes * 8)) - 1);
    if (out_of_range &&
        qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1, "integer value out of range") < 0)
        return -1;
    return 0;
}

/* PyMember_SetOne for the T_SHORT / T_USHORT / T_UINT / T_LONG / T_ULONG
   members of kevent */
static int qp_kev_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    struct kevent *e = qp_kev_ptr(ctx, self);
    int64_t s;
    uint64_t u;
    int range;
    const char *warn = NULL;
    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "can't delete numeric/char attribute");
        return -1;
    }
    if (!e)
        return -1;
    switch (magic) {
    case QP_KEV_FILTER:     /* T_SHORT */
        if (qp_kev_as_long(ctx, value, &s) < 0)
            return -1;
        e->filter = (short)s;
        if (s > SHRT_MAX || s < SHRT_MIN)
            warn = "Truncation of value to short";
        break;
    case QP_KEV_FLAGS:      /* T_USHORT */
        if (qp_kev_as_long(ctx, value, &s) < 0)
            return -1;
        e->flags = (unsigned short)s;
        if (s > USHRT_MAX || s < 0)
            warn = "Truncation of value to unsigned short";
        break;
    case QP_KEV_DATA:       /* T_LONGLONG (intptr_t) */
        if (qp_kev_int(ctx, value, &s, &u, &range) < 0)
            return -1;
        if (range) {
            qp_raise_overflow_error(ctx, "int too big to convert");
            return -1;
        }
        e->data = (intptr_t)s;
        break;
    default:                /* T_UINT (fflags), T_ULONGLONG (ident, udata) */
        if (qp_kev_int(ctx, value, &s, &u, &range) < 0)
            return -1;
        if (range < 0 || (range == 0 && s < 0)) {
            if (range) {
                qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
                return -1;
            }
            warn = "Writing negative value into unsigned field";
        } else if (range == 2) {
            qp_raise_overflow_error(ctx, magic == QP_KEV_FFLAGS ?
                                    "Python int too large to convert to C unsigned long" :
                                    "int too big to convert");
            return -1;
        } else if (magic == QP_KEV_FFLAGS && u > UINT_MAX) {
            warn = "Truncation of value to unsigned int";
        }
        if (magic == QP_KEV_FFLAGS)
            e->fflags = (unsigned int)u;
        else if (magic == QP_KEV_IDENT)
            e->ident = (uintptr_t)u;
        else
            e->udata = (void *)(uintptr_t)u;
        break;
    }
    if (warn && qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1, "%s", warn) < 0)
        return -1;
    return 0;
}

static JSValue qp_kev_get(JSContext *ctx, JSValueConst self, int magic)
{
    struct kevent *e = qp_kev_ptr(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    switch (magic) {
    case QP_KEV_IDENT: return qp_long_from_uint64(ctx, (uint64_t)e->ident);
    case QP_KEV_FILTER: return qp_long_from_int64(ctx, e->filter);
    case QP_KEV_FLAGS: return qp_long_from_int64(ctx, e->flags);
    case QP_KEV_FFLAGS: return qp_long_from_int64(ctx, e->fflags);
    case QP_KEV_DATA: return qp_long_from_int64(ctx, (int64_t)e->data);
    default: return qp_long_from_uint64(ctx, (uint64_t)(uintptr_t)e->udata);
    }
}

static JSValue qp_kev_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    struct kevent e;
    memset(&e, 0, sizeof(e));
    return qp_kev_alloc(ctx, subtype, &e);
}

/* kqueue_event_init: "O|hHILK:kevent" */
static int qp_kev_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                       size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "ident", "filter", "flags", "fflags",
                                          "data", "udata", NULL };
    static const QPArgSpec spec = { "kevent", kwlist, 1, 6, 0 };
    JSValueConst a[6];
    struct kevent *e = qp_kev_ptr(ctx, self);
    int64_t s;
    uint64_t u;
    int range;
    if (!e)
        return -1;
    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return -1;
    EV_SET(e, 0, EVFILT_READ, EV_ADD, 0, 0, 0); /* defaults */
    if (!QP_IS_NULL(a[1])) {
        if (qp_kev_as_long(ctx, a[1], &s) < 0)
            return -1;
        if (s < SHRT_MIN) {
            qp_raise_overflow_error(ctx, "signed short integer is less than minimum");
            return -1;
        }
        if (s > SHRT_MAX) {
            qp_raise_overflow_error(ctx, "signed short integer is greater than maximum");
            return -1;
        }
        e->filter = (short)s;
    }
    if (!QP_IS_NULL(a[2])) {
        if (qp_kev_as_mask(ctx, a[2], sizeof(e->flags), &u) < 0)
            return -1;
        e->flags = (unsigned short)u;
    }
    if (!QP_IS_NULL(a[3])) {
        if (qp_kev_as_mask(ctx, a[3], sizeof(e->fflags), &u) < 0)
            return -1;
        e->fflags = (unsigned int)u;
    }
    if (!QP_IS_NULL(a[4])) {
        if (qp_kev_int(ctx, a[4], &s, &u, &range) < 0)
            return -1;
        if (range) {
            qp_raise_overflow_error(ctx, "int too big to convert");
            return -1;
        }
        e->data = (intptr_t)s;
    }
    if (!QP_IS_NULL(a[5])) {
        if (!qp_index_check(ctx, a[5])) {
            qp_raise_type_error(ctx, "kevent() argument 6 must be int, not %T", a[5]);
            return -1;
        }
        if (qp_kev_as_mask(ctx, a[5], 8, &u) < 0)
            return -1;
        e->udata = (void *)(uintptr_t)u;
    }
    if (qp_index_check(ctx, a[0])) {
        /* PyLong_AsNativeBytes(..., REJECT_NEGATIVE | UNSIGNED_BUFFER) */
        if (qp_kev_int(ctx, a[0], &s, &u, &range) < 0)
            return -1;
        if (range < 0 || (range == 0 && s < 0)) {
            qp_raise_value_error(ctx, "Cannot convert negative int");
            return -1;
        }
        if (range == 2 || u > UINTPTR_MAX) {
            qp_raise_overflow_error(ctx, "Python int too large for C kqueue event identifier");
            return -1;
        }
        e->ident = (uintptr_t)u;
    } else {
        int fd;
        if (qp_object_as_fd(ctx, a[0], &fd) < 0)
            return -1;
        e->ident = (uintptr_t)fd;
    }
    return 0;
}

static JSValue qp_kev_repr(JSContext *ctx, JSValueConst self)
{
    struct kevent *e = qp_kev_ptr(ctx, self);
    char buf[256], p[32];
    if (!e)
        return JS_EXCEPTION;
    snprintf(p, sizeof(p), "%p", (void *)e->udata);
    snprintf(buf, sizeof(buf),
             "<select.kevent ident=%zu filter=%d flags=0x%x fflags=0x%x "
             "data=0x%llx udata=%s%s>",
             (size_t)e->ident, (int)e->filter, (unsigned int)e->flags,
             (unsigned int)e->fflags, (long long)e->data,
             (p[0] == '0' && p[1] == 'x') ? "" : "0x", p);
    return qp_str_from_format(ctx, "%s", buf);
}

static JSValue qp_kev_richcompare(JSContext *ctx, JSValueConst lhs, JSValueConst rhs, int op)
{
    struct kevent *s, *o;
    int result, r;
    if (!qp_kq_find(ctx, rhs, &qp_kevent_spec))
        return qp_not_implemented(ctx);
    s = qp_kev_ptr(ctx, lhs);
    o = qp_kev_ptr(ctx, rhs);
    if (!s || !o)
        return JS_EXCEPTION;
#define QP_KEV_CMP(a, b) ((a) != (b)) ? ((a) < (b) ? -1 : 1)
    result = QP_KEV_CMP(s->ident, o->ident)
           : QP_KEV_CMP(s->filter, o->filter)
           : QP_KEV_CMP(s->flags, o->flags)
           : QP_KEV_CMP(s->fflags, o->fflags)
           : QP_KEV_CMP(s->data, o->data)
           : QP_KEV_CMP((intptr_t)s->udata, (intptr_t)o->udata)
           : 0;
#undef QP_KEV_CMP
    switch (op) {
    case QP_CMP_LT: r = result < 0; break;
    case QP_CMP_LE: r = result <= 0; break;
    case QP_CMP_EQ: r = result == 0; break;
    case QP_CMP_NE: r = result != 0; break;
    case QP_CMP_GT: r = result > 0; break;
    default: r = result >= 0; break;
    }
    return qp_bool_new(r);
}

static const QPGetSetDef qp_kev_getsets[] = {
    { "ident", qp_kev_get, qp_kev_set, QP_KEV_IDENT, NULL },
    { "filter", qp_kev_get, qp_kev_set, QP_KEV_FILTER, NULL },
    { "flags", qp_kev_get, qp_kev_set, QP_KEV_FLAGS, NULL },
    { "fflags", qp_kev_get, qp_kev_set, QP_KEV_FFLAGS, NULL },
    { "data", qp_kev_get, qp_kev_set, QP_KEV_DATA, NULL },
    { "udata", qp_kev_get, qp_kev_set, QP_KEV_UDATA, NULL },
    { NULL }
};

static const QPTypeSpec qp_kevent_spec = {
    .name = "select.kevent",
    .doc = "kevent(ident, filter=KQ_FILTER_READ, flags=KQ_EV_ADD, fflags=0, data=0, udata=0)\n\
\n\
This object is the equivalent of the struct kevent for the C API.\n\
\n\
See the kqueue manpage for more detailed information about the meaning\n\
of the arguments.\n\
\n\
One minor note: while you might hope that udata could store a\n\
reference to a python object, it cannot, because it is impossible to\n\
keep a proper reference count of the object once it's passed into the\n\
kernel. Therefore, I have restricted it to only storing an integer.  I\n\
recommend ignoring it and simply using the 'ident' field to key off\n\
of. You could also set up a dictionary on the python side to store a\n\
udata->object mapping.",
    .flags = QP_TPFLAGS_DEFAULT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_KEV_NSLOTS,
    .slots = {
        .tp_repr = qp_kev_repr,
        .tp_richcompare = qp_kev_richcompare,
        .tp_init = qp_kev_init,
        .tp_new = qp_kev_new,
    },
    .getsets = qp_kev_getsets,
};

static JSValue qp_kq_err_closed(JSContext *ctx)
{
    return qp_raise_value_error(ctx, "I/O operation on closed kqueue object");
}

/* the control fd, or -1 when closed or inherited across fork() (the
   after_in_child invalidation of kqueue_tracking_after_fork) */
static int qp_kq_fd(JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    int fd = JS_VALUE_GET_INT(slots[QP_KQ_SLOT_FD]);
    if (fd >= 0 && JS_VALUE_GET_INT(slots[QP_KQ_SLOT_PID]) != (int)getpid()) {
        slots[QP_KQ_SLOT_FD] = JS_NewInt32(NULL, -1);
        fd = -1;
    }
    return fd;
}

/* kqueue_queue_internal_close: 0 or the errno of close() */
static int qp_kq_internal_close(JSContext *ctx, JSValueConst self)
{
    int save_errno = 0, kqfd = qp_kq_fd(self);
    if (kqfd >= 0) {
        qp_instance_slots(self)[QP_KQ_SLOT_FD] = JS_NewInt32(ctx, -1);
        QP_BEGIN_ALLOW_THREADS(ctx)
        if (close(kqfd) < 0)
            save_errno = errno;
        QP_END_ALLOW_THREADS(ctx)
    }
    return save_errno;
}

/* newKqueue_Object */
static JSValue qp_kq_new_object(JSContext *ctx, QPType *type, int fd)
{
    JSValue obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_OBJECT);
    JSValue *slots;
    int kqfd = fd, err = 0;
    if (JS_IsException(obj))
        return obj;
    slots = qp_instance_slots(obj);
    slots[QP_KQ_SLOT_FD] = JS_NewInt32(ctx, -1);
    slots[QP_KQ_SLOT_PID] = JS_NewInt32(ctx, (int)getpid());
    if (fd == -1) {
        QP_BEGIN_ALLOW_THREADS(ctx)
        kqfd = kqueue();
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
    }
    if (kqfd < 0) {
        JS_FreeValue(ctx, obj);
        return qp_raise_os_error(ctx, fd == -1 ? err : EBADF, QP_NULL);
    }
    slots[QP_KQ_SLOT_FD] = JS_NewInt32(ctx, kqfd);
    if (fd == -1 && qp_os_set_inheritable(kqfd, 0) < 0) {
        err = errno;
        JS_FreeValue(ctx, obj);
        return qp_raise_os_error(ctx, err, QP_NULL);
    }
    return obj;
}

static JSValue qp_kq_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                         size_t nargsf, JSValueConst kwnames)
{
    if (qp_nargs(nargsf) > 0)
        return qp_raise_type_error(ctx, "kqueue() takes no positional arguments");
    if (qp_arg_no_keywords(ctx, "kqueue", kwnames) < 0)
        return JS_EXCEPTION;
    return qp_kq_new_object(ctx, subtype, -1);
}

/* kqueue_queue_finalize */
static JSValue qp_kq_del(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    (void)qp_kq_internal_close(ctx, self);
    return JS_UNDEFINED;
}

static JSValue qp_kq_close(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    int err = qp_kq_internal_close(ctx, self);
    if (err != 0)
        return qp_raise_os_error(ctx, err, QP_NULL);
    return JS_UNDEFINED;
}

static JSValue qp_kq_get_closed(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_bool_new(qp_kq_fd(self) < 0);
}

static JSValue qp_kq_fileno(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    int fd = qp_kq_fd(self);
    if (fd < 0)
        return qp_kq_err_closed(ctx);
    return JS_NewInt32(ctx, fd);
}

static JSValue qp_kq_fromfd(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_from_value(self);
    int fd;
    if (!t)
        return qp_raise_system_error(ctx, "kqueue.fromfd: bad type");
    if (qp_arg_int(ctx, args[0], &fd) < 0)
        return JS_EXCEPTION;
    return qp_kq_new_object(ctx, t, fd);
}

static JSValue qp_kq_control(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst changelist, otimeout;
    QPType *kq_type, *kev_type;
    JSValue tv, seq = JS_UNDEFINED, result = JS_UNDEFINED;
    struct kevent *chl = NULL, *evl = NULL;
    struct timespec timeoutspec, *ptimeoutspec = NULL;
    double timeout = 0, deadline = 0;
    int maxevents, nchanges = 0, gotevents = 0, err = 0, i, kqfd;

    if (qp_arg_check_positional(ctx, "control", nargs, 2, 3) < 0)
        return JS_EXCEPTION;
    changelist = args[0];
    if (qp_arg_int(ctx, args[1], &maxevents) < 0)
        return JS_EXCEPTION;
    otimeout = nargs > 2 ? args[2] : JS_NULL;
    kq_type = qp_kq_find(ctx, self, &qp_kqueue_spec);
    if (!kq_type)
        return qp_raise_system_error(ctx, "select.kqueue type missing");
    tv = JS_GetPropertyStr(ctx, QP_TYPE_VALUE(kq_type), QP_SEL_KEVENT_TYPE_PROP);
    if (JS_IsException(tv))
        return tv;
    kev_type = qp_type_from_value(tv);
    JS_FreeValue(ctx, tv);  /* the kqueue type keeps it alive */
    if (!kev_type)
        return qp_raise_system_error(ctx, "select.kevent type missing");

    if (qp_kq_fd(self) < 0)
        return qp_kq_err_closed(ctx);
    if (maxevents < 0)
        return qp_raise_value_error(ctx, "Length of eventlist must be 0 or positive, got %d",
                                    maxevents);
    if (!JS_IsNull(otimeout) && !JS_IsUndefined(otimeout)) {
        double sec;
        if (qp_sel_time_object(ctx, otimeout, &timeout) < 0) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                qp_raise_type_error(ctx, "timeout must be a real number or None, not %T", otimeout);
            }
            return JS_EXCEPTION;
        }
        /* _PyTime_ROUND_TIMEOUT to nanoseconds, then _PyTime_AsTimespec */
        timeout = ceil(timeout * 1e9) / 1e9;
        sec = floor(timeout);
        if (sec < 0)
            return qp_raise_value_error(ctx, "timeout must be positive or None");
        timeoutspec.tv_sec = (time_t)sec;
        timeoutspec.tv_nsec = (long)((timeout - sec) * 1e9 + 0.5);
        if (timeoutspec.tv_nsec >= 1000000000L) {
            timeoutspec.tv_sec++;
            timeoutspec.tv_nsec -= 1000000000L;
        }
        ptimeoutspec = &timeoutspec;
    }
    if (!JS_IsNull(changelist) && !JS_IsUndefined(changelist)) {
        const JSValue *items;
        Py_ssize_t n;
        seq = qp_lst_sequence_fast(ctx, changelist, "changelist is not iterable");
        if (JS_IsException(seq))
            return seq;
        if (qp_lst_seq_items(ctx, seq, &items, &n) < 0)
            goto error;
        if (n > INT_MAX) {
            qp_raise_overflow_error(ctx, "changelist is too long");
            goto error;
        }
        nchanges = (int)n;
        chl = js_malloc(ctx, sizeof(struct kevent) * (nchanges ? nchanges : 1));
        if (!chl)
            goto error;
        for (i = 0; i < nchanges; i++) {
            struct kevent *e;
            if (!qp_kq_find(ctx, items[i], &qp_kevent_spec)) {
                qp_raise_type_error(ctx, "changelist must be an iterable of select.kevent objects");
                goto error;
            }
            e = qp_kev_ptr(ctx, items[i]);
            if (!e)
                goto error;
            chl[i] = *e;
        }
        JS_FreeValue(ctx, seq);
        seq = JS_UNDEFINED;
    }
    /* event list */
    if (maxevents) {
        evl = js_malloc(ctx, sizeof(struct kevent) * maxevents);
        if (!evl)
            goto error;
    }
    if (ptimeoutspec)
        deadline = qp_sel_monotonic() + timeout;
    for (;;) {
        kqfd = qp_kq_fd(self);
        if (kqfd < 0) {
            qp_kq_err_closed(ctx);
            goto error;
        }
        QP_BEGIN_ALLOW_THREADS(ctx)
        errno = 0;
        gotevents = kevent(kqfd, chl, nchanges, evl, maxevents, ptimeoutspec);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
        if (err != EINTR)
            break;
        /* kevent() was interrupted by a signal */
        if (qp_signal_check_signals(ctx) < 0)
            goto error;
        if (ptimeoutspec) {
            double rem = deadline - qp_sel_monotonic(), sec;
            if (rem < 0) {
                gotevents = 0;
                break;
            }
            /* retry kevent() with the recomputed timeout */
            sec = floor(rem);
            timeoutspec.tv_sec = (time_t)sec;
            timeoutspec.tv_nsec = (long)ceil((rem - sec) * 1e9);
            if (timeoutspec.tv_nsec >= 1000000000L) {
                timeoutspec.tv_sec++;
                timeoutspec.tv_nsec -= 1000000000L;
            }
        }
    }
    if (gotevents == -1) {
        qp_raise_os_error(ctx, err, QP_NULL);
        goto error;
    }
    result = qp_list_new(ctx, 0);
    if (JS_IsException(result))
        goto error;
    for (i = 0; i < gotevents; i++) {
        JSValue ch = qp_kev_alloc(ctx, kev_type, &evl[i]);
        int r;
        if (JS_IsException(ch))
            goto error;
        r = qp_list_append(ctx, result, ch);
        JS_FreeValue(ctx, ch);
        if (r < 0)
            goto error;
    }
    js_free(ctx, chl);
    js_free(ctx, evl);
    return result;
error:
    js_free(ctx, chl);
    js_free(ctx, evl);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, seq);
    return JS_EXCEPTION;
}

static const QPMethodDef qp_kq_methods[] = {
    { "fromfd", qp_kq_fromfd, QP_METH_O | QP_METH_CLASS, 0, "fromfd($type, fd, /)\n"
"--\n"
"\n"
"Create a kqueue object from a given control fd." },
    { "close", qp_kq_close, QP_METH_NOARGS, 0, "close($self, /)\n"
"--\n"
"\n"
"Close the kqueue control file descriptor.\n"
"\n"
"Further operations on the kqueue object will raise an exception." },
    { "fileno", qp_kq_fileno, QP_METH_NOARGS, 0, "fileno($self, /)\n"
"--\n"
"\n"
"Return the kqueue control file descriptor." },
    { "control", qp_kq_control, QP_METH_FASTCALL, 0, "control($self, changelist, maxevents, timeout=None, /)\n"
"--\n"
"\n"
"Calls the kernel kevent function.\n"
"\n"
"  changelist\n"
"    Must be an iterable of kevent objects describing the changes to be made\n"
"    to the kernel\'s watch list or None.\n"
"  maxevents\n"
"    The maximum number of events that the kernel will return.\n"
"  timeout\n"
"    The maximum time to wait in seconds, or else None to wait forever.\n"
"    This accepts non-integers for smaller timeouts, too." },
    { "__del__", qp_kq_del, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPGetSetDef qp_kq_getsets[] = {
    { "closed", qp_kq_get_closed, NULL, 0, "True if the kqueue handler is closed" },
    { NULL }
};

static const QPTypeSpec qp_kqueue_spec = {
    .name = "select.kqueue",
    .doc = "kqueue()\n"
"--\n"
"\n"
"Kqueue syscall wrapper.\n"
"\n"
"For example, to start watching a socket for input:\n"
">>> kq = kqueue()\n"
">>> sock = socket()\n"
">>> sock.connect((host, port))\n"
">>> kq.control([kevent(sock, KQ_FILTER_WRITE, KQ_EV_ADD)], 0)\n"
"\n"
"To wait one second for it to become writeable:\n"
">>> kq.control(None, 1, 1000)\n"
"\n"
"To stop listening:\n"
">>> kq.control([kevent(sock, KQ_FILTER_WRITE, KQ_EV_DELETE)], 0)",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_HAS_DEL,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_KQ_NSLOTS,
    .slots = {
        .tp_new = qp_kq_new,
    },
    .methods = qp_kq_methods,
    .getsets = qp_kq_getsets,
};
#endif /* QP_HAVE_KQUEUE */

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
#ifdef QP_HAVE_KQUEUE
    {
        JSValue kev, kq;
        kev = qp_type_from_spec(ctx, &qp_kevent_spec);
        if (JS_IsException(kev))
            return -1;
        if (qp_sel_add(ctx, module, "kevent", JS_DupValue(ctx, kev)) < 0) {
            JS_FreeValue(ctx, kev);
            return -1;
        }
        kq = qp_type_from_spec(ctx, &qp_kqueue_spec);
        if (JS_IsException(kq)) {
            JS_FreeValue(ctx, kev);
            return -1;
        }
        if (JS_DefinePropertyValueStr(ctx, kq, QP_SEL_KEVENT_TYPE_PROP, kev, 0) < 0 ||
            qp_sel_add(ctx, module, "kqueue", kq) < 0)
            return -1;
    }
#define QP_KQ_INT(NAME, VAL) \
    do { if (qp_sel_add(ctx, module, NAME, qp_long_from_int64(ctx, (long)(VAL))) < 0) return -1; } while (0)
    /* event filters */
    QP_KQ_INT("KQ_FILTER_READ", EVFILT_READ);
    QP_KQ_INT("KQ_FILTER_WRITE", EVFILT_WRITE);
#ifdef EVFILT_AIO
    QP_KQ_INT("KQ_FILTER_AIO", EVFILT_AIO);
#endif
#ifdef EVFILT_VNODE
    QP_KQ_INT("KQ_FILTER_VNODE", EVFILT_VNODE);
#endif
#ifdef EVFILT_PROC
    QP_KQ_INT("KQ_FILTER_PROC", EVFILT_PROC);
#endif
#ifdef EVFILT_NETDEV
    QP_KQ_INT("KQ_FILTER_NETDEV", EVFILT_NETDEV);
#endif
#ifdef EVFILT_SIGNAL
    QP_KQ_INT("KQ_FILTER_SIGNAL", EVFILT_SIGNAL);
#endif
    QP_KQ_INT("KQ_FILTER_TIMER", EVFILT_TIMER);
    /* event flags */
    QP_KQ_INT("KQ_EV_ADD", EV_ADD);
    QP_KQ_INT("KQ_EV_DELETE", EV_DELETE);
    QP_KQ_INT("KQ_EV_ENABLE", EV_ENABLE);
    QP_KQ_INT("KQ_EV_DISABLE", EV_DISABLE);
    QP_KQ_INT("KQ_EV_ONESHOT", EV_ONESHOT);
    QP_KQ_INT("KQ_EV_CLEAR", EV_CLEAR);
#ifdef EV_SYSFLAGS
    QP_KQ_INT("KQ_EV_SYSFLAGS", EV_SYSFLAGS);
#endif
#ifdef EV_FLAG1
    QP_KQ_INT("KQ_EV_FLAG1", EV_FLAG1);
#endif
    QP_KQ_INT("KQ_EV_EOF", EV_EOF);
    QP_KQ_INT("KQ_EV_ERROR", EV_ERROR);
    /* READ WRITE filter flag */
#ifdef NOTE_LOWAT
    QP_KQ_INT("KQ_NOTE_LOWAT", NOTE_LOWAT);
#endif
    /* VNODE filter flags  */
#ifdef EVFILT_VNODE
    QP_KQ_INT("KQ_NOTE_DELETE", NOTE_DELETE);
    QP_KQ_INT("KQ_NOTE_WRITE", NOTE_WRITE);
    QP_KQ_INT("KQ_NOTE_EXTEND", NOTE_EXTEND);
    QP_KQ_INT("KQ_NOTE_ATTRIB", NOTE_ATTRIB);
    QP_KQ_INT("KQ_NOTE_LINK", NOTE_LINK);
    QP_KQ_INT("KQ_NOTE_RENAME", NOTE_RENAME);
    QP_KQ_INT("KQ_NOTE_REVOKE", NOTE_REVOKE);
#endif
    /* PROC filter flags  */
#ifdef EVFILT_PROC
    QP_KQ_INT("KQ_NOTE_EXIT", NOTE_EXIT);
    QP_KQ_INT("KQ_NOTE_FORK", NOTE_FORK);
    QP_KQ_INT("KQ_NOTE_EXEC", NOTE_EXEC);
    QP_KQ_INT("KQ_NOTE_PCTRLMASK", NOTE_PCTRLMASK);
    QP_KQ_INT("KQ_NOTE_PDATAMASK", NOTE_PDATAMASK);
    QP_KQ_INT("KQ_NOTE_TRACK", NOTE_TRACK);
    QP_KQ_INT("KQ_NOTE_CHILD", NOTE_CHILD);
    QP_KQ_INT("KQ_NOTE_TRACKERR", NOTE_TRACKERR);
#endif
    /* NETDEV filter flags */
#ifdef EVFILT_NETDEV
    QP_KQ_INT("KQ_NOTE_LINKUP", NOTE_LINKUP);
    QP_KQ_INT("KQ_NOTE_LINKDOWN", NOTE_LINKDOWN);
    QP_KQ_INT("KQ_NOTE_LINKINV", NOTE_LINKINV);
#endif
#undef QP_KQ_INT
#endif /* QP_HAVE_KQUEUE */
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
