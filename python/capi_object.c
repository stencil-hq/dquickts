/*
 * dquickts Python C API, unit A (object protocol): attributes, rich
 * comparison, hashing, repr/str, truth, items, iteration, isinstance,
 * format/dir, generic attribute and dict access, finalizers, and the call
 * API (PyObject_Call*, PyObject_Vectorcall*, _PyObject_* helpers).
 *
 * Semantics and messages: CPython 3.15 Objects/object.c, Objects/call.c,
 * Objects/abstract.c (object part).  The Python-level operations are the
 * M2 object protocol of python/object.h (unit h); arguments cross with the
 * unit A functions of capi_mirror.c.
 */

/* internal CPython names implemented here (declared by capi/compat shims) */
PyAPI_FUNC(PyObject *) _PyObject_CallMethod(PyObject *obj, PyObject *name, const char *format, ...);
PyAPI_FUNC(PyObject *) _PyObject_CallFunction_SizeT(PyObject *callable, const char *format, ...);
PyAPI_FUNC(PyObject *) _PyObject_CallMethod_SizeT(PyObject *obj, const char *name, const char *format, ...);
PyAPI_FUNC(PyObject *) _PyObject_MakeTpCall(PyThreadState *tstate, PyObject *callable,
                                            PyObject *const *args, Py_ssize_t nargs, PyObject *keywords);
PyAPI_FUNC(PyObject *) _Py_CheckFunctionResult(PyThreadState *tstate, PyObject *callable,
                                               PyObject *result, const char *where);
PyAPI_FUNC(PyObject *) _PyObject_LookupSpecial(PyObject *self, PyObject *attr);
PyAPI_FUNC(PyObject *) _PyObject_MaybeCallSpecialNoArgs(PyObject *self, PyObject *attr);
PyAPI_FUNC(int) _PyObject_VisitType(PyObject *op, visitproc visit, void *arg);
PyAPI_FUNC(PyObject *) _PyErr_FormatFromCause(PyObject *exception, const char *format, ...);

#define QP_CAPI_CTX() (qp_capi.ctx)

/* SystemError "null argument to internal routine" unless an error is set */
static PyObject *qp_capi_null_error(void)
{
    JSContext *ctx = QP_CAPI_CTX();
    if (ctx && !JS_HasException(ctx))
        qp_raise_system_error(ctx, "null argument to internal routine");
    return NULL;
}

static int qp_capi_null_error_int(void)
{
    qp_capi_null_error();
    return -1;
}

/* atom of PyUnicode_FromString(name): UnicodeDecodeError when name is not
   valid UTF-8 (the *AttrString functions) */
static JSAtom qp_capi_cstr_atom(JSContext *ctx, const char *name)
{
    const unsigned char *p;
    JSValue s;
    JSAtom atom;
    for (p = (const unsigned char *)name; *p; p++) {
        if (*p >= 0x80)
            break;
    }
    if (!*p)
        return JS_NewAtom(ctx, name);
    s = qp_str_decode(ctx, name, (Py_ssize_t)strlen(name), "utf-8", "strict");
    if (JS_IsException(s))
        return JS_ATOM_NULL;
    atom = qp_str_to_atom(ctx, s);
    JS_FreeValue(ctx, s);
    return atom;
}

/* atom of an attribute name object (TypeError for non-str) */
static JSAtom qp_capi_name_atom(JSContext *ctx, PyObject *name)
{
    JSValue v, s;
    JSAtom atom;
    if (!PyUnicode_Check(name)) {
        qp_raise_type_error(ctx, "attribute name must be string, not '%.200s'",
                            Py_TYPE(name)->tp_name);
        return JS_ATOM_NULL;
    }
    v = qp_capi_value(ctx, name);
    if (JS_IsException(v))
        return JS_ATOM_NULL;
    if (qp_str_check_exact(v))
        return qp_str_to_atom(ctx, v);
    s = qp_str_value(ctx, v);
    if (JS_IsException(s))
        return JS_ATOM_NULL;
    atom = qp_str_to_atom(ctx, s);
    JS_FreeValue(ctx, s);
    return atom;
}

/* ========================================================================= */
/* Attributes                                                                 */
/* ========================================================================= */

PyObject *PyObject_GetAttr(PyObject *v, PyObject *name)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, r;
    JSAtom atom;
    if (!v || !name)
        return qp_capi_null_error();
    atom = qp_capi_name_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return NULL;
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov)) {
        JS_FreeAtom(ctx, atom);
        return NULL;
    }
    r = qp_getattr(ctx, ov, atom);
    JS_FreeAtom(ctx, atom);
    return qp_capi_from_js_result(ctx, r);
}

PyObject *PyObject_GetAttrString(PyObject *v, const char *name)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, r;
    JSAtom atom;
    if (!v || !name)
        return qp_capi_null_error();
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov))
        return NULL;
    atom = qp_capi_cstr_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return NULL;
    r = qp_getattr(ctx, ov, atom);
    JS_FreeAtom(ctx, atom);
    return qp_capi_from_js_result(ctx, r);
}

static int qp_capi_getattr_opt_atom(JSContext *ctx, PyObject *v, JSAtom atom, PyObject **result)
{
    JSValue ov, r;
    int ret;
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov)) {
        *result = NULL;
        return -1;
    }
    ret = qp_getattr_opt(ctx, ov, atom, &r);
    if (ret <= 0) {
        *result = NULL;
        return ret;
    }
    *result = qp_capi_steal(ctx, r);
    return *result ? 1 : -1;
}

int PyObject_GetOptionalAttr(PyObject *v, PyObject *name, PyObject **result)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSAtom atom;
    int ret;
    if (!v || !name) {
        *result = NULL;
        return qp_capi_null_error_int();
    }
    atom = qp_capi_name_atom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        *result = NULL;
        return -1;
    }
    ret = qp_capi_getattr_opt_atom(ctx, v, atom, result);
    JS_FreeAtom(ctx, atom);
    return ret;
}

int PyObject_GetOptionalAttrString(PyObject *v, const char *name, PyObject **result)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSAtom atom;
    int ret;
    if (!v || !name) {
        *result = NULL;
        return qp_capi_null_error_int();
    }
    atom = qp_capi_cstr_atom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        *result = NULL;
        return -1;
    }
    ret = qp_capi_getattr_opt_atom(ctx, v, atom, result);
    JS_FreeAtom(ctx, atom);
    return ret;
}

int PyObject_HasAttrWithError(PyObject *v, PyObject *name)
{
    PyObject *res;
    int rc = PyObject_GetOptionalAttr(v, name, &res);
    Py_XDECREF(res);
    return rc;
}

int PyObject_HasAttrStringWithError(PyObject *v, const char *name)
{
    PyObject *res;
    int rc = PyObject_GetOptionalAttrString(v, name, &res);
    Py_XDECREF(res);
    return rc;
}

/* CPython: errors other than AttributeError are reported as unraisable */
static int qp_capi_hasattr_result(int rc)
{
    if (rc < 0) {
        PyErr_FormatUnraisable("Exception ignored in PyObject_HasAttr(); consider using "
                               "PyObject_HasAttrWithError(), PyObject_GetOptionalAttr() "
                               "or PyObject_GetAttr()");
        return 0;
    }
    return rc;
}

int PyObject_HasAttr(PyObject *v, PyObject *name)
{
    return qp_capi_hasattr_result(PyObject_HasAttrWithError(v, name));
}

int PyObject_HasAttrString(PyObject *v, const char *name)
{
    return qp_capi_hasattr_result(PyObject_HasAttrStringWithError(v, name));
}

static int qp_capi_setattr_atom(JSContext *ctx, PyObject *v, JSAtom atom, PyObject *w)
{
    JSValue ov, wv = QP_NULL;
    int r;
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov))
        return -1;
    if (w) {
        wv = qp_capi_value(ctx, w);
        if (JS_IsException(wv))
            return -1;
        r = qp_setattr(ctx, ov, atom, wv);
    } else {
        r = qp_delattr(ctx, ov, atom);
    }
    return r < 0 ? -1 : 0;
}

int PyObject_SetAttr(PyObject *v, PyObject *name, PyObject *w)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSAtom atom;
    int r;
    if (!v || !name)
        return qp_capi_null_error_int();
    atom = qp_capi_name_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_capi_setattr_atom(ctx, v, atom, w);
    JS_FreeAtom(ctx, atom);
    return r;
}

int PyObject_SetAttrString(PyObject *v, const char *name, PyObject *w)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSAtom atom;
    int r;
    if (!v || !name)
        return qp_capi_null_error_int();
    atom = qp_capi_cstr_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_capi_setattr_atom(ctx, v, atom, w);
    JS_FreeAtom(ctx, atom);
    return r;
}

int PyObject_DelAttr(PyObject *v, PyObject *name)
{
    return PyObject_SetAttr(v, name, NULL);
}

int PyObject_DelAttrString(PyObject *v, const char *name)
{
    return PyObject_SetAttrString(v, name, NULL);
}

PyObject *_PyObject_GetAttrId(PyObject *v, _Py_Identifier *name)
{
    PyObject *n = qp_capi_intern_id(name->string);
    if (!n)
        return NULL;
    return PyObject_GetAttr(v, n);
}

PyObject *PyObject_GenericGetAttr(PyObject *obj, PyObject *name)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, r;
    JSAtom atom = qp_capi_name_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return NULL;
    ov = qp_capi_value(ctx, obj);
    if (JS_IsException(ov)) {
        JS_FreeAtom(ctx, atom);
        return NULL;
    }
    r = qp_generic_getattr(ctx, ov, atom);
    JS_FreeAtom(ctx, atom);
    return qp_capi_from_js_result(ctx, r);
}

int PyObject_GenericSetAttr(PyObject *obj, PyObject *name, PyObject *value)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, vv = QP_NULL;
    JSAtom atom = qp_capi_name_atom(ctx, name);
    int r;
    if (atom == JS_ATOM_NULL)
        return -1;
    ov = qp_capi_value(ctx, obj);
    if (!JS_IsException(ov) && value)
        vv = qp_capi_value(ctx, value);
    if (JS_IsException(ov) || JS_IsException(vv)) {
        JS_FreeAtom(ctx, atom);
        return -1;
    }
    r = qp_generic_setattr(ctx, ov, atom, vv);
    JS_FreeAtom(ctx, atom);
    return r < 0 ? -1 : 0;
}

/* data descriptor of type(obj) named atom? (takes precedence over a dict) */
static BOOL qp_capi_has_data_descr(JSContext *ctx, JSValueConst ov, JSAtom atom)
{
    JSValue d = qp_type_lookup(ctx, qp_type_of(ctx, ov), atom);
    if (QP_IS_NULL(d))
        return FALSE;
    return qp_type_of(ctx, d)->slots.tp_descr_set != NULL;
}

/* AttributeError pending -> cleared, TRUE */
static BOOL qp_capi_clear_attribute_error(void)
{
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return TRUE;
    }
    return FALSE;
}

PyObject *_PyObject_GenericGetAttrWithDict(PyObject *obj, PyObject *name,
                                           PyObject *dict, int suppress)
{
    JSContext *ctx = QP_CAPI_CTX();
    PyObject *res;
    if (dict) {
        JSValue ov = qp_capi_value(ctx, obj);
        JSAtom atom;
        BOOL data;
        if (JS_IsException(ov))
            return NULL;
        atom = qp_capi_name_atom(ctx, name);
        if (atom == JS_ATOM_NULL)
            return NULL;
        data = qp_capi_has_data_descr(ctx, ov, atom);
        JS_FreeAtom(ctx, atom);
        if (!data) {
            int rc = PyDict_GetItemRef(dict, name, &res);
            if (rc < 0)
                return NULL;
            if (rc > 0)
                return res;
        }
    }
    res = PyObject_GenericGetAttr(obj, name);
    if (!res && suppress)
        qp_capi_clear_attribute_error();
    return res;
}

int _PyObject_GenericSetAttrWithDict(PyObject *obj, PyObject *name,
                                     PyObject *value, PyObject *dict)
{
    JSContext *ctx = QP_CAPI_CTX();
    if (dict) {
        JSValue ov = qp_capi_value(ctx, obj);
        JSAtom atom;
        BOOL data;
        int r;
        if (JS_IsException(ov))
            return -1;
        atom = qp_capi_name_atom(ctx, name);
        if (atom == JS_ATOM_NULL)
            return -1;
        data = qp_capi_has_data_descr(ctx, ov, atom);
        JS_FreeAtom(ctx, atom);
        if (!data) {
            if (value)
                return PyDict_SetItem(dict, name, value);
            r = PyDict_DelItem(dict, name);
            if (r < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
                PyErr_Format(PyExc_AttributeError,
                             "'%.100s' object has no attribute '%U'",
                             Py_TYPE(obj)->tp_name, name);
            }
            return r;
        }
    }
    return PyObject_GenericSetAttr(obj, name, value);
}

PyObject *PyObject_GenericGetDict(PyObject *obj, void *context)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, obj);
    (void)context;
    if (JS_IsException(ov))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_object_get_dict(ctx, ov));
}

int PyObject_GenericSetDict(PyObject *obj, PyObject *value, void *context)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, vv;
    JSAtom atom;
    int r;
    (void)context;
    if (!value) {
        qp_raise_type_error(ctx, "cannot delete __dict__");
        return -1;
    }
    if (!PyDict_Check(value)) {
        qp_raise_type_error(ctx, "__dict__ must be set to a dictionary, not a '%.200s'",
                            Py_TYPE(value)->tp_name);
        return -1;
    }
    ov = qp_capi_value(ctx, obj);
    if (JS_IsException(ov))
        return -1;
    vv = qp_capi_value(ctx, value);
    if (JS_IsException(vv))
        return -1;
    atom = JS_NewAtom(ctx, "__dict__");
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_generic_setattr(ctx, ov, atom, vv);
    JS_FreeAtom(ctx, atom);
    return r < 0 ? -1 : 0;
}

/* dicts of instances live on their JS objects (design 1.5) */
PyObject **_PyObject_GetDictPtr(PyObject *obj)
{
    (void)obj;
    return NULL;
}

int PyObject_VisitManagedDict(PyObject *obj, visitproc visit, void *arg)
{
    (void)obj;
    (void)visit;
    (void)arg;
    return 0;
}

void PyObject_ClearManagedDict(PyObject *obj)
{
    (void)obj;
}

/* weak references are cleared by QuickJS when the JS object dies */
void PyObject_ClearWeakRefs(PyObject *object)
{
    (void)object;
}

void PyUnstable_Object_ClearWeakRefsNoCallbacks(PyObject *object)
{
    (void)object;
}

PyObject *_PyObject_LookupSpecial(PyObject *self, PyObject *attr)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, r;
    JSAtom atom;
    ov = qp_capi_value(ctx, self);
    if (JS_IsException(ov))
        return NULL;
    atom = qp_capi_name_atom(ctx, attr);
    if (atom == JS_ATOM_NULL)
        return NULL;
    r = qp_lookup_special(ctx, ov, atom);
    JS_FreeAtom(ctx, atom);
    if (QP_IS_NULL(r))
        return NULL;
    return qp_capi_from_js_result(ctx, r);
}

PyObject *_PyObject_MaybeCallSpecialNoArgs(PyObject *self, PyObject *attr)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, m, r;
    JSAtom atom;
    ov = qp_capi_value(ctx, self);
    if (JS_IsException(ov))
        return NULL;
    atom = qp_capi_name_atom(ctx, attr);
    if (atom == JS_ATOM_NULL)
        return NULL;
    m = qp_lookup_special(ctx, ov, atom);
    JS_FreeAtom(ctx, atom);
    if (QP_IS_NULL(m))
        return NULL;
    if (JS_IsException(m))
        return NULL;
    r = qp_call0(ctx, m);
    JS_FreeValue(ctx, m);
    return qp_capi_from_js_result(ctx, r);
}

int _PyObject_VisitType(PyObject *op, visitproc visit, void *arg)
{
    PyTypeObject *tp = Py_TYPE(op);
    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)
        Py_VISIT(tp);
    return 0;
}

/* ========================================================================= */
/* repr, str, hash, comparison, truth                                         */
/* ========================================================================= */

static PyObject *qp_capi_unary_str(PyObject *v, JSValue (*fn)(JSContext *, JSValueConst))
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov;
    if (!v)
        return PyUnicode_FromString("<NULL>");
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov))
        return NULL;
    return qp_capi_from_js_result(ctx, fn(ctx, ov));
}

PyObject *PyObject_Repr(PyObject *v)
{
    return qp_capi_unary_str(v, qp_repr);
}

PyObject *PyObject_Str(PyObject *v)
{
    if (v && PyUnicode_CheckExact(v))
        return Py_NewRef(v);
    return qp_capi_unary_str(v, qp_str);
}

PyObject *PyObject_ASCII(PyObject *v)
{
    return qp_capi_unary_str(v, qp_ascii);
}

Py_hash_t PyObject_Hash(PyObject *v)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov))
        return -1;
    return qp_hash(ctx, ov);
}

Py_hash_t PyObject_HashNotImplemented(PyObject *v)
{
    JSContext *ctx = QP_CAPI_CTX();
    qp_raise_type_error(ctx, "unhashable type: '%.200s'", Py_TYPE(v)->tp_name);
    return -1;
}

PyObject *PyObject_RichCompare(PyObject *v, PyObject *w, int op)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue a, b;
    if (op < Py_LT || op > Py_GE) {
        qp_raise_system_error(ctx, "bad argument to internal function");
        return NULL;
    }
    if (!v || !w)
        return qp_capi_null_error();
    a = qp_capi_value(ctx, v);
    if (JS_IsException(a))
        return NULL;
    b = qp_capi_value(ctx, w);
    if (JS_IsException(b))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_richcompare(ctx, a, b, op));
}

int PyObject_RichCompareBool(PyObject *v, PyObject *w, int op)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue a, b;
    if (v == w) {
        if (op == Py_EQ)
            return 1;
        if (op == Py_NE)
            return 0;
    }
    if (op < Py_LT || op > Py_GE) {
        qp_raise_system_error(ctx, "bad argument to internal function");
        return -1;
    }
    if (!v || !w)
        return qp_capi_null_error_int();
    a = qp_capi_value(ctx, v);
    if (JS_IsException(a))
        return -1;
    b = qp_capi_value(ctx, w);
    if (JS_IsException(b))
        return -1;
    return qp_richcompare_bool(ctx, a, b, op);
}

int PyObject_IsTrue(PyObject *v)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov;
    if (v == Py_True)
        return 1;
    if (v == Py_False || v == Py_None)
        return 0;
    ov = qp_capi_value(ctx, v);
    if (JS_IsException(ov))
        return -1;
    return qp_bool(ctx, ov);
}

int PyObject_Not(PyObject *v)
{
    int res = PyObject_IsTrue(v);
    if (res < 0)
        return res;
    return res == 0;
}

int Py_ReprEnter(PyObject *obj)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, obj);
    if (JS_IsException(ov))
        return -1;
    return qp_repr_enter(ctx, ov);
}

void Py_ReprLeave(PyObject *obj)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov;
    PyObject *exc = PyErr_GetRaisedException();
    ov = qp_capi_value(ctx, obj);
    if (!JS_IsException(ov))
        qp_repr_leave(ctx, ov);
    else
        PyErr_Clear();
    PyErr_SetRaisedException(exc);
}

/* ========================================================================= */
/* Items, length, iteration, type checks (object part of abstract.h)          */
/* ========================================================================= */

PyObject *PyObject_Type(PyObject *o)
{
    if (!o)
        return qp_capi_null_error();
    return Py_NewRef((PyObject *)Py_TYPE(o));
}

Py_ssize_t PyObject_Size(PyObject *o)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov;
    if (!o) {
        qp_capi_null_error();
        return -1;
    }
    ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return -1;
    return qp_len(ctx, ov);
}

#undef PyObject_Length
Py_ssize_t PyObject_Length(PyObject *o)
{
    return PyObject_Size(o);
}

Py_ssize_t PyObject_LengthHint(PyObject *o, Py_ssize_t defaultvalue)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return -1;
    return qp_length_hint(ctx, ov, defaultvalue);
}

PyObject *PyObject_GetItem(PyObject *o, PyObject *key)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, kv;
    if (!o || !key)
        return qp_capi_null_error();
    ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return NULL;
    kv = qp_capi_value(ctx, key);
    if (JS_IsException(kv))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_getitem(ctx, ov, kv));
}

int PyObject_SetItem(PyObject *o, PyObject *key, PyObject *value)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, kv, vv;
    if (!o || !key || !value)
        return qp_capi_null_error_int();
    ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return -1;
    kv = qp_capi_value(ctx, key);
    if (JS_IsException(kv))
        return -1;
    vv = qp_capi_value(ctx, value);
    if (JS_IsException(vv))
        return -1;
    return qp_setitem(ctx, ov, kv, vv) < 0 ? -1 : 0;
}

int PyObject_DelItem(PyObject *o, PyObject *key)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, kv;
    if (!o || !key)
        return qp_capi_null_error_int();
    ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return -1;
    kv = qp_capi_value(ctx, key);
    if (JS_IsException(kv))
        return -1;
    return qp_delitem(ctx, ov, kv) < 0 ? -1 : 0;
}

int PyObject_DelItemString(PyObject *o, const char *key)
{
    PyObject *okey;
    int ret;
    if (!o || !key)
        return qp_capi_null_error_int();
    okey = PyUnicode_FromString(key);
    if (!okey)
        return -1;
    ret = PyObject_DelItem(o, okey);
    Py_DECREF(okey);
    return ret;
}

PyObject *PyObject_GetIter(PyObject *o)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_iter(ctx, ov));
}

PyObject *PyObject_GetAIter(PyObject *o)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_get_aiter(ctx, ov));
}

PyObject *PyObject_SelfIter(PyObject *obj)
{
    return Py_NewRef(obj);
}

int PyObject_IsInstance(PyObject *inst, PyObject *cls)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue iv, cv;
    if (!inst || !cls)
        return qp_capi_null_error_int();
    if (Py_TYPE(inst) == (PyTypeObject *)cls)
        return 1;
    iv = qp_capi_value(ctx, inst);
    if (JS_IsException(iv))
        return -1;
    cv = qp_capi_value(ctx, cls);
    if (JS_IsException(cv))
        return -1;
    return qp_isinstance(ctx, iv, cv);
}

int PyObject_IsSubclass(PyObject *derived, PyObject *cls)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue dv, cv;
    if (!derived || !cls)
        return qp_capi_null_error_int();
    dv = qp_capi_value(ctx, derived);
    if (JS_IsException(dv))
        return -1;
    cv = qp_capi_value(ctx, cls);
    if (JS_IsException(cv))
        return -1;
    return qp_issubclass(ctx, dv, cv);
}

PyObject *PyObject_Format(PyObject *obj, PyObject *format_spec)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov, sv = QP_NULL;
    if (format_spec && !PyUnicode_Check(format_spec)) {
        qp_raise_system_error(ctx, "Format specifier must be a string, not %.200s",
                              Py_TYPE(format_spec)->tp_name);
        return NULL;
    }
    /* fast path for common types with an empty spec (CPython) */
    if (!format_spec || PyUnicode_GET_LENGTH(format_spec) == 0) {
        if (PyUnicode_CheckExact(obj))
            return Py_NewRef(obj);
        if (PyLong_CheckExact(obj))
            return PyObject_Str(obj);
    }
    ov = qp_capi_value(ctx, obj);
    if (JS_IsException(ov))
        return NULL;
    if (format_spec) {
        sv = qp_capi_value(ctx, format_spec);
        if (JS_IsException(sv))
            return NULL;
    }
    return qp_capi_from_js_result(ctx, qp_format(ctx, ov, sv));
}

PyObject *PyObject_Dir(PyObject *obj)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue ov;
    if (!obj) {
        /* dir() of the current frame's locals, sorted */
        PyObject *locals = PyEval_GetFrameLocals();
        PyObject *names;
        if (!locals)
            return NULL;
        names = PyMapping_Keys(locals);
        Py_DECREF(locals);
        if (!names)
            return NULL;
        if (!PyList_Check(names)) {
            PyErr_Format(PyExc_TypeError,
                         "dir(): expected keys() of locals to be a list, not '%.200s'",
                         Py_TYPE(names)->tp_name);
            Py_DECREF(names);
            return NULL;
        }
        if (PyList_Sort(names) < 0) {
            Py_DECREF(names);
            return NULL;
        }
        return names;
    }
    ov = qp_capi_value(ctx, obj);
    if (JS_IsException(ov))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_dir(ctx, ov));
}

/* type(v)'s slots: natives use their C type, everything else its QP type */
static QPType *qp_capi_qp_type_of_obj(JSContext *ctx, PyObject *o)
{
    JSValue ov = qp_capi_value(ctx, o);
    if (JS_IsException(ov)) {
        PyErr_Clear();
        return NULL;
    }
    return qp_type_of(ctx, ov);
}

int PyCallable_Check(PyObject *x)
{
    JSContext *ctx = QP_CAPI_CTX();
    QPType *t;
    JSValue ov;
    if (!x)
        return 0;
    if (Py_TYPE(x)->tp_call != NULL && QP_CAPI_HAS_HEAD(x) &&
        (QP_CAPI_HEAD(x)->flags & QP_CAPI_F_NATIVE))
        return 1;
    ov = qp_capi_value(ctx, x);
    if (JS_IsException(ov)) {
        PyErr_Clear();
        return Py_TYPE(x)->tp_call != NULL;
    }
    t = qp_type_of(ctx, ov);
    if (t->slots.tp_call != NULL)
        return 1;
    return JS_IsFunction(ctx, ov);
}

int PyIndex_Check(PyObject *obj)
{
    JSContext *ctx = QP_CAPI_CTX();
    QPType *t;
    PyNumberMethods *nb;
    if (PyLong_Check(obj))
        return 1;
    nb = Py_TYPE(obj)->tp_as_number;
    if (QP_CAPI_HAS_HEAD(obj) && (QP_CAPI_HEAD(obj)->flags & QP_CAPI_F_NATIVE))
        return nb != NULL && nb->nb_index != NULL;
    t = qp_capi_qp_type_of_obj(ctx, obj);
    if (!t)
        return nb != NULL && nb->nb_index != NULL;
    return t->slots.nb.nb_index != NULL;
}

/* ========================================================================= */
/* Printing and debugging                                                     */
/* ========================================================================= */

int PyObject_Print(PyObject *op, FILE *fp, int flags)
{
    int ret = 0;
    clearerr(fp);
    if (!op) {
        fprintf(fp, "<nil>");
    } else {
        PyObject *s = (flags & Py_PRINT_RAW) ? PyObject_Str(op) : PyObject_Repr(op);
        if (!s) {
            ret = -1;
        } else {
            Py_ssize_t len;
            const char *t = PyUnicode_AsUTF8AndSize(s, &len);
            if (!t)
                ret = -1;
            else
                fwrite(t, 1, (size_t)len, fp);
            Py_DECREF(s);
        }
    }
    if (ret == 0 && ferror(fp)) {
        PyErr_SetFromErrno(PyExc_OSError);
        clearerr(fp);
        ret = -1;
    }
    return ret;
}

void PyObject_Dump(PyObject *op)
{
    if (!op) {
        fprintf(stderr, "<object at NULL>\n");
        fflush(stderr);
        return;
    }
    fprintf(stderr, "object address  : %p\n", (void *)op);
    fprintf(stderr, "object refcount : %zd\n", (Py_ssize_t)op->ob_refcnt);
    fprintf(stderr, "object type     : %p\n", (void *)Py_TYPE(op));
    fprintf(stderr, "object type name: %s\n",
            Py_TYPE(op) ? Py_TYPE(op)->tp_name : "NULL");
    if (QP_CAPI_HAS_HEAD(op)) {
        QPCapiHead *h = QP_CAPI_HEAD(op);
        fprintf(stderr, "object mirror   : kind %s, flags 0x%x%s\n",
                h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].name ?
                qp_capi.kind_ops[h->kind].name : "?",
                h->flags, QP_IS_NULL(h->v) ? ", unbound" : "");
    }
    fprintf(stderr, "object repr     : ");
    fflush(stderr);
    if (qp_capi.ctx) {
        PyObject *exc = PyErr_GetRaisedException();
        (void)PyObject_Print(op, stderr, 0);
        fflush(stderr);
        PyErr_Clear();
        PyErr_SetRaisedException(exc);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

void _PyObject_AssertFailed(PyObject *obj, const char *expr, const char *msg,
                            const char *file, int line, const char *function)
{
    fprintf(stderr, "%s:%d: ", file, line);
    if (function)
        fprintf(stderr, "%s: ", function);
    fflush(stderr);
    if (expr)
        fprintf(stderr, "Assertion \"%s\" failed", expr);
    else
        fprintf(stderr, "Assertion failed");
    if (msg)
        fprintf(stderr, ": %s", msg);
    fprintf(stderr, "\n");
    fflush(stderr);
    if (obj)
        PyObject_Dump(obj);
    abort();
}

/* "qualname()" / "module.qualname()" of a callable for error messages */
PyObject *_PyObject_FunctionStr(PyObject *x)
{
    PyObject *qualname, *module = NULL, *result;
    int rc;
    rc = PyObject_GetOptionalAttr(x, qp_capi_intern_id("__qualname__"), &qualname);
    if (rc <= 0) {
        if (rc < 0)
            return NULL;
        return PyObject_Str(x);
    }
    rc = PyObject_GetOptionalAttr(x, qp_capi_intern_id("__module__"), &module);
    if (rc < 0) {
        Py_DECREF(qualname);
        return NULL;
    }
    if (module && module != Py_None && PyUnicode_Check(module) &&
        !PyUnicode_EqualToUTF8(module, "builtins"))
        result = PyUnicode_FromFormat("%S.%S()", module, qualname);
    else
        result = PyUnicode_FromFormat("%S()", qualname);
    Py_XDECREF(module);
    Py_DECREF(qualname);
    return result;
}

/* ========================================================================= */
/* Reference helpers of cpython/object.h                                      */
/* ========================================================================= */

void _Py_NewReference(PyObject *op)
{
    op->ob_refcnt = 1;
    if (QP_CAPI_HAS_HEAD(op)) {
        QPCapiHead *h = QP_CAPI_HEAD(op);
        h->flags &= ~QP_CAPI_F_DEALLOC;
        if (!(h->flags & QP_CAPI_F_LIVE))
            qp_capi_make_live(h);
    }
}

void _Py_NewReferenceNoTotal(PyObject *op)
{
    _Py_NewReference(op);
}

void _Py_ResurrectReference(PyObject *op)
{
    _Py_NewReference(op);
}

void _Py_ForgetReference(PyObject *op)
{
    (void)op;
}

void _Py_BreakPoint(void)
{
}

static PyRefTracer qp_capi_ref_tracer;
static void *qp_capi_ref_tracer_data;

/* tracers are recorded but never invoked (objects are created by the JS
   runtime as well, which has no tracing hooks) */
int PyRefTracer_SetTracer(PyRefTracer tracer, void *data)
{
    qp_capi_ref_tracer = tracer;
    qp_capi_ref_tracer_data = data;
    return 0;
}

PyRefTracer PyRefTracer_GetTracer(void **data)
{
    if (data)
        *data = qp_capi_ref_tracer_data;
    return qp_capi_ref_tracer;
}

void _PyTrash_thread_deposit_object(PyThreadState *tstate, PyObject *op)
{
    (void)tstate;
    Py_TYPE(op)->tp_dealloc(op);
}

void _PyTrash_thread_destroy_chain(PyThreadState *tstate)
{
    (void)tstate;
}

int PyUnstable_Object_EnableDeferredRefcount(PyObject *op)
{
    (void)op;
    return 0;
}

int PyUnstable_Object_IsUniqueReferencedTemporary(PyObject *op)
{
    (void)op;
    return 0;
}

int PyUnstable_IsImmortal(PyObject *op)
{
    return _Py_IsImmortal(op);
}

int PyUnstable_TryIncRef(PyObject *op)
{
    if (op->ob_refcnt == 0 && !_Py_IsImmortal(op))
        return 0;
    Py_INCREF(op);
    return 1;
}

void PyUnstable_EnableTryIncRef(PyObject *op)
{
    (void)op;
}

int PyUnstable_Object_IsUniquelyReferenced(PyObject *op)
{
    return Py_REFCNT(op) == 1;
}

int PyUnstable_SetImmortal(PyObject *op)
{
    if (!QP_CAPI_HAS_HEAD(op))
        return 0;
    if (Py_REFCNT(op) != 1)
        return 0;
    qp_capi_immortalize(op);
    return 1;
}

/* ========================================================================= */
/* Finalizers (PEP 442)                                                       */
/* ========================================================================= */

void PyObject_CallFinalizer(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);
    if (!tp->tp_finalize)
        return;
    if (QP_CAPI_HAS_HEAD(self)) {
        QPCapiHead *h = QP_CAPI_HEAD(self);
        if (h->flags & QP_CAPI_F_FINALIZED)
            return;
        h->flags |= QP_CAPI_F_FINALIZED;
    }
    tp->tp_finalize(self);
}

int PyObject_CallFinalizerFromDealloc(PyObject *self)
{
    QPCapiHead *h;
    if (self->ob_refcnt != 0) {
        fprintf(stderr, "Fatal Python error: PyObject_CallFinalizerFromDealloc called on "
                "object with a non-zero refcount\n");
        abort();
    }
    if (!QP_CAPI_HAS_HEAD(self))
        return 0;
    h = QP_CAPI_HEAD(self);
    /* already finalized, or inside a cycle collection (the M2 __del__
       gap: finalizers of garbage cycles are skipped) */
    if ((h->flags & QP_CAPI_F_FINALIZED) || !Py_TYPE(self)->tp_finalize ||
        (qp_capi.rt && qp_capi.rt->gc_phase == JS_GC_PHASE_REMOVE_CYCLES))
        return 0;
    /* temporarily resurrect: the DEALLOC flag keeps a nested drop to zero
       from re-entering _Py_Dealloc */
    self->ob_refcnt = 1;
    PyObject_CallFinalizer(self);
    if (--self->ob_refcnt == 0)
        return 0;       /* the finalizer did not resurrect it */
    /* resurrected: the object is alive again */
    h->flags &= ~QP_CAPI_F_DEALLOC;
    if (!(h->flags & QP_CAPI_F_LIVE))
        qp_capi_make_live(h);
    return -1;
}

/* ========================================================================= */
/* Calls                                                                      */
/* ========================================================================= */

#undef PyVectorcall_NARGS
Py_ssize_t PyVectorcall_NARGS(size_t nargsf)
{
    return _PyVectorcall_NARGS(nargsf);
}

vectorcallfunc PyVectorcall_Function(PyObject *callable)
{
    PyTypeObject *tp;
    Py_ssize_t offset;
    vectorcallfunc ptr;
    tp = Py_TYPE(callable);
    if (!PyType_HasFeature(tp, Py_TPFLAGS_HAVE_VECTORCALL))
        return NULL;
    offset = tp->tp_vectorcall_offset;
    if (offset <= 0)
        return NULL;
    memcpy(&ptr, (char *)callable + offset, sizeof(ptr));
    return ptr;
}

/* repr of a callable for the result-check messages (never fails) */
static JSValue qp_capi_callable_repr(JSContext *ctx, PyObject *callable)
{
    JSValue ov = qp_capi_value(ctx, callable);
    JSValue r;
    if (!JS_IsException(ov)) {
        r = qp_repr(ctx, ov);
        if (!JS_IsException(r))
            return r;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return qp_str_from_cstr(ctx, Py_TYPE(callable)->tp_name);
}

static PyObject *qp_capi_check_result(PyObject *callable, PyObject *result, const char *where)
{
    JSContext *ctx = QP_CAPI_CTX();
    if (!result) {
        if (!JS_HasException(ctx)) {
            if (where) {
                qp_raise_system_error(ctx, "%s returned NULL without setting an exception", where);
            } else {
                JSValue r = qp_capi_callable_repr(ctx, callable);
                if (!JS_IsException(r)) {
                    qp_raise_system_error(ctx, "%U returned NULL without setting an exception", r);
                    JS_FreeValue(ctx, r);
                }
            }
        }
        return NULL;
    }
    if (JS_HasException(ctx)) {
        Py_DECREF(result);
        if (where) {
            _PyErr_FormatFromCause(PyExc_SystemError,
                                   "%s returned a result with an exception set", where);
        } else {
            _PyErr_FormatFromCause(PyExc_SystemError,
                                   "%R returned a result with an exception set", callable);
        }
        return NULL;
    }
    return result;
}

PyObject *_Py_CheckFunctionResult(PyThreadState *tstate, PyObject *callable,
                                  PyObject *result, const char *where)
{
    (void)tstate;
    return qp_capi_check_result(callable, result, where);
}

/* JS path: callable(*args, **dict(zip(kwnames, args[nargs:]))) through the
   M2 call protocol; args are borrowed */
#define QP_CAPI_SMALL_STACK 8

static PyObject *qp_capi_vectorcall(PyObject *callable, PyObject *const *args,
                                    size_t nargsf, PyObject *kwnames)
{
    JSContext *ctx = QP_CAPI_CTX();
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t total = nargs + nkw, i;
    JSValue small[QP_CAPI_SMALL_STACK + 1], *buf = small;
    JSValue fv, kwv = QP_NULL, r;

    fv = qp_capi_value(ctx, callable);
    if (JS_IsException(fv))
        return NULL;
    if (kwnames) {
        kwv = qp_capi_value(ctx, kwnames);
        if (JS_IsException(kwv))
            return NULL;
    }
    if (total > QP_CAPI_SMALL_STACK) {
        buf = js_malloc(ctx, (size_t)(total + 1) * sizeof(JSValue));
        if (!buf)
            return NULL;
    }
    buf[0] = JS_UNDEFINED;      /* scratch slot for ARGUMENTS_OFFSET */
    for (i = 0; i < total; i++) {
        buf[i + 1] = qp_capi_value(ctx, args[i]);
        if (JS_IsException(buf[i + 1])) {
            if (buf != small)
                js_free(ctx, buf);
            return NULL;
        }
    }
    r = qp_vectorcall(ctx, fv, (JSValueConst *)(buf + 1), (size_t)nargs | QP_VECTORCALL_ARGUMENTS_OFFSET, kwv);
    if (buf != small)
        js_free(ctx, buf);
    return qp_capi_from_js_result(ctx, r);
}

/* call through C slots: extension instances keep their C tp_call /
   vectorcall; everything else goes through the JS path */
static BOOL qp_capi_use_c_call(PyObject *callable)
{
    return QP_CAPI_HAS_HEAD(callable) && (QP_CAPI_HEAD(callable)->flags & QP_CAPI_F_NATIVE);
}

/* dict(zip(kwnames, values)) */
PyObject *_PyStack_AsDict(PyObject *const *values, PyObject *kwnames)
{
    Py_ssize_t nkwargs, i;
    PyObject *kwdict;
    nkwargs = PyTuple_GET_SIZE(kwnames);
    kwdict = PyDict_New();
    if (!kwdict)
        return NULL;
    for (i = 0; i < nkwargs; i++) {
        if (PyDict_SetItem(kwdict, PyTuple_GET_ITEM(kwnames, i), *values++) < 0) {
            Py_DECREF(kwdict);
            return NULL;
        }
    }
    return kwdict;
}

/* tp_call of a callable without vectorcall (CPython _PyObject_MakeTpCall) */
PyObject *_PyObject_MakeTpCall(PyThreadState *tstate, PyObject *callable,
                               PyObject *const *args, Py_ssize_t nargs, PyObject *keywords)
{
    ternaryfunc call = Py_TYPE(callable)->tp_call;
    PyObject *argstuple, *kwdict = NULL, *result;
    Py_ssize_t i;
    (void)tstate;
    if (!call) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object is not callable",
                     Py_TYPE(callable)->tp_name);
        return NULL;
    }
    argstuple = PyTuple_New(nargs);
    if (!argstuple)
        return NULL;
    for (i = 0; i < nargs; i++)
        PyTuple_SET_ITEM(argstuple, i, Py_NewRef(args[i]));
    if (keywords && PyDict_Check(keywords)) {
        if (PyDict_GET_SIZE(keywords))
            kwdict = Py_NewRef(keywords);
    } else if (keywords && PyTuple_GET_SIZE(keywords)) {
        kwdict = _PyStack_AsDict(args + nargs, keywords);
        if (!kwdict) {
            Py_DECREF(argstuple);
            return NULL;
        }
    }
    if (Py_EnterRecursiveCall(" while calling a Python object")) {
        Py_DECREF(argstuple);
        Py_XDECREF(kwdict);
        return NULL;
    }
    result = call(callable, argstuple, kwdict);
    Py_LeaveRecursiveCall();
    Py_DECREF(argstuple);
    Py_XDECREF(kwdict);
    return qp_capi_check_result(callable, result, NULL);
}

PyObject *PyObject_Vectorcall(PyObject *callable, PyObject *const *args,
                              size_t nargsf, PyObject *kwnames)
{
    vectorcallfunc func;
    if (!callable)
        return qp_capi_null_error();
    func = PyVectorcall_Function(callable);
    if (func)
        return qp_capi_check_result(callable, func(callable, args, nargsf, kwnames), NULL);
    if (qp_capi_use_c_call(callable))
        return _PyObject_MakeTpCall(NULL, callable, args, PyVectorcall_NARGS(nargsf), kwnames);
    return qp_capi_vectorcall(callable, args, nargsf, kwnames);
}

/* unpack kwargs dict after the positional arguments: new stack (owned
   references, *pkwnames a new tuple); -1 on error */
static int qp_capi_unpack_dict(PyObject *const *args, Py_ssize_t nargs, PyObject *kwargs,
                               PyObject ***pstack, PyObject **pkwnames)
{
    Py_ssize_t nkw = PyDict_GET_SIZE(kwargs), i, pos = 0;
    PyObject **stack, *kwnames, *key, *value;
    stack = PyMem_Malloc((size_t)(1 + nargs + nkw) * sizeof(PyObject *));
    if (!stack) {
        PyErr_NoMemory();
        return -1;
    }
    kwnames = PyTuple_New(nkw);
    if (!kwnames) {
        PyMem_Free(stack);
        return -1;
    }
    stack++;    /* scratch slot for PY_VECTORCALL_ARGUMENTS_OFFSET */
    for (i = 0; i < nargs; i++)
        stack[i] = Py_NewRef(args[i]);
    i = 0;
    while (PyDict_Next(kwargs, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "keywords must be strings");
            for (pos = 0; pos < nargs + i; pos++)
                Py_DECREF(stack[pos]);
            PyMem_Free(stack - 1);
            Py_DECREF(kwnames);
            return -1;
        }
        PyTuple_SET_ITEM(kwnames, i, Py_NewRef(key));
        stack[nargs + i] = Py_NewRef(value);
        i++;
    }
    *pstack = stack;
    *pkwnames = kwnames;
    return 0;
}

static void qp_capi_free_stack(PyObject **stack, Py_ssize_t n, PyObject *kwnames)
{
    Py_ssize_t i;
    for (i = 0; i < n; i++)
        Py_DECREF(stack[i]);
    PyMem_Free(stack - 1);
    Py_DECREF(kwnames);
}

PyObject *PyObject_VectorcallDict(PyObject *callable, PyObject *const *args,
                                  size_t nargsf, PyObject *kwargs)
{
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    vectorcallfunc func;
    PyObject **stack, *kwnames, *res;
    if (!callable)
        return qp_capi_null_error();
    if (!kwargs || PyDict_GET_SIZE(kwargs) == 0)
        return PyObject_Vectorcall(callable, args, nargsf, NULL);
    func = PyVectorcall_Function(callable);
    if (!func && qp_capi_use_c_call(callable))
        return _PyObject_MakeTpCall(NULL, callable, args, nargs, kwargs);
    if (qp_capi_unpack_dict(args, nargs, kwargs, &stack, &kwnames) < 0)
        return NULL;
    if (func)
        res = qp_capi_check_result(callable,
                                   func(callable, stack, (size_t)nargs | PY_VECTORCALL_ARGUMENTS_OFFSET,
                                        kwnames), NULL);
    else
        res = qp_capi_vectorcall(callable, stack, (size_t)nargs, kwnames);
    qp_capi_free_stack(stack, nargs + PyTuple_GET_SIZE(kwnames), kwnames);
    return res;
}

PyObject *PyVectorcall_Call(PyObject *callable, PyObject *tuple, PyObject *kwargs)
{
    vectorcallfunc func = PyVectorcall_Function(callable);
    Py_ssize_t nargs;
    PyObject **stack, *kwnames, *res;
    if (!func) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object does not support vectorcall",
                     Py_TYPE(callable)->tp_name);
        return NULL;
    }
    nargs = PyTuple_GET_SIZE(tuple);
    if (!kwargs || PyDict_GET_SIZE(kwargs) == 0)
        return qp_capi_check_result(callable,
                                    func(callable, _PyTuple_CAST(tuple)->ob_item, (size_t)nargs, NULL), NULL);
    if (qp_capi_unpack_dict(_PyTuple_CAST(tuple)->ob_item, nargs, kwargs, &stack, &kwnames) < 0)
        return NULL;
    res = func(callable, stack, (size_t)nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    qp_capi_free_stack(stack, nargs + PyTuple_GET_SIZE(kwnames), kwnames);
    return qp_capi_check_result(callable, res, NULL);
}

PyObject *PyObject_Call(PyObject *callable, PyObject *args, PyObject *kwargs)
{
    JSContext *ctx = QP_CAPI_CTX();
    JSValue fv, av, kv = QP_NULL;
    ternaryfunc call;
    PyObject *result;

    if (!callable || !args)
        return qp_capi_null_error();
    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    }
    if (kwargs && !PyDict_Check(kwargs)) {
        PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary");
        return NULL;
    }
    if (PyVectorcall_Function(callable))
        return PyVectorcall_Call(callable, args, kwargs);
    if (qp_capi_use_c_call(callable)) {
        call = Py_TYPE(callable)->tp_call;
        if (!call) {
            PyErr_Format(PyExc_TypeError, "'%.200s' object is not callable",
                         Py_TYPE(callable)->tp_name);
            return NULL;
        }
        if (Py_EnterRecursiveCall(" while calling a Python object"))
            return NULL;
        result = call(callable, args, kwargs);
        Py_LeaveRecursiveCall();
        return qp_capi_check_result(callable, result, NULL);
    }
    fv = qp_capi_value(ctx, callable);
    if (JS_IsException(fv))
        return NULL;
    av = qp_capi_value(ctx, args);
    if (JS_IsException(av))
        return NULL;
    if (kwargs) {
        kv = qp_capi_value(ctx, kwargs);
        if (JS_IsException(kv))
            return NULL;
    }
    return qp_capi_from_js_result(ctx, qp_call_tuple(ctx, fv, av, kv));
}

PyObject *PyObject_CallObject(PyObject *callable, PyObject *args)
{
    if (!args)
        return PyObject_CallNoArgs(callable);
    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    }
    return PyObject_Call(callable, args, NULL);
}

PyObject *PyObject_CallNoArgs(PyObject *func)
{
    return PyObject_Vectorcall(func, NULL, 0 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
}

PyObject *PyObject_CallOneArg(PyObject *func, PyObject *arg)
{
    PyObject *stack[2];
    stack[0] = NULL;
    stack[1] = arg;
    return PyObject_Vectorcall(func, stack + 1, 1 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
}

/* callable(*Py_VaBuildValue(format, va)) with CPython's compatibility rule:
   a single tuple result is the argument tuple */
static PyObject *qp_capi_call_va(PyObject *callable, const char *format, va_list va)
{
    PyObject *args, *result;
    if (!callable)
        return qp_capi_null_error();
    if (!format || !*format)
        return PyObject_CallNoArgs(callable);
    args = Py_VaBuildValue(format, va);
    if (!args)
        return NULL;
    if (!PyTuple_Check(args)) {
        PyObject *t = PyTuple_New(1);
        if (!t) {
            Py_DECREF(args);
            return NULL;
        }
        PyTuple_SET_ITEM(t, 0, args);
        args = t;
    }
    result = PyObject_Call(callable, args, NULL);
    Py_DECREF(args);
    return result;
}

PyObject *PyObject_CallFunction(PyObject *callable, const char *format, ...)
{
    va_list va;
    PyObject *r;
    va_start(va, format);
    r = qp_capi_call_va(callable, format, va);
    va_end(va);
    return r;
}

PyObject *_PyObject_CallFunction_SizeT(PyObject *callable, const char *format, ...)
{
    va_list va;
    PyObject *r;
    va_start(va, format);
    r = qp_capi_call_va(callable, format, va);
    va_end(va);
    return r;
}

static PyObject *qp_capi_callmethod_va(PyObject *obj, PyObject *callable,
                                       const char *format, va_list va)
{
    PyObject *r;
    (void)obj;
    if (!callable)
        return NULL;
    if (!PyCallable_Check(callable)) {
        PyErr_Format(PyExc_TypeError, "attribute of type '%.200s' is not callable",
                     Py_TYPE(callable)->tp_name);
        Py_DECREF(callable);
        return NULL;
    }
    r = qp_capi_call_va(callable, format, va);
    Py_DECREF(callable);
    return r;
}

PyObject *PyObject_CallMethod(PyObject *obj, const char *name, const char *format, ...)
{
    va_list va;
    PyObject *r;
    if (!obj || !name)
        return qp_capi_null_error();
    va_start(va, format);
    r = qp_capi_callmethod_va(obj, PyObject_GetAttrString(obj, name), format, va);
    va_end(va);
    return r;
}

PyObject *_PyObject_CallMethod_SizeT(PyObject *obj, const char *name, const char *format, ...)
{
    va_list va;
    PyObject *r;
    if (!obj || !name)
        return qp_capi_null_error();
    va_start(va, format);
    r = qp_capi_callmethod_va(obj, PyObject_GetAttrString(obj, name), format, va);
    va_end(va);
    return r;
}

PyObject *_PyObject_CallMethod(PyObject *obj, PyObject *name, const char *format, ...)
{
    va_list va;
    PyObject *r;
    if (!obj || !name)
        return qp_capi_null_error();
    va_start(va, format);
    r = qp_capi_callmethod_va(obj, PyObject_GetAttr(obj, name), format, va);
    va_end(va);
    return r;
}

PyObject *_PyObject_CallMethodId(PyObject *obj, _Py_Identifier *name, const char *format, ...)
{
    va_list va;
    PyObject *r, *n;
    if (!obj || !name)
        return qp_capi_null_error();
    n = qp_capi_intern_id(name->string);
    if (!n)
        return NULL;
    va_start(va, format);
    r = qp_capi_callmethod_va(obj, PyObject_GetAttr(obj, n), format, va);
    va_end(va);
    return r;
}

/* collect a NULL-terminated argument list after `first` slots */
static PyObject **qp_capi_collect_va(PyObject **small, Py_ssize_t nsmall, Py_ssize_t first,
                                     va_list va, Py_ssize_t *pn)
{
    va_list count;
    Py_ssize_t n = 0, i;
    PyObject **stack;
    va_copy(count, va);
    while (va_arg(count, PyObject *) != NULL)
        n++;
    va_end(count);
    if (first + n + 1 <= nsmall) {
        stack = small;
    } else {
        stack = PyMem_Malloc((size_t)(first + n + 1) * sizeof(PyObject *));
        if (!stack) {
            PyErr_NoMemory();
            return NULL;
        }
    }
    for (i = 0; i < n; i++)
        stack[1 + first + i] = va_arg(va, PyObject *);
    *pn = n;
    return stack;
}

PyObject *PyObject_CallFunctionObjArgs(PyObject *callable, ...)
{
    PyObject *small[QP_CAPI_SMALL_STACK + 2], **stack, *r;
    Py_ssize_t n;
    va_list va;
    if (!callable)
        return qp_capi_null_error();
    va_start(va, callable);
    stack = qp_capi_collect_va(small, QP_CAPI_SMALL_STACK + 2, 0, va, &n);
    va_end(va);
    if (!stack)
        return NULL;
    stack[0] = NULL;
    r = PyObject_Vectorcall(callable, stack + 1, (size_t)n | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    if (stack != small)
        PyMem_Free(stack);
    return r;
}

PyObject *PyObject_VectorcallMethod(PyObject *name, PyObject *const *args,
                                    size_t nargsf, PyObject *kwnames)
{
    JSContext *ctx = QP_CAPI_CTX();
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t total, i;
    JSValue small[QP_CAPI_SMALL_STACK + 1], *buf = small;
    JSValue self, kwv = QP_NULL, r;
    JSAtom atom;

    if (!name || !args || nargs < 1)
        return qp_capi_null_error();
    if (qp_capi_use_c_call(args[0]) || !QP_CAPI_HAS_HEAD(args[0])) {
        /* generic form: bound attribute, then call with the rest */
        PyObject *callable = PyObject_GetAttr(args[0], name), *res;
        if (!callable)
            return NULL;
        res = PyObject_Vectorcall(callable, args + 1,
                                  (size_t)(nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
        Py_DECREF(callable);
        return res;
    }
    atom = qp_capi_name_atom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return NULL;
    self = qp_capi_value(ctx, args[0]);
    if (JS_IsException(self))
        goto fail_atom;
    if (kwnames) {
        kwv = qp_capi_value(ctx, kwnames);
        if (JS_IsException(kwv))
            goto fail_atom;
    }
    total = nargs - 1 + nkw;
    if (total > QP_CAPI_SMALL_STACK) {
        buf = js_malloc(ctx, (size_t)(total + 1) * sizeof(JSValue));
        if (!buf)
            goto fail_atom;
    }
    buf[0] = JS_UNDEFINED;
    for (i = 0; i < total; i++) {
        buf[i + 1] = qp_capi_value(ctx, args[i + 1]);
        if (JS_IsException(buf[i + 1])) {
            if (buf != small)
                js_free(ctx, buf);
            goto fail_atom;
        }
    }
    r = qp_call_method(ctx, self, atom, (JSValueConst *)(buf + 1),
                       (size_t)(nargs - 1) | QP_VECTORCALL_ARGUMENTS_OFFSET, kwv);
    if (buf != small)
        js_free(ctx, buf);
    JS_FreeAtom(ctx, atom);
    return qp_capi_from_js_result(ctx, r);
 fail_atom:
    JS_FreeAtom(ctx, atom);
    return NULL;
}

PyObject *PyObject_CallMethodObjArgs(PyObject *obj, PyObject *name, ...)
{
    PyObject *small[QP_CAPI_SMALL_STACK + 2], **stack, *r;
    Py_ssize_t n;
    va_list va;
    if (!obj || !name)
        return qp_capi_null_error();
    va_start(va, name);
    stack = qp_capi_collect_va(small, QP_CAPI_SMALL_STACK + 2, 1, va, &n);
    va_end(va);
    if (!stack)
        return NULL;
    stack[0] = NULL;
    stack[1] = obj;
    r = PyObject_VectorcallMethod(name, stack + 1,
                                  (size_t)(n + 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    if (stack != small)
        PyMem_Free(stack);
    return r;
}
