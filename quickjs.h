/*
 * QuickJS Javascript Engine
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
#ifndef QUICKJS_H
#define QUICKJS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define js_likely(x)          __builtin_expect(!!(x), 1)
#define js_unlikely(x)        __builtin_expect(!!(x), 0)
#define js_force_inline       inline __attribute__((always_inline))
#define __js_printf_like(f, a)   __attribute__((format(printf, f, a)))
#else
#define js_likely(x)     (x)
#define js_unlikely(x)   (x)
#define js_force_inline  inline
#define __js_printf_like(a, b)
#endif

/* JS_CALLX_(F, X, JS_COUNT_ARGS(a, ...), a, ...) expands to F(X, a);
   F(X, ...); for 1 to 8 arguments */
#define JS_COUNT_ARGS_(_0, _8, _7, _6, _5, _4, _3, _2, _1, N, ...) N
#define JS_COUNT_ARGS(...) JS_COUNT_ARGS_(0, __VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define JS_CALLX1(F,X,a)               do { F(X,a); } while (0)
#define JS_CALLX2(F,X,a,b)             do { F(X,a); F(X,b); } while (0)
#define JS_CALLX3(F,X,a,b,c)           do { F(X,a); F(X,b); F(X,c); } while (0)
#define JS_CALLX4(F,X,a,b,c,d)         do { F(X,a); F(X,b); F(X,c); F(X,d); } while (0)
#define JS_CALLX5(F,X,a,b,c,d,e)       do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); } while (0)
#define JS_CALLX6(F,X,a,b,c,d,e,f)     do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); } while (0)
#define JS_CALLX7(F,X,a,b,c,d,e,f,g)   do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); F(X,g); } while (0)
#define JS_CALLX8(F,X,a,b,c,d,e,f,g,h) do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); F(X,g); F(X,h); } while (0)

#define JS_CALLX__(F, X, N, ...) JS_CALLX##N(F, X, __VA_ARGS__)
#define JS_CALLX_(F, X, N, ...) JS_CALLX__(F, X, N, __VA_ARGS__)

#define JS_BOOL int

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct JSClass JSClass;
typedef uint32_t JSClassID;
typedef uint32_t JSAtom;

#if INTPTR_MAX >= INT64_MAX
#define JS_PTR64
#define JS_PTR64_DEF(a) a
#else
#define JS_PTR64_DEF(a)
#endif

#ifndef JS_PTR64
#define JS_NAN_BOXING
#endif

#if defined(__SIZEOF_INT128__) && (INTPTR_MAX >= INT64_MAX)
#define JS_LIMB_BITS 64
#else
#define JS_LIMB_BITS 32
#endif

#define JS_SHORT_BIG_INT_BITS JS_LIMB_BITS
    
enum {
    /* all tags with a reference count are negative */
    JS_TAG_FIRST       = -9, /* first negative tag */
    JS_TAG_BIG_INT     = -9,
    JS_TAG_SYMBOL      = -8,
    JS_TAG_STRING      = -7,
    JS_TAG_STRING_ROPE = -6,
    JS_TAG_MODULE      = -3, /* used internally */
    JS_TAG_FUNCTION_BYTECODE = -2, /* used internally */
    JS_TAG_OBJECT      = -1,

    JS_TAG_INT         = 0,
    JS_TAG_BOOL        = 1,
    JS_TAG_NULL        = 2,
    JS_TAG_UNDEFINED   = 3,
    JS_TAG_UNINITIALIZED = 4,
    JS_TAG_CATCH_OFFSET = 5,
    JS_TAG_EXCEPTION   = 6,
    JS_TAG_SHORT_BIG_INT = 7,
    JS_TAG_FLOAT64     = 8,
    /* any larger tag is FLOAT64 if JS_NAN_BOXING */
};

/* must match the layout of 'JSMallocBlockHeader' */
typedef struct JSRefCountHeader {
    int ref_count;
} JSRefCountHeader;

#define JS_FLOAT64_NAN NAN

#ifdef CONFIG_CHECK_JSVALUE
/* JSValue consistency : it is not possible to run the code in this
   mode, but it is useful to detect simple reference counting
   errors. It would be interesting to modify a static C analyzer to
   handle specific annotations (clang has such annotations but only
   for objective C) */
typedef struct __JSValue *JSValue;
typedef const struct __JSValue *JSValueConst;

#define JS_VALUE_GET_TAG(v) (int)((uintptr_t)(v) & 0xf)
/* same as JS_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
#define JS_VALUE_GET_NORM_TAG(v) JS_VALUE_GET_TAG(v)
#define JS_VALUE_GET_INT(v) (int)((intptr_t)(v) >> 4)
#define JS_VALUE_GET_BOOL(v) JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_FLOAT64(v) (double)JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_PTR(v) (void *)((intptr_t)(v) & ~0xf)

#define JS_MKVAL(tag, val) (JSValue)(intptr_t)(((val) << 4) | (tag))
#define JS_MKPTR(tag, p) (JSValue)((intptr_t)(p) | (tag))

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN JS_MKVAL(JS_TAG_FLOAT64, 1)

static inline JSValue __JS_NewFloat64(JSContext *ctx, double d)
{
    return JS_MKVAL(JS_TAG_FLOAT64, (int)d);
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    return 0;
}

static inline JSValue __JS_NewShortBigInt(JSContext *ctx, int32_t d)
{
    return JS_MKVAL(JS_TAG_SHORT_BIG_INT, d);
}

#elif defined(JS_NAN_BOXING)

typedef uint64_t JSValue;

#define JSValueConst JSValue

#define JS_VALUE_GET_TAG(v) (int)((v) >> 32)
#define JS_VALUE_GET_INT(v) (int)(v)
#define JS_VALUE_GET_BOOL(v) (int)(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) (int)(v)
#define JS_VALUE_GET_PTR(v) (void *)(intptr_t)(v)

#define JS_MKVAL(tag, val) (((uint64_t)(tag) << 32) | (uint32_t)(val))
#define JS_MKPTR(tag, ptr) (((uint64_t)(tag) << 32) | (uintptr_t)(ptr))

#define JS_FLOAT64_TAG_ADDEND (0x7ff80000 - JS_TAG_FIRST + 1) /* quiet NaN encoding */

static inline double JS_VALUE_GET_FLOAT64(JSValue v)
{
    union {
        JSValue v;
        double d;
    } u;
    u.v = v;
    u.v += (uint64_t)JS_FLOAT64_TAG_ADDEND << 32;
    return u.d;
}

#define JS_NAN (0x7ff8000000000000 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32))

static inline JSValue __JS_NewFloat64(JSContext *ctx, double d)
{
    union {
        double d;
        uint64_t u64;
    } u;
    JSValue v;
    u.d = d;
    /* normalize NaN */
    if (js_unlikely((u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000))
        v = JS_NAN;
    else
        v = u.u64 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32);
    return v;
}

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)((tag) - JS_TAG_FIRST) >= (JS_TAG_FLOAT64 - JS_TAG_FIRST))

/* same as JS_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
static inline int JS_VALUE_GET_NORM_TAG(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    if (JS_TAG_IS_FLOAT64(tag))
        return JS_TAG_FLOAT64;
    else
        return tag;
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    return tag == (JS_NAN >> 32);
}

static inline JSValue __JS_NewShortBigInt(JSContext *ctx, int32_t d)
{
    return JS_MKVAL(JS_TAG_SHORT_BIG_INT, d);
}

#else /* !JS_NAN_BOXING */

typedef union JSValueUnion {
    uint64_t uint64;
    double float64;
    void *ptr;
#if JS_SHORT_BIG_INT_BITS == 32
    int32_t short_big_int;
#else
    int64_t short_big_int;
#endif
} JSValueUnion;

typedef struct JSValue {
    JSValueUnion u;
    int64_t tag;
} JSValue;

#define JSValueConst JSValue

#define JS_VALUE_GET_TAG(v) ((int32_t)(v).tag)
/* same as JS_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
#define JS_VALUE_GET_NORM_TAG(v) JS_VALUE_GET_TAG(v)
#define JS_VALUE_GET_INT(v) ((int)(v).u.uint64)
#define JS_VALUE_GET_BOOL(v) ((int)(v).u.uint64)
#define JS_VALUE_GET_FLOAT64(v) ((v).u.float64)
#define JS_VALUE_GET_SHORT_BIG_INT(v) ((v).u.short_big_int)
#define JS_VALUE_GET_PTR(v) ((v).u.ptr)

/* avoid uninitialized data by using a 64 bit field even if only 32
   bits are needed because some compilers generate slower code */
#define JS_MKVAL(tag, val) (JSValue){ (JSValueUnion){ .uint64 = (uint32_t)(val) }, tag }
#define JS_MKPTR(tag, p) (JSValue){ (JSValueUnion){ .ptr = p }, tag }

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN (JSValue){ .u.float64 = JS_FLOAT64_NAN, JS_TAG_FLOAT64 }

static inline JSValue __JS_NewFloat64(JSContext *ctx, double d)
{
    JSValue v;
    v.tag = JS_TAG_FLOAT64;
    v.u.float64 = d;
    return v;
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    union {
        double d;
        uint64_t u64;
    } u;
    if (v.tag != JS_TAG_FLOAT64)
        return 0;
    u.d = v.u.float64;
    return (u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000;
}

static inline JSValue __JS_NewShortBigInt(JSContext *ctx, int64_t d)
{
    JSValue v;
    v.tag = JS_TAG_SHORT_BIG_INT;
    v.u.short_big_int = d;
    return v;
}

#endif /* !JS_NAN_BOXING */

#define JS_VALUE_IS_BOTH_INT(v1, v2) ((JS_VALUE_GET_TAG(v1) | JS_VALUE_GET_TAG(v2)) == 0)
#define JS_VALUE_IS_BOTH_FLOAT(v1, v2) (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v1)) && JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v2)))

#define JS_VALUE_HAS_REF_COUNT(v) ((unsigned)JS_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST)

/* special values */
#define JS_NULL      JS_MKVAL(JS_TAG_NULL, 0)
#define JS_UNDEFINED JS_MKVAL(JS_TAG_UNDEFINED, 0)
#define JS_FALSE     JS_MKVAL(JS_TAG_BOOL, 0)
#define JS_TRUE      JS_MKVAL(JS_TAG_BOOL, 1)
#define JS_EXCEPTION JS_MKVAL(JS_TAG_EXCEPTION, 0)
#define JS_UNINITIALIZED JS_MKVAL(JS_TAG_UNINITIALIZED, 0)

/* flags for object properties */
#define JS_PROP_CONFIGURABLE  (1 << 0)
#define JS_PROP_WRITABLE      (1 << 1)
#define JS_PROP_ENUMERABLE    (1 << 2)
#define JS_PROP_C_W_E         (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE)
#define JS_PROP_LENGTH        (1 << 3) /* used internally in Arrays */
#define JS_PROP_TMASK         (3 << 4) /* mask for NORMAL, GETSET, VARREF, AUTOINIT */
#define JS_PROP_NORMAL         (0 << 4)
#define JS_PROP_GETSET         (1 << 4)
#define JS_PROP_VARREF         (2 << 4) /* used internally */
#define JS_PROP_AUTOINIT       (3 << 4) /* used internally */

/* flags for JS_DefineProperty */
#define JS_PROP_HAS_SHIFT        8
#define JS_PROP_HAS_CONFIGURABLE (1 << 8)
#define JS_PROP_HAS_WRITABLE     (1 << 9)
#define JS_PROP_HAS_ENUMERABLE   (1 << 10)
#define JS_PROP_HAS_GET          (1 << 11)
#define JS_PROP_HAS_SET          (1 << 12)
#define JS_PROP_HAS_VALUE        (1 << 13)

/* throw an exception if false would be returned
   (JS_DefineProperty/JS_SetProperty) */
#define JS_PROP_THROW            (1 << 14)
/* throw an exception if false would be returned in strict mode
   (JS_SetProperty) */
#define JS_PROP_THROW_STRICT     (1 << 15)

#define JS_PROP_NO_EXOTIC        (1 << 16) /* internal use */

#ifndef JS_DEFAULT_STACK_SIZE
#define JS_DEFAULT_STACK_SIZE (1024 * 1024)
#endif

/* JS_Eval() flags */
#define JS_EVAL_TYPE_GLOBAL   (0 << 0) /* global code (default) */
#define JS_EVAL_TYPE_MODULE   (1 << 0) /* module code */
#define JS_EVAL_TYPE_DIRECT   (2 << 0) /* direct call (internal use) */
#define JS_EVAL_TYPE_INDIRECT (3 << 0) /* indirect call (internal use) */
#define JS_EVAL_TYPE_MASK     (3 << 0)

#define JS_EVAL_FLAG_STRICT   (1 << 3) /* force 'strict' mode */
/* compile but do not run. The result is an object with a
   JS_TAG_FUNCTION_BYTECODE or JS_TAG_MODULE tag. It can be executed
   with JS_EvalFunction(). */
#define JS_EVAL_FLAG_COMPILE_ONLY (1 << 5)
/* don't include the stack frames before this eval in the Error() backtraces */
#define JS_EVAL_FLAG_BACKTRACE_BARRIER (1 << 6)
/* allow top-level await in normal script. JS_Eval() returns a
   promise. Only allowed with JS_EVAL_TYPE_GLOBAL */
#define JS_EVAL_FLAG_ASYNC (1 << 7)
/* parse the source as TypeScript: type annotations and other type-only
   syntax are erased while parsing, enums, namespaces and parameter
   properties are compiled directly to bytecode, 'const enum' members
   are folded to constants and module imports only used as types are
   elided. Only applies to the evaluated source: direct eval() code and
   Function() bodies are always JavaScript. See the "TypeScript" section
   of the documentation for the supported subset. */
#define JS_EVAL_FLAG_TYPESCRIPT (1 << 8)
/* with JS_EVAL_FLAG_TYPESCRIPT: TypeScript legacy decorators (tsc
   'experimentalDecorators') instead of the standard ECMAScript
   decorators. Ignored without JS_EVAL_FLAG_TYPESCRIPT. */
#define JS_EVAL_FLAG_TS_LEGACY_DECORATORS (1 << 9)
/* with JS_EVAL_FLAG_TS_LEGACY_DECORATORS: the decorated members and
   classes also get the 'design:type', 'design:paramtypes' and
   'design:returntype' metadata computed from their type annotations
   (tsc 'emitDecoratorMetadata'), defined with Reflect.metadata() when
   it exists at run time. */
#define JS_EVAL_FLAG_TS_DECORATOR_METADATA (1 << 10)

#define JS_EVAL_OPTIONS_VERSION 1

/* options of JS_Eval2() and JS_EvalThis2() */
typedef struct JSEvalOptions {
    int version; /* must be JS_EVAL_OPTIONS_VERSION */
    int eval_flags; /* JS_EVAL_TYPE_x and JS_EVAL_FLAG_x */
    const char *filename; /* "<unnamed>" if NULL */
    int line_num; /* line number of the first line (1 if 0) */
    /* new fields are added by incrementing JS_EVAL_OPTIONS_VERSION */
} JSEvalOptions;

typedef JSValue JSCFunction(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
typedef JSValue JSCFunctionMagic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
typedef JSValue JSCFunctionData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *func_data);
/* C function of JS_NewCClosure() */
typedef JSValue JSCClosure(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, void *opaque);

typedef struct JSMallocState {
    size_t malloc_count;
    size_t malloc_size;
    size_t malloc_limit;
    void *opaque; /* user opaque */
} JSMallocState;

typedef struct JSMallocFunctions {
    /* zero initialized allocation of 'count' * 'size' bytes (required) */
    void *(*js_calloc)(JSMallocState *s, size_t count, size_t size);
    void *(*js_malloc)(JSMallocState *s, size_t size);
    void (*js_free)(JSMallocState *s, void *ptr);
    void *(*js_realloc)(JSMallocState *s, void *ptr, size_t size);
    size_t (*js_malloc_usable_size)(const void *ptr);
} JSMallocFunctions;

/* Debug trace system: the debug output is produced on stdout if the
   dumps are compiled in (ENABLE_DUMPS) and JS_SetDumpFlags() is
   invoked with the corresponding bit set. */
#define JS_DUMP_BYTECODE_FINAL   0x01  /* dump pass 3 final byte code */
#define JS_DUMP_BYTECODE_PASS2   0x02  /* dump pass 2 code */
#define JS_DUMP_BYTECODE_PASS1   0x04  /* dump pass 1 code */
#define JS_DUMP_BYTECODE_HEX     0x10  /* dump bytecode in hex */
#define JS_DUMP_BYTECODE_PC2LINE 0x20  /* dump line number table */
#define JS_DUMP_BYTECODE_STACK   0x40  /* dump compute_stack_size */
#define JS_DUMP_READ_OBJECT     0x100  /* dump the marshalled objects at load time */
#define JS_DUMP_FREE            0x200  /* dump every object free */
#define JS_DUMP_GC              0x400  /* dump the occurrence of the automatic GC */
#define JS_DUMP_GC_FREE         0x800  /* dump objects freed by the GC */
#define JS_DUMP_MODULE_RESOLVE 0x1000  /* dump module resolution steps */
#define JS_DUMP_PROMISE        0x2000  /* dump promise steps */
#define JS_DUMP_LEAKS          0x4000  /* dump leaked objects and strings in JS_FreeRuntime */
#define JS_DUMP_ATOM_LEAKS     0x8000  /* dump leaked atoms in JS_FreeRuntime */
#define JS_DUMP_MEM           0x10000  /* dump memory usage in JS_FreeContext */
#define JS_DUMP_OBJECTS       0x20000  /* dump objects in JS_FreeContext */
#define JS_DUMP_ATOMS         0x40000  /* dump atoms in JS_FreeContext */
#define JS_DUMP_SHAPES        0x80000  /* dump shapes in JS_FreeContext */
#define JS_ABORT_ON_LEAKS    0x10C000  /* abort on atom/object/string leaks; for testing */
#define JS_DUMP_MODULE_EXEC  0x200000  /* dump module evaluation steps */
#define JS_DUMP_CLOSURE      0x400000  /* dump closure variables (reserved) */
#define JS_DUMP_ROPE_REBALANCE 0x800000 /* dump string rope rebalancing */

/* runtime finalizers run in LIFO order at the very end of
   JS_FreeRuntime(), once the runtime is no longer usable. They are
   host state: heap snapshots do not save them. */
typedef void JSRuntimeFinalizer(JSRuntime *rt, void *arg);

typedef struct JSGCObjectHeader JSGCObjectHeader;

JSRuntime *JS_NewRuntime(void);
/* info lifetime must exceed that of rt */
void JS_SetRuntimeInfo(JSRuntime *rt, const char *info);
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit);
/* select the debug dumps (JS_DUMP_xxx flags). Ignored unless the
   library is compiled with ENABLE_DUMPS (Makefile: CONFIG_DUMPS=y) */
void JS_SetDumpFlags(JSRuntime *rt, uint64_t flags);
/* return the debug dump flags (always 0 without ENABLE_DUMPS) */
uint64_t JS_GetDumpFlags(JSRuntime *rt);
/* return the malloc size triggering the next automatic GC */
size_t JS_GetGCThreshold(JSRuntime *rt);
void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold);
/* use 0 to disable maximum stack size check */
void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size);
/* should be called when changing thread to update the stack top value
   used to check stack overflow. */
void JS_UpdateStackTop(JSRuntime *rt);
JSRuntime *JS_NewRuntime2(const JSMallocFunctions *mf, void *opaque);
void JS_FreeRuntime(JSRuntime *rt);
void *JS_GetRuntimeOpaque(JSRuntime *rt);
void JS_SetRuntimeOpaque(JSRuntime *rt, void *opaque);
/* call finalizer(rt, arg) at the end of JS_FreeRuntime(). Return -1 if
   out of memory. */
int JS_AddRuntimeFinalizer(JSRuntime *rt, JSRuntimeFinalizer *finalizer,
                           void *arg);
typedef void JS_MarkFunc(JSRuntime *rt, JSGCObjectHeader *gp);
void JS_MarkValue(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
void JS_RunGC(JSRuntime *rt);
JS_BOOL JS_IsLiveObject(JSRuntime *rt, JSValueConst obj);

JSContext *JS_NewContext(JSRuntime *rt);
void __JS_FreeContext(JSContext *ctx);

/* The context reference count is stored in the memory block header
   right before the JSContext, at the same place as the reference
   count of the reference counted values. */
static inline JSContext *JS_DupContext(JSContext *ctx)
{
    ((JSRefCountHeader *)((uint32_t *)ctx - 1))->ref_count++;
    return ctx;
}

/* release a reference to the context; it is freed (with its intrinsic
   objects) when the last reference is released */
static inline void JS_FreeContext(JSContext *ctx)
{
    JSRefCountHeader *p = (JSRefCountHeader *)((uint32_t *)ctx - 1);
    if (--p->ref_count <= 0) {
        __JS_FreeContext(ctx);
    }
}
void *JS_GetContextOpaque(JSContext *ctx);
void JS_SetContextOpaque(JSContext *ctx, void *opaque);
JSRuntime *JS_GetRuntime(JSContext *ctx);
void JS_SetClassProto(JSContext *ctx, JSClassID class_id, JSValue obj);
JSValue JS_GetClassProto(JSContext *ctx, JSClassID class_id);

enum {
    JS_INTL_CTOR_NUMBERFORMAT = 0,
    JS_INTL_CTOR_DATETIMEFORMAT,
    JS_INTL_CTOR_COLLATOR,
    JS_INTL_CTOR_LISTFORMAT,
    JS_INTL_CTOR_LOCALE,
    JS_INTL_CTOR_DURATIONFORMAT,
    JS_INTL_FALLBACK_SYMBOL,
    JS_CONTEXT_VALUE_COUNT,
};

void JS_SetContextValue(JSContext *ctx, int idx, JSValue val);
JSValue JS_GetContextValue(JSContext *ctx, int idx);

/* the following functions are used to select the intrinsic object to
   save memory */
JSContext *JS_NewContextRaw(JSRuntime *rt);
int JS_AddIntrinsicBaseObjects(JSContext *ctx);
int JS_AddIntrinsicDate(JSContext *ctx);
int JS_AddIntrinsicEval(JSContext *ctx);
int JS_AddIntrinsicStringNormalize(JSContext *ctx);
void JS_AddIntrinsicRegExpCompiler(JSContext *ctx);
int JS_AddIntrinsicRegExp(JSContext *ctx);
int JS_AddIntrinsicJSON(JSContext *ctx);
int JS_AddIntrinsicProxy(JSContext *ctx);
/* same as 'new Proxy(target, handler)' */
JSValue JS_NewProxy(JSContext *ctx, JSValueConst target, JSValueConst handler);
JS_BOOL JS_IsProxy(JSValueConst val);
/* throw a TypeError if 'proxy' is not a proxy or is revoked */
JSValue JS_GetProxyTarget(JSContext *ctx, JSValueConst proxy);
JSValue JS_GetProxyHandler(JSContext *ctx, JSValueConst proxy);
int JS_AddIntrinsicMapSet(JSContext *ctx);
int JS_AddIntrinsicTypedArrays(JSContext *ctx);
int JS_AddIntrinsicPromise(JSContext *ctx);
int JS_AddIntrinsicWeakRef(JSContext *ctx);
/* DOMException constructor (WebIDL), also needed by JS_AddIntrinsicAToB() */
int JS_AddIntrinsicDOMException(JSContext *ctx);
/* btoa() and atob() */
int JS_AddIntrinsicAToB(JSContext *ctx);
/* 'performance' object with now() and timeOrigin. A context restored
   from a full snapshot or a delta keeps its time origin and
   performance.now() continues from its value at the write (the time
   between the write and the read is not counted); a base read alone
   starts a new time origin. */
int JS_AddPerformance(JSContext *ctx);
#ifdef CONFIG_TEMPORAL_INTL
int JS_AddIntrinsicTemporal(JSContext *ctx);
int JS_AddIntrinsicIntl(JSContext *ctx);
#endif

JSValue js_string_codePointRange(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);

/* zero initialized, NULL if 'count' * 'size' overflows */
void *js_calloc_rt(JSRuntime *rt, size_t count, size_t size);
void *js_malloc_rt(JSRuntime *rt, size_t size);
void js_free_rt(JSRuntime *rt, void *ptr);
void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size);
size_t js_malloc_usable_size_rt(JSRuntime *rt, const void *ptr);
void *js_mallocz_rt(JSRuntime *rt, size_t size);

/* same as js_calloc_rt(), throws out of memory in case of error */
void *js_calloc(JSContext *ctx, size_t count, size_t size);
void *js_malloc(JSContext *ctx, size_t size);
void js_free(JSContext *ctx, void *ptr);
void *js_realloc(JSContext *ctx, void *ptr, size_t size);
size_t js_malloc_usable_size(JSContext *ctx, const void *ptr);
void *js_realloc2(JSContext *ctx, void *ptr, size_t size, size_t *pslack);
void *js_mallocz(JSContext *ctx, size_t size);
char *js_strdup(JSContext *ctx, const char *str);
char *js_strndup(JSContext *ctx, const char *s, size_t n);

typedef struct JSMemoryUsage {
    int64_t malloc_size, malloc_limit, memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t js_func_pc2line_count, js_func_pc2line_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} JSMemoryUsage;

void JS_ComputeMemoryUsage(JSRuntime *rt, JSMemoryUsage *s);
void JS_DumpMemoryUsage(FILE *fp, const JSMemoryUsage *s, JSRuntime *rt);

/* atom support */
#define JS_ATOM_NULL 0

JSAtom JS_NewAtomLen(JSContext *ctx, const char *str, size_t len);
JSAtom JS_NewAtom(JSContext *ctx, const char *str);
/* Return the atom JS_NewAtomLen() would return for the UTF-8 string
   'str' if it already exists, JS_ATOM_NULL otherwise. Never creates an
   atom, allocates memory or raises an exception. A non null result
   must be freed with JS_FreeAtom(). */
JSAtom JS_FindAtomLen(JSContext *ctx, const char *str, size_t len);
JSAtom JS_NewAtomUInt32(JSContext *ctx, uint32_t n);
JSAtom JS_DupAtom(JSContext *ctx, JSAtom v);
JSAtom JS_DupAtomRT(JSRuntime *rt, JSAtom v);
void JS_FreeAtom(JSContext *ctx, JSAtom v);
void JS_FreeAtomRT(JSRuntime *rt, JSAtom v);
/* free 1 to 8 atoms */
#define JS_FreeAtoms(ctx, ...) JS_CALLX_(JS_FreeAtom, ctx, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
#define JS_FreeAtomsRT(rt, ...) JS_CALLX_(JS_FreeAtomRT, rt, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
JSValue JS_AtomToValue(JSContext *ctx, JSAtom atom);
JSValue JS_AtomToString(JSContext *ctx, JSAtom atom);
const char *JS_AtomToCStringLen(JSContext *ctx, size_t *plen, JSAtom atom);
static inline const char *JS_AtomToCString(JSContext *ctx, JSAtom atom)
{
    return JS_AtomToCStringLen(ctx, NULL, atom);
}
/* size of the 'buf' argument of JS_AtomGetStringData() */
#define JS_ATOM_DATA_BUF_SIZE 16
/* Borrowed view of JS_AtomToString(ctx, atom) as JS_GetStringData()
   returns it, without allocation: the decimal digits of an integer
   atom are written to 'buf' (JS_ATOM_DATA_BUF_SIZE bytes) and
   returned, the contents of other atoms (the description of a symbol)
   remain valid while the atom is alive. */
const void *JS_AtomGetStringData(JSContext *ctx, JSAtom atom, size_t *plen,
                                 JS_BOOL *pwide, char *buf);
JSAtom JS_ValueToAtom(JSContext *ctx, JSValueConst val);

/* object class support */

typedef struct JSPropertyEnum {
    JS_BOOL is_enumerable;
    JSAtom atom;
} JSPropertyEnum;

typedef struct JSPropertyDescriptor {
    int flags;
    JSValue value;
    JSValue getter;
    JSValue setter;
} JSPropertyDescriptor;

typedef struct JSClassExoticMethods {
    /* Return -1 if exception (can only happen in case of Proxy object),
       FALSE if the property does not exists, TRUE if it exists. If 1 is
       returned, the property descriptor 'desc' is filled if != NULL. */
    int (*get_own_property)(JSContext *ctx, JSPropertyDescriptor *desc,
                             JSValueConst obj, JSAtom prop);
    /* '*ptab' should hold the '*plen' property keys. Return 0 if OK,
       -1 if exception. The 'is_enumerable' field is ignored.
    */
    int (*get_own_property_names)(JSContext *ctx, JSPropertyEnum **ptab,
                                  uint32_t *plen,
                                  JSValueConst obj);
    /* return < 0 if exception, or TRUE/FALSE */
    int (*delete_property)(JSContext *ctx, JSValueConst obj, JSAtom prop);
    /* return < 0 if exception or TRUE/FALSE */
    int (*define_own_property)(JSContext *ctx, JSValueConst this_obj,
                               JSAtom prop, JSValueConst val,
                               JSValueConst getter, JSValueConst setter,
                               int flags);
    /* The following methods can be emulated with the previous ones,
       so they are usually not needed */
    /* return < 0 if exception or TRUE/FALSE */
    int (*has_property)(JSContext *ctx, JSValueConst obj, JSAtom atom);
    JSValue (*get_property)(JSContext *ctx, JSValueConst obj, JSAtom atom,
                            JSValueConst receiver);
    /* return < 0 if exception or TRUE/FALSE */
    int (*set_property)(JSContext *ctx, JSValueConst obj, JSAtom atom,
                        JSValueConst value, JSValueConst receiver, int flags);

    /* To get a consistent object behavior when get_prototype != NULL,
       get_property, set_property and set_prototype must be != NULL
       and the object must be created with a JS_NULL prototype. */
    JSValue (*get_prototype)(JSContext *ctx, JSValueConst obj);
    /* return < 0 if exception or TRUE/FALSE */
    int (*set_prototype)(JSContext *ctx, JSValueConst obj, JSValueConst proto_val);
    /* return < 0 if exception or TRUE/FALSE */
    int (*is_extensible)(JSContext *ctx, JSValueConst obj);
    /* return < 0 if exception or TRUE/FALSE */
    int (*prevent_extensions)(JSContext *ctx, JSValueConst obj);
} JSClassExoticMethods;

typedef void JSClassFinalizer(JSRuntime *rt, JSValue val);
typedef void JSClassGCMark(JSRuntime *rt, JSValueConst val,
                           JS_MarkFunc *mark_func);
#define JS_CALL_FLAG_CONSTRUCTOR (1 << 0)
typedef JSValue JSClassCall(JSContext *ctx, JSValueConst func_obj,
                            JSValueConst this_val, int argc, JSValueConst *argv,
                            int flags);

typedef struct JSClassDef {
    const char *class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    /* if call != NULL, the object is a function. If (flags &
       JS_CALL_FLAG_CONSTRUCTOR) != 0, the function is called as a
       constructor. In this case, 'this_val' is new.target. A
       constructor call only happens if the object constructor bit is
       set (see JS_SetConstructorBit()). */
    JSClassCall *call;
    /* XXX: suppress this indirection ? It is here only to save memory
       because only a few classes need these methods */
    JSClassExoticMethods *exotic;
} JSClassDef;

#define JS_INVALID_CLASS_ID 0
JSClassID JS_NewClassID(JSClassID *pclass_id);
/* Returns the class ID if `v` is an object, otherwise returns JS_INVALID_CLASS_ID. */
JSClassID JS_GetClassID(JSValueConst v);
int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def);
int JS_IsRegisteredClass(JSRuntime *rt, JSClassID class_id);
/* name of a registered class (to be freed with JS_FreeAtomRT()) or
   JS_ATOM_NULL */
JSAtom JS_GetClassName(JSRuntime *rt, JSClassID class_id);

/* value handling */

static js_force_inline JSValue JS_NewBool(JSContext *ctx, JS_BOOL val)
{
    return JS_MKVAL(JS_TAG_BOOL, (val != 0));
}

static js_force_inline JSValue JS_NewInt32(JSContext *ctx, int32_t val)
{
    return JS_MKVAL(JS_TAG_INT, val);
}

static js_force_inline JSValue JS_NewCatchOffset(JSContext *ctx, int32_t val)
{
    return JS_MKVAL(JS_TAG_CATCH_OFFSET, val);
}

static js_force_inline JSValue JS_NewInt64(JSContext *ctx, int64_t val)
{
    JSValue v;
    if (val == (int32_t)val) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

static js_force_inline JSValue JS_NewUint32(JSContext *ctx, uint32_t val)
{
    JSValue v;
    if (val <= 0x7fffffff) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

/* a Number (rounded if > 2^53) */
static js_force_inline JSValue JS_NewUint64(JSContext *ctx, uint64_t val)
{
    JSValue v;
    if (val <= 0x7fffffff) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

JSValue JS_NewBigInt64(JSContext *ctx, int64_t v);
JSValue JS_NewBigUint64(JSContext *ctx, uint64_t v);

static js_force_inline JSValue JS_NewFloat64(JSContext *ctx, double d)
{
    int32_t val;
    union {
        double d;
        uint64_t u;
    } u, t;
    if (d >= INT32_MIN && d <= INT32_MAX) {
        u.d = d;
        val = (int32_t)d;
        t.d = val;
        /* -0 cannot be represented as integer, so we compare the bit
           representation */
        if (u.u == t.u)
            return JS_MKVAL(JS_TAG_INT, val);
    }
    return __JS_NewFloat64(ctx, d);
}

static inline JS_BOOL JS_IsNumber(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag);
}

static inline JS_BOOL JS_IsBigInt(JSContext *ctx, JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline JS_BOOL JS_IsBool(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_BOOL;
}

static inline JS_BOOL JS_IsNull(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_NULL;
}

static inline JS_BOOL JS_IsUndefined(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED;
}

static inline JS_BOOL JS_IsException(JSValueConst v)
{
    return js_unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION);
}

static inline JS_BOOL JS_IsUninitialized(JSValueConst v)
{
    return js_unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED);
}

static inline JS_BOOL JS_IsString(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING ||
        JS_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE;
}

static inline JS_BOOL JS_IsSymbol(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_SYMBOL;
}

static inline JS_BOOL JS_IsObject(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT;
}

/* TRUE for a compiled module (JS_Eval() with JS_EVAL_TYPE_MODULE |
   JS_EVAL_FLAG_COMPILE_ONLY) */
static inline JS_BOOL JS_IsModule(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_MODULE;
}

JSValue JS_Throw(JSContext *ctx, JSValue obj);
void JS_SetUncatchableException(JSContext *ctx, JS_BOOL flag);
/* TRUE if the pending exception cannot be caught by JS code (interruption,
   suspension) */
JS_BOOL JS_IsUncatchableException(JSContext *ctx);
JSValue JS_GetException(JSContext *ctx);
JS_BOOL JS_HasException(JSContext *ctx);
JS_BOOL JS_IsError(JSContext *ctx, JSValueConst val);
JSValue JS_NewError(JSContext *ctx);
/* error kinds of JS_NewErrorLen() and JS_ThrowErrorLen() */
typedef enum JSErrorEnum {
    JS_EVAL_ERROR,
    JS_RANGE_ERROR,
    JS_REFERENCE_ERROR,
    JS_SYNTAX_ERROR,
    JS_TYPE_ERROR,
    JS_URI_ERROR,
    JS_INTERNAL_ERROR,
    JS_AGGREGATE_ERROR,
    JS_SUPPRESSED_ERROR,

    JS_NATIVE_ERROR_COUNT, /* number of different NativeError objects */
    JS_PLAIN_ERROR = JS_NATIVE_ERROR_COUNT, /* 'Error' */
} JSErrorEnum;
/* new error object of kind 'error_num' whose message is the UTF-8
   string 'msg' of 'len' bytes (no formatting and no length limit,
   unlike the printf like functions below). Return JS_EXCEPTION if out
   of memory. */
JSValue JS_NewErrorLen(JSContext *ctx, JSErrorEnum error_num,
                       const char *msg, size_t len);
/* same as JS_NewErrorLen() but throw the error. Return JS_EXCEPTION. */
JSValue JS_ThrowErrorLen(JSContext *ctx, JSErrorEnum error_num,
                         const char *msg, size_t len);
/* same as the JS_Throw*Error() functions but return the error object
   (or JS_EXCEPTION) without throwing it */
JSValue __js_printf_like(2, 3) JS_NewSyntaxError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_NewTypeError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_NewReferenceError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_NewRangeError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_NewInternalError(JSContext *ctx, const char *fmt, ...);
/* new 'Error' object */
JSValue __js_printf_like(2, 3) JS_NewPlainError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowSyntaxError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowReferenceError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...);
/* throw an 'Error' object */
JSValue __js_printf_like(2, 3) JS_ThrowPlainError(JSContext *ctx, const char *fmt, ...);
/* throw a DOMException of the given name (e.g. "InvalidCharacterError");
   needs JS_AddIntrinsicDOMException() */
JSValue __js_printf_like(3, 4) JS_ThrowDOMException(JSContext *ctx, const char *name, const char *fmt, ...);
JSValue JS_ThrowOutOfMemory(JSContext *ctx);

void __JS_FreeValue(JSContext *ctx, JSValue v);

static inline JSRefCountHeader *__js_rc(void *ptr)
{
    return (JSRefCountHeader *)((uint32_t *)ptr - 1);
}

static inline void JS_FreeValue(JSContext *ctx, JSValue v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader *p = __js_rc(JS_VALUE_GET_PTR(v));
        if (--p->ref_count <= 0) {
            __JS_FreeValue(ctx, v);
        }
    }
}
void __JS_FreeValueRT(JSRuntime *rt, JSValue v);
static inline void JS_FreeValueRT(JSRuntime *rt, JSValue v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader *p = __js_rc(JS_VALUE_GET_PTR(v));
        if (--p->ref_count <= 0) {
            __JS_FreeValueRT(rt, v);
        }
    }
}
/* free 1 to 8 values */
#define JS_FreeValues(ctx, ...) JS_CALLX_(JS_FreeValue, ctx, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
#define JS_FreeValuesRT(rt, ...) JS_CALLX_(JS_FreeValueRT, rt, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

static inline JSValue JS_DupValue(JSContext *ctx, JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader *p = __js_rc(JS_VALUE_GET_PTR(v));
        p->ref_count++;
    }
    return (JSValue)v;
}

static inline JSValue JS_DupValueRT(JSRuntime *rt, JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader *p = __js_rc(JS_VALUE_GET_PTR(v));
        p->ref_count++;
    }
    return (JSValue)v;
}

/* abstract equality (==): return -1 if exception */
int JS_IsEqual(JSContext *ctx, JSValueConst op1, JSValueConst op2);
JS_BOOL JS_StrictEq(JSContext *ctx, JSValueConst op1, JSValueConst op2);
JS_BOOL JS_SameValue(JSContext *ctx, JSValueConst op1, JSValueConst op2);
JS_BOOL JS_SameValueZero(JSContext *ctx, JSValueConst op1, JSValueConst op2);

JSValue JS_ToObject(JSContext *ctx, JSValueConst val);
/* Object.prototype.toString.call(val), e.g. "[object Object]" */
JSValue JS_ToObjectString(JSContext *ctx, JSValueConst val);
#define JS_HINT_STRING  0
#define JS_HINT_NUMBER  1
#define JS_HINT_NONE    2
JSValue JS_ToPrimitive(JSContext *ctx, JSValueConst val, int hint);
int JS_ToBool(JSContext *ctx, JSValueConst val); /* return -1 for JS_EXCEPTION */
/* ToNumber(): return a Number or JS_EXCEPTION (BigInt values throw) */
JSValue JS_ToNumber(JSContext *ctx, JSValueConst val);
int JS_ToInt32(JSContext *ctx, int32_t *pres, JSValueConst val);
static inline int JS_ToUint32(JSContext *ctx, uint32_t *pres, JSValueConst val)
{
    return JS_ToInt32(ctx, (int32_t*)pres, val);
}
int JS_ToInt32Sat(JSContext *ctx, int *pres, JSValueConst val);
int JS_ToInt64(JSContext *ctx, int64_t *pres, JSValueConst val);
int JS_ToIndex(JSContext *ctx, uint64_t *plen, JSValueConst val);
int JS_ToFloat64(JSContext *ctx, double *pres, JSValueConst val);
/* return an exception if 'val' is a Number */
int JS_ToBigInt64(JSContext *ctx, int64_t *pres, JSValueConst val);
/* same as JS_ToBigInt64() with the result as unsigned (modulo 2^64) */
int JS_ToBigUint64(JSContext *ctx, uint64_t *pres, JSValueConst val);
/* same as JS_ToInt64() but allow BigInt */
int JS_ToInt64Ext(JSContext *ctx, int64_t *pres, JSValueConst val);
JSValue JS_NewBigInt128(JSContext *ctx, uint64_t low, uint64_t high);
int JS_ToBigInt128(JSContext *ctx, uint64_t *plow, uint64_t *phigh, JSValueConst val);
int JS_ToBigInt128Sat(JSContext *ctx, uint64_t *plow, uint64_t *phigh, JSValueConst val);

JSValue JS_NewStringLen(JSContext *ctx, const char *str1, size_t len1);
static inline JSValue JS_NewString(JSContext *ctx, const char *str)
{
    return JS_NewStringLen(ctx, str, strlen(str));
}
/* copy 'len' UTF-16 code units; unpaired surrogates are kept */
JSValue JS_NewStringUTF16(JSContext *ctx, const uint16_t *buf, size_t len);
/* Latin-1 (ISO-8859-1) bytes: one code unit per byte, no UTF-8 decoding */
JSValue JS_NewStringLatin1Len(JSContext *ctx, const uint8_t *buf, size_t len);
JSValue JS_NewAtomString(JSContext *ctx, const char *str);
JSValue JS_ToString(JSContext *ctx, JSValueConst val);
JSValue JS_ToPropertyKey(JSContext *ctx, JSValueConst val);
const char *JS_ToCStringLen2(JSContext *ctx, size_t *plen, JSValueConst val1, JS_BOOL cesu8);
static inline const char *JS_ToCStringLen(JSContext *ctx, size_t *plen, JSValueConst val1)
{
    return JS_ToCStringLen2(ctx, plen, val1, 0);
}
static inline const char *JS_ToCString(JSContext *ctx, JSValueConst val1)
{
    return JS_ToCStringLen2(ctx, NULL, val1, 0);
}
const char *JS_ToCStringNZ(JSContext *ctx, JSValueConst val1);
/* UTF-16 code units of ToString(val1) in native endianness; not zero
   terminated, unpaired surrogates are kept, '*plen' is in code
   units. Free with JS_FreeCStringUTF16(). Return NULL if exception. */
const uint16_t *JS_ToCStringLenUTF16(JSContext *ctx, size_t *plen, JSValueConst val1);
static inline const uint16_t *JS_ToCStringUTF16(JSContext *ctx, JSValueConst val1)
{
    return JS_ToCStringLenUTF16(ctx, NULL, val1);
}
/* Borrowed view of the contents of a string (JS_IsString(val)): *plen
   Latin-1 bytes if *pwide is false, *plen UTF-16 code units in native
   endianness otherwise. Not zero terminated. Nothing is copied or
   freed: a rope is flattened in place and the pointer remains valid
   while the caller holds a reference to val. Return NULL (and *plen =
   0) with a pending exception if val is not a string or on memory
   error. */
const void *JS_GetStringData(JSContext *ctx, JSValueConst val, size_t *plen,
                             JS_BOOL *pwide);
void JS_FreeCString(JSContext *ctx, const char *ptr);
void JS_FreeCStringRT(JSRuntime *rt, const char *ptr);
void JS_FreeCStringUTF16(JSContext *ctx, const uint16_t *ptr);
void JS_FreeCStringRT_UTF16(JSRuntime *rt, const uint16_t *ptr);
int JS_ToUTF32String(JSContext *ctx, uint32_t **pbuf, JSValueConst val);

JSValue JS_NewSymbol(JSContext *ctx, const char *description, JS_BOOL is_global);
/* symbol only visible to C code: its properties are not enumerated
   (Reflect.ownKeys(), Object.getOwnPropertySymbols()...) and are own
   properties of the object even on proxies (no trap is called) */
JSValue JS_NewPrivateSymbol(JSContext *ctx, const char *description);

JSValue JS_NewObjectProtoClass(JSContext *ctx, JSValueConst proto, JSClassID class_id);
JSValue JS_NewObjectClass(JSContext *ctx, int class_id);
JSValue JS_NewObjectProto(JSContext *ctx, JSValueConst proto);
JSValue JS_NewObject(JSContext *ctx);
/* new object with the 'count' enumerable properties props[i] = values[i].
   The values are freed, even in case of exception. The properties are
   defined, as with JS_DefinePropertyValue(JS_PROP_C_W_E), on an object
   of JS_NewObject(): the last value of a repeated property is kept. */
JSValue JS_NewObjectFrom(JSContext *ctx, int count, const JSAtom *props,
                         const JSValue *values);
/* Return TRUE if JS_SetProperty() of 'prop' on an object of
   JS_NewObject() which doesn't have it yet only defines it as
   JS_NewObjectFrom() does: no object of the prototype chain has an
   exotic behavior, a setter or a read-only property 'prop' (e.g. FALSE
   for '__proto__'). The answer holds until Object.prototype changes. */
JS_BOOL JS_NewObjectSetIsDefine(JSContext *ctx, JSAtom prop);
/* same as JS_NewObjectFrom() with property names as UTF-8 strings */
JSValue JS_NewObjectFromStr(JSContext *ctx, int count, const char **props,
                            const JSValue *values);
JSValue JS_CreateFromConstructor(JSContext *ctx, JSValueConst ctor, 
                                 int class_id);

JS_BOOL JS_IsFunction(JSContext* ctx, JSValueConst val);
JS_BOOL JS_IsConstructor(JSContext* ctx, JSValueConst val);
JS_BOOL JS_IsAsyncFunction(JSValueConst val);
JS_BOOL JS_SetConstructorBit(JSContext *ctx, JSValueConst func_obj, JS_BOOL val);

JSValue JS_NewArray(JSContext *ctx);
int JS_IsArray(JSContext *ctx, JSValueConst val);
/* new array with the 'count' elements values[]. The values are freed,
   even in case of exception. */
JSValue JS_NewArrayFrom(JSContext *ctx, int count, const JSValue *values);
/* Return a pointer to a flag of 'ctx', valid as long as 'ctx', which is
   non zero while setting the elements of a new array of 'ctx' in index
   order (JS_SetPropertyUint32() on JS_NewArray()) only defines them
   as own data properties, as JS_NewArrayFrom() does: no object of the
   prototype chain of the arrays can intercept the sets (Array.prototype
   and Object.prototype have no index property other than writable data
   elements of Array.prototype, and the prototype of Array.prototype was
   not changed). Conservative: once 0, it stays 0. Only the engine
   changes it, so reading it is enough after any operation which may
   run JS. */
const int *JS_GetStdArrayPrototypeFlag(JSContext *ctx);
/* If 'obj' is an Array whose elements are stored contiguously (a
   "fast array"), return TRUE and set *pvalues to its element storage
   and *plen to the number of stored elements. Otherwise (array with
   holes or non-index properties, Proxy, typed array, non-object...)
   return FALSE. Never throws.
   The indexes 0 to *plen - 1 are own writable data properties.
   *plen can be smaller than 'length': the indexes from *plen to
   length - 1 are holes. The values are borrowed (use JS_DupValue() to
   keep one) and must not be modified. The pointer is only valid until
   the array is modified or freed, i.e. until any JS code, getter,
   setter, Proxy trap, finalizer or garbage collection may run: check
   again after such operations. */
JS_BOOL JS_GetFastArray(JSContext *ctx, JSValueConst obj,
                        JSValue **pvalues, uint32_t *plen);
/* ToLength(obj.length): return -1 if exception */
int JS_GetLength(JSContext *ctx, JSValueConst obj, int64_t *pres);
/* obj.length = len (throwing on failure): return -1 if exception */
int JS_SetLength(JSContext *ctx, JSValueConst obj, int64_t len);
JS_BOOL JS_IsRegExp(JSValueConst val);
JS_BOOL JS_IsMap(JSValueConst val);
JS_BOOL JS_IsSet(JSValueConst val);
JS_BOOL JS_IsWeakRef(JSValueConst val);
JS_BOOL JS_IsWeakMap(JSValueConst val);
JS_BOOL JS_IsWeakSet(JSValueConst val);
JS_BOOL JS_IsDataView(JSValueConst val);

JSValue JS_NewDate(JSContext *ctx, double epoch_ms);
JS_BOOL JS_IsDate(JSValueConst val);
int JS_ThisDateValue(JSContext *ctx, double *valp, JSValueConst this_val);

JSValue JS_GetPropertyInternal(JSContext *ctx, JSValueConst obj,
                               JSAtom prop, JSValueConst receiver,
                               JS_BOOL throw_ref_error);
static js_force_inline JSValue JS_GetProperty(JSContext *ctx, JSValueConst this_obj,
                                              JSAtom prop)
{
    return JS_GetPropertyInternal(ctx, this_obj, prop, this_obj, 0);
}
JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                          const char *prop);
/* this_obj[prop] where 'prop' is a UTF-8 string of 'len' bytes (not
   necessarily zero terminated). No atom is created when the key is not
   already an atom and the lookup cannot observe it (no exotic object
   such as a Proxy in the prototype chain). */
JSValue JS_GetPropertyStrLen(JSContext *ctx, JSValueConst this_obj,
                             const char *prop, size_t len);
JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                             uint32_t idx);
JSValue JS_GetPropertyInt64(JSContext *ctx, JSValueConst this_obj,
                            int64_t idx);
/* this_obj[prop]. 'prop' is freed */
JSValue JS_GetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                            JSValue prop);

int JS_SetPropertyInternal(JSContext *ctx, JSValueConst obj,
                           JSAtom prop, JSValue val, JSValueConst this_obj,
                           int flags);
static inline int JS_SetProperty(JSContext *ctx, JSValueConst this_obj,
                                 JSAtom prop, JSValue val)
{
    return JS_SetPropertyInternal(ctx, this_obj, prop, val, this_obj, JS_PROP_THROW);
}
int JS_SetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                         uint32_t idx, JSValue val);
int JS_SetPropertyInt64(JSContext *ctx, JSValueConst this_obj,
                        int64_t idx, JSValue val);
int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                      const char *prop, JSValue val);
/* same as JS_SetPropertyStr() with a UTF-8 key of 'len' bytes (not
   necessarily zero terminated) */
int JS_SetPropertyStrLen(JSContext *ctx, JSValueConst this_obj,
                         const char *prop, size_t len, JSValue val);
/* this_obj[prop] = val. 'prop' and 'val' are freed. 'flags' can be
   JS_PROP_THROW or JS_PROP_THROW_STRICT */
int JS_SetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                        JSValue prop, JSValue val, int flags);
int JS_HasProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop);
/* same as JS_HasProperty() with a UTF-8 key of 'len' bytes, creating no
   atom in the same cases as JS_GetPropertyStrLen() */
int JS_HasPropertyStrLen(JSContext *ctx, JSValueConst this_obj,
                         const char *prop, size_t len);
int JS_IsExtensible(JSContext *ctx, JSValueConst obj);
int JS_PreventExtensions(JSContext *ctx, JSValueConst obj);
/* Object.seal() / Object.freeze(): return -1 if exception. Non objects
   are left unchanged. */
int JS_SealObject(JSContext *ctx, JSValueConst obj);
int JS_FreezeObject(JSContext *ctx, JSValueConst obj);
int JS_DeleteProperty(JSContext *ctx, JSValueConst obj, JSAtom prop, int flags);
int JS_SetPrototype(JSContext *ctx, JSValueConst obj, JSValueConst proto_val);
JSValue JS_GetPrototype(JSContext *ctx, JSValueConst val);

#define JS_GPN_STRING_MASK  (1 << 0)
#define JS_GPN_SYMBOL_MASK  (1 << 1)
#define JS_GPN_PRIVATE_MASK (1 << 2)
/* only include the enumerable properties */
#define JS_GPN_ENUM_ONLY    (1 << 4)
/* set theJSPropertyEnum.is_enumerable field */
#define JS_GPN_SET_ENUM     (1 << 5)

int JS_GetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                           uint32_t *plen, JSValueConst obj, int flags);
/* set '*plen' to the number of properties JS_GetOwnPropertyNames()
   would return, without building the list for ordinary objects. Return
   -1 if exception. */
int JS_GetOwnPropertyCount(JSContext *ctx, uint32_t *plen, JSValueConst obj,
                           int flags);
void JS_FreePropertyEnum(JSContext *ctx, JSPropertyEnum *tab,
                         uint32_t len);
int JS_GetOwnProperty(JSContext *ctx, JSPropertyDescriptor *desc,
                      JSValueConst obj, JSAtom prop);

JSValue JS_Call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_obj,
                int argc, JSValueConst *argv);
JSValue JS_Invoke(JSContext *ctx, JSValueConst this_val, JSAtom atom,
                  int argc, JSValueConst *argv);
JSValue JS_CallConstructor(JSContext *ctx, JSValueConst func_obj,
                           int argc, JSValueConst *argv);
JSValue JS_CallConstructor2(JSContext *ctx, JSValueConst func_obj,
                            JSValueConst new_target,
                            int argc, JSValueConst *argv);
JS_BOOL JS_DetectModule(const char *input, size_t input_len);
/* 'input' must be zero terminated i.e. input[input_len] = '\0'. */
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);
/* same as JS_Eval() but with an explicit 'this_obj' parameter */
JSValue JS_EvalThis(JSContext *ctx, JSValueConst this_obj,
                    const char *input, size_t input_len,
                    const char *filename, int eval_flags);
/* same as JS_Eval() with the filename, first line number and flags in
   'options' (may be NULL) */
JSValue JS_Eval2(JSContext *ctx, const char *input, size_t input_len,
                 JSEvalOptions *options);
/* same as JS_Eval2() with an explicit 'this_obj' parameter */
JSValue JS_EvalThis2(JSContext *ctx, JSValueConst this_obj,
                     const char *input, size_t input_len,
                     JSEvalOptions *options);
JSValue JS_GetGlobalObject(JSContext *ctx);
/* Function.prototype of the context */
JSValue JS_GetFunctionProto(JSContext *ctx);
int JS_OrdinaryIsInstanceOf(JSContext *ctx, JSValueConst val,
                            JSValueConst obj);
int JS_IsInstanceOf(JSContext *ctx, JSValueConst val, JSValueConst obj);
int JS_DefineProperty(JSContext *ctx, JSValueConst this_obj,
                      JSAtom prop, JSValueConst val,
                      JSValueConst getter, JSValueConst setter, int flags);
int JS_DefinePropertyValue(JSContext *ctx, JSValueConst this_obj,
                           JSAtom prop, JSValue val, int flags);
int JS_DefinePropertyValueUint32(JSContext *ctx, JSValueConst this_obj,
                                 uint32_t idx, JSValue val, int flags);
int JS_DefinePropertyValueStr(JSContext *ctx, JSValueConst this_obj,
                              const char *prop, JSValue val, int flags);
int JS_DefinePropertyGetSet(JSContext *ctx, JSValueConst this_obj,
                            JSAtom prop, JSValue getter, JSValue setter,
                            int flags);
void JS_SetOpaque(JSValue obj, void *opaque);
void *JS_GetOpaque(JSValueConst obj, JSClassID class_id);
void *JS_GetOpaque2(JSContext *ctx, JSValueConst obj, JSClassID class_id);
void *JS_GetAnyOpaque(JSValueConst obj, JSClassID *class_id);

/* 'buf' must be zero terminated i.e. buf[buf_len] = '\0'. */
JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename);
#define JS_PARSE_JSON_EXT (1 << 0) /* allow extended JSON */
JSValue JS_ParseJSON2(JSContext *ctx, const char *buf, size_t buf_len,
                      const char *filename, int flags);
JSValue JS_JSONStringify(JSContext *ctx, JSValueConst obj,
                         JSValueConst replacer, JSValueConst space0);

typedef void JSFreeArrayBufferDataFunc(JSRuntime *rt, void *opaque, void *ptr);
JSValue JS_NewArrayBuffer(JSContext *ctx, uint8_t *buf, size_t len,
                          JSFreeArrayBufferDataFunc *free_func, void *opaque,
                          JS_BOOL is_shared);
JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len);
/* does nothing if 'obj' is an immutable ArrayBuffer */
void JS_DetachArrayBuffer(JSContext *ctx, JSValueConst obj);
uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj);
/* TRUE for an ArrayBuffer (not a SharedArrayBuffer) */
JS_BOOL JS_IsArrayBuffer(JSValueConst val);
/* TRUE or FALSE if 'obj' is an ArrayBuffer, -1 otherwise */
int JS_IsImmutableArrayBuffer(JSValueConst obj);
/* set whether the ArrayBuffer 'obj' is immutable: 0, or -1 if not an ArrayBuffer */
int JS_SetImmutableArrayBuffer(JSValueConst obj, JS_BOOL immutable);

typedef enum JSTypedArrayEnum {
    JS_TYPED_ARRAY_UINT8C = 0,
    JS_TYPED_ARRAY_INT8,
    JS_TYPED_ARRAY_UINT8,
    JS_TYPED_ARRAY_INT16,
    JS_TYPED_ARRAY_UINT16,
    JS_TYPED_ARRAY_INT32,
    JS_TYPED_ARRAY_UINT32,
    JS_TYPED_ARRAY_BIG_INT64,
    JS_TYPED_ARRAY_BIG_UINT64,
    JS_TYPED_ARRAY_FLOAT16,
    JS_TYPED_ARRAY_FLOAT32,
    JS_TYPED_ARRAY_FLOAT64,
} JSTypedArrayEnum;

JSValue JS_NewTypedArray(JSContext *ctx, int argc, JSValueConst *argv,
                         JSTypedArrayEnum array_type);
/* the JSTypedArrayEnum of a typed array, or -1 */
int JS_GetTypedArrayType(JSValueConst obj);
JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj,
                               size_t *pbyte_offset,
                               size_t *pbyte_length,
                               size_t *pbytes_per_element);
/* new Uint8Array covering JS_NewArrayBuffer(buf, len, ...) */
JSValue JS_NewUint8Array(JSContext *ctx, uint8_t *buf, size_t len,
                         JSFreeArrayBufferDataFunc *free_func, void *opaque,
                         JS_BOOL is_shared);
/* new Uint8Array over a copy of 'buf' */
JSValue JS_NewUint8ArrayCopy(JSContext *ctx, const uint8_t *buf, size_t len);
/* data and byte length of a Uint8Array or Uint8ClampedArray view.
   Return NULL if exception. WARNING: any JS call can detach or resize
   the buffer and render the returned pointer invalid */
uint8_t *JS_GetUint8Array(JSContext *ctx, size_t *psize, JSValueConst obj);
/* data of a typed array view of any element type (the start of the view,
   not of its buffer), its current byte length (which follows the buffer
   for a length-tracking view), its byte offset in the buffer and its
   element size. pbyte_offset, pbyte_length or pbytes_per_element can be
   NULL. Unlike JS_GetTypedArrayBuffer(), the buffer object is not
   referenced and no exception is raised: return NULL if 'obj' is not a
   typed array (see JS_GetTypedArrayType()) or if the view is out of
   bounds or its buffer is detached. WARNING: any JS call can detach or
   resize the buffer and render the returned pointer invalid */
uint8_t *JS_GetTypedArrayData(JSContext *ctx, JSValueConst obj,
                              size_t *pbyte_offset,
                              size_t *pbyte_length,
                              size_t *pbytes_per_element);
/* sab_free(), sab_dup() and the SharedArrayBuffer memory returned by
   JS_WriteObject2()/JS_ReadObject2() use the pointer returned by
   sab_alloc(). For a growable SharedArrayBuffer, the engine stores the
   byte length shared by its clones at the start of this memory. */
typedef struct {
    void *(*sab_alloc)(void *opaque, size_t size);
    void (*sab_free)(void *opaque, void *ptr);
    void (*sab_dup)(void *opaque, void *ptr);
    void *sab_opaque;
} JSSharedArrayBufferFunctions;
void JS_SetSharedArrayBufferFunctions(JSRuntime *rt,
                                      const JSSharedArrayBufferFunctions *sf);

typedef enum JSPromiseStateEnum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

JSValue JS_NewPromiseCapability(JSContext *ctx, JSValue *resolving_funcs);
JSPromiseStateEnum JS_PromiseState(JSContext *ctx, JSValue promise);
/* consider a rejected promise as handled (the rejection tracker is told
   so if it was not) */
void JS_PromiseMarkAsHandled(JSContext *ctx, JSValueConst promise);
JSValue JS_PromiseResult(JSContext *ctx, JSValue promise);
JS_BOOL JS_IsPromise(JSValueConst val);
/* same as Promise.resolve(value) or Promise.reject(value) */
JSValue JS_NewSettledPromise(JSContext *ctx, JS_BOOL is_reject,
                             JSValueConst value);
/* same as promise.then(on_fulfilled, on_rejected) for a promise object,
   but neither 'then' nor Symbol.species are looked up: the result is a
   new %Promise% */
JSValue JS_PromiseThen(JSContext *ctx, JSValueConst promise,
                       JSValueConst on_fulfilled, JSValueConst on_rejected);

typedef enum JSPromiseHookType {
    JS_PROMISE_HOOK_INIT, /* a promise is created */
    JS_PROMISE_HOOK_BEFORE, /* before the 'then' of a thenable resolution is called */
    JS_PROMISE_HOOK_AFTER, /* after the 'then' of a thenable resolution is called */
    JS_PROMISE_HOOK_RESOLVE, /* a promise is fulfilled (not when rejected) */
} JSPromiseHookType;

/* 'parent_promise' is only set for JS_PROMISE_HOOK_INIT: the promise
   whose then() created 'promise', JS_UNDEFINED otherwise. The hook must
   not run JS code that throws: its exceptions are ignored. */
typedef void JSPromiseHook(JSContext *ctx, JSPromiseHookType type,
                           JSValueConst promise, JSValueConst parent_promise,
                           void *opaque);
/* NULL removes the hook */
void JS_SetPromiseHook(JSRuntime *rt, JSPromiseHook *promise_hook, void *opaque);

/* is_handled = TRUE means that the rejection is handled */
typedef void JSHostPromiseRejectionTracker(JSContext *ctx, JSValueConst promise,
                                           JSValueConst reason,
                                           JS_BOOL is_handled, void *opaque);
void JS_SetHostPromiseRejectionTracker(JSRuntime *rt, JSHostPromiseRejectionTracker *cb, void *opaque);

/* return != 0 if the JS code needs to be interrupted */
typedef int JSInterruptHandler(JSRuntime *rt, void *opaque);
void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque);
/* return the current interrupt handler (NULL if none) and store its
   opaque in '*popaque' if 'popaque' is not NULL. Used to chain
   handlers. */
JSInterruptHandler *JS_GetInterruptHandler(JSRuntime *rt, void **popaque);
/* Call the interrupt handler at the next poll of the code running in
   'ctx' instead of at the end of the current polling period (every
   10000 bytecode instructions). Must be called from the thread running
   'ctx', e.g. in a native function. */
void JS_TriggerInterruptHandler(JSContext *ctx);

/* Return the number of executed bytecode branch safepoints in 'ctx': the
   backward and conditional branches of the bytecode, the only points
   where a running synchronous execution can be suspended. The count is
   incremented unconditionally at every such branch, so summing it over
   every process slice of a suspended-and-restored execution gives the
   count of the uninterrupted execution.

   It is process and context local and deliberately not serialized: a
   context restored with JS_ReadSnapshot() starts again at zero. It
   counts executed safepoints, not calls to the interrupt handler
   (JSInterruptHandler), whose rate depends on JS_INTERRUPT_COUNTER_INIT. */
uint64_t JS_GetProgramBranchCount(JSContext *ctx);

/* Suspension

   A running synchronous execution can be suspended in-process, then
   resumed (or saved with JS_WriteSnapshot()). The suspension happens at
   a backward or conditional branch of the bytecode, when the interrupt
   counter expires (i.e. at the next interrupt handler call), and only
   if the whole call stack is suspendable:
   - every frame is a normal (non generator, non async) JS function,
   - the outermost frame is the JS function called directly by
     JS_Call(), JS_CallConstructor(), JS_CallConstructor2(), JS_Invoke()
     or the script run by JS_Eval()/JS_EvalThis()/JS_EvalFunction(),
     with no JS or native code running (in particular not from a C
     function, a class call handler, JS_ExecutePendingJob() or
     JS_ExecutePendingJobs()),
   - every other frame was called by the previous one through a plain
     call, method call, 'new', spread call or tail call, possibly
     through bound functions.
   JS code run by the engine on behalf of native code is not
   suspendable: getters, setters, conversions (valueOf, toString...),
   Proxy traps, promise resolution (thenable 'then'), callbacks of
   native functions (Array.prototype.forEach, JSON.stringify,
   DisposableStack.prototype.dispose...), eval code and
   Function.prototype.call/apply calls, including when they
   are reached from host API calls such as JS_GetProperty(),
   JS_ToString() or a JS_Call() of a bound function or of a native
   function. Otherwise the request stays pending until a later
   safepoint where the stack is suspendable.

   When the suspension happens, the host API call that started the
   execution returns JS_EXCEPTION and JS_IsSuspended() returns TRUE. The
   pending exception is an internal uncatchable marker which is
   cleared by JS_Resume(). The 'catch' and 'finally' clauses are not
   executed during the suspension, nor the disposal of the 'using'
   declarations (the dispose methods called when leaving the scope of
   a 'using' declaration are method calls of the frame: they are
   suspendable). Only one execution can be suspended at a time.
   JS_FreeRuntime() releases a suspended execution.

   If a suspended frame cannot be resumed (stack overflow, interruption
   by the interrupt handler), it and its suspended callees are
   terminated: their 'catch' and 'finally' clauses are not executed
   and their 'using' resources are not disposed, but the closures
   they created keep working. The error is thrown at the
   call site of the frame (JS_Resume() returns it for the outermost
   frame). An out of memory error while suspending terminates the
   execution in the same way with an uncatchable error. */

/* Ask the engine to suspend the running JS at the next safepoint.
   Typically called from an interrupt handler. */
void JS_RequestSuspend(JSRuntime *rt);
/* TRUE if a suspended execution is pending. */
JS_BOOL JS_IsSuspended(JSRuntime *rt);
/* TRUE if the pending exception is the marker of a suspension, i.e. the
   host call which returned JS_EXCEPTION was suspended (other host calls
   can run and fail while an execution is suspended). */
JS_BOOL JS_IsSuspendException(JSContext *ctx);
/* Continue a suspended execution. Returns the completion value of the
   host call that was suspended (JS_Eval(), JS_Call()...), or
   JS_EXCEPTION (check JS_IsSuspended() first: it may have been
   suspended again). */
JSValue JS_Resume(JSContext *ctx);

/* Suspend the JS code which called the running C function (or class
   'call' handler), at the call. The C function must return the result
   of JS_SuspendCall(), JS_EXCEPTION: the execution is suspended as with
   JS_RequestSuspend() and the call completes when it is resumed, with
   the value given to JS_ResumeWith() (undefined with JS_Resume()). The
   stack must be suspendable from the caller (see JS_RequestSuspend()),
   which must call the C function directly: otherwise a TypeError is
   thrown. The suspended execution can be saved with JS_WriteSnapshot(),
   e.g. to complete the call in another process. */
JSValue JS_SuspendCall(JSContext *ctx);
/* Same as JS_Resume() but the call suspended by JS_SuspendCall()
   returns 'result', or throws it if 'throw_flag' is TRUE. 'result' is
   freed (it is ignored if no call was suspended by JS_SuspendCall()). */
JSValue JS_ResumeWith(JSContext *ctx, JSValue result, JS_BOOL throw_flag);

/* if can_block is TRUE, Atomics.wait() can be used */
void JS_SetCanBlock(JSRuntime *rt, JS_BOOL can_block);
/* select which debug info is stripped from the compiled code */
#define JS_STRIP_SOURCE (1 << 0) /* strip source code */
#define JS_STRIP_DEBUG  (1 << 1) /* strip all debug info including source code */
void JS_SetStripInfo(JSRuntime *rt, int flags);
int JS_GetStripInfo(JSRuntime *rt);

/* set the [IsHTMLDDA] internal slot */
void JS_SetIsHTMLDDA(JSContext *ctx, JSValueConst obj);

typedef struct JSModuleDef JSModuleDef;

/* return the module specifier (allocated with js_malloc()) or NULL if
   exception */
typedef char *JSModuleNormalizeFunc(JSContext *ctx,
                                    const char *module_base_name,
                                    const char *module_name, void *opaque);
/* same as JSModuleNormalizeFunc with the import attributes (object or
   undefined) */
typedef char *JSModuleNormalizeFunc2(JSContext *ctx,
                                     const char *module_base_name,
                                     const char *module_name,
                                     JSValueConst attributes, void *opaque);
typedef JSModuleDef *JSModuleLoaderFunc(JSContext *ctx,
                                        const char *module_name, void *opaque);
typedef JSModuleDef *JSModuleLoaderFunc2(JSContext *ctx,
                                         const char *module_name, void *opaque,
                                         JSValueConst attributes);
/* return -1 if exception, 0 if OK */
typedef int JSModuleCheckSupportedImportAttributes(JSContext *ctx, void *opaque,
                                                   JSValueConst attributes);
                                                   
/* module_normalize = NULL is allowed and invokes the default module
   filename normalizer */
void JS_SetModuleLoaderFunc(JSRuntime *rt,
                            JSModuleNormalizeFunc *module_normalize,
                            JSModuleLoaderFunc *module_loader, void *opaque);
/* same as JS_SetModuleLoaderFunc but with attributes. if
   module_check_attrs = NULL, no attribute checking is done. */
void JS_SetModuleLoaderFunc2(JSRuntime *rt,
                             JSModuleNormalizeFunc *module_normalize,
                             JSModuleLoaderFunc2 *module_loader,
                             JSModuleCheckSupportedImportAttributes *module_check_attrs,
                             void *opaque);
/* replace the module normalizer with one receiving the import attributes
   (NULL for the default normalizer). Call it after
   JS_SetModuleLoaderFunc*(), which reset it. The modules stay keyed by
   name and attributes. */
void JS_SetModuleNormalizeFunc2(JSRuntime *rt,
                                JSModuleNormalizeFunc2 *module_normalize);
/* return the import.meta object of a module */
JSValue JS_GetImportMeta(JSContext *ctx, JSModuleDef *m);
JSAtom JS_GetModuleName(JSContext *ctx, JSModuleDef *m);
JSValue JS_GetModuleNamespace(JSContext *ctx, JSModuleDef *m);

/* JS Job support */

typedef JSValue JSJobFunc(JSContext *ctx, int argc, JSValueConst *argv);
int JS_EnqueueJob(JSContext *ctx, JSJobFunc *job_func, int argc, JSValueConst *argv);

JS_BOOL JS_IsJobPending(JSRuntime *rt);
/* the realm of the next pending job, NULL if none */
JSContext *JS_GetPendingJobContext(JSRuntime *rt);
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx);
/* Execute the pending jobs in order, including the jobs enqueued
   meanwhile, until none is left or 'max_jobs' were executed (no limit
   if 'max_jobs' < 0). Return the number of executed jobs and set
   '*pctx' (if pctx != NULL) to NULL. If a job throws, stop there and
   return -1 with '*pctx' set to its context, in which the exception is
   pending (NULL if the job destroyed it), like JS_ExecutePendingJob().
   Same as calling JS_ExecutePendingJob() in a loop, without the host
   regaining control between the jobs: jobs are never suspendable, and
   a suspension requested meanwhile (JS_RequestSuspend()) stays pending
   for the next host call as with the loop. Use 'max_jobs' to bound the
   work done in one call. */
int JS_ExecutePendingJobs(JSRuntime *rt, int max_jobs, JSContext **pctx);

/* Object Writer/Reader (currently only used to handle precompiled code) */
#define JS_WRITE_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_WRITE_OBJ_BSWAP     (1 << 1) /* byte swapped output */
#define JS_WRITE_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_WRITE_OBJ_REFERENCE (1 << 3) /* allow object references to
                                           encode arbitrary object
                                           graph */
/* the strip flags apply when writing, JS_SetStripInfo() when compiling */
#define JS_WRITE_OBJ_STRIP_SOURCE (1 << 4) /* do not write the function source code */
#define JS_WRITE_OBJ_STRIP_DEBUG  (1 << 5) /* do not write the debug info (file name, line numbers, source code) */
/* The output starts with a version byte and a checksum of the rest of
   the output: JS_ReadObject() rejects a different version ("invalid
   version") and a corrupted input ("checksum error", both
   SyntaxError). Besides the JSON values, the Date, RegExp (source and
   flags, lastIndex is reset), Map, Set, ArrayBuffer, typed array,
   Number/String/Boolean/BigInt objects and the symbols (Symbol.for()
   and well-known symbols keep their identity) are supported. */
uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj,
                        int flags);
uint8_t *JS_WriteObject2(JSContext *ctx, size_t *psize, JSValueConst obj,
                         int flags, uint8_t ***psab_tab, size_t *psab_tab_len);

#define JS_READ_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_READ_OBJ_ROM_DATA  (1 << 1) /* avoid duplicating 'buf' data */
#define JS_READ_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_READ_OBJ_REFERENCE (1 << 3) /* allow object references */
JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                      int flags);
/* same as JS_ReadObject() and return in *psab_tab (allocated with
   js_malloc(), if not NULL) the memory of the SharedArrayBuffers read */
JSValue JS_ReadObject2(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                       int flags, uint8_t ***psab_tab, size_t *psab_tab_len);
/* instantiate and evaluate a bytecode function. Only used when
   reading a script or module with JS_ReadObject() */
JSValue JS_EvalFunction(JSContext *ctx, JSValue fun_obj);
/* load the dependencies of the module 'obj'. Useful when JS_ReadObject()
   returns a module. */
int JS_ResolveModule(JSContext *ctx, JSValueConst obj);
/* Called by JS_EnumCodeNames() for each name; 'name' is only valid
   during the call. */
typedef void JSCodeNameFunc(JSContext *ctx, JSAtom name, void *opaque);
/* Call 'func' for each name that the code 'obj' (a script or module
   compiled with JS_EVAL_FLAG_COMPILE_ONLY) may look up at run time:
   variable and property names, string constants and module imports, and
   the global needed by a construct: "Promise" for modules and async
   functions, "RegExp" for regexp literals and "import" for import().
   A name can be given several times. Names computed at run time are not
   seen. qjsc uses it to omit the unused built-ins (-fno-unused). Return
   -1 if out of memory. */
int JS_EnumCodeNames(JSContext *ctx, JSValueConst obj,
                     JSCodeNameFunc *func, void *opaque);

/* Heap snapshots

   JS_WriteSnapshot() serializes the complete state of a runtime: every
   context, object, closure, suspended async function or generator, the
   pending jobs and, when JS_IsSuspended(), the suspended call stack.
   JS_ReadSnapshot() rebuilds that state in a fresh runtime of the same
   engine build, in any process.

   Code pointers stored in the heap are encoded through a registry made of
   the engine builtins and the host natives listed in JSSnapshotHost. The
   host must give the same natives in the same order when reading.

   Incremental snapshots: a full snapshot written with JS_SNAPSHOT_BASE
   becomes the base of the runtime (e.g. once the host natives are set
   up and the application script has run). A snapshot written later with
   JS_SNAPSHOT_DELTA only contains what differs from the base: the cells
   (objects, closure variables, frames...) that were modified or created
   since, their new strings and atoms, and the roots. JS_ReadSnapshot()
   applies a delta on top of its base. A runtime restored from a base
   (with or without a delta) has that base attached, so its next deltas
   are relative to the same base. Deltas are not chained: each one only
   needs the base.

   A base is reproducible: runtimes of the same engine build set up the
   same way (same host natives, same code run) write the same base bytes,
   in any process. A host can therefore rebuild a base instead of storing
   it and only keep the deltas (qjs --drepl does). The state of
   Math.random() and the time origin of performance.now() are not part
   of a base: full snapshots and deltas save them, and a base read
   without a delta gets a fresh seed and time origin.

   While a base is attached, the runtime references every cell, string
   and atom of the base so that they keep their identity: they are not
   garbage collected (weak references to them never become empty) until
   JS_DropSnapshotBase() or JS_FreeRuntime().

   ArrayBuffer data is always restored into engine-owned memory (host
   buffers are copied and their free function is not kept).

   Unsupported (JS_WriteSnapshot() fails with a TypeError): host-class
   objects with opaque data when no save_opaque hook is given, C closures
   (JS_NewCClosure()), contexts
   with the Intl or Temporal intrinsics, SharedArrayBuffers whose memory may be
   shared (host SharedArrayBuffer allocator or host memory), and JS code
   running on the C stack (snapshots are taken between jobs or while
   suspended).

   Like bytecode, a snapshot is trusted input: JS_ReadSnapshot() detects
   truncated or mismatched snapshots but not malicious ones. */

struct JSCFunctionListEntry;

typedef struct JSSnapshotHost {
    /* host code pointers that can be stored in the heap: C functions
       given to JS_NewCFunction*(), JSCFunctionData callbacks, job
       functions, C module init functions */
    const void * const *natives;
    int native_count;
    /* host function lists given to JS_SetPropertyFunctionList(): lazily
       instantiated properties keep a pointer to their entry */
    const struct JSCFunctionListEntry * const *func_lists;
    const int *func_list_lens;
    int func_list_count;
    /* Host classes (class_id >= JS_CLASS_INIT_COUNT) with opaque data.
       save_opaque() returns a JS value describing the opaque data of 'obj'
       (JS_UNDEFINED if none) or JS_EXCEPTION. load_opaque() is called once
       the whole heap is rebuilt and must attach the opaque data with
       JS_SetOpaque(); it returns < 0 and throws on error. */
    JSValue (*save_opaque)(JSContext *ctx, JSValueConst obj, void *opaque);
    int (*load_opaque)(JSContext *ctx, JSValueConst obj, JSValueConst saved,
                       void *opaque);
    void *opaque;
} JSSnapshotHost;

/* full snapshot that becomes the base of the runtime (replacing the
   previous base) */
#define JS_SNAPSHOT_BASE  (1 << 0)
/* only the differences with the base of the runtime */
#define JS_SNAPSHOT_DELTA (1 << 1)

/* Serialize the runtime of 'ctx' ('flags' is 0 for a self-contained
   snapshot, or one of JS_SNAPSHOT_x). 'roots' are values held by the host
   that JS_ReadSnapshot() hands back in the same order. Return a buffer to
   free with js_free(), or NULL with a pending exception. */
uint8_t *JS_WriteSnapshot(JSContext *ctx, size_t *psize,
                          const JSSnapshotHost *host,
                          JSValueConst *roots, int root_count, int flags);

typedef struct JSSnapshotResult {
    /* contexts in creation order; the host owns one reference to each */
    JSContext **contexts;
    int context_count;
    /* the 'roots' given to JS_WriteSnapshot(); owned by the host */
    JSValue *roots;
    int root_count;
} JSSnapshotResult;

/* Rebuild a full snapshot 'buf', with the delta 'delta' applied on top
   of it if not NULL ('buf' must then be its base), into 'rt', a runtime
   without contexts on which the host registered the same classes as
   when writing. Return 0 on success, or -1 with a message in 'errbuf';
   the runtime must then be freed. */
int JS_ReadSnapshot(JSRuntime *rt, const uint8_t *buf, size_t buf_len,
                    const uint8_t *delta, size_t delta_len,
                    const JSSnapshotHost *host, JSSnapshotResult *res,
                    char *errbuf, size_t errbuf_size);
/* Detach the base of the incremental snapshots (if any): the cells of
   the base can be garbage collected again. */
void JS_DropSnapshotBase(JSRuntime *rt);
/* free the arrays of 'res' (not the contexts and values it references) */
void JS_FreeSnapshotResult(JSRuntime *rt, JSSnapshotResult *res);

/* only exported for os.Worker() */
JSAtom JS_GetScriptOrModuleName(JSContext *ctx, int n_stack_levels);
/* only exported for os.Worker() */
JSValue JS_LoadModule(JSContext *ctx, const char *basename,
                      const char *filename);

/* C function definition */
typedef enum JSCFunctionEnum {  /* XXX: should rename for namespace isolation */
    JS_CFUNC_generic,
    JS_CFUNC_generic_magic,
    JS_CFUNC_constructor,
    JS_CFUNC_constructor_magic,
    JS_CFUNC_constructor_or_func,
    JS_CFUNC_constructor_or_func_magic,
    JS_CFUNC_constructor_or_func_flags,
    JS_CFUNC_f_f,
    JS_CFUNC_f_f_f,
    JS_CFUNC_getter,
    JS_CFUNC_setter,
    JS_CFUNC_getter_magic,
    JS_CFUNC_setter_magic,
    JS_CFUNC_iterator_next,
} JSCFunctionEnum;

typedef union JSCFunctionType {
    JSCFunction *generic;
    JSValue (*generic_magic)(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
    JSCFunction *constructor;
    JSValue (*constructor_magic)(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic);
    JSCFunction *constructor_or_func;
    JSValue (*constructor_or_func_flags)(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val, int argc, JSValueConst *argv, int flags);
    double (*f_f)(double);
    double (*f_f_f)(double, double);
    JSValue (*getter)(JSContext *ctx, JSValueConst this_val);
    JSValue (*setter)(JSContext *ctx, JSValueConst this_val, JSValueConst val);
    JSValue (*getter_magic)(JSContext *ctx, JSValueConst this_val, int magic);
    JSValue (*setter_magic)(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);
    JSValue (*iterator_next)(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int *pdone, int magic);
} JSCFunctionType;

JSValue JS_NewCFunction2(JSContext *ctx, JSCFunction *func,
                         const char *name,
                         int length, JSCFunctionEnum cproto, int magic);
/* same as JS_NewCFunction2() with an explicit prototype 'proto_val' and
   room preallocated for 'n_fields' properties */
JSValue JS_NewCFunction3(JSContext *ctx, JSCFunction *func,
                         const char *name,
                         int length, JSCFunctionEnum cproto, int magic,
                         JSValueConst proto_val, int n_fields);
JSValue JS_NewCFunctionData(JSContext *ctx, JSCFunctionData *func,
                            int length, int magic, int data_len,
                            JSValueConst *data);
/* same as JS_NewCFunctionData() with a function name (NULL for '') */
JSValue JS_NewCFunctionData2(JSContext *ctx, JSCFunctionData *func,
                             const char *name,
                             int length, int magic, int data_len,
                             JSValueConst *data);
/* called with the 'opaque' pointer when the closure is freed */
typedef void JSCClosureFinalizerFunc(void *opaque);
/* C function with a host 'opaque' pointer, given to 'func' on each call
   and to 'opaque_finalize' (if not NULL) when the function is freed.
   JS_WriteSnapshot() rejects C closures. */
JSValue JS_NewCClosure(JSContext *ctx, JSCClosure *func, const char *name,
                       JSCClosureFinalizerFunc *opaque_finalize,
                       int length, int magic, void *opaque);

static inline JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func, const char *name,
                                      int length)
{
    return JS_NewCFunction2(ctx, func, name, length, JS_CFUNC_generic, 0);
}

static inline JSValue JS_NewCFunctionMagic(JSContext *ctx, JSCFunctionMagic *func,
                                           const char *name,
                                           int length, JSCFunctionEnum cproto, int magic)
{
    /* Used to squelch a -Wcast-function-type warning. */
    JSCFunctionType ft = { .generic_magic = func };
    return JS_NewCFunction2(ctx, ft.generic, name, length, cproto, magic);
}
int JS_SetConstructor(JSContext *ctx, JSValueConst func_obj,
                      JSValueConst proto);

/* C property definition */

typedef struct JSCFunctionListEntry {
    const char *name;
    uint8_t prop_flags;
    uint8_t def_type;
    int16_t magic;
    union {
        struct {
            uint8_t length; /* XXX: should move outside union */
            uint8_t cproto; /* XXX: should move outside union */
            JSCFunctionType cfunc;
        } func;
        struct {
            JSCFunctionType get;
            JSCFunctionType set;
        } getset;
        struct {
            const char *name;
            int base;
        } alias;
        struct {
            const struct JSCFunctionListEntry *tab;
            int len;
        } prop_list;
        const char *str;
        int32_t i32;
        int64_t i64;
        uint64_t u64;
        double f64;
    } u;
} JSCFunctionListEntry;

#define JS_DEF_CFUNC          0
#define JS_DEF_CGETSET        1
#define JS_DEF_CGETSET_MAGIC  2
#define JS_DEF_PROP_STRING    3
#define JS_DEF_PROP_INT32     4
#define JS_DEF_PROP_INT64     5
#define JS_DEF_PROP_DOUBLE    6
#define JS_DEF_PROP_UNDEFINED 7
#define JS_DEF_OBJECT         8
#define JS_DEF_ALIAS          9
#define JS_DEF_PROP_ATOM     10
#define JS_DEF_PROP_BOOL     11

/* Note: c++ does not like nested designators */
#define JS_CFUNC_DEF(name, length, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } }
/* same as JS_CFUNC_DEF() with explicit property flags */
#define JS_CFUNC_DEF2(name, length, func1, prop_flags) { name, prop_flags, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } }
#define JS_CFUNC_MAGIC_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } } }
#define JS_CFUNC_SPECIAL_DEF(name, length, cproto, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_ ## cproto, { .cproto = func1 } } } }
#define JS_ITERATOR_NEXT_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_iterator_next, { .iterator_next = func1 } } } }
#define JS_CGETSET_DEF(name, fgetter, fsetter) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, .u = { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } }
/* same as JS_CGETSET_DEF() with explicit property flags */
#define JS_CGETSET_DEF2(name, fgetter, fsetter, prop_flags) { name, prop_flags, JS_DEF_CGETSET, 0, .u = { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } }
#define JS_CGETSET_MAGIC_DEF(name, fgetter, fsetter, magic) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, magic, .u = { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } }
/* same as JS_CGETSET_MAGIC_DEF() with explicit property flags */
#define JS_CGETSET_MAGIC_DEF2(name, fgetter, fsetter, magic, prop_flags) { name, prop_flags, JS_DEF_CGETSET_MAGIC, magic, .u = { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } }
#define JS_PROP_STRING_DEF(name, cstr, prop_flags) { name, prop_flags, JS_DEF_PROP_STRING, 0, .u = { .str = cstr } }
#define JS_PROP_INT32_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT32, 0, .u = { .i32 = val } }
#define JS_PROP_INT64_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT64, 0, .u = { .i64 = val } }
#define JS_PROP_DOUBLE_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, .u = { .f64 = val } }
/* double property given by its IEEE 754 bit pattern 'val' (e.g. a
   specific NaN) */
#define JS_PROP_U2D_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, .u = { .u64 = val } }
#define JS_PROP_UNDEFINED_DEF(name, prop_flags) { name, prop_flags, JS_DEF_PROP_UNDEFINED, 0, .u = { .i32 = 0 } }
#define JS_PROP_ATOM_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_ATOM, 0, .u = { .i32 = val } }
#define JS_PROP_BOOL_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_BOOL, 0, .u = { .i32 = val } }
#define JS_OBJECT_DEF(name, tab, len, prop_flags) { name, prop_flags, JS_DEF_OBJECT, 0, .u = { .prop_list = { tab, len } } }
#define JS_ALIAS_DEF(name, from) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, -1 } } }
#define JS_ALIAS_BASE_DEF(name, from, base) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, base } } }

int JS_SetPropertyFunctionList(JSContext *ctx, JSValueConst obj,
                               const JSCFunctionListEntry *tab,
                               int len);

#define JS_NEW_CTOR_NO_GLOBAL   (1 << 0) /* don't create a global binding */
#define JS_NEW_CTOR_PROTO_CLASS (1 << 1) /* the prototype class is 'class_id' instead of JS_CLASS_OBJECT */
#define JS_NEW_CTOR_PROTO_EXIST (1 << 2) /* the prototype is already defined */
#define JS_NEW_CTOR_READONLY    (1 << 3) /* read-only constructor field */

JSValue JS_NewCConstructor(JSContext *ctx, int class_id, const char *name,
                           JSCFunction *func, int length, JSCFunctionEnum cproto, int magic,
                           JSValueConst parent_ctor,
                           const JSCFunctionListEntry *ctor_fields, int n_ctor_fields,
                           const JSCFunctionListEntry *proto_fields, int n_proto_fields,
                           int flags);

/* iterator helpers */

JSValue JS_GetIterator(JSContext *ctx, JSValueConst obj, JS_BOOL is_async);
JSValue JS_IteratorNext(JSContext *ctx, JSValueConst enum_obj,
                        JSValueConst method, JS_BOOL *pdone);
int JS_IteratorClose(JSContext *ctx, JSValueConst enum_obj,
                     JS_BOOL is_exception_pending);

#ifdef CONFIG_TEMPORAL_INTL
/*
 * Internal string helpers: caller must ensure 'val' is a JS_TAG_STRING
 * (use JS_ToString() which never returns a rope). We may later export
 * an opaque struct JSString and use it as parameter instead.
 */
int js_string_get_len(JSValueConst val);
int js_string_getc(JSValueConst val, int *pidx);
JSValue js_sub_string_val(JSContext *ctx, JSValueConst val, int start, int end);
#endif /* CONFIG_TEMPORAL_INTL */

/* C module definition */

typedef int JSModuleInitFunc(JSContext *ctx, JSModuleDef *m);

JSModuleDef *JS_NewCModule(JSContext *ctx, const char *name_str,
                           JSModuleInitFunc *func);
/* can only be called before the module is instantiated */
int JS_AddModuleExport(JSContext *ctx, JSModuleDef *m, const char *name_str);
int JS_AddModuleExportList(JSContext *ctx, JSModuleDef *m,
                           const JSCFunctionListEntry *tab, int len);
/* can only be called after the module is instantiated */
int JS_SetModuleExport(JSContext *ctx, JSModuleDef *m, const char *export_name,
                       JSValue val);
int JS_SetModuleExportList(JSContext *ctx, JSModuleDef *m,
                           const JSCFunctionListEntry *tab, int len);
/* associate a JSValue to a C module */
int JS_SetModulePrivateValue(JSContext *ctx, JSModuleDef *m, JSValue val);
JSValue JS_GetModulePrivateValue(JSContext *ctx, JSModuleDef *m);
/* set the import attributes (object or undefined) of a module created
   before being imported (e.g. a JSON module embedded by qjsc): the
   imports find a loaded module by name and attributes. 'attributes' is
   freed by the function. */
int JS_SetModuleAttributes(JSContext *ctx, JSModuleDef *m, JSValue attributes);
                        
/* locale specific callbacks to override the default functions */
typedef struct JSLocaleCallbacks {
    JSValue (*number_toLocaleString)(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
    JSValue (*date_toLocaleString)(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int part);
    JSValue (*string_localeCompare)(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv);
    JSValue (*bigint_toLocaleString)(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
    int (*get_string_locale_lang)(JSContext *ctx, int argc, JSValueConst *argv);
} JSLocaleCallbacks;

void JS_SetLocaleCallbacks(JSContext *ctx, const JSLocaleCallbacks *callbacks);

/* debug value output */

typedef struct {
    JS_BOOL show_hidden : 8; /* only show enumerable properties */
    JS_BOOL raw_dump : 8; /* avoid doing autoinit and avoid any malloc() call (for internal use) */
    uint32_t max_depth; /* recurse up to this depth, 0 = no limit */
    uint32_t max_string_length; /* print no more than this length for
                                   strings, 0 = no limit */
    uint32_t max_item_count; /*  print no more than this count for
                                 arrays or objects, 0 = no limit */
} JSPrintValueOptions;

typedef void JSPrintValueWrite(void *opaque, const char *buf, size_t len);

void JS_PrintValueSetDefaultOptions(JSPrintValueOptions *options);
void JS_PrintValueRT(JSRuntime *rt, JSPrintValueWrite *write_func, void *write_opaque,
                     JSValueConst val, const JSPrintValueOptions *options);
void JS_PrintValue(JSContext *ctx, JSPrintValueWrite *write_func, void *write_opaque,
                   JSValueConst val, const JSPrintValueOptions *options);

#undef js_unlikely
#undef js_force_inline

/* engine version (the VERSION file): JS_GetVersion() returns
   QJS_VERSION_STRING */
#define QJS_VERSION_YEAR 2026
#define QJS_VERSION_MONTH 6
#define QJS_VERSION_DAY 4
#define QJS_VERSION_STRING "2026-06-04"
const char *JS_GetVersion(void);

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* QUICKJS_H */
