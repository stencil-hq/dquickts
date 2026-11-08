/*
 * qbun:http — the HTTP(S) and WebSocket client of fetch() and WebSocket.
 *
 * libcurl's multi interface driven by the libuv loop of the runtime:
 * CURLMOPT_SOCKETFUNCTION maps every socket to a uv_poll_t,
 * CURLMOPT_TIMERFUNCTION to a uv_timer_t, and the poll/timer callbacks
 * call curl_multi_socket_action(). One multi handle per runtime, owned by
 * a hidden "HttpState" object given as function data to the exported
 * functions (no static mutable state: workers have their own).
 *
 * JavaScript is never called from a libcurl callback (libcurl forbids
 * removing a handle from its own callbacks): the callbacks record the
 * events (headers, data, end, pull, WebSocket frames) on the item, which
 * is queued, and the queue is delivered after curl_multi_socket_action()
 * returns (or from a 0 ms timer when the events come from a JS call such
 * as resume()).
 *
 *   request(options, handlers) -> HttpRequest
 *     options: { url, method, headers: (string | null)[] (flat name/value
 *       list; a null value removes a header libcurl adds by itself, "" sends
 *       an empty header), body?: bytes (whole), streaming?: boolean (the
 *       body comes from write()/end(), pulled with onPull), bodyLength?,
 *       decompress?, httpVersion?: 1 | 2, proxy?: string ("" = direct),
 *       noProxy?, proxyHeaders?, rejectUnauthorized?, ca?, extraCa? (added
 *       to the default bundle), cert?, key?,
 *       passphrase?, ciphers?, unix?, verbose?, keepalive?, connectTimeout? }
 *     handlers: onHeaders(status, statusText, headers[], url, version),
 *       onData(Buffer), onPull(), onEnd(), onError(info)
 *     HttpRequest: write(bytes), end(), pause(), resume(), abort(), ref(),
 *       unref(), hasRef()
 *   websocket(options, handlers) -> HttpWebSocket
 *     options: { url (ws:/wss:), headers, proxy..., TLS options, verbose }
 *     handlers: onOpen(status, headers[], url), onMessage(data: string |
 *       Buffer, binary), onPing(Buffer), onPong(Buffer), onError(info),
 *       onClose(code, reason, wasClean)
 *     HttpWebSocket: send(data, binary), ping(data?), pong(data?),
 *       close(code, reason), terminate(), pause(), resume(), ref(),
 *       unref(), hasRef(), bufferedAmount
 *
 *   info (errors): { code, message, curlCode, errno?, syscall?, status?,
 *     tlsMessage? }: code is Node-like (ENOTFOUND, ECONNREFUSED,
 *     ETIMEDOUT, ECONNRESET, CERT_HAS_EXPIRED, ERR_TLS_CERT_ALTNAME_INVALID...)
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include "qbun.h"

/* received bytes buffered per request before libcurl is paused */
#define QH_MAX_BUFFER (1024 * 1024)

static pthread_once_t qh_curl_once = PTHREAD_ONCE_INIT;

static void qh_curl_global_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static JSClassID qh_state_class_id;
static JSClassID qh_req_class_id;
static JSClassID qh_ws_class_id;

/* ----------------------------------------------------------------- buffers */

typedef struct QHBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
} QHBuf;

static int qh_buf_append(QHBuf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        uint8_t *d;
        while (cap < b->len + n)
            cap *= 2;
        d = realloc(b->data, cap);
        if (!d)
            return -1;
        b->data = d;
        b->cap = cap;
    }
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static void qh_buf_free(QHBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* the same with the allocator of the runtime: the bytes become a Buffer
   (qb_new_buffer_owned) without a copy */
static int qh_rbuf_append(JSRuntime *rt, QHBuf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *d;
        while (cap < b->len + n)
            cap *= 2;
        d = js_realloc_rt(rt, b->data, cap);
        if (!d)
            return -1;
        b->data = d;
        b->cap = cap;
    }
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static void qh_rbuf_free(JSRuntime *rt, QHBuf *b)
{
    if (b->data)
        js_free_rt(rt, b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* a Buffer taking the bytes of 'b' (left empty) */
static JSValue qh_rbuf_take(JSContext *ctx, QHBuf *b)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    uint8_t *data = b->data;
    size_t len = b->len;
    if (!data || !len) {
        qh_rbuf_free(rt, b);
        return qb_new_buffer(ctx, (const uint8_t *)"", 0);
    }
    if (b->cap > len + 4096) {
        uint8_t *d = js_realloc_rt(rt, data, len);
        if (d)
            data = d;
    }
    b->data = NULL;
    b->len = b->cap = 0;
    return qb_new_buffer_owned(ctx, data, len);
}

/* ------------------------------------------------------------------- types */

typedef struct QHMulti QHMulti;
typedef struct QHItem QHItem;
typedef struct QHWs QHWs;

typedef struct QHSock {
    uv_poll_t poll;
    QHMulti *m;
    curl_socket_t fd;
    int curl_events; /* UV_READABLE / UV_WRITABLE asked by libcurl */
    int ws_events;   /* asked by the WebSocket using the socket */
    QHWs *ws;
    struct QHSock *next;
} QHSock;

struct QHMulti {
    QBRuntime *qr;
    CURLM *multi;
    uv_timer_t timer;    /* libcurl's timeout */
    uv_timer_t dispatch; /* deferred delivery of the events */
    uv_async_t alive;    /* ref'd while an item keeps the loop alive */
    int nalive;
    int refs;            /* the state object + every item */
    int handles;         /* open uv handles */
    int dead;
    int busy;            /* inside a libcurl action: delivery follows */
    int in_dispatch;
    int dispatch_scheduled;
    QHSock *socks;
    QHItem *pending;
    QHItem *pending_tail;
};

enum { QH_REQ = 1, QH_WS = 2 };

struct QHItem {
    int kind;
    QHMulti *m;
    CURL *easy;
    JSValue self; /* strong while active */
    JSValue cbs;  /* the handlers object */
    QHItem *next_pending;
    int queued;
    int added;    /* in the multi handle */
    int finished; /* detached from libcurl, no more events */
    int counted_alive;
    int want_ref;
    struct curl_slist *headers;
    struct curl_slist *proxy_headers;
    char errbuf[CURL_ERROR_SIZE];
    /* response head */
    long status;
    char status_text[128];
    char version[16];
    QHBuf hdrs; /* "name\0value\0"... */
    int headers_ready;
    int headers_sent;
    int done;
    int done_sent;
    CURLcode result;
};

typedef struct QHReq {
    QHItem b;
    QHBuf body;      /* received bytes (runtime allocator) */
    int want_pause;  /* pause() from JavaScript */
    int curl_paused; /* the write callback paused: too much buffered */
    int send_paused; /* the read callback paused: no upload data */
    int streaming;
    QHBuf up;        /* upload data from write() */
    size_t up_off;
    int up_done;
    int want_pull;
    int pull_sent;
} QHReq;

typedef struct QHWsOut {
    struct QHWsOut *next;
    unsigned int flags;
    size_t len;
    size_t off;
    uint8_t data[];
} QHWsOut;

enum { QH_EV_OPEN, QH_EV_MESSAGE, QH_EV_PING, QH_EV_PONG, QH_EV_ERROR, QH_EV_CLOSE };

typedef struct QHWsEv {
    struct QHWsEv *next;
    int type;
    int binary;
    int code;
    int clean;
    CURLcode rc;
    uint8_t *data; /* runtime allocator */
    size_t len;
} QHWsEv;

struct QHWs {
    QHItem b;
    curl_socket_t fd;
    QHSock *sock;
    int open;
    int paused;
    int closed; /* the close event is queued */
    int close_sent;
    int close_received;
    int close_code;
    char close_reason[128];
    size_t close_reason_len;
    QHBuf msg; /* message being received (runtime allocator) */
    int in_msg;
    int msg_binary;
    uint8_t ctrl[128];
    size_t ctrl_len;
    QHWsOut *out_head;
    QHWsOut *out_tail;
    size_t buffered;
    QHWsEv *ev_head;
    QHWsEv *ev_tail;
};

static void qh_req_deliver(QHReq *r);
static void qh_ws_deliver(QHWs *ws);
static void qh_ws_read(QHWs *ws);
static int qh_ws_flush(QHWs *ws);
static void qh_poll_cb(uv_poll_t *h, int status, int events);

/* ----------------------------------------------------------- multi handle */

static void qh_multi_handle_closed(QHMulti *m)
{
    if (--m->handles == 0 && m->dead)
        free(m);
}

static void qh_multi_close_cb(uv_handle_t *h)
{
    qh_multi_handle_closed(h->data);
}

static void qh_sock_close_cb(uv_handle_t *h)
{
    QHSock *sk = h->data;
    QHMulti *m = sk->m;
    free(sk);
    qh_multi_handle_closed(m);
}

static QHSock *qh_sock_find(QHMulti *m, curl_socket_t fd)
{
    QHSock *sk;
    for (sk = m->socks; sk; sk = sk->next) {
        if (sk->fd == fd)
            return sk;
    }
    return NULL;
}

static QHSock *qh_sock_new(QHMulti *m, curl_socket_t fd)
{
    QHSock *sk = calloc(1, sizeof(*sk));
    if (!sk)
        return NULL;
    if (uv_poll_init_socket(m->qr->loop, &sk->poll, fd) < 0) {
        free(sk);
        return NULL;
    }
    sk->poll.data = sk;
    sk->m = m;
    sk->fd = fd;
    /* the loop is kept alive by m->alive, not by the sockets (idle
       keep-alive connections must not hold the process) */
    uv_unref((uv_handle_t *)&sk->poll);
    m->handles++;
    sk->next = m->socks;
    m->socks = sk;
    return sk;
}

static void qh_sock_close(QHSock *sk)
{
    QHMulti *m = sk->m;
    QHSock **pp;
    for (pp = &m->socks; *pp; pp = &(*pp)->next) {
        if (*pp == sk) {
            *pp = sk->next;
            break;
        }
    }
    if (sk->ws)
        sk->ws->sock = NULL;
    uv_poll_stop(&sk->poll);
    uv_close((uv_handle_t *)&sk->poll, qh_sock_close_cb);
}

/* start/stop the poll handle for the union of the interests */
static void qh_sock_update(QHSock *sk)
{
    int ev = sk->curl_events | sk->ws_events;
    if (!ev && !sk->ws) {
        qh_sock_close(sk);
        return;
    }
    if (ev)
        uv_poll_start(&sk->poll, ev, qh_poll_cb);
    else
        uv_poll_stop(&sk->poll);
}

static int qh_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp)
{
    QHMulti *m = userp;
    QHSock *sk = qh_sock_find(m, s);
    (void)easy;
    (void)socketp;
    if (what == CURL_POLL_REMOVE) {
        if (sk) {
            sk->curl_events = 0;
            qh_sock_update(sk);
        }
        return 0;
    }
    if (!sk) {
        sk = qh_sock_new(m, s);
        if (!sk)
            return -1;
    }
    sk->curl_events =
        ((what & CURL_POLL_IN) ? UV_READABLE : 0) | ((what & CURL_POLL_OUT) ? UV_WRITABLE : 0);
    qh_sock_update(sk);
    return 0;
}

static void qh_timer_cb(uv_timer_t *h);

static int qh_timer_func(CURLM *multi, long timeout_ms, void *userp)
{
    QHMulti *m = userp;
    (void)multi;
    if (m->dead)
        return 0;
    if (timeout_ms < 0)
        uv_timer_stop(&m->timer);
    else
        uv_timer_start(&m->timer, qh_timer_cb, (uint64_t)timeout_ms, 0);
    return 0;
}

static void qh_dispatch_cb(uv_timer_t *h);

static void qh_schedule(QHMulti *m)
{
    if (m->busy || m->in_dispatch || m->dispatch_scheduled || m->dead)
        return;
    m->dispatch_scheduled = 1;
    uv_timer_start(&m->dispatch, qh_dispatch_cb, 0, 0);
}

static void qh_queue(QHItem *it)
{
    QHMulti *m = it->m;
    if (it->queued || it->finished)
        return;
    it->queued = 1;
    it->next_pending = NULL;
    if (m->pending_tail)
        m->pending_tail->next_pending = it;
    else
        m->pending = it;
    m->pending_tail = it;
    qh_schedule(m);
}

static void qh_unqueue(QHItem *it)
{
    QHMulti *m = it->m;
    QHItem **pp, *prev = NULL;
    if (!it->queued)
        return;
    for (pp = &m->pending; *pp; prev = *pp, pp = &(*pp)->next_pending) {
        if (*pp == it) {
            *pp = it->next_pending;
            if (m->pending_tail == it)
                m->pending_tail = prev;
            break;
        }
    }
    it->queued = 0;
    it->next_pending = NULL;
}

static void qh_dispatch(QHMulti *m)
{
    if (m->in_dispatch)
        return;
    m->in_dispatch = 1;
    if (m->dispatch_scheduled) {
        m->dispatch_scheduled = 0;
        uv_timer_stop(&m->dispatch);
    }
    while (m->pending) {
        QHItem *it = m->pending;
        qh_unqueue(it);
        if (it->kind == QH_REQ)
            qh_req_deliver((QHReq *)it);
        else
            qh_ws_deliver((QHWs *)it);
    }
    m->in_dispatch = 0;
}

static void qh_dispatch_cb(uv_timer_t *h)
{
    QHMulti *m = h->data;
    m->dispatch_scheduled = 0;
    qh_dispatch(m);
}

static void qh_req_done(QHReq *r, CURLcode rc);
static void qh_ws_connected(QHWs *ws, CURLcode rc);

static void qh_check_info(QHMulti *m)
{
    CURLMsg *msg;
    int left;
    while ((msg = curl_multi_info_read(m->multi, &left))) {
        char *priv = NULL;
        CURLcode rc;
        QHItem *it;
        if (msg->msg != CURLMSG_DONE)
            continue;
        rc = msg->data.result;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
        it = (QHItem *)priv;
        if (!it || it->finished)
            continue;
        if (it->kind == QH_REQ)
            qh_req_done((QHReq *)it, rc);
        else
            qh_ws_connected((QHWs *)it, rc);
    }
}

static void qh_timer_cb(uv_timer_t *h)
{
    QHMulti *m = h->data;
    int running;
    m->busy++;
    curl_multi_socket_action(m->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    qh_check_info(m);
    m->busy--;
    qh_dispatch(m);
}

static void qh_poll_cb(uv_poll_t *h, int status, int events)
{
    QHSock *sk = h->data;
    QHMulti *m = sk->m;
    curl_socket_t fd = sk->fd;
    int flags = 0, running;

    if (status < 0)
        flags |= CURL_CSELECT_ERR;
    if (events & UV_READABLE)
        flags |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE)
        flags |= CURL_CSELECT_OUT;
    m->busy++;
    if (sk->curl_events || status < 0) {
        curl_multi_socket_action(m->multi, fd, flags, &running);
        qh_check_info(m);
    }
    /* the socket may have been closed by libcurl meanwhile: look it up */
    sk = qh_sock_find(m, fd);
    if (sk && sk->ws) {
        QHWs *ws = sk->ws;
        if ((events & UV_WRITABLE) || status < 0)
            qh_ws_flush(ws);
        if ((events & UV_READABLE) || status < 0)
            qh_ws_read(ws);
    }
    m->busy--;
    qh_dispatch(m);
}

static void qh_alive_cb(uv_async_t *h)
{
    (void)h;
}

static void qh_multi_unref(QHMulti *m)
{
    if (--m->refs > 0)
        return;
    m->dead = 1;
    if (m->multi) {
        curl_multi_cleanup(m->multi);
        m->multi = NULL;
    }
    while (m->socks)
        qh_sock_close(m->socks);
    uv_timer_stop(&m->timer);
    uv_timer_stop(&m->dispatch);
    uv_close((uv_handle_t *)&m->timer, qh_multi_close_cb);
    uv_close((uv_handle_t *)&m->dispatch, qh_multi_close_cb);
    uv_close((uv_handle_t *)&m->alive, qh_multi_close_cb);
}

/* ------------------------------------------------------------------ items */

static int qh_item_wants_alive(QHItem *it)
{
    if (it->finished || !it->added || !it->want_ref)
        return 0;
    if (it->kind == QH_REQ)
        return !((QHReq *)it)->want_pause;
    return 1;
}

static void qh_item_update_alive(QHItem *it)
{
    QHMulti *m = it->m;
    int want = qh_item_wants_alive(it);
    if (want == it->counted_alive)
        return;
    it->counted_alive = want;
    m->nalive += want ? 1 : -1;
    if (m->nalive > 0)
        uv_ref((uv_handle_t *)&m->alive);
    else
        uv_unref((uv_handle_t *)&m->alive);
}

static void qh_item_init(QHItem *it, int kind, QHMulti *m)
{
    it->kind = kind;
    it->m = m;
    it->self = JS_UNDEFINED;
    it->cbs = JS_UNDEFINED;
    it->want_ref = 1;
    m->refs++;
}

static void qh_ws_free_queues(QHWs *ws)
{
    JSRuntime *rt = ws->b.m->qr->rt;
    while (ws->out_head) {
        QHWsOut *o = ws->out_head;
        ws->out_head = o->next;
        free(o);
    }
    ws->out_tail = NULL;
    ws->buffered = 0;
    while (ws->ev_head) {
        QHWsEv *ev = ws->ev_head;
        ws->ev_head = ev->next;
        if (ev->data)
            js_free_rt(rt, ev->data);
        free(ev);
    }
    ws->ev_tail = NULL;
    qh_rbuf_free(rt, &ws->msg);
}

/* detach the item from libcurl: no more events. Releases 'self' last
   (which can free the item when JS no longer references it). */
static void qh_item_finish(QHItem *it)
{
    QHMulti *m = it->m;
    JSValue self;

    if (it->finished)
        return;
    it->finished = 1;
    qh_unqueue(it);
    if (it->kind == QH_WS) {
        QHWs *ws = (QHWs *)it;
        if (ws->sock) {
            QHSock *sk = ws->sock;
            sk->ws = NULL;
            sk->ws_events = 0;
            ws->sock = NULL;
            qh_sock_update(sk);
        }
        qh_ws_free_queues(ws);
    }
    if (it->easy) {
        if (it->added && m->multi)
            curl_multi_remove_handle(m->multi, it->easy);
        it->added = 0;
        curl_easy_cleanup(it->easy);
        it->easy = NULL;
    }
    it->added = 0;
    if (it->headers) {
        curl_slist_free_all(it->headers);
        it->headers = NULL;
    }
    if (it->proxy_headers) {
        curl_slist_free_all(it->proxy_headers);
        it->proxy_headers = NULL;
    }
    qh_item_update_alive(it);
    self = it->self;
    it->self = JS_UNDEFINED;
    JS_FreeValue(m->qr->ctx, self);
}

static void qh_item_free(QHItem *it)
{
    QHMulti *m = it->m;
    qh_buf_free(&it->hdrs);
    if (it->kind == QH_REQ) {
        QHReq *r = (QHReq *)it;
        qh_rbuf_free(m->qr->rt, &r->body);
        qh_buf_free(&r->up);
    }
    free(it);
    qh_multi_unref(m);
}

static void qh_item_finalize(JSRuntime *rt, QHItem *it)
{
    if (!it)
        return;
    JS_FreeValueRT(rt, it->cbs);
    it->cbs = JS_UNDEFINED;
    if (!it->finished)
        qh_item_finish(it);
    qh_item_free(it);
}

static void qh_req_finalizer(JSRuntime *rt, JSValue val)
{
    qh_item_finalize(rt, JS_GetOpaque(val, qh_req_class_id));
}

static void qh_ws_finalizer(JSRuntime *rt, JSValue val)
{
    qh_item_finalize(rt, JS_GetOpaque(val, qh_ws_class_id));
}

static void qh_req_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QHItem *it = JS_GetOpaque(val, qh_req_class_id);
    if (it)
        JS_MarkValue(rt, it->cbs, mark_func);
}

static void qh_ws_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QHItem *it = JS_GetOpaque(val, qh_ws_class_id);
    if (it)
        JS_MarkValue(rt, it->cbs, mark_func);
}

static void qh_state_finalizer(JSRuntime *rt, JSValue val)
{
    QHMulti *m = JS_GetOpaque(val, qh_state_class_id);
    (void)rt;
    if (m)
        qh_multi_unref(m);
}

static JSClassDef qh_state_class = {"HttpState", .finalizer = qh_state_finalizer};
static JSClassDef qh_req_class = {"HttpRequest", .finalizer = qh_req_finalizer,
                                  .gc_mark = qh_req_mark};
static JSClassDef qh_ws_class = {"HttpWebSocket", .finalizer = qh_ws_finalizer,
                                 .gc_mark = qh_ws_mark};

/* call handlers[name](...args) */
static void qh_emit(QHItem *it, const char *name, int argc, JSValueConst *argv)
{
    JSContext *ctx = it->m->qr->ctx;
    JSValue cbs, fn, ret;
    if (JS_IsUndefined(it->cbs))
        return;
    cbs = JS_DupValue(ctx, it->cbs);
    fn = JS_GetPropertyStr(ctx, cbs, name);
    if (JS_IsException(fn)) {
        qb_report_exception(ctx);
    } else if (JS_IsFunction(ctx, fn)) {
        ret = qb_call(ctx, fn, cbs, argc, argv);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, cbs);
}

static JSValue qh_headers_array(JSContext *ctx, QHItem *it)
{
    JSValue arr = JS_NewArray(ctx);
    size_t off = 0;
    uint32_t i = 0;
    if (JS_IsException(arr))
        return arr;
    while (off < it->hdrs.len) {
        const char *s = (const char *)it->hdrs.data + off;
        size_t l = strlen(s);
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, s, l));
        off += l + 1;
    }
    return arr;
}

static JSValue qh_effective_url(JSContext *ctx, QHItem *it)
{
    char *u = NULL;
    if (it->easy)
        curl_easy_getinfo(it->easy, CURLINFO_EFFECTIVE_URL, &u);
    return JS_NewString(ctx, u ? u : "");
}

/* --------------------------------------------------------------- errors */

typedef struct QHTlsName {
    long err;
    const char *name;
} QHTlsName;

#define QH_TLS(x) {X509_V_ERR_##x, #x}
static const QHTlsName qh_tls_names[] = {
    QH_TLS(UNABLE_TO_GET_ISSUER_CERT),
    QH_TLS(UNABLE_TO_GET_CRL),
    QH_TLS(UNABLE_TO_DECRYPT_CERT_SIGNATURE),
    QH_TLS(UNABLE_TO_DECRYPT_CRL_SIGNATURE),
    QH_TLS(UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY),
    QH_TLS(CERT_SIGNATURE_FAILURE),
    QH_TLS(CRL_SIGNATURE_FAILURE),
    QH_TLS(CERT_NOT_YET_VALID),
    QH_TLS(CERT_HAS_EXPIRED),
    QH_TLS(CRL_NOT_YET_VALID),
    QH_TLS(CRL_HAS_EXPIRED),
    QH_TLS(ERROR_IN_CERT_NOT_BEFORE_FIELD),
    QH_TLS(ERROR_IN_CERT_NOT_AFTER_FIELD),
    QH_TLS(ERROR_IN_CRL_LAST_UPDATE_FIELD),
    QH_TLS(ERROR_IN_CRL_NEXT_UPDATE_FIELD),
    QH_TLS(OUT_OF_MEM),
    QH_TLS(DEPTH_ZERO_SELF_SIGNED_CERT),
    QH_TLS(SELF_SIGNED_CERT_IN_CHAIN),
    QH_TLS(UNABLE_TO_GET_ISSUER_CERT_LOCALLY),
    QH_TLS(UNABLE_TO_VERIFY_LEAF_SIGNATURE),
    QH_TLS(CERT_CHAIN_TOO_LONG),
    QH_TLS(CERT_REVOKED),
    QH_TLS(INVALID_CA),
    QH_TLS(PATH_LENGTH_EXCEEDED),
    QH_TLS(INVALID_PURPOSE),
    QH_TLS(CERT_UNTRUSTED),
    QH_TLS(CERT_REJECTED),
};
#undef QH_TLS

static const char *qh_tls_code(long verify)
{
    size_t i;
    if (verify == X509_V_ERR_HOSTNAME_MISMATCH)
        return "ERR_TLS_CERT_ALTNAME_INVALID";
    for (i = 0; i < countof(qh_tls_names); i++) {
        if (qh_tls_names[i].err == verify)
            return qh_tls_names[i].name;
    }
    return "ERR_TLS_CERT";
}

static const char *qh_errno_code(long oserr, const char *def)
{
    if (oserr <= 0)
        return def;
    return uv_err_name((int)-oserr);
}

/* describe the failure 'rc' of an item for JavaScript */
static JSValue qh_error_info(JSContext *ctx, QHItem *it, CURLcode rc)
{
    long oserr = 0, verify = 0;
    const char *code = "ECONNRESET", *syscall = NULL;
    const char *detail = it->errbuf[0] ? it->errbuf : curl_easy_strerror(rc);
    int tls = 0;
    JSValue e;

    if (it->easy) {
        curl_easy_getinfo(it->easy, CURLINFO_OS_ERRNO, &oserr);
        curl_easy_getinfo(it->easy, CURLINFO_SSL_VERIFYRESULT, &verify);
    }
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        code = "ENOTFOUND";
        syscall = "getaddrinfo";
        break;
    case CURLE_COULDNT_CONNECT:
        code = qh_errno_code(oserr, "ECONNREFUSED");
        syscall = "connect";
        break;
    case CURLE_OPERATION_TIMEDOUT:
        code = "ETIMEDOUT";
        break;
    case CURLE_SEND_ERROR:
        code = qh_errno_code(oserr, "ECONNRESET");
        syscall = "write";
        break;
    case CURLE_RECV_ERROR:
        code = qh_errno_code(oserr, "ECONNRESET");
        syscall = "read";
        break;
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_HTTP3:
        code = "ECONNRESET";
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
        tls = 1;
        code = verify != X509_V_OK ? qh_tls_code(verify) : "ERR_TLS_CERT_ALTNAME_INVALID";
        break;
    case CURLE_SSL_CONNECT_ERROR:
        if (verify != X509_V_OK) {
            tls = 1;
            code = qh_tls_code(verify);
        } else {
            code = "EPROTO";
        }
        break;
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
        code = "ERR_TLS_INVALID_CERT";
        break;
    case CURLE_SSL_CIPHER:
        code = "ERR_TLS_INVALID_CIPHER";
        break;
    case CURLE_BAD_CONTENT_ENCODING:
        code = "ZlibError";
        break;
    case CURLE_URL_MALFORMAT:
        code = "ERR_INVALID_URL";
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
        code = "ERR_UNSUPPORTED_PROTOCOL";
        break;
    case CURLE_ABORTED_BY_CALLBACK:
    case CURLE_WRITE_ERROR:
    case CURLE_READ_ERROR:
        code = "ECONNABORTED";
        break;
    case CURLE_OUT_OF_MEMORY:
        code = "ENOMEM";
        break;
    case CURLE_SEND_FAIL_REWIND:
        code = "ERR_BODY_REWIND";
        break;
    case CURLE_HTTP_RETURNED_ERROR:
        code = "ERR_UNEXPECTED_RESPONSE";
        break;
    case CURLE_PROXY:
        code = "ERR_PROXY";
        break;
    default:
        code = qh_errno_code(oserr, "ECONNRESET");
        break;
    }

    e = JS_NewObject(ctx);
    if (JS_IsException(e))
        return e;
    qb_set(ctx, e, "code", JS_NewString(ctx, code));
    qb_set(ctx, e, "message", JS_NewString(ctx, detail));
    qb_set(ctx, e, "curlCode", JS_NewInt32(ctx, (int32_t)rc));
    if (oserr > 0)
        qb_set(ctx, e, "errno", JS_NewInt32(ctx, (int32_t)-oserr));
    if (syscall)
        qb_set(ctx, e, "syscall", JS_NewString(ctx, syscall));
    if (it->status > 0)
        qb_set(ctx, e, "status", JS_NewInt32(ctx, (int32_t)it->status));
    if (tls && verify != X509_V_OK) {
        const char *s = X509_verify_cert_error_string(verify);
        qb_set(ctx, e, "tlsMessage", JS_NewString(ctx, s ? s : ""));
    }
    return e;
}

/* ------------------------------------------------------- libcurl callbacks */

static size_t qh_header_cb(char *buf, size_t size, size_t n, void *userp)
{
    QHItem *it = userp;
    size_t len = size * n, l = len, nl, vs;
    const char *colon;

    while (l && (buf[l - 1] == '\r' || buf[l - 1] == '\n'))
        l--;
    if (it->headers_ready)
        return len; /* trailers */
    if (l >= 5 && memcmp(buf, "HTTP/", 5) == 0) {
        size_t i = 0, j;
        it->hdrs.len = 0;
        it->status = 0;
        it->status_text[0] = '\0';
        while (i < l && buf[i] != ' ' && i < sizeof(it->version) - 1) {
            it->version[i] = buf[i];
            i++;
        }
        it->version[i] = '\0';
        while (i < l && buf[i] != ' ')
            i++;
        while (i < l && buf[i] == ' ')
            i++;
        while (i < l && buf[i] >= '0' && buf[i] <= '9') {
            it->status = it->status * 10 + (buf[i] - '0');
            i++;
        }
        while (i < l && buf[i] == ' ')
            i++;
        j = l - i;
        if (j > sizeof(it->status_text) - 1)
            j = sizeof(it->status_text) - 1;
        memcpy(it->status_text, buf + i, j);
        it->status_text[j] = '\0';
        return len;
    }
    if (l == 0) {
        if (it->status >= 200 || it->status == 101) {
            it->headers_ready = 1;
            if (it->kind == QH_REQ)
                qh_queue(it);
        }
        return len;
    }
    if (buf[0] == ' ' || buf[0] == '\t') {
        /* obsolete line folding: append to the last value */
        size_t s = 0;
        while (s < l && (buf[s] == ' ' || buf[s] == '\t'))
            s++;
        if (it->hdrs.len > 0 && s < l) {
            it->hdrs.len--; /* drop the NUL of the last value */
            if (qh_buf_append(&it->hdrs, " ", 1) < 0 ||
                qh_buf_append(&it->hdrs, buf + s, l - s) < 0 || qh_buf_append(&it->hdrs, "", 1) < 0)
                return 0;
        }
        return len;
    }
    colon = memchr(buf, ':', l);
    if (!colon)
        return len;
    nl = (size_t)(colon - buf);
    while (nl > 0 && (buf[nl - 1] == ' ' || buf[nl - 1] == '\t'))
        nl--;
    vs = (size_t)(colon - buf) + 1;
    while (vs < l && (buf[vs] == ' ' || buf[vs] == '\t'))
        vs++;
    while (l > vs && (buf[l - 1] == ' ' || buf[l - 1] == '\t'))
        l--;
    if (qh_buf_append(&it->hdrs, buf, nl) < 0 || qh_buf_append(&it->hdrs, "", 1) < 0 ||
        qh_buf_append(&it->hdrs, buf + vs, l - vs) < 0 || qh_buf_append(&it->hdrs, "", 1) < 0)
        return 0;
    return len;
}

static size_t qh_write_cb(char *ptr, size_t size, size_t n, void *userp)
{
    QHReq *r = userp;
    size_t len = size * n;
    if (r->b.finished)
        return 0;
    if (r->body.len >= QH_MAX_BUFFER) {
        r->curl_paused = 1;
        return CURL_WRITEFUNC_PAUSE;
    }
    if (qh_rbuf_append(r->b.m->qr->rt, &r->body, ptr, len) < 0)
        return 0;
    if (!r->b.headers_ready && r->b.status > 0)
        r->b.headers_ready = 1;
    qh_queue(&r->b);
    return len;
}

static size_t qh_read_cb(char *buf, size_t size, size_t n, void *userp)
{
    QHReq *r = userp;
    size_t cap = size * n, avail = r->up.len - r->up_off;
    if (avail) {
        size_t c = avail < cap ? avail : cap;
        memcpy(buf, r->up.data + r->up_off, c);
        r->up_off += c;
        if (r->up_off == r->up.len)
            r->up.len = r->up_off = 0;
        return c;
    }
    if (r->up_done)
        return 0;
    r->send_paused = 1;
    if (!r->want_pull) {
        r->want_pull = 1;
        r->pull_sent = 0;
        qh_queue(&r->b);
    }
    return CURL_READFUNC_PAUSE;
}

/* ------------------------------------------------------------ option help */

/* obj[name] as a C string (JS_FreeCString it); NULL when undefined/null
   or on exception (*perr set) */
static const char *qh_get_cstr(JSContext *ctx, JSValueConst obj, const char *name, size_t *plen,
                               int *perr)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s;
    if (JS_IsException(v)) {
        *perr = 1;
        return NULL;
    }
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return NULL;
    s = JS_ToCStringLen(ctx, plen, v);
    JS_FreeValue(ctx, v);
    if (!s)
        *perr = 1;
    return s;
}

/* obj[name] as a boolean: 'def' when undefined, -1 on exception */
static int qh_get_bool(JSContext *ctx, JSValueConst obj, const char *name, int def)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int b;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return def;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* obj[name] as a number: 0 when undefined/null, 1 when set, -1 on exception */
static int qh_get_num(JSContext *ctx, JSValueConst obj, const char *name, double *out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int r;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    r = JS_ToFloat64(ctx, out, v) ? -1 : 1;
    JS_FreeValue(ctx, v);
    return r;
}

/* a flat [name, value, ...] list of obj[name] appended to *plist */
static int qh_get_headers(JSContext *ctx, JSValueConst obj, const char *name,
                          struct curl_slist **plist)
{
    JSValue arr = JS_GetPropertyStr(ctx, obj, name);
    int64_t len, i;
    int ret = -1;

    if (JS_IsException(arr))
        return -1;
    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;
    if (JS_GetLength(ctx, arr, &len))
        goto done;
    for (i = 0; i + 1 < len; i += 2) {
        JSValue nv = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        JSValue vv = JS_GetPropertyUint32(ctx, arr, (uint32_t)(i + 1));
        const char *ns = NULL, *vs = NULL;
        size_t nlen = 0, vlen = 0;
        char *line = NULL;
        struct curl_slist *t;
        int ok = 0;

        if (JS_IsException(nv) || JS_IsException(vv))
            goto item_done;
        ns = JS_ToCStringLen(ctx, &nlen, nv);
        if (!ns)
            goto item_done;
        if (!JS_IsNull(vv) && !JS_IsUndefined(vv)) {
            vs = JS_ToCStringLen(ctx, &vlen, vv);
            if (!vs)
                goto item_done;
        }
        line = malloc(nlen + vlen + 3);
        if (!line) {
            JS_ThrowOutOfMemory(ctx);
            goto item_done;
        }
        memcpy(line, ns, nlen);
        if (!vs) {
            /* "Name:" removes a header libcurl would add */
            line[nlen] = ':';
            line[nlen + 1] = '\0';
        } else if (vlen == 0) {
            /* "Name;" sends an empty header */
            line[nlen] = ';';
            line[nlen + 1] = '\0';
        } else {
            line[nlen] = ':';
            line[nlen + 1] = ' ';
            memcpy(line + nlen + 2, vs, vlen);
            line[nlen + 2 + vlen] = '\0';
        }
        t = curl_slist_append(*plist, line);
        if (!t) {
            JS_ThrowOutOfMemory(ctx);
            goto item_done;
        }
        *plist = t;
        ok = 1;
    item_done:
        free(line);
        if (ns)
            JS_FreeCString(ctx, ns);
        if (vs)
            JS_FreeCString(ctx, vs);
        JS_FreeValue(ctx, nv);
        JS_FreeValue(ctx, vv);
        if (!ok)
            goto done;
    }
    ret = 0;
done:
    JS_FreeValue(ctx, arr);
    return ret;
}

/* obj[name] (string or bytes) as a copied curl blob option */
static int qh_set_blob(JSContext *ctx, CURL *e, JSValueConst obj, const char *name, CURLoption opt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    struct curl_blob blob;
    int ret = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_IsString(v)) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        if (!s) {
            ret = -1;
        } else {
            blob.data = (void *)s;
            blob.len = len;
            blob.flags = CURL_BLOB_COPY;
            curl_easy_setopt(e, opt, &blob);
            JS_FreeCString(ctx, s);
        }
    } else {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, v, &len);
        if (!p) {
            ret = -1;
        } else {
            blob.data = p;
            blob.len = len;
            blob.flags = CURL_BLOB_COPY;
            curl_easy_setopt(e, opt, &blob);
        }
    }
    JS_FreeValue(ctx, v);
    return ret;
}

/* obj.extraCa (NODE_EXTRA_CA_CERTS): the default CA bundle of libcurl
   followed by the extra certificates, as CURLOPT_CAINFO_BLOB */
static int qh_set_extra_ca(JSContext *ctx, CURL *e, JSValueConst obj)
{
    int err = 0;
    size_t elen = 0;
    const char *extra = qh_get_cstr(ctx, obj, "extraCa", &elen, &err);
    char *path = NULL;
    QHBuf pem = {0};
    struct curl_blob blob;
    FILE *f;

    if (err)
        return -1;
    if (!extra)
        return 0;
    curl_easy_getinfo(e, CURLINFO_CAINFO, &path);
    if (path && (f = fopen(path, "rb"))) {
        char chunk[16384];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (qh_buf_append(&pem, chunk, n) < 0)
                break;
        }
        fclose(f);
        qh_buf_append(&pem, "\n", 1);
    }
    if (qh_buf_append(&pem, extra, elen) < 0) {
        JS_FreeCString(ctx, extra);
        qh_buf_free(&pem);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    JS_FreeCString(ctx, extra);
    blob.data = pem.data;
    blob.len = pem.len;
    blob.flags = CURL_BLOB_COPY;
    curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
    qh_buf_free(&pem);
    return 0;
}

/* set a string option from obj[name] when present */
static int qh_set_str(JSContext *ctx, CURL *e, JSValueConst obj, const char *name, CURLoption opt)
{
    int err = 0;
    const char *s = qh_get_cstr(ctx, obj, name, NULL, &err);
    if (err)
        return -1;
    if (s) {
        curl_easy_setopt(e, opt, s);
        JS_FreeCString(ctx, s);
    }
    return 0;
}

/* the options shared by requests and WebSockets */
static int qh_setup_common(JSContext *ctx, QHItem *it, JSValueConst opts)
{
    CURL *e;
    const char *url;
    int err = 0, b;
    double num;

    e = curl_easy_init();
    if (!e) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    it->easy = e;
    curl_easy_setopt(e, CURLOPT_PRIVATE, (void *)it);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, it->errbuf);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(e, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, qh_header_cb);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, (void *)it);

    url = qh_get_cstr(ctx, opts, "url", NULL, &err);
    if (err)
        return -1;
    if (!url) {
        JS_ThrowTypeError(ctx, "url must be a string");
        return -1;
    }
    curl_easy_setopt(e, CURLOPT_URL, url);
    JS_FreeCString(ctx, url);

    if (qh_get_headers(ctx, opts, "headers", &it->headers) < 0)
        return -1;
    if (it->headers)
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, it->headers);

    if (qh_set_str(ctx, e, opts, "proxy", CURLOPT_PROXY) < 0 ||
        qh_set_str(ctx, e, opts, "noProxy", CURLOPT_NOPROXY) < 0)
        return -1;
    if (qh_get_headers(ctx, opts, "proxyHeaders", &it->proxy_headers) < 0)
        return -1;
    if (it->proxy_headers) {
        curl_easy_setopt(e, CURLOPT_PROXYHEADER, it->proxy_headers);
        curl_easy_setopt(e, CURLOPT_HEADEROPT, (long)CURLHEADER_SEPARATE);
    }

    b = qh_get_bool(ctx, opts, "rejectUnauthorized", 1);
    if (b < 0)
        return -1;
    if (!b) {
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(e, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(e, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
    }
    if (qh_set_extra_ca(ctx, e, opts) < 0 ||
        qh_set_blob(ctx, e, opts, "ca", CURLOPT_CAINFO_BLOB) < 0 ||
        qh_set_blob(ctx, e, opts, "cert", CURLOPT_SSLCERT_BLOB) < 0 ||
        qh_set_blob(ctx, e, opts, "key", CURLOPT_SSLKEY_BLOB) < 0 ||
        qh_set_str(ctx, e, opts, "passphrase", CURLOPT_KEYPASSWD) < 0 ||
        qh_set_str(ctx, e, opts, "ciphers", CURLOPT_SSL_CIPHER_LIST) < 0 ||
        qh_set_str(ctx, e, opts, "unix", CURLOPT_UNIX_SOCKET_PATH) < 0)
        return -1;

    b = qh_get_bool(ctx, opts, "verbose", 0);
    if (b < 0)
        return -1;
    if (b)
        curl_easy_setopt(e, CURLOPT_VERBOSE, 1L);
    b = qh_get_bool(ctx, opts, "keepalive", 1);
    if (b < 0)
        return -1;
    if (!b)
        curl_easy_setopt(e, CURLOPT_FORBID_REUSE, 1L);
    switch (qh_get_num(ctx, opts, "connectTimeout", &num)) {
    case -1:
        return -1;
    case 1:
        if (num > 0)
            curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, (long)num);
        break;
    }
    return 0;
}

/* --------------------------------------------------------------- requests */

static void qh_req_apply_pause(QHReq *r)
{
    int bits;
    if (!r->b.added || r->b.done || r->b.finished)
        return;
    bits = ((r->want_pause || r->curl_paused) ? CURLPAUSE_RECV : 0) |
           (r->send_paused ? CURLPAUSE_SEND : 0);
    curl_easy_pause(r->b.easy, bits);
}

static void qh_req_done(QHReq *r, CURLcode rc)
{
    QHItem *it = &r->b;
    QHMulti *m = it->m;
    it->done = 1;
    it->result = rc;
    if (rc == CURLE_OK && !it->headers_ready) {
        long code = 0;
        curl_easy_getinfo(it->easy, CURLINFO_RESPONSE_CODE, &code);
        it->status = code;
        it->headers_ready = 1;
    }
    /* back to the connection pool */
    if (it->added) {
        curl_multi_remove_handle(m->multi, it->easy);
        it->added = 0;
    }
    qh_queue(it);
}

static void qh_req_deliver(QHReq *r)
{
    QHItem *it = &r->b;
    JSContext *ctx = it->m->qr->ctx;
    JSValue self = JS_DupValue(ctx, it->self);

    if (it->finished)
        goto out;
    if (it->headers_ready && !it->headers_sent) {
        JSValue args[5];
        it->headers_sent = 1;
        args[0] = JS_NewInt32(ctx, (int32_t)it->status);
        args[1] = JS_NewString(ctx, it->status_text);
        args[2] = qh_headers_array(ctx, it);
        args[3] = qh_effective_url(ctx, it);
        args[4] = JS_NewString(ctx, it->version);
        qh_emit(it, "onHeaders", 5, (JSValueConst *)args);
        for (int i = 0; i < 5; i++)
            JS_FreeValue(ctx, args[i]);
        if (it->finished)
            goto out;
    }
    if (r->want_pull && !r->pull_sent) {
        r->pull_sent = 1;
        qh_emit(it, "onPull", 0, NULL);
        if (it->finished)
            goto out;
    }
    if (r->body.len && it->headers_sent) {
        JSValue buf = qh_rbuf_take(ctx, &r->body);
        qh_emit(it, "onData", 1, (JSValueConst *)&buf);
        JS_FreeValue(ctx, buf);
        if (it->finished)
            goto out;
        if (r->curl_paused) {
            r->curl_paused = 0;
            qh_req_apply_pause(r);
        }
    }
    if (it->done && !it->done_sent && r->body.len == 0) {
        it->done_sent = 1;
        if (it->result == CURLE_OK && it->headers_sent) {
            qh_emit(it, "onEnd", 0, NULL);
        } else {
            JSValue info =
                qh_error_info(ctx, it, it->result == CURLE_OK ? CURLE_GOT_NOTHING : it->result);
            qh_emit(it, "onError", 1, (JSValueConst *)&info);
            JS_FreeValue(ctx, info);
        }
        qh_item_finish(it);
    }
out:
    JS_FreeValue(ctx, self);
}

static int qh_setup_request(JSContext *ctx, QHReq *r, JSValueConst opts)
{
    CURL *e;
    const char *method;
    JSValue body;
    int err = 0, b, has_body = 0;
    double num;

    if (qh_setup_common(ctx, &r->b, opts) < 0)
        return -1;
    e = r->b.easy;
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, qh_write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, (void *)r);
    curl_easy_setopt(e, CURLOPT_PIPEWAIT, 1L);

    b = qh_get_bool(ctx, opts, "decompress", 1);
    if (b < 0)
        return -1;
    if (b)
        curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    switch (qh_get_num(ctx, opts, "httpVersion", &num)) {
    case -1:
        return -1;
    case 1:
        if (num == 1) {
            curl_easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
            break;
        }
        /* fall through */
    default:
        curl_easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
        break;
    }

    body = JS_GetPropertyStr(ctx, opts, "body");
    if (JS_IsException(body))
        return -1;
    if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, body, &len);
        if (!p) {
            JS_FreeValue(ctx, body);
            return -1;
        }
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
        curl_easy_setopt(e, CURLOPT_COPYPOSTFIELDS, len ? (const char *)p : "");
        has_body = 1;
    }
    JS_FreeValue(ctx, body);

    if (!has_body) {
        b = qh_get_bool(ctx, opts, "streaming", 0);
        if (b < 0)
            return -1;
        if (b) {
            curl_off_t size = -1;
            double blen = -1;
            r->streaming = 1;
            if (qh_get_num(ctx, opts, "bodyLength", &blen) < 0)
                return -1;
            if (blen >= 0)
                size = (curl_off_t)blen;
            curl_easy_setopt(e, CURLOPT_POST, 1L);
            curl_easy_setopt(e, CURLOPT_READFUNCTION, qh_read_cb);
            curl_easy_setopt(e, CURLOPT_READDATA, (void *)r);
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, size);
            has_body = 1;
        }
    }

    method = qh_get_cstr(ctx, opts, "method", NULL, &err);
    if (err)
        return -1;
    if (method) {
        if (strcmp(method, "HEAD") == 0 && !has_body) {
            curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
        } else if (strcmp(method, "GET") == 0 && !has_body) {
            curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
        } else if (!(strcmp(method, "POST") == 0 && has_body)) {
            curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, method);
        }
        JS_FreeCString(ctx, method);
    }
    return 0;
}

static QHMulti *qh_state(JSContext *ctx, JSValue *data)
{
    QHMulti *m = JS_GetOpaque(data[0], qh_state_class_id);
    if (!m || m->dead || !m->multi)
        JS_ThrowInternalError(ctx, "qbun:http is not initialized");
    return m && !m->dead && m->multi ? m : NULL;
}

/* request(options, handlers) */
static JSValue qh_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic, JSValue *data)
{
    QHMulti *m = qh_state(ctx, data);
    QHReq *r;
    JSValue obj;
    CURLMcode mc;

    if (!m)
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "request(options, handlers) expects two objects");
    obj = JS_NewObjectClass(ctx, qh_req_class_id);
    if (JS_IsException(obj))
        return obj;
    r = calloc(1, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    qh_item_init(&r->b, QH_REQ, m);
    JS_SetOpaque(obj, r);
    r->b.cbs = JS_DupValue(ctx, argv[1]);
    if (qh_setup_request(ctx, r, argv[0]) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    mc = curl_multi_add_handle(m->multi, r->b.easy);
    if (mc != CURLM_OK) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx, "curl_multi_add_handle: %s", curl_multi_strerror(mc));
    }
    r->b.added = 1;
    r->b.self = JS_DupValue(ctx, obj);
    qh_item_update_alive(&r->b);
    return obj;
}

static QHReq *qh_req_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qh_req_class_id);
}

/* write(bytes): upload data of a streaming request */
static JSValue qh_req_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QHReq *r = qh_req_get(ctx, this_val);
    uint8_t *p;
    size_t len;
    if (!r)
        return JS_EXCEPTION;
    if (r->b.finished || r->up_done || !r->streaming)
        return JS_UNDEFINED;
    if (JS_IsString(argv[0])) {
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        int rc;
        if (!s)
            return JS_EXCEPTION;
        rc = qh_buf_append(&r->up, s, len);
        JS_FreeCString(ctx, s);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
    } else {
        p = qb_get_bytes(ctx, argv[0], &len);
        if (!p)
            return JS_EXCEPTION;
        if (qh_buf_append(&r->up, p, len) < 0)
            return JS_ThrowOutOfMemory(ctx);
    }
    if (len && r->send_paused) {
        r->send_paused = 0;
        r->want_pull = 0;
        r->pull_sent = 0;
        qh_req_apply_pause(r);
    }
    return JS_UNDEFINED;
}

static JSValue qh_req_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                             int magic)
{
    QHReq *r = qh_req_get(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* pause */
        if (!r->want_pause) {
            r->want_pause = 1;
            qh_req_apply_pause(r);
            qh_item_update_alive(&r->b);
        }
        break;
    case 1: /* resume */
        if (r->want_pause) {
            r->want_pause = 0;
            qh_req_apply_pause(r);
            qh_item_update_alive(&r->b);
        }
        break;
    case 2: /* abort */
        qh_item_finish(&r->b);
        break;
    case 3: /* end */
        if (!r->up_done) {
            r->up_done = 1;
            if (r->send_paused) {
                r->send_paused = 0;
                r->want_pull = 0;
                qh_req_apply_pause(r);
            }
        }
        break;
    case 4: /* ref */
        r->b.want_ref = 1;
        qh_item_update_alive(&r->b);
        break;
    case 5: /* unref */
        r->b.want_ref = 0;
        qh_item_update_alive(&r->b);
        break;
    case 6: /* hasRef */
        return JS_NewBool(ctx, r->b.counted_alive);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qh_req_proto_funcs[] = {
    JS_CFUNC_DEF("write", 1, qh_req_write),
    JS_CFUNC_MAGIC_DEF("pause", 0, qh_req_method, 0),
    JS_CFUNC_MAGIC_DEF("resume", 0, qh_req_method, 1),
    JS_CFUNC_MAGIC_DEF("abort", 0, qh_req_method, 2),
    JS_CFUNC_MAGIC_DEF("end", 0, qh_req_method, 3),
    JS_CFUNC_MAGIC_DEF("ref", 0, qh_req_method, 4),
    JS_CFUNC_MAGIC_DEF("unref", 0, qh_req_method, 5),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qh_req_method, 6),
};

/* ------------------------------------------------------------- WebSockets */

static void qh_ws_event(QHWs *ws, int type, uint8_t *data, size_t len)
{
    QHWsEv *ev = calloc(1, sizeof(*ev));
    if (!ev) {
        if (data)
            js_free_rt(ws->b.m->qr->rt, data);
        return;
    }
    ev->type = type;
    ev->data = data;
    ev->len = len;
    if (ws->ev_tail)
        ws->ev_tail->next = ev;
    else
        ws->ev_head = ev;
    ws->ev_tail = ev;
    qh_queue(&ws->b);
}

/* a copy of bytes in the runtime allocator (NULL for empty) */
static uint8_t *qh_rt_dup(JSRuntime *rt, const uint8_t *p, size_t len)
{
    uint8_t *d;
    if (!len)
        return NULL;
    d = js_malloc_rt(rt, len);
    if (d)
        memcpy(d, p, len);
    return d;
}

static void qh_ws_update(QHWs *ws)
{
    int ev = 0;
    if (!ws->sock)
        return;
    if (ws->open && !ws->closed) {
        if (!ws->paused)
            ev |= UV_READABLE;
        if (ws->out_head)
            ev |= UV_WRITABLE;
    }
    if (ws->sock->ws_events != ev) {
        ws->sock->ws_events = ev;
        qh_sock_update(ws->sock);
    }
}

static void qh_ws_close_event(QHWs *ws, int code, const char *reason, size_t rlen, int clean)
{
    QHWsEv *ev;
    if (ws->closed)
        return;
    ws->closed = 1;
    qh_ws_event(ws, QH_EV_CLOSE, qh_rt_dup(ws->b.m->qr->rt, (const uint8_t *)reason, rlen), rlen);
    ev = ws->ev_tail;
    if (ev && ev->type == QH_EV_CLOSE) {
        ev->code = code;
        ev->clean = clean;
    }
    qh_ws_update(ws);
}

/* the connection failed: error then close 1006 */
static void qh_ws_fail(QHWs *ws, CURLcode rc)
{
    if (ws->closed)
        return;
    qh_ws_event(ws, QH_EV_ERROR, NULL, 0);
    if (ws->ev_tail && ws->ev_tail->type == QH_EV_ERROR)
        ws->ev_tail->rc = rc;
    qh_ws_close_event(ws, 1006, "", 0, 0);
}

static void qh_ws_enqueue(QHWs *ws, unsigned int flags, const uint8_t *p, size_t len)
{
    QHWsOut *o = malloc(sizeof(*o) + len);
    if (!o)
        return;
    o->next = NULL;
    o->flags = flags;
    o->len = len;
    o->off = 0;
    if (len)
        memcpy(o->data, p, len);
    if (ws->out_tail)
        ws->out_tail->next = o;
    else
        ws->out_head = o;
    ws->out_tail = o;
    ws->buffered += len;
}

/* send the queued frames until the socket would block */
static int qh_ws_flush(QHWs *ws)
{
    if (ws->b.finished || !ws->open || !ws->b.easy)
        return 0;
    while (ws->out_head) {
        QHWsOut *o = ws->out_head;
        size_t sent = 0;
        CURLcode rc =
            curl_ws_send(ws->b.easy, o->data + o->off, o->len - o->off, &sent, 0, o->flags);
        if (rc == CURLE_AGAIN)
            break;
        if (rc != CURLE_OK) {
            qh_ws_fail(ws, rc);
            return -1;
        }
        o->off += sent;
        ws->buffered -= sent;
        if (o->off < o->len)
            continue;
        ws->out_head = o->next;
        if (!ws->out_head)
            ws->out_tail = NULL;
        free(o);
    }
    if (!ws->out_head && ws->close_sent && ws->close_received && !ws->closed)
        qh_ws_close_event(ws, ws->close_code, ws->close_reason, ws->close_reason_len, 1);
    qh_ws_update(ws);
    return 0;
}

static void qh_ws_on_close_frame(QHWs *ws)
{
    size_t rlen = 0;
    ws->close_received = 1;
    if (ws->ctrl_len >= 2) {
        ws->close_code = (ws->ctrl[0] << 8) | ws->ctrl[1];
        rlen = ws->ctrl_len - 2;
        if (rlen > sizeof(ws->close_reason))
            rlen = sizeof(ws->close_reason);
        memcpy(ws->close_reason, ws->ctrl + 2, rlen);
    } else {
        ws->close_code = 1005;
    }
    ws->close_reason_len = rlen;
    if (!ws->close_sent) {
        /* echo the status code (none for 1005) */
        ws->close_sent = 1;
        qh_ws_enqueue(ws, CURLWS_CLOSE, ws->ctrl, ws->ctrl_len >= 2 ? 2 : 0);
    }
    qh_ws_flush(ws);
}

/* receive the frames until the socket would block */
static void qh_ws_read(QHWs *ws)
{
    JSRuntime *rt = ws->b.m->qr->rt;
    uint8_t tmp[16384];

    while (!ws->b.finished && ws->open && !ws->closed && !ws->paused) {
        size_t n = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(ws->b.easy, tmp, sizeof(tmp), &n, &meta);
        int fl;
        if (rc == CURLE_AGAIN)
            break;
        if (rc == CURLE_GOT_NOTHING) {
            /* the server closed the connection */
            if (ws->close_received)
                qh_ws_close_event(ws, ws->close_code, ws->close_reason, ws->close_reason_len,
                                  ws->close_sent);
            else
                qh_ws_close_event(ws, 1006, "", 0, 0);
            break;
        }
        if (rc != CURLE_OK || !meta) {
            qh_ws_fail(ws, rc != CURLE_OK ? rc : CURLE_RECV_ERROR);
            break;
        }
        fl = meta->flags;
        if (fl & (CURLWS_TEXT | CURLWS_BINARY)) {
            if (!ws->in_msg) {
                ws->in_msg = 1;
                ws->msg_binary = (fl & CURLWS_BINARY) != 0;
                ws->msg.len = 0;
            }
            if (qh_rbuf_append(rt, &ws->msg, tmp, n) < 0) {
                qh_ws_fail(ws, CURLE_OUT_OF_MEMORY);
                break;
            }
            if (meta->bytesleft == 0 && !(fl & CURLWS_CONT)) {
                size_t len = ws->msg.len;
                uint8_t *data = ws->msg.data;
                int binary = ws->msg_binary;
                ws->msg.data = NULL;
                ws->msg.len = ws->msg.cap = 0;
                ws->in_msg = 0;
                if (!len && data) {
                    js_free_rt(rt, data);
                    data = NULL;
                }
                qh_ws_event(ws, QH_EV_MESSAGE, data, len);
                if (ws->ev_tail && ws->ev_tail->type == QH_EV_MESSAGE)
                    ws->ev_tail->binary = binary;
            }
        } else if (fl & (CURLWS_PING | CURLWS_PONG | CURLWS_CLOSE)) {
            size_t c;
            if (meta->offset == 0)
                ws->ctrl_len = 0;
            c = n;
            if (ws->ctrl_len + c > sizeof(ws->ctrl))
                c = sizeof(ws->ctrl) - ws->ctrl_len;
            memcpy(ws->ctrl + ws->ctrl_len, tmp, c);
            ws->ctrl_len += c;
            if (meta->bytesleft == 0) {
                if (fl & CURLWS_PING) {
                    if (!ws->close_sent) {
                        qh_ws_enqueue(ws, CURLWS_PONG, ws->ctrl, ws->ctrl_len);
                        qh_ws_flush(ws);
                    }
                    qh_ws_event(ws, QH_EV_PING, qh_rt_dup(rt, ws->ctrl, ws->ctrl_len),
                                ws->ctrl_len);
                } else if (fl & CURLWS_PONG) {
                    qh_ws_event(ws, QH_EV_PONG, qh_rt_dup(rt, ws->ctrl, ws->ctrl_len),
                                ws->ctrl_len);
                } else {
                    qh_ws_on_close_frame(ws);
                }
            }
        }
    }
}

static void qh_ws_connected(QHWs *ws, CURLcode rc)
{
    QHItem *it = &ws->b;
    QHMulti *m = it->m;
    curl_socket_t fd = CURL_SOCKET_BAD;
    long code = 0;
    QHSock *sk;

    it->done = 1;
    it->result = rc;
    curl_easy_getinfo(it->easy, CURLINFO_RESPONSE_CODE, &code);
    if (code > 0)
        it->status = code;
    if (rc != CURLE_OK) {
        qh_ws_fail(ws, rc);
        return;
    }
    if (curl_easy_getinfo(it->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK ||
        fd == CURL_SOCKET_BAD) {
        qh_ws_fail(ws, CURLE_COULDNT_CONNECT);
        return;
    }
    sk = qh_sock_find(m, fd);
    if (!sk)
        sk = qh_sock_new(m, fd);
    if (!sk) {
        qh_ws_fail(ws, CURLE_OUT_OF_MEMORY);
        return;
    }
    ws->fd = fd;
    ws->sock = sk;
    sk->ws = ws;
    ws->open = 1;
    qh_ws_event(ws, QH_EV_OPEN, NULL, 0);
    qh_ws_flush(ws);
    qh_ws_update(ws);
    /* frames received with the handshake are buffered by libcurl */
    qh_ws_read(ws);
}

static void qh_ws_deliver(QHWs *ws)
{
    QHItem *it = &ws->b;
    JSContext *ctx = it->m->qr->ctx;
    JSRuntime *rt = it->m->qr->rt;
    JSValue self = JS_DupValue(ctx, it->self);

    while (ws->ev_head && !it->finished) {
        QHWsEv *ev = ws->ev_head;
        JSValue args[5];
        int argc = 0, i, close = 0;
        const char *name = NULL;

        ws->ev_head = ev->next;
        if (!ws->ev_head)
            ws->ev_tail = NULL;
        switch (ev->type) {
        case QH_EV_OPEN:
            name = "onOpen";
            args[0] = JS_NewInt32(ctx, (int32_t)it->status);
            args[1] = qh_headers_array(ctx, it);
            args[2] = qh_effective_url(ctx, it);
            argc = 3;
            break;
        case QH_EV_MESSAGE:
            name = "onMessage";
            if (ev->binary) {
                if (ev->data) {
                    args[0] = qb_new_buffer_owned(ctx, ev->data, ev->len);
                    ev->data = NULL;
                } else {
                    args[0] = qb_new_buffer(ctx, (const uint8_t *)"", 0);
                }
            } else {
                args[0] = JS_NewStringLen(ctx, ev->data ? (const char *)ev->data : "", ev->len);
            }
            args[1] = JS_NewBool(ctx, ev->binary);
            argc = 2;
            break;
        case QH_EV_PING:
        case QH_EV_PONG:
            name = ev->type == QH_EV_PING ? "onPing" : "onPong";
            args[0] = qb_new_buffer(ctx, ev->data ? ev->data : (const uint8_t *)"", ev->len);
            argc = 1;
            break;
        case QH_EV_ERROR:
            name = "onError";
            args[0] = qh_error_info(ctx, it, ev->rc);
            argc = 1;
            break;
        case QH_EV_CLOSE:
            name = "onClose";
            args[0] = JS_NewInt32(ctx, ev->code);
            args[1] = JS_NewStringLen(ctx, ev->data ? (const char *)ev->data : "", ev->len);
            args[2] = JS_NewBool(ctx, ev->clean);
            argc = 3;
            close = 1;
            break;
        }
        if (ev->data)
            js_free_rt(rt, ev->data);
        free(ev);
        if (name)
            qh_emit(it, name, argc, (JSValueConst *)args);
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, args[i]);
        if (close)
            qh_item_finish(it);
    }
    JS_FreeValue(ctx, self);
}

/* websocket(options, handlers) */
static JSValue qh_websocket(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    QHMulti *m = qh_state(ctx, data);
    QHWs *ws;
    JSValue obj;
    CURLMcode mc;
    int err = 0;
    const char *proxy;

    if (!m)
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "websocket(options, handlers) expects two objects");
    obj = JS_NewObjectClass(ctx, qh_ws_class_id);
    if (JS_IsException(obj))
        return obj;
    ws = calloc(1, sizeof(*ws));
    if (!ws) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    qh_item_init(&ws->b, QH_WS, m);
    ws->fd = CURL_SOCKET_BAD;
    JS_SetOpaque(obj, ws);
    ws->b.cbs = JS_DupValue(ctx, argv[1]);
    if (qh_setup_common(ctx, &ws->b, argv[0]) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    curl_easy_setopt(ws->b.easy, CURLOPT_PROTOCOLS_STR, "ws,wss");
    curl_easy_setopt(ws->b.easy, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(ws->b.easy, CURLOPT_WS_OPTIONS, (long)CURLWS_NOAUTOPONG);
    proxy = qh_get_cstr(ctx, argv[0], "proxy", NULL, &err);
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (proxy) {
        if (proxy[0])
            curl_easy_setopt(ws->b.easy, CURLOPT_HTTPPROXYTUNNEL, 1L);
        JS_FreeCString(ctx, proxy);
    }
    mc = curl_multi_add_handle(m->multi, ws->b.easy);
    if (mc != CURLM_OK) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx, "curl_multi_add_handle: %s", curl_multi_strerror(mc));
    }
    ws->b.added = 1;
    ws->b.self = JS_DupValue(ctx, obj);
    qh_item_update_alive(&ws->b);
    return obj;
}

static QHWs *qh_ws_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qh_ws_class_id);
}

/* the bytes of a string (UTF-8) or a buffer source, for a frame */
static int qh_ws_send_value(JSContext *ctx, QHWs *ws, JSValueConst v, unsigned int flags)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        qh_ws_enqueue(ws, flags, NULL, 0);
    } else if (JS_IsString(v)) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        if (!s)
            return -1;
        qh_ws_enqueue(ws, flags, (const uint8_t *)s, len);
        JS_FreeCString(ctx, s);
    } else {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, v, &len);
        if (!p)
            return -1;
        qh_ws_enqueue(ws, flags, p, len);
    }
    return 0;
}

/* send(data, binary), ping(data), pong(data) */
static JSValue qh_ws_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic)
{
    QHWs *ws = qh_ws_get(ctx, this_val);
    unsigned int flags;
    if (!ws)
        return JS_EXCEPTION;
    if (ws->b.finished || !ws->open || ws->closed || ws->close_sent)
        return JS_UNDEFINED;
    switch (magic) {
    case 0:
        flags = (argc > 1 && JS_ToBool(ctx, argv[1])) ? CURLWS_BINARY : CURLWS_TEXT;
        break;
    case 1:
        flags = CURLWS_PING;
        break;
    default:
        flags = CURLWS_PONG;
        break;
    }
    if (qh_ws_send_value(ctx, ws, argc > 0 ? argv[0] : JS_UNDEFINED, flags) < 0)
        return JS_EXCEPTION;
    qh_ws_flush(ws);
    qh_schedule(ws->b.m);
    return JS_UNDEFINED;
}

/* close(code (< 0: none), reason) */
static JSValue qh_ws_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QHWs *ws = qh_ws_get(ctx, this_val);
    int32_t code = -1;
    uint8_t payload[125];
    size_t plen = 0;
    if (!ws)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &code, argv[0]))
        return JS_EXCEPTION;
    if (ws->b.finished || !ws->open || ws->closed || ws->close_sent)
        return JS_UNDEFINED;
    if (code >= 0) {
        payload[0] = (uint8_t)(code >> 8);
        payload[1] = (uint8_t)code;
        plen = 2;
        if (argc > 1 && JS_IsString(argv[1])) {
            size_t len;
            const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
            if (!s)
                return JS_EXCEPTION;
            if (len > sizeof(payload) - 2)
                len = sizeof(payload) - 2;
            memcpy(payload + 2, s, len);
            plen += len;
            JS_FreeCString(ctx, s);
        }
    }
    ws->close_sent = 1;
    qh_ws_enqueue(ws, CURLWS_CLOSE, payload, plen);
    qh_ws_flush(ws);
    qh_schedule(ws->b.m);
    return JS_UNDEFINED;
}

static JSValue qh_ws_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic)
{
    QHWs *ws = qh_ws_get(ctx, this_val);
    if (!ws)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* terminate */
        qh_item_finish(&ws->b);
        break;
    case 1: /* pause */
        ws->paused = 1;
        qh_ws_update(ws);
        break;
    case 2: /* resume */
        if (ws->paused) {
            ws->paused = 0;
            qh_ws_update(ws);
            qh_ws_read(ws);
            qh_schedule(ws->b.m);
        }
        break;
    case 3: /* ref */
        ws->b.want_ref = 1;
        qh_item_update_alive(&ws->b);
        break;
    case 4: /* unref */
        ws->b.want_ref = 0;
        qh_item_update_alive(&ws->b);
        break;
    case 5: /* hasRef */
        return JS_NewBool(ctx, ws->b.counted_alive);
    }
    return JS_UNDEFINED;
}

static JSValue qh_ws_get_buffered(JSContext *ctx, JSValueConst this_val)
{
    QHWs *ws = qh_ws_get(ctx, this_val);
    if (!ws)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)ws->buffered);
}

static const JSCFunctionListEntry qh_ws_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("send", 2, qh_ws_send, 0),
    JS_CFUNC_MAGIC_DEF("ping", 1, qh_ws_send, 1),
    JS_CFUNC_MAGIC_DEF("pong", 1, qh_ws_send, 2),
    JS_CFUNC_DEF("close", 2, qh_ws_close),
    JS_CFUNC_MAGIC_DEF("terminate", 0, qh_ws_method, 0),
    JS_CFUNC_MAGIC_DEF("pause", 0, qh_ws_method, 1),
    JS_CFUNC_MAGIC_DEF("resume", 0, qh_ws_method, 2),
    JS_CFUNC_MAGIC_DEF("ref", 0, qh_ws_method, 3),
    JS_CFUNC_MAGIC_DEF("unref", 0, qh_ws_method, 4),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qh_ws_method, 5),
    JS_CGETSET_DEF("bufferedAmount", qh_ws_get_buffered, NULL),
};

/* ----------------------------------------------------------------- module */

static QHMulti *qh_multi_new(JSContext *ctx)
{
    QBRuntime *qr = qb_get(ctx);
    QHMulti *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->qr = qr;
    m->multi = curl_multi_init();
    if (!m->multi) {
        free(m);
        return NULL;
    }
    curl_multi_setopt(m->multi, CURLMOPT_SOCKETFUNCTION, qh_socket_cb);
    curl_multi_setopt(m->multi, CURLMOPT_SOCKETDATA, (void *)m);
    curl_multi_setopt(m->multi, CURLMOPT_TIMERFUNCTION, qh_timer_func);
    curl_multi_setopt(m->multi, CURLMOPT_TIMERDATA, (void *)m);
    uv_timer_init(qr->loop, &m->timer);
    m->timer.data = m;
    uv_timer_init(qr->loop, &m->dispatch);
    m->dispatch.data = m;
    uv_async_init(qr->loop, &m->alive, qh_alive_cb);
    m->alive.data = m;
    /* libcurl's timer runs only for the transfers, which keep the loop
       alive through m->alive; pending deliveries keep it alive */
    uv_unref((uv_handle_t *)&m->timer);
    uv_unref((uv_handle_t *)&m->alive);
    m->handles = 3;
    m->refs = 1;
    return m;
}

static int qh_http_module_init(JSContext *ctx, JSModuleDef *mod)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue state, proto;
    QHMulti *m;

    pthread_once(&qh_curl_once, qh_curl_global_init);
    JS_NewClassID(&qh_state_class_id);
    JS_NewClassID(&qh_req_class_id);
    JS_NewClassID(&qh_ws_class_id);
    if (!JS_IsRegisteredClass(rt, qh_state_class_id)) {
        if (JS_NewClass(rt, qh_state_class_id, &qh_state_class) < 0 ||
            JS_NewClass(rt, qh_req_class_id, &qh_req_class) < 0 ||
            JS_NewClass(rt, qh_ws_class_id, &qh_ws_class) < 0)
            return -1;
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, qh_req_proto_funcs, countof(qh_req_proto_funcs));
        JS_SetClassProto(ctx, qh_req_class_id, proto);
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, qh_ws_proto_funcs, countof(qh_ws_proto_funcs));
        JS_SetClassProto(ctx, qh_ws_class_id, proto);
    }

    state = JS_NewObjectClass(ctx, qh_state_class_id);
    if (JS_IsException(state))
        return -1;
    m = qh_multi_new(ctx);
    if (!m) {
        JS_FreeValue(ctx, state);
        JS_ThrowInternalError(ctx, "curl_multi_init failed");
        return -1;
    }
    JS_SetOpaque(state, m);
    if (JS_SetModuleExport(ctx, mod, "request",
                           JS_NewCFunctionData(ctx, qh_request, 2, 0, 1, (JSValueConst *)&state)) <
            0 ||
        JS_SetModuleExport(
            ctx, mod, "websocket",
            JS_NewCFunctionData(ctx, qh_websocket, 2, 0, 1, (JSValueConst *)&state)) < 0 ||
        JS_SetModuleExport(ctx, mod, "curlVersion", JS_NewString(ctx, curl_version())) < 0) {
        JS_FreeValue(ctx, state);
        return -1;
    }
    JS_FreeValue(ctx, state);
    return 0;
}

JSModuleDef *qb_init_http(JSContext *ctx, const char *name)
{
    JSModuleDef *mod = JS_NewCModule(ctx, name, qh_http_module_init);
    if (!mod)
        return NULL;
    if (JS_AddModuleExport(ctx, mod, "request") < 0 ||
        JS_AddModuleExport(ctx, mod, "websocket") < 0 ||
        JS_AddModuleExport(ctx, mod, "curlVersion") < 0)
        return NULL;
    return mod;
}
