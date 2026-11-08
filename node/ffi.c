/* bun:ffi's ABI bridge. libffi owns the machine-code generation. */
#include "qbun.h"
#include <ffi.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef union {
    ffi_arg word;
    int8_t i8;
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    float f32;
    double f64;
    void *ptr;
} QBFFIValue;
typedef struct {
    void *handle;
} QBFFILib;
typedef struct QBFFI QBFFI;
typedef struct QBFFIRequest {
    struct QBFFIRequest *next;
    void **args;
    void *result;
    uv_sem_t done;
} QBFFIRequest;
struct QBFFI {
    ffi_cif cif;
    ffi_type **types;
    int *codes;
    unsigned count;
    int returns;
    void *address;
    ffi_closure *closure;
    JSContext *ctx;
    JSValue callback;
    uv_thread_t thread;
    uv_async_t async;
    uv_mutex_t mutex;
    QBFFIRequest *head, *tail;
    int threadsafe, closed;
};

static int pointer_value(JSContext *ctx, JSValueConst value, void **out)
{
    int64_t n;
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        *out = NULL;
        return 0;
    }
    if (JS_IsObject(value)) {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, value, &len);
        if (!p)
            return -1;
        *out = p;
        return 0;
    }
    if (JS_ToInt64Ext(ctx, &n, value))
        return -1;
    *out = (void *)(uintptr_t)n;
    return 0;
}
static ffi_type *ffi_type_code(int code)
{
    switch (code) {
    case 0:
    case 1:
        return &ffi_type_sint8;
    case 2:
    case 11:
        return &ffi_type_uint8;
    case 3:
        return &ffi_type_sint16;
    case 4:
        return &ffi_type_uint16;
    case 5:
        return &ffi_type_sint32;
    case 6:
        return &ffi_type_uint32;
    case 7:
    case 15:
        return &ffi_type_sint64;
    case 8:
    case 16:
        return &ffi_type_uint64;
    case 9:
        return &ffi_type_double;
    case 10:
        return &ffi_type_float;
    case 12:
    case 14:
    case 17:
    case 20:
        return &ffi_type_pointer;
    case 13:
        return &ffi_type_void;
    case 21:
        return &ffi_type_uint64;
    default:
        return NULL;
    }
}
static int value_in(JSContext *ctx, int code, JSValueConst v, QBFFIValue *out)
{
    int64_t i;
    double d;
    memset(out, 0, sizeof(*out));
    if (code == 12 || code == 14 || code == 17 || code == 20)
        return pointer_value(ctx, v, &out->ptr);
    if (code == 13)
        return 0;
    if (code == 21) {
        size_t length;
        if (!qb_get_bytes(ctx, v, &length))
            return -1;
        out->u64 = length;
        return 0;
    }
    if (code == 11) {
        int b = JS_ToBool(ctx, v);
        if (b < 0)
            return -1;
        out->u8 = b;
        return 0;
    }
    if (code == 9 || code == 10) {
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (code == 9)
            out->f64 = d;
        else
            out->f32 = (float)d;
        return 0;
    }
    if (JS_ToInt64Ext(ctx, &i, v))
        return -1;
    switch (code) {
    case 0:
    case 1:
        out->i8 = i;
        break;
    case 2:
        out->u8 = i;
        break;
    case 3:
        out->i16 = i;
        break;
    case 4:
        out->u16 = i;
        break;
    case 5:
        out->i32 = i;
        break;
    case 6:
        out->u32 = i;
        break;
    default:
        out->i64 = i;
    }
    return 0;
}
static JSValue value_out(JSContext *ctx, int code, const void *p)
{
    QBFFIValue v;
    memset(&v, 0, sizeof(v));
    ffi_type *t = ffi_type_code(code);
    if (t && code != 13)
        memcpy(&v, p, t->size);
    switch (code) {
    case 0:
    case 1:
        return JS_NewInt32(ctx, v.i8);
    case 2:
        return JS_NewInt32(ctx, v.u8);
    case 3:
        return JS_NewInt32(ctx, v.i16);
    case 4:
        return JS_NewInt32(ctx, v.u16);
    case 5:
        return JS_NewInt32(ctx, v.i32);
    case 6:
        return JS_NewUint32(ctx, v.u32);
    case 7:
        return JS_NewBigInt64(ctx, v.i64);
    case 8:
        return JS_NewBigUint64(ctx, v.u64);
    case 15:
        if (v.i64 >= -9007199254740991LL && v.i64 <= 9007199254740991LL)
            return JS_NewFloat64(ctx, (double)v.i64);
        return JS_NewBigInt64(ctx, v.i64);
    case 16:
        if (v.u64 <= 9007199254740991ULL)
            return JS_NewFloat64(ctx, (double)v.u64);
        return JS_NewBigUint64(ctx, v.u64);
    case 9:
        return JS_NewFloat64(ctx, v.f64);
    case 10:
        return JS_NewFloat64(ctx, v.f32);
    case 11:
        return JS_NewBool(ctx, v.u8 != 0);
    case 13:
        return JS_UNDEFINED;
    case 21:
        return JS_NewFloat64(ctx, (double)v.u64);
    default:
        return JS_NewFloat64(ctx, (double)(uintptr_t)v.ptr);
    }
}
static void lib_free(JSRuntime *rt, void *opaque, void *ptr)
{
    QBFFILib *l = opaque;
    (void)rt;
    (void)ptr;
    if (l->handle)
        dlclose(l->handle);
    free(l);
}
static QBFFILib *get_lib(JSContext *ctx, JSValueConst v)
{
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    if (!p || n != sizeof(QBFFILib)) {
        JS_ThrowTypeError(ctx, "Invalid FFI library handle");
        return NULL;
    }
    return (QBFFILib *)p;
}
static JSValue lib_open(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    void *h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    JS_FreeCString(ctx, path);
    if (!h)
        return JS_ThrowTypeError(ctx, "%s", dlerror());
    QBFFILib *l = calloc(1, sizeof(*l));
    if (!l) {
        dlclose(h);
        return JS_ThrowOutOfMemory(ctx);
    }
    l->handle = h;
    return JS_NewArrayBuffer(ctx, (uint8_t *)l, sizeof(*l), lib_free, l, 0);
}
static JSValue lib_close(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    QBFFILib *l = get_lib(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    if (l->handle) {
        dlclose(l->handle);
        l->handle = NULL;
    }
    return JS_UNDEFINED;
}
static JSValue lib_symbol(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    QBFFILib *l = get_lib(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    if (!l->handle)
        return JS_ThrowTypeError(ctx, "Library has been closed");
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name)
        return JS_EXCEPTION;
    dlerror();
    void *p = dlsym(l->handle, name);
    const char *e = dlerror();
    JS_FreeCString(ctx, name);
    if (e)
        return JS_ThrowTypeError(ctx, "%s", e);
    return JS_NewFloat64(ctx, (double)(uintptr_t)p);
}
static void cif_destroy(QBFFI *f)
{
    free(f->types);
    free(f->codes);
    free(f);
}
static void cif_free(JSRuntime *rt, void *opaque, void *ptr)
{
    (void)rt;
    (void)ptr;
    cif_destroy(opaque);
}
static QBFFI *prepare(JSContext *ctx, JSValueConst args, JSValueConst returns)
{
    QBFFI *f = calloc(1, sizeof(*f));
    if (!f) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    JSValue len = JS_GetPropertyStr(ctx, args, "length");
    uint32_t n;
    int rc = JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    if (rc)
        goto fail;
    f->count = n;
    f->ctx = ctx;
    f->callback = JS_UNDEFINED;
    f->types = calloc(n ? n : 1, sizeof(*f->types));
    f->codes = calloc(n ? n : 1, sizeof(*f->codes));
    if (!f->types || !f->codes) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (JS_ToInt32(ctx, &f->returns, returns))
        goto fail;
    ffi_type *ret = ffi_type_code(f->returns);
    if (!ret)
        goto unsupported;
    for (unsigned i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, args, i);
        rc = JS_ToInt32(ctx, &f->codes[i], v);
        JS_FreeValue(ctx, v);
        if (rc)
            goto fail;
        f->types[i] = ffi_type_code(f->codes[i]);
        if (!f->types[i] || f->codes[i] == 13)
            goto unsupported;
    }
    if (ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, n, ret, f->types) != FFI_OK) {
        JS_ThrowTypeError(ctx, "Invalid FFI signature");
        goto fail;
    }
    return f;
unsupported:
    qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_NOT_SUPPORTED",
                  "Unsupported FFI type (napi_env/napi_value are not supported)");
fail:
    cif_destroy(f);
    return NULL;
}
static JSValue invoke(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                      JSValue *data)
{
    size_t len;
    QBFFI *f = (QBFFI *)JS_GetArrayBuffer(ctx, &len, data[0]);
    if (!f)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(data[1])) {
        QBFFILib *l = get_lib(ctx, data[1]);
        if (!l)
            return JS_EXCEPTION;
        if (!l->handle)
            return JS_ThrowTypeError(ctx, "Library has been closed");
    }
    if ((unsigned)argc < f->count)
        return JS_ThrowTypeError(ctx, "Expected %u arguments, received %d", f->count, argc);
    QBFFIValue values[f->count ? f->count : 1], result;
    void *pointers[f->count ? f->count : 1];
    for (unsigned i = 0; i < f->count; i++) {
        if (value_in(ctx, f->codes[i], argv[i], &values[i]))
            return JS_EXCEPTION;
        pointers[i] = &values[i];
    }
    memset(&result, 0, sizeof(result));
    ffi_call(&f->cif, FFI_FN(f->address), &result, pointers);
    return value_out(ctx, f->returns, &result);
}
static JSValue make_function(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    QBFFI *f = prepare(ctx, argv[1], argv[2]);
    if (!f)
        return JS_EXCEPTION;
    if (pointer_value(ctx, argv[0], &f->address)) {
        cif_destroy(f);
        return JS_EXCEPTION;
    }
    if (!f->address) {
        cif_destroy(f);
        return JS_ThrowTypeError(ctx, "FFI function pointer must not be null");
    }
    JSValue data[2] = {JS_NewArrayBuffer(ctx, (uint8_t *)f, sizeof(*f), cif_free, f, 0),
                       argc > 3 ? JS_DupValue(ctx, argv[3]) : JS_UNDEFINED};
    if (JS_IsException(data[0])) {
        JS_FreeValue(ctx, data[1]);
        return JS_EXCEPTION;
    }
    JSValue fn = JS_NewCFunctionData(ctx, invoke, f->count, 0, 2, data);
    JS_FreeValue(ctx, data[0]);
    JS_FreeValue(ctx, data[1]);
    return fn;
}
static void callback_run(QBFFI *f, void *result, void **args)
{
    JSValue values[f->count ? f->count : 1];
    for (unsigned i = 0; i < f->count; i++)
        values[i] = value_out(f->ctx, f->codes[i], args[i]);
    JSValue ret = qb_call(f->ctx, f->callback, JS_UNDEFINED, f->count, values);
    QBFFIValue converted;
    if (f->returns != 13) {
        if (value_in(f->ctx, f->returns, ret, &converted) == 0)
            memcpy(result, &converted, f->cif.rtype->size);
        else
            JS_FreeValue(f->ctx, JS_GetException(f->ctx));
    }
    JS_FreeValue(f->ctx, ret);
    for (unsigned i = 0; i < f->count; i++)
        JS_FreeValue(f->ctx, values[i]);
}
static void callback_async(uv_async_t *handle)
{
    QBFFI *f = handle->data;
    for (;;) {
        uv_mutex_lock(&f->mutex);
        QBFFIRequest *r = f->head;
        if (r) {
            f->head = r->next;
            if (!f->head)
                f->tail = NULL;
        }
        uv_mutex_unlock(&f->mutex);
        if (!r)
            break;
        callback_run(f, r->result, r->args);
        uv_sem_post(&r->done);
    }
}
static void callback_entry(ffi_cif *cif, void *result, void **args, void *opaque)
{
    QBFFI *f = opaque;
    if (cif->rtype->size)
        memset(result, 0, cif->rtype->size);
    uv_thread_t current = uv_thread_self();
    if (uv_thread_equal(&current, &f->thread)) {
        callback_run(f, result, args);
        return;
    }
    if (!f->threadsafe)
        return;
    QBFFIRequest request = {.args = args, .result = result};
    if (uv_sem_init(&request.done, 0))
        return;
    uv_mutex_lock(&f->mutex);
    if (f->tail)
        f->tail->next = &request;
    else
        f->head = &request;
    f->tail = &request;
    uv_mutex_unlock(&f->mutex);
    uv_async_send(&f->async);
    uv_sem_wait(&request.done);
    uv_sem_destroy(&request.done);
}
static void callback_closed(uv_handle_t *h)
{
    QBFFI *f = h->data;
    uv_mutex_destroy(&f->mutex);
    cif_destroy(f);
}
static void callback_free(JSRuntime *rt, void *opaque, void *ptr)
{
    QBFFI *f = opaque;
    (void)ptr;
    if (f->closure)
        ffi_closure_free(f->closure);
    JS_FreeValueRT(rt, f->callback);
    if (f->threadsafe)
        uv_close((uv_handle_t *)&f->async, callback_closed);
    else
        cif_destroy(f);
}
static JSValue make_callback(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    QBFFI *f = prepare(ctx, argv[1], argv[2]);
    if (!f)
        return JS_EXCEPTION;
    f->callback = JS_DupValue(ctx, argv[0]);
    f->thread = uv_thread_self();
    f->closure = ffi_closure_alloc(sizeof(ffi_closure), &f->address);
    if (!f->closure ||
        ffi_prep_closure_loc(f->closure, &f->cif, callback_entry, f, f->address) != FFI_OK) {
        if (f->closure)
            ffi_closure_free(f->closure);
        JS_FreeValue(ctx, f->callback);
        cif_destroy(f);
        return JS_ThrowInternalError(ctx, "Unable to allocate FFI callback");
    }
    if (argc > 3 && JS_ToBool(ctx, argv[3])) {
        int err = uv_mutex_init(&f->mutex);
        if (!err) {
            err = uv_async_init(qb_get(ctx)->loop, &f->async, callback_async);
            if (err)
                uv_mutex_destroy(&f->mutex);
        }
        if (err) {
            ffi_closure_free(f->closure);
            JS_FreeValue(ctx, f->callback);
            cif_destroy(f);
            return qb_throw_uv(ctx, err, "uv_async_init", NULL, NULL);
        }
        f->threadsafe = 1;
        f->async.data = f;
        uv_unref((uv_handle_t *)&f->async);
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "ptr", JS_NewFloat64(ctx, (double)(uintptr_t)f->address));
    JS_SetPropertyStr(ctx, o, "handle",
                      JS_NewArrayBuffer(ctx, (uint8_t *)f, sizeof(*f), callback_free, f, 0));
    return o;
}
static JSValue close_callback(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    size_t n;
    QBFFI *f = (QBFFI *)JS_GetArrayBuffer(ctx, &n, argv[0]);
    if (!f)
        return JS_EXCEPTION;
    if (f->closure) {
        ffi_closure_free(f->closure);
        f->closure = NULL;
    }
    JS_FreeValue(ctx, f->callback);
    f->callback = JS_UNDEFINED;
    return JS_UNDEFINED;
}
static JSValue get_pointer(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    size_t n;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    int64_t offset = 0;
    if (!p)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt64(ctx, &offset, argv[1]))
        return JS_EXCEPTION;
    if (offset < 0 || (uint64_t)offset > n)
        return JS_ThrowRangeError(ctx, "byteOffset is outside the view");
    return JS_NewFloat64(ctx, (double)(uintptr_t)(p + offset));
}
typedef struct {
    void (*fn)(void *, void *);
    void *context;
} QBFFIDeallocator;
static void memory_free(JSRuntime *rt, void *opaque, void *p)
{
    QBFFIDeallocator *d = opaque;
    (void)rt;
    if (d) {
        d->fn(p, d->context);
        free(d);
    }
}
static JSValue memory_view(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    void *p;
    int64_t offset = 0;
    uint64_t length;
    if (pointer_value(ctx, argv[0], &p))
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt64(ctx, &offset, argv[1]))
        return JS_EXCEPTION;
    uint8_t *start = p ? (uint8_t *)p + offset : NULL;
    if (argc < 3 || JS_IsUndefined(argv[2]))
        length = start ? strlen((char *)start) : 0;
    else if (JS_ToIndex(ctx, &length, argv[2]))
        return JS_EXCEPTION;
    if ((!start && length) || length > SIZE_MAX)
        return JS_ThrowRangeError(ctx, "Invalid memory range");
    QBFFIDeallocator *d = NULL;
    if (argc > 3) {
        void *fn = NULL, *context = NULL;
        if (argc > 4 && !JS_IsUndefined(argv[4])) {
            if (pointer_value(ctx, argv[3], &context) || pointer_value(ctx, argv[4], &fn))
                return JS_EXCEPTION;
        } else if (pointer_value(ctx, argv[3], &fn))
            return JS_EXCEPTION;
        if (fn) {
            d = malloc(sizeof(*d));
            if (!d)
                return JS_ThrowOutOfMemory(ctx);
            d->fn = (void (*)(void *, void *))fn;
            d->context = context;
        }
    }
    return JS_NewArrayBuffer(ctx, start, (size_t)length, memory_free, d, 0);
}
static JSValue memory_string(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    void *p;
    int64_t off = 0;
    uint64_t n;
    if (pointer_value(ctx, argv[0], &p))
        return JS_EXCEPTION;
    if (!p)
        return JS_NewString(ctx, "");
    if (argc > 1 && JS_ToInt64(ctx, &off, argv[1]))
        return JS_EXCEPTION;
    const char *s = (const char *)p + off;
    if (argc < 3 || JS_IsUndefined(argv[2]))
        n = strlen(s);
    else if (JS_ToIndex(ctx, &n, argv[2]))
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, s, n);
}
static JSValue memory_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    void *p;
    int64_t offset = 0;
    int code;
    if (pointer_value(ctx, argv[0], &p) || JS_ToInt32(ctx, &code, argv[2]))
        return JS_EXCEPTION;
    if (!p)
        return JS_ThrowRangeError(ctx, "Cannot read a null pointer");
    if (!JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &offset, argv[1]))
        return JS_EXCEPTION;
    return value_out(ctx, code, (uint8_t *)p + offset);
}
static const JSCFunctionListEntry qb_ffi_funcs[] = {
    JS_CFUNC_DEF("open", 1, lib_open),
    JS_CFUNC_DEF("close", 1, lib_close),
    JS_CFUNC_DEF("symbol", 2, lib_symbol),
    JS_CFUNC_DEF("function", 4, make_function),
    JS_CFUNC_DEF("callback", 4, make_callback),
    JS_CFUNC_DEF("closeCallback", 1, close_callback),
    JS_CFUNC_DEF("ptr", 2, get_pointer),
    JS_CFUNC_DEF("toArrayBuffer", 5, memory_view),
    JS_CFUNC_DEF("cstring", 3, memory_string),
    JS_CFUNC_DEF("read", 3, memory_read),
#ifdef __APPLE__
    JS_PROP_STRING_DEF("suffix", "dylib", JS_PROP_ENUMERABLE),
#else
    JS_PROP_STRING_DEF("suffix", "so", JS_PROP_ENUMERABLE),
#endif
};
QB_SIMPLE_MODULE(ffi, qb_ffi_funcs)
