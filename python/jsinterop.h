/*
 * dquickts Python, milestone M4: JS <-> Python interop hooks (plan D3).
 *
 * Included by quickjs.c right after python/ceval.h (all engine structs are
 * defined at that point); the definitions are in python/jsinterop.c, which
 * python/quickpy.c includes after pythonrun.c.  Everything here is used by
 * the delimited `M4 jsinterop` hooks in quickjs.c.
 */
#ifndef QP_JSINTEROP_H
#define QP_JSINTEROP_H

/* JS_CallInternal flag: the call already went through the Python -> JS
   boundary conversion (qp_jsi_call_js_from_py); bits 0-4 are quickjs.c's,
   5-6 are JS_CALL_FLAG_PY_KW / JS_CALL_FLAG_PY_ARGS_OFFSET (object.h). */
#define JS_CALL_FLAG_PY_BOUNDARY (1 << 7)

struct QPJsState;

/* TRUE when the innermost running frame is Python code, i.e. a call made
   now crosses from Python into its callee.  C functions called by Python
   (builtins) do not push a frame, so their nested calls count as made by
   Python too, like CPython's PyObject_Call from a builtin. */
static inline BOOL qp_jsi_caller_is_py(JSRuntime *rt)
{
    JSStackFrame *sf = rt->current_stack_frame;
    JSObject *p;
    if (!sf || JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    return p->class_id == JS_CLASS_BYTECODE_FUNCTION && p->u.func.function_bytecode->is_py;
}

/* Python -> JS call boundary (D3 numbers): ints outside int32 but within
   +-2^53 are passed as Number; an integral finite float64 result becomes a
   Python int.  Calls JS_CallInternal again with JS_CALL_FLAG_PY_BOUNDARY. */
static JSValue qp_jsi_call_js_from_py(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj, JSValueConst new_target,
                                      int argc, JSValueConst *argv, int flags);
/* JS_IsFunction() for Python class ids: the object is callable from
   Python (its type has tp_call, or it is a type). */
static BOOL qp_jsi_py_callable(JSObject *p);
/* Module loader for specifiers ending in ".py" (called by
   js_host_resolve_imported_module with the normalized name): executes the
   Python file and returns a synthetic C module, NULL with an exception. */
static JSModuleDef *qp_jsi_load_py_module(JSContext *ctx, const char *filename);
/* JSContext.py_js: mark / free (free runs before qp_state_free). */
static void qp_jsi_mark(JSRuntime *rt, struct QPJsState *st, JS_MarkFunc *mark_func);
static void qp_jsi_free(JSContext *ctx);
/* Spec of js.JsObject (readied by typeobject.c qp_init_typeobject). */
static const QPTypeSpec *qp_jsobject_type_spec(void);

#endif /* QP_JSINTEROP_H */
