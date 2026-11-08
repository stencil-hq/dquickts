/*
 * qbun:async: the async context of AsyncLocalStorage.
 *
 * The engine carries one value per runtime along the asynchronous
 * continuations (JS_GetAsyncContext()/JS_SetAsyncContext() in quickjs.h):
 * promise reactions (then, await) capture it when registered, other jobs
 * (queueMicrotask...) when enqueued, and run with it. internal:async-context
 * stores there an immutable frame (a frozen Map of AsyncLocalStorage →
 * store) and binds the host callbacks (timers, I/O) itself.
 */
#include "qbun.h"

/* getContext(): the current context (undefined when none) */
static JSValue qb_async_get_context(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    return JS_GetAsyncContext(JS_GetRuntime(ctx));
}

/* setContext(frame): make 'frame' the current context */
static JSValue qb_async_set_context(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    JS_SetAsyncContext(JS_GetRuntime(ctx), JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

/* exchangeContext(frame): make 'frame' the current context, return the
   previous one */
static JSValue qb_async_exchange_context(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue prev = JS_GetAsyncContext(rt);
    JS_SetAsyncContext(rt, JS_DupValue(ctx, argv[0]));
    return prev;
}

static const JSCFunctionListEntry qb_async_funcs[] = {
    JS_CFUNC_DEF("getContext", 0, qb_async_get_context),
    JS_CFUNC_DEF("setContext", 1, qb_async_set_context),
    JS_CFUNC_DEF("exchangeContext", 1, qb_async_exchange_context),
};

QB_SIMPLE_MODULE(async, qb_async_funcs)
