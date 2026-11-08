/*
 * qbun: the Bun/Node.js runtime of dquickts.
 *
 * Shared C interface of the native bindings (node/NAME.c). Each binding is a
 * C module named "qbun:<name>" that the TypeScript library (node/lib)
 * imports; the libuv loop of the runtime (QBRuntime.loop) drives all I/O.
 *
 * Conventions for the bindings:
 *   - no mutable static state: a worker thread runs its own JSRuntime with
 *     its own QBRuntime and uv loop (class IDs are process wide, the
 *     classes are registered per runtime with JS_NewClass()).
 *   - C code that calls into JS from a libuv callback goes through
 *     qb_call(), which reports an uncaught exception and runs the
 *     microtasks afterwards, like Node's MakeCallback().
 *   - failed system calls throw (or pass to callbacks) the errors made by
 *     qb_uv_error(): `code`, `errno`, `syscall`, `path` and `dest` like
 *     Node's.
 */
#ifndef QBUN_H
#define QBUN_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>
#include "../quickjs.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct QBRuntime QBRuntime;

struct QBRuntime {
    JSRuntime *rt;
    JSContext *ctx;
    uv_loop_t *loop;
    int is_main_thread;
    /* directory of the TypeScript library (node/lib) */
    char *lib_dir;
    char *exec_path;
    /* realpath of the entry module, NULL for -e */
    char *main_path;
    /* process.argv and process.execArgv */
    int argc;
    char **argv;
    int exec_argc;
    char **exec_argv;
    /* the object given to qbun:core setHooks() by internal:bootstrap */
    JSValue hooks;
    /* Buffer.prototype, set by node:buffer; plain Uint8Arrays before */
    JSValue buffer_proto;
    /* rejected promises without a handler, checked after the microtasks */
    struct QBRejection *rejections;
    /* immediates: run by a check handle, the idle handle keeps the poll
       phase from blocking while some are pending */
    uv_check_t immediate_check;
    uv_idle_t immediate_idle;
    int immediate_pending;
    /* set by process.exit() / a fatal error: the loop stops */
    int exiting;
    int exit_code;
    /* per-binding state, owned by the binding that sets it */
    void *napi;
    void *worker;
    /* uv_hrtime() at runtime creation */
    uint64_t start_time;
    /* qb_add_cleanup_hook() list, run by qb_runtime_exit() */
    struct QBCleanupHook *cleanup_hooks;
};

typedef void QBCleanupFunc(void *arg);
/* run fn(arg) when the runtime exits (after process 'exit', in reverse
   order of registration; Node's environment cleanup hooks). Bindings
   release the JS values their C structures hold there. */
void qb_add_cleanup_hook(QBRuntime *qr, QBCleanupFunc *fn, void *arg);
/* remove the first hook registered with (fn, arg) */
void qb_remove_cleanup_hook(QBRuntime *qr, QBCleanupFunc *fn, void *arg);

/* the QBRuntime of a context (the opaque of its runtime) */
QBRuntime *qb_get(JSContext *ctx);

/* call 'func' from a libuv callback: an exception goes to
   process.on('uncaughtException') or ends the process, then the
   microtasks run. Return the result (JS_UNDEFINED after an exception). */
JSValue qb_call(JSContext *ctx, JSValueConst func, JSValueConst this_obj, int argc,
                JSValueConst *argv);
/* run the microtasks and report the unhandled rejections */
void qb_drain(QBRuntime *qr);
/* report the pending exception of 'ctx' as uncaught */
void qb_report_exception(JSContext *ctx);

/* a Node-style system error for the libuv error 'err' (< 0): message
   "ENOENT: no such file or directory, open 'path'", properties code,
   errno, syscall, path and dest (when not NULL) */
JSValue qb_uv_error(JSContext *ctx, int err, const char *syscall, const char *path,
                    const char *dest);
/* throw qb_uv_error() and return JS_EXCEPTION */
JSValue qb_throw_uv(JSContext *ctx, int err, const char *syscall, const char *path,
                    const char *dest);
/* throw an Error (or TypeError/RangeError with 'kind') with a `code`
   property, like Node's ERR_* errors */
JSValue qb_throw_code(JSContext *ctx, JSErrorEnum kind, const char *code, const char *fmt, ...);

/* the bytes of an ArrayBuffer, a SharedArrayBuffer, a typed array or a
   DataView (borrowed: any JS call can invalidate them). NULL with a
   TypeError for any other value. */
uint8_t *qb_get_bytes(JSContext *ctx, JSValueConst val, size_t *plen);
/* a Buffer holding a copy of 'data' */
JSValue qb_new_buffer(JSContext *ctx, const uint8_t *data, size_t len);
/* a Buffer that takes 'data' (allocated with js_malloc_rt() of the
   runtime, freed when collected) */
JSValue qb_new_buffer_owned(JSContext *ctx, uint8_t *data, size_t len);
/* give a Uint8Array the Buffer prototype */
JSValue qb_to_buffer(JSContext *ctx, JSValue u8);

/* define qb_init_<id>(): the C module exporting the function list
   'funcs'. Bindings with classes write their own init: register the
   classes (JS_NewClassID() once, JS_NewClass() per runtime) in the module
   init function, which runs once per runtime. */
#define QB_SIMPLE_MODULE(id, funcs)                                                                \
    static int qb_##id##_module_init(JSContext *ctx, JSModuleDef *m)                               \
    {                                                                                              \
        return JS_SetModuleExportList(ctx, m, funcs, countof(funcs));                              \
    }                                                                                              \
    JSModuleDef *qb_init_##id(JSContext *ctx, const char *name)                                    \
    {                                                                                              \
        JSModuleDef *m = JS_NewCModule(ctx, name, qb_##id##_module_init);                          \
        if (m && JS_AddModuleExportList(ctx, m, funcs, countof(funcs)) < 0)                        \
            return NULL;                                                                           \
        return m;                                                                                  \
    }

/* set a property of a new object, freeing 'val' (returns -1 on error) */
int qb_set(JSContext *ctx, JSValueConst obj, const char *name, JSValue val);

/* the native modules: qbun:<name> */
typedef JSModuleDef *QBModuleInit(JSContext *ctx, const char *name);
QBModuleInit qb_init_core;
QBModuleInit qb_init_fs;
QBModuleInit qb_init_os;
QBModuleInit qb_init_process;
QBModuleInit qb_init_net;
QBModuleInit qb_init_tls;
QBModuleInit qb_init_crypto;
QBModuleInit qb_init_zlib;
QBModuleInit qb_init_text;
QBModuleInit qb_init_http;
QBModuleInit qb_init_sqlite;
QBModuleInit qb_init_ffi;
QBModuleInit qb_init_napi;
QBModuleInit qb_init_worker;
QBModuleInit qb_init_async;
QBModuleInit qb_init_misc;
QBModuleInit qb_init_image;

/* the name of the signal 'signum' ("SIGINT"), NULL if unknown, and the
   { SIGHUP: 1, ... } object of the signals of the system (node/os.c) */
const char *qb_signal_name(int signum);
JSValue qb_signals_object(JSContext *ctx);

/* the uv_stream_t of a qbun:net stream handle (TCP, Pipe or TTY), NULL
   with a TypeError otherwise (node/net.c; used by spawn in node/process.c) */
uv_stream_t *qb_stream_handle(JSContext *ctx, JSValueConst obj);

/* runtime lifecycle (node/core.c), used by qbun.c and node/worker.c */
typedef struct QBOptions {
    const char *lib_dir;
    const char *exec_path;
    int argc; /* process.argv */
    char **argv;
    int exec_argc;
    char **exec_argv;
    uv_loop_t *loop; /* NULL: a new loop owned by the runtime */
    int is_main_thread;
} QBOptions;

QBRuntime *qb_runtime_new(const QBOptions *opts);
/* run internal:bootstrap (the globals); -1 on error (reported) */
int qb_bootstrap(QBRuntime *qr);
/* load the module 'spec' (resolved from the working directory) and run it
   to completion before the entry module, like bun --preload; -1 on error
   (reported) */
int qb_preload(QBRuntime *qr, const char *spec);
/* run the entry module 'filename' (ESM or CommonJS); -1 on error */
int qb_run_main(QBRuntime *qr, const char *filename);
/* evaluate 'src' as the entry module "[eval]" (TypeScript if 'ts') */
int qb_run_eval(QBRuntime *qr, const char *src, int ts);
/* run the event loop until no referenced handle is left, emitting
   process 'beforeExit'; return the exit code */
int qb_run_loop(QBRuntime *qr);
/* emit process 'exit' (once) and return the exit code; the runtime is
   not freed (the main thread exits with it, node/worker.c frees the
   runtimes of the workers) */
int qb_runtime_exit(QBRuntime *qr);
/* free a runtime after qb_runtime_exit() (worker threads): the context,
   the JS runtime and, when the runtime created it, the loop (its
   remaining handles are closed). Bindings must not keep JSValues of the
   runtime in C structures that outlive it. */
void qb_runtime_free(QBRuntime *qr);
/* runtime setup owned by node/worker.c (SharedArrayBuffer allocator,
   Atomics.wait), called by qb_runtime_new() for every runtime */
void qb_worker_runtime_init(QBRuntime *qr);

#endif /* QBUN_H */
