/*
 * dquickts Python (M2, unit j): generators, coroutines, async generators.
 *
 * Derived from CPython Objects/genobject.c, Python/ceval.c
 * (_PyEval_GetAwaitable, _PyEval_GetANext), Python/bytecodes.c (GET_AITER),
 * Python/intrinsics.c (stopiteration_error) and Python/_warnings.c
 * (unawaited coroutine warnings) (PSF-2.0).
 *
 * A generator owns a QuickJS heap frame (JSAsyncFunctionState created by
 * async_func_init, never started until the first send: lazy start) and is
 * resumed with async_func_resume.  The Python bytecode suspends with
 * FUNC_RET_YIELD (plain yield) or FUNC_RET_YIELD_STAR (yield inside a
 * yield-from/await send loop: the delegate is cur_sp[-4]); the yielded
 * value is cur_sp[-1] and the sent value replaces it on resume.  Return is
 * OP_return_async with the value in cur_sp[-1] (async_func_resume hands it
 * back and frees the frame).  Throwing sets throw_flag with the exception
 * pending, so it is raised at the suspension point.
 */

/* frame states (CPython FRAME_*) */
enum {
    QP_GEN_CREATED,
    QP_GEN_SUSPENDED,
    QP_GEN_SUSPENDED_YIELD_FROM,
    QP_GEN_EXECUTING,
    QP_GEN_CLEARED,             /* completed or cleared */
};
#define QP_GEN_FINISHED(st)  ((st) == QP_GEN_CLEARED)
#define QP_GEN_SUSPENDED_ANY(st) ((st) == QP_GEN_SUSPENDED || (st) == QP_GEN_SUSPENDED_YIELD_FROM)

enum { QP_GENK_GEN, QP_GENK_CORO, QP_GENK_AGEN };

typedef struct QPGen {
    JSAsyncFunctionState *s;    /* heap frame (counted), NULL once finished */
    JSValue func;               /* the function that created it (gi_code) */
    JSValue name, qualname;     /* __name__, __qualname__ */
    QPExcStackItem exc_state;   /* gi_exc_state */
    JSValue ag_finalizer;       /* async generators: sys asyncgen finalizer hook; coroutines:
                                   cr_origin (CPython cr_origin_or_finalizer); QP_NULL: none */
    uint8_t kind;               /* QP_GENK_* */
    uint8_t state;              /* QP_GEN_* */
    uint8_t started;            /* first resume done (a sent value has a slot) */
    uint8_t ag_closed, ag_hooks_inited, ag_running_async;
} QPGen;

/* PY_GEN_AUX payloads */
enum { QP_AUX_CORO_WRAPPER, QP_AUX_ASEND, QP_AUX_ATHROW, QP_AUX_WRAPPED };
enum { QP_AWAITABLE_STATE_INIT, QP_AWAITABLE_STATE_ITER, QP_AWAITABLE_STATE_CLOSED };

typedef struct QPGenAux {
    uint8_t kind;               /* QP_AUX_* */
    uint8_t state;              /* QP_AWAITABLE_STATE_* (asend/athrow) */
    JSValue gen;                /* coroutine (wrapper) / async generator (asend, athrow) */
    JSValue v1;                 /* asend: sendval; athrow: typ (QP_NULL = aclose); wrapped: value */
    JSValue v2, v3;             /* athrow: val, tb (QP_NULL if absent) */
} QPGenAux;

static const char QP_NON_INIT_CORO_MSG[] =
    "can't send non-None value to a just-started coroutine";
static const char QP_ASYNC_GEN_IGNORED_EXIT_MSG[] =
    "async generator ignored GeneratorExit";

static JSValue qp__gen_close(JSContext *ctx, JSValueConst self);
static JSValue qp__gen_throw(JSContext *ctx, JSValueConst self, int close_on_genexit,
                             JSValueConst typ, JSValueConst val, JSValueConst tb);

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static inline int qp__class_id(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return -1;
    return JS_VALUE_GET_OBJ(v)->class_id;
}

static inline BOOL qp__is_gen_class(JSValueConst v)
{
    int id = qp__class_id(v);
    return id == JS_CLASS_PY_GENERATOR || id == JS_CLASS_PY_COROUTINE ||
        id == JS_CLASS_PY_ASYNC_GEN;
}

static inline QPGen *qp__gen(JSValueConst v)
{
    return (QPGen *)JS_VALUE_GET_OBJ(v)->u.opaque;
}

static inline QPGenAux *qp__aux(JSValueConst v)
{
    return (QPGenAux *)JS_VALUE_GET_OBJ(v)->u.opaque;
}

static BOOL qp_gen_check(JSValueConst v)
{
    return qp__class_id(v) == JS_CLASS_PY_GENERATOR;
}

static BOOL qp_coro_check(JSValueConst v)
{
    return qp__class_id(v) == JS_CLASS_PY_COROUTINE;
}

static inline BOOL qp__is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static BOOL qp__is_wrapped_value(JSValueConst v)
{
    return qp__class_id(v) == JS_CLASS_PY_GEN_AUX && qp__aux(v)->kind == QP_AUX_WRAPPED;
}

static uint32_t qp__gen_co_flags(QPGen *g)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(g->func)->u.func.function_bytecode;
    QPCodeInfo *ci = qp_code_info(b);
    QPFuncData *d = qp_function_data(g->func);
    uint32_t flags = ci ? ci->co_flags : 0;
    /* the function's __code__ may carry CO_ITERABLE_COROUTINE (types.coroutine) */
    if (d && !QP_IS_NULL(d->code_obj))
        flags |= qp_code_extra_flags(d->code_obj);
    return flags;
}

/* gen_is_coroutine: generator whose code has CO_ITERABLE_COROUTINE
   (types.coroutine). */
static BOOL qp__gen_is_coroutine(JSValueConst o)
{
    if (qp_gen_check(o))
        return (qp__gen_co_flags(qp__gen(o)) & CO_ITERABLE_COROUTINE) != 0;
    return FALSE;
}

/* tp_name of a type as a str (builtins: spec name, heap types: __name__) */
static JSValue qp__tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* PyErr_SetString(type, msg) */
static JSValue qp__raise_str(JSContext *ctx, QPType *t, const char *msg)
{
    return qp_raise(ctx, t, "%s", msg);
}

/* PyErr_SetNone(type) */
static JSValue qp__raise_none(JSContext *ctx, QPType *t)
{
    return qp_raise_value(ctx, t, JS_UNDEFINED);
}

/* _PyGen_SetStopIterationValue: StopIteration(value) built with one
   argument even when value is a tuple. */
static JSValue qp__set_stop_iteration_value(JSContext *ctx, JSValueConst value)
{
    JSValue exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 1, &value);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* Discard the pending exception. */
static void qp__clear_exc(JSContext *ctx)
{
    JS_FreeValue(ctx, JS_GetException(ctx));
}

/* _PyGen_FetchStopIterationValue on a pending exception: 0 and *pvalue
   (new ref) when it is StopIteration, else -1 (left pending). */
static int qp__fetch_stop_iteration_value(JSContext *ctx, JSValue *pvalue)
{
    JSValue exc;
    if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration)))
        return -1;
    exc = qp_exc_fetch(ctx);
    *pvalue = qp_exc_stopiteration_value(ctx, exc);
    JS_FreeValue(ctx, exc);
    return 0;
}

static const char *qp__gen_kind_word(QPGen *g)
{
    switch (g->kind) {
    case QP_GENK_CORO: return "coroutine";
    case QP_GENK_AGEN: return "async generator";
    default: return "generator";
    }
}

/* Release the frame and the exception state (gen_clear_frame). */
static void qp__gen_clear(JSContext *ctx, QPGen *g)
{
    g->state = QP_GEN_CLEARED;
    if (g->s) {
        JSAsyncFunctionState *s = g->s;
        g->s = NULL;
        s->py_gen = NULL;
        async_func_free(ctx->rt, s);
    }
    if (!JS_IsUndefined(g->exc_state.exc_value)) {
        JSValue v = g->exc_state.exc_value;
        g->exc_state.exc_value = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static JSValue qp__gen_raise_already_executing(JSContext *ctx, QPGen *g)
{
    return qp_raise_value_error(ctx, "%s already executing", qp__gen_kind_word(g));
}

/* _PyErr_ChainStackItem: the pending exception gets the generator's
   handled exception as __context__ (exc_info is the generator's item). */
static void qp__chain_stack_item(JSContext *ctx)
{
    QPExcStackItem *ei = QP_STATE(ctx)->exc_info;
    JSValue exc, o, ctxv;
    JSAtom a_ctx = QP_ATOM(ctx, __context__);

    if (qp__is_none(ei->exc_value) || QP_IS_NULL(ei->exc_value))
        return;
    exc = qp_exc_fetch(ctx);
    if (!qp_is(exc, ei->exc_value)) {
        /* avoid reference cycles through the context chain
           (_PyErr_SetObject) */
        o = JS_DupValue(ctx, ei->exc_value);
        for (;;) {
            ctxv = qp_getattr(ctx, o, a_ctx);
            if (JS_IsException(ctxv)) {
                qp__clear_exc(ctx);
                break;
            }
            if (qp__is_none(ctxv)) {
                JS_FreeValue(ctx, ctxv);
                break;
            }
            if (qp_is(ctxv, exc)) {
                JS_FreeValue(ctx, ctxv);
                if (qp_setattr(ctx, o, a_ctx, JS_UNDEFINED) < 0)
                    qp__clear_exc(ctx);
                break;
            }
            JS_FreeValue(ctx, o);
            o = ctxv;
        }
        JS_FreeValue(ctx, o);
        if (qp_setattr(ctx, exc, a_ctx, ei->exc_value) < 0)
            qp__clear_exc(ctx);
    }
    JS_Throw(ctx, exc);
}

/* ------------------------------------------------------------------------ */
/* core: send / throw / close                                               */
/* ------------------------------------------------------------------------ */

/* gen_send_ex2: run the frame (state already EXECUTING).  arg is the sent
   value (QP_NULL: called by __next__); exc: an exception is pending and
   is raised at the suspension point.  QP_SEND_NEXT: *presult yielded;
   QP_SEND_RETURN: *presult returned value, QP_NULL for a plain None
   return of a generator/coroutine driven by __next__; QP_SEND_ERROR. */
static int qp__gen_send_ex2(JSContext *ctx, QPGen *g, JSValueConst arg,
                            JSValue *presult, int exc)
{
    QPState *st = QP_STATE(ctx);
    JSAsyncFunctionState *s = g->s;
    JSValue ret, *sp;

    *presult = QP_NULL;
    if (!g->started && exc) {
        /* thrown into a just-created frame: no handler is active at its
           first instruction, the exception propagates and the frame is
           finished */
        qp__gen_clear(ctx, g);
        return QP_SEND_ERROR;
    }
    if (g->started && !exc) {
        sp = s->frame.cur_sp;
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = QP_IS_NULL(arg) ? JS_UNDEFINED : JS_DupValue(ctx, arg);
    }
    g->started = 1;

    g->exc_state.previous = st->exc_info;
    st->exc_info = &g->exc_state;
    if (exc) {
        qp__chain_stack_item(ctx);
        s->throw_flag = TRUE;
    }
    js_rc(s)->ref_count++;   /* keep s alive across the resume */
    ret = async_func_resume(ctx, s);
    s->throw_flag = FALSE;
    st->exc_info = g->exc_state.previous;
    g->exc_state.previous = NULL;

    if (!s->is_completed) {
        sp = s->frame.cur_sp;
        *presult = sp[-1];
        sp[-1] = JS_UNDEFINED;
        g->state = (JS_VALUE_GET_INT(ret) == FUNC_RET_YIELD_STAR) ?
            QP_GEN_SUSPENDED_YIELD_FROM : QP_GEN_SUSPENDED;
        async_func_free(ctx->rt, s);
        return QP_SEND_NEXT;
    }
    async_func_free(ctx->rt, s);
    qp__gen_clear(ctx, g);
    if (JS_IsException(ret))
        return QP_SEND_ERROR;
    if (JS_IsUndefined(ret) && g->kind != QP_GENK_AGEN && QP_IS_NULL(arg)) {
        /* return None from __next__: plain exhaustion */
        return QP_SEND_RETURN;
    }
    *presult = ret;
    return QP_SEND_RETURN;
}

/* gen_send_ex: state checks, then gen_send_ex2.  A finished generator
   driven by __next__ (arg QP_NULL) gives QP_SEND_RETURN with QP_NULL. */
static int qp__gen_send_ex(JSContext *ctx, QPGen *g, JSValueConst arg, JSValue *presult)
{
    *presult = QP_NULL;
    if (g->state == QP_GEN_CREATED && !QP_IS_NULL(arg) && !qp__is_none(arg)) {
        qp_raise_type_error(ctx, "can't send non-None value to a just-started %s",
                            qp__gen_kind_word(g));
        return QP_SEND_ERROR;
    }
    if (g->state == QP_GEN_EXECUTING) {
        qp__gen_raise_already_executing(ctx, g);
        return QP_SEND_ERROR;
    }
    if (QP_GEN_FINISHED(g->state)) {
        if (g->kind == QP_GENK_CORO) {
            /* exhausted coroutine */
            qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                          "cannot reuse already awaited coroutine");
            return QP_SEND_ERROR;
        }
        if (!QP_IS_NULL(arg))
            *presult = JS_UNDEFINED;
        return QP_SEND_RETURN;
    }
    /* RESUME of a frame starting or resumed after a plain yield checks the
       eval breaker; resumed after a yield from / await, it does not and
       its frame goes back to the delegate (JUMP_BACKWARD_NO_INTERRUPT),
       whose resumption checks: a pending signal's exception is raised
       inside the innermost generator of a yield-from chain (bpo-30039) */
    if (g->state == QP_GEN_SUSPENDED_YIELD_FROM) {
        int r;
        QPState *st = QP_STATE(ctx);
        g->state = QP_GEN_EXECUTING;
        st->signals_deferred = 1;
        r = qp__gen_send_ex2(ctx, g, arg, presult, 0);
        st->signals_deferred = 0;
        return r;
    }
    QP_STATE(ctx)->signals_deferred = 0;
    g->state = QP_GEN_EXECUTING;
    if (unlikely(ctx->interrupt_counter <= 1) && js_poll_interrupts(ctx))
        return qp__gen_send_ex2(ctx, g, arg, presult, 1);
    return qp__gen_send_ex2(ctx, g, arg, presult, 0);
}

/* gen_set_stop_iteration (consumes result) */
static JSValue qp__gen_set_stop_iteration(JSContext *ctx, QPGen *g, JSValue result)
{
    JSValue r;
    if (g->kind == QP_GENK_AGEN)
        r = qp__raise_none(ctx, QP_EXC(ctx, StopAsyncIteration));
    else if (JS_IsUndefined(result))
        r = qp__raise_none(ctx, QP_EXC(ctx, StopIteration));
    else
        r = qp__set_stop_iteration_value(ctx, result);
    JS_FreeValue(ctx, result);
    return r;
}

/* gen_send (arg may be QP_NULL: then a silently exhausted generator gives
   QP_NULL without exception). */
static JSValue qp__gen_send(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    QPGen *g = qp__gen(self);
    JSValue res;
    int r = qp__gen_send_ex(ctx, g, arg, &res);
    if (r == QP_SEND_RETURN) {
        if (QP_IS_NULL(res))
            return QP_NULL;
        return qp__gen_set_stop_iteration(ctx, g, res);
    }
    if (r == QP_SEND_ERROR)
        return JS_EXCEPTION;
    return res;
}

/* PyGen_am_send */
static int qp__gen_am_send(JSContext *ctx, JSValueConst self, JSValueConst arg, JSValue *presult)
{
    int r = qp__gen_send_ex(ctx, qp__gen(self), arg, presult);
    if (r == QP_SEND_RETURN && QP_IS_NULL(*presult))
        *presult = JS_UNDEFINED;
    return r;
}

/* gen_iternext (generator and coroutine wrapper) */
static JSValue qp__gen_iternext(JSContext *ctx, JSValueConst self)
{
    QPGen *g = qp__gen(self);
    JSValue res;
    int r = qp__gen_send_ex(ctx, g, QP_NULL, &res);
    if (r == QP_SEND_RETURN) {
        if (!QP_IS_NULL(res)) {
            if (!JS_IsUndefined(res)) {
                JSValue e = qp__set_stop_iteration_value(ctx, res);
                JS_FreeValue(ctx, res);
                return e;
            }
            JS_FreeValue(ctx, res);
        }
        return QP_NULL;
    }
    if (r == QP_SEND_ERROR)
        return JS_EXCEPTION;
    return res;
}

/* gen_close_iter: close the delegate of a yield from. */
static int qp__gen_close_iter(JSContext *ctx, JSValueConst yf)
{
    JSValue retval = JS_UNDEFINED, meth;
    int id = qp__class_id(yf);

    if (id == JS_CLASS_PY_GENERATOR || id == JS_CLASS_PY_COROUTINE) {
        retval = qp__gen_close(ctx, yf);
        if (JS_IsException(retval))
            return -1;
    } else {
        int r = qp_getattr_opt(ctx, yf, QP_ATOM(ctx, close), &meth);
        if (r < 0) {
            qp_format_unraisable(ctx, "Exception ignored while closing generator %R", yf);
        } else if (r > 0) {
            retval = qp_call0(ctx, meth);
            JS_FreeValue(ctx, meth);
            if (JS_IsException(retval))
                return -1;
        }
    }
    JS_FreeValue(ctx, retval);
    return 0;
}

/* gen_close */
static JSValue qp__gen_close(JSContext *ctx, JSValueConst self)
{
    QPGen *g = qp__gen(self);
    JSValue retval, exc;
    int err = 0, prev, r;

    if (g->state == QP_GEN_CREATED) {
        qp__gen_clear(ctx, g);
        return JS_UNDEFINED;
    }
    if (QP_GEN_FINISHED(g->state))
        return JS_UNDEFINED;
    if (g->state == QP_GEN_EXECUTING)
        return qp__gen_raise_already_executing(ctx, g);

    prev = g->state;
    g->state = QP_GEN_EXECUTING;
    if (prev == QP_GEN_SUSPENDED_YIELD_FROM) {
        JSValue yf = JS_DupValue(ctx, g->s->frame.cur_sp[-4]);
        err = qp__gen_close_iter(ctx, yf);
        JS_FreeValue(ctx, yf);
    }
    if (err == 0) {
        exc = qp_exc_new(ctx, QP_EXC(ctx, GeneratorExit), 0, NULL);
        if (JS_IsException(exc)) {
            g->state = prev;
            return exc;
        }
        JS_Throw(ctx, exc);
    }
    r = qp__gen_send_ex2(ctx, g, JS_UNDEFINED, &retval, 1);
    if (r == QP_SEND_RETURN) {
        /* the generator returned a value while closing */
        return QP_IS_NULL(retval) ? JS_UNDEFINED : retval;
    }
    if (r == QP_SEND_NEXT) {
        const char *msg = "generator ignored GeneratorExit";
        if (g->kind == QP_GENK_CORO)
            msg = "coroutine ignored GeneratorExit";
        else if (g->kind == QP_GENK_AGEN)
            msg = QP_ASYNC_GEN_IGNORED_EXIT_MSG;
        JS_FreeValue(ctx, retval);
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), msg);
    }
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, GeneratorExit))) {
        qp__clear_exc(ctx);   /* ignore this error */
        return JS_UNDEFINED;
    }
    return JS_EXCEPTION;
}

/* gen_set_exception: make typ/val/tb as given to throw() the pending
   exception (QP_NULL for absent arguments): 0.  -1 when the arguments are
   invalid (that error is pending instead). */
static int qp__gen_set_exception(JSContext *ctx, JSValueConst typ, JSValueConst val,
                                 JSValueConst tb)
{
    JSValue exc;
    QPType *t;

    if (!QP_IS_NULL(tb) && qp__is_none(tb)) {
        tb = QP_NULL;
    } else if (!QP_IS_NULL(tb) && qp_type_of(ctx, tb) != QP_TYPE(ctx, traceback)) {
        qp_raise_type_error(ctx, "throw() third argument must be a traceback object");
        return -1;
    }

    if (qp_exc_type_check(typ)) {
        /* PyErr_NormalizeException */
        t = qp_type_from_value(typ);
        if (!QP_IS_NULL(val) && qp_typecheck(ctx, val, t)) {
            exc = JS_DupValue(ctx, val);
        } else {
            if (QP_IS_NULL(val) || qp__is_none(val)) {
                exc = qp_vectorcall(ctx, typ, NULL, 0, QP_NULL);
            } else if (qp__class_id(val) == JS_CLASS_PY_TUPLE) {
                exc = qp_vectorcall(ctx, typ, (JSValueConst *)QP_TUPLE_ITEMS(val),
                                    QP_TUPLE_SIZE(val), QP_NULL);
            } else {
                exc = qp_vectorcall(ctx, typ, &val, 1, QP_NULL);
            }
            if (JS_IsException(exc))
                return -1;
            if (!qp_exc_check(ctx, exc)) {
                qp_raise_type_error(ctx,
                    "calling %R should have returned an instance of BaseException, not %T",
                    typ, exc);
                JS_FreeValue(ctx, exc);
                return -1;
            }
        }
    } else if (qp_exc_check(ctx, typ)) {
        if (!QP_IS_NULL(val) && !qp__is_none(val)) {
            qp_raise_type_error(ctx, "instance exception may not have a separate value");
            return -1;
        }
        exc = JS_DupValue(ctx, typ);
    } else {
        JSValue name = qp__tp_name(ctx, qp_type_of(ctx, typ));
        if (JS_IsException(name))
            return -1;
        qp_raise_type_error(ctx, "exceptions must be classes or instances "
                            "deriving from BaseException, not %U", name);
        JS_FreeValue(ctx, name);
        return -1;
    }
    if (!QP_IS_NULL(tb)) {
        if (qp_exc_set_traceback(ctx, exc, tb) < 0) {
            JS_FreeValue(ctx, exc);
            return -1;
        }
    }
    JS_Throw(ctx, exc);
    return 0;
}

/* gen_throw_current_exception: resume with the pending exception. */
static JSValue qp__gen_throw_current(JSContext *ctx, QPGen *g)
{
    JSValue result;
    int r = qp__gen_send_ex2(ctx, g, JS_UNDEFINED, &result, 1);
    if (r == QP_SEND_RETURN)
        return qp__gen_set_stop_iteration(ctx, g, QP_IS_NULL(result) ? JS_UNDEFINED : result);
    if (r == QP_SEND_ERROR)
        return JS_EXCEPTION;
    return result;
}

/* _gen_throw */
static JSValue qp__gen_throw(JSContext *ctx, JSValueConst self, int close_on_genexit,
                             JSValueConst typ, JSValueConst val, JSValueConst tb)
{
    QPGen *g = qp__gen(self);
    int prev;

    if (g->state == QP_GEN_EXECUTING)
        return qp__gen_raise_already_executing(ctx, g);
    if (QP_GEN_FINISHED(g->state)) {
        if (g->kind == QP_GENK_CORO)
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                                 "cannot reuse already awaited coroutine");
        qp__gen_set_exception(ctx, typ, val, tb);
        return JS_EXCEPTION;
    }
    prev = g->state;
    g->state = QP_GEN_EXECUTING;

    if (prev == QP_GEN_SUSPENDED_YIELD_FROM) {
        JSValue yf = JS_DupValue(ctx, g->s->frame.cur_sp[-4]);
        JSValue ret;
        int id = qp__class_id(yf);
        if (close_on_genexit &&
            qp_exc_given_matches(ctx, typ, QP_TYPE_VALUE(QP_EXC(ctx, GeneratorExit)))) {
            /* close the delegate first (asynchronous generators must not:
               close_on_genexit == 0) */
            int err = qp__gen_close_iter(ctx, yf);
            JS_FreeValue(ctx, yf);
            if (err < 0)
                return qp__gen_throw_current(ctx, g);
            goto throw_here;
        }
        if (id == JS_CLASS_PY_GENERATOR || id == JS_CLASS_PY_COROUTINE) {
            ret = qp__gen_throw(ctx, yf, close_on_genexit, typ, val, tb);
        } else {
            JSValue meth;
            JSValueConst args[3];
            int nargs = 1, r;
            r = qp_getattr_opt(ctx, yf, QP_ATOM(ctx, throw), &meth);
            if (r < 0) {
                JS_FreeValue(ctx, yf);
                g->state = prev;
                return JS_EXCEPTION;
            }
            if (r == 0) {
                JS_FreeValue(ctx, yf);
                goto throw_here;
            }
            /* PyObject_CallFunctionObjArgs(meth, typ, val, tb, NULL) */
            args[0] = typ;
            if (!QP_IS_NULL(val)) {
                args[nargs++] = val;
                if (!QP_IS_NULL(tb))
                    args[nargs++] = tb;
            }
            ret = qp_vectorcall(ctx, meth, args, nargs, QP_NULL);
            JS_FreeValue(ctx, meth);
        }
        JS_FreeValue(ctx, yf);
        if (JS_IsException(ret))
            return qp__gen_throw_current(ctx, g);
        g->state = prev;
        return ret;
    }

throw_here:
    if (qp__gen_set_exception(ctx, typ, val, tb) < 0) {
        /* invalid arguments: the generator is left untouched */
        g->state = prev;
        return JS_EXCEPTION;
    }
    return qp__gen_throw_current(ctx, g);
}

/* Argument handling of throw(): returns JS_EXCEPTION on bad arguments
   (deprecation warning for the 3-argument form). */
static JSValue qp__gen_throw_args(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                  int close_on_genexit)
{
    if (qp_arg_check_positional(ctx, "throw", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (nargs > 1) {
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "the (type, exc, tb) signature of throw() is deprecated, "
                    "use the single-arg signature instead.") < 0)
            return JS_EXCEPTION;
    }
    return qp__gen_throw(ctx, self, close_on_genexit, args[0],
                         nargs > 1 ? args[1] : QP_NULL, nargs > 2 ? args[2] : QP_NULL);
}

/* ------------------------------------------------------------------------ */
/* Python-level methods and attributes of generator / coroutine / agen      */
/* ------------------------------------------------------------------------ */

static JSValue qp__m_gen_send(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_send(ctx, self, args[0]);
}

static JSValue qp__m_gen_throw(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_throw_args(ctx, self, args, nargs, 1);
}

static JSValue qp__m_gen_close(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_close(ctx, self);
}

static JSValue qp__m_gen_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPGen *g = qp__gen(self);
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(g->func)->u.func.function_bytecode;
    int64_t res = sizeof(QPGen) + sizeof(JSAsyncFunctionState) +
        sizeof(JSValue) * (int64_t)(b->arg_count + b->var_count + b->stack_size);
    return JS_NewInt64(ctx, res);
}

static JSValue qp__m_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static JSValue qp__g_name(JSContext *ctx, JSValueConst self, int magic)
{
    QPGen *g = qp__gen(self);
    return JS_DupValue(ctx, magic ? g->qualname : g->name);
}

static int qp__s_name(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPGen *g = qp__gen(self);
    JSValue *slot = magic ? &g->qualname : &g->name;
    JSValue old;
    /* not legal to delete it or to set it to anything other than a str */
    if (QP_IS_NULL(value) || !qp_str_check(ctx, value)) {
        qp_raise_type_error(ctx, magic ? "__qualname__ must be set to a string object"
                                       : "__name__ must be set to a string object");
        return -1;
    }
    old = *slot;
    *slot = JS_DupValue(ctx, value);
    JS_FreeValue(ctx, old);
    return 0;
}

/* gi_yieldfrom / cr_await / ag_await */
static JSValue qp__g_yieldfrom(JSContext *ctx, JSValueConst self, int magic)
{
    QPGen *g = qp__gen(self);
    if (g->state != QP_GEN_SUSPENDED_YIELD_FROM)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, g->s->frame.cur_sp[-4]);
}

static JSValue qp__g_running(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_bool_new(qp__gen(self)->state == QP_GEN_EXECUTING);
}

static JSValue qp__g_ag_running(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_bool_new(qp__gen(self)->ag_running_async);
}

static JSValue qp__g_suspended(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_bool_new(QP_GEN_SUSPENDED_ANY(qp__gen(self)->state));
}

/* gi_state / cr_state / ag_state (magic: 0 GEN_, 1 CORO_, 2 AGEN_) */
static JSValue qp__g_state(JSContext *ctx, JSValueConst self, int magic)
{
    static const char *const prefix[] = { "GEN_", "CORO_", "AGEN_" };
    const char *what;
    char buf[32];
    switch (qp__gen(self)->state) {
    case QP_GEN_CREATED: what = "CREATED"; break;
    case QP_GEN_EXECUTING: what = "RUNNING"; break;
    case QP_GEN_CLEARED: what = "CLOSED"; break;
    default: what = "SUSPENDED"; break;
    }
    snprintf(buf, sizeof(buf), "%s%s", prefix[magic], what);
    return qp_str_from_cstr(ctx, buf);
}

/* gi_frame / cr_frame / ag_frame: None once finished, else the frame
   object of the heap frame (the one sys._getframe() returns while it
   runs, CPython _PyFrame_GetFrameObject). */
static JSValue qp__g_frame(JSContext *ctx, JSValueConst self, int magic)
{
    QPGen *g = qp__gen(self);
    JSValue f;
    if (QP_GEN_FINISHED(g->state) || !g->s)
        return JS_UNDEFINED;
    f = qp_frame_of(ctx, &g->s->frame);
    return JS_IsException(f) ? f : JS_DupValue(ctx, f);
}

static JSValue qp__g_code(JSContext *ctx, JSValueConst self, int magic)
{
    /* g.gi_code is f.__code__ (cached facade) */
    return qp_getattr(ctx, qp__gen(self)->func, QP_ATOM(ctx, __code__));
}

static JSValue qp__g_origin(JSContext *ctx, JSValueConst self, int magic)
{
    QPGen *g = qp__gen(self);
    if (g->kind != QP_GENK_CORO || QP_IS_NULL(g->ag_finalizer))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, g->ag_finalizer);
}

/* cr_origin of a coroutine created now (CPython _PyCoro_ComputeOrigin):
   ((co_filename, f_lineno, co_name), ...) of the `depth` innermost Python
   frames, the creating frame first */
static JSValue qp__coro_origin(JSContext *ctx, int depth)
{
    JSStackFrame *sf;
    JSValue *items, r;
    int n = 0, i = 0;

    for (sf = ctx->rt->current_stack_frame; sf && n < depth; sf = sf->prev_frame) {
        if (qp_frame_py_bytecode(sf))
            n++;
    }
    items = js_mallocz(ctx, sizeof(JSValue) * (n ? n : 1));
    if (!items)
        return JS_EXCEPTION;
    for (sf = ctx->rt->current_stack_frame; sf && i < n; sf = sf->prev_frame) {
        JSFunctionBytecode *b = qp_frame_py_bytecode(sf);
        JSValue code, info[3];
        QPLoc loc;
        int line;

        if (!b)
            continue;
        code = qp_getattr(ctx, sf->cur_func, QP_ATOM(ctx, __code__));
        if (JS_IsException(code))
            goto fail;
        info[0] = qp_co_get_filename(ctx, code);
        info[2] = qp_co_get_name(ctx, code);
        JS_FreeValue(ctx, code);
        line = b->py_code->firstlineno;
        if (sf->cur_pc > b->byte_code_buf &&
            qp_code_addr2loc(b, qp_fr_sf_pc(sf, b), &loc) == 0 && loc.lineno >= 0)
            line = loc.lineno;
        info[1] = JS_NewInt32(ctx, line);
        if (JS_IsException(info[0]) || JS_IsException(info[2])) {
            JS_FreeValue(ctx, info[0]);
            JS_FreeValue(ctx, info[2]);
            goto fail;
        }
        items[i] = qp_tuple_new_steal(ctx, 3, info);
        if (JS_IsException(items[i]))
            goto fail;
        i++;
    }
    r = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    return r;
fail:
    while (i > 0)
        JS_FreeValue(ctx, items[--i]);
    js_free(ctx, items);
    return JS_EXCEPTION;
}

static JSValue qp__gen_repr(JSContext *ctx, JSValueConst self)
{
    QPGen *g = qp__gen(self);
    const char *fmt = "<generator object %S at %p>";
    if (g->kind == QP_GENK_CORO)
        fmt = "<coroutine object %S at %p>";
    else if (g->kind == QP_GENK_AGEN)
        fmt = "<async_generator object %S at %p>";
    return qp_str_from_format(ctx, fmt, g->qualname, JS_VALUE_GET_PTR(self));
}

static JSValue qp__self_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

/* ------------------------------------------------------------------------ */
/* coroutine_wrapper                                                         */
/* ------------------------------------------------------------------------ */

static JSValue qp__aux_new(JSContext *ctx, QPType *t, int kind, JSValueConst gen,
                           JSValueConst v1, JSValueConst v2, JSValueConst v3)
{
    QPGenAux *a;
    JSValue obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_GEN_AUX);
    if (JS_IsException(obj))
        return obj;
    a = js_mallocz(ctx, sizeof(*a));
    if (!a) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    a->kind = kind;
    a->state = QP_AWAITABLE_STATE_INIT;
    a->gen = JS_DupValue(ctx, gen);
    a->v1 = JS_DupValue(ctx, v1);
    a->v2 = JS_DupValue(ctx, v2);
    a->v3 = JS_DupValue(ctx, v3);
    JS_VALUE_GET_OBJ(obj)->u.opaque = a;
    return obj;
}

/* coro_await: __await__ of coroutines */
static JSValue qp__coro_await(JSContext *ctx, JSValueConst self)
{
    return qp__aux_new(ctx, QP_TYPE(ctx, coroutine_wrapper), QP_AUX_CORO_WRAPPER, self,
                       QP_NULL, QP_NULL, QP_NULL);
}

static JSValue qp__cw_iternext(JSContext *ctx, JSValueConst self)
{
    return qp__gen_iternext(ctx, qp__aux(self)->gen);
}

static JSValue qp__m_cw_send(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_send(ctx, qp__aux(self)->gen, args[0]);
}

static JSValue qp__m_cw_throw(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_throw_args(ctx, qp__aux(self)->gen, args, nargs, 1);
}

static JSValue qp__m_cw_close(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    return qp__gen_close(ctx, qp__aux(self)->gen);
}

/* ------------------------------------------------------------------------ */
/* async generators                                                          */
/* ------------------------------------------------------------------------ */

static BOOL qp__hook_set(JSValueConst v)
{
    return !QP_IS_NULL(v) && !qp__is_none(v);
}

/* async_gen_init_hooks: -1 on error */
static int qp__agen_init_hooks(JSContext *ctx, JSValueConst self)
{
    QPGen *ag = qp__gen(self);
    QPState *st = QP_STATE(ctx);
    if (ag->ag_hooks_inited)
        return 0;
    ag->ag_hooks_inited = 1;
    if (qp__hook_set(st->asyncgen_finalizer)) {
        JS_FreeValue(ctx, ag->ag_finalizer);
        ag->ag_finalizer = JS_DupValue(ctx, st->asyncgen_finalizer);
    }
    if (qp__hook_set(st->asyncgen_firstiter)) {
        JSValue firstiter = JS_DupValue(ctx, st->asyncgen_firstiter);
        JSValue res = qp_call1(ctx, firstiter, self);
        JS_FreeValue(ctx, firstiter);
        if (JS_IsException(res))
            return -1;
        JS_FreeValue(ctx, res);
    }
    return 0;
}

static JSValue qp__asend_new(JSContext *ctx, JSValueConst ag, JSValueConst sendval)
{
    return qp__aux_new(ctx, QP_TYPE(ctx, async_generator_asend), QP_AUX_ASEND, ag,
                       sendval, QP_NULL, QP_NULL);
}

static JSValue qp__athrow_new(JSContext *ctx, JSValueConst ag, JSValueConst *args, int nargs)
{
    return qp__aux_new(ctx, QP_TYPE(ctx, async_generator_athrow), QP_AUX_ATHROW, ag,
                       nargs > 0 ? args[0] : QP_NULL, nargs > 1 ? args[1] : QP_NULL,
                       nargs > 2 ? args[2] : QP_NULL);
}

/* am_anext of async generators */
static JSValue qp__agen_anext(JSContext *ctx, JSValueConst self)
{
    if (qp__agen_init_hooks(ctx, self) < 0)
        return JS_EXCEPTION;
    return qp__asend_new(ctx, self, QP_NULL);
}

static JSValue qp__m_agen_asend(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (qp__agen_init_hooks(ctx, self) < 0)
        return JS_EXCEPTION;
    return qp__asend_new(ctx, self, args[0]);
}

static JSValue qp__m_agen_aclose(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    if (qp__agen_init_hooks(ctx, self) < 0)
        return JS_EXCEPTION;
    return qp__athrow_new(ctx, self, NULL, 0);
}

static JSValue qp__m_agen_athrow(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    if (nargs > 1) {
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "the (type, exc, tb) signature of athrow() is deprecated, "
                    "use the single-arg signature instead.") < 0)
            return JS_EXCEPTION;
    }
    if (qp__agen_init_hooks(ctx, self) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "athrow", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    return qp__athrow_new(ctx, self, args, nargs);
}

/* async_gen_unwrap_value (result may be JS_EXCEPTION or QP_NULL = ended
   without exception; consumed) */
static JSValue qp__agen_unwrap(JSContext *ctx, QPGen *ag, JSValue result)
{
    if (JS_IsException(result) || QP_IS_NULL(result)) {
        if (QP_IS_NULL(result))
            qp__raise_none(ctx, QP_EXC(ctx, StopAsyncIteration));
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopAsyncIteration)) ||
            qp_exc_pending_matches(ctx, QP_EXC(ctx, GeneratorExit)))
            ag->ag_closed = 1;
        ag->ag_running_async = 0;
        return JS_EXCEPTION;
    }
    if (qp__is_wrapped_value(result)) {
        /* async yield */
        qp__set_stop_iteration_value(ctx, qp__aux(result)->v1);
        JS_FreeValue(ctx, result);
        ag->ag_running_async = 0;
        return JS_EXCEPTION;
    }
    return result;
}

/* ---- asend ---- */

static JSValue qp__asend_send(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    QPGenAux *o = qp__aux(self);
    QPGen *ag = qp__gen(o->gen);
    JSValue result;

    if (o->state == QP_AWAITABLE_STATE_CLOSED)
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                             "cannot reuse already awaited __anext__()/asend()");
    if (o->state == QP_AWAITABLE_STATE_INIT) {
        if (ag->ag_running_async) {
            o->state = QP_AWAITABLE_STATE_CLOSED;
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                                 "anext(): asynchronous generator is already running");
        }
        if (QP_IS_NULL(arg) || qp__is_none(arg))
            arg = o->v1;
        o->state = QP_AWAITABLE_STATE_ITER;
    }
    ag->ag_running_async = 1;
    result = qp__gen_send(ctx, o->gen, arg);
    result = qp__agen_unwrap(ctx, ag, result);
    if (JS_IsException(result))
        o->state = QP_AWAITABLE_STATE_CLOSED;
    return result;
}

/* _PyAsyncGenASend_Send */
static int qp__asend_am_send(JSContext *ctx, JSValueConst self, JSValueConst arg, JSValue *presult)
{
    *presult = qp__asend_send(ctx, self, arg);
    if (!JS_IsException(*presult))
        return QP_SEND_NEXT;
    if (qp__fetch_stop_iteration_value(ctx, presult) == 0)
        return QP_SEND_RETURN;
    *presult = QP_NULL;
    return QP_SEND_ERROR;
}

static JSValue qp__asend_iternext(JSContext *ctx, JSValueConst self)
{
    return qp__asend_send(ctx, self, QP_NULL);
}

static JSValue qp__asend_throw(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs)
{
    QPGenAux *o = qp__aux(self);
    QPGen *ag = qp__gen(o->gen);
    JSValue result;

    if (o->state == QP_AWAITABLE_STATE_CLOSED)
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                             "cannot reuse already awaited __anext__()/asend()");
    if (o->state == QP_AWAITABLE_STATE_INIT) {
        if (ag->ag_running_async) {
            o->state = QP_AWAITABLE_STATE_CLOSED;
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                                 "anext(): asynchronous generator is already running");
        }
        o->state = QP_AWAITABLE_STATE_ITER;
        ag->ag_running_async = 1;
    }
    result = qp__gen_throw_args(ctx, o->gen, args, nargs, 1);
    result = qp__agen_unwrap(ctx, ag, result);
    if (JS_IsException(result)) {
        ag->ag_running_async = 0;
        o->state = QP_AWAITABLE_STATE_CLOSED;
    }
    return result;
}

static JSValue qp__m_asend_send(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp__asend_send(ctx, self, args[0]);
}

static JSValue qp__m_asend_throw(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp__asend_throw(ctx, self, args, nargs);
}

/* close() of asend/athrow (magic 0: asend, 1: athrow) */
static JSValue qp__athrow_throw(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs);

static JSValue qp__m_aux_close(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPGenAux *o = qp__aux(self);
    JSValueConst ge = QP_TYPE_VALUE(QP_EXC(ctx, GeneratorExit));
    JSValue result;
    if (o->state == QP_AWAITABLE_STATE_CLOSED)
        return JS_UNDEFINED;
    result = magic ? qp__athrow_throw(ctx, self, &ge, 1) : qp__asend_throw(ctx, self, &ge, 1);
    if (JS_IsException(result)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration)) ||
            qp_exc_pending_matches(ctx, QP_EXC(ctx, StopAsyncIteration)) ||
            qp_exc_pending_matches(ctx, QP_EXC(ctx, GeneratorExit))) {
            qp__clear_exc(ctx);
            return JS_UNDEFINED;
        }
        return result;
    }
    JS_FreeValue(ctx, result);
    return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), "coroutine ignored GeneratorExit");
}

/* ---- athrow / aclose ---- */

static JSValue qp__athrow_send(JSContext *ctx, JSValueConst self, JSValueConst arg)
{
    QPGenAux *o = qp__aux(self);
    QPGen *ag = qp__gen(o->gen);
    JSValue retval;
    BOOL aclose = QP_IS_NULL(o->v1);

    if (o->state == QP_AWAITABLE_STATE_CLOSED)
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                             "cannot reuse already awaited aclose()/athrow()");
    if (QP_GEN_FINISHED(ag->state)) {
        o->state = QP_AWAITABLE_STATE_CLOSED;
        return qp__raise_none(ctx, QP_EXC(ctx, StopIteration));
    }
    if (o->state == QP_AWAITABLE_STATE_INIT) {
        if (ag->ag_running_async) {
            o->state = QP_AWAITABLE_STATE_CLOSED;
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), aclose ?
                                 "aclose(): asynchronous generator is already running" :
                                 "athrow(): asynchronous generator is already running");
        }
        if (ag->ag_closed) {
            o->state = QP_AWAITABLE_STATE_CLOSED;
            return qp__raise_none(ctx, QP_EXC(ctx, StopAsyncIteration));
        }
        if (QP_IS_NULL(arg) || !qp__is_none(arg))
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), QP_NON_INIT_CORO_MSG);
        o->state = QP_AWAITABLE_STATE_ITER;
        ag->ag_running_async = 1;
        if (aclose) {
            ag->ag_closed = 1;
            /* do not close the delegate when GeneratorExit is passed */
            retval = qp__gen_throw(ctx, o->gen, 0, QP_TYPE_VALUE(QP_EXC(ctx, GeneratorExit)),
                                   QP_NULL, QP_NULL);
            if (!JS_IsException(retval) && qp__is_wrapped_value(retval)) {
                JS_FreeValue(ctx, retval);
                goto yield_close;
            }
        } else {
            retval = qp__gen_throw(ctx, o->gen, 0, o->v1, o->v2, o->v3);
            retval = qp__agen_unwrap(ctx, ag, retval);
        }
        if (JS_IsException(retval))
            goto check_error;
        return retval;
    }

    retval = qp__gen_send(ctx, o->gen, arg);
    if (!aclose)
        return qp__agen_unwrap(ctx, ag, retval);
    if (!JS_IsException(retval) && !QP_IS_NULL(retval)) {
        if (qp__is_wrapped_value(retval)) {
            JS_FreeValue(ctx, retval);
            goto yield_close;
        }
        return retval;
    }
    if (QP_IS_NULL(retval))
        qp__raise_none(ctx, QP_EXC(ctx, StopAsyncIteration));
    goto check_error;

yield_close:
    ag->ag_running_async = 0;
    o->state = QP_AWAITABLE_STATE_CLOSED;
    return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), QP_ASYNC_GEN_IGNORED_EXIT_MSG);

check_error:
    ag->ag_running_async = 0;
    o->state = QP_AWAITABLE_STATE_CLOSED;
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopAsyncIteration)) ||
        qp_exc_pending_matches(ctx, QP_EXC(ctx, GeneratorExit))) {
        if (aclose) {
            /* aclose() does not propagate StopAsyncIteration or
               GeneratorExit: this 'aclose()' await is done */
            qp__clear_exc(ctx);
            qp__raise_none(ctx, QP_EXC(ctx, StopIteration));
        }
    }
    return JS_EXCEPTION;
}

static JSValue qp__athrow_throw(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs)
{
    QPGenAux *o = qp__aux(self);
    QPGen *ag = qp__gen(o->gen);
    BOOL aclose = QP_IS_NULL(o->v1);
    JSValue retval;

    if (o->state == QP_AWAITABLE_STATE_CLOSED)
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError),
                             "cannot reuse already awaited aclose()/athrow()");
    if (o->state == QP_AWAITABLE_STATE_INIT) {
        if (ag->ag_running_async) {
            o->state = QP_AWAITABLE_STATE_CLOSED;
            return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), aclose ?
                                 "aclose(): asynchronous generator is already running" :
                                 "athrow(): asynchronous generator is already running");
        }
        o->state = QP_AWAITABLE_STATE_ITER;
        ag->ag_running_async = 1;
    }
    retval = qp__gen_throw_args(ctx, o->gen, args, nargs, 1);
    if (!aclose) {
        retval = qp__agen_unwrap(ctx, ag, retval);
        if (JS_IsException(retval)) {
            ag->ag_running_async = 0;
            o->state = QP_AWAITABLE_STATE_CLOSED;
        }
        return retval;
    }
    /* aclose() mode */
    if (!JS_IsException(retval) && qp__is_wrapped_value(retval)) {
        ag->ag_running_async = 0;
        o->state = QP_AWAITABLE_STATE_CLOSED;
        JS_FreeValue(ctx, retval);
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), QP_ASYNC_GEN_IGNORED_EXIT_MSG);
    }
    if (JS_IsException(retval)) {
        ag->ag_running_async = 0;
        o->state = QP_AWAITABLE_STATE_CLOSED;
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopAsyncIteration)) ||
            qp_exc_pending_matches(ctx, QP_EXC(ctx, GeneratorExit))) {
            qp__clear_exc(ctx);
            qp__raise_none(ctx, QP_EXC(ctx, StopIteration));
        }
    }
    return retval;
}

static JSValue qp__athrow_iternext(JSContext *ctx, JSValueConst self)
{
    return qp__athrow_send(ctx, self, JS_UNDEFINED);
}

static JSValue qp__m_athrow_send(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp__athrow_send(ctx, self, args[0]);
}

static JSValue qp__m_athrow_throw(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    return qp__athrow_throw(ctx, self, args, nargs);
}

/* ------------------------------------------------------------------------ */
/* object.h section (j) entry points                                         */
/* ------------------------------------------------------------------------ */

static JSValue qp_gen_new(JSContext *ctx, JSValueConst func, JSValue *arg_buf)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(func)->u.func.function_bytecode;
    QPCodeInfo *ci = qp_code_info(b);
    uint32_t flags = ci ? ci->co_flags : 0;
    QPFuncData *fd = qp_function_data(func);
    JSAsyncFunctionState *s;
    QPType *t;
    QPGen *g;
    JSValue obj;
    int i, kind, class_id;

    if (flags & CO_COROUTINE) {
        kind = QP_GENK_CORO;
        t = QP_TYPE(ctx, coroutine);
        class_id = JS_CLASS_PY_COROUTINE;
    } else if (flags & CO_ASYNC_GENERATOR) {
        kind = QP_GENK_AGEN;
        t = QP_TYPE(ctx, async_generator);
        class_id = JS_CLASS_PY_ASYNC_GEN;
    } else {
        kind = QP_GENK_GEN;
        t = QP_TYPE(ctx, generator);
        class_id = JS_CLASS_PY_GENERATOR;
    }

    s = async_func_init(ctx, func, JS_UNDEFINED, b->arg_count, (JSValueConst *)arg_buf);
    for (i = 0; i < b->arg_count; i++) {
        JS_FreeValue(ctx, arg_buf[i]);
        arg_buf[i] = JS_UNDEFINED;
    }
    if (!s)
        return JS_EXCEPTION;
    /* locals start unbound */
    for (i = 0; i < b->var_count; i++)
        s->frame.var_buf[i] = JS_UNINITIALIZED;

    obj = qp_object_new_instance(ctx, t, class_id);
    if (JS_IsException(obj)) {
        async_func_free(ctx->rt, s);
        return obj;
    }
    g = js_mallocz(ctx, sizeof(*g));
    if (!g) {
        async_func_free(ctx->rt, s);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    g->s = s;
    s->py_gen = JS_VALUE_GET_OBJ(obj);
    g->func = JS_DupValue(ctx, func);
    if (fd) {
        g->name = JS_DupValue(ctx, fd->name);
        g->qualname = JS_DupValue(ctx, fd->qualname);
    } else {
        g->name = JS_AtomToString(ctx, b->func_name);
        g->qualname = JS_DupValue(ctx, g->name);
    }
    g->exc_state.exc_value = JS_UNDEFINED;
    g->exc_state.previous = NULL;
    g->ag_finalizer = QP_NULL;
    g->kind = kind;
    g->state = QP_GEN_CREATED;
    JS_VALUE_GET_OBJ(obj)->u.opaque = g;
    if (JS_IsException(g->name) || JS_IsException(g->qualname)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (kind == QP_GENK_CORO && QP_STATE(ctx)->coro_origin_depth > 0) {
        g->ag_finalizer = qp__coro_origin(ctx, QP_STATE(ctx)->coro_origin_depth);
        if (JS_IsException(g->ag_finalizer)) {
            g->ag_finalizer = QP_NULL;
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

/* PyIter_Send */
static int qp_iter_send(JSContext *ctx, JSValueConst recv, JSValueConst v, JSValue *presult)
{
    QPType *t = qp_type_of(ctx, recv);
    JSValue res;

    *presult = QP_NULL;
    if (t->slots.am.am_send)
        return t->slots.am.am_send(ctx, recv, v, presult);
    if (qp__is_none(v) && t->slots.tp_iternext) {
        if (t->flags & QP_TPFLAGS_HEAPTYPE) {
            /* call __next__ so that StopIteration(value) keeps its value */
            res = qp_call_method(ctx, recv, QP_ATOM(ctx, __next__), NULL, 0, QP_NULL);
        } else {
            res = t->slots.tp_iternext(ctx, recv);
        }
    } else {
        res = qp_call_method(ctx, recv, QP_ATOM(ctx, send), &v, 1, QP_NULL);
    }
    if (QP_IS_NULL(res)) {
        *presult = JS_UNDEFINED;
        return QP_SEND_RETURN;
    }
    if (!JS_IsException(res)) {
        *presult = res;
        return QP_SEND_NEXT;
    }
    if (qp__fetch_stop_iteration_value(ctx, presult) == 0)
        return QP_SEND_RETURN;
    *presult = QP_NULL;
    return QP_SEND_ERROR;
}

/* _PyCoro_GetAwaitableIter */
static JSValue qp__coro_get_awaitable_iter(JSContext *ctx, JSValueConst o)
{
    QPType *ot;
    JSValue res;

    if (qp_coro_check(o) || qp__gen_is_coroutine(o))
        return JS_DupValue(ctx, o);
    ot = qp_type_of(ctx, o);
    if (ot->slots.am.am_await) {
        res = ot->slots.am.am_await(ctx, o);
        if (JS_IsException(res))
            return res;
        if (qp_coro_check(res) || qp__gen_is_coroutine(res)) {
            /* __await__ must return an *iterator*, not a coroutine or
               another awaitable (PEP 492) */
            qp_raise_type_error(ctx, "%T.__await__() must return an iterator, not coroutine", o);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if (!qp_type_of(ctx, res)->slots.tp_iternext) {
            qp_raise_type_error(ctx, "%T.__await__() must return an iterator, not %T", o, res);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        return res;
    }
    {
        JSValue name = qp__tp_name(ctx, ot);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "'%U' object can't be awaited", name);
        JS_FreeValue(ctx, name);
    }
    return JS_EXCEPTION;
}

/* _PyEval_GetAwaitable (+ _PyEval_FormatAwaitableError) */
static JSValue qp_get_awaitable(JSContext *ctx, JSValueConst obj, int where)
{
    JSValue iter = qp__coro_get_awaitable_iter(ctx, obj);
    if (JS_IsException(iter)) {
        QPType *type = qp_type_of(ctx, obj);
        if (!type->slots.am.am_await &&
            (where == QP_AWAITABLE_AENTER || where == QP_AWAITABLE_AEXIT)) {
            JSValue name = qp__tp_name(ctx, type);
            qp__clear_exc(ctx);
            if (JS_IsException(name))
                return name;
            qp_raise_type_error(ctx, "'async with' received an object from %s "
                                "that does not implement __await__: %U",
                                where == QP_AWAITABLE_AENTER ? "__aenter__" : "__aexit__",
                                name);
            JS_FreeValue(ctx, name);
        }
        return JS_EXCEPTION;
    }
    if (qp_coro_check(iter) && qp__gen(iter)->state == QP_GEN_SUSPENDED_YIELD_FROM) {
        /* `iter` is a coroutine object that is being awaited */
        JS_FreeValue(ctx, iter);
        return qp__raise_str(ctx, QP_EXC(ctx, RuntimeError), "coroutine is being awaited already");
    }
    return iter;
}

/* GET_AITER */
static JSValue qp_get_aiter(JSContext *ctx, JSValueConst obj)
{
    QPType *type = qp_type_of(ctx, obj);
    JSValue iter, name;

    if (!type->slots.am.am_aiter) {
        name = qp__tp_name(ctx, type);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "'async for' requires an object with "
                            "__aiter__ method, got %U", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    iter = type->slots.am.am_aiter(ctx, obj);
    if (JS_IsException(iter))
        return iter;
    type = qp_type_of(ctx, iter);
    if (!type->slots.am.am_anext) {
        name = qp__tp_name(ctx, type);
        JS_FreeValue(ctx, iter);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "'async for' received an object from __aiter__ "
                            "that does not implement __anext__: %U", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    return iter;
}

/* _PyEval_GetANext */
static JSValue qp_get_anext(JSContext *ctx, JSValueConst aiter)
{
    QPType *type = qp_type_of(ctx, aiter);
    JSValue next_iter, awaitable, name;

    if (qp__class_id(aiter) == JS_CLASS_PY_ASYNC_GEN)
        return qp__agen_anext(ctx, aiter);
    if (!type->slots.am.am_anext) {
        name = qp__tp_name(ctx, type);
        if (JS_IsException(name))
            return name;
        qp_raise_type_error(ctx, "'async for' requires an iterator with "
                            "__anext__ method, got %U", name);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    next_iter = type->slots.am.am_anext(ctx, aiter);
    if (JS_IsException(next_iter))
        return next_iter;
    awaitable = qp__coro_get_awaitable_iter(ctx, next_iter);
    if (JS_IsException(awaitable)) {
        /* _PyErr_FormatFromCause */
        JSValue cause = qp_exc_fetch(ctx), err;
        name = qp__tp_name(ctx, qp_type_of(ctx, next_iter));
        JS_FreeValue(ctx, next_iter);
        if (JS_IsException(name)) {
            JS_FreeValue(ctx, cause);
            return name;
        }
        qp_raise_type_error(ctx, "'async for' received an invalid object "
                            "from __anext__: %U", name);
        JS_FreeValue(ctx, name);
        err = qp_exc_fetch(ctx);
        if (qp_setattr(ctx, err, QP_ATOM(ctx, __cause__), cause) < 0 ||
            qp_setattr(ctx, err, QP_ATOM(ctx, __context__), cause) < 0) {
            JS_FreeValue(ctx, cause);
            JS_FreeValue(ctx, err);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, cause);
        JS_Throw(ctx, err);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, next_iter);
    return awaitable;
}

/* INTRINSIC_STOPITERATION_ERROR (Python/intrinsics.c stopiteration_error) */
static JSValue qp_gen_stopiteration_error(JSContext *ctx, JSValueConst exc, uint32_t co_flags)
{
    const char *msg = NULL;
    JSValue message, error;

    if (qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, StopIteration)))) {
        msg = "generator raised StopIteration";
        if (co_flags & CO_ASYNC_GENERATOR)
            msg = "async generator raised StopIteration";
        else if (co_flags & CO_COROUTINE)
            msg = "coroutine raised StopIteration";
    } else if ((co_flags & CO_ASYNC_GENERATOR) &&
               qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, StopAsyncIteration)))) {
        /* code in an async generator raised StopAsyncIteration */
        msg = "async generator raised StopAsyncIteration";
    }
    if (!msg)
        return JS_DupValue(ctx, exc);
    message = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(message))
        return message;
    error = qp_exc_new(ctx, QP_EXC(ctx, RuntimeError), 1, (JSValueConst *)&message);
    JS_FreeValue(ctx, message);
    if (JS_IsException(error))
        return error;
    if (qp_setattr(ctx, error, QP_ATOM(ctx, __cause__), exc) < 0 ||
        qp_setattr(ctx, error, QP_ATOM(ctx, __context__), exc) < 0) {
        JS_FreeValue(ctx, error);
        return JS_EXCEPTION;
    }
    return error;
}

/* INTRINSIC_ASYNC_GEN_WRAP (_PyAsyncGenValueWrapperNew) */
static JSValue qp_asyncgen_wrap_value(JSContext *ctx, JSValueConst v)
{
    return qp__aux_new(ctx, QP_TYPE(ctx, async_generator_wrapped_value), QP_AUX_WRAPPED,
                       QP_NULL, v, QP_NULL, QP_NULL);
}

/* ------------------------------------------------------------------------ */
/* finalization                                                              */
/* ------------------------------------------------------------------------ */

/* _PyErr_WarnUnawaitedCoroutine: warnings._warn_unawaited_coroutine (which
   shows cr_origin), falling back to a plain RuntimeWarning */
static void qp__warn_unawaited_coro(JSContext *ctx, JSValueConst obj, QPGen *g)
{
    BOOL warned = FALSE;
    JSValue mod, fn, res;
    JSAtom name;

    mod = qp_import_module(ctx, "warnings");
    if (!JS_IsException(mod)) {
        name = JS_NewAtom(ctx, "_warn_unawaited_coroutine");
        fn = name == JS_ATOM_NULL ? JS_EXCEPTION : qp_getattr(ctx, mod, name);
        JS_FreeAtom(ctx, name);
        JS_FreeValue(ctx, mod);
        if (!JS_IsException(fn)) {
            res = qp_call1(ctx, fn, obj);
            JS_FreeValue(ctx, fn);
            if (!JS_IsException(res)) {
                warned = TRUE;
                JS_FreeValue(ctx, res);
            } else if (qp_exc_pending_matches(ctx, QP_EXC(ctx, RuntimeWarning))) {
                warned = TRUE;
            }
        }
    }
    if (JS_HasException(ctx))
        qp_format_unraisable(ctx, "Exception ignored while finalizing coroutine %R", obj);
    if (!warned &&
        qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1,
                "coroutine '%S' was never awaited", g->qualname) < 0)
        qp_format_unraisable(ctx, "Exception ignored while finalizing coroutine %R", obj);
}

/* tp_finalize (gen_finalize, async_gen_asend_finalize,
   async_gen_athrow_finalize); the pending exception is preserved. */
static void qp_gen_finalize(JSContext *ctx, JSValueConst obj)
{
    JSValue saved = JS_GetException(ctx);
    int id = qp__class_id(obj);

    if (id == JS_CLASS_PY_GEN_AUX) {
        QPGenAux *o = qp__aux(obj);
        if ((o->kind == QP_AUX_ASEND || o->kind == QP_AUX_ATHROW) &&
            o->state == QP_AWAITABLE_STATE_INIT) {
            /* _PyErr_WarnUnawaitedAgenMethod */
            const char *method = o->kind == QP_AUX_ASEND ? "asend" :
                (QP_IS_NULL(o->v1) ? "aclose" : "athrow");
            if (qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1,
                        "coroutine method '%s' of %R was never awaited",
                        method, qp__gen(o->gen)->qualname) < 0)
                qp_format_unraisable(ctx, "Exception ignored while finalizing async generator %R",
                                     o->gen);
        }
    } else if (id == JS_CLASS_PY_GENERATOR || id == JS_CLASS_PY_COROUTINE ||
               id == JS_CLASS_PY_ASYNC_GEN) {
        QPGen *g = qp__gen(obj);
        if (QP_GEN_FINISHED(g->state)) {
            /* not paused: nothing to close */
        } else if (g->kind == QP_GENK_AGEN && !QP_IS_NULL(g->ag_finalizer) && !g->ag_closed) {
            JSValue fin = JS_DupValue(ctx, g->ag_finalizer);
            JSValue res = qp_call1(ctx, fin, obj);
            JS_FreeValue(ctx, fin);
            if (JS_IsException(res))
                qp_format_unraisable(ctx, "Exception ignored while finalizing generator %R", obj);
            else
                JS_FreeValue(ctx, res);
        } else if (g->kind == QP_GENK_CORO && g->state == QP_GEN_CREATED) {
            qp__warn_unawaited_coro(ctx, obj, g);
            qp__gen_clear(ctx, g);
        } else {
            JSValue res = qp__gen_close(ctx, obj);
            if (JS_IsException(res)) {
                if (JS_HasException(ctx))
                    qp_format_unraisable(ctx, "Exception ignored while closing generator %R", obj);
            } else {
                JS_FreeValue(ctx, res);
            }
        }
    }
    if (!JS_IsUninitialized(saved))
        JS_Throw(ctx, saved);
}

static void qp_gen_finalizer(JSRuntime *rt, JSValue val)
{
    QPGen *g = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!g)
        return;
    if (g->s) {
        g->s->py_gen = NULL;
        async_func_free(rt, g->s);
    }
    JS_FreeValueRT(rt, g->func);
    JS_FreeValueRT(rt, g->name);
    JS_FreeValueRT(rt, g->qualname);
    JS_FreeValueRT(rt, g->exc_state.exc_value);
    JS_FreeValueRT(rt, g->ag_finalizer);
    js_free_rt(rt, g);
}

static void qp_gen_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPGen *g = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!g)
        return;
    if (g->s)
        mark_func(rt, &g->s->header);
    JS_MarkValue(rt, g->func, mark_func);
    JS_MarkValue(rt, g->name, mark_func);
    JS_MarkValue(rt, g->qualname, mark_func);
    JS_MarkValue(rt, g->exc_state.exc_value, mark_func);
    JS_MarkValue(rt, g->ag_finalizer, mark_func);
}

static void qp_genaux_finalizer(JSRuntime *rt, JSValue val)
{
    QPGenAux *a = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!a)
        return;
    JS_FreeValueRT(rt, a->gen);
    JS_FreeValueRT(rt, a->v1);
    JS_FreeValueRT(rt, a->v2);
    JS_FreeValueRT(rt, a->v3);
    js_free_rt(rt, a);
}

static void qp_genaux_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPGenAux *a = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!a)
        return;
    JS_MarkValue(rt, a->gen, mark_func);
    JS_MarkValue(rt, a->v1, mark_func);
    JS_MarkValue(rt, a->v2, mark_func);
    JS_MarkValue(rt, a->v3, mark_func);
}

/* ------------------------------------------------------------------------ */
/* types                                                                     */
/* ------------------------------------------------------------------------ */

static const QPMethodDef qp__gen_methods[] = {
    { "send", qp__m_gen_send, QP_METH_O, 0,
      "send(value) -> send 'value' into generator,\nreturn next yielded value or raise StopIteration." },
    { "throw", qp__m_gen_throw, QP_METH_FASTCALL, 0,
      "throw(value)\nthrow(type[,value[,tb]])\n\nRaise exception in generator, return next yielded value or raise\n"
      "StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp__m_gen_close, QP_METH_NOARGS, 0, "close() -> raise GeneratorExit inside generator." },
    { "__sizeof__", qp__m_gen_sizeof, QP_METH_NOARGS, 0, "gen.__sizeof__() -> size of gen in memory, in bytes" },
    { "__class_getitem__", qp__m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "generators are generic over the types of their yield, send, and return values" },
    { NULL }
};

static const QPGetSetDef qp__gen_getsets[] = {
    { "__name__", qp__g_name, qp__s_name, 0, "name of the generator" },
    { "__qualname__", qp__g_name, qp__s_name, 1, "qualified name of the generator" },
    { "gi_yieldfrom", qp__g_yieldfrom, NULL, 0, "object being iterated by yield from, or None" },
    { "gi_running", qp__g_running, NULL, 0, NULL },
    { "gi_frame", qp__g_frame, NULL, 0, NULL },
    { "gi_suspended", qp__g_suspended, NULL, 0, NULL },
    { "gi_code", qp__g_code, NULL, 0, NULL },
    { "gi_state", qp__g_state, NULL, 0, "state of the generator" },
    { NULL }
};

static const QPMethodDef qp__coro_methods[] = {
    { "send", qp__m_gen_send, QP_METH_O, 0,
      "send(arg) -> send 'arg' into coroutine,\nreturn next iterated value or raise StopIteration." },
    { "throw", qp__m_gen_throw, QP_METH_FASTCALL, 0,
      "throw(value)\nthrow(type[,value[,traceback]])\n\nRaise exception in coroutine, return next iterated value or raise\n"
      "StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp__m_gen_close, QP_METH_NOARGS, 0, "close() -> raise GeneratorExit inside coroutine." },
    { "__sizeof__", qp__m_gen_sizeof, QP_METH_NOARGS, 0, "gen.__sizeof__() -> size of gen in memory, in bytes" },
    { "__class_getitem__", qp__m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "coroutines are generic over the types of their yield, send, and return values" },
    { NULL }
};

static const QPGetSetDef qp__coro_getsets[] = {
    { "__name__", qp__g_name, qp__s_name, 0, "name of the coroutine" },
    { "__qualname__", qp__g_name, qp__s_name, 1, "qualified name of the coroutine" },
    { "cr_await", qp__g_yieldfrom, NULL, 0, "object being awaited on, or None" },
    { "cr_running", qp__g_running, NULL, 0, NULL },
    { "cr_frame", qp__g_frame, NULL, 0, NULL },
    { "cr_code", qp__g_code, NULL, 0, NULL },
    { "cr_suspended", qp__g_suspended, NULL, 0, NULL },
    { "cr_state", qp__g_state, NULL, 1, "state of the coroutine" },
    { "cr_origin", qp__g_origin, NULL, 0, NULL, QP_GETSET_MEMBER },
    { NULL }
};

static const QPMethodDef qp__agen_methods[] = {
    { "asend", qp__m_agen_asend, QP_METH_O, 0, "asend(v) -> send 'v' in generator." },
    { "athrow", qp__m_agen_athrow, QP_METH_FASTCALL, 0,
      "athrow(value)\nathrow(type[,value[,tb]])\n\nraise exception in generator.\n"
      "the (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "aclose", qp__m_agen_aclose, QP_METH_NOARGS, 0, "aclose() -> raise GeneratorExit inside generator." },
    { "__sizeof__", qp__m_gen_sizeof, QP_METH_NOARGS, 0, "gen.__sizeof__() -> size of gen in memory, in bytes" },
    { "__class_getitem__", qp__m_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "async generators are generic over the types of their yield and send values" },
    { NULL }
};

static const QPGetSetDef qp__agen_getsets[] = {
    { "__name__", qp__g_name, qp__s_name, 0, "name of the async generator" },
    { "__qualname__", qp__g_name, qp__s_name, 1, "qualified name of the async generator" },
    { "ag_await", qp__g_yieldfrom, NULL, 0, "object being awaited on, or None" },
    { "ag_frame", qp__g_frame, NULL, 0, NULL },
    { "ag_code", qp__g_code, NULL, 0, NULL },
    { "ag_suspended", qp__g_suspended, NULL, 0, NULL },
    { "ag_state", qp__g_state, NULL, 2, "state of the async generator" },
    { "ag_running", qp__g_ag_running, NULL, 0, NULL },
    { NULL }
};

static const QPMethodDef qp__cw_methods[] = {
    { "send", qp__m_cw_send, QP_METH_O, 0,
      "send(arg) -> send 'arg' into coroutine,\nreturn next iterated value or raise StopIteration." },
    { "throw", qp__m_cw_throw, QP_METH_FASTCALL, 0,
      "throw(value)\nthrow(type[,value[,traceback]])\n\nRaise exception in coroutine, return next iterated value or raise\n"
      "StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp__m_cw_close, QP_METH_NOARGS, 0, "close() -> raise GeneratorExit inside coroutine." },
    { NULL }
};

static const QPMethodDef qp__asend_methods[] = {
    { "send", qp__m_asend_send, QP_METH_O, 0,
      "send(arg) -> send 'arg' into generator,\nreturn next yielded value or raise StopIteration." },
    { "throw", qp__m_asend_throw, QP_METH_FASTCALL, 0,
      "throw(value)\nthrow(type[,value[,tb]])\n\nRaise exception in generator, return next yielded value or raise\n"
      "StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp__m_aux_close, QP_METH_NOARGS, 0, "close() -> raise GeneratorExit inside generator." },
    { NULL }
};

static const QPMethodDef qp__athrow_methods[] = {
    { "send", qp__m_athrow_send, QP_METH_O, 0,
      "send(arg) -> send 'arg' into generator,\nreturn next yielded value or raise StopIteration." },
    { "throw", qp__m_athrow_throw, QP_METH_FASTCALL, 0,
      "throw(value)\nthrow(type[,value[,tb]])\n\nRaise exception in generator, return next yielded value or raise\n"
      "StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp__m_aux_close, QP_METH_NOARGS, 1, "close() -> raise GeneratorExit inside generator." },
    { NULL }
};

#define QP__GEN_FLAGS (QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_HAS_DEL)

static const QPTypeSpec qp__generator_spec = {
    .name = "generator", .flags = QP__GEN_FLAGS | QP_TPFLAGS_WEAKREF, .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_GENERATOR,
    .slots = {
        .tp_repr = qp__gen_repr,
        .tp_iter = qp__self_iter,
        .tp_iternext = qp__gen_iternext,
        .tp_finalize = qp_gen_finalize,
        .am = { .am_send = qp__gen_am_send },
    },
    .methods = qp__gen_methods, .getsets = qp__gen_getsets,
};

static const QPTypeSpec qp__coroutine_spec = {
    .name = "coroutine", .flags = QP__GEN_FLAGS | QP_TPFLAGS_WEAKREF, .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_COROUTINE,
    .slots = {
        .tp_repr = qp__gen_repr,
        .tp_finalize = qp_gen_finalize,
        .am = { .am_await = qp__coro_await, .am_send = qp__gen_am_send },
    },
    .methods = qp__coro_methods, .getsets = qp__coro_getsets,
};

static const QPTypeSpec qp__async_generator_spec = {
    .name = "async_generator", .flags = QP__GEN_FLAGS | QP_TPFLAGS_WEAKREF, .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_ASYNC_GEN,
    .slots = {
        .tp_repr = qp__gen_repr,
        .tp_finalize = qp_gen_finalize,
        .am = { .am_aiter = qp__self_iter, .am_anext = qp__agen_anext, .am_send = qp__gen_am_send },
    },
    .methods = qp__agen_methods, .getsets = qp__agen_getsets,
};

static const QPTypeSpec qp__coroutine_wrapper_spec = {
    .name = "coroutine_wrapper", .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_GEN_AUX,
    .slots = {
        .tp_iter = qp__self_iter,
        .tp_iternext = qp__cw_iternext,
    },
    .methods = qp__cw_methods,
};

static const QPTypeSpec qp__asend_spec = {
    .name = "async_generator_asend", .flags = QP__GEN_FLAGS, .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_GEN_AUX,
    .slots = {
        .tp_iter = qp__self_iter,
        .tp_iternext = qp__asend_iternext,
        .tp_finalize = qp_gen_finalize,
        .am = { .am_await = qp__self_iter, .am_send = qp__asend_am_send },
    },
    .methods = qp__asend_methods,
};

static const QPTypeSpec qp__athrow_spec = {
    .name = "async_generator_athrow", .flags = QP__GEN_FLAGS, .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_GEN_AUX,
    .slots = {
        .tp_iter = qp__self_iter,
        .tp_iternext = qp__athrow_iternext,
        .tp_finalize = qp_gen_finalize,
        .am = { .am_await = qp__self_iter },
    },
    .methods = qp__athrow_methods,
};

static const QPTypeSpec qp__wrapped_value_spec = {
    .name = "async_generator_wrapped_value",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_GEN_AUX,
};

static int qp_init_generators(JSContext *ctx)
{
    if (qp_type_ready(ctx, QP_TYPE(ctx, generator), &qp__generator_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, coroutine), &qp__coroutine_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, async_generator), &qp__async_generator_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, coroutine_wrapper), &qp__coroutine_wrapper_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, async_generator_asend), &qp__asend_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, async_generator_athrow), &qp__athrow_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, async_generator_wrapped_value), &qp__wrapped_value_spec) < 0)
        return -1;
    QP_STATE(ctx)->asyncgen_firstiter = JS_UNDEFINED;
    QP_STATE(ctx)->asyncgen_finalizer = JS_UNDEFINED;
    return 0;
}
