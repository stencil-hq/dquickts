/*
 * dquickts Python: builtin exception types (object.h section (i), the
 * exceptions.c part).
 *
 * Derived from CPython Objects/exceptions.c and the except* helpers of
 * Python/ceval.c (_PyEval_ExceptionGroupMatch,
 * _PyEval_CheckExceptStarTypeValid) (PSF-2.0).
 *
 * Every BaseException instance is a JS_CLASS_PY_EXCEPTION object whose
 * u.opaque is a QPExcObject (private to this file; errors.c/traceback.c use
 * the accessor functions at the end of the "payload" section).  Instance
 * attributes (incl. __notes__, like CPython's __dict__) live in QuickJS
 * shape properties handled by the generic attribute code of unit h.
 *
 * Subclass fields (CPython PyOSErrorObject.myerrno, PySyntaxErrorObject.msg,
 * ...) share one fixed array f[]; the QP_EXC_F_* indices below give the
 * layout per exception family.  CPython's T_OBJECT members become getsets
 * with the same semantics (unset reads as None, delete clears).
 */

#include <errno.h>

/* Provided by traceback.c / errors.c (unit i, M2ErrTb). */
static int qp_init_traceback(JSContext *ctx);
static JSValue qp_tb_from_current_frame(JSContext *ctx);
static JSValue qp_calculate_suggestions(JSContext *ctx, JSValueConst dir_list, JSValueConst name);

/* ========================================================================= */
/* Payload                                                                    */
/* ========================================================================= */

#define QP_EXC_NFIELDS 9

/* StopIteration */
#define QP_EXC_F_VALUE          0
/* SystemExit */
#define QP_EXC_F_CODE           0
/* BaseExceptionGroup */
#define QP_EXC_F_EG_MSG         0
#define QP_EXC_F_EG_EXCS        1
#define QP_EXC_F_EG_EXCS_STR    2   /* repr of a non list/tuple sequence argument */
/* ImportError */
#define QP_EXC_F_IMP_MSG        0
#define QP_EXC_F_IMP_NAME       1
#define QP_EXC_F_IMP_PATH       2
#define QP_EXC_F_IMP_NAME_FROM  3
/* OSError */
#define QP_EXC_F_OS_ERRNO       0
#define QP_EXC_F_OS_STRERROR    1
#define QP_EXC_F_OS_FILENAME    2
#define QP_EXC_F_OS_FILENAME2   3
/* NameError */
#define QP_EXC_F_NAME_NAME      0
/* AttributeError */
#define QP_EXC_F_ATTR_NAME      0
#define QP_EXC_F_ATTR_OBJ       1
/* SyntaxError */
#define QP_EXC_F_SE_MSG         0
#define QP_EXC_F_SE_FILENAME    1
#define QP_EXC_F_SE_LINENO      2
#define QP_EXC_F_SE_OFFSET      3
#define QP_EXC_F_SE_TEXT        4
#define QP_EXC_F_SE_END_LINENO  5
#define QP_EXC_F_SE_END_OFFSET  6
#define QP_EXC_F_SE_PFAL        7   /* print_file_and_line */
#define QP_EXC_F_SE_METADATA    8   /* _metadata */
/* UnicodeError */
#define QP_EXC_F_UE_ENCODING    0
#define QP_EXC_F_UE_OBJECT      1
#define QP_EXC_F_UE_REASON      2

typedef struct QPExcObject {
    JSValue args;               /* tuple */
    JSValue context;            /* QP_NULL = unset (reads None) */
    JSValue cause;              /* QP_NULL = unset */
    JSValue traceback;          /* traceback object or QP_NULL */
    uint8_t suppress_context;
    Py_ssize_t written;         /* BlockingIOError.characters_written, -1 = unset */
    Py_ssize_t start, end;      /* UnicodeError */
    JSValue f[QP_EXC_NFIELDS];  /* subclass fields, QP_NULL = unset */
} QPExcObject;

/* Payload of a PY_EXCEPTION object, NULL for anything else. */
static QPExcObject *qp_exc_obj(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_EXCEPTION)
        return NULL;
    return (QPExcObject *)p->u.opaque;
}

static void qp_exc_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    QPExcObject *e = (QPExcObject *)p->u.opaque;
    int i;
    if (!e)
        return;
    JS_FreeValueRT(rt, e->args);
    JS_FreeValueRT(rt, e->context);
    JS_FreeValueRT(rt, e->cause);
    JS_FreeValueRT(rt, e->traceback);
    for (i = 0; i < QP_EXC_NFIELDS; i++)
        JS_FreeValueRT(rt, e->f[i]);
    js_free_rt(rt, e);
    p->u.opaque = NULL;
}

static void qp_exc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPExcObject *e = (QPExcObject *)JS_VALUE_GET_OBJ(val)->u.opaque;
    int i;
    if (!e)
        return;
    JS_MarkValue(rt, e->args, mark_func);
    JS_MarkValue(rt, e->context, mark_func);
    JS_MarkValue(rt, e->cause, mark_func);
    JS_MarkValue(rt, e->traceback, mark_func);
    for (i = 0; i < QP_EXC_NFIELDS; i++)
        JS_MarkValue(rt, e->f[i], mark_func);
}

static inline JSValue qp_exc_or_none(JSContext *ctx, JSValueConst v)
{
    return QP_IS_NULL(v) ? JS_UNDEFINED : JS_DupValue(ctx, v);
}

/* Replace a payload slot (consumes v; None is stored as QP_NULL when
   none_is_null). */
static inline void qp_exc_slot_set(JSContext *ctx, JSValue *slot, JSValue v, BOOL none_is_null)
{
    JSValue old = *slot;
    if (none_is_null && JS_IsUndefined(v))
        v = QP_NULL;
    *slot = v;
    JS_FreeValue(ctx, old);
}

static inline BOOL qp_exc_same(JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_VALUE_GET_TAG(b))
        return FALSE;
    if (JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT)
        return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
    return TRUE;
}

/* ---- accessors used by errors.c / traceback.c (no exceptions raised) ---- */

static JSValue qp_exc_context(JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    return e ? e->context : QP_NULL;
}

static JSValue qp_exc_cause(JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    return e ? e->cause : QP_NULL;
}

static BOOL qp_exc_suppress_context(JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    return e ? e->suppress_context : FALSE;
}

/* PyException_SetContext (consumes context; QP_NULL/None clears). */
static void qp_exc_set_context(JSContext *ctx, JSValueConst exc, JSValue context)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e) {
        JS_FreeValue(ctx, context);
        return;
    }
    qp_exc_slot_set(ctx, &e->context, context, TRUE);
}

/* PyException_SetCause (consumes cause; QP_NULL/None clears; always sets
   __suppress_context__). */
static void qp_exc_set_cause(JSContext *ctx, JSValueConst exc, JSValue cause)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e) {
        JS_FreeValue(ctx, cause);
        return;
    }
    e->suppress_context = 1;
    qp_exc_slot_set(ctx, &e->cause, cause, TRUE);
}

/* Borrowed __traceback__: traceback object or None. */
static JSValue qp_exc_traceback(JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e || QP_IS_NULL(e->traceback))
        return JS_UNDEFINED;
    return e->traceback;
}

/* Store __traceback__ without type check (consumes tb; None clears). */
static void qp_exc_set_traceback_raw(JSContext *ctx, JSValueConst exc, JSValue tb)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e) {
        JS_FreeValue(ctx, tb);
        return;
    }
    qp_exc_slot_set(ctx, &e->traceback, tb, TRUE);
}

/* Borrowed args tuple. */
static __maybe_unused JSValue qp_exc_args(JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    return e ? e->args : QP_NULL;
}

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

static BOOL qp_exc_check(JSContext *ctx, JSValueConst v)
{
    return qp_exc_obj(v) != NULL;
}

/* BaseException subclass: every such type has the solid layout of
   BaseException, i.e. instances of class PY_EXCEPTION. */
static BOOL qp_exc_type_check(JSValueConst v)
{
    QPType *t = qp_type_from_value(v);
    if (!t)
        return FALSE;
    return (t->flags & QP_TPFLAGS_BASE_EXC_SUBCLASS) != 0 ||
        t->instance_class_id == JS_CLASS_PY_EXCEPTION;
}

/* Payload of self for C methods/getsets (TypeError if not an exception). */
static QPExcObject *qp_exc_self(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_obj(self);
    if (!e)
        qp_raise_type_error(ctx, "descriptor requires a 'BaseException' object but received a '%T'", self);
    return e;
}

/* CPython tp_name of a type as a str (heap types: __name__). */
static JSValue qp_exc_tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* _PyType_Name: tp_name after the last dot. */
static JSValue qp_exc_short_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name) {
        const char *s = strrchr(t->tp_name, '.');
        return qp_str_from_cstr(ctx, s ? s + 1 : t->tp_name);
    }
    return qp_str_from_atom(ctx, t->name);
}

/* Type name used by getargs.c messages ("None" for None). */
static JSValue qp_exc_arg_type_name(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return qp_str_from_cstr(ctx, "None");
    return qp_exc_tp_name(ctx, qp_type_of(ctx, v));
}

static inline int qp_exc_nkw(JSValueConst kwnames)
{
    return QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
}

/* _PyArg_NoKeywords(tp_name, kwds) */
static int qp_exc_no_keywords(JSContext *ctx, QPType *t, JSValueConst kwnames)
{
    JSValue name;
    if (qp_exc_nkw(kwnames) == 0)
        return 0;
    name = qp_exc_tp_name(ctx, t);
    if (JS_IsException(name))
        return -1;
    qp_raise_type_error(ctx, "%U() takes no keyword arguments", name);
    JS_FreeValue(ctx, name);
    return -1;
}

/* getargs.c argument count error ("%s%s takes %s %d argument%s (%zd given)").
   fname NULL = "function". */
static int qp_exc_argcount_error(JSContext *ctx, const char *fname, int min, int max, Py_ssize_t n)
{
    int lim = n < min ? min : max;
    qp_raise_type_error(ctx, "%.150s%s takes %s %d argument%s (%zd given)",
                        fname ? fname : "function", fname ? "()" : "",
                        min == max ? "exactly" : n < min ? "at least" : "at most",
                        lim, lim == 1 ? "" : "s", n);
    return -1;
}

/* getargs.c 'U' format check: "argument N must be str, not T". */
static int qp_exc_arg_str(JSContext *ctx, const char *fname, int index, JSValueConst v)
{
    JSValue tn;
    if (qp_str_check(ctx, v))
        return 0;
    tn = qp_exc_arg_type_name(ctx, v);
    if (JS_IsException(tn))
        return -1;
    qp_raise_type_error(ctx, "%s%sargument %d must be str, not %U",
                        fname ? fname : "", fname ? "() " : "", index, tn);
    JS_FreeValue(ctx, tn);
    return -1;
}

/* PyArg_ParseTupleAndKeywords(empty, kwds, "|$O...:fname", kwlist): only
   keyword arguments (kwnames values follow args[0..nargs)); out[i] =
   borrowed value or QP_NULL. */
static int qp_exc_parse_kwonly(JSContext *ctx, const char *fname, const char *const *kwlist,
                               int nkwlist, JSValueConst *args, int nargs,
                               JSValueConst kwnames, JSValueConst *out)
{
    int i, j, nkw = qp_exc_nkw(kwnames);
    for (j = 0; j < nkwlist; j++)
        out[j] = QP_NULL;
    for (i = 0; i < nkw; i++) {
        JSValueConst kw = QP_TUPLE_ITEMS(kwnames)[i];
        const char *s;
        size_t len;
        int found = -1;
        s = qp_str_utf8(ctx, kw, &len);
        if (!s)
            return -1;
        for (j = 0; j < nkwlist; j++) {
            if (strlen(kwlist[j]) == len && !memcmp(kwlist[j], s, len)) {
                found = j;
                break;
            }
        }
        JS_FreeCString(ctx, s);
        if (found >= 0) {
            out[found] = args[nargs + i];
        } else {
            JSValue list, sugg;
            list = qp_list_new(ctx, 0);
            if (JS_IsException(list))
                return -1;
            for (j = 0; j < nkwlist; j++) {
                JSValue k = qp_str_from_cstr(ctx, kwlist[j]);
                if (JS_IsException(k) || qp_list_append(ctx, list, k) < 0) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, list);
                    return -1;
                }
                JS_FreeValue(ctx, k);
            }
            sugg = qp_calculate_suggestions(ctx, list, kw);
            JS_FreeValue(ctx, list);
            if (JS_IsException(sugg))
                return -1;
            if (!QP_IS_NULL(sugg)) {
                qp_raise_type_error(ctx, "%.200s%s got an unexpected keyword argument '%S'."
                                    " Did you mean '%S'?", fname, "()", kw, sugg);
                JS_FreeValue(ctx, sugg);
            } else {
                qp_raise_type_error(ctx, "%.200s%s got an unexpected keyword argument '%S'",
                                    fname, "()", kw);
            }
            return -1;
        }
    }
    return 0;
}

/* PyCallable_Check */
static BOOL qp_exc_callable_check(JSContext *ctx, JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* PySequence_Check */
static BOOL qp_exc_sequence_check(JSContext *ctx, JSValueConst v)
{
    if (qp_dict_check(ctx, v))
        return FALSE;
    if (qp_list_check(ctx, v) || qp_tuple_check(ctx, v))
        return TRUE;
    return qp_type_of(ctx, v)->slots.sq.sq_item != NULL;
}

/* PyNumber_Check */
static BOOL qp_exc_number_check(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    return t->slots.nb.nb_index || t->slots.nb.nb_int || t->slots.nb.nb_float ||
        qp_complex_check(ctx, v);
}

/* The instance __dict__ if the object has attributes (CPython: dict
   created on first attribute store), else QP_NULL.  JS_EXCEPTION on
   error. */
static JSValue qp_exc_instance_dict(JSContext *ctx, JSValueConst self)
{
    JSValue d = qp_object_get_dict(ctx, self);
    if (JS_IsException(d))
        return d;
    if (qp_dict_size(d) == 0) {
        JS_FreeValue(ctx, d);
        return QP_NULL;
    }
    return d;
}

/* ========================================================================= */
/* BaseException                                                              */
/* ========================================================================= */

/* Allocate an instance of `type` with the given args tuple (consumed). */
static JSValue qp_exc_alloc(JSContext *ctx, QPType *type, JSValue args)
{
    JSValue obj;
    QPExcObject *e;
    int i;

    obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_EXCEPTION);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, args);
        return obj;
    }
    e = js_mallocz(ctx, sizeof(*e));
    if (!e) {
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    e->args = args;
    e->context = e->cause = e->traceback = QP_NULL;
    e->suppress_context = 0;
    e->written = -1;
    e->start = e->end = 0;
    for (i = 0; i < QP_EXC_NFIELDS; i++)
        e->f[i] = QP_NULL;
    JS_VALUE_GET_OBJ(obj)->u.opaque = e;
    return obj;
}

/* BaseException_new: args = positional arguments; keywords ignored here. */
static JSValue qp_exc_base_new(JSContext *ctx, QPType *type, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    JSValue tup = qp_tuple_pack(ctx, qp_nargs(nargsf), args);
    if (JS_IsException(tup))
        return tup;
    return qp_exc_alloc(ctx, type, tup);
}

/* BaseException_init */
static int qp_exc_base_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            size_t nargsf, JSValueConst kwnames)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue tup;
    if (!e)
        return -1;
    if (qp_exc_no_keywords(ctx, qp_type_of(ctx, self), kwnames) < 0)
        return -1;
    tup = qp_tuple_pack(ctx, qp_nargs(nargsf), args);
    if (JS_IsException(tup))
        return -1;
    qp_exc_slot_set(ctx, &e->args, tup, FALSE);
    return 0;
}

/* BaseException_init with kwds == NULL (subclasses parsing their own
   keyword-only arguments). */
static int qp_exc_base_init_nokw(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 size_t nargsf)
{
    return qp_exc_base_init(ctx, self, args, qp_nargs(nargsf), QP_NULL);
}

static JSValue qp_exc_base_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    switch (QP_TUPLE_SIZE(e->args)) {
    case 0:
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    case 1:
        return qp_str(ctx, QP_TUPLE_ITEMS(e->args)[0]);
    default:
        return qp_str(ctx, e->args);
    }
}

static JSValue qp_exc_base_repr(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue name, res, args;
    if (!e)
        return JS_EXCEPTION;
    name = qp_exc_short_name(ctx, qp_type_of(ctx, self));
    if (JS_IsException(name))
        return name;
    args = JS_DupValue(ctx, e->args);   /* keep alive across repr calls */
    if (QP_TUPLE_SIZE(args) == 1)
        res = qp_str_from_format(ctx, "%U(%R)", name, QP_TUPLE_ITEMS(args)[0]);
    else
        res = qp_str_from_format(ctx, "%U%R", name, args);
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, name);
    return res;
}

/* __reduce__ */
static JSValue qp_exc_base_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue d, items[3];
    if (!e)
        return JS_EXCEPTION;
    d = qp_exc_instance_dict(ctx, self);
    if (JS_IsException(d))
        return d;
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = JS_DupValue(ctx, e->args);
    if (QP_IS_NULL(d))
        return qp_tuple_new_steal(ctx, 2, items);
    items[2] = d;
    return qp_tuple_new_steal(ctx, 3, items);
}

/* __setstate__ */
static JSValue qp_exc_base_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst state = args[0];
    JSValue items;
    Py_ssize_t i, n;

    if (JS_IsUndefined(state) || JS_IsNull(state))
        return JS_UNDEFINED;
    if (!qp_dict_check(ctx, state))
        return qp_raise_type_error(ctx, "state is not a dictionary");
    items = qp_dict_items_list(ctx, state);
    if (JS_IsException(items))
        return items;
    n = qp_list_size(items);
    for (i = 0; i < n; i++) {
        JSValue pair = qp_getitem(ctx, items, JS_NewInt32(ctx, (int32_t)i));
        JSValueConst key, value;
        JSAtom atom;
        int r;
        if (JS_IsException(pair))
            goto fail;
        key = QP_TUPLE_ITEMS(pair)[0];
        value = QP_TUPLE_ITEMS(pair)[1];
        if (!qp_str_check(ctx, key)) {
            JSValue tn = qp_exc_tp_name(ctx, qp_type_of(ctx, key));
            if (!JS_IsException(tn)) {
                qp_raise_type_error(ctx, "attribute name must be string, not '%U'", tn);
                JS_FreeValue(ctx, tn);
            }
            JS_FreeValue(ctx, pair);
            goto fail;
        }
        {
            JSValue ks = qp_str_value(ctx, key);
            if (JS_IsException(ks)) {
                JS_FreeValue(ctx, pair);
                goto fail;
            }
            atom = qp_str_to_atom(ctx, ks);
            JS_FreeValue(ctx, ks);
        }
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, pair);
            goto fail;
        }
        r = qp_setattr(ctx, self, atom, value);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, pair);
        if (r < 0)
            goto fail;
    }
    JS_FreeValue(ctx, items);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, items);
    return JS_EXCEPTION;
}

/* __traceback__ setter (BaseException___traceback___set_impl). */
static int qp_exc_set_traceback(JSContext *ctx, JSValueConst exc, JSValueConst tb)
{
    QPExcObject *e = qp_exc_self(ctx, exc);
    if (!e)
        return -1;
    if (QP_IS_NULL(tb))
        return qp_raise_type_error(ctx, "__traceback__ may not be deleted"), -1;
    if (JS_VALUE_GET_TAG(tb) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(tb)->class_id == JS_CLASS_PY_TRACEBACK) {
        qp_exc_slot_set(ctx, &e->traceback, JS_DupValue(ctx, tb), FALSE);
    } else if (JS_IsUndefined(tb) || JS_IsNull(tb)) {
        qp_exc_slot_set(ctx, &e->traceback, QP_NULL, FALSE);
    } else {
        qp_raise_type_error(ctx, "__traceback__ must be a traceback or None");
        return -1;
    }
    return 0;
}

static JSValue qp_exc_get_traceback(JSContext *ctx, JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e)
        return JS_UNDEFINED;
    return qp_exc_or_none(ctx, e->traceback);
}

/* with_traceback(tb) */
static JSValue qp_exc_base_with_traceback(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    if (qp_exc_set_traceback(ctx, self, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

/* add_note(note) */
static JSValue qp_exc_base_add_note(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst note = args[0];
    JSValue notes;
    int r;

    if (!qp_str_check(ctx, note)) {
        JSValue tn = qp_exc_arg_type_name(ctx, note);
        if (JS_IsException(tn))
            return tn;
        qp_raise_type_error(ctx, "add_note() argument must be str, not %U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    r = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __notes__), &notes);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        notes = qp_list_new(ctx, 0);
        if (JS_IsException(notes))
            return notes;
        if (qp_setattr(ctx, self, QP_ATOM(ctx, __notes__), notes) < 0) {
            JS_FreeValue(ctx, notes);
            return JS_EXCEPTION;
        }
    } else if (!qp_list_check(ctx, notes)) {
        JS_FreeValue(ctx, notes);
        return qp_raise_type_error(ctx, "Cannot add note: __notes__ is not a list");
    }
    r = qp_list_append(ctx, notes, note);
    JS_FreeValue(ctx, notes);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* getsets of BaseException */
enum {
    QP_EXC_GS_ARGS,
    QP_EXC_GS_TRACEBACK,
    QP_EXC_GS_CONTEXT,
    QP_EXC_GS_CAUSE,
    QP_EXC_GS_SUPPRESS,
};

static JSValue qp_exc_base_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    switch (magic) {
    case QP_EXC_GS_ARGS:
        return qp_exc_or_none(ctx, e->args);
    case QP_EXC_GS_TRACEBACK:
        return qp_exc_or_none(ctx, e->traceback);
    case QP_EXC_GS_CONTEXT:
        return qp_exc_or_none(ctx, e->context);
    case QP_EXC_GS_CAUSE:
        return qp_exc_or_none(ctx, e->cause);
    default:
        return JS_NewBool(ctx, e->suppress_context);
    }
}

static int qp_exc_base_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return -1;
    switch (magic) {
    case QP_EXC_GS_ARGS: {
        JSValue seq;
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "args may not be deleted");
            return -1;
        }
        seq = qp_tuple_from_iterable(ctx, value);
        if (JS_IsException(seq))
            return -1;
        qp_exc_slot_set(ctx, &e->args, seq, FALSE);
        return 0;
    }
    case QP_EXC_GS_TRACEBACK:
        return qp_exc_set_traceback(ctx, self, value);
    case QP_EXC_GS_CONTEXT:
    case QP_EXC_GS_CAUSE: {
        BOOL is_cause = magic == QP_EXC_GS_CAUSE;
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, is_cause ? "__cause__ may not be deleted"
                                : "__context__ may not be deleted");
            return -1;
        }
        if (!(JS_IsUndefined(value) || JS_IsNull(value)) && !qp_exc_check(ctx, value)) {
            qp_raise_type_error(ctx, is_cause
                                ? "exception cause must be None or derive from BaseException"
                                : "exception context must be None or derive from BaseException");
            return -1;
        }
        if (is_cause)
            qp_exc_set_cause(ctx, self, JS_DupValue(ctx, value));
        else
            qp_exc_set_context(ctx, self, JS_DupValue(ctx, value));
        return 0;
    }
    default:
        /* Py_T_BOOL member */
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "can't delete numeric/char attribute");
            return -1;
        }
        if (JS_VALUE_GET_TAG(value) != JS_TAG_BOOL) {
            qp_raise_type_error(ctx, "attribute value type must be bool");
            return -1;
        }
        e->suppress_context = JS_VALUE_GET_BOOL(value) != 0;
        return 0;
    }
}

/* Generic T_OBJECT member over e->f[magic]: unset reads None. */
static JSValue qp_exc_field_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    return qp_exc_or_none(ctx, e->f[magic]);
}

static int qp_exc_field_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return -1;
    qp_exc_slot_set(ctx, &e->f[magic], QP_IS_NULL(value) ? QP_NULL : JS_DupValue(ctx, value), FALSE);
    return 0;
}

static const QPMethodDef qp_exc_base_methods[] = {
    { "__reduce__", qp_exc_base_reduce, QP_METH_NOARGS, 0, "__reduce__($self, /)\n--\n\n" },
    { "__setstate__", qp_exc_base_setstate, QP_METH_O, 0, "__setstate__($self, state, /)\n--\n\n" },
    { "with_traceback", qp_exc_base_with_traceback, QP_METH_O, 0,
      "with_traceback($self, tb, /)\n--\n\nSet self.__traceback__ to tb and return self." },
    { "add_note", qp_exc_base_add_note, QP_METH_O, 0,
      "add_note($self, note, /)\n--\n\nAdd a note to the exception" },
    { NULL },
};

static const QPGetSetDef qp_exc_base_getsets[] = {
    /* PyObject_GenericGetDict / PyObject_GenericSetDict */
    { "__dict__", qp_subtype_dict_get, qp_subtype_dict_set, 0, NULL },
    { "args", qp_exc_base_get, qp_exc_base_set, QP_EXC_GS_ARGS, NULL },
    { "__traceback__", qp_exc_base_get, qp_exc_base_set, QP_EXC_GS_TRACEBACK, NULL },
    { "__context__", qp_exc_base_get, qp_exc_base_set, QP_EXC_GS_CONTEXT, NULL },
    { "__cause__", qp_exc_base_get, qp_exc_base_set, QP_EXC_GS_CAUSE, NULL },
    { "__suppress_context__", qp_exc_base_get, qp_exc_base_set, QP_EXC_GS_SUPPRESS, NULL,
      QP_GETSET_MEMBER },
    { NULL },
};

/* ========================================================================= */
/* StopIteration, SystemExit                                                  */
/* ========================================================================= */

static int qp_exc_stopiteration_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t n = qp_nargs(nargsf);
    QPExcObject *e;
    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_VALUE],
                    n > 0 ? JS_DupValue(ctx, args[0]) : JS_UNDEFINED, FALSE);
    return 0;
}

static JSValue qp_exc_stopiteration_value(JSContext *ctx, JSValueConst exc)
{
    QPExcObject *e = qp_exc_obj(exc);
    if (!e || !qp_typecheck(ctx, exc, QP_EXC(ctx, StopIteration)))
        return JS_UNDEFINED;
    return qp_exc_or_none(ctx, e->f[QP_EXC_F_VALUE]);
}

static const QPGetSetDef qp_exc_stopiteration_getsets[] = {
    { "value", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_VALUE, "generator return value" },
    { NULL },
};

static int qp_exc_systemexit_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t n = qp_nargs(nargsf);
    QPExcObject *e;
    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    e = qp_exc_obj(self);
    if (n == 0)
        return 0;
    if (n == 1)
        qp_exc_slot_set(ctx, &e->f[QP_EXC_F_CODE], JS_DupValue(ctx, args[0]), FALSE);
    else
        qp_exc_slot_set(ctx, &e->f[QP_EXC_F_CODE], JS_DupValue(ctx, e->args), FALSE);
    return 0;
}

static const QPGetSetDef qp_exc_systemexit_getsets[] = {
    { "code", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_CODE, "exception code" },
    { NULL },
};

/* ========================================================================= */
/* BaseExceptionGroup                                                         */
/* ========================================================================= */

static JSValue qp_exc_eg_new(JSContext *ctx, QPType *type, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf), i, numexcs;
    JSValueConst message, exceptions;
    JSValue excs = JS_UNDEFINED, excs_str = QP_NULL, self;
    BOOL nested_base_exceptions = FALSE;
    QPType *cls = type;
    QPExcObject *e;

    if (nargs != 2)
        return qp_exc_argcount_error(ctx, "BaseExceptionGroup.__new__", 2, 2, nargs), JS_EXCEPTION;
    message = args[0];
    exceptions = args[1];
    if (qp_exc_arg_str(ctx, "BaseExceptionGroup.__new__", 1, message) < 0)
        return JS_EXCEPTION;
    if (!qp_exc_sequence_check(ctx, exceptions))
        return qp_raise_type_error(ctx, "second argument (exceptions) must be a sequence");
    /* Save initial exceptions sequence as a string in case sequence is mutated */
    if (!qp_list_check(ctx, exceptions) && !qp_tuple_check(ctx, exceptions)) {
        excs_str = qp_repr(ctx, exceptions);
        if (JS_IsException(excs_str))
            return excs_str;
    }
    excs = qp_tuple_from_iterable(ctx, exceptions);
    if (JS_IsException(excs))
        goto error;
    numexcs = QP_TUPLE_SIZE(excs);
    if (numexcs == 0) {
        qp_raise_value_error(ctx, "second argument (exceptions) must be a non-empty sequence");
        goto error;
    }
    for (i = 0; i < numexcs; i++) {
        JSValueConst exc = QP_TUPLE_ITEMS(excs)[i];
        if (!qp_exc_check(ctx, exc)) {
            qp_raise_value_error(ctx, "Item %zd of second argument (exceptions) is not an exception", i);
            goto error;
        }
        if (!qp_typecheck(ctx, exc, QP_EXC(ctx, Exception)))
            nested_base_exceptions = TRUE;
    }
    if (cls == QP_EXC(ctx, ExceptionGroup)) {
        if (nested_base_exceptions) {
            qp_raise_type_error(ctx, "Cannot nest BaseExceptions in an ExceptionGroup");
            goto error;
        }
    } else if (cls == QP_EXC(ctx, BaseExceptionGroup)) {
        /* All nested exceptions are Exception subclasses: wrap them in an
           ExceptionGroup */
        if (!nested_base_exceptions && QP_EXC(ctx, ExceptionGroup))
            cls = QP_EXC(ctx, ExceptionGroup);
    } else if (nested_base_exceptions) {
        /* user-defined subclass */
        if (qp_type_is_subtype(cls, QP_EXC(ctx, Exception))) {
            JSValue tn = qp_exc_tp_name(ctx, cls);
            if (!JS_IsException(tn)) {
                qp_raise_type_error(ctx, "Cannot nest BaseExceptions in '%U'", tn);
                JS_FreeValue(ctx, tn);
            }
            goto error;
        }
    }
    self = qp_exc_base_new(ctx, cls, args, nargsf, kwnames);
    if (JS_IsException(self))
        goto error;
    e = qp_exc_obj(self);
    e->f[QP_EXC_F_EG_MSG] = JS_DupValue(ctx, message);
    e->f[QP_EXC_F_EG_EXCS] = excs;
    e->f[QP_EXC_F_EG_EXCS_STR] = excs_str;
    return self;
error:
    JS_FreeValue(ctx, excs);
    JS_FreeValue(ctx, excs_str);
    return JS_EXCEPTION;
}

static int qp_exc_eg_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    if (qp_exc_no_keywords(ctx, qp_type_of(ctx, self), kwnames) < 0)
        return -1;
    return qp_exc_base_init(ctx, self, args, nargsf, kwnames);
}

/* Is v an instance of BaseExceptionGroup (_PyBaseExceptionGroup_Check)? */
static BOOL qp_exc_is_group(JSContext *ctx, JSValueConst v)
{
    return qp_exc_obj(v) != NULL && qp_typecheck(ctx, v, QP_EXC(ctx, BaseExceptionGroup));
}

static JSValue qp_exc_eg_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    Py_ssize_t n;
    if (!e)
        return JS_EXCEPTION;
    n = QP_IS_NULL(e->f[QP_EXC_F_EG_EXCS]) ? 0 : QP_TUPLE_SIZE(e->f[QP_EXC_F_EG_EXCS]);
    return qp_str_from_format(ctx, "%S (%zd sub-exception%s)",
                              QP_IS_NULL(e->f[QP_EXC_F_EG_MSG]) ? JS_UNDEFINED : e->f[QP_EXC_F_EG_MSG],
                              n, n > 1 ? "s" : "");
}

static JSValue qp_exc_eg_repr(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue exceptions_str, name, res;
    JSValueConst excs;
    if (!e)
        return JS_EXCEPTION;
    excs = e->f[QP_EXC_F_EG_EXCS];
    if (!QP_IS_NULL(e->f[QP_EXC_F_EG_EXCS_STR])) {
        exceptions_str = JS_DupValue(ctx, e->f[QP_EXC_F_EG_EXCS_STR]);
    } else if (QP_IS_NULL(excs)) {
        exceptions_str = qp_repr(ctx, JS_UNDEFINED);
    } else if (QP_TUPLE_SIZE(e->args) == 2 && qp_list_check(ctx, QP_TUPLE_ITEMS(e->args)[1])) {
        /* use self.exceptions, looking like self.args[1] */
        JSValue l = qp_list_from_iterable(ctx, excs);
        if (JS_IsException(l))
            return l;
        exceptions_str = qp_repr(ctx, l);
        JS_FreeValue(ctx, l);
    } else {
        exceptions_str = qp_repr(ctx, excs);
    }
    if (JS_IsException(exceptions_str))
        return exceptions_str;
    name = qp_exc_short_name(ctx, qp_type_of(ctx, self));
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, exceptions_str);
        return name;
    }
    res = qp_str_from_format(ctx, "%U(%R, %U)", name,
                             QP_IS_NULL(e->f[QP_EXC_F_EG_MSG]) ? JS_UNDEFINED : e->f[QP_EXC_F_EG_MSG],
                             exceptions_str);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, exceptions_str);
    return res;
}

/* derive(excs): BaseExceptionGroup(self.message, excs) */
static JSValue qp_exc_eg_derive(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    return qp_call2(ctx, QP_TYPE_VALUE(QP_EXC(ctx, BaseExceptionGroup)),
                    QP_IS_NULL(e->f[QP_EXC_F_EG_MSG]) ? JS_UNDEFINED : e->f[QP_EXC_F_EG_MSG],
                    args[0]);
}

/* exceptiongroup_subset: *result = orig.derive(excs) with orig's metadata
   copied, or QP_NULL if excs is empty. */
static int qp_exc_eg_subset(JSContext *ctx, JSValueConst orig, JSValueConst excs, JSValue *result)
{
    QPExcObject *oe = qp_exc_obj(orig), *ne;
    JSValue eg, notes;
    int r;

    *result = QP_NULL;
    if (qp_list_size(excs) == 0)
        return 0;
    eg = qp_call_method(ctx, orig, QP_ATOM(ctx, derive), &excs, 1, QP_NULL);
    if (JS_IsException(eg))
        return -1;
    if (!qp_exc_is_group(ctx, eg)) {
        qp_raise_type_error(ctx, "derive must return an instance of BaseExceptionGroup");
        goto error;
    }
    ne = qp_exc_obj(eg);
    if (!QP_IS_NULL(oe->traceback))
        qp_exc_slot_set(ctx, &ne->traceback, JS_DupValue(ctx, oe->traceback), FALSE);
    qp_exc_slot_set(ctx, &ne->context, JS_DupValue(ctx, oe->context), FALSE);
    ne->suppress_context = 1;   /* PyException_SetCause */
    qp_exc_slot_set(ctx, &ne->cause, JS_DupValue(ctx, oe->cause), FALSE);

    r = qp_getattr_opt(ctx, orig, QP_ATOM(ctx, __notes__), &notes);
    if (r < 0)
        goto error;
    if (r > 0) {
        if (qp_exc_sequence_check(ctx, notes)) {
            /* Make a copy so the parts have independent notes lists. */
            JSValue copy = qp_list_from_iterable(ctx, notes);
            JS_FreeValue(ctx, notes);
            if (JS_IsException(copy))
                goto error;
            r = qp_setattr(ctx, eg, QP_ATOM(ctx, __notes__), copy);
            JS_FreeValue(ctx, copy);
            if (r < 0)
                goto error;
        } else {
            /* notes of non-sequence type are ignored */
            JS_FreeValue(ctx, notes);
        }
    }
    *result = eg;
    return 0;
error:
    JS_FreeValue(ctx, eg);
    return -1;
}

enum {
    QP_EG_MATCH_BY_TYPE,        /* exception type or tuple thereof */
    QP_EG_MATCH_BY_PREDICATE,   /* callable returning truthy for matches */
    QP_EG_MATCH_INSTANCE_IDS,   /* identities of leaf exceptions (interpreter) */
};

typedef struct QPEgMatcher {
    int type;
    JSValueConst value;
    JSObject **ids;
    int nids, ids_size;
} QPEgMatcher;

static int qp_exc_eg_get_matcher_type(JSContext *ctx, JSValueConst value, int *type)
{
    if (qp_exc_callable_check(ctx, value) && !qp_type_from_value(value)) {
        *type = QP_EG_MATCH_BY_PREDICATE;
        return 0;
    }
    if (qp_exc_type_check(value)) {
        *type = QP_EG_MATCH_BY_TYPE;
        return 0;
    }
    if (qp_tuple_check_exact(ctx, value)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(value);
        for (i = 0; i < n; i++) {
            if (!qp_exc_type_check(QP_TUPLE_ITEMS(value)[i]))
                goto error;
        }
        *type = QP_EG_MATCH_BY_TYPE;
        return 0;
    }
error:
    qp_raise_type_error(ctx, "expected an exception type, a tuple of exception types, "
                        "or a callable (other than a class)");
    return -1;
}

static int qp_exc_eg_check_match(JSContext *ctx, JSValueConst exc, const QPEgMatcher *m)
{
    switch (m->type) {
    case QP_EG_MATCH_BY_TYPE:
        return qp_exc_given_matches(ctx, exc, m->value);
    case QP_EG_MATCH_BY_PREDICATE: {
        JSValue r = qp_call1(ctx, m->value, exc);
        int b;
        if (JS_IsException(r))
            return -1;
        b = qp_bool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    default:
        if (!qp_exc_is_group(ctx, exc)) {
            int i;
            JSObject *p = JS_VALUE_GET_OBJ(exc);
            for (i = 0; i < m->nids; i++) {
                if (m->ids[i] == p)
                    return 1;
            }
        }
        return 0;
    }
}

/* exceptiongroup_split_recursive: *pmatch / *prest new refs or QP_NULL. */
static int qp_exc_eg_split_recursive(JSContext *ctx, JSValueConst exc, const QPEgMatcher *m,
                                     BOOL construct_rest, JSValue *pmatch, JSValue *prest)
{
    QPExcObject *e;
    JSValue match_list = JS_UNDEFINED, rest_list = JS_UNDEFINED;
    Py_ssize_t i, num_excs;
    int is_match, retval = -1;

    *pmatch = QP_NULL;
    *prest = QP_NULL;
    is_match = qp_exc_eg_check_match(ctx, exc, m);
    if (is_match < 0)
        return -1;
    if (is_match) {
        /* Full match */
        *pmatch = JS_DupValue(ctx, exc);
        return 0;
    }
    if (!qp_exc_is_group(ctx, exc)) {
        /* Leaf exception and no match */
        if (construct_rest)
            *prest = JS_DupValue(ctx, exc);
        return 0;
    }
    /* Partial match */
    e = qp_exc_obj(exc);
    match_list = qp_list_new(ctx, 0);
    if (JS_IsException(match_list))
        return -1;
    if (construct_rest) {
        rest_list = qp_list_new(ctx, 0);
        if (JS_IsException(rest_list))
            goto done;
    }
    num_excs = QP_IS_NULL(e->f[QP_EXC_F_EG_EXCS]) ? 0 : QP_TUPLE_SIZE(e->f[QP_EXC_F_EG_EXCS]);
    for (i = 0; i < num_excs; i++) {
        JSValueConst sub = QP_TUPLE_ITEMS(e->f[QP_EXC_F_EG_EXCS])[i];
        JSValue rm, rr;
        int r;
        if (js_check_stack_overflow(ctx->rt, 0)) {
            qp_raise(ctx, QP_EXC(ctx, RecursionError),
                     "maximum recursion depth exceeded in exceptiongroup_split_recursive");
            goto done;
        }
        if (qp_exc_eg_split_recursive(ctx, sub, m, construct_rest, &rm, &rr) < 0)
            goto done;
        r = 0;
        if (!QP_IS_NULL(rm))
            r = qp_list_append(ctx, match_list, rm);
        if (r == 0 && !QP_IS_NULL(rr))
            r = qp_list_append(ctx, rest_list, rr);
        JS_FreeValue(ctx, rm);
        JS_FreeValue(ctx, rr);
        if (r < 0)
            goto done;
    }
    /* construct result */
    if (qp_exc_eg_subset(ctx, exc, match_list, pmatch) < 0)
        goto done;
    if (construct_rest) {
        if (qp_exc_eg_subset(ctx, exc, rest_list, prest) < 0) {
            JS_FreeValue(ctx, *pmatch);
            *pmatch = QP_NULL;
            goto done;
        }
    }
    retval = 0;
done:
    JS_FreeValue(ctx, match_list);
    JS_FreeValue(ctx, rest_list);
    return retval;
}

/* split(matcher_value) / subgroup(matcher_value) (magic 1 = split) */
static JSValue qp_exc_eg_split(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPEgMatcher m = { 0 };
    JSValue match, rest, items[2];

    if (!qp_exc_self(ctx, self))
        return JS_EXCEPTION;
    if (qp_exc_eg_get_matcher_type(ctx, args[0], &m.type) < 0)
        return JS_EXCEPTION;
    m.value = args[0];
    if (qp_exc_eg_split_recursive(ctx, self, &m, magic != 0, &match, &rest) < 0)
        return JS_EXCEPTION;
    if (!magic)
        return QP_IS_NULL(match) ? JS_UNDEFINED : match;
    items[0] = QP_IS_NULL(match) ? JS_UNDEFINED : match;
    items[1] = QP_IS_NULL(rest) ? JS_UNDEFINED : rest;
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_exc_eg_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qp_exc_eg_methods[] = {
    { "__class_getitem__", qp_exc_eg_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Exception groups are generic over the type of their contained exceptions" },
    { "derive", qp_exc_eg_derive, QP_METH_O, 0, "derive($self, excs, /)\n--\n\n" },
    { "split", qp_exc_eg_split, QP_METH_O, 1, "split($self, matcher_value, /)\n--\n\n" },
    { "subgroup", qp_exc_eg_split, QP_METH_O, 0, "subgroup($self, matcher_value, /)\n--\n\n" },
    { NULL },
};

static const QPGetSetDef qp_exc_eg_getsets[] = {
    { "message", qp_exc_field_get, NULL, QP_EXC_F_EG_MSG, "exception message" },
    { "exceptions", qp_exc_field_get, NULL, QP_EXC_F_EG_EXCS, "nested exceptions" },
    { NULL },
};

/* ---- except* support (Python/ceval.c, Objects/exceptions.c) ------------- */

/* collect_exception_group_leaf_ids */
static int qp_exc_eg_collect_leaf_ids(JSContext *ctx, JSValueConst exc, QPEgMatcher *m)
{
    QPExcObject *e;
    Py_ssize_t i, n;

    if (JS_IsUndefined(exc) || JS_IsNull(exc))
        return 0;
    if (!qp_exc_is_group(ctx, exc)) {
        if (m->nids == m->ids_size) {
            int ns = m->ids_size ? m->ids_size * 2 : 8;
            JSObject **nids = js_realloc(ctx, m->ids, ns * sizeof(*nids));
            if (!nids)
                return -1;
            m->ids = nids;
            m->ids_size = ns;
        }
        m->ids[m->nids++] = JS_VALUE_GET_OBJ(exc);
        return 0;
    }
    e = qp_exc_obj(exc);
    n = QP_IS_NULL(e->f[QP_EXC_F_EG_EXCS]) ? 0 : QP_TUPLE_SIZE(e->f[QP_EXC_F_EG_EXCS]);
    for (i = 0; i < n; i++) {
        if (js_check_stack_overflow(ctx->rt, 0)) {
            qp_raise(ctx, QP_EXC(ctx, RecursionError),
                     "maximum recursion depth exceeded in collect_exception_group_leaf_ids");
            return -1;
        }
        if (qp_exc_eg_collect_leaf_ids(ctx, QP_TUPLE_ITEMS(e->f[QP_EXC_F_EG_EXCS])[i], m) < 0)
            return -1;
    }
    return 0;
}

/* exception_group_projection: the sub-group of eg containing the leaf
   exceptions of the groups in keep (a tuple/array of n groups). */
static JSValue qp_exc_eg_projection(JSContext *ctx, JSValueConst eg, JSValueConst *keep, int n)
{
    QPEgMatcher m = { QP_EG_MATCH_INSTANCE_IDS };
    JSValue match, rest;
    int i, r;

    m.value = JS_UNDEFINED;
    for (i = 0; i < n; i++) {
        if (qp_exc_eg_collect_leaf_ids(ctx, keep[i], &m) < 0) {
            js_free(ctx, m.ids);
            return JS_EXCEPTION;
        }
    }
    r = qp_exc_eg_split_recursive(ctx, eg, &m, FALSE, &match, &rest);
    js_free(ctx, m.ids);
    if (r < 0)
        return JS_EXCEPTION;
    return QP_IS_NULL(match) ? JS_UNDEFINED : match;
}

static BOOL qp_exc_same_metadata(JSValueConst a, JSValueConst b)
{
    QPExcObject *e1 = qp_exc_obj(a), *e2 = qp_exc_obj(b);
    if (!e1 || !e2)
        return FALSE;
    return qp_exc_same(e1->traceback, e2->traceback) &&
        qp_exc_same(e1->cause, e2->cause) &&
        qp_exc_same(e1->context, e2->context);
}

/* _PyExc_CreateExceptionGroup(msg, excs) */
static JSValue qp_exc_create_group(JSContext *ctx, const char *msg, JSValueConst excs)
{
    JSValue m = qp_str_from_cstr(ctx, msg), res;
    if (JS_IsException(m))
        return m;
    res = qp_call2(ctx, QP_TYPE_VALUE(QP_EXC(ctx, BaseExceptionGroup)), m, excs);
    JS_FreeValue(ctx, m);
    return res;
}

/* _PyExc_PrepReraiseStar(orig, excs): the exception to raise at the end of
   a try-except* (None if nothing).  excs is the list of exceptions raised
   or reraised by the except* clauses (None entries allowed). */
static JSValue qp_exc_prep_reraise_star(JSContext *ctx, JSValueConst orig, JSValueConst excs)
{
    JSValue tup, raised_list = JS_UNDEFINED, reraised_tup = JS_UNDEFINED;
    JSValue reraised_list = JS_UNDEFINED, reraised_eg, result = JS_EXCEPTION;
    Py_ssize_t i, numexcs, num_raised;

    tup = qp_tuple_from_iterable(ctx, excs);
    if (JS_IsException(tup))
        return tup;
    numexcs = QP_TUPLE_SIZE(tup);
    if (numexcs == 0) {
        JS_FreeValue(ctx, tup);
        return JS_UNDEFINED;
    }
    if (!qp_exc_is_group(ctx, orig)) {
        /* a naked exception was caught and wrapped: only one except* clause
           could have executed, so there is at most one exception to raise */
        result = JS_DupValue(ctx, QP_TUPLE_ITEMS(tup)[0]);
        JS_FreeValue(ctx, tup);
        return result;
    }
    raised_list = qp_list_new(ctx, 0);
    if (JS_IsException(raised_list))
        goto done;
    reraised_list = qp_list_new(ctx, 0);
    if (JS_IsException(reraised_list))
        goto done;
    /* Split excs into raised and reraised by comparing metadata with orig */
    for (i = 0; i < numexcs; i++) {
        JSValueConst e = QP_TUPLE_ITEMS(tup)[i];
        if (JS_IsUndefined(e) || JS_IsNull(e))
            continue;
        if (qp_list_append(ctx, qp_exc_same_metadata(e, orig) ? reraised_list : raised_list, e) < 0)
            goto done;
    }
    reraised_tup = qp_list_as_tuple(ctx, reraised_list);
    if (JS_IsException(reraised_tup))
        goto done;
    reraised_eg = qp_exc_eg_projection(ctx, orig, (JSValueConst *)QP_TUPLE_ITEMS(reraised_tup),
                                       (int)QP_TUPLE_SIZE(reraised_tup));
    if (JS_IsException(reraised_eg))
        goto done;
    num_raised = qp_list_size(raised_list);
    if (num_raised == 0) {
        result = reraised_eg;
    } else {
        int r = 0;
        if (!JS_IsUndefined(reraised_eg))
            r = qp_list_append(ctx, raised_list, reraised_eg);
        JS_FreeValue(ctx, reraised_eg);
        if (r < 0)
            goto done;
        if (qp_list_size(raised_list) > 1)
            result = qp_exc_create_group(ctx, "", raised_list);
        else
            result = qp_getitem(ctx, raised_list, JS_NewInt32(ctx, 0));
    }
done:
    JS_FreeValue(ctx, tup);
    JS_FreeValue(ctx, raised_list);
    JS_FreeValue(ctx, reraised_list);
    JS_FreeValue(ctx, reraised_tup);
    return result;
}

#define QP_CANNOT_CATCH_MSG \
    "catching classes that do not inherit from BaseException is not allowed"

/* _PyEval_CheckExceptStarTypeValid */
static int qp_exc_check_except_star_type(JSContext *ctx, JSValueConst right)
{
    QPType *beg = QP_EXC(ctx, BaseExceptionGroup);
    BOOL is_subclass = FALSE;

    if (qp_tuple_check(ctx, right)) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(right);
        for (i = 0; i < n; i++) {
            if (!qp_exc_type_check(QP_TUPLE_ITEMS(right)[i])) {
                qp_raise_type_error(ctx, QP_CANNOT_CATCH_MSG);
                return -1;
            }
        }
        for (i = 0; i < n && !is_subclass; i++) {
            int r = qp_issubclass(ctx, QP_TUPLE_ITEMS(right)[i], QP_TYPE_VALUE(beg));
            if (r < 0)
                return -1;
            is_subclass = r;
        }
    } else {
        int r;
        if (!qp_exc_type_check(right)) {
            qp_raise_type_error(ctx, QP_CANNOT_CATCH_MSG);
            return -1;
        }
        r = qp_issubclass(ctx, right, QP_TYPE_VALUE(beg));
        if (r < 0)
            return -1;
        is_subclass = r;
    }
    if (is_subclass) {
        qp_raise_type_error(ctx, "catching ExceptionGroup with except* is not allowed. "
                            "Use except instead.");
        return -1;
    }
    return 0;
}

/* CHECK_EG_MATCH: type validation, _PyEval_ExceptionGroupMatch, then
   PyErr_SetHandledException(match) when something matched. */
static int qp_exc_group_match(JSContext *ctx, JSValueConst exc, JSValueConst match_type,
                              JSValue *pmatch, JSValue *prest)
{
    JSValue match = JS_UNDEFINED, rest = JS_UNDEFINED;

    *pmatch = JS_UNDEFINED;
    *prest = JS_UNDEFINED;
    if (qp_exc_check_except_star_type(ctx, match_type) < 0)
        return -1;
    if (JS_IsUndefined(exc) || JS_IsNull(exc))
        return 0;
    if (qp_exc_given_matches(ctx, exc, match_type)) {
        /* Full match of exc itself */
        if (qp_exc_is_group(ctx, exc)) {
            match = JS_DupValue(ctx, exc);
        } else {
            /* naked exception - wrap it.  CPython gives the wrapper a
               traceback of the current frame only (at the except* clause) */
            JSValue excs = qp_tuple_pack(ctx, 1, &exc), tb;
            if (JS_IsException(excs))
                return -1;
            match = qp_exc_create_group(ctx, "", excs);
            JS_FreeValue(ctx, excs);
            if (JS_IsException(match))
                return -1;
            tb = qp_tb_from_current_frame(ctx);
            if (JS_IsException(tb)) {
                JS_FreeValue(ctx, match);
                return -1;
            }
            if (!JS_IsUndefined(tb))
                qp_exc_set_traceback_raw(ctx, match, tb);
        }
        rest = JS_UNDEFINED;
    } else if (qp_exc_is_group(ctx, exc)) {
        /* Partial match */
        JSValue pair = qp_call_method(ctx, exc, QP_ATOM(ctx, split), &match_type, 1, QP_NULL);
        if (JS_IsException(pair))
            return -1;
        if (!qp_tuple_check_exact(ctx, pair)) {
            JSValue n1 = qp_exc_tp_name(ctx, qp_type_of(ctx, exc));
            JSValue n2 = qp_exc_tp_name(ctx, qp_type_of(ctx, pair));
            if (!JS_IsException(n1) && !JS_IsException(n2))
                qp_raise_type_error(ctx, "%U.split must return a tuple, not %U", n1, n2);
            JS_FreeValue(ctx, n1);
            JS_FreeValue(ctx, n2);
            JS_FreeValue(ctx, pair);
            return -1;
        }
        /* allow tuples of length > 2 for backwards compatibility */
        if (QP_TUPLE_SIZE(pair) < 2) {
            JSValue n1 = qp_exc_tp_name(ctx, qp_type_of(ctx, exc));
            if (!JS_IsException(n1))
                qp_raise_type_error(ctx, "%U.split must return a 2-tuple, got tuple of size %zd",
                                    n1, QP_TUPLE_SIZE(pair));
            JS_FreeValue(ctx, n1);
            JS_FreeValue(ctx, pair);
            return -1;
        }
        match = JS_DupValue(ctx, QP_TUPLE_ITEMS(pair)[0]);
        rest = JS_DupValue(ctx, QP_TUPLE_ITEMS(pair)[1]);
        JS_FreeValue(ctx, pair);
    } else {
        /* no match */
        rest = JS_DupValue(ctx, exc);
    }
    if (!JS_IsUndefined(match) && !JS_IsNull(match)) {
        QPExcStackItem *item = QP_STATE(ctx)->exc_info;
        if (item) {
            JSValue old = item->exc_value;
            item->exc_value = JS_DupValue(ctx, match);
            JS_FreeValue(ctx, old);
        }
    }
    *pmatch = match;
    *prest = rest;
    return 0;
}

/* ========================================================================= */
/* ImportError                                                                */
/* ========================================================================= */

static int qp_exc_importerror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "name", "path", "name_from" };
    Py_ssize_t nargs = qp_nargs(nargsf);
    JSValueConst out[3];
    QPExcObject *e;

    if (qp_exc_base_init_nokw(ctx, self, args, nargsf) < 0)
        return -1;
    if (qp_exc_parse_kwonly(ctx, "ImportError", kwlist, 3, args, (int)nargs, kwnames, out) < 0)
        return -1;
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_IMP_NAME], JS_DupValue(ctx, out[0]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_IMP_PATH], JS_DupValue(ctx, out[1]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_IMP_NAME_FROM], JS_DupValue(ctx, out[2]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_IMP_MSG],
                    QP_TUPLE_SIZE(e->args) == 1 ? JS_DupValue(ctx, QP_TUPLE_ITEMS(e->args)[0]) : QP_NULL,
                    FALSE);
    return 0;
}

static JSValue qp_exc_importerror_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(e->f[QP_EXC_F_IMP_MSG]) && qp_str_check_exact(e->f[QP_EXC_F_IMP_MSG]))
        return JS_DupValue(ctx, e->f[QP_EXC_F_IMP_MSG]);
    return qp_exc_base_str(ctx, self);
}

/* Copy of the instance dict (or a new dict) with the given payload fields
   added under their attribute names (ImportError/AttributeError
   __getstate__).  Returns None when there is nothing to save. */
static JSValue qp_exc_getstate_fields(JSContext *ctx, JSValueConst self, const int *fields,
                                      const JSAtom *names, int n, BOOL add_args)
{
    QPExcObject *e = qp_exc_obj(self);
    JSValue d, state;
    BOOL any = add_args;
    int i;

    for (i = 0; i < n; i++)
        any |= !QP_IS_NULL(e->f[fields[i]]);
    d = qp_exc_instance_dict(ctx, self);
    if (JS_IsException(d))
        return d;
    if (!any)
        return QP_IS_NULL(d) ? JS_UNDEFINED : d;
    if (QP_IS_NULL(d)) {
        state = qp_dict_new(ctx);
    } else {
        state = qp_dict_copy(ctx, d);
        JS_FreeValue(ctx, d);
    }
    if (JS_IsException(state))
        return state;
    for (i = 0; i < n; i++) {
        if (!QP_IS_NULL(e->f[fields[i]]) &&
            qp_dict_set_item_atom(ctx, state, names[i], e->f[fields[i]]) < 0)
            goto fail;
    }
    if (add_args && qp_dict_set_item_atom(ctx, state, QP_ATOM(ctx, args), e->args) < 0)
        goto fail;
    return state;
fail:
    JS_FreeValue(ctx, state);
    return JS_EXCEPTION;
}

/* (type, args) or (type, args, state) */
static JSValue qp_exc_reduce_with_state(JSContext *ctx, JSValueConst self, JSValue state, BOOL keep_none)
{
    QPExcObject *e = qp_exc_obj(self);
    JSValue items[3];
    if (JS_IsException(state))
        return state;
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = JS_DupValue(ctx, e->args);
    if (JS_IsUndefined(state) && !keep_none)
        return qp_tuple_new_steal(ctx, 2, items);
    items[2] = state;
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_exc_importerror_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    int fields[3] = { QP_EXC_F_IMP_NAME, QP_EXC_F_IMP_PATH, QP_EXC_F_IMP_NAME_FROM };
    JSAtom names[3];
    if (!qp_exc_self(ctx, self))
        return JS_EXCEPTION;
    names[0] = QP_ATOM(ctx, name);
    names[1] = QP_ATOM(ctx, path);
    names[2] = QP_ATOM(ctx, name_from);
    return qp_exc_reduce_with_state(ctx, self,
                                    qp_exc_getstate_fields(ctx, self, fields, names, 3, FALSE),
                                    FALSE);
}

static JSValue qp_exc_importerror_repr(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue r, head, res;
    BOOL hasargs;
    Py_ssize_t len;
    if (!e)
        return JS_EXCEPTION;
    if (QP_IS_NULL(e->f[QP_EXC_F_IMP_NAME]) && QP_IS_NULL(e->f[QP_EXC_F_IMP_PATH]))
        return qp_exc_base_repr(ctx, self);
    hasargs = QP_TUPLE_SIZE(e->args) != 0;
    r = qp_exc_base_repr(ctx, self);
    if (JS_IsException(r))
        return r;
    len = qp_str_len(ctx, r);
    head = qp_str_substring(ctx, r, 0, len - 1);
    JS_FreeValue(ctx, r);
    if (JS_IsException(head))
        return head;
    if (!QP_IS_NULL(e->f[QP_EXC_F_IMP_NAME])) {
        res = qp_str_from_format(ctx, "%U%sname=%R", head, hasargs ? ", " : "",
                                 e->f[QP_EXC_F_IMP_NAME]);
        JS_FreeValue(ctx, head);
        if (JS_IsException(res))
            return res;
        head = res;
        hasargs = TRUE;
    }
    if (!QP_IS_NULL(e->f[QP_EXC_F_IMP_PATH])) {
        res = qp_str_from_format(ctx, "%U%spath=%R", head, hasargs ? ", " : "",
                                 e->f[QP_EXC_F_IMP_PATH]);
        JS_FreeValue(ctx, head);
        if (JS_IsException(res))
            return res;
        head = res;
    }
    res = qp_str_from_format(ctx, "%U)", head);
    JS_FreeValue(ctx, head);
    return res;
}

static const QPMethodDef qp_exc_importerror_methods[] = {
    { "__reduce__", qp_exc_importerror_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPGetSetDef qp_exc_importerror_getsets[] = {
    { "msg", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_IMP_MSG, "exception message" },
    { "name", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_IMP_NAME, "module name" },
    { "path", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_IMP_PATH, "module path" },
    { "name_from", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_IMP_NAME_FROM,
      "name imported from module" },
    { NULL },
};

/* ========================================================================= */
/* OSError                                                                    */
/* ========================================================================= */

static JSValue qp_exc_oserror_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames);
static int qp_exc_oserror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames);

/* errno -> OSError subclass (CPython errnomap), NULL if none. */
static QPType *qp_exc_errno_type(JSContext *ctx, int64_t err)
{
    switch (err) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EALREADY:
    case EINPROGRESS:
        return QP_EXC(ctx, BlockingIOError);
    case EPIPE:
#ifdef ESHUTDOWN
    case ESHUTDOWN:
#endif
        return QP_EXC(ctx, BrokenPipeError);
    case ECHILD:
        return QP_EXC(ctx, ChildProcessError);
    case ECONNABORTED:
        return QP_EXC(ctx, ConnectionAbortedError);
    case ECONNREFUSED:
        return QP_EXC(ctx, ConnectionRefusedError);
    case ECONNRESET:
        return QP_EXC(ctx, ConnectionResetError);
    case EEXIST:
        return QP_EXC(ctx, FileExistsError);
    case ENOENT:
        return QP_EXC(ctx, FileNotFoundError);
    case EISDIR:
        return QP_EXC(ctx, IsADirectoryError);
    case ENOTDIR:
        return QP_EXC(ctx, NotADirectoryError);
    case EINTR:
        return QP_EXC(ctx, InterruptedError);
    case EACCES:
    case EPERM:
#ifdef ENOTCAPABLE
    case ENOTCAPABLE:
#endif
        return QP_EXC(ctx, PermissionError);
    case ESRCH:
        return QP_EXC(ctx, ProcessLookupError);
    case ETIMEDOUT:
        return QP_EXC(ctx, TimeoutError);
    default:
        return NULL;
    }
}

/* oserror_use_init: a subclass defining __init__ (but not __new__) parses
   its arguments in __init__. */
static BOOL qp_exc_oserror_use_init(QPType *type)
{
    return type->slots.tp_init != qp_exc_oserror_init &&
        type->slots.tp_new == qp_exc_oserror_new;
}

typedef struct QPOSErrorArgs {
    JSValueConst myerrno, strerror, filename, filename2;
} QPOSErrorArgs;

/* oserror_parse_args (2..5 arguments: errno, strerror, filename,
   winerror (ignored), filename2). */
static void qp_exc_oserror_parse_args(Py_ssize_t nargs, JSValueConst *args, QPOSErrorArgs *a)
{
    a->myerrno = a->strerror = a->filename = a->filename2 = QP_NULL;
    if (nargs >= 2 && nargs <= 5) {
        a->myerrno = args[0];
        a->strerror = args[1];
        if (nargs >= 3)
            a->filename = args[2];
        if (nargs >= 5)
            a->filename2 = args[4];
    }
}

/* oserror_init: fills the fields and args (positional arguments, cut to
   (errno, strerror) when a filename is given). */
static int qp_exc_oserror_fill(JSContext *ctx, JSValueConst self, Py_ssize_t nargs,
                               JSValueConst *args, const QPOSErrorArgs *a)
{
    QPExcObject *e = qp_exc_obj(self);
    JSValue tup;
    BOOL cut = FALSE;

    if (!QP_IS_NULL(a->filename) && !JS_IsUndefined(a->filename)) {
        if (qp_type_of(ctx, self) == QP_EXC(ctx, BlockingIOError) &&
            qp_exc_number_check(ctx, a->filename)) {
            /* BlockingIOError's 3rd argument can be the number of
               characters written. */
            Py_ssize_t n;
            if (qp_as_ssize(ctx, a->filename, QP_EXC(ctx, ValueError), &n) < 0)
                return -1;
            e->written = n;
        } else {
            qp_exc_slot_set(ctx, &e->f[QP_EXC_F_OS_FILENAME], JS_DupValue(ctx, a->filename), FALSE);
            if (!QP_IS_NULL(a->filename2) && !JS_IsUndefined(a->filename2))
                qp_exc_slot_set(ctx, &e->f[QP_EXC_F_OS_FILENAME2],
                                JS_DupValue(ctx, a->filename2), FALSE);
            /* filename, filename2 and winerror are removed from args */
            cut = nargs >= 2 && nargs <= 5;
        }
    }
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_OS_ERRNO],
                    QP_IS_NULL(a->myerrno) ? QP_NULL : JS_DupValue(ctx, a->myerrno), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_OS_STRERROR],
                    QP_IS_NULL(a->strerror) ? QP_NULL : JS_DupValue(ctx, a->strerror), FALSE);
    tup = qp_tuple_pack(ctx, cut ? 2 : nargs, args);
    if (JS_IsException(tup))
        return -1;
    qp_exc_slot_set(ctx, &e->args, tup, FALSE);
    return 0;
}

static JSValue qp_exc_oserror_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPOSErrorArgs a;
    JSValue self, empty;

    a.myerrno = a.strerror = a.filename = a.filename2 = QP_NULL;
    if (!qp_exc_oserror_use_init(type)) {
        if (qp_exc_no_keywords(ctx, type, kwnames) < 0)
            return JS_EXCEPTION;
        qp_exc_oserror_parse_args(nargs, args, &a);
        if (!QP_IS_NULL(a.myerrno) && qp_long_check(ctx, a.myerrno) && type == QP_EXC(ctx, OSError)) {
            JSValue iv = qp_long_value(ctx, a.myerrno);
            int64_t v;
            int overflow = 0;
            QPType *nt;
            if (JS_IsException(iv))
                return iv;
            qp_long_as_int64_overflow(iv, &v, &overflow);
            JS_FreeValue(ctx, iv);
            if (!overflow && (nt = qp_exc_errno_type(ctx, v)) != NULL)
                type = nt;
        }
    }
    empty = qp_tuple_pack(ctx, 0, NULL);
    if (JS_IsException(empty))
        return empty;
    self = qp_exc_alloc(ctx, type, empty);
    if (JS_IsException(self))
        return self;
    if (!qp_exc_oserror_use_init(type)) {
        if (qp_exc_oserror_fill(ctx, self, nargs, args, &a) < 0) {
            JS_FreeValue(ctx, self);
            return JS_EXCEPTION;
        }
    }
    return self;
}

static int qp_exc_oserror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t = qp_type_of(ctx, self);
    QPOSErrorArgs a;

    if (!qp_exc_self(ctx, self))
        return -1;
    if (!qp_exc_oserror_use_init(t))
        return 0;   /* everything already done in OSError.__new__ */
    if (qp_exc_no_keywords(ctx, t, kwnames) < 0)
        return -1;
    qp_exc_oserror_parse_args(nargs, args, &a);
    return qp_exc_oserror_fill(ctx, self, nargs, args, &a);
}

static JSValue qp_exc_oserror_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValueConst no, se;
    if (!e)
        return JS_EXCEPTION;
#define QP_OR_NONE(x) (QP_IS_NULL(x) ? JS_UNDEFINED : (x))
    no = QP_OR_NONE(e->f[QP_EXC_F_OS_ERRNO]);
    se = QP_OR_NONE(e->f[QP_EXC_F_OS_STRERROR]);
    if (!QP_IS_NULL(e->f[QP_EXC_F_OS_FILENAME])) {
        if (!QP_IS_NULL(e->f[QP_EXC_F_OS_FILENAME2]))
            return qp_str_from_format(ctx, "[Errno %S] %S: %R -> %R", no, se,
                                      e->f[QP_EXC_F_OS_FILENAME], e->f[QP_EXC_F_OS_FILENAME2]);
        return qp_str_from_format(ctx, "[Errno %S] %S: %R", no, se, e->f[QP_EXC_F_OS_FILENAME]);
    }
#undef QP_OR_NONE
    if (!QP_IS_NULL(e->f[QP_EXC_F_OS_ERRNO]) && !QP_IS_NULL(e->f[QP_EXC_F_OS_STRERROR]))
        return qp_str_from_format(ctx, "[Errno %S] %S", no, se);
    return qp_exc_base_str(ctx, self);
}

static JSValue qp_exc_oserror_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue a, d, items[3];
    if (!e)
        return JS_EXCEPTION;
    /* args is only the first two real arguments if a file name was given */
    if (QP_TUPLE_SIZE(e->args) == 2 && !QP_IS_NULL(e->f[QP_EXC_F_OS_FILENAME])) {
        JSValue v[5];
        int n = QP_IS_NULL(e->f[QP_EXC_F_OS_FILENAME2]) ? 3 : 5;
        v[0] = JS_DupValue(ctx, QP_TUPLE_ITEMS(e->args)[0]);
        v[1] = JS_DupValue(ctx, QP_TUPLE_ITEMS(e->args)[1]);
        v[2] = JS_DupValue(ctx, e->f[QP_EXC_F_OS_FILENAME]);
        if (n == 5) {
            v[3] = JS_UNDEFINED;   /* winerror */
            v[4] = JS_DupValue(ctx, e->f[QP_EXC_F_OS_FILENAME2]);
        }
        a = qp_tuple_new_steal(ctx, n, v);
        if (JS_IsException(a))
            return a;
    } else {
        a = JS_DupValue(ctx, e->args);
    }
    d = qp_exc_instance_dict(ctx, self);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, a);
        return d;
    }
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = a;
    if (QP_IS_NULL(d))
        return qp_tuple_new_steal(ctx, 2, items);
    items[2] = d;
    return qp_tuple_new_steal(ctx, 3, items);
}

static JSValue qp_exc_oserror_written_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    if (e->written == -1)
        return qp_raise(ctx, QP_EXC(ctx, AttributeError), "characters_written");
    return qp_long_from_int64(ctx, e->written);
}

static int qp_exc_oserror_written_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    Py_ssize_t n;
    if (!e)
        return -1;
    if (QP_IS_NULL(value)) {
        if (e->written == -1) {
            qp_raise(ctx, QP_EXC(ctx, AttributeError), "characters_written");
            return -1;
        }
        e->written = -1;
        return 0;
    }
    if (qp_as_ssize(ctx, value, QP_EXC(ctx, ValueError), &n) < 0)
        return -1;
    e->written = n;
    return 0;
}

static const QPMethodDef qp_exc_oserror_methods[] = {
    { "__reduce__", qp_exc_oserror_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPGetSetDef qp_exc_oserror_getsets[] = {
    { "errno", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_OS_ERRNO, "POSIX exception code" },
    { "strerror", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_OS_STRERROR, "exception strerror" },
    { "filename", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_OS_FILENAME, "exception filename" },
    { "filename2", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_OS_FILENAME2,
      "second exception filename" },
    { "characters_written", qp_exc_oserror_written_get, qp_exc_oserror_written_set, 0, NULL },
    { NULL },
};

/* PyErr_SetFromErrnoWithFilenameObject */
static JSValue qp_raise_os_error(JSContext *ctx, int errnum, JSValueConst filename)
{
    JSValueConst items[3];
    JSValue msg, exc;
    int n = 2;

    msg = qp_str_from_cstr(ctx, errnum != 0 ? strerror(errnum) : "Error");
    if (JS_IsException(msg))
        return JS_EXCEPTION;
    items[0] = JS_NewInt32(ctx, errnum);
    items[1] = msg;
    if (!QP_IS_NULL(filename) && !JS_IsUndefined(filename) && !JS_IsNull(filename)) {
        items[2] = filename;
        n = 3;
    }
    exc = qp_exc_new(ctx, QP_EXC(ctx, OSError), n, items);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* ========================================================================= */
/* NameError, AttributeError                                                  */
/* ========================================================================= */

static int qp_exc_nameerror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "name" };
    JSValueConst out[1];
    if (qp_exc_base_init_nokw(ctx, self, args, nargsf) < 0)
        return -1;
    if (qp_exc_parse_kwonly(ctx, "NameError", kwlist, 1, args, (int)qp_nargs(nargsf),
                            kwnames, out) < 0)
        return -1;
    qp_exc_slot_set(ctx, &qp_exc_obj(self)->f[QP_EXC_F_NAME_NAME], JS_DupValue(ctx, out[0]), FALSE);
    return 0;
}

static const QPGetSetDef qp_exc_nameerror_getsets[] = {
    { "name", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_NAME_NAME, "name" },
    { NULL },
};

static int qp_exc_attributeerror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      size_t nargsf, JSValueConst kwnames)
{
    static const char *const kwlist[] = { "name", "obj" };
    JSValueConst out[2];
    QPExcObject *e;
    if (qp_exc_base_init_nokw(ctx, self, args, nargsf) < 0)
        return -1;
    if (qp_exc_parse_kwonly(ctx, "AttributeError", kwlist, 2, args, (int)qp_nargs(nargsf),
                            kwnames, out) < 0)
        return -1;
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_ATTR_NAME], JS_DupValue(ctx, out[0]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_ATTR_OBJ], JS_DupValue(ctx, out[1]), FALSE);
    return 0;
}

/* AttributeError.__getstate__ (obj is deliberately not pickled) */
static JSValue qp_exc_attributeerror_getstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                              int nargs, JSValueConst kwnames, int magic)
{
    int fields[1] = { QP_EXC_F_ATTR_NAME };
    JSAtom names[1];
    if (!qp_exc_self(ctx, self))
        return JS_EXCEPTION;
    names[0] = QP_ATOM(ctx, name);
    return qp_exc_getstate_fields(ctx, self, fields, names, 1, TRUE);
}

static JSValue qp_exc_attributeerror_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                            int nargs, JSValueConst kwnames, int magic)
{
    if (!qp_exc_self(ctx, self))
        return JS_EXCEPTION;
    return qp_exc_reduce_with_state(ctx, self,
                                    qp_exc_attributeerror_getstate(ctx, self, NULL, 0, QP_NULL, 0),
                                    TRUE);
}

static const QPMethodDef qp_exc_attributeerror_methods[] = {
    { "__getstate__", qp_exc_attributeerror_getstate, QP_METH_NOARGS, 0, NULL },
    { "__reduce__", qp_exc_attributeerror_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPGetSetDef qp_exc_attributeerror_getsets[] = {
    { "name", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_ATTR_NAME, "attribute name" },
    { "obj", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_ATTR_OBJ, "object" },
    { NULL },
};

/* ========================================================================= */
/* SyntaxError                                                                */
/* ========================================================================= */

static int qp_exc_syntaxerror_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t lenargs = qp_nargs(nargsf), n, i;
    QPExcObject *e;
    JSValue info;

    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    e = qp_exc_obj(self);
    if (lenargs >= 1)
        qp_exc_slot_set(ctx, &e->f[QP_EXC_F_SE_MSG], JS_DupValue(ctx, args[0]), FALSE);
    if (lenargs == 2) {
        info = qp_tuple_from_iterable(ctx, args[1]);
        if (JS_IsException(info))
            return -1;
        n = QP_TUPLE_SIZE(info);
        if (n < 4 || n > 7) {
            JS_FreeValue(ctx, info);
            return qp_exc_argcount_error(ctx, NULL, 4, 7, n);
        }
        /* filename, lineno, offset, text, [end_lineno, end_offset, metadata] */
        for (i = 0; i < 7; i++) {
            static const int idx[7] = {
                QP_EXC_F_SE_FILENAME, QP_EXC_F_SE_LINENO, QP_EXC_F_SE_OFFSET, QP_EXC_F_SE_TEXT,
                QP_EXC_F_SE_END_LINENO, QP_EXC_F_SE_END_OFFSET, QP_EXC_F_SE_METADATA,
            };
            qp_exc_slot_set(ctx, &e->f[idx[i]],
                            i < n ? JS_DupValue(ctx, QP_TUPLE_ITEMS(info)[i]) : QP_NULL, FALSE);
        }
        JS_FreeValue(ctx, info);
        if (!QP_IS_NULL(e->f[QP_EXC_F_SE_END_LINENO]) && QP_IS_NULL(e->f[QP_EXC_F_SE_END_OFFSET])) {
            qp_raise_type_error(ctx, "end_offset must be provided when end_lineno is provided");
            return -1;
        }
    }
    return 0;
}

/* my_basename: the part after the last '/'. */
static JSValue qp_exc_basename(JSContext *ctx, JSValueConst name)
{
    qp_strview v;
    Py_ssize_t i, offset = 0, size;
    JSValue s = qp_str_value(ctx, name);
    if (JS_IsException(s))
        return s;
    if (qp_str_view_acquire(ctx, s, &v) < 0) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    size = v.len;
    for (i = 0; i < size; i++) {
        if (QP_SL_READ(v.kind, v.data, i) == '/')
            offset = i + 1;
    }
    qp_str_view_release(ctx, &v);
    if (offset != 0) {
        JSValue r = qp_str_substring(ctx, s, offset, size);
        JS_FreeValue(ctx, s);
        return r;
    }
    return s;
}

static JSValue qp_exc_syntaxerror_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue filename = QP_NULL, res;
    JSValueConst msg;
    BOOL have_lineno;
    int64_t lineno = -1;
    int overflow = 0;

    if (!e)
        return JS_EXCEPTION;
    msg = QP_IS_NULL(e->f[QP_EXC_F_SE_MSG]) ? JS_UNDEFINED : e->f[QP_EXC_F_SE_MSG];
    if (!QP_IS_NULL(e->f[QP_EXC_F_SE_FILENAME]) && qp_str_check(ctx, e->f[QP_EXC_F_SE_FILENAME])) {
        filename = qp_exc_basename(ctx, e->f[QP_EXC_F_SE_FILENAME]);
        if (JS_IsException(filename))
            return filename;
    }
    have_lineno = !QP_IS_NULL(e->f[QP_EXC_F_SE_LINENO]) && qp_long_check_exact(e->f[QP_EXC_F_SE_LINENO]);
    if (QP_IS_NULL(filename) && !have_lineno)
        return qp_str(ctx, msg);
    if (have_lineno) {
        /* overflow is ignored: -1 is printed, like CPython */
        if (qp_long_as_int64_overflow(e->f[QP_EXC_F_SE_LINENO], &lineno, &overflow) < 0 || overflow)
            lineno = -1;
    }
    if (!QP_IS_NULL(filename) && have_lineno)
        res = qp_str_from_format(ctx, "%S (%U, line %lld)", msg, filename, (long long)lineno);
    else if (!QP_IS_NULL(filename))
        res = qp_str_from_format(ctx, "%S (%U)", msg, filename);
    else
        res = qp_str_from_format(ctx, "%S (line %lld)", msg, (long long)lineno);
    JS_FreeValue(ctx, filename);
    return res;
}

static const QPGetSetDef qp_exc_syntaxerror_getsets[] = {
    { "msg", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_MSG, "exception msg" },
    { "filename", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_FILENAME, "exception filename" },
    { "lineno", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_LINENO, "exception lineno" },
    { "offset", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_OFFSET, "exception offset" },
    { "text", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_TEXT, "exception text" },
    { "end_lineno", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_END_LINENO,
      "exception end lineno" },
    { "end_offset", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_END_OFFSET,
      "exception end offset" },
    { "print_file_and_line", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_PFAL,
      "exception print_file_and_line" },
    { "_metadata", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_SE_METADATA,
      "exception private metadata" },
    { NULL },
};

/* ========================================================================= */
/* KeyError                                                                   */
/* ========================================================================= */

/* str(KeyError(k)) is repr(k), so {}[''] prints KeyError: '' */
static JSValue qp_exc_keyerror_str(JSContext *ctx, JSValueConst self)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    if (QP_TUPLE_SIZE(e->args) == 1)
        return qp_repr(ctx, QP_TUPLE_ITEMS(e->args)[0]);
    return qp_exc_base_str(ctx, self);
}

/* ========================================================================= */
/* UnicodeError subclasses                                                    */
/* ========================================================================= */

/* check_unicode_error_attribute */
static int qp_exc_check_ue_attr(JSContext *ctx, JSValueConst attr, const char *name, BOOL as_bytes)
{
    if (QP_IS_NULL(attr)) {
        qp_raise_type_error(ctx, "UnicodeError '%s' attribute is not set", name);
        return -1;
    }
    if (!(as_bytes ? qp_bytes_check(ctx, attr) : qp_str_check(ctx, attr))) {
        qp_raise_type_error(ctx, "UnicodeError '%s' attribute must be a %s",
                            name, as_bytes ? "bytes" : "string");
        return -1;
    }
    return 0;
}

/* PyArg_ParseTuple(args, fmt) for the UnicodeError constructors: fmt
   letters U (str), O (any), n (Py_ssize_t); no function name. */
static int qp_exc_ue_parse(JSContext *ctx, const char *fmt, JSValueConst *args, Py_ssize_t nargs,
                           JSValueConst *objs, Py_ssize_t *ints)
{
    int i, n = (int)strlen(fmt), ni = 0;
    if (nargs != n)
        return qp_exc_argcount_error(ctx, NULL, n, n, nargs);
    for (i = 0; i < n; i++) {
        objs[i] = args[i];
        if (fmt[i] == 'U') {
            if (qp_exc_arg_str(ctx, NULL, i + 1, args[i]) < 0)
                return -1;
        } else if (fmt[i] == 'n') {
            if (qp_as_ssize(ctx, args[i], QP_EXC(ctx, OverflowError), &ints[ni++]) < 0)
                return -1;
        }
    }
    return 0;
}

static int qp_exc_ue_encode_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    JSValueConst o[5];
    Py_ssize_t iv[2];
    QPExcObject *e;
    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    if (qp_exc_ue_parse(ctx, "UUnnU", args, qp_nargs(nargsf), o, iv) < 0)
        return -1;
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_ENCODING], JS_DupValue(ctx, o[0]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_OBJECT], JS_DupValue(ctx, o[1]), FALSE);
    e->start = iv[0];
    e->end = iv[1];
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_REASON], JS_DupValue(ctx, o[4]), FALSE);
    return 0;
}

static int qp_exc_ue_decode_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    JSValueConst o[5];
    Py_ssize_t iv[2];
    QPExcObject *e;
    JSValue object;
    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    if (qp_exc_ue_parse(ctx, "UOnnU", args, qp_nargs(nargsf), o, iv) < 0)
        return -1;
    if (qp_bytes_check(ctx, o[1])) {
        object = JS_DupValue(ctx, o[1]);
    } else {
        QPBuffer view;
        if (qp_buffer_get(ctx, o[1], &view, QP_BUF_SIMPLE) < 0)
            return -1;
        object = qp_bytes_new(ctx, view.buf, view.len);
        qp_buffer_release(ctx, &view);
        if (JS_IsException(object))
            return -1;
    }
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_ENCODING], JS_DupValue(ctx, o[0]), FALSE);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_OBJECT], object, FALSE);
    e->start = iv[0];
    e->end = iv[1];
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_REASON], JS_DupValue(ctx, o[4]), FALSE);
    return 0;
}

static int qp_exc_ue_translate_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    JSValueConst o[4];
    Py_ssize_t iv[2];
    QPExcObject *e;
    if (qp_exc_base_init(ctx, self, args, nargsf, kwnames) < 0)
        return -1;
    if (qp_exc_ue_parse(ctx, "UnnU", args, qp_nargs(nargsf), o, iv) < 0)
        return -1;
    e = qp_exc_obj(self);
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_OBJECT], JS_DupValue(ctx, o[0]), FALSE);
    e->start = iv[0];
    e->end = iv[1];
    qp_exc_slot_set(ctx, &e->f[QP_EXC_F_UE_REASON], JS_DupValue(ctx, o[3]), FALSE);
    return 0;
}

/* magic: 0 encode, 1 decode, 2 translate */
static JSValue qp_exc_ue_str(JSContext *ctx, JSValueConst self, int kind)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue reason_str = JS_UNDEFINED, encoding_str = JS_UNDEFINED, object, result = JS_EXCEPTION;
    Py_ssize_t len, start, end;

    if (!e)
        return JS_EXCEPTION;
    if (QP_IS_NULL(e->f[QP_EXC_F_UE_OBJECT]))   /* not properly initialized */
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    /* reason and encoding may have been modified after construction */
    reason_str = qp_str(ctx, QP_IS_NULL(e->f[QP_EXC_F_UE_REASON]) ? JS_UNDEFINED : e->f[QP_EXC_F_UE_REASON]);
    if (JS_IsException(reason_str))
        return reason_str;
    if (kind != 2) {
        encoding_str = qp_str(ctx, QP_IS_NULL(e->f[QP_EXC_F_UE_ENCODING])
                              ? JS_UNDEFINED : e->f[QP_EXC_F_UE_ENCODING]);
        if (JS_IsException(encoding_str))
            goto done;
    }
    /* the str() calls above might have mutated the object */
    if (qp_exc_check_ue_attr(ctx, e->f[QP_EXC_F_UE_OBJECT], "object", kind == 1) < 0)
        goto done;
    object = JS_DupValue(ctx, e->f[QP_EXC_F_UE_OBJECT]);
    start = e->start;
    end = e->end;
    if (kind == 1) {
        const uint8_t *data = qp_bytes_data(object, &len);
        if ((start >= 0 && start < len) && (end >= 0 && end <= len) && end == start + 1)
            result = qp_str_from_format(ctx, "'%U' codec can't decode byte 0x%02x in position %zd: %U",
                                        encoding_str, (int)data[start], start, reason_str);
        else
            result = qp_str_from_format(ctx, "'%U' codec can't decode bytes in position %zd-%zd: %U",
                                        encoding_str, start, end - 1, reason_str);
    } else {
        qp_strview v;
        JSValue s = qp_str_value(ctx, object);
        if (JS_IsException(s) || qp_str_view_acquire(ctx, s, &v) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, object);
            goto done;
        }
        len = v.len;
        if ((start >= 0 && start < len) && (end >= 0 && end <= len) && end == start + 1) {
            uint32_t badchar = QP_SL_READ(v.kind, v.data, start);
            const char *fmt;
            if (kind == 0) {
                fmt = badchar <= 0xff ? "'%U' codec can't encode character '\\x%02x' in position %zd: %U"
                    : badchar <= 0xffff ? "'%U' codec can't encode character '\\u%04x' in position %zd: %U"
                    : "'%U' codec can't encode character '\\U%08x' in position %zd: %U";
                result = qp_str_from_format(ctx, fmt, encoding_str, (int)badchar, start, reason_str);
            } else {
                fmt = badchar <= 0xff ? "can't translate character '\\x%02x' in position %zd: %U"
                    : badchar <= 0xffff ? "can't translate character '\\u%04x' in position %zd: %U"
                    : "can't translate character '\\U%08x' in position %zd: %U";
                result = qp_str_from_format(ctx, fmt, (int)badchar, start, reason_str);
            }
        } else if (kind == 0) {
            result = qp_str_from_format(ctx, "'%U' codec can't encode characters in position %zd-%zd: %U",
                                        encoding_str, start, end - 1, reason_str);
        } else {
            result = qp_str_from_format(ctx, "can't translate characters in position %zd-%zd: %U",
                                        start, end - 1, reason_str);
        }
        qp_str_view_release(ctx, &v);
        JS_FreeValue(ctx, s);
    }
    JS_FreeValue(ctx, object);
done:
    JS_FreeValue(ctx, reason_str);
    JS_FreeValue(ctx, encoding_str);
    return result;
}

static JSValue qp_exc_ue_encode_str(JSContext *ctx, JSValueConst self)
{
    return qp_exc_ue_str(ctx, self, 0);
}

static JSValue qp_exc_ue_decode_str(JSContext *ctx, JSValueConst self)
{
    return qp_exc_ue_str(ctx, self, 1);
}

static JSValue qp_exc_ue_translate_str(JSContext *ctx, JSValueConst self)
{
    return qp_exc_ue_str(ctx, self, 2);
}

/* Py_T_PYSSIZET members start (magic 0) / end (magic 1) */
static JSValue qp_exc_ue_pos_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    if (!e)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, magic ? e->end : e->start);
}

static int qp_exc_ue_pos_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPExcObject *e = qp_exc_self(ctx, self);
    JSValue iv;
    Py_ssize_t n;
    int r;
    if (!e)
        return -1;
    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "can't delete numeric/char attribute");
        return -1;
    }
    if (!qp_long_check(ctx, value)) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    iv = qp_long_value(ctx, value);
    if (JS_IsException(iv))
        return -1;
    r = qp_long_as_ssize(ctx, iv, &n);
    JS_FreeValue(ctx, iv);
    if (r < 0)
        return -1;
    if (magic)
        e->end = n;
    else
        e->start = n;
    return 0;
}

static const QPGetSetDef qp_exc_ue_getsets[] = {
    { "encoding", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_UE_ENCODING, "exception encoding" },
    { "object", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_UE_OBJECT, "exception object" },
    { "start", qp_exc_ue_pos_get, qp_exc_ue_pos_set, 0, "exception start" },
    { "end", qp_exc_ue_pos_get, qp_exc_ue_pos_set, 1, "exception end" },
    { "reason", qp_exc_field_get, qp_exc_field_set, QP_EXC_F_UE_REASON, "exception reason" },
    { NULL },
};

static JSValue qp_raise_unicode_decode(JSContext *ctx, const char *encoding, const char *data,
                                       Py_ssize_t len, Py_ssize_t start, Py_ssize_t end,
                                       const char *reason)
{
    JSValue v[5], exc;
    int i;
    v[0] = qp_str_from_cstr(ctx, encoding);
    v[1] = qp_bytes_new(ctx, data, len);
    v[2] = qp_long_from_int64(ctx, start);
    v[3] = qp_long_from_int64(ctx, end);
    v[4] = qp_str_from_cstr(ctx, reason);
    for (i = 0; i < 5; i++) {
        if (JS_IsException(v[i])) {
            exc = JS_EXCEPTION;
            goto done;
        }
    }
    exc = qp_exc_new(ctx, QP_EXC(ctx, UnicodeDecodeError), 5, (JSValueConst *)v);
done:
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, v[i]);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

static JSValue qp_raise_unicode_encode(JSContext *ctx, const char *encoding, JSValueConst str,
                                       Py_ssize_t start, Py_ssize_t end, const char *reason)
{
    JSValue v[5], exc;
    int i;
    v[0] = qp_str_from_cstr(ctx, encoding);
    v[1] = JS_DupValue(ctx, str);
    v[2] = qp_long_from_int64(ctx, start);
    v[3] = qp_long_from_int64(ctx, end);
    v[4] = qp_str_from_cstr(ctx, reason);
    for (i = 0; i < 5; i++) {
        if (JS_IsException(v[i])) {
            exc = JS_EXCEPTION;
            goto done;
        }
    }
    exc = qp_exc_new(ctx, QP_EXC(ctx, UnicodeEncodeError), 5, (JSValueConst *)v);
done:
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, v[i]);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* ========================================================================= */
/* Construction from C                                                       */
/* ========================================================================= */

/* type(*args): builtin types run their C tp_new/tp_init directly (like
   type_call), other types go through the generic call. */
static JSValue qp_exc_new(JSContext *ctx, QPType *type, int nargs, JSValueConst *args)
{
    JSValue obj;
    QPType *t2;

    if (!type->spec || !type->slots.tp_new || !type->slots.tp_init)
        return qp_vectorcall(ctx, QP_TYPE_VALUE(type), args, nargs, QP_NULL);
    obj = type->slots.tp_new(ctx, type, args, nargs, QP_NULL);
    if (JS_IsException(obj))
        return obj;
    t2 = qp_type_of(ctx, obj);
    if (qp_type_is_subtype(t2, type) && t2->slots.tp_init &&
        t2->slots.tp_init(ctx, obj, args, nargs, QP_NULL) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* Rebuild UnicodeDecodeError fields from the message M1 formatted with
   UnicodeDecodeError.__str__ rules ("'%s' codec can't decode byte 0x%02x
   in position %lld: %s" or "... bytes in position %lld-%lld: %s").  The
   undecodable source bytes themselves are not carried by qp_error, so
   `object` holds the offending byte at `start` (single-byte form) or
   zero bytes up to `end` (range form): str(), start, end, encoding and
   reason match CPython.  Returns QP_NULL if msg has another shape. */
static JSValue qp_exc_decode_error_from_msg(JSContext *ctx, const char *msg)
{
    char enc[64], reason[256];
    long long a = 0, b = 0;
    unsigned int byte = 0;
    int n = 0;
    JSValue v[5], exc;
    uint8_t *buf;
    Py_ssize_t start, end, len, i;

    if (sscanf(msg, "'%63[^']' codec can't decode byte 0x%2x in position %lld: %n",
               enc, &byte, &a, &n) == 3 && n > 0) {
        start = (Py_ssize_t)a;
        end = start + 1;
    } else if (n = 0, sscanf(msg, "'%63[^']' codec can't decode bytes in position %lld-%lld: %n",
                             enc, &a, &b, &n) == 3 && n > 0) {
        start = (Py_ssize_t)a;
        end = (Py_ssize_t)b + 1;
    } else {
        return QP_NULL;
    }
    snprintf(reason, sizeof(reason), "%s", msg + n);
    if (start < 0 || end < start || end > (1 << 20))
        return QP_NULL;
    len = end;
    buf = js_mallocz(ctx, len ? len : 1);
    if (!buf)
        return JS_EXCEPTION;
    if (end == start + 1)
        buf[start] = (uint8_t)byte;
    v[0] = qp_str_from_cstr(ctx, enc);
    v[1] = qp_bytes_new(ctx, buf, len);
    js_free(ctx, buf);
    v[2] = qp_long_from_int64(ctx, start);
    v[3] = qp_long_from_int64(ctx, end);
    v[4] = qp_str_from_cstr(ctx, reason);
    exc = JS_UNDEFINED;
    for (i = 0; i < 5; i++) {
        if (JS_IsException(v[i]))
            exc = JS_EXCEPTION;
    }
    if (!JS_IsException(exc))
        exc = qp_exc_new(ctx, QP_EXC(ctx, UnicodeDecodeError), 5, (JSValueConst *)v);
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, v[i]);
    return exc;
}

/* Raise type(msg) for a non-located front-end error. */
static JSValue qp_exc_raise_msg(JSContext *ctx, QPType *type, const char *msg)
{
    JSValue m = qp_str_from_cstr(ctx, msg ? msg : ""), exc;
    if (JS_IsException(m))
        return m;
    exc = qp_exc_new(ctx, type, 1, (JSValueConst *)&m);
    JS_FreeValue(ctx, m);
    if (JS_IsException(exc))
        return exc;
    return qp_raise_object(ctx, exc);
}

/* SyntaxError & co. from a front-end error: args = (msg, (filename,
   lineno, offset, text, end_lineno, end_offset)) like
   _PyPegen_raise_error_known_location / PyErr_SyntaxLocationObject. */
static JSValue qp_raise_qp_error(JSContext *ctx, const qp_error *err, const char *filename)
{
    QPType *type;
    JSValue info[6], args[2], exc;
    int i;

    switch (err->kind) {
    case QP_ERR_SYNTAX:
        type = QP_EXC(ctx, SyntaxError);
        break;
    case QP_ERR_INDENTATION:
        type = QP_EXC(ctx, IndentationError);
        break;
    case QP_ERR_TAB:
        type = QP_EXC(ctx, TabError);
        break;
    case QP_ERR_INCOMPLETE:
        type = QP_EXC(ctx, _IncompleteInputError);
        break;
    case QP_ERR_MEMORY:
        /* (a message: the parser's stack overflow; none: out of memory) */
        if (err->msg && err->msg[0])
            return qp_exc_raise_msg(ctx, QP_EXC(ctx, MemoryError), err->msg);
        return qp_raise_memory_error(ctx);
    case QP_ERR_VALUE:
        return qp_exc_raise_msg(ctx, QP_EXC(ctx, ValueError), err->msg);
    case QP_ERR_RECURSION:
        return qp_exc_raise_msg(ctx, QP_EXC(ctx, RecursionError), err->msg);
    case QP_ERR_OVERFLOW:
        return qp_exc_raise_msg(ctx, QP_EXC(ctx, OverflowError), err->msg);
    case QP_ERR_PENDING:
        return JS_EXCEPTION;
    case QP_ERR_UNICODE_DECODE:
        exc = err->msg ? qp_exc_decode_error_from_msg(ctx, err->msg) : QP_NULL;
        if (JS_IsException(exc))
            return exc;
        if (QP_IS_NULL(exc))   /* message of another shape: keep it as is */
            return qp_exc_raise_msg(ctx, QP_EXC(ctx, SyntaxError), err->msg);
        return qp_raise_object(ctx, exc);
    default:
        return qp_raise_system_error(ctx, "error return without exception set");
    }

    info[0] = filename ? qp_str_from_cstr(ctx, filename) : JS_UNDEFINED;
    info[1] = JS_NewInt32(ctx, err->lineno);
    /* 0 without an end position = unknown (None); a negative offset (-1 of
       _PyTokenizer_raise_init_error) or 0 with an end position (a
       tokenizer error at column 0) is the value CPython reports */
    info[2] = err->offset != 0 || err->end_lineno > 0 ? JS_NewInt32(ctx, err->offset) :
        JS_UNDEFINED;
    if (err->text) {
        info[3] = qp_str_from_utf8(ctx, err->text, strlen(err->text));
    } else {
        /* errors located by the compiler (_PyErr_RaiseSyntaxError,
           PyErr_RangedSyntaxLocationObject) read the line back from the
           source file, if `filename` names one */
        info[3] = qp_err_program_text(ctx, filename, err->lineno);
        if (QP_IS_NULL(info[3]))
            info[3] = JS_UNDEFINED;
    }
    if (err->end_lineno > 0) {
        info[4] = JS_NewInt32(ctx, err->end_lineno);
        info[5] = JS_NewInt32(ctx, err->end_offset);
    } else {
        info[4] = JS_UNDEFINED;
        info[5] = JS_UNDEFINED;
    }
    if (JS_IsException(info[3])) {
        /* undecodable source line: CPython falls back to no text */
        JS_FreeValue(ctx, JS_GetException(ctx));
        info[3] = JS_UNDEFINED;
    }
    if (JS_IsException(info[0])) {
        for (i = 1; i < 6; i++)
            JS_FreeValue(ctx, info[i]);
        return JS_EXCEPTION;
    }
    args[1] = qp_tuple_new_steal(ctx, 6, info);
    if (JS_IsException(args[1]))
        return JS_EXCEPTION;
    args[0] = qp_str_from_cstr(ctx, err->msg ? err->msg : "invalid syntax");
    if (JS_IsException(args[0])) {
        JS_FreeValue(ctx, args[1]);
        return JS_EXCEPTION;
    }
    exc = qp_exc_new(ctx, type, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(exc))
        return exc;
    if (err->has_metadata) {
        /* _PyPegen_set_syntax_error_metadata: (lineno, col_offset, source) */
        JSValue meta[3], tup;
        meta[0] = JS_NewInt32(ctx, err->metadata_lineno);
        meta[1] = JS_NewInt32(ctx, err->metadata_col_offset);
        meta[2] = err->metadata_source
            ? qp_str_from_utf8(ctx, err->metadata_source, strlen(err->metadata_source))
            : JS_UNDEFINED;
        if (JS_IsException(meta[2])) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            meta[2] = JS_UNDEFINED;
        }
        tup = qp_tuple_new_steal(ctx, 3, meta);
        if (JS_IsException(tup)) {
            JS_FreeValue(ctx, exc);
            return tup;
        }
        qp_exc_field_set(ctx, exc, tup, QP_EXC_F_SE_METADATA);
        JS_FreeValue(ctx, tup);
    }
    return qp_raise_object(ctx, exc);
}

/* ========================================================================= */
/* Type table and init                                                        */
/* ========================================================================= */

#define QP_EXC_TPFLAGS \
    (QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_BASE_EXC_SUBCLASS | \
     QP_TPFLAGS_IMMUTABLETYPE)

#define QP_EXC_SPEC(NAME, TPNAME, BASE, DOC, NEW, INIT, STR, REPR, METHODS, GETSETS) \
    [QP_EXC_ ## NAME] = {                                                    \
        .name = TPNAME, .doc = DOC, .flags = QP_EXC_TPFLAGS, .base = -1,     \
        .exc_base = BASE, .instance_class_id = JS_CLASS_PY_EXCEPTION,        \
        .nslots = 0,                                                         \
        .slots = { .tp_repr = REPR, .tp_str = STR, .tp_init = INIT, .tp_new = NEW }, \
        .methods = METHODS, .getsets = GETSETS, .members = NULL,             \
    }

/* SimpleExtendsException: own __init__/__new__ = BaseException's */
#define QP_EXC_SIMPLE(NAME, BASE, DOC) \
    QP_EXC_SPEC(NAME, #NAME, QP_EXC_ ## BASE, DOC, qp_exc_base_new, qp_exc_base_init, \
                NULL, NULL, NULL, NULL)
/* MiddlingExtendsException: own __init__ of the storage family, inherited __new__ */
#define QP_EXC_MIDDLING(NAME, BASE, INIT, DOC) \
    QP_EXC_SPEC(NAME, #NAME, QP_EXC_ ## BASE, DOC, NULL, INIT, NULL, NULL, NULL, NULL)

static const QPTypeSpec qp_exc_specs[QP_EXC__COUNT] = {
    QP_EXC_SPEC(BaseException, "BaseException", -1, "Common base class for all exceptions",
                qp_exc_base_new, qp_exc_base_init, qp_exc_base_str, qp_exc_base_repr,
                qp_exc_base_methods, qp_exc_base_getsets),
    QP_EXC_SIMPLE(Exception, BaseException, "Common base class for all non-exit exceptions."),
    QP_EXC_SIMPLE(TypeError, Exception, "Inappropriate argument type."),
    QP_EXC_SIMPLE(StopAsyncIteration, Exception, "Signal the end from iterator.__anext__()."),
    QP_EXC_SPEC(StopIteration, "StopIteration", QP_EXC_Exception,
                "Signal the end from iterator.__next__().",
                NULL, qp_exc_stopiteration_init, NULL, NULL, NULL, qp_exc_stopiteration_getsets),
    QP_EXC_SIMPLE(GeneratorExit, BaseException, "Request that a generator exit."),
    QP_EXC_SPEC(SystemExit, "SystemExit", QP_EXC_BaseException,
                "Request to exit from the interpreter.",
                NULL, qp_exc_systemexit_init, NULL, NULL, NULL, qp_exc_systemexit_getsets),
    QP_EXC_SIMPLE(KeyboardInterrupt, BaseException, "Program interrupted by user."),
    QP_EXC_SPEC(BaseExceptionGroup, "BaseExceptionGroup", QP_EXC_BaseException,
                "A combination of multiple unrelated exceptions.",
                qp_exc_eg_new, qp_exc_eg_init, qp_exc_eg_str, qp_exc_eg_repr,
                qp_exc_eg_methods, qp_exc_eg_getsets),
    /* ExceptionGroup: heap type created in qp_init_exceptions */
    QP_EXC_SPEC(ImportError, "ImportError", QP_EXC_Exception,
                "Import can't find module, or can't find name in module.",
                NULL, qp_exc_importerror_init, qp_exc_importerror_str, qp_exc_importerror_repr,
                qp_exc_importerror_methods, qp_exc_importerror_getsets),
    QP_EXC_MIDDLING(ImportCycleError, ImportError, qp_exc_importerror_init, "Import produces a cycle."),
    QP_EXC_MIDDLING(ModuleNotFoundError, ImportError, qp_exc_importerror_init, "Module not found."),
    QP_EXC_SPEC(OSError, "OSError", QP_EXC_Exception, "Base class for I/O related errors.",
                qp_exc_oserror_new, qp_exc_oserror_init, qp_exc_oserror_str, NULL,
                qp_exc_oserror_methods, qp_exc_oserror_getsets),
    QP_EXC_MIDDLING(BlockingIOError, OSError, qp_exc_oserror_init, "I/O operation would block."),
    QP_EXC_MIDDLING(ConnectionError, OSError, qp_exc_oserror_init, "Connection error."),
    QP_EXC_MIDDLING(ChildProcessError, OSError, qp_exc_oserror_init, "Child process error."),
    QP_EXC_MIDDLING(BrokenPipeError, ConnectionError, qp_exc_oserror_init, "Broken pipe."),
    QP_EXC_MIDDLING(ConnectionAbortedError, ConnectionError, qp_exc_oserror_init, "Connection aborted."),
    QP_EXC_MIDDLING(ConnectionRefusedError, ConnectionError, qp_exc_oserror_init, "Connection refused."),
    QP_EXC_MIDDLING(ConnectionResetError, ConnectionError, qp_exc_oserror_init, "Connection reset."),
    QP_EXC_MIDDLING(FileExistsError, OSError, qp_exc_oserror_init, "File already exists."),
    QP_EXC_MIDDLING(FileNotFoundError, OSError, qp_exc_oserror_init, "File not found."),
    QP_EXC_MIDDLING(IsADirectoryError, OSError, qp_exc_oserror_init,
                    "Operation doesn't work on directories."),
    QP_EXC_MIDDLING(NotADirectoryError, OSError, qp_exc_oserror_init,
                    "Operation only works on directories."),
    QP_EXC_MIDDLING(InterruptedError, OSError, qp_exc_oserror_init, "Interrupted by signal."),
    QP_EXC_MIDDLING(PermissionError, OSError, qp_exc_oserror_init, "Not enough permissions."),
    QP_EXC_MIDDLING(ProcessLookupError, OSError, qp_exc_oserror_init, "Process not found."),
    QP_EXC_MIDDLING(TimeoutError, OSError, qp_exc_oserror_init, "Timeout expired."),
    QP_EXC_SIMPLE(EOFError, Exception, "Read beyond end of file."),
    QP_EXC_SIMPLE(RuntimeError, Exception, "Unspecified run-time error."),
    QP_EXC_SIMPLE(RecursionError, RuntimeError, "Recursion limit exceeded."),
    QP_EXC_SIMPLE(PythonFinalizationError, RuntimeError,
                  "Operation blocked during Python finalization."),
    QP_EXC_SIMPLE(NotImplementedError, RuntimeError,
                  "Method or function hasn't been implemented yet."),
    QP_EXC_SPEC(NameError, "NameError", QP_EXC_Exception, "Name not found globally.",
                NULL, qp_exc_nameerror_init, qp_exc_base_str, NULL, NULL, qp_exc_nameerror_getsets),
    QP_EXC_MIDDLING(UnboundLocalError, NameError, qp_exc_nameerror_init,
                    "Local name referenced but not bound to a value."),
    QP_EXC_SPEC(AttributeError, "AttributeError", QP_EXC_Exception, "Attribute not found.",
                NULL, qp_exc_attributeerror_init, qp_exc_base_str, NULL,
                qp_exc_attributeerror_methods, qp_exc_attributeerror_getsets),
    QP_EXC_SPEC(SyntaxError, "SyntaxError", QP_EXC_Exception, "Invalid syntax.",
                NULL, qp_exc_syntaxerror_init, qp_exc_syntaxerror_str, NULL, NULL,
                qp_exc_syntaxerror_getsets),
    QP_EXC_MIDDLING(IndentationError, SyntaxError, qp_exc_syntaxerror_init, "Improper indentation."),
    QP_EXC_MIDDLING(TabError, IndentationError, qp_exc_syntaxerror_init,
                    "Improper mixture of spaces and tabs."),
    QP_EXC_MIDDLING(_IncompleteInputError, SyntaxError, qp_exc_syntaxerror_init, "incomplete input."),
    QP_EXC_SIMPLE(LookupError, Exception, "Base class for lookup errors."),
    QP_EXC_SIMPLE(IndexError, LookupError, "Sequence index out of range."),
    QP_EXC_SPEC(KeyError, "KeyError", QP_EXC_LookupError, "Mapping key not found.",
                NULL, qp_exc_base_init, qp_exc_keyerror_str, NULL, NULL, NULL),
    QP_EXC_SIMPLE(ValueError, Exception, "Inappropriate argument value (of correct type)."),
    QP_EXC_SIMPLE(UnicodeError, ValueError, "Unicode related error."),
    QP_EXC_SPEC(UnicodeEncodeError, "UnicodeEncodeError", QP_EXC_UnicodeError,
                "Unicode encoding error.", qp_exc_base_new, qp_exc_ue_encode_init,
                qp_exc_ue_encode_str, NULL, NULL, qp_exc_ue_getsets),
    QP_EXC_SPEC(UnicodeDecodeError, "UnicodeDecodeError", QP_EXC_UnicodeError,
                "Unicode decoding error.", qp_exc_base_new, qp_exc_ue_decode_init,
                qp_exc_ue_decode_str, NULL, NULL, qp_exc_ue_getsets),
    QP_EXC_SPEC(UnicodeTranslateError, "UnicodeTranslateError", QP_EXC_UnicodeError,
                "Unicode translation error.", qp_exc_base_new, qp_exc_ue_translate_init,
                qp_exc_ue_translate_str, NULL, NULL, qp_exc_ue_getsets),
    QP_EXC_SIMPLE(AssertionError, Exception, "Assertion failed."),
    QP_EXC_SIMPLE(ArithmeticError, Exception, "Base class for arithmetic errors."),
    QP_EXC_SIMPLE(FloatingPointError, ArithmeticError, "Floating-point operation failed."),
    QP_EXC_SIMPLE(OverflowError, ArithmeticError, "Result too large to be represented."),
    QP_EXC_SIMPLE(ZeroDivisionError, ArithmeticError,
                  "Second argument to a division or modulo operation was zero."),
    QP_EXC_SIMPLE(SystemError, Exception,
                  "Internal error in the Python interpreter.\n\n"
                  "Please report this to the Python maintainer, along with the traceback,\n"
                  "the Python version, and the hardware/OS platform and version."),
    QP_EXC_SIMPLE(ReferenceError, Exception, "Weak ref proxy used after referent went away."),
    /* MemoryError_new only adds a free list to BaseException_new */
    QP_EXC_SIMPLE(MemoryError, Exception, "Out of memory."),
    QP_EXC_SIMPLE(BufferError, Exception, "Buffer error."),
    QP_EXC_SIMPLE(Warning, Exception, "Base class for warning categories."),
    QP_EXC_SIMPLE(UserWarning, Warning, "Base class for warnings generated by user code."),
    QP_EXC_SIMPLE(EncodingWarning, Warning, "Base class for warnings about encodings."),
    QP_EXC_SIMPLE(DeprecationWarning, Warning, "Base class for warnings about deprecated features."),
    QP_EXC_SIMPLE(PendingDeprecationWarning, Warning,
                  "Base class for warnings about features which will be deprecated\n"
                  "in the future."),
    QP_EXC_SIMPLE(SyntaxWarning, Warning, "Base class for warnings about dubious syntax."),
    QP_EXC_SIMPLE(RuntimeWarning, Warning,
                  "Base class for warnings about dubious runtime behavior."),
    QP_EXC_SIMPLE(FutureWarning, Warning,
                  "Base class for warnings about constructs that will change semantically\n"
                  "in the future."),
    QP_EXC_SIMPLE(ImportWarning, Warning,
                  "Base class for warnings about probable mistakes in module imports"),
    QP_EXC_SIMPLE(UnicodeWarning, Warning,
                  "Base class for warnings about Unicode related problems, mostly\n"
                  "related to conversion problems."),
    QP_EXC_SIMPLE(BytesWarning, Warning,
                  "Base class for warnings about bytes and buffer related problems, mostly\n"
                  "related to conversion from str or comparing to str."),
    QP_EXC_SIMPLE(ResourceWarning, Warning, "Base class for warnings about resource usage."),
    /* js.JsError (D3): a JavaScript value thrown into Python code */
    QP_EXC_SPEC(JsError, "js.JsError", QP_EXC_Exception, "Error thrown by JavaScript code.",
                qp_exc_base_new, qp_exc_base_init, NULL, NULL, NULL, NULL),
};

/* ExceptionGroup is a heap type like CPython's PyErr_NewException(
   "builtins.ExceptionGroup", (BaseExceptionGroup, Exception), NULL). */
static int qp_exc_create_exception_group_type(JSContext *ctx)
{
    JSValue name, bases, ns, mod, cls;
    JSValue b[2];
    QPType *t;

    b[0] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, BaseExceptionGroup)));
    b[1] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, Exception)));
    bases = qp_tuple_new_steal(ctx, 2, b);
    if (JS_IsException(bases))
        return -1;
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns)) {
        JS_FreeValue(ctx, bases);
        return -1;
    }
    mod = qp_str_from_cstr(ctx, "builtins");
    name = qp_str_from_cstr(ctx, "ExceptionGroup");
    if (JS_IsException(mod) || JS_IsException(name) ||
        qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __module__), mod) < 0) {
        cls = JS_EXCEPTION;
    } else {
        cls = qp_type_new(ctx, QP_TYPE(ctx, type), name, bases, ns, QP_NULL);
    }
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, bases);
    if (JS_IsException(cls))
        return -1;
    t = qp_type_from_value(cls);
    if (!t) {
        JS_FreeValue(ctx, cls);
        qp_raise_system_error(ctx, "ExceptionGroup is not a type");
        return -1;
    }
    /* QPState owns this reference (released with the other exc_types). */
    QP_STATE(ctx)->exc_types[QP_EXC_ExceptionGroup] = t;
    return 0;
}

/* Ready every builtin exception type (bases first, QP_EXCEPTIONS order),
   create ExceptionGroup, then the traceback type. */
static int qp_init_exceptions(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    int i;

    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (i == QP_EXC_ExceptionGroup) {
            if (qp_exc_create_exception_group_type(ctx) < 0)
                return -1;
            continue;
        }
        if (qp_type_ready(ctx, s->exc_types[i], &qp_exc_specs[i]) < 0)
            return -1;
    }
    return qp_init_traceback(ctx);
}
