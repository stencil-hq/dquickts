/*
 * qbun:napi — Node-API (N-API version 10) for native addons.
 *
 * The napi_* / node_api_* C functions are exported by the qbun executable
 * (linked with -export_dynamic / -rdynamic): addons resolve them when they
 * are loaded (napi-rs looks them up with dlsym(dlopen(NULL))).
 *
 * Model (after Node's src/js_native_api_v8.cc and src/node_api.cc):
 *   - one napi_env per loaded module (dlopen), holding the context, the
 *     references, the instance data, the last error and the pending
 *     exception (an exception thrown by JS code called from native code, or
 *     by napi_throw*, is kept there and thrown when the native callback
 *     returns to JS).
 *   - napi_value: a pointer to a JSValue. The values made by the API live in
 *     a stack of blocks; a handle scope records the top of the stack when it
 *     is opened and frees the values above it when it is closed. A default
 *     scope surrounds every call from JS into native code, finalizers, async
 *     work completions and thread-safe function calls. The arguments and
 *     'this' of a callback are borrowed from the caller.
 *   - napi_wrap / type tags / weak references and finalizers of objects: a
 *     hidden holder object of a private class, stored on the object with a
 *     private symbol (a weak map for non-extensible objects), whose class
 *     finalizer runs when the object is collected. Finalizers that call into
 *     the addon run later from the event loop (like Node's second pass weak
 *     callbacks), never from the garbage collector.
 *   - references: refcount > 0 holds the value strongly; refcount 0 keeps
 *     an object weakly (napi_get_reference_value() returns NULL once it was
 *     collected). Symbols stay strongly held; other primitives are dropped.
 *   - async work on the libuv thread pool, thread-safe functions with a
 *     queue, a mutex and a uv_async_t (as Node's ThreadSafeFunction), env
 *     cleanup hooks with qb_add_cleanup_hook() (run at runtime exit).
 */
#define NAPI_EXPERIMENTAL
#define NODE_API_EXPERIMENTAL_NO_WARNING

#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qbun.h"
#include "napi/node_api.h"

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((uint8_t *)(ptr) - offsetof(type, member)))
#endif
#include "../list.h"

#define QBN_API_VERSION 10
/* Node's NODE_API_DEFAULT_MODULE_API_VERSION */
#define QBN_DEFAULT_MODULE_API_VERSION 8

/* the Node.js version reported to addons (node/lib/node/process.ts) */
#define QBN_NODE_MAJOR 24
#define QBN_NODE_MINOR 3
#define QBN_NODE_PATCH 0

/* napi_finalize from node_api_basic_finalize (same ABI, const env) */
#define QBN_FINALIZE(cb) ((napi_finalize)(void (*)(void))(cb))
#define QBN_V(nv)        (*(JSValue *)(nv))
#define QBN_NV(p)        ((napi_value)(p))

typedef struct QBNapiState QBNapiState;
typedef struct QBNRef QBNRef;
typedef struct QBNObjData QBNObjData;

/* ------------------------------------------------------------ structures */

#define QBN_BLOCK_SIZE 254

typedef struct QBNBlock {
    struct QBNBlock *prev;
    struct QBNBlock *next; /* spare block kept for reuse */
    int count;
    JSValue v[QBN_BLOCK_SIZE];
} QBNBlock;

struct napi_handle_scope__ {
    struct napi_handle_scope__ *parent;
    QBNBlock *block;      /* top of the value stack when opened */
    int count;
    JSValue *escape_slot; /* escapable scope: slot in the parent scope */
    uint8_t escapable;
    uint8_t escape_called;
    uint8_t heap; /* malloc'd (napi_open_handle_scope) */
};

struct napi_callback_info__ {
    JSValue this_val;
    JSValue new_target; /* JS_UNDEFINED unless a construct call */
    int is_construct;
    int argc;
    JSValueConst *argv;
    void *data;
};

/* the C side of a function made by napi_create_function / define_* */
typedef struct QBNCallback {
    napi_env env;
    napi_callback cb;
    void *data;
} QBNCallback;

enum { QBN_OWN_USERLAND, QBN_OWN_RUNTIME };
enum { QBN_KIND_VALUE, QBN_KIND_TRACKER };

/* a napi_ref, a finalizer attached to an object (napi_wrap,
   napi_add_finalizer, externals) or a tracked finalizer (instance data,
   node_api_post_finalizer, external buffers) */
struct QBNRef {
    struct list_head link;      /* env->reflist / finalizing_reflist */
    struct list_head pend_link; /* env->pending_finalizers */
    struct list_head obj_link;  /* QBNObjData.refs while weakly held */
    napi_env env;
    JSValue value;              /* counted if 'strong', else raw */
    QBNObjData *od;             /* registered in od->refs (weak object) */
    QBNObjData *wrap_od;        /* the object data whose wrap this is */
    QBNRef **backref;           /* cleared when the ref is freed */
    uint32_t refcount;
    uint8_t kind;
    uint8_t ownership;
    uint8_t is_object;
    uint8_t strong;
    uint8_t empty;         /* value collected or dropped */
    uint8_t has_finalizer; /* in finalizing_reflist (or a tracker) */
    uint8_t holds_env;     /* holds a reference to the env */
    napi_finalize finalize_cb;
    void *data;
    void *hint;
};

/* the hidden data of an object (opaque of the holder object) */
struct QBNObjData {
    struct list_head refs; /* QBNRef.obj_link */
    QBNRef *wrap;
    int has_tag;
    napi_type_tag tag;
};

typedef struct QBNExternal {
    void *data;
    QBNRef *fin; /* finalizer tracker or NULL */
} QBNExternal;

struct napi_deferred__ {
    struct list_head link; /* env->deferreds */
    JSValue resolve;
    JSValue reject;
};

struct napi_async_context__ {
    struct list_head link; /* env->async_contexts */
    JSValue frame;         /* the async context captured at init */
};

struct napi_callback_scope__ {
    napi_env env;
    JSValue saved; /* async context restored at close */
};

struct napi_async_work__ {
    uv_work_t req;
    struct list_head link; /* env->works */
    napi_env env;          /* NULL once the env was torn down */
    napi_async_execute_callback execute;
    napi_async_complete_callback complete;
    void *data;
    JSValue frame;
    uint8_t linked;  /* in env->works */
    uint8_t queued;  /* in the thread pool */
    uint8_t deleted; /* deleted while queued */
};

struct napi_async_cleanup_hook_handle__ {
    napi_env env;
    napi_async_cleanup_hook hook;
    void *arg;
    uint8_t running;
    uint8_t removed;
};

typedef struct QBNTsfnItem {
    struct QBNTsfnItem *next;
    void *data;
} QBNTsfnItem;

enum { QBN_TSFN_OPEN, QBN_TSFN_CLOSING, QBN_TSFN_CLOSED };
#define QBN_DISPATCH_IDLE       0u
#define QBN_DISPATCH_RUNNING    1u
#define QBN_DISPATCH_PENDING    2u
#define QBN_TSFN_MAX_ITERATIONS 1000

struct napi_threadsafe_function__ {
    uv_async_t async;
    uv_mutex_t mutex;
    uv_cond_t cond;
    int has_cond;
    QBNTsfnItem *head;
    QBNTsfnItem *tail;
    size_t queue_size;
    size_t thread_count;
    int state;
    atomic_uint dispatch_state;
    void *context;
    size_t max_queue_size;
    napi_env env;
    void *finalize_data;
    napi_finalize finalize_cb;
    napi_threadsafe_function_call_js call_js_cb;
    JSValue func;
    JSValue frame;
    int handles_closing;
    int finalized; /* EmptyQueue + finalize_cb done */
    int released;  /* func, cleanup hook and env reference released */
};

struct napi_env__ {
    JSContext *ctx;
    QBRuntime *qr;
    QBNapiState *st;
    struct list_head link; /* st->envs */
    char *filename;        /* file: URL of the module */
    int32_t module_api_version;
    int refs;
    uint8_t destructing;
    napi_extended_error_info last_error;
    JSValue pending;
    uint8_t has_pending;
    uint8_t pending_uncatchable;
    QBNBlock *top;
    struct napi_handle_scope__ *scope;
    int open_handle_scopes;
    int open_callback_scopes;
    struct list_head reflist;
    struct list_head finalizing_reflist;
    struct list_head pending_finalizers;
    struct list_head deferreds;
    struct list_head async_contexts;
    struct list_head works;
    QBNRef *instance_data;
    JSValue c_undefined;
    JSValue c_null;
    JSValue c_true;
    JSValue c_false;
    QBNBlock base; /* first block of the value stack */
};

/* per runtime (QBRuntime.napi) */
struct QBNapiState {
    QBRuntime *qr;
    JSContext *ctx;
    struct list_head envs;
    JSAtom od_atom;  /* private symbol: object data holder */
    JSAtom cb_atom;  /* private symbol: class callback holder */
    JSValue weakmap; /* holders of non-extensible objects */
    JSValue wm_get;
    JSValue wm_set;
    JSValue neg_fn;   /* x => -x */
    uv_timer_t timer; /* runs the queued finalizers */
    int timer_active;
    int js_depth;     /* active calls from JS into native code */
    int64_t external_memory;
};

static JSClassID qbn_objdata_class_id;
static JSClassID qbn_external_class_id;
static JSClassID qbn_cbholder_class_id;

/* napi_module_register() during dlopen() (static constructors run on the
   thread that calls dlopen) */
static __thread napi_module *qbn_modpending;

/* modules registered with napi_module_register(), by library handle: a
   library opened again does not run its constructors again */
typedef struct QBNLoaded {
    struct QBNLoaded *next;
    void *handle;
    napi_module *mod;
} QBNLoaded;
static QBNLoaded *qbn_loaded;
static pthread_mutex_t qbn_loaded_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const qbn_error_messages[] = {
    NULL,
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An arraybuffer was expected",
    "A detachable arraybuffer was expected",
    "Main thread would deadlock",
    "External buffers are not allowed",
    "Cannot run JavaScript",
};

/* -------------------------------------------------------- error status */

static inline napi_status qbn_clear_error(napi_env env)
{
    env->last_error.error_code = napi_ok;
    env->last_error.engine_error_code = 0;
    env->last_error.engine_reserved = NULL;
    env->last_error.error_message = NULL;
    return napi_ok;
}

static inline napi_status qbn_set_error(napi_env env, napi_status status)
{
    env->last_error.error_code = status;
    env->last_error.engine_error_code = 0;
    env->last_error.engine_reserved = NULL;
    return status;
}

static inline int qbn_can_run_js(napi_env env)
{
    return !env->qr->exiting && !env->destructing;
}

#define QBN_CHECK_ENV(env)                                                                         \
    do {                                                                                           \
        if (!(env))                                                                                \
            return napi_invalid_arg;                                                               \
    } while (0)

#define QBN_CHECK_ARG(env, arg)                                                                    \
    do {                                                                                           \
        if (!(arg))                                                                                \
            return qbn_set_error((env), napi_invalid_arg);                                         \
    } while (0)

#define QBN_RETURN_IF_FALSE(env, cond, status)                                                     \
    do {                                                                                           \
        if (!(cond))                                                                               \
            return qbn_set_error((env), (status));                                                 \
    } while (0)

/* Node's NAPI_PREAMBLE: no pending exception and JS can run */
#define QBN_PREAMBLE(env)                                                                          \
    do {                                                                                           \
        QBN_CHECK_ENV(env);                                                                        \
        if ((env)->has_pending)                                                                    \
            return qbn_set_error((env), napi_pending_exception);                                   \
        if (!qbn_can_run_js(env))                                                                  \
            return qbn_set_error((env), (env)->module_api_version >= 10 ? napi_cannot_run_js       \
                                                                        : napi_pending_exception); \
        qbn_clear_error(env);                                                                      \
    } while (0)

/* move the exception of the context to the pending exception of 'env' */
static napi_status qbn_catch(napi_env env)
{
    JSContext *ctx = env->ctx;
    int uncatchable;
    JSValue e;

    if (!JS_HasException(ctx))
        return qbn_set_error(env, napi_generic_failure);
    uncatchable = JS_IsUncatchableException(ctx);
    e = JS_GetException(ctx);
    if (env->has_pending)
        JS_FreeValue(ctx, env->pending);
    env->pending = e;
    env->has_pending = 1;
    env->pending_uncatchable = uncatchable;
    return qbn_set_error(env, napi_pending_exception);
}

/* make 'e' (taken) the pending exception */
static void qbn_set_pending(napi_env env, JSValue e)
{
    if (env->has_pending)
        JS_FreeValue(env->ctx, env->pending);
    env->pending = e;
    env->has_pending = 1;
    env->pending_uncatchable = 0;
}

/* throw the pending exception into the context: JS_EXCEPTION */
static JSValue qbn_rethrow(napi_env env)
{
    JSValue e = env->pending;
    int uncatchable = env->pending_uncatchable;

    env->pending = JS_UNDEFINED;
    env->has_pending = 0;
    env->pending_uncatchable = 0;
    JS_Throw(env->ctx, e);
    if (uncatchable)
        JS_SetUncatchableException(env->ctx, TRUE);
    return JS_EXCEPTION;
}

/* a callback into the module from the event loop left an exception:
   report it as uncaught (Node's trigger_fatal_exception) */
static void qbn_report_pending(napi_env env)
{
    if (!env->has_pending)
        return;
    qbn_rethrow(env);
    qb_report_exception(env->ctx);
}

/* ---------------------------------------------------------- value stack */

/* push 'v' (taken) in the current handle scope */
static napi_value qbn_push(napi_env env, JSValue v)
{
    QBNBlock *b = env->top, *n;

    if (b->count == QBN_BLOCK_SIZE) {
        n = b->next;
        if (!n) {
            n = malloc(sizeof(*n));
            if (!n) {
                JS_FreeValue(env->ctx, v);
                return NULL;
            }
            n->prev = b;
            n->next = NULL;
            b->next = n;
        }
        n->count = 0;
        env->top = b = n;
    }
    b->v[b->count] = v;
    return QBN_NV(&b->v[b->count++]);
}

/* free the values above (pb, pc) */
static void qbn_pop_to(napi_env env, QBNBlock *pb, int pc)
{
    JSContext *ctx = env->ctx;
    QBNBlock *b;
    int stop;

    for (;;) {
        b = env->top;
        stop = (b == pb) ? pc : 0;
        while (b->count > stop) {
            JSValue v = b->v[--b->count];
            JS_FreeValue(ctx, v);
        }
        if (b == pb || !b->prev)
            break;
        env->top = b->prev;
    }
}

/* store 'v' (taken) in *result: napi_ok, or the error for JS_EXCEPTION */
static napi_status qbn_result(napi_env env, JSValue v, napi_value *result)
{
    napi_value nv;
    if (JS_IsException(v))
        return qbn_catch(env);
    nv = qbn_push(env, v);
    if (!nv)
        return qbn_set_error(env, napi_generic_failure);
    *result = nv;
    return qbn_clear_error(env);
}

static int qbn_scope_open(napi_env env, struct napi_handle_scope__ *s, int escapable, int heap)
{
    s->parent = env->scope;
    s->escapable = escapable;
    s->escape_called = 0;
    s->heap = heap;
    s->escape_slot = NULL;
    if (escapable) {
        napi_value slot = qbn_push(env, JS_UNDEFINED);
        if (!slot)
            return -1;
        s->escape_slot = (JSValue *)slot;
    }
    s->block = env->top;
    s->count = env->top->count;
    env->scope = s;
    env->open_handle_scopes++;
    return 0;
}

/* close the innermost scope 's' */
static void qbn_scope_pop(napi_env env, struct napi_handle_scope__ *s)
{
    qbn_pop_to(env, s->block, s->count);
    env->scope = s->parent;
    env->open_handle_scopes--;
    if (s->heap)
        free(s);
}

/* close the internal scope 's' and any scope left open above it */
static void qbn_scope_close(napi_env env, struct napi_handle_scope__ *s)
{
    struct napi_handle_scope__ *p;
    for (p = env->scope; p && p != s; p = p->parent)
        ;
    if (!p)
        return;
    while (env->scope != s)
        qbn_scope_pop(env, env->scope);
    qbn_scope_pop(env, s);
}

/* -------------------------------------------------------------- strings */

/* UTF-8 bytes of a string (Latin-1 or UTF-16 data), unpaired surrogates
   replaced by U+FFFD; no character is cut: at most 'cap' bytes. 'out' NULL
   only counts. */
static size_t qbn_utf8_encode(const void *data, size_t len, int wide, char *out, size_t cap)
{
    size_t n = 0, i;

    if (!wide) {
        const uint8_t *s = data;
        for (i = 0; i < len; i++) {
            uint8_t c = s[i];
            if (c < 0x80) {
                if (n + 1 > cap)
                    break;
                if (out)
                    out[n] = (char)c;
                n++;
            } else {
                if (n + 2 > cap)
                    break;
                if (out) {
                    out[n] = (char)(0xc0 | (c >> 6));
                    out[n + 1] = (char)(0x80 | (c & 0x3f));
                }
                n += 2;
            }
        }
    } else {
        const uint16_t *s = data;
        for (i = 0; i < len; i++) {
            uint32_t c = s[i];
            int k;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < len && s[i + 1] >= 0xdc00 &&
                s[i + 1] < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
                k = 4;
            } else if (c >= 0xd800 && c < 0xe000) {
                c = 0xfffd;
                k = 3;
            } else {
                k = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            }
            if (n + k > cap)
                break;
            if (out) {
                switch (k) {
                case 1:
                    out[n] = (char)c;
                    break;
                case 2:
                    out[n] = (char)(0xc0 | (c >> 6));
                    out[n + 1] = (char)(0x80 | (c & 0x3f));
                    break;
                case 3:
                    out[n] = (char)(0xe0 | (c >> 12));
                    out[n + 1] = (char)(0x80 | ((c >> 6) & 0x3f));
                    out[n + 2] = (char)(0x80 | (c & 0x3f));
                    break;
                default:
                    out[n] = (char)(0xf0 | (c >> 18));
                    out[n + 1] = (char)(0x80 | ((c >> 12) & 0x3f));
                    out[n + 2] = (char)(0x80 | ((c >> 6) & 0x3f));
                    out[n + 3] = (char)(0x80 | (c & 0x3f));
                    break;
                }
            }
            n += k;
            if (k == 4)
                i++;
        }
    }
    return n;
}

/* borrowed contents of the string 'v'; -1 on error (exception cleared) */
static int qbn_string_data(napi_env env, JSValueConst v, const void **pdata, size_t *plen,
                           int *pwide)
{
    JSContext *ctx = env->ctx;
    JS_BOOL wide = FALSE;
    size_t len = 0;
    const void *p = JS_GetStringData(ctx, v, &len, &wide);
    if (!p) {
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return -1;
        }
        len = 0;
        p = "";
        wide = FALSE;
    }
    *pdata = p;
    *plen = len;
    *pwide = wide;
    return 0;
}

static size_t qbn_strlen16(const char16_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

#define QBN_CHECK_NEW_STRING_ARGS(env, str, length, result)                                        \
    do {                                                                                           \
        QBN_CHECK_ENV(env);                                                                        \
        if ((length) > 0)                                                                          \
            QBN_CHECK_ARG((env), (str));                                                           \
        QBN_CHECK_ARG((env), (result));                                                            \
        QBN_RETURN_IF_FALSE((env), ((length) == NAPI_AUTO_LENGTH) || (length) <= INT_MAX,          \
                            napi_invalid_arg);                                                     \
    } while (0)

static JSValue qbn_new_utf8(JSContext *ctx, const char *str, size_t length)
{
    if (!str)
        return JS_NewStringLen(ctx, "", 0);
    if (length == NAPI_AUTO_LENGTH)
        length = strlen(str);
    return JS_NewStringLen(ctx, str, length);
}

static JSValue qbn_new_latin1(JSContext *ctx, const char *str, size_t length)
{
    if (!str)
        return JS_NewStringLen(ctx, "", 0);
    if (length == NAPI_AUTO_LENGTH)
        length = strlen(str);
    return JS_NewStringLatin1Len(ctx, (const uint8_t *)str, length);
}

static JSValue qbn_new_utf16(JSContext *ctx, const char16_t *str, size_t length)
{
    if (!str)
        return JS_NewStringLen(ctx, "", 0);
    if (length == NAPI_AUTO_LENGTH)
        length = qbn_strlen16(str);
    return JS_NewStringUTF16(ctx, (const uint16_t *)str, length);
}

/* ---------------------------------------------------------- environment */

static void qbn_env_unref(napi_env env)
{
    QBNBlock *b, *n;
    if (--env->refs > 0)
        return;
    /* the value stack is empty since the teardown */
    for (b = env->base.next; b; b = n) {
        n = b->next;
        free(b);
    }
    free(env->filename);
    free(env);
}

static void qbn_schedule_finalizers(QBNapiState *st);

/* ------------------------------------------------------------ references */

static void qbn_ref_unlink(QBNRef *r)
{
    if (r->link.next)
        list_del(&r->link);
}

static void qbn_ref_dequeue(QBNRef *r)
{
    if (r->pend_link.next)
        list_del(&r->pend_link);
}

/* drop the value (strong or weak) of 'r' */
static void qbn_ref_clear_value(QBNRef *r)
{
    if (r->od) {
        list_del(&r->obj_link);
        r->od = NULL;
    }
    if (r->strong) {
        JSValue v = r->value;
        r->strong = 0;
        r->value = JS_UNDEFINED;
        JS_FreeValue(r->env->ctx, v);
    }
    r->value = JS_UNDEFINED;
    r->empty = 1;
}

static void qbn_ref_free(QBNRef *r)
{
    napi_env env = r->env;
    int holds = r->holds_env;

    if (r->kind == QBN_KIND_VALUE && !r->empty)
        qbn_ref_clear_value(r);
    else if (r->od) {
        list_del(&r->obj_link);
        r->od = NULL;
    }
    qbn_ref_unlink(r);
    qbn_ref_dequeue(r);
    if (r->wrap_od) {
        if (r->wrap_od->wrap == r)
            r->wrap_od->wrap = NULL;
        r->wrap_od = NULL;
    }
    if (r->backref) {
        *r->backref = NULL;
        r->backref = NULL;
    }
    free(r);
    if (holds)
        qbn_env_unref(env);
}

/* call a finalizer of the addon (from the event loop, the teardown or a
   native call) */
static void qbn_call_finalizer(napi_env env, napi_finalize cb, void *data, void *hint)
{
    struct napi_handle_scope__ scope;

    if (!cb)
        return;
    if (qbn_scope_open(env, &scope, 0, 0) < 0) {
        cb(env, data, hint);
        return;
    }
    qbn_clear_error(env);
    cb(env, data, hint);
    qbn_scope_close(env, &scope);
    qbn_report_pending(env);
}

/* Node's Reference::Finalize / TrackedFinalizer::Finalize */
static void qbn_ref_finalize(QBNRef *r)
{
    napi_env env = r->env;
    napi_finalize cb = r->finalize_cb;
    void *data = r->data, *hint = r->hint;
    int delete_me = r->ownership == QBN_OWN_RUNTIME;

    if (r->kind == QBN_KIND_VALUE && !r->empty)
        qbn_ref_clear_value(r);
    qbn_ref_unlink(r);
    qbn_ref_dequeue(r);
    r->finalize_cb = NULL;
    r->hint = NULL;
    if (cb)
        r->data = NULL;
    if (r->holds_env) {
        /* external buffer: the env stays alive until the call is done */
        env->refs++;
        if (cb)
            qbn_call_finalizer(env, cb, data, hint);
        qbn_ref_free(r);
        qbn_env_unref(env);
        return;
    }
    if (cb)
        qbn_call_finalizer(env, cb, data, hint);
    /* a userland reference may have been deleted by the finalizer */
    if (delete_me)
        qbn_ref_free(r);
}

static void qbn_enqueue_finalizer(QBNRef *r)
{
    napi_env env = r->env;
    if (r->pend_link.next)
        return;
    list_add_tail(&r->pend_link, &env->pending_finalizers);
    qbn_schedule_finalizers(env->st);
}

/* the object of 'r' was collected (runs in the garbage collector: no call
   into the addon) */
static void qbn_ref_collected(QBNRef *r)
{
    if (r->has_finalizer)
        qbn_enqueue_finalizer(r);
    else
        qbn_ref_finalize(r); /* no user callback */
}

static QBNObjData *qbn_objdata_ensure(napi_env env, JSValueConst obj);

/* refcount 0: hold the value weakly */
static void qbn_ref_set_weak(QBNRef *r)
{
    napi_env env = r->env;
    QBNObjData *od;
    JSValue v;

    if (!r->strong || r->empty)
        return;
    if (r->is_object) {
        od = qbn_objdata_ensure(env, r->value);
        if (!od)
            return; /* cannot be tracked: stays strong */
        list_add_tail(&r->obj_link, &od->refs);
        r->od = od;
        v = r->value;
        r->strong = 0;
        /* may collect the object now: then r->empty is set */
        JS_FreeValue(env->ctx, v);
    } else if (JS_IsSymbol(r->value)) {
        /* symbols are kept (never reported collected) */
    } else {
        /* primitives cannot be held weakly (Node resets them) */
        v = r->value;
        r->strong = 0;
        r->value = JS_UNDEFINED;
        r->empty = 1;
        JS_FreeValue(env->ctx, v);
    }
}

/* refcount 1: hold the value strongly */
static void qbn_ref_set_strong(QBNRef *r)
{
    if (r->strong || r->empty)
        return;
    if (r->od) {
        list_del(&r->obj_link);
        r->od = NULL;
    }
    /* not collected: the object is alive */
    r->value = JS_DupValue(r->env->ctx, r->value);
    r->strong = 1;
}

static QBNRef *qbn_ref_new(napi_env env, JSValueConst value, uint32_t refcount, int ownership,
                           int has_finalizer, napi_finalize cb, void *data, void *hint)
{
    QBNRef *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->env = env;
    r->kind = QBN_KIND_VALUE;
    r->ownership = ownership;
    r->refcount = refcount;
    r->is_object = JS_IsObject(value);
    r->value = JS_DupValue(env->ctx, value);
    r->strong = 1;
    r->has_finalizer = has_finalizer;
    r->finalize_cb = cb;
    r->data = data;
    r->hint = hint;
    list_add_tail(&r->link, has_finalizer ? &env->finalizing_reflist : &env->reflist);
    if (refcount == 0)
        qbn_ref_set_weak(r);
    return r;
}

/* a tracked finalizer: in the finalizing list (instance data, post
   finalizer, externals) or holding the env (external buffers) */
static QBNRef *qbn_tracker_new(napi_env env, napi_finalize cb, void *data, void *hint, int tracked)
{
    QBNRef *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->env = env;
    r->kind = QBN_KIND_TRACKER;
    r->ownership = QBN_OWN_RUNTIME;
    r->value = JS_UNDEFINED;
    r->empty = 1;
    r->has_finalizer = 1;
    r->finalize_cb = cb;
    r->data = data;
    r->hint = hint;
    if (tracked) {
        list_add_tail(&r->link, &env->finalizing_reflist);
    } else {
        r->holds_env = 1;
        env->refs++;
    }
    return r;
}

static void qbn_drain_finalizers(napi_env env)
{
    while (!list_empty(&env->pending_finalizers)) {
        QBNRef *r = list_entry(env->pending_finalizers.next, QBNRef, pend_link);
        list_del(&r->pend_link);
        qbn_ref_finalize(r);
    }
}

static void qbn_finalize_all(struct list_head *list)
{
    while (!list_empty(list)) {
        QBNRef *r = list_entry(list->next, QBNRef, link);
        qbn_ref_finalize(r); /* unlinks r */
    }
}

static void qbn_timer_cb(uv_timer_t *h)
{
    QBNapiState *st = h->data;
    struct list_head *el, *el1;

    st->timer_active = 0;
    list_for_each_safe(el, el1, &st->envs)
    {
        napi_env env = list_entry(el, struct napi_env__, link);
        env->refs++;
        qbn_drain_finalizers(env);
        qbn_env_unref(env);
    }
    qb_drain(st->qr);
}

static void qbn_schedule_finalizers(QBNapiState *st)
{
    if (st->timer_active)
        return;
    st->timer_active = 1;
    uv_timer_start(&st->timer, qbn_timer_cb, 0, 0);
}

/* ---------------------------------------------------------- object data */

static void qbn_objdata_finalizer(JSRuntime *rt, JSValue val)
{
    QBNObjData *od = JS_GetOpaque(val, qbn_objdata_class_id);
    if (!od)
        return;
    if (od->wrap) {
        od->wrap->wrap_od = NULL;
        od->wrap = NULL;
    }
    while (!list_empty(&od->refs)) {
        QBNRef *r = list_entry(od->refs.next, QBNRef, obj_link);
        list_del(&r->obj_link);
        r->od = NULL;
        r->value = JS_UNDEFINED;
        r->empty = 1;
        qbn_ref_collected(r);
    }
    free(od);
}

static JSClassDef qbn_objdata_class = {
    "NapiObjectData",
    .finalizer = qbn_objdata_finalizer,
};

static int qbn_weakmap_init(napi_env env)
{
    QBNapiState *st = env->st;
    JSContext *ctx = env->ctx;
    JSValue global, ctor, proto;

    if (JS_IsObject(st->weakmap))
        return 0;
    global = JS_GetGlobalObject(ctx);
    ctor = JS_GetPropertyStr(ctx, global, "WeakMap");
    JS_FreeValue(ctx, global);
    if (JS_IsException(ctor))
        return -1;
    st->weakmap = JS_CallConstructor(ctx, ctor, 0, NULL);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(st->weakmap)) {
        st->weakmap = JS_UNDEFINED;
        return -1;
    }
    proto = JS_GetPrototype(ctx, st->weakmap);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, st->weakmap);
        st->weakmap = JS_UNDEFINED;
        return -1;
    }
    st->wm_get = JS_GetPropertyStr(ctx, proto, "get");
    st->wm_set = JS_GetPropertyStr(ctx, proto, "set");
    JS_FreeValue(ctx, proto);
    if (!JS_IsFunction(ctx, st->wm_get) || !JS_IsFunction(ctx, st->wm_set)) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, st->weakmap);
        JS_FreeValue(ctx, st->wm_get);
        JS_FreeValue(ctx, st->wm_set);
        st->weakmap = st->wm_get = st->wm_set = JS_UNDEFINED;
        return -1;
    }
    return 0;
}

/* the object data of 'obj' or NULL (never throws) */
static QBNObjData *qbn_objdata_get(napi_env env, JSValueConst obj)
{
    QBNapiState *st = env->st;
    JSContext *ctx = env->ctx;
    JSPropertyDescriptor desc;
    QBNObjData *od = NULL;
    JSValue h;
    int r;

    if (!JS_IsObject(obj))
        return NULL;
    r = JS_GetOwnProperty(ctx, &desc, obj, st->od_atom);
    if (r > 0) {
        od = JS_GetOpaque(desc.value, qbn_objdata_class_id);
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        return od;
    }
    if (r < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    if (JS_IsObject(st->weakmap)) {
        h = JS_Call(ctx, st->wm_get, st->weakmap, 1, &obj);
        if (JS_IsException(h)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return NULL;
        }
        od = JS_GetOpaque(h, qbn_objdata_class_id);
        JS_FreeValue(ctx, h);
    }
    return od;
}

/* the object data of 'obj', created if needed; NULL on failure */
static QBNObjData *qbn_objdata_ensure(napi_env env, JSValueConst obj)
{
    QBNapiState *st = env->st;
    JSContext *ctx = env->ctx;
    QBNObjData *od;
    JSValue holder, args[2], ret;
    int r;

    od = qbn_objdata_get(env, obj);
    if (od)
        return od;
    od = calloc(1, sizeof(*od));
    if (!od)
        return NULL;
    init_list_head(&od->refs);
    holder = JS_NewObjectProtoClass(ctx, JS_NULL, qbn_objdata_class_id);
    if (JS_IsException(holder)) {
        free(od);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    JS_SetOpaque(holder, od);
    r = JS_DefinePropertyValue(ctx, obj, st->od_atom, JS_DupValue(ctx, holder), 0);
    if (r > 0) {
        JS_FreeValue(ctx, holder);
        return od;
    }
    if (r < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    /* not extensible: keep the holder in the weak map */
    if (qbn_weakmap_init(env) == 0) {
        args[0] = (JSValue)obj;
        args[1] = holder;
        ret = JS_Call(ctx, st->wm_set, st->weakmap, 2, (JSValueConst *)args);
        if (!JS_IsException(ret)) {
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, holder);
            return od;
        }
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, holder); /* frees od */
    return NULL;
}

/* -------------------------------------------------------------- externals */

static void qbn_external_finalizer(JSRuntime *rt, JSValue val)
{
    QBNExternal *ext = JS_GetOpaque(val, qbn_external_class_id);
    if (!ext)
        return;
    if (ext->fin) {
        QBNRef *f = ext->fin;
        f->backref = NULL;
        ext->fin = NULL;
        qbn_enqueue_finalizer(f);
    }
    free(ext);
}

static JSClassDef qbn_external_class = {
    "External",
    .finalizer = qbn_external_finalizer,
};

static void qbn_cbholder_finalizer(JSRuntime *rt, JSValue val)
{
    free(JS_GetOpaque(val, qbn_cbholder_class_id));
}

static JSClassDef qbn_cbholder_class = {
    "NapiCallback",
    .finalizer = qbn_cbholder_finalizer,
};

/* ------------------------------------------------------------ functions */

static JSValue qbn_invoke(JSContext *ctx, QBNCallback *b, JSValueConst this_val,
                          JSValueConst new_target, int is_construct, int argc, JSValueConst *argv)
{
    napi_env env = b->env;
    QBNapiState *st = env->st;
    struct napi_callback_info__ info;
    struct napi_handle_scope__ scope;
    napi_value r;
    JSValue ret;

    if (env->has_pending)
        return qbn_rethrow(env);
    info.this_val = (JSValue)this_val;
    info.new_target = (JSValue)new_target;
    info.is_construct = is_construct;
    info.argc = argc;
    info.argv = argv;
    info.data = b->data;
    if (qbn_scope_open(env, &scope, 0, 0) < 0)
        return JS_ThrowOutOfMemory(ctx);
    st->js_depth++;
    qbn_clear_error(env);
    r = b->cb(env, &info);
    st->js_depth--;
    ret = r ? JS_DupValue(ctx, QBN_V(r)) : JS_UNDEFINED;
    qbn_scope_close(env, &scope);
    if (env->has_pending) {
        JS_FreeValue(ctx, ret);
        return qbn_rethrow(env);
    }
    return ret;
}

static JSValue qbn_closure_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                int magic, void *opaque)
{
    return qbn_invoke(ctx, opaque, this_val, JS_UNDEFINED, 0, argc, argv);
}

static void qbn_closure_free(void *opaque)
{
    free(opaque);
}

/* a JS function calling 'cb'; named after 'name' (JS_ATOM_NULL: no name,
   symbols are not used as names) */
static JSValue qbn_new_function(napi_env env, JSAtom name, napi_callback cb, void *data)
{
    JSContext *ctx = env->ctx;
    QBNCallback *b;
    JSValue f, nv;

    b = malloc(sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    b->env = env;
    b->cb = cb;
    b->data = data;
    /* no name here: the closure owns 'b' once created */
    f = JS_NewCClosure(ctx, qbn_closure_call, NULL, qbn_closure_free, 0, 0, b);
    if (JS_IsException(f)) {
        free(b);
        return f;
    }
    if (name != JS_ATOM_NULL) {
        nv = JS_AtomToValue(ctx, name);
        if (JS_IsException(nv)) {
            JS_FreeValue(ctx, f);
            return nv;
        }
        if (JS_IsString(nv)) {
            if (JS_DefinePropertyValueStr(ctx, f, "name", nv, JS_PROP_CONFIGURABLE) < 0) {
                JS_FreeValue(ctx, f);
                return JS_EXCEPTION;
            }
        } else {
            JS_FreeValue(ctx, nv);
        }
    }
    return f;
}

/* constructor of napi_define_class: a construct call creates 'this' from
   new.target.prototype */
static JSValue qbn_class_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                              int argc, JSValueConst *argv, int flags)
{
    QBRuntime *qr = qb_get(ctx);
    QBNapiState *st = qr->napi;
    JSPropertyDescriptor desc;
    QBNCallback *b = NULL;
    JSValue proto, obj, ret;

    if (st && JS_GetOwnProperty(ctx, &desc, func_obj, st->cb_atom) > 0) {
        b = JS_GetOpaque(desc.value, qbn_cbholder_class_id);
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
    }
    if (!b)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (!(flags & JS_CALL_FLAG_CONSTRUCTOR))
        return qbn_invoke(ctx, b, this_val, JS_UNDEFINED, 0, argc, argv);
    /* this_val is new.target */
    proto = JS_GetPropertyStr(ctx, this_val, "prototype");
    if (JS_IsException(proto))
        return proto;
    if (JS_IsObject(proto))
        obj = JS_NewObjectProto(ctx, proto);
    else
        obj = JS_NewObject(ctx);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    ret = qbn_invoke(ctx, b, obj, this_val, 1, argc, argv);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, obj);
        return ret;
    }
    if (JS_IsObject(ret)) {
        JS_FreeValue(ctx, obj);
        return ret;
    }
    JS_FreeValue(ctx, ret);
    return obj;
}

/* ------------------------------------------------------------- helpers */

/* ToObject(nv) (a new reference in *pobj) like Node's CHECK_TO_OBJECT */
static napi_status qbn_to_object(napi_env env, napi_value nv, JSValue *pobj)
{
    JSContext *ctx = env->ctx;
    JSValueConst v;
    JSValue o;

    if (!nv)
        return qbn_set_error(env, napi_invalid_arg);
    v = QBN_V(nv);
    if (JS_IsObject(v)) {
        *pobj = JS_DupValue(ctx, v);
        return napi_ok;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v))
        return qbn_set_error(env, napi_object_expected);
    o = JS_ToObject(ctx, v);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qbn_set_error(env, napi_object_expected);
    }
    *pobj = o;
    return napi_ok;
}

static JSValue qbn_global_prop(JSContext *ctx, const char *name)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return v;
}

/* the property key of a descriptor */
static napi_status qbn_desc_atom(napi_env env, const napi_property_descriptor *p, JSAtom *patom)
{
    JSContext *ctx = env->ctx;
    JSAtom a;

    if (p->utf8name) {
        a = JS_NewAtom(ctx, p->utf8name);
    } else {
        if (!p->name)
            return qbn_set_error(env, napi_name_expected);
        if (!JS_IsString(QBN_V(p->name)) && !JS_IsSymbol(QBN_V(p->name)))
            return qbn_set_error(env, napi_name_expected);
        a = JS_ValueToAtom(ctx, QBN_V(p->name));
    }
    if (a == JS_ATOM_NULL)
        return qbn_catch(env);
    *patom = a;
    return napi_ok;
}

/* define the property 'p' on 'obj' (napi_define_properties and the
   prototype template of napi_define_class) */
static napi_status qbn_define_property(napi_env env, JSValueConst obj,
                                       const napi_property_descriptor *p)
{
    JSContext *ctx = env->ctx;
    napi_status status;
    JSAtom atom;
    JSValue g = JS_UNDEFINED, s = JS_UNDEFINED, f;
    int flags, r;

    status = qbn_desc_atom(env, p, &atom);
    if (status != napi_ok)
        return status;
    flags = JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;
    if (p->attributes & napi_enumerable)
        flags |= JS_PROP_ENUMERABLE;
    if (p->attributes & napi_configurable)
        flags |= JS_PROP_CONFIGURABLE;
    if (p->getter || p->setter) {
        if (p->getter) {
            g = qbn_new_function(env, atom, p->getter, p->data);
            if (JS_IsException(g)) {
                JS_FreeAtom(ctx, atom);
                return qbn_catch(env);
            }
        }
        if (p->setter) {
            s = qbn_new_function(env, atom, p->setter, p->data);
            if (JS_IsException(s)) {
                JS_FreeValue(ctx, g);
                JS_FreeAtom(ctx, atom);
                return qbn_catch(env);
            }
        }
        r = JS_DefineProperty(ctx, obj, atom, JS_UNDEFINED, g, s,
                              flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET);
        JS_FreeValue(ctx, g);
        JS_FreeValue(ctx, s);
        JS_FreeAtom(ctx, atom);
        if (r < 0)
            return qbn_catch(env);
        if (r == 0)
            return qbn_set_error(env, napi_invalid_arg);
        return napi_ok;
    }
    flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
    if (p->attributes & napi_writable)
        flags |= JS_PROP_WRITABLE;
    if (p->method) {
        f = qbn_new_function(env, atom, p->method, p->data);
        if (JS_IsException(f)) {
            JS_FreeAtom(ctx, atom);
            return qbn_catch(env);
        }
        r = JS_DefineProperty(ctx, obj, atom, f, JS_UNDEFINED, JS_UNDEFINED, flags);
        JS_FreeValue(ctx, f);
        JS_FreeAtom(ctx, atom);
        if (r < 0)
            return qbn_catch(env);
        if (r == 0)
            return qbn_set_error(env, napi_generic_failure);
        return napi_ok;
    }
    r = JS_DefineProperty(ctx, obj, atom, p->value ? QBN_V(p->value) : JS_UNDEFINED, JS_UNDEFINED,
                          JS_UNDEFINED, flags);
    JS_FreeAtom(ctx, atom);
    if (r < 0)
        return qbn_catch(env);
    if (r == 0)
        return qbn_set_error(env, napi_invalid_arg);
    return napi_ok;
}

/* ===================================================================== */
/*                           js_native_api.h                              */
/* ===================================================================== */

NAPI_EXTERN napi_status NAPI_CDECL napi_get_last_error_info(node_api_basic_env basic_env,
                                                            const napi_extended_error_info **result)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    if ((unsigned)env->last_error.error_code > napi_cannot_run_js)
        env->last_error.error_code = napi_generic_failure;
    env->last_error.error_message = qbn_error_messages[env->last_error.error_code];
    if (env->last_error.error_code == napi_ok)
        qbn_clear_error(env);
    *result = &env->last_error;
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = QBN_NV(&env->c_undefined);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = QBN_NV(&env->c_null);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_GetGlobalObject(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = QBN_NV(value ? &env->c_true : &env->c_false);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewObject(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_object_with_properties(
    napi_env env, napi_value prototype_or_null, napi_value *property_names,
    napi_value *property_values, size_t property_count, napi_value *result)
{
    JSContext *ctx;
    JSValue proto, obj;
    size_t i;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    if (property_count > 0) {
        QBN_CHECK_ARG(env, property_names);
        QBN_CHECK_ARG(env, property_values);
    }
    ctx = env->ctx;
    proto = prototype_or_null ? QBN_V(prototype_or_null) : JS_NULL;
    if (!JS_IsObject(proto) && !JS_IsNull(proto))
        return qbn_set_error(env, napi_invalid_arg);
    for (i = 0; i < property_count; i++) {
        JSValueConst k;
        QBN_CHECK_ARG(env, property_names[i]);
        QBN_CHECK_ARG(env, property_values[i]);
        k = QBN_V(property_names[i]);
        QBN_RETURN_IF_FALSE(env, JS_IsString(k) || JS_IsSymbol(k), napi_name_expected);
    }
    obj = JS_NewObjectProto(ctx, proto);
    if (JS_IsException(obj))
        return qbn_catch(env);
    for (i = 0; i < property_count; i++) {
        JSAtom a = JS_ValueToAtom(ctx, QBN_V(property_names[i]));
        int r;
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            return qbn_catch(env);
        }
        r = JS_DefinePropertyValue(ctx, obj, a, JS_DupValue(ctx, QBN_V(property_values[i])),
                                   JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
        if (r < 0) {
            JS_FreeValue(ctx, obj);
            return qbn_catch(env);
        }
    }
    return qbn_result(env, obj, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewArray(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array_with_length(napi_env env, size_t length,
                                                                 napi_value *result)
{
    JSContext *ctx;
    JSValue a;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return qbn_catch(env);
    /* v8::Array::New(isolate, int length): negative lengths give [] */
    if (length > 0 && length <= INT_MAX && JS_SetLength(ctx, a, (int64_t)length) < 0) {
        JS_FreeValue(ctx, a);
        return qbn_catch(env);
    }
    return qbn_result(env, a, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_double(napi_env env, double value,
                                                      napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewFloat64(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value,
                                                     napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewInt32(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value,
                                                      napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewUint32(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value,
                                                     napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewFloat64(env->ctx, (double)value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_latin1(napi_env env, const char *str,
                                                             size_t length, napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_latin1(env->ctx, str, length), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(napi_env env, const char *str,
                                                           size_t length, napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_utf8(env->ctx, str, length), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf16(napi_env env, const char16_t *str,
                                                            size_t length, napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_utf16(env->ctx, str, length), result);
}

/* external strings are copied: the finalizer runs at once (Node's
   behavior when external strings are not available) */
NAPI_EXTERN napi_status NAPI_CDECL node_api_create_external_string_latin1(
    napi_env env, char *str, size_t length, node_api_basic_finalize finalize_callback,
    void *finalize_hint, napi_value *result, bool *copied)
{
    napi_status status;
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    status = qbn_result(env, qbn_new_latin1(env->ctx, str, length), result);
    if (status == napi_ok) {
        if (copied)
            *copied = true;
        if (finalize_callback)
            qbn_call_finalizer(env, QBN_FINALIZE(finalize_callback), str, finalize_hint);
        qbn_clear_error(env);
    }
    return status;
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_external_string_utf16(
    napi_env env, char16_t *str, size_t length, node_api_basic_finalize finalize_callback,
    void *finalize_hint, napi_value *result, bool *copied)
{
    napi_status status;
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    status = qbn_result(env, qbn_new_utf16(env->ctx, str, length), result);
    if (status == napi_ok) {
        if (copied)
            *copied = true;
        if (finalize_callback)
            qbn_call_finalizer(env, QBN_FINALIZE(finalize_callback), str, finalize_hint);
        qbn_clear_error(env);
    }
    return status;
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_latin1(napi_env env,
                                                                       const char *str,
                                                                       size_t length,
                                                                       napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_latin1(env->ctx, str, length), result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_utf8(napi_env env, const char *str,
                                                                     size_t length,
                                                                     napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_utf8(env->ctx, str, length), result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_utf16(napi_env env,
                                                                      const char16_t *str,
                                                                      size_t length,
                                                                      napi_value *result)
{
    QBN_CHECK_NEW_STRING_ARGS(env, str, length, result);
    return qbn_result(env, qbn_new_utf16(env->ctx, str, length), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_symbol(napi_env env, napi_value description,
                                                      napi_value *result)
{
    JSContext *ctx;
    const char *s;
    JSValue sym;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    if (!description)
        return qbn_result(env, JS_NewSymbol(ctx, NULL, FALSE), result);
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(description)), napi_string_expected);
    s = JS_ToCString(ctx, QBN_V(description));
    if (!s)
        return qbn_catch(env);
    sym = JS_NewSymbol(ctx, s, FALSE);
    JS_FreeCString(ctx, s);
    return qbn_result(env, sym, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_symbol_for(napi_env env, const char *utf8description,
                                                       size_t length, napi_value *result)
{
    JSContext *ctx;
    char *buf;
    JSValue sym;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, utf8description);
    QBN_RETURN_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= INT_MAX, napi_invalid_arg);
    ctx = env->ctx;
    if (length == NAPI_AUTO_LENGTH)
        length = strlen(utf8description);
    buf = malloc(length + 1);
    if (!buf)
        return qbn_set_error(env, napi_generic_failure);
    memcpy(buf, utf8description, length);
    buf[length] = '\0';
    sym = JS_NewSymbol(ctx, buf, TRUE);
    free(buf);
    return qbn_result(env, sym, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_function(napi_env env, const char *utf8name,
                                                        size_t length, napi_callback cb, void *data,
                                                        napi_value *result)
{
    JSContext *ctx;
    JSAtom name = JS_ATOM_NULL;
    JSValue f;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, cb);
    ctx = env->ctx;
    if (utf8name) {
        QBN_RETURN_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= INT_MAX, napi_invalid_arg);
        name = JS_NewAtomLen(ctx, utf8name, length == NAPI_AUTO_LENGTH ? strlen(utf8name) : length);
        if (name == JS_ATOM_NULL)
            return qbn_catch(env);
    }
    f = qbn_new_function(env, name, cb, data);
    if (name != JS_ATOM_NULL)
        JS_FreeAtom(ctx, name);
    return qbn_result(env, f, result);
}

static napi_status qbn_create_error(napi_env env, JSErrorEnum kind, napi_value code, napi_value msg,
                                    napi_value *result)
{
    JSContext *ctx;
    const char *s;
    size_t len;
    JSValue e;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, msg);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(msg)), napi_string_expected);
    if (code)
        QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(code)), napi_string_expected);
    s = JS_ToCStringLen(ctx, &len, QBN_V(msg));
    if (!s)
        return qbn_catch(env);
    e = JS_NewErrorLen(ctx, kind, s, len);
    JS_FreeCString(ctx, s);
    if (JS_IsException(e))
        return qbn_catch(env);
    if (code && JS_DefinePropertyValueStr(ctx, e, "code", JS_DupValue(ctx, QBN_V(code)),
                                          JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qbn_set_error(env, napi_generic_failure);
    }
    return qbn_result(env, e, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code, napi_value msg,
                                                     napi_value *result)
{
    return qbn_create_error(env, JS_PLAIN_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code,
                                                          napi_value msg, napi_value *result)
{
    return qbn_create_error(env, JS_TYPE_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code,
                                                           napi_value msg, napi_value *result)
{
    return qbn_create_error(env, JS_RANGE_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_syntax_error(napi_env env, napi_value code,
                                                                napi_value msg, napi_value *result)
{
    return qbn_create_error(env, JS_SYNTAX_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value,
                                               napi_valuetype *result)
{
    JSValueConst v;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    switch (JS_VALUE_GET_NORM_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
        *result = napi_number;
        break;
    case JS_TAG_BIG_INT:
    case JS_TAG_SHORT_BIG_INT:
        *result = napi_bigint;
        break;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        *result = napi_string;
        break;
    case JS_TAG_SYMBOL:
        *result = napi_symbol;
        break;
    case JS_TAG_BOOL:
        *result = napi_boolean;
        break;
    case JS_TAG_NULL:
        *result = napi_null;
        break;
    case JS_TAG_UNDEFINED:
        *result = napi_undefined;
        break;
    case JS_TAG_OBJECT:
        if (JS_IsFunction(env->ctx, v))
            *result = napi_function;
        else if (JS_GetClassID(v) == qbn_external_class_id)
            *result = napi_external;
        else
            *result = napi_object;
        break;
    default:
        return qbn_set_error(env, napi_invalid_arg);
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_double(napi_env env, napi_value value,
                                                         double *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    QBN_RETURN_IF_FALSE(env, JS_IsNumber(QBN_V(value)), napi_number_expected);
    JS_ToFloat64(env->ctx, result, QBN_V(value));
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int32(napi_env env, napi_value value,
                                                        int32_t *result)
{
    JSValueConst v;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    QBN_RETURN_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        *result = JS_VALUE_GET_INT(v);
    else
        JS_ToInt32(env->ctx, result, v);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_uint32(napi_env env, napi_value value,
                                                         uint32_t *result)
{
    JSValueConst v;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    QBN_RETURN_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        *result = (uint32_t)JS_VALUE_GET_INT(v);
    else
        JS_ToUint32(env->ctx, result, v);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int64(napi_env env, napi_value value,
                                                        int64_t *result)
{
    JSValueConst v;
    double d;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    QBN_RETURN_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *result = JS_VALUE_GET_INT(v);
    } else {
        JS_ToFloat64(env->ctx, &d, v);
        /* non-finite: 0; out of range: saturated (V8's IntegerValue) */
        if (!isfinite(d))
            *result = 0;
        else if (d >= 9223372036854775808.0)
            *result = INT64_MAX;
        else if (d <= -9223372036854775808.0)
            *result = INT64_MIN;
        else
            *result = (int64_t)d;
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bool(napi_env env, napi_value value, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    QBN_RETURN_IF_FALSE(env, JS_IsBool(QBN_V(value)), napi_boolean_expected);
    *result = JS_VALUE_GET_BOOL(QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_latin1(napi_env env, napi_value value,
                                                                char *buf, size_t bufsize,
                                                                size_t *result)
{
    const void *data;
    size_t len, n, i;
    int wide;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(value)), napi_string_expected);
    if (qbn_string_data(env, QBN_V(value), &data, &len, &wide) < 0)
        return qbn_set_error(env, napi_generic_failure);
    if (!buf) {
        QBN_CHECK_ARG(env, result);
        *result = len;
    } else if (bufsize != 0) {
        n = len < bufsize - 1 ? len : bufsize - 1;
        if (wide) {
            const uint16_t *s = data;
            for (i = 0; i < n; i++)
                buf[i] = (char)(s[i] & 0xff);
        } else {
            memcpy(buf, data, n);
        }
        buf[n] = '\0';
        if (result)
            *result = n;
    } else if (result) {
        *result = 0;
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf8(napi_env env, napi_value value,
                                                              char *buf, size_t bufsize,
                                                              size_t *result)
{
    const void *data;
    size_t len, n;
    int wide;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(value)), napi_string_expected);
    if (qbn_string_data(env, QBN_V(value), &data, &len, &wide) < 0)
        return qbn_set_error(env, napi_generic_failure);
    if (!buf) {
        QBN_CHECK_ARG(env, result);
        *result = qbn_utf8_encode(data, len, wide, NULL, SIZE_MAX);
    } else if (bufsize != 0) {
        n = qbn_utf8_encode(data, len, wide, buf, bufsize - 1);
        buf[n] = '\0';
        if (result)
            *result = n;
    } else if (result) {
        *result = 0;
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env, napi_value value,
                                                               char16_t *buf, size_t bufsize,
                                                               size_t *result)
{
    const void *data;
    size_t len, n, i;
    int wide;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(value)), napi_string_expected);
    if (qbn_string_data(env, QBN_V(value), &data, &len, &wide) < 0)
        return qbn_set_error(env, napi_generic_failure);
    if (!buf) {
        QBN_CHECK_ARG(env, result);
        *result = len;
    } else if (bufsize != 0) {
        n = len < bufsize - 1 ? len : bufsize - 1;
        if (wide) {
            memcpy(buf, data, n * sizeof(uint16_t));
        } else {
            const uint8_t *s = data;
            for (i = 0; i < n; i++)
                buf[i] = s[i];
        }
        buf[n] = 0;
        if (result)
            *result = n;
    } else if (result) {
        *result = 0;
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value,
                                                       napi_value *result)
{
    int b;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    b = JS_ToBool(env->ctx, QBN_V(value));
    if (b < 0)
        return qbn_catch(env);
    *result = QBN_NV(b ? &env->c_true : &env->c_false);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_number(napi_env env, napi_value value,
                                                         napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_ToNumber(env->ctx, QBN_V(value)), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_object(napi_env env, napi_value value,
                                                         napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_ToObject(env->ctx, QBN_V(value)), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_string(napi_env env, napi_value value,
                                                         napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_ToString(env->ctx, QBN_V(value)), result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_set_prototype(napi_env env, napi_value object,
                                                          napi_value value)
{
    napi_status status;
    JSValue obj;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    r = JS_SetPrototype(env->ctx, obj, QBN_V(value));
    JS_FreeValue(env->ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    if (r == 0)
        return qbn_set_error(env, napi_generic_failure);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_prototype(napi_env env, napi_value object,
                                                      napi_value *result)
{
    napi_status status;
    JSValue obj, proto;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    proto = JS_GetPrototype(env->ctx, obj);
    JS_FreeValue(env->ctx, obj);
    return qbn_result(env, proto, result);
}

/* ------------------------------------------------------- property names */

typedef struct QBNAtomSet {
    JSAtom *tab;
    uint32_t size; /* power of 2 */
    uint32_t count;
} QBNAtomSet;

static int qbn_atomset_add(QBNAtomSet *s, JSContext *ctx, JSAtom a)
{
    uint32_t i, mask;

    if ((s->count + 1) * 2 > s->size) {
        uint32_t nsize = s->size ? s->size * 2 : 32, j;
        JSAtom *nt = calloc(nsize, sizeof(JSAtom));
        if (!nt)
            return -1;
        for (j = 0; j < s->size; j++) {
            JSAtom b = s->tab[j];
            if (b != JS_ATOM_NULL) {
                i = (b * 2654435761u) & (nsize - 1);
                while (nt[i] != JS_ATOM_NULL)
                    i = (i + 1) & (nsize - 1);
                nt[i] = b;
            }
        }
        free(s->tab);
        s->tab = nt;
        s->size = nsize;
    }
    mask = s->size - 1;
    i = (a * 2654435761u) & mask;
    while (s->tab[i] != JS_ATOM_NULL) {
        if (s->tab[i] == a)
            return 0;
        i = (i + 1) & mask;
    }
    s->tab[i] = JS_DupAtom(ctx, a);
    s->count++;
    return 1;
}

static void qbn_atomset_free(QBNAtomSet *s, JSContext *ctx)
{
    uint32_t i;
    for (i = 0; i < s->size; i++) {
        if (s->tab[i] != JS_ATOM_NULL)
            JS_FreeAtom(ctx, s->tab[i]);
    }
    free(s->tab);
}

/* canonical array index string -> number (napi_key_keep_numbers) */
static int qbn_index_of_string(JSContext *ctx, JSValueConst str, uint32_t *pidx)
{
    const void *p;
    const uint8_t *s;
    size_t len, i;
    JS_BOOL wide;
    uint64_t v = 0;

    p = JS_GetStringData(ctx, str, &len, &wide);
    if (!p || wide || len == 0 || len > 10)
        return 0;
    s = p;
    if (s[0] == '0' && len > 1)
        return 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 4294967294u)
        return 0;
    *pidx = (uint32_t)v;
    return 1;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_all_property_names(napi_env env, napi_value object,
                                                               napi_key_collection_mode key_mode,
                                                               napi_key_filter key_filter,
                                                               napi_key_conversion key_conversion,
                                                               napi_value *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj, cur, arr, key, next;
    JSPropertyEnum *tab;
    uint32_t len, i, idx = 0, n;
    QBNAtomSet seen = {NULL, 0, 0};
    int include_proto, gpn, need_desc, keep;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    if (key_mode != napi_key_include_prototypes && key_mode != napi_key_own_only)
        return qbn_set_error(env, napi_invalid_arg);
    if (key_conversion != napi_key_keep_numbers && key_conversion != napi_key_numbers_to_strings)
        return qbn_set_error(env, napi_invalid_arg);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    include_proto = key_mode == napi_key_include_prototypes;
    gpn = 0;
    if (!(key_filter & napi_key_skip_strings))
        gpn |= JS_GPN_STRING_MASK;
    if (!(key_filter & napi_key_skip_symbols))
        gpn |= JS_GPN_SYMBOL_MASK;
    if (key_filter & napi_key_enumerable)
        gpn |= include_proto ? JS_GPN_SET_ENUM : JS_GPN_ENUM_ONLY;
    need_desc = (key_filter & (napi_key_writable | napi_key_configurable)) != 0;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    cur = obj;
    while (gpn & (JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, cur, gpn) < 0)
            goto exception;
        for (i = 0; i < len; i++) {
            JSAtom a = tab[i].atom;
            if (include_proto) {
                int added = qbn_atomset_add(&seen, ctx, a);
                if (added < 0) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    JS_ThrowOutOfMemory(ctx);
                    goto exception;
                }
                if (added == 0)
                    continue; /* shadowed by an earlier object */
                if ((key_filter & napi_key_enumerable) && !tab[i].is_enumerable)
                    continue;
            }
            if (need_desc) {
                JSPropertyDescriptor desc;
                int r = JS_GetOwnProperty(ctx, &desc, cur, a);
                if (r < 0) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    goto exception;
                }
                if (r == 0)
                    continue;
                JS_FreeValue(ctx, desc.value);
                JS_FreeValue(ctx, desc.getter);
                JS_FreeValue(ctx, desc.setter);
                keep = 1;
                if ((key_filter & napi_key_writable) && !(desc.flags & JS_PROP_GETSET) &&
                    !(desc.flags & JS_PROP_WRITABLE))
                    keep = 0;
                if ((key_filter & napi_key_configurable) && !(desc.flags & JS_PROP_CONFIGURABLE))
                    keep = 0;
                if (!keep)
                    continue;
            }
            key = JS_AtomToValue(ctx, a);
            if (JS_IsException(key)) {
                JS_FreePropertyEnum(ctx, tab, len);
                goto exception;
            }
            if (key_conversion == napi_key_keep_numbers && JS_IsString(key) &&
                qbn_index_of_string(ctx, key, &n)) {
                JS_FreeValue(ctx, key);
                key = JS_NewUint32(ctx, n);
            }
            if (JS_DefinePropertyValueUint32(ctx, arr, idx++, key, JS_PROP_C_W_E) < 0) {
                JS_FreePropertyEnum(ctx, tab, len);
                goto exception;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
        if (!include_proto)
            break;
        next = JS_GetPrototype(ctx, cur);
        if (JS_IsException(next))
            goto exception;
        JS_FreeValue(ctx, cur);
        cur = next;
        if (!JS_IsObject(cur))
            break;
    }
    JS_FreeValue(ctx, cur);
    qbn_atomset_free(&seen, ctx);
    return qbn_result(env, arr, result);
exception:
    JS_FreeValue(ctx, cur);
    JS_FreeValue(ctx, arr);
    qbn_atomset_free(&seen, ctx);
    return qbn_catch(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(napi_env env, napi_value object,
                                                           napi_value *result)
{
    return napi_get_all_property_names(
        env, object, napi_key_include_prototypes,
        (napi_key_filter)(napi_key_enumerable | napi_key_skip_symbols), napi_key_numbers_to_strings,
        result);
}

/* ----------------------------------------------------------- properties */

NAPI_EXTERN napi_status NAPI_CDECL napi_set_property(napi_env env, napi_value object,
                                                     napi_value key, napi_value value)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, key);
    QBN_CHECK_ARG(env, value);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_ValueToAtom(ctx, QBN_V(key));
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_SetPropertyInternal(ctx, obj, a, JS_DupValue(ctx, QBN_V(value)), obj, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_property(napi_env env, napi_value object,
                                                     napi_value key, bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, key);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_ValueToAtom(ctx, QBN_V(key));
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_HasProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(napi_env env, napi_value object,
                                                     napi_value key, napi_value *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj, v;
    JSAtom a;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, key);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_ValueToAtom(ctx, QBN_V(key));
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    v = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    return qbn_result(env, v, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(napi_env env, napi_value object,
                                                        napi_value key, bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, key);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_ValueToAtom(ctx, QBN_V(key));
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_DeleteProperty(ctx, obj, a, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    if (result)
        *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_own_property(napi_env env, napi_value object,
                                                         napi_value key, bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, key);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    if (!JS_IsString(QBN_V(key)) && !JS_IsSymbol(QBN_V(key))) {
        JS_FreeValue(ctx, obj);
        return qbn_set_error(env, napi_name_expected);
    }
    a = JS_ValueToAtom(ctx, QBN_V(key));
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_GetOwnProperty(ctx, NULL, obj, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object,
                                                           const char *utf8name, napi_value value)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, utf8name);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_NewAtom(ctx, utf8name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_SetPropertyInternal(ctx, obj, a, JS_DupValue(ctx, QBN_V(value)), obj, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_named_property(napi_env env, napi_value object,
                                                           const char *utf8name, bool *result)
{
    napi_status status;
    JSValue obj;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, utf8name);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    r = JS_HasPropertyStrLen(env->ctx, obj, utf8name, strlen(utf8name));
    JS_FreeValue(env->ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object,
                                                           const char *utf8name, napi_value *result)
{
    napi_status status;
    JSValue obj, v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, utf8name);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    v = JS_GetPropertyStrLen(env->ctx, obj, utf8name, strlen(utf8name));
    JS_FreeValue(env->ctx, obj);
    return qbn_result(env, v, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(napi_env env, napi_value object, uint32_t index,
                                                    napi_value value)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_NewAtomUInt32(ctx, index);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_SetPropertyInternal(ctx, obj, a, JS_DupValue(ctx, QBN_V(value)), obj, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_element(napi_env env, napi_value object, uint32_t index,
                                                    bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_NewAtomUInt32(ctx, index);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_HasProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(napi_env env, napi_value object, uint32_t index,
                                                    napi_value *result)
{
    napi_status status;
    JSValue obj, v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    v = JS_GetPropertyUint32(env->ctx, obj, index);
    JS_FreeValue(env->ctx, obj);
    return qbn_result(env, v, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_element(napi_env env, napi_value object,
                                                       uint32_t index, bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj;
    JSAtom a;
    int r;

    QBN_PREAMBLE(env);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    a = JS_NewAtomUInt32(ctx, index);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, obj);
        return qbn_catch(env);
    }
    r = JS_DeleteProperty(ctx, obj, a, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    if (result)
        *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_define_properties(napi_env env, napi_value object, size_t property_count,
                       const napi_property_descriptor *properties)
{
    napi_status status;
    JSValue obj;
    size_t i;

    QBN_PREAMBLE(env);
    if (property_count > 0)
        QBN_CHECK_ARG(env, properties);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    for (i = 0; i < property_count; i++) {
        status = qbn_define_property(env, obj, &properties[i]);
        if (status != napi_ok) {
            JS_FreeValue(env->ctx, obj);
            return status;
        }
    }
    JS_FreeValue(env->ctx, obj);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool *result)
{
    int r;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    r = JS_IsArray(env->ctx, QBN_V(value));
    if (r < 0) {
        JS_FreeValue(env->ctx, JS_GetException(env->ctx));
        r = 0;
    }
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_array_length(napi_env env, napi_value value,
                                                         uint32_t *result)
{
    int64_t len;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    r = JS_IsArray(env->ctx, QBN_V(value));
    if (r < 0)
        return qbn_catch(env);
    QBN_RETURN_IF_FALSE(env, r, napi_array_expected);
    if (JS_GetLength(env->ctx, QBN_V(value), &len) < 0)
        return qbn_catch(env);
    *result = (uint32_t)len;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs,
                                                      bool *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, lhs);
    QBN_CHECK_ARG(env, rhs);
    QBN_CHECK_ARG(env, result);
    *result = JS_StrictEq(env->ctx, QBN_V(lhs), QBN_V(rhs)) != 0;
    return qbn_clear_error(env);
}

/* ------------------------------------------------------------ functions */

#define QBN_STACK_ARGS 8

NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(napi_env env, napi_value recv,
                                                      napi_value func, size_t argc,
                                                      const napi_value *argv, napi_value *result)
{
    JSContext *ctx;
    JSValue stack_args[QBN_STACK_ARGS], *args = stack_args, ret;
    size_t i;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, recv);
    if (argc > 0)
        QBN_CHECK_ARG(env, argv);
    QBN_CHECK_ARG(env, func);
    ctx = env->ctx;
    QBN_RETURN_IF_FALSE(env, JS_IsFunction(ctx, QBN_V(func)), napi_invalid_arg);
    QBN_RETURN_IF_FALSE(env, argc <= INT_MAX, napi_invalid_arg);
    if (argc > QBN_STACK_ARGS) {
        args = malloc(argc * sizeof(JSValue));
        if (!args)
            return qbn_set_error(env, napi_generic_failure);
    }
    for (i = 0; i < argc; i++)
        args[i] = argv[i] ? QBN_V(argv[i]) : JS_UNDEFINED;
    ret = JS_Call(ctx, QBN_V(func), QBN_V(recv), (int)argc, (JSValueConst *)args);
    if (args != stack_args)
        free(args);
    if (JS_IsException(ret))
        return qbn_catch(env);
    if (result)
        return qbn_result(env, ret, result);
    JS_FreeValue(ctx, ret);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_new_instance(napi_env env, napi_value constructor,
                                                     size_t argc, const napi_value *argv,
                                                     napi_value *result)
{
    JSContext *ctx;
    JSValue stack_args[QBN_STACK_ARGS], *args = stack_args, ret;
    size_t i;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, constructor);
    if (argc > 0)
        QBN_CHECK_ARG(env, argv);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    QBN_RETURN_IF_FALSE(env, JS_IsFunction(ctx, QBN_V(constructor)), napi_invalid_arg);
    QBN_RETURN_IF_FALSE(env, argc <= INT_MAX, napi_invalid_arg);
    if (argc > QBN_STACK_ARGS) {
        args = malloc(argc * sizeof(JSValue));
        if (!args)
            return qbn_set_error(env, napi_generic_failure);
    }
    for (i = 0; i < argc; i++)
        args[i] = argv[i] ? QBN_V(argv[i]) : JS_UNDEFINED;
    ret = JS_CallConstructor(ctx, QBN_V(constructor), (int)argc, (JSValueConst *)args);
    if (args != stack_args)
        free(args);
    return qbn_result(env, ret, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_instanceof(napi_env env, napi_value object,
                                                   napi_value constructor, bool *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue ctor;
    int r;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, object);
    QBN_CHECK_ARG(env, result);
    *result = false;
    status = qbn_to_object(env, constructor, &ctor);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        napi_throw_type_error(env, "ERR_NAPI_CONS_FUNCTION", "Constructor must be a function");
        return qbn_set_error(env, napi_function_expected);
    }
    r = JS_IsInstanceOf(ctx, QBN_V(object), ctor);
    JS_FreeValue(ctx, ctor);
    if (r < 0)
        return qbn_catch(env);
    *result = r != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_cb_info(napi_env env, napi_callback_info cbinfo,
                                                    size_t *argc, napi_value *argv,
                                                    napi_value *this_arg, void **data)
{
    size_t i, n;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, cbinfo);
    if (argv) {
        QBN_CHECK_ARG(env, argc);
        n = *argc < (size_t)cbinfo->argc ? *argc : (size_t)cbinfo->argc;
        for (i = 0; i < n; i++)
            argv[i] = QBN_NV((JSValue *)&cbinfo->argv[i]);
        for (; i < *argc; i++)
            argv[i] = QBN_NV(&env->c_undefined);
    }
    if (argc)
        *argc = cbinfo->argc;
    if (this_arg)
        *this_arg = QBN_NV(&cbinfo->this_val);
    if (data)
        *data = cbinfo->data;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_new_target(napi_env env, napi_callback_info cbinfo,
                                                       napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, cbinfo);
    QBN_CHECK_ARG(env, result);
    *result = cbinfo->is_construct ? QBN_NV(&cbinfo->new_target) : NULL;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_define_class(napi_env env, const char *utf8name,
                                                     size_t length, napi_callback constructor,
                                                     void *data, size_t property_count,
                                                     const napi_property_descriptor *properties,
                                                     napi_value *result)
{
    JSContext *ctx;
    QBNCallback *b;
    JSCFunctionType ft;
    JSValue ctor, holder, proto, name;
    napi_value nv;
    napi_status status;
    size_t i;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, constructor);
    if (property_count > 0)
        QBN_CHECK_ARG(env, properties);
    QBN_CHECK_ARG(env, utf8name);
    QBN_RETURN_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= INT_MAX, napi_invalid_arg);
    ctx = env->ctx;
    b = malloc(sizeof(*b));
    if (!b)
        return qbn_set_error(env, napi_generic_failure);
    b->env = env;
    b->cb = constructor;
    b->data = data;
    ft.constructor_or_func_flags = qbn_class_call;
    ctor = JS_NewCFunction2(ctx, ft.generic, NULL, 0, JS_CFUNC_constructor_or_func_flags, 0);
    if (JS_IsException(ctor)) {
        free(b);
        return qbn_catch(env);
    }
    holder = JS_NewObjectProtoClass(ctx, JS_NULL, qbn_cbholder_class_id);
    if (JS_IsException(holder)) {
        free(b);
        JS_FreeValue(ctx, ctor);
        return qbn_catch(env);
    }
    JS_SetOpaque(holder, b);
    /* the holder (and 'b') lives as long as the constructor */
    if (JS_DefinePropertyValue(ctx, ctor, env->st->cb_atom, holder, 0) < 0)
        goto exception;
    name = qbn_new_utf8(ctx, utf8name, length);
    if (JS_IsException(name))
        goto exception;
    if (JS_DefinePropertyValueStr(ctx, ctor, "name", name, JS_PROP_CONFIGURABLE) < 0)
        goto exception;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        goto exception;
    if (JS_SetConstructor(ctx, ctor, proto) < 0) {
        JS_FreeValue(ctx, proto);
        goto exception;
    }
    for (i = 0; i < property_count; i++) {
        if (properties[i].attributes & napi_static)
            continue;
        status = qbn_define_property(env, proto, &properties[i]);
        if (status != napi_ok) {
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, ctor);
            return status;
        }
    }
    JS_FreeValue(ctx, proto);
    nv = qbn_push(env, ctor);
    if (!nv)
        return qbn_set_error(env, napi_generic_failure);
    for (i = 0; i < property_count; i++) {
        if (!(properties[i].attributes & napi_static))
            continue;
        status = qbn_define_property(env, QBN_V(nv), &properties[i]);
        if (status != napi_ok)
            return status;
    }
    *result = nv;
    return qbn_clear_error(env);
exception:
    JS_FreeValue(ctx, ctor);
    return qbn_catch(env);
}

/* ---------------------------------------------------------------- wrap */

static napi_status qbn_wrap(napi_env env, napi_value js_object, void *native_object,
                            napi_finalize finalize_cb, void *finalize_hint, napi_ref *result)
{
    QBNObjData *od;
    QBNRef *r;
    JSValueConst v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, js_object);
    v = QBN_V(js_object);
    QBN_RETURN_IF_FALSE(env, JS_IsObject(v), napi_invalid_arg);
    if (result)
        QBN_CHECK_ARG(env, finalize_cb);
    od = qbn_objdata_ensure(env, v);
    if (!od)
        return qbn_set_error(env, napi_generic_failure);
    QBN_RETURN_IF_FALSE(env, !od->wrap, napi_invalid_arg);
    r = qbn_ref_new(env, v, 0, result ? QBN_OWN_USERLAND : QBN_OWN_RUNTIME, finalize_cb != NULL,
                    finalize_cb, native_object, finalize_hint);
    if (!r)
        return qbn_set_error(env, napi_generic_failure);
    r->wrap_od = od;
    od->wrap = r;
    if (result)
        *result = (napi_ref)r;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_wrap(napi_env env, napi_value js_object,
                                             void *native_object,
                                             node_api_basic_finalize finalize_cb,
                                             void *finalize_hint, napi_ref *result)
{
    return qbn_wrap(env, js_object, native_object, QBN_FINALIZE(finalize_cb), finalize_hint,
                    result);
}

static napi_status qbn_unwrap(napi_env env, napi_value js_object, void **result, int remove)
{
    QBNObjData *od;
    QBNRef *r;
    JSValueConst v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, js_object);
    if (!remove)
        QBN_CHECK_ARG(env, result);
    v = QBN_V(js_object);
    QBN_RETURN_IF_FALSE(env, JS_IsObject(v), napi_invalid_arg);
    od = qbn_objdata_get(env, v);
    QBN_RETURN_IF_FALSE(env, od && od->wrap, napi_invalid_arg);
    r = od->wrap;
    if (result)
        *result = r->data;
    if (remove) {
        od->wrap = NULL;
        r->wrap_od = NULL;
        if (r->ownership == QBN_OWN_USERLAND) {
            /* the reference stays, without its finalizer */
            r->finalize_cb = NULL;
            r->data = NULL;
            r->hint = NULL;
        } else {
            qbn_ref_free(r);
        }
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void **result)
{
    return qbn_unwrap(env, js_object, result, 0);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object,
                                                    void **result)
{
    return qbn_unwrap(env, js_object, result, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external(napi_env env, void *data,
                                                        node_api_basic_finalize finalize_cb,
                                                        void *finalize_hint, napi_value *result)
{
    JSContext *ctx;
    QBNExternal *ext;
    JSValue obj;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    ext = calloc(1, sizeof(*ext));
    if (!ext)
        return qbn_set_error(env, napi_generic_failure);
    ext->data = data;
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, qbn_external_class_id);
    if (JS_IsException(obj)) {
        free(ext);
        return qbn_catch(env);
    }
    JS_SetOpaque(obj, ext);
    if (finalize_cb) {
        ext->fin = qbn_tracker_new(env, QBN_FINALIZE(finalize_cb), data, finalize_hint, 1);
        if (!ext->fin) {
            JS_FreeValue(ctx, obj);
            return qbn_set_error(env, napi_generic_failure);
        }
        ext->fin->backref = &ext->fin;
    }
    return qbn_result(env, obj, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_external(napi_env env, napi_value value,
                                                           void **result)
{
    QBNExternal *ext;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    ext = JS_GetOpaque(QBN_V(value), qbn_external_class_id);
    QBN_RETURN_IF_FALSE(env, JS_GetClassID(QBN_V(value)) == qbn_external_class_id,
                        napi_invalid_arg);
    *result = ext ? ext->data : NULL;
    return qbn_clear_error(env);
}

/* ----------------------------------------------------------- references */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value,
                                                         uint32_t initial_refcount,
                                                         napi_ref *result)
{
    JSValueConst v;
    QBNRef *r;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    if (env->module_api_version < 10 && !JS_IsObject(v) && !JS_IsSymbol(v))
        return qbn_set_error(env, napi_invalid_arg);
    r = qbn_ref_new(env, v, initial_refcount, QBN_OWN_USERLAND, 0, NULL, NULL, NULL);
    if (!r)
        return qbn_set_error(env, napi_generic_failure);
    *result = (napi_ref)r;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env basic_env, napi_ref ref)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, ref);
    qbn_ref_free((QBNRef *)ref);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_ref(napi_env env, napi_ref ref, uint32_t *result)
{
    QBNRef *r = (QBNRef *)ref;
    uint32_t count;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, ref);
    if (r->empty) {
        count = 0;
    } else {
        if (++r->refcount == 1)
            qbn_ref_set_strong(r);
        count = r->refcount;
    }
    if (result)
        *result = count;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_unref(napi_env env, napi_ref ref,
                                                        uint32_t *result)
{
    QBNRef *r = (QBNRef *)ref;
    uint32_t count;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, ref);
    if (r->refcount == 0)
        return qbn_set_error(env, napi_generic_failure);
    if (r->empty) {
        count = 0;
    } else {
        count = --r->refcount;
        if (count == 0)
            qbn_ref_set_weak(r);
    }
    if (result)
        *result = count;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_reference_value(napi_env env, napi_ref ref,
                                                            napi_value *result)
{
    QBNRef *r = (QBNRef *)ref;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, ref);
    QBN_CHECK_ARG(env, result);
    if (r->empty || r->kind != QBN_KIND_VALUE) {
        *result = NULL;
        return qbn_clear_error(env);
    }
    return qbn_result(env, JS_DupValue(env->ctx, r->value), result);
}

/* --------------------------------------------------------- handle scopes */

NAPI_EXTERN napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope *result)
{
    struct napi_handle_scope__ *s;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    s = malloc(sizeof(*s));
    if (!s)
        return qbn_set_error(env, napi_generic_failure);
    qbn_scope_open(env, s, 0, 1);
    *result = s;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, scope);
    if (env->open_handle_scopes == 0 || env->scope != scope)
        return qbn_set_error(env, napi_handle_scope_mismatch);
    qbn_scope_pop(env, scope);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope *result)
{
    struct napi_handle_scope__ *s;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    s = malloc(sizeof(*s));
    if (!s)
        return qbn_set_error(env, napi_generic_failure);
    if (qbn_scope_open(env, s, 1, 1) < 0) {
        free(s);
        return qbn_set_error(env, napi_generic_failure);
    }
    *result = (napi_escapable_handle_scope)s;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope)
{
    struct napi_handle_scope__ *s = (struct napi_handle_scope__ *)scope;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, scope);
    if (env->open_handle_scopes == 0 || env->scope != s)
        return qbn_set_error(env, napi_handle_scope_mismatch);
    qbn_scope_pop(env, s);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_escape_handle(napi_env env,
                                                      napi_escapable_handle_scope scope,
                                                      napi_value escapee, napi_value *result)
{
    struct napi_handle_scope__ *s = (struct napi_handle_scope__ *)scope;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, scope);
    QBN_CHECK_ARG(env, escapee);
    QBN_CHECK_ARG(env, result);
    if (!s->escapable || !s->escape_slot)
        return qbn_set_error(env, napi_invalid_arg);
    if (s->escape_called)
        return qbn_set_error(env, napi_escape_called_twice);
    s->escape_called = 1;
    *s->escape_slot = JS_DupValue(env->ctx, QBN_V(escapee));
    *result = QBN_NV(s->escape_slot);
    return qbn_clear_error(env);
}

/* ------------------------------------------------------------- exceptions */

NAPI_EXTERN napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, error);
    qbn_set_pending(env, JS_DupValue(env->ctx, QBN_V(error)));
    return qbn_clear_error(env);
}

static napi_status qbn_throw_new(napi_env env, JSErrorEnum kind, const char *code, const char *msg)
{
    JSContext *ctx;
    JSValue e;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, msg);
    ctx = env->ctx;
    e = JS_NewErrorLen(ctx, kind, msg, strlen(msg));
    if (JS_IsException(e))
        return qbn_catch(env);
    if (code &&
        JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qbn_set_error(env, napi_generic_failure);
    }
    qbn_set_pending(env, e);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_error(napi_env env, const char *code, const char *msg)
{
    return qbn_throw_new(env, JS_PLAIN_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char *code,
                                                         const char *msg)
{
    return qbn_throw_new(env, JS_TYPE_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_range_error(napi_env env, const char *code,
                                                          const char *msg)
{
    return qbn_throw_new(env, JS_RANGE_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env, const char *code,
                                                               const char *msg)
{
    return qbn_throw_new(env, JS_SYNTAX_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    *result = JS_IsError(env->ctx, QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = env->has_pending != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env,
                                                                     napi_value *result)
{
    JSValue e;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    if (!env->has_pending)
        return napi_get_undefined(env, result);
    e = env->pending;
    env->pending = JS_UNDEFINED;
    env->has_pending = 0;
    env->pending_uncatchable = 0;
    return qbn_result(env, e, result);
}

/* --------------------------------------------------------- array buffers */

NAPI_EXTERN napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    *result = JS_IsArrayBuffer(QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env, size_t byte_length,
                                                           void **data, napi_value *result)
{
    JSContext *ctx;
    JSValue ab;
    size_t size;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    /* zero filled */
    ab = JS_NewArrayBufferCopy(ctx, NULL, byte_length);
    if (JS_IsException(ab))
        return qbn_catch(env);
    if (data)
        *data = JS_GetArrayBuffer(ctx, &size, ab);
    return qbn_result(env, ab, result);
}

static void qbn_buffer_tracker_fire(QBNRef *t)
{
    napi_env env = t->env;
    if (env->destructing) {
        /* the env was (or is being) torn down: no event loop pass left */
        napi_finalize cb = t->finalize_cb;
        void *data = t->data, *hint = t->hint;
        env->refs++;
        qbn_ref_free(t);
        if (cb)
            cb(env, data, hint);
        qbn_env_unref(env);
        return;
    }
    qbn_enqueue_finalizer(t);
}

static void qbn_ab_free(JSRuntime *rt, void *opaque, void *ptr)
{
    qbn_buffer_tracker_fire(opaque);
}

/* an ArrayBuffer over external memory; 'cb' runs (from the event loop)
   when it is collected or detached */
static JSValue qbn_new_external_ab(napi_env env, void *data, size_t len, napi_finalize cb,
                                   void *hint)
{
    JSContext *ctx = env->ctx;
    QBNRef *t = NULL;
    JSValue ab;

    if (cb) {
        t = qbn_tracker_new(env, cb, data, hint, 0);
        if (!t)
            return JS_ThrowOutOfMemory(ctx);
    }
    ab = JS_NewArrayBuffer(ctx, data, len, t ? qbn_ab_free : NULL, t, FALSE);
    if (JS_IsException(ab) && t)
        qbn_ref_free(t);
    return ab;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env, void *external_data, size_t byte_length, node_api_basic_finalize finalize_cb,
    void *finalize_hint, napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env,
                      qbn_new_external_ab(env, external_data, byte_length,
                                          QBN_FINALIZE(finalize_cb), finalize_hint),
                      result);
}

/* SharedArrayBuffers over external memory would bypass the allocator the
   runtime shares with its workers: Node's answer when external buffers are
   not available */
NAPI_EXTERN napi_status NAPI_CDECL node_api_create_external_sharedarraybuffer(
    napi_env env, void *external_data, size_t byte_length, node_api_noenv_finalize finalize_cb,
    void *finalize_hint, napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    return qbn_set_error(env, napi_no_external_buffers_allowed);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer,
                                                             void **data, size_t *byte_length)
{
    JSContext *ctx;
    uint8_t *p;
    size_t size;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, arraybuffer);
    ctx = env->ctx;
    p = JS_GetArrayBuffer(ctx, &size, QBN_V(arraybuffer));
    if (!p && JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        /* a detached ArrayBuffer has no data */
        QBN_RETURN_IF_FALSE(env, JS_IsArrayBuffer(QBN_V(arraybuffer)), napi_invalid_arg);
        size = 0;
    }
    if (data)
        *data = p;
    if (byte_length)
        *byte_length = size;
    return qbn_clear_error(env);
}

static int qbn_is_sab(JSContext *ctx, JSValueConst v)
{
    uint8_t *p;
    size_t size;

    if (!JS_IsObject(v) || JS_IsArrayBuffer(v))
        return 0;
    p = JS_GetArrayBuffer(ctx, &size, v);
    if (!p && JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    return 1;
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_is_sharedarraybuffer(napi_env env, napi_value value,
                                                                 bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    *result = qbn_is_sab(env->ctx, QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env,
                                                                     size_t byte_length,
                                                                     void **data,
                                                                     napi_value *result)
{
    JSContext *ctx;
    JSValue ctor, len, sab;
    size_t size;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    ctor = qbn_global_prop(ctx, "SharedArrayBuffer");
    if (JS_IsException(ctor))
        return qbn_catch(env);
    len = JS_NewFloat64(ctx, (double)byte_length);
    sab = JS_CallConstructor(ctx, ctor, 1, &len);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(sab))
        return qbn_catch(env);
    if (data)
        *data = JS_GetArrayBuffer(ctx, &size, sab);
    return qbn_result(env, sab, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, arraybuffer);
    QBN_RETURN_IF_FALSE(env, JS_IsArrayBuffer(QBN_V(arraybuffer)), napi_arraybuffer_expected);
    QBN_RETURN_IF_FALSE(env, JS_IsImmutableArrayBuffer(QBN_V(arraybuffer)) == 0,
                        napi_detachable_arraybuffer_expected);
    JS_DetachArrayBuffer(env->ctx, QBN_V(arraybuffer));
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env, napi_value value,
                                                                bool *result)
{
    JSContext *ctx;
    uint8_t *p;
    size_t size;
    int detached = 0;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    if (JS_IsArrayBuffer(QBN_V(value))) {
        p = JS_GetArrayBuffer(ctx, &size, QBN_V(value));
        if (!p && JS_HasException(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            detached = 1;
        }
    }
    *result = detached != 0;
    return qbn_clear_error(env);
}

/* ---------------------------------------------------------- typed arrays */

static const struct {
    int8_t js;
    uint8_t size;
    const char *name;
} qbn_ta_info[] = {
    [napi_int8_array] = {JS_TYPED_ARRAY_INT8, 1, "Int8Array"},
    [napi_uint8_array] = {JS_TYPED_ARRAY_UINT8, 1, "Uint8Array"},
    [napi_uint8_clamped_array] = {JS_TYPED_ARRAY_UINT8C, 1, "Uint8ClampedArray"},
    [napi_int16_array] = {JS_TYPED_ARRAY_INT16, 2, "Int16Array"},
    [napi_uint16_array] = {JS_TYPED_ARRAY_UINT16, 2, "Uint16Array"},
    [napi_int32_array] = {JS_TYPED_ARRAY_INT32, 4, "Int32Array"},
    [napi_uint32_array] = {JS_TYPED_ARRAY_UINT32, 4, "Uint32Array"},
    [napi_float32_array] = {JS_TYPED_ARRAY_FLOAT32, 4, "Float32Array"},
    [napi_float64_array] = {JS_TYPED_ARRAY_FLOAT64, 8, "Float64Array"},
    [napi_bigint64_array] = {JS_TYPED_ARRAY_BIG_INT64, 8, "BigInt64Array"},
    [napi_biguint64_array] = {JS_TYPED_ARRAY_BIG_UINT64, 8, "BigUint64Array"},
    [napi_float16_array] = {JS_TYPED_ARRAY_FLOAT16, 2, "Float16Array"},
};

static napi_typedarray_type qbn_ta_from_js(int t)
{
    switch (t) {
    case JS_TYPED_ARRAY_UINT8C:
        return napi_uint8_clamped_array;
    case JS_TYPED_ARRAY_INT8:
        return napi_int8_array;
    case JS_TYPED_ARRAY_UINT8:
        return napi_uint8_array;
    case JS_TYPED_ARRAY_INT16:
        return napi_int16_array;
    case JS_TYPED_ARRAY_UINT16:
        return napi_uint16_array;
    case JS_TYPED_ARRAY_INT32:
        return napi_int32_array;
    case JS_TYPED_ARRAY_UINT32:
        return napi_uint32_array;
    case JS_TYPED_ARRAY_BIG_INT64:
        return napi_bigint64_array;
    case JS_TYPED_ARRAY_BIG_UINT64:
        return napi_biguint64_array;
    case JS_TYPED_ARRAY_FLOAT16:
        return napi_float16_array;
    case JS_TYPED_ARRAY_FLOAT32:
        return napi_float32_array;
    default:
        return napi_float64_array;
    }
}

/* byte length of an ArrayBuffer or SharedArrayBuffer; -1 for other values */
static int64_t qbn_ab_length(JSContext *ctx, JSValueConst v)
{
    uint8_t *p;
    size_t size;

    if (!JS_IsObject(v))
        return -1;
    p = JS_GetArrayBuffer(ctx, &size, v);
    if (!p && JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_IsArrayBuffer(v) ? 0 : -1; /* detached */
    }
    return (int64_t)size;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    *result = JS_GetTypedArrayType(QBN_V(value)) >= 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_typedarray(napi_env env, napi_typedarray_type type,
                                                          size_t length, napi_value arraybuffer,
                                                          size_t byte_offset, napi_value *result)
{
    JSContext *ctx;
    JSValue args[3];
    int64_t ablen;
    size_t size;
    char msg[96];

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, arraybuffer);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    ablen = qbn_ab_length(ctx, QBN_V(arraybuffer));
    QBN_RETURN_IF_FALSE(env, ablen >= 0, napi_invalid_arg);
    if ((unsigned)type >= countof(qbn_ta_info) || !qbn_ta_info[type].name)
        return qbn_set_error(env, napi_invalid_arg);
    size = qbn_ta_info[type].size;
    if (size > 1 && byte_offset % size != 0) {
        snprintf(msg, sizeof(msg), "start offset of %s should be a multiple of %u",
                 qbn_ta_info[type].name, (unsigned)size);
        napi_throw_range_error(env, "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", msg);
        return qbn_set_error(env, napi_generic_failure);
    }
    if (length > ((uint64_t)ablen) / size || byte_offset > (uint64_t)ablen - length * size) {
        napi_throw_range_error(env, "ERR_NAPI_INVALID_TYPEDARRAY_LENGTH",
                               "Invalid typed array length");
        return qbn_set_error(env, napi_generic_failure);
    }
    args[0] = QBN_V(arraybuffer);
    args[1] = JS_NewFloat64(ctx, (double)byte_offset);
    args[2] = JS_NewFloat64(ctx, (double)length);
    return qbn_result(
        env, JS_NewTypedArray(ctx, 3, (JSValueConst *)args, (JSTypedArrayEnum)qbn_ta_info[type].js),
        result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env, napi_value typedarray,
                                                            napi_typedarray_type *type,
                                                            size_t *length, void **data,
                                                            napi_value *arraybuffer,
                                                            size_t *byte_offset)
{
    JSContext *ctx;
    JSValueConst v;
    size_t off = 0, blen = 0, bpe = 1;
    uint8_t *p;
    int t;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, typedarray);
    ctx = env->ctx;
    v = QBN_V(typedarray);
    t = JS_GetTypedArrayType(v);
    QBN_RETURN_IF_FALSE(env, t >= 0, napi_invalid_arg);
    /* NULL (lengths 0) when the buffer is detached or out of bounds */
    p = JS_GetTypedArrayData(ctx, v, &off, &blen, &bpe);
    if (!p) {
        off = 0;
        blen = 0;
    }
    if (type)
        *type = qbn_ta_from_js(t);
    if (length)
        *length = bpe ? blen / bpe : 0;
    if (data)
        *data = p;
    if (byte_offset)
        *byte_offset = off;
    if (arraybuffer) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            ab = JS_GetPropertyStr(ctx, v, "buffer");
        }
        return qbn_result(env, ab, arraybuffer);
    }
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_dataview(napi_env env, size_t length,
                                                        napi_value arraybuffer, size_t byte_offset,
                                                        napi_value *result)
{
    JSContext *ctx;
    JSValue ctor, args[3], dv;
    int64_t ablen;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, arraybuffer);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    ablen = qbn_ab_length(ctx, QBN_V(arraybuffer));
    QBN_RETURN_IF_FALSE(env, ablen >= 0, napi_invalid_arg);
    if (length > (uint64_t)ablen || byte_offset > (uint64_t)ablen - length) {
        napi_throw_range_error(env, "ERR_NAPI_INVALID_DATAVIEW_ARGS",
                               "byte_offset + byte_length should be less than or "
                               "equal to the size in bytes of the array passed in");
        return qbn_set_error(env, napi_pending_exception);
    }
    ctor = qbn_global_prop(ctx, "DataView");
    if (JS_IsException(ctor))
        return qbn_catch(env);
    args[0] = QBN_V(arraybuffer);
    args[1] = JS_NewFloat64(ctx, (double)byte_offset);
    args[2] = JS_NewFloat64(ctx, (double)length);
    dv = JS_CallConstructor(ctx, ctor, 3, (JSValueConst *)args);
    JS_FreeValue(ctx, ctor);
    return qbn_result(env, dv, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    *result = JS_IsDataView(QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

/* data, length, offset and buffer of a DataView (zeros when detached) */
static void qbn_dataview_info(JSContext *ctx, JSValueConst v, uint8_t **pdata, size_t *plen,
                              size_t *poff, JSValue *pbuf)
{
    JSValue buf, tmp;
    int64_t off = 0, len = 0;
    uint8_t *p = NULL;
    size_t size = 0;

    buf = JS_GetPropertyStr(ctx, v, "buffer");
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buf = JS_UNDEFINED;
    }
    tmp = JS_GetPropertyStr(ctx, v, "byteOffset");
    if (JS_IsException(tmp) || JS_ToInt64(ctx, &off, tmp) < 0) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        off = 0;
    }
    JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "byteLength");
    if (JS_IsException(tmp) || JS_ToInt64(ctx, &len, tmp) < 0) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        len = 0;
    }
    JS_FreeValue(ctx, tmp);
    if (JS_IsObject(buf)) {
        p = JS_GetArrayBuffer(ctx, &size, buf);
        if (!p && JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (!p || off < 0 || len < 0 || (uint64_t)(off + len) > size) {
        off = 0;
        len = 0;
        p = NULL;
    }
    *pdata = p ? p + off : NULL;
    *plen = (size_t)len;
    *poff = (size_t)off;
    if (pbuf)
        *pbuf = buf;
    else
        JS_FreeValue(ctx, buf);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_dataview_info(napi_env env, napi_value dataview,
                                                          size_t *bytelength, void **data,
                                                          napi_value *arraybuffer,
                                                          size_t *byte_offset)
{
    JSValue buf;
    uint8_t *p;
    size_t len, off;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, dataview);
    QBN_RETURN_IF_FALSE(env, JS_IsDataView(QBN_V(dataview)), napi_invalid_arg);
    qbn_dataview_info(env->ctx, QBN_V(dataview), &p, &len, &off, arraybuffer ? &buf : NULL);
    if (bytelength)
        *bytelength = len;
    if (data)
        *data = p;
    if (byte_offset)
        *byte_offset = off;
    if (arraybuffer)
        return qbn_result(env, buf, arraybuffer);
    return qbn_clear_error(env);
}

/* --------------------------------------------------------------- version */

NAPI_EXTERN napi_status NAPI_CDECL napi_get_version(node_api_basic_env basic_env, uint32_t *result)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = QBN_API_VERSION;
    return qbn_clear_error(env);
}

/* -------------------------------------------------------------- promises */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_promise(napi_env env, napi_deferred *deferred,
                                                       napi_value *promise)
{
    JSContext *ctx;
    JSValue funcs[2], p;
    struct napi_deferred__ *d;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, deferred);
    QBN_CHECK_ARG(env, promise);
    ctx = env->ctx;
    d = malloc(sizeof(*d));
    if (!d)
        return qbn_set_error(env, napi_generic_failure);
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
        free(d);
        return qbn_catch(env);
    }
    d->resolve = funcs[0];
    d->reject = funcs[1];
    list_add_tail(&d->link, &env->deferreds);
    *deferred = d;
    return qbn_result(env, p, promise);
}

static void qbn_deferred_free(JSContext *ctx, struct napi_deferred__ *d)
{
    list_del(&d->link);
    JS_FreeValue(ctx, d->resolve);
    JS_FreeValue(ctx, d->reject);
    free(d);
}

static napi_status qbn_conclude_deferred(napi_env env, napi_deferred deferred, napi_value result,
                                         int reject)
{
    JSContext *ctx;
    JSValue fn, ret;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, deferred);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    fn = JS_DupValue(ctx, reject ? deferred->reject : deferred->resolve);
    qbn_deferred_free(ctx, deferred);
    ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&QBN_V(result));
    JS_FreeValue(ctx, fn);
    if (JS_IsException(ret))
        return qbn_catch(env);
    JS_FreeValue(ctx, ret);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_resolve_deferred(napi_env env, napi_deferred deferred,
                                                         napi_value resolution)
{
    return qbn_conclude_deferred(env, deferred, resolution, 0);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reject_deferred(napi_env env, napi_deferred deferred,
                                                        napi_value rejection)
{
    return qbn_conclude_deferred(env, deferred, rejection, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool *is_promise)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, is_promise);
    *is_promise = JS_IsPromise(QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_run_script(napi_env env, napi_value script,
                                                   napi_value *result)
{
    JSContext *ctx;
    const char *src;
    size_t len;
    JSValue ret;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, script);
    QBN_CHECK_ARG(env, result);
    QBN_RETURN_IF_FALSE(env, JS_IsString(QBN_V(script)), napi_string_expected);
    ctx = env->ctx;
    src = JS_ToCStringLen(ctx, &len, QBN_V(script));
    if (!src)
        return qbn_catch(env);
    ret = JS_Eval(ctx, src, len, "evalmachine.<anonymous>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    return qbn_result(env, ret, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_adjust_external_memory(node_api_basic_env basic_env,
                                                               int64_t change_in_bytes,
                                                               int64_t *adjusted_value)
{
    napi_env env = (napi_env)basic_env;
    QBNapiState *st;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, adjusted_value);
    st = env->st;
    st->external_memory += change_in_bytes;
    if (st->external_memory < 0)
        st->external_memory = 0;
    *adjusted_value = st->external_memory;
    return qbn_clear_error(env);
}

/* ------------------------------------------------------------------ dates */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_date(napi_env env, double time, napi_value *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewDate(env->ctx, time), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool *is_date)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, is_date);
    *is_date = JS_IsDate(QBN_V(value)) != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value,
                                                       double *result)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    QBN_RETURN_IF_FALSE(env, JS_IsDate(QBN_V(value)), napi_date_expected);
    if (JS_ThisDateValue(env->ctx, result, QBN_V(value)) < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_add_finalizer(napi_env env, napi_value js_object,
                                                      void *finalize_data,
                                                      node_api_basic_finalize finalize_cb,
                                                      void *finalize_hint, napi_ref *result)
{
    QBNRef *r;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, js_object);
    QBN_CHECK_ARG(env, finalize_cb);
    QBN_RETURN_IF_FALSE(env, JS_IsObject(QBN_V(js_object)), napi_invalid_arg);
    r = qbn_ref_new(env, QBN_V(js_object), 0, result ? QBN_OWN_USERLAND : QBN_OWN_RUNTIME, 1,
                    QBN_FINALIZE(finalize_cb), finalize_data, finalize_hint);
    if (!r)
        return qbn_set_error(env, napi_generic_failure);
    if (result)
        *result = (napi_ref)r;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_post_finalizer(node_api_basic_env basic_env,
                                                           napi_finalize finalize_cb,
                                                           void *finalize_data, void *finalize_hint)
{
    napi_env env = (napi_env)basic_env;
    QBNRef *t;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, finalize_cb);
    t = qbn_tracker_new(env, finalize_cb, finalize_data, finalize_hint, 1);
    if (!t)
        return qbn_set_error(env, napi_generic_failure);
    qbn_enqueue_finalizer(t);
    return qbn_clear_error(env);
}

/* ---------------------------------------------------------------- bigint */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env, int64_t value,
                                                            napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewBigInt64(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env, uint64_t value,
                                                             napi_value *result)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    return qbn_result(env, JS_NewBigUint64(env->ctx, value), result);
}

static JSValue qbn_bigint_neg(napi_env env, JSValue v)
{
    QBNapiState *st = env->st;
    JSContext *ctx = env->ctx;
    static const char src[] = "(function (x) { return -x; })";
    JSValue r;

    if (JS_IsException(v))
        return v;
    if (!JS_IsFunction(ctx, st->neg_fn)) {
        st->neg_fn = JS_Eval(ctx, src, sizeof(src) - 1, "<napi>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(st->neg_fn)) {
            st->neg_fn = JS_UNDEFINED;
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
    }
    r = JS_Call(ctx, st->neg_fn, JS_UNDEFINED, 1, (JSValueConst *)&v);
    JS_FreeValue(ctx, v);
    return r;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_words(napi_env env, int sign_bit,
                                                            size_t word_count,
                                                            const uint64_t *words,
                                                            napi_value *result)
{
    JSContext *ctx;
    size_t n = word_count, i;
    uint64_t lo, hi;
    JSValue v, str, ctor;
    char *s, *p;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, words);
    QBN_CHECK_ARG(env, result);
    QBN_RETURN_IF_FALSE(env, word_count <= INT_MAX, napi_invalid_arg);
    ctx = env->ctx;
    while (n > 0 && words[n - 1] == 0)
        n--;
    if (n == 0) {
        v = JS_NewBigInt64(ctx, 0);
    } else if (n == 1 && (!sign_bit || words[0] <= ((uint64_t)1 << 63))) {
        if (sign_bit)
            v = JS_NewBigInt64(ctx, (int64_t)(0 - words[0]));
        else
            v = JS_NewBigUint64(ctx, words[0]);
    } else if (n == 2 && (words[1] >> 63) == 0) {
        lo = words[0];
        hi = words[1];
        if (sign_bit) {
            lo = ~lo + 1;
            hi = ~hi + (lo == 0);
        }
        v = JS_NewBigInt128(ctx, lo, hi);
    } else {
        /* BigInt("0x...") (then negated) */
        s = malloc(n * 16 + 3);
        if (!s)
            return qbn_set_error(env, napi_generic_failure);
        p = s;
        *p++ = '0';
        *p++ = 'x';
        for (i = n; i-- > 0;) {
            snprintf(p, 17, "%016llx", (unsigned long long)words[i]);
            p += 16;
        }
        str = JS_NewStringLen(ctx, s, p - s);
        free(s);
        if (JS_IsException(str))
            return qbn_catch(env);
        ctor = qbn_global_prop(ctx, "BigInt");
        if (JS_IsException(ctor)) {
            JS_FreeValue(ctx, str);
            return qbn_catch(env);
        }
        v = JS_Call(ctx, ctor, JS_UNDEFINED, 1, (JSValueConst *)&str);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, str);
        if (sign_bit)
            v = qbn_bigint_neg(env, v);
    }
    return qbn_result(env, v, result);
}

/* sign and magnitude of a BigInt; *pwords allocated with malloc when
   more than 'cap' words are needed (else 'buf' is used) */
static int qbn_bigint_magnitude(napi_env env, JSValueConst v, int *psign, uint64_t *buf, size_t cap,
                                uint64_t **pwords, size_t *pcount)
{
    JSContext *ctx = env->ctx;
    uint64_t lo, hi, *w;
    JSValue arg, s;
    JSAtom a;
    const char *str, *d;
    size_t len, ndig, count, i;
    int neg = 0;

    *pwords = buf;
    if (JS_ToBigInt128Sat(ctx, &lo, &hi, v) == 0 &&
        !(hi == 0x7fffffffffffffffull && lo == UINT64_MAX) &&
        !(hi == 0x8000000000000000ull && lo == 0)) {
        neg = (int)(hi >> 63);
        if (neg) {
            lo = ~lo + 1;
            hi = ~hi + (lo == 0);
        }
        buf[0] = lo;
        buf[1] = hi;
        *psign = neg;
        *pcount = hi ? 2 : lo ? 1 : 0;
        return 0;
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    /* more than 127 bits: parse value.toString(16) */
    a = JS_NewAtom(ctx, "toString");
    arg = JS_NewInt32(ctx, 16);
    s = JS_Invoke(ctx, v, a, 1, (JSValueConst *)&arg);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(s))
        return -1;
    str = JS_ToCStringLen(ctx, &len, s);
    JS_FreeValue(ctx, s);
    if (!str)
        return -1;
    d = str;
    if (*d == '-') {
        neg = 1;
        d++;
        len--;
    }
    ndig = len;
    count = (ndig + 15) / 16;
    if (count > cap) {
        w = calloc(count, sizeof(uint64_t));
        if (!w) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        *pwords = w;
    } else {
        w = buf;
        memset(w, 0, cap * sizeof(uint64_t));
    }
    for (i = 0; i < ndig; i++) {
        char c = d[ndig - 1 - i];
        uint64_t digit = (c >= '0' && c <= '9')   ? (uint64_t)(c - '0')
                         : (c >= 'a' && c <= 'f') ? (uint64_t)(c - 'a' + 10)
                                                  : (uint64_t)(c - 'A' + 10);
        w[i / 16] |= digit << (4 * (i % 16));
    }
    JS_FreeCString(ctx, str);
    while (count > 0 && w[count - 1] == 0)
        count--;
    *psign = count ? neg : 0;
    *pcount = count;
    return 0;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env, napi_value value,
                                                               int64_t *result, bool *lossless)
{
    JSContext *ctx;
    uint64_t lo, hi;
    int ok;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, lossless);
    ctx = env->ctx;
    QBN_RETURN_IF_FALSE(env, JS_IsBigInt(ctx, QBN_V(value)), napi_bigint_expected);
    if (JS_ToBigInt64(ctx, result, QBN_V(value)) < 0)
        return qbn_catch(env);
    ok = JS_ToBigInt128Sat(ctx, &lo, &hi, QBN_V(value)) == 0 && hi == (uint64_t)0 - (lo >> 63);
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    *lossless = ok != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env, napi_value value,
                                                                uint64_t *result, bool *lossless)
{
    JSContext *ctx;
    uint64_t lo, hi;
    int ok;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, lossless);
    ctx = env->ctx;
    QBN_RETURN_IF_FALSE(env, JS_IsBigInt(ctx, QBN_V(value)), napi_bigint_expected);
    if (JS_ToBigUint64(ctx, result, QBN_V(value)) < 0)
        return qbn_catch(env);
    ok = JS_ToBigInt128Sat(ctx, &lo, &hi, QBN_V(value)) == 0 && hi == 0;
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    *lossless = ok != 0;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env, napi_value value,
                                                               int *sign_bit, size_t *word_count,
                                                               uint64_t *words)
{
    uint64_t buf[2], *w;
    size_t count, n;
    int sign;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, word_count);
    QBN_RETURN_IF_FALSE(env, JS_IsBigInt(env->ctx, QBN_V(value)), napi_bigint_expected);
    if (sign_bit || words) {
        QBN_CHECK_ARG(env, sign_bit);
        QBN_CHECK_ARG(env, words);
    }
    if (qbn_bigint_magnitude(env, QBN_V(value), &sign, buf, countof(buf), &w, &count) < 0)
        return qbn_catch(env);
    if (words) {
        n = *word_count < count ? *word_count : count;
        memcpy(words, w, n * sizeof(uint64_t));
        *sign_bit = sign;
    }
    *word_count = count;
    if (w != buf)
        free(w);
    return qbn_clear_error(env);
}

/* ---------------------------------------------------------- instance data */

NAPI_EXTERN napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env basic_env, void *data,
                                                          napi_finalize finalize_cb,
                                                          void *finalize_hint)
{
    napi_env env = (napi_env)basic_env;
    QBNRef *t;

    QBN_CHECK_ENV(env);
    if (env->instance_data) {
        /* the old data is not finalized (Node's contract) */
        env->instance_data->finalize_cb = NULL;
        qbn_ref_free(env->instance_data);
    }
    t = qbn_tracker_new(env, finalize_cb, data, finalize_hint, 1);
    if (!t)
        return qbn_set_error(env, napi_generic_failure);
    t->backref = &env->instance_data;
    env->instance_data = t;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env basic_env, void **data)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, data);
    *data = env->instance_data ? env->instance_data->data : NULL;
    return qbn_clear_error(env);
}

/* -------------------------------------------------------------- type tags */

NAPI_EXTERN napi_status NAPI_CDECL napi_type_tag_object(napi_env env, napi_value value,
                                                        const napi_type_tag *type_tag)
{
    QBNObjData *od;
    JSValueConst v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, type_tag);
    v = QBN_V(value);
    QBN_RETURN_IF_FALSE(env, !JS_IsNull(v) && !JS_IsUndefined(v), napi_object_expected);
    if (!JS_IsObject(v))
        return qbn_clear_error(env); /* tags a temporary wrapper object */
    od = qbn_objdata_ensure(env, v);
    if (!od)
        return qbn_set_error(env, napi_generic_failure);
    QBN_RETURN_IF_FALSE(env, !od->has_tag, napi_invalid_arg);
    od->tag = *type_tag;
    od->has_tag = 1;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env, napi_value value,
                                                              const napi_type_tag *type_tag,
                                                              bool *result)
{
    QBNObjData *od;
    JSValueConst v;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, type_tag);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    QBN_RETURN_IF_FALSE(env, !JS_IsNull(v) && !JS_IsUndefined(v), napi_object_expected);
    od = qbn_objdata_get(env, v);
    *result =
        od && od->has_tag && od->tag.lower == type_tag->lower && od->tag.upper == type_tag->upper;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object)
{
    napi_status status;
    JSValue obj;
    int r;

    QBN_PREAMBLE(env);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    r = JS_FreezeObject(env->ctx, obj);
    JS_FreeValue(env->ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object)
{
    napi_status status;
    JSValue obj;
    int r;

    QBN_PREAMBLE(env);
    status = qbn_to_object(env, object, &obj);
    if (status != napi_ok)
        return status;
    r = JS_SealObject(env->ctx, obj);
    JS_FreeValue(env->ctx, obj);
    if (r < 0)
        return qbn_catch(env);
    return qbn_clear_error(env);
}

/* ===================================================================== */
/*                               node_api.h                               */
/* ===================================================================== */

NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module *mod)
{
    qbn_modpending = mod;
}

NAPI_EXTERN NAPI_NO_RETURN void NAPI_CDECL napi_fatal_error(const char *location,
                                                            size_t location_len,
                                                            const char *message, size_t message_len)
{
    if (!location)
        location = "";
    if (!message)
        message = "";
    if (location_len == NAPI_AUTO_LENGTH)
        location_len = strlen(location);
    if (message_len == NAPI_AUTO_LENGTH)
        message_len = strlen(message);
    fflush(stdout);
    fprintf(stderr, "FATAL ERROR: %.*s %.*s\n", (int)location_len, location, (int)message_len,
            message);
    fflush(stderr);
    abort();
}

/* ------------------------------------------------------- async contexts */

/* run with the async context 'frame': return the previous one (owned) */
static JSValue qbn_enter_frame(napi_env env, JSValueConst frame)
{
    JSRuntime *rt = JS_GetRuntime(env->ctx);
    JSValue saved = JS_GetAsyncContext(rt);
    JS_SetAsyncContext(rt, JS_DupValue(env->ctx, frame));
    return saved;
}

static void qbn_leave_frame(napi_env env, JSValue saved)
{
    JS_SetAsyncContext(JS_GetRuntime(env->ctx), saved);
}

/* Node's CHECK_TO_OBJECT on an optional async resource and CHECK_TO_STRING
   on its name */
static napi_status qbn_check_resource(napi_env env, napi_value resource, napi_value name)
{
    JSContext *ctx = env->ctx;
    JSValue s;

    if (resource) {
        JSValueConst v = QBN_V(resource);
        QBN_RETURN_IF_FALSE(env, !JS_IsNull(v) && !JS_IsUndefined(v), napi_object_expected);
    }
    if (name) {
        s = JS_ToString(ctx, QBN_V(name));
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return qbn_set_error(env, napi_string_expected);
        }
        JS_FreeValue(ctx, s);
    }
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_init(napi_env env, napi_value async_resource,
                                                   napi_value async_resource_name,
                                                   napi_async_context *result)
{
    struct napi_async_context__ *ac;
    napi_status status;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, async_resource_name);
    QBN_CHECK_ARG(env, result);
    status = qbn_check_resource(env, async_resource, async_resource_name);
    if (status != napi_ok)
        return status;
    ac = malloc(sizeof(*ac));
    if (!ac)
        return qbn_set_error(env, napi_generic_failure);
    ac->frame = JS_GetAsyncContext(JS_GetRuntime(env->ctx));
    list_add_tail(&ac->link, &env->async_contexts);
    *result = ac;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_destroy(napi_env env,
                                                      napi_async_context async_context)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, async_context);
    list_del(&async_context->link);
    JS_FreeValue(env->ctx, async_context->frame);
    free(async_context);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_make_callback(napi_env env,
                                                      napi_async_context async_context,
                                                      napi_value recv, napi_value func, size_t argc,
                                                      const napi_value *argv, napi_value *result)
{
    JSContext *ctx;
    napi_status status;
    JSValue obj, stack_args[QBN_STACK_ARGS], *args = stack_args, ret, saved;
    size_t i;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, recv);
    if (argc > 0)
        QBN_CHECK_ARG(env, argv);
    QBN_RETURN_IF_FALSE(env, argc <= INT_MAX, napi_invalid_arg);
    status = qbn_to_object(env, recv, &obj);
    if (status != napi_ok)
        return status;
    ctx = env->ctx;
    if (!func || !JS_IsFunction(ctx, QBN_V(func))) {
        JS_FreeValue(ctx, obj);
        return qbn_set_error(env, napi_invalid_arg);
    }
    if (argc > QBN_STACK_ARGS) {
        args = malloc(argc * sizeof(JSValue));
        if (!args) {
            JS_FreeValue(ctx, obj);
            return qbn_set_error(env, napi_generic_failure);
        }
    }
    for (i = 0; i < argc; i++)
        args[i] = argv[i] ? QBN_V(argv[i]) : JS_UNDEFINED;
    saved = async_context ? qbn_enter_frame(env, async_context->frame) : JS_UNINITIALIZED;
    ret = JS_Call(ctx, QBN_V(func), obj, (int)argc, (JSValueConst *)args);
    if (async_context)
        qbn_leave_frame(env, saved);
    if (args != stack_args)
        free(args);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(ret))
        return qbn_catch(env);
    /* outermost callback (not under JS): run the microtasks */
    if (env->st->js_depth == 0 && env->open_callback_scopes == 0)
        qb_drain(env->qr);
    if (result)
        return qbn_result(env, ret, result);
    JS_FreeValue(ctx, ret);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_callback_scope(napi_env env,
                                                            napi_value resource_object,
                                                            napi_async_context context,
                                                            napi_callback_scope *result)
{
    struct napi_callback_scope__ *s;
    napi_status status;
    JSValue obj;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    status = qbn_to_object(env, resource_object, &obj);
    if (status != napi_ok)
        return status;
    JS_FreeValue(env->ctx, obj);
    s = malloc(sizeof(*s));
    if (!s)
        return qbn_set_error(env, napi_generic_failure);
    s->env = env;
    s->saved = context ? qbn_enter_frame(env, context->frame) : JS_UNINITIALIZED;
    env->open_callback_scopes++;
    *result = s;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_callback_scope(napi_env env,
                                                             napi_callback_scope scope)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, scope);
    if (env->open_callback_scopes == 0)
        return qbn_set_error(env, napi_callback_scope_mismatch);
    env->open_callback_scopes--;
    if (!JS_IsUninitialized(scope->saved))
        qbn_leave_frame(env, scope->saved);
    free(scope);
    if (env->open_callback_scopes == 0 && env->st->js_depth == 0 && !env->has_pending)
        qb_drain(env->qr);
    return qbn_clear_error(env);
}

/* --------------------------------------------------------------- buffers */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer(napi_env env, size_t length, void **data,
                                                      napi_value *result)
{
    JSContext *ctx;
    JSValue b;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    b = qb_new_buffer(ctx, NULL, length); /* zero filled */
    if (JS_IsException(b))
        return qbn_catch(env);
    if (data)
        *data = JS_GetTypedArrayData(ctx, b, NULL, NULL, NULL);
    return qbn_result(env, b, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_buffer(napi_env env, size_t length,
                                                               void *data,
                                                               node_api_basic_finalize finalize_cb,
                                                               void *finalize_hint,
                                                               napi_value *result)
{
    JSContext *ctx;
    JSValue ab, u8;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    ab = qbn_new_external_ab(env, data, length, QBN_FINALIZE(finalize_cb), finalize_hint);
    if (JS_IsException(ab))
        return qbn_catch(env);
    /* the finalizer now belongs to the ArrayBuffer */
    u8 = JS_NewTypedArray(ctx, 1, (JSValueConst *)&ab, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8))
        return qbn_catch(env);
    return qbn_result(env, qb_to_buffer(ctx, u8), result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(napi_env env,
                                                                           napi_value arraybuffer,
                                                                           size_t byte_offset,
                                                                           size_t byte_length,
                                                                           napi_value *result)
{
    JSContext *ctx;
    JSValue args[3], u8;
    int64_t ablen;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, arraybuffer);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    if (!JS_IsArrayBuffer(QBN_V(arraybuffer)))
        return qbn_set_error(env, napi_invalid_arg);
    ablen = qbn_ab_length(ctx, QBN_V(arraybuffer));
    if (byte_length > (uint64_t)ablen || byte_offset > (uint64_t)ablen - byte_length) {
        napi_throw_range_error(env, "ERR_OUT_OF_RANGE", "The byte offset + length is out of range");
        return qbn_set_error(env, napi_pending_exception);
    }
    args[0] = QBN_V(arraybuffer);
    args[1] = JS_NewFloat64(ctx, (double)byte_offset);
    args[2] = JS_NewFloat64(ctx, (double)byte_length);
    u8 = JS_NewTypedArray(ctx, 3, (JSValueConst *)args, JS_TYPED_ARRAY_UINT8);
    if (JS_IsException(u8))
        return qbn_catch(env);
    return qbn_result(env, qb_to_buffer(ctx, u8), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env, size_t length,
                                                           const void *data, void **result_data,
                                                           napi_value *result)
{
    JSContext *ctx;
    JSValue b;

    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, result);
    ctx = env->ctx;
    b = qb_new_buffer(ctx, length ? data : NULL, length);
    if (JS_IsException(b))
        return qbn_catch(env);
    if (result_data)
        *result_data = JS_GetTypedArrayData(ctx, b, NULL, NULL, NULL);
    return qbn_result(env, b, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool *result)
{
    JSValueConst v;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    QBN_CHECK_ARG(env, result);
    v = QBN_V(value);
    /* node::Buffer::HasInstance: any ArrayBufferView */
    *result = JS_GetTypedArrayType(v) >= 0 || JS_IsDataView(v);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_buffer_info(napi_env env, napi_value value, void **data,
                                                        size_t *length)
{
    JSValueConst v;
    uint8_t *p;
    size_t len = 0, off;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, value);
    v = QBN_V(value);
    if (JS_GetTypedArrayType(v) >= 0) {
        p = JS_GetTypedArrayData(env->ctx, v, NULL, &len, NULL);
        if (!p)
            len = 0;
    } else if (JS_IsDataView(v)) {
        qbn_dataview_info(env->ctx, v, &p, &len, &off, NULL);
    } else {
        return qbn_set_error(env, napi_invalid_arg);
    }
    if (data)
        *data = p;
    if (length)
        *length = len;
    return qbn_clear_error(env);
}

/* ------------------------------------------------------------ async work */

static void qbn_work_execute(uv_work_t *req)
{
    struct napi_async_work__ *w = container_of(req, struct napi_async_work__, req);
    w->execute(w->env, w->data);
}

static void qbn_work_after(uv_work_t *req, int status)
{
    struct napi_async_work__ *w = container_of(req, struct napi_async_work__, req);
    napi_env env = w->env;
    struct napi_handle_scope__ scope;
    napi_async_complete_callback complete = w->complete;
    void *data = w->data;
    JSValue saved;

    w->queued = 0;
    if (w->deleted) {
        free(w);
        return;
    }
    if (!env || !complete)
        return;
    if (env->qr->exiting)
        return;
    env->refs++;
    if (qbn_scope_open(env, &scope, 0, 0) < 0) {
        qbn_env_unref(env);
        return;
    }
    saved = qbn_enter_frame(env, w->frame);
    qbn_clear_error(env);
    /* 'w' may be deleted by the callback */
    complete(env, status == UV_ECANCELED ? napi_cancelled : napi_ok, data);
    qbn_leave_frame(env, saved);
    qbn_scope_close(env, &scope);
    qbn_report_pending(env);
    qb_drain(env->qr);
    qbn_env_unref(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_async_work(napi_env env, napi_value async_resource,
                                                          napi_value async_resource_name,
                                                          napi_async_execute_callback execute,
                                                          napi_async_complete_callback complete,
                                                          void *data, napi_async_work *result)
{
    struct napi_async_work__ *w;
    napi_status status;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, execute);
    QBN_CHECK_ARG(env, result);
    QBN_CHECK_ARG(env, async_resource_name);
    status = qbn_check_resource(env, async_resource, async_resource_name);
    if (status != napi_ok)
        return status;
    w = calloc(1, sizeof(*w));
    if (!w)
        return qbn_set_error(env, napi_generic_failure);
    w->env = env;
    w->execute = execute;
    w->complete = complete;
    w->data = data;
    w->frame = JS_GetAsyncContext(JS_GetRuntime(env->ctx));
    list_add_tail(&w->link, &env->works);
    w->linked = 1;
    *result = w;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_async_work(napi_env env, napi_async_work work)
{
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, work);
    if (work->linked) {
        list_del(&work->link);
        work->linked = 0;
        JS_FreeValue(env->ctx, work->frame);
        work->frame = JS_UNDEFINED;
    }
    if (work->queued) {
        /* still in the thread pool: freed when it completes */
        work->deleted = 1;
        work->env = NULL;
        work->complete = NULL;
        return qbn_clear_error(env);
    }
    free(work);
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env basic_env,
                                                         napi_async_work work)
{
    napi_env env = (napi_env)basic_env;
    int r;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, work);
    r = uv_queue_work(env->qr->loop, &work->req, qbn_work_execute, qbn_work_after);
    if (r < 0) {
        env->last_error.engine_error_code = (uint32_t)r;
        return qbn_set_error(env, r == UV_EINVAL ? napi_invalid_arg : napi_generic_failure);
    }
    work->queued = 1;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_cancel_async_work(node_api_basic_env basic_env,
                                                          napi_async_work work)
{
    napi_env env = (napi_env)basic_env;
    napi_status status;
    int r;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, work);
    r = uv_cancel((uv_req_t *)&work->req);
    if (r == 0)
        return qbn_clear_error(env);
    status = r == UV_EINVAL      ? napi_invalid_arg
             : r == UV_ECANCELED ? napi_cancelled
                                 : napi_generic_failure;
    qbn_set_error(env, status);
    env->last_error.engine_error_code = (uint32_t)r;
    return status;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_node_version(node_api_basic_env basic_env,
                                                         const napi_node_version **version)
{
    static const napi_node_version qbn_node_version = {QBN_NODE_MAJOR, QBN_NODE_MINOR,
                                                       QBN_NODE_PATCH, "node"};
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, version);
    *version = &qbn_node_version;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_uv_event_loop(node_api_basic_env basic_env,
                                                          struct uv_loop_s **loop)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, loop);
    *loop = env->qr->loop;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err)
{
    QBN_PREAMBLE(env);
    QBN_CHECK_ARG(env, err);
    JS_Throw(env->ctx, JS_DupValue(env->ctx, QBN_V(err)));
    qb_report_exception(env->ctx);
    return qbn_clear_error(env);
}

/* ---------------------------------------------------------- cleanup hooks */

NAPI_EXTERN napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env basic_env,
                                                             napi_cleanup_hook fun, void *arg)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, fun);
    qb_add_cleanup_hook(env->qr, fun, arg);
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env basic_env,
                                                                napi_cleanup_hook fun, void *arg)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, fun);
    qb_remove_cleanup_hook(env->qr, fun, arg);
    return napi_ok;
}

/* run an async cleanup hook at exit, then let the loop run its handle
   closes until the addon removes the hook */
static void qbn_async_cleanup_run(void *arg)
{
    napi_async_cleanup_hook_handle h = arg;
    uv_loop_t *loop = h->env->qr->loop;
    int i;

    h->running = 1;
    h->hook(h, h->arg);
    for (i = 0; i < 64 && !h->removed; i++)
        uv_run(loop, UV_RUN_NOWAIT);
    if (h->removed)
        free(h);
    else
        h->running = 2; /* still pending: freed by the removal */
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_add_async_cleanup_hook(node_api_basic_env basic_env, napi_async_cleanup_hook hook, void *arg,
                            napi_async_cleanup_hook_handle *remove_handle)
{
    napi_env env = (napi_env)basic_env;
    napi_async_cleanup_hook_handle h;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, hook);
    h = calloc(1, sizeof(*h));
    if (!h)
        return qbn_set_error(env, napi_generic_failure);
    h->env = env;
    h->hook = hook;
    h->arg = arg;
    qb_add_cleanup_hook(env->qr, qbn_async_cleanup_run, h);
    if (remove_handle)
        *remove_handle = h;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle remove_handle)
{
    napi_async_cleanup_hook_handle h = remove_handle;

    if (!h)
        return napi_invalid_arg;
    if (h->running == 1) {
        h->removed = 1; /* freed by qbn_async_cleanup_run() */
    } else if (h->running == 2) {
        free(h);
    } else {
        qb_remove_cleanup_hook(h->env->qr, qbn_async_cleanup_run, h);
        free(h);
    }
    return napi_ok;
}

/* ------------------------------------------------- thread-safe functions */

static void qbn_tsfn_destroy(napi_threadsafe_function t)
{
    QBNTsfnItem *it, *n;
    for (it = t->head; it; it = n) {
        n = it->next;
        free(it);
    }
    uv_mutex_destroy(&t->mutex);
    if (t->has_cond)
        uv_cond_destroy(&t->cond);
    free(t);
}

/* any thread, mutex held */
static void qbn_tsfn_send(napi_threadsafe_function t)
{
    unsigned cur = atomic_fetch_or(&t->dispatch_state, QBN_DISPATCH_PENDING);
    if ((cur & QBN_DISPATCH_RUNNING) == QBN_DISPATCH_RUNNING)
        return;
    uv_async_send(&t->async);
}

static void qbn_tsfn_close_cb(uv_handle_t *h);

/* loop thread */
static void qbn_tsfn_close_handles(napi_threadsafe_function t)
{
    if (t->handles_closing)
        return;
    t->handles_closing = 1;
    uv_close((uv_handle_t *)&t->async, qbn_tsfn_close_cb);
}

static void qbn_tsfn_cleanup(void *arg);

/* loop thread: drop the JS function, the cleanup hook and the env */
static void qbn_tsfn_release_resources(napi_threadsafe_function t)
{
    napi_env env = t->env;
    if (t->released)
        return;
    t->released = 1;
    JS_FreeValue(env->ctx, t->func);
    t->func = JS_UNDEFINED;
    JS_FreeValue(env->ctx, t->frame);
    t->frame = JS_UNDEFINED;
    qb_remove_cleanup_hook(env->qr, qbn_tsfn_cleanup, t);
    qbn_env_unref(env);
}

/* loop thread: EmptyQueue() and the finalizer (Node's Finalize()) */
static void qbn_tsfn_finalize(napi_threadsafe_function t)
{
    QBNTsfnItem *it, *n;
    napi_env env = t->env;
    JSValue saved;

    if (t->finalized || t->released)
        return;
    t->finalized = 1;
    uv_mutex_lock(&t->mutex);
    it = t->head;
    t->head = t->tail = NULL;
    t->queue_size = 0;
    uv_mutex_unlock(&t->mutex);
    for (; it; it = n) {
        n = it->next;
        t->call_js_cb(NULL, NULL, t->context, it->data);
        free(it);
    }
    if (t->finalize_cb) {
        saved = qbn_enter_frame(env, t->frame);
        qbn_call_finalizer(env, t->finalize_cb, t->finalize_data, t->context);
        qbn_leave_frame(env, saved);
    }
}

static void qbn_tsfn_maybe_delete(napi_threadsafe_function t)
{
    qbn_tsfn_release_resources(t);
    uv_mutex_lock(&t->mutex);
    t->state = QBN_TSFN_CLOSED;
    if (t->thread_count > 0) {
        /* other threads still hold it: freed by their last release */
        uv_mutex_unlock(&t->mutex);
        return;
    }
    uv_mutex_unlock(&t->mutex);
    qbn_tsfn_destroy(t);
}

static void qbn_tsfn_close_cb(uv_handle_t *h)
{
    napi_threadsafe_function t =
        container_of((uv_async_t *)h, struct napi_threadsafe_function__, async);
    qbn_tsfn_finalize(t);
    qbn_tsfn_maybe_delete(t);
}

/* runtime exit: close now, while the context is alive */
static void qbn_tsfn_cleanup(void *arg)
{
    napi_threadsafe_function t = arg;

    uv_mutex_lock(&t->mutex);
    t->state = QBN_TSFN_CLOSING;
    if (t->max_queue_size > 0)
        uv_cond_broadcast(&t->cond);
    uv_mutex_unlock(&t->mutex);
    qbn_tsfn_finalize(t);
    qbn_tsfn_release_resources(t);
    qbn_tsfn_close_handles(t);
}

/* call the JS side for one item */
static void qbn_tsfn_call(napi_threadsafe_function t, void *data)
{
    napi_env env = t->env;
    struct napi_handle_scope__ scope;
    napi_value js_cb = NULL;
    JSValue saved;

    if (env->qr->exiting) {
        /* no JS anymore: the addon releases 'data' (env NULL) */
        t->call_js_cb(NULL, NULL, t->context, data);
        return;
    }
    if (qbn_scope_open(env, &scope, 0, 0) < 0) {
        t->call_js_cb(NULL, NULL, t->context, data);
        return;
    }
    if (!JS_IsUndefined(t->func))
        js_cb = qbn_push(env, JS_DupValue(env->ctx, t->func));
    saved = qbn_enter_frame(env, t->frame);
    qbn_clear_error(env);
    t->call_js_cb(env, js_cb, t->context, data);
    qbn_leave_frame(env, saved);
    qbn_scope_close(env, &scope);
    qbn_report_pending(env);
    qb_drain(env->qr);
}

/* loop thread: one item (Node's DispatchOne) */
static int qbn_tsfn_dispatch_one(napi_threadsafe_function t)
{
    QBNTsfnItem *it;
    void *data = NULL;
    int popped = 0, has_more = 0;
    size_t size;

    uv_mutex_lock(&t->mutex);
    if (t->state == QBN_TSFN_OPEN) {
        size = t->queue_size;
        if (size > 0) {
            it = t->head;
            t->head = it->next;
            if (!t->head)
                t->tail = NULL;
            t->queue_size--;
            data = it->data;
            free(it);
            popped = 1;
            if (size == t->max_queue_size && t->max_queue_size > 0)
                uv_cond_signal(&t->cond);
            size--;
        }
        if (size == 0) {
            if (t->thread_count == 0) {
                t->state = QBN_TSFN_CLOSING;
                if (t->max_queue_size > 0)
                    uv_cond_broadcast(&t->cond);
                qbn_tsfn_close_handles(t);
            }
        } else {
            has_more = 1;
        }
    } else {
        qbn_tsfn_close_handles(t);
    }
    uv_mutex_unlock(&t->mutex);
    if (popped)
        qbn_tsfn_call(t, data);
    return has_more;
}

static void qbn_tsfn_async_cb(uv_async_t *h)
{
    napi_threadsafe_function t = container_of(h, struct napi_threadsafe_function__, async);
    int has_more = 1;
    unsigned iterations_left = QBN_TSFN_MAX_ITERATIONS;

    while (has_more && --iterations_left != 0) {
        atomic_store(&t->dispatch_state, QBN_DISPATCH_RUNNING);
        has_more = qbn_tsfn_dispatch_one(t);
        /* qbn_tsfn_send() was called while the item was handled */
        if (atomic_exchange(&t->dispatch_state, QBN_DISPATCH_IDLE) != QBN_DISPATCH_RUNNING)
            has_more = 1;
        if (t->handles_closing)
            break;
    }
    if (has_more && !t->handles_closing) {
        uv_mutex_lock(&t->mutex);
        qbn_tsfn_send(t);
        uv_mutex_unlock(&t->mutex);
    }
}

/* Node's default call_js_cb: call the function without arguments */
static void qbn_tsfn_default_call_js(napi_env env, napi_value cb, void *context, void *data)
{
    napi_value recv;
    napi_status status;

    if (!env || !cb)
        return;
    status = napi_get_undefined(env, &recv);
    if (status != napi_ok) {
        napi_throw_error(env, "ERR_NAPI_TSFN_GET_UNDEFINED", "Failed to retrieve undefined value");
        return;
    }
    status = napi_call_function(env, recv, cb, 0, NULL, NULL);
    if (status != napi_ok && status != napi_pending_exception) {
        napi_throw_error(env, "ERR_NAPI_TSFN_CALL_JS", "Failed to call JS callback");
    }
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource, napi_value async_resource_name,
    size_t max_queue_size, size_t initial_thread_count, void *thread_finalize_data,
    napi_finalize thread_finalize_cb, void *context, napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function *result)
{
    napi_threadsafe_function t;
    napi_status status;

    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, async_resource_name);
    QBN_RETURN_IF_FALSE(env, initial_thread_count > 0, napi_invalid_arg);
    QBN_CHECK_ARG(env, result);
    if (!func)
        QBN_CHECK_ARG(env, call_js_cb);
    else
        QBN_RETURN_IF_FALSE(env, JS_IsFunction(env->ctx, QBN_V(func)), napi_invalid_arg);
    status = qbn_check_resource(env, async_resource, async_resource_name);
    if (status != napi_ok)
        return status;
    t = calloc(1, sizeof(*t));
    if (!t)
        return qbn_set_error(env, napi_generic_failure);
    if (uv_mutex_init(&t->mutex) < 0) {
        free(t);
        return qbn_set_error(env, napi_generic_failure);
    }
    if (max_queue_size > 0) {
        if (uv_cond_init(&t->cond) < 0) {
            uv_mutex_destroy(&t->mutex);
            free(t);
            return qbn_set_error(env, napi_generic_failure);
        }
        t->has_cond = 1;
    }
    if (uv_async_init(env->qr->loop, &t->async, qbn_tsfn_async_cb) < 0) {
        if (t->has_cond)
            uv_cond_destroy(&t->cond);
        uv_mutex_destroy(&t->mutex);
        free(t);
        return qbn_set_error(env, napi_generic_failure);
    }
    atomic_init(&t->dispatch_state, QBN_DISPATCH_IDLE);
    t->thread_count = initial_thread_count;
    t->state = QBN_TSFN_OPEN;
    t->context = context;
    t->max_queue_size = max_queue_size;
    t->env = env;
    t->finalize_data = thread_finalize_data;
    t->finalize_cb = thread_finalize_cb;
    t->call_js_cb = call_js_cb ? call_js_cb : qbn_tsfn_default_call_js;
    t->func = func ? JS_DupValue(env->ctx, QBN_V(func)) : JS_UNDEFINED;
    t->frame = JS_GetAsyncContext(JS_GetRuntime(env->ctx));
    env->refs++;
    qb_add_cleanup_hook(env->qr, qbn_tsfn_cleanup, t);
    *result = t;
    return qbn_clear_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_threadsafe_function_context(napi_threadsafe_function func, void **result)
{
    if (!func || !result)
        return napi_invalid_arg;
    *result = func->context;
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_threadsafe_function(
    napi_threadsafe_function func, void *data, napi_threadsafe_function_call_mode is_blocking)
{
    napi_threadsafe_function t = func;
    QBNTsfnItem *it;

    if (!t)
        return napi_invalid_arg;
    uv_mutex_lock(&t->mutex);
    while (t->queue_size >= t->max_queue_size && t->max_queue_size > 0 &&
           t->state == QBN_TSFN_OPEN) {
        if (is_blocking == napi_tsfn_nonblocking) {
            uv_mutex_unlock(&t->mutex);
            return napi_queue_full;
        }
        uv_cond_wait(&t->cond, &t->mutex);
    }
    if (t->state == QBN_TSFN_OPEN) {
        it = malloc(sizeof(*it));
        if (!it) {
            uv_mutex_unlock(&t->mutex);
            return napi_generic_failure;
        }
        it->next = NULL;
        it->data = data;
        if (t->tail)
            t->tail->next = it;
        else
            t->head = it;
        t->tail = it;
        t->queue_size++;
        qbn_tsfn_send(t);
        uv_mutex_unlock(&t->mutex);
        return napi_ok;
    }
    if (t->thread_count == 0) {
        uv_mutex_unlock(&t->mutex);
        return napi_invalid_arg;
    }
    t->thread_count--;
    if (!(t->state == QBN_TSFN_CLOSED && t->thread_count == 0)) {
        uv_mutex_unlock(&t->mutex);
        return napi_closing;
    }
    uv_mutex_unlock(&t->mutex);
    qbn_tsfn_destroy(t);
    return napi_closing;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_acquire_threadsafe_function(napi_threadsafe_function func)
{
    napi_threadsafe_function t = func;
    napi_status status = napi_closing;

    if (!t)
        return napi_invalid_arg;
    uv_mutex_lock(&t->mutex);
    if (t->state == QBN_TSFN_OPEN) {
        t->thread_count++;
        status = napi_ok;
    }
    uv_mutex_unlock(&t->mutex);
    return status;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode)
{
    napi_threadsafe_function t = func;

    if (!t)
        return napi_invalid_arg;
    uv_mutex_lock(&t->mutex);
    if (t->thread_count == 0) {
        uv_mutex_unlock(&t->mutex);
        return napi_invalid_arg;
    }
    t->thread_count--;
    if (t->thread_count == 0 || mode == napi_tsfn_abort) {
        if (t->state == QBN_TSFN_OPEN) {
            if (mode == napi_tsfn_abort)
                t->state = QBN_TSFN_CLOSING;
            if (t->state == QBN_TSFN_CLOSING && t->max_queue_size > 0)
                uv_cond_broadcast(&t->cond);
            qbn_tsfn_send(t);
        }
    }
    if (!(t->state == QBN_TSFN_CLOSED && t->thread_count == 0)) {
        uv_mutex_unlock(&t->mutex);
        return napi_ok;
    }
    uv_mutex_unlock(&t->mutex);
    qbn_tsfn_destroy(t);
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unref_threadsafe_function(node_api_basic_env env,
                                                                  napi_threadsafe_function func)
{
    if (!func)
        return napi_invalid_arg;
    uv_unref((uv_handle_t *)&func->async);
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_ref_threadsafe_function(node_api_basic_env env,
                                                                napi_threadsafe_function func)
{
    if (!func)
        return napi_invalid_arg;
    if (!func->handles_closing)
        uv_ref((uv_handle_t *)&func->async);
    return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_get_module_file_name(node_api_basic_env basic_env,
                                                                 const char **result)
{
    napi_env env = (napi_env)basic_env;
    QBN_CHECK_ENV(env);
    QBN_CHECK_ARG(env, result);
    *result = env->filename;
    return qbn_clear_error(env);
}

/* ===================================================================== */
/*                          runtime and loading                           */
/* ===================================================================== */

/* runtime exit: Node's napi_env__::DeleteMe() */
static void qbn_env_teardown(void *arg)
{
    napi_env env = arg;
    JSContext *ctx = env->ctx;
    struct list_head *el, *el1;

    env->destructing = 1;
    qbn_drain_finalizers(env);
    /* the finalizers first: they may delete other references */
    qbn_finalize_all(&env->finalizing_reflist);
    qbn_finalize_all(&env->reflist);
    qbn_drain_finalizers(env);
    list_for_each_safe(el, el1, &env->deferreds)
    {
        qbn_deferred_free(ctx, list_entry(el, struct napi_deferred__, link));
    }
    list_for_each_safe(el, el1, &env->async_contexts)
    {
        struct napi_async_context__ *ac = list_entry(el, struct napi_async_context__, link);
        list_del(&ac->link);
        JS_FreeValue(ctx, ac->frame);
        ac->frame = JS_UNDEFINED;
        /* napi_async_destroy() may still be called: keep it linkable */
        init_list_head(&ac->link);
    }
    list_for_each_safe(el, el1, &env->works)
    {
        struct napi_async_work__ *w = list_entry(el, struct napi_async_work__, link);
        list_del(&w->link);
        w->linked = 0;
        JS_FreeValue(ctx, w->frame);
        w->frame = JS_UNDEFINED;
        w->env = NULL;
    }
    while (env->scope)
        qbn_scope_pop(env, env->scope);
    qbn_pop_to(env, &env->base, 0);
    if (env->has_pending) {
        JS_FreeValue(ctx, env->pending);
        env->pending = JS_UNDEFINED;
        env->has_pending = 0;
    }
    list_del(&env->link);
    qbn_env_unref(env);
}

static void qbn_state_close_cb(uv_handle_t *h)
{
    free(h->data);
}

/* runtime exit, after the envs (registered first, run last) */
static void qbn_state_cleanup(void *arg)
{
    QBNapiState *st = arg;
    JSContext *ctx = st->ctx;

    JS_FreeValue(ctx, st->weakmap);
    JS_FreeValue(ctx, st->wm_get);
    JS_FreeValue(ctx, st->wm_set);
    JS_FreeValue(ctx, st->neg_fn);
    st->weakmap = st->wm_get = st->wm_set = st->neg_fn = JS_UNDEFINED;
    JS_FreeAtom(ctx, st->od_atom);
    JS_FreeAtom(ctx, st->cb_atom);
    st->qr->napi = NULL;
    uv_timer_stop(&st->timer);
    uv_close((uv_handle_t *)&st->timer, qbn_state_close_cb);
}

static JSAtom qbn_private_atom(JSContext *ctx, const char *name)
{
    JSValue sym = JS_NewPrivateSymbol(ctx, name);
    JSAtom a;
    if (JS_IsException(sym))
        return JS_ATOM_NULL;
    a = JS_ValueToAtom(ctx, sym);
    JS_FreeValue(ctx, sym);
    return a;
}

static QBNapiState *qbn_state(JSContext *ctx)
{
    QBRuntime *qr = qb_get(ctx);
    QBNapiState *st = qr->napi;

    if (st)
        return st;
    st = calloc(1, sizeof(*st));
    if (!st) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    st->qr = qr;
    st->ctx = qr->ctx;
    init_list_head(&st->envs);
    st->weakmap = st->wm_get = st->wm_set = st->neg_fn = JS_UNDEFINED;
    st->od_atom = qbn_private_atom(ctx, "napi_object_data");
    st->cb_atom = qbn_private_atom(ctx, "napi_callback");
    if (st->od_atom == JS_ATOM_NULL || st->cb_atom == JS_ATOM_NULL) {
        if (st->od_atom != JS_ATOM_NULL)
            JS_FreeAtom(ctx, st->od_atom);
        if (st->cb_atom != JS_ATOM_NULL)
            JS_FreeAtom(ctx, st->cb_atom);
        free(st);
        return NULL;
    }
    uv_timer_init(qr->loop, &st->timer);
    st->timer.data = st;
    uv_unref((uv_handle_t *)&st->timer);
    qb_add_cleanup_hook(qr, qbn_state_cleanup, st);
    qr->napi = st;
    return st;
}

static napi_env qbn_env_new(QBNapiState *st, const char *filename, int32_t module_api_version)
{
    napi_env env = calloc(1, sizeof(*env));
    if (!env)
        return NULL;
    env->filename = strdup(filename);
    if (!env->filename) {
        free(env);
        return NULL;
    }
    env->ctx = st->ctx;
    env->qr = st->qr;
    env->st = st;
    env->module_api_version = module_api_version;
    env->refs = 1;
    env->pending = JS_UNDEFINED;
    env->top = &env->base;
    env->base.prev = NULL;
    env->base.next = NULL;
    env->base.count = 0;
    init_list_head(&env->reflist);
    init_list_head(&env->finalizing_reflist);
    init_list_head(&env->pending_finalizers);
    init_list_head(&env->deferreds);
    init_list_head(&env->async_contexts);
    init_list_head(&env->works);
    env->c_undefined = JS_UNDEFINED;
    env->c_null = JS_NULL;
    env->c_true = JS_TRUE;
    env->c_false = JS_FALSE;
    list_add_tail(&env->link, &st->envs);
    qb_add_cleanup_hook(st->qr, qbn_env_teardown, env);
    return env;
}

/* "file://" URL of an absolute path (Node's url::FromFilePath) */
static char *qbn_file_url(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(path), i;
    char *url, *p;

    if (!len)
        return strdup("");
    url = malloc(7 + len * 3 + 1);
    if (!url)
        return NULL;
    memcpy(url, "file://", 7);
    p = url + 7;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '#' || c == '%' || c == '<' || c == '>' ||
            c == '?' || c == '`' || c == '{' || c == '}' || c == '\\' || c == '^' || c == '|') {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        } else {
            *p++ = (char)c;
        }
    }
    *p = '\0';
    return url;
}

static JSValue qbn_throw_coded(JSContext *ctx, const char *code, const char *msg)
{
    JSValue e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, strlen(msg));
    if (JS_IsException(e))
        return e;
    if (code)
        JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    return JS_Throw(ctx, e);
}

static napi_module *qbn_loaded_find(void *handle)
{
    QBNLoaded *l;
    napi_module *mod = NULL;
    pthread_mutex_lock(&qbn_loaded_lock);
    for (l = qbn_loaded; l; l = l->next) {
        if (l->handle == handle) {
            mod = l->mod;
            break;
        }
    }
    pthread_mutex_unlock(&qbn_loaded_lock);
    return mod;
}

static void qbn_loaded_save(void *handle, napi_module *mod)
{
    QBNLoaded *l;
    pthread_mutex_lock(&qbn_loaded_lock);
    for (l = qbn_loaded; l; l = l->next) {
        if (l->handle == handle) {
            l->mod = mod;
            pthread_mutex_unlock(&qbn_loaded_lock);
            return;
        }
    }
    l = malloc(sizeof(*l));
    if (l) {
        l->handle = handle;
        l->mod = mod;
        l->next = qbn_loaded;
        qbn_loaded = l;
    }
    pthread_mutex_unlock(&qbn_loaded_lock);
}

/* dlopen(module, filename, flags?): load the Node-API addon 'filename'
   and initialize it with module.exports (process.dlopen) */
static JSValue qbn_js_dlopen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBNapiState *st;
    napi_env env;
    napi_module *mod;
    napi_addon_register_func init = NULL;
    node_api_addon_get_api_version_func get_version;
    struct napi_handle_scope__ scope;
    napi_value nv_exports, r;
    JSValue module = JS_UNDEFINED, exports_v = JS_UNDEFINED, exports = JS_UNDEFINED;
    JSValue fname, ret;
    const char *filename = NULL, *err;
    char *url = NULL, msg[PATH_MAX * 4 + 256];
    int32_t version = QBN_DEFAULT_MODULE_API_VERSION, flags = RTLD_NOW | RTLD_LOCAL;
    void *handle;
    int set_exports;

    if (argc < 2)
        return qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_MISSING_ARGS",
                             "process.dlopen needs at least 2 arguments");
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32(ctx, &flags, argv[2]) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_TYPE",
                                 "flag argument must be an integer.");
        }
    }
    module = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(module))
        return module;
    exports_v = JS_GetPropertyStr(ctx, module, "exports");
    if (JS_IsException(exports_v))
        goto fail;
    exports = JS_ToObject(ctx, exports_v);
    JS_FreeValue(ctx, exports_v);
    if (JS_IsException(exports))
        goto fail;
    filename = JS_ToCString(ctx, argv[1]);
    if (!filename)
        goto fail;
    st = qbn_state(ctx);
    if (!st)
        goto fail;

    qbn_modpending = NULL;
    dlerror();
    handle = dlopen(filename, flags);
    mod = qbn_modpending;
    qbn_modpending = NULL;
    if (!handle) {
        err = dlerror();
        qbn_throw_coded(ctx, "ERR_DLOPEN_FAILED", err ? err : "dlopen failed");
        goto fail;
    }
    if (mod) {
        qbn_loaded_save(handle, mod);
    } else {
        init = (napi_addon_register_func)(void (*)(void))dlsym(handle, "napi_register_module_v1");
        if (init) {
            get_version = (node_api_addon_get_api_version_func)(void (*)(void))dlsym(
                handle, "node_api_module_get_api_version_v1");
            if (get_version)
                version = get_version();
        } else {
            mod = qbn_loaded_find(handle);
            if (!mod || !mod->nm_register_func) {
                dlclose(handle);
                snprintf(msg, sizeof(msg), "Module did not self-register: '%s'.", filename);
                qbn_throw_coded(ctx, "ERR_DLOPEN_FAILED", msg);
                goto fail;
            }
        }
    }
    if (mod) {
        init = mod->nm_register_func;
        version = QBN_DEFAULT_MODULE_API_VERSION;
    }
    if (!init) {
        qbn_throw_coded(ctx, NULL, "Module has no declared entry point.");
        goto fail;
    }

    /* env->filename: the URL of module.filename (Node) */
    fname = JS_GetPropertyStr(ctx, module, "filename");
    if (JS_IsException(fname))
        goto fail;
    if (JS_IsString(fname)) {
        const char *s = JS_ToCString(ctx, fname);
        if (s) {
            url = qbn_file_url(s);
            JS_FreeCString(ctx, s);
        }
    } else {
        url = strdup("");
    }
    JS_FreeValue(ctx, fname);
    if (!url) {
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (version < QBN_DEFAULT_MODULE_API_VERSION) {
        version = QBN_DEFAULT_MODULE_API_VERSION;
    } else if (version > QBN_API_VERSION && version != NAPI_VERSION_EXPERIMENTAL) {
        snprintf(msg, sizeof(msg),
                 "%s requires Node-API version %d, but this version of Node.js "
                 "only supports version %d add-ons.",
                 url, (int)version, QBN_API_VERSION);
        qbn_throw_coded(ctx, NULL, msg);
        goto fail;
    }
    env = qbn_env_new(st, url, version);
    free(url);
    url = NULL;
    if (!env) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }

    if (qbn_scope_open(env, &scope, 0, 0) < 0) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    nv_exports = qbn_push(env, JS_DupValue(ctx, exports));
    if (!nv_exports) {
        qbn_scope_close(env, &scope);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    st->js_depth++;
    qbn_clear_error(env);
    r = init(env, nv_exports);
    st->js_depth--;
    set_exports = r != NULL && r != nv_exports;
    ret = set_exports ? JS_DupValue(ctx, QBN_V(r)) : JS_UNDEFINED;
    qbn_scope_close(env, &scope);
    if (env->has_pending) {
        JS_FreeValue(ctx, ret);
        qbn_rethrow(env);
        goto fail;
    }
    if (set_exports && JS_SetPropertyStr(ctx, module, "exports", ret) < 0)
        goto fail;

    JS_FreeCString(ctx, filename);
    JS_FreeValue(ctx, exports);
    JS_FreeValue(ctx, module);
    return JS_UNDEFINED;
fail:
    free(url);
    if (filename)
        JS_FreeCString(ctx, filename);
    JS_FreeValue(ctx, exports);
    JS_FreeValue(ctx, module);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry qb_napi_funcs[] = {
    JS_CFUNC_DEF("dlopen", 3, qbn_js_dlopen),
    JS_PROP_INT32_DEF("version", QBN_API_VERSION, 0),
};

static int qb_napi_module_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, qb_napi_funcs, countof(qb_napi_funcs));
}

JSModuleDef *qb_init_napi(JSContext *ctx, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSModuleDef *m;

    JS_NewClassID(&qbn_objdata_class_id);
    JS_NewClassID(&qbn_external_class_id);
    JS_NewClassID(&qbn_cbholder_class_id);
    if (!JS_IsRegisteredClass(rt, qbn_objdata_class_id) &&
        JS_NewClass(rt, qbn_objdata_class_id, &qbn_objdata_class) < 0)
        return NULL;
    if (!JS_IsRegisteredClass(rt, qbn_external_class_id) &&
        JS_NewClass(rt, qbn_external_class_id, &qbn_external_class) < 0)
        return NULL;
    if (!JS_IsRegisteredClass(rt, qbn_cbholder_class_id) &&
        JS_NewClass(rt, qbn_cbholder_class_id, &qbn_cbholder_class) < 0)
        return NULL;
    m = JS_NewCModule(ctx, name, qb_napi_module_init);
    if (m && JS_AddModuleExportList(ctx, m, qb_napi_funcs, countof(qb_napi_funcs)) < 0)
        return NULL;
    return m;
}
