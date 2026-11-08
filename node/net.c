/*
 * qbun:net native binding: libuv stream handles (TCP, Pipe, TTY) for
 * node:net, node:tty, child_process stdio and process.stdin, plus
 * getaddrinfo/getnameinfo for node:dns.
 *
 * A StreamHandle object owns a QBStream. Like the Timer of node/core.c,
 * the struct is freed when both the libuv handle is closed and the JS
 * object is collected; the object is held strongly ('self') while the
 * handle reads, listens, has pending requests or is closing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "qbun.h"

#define QB_READ_SIZE (64 * 1024)

enum {
    QB_KIND_TCP,
    QB_KIND_PIPE,
    QB_KIND_TTY,
};

static JSClassID qb_stream_class_id;

typedef struct QBStream {
    union {
        uv_handle_t handle;
        uv_stream_t stream;
        uv_tcp_t tcp;
        uv_pipe_t pipe;
        uv_tty_t tty;
    } h;
    QBRuntime *qr;
    int kind;
    int ipc;
    JSValue onread;
    JSValue onconnection;
    JSValue close_cb;
    JSValue self;  /* strong while reading/listening/requests/closing */
    uint8_t *rbuf; /* read buffer (js_malloc_rt), reused between reads */
    int pending;   /* write/shutdown/connect requests in flight */
    int reading;
    int listening;
    int closed; /* uv_close() called */
    int close_done;
    int js_dead;
    int bind_port;
    char bind_addr[256];
} QBStream;

/* ---------------------------------------------------------------- errors */

/* a network error like Node's ExceptionWithHostPort (uv_style = 0:
   "connect ECONNREFUSED 127.0.0.1:80") or UVExceptionWithHostPort
   (uv_style = 1: "listen EADDRINUSE: address already in use :::80") */
static JSValue qb_net_error(JSContext *ctx, int err, const char *syscall, const char *address,
                            int port, int uv_style)
{
    char msg[1024];
    const char *code = uv_err_name(err);
    JSValue e;
    int n;

    if (uv_style)
        n = snprintf(msg, sizeof(msg), "%s %s: %s", syscall, code, uv_strerror(err));
    else
        n = snprintf(msg, sizeof(msg), "%s %s", syscall, code);
    if (address && *address && n < (int)sizeof(msg)) {
        if (port > 0)
            n += snprintf(msg + n, sizeof(msg) - n, " %s:%d", address, port);
        else
            n += snprintf(msg + n, sizeof(msg) - n, " %s", address);
    }
    if (n >= (int)sizeof(msg))
        n = sizeof(msg) - 1;
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, n);
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    if (address && *address)
        JS_DefinePropertyValueStr(ctx, e, "address", JS_NewString(ctx, address), JS_PROP_C_W_E);
    if (port > 0)
        JS_DefinePropertyValueStr(ctx, e, "port", JS_NewInt32(ctx, port), JS_PROP_C_W_E);
    return e;
}

static JSValue qb_throw_net(JSContext *ctx, int err, const char *syscall, const char *address,
                            int port, int uv_style)
{
    JSValue e = qb_net_error(ctx, err, syscall, address, port, uv_style);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

/* a DNS error like Node's DNSException: "getaddrinfo ENOTFOUND host" */
static JSValue qb_dns_error(JSContext *ctx, int err, const char *syscall, const char *hostname)
{
    char msg[1024];
    const char *code;
    JSValue e;
    int n;

    if (err == UV_EAI_NODATA || err == UV_EAI_NONAME)
        code = "ENOTFOUND";
    else
        code = uv_err_name(err);
    if (hostname && *hostname)
        n = snprintf(msg, sizeof(msg), "%s %s %s", syscall, code, hostname);
    else
        n = snprintf(msg, sizeof(msg), "%s %s", syscall, code);
    if (n >= (int)sizeof(msg))
        n = sizeof(msg) - 1;
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, n);
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    if (hostname && *hostname)
        JS_DefinePropertyValueStr(ctx, e, "hostname", JS_NewString(ctx, hostname), JS_PROP_C_W_E);
    return e;
}

/* job: argv[0](...argv[2..]) with this = argv[1] */
static JSValue qb_net_job_call(JSContext *ctx, int argc, JSValueConst *argv)
{
    return JS_Call(ctx, argv[0], argv[1], argc - 2, argv + 2);
}

/* call 'cb' with 'err' (freed) from a microtask: the callbacks of the
   requests that fail synchronously still run asynchronously */
static void qb_defer_error(JSContext *ctx, JSValueConst cb, JSValueConst this_val, JSValue err)
{
    JSValueConst args[3];
    if (JS_IsException(err)) {
        qb_report_exception(ctx);
        return;
    }
    if (JS_IsFunction(ctx, cb)) {
        args[0] = cb;
        args[1] = this_val;
        args[2] = err;
        if (JS_EnqueueJob(ctx, qb_net_job_call, 3, args) < 0)
            qb_report_exception(ctx);
    }
    JS_FreeValue(ctx, err);
}

/* -------------------------------------------------------------- addresses */

/* a sockaddr from a numeric IPv4 or IPv6 address */
static int qb_parse_addr(const char *host, int port, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof(*ss));
    if (strchr(host, ':'))
        return uv_ip6_addr(host, port, (struct sockaddr_in6 *)ss);
    return uv_ip4_addr(host, port, (struct sockaddr_in *)ss);
}

/* { address, family, port } of a sockaddr, null for other families */
static JSValue qb_addr_object(JSContext *ctx, const struct sockaddr_storage *ss)
{
    char ip[INET6_ADDRSTRLEN + 1];
    const char *family;
    int port;
    JSValue obj;

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        uv_ip4_name(a, ip, sizeof(ip));
        port = ntohs(a->sin_port);
        family = "IPv4";
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        uv_ip6_name(a, ip, sizeof(ip));
        port = ntohs(a->sin6_port);
        family = "IPv6";
    } else {
        return JS_NULL;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (qb_set(ctx, obj, "address", JS_NewString(ctx, ip)) < 0 ||
        qb_set(ctx, obj, "family", JS_NewString(ctx, family)) < 0 ||
        qb_set(ctx, obj, "port", JS_NewInt32(ctx, port)) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* ---------------------------------------------------------- the handle */

static void qb_stream_free(QBStream *s)
{
    if (s->rbuf)
        js_free_rt(s->qr->rt, s->rbuf);
    free(s);
}

static void qb_stream_set_value(JSContext *ctx, JSValue *pv, JSValueConst v)
{
    JSValue old = *pv;
    *pv = JS_DupValue(ctx, v);
    JS_FreeValue(ctx, old);
}

/* take or drop the strong reference to the object; 'this_val' is the
   object when called from a method (JS_UNDEFINED from callbacks, which
   only drop it). Callbacks keep their own reference while they run. */
static void qb_stream_update(QBStream *s, JSValueConst this_val)
{
    JSContext *ctx = s->qr->ctx;
    int want = s->reading || s->listening || s->pending > 0 || (s->closed && !s->close_done);
    if (want) {
        if (JS_IsUndefined(s->self) && JS_IsObject(this_val))
            s->self = JS_DupValue(ctx, this_val);
    } else if (!JS_IsUndefined(s->self)) {
        JSValue v = s->self;
        s->self = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static void qb_stream_close_cb(uv_handle_t *h)
{
    QBStream *s = h->data;
    JSContext *ctx;
    JSValue self, cb, ret;

    s->close_done = 1;
    if (s->js_dead) {
        qb_stream_free(s);
        return;
    }
    ctx = s->qr->ctx;
    self = JS_DupValue(ctx, s->self);
    cb = s->close_cb;
    s->close_cb = JS_UNDEFINED;
    if (JS_IsFunction(ctx, cb)) {
        ret = qb_call(ctx, cb, self, 0, NULL);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    qb_stream_update(s, JS_UNDEFINED);
    JS_FreeValue(ctx, self);
}

static void qb_stream_finalizer(JSRuntime *rt, JSValue val)
{
    QBStream *s = JS_GetOpaque(val, qb_stream_class_id);
    if (!s)
        return;
    JS_FreeValueRT(rt, s->onread);
    JS_FreeValueRT(rt, s->onconnection);
    JS_FreeValueRT(rt, s->close_cb);
    s->onread = JS_UNDEFINED;
    s->onconnection = JS_UNDEFINED;
    s->close_cb = JS_UNDEFINED;
    s->js_dead = 1;
    if (!s->closed) {
        s->closed = 1;
        uv_close(&s->h.handle, qb_stream_close_cb);
    } else if (s->close_done) {
        qb_stream_free(s);
    }
}

static void qb_stream_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBStream *s = JS_GetOpaque(val, qb_stream_class_id);
    if (!s)
        return;
    JS_MarkValue(rt, s->onread, mark_func);
    JS_MarkValue(rt, s->onconnection, mark_func);
    JS_MarkValue(rt, s->close_cb, mark_func);
}

static JSClassDef qb_stream_class = {
    "StreamHandle",
    .finalizer = qb_stream_finalizer,
    .gc_mark = qb_stream_mark,
};

/* a new object with an uninitialized QBStream (the opaque is set by
   qb_stream_attach() once the libuv handle is initialized) */
static JSValue qb_stream_alloc(JSContext *ctx, int kind, int ipc, QBStream **ps)
{
    JSValue obj;
    QBStream *s;

    obj = JS_NewObjectClass(ctx, qb_stream_class_id);
    if (JS_IsException(obj))
        return obj;
    s = calloc(1, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->qr = qb_get(ctx);
    s->kind = kind;
    s->ipc = ipc;
    s->onread = JS_UNDEFINED;
    s->onconnection = JS_UNDEFINED;
    s->close_cb = JS_UNDEFINED;
    s->self = JS_UNDEFINED;
    *ps = s;
    return obj;
}

static void qb_stream_attach(JSValue obj, QBStream *s)
{
    s->h.handle.data = s;
    JS_SetOpaque(obj, s);
}

/* a new TCP or Pipe handle */
static JSValue qb_stream_new(JSContext *ctx, int kind, int ipc)
{
    QBStream *s;
    JSValue obj = qb_stream_alloc(ctx, kind, ipc, &s);
    int err;
    if (JS_IsException(obj))
        return obj;
    if (kind == QB_KIND_TCP)
        err = uv_tcp_init(s->qr->loop, &s->h.tcp);
    else
        err = uv_pipe_init(s->qr->loop, &s->h.pipe, ipc);
    if (err < 0) {
        free(s);
        JS_FreeValue(ctx, obj);
        return qb_throw_uv(ctx, err, kind == QB_KIND_TCP ? "uv_tcp_init" : "uv_pipe_init", NULL,
                           NULL);
    }
    qb_stream_attach(obj, s);
    return obj;
}

static QBStream *qb_stream_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_stream_class_id);
}

uv_stream_t *qb_stream_handle(JSContext *ctx, JSValueConst obj)
{
    QBStream *s = JS_GetOpaque(obj, qb_stream_class_id);
    if (!s) {
        JS_ThrowTypeError(ctx, "expected a stream handle");
        return NULL;
    }
    if (s->closed) {
        JS_ThrowTypeError(ctx, "the stream handle is closed");
        return NULL;
    }
    return &s->h.stream;
}

static QBStream *qb_stream_get_kind(JSContext *ctx, JSValueConst obj, int kind, const char *what)
{
    QBStream *s = qb_stream_get(ctx, obj);
    if (!s)
        return NULL;
    if (s->kind != kind) {
        JS_ThrowTypeError(ctx, "%s is only supported by %s handles", what,
                          kind == QB_KIND_TCP    ? "TCP"
                          : kind == QB_KIND_PIPE ? "Pipe"
                                                 : "TTY");
        return NULL;
    }
    return s;
}

/* ------------------------------------------------------------- reading */

static void qb_stream_alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
    QBStream *s = h->data;
    if (!s->rbuf)
        s->rbuf = js_malloc_rt(s->qr->rt, QB_READ_SIZE);
    buf->base = (char *)s->rbuf;
    buf->len = s->rbuf ? QB_READ_SIZE : 0;
}

static void qb_stream_read_cb(uv_stream_t *st, ssize_t nread, const uv_buf_t *buf)
{
    QBStream *s = st->data;
    JSContext *ctx = s->qr->ctx;
    JSValue args[2], self, cb, ret;
    int argc = 1;

    if (nread == 0 || s->js_dead)
        return;
    if (nread > 0) {
        if (nread >= QB_READ_SIZE / 2 && buf->base == (char *)s->rbuf) {
            /* a big chunk: the buffer becomes the Buffer's storage */
            s->rbuf = NULL;
            args[0] = qb_new_buffer_owned(ctx, (uint8_t *)buf->base, (size_t)nread);
        } else {
            args[0] = qb_new_buffer(ctx, (const uint8_t *)buf->base, (size_t)nread);
        }
        if (JS_IsException(args[0])) {
            qb_report_exception(ctx);
            return;
        }
    } else {
        /* end of stream or error: reading stops */
        uv_read_stop(st);
        s->reading = 0;
        args[0] = JS_NULL;
        if (nread != UV_EOF) {
            args[1] = qb_net_error(ctx, (int)nread, "read", NULL, 0, 0);
            if (JS_IsException(args[1]))
                args[1] = JS_UNDEFINED;
            argc = 2;
        }
    }
    self = JS_DupValue(ctx, s->self);
    cb = JS_DupValue(ctx, s->onread);
    if (JS_IsFunction(ctx, cb)) {
        ret = qb_call(ctx, cb, self, argc, (JSValueConst *)args);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, args[0]);
    if (argc > 1)
        JS_FreeValue(ctx, args[1]);
    qb_stream_update(s, JS_UNDEFINED);
    JS_FreeValue(ctx, self);
}

static JSValue qb_stream_readStart(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (s->closed)
        return JS_NewInt32(ctx, UV_EBADF);
    if (s->reading)
        return JS_NewInt32(ctx, 0);
    err = uv_read_start(&s->h.stream, qb_stream_alloc_cb, qb_stream_read_cb);
    if (err == 0 || err == UV_EALREADY) {
        err = 0;
        s->reading = 1;
        qb_stream_update(s, this_val);
    }
    return JS_NewInt32(ctx, err);
}

static JSValue qb_stream_readStop(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int err = 0;
    if (!s)
        return JS_EXCEPTION;
    if (s->reading) {
        if (!s->closed)
            err = uv_read_stop(&s->h.stream);
        s->reading = 0;
        qb_stream_update(s, this_val);
    }
    return JS_NewInt32(ctx, err);
}

static JSValue qb_stream_get_onread(JSContext *ctx, JSValueConst this_val)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (JS_IsUndefined(s->onread))
        return JS_NULL;
    return JS_DupValue(ctx, s->onread);
}

static JSValue qb_stream_set_onread(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (JS_IsFunction(ctx, val))
        qb_stream_set_value(ctx, &s->onread, val);
    else
        qb_stream_set_value(ctx, &s->onread, JS_UNDEFINED);
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------- writing */

typedef struct QBWriteReq {
    uv_write_t req;
    QBStream *s;
    JSValue cb;
    size_t len;
    uint8_t data[];
} QBWriteReq;

typedef struct QBChunk {
    const uint8_t *ptr;
    size_t len;
    const char *cstr; /* JS_ToCStringLen() of a string chunk */
} QBChunk;

static int qb_chunk_get(JSContext *ctx, JSValueConst v, QBChunk *c)
{
    c->cstr = NULL;
    if (JS_IsString(v)) {
        c->cstr = JS_ToCStringLen(ctx, &c->len, v);
        if (!c->cstr)
            return -1;
        c->ptr = (const uint8_t *)c->cstr;
        return 0;
    }
    c->ptr = qb_get_bytes(ctx, v, &c->len);
    return c->ptr ? 0 : -1;
}

static void qb_chunks_free(JSContext *ctx, QBChunk *chunks, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (chunks[i].cstr)
            JS_FreeCString(ctx, chunks[i].cstr);
    }
}

static void qb_write_cb(uv_write_t *req, int status)
{
    QBWriteReq *w = req->data;
    QBStream *s = w->s;
    JSContext *ctx = s->qr->ctx;
    JSValue self, arg, ret;

    s->pending--;
    self = JS_DupValue(ctx, s->self);
    if (status < 0)
        arg = qb_net_error(ctx, status, "write", NULL, 0, 0);
    else
        arg = JS_NULL;
    if (JS_IsException(arg)) {
        qb_report_exception(ctx);
        arg = JS_UNDEFINED;
    }
    if (JS_IsFunction(ctx, w->cb)) {
        ret = qb_call(ctx, w->cb, self, 1, (JSValueConst *)&arg);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, w->cb);
    free(w);
    qb_stream_update(s, JS_UNDEFINED);
    JS_FreeValue(ctx, self);
}

/* write the concatenation of 'chunks' (copied into the request) */
static JSValue qb_stream_write_chunks(JSContext *ctx, JSValueConst this_val, QBStream *s,
                                      QBChunk *chunks, uint32_t n, JSValueConst cb)
{
    QBWriteReq *w;
    size_t total = 0, off = 0;
    uv_buf_t b;
    uint32_t i;
    int err;

    for (i = 0; i < n; i++)
        total += chunks[i].len;
    w = malloc(sizeof(*w) + (total ? total : 1));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        if (chunks[i].len)
            memcpy(w->data + off, chunks[i].ptr, chunks[i].len);
        off += chunks[i].len;
    }
    w->s = s;
    w->len = total;
    w->req.data = w;
    if (s->closed) {
        err = UV_EBADF;
    } else {
        b.base = (char *)w->data;
        b.len = total;
        err = uv_write(&w->req, &s->h.stream, &b, 1, qb_write_cb);
    }
    if (err < 0) {
        free(w);
        qb_defer_error(ctx, cb, this_val, qb_net_error(ctx, err, "write", NULL, 0, 0));
        return JS_UNDEFINED;
    }
    w->cb = JS_DupValue(ctx, cb);
    s->pending++;
    qb_stream_update(s, this_val);
    return JS_UNDEFINED;
}

static JSValue qb_stream_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    QBChunk c;
    JSValue ret;
    if (!s)
        return JS_EXCEPTION;
    if (qb_chunk_get(ctx, argv[0], &c) < 0)
        return JS_EXCEPTION;
    ret = qb_stream_write_chunks(ctx, this_val, s, &c, 1, argc > 1 ? argv[1] : JS_UNDEFINED);
    qb_chunks_free(ctx, &c, 1);
    return ret;
}

static JSValue qb_stream_writev(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    JSValue *vals = NULL, ret = JS_EXCEPTION;
    QBChunk *chunks = NULL;
    int64_t len;
    uint32_t i, n = 0, got = 0;

    if (!s)
        return JS_EXCEPTION;
    if (!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "chunks must be an array");
    if (JS_GetLength(ctx, argv[0], &len) < 0)
        return JS_EXCEPTION;
    if (len < 0 || len > UINT32_MAX / 2)
        return JS_ThrowRangeError(ctx, "too many chunks");
    n = (uint32_t)len;
    vals = calloc(n ? n : 1, sizeof(*vals));
    chunks = calloc(n ? n : 1, sizeof(*chunks));
    if (!vals || !chunks) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    /* get every element first: the getters may run JS, after which the
       borrowed bytes are taken without running JS */
    for (i = 0; i < n; i++) {
        vals[i] = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(vals[i])) {
            vals[i] = JS_UNDEFINED;
            goto done;
        }
    }
    for (got = 0; got < n; got++) {
        if (qb_chunk_get(ctx, vals[got], &chunks[got]) < 0)
            goto done;
    }
    ret = qb_stream_write_chunks(ctx, this_val, s, chunks, n, argc > 1 ? argv[1] : JS_UNDEFINED);
done:
    if (chunks)
        qb_chunks_free(ctx, chunks, got);
    if (vals) {
        for (i = 0; i < n; i++)
            JS_FreeValue(ctx, vals[i]);
    }
    free(chunks);
    free(vals);
    return ret;
}

static JSValue qb_stream_tryWrite(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    QBChunk c;
    uv_buf_t b;
    int r;
    if (!s)
        return JS_EXCEPTION;
    if (qb_chunk_get(ctx, argv[0], &c) < 0)
        return JS_EXCEPTION;
    if (s->closed) {
        r = UV_EBADF;
    } else if (c.len == 0) {
        r = 0;
    } else {
        b.base = (char *)c.ptr;
        b.len = c.len;
        r = uv_try_write(&s->h.stream, &b, 1);
    }
    qb_chunks_free(ctx, &c, 1);
    return JS_NewInt32(ctx, r);
}

static JSValue qb_stream_get_writeQueueSize(JSContext *ctx, JSValueConst this_val)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (s->closed)
        return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, (double)uv_stream_get_write_queue_size(&s->h.stream));
}

static JSValue qb_stream_get_fd(JSContext *ctx, JSValueConst this_val)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    uv_os_fd_t fd;
    if (!s)
        return JS_EXCEPTION;
    if (s->closed || uv_fileno(&s->h.handle, &fd) < 0)
        return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, (int)fd);
}

/* ------------------------------------------- shutdown and connect requests */

typedef struct QBReq {
    union {
        uv_shutdown_t shutdown;
        uv_connect_t connect;
    } r;
    QBStream *s;
    JSValue cb;
    int port;
    char addr[256];
} QBReq;

static void qb_req_done(QBReq *q, int status, const char *syscall)
{
    QBStream *s = q->s;
    JSContext *ctx = s->qr->ctx;
    JSValue self, arg, ret;

    s->pending--;
    self = JS_DupValue(ctx, s->self);
    if (status < 0)
        arg = qb_net_error(ctx, status, syscall, q->addr, q->port, 0);
    else
        arg = JS_NULL;
    if (JS_IsException(arg)) {
        qb_report_exception(ctx);
        arg = JS_UNDEFINED;
    }
    if (JS_IsFunction(ctx, q->cb)) {
        ret = qb_call(ctx, q->cb, self, 1, (JSValueConst *)&arg);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, q->cb);
    free(q);
    qb_stream_update(s, JS_UNDEFINED);
    JS_FreeValue(ctx, self);
}

static void qb_shutdown_cb(uv_shutdown_t *req, int status)
{
    qb_req_done(req->data, status, "shutdown");
}

static void qb_connect_cb(uv_connect_t *req, int status)
{
    qb_req_done(req->data, status, "connect");
}

static QBReq *qb_req_new(JSContext *ctx, QBStream *s)
{
    QBReq *q = calloc(1, sizeof(*q));
    if (!q) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    q->s = s;
    q->cb = JS_UNDEFINED;
    return q;
}

/* the request started (err == 0) or failed synchronously */
static JSValue qb_req_started(JSContext *ctx, JSValueConst this_val, QBStream *s, QBReq *q, int err,
                              JSValueConst cb, const char *syscall)
{
    if (err < 0) {
        JSValue e = qb_net_error(ctx, err, syscall, q->addr, q->port, 0);
        free(q);
        qb_defer_error(ctx, cb, this_val, e);
        return JS_UNDEFINED;
    }
    q->cb = JS_DupValue(ctx, cb);
    s->pending++;
    qb_stream_update(s, this_val);
    return JS_UNDEFINED;
}

static JSValue qb_stream_shutdown(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    QBReq *q;
    int err;
    if (!s)
        return JS_EXCEPTION;
    q = qb_req_new(ctx, s);
    if (!q)
        return JS_EXCEPTION;
    q->r.shutdown.data = q;
    if (s->closed)
        err = UV_EBADF;
    else
        err = uv_shutdown(&q->r.shutdown, &s->h.stream, qb_shutdown_cb);
    return qb_req_started(ctx, this_val, s, q, err, argc > 0 ? argv[0] : JS_UNDEFINED, "shutdown");
}

static JSValue qb_stream_connect(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    const char *target;
    size_t target_len;
    int32_t port = 0;
    QBReq *q;
    int err;

    if (!s)
        return JS_EXCEPTION;
    if (s->kind == QB_KIND_TTY)
        return JS_ThrowTypeError(ctx, "connect is not supported by TTY handles");
    target = JS_ToCStringLen(ctx, &target_len, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    if (s->kind == QB_KIND_TCP && argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt32(ctx, &port, argv[1])) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    q = qb_req_new(ctx, s);
    if (!q) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    snprintf(q->addr, sizeof(q->addr), "%s", target);
    q->port = s->kind == QB_KIND_TCP ? port : 0;
    q->r.connect.data = q;
    if (s->closed) {
        err = UV_EBADF;
    } else if (s->kind == QB_KIND_TCP) {
        struct sockaddr_storage ss;
        err = qb_parse_addr(target, port, &ss);
        if (err == 0)
            err = uv_tcp_connect(&q->r.connect, &s->h.tcp, (const struct sockaddr *)&ss,
                                 qb_connect_cb);
    } else {
        err = uv_pipe_connect2(&q->r.connect, &s->h.pipe, target, target_len, 0, qb_connect_cb);
    }
    JS_FreeCString(ctx, target);
    return qb_req_started(ctx, this_val, s, q, err, argc > 2 ? argv[2] : JS_UNDEFINED, "connect");
}

/* --------------------------------------------------------------- servers */

static JSValue qb_stream_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    const char *target;
    size_t target_len;
    int32_t port = 0;
    int err, ipv6only = 0;

    if (!s)
        return JS_EXCEPTION;
    if (s->kind == QB_KIND_TTY)
        return JS_ThrowTypeError(ctx, "bind is not supported by TTY handles");
    if (s->closed)
        return qb_throw_uv(ctx, UV_EBADF, "bind", NULL, NULL);
    target = JS_ToCStringLen(ctx, &target_len, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    if (s->kind == QB_KIND_TCP) {
        struct sockaddr_storage ss;
        if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &port, argv[1]))
            goto exception;
        if (argc > 2)
            ipv6only = JS_ToBool(ctx, argv[2]) > 0;
        err = qb_parse_addr(target, port, &ss);
        if (err == 0)
            err = uv_tcp_bind(&s->h.tcp, (const struct sockaddr *)&ss,
                              (ipv6only && ss.ss_family == AF_INET6) ? UV_TCP_IPV6ONLY : 0);
        if (err < 0) {
            qb_throw_net(ctx, err, "bind", target, port, 0);
            goto exception;
        }
    } else {
        err = uv_pipe_bind2(&s->h.pipe, target, target_len, 0);
        if (err < 0) {
            /* pipes are bound by server.listen() in Node */
            qb_throw_net(ctx, err, "listen", target, 0, 1);
            goto exception;
        }
    }
    snprintf(s->bind_addr, sizeof(s->bind_addr), "%s", target);
    s->bind_port = port;
    JS_FreeCString(ctx, target);
    return JS_UNDEFINED;
exception:
    JS_FreeCString(ctx, target);
    return JS_EXCEPTION;
}

static void qb_connection_cb(uv_stream_t *server, int status)
{
    QBStream *s = server->data;
    JSContext *ctx = s->qr->ctx;
    JSValue args[2], self, cb, ret;
    int argc = 1;

    if (s->js_dead)
        return;
    args[1] = JS_UNDEFINED;
    if (status < 0) {
        args[0] = qb_net_error(ctx, status, "accept", NULL, 0, 0);
    } else {
        QBStream *c;
        JSValue client = qb_stream_new(ctx, s->kind, s->ipc);
        int err;
        if (JS_IsException(client)) {
            qb_report_exception(ctx);
            return;
        }
        c = JS_GetOpaque(client, qb_stream_class_id);
        err = uv_accept(server, &c->h.stream);
        if (err < 0) {
            JS_FreeValue(ctx, client);
            args[0] = qb_net_error(ctx, err, "accept", NULL, 0, 0);
        } else {
            args[0] = JS_NULL;
            args[1] = client;
            argc = 2;
        }
    }
    if (JS_IsException(args[0])) {
        qb_report_exception(ctx);
        args[0] = JS_UNDEFINED;
    }
    self = JS_DupValue(ctx, s->self);
    cb = JS_DupValue(ctx, s->onconnection);
    if (JS_IsFunction(ctx, cb)) {
        ret = qb_call(ctx, cb, self, argc, (JSValueConst *)args);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, self);
}

static JSValue qb_stream_listen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int32_t backlog = 511;
    int err;

    if (!s)
        return JS_EXCEPTION;
    if (s->kind == QB_KIND_TTY)
        return JS_ThrowTypeError(ctx, "listen is not supported by TTY handles");
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &backlog, argv[0]))
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "onconnection must be a function");
    if (s->closed)
        return qb_throw_net(ctx, UV_EBADF, "listen", s->bind_addr, s->bind_port, 1);
    qb_stream_set_value(ctx, &s->onconnection, argv[1]);
    err = uv_listen(&s->h.stream, backlog, qb_connection_cb);
    if (err < 0) {
        qb_stream_set_value(ctx, &s->onconnection, JS_UNDEFINED);
        return qb_throw_net(ctx, err, "listen", s->bind_addr, s->bind_port, 1);
    }
    s->listening = 1;
    qb_stream_update(s, this_val);
    return JS_UNDEFINED;
}

static JSValue qb_stream_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                              int peer)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (s->closed)
        return JS_NULL;
    if (s->kind == QB_KIND_TCP) {
        struct sockaddr_storage ss;
        int len = sizeof(ss);
        memset(&ss, 0, sizeof(ss));
        if (peer)
            err = uv_tcp_getpeername(&s->h.tcp, (struct sockaddr *)&ss, &len);
        else
            err = uv_tcp_getsockname(&s->h.tcp, (struct sockaddr *)&ss, &len);
        if (err < 0)
            return JS_NULL;
        return qb_addr_object(ctx, &ss);
    }
    /* pipes and TTYs have no IP address (node:net keeps the pipe path) */
    return JS_NULL;
}

/* ---------------------------------------------------------------- options */

/* TCP options: no-ops returning 0 on pipes and TTYs, whose sockets have no
   such option (net.Socket calls them on every connection of a server) */
static JSValue qb_stream_setNoDelay(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (s->kind != QB_KIND_TCP)
        return JS_NewInt32(ctx, 0);
    if (s->closed)
        return JS_NewInt32(ctx, UV_EBADF);
    return JS_NewInt32(ctx, uv_tcp_nodelay(&s->h.tcp, argc > 0 && JS_ToBool(ctx, argv[0]) > 0));
}

static JSValue qb_stream_setKeepAlive(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int32_t delay = 0;
    int enable;
    if (!s)
        return JS_EXCEPTION;
    if (s->kind != QB_KIND_TCP)
        return JS_NewInt32(ctx, 0);
    enable = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &delay, argv[1]))
        return JS_EXCEPTION;
    if (delay < 0)
        delay = 0;
    if (s->closed)
        return JS_NewInt32(ctx, UV_EBADF);
    /* like Node: a zero delay sets SO_KEEPALIVE with the system's idle time
       (libuv then reports EINVAL, ignored by the callers) */
    return JS_NewInt32(ctx, uv_tcp_keepalive(&s->h.tcp, enable, (unsigned int)delay));
}

static JSValue qb_stream_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    int32_t fd;
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (s->closed)
        return qb_throw_net(ctx, UV_EBADF, "open", NULL, 0, 0);
    if (s->kind == QB_KIND_PIPE)
        err = uv_pipe_open(&s->h.pipe, fd);
    else if (s->kind == QB_KIND_TCP)
        err = uv_tcp_open(&s->h.tcp, (uv_os_sock_t)fd);
    else
        return JS_ThrowTypeError(ctx, "open is not supported by TTY handles");
    if (err < 0)
        return qb_throw_net(ctx, err, "open", NULL, 0, 0);
    return JS_UNDEFINED;
}

static JSValue qb_stream_setRawMode(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    QBStream *s = qb_stream_get_kind(ctx, this_val, QB_KIND_TTY, "setRawMode");
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (s->closed)
        return qb_throw_net(ctx, UV_EBADF, "setRawMode", NULL, 0, 0);
    err =
        uv_tty_set_mode(&s->h.tty, (argc > 0 && JS_ToBool(ctx, argv[0]) > 0) ? UV_TTY_MODE_RAW
                                                                             : UV_TTY_MODE_NORMAL);
    if (err < 0)
        return qb_throw_net(ctx, err, "setRawMode", NULL, 0, 0);
    return JS_UNDEFINED;
}

static JSValue qb_stream_getWindowSize(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBStream *s = qb_stream_get_kind(ctx, this_val, QB_KIND_TTY, "getWindowSize");
    JSValue v[2];
    int w, h;
    if (!s)
        return JS_EXCEPTION;
    if (s->closed || uv_tty_get_winsize(&s->h.tty, &w, &h) < 0)
        return JS_NULL;
    v[0] = JS_NewInt32(ctx, w);
    v[1] = JS_NewInt32(ctx, h);
    return JS_NewArrayFrom(ctx, 2, v);
}

static JSValue qb_stream_setBlocking(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (s->closed)
        return JS_NewInt32(ctx, UV_EBADF);
    return JS_NewInt32(
        ctx, uv_stream_set_blocking(&s->h.stream, argc > 0 && JS_ToBool(ctx, argv[0]) > 0));
}

/* ref(), unref(), hasRef(), close(cb?) */
static JSValue qb_stream_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                int magic)
{
    QBStream *s = qb_stream_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* ref */
        if (!s->closed)
            uv_ref(&s->h.handle);
        break;
    case 1: /* unref */
        if (!s->closed)
            uv_unref(&s->h.handle);
        break;
    case 2: /* hasRef */
        return JS_NewBool(ctx, !s->closed && uv_has_ref(&s->h.handle));
    case 3: /* close */
        if (!s->closed) {
            if (s->reading)
                uv_read_stop(&s->h.stream);
            s->reading = 0;
            s->listening = 0;
            if (argc > 0 && JS_IsFunction(ctx, argv[0]))
                qb_stream_set_value(ctx, &s->close_cb, argv[0]);
            s->closed = 1;
            uv_close(&s->h.handle, qb_stream_close_cb);
            qb_stream_set_value(ctx, &s->onconnection, JS_UNDEFINED);
            qb_stream_update(s, this_val);
        }
        break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_stream_proto_funcs[] = {
    JS_CGETSET_DEF("fd", qb_stream_get_fd, NULL),
    JS_CGETSET_DEF("onread", qb_stream_get_onread, qb_stream_set_onread),
    JS_CGETSET_DEF("writeQueueSize", qb_stream_get_writeQueueSize, NULL),
    JS_CFUNC_DEF("readStart", 0, qb_stream_readStart),
    JS_CFUNC_DEF("readStop", 0, qb_stream_readStop),
    JS_CFUNC_DEF("write", 2, qb_stream_write),
    JS_CFUNC_DEF("writev", 2, qb_stream_writev),
    JS_CFUNC_DEF("tryWrite", 1, qb_stream_tryWrite),
    JS_CFUNC_DEF("shutdown", 1, qb_stream_shutdown),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_stream_method, 0),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_stream_method, 1),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_stream_method, 2),
    JS_CFUNC_MAGIC_DEF("close", 1, qb_stream_method, 3),
    JS_CFUNC_DEF("setBlocking", 1, qb_stream_setBlocking),
    JS_CFUNC_DEF("bind", 3, qb_stream_bind),
    JS_CFUNC_DEF("listen", 2, qb_stream_listen),
    JS_CFUNC_DEF("connect", 3, qb_stream_connect),
    JS_CFUNC_MAGIC_DEF("getsockname", 0, qb_stream_name, 0),
    JS_CFUNC_MAGIC_DEF("getpeername", 0, qb_stream_name, 1),
    JS_CFUNC_DEF("setNoDelay", 1, qb_stream_setNoDelay),
    JS_CFUNC_DEF("setKeepAlive", 2, qb_stream_setKeepAlive),
    JS_CFUNC_DEF("open", 1, qb_stream_open),
    JS_CFUNC_DEF("setRawMode", 1, qb_stream_setRawMode),
    JS_CFUNC_DEF("getWindowSize", 0, qb_stream_getWindowSize),
};

/* ------------------------------------------------------------ constructors */

static JSValue qb_net_createTCP(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qb_stream_new(ctx, QB_KIND_TCP, 0);
}

static JSValue qb_net_createPipe(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    int ipc = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    return qb_stream_new(ctx, QB_KIND_PIPE, ipc);
}

static JSValue qb_net_createTTY(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBStream *s;
    JSValue obj;
    int32_t fd;
    int readable, err;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    readable = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    obj = qb_stream_alloc(ctx, QB_KIND_TTY, 0, &s);
    if (JS_IsException(obj))
        return obj;
    err = uv_tty_init(s->qr->loop, &s->h.tty, fd, readable);
    if (err < 0) {
        /* the handle is not initialized: nothing to close */
        free(s);
        JS_FreeValue(ctx, obj);
        return qb_throw_uv(ctx, err, "uv_tty_init", NULL, NULL);
    }
    qb_stream_attach(obj, s);
    return obj;
}

static JSValue qb_net_guessHandleType(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    int32_t fd;
    const char *type;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (fd < 0)
        return JS_NewString(ctx, "UNKNOWN");
    switch (uv_guess_handle(fd)) {
    case UV_TTY:
        type = "TTY";
        break;
    case UV_NAMED_PIPE:
        type = "PIPE";
        break;
    case UV_TCP:
        type = "TCP";
        break;
    case UV_UDP:
        type = "UDP";
        break;
    case UV_FILE:
        type = "FILE";
        break;
    default:
        type = "UNKNOWN";
        break;
    }
    return JS_NewString(ctx, type);
}

static JSValue qb_net_isatty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t fd;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, fd >= 0 && uv_guess_handle(fd) == UV_TTY);
}

static JSValue qb_net_resetTTY(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uv_tty_reset_mode();
    return JS_UNDEFINED;
}

/* --------------------------------------------------------------------- dns */

typedef struct QBDnsReq {
    union {
        uv_getaddrinfo_t ai;
        uv_getnameinfo_t ni;
    } r;
    QBRuntime *qr;
    JSValue cb;
    char host[256];
} QBDnsReq;

static void qb_dns_finish(QBDnsReq *q, int argc, JSValue *args)
{
    JSContext *ctx = q->qr->ctx;
    JSValue ret;
    int i;
    if (JS_IsFunction(ctx, q->cb)) {
        ret = qb_call(ctx, q->cb, JS_UNDEFINED, argc, (JSValueConst *)args);
        JS_FreeValue(ctx, ret);
    }
    for (i = 0; i < argc; i++)
        JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, q->cb);
    free(q);
}

static void qb_getaddrinfo_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *res)
{
    QBDnsReq *q = req->data;
    JSContext *ctx = q->qr->ctx;
    JSValue args[2];
    struct addrinfo *ai;
    uint32_t n = 0;

    if (status == 0) {
        char prev[INET6_ADDRSTRLEN + 1] = "";
        int prev_family = 0;
        args[1] = JS_NewArray(ctx);
        if (JS_IsException(args[1]))
            goto fail;
        for (ai = res; ai; ai = ai->ai_next) {
            char ip[INET6_ADDRSTRLEN + 1];
            int family;
            JSValue obj;
            if (ai->ai_family == AF_INET) {
                uv_ip4_name((const struct sockaddr_in *)ai->ai_addr, ip, sizeof(ip));
                family = 4;
            } else if (ai->ai_family == AF_INET6) {
                uv_ip6_name((const struct sockaddr_in6 *)ai->ai_addr, ip, sizeof(ip));
                family = 6;
            } else {
                continue;
            }
            /* one entry per socket type may be returned */
            if (family == prev_family && !strcmp(ip, prev))
                continue;
            prev_family = family;
            memcpy(prev, ip, sizeof(prev));
            obj = JS_NewObject(ctx);
            if (JS_IsException(obj)) {
                JS_FreeValue(ctx, args[1]);
                goto fail;
            }
            if (qb_set(ctx, obj, "address", JS_NewString(ctx, ip)) < 0 ||
                qb_set(ctx, obj, "family", JS_NewInt32(ctx, family)) < 0) {
                JS_FreeValue(ctx, obj);
                JS_FreeValue(ctx, args[1]);
                goto fail;
            }
            /* JS_SetPropertyUint32() takes 'obj' */
            if (JS_SetPropertyUint32(ctx, args[1], n++, obj) < 0) {
                JS_FreeValue(ctx, args[1]);
                goto fail;
            }
        }
        if (n == 0) {
            JS_FreeValue(ctx, args[1]);
            status = UV_EAI_NODATA;
        }
    }
    if (res)
        uv_freeaddrinfo(res);
    if (status < 0) {
        args[0] = qb_dns_error(ctx, status, "getaddrinfo", q->host);
        if (JS_IsException(args[0])) {
            qb_report_exception(ctx);
            args[0] = JS_UNDEFINED;
        }
        qb_dns_finish(q, 1, args);
    } else {
        args[0] = JS_NULL;
        qb_dns_finish(q, 2, args);
    }
    return;
fail:
    if (res)
        uv_freeaddrinfo(res);
    qb_report_exception(ctx);
    args[0] = qb_dns_error(ctx, UV_ENOMEM, "getaddrinfo", q->host);
    if (JS_IsException(args[0])) {
        qb_report_exception(ctx);
        args[0] = JS_UNDEFINED;
    }
    qb_dns_finish(q, 1, args);
}

/* getaddrinfo(host, family, flags, cb) */
static JSValue qb_net_getaddrinfo(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    struct addrinfo hints;
    const char *host;
    int32_t family = 0, flags = 0;
    QBDnsReq *q;
    int err;

    if (argc < 4 || !JS_IsFunction(ctx, argv[3]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    if (JS_ToInt32(ctx, &family, argv[1]) || JS_ToInt32(ctx, &flags, argv[2]))
        return JS_EXCEPTION;
    host = JS_ToCString(ctx, argv[0]);
    if (!host)
        return JS_EXCEPTION;
    q = calloc(1, sizeof(*q));
    if (!q) {
        JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    q->qr = qr;
    snprintf(q->host, sizeof(q->host), "%s", host);
    q->r.ai.data = q;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    err = uv_getaddrinfo(qr->loop, &q->r.ai, qb_getaddrinfo_cb, host, NULL, &hints);
    JS_FreeCString(ctx, host);
    if (err < 0) {
        JSValue e = qb_dns_error(ctx, err, "getaddrinfo", q->host);
        free(q);
        qb_defer_error(ctx, argv[3], JS_UNDEFINED, e);
        return JS_UNDEFINED;
    }
    q->cb = JS_DupValue(ctx, argv[3]);
    return JS_UNDEFINED;
}

static void qb_getnameinfo_cb(uv_getnameinfo_t *req, int status, const char *hostname,
                              const char *service)
{
    QBDnsReq *q = req->data;
    JSContext *ctx = q->qr->ctx;
    JSValue args[3];

    if (status < 0) {
        args[0] = qb_dns_error(ctx, status, "getnameinfo", q->host);
        if (JS_IsException(args[0])) {
            qb_report_exception(ctx);
            args[0] = JS_UNDEFINED;
        }
        qb_dns_finish(q, 1, args);
        return;
    }
    args[0] = JS_NULL;
    args[1] = JS_NewString(ctx, hostname ? hostname : "");
    args[2] = JS_NewString(ctx, service ? service : "");
    if (JS_IsException(args[1]) || JS_IsException(args[2])) {
        qb_report_exception(ctx);
        if (JS_IsException(args[1]))
            args[1] = JS_UNDEFINED;
        if (JS_IsException(args[2]))
            args[2] = JS_UNDEFINED;
    }
    qb_dns_finish(q, 3, args);
}

/* getnameinfo(address, port, cb) */
static JSValue qb_net_getnameinfo(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    struct sockaddr_storage ss;
    const char *addr;
    int32_t port;
    QBDnsReq *q;
    int err;

    if (argc < 3 || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    if (JS_ToInt32(ctx, &port, argv[1]))
        return JS_EXCEPTION;
    addr = JS_ToCString(ctx, argv[0]);
    if (!addr)
        return JS_EXCEPTION;
    err = qb_parse_addr(addr, port, &ss);
    if (err < 0) {
        JSValue e = qb_dns_error(ctx, err, "getnameinfo", addr);
        JS_FreeCString(ctx, addr);
        if (JS_IsException(e))
            return e;
        return JS_Throw(ctx, e);
    }
    q = calloc(1, sizeof(*q));
    if (!q) {
        JS_FreeCString(ctx, addr);
        return JS_ThrowOutOfMemory(ctx);
    }
    q->qr = qr;
    snprintf(q->host, sizeof(q->host), "%s", addr);
    JS_FreeCString(ctx, addr);
    q->r.ni.data = q;
    err = uv_getnameinfo(qr->loop, &q->r.ni, qb_getnameinfo_cb, (const struct sockaddr *)&ss,
                         NI_NAMEREQD);
    if (err < 0) {
        JSValue e = qb_dns_error(ctx, err, "getnameinfo", q->host);
        free(q);
        qb_defer_error(ctx, argv[2], JS_UNDEFINED, e);
        return JS_UNDEFINED;
    }
    q->cb = JS_DupValue(ctx, argv[2]);
    return JS_UNDEFINED;
}

/* ----------------------------------------------------------------- module */

static const JSCFunctionListEntry qb_net_funcs[] = {
    JS_CFUNC_DEF("guessHandleType", 1, qb_net_guessHandleType),
    JS_CFUNC_DEF("isatty", 1, qb_net_isatty),
    JS_CFUNC_DEF("createTCP", 0, qb_net_createTCP),
    JS_CFUNC_DEF("createPipe", 1, qb_net_createPipe),
    JS_CFUNC_DEF("createTTY", 2, qb_net_createTTY),
    JS_CFUNC_DEF("resetTTY", 0, qb_net_resetTTY),
    JS_CFUNC_DEF("getaddrinfo", 4, qb_net_getaddrinfo),
    JS_CFUNC_DEF("getnameinfo", 3, qb_net_getnameinfo),
    /* getaddrinfo() flags (dns.ADDRCONFIG, dns.V4MAPPED, dns.ALL) */
    JS_PROP_INT32_DEF("AI_ADDRCONFIG", AI_ADDRCONFIG, JS_PROP_ENUMERABLE),
#ifdef AI_V4MAPPED
    JS_PROP_INT32_DEF("AI_V4MAPPED", AI_V4MAPPED, JS_PROP_ENUMERABLE),
#else
    JS_PROP_INT32_DEF("AI_V4MAPPED", 0, JS_PROP_ENUMERABLE),
#endif
#ifdef AI_ALL
    JS_PROP_INT32_DEF("AI_ALL", AI_ALL, JS_PROP_ENUMERABLE),
#else
    JS_PROP_INT32_DEF("AI_ALL", 0, JS_PROP_ENUMERABLE),
#endif
};

static int qb_net_module_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, qb_net_funcs, countof(qb_net_funcs));
}

JSModuleDef *qb_init_net(JSContext *ctx, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSModuleDef *m;

    JS_NewClassID(&qb_stream_class_id);
    if (!JS_IsRegisteredClass(rt, qb_stream_class_id)) {
        JSValue proto;
        if (JS_NewClass(rt, qb_stream_class_id, &qb_stream_class) < 0)
            return NULL;
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return NULL;
        if (JS_SetPropertyFunctionList(ctx, proto, qb_stream_proto_funcs,
                                       countof(qb_stream_proto_funcs)) < 0) {
            JS_FreeValue(ctx, proto);
            return NULL;
        }
        JS_SetClassProto(ctx, qb_stream_class_id, proto);
    }
    m = JS_NewCModule(ctx, name, qb_net_module_init);
    if (!m)
        return NULL;
    if (JS_AddModuleExportList(ctx, m, qb_net_funcs, countof(qb_net_funcs)) < 0)
        return NULL;
    return m;
}
