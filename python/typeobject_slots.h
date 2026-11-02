/*
 * dquickts Python, unit h: slotdefs, slot wrappers (wrapper_descriptor
 * implementations), slot_* dispatchers for heap types, update_one_slot.
 * Private part of python/typeobject.c (included once from it).
 *
 * Derived from CPython Objects/typeobject.c (PSF-2.0).
 */

/* ========================================================================= */
/* Method lookup helpers (lookup_maybe_method, vectorcall_method, ...)        */
/* ========================================================================= */

/* lookup_method_ex: 1 = unbound function in *out (call with self first),
   0 = bound/plain value in *out, -1 = error, -2 = absent (no exception;
   only when !raise).  With raise, absence raises AttributeError(name). */
static int qp_lookup_method_ex(JSContext *ctx, JSValueConst self, JSAtom attr,
                               JSValue *out, int raise)
{
    QPType *tp = qp_type_of(ctx, self);
    JSValue v = qp_type_lookup(ctx, tp, attr);
    QPType *vt;

    *out = QP_NULL;
    if (QP_IS_NULL(v)) {
        if (raise) {
            JSValue n = qp_str_from_atom(ctx, attr);
            if (!JS_IsException(n)) {
                qp_raise_value(ctx, QP_EXC(ctx, AttributeError), n);
                JS_FreeValue(ctx, n);
            }
            return -1;
        }
        return -2;
    }
    v = JS_DupValue(ctx, v);
    vt = qp_type_of(ctx, v);
    if (vt->flags & QP_TPFLAGS_METHOD_DESCRIPTOR) {
        *out = v;
        return 1;
    }
    if (vt->slots.tp_descr_get) {
        JSValue r = vt->slots.tp_descr_get(ctx, v, self, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, v);
        if (JS_IsException(r)) {
            if (!raise && qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
                qp_tw_clear_exc(ctx);
                return -2;
            }
            return -1;
        }
        *out = r;
        return 0;
    }
    *out = v;
    return 0;
}

/* Call func(self, *args, **kw) where args[0..nargs) + kw values follow. */
static JSValue qp_call_prepend(JSContext *ctx, JSValueConst func, JSValueConst self,
                               JSValueConst *args, Py_ssize_t nargs, JSValueConst kwnames)
{
    Py_ssize_t nkw = qp_kwcount(kwnames), i, tot = nargs + nkw;
    JSValueConst stackbuf[9], *buf = stackbuf;
    JSValue r;

    if (tot + 2 > (Py_ssize_t)countof(stackbuf)) {
        buf = js_malloc(ctx, (tot + 2) * sizeof(JSValueConst));
        if (!buf)
            return JS_EXCEPTION;
    }
    buf[0] = JS_UNDEFINED; /* scratch for QP_VECTORCALL_ARGUMENTS_OFFSET */
    buf[1] = self;
    for (i = 0; i < tot; i++)
        buf[i + 2] = args[i];
    r = qp_vectorcall(ctx, func, buf + 1, (size_t)(nargs + 1) | QP_VECTORCALL_ARGUMENTS_OFFSET,
                      nkw ? kwnames : QP_NULL);
    if (buf != stackbuf)
        js_free(ctx, buf);
    return r;
}

static JSValue qp_call_unbound(JSContext *ctx, int unbound, JSValueConst func,
                               JSValueConst *args, int nargs)
{
    if (unbound)
        return qp_vectorcall(ctx, func, args, nargs, QP_NULL);
    return qp_vectorcall(ctx, func, args + 1, nargs - 1, QP_NULL);
}

/* vectorcall_method: args[0] is self; AttributeError(name) when missing. */
static JSValue qp_vectorcall_method(JSContext *ctx, JSAtom name, JSValueConst *args, int nargs)
{
    JSValue func, r;
    int unbound = qp_lookup_method_ex(ctx, args[0], name, &func, 1);
    if (unbound < 0)
        return JS_EXCEPTION;
    r = qp_call_unbound(ctx, unbound, func, args, nargs);
    JS_FreeValue(ctx, func);
    return r;
}

/* vectorcall_maybe: NotImplemented when the method is missing. */
static JSValue qp_vectorcall_maybe(JSContext *ctx, JSAtom name, JSValueConst *args, int nargs)
{
    JSValue func, r;
    int unbound = qp_lookup_method_ex(ctx, args[0], name, &func, 0);
    if (unbound == -1)
        return JS_EXCEPTION;
    if (unbound == -2)
        return qp_not_implemented(ctx);
    r = qp_call_unbound(ctx, unbound, func, args, nargs);
    JS_FreeValue(ctx, func);
    return r;
}

/* maybe_call_special_{no,one}_arg: QP_NULL when absent (no exception);
   with attr_is_none, a None attribute is reported and not called. */
static JSValue qp_maybe_call_special(JSContext *ctx, JSValueConst self, JSAtom attr,
                                     JSValueConst arg, int *attr_is_none)
{
    JSValue func, r = QP_NULL;
    JSValueConst a[2];
    int unbound = qp_lookup_method_ex(ctx, self, attr, &func, 0);
    if (attr_is_none)
        *attr_is_none = 0;
    if (unbound == -1)
        return JS_EXCEPTION;
    if (unbound == -2)
        return QP_NULL;
    if (attr_is_none && JS_IsUndefined(func) && !unbound) {
        *attr_is_none = 1;
        JS_FreeValue(ctx, func);
        return QP_NULL;
    }
    a[0] = self;
    a[1] = arg;
    r = qp_call_unbound(ctx, unbound, func, a, QP_IS_NULL(arg) ? 1 : 2);
    JS_FreeValue(ctx, func);
    return r;
}

/* ========================================================================= */
/* Wrappers (wrapper_descriptor bodies)                                       */
/* ========================================================================= */

static int qp_check_num_args(JSContext *ctx, int nargs, int n)
{
    if (nargs == n)
        return 0;
    qp_raise_type_error(ctx, "expected %d argument%s, got %d", n, n == 1 ? "" : "s", nargs);
    return -1;
}

static JSValue qp_wrap_lenfunc(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                               JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t r;
    if (qp_check_num_args(ctx, nargs, 0) < 0)
        return JS_EXCEPTION;
    r = ((QPLenFunc *)wrapped)(ctx, self);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, r);
}

static JSValue qp_wrap_inquirypred(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, void *wrapped)
{
    int r;
    if (qp_check_num_args(ctx, nargs, 0) < 0)
        return JS_EXCEPTION;
    r = ((QPInquiry *)wrapped)(ctx, self);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

static JSValue qp_wrap_binaryfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    return ((QPBinaryFunc *)wrapped)(ctx, self, args[0]);
}

static JSValue qp_wrap_binaryfunc_l(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    return ((QPBinaryFunc *)wrapped)(ctx, self, args[0]);
}

static JSValue qp_wrap_binaryfunc_r(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    return ((QPBinaryFunc *)wrapped)(ctx, args[0], self);
}

static int qp_check_pow_args(JSContext *ctx, int nargs)
{
    if (nargs >= 1 && nargs <= 2)
        return 0;
    qp_raise_type_error(ctx, "expected %d or %d arguments, got %d", 1, 2, nargs);
    return -1;
}

static JSValue qp_wrap_ternaryfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_pow_args(ctx, nargs) < 0)
        return JS_EXCEPTION;
    return ((QPTernaryFunc *)wrapped)(ctx, self, args[0], nargs == 2 ? args[1] : JS_UNDEFINED);
}

static JSValue qp_wrap_ternaryfunc_r(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_pow_args(ctx, nargs) < 0)
        return JS_EXCEPTION;
    return ((QPTernaryFunc *)wrapped)(ctx, args[0], self, nargs == 2 ? args[1] : JS_UNDEFINED);
}

static JSValue qp_wrap_unaryfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 0) < 0)
        return JS_EXCEPTION;
    return ((QPUnaryFunc *)wrapped)(ctx, self);
}

static JSValue qp_wrap_indexargfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t i;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (qp_as_ssize(ctx, args[0], QP_EXC(ctx, OverflowError), &i) < 0)
        return JS_EXCEPTION;
    return ((QPSsizeArgFunc *)wrapped)(ctx, self, i);
}

/* getindex: negative indices adjusted by sq_length. */
static int qp_getindex(JSContext *ctx, JSValueConst self, JSValueConst arg, Py_ssize_t *pi)
{
    Py_ssize_t i;
    QPType *t;
    if (qp_as_ssize(ctx, arg, QP_EXC(ctx, OverflowError), &i) < 0)
        return -1;
    if (i < 0) {
        t = qp_type_of(ctx, self);
        if (t->slots.sq.sq_length) {
            Py_ssize_t n = t->slots.sq.sq_length(ctx, self);
            if (n < 0)
                return -1;
            i += n;
        }
    }
    *pi = i;
    return 0;
}

static JSValue qp_wrap_sq_item(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t i;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (qp_getindex(ctx, self, args[0], &i) < 0)
        return JS_EXCEPTION;
    return ((QPSsizeArgFunc *)wrapped)(ctx, self, i);
}

static JSValue qp_wrap_sq_setitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t i;
    if (qp_tw_unpack_check(ctx, "__setitem__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_getindex(ctx, self, args[0], &i) < 0)
        return JS_EXCEPTION;
    if (((QPSsizeObjArgProc *)wrapped)(ctx, self, i, args[1]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_sq_delitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t i;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (qp_getindex(ctx, self, args[0], &i) < 0)
        return JS_EXCEPTION;
    if (((QPSsizeObjArgProc *)wrapped)(ctx, self, i, QP_NULL) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_objobjproc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, void *wrapped)
{
    int r;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    r = ((QPObjObjProc *)wrapped)(ctx, self, args[0]);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

static JSValue qp_wrap_objobjargproc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_tw_unpack_check(ctx, "__setitem__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (((QPObjObjArgProc *)wrapped)(ctx, self, args[0], args[1]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_delitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (((QPObjObjArgProc *)wrapped)(ctx, self, args[0], QP_NULL) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static int qp_slot_tp_setattro(JSContext *ctx, JSValueConst self, JSAtom name, JSValueConst value);

/* hackcheck (Carlo Verre hack): object.__setattr__ applied to a type must
   not skip intermediate C-level setattro overrides. */
static int qp_hackcheck(JSContext *ctx, JSValueConst self, QPSetAttroFunc *func, const char *what)
{
    QPType *type = qp_type_from_value(self), *defining, *base;
    Py_ssize_t i;
    if (!type)
        return 1;
    type = qp_type_of(ctx, self);
    if (JS_VALUE_GET_TAG(type->mro) != JS_TAG_OBJECT)
        return 1;
    defining = type;
    for (i = QP_TUPLE_SIZE(type->mro) - 1; i >= 0; i--) {
        base = qp_type_from_value(QP_TUPLE_ITEMS(type->mro)[i]);
        if (base->slots.tp_setattro == qp_slot_tp_setattro) {
        } else if (base->slots.tp_setattro == type->slots.tp_setattro) {
            defining = base;
            break;
        }
    }
    for (base = defining; base; base = base->base) {
        if (base->slots.tp_setattro == func)
            break;
        if (base->slots.tp_setattro != qp_slot_tp_setattro) {
            JSValue tn = qp_type_tp_name(ctx, type);
            qp_raise_type_error(ctx, "can't apply this %s to %U object", what, tn);
            JS_FreeValue(ctx, tn);
            return 0;
        }
    }
    return 1;
}

static JSValue qp_wrap_setattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, void *wrapped)
{
    JSAtom a;
    int r;
    if (qp_tw_unpack_check(ctx, "__setattr__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_hackcheck(ctx, self, (QPSetAttroFunc *)wrapped, "__setattr__"))
        return JS_EXCEPTION;
    a = qp_tw_name_to_atom(ctx, args[0]);
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = ((QPSetAttroFunc *)wrapped)(ctx, self, a, args[1]);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_delattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, void *wrapped)
{
    JSAtom a;
    int r;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (!qp_hackcheck(ctx, self, (QPSetAttroFunc *)wrapped, "__delattr__"))
        return JS_EXCEPTION;
    a = qp_tw_name_to_atom(ctx, args[0]);
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = ((QPSetAttroFunc *)wrapped)(ctx, self, a, QP_NULL);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_getattro(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, void *wrapped)
{
    JSAtom a;
    JSValue r;
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    a = qp_tw_name_to_atom(ctx, args[0]);
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = ((QPGetAttroFunc *)wrapped)(ctx, self, a);
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue qp_wrap_hashfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_hash_t h;
    if (qp_check_num_args(ctx, nargs, 0) < 0)
        return JS_EXCEPTION;
    h = ((QPHashFunc *)wrapped)(ctx, self);
    if (h == -1 && JS_HasException(ctx))
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, h);
}

static JSValue qp_wrap_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, void *wrapped)
{
    return ((QPVectorcallFunc *)wrapped)(ctx, self, args, nargs, kwnames);
}

static JSValue qp_wrap_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, void *wrapped)
{
    if (((QPInitProc *)wrapped)(ctx, self, args, nargs, kwnames) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_richcmpfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, void *wrapped, int op)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    return ((QPRichCmpFunc *)wrapped)(ctx, self, args[0], op);
}

#define QP_RICHCMP_WRAPPER(NAME, OP)                                          \
static JSValue qp_richcmp_##NAME(JSContext *ctx, JSValueConst self,           \
                                 JSValueConst *args, int nargs,               \
                                 JSValueConst kwnames, void *wrapped)         \
{                                                                             \
    return qp_wrap_richcmpfunc(ctx, self, args, nargs, wrapped, OP);          \
}
QP_RICHCMP_WRAPPER(lt, QP_CMP_LT)
QP_RICHCMP_WRAPPER(le, QP_CMP_LE)
QP_RICHCMP_WRAPPER(eq, QP_CMP_EQ)
QP_RICHCMP_WRAPPER(ne, QP_CMP_NE)
QP_RICHCMP_WRAPPER(gt, QP_CMP_GT)
QP_RICHCMP_WRAPPER(ge, QP_CMP_GE)

static JSValue qp_wrap_next(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, void *wrapped)
{
    JSValue r;
    if (qp_check_num_args(ctx, nargs, 0) < 0)
        return JS_EXCEPTION;
    r = ((QPIterNextFunc *)wrapped)(ctx, self);
    if (QP_IS_NULL(r))
        return qp_raise_value(ctx, QP_EXC(ctx, StopIteration), JS_UNDEFINED);
    return r;
}

static JSValue qp_wrap_descr_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, void *wrapped)
{
    JSValueConst obj, type = QP_NULL;
    if (qp_tw_unpack_check(ctx, "__get__", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    obj = args[0];
    if (nargs == 2)
        type = args[1];
    if (JS_IsUndefined(obj) || JS_IsNull(obj))
        obj = QP_NULL;
    if (JS_IsUndefined(type) || JS_IsNull(type))
        type = QP_NULL;
    if (QP_IS_NULL(obj) && QP_IS_NULL(type))
        return qp_raise_type_error(ctx, "__get__(None, None) is invalid");
    return ((QPDescrGetFunc *)wrapped)(ctx, self, obj, type);
}

static JSValue qp_wrap_descr_set(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_tw_unpack_check(ctx, "__set__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (((QPDescrSetFunc *)wrapped)(ctx, self, args[0], args[1]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_wrap_descr_delete(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, void *wrapped)
{
    if (qp_check_num_args(ctx, nargs, 1) < 0)
        return JS_EXCEPTION;
    if (((QPDescrSetFunc *)wrapped)(ctx, self, args[0], QP_NULL) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* __buffer__(flags): memoryview over the exporter (_PyMemoryView_FromBufferProc). */
static JSValue qp_wrap_buffer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, void *wrapped)
{
    Py_ssize_t flags;
    if (qp_tw_unpack_check(ctx, "__buffer__", nargs, 1, 1) < 0)
        return JS_EXCEPTION;
    if (qp_as_ssize(ctx, args[0], QP_EXC(ctx, OverflowError), &flags) < 0)
        return JS_EXCEPTION;
    if (flags > INT_MAX || flags < INT_MIN)
        return qp_raise_overflow_error(ctx, "buffer flags out of range");
    return qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, memoryview)), self);
}

static JSValue qp_wrap_releasebuffer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, void *wrapped)
{
    JSValue r;
    JSAtom rel;
    if (qp_tw_unpack_check(ctx, "__release_buffer__", nargs, 1, 1) < 0)
        return JS_EXCEPTION;
    if (!qp_typecheck(ctx, args[0], QP_TYPE(ctx, memoryview)))
        return qp_raise_type_error(ctx, "expected a memoryview object");
    rel = JS_NewAtom(ctx, "release");
    r = qp_call_method(ctx, args[0], rel, NULL, 0, QP_NULL);
    JS_FreeAtom(ctx, rel);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* slot_* dispatchers (heap types)                                            */
/* ========================================================================= */

#define QP_SLOT0(FUNC, DUNDER)                                                \
static JSValue FUNC(JSContext *ctx, JSValueConst self)                        \
{                                                                             \
    JSValueConst st[1] = { self };                                            \
    return qp_vectorcall_method(ctx, QP_ATOM(ctx, DUNDER), st, 1);            \
}

#define QP_SLOT1(FUNC, DUNDER)                                                \
static JSValue FUNC(JSContext *ctx, JSValueConst self, JSValueConst arg1)     \
{                                                                             \
    JSValueConst st[2] = { self, arg1 };                                      \
    return qp_vectorcall_method(ctx, QP_ATOM(ctx, DUNDER), st, 2);            \
}

/* method_is_overloaded: right's type overrides `name` differently. */
static int qp_method_is_overloaded(JSContext *ctx, JSValueConst left, JSValueConst right,
                                   JSAtom name)
{
    JSValue a, b;
    int r, ok;
    r = qp_getattr_opt(ctx, QP_TYPE_VALUE(qp_type_of(ctx, right)), name, &b);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    r = qp_getattr_opt(ctx, QP_TYPE_VALUE(qp_type_of(ctx, left)), name, &a);
    if (r < 0) {
        JS_FreeValue(ctx, b);
        return -1;
    }
    if (r == 0) {
        JS_FreeValue(ctx, b);
        return 1;
    }
    ok = qp_richcompare_bool(ctx, a, b, QP_CMP_NE);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return ok;
}

#define QP_SLOT1BINFULL(FUNC, TESTFUNC, SLOT, DUNDER, RDUNDER)                \
static JSValue FUNC(JSContext *ctx, JSValueConst self, JSValueConst other)    \
{                                                                             \
    QPType *ts = qp_type_of(ctx, self), *to = qp_type_of(ctx, other);         \
    JSValueConst st[2];                                                       \
    JSValue r;                                                                \
    int do_other = ts != to && (void *)to->slots.nb.SLOT == (void *)TESTFUNC; \
    if ((void *)ts->slots.nb.SLOT == (void *)TESTFUNC) {                      \
        if (do_other && qp_type_is_subtype(to, ts)) {                         \
            int ok = qp_method_is_overloaded(ctx, self, other, QP_ATOM(ctx, RDUNDER)); \
            if (ok < 0)                                                       \
                return JS_EXCEPTION;                                          \
            if (ok) {                                                         \
                st[0] = other;                                                \
                st[1] = self;                                                 \
                r = qp_vectorcall_maybe(ctx, QP_ATOM(ctx, RDUNDER), st, 2);   \
                if (!qp_is_notimpl(ctx, r))                                   \
                    return r;                                                 \
                JS_FreeValue(ctx, r);                                         \
                do_other = 0;                                                 \
            }                                                                 \
        }                                                                     \
        st[0] = self;                                                         \
        st[1] = other;                                                        \
        r = qp_vectorcall_maybe(ctx, QP_ATOM(ctx, DUNDER), st, 2);            \
        if (!qp_is_notimpl(ctx, r) || to == ts)                               \
            return r;                                                         \
        JS_FreeValue(ctx, r);                                                 \
    }                                                                         \
    if (do_other) {                                                           \
        st[0] = other;                                                        \
        st[1] = self;                                                         \
        return qp_vectorcall_maybe(ctx, QP_ATOM(ctx, RDUNDER), st, 2);        \
    }                                                                         \
    return qp_not_implemented(ctx);                                           \
}

#define QP_SLOT1BIN(FUNC, SLOT, DUNDER, RDUNDER) \
    QP_SLOT1BINFULL(FUNC, FUNC, SLOT, DUNDER, RDUNDER)

static Py_ssize_t qp_slot_sq_length(JSContext *ctx, JSValueConst self)
{
    JSValueConst st[1] = { self };
    JSValue res = qp_vectorcall_method(ctx, QP_ATOM(ctx, __len__), st, 1), idx;
    Py_ssize_t len;
    if (JS_IsException(res))
        return -1;
    idx = qp_index(ctx, res);
    JS_FreeValue(ctx, res);
    if (JS_IsException(idx))
        return -1;
    if (qp_long_sign(idx) < 0) {
        JS_FreeValue(ctx, idx);
        qp_raise_value_error(ctx, "__len__() should return >= 0");
        return -1;
    }
    if (qp_as_ssize(ctx, idx, QP_EXC(ctx, OverflowError), &len) < 0)
        len = -1;
    JS_FreeValue(ctx, idx);
    return len;
}

static JSValue qp_slot_sq_item(JSContext *ctx, JSValueConst self, Py_ssize_t i)
{
    JSValue ival = qp_long_from_int64(ctx, i), r;
    JSValueConst st[2];
    if (JS_IsException(ival))
        return ival;
    st[0] = self;
    st[1] = ival;
    r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __getitem__), st, 2);
    JS_FreeValue(ctx, ival);
    return r;
}

static int qp_slot_sq_ass_item(JSContext *ctx, JSValueConst self, Py_ssize_t index,
                               JSValueConst value)
{
    JSValue io = qp_long_from_int64(ctx, index), r;
    JSValueConst st[3];
    if (JS_IsException(io))
        return -1;
    st[0] = self;
    st[1] = io;
    st[2] = value;
    if (QP_IS_NULL(value))
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __delitem__), st, 2);
    else
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __setitem__), st, 3);
    JS_FreeValue(ctx, io);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* _PySequence_IterSearch(PY_ITERSEARCH_CONTAINS) */
static int qp_iter_search_contains(JSContext *ctx, JSValueConst seq, JSValueConst value)
{
    JSValue it = qp_iter(ctx, seq), item;
    int r;
    if (JS_IsException(it))
        return -1;
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item)) {
            r = -1;
            break;
        }
        if (QP_IS_NULL(item)) {
            r = 0;
            break;
        }
        r = qp_richcompare_bool(ctx, item, value, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (r != 0)
            break;
    }
    JS_FreeValue(ctx, it);
    return r;
}

static int qp_slot_sq_contains(JSContext *ctx, JSValueConst self, JSValueConst value)
{
    int attr_is_none = 0, r;
    JSValue res = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __contains__), value,
                                        &attr_is_none);
    if (attr_is_none) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
        qp_raise_type_error(ctx, "'%.200U' object is not a container", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    if (JS_IsException(res))
        return -1;
    if (QP_IS_NULL(res))
        return qp_iter_search_contains(ctx, self, value);
    r = qp_bool(ctx, res);
    JS_FreeValue(ctx, res);
    return r;
}

#define qp_slot_mp_length qp_slot_sq_length

QP_SLOT1(qp_slot_mp_subscript, __getitem__)

static int qp_slot_mp_ass_subscript(JSContext *ctx, JSValueConst self, JSValueConst key,
                                    JSValueConst value)
{
    JSValueConst st[3] = { self, key, value };
    JSValue r;
    if (QP_IS_NULL(value))
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __delitem__), st, 2);
    else
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __setitem__), st, 3);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

QP_SLOT1BIN(qp_slot_nb_add, nb_add, __add__, __radd__)
QP_SLOT1BIN(qp_slot_nb_subtract, nb_subtract, __sub__, __rsub__)
QP_SLOT1BIN(qp_slot_nb_multiply, nb_multiply, __mul__, __rmul__)
QP_SLOT1BIN(qp_slot_nb_matrix_multiply, nb_matrix_multiply, __matmul__, __rmatmul__)
QP_SLOT1BIN(qp_slot_nb_remainder, nb_remainder, __mod__, __rmod__)
QP_SLOT1BIN(qp_slot_nb_divmod, nb_divmod, __divmod__, __rdivmod__)

static JSValue qp_slot_nb_power(JSContext *ctx, JSValueConst self, JSValueConst other,
                                JSValueConst modulus);

QP_SLOT1BINFULL(qp_slot_nb_power_binary, qp_slot_nb_power, nb_power, __pow__, __rpow__)

static JSValue qp_slot_nb_power(JSContext *ctx, JSValueConst self, JSValueConst other,
                                JSValueConst modulus)
{
    QPType *ts, *to;
    JSValueConst st[3];
    JSValue r;
    int do_other;

    if (JS_IsUndefined(modulus) || JS_IsNull(modulus))
        return qp_slot_nb_power_binary(ctx, self, other);
    ts = qp_type_of(ctx, self);
    to = qp_type_of(ctx, other);
    do_other = ts != to && to->slots.nb.nb_power == qp_slot_nb_power;
    if (ts->slots.nb.nb_power == qp_slot_nb_power) {
        if (do_other && qp_type_is_subtype(to, ts)) {
            int ok = qp_method_is_overloaded(ctx, self, other, QP_ATOM(ctx, __rpow__));
            if (ok < 0)
                return JS_EXCEPTION;
            if (ok) {
                st[0] = other;
                st[1] = self;
                st[2] = modulus;
                r = qp_vectorcall_maybe(ctx, QP_ATOM(ctx, __rpow__), st, 3);
                if (!qp_is_notimpl(ctx, r))
                    return r;
                JS_FreeValue(ctx, r);
                do_other = 0;
            }
        }
        st[0] = self;
        st[1] = other;
        st[2] = modulus;
        r = qp_vectorcall_maybe(ctx, QP_ATOM(ctx, __pow__), st, 3);
        if (!qp_is_notimpl(ctx, r) || to == ts)
            return r;
        JS_FreeValue(ctx, r);
    }
    if (do_other) {
        st[0] = other;
        st[1] = self;
        st[2] = modulus;
        return qp_vectorcall_maybe(ctx, QP_ATOM(ctx, __rpow__), st, 3);
    }
    return qp_not_implemented(ctx);
}

QP_SLOT0(qp_slot_nb_negative, __neg__)
QP_SLOT0(qp_slot_nb_positive, __pos__)
QP_SLOT0(qp_slot_nb_absolute, __abs__)

static int qp_slot_nb_bool(JSContext *ctx, JSValueConst self)
{
    int using_len = 0, attr_is_none = 0, result;
    JSValue value = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __bool__), QP_NULL,
                                          &attr_is_none);
    if (attr_is_none) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
        qp_raise_type_error(ctx, "'%.200U' cannot be interpreted as a boolean", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    if (JS_IsException(value))
        return -1;
    if (QP_IS_NULL(value)) {
        value = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __len__), QP_NULL, NULL);
        if (JS_IsException(value))
            return -1;
        if (QP_IS_NULL(value))
            return 1;
        using_len = 1;
    }
    if (using_len) {
        result = qp_bool(ctx, value);
    } else if (JS_VALUE_GET_TAG(value) == JS_TAG_BOOL) {
        result = JS_VALUE_GET_BOOL(value);
    } else {
        qp_raise_type_error(ctx, "%T.__bool__() must return a bool, not %T", self, value);
        result = -1;
    }
    JS_FreeValue(ctx, value);
    return result;
}

QP_SLOT0(qp_slot_nb_index, __index__)
QP_SLOT0(qp_slot_nb_invert, __invert__)
QP_SLOT1BIN(qp_slot_nb_lshift, nb_lshift, __lshift__, __rlshift__)
QP_SLOT1BIN(qp_slot_nb_rshift, nb_rshift, __rshift__, __rrshift__)
QP_SLOT1BIN(qp_slot_nb_and, nb_and, __and__, __rand__)
QP_SLOT1BIN(qp_slot_nb_xor, nb_xor, __xor__, __rxor__)
QP_SLOT1BIN(qp_slot_nb_or, nb_or, __or__, __ror__)
QP_SLOT0(qp_slot_nb_int, __int__)
QP_SLOT0(qp_slot_nb_float, __float__)
QP_SLOT1(qp_slot_nb_inplace_add, __iadd__)
QP_SLOT1(qp_slot_nb_inplace_subtract, __isub__)
QP_SLOT1(qp_slot_nb_inplace_multiply, __imul__)
QP_SLOT1(qp_slot_nb_inplace_matrix_multiply, __imatmul__)
QP_SLOT1(qp_slot_nb_inplace_remainder, __imod__)

static JSValue qp_slot_nb_inplace_power(JSContext *ctx, JSValueConst self, JSValueConst arg1,
                                        JSValueConst arg2)
{
    JSValueConst st[2] = { self, arg1 };
    return qp_vectorcall_method(ctx, QP_ATOM(ctx, __ipow__), st, 2);
}

QP_SLOT1(qp_slot_nb_inplace_lshift, __ilshift__)
QP_SLOT1(qp_slot_nb_inplace_rshift, __irshift__)
QP_SLOT1(qp_slot_nb_inplace_and, __iand__)
QP_SLOT1(qp_slot_nb_inplace_xor, __ixor__)
QP_SLOT1(qp_slot_nb_inplace_or, __ior__)
QP_SLOT1BIN(qp_slot_nb_floor_divide, nb_floor_divide, __floordiv__, __rfloordiv__)
QP_SLOT1BIN(qp_slot_nb_true_divide, nb_true_divide, __truediv__, __rtruediv__)
QP_SLOT1(qp_slot_nb_inplace_floor_divide, __ifloordiv__)
QP_SLOT1(qp_slot_nb_inplace_true_divide, __itruediv__)

static JSValue qp_slot_tp_repr(JSContext *ctx, JSValueConst self)
{
    JSValue res = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __repr__), QP_NULL, NULL), tn;
    if (!QP_IS_NULL(res))
        return res;
    tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
    res = qp_str_from_format(ctx, "<%U object at %p>", tn, qp_obj_addr(self));
    JS_FreeValue(ctx, tn);
    return res;
}

QP_SLOT0(qp_slot_tp_str, __str__)

static Py_hash_t qp_slot_tp_hash(JSContext *ctx, JSValueConst self)
{
    int attr_is_none = 0;
    JSValue res = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __hash__), QP_NULL,
                                        &attr_is_none), v;
    int64_t h;
    if (attr_is_none || QP_IS_NULL(res) || JS_IsException(res)) {
        if (JS_IsException(res))
            return -1;
        return qp_hash_not_implemented(ctx, self);
    }
    if (!qp_long_check(ctx, res)) {
        JS_FreeValue(ctx, res);
        qp_raise_type_error(ctx, "__hash__ method should return an integer");
        return -1;
    }
    v = qp_long_value(ctx, res);
    JS_FreeValue(ctx, res);
    if (JS_IsException(v))
        return -1;
    {
        int overflow = 0;
        qp_long_as_int64_overflow(v, &h, &overflow);
        if (overflow)
            h = qp_long_hash(v);
    }
    JS_FreeValue(ctx, v);
    if (h == -1)
        h = -2;
    return (Py_hash_t)h;
}

static JSValue qp_slot_tp_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    JSValue meth, r;
    Py_ssize_t nargs = qp_nargs(nargsf);
    int unbound = qp_lookup_method_ex(ctx, self, QP_ATOM(ctx, __call__), &meth, 1);
    if (unbound < 0)
        return JS_EXCEPTION;
    if (unbound)
        r = qp_call_prepend(ctx, meth, self, args, nargs, kwnames);
    else
        r = qp_vectorcall(ctx, meth, args, nargs, kwnames);
    JS_FreeValue(ctx, meth);
    return r;
}

static JSValue qp_slot_tp_getattro(JSContext *ctx, JSValueConst self, JSAtom name)
{
    JSValue n = qp_str_from_atom(ctx, name), r;
    JSValueConst st[2];
    if (JS_IsException(n))
        return n;
    st[0] = self;
    st[1] = n;
    r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __getattribute__), st, 2);
    JS_FreeValue(ctx, n);
    return r;
}

static JSValue qp_call_attribute(JSContext *ctx, JSValueConst self, JSValueConst attr,
                                 JSAtom name)
{
    QPType *at = qp_type_of(ctx, attr);
    JSValue n = qp_str_from_atom(ctx, name), descr = QP_NULL, r;
    if (JS_IsException(n))
        return n;
    if (at->flags & QP_TPFLAGS_METHOD_DESCRIPTOR) {
        JSValueConst a[2] = { self, n };
        r = qp_vectorcall(ctx, attr, a, 2, QP_NULL);
        JS_FreeValue(ctx, n);
        return r;
    }
    if (at->slots.tp_descr_get) {
        descr = at->slots.tp_descr_get(ctx, attr, self, QP_TYPE_VALUE(qp_type_of(ctx, self)));
        if (JS_IsException(descr)) {
            JS_FreeValue(ctx, n);
            return descr;
        }
        attr = descr;
    }
    r = qp_call1(ctx, attr, n);
    JS_FreeValue(ctx, descr);
    JS_FreeValue(ctx, n);
    return r;
}

static JSValue qp_slot_tp_getattr_hook(JSContext *ctx, JSValueConst self, JSAtom name)
{
    QPType *tp = qp_type_of(ctx, self);
    JSValue getattr, getattribute, res;
    QPDescr *d;

    getattr = qp_type_lookup(ctx, tp, QP_ATOM(ctx, __getattr__));
    if (QP_IS_NULL(getattr)) {
        tp->slots.tp_getattro = qp_slot_tp_getattro;
        return qp_slot_tp_getattro(ctx, self, name);
    }
    getattr = JS_DupValue(ctx, getattr);
    getattribute = qp_type_lookup(ctx, tp, QP_ATOM(ctx, __getattribute__));
    d = QP_IS_NULL(getattribute) ? NULL : qp_descr_get(getattribute);
    if (QP_IS_NULL(getattribute) ||
        (d && d->kind == QP_DESCR_WRAPPER && d->u.wrapper.wrapped == (void *)qp_generic_getattr)) {
        res = qp_generic_getattr_ex(ctx, self, name, 1);
        if (QP_IS_NULL(res))
            res = qp_call_attribute(ctx, self, getattr, name);
    } else {
        getattribute = JS_DupValue(ctx, getattribute);
        res = qp_call_attribute(ctx, self, getattribute, name);
        JS_FreeValue(ctx, getattribute);
        if (JS_IsException(res) && qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
            qp_tw_clear_exc(ctx);
            res = qp_call_attribute(ctx, self, getattr, name);
        }
    }
    JS_FreeValue(ctx, getattr);
    return res;
}

static int qp_slot_tp_setattro(JSContext *ctx, JSValueConst self, JSAtom name, JSValueConst value)
{
    JSValue n = qp_str_from_atom(ctx, name), r;
    JSValueConst st[3];
    if (JS_IsException(n))
        return -1;
    st[0] = self;
    st[1] = n;
    st[2] = value;
    if (QP_IS_NULL(value))
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __delattr__), st, 2);
    else
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __setattr__), st, 3);
    JS_FreeValue(ctx, n);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static JSValue qp_slot_tp_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other,
                                      int op)
{
    static const int ids[6] = { QP_ATOM___lt__, QP_ATOM___le__, QP_ATOM___eq__,
                                QP_ATOM___ne__, QP_ATOM___gt__, QP_ATOM___ge__ };
    JSValue res = qp_maybe_call_special(ctx, self, QP_STATE(ctx)->atoms[ids[op & 7]], other, NULL);
    if (QP_IS_NULL(res))
        return qp_not_implemented(ctx);
    return res;
}

static JSValue qp_slot_tp_iter(JSContext *ctx, JSValueConst self)
{
    int attr_is_none = 0;
    JSValue res = qp_maybe_call_special(ctx, self, QP_ATOM(ctx, __iter__), QP_NULL,
                                        &attr_is_none), gi;
    if (!QP_IS_NULL(res))
        return res;
    if (!attr_is_none) {
        int r = qp_lookup_method_ex(ctx, self, QP_ATOM(ctx, __getitem__), &gi, 0);
        if (r == -1)
            return JS_EXCEPTION;
        if (r != -2) {
            JS_FreeValue(ctx, gi);
            return qp_seqiter_new(ctx, self);
        }
    }
    {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
        qp_raise_type_error(ctx, "'%.200U' object is not iterable", tn);
        JS_FreeValue(ctx, tn);
    }
    return JS_EXCEPTION;
}

/* tp_iternext contract: QP_NULL when exhausted, StopIteration consumed. */
static JSValue qp_slot_tp_iternext(JSContext *ctx, JSValueConst self)
{
    JSValueConst st[1] = { self };
    JSValue r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __next__), st, 1);
    if (JS_IsException(r) && qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
        qp_tw_clear_exc(ctx);
        return QP_NULL;
    }
    return r;
}

static JSValue qp_slot_tp_descr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                    JSValueConst type)
{
    QPType *tp = qp_type_of(ctx, self);
    JSValue get = qp_type_lookup(ctx, tp, QP_ATOM(ctx, __get__)), r;
    JSValueConst st[3];
    if (QP_IS_NULL(get)) {
        if (tp->slots.tp_descr_get == qp_slot_tp_descr_get)
            tp->slots.tp_descr_get = NULL;
        return JS_DupValue(ctx, self);
    }
    get = JS_DupValue(ctx, get);
    st[0] = self;
    st[1] = QP_IS_NULL(obj) ? JS_UNDEFINED : obj;
    st[2] = QP_IS_NULL(type) ? JS_UNDEFINED : type;
    r = qp_vectorcall(ctx, get, st, 3, QP_NULL);
    JS_FreeValue(ctx, get);
    return r;
}

static int qp_slot_tp_descr_set(JSContext *ctx, JSValueConst self, JSValueConst target,
                                JSValueConst value)
{
    JSValueConst st[3] = { self, target, value };
    JSValue r;
    if (QP_IS_NULL(value))
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __delete__), st, 2);
    else
        r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __set__), st, 3);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int qp_slot_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           size_t nargsf, JSValueConst kwnames)
{
    JSValue meth, r;
    Py_ssize_t nargs = qp_nargs(nargsf);
    int unbound = qp_lookup_method_ex(ctx, self, QP_ATOM(ctx, __init__), &meth, 1);
    if (unbound < 0)
        return -1;
    if (unbound)
        r = qp_call_prepend(ctx, meth, self, args, nargs, kwnames);
    else
        r = qp_vectorcall(ctx, meth, args, nargs, kwnames);
    JS_FreeValue(ctx, meth);
    if (JS_IsException(r))
        return -1;
    if (!JS_IsUndefined(r)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, r));
        qp_raise_type_error(ctx, "__init__() should return None, not '%.200U'", tn);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, r);
        return -1;
    }
    return 0;
}

static JSValue qp_slot_tp_new(JSContext *ctx, QPType *type, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    JSValue func = qp_getattr(ctx, QP_TYPE_VALUE(type), QP_ATOM(ctx, __new__)), r;
    if (JS_IsException(func))
        return func;
    r = qp_call_prepend(ctx, func, QP_TYPE_VALUE(type), args, qp_nargs(nargsf), kwnames);
    JS_FreeValue(ctx, func);
    return r;
}

static JSValue qp_slot_am_generic(JSContext *ctx, JSValueConst self, JSAtom name)
{
    JSValue res = qp_maybe_call_special(ctx, self, name, QP_NULL, NULL);
    if (QP_IS_NULL(res)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
        JSValue n = qp_str_from_atom(ctx, name);
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "object %.50U does not have %U method", tn, n);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, n);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue qp_slot_am_await(JSContext *ctx, JSValueConst self)
{
    return qp_slot_am_generic(ctx, self, QP_ATOM(ctx, __await__));
}

static JSValue qp_slot_am_aiter(JSContext *ctx, JSValueConst self)
{
    return qp_slot_am_generic(ctx, self, QP_ATOM(ctx, __aiter__));
}

static JSValue qp_slot_am_anext(JSContext *ctx, JSValueConst self)
{
    return qp_slot_am_generic(ctx, self, QP_ATOM(ctx, __anext__));
}

/* __buffer__ defined in Python: the returned memoryview exports the buffer. */
static int qp_slot_bf_getbuffer(JSContext *ctx, JSValueConst self, QPBuffer *view, int flags)
{
    JSValue fl = JS_NewInt32(ctx, flags), r;
    JSValueConst st[2] = { self, fl };
    int ret;
    r = qp_vectorcall_method(ctx, QP_ATOM(ctx, __buffer__), st, 2);
    if (JS_IsException(r))
        return -1;
    if (!qp_typecheck(ctx, r, QP_TYPE(ctx, memoryview))) {
        JS_FreeValue(ctx, r);
        qp_raise_type_error(ctx, "__buffer__ returned non-memoryview object");
        return -1;
    }
    ret = qp_buffer_get(ctx, r, view, flags);
    JS_FreeValue(ctx, r);
    return ret;
}

/* ========================================================================= */
/* slotdefs                                                                  */
/* ========================================================================= */

#define QP_SO(f) ((int)offsetof(QPTypeSlots, f))
#define QP_TPSLOT(NAME, SLOT, FUNC, WRAP, DOC) \
    { #NAME, QP_ATOM_##NAME, QP_SO(SLOT), (void *)(FUNC), WRAP, DOC, 0 }
#define QP_FLSLOT(NAME, SLOT, FUNC, WRAP, DOC, FLAGS) \
    { #NAME, QP_ATOM_##NAME, QP_SO(SLOT), (void *)(FUNC), WRAP, DOC, FLAGS }
#define QP_UNSLOT(NAME, SLOT, FUNC, WRAP, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, WRAP, #NAME "($self, /)\n--\n\n" DOC)
#define QP_IBSLOT(NAME, SLOT, FUNC, WRAP, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, WRAP, #NAME "($self, value, /)\n--\n\nReturn self" DOC "value.")
#define QP_BINSLOT(NAME, SLOT, FUNC, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, qp_wrap_binaryfunc_l, #NAME "($self, value, /)\n--\n\nReturn self" DOC "value.")
#define QP_RBINSLOT(NAME, SLOT, FUNC, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, qp_wrap_binaryfunc_r, #NAME "($self, value, /)\n--\n\nReturn value" DOC "self.")
#define QP_BINSLOTNOTINFIX(NAME, SLOT, FUNC, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, qp_wrap_binaryfunc_l, #NAME "($self, value, /)\n--\n\n" DOC)
#define QP_RBINSLOTNOTINFIX(NAME, SLOT, FUNC, DOC) \
    QP_TPSLOT(NAME, nb.SLOT, FUNC, qp_wrap_binaryfunc_r, #NAME "($self, value, /)\n--\n\n" DOC)

/* Same order as CPython (entries for one slot are adjacent; as_mapping
   before as_sequence so that mp_subscript wins for __getitem__). */
static const QPSlotDef qp_slotdefs[] = {
    QP_TPSLOT(__repr__, tp_repr, qp_slot_tp_repr, qp_wrap_unaryfunc,
              "__repr__($self, /)\n--\n\nReturn repr(self)."),
    QP_TPSLOT(__hash__, tp_hash, qp_slot_tp_hash, qp_wrap_hashfunc,
              "__hash__($self, /)\n--\n\nReturn hash(self)."),
    QP_FLSLOT(__call__, tp_call, qp_slot_tp_call, qp_wrap_call,
              "__call__($self, /, *args, **kwargs)\n--\n\nCall self as a function.",
              QP_SLOTDEF_KEYWORDS),
    QP_TPSLOT(__str__, tp_str, qp_slot_tp_str, qp_wrap_unaryfunc,
              "__str__($self, /)\n--\n\nReturn str(self)."),
    QP_TPSLOT(__getattribute__, tp_getattro, qp_slot_tp_getattr_hook, qp_wrap_getattro,
              "__getattribute__($self, name, /)\n--\n\nReturn getattr(self, name)."),
    QP_TPSLOT(__getattr__, tp_getattro, qp_slot_tp_getattr_hook, NULL,
              "__getattr__($self, name, /)\n--\n\nImplement getattr(self, name)."),
    QP_TPSLOT(__setattr__, tp_setattro, qp_slot_tp_setattro, qp_wrap_setattr,
              "__setattr__($self, name, value, /)\n--\n\nImplement setattr(self, name, value)."),
    QP_TPSLOT(__delattr__, tp_setattro, qp_slot_tp_setattro, qp_wrap_delattr,
              "__delattr__($self, name, /)\n--\n\nImplement delattr(self, name)."),
    QP_TPSLOT(__lt__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_lt,
              "__lt__($self, value, /)\n--\n\nReturn self<value."),
    QP_TPSLOT(__le__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_le,
              "__le__($self, value, /)\n--\n\nReturn self<=value."),
    QP_TPSLOT(__eq__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_eq,
              "__eq__($self, value, /)\n--\n\nReturn self==value."),
    QP_TPSLOT(__ne__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_ne,
              "__ne__($self, value, /)\n--\n\nReturn self!=value."),
    QP_TPSLOT(__gt__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_gt,
              "__gt__($self, value, /)\n--\n\nReturn self>value."),
    QP_TPSLOT(__ge__, tp_richcompare, qp_slot_tp_richcompare, qp_richcmp_ge,
              "__ge__($self, value, /)\n--\n\nReturn self>=value."),
    QP_TPSLOT(__iter__, tp_iter, qp_slot_tp_iter, qp_wrap_unaryfunc,
              "__iter__($self, /)\n--\n\nImplement iter(self)."),
    QP_TPSLOT(__next__, tp_iternext, qp_slot_tp_iternext, qp_wrap_next,
              "__next__($self, /)\n--\n\nImplement next(self)."),
    QP_TPSLOT(__get__, tp_descr_get, qp_slot_tp_descr_get, qp_wrap_descr_get,
              "__get__($self, instance, owner=None, /)\n--\n\nReturn an attribute of instance, which is of type owner."),
    QP_TPSLOT(__set__, tp_descr_set, qp_slot_tp_descr_set, qp_wrap_descr_set,
              "__set__($self, instance, value, /)\n--\n\nSet an attribute of instance to value."),
    QP_TPSLOT(__delete__, tp_descr_set, qp_slot_tp_descr_set, qp_wrap_descr_delete,
              "__delete__($self, instance, /)\n--\n\nDelete an attribute of instance."),
    QP_FLSLOT(__init__, tp_init, qp_slot_tp_init, qp_wrap_init,
              "__init__($self, /, *args, **kwargs)\n--\n\n"
              "Initialize self.  See help(type(self)) for accurate signature.",
              QP_SLOTDEF_KEYWORDS),
    QP_TPSLOT(__new__, tp_new, qp_slot_tp_new, NULL,
              "__new__(type, /, *args, **kwargs)\n--\n\n"
              "Create and return new object.  See help(type) for accurate signature."),

    QP_TPSLOT(__buffer__, bf.bf_getbuffer, qp_slot_bf_getbuffer, qp_wrap_buffer,
              "__buffer__($self, flags, /)\n--\n\n"
              "Return a buffer object that exposes the underlying memory of the object."),
    /* no Python-level dispatcher: the memoryview handed to __release_buffer__
       is owned by the exporter's caller in this implementation */
    QP_TPSLOT(__release_buffer__, bf.bf_releasebuffer, NULL, qp_wrap_releasebuffer,
              "__release_buffer__($self, buffer, /)\n--\n\n"
              "Release the buffer object that exposes the underlying memory of the object."),

    QP_TPSLOT(__await__, am.am_await, qp_slot_am_await, qp_wrap_unaryfunc,
              "__await__($self, /)\n--\n\nReturn an iterator to be used in await expression."),
    QP_TPSLOT(__aiter__, am.am_aiter, qp_slot_am_aiter, qp_wrap_unaryfunc,
              "__aiter__($self, /)\n--\n\nReturn an awaitable, that resolves in asynchronous iterator."),
    QP_TPSLOT(__anext__, am.am_anext, qp_slot_am_anext, qp_wrap_unaryfunc,
              "__anext__($self, /)\n--\n\nReturn a value or raise StopAsyncIteration."),

    QP_BINSLOT(__add__, nb_add, qp_slot_nb_add, "+"),
    QP_RBINSLOT(__radd__, nb_add, qp_slot_nb_add, "+"),
    QP_BINSLOT(__sub__, nb_subtract, qp_slot_nb_subtract, "-"),
    QP_RBINSLOT(__rsub__, nb_subtract, qp_slot_nb_subtract, "-"),
    QP_BINSLOT(__mul__, nb_multiply, qp_slot_nb_multiply, "*"),
    QP_RBINSLOT(__rmul__, nb_multiply, qp_slot_nb_multiply, "*"),
    QP_BINSLOT(__mod__, nb_remainder, qp_slot_nb_remainder, "%"),
    QP_RBINSLOT(__rmod__, nb_remainder, qp_slot_nb_remainder, "%"),
    QP_BINSLOTNOTINFIX(__divmod__, nb_divmod, qp_slot_nb_divmod, "Return divmod(self, value)."),
    QP_RBINSLOTNOTINFIX(__rdivmod__, nb_divmod, qp_slot_nb_divmod, "Return divmod(value, self)."),
    QP_TPSLOT(__pow__, nb.nb_power, qp_slot_nb_power, qp_wrap_ternaryfunc,
              "__pow__($self, value, mod=None, /)\n--\n\nReturn pow(self, value, mod)."),
    QP_TPSLOT(__rpow__, nb.nb_power, qp_slot_nb_power, qp_wrap_ternaryfunc_r,
              "__rpow__($self, value, mod=None, /)\n--\n\nReturn pow(value, self, mod)."),
    QP_UNSLOT(__neg__, nb_negative, qp_slot_nb_negative, qp_wrap_unaryfunc, "-self"),
    QP_UNSLOT(__pos__, nb_positive, qp_slot_nb_positive, qp_wrap_unaryfunc, "+self"),
    QP_UNSLOT(__abs__, nb_absolute, qp_slot_nb_absolute, qp_wrap_unaryfunc, "abs(self)"),
    QP_UNSLOT(__bool__, nb_bool, qp_slot_nb_bool, qp_wrap_inquirypred, "True if self else False"),
    QP_UNSLOT(__invert__, nb_invert, qp_slot_nb_invert, qp_wrap_unaryfunc, "~self"),
    QP_BINSLOT(__lshift__, nb_lshift, qp_slot_nb_lshift, "<<"),
    QP_RBINSLOT(__rlshift__, nb_lshift, qp_slot_nb_lshift, "<<"),
    QP_BINSLOT(__rshift__, nb_rshift, qp_slot_nb_rshift, ">>"),
    QP_RBINSLOT(__rrshift__, nb_rshift, qp_slot_nb_rshift, ">>"),
    QP_BINSLOT(__and__, nb_and, qp_slot_nb_and, "&"),
    QP_RBINSLOT(__rand__, nb_and, qp_slot_nb_and, "&"),
    QP_BINSLOT(__xor__, nb_xor, qp_slot_nb_xor, "^"),
    QP_RBINSLOT(__rxor__, nb_xor, qp_slot_nb_xor, "^"),
    QP_BINSLOT(__or__, nb_or, qp_slot_nb_or, "|"),
    QP_RBINSLOT(__ror__, nb_or, qp_slot_nb_or, "|"),
    QP_UNSLOT(__int__, nb_int, qp_slot_nb_int, qp_wrap_unaryfunc, "int(self)"),
    QP_UNSLOT(__float__, nb_float, qp_slot_nb_float, qp_wrap_unaryfunc, "float(self)"),
    QP_IBSLOT(__iadd__, nb_inplace_add, qp_slot_nb_inplace_add, qp_wrap_binaryfunc, "+="),
    QP_IBSLOT(__isub__, nb_inplace_subtract, qp_slot_nb_inplace_subtract, qp_wrap_binaryfunc, "-="),
    QP_IBSLOT(__imul__, nb_inplace_multiply, qp_slot_nb_inplace_multiply, qp_wrap_binaryfunc, "*="),
    QP_IBSLOT(__imod__, nb_inplace_remainder, qp_slot_nb_inplace_remainder, qp_wrap_binaryfunc, "%="),
    QP_IBSLOT(__ipow__, nb_inplace_power, qp_slot_nb_inplace_power, qp_wrap_ternaryfunc, "**="),
    QP_IBSLOT(__ilshift__, nb_inplace_lshift, qp_slot_nb_inplace_lshift, qp_wrap_binaryfunc, "<<="),
    QP_IBSLOT(__irshift__, nb_inplace_rshift, qp_slot_nb_inplace_rshift, qp_wrap_binaryfunc, ">>="),
    QP_IBSLOT(__iand__, nb_inplace_and, qp_slot_nb_inplace_and, qp_wrap_binaryfunc, "&="),
    QP_IBSLOT(__ixor__, nb_inplace_xor, qp_slot_nb_inplace_xor, qp_wrap_binaryfunc, "^="),
    QP_IBSLOT(__ior__, nb_inplace_or, qp_slot_nb_inplace_or, qp_wrap_binaryfunc, "|="),
    QP_BINSLOT(__floordiv__, nb_floor_divide, qp_slot_nb_floor_divide, "//"),
    QP_RBINSLOT(__rfloordiv__, nb_floor_divide, qp_slot_nb_floor_divide, "//"),
    QP_BINSLOT(__truediv__, nb_true_divide, qp_slot_nb_true_divide, "/"),
    QP_RBINSLOT(__rtruediv__, nb_true_divide, qp_slot_nb_true_divide, "/"),
    QP_IBSLOT(__ifloordiv__, nb_inplace_floor_divide, qp_slot_nb_inplace_floor_divide,
              qp_wrap_binaryfunc, "//="),
    QP_IBSLOT(__itruediv__, nb_inplace_true_divide, qp_slot_nb_inplace_true_divide,
              qp_wrap_binaryfunc, "/="),
    QP_TPSLOT(__index__, nb.nb_index, qp_slot_nb_index, qp_wrap_unaryfunc,
              "__index__($self, /)\n--\n\n"
              "Return self converted to an integer, if self is suitable "
              "for use as an index into a list."),
    QP_BINSLOT(__matmul__, nb_matrix_multiply, qp_slot_nb_matrix_multiply, "@"),
    QP_RBINSLOT(__rmatmul__, nb_matrix_multiply, qp_slot_nb_matrix_multiply, "@"),
    QP_IBSLOT(__imatmul__, nb_inplace_matrix_multiply, qp_slot_nb_inplace_matrix_multiply,
              qp_wrap_binaryfunc, "@="),
    QP_TPSLOT(__len__, mp.mp_length, qp_slot_mp_length, qp_wrap_lenfunc,
              "__len__($self, /)\n--\n\nReturn len(self)."),
    QP_TPSLOT(__getitem__, mp.mp_subscript, qp_slot_mp_subscript, qp_wrap_binaryfunc,
              "__getitem__($self, key, /)\n--\n\nReturn self[key]."),
    QP_TPSLOT(__setitem__, mp.mp_ass_subscript, qp_slot_mp_ass_subscript, qp_wrap_objobjargproc,
              "__setitem__($self, key, value, /)\n--\n\nSet self[key] to value."),
    QP_TPSLOT(__delitem__, mp.mp_ass_subscript, qp_slot_mp_ass_subscript, qp_wrap_delitem,
              "__delitem__($self, key, /)\n--\n\nDelete self[key]."),

    QP_TPSLOT(__len__, sq.sq_length, qp_slot_sq_length, qp_wrap_lenfunc,
              "__len__($self, /)\n--\n\nReturn len(self)."),
    QP_TPSLOT(__add__, sq.sq_concat, NULL, qp_wrap_binaryfunc,
              "__add__($self, value, /)\n--\n\nReturn self+value."),
    QP_TPSLOT(__mul__, sq.sq_repeat, NULL, qp_wrap_indexargfunc,
              "__mul__($self, value, /)\n--\n\nReturn self*value."),
    QP_TPSLOT(__rmul__, sq.sq_repeat, NULL, qp_wrap_indexargfunc,
              "__rmul__($self, value, /)\n--\n\nReturn value*self."),
    QP_TPSLOT(__getitem__, sq.sq_item, qp_slot_sq_item, qp_wrap_sq_item,
              "__getitem__($self, key, /)\n--\n\nReturn self[key]."),
    QP_TPSLOT(__setitem__, sq.sq_ass_item, qp_slot_sq_ass_item, qp_wrap_sq_setitem,
              "__setitem__($self, key, value, /)\n--\n\nSet self[key] to value."),
    QP_TPSLOT(__delitem__, sq.sq_ass_item, qp_slot_sq_ass_item, qp_wrap_sq_delitem,
              "__delitem__($self, key, /)\n--\n\nDelete self[key]."),
    QP_TPSLOT(__contains__, sq.sq_contains, qp_slot_sq_contains, qp_wrap_objobjproc,
              "__contains__($self, key, /)\n--\n\nReturn bool(key in self)."),
    QP_TPSLOT(__iadd__, sq.sq_inplace_concat, NULL, qp_wrap_binaryfunc,
              "__iadd__($self, value, /)\n--\n\nImplement self+=value."),
    QP_TPSLOT(__imul__, sq.sq_inplace_repeat, NULL, qp_wrap_indexargfunc,
              "__imul__($self, value, /)\n--\n\nImplement self*=value."),
    { NULL, 0, -1, NULL, NULL, NULL, 0 }
};

#define QP_NSLOTDEFS ((int)countof(qp_slotdefs) - 1)
#define QP_MAX_EQUIV 10

/* slotdefs_dups[i] = { count, indices of the slotdefs with the same name } */
static uint8_t qp_slotdefs_dups[countof(qp_slotdefs)][1 + QP_MAX_EQUIV];
static BOOL qp_slotdefs_ready;

static void qp_slotdefs_init(void)
{
    int i, j;
    if (qp_slotdefs_ready)
        return;
    for (i = 0; i < QP_NSLOTDEFS; i++) {
        uint8_t n = 0;
        for (j = 0; j < QP_NSLOTDEFS && n < QP_MAX_EQUIV; j++) {
            if (qp_slotdefs[j].atom_id == qp_slotdefs[i].atom_id)
                qp_slotdefs_dups[i][1 + n++] = (uint8_t)j;
        }
        qp_slotdefs_dups[i][0] = n;
    }
    qp_slotdefs_ready = TRUE;
}

static inline void **qp_slotptr(QPType *t, int offset)
{
    return (void **)((char *)&t->slots + offset);
}

static int qp_is_slot_name(JSContext *ctx, JSAtom name)
{
    const QPSlotDef *p;
    for (p = qp_slotdefs; p->name; p++) {
        if (QP_STATE(ctx)->atoms[p->atom_id] == name)
            return 1;
    }
    return 0;
}

/* Is v the builtin __new__ (tp_new_wrapper)? */
static BOOL qp_is_tp_new_wrapper(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(v);
    return p->class_id == JS_CLASS_PY_CFUNCTION && p->u.opaque &&
        ((QPCFunctionObject *)p->u.opaque)->ml == &qp_tp_new_methoddef;
}

/* update_one_slot: recompute the slot of the group starting at p; returns
   the first slotdef of the next group. */
static const QPSlotDef *qp_update_one_slot(JSContext *ctx, QPType *t, const QPSlotDef *p)
{
    void *specific = NULL, *generic = NULL;
    int use_generic = 0, offset = p->offset;
    void **ptr = qp_slotptr(t, offset);

    do {
        JSValue descr = qp_find_name_in_mro(ctx, t, QP_STATE(ctx)->atoms[p->atom_id]);
        QPDescr *d;
        if (QP_IS_NULL(descr))
            continue;
        d = qp_descr_get(descr);
        if (d && d->kind == QP_DESCR_WRAPPER && d->u.wrapper.slot->atom_id == p->atom_id) {
            void **tptr;
            int index = (int)(p - qp_slotdefs), i;
            if (qp_slotdefs_dups[index][0] > 1) {
                tptr = NULL;
                for (i = 1; i <= qp_slotdefs_dups[index][0]; i++) {
                    const QPSlotDef *q = &qp_slotdefs[qp_slotdefs_dups[index][i]];
                    void **qptr = qp_slotptr(t, q->offset);
                    if (*qptr == NULL)
                        continue;
                    if (tptr != NULL) {
                        tptr = NULL;
                        break;
                    }
                    tptr = qptr;
                }
            } else {
                tptr = ptr;
            }
            if (tptr == NULL || tptr == ptr)
                generic = p->function;
            if ((specific == NULL || specific == d->u.wrapper.wrapped) &&
                d->u.wrapper.slot->wrapper == p->wrapper &&
                qp_type_is_subtype(t, qp_type_from_value(d->objclass))) {
                specific = d->u.wrapper.wrapped;
            } else {
                use_generic = 1;
            }
        } else if (qp_is_tp_new_wrapper(descr) && ptr == (void **)&t->slots.tp_new) {
            specific = (void *)t->slots.tp_new;
        } else if (JS_IsUndefined(descr) && ptr == (void **)&t->slots.tp_hash) {
            specific = (void *)qp_hash_not_implemented;
        } else {
            use_generic = 1;
            generic = p->function;
        }
    } while ((++p)->offset == offset);

    *ptr = (specific && !use_generic) ? specific : generic;
    return p;
}

/* Non-slotdef state derived from the dunders of a heap type: __del__ flag
   and the virtual-iteration slot. */
static void qp_fixup_derived(JSContext *ctx, QPType *t)
{
    Py_ssize_t i;
    BOOL has_del = !QP_IS_NULL(qp_find_name_in_mro(ctx, t, QP_ATOM(ctx, __del__)));
    if (!has_del && JS_VALUE_GET_TAG(t->mro) == JS_TAG_OBJECT) {
        for (i = 1; i < QP_TUPLE_SIZE(t->mro); i++) {
            QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(t->mro)[i]);
            if (b && !(b->flags & QP_TPFLAGS_HEAPTYPE) && (b->flags & QP_TPFLAGS_HAS_DEL))
                has_del = TRUE;
        }
    }
    if (has_del)
        t->flags |= QP_TPFLAGS_HAS_DEL;
    else
        t->flags &= ~(uint64_t)QP_TPFLAGS_HAS_DEL;
    if (t->base && t->slots.tp_iter == t->base->slots.tp_iter &&
        t->slots.sq.sq_item == t->base->slots.sq.sq_item &&
        t->slots.mp.mp_subscript == t->base->slots.mp.mp_subscript &&
        t->slots.sq.sq_length == t->base->slots.sq.sq_length &&
        t->slots.mp.mp_length == t->base->slots.mp.mp_length)
        t->slots.tp_iteritem = t->base->slots.tp_iteritem;
    else
        t->slots.tp_iteritem = NULL;
}

/* fixup_slot_dispatchers */
static int qp_fixup_slot_dispatchers(JSContext *ctx, QPType *t)
{
    const QPSlotDef *p = qp_slotdefs;
    while (p->name)
        p = qp_update_one_slot(ctx, t, p);
    qp_fixup_derived(ctx, t);
    return 0;
}

static void qp_update_slot_rec(JSContext *ctx, QPType *t, JSAtom name, const int *heads, int n)
{
    int i;
    for (i = 0; i < n; i++)
        qp_update_one_slot(ctx, t, &qp_slotdefs[heads[i]]);
    qp_fixup_derived(ctx, t);
    for (i = 0; i < t->subclass_count; i++) {
        QPType *sub = t->subclasses[i];
        JSValue tmp;
        if (JS_VALUE_GET_TAG(sub->dict) == JS_TAG_OBJECT) {
            int r = qp_dict_get_item_atom(ctx, sub->dict, name, &tmp);
            if (r > 0) {
                JS_FreeValue(ctx, tmp);
                continue;
            }
            if (r < 0)
                qp_tw_clear_exc(ctx);
        }
        qp_update_slot_rec(ctx, sub, name, heads, n);
    }
}

/* update_slot: after setting/deleting dunder `name` on heap type t. */
static int qp_update_slot(JSContext *ctx, QPType *t, JSAtom name)
{
    int heads[QP_MAX_EQUIV], n = 0, i, j;
    const QPSlotDef *p;

    for (p = qp_slotdefs; p->name; p++) {
        if (QP_STATE(ctx)->atoms[p->atom_id] != name)
            continue;
        /* head of the group of this offset */
        for (i = 0; qp_slotdefs[i].offset != p->offset; i++)
            ;
        for (j = 0; j < n; j++)
            if (heads[j] == i)
                break;
        if (j == n && n < QP_MAX_EQUIV)
            heads[n++] = i;
    }
    if (n == 0) {
        if (name == QP_ATOM(ctx, __del__))
            qp_update_slot_rec(ctx, t, name, heads, 0);
        return 0;
    }
    qp_update_slot_rec(ctx, t, name, heads, n);
    return 0;
}

/* add_operators: slot wrappers for the C slots defined by a builtin type. */
static int qp_add_operators(JSContext *ctx, QPType *t)
{
    const QPSlotDef *p;
    for (p = qp_slotdefs; p->name; p++) {
        void **ptr;
        JSValue descr, tmp;
        JSAtom name;
        int r;
        if (p->wrapper == NULL)
            continue;
        ptr = qp_slotptr(t, p->offset);
        if (!*ptr)
            continue;
        /* ignore when the slot has been inherited */
        if (!(t->flags & QP_TPFLAGS_HEAPTYPE) && t->base) {
            void **bptr = qp_slotptr(t->base, p->offset);
            if (*bptr == *ptr) {
                if (ptr == (void **)&t->slots.tp_hash) {
                    if (t->slots.tp_richcompare == t->base->slots.tp_richcompare)
                        continue;
                } else if (ptr == (void **)&t->slots.tp_richcompare) {
                    if (t->slots.tp_hash == t->base->slots.tp_hash)
                        continue;
                } else {
                    continue;
                }
            }
        }
        name = QP_STATE(ctx)->atoms[p->atom_id];
        r = qp_dict_get_item_atom(ctx, t->dict, name, &tmp);
        if (r < 0)
            return -1;
        if (r > 0) {
            JS_FreeValue(ctx, tmp);
            continue;
        }
        if (*ptr == (void *)qp_hash_not_implemented) {
            if (qp_dict_set_item_atom(ctx, t->dict, name, JS_UNDEFINED) < 0)
                return -1;
        } else {
            descr = qp_descr_new_wrapper(ctx, t, p, *ptr);
            if (JS_IsException(descr))
                return -1;
            r = qp_dict_set_item_atom(ctx, t->dict, name, descr);
            JS_FreeValue(ctx, descr);
            if (r < 0)
                return -1;
        }
    }
    return 0;
}

/* PyObject_HashNotImplemented */
static Py_hash_t qp_hash_not_implemented(JSContext *ctx, JSValueConst v)
{
    JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, v));
    qp_raise_type_error(ctx, "unhashable type: '%.200U'", tn);
    JS_FreeValue(ctx, tn);
    return -1;
}

/* tp_new_wrapper: T.__new__(S, *args, **kwargs). */
static JSValue qp_tp_new_wrapper(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPType *type = qp_type_from_value(self), *subtype, *staticbase;
    JSValue tn, sn, bn;
    if (!type)
        return qp_raise_system_error(ctx, "__new__() called with non-type 'self'");
    if (nargs < 1) {
        tn = qp_type_tp_name(ctx, type);
        qp_raise_type_error(ctx, "%U.__new__(): not enough arguments", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    subtype = qp_type_from_value(args[0]);
    if (!subtype) {
        tn = qp_type_tp_name(ctx, type);
        sn = qp_type_tp_name(ctx, qp_type_of(ctx, args[0]));
        qp_raise_type_error(ctx, "%U.__new__(X): X is not a type object (%U)", tn, sn);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, sn);
        return JS_EXCEPTION;
    }
    if (!qp_type_is_subtype(subtype, type)) {
        tn = qp_type_tp_name(ctx, type);
        sn = qp_type_tp_name(ctx, subtype);
        qp_raise_type_error(ctx, "%U.__new__(%U): %U is not a subtype of %U", tn, sn, sn, tn);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, sn);
        return JS_EXCEPTION;
    }
    staticbase = subtype;
    while (staticbase && staticbase->slots.tp_new == qp_slot_tp_new)
        staticbase = staticbase->base;
    if (staticbase && staticbase->slots.tp_new != type->slots.tp_new) {
        sn = qp_type_tp_name(ctx, subtype);
        if (staticbase->slots.tp_new == NULL) {
            qp_raise_type_error(ctx, "cannot create '%U' instances", sn);
        } else {
            tn = qp_type_tp_name(ctx, type);
            bn = qp_type_tp_name(ctx, staticbase);
            qp_raise_type_error(ctx, "%U.__new__(%U) is not safe, use %U.__new__()", tn, sn, bn);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, bn);
        }
        JS_FreeValue(ctx, sn);
        return JS_EXCEPTION;
    }
    return type->slots.tp_new(ctx, subtype, args + 1, nargs - 1, kwnames);
}

static const QPMethodDef qp_tp_new_methoddef = {
    "__new__", qp_tp_new_wrapper, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
    "__new__($type, *args, **kwargs)\n--\n\n"
    "Create and return a new object.  See help(type) for accurate signature."
};
