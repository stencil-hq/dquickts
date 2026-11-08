/*
 * The C-API half of _testinternalcapi (python/mod_testinternalcapi.c): the
 * functions of CPython's Modules/_testinternalcapi.c (+ _testinternalcapi/
 * complex.c) that exercise C API entry points, written as CPython wrote
 * them (PyMethodDef, PyArg_Parse*).  The module's QP functions
 * QP_TIC_CAPI_* forward to them through qp_capi_tic_call, which binds the
 * C API on the first call only: importing _testinternalcapi (test_class,
 * test_with) does not bind the context to the C API.
 */

/* ---- pending calls ------------------------------------------------------ */

static int qp_capi_tic_pending_callback(void *arg)
{
    /* we assume the argument is callable object to which we own a reference */
    PyObject *callable = (PyObject *)arg;
    PyObject *r = PyObject_CallNoArgs(callable);
    Py_DECREF(callable);
    Py_XDECREF(r);
    return r != NULL ? 0 : -1;
}

/* pending_threadfunc(callback, num=1, *, blocking=False, ensure_added=False):
   queue num calls of callback on the interpreter queue (any thread runs
   them, _PyEval_AddPendingCall(interp, ..., 0)); the number queued. */
static PyObject *qp_capi_tic_pending_threadfunc(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *callable;
    unsigned int num = 1;
    int blocking = 0;
    int ensure_added = 0;
    static char *kwlist[] = {(char *)"callback", (char *)"num", (char *)"blocking",
                             (char *)"ensure_added", NULL};
    PyThreadState *save_tstate = NULL;
    unsigned int i, num_added;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|I$pp:pending_threadfunc", kwlist,
                                     &callable, &num, &blocking, &ensure_added))
        return NULL;
    /* create the reference for the callback while we hold the lock */
    for (i = 0; i < num; i++)
        Py_INCREF(callable);
    if (!blocking)
        save_tstate = PyEval_SaveThread();
    for (num_added = 0; num_added < num; num_added++) {
        if (ensure_added) {
            while (qp_capi_add_pending(&qp_capi_pending_interp, qp_capi_tic_pending_callback,
                                       callable) < 0)
                ;
        } else if (qp_capi_add_pending(&qp_capi_pending_interp, qp_capi_tic_pending_callback,
                                       callable) < 0) {
            break;
        }
    }
    if (!blocking)
        PyEval_RestoreThread(save_tstate);
    for (i = num_added; i < num; i++)
        Py_DECREF(callable); /* unsuccessful add, destroy the extra reference */
    /* The callable is decref'ed in qp_capi_tic_pending_callback(). */
    return PyLong_FromUnsignedLong((unsigned long)num_added);
}

static PyObject *qp_capi_tic_simple_pending_call(PyObject *self, PyObject *callable)
{
    if (qp_capi_add_pending(&qp_capi_pending_interp, qp_capi_tic_pending_callback,
                            Py_NewRef(callable)) < 0) {
        Py_DECREF(callable);
        PyErr_SetString(PyExc_RuntimeError, "the pending call queue is full");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* ---- get_configs: _Py_GetConfigsAsDict ---------------------------------- */

static int qp_capi_tic_set_item(PyObject *dict, const char *key, PyObject *value)
{
    int res;

    if (value == NULL)
        return -1;
    res = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return res;
}

/* _Py_GetGlobalVariablesAsDict */
static PyObject *qp_capi_tic_global_config(void)
{
    PyObject *dict = PyDict_New();

    if (dict == NULL)
        return NULL;
_Py_COMP_DIAG_PUSH
_Py_COMP_DIAG_IGNORE_DEPR_DECLS
#define QP_TIC_INT(VAR) qp_capi_tic_set_item(dict, #VAR, PyLong_FromLong(VAR)) < 0
#define QP_TIC_STR(VAR) \
    qp_capi_tic_set_item(dict, #VAR, VAR ? PyUnicode_FromString(VAR) : Py_NewRef(Py_None)) < 0
    if (QP_TIC_STR(Py_FileSystemDefaultEncoding) ||
        QP_TIC_INT(Py_HasFileSystemDefaultEncoding) ||
        QP_TIC_STR(Py_FileSystemDefaultEncodeErrors) ||
        QP_TIC_INT(Py_UTF8Mode) ||
        QP_TIC_INT(Py_DebugFlag) ||
        QP_TIC_INT(Py_VerboseFlag) ||
        QP_TIC_INT(Py_QuietFlag) ||
        QP_TIC_INT(Py_InteractiveFlag) ||
        QP_TIC_INT(Py_InspectFlag) ||
        QP_TIC_INT(Py_OptimizeFlag) ||
        QP_TIC_INT(Py_NoSiteFlag) ||
        QP_TIC_INT(Py_BytesWarningFlag) ||
        QP_TIC_INT(Py_FrozenFlag) ||
        QP_TIC_INT(Py_IgnoreEnvironmentFlag) ||
        QP_TIC_INT(Py_DontWriteBytecodeFlag) ||
        QP_TIC_INT(Py_NoUserSiteDirectory) ||
        QP_TIC_INT(Py_UnbufferedStdioFlag) ||
        QP_TIC_INT(Py_HashRandomizationFlag) ||
        QP_TIC_INT(Py_IsolatedFlag)) {
        Py_DECREF(dict);
        return NULL;
    }
#undef QP_TIC_INT
#undef QP_TIC_STR
_Py_COMP_DIAG_POP
    return dict;
}

/* get_configs(): {'global_config', 'pre_config', 'config'}; the two config
   dicts hold the PyConfig_Get() value of every option of the PEP 741 table
   (the preconfig ones as ints, as _PyPreConfig_AsDict gives them). */
static PyObject *qp_capi_tic_get_configs(PyObject *self, PyObject *unused)
{
    static const char *const pre_names[] = {
        "allocator", "coerce_c_locale", "coerce_c_locale_warn", "configure_locale",
        "dev_mode", "isolated", "parse_argv", "use_environment", "utf8_mode", NULL,
    };
    const QPConfigSpec *spec;
    const char *const *name;
    PyObject *result, *config, *pre;
    int value;

    result = PyDict_New();
    if (result == NULL)
        return NULL;
    if (qp_capi_tic_set_item(result, "global_config", qp_capi_tic_global_config()) < 0)
        goto error;
    pre = PyDict_New();
    if (qp_capi_tic_set_item(result, "pre_config", Py_XNewRef(pre)) < 0) {
        Py_XDECREF(pre);
        goto error;
    }
    for (name = pre_names; *name; name++) {
        if (PyConfig_GetInt(*name, &value) < 0 ||
            qp_capi_tic_set_item(pre, *name, PyLong_FromLong(value)) < 0) {
            Py_DECREF(pre);
            goto error;
        }
    }
    Py_DECREF(pre);
    config = PyDict_New();
    if (qp_capi_tic_set_item(result, "config", Py_XNewRef(config)) < 0) {
        Py_XDECREF(config);
        goto error;
    }
    for (spec = qp_config_spec; spec->name; spec++) {
        if (qp_capi_tic_set_item(config, spec->name, qp_config_get(spec)) < 0) {
            Py_DECREF(config);
            goto error;
        }
    }
    Py_DECREF(config);
    return result;
 error:
    Py_DECREF(result);
    return NULL;
}

/* ---- objects, errors, str ----------------------------------------------- */

static PyObject *qp_capi_tic_is_static_immortal(PyObject *self, PyObject *op)
{
    return PyBool_FromLong(_Py_IsStaticImmortal(op));
}

/* _PyErr_SetKeyError() overrides the current exception */
static PyObject *qp_capi_tic_pyerr_setkeyerror(PyObject *self, PyObject *arg)
{
    PyObject *exc;

    PyErr_NoMemory();
    /* _PyErr_SetKeyError(arg) */
    PyErr_Clear();
    exc = PyObject_CallOneArg(PyExc_KeyError, arg);
    if (exc) {
        PyErr_SetObject((PyObject *)Py_TYPE(exc), exc);
        Py_DECREF(exc);
    }
    return NULL;
}

/* _PyUnicode_TransformDecimalAndSpaceToASCII(): non-ASCII spaces become ' ',
   decimal digits their ASCII digit; the result stops after a '?' at the
   first other non-ASCII character. */
static PyObject *qp_capi_tic_transform_decimal(PyObject *self, PyObject *unicode)
{
    Py_ssize_t len, i;
    PyObject *result;
    Py_UCS1 *out;
    const void *data;
    int kind;

    if (unicode == Py_None || !PyUnicode_Check(unicode)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (PyUnicode_IS_ASCII(unicode))
        return Py_NewRef(unicode);
    len = PyUnicode_GET_LENGTH(unicode);
    kind = PyUnicode_KIND(unicode);
    data = PyUnicode_DATA(unicode);
    for (i = 0; i < len; i++) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i);
        if (ch >= 127 && !Py_UNICODE_ISSPACE(ch) && Py_UNICODE_TODECIMAL(ch) < 0) {
            len = i + 1;
            break;
        }
    }
    result = PyUnicode_New(len, 127);
    if (result == NULL)
        return NULL;
    out = PyUnicode_1BYTE_DATA(result);
    for (i = 0; i < len; i++) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i);
        if (ch < 127)
            out[i] = (Py_UCS1)ch;
        else if (Py_UNICODE_ISSPACE(ch))
            out[i] = ' ';
        else if (Py_UNICODE_TODECIMAL(ch) >= 0)
            out[i] = (Py_UCS1)('0' + Py_UNICODE_TODECIMAL(ch));
        else
            out[i] = '?';
    }
    return result;
}

/* ---- set: _testinternalcapi/set.c --------------------------------------- */

/* NULLABLE() of Modules/_testcapi/util.h */
static PyObject *qp_capi_tic_nullable(PyObject *obj)
{
    return obj == Py_None ? NULL : obj;
}

static PyObject *qp_capi_tic_set_update(PyObject *self, PyObject *args)
{
    PyObject *set, *iterable;
    int r;

    if (!PyArg_ParseTuple(args, "OO", &set, &iterable))
        return NULL;
    set = qp_capi_tic_nullable(set);
    iterable = qp_capi_tic_nullable(iterable);
    r = _PySet_Update(set, iterable);
    if (r == -1 && PyErr_Occurred())
        return NULL;
    return PyLong_FromLong(r);   /* RETURN_INT */
}

static PyObject *qp_capi_tic_set_next_entry(PyObject *self, PyObject *args)
{
    int rc;
    Py_ssize_t pos;
    Py_hash_t hash = (Py_hash_t)-1;
    PyObject *set, *item = NULL, *ret;

    if (!PyArg_ParseTuple(args, "On", &set, &pos))
        return NULL;
    set = qp_capi_tic_nullable(set);
    rc = _PySet_NextEntryRef(set, &pos, &item, &hash);
    if (rc == 1) {
        ret = Py_BuildValue("innO", rc, pos, hash, item);
        Py_DECREF(item);
        return ret;
    }
    if (rc == -1)
        return NULL;
    Py_RETURN_NONE;
}

/* ---- module_get_gc_hooks(mod): (traverse, clear, free) as ints ---------- */

static PyObject *qp_capi_tic_module_get_gc_hooks(PyObject *self, PyObject *arg)
{
    QPCapiModuleObject *mod = (QPCapiModuleObject *)arg;
    PyObject *traverse = NULL, *clear = NULL, *free = NULL, *result = NULL;

    traverse = PyLong_FromVoidPtr((void *)mod->md_state_traverse);
    if (!traverse)
        goto finally;
    clear = PyLong_FromVoidPtr((void *)mod->md_state_clear);
    if (!clear)
        goto finally;
    free = PyLong_FromVoidPtr((void *)mod->md_state_free);
    if (!free)
        goto finally;
    result = PyTuple_FromArray((PyObject *[]){ traverse, clear, free }, 3);
finally:
    Py_XDECREF(traverse);
    Py_XDECREF(clear);
    Py_XDECREF(free);
    return result;
}

/* ---- complex: _testinternalcapi/complex.c ------------------------------- */

#define QP_TIC_CR_FUNC2(suffix)                                              \
    static PyObject *qp_capi_tic_py_cr_##suffix(PyObject *self, PyObject *args) \
    {                                                                        \
        Py_complex a, res;                                                   \
        double b;                                                            \
        if (!PyArg_ParseTuple(args, "Dd", &a, &b))                           \
            return NULL;                                                     \
        errno = 0;                                                           \
        res = _Py_cr_##suffix(a, b);                                         \
        return Py_BuildValue("Di", &res, errno);                             \
    }
#define QP_TIC_RC_FUNC2(suffix)                                              \
    static PyObject *qp_capi_tic_py_rc_##suffix(PyObject *self, PyObject *args) \
    {                                                                        \
        Py_complex b, res;                                                   \
        double a;                                                            \
        if (!PyArg_ParseTuple(args, "dD", &a, &b))                           \
            return NULL;                                                     \
        errno = 0;                                                           \
        res = _Py_rc_##suffix(a, b);                                         \
        return Py_BuildValue("Di", &res, errno);                             \
    }
QP_TIC_CR_FUNC2(sum)
QP_TIC_CR_FUNC2(diff)
QP_TIC_RC_FUNC2(diff)
QP_TIC_CR_FUNC2(prod)
QP_TIC_CR_FUNC2(quot)
QP_TIC_RC_FUNC2(quot)
#undef QP_TIC_CR_FUNC2
#undef QP_TIC_RC_FUNC2

/* ---- bridge -------------------------------------------------------------- */

static PyMethodDef qp_capi_tic_defs[QP_TIC_CAPI__COUNT] = {
    [QP_TIC_CAPI_PENDING_THREADFUNC] = {
        "pending_threadfunc", _PyCFunction_CAST(qp_capi_tic_pending_threadfunc),
        METH_VARARGS | METH_KEYWORDS, NULL },
    [QP_TIC_CAPI_SIMPLE_PENDING_CALL] = {
        "simple_pending_call", qp_capi_tic_simple_pending_call, METH_O, NULL },
    [QP_TIC_CAPI_GET_CONFIGS] = { "get_configs", qp_capi_tic_get_configs, METH_NOARGS, NULL },
    [QP_TIC_CAPI_IS_STATIC_IMMORTAL] = {
        "is_static_immortal", qp_capi_tic_is_static_immortal, METH_O, NULL },
    [QP_TIC_CAPI_PYERR_SETKEYERROR] = {
        "_pyerr_setkeyerror", qp_capi_tic_pyerr_setkeyerror, METH_O, NULL },
    [QP_TIC_CAPI_TRANSFORM_DECIMAL] = {
        "_PyUnicode_TransformDecimalAndSpaceToASCII", qp_capi_tic_transform_decimal,
        METH_O, NULL },
    [QP_TIC_CAPI_PY_CR_SUM] = { "_py_cr_sum", qp_capi_tic_py_cr_sum, METH_VARARGS, NULL },
    [QP_TIC_CAPI_PY_CR_DIFF] = { "_py_cr_diff", qp_capi_tic_py_cr_diff, METH_VARARGS, NULL },
    [QP_TIC_CAPI_PY_RC_DIFF] = { "_py_rc_diff", qp_capi_tic_py_rc_diff, METH_VARARGS, NULL },
    [QP_TIC_CAPI_PY_CR_PROD] = { "_py_cr_prod", qp_capi_tic_py_cr_prod, METH_VARARGS, NULL },
    [QP_TIC_CAPI_PY_CR_QUOT] = { "_py_cr_quot", qp_capi_tic_py_cr_quot, METH_VARARGS, NULL },
    [QP_TIC_CAPI_PY_RC_QUOT] = { "_py_rc_quot", qp_capi_tic_py_rc_quot, METH_VARARGS, NULL },
    [QP_TIC_CAPI_SET_UPDATE] = { "set_update", qp_capi_tic_set_update, METH_VARARGS, NULL },
    [QP_TIC_CAPI_SET_NEXT_ENTRY] = {
        "set_next_entry", qp_capi_tic_set_next_entry, METH_VARARGS, NULL },
    [QP_TIC_CAPI_MODULE_GET_GC_HOOKS] = {
        "module_get_gc_hooks", qp_capi_tic_module_get_gc_hooks, METH_O, NULL },
};

/* The QP function of entry `magic` of qp_capi_tic_defs: bind the C API,
   then call the entry as a builtin of the C API (its calling convention,
   argument checks and error messages). */
static JSValue qp_capi_tic_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    PyObject *f;
    JSValue fv, r;

    if (qp_capi_init(ctx) < 0)
        return JS_EXCEPTION;
    f = PyCFunction_NewEx(&qp_capi_tic_defs[magic], NULL, NULL);
    if (f == NULL)
        return JS_EXCEPTION;
    fv = qp_capi_to_js(ctx, f);
    Py_DECREF(f);
    if (JS_IsException(fv))
        return fv;
    r = qp_vectorcall(ctx, fv, args, nargs, kwnames);
    JS_FreeValue(ctx, fv);
    return r;
}
