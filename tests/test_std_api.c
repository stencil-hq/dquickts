/*
 * quickjs-libc tests which need the C API
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#include "../quickjs-libc.h"

static int interrupt_count;

/* host interrupt handler: '*opaque' tells if the JS code must be
   interrupted */
static int host_interrupt_handler(JSRuntime *rt, void *opaque)
{
    interrupt_count++;
    return *(int *)opaque;
}

static void check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "test_std_api: %s\n", msg);
        exit(1);
    }
}

static JSValue eval_str(JSContext *ctx, const char *str, int flags)
{
    return JS_Eval(ctx, str, strlen(str), "<test>", flags);
}

/* std.evalScript() installs its own interrupt handler (for SIGINT in
   the REPL): the handler of the host must still be called during the
   evaluation, it must be restored afterwards and its interruptions
   must stay uncatchable */
static void test_eval_script_interrupt(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;
    void *opaque;
    int stop = 0;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    check(rt && ctx, "cannot create the context");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    js_init_module_std(ctx, "std");
    val = eval_str(ctx, "import * as std from 'std'; globalThis.std = std;",
                   JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    check(!JS_IsException(val), "cannot import std");
    JS_FreeValue(ctx, val);

    JS_SetInterruptHandler(rt, host_interrupt_handler, &stop);
    interrupt_count = 0;
    val = eval_str(ctx, "std.evalScript('for (let i = 0; i < 1e6; i++);')",
                   JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(val), "evalScript failed");
    JS_FreeValue(ctx, val);
    check(interrupt_count > 0, "host interrupt handler not called");
    check(JS_GetInterruptHandler(rt, &opaque) == host_interrupt_handler &&
          opaque == &stop, "host interrupt handler not restored");

    stop = 1;
    val = eval_str(ctx,
                   "try { std.evalScript('for (;;);') } catch (e) { 'caught' }",
                   JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(val), "host interruption was caught");
    check(JS_IsUncatchableException(ctx), "host interruption is catchable");
    JS_FreeValue(ctx, JS_GetException(ctx));

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* a context with the 'std' and 'os' modules as globals */
static JSContext *new_std_context(JSRuntime *rt)
{
    JSContext *ctx;
    JSValue val;

    ctx = JS_NewContext(rt);
    check(ctx != NULL, "cannot create the context");
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    val = eval_str(ctx, "import * as std from 'std'; import * as os from 'os';"
                   "globalThis.std = std; globalThis.os = os;",
                   JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    check(!JS_IsException(val), "cannot import std and os");
    JS_FreeValue(ctx, val);
    return ctx;
}

static void eval_check(JSContext *ctx, const char *str)
{
    JSValue val = eval_str(ctx, str, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        check(0, str);
    }
    JS_FreeValue(ctx, val);
}

/* value of the global expression 'str' as an int32 */
static int eval_int(JSContext *ctx, const char *str)
{
    JSValue val = eval_str(ctx, str, JS_EVAL_TYPE_GLOBAL);
    int v = -1;
    check(!JS_IsException(val) && JS_ToInt32(ctx, &v, val) == 0, str);
    JS_FreeValue(ctx, val);
    return v;
}

/* the pending exception is an error whose message is 'msg' */
static void check_exception(JSContext *ctx, const char *msg)
{
    JSValue exc = JS_GetException(ctx);
    JSValue m = JS_GetPropertyStr(ctx, exc, "message");
    const char *str = JS_ToCString(ctx, m);
    check(str && !strcmp(str, msg), msg);
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, exc);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* js_std_loop_once() and js_std_poll_io(): an event loop driven by
   the host */
static void test_loop_once(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    int ret, i;
    int64_t t0;

    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);

    check(js_std_loop_once(ctx) == -1, "loop_once: idle");

    /* the jobs are run */
    eval_check(ctx, "globalThis.n = 0; Promise.resolve().then(() => n++);");
    check(js_std_loop_once(ctx) == -1, "loop_once: jobs");
    check(eval_int(ctx, "n") == 1, "loop_once: job not run");

    /* the delay until the next timer, at most one timer per call */
    eval_check(ctx, "os.setTimeout(() => n++, 0); os.setTimeout(() => n++, 0);"
               "os.setTimeout(() => n += 10, 30);");
    check(js_std_loop_once(ctx) == 0, "loop_once: expired timer");
    check(eval_int(ctx, "n") == 2, "loop_once: first timer");
    check(js_std_loop_once(ctx) == 0, "loop_once: second expired timer");
    check(eval_int(ctx, "n") == 3, "loop_once: second timer");
    ret = js_std_loop_once(ctx);
    check(ret > 0 && ret <= 30, "loop_once: delay of the next timer");
    for(i = 0; i < 100 && (ret = js_std_loop_once(ctx)) > 0; i++)
        js_std_poll_io(ctx, ret);
    check(ret == 0 && eval_int(ctx, "n") == 13, "loop_once: delayed timer");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle after the timers");

    /* the jobs queued by a timer are run by the next call */
    eval_check(ctx, "os.setTimeout(() => Promise.resolve().then(() => n++), 0);");
    check(js_std_loop_once(ctx) == 0, "loop_once: job of a timer pending");
    check(eval_int(ctx, "n") == 13, "loop_once: job of a timer run early");
    check(js_std_loop_once(ctx) == -1, "loop_once: job of a timer");
    check(eval_int(ctx, "n") == 14, "loop_once: job of a timer not run");

    /* the exceptions are left to the host */
    eval_check(ctx, "os.setTimeout(() => { throw Error('timer') }, 0);");
    check(js_std_loop_once(ctx) == -2, "loop_once: throwing timer");
    check_exception(ctx, "timer");
    eval_check(ctx, "queueMicrotask(() => { throw Error('job') });");
    check(js_std_loop_once(ctx) == -2, "loop_once: throwing job");
    check_exception(ctx, "job");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle after errors");

    /* poll_io() does not run the timers and returns at once if there
       is nothing to wait for */
    eval_check(ctx, "globalThis.tid = os.setTimeout(() => n++, 0);");
    t0 = now_ms();
    check(js_std_poll_io(ctx, -1) == 0, "poll_io: no handler");
    check(now_ms() - t0 < 1000, "poll_io: waited without handler");
    check(eval_int(ctx, "n") == 14, "poll_io: timer run");
    eval_check(ctx, "os.clearTimeout(tid);");

    /* read handlers */
    eval_check(ctx,
               "globalThis.fds = os.pipe(); globalThis.got = 0;"
               "os.setReadHandler(fds[0], () => {"
               "  var b = new Uint8Array(1);"
               "  os.read(fds[0], b.buffer, 0, 1);"
               "  if (b[0] == 0x21) throw Error('handler');"
               "  got = b[0];"
               "});");
    t0 = now_ms();
    check(js_std_poll_io(ctx, 20) == 0, "poll_io: timeout");
    check(now_ms() - t0 >= 15, "poll_io: timeout not waited");
    check(eval_int(ctx, "got") == 0, "poll_io: handler without data");
    eval_check(ctx, "os.write(fds[1], new Uint8Array([42]).buffer, 0, 1);");
    check(js_std_poll_io(ctx, 1000) == 0, "poll_io: data");
    check(eval_int(ctx, "got") == 42, "poll_io: handler not called");
    eval_check(ctx, "os.write(fds[1], new Uint8Array([0x21]).buffer, 0, 1);");
    check(js_std_poll_io(ctx, 1000) == -2, "poll_io: throwing handler");
    check_exception(ctx, "handler");
    eval_check(ctx, "os.setReadHandler(fds[0], null);"
               "os.close(fds[0]); os.close(fds[1]);");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle at the end");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static int wait_enter_count, wait_leave_count;

static void *count_wait_enter(JSRuntime *rt)
{
    wait_enter_count++;
    return &wait_enter_count;
}

static void count_wait_leave(JSRuntime *rt, void *state)
{
    check(state == &wait_enter_count, "wait hooks: state");
    wait_leave_count++;
}

/* a thread which adds a timer while the event loop waits (the wait
   hooks give it the runtime, as the GIL of Python does) */
static pthread_t wake_thread;
static int wake_thread_started;
static JSContext *wake_ctx;

static void *wake_thread_func(void *opaque)
{
    JSContext *ctx = opaque;
    usleep(20 * 1000);
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    eval_check(ctx, "setTimeout(() => log.push('woken'), 0);");
    return NULL;
}

static void *wake_wait_enter(JSRuntime *rt)
{
    if (!wake_thread_started) {
        check(pthread_create(&wake_thread, NULL, wake_thread_func, wake_ctx) == 0,
              "cannot create the thread");
        wake_thread_started = 1;
    }
    return NULL;
}

static void wake_wait_leave(JSRuntime *rt, void *state)
{
    if (wake_thread_started == 1) {
        pthread_join(wake_thread, NULL);
        wake_thread_started = 2;
    }
    JS_UpdateStackTop(rt);
}

/* the timers of js_std_add_helpers() without the 'os' module,
   js_std_poll() and js_std_set_wait_hooks() */
static void test_global_timers(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    int ret, i;
    int64_t t0;

    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    ctx = JS_NewContext(rt);
    check(ctx != NULL, "cannot create the context");
    /* without the event loop, the timers throw */
    js_std_add_helpers(ctx, -1, NULL);
    check(!JS_HasException(ctx), "add_helpers failed");
    check(JS_IsException(eval_str(ctx, "setTimeout(() => 0, 0)", JS_EVAL_TYPE_GLOBAL)),
          "setTimeout without js_std_init_handlers()");
    JS_FreeValue(ctx, JS_GetException(ctx));
    js_std_init_handlers(rt);

    /* js_std_loop() runs them */
    eval_check(ctx, "globalThis.log = [];"
               "setTimeout(() => log.push('t'), 10);"
               "globalThis.iid = setInterval(() => { log.push('i'); clearInterval(iid); }, 5);");
    js_std_loop(ctx);
    check(eval_int(ctx, "log.join() == 'i,t'"), "loop: global timers");

    /* js_std_poll(): one event per call, the waits between the hooks,
       the exceptions left to the host */
    js_std_set_wait_hooks(rt, count_wait_enter, count_wait_leave);
    check(js_std_poll(ctx) == -1, "poll: idle");
    eval_check(ctx, "log = []; setTimeout(() => { throw Error('timer') }, 15);"
               "setTimeout(() => log.push(1), 5);");
    for(i = 0; i < 100 && (ret = js_std_poll(ctx)) == 0; i++)
        continue;
    check(ret == -2, "poll: throwing timer");
    check_exception(ctx, "timer");
    check(eval_int(ctx, "log.join() == '1'"), "poll: timer before the throwing one");
    check(js_std_poll(ctx) == -1, "poll: idle after the timers");
    check(wait_enter_count > 0 && wait_enter_count == wait_leave_count,
          "poll: wait hooks");

    /* a timer added by another thread during the wait ends it */
    wake_ctx = ctx;
    js_std_set_wait_hooks(rt, wake_wait_enter, wake_wait_leave);
    eval_check(ctx, "log = []; globalThis.tid = setTimeout(() => log.push('late'), 5000);");
    t0 = now_ms();
    for(i = 0; i < 100 && !eval_int(ctx, "log.length"); i++)
        check(js_std_poll(ctx) == 0, "poll: wake");
    check(eval_int(ctx, "log.join() == 'woken'"), "poll: timer of the other thread");
    check(now_ms() - t0 < 2000, "poll: the wait was not ended");
    eval_check(ctx, "clearTimeout(tid);");
    js_std_set_wait_hooks(rt, NULL, NULL);

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* test_worker_module.js replies "done" to "abort" */
static const char worker_src[] =
    "const w = new os.Worker('./test_worker_module.js');\n"
    "globalThis.done = 0;\n"
    "w.onmessage = (e) => {\n"
    "    if (e.data.type == 'done') {\n"
    "        done = 1;\n"
    "        w.onmessage = null;\n"
    "    }\n"
    "};\n"
    "w.postMessage({ type: 'abort' });\n";

static atomic_int worker_runtime_count;
static JSRuntime *_Atomic worker_runtime;

static JSRuntime *worker_new_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    atomic_fetch_add(&worker_runtime_count, 1);
    worker_runtime = rt;
    return rt;
}

static JSContext *worker_new_context(JSRuntime *rt)
{
    JSContext *ctx;
    /* the runtime is the one of worker_new_runtime() */
    if (rt != worker_runtime)
        return NULL;
    ctx = JS_NewContext(rt);
    if (ctx) {
        js_init_module_std(ctx, "std");
        js_init_module_os(ctx, "os");
    }
    return ctx;
}

/* js_std_set_worker_new_runtime_func(): the runtimes of the workers
   are created by the host */
static void test_worker_new_runtime(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;

    js_std_set_worker_new_runtime_func(worker_new_runtime);
    js_std_set_worker_new_context_func(worker_new_context);
    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);
    /* the worker module name is relative to the module name */
    val = JS_Eval(ctx, worker_src, strlen(worker_src),
                  "tests/test_std_api.js", JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    if (JS_IsException(val))
        js_std_dump_error(ctx);
    check(!JS_IsException(val), "cannot create the worker");
    JS_FreeValue(ctx, val);
    js_std_loop(ctx);
    check(eval_int(ctx, "done") == 1, "no reply from the worker");
    check(worker_runtime_count == 1, "worker runtime hook not called");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    js_std_set_worker_new_runtime_func(JS_NewRuntime);
}

/* bellard/quickjs#538: without js_std_set_worker_new_context_func(),
   the Worker constructor throws instead of calling a NULL function in
   the worker thread */
static void test_worker_no_context_func(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;

    js_std_set_worker_new_context_func(NULL);
    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);
    val = JS_Eval(ctx, worker_src, strlen(worker_src),
                  "tests/test_std_api.js", JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    check(JS_IsException(val), "worker created without a context function");
    check_exception(ctx, "worker context function not set");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* "abort" makes the worker exit after its first messages */
static const char worker_cycle_src[] =
    "globalThis.n = 0;\n"
    "globalThis.h = { w: new os.Worker('./test_worker_module.js') };\n"
    "h.w.self_ref = h.w;\n"
    "globalThis.wr = new WeakRef(h.w);\n"
    "h.w.onmessage = (e) => { n++; };\n"
    "h.w.postMessage({ type: 'abort' });\n"
    "h.w = null;\n";

/* bellard/quickjs#527: the GC run while the message event is built
   collects the Worker object (only referenced by itself) and frees its
   message handler */
static void test_worker_gc_in_message(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;

    js_std_set_worker_new_runtime_func(worker_new_runtime);
    js_std_set_worker_new_context_func(worker_new_context);
    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);
    val = JS_Eval(ctx, worker_cycle_src, strlen(worker_cycle_src),
                  "tests/test_std_api.js", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(val), "gc_in_message: cannot create the worker");
    JS_FreeValue(ctx, val);
    /* the next object allocation runs the GC */
    JS_SetGCThreshold(rt, 0);
    check(js_std_poll_io(ctx, 10000) == 0, "gc_in_message: poll");
    check(eval_int(ctx, "n") == 1, "gc_in_message: message not handled");
    check(eval_int(ctx, "+(wr.deref() === undefined)") == 1,
          "gc_in_message: worker not collected");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    js_std_set_worker_new_runtime_func(JS_NewRuntime);
}

/* create a context with the std, os and bjson modules imported. Return
   0 if a step failed (cleanly) because of the memory limit */
static JS_BOOL init_std_context(JSRuntime *rt)
{
    static const char src[] =
        "import * as std from 'std';"
        "import * as os from 'os';"
        "import * as bjson from 'bjson';"
        "globalThis.ok = typeof std.out.puts === 'function' &&"
        "  typeof os.setTimeout === 'function' &&"
        "  typeof bjson.write === 'function' && typeof print === 'function';";
    JSContext *ctx, *ctx1;
    JSValue val;
    JS_BOOL ok = 1;
    int ret;

    ctx = JS_NewContext(rt);
    if (!ctx)
        return 0;
    js_std_add_helpers(ctx, 0, NULL);
    if (JS_HasException(ctx)) {
        ok = 0;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (!js_init_module_std(ctx, "std") || !js_init_module_os(ctx, "os") ||
        !js_init_module_bjson(ctx, "bjson")) {
        ok = 0;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    val = eval_str(ctx, src, JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        ok = 0;
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
        while ((ret = JS_ExecutePendingJob(rt, &ctx1)) != 0) {
            if (ret < 0)
                JS_FreeValue(ctx1, JS_GetException(ctx1));
        }
        if (JS_PromiseState(ctx, val) != JS_PROMISE_FULFILLED)
            ok = 0;
        JS_FreeValue(ctx, val);
    }
#ifdef CONFIG_TEMPORAL_INTL
    if (JS_AddIntrinsicTemporal(ctx) < 0 || JS_AddIntrinsicIntl(ctx) < 0) {
        ok = 0;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
#endif
    if (ok) {
        val = eval_str(ctx, "ok", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
            ok = 0;
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            check(JS_ToBool(ctx, val) == 1, "std/os/bjson not imported");
        }
        JS_FreeValue(ctx, val);
    }
    JS_FreeContext(ctx);
    return ok;
}

/* regression test: the context and module initialization failures
   under a memory limit were not all checked (upstream bellard/quickjs
   issue 404: js_std_init() ignored a JS_NewClass() failure and
   JS_SetClassProto() asserted) */
static void test_init_oom(void)
{
    JSRuntime *rt;
    JSMemoryUsage stats;
    size_t limit;
    int n_failed = 0;
    JS_BOOL ok;

    for(limit = 0;; limit += 64) {
        check(limit < 16 << 20, "the initialization never succeeds");
        rt = JS_NewRuntime();
        check(rt != NULL, "cannot create the runtime");
        js_std_init_handlers(rt);
        JS_ComputeMemoryUsage(rt, &stats);
        JS_SetMemoryLimit(rt, stats.malloc_size + limit);
        ok = init_std_context(rt);
        js_std_free_handlers(rt);
        JS_FreeRuntime(rt); /* asserts that no object is leaked */
        if (ok)
            break;
        n_failed++;
    }
    check(n_failed > 0, "no allocation failure");
}

int main(int argc, char **argv)
{
    /* first: no 'os' module was imported before */
    test_global_timers();
    test_eval_script_interrupt();
    test_loop_once();
    test_worker_new_runtime();
    test_worker_no_context_func();
    test_worker_gc_in_message();
    test_init_oom();
    return 0;
}
