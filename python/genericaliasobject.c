/*
 * dquickts Python: types.GenericAlias (list[int]), its iterator, and
 * typing.Union (int | str).
 *
 * Derived from CPython Objects/genericaliasobject.c and
 * Objects/unionobject.c (PSF-2.0).
 */

/* from typevarobject.c (unit k) */
static int qp_typing_type_repr(JSContext *ctx, StringBuffer *b, JSValueConst p);

/* GenericAlias instance slots (CPython gaobject) */
#define QP_GA_ORIGIN    0
#define QP_GA_ARGS      1
#define QP_GA_PARAMS    2   /* QP_NULL until __parameters__ is computed */
#define QP_GA_STARRED   3   /* JS bool: *tuple[int] */
#define QP_GA_NSLOTS    4

/* Union instance slots (CPython unionobject) */
#define QP_UNION_ARGS        0  /* tuple of all args */
#define QP_UNION_HASHABLE    1  /* frozenset */
#define QP_UNION_UNHASHABLE  2  /* tuple or QP_NULL */
#define QP_UNION_PARAMS      3  /* QP_NULL until computed */
#define QP_UNION_NSLOTS      4

/* generic_alias_iterator slots */
#define QP_GAITER_OBJ   0   /* the alias, QP_NULL once exhausted */
#define QP_GAITER_NSLOTS 1

static BOOL qp_ga_check(JSContext *ctx, JSValueConst v)
{
    return qp_typecheck(ctx, v, QP_TYPE(ctx, GenericAlias));
}

static BOOL qp_union_check(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v) == QP_TYPE(ctx, Union);
}

static BOOL qp_ga_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static BOOL qp_ga_starred(JSValueConst self)
{
    JSValue v = qp_instance_slots(self)[QP_GA_STARRED];
    return JS_VALUE_GET_TAG(v) == JS_TAG_BOOL && JS_VALUE_GET_INT(v);
}

/* ---- growable array of owned values (replaces _PyTuple_Resize) ---------- */

typedef struct QPGAVec {
    JSValue *items;
    Py_ssize_t len, cap;
} QPGAVec;

/* Append v (consumed). */
static int qp_gavec_push_steal(JSContext *ctx, QPGAVec *vec, JSValue v)
{
    if (vec->len >= vec->cap) {
        Py_ssize_t ncap = vec->cap ? vec->cap * 2 : 8;
        JSValue *n = js_realloc(ctx, vec->items, sizeof(JSValue) * ncap);
        if (!n) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        vec->items = n;
        vec->cap = ncap;
    }
    vec->items[vec->len++] = v;
    return 0;
}

static int qp_gavec_push(JSContext *ctx, QPGAVec *vec, JSValueConst v)
{
    return qp_gavec_push_steal(ctx, vec, JS_DupValue(ctx, v));
}

static int qp_gavec_extend(JSContext *ctx, QPGAVec *vec, const JSValue *src, Py_ssize_t n)
{
    for (Py_ssize_t i = 0; i < n; i++) {
        if (qp_gavec_push(ctx, vec, src[i]) < 0)
            return -1;
    }
    return 0;
}

/* Index of item (identity) in items[0..len), or -1 (CPython tuple_index). */
static Py_ssize_t qp_ga_index(const JSValue *items, Py_ssize_t len, JSValueConst item)
{
    for (Py_ssize_t i = 0; i < len; i++) {
        if (qp_is(items[i], item))
            return i;
    }
    return -1;
}

/* CPython tuple_add: append item unless already present (identity). */
static int qp_gavec_add_unique(JSContext *ctx, QPGAVec *vec, JSValueConst item)
{
    if (qp_ga_index(vec->items, vec->len, item) >= 0)
        return 0;
    return qp_gavec_push(ctx, vec, item);
}

static void qp_gavec_free(JSContext *ctx, QPGAVec *vec)
{
    for (Py_ssize_t i = 0; i < vec->len; i++)
        JS_FreeValue(ctx, vec->items[i]);
    js_free(ctx, vec->items);
    vec->items = NULL;
    vec->len = vec->cap = 0;
}

/* New tuple from the vector contents; the vector is emptied. */
static JSValue qp_gavec_to_tuple(JSContext *ctx, QPGAVec *vec)
{
    JSValue t = qp_tuple_new_steal(ctx, vec->len, vec->items);
    js_free(ctx, vec->items);
    vec->items = NULL;
    vec->len = vec->cap = 0;
    return t;
}

/* ---- repr ---------------------------------------------------------------- */

/* list[i] as a new reference; IndexError if the list shrank meanwhile. */
static JSValue qp_ga_list_item(JSContext *ctx, JSValueConst list, Py_ssize_t i)
{
    Py_ssize_t n;
    JSValue *items = qp_list_items(list, &n);
    if (items) {
        if (i >= n)
            return qp_raise_index_error(ctx, "list index out of range");
        return JS_DupValue(ctx, items[i]);
    }
    return qp_getitem(ctx, list, JS_NewInt32(ctx, (int32_t)i));
}

/* "[a, b]" for the ParamSpec-style list args of Callable[[a, b], r]. */
static int qp_ga_repr_items_list(JSContext *ctx, StringBuffer *b, JSValueConst p)
{
    Py_ssize_t len = qp_list_size(p);
    if (string_buffer_putc8(b, '['))
        return -1;
    for (Py_ssize_t i = 0; i < len; i++) {
        if (i > 0 && string_buffer_puts8(b, ", "))
            return -1;
        JSValue item = qp_ga_list_item(ctx, p, i);
        if (JS_IsException(item))
            return -1;
        int r = qp_typing_type_repr(ctx, b, item);
        JS_FreeValue(ctx, item);
        if (r < 0)
            return -1;
    }
    if (string_buffer_putc8(b, ']'))
        return -1;
    return 0;
}

static JSValue qp_ga_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    JSValueConst args = s[QP_GA_ARGS];
    Py_ssize_t len = QP_TUPLE_SIZE(args);
    StringBuffer b;

    string_buffer_init(ctx, &b, 16);
    if (qp_ga_starred(self) && string_buffer_putc8(&b, '*'))
        goto error;
    if (qp_typing_type_repr(ctx, &b, s[QP_GA_ORIGIN]) < 0)
        goto error;
    if (string_buffer_putc8(&b, '['))
        goto error;
    for (Py_ssize_t i = 0; i < len; i++) {
        if (i > 0 && string_buffer_puts8(&b, ", "))
            goto error;
        JSValueConst p = QP_TUPLE_ITEMS(args)[i];
        if (qp_list_check_exact(ctx, p)) {
            /* Looks like we are working with ParamSpec's list of type args */
            if (qp_ga_repr_items_list(ctx, &b, p) < 0)
                goto error;
        } else if (qp_typing_type_repr(ctx, &b, p) < 0) {
            goto error;
        }
    }
    /* for something like tuple[()] we should print a "()" */
    if (len == 0 && string_buffer_puts8(&b, "()"))
        goto error;
    if (string_buffer_putc8(&b, ']'))
        goto error;
    return string_buffer_end(&b);
error:
    string_buffer_free(&b);
    return JS_EXCEPTION;
}

/* ---- parameters and substitution ---------------------------------------- */

/* _Py_make_parameters: the type variables (objects with __typing_subst__)
   of args (tuple or list), recursively through __parameters__ and nested
   lists/tuples, deduplicated by identity. */
static JSValue qp_make_parameters(JSContext *ctx, JSValueConst args)
{
    JSValue tuple_args = JS_UNDEFINED;
    QPGAVec params = { 0 };

    if (js_check_stack_overflow(ctx->rt, 0))
        return qp_raise(ctx, QP_EXC(ctx, RecursionError),
                        "maximum recursion depth exceeded in __parameter__ calculation");
    if (qp_list_check(ctx, args)) {
        tuple_args = qp_tuple_from_iterable(ctx, args);
        if (JS_IsException(tuple_args))
            return JS_EXCEPTION;
        args = tuple_args;
    }
    Py_ssize_t nargs = QP_TUPLE_SIZE(args);
    for (Py_ssize_t iarg = 0; iarg < nargs; iarg++) {
        JSValueConst t = QP_TUPLE_ITEMS(args)[iarg];
        JSValue tmp;
        /* We don't want __parameters__ descriptor of a bare Python class. */
        if (qp_type_from_value(t))
            continue;
        int rc = qp_getattr_opt(ctx, t, QP_ATOM(ctx, __typing_subst__), &tmp);
        if (rc < 0)
            goto error;
        JS_FreeValue(ctx, tmp);
        if (rc) {
            if (qp_gavec_add_unique(ctx, &params, t) < 0)
                goto error;
            continue;
        }
        JSValue subparams;
        if (qp_getattr_opt(ctx, t, QP_ATOM(ctx, __parameters__), &subparams) < 0)
            goto error;
        if (QP_IS_NULL(subparams) && (qp_tuple_check(ctx, t) || qp_list_check(ctx, t))) {
            /* Recursively call make_parameters for lists/tuples and add the
               results to the current parameters. */
            subparams = qp_make_parameters(ctx, t);
            if (JS_IsException(subparams))
                goto error;
        }
        if (!QP_IS_NULL(subparams) && qp_tuple_check(ctx, subparams)) {
            Py_ssize_t len2 = QP_TUPLE_SIZE(subparams);
            for (Py_ssize_t j = 0; j < len2; j++) {
                if (qp_gavec_add_unique(ctx, &params, QP_TUPLE_ITEMS(subparams)[j]) < 0) {
                    JS_FreeValue(ctx, subparams);
                    goto error;
                }
            }
        }
        JS_FreeValue(ctx, subparams);
    }
    JS_FreeValue(ctx, tuple_args);
    return qp_gavec_to_tuple(ctx, &params);
error:
    qp_gavec_free(ctx, &params);
    JS_FreeValue(ctx, tuple_args);
    return JS_EXCEPTION;
}

/* subs_tvars: if obj is a generic alias, substitute its type variables
   (found in params) with the corresponding argitems, e.g. list[T] with
   params (T, S) and argitems (str, int) -> list[str].  If obj has no
   non-empty __parameters__ tuple, return a new reference to obj. */
static JSValue qp_subs_tvars(JSContext *ctx, JSValueConst obj, JSValueConst params,
                             const JSValue *argitems, Py_ssize_t nargs)
{
    JSValue subparams, res;
    if (qp_getattr_opt(ctx, obj, QP_ATOM(ctx, __parameters__), &subparams) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(subparams) && qp_tuple_check(ctx, subparams) && QP_TUPLE_SIZE(subparams)) {
        Py_ssize_t nparams = QP_TUPLE_SIZE(params);
        Py_ssize_t nsubargs = QP_TUPLE_SIZE(subparams);
        QPGAVec subargs = { 0 };
        for (Py_ssize_t i = 0; i < nsubargs; ++i) {
            JSValueConst arg = QP_TUPLE_ITEMS(subparams)[i];
            Py_ssize_t iparam = qp_ga_index(QP_TUPLE_ITEMS(params), nparams, arg);
            if (iparam >= 0) {
                JSValueConst param = QP_TUPLE_ITEMS(params)[iparam];
                arg = argitems[iparam];
                if (qp_type_of(ctx, param)->slots.tp_iter && qp_tuple_check(ctx, arg)) {
                    /* TypeVarTuple */
                    if (qp_gavec_extend(ctx, &subargs, QP_TUPLE_ITEMS(arg), QP_TUPLE_SIZE(arg)) < 0)
                        goto fail;
                    continue;
                }
            }
            if (qp_gavec_push(ctx, &subargs, arg) < 0)
                goto fail;
            continue;
        fail:
            qp_gavec_free(ctx, &subargs);
            JS_FreeValue(ctx, subparams);
            return JS_EXCEPTION;
        }
        JSValue t = qp_gavec_to_tuple(ctx, &subargs);
        if (JS_IsException(t)) {
            JS_FreeValue(ctx, subparams);
            return JS_EXCEPTION;
        }
        res = qp_getitem(ctx, obj, t);
        JS_FreeValue(ctx, t);
    } else {
        res = JS_DupValue(ctx, obj);
    }
    JS_FreeValue(ctx, subparams);
    return res;
}

/* _is_unpacked_typevartuple: 1/0/-1. */
static int qp_is_unpacked_typevartuple(JSContext *ctx, JSValueConst arg)
{
    JSValue tmp;
    if (qp_type_from_value(arg))
        return 0;
    int res = qp_getattr_opt(ctx, arg, QP_ATOM(ctx, __typing_is_unpacked_typevartuple__), &tmp);
    if (res > 0) {
        res = qp_bool(ctx, tmp);
        JS_FreeValue(ctx, tmp);
    }
    return res;
}

/* _unpacked_tuple_args: 1 with *pres (new ref) for *tuple[...] -like args,
   0 if not one, -1 on error. */
static int qp_unpacked_tuple_args(JSContext *ctx, JSValueConst arg, JSValue *pres)
{
    JSValue result;
    *pres = QP_NULL;
    /* Fast path */
    if (qp_ga_check(ctx, arg) && qp_ga_starred(arg) &&
        qp_is(qp_instance_slots(arg)[QP_GA_ORIGIN], QP_TYPE_VALUE(QP_TYPE(ctx, tuple)))) {
        *pres = JS_DupValue(ctx, qp_instance_slots(arg)[QP_GA_ARGS]);
        return 1;
    }
    int r = qp_getattr_opt(ctx, arg, QP_ATOM(ctx, __typing_unpacked_tuple_args__), &result);
    if (r <= 0)
        return r;
    if (qp_ga_is_none(result)) {
        JS_FreeValue(ctx, result);
        return 0;
    }
    *pres = result;
    return 1;
}

/* _unpack_args: tuple of the items of item (a tuple or a single value)
   with *tuple[a, b] items spliced in (not for *tuple[a, ...]). */
static JSValue qp_unpack_args(JSContext *ctx, JSValueConst item)
{
    QPGAVec newargs = { 0 };
    BOOL is_tuple = qp_tuple_check(ctx, item);
    Py_ssize_t nitems = is_tuple ? QP_TUPLE_SIZE(item) : 1;
    const JSValue *argitems = is_tuple ? QP_TUPLE_ITEMS(item) : &item;

    for (Py_ssize_t i = 0; i < nitems; i++) {
        JSValueConst it = argitems[i];
        if (!qp_type_from_value(it)) {
            JSValue subargs;
            int r = qp_unpacked_tuple_args(ctx, it, &subargs);
            if (r < 0)
                goto error;
            if (r > 0) {
                if (qp_tuple_check(ctx, subargs) &&
                    !(QP_TUPLE_SIZE(subargs) &&
                      qp_is(QP_TUPLE_ITEMS(subargs)[QP_TUPLE_SIZE(subargs) - 1],
                            QP_STATE(ctx)->ellipsis))) {
                    int er = qp_gavec_extend(ctx, &newargs, QP_TUPLE_ITEMS(subargs),
                                             QP_TUPLE_SIZE(subargs));
                    JS_FreeValue(ctx, subargs);
                    if (er < 0)
                        goto error;
                    continue;
                }
                JS_FreeValue(ctx, subargs);
            }
        }
        if (qp_gavec_push(ctx, &newargs, it) < 0)
            goto error;
    }
    return qp_gavec_to_tuple(ctx, &newargs);
error:
    qp_gavec_free(ctx, &newargs);
    return JS_EXCEPTION;
}

/* _Py_subs_parameters: substitute the type variables `parameters` of self
   (whose arguments are args, a tuple or list) with item; returns the new
   argument tuple. */
static JSValue qp_subs_parameters(JSContext *ctx, JSValueConst self, JSValueConst args,
                                  JSValueConst parameters, JSValueConst item_in)
{
    Py_ssize_t nparams = QP_TUPLE_SIZE(parameters);
    JSValue item, tuple_args = JS_UNDEFINED;
    QPGAVec newargs = { 0 };

    if (nparams == 0)
        return qp_raise_type_error(ctx, "%R is not a generic class", self);
    item = qp_unpack_args(ctx, item_in);
    if (JS_IsException(item))
        return JS_EXCEPTION;
    for (Py_ssize_t i = 0; i < nparams; i++) {
        JSValueConst param = QP_TUPLE_ITEMS(parameters)[i];
        JSValue prepare, tmp;
        if (qp_getattr_opt(ctx, param, QP_ATOM(ctx, __typing_prepare_subst__), &prepare) < 0) {
            JS_FreeValue(ctx, item);
            return JS_EXCEPTION;
        }
        if (!QP_IS_NULL(prepare) && !qp_ga_is_none(prepare)) {
            if (qp_tuple_check(ctx, item)) {
                tmp = qp_call2(ctx, prepare, self, item);
            } else {
                JSValue one = qp_tuple_pack(ctx, 1, (JSValueConst *)&item);
                tmp = JS_IsException(one) ? JS_EXCEPTION : qp_call2(ctx, prepare, self, one);
                JS_FreeValue(ctx, one);
            }
            JS_FreeValue(ctx, prepare);
            JS_FreeValue(ctx, item);
            item = tmp;
            if (JS_IsException(item))
                return JS_EXCEPTION;
        } else {
            JS_FreeValue(ctx, prepare);
        }
    }
    BOOL is_tuple = qp_tuple_check(ctx, item);
    Py_ssize_t nitems = is_tuple ? QP_TUPLE_SIZE(item) : 1;
    const JSValue *argitems = is_tuple ? QP_TUPLE_ITEMS(item) : &item;
    if (nitems != nparams) {
        qp_raise_type_error(ctx, "Too %s arguments for %R; actual %zd, expected %zd",
                            nitems > nparams ? "many" : "few", self, nitems, nparams);
        JS_FreeValue(ctx, item);
        return JS_EXCEPTION;
    }
    /* Replace all type variables (specified by parameters) with
       corresponding values specified by argitems.
        t = list[T];          t[int]      -> newargs = [int]
        t = dict[str, T];     t[int]      -> newargs = [str, int]
        t = dict[T, list[S]]; t[str, int] -> newargs = [str, list[int]]
        t = list[[T]];        t[str]      -> newargs = [[str]] */
    if (qp_list_check(ctx, args)) {
        tuple_args = qp_tuple_from_iterable(ctx, args);
        if (JS_IsException(tuple_args)) {
            JS_FreeValue(ctx, item);
            return JS_EXCEPTION;
        }
        args = tuple_args;
    }
    Py_ssize_t nargs = QP_TUPLE_SIZE(args);
    for (Py_ssize_t iarg = 0; iarg < nargs; iarg++) {
        JSValueConst arg = QP_TUPLE_ITEMS(args)[iarg];
        JSValue newarg;
        if (qp_type_from_value(arg)) {
            if (qp_gavec_push(ctx, &newargs, arg) < 0)
                goto error;
            continue;
        }
        /* Recursively substitute params in lists/tuples. */
        if (qp_tuple_check(ctx, arg) || qp_list_check(ctx, arg)) {
            JSValue subargs = qp_subs_parameters(ctx, self, arg, parameters, item);
            if (JS_IsException(subargs))
                goto error;
            if (!qp_tuple_check(ctx, arg)) {
                /* subs_parameters returns a tuple: if the original arg was a
                   list, convert subargs to a list as well. */
                JSValue l = qp_list_from_iterable(ctx, subargs);
                JS_FreeValue(ctx, subargs);
                if (JS_IsException(l))
                    goto error;
                subargs = l;
            }
            if (qp_gavec_push_steal(ctx, &newargs, subargs) < 0)
                goto error;
            continue;
        }
        int unpack = qp_is_unpacked_typevartuple(ctx, arg);
        if (unpack < 0)
            goto error;
        JSValue subst;
        if (qp_getattr_opt(ctx, arg, QP_ATOM(ctx, __typing_subst__), &subst) < 0)
            goto error;
        if (!QP_IS_NULL(subst)) {
            Py_ssize_t iparam = qp_ga_index(QP_TUPLE_ITEMS(parameters), nparams, arg);
            if (iparam < 0) {
                /* __parameters__ may be stale if an argument gained
                   __typing_subst__ after the tuple was computed. */
                newarg = qp_raise_type_error(ctx, "argument %R with __typing_subst__ was not "
                                             "found in __parameters__", arg);
            } else {
                newarg = qp_call1(ctx, subst, argitems[iparam]);
            }
            JS_FreeValue(ctx, subst);
        } else {
            newarg = qp_subs_tvars(ctx, arg, parameters, argitems, nitems);
        }
        if (JS_IsException(newarg))
            goto error;
        if (unpack) {
            if (!qp_tuple_check(ctx, newarg)) {
                qp_raise_type_error(ctx, "expected __typing_subst__ of %T objects to return "
                                    "a tuple, not %T", arg, newarg);
                JS_FreeValue(ctx, newarg);
                goto error;
            }
            int er = qp_gavec_extend(ctx, &newargs, QP_TUPLE_ITEMS(newarg), QP_TUPLE_SIZE(newarg));
            JS_FreeValue(ctx, newarg);
            if (er < 0)
                goto error;
        } else if (qp_gavec_push_steal(ctx, &newargs, newarg) < 0) {
            goto error;
        }
    }
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, tuple_args);
    return qp_gavec_to_tuple(ctx, &newargs);
error:
    qp_gavec_free(ctx, &newargs);
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, tuple_args);
    return JS_EXCEPTION;
}

/* ---- GenericAlias -------------------------------------------------------- */

/* setup_ga: fill the slots of a fresh alias (args wrapped in a 1-tuple
   unless already a tuple). */
static int qp_ga_setup(JSContext *ctx, JSValueConst self, JSValueConst origin, JSValueConst args)
{
    JSValue targs;
    if (!qp_tuple_check(ctx, args)) {
        targs = qp_tuple_pack(ctx, 1, &args);
        if (JS_IsException(targs))
            return -1;
    } else {
        targs = JS_DupValue(ctx, args);
    }
    JSValue *s = qp_instance_slots(self);
    s[QP_GA_ORIGIN] = JS_DupValue(ctx, origin);
    s[QP_GA_ARGS] = targs;
    s[QP_GA_PARAMS] = QP_NULL;
    s[QP_GA_STARRED] = JS_FALSE;
    return 0;
}

/* Py_GenericAlias */
static JSValue qp_generic_alias_new(JSContext *ctx, JSValueConst origin, JSValueConst args)
{
    JSValue alias = qp_object_new_instance(ctx, QP_TYPE(ctx, GenericAlias), JS_CLASS_PY_OBJECT);
    if (JS_IsException(alias))
        return JS_EXCEPTION;
    if (qp_ga_setup(ctx, alias, origin, args) < 0) {
        JS_FreeValue(ctx, alias);
        return JS_EXCEPTION;
    }
    return alias;
}

static JSValue qp_ga_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                         size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    if (qp_arg_no_keywords(ctx, "GenericAlias", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "GenericAlias", (int)nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    JSValue self = qp_object_new_instance(ctx, subtype, subtype->instance_class_id);
    if (JS_IsException(self))
        return JS_EXCEPTION;
    if (qp_ga_setup(ctx, self, args[0], args[1]) < 0) {
        JS_FreeValue(ctx, self);
        return JS_EXCEPTION;
    }
    return self;
}

/* __parameters__, computed on first use (ga_parameters_lock_held) */
static JSValue qp_ga_parameters(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue *s = qp_instance_slots(self);
    if (QP_IS_NULL(s[QP_GA_PARAMS])) {
        JSValue p = qp_make_parameters(ctx, s[QP_GA_ARGS]);
        if (JS_IsException(p))
            return JS_EXCEPTION;
        s = qp_instance_slots(self);
        if (QP_IS_NULL(s[QP_GA_PARAMS]))
            s[QP_GA_PARAMS] = p;
        else
            JS_FreeValue(ctx, p);
    }
    return JS_DupValue(ctx, s[QP_GA_PARAMS]);
}

static JSValue qp_ga_unpacked_tuple_args(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue *s = qp_instance_slots(self);
    if (qp_ga_starred(self) && qp_is(s[QP_GA_ORIGIN], QP_TYPE_VALUE(QP_TYPE(ctx, tuple))))
        return JS_DupValue(ctx, s[QP_GA_ARGS]);
    return qp_none();
}

static JSValue qp_ga_getitem(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    JSValue parameters = qp_ga_parameters(ctx, self, 0);
    if (JS_IsException(parameters))
        return JS_EXCEPTION;
    JSValue *s = qp_instance_slots(self);
    JSValue newargs = qp_subs_parameters(ctx, self, s[QP_GA_ARGS], parameters, item);
    JS_FreeValue(ctx, parameters);
    if (JS_IsException(newargs))
        return JS_EXCEPTION;
    s = qp_instance_slots(self);
    JSValue res = qp_generic_alias_new(ctx, s[QP_GA_ORIGIN], newargs);
    JS_FreeValue(ctx, newargs);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    qp_instance_slots(res)[QP_GA_STARRED] = JS_NewBool(ctx, qp_ga_starred(self));
    return res;
}

static Py_hash_t qp_ga_hash(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    Py_hash_t h0 = qp_hash(ctx, s[QP_GA_ORIGIN]);
    if (h0 == -1)
        return -1;
    Py_hash_t h1 = qp_hash(ctx, s[QP_GA_ARGS]);
    if (h1 == -1)
        return -1;
    return h0 ^ h1;
}

/* GenericAlias(...)(*args): instantiate the origin, then try to set
   __orig_class__ (AttributeError/TypeError are ignored). */
static JSValue qp_ga_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    JSValue obj = qp_vectorcall(ctx, qp_instance_slots(self)[QP_GA_ORIGIN], args, nargsf, kwnames);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (qp_setattr(ctx, obj, QP_ATOM(ctx, __orig_class__), self) < 0) {
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)) &&
            !qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    }
    return obj;
}

/* Own attributes of the alias: everything else is looked up on __origin__. */
static const uint16_t qp_ga_attr_exceptions[] = {
    QP_ATOM___class__,
    QP_ATOM___origin__,
    QP_ATOM___args__,
    QP_ATOM___unpacked__,
    QP_ATOM___parameters__,
    QP_ATOM___typing_unpacked_tuple_args__,
    QP_ATOM___mro_entries__,
    QP_ATOM___reduce_ex__,  /* needed so we don't look up object.__reduce_ex__ */
    QP_ATOM___reduce__,
};

/* Never proxied to __origin__ (would break existing code). */
static const uint16_t qp_ga_attr_blocked[] = {
    QP_ATOM___bases__,
    QP_ATOM___copy__,
    QP_ATOM___deepcopy__,
};

static JSValue qp_ga_getattro(JSContext *ctx, JSValueConst self, JSAtom name)
{
    const JSAtom *atoms = QP_STATE(ctx)->atoms;
    size_t i;
    for (i = 0; i < countof(qp_ga_attr_blocked); i++) {
        if (atoms[qp_ga_attr_blocked[i]] == name)
            return qp_generic_getattr(ctx, self, name);
    }
    for (i = 0; i < countof(qp_ga_attr_exceptions); i++) {
        if (atoms[qp_ga_attr_exceptions[i]] == name)
            return qp_generic_getattr(ctx, self, name);
    }
    return qp_getattr(ctx, qp_instance_slots(self)[QP_GA_ORIGIN], name);
}

static JSValue qp_ga_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    if (!qp_ga_check(ctx, b) || (op != QP_CMP_EQ && op != QP_CMP_NE))
        return qp_not_implemented(ctx);
    if (op == QP_CMP_NE) {
        JSValue eq = qp_ga_richcompare(ctx, a, b, QP_CMP_EQ);
        if (JS_IsException(eq))
            return JS_EXCEPTION;
        BOOL is_true = JS_VALUE_GET_TAG(eq) == JS_TAG_BOOL && JS_VALUE_GET_INT(eq);
        JS_FreeValue(ctx, eq);
        return qp_bool_new(!is_true);
    }
    if (qp_ga_starred(a) != qp_ga_starred(b))
        return JS_FALSE;
    JSValue *sa = qp_instance_slots(a), *sb = qp_instance_slots(b);
    int eq = qp_richcompare_bool(ctx, sa[QP_GA_ORIGIN], sb[QP_GA_ORIGIN], QP_CMP_EQ);
    if (eq < 0)
        return JS_EXCEPTION;
    if (!eq)
        return JS_FALSE;
    sa = qp_instance_slots(a);
    sb = qp_instance_slots(b);
    return qp_richcompare(ctx, sa[QP_GA_ARGS], sb[QP_GA_ARGS], QP_CMP_EQ);
}

static JSValue qp_ga_mro_entries(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_tuple_pack(ctx, 1, &qp_instance_slots(self)[QP_GA_ORIGIN]);
}

static JSValue qp_ga_instancecheck(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_raise_type_error(ctx, "isinstance() argument 2 cannot be a parameterized generic");
}

static JSValue qp_ga_subclasscheck(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_raise_type_error(ctx, "issubclass() argument 2 cannot be a parameterized generic");
}

/* _PyEval_GetBuiltin: builtins of the calling frame (AttributeError(name)
   if missing). */
static JSValue qp_ga_get_builtin(JSContext *ctx, JSAtom name)
{
    JSValue builtins = qp_frame_builtins(qp_py_frame_at(ctx, 0));
    JSValue v;
    if (QP_IS_NULL(builtins))
        builtins = QP_STATE(ctx)->builtins_dict;
    int r = qp_dict_get_item_atom(ctx, builtins, name, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        JSValue s = qp_str_from_atom(ctx, name);
        if (JS_IsException(s))
            return JS_EXCEPTION;
        qp_raise_value(ctx, QP_EXC(ctx, AttributeError), s);
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    return v;
}

static JSValue qp_ga_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    if (qp_ga_starred(self)) {
        /* *alias -> next(iter(alias)) */
        JSValue tmp = qp_generic_alias_new(ctx, s[QP_GA_ORIGIN], s[QP_GA_ARGS]);
        if (JS_IsException(tmp))
            return JS_EXCEPTION;
        JSValue it = qp_iter(ctx, tmp);
        JS_FreeValue(ctx, tmp);
        if (JS_IsException(it))
            return JS_EXCEPTION;
        JSValue fn = qp_ga_get_builtin(ctx, QP_ATOM(ctx, next));
        if (JS_IsException(fn)) {
            JS_FreeValue(ctx, it);
            return JS_EXCEPTION;
        }
        JSValue inner = qp_tuple_new_steal(ctx, 1, &it);
        if (JS_IsException(inner)) {
            JS_FreeValue(ctx, fn);
            return JS_EXCEPTION;
        }
        JSValue items[2] = { fn, inner };
        return qp_tuple_new_steal(ctx, 2, items);
    }
    JSValue inner = qp_tuple_pack(ctx, 2, &s[QP_GA_ORIGIN]);
    if (JS_IsException(inner))
        return JS_EXCEPTION;
    JSValue items[2] = { JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self))), inner };
    return qp_tuple_new_steal(ctx, 2, items);
}

/* dir(origin) plus the alias' own attributes */
static JSValue qp_ga_dir(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    JSValue dir = qp_dir(ctx, qp_instance_slots(self)[QP_GA_ORIGIN]);
    if (JS_IsException(dir))
        return JS_EXCEPTION;
    for (size_t i = 0; i < countof(qp_ga_attr_exceptions); i++) {
        JSValue entry = qp_str_from_atom(ctx, QP_STATE(ctx)->atoms[qp_ga_attr_exceptions[i]]);
        if (JS_IsException(entry))
            goto error;
        int contains = qp_contains(ctx, dir, entry);
        if (contains < 0 || (contains == 0 && qp_list_append(ctx, dir, entry) < 0)) {
            JS_FreeValue(ctx, entry);
            goto error;
        }
        JS_FreeValue(ctx, entry);
    }
    return dir;
error:
    JS_FreeValue(ctx, dir);
    return JS_EXCEPTION;
}

static JSValue qp_ga_iter(JSContext *ctx, JSValueConst self)
{
    JSValue gi = qp_object_new_instance(ctx, QP_TYPE(ctx, generic_alias_iterator),
                                        JS_CLASS_PY_OBJECT);
    if (JS_IsException(gi))
        return JS_EXCEPTION;
    qp_instance_slots(gi)[QP_GAITER_OBJ] = JS_DupValue(ctx, self);
    return gi;
}

static const QPMethodDef qp_ga_methods[] = {
    { "__mro_entries__", qp_ga_mro_entries, QP_METH_O, 0, NULL },
    { "__instancecheck__", qp_ga_instancecheck, QP_METH_O, 0, NULL },
    { "__subclasscheck__", qp_ga_subclasscheck, QP_METH_O, 0, NULL },
    { "__reduce__", qp_ga_reduce, QP_METH_NOARGS, 0, NULL },
    { "__dir__", qp_ga_dir, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPMemberDef qp_ga_members[] = {
    { "__origin__", QP_GA_ORIGIN, QP_MEMBER_READONLY, NULL },
    { "__args__", QP_GA_ARGS, QP_MEMBER_READONLY, NULL },
    { "__unpacked__", QP_GA_STARRED, QP_MEMBER_READONLY, NULL },
    { NULL },
};

static const QPGetSetDef qp_ga_getsets[] = {
    { "__parameters__", qp_ga_parameters, NULL, 0, "Type variables in the GenericAlias." },
    { "__typing_unpacked_tuple_args__", qp_ga_unpacked_tuple_args, NULL, 0, NULL },
    { NULL },
};

static const QPTypeSpec qp_ga_spec = {
    .name = "types.GenericAlias",
    .doc = "GenericAlias(origin, args, /)\n"
           "--\n\n"
           "Represent a PEP 585 generic type\n"
           "\n"
           "For example, for t = list[int], t.__origin__ is list and t.__args__\n"
           "is (int,).",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_GA_NSLOTS,
    .slots = {
        .tp_repr = qp_ga_repr,
        .nb = { .nb_or = qp_union_new },  /* allow X | Y of GenericAlias objs */
        .mp = { .mp_subscript = qp_ga_getitem },
        .tp_hash = qp_ga_hash,
        .tp_call = qp_ga_call,
        .tp_getattro = qp_ga_getattro,
        .tp_richcompare = qp_ga_richcompare,
        .tp_new = qp_ga_new,
        .tp_iter = qp_ga_iter,
    },
    .methods = qp_ga_methods,
    .getsets = qp_ga_getsets,
    .members = qp_ga_members,
};

/* ---- generic_alias_iterator ---------------------------------------------- */

static JSValue qp_ga_iter_self(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

/* Yields one starred copy of the alias (*tuple[int]), then stops. */
static JSValue qp_ga_iternext(JSContext *ctx, JSValueConst it)
{
    JSValue *s = qp_instance_slots(it);
    JSValue obj = s[QP_GAITER_OBJ];
    s[QP_GAITER_OBJ] = QP_NULL;
    if (QP_IS_NULL(obj))
        return QP_NULL;
    JSValue *as = qp_instance_slots(obj);
    JSValue starred = qp_generic_alias_new(ctx, as[QP_GA_ORIGIN], as[QP_GA_ARGS]);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(starred))
        return JS_EXCEPTION;
    qp_instance_slots(starred)[QP_GA_STARRED] = JS_TRUE;
    return starred;
}

static JSValue qp_ga_iter_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    /* the builtin lookup may run arbitrary code: read the slot after it */
    JSValue fn = qp_ga_get_builtin(ctx, QP_ATOM(ctx, iter));
    if (JS_IsException(fn))
        return JS_EXCEPTION;
    JSValue obj = qp_instance_slots(self)[QP_GAITER_OBJ];
    JSValue inner;
    if (!QP_IS_NULL(obj)) {
        inner = qp_tuple_pack(ctx, 1, &obj);
    } else {
        JSValue empty = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
        inner = qp_tuple_new_steal(ctx, 1, &empty);
    }
    if (JS_IsException(inner)) {
        JS_FreeValue(ctx, fn);
        return JS_EXCEPTION;
    }
    JSValue items[2] = { fn, inner };
    return qp_tuple_new_steal(ctx, 2, items);
}

static const QPMethodDef qp_ga_iter_methods[] = {
    { "__reduce__", qp_ga_iter_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL },
};

static const QPTypeSpec qp_ga_iter_spec = {
    .name = "generic_alias_iterator",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_GAITER_NSLOTS,
    .slots = {
        .tp_iter = qp_ga_iter_self,
        .tp_iternext = qp_ga_iternext,
    },
    .methods = qp_ga_iter_methods,
};

/* ---- typing.Union ---------------------------------------------------------- */

/* Borrowed __args__ tuple of a Union (CPython _Py_union_args). */
static JSValue qp_union_args(JSValueConst self)
{
    return qp_instance_slots(self)[QP_UNION_ARGS];
}

static Py_hash_t qp_union_hash(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    /* If there are any unhashable args, treat this union as unhashable.
       Otherwise, two unions might compare equal but have different hashes. */
    if (!QP_IS_NULL(s[QP_UNION_UNHASHABLE])) {
        JSValue u = JS_DupValue(ctx, s[QP_UNION_UNHASHABLE]);
        Py_ssize_t n = QP_TUPLE_SIZE(u);
        /* Attempt to get an error from one of the values. */
        for (Py_ssize_t i = 0; i < n; i++) {
            if (qp_hash(ctx, QP_TUPLE_ITEMS(u)[i]) == -1) {
                JS_FreeValue(ctx, u);
                return -1;
            }
        }
        JS_FreeValue(ctx, u);
        /* The unhashable values somehow became hashable again. Still raise
           an error. */
        qp_raise_type_error(ctx, "union contains %zd unhashable elements", n);
        return -1;
    }
    return qp_hash(ctx, s[QP_UNION_HASHABLE]);
}

/* 1 if every element of a is in b (== comparison), 0, or -1. */
static int qp_union_all_contained(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    Py_ssize_t n = QP_TUPLE_SIZE(a);
    for (Py_ssize_t i = 0; i < n; i++) {
        int r = qp_contains(ctx, b, QP_TUPLE_ITEMS(a)[i]);
        if (r <= 0)
            return r;
    }
    return 1;
}

static int qp_unions_equal(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue *sa = qp_instance_slots(a), *sb = qp_instance_slots(b);
    int result = qp_richcompare_bool(ctx, sa[QP_UNION_HASHABLE], sb[QP_UNION_HASHABLE], QP_CMP_EQ);
    if (result <= 0)
        return result;
    sa = qp_instance_slots(a);
    sb = qp_instance_slots(b);
    JSValueConst ua = sa[QP_UNION_UNHASHABLE], ub = sb[QP_UNION_UNHASHABLE];
    if (!QP_IS_NULL(ua) && !QP_IS_NULL(ub)) {
        if (QP_TUPLE_SIZE(ua) != QP_TUPLE_SIZE(ub))
            return 0;
        result = qp_union_all_contained(ctx, ua, ub);
        if (result <= 0)
            return result;
        return qp_union_all_contained(ctx, ub, ua);
    }
    if (!QP_IS_NULL(ua) || !QP_IS_NULL(ub))
        return 0;
    return 1;
}

static JSValue qp_union_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    if (!qp_union_check(ctx, b) || (op != QP_CMP_EQ && op != QP_CMP_NE))
        return qp_not_implemented(ctx);
    int equal = qp_unions_equal(ctx, a, b);
    if (equal < 0)
        return JS_EXCEPTION;
    return qp_bool_new(op == QP_CMP_EQ ? equal : !equal);
}

/* Accumulates the deduplicated, flattened members of a new union. */
typedef struct QPUnionBuilder {
    JSValue args;               /* list */
    JSValue hashable_args;      /* set */
    JSValue unhashable_args;    /* list or QP_NULL */
    BOOL is_checked;            /* whether to call type_check() */
} QPUnionBuilder;

static int qp_unionbuilder_init(JSContext *ctx, QPUnionBuilder *ub, BOOL is_checked)
{
    ub->args = qp_list_new(ctx, 0);
    if (JS_IsException(ub->args))
        return -1;
    ub->hashable_args = qp_set_new(ctx, QP_NULL);
    if (JS_IsException(ub->hashable_args)) {
        JS_FreeValue(ctx, ub->args);
        return -1;
    }
    ub->unhashable_args = QP_NULL;
    ub->is_checked = is_checked;
    return 0;
}

static void qp_unionbuilder_finalize(JSContext *ctx, QPUnionBuilder *ub)
{
    JS_FreeValue(ctx, ub->args);
    JS_FreeValue(ctx, ub->hashable_args);
    JS_FreeValue(ctx, ub->unhashable_args);
}

static int qp_unionbuilder_add_single_unchecked(JSContext *ctx, QPUnionBuilder *ub,
                                                JSValueConst arg)
{
    Py_hash_t hash = qp_hash(ctx, arg);
    if (hash == -1) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        if (QP_IS_NULL(ub->unhashable_args)) {
            ub->unhashable_args = qp_list_new(ctx, 0);
            if (JS_IsException(ub->unhashable_args)) {
                ub->unhashable_args = QP_NULL;
                return -1;
            }
        } else {
            int contains = qp_contains(ctx, ub->unhashable_args, arg);
            if (contains < 0)
                return -1;
            if (contains == 1)
                return 0;
        }
        if (qp_list_append(ctx, ub->unhashable_args, arg) < 0)
            return -1;
    } else {
        int contains = qp_set_contains(ctx, ub->hashable_args, arg);
        if (contains < 0)
            return -1;
        if (contains == 1)
            return 0;
        if (qp_set_add(ctx, ub->hashable_args, arg) < 0)
            return -1;
    }
    return qp_list_append(ctx, ub->args, arg);
}

/* call_typing_func_object: typing.<name>(*args) */
static JSValue qp_union_call_typing(JSContext *ctx, JSAtom name, JSValueConst *args, size_t nargs)
{
    JSValue typing = qp_import_module(ctx, "typing");
    if (JS_IsException(typing))
        return JS_EXCEPTION;
    JSValue func = qp_getattr(ctx, typing, name);
    JS_FreeValue(ctx, typing);
    if (JS_IsException(func))
        return JS_EXCEPTION;
    JSValue result = qp_vectorcall(ctx, func, args, nargs, QP_NULL);
    JS_FreeValue(ctx, func);
    return result;
}

/* is_unionable: valid operand of the | operator for types. */
static BOOL qp_is_unionable(JSContext *ctx, JSValueConst obj)
{
    QPType *t;
    if (qp_ga_is_none(obj) || qp_type_from_value(obj) || qp_ga_check(ctx, obj))
        return TRUE;
    t = qp_type_of(ctx, obj);
    return t == QP_TYPE(ctx, sentinel) || t == QP_TYPE(ctx, Union) ||
           t == QP_TYPE(ctx, TypeAliasType);
}

/* type_check: None -> NoneType, unionable values unchanged, anything else
   through typing._type_check(arg, msg). */
static JSValue qp_union_type_check(JSContext *ctx, JSValueConst arg, const char *msg)
{
    if (qp_ga_is_none(arg))
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, NoneType)));
    /* Fast path to avoid calling into typing.py */
    if (qp_is_unionable(ctx, arg))
        return JS_DupValue(ctx, arg);
    JSValue message_str = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(message_str))
        return JS_EXCEPTION;
    JSValueConst args[2] = { arg, message_str };
    JSValue result = qp_union_call_typing(ctx, QP_ATOM(ctx, _type_check), args, 2);
    JS_FreeValue(ctx, message_str);
    return result;
}

static int qp_unionbuilder_add_tuple(JSContext *ctx, QPUnionBuilder *ub, JSValueConst tuple);

static int qp_unionbuilder_add_single(JSContext *ctx, QPUnionBuilder *ub, JSValueConst arg)
{
    if (qp_ga_is_none(arg)) {
        arg = QP_TYPE_VALUE(QP_TYPE(ctx, NoneType));
    } else if (qp_union_check(ctx, arg)) {
        return qp_unionbuilder_add_tuple(ctx, ub, qp_union_args(arg));
    }
    if (ub->is_checked) {
        JSValue type = qp_union_type_check(ctx, arg, "Union[arg, ...]: each arg must be a type.");
        if (JS_IsException(type))
            return -1;
        int r = qp_unionbuilder_add_single_unchecked(ctx, ub, type);
        JS_FreeValue(ctx, type);
        return r;
    }
    return qp_unionbuilder_add_single_unchecked(ctx, ub, arg);
}

static int qp_unionbuilder_add_tuple(JSContext *ctx, QPUnionBuilder *ub, JSValueConst tuple)
{
    /* hold the tuple: adding members may run arbitrary code */
    JSValue t = JS_DupValue(ctx, tuple);
    Py_ssize_t n = QP_TUPLE_SIZE(t);
    for (Py_ssize_t i = 0; i < n; i++) {
        if (qp_unionbuilder_add_single(ctx, ub, QP_TUPLE_ITEMS(t)[i]) < 0) {
            JS_FreeValue(ctx, t);
            return -1;
        }
    }
    JS_FreeValue(ctx, t);
    return 0;
}

/* make_union: consumes the builder.  A single member is returned as is. */
static JSValue qp_make_union(JSContext *ctx, QPUnionBuilder *ub)
{
    JSValue args = JS_UNDEFINED, hashable_args = JS_UNDEFINED, unhashable_args = QP_NULL;
    Py_ssize_t n = qp_list_size(ub->args);
    if (n == 0) {
        qp_raise_type_error(ctx, "Cannot take a Union of no types.");
        qp_unionbuilder_finalize(ctx, ub);
        return JS_EXCEPTION;
    }
    if (n == 1) {
        JSValue result = qp_ga_list_item(ctx, ub->args, 0);
        qp_unionbuilder_finalize(ctx, ub);
        return result;
    }
    args = qp_list_as_tuple(ctx, ub->args);
    if (JS_IsException(args))
        goto error;
    hashable_args = qp_frozenset_new(ctx, ub->hashable_args);
    if (JS_IsException(hashable_args))
        goto error;
    if (!QP_IS_NULL(ub->unhashable_args)) {
        unhashable_args = qp_list_as_tuple(ctx, ub->unhashable_args);
        if (JS_IsException(unhashable_args)) {
            unhashable_args = QP_NULL;
            goto error;
        }
    }
    JSValue result = qp_object_new_instance(ctx, QP_TYPE(ctx, Union), JS_CLASS_PY_OBJECT);
    if (JS_IsException(result))
        goto error;
    qp_unionbuilder_finalize(ctx, ub);
    JSValue *s = qp_instance_slots(result);
    s[QP_UNION_ARGS] = args;
    s[QP_UNION_HASHABLE] = hashable_args;
    s[QP_UNION_UNHASHABLE] = unhashable_args;
    s[QP_UNION_PARAMS] = QP_NULL;
    return result;
error:
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, hashable_args);
    JS_FreeValue(ctx, unhashable_args);
    qp_unionbuilder_finalize(ctx, ub);
    return JS_EXCEPTION;
}

/* _Py_union_type_or: X | Y for types, aliases and unions; NotImplemented
   for any other operand. */
static JSValue qp_union_new(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    QPUnionBuilder ub;
    if (!qp_is_unionable(ctx, self) || !qp_is_unionable(ctx, other))
        return qp_not_implemented(ctx);
    /* unchecked because we already checked is_unionable() */
    if (qp_unionbuilder_init(ctx, &ub, FALSE) < 0)
        return JS_EXCEPTION;
    if (qp_unionbuilder_add_single(ctx, &ub, self) < 0 ||
        qp_unionbuilder_add_single(ctx, &ub, other) < 0) {
        qp_unionbuilder_finalize(ctx, &ub);
        return JS_EXCEPTION;
    }
    return qp_make_union(ctx, &ub);
}

/* _Py_union_from_tuple: Union[args] (an exact tuple is a member list). */
static JSValue qp_union_from_tuple(JSContext *ctx, JSValueConst args)
{
    QPUnionBuilder ub;
    int r;
    if (qp_unionbuilder_init(ctx, &ub, TRUE) < 0)
        return JS_EXCEPTION;
    if (qp_tuple_check_exact(ctx, args))
        r = qp_unionbuilder_add_tuple(ctx, &ub, args);
    else
        r = qp_unionbuilder_add_single(ctx, &ub, args);
    if (r < 0) {
        qp_unionbuilder_finalize(ctx, &ub);
        return JS_EXCEPTION;
    }
    return qp_make_union(ctx, &ub);
}

static JSValue qp_union_repr(JSContext *ctx, JSValueConst self)
{
    JSValueConst args = qp_union_args(self);
    Py_ssize_t len = QP_TUPLE_SIZE(args);
    StringBuffer b;

    string_buffer_init(ctx, &b, 16);
    for (Py_ssize_t i = 0; i < len; i++) {
        if (i > 0 && string_buffer_puts8(&b, " | "))
            goto error;
        if (qp_typing_type_repr(ctx, &b, QP_TUPLE_ITEMS(args)[i]) < 0)
            goto error;
    }
    return string_buffer_end(&b);
error:
    string_buffer_free(&b);
    return JS_EXCEPTION;
}

/* union_init_parameters: compute __parameters__ once. */
static int qp_union_init_parameters(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    if (QP_IS_NULL(s[QP_UNION_PARAMS])) {
        JSValue p = qp_make_parameters(ctx, s[QP_UNION_ARGS]);
        if (JS_IsException(p))
            return -1;
        s = qp_instance_slots(self);
        if (QP_IS_NULL(s[QP_UNION_PARAMS]))
            s[QP_UNION_PARAMS] = p;
        else
            JS_FreeValue(ctx, p);
    }
    return 0;
}

static JSValue qp_union_getitem(JSContext *ctx, JSValueConst self, JSValueConst item)
{
    if (qp_union_init_parameters(ctx, self) < 0)
        return JS_EXCEPTION;
    JSValue *s = qp_instance_slots(self);
    JSValue params = JS_DupValue(ctx, s[QP_UNION_PARAMS]);
    JSValue newargs = qp_subs_parameters(ctx, self, s[QP_UNION_ARGS], params, item);
    JS_FreeValue(ctx, params);
    if (JS_IsException(newargs))
        return JS_EXCEPTION;
    JSValue res = qp_union_from_tuple(ctx, newargs);
    JS_FreeValue(ctx, newargs);
    return res;
}

static JSValue qp_union_parameters(JSContext *ctx, JSValueConst self, int magic)
{
    if (qp_union_init_parameters(ctx, self) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, qp_instance_slots(self)[QP_UNION_PARAMS]);
}

static JSValue qp_union_name(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_str_from_cstr(ctx, "Union");
}

static JSValue qp_union_origin(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, Union)));
}

/* Union | X: like Union[a, b] (checked through typing._type_check). */
static JSValue qp_union_nb_or(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QPUnionBuilder ub;
    if (qp_unionbuilder_init(ctx, &ub, TRUE) < 0)
        return JS_EXCEPTION;
    if (qp_unionbuilder_add_single(ctx, &ub, a) < 0 ||
        qp_unionbuilder_add_single(ctx, &ub, b) < 0) {
        qp_unionbuilder_finalize(ctx, &ub);
        return JS_EXCEPTION;
    }
    return qp_make_union(ctx, &ub);
}

/* __module__ comes from the class (required by the typing module). */
static JSValue qp_union_getattro(JSContext *ctx, JSValueConst self, JSAtom name)
{
    if (name == QP_ATOM(ctx, __module__))
        return qp_getattr(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)), name);
    return qp_generic_getattr(ctx, self, name);
}

static JSValue qp_union_class_getitem(JSContext *ctx, JSValueConst cls, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_union_from_tuple(ctx, args[0]);
}

static JSValue qp_union_mro_entries(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_raise_type_error(ctx, "Cannot subclass %R", self);
}

static const QPMethodDef qp_union_methods[] = {
    { "__mro_entries__", qp_union_mro_entries, QP_METH_O, 0, NULL },
    { "__class_getitem__", qp_union_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Create a union containing the given types" },
    { NULL },
};

static const QPMemberDef qp_union_members[] = {
    { "__args__", QP_UNION_ARGS, QP_MEMBER_READONLY, NULL },
    { NULL },
};

static const QPGetSetDef qp_union_getsets[] = {
    { "__name__", qp_union_name, NULL, 0, "Name of the type" },
    { "__qualname__", qp_union_name, NULL, 0, "Qualified name of the type" },
    { "__origin__", qp_union_origin, NULL, 0, "Always returns the type" },
    { "__parameters__", qp_union_parameters, NULL, 0, "Type variables in the types.UnionType." },
    { NULL },
};

static const QPTypeSpec qp_union_spec = {
    .name = "typing.Union",
    .doc = "Represent a union type\n"
           "\n"
           "E.g. for int | str",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_UNION_NSLOTS,
    .slots = {
        .tp_hash = qp_union_hash,
        .tp_getattro = qp_union_getattro,
        .tp_richcompare = qp_union_richcompare,
        .mp = { .mp_subscript = qp_union_getitem },
        .nb = { .nb_or = qp_union_nb_or },
        .tp_repr = qp_union_repr,
    },
    .methods = qp_union_methods,
    .getsets = qp_union_getsets,
    .members = qp_union_members,
};

/* Ready types.GenericAlias, typing.Union and generic_alias_iterator. */
static int qp_init_genericalias(JSContext *ctx)
{
    if (qp_type_ready(ctx, QP_TYPE(ctx, GenericAlias), &qp_ga_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, Union), &qp_union_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, generic_alias_iterator), &qp_ga_iter_spec) < 0)
        return -1;
    return 0;
}
