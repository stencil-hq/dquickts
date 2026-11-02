/*
 * Derived from CPython Objects/descrobject.c, methodobject.c, funcobject.c (PSF-2.0)
 *
 * Unit h: descriptor objects (method_descriptor, classmethod_descriptor,
 * getset_descriptor, member_descriptor, wrapper_descriptor, method-wrapper),
 * builtin_function_or_method, property, classmethod and staticmethod.
 * #included into quickjs.c (single translation unit).
 */

#include "typeobject_internal.h"

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

static inline BOOL qp_descr_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static inline int qp_descr_kwcount(JSValueConst kwnames)
{
    if (QP_IS_NULL(kwnames) || qp_descr_is_none(kwnames))
        return 0;
    return (int)QP_TUPLE_SIZE(kwnames);
}

/* Address shown by %p in reprs (CPython prints the object address). */
static void *qp_descr_value_ptr(JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v))
        return JS_VALUE_GET_PTR(v);
    return (void *)(uintptr_t)(((uint64_t)(uint32_t)JS_VALUE_GET_TAG(v) << 32) ^
                               (uint32_t)JS_VALUE_GET_INT(v));
}

/* Identity of a value as used by `is` (QP_NULL is CPython's NULL). */
static BOOL qp_descr_same(JSValueConst a, JSValueConst b)
{
    if (QP_IS_NULL(a) || QP_IS_NULL(b))
        return QP_IS_NULL(a) && QP_IS_NULL(b);
    return qp_is(a, b);
}

/* PyObject_GenericHash (identity hash); Py_HashPointer(NULL) = 0. */
static Py_hash_t qp_descr_identity_hash(JSContext *ctx, JSValueConst v)
{
    if (QP_IS_NULL(v))
        return 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT)
        return (Py_hash_t)qp_hash_pointer((uintptr_t)JS_VALUE_GET_PTR(v));
    /* unboxed values have no address: hash consistently with `is` */
    return qp_hash(ctx, v);
}

/* New str: tp_name of the type of v. */
static JSValue qp_descr_tp_name_of(JSContext *ctx, JSValueConst v)
{
    return qp_type_tp_name(ctx, qp_type_of(ctx, v));
}

/* _PyObject_IsAbstract: 1/0/-1 (v may be QP_NULL). */
static int qp_descr_is_abstract(JSContext *ctx, JSValueConst v)
{
    JSValue res;
    int r;

    if (QP_IS_NULL(v))
        return 0;
    r = qp_getattr_opt(ctx, v, QP_ATOM(ctx, __isabstractmethod__), &res);
    if (r <= 0)
        return r;
    r = qp_bool(ctx, res);
    JS_FreeValue(ctx, res);
    return r;
}

/* _PyEval_GetBuiltin("getattr") */
static JSValue qp_descr_builtin_getattr(JSContext *ctx)
{
    JSAtom atom = JS_NewAtom(ctx, "getattr");
    JSValue res;
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    res = qp_eval_get_builtin(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return res;
}

/* (getattr, (a, b)) used by the __reduce__ methods. */
static JSValue qp_descr_reduce_getattr(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue items[2], args;

    items[0] = qp_descr_builtin_getattr(ctx);
    if (JS_IsException(items[0]))
        return JS_EXCEPTION;
    args = qp_tuple_pack(ctx, 2, (JSValueConst[]){ a, b });
    if (JS_IsException(args)) {
        JS_FreeValue(ctx, items[0]);
        return JS_EXCEPTION;
    }
    items[1] = args;
    return qp_tuple_new_steal(ctx, 2, items);
}

/* ========================================================================= */
/* Internal doc helpers (typeobject.c find_signature & co.)                   */
/* ========================================================================= */

#define QP_SIGNATURE_END_MARKER         ")\n--\n\n"
#define QP_SIGNATURE_END_MARKER_LENGTH  6

static const char *qp_find_signature(const char *name, const char *doc)
{
    const char *dot;
    size_t length;

    if (!doc || !name)
        return NULL;
    /* for dotted names like classes, only use the last component */
    dot = strrchr(name, '.');
    if (dot)
        name = dot + 1;
    length = strlen(name);
    if (strncmp(doc, name, length))
        return NULL;
    doc += length;
    if (*doc != '(')
        return NULL;
    return doc;
}

static const char *qp_skip_signature(const char *doc)
{
    while (*doc) {
        if (*doc == *QP_SIGNATURE_END_MARKER &&
            !strncmp(doc, QP_SIGNATURE_END_MARKER, QP_SIGNATURE_END_MARKER_LENGTH))
            return doc + QP_SIGNATURE_END_MARKER_LENGTH;
        if (*doc == '\n' && doc[1] == '\n')
            return NULL;
        doc++;
    }
    return NULL;
}

static JSValue qp_doc_from_internal(JSContext *ctx, const char *name, const char *doc)
{
    const char *d = qp_find_signature(name, doc);

    if (d) {
        d = qp_skip_signature(d);
        if (!d)
            d = doc;
    } else {
        d = doc;
    }
    if (!d || *d == '\0')
        return JS_UNDEFINED;
    return qp_str_from_utf8(ctx, d, strlen(d));
}

static const char *qp_signature_from_flags(int flags)
{
    switch (flags & ~(QP_METH_COEXIST | QP_METH_VARARGS)) {
    case QP_METH_NOARGS:
        return "($self, /)";
    case QP_METH_NOARGS | QP_METH_CLASS:
        return "($type, /)";
    case QP_METH_NOARGS | QP_METH_STATIC:
        return "()";
    case QP_METH_O:
        return "($self, object, /)";
    case QP_METH_O | QP_METH_CLASS:
        return "($type, object, /)";
    case QP_METH_O | QP_METH_STATIC:
        return "(object, /)";
    default:
        return NULL;
    }
}

/* _PyType_GetTextSignatureFromInternalDoc with the method flags. */
static JSValue qp_text_signature_from_internal_flags(JSContext *ctx, const char *name,
                                                     const char *doc, int flags)
{
    const char *start = qp_find_signature(name, doc);
    const char *end;

    end = start ? qp_skip_signature(start) : NULL;
    if (!end) {
        start = qp_signature_from_flags(flags);
        if (start)
            return qp_str_from_cstr(ctx, start);
        return JS_UNDEFINED;
    }
    /* back "end" up until it points just past the final ')' */
    end -= QP_SIGNATURE_END_MARKER_LENGTH - 1;
    return qp_str_from_utf8(ctx, start, end - start);
}

static JSValue qp_text_signature_from_internal(JSContext *ctx, const char *name,
                                               const char *doc)
{
    return qp_text_signature_from_internal_flags(ctx, name, doc, 0);
}

/* ========================================================================= */
/* builtin_function_or_method (methodobject.c)                                */
/* ========================================================================= */

static QPCFunctionObject *qp_cfunction_payload(JSValueConst v)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_CFUNCTION)
        return NULL;
    return p->u.opaque;
}

static void qp_cfunction_finalizer(JSRuntime *rt, JSValue val)
{
    QPCFunctionObject *f = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!f)
        return;
    JS_FreeValueRT(rt, f->self);
    JS_FreeValueRT(rt, f->module);
    js_free_rt(rt, f);
}

static void qp_cfunction_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPCFunctionObject *f = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!f)
        return;
    JS_MarkValue(rt, f->self, mark_func);
    JS_MarkValue(rt, f->module, mark_func);
}

/* PyCMethod_New(ml, self, module, NULL): self QP_NULL = none, module
   QP_NULL = None. */
static JSValue qp_cfunction_new(JSContext *ctx, const QPMethodDef *ml, JSValueConst self,
                                JSValueConst module)
{
    QPCFunctionObject *f;
    JSValue obj;

    f = js_malloc(ctx, sizeof(*f));
    if (!f)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, builtin_function_or_method)),
                                 JS_CLASS_PY_CFUNCTION);
    if (JS_IsException(obj)) {
        js_free(ctx, f);
        return obj;
    }
    f->ml = ml;
    f->self = JS_DupValue(ctx, self);
    f->module = QP_IS_NULL(module) ? JS_UNDEFINED : JS_DupValue(ctx, module);
    JS_VALUE_GET_OBJ(obj)->u.opaque = f;
    return obj;
}

/* PyCFunction_GET_SELF: NULL for METH_STATIC entries. */
static JSValue qp_cfunction_get_self_raw(QPCFunctionObject *f)
{
    if (f->ml->flags & QP_METH_STATIC)
        return QP_NULL;
    return f->self;
}

/* meth_get__qualname__ logic, new str. */
static JSValue qp_cfunction_qualname_of(JSContext *ctx, const QPMethodDef *ml, JSValueConst self)
{
    QPType *t;
    JSValue tq, res;

    if (QP_IS_NULL(self) || qp_module_check(ctx, self))
        return qp_str_from_cstr(ctx, ml->name);
    t = qp_type_from_value(self);
    if (!t)
        t = qp_type_of(ctx, self);
    tq = qp_type_qualname(ctx, t);
    if (JS_IsException(tq))
        return tq;
    res = qp_str_from_format(ctx, "%S.%s", tq, ml->name);
    JS_FreeValue(ctx, tq);
    return res;
}

/* _PyObject_FunctionStr: "[module.]qualname()" (module omitted when it is
   None or 'builtins').  Consumes qualname. */
static JSValue qp_descr_funcstr_build(JSContext *ctx, JSValue qualname, JSValueConst module)
{
    JSValue res;

    if (JS_IsException(qualname))
        return qualname;
    if (!QP_IS_NULL(module) && !qp_descr_is_none(module)) {
        JSValue b = qp_str_from_cstr(ctx, "builtins");
        int ne;

        if (JS_IsException(b)) {
            JS_FreeValue(ctx, qualname);
            return JS_EXCEPTION;
        }
        ne = qp_richcompare_bool(ctx, module, b, QP_CMP_NE);
        JS_FreeValue(ctx, b);
        if (ne < 0) {
            JS_FreeValue(ctx, qualname);
            return JS_EXCEPTION;
        }
        if (ne) {
            res = qp_str_from_format(ctx, "%S.%S()", module, qualname);
            JS_FreeValue(ctx, qualname);
            return res;
        }
    }
    res = qp_str_from_format(ctx, "%S()", qualname);
    JS_FreeValue(ctx, qualname);
    return res;
}

/* Function string of a builtin_function_or_method object. */
static JSValue qp_cfunction_funcstr(JSContext *ctx, JSValueConst func)
{
    QPCFunctionObject *f = qp_cfunction_payload(func);

    return qp_descr_funcstr_build(ctx, qp_cfunction_qualname_of(ctx, f->ml, f->self), f->module);
}

/* Function string of a method_descriptor ("list.append()"). */
static JSValue qp_methoddescr_funcstr(JSContext *ctx, JSValueConst descr);

typedef JSValue QPDescrFuncStrFunc(JSContext *ctx, JSValueConst func);

/* Raise TypeError(fmt % (funcstr(func), n)). */
static JSValue qp_descr_raise_funcstr(JSContext *ctx, QPDescrFuncStrFunc *funcstr,
                                      JSValueConst func, const char *fmt, Py_ssize_t n)
{
    JSValue s = funcstr(ctx, func);

    if (JS_IsException(s))
        return s;
    qp_raise_type_error(ctx, fmt, s, n);
    JS_FreeValue(ctx, s);
    return JS_EXCEPTION;
}

/* Flag checks and call of a builtin C method (cfunction_vectorcall_* /
   method_vectorcall_*); `func` names the callable in error messages.
   args: nargs positionals then len(kwnames) keyword values. */
static JSValue qp_descr_call_cfunc(JSContext *ctx, const QPMethodDef *ml, JSValueConst self,
                                   JSValueConst *args, int nargs, JSValueConst kwnames,
                                   QPDescrFuncStrFunc *funcstr, JSValueConst func)
{
    int nkw = qp_descr_kwcount(kwnames);
    int flags = ml->flags;

    if (nkw && (!(flags & QP_METH_KEYWORDS) || (flags & (QP_METH_NOARGS | QP_METH_O))))
        return qp_descr_raise_funcstr(ctx, funcstr, func, "%U takes no keyword arguments", 0);
    if (flags & QP_METH_NOARGS) {
        if (nargs != 0)
            return qp_descr_raise_funcstr(ctx, funcstr, func,
                                          "%U takes no arguments (%zd given)", nargs);
        return ml->meth(ctx, self, args, 0, QP_NULL, ml->magic);
    }
    if (flags & QP_METH_O) {
        if (nargs != 1)
            return qp_descr_raise_funcstr(ctx, funcstr, func,
                                          "%U takes exactly one argument (%zd given)", nargs);
        return ml->meth(ctx, self, args, 1, QP_NULL, ml->magic);
    }
    return ml->meth(ctx, self, args, nargs, nkw ? kwnames : QP_NULL, ml->magic);
}

static JSValue qp_cfunction_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    QPCFunctionObject *f = qp_cfunction_payload(callable);
    JSValue self, res;

    if (!f)
        return qp_raise_system_error(ctx, "bad builtin function object");
    if ((f->ml->flags & QP_METH_VARARGS) && qp_descr_kwcount(kwnames))
        return qp_raise_type_error(ctx, "%.200s() takes no keyword arguments", f->ml->name);
    /* the call may drop the last reference to `callable` */
    self = JS_DupValue(ctx, f->self);
    res = qp_descr_call_cfunc(ctx, f->ml, self, args, (int)qp_nargs(nargsf), kwnames,
                              qp_cfunction_funcstr, callable);
    JS_FreeValue(ctx, self);
    return res;
}

static JSValue qp_cfunction_repr(JSContext *ctx, JSValueConst self)
{
    QPCFunctionObject *f = qp_cfunction_payload(self);
    JSValue tn, res;

    if (QP_IS_NULL(f->self) || qp_module_check(ctx, f->self))
        return qp_str_from_format(ctx, "<built-in function %s>", f->ml->name);
    tn = qp_descr_tp_name_of(ctx, f->self);
    if (JS_IsException(tn))
        return tn;
    res = qp_str_from_format(ctx, "<built-in method %s of %U object at %p>",
                             f->ml->name, tn, qp_descr_value_ptr(f->self));
    JS_FreeValue(ctx, tn);
    return res;
}

static JSValue qp_cfunction_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    QPCFunctionObject *fa, *fb;
    BOOL eq;

    op &= QP_CMP_OP_MASK;
    fa = qp_cfunction_payload(a);
    fb = qp_cfunction_payload(b);
    if ((op != QP_CMP_EQ && op != QP_CMP_NE) || !fa || !fb)
        return qp_not_implemented(ctx);
    /* CPython compares ml_meth; QP functions sharing one meth (C API
       methods: qp_capi_meth_call) differ by magic */
    eq = qp_descr_same(fa->self, fb->self) && fa->ml->meth == fb->ml->meth &&
         fa->ml->magic == fb->ml->magic;
    return qp_bool_new(op == QP_CMP_EQ ? eq : !eq);
}

static Py_hash_t qp_cfunction_hash(JSContext *ctx, JSValueConst self)
{
    QPCFunctionObject *f = qp_cfunction_payload(self);
    Py_hash_t x, y;

    x = qp_descr_identity_hash(ctx, f->self);
    if (x == -1)
        return -1;
    y = (Py_hash_t)qp_hash_pointer((uintptr_t)f->ml->meth ^ ((uintptr_t)(uint32_t)f->ml->magic << 32));
    x ^= y;
    if (x == -1)
        x = -2;
    return x;
}

static JSValue qp_cfunction_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPCFunctionObject *f = qp_cfunction_payload(self);
    JSValue name, res;

    if (QP_IS_NULL(f->self) || qp_module_check(ctx, f->self))
        return qp_str_from_cstr(ctx, f->ml->name);
    name = qp_str_from_cstr(ctx, f->ml->name);
    if (JS_IsException(name))
        return name;
    res = qp_descr_reduce_getattr(ctx, f->self, name);
    JS_FreeValue(ctx, name);
    return res;
}

/* magic: 0 __doc__, 1 __name__, 2 __qualname__, 3 __self__,
   4 __text_signature__, 5 __module__ */
static JSValue qp_cfunction_getattr(JSContext *ctx, JSValueConst self, int magic)
{
    QPCFunctionObject *f = qp_cfunction_payload(self);
    JSValue s;

    switch (magic) {
    case 0:
        return qp_doc_from_internal(ctx, f->ml->name, qp_capi_method_doc(f->ml));
    case 1:
        return qp_str_from_cstr(ctx, f->ml->name);
    case 2:
        return qp_cfunction_qualname_of(ctx, f->ml, f->self);
    case 3:
        s = qp_cfunction_get_self_raw(f);
        return QP_IS_NULL(s) ? JS_UNDEFINED : JS_DupValue(ctx, s);
    case 4:
        return qp_text_signature_from_internal_flags(ctx, f->ml->name, qp_capi_method_doc(f->ml),
                                                     f->ml->flags);
    default:
        return QP_IS_NULL(f->module) ? JS_UNDEFINED : JS_DupValue(ctx, f->module);
    }
}

static int qp_cfunction_set_module(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPCFunctionObject *f = qp_cfunction_payload(self);
    JSValue old = f->module;

    f->module = QP_IS_NULL(value) ? JS_UNDEFINED : JS_DupValue(ctx, value);
    JS_FreeValue(ctx, old);
    return 0;
}

static const QPMethodDef qp_cfunction_methods[] = {
    { "__reduce__", qp_cfunction_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPGetSetDef qp_cfunction_getsets[] = {
    { "__doc__", qp_cfunction_getattr, NULL, 0, NULL },
    { "__name__", qp_cfunction_getattr, NULL, 1, NULL },
    { "__qualname__", qp_cfunction_getattr, NULL, 2, NULL },
    { "__self__", qp_cfunction_getattr, NULL, 3, NULL },
    { "__text_signature__", qp_cfunction_getattr, NULL, 4, NULL },
    { "__module__", qp_cfunction_getattr, qp_cfunction_set_module, 5, NULL, QP_GETSET_MEMBER },
    { NULL }
};

/* ========================================================================= */
/* Descriptors (descrobject.c)                                                */
/* ========================================================================= */

static QPDescr *qp_descr_get(JSValueConst v)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_DESCR)
        return NULL;
    return p->u.opaque;
}

static void qp_descr_finalizer(JSRuntime *rt, JSValue val)
{
    QPDescr *d = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!d)
        return;
    JS_FreeValueRT(rt, d->objclass);
    JS_FreeValueRT(rt, d->self);
    if (d->name != JS_ATOM_NULL)
        JS_FreeAtomRT(rt, d->name);
    if (d->kind == QP_DESCR_MEMBER && d->u.member.private_atom != JS_ATOM_NULL)
        JS_FreeAtomRT(rt, d->u.member.private_atom);
    js_free_rt(rt, d);
}

static void qp_descr_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPDescr *d = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!d)
        return;
    JS_MarkValue(rt, d->objclass, mark_func);
    JS_MarkValue(rt, d->self, mark_func);
}

/* descr_new: object of type `descrtype` (consumes `name`). */
static JSValue qp_descr_alloc(JSContext *ctx, QPType *descrtype, int kind, JSValueConst objclass,
                              JSAtom name, QPDescr **pd)
{
    QPDescr *d;
    JSValue obj;

    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) {
        JS_FreeAtom(ctx, name);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(descrtype), JS_CLASS_PY_DESCR);
    if (JS_IsException(obj)) {
        JS_FreeAtom(ctx, name);
        js_free(ctx, d);
        return obj;
    }
    d->kind = kind;
    d->objclass = JS_DupValue(ctx, objclass);
    d->name = name;
    d->self = QP_NULL;
    JS_VALUE_GET_OBJ(obj)->u.opaque = d;
    *pd = d;
    return obj;
}

static QPType *qp_descr_objclass(QPDescr *d)
{
    return qp_type_from_value(d->objclass);
}

/* descr_check: TypeError unless obj is an instance of __objclass__. */
static int qp_descr_check(JSContext *ctx, QPDescr *d, JSValueConst obj)
{
    QPType *t = qp_descr_objclass(d);
    JSValue name, tn, on;

    if (qp_typecheck(ctx, obj, t))
        return 0;
    name = qp_str_from_atom(ctx, d->name);
    tn = qp_type_tp_name(ctx, t);
    on = qp_descr_tp_name_of(ctx, obj);
    if (!JS_IsException(name) && !JS_IsException(tn) && !JS_IsException(on))
        qp_raise_type_error(ctx, "descriptor '%U' for '%U' objects doesn't apply to a '%U' object",
                            name, tn, on);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, tn);
    JS_FreeValue(ctx, on);
    return -1;
}

/* Raise exc_type(fmt % (descr name, objclass tp_name)). */
static JSValue qp_descr_raise_name_type(JSContext *ctx, QPType *exc_type, QPDescr *d,
                                        const char *fmt)
{
    JSValue name = qp_str_from_atom(ctx, d->name);
    JSValue tn = qp_type_tp_name(ctx, qp_descr_objclass(d));

    if (!JS_IsException(name) && !JS_IsException(tn))
        qp_raise(ctx, exc_type, fmt, name, tn);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

static JSValue qp_methoddescr_funcstr(JSContext *ctx, JSValueConst descr)
{
    QPDescr *d = qp_descr_get(descr);
    JSValue tq, name, res;

    tq = qp_type_qualname(ctx, qp_descr_objclass(d));
    if (JS_IsException(tq))
        return tq;
    name = qp_str_from_atom(ctx, d->name);
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, tq);
        return name;
    }
    res = qp_str_from_format(ctx, "%S.%S()", tq, name);
    JS_FreeValue(ctx, tq);
    JS_FreeValue(ctx, name);
    return res;
}

/* ---- constructors -------------------------------------------------------- */

static JSValue qp_staticmethod_new(JSContext *ctx, JSValueConst callable);

static JSValue qp_descr_new_classmethod(JSContext *ctx, QPType *t, const QPMethodDef *ml)
{
    QPDescr *d;
    JSValue obj;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, classmethod_descriptor), QP_DESCR_CLASSMETHOD,
                         QP_TYPE_VALUE(t), JS_NewAtom(ctx, ml->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.method = ml;
    return obj;
}

static JSValue qp_descr_new_method(JSContext *ctx, QPType *t, const QPMethodDef *ml)
{
    QPDescr *d;
    JSValue obj;

    if (ml->flags & QP_METH_CLASS)
        return qp_descr_new_classmethod(ctx, t, ml);
    if (ml->flags & QP_METH_STATIC) {
        /* type_add_method: staticmethod(PyCFunction_NewEx(ml, type, module)) */
        JSValue module, cfunc, sm;

        module = qp_type_module(ctx, t);
        if (JS_IsException(module)) {
            /* a heap type without __module__ (PyType_FromSpec of an
               undotted name): no module */
            if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)))
                return module;
            JS_FreeValue(ctx, JS_GetException(ctx));
            module = JS_UNDEFINED;
        }
        cfunc = qp_cfunction_new(ctx, ml, QP_TYPE_VALUE(t), module);
        JS_FreeValue(ctx, module);
        if (JS_IsException(cfunc))
            return cfunc;
        sm = qp_staticmethod_new(ctx, cfunc);
        JS_FreeValue(ctx, cfunc);
        return sm;
    }
    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, method_descriptor), QP_DESCR_METHOD,
                         QP_TYPE_VALUE(t), JS_NewAtom(ctx, ml->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.method = ml;
    return obj;
}

static JSValue qp_descr_new_getset(JSContext *ctx, QPType *t, const QPGetSetDef *gs)
{
    QPDescr *d;
    JSValue obj;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, getset_descriptor), QP_DESCR_GETSET,
                         QP_TYPE_VALUE(t), JS_NewAtom(ctx, gs->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.getset = gs;
    return obj;
}

static JSValue qp_descr_new_member(JSContext *ctx, QPType *t, JSAtom name, uint16_t slot,
                                   uint8_t flags)
{
    QPDescr *d;
    JSValue obj, sym;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, member_descriptor), QP_DESCR_MEMBER,
                         QP_TYPE_VALUE(t), JS_DupAtom(ctx, name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.member.slot = slot;
    d->u.member.flags = flags;
    d->u.member.private_atom = JS_ATOM_NULL;
    sym = JS_NewSymbolFromAtom(ctx, name, JS_ATOM_TYPE_PRIVATE);
    if (JS_IsException(sym)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    /* the symbol's reference becomes the reference of the atom */
    d->u.member.private_atom = js_symbol_to_atom(ctx, sym);
    return obj;
}

/* D5 (CapiB) R2: member_descriptor over a C-API PyMemberDef wrapper. */
static JSValue qp_descr_new_member_getset(JSContext *ctx, QPType *t, const QPGetSetDef *gs)
{
    QPDescr *d;
    JSValue obj;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, member_descriptor), QP_DESCR_MEMBER,
                         QP_TYPE_VALUE(t), JS_NewAtom(ctx, gs->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.member.slot = 0;
    d->u.member.flags = gs->set ? 0 : QP_MEMBER_READONLY;
    d->u.member.private_atom = JS_ATOM_NULL;
    d->u.member.getset = gs;
    return obj;
}

static JSValue qp_descr_new_wrapper(JSContext *ctx, QPType *t, const QPSlotDef *sd,
                                    void *wrapped)
{
    QPDescr *d;
    JSValue obj;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, wrapper_descriptor), QP_DESCR_WRAPPER,
                         QP_TYPE_VALUE(t), JS_NewAtom(ctx, sd->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->u.wrapper.slot = sd;
    d->u.wrapper.wrapped = wrapped;
    return obj;
}

/* PyWrapper_New: method-wrapper binding a wrapper_descriptor to self. */
static JSValue qp_methodwrapper_new(JSContext *ctx, QPDescr *wd, JSValueConst self)
{
    QPDescr *d;
    JSValue obj;

    obj = qp_descr_alloc(ctx, QP_TYPE(ctx, method_wrapper), QP_DESCR_METHOD_WRAPPER,
                         wd->objclass, JS_DupAtom(ctx, wd->name), &d);
    if (JS_IsException(obj))
        return obj;
    d->self = JS_DupValue(ctx, self);
    d->u.wrapper = wd->u.wrapper;
    return obj;
}

/* ---- reprs --------------------------------------------------------------- */

static JSValue qp_descr_repr_kind(JSContext *ctx, JSValueConst self, const char *kind)
{
    QPDescr *d = qp_descr_get(self);
    QPType *t = qp_descr_objclass(d);
    JSValue name, tn, res;

    name = qp_str_from_atom(ctx, d->name);
    if (JS_IsException(name))
        return name;
    if (t == QP_TYPE(ctx, object)) {
        res = qp_str_from_format(ctx, "<%s '%U'>", kind, name);
        JS_FreeValue(ctx, name);
        return res;
    }
    tn = qp_type_tp_name(ctx, t);
    if (JS_IsException(tn)) {
        JS_FreeValue(ctx, name);
        return tn;
    }
    res = qp_str_from_format(ctx, "<%s '%U' of '%U' objects>", kind, name, tn);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, tn);
    return res;
}

static JSValue qp_methoddescr_repr(JSContext *ctx, JSValueConst self)
{
    return qp_descr_repr_kind(ctx, self, "method");
}

static JSValue qp_memberdescr_repr(JSContext *ctx, JSValueConst self)
{
    return qp_descr_repr_kind(ctx, self, "member");
}

static JSValue qp_getsetdescr_repr(JSContext *ctx, JSValueConst self)
{
    return qp_descr_repr_kind(ctx, self, "attribute");
}

static JSValue qp_wrapperdescr_repr(JSContext *ctx, JSValueConst self)
{
    return qp_descr_repr_kind(ctx, self, "slot wrapper");
}

static JSValue qp_methodwrapper_repr(JSContext *ctx, JSValueConst self)
{
    QPDescr *d = qp_descr_get(self);
    JSValue tn, res;

    tn = qp_descr_tp_name_of(ctx, d->self);
    if (JS_IsException(tn))
        return tn;
    res = qp_str_from_format(ctx, "<method-wrapper '%s' of %U object at %p>",
                             d->u.wrapper.slot->name, tn, qp_descr_value_ptr(d->self));
    JS_FreeValue(ctx, tn);
    return res;
}

/* ---- __get__ ------------------------------------------------------------- */

static JSValue qp_methoddescr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                  JSValueConst type)
{
    QPDescr *d = qp_descr_get(self);

    if (QP_IS_NULL(obj))
        return JS_DupValue(ctx, self);
    if (qp_descr_check(ctx, d, obj) < 0)
        return JS_EXCEPTION;
    return qp_cfunction_new(ctx, d->u.method, obj, QP_NULL);
}

static JSValue qp_classmethoddescr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                       JSValueConst type)
{
    QPDescr *d = qp_descr_get(self);
    QPType *t, *objclass = qp_descr_objclass(d);
    JSValue name, tn, an;

    /* Ensure a valid type.  Class methods ignore obj. */
    if (QP_IS_NULL(type)) {
        if (QP_IS_NULL(obj))
            return qp_descr_raise_name_type(ctx, QP_EXC(ctx, TypeError), d,
                                            "descriptor '%U' for type '%U' "
                                            "needs either an object or a type");
        type = QP_TYPE_VALUE(qp_type_of(ctx, obj));
    }
    t = qp_type_from_value(type);
    if (!t || !qp_type_is_subtype(t, objclass)) {
        name = qp_str_from_atom(ctx, d->name);
        tn = qp_type_tp_name(ctx, objclass);
        an = t ? qp_type_tp_name(ctx, t) : qp_descr_tp_name_of(ctx, type);
        if (!JS_IsException(name) && !JS_IsException(tn) && !JS_IsException(an)) {
            if (!t)
                qp_raise_type_error(ctx, "descriptor '%U' for type '%U' "
                                    "needs a type, not a '%U' as arg 2", name, tn, an);
            else
                qp_raise_type_error(ctx, "descriptor '%U' requires a subtype of '%U' "
                                    "but received '%U'", name, tn, an);
        }
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, an);
        return JS_EXCEPTION;
    }
    return qp_cfunction_new(ctx, d->u.method, type, QP_NULL);
}

/* member storage: QPInstance slot of PY_OBJECT instances, else a hidden
   own property keyed by the private atom.  Returns the slot pointer or
   NULL for the hidden-property path. */
static JSValue *qp_member_slot_ptr(JSValueConst obj, QPDescr *d)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);

    if (p->class_id == JS_CLASS_PY_OBJECT) {
        QPInstance *inst = p->u.opaque;
        if (inst && d->u.member.slot < inst->nslots)
            return &inst->slots[d->u.member.slot];
    }
    return NULL;
}

/* Borrowed value of the member, QP_NULL when unset. */
static JSValue qp_member_peek(JSValueConst obj, QPDescr *d)
{
    JSValue *slot;
    JSProperty *pr;
    JSShapeProperty *prs;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return QP_NULL;
    slot = qp_member_slot_ptr(obj, d);
    if (slot)
        return *slot;
    prs = find_own_property(&pr, JS_VALUE_GET_OBJ(obj), d->u.member.private_atom);
    if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL)
        return pr->u.value;
    return QP_NULL;
}

static JSValue qp_memberdescr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                  JSValueConst type)
{
    QPDescr *d = qp_descr_get(self);
    JSValue v, name;

    if (QP_IS_NULL(obj))
        return JS_DupValue(ctx, self);
    if (qp_descr_check(ctx, d, obj) < 0)
        return JS_EXCEPTION;
    if (d->u.member.getset)     /* D5 (CapiB) R2 */
        return d->u.member.getset->get(ctx, obj, d->u.member.getset->magic);
    v = qp_member_peek(obj, d);
    if (!QP_IS_NULL(v))
        return JS_DupValue(ctx, v);
    if (d->u.member.flags & QP_MEMBER_NONE_IF_NULL)
        return JS_UNDEFINED;
    name = qp_str_from_atom(ctx, d->name);
    if (JS_IsException(name))
        return name;
    qp_raise(ctx, QP_EXC(ctx, AttributeError), "'%T' object has no attribute '%U'", obj, name);
    JS_FreeValue(ctx, name);
    return JS_EXCEPTION;
}

static int qp_memberdescr_set(JSContext *ctx, JSValueConst self, JSValueConst obj,
                              JSValueConst value)
{
    QPDescr *d = qp_descr_get(self);
    JSValue *slot, name;
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;
    BOOL was_set;

    if (qp_descr_check(ctx, d, obj) < 0)
        return -1;
    if (d->u.member.flags & QP_MEMBER_READONLY) {
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "readonly attribute");
        return -1;
    }
    if (d->u.member.getset)     /* D5 (CapiB) R2 */
        return d->u.member.getset->set(ctx, obj, value, d->u.member.getset->magic);
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        qp_raise_attribute_error(ctx, obj, d->name);
        return -1;
    }
    p = JS_VALUE_GET_OBJ(obj);
    slot = qp_member_slot_ptr(obj, d);
    if (slot) {
        JSValue old = *slot;
        was_set = !QP_IS_NULL(old);
        *slot = QP_IS_NULL(value) ? QP_NULL : JS_DupValue(ctx, value);
        JS_FreeValue(ctx, old);
    } else {
        prs = find_own_property(&pr, p, d->u.member.private_atom);
        was_set = prs != NULL;
        if (QP_IS_NULL(value)) {
            if (prs && delete_property(ctx, p, d->u.member.private_atom) < 0)
                return -1;
        } else if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL) {
            set_value(ctx, &pr->u.value, JS_DupValue(ctx, value));
        } else {
            if (JS_DefinePropertyValue(ctx, obj, d->u.member.private_atom,
                                       JS_DupValue(ctx, value),
                                       JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
                return -1;
        }
    }
    if (QP_IS_NULL(value) && !was_set && !(d->u.member.flags & QP_MEMBER_NONE_IF_NULL)) {
        /* Py_T_OBJECT_EX: deleting an unset attribute */
        name = qp_str_from_atom(ctx, d->name);
        if (JS_IsException(name))
            return -1;
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "%U", name);
        JS_FreeValue(ctx, name);
        return -1;
    }
    return 0;
}

static JSValue qp_getsetdescr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                  JSValueConst type)
{
    QPDescr *d = qp_descr_get(self);

    if (QP_IS_NULL(obj))
        return JS_DupValue(ctx, self);
    if (qp_descr_check(ctx, d, obj) < 0)
        return JS_EXCEPTION;
    if (d->u.getset->get)
        return d->u.getset->get(ctx, obj, d->u.getset->magic);
    return qp_descr_raise_name_type(ctx, QP_EXC(ctx, AttributeError), d,
                                    "attribute '%U' of '%U' objects is not readable");
}

static int qp_getsetdescr_set(JSContext *ctx, JSValueConst self, JSValueConst obj,
                              JSValueConst value)
{
    QPDescr *d = qp_descr_get(self);

    if (qp_descr_check(ctx, d, obj) < 0)
        return -1;
    if (d->u.getset->set)
        return d->u.getset->set(ctx, obj, value, d->u.getset->magic);
    qp_descr_raise_name_type(ctx, QP_EXC(ctx, AttributeError), d,
                             "attribute '%U' of '%U' objects is not writable");
    return -1;
}

static JSValue qp_wrapperdescr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                   JSValueConst type)
{
    QPDescr *d = qp_descr_get(self);

    if (QP_IS_NULL(obj))
        return JS_DupValue(ctx, self);
    if (qp_descr_check(ctx, d, obj) < 0)
        return JS_EXCEPTION;
    return qp_methodwrapper_new(ctx, d, obj);
}

/* ---- calls --------------------------------------------------------------- */

static JSValue qp_methoddescr_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    QPDescr *d = qp_descr_get(callable);
    int nargs = (int)qp_nargs(nargsf);
    int nkw = qp_descr_kwcount(kwnames);

    /* method_check_args */
    if (nargs < 1)
        return qp_descr_raise_funcstr(ctx, qp_methoddescr_funcstr, callable,
                                      "unbound method %U needs an argument", 0);
    if (qp_descr_check(ctx, d, args[0]) < 0)
        return JS_EXCEPTION;
    if (nkw && !(d->u.method->flags & QP_METH_KEYWORDS))
        return qp_descr_raise_funcstr(ctx, qp_methoddescr_funcstr, callable,
                                      "%U takes no keyword arguments", 0);
    return qp_descr_call_cfunc(ctx, d->u.method, args[0], args + 1, nargs - 1, kwnames,
                               qp_methoddescr_funcstr, callable);
}

static JSValue qp_classmethoddescr_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                        size_t nargsf, JSValueConst kwnames)
{
    QPDescr *d = qp_descr_get(callable);
    int nargs = (int)qp_nargs(nargsf);
    JSValue bound, res;

    if (nargs < 1)
        return qp_descr_raise_name_type(ctx, QP_EXC(ctx, TypeError), d,
                                        "descriptor '%U' of '%U' object needs an argument");
    bound = qp_classmethoddescr_get(ctx, callable, QP_NULL, args[0]);
    if (JS_IsException(bound))
        return bound;
    res = qp_vectorcall(ctx, bound, args + 1, nargs - 1, kwnames);
    JS_FreeValue(ctx, bound);
    return res;
}

/* wrapperdescr_raw_call */
static JSValue qp_wrapper_raw_call(JSContext *ctx, QPDescr *d, JSValueConst self,
                                   JSValueConst *args, int nargs, JSValueConst kwnames)
{
    const QPSlotDef *sd = d->u.wrapper.slot;
    int nkw = qp_descr_kwcount(kwnames);
    QPState *s = QP_STATE(ctx);
    JSValue res;

    if (!(sd->flags & QP_SLOTDEF_KEYWORDS) && nkw)
        return qp_raise_type_error(ctx, "wrapper %s() takes no keyword arguments", sd->name);
    /* CPython calls the wrapped C function of the descriptor's type
       (d_wrapped); a C-defined type's wrapped slot is a forward bridge
       that otherwise resolves the C function from type(self) */
    s->wrapper_type = qp_descr_objclass(d);
    s->wrapper_slot = sd->offset;
    res = sd->wrapper(ctx, self, args, nargs, nkw ? kwnames : QP_NULL, d->u.wrapper.wrapped);
    s->wrapper_type = NULL;
    return res;
}

static JSValue qp_wrapperdescr_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    QPDescr *d = qp_descr_get(callable);
    QPType *objclass = qp_descr_objclass(d);
    int nargs = (int)qp_nargs(nargsf);
    JSValue name, tn, sn;

    if (nargs < 1)
        return qp_descr_raise_name_type(ctx, QP_EXC(ctx, TypeError), d,
                                        "descriptor '%U' of '%U' object needs an argument");
    if (!qp_type_is_subtype(qp_type_of(ctx, args[0]), objclass)) {
        name = qp_str_from_atom(ctx, d->name);
        tn = qp_type_tp_name(ctx, objclass);
        sn = qp_descr_tp_name_of(ctx, args[0]);
        if (!JS_IsException(name) && !JS_IsException(tn) && !JS_IsException(sn))
            qp_raise_type_error(ctx, "descriptor '%U' requires a '%U' object but received a '%U'",
                                name, tn, sn);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, sn);
        return JS_EXCEPTION;
    }
    return qp_wrapper_raw_call(ctx, d, args[0], args + 1, nargs - 1, kwnames);
}

static JSValue qp_methodwrapper_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                     size_t nargsf, JSValueConst kwnames)
{
    QPDescr *d = qp_descr_get(callable);
    JSValue self, res;

    self = JS_DupValue(ctx, d->self);
    res = qp_wrapper_raw_call(ctx, d, self, args, (int)qp_nargs(nargsf), kwnames);
    JS_FreeValue(ctx, self);
    return res;
}

/* ---- method-wrapper compare / hash ---------------------------------------- */

static JSValue qp_methodwrapper_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    QPDescr *da = qp_descr_get(a), *db = qp_descr_get(b);
    BOOL eq;

    op &= QP_CMP_OP_MASK;
    if ((op != QP_CMP_EQ && op != QP_CMP_NE) ||
        !da || da->kind != QP_DESCR_METHOD_WRAPPER ||
        !db || db->kind != QP_DESCR_METHOD_WRAPPER)
        return qp_not_implemented(ctx);
    eq = da->u.wrapper.slot == db->u.wrapper.slot &&
         da->u.wrapper.wrapped == db->u.wrapper.wrapped &&
         JS_VALUE_GET_PTR(da->objclass) == JS_VALUE_GET_PTR(db->objclass) &&
         qp_descr_same(da->self, db->self);
    return qp_bool_new(op == QP_CMP_EQ ? eq : !eq);
}

static Py_hash_t qp_methodwrapper_hash(JSContext *ctx, JSValueConst self)
{
    QPDescr *d = qp_descr_get(self);
    Py_hash_t x, y;

    x = qp_descr_identity_hash(ctx, d->self);
    if (x == -1)
        return -1;
    y = (Py_hash_t)qp_hash_pointer((uintptr_t)d->u.wrapper.slot ^
                                   (uintptr_t)d->u.wrapper.wrapped ^
                                   (uintptr_t)JS_VALUE_GET_PTR(d->objclass));
    x ^= y;
    if (x == -1)
        x = -2;
    return x;
}

/* ---- attributes of descriptor objects ----------------------------------- */

/* descr_get_qualname / calculate_qualname */
static JSValue qp_descr_qualname(JSContext *ctx, QPDescr *d)
{
    JSValue tq, name, res;

    tq = qp_type_qualname(ctx, qp_descr_objclass(d));
    if (JS_IsException(tq))
        return tq;
    name = qp_str_from_atom(ctx, d->name);
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, tq);
        return name;
    }
    res = qp_str_from_format(ctx, "%S.%S", tq, name);
    JS_FreeValue(ctx, tq);
    JS_FreeValue(ctx, name);
    return res;
}

enum {
    QP_DESCR_ATTR_OBJCLASS,
    QP_DESCR_ATTR_NAME,
    QP_DESCR_ATTR_QUALNAME,
    QP_DESCR_ATTR_DOC,
    QP_DESCR_ATTR_TEXT_SIGNATURE,
    QP_DESCR_ATTR_SELF,
};

static JSValue qp_descr_getattr(JSContext *ctx, JSValueConst self, int magic)
{
    QPDescr *d = qp_descr_get(self);
    const char *doc = NULL, *name = NULL;
    int flags = 0;

    switch (magic) {
    case QP_DESCR_ATTR_OBJCLASS:
        return JS_DupValue(ctx, d->objclass);
    case QP_DESCR_ATTR_NAME:
        return qp_str_from_atom(ctx, d->name);
    case QP_DESCR_ATTR_QUALNAME:
        return qp_descr_qualname(ctx, d);
    case QP_DESCR_ATTR_SELF:
        return QP_IS_NULL(d->self) ? JS_UNDEFINED : JS_DupValue(ctx, d->self);
    default:
        break;
    }
    switch (d->kind) {
    case QP_DESCR_METHOD:
    case QP_DESCR_CLASSMETHOD:
        name = d->u.method->name;
        doc = qp_capi_method_doc(d->u.method);
        flags = d->u.method->flags;
        break;
    case QP_DESCR_WRAPPER:
    case QP_DESCR_METHOD_WRAPPER:
        name = d->u.wrapper.slot->name;
        doc = d->u.wrapper.slot->doc;
        break;
    case QP_DESCR_GETSET:
        /* getset_get_doc: raw doc string */
        doc = qp_capi_getset_doc(d->u.getset);
        if (magic == QP_DESCR_ATTR_DOC && doc)
            return qp_str_from_cstr(ctx, doc);
        return JS_UNDEFINED;
    default:
        /* member_get_doc: members made by __slots__ have no doc; C-API
           members (D5 R2) have the PyMemberDef doc */
        doc = d->kind == QP_DESCR_MEMBER && d->u.member.getset ?
            qp_capi_getset_doc(d->u.member.getset) : NULL;
        if (magic == QP_DESCR_ATTR_DOC && doc)
            return qp_str_from_cstr(ctx, doc);
        return JS_UNDEFINED;
    }
    if (magic == QP_DESCR_ATTR_DOC)
        return qp_doc_from_internal(ctx, name, doc);
    return qp_text_signature_from_internal_flags(ctx, name, doc, flags);
}

/* descr_reduce: (getattr, (objclass, name)); wrapper_reduce: (getattr,
   (self, name)). */
static JSValue qp_descr_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPDescr *d = qp_descr_get(self);
    JSValue name, res;

    name = qp_str_from_atom(ctx, d->name);
    if (JS_IsException(name))
        return name;
    res = qp_descr_reduce_getattr(ctx, d->kind == QP_DESCR_METHOD_WRAPPER ? d->self : d->objclass,
                                  name);
    JS_FreeValue(ctx, name);
    return res;
}

static const QPMethodDef qp_descr_methods[] = {
    { "__reduce__", qp_descr_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

#define QP_DESCR_MEMBERS_GETSETS                                                       \
    { "__objclass__", qp_descr_getattr, NULL, QP_DESCR_ATTR_OBJCLASS, NULL, QP_GETSET_MEMBER }, \
    { "__name__", qp_descr_getattr, NULL, QP_DESCR_ATTR_NAME, NULL, QP_GETSET_MEMBER }

/* method_descriptor, classmethod_descriptor, wrapper_descriptor */
static const QPGetSetDef qp_methoddescr_getsets[] = {
    QP_DESCR_MEMBERS_GETSETS,
    { "__doc__", qp_descr_getattr, NULL, QP_DESCR_ATTR_DOC, NULL },
    { "__qualname__", qp_descr_getattr, NULL, QP_DESCR_ATTR_QUALNAME, NULL },
    { "__text_signature__", qp_descr_getattr, NULL, QP_DESCR_ATTR_TEXT_SIGNATURE, NULL },
    { NULL }
};

/* member_descriptor, getset_descriptor */
static const QPGetSetDef qp_attrdescr_getsets[] = {
    QP_DESCR_MEMBERS_GETSETS,
    { "__doc__", qp_descr_getattr, NULL, QP_DESCR_ATTR_DOC, NULL },
    { "__qualname__", qp_descr_getattr, NULL, QP_DESCR_ATTR_QUALNAME, NULL },
    { NULL }
};

static const QPGetSetDef qp_methodwrapper_getsets[] = {
    { "__self__", qp_descr_getattr, NULL, QP_DESCR_ATTR_SELF, NULL, QP_GETSET_MEMBER },
    { "__objclass__", qp_descr_getattr, NULL, QP_DESCR_ATTR_OBJCLASS, NULL },
    { "__name__", qp_descr_getattr, NULL, QP_DESCR_ATTR_NAME, NULL },
    { "__qualname__", qp_descr_getattr, NULL, QP_DESCR_ATTR_QUALNAME, NULL },
    { "__doc__", qp_descr_getattr, NULL, QP_DESCR_ATTR_DOC, NULL },
    { "__text_signature__", qp_descr_getattr, NULL, QP_DESCR_ATTR_TEXT_SIGNATURE, NULL },
    { NULL }
};

#undef QP_DESCR_MEMBERS_GETSETS

/* ========================================================================= */
/* property (descrobject.c)                                                   */
/* ========================================================================= */

typedef struct QPProperty {
    JSValue fget, fset, fdel;   /* QP_NULL = absent */
    JSValue doc;                /* QP_NULL = absent (reads as None) */
    JSValue name;               /* set by __set_name__ / __name__ setter, QP_NULL = none */
    int getter_doc;
} QPProperty;

static QPProperty *qp_property_alloc_payload(JSContext *ctx)
{
    QPProperty *pp = js_malloc(ctx, sizeof(*pp));

    if (!pp)
        return NULL;
    pp->fget = QP_NULL;
    pp->fset = QP_NULL;
    pp->fdel = QP_NULL;
    pp->doc = QP_NULL;
    pp->name = QP_NULL;
    pp->getter_doc = 0;
    return pp;
}

/* Payload of a property (or subclass) instance; created empty when the
   object was allocated without one (like CPython's zeroed allocation). */
static QPProperty *qp_property_payload(JSContext *ctx, JSValueConst self)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(self)->class_id != JS_CLASS_PY_PROPERTY) {
        qp_raise_system_error(ctx, "bad property object");
        return NULL;
    }
    p = JS_VALUE_GET_OBJ(self);
    if (!p->u.opaque)
        p->u.opaque = qp_property_alloc_payload(ctx);
    return p->u.opaque;
}

static void qp_property_finalizer(JSRuntime *rt, JSValue val)
{
    QPProperty *pp = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!pp)
        return;
    JS_FreeValueRT(rt, pp->fget);
    JS_FreeValueRT(rt, pp->fset);
    JS_FreeValueRT(rt, pp->fdel);
    JS_FreeValueRT(rt, pp->doc);
    JS_FreeValueRT(rt, pp->name);
    js_free_rt(rt, pp);
}

static void qp_property_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPProperty *pp = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!pp)
        return;
    JS_MarkValue(rt, pp->fget, mark_func);
    JS_MarkValue(rt, pp->fset, mark_func);
    JS_MarkValue(rt, pp->fdel, mark_func);
    JS_MarkValue(rt, pp->doc, mark_func);
    JS_MarkValue(rt, pp->name, mark_func);
}

static void qp_property_setref(JSContext *ctx, JSValue *field, JSValueConst v)
{
    JSValue old = *field;

    *field = JS_DupValue(ctx, v);
    JS_FreeValue(ctx, old);
}

/* property_name: 1 *pname set, 0 none (*pname = QP_NULL), -1 error. */
static int qp_property_name(JSContext *ctx, QPProperty *pp, JSValue *pname)
{
    JSValue fget;
    int r;

    if (!QP_IS_NULL(pp->name)) {
        *pname = JS_DupValue(ctx, pp->name);
        return 1;
    }
    if (QP_IS_NULL(pp->fget)) {
        *pname = QP_NULL;
        return 0;
    }
    fget = JS_DupValue(ctx, pp->fget);
    r = qp_getattr_opt(ctx, fget, QP_ATOM(ctx, __name__), pname);
    JS_FreeValue(ctx, fget);
    return r;
}

/* AttributeError "property %R of %R object has no <what>" and variants. */
static void qp_property_raise_missing(JSContext *ctx, QPProperty *pp, JSValueConst obj,
                                      const char *what)
{
    JSValue propname, qualname = QP_NULL;

    if (qp_property_name(ctx, pp, &propname) < 0)
        return;
    if (!QP_IS_NULL(obj)) {
        qualname = qp_type_qualname(ctx, qp_type_of(ctx, obj));
        if (JS_IsException(qualname)) {
            JS_FreeValue(ctx, propname);
            return;
        }
    }
    if (!QP_IS_NULL(propname) && !QP_IS_NULL(qualname))
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "property %R of %R object has no %s",
                 propname, qualname, what);
    else if (!QP_IS_NULL(qualname))
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "property of %R object has no %s",
                 qualname, what);
    else
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "property has no %s", what);
    JS_FreeValue(ctx, propname);
    JS_FreeValue(ctx, qualname);
}

static JSValue qp_property_descr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                     JSValueConst type)
{
    QPProperty *pp;
    JSValue fget, res;

    if (QP_IS_NULL(obj) || qp_descr_is_none(obj))
        return JS_DupValue(ctx, self);
    pp = qp_property_payload(ctx, self);
    if (!pp)
        return JS_EXCEPTION;
    if (QP_IS_NULL(pp->fget)) {
        qp_property_raise_missing(ctx, pp, obj, "getter");
        return JS_EXCEPTION;
    }
    fget = JS_DupValue(ctx, pp->fget);
    res = qp_call1(ctx, fget, obj);
    JS_FreeValue(ctx, fget);
    return res;
}

static int qp_property_descr_set(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                 JSValueConst value)
{
    QPProperty *pp = qp_property_payload(ctx, self);
    JSValue func, res;

    if (!pp)
        return -1;
    func = QP_IS_NULL(value) ? pp->fdel : pp->fset;
    if (QP_IS_NULL(func)) {
        qp_property_raise_missing(ctx, pp, obj, QP_IS_NULL(value) ? "deleter" : "setter");
        return -1;
    }
    func = JS_DupValue(ctx, func);
    if (QP_IS_NULL(value))
        res = qp_call1(ctx, func, obj);
    else
        res = qp_call2(ctx, func, obj, value);
    JS_FreeValue(ctx, func);
    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
    return 0;
}

/* property_copy: type(old)(fget, fset, fdel, doc) keeping the name.
   get/set/del QP_NULL = keep the old one. */
static JSValue qp_property_copy(JSContext *ctx, JSValueConst old, JSValueConst get,
                                JSValueConst set, JSValueConst del)
{
    QPProperty *pold = qp_property_payload(ctx, old), *pnew;
    JSValue argv[4], name, res;
    JSObject *p;
    int i;

    if (!pold)
        return JS_EXCEPTION;
    if (QP_IS_NULL(get) || qp_descr_is_none(get))
        get = QP_IS_NULL(pold->fget) ? JS_UNDEFINED : pold->fget;
    if (QP_IS_NULL(set) || qp_descr_is_none(set))
        set = QP_IS_NULL(pold->fset) ? JS_UNDEFINED : pold->fset;
    if (QP_IS_NULL(del) || qp_descr_is_none(del))
        del = QP_IS_NULL(pold->fdel) ? JS_UNDEFINED : pold->fdel;
    argv[0] = JS_DupValue(ctx, get);
    argv[1] = JS_DupValue(ctx, set);
    argv[2] = JS_DupValue(ctx, del);
    if (pold->getter_doc && !qp_descr_is_none(get))
        argv[3] = JS_UNDEFINED; /* make __init__ use __doc__ from getter */
    else
        argv[3] = QP_IS_NULL(pold->doc) ? JS_UNDEFINED : JS_DupValue(ctx, pold->doc);
    name = JS_DupValue(ctx, pold->name);
    res = qp_vectorcall(ctx, QP_TYPE_VALUE(qp_type_of(ctx, old)), (JSValueConst *)argv, 4, QP_NULL);
    for (i = 0; i < 4; i++)
        JS_FreeValue(ctx, argv[i]);
    if (!JS_IsException(res) && qp_typecheck(ctx, res, QP_TYPE(ctx, property)) &&
        JS_VALUE_GET_TAG(res) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(res);
        if (p->class_id == JS_CLASS_PY_PROPERTY) {
            pnew = qp_property_payload(ctx, res);
            if (!pnew) {
                JS_FreeValue(ctx, name);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            qp_property_setref(ctx, &pnew->name, name);
        }
    }
    JS_FreeValue(ctx, name);
    return res;
}

/* getter / setter / deleter (magic 0 / 1 / 2) */
static JSValue qp_property_accessor(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_property_copy(ctx, self, magic == 0 ? args[0] : QP_NULL,
                            magic == 1 ? args[0] : QP_NULL, magic == 2 ? args[0] : QP_NULL);
}

static JSValue qp_property_set_name(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPProperty *pp;

    if (nargs != 2)
        return qp_raise_type_error(ctx, "__set_name__() takes 2 positional arguments but %zd "
                                   "were given", (Py_ssize_t)nargs);
    pp = qp_property_payload(ctx, self);
    if (!pp)
        return JS_EXCEPTION;
    qp_property_setref(ctx, &pp->name, args[1]);
    return JS_UNDEFINED;
}

/* magic: 0 fget, 1 fset, 2 fdel, 3 __doc__, 4 __name__,
   5 __isabstractmethod__ */
static JSValue qp_property_getattr(JSContext *ctx, JSValueConst self, int magic)
{
    QPProperty *pp = qp_property_payload(ctx, self);
    JSValue v, name;
    int r;

    if (!pp)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        v = pp->fget;
        break;
    case 1:
        v = pp->fset;
        break;
    case 2:
        v = pp->fdel;
        break;
    case 3:
        v = pp->doc;
        break;
    case 4:
        r = qp_property_name(ctx, pp, &name);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0)
            return qp_raise(ctx, QP_EXC(ctx, AttributeError),
                            "'property' object has no attribute '__name__'");
        return name;
    default:
        for (magic = 0; magic < 3; magic++) {
            /* the getter may change the fields: re-read them */
            v = magic == 0 ? pp->fget : magic == 1 ? pp->fset : pp->fdel;
            v = JS_DupValue(ctx, v);
            r = qp_descr_is_abstract(ctx, v);
            JS_FreeValue(ctx, v);
            if (r < 0)
                return JS_EXCEPTION;
            if (r)
                return JS_TRUE;
        }
        return JS_FALSE;
    }
    return QP_IS_NULL(v) ? JS_UNDEFINED : JS_DupValue(ctx, v);
}

/* __doc__ (magic 3) and __name__ (magic 4) setters; QP_NULL deletes. */
static int qp_property_setattr(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPProperty *pp = qp_property_payload(ctx, self);

    if (!pp)
        return -1;
    qp_property_setref(ctx, magic == 3 ? &pp->doc : &pp->name, value);
    return 0;
}

static JSValue qp_property_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    QPProperty *pp;
    JSValue obj;

    pp = qp_property_alloc_payload(ctx);
    if (!pp)
        return JS_EXCEPTION;
    obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_PROPERTY);
    if (JS_IsException(obj)) {
        js_free(ctx, pp);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = pp;
    return obj;
}

static int qp_property_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            size_t nargsf, JSValueConst kwnames)
{
    static const char *const keywords[] = { "fget", "fset", "fdel", "doc", NULL };
    static const QPArgSpec spec = { "property", keywords, 0, 4, 0 };
    JSValueConst out[4];
    JSValueConst fget, fset, fdel, doc;
    JSValue prop_doc = QP_NULL;
    QPProperty *pp;
    int r, err;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, out) < 0)
        return -1;
    pp = qp_property_payload(ctx, self);
    if (!pp)
        return -1;
    fget = out[0];
    fset = out[1];
    fdel = out[2];
    doc = out[3];
    if (qp_descr_is_none(fget))
        fget = QP_NULL;
    if (qp_descr_is_none(fset))
        fset = QP_NULL;
    if (qp_descr_is_none(fdel))
        fdel = QP_NULL;

    qp_property_setref(ctx, &pp->fget, fget);
    qp_property_setref(ctx, &pp->fset, fset);
    qp_property_setref(ctx, &pp->fdel, fdel);
    qp_property_setref(ctx, &pp->doc, QP_NULL);
    qp_property_setref(ctx, &pp->name, QP_NULL);
    pp->getter_doc = 0;

    if (!QP_IS_NULL(doc) && !qp_descr_is_none(doc)) {
        prop_doc = JS_DupValue(ctx, doc);
    } else if (!QP_IS_NULL(fget)) {
        /* if no docstring given and the getter has one, use that one */
        r = qp_getattr_opt(ctx, fget, QP_ATOM(ctx, __doc__), &prop_doc);
        if (r < 0)
            return -1;
        if (qp_descr_is_none(prop_doc)) {
            JS_FreeValue(ctx, prop_doc);
            prop_doc = QP_NULL;
        }
        pp = qp_property_payload(ctx, self);
        if (!pp) {
            JS_FreeValue(ctx, prop_doc);
            return -1;
        }
        if (!QP_IS_NULL(prop_doc))
            pp->getter_doc = 1;
    }

    if (qp_type_of(ctx, self) == QP_TYPE(ctx, property)) {
        JSValue old = pp->doc;
        pp->doc = prop_doc;
        JS_FreeValue(ctx, old);
        return 0;
    }
    /* property subclass: put __doc__ in the instance dict, otherwise it
       gets shadowed by __doc__ in the class's dict */
    if (QP_IS_NULL(prop_doc))
        prop_doc = JS_UNDEFINED;
    err = qp_setattr(ctx, self, QP_ATOM(ctx, __doc__), prop_doc);
    JS_FreeValue(ctx, prop_doc);
    if (err < 0) {
        pp = qp_property_payload(ctx, self);
        if (pp && !pp->getter_doc && qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
            /* Python silently dropped this doc assignment through 3.11 */
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return 0;
        }
        return -1;
    }
    return 0;
}

static const QPMethodDef qp_property_methods[] = {
    { "getter", qp_property_accessor, QP_METH_O, 0,
      "Descriptor to obtain a copy of the property with a different getter." },
    { "setter", qp_property_accessor, QP_METH_O, 1,
      "Descriptor to obtain a copy of the property with a different setter." },
    { "deleter", qp_property_accessor, QP_METH_O, 2,
      "Descriptor to obtain a copy of the property with a different deleter." },
    { "__set_name__", qp_property_set_name, QP_METH_FASTCALL, 0,
      "__set_name__($self, owner, name, /)\n--\n\nMethod to set name of a property." },
    { NULL }
};

static const QPGetSetDef qp_property_getsets[] = {
    /* property_members */
    { "fget", qp_property_getattr, NULL, 0, NULL, QP_GETSET_MEMBER },
    { "fset", qp_property_getattr, NULL, 1, NULL, QP_GETSET_MEMBER },
    { "fdel", qp_property_getattr, NULL, 2, NULL, QP_GETSET_MEMBER },
    { "__doc__", qp_property_getattr, qp_property_setattr, 3, NULL, QP_GETSET_MEMBER },
    { "__name__", qp_property_getattr, qp_property_setattr, 4, NULL },
    { "__isabstractmethod__", qp_property_getattr, NULL, 5, NULL },
    { NULL }
};

static const char qp_property_doc[] =
    "property(fget=None, fset=None, fdel=None, doc=None)\n"
    "--\n"
    "\n"
    "Property attribute.\n"
    "\n"
    "  fget\n"
    "    function to be used for getting an attribute value\n"
    "  fset\n"
    "    function to be used for setting an attribute value\n"
    "  fdel\n"
    "    function to be used for del\'ing an attribute\n"
    "  doc\n"
    "    docstring\n"
    "\n"
    "Typical use is to define a managed attribute x:\n"
    "\n"
    "class C(object):\n"
    "    def getx(self): return self._x\n"
    "    def setx(self, value): self._x = value\n"
    "    def delx(self): del self._x\n"
    "    x = property(getx, setx, delx, \"I\'m the \'x\' property.\")\n"
    "\n"
    "Decorators make defining new properties or modifying existing ones easy:\n"
    "\n"
    "class C(object):\n"
    "    @property\n"
    "    def x(self):\n"
    "        \"I am the \'x\' property.\"\n"
    "        return self._x\n"
    "    @x.setter\n"
    "    def x(self, value):\n"
    "        self._x = value\n"
    "    @x.deleter\n"
    "    def x(self):\n"
    "        del self._x";

/* ========================================================================= */
/* classmethod / staticmethod (funcobject.c)                                  */
/* ========================================================================= */

static BOOL qp_funcwrap_class_ok(JSValueConst v)
{
    int cid;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    cid = JS_VALUE_GET_OBJ(v)->class_id;
    return cid == JS_CLASS_PY_CLASSMETHOD || cid == JS_CLASS_PY_STATICMETHOD;
}

static QPFuncWrap *qp_funcwrap_alloc_payload(JSContext *ctx)
{
    QPFuncWrap *fw = js_malloc(ctx, sizeof(*fw));

    if (!fw)
        return NULL;
    fw->callable = QP_NULL;
    fw->dict = QP_NULL;
    return fw;
}

/* Payload of a classmethod/staticmethod (or subclass) instance, created
   empty when missing. */
static QPFuncWrap *qp_funcwrap_payload(JSContext *ctx, JSValueConst v)
{
    JSObject *p;

    if (!qp_funcwrap_class_ok(v)) {
        qp_raise_system_error(ctx, "bad classmethod/staticmethod object");
        return NULL;
    }
    p = JS_VALUE_GET_OBJ(v);
    if (!p->u.opaque)
        p->u.opaque = qp_funcwrap_alloc_payload(ctx);
    return p->u.opaque;
}

static BOOL qp_funcwrap_is_static(JSValueConst v)
{
    return JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_STATICMETHOD;
}

static void qp_funcwrap_finalizer(JSRuntime *rt, JSValue val)
{
    QPFuncWrap *fw = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!fw)
        return;
    JS_FreeValueRT(rt, fw->callable);
    JS_FreeValueRT(rt, fw->dict);
    js_free_rt(rt, fw);
}

static void qp_funcwrap_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPFuncWrap *fw = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!fw)
        return;
    JS_MarkValue(rt, fw->callable, mark_func);
    JS_MarkValue(rt, fw->dict, mark_func);
}

/* The instance dict (created on demand), borrowed. */
static JSValue qp_funcwrap_dict(JSContext *ctx, QPFuncWrap *fw)
{
    if (QP_IS_NULL(fw->dict)) {
        JSValue d = qp_dict_new(ctx);
        if (JS_IsException(d))
            return d;
        fw->dict = d;
    }
    return fw->dict;
}

/* functools_wraps: copy __module__, __name__, __qualname__, __doc__. */
static int qp_funcwrap_wraps(JSContext *ctx, JSValueConst wrapper, JSValueConst wrapped)
{
    static const int names[] = {
        QP_ATOM___module__, QP_ATOM___name__, QP_ATOM___qualname__, QP_ATOM___doc__,
    };
    QPType *exact = qp_funcwrap_is_static(wrapper) ? QP_TYPE(ctx, staticmethod)
                                                   : QP_TYPE(ctx, classmethod);
    BOOL direct = qp_type_of(ctx, wrapper) == exact;
    JSValue value, dict;
    QPFuncWrap *fw;
    size_t i;
    int r;

    for (i = 0; i < countof(names); i++) {
        JSAtom name = QP_STATE(ctx)->atoms[names[i]];

        r = qp_getattr_opt(ctx, wrapped, name, &value);
        if (r < 0)
            return -1;
        if (r == 0)
            continue;
        if (direct) {
            /* exact type: no data descriptor of these names, setattr
               stores into the instance dict */
            fw = qp_funcwrap_payload(ctx, wrapper);
            dict = fw ? qp_funcwrap_dict(ctx, fw) : JS_EXCEPTION;
            r = JS_IsException(dict) ? -1 : qp_dict_set_item_atom(ctx, dict, name, value);
        } else {
            r = qp_setattr(ctx, wrapper, name, value);
        }
        JS_FreeValue(ctx, value);
        if (r < 0)
            return -1;
    }
    return 0;
}

/* cm_set_callable / sm_set_callable */
static int qp_funcwrap_set_callable(JSContext *ctx, JSValueConst self, JSValueConst callable)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);
    JSValue old;

    if (!fw)
        return -1;
    if (!QP_IS_NULL(fw->callable) && qp_is(fw->callable, callable))
        return 0; /* __init__ sets the same callable as __new__ */
    old = fw->callable;
    fw->callable = JS_DupValue(ctx, callable);
    JS_FreeValue(ctx, old);
    return qp_funcwrap_wraps(ctx, self, callable);
}

static JSValue qp_funcwrap_create(JSContext *ctx, QPType *t, int class_id, JSValueConst callable)
{
    QPFuncWrap *fw;
    JSValue obj;

    fw = qp_funcwrap_alloc_payload(ctx);
    if (!fw)
        return JS_EXCEPTION;
    if (t == QP_TYPE(ctx, classmethod) || t == QP_TYPE(ctx, staticmethod))
        obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), class_id);
    else
        obj = qp_object_new_instance(ctx, t, class_id);
    if (JS_IsException(obj)) {
        js_free(ctx, fw);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = fw;
    if (qp_funcwrap_set_callable(ctx, obj, callable) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue qp_classmethod_new(JSContext *ctx, JSValueConst callable)
{
    return qp_funcwrap_create(ctx, QP_TYPE(ctx, classmethod), JS_CLASS_PY_CLASSMETHOD, callable);
}

static JSValue qp_staticmethod_new(JSContext *ctx, JSValueConst callable)
{
    return qp_funcwrap_create(ctx, QP_TYPE(ctx, staticmethod), JS_CLASS_PY_STATICMETHOD, callable);
}

/* Argument checks of cm_new/cm_init/sm_new/sm_init. */
static int qp_funcwrap_parse(JSContext *ctx, const char *fname, size_t nargsf,
                             JSValueConst kwnames)
{
    if (qp_arg_no_keywords(ctx, fname, kwnames) < 0)
        return -1;
    return qp_arg_check_positional(ctx, fname, (int)qp_nargs(nargsf), 1, 1);
}

static JSValue qp_classmethod_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                     size_t nargsf, JSValueConst kwnames)
{
    if (qp_funcwrap_parse(ctx, "classmethod", nargsf, kwnames) < 0)
        return JS_EXCEPTION;
    return qp_funcwrap_create(ctx, subtype, JS_CLASS_PY_CLASSMETHOD, args[0]);
}

static JSValue qp_staticmethod_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                      size_t nargsf, JSValueConst kwnames)
{
    if (qp_funcwrap_parse(ctx, "staticmethod", nargsf, kwnames) < 0)
        return JS_EXCEPTION;
    return qp_funcwrap_create(ctx, subtype, JS_CLASS_PY_STATICMETHOD, args[0]);
}

static int qp_classmethod_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    if (qp_funcwrap_parse(ctx, "classmethod", nargsf, kwnames) < 0)
        return -1;
    return qp_funcwrap_set_callable(ctx, self, args[0]);
}

static int qp_staticmethod_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    if (qp_funcwrap_parse(ctx, "staticmethod", nargsf, kwnames) < 0)
        return -1;
    return qp_funcwrap_set_callable(ctx, self, args[0]);
}

static JSValue qp_classmethod_descr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                        JSValueConst type)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);

    if (!fw)
        return JS_EXCEPTION;
    if (QP_IS_NULL(fw->callable))
        return qp_raise_runtime_error(ctx, "uninitialized classmethod object");
    if (QP_IS_NULL(type)) {
        if (QP_IS_NULL(obj))
            return qp_raise_type_error(ctx, "__get__(None, None) is invalid");
        type = QP_TYPE_VALUE(qp_type_of(ctx, obj));
    }
    return qp_method_new(ctx, fw->callable, type);
}

static JSValue qp_staticmethod_descr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                         JSValueConst type)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);

    if (!fw)
        return JS_EXCEPTION;
    if (QP_IS_NULL(fw->callable))
        return qp_raise_runtime_error(ctx, "uninitialized staticmethod object");
    return JS_DupValue(ctx, fw->callable);
}

static JSValue qp_staticmethod_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, callable);
    JSValue func, res;

    if (!fw)
        return JS_EXCEPTION;
    if (QP_IS_NULL(fw->callable))
        return qp_raise_runtime_error(ctx, "uninitialized staticmethod object");
    func = JS_DupValue(ctx, fw->callable);
    res = qp_vectorcall(ctx, func, args, nargsf, kwnames);
    JS_FreeValue(ctx, func);
    return res;
}

static JSValue qp_funcwrap_repr(JSContext *ctx, JSValueConst self)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);
    const char *fmt = qp_funcwrap_is_static(self) ? "<staticmethod(%R)>" : "<classmethod(%R)>";

    if (!fw)
        return JS_EXCEPTION;
    if (QP_IS_NULL(fw->callable))
        return qp_str_from_format(ctx, qp_funcwrap_is_static(self) ? "<staticmethod(<NULL>)>"
                                                                   : "<classmethod(<NULL>)>");
    return qp_str_from_format(ctx, fmt, fw->callable);
}

/* PyObject_GenericGetAttr with the instance dict in QPFuncWrap.dict. */
static JSValue qp_funcwrap_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPType *tp = qp_type_of(ctx, obj), *dt;
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, obj);
    QPDescrGetFunc *f = NULL;
    JSValue descr, dict, res;
    int r;

    if (!fw)
        return JS_EXCEPTION;
    descr = JS_DupValue(ctx, qp_type_lookup(ctx, tp, name));
    if (!QP_IS_NULL(descr)) {
        dt = qp_type_of(ctx, descr);
        f = dt->slots.tp_descr_get;
        if (f && dt->slots.tp_descr_set) {
            res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
            JS_FreeValue(ctx, descr);
            return res;
        }
    }
    if (!QP_IS_NULL(fw->dict)) {
        dict = JS_DupValue(ctx, fw->dict);
        r = qp_dict_get_item_atom(ctx, dict, name, &res);
        JS_FreeValue(ctx, dict);
        if (r != 0) {
            JS_FreeValue(ctx, descr);
            return r > 0 ? res : JS_EXCEPTION;
        }
    }
    if (f) {
        res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, descr);
        return res;
    }
    if (!QP_IS_NULL(descr))
        return descr;
    return qp_raise_attribute_error(ctx, obj, name);
}

/* PyObject_GenericSetAttr with the instance dict in QPFuncWrap.dict. */
static int qp_funcwrap_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPType *tp = qp_type_of(ctx, obj), *dt;
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, obj);
    JSValue descr, dict;
    int r;

    if (!fw)
        return -1;
    descr = qp_type_lookup(ctx, tp, name);
    if (!QP_IS_NULL(descr)) {
        dt = qp_type_of(ctx, descr);
        if (dt->slots.tp_descr_set) {
            descr = JS_DupValue(ctx, descr);
            r = dt->slots.tp_descr_set(ctx, descr, obj, v);
            JS_FreeValue(ctx, descr);
            return r;
        }
    }
    if (QP_IS_NULL(v)) {
        if (QP_IS_NULL(fw->dict)) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        dict = JS_DupValue(ctx, fw->dict);
        r = qp_dict_del_item_atom(ctx, dict, name);
        JS_FreeValue(ctx, dict);
        if (r == 0) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        return r < 0 ? -1 : 0;
    }
    dict = qp_funcwrap_dict(ctx, fw);
    if (JS_IsException(dict))
        return -1;
    dict = JS_DupValue(ctx, dict);
    r = qp_dict_set_item_atom(ctx, dict, name, v);
    JS_FreeValue(ctx, dict);
    return r;
}

/* magic: 0 __func__ / __wrapped__, 1 __isabstractmethod__, 2 __dict__,
   3 __annotations__, 4 __annotate__ */
static JSValue qp_funcwrap_getattr(JSContext *ctx, JSValueConst self, int magic)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);
    JSValue dict, res, callable;
    JSAtom name;
    int r;

    if (!fw)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return QP_IS_NULL(fw->callable) ? JS_UNDEFINED : JS_DupValue(ctx, fw->callable);
    case 1:
        callable = JS_DupValue(ctx, fw->callable);
        r = qp_descr_is_abstract(ctx, callable);
        JS_FreeValue(ctx, callable);
        if (r < 0)
            return JS_EXCEPTION;
        return qp_bool_new(r);
    case 2:
        dict = qp_funcwrap_dict(ctx, fw);
        return JS_IsException(dict) ? dict : JS_DupValue(ctx, dict);
    default:
        /* descriptor_get_wrapped_attribute */
        name = magic == 3 ? QP_ATOM(ctx, __annotations__) : QP_ATOM(ctx, __annotate__);
        dict = qp_funcwrap_dict(ctx, fw);
        if (JS_IsException(dict))
            return dict;
        dict = JS_DupValue(ctx, dict);
        r = qp_dict_get_item_atom(ctx, dict, name, &res);
        if (r != 0) {
            JS_FreeValue(ctx, dict);
            return r > 0 ? res : JS_EXCEPTION;
        }
        callable = JS_DupValue(ctx, fw->callable);
        if (QP_IS_NULL(callable)) {
            JS_FreeValue(ctx, dict);
            return qp_raise_runtime_error(ctx, qp_funcwrap_is_static(self)
                                          ? "uninitialized staticmethod object"
                                          : "uninitialized classmethod object");
        }
        res = qp_getattr(ctx, callable, name);
        JS_FreeValue(ctx, callable);
        if (!JS_IsException(res) && qp_dict_set_item_atom(ctx, dict, name, res) < 0) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, dict);
        return res;
    }
}

/* __dict__ (magic 2), __annotations__ (3), __annotate__ (4) setters. */
static int qp_funcwrap_setattr(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPFuncWrap *fw = qp_funcwrap_payload(ctx, self);
    JSValue dict, old, tn;
    JSAtom name;
    int r;

    if (!fw)
        return -1;
    if (magic == 2) {
        /* PyObject_GenericSetDict */
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "cannot delete __dict__");
            return -1;
        }
        if (!qp_dict_check(ctx, value)) {
            tn = qp_descr_tp_name_of(ctx, value);
            if (!JS_IsException(tn)) {
                qp_raise_type_error(ctx, "__dict__ must be set to a dictionary, not a '%U'", tn);
                JS_FreeValue(ctx, tn);
            }
            return -1;
        }
        old = fw->dict;
        fw->dict = JS_DupValue(ctx, value);
        JS_FreeValue(ctx, old);
        return 0;
    }
    /* descriptor_set_wrapped_attribute */
    name = magic == 3 ? QP_ATOM(ctx, __annotations__) : QP_ATOM(ctx, __annotate__);
    dict = qp_funcwrap_dict(ctx, fw);
    if (JS_IsException(dict))
        return -1;
    dict = JS_DupValue(ctx, dict);
    if (QP_IS_NULL(value)) {
        r = qp_dict_del_item_atom(ctx, dict, name);
        if (r == 0) {
            JSValue ns = qp_str_from_atom(ctx, name);
            if (!JS_IsException(ns)) {
                qp_raise(ctx, QP_EXC(ctx, AttributeError), "'%s' object has no attribute '%U'",
                         qp_funcwrap_is_static(self) ? "staticmethod" : "classmethod", ns);
                JS_FreeValue(ctx, ns);
            }
            r = -1;
        } else if (r > 0) {
            r = 0;
        }
    } else {
        r = qp_dict_set_item_atom(ctx, dict, name, value);
    }
    JS_FreeValue(ctx, dict);
    return r;
}

/* __class_getitem__ = Py_GenericAlias (METH_O | METH_CLASS) */
static JSValue qp_funcwrap_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_funcwrap_methods[] = {
    { "__class_getitem__", qp_funcwrap_class_getitem, QP_METH_O | QP_METH_CLASS, 0, NULL },
    { NULL }
};

static const QPGetSetDef qp_funcwrap_getsets[] = {
    { "__func__", qp_funcwrap_getattr, NULL, 0, NULL, QP_GETSET_MEMBER },
    { "__wrapped__", qp_funcwrap_getattr, NULL, 0, NULL, QP_GETSET_MEMBER },
    { "__isabstractmethod__", qp_funcwrap_getattr, NULL, 1, NULL },
    { "__dict__", qp_funcwrap_getattr, qp_funcwrap_setattr, 2, NULL },
    { "__annotations__", qp_funcwrap_getattr, qp_funcwrap_setattr, 3, NULL },
    { "__annotate__", qp_funcwrap_getattr, qp_funcwrap_setattr, 4, NULL },
    { NULL }
};

static const char qp_classmethod_doc[] =
    "classmethod(function, /)\n"
    "--\n"
    "\n"
    "Convert a function to be a class method.\n"
    "\n"
    "A class method receives the class as implicit first argument,\n"
    "just like an instance method receives the instance.\n"
    "To declare a class method, use this idiom:\n"
    "\n"
    "  class C:\n"
    "      @classmethod\n"
    "      def f(cls, arg1, arg2, argN):\n"
    "          ...\n"
    "\n"
    "It can be called either on the class (e.g. C.f()) or on an instance\n"
    "(e.g. C().f()).  The instance is ignored except for its class.\n"
    "If a class method is called for a derived class, the derived class\n"
    "object is passed as the implied first argument.\n"
    "\n"
    "Class methods are different than C++ or Java static methods.\n"
    "If you want those, see the staticmethod builtin.";

static const char qp_staticmethod_doc[] =
    "staticmethod(function, /)\n"
    "--\n"
    "\n"
    "Convert a function to be a static method.\n"
    "\n"
    "A static method does not receive an implicit first argument.\n"
    "To declare a static method, use this idiom:\n"
    "\n"
    "     class C:\n"
    "         @staticmethod\n"
    "         def f(arg1, arg2, argN):\n"
    "             ...\n"
    "\n"
    "It can be called either on the class (e.g. C.f()) or on an instance\n"
    "(e.g. C().f()). Both the class and the instance are ignored, and\n"
    "neither is passed implicitly as the first argument to the method.\n"
    "\n"
    "Static methods in Python are similar to those found in Java or C++.\n"
    "For a more advanced concept, see the classmethod builtin.";

/* ========================================================================= */
/* Type specs and init                                                        */
/* ========================================================================= */

static const QPTypeSpec qp_builtin_function_or_method_spec = {
    .name = "builtin_function_or_method",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_CFUNCTION,
    .slots = {
        .tp_repr = qp_cfunction_repr,
        .tp_hash = qp_cfunction_hash,
        .tp_call = qp_cfunction_call,
        .tp_richcompare = qp_cfunction_richcompare,
    },
    .methods = qp_cfunction_methods,
    .getsets = qp_cfunction_getsets,
};

static const QPTypeSpec qp_method_descriptor_spec = {
    .name = "method_descriptor",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_METHOD_DESCRIPTOR,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_methoddescr_repr,
        .tp_call = qp_methoddescr_call,
        .tp_descr_get = qp_methoddescr_get,
    },
    .methods = qp_descr_methods,
    .getsets = qp_methoddescr_getsets,
};

static const QPTypeSpec qp_classmethod_descriptor_spec = {
    .name = "classmethod_descriptor",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_methoddescr_repr,
        .tp_call = qp_classmethoddescr_call,
        .tp_descr_get = qp_classmethoddescr_get,
    },
    .getsets = qp_methoddescr_getsets,
};

static const QPTypeSpec qp_getset_descriptor_spec = {
    .name = "getset_descriptor",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_getsetdescr_repr,
        .tp_descr_get = qp_getsetdescr_get,
        .tp_descr_set = qp_getsetdescr_set,
    },
    .getsets = qp_attrdescr_getsets,
};

static const QPTypeSpec qp_member_descriptor_spec = {
    .name = "member_descriptor",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_memberdescr_repr,
        .tp_descr_get = qp_memberdescr_get,
        .tp_descr_set = qp_memberdescr_set,
    },
    .methods = qp_descr_methods,
    .getsets = qp_attrdescr_getsets,
};

static const QPTypeSpec qp_wrapper_descriptor_spec = {
    .name = "wrapper_descriptor",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_METHOD_DESCRIPTOR,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_wrapperdescr_repr,
        .tp_call = qp_wrapperdescr_call,
        .tp_descr_get = qp_wrapperdescr_get,
    },
    .methods = qp_descr_methods,
    .getsets = qp_methoddescr_getsets,
};

static const QPTypeSpec qp_method_wrapper_spec = {
    .name = "method-wrapper",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_DESCR,
    .slots = {
        .tp_repr = qp_methodwrapper_repr,
        .tp_hash = qp_methodwrapper_hash,
        .tp_call = qp_methodwrapper_call,
        .tp_richcompare = qp_methodwrapper_richcompare,
    },
    .methods = qp_descr_methods,
    .getsets = qp_methodwrapper_getsets,
};

static const QPTypeSpec qp_property_spec = {
    .name = "property",
    .doc = qp_property_doc,
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_PROPERTY,
    .slots = {
        .tp_descr_get = qp_property_descr_get,
        .tp_descr_set = qp_property_descr_set,
        .tp_init = qp_property_init,
        .tp_new = qp_property_new,
    },
    .methods = qp_property_methods,
    .getsets = qp_property_getsets,
};

static const QPTypeSpec qp_classmethod_spec = {
    .name = "classmethod",
    .doc = qp_classmethod_doc,
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_CLASSMETHOD,
    .slots = {
        .tp_getattro = qp_funcwrap_getattro,
        .tp_setattro = qp_funcwrap_setattro,
        .tp_repr = qp_funcwrap_repr,
        .tp_descr_get = qp_classmethod_descr_get,
        .tp_init = qp_classmethod_tp_init,
        .tp_new = qp_classmethod_tp_new,
    },
    .methods = qp_funcwrap_methods,
    .getsets = qp_funcwrap_getsets,
    .generic_attr = QP_GENERIC_GETATTR | QP_GENERIC_SETATTR,
};

static const QPTypeSpec qp_staticmethod_spec = {
    .name = "staticmethod",
    .doc = qp_staticmethod_doc,
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_STATICMETHOD,
    .slots = {
        .tp_getattro = qp_funcwrap_getattro,
        .tp_setattro = qp_funcwrap_setattro,
        .tp_repr = qp_funcwrap_repr,
        .tp_call = qp_staticmethod_call,
        .tp_descr_get = qp_staticmethod_descr_get,
        .tp_init = qp_staticmethod_tp_init,
        .tp_new = qp_staticmethod_tp_new,
    },
    .methods = qp_funcwrap_methods,
    .getsets = qp_funcwrap_getsets,
    .generic_attr = QP_GENERIC_GETATTR | QP_GENERIC_SETATTR,
};

static int qp_init_descr_types(JSContext *ctx)
{
    /* builtin_function_or_method and the descriptor types first: readying
       the others creates descriptors and (static methods) cfunctions;
       staticmethod before classmethod (its __class_getitem__ is a
       classmethod_descriptor, no staticmethod needed) */
    if (qp_type_ready(ctx, QP_TYPE(ctx, builtin_function_or_method),
                      &qp_builtin_function_or_method_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, method_descriptor), &qp_method_descriptor_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, classmethod_descriptor),
                      &qp_classmethod_descriptor_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, getset_descriptor), &qp_getset_descriptor_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, member_descriptor), &qp_member_descriptor_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, wrapper_descriptor), &qp_wrapper_descriptor_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, method_wrapper), &qp_method_wrapper_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, staticmethod), &qp_staticmethod_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, classmethod), &qp_classmethod_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, property), &qp_property_spec) < 0)
        return -1;
    return 0;
}
