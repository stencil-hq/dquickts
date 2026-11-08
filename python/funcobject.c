/*
 * dquickts Python (M2): function, bound method and cell objects.
 *
 * Derived from CPython 3.15 Objects/funcobject.c, Objects/classobject.c and
 * Objects/cellobject.c (PSF-2.0).
 *
 * A Python function is a JS_CLASS_BYTECODE_FUNCTION whose bytecode has
 * is_py; its Python metadata (QPFuncData, object.h) lives in a hidden
 * PY_FUNC_DATA object stored in u.func.home_object.  Free variables are the
 * function's var_refs (closure_var order = co_freevars); a cell object
 * (PY_CELL) wraps one JSVarRef.  User attributes of functions live in
 * QPFuncData.dict (never in the JS shape, which holds the JS 'name' and
 * 'length' properties), so the function type has its own tp_getattro /
 * tp_setattro (CPython: generic attribute access with tp_dictoffset).
 */

/* ---- private helpers shared with codeobject.c / frameobject.c ---------- */
static BOOL qp_co_check(JSValueConst v);
static JSValue qp_co_get_name(JSContext *ctx, JSValueConst code);
static JSValue qp_co_get_qualname(JSContext *ctx, JSValueConst code);
static JSValue qp_fo_get_code(JSContext *ctx, JSValueConst func);
static JSValue qp_fo_type_name(JSContext *ctx, QPType *t);
static BOOL qp_fo_callable(JSContext *ctx, JSValueConst v);

/* QPFuncData plus the fields private to this file. */
typedef struct QPFuncDataPriv {
    QPFuncData d;
    /* bytecodes replaced by `f.__code__ = ...`: kept alive until the
       function dies because a running frame of the function may still use
       them (CPython frames own a reference to their code). */
    JSValue *retired;
    int retired_count, retired_size;
    /* closure stored by PyFunction_SetClosure() (tuple, or JS_UNDEFINED
       for None); QP_NULL: __closure__ reflects the var_refs */
    JSValue closure;
} QPFuncDataPriv;

/* ======================================================================== */
/* PY_FUNC_DATA class                                                        */
/* ======================================================================== */

static void qp_funcdata_finalizer(JSRuntime *rt, JSValue val)
{
    QPFuncDataPriv *pd = JS_VALUE_GET_OBJ(val)->u.opaque;
    QPFuncData *d;
    int i;

    if (!pd)
        return;
    d = &pd->d;
    JS_FreeValueRT(rt, d->globals);
    JS_FreeValueRT(rt, d->builtins);
    JS_FreeValueRT(rt, d->name);
    JS_FreeValueRT(rt, d->qualname);
    JS_FreeValueRT(rt, d->module);
    JS_FreeValueRT(rt, d->doc);
    JS_FreeValueRT(rt, d->defaults);
    JS_FreeValueRT(rt, d->kwdefaults);
    JS_FreeValueRT(rt, d->annotations);
    JS_FreeValueRT(rt, d->annotate);
    JS_FreeValueRT(rt, d->type_params);
    JS_FreeValueRT(rt, d->dict);
    JS_FreeValueRT(rt, d->code_obj);
    for (i = 0; i < pd->retired_count; i++)
        JS_FreeValueRT(rt, pd->retired[i]);
    js_free_rt(rt, pd->retired);
    JS_FreeValueRT(rt, pd->closure);
    js_free_rt(rt, pd);
}

static void qp_funcdata_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPFuncDataPriv *pd = JS_VALUE_GET_OBJ(val)->u.opaque;
    QPFuncData *d;
    int i;

    if (!pd)
        return;
    d = &pd->d;
    JS_MarkValue(rt, d->globals, mark_func);
    JS_MarkValue(rt, d->builtins, mark_func);
    JS_MarkValue(rt, d->name, mark_func);
    JS_MarkValue(rt, d->qualname, mark_func);
    JS_MarkValue(rt, d->module, mark_func);
    JS_MarkValue(rt, d->doc, mark_func);
    JS_MarkValue(rt, d->defaults, mark_func);
    JS_MarkValue(rt, d->kwdefaults, mark_func);
    JS_MarkValue(rt, d->annotations, mark_func);
    JS_MarkValue(rt, d->annotate, mark_func);
    JS_MarkValue(rt, d->type_params, mark_func);
    JS_MarkValue(rt, d->dict, mark_func);
    JS_MarkValue(rt, d->code_obj, mark_func);
    for (i = 0; i < pd->retired_count; i++)
        JS_MarkValue(rt, pd->retired[i], mark_func);
    JS_MarkValue(rt, pd->closure, mark_func);
}

static QPFuncData *qp_function_data(JSValueConst v)
{
    JSObject *p, *h;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION || !p->u.func.function_bytecode)
        return NULL;
    h = p->u.func.home_object;
    if (!h || h->class_id != JS_CLASS_PY_FUNC_DATA)
        return NULL;
    return h->u.opaque;
}

static BOOL qp_is_py_function(JSValueConst v)
{
    return qp_function_data(v) != NULL;
}

/* PyEval_GetBuiltins(): builtins of the innermost running Python frame,
   else the interpreter's builtins dict (borrowed). */
static JSValueConst qp_fo_current_builtins(JSContext *ctx)
{
    JSStackFrame *sf;
    QPFuncData *d;

    for (sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        d = qp_function_data(sf->cur_func);
        if (d)
            return d->builtins;
    }
    return QP_STATE(ctx)->builtins_dict;
}

/* _PyDict_LoadBuiltinsFromGlobals: globals['__builtins__'] (a module is
   replaced by its dict); missing -> `fallback` if given, else the current
   builtins.  New reference. */
static JSValue qp_fo_builtins_from_globals(JSContext *ctx, JSValueConst globals,
                                           JSValueConst fallback)
{
    JSValue v;
    int r;

    r = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__), &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        if (!QP_IS_NULL(fallback))
            return JS_DupValue(ctx, fallback);
        return JS_DupValue(ctx, qp_fo_current_builtins(ctx));
    }
    if (qp_module_check(ctx, v)) {
        JSValue d = JS_DupValue(ctx, qp_module_get_dict(v));
        JS_FreeValue(ctx, v);
        return d;
    }
    return v;
}

/* Create the PY_FUNC_DATA companion of `func` (bytecode already set) as in
   _PyFunction_FromConstructor: name, qualname and doc from the code (or
   from the PY_CODE facade `code_obj` when given, which also becomes the
   cached __code__), module = globals['__name__'], builtins from globals. */
static int qp_fo_attach(JSContext *ctx, JSValueConst func, JSValueConst code_obj,
                        JSValueConst globals, JSValueConst builtins_fallback)
{
    JSObject *p = JS_VALUE_GET_OBJ(func);
    JSFunctionBytecode *b = p->u.func.function_bytecode;
    QPCodeInfo *ci = b->py_code;
    QPFuncDataPriv *pd;
    QPFuncData *d;
    JSValue dobj, v;
    int r;

    pd = js_mallocz(ctx, sizeof(*pd));
    if (!pd)
        return -1;
    d = &pd->d;
    d->globals = JS_UNDEFINED;
    d->builtins = JS_UNDEFINED;
    d->name = JS_UNDEFINED;
    d->qualname = JS_UNDEFINED;
    d->module = JS_UNDEFINED;
    d->doc = JS_UNDEFINED;
    d->defaults = JS_UNDEFINED;
    d->kwdefaults = JS_UNDEFINED;
    d->annotations = JS_UNDEFINED;
    d->annotate = JS_UNDEFINED;
    d->type_params = JS_UNDEFINED;
    d->dict = QP_NULL;
    d->code_obj = QP_NULL;
    d->version = 1;
    pd->closure = QP_NULL;
    dobj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_PY_FUNC_DATA);
    if (JS_IsException(dobj)) {
        js_free(ctx, pd);
        return -1;
    }
    JS_VALUE_GET_OBJ(dobj)->u.opaque = pd;

    d->globals = JS_DupValue(ctx, globals);
    d->type_params = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    if (!QP_IS_NULL(code_obj)) {
        d->code_obj = JS_DupValue(ctx, code_obj);
        d->name = qp_co_get_name(ctx, code_obj);
        if (JS_IsException(d->name))
            goto fail;
        d->qualname = qp_co_get_qualname(ctx, code_obj);
        if (JS_IsException(d->qualname))
            goto fail;
    } else {
        d->name = qp_str_from_atom(ctx, b->func_name != JS_ATOM_NULL ? b->func_name
                                   : JS_ATOM_empty_string);
        if (JS_IsException(d->name))
            goto fail;
        if (ci && ci->qualname != JS_ATOM_NULL) {
            d->qualname = qp_str_from_atom(ctx, ci->qualname);
            if (JS_IsException(d->qualname))
                goto fail;
        } else {
            d->qualname = JS_DupValue(ctx, d->name);
        }
    }
    /* __doc__: co_consts[0] if CO_HAS_DOCSTRING and it is a str */
    if (ci && (ci->co_flags & CO_HAS_DOCSTRING) && ci->doc_cpool_idx >= 0 &&
        ci->doc_cpool_idx < b->cpool_count &&
        qp_str_check(ctx, b->cpool[ci->doc_cpool_idx])) {
        d->doc = JS_DupValue(ctx, b->cpool[ci->doc_cpool_idx]);
    }
    /* __module__: globals['__name__'] or None */
    r = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __name__), &v);
    if (r < 0)
        goto fail;
    if (r > 0)
        d->module = v;
    d->builtins = qp_fo_builtins_from_globals(ctx, globals, builtins_fallback);
    if (JS_IsException(d->builtins)) {
        d->builtins = JS_UNDEFINED;
        goto fail;
    }
    p->u.func.home_object = JS_VALUE_GET_OBJ(dobj); /* reference transferred */
    return 0;
 fail:
    if (JS_IsException(d->name))
        d->name = JS_UNDEFINED;
    if (JS_IsException(d->qualname))
        d->qualname = JS_UNDEFINED;
    JS_FreeValue(ctx, dobj);
    return -1;
}

static int qp_function_init(JSContext *ctx, JSValueConst func, JSValueConst parent_func)
{
    QPFuncData *pd = qp_function_data(parent_func);

    if (!pd) {
        qp_raise_system_error(ctx, "MAKE_FUNCTION outside of a Python function");
        return -1;
    }
    if (qp_fo_attach(ctx, func, QP_NULL, pd->globals, pd->builtins) < 0)
        return -1;
#ifdef CONFIG_PYTHON_CAPI
    QP_FUNC_EVENT(ctx, CREATE, func, QP_NULL);
#endif
    return 0;
}

/* New Python function over `bytecode` (a JS_TAG_FUNCTION_BYTECODE value).
   closure: QP_NULL (free variables become empty cells) or a tuple of
   exactly closure_var_count PY_CELL objects (checked by the caller). */
static JSValue qp_fo_create_quiet(JSContext *ctx, JSValueConst bytecode, JSValueConst code_obj,
                                  JSValueConst globals, JSValueConst closure)
{
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(bytecode);
    JSValue func;
    JSObject *p;
    int i;

    func = JS_NewObjectClass(ctx, JS_CLASS_BYTECODE_FUNCTION);
    if (JS_IsException(func))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(func);
    p->u.func.function_bytecode = b;
    p->u.func.var_refs = NULL;
    p->u.func.home_object = NULL;
    JS_DupValue(ctx, bytecode);
    if (b->closure_var_count) {
        JSVarRef **var_refs = js_mallocz(ctx, sizeof(var_refs[0]) * b->closure_var_count);
        if (!var_refs)
            goto fail;
        p->u.func.var_refs = var_refs;
        for (i = 0; i < b->closure_var_count; i++) {
            if (!QP_IS_NULL(closure)) {
                JSValueConst cell = QP_TUPLE_ITEMS(closure)[i];
                QPCell *c = JS_VALUE_GET_OBJ(cell)->u.opaque;
                js_rc(c->var_ref)->ref_count++;
                var_refs[i] = c->var_ref;
            } else {
                var_refs[i] = js_create_var_ref(ctx, TRUE);
                if (!var_refs[i])
                    goto fail;
            }
        }
    }
    js_function_set_properties(ctx, func, b->func_name != JS_ATOM_NULL ? b->func_name
                               : JS_ATOM_empty_string, b->defined_arg_count);
    if (qp_fo_attach(ctx, func, code_obj, globals, QP_NULL) < 0)
        goto fail;
    return func;
 fail:
    JS_FreeValue(ctx, func);
    return JS_EXCEPTION;
}

/* The same, reported to the function watchers (C API). */
static JSValue qp_fo_create(JSContext *ctx, JSValueConst bytecode, JSValueConst code_obj,
                            JSValueConst globals, JSValueConst closure)
{
    JSValue func = qp_fo_create_quiet(ctx, bytecode, code_obj, globals, closure);
#ifdef CONFIG_PYTHON_CAPI
    if (!JS_IsException(func))
        QP_FUNC_EVENT(ctx, CREATE, func, QP_NULL);
#endif
    return func;
}

static JSValue qp_function_new(JSContext *ctx, JSValueConst bytecode, JSValueConst globals)
{
    if (JS_VALUE_GET_TAG(bytecode) != JS_TAG_FUNCTION_BYTECODE ||
        !((JSFunctionBytecode *)JS_VALUE_GET_PTR(bytecode))->is_py)
        return qp_raise_system_error(ctx, "qp_function_new: not Python bytecode");
    return qp_fo_create(ctx, bytecode, QP_NULL, globals, QP_NULL);
}

static int qp_function_set_attr(JSContext *ctx, JSValueConst func, int flag, JSValueConst value)
{
    QPFuncData *d = qp_function_data(func);
    JSValue *slot;

    if (!d) {
        qp_raise_system_error(ctx, "SET_FUNCTION_ATTRIBUTE on a non-function");
        return -1;
    }
    switch (flag) {
    case 0:
        slot = &d->type_params;
        break;
    case QP_FUNC_ATTR_DEFAULTS:
        slot = &d->defaults;
        d->version++;
        break;
    case QP_FUNC_ATTR_KWDEFAULTS:
        slot = &d->kwdefaults;
        d->version++;
        break;
    case QP_FUNC_ATTR_ANNOTATIONS:
        slot = &d->annotations;
        break;
    case QP_FUNC_ATTR_ANNOTATE:
        slot = &d->annotate;
        break;
    default:
        qp_raise_system_error(ctx, "unexpected function attribute flag %d", flag);
        return -1;
    }
    JS_FreeValue(ctx, *slot);
    *slot = JS_DupValue(ctx, value);
    return 0;
}

/* __code__ of a Python function: the cached PY_CODE facade (new ref). */
static JSValue qp_fo_get_code(JSContext *ctx, JSValueConst func)
{
    QPFuncData *d = qp_function_data(func);
    JSFunctionBytecode *b;

    if (!d)
        return qp_raise_type_error(ctx, "expected a function object");
    if (QP_IS_NULL(d->code_obj)) {
        b = JS_VALUE_GET_OBJ(func)->u.func.function_bytecode;
        d->code_obj = qp_code_new(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b));
        if (JS_IsException(d->code_obj)) {
            d->code_obj = QP_NULL;
            return JS_EXCEPTION;
        }
    }
    return JS_DupValue(ctx, d->code_obj);
}

/* Name of a type as CPython's tp_name (new str). */
static JSValue qp_fo_type_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* PyCallable_Check */
static BOOL qp_fo_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* _Py_HashPointer */
static Py_hash_t qp_fo_hash_pointer(const void *ptr)
{
    size_t y = (size_t)ptr;
    Py_hash_t h;

    y = (y >> 4) | (y << (8 * sizeof(void *) - 4));
    h = (Py_hash_t)y;
    if (h == -1)
        h = -2;
    return h;
}

/* ======================================================================== */
/* function type                                                             */
/* ======================================================================== */

enum {
    QP_FA_CODE,
    QP_FA_DEFAULTS,
    QP_FA_KWDEFAULTS,
    QP_FA_ANNOTATIONS,
    QP_FA_ANNOTATE,
    QP_FA_DICT,
    QP_FA_DOC,
    QP_FA_MODULE,
    QP_FA_NAME,
    QP_FA_QUALNAME,
    QP_FA_TYPE_PARAMS,
    QP_FA_CLOSURE,
    QP_FA_GLOBALS,
    QP_FA_BUILTINS,
};

/* func_get_annotation_dict: materialize __annotations__ (calling
   __annotate__(1) when there is no dict yet); borrowed result. */
static JSValue qp_fo_annotation_dict(JSContext *ctx, QPFuncData *d)
{
    if (JS_IsUndefined(d->annotations)) {
        JSValue one, ann;

        if (JS_IsUndefined(d->annotate) || !qp_fo_callable(ctx, d->annotate))
            return JS_UNDEFINED;
        one = JS_NewInt32(ctx, 1);
        ann = qp_call1(ctx, d->annotate, one);
        if (JS_IsException(ann))
            return JS_EXCEPTION;
        if (!qp_dict_check(ctx, ann)) {
            qp_raise_type_error(ctx, "__annotate__() must return a dict, not %T", ann);
            JS_FreeValue(ctx, ann);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, d->annotations);
        d->annotations = ann;
        return ann;
    }
    if (qp_tuple_check_exact(ctx, d->annotations)) {
        /* legacy (name, value, ...) tuple form */
        JSValue tup = d->annotations, dict;
        Py_ssize_t i, n = QP_TUPLE_SIZE(tup);

        dict = qp_dict_new(ctx);
        if (JS_IsException(dict))
            return JS_EXCEPTION;
        for (i = 0; i + 1 < n; i += 2) {
            if (qp_dict_set_item(ctx, dict, QP_TUPLE_ITEMS(tup)[i],
                                 QP_TUPLE_ITEMS(tup)[i + 1]) < 0) {
                JS_FreeValue(ctx, dict);
                return JS_EXCEPTION;
            }
        }
        d->annotations = dict;
        JS_FreeValue(ctx, tup);
    }
    return d->annotations;
}

static JSValue qp_fo_func_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPFuncData *d = qp_function_data(self);
    JSObject *p;
    JSFunctionBytecode *b;

    if (!d)
        return qp_raise_type_error(ctx, "descriptor requires a 'function' object but received '%T'", self);
    switch (magic) {
    case QP_FA_CODE:
        return qp_fo_get_code(ctx, self);
    case QP_FA_DEFAULTS:
        return JS_DupValue(ctx, d->defaults);
    case QP_FA_KWDEFAULTS:
        return JS_DupValue(ctx, d->kwdefaults);
    case QP_FA_ANNOTATIONS:
        {
            JSValue a;
            if (JS_IsUndefined(d->annotations) &&
                (JS_IsUndefined(d->annotate) || !qp_fo_callable(ctx, d->annotate))) {
                d->annotations = qp_dict_new(ctx);
                if (JS_IsException(d->annotations)) {
                    d->annotations = JS_UNDEFINED;
                    return JS_EXCEPTION;
                }
            }
            a = qp_fo_annotation_dict(ctx, d);
            return JS_DupValue(ctx, a);
        }
    case QP_FA_ANNOTATE:
        return JS_DupValue(ctx, d->annotate);
    case QP_FA_DICT:
        if (QP_IS_NULL(d->dict)) {
            d->dict = qp_dict_new(ctx);
            if (JS_IsException(d->dict)) {
                d->dict = QP_NULL;
                return JS_EXCEPTION;
            }
        }
        return JS_DupValue(ctx, d->dict);
    case QP_FA_DOC:
        return JS_DupValue(ctx, d->doc);
    case QP_FA_MODULE:
        return JS_DupValue(ctx, d->module);
    case QP_FA_NAME:
        return JS_DupValue(ctx, d->name);
    case QP_FA_QUALNAME:
        return JS_DupValue(ctx, d->qualname);
    case QP_FA_TYPE_PARAMS:
        if (JS_IsUndefined(d->type_params))
            return JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
        return JS_DupValue(ctx, d->type_params);
    case QP_FA_CLOSURE:
        {
            JSValue tup;
            int i;

            if (!QP_IS_NULL(((QPFuncDataPriv *)d)->closure))
                return JS_DupValue(ctx, ((QPFuncDataPriv *)d)->closure);
            p = JS_VALUE_GET_OBJ(self);
            b = p->u.func.function_bytecode;
            if (b->closure_var_count == 0)
                return JS_UNDEFINED;
            tup = qp_tuple_new(ctx, b->closure_var_count);
            if (JS_IsException(tup))
                return JS_EXCEPTION;
            for (i = 0; i < b->closure_var_count; i++) {
                JSVarRef *vr = p->u.func.var_refs ? p->u.func.var_refs[i] : NULL;
                JSValue cell = qp_cell_new(ctx, vr);
                if (JS_IsException(cell)) {
                    JS_FreeValue(ctx, tup);
                    return JS_EXCEPTION;
                }
                JS_FreeValue(ctx, QP_TUPLE_ITEMS(tup)[i]);
                QP_TUPLE_ITEMS(tup)[i] = cell;
            }
            return tup;
        }
    case QP_FA_GLOBALS:
        return JS_DupValue(ctx, d->globals);
    case QP_FA_BUILTINS:
        return JS_DupValue(ctx, d->builtins);
    default:
        abort();
    }
}

/* Replace a function's bytecode (func_set_code). */
static int qp_fo_set_code(JSContext *ctx, JSValueConst self, QPFuncData *d, JSValueConst value)
{
    QPFuncDataPriv *pd = (QPFuncDataPriv *)d;
    JSObject *p = JS_VALUE_GET_OBJ(self);
    JSFunctionBytecode *ob = p->u.func.function_bytecode, *nb;
    int mask = CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR;

    if (QP_IS_NULL(value) || !qp_co_check(value)) {
        qp_raise_type_error(ctx, "__code__ must be set to a code object");
        return -1;
    }
    nb = qp_code_bytecode(value);
    if (nb->closure_var_count != ob->closure_var_count) {
        qp_raise_value_error(ctx, "%U() requires a code object with %zd free vars, not %d",
                             d->name, (Py_ssize_t)ob->closure_var_count,
                             nb->closure_var_count);
        return -1;
    }
    if ((ob->py_code->co_flags & mask) != (nb->py_code->co_flags & mask)) {
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "Assigning a code object of non-matching type is deprecated "
                    "(e.g., from a generator to a plain function)") < 0)
            return -1;
    }
#ifdef CONFIG_PYTHON_CAPI
    QP_FUNC_EVENT(ctx, MODIFY_CODE, self, value);
#endif
    if (nb != ob) {
        if (pd->retired_count >= pd->retired_size) {
            int new_size = pd->retired_size ? pd->retired_size * 2 : 2;
            JSValue *r = js_realloc(ctx, pd->retired, sizeof(JSValue) * new_size);
            if (!r)
                return -1;
            pd->retired = r;
            pd->retired_size = new_size;
        }
        /* the function's reference to the old bytecode moves to `retired` */
        pd->retired[pd->retired_count++] = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, ob);
        JS_DupValue(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, nb));
        p->u.func.function_bytecode = nb;
    }
    JS_FreeValue(ctx, d->code_obj);
    d->code_obj = JS_DupValue(ctx, value);
    d->version++;
    return 0;
}

static int qp_fo_func_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPFuncData *d = qp_function_data(self);
    JSValue *slot;

    if (!d) {
        qp_raise_type_error(ctx, "descriptor requires a 'function' object but received '%T'", self);
        return -1;
    }
    switch (magic) {
    case QP_FA_CODE:
        return qp_fo_set_code(ctx, self, d, value);
    case QP_FA_DEFAULTS:
        if (QP_IS_NULL(value))
            value = JS_UNDEFINED;
        if (!JS_IsUndefined(value) && !qp_tuple_check(ctx, value)) {
            qp_raise_type_error(ctx, "__defaults__ must be set to a tuple object");
            return -1;
        }
#ifdef CONFIG_PYTHON_CAPI
        QP_FUNC_EVENT(ctx, MODIFY_DEFAULTS, self, value);
#endif
        slot = &d->defaults;
        d->version++;
        break;
    case QP_FA_KWDEFAULTS:
        if (QP_IS_NULL(value))
            value = JS_UNDEFINED;
        if (!JS_IsUndefined(value) && !qp_dict_check(ctx, value)) {
            qp_raise_type_error(ctx, "__kwdefaults__ must be set to a dict object");
            return -1;
        }
#ifdef CONFIG_PYTHON_CAPI
        QP_FUNC_EVENT(ctx, MODIFY_KWDEFAULTS, self, value);
#endif
        slot = &d->kwdefaults;
        d->version++;
        break;
    case QP_FA_ANNOTATIONS:
        if (QP_IS_NULL(value))
            value = JS_UNDEFINED;
        if (!JS_IsUndefined(value) && !qp_dict_check(ctx, value)) {
            qp_raise_type_error(ctx, "__annotations__ must be set to a dict object");
            return -1;
        }
        JS_FreeValue(ctx, d->annotate);
        d->annotate = JS_UNDEFINED;
        slot = &d->annotations;
        break;
    case QP_FA_ANNOTATE:
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "__annotate__ cannot be deleted");
            return -1;
        }
        if (JS_IsUndefined(value) || JS_IsNull(value)) {
            value = JS_UNDEFINED;
        } else if (qp_fo_callable(ctx, value)) {
            JS_FreeValue(ctx, d->annotations);
            d->annotations = JS_UNDEFINED;
        } else {
            qp_raise_type_error(ctx, "__annotate__ must be callable or None");
            return -1;
        }
        slot = &d->annotate;
        break;
    case QP_FA_DICT:
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "cannot delete __dict__");
            return -1;
        }
        if (!qp_dict_check(ctx, value)) {
            qp_raise_type_error(ctx, "__dict__ must be set to a dictionary, not a '%T'", value);
            return -1;
        }
        slot = &d->dict;
        break;
    case QP_FA_DOC:
        if (QP_IS_NULL(value))
            value = JS_UNDEFINED;
        slot = &d->doc;
        break;
    case QP_FA_MODULE:
        if (QP_IS_NULL(value))
            value = JS_UNDEFINED;
        slot = &d->module;
        break;
    case QP_FA_NAME:
        if (QP_IS_NULL(value) || !qp_str_check(ctx, value)) {
            qp_raise_type_error(ctx, "__name__ must be set to a string object");
            return -1;
        }
        slot = &d->name;
        break;
    case QP_FA_QUALNAME:
        if (QP_IS_NULL(value) || !qp_str_check(ctx, value)) {
            qp_raise_type_error(ctx, "__qualname__ must be set to a string object");
            return -1;
        }
#ifdef CONFIG_PYTHON_CAPI
        QP_FUNC_EVENT(ctx, MODIFY_QUALNAME, self, value);
#endif
        slot = &d->qualname;
        break;
    case QP_FA_TYPE_PARAMS:
        if (QP_IS_NULL(value) || !qp_tuple_check(ctx, value)) {
            qp_raise_type_error(ctx, "__type_params__ must be set to a tuple");
            return -1;
        }
        slot = &d->type_params;
        break;
    default:
        /* __closure__, __globals__, __builtins__ (read-only members) */
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "readonly attribute");
        return -1;
    }
    JS_FreeValue(ctx, *slot);
    *slot = JS_DupValue(ctx, value);
    return 0;
}

/* Generic attribute lookup with the function's __dict__
   (PyObject_GenericGetAttr, tp_dictoffset = func_dict). */
static JSValue qp_fo_func_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPFuncData *d = qp_function_data(obj);
    QPType *t = qp_type_of(ctx, obj);
    QPDescrGetFunc *f = NULL;
    JSValue descr, res;
    int r;

    descr = qp_type_lookup(ctx, t, name);
    if (!QP_IS_NULL(descr)) {
        QPType *dt = qp_type_of(ctx, descr);
        descr = JS_DupValue(ctx, descr);
        f = dt->slots.tp_descr_get;
        if (f && dt->slots.tp_descr_set) {
            res = f(ctx, descr, obj, QP_TYPE_VALUE(t));
            JS_FreeValue(ctx, descr);
            return res;
        }
    }
    if (d && !QP_IS_NULL(d->dict)) {
        JSValue dict = JS_DupValue(ctx, d->dict);
        r = qp_dict_get_item_atom(ctx, dict, name, &res);
        JS_FreeValue(ctx, dict);
        if (r != 0) {
            JS_FreeValue(ctx, descr);
            return r < 0 ? JS_EXCEPTION : res;
        }
    }
    if (!QP_IS_NULL(descr)) {
        if (f) {
            res = f(ctx, descr, obj, QP_TYPE_VALUE(t));
            JS_FreeValue(ctx, descr);
            return res;
        }
        return descr;
    }
    return qp_raise_attribute_error(ctx, obj, name);
}

static int qp_fo_func_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPFuncData *d = qp_function_data(obj);
    QPType *t = qp_type_of(ctx, obj);
    JSValue descr;
    int r;

    descr = qp_type_lookup(ctx, t, name);
    if (!QP_IS_NULL(descr)) {
        QPType *dt = qp_type_of(ctx, descr);
        if (dt->slots.tp_descr_set) {
            descr = JS_DupValue(ctx, descr);
            r = dt->slots.tp_descr_set(ctx, descr, obj, v);
            JS_FreeValue(ctx, descr);
            return r;
        }
    }
    if (!d) {
        qp_raise_attribute_error(ctx, obj, name);
        return -1;
    }
    if (QP_IS_NULL(d->dict)) {
        if (QP_IS_NULL(v)) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        d->dict = qp_dict_new(ctx);
        if (JS_IsException(d->dict)) {
            d->dict = QP_NULL;
            return -1;
        }
    }
    if (QP_IS_NULL(v)) {
        JSValue dict = JS_DupValue(ctx, d->dict);
        r = qp_dict_del_item_atom(ctx, dict, name);
        JS_FreeValue(ctx, dict);
        if (r < 0)
            return -1;
        if (r == 0) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        return 0;
    }
    return qp_dict_set_item_atom(ctx, d->dict, name, v);
}

static JSValue qp_fo_func_repr(JSContext *ctx, JSValueConst self)
{
    QPFuncData *d = qp_function_data(self);

    if (!d)
        return qp_raise_type_error(ctx, "expected a function object");
    return qp_str_from_format(ctx, "<function %U at %p>", d->qualname,
                              JS_VALUE_GET_PTR(self));
}

static JSValue qp_fo_func_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    return qp_vectorcall(ctx, callable, args, nargsf, kwnames);
}

/* func_descr_get: bind to an instance */
static JSValue qp_fo_func_descr_get(JSContext *ctx, JSValueConst func, JSValueConst obj,
                                    JSValueConst type)
{
    if (QP_IS_NULL(obj) || JS_IsUndefined(obj) || JS_IsNull(obj))
        return JS_DupValue(ctx, func);
    return qp_method_new(ctx, func, obj);
}

/* function(code, globals, name=None, argdefs=None, closure=None, kwdefaults=None) */
static JSValue qp_fo_func_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = {
        "code", "globals", "name", "argdefs", "closure", "kwdefaults", NULL
    };
    static const QPArgSpec spec = { "function", kwlist, 2, 6, 0 };
    JSValueConst a[6];
    JSValueConst code, globals, name, defaults, closure, kwdefaults;
    JSFunctionBytecode *b;
    Py_ssize_t nclosure, i;
    JSValue func, co_name;
    QPFuncData *d;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    code = a[0];
    globals = a[1];
    name = QP_IS_NULL(a[2]) ? JS_UNDEFINED : a[2];
    defaults = QP_IS_NULL(a[3]) ? JS_UNDEFINED : a[3];
    closure = QP_IS_NULL(a[4]) ? JS_UNDEFINED : a[4];
    kwdefaults = QP_IS_NULL(a[5]) ? JS_UNDEFINED : a[5];
    if (!qp_co_check(code))
        return qp_raise_type_error(ctx, "function() argument 'code' must be code, not %T", code);
    if (!qp_dict_check(ctx, globals))
        return qp_raise_type_error(ctx, "function() argument 'globals' must be dict, not %T", globals);
    b = qp_code_bytecode(code);
    if (!JS_IsUndefined(name) && !qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "arg 3 (name) must be None or string");
    if (!JS_IsUndefined(defaults) && !qp_tuple_check(ctx, defaults))
        return qp_raise_type_error(ctx, "arg 4 (defaults) must be None or tuple");
    if (!qp_tuple_check(ctx, closure)) {
        if (b->closure_var_count && JS_IsUndefined(closure))
            return qp_raise_type_error(ctx, "arg 5 (closure) must be tuple");
        else if (!JS_IsUndefined(closure))
            return qp_raise_type_error(ctx, "arg 5 (closure) must be None or tuple");
    }
    if (!JS_IsUndefined(kwdefaults) && !qp_dict_check(ctx, kwdefaults))
        return qp_raise_type_error(ctx, "arg 6 (kwdefaults) must be None or dict");
    nclosure = JS_IsUndefined(closure) ? 0 : QP_TUPLE_SIZE(closure);
    if (b->closure_var_count != nclosure) {
        co_name = qp_co_get_name(ctx, code);
        if (JS_IsException(co_name))
            return JS_EXCEPTION;
        qp_raise_value_error(ctx, "%U requires closure of length %d, not %zd",
                             co_name, b->closure_var_count, nclosure);
        JS_FreeValue(ctx, co_name);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nclosure; i++) {
        JSValueConst o = QP_TUPLE_ITEMS(closure)[i];
        if (JS_VALUE_GET_TAG(o) != JS_TAG_OBJECT ||
            JS_VALUE_GET_OBJ(o)->class_id != JS_CLASS_PY_CELL) {
            JSValue tn = qp_fo_type_name(ctx, qp_type_of(ctx, o));
            if (JS_IsException(tn))
                return JS_EXCEPTION;
            qp_raise_type_error(ctx, "arg 5 (closure) expected cell, found %U", tn);
            JS_FreeValue(ctx, tn);
            return JS_EXCEPTION;
        }
    }
    func = qp_fo_create(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b), code, globals,
                        nclosure ? closure : QP_NULL);
    if (JS_IsException(func))
        return JS_EXCEPTION;
    d = qp_function_data(func);
    if (!JS_IsUndefined(name)) {
        JS_FreeValue(ctx, d->name);
        d->name = JS_DupValue(ctx, name);
    }
    if (!JS_IsUndefined(defaults))
        d->defaults = JS_DupValue(ctx, defaults);
    if (!JS_IsUndefined(kwdefaults))
        d->kwdefaults = JS_DupValue(ctx, kwdefaults);
    return func;
}

static const QPGetSetDef qp_fo_func_getsets[] = {
    { "__code__", qp_fo_func_get, qp_fo_func_set, QP_FA_CODE, NULL },
    { "__defaults__", qp_fo_func_get, qp_fo_func_set, QP_FA_DEFAULTS, NULL },
    { "__kwdefaults__", qp_fo_func_get, qp_fo_func_set, QP_FA_KWDEFAULTS, NULL },
    { "__annotations__", qp_fo_func_get, qp_fo_func_set, QP_FA_ANNOTATIONS,
      "Dict of annotations in a function object." },
    { "__annotate__", qp_fo_func_get, qp_fo_func_set, QP_FA_ANNOTATE,
      "Get the code object for a function." },
    { "__dict__", qp_fo_func_get, qp_fo_func_set, QP_FA_DICT, NULL },
    { "__doc__", qp_fo_func_get, qp_fo_func_set, QP_FA_DOC, NULL },
    { "__module__", qp_fo_func_get, qp_fo_func_set, QP_FA_MODULE, NULL },
    { "__name__", qp_fo_func_get, qp_fo_func_set, QP_FA_NAME, NULL },
    { "__qualname__", qp_fo_func_get, qp_fo_func_set, QP_FA_QUALNAME, NULL },
    { "__type_params__", qp_fo_func_get, qp_fo_func_set, QP_FA_TYPE_PARAMS,
      "Get the declared type parameters for a function." },
    /* func_memberlist */
    { "__closure__", qp_fo_func_get, NULL, QP_FA_CLOSURE, NULL, QP_GETSET_MEMBER },
    { "__globals__", qp_fo_func_get, NULL, QP_FA_GLOBALS, NULL, QP_GETSET_MEMBER },
    { "__builtins__", qp_fo_func_get, NULL, QP_FA_BUILTINS, NULL, QP_GETSET_MEMBER },
    { NULL }
};

static const QPTypeSpec qp_fo_function_spec = {
    .name = "function",
    .doc = "function(code, globals, name=None, argdefs=None, closure=None, kwdefaults=None)\n"
           "--\n\n"
           "Create a function object.\n\n"
           "  code\n    a code object\n"
           "  globals\n    the globals dictionary\n"
           "  name\n    a string that overrides the name from the code object\n"
           "  argdefs\n    a tuple that specifies the default argument values\n"
           "  closure\n    a tuple that supplies the bindings for free variables\n"
           "  kwdefaults\n    a dictionary that specifies the default keyword argument values",
    .flags = QP_TPFLAGS_METHOD_DESCRIPTOR | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_BYTECODE_FUNCTION,
    .generic_attr = QP_GENERIC_GETATTR | QP_GENERIC_SETATTR,
    .slots = {
        .tp_getattro = qp_fo_func_getattro,
        .tp_setattro = qp_fo_func_setattro,
        .tp_repr = qp_fo_func_repr,
        .tp_call = qp_fo_func_call,
        .tp_descr_get = qp_fo_func_descr_get,
        .tp_new = qp_fo_func_new,
    },
    .getsets = qp_fo_func_getsets,
};

/* ======================================================================== */
/* method type (bound methods)                                               */
/* ======================================================================== */

static void qp_method_finalizer(JSRuntime *rt, JSValue val)
{
    QPMethodObject *m = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!m)
        return;
    JS_FreeValueRT(rt, m->func);
    JS_FreeValueRT(rt, m->self);
    js_free_rt(rt, m);
}

static void qp_method_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPMethodObject *m = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!m)
        return;
    JS_MarkValue(rt, m->func, mark_func);
    JS_MarkValue(rt, m->self, mark_func);
}

static JSValue qp_method_new(JSContext *ctx, JSValueConst func, JSValueConst self)
{
    QPMethodObject *m;
    JSValue obj;

    if (QP_IS_NULL(self))
        return qp_raise_system_error(ctx, "bad argument to internal function");
    m = js_malloc(ctx, sizeof(*m));
    if (!m)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, method)), JS_CLASS_PY_METHOD);
    if (JS_IsException(obj)) {
        js_free(ctx, m);
        return JS_EXCEPTION;
    }
    m->func = JS_DupValue(ctx, func);
    m->self = JS_DupValue(ctx, self);
    JS_VALUE_GET_OBJ(obj)->u.opaque = m;
    return obj;
}

static QPMethodObject *qp_fo_method_data(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_METHOD)
        return NULL;
    return JS_VALUE_GET_OBJ(v)->u.opaque;
}

/* method_vectorcall: func(self, *args, **kw) prepending self in place when
   the caller allows it (QP_VECTORCALL_ARGUMENTS_OFFSET). */
static JSValue qp_fo_method_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    QPMethodObject *m = qp_fo_method_data(callable);
    Py_ssize_t nargs = qp_nargs(nargsf);
    Py_ssize_t nkw = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames);
    Py_ssize_t total = nargs + nkw;
    JSValue small[10], *buf, res;

    if (!m)
        return qp_raise_type_error(ctx, "expected a method object");
    if (nargsf & QP_VECTORCALL_ARGUMENTS_OFFSET) {
        JSValue *newargs = (JSValue *)args - 1;
        JSValue tmp = newargs[0];
        newargs[0] = m->self;
        res = qp_vectorcall(ctx, m->func, (JSValueConst *)newargs, nargs + 1, kwnames);
        newargs[0] = tmp;
        return res;
    }
    buf = small;
    if (total + 2 > (Py_ssize_t)countof(small)) {
        buf = js_malloc(ctx, sizeof(JSValue) * (total + 2));
        if (!buf)
            return JS_EXCEPTION;
    }
    buf[0] = JS_UNDEFINED;
    buf[1] = m->self;
    if (total)
        memcpy(buf + 2, args, sizeof(JSValue) * total);
    res = qp_vectorcall(ctx, m->func, (JSValueConst *)(buf + 1),
                        (size_t)(nargs + 1) | QP_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    if (buf != small)
        js_free(ctx, buf);
    return res;
}

/* method_getattro: attributes of the method type win, everything else is
   looked up on __func__. */
static JSValue qp_fo_method_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPMethodObject *m = qp_fo_method_data(obj);
    QPType *t = qp_type_of(ctx, obj);
    JSValue descr, res;

    descr = qp_type_lookup(ctx, t, name);
    if (!QP_IS_NULL(descr)) {
        QPType *dt = qp_type_of(ctx, descr);
        if (dt->slots.tp_descr_get) {
            descr = JS_DupValue(ctx, descr);
            res = dt->slots.tp_descr_get(ctx, descr, obj, QP_TYPE_VALUE(t));
            JS_FreeValue(ctx, descr);
            return res;
        }
        return JS_DupValue(ctx, descr);
    }
    if (!m)
        return qp_raise_attribute_error(ctx, obj, name);
    return qp_getattr(ctx, m->func, name);
}

static JSValue qp_fo_method_repr(JSContext *ctx, JSValueConst self)
{
    QPMethodObject *m = qp_fo_method_data(self);
    JSValue funcname = QP_NULL, res;

    if (!m)
        return qp_raise_type_error(ctx, "expected a method object");
    if (qp_getattr_opt(ctx, m->func, QP_ATOM(ctx, __qualname__), &funcname) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(funcname) &&
        qp_getattr_opt(ctx, m->func, QP_ATOM(ctx, __name__), &funcname) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(funcname) && !qp_str_check(ctx, funcname)) {
        JS_FreeValue(ctx, funcname);
        funcname = QP_NULL;
    }
    if (QP_IS_NULL(funcname))
        return qp_str_from_format(ctx, "<bound method ? of %R>", m->self);
    res = qp_str_from_format(ctx, "<bound method %U of %R>", funcname, m->self);
    JS_FreeValue(ctx, funcname);
    return res;
}

/* identity hash of any value (PyObject_GenericHash) */
static Py_hash_t qp_fo_identity_hash(JSContext *ctx, JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v))
        return qp_fo_hash_pointer(JS_VALUE_GET_PTR(v));
    return qp_hash(ctx, v);
}

static Py_hash_t qp_fo_method_hash(JSContext *ctx, JSValueConst self)
{
    QPMethodObject *m = qp_fo_method_data(self);
    Py_hash_t x, y;

    x = qp_fo_identity_hash(ctx, m->self);
    if (x == -1)
        return -1;
    y = qp_hash(ctx, m->func);
    if (y == -1)
        return -1;
    x = x ^ y;
    if (x == -1)
        x = -2;
    return x;
}

static JSValue qp_fo_method_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    QPMethodObject *ma = qp_fo_method_data(a), *mb = qp_fo_method_data(b);
    int eq;

    if ((op != QP_CMP_EQ && op != QP_CMP_NE) || !ma || !mb)
        return qp_not_implemented(ctx);
    eq = qp_richcompare_bool(ctx, ma->func, mb->func, QP_CMP_EQ);
    if (eq < 0)
        return JS_EXCEPTION;
    if (eq == 1)
        eq = qp_is(ma->self, mb->self);
    return qp_bool_new(op == QP_CMP_EQ ? eq : !eq);
}

static JSValue qp_fo_method_descr_get(JSContext *ctx, JSValueConst meth, JSValueConst obj,
                                      JSValueConst type)
{
    return JS_DupValue(ctx, meth);
}

/* method(function, instance, /) */
static JSValue qp_fo_method_new_type(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                     size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);

    if (qp_arg_no_keywords(ctx, "method", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "method", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_fo_callable(ctx, args[0]))
        return qp_raise_type_error(ctx, "first argument must be callable");
    if (JS_IsUndefined(args[1]) || JS_IsNull(args[1]))
        return qp_raise_type_error(ctx, "instance must not be None");
    return qp_method_new(ctx, args[0], args[1]);
}

enum { QP_MA_FUNC, QP_MA_SELF, QP_MA_DOC };

static JSValue qp_fo_method_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPMethodObject *m = qp_fo_method_data(self);

    if (!m)
        return qp_raise_type_error(ctx, "descriptor requires a 'method' object but received '%T'", self);
    switch (magic) {
    case QP_MA_FUNC:
        return JS_DupValue(ctx, m->func);
    case QP_MA_SELF:
        return JS_DupValue(ctx, m->self);
    default:
        return qp_getattr(ctx, m->func, QP_ATOM(ctx, __doc__));
    }
}

/* method.__reduce__(): (getattr, (self, name)) with private-name mangling */
static JSValue qp_fo_method_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPMethodObject *m = qp_fo_method_data(self);
    JSValue funcname, getattr_fn = QP_NULL, res, items[2], pair;
    JSAtom getattr_atom;
    const char *s;
    size_t len;

    funcname = qp_getattr(ctx, m->func, QP_ATOM(ctx, __name__));
    if (JS_IsException(funcname))
        return JS_EXCEPTION;
    /* _Py_IsPrivateName: "__x" not ending with "__" */
    if (qp_str_check_exact(funcname)) {
        s = qp_str_utf8(ctx, funcname, &len);
        if (!s) {
            JS_FreeValue(ctx, funcname);
            return JS_EXCEPTION;
        }
        if (len > 2 && s[0] == '_' && s[1] == '_' &&
            !(s[len - 1] == '_' && s[len - 2] == '_')) {
            QPType *cls = qp_type_from_value(m->self);
            JSValue cname, mangled;
            const char *cs;
            size_t clen;

            if (!cls)
                cls = qp_type_of(ctx, m->self);
            cname = qp_str_from_atom(ctx, cls->name);
            if (JS_IsException(cname)) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, funcname);
                return JS_EXCEPTION;
            }
            cs = qp_str_utf8(ctx, cname, &clen);
            JS_FreeValue(ctx, cname);
            if (!cs) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, funcname);
                return JS_EXCEPTION;
            }
            {
                const char *c = cs;
                while (*c == '_')
                    c++;
                if (*c) {
                    mangled = qp_str_from_format(ctx, "_%s%s", c, s);
                    if (JS_IsException(mangled)) {
                        JS_FreeCString(ctx, cs);
                        JS_FreeCString(ctx, s);
                        JS_FreeValue(ctx, funcname);
                        return JS_EXCEPTION;
                    }
                    JS_FreeValue(ctx, funcname);
                    funcname = mangled;
                }
            }
            JS_FreeCString(ctx, cs);
        }
        JS_FreeCString(ctx, s);
    }
    getattr_atom = JS_NewAtom(ctx, "getattr");
    if (getattr_atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, funcname);
        return JS_EXCEPTION;
    }
    getattr_fn = qp_eval_get_builtin(ctx, getattr_atom);
    JS_FreeAtom(ctx, getattr_atom);
    if (JS_IsException(getattr_fn)) {
        JS_FreeValue(ctx, funcname);
        return JS_EXCEPTION;
    }
    items[0] = JS_DupValue(ctx, m->self);
    items[1] = funcname;
    pair = qp_tuple_new_steal(ctx, 2, items);
    if (JS_IsException(pair)) {
        JS_FreeValue(ctx, getattr_fn);
        return JS_EXCEPTION;
    }
    items[0] = getattr_fn;
    items[1] = pair;
    res = qp_tuple_new_steal(ctx, 2, items);
    return res;
}

static const QPGetSetDef qp_fo_method_getsets[] = {
    { "__func__", qp_fo_method_get, NULL, QP_MA_FUNC,
      "the function (or other callable) implementing a method", QP_GETSET_MEMBER },
    { "__self__", qp_fo_method_get, NULL, QP_MA_SELF,
      "the instance to which a method is bound", QP_GETSET_MEMBER },
    { "__doc__", qp_fo_method_get, NULL, QP_MA_DOC, NULL },
    { NULL }
};

static const QPMethodDef qp_fo_method_methods[] = {
    { "__reduce__", qp_fo_method_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_fo_method_spec = {
    .name = "method",
    .doc = "method(function, instance, /)\n--\n\nCreate a bound instance method object.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_METHOD,
    .slots = {
        .tp_getattro = qp_fo_method_getattro,
        .tp_repr = qp_fo_method_repr,
        .tp_hash = qp_fo_method_hash,
        .tp_call = qp_fo_method_call,
        .tp_richcompare = qp_fo_method_richcompare,
        .tp_descr_get = qp_fo_method_descr_get,
        .tp_new = qp_fo_method_new_type,
    },
    .methods = qp_fo_method_methods,
    .getsets = qp_fo_method_getsets,
};

/* ======================================================================== */
/* cell type                                                                 */
/* ======================================================================== */

static void qp_cell_finalizer(JSRuntime *rt, JSValue val)
{
    QPCell *c = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!c)
        return;
    free_var_ref(rt, c->var_ref);
    js_free_rt(rt, c);
}

static void qp_cell_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPCell *c = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (c && c->var_ref)
        mark_func(rt, &c->var_ref->header);
}

static JSValue qp_cell_new(JSContext *ctx, struct JSVarRef *var_ref)
{
    QPCell *c;
    JSValue obj;

    c = js_malloc(ctx, sizeof(*c));
    if (!c)
        return JS_EXCEPTION;
    if (var_ref) {
        js_rc(var_ref)->ref_count++;
    } else {
        var_ref = js_create_var_ref(ctx, TRUE);   /* empty (JS_UNINITIALIZED) */
        if (!var_ref) {
            js_free(ctx, c);
            return JS_EXCEPTION;
        }
    }
    c->var_ref = var_ref;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, cell)), JS_CLASS_PY_CELL);
    if (JS_IsException(obj)) {
        free_var_ref(ctx->rt, var_ref);
        js_free(ctx, c);
        return JS_EXCEPTION;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = c;
    return obj;
}

static QPCell *qp_fo_cell_data(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_CELL)
        return NULL;
    return JS_VALUE_GET_OBJ(v)->u.opaque;
}

/* PyFunction_SetClosure: func_closure = closure (a tuple, or JS_UNDEFINED
   for None; the items are not checked, as in CPython).  The free variables
   the code reads become the given cells. */
static __maybe_unused void qp_fo_set_closure(JSContext *ctx, JSValueConst func, JSValueConst closure)
{
    JSObject *p = JS_VALUE_GET_OBJ(func);
    JSFunctionBytecode *b = p->u.func.function_bytecode;
    QPFuncDataPriv *pd = (QPFuncDataPriv *)qp_function_data(func);
    int i, n;

    if (!JS_IsUndefined(closure) && p->u.func.var_refs) {
        n = (int)QP_TUPLE_SIZE(closure);
        for (i = 0; i < b->closure_var_count && i < n; i++) {
            QPCell *c = qp_fo_cell_data(QP_TUPLE_ITEMS(closure)[i]);
            if (!c)
                continue;
            js_rc(c->var_ref)->ref_count++;
            free_var_ref(ctx->rt, p->u.func.var_refs[i]);
            p->u.func.var_refs[i] = c->var_ref;
        }
    }
    JS_FreeValue(ctx, pd->closure);
    pd->closure = JS_DupValue(ctx, closure);
}

static JSValue qp_cell_get(JSContext *ctx, JSValueConst cell)
{
    QPCell *c = qp_fo_cell_data(cell);
    JSValue v;

    if (!c)
        return QP_NULL;
    v = *c->var_ref->pvalue;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED)
        return QP_NULL;
    return JS_DupValue(ctx, v);
}

static int qp_cell_set(JSContext *ctx, JSValueConst cell, JSValueConst v)
{
    QPCell *c = qp_fo_cell_data(cell);
    JSValue old;

    if (!c) {
        qp_raise_system_error(ctx, "bad argument to internal function");
        return -1;
    }
    old = *c->var_ref->pvalue;
    *c->var_ref->pvalue = QP_IS_NULL(v) ? JS_UNINITIALIZED : JS_DupValue(ctx, v);
    JS_FreeValue(ctx, old);
    return 0;
}

static JSValue qp_fo_cell_repr(JSContext *ctx, JSValueConst self)
{
    JSValue ref = qp_cell_get(ctx, self), tn, res;

    if (QP_IS_NULL(ref))
        return qp_str_from_format(ctx, "<cell at %p: empty>", JS_VALUE_GET_PTR(self));
    tn = qp_fo_type_name(ctx, qp_type_of(ctx, ref));
    if (JS_IsException(tn)) {
        JS_FreeValue(ctx, ref);
        return JS_EXCEPTION;
    }
    res = qp_str_from_format(ctx, "<cell at %p: %.80U object at %p>", JS_VALUE_GET_PTR(self),
                             tn, JS_VALUE_HAS_REF_COUNT(ref) ? JS_VALUE_GET_PTR(ref)
                             : (void *)(uintptr_t)JS_VALUE_GET_INT(ref));
    JS_FreeValue(ctx, tn);
    JS_FreeValue(ctx, ref);
    return res;
}

/* cells compare by contents; empty cells come before anything else */
static JSValue qp_fo_cell_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    JSValue va, vb, res;
    int x, y, r;

    if (!qp_fo_cell_data(a) || !qp_fo_cell_data(b))
        return qp_not_implemented(ctx);
    va = qp_cell_get(ctx, a);
    vb = qp_cell_get(ctx, b);
    if (!QP_IS_NULL(va) && !QP_IS_NULL(vb)) {
        res = qp_richcompare(ctx, va, vb, op);
        JS_FreeValue(ctx, va);
        JS_FreeValue(ctx, vb);
        return res;
    }
    /* Py_RETURN_RICHCOMPARE(b == NULL, a == NULL, op) */
    x = QP_IS_NULL(vb);
    y = QP_IS_NULL(va);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    switch (op) {
    case QP_CMP_LT: r = x < y; break;
    case QP_CMP_LE: r = x <= y; break;
    case QP_CMP_EQ: r = x == y; break;
    case QP_CMP_NE: r = x != y; break;
    case QP_CMP_GT: r = x > y; break;
    default: r = x >= y; break;
    }
    return qp_bool_new(r);
}

static JSValue qp_fo_cell_contents_get(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue v;

    if (!qp_fo_cell_data(self))
        return qp_raise_type_error(ctx, "descriptor requires a 'cell' object but received '%T'", self);
    v = qp_cell_get(ctx, self);
    if (QP_IS_NULL(v))
        return qp_raise_value_error(ctx, "Cell is empty");
    return v;
}

static int qp_fo_cell_contents_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    return qp_cell_set(ctx, self, value);
}

/* cell([contents]) */
static JSValue qp_fo_cell_new_type(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    JSValue cell;

    if (qp_arg_no_keywords(ctx, "cell", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "cell", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    cell = qp_cell_new(ctx, NULL);
    if (JS_IsException(cell))
        return JS_EXCEPTION;
    if (nargs == 1)
        qp_cell_set(ctx, cell, args[0]);
    return cell;
}

static const QPGetSetDef qp_fo_cell_getsets[] = {
    { "cell_contents", qp_fo_cell_contents_get, qp_fo_cell_contents_set, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_fo_cell_spec = {
    .name = "cell",
    .doc = "cell([contents])\n--\n\n"
           "Create a new cell object.\n\n"
           "  contents\n"
           "    the contents of the cell. If not specified, the cell will be empty,\n"
           "    and further attempts to access its cell_contents attribute will\n"
           "    raise a ValueError.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_CELL,
    .slots = {
        .tp_repr = qp_fo_cell_repr,
        .tp_richcompare = qp_fo_cell_richcompare,
        .tp_new = qp_fo_cell_new_type,
    },
    .getsets = qp_fo_cell_getsets,
};

/* ======================================================================== */
/* init                                                                      */
/* ======================================================================== */

static const QPTypeSpec qp_co_code_spec;
static const QPTypeSpec qp_fr_frame_spec;

static int qp_init_funcobject(JSContext *ctx)
{
    if (qp_type_ready(ctx, QP_TYPE(ctx, function), &qp_fo_function_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, method), &qp_fo_method_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, cell), &qp_fo_cell_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, code), &qp_co_code_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, frame), &qp_fr_frame_spec) < 0)
        return -1;
    return 0;
}
