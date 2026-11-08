/*
 * qbun:worker — worker threads, message ports, BroadcastChannel and the
 * vm contexts.
 *
 * Message passing: a channel joins endpoints (two for a MessageChannel,
 * any number for a BroadcastChannel) behind one mutex. Every endpoint has
 * its own queue of messages; the JS object owning an endpoint (a port
 * handle) belongs to one runtime and is woken by an uv_async_t of that
 * runtime's loop. Messages are written with JS_StructuredSerialize() (HTML
 * structured clone, SharedArrayBuffers shared) into malloc memory, so any
 * thread can read them. The clone host of qbun (also the host of the
 * global structuredClone() and of serialize()/deserialize()) keeps Blob
 * and File objects and writes the transferred MessagePorts as indexes: a
 * transferred port moves its endpoint, queue included, into the message;
 * the receiver binds it to a new handle wrapped by the createPort()
 * function of setCloneHooks().
 *
 * A worker runs its own QBRuntime and loop on an uv thread. The parent
 * keeps a handle (exit notification through an uv_async_t of its loop)
 * and the parent side of the worker's internal channel; terminate() sets
 * the worker runtime's 'exiting' flag (seen by the interrupt handler) and
 * stops its loop through another uv_async_t.
 *
 * Process wide state (all synchronized): the thread id counter and the
 * BroadcastChannel registry.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "qbun.h"

/* ------------------------------------------------- SharedArrayBuffers */

typedef union QBSABHeader {
    _Atomic int ref_count;
    max_align_t align;
} QBSABHeader;

static void *qb_sab_alloc(void *opaque, size_t size)
{
    QBSABHeader *h = malloc(sizeof(QBSABHeader) + size);
    if (!h)
        return NULL;
    atomic_init(&h->ref_count, 1);
    return h + 1;
}

static void qb_sab_free(void *opaque, void *ptr)
{
    QBSABHeader *h = (QBSABHeader *)ptr - 1;
    if (atomic_fetch_sub(&h->ref_count, 1) == 1)
        free(h);
}

static void qb_sab_dup(void *opaque, void *ptr)
{
    QBSABHeader *h = (QBSABHeader *)ptr - 1;
    atomic_fetch_add(&h->ref_count, 1);
}

/* ------------------------------------------------- structured clone host */

/* kinds of the host objects (first integer of their data) */
enum {
    QB_CLONE_PORT = 1, /* index in the transferred ports */
    QB_CLONE_BLOB,     /* type, bytes */
    QB_CLONE_FILE,     /* type, name, lastModified, bytes */
    QB_CLONE_BUFFER,   /* byte offset, length, ArrayBuffer (serialize() with 'buffers') */
    QB_CLONE_EXTERNAL, /* id of an ArrayBuffer transferred out of band (node:v8) */
};

typedef struct QBCloneState QBCloneState;

/* the opaque of one serialization or deserialization */
typedef struct QBCloneCall {
    QBCloneState *st;
    JSValue *ports;         /* the transferred MessagePorts, sent or received */
    int nports;
    int buffers;            /* the Buffers keep their prototype */
    JSValueConst externals; /* Map: ArrayBuffer -> id (write), id -> ArrayBuffer (read) */
} QBCloneCall;

/* per runtime */
struct QBCloneState {
    QBRuntime *qr;
    QBCloneCall dflt;       /* opaque of the runtime host: structuredClone() */
    JSValue port_proto;     /* MessagePort.prototype (setCloneHooks()) */
    JSValue create_port;    /* createPort(handle) (setCloneHooks()) */
    JSValue marks;          /* WeakSet of the markUncloneable() objects or undefined */
    JSValue marks_has;
    JSValue object_proto;
    /* node:buffer, loaded on first use */
    int buffer_loaded;
    JSValue blob_ctor, file_ctor, blob_proto, file_proto, get_blob_bytes;
};

/* Blob, File and getBlobBytes of node:buffer. Return -1 (no exception)
   if they are not available, e.g. while node:buffer is evaluated */
static int qb_clone_load_buffer(JSContext *ctx, QBCloneState *st)
{
    JSValue ns, blob, file, get, bp = JS_UNDEFINED, fp = JS_UNDEFINED;

    if (st->buffer_loaded)
        return 0;
    ns = JS_LoadModuleSync(ctx, "", "node:buffer", JS_UNDEFINED);
    if (JS_IsException(ns)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    blob = JS_GetPropertyStr(ctx, ns, "Blob");
    file = JS_GetPropertyStr(ctx, ns, "File");
    get = JS_GetPropertyStr(ctx, ns, "getBlobBytes");
    JS_FreeValue(ctx, ns);
    if (JS_IsFunction(ctx, blob) && JS_IsFunction(ctx, file) && JS_IsFunction(ctx, get)) {
        bp = JS_GetPropertyStr(ctx, blob, "prototype");
        fp = JS_GetPropertyStr(ctx, file, "prototype");
    }
    if (!JS_IsObject(bp) || !JS_IsObject(fp)) {
        /* uninitialized exports */
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, blob);
        JS_FreeValue(ctx, file);
        JS_FreeValue(ctx, get);
        JS_FreeValue(ctx, bp);
        JS_FreeValue(ctx, fp);
        return -1;
    }
    st->blob_ctor = blob;
    st->file_ctor = file;
    st->get_blob_bytes = get;
    st->blob_proto = bp;
    st->file_proto = fp;
    st->buffer_loaded = 1;
    return 0;
}

static JSValue qb_map_get(JSContext *ctx, JSValueConst map, JSValueConst key)
{
    JSValue fn, v;
    fn = JS_GetPropertyStr(ctx, map, "get");
    if (JS_IsException(fn))
        return fn;
    v = JS_Call(ctx, fn, map, 1, &key);
    JS_FreeValue(ctx, fn);
    return v;
}

static int qb_same_object(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static int qb_clone_write_blob(JSContext *ctx, JSStructuredWriter *w, QBCloneState *st,
                               JSValueConst obj, int kind)
{
    JSValue type, name = JS_UNDEFINED, last = JS_UNDEFINED, bytes = JS_UNDEFINED;
    uint8_t *data;
    size_t len;
    int ret = -1;

    type = JS_GetPropertyStr(ctx, obj, "type");
    if (JS_IsException(type))
        return -1;
    if (kind == QB_CLONE_FILE) {
        name = JS_GetPropertyStr(ctx, obj, "name");
        if (JS_IsException(name))
            goto done;
        last = JS_GetPropertyStr(ctx, obj, "lastModified");
        if (JS_IsException(last))
            goto done;
    }
    bytes = JS_Call(ctx, st->get_blob_bytes, JS_UNDEFINED, 1, &obj);
    if (JS_IsException(bytes))
        goto done;
    if (JS_StructuredWriteUint32(w, kind) || JS_StructuredWriteValue(w, type))
        goto done;
    if (kind == QB_CLONE_FILE &&
        (JS_StructuredWriteValue(w, name) || JS_StructuredWriteValue(w, last)))
        goto done;
    /* no JS call between qb_get_bytes() and the copy */
    data = qb_get_bytes(ctx, bytes, &len);
    if (!data || JS_StructuredWriteBytes(w, data, len))
        goto done;
    ret = 1;
 done:
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, last);
    JS_FreeValue(ctx, bytes);
    return ret;
}

static int qb_clone_write(JSContext *ctx, JSStructuredWriter *w, JSValueConst obj,
                          void *opaque)
{
    QBCloneCall *c = opaque;
    QBCloneState *st = c->st;
    JSValue proto, next, v;
    uint32_t id;
    size_t off, len, bpe;
    int i, kind = 0, depth, ret;

    if (!JS_IsUndefined(st->marks)) {
        v = JS_Call(ctx, st->marks_has, st->marks, 1, &obj);
        if (JS_IsException(v))
            return -1;
        ret = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (ret) {
            JS_ThrowDataCloneError(ctx, "The object could not be cloned.");
            return -1;
        }
    }
    for(i = 0; i < c->nports; i++) {
        if (qb_same_object(c->ports[i], obj)) {
            if (JS_StructuredWriteUint32(w, QB_CLONE_PORT) || JS_StructuredWriteUint32(w, i))
                return -1;
            return 1;
        }
    }
    if (!JS_IsUndefined(c->externals) && JS_IsArrayBuffer(obj)) {
        v = qb_map_get(ctx, c->externals, obj);
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            ret = JS_ToUint32(ctx, &id, v);
            JS_FreeValue(ctx, v);
            if (ret || JS_StructuredWriteUint32(w, QB_CLONE_EXTERNAL) ||
                JS_StructuredWriteUint32(w, id))
                return -1;
            return 1;
        }
    }
    proto = JS_GetPrototype(ctx, obj);
    if (JS_IsException(proto))
        return -1;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return 0;
    }
    if (JS_IsUndefined(st->object_proto)) {
        v = JS_NewObject(ctx);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, proto);
            return -1;
        }
        st->object_proto = JS_GetPrototype(ctx, v);
        JS_FreeValue(ctx, v);
    }
    /* plain object: the common case */
    if (qb_same_object(proto, st->object_proto)) {
        JS_FreeValue(ctx, proto);
        return 0;
    }
    qb_clone_load_buffer(ctx, st);
    for(depth = 0; depth < 64 && JS_IsObject(proto); depth++) {
        if (qb_same_object(proto, st->port_proto)) {
            JS_FreeValue(ctx, proto);
            JS_ThrowDataCloneError(ctx, "Object that needs transfer was found in message but not listed in transferList");
            return -1;
        }
        if (st->buffer_loaded && qb_same_object(proto, st->file_proto)) {
            kind = QB_CLONE_FILE;
            break;
        }
        if (st->buffer_loaded && qb_same_object(proto, st->blob_proto)) {
            kind = QB_CLONE_BLOB;
            break;
        }
        if (c->buffers && qb_same_object(proto, st->qr->buffer_proto)) {
            kind = QB_CLONE_BUFFER;
            break;
        }
        next = JS_GetPrototype(ctx, proto);
        JS_FreeValue(ctx, proto);
        proto = next;
        if (JS_IsException(proto))
            return -1;
    }
    JS_FreeValue(ctx, proto);
    switch(kind) {
    case QB_CLONE_BLOB:
    case QB_CLONE_FILE:
        return qb_clone_write_blob(ctx, w, st, obj, kind);
    case QB_CLONE_BUFFER:
        v = JS_GetTypedArrayBuffer(ctx, obj, &off, &len, &bpe);
        if (JS_IsException(v))
            return -1;
        ret = JS_StructuredWriteUint32(w, QB_CLONE_BUFFER) ||
            JS_StructuredWriteUint32(w, off) || JS_StructuredWriteUint32(w, len) ||
            JS_StructuredWriteValue(w, v);
        JS_FreeValue(ctx, v);
        return ret ? -1 : 1;
    default:
        return 0;
    }
}

static JSValue qb_clone_read_blob(JSContext *ctx, JSStructuredReader *r, QBCloneState *st,
                                  int kind)
{
    JSValue type, name = JS_UNDEFINED, last = JS_UNDEFINED, parts = JS_UNDEFINED;
    JSValue opts = JS_UNDEFINED, args[3], ret = JS_EXCEPTION;
    const uint8_t *data;
    size_t len;

    type = JS_StructuredReadValue(r);
    if (JS_IsException(type))
        return type;
    if (kind == QB_CLONE_FILE) {
        name = JS_StructuredReadValue(r);
        if (JS_IsException(name))
            goto done;
        last = JS_StructuredReadValue(r);
        if (JS_IsException(last))
            goto done;
    }
    if (JS_StructuredReadBytes(r, &data, &len))
        goto done;
    parts = JS_NewArray(ctx);
    if (JS_IsException(parts) ||
        JS_SetPropertyUint32(ctx, parts, 0, JS_NewUint8ArrayCopy(ctx, data, len)) < 0)
        goto done;
    if (qb_clone_load_buffer(ctx, st)) {
        JS_ThrowTypeError(ctx, "Blob is not available");
        goto done;
    }
    opts = JS_NewObject(ctx);
    if (JS_IsException(opts) ||
        JS_SetPropertyStr(ctx, opts, "type", JS_DupValue(ctx, type)) < 0)
        goto done;
    if (kind == QB_CLONE_FILE) {
        if (JS_SetPropertyStr(ctx, opts, "lastModified", JS_DupValue(ctx, last)) < 0)
            goto done;
        args[0] = parts;
        args[1] = name;
        args[2] = opts;
        ret = JS_CallConstructor(ctx, st->file_ctor, 3, (JSValueConst *)args);
    } else {
        args[0] = parts;
        args[1] = opts;
        ret = JS_CallConstructor(ctx, st->blob_ctor, 2, (JSValueConst *)args);
    }
 done:
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, last);
    JS_FreeValue(ctx, parts);
    JS_FreeValue(ctx, opts);
    return ret;
}

static JSValue qb_clone_read(JSContext *ctx, JSStructuredReader *r, void *opaque)
{
    QBCloneCall *c = opaque;
    uint32_t kind, a, b;
    JSValue v, args[3];
    size_t len;

    if (JS_StructuredReadUint32(r, &kind))
        return JS_EXCEPTION;
    switch(kind) {
    case QB_CLONE_PORT:
        if (JS_StructuredReadUint32(r, &a))
            return JS_EXCEPTION;
        if (a >= (uint32_t)c->nports)
            return JS_ThrowDataCloneError(ctx, "Invalid MessagePort in the cloned data");
        return JS_DupValue(ctx, c->ports[a]);
    case QB_CLONE_BLOB:
    case QB_CLONE_FILE:
        return qb_clone_read_blob(ctx, r, c->st, kind);
    case QB_CLONE_BUFFER:
        if (JS_StructuredReadUint32(r, &a) || JS_StructuredReadUint32(r, &b))
            return JS_EXCEPTION;
        args[0] = JS_StructuredReadValue(r);
        if (JS_IsException(args[0]))
            return JS_EXCEPTION;
        args[1] = JS_NewUint32(ctx, a);
        args[2] = JS_NewUint32(ctx, b);
        v = JS_NewTypedArray(ctx, 3, (JSValueConst *)args, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, args[0]);
        return qb_to_buffer(ctx, v);
    case QB_CLONE_EXTERNAL:
        if (JS_StructuredReadUint32(r, &a))
            return JS_EXCEPTION;
        if (JS_IsUndefined(c->externals))
            return JS_ThrowDataCloneError(ctx, "Unknown ArrayBuffer transfer id %u", a);
        args[0] = JS_NewUint32(ctx, a);
        v = qb_map_get(ctx, c->externals, args[0]);
        if (JS_IsException(v))
            return v;
        /* an ArrayBuffer or a SharedArrayBuffer */
        if (!JS_GetArrayBuffer(ctx, &len, v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, v);
            return JS_ThrowDataCloneError(ctx, "Unknown ArrayBuffer transfer id %u", a);
        }
        return v;
    default:
        return JS_ThrowDataCloneError(ctx, "Unable to deserialize cloned data.");
    }
}

static int qb_clone_transfer(JSContext *ctx, JSValueConst obj, void *opaque)
{
    QBCloneCall *c = opaque;
    JSValue proto;
    int i, is_port;

    for(i = 0; i < c->nports; i++) {
        if (qb_same_object(c->ports[i], obj))
            return 1;
    }
    proto = JS_GetPrototype(ctx, obj);
    if (JS_IsException(proto))
        return -1;
    is_port = qb_same_object(proto, c->st->port_proto);
    JS_FreeValue(ctx, proto);
    if (is_port) {
        JS_ThrowDataCloneError(ctx, "A MessagePort can only be transferred by postMessage()");
        return -1;
    }
    return 0;
}

static const JSStructuredCloneHost qb_clone_host = {
    .write = qb_clone_write,
    .read = qb_clone_read,
    .transfer = qb_clone_transfer,
};

/* the clone state of the runtime of 'ctx', NULL after its exit */
static QBCloneState *qb_clone_state(JSContext *ctx)
{
    void *opaque;
    if (JS_GetStructuredCloneHost(JS_GetRuntime(ctx), &opaque) != &qb_clone_host)
        return NULL;
    return ((QBCloneCall *)opaque)->st;
}

static void qb_clone_cleanup(void *arg)
{
    QBCloneState *st = arg;
    JSRuntime *rt = st->qr->rt;

    JS_SetStructuredCloneHost(rt, NULL, NULL);
    JS_FreeValueRT(rt, st->port_proto);
    JS_FreeValueRT(rt, st->create_port);
    JS_FreeValueRT(rt, st->marks);
    JS_FreeValueRT(rt, st->marks_has);
    JS_FreeValueRT(rt, st->object_proto);
    JS_FreeValueRT(rt, st->blob_ctor);
    JS_FreeValueRT(rt, st->file_ctor);
    JS_FreeValueRT(rt, st->blob_proto);
    JS_FreeValueRT(rt, st->file_proto);
    JS_FreeValueRT(rt, st->get_blob_bytes);
    free(st);
}

static void qb_clone_init(QBRuntime *qr)
{
    QBCloneState *st = calloc(1, sizeof(*st));
    if (!st)
        return;
    st->qr = qr;
    st->dflt.st = st;
    st->dflt.externals = JS_UNDEFINED;
    st->port_proto = JS_UNDEFINED;
    st->create_port = JS_UNDEFINED;
    st->marks = JS_UNDEFINED;
    st->marks_has = JS_UNDEFINED;
    st->object_proto = JS_UNDEFINED;
    st->blob_ctor = JS_UNDEFINED;
    st->file_ctor = JS_UNDEFINED;
    st->blob_proto = JS_UNDEFINED;
    st->file_proto = JS_UNDEFINED;
    st->get_blob_bytes = JS_UNDEFINED;
    JS_SetStructuredCloneHost(qr->rt, &qb_clone_host, &st->dflt);
    qb_add_cleanup_hook(qr, qb_clone_cleanup, st);
}

/* setCloneHooks(MessagePort, createPort) */
static JSValue qb_worker_setCloneHooks(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    QBCloneState *st = qb_clone_state(ctx);
    JSValue proto;

    if (!st)
        return JS_UNDEFINED;
    proto = JS_GetPropertyStr(ctx, argv[0], "prototype");
    if (JS_IsException(proto))
        return proto;
    JS_FreeValue(ctx, st->port_proto);
    st->port_proto = proto;
    JS_FreeValue(ctx, st->create_port);
    st->create_port = JS_DupValue(ctx, argv[1]);
    return JS_UNDEFINED;
}

/* markUncloneable(object): serializing it throws a DataCloneError */
static JSValue qb_worker_markUncloneable(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    QBCloneState *st = qb_clone_state(ctx);
    JSValue global, ctor, set, fn, ret;

    if (!st || !JS_IsObject(argv[0]))
        return JS_UNDEFINED;
    if (JS_IsUndefined(st->marks)) {
        global = JS_GetGlobalObject(ctx);
        ctor = JS_GetPropertyStr(ctx, global, "WeakSet");
        JS_FreeValue(ctx, global);
        if (JS_IsException(ctor))
            return ctor;
        set = JS_CallConstructor(ctx, ctor, 0, NULL);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(set))
            return set;
        fn = JS_GetPropertyStr(ctx, set, "has");
        if (JS_IsException(fn)) {
            JS_FreeValue(ctx, set);
            return fn;
        }
        st->marks = set;
        st->marks_has = fn;
    }
    fn = JS_GetPropertyStr(ctx, st->marks, "add");
    if (JS_IsException(fn))
        return fn;
    ret = JS_Call(ctx, fn, st->marks, 1, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(ret))
        return ret;
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

/* the 'buffers' and 'externals' options of serialize()/deserialize();
   *pexternals is owned */
static int qb_clone_options(JSContext *ctx, JSValueConst opts, QBCloneCall *c,
                            JSValue *pexternals)
{
    JSValue v;

    *pexternals = JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "buffers");
    if (JS_IsException(v))
        return -1;
    c->buffers = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "externals");
    if (JS_IsException(v))
        return -1;
    if (JS_IsObject(v)) {
        *pexternals = v;
        c->externals = v;
    } else {
        JS_FreeValue(ctx, v);
    }
    return 0;
}

/* serialize(value, transfer?, { buffers?, externals? }?): a Buffer */
static JSValue qb_worker_serialize(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    QBCloneState *st = qb_clone_state(ctx);
    QBCloneCall c = { st, NULL, 0, 0, JS_UNDEFINED };
    JSValue externals;
    uint8_t *buf;
    size_t len;

    if (qb_clone_options(ctx, argv[2], &c, &externals))
        return JS_EXCEPTION;
    buf = JS_StructuredSerialize(ctx, &len, argv[0], argv[1], NULL, NULL,
                                 st ? &qb_clone_host : NULL, &c);
    JS_FreeValue(ctx, externals);
    if (!buf)
        return JS_EXCEPTION;
    return qb_new_buffer_owned(ctx, buf, len);
}

/* deserialize(bytes, { externals? }?) */
static JSValue qb_worker_deserialize(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    QBCloneState *st = qb_clone_state(ctx);
    QBCloneCall c = { st, NULL, 0, 0, JS_UNDEFINED };
    JSValue externals, ret;
    uint8_t *data, *copy;
    size_t len;

    data = qb_get_bytes(ctx, argv[0], &len);
    if (!data)
        return JS_EXCEPTION;
    /* the deserialization runs JS code (Blob constructors) that could
       modify the input */
    copy = js_malloc(ctx, len ? len : 1);
    if (!copy)
        return JS_EXCEPTION;
    memcpy(copy, data, len);
    if (qb_clone_options(ctx, argv[1], &c, &externals)) {
        js_free(ctx, copy);
        return JS_EXCEPTION;
    }
    ret = JS_StructuredDeserialize(ctx, copy, len, NULL, 0,
                                   st ? &qb_clone_host : NULL, &c);
    JS_FreeValue(ctx, externals);
    js_free(ctx, copy);
    return ret;
}

void qb_worker_runtime_init(QBRuntime *qr)
{
    JSSharedArrayBufferFunctions sf;
    sf.sab_alloc = qb_sab_alloc;
    sf.sab_free = qb_sab_free;
    sf.sab_dup = qb_sab_dup;
    sf.sab_opaque = NULL;
    JS_SetSharedArrayBufferFunctions(qr->rt, &sf);
    /* Atomics.wait() is allowed in every thread, like Node */
    JS_SetCanBlock(qr->rt, 1);
    qb_clone_init(qr);
}

/* ------------------------------------------------- channels + messages */

typedef struct QBPortEnd QBPortEnd;
typedef struct QBPort QBPort;

typedef struct QBMessage {
    struct QBMessage *next;
    uint8_t *data;
    size_t len;
    uint8_t **sabs;    /* one reference each */
    size_t nsabs;
    QBPortEnd **ports; /* transferred endpoints, owned */
    int nports;
    int kind;          /* opaque to C (bit 1: control message) */
} QBMessage;

typedef struct QBChannel {
    uv_mutex_t mutex;
    QBPortEnd *ends;
    int nends;
    int broadcast;
    char *name;             /* broadcast */
    struct QBChannel *next; /* broadcast registry */
} QBChannel;

struct QBPortEnd {
    QBChannel *ch;
    QBPortEnd *next;
    QBMessage *head, *tail; /* incoming, guarded by ch->mutex */
    int count;
    QBPort *port;           /* binding, guarded by ch->mutex; NULL in transit */
};

struct QBPort {
    uv_async_t async;
    QBRuntime *qr;
    QBPortEnd *end;  /* NULL once closed or transferred */
    JSValue handler; /* handler(type, value, ports, kind) */
    JSValue self;    /* strong while started or referenced */
    int started;
    int refed;
    int handle_closed;
    int close_done;
    int js_dead;
};

static uv_once_t qb_bc_once = UV_ONCE_INIT;
static uv_mutex_t qb_bc_mutex;
static QBChannel *qb_bc_list;

static _Atomic int64_t qb_next_thread_id = 1;

static void qb_bc_init(void)
{
    uv_mutex_init(&qb_bc_mutex);
}

static void qb_end_close(QBPortEnd *e);

static void qb_message_free(QBMessage *m)
{
    size_t i;
    int j;
    for (i = 0; i < m->nsabs; i++)
        qb_sab_free(NULL, m->sabs[i]);
    for (j = 0; j < m->nports; j++) {
        if (m->ports[j])
            qb_end_close(m->ports[j]);
    }
    free(m->sabs);
    free(m->ports);
    free(m->data);
    free(m);
}

/* a copy for another BroadcastChannel subscriber (no ports) */
static QBMessage *qb_message_clone(const QBMessage *m)
{
    QBMessage *n = calloc(1, sizeof(*n));
    size_t i;
    if (!n)
        return NULL;
    n->data = malloc(m->len);
    if (!n->data) {
        free(n);
        return NULL;
    }
    memcpy(n->data, m->data, m->len);
    n->len = m->len;
    if (m->nsabs) {
        n->sabs = malloc(sizeof(n->sabs[0]) * m->nsabs);
        if (!n->sabs) {
            free(n->data);
            free(n);
            return NULL;
        }
        for (i = 0; i < m->nsabs; i++) {
            n->sabs[i] = m->sabs[i];
            qb_sab_dup(NULL, n->sabs[i]);
        }
        n->nsabs = m->nsabs;
    }
    n->kind = m->kind;
    return n;
}

static QBChannel *qb_channel_new(void)
{
    QBChannel *ch = calloc(1, sizeof(*ch));
    if (!ch)
        return NULL;
    if (uv_mutex_init(&ch->mutex)) {
        free(ch);
        return NULL;
    }
    return ch;
}

/* add an endpoint (the caller holds ch->mutex or owns the channel alone) */
static QBPortEnd *qb_end_new(QBChannel *ch)
{
    QBPortEnd *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->ch = ch;
    e->next = ch->ends;
    ch->ends = e;
    ch->nends++;
    return e;
}

/* the other endpoint of a MessageChannel (ch->mutex held) */
static QBPortEnd *qb_end_peer(QBPortEnd *e)
{
    QBPortEnd *o;
    for (o = e->ch->ends; o; o = o->next) {
        if (o != e)
            return o;
    }
    return NULL;
}

/* remove an endpoint from its channel, drop its queue, free it (and the
   channel with the last endpoint). The peer of a MessageChannel is woken
   to see the close. */
static void qb_end_close(QBPortEnd *e)
{
    QBChannel *ch = e->ch;
    QBPortEnd **pe, *peer;
    QBChannel **pc;
    QBMessage *q, *m;
    int broadcast = ch->broadcast, empty;

    if (broadcast)
        uv_mutex_lock(&qb_bc_mutex);
    uv_mutex_lock(&ch->mutex);
    for (pe = &ch->ends; *pe; pe = &(*pe)->next) {
        if (*pe == e) {
            *pe = e->next;
            break;
        }
    }
    ch->nends--;
    q = e->head;
    e->head = e->tail = NULL;
    e->count = 0;
    if (!broadcast) {
        peer = ch->ends;
        if (peer && peer->port)
            uv_async_send(&peer->port->async);
    }
    empty = ch->nends == 0;
    if (empty && broadcast) {
        for (pc = &qb_bc_list; *pc; pc = &(*pc)->next) {
            if (*pc == ch) {
                *pc = ch->next;
                break;
            }
        }
    }
    uv_mutex_unlock(&ch->mutex);
    if (broadcast)
        uv_mutex_unlock(&qb_bc_mutex);
    while (q) {
        m = q;
        q = m->next;
        qb_message_free(m);
    }
    free(e);
    if (empty) {
        uv_mutex_destroy(&ch->mutex);
        free(ch->name);
        free(ch);
    }
}

/* ch->mutex held */
static void qb_end_push_locked(QBPortEnd *e, QBMessage *m)
{
    m->next = NULL;
    if (e->tail)
        e->tail->next = m;
    else
        e->head = m;
    e->tail = m;
    e->count++;
    if (e->port)
        uv_async_send(&e->port->async);
}

static QBMessage *qb_end_pop(QBPortEnd *e)
{
    QBMessage *m;
    uv_mutex_lock(&e->ch->mutex);
    m = e->head;
    if (m) {
        e->head = m->next;
        if (!e->head)
            e->tail = NULL;
        e->count--;
        m->next = NULL;
    }
    uv_mutex_unlock(&e->ch->mutex);
    return m;
}

/* the queue is empty and the peer of a MessageChannel is gone */
static int qb_end_orphaned(QBPortEnd *e)
{
    int r;
    if (e->ch->broadcast)
        return 0;
    uv_mutex_lock(&e->ch->mutex);
    r = e->ch->nends < 2 && !e->head;
    uv_mutex_unlock(&e->ch->mutex);
    return r;
}

static int qb_end_has_work(QBPortEnd *e)
{
    int r;
    uv_mutex_lock(&e->ch->mutex);
    r = e->head != NULL || (!e->ch->broadcast && e->ch->nends < 2);
    uv_mutex_unlock(&e->ch->mutex);
    return r;
}

/* ------------------------------------------------------- port handles */

static JSClassID qb_port_class_id;

static void qb_port_cleanup(void *arg);

static void qb_port_close_cb(uv_handle_t *h)
{
    QBPort *p = h->data;
    p->close_done = 1;
    if (p->js_dead)
        free(p);
}

/* detach the endpoint from the handle and close the uv handle; return
   the endpoint (the caller closes or transfers it) */
static QBPortEnd *qb_port_unbind(QBPort *p)
{
    QBPortEnd *e = p->end;
    if (!e)
        return NULL;
    uv_mutex_lock(&e->ch->mutex);
    e->port = NULL;
    uv_mutex_unlock(&e->ch->mutex);
    p->end = NULL;
    p->started = 0;
    qb_remove_cleanup_hook(p->qr, qb_port_cleanup, p);
    if (!p->handle_closed) {
        p->handle_closed = 1;
        uv_close((uv_handle_t *)&p->async, qb_port_close_cb);
    }
    return e;
}

/* drop the strong self reference when the handle needs none; must be the
   last access to 'p' of the caller (the object may be finalized) */
static void qb_port_release_self(QBPort *p)
{
    JSValue s;
    if ((p->started || p->refed) && p->end)
        return;
    s = p->self;
    p->self = JS_UNDEFINED;
    JS_FreeValue(p->qr->ctx, s);
}

static void qb_port_retain_self(JSContext *ctx, QBPort *p, JSValueConst obj)
{
    if (JS_IsUndefined(p->self) && p->end)
        p->self = JS_DupValue(ctx, obj);
}

static void qb_port_cleanup(void *arg)
{
    QBPort *p = arg;
    JSContext *ctx = p->qr->ctx;
    QBPortEnd *e;
    JSValue h, s;

    e = qb_port_unbind(p);
    h = p->handler;
    s = p->self;
    p->handler = JS_UNDEFINED;
    p->self = JS_UNDEFINED;
    if (e)
        qb_end_close(e);
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, s);
}

static void qb_port_finalizer(JSRuntime *rt, JSValue val)
{
    QBPort *p = JS_GetOpaque(val, qb_port_class_id);
    QBPortEnd *e;
    if (!p)
        return;
    JS_FreeValueRT(rt, p->handler);
    p->handler = JS_UNDEFINED;
    p->self = JS_UNDEFINED;
    p->js_dead = 1;
    e = qb_port_unbind(p);
    if (e)
        qb_end_close(e);
    if (p->close_done)
        free(p);
}

static void qb_port_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBPort *p = JS_GetOpaque(val, qb_port_class_id);
    if (p)
        JS_MarkValue(rt, p->handler, mark_func);
}

static JSClassDef qb_port_class = {
    "MessagePortHandle",
    .finalizer = qb_port_finalizer,
    .gc_mark = qb_port_mark,
};

static void qb_port_async_cb(uv_async_t *h);

/* a new handle of the current runtime owning 'e' (closed on failure) */
static JSValue qb_port_new(JSContext *ctx, QBPortEnd *e)
{
    QBRuntime *qr = qb_get(ctx);
    QBPort *p;
    JSValue obj;
    int work;

    obj = JS_NewObjectClass(ctx, qb_port_class_id);
    if (JS_IsException(obj)) {
        qb_end_close(e);
        return obj;
    }
    p = calloc(1, sizeof(*p));
    if (!p) {
        JS_FreeValue(ctx, obj);
        qb_end_close(e);
        return JS_ThrowOutOfMemory(ctx);
    }
    p->qr = qr;
    p->handler = JS_UNDEFINED;
    p->self = JS_UNDEFINED;
    uv_async_init(qr->loop, &p->async, qb_port_async_cb);
    p->async.data = p;
    uv_unref((uv_handle_t *)&p->async);
    uv_mutex_lock(&e->ch->mutex);
    e->port = p;
    work = !e->ch->broadcast && e->ch->nends < 2;
    uv_mutex_unlock(&e->ch->mutex);
    p->end = e;
    qb_add_cleanup_hook(qr, qb_port_cleanup, p);
    JS_SetOpaque(obj, p);
    /* the peer may be gone already: report the close */
    if (work)
        uv_async_send(&p->async);
    return obj;
}

/* the value of a message and its transferred ports (array of MessagePorts,
   or of handles before setCloneHooks(); undefined if none); frees the
   message */
static JSValue qb_message_read(JSContext *ctx, QBMessage *m, JSValue *pports)
{
    QBCloneState *st = qb_clone_state(ctx);
    QBCloneCall c = { st, NULL, 0, 0, JS_UNDEFINED };
    JSValue val, arr, port, wrapped;
    int i;

    *pports = JS_UNDEFINED;
    if (m->nports > 0) {
        /* the ports exist before the value, which may refer to them */
        c.ports = malloc(sizeof(c.ports[0]) * m->nports);
        if (!c.ports) {
            qb_message_free(m);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < m->nports; i++) {
            QBPortEnd *e = m->ports[i];
            m->ports[i] = NULL;
            port = qb_port_new(ctx, e);
            if (JS_IsException(port)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                port = JS_NULL;
            } else if (st && JS_IsFunction(ctx, st->create_port)) {
                wrapped = JS_Call(ctx, st->create_port, JS_UNDEFINED, 1, &port);
                JS_FreeValue(ctx, port);
                if (JS_IsException(wrapped)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    wrapped = JS_NULL;
                }
                port = wrapped;
            }
            c.ports[i] = port;
        }
        c.nports = m->nports;
    }
    val = JS_StructuredDeserialize(ctx, m->data, m->len, m->sabs, m->nsabs,
                                   st ? &qb_clone_host : NULL, &c);
    if (!JS_IsException(val) && c.nports > 0) {
        arr = JS_NewArray(ctx);
        for (i = 0; i < c.nports; i++) {
            JS_SetPropertyUint32(ctx, arr, i, c.ports[i]);
            c.ports[i] = JS_UNDEFINED;
        }
        *pports = arr;
    }
    for (i = 0; i < c.nports; i++)
        JS_FreeValue(ctx, c.ports[i]);
    free(c.ports);
    qb_message_free(m);
    return val;
}

static void qb_port_dispatch(JSContext *ctx, QBPort *p, QBMessage *m)
{
    JSValue args[4], ports, val, fn, ret;
    int kind = m->kind;

    val = qb_message_read(ctx, m, &ports);
    if (JS_IsException(val)) {
        args[0] = JS_NewInt32(ctx, 1);
        args[1] = JS_GetException(ctx);
    } else {
        args[0] = JS_NewInt32(ctx, 0);
        args[1] = val;
    }
    args[2] = ports;
    args[3] = JS_NewInt32(ctx, kind);
    fn = JS_DupValue(ctx, p->handler);
    if (JS_IsFunction(ctx, fn)) {
        ret = qb_call(ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
}

/* the peer of the channel is gone: the handle closes and says so */
static void qb_port_emit_close(JSContext *ctx, QBPort *p)
{
    QBPortEnd *e;
    JSValue fn, s, arg, ret;

    e = qb_port_unbind(p);
    fn = JS_DupValue(ctx, p->handler);
    s = p->self;
    p->self = JS_UNDEFINED;
    if (e)
        qb_end_close(e);
    if (JS_IsFunction(ctx, fn)) {
        arg = JS_NewInt32(ctx, 2);
        ret = qb_call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, s);
}

static void qb_port_async_cb(uv_async_t *h)
{
    QBPort *p = h->data;
    JSContext *ctx = p->qr->ctx;
    QBMessage *m;
    int budget;

    if (!p->end)
        return;
    if (p->started) {
        /* the messages queued now: later ones wait for the next wakeup,
           so a busy sender cannot starve the loop */
        uv_mutex_lock(&p->end->ch->mutex);
        budget = p->end->count;
        uv_mutex_unlock(&p->end->ch->mutex);
        while (budget-- > 0 && p->end && p->started && !p->qr->exiting) {
            m = qb_end_pop(p->end);
            if (!m)
                break;
            qb_port_dispatch(ctx, p, m);
        }
        if (p->end && p->started && !p->qr->exiting) {
            uv_mutex_lock(&p->end->ch->mutex);
            budget = p->end->head != NULL;
            uv_mutex_unlock(&p->end->ch->mutex);
            if (budget)
                uv_async_send(&p->async);
        }
    }
    if (p->end && !p->qr->exiting && qb_end_orphaned(p->end))
        qb_port_emit_close(ctx, p);
}

static QBPort *qb_port_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_port_class_id);
}

static int qb_get_array(JSContext *ctx, JSValueConst v, int64_t *plen)
{
    *plen = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_IsArray(ctx, v) <= 0) {
        JS_ThrowTypeError(ctx, "transfer list must be an array");
        return -1;
    }
    return JS_GetLength(ctx, v, plen);
}

/* postMessage(value, transfer?, ports?, handles?, kind?): 'transfer' is
   the transfer list (ArrayBuffers and MessagePorts), 'ports' its
   MessagePorts and 'handles' their handles, in the same order */
static JSValue qb_port_postMessage(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    QBPort *p = qb_port_get(ctx, this_val);
    QBPort **plist = NULL;
    QBMessage *m = NULL;
    QBPortEnd *e, *o, *peer;
    QBChannel *ch;
    QBCloneCall c = { qb_clone_state(ctx), NULL, 0, 0, JS_UNDEFINED };
    uint8_t *data = NULL, **sab_tab = NULL;
    size_t len, sab_len = 0, i;
    int64_t nports = 0, j, k;
    int kind = 0, delivered = 0;
    JSValue v;

    if (!p)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &kind, argv[4]))
        return JS_EXCEPTION;
    if (!p->end)
        return JS_NewInt32(ctx, 0); /* closed: the message is dropped, like Node */
    if (qb_get_array(ctx, argv[3], &nports) < 0)
        return JS_EXCEPTION;
    if (nports > 0) {
        if (p->end->ch->broadcast)
            return JS_ThrowDOMException(ctx, "DataCloneError",
                                        "MessagePorts cannot be sent over a BroadcastChannel");
        if (nports > INT32_MAX / (int64_t)sizeof(JSValue))
            return JS_ThrowRangeError(ctx, "too many MessagePorts");
        plist = calloc(nports, sizeof(*plist));
        c.ports = malloc(sizeof(c.ports[0]) * nports);
        if (!plist || !c.ports) {
            free(plist);
            free(c.ports);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (j = 0; j < nports; j++) {
            c.ports[j] = JS_GetPropertyUint32(ctx, argv[2], j);
            c.nports++;
            if (!JS_IsObject(c.ports[j])) {
                JS_ThrowTypeError(ctx, "invalid MessagePort in the transfer list");
                goto fail;
            }
            v = JS_GetPropertyUint32(ctx, argv[3], j);
            /* the array keeps the object alive */
            plist[j] = JS_GetOpaque(v, qb_port_class_id);
            JS_FreeValue(ctx, v);
            if (!plist[j]) {
                JS_ThrowTypeError(ctx, "invalid MessagePort in the transfer list");
                goto fail;
            }
            if (plist[j] == p) {
                JS_ThrowDOMException(ctx, "DataCloneError", "Transfer list contains source port");
                goto fail;
            }
            if (!plist[j]->end) {
                JS_ThrowDOMException(ctx, "DataCloneError",
                                     "MessagePort in transfer list is already detached");
                goto fail;
            }
            if (plist[j]->end->ch == p->end->ch) {
                JS_ThrowDOMException(ctx, "DataCloneError",
                                     "Transfer list contains the target port");
                goto fail;
            }
            for (k = 0; k < j; k++) {
                if (plist[k] == plist[j]) {
                    JS_ThrowDOMException(ctx, "DataCloneError",
                                         "Transfer list contains duplicate MessagePort");
                    goto fail;
                }
            }
        }
    }

    /* the transferred ArrayBuffers are detached once serialized */
    data = JS_StructuredSerialize(ctx, &len, argv[0], argv[1], &sab_tab, &sab_len,
                                  c.st ? &qb_clone_host : NULL, &c);
    if (!data)
        goto fail;
    m = calloc(1, sizeof(*m));
    if (!m)
        goto oom;
    m->kind = kind;
    m->data = malloc(len ? len : 1);
    if (!m->data)
        goto oom;
    memcpy(m->data, data, len);
    m->len = len;
    if (sab_len) {
        m->sabs = malloc(sizeof(m->sabs[0]) * sab_len);
        if (!m->sabs)
            goto oom;
        for (i = 0; i < sab_len; i++) {
            m->sabs[i] = sab_tab[i];
            qb_sab_dup(NULL, m->sabs[i]);
        }
        m->nsabs = sab_len;
    }
    js_free(ctx, data);
    data = NULL;
    js_free(ctx, sab_tab);
    sab_tab = NULL;

    /* transferred ports: their endpoints move into the message */
    if (nports > 0) {
        m->ports = calloc(nports, sizeof(m->ports[0]));
        if (!m->ports)
            goto oom;
        for (j = 0; j < nports; j++) {
            m->ports[j] = qb_port_unbind(plist[j]);
            m->nports++;
            qb_port_release_self(plist[j]);
        }
    }
    free(plist);
    plist = NULL;
    for (j = 0; j < c.nports; j++)
        JS_FreeValue(ctx, c.ports[j]);
    free(c.ports);
    c.ports = NULL;
    c.nports = 0;

    e = p->end;
    if (!e) {
        qb_message_free(m);
        return JS_NewInt32(ctx, 0);
    }
    ch = e->ch;
    uv_mutex_lock(&ch->mutex);
    if (ch->broadcast) {
        for (o = ch->ends; o; o = o->next) {
            QBMessage *copy;
            if (o == e)
                continue;
            copy = qb_message_clone(m);
            if (copy) {
                qb_end_push_locked(o, copy);
                delivered++;
            }
        }
    } else {
        peer = qb_end_peer(e);
        if (peer) {
            qb_end_push_locked(peer, m);
            m = NULL;
            delivered = 1;
        }
    }
    uv_mutex_unlock(&ch->mutex);
    if (m)
        qb_message_free(m);
    return JS_NewInt32(ctx, delivered);

oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    if (data)
        js_free(ctx, data);
    if (sab_tab)
        js_free(ctx, sab_tab);
    if (m)
        qb_message_free(m);
    free(plist);
    for (j = 0; j < c.nports; j++)
        JS_FreeValue(ctx, c.ports[j]);
    free(c.ports);
    return JS_EXCEPTION;
}

/* receive(): the next queued message as [value, ports, kind], undefined
   when there is none */
static JSValue qb_port_receive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBPort *p = qb_port_get(ctx, this_val);
    QBMessage *m;
    JSValue val, ports, arr;
    int kind;

    if (!p)
        return JS_EXCEPTION;
    if (!p->end)
        return JS_UNDEFINED;
    m = qb_end_pop(p->end);
    if (!m)
        return JS_UNDEFINED;
    kind = m->kind;
    if (qb_end_orphaned(p->end))
        uv_async_send(&p->async);
    val = qb_message_read(ctx, m, &ports);
    if (JS_IsException(val))
        return val;
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, val);
    JS_SetPropertyUint32(ctx, arr, 1, ports);
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewInt32(ctx, kind));
    return arr;
}

static JSValue qb_port_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                              int magic)
{
    QBPort *p = qb_port_get(ctx, this_val);
    QBPortEnd *e;
    JSValue old;

    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* start */
        if (!p->end)
            break;
        p->started = 1;
        qb_port_retain_self(ctx, p, this_val);
        if (qb_end_has_work(p->end))
            uv_async_send(&p->async);
        break;
    case 1: /* stop */
        p->started = 0;
        qb_port_release_self(p);
        break;
    case 2: /* ref */
        p->refed = 1;
        if (!p->handle_closed)
            uv_ref((uv_handle_t *)&p->async);
        qb_port_retain_self(ctx, p, this_val);
        break;
    case 3: /* unref */
        p->refed = 0;
        if (!p->handle_closed)
            uv_unref((uv_handle_t *)&p->async);
        qb_port_release_self(p);
        break;
    case 4: /* hasRef */
        return JS_NewBool(ctx, !p->handle_closed && uv_has_ref((uv_handle_t *)&p->async));
    case 5: /* close */
        e = qb_port_unbind(p);
        if (e)
            qb_end_close(e);
        qb_port_release_self(p);
        break;
    case 6: /* setHandler */
        old = p->handler;
        p->handler = JS_IsFunction(ctx, argv[0]) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
        JS_FreeValue(ctx, old);
        break;
    case 7: /* isClosed */
        return JS_NewBool(ctx, p->end == NULL);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_port_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 5, qb_port_postMessage),
    JS_CFUNC_DEF("receive", 0, qb_port_receive),
    JS_CFUNC_MAGIC_DEF("start", 0, qb_port_method, 0),
    JS_CFUNC_MAGIC_DEF("stop", 0, qb_port_method, 1),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_port_method, 2),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_port_method, 3),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_port_method, 4),
    JS_CFUNC_MAGIC_DEF("close", 0, qb_port_method, 5),
    JS_CFUNC_MAGIC_DEF("setHandler", 1, qb_port_method, 6),
    JS_CFUNC_MAGIC_DEF("isClosed", 0, qb_port_method, 7),
};

/* createChannel(): [port1, port2] */
static JSValue qb_worker_createChannel(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBChannel *ch = qb_channel_new();
    QBPortEnd *e1, *e2;
    JSValue arr, p1, p2;

    if (!ch)
        return JS_ThrowOutOfMemory(ctx);
    e1 = qb_end_new(ch);
    e2 = e1 ? qb_end_new(ch) : NULL;
    if (!e2) {
        if (e1)
            qb_end_close(e1);
        else {
            uv_mutex_destroy(&ch->mutex);
            free(ch);
        }
        return JS_ThrowOutOfMemory(ctx);
    }
    p1 = qb_port_new(ctx, e1);
    if (JS_IsException(p1)) {
        qb_end_close(e2);
        return p1;
    }
    p2 = qb_port_new(ctx, e2);
    if (JS_IsException(p2)) {
        JS_FreeValue(ctx, p1);
        return p2;
    }
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, p1);
    JS_SetPropertyUint32(ctx, arr, 1, p2);
    return arr;
}

/* openBroadcast(name): a port subscribed to the BroadcastChannel 'name'
   of the process */
static JSValue qb_worker_openBroadcast(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    const char *name;
    QBChannel *ch;
    QBPortEnd *e = NULL;

    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    uv_once(&qb_bc_once, qb_bc_init);
    uv_mutex_lock(&qb_bc_mutex);
    for (ch = qb_bc_list; ch; ch = ch->next) {
        if (!strcmp(ch->name, name))
            break;
    }
    if (!ch) {
        ch = qb_channel_new();
        if (ch) {
            ch->broadcast = 1;
            ch->name = strdup(name);
            if (!ch->name) {
                uv_mutex_destroy(&ch->mutex);
                free(ch);
                ch = NULL;
            } else {
                ch->next = qb_bc_list;
                qb_bc_list = ch;
            }
        }
    }
    if (ch) {
        uv_mutex_lock(&ch->mutex);
        e = qb_end_new(ch);
        uv_mutex_unlock(&ch->mutex);
        if (!e && ch->nends == 0) {
            QBChannel **pc;
            for (pc = &qb_bc_list; *pc; pc = &(*pc)->next) {
                if (*pc == ch) {
                    *pc = ch->next;
                    break;
                }
            }
            uv_mutex_destroy(&ch->mutex);
            free(ch->name);
            free(ch);
        }
    }
    uv_mutex_unlock(&qb_bc_mutex);
    JS_FreeCString(ctx, name);
    if (!e)
        return JS_ThrowOutOfMemory(ctx);
    return qb_port_new(ctx, e);
}

/* ------------------------------------------------------------ workers */

typedef struct QBWorkerHandle QBWorkerHandle;

/* shared by the parent handle and the worker thread */
typedef struct QBWorker {
    _Atomic int refs;
    uv_mutex_t mutex;
    uv_thread_t tid;
    int thread_started;
    char *filename;
    char *eval_src;
    int eval_ts;
    int argc;
    char **argv;
    int exec_argc;
    char **exec_argv;
    char *lib_dir;
    char *exec_path;
    int64_t thread_id;
    QBPortEnd *child_end; /* claimed by the worker runtime */
    /* guarded by mutex */
    QBRuntime *child;       /* while the worker runtime exists */
    uv_async_t *stop_async; /* of the worker loop */
    int terminate;
    int exited;
    int exit_code;
    QBWorkerHandle *parent; /* NULL once the parent let go */
} QBWorker;

struct QBWorkerHandle {
    uv_async_t exit_async;
    QBRuntime *qr;
    QBWorker *w;
    JSValue onexit;
    JSValue self; /* strong while the thread runs */
    int refed;
    int running;
    int handle_closed;
    int close_done;
    int js_dead;
};

/* qr->worker of a worker runtime */
typedef struct QBWorkerThread {
    QBWorker *w;
    uv_async_t stop_async;
} QBWorkerThread;

static JSClassID qb_worker_class_id;

static void qb_free_strv(int n, char **v)
{
    int i;
    if (!v)
        return;
    for (i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static void qb_worker_release(QBWorker *w)
{
    if (atomic_fetch_sub(&w->refs, 1) != 1)
        return;
    if (w->child_end)
        qb_end_close(w->child_end);
    free(w->filename);
    free(w->eval_src);
    qb_free_strv(w->argc, w->argv);
    qb_free_strv(w->exec_argc, w->exec_argv);
    free(w->lib_dir);
    free(w->exec_path);
    uv_mutex_destroy(&w->mutex);
    free(w);
}

/* ask the worker runtime to stop (w->mutex held) */
static void qb_worker_stop_locked(QBWorker *w)
{
    w->terminate = 1;
    if (w->child) {
        __atomic_store_n(&w->child->exiting, 1, __ATOMIC_SEQ_CST);
        if (w->stop_async)
            uv_async_send(w->stop_async);
    }
}

static void qb_worker_stop_cb(uv_async_t *h)
{
    QBRuntime *qr = h->data;
    qr->exiting = 1;
    uv_stop(qr->loop);
}

static int qb_worker_setup(QBRuntime *qr, QBWorker *w)
{
    JSContext *ctx = qr->ctx;
    JSValue port, ns, fn, args[2], ret;
    int ok;

    /* the module imports qbun:worker, whose init registers the port class
       in this runtime: load it before creating the port */
    ns = JS_LoadModuleSync(ctx, "", "internal:worker/thread", JS_UNDEFINED);
    if (JS_IsException(ns)) {
        qb_report_exception(ctx);
        return -1;
    }
    port = qb_port_new(ctx, w->child_end);
    w->child_end = NULL;
    if (JS_IsException(port)) {
        JS_FreeValue(ctx, ns);
        qb_report_exception(ctx);
        return -1;
    }
    fn = JS_GetPropertyStr(ctx, ns, "setup");
    JS_FreeValue(ctx, ns);
    args[0] = port;
    args[1] = JS_DupValue(ctx, qr->hooks);
    ret = qb_call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
    ok = JS_ToBool(ctx, ret);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return ok > 0 ? 0 : -1;
}

static void qb_worker_main(void *arg)
{
    QBWorker *w = arg;
    QBOptions o;
    QBRuntime *qr;
    QBWorkerThread *wt = NULL;
    int code = 1, failed = 0;

    memset(&o, 0, sizeof(o));
    o.lib_dir = w->lib_dir;
    o.exec_path = w->exec_path;
    o.argc = w->argc;
    o.argv = w->argv;
    o.exec_argc = w->exec_argc;
    o.exec_argv = w->exec_argv;
    o.loop = NULL;
    o.is_main_thread = 0;
    qr = qb_runtime_new(&o);
    if (!qr)
        goto done;
    wt = calloc(1, sizeof(*wt));
    if (!wt) {
        qr->exit_code = 1;
        qr->exiting = 1;
    } else {
        wt->w = w;
        qr->worker = wt;
        uv_async_init(qr->loop, &wt->stop_async, qb_worker_stop_cb);
        wt->stop_async.data = qr;
        uv_unref((uv_handle_t *)&wt->stop_async);
        uv_mutex_lock(&w->mutex);
        w->child = qr;
        w->stop_async = &wt->stop_async;
        if (w->terminate)
            qr->exiting = 1;
        uv_mutex_unlock(&w->mutex);
    }
    if (!qr->exiting) {
        if (qb_bootstrap(qr) < 0 || qr->exiting || qb_worker_setup(qr, w) < 0) {
            failed = 1;
        } else if (!qr->exiting) {
            if (w->eval_src)
                qb_run_eval(qr, w->eval_src, w->eval_ts);
            else
                qb_run_main(qr, w->filename);
            qb_run_loop(qr);
        }
    }
    code = qb_runtime_exit(qr);
    if (failed && code == 0)
        code = 1;
    if (wt) {
        uv_mutex_lock(&w->mutex);
        w->child = NULL;
        w->stop_async = NULL;
        if (w->terminate)
            code = 1;
        uv_mutex_unlock(&w->mutex);
        uv_close((uv_handle_t *)&wt->stop_async, NULL);
    }
    qb_runtime_free(qr);
    free(wt);
done:
    uv_mutex_lock(&w->mutex);
    w->exited = 1;
    w->exit_code = code;
    if (w->parent)
        uv_async_send(&w->parent->exit_async);
    uv_mutex_unlock(&w->mutex);
    qb_worker_release(w);
}

static void qb_worker_handle_cleanup(void *arg);

static void qb_worker_handle_close_cb(uv_handle_t *h)
{
    QBWorkerHandle *wh = h->data;
    wh->close_done = 1;
    if (wh->js_dead)
        free(wh);
}

/* the worker is gone (or the parent runtime exits): release everything;
   onexit(code) when 'call' */
static void qb_worker_handle_finish(QBWorkerHandle *wh, int code, int call)
{
    JSContext *ctx = wh->qr->ctx;
    JSValue fn = wh->onexit, s = wh->self, arg, ret;
    QBWorker *w = wh->w;

    wh->onexit = JS_UNDEFINED;
    wh->self = JS_UNDEFINED;
    wh->running = 0;
    wh->w = NULL;
    qb_remove_cleanup_hook(wh->qr, qb_worker_handle_cleanup, wh);
    if (!wh->handle_closed) {
        wh->handle_closed = 1;
        uv_close((uv_handle_t *)&wh->exit_async, qb_worker_handle_close_cb);
    }
    if (w)
        qb_worker_release(w);
    if (call && JS_IsFunction(ctx, fn)) {
        arg = JS_NewInt32(ctx, code);
        ret = qb_call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, s);
}

static void qb_worker_exit_cb(uv_async_t *h)
{
    QBWorkerHandle *wh = h->data;
    QBWorker *w = wh->w;
    int exited, code;

    if (!w)
        return;
    uv_mutex_lock(&w->mutex);
    exited = w->exited;
    code = w->exit_code;
    if (exited)
        w->parent = NULL;
    uv_mutex_unlock(&w->mutex);
    if (!exited)
        return;
    uv_thread_join(&w->tid);
    qb_worker_handle_finish(wh, code, 1);
}

/* the parent runtime exits first: the worker is terminated and the
   thread detached */
static void qb_worker_handle_cleanup(void *arg)
{
    QBWorkerHandle *wh = arg;
    QBWorker *w = wh->w;
    if (w) {
        uv_mutex_lock(&w->mutex);
        w->parent = NULL;
        qb_worker_stop_locked(w);
        uv_mutex_unlock(&w->mutex);
        if (w->thread_started)
            uv_thread_detach(&w->tid);
    }
    qb_worker_handle_finish(wh, 1, 0);
}

static void qb_worker_handle_finalizer(JSRuntime *rt, JSValue val)
{
    QBWorkerHandle *wh = JS_GetOpaque(val, qb_worker_class_id);
    QBWorker *w;
    if (!wh)
        return;
    JS_FreeValueRT(rt, wh->onexit);
    wh->onexit = JS_UNDEFINED;
    wh->self = JS_UNDEFINED;
    wh->js_dead = 1;
    w = wh->w;
    if (w) {
        /* never started (a running worker holds its handle) */
        wh->w = NULL;
        uv_mutex_lock(&w->mutex);
        w->parent = NULL;
        qb_worker_stop_locked(w);
        uv_mutex_unlock(&w->mutex);
        if (w->thread_started)
            uv_thread_detach(&w->tid);
        qb_remove_cleanup_hook(wh->qr, qb_worker_handle_cleanup, wh);
        qb_worker_release(w);
    }
    if (!wh->handle_closed) {
        wh->handle_closed = 1;
        uv_close((uv_handle_t *)&wh->exit_async, qb_worker_handle_close_cb);
    } else if (wh->close_done) {
        free(wh);
    }
}

static void qb_worker_handle_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBWorkerHandle *wh = JS_GetOpaque(val, qb_worker_class_id);
    if (wh)
        JS_MarkValue(rt, wh->onexit, mark_func);
}

static JSClassDef qb_worker_class = {
    "WorkerHandle",
    .finalizer = qb_worker_handle_finalizer,
    .gc_mark = qb_worker_handle_mark,
};

static char *qb_dup_prop_str(JSContext *ctx, JSValueConst obj, const char *name, int *perr)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s;
    char *r = NULL;
    if (JS_IsException(v)) {
        *perr = 1;
        return NULL;
    }
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return NULL;
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        *perr = 1;
        return NULL;
    }
    r = strdup(s);
    JS_FreeCString(ctx, s);
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        *perr = 1;
    }
    return r;
}

/* the strings of the array property 'name' (NULL terminated) */
static char **qb_dup_prop_strv(JSContext *ctx, JSValueConst obj, const char *name, int *pn,
                               int *perr)
{
    JSValue arr = JS_GetPropertyStr(ctx, obj, name), v;
    int64_t len = 0, i;
    char **res;
    const char *s;

    *pn = 0;
    if (JS_IsException(arr)) {
        *perr = 1;
        return NULL;
    }
    if (!JS_IsArray(ctx, arr) || JS_GetLength(ctx, arr, &len) < 0)
        len = 0;
    res = calloc(len + 1, sizeof(char *));
    if (!res) {
        JS_FreeValue(ctx, arr);
        JS_ThrowOutOfMemory(ctx);
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        v = JS_GetPropertyUint32(ctx, arr, i);
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s || !(res[i] = strdup(s))) {
            if (s)
                JS_ThrowOutOfMemory(ctx);
            JS_FreeCString(ctx, s);
            qb_free_strv(i, res);
            JS_FreeValue(ctx, arr);
            *perr = 1;
            return NULL;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, arr);
    *pn = (int)len;
    return res;
}

/* createWorker({ filename?, evalSource?, evalTs?, argv, execArgv }, onexit):
   [handle, port, threadId]; the thread starts with handle.start() */
static JSValue qb_worker_createWorker(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    QBWorker *w;
    QBWorkerHandle *wh;
    QBChannel *ch;
    QBPortEnd *pe;
    JSValue obj, port, arr, v;
    int err = 0;

    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "options must be an object");
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "onexit must be a function");
    w = calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    if (uv_mutex_init(&w->mutex)) {
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    atomic_init(&w->refs, 1);
    w->filename = qb_dup_prop_str(ctx, argv[0], "filename", &err);
    if (!err)
        w->eval_src = qb_dup_prop_str(ctx, argv[0], "evalSource", &err);
    if (!err) {
        v = JS_GetPropertyStr(ctx, argv[0], "evalTs");
        w->eval_ts = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
        w->argv = qb_dup_prop_strv(ctx, argv[0], "argv", &w->argc, &err);
    }
    if (!err)
        w->exec_argv = qb_dup_prop_strv(ctx, argv[0], "execArgv", &w->exec_argc, &err);
    if (!err && !w->filename && !w->eval_src) {
        JS_ThrowTypeError(ctx, "a worker needs a filename or evalSource");
        err = 1;
    }
    if (!err) {
        w->lib_dir = strdup(qr->lib_dir);
        w->exec_path = strdup(qr->exec_path ? qr->exec_path : "");
        if (!w->lib_dir || !w->exec_path) {
            JS_ThrowOutOfMemory(ctx);
            err = 1;
        }
    }
    ch = err ? NULL : qb_channel_new();
    if (!err && !ch) {
        JS_ThrowOutOfMemory(ctx);
        err = 1;
    }
    if (err) {
        qb_worker_release(w);
        return JS_EXCEPTION;
    }
    pe = qb_end_new(ch);
    w->child_end = pe ? qb_end_new(ch) : NULL;
    if (!w->child_end) {
        if (pe)
            qb_end_close(pe);
        else {
            uv_mutex_destroy(&ch->mutex);
            free(ch);
        }
        qb_worker_release(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->thread_id = atomic_fetch_add(&qb_next_thread_id, 1);

    obj = JS_NewObjectClass(ctx, qb_worker_class_id);
    if (JS_IsException(obj)) {
        qb_end_close(pe);
        qb_worker_release(w);
        return obj;
    }
    wh = calloc(1, sizeof(*wh));
    if (!wh) {
        JS_FreeValue(ctx, obj);
        qb_end_close(pe);
        qb_worker_release(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    wh->qr = qr;
    wh->w = w;
    wh->onexit = JS_DupValue(ctx, argv[1]);
    wh->self = JS_UNDEFINED;
    wh->refed = 1;
    uv_async_init(qr->loop, &wh->exit_async, qb_worker_exit_cb);
    wh->exit_async.data = wh;
    uv_unref((uv_handle_t *)&wh->exit_async);
    w->parent = wh;
    qb_add_cleanup_hook(qr, qb_worker_handle_cleanup, wh);
    JS_SetOpaque(obj, wh);

    port = qb_port_new(ctx, pe);
    if (JS_IsException(port)) {
        JS_FreeValue(ctx, obj);
        return port;
    }
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, obj);
    JS_SetPropertyUint32(ctx, arr, 1, port);
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewInt64(ctx, w->thread_id));
    return arr;
}

static JSValue qb_worker_handle_method(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv, int magic)
{
    QBWorkerHandle *wh = JS_GetOpaque2(ctx, this_val, qb_worker_class_id);
    QBWorker *w;
    uv_thread_options_t opts;
    int r;

    if (!wh)
        return JS_EXCEPTION;
    w = wh->w;
    switch (magic) {
    case 0: /* start */
        if (!w || w->thread_started)
            break;
        opts.flags = UV_THREAD_HAS_STACK_SIZE;
        opts.stack_size = 8 * 1024 * 1024;
        atomic_fetch_add(&w->refs, 1);
        r = uv_thread_create_ex(&w->tid, &opts, qb_worker_main, w);
        if (r) {
            atomic_fetch_sub(&w->refs, 1);
            return qb_throw_uv(ctx, r, "uv_thread_create", NULL, NULL);
        }
        w->thread_started = 1;
        wh->running = 1;
        if (JS_IsUndefined(wh->self))
            wh->self = JS_DupValue(ctx, this_val);
        if (wh->refed && !wh->handle_closed)
            uv_ref((uv_handle_t *)&wh->exit_async);
        break;
    case 1: /* terminate */
        if (!w)
            break;
        uv_mutex_lock(&w->mutex);
        qb_worker_stop_locked(w);
        uv_mutex_unlock(&w->mutex);
        break;
    case 2: /* ref */
        wh->refed = 1;
        if (wh->running && !wh->handle_closed)
            uv_ref((uv_handle_t *)&wh->exit_async);
        break;
    case 3: /* unref */
        wh->refed = 0;
        if (!wh->handle_closed)
            uv_unref((uv_handle_t *)&wh->exit_async);
        break;
    case 4: /* hasRef */
        return JS_NewBool(ctx, wh->running && !wh->handle_closed &&
                                   uv_has_ref((uv_handle_t *)&wh->exit_async));
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_worker_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("start", 0, qb_worker_handle_method, 0),
    JS_CFUNC_MAGIC_DEF("terminate", 0, qb_worker_handle_method, 1),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_worker_handle_method, 2),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_worker_handle_method, 3),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_worker_handle_method, 4),
};

/* threadId(): 0 in the main thread */
static JSValue qb_worker_threadId(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    QBWorkerThread *wt = qr->worker;
    return JS_NewInt64(ctx, wt ? wt->w->thread_id : 0);
}

/* stopRuntime(code): end the current worker runtime without throwing
   (used after an uncaught error was reported to the parent) */
static JSValue qb_worker_stopRuntime(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    int code = 1;
    if (argc > 0 && JS_ToInt32(ctx, &code, argv[0]))
        return JS_EXCEPTION;
    if (qr->is_main_thread)
        return JS_ThrowTypeError(ctx, "stopRuntime() is only available in workers");
    if (!qr->exiting) {
        qr->exit_code = code;
        qr->exiting = 1;
    }
    uv_stop(qr->loop);
    return JS_UNDEFINED;
}

/* ---------------------------------------------------------- vm contexts */

static JSClassID qb_vmctx_class_id;
static JSClassID qb_script_class_id;

static void qb_vmctx_finalizer(JSRuntime *rt, JSValue val)
{
    JSContext *c = JS_GetOpaque(val, qb_vmctx_class_id);
    if (c)
        JS_FreeContext(c);
}

static void qb_vmctx_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSContext *c = JS_GetOpaque(val, qb_vmctx_class_id);
    /* the GC header comes first in JSContext */
    if (c)
        mark_func(rt, (JSGCObjectHeader *)c);
}

static JSClassDef qb_vmctx_class = {
    "VMContext",
    .finalizer = qb_vmctx_finalizer,
    .gc_mark = qb_vmctx_mark,
};

typedef struct QBScript {
    JSContext *ctx; /* referenced */
    JSValue code;   /* compiled script (JS_TAG_FUNCTION_BYTECODE) */
} QBScript;

static void qb_script_finalizer(JSRuntime *rt, JSValue val)
{
    QBScript *s = JS_GetOpaque(val, qb_script_class_id);
    if (!s)
        return;
    JS_FreeValueRT(rt, s->code);
    JS_FreeContext(s->ctx);
    free(s);
}

static void qb_script_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBScript *s = JS_GetOpaque(val, qb_script_class_id);
    if (!s)
        return;
    JS_MarkValue(rt, s->code, mark_func);
    mark_func(rt, (JSGCObjectHeader *)s->ctx);
}

static JSClassDef qb_script_class = {
    "VMScript",
    .finalizer = qb_script_finalizer,
    .gc_mark = qb_script_mark,
};

/* createContext(): a new realm (the JS builtins only) of this runtime */
static JSValue qb_worker_createContext(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    JSContext *c;
    JSValue obj;

    c = JS_NewContext(JS_GetRuntime(ctx));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
#ifdef CONFIG_TEMPORAL_INTL
    JS_AddIntrinsicTemporal(c);
    JS_AddIntrinsicIntl(c);
#endif
    obj = JS_NewObjectClass(ctx, qb_vmctx_class_id);
    if (JS_IsException(obj)) {
        JS_FreeContext(c);
        return obj;
    }
    JS_SetOpaque(obj, c);
    return obj;
}

/* the realm of a context handle, the current one for null/undefined */
static JSContext *qb_vmctx_get(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return ctx;
    return JS_GetOpaque2(ctx, v, qb_vmctx_class_id);
}

static JSValue qb_vmctx_global(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSContext *c = JS_GetOpaque2(ctx, this_val, qb_vmctx_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_GetGlobalObject(c);
}

/* compileScript(context|null, src, filename): a script compiled in the
   realm; script.run() evaluates it there */
static JSValue qb_worker_compileScript(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    JSContext *c = qb_vmctx_get(ctx, argv[0]);
    const char *src, *filename;
    size_t len;
    JSValue code, obj;
    QBScript *s;

    if (!c)
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!src)
        return JS_EXCEPTION;
    filename = JS_ToCString(ctx, argv[2]);
    if (!filename) {
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    code = JS_Eval(c, src, len, filename, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, filename);
    if (JS_IsException(code))
        return code;
    obj = JS_NewObjectClass(ctx, qb_script_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, code);
        return obj;
    }
    s = malloc(sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, code);
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->ctx = JS_DupContext(c);
    s->code = code;
    JS_SetOpaque(obj, s);
    return obj;
}

static JSValue qb_script_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBScript *s = JS_GetOpaque2(ctx, this_val, qb_script_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_EvalFunction(s->ctx, JS_DupValue(s->ctx, s->code));
}

/* evalInContext(context|null, src, filename): evaluate a global script */
static JSValue qb_worker_evalInContext(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    JSContext *c = qb_vmctx_get(ctx, argv[0]);
    const char *src, *filename;
    size_t len;
    JSValue ret;

    if (!c)
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!src)
        return JS_EXCEPTION;
    filename = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
    ret =
        JS_Eval(c, src, len, filename ? filename : "evalmachine.<anonymous>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    if (filename)
        JS_FreeCString(ctx, filename);
    return ret;
}

static const JSCFunctionListEntry qb_vmctx_proto_funcs[] = {
    JS_CFUNC_DEF("global", 0, qb_vmctx_global),
};

static const JSCFunctionListEntry qb_script_proto_funcs[] = {
    JS_CFUNC_DEF("run", 0, qb_script_run),
};

/* ---------------------------------------------------------------- module */

static const JSCFunctionListEntry qb_worker_funcs[] = {
    JS_CFUNC_DEF("createChannel", 0, qb_worker_createChannel),
    JS_CFUNC_DEF("openBroadcast", 1, qb_worker_openBroadcast),
    JS_CFUNC_DEF("createWorker", 2, qb_worker_createWorker),
    JS_CFUNC_DEF("threadId", 0, qb_worker_threadId),
    JS_CFUNC_DEF("stopRuntime", 1, qb_worker_stopRuntime),
    JS_CFUNC_DEF("createContext", 0, qb_worker_createContext),
    JS_CFUNC_DEF("compileScript", 3, qb_worker_compileScript),
    JS_CFUNC_DEF("evalInContext", 3, qb_worker_evalInContext),
    JS_CFUNC_DEF("serialize", 3, qb_worker_serialize),
    JS_CFUNC_DEF("deserialize", 2, qb_worker_deserialize),
    JS_CFUNC_DEF("setCloneHooks", 2, qb_worker_setCloneHooks),
    JS_CFUNC_DEF("markUncloneable", 1, qb_worker_markUncloneable),
};

static void qb_worker_new_class(JSContext *ctx, JSClassID *pid, JSClassDef *def,
                                const JSCFunctionListEntry *funcs, int n)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;
    JS_NewClassID(pid);
    if (!JS_IsRegisteredClass(rt, *pid))
        JS_NewClass(rt, *pid, def);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, n);
    JS_SetClassProto(ctx, *pid, proto);
}

static int qb_worker_module_init(JSContext *ctx, JSModuleDef *m)
{
    qb_worker_new_class(ctx, &qb_port_class_id, &qb_port_class, qb_port_proto_funcs,
                        countof(qb_port_proto_funcs));
    qb_worker_new_class(ctx, &qb_worker_class_id, &qb_worker_class, qb_worker_proto_funcs,
                        countof(qb_worker_proto_funcs));
    qb_worker_new_class(ctx, &qb_vmctx_class_id, &qb_vmctx_class, qb_vmctx_proto_funcs,
                        countof(qb_vmctx_proto_funcs));
    qb_worker_new_class(ctx, &qb_script_class_id, &qb_script_class, qb_script_proto_funcs,
                        countof(qb_script_proto_funcs));
    return JS_SetModuleExportList(ctx, m, qb_worker_funcs, countof(qb_worker_funcs));
}

JSModuleDef *qb_init_worker(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, qb_worker_module_init);
    if (m && JS_AddModuleExportList(ctx, m, qb_worker_funcs, countof(qb_worker_funcs)) < 0)
        return NULL;
    return m;
}
