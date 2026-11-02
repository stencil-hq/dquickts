/*
 * dquickts Python: raising exceptions, exception state, matching, the
 * raise statement, unraisable reports, warnings, name suggestions and the
 * uncaught-exception path of the launcher.
 *
 * Derived from CPython Python/errors.c, Python/ceval.c (do_raise,
 * _PyEval_CheckExceptTypeValid), Python/_warnings.c, Python/suggestions.c
 * and Python/pythonrun.c (_Py_HandleSystemExitAndKeyboardInterrupt,
 * _PyErr_PrintEx) (PSF-2.0).
 *
 * Part of the quickjs.c translation unit: everything is static.
 */

/* ---- exceptions.c payload accessors (unit i, M2Exc) --------------------- */
static JSValue qp_exc_context(JSValueConst exc);   /* borrowed, QP_NULL if unset */
static JSValue qp_exc_cause(JSValueConst exc);     /* borrowed, QP_NULL if unset */
static BOOL qp_exc_suppress_context(JSValueConst exc);
static void qp_exc_set_context(JSContext *ctx, JSValueConst exc, JSValue context);
static void qp_exc_set_cause(JSContext *ctx, JSValueConst exc, JSValue cause);
static JSValue qp_exc_traceback(JSValueConst exc); /* borrowed: traceback or None */
static void qp_exc_set_traceback_raw(JSContext *ctx, JSValueConst exc, JSValue tb);
static JSValue qp_exc_args(JSValueConst exc);      /* borrowed args tuple */

/* ---- sysmodule.c (unit k, M2Sys) ---------------------------------------- */
static JSValue qp_unraisable_hook_args_new(JSContext *ctx, JSValueConst exc_type,
                                           JSValueConst exc_value, JSValueConst exc_tb,
                                           JSValueConst err_msg, JSValueConst obj);

/* ---- traceback.c (this unit) -------------------------------------------- */
static JSValue qp_tb_new_internal(JSContext *ctx, JSValueConst next, JSValueConst func,
                                  uint32_t pc, JSValueConst self_hint);

/* ---- this file, used by other units ------------------------------------- */
static void qp_exc_chain_context(JSContext *ctx, JSValueConst value);
static JSValue qp_calculate_suggestions(JSContext *ctx, JSValueConst dir_list, JSValueConst name);
static int qp_unraisable_default_hook(JSContext *ctx, JSValueConst exc_type,
                                      JSValueConst exc_value, JSValueConst exc_tb,
                                      JSValueConst err_msg, JSValueConst obj);
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);
static int qp_err_print_uncaught(JSContext *ctx, int *pkbint);
static int qp_linecache_register(JSContext *ctx, const char *filename, const char *src, size_t len);
static int qp_file_write_str(JSContext *ctx, JSValueConst file, JSValueConst str);
static int qp_file_write_cstr(JSContext *ctx, JSValueConst file, const char *s);
static void qp_file_flush(JSContext *ctx, JSValueConst file);
static JSValue qp_err_private_dict(JSContext *ctx, const char *key);

#define QP_IS_NONE_OR_NULL(v) (JS_IsUndefined(v) || QP_IS_NULL(v))
#define QP_ERR_NAMEBUF 256

static inline BOOL qp_err_same(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static inline BOOL qp_err_is_pyexc_obj(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_EXCEPTION;
}

/* Drop any pending exception (PyErr_Clear). */
static void qp_err_clear(JSContext *ctx)
{
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

/* tp_name of a type as a C string (CPython tp->tp_name: the spec name for
   builtin types, __name__ for heap types). */
static const char *qp_err_tp_name(JSContext *ctx, QPType *t, char *buf, int size)
{
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* A Python dict kept per context as a hidden, non-enumerable property of the
   JS global object (warnings once-registry, registered sources).  Borrowed
   is not possible with JS properties: returns a new reference, JS_EXCEPTION
   on failure. */
static JSValue qp_err_private_dict(JSContext *ctx, const char *key)
{
    JSValue d = JS_GetPropertyStr(ctx, ctx->global_obj, key);
    if (JS_IsException(d))
        return d;
    if (JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(d)->class_id == JS_CLASS_PY_DICT)
        return d;
    JS_FreeValue(ctx, d);
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return d;
    if (JS_DefinePropertyValueStr(ctx, ctx->global_obj, key, JS_DupValue(ctx, d),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) {
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    return d;
}

/* ========================================================================= */
/* Handled-exception stack                                                    */
/* ========================================================================= */

/* _PyErr_GetTopmostException: an item whose exc_value is unset (QP_NULL or
   None) defers to the previous one. */
static QPExcStackItem *qp_exc_topmost(JSContext *ctx)
{
    QPExcStackItem *it = QP_STATE(ctx)->exc_info;
    while (QP_IS_NONE_OR_NULL(it->exc_value) && it->previous != NULL)
        it = it->previous;
    return it;
}

static JSValue qp_exc_info_get(JSContext *ctx)
{
    QPExcStackItem *it = qp_exc_topmost(ctx);
    if (QP_IS_NONE_OR_NULL(it->exc_value))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, it->exc_value);
}

static void qp_exc_info_push(JSContext *ctx, JSValueConst exc, JSValue *prev)
{
    QPExcStackItem *it = QP_STATE(ctx)->exc_info;
    JSValue old = it->exc_value;
    *prev = QP_IS_NULL(old) ? JS_UNDEFINED : old;
    it->exc_value = JS_DupValue(ctx, exc);
}

static void qp_exc_info_pop(JSContext *ctx, JSValue prev)
{
    QPExcStackItem *it = QP_STATE(ctx)->exc_info;
    JSValue old = it->exc_value;
    it->exc_value = JS_IsUndefined(prev) ? QP_NULL : prev;
    JS_FreeValue(ctx, old);
}

/* Implicit exception chaining of _PyErr_SetObject: value.__context__ = the
   handled exception, breaking cycles through the context chain (Floyd's
   algorithm as in CPython). */
static void qp_exc_chain_context(JSContext *ctx, JSValueConst value)
{
    QPExcStackItem *it = qp_exc_topmost(ctx);
    JSValueConst exc_value = it->exc_value, o, slow_o, context;
    int toggle = 0;

    if (QP_IS_NONE_OR_NULL(exc_value) || qp_err_same(exc_value, value) ||
        !qp_err_is_pyexc_obj(value))
        return;
    o = exc_value;
    slow_o = o;
    while (qp_err_is_pyexc_obj(o) && !QP_IS_NONE_OR_NULL(context = qp_exc_context(o))) {
        if (qp_err_same(context, value)) {
            qp_exc_set_context(ctx, o, QP_NULL);
            break;
        }
        o = context;
        if (qp_err_same(o, slow_o))
            break;      /* pre-existing cycle */
        if (toggle) {
            slow_o = qp_exc_context(slow_o);
            if (QP_IS_NONE_OR_NULL(slow_o))
                break;
        }
        toggle = !toggle;
    }
    qp_exc_set_context(ctx, value, JS_DupValue(ctx, exc_value));
}

/* ========================================================================= */
/* Raising                                                                    */
/* ========================================================================= */

/* PyErr_SetRaisedException for an instance: chain __context__ and throw. */
static JSValue qp_raise_object(JSContext *ctx, JSValue exc)
{
    qp_exc_chain_context(ctx, exc);
    return JS_Throw(ctx, exc);
}

/* _PyErr_CreateException: type(), type(*tuple) or type(value). */
static JSValue qp_err_create_exception(JSContext *ctx, QPType *type, JSValueConst value)
{
    JSValue exc;
    char buf[QP_ERR_NAMEBUF];

    if (QP_IS_NONE_OR_NULL(value))
        exc = qp_exc_new(ctx, type, 0, NULL);
    else if (qp_tuple_check(ctx, value))
        exc = qp_exc_new(ctx, type, (int)QP_TUPLE_SIZE(value), (JSValueConst *)QP_TUPLE_ITEMS(value));
    else
        exc = qp_exc_new(ctx, type, 1, &value);
    if (JS_IsException(exc))
        return exc;
    if (!qp_exc_check(ctx, exc)) {
        QPType *rt = qp_type_of(ctx, exc);
        JS_FreeValue(ctx, exc);
        qp_raise_type_error(ctx, "calling %R should have returned an instance of "
                            "BaseException, not %s", QP_TYPE_VALUE(type),
                            qp_err_tp_name(ctx, rt, buf, sizeof(buf)));
        return JS_EXCEPTION;
    }
    return exc;
}

/* get_normalization_failure_note + _PyException_AddNote on the pending
   exception (errors ignored). */
static void qp_err_add_normalization_note(JSContext *ctx, QPType *type, JSValueConst value)
{
    JSValue exc, args, note, r;
    char buf[QP_ERR_NAMEBUF];
    const char *tpname = qp_err_tp_name(ctx, type, buf, sizeof(buf));

    exc = JS_GetException(ctx);
    args = qp_repr(ctx, QP_IS_NULL(value) ? JS_UNDEFINED : value);
    if (JS_IsException(args)) {
        qp_err_clear(ctx);
        args = qp_str_from_cstr(ctx, "<unknown>");
    }
    if (JS_IsException(args)) {
        qp_err_clear(ctx);
        note = qp_str_from_format(ctx, "Normalization failed: type=%s", tpname);
    } else {
        note = qp_str_from_format(ctx, "Normalization failed: type=%s args=%S", tpname, args);
        JS_FreeValue(ctx, args);
    }
    if (!JS_IsException(note) && qp_err_is_pyexc_obj(exc)) {
        JSValueConst a[1] = { note };
        r = qp_call_method(ctx, exc, QP_ATOM(ctx, add_note), a, 1, QP_NULL);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, note);
    qp_err_clear(ctx);
    JS_Throw(ctx, exc);
}

/* _PyErr_SetObject */
static JSValue qp_raise_value(JSContext *ctx, QPType *type, JSValueConst value)
{
    JSValue exc;
    int is_subclass = 0;

    if (!qp_exc_type_check(QP_TYPE_VALUE(type))) {
        return qp_raise_system_error(ctx, "_PyErr_SetObject: exception %R is not a "
                                     "BaseException subclass", QP_TYPE_VALUE(type));
    }
    if (!QP_IS_NULL(value) && qp_exc_check(ctx, value)) {
        QPType *vt = qp_type_of(ctx, value);
        if (vt == type) {
            is_subclass = 1;
        } else {
            is_subclass = qp_issubclass(ctx, QP_TYPE_VALUE(vt), QP_TYPE_VALUE(type));
            if (is_subclass < 0)
                return JS_EXCEPTION;
        }
    }
    if (is_subclass) {
        exc = JS_DupValue(ctx, value);
    } else {
        qp_err_clear(ctx);
        exc = qp_err_create_exception(ctx, type, value);
        if (JS_IsException(exc)) {
            if (JS_HasException(ctx))
                qp_err_add_normalization_note(ctx, type, value);
            return JS_EXCEPTION;
        }
    }
    return qp_raise_object(ctx, exc);
}

static JSValue qp_raise(JSContext *ctx, QPType *type, const char *fmt, ...)
{
    va_list ap;
    JSValue msg;

    va_start(ap, fmt);
    msg = qp_str_from_format_v(ctx, fmt, ap);
    va_end(ap);
    if (JS_IsException(msg))
        return JS_EXCEPTION;
    qp_raise_value(ctx, type, msg);
    JS_FreeValue(ctx, msg);
    return JS_EXCEPTION;
}

/* Raise type(msg) with attributes set by name (AttributeError.name/obj,
   NameError.name); setattr failures are ignored like CPython. */
static JSValue qp_err_raise_with_attrs(JSContext *ctx, QPType *type, JSValue msg,
                                       JSAtom a1, JSValueConst v1, JSAtom a2, JSValueConst v2)
{
    JSValue exc;

    if (JS_IsException(msg))
        return JS_EXCEPTION;
    exc = qp_err_create_exception(ctx, type, msg);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return JS_EXCEPTION;
    if (a1 != JS_ATOM_NULL && qp_setattr(ctx, exc, a1, v1) < 0)
        qp_err_clear(ctx);
    if (a2 != JS_ATOM_NULL && qp_setattr(ctx, exc, a2, v2) < 0)
        qp_err_clear(ctx);
    return qp_raise_object(ctx, exc);
}

static JSValue qp_raise_key_error(JSContext *ctx, JSValueConst key)
{
    JSValue exc = qp_exc_new(ctx, QP_EXC(ctx, KeyError), 1, &key);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* PyObject_GenericGetAttr / type_getattro / module_getattro messages
   (generic: always the PyObject_GenericGetAttr one), then
   set_attribute_error_context (name, obj). */
static JSValue qp_raise_attribute_error_ex(JSContext *ctx, JSValueConst obj, JSAtom name,
                                           BOOL generic)
{
    char buf[QP_ERR_NAMEBUF];
    JSValue msg, name_str, res;
    QPType *t = generic ? NULL : qp_type_from_value(obj);

    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    if (t) {
        msg = qp_str_from_format(ctx, "type object '%.100s' has no attribute '%U'",
                                 qp_err_tp_name(ctx, t, buf, sizeof(buf)), name_str);
    } else if (!generic && qp_module_check(ctx, obj)) {
        JSValue modname = QP_NULL;
        JSValue d = qp_module_get_dict(obj);
        int r = 0;
        if (JS_VALUE_GET_TAG(d) == JS_TAG_OBJECT)
            r = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __name__), &modname);
        if (r < 0)
            qp_err_clear(ctx);
        if (r > 0 && qp_str_check(ctx, modname))
            msg = qp_str_from_format(ctx, "module '%U' has no attribute '%U'", modname, name_str);
        else
            msg = qp_str_from_format(ctx, "module has no attribute '%U'", name_str);
        if (r > 0)
            JS_FreeValue(ctx, modname);
    } else {
        msg = qp_str_from_format(ctx, "'%.100s' object has no attribute '%U'",
                                 qp_err_tp_name(ctx, qp_type_of(ctx, obj), buf, sizeof(buf)),
                                 name_str);
    }
    res = qp_err_raise_with_attrs(ctx, QP_EXC(ctx, AttributeError), msg,
                                  QP_ATOM(ctx, name), name_str, QP_ATOM(ctx, obj), obj);
    JS_FreeValue(ctx, name_str);
    return res;
}

static JSValue qp_raise_attribute_error(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    return qp_raise_attribute_error_ex(ctx, obj, name, FALSE);
}

/* NAME_ERROR_MSG + _PyEval_FormatExcCheckArg (sets NameError.name) */
static JSValue qp_raise_name_error(JSContext *ctx, JSAtom name)
{
    char buf[QP_ERR_NAMEBUF];
    JSValue name_str, msg, res;

    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    msg = qp_str_from_format(ctx, "name '%.200s' is not defined",
                             JS_AtomGetStr(ctx, buf, sizeof(buf), name));
    res = qp_err_raise_with_attrs(ctx, QP_EXC(ctx, NameError), msg,
                                  QP_ATOM(ctx, name), name_str, JS_ATOM_NULL, JS_UNDEFINED);
    JS_FreeValue(ctx, name_str);
    return res;
}

/* _PyEval_FormatExcUnbound: only the free-variable NameError gets .name
   (UnboundLocalError is not PyExc_NameError itself). */
static JSValue qp_raise_unbound(JSContext *ctx, JSAtom name, BOOL is_free)
{
    char buf[QP_ERR_NAMEBUF];
    const char *s = JS_AtomGetStr(ctx, buf, sizeof(buf), name);
    JSValue name_str, msg, res;

    if (!is_free)
        return qp_raise(ctx, QP_EXC(ctx, UnboundLocalError),
                        "cannot access local variable '%s' where it is not "
                        "associated with a value", s);
    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    msg = qp_str_from_format(ctx, "cannot access free variable '%s' where it is not "
                             "associated with a value in enclosing scope", s);
    res = qp_err_raise_with_attrs(ctx, QP_EXC(ctx, NameError), msg,
                                  QP_ATOM(ctx, name), name_str, JS_ATOM_NULL, JS_UNDEFINED);
    JS_FreeValue(ctx, name_str);
    return res;
}

/* Generator return: None -> StopIteration() (PyErr_SetNone), anything else
   -> StopIteration(value) (_PyGen_SetStopIterationValue, no tuple
   unpacking). */
static JSValue qp_raise_stop_iteration(JSContext *ctx, JSValueConst value)
{
    JSValue exc;
    if (QP_IS_NONE_OR_NULL(value))
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 0, NULL);
    else
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 1, &value);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* PyErr_NoMemory.  If even the MemoryError instance cannot be allocated,
   the QuickJS out-of-memory error stays pending; qp_exc_normalize turns it
   into a MemoryError later. */
static JSValue qp_raise_memory_error(JSContext *ctx)
{
    JSValue exc = qp_exc_new(ctx, QP_EXC(ctx, MemoryError), 0, NULL);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

static JSValue qp_raise_recursion_error(JSContext *ctx)
{
    return qp_raise(ctx, QP_EXC(ctx, RecursionError), "maximum recursion depth exceeded");
}

/* ========================================================================= */
/* Converting thrown values                                                   */
/* ========================================================================= */

/* message of a JS InternalError object, or NULL (caller frees with
   JS_FreeCString). */
static const char *qp_err_js_internal_message(JSContext *ctx, JSValueConst v)
{
    JSObject *p;
    JSValue msg;
    const char *s;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_ERROR || !p->shape->proto ||
        JS_VALUE_GET_TAG(ctx->native_error_proto[JS_INTERNAL_ERROR]) != JS_TAG_OBJECT ||
        p->shape->proto != JS_VALUE_GET_OBJ(ctx->native_error_proto[JS_INTERNAL_ERROR]))
        return NULL;
    msg = JS_GetProperty(ctx, v, JS_ATOM_message);
    if (JS_IsException(msg)) {
        qp_err_clear(ctx);
        return NULL;
    }
    if (!JS_IsString(msg)) {
        JS_FreeValue(ctx, msg);
        return NULL;
    }
    s = JS_ToCString(ctx, msg);
    JS_FreeValue(ctx, msg);
    if (!s)
        qp_err_clear(ctx);
    return s;
}

static JSValue qp_exc_normalize(JSContext *ctx, JSValue thrown)
{
    const char *m;
    JSValue exc, msg, atom_val;
    JSAtom atom;

    if (qp_err_is_pyexc_obj(thrown))
        return thrown;
    if (QP_IS_NULL(thrown)) {
        /* nothing was thrown: CPython's "error return without exception set" */
        msg = qp_str_from_cstr(ctx, "error return without exception set");
        if (JS_IsException(msg))
            goto fail;
        exc = qp_exc_new(ctx, QP_EXC(ctx, SystemError), 1, (JSValueConst *)&msg);
        JS_FreeValue(ctx, msg);
        if (JS_IsException(exc))
            goto fail;
        return exc;
    }
    m = qp_err_js_internal_message(ctx, thrown);
    if (m) {
        int kind = 0;
        if (!strcmp(m, "stack overflow") || !strcmp(m, "too much recursion"))
            kind = 1;
        else if (!strcmp(m, "out of memory"))
            kind = 2;
        JS_FreeCString(ctx, m);
        if (kind == 1) {
            msg = qp_str_from_cstr(ctx, "maximum recursion depth exceeded");
            if (JS_IsException(msg))
                goto fail;
            exc = qp_exc_new(ctx, QP_EXC(ctx, RecursionError), 1, (JSValueConst *)&msg);
            JS_FreeValue(ctx, msg);
            if (JS_IsException(exc))
                goto fail;
            JS_FreeValue(ctx, thrown);
            return exc;
        }
        if (kind == 2) {
            exc = qp_exc_new(ctx, QP_EXC(ctx, MemoryError), 0, NULL);
            if (JS_IsException(exc))
                goto fail;
            JS_FreeValue(ctx, thrown);
            return exc;
        }
    }
    /* any other JS value: js.JsError(str(value)) with .js_error = value */
    msg = JS_ToString(ctx, thrown);
    if (JS_IsException(msg)) {
        qp_err_clear(ctx);
        msg = qp_str_from_cstr(ctx, "<unprintable JS value>");
        if (JS_IsException(msg))
            goto fail;
    }
    exc = qp_exc_new(ctx, QP_EXC(ctx, JsError), 1, (JSValueConst *)&msg);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        goto fail;
    atom = JS_NewAtom(ctx, "js_error");
    if (atom != JS_ATOM_NULL) {
        atom_val = thrown;
        if (qp_setattr(ctx, exc, atom, atom_val) < 0)
            qp_err_clear(ctx);
        JS_FreeAtom(ctx, atom);
    } else {
        qp_err_clear(ctx);
    }
    JS_FreeValue(ctx, thrown);
    return exc;
 fail:
    /* could not build the Python exception (out of memory): keep the new
       pending error if it is a Python exception, else the original value */
    exc = JS_GetException(ctx);
    if (qp_err_is_pyexc_obj(exc)) {
        JS_FreeValue(ctx, thrown);
        return exc;
    }
    JS_FreeValue(ctx, exc);
    return thrown;
}

/* PyErr_GetRaisedException.  Uncatchable values (interrupts) are returned
   unconverted. */
static JSValue qp_exc_fetch(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    if (!JS_HasException(ctx))
        return QP_NULL;
    if (rt->current_exception_is_uncatchable)
        return JS_GetException(ctx);
    return qp_exc_normalize(ctx, JS_GetException(ctx));
}

/* Normalize the pending exception in place (not uncatchable ones). */
static void qp_err_normalize_pending(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    if (!JS_HasException(ctx) || rt->current_exception_is_uncatchable ||
        qp_err_is_pyexc_obj(rt->current_exception))
        return;
    JS_Throw(ctx, qp_exc_normalize(ctx, JS_GetException(ctx)));
}

/* ========================================================================= */
/* Matching                                                                   */
/* ========================================================================= */

static BOOL qp_exc_given_matches(JSContext *ctx, JSValueConst err, JSValueConst exc)
{
    QPType *et, *xt;

    if (QP_IS_NULL(err) || QP_IS_NULL(exc))
        return FALSE;
    if (qp_tuple_check(ctx, exc)) {
        Py_ssize_t i, n;
        if (js_check_stack_overflow(ctx->rt, 0)) {
            JSValue saved = JS_HasException(ctx) ? JS_GetException(ctx) : QP_NULL;
            JSValue rec;
            qp_raise(ctx, QP_EXC(ctx, RecursionError),
                     "maximum recursion depth exceeded while checking exception tuple");
            rec = qp_exc_fetch(ctx);
            qp_exc_write_unraisable_msg(ctx, rec, "Exception ignored while checking "
                                        "exception tuple", QP_NULL);
            JS_FreeValue(ctx, rec);
            if (!QP_IS_NULL(saved))
                JS_Throw(ctx, saved);
            return FALSE;
        }
        n = QP_TUPLE_SIZE(exc);
        for (i = 0; i < n; i++) {
            if (qp_exc_given_matches(ctx, err, QP_TUPLE_ITEMS(exc)[i]))
                return TRUE;
        }
        return FALSE;
    }
    if (qp_exc_check(ctx, err))
        et = qp_type_of(ctx, err);
    else
        et = qp_type_from_value(err);
    xt = qp_type_from_value(exc);
    if (et && xt && qp_exc_type_check(QP_TYPE_VALUE(et)) && qp_exc_type_check(exc))
        return qp_type_is_subtype(et, xt);
    if (et)
        return qp_err_same(QP_TYPE_VALUE(et), exc);
    return qp_err_same(err, exc);
}

static BOOL qp_exc_pending_matches(JSContext *ctx, QPType *type)
{
    JSRuntime *rt = ctx->rt;
    if (!JS_HasException(ctx) || rt->current_exception_is_uncatchable)
        return FALSE;
    qp_err_normalize_pending(ctx);
    return qp_exc_given_matches(ctx, rt->current_exception, QP_TYPE_VALUE(type));
}

/* _PyEval_CheckExceptTypeValid + PyErr_GivenExceptionMatches */
static int qp_exc_check_match(JSContext *ctx, JSValueConst exc, JSValueConst type_or_tuple)
{
    if (qp_tuple_check(ctx, type_or_tuple)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(type_or_tuple);
        for (i = 0; i < n; i++) {
            if (!qp_exc_type_check(QP_TUPLE_ITEMS(type_or_tuple)[i]))
                goto bad;
        }
    } else if (!qp_exc_type_check(type_or_tuple)) {
        goto bad;
    }
    return qp_exc_given_matches(ctx, exc, type_or_tuple);
 bad:
    qp_raise_type_error(ctx, "catching classes that do not inherit from "
                        "BaseException is not allowed");
    return -1;
}

/* ========================================================================= */
/* The raise statement (CPython do_raise)                                     */
/* ========================================================================= */

static int qp_do_raise(JSContext *ctx, JSValueConst exc, JSValueConst cause)
{
    JSValue value, fixed_cause = QP_NULL;

    if (QP_IS_NULL(exc)) {
        /* bare raise: re-raise without a new traceback entry */
        QPExcStackItem *it = qp_exc_topmost(ctx);
        if (QP_IS_NONE_OR_NULL(it->exc_value)) {
            qp_raise_runtime_error(ctx, "No active exception to reraise");
            return -1;
        }
        JS_Throw(ctx, JS_DupValue(ctx, it->exc_value));
        QP_STATE(ctx)->reraise = 1;
        return -1;
    }
    if (qp_exc_type_check(exc)) {
        value = qp_call0(ctx, exc);
        if (JS_IsException(value))
            return -1;
        if (!qp_exc_check(ctx, value)) {
            qp_raise_type_error(ctx, "calling %R should have returned an instance of "
                                "BaseException, not %R", exc,
                                QP_TYPE_VALUE(qp_type_of(ctx, value)));
            JS_FreeValue(ctx, value);
            return -1;
        }
    } else if (qp_exc_check(ctx, exc)) {
        value = JS_DupValue(ctx, exc);
    } else {
        qp_raise_type_error(ctx, "exceptions must derive from BaseException");
        return -1;
    }
    if (!QP_IS_NULL(cause)) {
        if (qp_exc_type_check(cause)) {
            fixed_cause = qp_call0(ctx, cause);
            if (JS_IsException(fixed_cause))
                goto fail;
            if (!qp_exc_check(ctx, fixed_cause)) {
                qp_raise_type_error(ctx, "calling %R should have returned an instance of "
                                    "BaseException, not %R", cause,
                                    QP_TYPE_VALUE(qp_type_of(ctx, fixed_cause)));
                JS_FreeValue(ctx, fixed_cause);
                goto fail;
            }
        } else if (qp_exc_check(ctx, cause)) {
            fixed_cause = JS_DupValue(ctx, cause);
        } else if (JS_IsUndefined(cause) || JS_IsNull(cause)) {
            fixed_cause = QP_NULL;
        } else {
            qp_raise_type_error(ctx, "exception causes must derive from BaseException");
            goto fail;
        }
        qp_exc_set_cause(ctx, value, fixed_cause);
    }
    qp_raise_object(ctx, value);
    return -1;
 fail:
    JS_FreeValue(ctx, value);
    return -1;
}

/* ========================================================================= */
/* Writing to file objects                                                    */
/* ========================================================================= */

/* Write a str to C stderr as UTF-8 (lone surrogates as \udXXX escapes, like
   the backslashreplace error handler of sys.stderr). */
static void qp_err_write_c_stderr(JSContext *ctx, JSValueConst str)
{
    uint32_t *buf;
    int i, n = JS_ToUTF32String(ctx, &buf, str);
    char out[8];

    if (n < 0) {
        qp_err_clear(ctx);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t c = buf[i];
        if (c >= 0xD800 && c <= 0xDFFF) {
            fprintf(stderr, "\\u%04x", c);
        } else {
            int l = unicode_to_utf8((uint8_t *)out, c);
            fwrite(out, 1, l, stderr);
        }
    }
    js_free(ctx, buf);
}

/* PyFile_WriteObject(str, file, Py_PRINT_RAW) for an exact str; file
   QP_NULL or None writes to C stderr. */
static int qp_file_write_str(JSContext *ctx, JSValueConst file, JSValueConst str)
{
    JSValue r;
    if (QP_IS_NONE_OR_NULL(file)) {
        qp_err_write_c_stderr(ctx, str);
        return 0;
    }
    r = qp_call_method(ctx, file, QP_ATOM(ctx, write), &str, 1, QP_NULL);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_file_write_cstr(JSContext *ctx, JSValueConst file, const char *s)
{
    JSValue str;
    int r;
    if (QP_IS_NONE_OR_NULL(file)) {
        fputs(s, stderr);
        return 0;
    }
    str = qp_str_from_cstr(ctx, s);
    if (JS_IsException(str))
        return -1;
    r = qp_file_write_str(ctx, file, str);
    JS_FreeValue(ctx, str);
    return r;
}

/* PyFile_WriteObject(obj, file, flags): raw = str(obj), else repr(obj). */
static int qp_file_write_object(JSContext *ctx, JSValueConst file, JSValueConst obj, BOOL raw)
{
    JSValue s = raw ? qp_str(ctx, obj) : qp_repr(ctx, obj);
    int r;
    if (JS_IsException(s))
        return -1;
    r = qp_file_write_str(ctx, file, s);
    JS_FreeValue(ctx, s);
    return r;
}

/* _PyFile_Flush, errors ignored. */
static void qp_file_flush(JSContext *ctx, JSValueConst file)
{
    JSValue r;
    if (QP_IS_NONE_OR_NULL(file)) {
        fflush(stderr);
        return;
    }
    r = qp_call_method(ctx, file, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
    if (JS_IsException(r))
        qp_err_clear(ctx);
    else
        JS_FreeValue(ctx, r);
}

/* sys.stderr: new reference, QP_NULL when missing (PySys_GetOptionalAttr). */
static JSValue qp_err_sys_stderr(JSContext *ctx)
{
    return qp_sys_get(ctx, QP_ATOM(ctx, stderr));
}

/* ========================================================================= */
/* Unraisable exceptions                                                      */
/* ========================================================================= */

/* write_unraisable_exc_file + write_unraisable_exc: the default
   sys.unraisablehook.  err_msg/obj may be QP_NULL or None. */
static int qp_unraisable_default_hook(JSContext *ctx, JSValueConst exc_type,
                                      JSValueConst exc_value, JSValueConst exc_tb,
                                      JSValueConst err_msg, JSValueConst obj)
{
    JSValue file = qp_err_sys_stderr(ctx);
    int ret = -1;

    if (QP_IS_NONE_OR_NULL(file)) {
        JS_FreeValue(ctx, file);
        return 0;
    }
    if (!QP_IS_NONE_OR_NULL(obj)) {
        if (!QP_IS_NONE_OR_NULL(err_msg)) {
            if (qp_file_write_object(ctx, file, err_msg, TRUE) < 0 ||
                qp_file_write_cstr(ctx, file, ": ") < 0)
                goto done;
        } else if (qp_file_write_cstr(ctx, file, "Exception ignored in: ") < 0) {
            goto done;
        }
        if (qp_file_write_object(ctx, file, obj, FALSE) < 0) {
            qp_err_clear(ctx);
            if (qp_file_write_cstr(ctx, file, "<object repr() failed>") < 0)
                goto done;
        }
        if (qp_file_write_cstr(ctx, file, "\n") < 0)
            goto done;
    } else if (!QP_IS_NONE_OR_NULL(err_msg)) {
        if (qp_file_write_object(ctx, file, err_msg, TRUE) < 0 ||
            qp_file_write_cstr(ctx, file, ":\n") < 0)
            goto done;
    }
    /* traceback._print_exception_bltin(exc_value, file) */
    if (!QP_IS_NULL(exc_value) && qp_exc_check(ctx, exc_value)) {
        if (qp_exc_display(ctx, exc_value, file) < 0)
            goto done;
    } else if (!QP_IS_NONE_OR_NULL(exc_type)) {
        QPType *t = qp_type_from_value(exc_type);
        if (t) {
            JSValue line;
            char buf[QP_ERR_NAMEBUF];
            if (QP_IS_NONE_OR_NULL(exc_value))
                line = qp_str_from_format(ctx, "%s\n", qp_err_tp_name(ctx, t, buf, sizeof(buf)));
            else
                line = qp_str_from_format(ctx, "%s: %S\n",
                                          qp_err_tp_name(ctx, t, buf, sizeof(buf)), exc_value);
            if (JS_IsException(line) || qp_file_write_str(ctx, file, line) < 0) {
                JS_FreeValue(ctx, line);
                goto done;
            }
            JS_FreeValue(ctx, line);
        }
    }
    qp_file_flush(ctx, file);
    ret = 0;
 done:
    JS_FreeValue(ctx, file);
    return ret;
}

/* format_unraisable_v for a given exception (not pending).  err_msg is a
   C string or NULL ("Exception ignored in"). */
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc_in,
                                        const char *err_msg_str, JSValueConst obj)
{
    JSValue saved, exc, exc_type, tb, err_msg = QP_NULL, hook = QP_NULL, args, res;
    JSValue orig_hook;
    JSAtom atom;

    saved = JS_HasException(ctx) ? JS_GetException(ctx) : QP_NULL;
    if (QP_IS_NULL(exc_in))
        goto end;
    exc = JS_DupValue(ctx, exc_in);
    if (!qp_err_is_pyexc_obj(exc))
        exc = qp_exc_normalize(ctx, exc);
    if (!qp_err_is_pyexc_obj(exc)) {
        JS_FreeValue(ctx, exc);
        goto end;
    }
    exc_type = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, exc)));
    tb = JS_DupValue(ctx, qp_exc_traceback(exc));
    if (JS_IsUndefined(tb)) {
        /* _PyTraceBack_FromFrame(NULL, current frame) */
        struct JSStackFrame *sf = qp_py_frame_at(ctx, 0);
        if (sf && JS_VALUE_GET_TAG(sf->cur_func) == JS_TAG_OBJECT &&
            JS_VALUE_GET_OBJ(sf->cur_func)->class_id == JS_CLASS_BYTECODE_FUNCTION) {
            JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
            uint32_t pc = sf->cur_pc ? (uint32_t)(sf->cur_pc - b->byte_code_buf - 1) : 0;
            tb = qp_tb_new_internal(ctx, JS_UNDEFINED, sf->cur_func, pc, QP_NULL);
            if (JS_IsException(tb)) {
                qp_err_clear(ctx);
                tb = JS_UNDEFINED;
            } else {
                qp_exc_set_traceback_raw(ctx, exc, JS_DupValue(ctx, tb));
            }
        }
    }
    if (err_msg_str) {
        err_msg = qp_str_from_cstr(ctx, err_msg_str);
        if (JS_IsException(err_msg)) {
            qp_err_clear(ctx);
            err_msg = QP_NULL;
        }
    }
    hook = qp_sys_get(ctx, QP_ATOM(ctx, unraisablehook));
    if (JS_IsException(hook)) {
        qp_err_clear(ctx);
        hook = QP_NULL;
    }
    if (QP_IS_NONE_OR_NULL(hook))
        goto default_hook;
    /* the builtin default hook: no need to build UnraisableHookArgs */
    atom = JS_NewAtom(ctx, "__unraisablehook__");
    orig_hook = atom != JS_ATOM_NULL ? qp_sys_get(ctx, atom) : QP_NULL;
    if (atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, atom);
    if (JS_IsException(orig_hook)) {
        qp_err_clear(ctx);
        orig_hook = QP_NULL;
    }
    if (!QP_IS_NULL(orig_hook) && qp_err_same(orig_hook, hook)) {
        JS_FreeValue(ctx, orig_hook);
        goto default_hook;
    }
    JS_FreeValue(ctx, orig_hook);
    args = qp_unraisable_hook_args_new(ctx, exc_type, exc, tb, err_msg,
                                       QP_IS_NULL(obj) ? JS_UNDEFINED : obj);
    if (JS_IsException(args)) {
        JS_FreeValue(ctx, err_msg);
        err_msg = qp_str_from_cstr(ctx, "Exception ignored while building "
                                   "sys.unraisablehook arguments");
        obj = QP_NULL;
        goto hook_error;
    }
    res = qp_call1(ctx, hook, args);
    JS_FreeValue(ctx, args);
    if (!JS_IsException(res)) {
        JS_FreeValue(ctx, res);
        goto done;
    }
    /* sys.unraisablehook failed: log its error using the default hook */
    JS_FreeValue(ctx, err_msg);
    err_msg = qp_str_from_cstr(ctx, "Exception ignored in sys.unraisablehook");
    obj = hook;
 hook_error:
    if (JS_IsException(err_msg))
        err_msg = QP_NULL;
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, exc_type);
    JS_FreeValue(ctx, tb);
    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc)) {
        exc_type = JS_UNDEFINED;
        tb = JS_UNDEFINED;
        exc = JS_UNDEFINED;
    } else {
        exc_type = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, exc)));
        tb = qp_err_is_pyexc_obj(exc) ? JS_DupValue(ctx, qp_exc_traceback(exc)) : JS_UNDEFINED;
    }
 default_hook:
    qp_unraisable_default_hook(ctx, exc_type, exc, tb, err_msg, obj);
 done:
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, exc_type);
    JS_FreeValue(ctx, tb);
    JS_FreeValue(ctx, err_msg);
    JS_FreeValue(ctx, hook);
    qp_err_clear(ctx);
 end:
    if (!QP_IS_NULL(saved))
        JS_Throw(ctx, saved);
}

/* PyErr_WriteUnraisable(obj) for an exception that is not pending (the
   caller fetched it). */
static void qp_exc_write_unraisable(JSContext *ctx, JSValueConst exc, JSValueConst obj)
{
    qp_exc_write_unraisable_msg(ctx, exc, NULL, obj);
}

/* ========================================================================= */
/* Warnings                                                                   */
/* ========================================================================= */

/* The _warnings port (filters, registries, showwarning) lives in
   mod_misc.c; declared here until object.h carries it. */
static int qp_warnings_warn(JSContext *ctx, QPType *category, JSValueConst message,
                            Py_ssize_t stack_level, JSValueConst source);

/* PyErr_WarnFormat: format the message and warn (0, or -1 when the warning
   was turned into an exception or failed). */
static int qp_warn(JSContext *ctx, QPType *category, int stacklevel, const char *fmt, ...)
{
    va_list ap;
    JSValue text;
    int ret;

    va_start(ap, fmt);
    text = qp_str_from_format_v(ctx, fmt, ap);
    va_end(ap);
    if (JS_IsException(text))
        return -1;
    ret = qp_warnings_warn(ctx, category, text, stacklevel, QP_NULL);
    JS_FreeValue(ctx, text);
    return ret;
}

/* Universal-newline line reader of err_programtext: 1 line read, 0 EOF. */
static int qp_err_fgets_univ(FILE *fp, char *buf, size_t size, size_t *plen)
{
    size_t n = 0;
    int c;

    while (n + 1 < size) {
        c = getc(fp);
        if (c == EOF)
            break;
        if (c == '\r') {
            c = getc(fp);
            if (c != '\n' && c != EOF)
                ungetc(c, fp);
            c = '\n';
        }
        buf[n++] = (char)c;
        if (c == '\n')
            break;
    }
    buf[n] = '\0';
    *plen = n;
    return n > 0;
}

/* Source encoding of the coding cookie in the first two lines (PEP 263),
   default UTF-8 (_PyTokenizer_FindEncodingFilename).  `enc` receives a
   NUL-terminated name. */
static void qp_err_find_encoding(FILE *fp, char *enc, size_t size)
{
    char line[1000];
    size_t len;
    int i;

    snprintf(enc, size, "utf-8");
    for (i = 0; i < 2; i++) {
        const char *p, *q;
        if (!qp_err_fgets_univ(fp, line, sizeof(line), &len))
            break;
        p = line;
        if (i == 0 && len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
            p += 3;
        while (*p == ' ' || *p == '\t' || *p == '\f')
            p++;
        if (*p != '#') {
            if (*p == '\n' || *p == '\0')
                continue;   /* blank line: the cookie may be on line 2 */
            break;
        }
        q = strstr(p, "coding");
        if (q && (q[6] == ':' || q[6] == '=')) {
            size_t k = 0;
            q += 7;
            while (*q == ' ' || *q == '\t')
                q++;
            while (k + 1 < size && (isalnum((unsigned char)*q) || *q == '-' || *q == '_' ||
                                    *q == '.'))
                enc[k++] = *q++;
            if (k > 0) {
                enc[k] = '\0';
                return;
            }
        }
    }
}

/* PyErr_ProgramTextObject (err_programtext): line `lineno` of the file
   named `filename` decoded with its source encoding ("replace" errors), or
   QP_NULL (no exception pending) if it cannot be read. */
static JSValue qp_err_program_text(JSContext *ctx, const char *filename, int lineno)
{
    char linebuf[1000], encoding[64];
    size_t line_size = 0;
    const char *line;
    JSValue res;
    FILE *fp;
    int i;

    if (filename == NULL || lineno <= 0)
        return QP_NULL;
    fp = fopen(filename, "r");
    if (fp == NULL)
        return QP_NULL;
    qp_err_find_encoding(fp, encoding, sizeof(encoding));
    rewind(fp);
    for (i = 0; i < lineno; ) {
        if (!qp_err_fgets_univ(fp, linebuf, sizeof(linebuf), &line_size)) {
            fclose(fp);     /* error or EOF */
            return QP_NULL;
        }
        /* a line longer than the buffer continues in the next read */
        if (i + 1 < lineno && line_size == sizeof(linebuf) - 1 &&
            linebuf[sizeof(linebuf) - 2] != '\n')
            continue;
        i++;
    }
    fclose(fp);
    line = linebuf;
    if (lineno == 1 && line_size >= 3 && memcmp(line, "\xef\xbb\xbf", 3) == 0) {
        line += 3;
        line_size -= 3;
    }
    res = qp_str_decode(ctx, line, (Py_ssize_t)line_size, encoding, "replace");
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return QP_NULL;
    }
    return res;
}

static int qp_syntax_warn(JSContext *ctx, const char *filename, JSValueConst module,
                          int lineno, const char *msg)
{
    JSValue text, fname;
    int ret;

    if (JS_IsUndefined(module))
        return 0;
    text = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(text))
        return -1;
    fname = qp_str_from_cstr(ctx, filename ? filename : "<string>");
    if (JS_IsException(fname)) {
        JS_FreeValue(ctx, text);
        return -1;
    }
    ret = qp_warnings_warn_explicit(ctx, QP_EXC(ctx, SyntaxWarning), text, fname, lineno,
                                    module, QP_NULL, QP_NULL);
    JS_FreeValue(ctx, text);
    JS_FreeValue(ctx, fname);
    if (ret >= 0)
        return 0;
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, SyntaxWarning))) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 1;
    }
    return -1;
}

static int qp_syntax_warn_err(JSContext *ctx, qp_error *err, const char *filename,
                              JSValueConst module, const char *msg, int lineno, int offset,
                              int end_lineno, int end_offset)
{
    int r = qp_syntax_warn(ctx, filename, module, lineno, msg);
    if (r == 0)
        return 0;
    if (r > 0)
        qp_error_set(err, QP_ERR_SYNTAX, lineno, offset, end_lineno, end_offset, "%s", msg);
    else
        qp_error_set(err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
    return -1;
}

/* ========================================================================= */
/* Name suggestions (Python/suggestions.c)                                    */
/* ========================================================================= */

#ifndef QP_SUGG_MOVE_COST
#define QP_SUGG_MAX_CANDIDATE_ITEMS 750
#define QP_SUGG_MAX_STRING_SIZE 40
#define QP_SUGG_MOVE_COST 2
#define QP_SUGG_CASE_COST 1
#endif
#define QP_SUGG_LEAST_FIVE_BITS(n) ((n) & 31)

static inline size_t qp_err_min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static inline int qp_sugg_substitution_cost(char a, char b)
{
    if (QP_SUGG_LEAST_FIVE_BITS(a) != QP_SUGG_LEAST_FIVE_BITS(b))
        return QP_SUGG_MOVE_COST;
    if (a == b)
        return 0;
    if ('A' <= a && a <= 'Z')
        a += ('a' - 'A');
    if ('A' <= b && b <= 'Z')
        b += ('a' - 'A');
    if (a == b)
        return QP_SUGG_CASE_COST;
    return QP_SUGG_MOVE_COST;
}

/* levenshtein_distance over UTF-8 bytes */
static Py_ssize_t qp_sugg_levenshtein(const char *a, size_t a_size, const char *b,
                                      size_t b_size, size_t max_cost, size_t *buffer)
{
    size_t i, tmp, result, b_index, index;

    if (a == b)
        return 0;
    while (a_size && b_size && a[0] == b[0]) {
        a++; a_size--;
        b++; b_size--;
    }
    while (a_size && b_size && a[a_size - 1] == b[b_size - 1]) {
        a_size--;
        b_size--;
    }
    if (a_size == 0 || b_size == 0)
        return (a_size + b_size) * QP_SUGG_MOVE_COST;
    if (a_size > QP_SUGG_MAX_STRING_SIZE || b_size > QP_SUGG_MAX_STRING_SIZE)
        return max_cost + 1;
    if (b_size < a_size) {
        const char *t = a; a = b; b = t;
        size_t ts = a_size; a_size = b_size; b_size = ts;
    }
    if ((b_size - a_size) * QP_SUGG_MOVE_COST > max_cost)
        return max_cost + 1;
    tmp = QP_SUGG_MOVE_COST;
    for (i = 0; i < a_size; i++) {
        buffer[i] = tmp;
        tmp += QP_SUGG_MOVE_COST;
    }
    result = 0;
    for (b_index = 0; b_index < b_size; b_index++) {
        char code = b[b_index];
        size_t distance = result = b_index * QP_SUGG_MOVE_COST;
        size_t minimum = SIZE_MAX;
        for (index = 0; index < a_size; index++) {
            size_t substitute = distance + qp_sugg_substitution_cost(code, a[index]);
            size_t insert_delete;
            distance = buffer[index];
            insert_delete = qp_err_min_size(result, distance) + QP_SUGG_MOVE_COST;
            result = qp_err_min_size(insert_delete, substitute);
            buffer[index] = result;
            if (result < minimum)
                minimum = result;
        }
        if (minimum > max_cost)
            return max_cost + 1;
    }
    return result;
}

/* _Py_CalculateSuggestions: dir_list is a list or tuple; non-str items are
   skipped.  New str, QP_NULL when nothing is close enough, JS_EXCEPTION. */
static JSValue qp_calculate_suggestions(JSContext *ctx, JSValueConst dir_list, JSValueConst name)
{
    Py_ssize_t n, i, suggestion_distance = PY_SSIZE_T_MAX;
    JSValueConst *items;
    JSValue tmp_list = QP_NULL, suggestion = QP_NULL;
    const char *name_str;
    size_t name_size, buffer[QP_SUGG_MAX_STRING_SIZE];

    if (qp_tuple_check(ctx, dir_list)) {
        n = QP_TUPLE_SIZE(dir_list);
        items = (JSValueConst *)QP_TUPLE_ITEMS(dir_list);
    } else {
        JSValue *li = qp_list_items(dir_list, &n);
        if (!li) {
            tmp_list = qp_list_as_tuple(ctx, dir_list);
            if (JS_IsException(tmp_list))
                return tmp_list;
            n = QP_TUPLE_SIZE(tmp_list);
            items = (JSValueConst *)QP_TUPLE_ITEMS(tmp_list);
        } else {
            items = (JSValueConst *)li;
        }
    }
    if (n >= QP_SUGG_MAX_CANDIDATE_ITEMS) {
        JS_FreeValue(ctx, tmp_list);
        return QP_NULL;
    }
    name_str = qp_str_utf8(ctx, name, &name_size);
    if (!name_str) {
        JS_FreeValue(ctx, tmp_list);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        JSValueConst item = items[i];
        const char *item_str;
        size_t item_size;
        Py_ssize_t max_distance, cur;
        int eq;

        if (!qp_str_check(ctx, item))
            continue;
        eq = qp_str_eq(ctx, name, item);
        if (eq < 0)
            goto fail;
        if (eq)
            continue;
        item_str = qp_str_utf8(ctx, item, &item_size);
        if (!item_str)
            goto fail;
        max_distance = (Py_ssize_t)(name_size + item_size + 3) * QP_SUGG_MOVE_COST / 6;
        if (max_distance > suggestion_distance - 1)
            max_distance = suggestion_distance - 1;
        cur = qp_sugg_levenshtein(name_str, name_size, item_str, item_size,
                                  max_distance, buffer);
        JS_FreeCString(ctx, item_str);
        if (cur > max_distance)
            continue;
        if (QP_IS_NULL(suggestion) || cur < suggestion_distance) {
            JS_FreeValue(ctx, suggestion);
            suggestion = JS_DupValue(ctx, item);
            suggestion_distance = cur;
        }
    }
    JS_FreeCString(ctx, name_str);
    JS_FreeValue(ctx, tmp_list);
    return suggestion;
 fail:
    JS_FreeCString(ctx, name_str);
    JS_FreeValue(ctx, tmp_list);
    JS_FreeValue(ctx, suggestion);
    return JS_EXCEPTION;
}

/* Modules/_suggestions.c: _generate_suggestions(candidates, item, /) */
static JSValue qp_suggestions_generate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    JSValue r;

    if (nargs != 2)
        return qp_raise_type_error(ctx, "_generate_suggestions expected 2 arguments, got %d",
                                   nargs);
    if (!qp_list_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "_generate_suggestions() argument 1 must be list, "
                                   "not %T", args[0]);
    if (!qp_str_check(ctx, args[1]))
        return qp_raise_type_error(ctx, "_generate_suggestions() argument 2 must be str, "
                                   "not %T", args[1]);
    r = qp_calculate_suggestions(ctx, args[0], args[1]);
    return QP_IS_NULL(r) ? JS_UNDEFINED : r;
}

static const QPMethodDef qp_suggestions_methods[] = {
    { "_generate_suggestions", qp_suggestions_generate, QP_METH_FASTCALL, 0,
      "_generate_suggestions($module, candidates, item, /)\n--\n\n"
      "Returns the candidate in candidates that's closest to item" },
    { NULL, NULL, 0, 0, NULL },
};

static const QPModuleDef qp_moddef__suggestions = {
    "_suggestions", "A module for generating suggestions for typos", qp_suggestions_methods, NULL,
};

/* ========================================================================= */
/* Uncaught exceptions (Python/pythonrun.c)                                   */
/* ========================================================================= */

/* parse_exit_code(): 1 when code is an int or None. */
static int qp_err_parse_exit_code(JSContext *ctx, JSValueConst code, int *exitcode)
{
    if (qp_long_check(ctx, code)) {
        JSValue v = qp_long_value(ctx, code);
        int64_t x;
        if (JS_IsException(v) || qp_long_as_int64(ctx, v, &x) < 0) {
            JS_FreeValue(ctx, v);
            qp_err_clear(ctx);
            *exitcode = -1;
            return 1;
        }
        JS_FreeValue(ctx, v);
        *exitcode = (int)x;
        return 1;
    }
    if (JS_IsUndefined(code) || JS_IsNull(code)) {
        *exitcode = 0;
        return 1;
    }
    return 0;
}

/* _Py_HandleSystemExitAndKeyboardInterrupt for a SystemExit instance:
   returns the exit code (printing a non-int code to stderr). */
static int qp_err_system_exit_code(JSContext *ctx, JSValueConst exc)
{
    JSValue code, file, toprint;
    int exitcode = 1;

    fflush(stdout);
    code = qp_getattr(ctx, exc, QP_ATOM(ctx, code));
    if (JS_IsException(code)) {
        qp_err_clear(ctx);
        toprint = JS_DupValue(ctx, exc);
    } else if (qp_err_parse_exit_code(ctx, code, &exitcode)) {
        JS_FreeValue(ctx, code);
        return exitcode;
    } else {
        toprint = code;
    }
    file = qp_err_sys_stderr(ctx);
    if (JS_IsException(file)) {
        qp_err_clear(ctx);
        file = QP_NULL;
    }
    if (qp_file_write_object(ctx, QP_IS_NULL(file) ? JS_UNDEFINED : file, toprint, TRUE) < 0)
        qp_err_clear(ctx);
    if (QP_IS_NONE_OR_NULL(file))
        fflush(stderr);
    qp_sys_write_stderr(ctx, "\n");
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, toprint);
    return 1;
}

static void qp_err_set_sys_attr(JSContext *ctx, const char *name, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL || qp_setattr(ctx, QP_STATE(ctx)->sys_module, a, v) < 0)
        qp_err_clear(ctx);
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
}

/* PyErr_Print() for the launcher, taking the pending exception: returns the
   process exit code (SystemExit's code, else 1 after sys.excepthook ran);
   *pkbint = 1 for KeyboardInterrupt. */
static int qp_err_print_uncaught(JSContext *ctx, int *pkbint)
{
    JSValue exc, typ, tb, hook, args[3], res;

    *pkbint = 0;
    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return 0;
    if (!qp_err_is_pyexc_obj(exc))
        exc = qp_exc_normalize(ctx, exc);
    if (!qp_err_is_pyexc_obj(exc)) {
        JS_FreeValue(ctx, exc);
        return 1;
    }
    if (qp_typecheck(ctx, exc, QP_EXC(ctx, KeyboardInterrupt))) {
        *pkbint = 1;
    } else if (!QP_STATE(ctx)->config.inspect &&
               qp_typecheck(ctx, exc, QP_EXC(ctx, SystemExit))) {
        int code = qp_err_system_exit_code(ctx, exc);
        JS_FreeValue(ctx, exc);
        return code;
    }
    typ = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, exc)));
    tb = JS_DupValue(ctx, qp_exc_traceback(exc));
    qp_err_set_sys_attr(ctx, "last_exc", exc);
    qp_err_set_sys_attr(ctx, "last_type", typ);
    qp_err_set_sys_attr(ctx, "last_value", exc);
    qp_err_set_sys_attr(ctx, "last_traceback", tb);
    hook = qp_sys_get(ctx, QP_ATOM(ctx, excepthook));
    if (JS_IsException(hook)) {
        qp_err_clear(ctx);
        hook = QP_NULL;
    }
    if (!QP_IS_NULL(hook)) {
        args[0] = typ;
        args[1] = exc;
        args[2] = tb;
        res = qp_vectorcall(ctx, hook, (JSValueConst *)args, 3, QP_NULL);
        if (JS_IsException(res)) {
            JSValue exc2 = qp_exc_fetch(ctx);
            if (qp_err_is_pyexc_obj(exc2) && !QP_STATE(ctx)->config.inspect &&
                qp_typecheck(ctx, exc2, QP_EXC(ctx, SystemExit))) {
                int code = qp_err_system_exit_code(ctx, exc2);
                JS_FreeValue(ctx, exc2);
                JS_FreeValue(ctx, hook);
                JS_FreeValue(ctx, typ);
                JS_FreeValue(ctx, tb);
                JS_FreeValue(ctx, exc);
                return code;
            }
            fflush(stdout);
            qp_sys_write_stderr(ctx, "Error in sys.excepthook:\n");
            if (qp_err_is_pyexc_obj(exc2))
                qp_exc_display(ctx, exc2, QP_NULL);
            qp_sys_write_stderr(ctx, "\nOriginal exception was:\n");
            qp_exc_display(ctx, exc, QP_NULL);
            JS_FreeValue(ctx, exc2);
        } else {
            JS_FreeValue(ctx, res);
        }
    } else {
        qp_sys_write_stderr(ctx, "sys.excepthook is missing\n");
        qp_exc_display(ctx, exc, QP_NULL);
    }
    qp_err_clear(ctx);
    JS_FreeValue(ctx, hook);
    JS_FreeValue(ctx, typ);
    JS_FreeValue(ctx, tb);
    JS_FreeValue(ctx, exc);
    return 1;
}

/* ========================================================================= */
/* Registered sources (linecache._register_code for -c and the REPL)          */
/* ========================================================================= */

/* Make `src` the source text shown in tracebacks for frames whose
   co_filename is `filename` (a "<...>" pseudo file name).  0 or -1. */
static int qp_linecache_register(JSContext *ctx, const char *filename, const char *src, size_t len)
{
    JSValue d, key, text;
    int r = -1;

    d = qp_err_private_dict(ctx, "\x01qp_linecache");
    if (JS_IsException(d))
        return -1;
    key = qp_str_from_cstr(ctx, filename);
    if (JS_IsException(key))
        goto done;
    text = qp_str_from_utf8(ctx, src, len);
    if (JS_IsException(text)) {
        JS_FreeValue(ctx, key);
        goto done;
    }
    r = qp_dict_set_item(ctx, d, key, text);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, text);
 done:
    JS_FreeValue(ctx, d);
    return r;
}
