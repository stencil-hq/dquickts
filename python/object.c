/*
 * dquickts Python: generic object protocol (object.h section (h), object.c
 * half).
 *
 * Derived from CPython 3.15 Objects/object.c (PSF-2.0): PyObject_GetAttr,
 * PyObject_GetOptionalAttr, PyObject_SetAttr, PyObject_Repr, PyObject_Str,
 * PyObject_ASCII, PyObject_Hash, PyObject_IsTrue, do_richcompare,
 * PyObject_RichCompare(Bool), PyObject_Dir, Py_ReprEnter/Py_ReprLeave,
 * _PyObject_SetAttributeErrorContext; and Objects/call.c
 * PyObject_VectorcallMethod.
 *
 * Type slots, the generic getattr/setattr, method lookup and the type
 * objects themselves live in typeobject.c (M2TypeObj).
 */

/* Helpers shared with abstract.c (forward-declared there as well). */
static const char *qp_obj_tpname(JSContext *ctx, QPType *t, char *buf, int size);
static void qp_obj_clear_exc(JSContext *ctx);
static int qp_obj_enter_recursive(JSContext *ctx);

#define QP_TPNAME_BUF 256

/* CPython tp_name: the spec name of builtin types ("int",
   "decimal.Decimal"), __name__ of heap types.  UTF-8, valid while buf
   lives. */
static const char *qp_obj_tpname(JSContext *ctx, QPType *t, char *buf, int size)
{
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* PyErr_Clear for the pending exception. */
static void qp_obj_clear_exc(JSContext *ctx)
{
    JSValue e = qp_exc_fetch(ctx);
    JS_FreeValue(ctx, e);
}

/* _Py_EnterRecursiveCall: C-stack guard of the native recursion points
   (repr, comparison, isinstance).  -1 with RecursionError pending. */
static int qp_obj_enter_recursive(JSContext *ctx)
{
    if (js_check_stack_overflow(ctx->rt, 0)) {
        qp_raise_recursion_error(ctx);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Attributes                                                                */
/* ------------------------------------------------------------------------ */

/* tp_getattro dispatch without the AttributeError augmentation. */
static JSValue qp_obj_getattr_raw(JSContext *ctx, JSValueConst v, JSAtom name)
{
    QPType *tp = qp_type_of(ctx, v);
    if (tp->slots.tp_getattro)
        return tp->slots.tp_getattro(ctx, v, name);
    return qp_raise_attribute_error(ctx, v, name);
}

/* _PyObject_SetAttributeErrorContext: give a pending AttributeError that
   has neither `name` nor `obj` set the failing name and object (used for
   the "Did you mean" suggestions of tracebacks). */
static void qp_obj_set_attribute_error_context(JSContext *ctx, JSValueConst v, JSAtom name)
{
    JSValue exc, cur, s;
    int augmented;

    if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)))
        return;
    exc = qp_exc_fetch(ctx);
    if (!qp_typecheck(ctx, exc, QP_EXC(ctx, AttributeError)))
        goto restore;
    augmented = 0;
    if (qp_getattr_opt(ctx, exc, QP_ATOM(ctx, name), &cur) < 0) {
        qp_obj_clear_exc(ctx);
        goto restore;
    }
    if (!QP_IS_NULL(cur) && !JS_IsUndefined(cur) && !JS_IsNull(cur))
        augmented = 1;
    JS_FreeValue(ctx, cur);
    if (!augmented) {
        if (qp_getattr_opt(ctx, exc, QP_ATOM(ctx, obj), &cur) < 0) {
            qp_obj_clear_exc(ctx);
            goto restore;
        }
        if (!QP_IS_NULL(cur) && !JS_IsUndefined(cur) && !JS_IsNull(cur))
            augmented = 1;
        JS_FreeValue(ctx, cur);
    }
    if (!augmented) {
        s = qp_str_from_atom(ctx, name);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, exc);
            return;
        }
        if (qp_setattr(ctx, exc, QP_ATOM(ctx, name), s) < 0 ||
            qp_setattr(ctx, exc, QP_ATOM(ctx, obj), v) < 0) {
            /* like CPython: the setattr error replaces the original */
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, exc);
            return;
        }
        JS_FreeValue(ctx, s);
    }
 restore:
    JS_Throw(ctx, exc);
}

static JSValue qp_getattr(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    JSValue res = qp_obj_getattr_raw(ctx, obj, name);
    if (JS_IsException(res))
        qp_obj_set_attribute_error_context(ctx, obj, name);
    return res;
}

static int qp_getattr_opt(JSContext *ctx, JSValueConst obj, JSAtom name, JSValue *pres)
{
    JSValue res = qp_obj_getattr_raw(ctx, obj, name);
    if (JS_IsException(res)) {
        *pres = QP_NULL;
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
            qp_obj_clear_exc(ctx);
            return 0;
        }
        return -1;
    }
    if (QP_IS_NULL(res)) {
        /* defensive: a getattro that reports absence without raising */
        *pres = QP_NULL;
        return 0;
    }
    *pres = res;
    return 1;
}

/* PyObject_SetAttr / PyObject_DelAttr (v == QP_NULL deletes). */
static int qp_obj_setattr_impl(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPType *tp = qp_type_of(ctx, obj);
    char buf[QP_TPNAME_BUF];
    JSValue s;

    if (tp->slots.tp_setattro)
        return tp->slots.tp_setattro(ctx, obj, name, v);
    s = qp_str_from_atom(ctx, name);
    if (JS_IsException(s))
        return -1;
    if (tp->slots.tp_getattro == NULL)
        qp_raise_type_error(ctx, "'%.100s' object has no attributes (%s .%U)",
                            qp_obj_tpname(ctx, tp, buf, sizeof(buf)),
                            QP_IS_NULL(v) ? "del" : "assign to", s);
    else
        qp_raise_type_error(ctx, "'%.100s' object has only read-only attributes (%s .%U)",
                            qp_obj_tpname(ctx, tp, buf, sizeof(buf)),
                            QP_IS_NULL(v) ? "del" : "assign to", s);
    JS_FreeValue(ctx, s);
    return -1;
}

static int qp_setattr(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    return qp_obj_setattr_impl(ctx, obj, name, v);
}

static int qp_delattr(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    return qp_obj_setattr_impl(ctx, obj, name, QP_NULL);
}

/* PyObject_VectorcallMethod: obj.name(*args, **kw) through load_method so
   that unbound methods are called with obj prepended, without creating a
   bound method. */
static JSValue qp_call_method(JSContext *ctx, JSValueConst obj, JSAtom name,
                              JSValueConst *args, size_t nargsf, JSValueConst kwnames)
{
    JSValue meth = QP_NULL, self = QP_NULL, res;
    Py_ssize_t nargs = qp_nargs(nargsf);
    Py_ssize_t nkw = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames);
    Py_ssize_t total = nargs + nkw, i;

    if (qp_load_method(ctx, obj, name, &meth, &self) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(self)) {
        res = qp_vectorcall(ctx, meth, args, nargsf, kwnames);
    } else if (nargsf & QP_VECTORCALL_ARGUMENTS_OFFSET) {
        /* args[-1] is scratch: prepend self in place */
        JSValue *a = (JSValue *)args - 1;
        JSValue saved = a[0];
        a[0] = self;
        res = qp_vectorcall(ctx, meth, a, (size_t)(nargs + 1), kwnames);
        a[0] = saved;
    } else {
        JSValue stack_buf[9];
        JSValue *a = stack_buf;
        if (total + 2 > (Py_ssize_t)countof(stack_buf)) {
            a = js_malloc(ctx, sizeof(JSValue) * (total + 2));
            if (!a) {
                JS_FreeValue(ctx, meth);
                JS_FreeValue(ctx, self);
                return JS_EXCEPTION;
            }
        }
        /* a[0] is left as scratch so the callee may use args[-1] */
        a[0] = JS_UNDEFINED;
        a[1] = self;
        for (i = 0; i < total; i++)
            a[i + 2] = args[i];
        res = qp_vectorcall(ctx, meth, a + 1,
                            (size_t)(nargs + 1) | QP_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
        if (a != stack_buf)
            js_free(ctx, a);
    }
    JS_FreeValue(ctx, meth);
    JS_FreeValue(ctx, self);
    return res;
}

/* ------------------------------------------------------------------------ */
/* repr / str / ascii                                                        */
/* ------------------------------------------------------------------------ */

static JSValue qp_repr(JSContext *ctx, JSValueConst v)
{
    QPType *tp = qp_type_of(ctx, v);
    JSValue res;

    if (tp->slots.tp_repr == NULL) {
        char buf[QP_TPNAME_BUF];
        const void *ptr = JS_VALUE_HAS_REF_COUNT(v) ? JS_VALUE_GET_PTR(v) : NULL;
        return qp_str_from_format(ctx, "<%s object at %p>",
                                  qp_obj_tpname(ctx, tp, buf, sizeof(buf)), ptr);
    }
    if (qp_obj_enter_recursive(ctx) < 0)
        return JS_EXCEPTION;
    res = tp->slots.tp_repr(ctx, v);
    if (JS_IsException(res))
        return res;
    if (!qp_str_check(ctx, res)) {
        qp_raise_type_error(ctx, "%T.__repr__() must return a str, not %T", v, res);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue qp_str(JSContext *ctx, JSValueConst v)
{
    QPType *tp;
    JSValue res;

    if (qp_str_check_exact(v))
        return JS_DupValue(ctx, v);
    tp = qp_type_of(ctx, v);
    if (tp->slots.tp_str == NULL)
        return qp_repr(ctx, v);
    if (qp_obj_enter_recursive(ctx) < 0)
        return JS_EXCEPTION;
    res = tp->slots.tp_str(ctx, v);
    if (JS_IsException(res))
        return res;
    if (!qp_str_check(ctx, res)) {
        qp_raise_type_error(ctx, "%T.__str__() must return a str, not %T", v, res);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue qp_ascii(JSContext *ctx, JSValueConst v)
{
    JSValue repr = qp_repr(ctx, v), res;
    if (JS_IsException(repr))
        return repr;
    res = qp_str_ascii_escape(ctx, repr);
    JS_FreeValue(ctx, repr);
    return res;
}

/* ------------------------------------------------------------------------ */
/* hash / truth / identity                                                   */
/* ------------------------------------------------------------------------ */

static Py_hash_t qp_hash(JSContext *ctx, JSValueConst v)
{
    QPType *tp;
    char buf[QP_TPNAME_BUF];

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return qp_long_hash(v);
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return qp_str_hash(ctx, v);
    default:
        break;
    }
    tp = qp_type_of(ctx, v);
    if (tp->slots.tp_hash)
        return tp->slots.tp_hash(ctx, v);
    qp_raise_type_error(ctx, "unhashable type: '%.200s'",
                        qp_obj_tpname(ctx, tp, buf, sizeof(buf)));
    return -1;
}

/* PyObject_IsTrue */
static int qp_bool(JSContext *ctx, JSValueConst v)
{
    QPType *tp;
    Py_ssize_t res;

    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_BOOL:
        return JS_VALUE_GET_BOOL(v) != 0;
    case JS_TAG_INT:
        return JS_VALUE_GET_INT(v) != 0;
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL:
        return 0;
    case JS_TAG_FLOAT64:
        return JS_VALUE_GET_FLOAT64(v) != 0.0;
    case JS_TAG_STRING:
        return JS_VALUE_GET_STRING(v)->len != 0;
    default:
        break;
    }
    tp = qp_type_of(ctx, v);
    if (tp->slots.nb.nb_bool)
        res = tp->slots.nb.nb_bool(ctx, v);
    else if (tp->slots.mp.mp_length)
        res = tp->slots.mp.mp_length(ctx, v);
    else if (tp->slots.sq.sq_length)
        res = tp->slots.sq.sq_length(ctx, v);
    else
        return 1;
    return res > 0 ? 1 : (int)res;
}

/* `is`: None is JS undefined or null (D3); ints compare by value (JS has
   no int identity); floats by their 64-bit pattern (NaNs created by
   float() carry unique payloads, see floatobject.c); heap values by
   pointer. */
static BOOL qp_is(JSValueConst a, JSValueConst b)
{
    int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b);

    if (ta == JS_TAG_NULL)
        ta = JS_TAG_UNDEFINED;
    if (tb == JS_TAG_NULL)
        tb = JS_TAG_UNDEFINED;
    if (ta != tb)
        return FALSE;
    switch (ta) {
    case JS_TAG_UNDEFINED:
        return TRUE;
    case JS_TAG_INT:
        return JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    case JS_TAG_BOOL:
        return (JS_VALUE_GET_BOOL(a) != 0) == (JS_VALUE_GET_BOOL(b) != 0);
    case JS_TAG_SHORT_BIG_INT:
        return JS_VALUE_GET_SHORT_BIG_INT(a) == JS_VALUE_GET_SHORT_BIG_INT(b);
    case JS_TAG_FLOAT64: {
        double da = JS_VALUE_GET_FLOAT64(a), db = JS_VALUE_GET_FLOAT64(b);
        return memcmp(&da, &db, sizeof(double)) == 0;
    }
    default:
        if (JS_VALUE_HAS_REF_COUNT(a))
            return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
        return TRUE;  /* other immediate singletons (uninitialized, ...) */
    }
}

/* ------------------------------------------------------------------------ */
/* Rich comparison                                                           */
/* ------------------------------------------------------------------------ */

static const int qp_swapped_op[6] = {
    QP_CMP_GT, QP_CMP_GE, QP_CMP_EQ, QP_CMP_NE, QP_CMP_LT, QP_CMP_LE
};
static const char *const qp_cmp_opstrings[6] = { "<", "<=", "==", "!=", ">", ">=" };

static JSValue qp_obj_cmp_result(int op, int c)
{
    BOOL r;
    switch (op) {
    case QP_CMP_LT: r = c < 0; break;
    case QP_CMP_LE: r = c <= 0; break;
    case QP_CMP_EQ: r = c == 0; break;
    case QP_CMP_NE: r = c != 0; break;
    case QP_CMP_GT: r = c > 0; break;
    default: r = c >= 0; break;
    }
    return qp_bool_new(r);
}

/* do_richcompare: reflected operation first when type(w) is a strict
   subclass of type(v); NotImplemented from both sides falls back to
   identity for ==/!= and raises TypeError otherwise. */
static JSValue qp_obj_do_richcompare(JSContext *ctx, JSValueConst v, JSValueConst w, int op)
{
    QPType *tv = qp_type_of(ctx, v), *tw = qp_type_of(ctx, w);
    QPRichCmpFunc *f;
    JSValue res;
    int checked_reverse_op = 0;

    if (tv != tw && qp_type_is_subtype(tw, tv) &&
        (f = tw->slots.tp_richcompare) != NULL) {
        checked_reverse_op = 1;
        res = f(ctx, w, v, qp_swapped_op[op]);
        if (!qp_is_notimpl(ctx, res))
            return res;
        JS_FreeValue(ctx, res);
    }
    if ((f = tv->slots.tp_richcompare) != NULL) {
        res = f(ctx, v, w, op);
        if (!qp_is_notimpl(ctx, res))
            return res;
        JS_FreeValue(ctx, res);
    }
    if (!checked_reverse_op && (f = tw->slots.tp_richcompare) != NULL) {
        res = f(ctx, w, v, qp_swapped_op[op]);
        if (!qp_is_notimpl(ctx, res))
            return res;
        JS_FreeValue(ctx, res);
    }
    switch (op) {
    case QP_CMP_EQ:
        return qp_bool_new(qp_is(v, w));
    case QP_CMP_NE:
        return qp_bool_new(!qp_is(v, w));
    default: {
        char b1[QP_TPNAME_BUF], b2[QP_TPNAME_BUF];
        return qp_raise_type_error(ctx,
            "'%s' not supported between instances of '%.100s' and '%.100s'",
            qp_cmp_opstrings[op], qp_obj_tpname(ctx, tv, b1, sizeof(b1)),
            qp_obj_tpname(ctx, tw, b2, sizeof(b2)));
    }
    }
}

/* PyObject_RichCompare.  op = QP_CMP_*, optionally | QP_CMP_TO_BOOL
   (result converted to an exact bool, CPython COMPARE_OP oparg bit). */
static JSValue qp_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    int to_bool = op & QP_CMP_TO_BOOL;
    int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b);
    JSValue res;

    op &= QP_CMP_OP_MASK;
    /* fast paths for exact int32 and exact str operands */
    if (ta == JS_TAG_INT && tb == JS_TAG_INT) {
        int32_t x = JS_VALUE_GET_INT(a), y = JS_VALUE_GET_INT(b);
        return qp_obj_cmp_result(op, (x > y) - (x < y));
    }
    if ((ta == JS_TAG_STRING || ta == JS_TAG_STRING_ROPE) &&
        (tb == JS_TAG_STRING || tb == JS_TAG_STRING_ROPE)) {
        int c;
        if (op == QP_CMP_EQ || op == QP_CMP_NE) {
            c = qp_str_eq(ctx, a, b);
            if (c < 0)
                return JS_EXCEPTION;
            return qp_bool_new((op == QP_CMP_EQ) == (c != 0));
        }
        c = qp_str_compare(ctx, a, b);
        if (c == INT_MIN)
            return JS_EXCEPTION;
        return qp_obj_cmp_result(op, c);
    }
    if (qp_obj_enter_recursive(ctx) < 0)
        return JS_EXCEPTION;
    res = qp_obj_do_richcompare(ctx, a, b, op);
    if (to_bool && !JS_IsException(res) && JS_VALUE_GET_TAG(res) != JS_TAG_BOOL) {
        int t = qp_bool(ctx, res);
        JS_FreeValue(ctx, res);
        if (t < 0)
            return JS_EXCEPTION;
        res = qp_bool_new(t);
    }
    return res;
}

static int qp_richcompare_bool(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    JSValue res;
    int ok;

    op &= QP_CMP_OP_MASK;
    /* identity implies equality (containers rely on it) */
    if (qp_is(a, b)) {
        if (op == QP_CMP_EQ)
            return 1;
        if (op == QP_CMP_NE)
            return 0;
    }
    res = qp_richcompare(ctx, a, b, op);
    if (JS_IsException(res))
        return -1;
    if (JS_VALUE_GET_TAG(res) == JS_TAG_BOOL)
        ok = JS_VALUE_GET_BOOL(res) != 0;
    else
        ok = qp_bool(ctx, res);
    JS_FreeValue(ctx, res);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* dir()                                                                     */
/* ------------------------------------------------------------------------ */

/* PyMapping_Keys + sort for dir() without argument. */
static JSValue qp_obj_dir_locals(JSContext *ctx)
{
    struct JSStackFrame *sf = qp_py_frame_at(ctx, 0);
    JSValue locals, names, it_src;
    char b1[QP_TPNAME_BUF];

    if (!sf)
        return qp_raise_system_error(ctx, "frame does not exist");
    locals = qp_frame_locals(ctx, sf);
    if (JS_IsException(locals))
        return locals;
    if (qp_dict_check_exact(ctx, locals)) {
        names = qp_dict_keys_list(ctx, locals);
    } else {
        it_src = qp_call_method(ctx, locals, QP_ATOM(ctx, keys), NULL, 0, QP_NULL);
        if (JS_IsException(it_src)) {
            JS_FreeValue(ctx, locals);
            return it_src;
        }
        if (qp_list_check_exact(ctx, it_src)) {
            names = it_src;
        } else {
            JSValue it = qp_iter(ctx, it_src);
            if (JS_IsException(it)) {
                if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                    qp_obj_clear_exc(ctx);
                    qp_raise_type_error(ctx, "%T.%s() returned a non-iterable (type %T)",
                                        locals, "keys", it_src);
                }
                JS_FreeValue(ctx, it_src);
                JS_FreeValue(ctx, locals);
                return JS_EXCEPTION;
            }
            names = qp_list_from_iterable(ctx, it);
            JS_FreeValue(ctx, it);
            JS_FreeValue(ctx, it_src);
        }
    }
    JS_FreeValue(ctx, locals);
    if (JS_IsException(names))
        return names;
    if (!qp_list_check(ctx, names)) {
        qp_raise_type_error(ctx, "dir(): expected keys() of locals to be a list, not '%.200s'",
                            qp_obj_tpname(ctx, qp_type_of(ctx, names), b1, sizeof(b1)));
        JS_FreeValue(ctx, names);
        return JS_EXCEPTION;
    }
    if (qp_list_sort(ctx, names, QP_NULL, 0) < 0) {
        JS_FreeValue(ctx, names);
        return JS_EXCEPTION;
    }
    return names;
}

/* PyObject_Dir: dir(obj) = sorted(type(obj).__dir__(obj)); obj QP_NULL =
   names of the caller's local scope. */
static JSValue qp_dir(JSContext *ctx, JSValueConst obj)
{
    JSValue dirfunc, result, sorted;

    if (QP_IS_NULL(obj))
        return qp_obj_dir_locals(ctx);
    dirfunc = qp_lookup_special(ctx, obj, QP_ATOM(ctx, __dir__));
    if (JS_IsException(dirfunc))
        return dirfunc;
    if (QP_IS_NULL(dirfunc))
        return qp_raise_type_error(ctx, "object does not provide __dir__");
    result = qp_call0(ctx, dirfunc);
    JS_FreeValue(ctx, dirfunc);
    if (JS_IsException(result))
        return result;
    sorted = qp_list_from_iterable(ctx, result);
    JS_FreeValue(ctx, result);
    if (JS_IsException(sorted))
        return sorted;
    if (qp_list_sort(ctx, sorted, QP_NULL, 0) < 0) {
        JS_FreeValue(ctx, sorted);
        return JS_EXCEPTION;
    }
    return sorted;
}

/* ------------------------------------------------------------------------ */
/* Recursive repr guard                                                      */
/* ------------------------------------------------------------------------ */

/* QPState.repr_stack is a list created on first use. */
static int qp_repr_enter(JSContext *ctx, JSValueConst obj)
{
    QPState *s = QP_STATE(ctx);
    JSValue *items;
    Py_ssize_t len, i;

    if (!JS_IsObject(s->repr_stack)) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return -1;
        s->repr_stack = l;
    }
    items = qp_list_items(s->repr_stack, &len);
    if (items) {
        for (i = len - 1; i >= 0; i--) {
            if (qp_is(items[i], obj))
                return 1;
        }
    }
    if (qp_list_append(ctx, s->repr_stack, obj) < 0)
        return -1;
    return 0;
}

static void qp_repr_leave(JSContext *ctx, JSValueConst obj)
{
    QPState *s = QP_STATE(ctx);
    JSValue *items, v;
    Py_ssize_t len, i;

    if (!JS_IsObject(s->repr_stack))
        return;
    items = qp_list_items(s->repr_stack, &len);
    if (!items)
        return;
    /* count backwards: obj is normally the last item */
    for (i = len - 1; i >= 0; i--) {
        if (qp_is(items[i], obj)) {
            v = items[i];
            memmove(&items[i], &items[i + 1], sizeof(JSValue) * (len - 1 - i));
            items[len - 1] = JS_UNDEFINED;
            /* shrinking a fast array never throws */
            JS_SetProperty(ctx, s->repr_stack, JS_ATOM_length, JS_NewInt32(ctx, (int32_t)(len - 1)));
            JS_FreeValue(ctx, v);
            break;
        }
    }
}
