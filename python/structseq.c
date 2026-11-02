/*
 * Struct sequences (CPython Objects/structseq.c, PSF-2.0): named tuple-like
 * types such as os.stat_result, time.struct_time, sys.version_info.
 *
 * Layout: instances are PY_TUPLE objects whose QPTuple holds the visible
 * fields (n_sequence_fields); the hidden named fields (index >=
 * n_sequence_fields) are stored as integer-indexed JS own properties of
 * the instance object, which Python attribute lookup never sees.  Named
 * fields are exposed by read-only properties on the type.  The descriptor
 * pointer is kept as a JS (not Python) property of the type object.
 */

static JSValue qp_tup_alloc(JSContext *ctx, QPType *type, Py_ssize_t n);

#define QP_STRUCTSEQ_DESC_PROP "\x01qp_structseq_desc"

static const QPStructSeqDesc *qp_ss_desc_of_type(JSContext *ctx, QPType *t)
{
    for (; t; t = t->base) {
        JSValue tv = JS_MKPTR(JS_TAG_OBJECT, t->obj);
        JSValue v = JS_GetPropertyStr(ctx, tv, QP_STRUCTSEQ_DESC_PROP);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64 || JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
            double d;
            JS_ToFloat64(ctx, &d, v);
            if (d != 0)
                return (const QPStructSeqDesc *)(uintptr_t)d;
        }
        JS_FreeValue(ctx, v);
    }
    return NULL;
}

static void qp_ss_counts(const QPStructSeqDesc *desc, int *n_fields, int *n_unnamed)
{
    int i, nu = 0;
    for (i = 0; desc->fields[i]; i++)
        if (desc->fields[i] == QP_STRUCTSEQ_UNNAMED)
            nu++;
    *n_fields = i;
    *n_unnamed = nu;
}

/* name of the i-th member in CPython's tp_members (named fields only) */
static const char *qp_ss_member_name(const QPStructSeqDesc *desc, int k)
{
    int i, j = 0;
    for (i = 0; desc->fields[i]; i++) {
        if (desc->fields[i] == QP_STRUCTSEQ_UNNAMED)
            continue;
        if (j++ == k)
            return desc->fields[i];
    }
    return NULL;
}

static const QPStructSeqDesc *qp_ss_desc(JSContext *ctx, JSValueConst obj)
{
    const QPStructSeqDesc *d = qp_ss_desc_of_type(ctx, qp_type_of(ctx, obj));
    if (!d)
        qp_raise_system_error(ctx, "not a structseq instance");
    return d;
}

static JSValue qp_ss_alloc(JSContext *ctx, QPType *t)
{
    const QPStructSeqDesc *d = qp_ss_desc_of_type(ctx, t);
    if (!d)
        return qp_raise_system_error(ctx, "not a structseq type");
    return qp_tup_alloc(ctx, t, d->n_in_sequence);
}

static int qp_ss_set(JSContext *ctx, JSValueConst obj, int i, JSValue v)
{
    QPTuple *tp = QP_TUPLE(obj);
    if (i < (int)tp->len) {
        JS_FreeValue(ctx, tp->items[i]);
        tp->items[i] = v;
        return 0;
    }
    if (JS_DefinePropertyValueUint32(ctx, obj, (uint32_t)i, v, JS_PROP_WRITABLE) < 0)
        return -1;
    return 0;
}

static JSValue qp_ss_get(JSContext *ctx, JSValueConst obj, int i)
{
    QPTuple *tp = QP_TUPLE(obj);
    if (i < (int)tp->len)
        return JS_DupValue(ctx, tp->items[i]);
    return JS_GetPropertyUint32(ctx, obj, (uint32_t)i);
}

/* property getter: self = field index */
static JSValue qp_ss_field_get(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    int idx = JS_VALUE_GET_INT(self);
    if (!qp_tuple_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "descriptor requires a structseq instance");
    return qp_ss_get(ctx, args[0], idx);
}

static const QPMethodDef qp_ss_field_get_def = {
    "_structseq_field", qp_ss_field_get, QP_METH_O, 0, NULL
};

/* structseq_new_impl: __new__(cls, sequence, dict=None) */
static JSValue qp_ss_tp_new(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "sequence", "dict", NULL };
    static const QPArgSpec spec = { "structseq", kw, 2, 3, 0 };
    JSValueConst a[3];
    QPType *type;
    const QPStructSeqDesc *d;
    int n_fields, n_unnamed, min_len, max_len, i;
    Py_ssize_t len;
    JSValue seq, res;
    JSValueConst dict;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    type = qp_type_from_value(a[0]);
    d = type ? qp_ss_desc_of_type(ctx, type) : NULL;
    if (!d)
        return qp_raise_type_error(ctx, "structseq.__new__(X): X is not a structseq type");
    qp_ss_counts(d, &n_fields, &n_unnamed);
    min_len = d->n_in_sequence;
    max_len = n_fields;
    dict = JS_IsUninitialized(a[2]) || JS_IsUndefined(a[2]) ? QP_NULL : a[2];

    if (qp_tuple_check(ctx, a[1]) || qp_list_check(ctx, a[1])) {
        seq = JS_DupValue(ctx, a[1]);
    } else {
        /* PySequence_Fast */
        JSValue it = qp_iter(ctx, a[1]);
        if (JS_IsException(it)) {
            if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return qp_raise_type_error(ctx, "constructor requires a sequence");
            }
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, it);
        seq = qp_list_from_iterable(ctx, a[1]);
        if (JS_IsException(seq))
            return seq;
    }
    if (!JS_IsUninitialized(dict) && !qp_dict_check(ctx, dict)) {
        JS_FreeValue(ctx, seq);
        return qp_raise_type_error(ctx, "%s() takes a dict as second arg, if any", d->name);
    }
    len = qp_tuple_check(ctx, seq) ? QP_TUPLE_SIZE(seq) : qp_list_size(seq);
    if (min_len != max_len) {
        if (len < min_len) {
            JS_FreeValue(ctx, seq);
            return qp_raise_type_error(ctx, "%s() takes an at least %zd-sequence (%zd-sequence given)",
                                       d->name, (Py_ssize_t)min_len, len);
        }
        if (len > max_len) {
            JS_FreeValue(ctx, seq);
            return qp_raise_type_error(ctx, "%s() takes an at most %zd-sequence (%zd-sequence given)",
                                       d->name, (Py_ssize_t)max_len, len);
        }
    } else if (len != min_len) {
        JS_FreeValue(ctx, seq);
        return qp_raise_type_error(ctx, "%s() takes a %zd-sequence (%zd-sequence given)",
                                   d->name, (Py_ssize_t)min_len, len);
    }
    res = qp_ss_alloc(ctx, type);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, seq);
        return res;
    }
    for (i = 0; i < len; i++) {
        JSValue v;
        if (qp_tuple_check(ctx, seq))
            v = JS_DupValue(ctx, QP_TUPLE_ITEMS(seq)[i]);
        else {
            Py_ssize_t n;
            JSValue *items = qp_list_items(seq, &n);
            v = i < n ? JS_DupValue(ctx, items[i]) : JS_UNDEFINED;
        }
        if (qp_ss_set(ctx, res, i, v) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, seq);
    seq = JS_UNDEFINED;
    if (!JS_IsUninitialized(dict) && qp_dict_size(dict) > 0) {
        Py_ssize_t found = 0;
        for (i = len; i < max_len; i++) {
            const char *name = qp_ss_member_name(d, i - n_unnamed);
            JSAtom at = JS_NewAtom(ctx, name);
            JSValue ob;
            int r = qp_dict_get_item_atom(ctx, dict, at, &ob);
            JS_FreeAtom(ctx, at);
            if (r < 0)
                goto fail;
            if (r == 0 || JS_IsUninitialized(ob))
                ob = JS_UNDEFINED;
            else
                found++;
            if (qp_ss_set(ctx, res, i, ob) < 0)
                goto fail;
        }
        if (qp_dict_size(dict) > found) {
            JS_FreeValue(ctx, res);
            return qp_raise_type_error(ctx, "%s() got duplicate or unexpected field name(s)", d->name);
        }
    } else {
        for (i = len; i < max_len; i++)
            if (qp_ss_set(ctx, res, i, JS_UNDEFINED) < 0)
                goto fail;
    }
    return res;
fail:
    JS_FreeValue(ctx, seq);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

/* structseq_repr */
static JSValue qp_ss_repr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    const QPStructSeqDesc *d = qp_ss_desc(ctx, self);
    JSValue acc, part;
    int i, n;
    if (!d)
        return JS_EXCEPTION;
    n = (int)QP_TUPLE_SIZE(self);
    acc = qp_str_from_format(ctx, "%s(", d->name);
    for (i = 0; i < n && !JS_IsException(acc); i++) {
        const char *name = qp_ss_member_name(d, i);
        if (!name) {
            JS_FreeValue(ctx, acc);
            return qp_raise_system_error(ctx, "In structseq_repr(), member %d name is NULL for type %s",
                                         i, d->name);
        }
        part = qp_str_from_format(ctx, "%s%s=%R", i > 0 ? ", " : "", name, QP_TUPLE_ITEMS(self)[i]);
        if (JS_IsException(part)) {
            JS_FreeValue(ctx, acc);
            return part;
        }
        {
            JSValue t = qp_str_concat(ctx, acc, part);
            JS_FreeValue(ctx, acc);
            JS_FreeValue(ctx, part);
            acc = t;
        }
    }
    if (JS_IsException(acc))
        return acc;
    part = qp_str_from_cstr(ctx, ")");
    {
        JSValue t = qp_str_concat(ctx, acc, part);
        JS_FreeValue(ctx, acc);
        JS_FreeValue(ctx, part);
        return t;
    }
}

/* structseq_reduce */
static JSValue qp_ss_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    const QPStructSeqDesc *d = qp_ss_desc(ctx, self);
    int n_fields, n_unnamed, nvis, i;
    JSValue tup, dict, inner, items[2], res, tv;
    if (!d)
        return JS_EXCEPTION;
    qp_ss_counts(d, &n_fields, &n_unnamed);
    nvis = (int)QP_TUPLE_SIZE(self);
    tup = qp_tuple_pack(ctx, nvis, QP_TUPLE_ITEMS(self));
    if (JS_IsException(tup))
        return tup;
    dict = qp_dict_new(ctx);
    if (JS_IsException(dict)) {
        JS_FreeValue(ctx, tup);
        return dict;
    }
    for (i = nvis; i < n_fields; i++) {
        JSAtom at = JS_NewAtom(ctx, qp_ss_member_name(d, i - n_unnamed));
        JSValue v = qp_ss_get(ctx, self, i);
        int r = JS_IsException(v) ? -1 : qp_dict_set_item_atom(ctx, dict, at, v);
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, v);
        if (r < 0) {
            JS_FreeValue(ctx, tup);
            JS_FreeValue(ctx, dict);
            return JS_EXCEPTION;
        }
    }
    items[0] = tup;
    items[1] = dict;
    inner = qp_tuple_new_steal(ctx, 2, items);
    if (JS_IsException(inner))
        return inner;
    tv = JS_MKPTR(JS_TAG_OBJECT, qp_type_of(ctx, self)->obj);
    items[0] = JS_DupValue(ctx, tv);
    items[1] = inner;
    res = qp_tuple_new_steal(ctx, 2, items);
    return res;
}

/* structseq_replace */
static JSValue qp_ss_replace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    const QPStructSeqDesc *d = qp_ss_desc(ctx, self);
    int n_fields, n_unnamed, i, nkw, k;
    JSValue res;
    uint8_t *used = NULL;
    if (!d)
        return JS_EXCEPTION;
    if (nargs > 0)
        return qp_raise_type_error(ctx, "__replace__() takes no positional arguments");
    qp_ss_counts(d, &n_fields, &n_unnamed);
    if (n_unnamed > 0)
        return qp_raise_type_error(ctx, "__replace__() is not supported for %s because it has unnamed field(s)",
                                   d->name);
    res = qp_ss_alloc(ctx, qp_type_of(ctx, self));
    if (JS_IsException(res))
        return res;
    nkw = JS_IsUninitialized(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    if (nkw) {
        used = js_mallocz(ctx, nkw);
        if (!used)
            goto fail;
    }
    for (i = 0; i < n_fields; i++) {
        JSValue v = QP_NULL;
        for (k = 0; k < nkw; k++) {
            const char *kn = qp_str_utf8(ctx, QP_TUPLE_ITEMS(kwnames)[k], NULL);
            if (kn && !strcmp(kn, d->fields[i])) {
                v = JS_DupValue(ctx, args[nargs + k]);
                used[k] = 1;
                break;
            }
        }
        if (JS_IsUninitialized(v))
            v = qp_ss_get(ctx, self, i);
        if (JS_IsException(v) || qp_ss_set(ctx, res, i, v) < 0)
            goto fail;
    }
    for (k = 0; k < nkw; k++) {
        if (!used[k]) {
            JSValue names = qp_list_new(ctx, 0);
            int j;
            if (JS_IsException(names))
                goto fail;
            for (j = 0; j < nkw; j++)
                if (!used[j] && qp_list_append(ctx, names, QP_TUPLE_ITEMS(kwnames)[j]) < 0) {
                    JS_FreeValue(ctx, names);
                    goto fail;
                }
            qp_raise_type_error(ctx, "Got unexpected field name(s): %R", names);
            JS_FreeValue(ctx, names);
            goto fail;
        }
    }
    js_free(ctx, used);
    return res;
fail:
    js_free(ctx, used);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static const QPMethodDef qp_ss_new_def = {
    "__new__", qp_ss_tp_new, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
    "Create and return a new object.  See help(type) for accurate signature."
};
static const QPMethodDef qp_ss_methods[] = {
    { "__repr__", qp_ss_repr, QP_METH_NOARGS, 0, "Return repr(self)." },
    { "__reduce__", qp_ss_reduce, QP_METH_NOARGS, 0, NULL },
    { "__replace__", qp_ss_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "__replace__($self, /, **changes)\n--\n\nReturn a copy of the structure with new values for the specified fields." },
    { NULL }
};

static int qp_ss_set_str(JSContext *ctx, JSValueConst d, const char *key, JSValue v)
{
    JSAtom at;
    int r;
    if (JS_IsException(v))
        return -1;
    at = JS_NewAtom(ctx, key);
    r = qp_dict_set_item_atom(ctx, d, at, v);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue qp_structseq_new_type(JSContext *ctx, const QPStructSeqDesc *desc)
{
    int n_fields, n_unnamed, i;
    const char *dot = strrchr(desc->name, '.');
    JSValue ns, name, bases, type = JS_EXCEPTION, match, base;
    QPType *t;

    qp_ss_counts(desc, &n_fields, &n_unnamed);
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns))
        return ns;
    if (dot) {
        if (qp_ss_set_str(ctx, ns, "__module__", qp_str_from_utf8(ctx, desc->name, dot - desc->name)) < 0)
            goto done;
    } else if (qp_ss_set_str(ctx, ns, "__module__", qp_str_from_cstr(ctx, "builtins")) < 0)
        goto done;
    if (qp_ss_set_str(ctx, ns, "__doc__", desc->doc ? qp_str_from_cstr(ctx, desc->doc) : JS_UNDEFINED) < 0 ||
        qp_ss_set_str(ctx, ns, "__slots__", qp_tuple_new(ctx, 0)) < 0 ||
        qp_ss_set_str(ctx, ns, "n_sequence_fields", JS_NewInt32(ctx, desc->n_in_sequence)) < 0 ||
        qp_ss_set_str(ctx, ns, "n_fields", JS_NewInt32(ctx, n_fields)) < 0 ||
        qp_ss_set_str(ctx, ns, "n_unnamed_fields", JS_NewInt32(ctx, n_unnamed)) < 0)
        goto done;
    /* __match_args__: visible named fields */
    match = qp_list_new(ctx, 0);
    if (JS_IsException(match))
        goto done;
    for (i = 0; i < desc->n_in_sequence; i++) {
        JSValue s;
        if (desc->fields[i] == QP_STRUCTSEQ_UNNAMED)
            continue;
        s = qp_str_from_cstr(ctx, desc->fields[i]);
        if (JS_IsException(s) || qp_list_append(ctx, match, s) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, match);
            goto done;
        }
        JS_FreeValue(ctx, s);
    }
    if (qp_ss_set_str(ctx, ns, "__match_args__", qp_list_as_tuple(ctx, match)) < 0) {
        JS_FreeValue(ctx, match);
        goto done;
    }
    JS_FreeValue(ctx, match);
    /* field properties */
    for (i = 0; desc->fields[i]; i++) {
        JSValue fget, argv[4], prop;
        if (desc->fields[i] == QP_STRUCTSEQ_UNNAMED)
            continue;
        fget = qp_cfunction_new(ctx, &qp_ss_field_get_def, JS_NewInt32(ctx, i), JS_UNDEFINED);
        if (JS_IsException(fget))
            goto done;
        argv[0] = fget;
        argv[1] = JS_UNDEFINED;
        argv[2] = JS_UNDEFINED;
        argv[3] = desc->field_docs && desc->field_docs[i] ? qp_str_from_cstr(ctx, desc->field_docs[i]) : JS_UNDEFINED;
        prop = qp_vectorcall(ctx, JS_MKPTR(JS_TAG_OBJECT, QP_TYPE(ctx, property)->obj), argv, 4, QP_NULL);
        JS_FreeValue(ctx, fget);
        JS_FreeValue(ctx, argv[3]);
        if (JS_IsException(prop))
            goto done;
        if (qp_ss_set_str(ctx, ns, desc->fields[i], prop) < 0)
            goto done;
    }
    name = qp_str_from_cstr(ctx, dot ? dot + 1 : desc->name);
    if (JS_IsException(name))
        goto done;
    base = JS_MKPTR(JS_TAG_OBJECT, QP_TYPE(ctx, tuple)->obj);
    bases = qp_tuple_pack(ctx, 1, &base);
    if (JS_IsException(bases)) {
        JS_FreeValue(ctx, name);
        goto done;
    }
    type = qp_type_new(ctx, QP_TYPE(ctx, type), name, bases, ns, QP_NULL);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, bases);
    if (JS_IsException(type))
        goto done;
    t = qp_type_from_value(type);
    if (JS_DefinePropertyValueStr(ctx, type, QP_STRUCTSEQ_DESC_PROP,
                                  JS_NewFloat64(ctx, (double)(uintptr_t)desc), 0) < 0)
        goto fail;
    {
        JSValue nw = qp_cfunction_new(ctx, &qp_ss_new_def, type, JS_UNDEFINED), sm;
        JSAtom at;
        int r;
        if (JS_IsException(nw))
            goto fail;
        sm = qp_staticmethod_new(ctx, nw);
        JS_FreeValue(ctx, nw);
        if (JS_IsException(sm))
            goto fail;
        at = JS_NewAtom(ctx, "__new__");
        r = qp_setattr(ctx, type, at, sm);
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, sm);
        if (r < 0)
            goto fail;
    }
    for (i = 0; qp_ss_methods[i].name; i++) {
        JSValue m = qp_descr_new_method(ctx, t, &qp_ss_methods[i]);
        JSAtom at;
        int r;
        if (JS_IsException(m))
            goto fail;
        at = JS_NewAtom(ctx, qp_ss_methods[i].name);
        r = qp_setattr(ctx, type, at, m);
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, m);
        if (r < 0)
            goto fail;
    }
    /* CPython structseq types are immutable static-like types */
    t->flags |= QP_TPFLAGS_IMMUTABLETYPE;
    if (desc->flags & QP_STRUCTSEQ_DISALLOW_INSTANTIATION) {
        /* type_call: "cannot create '%s' instances" (sys.version_info...) */
        t->flags |= QP_TPFLAGS_DISALLOW_INSTANTIATION;
        t->flags &= ~QP_TPFLAGS_BASETYPE;
    }
    goto done;
fail:
    JS_FreeValue(ctx, type);
    type = JS_EXCEPTION;
done:
    JS_FreeValue(ctx, ns);
    return type;
}

static JSValue qp_structseq_new(JSContext *ctx, QPType *t, JSValue *items)
{
    const QPStructSeqDesc *d = qp_ss_desc_of_type(ctx, t);
    int n_fields, n_unnamed, i;
    JSValue res;
    if (!d)
        return qp_raise_system_error(ctx, "not a structseq type");
    qp_ss_counts(d, &n_fields, &n_unnamed);
    res = qp_tup_alloc(ctx, t, d->n_in_sequence);
    if (JS_IsException(res)) {
        for (i = 0; i < n_fields; i++)
            JS_FreeValue(ctx, items[i]);
        return res;
    }
    for (i = 0; i < n_fields; i++) {
        if (qp_ss_set(ctx, res, i, items[i]) < 0) {
            for (i++; i < n_fields; i++)
                JS_FreeValue(ctx, items[i]);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}
