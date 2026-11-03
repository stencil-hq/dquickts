/*
 * _testinternalcapi: the part of CPython's Modules/_testinternalcapi.c that
 * is meaningful for this interpreter and that Lib/test imports at module
 * level (test_class: has_inline_values, test_with:
 * SelfInterruptingContextManager) or uses outside @cpython_only tests
 * (SPECIALIZATION_THRESHOLD / SPECIALIZATION_COOLDOWN, get_recursion_depth,
 * get_tracked_heap_size, EncodeLocaleEx / DecodeLocaleEx), the constants
 * test_capi imports, and (CONFIG_PYTHON_CAPI) the functions that exercise C
 * API entry points (python/capi_testinternal.c).
 * Everything else of the CPython module inspects CPython internals
 * (specializer, JIT, frames, subinterpreters) and is absent.
 */

#ifdef CONFIG_PYTHON_CAPI
/* entries of qp_capi_tic_defs (python/capi_testinternal.c) */
enum {
    QP_TIC_CAPI_PENDING_THREADFUNC,
    QP_TIC_CAPI_SIMPLE_PENDING_CALL,
    QP_TIC_CAPI_GET_CONFIGS,
    QP_TIC_CAPI_IS_STATIC_IMMORTAL,
    QP_TIC_CAPI_PYERR_SETKEYERROR,
    QP_TIC_CAPI_TRANSFORM_DECIMAL,
    QP_TIC_CAPI_PY_CR_SUM,
    QP_TIC_CAPI_PY_CR_DIFF,
    QP_TIC_CAPI_PY_RC_DIFF,
    QP_TIC_CAPI_PY_CR_PROD,
    QP_TIC_CAPI_PY_CR_QUOT,
    QP_TIC_CAPI_PY_RC_QUOT,
    QP_TIC_CAPI_SET_UPDATE,
    QP_TIC_CAPI_SET_NEXT_ENTRY,
    QP_TIC_CAPI_MODULE_GET_GC_HOOKS,
    QP_TIC_CAPI__COUNT
};
static JSValue qp_capi_tic_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic);
#define QP_TIC_CAPI(name, idx) \
    { name, qp_capi_tic_call, QP_METH_FASTCALL | QP_METH_KEYWORDS, QP_TIC_CAPI_##idx, NULL }
#endif

/* has_inline_values(obj, /): objects never store their attributes in
   CPython's inline values layout. */
static JSValue qp_tic_has_inline_values(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(FALSE);
}

/* get_recursion_depth(): Python frames in use, the calling one included */
static JSValue qp_tic_get_recursion_depth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    return JS_NewInt32(ctx, QP_STATE(ctx)->recursion_depth);
}

/* get_tracked_heap_size(): the number of objects the collector tracks
   (CPython gc.heap_size; gc.get_objects() lists the same objects) */
static JSValue qp_tic_get_tracked_heap_size(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                            int nargs, JSValueConst kwnames, int magic)
{
    return JS_NewInt64(ctx, qp_gc_count_tracked(ctx->rt));
}

/* ---- EncodeLocaleEx / DecodeLocaleEx ------------------------------------
   _Py_EncodeLocaleEx / _Py_DecodeLocaleEx with the locale encoding forced
   to UTF-8 (_Py_FORCE_UTF8_LOCALE on macOS, UTF-8 mode elsewhere), i.e.
   _Py_EncodeUTF8Ex / _Py_DecodeUTF8Ex: only the strict, surrogateescape and
   surrogatepass handlers; an encoding error is a RuntimeError with the
   position.  The result is a C string (cut at the first NUL). */

static int qp_tic_locale_args(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                              BOOL decode, const char **errors)
{
    const char *e;

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return -1;
    if (decode ? !qp_bytes_check(ctx, args[0]) : !qp_str_check(ctx, args[0])) {
        qp_raise_type_error(ctx, "%s() argument 1 must be %s, not %T", fname,
                            decode ? "bytes" : "str", args[0]);
        return -1;
    }
    if (nargs > 1) {
        int cur;
        if (qp_arg_int(ctx, args[1], &cur) < 0)
            return -1;
    }
    *errors = "strict";
    if (nargs > 2) {
        if (!qp_str_check(ctx, args[2])) {
            qp_raise_type_error(ctx, "%s() argument 3 must be str, not %T", fname, args[2]);
            return -1;
        }
        e = qp_str_utf8(ctx, args[2], NULL);
        if (!e)
            return -1;
        if (!strcmp(e, "strict"))
            *errors = "strict";
        else if (!strcmp(e, "surrogateescape"))
            *errors = "surrogateescape";
        else if (!strcmp(e, "surrogatepass"))
            *errors = "surrogatepass";
        else
            *errors = NULL;
        JS_FreeCString(ctx, e);
        if (!*errors) {
            qp_raise_value_error(ctx, "unsupported error handler");
            return -1;
        }
    }
    return 0;
}

/* A pending Unicode{En,De}codeError becomes RuntimeError("<what> error:
   pos=<start>, reason=<what>ing error"); anything else stays pending. */
static JSValue qp_tic_locale_error(JSContext *ctx, BOOL decode)
{
    JSValue exc = JS_GetException(ctx), start;
    QPType *t = decode ? QP_EXC(ctx, UnicodeDecodeError) : QP_EXC(ctx, UnicodeEncodeError);
    JSAtom a;
    Py_ssize_t pos;

    if (!qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(t)))
        return JS_Throw(ctx, exc);
    a = JS_NewAtom(ctx, "start");
    start = a == JS_ATOM_NULL ? JS_EXCEPTION : qp_getattr(ctx, exc, a);
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, exc);
    if (JS_IsException(start))
        return start;
    if (qp_arg_ssize(ctx, start, &pos) < 0) {
        JS_FreeValue(ctx, start);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, start);
    return qp_raise_runtime_error(ctx, "%s error: pos=%zd, reason=%s",
                                  decode ? "decode" : "encode", pos,
                                  decode ? "decoding error" : "encoding error");
}

/* EncodeLocaleEx(text, current_locale=0, errors='strict', /) */
static JSValue qp_tic_encode_locale_ex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    const char *errors;
    const uint8_t *data;
    Py_ssize_t len;
    JSValue res, out;

    if (qp_tic_locale_args(ctx, "EncodeLocaleEx", args, nargs, FALSE, &errors) < 0)
        return JS_EXCEPTION;
    res = qp_str_encode(ctx, args[0], "utf-8", errors);
    if (JS_IsException(res))
        return qp_tic_locale_error(ctx, FALSE);
    data = qp_bytes_data(res, &len);
    out = qp_bytes_new(ctx, data, strnlen((const char *)data, len));
    JS_FreeValue(ctx, res);
    return out;
}

/* DecodeLocaleEx(encoded, current_locale=0, errors='strict', /) */
static JSValue qp_tic_decode_locale_ex(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    const char *errors;
    const uint8_t *data;
    Py_ssize_t len;
    JSValue res;

    if (qp_tic_locale_args(ctx, "DecodeLocaleEx", args, nargs, TRUE, &errors) < 0)
        return JS_EXCEPTION;
    data = qp_bytes_data(args[0], &len);
    if (memchr(data, 0, len))
        return qp_raise_value_error(ctx, "embedded null byte");
    res = qp_cd_decode_utf8(ctx, data, len, errors, NULL);
    if (JS_IsException(res))
        return qp_tic_locale_error(ctx, TRUE);
    return res;
}

/* ---- SelfInterruptingContextManager -------------------------------------
   slot 0: `within` flag (JS bool).  __enter__ sets it and schedules a
   KeyboardInterrupt as an asynchronous exception, raised at the next
   interpreter poll; __exit__ clears it. */

static JSValue qp_tic_sicm_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    JSValue obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_OBJECT);

    if (!JS_IsException(obj))
        qp_instance_slots(obj)[0] = JS_FALSE;
    return obj;
}

static JSValue qp_tic_sicm_enter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPModState *ms = qp_modstate(ctx);

    if (!ms)
        return JS_EXCEPTION;
    qp_instance_slots(self)[0] = JS_TRUE;
    if (!QP_IS_NULL(ms->async_exc))
        JS_FreeValue(ctx, ms->async_exc);
    ms->async_exc = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, KeyboardInterrupt)));
    ctx->interrupt_counter = 1;
    return JS_DupValue(ctx, self);
}

static JSValue qp_tic_sicm_within(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, qp_instance_slots(self)[0]);
}

static JSValue qp_tic_sicm_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    qp_instance_slots(self)[0] = JS_FALSE;
    return JS_UNDEFINED;
}

#ifdef CONFIG_PYTHON_CAPI
/* tuple_from_pair(first, second, /) / tuple_from_pair_steal(first,
   second, /): _PyTuple_FromPair / _PyTuple_FromPairSteal (capi_tuple.c) */
static JSValue qp_capi_tic_tuple_from_pair(JSContext *ctx, JSValueConst a, JSValueConst b,
                                           BOOL steal);

static JSValue qp_tic_tuple_from_pair(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    if (nargs != 2)
        return qp_raise_type_error(ctx, "function takes exactly 2 arguments (%d given)", nargs);
    return qp_capi_tic_tuple_from_pair(ctx, args[0], args[1], magic);
}

/* check_pyobject_{null,uninitialized,forbidden_bytes,freed}_is_freed():
   _PyObject_IsFreed() of such objects (PYTHONMALLOC=debug hooks,
   capi_mirror.c) */
static JSValue qp_capi_tic_check_pyobject_is_freed(JSContext *ctx, int which);

static JSValue qp_tic_check_pyobject_is_freed(JSContext *ctx, JSValueConst self,
                                              JSValueConst *args, int nargs,
                                              JSValueConst kwnames, int magic)
{
    return qp_capi_tic_check_pyobject_is_freed(ctx, magic);
}
#endif

static const QPMethodDef qp_tic_sicm_methods[] = {
    { "__enter__", qp_tic_sicm_enter, QP_METH_NOARGS, 0, NULL },
    { "within", qp_tic_sicm_within, QP_METH_NOARGS, 0, NULL },
    { "__exit__", qp_tic_sicm_exit, QP_METH_FASTCALL, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_tic_sicm_spec = {
    .name = "_testinternalcapi.SelfInterruptingContextManager",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_new = qp_tic_sicm_new,
    },
    .methods = qp_tic_sicm_methods,
};

static const QPMethodDef qp_tic_methods[] = {
    { "has_inline_values", qp_tic_has_inline_values, QP_METH_O, 0, NULL },
    { "get_recursion_depth", qp_tic_get_recursion_depth, QP_METH_NOARGS, 0, NULL },
    { "get_tracked_heap_size", qp_tic_get_tracked_heap_size, QP_METH_NOARGS, 0, NULL },
    { "EncodeLocaleEx", qp_tic_encode_locale_ex, QP_METH_FASTCALL, 0, NULL },
    { "DecodeLocaleEx", qp_tic_decode_locale_ex, QP_METH_FASTCALL, 0, NULL },
#ifdef CONFIG_PYTHON_CAPI
    { "tuple_from_pair", qp_tic_tuple_from_pair, QP_METH_FASTCALL, 0, NULL },
    { "tuple_from_pair_steal", qp_tic_tuple_from_pair, QP_METH_FASTCALL, 1, NULL },
    { "check_pyobject_null_is_freed", qp_tic_check_pyobject_is_freed, QP_METH_NOARGS, 0, NULL },
    { "check_pyobject_uninitialized_is_freed", qp_tic_check_pyobject_is_freed,
      QP_METH_NOARGS, 1, NULL },
    { "check_pyobject_forbidden_bytes_is_freed", qp_tic_check_pyobject_is_freed,
      QP_METH_NOARGS, 2, NULL },
    { "check_pyobject_freed_is_freed", qp_tic_check_pyobject_is_freed, QP_METH_NOARGS, 3, NULL },
    QP_TIC_CAPI("pending_threadfunc", PENDING_THREADFUNC),
    QP_TIC_CAPI("simple_pending_call", SIMPLE_PENDING_CALL),
    QP_TIC_CAPI("get_configs", GET_CONFIGS),
    QP_TIC_CAPI("is_static_immortal", IS_STATIC_IMMORTAL),
    QP_TIC_CAPI("_pyerr_setkeyerror", PYERR_SETKEYERROR),
    QP_TIC_CAPI("_PyUnicode_TransformDecimalAndSpaceToASCII", TRANSFORM_DECIMAL),
    QP_TIC_CAPI("_py_cr_sum", PY_CR_SUM),
    QP_TIC_CAPI("_py_cr_diff", PY_CR_DIFF),
    QP_TIC_CAPI("_py_rc_diff", PY_RC_DIFF),
    QP_TIC_CAPI("_py_cr_prod", PY_CR_PROD),
    QP_TIC_CAPI("_py_cr_quot", PY_CR_QUOT),
    QP_TIC_CAPI("_py_rc_quot", PY_RC_QUOT),
    QP_TIC_CAPI("set_update", SET_UPDATE),
    QP_TIC_CAPI("set_next_entry", SET_NEXT_ENTRY),
    QP_TIC_CAPI("module_get_gc_hooks", MODULE_GET_GC_HOOKS),
#endif
    { NULL }
};

static int qp_tic_add(JSContext *ctx, JSValueConst mod, const char *name, JSValue v)
{
    JSAtom a;
    int r;

    if (JS_IsException(v))
        return -1;
    a = JS_NewAtom(ctx, name);
    r = a == JS_ATOM_NULL ? -1 : qp_setattr(ctx, mod, a, v);
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return r;
}

static int qp_tic_exec(JSContext *ctx, JSValueConst mod)
{
    /* ADAPTIVE_WARMUP_VALUE + 1, ADAPTIVE_COOLDOWN_VALUE + 1 */
    if (qp_tic_add(ctx, mod, "SPECIALIZATION_THRESHOLD", JS_NewInt32(ctx, 2)) < 0 ||
        qp_tic_add(ctx, mod, "SPECIALIZATION_COOLDOWN", JS_NewInt32(ctx, 53)) < 0 ||
        /* CPython's default tier-2 thresholds (no JIT here: test_opt imports
           them), SHARED_KEYS_MAX_SIZE, the small int cache bound
           (python/capi_mirror.c statics) and the largest code point */
        qp_tic_add(ctx, mod, "TIER2_THRESHOLD", JS_NewInt32(ctx, 4002)) < 0 ||
        qp_tic_add(ctx, mod, "TIER2_RESUME_THRESHOLD", JS_NewInt32(ctx, 8192)) < 0 ||
        qp_tic_add(ctx, mod, "SHARED_KEYS_MAX_SIZE", JS_NewInt32(ctx, 30)) < 0 ||
        qp_tic_add(ctx, mod, "_PY_NSMALLPOSINTS", JS_NewInt32(ctx, 1025)) < 0 ||
        qp_tic_add(ctx, mod, "_Py_MAX_UNICODE", JS_NewInt32(ctx, 0x10ffff)) < 0 ||
        qp_tic_add(ctx, mod, "SelfInterruptingContextManager",
                   qp_type_from_spec(ctx, &qp_tic_sicm_spec)) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef__testinternalcapi = {
    "_testinternalcapi",
    NULL,
    qp_tic_methods,
    qp_tic_exec,
};
