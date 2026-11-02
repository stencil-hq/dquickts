/*
 * QuickJS stand alone interpreter
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__) || defined(__CYGWIN__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#endif

#include "cutils.h"
#include "quickjs-libc.h"
#ifdef CONFIG_PYTHON
#include "python/quickpy.h"
#endif

extern const uint8_t qjsc_repl[];
extern const uint32_t qjsc_repl_size;

#ifdef CONFIG_TEMPORAL_INTL
static int enable_experimental = 0;
#endif

/* command line of the process, for execArgv and argv0 */
static int qjs_argc;
static char **qjs_argv;

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags)
{
    JSValue val;
    int ret;

    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        /* for the modules, we compile then run to be able to set
           import.meta */
        val = JS_Eval(ctx, buf, buf_len, filename,
                      eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            /* "<input>", "<cmdline>" and "<stdin>" are not files:
               realpath() would fail */
            if (js_module_set_import_meta(ctx, val, filename[0] != '<',
                                          TRUE) < 0) {
                JS_FreeValue(ctx, val);
                val = JS_EXCEPTION;
            } else {
                val = JS_EvalFunction(ctx, val);
            }
        }
        val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

/* read the standard input (zero terminated like js_load_file()) */
static uint8_t *load_stdin(JSContext *ctx, size_t *pbuf_len)
{
    uint8_t *buf = NULL, *new_buf;
    size_t len = 0, size = 0, n;

    for(;;) {
        if (len + 1 >= size) {
            size = size ? size * 2 : 4096;
            new_buf = js_realloc(ctx, buf, size);
            if (!new_buf) {
                js_free(ctx, buf);
                return NULL;
            }
            buf = new_buf;
        }
        n = fread(buf + len, 1, size - len - 1, stdin);
        if (n == 0)
            break;
        len += n;
    }
    buf[len] = '\0';
    *pbuf_len = len;
    return buf;
}

/* 'filename' = "-" reads the standard input. 'typescript' forces
   TypeScript, otherwise it is selected by the file extension. */
static int eval_file(JSContext *ctx, const char *filename, int module, int strict,
                     int typescript)
{
    uint8_t *buf;
    int ret, eval_flags, ts_flag;
    size_t buf_len;

    if (!strcmp(filename, "-")) {
        buf = load_stdin(ctx, &buf_len);
        filename = "<stdin>";
    } else {
        buf = js_load_file(ctx, &buf_len, filename);
    }
    if (!buf) {
        perror(filename);
        exit(1);
    }

    if (typescript) {
        ts_flag = js_std_get_typescript_flag(ctx, ".ts");
    } else {
        ts_flag = js_std_get_typescript_flag(ctx, filename);
        if (ts_flag < 0) {
            js_std_dump_error(ctx);
            js_free(ctx, buf);
            return -1;
        }
    }
    if (module < 0) {
        module = (has_suffix(filename, ".mjs") || has_suffix(filename, ".mts") ||
                  (!has_suffix(filename, ".cts") &&
                   JS_DetectModule((const char *)buf, buf_len)));
    }
    if (module) {
        eval_flags = JS_EVAL_TYPE_MODULE;
    } else {
        eval_flags = JS_EVAL_TYPE_GLOBAL;
        if (strict)
            eval_flags |= JS_EVAL_FLAG_STRICT;
    }
    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags | ts_flag);
    js_free(ctx, buf);
    return ret;
}

static JSValue js_gc(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue js_navigator_get_userAgent(JSContext *ctx, JSValueConst this_val)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "quickjs/%s", JS_GetVersion());
    return JS_NewString(ctx, buf);
}

static const JSCFunctionListEntry js_navigator_proto_funcs[] = {
    JS_CGETSET_DEF("userAgent", js_navigator_get_userAgent, NULL ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Navigator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_qjs_global_funcs[] = {
    JS_CFUNC_DEF("gc", 0, js_gc ),
};

/* gc(), navigator, execArgv (the command line of the process) and
   argv0 */
static int add_qjs_globals(JSContext *ctx)
{
    JSValue global, args, proto, navigator;
    int i;

    global = JS_GetGlobalObject(ctx);
    if (JS_SetPropertyFunctionList(ctx, global, js_qjs_global_funcs,
                                   countof(js_qjs_global_funcs)))
        goto fail;
    args = JS_NewArray(ctx);
    if (JS_IsException(args))
        goto fail;
    for(i = 0; i < qjs_argc; i++) {
        if (JS_SetPropertyUint32(ctx, args, i,
                                 JS_NewString(ctx, qjs_argv[i])) < 0) {
            JS_FreeValue(ctx, args);
            goto fail;
        }
    }
    if (JS_SetPropertyStr(ctx, global, "execArgv", args) < 0)
        goto fail;
    if (JS_SetPropertyStr(ctx, global, "argv0",
                          JS_NewString(ctx, qjs_argc > 0 ? qjs_argv[0] : "")) < 0)
        goto fail;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        goto fail;
    if (JS_SetPropertyFunctionList(ctx, proto, js_navigator_proto_funcs,
                                   countof(js_navigator_proto_funcs))) {
        JS_FreeValue(ctx, proto);
        goto fail;
    }
    navigator = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    if (JS_DefinePropertyValueStr(ctx, global, "navigator", navigator,
                                  JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0)
        goto fail;
    JS_FreeValue(ctx, global);
    return 0;
 fail:
    JS_FreeValue(ctx, global);
    return -1;
}

/* also used to initialize the worker context */
static JSContext *JS_NewCustomContext(JSRuntime *rt)
{
    JSContext *ctx;
    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    /* system modules */
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_init_module_bjson(ctx, "bjson");
    if (add_qjs_globals(ctx) < 0) {
        JS_FreeContext(ctx);
        return NULL;
    }
#ifdef CONFIG_TEMPORAL_INTL
    if (enable_experimental) {
        JS_AddIntrinsicTemporal(ctx);
        JS_AddIntrinsicIntl(ctx);
    }
#endif
    return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD  0
#else
#define MALLOC_OVERHEAD  8
#endif

struct trace_malloc_data {
    uint8_t *base;
};

static inline unsigned long long js_trace_malloc_ptr_offset(uint8_t *ptr,
                                                struct trace_malloc_data *dp)
{
    return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static size_t js_trace_malloc_usable_size(const void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void *)ptr);
#elif defined(__EMSCRIPTEN__)
    return 0;
#elif defined(__linux__) || defined(__GLIBC__) || defined(__CYGWIN__)
    return malloc_usable_size((void *)ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size((void *)ptr);
#endif
}

static void
#ifdef _WIN32
/* mingw printf is used */
__attribute__((format(gnu_printf, 2, 3)))
#else
__attribute__((format(printf, 2, 3)))
#endif
    js_trace_malloc_printf(JSMallocState *s, const char *fmt, ...)
{
    va_list ap;
    int c;

    va_start(ap, fmt);
    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            /* only handle %p and %zd */
            if (*fmt == 'p') {
                uint8_t *ptr = va_arg(ap, void *);
                if (ptr == NULL) {
                    printf("NULL");
                } else {
                    printf("H%+06lld.%zd",
                           js_trace_malloc_ptr_offset(ptr, s->opaque),
                           js_trace_malloc_usable_size(ptr));
                }
                fmt++;
                continue;
            }
            if (fmt[0] == 'z' && fmt[1] == 'd') {
                size_t sz = va_arg(ap, size_t);
                printf("%zd", sz);
                fmt += 2;
                continue;
            }
        }
        putc(c, stdout);
    }
    va_end(ap);
}

static void js_trace_malloc_init(struct trace_malloc_data *s)
{
    free(s->base = malloc(8));
}

static void *js_trace_malloc(JSMallocState *s, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    assert(size != 0);

    if (unlikely(s->malloc_size + size > s->malloc_limit))
        return NULL;
    ptr = malloc(size);
    js_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    }
    return ptr;
}

static void *js_trace_calloc(JSMallocState *s, size_t count, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    assert(count != 0 && size != 0);

    if (unlikely(count > SIZE_MAX / size))
        return NULL;
    if (unlikely(s->malloc_size + count * size > s->malloc_limit))
        return NULL;
    ptr = calloc(count, size);
    js_trace_malloc_printf(s, "C %zd %zd -> %p\n", count, size, ptr);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    }
    return ptr;
}

static void js_trace_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;

    js_trace_malloc_printf(s, "F %p\n", ptr);
    s->malloc_count--;
    s->malloc_size -= js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    free(ptr);
}

static void *js_trace_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_trace_malloc(s, size);
    }
    old_size = js_trace_malloc_usable_size(ptr);
    if (size == 0) {
        js_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    if (s->malloc_size + size - old_size > s->malloc_limit)
        return NULL;

    js_trace_malloc_printf(s, "R %zd %p", size, ptr);

    ptr = realloc(ptr, size);
    js_trace_malloc_printf(s, " -> %p\n", ptr);
    if (ptr) {
        s->malloc_size += js_trace_malloc_usable_size(ptr) - old_size;
    }
    return ptr;
}

static const JSMallocFunctions trace_mf = {
    js_trace_calloc,
    js_trace_malloc,
    js_trace_free,
    js_trace_realloc,
    js_trace_malloc_usable_size,
};

static size_t get_suffixed_size(const char *str)
{
    char *p;
    size_t v;
    v = (size_t)strtod(str, &p);
    switch(*p) {
    case 'G':
        v <<= 30;
        break;
    case 'M':
        v <<= 20;
        break;
    case 'k':
    case 'K':
        v <<= 10;
        break;
    default:
        if (*p != '\0') {
            fprintf(stderr, "qjs: invalid suffix: %s\n", p);
            exit(1);
        }
        break;
    }
    return v;
}

/* Durable REPL (qjs --drepl FILE)

   The commands run in a second runtime, the durable runtime, saved to
   FILE with JS_WriteSnapshot() after each command and restored from it
   at startup: the declarations survive the process. FILE only holds a
   delta (JS_SNAPSHOT_DELTA) of a new session: a base is reproducible, so
   the base of a new session is rebuilt at startup instead of being
   stored. The line editor (repl.js) stays in the main runtime and drives
   the durable runtime through the 'durable' object of its options. No
   value crosses the runtimes: the durable runtime renders its results
   to text and copies the completion data through JSON.

   The durable runtime only has the standard objects, print() and
   console.log(): the 'std' and 'os' modules hold host state (files,
   timers, handlers) that a snapshot cannot save. Its commands are
   TypeScript. */

typedef struct {
    JSRuntime *rt; /* NULL without --drepl */
    JSContext *ctx;
    const char *path;
    int eval_flags;
    BOOL restored; /* TRUE if restored from 'path' */
    void (*prev_sigint)(int); /* Ctrl-C handler of the main runtime */
} DurableRepl;

static DurableRepl drepl;

/* the C functions that the durable heap references */
static const void * const drepl_natives[] = {
    (const void *)js_std_print,
    (const void *)js_std_console_log,
};

static const JSSnapshotHost drepl_host = {
    .natives = drepl_natives,
    .native_count = countof(drepl_natives),
};

/* set by Ctrl-C while the durable runtime runs */
static volatile sig_atomic_t drepl_sigint;

static void drepl_sigint_handler(int sig_num)
{
    drepl_sigint = 1;
}

static int drepl_interrupt_handler(JSRuntime *rt, void *opaque)
{
    return drepl_sigint;
}

/* while the durable runtime runs, Ctrl-C interrupts it instead of
   reaching the main runtime */
static void drepl_enter(void)
{
    /* it is called from a C function of the main runtime */
    JS_UpdateStackTop(drepl.rt);
    drepl_sigint = 0;
    drepl.prev_sigint = signal(SIGINT, drepl_sigint_handler);
}

static void drepl_leave(void)
{
    signal(SIGINT, drepl.prev_sigint);
    /* make the "interrupted" error catchable, as std.evalScript() does */
    if (drepl_sigint && JS_HasException(drepl.ctx))
        JS_SetUncatchableException(drepl.ctx, FALSE);
    drepl_sigint = 0;
}

static void drepl_write(void *opaque, const char *buf, size_t len)
{
    dbuf_put(opaque, (const uint8_t *)buf, len);
}

/* 'prefix' followed by the rendering of 'val', a value of the durable
   runtime, as a string of 'ctx' */
static JSValue drepl_render(JSContext *ctx, JSValueConst val,
                            const char *prefix)
{
    DynBuf dbuf;
    JSValue str;

    dbuf_init(&dbuf);
    dbuf_putstr(&dbuf, prefix);
    JS_PrintValue(drepl.ctx, drepl_write, &dbuf, val, NULL);
    if (dbuf_error(&dbuf))
        str = JS_ThrowOutOfMemory(ctx);
    else
        str = JS_NewStringLen(ctx, (const char *)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return str;
}

/* throw in 'ctx' the rendering of the exception of the durable runtime */
static JSValue drepl_rethrow(JSContext *ctx)
{
    JSValue exc, text;

    exc = JS_GetException(drepl.ctx);
    text = drepl_render(ctx, exc, "");
    JS_FreeValue(drepl.ctx, exc);
    if (JS_IsException(text))
        return text;
    return JS_Throw(ctx, text);
}

/* report the pending exception of the durable runtime on stderr */
static void drepl_report(const char *what)
{
    JSValue exc = JS_GetException(drepl.ctx);
    DynBuf dbuf;

    dbuf_init(&dbuf);
    JS_PrintValue(drepl.ctx, drepl_write, &dbuf, exc, NULL);
    fprintf(stderr, "qjs: %s %s: %.*s\n", drepl.path, what,
            (int)dbuf.size, (const char *)dbuf.buf);
    dbuf_free(&dbuf);
    JS_FreeValue(drepl.ctx, exc);
}

/* write the delta of the durable runtime to its file, through a
   temporary file renamed over it so that the file always holds a
   complete delta. The errors are reported on stderr. */
static int drepl_save(void)
{
    JSContext *dctx = drepl.ctx;
    uint8_t *buf;
    size_t len, len_tmp, written;
    char *tmp;
    FILE *f;
    int ret = -1;

    buf = JS_WriteSnapshot(dctx, &len, &drepl_host, NULL, 0, JS_SNAPSHOT_DELTA);
    if (!buf) {
        drepl_report("not saved");
        return -1;
    }
    len_tmp = strlen(drepl.path) + 5;
    tmp = malloc(len_tmp);
    if (tmp) {
        snprintf(tmp, len_tmp, "%s.tmp", drepl.path);
        f = fopen(tmp, "wb");
        if (f) {
            written = fwrite(buf, 1, len, f);
            if (fclose(f) == 0 && written == len) {
#ifdef _WIN32
                /* rename() does not replace an existing file */
                remove(drepl.path);
#endif
                if (rename(tmp, drepl.path) == 0)
                    ret = 0;
            }
        }
    }
    if (ret < 0) {
        fprintf(stderr, "qjs: %s not saved: %s\n", drepl.path,
                strerror(errno));
        if (tmp)
            remove(tmp);
    }
    free(tmp);
    js_free(dctx, buf);
    return ret;
}

/* Run the jobs of the command whose async evaluation returned
   'promise', then return its completion value (JS_EXCEPTION if it
   threw). A promise still pending awaits something that no job
   settles: it is the completion value. */
static JSValue drepl_settle(JSValue promise)
{
    JSContext *dctx = drepl.ctx, *ctx1;
    JSValue res, val;
    int ret;

    if (JS_IsException(promise))
        return promise;
    while ((ret = JS_ExecutePendingJob(drepl.rt, &ctx1)) != 0) {
        if (ret < 0) {
            /* e.g. interrupted: the other jobs run after the next
               command */
            JS_FreeValue(dctx, promise);
            return JS_EXCEPTION;
        }
    }
    switch (JS_PromiseState(dctx, promise)) {
    case JS_PROMISE_FULFILLED:
        res = JS_PromiseResult(dctx, promise);
        val = JS_GetPropertyStr(dctx, res, "value");
        JS_FreeValue(dctx, res);
        break;
    case JS_PROMISE_REJECTED:
        val = JS_Throw(dctx, JS_PromiseResult(dctx, promise));
        break;
    default:
        return promise;
    }
    JS_FreeValue(dctx, promise);
    return val;
}

/* a copy in 'ctx' of 'val' if it is a number or a bigint (for the
   hexadecimal display of repl.js), undefined otherwise */
static JSValue drepl_copy_number(JSContext *ctx, JSValueConst val)
{
    JSValue global, ctor, str, ret;
    const char *s;

    if (JS_IsNumber(val)) {
        double d;
        JS_ToFloat64(drepl.ctx, &d, val); /* cannot fail on a number */
        return JS_NewFloat64(ctx, d);
    }
    if (!JS_IsBigInt(drepl.ctx, val))
        return JS_UNDEFINED;
    s = JS_ToCString(drepl.ctx, val);
    if (!s)
        return drepl_rethrow(ctx);
    str = JS_NewString(ctx, s);
    JS_FreeCString(drepl.ctx, s);
    if (JS_IsException(str))
        return str;
    global = JS_GetGlobalObject(ctx);
    ctor = JS_GetPropertyStr(ctx, global, "BigInt");
    ret = JS_Call(ctx, ctor, JS_UNDEFINED, 1, (JSValueConst *)&str);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, str);
    return ret;
}

/* durable.eval(src): run the command 'src' and its jobs in the durable
   runtime, then save it. Returns {text, value, error}: 'text' renders
   the completion value or, if 'error', the exception; 'value' is the
   completion value if it is a number or a bigint. */
static JSValue js_drepl_eval(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    static const char *props[] = { "text", "value", "error" };
    JSContext *dctx = drepl.ctx;
    JSValue val, global, values[3];
    const char *str;
    size_t len;
    BOOL error;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    drepl_enter();
    val = drepl_settle(JS_Eval(dctx, str, len, "<input>", drepl.eval_flags));
    drepl_leave();
    JS_FreeCString(ctx, str);
    error = JS_IsException(val);
    if (error) {
        val = JS_GetException(dctx);
    } else {
        /* the last result, as in the local REPL (a non writable '_'
           keeps its value) */
        global = JS_GetGlobalObject(dctx);
        if (JS_SetPropertyStr(dctx, global, "_", JS_DupValue(dctx, val)) < 0)
            JS_FreeValue(dctx, JS_GetException(dctx));
        JS_FreeValue(dctx, global);
    }
    /* repl.js prints the same prefix for its own exceptions */
    values[0] = drepl_render(ctx, val,
                             (error && !JS_IsError(dctx, val)) ? "Throw: " : "");
    values[1] = error ? JS_UNDEFINED : drepl_copy_number(ctx, val);
    values[2] = JS_NewBool(ctx, error);
    JS_FreeValue(dctx, val);
    drepl_save();
    if (JS_IsException(values[0]) || JS_IsException(values[1])) {
        JS_FreeValue(ctx, values[0]);
        JS_FreeValue(ctx, values[1]);
        return JS_EXCEPTION;
    }
    return JS_NewObjectFromStr(ctx, countof(props), props, values);
}

/* durable.query(src): a copy (through JSON) of the value of the
   expression 'src' in the durable runtime, for the completions. The
   durable runtime is not saved. */
static JSValue js_drepl_query(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSContext *dctx = drepl.ctx;
    JSValue val, json;
    const char *str;
    size_t len;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    drepl_enter();
    val = JS_Eval(dctx, str, len, "<completion>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, str);
    if (!JS_IsException(val)) {
        json = JS_JSONStringify(dctx, val, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(dctx, val);
        val = json;
    }
    drepl_leave();
    if (JS_IsException(val))
        return drepl_rethrow(ctx);
    if (JS_IsUndefined(val))
        return val; /* no JSON representation */
    str = JS_ToCStringLen(dctx, &len, val);
    JS_FreeValue(dctx, val);
    if (!str)
        return drepl_rethrow(ctx);
    val = JS_ParseJSON(ctx, str, len, "<completion>");
    JS_FreeCString(dctx, str);
    return val;
}

/* A new session: the standard objects, print() and console.log(). Every
   lazily created builtin (Math, the methods of the prototypes...) is
   instantiated, so that using one does not change the heap: the deltas
   only hold what the commands create. */
static JSContext *drepl_new_context(JSRuntime *rt)
{
    static const char instantiate[] =
        "(() => {"
        "  const seen = new Set();"
        "  (function walk(o) {"
        "    if ((typeof o !== 'object' && typeof o !== 'function') ||"
        "        o === null || seen.has(o))"
        "      return;"
        "    seen.add(o);"
        "    for (const k of Reflect.ownKeys(o)) {"
        "      const d = Object.getOwnPropertyDescriptor(o, k);"
        "      walk(d.value); walk(d.get); walk(d.set);"
        "    }"
        "    walk(Object.getPrototypeOf(o));"
        "  })(globalThis);"
        "})()";
    JSContext *ctx;
    JSValue global, console, val;

    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    global = JS_GetGlobalObject(ctx);
    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_std_console_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_std_print, "print", 1));
    JS_FreeValue(ctx, global);
    val = JS_Eval(ctx, instantiate, sizeof(instantiate) - 1, "<drepl>",
                  JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        JS_FreeContext(ctx);
        return NULL;
    }
    JS_FreeValue(ctx, val);
    return ctx;
}

/* Restore the durable runtime from 'delta', a delta of a new session
   whose base is rebuilt: the same bytes in every process of this qjs
   build. */
static int drepl_restore(const uint8_t *delta, size_t delta_len)
{
    JSSnapshotResult res;
    JSRuntime *base_rt;
    JSContext *base_ctx = NULL;
    uint8_t *base = NULL;
    size_t base_len;
    char err[256];
    int i, ret = -1;

    base_rt = JS_NewRuntime();
    if (base_rt)
        base_ctx = drepl_new_context(base_rt);
    if (base_ctx)
        base = JS_WriteSnapshot(base_ctx, &base_len, &drepl_host, NULL, 0,
                                JS_SNAPSHOT_BASE);
    if (!base) {
        snprintf(err, sizeof(err), "cannot rebuild the base of the session");
    } else if (JS_ReadSnapshot(drepl.rt, base, base_len, delta, delta_len,
                               &drepl_host, &res, err, sizeof(err)) == 0) {
        if (res.context_count == 1) {
            drepl.ctx = res.contexts[0];
            ret = 0;
        } else {
            for(i = 0; i < res.context_count; i++)
                JS_FreeContext(res.contexts[i]);
            snprintf(err, sizeof(err), "not a --drepl session");
        }
        JS_FreeSnapshotResult(drepl.rt, &res);
    }
    if (base)
        js_free(base_ctx, base);
    if (base_ctx)
        JS_FreeContext(base_ctx);
    if (base_rt)
        JS_FreeRuntime(base_rt);
    if (ret < 0)
        fprintf(stderr, "qjs: %s: %s\n", drepl.path, err);
    return ret;
}

/* Set up the durable runtime 'rt' for 'path': restored from the file if
   it exists, otherwise new and saved at once (so that an unwritable path
   fails before the REPL starts). 'ts_flags' are the TypeScript decorator
   flags of the commands. */
static int drepl_open(JSRuntime *rt, const char *path, int ts_flags)
{
    uint8_t *buf;
    size_t len;
    int ret;

    drepl.rt = rt;
    drepl.path = path;
    drepl.eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC |
        JS_EVAL_FLAG_BACKTRACE_BARRIER | JS_EVAL_FLAG_TYPESCRIPT | ts_flags;
    JS_SetInterruptHandler(rt, drepl_interrupt_handler, NULL);

    buf = js_load_file(NULL, &len, path);
    if (buf) {
        ret = drepl_restore(buf, len);
        free(buf);
        drepl.restored = (ret == 0);
        return ret;
    }
    if (errno != ENOENT) {
        perror(path);
        return -1;
    }
    drepl.ctx = drepl_new_context(rt);
    if (!drepl.ctx) {
        fprintf(stderr, "qjs: cannot allocate JS context\n");
        return -1;
    }
    /* the base of the deltas saved after each command */
    buf = JS_WriteSnapshot(drepl.ctx, &len, &drepl_host, NULL, 0,
                           JS_SNAPSHOT_BASE);
    if (!buf) {
        drepl_report("not saved");
        return -1;
    }
    js_free(drepl.ctx, buf);
    return drepl_save();
}

static void drepl_close(void)
{
    if (!drepl.rt)
        return;
    if (drepl.ctx)
        JS_FreeContext(drepl.ctx);
    JS_FreeRuntime(drepl.rt);
}

/* Define the options of repl.js: 'typescript' and, with --drepl, the
   'durable' object which runs the commands. */
static int set_repl_options(JSContext *ctx, BOOL typescript)
{
    static const JSCFunctionListEntry durable_funcs[] = {
        JS_CFUNC_DEF("eval", 1, js_drepl_eval ),
        JS_CFUNC_DEF("query", 1, js_drepl_query ),
    };
    JSValue options, durable, global;
    int ret;

    options = JS_NewObject(ctx);
    if (JS_IsException(options))
        return -1;
    JS_SetPropertyStr(ctx, options, "typescript", JS_NewBool(ctx, typescript));
    if (drepl.rt) {
        durable = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, durable, durable_funcs,
                                   countof(durable_funcs));
        JS_SetPropertyStr(ctx, durable, "path", JS_NewString(ctx, drepl.path));
        JS_SetPropertyStr(ctx, durable, "restored",
                          JS_NewBool(ctx, drepl.restored));
        JS_SetPropertyStr(ctx, options, "durable", durable);
    }
    global = JS_GetGlobalObject(ctx);
    ret = JS_SetPropertyStr(ctx, global, "__repl", options);
    JS_FreeValue(ctx, global);
    return ret;
}

/* apply the --memory-limit, --stack-size and strip options */
static void set_runtime_limits(JSRuntime *rt, size_t memory_limit,
                               size_t stack_size, int strip_flags)
{
    if (memory_limit != 0)
        JS_SetMemoryLimit(rt, memory_limit);
    if (stack_size != 0)
        JS_SetMaxStackSize(rt, stack_size);
    JS_SetStripInfo(rt, strip_flags);
}

#define PROG_NAME "qjs"

void help(int exit_status)
{
    printf("QuickJS version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [file [args]]  (file = - reads stdin)\n"
           "-h  --help         list options\n"
           "-v  --version      print the version and exit\n"
           "-e  --eval EXPR    evaluate EXPR\n"
           "-i  --interactive  go to interactive mode\n"
           "    --drepl file   durable interactive mode: the TypeScript commands\n"
           "                   run in a runtime saved to 'file' after each command\n"
           "                   and restored from it at startup\n"
           "-m  --module       load as ES6 module (default=autodetect)\n"
           "    --script       load as ES6 script (default=autodetect)\n"
           "    --strict       force strict mode\n"
           "    --typescript   parse the scripts as TypeScript (default: from the\n"
           "                   .ts, .mts and .cts file extensions)\n"
           "    --experimental-decorators  TypeScript legacy decorators\n"
           "    --emit-decorator-metadata  TypeScript legacy decorator metadata\n"
           "-I  --include file include an additional file\n"
           "    --std          make 'std', 'os' and 'bjson' available to the loaded script\n"
           "-T  --trace        trace memory allocation\n"
           "-d  --dump         dump the memory usage stats\n"
           "-D  --dump-flags hexmask  set the debug dump flags (JS_DUMP_xxx, default:\n"
           "                   $QJS_DUMP_FLAGS; needs CONFIG_DUMPS=y)\n"
           "    --memory-limit n  limit the memory usage to 'n' bytes (SI suffixes allowed)\n"
           "    --stack-size n    limit the stack size to 'n' bytes (SI suffixes allowed)\n"
           "    --no-unhandled-rejection  ignore unhandled promise rejections\n"
           "-s                    strip all the debug info\n"
           "    --strip-source    strip the source code\n"
#ifdef CONFIG_TEMPORAL_INTL
           "    --experimental    enable experimental Temporal and Intl support\n"
#endif
           "-q  --quit         just instantiate the interpreter and quit\n"
#ifdef CONFIG_PYTHON
           "    --python [python options] [args]  run Python 3.15 with the\n"
           "                   'qpy' command line (also implied by a .py file)\n"
#endif
           "The values of the long options can also be given as --option=value.\n");
    exit(exit_status);
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    struct trace_malloc_data trace_data = { NULL };
    int optind;
    char *expr = NULL;
    int interactive = 0;
    int dump_memory = 0;
    int trace_memory = 0;
    int empty_run = 0;
    int module = -1;
    int strict = 0;
    int typescript = 0;
    int ts_flags = 0;
    int load_std = 0;
    int dump_unhandled_promise_rejection = 1;
    size_t memory_limit = 0;
    char *include_list[32];
    int i, include_count = 0;
    int strip_flags = 0;
    size_t stack_size = 0;
    const char *drepl_path = NULL;
    uint64_t dump_flags = 0;
    const char *dump_flags_env;

    dump_flags_env = getenv("QJS_DUMP_FLAGS");
    if (dump_flags_env)
        dump_flags = strtoull(dump_flags_env, NULL, 16);

    qjs_argc = argc;
    qjs_argv = argv;

    /* cannot use getopt because we want to pass the command line to
       the script */
    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        char *arg = argv[optind] + 1;
        const char *longopt = "";
        char *optarg = NULL; /* value of --option=value */
        char longbuf[32];
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            /* the command line stays unmodified for execArgv */
            size_t len;
            arg++;
            optarg = strchr(arg, '=');
            len = optarg ? optarg++ - arg : strlen(arg);
            if (len >= sizeof(longbuf))
                len = sizeof(longbuf) - 1;
            memcpy(longbuf, arg, len);
            longbuf[len] = '\0';
            longopt = longbuf;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help(0);
            }
            if (opt == 'v' || !strcmp(longopt, "version")) {
                printf("%s\n", JS_GetVersion());
                return 0;
            }
            if (opt == 'e' || !strcmp(longopt, "eval")) {
                if (optarg) {
                    expr = optarg;
                    break;
                }
                if (*arg) {
                    expr = arg;
                    break;
                }
                if (optind < argc) {
                    expr = argv[optind++];
                    break;
                }
                fprintf(stderr, "qjs: missing expression for -e\n");
                exit(2);
            }
            if (opt == 'I' || !strcmp(longopt, "include")) {
                if (optarg) {
                    if (include_count >= countof(include_list)) {
                        fprintf(stderr, "too many included files");
                        exit(1);
                    }
                    include_list[include_count++] = optarg;
                    continue;
                }
                if (optind >= argc) {
                    fprintf(stderr, "expecting filename");
                    exit(1);
                }
                if (include_count >= countof(include_list)) {
                    fprintf(stderr, "too many included files");
                    exit(1);
                }
                include_list[include_count++] = argv[optind++];
                continue;
            }
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                continue;
            }
            if (!strcmp(longopt, "drepl")) {
                if (!optarg) {
                    if (optind >= argc) {
                        fprintf(stderr, "qjs: missing file for --drepl\n");
                        exit(2);
                    }
                    optarg = argv[optind++];
                }
                drepl_path = optarg;
                continue;
            }
            if (opt == 'm' || !strcmp(longopt, "module")) {
                module = 1;
                continue;
            }
            if (!strcmp(longopt, "script")) {
                module = 0;
                continue;
            }
            if (!strcmp(longopt, "strict")) {
                strict = 1;
                continue;
            }
            if (!strcmp(longopt, "typescript")) {
                typescript = 1;
                continue;
            }
            if (!strcmp(longopt, "experimental-decorators")) {
                ts_flags |= JS_EVAL_FLAG_TS_LEGACY_DECORATORS;
                continue;
            }
            if (!strcmp(longopt, "emit-decorator-metadata")) {
                ts_flags |= JS_EVAL_FLAG_TS_LEGACY_DECORATORS |
                    JS_EVAL_FLAG_TS_DECORATOR_METADATA;
                continue;
            }
            if (opt == 'd' || !strcmp(longopt, "dump")) {
                dump_memory++;
                continue;
            }
            if (opt == 'D' || !strcmp(longopt, "dump-flags")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting dump flags");
                    exit(1);
                }
                dump_flags = strtoull(argv[optind++], NULL, 16);
                continue;
            }
            if (opt == 'T' || !strcmp(longopt, "trace")) {
                trace_memory++;
                continue;
            }
            if (!strcmp(longopt, "std")) {
                load_std = 1;
                continue;
            }
            if (!strcmp(longopt, "no-unhandled-rejection")) {
                dump_unhandled_promise_rejection = 0;
                continue;
            }
#ifdef CONFIG_TEMPORAL_INTL
            if (!strcmp(longopt, "experimental")) {
                enable_experimental = TRUE;
                continue;
            }
#endif     
            if (opt == 'q' || !strcmp(longopt, "quit")) {
                empty_run++;
                continue;
            }
            if (!strcmp(longopt, "memory-limit")) {
                if (!optarg) {
                    if (optind >= argc) {
                        fprintf(stderr, "expecting memory limit");
                        exit(1);
                    }
                    optarg = argv[optind++];
                }
                memory_limit = get_suffixed_size(optarg);
                continue;
            }
            if (!strcmp(longopt, "stack-size")) {
                if (!optarg) {
                    if (optind >= argc) {
                        fprintf(stderr, "expecting stack size");
                        exit(1);
                    }
                    optarg = argv[optind++];
                }
                stack_size = get_suffixed_size(optarg);
                continue;
            }
            if (opt == 's') {
                strip_flags = JS_STRIP_DEBUG;
                continue;
            }
            if (!strcmp(longopt, "strip-source")) {
                strip_flags = JS_STRIP_SOURCE;
                continue;
            }
#ifdef CONFIG_PYTHON
            if (!strcmp(longopt, "python"))
                goto run_python;
#endif
            if (opt) {
                fprintf(stderr, "qjs: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "qjs: unknown option '--%s'\n", longopt);
            }
            help(1);
        }
    }

#ifdef CONFIG_PYTHON
    /* a .py script runs with the Python launcher; the slot before the
       Python arguments receives the program name (CPython's argv[0]) */
    if (optind < argc && !expr) {
        size_t len = strlen(argv[optind]);
        if (len > 3 && !strcmp(argv[optind] + len - 3, ".py")) {
        run_python:
            argv[optind - 1] = argv[0];
            return qp_main(argc - optind + 1, argv + optind - 1);
        }
    }
#endif

    if (trace_memory) {
        js_trace_malloc_init(&trace_data);
        rt = JS_NewRuntime2(&trace_mf, &trace_data);
    } else {
        rt = JS_NewRuntime();
    }
    if (!rt) {
        fprintf(stderr, "qjs: cannot allocate JS runtime\n");
        exit(2);
    }
    JS_SetDumpFlags(rt, dump_flags);
    set_runtime_limits(rt, memory_limit, stack_size, strip_flags);
    if (drepl_path) {
        JSRuntime *drt;
        if (expr || optind < argc || include_count || load_std
#ifdef CONFIG_TEMPORAL_INTL
            || enable_experimental
#endif
            ) {
            fprintf(stderr, "qjs: --drepl cannot be combined with a script, -e, -I, --std or --experimental\n");
            exit(2);
        }
        drt = JS_NewRuntime();
        if (!drt) {
            fprintf(stderr, "qjs: cannot allocate JS runtime\n");
            exit(2);
        }
        JS_SetDumpFlags(drt, dump_flags);
        set_runtime_limits(drt, memory_limit, stack_size, strip_flags);
        if (drepl_open(drt, drepl_path, ts_flags) < 0) {
            drepl_close();
            JS_FreeRuntime(rt);
            return 1;
        }
    }
    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);
    js_std_set_typescript_flags(rt, ts_flags);
    ctx = JS_NewCustomContext(rt);
    if (!ctx) {
        fprintf(stderr, "qjs: cannot allocate JS context\n");
        exit(2);
    }

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, js_module_normalize, js_module_loader, js_module_check_attributes, NULL);

    if (dump_unhandled_promise_rejection) {
        JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker,
                                          NULL);
    }

    if (!empty_run) {
        js_std_add_helpers(ctx, argc - optind, argv + optind);

        /* make 'std' and 'os' visible to non module code */
        if (load_std) {
            const char *str = "import * as std from 'std';\n"
                "import * as os from 'os';\n"
                "import * as bjson from 'bjson';\n"
                "globalThis.std = std;\n"
                "globalThis.os = os;\n"
                "globalThis.bjson = bjson;\n";
            eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE);
        }

        for(i = 0; i < include_count; i++) {
            if (eval_file(ctx, include_list[i], 0, strict, typescript))
                goto fail;
        }

        if (expr) {
            int eval_flags;
            if (module > 0) {
                eval_flags = JS_EVAL_TYPE_MODULE;
            } else {
                eval_flags = JS_EVAL_TYPE_GLOBAL;
                if (strict)
                    eval_flags |= JS_EVAL_FLAG_STRICT;
            }
            if (typescript)
                eval_flags |= js_std_get_typescript_flag(ctx, ".ts");
            if (eval_buf(ctx, expr, strlen(expr), "<cmdline>", eval_flags))
                goto fail;
        } else
        if (optind >= argc) {
            /* interactive mode */
            interactive = 1;
        } else {
            const char *filename;
            filename = argv[optind];
            if (eval_file(ctx, filename, module, strict, typescript))
                goto fail;
        }
        if (interactive) {
            JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
            if (set_repl_options(ctx, typescript || drepl_path) < 0) {
                js_std_dump_error(ctx);
                goto fail;
            }
            js_std_eval_binary(ctx, qjsc_repl, qjsc_repl_size, 0);
        }
        js_std_loop(ctx);
    }

    if (dump_memory) {
        JSMemoryUsage stats;
        JS_ComputeMemoryUsage(rt, &stats);
        JS_DumpMemoryUsage(stdout, &stats, rt);
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    drepl_close();

    if (empty_run && dump_memory) {
        clock_t t[5];
        double best[5];
        int i, j;
        for (i = 0; i < 100; i++) {
            t[0] = clock();
            rt = JS_NewRuntime();
            t[1] = clock();
            ctx = JS_NewContext(rt);
            t[2] = clock();
            JS_FreeContext(ctx);
            t[3] = clock();
            JS_FreeRuntime(rt);
            t[4] = clock();
            for (j = 4; j > 0; j--) {
                double ms = 1000.0 * (t[j] - t[j - 1]) / CLOCKS_PER_SEC;
                if (i == 0 || best[j] > ms)
                    best[j] = ms;
            }
        }
        printf("\nInstantiation times (ms): %.3f = %.3f+%.3f+%.3f+%.3f\n",
               best[1] + best[2] + best[3] + best[4],
               best[1], best[2], best[3], best[4]);
    }
    return 0;
 fail:
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    drepl_close();
    return 1;
}
