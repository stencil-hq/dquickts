/*
 * dquickts Python, milestone M2: private declarations of python/ceval.c
 * used by the OP_py handlers (python/ceval_ops.h, included inside
 * JS_CallInternal) and by the quickjs.c integration.  Included by
 * quickjs.c right after python/object.h.
 */
#ifndef QP_CEVAL_H
#define QP_CEVAL_H

struct JSStackFrame;
struct JSFunctionBytecode;

/* allocate the inline caches of a new Python bytecode (kinds from the
   IC operands of its subops); 0 or -1 (exception pending) */
static int qp_code_info_setup_ic(JSContext *ctx, struct JSFunctionBytecode *b);

static JSValue qp_load_common_const(JSContext *ctx, struct JSStackFrame *sf, int id);
static JSValue qp_ceval_load_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                    QPGlobalCache *ic);
static int qp_ceval_store_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                 JSValueConst v);
static int qp_ceval_delete_global(JSContext *ctx, JSValueConst func, JSAtom atom);
/* mapping[str(atom)] without KeyError: 1 (*pv new ref) / 0 / -1 */
static int qp_ceval_mapping_get(JSContext *ctx, JSValueConst mapping, JSAtom atom,
                                JSValue *pv);
static JSValue qp_ceval_load_name(JSContext *ctx, JSValueConst func, JSValueConst locals,
                                  JSAtom atom);
static int qp_ceval_store_name(JSContext *ctx, JSValueConst locals, JSAtom atom,
                               JSValueConst v);
static int qp_ceval_delete_name(JSContext *ctx, JSValueConst locals, JSAtom atom);
static JSValue qp_ceval_call_ex(JSContext *ctx, JSValueConst callable, JSValueConst self,
                                JSValueConst args, JSValueConst kwargs);
static int qp_ceval_list_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable);
static int qp_ceval_dict_update(JSContext *ctx, JSValueConst dict, JSValueConst update);
static JSValue qp_ceval_with_except_start(JSContext *ctx, JSValueConst exit_func,
                                          JSValueConst self, JSValueConst exc);
static JSValue qp_ceval_load_build_class(JSContext *ctx, JSValueConst func);
static int qp_ceval_setup_annotations(JSContext *ctx, JSValueConst locals);
static JSValue qp_ceval_import_name(JSContext *ctx, JSValueConst func,
                                    struct JSFunctionBytecode *b, JSValue *arg_buf,
                                    JSAtom name, JSValueConst level,
                                    JSValueConst fromlist, int mode);
static JSValue qp_ceval_intrinsic1(JSContext *ctx, struct JSStackFrame *sf,
                                   struct JSFunctionBytecode *b, JSValue *arg_buf,
                                   int id, JSValueConst v);
static JSValue qp_ceval_intrinsic2(JSContext *ctx, int id, JSValueConst a, JSValueConst b);

/* python/unicodeobject.c (unit d): releases QPState.str_cache */
static void qp_str_state_free(JSContext *ctx);
/* M3 (python/ modules state, signals, weakrefs; owner M3Misc) */
static void qp_modstate_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func);
static void qp_modstate_free(JSContext *ctx);
static void qp_weakref_object_freed(JSRuntime *rt, struct JSObject *p);
static int qp_signal_check(JSContext *ctx);
/* python/getargs.c (unit k): _Py_CalculateSuggestions over C strings */
static int qp_suggest_keyword(const char *name, const char *const *cands, int ncands);

#endif /* QP_CEVAL_H */
