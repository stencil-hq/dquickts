/*
 * dquickts Python, milestone M4: JavaScript <-> Python interop (plan D3).
 *
 * Python and JS share one runtime and one heap; values are shared by
 * identity (list = Array, str = string, int = int32/BigInt, None =
 * undefined, functions are functions).  This unit adds what identity alone
 * does not give:
 *
 *  - js.JsObject: the Python type of every foreign JS object (Map, Promise,
 *    functions, plain objects...).  Attribute access is JS [[Get]]
 *    (AttributeError when the property is absent; non-constructor functions
 *    are returned bound to the object so `o.method()` gets its `this`),
 *    items, iteration through Symbol.iterator (a Map iterates its keys and
 *    reads as a mapping), len() through `size`/`length`, `in`, identity
 *    `==`, awaiting Promises/thenables, `to_py()` deep conversion and
 *    `new(*args)` for constructors.
 *  - The JS view of Python objects: exotic handlers on every Python class
 *    id.  `o.x` is getattr (AttributeError -> undefined), integer keys and
 *    the str keys of mappings are items, Symbol.iterator iterates, and when
 *    the Python object has no attribute of that name: toString -> str(),
 *    toJSON (dict -> object, tuple/set/views/range -> array, bytes -> array
 *    of ints, int/float/str subclasses -> their value, else str()), valueOf,
 *    length -> len(), callKw (keyword calls), toJs (deep conversion),
 *    `then` on coroutines (the coroutine is scheduled as a Task on the job
 *    queue on first use) and Symbol.hasInstance on types (isinstance).
 *  - Calls: JS has no keyword arguments; a trailing plain object passed by
 *    JS to a Python function is an ordinary positional argument.  Use
 *    `f.callKw(args, kwargs)` (on Function.prototype and on Python
 *    callables).  Python keyword arguments to JS functions arrive as one
 *    trailing plain object (ceval.c qp_call_js_kw).
 *  - Numbers at Python -> JS call boundaries (qp_jsi_call_js_from_py).
 *  - Async: Python coroutines run as Tasks driven by the QuickJS job queue;
 *    `await promise` in Python suspends the Task until the promise settles.
 *  - The builtin `js` module and ES modules importing `.py` files.
 *  - The public C API of quickjs.h: JS_InitPython, JS_EvalPython,
 *    JS_PythonImport.
 *
 * GC: no wrapper objects exist, so nothing here adds external roots; the
 * per-context state holds only shared functions and the awaiter type.
 * Task promises are stored on the coroutine under a private symbol and
 * reference the coroutine through their reactions: an ordinary cycle that
 * the trial-deletion collector reclaims.
 */

/* pythonrun.c (included earlier in the translation unit) */
static JSValue qp_run_code(JSContext *ctx, JSValue bytecode, JSValueConst globals,
                           JSValueConst locals);
static JSValue qp_run_main_module(JSContext *ctx);
static int qp_main_add_path0(JSContext *ctx, const char *path0);

typedef struct QPJsState {
    JSAtom atom_task;           /* private symbol: Task promise of a coroutine */
    JSAtom atom_callKw, atom_toJs;
    JSValue awaiter_type;       /* js._PromiseAwaiter */
    JSValue fn_iterator, fn_tostring, fn_tojson, fn_valueof, fn_then;
    JSValue fn_callkw, fn_hasinstance, fn_tojs;
} QPJsState;

static QPJsState *qp_jsi_state(JSContext *ctx);
static JSValue qp_jsi_to_js(JSContext *ctx, JSValueConst v, int depth);
static JSValue qp_jsi_to_py(JSContext *ctx, JSValueConst v, int depth);
static JSValue qp_jsi_task_for(JSContext *ctx, JSValueConst coro);
static JSModuleDef *qp_jsi_default_module_loader(JSContext *ctx, const char *module_name,
                                                 void *opaque);

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

static inline BOOL qp_jsi_is_pyobj(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        QP_IS_PY_CLASS(JS_VALUE_GET_OBJ(v)->class_id);
}

/* a JS object that is neither a Python object nor a Python function */
static inline BOOL qp_jsi_is_foreign(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && !qp_jsi_is_pyobj(v) &&
        !qp_is_py_function(v);
}

static inline int qp_jsi_class(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT ? JS_VALUE_GET_OBJ(v)->class_id : -1;
}

/* JS_GetException + clear when the pending exception matches type */
static BOOL qp_jsi_clear_if(JSContext *ctx, QPType *type)
{
    if (qp_exc_pending_matches(ctx, type)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return TRUE;
    }
    return FALSE;
}

/* An int that D3 passes to JS as a Number: not int32, |v| <= 2^53. */
static BOOL qp_jsi_int_to_number(JSValueConst v, double *pd)
{
    int64_t i;
    int ovf;
    int tag = JS_VALUE_GET_TAG(v);
    if (tag != JS_TAG_SHORT_BIG_INT && tag != JS_TAG_BIG_INT)
        return FALSE;
    if (qp_long_as_int64_overflow(v, &i, &ovf) < 0 || ovf)
        return FALSE;
    if (i > ((int64_t)1 << 53) || i < -((int64_t)1 << 53))
        return FALSE;
    *pd = (double)i;
    return TRUE;
}

/* Integral finite float64 -> Python int (consumes v). */
static JSValue qp_jsi_number_to_int(JSContext *ctx, JSValue v)
{
    double d;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_FLOAT64)
        return v;
    d = JS_VALUE_GET_FLOAT64(v);
    if (!isfinite(d) || floor(d) != d)
        return v;
    return qp_long_from_double(ctx, d);
}

/* Python key for a property atom: int for array indexes, else str. */
static JSValue qp_jsi_atom_key(JSContext *ctx, JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom)) {
        uint32_t idx = __JS_AtomToUInt32(atom);
        return qp_long_from_int64(ctx, idx);
    }
    return JS_AtomToString(ctx, atom);
}

static BOOL qp_jsi_is_mapping(JSContext *ctx, JSValueConst obj)
{
    QPType *t;
    if (qp_anydict_check(ctx, obj))
        return TRUE;
    t = qp_type_of(ctx, obj);
    return (t->flags & QP_TPFLAGS_MAPPING) && t->slots.mp.mp_subscript;
}

static BOOL qp_jsi_has_getitem(JSContext *ctx, JSValueConst obj)
{
    QPType *t = qp_type_of(ctx, obj);
    return t->slots.mp.mp_subscript || t->slots.sq.sq_item;
}

static BOOL qp_jsi_has_len(JSContext *ctx, JSValueConst obj)
{
    QPType *t = qp_type_of(ctx, obj);
    return t->slots.sq.sq_length || t->slots.mp.mp_length;
}

static BOOL qp_jsi_is_iterable(JSContext *ctx, JSValueConst obj)
{
    QPType *t = qp_type_of(ctx, obj);
    return t->slots.tp_iter || t->slots.sq.sq_item;
}

/* ========================================================================= */
/* Python -> JS call boundary, JS_IsFunction                                  */
/* ========================================================================= */

static JSValue qp_jsi_call_js_from_py(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj, JSValueConst new_target,
                                      int argc, JSValueConst *argv, int flags)
{
    JSValue buf[8], *conv = NULL, ret;
    double d;
    int i, n = 0;

    for (i = 0; i < argc; i++) {
        if (qp_jsi_int_to_number(argv[i], &d))
            n++;
    }
    if (n == 0) {
        ret = JS_CallInternal(ctx, func_obj, this_obj, new_target, argc, (JSValue *)argv,
                              flags | JS_CALL_FLAG_PY_BOUNDARY);
    } else {
        /* the callee may overwrite its (non-copied) argument slots: the
           converted array owns its values and is released afterwards */
        if (argc <= (int)countof(buf)) {
            conv = buf;
        } else {
            conv = js_malloc(ctx, sizeof(JSValue) * argc);
            if (!conv)
                return JS_EXCEPTION;
        }
        for (i = 0; i < argc; i++) {
            if (qp_jsi_int_to_number(argv[i], &d))
                conv[i] = JS_NewFloat64(ctx, d);
            else
                conv[i] = JS_DupValue(ctx, argv[i]);
        }
        ret = JS_CallInternal(ctx, func_obj, this_obj, new_target, argc, conv,
                              (flags & ~JS_CALL_FLAG_PY_ARGS_OFFSET) | JS_CALL_FLAG_PY_BOUNDARY);
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, conv[i]);
        if (conv != buf)
            js_free(ctx, conv);
    }
    if (JS_IsException(ret))
        return ret;
    return qp_jsi_number_to_int(ctx, ret);
}

static BOOL qp_jsi_py_callable(JSObject *p)
{
    JSObject *proto;
    if (p->class_id == JS_CLASS_PY_TYPE)
        return TRUE;
    proto = p->shape ? p->shape->proto : NULL;
    if (proto && proto->class_id == JS_CLASS_PY_TYPE && proto->u.opaque)
        return ((QPType *)proto->u.opaque)->slots.tp_call != NULL;
    return p->class_id == JS_CLASS_PY_CFUNCTION || p->class_id == JS_CLASS_PY_METHOD;
}

/* ========================================================================= */
/* Tasks: Python coroutines driven by the job queue                           */
/* ========================================================================= */

/* data = { coro, resolve, reject } */
static int qp_jsi_task_step(JSContext *ctx, JSValueConst *data, JSValueConst value, int is_throw);

static JSValue qp_jsi_task_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    /* argv = coro, resolve, reject, value, is_throw */
    if (qp_jsi_task_step(ctx, argv, argv[3], JS_VALUE_GET_INT(argv[4])) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static int qp_jsi_task_schedule(JSContext *ctx, JSValueConst *data, JSValueConst value,
                                int is_throw)
{
    JSValueConst args[5];
    args[0] = data[0];
    args[1] = data[1];
    args[2] = data[2];
    args[3] = value;
    args[4] = JS_NewInt32(ctx, is_throw);
    return JS_EnqueueJob(ctx, qp_jsi_task_job, 5, args);
}

/* promise reaction: magic 0 = fulfilled (send), 1 = rejected (throw) */
static JSValue qp_jsi_task_resume(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv, int magic, JSValue *data)
{
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (qp_jsi_task_step(ctx, (JSValueConst *)data, v, magic) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static BOOL qp_jsi_is_thenable(JSContext *ctx, JSValueConst v)
{
    JSValue then;
    BOOL r;
    if (!qp_jsi_is_foreign(v))
        return FALSE;
    if (JS_IsPromise(v))
        return TRUE;
    then = JS_GetProperty(ctx, v, JS_ATOM_then);
    if (JS_IsException(then)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return FALSE;
    }
    r = JS_IsFunction(ctx, then);
    JS_FreeValue(ctx, then);
    return r;
}

/* The coroutine yielded `y`: wait for it (asyncio.Task.__step semantics:
   a bare yield (None) reschedules, a Promise/thenable resumes the task when
   it settles, anything else is thrown back as RuntimeError). */
static int qp_jsi_task_wait(JSContext *ctx, JSValueConst *data, JSValueConst y)
{
    JSValue p, fns[2], r;
    int rc;

    if (JS_IsUndefined(y) || JS_IsNull(y))
        return qp_jsi_task_schedule(ctx, data, JS_UNDEFINED, 0);
    if (!qp_jsi_is_thenable(ctx, y)) {
        JSValue exc;
        qp_raise_runtime_error(ctx, "Task got bad yield: %R", y);
        exc = qp_exc_fetch(ctx);
        if (QP_IS_NULL(exc))
            return -1;
        rc = qp_jsi_task_schedule(ctx, data, exc, 1);
        JS_FreeValue(ctx, exc);
        return rc;
    }
    p = js_promise_resolve(ctx, ctx->promise_ctor, 1, &y, 0);
    if (JS_IsException(p))
        return -1;
    fns[0] = JS_NewCFunctionData(ctx, qp_jsi_task_resume, 1, 0, 3, data);
    fns[1] = JS_NewCFunctionData(ctx, qp_jsi_task_resume, 1, 1, 3, data);
    if (JS_IsException(fns[0]) || JS_IsException(fns[1])) {
        r = JS_EXCEPTION;
    } else {
        r = js_promise_then(ctx, p, 2, (JSValueConst *)fns);
    }
    JS_FreeValue(ctx, fns[0]);
    JS_FreeValue(ctx, fns[1]);
    JS_FreeValue(ctx, p);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_jsi_settle(JSContext *ctx, JSValueConst func, JSValueConst v)
{
    JSValue r = JS_Call(ctx, func, JS_UNDEFINED, 1, &v);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_jsi_task_step(JSContext *ctx, JSValueConst *data, JSValueConst value, int is_throw)
{
    JSValueConst coro = data[0];
    JSValue res = QP_NULL, exc;
    int r, rc;

    if (!is_throw) {
        r = qp_iter_send(ctx, coro, value, &res);
    } else {
        /* a rejection reason that is not a Python exception becomes
           js.JsError holding it */
        exc = qp_exc_normalize(ctx, JS_DupValue(ctx, value));
        res = qp_call_method(ctx, coro, QP_ATOM(ctx, throw), (JSValueConst *)&exc, 1, QP_NULL);
        JS_FreeValue(ctx, exc);
        if (!JS_IsException(res)) {
            r = QP_SEND_NEXT;
        } else if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
            exc = qp_exc_fetch(ctx);
            res = qp_exc_stopiteration_value(ctx, exc);
            JS_FreeValue(ctx, exc);
            r = QP_SEND_RETURN;
        } else {
            res = QP_NULL;
            r = QP_SEND_ERROR;
        }
    }
    switch (r) {
    case QP_SEND_RETURN:
        rc = qp_jsi_settle(ctx, data[1], res);
        break;
    case QP_SEND_NEXT:
        rc = qp_jsi_task_wait(ctx, data, res);
        break;
    default:
        if (ctx->rt->current_exception_is_uncatchable)
            return -1;      /* interrupts propagate to the job runner */
        exc = JS_GetException(ctx);
        rc = qp_jsi_settle(ctx, data[2], exc);
        JS_FreeValue(ctx, exc);
        break;
    }
    JS_FreeValue(ctx, res);
    return rc;
}

/* New Task for coro: the first step runs from the job queue (like
   asyncio's call_soon); returns the Task's promise. */
static JSValue qp_jsi_task_new(JSContext *ctx, JSValueConst coro)
{
    JSValue funcs[2], promise, data[3];
    int rc;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    data[0] = (JSValue)coro;
    data[1] = funcs[0];
    data[2] = funcs[1];
    rc = qp_jsi_task_schedule(ctx, (JSValueConst *)data, JS_UNDEFINED, 0);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (rc < 0) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

/* The Task of a coroutine, created on first use and kept on the coroutine
   under a private symbol, so every `then` / js.create_task observes the
   same run. */
static JSValue qp_jsi_task_for(JSContext *ctx, JSValueConst coro)
{
    QPJsState *st = qp_jsi_state(ctx);
    JSShapeProperty *prs;
    JSProperty *pr;
    JSValue promise;

    if (!st)
        return JS_EXCEPTION;
    if (!qp_coro_check(coro))
        return qp_raise_type_error(ctx, "a coroutine was expected, got %R", coro);
    prs = find_own_property(&pr, JS_VALUE_GET_OBJ(coro), st->atom_task);
    if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL)
        return JS_DupValue(ctx, pr->u.value);
    promise = qp_jsi_task_new(ctx, coro);
    if (JS_IsException(promise))
        return promise;
    if (JS_DefinePropertyValue(ctx, coro, st->atom_task, JS_DupValue(ctx, promise), 0) < 0) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

/* ========================================================================= */
/* js._PromiseAwaiter: the iterator returned by JsObject.__await__            */
/* ========================================================================= */

/* slots: 0 = the awaited promise, 1 = state (0 not yet yielded, 1 waiting) */
#define QP_JSI_AW_PROMISE 0
#define QP_JSI_AW_STATE   1

/* am_send: the first send yields the promise to the driving Task; the next
   one is the settled value, which is the result of the await.  A rejection
   is thrown into the coroutine by the Task (no throw() here: the exception
   is raised at the await). */
static int qp_jsi_awaiter_send(JSContext *ctx, JSValueConst self, JSValueConst arg,
                               JSValue *presult)
{
    JSValue *sl = qp_instance_slots(self);
    if (!sl) {
        qp_raise_type_error(ctx, "invalid awaiter");
        *presult = QP_NULL;
        return QP_SEND_ERROR;
    }
    if (JS_VALUE_GET_TAG(sl[QP_JSI_AW_STATE]) != JS_TAG_INT ||
        JS_VALUE_GET_INT(sl[QP_JSI_AW_STATE]) == 0) {
        sl[QP_JSI_AW_STATE] = JS_NewInt32(ctx, 1);
        *presult = JS_DupValue(ctx, sl[QP_JSI_AW_PROMISE]);
        return QP_SEND_NEXT;
    }
    *presult = JS_DupValue(ctx, arg);
    return QP_SEND_RETURN;
}

static JSValue qp_jsi_awaiter_iternext(JSContext *ctx, JSValueConst self)
{
    JSValue res;
    int r = qp_jsi_awaiter_send(ctx, self, JS_UNDEFINED, &res);
    if (r == QP_SEND_ERROR)
        return JS_EXCEPTION;
    if (r == QP_SEND_RETURN) {
        JS_FreeValue(ctx, res);
        return QP_NULL;
    }
    return res;
}

static JSValue qp_jsi_self_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

static const QPTypeSpec qp_jsi_awaiter_spec = {
    .name = "js._PromiseAwaiter",
    .doc = "Iterator of `await` on a JavaScript Promise.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 2,
    .slots = {
        .tp_iter = qp_jsi_self_iter,
        .tp_iternext = qp_jsi_awaiter_iternext,
        .am = { .am_send = qp_jsi_awaiter_send },
    },
};

/* ========================================================================= */
/* js.JsObject                                                                */
/* ========================================================================= */

static BOOL qp_jsi_is_dunder(JSContext *ctx, JSAtom name)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *s;
    size_t n;
    if (__JS_AtomIsTaggedInt(name) || JS_AtomGetKind(ctx, name) != JS_ATOM_KIND_STRING)
        return FALSE;
    s = JS_AtomGetStr(ctx, buf, sizeof(buf), name);
    n = strlen(s);
    return n > 4 && s[0] == '_' && s[1] == '_' && s[n - 1] == '_' && s[n - 2] == '_';
}

/* o.x: JS [[Get]] when the property exists (functions that are not
   constructors come back bound to o), else the JsObject type's attributes
   (to_py, new, dunders), else AttributeError.  Dunder names look at the
   type first so that Python protocols are not shadowed. */
static JSValue qp_jsobj_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    JSValue v, bound;
    int r;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return qp_generic_getattr(ctx, obj, name);
    if (qp_jsi_is_dunder(ctx, name)) {
        v = qp_generic_getattr(ctx, obj, name);
        if (!JS_IsException(v) || !qp_jsi_clear_if(ctx, QP_EXC(ctx, AttributeError)))
            return v;
    }
    r = JS_HasProperty(ctx, obj, name);
    if (r < 0)
        return JS_EXCEPTION;
    if (!r)
        return qp_generic_getattr(ctx, obj, name);
    v = JS_GetProperty(ctx, obj, name);
    if (JS_IsException(v) || !qp_jsi_is_foreign(v) || !JS_IsFunction(ctx, v) ||
        JS_IsConstructor(ctx, v))
        return v;
    bound = js_function_bind(ctx, v, 1, &obj);
    JS_FreeValue(ctx, v);
    return bound;
}

static int qp_jsobj_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    int r;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return qp_generic_setattr(ctx, obj, name, v);
    if (QP_IS_NULL(v)) {
        r = JS_HasProperty(ctx, obj, name);
        if (r <= 0) {
            if (r == 0)
                qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        return JS_DeleteProperty(ctx, obj, name, JS_PROP_THROW) < 0 ? -1 : 0;
    }
    return JS_SetProperty(ctx, obj, name, JS_DupValue(ctx, v)) < 0 ? -1 : 0;
}

/* String(v) that never fails (functions: "function <name>") */
static JSValue qp_jsi_js_string(JSContext *ctx, JSValueConst v)
{
    JSValue s;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_IsFunction(ctx, v)) {
        JSValue name = JS_GetProperty(ctx, v, JS_ATOM_name);
        if (JS_IsException(name)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            name = JS_UNDEFINED;
        }
        if (JS_IsString(name) && JS_VALUE_GET_STRING(name)->len > 0)
            s = qp_str_from_format(ctx, "[function %U]", name);
        else
            s = qp_str_from_cstr(ctx, "[function]");
        JS_FreeValue(ctx, name);
        return s;
    }
    s = JS_ToStringInternal(ctx, v, TRUE);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        s = js_object_toString(ctx, v, 0, NULL);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            s = qp_str_from_cstr(ctx, "[object]");
        }
    }
    return s;
}

/* repr: <JsObject [object Object]>, <JsObject [object Map]>, ... */
static JSValue qp_jsobj_repr(JSContext *ctx, JSValueConst self)
{
    JSValue s = qp_jsi_js_string(ctx, self), r;
    if (JS_IsException(s))
        return s;
    r = qp_str_from_format(ctx, "<JsObject %U>", s);
    JS_FreeValue(ctx, s);
    return r;
}

/* str: the JS String() of the value */
static JSValue qp_jsobj_str(JSContext *ctx, JSValueConst self)
{
    if (JS_VALUE_GET_TAG(self) == JS_TAG_OBJECT && JS_IsFunction(ctx, self))
        return qp_jsi_js_string(ctx, self);
    return JS_ToStringInternal(ctx, self, TRUE);
}

static BOOL qp_jsi_is_map(JSValueConst v)
{
    int c = qp_jsi_class(v);
    return c == JS_CLASS_MAP || c == JS_CLASS_WEAKMAP;
}

static BOOL qp_jsi_is_set(JSValueConst v)
{
    int c = qp_jsi_class(v);
    return c == JS_CLASS_SET || c == JS_CLASS_WEAKSET;
}

/* size of a Map/Set or numeric `length`; 1 found, 0 none, -1 error */
static int qp_jsi_js_length(JSContext *ctx, JSValueConst obj, int64_t *plen)
{
    JSValue v;
    double d;
    v = JS_GetProperty(ctx, obj, (qp_jsi_is_map(obj) || qp_jsi_is_set(obj)) ?
                       JS_ATOM_size : JS_ATOM_length);
    if (JS_IsException(v))
        return -1;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    if (!(d >= 0))
        d = 0;
    *plen = d > 9.0e18 ? INT64_MAX : (int64_t)d;
    return 1;
}

static Py_ssize_t qp_jsobj_length(JSContext *ctx, JSValueConst self)
{
    int64_t n;
    int r;
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT)
        r = 0;
    else
        r = qp_jsi_js_length(ctx, self, &n);
    if (r < 0)
        return -1;
    if (r == 0) {
        qp_raise_type_error(ctx, "object of type '%T' has no len()", self);
        return -1;
    }
    return (Py_ssize_t)n;
}

static int qp_jsobj_bool(JSContext *ctx, JSValueConst self)
{
    int64_t n;
    int r;
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT)
        return JS_ToBool(ctx, self);
    if (JS_IsFunction(ctx, self))
        return 1;
    r = qp_jsi_js_length(ctx, self, &n);
    if (r < 0)
        return -1;
    return r == 0 || n != 0;
}

static JSValue qp_jsi_invoke1(JSContext *ctx, JSValueConst obj, JSAtom m, JSValueConst a)
{
    return JS_Invoke(ctx, obj, m, 1, &a);
}

static JSValue qp_jsobj_subscript(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    JSAtom atom;
    JSValue v;
    int r;

    if (qp_jsi_is_map(self)) {
        v = qp_jsi_invoke1(ctx, self, JS_ATOM_has, key);
        if (JS_IsException(v))
            return v;
        r = JS_ToBoolFree(ctx, v);
        if (!r)
            return qp_raise_key_error(ctx, key);
        return qp_jsi_invoke1(ctx, self, JS_ATOM_get, key);
    }
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT)
        return qp_raise_type_error(ctx, "'%T' object is not subscriptable", self);
    atom = JS_ValueToAtom(ctx, key);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_HasProperty(ctx, self, atom);
    if (r <= 0) {
        JS_FreeAtom(ctx, atom);
        if (r == 0)
            qp_raise_key_error(ctx, key);
        return JS_EXCEPTION;
    }
    v = JS_GetProperty(ctx, self, atom);
    JS_FreeAtom(ctx, atom);
    return v;
}

static int qp_jsobj_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst key,
                                  JSValueConst v)
{
    JSAtom atom;
    JSValue r;
    int rc;

    if (qp_jsi_is_map(self)) {
        if (QP_IS_NULL(v)) {
            r = qp_jsi_invoke1(ctx, self, JS_ATOM_delete, key);
            if (JS_IsException(r))
                return -1;
            if (!JS_ToBoolFree(ctx, r)) {
                qp_raise_key_error(ctx, key);
                return -1;
            }
            return 0;
        } else {
            JSValueConst args[2] = { key, v };
            r = JS_Invoke(ctx, self, JS_ATOM_set, 2, args);
            if (JS_IsException(r))
                return -1;
            JS_FreeValue(ctx, r);
            return 0;
        }
    }
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT) {
        qp_raise_type_error(ctx, "'%T' object does not support item assignment", self);
        return -1;
    }
    atom = JS_ValueToAtom(ctx, key);
    if (atom == JS_ATOM_NULL)
        return -1;
    if (QP_IS_NULL(v)) {
        rc = JS_HasProperty(ctx, self, atom);
        if (rc == 0)
            qp_raise_key_error(ctx, key);
        if (rc > 0)
            rc = JS_DeleteProperty(ctx, self, atom, JS_PROP_THROW);
        rc = rc > 0 ? 0 : -1;
    } else {
        rc = JS_SetProperty(ctx, self, atom, JS_DupValue(ctx, v)) < 0 ? -1 : 0;
    }
    JS_FreeAtom(ctx, atom);
    return rc;
}

static int qp_jsobj_contains(JSContext *ctx, JSValueConst self, JSValueConst key)
{
    JSAtom atom;
    int r;
    if (qp_jsi_is_map(self) || qp_jsi_is_set(self)) {
        JSValue v = qp_jsi_invoke1(ctx, self, JS_ATOM_has, key);
        if (JS_IsException(v))
            return -1;
        return JS_ToBoolFree(ctx, v);
    }
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT) {
        qp_raise_type_error(ctx, "argument of type '%T' is not a container", self);
        return -1;
    }
    atom = JS_ValueToAtom(ctx, key);
    if (atom == JS_ATOM_NULL)
        return -1;
    r = JS_HasProperty(ctx, self, atom);
    JS_FreeAtom(ctx, atom);
    return r;
}

/* iter(o): a Map iterates its keys (mapping view), anything else uses
   o[Symbol.iterator](); the JS iterator is itself a JsObject whose
   __next__ calls next(). */
static JSValue qp_jsobj_iter(JSContext *ctx, JSValueConst self)
{
    JSValue m, it;
    if (qp_jsi_is_map(self))
        return JS_Invoke(ctx, self, JS_ATOM_keys, 0, NULL);
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT)
        return qp_raise_type_error(ctx, "'%T' object is not iterable", self);
    m = JS_GetProperty(ctx, self, JS_ATOM_Symbol_iterator);
    if (JS_IsException(m))
        return m;
    if (!JS_IsFunction(ctx, m)) {
        JS_FreeValue(ctx, m);
        return qp_raise_type_error(ctx, "'%T' object is not iterable", self);
    }
    it = JS_CallFree(ctx, m, self, 0, NULL);
    if (JS_IsException(it))
        return it;
    if (!JS_IsObject(it)) {
        JS_FreeValue(ctx, it);
        return qp_raise_type_error(ctx, "Symbol.iterator returned a non-object");
    }
    return it;
}

static JSValue qp_jsobj_iternext(JSContext *ctx, JSValueConst self)
{
    JSValue m, r, done, v;
    int d;
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT)
        return qp_raise_type_error(ctx, "'%T' object is not an iterator", self);
    m = JS_GetProperty(ctx, self, JS_ATOM_next);
    if (JS_IsException(m))
        return m;
    if (!JS_IsFunction(ctx, m)) {
        JS_FreeValue(ctx, m);
        return qp_raise_type_error(ctx, "'%T' object is not an iterator", self);
    }
    r = JS_CallFree(ctx, m, self, 0, NULL);
    if (JS_IsException(r))
        return r;
    if (!JS_IsObject(r)) {
        JS_FreeValue(ctx, r);
        return qp_raise_type_error(ctx, "iterator result is not an object");
    }
    done = JS_GetProperty(ctx, r, JS_ATOM_done);
    if (JS_IsException(done)) {
        JS_FreeValue(ctx, r);
        return done;
    }
    d = JS_ToBoolFree(ctx, done);
    if (d) {
        JS_FreeValue(ctx, r);
        return QP_NULL;
    }
    v = JS_GetProperty(ctx, r, JS_ATOM_value);
    JS_FreeValue(ctx, r);
    return v;
}

/* __await__: Promises and thenables */
static JSValue qp_jsobj_await(JSContext *ctx, JSValueConst self)
{
    QPJsState *st;
    JSValue aw;
    JSValue *sl;

    if (!qp_jsi_is_thenable(ctx, self))
        return qp_raise_type_error(ctx, "'%T' object can't be awaited (not a Promise or thenable)",
                                   self);
    st = qp_jsi_state(ctx);
    if (!st)
        return JS_EXCEPTION;
    aw = qp_object_new_instance(ctx, qp_type_from_value(st->awaiter_type), JS_CLASS_PY_OBJECT);
    if (JS_IsException(aw))
        return aw;
    sl = qp_instance_slots(aw);
    sl[QP_JSI_AW_PROMISE] = JS_DupValue(ctx, self);
    sl[QP_JSI_AW_STATE] = JS_NewInt32(ctx, 0);
    return aw;
}

/* JsObject.to_py(depth=-1) */
static JSValue qp_jsobj_m_to_py(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "depth", NULL };
    static const QPArgSpec spec = { "to_py", kw, 0, 1, 0 };
    JSValueConst out[1];
    int depth = -1;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(out[0]) && qp_arg_int(ctx, out[0], &depth) < 0)
        return JS_EXCEPTION;
    return qp_jsi_to_py(ctx, self, depth);
}

/* JsObject.new(*args): `new o(...args)` */
static JSValue qp_jsobj_m_new(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                              JSValueConst kwnames, int magic)
{
    if (!JS_IsConstructor(ctx, self))
        return qp_raise_type_error(ctx, "%R is not a constructor", self);
    return JS_CallConstructor(ctx, self, nargs, args);
}

static const QPMethodDef qp_jsobj_methods[] = {
    { "to_py", qp_jsobj_m_to_py, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "to_py($self, /, depth=-1)\n--\n\n"
      "Deep copy into Python containers: Array -> list, plain object -> dict,\n"
      "Map -> dict, Set -> set; other values are kept.  depth limits the\n"
      "number of converted levels (-1: unlimited)." },
    { "new", qp_jsobj_m_new, QP_METH_FASTCALL, 0,
      "new($self, /, *args)\n--\n\nCall the JavaScript constructor: `new self(...args)`." },
    { NULL }
};

static const QPTypeSpec qp_jsobject_spec = {
    .name = "js.JsObject",
    .doc = "A JavaScript object seen from Python (shared by identity).",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_OBJECT,
    .slots = {
        .tp_getattro = qp_jsobj_getattro,
        .tp_setattro = qp_jsobj_setattro,
        .tp_repr = qp_jsobj_repr,
        .tp_str = qp_jsobj_str,
        .tp_iter = qp_jsobj_iter,
        .tp_iternext = qp_jsobj_iternext,
        .nb = { .nb_bool = qp_jsobj_bool },
        .sq = { .sq_contains = qp_jsobj_contains },
        .mp = { .mp_length = qp_jsobj_length, .mp_subscript = qp_jsobj_subscript,
                .mp_ass_subscript = qp_jsobj_ass_subscript },
        .am = { .am_await = qp_jsobj_await },
    },
    .methods = qp_jsobj_methods,
};

static const QPTypeSpec *qp_jsobject_type_spec(void)
{
    return &qp_jsobject_spec;
}

/* ========================================================================= */
/* Deep conversions                                                           */
/* ========================================================================= */

/* memo: Python dict int(address) -> converted value (cycles, sharing) */
static int qp_jsi_memo_get(JSContext *ctx, JSValueConst memo, JSValueConst v, JSValue *pres)
{
    JSValue key = qp_long_from_int64(ctx, (int64_t)(intptr_t)JS_VALUE_GET_PTR(v));
    int r;
    if (JS_IsException(key))
        return -1;
    r = qp_dict_get_item(ctx, memo, key, pres);
    JS_FreeValue(ctx, key);
    return r;
}

static int qp_jsi_memo_set(JSContext *ctx, JSValueConst memo, JSValueConst v, JSValueConst res)
{
    JSValue key = qp_long_from_int64(ctx, (int64_t)(intptr_t)JS_VALUE_GET_PTR(v));
    int r;
    if (JS_IsException(key))
        return -1;
    r = qp_dict_set_item(ctx, memo, key, res);
    JS_FreeValue(ctx, key);
    return r;
}

static JSValue qp_jsi_to_py_rec(JSContext *ctx, JSValueConst v, int depth, JSValueConst memo);

static int qp_jsi_to_py_item(JSContext *ctx, JSValueConst v, int depth, JSValueConst memo,
                             JSValue *pres)
{
    *pres = qp_jsi_to_py_rec(ctx, v, depth, memo);
    return JS_IsException(*pres) ? -1 : 0;
}

static JSValue qp_jsi_to_py_rec(JSContext *ctx, JSValueConst v, int depth, JSValueConst memo)
{
    JSObject *p, *proto;
    JSValue res, it, item, conv, conv2, val;
    int r;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || !qp_jsi_is_foreign(v) || depth == 0)
        return JS_DupValue(ctx, v);
    p = JS_VALUE_GET_OBJ(v);
    proto = p->shape->proto;
    if (p->class_id == JS_CLASS_ARRAY && proto && proto->class_id == JS_CLASS_PY_TYPE)
        return JS_DupValue(ctx, v);     /* a Python list subclass instance */
    if (p->class_id != JS_CLASS_ARRAY && p->class_id != JS_CLASS_MAP &&
        p->class_id != JS_CLASS_SET &&
        !(p->class_id == JS_CLASS_OBJECT &&
          (!proto || proto == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_OBJECT]))))
        return JS_DupValue(ctx, v);
    r = qp_jsi_memo_get(ctx, memo, v, &res);
    if (r != 0)
        return r < 0 ? JS_EXCEPTION : res;
    if (depth > 0)
        depth--;

    switch (p->class_id) {
    case JS_CLASS_ARRAY: {
        int64_t len, i;
        res = qp_list_new(ctx, 0);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        if (js_get_length64(ctx, &len, v))
            goto fail;
        for (i = 0; i < len; i++) {
            item = JS_GetPropertyInt64(ctx, v, i);
            if (JS_IsException(item))
                goto fail;
            r = qp_jsi_to_py_item(ctx, item, depth, memo, &conv);
            JS_FreeValue(ctx, item);
            if (r < 0)
                goto fail;
            r = qp_list_append(ctx, res, conv);
            JS_FreeValue(ctx, conv);
            if (r < 0)
                goto fail;
        }
        return res;
    }
    case JS_CLASS_MAP:
    case JS_CLASS_SET:
        res = p->class_id == JS_CLASS_MAP ? qp_dict_new(ctx) : qp_set_from_array(ctx, 0, NULL);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        it = qp_jsobj_iter(ctx, v);     /* Map: keys, Set: values */
        if (JS_IsException(it))
            goto fail;
        for (;;) {
            item = qp_jsobj_iternext(ctx, it);
            if (QP_IS_NULL(item))
                break;
            if (JS_IsException(item))
                goto fail_it;
            r = qp_jsi_to_py_item(ctx, item, depth, memo, &conv);
            if (r < 0) {
                JS_FreeValue(ctx, item);
                goto fail_it;
            }
            if (p->class_id == JS_CLASS_MAP) {
                val = qp_jsi_invoke1(ctx, v, JS_ATOM_get, item);
                r = JS_IsException(val) ? -1 : qp_jsi_to_py_item(ctx, val, depth, memo, &conv2);
                JS_FreeValue(ctx, val);
                if (r == 0) {
                    r = qp_dict_set_item(ctx, res, conv, conv2);
                    JS_FreeValue(ctx, conv2);
                }
            } else {
                r = qp_set_add(ctx, res, conv);
            }
            JS_FreeValue(ctx, conv);
            JS_FreeValue(ctx, item);
            if (r < 0)
                goto fail_it;
        }
        JS_FreeValue(ctx, it);
        return res;
    fail_it:
        JS_FreeValue(ctx, it);
        goto fail;
    default: {
        /* plain object: own enumerable string keys */
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0, i;
        res = qp_dict_new(ctx);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail;
        for (i = 0; i < len; i++) {
            JSValue key = JS_AtomToString(ctx, tab[i].atom);
            r = -1;
            if (!JS_IsException(key)) {
                val = JS_GetProperty(ctx, v, tab[i].atom);
                if (!JS_IsException(val)) {
                    r = qp_jsi_to_py_item(ctx, val, depth, memo, &conv);
                    if (r == 0) {
                        r = qp_dict_set_item(ctx, res, key, conv);
                        JS_FreeValue(ctx, conv);
                    }
                    JS_FreeValue(ctx, val);
                }
                JS_FreeValue(ctx, key);
            }
            if (r < 0) {
                JS_FreePropertyEnum(ctx, tab, len);
                goto fail;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
        return res;
    }
    }
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static JSValue qp_jsi_to_py(JSContext *ctx, JSValueConst v, int depth)
{
    JSValue memo = qp_dict_new(ctx), r;
    if (JS_IsException(memo))
        return memo;
    r = qp_jsi_to_py_rec(ctx, v, depth, memo);
    JS_FreeValue(ctx, memo);
    return r;
}

static JSValue qp_jsi_to_js_rec(JSContext *ctx, JSValueConst v, int depth, JSValueConst memo);

/* append the converted `item` to the JS array `arr` at index i */
static int qp_jsi_to_js_put(JSContext *ctx, JSValueConst arr, uint32_t i, JSValueConst item,
                            int depth, JSValueConst memo)
{
    JSValue c = qp_jsi_to_js_rec(ctx, item, depth, memo);
    if (JS_IsException(c))
        return -1;
    return JS_DefinePropertyValueUint32(ctx, arr, i, c, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

static JSValue qp_jsi_to_js_rec(JSContext *ctx, JSValueConst v, int depth, JSValueConst memo)
{
    JSValue res, k, val, c;
    double d;
    int r;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) {
        if (qp_jsi_int_to_number(v, &d))
            return JS_NewFloat64(ctx, d);
        return JS_DupValue(ctx, v);
    }
    if (depth == 0)
        return JS_DupValue(ctx, v);
    if (!qp_list_check(ctx, v) && !qp_tuple_check(ctx, v) && !qp_anydict_check(ctx, v) &&
        !qp_anyset_check(ctx, v) && !qp_bytes_check(ctx, v) && !qp_bytearray_check(ctx, v))
        return JS_DupValue(ctx, v);
    r = qp_jsi_memo_get(ctx, memo, v, &res);
    if (r != 0)
        return r < 0 ? JS_EXCEPTION : res;
    if (depth > 0)
        depth--;

    if (qp_bytes_check(ctx, v) || qp_bytearray_check(ctx, v)) {
        Py_ssize_t len;
        const uint8_t *data = qp_bytes_data(v, &len);
        res = JS_NewUint8ArrayCopy(ctx, data, len);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        return res;
    }
    if (qp_list_check(ctx, v) || qp_tuple_check(ctx, v)) {
        Py_ssize_t i, n;
        res = JS_NewArray(ctx);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        if (qp_tuple_check(ctx, v) && JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_TUPLE) {
            n = QP_TUPLE_SIZE(v);
            for (i = 0; i < n; i++) {
                if (qp_jsi_to_js_put(ctx, res, (uint32_t)i, QP_TUPLE_ITEMS(v)[i], depth, memo) < 0)
                    goto fail;
            }
        } else {
            JSValue it = qp_iter(ctx, v), item;
            if (JS_IsException(it))
                goto fail;
            for (i = 0;; i++) {
                item = qp_next(ctx, it);
                if (QP_IS_NULL(item))
                    break;
                if (JS_IsException(item) ||
                    qp_jsi_to_js_put(ctx, res, (uint32_t)i, item, depth, memo) < 0) {
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, it);
                    goto fail;
                }
                JS_FreeValue(ctx, item);
            }
            JS_FreeValue(ctx, it);
        }
        return res;
    }
    if (qp_anydict_check(ctx, v)) {
        Py_ssize_t pos = 0;
        JSAtom atom;
        res = JS_NewObject(ctx);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        while (qp_dict_next(v, &pos, &k, &val)) {
            /* qp_dict_next returns borrowed references */
            k = JS_DupValue(ctx, k);
            val = JS_DupValue(ctx, val);
            atom = JS_ValueToAtom(ctx, k);
            c = atom == JS_ATOM_NULL ? JS_EXCEPTION : qp_jsi_to_js_rec(ctx, val, depth, memo);
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, val);
            if (JS_IsException(c) ||
                JS_DefinePropertyValue(ctx, res, atom, c, JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(ctx, atom);
                goto fail;
            }
            JS_FreeAtom(ctx, atom);
        }
        return res;
    }
    /* set / frozenset -> Set */
    {
        JSValue ctor, it, item, rr;
        ctor = JS_GetProperty(ctx, ctx->global_obj, JS_ATOM_Set);
        if (JS_IsException(ctor))
            return ctor;
        res = JS_CallConstructor(ctx, ctor, 0, NULL);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(res) || qp_jsi_memo_set(ctx, memo, v, res) < 0)
            goto fail;
        it = qp_iter(ctx, v);
        if (JS_IsException(it))
            goto fail;
        for (;;) {
            item = qp_next(ctx, it);
            if (QP_IS_NULL(item))
                break;
            c = JS_IsException(item) ? JS_EXCEPTION : qp_jsi_to_js_rec(ctx, item, depth, memo);
            JS_FreeValue(ctx, item);
            rr = JS_IsException(c) ? JS_EXCEPTION : qp_jsi_invoke1(ctx, res, JS_ATOM_add, c);
            JS_FreeValue(ctx, c);
            if (JS_IsException(rr)) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
            JS_FreeValue(ctx, rr);
        }
        JS_FreeValue(ctx, it);
        return res;
    }
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

/* Deep Python -> JS copy: list/tuple -> Array, dict -> plain object (keys
   through ToPropertyKey), set/frozenset -> Set, bytes/bytearray ->
   Uint8Array copy, ints within +-2^53 -> Number; other values kept. */
static JSValue qp_jsi_to_js(JSContext *ctx, JSValueConst v, int depth)
{
    JSValue memo, r;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return qp_jsi_to_js_rec(ctx, v, depth, JS_UNDEFINED);
    memo = qp_dict_new(ctx);
    if (JS_IsException(memo))
        return memo;
    r = qp_jsi_to_js_rec(ctx, v, depth, memo);
    JS_FreeValue(ctx, memo);
    return r;
}

/* Shallow toJSON of a Python object (JSON.stringify recurses itself). */
static JSValue qp_jsi_tojson(JSContext *ctx, JSValueConst v)
{
    JSObject *p;
    JSValue res, it, item, k, val;
    int cls;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return JS_DupValue(ctx, v);
    p = JS_VALUE_GET_OBJ(v);
    cls = p->class_id;
    if (cls == JS_CLASS_PY_BOXED)
        return JS_DupValue(ctx, p->u.object_data);
    if (qp_anydict_check(ctx, v)) {
        Py_ssize_t pos = 0;
        JSAtom atom;
        res = JS_NewObject(ctx);
        if (JS_IsException(res))
            return res;
        while (qp_dict_next(v, &pos, &k, &val)) {
            atom = JS_ValueToAtom(ctx, k);
            if (atom == JS_ATOM_NULL ||
                JS_DefinePropertyValue(ctx, res, atom, JS_DupValue(ctx, val), JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(ctx, atom);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            JS_FreeAtom(ctx, atom);
        }
        return res;
    }
    if (qp_bytes_check(ctx, v) || qp_bytearray_check(ctx, v)) {
        Py_ssize_t len, i;
        const uint8_t *data = qp_bytes_data(v, &len);
        res = JS_NewArray(ctx);
        for (i = 0; !JS_IsException(res) && i < len; i++) {
            if (JS_DefinePropertyValueUint32(ctx, res, (uint32_t)i, JS_NewInt32(ctx, data[i]),
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
        return res;
    }
    if (qp_tuple_check(ctx, v) || qp_anyset_check(ctx, v) || cls == JS_CLASS_PY_DICT_VIEW ||
        cls == JS_CLASS_PY_RANGE || (qp_type_of(ctx, v)->flags & QP_TPFLAGS_SEQUENCE)) {
        uint32_t i;
        res = JS_NewArray(ctx);
        if (JS_IsException(res))
            return res;
        it = qp_iter(ctx, v);
        if (JS_IsException(it)) {
            JS_FreeValue(ctx, res);
            return it;
        }
        for (i = 0;; i++) {
            item = qp_next(ctx, it);
            if (QP_IS_NULL(item))
                break;
            if (JS_IsException(item) ||
                JS_DefinePropertyValueUint32(ctx, res, i, item, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, it);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, it);
        return res;
    }
    return qp_str(ctx, v);
}

/* ========================================================================= */
/* JS view of Python objects: exotic handlers                                 */
/* ========================================================================= */

/* fallback methods (this = the Python object) */
static JSValue qp_jsi_fn_tostring(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    if (!ctx->py)
        return js_object_toString(ctx, this_val, 0, NULL);
    return qp_str(ctx, this_val);
}

static JSValue qp_jsi_fn_valueof(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    if (qp_jsi_class(this_val) == JS_CLASS_PY_BOXED)
        return JS_DupValue(ctx, JS_VALUE_GET_OBJ(this_val)->u.object_data);
    return JS_DupValue(ctx, this_val);
}

static JSValue qp_jsi_fn_tojson(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    if (!ctx->py)
        return JS_UNDEFINED;
    return qp_jsi_tojson(ctx, this_val);
}

static JSValue qp_jsi_fn_tojs(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    int depth = -1;
    if (!ctx->py)
        return JS_DupValue(ctx, this_val);
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &depth, argv[0]) < 0)
        return JS_EXCEPTION;
    return qp_jsi_to_js(ctx, this_val, depth);
}

static JSValue qp_jsi_iter_next(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic, JSValue *data)
{
    JSValue v = qp_next(ctx, data[0]);
    if (JS_IsException(v))
        return v;
    if (QP_IS_NULL(v))
        return js_create_iterator_result(ctx, JS_UNDEFINED, TRUE);
    return js_create_iterator_result(ctx, v, FALSE);
}

/* [Symbol.iterator]() of Python iterables: an Iterator.prototype object
   whose next() steps the Python iterator */
static JSValue qp_jsi_fn_iterator(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    JSValue it, obj, next;
    if (!ctx->py)
        return JS_ThrowTypeError(ctx, "Python is not initialized in this context");
    it = qp_iter(ctx, this_val);
    if (JS_IsException(it))
        return it;
    obj = JS_NewObjectProto(ctx, ctx->class_proto[JS_CLASS_ITERATOR]);
    next = JS_IsException(obj) ? JS_EXCEPTION :
        JS_NewCFunctionData(ctx, qp_jsi_iter_next, 0, 0, 1, (JSValueConst *)&it);
    JS_FreeValue(ctx, it);
    if (JS_IsException(next) ||
        JS_DefinePropertyValue(ctx, obj, JS_ATOM_next, next,
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* coroutine.then(onFulfilled, onRejected): schedule it as a Task */
static JSValue qp_jsi_fn_then(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    JSValue task, r;
    if (!ctx->py)
        return JS_ThrowTypeError(ctx, "Python is not initialized in this context");
    task = qp_jsi_task_for(ctx, this_val);
    if (JS_IsException(task))
        return task;
    r = js_promise_then(ctx, task, argc, argv);
    JS_FreeValue(ctx, task);
    return r;
}

/* f.callKw(args, kwargs): call any callable with keyword arguments.
   args: array-like or undefined; kwargs: plain object (own enumerable
   string keys), a Python dict with str keys, or undefined. */
static JSValue qp_jsi_fn_callkw(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    JSValueConst args = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst kwargs = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue *vals = NULL, kwnames = QP_NULL, ret = JS_EXCEPTION;
    JSPropertyEnum *tab = NULL;
    uint32_t ntab = 0;
    int64_t nargs = 0, nkw = 0, i, n = 0;

    if (!ctx->py)
        return JS_ThrowTypeError(ctx, "Python is not initialized in this context");
    if (!JS_IsUndefined(args) && !JS_IsNull(args) && js_get_length64(ctx, &nargs, args))
        return JS_EXCEPTION;
    if (JS_IsUndefined(kwargs) || JS_IsNull(kwargs)) {
        nkw = 0;
    } else if (qp_anydict_check(ctx, kwargs)) {
        nkw = qp_dict_size(kwargs);
    } else if (JS_IsObject(kwargs)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, kwargs,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return JS_EXCEPTION;
        nkw = ntab;
    } else {
        return JS_ThrowTypeError(ctx, "callKw: kwargs must be an object");
    }
    if (nargs + nkw > 65535) {
        JS_ThrowRangeError(ctx, "callKw: too many arguments");
        goto done;
    }
    vals = js_mallocz(ctx, sizeof(JSValue) * (size_t)(nargs + nkw + 1));
    if (!vals)
        goto done;
    for (i = 0; i < nargs; i++, n++) {
        vals[n] = JS_GetPropertyInt64(ctx, args, i);
        if (JS_IsException(vals[n]))
            goto done;
    }
    if (nkw) {
        kwnames = qp_tuple_new(ctx, nkw);
        if (JS_IsException(kwnames)) {
            kwnames = QP_NULL;
            goto done;
        }
        if (tab) {
            for (i = 0; i < nkw; i++, n++) {
                JSValue name = JS_AtomToString(ctx, tab[i].atom);
                if (JS_IsException(name))
                    goto done;
                JS_FreeValue(ctx, QP_TUPLE_ITEMS(kwnames)[i]);
                QP_TUPLE_ITEMS(kwnames)[i] = name;
                vals[n] = JS_GetProperty(ctx, kwargs, tab[i].atom);
                if (JS_IsException(vals[n]))
                    goto done;
            }
        } else {
            Py_ssize_t pos = 0;
            JSValue k, v;
            for (i = 0; i < nkw && qp_dict_next(kwargs, &pos, &k, &v); i++, n++) {
                if (!qp_str_check(ctx, k)) {
                    qp_raise_type_error(ctx, "keywords must be strings");
                    goto done;
                }
                JS_FreeValue(ctx, QP_TUPLE_ITEMS(kwnames)[i]);
                QP_TUPLE_ITEMS(kwnames)[i] = JS_DupValue(ctx, k);
                vals[n] = JS_DupValue(ctx, v);
            }
            if (i != nkw) {
                qp_raise_runtime_error(ctx, "dictionary changed size during iteration");
                goto done;
            }
        }
    }
    ret = qp_vectorcall(ctx, this_val, (JSValueConst *)vals, (size_t)nargs, kwnames);
 done:
    if (vals) {
        for (i = 0; i < n; i++)
            JS_FreeValue(ctx, vals[i]);
        js_free(ctx, vals);
    }
    if (!QP_IS_NULL(kwnames))
        JS_FreeValue(ctx, kwnames);
    if (tab)
        JS_FreePropertyEnum(ctx, tab, ntab);
    return ret;
}

/* PythonType[Symbol.hasInstance](v): isinstance(v, PythonType) */
static JSValue qp_jsi_fn_hasinstance(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    int r;
    if (!ctx->py || argc < 1)
        return JS_FALSE;
    r = qp_isinstance(ctx, argv[0], this_val);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

/* Receiver of a property access that reached a Python type through the
   prototype chain of one of its instances (e.g. a list subclass instance,
   whose class id is Array): look the name up on the instance. */
static JSValueConst qp_jsi_target(JSContext *ctx, JSValueConst obj, JSValueConst receiver)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id == JS_CLASS_PY_TYPE && JS_VALUE_GET_TAG(receiver) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(receiver) != JS_VALUE_GET_PTR(obj) &&
        qp_type_of(ctx, receiver) == (QPType *)p->u.opaque)
        return receiver;
    return obj;
}

static JSValue qp_jsi_get_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                                   JSValueConst receiver)
{
    QPJsState *st;
    JSValueConst target;
    JSValue v, key;
    int r;

    if (!ctx->py)
        return JS_UNDEFINED;
    st = qp_jsi_state(ctx);
    if (!st)
        return JS_EXCEPTION;
    target = qp_jsi_target(ctx, obj, receiver);
    if (__JS_AtomIsTaggedInt(atom)) {
        if (!qp_jsi_has_getitem(ctx, target))
            return JS_UNDEFINED;
        key = qp_jsi_atom_key(ctx, atom);
        if (JS_IsException(key))
            return key;
        v = qp_getitem(ctx, target, key);
        JS_FreeValue(ctx, key);
        if (JS_IsException(v) && qp_jsi_clear_if(ctx, QP_EXC(ctx, LookupError)))
            return JS_UNDEFINED;
        return v;
    }
    if (JS_AtomGetKind(ctx, atom) == JS_ATOM_KIND_SYMBOL) {
        if (atom == JS_ATOM_Symbol_iterator && qp_jsi_is_iterable(ctx, target))
            return JS_DupValue(ctx, st->fn_iterator);
        if (atom == JS_ATOM_Symbol_hasInstance && qp_jsi_class(target) == JS_CLASS_PY_TYPE)
            return JS_DupValue(ctx, st->fn_hasinstance);
        return JS_UNDEFINED;
    }
    r = qp_getattr_opt(ctx, target, atom, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return v;
    /* no Python attribute of that name */
    if (atom == JS_ATOM_toString)
        return JS_DupValue(ctx, st->fn_tostring);
    if (atom == JS_ATOM_toJSON)
        return JS_DupValue(ctx, st->fn_tojson);
    if (atom == JS_ATOM_valueOf)
        return JS_DupValue(ctx, st->fn_valueof);
    if (atom == st->atom_toJs)
        return JS_DupValue(ctx, st->fn_tojs);
    if (atom == JS_ATOM_then)
        return qp_coro_check(target) ? JS_DupValue(ctx, st->fn_then) : JS_UNDEFINED;
    if (atom == st->atom_callKw && JS_VALUE_GET_TAG(target) == JS_TAG_OBJECT &&
        qp_jsi_py_callable(JS_VALUE_GET_OBJ(target)))
        return JS_DupValue(ctx, st->fn_callkw);
    if (qp_jsi_is_mapping(ctx, target)) {
        key = JS_AtomToString(ctx, atom);
        if (JS_IsException(key))
            return key;
        v = qp_getitem(ctx, target, key);
        JS_FreeValue(ctx, key);
        if (!JS_IsException(v))
            return v;
        if (!qp_jsi_clear_if(ctx, QP_EXC(ctx, KeyError)))
            return v;
    }
    if (atom == JS_ATOM_length && qp_jsi_has_len(ctx, target)) {
        Py_ssize_t n = qp_len(ctx, target);
        if (n < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(ctx, n);
    }
    return JS_UNDEFINED;
}

static int qp_jsi_set_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                               JSValueConst val, JSValueConst receiver, int flags)
{
    JSValueConst target;
    JSValue key;
    int r;

    if (!ctx->py || JS_AtomGetKind(ctx, atom) == JS_ATOM_KIND_SYMBOL)
        return JS_DefinePropertyValue(ctx, obj, atom, JS_DupValue(ctx, val), JS_PROP_C_W_E);
    target = qp_jsi_target(ctx, obj, receiver);
    if (!__JS_AtomIsTaggedInt(atom)) {
        if (!qp_dict_check_exact(ctx, target)) {
            r = qp_setattr(ctx, target, atom, val);
            if (r == 0)
                return TRUE;
            if (!qp_type_of(ctx, target)->slots.mp.mp_ass_subscript ||
                !qp_jsi_clear_if(ctx, QP_EXC(ctx, AttributeError)))
                return -1;
        }
    }
    key = qp_jsi_atom_key(ctx, atom);
    if (JS_IsException(key))
        return -1;
    r = qp_setitem(ctx, target, key, val);
    JS_FreeValue(ctx, key);
    return r < 0 ? -1 : TRUE;
}

static int qp_jsi_has_property(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    JSValue v, key;
    int r;

    if (!ctx->py)
        return FALSE;
    if (JS_AtomGetKind(ctx, atom) == JS_ATOM_KIND_SYMBOL) {
        if (atom == JS_ATOM_Symbol_iterator)
            return qp_jsi_is_iterable(ctx, obj);
        if (atom == JS_ATOM_Symbol_hasInstance)
            return qp_jsi_class(obj) == JS_CLASS_PY_TYPE;
        return FALSE;
    }
    if (__JS_AtomIsTaggedInt(atom)) {
        if (!qp_jsi_has_getitem(ctx, obj))
            return FALSE;
        if (qp_jsi_is_mapping(ctx, obj)) {
            key = qp_jsi_atom_key(ctx, atom);
            if (JS_IsException(key))
                return -1;
            r = qp_contains(ctx, obj, key);
            JS_FreeValue(ctx, key);
            return r;
        }
        if (qp_jsi_has_len(ctx, obj)) {
            Py_ssize_t n = qp_len(ctx, obj);
            if (n < 0)
                return -1;
            return (int64_t)__JS_AtomToUInt32(atom) < n;
        }
        return FALSE;
    }
    r = qp_getattr_opt(ctx, obj, atom, &v);
    if (r != 0) {
        JS_FreeValue(ctx, v);
        return r;
    }
    if (qp_jsi_is_mapping(ctx, obj)) {
        key = JS_AtomToString(ctx, atom);
        if (JS_IsException(key))
            return -1;
        r = qp_contains(ctx, obj, key);
        JS_FreeValue(ctx, key);
        return r;
    }
    return FALSE;
}

static int qp_jsi_delete_property(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    JSValue key;
    int r;

    if (!ctx->py || JS_AtomGetKind(ctx, atom) == JS_ATOM_KIND_SYMBOL)
        return TRUE;
    if (!__JS_AtomIsTaggedInt(atom) && !qp_dict_check_exact(ctx, obj)) {
        if (qp_delattr(ctx, obj, atom) == 0)
            return TRUE;
        if (!qp_type_of(ctx, obj)->slots.mp.mp_ass_subscript ||
            !qp_jsi_clear_if(ctx, QP_EXC(ctx, AttributeError)))
            return -1;
    }
    key = qp_jsi_atom_key(ctx, atom);
    if (JS_IsException(key))
        return -1;
    r = qp_delitem(ctx, obj, key);
    JS_FreeValue(ctx, key);
    if (r < 0 && !qp_jsi_clear_if(ctx, QP_EXC(ctx, LookupError)))
        return -1;
    return TRUE;
}

/* Object.keys & co.: the str/int keys of dicts, the indexes of tuples */
static int qp_jsi_get_own_property_names(JSContext *ctx, JSPropertyEnum **ptab,
                                         uint32_t *plen, JSValueConst obj)
{
    JSPropertyEnum *tab;
    uint32_t n = 0, cap;
    *ptab = NULL;
    *plen = 0;
    if (!ctx->py)
        return 0;
    if (qp_anydict_check(ctx, obj)) {
        Py_ssize_t pos = 0;
        JSValue k, v;
        cap = (uint32_t)qp_dict_size(obj);
        tab = js_malloc(ctx, sizeof(*tab) * max_int(cap, 1));
        if (!tab)
            return -1;
        while (n < cap && qp_dict_next(obj, &pos, &k, &v)) {
            int t = JS_VALUE_GET_TAG(k);
            if (t != JS_TAG_STRING && t != JS_TAG_STRING_ROPE &&
                !(t == JS_TAG_INT && JS_VALUE_GET_INT(k) >= 0))
                continue;
            tab[n].atom = JS_ValueToAtom(ctx, k);
            if (tab[n].atom == JS_ATOM_NULL) {
                JS_FreePropertyEnum(ctx, tab, n);
                return -1;
            }
            tab[n].is_enumerable = TRUE;
            n++;
        }
    } else if (qp_tuple_check(ctx, obj) && qp_jsi_class(obj) == JS_CLASS_PY_TUPLE) {
        cap = (uint32_t)QP_TUPLE_SIZE(obj);
        tab = js_malloc(ctx, sizeof(*tab) * max_int(cap, 1));
        if (!tab)
            return -1;
        for (n = 0; n < cap; n++) {
            tab[n].atom = __JS_AtomFromUInt32(n);
            tab[n].is_enumerable = TRUE;
        }
    } else {
        return 0;
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

static int qp_jsi_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc,
                                   JSValueConst obj, JSAtom atom)
{
    JSValue key, v = JS_UNDEFINED;
    int r, flags;

    if (!ctx->py || JS_AtomGetKind(ctx, atom) == JS_ATOM_KIND_SYMBOL)
        return FALSE;
    if (qp_anydict_check(ctx, obj)) {
        key = qp_jsi_atom_key(ctx, atom);
        if (JS_IsException(key))
            return -1;
        r = qp_dict_get_item(ctx, obj, key, &v);
        JS_FreeValue(ctx, key);
        if (r <= 0)
            return r;
        flags = JS_PROP_C_W_E;
    } else if (__JS_AtomIsTaggedInt(atom) && qp_jsi_class(obj) == JS_CLASS_PY_TUPLE) {
        uint32_t idx = __JS_AtomToUInt32(atom);
        if (idx >= (uint32_t)QP_TUPLE_SIZE(obj))
            return FALSE;
        v = JS_DupValue(ctx, QP_TUPLE_ITEMS(obj)[idx]);
        flags = JS_PROP_ENUMERABLE;
    } else {
        return FALSE;
    }
    if (desc) {
        desc->flags = flags;
        desc->value = v;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    } else {
        JS_FreeValue(ctx, v);
    }
    return TRUE;
}

static const JSClassExoticMethods qp_jsi_exotic = {
    .get_own_property = qp_jsi_get_own_property,
    .get_own_property_names = qp_jsi_get_own_property_names,
    .delete_property = qp_jsi_delete_property,
    .has_property = qp_jsi_has_property,
    .get_property = qp_jsi_get_property,
    .set_property = qp_jsi_set_property,
};

/* Install the exotic handlers on every Python class id of the runtime
   (once).  Objects created before get their is_exotic bit here. */
static void qp_jsi_register_exotic(JSRuntime *rt)
{
    struct list_head *el;
    int id;

    if (rt->class_array[JS_CLASS_PY_OBJECT].exotic == &qp_jsi_exotic)
        return;
    for (id = JS_CLASS_PY_FIRST; id <= JS_CLASS_PY_LAST; id++) {
        if (id == JS_CLASS_PY_FUNC_DATA || id == JS_CLASS_PY_NATIVE)
            continue;
        rt->class_array[id].exotic = &qp_jsi_exotic;
    }
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (js_rc(gp)->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT) {
            JSObject *p = (JSObject *)gp;
            if (QP_IS_PY_CLASS(p->class_id) && rt->class_array[p->class_id].exotic)
                p->is_exotic = 1;
        }
    }
}

/* ========================================================================= */
/* Per-context state                                                          */
/* ========================================================================= */

static void qp_jsi_mark(JSRuntime *rt, QPJsState *st, JS_MarkFunc *mark_func)
{
    if (!st)
        return;
    JS_MarkValue(rt, st->awaiter_type, mark_func);
    JS_MarkValue(rt, st->fn_iterator, mark_func);
    JS_MarkValue(rt, st->fn_tostring, mark_func);
    JS_MarkValue(rt, st->fn_tojson, mark_func);
    JS_MarkValue(rt, st->fn_valueof, mark_func);
    JS_MarkValue(rt, st->fn_then, mark_func);
    JS_MarkValue(rt, st->fn_callkw, mark_func);
    JS_MarkValue(rt, st->fn_hasinstance, mark_func);
    JS_MarkValue(rt, st->fn_tojs, mark_func);
}

static void qp_jsi_free(JSContext *ctx)
{
    QPJsState *st = ctx->py_js;
    if (!st)
        return;
    ctx->py_js = NULL;
    JS_FreeValue(ctx, st->awaiter_type);
    JS_FreeValue(ctx, st->fn_iterator);
    JS_FreeValue(ctx, st->fn_tostring);
    JS_FreeValue(ctx, st->fn_tojson);
    JS_FreeValue(ctx, st->fn_valueof);
    JS_FreeValue(ctx, st->fn_then);
    JS_FreeValue(ctx, st->fn_callkw);
    JS_FreeValue(ctx, st->fn_hasinstance);
    JS_FreeValue(ctx, st->fn_tojs);
    JS_FreeAtom(ctx, st->atom_task);
    JS_FreeAtom(ctx, st->atom_callKw);
    JS_FreeAtom(ctx, st->atom_toJs);
    js_free(ctx, st);
}

/* The interop state of ctx, created on first use: shared fallback
   functions, the awaiter type, the exotic handlers of the runtime and
   Function.prototype.callKw.  NULL with an exception pending. */
static QPJsState *qp_jsi_state(JSContext *ctx)
{
    QPJsState *st = ctx->py_js;
    JSValue sym;

    if (st)
        return st;
    if (!ctx->py) {
        JS_ThrowInternalError(ctx, "Python is not initialized in this context");
        return NULL;
    }
    st = js_mallocz(ctx, sizeof(*st));
    if (!st) {
        qp_raise_memory_error(ctx);
        return NULL;
    }
    st->awaiter_type = JS_UNDEFINED;
    st->fn_iterator = st->fn_tostring = st->fn_tojson = st->fn_valueof = JS_UNDEFINED;
    st->fn_then = st->fn_callkw = st->fn_hasinstance = st->fn_tojs = JS_UNDEFINED;
    ctx->py_js = st;

    /* a private symbol: ordinary own property of the coroutine, invisible
       to JS and to the exotic handlers */
    sym = JS_NewSymbolFromAtom(ctx, QP_ATOM(ctx, __await__), JS_ATOM_TYPE_PRIVATE);
    if (JS_IsException(sym))
        goto fail;
    st->atom_task = JS_DupAtom(ctx, js_symbol_to_atom(ctx, sym));
    JS_FreeValue(ctx, sym);
    st->atom_callKw = JS_NewAtom(ctx, "callKw");
    st->atom_toJs = JS_NewAtom(ctx, "toJs");
    if (st->atom_callKw == JS_ATOM_NULL || st->atom_toJs == JS_ATOM_NULL)
        goto fail;
    st->awaiter_type = qp_type_from_spec(ctx, &qp_jsi_awaiter_spec);
    if (JS_IsException(st->awaiter_type)) {
        st->awaiter_type = JS_UNDEFINED;
        goto fail;
    }
#define QP_JSI_FN(field, fn, name, len)                                   \
    st->field = JS_NewCFunction(ctx, fn, name, len);                       \
    if (JS_IsException(st->field)) {                                       \
        st->field = JS_UNDEFINED;                                          \
        goto fail;                                                         \
    }
    QP_JSI_FN(fn_iterator, qp_jsi_fn_iterator, "[Symbol.iterator]", 0);
    QP_JSI_FN(fn_tostring, qp_jsi_fn_tostring, "toString", 0);
    QP_JSI_FN(fn_tojson, qp_jsi_fn_tojson, "toJSON", 1);
    QP_JSI_FN(fn_valueof, qp_jsi_fn_valueof, "valueOf", 0);
    QP_JSI_FN(fn_then, qp_jsi_fn_then, "then", 2);
    QP_JSI_FN(fn_callkw, qp_jsi_fn_callkw, "callKw", 2);
    QP_JSI_FN(fn_hasinstance, qp_jsi_fn_hasinstance, "[Symbol.hasInstance]", 1);
    QP_JSI_FN(fn_tojs, qp_jsi_fn_tojs, "toJs", 0);
#undef QP_JSI_FN
    /* Function.prototype.callKw: keyword calls for Python functions (which
       are bytecode functions) and JS functions alike */
    if (JS_DefinePropertyValue(ctx, ctx->function_proto, st->atom_callKw,
                               JS_DupValue(ctx, st->fn_callkw),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        goto fail;
    qp_jsi_register_exotic(ctx->rt);
    if (!ctx->rt->u.module_loader_func)
        JS_SetModuleLoaderFunc(ctx->rt, NULL, qp_jsi_default_module_loader, NULL);
    return st;
 fail:
    qp_jsi_free(ctx);
    return NULL;
}

/* ========================================================================= */
/* Python initialization (one path for the launcher and the C API)            */
/* ========================================================================= */

/* Py_InitializeFromConfig for a context: Python state, importlib (when the
   build boots it), __main__, sys.path[0], interop state.  0, or -1 with an
   exception pending.  qp_main (pythonrun.c) runs the same sequence. */
static int qp_python_init(JSContext *ctx, const QPConfig *cfg)
{
    JSValue m;
    if (!ctx->py) {
        if (qp_state_init(ctx, cfg) < 0)
            return -1;
#ifdef QP_IMPORTLIB_BOOT
        if (qp_importlib_boot(ctx) < 0)
            return -1;
#endif
        m = qp_run_main_module(ctx);
        if (JS_IsException(m))
            return -1;
        JS_FreeValue(ctx, m);
        if (cfg && cfg->sys_path_0 && qp_main_add_path0(ctx, cfg->sys_path_0) < 0)
            return -1;
    }
    return qp_jsi_state(ctx) ? 0 : -1;
}

int JS_InitPython(JSContext *ctx, const JSPythonConfig *cfg)
{
    QPConfig qc;
    memset(&qc, 0, sizeof(qc));
    qc.int_max_str_digits = -1;
    qc.lazy_imports = -1;
    qc.no_site = 1;
    if (cfg) {
        qc.argc = cfg->argc;
        qc.argv = cfg->argv;
        qc.orig_argc = cfg->argc;
        qc.orig_argv = cfg->argv;
        qc.executable = cfg->executable;
        qc.sys_path_0 = cfg->sys_path_0;
        qc.pythonpath = cfg->pythonpath;
        qc.hash_seed = cfg->hash_seed;
        qc.optimize = cfg->optimize;
        qc.isolated = cfg->isolated;
        qc.ignore_environment = cfg->isolated;
        qc.recursion_limit = cfg->recursion_limit;
        qc.home = cfg->home;
    }
    return qp_python_init(ctx, &qc);
}

static int qp_jsi_ensure_python(JSContext *ctx)
{
    if (ctx->py)
        return qp_jsi_state(ctx) ? 0 : -1;
    return JS_InitPython(ctx, NULL);
}

/* Run a module-level bytecode (consumed) in the globals of __main__. */
static JSValue qp_jsi_run_main(JSContext *ctx, JSValue bc)
{
    JSValue m, d, res;
    m = qp_run_main_module(ctx);
    if (JS_IsException(m)) {
        JS_FreeValue(ctx, bc);
        return m;
    }
    d = qp_module_get_dict(m);
    if (QP_IS_NULL(d)) {
        JS_FreeValue(ctx, bc);
        JS_FreeValue(ctx, m);
        return qp_raise_runtime_error(ctx, "__main__ has no __dict__");
    }
    res = qp_run_code(ctx, bc, d, QP_NULL);
    JS_FreeValue(ctx, m);
    return res;
}

JSValue JS_EvalPython(JSContext *ctx, const char *input, size_t input_len,
                      const char *filename, int flags)
{
    static const int modes[3] = { QP_INPUT_FILE, QP_INPUT_EVAL, QP_INPUT_SINGLE };
    int mode_idx = flags & JS_PY_EVAL_MODE_MASK;
    qp_error err;
    JSValue bc, res, funcs[2], promise;
    char *src;

    if (mode_idx > 2)
        return JS_ThrowRangeError(ctx, "JS_EvalPython: invalid mode");
    if (qp_jsi_ensure_python(ctx) < 0)
        return JS_EXCEPTION;
    /* the front end wants src[len] == '\0' */
    src = js_malloc(ctx, input_len + 1);
    if (!src)
        return JS_EXCEPTION;
    memcpy(src, input, input_len);
    src[input_len] = '\0';
    if (!filename)
        filename = "<string>";
    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, src, input_len, filename, QP_NULL, modes[mode_idx],
                           QP_PF_IGNORE_COOKIE |
                           ((flags & JS_PY_EVAL_FLAG_ASYNC) ? QP_PF_TOP_LEVEL_AWAIT : 0),
                           -1, &err);
    js_free(ctx, src);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        return bc;
    }
    qp_error_clear(&err);
    res = qp_jsi_run_main(ctx, bc);
    if (!(flags & JS_PY_EVAL_FLAG_ASYNC) || JS_IsException(res))
        return res;
    /* top-level await: the code object is a coroutine; run it as a Task */
    if (qp_coro_check(res)) {
        promise = qp_jsi_task_for(ctx, res);
        JS_FreeValue(ctx, res);
        return promise;
    }
    /* no await at top level: an already resolved promise */
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (!JS_IsException(promise)) {
        if (qp_jsi_settle(ctx, funcs[0], res) < 0) {
            JS_FreeValue(ctx, promise);
            promise = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
    }
    JS_FreeValue(ctx, res);
    return promise;
}

JSValue JS_PythonImport(JSContext *ctx, const char *module)
{
    if (qp_jsi_ensure_python(ctx) < 0)
        return JS_EXCEPTION;
    return qp_import_module(ctx, module);
}

/* ========================================================================= */
/* ES modules importing .py files                                             */
/* ========================================================================= */

static char *qp_jsi_read_file(JSContext *ctx, const char *filename, size_t *plen)
{
    FILE *f;
    char *buf = NULL;
    long n;

    f = fopen(filename, "rb");
    if (!f)
        goto fail;
    if (fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        goto fail;
    }
    buf = js_malloc(ctx, (size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        js_free(ctx, buf);
        goto fail;
    }
    fclose(f);
    buf[n] = '\0';
    *plen = (size_t)n;
    return buf;
 fail:
    JS_ThrowReferenceError(ctx, "could not load module filename '%s'", filename);
    return NULL;
}

/* Module loader installed when the host has none (the qpy launcher), so
   that js.import_module() can load ES module files: compile the file as a
   module (import.meta.url is not set). */
static JSModuleDef *qp_jsi_default_module_loader(JSContext *ctx, const char *module_name,
                                                 void *opaque)
{
    size_t len;
    char *buf = qp_jsi_read_file(ctx, module_name, &len);
    JSValue func;
    if (!buf)
        return NULL;
    func = JS_Eval(ctx, buf, len, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, buf);
    if (JS_IsException(func))
        return NULL;
    /* the module stays referenced by the context's module list */
    JS_FreeValue(ctx, func);
    return JS_VALUE_GET_PTR(func);
}

/* Insert dir at sys.path[0] unless sys.path already contains it (the
   directory of a script, as for `python script.py`), so that the module
   can import its siblings. */
static int qp_jsi_add_sys_path(JSContext *ctx, const char *dir, size_t len)
{
    JSValue path, s;
    int r;
    path = qp_sys_get(ctx, QP_ATOM(ctx, path));
    if (JS_IsException(path))
        return -1;
    if (QP_IS_NULL(path) || !qp_list_check(ctx, path)) {
        JS_FreeValue(ctx, path);
        return 0;
    }
    s = qp_str_from_utf8(ctx, dir, len);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, path);
        return -1;
    }
    r = qp_contains(ctx, path, s);
    if (r == 0)
        r = qp_list_insert(ctx, path, 0, s);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, path);
    return r < 0 ? -1 : 0;
}

/* Export names of a Python module (a dict used as an ordered set):
   __all__ when it is defined, else the globals not starting with '_';
   `default` is reserved for the module object. */
static JSValue qp_jsi_export_names(JSContext *ctx, JSValueConst d)
{
    JSValue names, all, it, k, v;
    Py_ssize_t pos = 0;
    int r;

    names = qp_dict_new(ctx);
    if (JS_IsException(names))
        return names;
    r = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __all__), &all);
    if (r < 0)
        goto fail;
    if (r > 0) {
        it = qp_iter(ctx, all);
        JS_FreeValue(ctx, all);
        if (JS_IsException(it))
            goto fail;
        for (;;) {
            k = qp_next(ctx, it);
            if (QP_IS_NULL(k))
                break;
            if (JS_IsException(k)) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
            if (!qp_str_check_exact(k)) {
                qp_raise_type_error(ctx, "Item in __all__ must be str, not %T", k);
                r = -1;
            } else {
                r = qp_dict_set_item(ctx, names, k, JS_UNDEFINED);
            }
            JS_FreeValue(ctx, k);
            if (r < 0) {
                JS_FreeValue(ctx, it);
                goto fail;
            }
        }
        JS_FreeValue(ctx, it);
    } else {
        while (qp_dict_next(d, &pos, &k, &v)) {
            JSString *str;
            if (JS_VALUE_GET_TAG(k) != JS_TAG_STRING)
                continue;
            str = JS_VALUE_GET_STRING(k);
            if (str->len > 0 && string_get(str, 0) == '_')
                continue;
            if (qp_dict_set_item(ctx, names, k, JS_UNDEFINED) < 0)
                goto fail;
        }
    }
    return names;
 fail:
    JS_FreeValue(ctx, names);
    return JS_EXCEPTION;
}

/* ES module evaluation of a synthetic .py module: the exports take the
   values the globals have now (named exports are snapshots, not live
   bindings). */
static int qp_jsi_py_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue mod = JS_GetModulePrivateValue(ctx, m), d, v;
    const char *cname;
    int i, r;

    d = qp_module_get_dict(mod);
    for (i = 0; i < m->export_entries_count; i++) {
        JSAtom name = m->export_entries[i].export_name;
        if (name == JS_ATOM_default) {
            v = JS_DupValue(ctx, mod);
        } else {
            r = QP_IS_NULL(d) ? 0 : qp_dict_get_item_atom(ctx, d, name, &v);
            if (r < 0)
                goto fail;
            if (r == 0)
                v = JS_UNDEFINED;
        }
        cname = JS_AtomToCString(ctx, name);
        if (!cname) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
        r = JS_SetModuleExport(ctx, m, cname, v);
        JS_FreeCString(ctx, cname);
        if (r < 0)
            goto fail;
    }
    JS_FreeValue(ctx, mod);
    return 0;
 fail:
    JS_FreeValue(ctx, mod);
    return -1;
}

/* `import ... from "x.py"`: the Python file runs now (module loading time,
   before the importing module is evaluated) as a module named after the
   file (`x`, or the directory name for __init__.py) with __file__ set; it
   is registered in sys.modules under that name unless the name is taken,
   and its directory is added to sys.path.  The ES module exports the
   public globals (see qp_jsi_export_names) and `default` = the module
   object.  Python exceptions propagate unchanged. */
static JSModuleDef *qp_jsi_load_py_module(JSContext *ctx, const char *filename)
{
    QPState *s;
    JSModuleDef *m = NULL;
    JSValue name = JS_UNDEFINED, mod = JS_UNDEFINED, file = JS_UNDEFINED;
    JSValue names = JS_UNDEFINED, bc, res, d, k, v;
    const char *base, *slash, *cname;
    size_t len, blen;
    Py_ssize_t pos;
    qp_error err;
    char *buf;
    int registered = 0, r;

    if (qp_jsi_ensure_python(ctx) < 0)
        return NULL;
    s = QP_STATE(ctx);
    buf = qp_jsi_read_file(ctx, filename, &len);
    if (!buf)
        return NULL;
    slash = strrchr(filename, '/');
    base = slash ? slash + 1 : filename;
    blen = strlen(base);
    if (blen >= 3 && !strcmp(base + blen - 3, ".py"))
        blen -= 3;
    if (blen == 8 && !memcmp(base, "__init__", 8) && slash) {
        /* package: the name of the directory */
        const char *p = slash;
        while (p > filename && p[-1] != '/')
            p--;
        base = p;
        blen = (size_t)(slash - p);
    }
    name = qp_str_from_utf8(ctx, base, blen);
    if (JS_IsException(name))
        goto fail;
    mod = qp_module_new(ctx, name);
    if (JS_IsException(mod))
        goto fail;
    d = qp_module_get_dict(mod);
    file = qp_str_from_cstr(ctx, filename);
    if (JS_IsException(file) || QP_IS_NULL(d) ||
        qp_dict_set_item_atom(ctx, d, QP_ATOM(ctx, __file__), file) < 0)
        goto fail;
    r = qp_dict_contains(ctx, s->modules, name);
    if (r < 0)
        goto fail;
    if (r == 0) {
        if (qp_dict_set_item(ctx, s->modules, name, mod) < 0)
            goto fail;
        registered = 1;
    }
    if (qp_jsi_add_sys_path(ctx, slash ? filename : "", slash ? (size_t)(slash - filename) : 0) < 0)
        goto fail;

    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, buf, len, filename, QP_NULL, QP_INPUT_FILE, 0, -1, &err);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        goto fail;
    }
    qp_error_clear(&err);
    res = qp_run_code(ctx, bc, d, QP_NULL);
    if (JS_IsException(res))
        goto fail;
    JS_FreeValue(ctx, res);

    names = qp_jsi_export_names(ctx, d);
    if (JS_IsException(names))
        goto fail;
    m = JS_NewCModule(ctx, filename, qp_jsi_py_module_init);
    if (!m)
        goto fail;
    pos = 0;
    while (qp_dict_next(names, &pos, &k, &v)) {
        cname = JS_ToCString(ctx, k);
        if (!cname)
            goto fail_m;
        r = strcmp(cname, "default") ? JS_AddModuleExport(ctx, m, cname) : 0;
        JS_FreeCString(ctx, cname);
        if (r < 0)
            goto fail_m;
    }
    if (JS_AddModuleExport(ctx, m, "default") < 0 ||
        JS_SetModulePrivateValue(ctx, m, JS_DupValue(ctx, mod)) < 0)
        goto fail_m;
    goto done;
 fail_m:
    /* an incomplete module definition must not be found by later imports */
    list_del(&m->link);
    m->link.prev = NULL;
    m->link.next = NULL;
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_MODULE, m));
    m = NULL;
 fail:
    if (registered) {
        JSValue exc = JS_GetException(ctx);
        if (qp_dict_del_item(ctx, s->modules, name) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_Throw(ctx, exc);
    }
 done:
    js_free(ctx, buf);
    JS_FreeValue(ctx, names);
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    return m;
}

/* ========================================================================= */
/* The `js` builtin module                                                    */
/* ========================================================================= */

/* js.__getattr__(name) (PEP 562): globalThis[name] */
static JSValue qp_jsmod_getattr(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    JSAtom atom;
    JSValue v;
    int r;

    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "attribute name must be string, not '%T'", args[0]);
    atom = qp_str_to_atom(ctx, args[0]);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_HasProperty(ctx, ctx->global_obj, atom);
    if (r == 0)
        v = qp_raise(ctx, QP_EXC(ctx, AttributeError), "module 'js' has no attribute %R", args[0]);
    else if (r < 0)
        v = JS_EXCEPTION;
    else
        v = JS_GetProperty(ctx, ctx->global_obj, atom);
    JS_FreeAtom(ctx, atom);
    return v;
}

static JSValue qp_jsi_return_data(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv, int magic, JSValue *data)
{
    return JS_DupValue(ctx, data[0]);
}

/* Read a settled promise: its value, or throw its reason (the rejection is
   marked handled so that hosts do not report it). */
static JSValue qp_jsi_promise_take(JSContext *ctx, JSValueConst promise)
{
    JSValue r, fns[2];
    if (JS_PromiseState(ctx, (JSValue)promise) == JS_PROMISE_FULFILLED)
        return JS_PromiseResult(ctx, (JSValue)promise);
    fns[0] = JS_UNDEFINED;
    fns[1] = JS_NewCFunctionData(ctx, qp_jsi_return_data, 1, 0, 1, (JSValueConst *)&fns[0]);
    if (JS_IsException(fns[1]))
        return fns[1];
    r = js_promise_then(ctx, promise, 2, (JSValueConst *)fns);
    JS_FreeValue(ctx, fns[1]);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    return JS_Throw(ctx, JS_PromiseResult(ctx, (JSValue)promise));
}

/* js.import_module(specifier): load, link and evaluate an ES module
   (relative specifiers resolve against the calling Python file) and return
   its namespace object; for a module using top-level await, a Promise of
   the namespace instead (`await js.import_module(...)`). */
static JSValue qp_jsmod_import_module(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    const char *spec, *base = NULL;
    JSAtom base_atom;
    JSModuleDef *m;
    JSValue promise, ns, fn, ret;

    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "import_module() argument must be str, not '%T'", args[0]);
    if (!qp_jsi_state(ctx))
        return JS_EXCEPTION;
    spec = JS_ToCString(ctx, args[0]);
    if (!spec)
        return JS_EXCEPTION;
    base_atom = JS_GetScriptOrModuleName(ctx, 0);
    if (base_atom != JS_ATOM_NULL) {
        base = JS_AtomToCString(ctx, base_atom);
        JS_FreeAtom(ctx, base_atom);
    }
    m = js_host_resolve_imported_module(ctx, base ? base : "", spec, JS_UNDEFINED);
    JS_FreeCString(ctx, spec);
    if (base)
        JS_FreeCString(ctx, base);
    if (!m)
        return JS_EXCEPTION;
    if (js_resolve_module(ctx, m) < 0) {
        js_free_modules(ctx, JS_FREE_MODULE_NOT_RESOLVED);
        return JS_EXCEPTION;
    }
    promise = JS_EvalFunction(ctx, JS_NewModuleValue(ctx, m));
    if (JS_IsException(promise))
        return promise;
    ns = JS_GetModuleNamespace(ctx, m);
    if (JS_IsException(ns)) {
        JS_FreeValue(ctx, promise);
        return ns;
    }
    if (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
        JSValue fns[2];
        fn = JS_NewCFunctionData(ctx, qp_jsi_return_data, 0, 0, 1, (JSValueConst *)&ns);
        fns[0] = fn;
        fns[1] = JS_UNDEFINED;  /* js_promise_then reads both arguments */
        ret = JS_IsException(fn) ? fn : js_promise_then(ctx, promise, 2, (JSValueConst *)fns);
        JS_FreeValue(ctx, fn);
    } else {
        ret = qp_jsi_promise_take(ctx, promise);
        if (!JS_IsException(ret)) {
            JS_FreeValue(ctx, ret);
            ret = JS_DupValue(ctx, ns);
        }
    }
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, promise);
    return ret;
}

static int qp_jsmod_depth(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                          JSValueConst kwnames, JSValueConst *pobj, int *pdepth)
{
    static const char *const kw[] = { "", "depth", NULL };
    QPArgSpec spec = { fname, kw, 1, 2, 0 };
    JSValueConst out[2];
    *pdepth = -1;
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return -1;
    *pobj = out[0];
    if (!QP_IS_NULL(out[1]) && qp_arg_int(ctx, out[1], pdepth) < 0)
        return -1;
    return 0;
}

/* js.to_js(obj, /, depth=-1) */
static JSValue qp_jsmod_to_js(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                              JSValueConst kwnames, int magic)
{
    JSValueConst obj;
    int depth;
    if (qp_jsmod_depth(ctx, "to_js", args, nargs, kwnames, &obj, &depth) < 0)
        return JS_EXCEPTION;
    return qp_jsi_to_js(ctx, obj, depth);
}

/* js.to_py(obj, /, depth=-1) */
static JSValue qp_jsmod_to_py(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                              JSValueConst kwnames, int magic)
{
    JSValueConst obj;
    int depth;
    if (qp_jsmod_depth(ctx, "to_py", args, nargs, kwnames, &obj, &depth) < 0)
        return JS_EXCEPTION;
    return qp_jsi_to_py(ctx, obj, depth);
}

/* js.create_task(coro): schedule the coroutine on the job queue; returns
   its Promise (the same one for every call on the same coroutine). */
static JSValue qp_jsmod_create_task(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_jsi_task_for(ctx, args[0]);
}

/* js.run(awaitable): drive the QuickJS job queue until the coroutine /
   Promise settles; return its value or raise its exception (asyncio.run
   for the job queue).  Host events (timers, I/O) are not served: an
   awaitable that waits for one raises RuntimeError when the queue runs dry. */
static JSValue qp_jsmod_run(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                            JSValueConst kwnames, int magic)
{
    JSValueConst aw = args[0];
    JSValue promise, ret;
    JSContext *jctx;
    int r;

    if (qp_coro_check(aw)) {
        promise = qp_jsi_task_for(ctx, aw);
    } else if (qp_jsi_is_thenable(ctx, aw)) {
        promise = js_promise_resolve(ctx, ctx->promise_ctor, 1, &aw, 0);
    } else {
        return qp_raise_type_error(ctx, "run() expected a coroutine or a Promise, got %R", aw);
    }
    if (JS_IsException(promise))
        return promise;
    while (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
        r = JS_ExecutePendingJob(ctx->rt, &jctx);
        if (r < 0) {
            if (jctx && jctx != ctx)
                JS_Throw(ctx, JS_GetException(jctx));
            JS_FreeValue(ctx, promise);
            return JS_EXCEPTION;
        }
        if (r == 0) {
            JS_FreeValue(ctx, promise);
            return qp_raise_runtime_error(ctx, "run(): the job queue is empty but the awaitable "
                                          "is still pending (it waits for a host event)");
        }
    }
    ret = qp_jsi_promise_take(ctx, promise);
    JS_FreeValue(ctx, promise);
    return ret;
}

static const QPMethodDef qp_jsmod_methods[] = {
    { "__getattr__", qp_jsmod_getattr, QP_METH_O, 0,
      "__getattr__($module, name, /)\n--\n\nglobalThis[name]." },
    { "import_module", qp_jsmod_import_module, QP_METH_O, 0,
      "import_module($module, specifier, /)\n--\n\n"
      "Import an ES module and return its namespace (a Promise of it when\n"
      "the module uses top-level await)." },
    { "to_js", qp_jsmod_to_js, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "to_js($module, obj, /, depth=-1)\n--\n\n"
      "Deep copy into JavaScript containers: list/tuple -> Array, dict -> object,\n"
      "set -> Set, bytes -> Uint8Array." },
    { "to_py", qp_jsmod_to_py, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "to_py($module, obj, /, depth=-1)\n--\n\n"
      "Deep copy into Python containers: Array -> list, object -> dict,\n"
      "Map -> dict, Set -> set." },
    { "create_task", qp_jsmod_create_task, QP_METH_O, 0,
      "create_task($module, coro, /)\n--\n\n"
      "Run the coroutine as a Task on the job queue; return its Promise." },
    { "run", qp_jsmod_run, QP_METH_O, 0,
      "run($module, awaitable, /)\n--\n\n"
      "Run the job queue until the coroutine or Promise settles; return its result." },
    { NULL }
};

static int qp_jsmod_add(JSContext *ctx, JSValueConst module, const char *name, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;
    if (a == JS_ATOM_NULL)
        return -1;
    r = qp_setattr(ctx, module, a, v);
    JS_FreeAtom(ctx, a);
    return r;
}

static int qp_jsmod_exec(JSContext *ctx, JSValueConst module)
{
    if (!qp_jsi_state(ctx))
        return -1;
    if (qp_jsmod_add(ctx, module, "JsObject", QP_TYPE_VALUE(QP_TYPE(ctx, JsObject))) < 0 ||
        qp_jsmod_add(ctx, module, "JsError", QP_TYPE_VALUE(QP_EXC(ctx, JsError))) < 0 ||
        qp_jsmod_add(ctx, module, "globalThis", ctx->global_obj) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef_js = {
    "js",
    "Access to JavaScript: globalThis names are module attributes\n"
    "(`from js import console`), JsObject is the type of JavaScript objects,\n"
    "JsError the exception raised for JavaScript throws.",
    qp_jsmod_methods,
    qp_jsmod_exec,
};
