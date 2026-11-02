/*
 * dquickts C API (D5, unit E1): exceptions and the error indicator,
 * warnings, tracebacks, and the exception init step (qp_capi_init_errors).
 *
 * Semantics ported from CPython 3.15.2 Python/errors.c,
 * Objects/exceptions.c (PyException_*, PyUnicode*Error_*),
 * Python/_warnings.c (C API), Python/traceback.c (PyTraceBack_*),
 * Python/pythonrun.c (PyErr_Print*, PyErr_Display*) and
 * Modules/signalmodule.c (PyErr_CheckSignals, PyErr_SetInterrupt*)
 * (PSF-2.0).  Part of the quickjs.c translation unit, after capi_dict.c.
 *
 * The error indicator is the pending exception of qp_capi.ctx: the
 * PyErr_* functions are thin layers over the M2 raising machinery
 * (python/errors.c, python/exceptions.c), so implicit chaining
 * (__context__), normalization and unraisable reports behave exactly as
 * for Python code.  Every builtin exception class is a static immortal
 * PyTypeObject (bound to its QPType at init) behind its PyExc_* pointer.
 */

/* Internal exports (CPython pycore_pyerrors.h, pycore_traceback.h). */
PyAPI_FUNC(PyObject *) _PyErr_FormatFromCause(PyObject *exception, const char *format, ...);
PyAPI_FUNC(void) _PyErr_FormatNote(const char *format, ...);
PyAPI_FUNC(int) _PyException_AddNote(PyObject *exc, PyObject *note);
PyAPI_FUNC(void) _PyTraceback_Add(const char *funcname, const char *filename, int lineno);
#ifndef MS_WINDOWS
/* Declared by pyerrors.h on Windows only; exported everywhere (on POSIX
   the Windows error code is passed to OSError as `winerror`, which the
   constructor ignores there). */
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilename(int ierr, const char *filename);
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErr(int ierr);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObject(PyObject *exc, int ierr,
                                                                    PyObject *filename);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObjects(PyObject *exc, int ierr,
                                                                     PyObject *filename,
                                                                     PyObject *filename2);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilename(PyObject *exc, int ierr,
                                                              const char *filename);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErr(PyObject *exc, int ierr);
#endif

/* Private helpers of the M2/M3 units (same translation unit). */
static QPExcStackItem *qp_exc_topmost(JSContext *ctx);              /* errors.c */
static volatile sig_atomic_t qp_signal_wakeup_fd;                   /* mod_misc.c */
static volatile sig_atomic_t qp_signal_wakeup_warn;                 /* mod_misc.c */

/* ========================================================================= */
/* Exception classes                                                          */
/* ========================================================================= */

/* One static immortal PyTypeObject per QP_EXCEPTIONS entry; tp_name,
   tp_basicsize and tp_base are completed by qp_capi_init_errors before
   the type is bound to its QPType. */
#define QP_CAPI_EXC_TYPE_INIT(name, base)                                   \
    [QP_EXC_ ## name] = {                                                   \
        PyVarObject_HEAD_INIT(&PyType_Type, 0)                              \
        .tp_name = #name,                                                   \
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |              \
                    Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC,      \
    },
static PyTypeObject qp_capi_exc_types[QP_EXC__COUNT] = {
    QP_EXCEPTIONS(QP_CAPI_EXC_TYPE_INIT)
};
#undef QP_CAPI_EXC_TYPE_INIT

#define QP_CAPI_EXC_BASE_INIT(name, base) [QP_EXC_ ## name] = QP_EXC_ ## base,
static const uint8_t qp_capi_exc_base[QP_EXC__COUNT] = {
    QP_EXCEPTIONS(QP_CAPI_EXC_BASE_INIT)
};
#undef QP_CAPI_EXC_BASE_INIT

#define QP_CAPI_EXC_OBJ(name) ((PyObject *)&qp_capi_exc_types[QP_EXC_ ## name])

/* The PyExc_* pointers declared by pyerrors.h (every QP_EXCEPTIONS entry
   except the private _IncompleteInputError and js.JsError). */
#define QP_CAPI_PYEXC(X)                                                     \
    X(BaseException) X(Exception) X(TypeError) X(StopAsyncIteration)         \
    X(StopIteration) X(GeneratorExit) X(SystemExit) X(KeyboardInterrupt)     \
    X(BaseExceptionGroup) X(ExceptionGroup) X(ImportError)                   \
    X(ImportCycleError) X(ModuleNotFoundError) X(OSError)                    \
    X(BlockingIOError) X(ConnectionError) X(ChildProcessError)               \
    X(BrokenPipeError) X(ConnectionAbortedError) X(ConnectionRefusedError)   \
    X(ConnectionResetError) X(FileExistsError) X(FileNotFoundError)          \
    X(IsADirectoryError) X(NotADirectoryError) X(InterruptedError)           \
    X(PermissionError) X(ProcessLookupError) X(TimeoutError) X(EOFError)     \
    X(RuntimeError) X(RecursionError) X(PythonFinalizationError)             \
    X(NotImplementedError) X(NameError) X(UnboundLocalError)                 \
    X(AttributeError) X(SyntaxError) X(IndentationError) X(TabError)         \
    X(LookupError) X(IndexError) X(KeyError) X(ValueError) X(UnicodeError)   \
    X(UnicodeEncodeError) X(UnicodeDecodeError) X(UnicodeTranslateError)     \
    X(AssertionError) X(ArithmeticError) X(FloatingPointError)               \
    X(OverflowError) X(ZeroDivisionError) X(SystemError) X(ReferenceError)   \
    X(MemoryError) X(BufferError) X(Warning) X(UserWarning)                  \
    X(EncodingWarning) X(DeprecationWarning) X(PendingDeprecationWarning)    \
    X(SyntaxWarning) X(RuntimeWarning) X(FutureWarning) X(ImportWarning)     \
    X(UnicodeWarning) X(BytesWarning) X(ResourceWarning)

#define QP_CAPI_PYEXC_DEF(name) PyObject *PyExc_ ## name = QP_CAPI_EXC_OBJ(name);
QP_CAPI_PYEXC(QP_CAPI_PYEXC_DEF)
#undef QP_CAPI_PYEXC_DEF

/* compatibility aliases */
PyObject *PyExc_EnvironmentError = QP_CAPI_EXC_OBJ(OSError);
PyObject *PyExc_IOError = QP_CAPI_EXC_OBJ(OSError);
#ifdef MS_WINDOWS
PyObject *PyExc_WindowsError = QP_CAPI_EXC_OBJ(OSError);
#endif

/* tp_basicsize of a builtin exception class: CPython's instance struct of
   the class or of its nearest base that has one. */
static Py_ssize_t qp_capi_exc_struct_size(int id)
{
    for (;;) {
        switch (id) {
        case QP_EXC_StopIteration:
            return sizeof(PyStopIterationObject);
        case QP_EXC_SystemExit:
            return sizeof(PySystemExitObject);
        case QP_EXC_BaseExceptionGroup:
            return sizeof(PyBaseExceptionGroupObject);
        case QP_EXC_ImportError:
            return sizeof(PyImportErrorObject);
        case QP_EXC_OSError:
            return sizeof(PyOSErrorObject);
        case QP_EXC_NameError:
            return sizeof(PyNameErrorObject);
        case QP_EXC_AttributeError:
            return sizeof(PyAttributeErrorObject);
        case QP_EXC_SyntaxError:
            return sizeof(PySyntaxErrorObject);
        case QP_EXC_UnicodeError:
            return sizeof(PyUnicodeErrorObject);
        case QP_EXC_BaseException:
            return sizeof(PyBaseExceptionObject);
        default:
            id = qp_capi_exc_base[id];
            break;
        }
    }
}

/* object.h hook: the bound PyTypeObject of t (C extension types, static
   types after the C-API init), else the static one of a builtin type or
   exception class (also before the C-API init). */
static BOOL qp_capi_type_layout(JSContext *ctx, QPType *t, QPTypeLayout *l)
{
    QPState *s = QP_STATE(ctx);
    /* the type maps are those of the runtime the C API is bound to */
    PyTypeObject *tp = qp_capi_owned_by(ctx->rt) ? qp_capi_type_by_qp_any(t) : NULL;
    size_t i;

    for (i = 0; !tp && i < countof(qp_capi_static_table); i++) {
        if (s->types[qp_capi_static_table[i].id] == t)
            tp = qp_capi_static_table[i].tp;
    }
    if (tp) {
        l->basicsize = tp->tp_basicsize;
        l->itemsize = tp->tp_itemsize;
        l->dictoffset = tp->tp_dictoffset;
        l->weaklistoffset = tp->tp_weaklistoffset;
        return TRUE;
    }
    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (s->exc_types[i] == t) {
            l->basicsize = qp_capi_exc_struct_size((int)i);
            l->itemsize = 0;
            l->dictoffset = l->weaklistoffset = 0;
            return TRUE;
        }
    }
    return FALSE;
}

/* EXCEPTION kind size: the builtin layout of the nearest builtin exception
   class (C fields of extension subclasses are sized by A as natives). */
static size_t qp_capi_exc_size(JSContext *ctx, JSValueConst v)
{
    PyTypeObject *tp = qp_capi_type_of(ctx, v);
    Py_ssize_t sz = sizeof(PyBaseExceptionObject);

    for (; tp; tp = tp->tp_base) {
        if (tp >= &qp_capi_exc_types[0] && tp < &qp_capi_exc_types[QP_EXC__COUNT]) {
            if (tp->tp_basicsize > sz)
                sz = tp->tp_basicsize;
            break;
        }
    }
    return (size_t)sz - sizeof(PyObject);
}

static int qp_capi_init_errors(JSContext *ctx)
{
    int i;

    qp_capi.kind_ops[QP_CAPI_K_EXCEPTION] = (QPCapiKindOps){
        .name = "exception",
        .size = qp_capi_exc_size,
    };
    /* QP_EXCEPTIONS lists bases first */
    for (i = 0; i < QP_EXC__COUNT; i++) {
        PyTypeObject *tp = &qp_capi_exc_types[i];
        QPType *t = QP_STATE(ctx)->exc_types[i];
        if (!t) {
            qp_raise_system_error(ctx, "exception class %s is not initialized", tp->tp_name);
            return -1;
        }
        if (t->tp_name)
            tp->tp_name = t->tp_name;
        tp->tp_basicsize = qp_capi_exc_struct_size(i);
        tp->tp_base = i == QP_EXC_BaseException ? &PyBaseObject_Type
                                                : &qp_capi_exc_types[qp_capi_exc_base[i]];
        if (qp_capi_bind_static_type(ctx, tp, t) < 0)
            return -1;
    }
    qp_capi.exc_pins = NULL;
    return 0;
}

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

/* QPType of an exception class, NULL if `type` is not one (no error). */
static QPType *qp_capi_exc_class(JSContext *ctx, PyObject *type)
{
    JSValue v;
    QPType *t;

    if (!type || !PyExceptionClass_Check(type))
        return NULL;
    v = qp_capi_value(ctx, type);
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
        return NULL;
    }
    t = qp_type_from_value(v);
    if (!t || !qp_exc_type_check(v))
        return NULL;
    return t;
}

/* _PyErr_SetObject(Py_TYPE(v), v) for a JS value (consumed). */
static void qp_capi_err_raise_instance(JSContext *ctx, JSValue v)
{
    qp_raise_value(ctx, qp_type_of(ctx, v), v);
    JS_FreeValue(ctx, v);
}

/* The JS object of the last uncatchable value handed out by
   PyErr_GetRaisedException (an interrupt): restoring it keeps it
   uncatchable.  Identity only, not counted. */
static void *qp_capi_uncatchable_ptr;

/* ========================================================================= */
/* Setting and querying the error indicator                                   */
/* ========================================================================= */

void PyErr_SetObject(PyObject *exception, PyObject *value)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t;
    JSValue v = QP_NULL;

    if (exception == NULL) {
        /* CPython restores (type(value), value) */
        if (value && PyExceptionInstance_Check(value)) {
            v = qp_capi_to_js(ctx, value);
            if (!JS_IsException(v))
                qp_raise_object(ctx, v);
        } else {
            qp_err_clear(ctx);
        }
        return;
    }
    t = qp_capi_exc_class(ctx, exception);
    if (!t) {
        PyErr_Format(PyExc_SystemError,
                     "_PyErr_SetObject: exception %R is not a BaseException subclass",
                     exception);
        return;
    }
    if (value) {
        v = qp_capi_value(ctx, value);
        if (JS_IsException(v))
            return;
    }
    qp_raise_value(ctx, t, v);
}

void PyErr_SetNone(PyObject *exception)
{
    PyErr_SetObject(exception, NULL);
}

void PyErr_SetString(PyObject *exception, const char *string)
{
    PyObject *value = PyUnicode_FromString(string);
    if (value != NULL) {
        PyErr_SetObject(exception, value);
        Py_DECREF(value);
    }
}

/* pycore_pyerrors.h: the message is a locale-encoded C string (strerror,
   dlerror) */
PyAPI_FUNC(void) _PyErr_SetLocaleString(PyObject *exception, const char *string)
{
    PyObject *value = PyUnicode_DecodeLocale(string, "surrogateescape");
    if (value != NULL) {
        PyErr_SetObject(exception, value);
        Py_DECREF(value);
    }
}

PyObject *PyErr_FormatV(PyObject *exception, const char *format, va_list vargs)
{
    PyObject *string;

    /* PyUnicode_FromFormatV() must not be called with an exception set
       (it may run arbitrary Python code) */
    qp_err_clear(qp_capi.ctx);
    string = PyUnicode_FromFormatV(format, vargs);
    if (string != NULL) {
        PyErr_SetObject(exception, string);
        Py_DECREF(string);
    }
    return NULL;
}

PyObject *PyErr_Format(PyObject *exception, const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    PyErr_FormatV(exception, format, vargs);
    va_end(vargs);
    return NULL;
}

/* Borrowed type of the pending exception (type mirrors persist). */
PyObject *PyErr_Occurred(void)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiErrSave saved;
    PyTypeObject *tp;
    QPType *t;

    if (!ctx || !JS_HasException(ctx))
        return NULL;
    qp_err_normalize_pending(ctx);
    t = qp_type_of(ctx, ctx->rt->current_exception);
    /* looking the type mirror up may allocate: keep the pending exception */
    qp_capi_err_save(ctx, &saved);
    tp = qp_capi_type_from_qp(ctx, t);
    qp_capi_err_restore(ctx, &saved);
    return tp ? (PyObject *)tp : QP_CAPI_EXC_OBJ(SystemError);
}

void PyErr_Clear(void)
{
    JSContext *ctx = qp_capi.ctx;
    if (ctx)
        qp_err_clear(ctx);
}

PyObject *PyErr_GetRaisedException(void)
{
    JSContext *ctx = qp_capi.ctx;
    BOOL uncatchable;
    JSValue exc;

    if (!JS_HasException(ctx))
        return NULL;
    uncatchable = ctx->rt->current_exception_is_uncatchable;
    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return NULL;
    if (uncatchable && JS_VALUE_GET_TAG(exc) == JS_TAG_OBJECT)
        qp_capi_uncatchable_ptr = JS_VALUE_GET_OBJ(exc);
    return qp_capi_steal(ctx, exc);
}

void PyErr_SetRaisedException(PyObject *exc)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v;

    if (exc == NULL) {
        qp_err_clear(ctx);
        return;
    }
    v = qp_capi_to_js(ctx, exc);
    Py_DECREF(exc);
    if (JS_IsException(v))
        return;
    JS_Throw(ctx, v);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && qp_capi_uncatchable_ptr &&
        JS_VALUE_GET_OBJ(v) == qp_capi_uncatchable_ptr && !qp_exc_check(ctx, v))
        JS_SetUncatchableException(ctx, TRUE);
}

void PyErr_Fetch(PyObject **p_type, PyObject **p_value, PyObject **p_traceback)
{
    PyObject *exc = PyErr_GetRaisedException();
    *p_value = exc;
    if (exc == NULL) {
        *p_type = NULL;
        *p_traceback = NULL;
    } else {
        *p_type = Py_NewRef((PyObject *)Py_TYPE(exc));
        *p_traceback = PyException_GetTraceback(exc);
    }
}

void PyErr_Restore(PyObject *type, PyObject *value, PyObject *traceback)
{
    JSContext *ctx = qp_capi.ctx;

    if (type == NULL) {
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        qp_err_clear(ctx);
        return;
    }
    if (value == NULL || type != (PyObject *)Py_TYPE(value)) {
        QPType *t = qp_capi_exc_class(ctx, type);
        JSValue jv = QP_NULL, exc;
        if (!t) {
            Py_XDECREF(value);
            Py_XDECREF(traceback);
            PyErr_Format(PyExc_SystemError,
                         "PyErr_Restore: exception %R is not a BaseException subclass", type);
            Py_DECREF(type);
            return;
        }
        if (value) {
            jv = qp_capi_value(ctx, value);
            if (JS_IsException(jv))
                goto drop;
        }
        exc = qp_err_create_exception(ctx, t, jv);
        Py_XDECREF(value);
        value = NULL;
        if (JS_IsException(exc))
            goto drop;
        value = qp_capi_steal(ctx, exc);
        if (!value)
            goto drop;
    }
    if (traceback != NULL) {
        if (PyException_SetTraceback(value, traceback) < 0)
            goto drop;
        Py_DECREF(traceback);
    }
    PyErr_SetRaisedException(value);
    Py_DECREF(type);
    return;
 drop:
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    Py_DECREF(type);
}

PyObject *PyErr_GetHandledException(void)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue exc = qp_exc_info_get(ctx);
    if (JS_IsUndefined(exc))
        return NULL;
    return qp_capi_steal(ctx, exc);
}

void PyErr_SetHandledException(PyObject *exc)
{
    JSContext *ctx = qp_capi.ctx;
    QPExcStackItem *it = QP_STATE(ctx)->exc_info;
    JSValue old = it->exc_value, nv = QP_NULL;

    if (exc && exc != Py_None) {
        nv = qp_capi_to_js(ctx, exc);
        if (JS_IsException(nv))
            return;
    }
    it->exc_value = nv;
    JS_FreeValue(ctx, old);
}

void PyErr_GetExcInfo(PyObject **p_type, PyObject **p_value, PyObject **p_traceback)
{
    JSContext *ctx = qp_capi.ctx;
    QPExcStackItem *it = qp_exc_topmost(ctx);
    JSValueConst v = it->exc_value;

    *p_type = *p_value = *p_traceback = NULL;
    if (QP_IS_NULL(v))
        return;
    *p_value = qp_capi_new_ref(ctx, v);
    if (!*p_value || *p_value == Py_None)
        return;
    *p_type = Py_NewRef((PyObject *)Py_TYPE(*p_value));
    *p_traceback = PyException_GetTraceback(*p_value);
}

void PyErr_SetExcInfo(PyObject *type, PyObject *value, PyObject *traceback)
{
    PyErr_SetHandledException(value);
    Py_XDECREF(value);
    /* type and traceback are not needed */
    Py_XDECREF(type);
    Py_XDECREF(traceback);
}

int PyErr_GivenExceptionMatches(PyObject *err, PyObject *exc)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e, x;

    if (err == NULL || exc == NULL)
        return 0;
    /* type objects and exception instances are bound: no allocation */
    e = qp_capi_value(ctx, err);
    if (JS_IsException(e))
        return 0;
    x = qp_capi_value(ctx, exc);
    if (JS_IsException(x))
        return 0;
    return qp_exc_given_matches(ctx, e, x);
}

int PyErr_ExceptionMatches(PyObject *exc)
{
    return PyErr_GivenExceptionMatches(PyErr_Occurred(), exc);
}

/* Used in many places to normalize a raised exception, including in
   eval_code2(), do_raise(), and PyErr_Print(). */
void PyErr_NormalizeException(PyObject **exc, PyObject **val, PyObject **tb)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *type, *value, *initial_tb;
    int recursion_depth = 0;

 restart:
    type = *exc;
    if (type == NULL)
        return;     /* there was no exception, so nothing to do */
    value = *val;
    if (!value)
        value = Py_NewRef(Py_None);
    /* Normalize the exception so that if the type is a class, the value
       will be an instance. */
    if (PyExceptionClass_Check(type)) {
        PyObject *inclass = NULL;
        int is_subclass = 0;

        if (PyExceptionInstance_Check(value)) {
            inclass = PyExceptionInstance_Class(value);
            is_subclass = PyObject_IsSubclass(inclass, type);
            if (is_subclass < 0)
                goto error;
        }
        /* If the value was not an instance, or is not an instance whose
           class is (or is derived from) type, then use the value as an
           argument to instantiation of the type class. */
        if (!is_subclass) {
            QPType *t = qp_capi_exc_class(ctx, type);
            JSValue jv, fixed;
            if (!t) {
                PyErr_Format(PyExc_SystemError, "exception %R is not a BaseException subclass",
                             type);
                goto error;
            }
            jv = qp_capi_value(ctx, value);
            if (JS_IsException(jv))
                goto error;
            fixed = qp_err_create_exception(ctx, t, jv);
            if (JS_IsException(fixed))
                goto error;
            Py_SETREF(value, qp_capi_steal(ctx, fixed));
            if (!value)
                goto error_novalue;
        } else if (inclass != type) {
            /* If the class of the instance doesn't exactly match the class
               of the type, believe the instance. */
            Py_SETREF(type, Py_NewRef(inclass));
        }
    }
    *exc = type;
    *val = value;
    return;

 error:
    Py_DECREF(value);
 error_novalue:
    Py_DECREF(type);
    initial_tb = *tb;
    /* If the new exception doesn't set a traceback and the old exception
       had a traceback, use the old traceback for the new exception.  It's
       better than nothing. */
    PyErr_Fetch(exc, val, tb);
    if (initial_tb != NULL) {
        if (*tb == NULL)
            *tb = initial_tb;
        else
            Py_DECREF(initial_tb);
    }
    /* Abort when Python is unable to normalize the exception even after
       multiple attempts. */
    if (++recursion_depth > 32) {
        PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded "
                        "while normalizing an exception");
        return;
    }
    goto restart;
}

int PyErr_BadArgument(void)
{
    PyErr_SetString(PyExc_TypeError, "bad argument type for built-in operation");
    return 0;
}

PyObject *PyErr_NoMemory(void)
{
    qp_raise_memory_error(qp_capi.ctx);
    return NULL;
}

void _PyErr_BadInternalCall(const char *filename, int lineno)
{
    PyErr_Format(PyExc_SystemError, "%s:%d: bad argument to internal function",
                 filename, lineno);
}

/* Remove the preprocessor macro for PyErr_BadInternalCall() so that we can
   export the entry point for existing object code. */
void (PyErr_BadInternalCall)(void)
{
    PyErr_SetString(PyExc_SystemError, "bad argument to internal function");
}

/* ---- OS errors ----------------------------------------------------------- */

/* exc(*args) raised as _PyErr_SetObject(type(v), v); args are borrowed JS
   values. */
static PyObject *qp_capi_err_call_raise(JSContext *ctx, PyObject *exc, int n, JSValueConst *args)
{
    JSValue e, tup, v;

    e = qp_capi_value(ctx, exc);
    if (JS_IsException(e))
        return NULL;
    tup = qp_tuple_pack(ctx, n, args);
    if (JS_IsException(tup))
        return NULL;
    v = qp_call_tuple(ctx, e, tup, QP_NULL);
    JS_FreeValue(ctx, tup);
    if (!JS_IsException(v))
        qp_capi_err_raise_instance(ctx, v);
    return NULL;
}

PyObject *PyErr_SetFromErrnoWithFilenameObjects(PyObject *exc, PyObject *filenameObject,
                                                PyObject *filenameObject2)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *message;
    JSValue args[5];
    int i = errno, n;

#ifdef EINTR
    if (i == EINTR && PyErr_CheckSignals())
        return NULL;
#endif
    if (i != 0)
        message = PyUnicode_DecodeLocale(strerror(i), "surrogateescape");
    else
        message = PyUnicode_FromString("Error");   /* sometimes errno didn't get set */
    if (message == NULL)
        return NULL;
    args[0] = JS_NewInt32(ctx, i);
    args[1] = qp_capi_value(ctx, message);
    n = 2;
    if (filenameObject != NULL) {
        args[2] = qp_capi_value(ctx, filenameObject);
        n = 3;
        if (filenameObject2 != NULL) {
            args[3] = JS_NewInt32(ctx, 0);
            args[4] = qp_capi_value(ctx, filenameObject2);
            n = 5;
        }
    }
    for (i = 0; i < n; i++) {
        if (JS_IsException(args[i])) {
            Py_DECREF(message);
            return NULL;
        }
    }
    qp_capi_err_call_raise(ctx, exc, n, (JSValueConst *)args);
    Py_DECREF(message);
    return NULL;
}

PyObject *PyErr_SetFromErrnoWithFilenameObject(PyObject *exc, PyObject *filenameObject)
{
    return PyErr_SetFromErrnoWithFilenameObjects(exc, filenameObject, NULL);
}

PyObject *PyErr_SetFromErrno(PyObject *exc)
{
    return PyErr_SetFromErrnoWithFilenameObjects(exc, NULL, NULL);
}

PyObject *PyErr_SetFromErrnoWithFilename(PyObject *exc, const char *filename)
{
    PyObject *name = NULL, *result;

    if (filename) {
        int i = errno;
        name = PyUnicode_DecodeFSDefault(filename);
        if (name == NULL)
            return NULL;
        errno = i;
    }
    result = PyErr_SetFromErrnoWithFilenameObjects(exc, name, NULL);
    Py_XDECREF(name);
    return result;
}

PyObject *PyErr_SetExcFromWindowsErrWithFilenameObjects(PyObject *exc, int ierr,
                                                        PyObject *filenameObject,
                                                        PyObject *filenameObject2)
{
    JSContext *ctx = qp_capi.ctx;
    unsigned int err = (unsigned int)ierr;
    PyObject *message;
    JSValue args[5];
    int i;

    message = PyUnicode_FromFormat("Windows Error 0x%x", err);
    if (message == NULL)
        return NULL;
    if (filenameObject == NULL)
        filenameObject = filenameObject2 = Py_None;
    else if (filenameObject2 == NULL)
        filenameObject2 = Py_None;
    /* the constructor signature of OSError: (errno, strerror, filename,
       winerror, filename2) */
    args[0] = JS_NewInt32(ctx, 0);
    args[1] = qp_capi_value(ctx, message);
    args[2] = qp_capi_value(ctx, filenameObject);
    args[3] = qp_long_from_uint64(ctx, err);
    args[4] = qp_capi_value(ctx, filenameObject2);
    for (i = 0; i < 5; i++) {
        if (JS_IsException(args[i]))
            goto done;
    }
    qp_capi_err_call_raise(ctx, exc, 5, (JSValueConst *)args);
 done:
    JS_FreeValue(ctx, args[3]);
    Py_DECREF(message);
    return NULL;
}

PyObject *PyErr_SetExcFromWindowsErrWithFilenameObject(PyObject *exc, int ierr,
                                                       PyObject *filenameObject)
{
    return PyErr_SetExcFromWindowsErrWithFilenameObjects(exc, ierr, filenameObject, NULL);
}

PyObject *PyErr_SetExcFromWindowsErr(PyObject *exc, int ierr)
{
    return PyErr_SetExcFromWindowsErrWithFilenameObjects(exc, ierr, NULL, NULL);
}

PyObject *PyErr_SetExcFromWindowsErrWithFilename(PyObject *exc, int ierr, const char *filename)
{
    PyObject *name = NULL, *ret;
    if (filename) {
        name = PyUnicode_DecodeFSDefault(filename);
        if (name == NULL)
            return NULL;
    }
    ret = PyErr_SetExcFromWindowsErrWithFilenameObjects(exc, ierr, name, NULL);
    Py_XDECREF(name);
    return ret;
}

PyObject *PyErr_SetFromWindowsErr(int ierr)
{
    return PyErr_SetExcFromWindowsErr(PyExc_OSError, ierr);
}

PyObject *PyErr_SetFromWindowsErrWithFilename(int ierr, const char *filename)
{
    return PyErr_SetExcFromWindowsErrWithFilename(PyExc_OSError, ierr, filename);
}

/* ---- ImportError ---------------------------------------------------------- */

PyObject *PyErr_SetImportErrorSubclass(PyObject *exception, PyObject *msg, PyObject *name,
                                       PyObject *path)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue kw[2], kwnames, args[3], e, r;
    int issubclass, i;

    issubclass = PyObject_IsSubclass(exception, PyExc_ImportError);
    if (issubclass < 0)
        return NULL;
    if (!issubclass) {
        PyErr_SetString(PyExc_TypeError, "expected a subclass of ImportError");
        return NULL;
    }
    if (msg == NULL) {
        PyErr_SetString(PyExc_TypeError, "expected a message argument");
        return NULL;
    }
    if (name == NULL)
        name = Py_None;
    if (path == NULL)
        path = Py_None;
    e = qp_capi_value(ctx, exception);
    args[0] = qp_capi_value(ctx, msg);
    args[1] = qp_capi_value(ctx, name);
    args[2] = qp_capi_value(ctx, path);
    if (JS_IsException(e))
        return NULL;
    for (i = 0; i < 3; i++) {
        if (JS_IsException(args[i]))
            return NULL;
    }
    kw[0] = qp_str_from_atom(ctx, QP_ATOM(ctx, name));
    kw[1] = qp_str_from_atom(ctx, QP_ATOM(ctx, path));
    kwnames = qp_tuple_new_steal(ctx, 2, kw);
    if (JS_IsException(kwnames))
        return NULL;
    r = qp_vectorcall(ctx, e, (JSValueConst *)args, 1, kwnames);
    JS_FreeValue(ctx, kwnames);
    if (!JS_IsException(r))
        qp_capi_err_raise_instance(ctx, r);
    return NULL;
}

PyObject *PyErr_SetImportError(PyObject *msg, PyObject *name, PyObject *path)
{
    return PyErr_SetImportErrorSubclass(PyExc_ImportError, msg, name, path);
}

/* ---- exception classes ---------------------------------------------------- */

PyObject *PyErr_NewException(const char *name, PyObject *base, PyObject *dict)
{
    PyObject *modulename = NULL, *mydict = NULL, *bases = NULL, *result = NULL;
    const char *dot = strrchr(name, '.');
    int r;

    if (dot == NULL) {
        PyErr_SetString(PyExc_SystemError, "PyErr_NewException: name must be module.class");
        return NULL;
    }
    if (base == NULL)
        base = PyExc_Exception;
    if (dict == NULL) {
        dict = mydict = PyDict_New();
        if (dict == NULL)
            goto failure;
    }
    r = PyDict_Contains(dict, _PyQp_InternId("__module__"));
    if (r < 0)
        goto failure;
    if (r == 0) {
        modulename = PyUnicode_FromStringAndSize(name, (Py_ssize_t)(dot - name));
        if (modulename == NULL)
            goto failure;
        if (PyDict_SetItem(dict, _PyQp_InternId("__module__"), modulename) != 0)
            goto failure;
    }
    if (PyTuple_Check(base)) {
        bases = Py_NewRef(base);
    } else {
        bases = PyTuple_Pack(1, base);
        if (bases == NULL)
            goto failure;
    }
    /* create a real class: type(name, bases, dict) */
    result = PyObject_CallFunction((PyObject *)&PyType_Type, "sOO", dot + 1, bases, dict);
 failure:
    Py_XDECREF(bases);
    Py_XDECREF(mydict);
    Py_XDECREF(modulename);
    return result;
}

PyObject *PyErr_NewExceptionWithDoc(const char *name, const char *doc, PyObject *base,
                                    PyObject *dict)
{
    PyObject *ret = NULL, *mydict = NULL, *docobj;
    int result;

    if (dict == NULL) {
        dict = mydict = PyDict_New();
        if (dict == NULL)
            return NULL;
    }
    if (doc != NULL) {
        docobj = PyUnicode_FromString(doc);
        if (docobj == NULL)
            goto failure;
        result = PyDict_SetItemString(dict, "__doc__", docobj);
        Py_DECREF(docobj);
        if (result < 0)
            goto failure;
    }
    ret = PyErr_NewException(name, base, dict);
 failure:
    Py_XDECREF(mydict);
    return ret;
}

/* ---- unraisable exceptions ----------------------------------------------- */

void PyErr_WriteUnraisable(PyObject *obj)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue exc, o = QP_NULL;

    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return;
    if (obj) {
        o = qp_capi_value(ctx, obj);
        if (JS_IsException(o)) {
            qp_err_clear(ctx);
            o = QP_NULL;
        }
    }
    qp_exc_write_unraisable_msg(ctx, exc, NULL, o);
    JS_FreeValue(ctx, exc);
}

void PyErr_FormatUnraisable(const char *format, ...)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *err_msg = NULL;
    const char *msg = NULL;
    va_list va;
    JSValue exc;

    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return;
    if (format != NULL) {
        va_start(va, format);
        err_msg = PyUnicode_FromFormatV(format, va);
        va_end(va);
        if (err_msg == NULL)
            qp_err_clear(ctx);
        else if ((msg = PyUnicode_AsUTF8(err_msg)) == NULL)
            qp_err_clear(ctx);
    }
    qp_exc_write_unraisable_msg(ctx, exc, msg, QP_NULL);
    Py_XDECREF(err_msg);
    JS_FreeValue(ctx, exc);
}

/* ---- chaining and notes -------------------------------------------------- */

void _PyErr_ChainExceptions1(PyObject *exc)
{
    if (exc == NULL)
        return;
    if (PyErr_Occurred()) {
        PyObject *exc2 = PyErr_GetRaisedException();
        PyException_SetContext(exc2, exc);
        PyErr_SetRaisedException(exc2);
    } else {
        PyErr_SetRaisedException(exc);
    }
}

PyObject *_PyErr_FormatFromCause(PyObject *exception, const char *format, ...)
{
    PyObject *exc, *exc2;
    va_list vargs;

    exc = PyErr_GetRaisedException();
    va_start(vargs, format);
    PyErr_FormatV(exception, format, vargs);
    va_end(vargs);
    exc2 = PyErr_GetRaisedException();
    if (exc2 && exc) {
        PyException_SetCause(exc2, Py_NewRef(exc));
        PyException_SetContext(exc2, Py_NewRef(exc));
    }
    Py_XDECREF(exc);
    PyErr_SetRaisedException(exc2);
    return NULL;
}

int _PyException_AddNote(PyObject *exc, PyObject *note)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue meth, args[2], r;

    if (!PyExceptionInstance_Check(exc)) {
        PyErr_Format(PyExc_TypeError, "exc must be an exception, not '%s'",
                     Py_TYPE(exc)->tp_name);
        return -1;
    }
    /* BaseException.add_note itself (not an override) */
    meth = qp_type_lookup(ctx, QP_EXC(ctx, BaseException), QP_ATOM(ctx, add_note));
    if (QP_IS_NULL(meth)) {
        PyErr_SetString(PyExc_SystemError, "BaseException.add_note is missing");
        return -1;
    }
    args[0] = qp_capi_value(ctx, exc);
    if (JS_IsException(args[0]))
        return -1;
    args[1] = qp_capi_value(ctx, note);
    if (JS_IsException(args[1]))
        return -1;
    meth = JS_DupValue(ctx, meth);
    r = qp_vectorcall(ctx, meth, (JSValueConst *)args, 2, QP_NULL);
    JS_FreeValue(ctx, meth);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

void _PyErr_FormatNote(const char *format, ...)
{
    PyObject *exc = PyErr_GetRaisedException(), *note;
    va_list vargs;
    int res;

    if (exc == NULL)
        return;
    va_start(vargs, format);
    note = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (note == NULL)
        goto error;
    res = _PyException_AddNote(exc, note);
    Py_DECREF(note);
    if (res < 0)
        goto error;
    PyErr_SetRaisedException(exc);
    return;
 error:
    _PyErr_ChainExceptions1(exc);
}

PyObject *PyUnstable_Exc_PrepReraiseStar(PyObject *orig, PyObject *excs)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue o, e;

    o = qp_capi_value(ctx, orig);
    if (JS_IsException(o))
        return NULL;
    e = qp_capi_value(ctx, excs);
    if (JS_IsException(e))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_exc_prep_reraise_star(ctx, o, e));
}

/* ========================================================================= */
/* PyException_*                                                              */
/* ========================================================================= */

/* JS value of an exception instance (NULL with no error when op is not an
   exception: CPython asserts). */
static JSValue qp_capi_exc_value(JSContext *ctx, PyObject *op)
{
    JSValue v;
    if (!op || !PyExceptionInstance_Check(op))
        return QP_NULL;
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
        return QP_NULL;
    }
    return qp_exc_check(ctx, v) ? v : QP_NULL;
}

PyObject *PyException_GetTraceback(PyObject *self)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e = qp_capi_exc_value(ctx, self), tb;

    if (QP_IS_NULL(e))
        return NULL;
    tb = qp_exc_get_traceback(ctx, e);
    if (JS_IsUndefined(tb))
        return NULL;
    return qp_capi_steal(ctx, tb);
}

int PyException_SetTraceback(PyObject *self, PyObject *tb)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e, t = QP_NULL;

    e = qp_capi_value(ctx, self);
    if (JS_IsException(e))
        return -1;
    if (tb) {
        t = qp_capi_value(ctx, tb);
        if (JS_IsException(t))
            return -1;
    }
    return qp_exc_set_traceback(ctx, e, t);
}

static PyObject *qp_capi_exc_link(JSValueConst v)
{
    if (QP_IS_NULL(v) || JS_IsUndefined(v))
        return NULL;
    return qp_capi_new_ref(qp_capi.ctx, v);
}

/* new JS reference of an optional object (stolen), QP_NULL for NULL */
static JSValue qp_capi_exc_take(JSContext *ctx, PyObject *op)
{
    JSValue v;
    if (!op)
        return QP_NULL;
    v = qp_capi_to_js(ctx, op);
    Py_DECREF(op);
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
        return QP_NULL;
    }
    return v;
}

PyObject *PyException_GetCause(PyObject *self)
{
    JSValue e = qp_capi_exc_value(qp_capi.ctx, self);
    return QP_IS_NULL(e) ? NULL : qp_capi_exc_link(qp_exc_cause(e));
}

void PyException_SetCause(PyObject *self, PyObject *cause)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e = qp_capi_exc_value(ctx, self);
    JSValue c = qp_capi_exc_take(ctx, cause);
    if (QP_IS_NULL(e)) {
        JS_FreeValue(ctx, c);
        return;
    }
    qp_exc_set_cause(ctx, e, c);
}

PyObject *PyException_GetContext(PyObject *self)
{
    JSValue e = qp_capi_exc_value(qp_capi.ctx, self);
    return QP_IS_NULL(e) ? NULL : qp_capi_exc_link(qp_exc_context(e));
}

void PyException_SetContext(PyObject *self, PyObject *context)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e = qp_capi_exc_value(ctx, self);
    JSValue c = qp_capi_exc_take(ctx, context);
    if (QP_IS_NULL(e)) {
        JS_FreeValue(ctx, c);
        return;
    }
    qp_exc_set_context(ctx, e, c);
}

PyObject *PyException_GetArgs(PyObject *self)
{
    JSValue e = qp_capi_exc_value(qp_capi.ctx, self);
    if (QP_IS_NULL(e))
        return NULL;
    return qp_capi_new_ref(qp_capi.ctx, qp_exc_args(e));
}

void PyException_SetArgs(PyObject *self, PyObject *args)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue e = qp_capi_exc_value(ctx, self), a;

    if (QP_IS_NULL(e) || !args)
        return;
    a = qp_capi_value(ctx, args);
    if (JS_IsException(a) || qp_setattr(ctx, e, QP_ATOM(ctx, args), a) < 0)
        qp_err_clear(ctx);
}

/* ========================================================================= */
/* PyUnicode{Encode,Decode,Translate}Error_*                                  */
/* ========================================================================= */

#define QP_CAPI_UE_ENCODE "UnicodeEncodeError"
#define QP_CAPI_UE_DECODE "UnicodeDecodeError"
#define QP_CAPI_UE_TRANSLATE "UnicodeTranslateError"

/* check_unicode_error_type: any UnicodeError subclass is accepted. */
static JSValue qp_capi_ue_self(JSContext *ctx, PyObject *self, const char *expect_type)
{
    JSValue v;
    if (!PyObject_TypeCheck(self, (PyTypeObject *)PyExc_UnicodeError)) {
        PyErr_Format(PyExc_TypeError, "expecting a %s object, got %T", expect_type, self);
        return JS_EXCEPTION;
    }
    v = qp_capi_value(ctx, self);
    return v;
}

static JSValue qp_capi_ue_getattr(JSContext *ctx, JSValueConst self, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr(ctx, self, a);
    JS_FreeAtom(ctx, a);
    return r;
}

static int qp_capi_ue_setattr(JSContext *ctx, JSValueConst self, const char *name, JSValue v)
{
    JSAtom a;
    int r;
    if (JS_IsException(v))
        return -1;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_setattr(ctx, self, a, v);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return r;
}

/* as_unicode_error_attribute: new reference of a str (as_bytes = 0) or
   bytes (1) attribute; an unset attribute reads as None. */
static JSValue qp_capi_ue_attr(JSContext *ctx, JSValueConst self, const char *name, int as_bytes)
{
    JSValue v = qp_capi_ue_getattr(ctx, self, name);
    if (JS_IsException(v))
        return v;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        PyErr_Format(PyExc_TypeError, "UnicodeError '%s' attribute is not set", name);
        return JS_EXCEPTION;
    }
    if (!(as_bytes ? qp_bytes_check(ctx, v) : qp_str_check(ctx, v))) {
        JS_FreeValue(ctx, v);
        PyErr_Format(PyExc_TypeError, "UnicodeError '%s' attribute must be a %s", name,
                     as_bytes ? "bytes" : "string");
        return JS_EXCEPTION;
    }
    return v;
}

static PyObject *qp_capi_ue_get_str(PyObject *self, const char *expect, const char *name,
                                    int as_bytes)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s = qp_capi_ue_self(ctx, self, expect), v;
    if (JS_IsException(s))
        return NULL;
    v = qp_capi_ue_attr(ctx, s, name, as_bytes);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_steal(ctx, v);
}

/* raw `start` / `end` integer attribute */
static int qp_capi_ue_pos(JSContext *ctx, JSValueConst self, const char *name, Py_ssize_t *out)
{
    JSValue v = qp_capi_ue_getattr(ctx, self, name);
    int r;
    if (JS_IsException(v))
        return -1;
    r = qp_as_ssize(ctx, v, NULL, out);
    JS_FreeValue(ctx, v);
    return r;
}

/* _PyUnicodeError_GetParams for start (which = 0) or end (1). */
static int qp_capi_ue_get_pos(PyObject *self, const char *expect, Py_ssize_t *out,
                              int as_bytes, int which)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s = qp_capi_ue_self(ctx, self, expect), obj;
    Py_ssize_t n, pos;

    if (JS_IsException(s))
        return -1;
    obj = qp_capi_ue_attr(ctx, s, "object", as_bytes);
    if (JS_IsException(obj))
        return -1;
    if (as_bytes)
        qp_bytes_data(obj, &n);
    else
        n = qp_str_len(ctx, obj);
    JS_FreeValue(ctx, obj);
    if (n < 0)
        return -1;
    if (qp_capi_ue_pos(ctx, s, which ? "end" : "start", &pos) < 0)
        return -1;
    if (which == 0) {
        if (pos < 0)
            pos = 0;
        if (pos >= n)
            pos = n == 0 ? 0 : n - 1;
    } else {
        if (pos < 1)
            pos = 1;
        if (pos > n)
            pos = n;
    }
    *out = pos;
    return 0;
}

static int qp_capi_ue_set_pos(PyObject *self, const char *expect, Py_ssize_t pos, int which)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s = qp_capi_ue_self(ctx, self, expect);
    if (JS_IsException(s))
        return -1;
    return qp_capi_ue_setattr(ctx, s, which ? "end" : "start", qp_long_from_int64(ctx, pos));
}

static int qp_capi_ue_set_reason(PyObject *self, const char *expect, const char *reason)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s = qp_capi_ue_self(ctx, self, expect);
    if (JS_IsException(s))
        return -1;
    return qp_capi_ue_setattr(ctx, s, "reason", qp_str_from_utf8(ctx, reason, strlen(reason)));
}

PyObject *PyUnicodeEncodeError_GetEncoding(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_ENCODE, "encoding", 0);
}

PyObject *PyUnicodeDecodeError_GetEncoding(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_DECODE, "encoding", 0);
}

PyObject *PyUnicodeEncodeError_GetObject(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_ENCODE, "object", 0);
}

PyObject *PyUnicodeDecodeError_GetObject(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_DECODE, "object", 1);
}

PyObject *PyUnicodeTranslateError_GetObject(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_TRANSLATE, "object", 0);
}

int PyUnicodeEncodeError_GetStart(PyObject *self, Py_ssize_t *start)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_ENCODE, start, 0, 0);
}

int PyUnicodeDecodeError_GetStart(PyObject *self, Py_ssize_t *start)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_DECODE, start, 1, 0);
}

int PyUnicodeTranslateError_GetStart(PyObject *self, Py_ssize_t *start)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_TRANSLATE, start, 0, 0);
}

int PyUnicodeEncodeError_SetStart(PyObject *self, Py_ssize_t start)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_ENCODE, start, 0);
}

int PyUnicodeDecodeError_SetStart(PyObject *self, Py_ssize_t start)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_DECODE, start, 0);
}

int PyUnicodeTranslateError_SetStart(PyObject *self, Py_ssize_t start)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_TRANSLATE, start, 0);
}

int PyUnicodeEncodeError_GetEnd(PyObject *self, Py_ssize_t *end)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_ENCODE, end, 0, 1);
}

int PyUnicodeDecodeError_GetEnd(PyObject *self, Py_ssize_t *end)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_DECODE, end, 1, 1);
}

int PyUnicodeTranslateError_GetEnd(PyObject *self, Py_ssize_t *end)
{
    return qp_capi_ue_get_pos(self, QP_CAPI_UE_TRANSLATE, end, 0, 1);
}

int PyUnicodeEncodeError_SetEnd(PyObject *self, Py_ssize_t end)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_ENCODE, end, 1);
}

int PyUnicodeDecodeError_SetEnd(PyObject *self, Py_ssize_t end)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_DECODE, end, 1);
}

int PyUnicodeTranslateError_SetEnd(PyObject *self, Py_ssize_t end)
{
    return qp_capi_ue_set_pos(self, QP_CAPI_UE_TRANSLATE, end, 1);
}

PyObject *PyUnicodeEncodeError_GetReason(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_ENCODE, "reason", 0);
}

PyObject *PyUnicodeDecodeError_GetReason(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_DECODE, "reason", 0);
}

PyObject *PyUnicodeTranslateError_GetReason(PyObject *self)
{
    return qp_capi_ue_get_str(self, QP_CAPI_UE_TRANSLATE, "reason", 0);
}

int PyUnicodeEncodeError_SetReason(PyObject *self, const char *reason)
{
    return qp_capi_ue_set_reason(self, QP_CAPI_UE_ENCODE, reason);
}

int PyUnicodeDecodeError_SetReason(PyObject *self, const char *reason)
{
    return qp_capi_ue_set_reason(self, QP_CAPI_UE_DECODE, reason);
}

int PyUnicodeTranslateError_SetReason(PyObject *self, const char *reason)
{
    return qp_capi_ue_set_reason(self, QP_CAPI_UE_TRANSLATE, reason);
}

PyObject *PyUnicodeDecodeError_Create(const char *encoding, const char *object,
                                      Py_ssize_t length, Py_ssize_t start, Py_ssize_t end,
                                      const char *reason)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue args[5], r;
    int i;

    args[0] = qp_str_from_utf8(ctx, encoding, strlen(encoding));
    args[1] = qp_bytes_new(ctx, object, length);
    args[2] = qp_long_from_int64(ctx, start);
    args[3] = qp_long_from_int64(ctx, end);
    args[4] = qp_str_from_utf8(ctx, reason, strlen(reason));
    for (i = 0; i < 5; i++) {
        if (JS_IsException(args[i])) {
            r = JS_EXCEPTION;
            goto done;
        }
    }
    r = qp_vectorcall(ctx, QP_TYPE_VALUE(QP_EXC(ctx, UnicodeDecodeError)), (JSValueConst *)args, 5, QP_NULL);
 done:
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, args[i]);
    return qp_capi_from_js_result(ctx, r);
}

/* ========================================================================= */
/* Syntax error locations and program text                                    */
/* ========================================================================= */

PyObject *PyErr_ProgramTextObject(PyObject *filename, int lineno)
{
    const char *fname;
    JSValue res;

    if (filename == NULL || lineno <= 0)
        return NULL;
    fname = PyUnicode_AsUTF8(filename);
    if (fname == NULL) {
        PyErr_Clear();
        return NULL;
    }
    res = qp_err_program_text(qp_capi.ctx, fname, lineno);
    if (QP_IS_NULL(res))
        return NULL;
    return qp_capi_steal(qp_capi.ctx, res);
}

PyObject *PyErr_ProgramText(const char *filename, int lineno)
{
    PyObject *filename_obj, *res;

    if (filename == NULL)
        return NULL;
    filename_obj = PyUnicode_DecodeFSDefault(filename);
    if (filename_obj == NULL) {
        PyErr_Clear();
        return NULL;
    }
    res = PyErr_ProgramTextObject(filename_obj, lineno);
    Py_DECREF(filename_obj);
    return res;
}

/* setattr ignoring errors (CPython clears them) */
static void qp_capi_set_attr_quiet(JSContext *ctx, JSValueConst obj, JSAtom name, JSValue v)
{
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
        return;
    }
    if (qp_setattr(ctx, obj, name, v) < 0)
        qp_err_clear(ctx);
    JS_FreeValue(ctx, v);
}

static JSValue qp_capi_int_or_none(JSContext *ctx, int v)
{
    return v >= 0 ? JS_NewInt32(ctx, v) : JS_UNDEFINED;
}

/* PyErr_SyntaxLocationObjectEx: add location attributes to the pending
   exception. */
static void qp_capi_syntax_location(PyObject *filename, int lineno, int col_offset,
                                    int end_lineno, int end_col_offset)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue exc, f, tmp;
    JSAtom pfal;
    int rc;

    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return;
    /* add attributes for the line number and filename for the error */
    qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, lineno), JS_NewInt32(ctx, lineno));
    qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, offset), qp_capi_int_or_none(ctx, col_offset));
    qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, end_lineno),
                           qp_capi_int_or_none(ctx, end_lineno));
    qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, end_offset),
                           qp_capi_int_or_none(ctx, end_col_offset));
    if (filename != NULL) {
        PyObject *text;
        f = qp_capi_to_js(ctx, filename);
        qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, filename), f);
        text = PyErr_ProgramTextObject(filename, lineno);
        if (text) {
            qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, text), qp_capi_to_js(ctx, text));
            Py_DECREF(text);
        } else {
            qp_err_clear(ctx);
        }
    }
    if (qp_type_of(ctx, exc) != QP_EXC(ctx, SyntaxError)) {
        rc = qp_getattr_opt(ctx, exc, QP_ATOM(ctx, msg), &tmp);
        if (rc < 0) {
            qp_err_clear(ctx);
        } else if (rc == 0) {
            qp_capi_set_attr_quiet(ctx, exc, QP_ATOM(ctx, msg), qp_str(ctx, exc));
        } else {
            JS_FreeValue(ctx, tmp);
        }
        pfal = JS_NewAtom(ctx, "print_file_and_line");
        if (pfal == JS_ATOM_NULL) {
            qp_err_clear(ctx);
        } else {
            rc = qp_getattr_opt(ctx, exc, pfal, &tmp);
            if (rc < 0)
                qp_err_clear(ctx);
            else if (rc == 0)
                qp_capi_set_attr_quiet(ctx, exc, pfal, JS_UNDEFINED);
            else
                JS_FreeValue(ctx, tmp);
            JS_FreeAtom(ctx, pfal);
        }
    }
    JS_Throw(ctx, exc);
}

void PyErr_SyntaxLocationObject(PyObject *filename, int lineno, int col_offset)
{
    qp_capi_syntax_location(filename, lineno, col_offset, lineno, -1);
}

void PyErr_RangedSyntaxLocationObject(PyObject *filename, int lineno, int col_offset,
                                      int end_lineno, int end_col_offset)
{
    qp_capi_syntax_location(filename, lineno, col_offset, end_lineno, end_col_offset);
}

void PyErr_SyntaxLocationEx(const char *filename, int lineno, int col_offset)
{
    PyObject *fileobj = NULL;
    if (filename != NULL) {
        PyObject *exc = PyErr_GetRaisedException();
        fileobj = PyUnicode_DecodeFSDefault(filename);
        if (fileobj == NULL)
            PyErr_Clear();
        PyErr_SetRaisedException(exc);
    }
    PyErr_SyntaxLocationObject(fileobj, lineno, col_offset);
    Py_XDECREF(fileobj);
}

void PyErr_SyntaxLocation(const char *filename, int lineno)
{
    PyErr_SyntaxLocationEx(filename, lineno, -1);
}

/* ========================================================================= */
/* Warnings                                                                   */
/* ========================================================================= */

/* get_category check: a Warning subclass (NULL = RuntimeWarning). */
static int qp_capi_warn_category(JSContext *ctx, PyObject *category, QPType **pt)
{
    JSValue v;
    QPType *t;

    *pt = NULL;
    if (category == NULL)
        return 0;
    v = qp_capi_value(ctx, category);
    if (JS_IsException(v))
        return -1;
    t = qp_type_from_value(v);
    if (!t || !qp_type_is_subtype(t, QP_EXC(ctx, Warning))) {
        PyErr_Format(PyExc_TypeError, "category must be a Warning subclass, not '%s'",
                     Py_TYPE(category)->tp_name);
        return -1;
    }
    *pt = t;
    return 0;
}

/* warn_unicode with a message str (consumed) and an optional source. */
static int qp_capi_warn(PyObject *category, JSValue message, Py_ssize_t stack_level,
                        PyObject *source)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t;
    JSValue src = QP_NULL;
    int r = -1;

    if (JS_IsException(message))
        return -1;
    if (qp_capi_warn_category(ctx, category, &t) < 0)
        goto done;
    if (source) {
        src = qp_capi_value(ctx, source);
        if (JS_IsException(src))
            goto done;
    }
    r = qp_warnings_warn(ctx, t, message, stack_level, src);
 done:
    JS_FreeValue(ctx, message);
    return r;
}

int PyErr_WarnEx(PyObject *category, const char *text, Py_ssize_t stack_level)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_warn(category, qp_str_from_utf8(ctx, text, strlen(text)), stack_level, NULL);
}

static JSValue qp_capi_format_js(const char *format, va_list vargs)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *s = PyUnicode_FromFormatV(format, vargs);
    JSValue v;
    if (!s)
        return JS_EXCEPTION;
    v = qp_capi_to_js(ctx, s);
    Py_DECREF(s);
    return v;
}

int PyErr_WarnFormat(PyObject *category, Py_ssize_t stack_level, const char *format, ...)
{
    va_list vargs;
    JSValue message;

    va_start(vargs, format);
    message = qp_capi_format_js(format, vargs);
    va_end(vargs);
    return qp_capi_warn(category, message, stack_level, NULL);
}

int PyErr_ResourceWarning(PyObject *source, Py_ssize_t stack_level, const char *format, ...)
{
    va_list vargs;
    JSValue message;

    va_start(vargs, format);
    message = qp_capi_format_js(format, vargs);
    va_end(vargs);
    return qp_capi_warn(PyExc_ResourceWarning, message, stack_level, source);
}

int PyErr_WarnExplicitObject(PyObject *category, PyObject *message, PyObject *filename,
                             int lineno, PyObject *module, PyObject *registry)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue m, f, mod, reg;
    QPType *t;

    if (qp_capi_warn_category(ctx, category, &t) < 0)
        return -1;
    m = qp_capi_value(ctx, message);
    if (JS_IsException(m))
        return -1;
    f = qp_capi_value(ctx, filename);
    if (JS_IsException(f))
        return -1;
    mod = qp_capi_e1_opt(ctx, module);
    if (JS_IsException(mod))
        return -1;
    reg = qp_capi_e1_opt(ctx, registry);
    if (JS_IsException(reg))
        return -1;
    return qp_warnings_warn_explicit(ctx, t, m, f, lineno, mod, reg, QP_NULL);
}

static int qp_capi_warn_explicit_c(PyObject *category, PyObject *message,
                                   const char *filename_str, int lineno,
                                   const char *module_str, PyObject *registry)
{
    PyObject *filename, *module = NULL;
    int ret = -1;

    if (message == NULL)
        return -1;
    filename = PyUnicode_DecodeFSDefault(filename_str);
    if (filename == NULL)
        goto exit;
    if (module_str != NULL) {
        module = PyUnicode_FromString(module_str);
        if (module == NULL)
            goto exit;
    }
    ret = PyErr_WarnExplicitObject(category, message, filename, lineno, module, registry);
 exit:
    Py_XDECREF(message);
    Py_XDECREF(module);
    Py_XDECREF(filename);
    return ret;
}

int PyErr_WarnExplicit(PyObject *category, const char *text, const char *filename_str,
                       int lineno, const char *module_str, PyObject *registry)
{
    return qp_capi_warn_explicit_c(category, PyUnicode_FromString(text), filename_str, lineno,
                                   module_str, registry);
}

int PyErr_WarnExplicitFormat(PyObject *category, const char *filename_str, int lineno,
                             const char *module_str, PyObject *registry,
                             const char *format, ...)
{
    PyObject *message;
    va_list vargs;

    va_start(vargs, format);
    message = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    return qp_capi_warn_explicit_c(category, message, filename_str, lineno, module_str,
                                   registry);
}

/* ========================================================================= */
/* Tracebacks                                                                 */
/* ========================================================================= */

/* Prepend an entry to the traceback of the pending exception. */
static int qp_capi_tb_push(JSContext *ctx, JSValueConst func, JSValueConst frame, int lineno)
{
    JSValue exc, tb;

    exc = qp_exc_fetch(ctx);
    if (QP_IS_NULL(exc))
        return 0;
    if (!qp_exc_check(ctx, exc)) {
        JS_Throw(ctx, exc);
        return 0;
    }
    tb = qp_tb_alloc(ctx, qp_exc_traceback(exc), func, frame, -1, lineno, QP_NULL, QP_NULL);
    if (JS_IsException(tb)) {
        /* chain the original exception to the new error */
        JSValue err = qp_exc_fetch(ctx);
        if (qp_exc_check(ctx, err))
            qp_exc_set_context(ctx, err, JS_DupValue(ctx, exc));
        JS_FreeValue(ctx, exc);
        JS_Throw(ctx, err);
        return -1;
    }
    qp_exc_set_traceback_raw(ctx, exc, tb);
    JS_Throw(ctx, exc);
    return 0;
}

int PyTraceBack_Here(PyFrameObject *frame)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue f, ln;
    JSAtom a;
    int lineno = -1;
    QPCapiErrSave saved;

    f = qp_capi_value(ctx, (PyObject *)frame);
    if (JS_IsException(f))
        return -1;
    /* f_lineno of the frame, read with the exception put aside */
    qp_capi_err_save(ctx, &saved);
    a = JS_NewAtom(ctx, "f_lineno");
    if (a != JS_ATOM_NULL) {
        int64_t x;
        ln = qp_getattr(ctx, f, a);
        JS_FreeAtom(ctx, a);
        if (!JS_IsException(ln) && qp_long_check_exact(ln) &&
            qp_long_as_int64(ctx, ln, &x) == 0 && x >= 0 && x <= INT_MAX)
            lineno = (int)x;
        JS_FreeValue(ctx, ln);
    }
    qp_capi_err_restore(ctx, &saved);
    return qp_capi_tb_push(ctx, QP_NULL, f, lineno);
}

/* _PyTraceback_Add: a traceback entry for a C function (code with the
   given co_name / co_filename, line `lineno`). */
void _PyTraceback_Add(const char *funcname, const char *filename, int lineno)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiErrSave saved;
    char src[300];
    const char *name = funcname;
    JSValue bc = JS_UNDEFINED, mod = JS_UNDEFINED, globals = JS_UNDEFINED, locals = JS_UNDEFINED;
    JSValue r, fn = QP_NULL;
    JSAtom atom;
    size_t i;

    /* the name must be an identifier to compile `def name(): pass` */
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_') || (i == 0 && isdigit(c)) || i >= 200)
            break;
    }
    if (i == 0 || name[i])
        name = "_c_function";
    snprintf(src, sizeof(src), "def %s(): pass\n", name);

    /* Python code must not run with an exception set */
    qp_capi_err_save(ctx, &saved);
    bc = qp_compile_source(ctx, src, strlen(src), filename, QP_NULL, QP_INPUT_FILE, 0, 0,
                           NULL);
    if (JS_IsException(bc))
        goto error;
    globals = qp_dict_new(ctx);
    if (JS_IsException(globals))
        goto error;
    locals = qp_dict_new(ctx);
    if (JS_IsException(locals))
        goto error;
    mod = qp_function_new(ctx, bc, globals);
    if (JS_IsException(mod))
        goto error;
    r = qp_call_with_locals(ctx, mod, locals);
    if (JS_IsException(r))
        goto error;
    JS_FreeValue(ctx, r);
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL)
        goto error;
    if (qp_dict_get_item_atom(ctx, locals, atom, &fn) <= 0) {
        JS_FreeAtom(ctx, atom);
        goto error;
    }
    JS_FreeAtom(ctx, atom);
    qp_capi_err_restore(ctx, &saved);
    qp_capi_tb_push(ctx, fn, QP_NULL, lineno);
    goto done;
 error:
    {
        /* chain the saved exception to the new error (_PyErr_ChainExceptions1) */
        JSValue err = JS_HasException(ctx) ? qp_exc_fetch(ctx) : QP_NULL;
        qp_capi_err_restore(ctx, &saved);
        if (!QP_IS_NULL(err)) {
            if (JS_HasException(ctx) && qp_exc_check(ctx, err)) {
                JSValue prev = qp_exc_fetch(ctx);
                qp_exc_set_context(ctx, err, prev);
            }
            JS_Throw(ctx, err);
        }
    }
 done:
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, locals);
    JS_FreeValue(ctx, globals);
    JS_FreeValue(ctx, bc);
}

/* attribute by C name, new reference or JS_EXCEPTION */
static JSValue qp_capi_getattr_cstr(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return r;
}

#define QP_CAPI_TB_RECURSIVE_CUTOFF 3
#define QP_CAPI_TB_LIMIT_DEFAULT 1000

static int qp_capi_tb_line_repeated(JSContext *ctx, JSValueConst f, long cnt)
{
    char buf[80];
    cnt -= QP_CAPI_TB_RECURSIVE_CUTOFF;
    snprintf(buf, sizeof(buf), "  [Previous line repeated %ld more time%s]\n", cnt,
             cnt > 1 ? "s" : "");
    return qp_file_write_cstr(ctx, f, buf);
}

/* tb_displayline: '  File "%U", line %d, in %S' and the source line. */
static int qp_capi_tb_displayline(JSContext *ctx, JSValueConst f, JSValueConst filename,
                                  int lineno, JSValueConst name)
{
    JSValue line, src;
    int r;

    line = qp_str_from_format(ctx, "  File \"%U\", line %d, in %S\n", filename, lineno, name);
    if (JS_IsException(line))
        return -1;
    r = qp_file_write_str(ctx, f, line);
    JS_FreeValue(ctx, line);
    if (r < 0)
        return -1;
    src = qp_tb_source_line_stripped(ctx, filename, lineno);
    if (QP_IS_NULL(src))
        return 0;
    line = qp_str_from_format(ctx, "    %U\n", src);
    JS_FreeValue(ctx, src);
    if (JS_IsException(line))
        return -1;
    r = qp_file_write_str(ctx, f, line);
    JS_FreeValue(ctx, line);
    return r;
}

/* tb_printinternal */
static int qp_capi_tb_print(JSContext *ctx, JSValueConst tb0, JSValueConst f, long limit)
{
    JSValue tb = JS_DupValue(ctx, tb0), next, frame, code, filename, name, ln;
    JSValue last_file = JS_UNDEFINED, last_name = JS_UNDEFINED;
    long depth = 0, cnt = 0;
    int last_line = -1, err = 0;
    int64_t lineno;

    for (next = JS_DupValue(ctx, tb0); !JS_IsUndefined(next) && !JS_IsNull(next); ) {
        JSValue n2 = qp_capi_getattr_cstr(ctx, next, "tb_next");
        JS_FreeValue(ctx, next);
        if (JS_IsException(n2)) {
            JS_FreeValue(ctx, tb);
            return -1;
        }
        next = n2;
        depth++;
    }
    while (depth > limit && !JS_IsUndefined(tb)) {
        next = qp_capi_getattr_cstr(ctx, tb, "tb_next");
        JS_FreeValue(ctx, tb);
        if (JS_IsException(next))
            return -1;
        tb = next;
        depth--;
    }
    while (!JS_IsUndefined(tb) && !JS_IsNull(tb) && !err) {
        frame = qp_capi_getattr_cstr(ctx, tb, "tb_frame");
        code = JS_IsException(frame) ? JS_EXCEPTION : qp_capi_getattr_cstr(ctx, frame, "f_code");
        JS_FreeValue(ctx, frame);
        filename = JS_IsException(code) ? JS_EXCEPTION
                                        : qp_capi_getattr_cstr(ctx, code, "co_filename");
        name = JS_IsException(code) ? JS_EXCEPTION : qp_capi_getattr_cstr(ctx, code, "co_name");
        JS_FreeValue(ctx, code);
        ln = qp_capi_getattr_cstr(ctx, tb, "tb_lineno");
        lineno = -1;
        if (JS_IsException(filename) || JS_IsException(name) || JS_IsException(ln)) {
            err = -1;
        } else {
            if (qp_long_check_exact(ln) && qp_long_as_int64(ctx, ln, &lineno) < 0)
                qp_err_clear(ctx);
            if (lineno < INT_MIN || lineno > INT_MAX)
                lineno = -1;
            if (JS_IsUndefined(last_file) || !qp_is(filename, last_file) ||
                last_line == -1 || lineno != last_line ||
                JS_IsUndefined(last_name) || !qp_is(name, last_name)) {
                if (cnt > QP_CAPI_TB_RECURSIVE_CUTOFF &&
                    qp_capi_tb_line_repeated(ctx, f, cnt) < 0)
                    err = -1;
                JS_FreeValue(ctx, last_file);
                JS_FreeValue(ctx, last_name);
                last_file = JS_DupValue(ctx, filename);
                last_name = JS_DupValue(ctx, name);
                last_line = (int)lineno;
                cnt = 0;
            }
            cnt++;
            if (!err && cnt <= QP_CAPI_TB_RECURSIVE_CUTOFF &&
                qp_capi_tb_displayline(ctx, f, filename, (int)lineno, name) < 0)
                err = -1;
            if (!err && qp_signal_check(ctx) < 0)
                err = -1;
        }
        JS_FreeValue(ctx, filename);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, ln);
        if (err)
            break;
        next = qp_capi_getattr_cstr(ctx, tb, "tb_next");
        JS_FreeValue(ctx, tb);
        if (JS_IsException(next)) {
            tb = JS_UNDEFINED;
            err = -1;
            break;
        }
        tb = next;
    }
    if (!err && cnt > QP_CAPI_TB_RECURSIVE_CUTOFF && qp_capi_tb_line_repeated(ctx, f, cnt) < 0)
        err = -1;
    JS_FreeValue(ctx, tb);
    JS_FreeValue(ctx, last_file);
    JS_FreeValue(ctx, last_name);
    return err;
}

int PyTraceBack_Print(PyObject *v, PyObject *f)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue tb, file, lim;
    JSAtom a;
    long limit = QP_CAPI_TB_LIMIT_DEFAULT;

    if (v == NULL)
        return 0;
    if (!PyTraceBack_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }
    tb = qp_capi_value(ctx, v);
    if (JS_IsException(tb))
        return -1;
    file = qp_capi_value(ctx, f);
    if (JS_IsException(file))
        return -1;
    a = JS_NewAtom(ctx, "tracebacklimit");
    if (a == JS_ATOM_NULL)
        return -1;
    lim = qp_sys_get(ctx, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(lim))
        return -1;
    if (!QP_IS_NULL(lim) && qp_long_check(ctx, lim)) {
        JSValue iv = qp_long_value(ctx, lim);
        int64_t x;
        int ov = 0;
        if (JS_IsException(iv)) {
            JS_FreeValue(ctx, lim);
            return -1;
        }
        if (qp_long_as_int64_overflow(iv, &x, &ov) == 0) {
            if (ov > 0)
                limit = LONG_MAX;
            else if (ov < 0 || x <= 0)
                limit = 0;
            else
                limit = (long)x;
        }
        JS_FreeValue(ctx, iv);
    }
    JS_FreeValue(ctx, lim);
    if (limit <= 0)
        return 0;
    if (qp_file_write_cstr(ctx, file, "Traceback (most recent call last):\n") < 0)
        return -1;
    return qp_capi_tb_print(ctx, tb, file, limit);
}

/* Write a C string to a file descriptor (async-signal-safe). */
static void qp_capi_fd_puts(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

#define QP_CAPI_DUMP_MAX_FRAMES 100

/* faulthandler-style dump of the Python frames of the running context. */
static void qp_capi_dump_frames(int fd, JSContext *ctx)
{
    char buf[512], name[128], file[256];
    int depth;

    qp_capi_fd_puts(fd, "Stack (most recent call first):\n");
    for (depth = 0; ; depth++) {
        struct JSStackFrame *sf = qp_py_frame_at(ctx, depth);
        JSFunctionBytecode *b;
        QPLoc loc;
        int lineno = -1;

        if (!sf)
            break;
        if (depth >= QP_CAPI_DUMP_MAX_FRAMES) {
            qp_capi_fd_puts(fd, "  ...\n");
            break;
        }
        if (JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT ||
            JS_VALUE_GET_OBJ(sf->cur_func)->class_id != JS_CLASS_BYTECODE_FUNCTION)
            continue;
        b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
        if (sf->cur_pc && qp_code_addr2loc(b, (uint32_t)(sf->cur_pc - b->byte_code_buf - 1),
                                           &loc) == 0)
            lineno = loc.lineno;
        JS_AtomGetStrRT(ctx->rt, file, sizeof(file),
                        b->has_debug ? b->debug.filename : JS_ATOM_empty_string);
        JS_AtomGetStrRT(ctx->rt, name, sizeof(name), b->func_name);
        if (lineno >= 0)
            snprintf(buf, sizeof(buf), "  File \"%s\", line %d in %s\n", file, lineno, name);
        else
            snprintf(buf, sizeof(buf), "  File \"%s\", line ??? in %s\n", file, name);
        qp_capi_fd_puts(fd, buf);
    }
    qp_capi_fd_puts(fd, "\n");
}

const char *PyUnstable_DumpTraceback(int fd, PyThreadState *tstate)
{
    JSContext *ctx = qp_capi.ctx;
    if (!ctx || !tstate)
        return "unable to get the current thread state";
    qp_capi_dump_frames(fd, ctx);
    return NULL;
}

const char *PyUnstable_DumpTracebackThreads(int fd, PyInterpreterState *interp,
                                            PyThreadState *current_tstate,
                                            Py_ssize_t max_threads)
{
    JSContext *ctx = qp_capi.ctx;
    char buf[64];

    if (!ctx)
        return "unable to get the interpreter state";
    if (max_threads < 1)
        return NULL;
    /* one thread state in this runtime (capi-design.md §5) */
    snprintf(buf, sizeof(buf), "Current thread 0x%lx (most recent call first):\n",
             (unsigned long)PyThread_get_thread_ident());
    qp_capi_fd_puts(fd, buf);
    qp_capi_dump_frames(fd, ctx);
    return NULL;
}

/* ========================================================================= */
/* Printing exceptions (pythonrun.h)                                          */
/* ========================================================================= */

void PyErr_Display(PyObject *unused, PyObject *value, PyObject *tb)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v, file;

    if (value == NULL)
        return;
    if (PyExceptionInstance_Check(value) && tb != NULL && PyTraceBack_Check(tb)) {
        /* put the traceback on the exception, otherwise it won't get
           displayed (issue #18776) */
        PyObject *cur_tb = PyException_GetTraceback(value);
        if (cur_tb == NULL)
            PyException_SetTraceback(value, tb);
        else
            Py_DECREF(cur_tb);
    }
    file = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
    if (JS_IsException(file)) {
        qp_err_clear(ctx);
        file = QP_NULL;
    }
    if (JS_IsUndefined(file))
        return;     /* sys.stderr is None */
    v = qp_capi_value(ctx, value);
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
    } else if (qp_exc_check(ctx, v)) {
        if (qp_exc_display(ctx, v, file) < 0)
            qp_err_clear(ctx);
    } else {
        JSValue s = qp_str_from_format(ctx, "TypeError: print_exception(): Exception "
                                       "expected for value, %T found\n", v);
        if (JS_IsException(s) || QP_IS_NULL(file) || qp_file_write_str(ctx, file, s) < 0)
            qp_err_clear(ctx);
        JS_FreeValue(ctx, s);
    }
    JS_FreeValue(ctx, file);
}

void PyErr_DisplayException(PyObject *exc)
{
    PyErr_Display(NULL, exc, NULL);
}

/* _Py_HandleSystemExitAndKeyboardInterrupt: a pending SystemExit exits the
   process (unless -i). */
static void qp_capi_handle_system_exit(JSContext *ctx)
{
    JSValue exc;
    int code;

    if (QP_STATE(ctx)->config.inspect || !PyErr_ExceptionMatches(PyExc_SystemExit))
        return;
    exc = qp_exc_fetch(ctx);
    code = qp_err_system_exit_code(ctx, exc);
    JS_FreeValue(ctx, exc);
    Py_Exit(code);
}

static void qp_capi_set_sys_attr(JSContext *ctx, const char *name, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL || qp_setattr(ctx, QP_STATE(ctx)->sys_module, a, v) < 0)
        qp_err_clear(ctx);
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
}

void PyErr_PrintEx(int set_sys_last_vars)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *exc, *typ = NULL, *tb = NULL, *hook = NULL, *result;
    JSValue h;

    qp_capi_handle_system_exit(ctx);
    exc = PyErr_GetRaisedException();
    if (exc == NULL)
        return;
    typ = Py_NewRef((PyObject *)Py_TYPE(exc));
    tb = PyException_GetTraceback(exc);
    if (tb == NULL)
        tb = Py_NewRef(Py_None);
    if (set_sys_last_vars) {
        JSValue e = qp_capi_value(ctx, exc);
        JSValue t = qp_capi_value(ctx, typ);
        JSValue b = qp_capi_value(ctx, tb);
        if (JS_IsException(e) || JS_IsException(t) || JS_IsException(b)) {
            qp_err_clear(ctx);
        } else {
            qp_capi_set_sys_attr(ctx, "last_exc", e);
            qp_capi_set_sys_attr(ctx, "last_type", t);
            qp_capi_set_sys_attr(ctx, "last_value", e);
            qp_capi_set_sys_attr(ctx, "last_traceback", b);
        }
    }
    h = qp_sys_get(ctx, QP_ATOM(ctx, excepthook));
    if (JS_IsException(h)) {
        qp_err_clear(ctx);
    } else if (!QP_IS_NULL(h)) {
        hook = qp_capi_steal(ctx, h);
        if (!hook)
            qp_err_clear(ctx);
    }
    if (PySys_Audit("sys.excepthook", "OOOO", hook ? hook : Py_None, typ, exc, tb) < 0) {
        if (PyErr_ExceptionMatches(PyExc_RuntimeError)) {
            PyErr_Clear();
            goto done;
        }
        PyErr_FormatUnraisable("Exception ignored in audit hook");
    }
    if (hook == NULL) {
        qp_sys_write_stderr(ctx, "sys.excepthook is missing\n");
        PyErr_DisplayException(exc);
        goto done;
    }
    {
        PyObject *args[3] = { typ, exc, tb };
        result = PyObject_Vectorcall(hook, args, 3, NULL);
    }
    if (result == NULL) {
        PyObject *exc2;
        qp_capi_handle_system_exit(ctx);
        exc2 = PyErr_GetRaisedException();
        fflush(stdout);
        qp_sys_write_stderr(ctx, "Error in sys.excepthook:\n");
        PyErr_DisplayException(exc2);
        qp_sys_write_stderr(ctx, "\nOriginal exception was:\n");
        PyErr_DisplayException(exc);
        Py_XDECREF(exc2);
    } else {
        Py_DECREF(result);
    }
 done:
    Py_XDECREF(hook);
    Py_XDECREF(typ);
    Py_XDECREF(exc);
    Py_XDECREF(tb);
}

void PyErr_Print(void)
{
    PyErr_PrintEx(1);
}

/* ========================================================================= */
/* Signals                                                                    */
/* ========================================================================= */

int PyErr_CheckSignals(void)
{
    return qp_signal_check(qp_capi.ctx);
}

int PyErr_SetInterruptEx(int signum)
{
    JSContext *ctx = qp_capi.ctx;

    if (signum < 1 || signum >= QP_NSIG)
        return -1;
    /* the handler table lives in the module state; no allocation here
       (async-signal-safe): without it no Python handler is installed */
    if (!ctx || !QP_STATE(ctx)->mod_state)
        return 0;
    if (qp_signal_trip(ctx, signum) < 0)
        qp_err_clear(ctx);
    return 0;
}

void PyErr_SetInterrupt(void)
{
    (void)PyErr_SetInterruptEx(SIGINT);
}

int PySignal_SetWakeupFd(int fd)
{
    int old_fd = qp_signal_wakeup_fd;
    if (fd < 0)
        fd = -1;
    qp_signal_wakeup_fd = fd;
    qp_signal_wakeup_warn = 1;
    return old_fd;
}
