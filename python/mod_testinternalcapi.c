/*
 * _testinternalcapi: the part of CPython's Modules/_testinternalcapi.c that
 * is meaningful for this interpreter and that Lib/test imports at module
 * level (test_class: has_inline_values, test_with:
 * SelfInterruptingContextManager) or uses outside @cpython_only tests
 * (SPECIALIZATION_THRESHOLD / SPECIALIZATION_COOLDOWN, get_recursion_depth).
 * Everything else of the CPython module inspects CPython internals and is
 * absent.
 */

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
