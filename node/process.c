/*
 * qbun:process native binding: child processes (uv_spawn, async and
 * sync), signals (uv_signal_t, kill) and the process information of
 * node:process (ids, umask, resource usage, title, memory limits).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "qbun.h"

/* node/os.c (to be declared in qbun.h) */
const char *qb_signal_name(int signum);
JSValue qb_signals_object(JSContext *ctx);

static JSClassID qb_process_class_id;
static JSClassID qb_signal_class_id;

/* ------------------------------------------------------------- helpers */

/* a NULL terminated copy of an array of strings (NULL with an exception;
 *pcount gets the length) */
static char **qb_str_array(JSContext *ctx, JSValueConst arr, int *pcount)
{
    int64_t len, i;
    char **v;

    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "expected an array of strings");
        return NULL;
    }
    if (JS_GetLength(ctx, arr, &len) < 0)
        return NULL;
    if (len < 0 || len > 1000000) {
        JS_ThrowRangeError(ctx, "too many strings");
        return NULL;
    }
    v = calloc((size_t)len + 1, sizeof(char *));
    if (!v) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char *s;
        if (JS_IsException(e))
            goto fail;
        s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s)
            goto fail;
        v[i] = strdup(s);
        JS_FreeCString(ctx, s);
        if (!v[i]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    if (pcount)
        *pcount = (int)len;
    return v;
fail:
    for (i = 0; i < len; i++)
        free(v[i]);
    free(v);
    return NULL;
}

static void qb_str_array_free(char **v)
{
    char **p;
    if (!v)
        return;
    for (p = v; *p; p++)
        free(*p);
    free(v);
}

/* a strdup() of the string property 'name' (NULL if undefined/null; -1
   on exception) */
static int qb_get_str_prop(JSContext *ctx, JSValueConst obj, const char *name, char **pres)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s;
    *pres = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *pres = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*pres) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

/* the number property 'name': 0 if undefined/null, 1 if set, -1 on exception */
static int qb_get_num_prop(JSContext *ctx, JSValueConst obj, const char *name, double *pres)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int r;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    r = JS_ToFloat64(ctx, pres, v);
    JS_FreeValue(ctx, v);
    return r ? -1 : 1;
}

static int qb_get_bool_prop(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int r;
    if (JS_IsException(v))
        return -1;
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

/* is 'h' a handle of 'loop' (in its handle queue)? */
typedef struct QBWalkFind {
    uv_handle_t *target;
    int found;
} QBWalkFind;

static void qb_walk_find_cb(uv_handle_t *h, void *arg)
{
    QBWalkFind *f = arg;
    if (h == f->target)
        f->found = 1;
}

static int qb_handle_in_loop(uv_loop_t *loop, uv_handle_t *h)
{
    QBWalkFind f = {h, 0};
    uv_walk(loop, qb_walk_find_cb, &f);
    return f.found;
}

/* the options shared by spawn() and spawnSync() */
typedef struct QBSpawnOpts {
    char *file;
    char **args;
    int nargs;
    char **env;
    char *cwd;
    unsigned int flags;
    uv_uid_t uid;
    uv_gid_t gid;
} QBSpawnOpts;

static void qb_spawn_opts_free(QBSpawnOpts *o)
{
    free(o->file);
    qb_str_array_free(o->args);
    qb_str_array_free(o->env);
    free(o->cwd);
    memset(o, 0, sizeof(*o));
}

static int qb_spawn_opts_parse(JSContext *ctx, JSValueConst obj, QBSpawnOpts *o)
{
    JSValue v;
    double d;
    int r;

    memset(o, 0, sizeof(*o));
    if (!JS_IsObject(obj)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    if (qb_get_str_prop(ctx, obj, "file", &o->file) < 0)
        goto fail;
    if (!o->file) {
        JS_ThrowTypeError(ctx, "options.file must be a string");
        goto fail;
    }
    v = JS_GetPropertyStr(ctx, obj, "args");
    if (JS_IsException(v))
        goto fail;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        o->args = calloc(2, sizeof(char *));
        if (!o->args || !(o->args[0] = strdup(o->file))) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        o->nargs = 1;
    } else {
        o->args = qb_str_array(ctx, v, &o->nargs);
        JS_FreeValue(ctx, v);
        if (!o->args)
            goto fail;
        if (o->nargs == 0) {
            /* uv_spawn() needs args[0] */
            char **a = realloc(o->args, 2 * sizeof(char *));
            if (!a) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            o->args = a;
            a[0] = strdup(o->file);
            a[1] = NULL;
            if (!a[0]) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            o->nargs = 1;
        }
    }
    v = JS_GetPropertyStr(ctx, obj, "env");
    if (JS_IsException(v))
        goto fail;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        o->env = qb_str_array(ctx, v, NULL);
        JS_FreeValue(ctx, v);
        if (!o->env)
            goto fail;
    }
    if (qb_get_str_prop(ctx, obj, "cwd", &o->cwd) < 0)
        goto fail;
    r = qb_get_bool_prop(ctx, obj, "detached");
    if (r < 0)
        goto fail;
    if (r)
        o->flags |= UV_PROCESS_DETACHED;
    r = qb_get_num_prop(ctx, obj, "uid", &d);
    if (r < 0)
        goto fail;
    if (r) {
        o->flags |= UV_PROCESS_SETUID;
        o->uid = (uv_uid_t)d;
    }
    r = qb_get_num_prop(ctx, obj, "gid", &d);
    if (r < 0)
        goto fail;
    if (r) {
        o->flags |= UV_PROCESS_SETGID;
        o->gid = (uv_gid_t)d;
    }
    return 0;
fail:
    qb_spawn_opts_free(o);
    return -1;
}

/* a spawn error like Node's errnoException(err, 'spawn ' + file):
   message "spawn file ENOENT", errno, code, syscall, path and spawnargs
   (the arguments after argv[0]) */
static JSValue qb_spawn_error(JSContext *ctx, int err, const char *prefix, const QBSpawnOpts *o)
{
    char syscall[1024], msg[1200];
    const char *code = uv_err_name(err);
    JSValue e, arr;
    int i;

    snprintf(syscall, sizeof(syscall), "%s %s", prefix, o->file);
    snprintf(msg, sizeof(msg), "%s %s", syscall, code);
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, strlen(msg));
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "path", JS_NewString(ctx, o->file), JS_PROP_C_W_E);
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        for (i = 1; i < o->nargs; i++)
            JS_SetPropertyUint32(ctx, arr, i - 1, JS_NewString(ctx, o->args[i]));
        JS_DefinePropertyValueStr(ctx, e, "spawnargs", arr, JS_PROP_C_W_E);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return e;
}

/* ------------------------------------------------------- ProcessHandle */

typedef struct QBProcess {
    uv_process_t h;
    QBRuntime *qr;
    JSValue onexit;
    JSValue self; /* strong until the exit (or close) */
    int pid;
    int exited;
    int closed;
    int close_done;
    int js_dead;
} QBProcess;

static void qb_process_release(QBProcess *p)
{
    JSValue self = p->self;
    p->self = JS_UNDEFINED;
    JS_FreeValue(p->qr->ctx, self);
}

static void qb_process_close_cb(uv_handle_t *h)
{
    QBProcess *p = h->data;
    p->close_done = 1;
    if (p->js_dead)
        free(p);
}

/* close callback of a handle without JS object (failed exec) */
static void qb_process_free_cb(uv_handle_t *h)
{
    free(h->data);
}

static void qb_process_finalizer(JSRuntime *rt, JSValue val)
{
    QBProcess *p = JS_GetOpaque(val, qb_process_class_id);
    if (!p)
        return;
    JS_FreeValueRT(rt, p->onexit);
    p->onexit = JS_UNDEFINED;
    p->js_dead = 1;
    if (!p->closed) {
        p->closed = 1;
        uv_close((uv_handle_t *)&p->h, qb_process_close_cb);
    } else if (p->close_done) {
        free(p);
    }
}

static void qb_process_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBProcess *p = JS_GetOpaque(val, qb_process_class_id);
    if (p)
        JS_MarkValue(rt, p->onexit, mark_func);
}

static JSClassDef qb_process_class = {
    "ProcessHandle",
    .finalizer = qb_process_finalizer,
    .gc_mark = qb_process_mark,
};

static void qb_process_exit_cb(uv_process_t *h, int64_t exit_status, int term_signal)
{
    QBProcess *p = h->data;
    JSContext *ctx = p->qr->ctx;
    JSValue self, cb, args[2], ret;
    const char *sig;

    p->exited = 1;
    if (p->js_dead)
        return;
    self = JS_DupValue(ctx, p->self);
    cb = JS_DupValue(ctx, p->onexit);
    qb_process_release(p);
    args[0] = JS_NewInt64(ctx, exit_status);
    sig = term_signal ? qb_signal_name(term_signal) : NULL;
    if (sig)
        args[1] = JS_NewString(ctx, sig);
    else if (term_signal)
        args[1] = JS_NewInt32(ctx, term_signal);
    else
        args[1] = JS_NULL;
    if (JS_IsFunction(ctx, cb)) {
        ret = qb_call(ctx, cb, self, 2, (JSValueConst *)args);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, self);
}

/* the stdio containers of spawn() */
static int qb_parse_stdio(JSContext *ctx, JSValueConst arr, uv_stdio_container_t **pstdio,
                          int *pcount)
{
    uv_stdio_container_t *stdio;
    int64_t len, i;

    *pstdio = NULL;
    *pcount = 0;
    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "options.stdio must be an array");
        return -1;
    }
    if (JS_GetLength(ctx, arr, &len) < 0)
        return -1;
    if (len < 0 || len > 1024) {
        JS_ThrowRangeError(ctx, "too many stdio entries");
        return -1;
    }
    stdio = calloc(len ? (size_t)len : 1, sizeof(*stdio));
    if (!stdio) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        JSValue tv;
        const char *type;
        if (JS_IsException(e))
            goto fail;
        if (!JS_IsObject(e)) {
            JS_FreeValue(ctx, e);
            stdio[i].flags = UV_IGNORE;
            continue;
        }
        tv = JS_GetPropertyStr(ctx, e, "type");
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, e);
            goto fail;
        }
        type = JS_ToCString(ctx, tv);
        JS_FreeValue(ctx, tv);
        if (!type) {
            JS_FreeValue(ctx, e);
            goto fail;
        }
        if (!strcmp(type, "pipe") || !strcmp(type, "overlapped")) {
            JSValue hv = JS_GetPropertyStr(ctx, e, "handle");
            uv_stream_t *st;
            int readable, writable, flags = UV_CREATE_PIPE;
            if (JS_IsException(hv))
                goto fail_entry;
            st = qb_stream_handle(ctx, hv);
            /* the handle object is kept alive by the stdio array */
            JS_FreeValue(ctx, hv);
            if (!st)
                goto fail_entry;
            if (st->type != UV_NAMED_PIPE) {
                JS_ThrowTypeError(ctx, "stdio pipes need a Pipe handle");
                goto fail_entry;
            }
            readable = qb_get_bool_prop(ctx, e, "readable");
            writable = qb_get_bool_prop(ctx, e, "writable");
            if (readable < 0 || writable < 0)
                goto fail_entry;
            /* readable/writable are from the child's point of view (the
               child reads its stdin), as are the libuv flags */
            if (readable)
                flags |= UV_READABLE_PIPE;
            if (writable)
                flags |= UV_WRITABLE_PIPE;
            if (!readable && !writable)
                flags |= UV_READABLE_PIPE | UV_WRITABLE_PIPE;
            stdio[i].flags = (uv_stdio_flags)flags;
            stdio[i].data.stream = st;
        } else if (!strcmp(type, "inherit") || !strcmp(type, "fd")) {
            JSValue fv = JS_GetPropertyStr(ctx, e, "fd");
            int32_t fd = (int32_t)i;
            if (JS_IsException(fv))
                goto fail_entry;
            if (!JS_IsUndefined(fv) && JS_ToInt32(ctx, &fd, fv)) {
                JS_FreeValue(ctx, fv);
                goto fail_entry;
            }
            JS_FreeValue(ctx, fv);
            stdio[i].flags = UV_INHERIT_FD;
            stdio[i].data.fd = fd;
        } else if (!strcmp(type, "wrap")) {
            JSValue hv = JS_GetPropertyStr(ctx, e, "handle");
            uv_stream_t *st;
            if (JS_IsException(hv))
                goto fail_entry;
            st = qb_stream_handle(ctx, hv);
            JS_FreeValue(ctx, hv);
            if (!st)
                goto fail_entry;
            stdio[i].flags = UV_INHERIT_STREAM;
            stdio[i].data.stream = st;
        } else {
            stdio[i].flags = UV_IGNORE;
        }
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, e);
        continue;
    fail_entry:
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, e);
        goto fail;
    }
    *pstdio = stdio;
    *pcount = (int)len;
    return 0;
fail:
    free(stdio);
    return -1;
}

/* spawn(options, onexit): a ProcessHandle; throws the spawn error */
static JSValue qb_process_spawn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    uv_process_options_t uo;
    uv_stdio_container_t *stdio = NULL;
    QBSpawnOpts o;
    QBProcess *p;
    JSValue stdio_val, obj;
    int nstdio, err;

    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "onexit must be a function");
    if (qb_spawn_opts_parse(ctx, argv[0], &o) < 0)
        return JS_EXCEPTION;
    stdio_val = JS_GetPropertyStr(ctx, argv[0], "stdio");
    if (JS_IsException(stdio_val))
        goto fail;
    if (qb_parse_stdio(ctx, stdio_val, &stdio, &nstdio) < 0) {
        JS_FreeValue(ctx, stdio_val);
        goto fail;
    }
    obj = JS_NewObjectClass(ctx, qb_process_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, stdio_val);
        goto fail;
    }
    p = calloc(1, sizeof(*p));
    if (!p) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, stdio_val);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    p->qr = qr;
    p->onexit = JS_UNDEFINED;
    p->self = JS_UNDEFINED;
    p->h.data = p;

    memset(&uo, 0, sizeof(uo));
    uo.exit_cb = qb_process_exit_cb;
    uo.file = o.file;
    uo.args = o.args;
    uo.env = o.env;
    uo.cwd = o.cwd;
    uo.flags = o.flags;
    uo.stdio_count = nstdio;
    uo.stdio = stdio;
    uo.uid = o.uid;
    uo.gid = o.gid;
    err = uv_spawn(qr->loop, &p->h, &uo);
    /* the stream handles of 'stdio' were borrowed from stdio_val */
    JS_FreeValue(ctx, stdio_val);
    free(stdio);
    stdio = NULL;
    if (err < 0) {
        JSValue e;
        /* a failed exec leaves an initialized handle to close; other
           failures removed it from the loop already */
        if (qb_handle_in_loop(qr->loop, (uv_handle_t *)&p->h))
            uv_close((uv_handle_t *)&p->h, qb_process_free_cb);
        else
            free(p);
        JS_FreeValue(ctx, obj);
        e = qb_spawn_error(ctx, err, "spawn", &o);
        qb_spawn_opts_free(&o);
        if (JS_IsException(e))
            return e;
        return JS_Throw(ctx, e);
    }
    qb_spawn_opts_free(&o);
    p->pid = p->h.pid;
    p->onexit = JS_DupValue(ctx, argv[1]);
    p->self = JS_DupValue(ctx, obj);
    JS_SetOpaque(obj, p);
    return obj;
fail:
    free(stdio);
    qb_spawn_opts_free(&o);
    return JS_EXCEPTION;
}

static QBProcess *qb_process_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_process_class_id);
}

static JSValue qb_process_get_pid(JSContext *ctx, JSValueConst this_val)
{
    QBProcess *p = qb_process_get(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->pid);
}

static JSValue qb_process_kill_method(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBProcess *p = qb_process_get(ctx, this_val);
    int32_t sig = SIGTERM;
    if (!p)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &sig, argv[0]))
        return JS_EXCEPTION;
    if (p->closed || p->exited)
        return JS_NewInt32(ctx, UV_ESRCH);
    return JS_NewInt32(ctx, uv_process_kill(&p->h, sig));
}

/* ref(), unref(), hasRef(), close() */
static JSValue qb_process_method(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic)
{
    QBProcess *p = qb_process_get(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!p->closed)
            uv_ref((uv_handle_t *)&p->h);
        break;
    case 1:
        if (!p->closed)
            uv_unref((uv_handle_t *)&p->h);
        break;
    case 2:
        return JS_NewBool(ctx, !p->closed && uv_has_ref((uv_handle_t *)&p->h));
    case 3:
        if (!p->closed) {
            p->closed = 1;
            uv_close((uv_handle_t *)&p->h, qb_process_close_cb);
        }
        qb_process_release(p);
        break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_process_proto_funcs[] = {
    JS_CGETSET_DEF("pid", qb_process_get_pid, NULL),
    JS_CFUNC_DEF("kill", 1, qb_process_kill_method),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_process_method, 0),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_process_method, 1),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_process_method, 2),
    JS_CFUNC_MAGIC_DEF("close", 0, qb_process_method, 3),
};

/* ----------------------------------------------------------- spawnSync */

#define QB_SYNC_READ_SIZE (64 * 1024)

struct QBSync;

typedef struct QBSyncPipe {
    uv_pipe_t pipe;
    struct QBSync *sync;
    int index;
    int child_reads; /* the child's stdin: we write */
    int used;        /* a "pipe" entry */
    int closing;
    uint8_t *data;
    size_t len, cap;
    uv_write_t wreq;
} QBSyncPipe;

typedef struct QBSync {
    uv_loop_t loop;
    uv_process_t proc;
    uv_timer_t timer;
    int timer_active;
    int timer_closing;
    QBSyncPipe *pipes;
    int npipes;
    uint8_t *input;
    size_t input_len;
    uint8_t *scratch;
    int spawned;
    int exited;
    int64_t exit_status;
    int term_signal;
    int killed;
    int error;
    int kill_signal;
    double max_buffer;
    double total;
} QBSync;

static void qb_sync_set_error(QBSync *s, int err)
{
    if (!s->error)
        s->error = err;
}

static void qb_sync_noop_close_cb(uv_handle_t *h)
{
}

static void qb_sync_close_timer(QBSync *s)
{
    if (s->timer_active && !s->timer_closing) {
        s->timer_closing = 1;
        uv_timer_stop(&s->timer);
        uv_close((uv_handle_t *)&s->timer, qb_sync_noop_close_cb);
    }
}

static void qb_sync_check_done(QBSync *s)
{
    int i;
    if (s->spawned && !s->exited)
        return;
    for (i = 0; i < s->npipes; i++) {
        if (s->pipes[i].used && !s->pipes[i].closing)
            return;
    }
    qb_sync_close_timer(s);
}

static void qb_sync_pipe_close_cb(uv_handle_t *h)
{
    QBSyncPipe *sp = h->data;
    qb_sync_check_done(sp->sync);
}

static void qb_sync_pipe_close(QBSyncPipe *sp)
{
    if (sp->used && !sp->closing) {
        sp->closing = 1;
        uv_close((uv_handle_t *)&sp->pipe, qb_sync_pipe_close_cb);
    }
}

static void qb_sync_close_pipes(QBSync *s)
{
    int i;
    for (i = 0; i < s->npipes; i++)
        qb_sync_pipe_close(&s->pipes[i]);
}

static void qb_sync_kill(QBSync *s)
{
    int r;
    if (s->killed)
        return;
    s->killed = 1;
    if (s->spawned && !s->exited) {
        r = uv_process_kill(&s->proc, s->kill_signal);
        if (r < 0 && r != UV_ESRCH) {
            qb_sync_set_error(s, r);
            r = uv_process_kill(&s->proc, SIGKILL);
            (void)r;
        }
    }
    qb_sync_close_pipes(s);
    qb_sync_close_timer(s);
}

static void qb_sync_exit_cb(uv_process_t *h, int64_t exit_status, int term_signal)
{
    QBSync *s = h->data;
    s->exited = 1;
    s->exit_status = exit_status;
    s->term_signal = term_signal;
    uv_close((uv_handle_t *)h, qb_sync_noop_close_cb);
    qb_sync_check_done(s);
}

static void qb_sync_timer_cb(uv_timer_t *h)
{
    QBSync *s = h->data;
    qb_sync_set_error(s, UV_ETIMEDOUT);
    qb_sync_kill(s);
}

static void qb_sync_alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
    QBSyncPipe *sp = h->data;
    buf->base = (char *)sp->sync->scratch;
    buf->len = QB_SYNC_READ_SIZE;
}

static void qb_sync_read_cb(uv_stream_t *st, ssize_t nread, const uv_buf_t *buf)
{
    QBSyncPipe *sp = st->data;
    QBSync *s = sp->sync;

    if (nread == 0)
        return;
    if (nread < 0) {
        if (nread != UV_EOF)
            qb_sync_set_error(s, (int)nread);
        qb_sync_pipe_close(sp);
        return;
    }
    if (sp->len + (size_t)nread > sp->cap) {
        size_t cap = sp->cap ? sp->cap * 2 : QB_SYNC_READ_SIZE;
        uint8_t *d;
        while (cap < sp->len + (size_t)nread)
            cap *= 2;
        d = realloc(sp->data, cap);
        if (!d) {
            qb_sync_set_error(s, UV_ENOMEM);
            qb_sync_kill(s);
            return;
        }
        sp->data = d;
        sp->cap = cap;
    }
    memcpy(sp->data + sp->len, buf->base, (size_t)nread);
    sp->len += (size_t)nread;
    s->total += (double)nread;
    if (s->max_buffer > 0 && s->total > s->max_buffer) {
        qb_sync_set_error(s, UV_ENOBUFS);
        qb_sync_kill(s);
    }
}

static void qb_sync_write_cb(uv_write_t *req, int status)
{
    QBSyncPipe *sp = req->data;
    /* EPIPE: the child does not read its input, as in Node not an error */
    if (status < 0 && status != UV_EPIPE && status != UV_ECANCELED)
        qb_sync_set_error(sp->sync, status);
    qb_sync_pipe_close(sp);
}

static void qb_sync_walk_close_cb(uv_handle_t *h, void *arg)
{
    if (!uv_is_closing(h))
        uv_close(h, qb_sync_noop_close_cb);
}

/* spawnSync(options) */
static JSValue qb_process_spawnSync(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    uv_process_options_t uo;
    uv_stdio_container_t *stdio = NULL;
    QBSpawnOpts o;
    QBSync *s;
    JSValue stdio_val = JS_UNDEFINED, v, result = JS_EXCEPTION, output;
    int64_t nstdio = 0;
    int i, err, loop_ok = 0, r;
    double d;

    if (qb_spawn_opts_parse(ctx, argv[0], &o) < 0)
        return JS_EXCEPTION;
    s = calloc(1, sizeof(*s));
    if (!s) {
        qb_spawn_opts_free(&o);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->kill_signal = SIGTERM;

    /* options */
    r = qb_get_num_prop(ctx, argv[0], "killSignal", &d);
    if (r < 0)
        goto done;
    if (r)
        s->kill_signal = (int)d;
    r = qb_get_num_prop(ctx, argv[0], "maxBuffer", &d);
    if (r < 0)
        goto done;
    if (r && d > 0)
        s->max_buffer = d;
    v = JS_GetPropertyStr(ctx, argv[0], "input");
    if (JS_IsException(v))
        goto done;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, v, &len);
        if (!p) {
            JS_FreeValue(ctx, v);
            goto done;
        }
        s->input = malloc(len ? len : 1);
        if (!s->input) {
            JS_FreeValue(ctx, v);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        memcpy(s->input, p, len);
        s->input_len = len;
    }
    JS_FreeValue(ctx, v);
    stdio_val = JS_GetPropertyStr(ctx, argv[0], "stdio");
    if (JS_IsException(stdio_val))
        goto done;
    if (!JS_IsUndefined(stdio_val) && !JS_IsNull(stdio_val)) {
        if (!JS_IsArray(ctx, stdio_val)) {
            JS_ThrowTypeError(ctx, "options.stdio must be an array");
            goto done;
        }
        if (JS_GetLength(ctx, stdio_val, &nstdio) < 0)
            goto done;
        if (nstdio < 0 || nstdio > 1024) {
            JS_ThrowRangeError(ctx, "too many stdio entries");
            goto done;
        }
    }
    s->scratch = malloc(QB_SYNC_READ_SIZE);
    s->pipes = calloc(nstdio ? (size_t)nstdio : 1, sizeof(QBSyncPipe));
    stdio = calloc(nstdio ? (size_t)nstdio : 1, sizeof(*stdio));
    if (!s->scratch || !s->pipes || !stdio) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    s->npipes = (int)nstdio;
    err = uv_loop_init(&s->loop);
    if (err < 0) {
        qb_throw_uv(ctx, err, "uv_loop_init", NULL, NULL);
        goto done;
    }
    loop_ok = 1;
    for (i = 0; i < (int)nstdio; i++) {
        QBSyncPipe *sp = &s->pipes[i];
        sp->sync = s;
        sp->index = i;
        v = JS_GetPropertyUint32(ctx, stdio_val, (uint32_t)i);
        if (JS_IsException(v))
            goto done;
        if (JS_IsNumber(v)) {
            int32_t fd;
            if (JS_ToInt32(ctx, &fd, v)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            stdio[i].flags = UV_INHERIT_FD;
            stdio[i].data.fd = fd;
        } else if (JS_IsString(v)) {
            const char *t = JS_ToCString(ctx, v);
            if (!t) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            if (!strcmp(t, "pipe") || !strcmp(t, "overlapped")) {
                uv_pipe_init(&s->loop, &sp->pipe, 0);
                sp->pipe.data = sp;
                sp->used = 1;
                sp->child_reads = i == 0;
                stdio[i].flags = (uv_stdio_flags)(UV_CREATE_PIPE |
                                                  (i == 0 ? UV_READABLE_PIPE : UV_WRITABLE_PIPE));
                stdio[i].data.stream = (uv_stream_t *)&sp->pipe;
            } else if (!strcmp(t, "inherit")) {
                stdio[i].flags = UV_INHERIT_FD;
                stdio[i].data.fd = i;
            } else {
                stdio[i].flags = UV_IGNORE;
            }
            JS_FreeCString(ctx, t);
        } else {
            stdio[i].flags = UV_IGNORE;
        }
        JS_FreeValue(ctx, v);
    }

    memset(&uo, 0, sizeof(uo));
    uo.exit_cb = qb_sync_exit_cb;
    uo.file = o.file;
    uo.args = o.args;
    uo.env = o.env;
    uo.cwd = o.cwd;
    uo.flags = o.flags;
    uo.stdio_count = (int)nstdio;
    uo.stdio = stdio;
    uo.uid = o.uid;
    uo.gid = o.gid;
    s->proc.data = s;
    err = uv_spawn(&s->loop, &s->proc, &uo);
    if (err < 0) {
        s->error = err;
        /* every handle gets closed below */
    } else {
        double timeout = 0;
        s->spawned = 1;
        r = qb_get_num_prop(ctx, argv[0], "timeout", &timeout);
        if (r < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            timeout = 0;
        }
        if (timeout > 0) {
            uv_timer_init(&s->loop, &s->timer);
            s->timer.data = s;
            s->timer_active = 1;
            uv_timer_start(&s->timer, qb_sync_timer_cb, (uint64_t)timeout, 0);
        }
        for (i = 0; i < s->npipes; i++) {
            QBSyncPipe *sp = &s->pipes[i];
            if (!sp->used)
                continue;
            if (sp->child_reads) {
                if (s->input && s->input_len > 0) {
                    uv_buf_t b;
                    b.base = (char *)s->input;
                    b.len = s->input_len;
                    sp->wreq.data = sp;
                    err = uv_write(&sp->wreq, (uv_stream_t *)&sp->pipe, &b, 1, qb_sync_write_cb);
                    if (err < 0) {
                        if (err != UV_EPIPE)
                            qb_sync_set_error(s, err);
                        qb_sync_pipe_close(sp);
                    }
                } else {
                    qb_sync_pipe_close(sp);
                }
            } else {
                err = uv_read_start((uv_stream_t *)&sp->pipe, qb_sync_alloc_cb, qb_sync_read_cb);
                if (err < 0) {
                    qb_sync_set_error(s, err);
                    qb_sync_pipe_close(sp);
                }
            }
        }
        if (s->error)
            qb_sync_kill(s);
        qb_sync_check_done(s);
        uv_run(&s->loop, UV_RUN_DEFAULT);
    }

    /* the result */
    result = JS_NewObject(ctx);
    if (JS_IsException(result))
        goto done;
    output = JS_NewArray(ctx);
    if (JS_IsException(output)) {
        JS_FreeValue(ctx, result);
        result = JS_EXCEPTION;
        goto done;
    }
    for (i = 0; i < s->npipes; i++) {
        QBSyncPipe *sp = &s->pipes[i];
        JSValue b;
        if (sp->used && !sp->child_reads && s->spawned)
            b = qb_new_buffer(ctx, sp->data ? sp->data : (const uint8_t *)"", sp->len);
        else
            b = JS_NULL;
        if (JS_IsException(b) || JS_SetPropertyUint32(ctx, output, (uint32_t)i, b) < 0) {
            JS_FreeValue(ctx, output);
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
            goto done;
        }
    }
    if (qb_set(ctx, result, "pid", JS_NewInt32(ctx, s->spawned ? s->proc.pid : 0)) < 0 ||
        qb_set(ctx, result, "output", output) < 0)
        goto fail_result;
    if (s->exited && s->term_signal == 0) {
        if (qb_set(ctx, result, "status", JS_NewInt64(ctx, s->exit_status)) < 0)
            goto fail_result;
    } else {
        if (qb_set(ctx, result, "status", JS_NULL) < 0)
            goto fail_result;
    }
    if (s->exited && s->term_signal) {
        const char *name = qb_signal_name(s->term_signal);
        if (qb_set(ctx, result, "signal",
                   name ? JS_NewString(ctx, name) : JS_NewInt32(ctx, s->term_signal)) < 0)
            goto fail_result;
    } else {
        if (qb_set(ctx, result, "signal", JS_NULL) < 0)
            goto fail_result;
    }
    if (s->error) {
        if (qb_set(ctx, result, "error", qb_spawn_error(ctx, s->error, "spawnSync", &o)) < 0)
            goto fail_result;
    }
    goto done;
fail_result:
    JS_FreeValue(ctx, result);
    result = JS_EXCEPTION;
done:
    if (loop_ok) {
        /* close what is left (the handles of a failed spawn, pipes kept
           open by a grandchild after a JS exception...) */
        uv_walk(&s->loop, qb_sync_walk_close_cb, NULL);
        uv_run(&s->loop, UV_RUN_DEFAULT);
        if (uv_loop_close(&s->loop) != 0) {
            /* should not happen: keep the memory rather than corrupt it */
            s = NULL;
        }
    }
    if (s) {
        for (i = 0; i < s->npipes; i++)
            free(s->pipes[i].data);
        free(s->pipes);
        free(s->scratch);
        free(s->input);
        free(s);
    }
    JS_FreeValue(ctx, stdio_val);
    free(stdio);
    qb_spawn_opts_free(&o);
    return result;
}

/* -------------------------------------------------------------- signals */

typedef struct QBSignal {
    uv_signal_t h;
    QBRuntime *qr;
    JSValue cb;
    JSValue self; /* strong while started */
    int signum;
    int closed;
    int close_done;
    int js_dead;
} QBSignal;

static void qb_signal_release(QBSignal *s)
{
    JSValue self = s->self;
    s->self = JS_UNDEFINED;
    JS_FreeValue(s->qr->ctx, self);
}

static void qb_signal_close_cb(uv_handle_t *h)
{
    QBSignal *s = h->data;
    s->close_done = 1;
    if (s->js_dead)
        free(s);
}

static void qb_signal_finalizer(JSRuntime *rt, JSValue val)
{
    QBSignal *s = JS_GetOpaque(val, qb_signal_class_id);
    if (!s)
        return;
    JS_FreeValueRT(rt, s->cb);
    s->cb = JS_UNDEFINED;
    s->js_dead = 1;
    if (!s->closed) {
        s->closed = 1;
        uv_close((uv_handle_t *)&s->h, qb_signal_close_cb);
    } else if (s->close_done) {
        free(s);
    }
}

static void qb_signal_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBSignal *s = JS_GetOpaque(val, qb_signal_class_id);
    if (s)
        JS_MarkValue(rt, s->cb, mark_func);
}

static JSClassDef qb_signal_class = {
    "SignalHandle",
    .finalizer = qb_signal_finalizer,
    .gc_mark = qb_signal_mark,
};

static void qb_signal_cb(uv_signal_t *h, int signum)
{
    QBSignal *s = h->data;
    JSContext *ctx = s->qr->ctx;
    JSValue self, cb, arg, ret;
    if (s->js_dead)
        return;
    self = JS_DupValue(ctx, s->self);
    cb = JS_DupValue(ctx, s->cb);
    arg = JS_NewInt32(ctx, signum);
    ret = qb_call(ctx, cb, self, 1, (JSValueConst *)&arg);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, self);
}

/* createSignal(signum, cb): a started signal watcher */
static JSValue qb_process_createSignal(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    QBSignal *s;
    JSValue obj;
    int32_t signum;
    int err;

    if (JS_ToInt32(ctx, &signum, argv[0]))
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    obj = JS_NewObjectClass(ctx, qb_signal_class_id);
    if (JS_IsException(obj))
        return obj;
    s = calloc(1, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->qr = qr;
    s->cb = JS_DupValue(ctx, argv[1]);
    s->self = JS_UNDEFINED;
    s->signum = signum;
    uv_signal_init(qr->loop, &s->h);
    s->h.data = s;
    JS_SetOpaque(obj, s);
    err = uv_signal_start(&s->h, qb_signal_cb, signum);
    if (err < 0) {
        JS_FreeValue(ctx, obj); /* the finalizer closes the handle */
        return qb_throw_uv(ctx, err, "uv_signal_start", NULL, NULL);
    }
    s->self = JS_DupValue(ctx, obj);
    return obj;
}

/* stop(), ref(), unref(), hasRef(), close(), start() */
static JSValue qb_signal_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                int magic)
{
    QBSignal *s = JS_GetOpaque2(ctx, this_val, qb_signal_class_id);
    int err;
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* stop */
        if (!s->closed)
            uv_signal_stop(&s->h);
        qb_signal_release(s);
        break;
    case 1:
        if (!s->closed)
            uv_ref((uv_handle_t *)&s->h);
        break;
    case 2:
        if (!s->closed)
            uv_unref((uv_handle_t *)&s->h);
        break;
    case 3:
        return JS_NewBool(ctx, !s->closed && uv_has_ref((uv_handle_t *)&s->h));
    case 4: /* close */
        if (!s->closed) {
            s->closed = 1;
            uv_close((uv_handle_t *)&s->h, qb_signal_close_cb);
        }
        qb_signal_release(s);
        break;
    case 5: /* start */
        if (s->closed)
            return qb_throw_uv(ctx, UV_EBADF, "uv_signal_start", NULL, NULL);
        err = uv_signal_start(&s->h, qb_signal_cb, s->signum);
        if (err < 0)
            return qb_throw_uv(ctx, err, "uv_signal_start", NULL, NULL);
        if (JS_IsUndefined(s->self))
            s->self = JS_DupValue(ctx, this_val);
        break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_signal_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("stop", 0, qb_signal_method, 0),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_signal_method, 1),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_signal_method, 2),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_signal_method, 3),
    JS_CFUNC_MAGIC_DEF("close", 0, qb_signal_method, 4),
    JS_CFUNC_MAGIC_DEF("start", 0, qb_signal_method, 5),
};

/* ---------------------------------------------------------- process info */

static JSValue qb_process_kill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t pid, sig;
    if (JS_ToInt32(ctx, &pid, argv[0]) || JS_ToInt32(ctx, &sig, argv[1]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, uv_kill(pid, sig));
}

static JSValue qb_process_ids(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue obj, groups;
    gid_t *list = NULL;
    gid_t egid = getegid();
    int n, i, has_egid = 0;
    uint32_t k = 0;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (qb_set(ctx, obj, "uid", JS_NewUint32(ctx, (uint32_t)getuid())) < 0 ||
        qb_set(ctx, obj, "euid", JS_NewUint32(ctx, (uint32_t)geteuid())) < 0 ||
        qb_set(ctx, obj, "gid", JS_NewUint32(ctx, (uint32_t)getgid())) < 0 ||
        qb_set(ctx, obj, "egid", JS_NewUint32(ctx, (uint32_t)egid)) < 0)
        goto fail;
    groups = JS_NewArray(ctx);
    if (JS_IsException(groups))
        goto fail;
    n = getgroups(0, NULL);
    if (n > 0) {
        list = malloc((size_t)n * sizeof(gid_t));
        if (list)
            n = getgroups(n, list);
        else
            n = 0;
    }
    for (i = 0; i < n; i++) {
        if (list[i] == egid)
            has_egid = 1;
        if (JS_SetPropertyUint32(ctx, groups, k++, JS_NewUint32(ctx, (uint32_t)list[i])) < 0) {
            free(list);
            JS_FreeValue(ctx, groups);
            goto fail;
        }
    }
    free(list);
    /* like Node's process.getgroups() */
    if (!has_egid &&
        JS_SetPropertyUint32(ctx, groups, k++, JS_NewUint32(ctx, (uint32_t)egid)) < 0) {
        JS_FreeValue(ctx, groups);
        goto fail;
    }
    if (qb_set(ctx, obj, "groups", groups) < 0)
        goto fail;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue qb_process_umask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    mode_t old;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        int32_t mask;
        if (JS_ToInt32(ctx, &mask, argv[0]))
            return JS_EXCEPTION;
        old = umask((mode_t)(mask & 0777));
    } else {
        old = umask(0);
        umask(old);
    }
    return JS_NewInt32(ctx, (int32_t)old);
}

static double qb_tv_us(const uv_timeval_t *tv)
{
    return (double)tv->tv_sec * 1e6 + (double)tv->tv_usec;
}

static JSValue qb_process_cpuUsage(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    uv_rusage_t ru;
    JSValue obj;
    int err = uv_getrusage(&ru);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_getrusage", NULL, NULL);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (qb_set(ctx, obj, "user", JS_NewFloat64(ctx, qb_tv_us(&ru.ru_utime))) < 0 ||
        qb_set(ctx, obj, "system", JS_NewFloat64(ctx, qb_tv_us(&ru.ru_stime))) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue qb_process_resourceUsage(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    uv_rusage_t ru;
    JSValue obj;
    int err = uv_getrusage(&ru);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_getrusage", NULL, NULL);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
#define QB_RU(name, val)                                                                           \
    if (qb_set(ctx, obj, name, JS_NewFloat64(ctx, (double)(val))) < 0)                             \
    goto fail
    QB_RU("userCPUTime", qb_tv_us(&ru.ru_utime));
    QB_RU("systemCPUTime", qb_tv_us(&ru.ru_stime));
    QB_RU("maxRSS", ru.ru_maxrss);
    QB_RU("sharedMemorySize", ru.ru_ixrss);
    QB_RU("unsharedDataSize", ru.ru_idrss);
    QB_RU("unsharedStackSize", ru.ru_isrss);
    QB_RU("minorPageFault", ru.ru_minflt);
    QB_RU("majorPageFault", ru.ru_majflt);
    QB_RU("swappedOut", ru.ru_nswap);
    QB_RU("fsRead", ru.ru_inblock);
    QB_RU("fsWrite", ru.ru_oublock);
    QB_RU("ipcSent", ru.ru_msgsnd);
    QB_RU("ipcReceived", ru.ru_msgrcv);
    QB_RU("signalsCount", ru.ru_nsignals);
    QB_RU("voluntaryContextSwitches", ru.ru_nvcsw);
    QB_RU("involuntaryContextSwitches", ru.ru_nivcsw);
#undef QB_RU
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue qb_process_setTitle(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    const char *title = JS_ToCString(ctx, argv[0]);
    int err;
    if (!title)
        return JS_EXCEPTION;
    err = uv_set_process_title(title);
    JS_FreeCString(ctx, title);
    if (err < 0 && err != UV_ENOSYS && err != UV_ENOBUFS)
        return qb_throw_uv(ctx, err, "uv_set_process_title", NULL, NULL);
    return JS_UNDEFINED;
}

static JSValue qb_process_getTitle(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    char buf[4096];
    int err = uv_get_process_title(buf, sizeof(buf));
    if (err < 0)
        return JS_NewString(ctx, "qbun");
    return JS_NewString(ctx, buf);
}

static JSValue qb_process_constrainedMemory(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    return JS_NewFloat64(ctx, (double)uv_get_constrained_memory());
}

static JSValue qb_process_availableMemory(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    return JS_NewFloat64(ctx, (double)uv_get_available_memory());
}

/* ---------------------------------------------------------------- module */

static const JSCFunctionListEntry qb_process_funcs[] = {
    JS_CFUNC_DEF("spawn", 2, qb_process_spawn),
    JS_CFUNC_DEF("spawnSync", 1, qb_process_spawnSync),
    JS_CFUNC_DEF("kill", 2, qb_process_kill),
    JS_CFUNC_DEF("createSignal", 2, qb_process_createSignal),
    JS_CFUNC_DEF("ids", 0, qb_process_ids),
    JS_CFUNC_DEF("umask", 1, qb_process_umask),
    JS_CFUNC_DEF("cpuUsage", 0, qb_process_cpuUsage),
    JS_CFUNC_DEF("resourceUsage", 0, qb_process_resourceUsage),
    JS_CFUNC_DEF("setTitle", 1, qb_process_setTitle),
    JS_CFUNC_DEF("getTitle", 0, qb_process_getTitle),
    JS_CFUNC_DEF("constrainedMemory", 0, qb_process_constrainedMemory),
    JS_CFUNC_DEF("availableMemory", 0, qb_process_availableMemory),
};

static int qb_process_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue signals;
    if (JS_SetModuleExportList(ctx, m, qb_process_funcs, countof(qb_process_funcs)) < 0)
        return -1;
    signals = qb_signals_object(ctx);
    if (JS_IsException(signals))
        return -1;
    return JS_SetModuleExport(ctx, m, "signals", signals);
}

static int qb_register_class(JSContext *ctx, JSClassID *pid, const JSClassDef *def,
                             const JSCFunctionListEntry *funcs, int nfuncs)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;
    JS_NewClassID(pid);
    if (JS_IsRegisteredClass(rt, *pid))
        return 0;
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, funcs, nfuncs) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, *pid, proto);
    return 0;
}

JSModuleDef *qb_init_process(JSContext *ctx, const char *name)
{
    JSModuleDef *m;
    if (qb_register_class(ctx, &qb_process_class_id, &qb_process_class, qb_process_proto_funcs,
                          countof(qb_process_proto_funcs)) < 0 ||
        qb_register_class(ctx, &qb_signal_class_id, &qb_signal_class, qb_signal_proto_funcs,
                          countof(qb_signal_proto_funcs)) < 0)
        return NULL;
    m = JS_NewCModule(ctx, name, qb_process_module_init);
    if (!m)
        return NULL;
    if (JS_AddModuleExportList(ctx, m, qb_process_funcs, countof(qb_process_funcs)) < 0 ||
        JS_AddModuleExport(ctx, m, "signals") < 0)
        return NULL;
    return m;
}
