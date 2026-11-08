/*
 * qbun core: the runtime state, the module loader (Bun/Node resolution,
 * TypeScript, CommonJS, JSON/text/file imports, builtin library modules),
 * the event loop driver, microtasks, uncaught errors, timers and
 * immediates. The qbun:core module gives the TypeScript library the
 * primitives the bootstrap needs.
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qbun.h"
#include "../cutils.h"
#include "../quickjs-libc.h"

extern char **environ;

typedef struct QBRejection {
    struct QBRejection *next;
    JSValue promise;
    JSValue reason;
} QBRejection;

static __thread int qb_call_depth;

QBRuntime *qb_get(JSContext *ctx)
{
    return JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

/* ---------------------------------------------------------------- errors */

JSValue qb_uv_error(JSContext *ctx, int err, const char *syscall, const char *path,
                    const char *dest)
{
    char msg[PATH_MAX * 2 + 256];
    const char *code = uv_err_name(err);
    JSValue e;
    int n;

    n = snprintf(msg, sizeof(msg), "%s: %s", code, uv_strerror(err));
    if (syscall)
        n += snprintf(msg + n, sizeof(msg) - n, ", %s", syscall);
    if (path && n < (int)sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - n, " '%s'", path);
    if (dest && n < (int)sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - n, " -> '%s'", dest);
    if (n >= (int)sizeof(msg))
        n = sizeof(msg) - 1;
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, n);
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    if (syscall)
        JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    if (path)
        JS_DefinePropertyValueStr(ctx, e, "path", JS_NewString(ctx, path), JS_PROP_C_W_E);
    if (dest)
        JS_DefinePropertyValueStr(ctx, e, "dest", JS_NewString(ctx, dest), JS_PROP_C_W_E);
    return e;
}

JSValue qb_throw_uv(JSContext *ctx, int err, const char *syscall, const char *path,
                    const char *dest)
{
    JSValue e = qb_uv_error(ctx, err, syscall, path, dest);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

JSValue qb_throw_code(JSContext *ctx, JSErrorEnum kind, const char *code, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    JSValue e;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(msg))
        n = sizeof(msg) - 1;
    e = JS_NewErrorLen(ctx, kind, msg, n);
    if (JS_IsException(e))
        return e;
    if (code)
        JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return JS_Throw(ctx, e);
}

/* --------------------------------------------------------------- buffers */

uint8_t *qb_get_bytes(JSContext *ctx, JSValueConst val, size_t *plen)
{
    uint8_t *p;
    size_t len;

    if (JS_GetTypedArrayType(val) >= 0) {
        p = JS_GetTypedArrayData(ctx, val, NULL, &len, NULL);
        if (!p) {
            /* empty or detached view */
            *plen = 0;
            return (uint8_t *)"";
        }
        *plen = len;
        return p;
    }
    if (JS_IsObject(val)) {
        p = JS_GetArrayBuffer(ctx, &len, val);
        if (p || (!JS_HasException(ctx) && len == 0)) {
            *plen = len;
            return p ? p : (uint8_t *)"";
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        /* DataView */
        {
            JSValue buf, off, blen;
            int64_t o, l;
            buf = JS_GetPropertyStr(ctx, val, "buffer");
            if (JS_IsObject(buf)) {
                p = JS_GetArrayBuffer(ctx, &len, buf);
                if (p) {
                    off = JS_GetPropertyStr(ctx, val, "byteOffset");
                    blen = JS_GetPropertyStr(ctx, val, "byteLength");
                    if (!JS_ToInt64(ctx, &o, off) && !JS_ToInt64(ctx, &l, blen) && o >= 0 &&
                        l >= 0 && (size_t)(o + l) <= len) {
                        JS_FreeValue(ctx, buf);
                        *plen = l;
                        return p + o;
                    }
                }
                if (JS_HasException(ctx))
                    JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, buf);
        }
    }
    JS_ThrowTypeError(ctx, "expected an ArrayBuffer, a typed array or a DataView");
    *plen = 0;
    return NULL;
}

JSValue qb_to_buffer(JSContext *ctx, JSValue u8)
{
    QBRuntime *qr = qb_get(ctx);
    if (!JS_IsException(u8) && qr && JS_IsObject(qr->buffer_proto))
        JS_SetPrototype(ctx, u8, qr->buffer_proto);
    return u8;
}

JSValue qb_new_buffer(JSContext *ctx, const uint8_t *data, size_t len)
{
    return qb_to_buffer(ctx, JS_NewUint8ArrayCopy(ctx, data, len));
}

static void qb_free_rt_buf(JSRuntime *rt, void *opaque, void *ptr)
{
    js_free_rt(rt, ptr);
}

JSValue qb_new_buffer_owned(JSContext *ctx, uint8_t *data, size_t len)
{
    return qb_to_buffer(ctx, JS_NewUint8Array(ctx, data, len, qb_free_rt_buf, NULL, FALSE));
}

int qb_set(JSContext *ctx, JSValueConst obj, const char *name, JSValue val)
{
    if (JS_IsException(val))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, name, val, JS_PROP_C_W_E);
}

/* -------------------------------------------------------- native modules */

static const struct {
    const char *name;
    QBModuleInit *init;
} qb_natives[] = {
    {"qbun:core", qb_init_core},     {"qbun:fs", qb_init_fs},
    {"qbun:os", qb_init_os},         {"qbun:process", qb_init_process},
    {"qbun:net", qb_init_net},       {"qbun:tls", qb_init_tls},
    {"qbun:crypto", qb_init_crypto}, {"qbun:zlib", qb_init_zlib},
    {"qbun:text", qb_init_text},     {"qbun:http", qb_init_http},
    {"qbun:sqlite", qb_init_sqlite}, {"qbun:ffi", qb_init_ffi},
    {"qbun:napi", qb_init_napi},     {"qbun:worker", qb_init_worker},
    {"qbun:async", qb_init_async},   {"qbun:misc", qb_init_misc},
    {"qbun:image", qb_init_image},
};

/* ------------------------------------------------------- helpers: files */

static int qb_stat_kind(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

/* the file contents with a terminating NUL (js_malloc), NULL if missing */
static uint8_t *qb_read_file(JSContext *ctx, const char *path, size_t *plen)
{
    FILE *f;
    uint8_t *buf;
    long n;
    size_t r;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        return NULL;
    }
    buf = js_malloc(ctx, n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    r = fread(buf, 1, n, f);
    fclose(f);
    buf[r] = '\0';
    *plen = r;
    return buf;
}

/* ------------------------------------------------------- loop + errors */

static JSValue qb_hook(QBRuntime *qr, const char *name)
{
    if (!JS_IsObject(qr->hooks))
        return JS_UNDEFINED;
    return JS_GetPropertyStr(qr->ctx, qr->hooks, name);
}

static void qb_print_error_fallback(JSContext *ctx, JSValueConst exc)
{
    const char *s;
    JSValue stack;

    s = JS_ToCString(ctx, exc);
    fprintf(stderr, "%s\n", s ? s : "[exception]");
    JS_FreeCString(ctx, s);
    if (JS_IsError(ctx, exc)) {
        stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(stack)) {
            s = JS_ToCString(ctx, stack);
            if (s)
                fputs(s, stderr);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, stack);
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

/* print 'exc' as the uncaught error that ends the process */
static void qb_print_fatal(QBRuntime *qr, JSValueConst exc)
{
    JSContext *ctx = qr->ctx;
    JSValue fn, ret;
    const char *s;

    fn = qb_hook(qr, "formatError");
    if (JS_IsFunction(ctx, fn)) {
        ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &exc);
        if (JS_IsString(ret)) {
            s = JS_ToCString(ctx, ret);
            if (s)
                fprintf(stderr, "%s\n", s);
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, fn);
            return;
        }
        if (JS_IsException(ret))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    qb_print_error_fallback(ctx, exc);
}

static void qb_run_cleanup_hooks(QBRuntime *qr);

static void qb_fatal_exit(QBRuntime *qr, int code)
{
    JSContext *ctx = qr->ctx;
    JSValue fn, ret, arg;

    if (qr->exiting)
        return;
    qr->exiting = 1;
    qr->exit_code = code;
    fn = qb_hook(qr, "emitExit");
    if (JS_IsFunction(ctx, fn)) {
        arg = JS_NewInt32(ctx, code);
        ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        if (JS_IsException(ret))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    if (qr->is_main_thread) {
        fflush(stdout);
        fflush(stderr);
        exit(code);
    }
    uv_stop(qr->loop);
}

void qb_report_exception(JSContext *ctx)
{
    QBRuntime *qr = qb_get(ctx);
    JSValue exc, fn, ret;
    int handled = 0;

    exc = JS_GetException(ctx);
    if (qr->exiting) {
        JS_FreeValue(ctx, exc);
        return;
    }
    fn = qb_hook(qr, "onUncaughtException");
    if (JS_IsFunction(ctx, fn)) {
        ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&exc);
        if (JS_IsException(ret)) {
            /* an error in the handler is fatal */
            JS_FreeValue(ctx, exc);
            exc = JS_GetException(ctx);
        } else {
            handled = JS_ToBool(ctx, ret);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    if (!handled && !qr->exiting) {
        qb_print_fatal(qr, exc);
        JS_FreeValue(ctx, exc);
        qb_fatal_exit(qr, 1);
        return;
    }
    JS_FreeValue(ctx, exc);
}

static void qb_rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                                 JS_BOOL is_handled, void *opaque)
{
    QBRuntime *qr = opaque;
    QBRejection *r, **pr;

    if (!is_handled) {
        r = malloc(sizeof(*r));
        if (!r)
            return;
        r->promise = JS_DupValue(ctx, promise);
        r->reason = JS_DupValue(ctx, reason);
        r->next = qr->rejections;
        qr->rejections = r;
    } else {
        for (pr = &qr->rejections; (r = *pr) != NULL; pr = &r->next) {
            if (JS_VALUE_GET_PTR(r->promise) == JS_VALUE_GET_PTR(promise)) {
                *pr = r->next;
                JS_FreeValue(ctx, r->promise);
                JS_FreeValue(ctx, r->reason);
                free(r);
                break;
            }
        }
    }
}

static void qb_check_rejections(QBRuntime *qr)
{
    JSContext *ctx = qr->ctx;
    QBRejection *list, *r, *rev = NULL;
    JSValue fn, ret, args[2];
    int handled;

    list = qr->rejections;
    qr->rejections = NULL;
    /* report in the order of the rejections */
    while (list) {
        r = list;
        list = r->next;
        r->next = rev;
        rev = r;
    }
    while (rev) {
        r = rev;
        rev = r->next;
        handled = 0;
        if (!qr->exiting) {
            fn = qb_hook(qr, "onUnhandledRejection");
            if (JS_IsFunction(ctx, fn)) {
                args[0] = r->reason;
                args[1] = r->promise;
                ret = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
                if (JS_IsException(ret)) {
                    JS_FreeValue(ctx, fn);
                    qb_report_exception(ctx);
                    handled = 1;
                    fn = JS_UNDEFINED;
                } else {
                    handled = JS_ToBool(ctx, ret);
                }
                JS_FreeValue(ctx, ret);
            }
            JS_FreeValue(ctx, fn);
            if (!handled && !qr->exiting) {
                qb_print_fatal(qr, r->reason);
                qb_fatal_exit(qr, 1);
            }
        }
        JS_FreeValue(ctx, r->promise);
        JS_FreeValue(ctx, r->reason);
        free(r);
    }
}

void qb_drain(QBRuntime *qr)
{
    JSContext *ctx1;
    int ret;

    for (;;) {
        if (qr->exiting)
            return;
        ret = JS_ExecutePendingJob(qr->rt, &ctx1);
        if (ret < 0) {
            qb_report_exception(ctx1 ? ctx1 : qr->ctx);
            continue;
        }
        if (ret == 0) {
            if (!qr->rejections)
                break;
            qb_check_rejections(qr);
        }
    }
}

JSValue qb_call(JSContext *ctx, JSValueConst func, JSValueConst this_obj, int argc,
                JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    JSValue ret, saved;

    if (qr->exiting)
        return JS_UNDEFINED;
    saved = JS_GetAsyncContext(qr->rt);
    qb_call_depth++;
    ret = JS_Call(ctx, func, this_obj, argc, argv);
    qb_call_depth--;
    /* a callback that entered an AsyncLocalStorage store does not leak it
       into the next callbacks (Node's InternalCallbackScope) */
    JS_SetAsyncContext(qr->rt, saved);
    if (JS_IsException(ret)) {
        qb_report_exception(ctx);
        ret = JS_UNDEFINED;
    }
    if (qb_call_depth == 0)
        qb_drain(qr);
    return ret;
}

/* call the hook 'name' from the loop */
static JSValue qb_call_hook(QBRuntime *qr, const char *name, int argc, JSValueConst *argv)
{
    JSValue fn, ret;
    fn = qb_hook(qr, name);
    if (!JS_IsFunction(qr->ctx, fn)) {
        JS_FreeValue(qr->ctx, fn);
        return JS_UNDEFINED;
    }
    ret = qb_call(qr->ctx, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(qr->ctx, fn);
    return ret;
}

/* ---------------------------------------------------------- module loader */

static int qb_is_builtin_name(const char *name)
{
    return !strncmp(name, "node:", 5) || !strncmp(name, "bun:", 4) || !strncmp(name, "qbun:", 5) ||
           !strncmp(name, "internal:", 9) || !strcmp(name, "bun");
}

static char *qb_normalize(JSContext *ctx, const char *base_name, const char *name,
                          JSValueConst attributes, void *opaque)
{
    QBRuntime *qr = opaque;
    JSValue fn, ret, args[3];
    const char *s;
    char *res;

    if (qb_is_builtin_name(name))
        return js_strdup(ctx, name);
    fn = qb_hook(qr, "resolve");
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        if (name[0] == '/')
            return js_strdup(ctx, name);
        JS_ThrowReferenceError(ctx, "cannot resolve '%s' before the bootstrap", name);
        return NULL;
    }
    args[0] = JS_NewString(ctx, name);
    args[1] = JS_NewString(ctx, base_name);
    args[2] = JS_NewString(ctx, "import");
    ret = JS_Call(ctx, fn, JS_UNDEFINED, 3, (JSValueConst *)args);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    if (JS_IsException(ret))
        return NULL;
    s = JS_ToCString(ctx, ret);
    JS_FreeValue(ctx, ret);
    if (!s)
        return NULL;
    res = js_strdup(ctx, s);
    JS_FreeCString(ctx, s);
    return res;
}

/* a module whose exports are 'default' = val and, when 'named', the own
   enumerable string keys of val (except "default") */
static int qb_value_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue priv, val, keys, k, v;
    const char *ks;
    uint32_t i, n;
    int64_t n64;

    priv = JS_GetModulePrivateValue(ctx, m);
    val = JS_GetPropertyUint32(ctx, priv, 0);
    keys = JS_GetPropertyUint32(ctx, priv, 1);
    if (JS_IsArray(ctx, keys)) {
        JS_ToInt64(ctx, &n64, JS_GetPropertyStr(ctx, keys, "length"));
        n = (uint32_t)n64;
        for (i = 0; i < n; i++) {
            k = JS_GetPropertyUint32(ctx, keys, i);
            ks = JS_ToCString(ctx, k);
            JS_FreeValue(ctx, k);
            if (!ks)
                goto fail;
            if (!strcmp(ks, "default")) {
                JS_FreeCString(ctx, ks);
                continue;
            }
            v = JS_GetPropertyStr(ctx, val, ks);
            if (JS_IsException(v)) {
                JS_FreeCString(ctx, ks);
                goto fail;
            }
            JS_SetModuleExport(ctx, m, ks, v);
            JS_FreeCString(ctx, ks);
        }
    }
    v = JS_GetPropertyUint32(ctx, priv, 2); /* explicit default */
    if (JS_IsUndefined(v)) {
        JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, val));
    } else {
        JS_FreeValue(ctx, v);
        JS_SetModuleExport(ctx, m, "default", JS_GetPropertyStr(ctx, val, "default"));
    }
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, priv);
    return 0;
fail:
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, priv);
    return -1;
}

/* 'named': export the own enumerable keys of val; 'es_default': the
   default export is val.default (CommonJS with __esModule) */
static JSModuleDef *qb_new_value_module(JSContext *ctx, const char *name, JSValue val, int named,
                                        int es_default)
{
    JSModuleDef *m;
    JSValue priv, keys = JS_UNDEFINED, k;
    const char *ks;
    uint32_t i, n;
    int64_t n64;

    m = JS_NewCModule(ctx, name, qb_value_module_init);
    if (!m) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    JS_AddModuleExport(ctx, m, "default");
    if (named && (JS_IsObject(val))) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue object = JS_GetPropertyStr(ctx, global, "Object");
        JSValue fkeys = JS_GetPropertyStr(ctx, object, "keys");
        keys = JS_Call(ctx, fkeys, object, 1, (JSValueConst *)&val);
        JS_FreeValue(ctx, fkeys);
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, global);
        if (JS_IsException(keys)) {
            JS_FreeValue(ctx, val);
            return NULL;
        }
        JS_ToInt64(ctx, &n64, JS_GetPropertyStr(ctx, keys, "length"));
        n = (uint32_t)n64;
        for (i = 0; i < n; i++) {
            k = JS_GetPropertyUint32(ctx, keys, i);
            ks = JS_ToCString(ctx, k);
            JS_FreeValue(ctx, k);
            if (!ks)
                break;
            if (strcmp(ks, "default"))
                JS_AddModuleExport(ctx, m, ks);
            JS_FreeCString(ctx, ks);
        }
    }
    priv = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, priv, 0, val);
    JS_SetPropertyUint32(ctx, priv, 1, keys);
    if (es_default)
        JS_SetPropertyUint32(ctx, priv, 2, JS_TRUE);
    JS_SetModulePrivateValue(ctx, m, priv);
    return m;
}

static int qb_ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !memcmp(s + a - b, suffix, b);
}

static const char *qb_ext(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash))
        return "";
    return dot;
}

static int qb_is_ts(const char *path)
{
    return qb_ends_with(path, ".ts") || qb_ends_with(path, ".mts") || qb_ends_with(path, ".cts");
}

/* file:// URL of an absolute path (percent-encoding the characters that
   cannot appear in a URL path) */
static char *qb_path_to_url(JSContext *ctx, const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(path), i, j;
    char *s = js_malloc(ctx, 7 + len * 3 + 1);
    unsigned char c;

    if (!s)
        return NULL;
    memcpy(s, "file://", 7);
    j = 7;
    for (i = 0; i < len; i++) {
        c = path[i];
        if (c <= 0x20 || c == '%' || c == '#' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}' || c >= 0x7f) {
            s[j++] = '%';
            s[j++] = hex[c >> 4];
            s[j++] = hex[c & 15];
        } else {
            s[j++] = c;
        }
    }
    s[j] = '\0';
    return s;
}

static JSValue qb_meta_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    QBRuntime *qr = qb_get(ctx);
    JSValue fn, ret, args[3];

    fn = qb_hook(qr, magic ? "requireFrom" : "metaResolve");
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return JS_ThrowTypeError(ctx, "the module loader is not ready");
    }
    args[0] = data[0];
    args[1] = argc > 0 ? argv[0] : JS_UNDEFINED;
    args[2] = argc > 1 ? argv[1] : JS_UNDEFINED;
    ret = JS_Call(ctx, fn, JS_UNDEFINED, 3, (JSValueConst *)args);
    JS_FreeValue(ctx, fn);
    return ret;
}

/* import.meta of the module 'name': a file path, maybe followed by a ?query
   (cache busting: url keeps it, path/dir/file do not, like Bun), or the name
   of a plugin module (url = path = name) */
static int qb_set_import_meta(JSContext *ctx, JSModuleDef *m, const char *name)
{
    QBRuntime *qr = qb_get(ctx);
    JSValue meta, pathv, fn, global, process;
    const char *slash = NULL, *query = NULL, *path = name;
    char *url = NULL, *path_alloc = NULL, *url2;
    size_t ulen, qlen;
    int is_main;

    meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta))
        return -1;
    if (name[0] == '/') {
        query = strchr(name, '?');
        if (query) {
            path_alloc = js_strndup(ctx, name, query - name);
            if (!path_alloc)
                goto fail;
            path = path_alloc;
        }
        url = qb_path_to_url(ctx, path);
        if (!url)
            goto fail;
        if (query) {
            ulen = strlen(url);
            qlen = strlen(query);
            url2 = js_realloc(ctx, url, ulen + qlen + 1);
            if (!url2)
                goto fail;
            url = url2;
            memcpy(url + ulen, query, qlen + 1);
        }
        slash = strrchr(path, '/');
    }
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, url ? url : name), JS_PROP_C_W_E);
    js_free(ctx, url);
    url = NULL;
    is_main = qr->main_path && !strcmp(qr->main_path, name);
    JS_DefinePropertyValueStr(ctx, meta, "main", JS_NewBool(ctx, is_main), JS_PROP_C_W_E);
    pathv = JS_NewString(ctx, path);
    JS_DefinePropertyValueStr(ctx, meta, "path", JS_DupValue(ctx, pathv), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "filename", JS_DupValue(ctx, pathv), JS_PROP_C_W_E);
    if (slash) {
        JSValue dir = JS_NewStringLen(ctx, path, slash == path ? 1 : (size_t)(slash - path));
        JS_DefinePropertyValueStr(ctx, meta, "dir", JS_DupValue(ctx, dir), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, meta, "dirname", dir, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, meta, "file", JS_NewString(ctx, slash + 1), JS_PROP_C_W_E);
    }
    fn = JS_NewCFunctionData(ctx, qb_meta_resolve, 1, 0, 1, (JSValueConst *)&pathv);
    JS_DefinePropertyValueStr(ctx, meta, "resolve", fn, JS_PROP_C_W_E);
    fn = JS_NewCFunctionData(ctx, qb_meta_resolve, 1, 1, 1, (JSValueConst *)&pathv);
    JS_DefinePropertyValueStr(ctx, meta, "require", fn, JS_PROP_C_W_E);
    JS_FreeValue(ctx, pathv);
    global = JS_GetGlobalObject(ctx);
    process = JS_GetPropertyStr(ctx, global, "process");
    if (JS_IsObject(process))
        JS_DefinePropertyValueStr(ctx, meta, "env", JS_GetPropertyStr(ctx, process, "env"),
                                  JS_PROP_C_W_E);
    JS_FreeValue(ctx, process);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, meta);
    js_free(ctx, path_alloc);
    return 0;
fail:
    js_free(ctx, url);
    js_free(ctx, path_alloc);
    JS_FreeValue(ctx, meta);
    return -1;
}

static int qb_ts_flags(const char *path)
{
    return qb_is_ts(path) ? JS_EVAL_FLAG_TYPESCRIPT : 0;
}

/* ------------------------------------------------------- bytecode cache */

/* Compiled modules are cached as bytecode in $QBUN_CACHE_DIR (default
   $XDG_CACHE_HOME/qbun or ~/.cache/qbun), one file per (name, flags,
   source) hash; QBUN_NO_CACHE=1 disables it. A file starts with
   QB_CACHE_MAGIC and the build stamp, so another qbun build recompiles. */
#define QB_CACHE_MAGIC "QBUNBC1\n"
static const char qb_cache_stamp[] = __DATE__ " " __TIME__;

static uint64_t qb_fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static char qb_cache_dir_buf[PATH_MAX];
static int qb_cache_state; /* 1 on, -1 off; set once by qb_cache_init() */
static pthread_once_t qb_cache_once = PTHREAD_ONCE_INIT;

static void qb_cache_init(void)
{
    char *dir = qb_cache_dir_buf;
    const char *env, *home;
    char *p;

    qb_cache_state = -1;
    env = getenv("QBUN_NO_CACHE");
    if (env && env[0] && strcmp(env, "0"))
        return;
    if ((env = getenv("QBUN_CACHE_DIR")) && env[0]) {
        snprintf(dir, PATH_MAX, "%s", env);
    } else if ((env = getenv("XDG_CACHE_HOME")) && env[0]) {
        snprintf(dir, PATH_MAX, "%s/qbun", env);
    } else if ((home = getenv("HOME")) && home[0]) {
        snprintf(dir, PATH_MAX, "%s/.cache/qbun", home);
    } else {
        return;
    }
    for (p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return;
    qb_cache_state = 1;
}

/* the cache directory (created), or NULL when caching is off */
static const char *qb_cache_dir(void)
{
    pthread_once(&qb_cache_once, qb_cache_init);
    return qb_cache_state > 0 ? qb_cache_dir_buf : NULL;
}

static void qb_cache_path(char *buf, size_t size, const char *dir, const char *name,
                          const char *src, size_t len, int flags)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    h = qb_fnv1a(h, name, strlen(name) + 1);
    h = qb_fnv1a(h, &flags, sizeof(flags));
    h = qb_fnv1a(h, src, len);
    snprintf(buf, size, "%s/%016llx.qbc", dir, (unsigned long long)h);
}

/* the cached module, JS_UNDEFINED when absent or stale */
static JSValue qb_cache_read(JSContext *ctx, const char *path)
{
    size_t len, hdr = sizeof(QB_CACHE_MAGIC) - 1 + sizeof(qb_cache_stamp);
    uint8_t *buf;
    JSValue val;

    buf = qb_read_file(ctx, path, &len);
    if (!buf)
        return JS_UNDEFINED;
    if (len <= hdr || memcmp(buf, QB_CACHE_MAGIC, sizeof(QB_CACHE_MAGIC) - 1) ||
        memcmp(buf + sizeof(QB_CACHE_MAGIC) - 1, qb_cache_stamp, sizeof(qb_cache_stamp))) {
        js_free(ctx, buf);
        return JS_UNDEFINED;
    }
    val = JS_ReadObject(ctx, buf + hdr, len - hdr, JS_READ_OBJ_BYTECODE);
    js_free(ctx, buf);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    if (JS_VALUE_GET_TAG(val) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, val);
        return JS_UNDEFINED;
    }
    return val;
}

static void qb_cache_write(JSContext *ctx, const char *path, JSValueConst val)
{
    char tmp[PATH_MAX + 32];
    uint8_t *data;
    size_t len;
    FILE *f;
    int ok;

    data = JS_WriteObject(ctx, &len, val, JS_WRITE_OBJ_BYTECODE);
    if (!data) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(QB_CACHE_MAGIC, 1, sizeof(QB_CACHE_MAGIC) - 1, f) ==
                 sizeof(QB_CACHE_MAGIC) - 1 &&
             fwrite(qb_cache_stamp, 1, sizeof(qb_cache_stamp), f) == sizeof(qb_cache_stamp) &&
             fwrite(data, 1, len, f) == len;
        if (fclose(f) != 0)
            ok = 0;
        if (!ok || rename(tmp, path) != 0)
            unlink(tmp);
    }
    js_free(ctx, data);
}

/* compile 'src' as the ES module 'name' (through the bytecode cache) */
static JSModuleDef *qb_compile_module(JSContext *ctx, const char *name, const char *src, size_t len,
                                      int flags, int set_meta)
{
    JSValue val;
    JSModuleDef *m;
    const char *dir = qb_cache_dir();
    char cpath[PATH_MAX + 64];

    val = JS_UNDEFINED;
    if (dir) {
        qb_cache_path(cpath, sizeof(cpath), dir, name, src, len, flags);
        val = qb_cache_read(ctx, cpath);
    }
    if (JS_IsUndefined(val)) {
        val = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | flags);
        if (JS_IsException(val))
            return NULL;
        if (dir)
            qb_cache_write(ctx, cpath, val);
    }
    m = JS_VALUE_GET_PTR(val);
    if (set_meta && qb_set_import_meta(ctx, m, name) < 0) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    JS_FreeValue(ctx, val);
    return m;
}

/* the path of the library source of the builtin 'name' */
static int qb_lib_path(QBRuntime *qr, const char *name, char *buf, size_t size)
{
    const char *colon;
    if (!strcmp(name, "bun"))
        return snprintf(buf, size, "%s/bun/index.ts", qr->lib_dir) >= (int)size ? -1 : 0;
    colon = strchr(name, ':');
    if (!colon)
        return -1;
    return snprintf(buf, size, "%s/%.*s/%s.ts", qr->lib_dir, (int)(colon - name), name,
                    colon + 1) >= (int)size
               ? -1
               : 0;
}

static int qb_is_ident_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '$';
}

/* an import or export statement at the start of a line, or import.meta
   (a heuristic: Bun decides with its parser) */
static int qb_has_esm_syntax(const char *src, size_t len)
{
    const char *p = src, *end = src + len, *q;
    int bol = 1;

    if (strstr(src, "import.meta"))
        return 1;
    while (p < end) {
        if (bol) {
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            q = p;
            if (end - q > 7 && !memcmp(q, "export", 6) && !qb_is_ident_char(q[6]))
                return 1;
            if (end - q > 7 && !memcmp(q, "import", 6) && !qb_is_ident_char(q[6])) {
                q += 6;
                while (q < end && (*q == ' ' || *q == '\t'))
                    q++;
                if (q < end && *q != '(' && *q != '.' && *q != '=' && *q != ':')
                    return 1;
            }
            bol = 0;
        }
        if (*p == '\n')
            bol = 1;
        p++;
    }
    return 0;
}

/* CommonJS: the source looks like a CommonJS module (no import/export
   statement, uses require/module/exports) */
static int qb_is_commonjs(const char *path, const char *src, size_t len)
{
    if (qb_ends_with(path, ".cjs") || qb_ends_with(path, ".cts"))
        return 1;
    if (qb_ends_with(path, ".mjs") || qb_ends_with(path, ".mts"))
        return 0;
    if (JS_DetectModule(src, len) || qb_has_esm_syntax(src, len))
        return 0;
    return strstr(src, "require(") || strstr(src, "module.exports") || strstr(src, "exports.") ||
           strstr(src, "exports[") || strstr(src, "__esModule");
}

static JSValue qb_call_hook_sync(QBRuntime *qr, const char *name, int argc, JSValueConst *argv)
{
    JSValue fn = qb_hook(qr, name), ret;
    if (!JS_IsFunction(qr->ctx, fn)) {
        JS_FreeValue(qr->ctx, fn);
        return JS_ThrowReferenceError(qr->ctx, "the '%s' loader hook is missing", name);
    }
    ret = JS_Call(qr->ctx, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(qr->ctx, fn);
    return ret;
}

/* the module 'name' holding the value of the data 'buf' (js_malloc, freed)
   of the file 'path' read with the type 'type' */
static JSModuleDef *qb_load_data(JSContext *ctx, QBRuntime *qr, const char *name, const char *path,
                                 const char *type, uint8_t *buf, size_t len, int named)
{
    JSValue val, args[3];

    if (!strcmp(type, "json")) {
        val = JS_ParseJSON2(ctx, (char *)buf, len, path, 0);
    } else if (!strcmp(type, "json5") || !strcmp(type, "jsonc")) {
        val = JS_ParseJSON2(ctx, (char *)buf, len, path, JS_PARSE_JSON_EXT);
    } else if (!strcmp(type, "text")) {
        val = JS_NewStringLen(ctx, (char *)buf, len);
        named = 0;
    } else if (!strcmp(type, "bytes")) {
        val = JS_NewUint8ArrayCopy(ctx, buf, len);
        named = 0;
    } else if (!strcmp(type, "file")) {
        val = JS_NewString(ctx, path);
        named = 0;
    } else {
        /* toml, yaml...: parsed by the library */
        args[0] = JS_NewString(ctx, type);
        args[1] = JS_NewStringLen(ctx, (char *)buf, len);
        args[2] = JS_NewString(ctx, path);
        val = qb_call_hook_sync(qr, "parseData", 3, (JSValueConst *)args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
    }
    js_free(ctx, buf);
    if (JS_IsException(val))
        return NULL;
    return qb_new_value_module(ctx, name, val, named, 0);
}

static const char *qb_type_of_ext(const char *ext)
{
    static const char *const map[][2] = {
        {".json", "json"}, {".jsonc", "jsonc"}, {".json5", "json5"}, {".toml", "toml"},
        {".yaml", "yaml"}, {".yml", "yaml"},    {".txt", "text"},    {".jsonl", "jsonl"},
    };
    size_t i;
    for (i = 0; i < countof(map); i++) {
        if (!strcmp(ext, map[i][0]))
            return map[i][1];
    }
    return NULL;
}

static int qb_is_code_ext(const char *ext)
{
    static const char *const exts[] = {
        ".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".jsx", ".tsx", "",
    };
    size_t i;
    for (i = 0; i < countof(exts); i++) {
        if (!strcmp(ext, exts[i]))
            return 1;
    }
    return 0;
}

/* the CommonJS module 'name' as an ES module: default = module.exports, the
   named exports are its keys (default = exports.default with __esModule).
   'src' (run with 'loader', js or ts): the contents given by a plugin
   onLoad callback; NULL: the file itself */
static JSModuleDef *qb_load_commonjs(JSContext *ctx, QBRuntime *qr, const char *name,
                                     const char *src, size_t src_len, const char *loader)
{
    JSValue args[3], val, flag;
    int es = 0;

    args[0] = JS_NewString(ctx, name);
    /* null: the plugins were consulted already */
    args[1] = src ? JS_NewStringLen(ctx, src, src_len) : JS_NULL;
    args[2] = loader ? JS_NewString(ctx, loader) : JS_UNDEFINED;
    val = qb_call_hook_sync(qr, "loadCommonJS", 3, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    if (JS_IsException(val))
        return NULL;
    if (JS_IsObject(val)) {
        flag = JS_GetPropertyStr(ctx, val, "__esModule");
        es = JS_ToBool(ctx, flag);
        JS_FreeValue(ctx, flag);
    }
    return qb_new_value_module(ctx, name, val, 1, es);
}

/* the N-API addon 'name' as an ES module: the exports of require() */
static JSModuleDef *qb_load_napi(JSContext *ctx, QBRuntime *qr, const char *name)
{
    JSValue args[2], val;

    args[0] = JS_NewString(ctx, name);
    args[1] = JS_NULL;
    val = qb_call_hook_sync(qr, "loadCommonJS", 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    if (JS_IsException(val))
        return NULL;
    return qb_new_value_module(ctx, name, val, 1, 0);
}

/* the module 'name' (file 'path') of the code 'buf' (js_malloc, freed) read
   with 'loader' (js, ts, jsx or tsx). 'plugin': the code comes from a plugin
   onLoad callback, so a CommonJS module runs it rather than the file. */
static JSModuleDef *qb_load_code(JSContext *ctx, QBRuntime *qr, const char *name, const char *path,
                                 const char *loader, uint8_t *buf, size_t len, int plugin)
{
    JSModuleDef *m;
    JSValue args[2], val;
    const char *src, *js = NULL, *ext, *cjs_loader = loader;
    char *tname = NULL;
    size_t src_len, len_t;
    int flags = 0;

    if (!strcmp(loader, "jsx") || !strcmp(loader, "tsx")) {
        /* JSX: Bun.Transpiler turns it into a JS module (its loader comes
           from the extension of the file name) */
        ext = qb_ext(path);
        if (!ext[0] || strcmp(ext + 1, loader)) {
            len_t = strlen(path) + strlen(loader) + 2;
            tname = js_malloc(ctx, len_t);
            if (!tname) {
                js_free(ctx, buf);
                return NULL;
            }
            snprintf(tname, len_t, "%s.%s", path, loader);
        }
        args[0] = JS_NewStringLen(ctx, (char *)buf, len);
        args[1] = JS_NewString(ctx, tname ? tname : path);
        js_free(ctx, buf);
        js_free(ctx, tname);
        val = qb_call_hook_sync(qr, "transpileForLoad", 2, (JSValueConst *)args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(val))
            return NULL;
        js = JS_ToCStringLen(ctx, &src_len, val);
        JS_FreeValue(ctx, val);
        if (!js)
            return NULL;
        src = js;
        cjs_loader = "js";
    } else {
        src = (const char *)buf;
        src_len = len;
        if (!strcmp(loader, "ts"))
            flags = JS_EVAL_FLAG_TYPESCRIPT;
    }
    if (qb_is_commonjs(path, src, src_len)) {
        /* a file: the CommonJS loader reads (and transpiles) it again
           (Module._extensions) */
        if (plugin)
            m = qb_load_commonjs(ctx, qr, name, src, src_len, cjs_loader);
        else
            m = qb_load_commonjs(ctx, qr, name, NULL, 0, NULL);
    } else {
        m = qb_compile_module(ctx, name, src, src_len, flags, 1);
    }
    if (js)
        JS_FreeCString(ctx, js);
    else
        js_free(ctx, buf);
    return m;
}

/* the fulfilled value of 'promise' (freed), waiting for it inside the
   synchronous module loader: runs the jobs and the libuv loop until it
   settles (nested waits are fine: each one loops until its own promise
   settles). Throws its rejection reason; throws when the loop has nothing
   left to run or the runtime is exiting. */
static JSValue qb_await(QBRuntime *qr, JSValue promise)
{
    JSContext *ctx = qr->ctx;
    JSValue ret;

    if (!JS_IsPromise(promise))
        return promise;
    /* its rejection is reported here, not as an unhandled rejection */
    JS_PromiseMarkAsHandled(ctx, promise);
    for (;;) {
        switch ((int)JS_PromiseState(ctx, promise)) {
        case JS_PROMISE_FULFILLED:
            ret = JS_PromiseResult(ctx, promise);
            JS_FreeValue(ctx, promise);
            return ret;
        case JS_PROMISE_REJECTED:
            ret = JS_PromiseResult(ctx, promise);
            JS_FreeValue(ctx, promise);
            return JS_Throw(ctx, ret);
        default:
            break;
        }
        if (qr->exiting) {
            JS_FreeValue(ctx, promise);
            /* a nested uv_run() resets the stop flag of the outer one */
            uv_stop(qr->loop);
            JS_ThrowInternalError(ctx, "the runtime is exiting");
            JS_SetUncatchableException(ctx, TRUE);
            return JS_EXCEPTION;
        }
        if (JS_IsJobPending(qr->rt)) {
            qb_drain(qr);
            continue;
        }
        if (!uv_run(qr->loop, UV_RUN_ONCE) && !JS_IsJobPending(qr->rt) && !qr->exiting &&
            JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
            JS_FreeValue(ctx, promise);
            return JS_ThrowTypeError(ctx, "a plugin promise never settled: nothing is left to run");
        }
    }
}

/* the module 'name' (file 'path') from the Bun.plugin() onLoad callbacks
   (the "pluginLoad" hook 'fn'): *done = 0 when no plugin answered */
static JSModuleDef *qb_load_plugin(JSContext *ctx, QBRuntime *qr, JSValueConst fn, const char *name,
                                   const char *path, const char *type, int *done)
{
    JSValue args[2], res, v, contents;
    const char *s;
    const uint8_t *p;
    uint8_t *buf;
    char loader[16];
    size_t len;
    int es;

    *done = 1;
    args[0] = JS_NewString(ctx, name);
    args[1] = type ? JS_NewString(ctx, type) : JS_UNDEFINED;
    res = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(res))
        return NULL;
    if (JS_IsPromise(res)) {
        res = qb_await(qr, res);
        if (JS_IsException(res))
            return NULL;
    }
    if (JS_IsUndefined(res) || JS_IsNull(res)) {
        *done = 0;
        return NULL;
    }
    /* { exports, esDefault }: the "object" loader */
    v = JS_GetPropertyStr(ctx, res, "exports");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, res);
        return NULL;
    }
    if (!JS_IsUndefined(v)) {
        es = JS_ToBool(ctx, JS_GetPropertyStr(ctx, res, "esDefault"));
        JS_FreeValue(ctx, res);
        return qb_new_value_module(ctx, name, v, 1, es);
    }
    /* { contents: string | bytes, loader } */
    v = JS_GetPropertyStr(ctx, res, "loader");
    contents = JS_GetPropertyStr(ctx, res, "contents");
    JS_FreeValue(ctx, res);
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        JS_FreeValue(ctx, contents);
        return NULL;
    }
    pstrcpy(loader, sizeof(loader), s);
    JS_FreeCString(ctx, s);
    if (JS_IsString(contents)) {
        s = JS_ToCStringLen(ctx, &len, contents);
        p = (const uint8_t *)s;
    } else {
        s = NULL;
        p = qb_get_bytes(ctx, contents, &len);
    }
    if (!p) {
        JS_FreeValue(ctx, contents);
        return NULL;
    }
    buf = js_malloc(ctx, len + 1);
    if (buf) {
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, contents);
    if (!buf)
        return NULL;
    if (!strcmp(loader, "js") || !strcmp(loader, "ts") || !strcmp(loader, "jsx") ||
        !strcmp(loader, "tsx"))
        return qb_load_code(ctx, qr, name, path, loader, buf, len, 1);
    if (!strcmp(loader, "napi")) {
        js_free(ctx, buf);
        return qb_load_napi(ctx, qr, name);
    }
    /* json, toml, text, file...: Bun gives data modules named exports */
    return qb_load_data(ctx, qr, name, path, loader, buf, len, 1);
}

/* the module 'name' read from the file 'path' */
static JSModuleDef *qb_load_file(JSContext *ctx, QBRuntime *qr, const char *name, const char *path,
                                 const char *type)
{
    uint8_t *buf;
    size_t len;
    const char *ext, *loader;

    ext = qb_ext(path);
    if (!type && !strcmp(ext, ".node"))
        return qb_load_napi(ctx, qr, name);
    buf = qb_read_file(ctx, path, &len);
    if (!buf) {
        qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_MODULE_NOT_FOUND", "Cannot find module '%s'", name);
        return NULL;
    }
    /* Bun gives data modules named exports too:
       import { version } from "./package.json" with { type: "json" } */
    if (type && strcmp(type, "js") && strcmp(type, "javascript"))
        return qb_load_data(ctx, qr, name, path, type, buf, len, 1);
    if (!qb_is_code_ext(ext)) {
        type = qb_type_of_ext(ext);
        /* Bun's "file" loader: the default export is the path */
        return qb_load_data(ctx, qr, name, path, type ? type : "file", buf, len,
                            type && strcmp(type, "text"));
    }
    if (!strcmp(ext, ".tsx"))
        loader = "tsx";
    else if (!strcmp(ext, ".jsx"))
        loader = "jsx";
    else if (qb_is_ts(path))
        loader = "ts";
    else
        loader = "js";
    return qb_load_code(ctx, qr, name, path, loader, buf, len, 0);
}

static JSModuleDef *qb_load(JSContext *ctx, const char *name, void *opaque, JSValueConst attributes)
{
    QBRuntime *qr = opaque;
    char path_buf[PATH_MAX + 64];
    uint8_t *buf;
    size_t len, i;
    JSModuleDef *m = NULL;
    JSValue tv, fn;
    const char *type = NULL, *path = name, *query;
    char type_buf[32], *path_alloc = NULL;
    int done = 0;

    if (!strncmp(name, "qbun:", 5)) {
        for (i = 0; i < countof(qb_natives); i++) {
            if (!strcmp(qb_natives[i].name, name))
                return qb_natives[i].init(ctx, name);
        }
        JS_ThrowReferenceError(ctx, "No such built-in module: %s", name);
        return NULL;
    }
    if (qb_is_builtin_name(name)) {
        if (qb_lib_path(qr, name, path_buf, sizeof(path_buf)) < 0 ||
            !(buf = qb_read_file(ctx, path_buf, &len))) {
            qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_UNKNOWN_BUILTIN_MODULE",
                          "No such built-in module: %s", name);
            return NULL;
        }
        m = qb_compile_module(ctx, name, (char *)buf, len, JS_EVAL_FLAG_TYPESCRIPT, 0);
        js_free(ctx, buf);
        return m;
    }

    if (JS_IsObject(attributes)) {
        tv = JS_GetPropertyStr(ctx, attributes, "type");
        if (JS_IsString(tv)) {
            const char *s = JS_ToCString(ctx, tv);
            if (s) {
                pstrcpy(type_buf, sizeof(type_buf), s);
                type = type_buf;
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, tv);
    }
    /* "/a/b.ts?mtime=1": a distinct module instance of the file /a/b.ts
       (cache busting, like Bun) */
    if (name[0] == '/' && (query = strchr(name, '?'))) {
        path_alloc = js_strndup(ctx, name, query - name);
        if (!path_alloc)
            return NULL;
        path = path_alloc;
    }
    /* Bun.plugin(): the hook exists only while plugins are registered */
    fn = qb_hook(qr, "pluginLoad");
    if (JS_IsFunction(ctx, fn))
        m = qb_load_plugin(ctx, qr, fn, name, path, type, &done);
    JS_FreeValue(ctx, fn);
    if (!done)
        m = qb_load_file(ctx, qr, name, path, type);
    js_free(ctx, path_alloc);
    return m;
}

static int qb_check_attributes(JSContext *ctx, void *opaque, JSValueConst attributes)
{
    return 0;
}

/* --------------------------------------------------------- qbun:core API */

static JSValue qb_core_setHooks(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    JS_FreeValue(ctx, qr->hooks);
    qr->hooks = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue qb_core_setBufferPrototype(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    JS_FreeValue(ctx, qr->buffer_proto);
    qr->buffer_proto = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* loadModuleSync(specifier, base, attributes?): the namespace */
static JSValue qb_core_loadModuleSync(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    const char *spec, *base = NULL;
    JSValue ret;

    spec = JS_ToCString(ctx, argv[0]);
    if (!spec)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsString(argv[1])) {
        base = JS_ToCString(ctx, argv[1]);
        if (!base) {
            JS_FreeCString(ctx, spec);
            return JS_EXCEPTION;
        }
    }
    ret = JS_LoadModuleSync(ctx, base ? base : "", spec, argc > 2 ? argv[2] : JS_UNDEFINED);
    JS_FreeCString(ctx, spec);
    if (base)
        JS_FreeCString(ctx, base);
    return ret;
}

/* compileFunction(src, filename, params): the function
   `function (params) { src }` compiled as a script of 'filename'
   (TypeScript for .ts/.cts/.mts) */
static JSValue qb_core_compileFunction(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    const char *src = NULL, *filename = NULL, *params = NULL;
    size_t src_len, params_len;
    DynBuf db;
    JSValue ret = JS_EXCEPTION;
    int flags;

    src = JS_ToCStringLen(ctx, &src_len, argv[0]);
    filename = JS_ToCString(ctx, argv[1]);
    params = JS_ToCStringLen(ctx, &params_len, argv[2]);
    if (!src || !filename || !params)
        goto done;
    dbuf_init(&db);
    dbuf_putstr(&db, "(function (");
    dbuf_put(&db, (const uint8_t *)params, params_len);
    dbuf_putstr(&db, ") {");
    if (src_len >= 2 && src[0] == '#' && src[1] == '!') {
        dbuf_putstr(&db, "//");
        dbuf_put(&db, (const uint8_t *)src + 2, src_len - 2);
    } else {
        dbuf_put(&db, (const uint8_t *)src, src_len);
    }
    dbuf_putstr(&db, "\n})");
    dbuf_putc(&db, '\0');
    if (db.error) {
        dbuf_free(&db);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    flags = JS_EVAL_TYPE_GLOBAL | qb_ts_flags(filename);
    if (argc > 3 && JS_ToBool(ctx, argv[3]))
        flags |= JS_EVAL_FLAG_TYPESCRIPT;
    ret = JS_Eval(ctx, (char *)db.buf, db.size - 1, filename, flags);
    dbuf_free(&db);
done:
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, params);
    return ret;
}

/* runScript(src, filename, ts?): evaluate a global script */
static JSValue qb_core_runScript(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    const char *src, *filename;
    size_t len;
    JSValue ret;
    int flags = JS_EVAL_TYPE_GLOBAL;

    src = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    filename = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (argc > 2 && JS_ToBool(ctx, argv[2]))
        flags |= JS_EVAL_FLAG_TYPESCRIPT;
    ret = JS_Eval(ctx, src, len, filename ? filename : "[eval]", flags);
    JS_FreeCString(ctx, src);
    if (filename)
        JS_FreeCString(ctx, filename);
    return ret;
}

static JSValue qb_core_detectModule(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    const char *src, *path = "";
    size_t len;
    int ret;

    src = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsString(argv[1]))
        path = JS_ToCString(ctx, argv[1]);
    ret = !qb_is_commonjs(path ? path : "", src, len);
    JS_FreeCString(ctx, src);
    if (argc > 1 && JS_IsString(argv[1]) && path)
        JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ret);
}

/* statKind(path): 0 missing, 1 file, 2 directory */
static JSValue qb_core_statKind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *path = JS_ToCString(ctx, argv[0]);
    int k;
    if (!path)
        return JS_EXCEPTION;
    k = qb_stat_kind(path);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, k);
}

static JSValue qb_core_realpath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *path = JS_ToCString(ctx, argv[0]);
    char buf[PATH_MAX];
    JSValue ret;
    if (!path)
        return JS_EXCEPTION;
    if (realpath(path, buf))
        ret = JS_NewString(ctx, buf);
    else
        ret = JS_NULL;
    JS_FreeCString(ctx, path);
    return ret;
}

/* readText(path): the UTF-8 contents, undefined if it cannot be read */
static JSValue qb_core_readText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *path = JS_ToCString(ctx, argv[0]);
    uint8_t *buf;
    size_t len, off = 0;
    JSValue ret;

    if (!path)
        return JS_EXCEPTION;
    buf = qb_read_file(ctx, path, &len);
    JS_FreeCString(ctx, path);
    if (!buf)
        return JS_UNDEFINED;
    if (len >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf)
        off = 3;
    ret = JS_NewStringLen(ctx, (char *)buf + off, len - off);
    js_free(ctx, buf);
    return ret;
}

static int qb_write_all(int fd, const uint8_t *p, size_t len)
{
    ssize_t n;
    struct pollfd pfd;

    while (len > 0) {
        n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                pfd.fd = fd;
                pfd.events = POLLOUT;
                poll(&pfd, 1, -1);
                continue;
            }
            return -errno;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* writeSync(fd, string | bytes): blocking write of everything */
static JSValue qb_core_writeSync(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    int fd, err;
    const uint8_t *p;
    const char *s = NULL;
    size_t len;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        s = JS_ToCStringLen(ctx, &len, argv[1]);
        if (!s)
            return JS_EXCEPTION;
        p = (const uint8_t *)s;
    } else {
        p = qb_get_bytes(ctx, argv[1], &len);
        if (!p)
            return JS_EXCEPTION;
    }
    err = qb_write_all(fd, p, len);
    if (s)
        JS_FreeCString(ctx, s);
    if (err < 0)
        return qb_throw_uv(ctx, err, "write", NULL, NULL);
    return JS_NewInt64(ctx, len);
}

static JSValue qb_core_hrtime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewBigUint64(ctx, uv_hrtime());
}

/* startTime(): uv_hrtime() (ns, bigint) when the runtime was created */
static JSValue qb_core_startTime(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    return JS_NewBigUint64(ctx, qb_get(ctx)->start_time);
}

/* drainMicrotasks(): run the pending jobs now (bun:jsc) */
static JSValue qb_core_drainMicrotasks(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    JSContext *ctx1;
    int ret;
    for (;;) {
        ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1);
        if (ret == 0)
            break;
        if (ret < 0)
            qb_report_exception(ctx1 ? ctx1 : ctx);
    }
    return JS_UNDEFINED;
}

static JSValue qb_core_reallyExit(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    int code = 0;
    if (argc > 0)
        JS_ToInt32(ctx, &code, argv[0]);
    qr->exit_code = code;
    if (qr->is_main_thread) {
        fflush(stdout);
        fflush(stderr);
        exit(code);
    }
    qr->exiting = 1;
    uv_stop(qr->loop);
    JS_ThrowInternalError(ctx, "worker exited");
    JS_SetUncatchableException(ctx, TRUE);
    return JS_EXCEPTION;
}

static JSValue qb_core_environ(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue obj = JS_NewObject(ctx);
    char **e;
    const char *eq;

    for (e = environ; *e; e++) {
        eq = strchr(*e, '=');
        if (!eq || eq == *e)
            continue;
        {
            JSAtom a = JS_NewAtomLen(ctx, *e, eq - *e);
            JS_DefinePropertyValue(ctx, obj, a, JS_NewString(ctx, eq + 1), JS_PROP_C_W_E);
            JS_FreeAtom(ctx, a);
        }
    }
    return obj;
}

static JSValue qb_core_setenv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *k = JS_ToCString(ctx, argv[0]), *v;
    if (!k)
        return JS_EXCEPTION;
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        unsetenv(k);
    } else {
        v = JS_ToCString(ctx, argv[1]);
        if (!v) {
            JS_FreeCString(ctx, k);
            return JS_EXCEPTION;
        }
        setenv(k, v, 1);
        JS_FreeCString(ctx, v);
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}

static JSValue qb_core_cwd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char buf[PATH_MAX];
    size_t size = sizeof(buf);
    int err = uv_cwd(buf, &size);
    if (err)
        return qb_throw_uv(ctx, err, "uv_cwd", NULL, NULL);
    return JS_NewStringLen(ctx, buf, size);
}

static JSValue qb_core_chdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *p = JS_ToCString(ctx, argv[0]);
    int err;
    if (!p)
        return JS_EXCEPTION;
    err = uv_chdir(p);
    if (err) {
        JSValue e = qb_throw_uv(ctx, err, "chdir", p, NULL);
        JS_FreeCString(ctx, p);
        return e;
    }
    JS_FreeCString(ctx, p);
    return JS_UNDEFINED;
}

static JSValue qb_new_string_array(JSContext *ctx, int n, char **v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, v[i]));
    return arr;
}

/* info(): the static facts of the process */
static JSValue qb_core_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    JSValue o = JS_NewObject(ctx);

    qb_set(ctx, o, "argv", qb_new_string_array(ctx, qr->argc, qr->argv));
    qb_set(ctx, o, "execArgv", qb_new_string_array(ctx, qr->exec_argc, qr->exec_argv));
    qb_set(ctx, o, "execPath", JS_NewString(ctx, qr->exec_path ? qr->exec_path : ""));
    qb_set(ctx, o, "mainPath", qr->main_path ? JS_NewString(ctx, qr->main_path) : JS_UNDEFINED);
    qb_set(ctx, o, "libDir", JS_NewString(ctx, qr->lib_dir));
    qb_set(ctx, o, "isMainThread", JS_NewBool(ctx, qr->is_main_thread));
    qb_set(ctx, o, "pid", JS_NewInt32(ctx, uv_os_getpid()));
    qb_set(ctx, o, "ppid", JS_NewInt32(ctx, uv_os_getppid()));
#if defined(__APPLE__)
    qb_set(ctx, o, "platform", JS_NewString(ctx, "darwin"));
#elif defined(__linux__)
    qb_set(ctx, o, "platform", JS_NewString(ctx, "linux"));
#elif defined(_WIN32)
    qb_set(ctx, o, "platform", JS_NewString(ctx, "win32"));
#else
    qb_set(ctx, o, "platform", JS_NewString(ctx, "unknown"));
#endif
#if defined(__aarch64__) || defined(__arm64__)
    qb_set(ctx, o, "arch", JS_NewString(ctx, "arm64"));
#elif defined(__x86_64__)
    qb_set(ctx, o, "arch", JS_NewString(ctx, "x64"));
#elif defined(__i386__)
    qb_set(ctx, o, "arch", JS_NewString(ctx, "ia32"));
#else
    qb_set(ctx, o, "arch", JS_NewString(ctx, "unknown"));
#endif
    qb_set(ctx, o, "engine", JS_NewString(ctx, JS_GetVersion()));
    qb_set(ctx, o, "uvVersion", JS_NewString(ctx, uv_version_string()));
    return o;
}

/* callerFilename(levels): the file of the function 'levels' frames above
   the caller of callerFilename (0: the caller) */
static JSValue qb_core_callerFilename(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    int n = 0;
    JSAtom a;
    JSValue ret;
    if (argc > 0 && JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    a = JS_GetScriptOrModuleName(ctx, n + 1);
    if (a == JS_ATOM_NULL)
        return JS_UNDEFINED;
    ret = JS_AtomToString(ctx, a);
    JS_FreeAtom(ctx, a);
    return ret;
}

/* require() and require.resolve() of the global scope (ES modules call
   them like Bun's import.meta.require): relative to the calling file */
static JSValue qb_core_globalRequire(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv, int magic)
{
    QBRuntime *qr = qb_get(ctx);
    JSAtom a;
    JSValue args[3], ret;

    a = JS_GetScriptOrModuleName(ctx, 1);
    args[0] = a == JS_ATOM_NULL ? JS_UNDEFINED : JS_AtomToString(ctx, a);
    JS_FreeAtom(ctx, a);
    args[1] = argc > 0 ? argv[0] : JS_UNDEFINED;
    args[2] = argc > 1 ? argv[1] : JS_UNDEFINED;
    ret = qb_call_hook_sync(qr, magic ? "requireResolveFrom" : "requireFrom", 3,
                            (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    return ret;
}

static JSValue qb_core_promiseState(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    JSValue arr;
    int st;
    if (!JS_IsPromise(argv[0]))
        return JS_UNDEFINED;
    st = JS_PromiseState(ctx, argv[0]);
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, st));
    if (st != JS_PROMISE_PENDING)
        JS_SetPropertyUint32(ctx, arr, 1, JS_PromiseResult(ctx, argv[0]));
    return arr;
}

static JSValue qb_core_proxyTarget(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    if (!JS_IsProxy(argv[0]))
        return JS_UNDEFINED;
    return JS_GetProxyTarget(ctx, argv[0]);
}

/* proxyDetails(p): [target, handler], undefined for a non-proxy */
static JSValue qb_core_proxyDetails(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    JSValue arr;
    if (!JS_IsProxy(argv[0]))
        return JS_UNDEFINED;
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_GetProxyTarget(ctx, argv[0]));
    JS_SetPropertyUint32(ctx, arr, 1, JS_GetProxyHandler(ctx, argv[0]));
    return arr;
}

static JSValue qb_core_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue qb_core_memoryUsage(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    JSMemoryUsage mu;
    size_t rss = 0;
    JSValue o = JS_NewObject(ctx);

    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
    uv_resident_set_memory(&rss);
    qb_set(ctx, o, "rss", JS_NewInt64(ctx, rss));
    qb_set(ctx, o, "heapTotal", JS_NewInt64(ctx, mu.malloc_size));
    qb_set(ctx, o, "heapUsed", JS_NewInt64(ctx, mu.memory_used_size));
    qb_set(ctx, o, "external", JS_NewInt64(ctx, mu.binary_object_size));
    qb_set(ctx, o, "arrayBuffers", JS_NewInt64(ctx, mu.binary_object_size));
    return o;
}

/* ------------------------------------------------------------------ timers */

static JSClassID qb_timer_class_id;

/* the struct is freed when both the handle is closed and the JS object
   is collected */
typedef struct QBTimer {
    uv_timer_t h;
    QBRuntime *qr;
    JSValue cb;
    JSValue self; /* strong while the timer is active */
    int closed;   /* uv_close() called */
    int close_done;
    int js_dead;
} QBTimer;

static void qb_timer_close_cb(uv_handle_t *h)
{
    QBTimer *t = h->data;
    t->close_done = 1;
    if (t->js_dead)
        free(t);
}

static void qb_timer_finalizer(JSRuntime *rt, JSValue val)
{
    QBTimer *t = JS_GetOpaque(val, qb_timer_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->cb);
    t->cb = JS_UNDEFINED;
    t->js_dead = 1;
    if (!t->closed) {
        t->closed = 1;
        uv_close((uv_handle_t *)&t->h, qb_timer_close_cb);
    } else if (t->close_done) {
        free(t);
    }
}

static void qb_timer_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBTimer *t = JS_GetOpaque(val, qb_timer_class_id);
    if (t)
        JS_MarkValue(rt, t->cb, mark_func);
}

static JSClassDef qb_timer_class = {
    "Timer",
    .finalizer = qb_timer_finalizer,
    .gc_mark = qb_timer_mark,
};

static void qb_timer_cleanup(void *arg);

static void qb_timer_release(QBTimer *t)
{
    JSValue self = t->self;
    if (JS_IsUndefined(self))
        return;
    t->self = JS_UNDEFINED;
    qb_remove_cleanup_hook(t->qr, qb_timer_cleanup, t);
    JS_FreeValue(t->qr->ctx, self);
}

/* runtime exit: an active timer drops the reference that kept it alive */
static void qb_timer_cleanup(void *arg)
{
    QBTimer *t = arg;
    JSValue self = t->self;
    if (!t->closed)
        uv_timer_stop(&t->h);
    t->self = JS_UNDEFINED;
    JS_FreeValue(t->qr->ctx, self);
}

static void qb_timer_cb(uv_timer_t *h)
{
    QBTimer *t = h->data;
    JSContext *ctx = t->qr->ctx;
    JSValue self, cb, ret;

    self = JS_DupValue(ctx, t->self);
    cb = JS_DupValue(ctx, t->cb);
    if (uv_timer_get_repeat(h) == 0)
        qb_timer_release(t);
    ret = qb_call(ctx, cb, self, 0, NULL);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, self);
}

/* createTimer(callback): a libuv timer; start(ms, repeatMs), stop(),
   ref(), unref(), hasRef(), close() */
static JSValue qb_core_createTimer(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    QBTimer *t;
    JSValue obj;

    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    obj = JS_NewObjectClass(ctx, qb_timer_class_id);
    if (JS_IsException(obj))
        return obj;
    t = calloc(1, sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->qr = qr;
    t->cb = JS_DupValue(ctx, argv[0]);
    t->self = JS_UNDEFINED;
    uv_timer_init(qr->loop, &t->h);
    t->h.data = t;
    JS_SetOpaque(obj, t);
    return obj;
}

static QBTimer *qb_timer_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_timer_class_id);
}

static JSValue qb_timer_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBTimer *t = qb_timer_get(ctx, this_val);
    double ms, rep = 0;
    if (!t)
        return JS_EXCEPTION;
    if (t->closed)
        return JS_UNDEFINED;
    if (JS_ToFloat64(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToFloat64(ctx, &rep, argv[1]))
        return JS_EXCEPTION;
    if (!(ms >= 0))
        ms = 0;
    if (!(rep >= 0))
        rep = 0;
    uv_update_time(t->qr->loop);
    uv_timer_start(&t->h, qb_timer_cb, (uint64_t)ms, (uint64_t)rep);
    if (JS_IsUndefined(t->self)) {
        t->self = JS_DupValue(ctx, this_val);
        qb_add_cleanup_hook(t->qr, qb_timer_cleanup, t);
    }
    return JS_UNDEFINED;
}

static JSValue qb_timer_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                               int magic)
{
    QBTimer *t = qb_timer_get(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* stop */
        if (!t->closed)
            uv_timer_stop(&t->h);
        qb_timer_release(t);
        break;
    case 1: /* ref */
        if (!t->closed)
            uv_ref((uv_handle_t *)&t->h);
        break;
    case 2: /* unref */
        if (!t->closed)
            uv_unref((uv_handle_t *)&t->h);
        break;
    case 3: /* hasRef */
        return JS_NewBool(ctx, !t->closed && uv_has_ref((uv_handle_t *)&t->h));
    case 4: /* close */
        if (!t->closed) {
            uv_timer_stop(&t->h);
            t->closed = 1;
            uv_close((uv_handle_t *)&t->h, qb_timer_close_cb);
        }
        qb_timer_release(t);
        break;
    case 5: /* isActive */
        return JS_NewBool(ctx, !t->closed && uv_is_active((uv_handle_t *)&t->h));
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_timer_proto_funcs[] = {
    JS_CFUNC_DEF("start", 2, qb_timer_start),
    JS_CFUNC_MAGIC_DEF("stop", 0, qb_timer_method, 0),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_timer_method, 1),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_timer_method, 2),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_timer_method, 3),
    JS_CFUNC_MAGIC_DEF("close", 0, qb_timer_method, 4),
    JS_CFUNC_MAGIC_DEF("isActive", 0, qb_timer_method, 5),
};

/* -------------------------------------------------------------- immediates */

static void qb_immediate_check_cb(uv_check_t *h)
{
    QBRuntime *qr = h->data;
    JSValue ret;
    if (!qr->immediate_pending)
        return;
    qr->immediate_pending = 0;
    uv_check_stop(&qr->immediate_check);
    uv_idle_stop(&qr->immediate_idle);
    ret = qb_call_hook(qr, "runImmediates", 0, NULL);
    JS_FreeValue(qr->ctx, ret);
}

static void qb_immediate_idle_cb(uv_idle_t *h)
{
}

/* scheduleImmediates(ref): run hooks.runImmediates() in the check phase
   of the loop; 'ref' keeps the loop alive for them */
static JSValue qb_core_scheduleImmediates(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBRuntime *qr = qb_get(ctx);
    int ref = argc > 0 ? JS_ToBool(ctx, argv[0]) : 1;
    if (!qr->immediate_pending) {
        qr->immediate_pending = 1;
        uv_check_start(&qr->immediate_check, qb_immediate_check_cb);
        uv_idle_start(&qr->immediate_idle, qb_immediate_idle_cb);
    }
    if (ref) {
        uv_ref((uv_handle_t *)&qr->immediate_check);
        uv_ref((uv_handle_t *)&qr->immediate_idle);
    } else {
        uv_unref((uv_handle_t *)&qr->immediate_check);
        uv_unref((uv_handle_t *)&qr->immediate_idle);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_core_funcs[] = {
    JS_CFUNC_DEF("setHooks", 1, qb_core_setHooks),
    JS_CFUNC_DEF("setBufferPrototype", 1, qb_core_setBufferPrototype),
    JS_CFUNC_DEF("loadModuleSync", 3, qb_core_loadModuleSync),
    JS_CFUNC_DEF("compileFunction", 4, qb_core_compileFunction),
    JS_CFUNC_DEF("runScript", 3, qb_core_runScript),
    JS_CFUNC_DEF("detectModule", 2, qb_core_detectModule),
    JS_CFUNC_DEF("statKind", 1, qb_core_statKind),
    JS_CFUNC_DEF("realpath", 1, qb_core_realpath),
    JS_CFUNC_DEF("readText", 1, qb_core_readText),
    JS_CFUNC_DEF("writeSync", 2, qb_core_writeSync),
    JS_CFUNC_DEF("hrtime", 0, qb_core_hrtime),
    JS_CFUNC_DEF("startTime", 0, qb_core_startTime),
    JS_CFUNC_DEF("drainMicrotasks", 0, qb_core_drainMicrotasks),
    JS_CFUNC_DEF("reallyExit", 1, qb_core_reallyExit),
    JS_CFUNC_DEF("environ", 0, qb_core_environ),
    JS_CFUNC_DEF("setenv", 2, qb_core_setenv),
    JS_CFUNC_DEF("cwd", 0, qb_core_cwd),
    JS_CFUNC_DEF("chdir", 1, qb_core_chdir),
    JS_CFUNC_DEF("info", 0, qb_core_info),
    JS_CFUNC_DEF("callerFilename", 1, qb_core_callerFilename),
    JS_CFUNC_MAGIC_DEF("globalRequire", 1, qb_core_globalRequire, 0),
    JS_CFUNC_MAGIC_DEF("globalRequireResolve", 1, qb_core_globalRequire, 1),
    JS_CFUNC_DEF("promiseState", 1, qb_core_promiseState),
    JS_CFUNC_DEF("proxyTarget", 1, qb_core_proxyTarget),
    JS_CFUNC_DEF("proxyDetails", 1, qb_core_proxyDetails),
    JS_CFUNC_DEF("gc", 0, qb_core_gc),
    JS_CFUNC_DEF("memoryUsage", 0, qb_core_memoryUsage),
    JS_CFUNC_DEF("createTimer", 1, qb_core_createTimer),
    JS_CFUNC_DEF("scheduleImmediates", 1, qb_core_scheduleImmediates),
};

QB_SIMPLE_MODULE(core, qb_core_funcs)

/* -------------------------------------------------------------- lifecycle */

static int qb_interrupt(JSRuntime *rt, void *opaque)
{
    QBRuntime *qr = opaque;
    return qr->exiting;
}

QBRuntime *qb_runtime_new(const QBOptions *o)
{
    QBRuntime *qr = calloc(1, sizeof(*qr));
    JSValue proto;

    if (!qr)
        return NULL;
    qr->lib_dir = strdup(o->lib_dir);
    qr->exec_path = strdup(o->exec_path ? o->exec_path : "");
    qr->argc = o->argc;
    qr->argv = o->argv;
    qr->exec_argc = o->exec_argc;
    qr->exec_argv = o->exec_argv;
    qr->is_main_thread = o->is_main_thread;
    qr->start_time = uv_hrtime();
    if (o->loop) {
        qr->loop = o->loop;
    } else {
        qr->loop = malloc(sizeof(uv_loop_t));
        uv_loop_init(qr->loop);
    }
    qr->rt = JS_NewRuntime();
    JS_SetRuntimeOpaque(qr->rt, qr);
    qb_worker_runtime_init(qr);
    /* Node's default is about 1 MB of native stack per JS frame budget;
       the TypeScript sources of big applications nest deeply */
    JS_SetMaxStackSize(qr->rt, 8 * 1024 * 1024 - 256 * 1024);
    qr->ctx = JS_NewContext(qr->rt);
#ifdef CONFIG_TEMPORAL_INTL
    JS_AddIntrinsicTemporal(qr->ctx);
    JS_AddIntrinsicIntl(qr->ctx);
#endif
    qr->hooks = JS_UNDEFINED;
    qr->buffer_proto = JS_UNDEFINED;
    JS_SetModuleLoaderFunc2(qr->rt, NULL, qb_load, qb_check_attributes, qr);
    JS_SetModuleNormalizeFunc2(qr->rt, qb_normalize);
    JS_SetHostPromiseRejectionTracker(qr->rt, qb_rejection_tracker, qr);
    JS_SetInterruptHandler(qr->rt, qb_interrupt, qr);

    JS_NewClassID(&qb_timer_class_id);
    JS_NewClass(qr->rt, qb_timer_class_id, &qb_timer_class);
    proto = JS_NewObject(qr->ctx);
    JS_SetPropertyFunctionList(qr->ctx, proto, qb_timer_proto_funcs, countof(qb_timer_proto_funcs));
    JS_SetClassProto(qr->ctx, qb_timer_class_id, proto);

    uv_check_init(qr->loop, &qr->immediate_check);
    qr->immediate_check.data = qr;
    uv_idle_init(qr->loop, &qr->immediate_idle);
    qr->immediate_idle.data = qr;
    uv_unref((uv_handle_t *)&qr->immediate_check);
    uv_unref((uv_handle_t *)&qr->immediate_idle);
    return qr;
}

int qb_bootstrap(QBRuntime *qr)
{
    JSValue ns;
    qb_call_depth++;
    ns = JS_LoadModuleSync(qr->ctx, "", "internal:bootstrap", JS_UNDEFINED);
    qb_call_depth--;
    if (JS_IsException(ns)) {
        JSValue exc = JS_GetException(qr->ctx);
        fprintf(stderr, "qbun: bootstrap failed: ");
        qb_print_error_fallback(qr->ctx, exc);
        JS_FreeValue(qr->ctx, exc);
        return -1;
    }
    JS_FreeValue(qr->ctx, ns);
    qb_drain(qr);
    return 0;
}

static JSValue qb_main_rejected(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JS_Throw(ctx, JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED));
    qb_report_exception(ctx);
    return JS_UNDEFINED;
}

/* catch the rejection of the promise of the entry module as an uncaught
   exception */
static int qb_watch_main(QBRuntime *qr, JSValue promise)
{
    JSContext *ctx = qr->ctx;
    JSValue then, args[2], ret;

    if (JS_IsException(promise)) {
        qb_report_exception(ctx);
        return -1;
    }
    then = JS_GetPropertyStr(ctx, promise, "then");
    args[0] = JS_UNDEFINED;
    args[1] = JS_NewCFunction(ctx, qb_main_rejected, "onMainRejected", 1);
    ret = JS_Call(ctx, then, promise, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, then);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, promise);
    qb_drain(qr);
    return 0;
}

int qb_preload(QBRuntime *qr, const char *spec)
{
    JSContext *ctx = qr->ctx;
    char base[PATH_MAX + 2];
    size_t size = PATH_MAX;
    JSValue ns;

    /* resolved like an import from the working directory */
    if (uv_cwd(base, &size))
        strcpy(base, "/");
    pstrcat(base, sizeof(base), "/");
    qb_call_depth++;
    ns = JS_LoadModuleSync(ctx, base, spec, JS_UNDEFINED);
    qb_call_depth--;
    if (JS_IsException(ns)) {
        qb_report_exception(ctx);
        return -1;
    }
    JS_FreeValue(ctx, ns);
    qb_drain(qr);
    return qr->exiting ? -1 : 0;
}

int qb_run_main(QBRuntime *qr, const char *filename)
{
    JSContext *ctx = qr->ctx;
    char buf[PATH_MAX];
    uint8_t *src;
    size_t len;
    JSValue ret, arg;

    if (!realpath(filename, buf)) {
        fprintf(stderr, "error: Module not found \"%s\"\n", filename);
        return -1;
    }
    free(qr->main_path);
    qr->main_path = strdup(buf);
    src = qb_read_file(ctx, buf, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read \"%s\"\n", buf);
        return -1;
    }
    if (qb_is_code_ext(qb_ext(buf)) && qb_is_commonjs(buf, (char *)src, len)) {
        js_free(ctx, src);
        arg = JS_NewString(ctx, buf);
        ret = qb_call_hook(qr, "runMain", 1, (JSValueConst *)&arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, ret);
        return 0;
    }
    js_free(ctx, src);
    qb_call_depth++;
    ret = JS_LoadModule(ctx, "", buf);
    qb_call_depth--;
    return qb_watch_main(qr, ret);
}

int qb_run_eval(QBRuntime *qr, const char *src, int ts)
{
    JSModuleDef *m;
    JSValue val;
    char path[PATH_MAX + 16];
    size_t size = PATH_MAX;

    if (uv_cwd(path, &size))
        strcpy(path, "/");
    pstrcat(path, sizeof(path), "/[eval]");
    qb_call_depth++;
    m = qb_compile_module(qr->ctx, path, src, strlen(src), ts ? JS_EVAL_FLAG_TYPESCRIPT : 0, 1);
    if (!m) {
        qb_call_depth--;
        qb_report_exception(qr->ctx);
        return -1;
    }
    val = JS_EvalFunction(qr->ctx, JS_DupValue(qr->ctx, JS_MKPTR(JS_TAG_MODULE, m)));
    qb_call_depth--;
    return qb_watch_main(qr, val);
}

int qb_run_loop(QBRuntime *qr)
{
    JSValue ret;
    for (;;) {
        qb_drain(qr);
        uv_run(qr->loop, UV_RUN_DEFAULT);
        if (qr->exiting)
            break;
        ret = qb_call_hook(qr, "beforeExit", 0, NULL);
        JS_FreeValue(qr->ctx, ret);
        if (qr->exiting || !uv_loop_alive(qr->loop))
            break;
    }
    return qr->exit_code;
}

typedef struct QBCleanupHook {
    struct QBCleanupHook *next;
    QBCleanupFunc *fn;
    void *arg;
} QBCleanupHook;

void qb_add_cleanup_hook(QBRuntime *qr, QBCleanupFunc *fn, void *arg)
{
    QBCleanupHook *h = malloc(sizeof(*h));
    if (!h)
        return;
    h->fn = fn;
    h->arg = arg;
    h->next = qr->cleanup_hooks;
    qr->cleanup_hooks = h;
}

void qb_remove_cleanup_hook(QBRuntime *qr, QBCleanupFunc *fn, void *arg)
{
    QBCleanupHook **ph, *h;
    for (ph = &qr->cleanup_hooks; (h = *ph) != NULL; ph = &h->next) {
        if (h->fn == fn && h->arg == arg) {
            *ph = h->next;
            free(h);
            return;
        }
    }
}

/* most recent first, like Node */
static void qb_run_cleanup_hooks(QBRuntime *qr)
{
    QBCleanupHook *h;
    while ((h = qr->cleanup_hooks) != NULL) {
        qr->cleanup_hooks = h->next;
        h->fn(h->arg);
        free(h);
    }
}

static void qb_walk_close(uv_handle_t *h, void *arg)
{
    if (!uv_is_closing(h))
        uv_close(h, NULL);
}

void qb_runtime_free(QBRuntime *qr)
{
    QBRejection *r;
    int own_loop = qr->loop != uv_default_loop();

    while ((r = qr->rejections) != NULL) {
        qr->rejections = r->next;
        JS_FreeValue(qr->ctx, r->promise);
        JS_FreeValue(qr->ctx, r->reason);
        free(r);
    }
    JS_FreeValue(qr->ctx, qr->hooks);
    qr->hooks = JS_UNDEFINED;
    JS_FreeValue(qr->ctx, qr->buffer_proto);
    qr->buffer_proto = JS_UNDEFINED;
    JS_FreeContext(qr->ctx);
    JS_FreeRuntime(qr->rt);
    if (own_loop) {
        /* the remaining handles belong to the freed runtime: close them
           and let their close callbacks run */
        uv_walk(qr->loop, qb_walk_close, NULL);
        uv_run(qr->loop, UV_RUN_DEFAULT);
        uv_loop_close(qr->loop);
        free(qr->loop);
    }
    free(qr->lib_dir);
    free(qr->exec_path);
    free(qr->main_path);
    free(qr);
}

int qb_runtime_exit(QBRuntime *qr)
{
    JSContext *ctx = qr->ctx;
    JSValue fn, ret;
    int code = qr->exit_code;

    if (qr->exiting) {
        qb_run_cleanup_hooks(qr);
        return qr->exit_code;
    }
    fn = qb_hook(qr, "emitExit");
    if (JS_IsFunction(ctx, fn)) {
        ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret))
            qb_report_exception(ctx);
        else if (JS_IsNumber(ret))
            JS_ToInt32(ctx, &code, ret);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
    if (!qr->exiting)
        qr->exit_code = code;
    qr->exiting = 1;
    qb_run_cleanup_hooks(qr);
    return qr->exit_code;
}
