/*
 * PEP 750 t-strings: string.templatelib.Template, Interpolation and the
 * Template iterator, plus the BUILD_TEMPLATE / BUILD_INTERPOLATION
 * constructors used by the interpreter.
 *
 * Derived from CPython 3.15.2 Objects/templateobject.c and
 * Objects/interpolationobject.c (PSF-2.0).
 *
 * All three are PY_OBJECT instances whose fields live in QPInstance slots
 * (exposed as read-only members like CPython's Py_T_OBJECT_EX members).
 */

/* Defined in unicode_format.c. */
static int qp_init_str_formatter(JSContext *ctx);
static Py_ssize_t qpf_str_len_any(JSContext *ctx, JSValueConst s);

/* Called by qp_init_str (unicodeobject.c) once str is ready. */
static int qp_init_templatelib(JSContext *ctx);

/* Template slots */
#define QPT_TMPL_STRINGS        0
#define QPT_TMPL_INTERPOLATIONS 1
#define QPT_TMPL_NSLOTS         2
/* Interpolation slots */
#define QPT_INTERP_VALUE        0
#define QPT_INTERP_EXPRESSION   1
#define QPT_INTERP_CONVERSION   2
#define QPT_INTERP_FORMAT_SPEC  3
#define QPT_INTERP_NSLOTS       4
/* TemplateIter slots */
#define QPT_ITER_STRINGS        0
#define QPT_ITER_INTERPOLATIONS 1
#define QPT_ITER_FROM_STRINGS   2  /* bool */
#define QPT_ITER_NSLOTS         3

static BOOL qpt_template_check_exact(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        qp_type_of(ctx, v) == QP_TYPE(ctx, Template);
}

static BOOL qpt_interpolation_check_exact(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        qp_type_of(ctx, v) == QP_TYPE(ctx, Interpolation);
}

/* __name__ of the type of v as a new str (_PyType_Name). */
static JSValue qpt_type_name(JSContext *ctx, JSValueConst v)
{
    return qp_str_from_atom(ctx, qp_type_of(ctx, v)->name);
}

/* ======================================================================= */
/* Interpolation                                                           */
/* ======================================================================= */

/* _PyInterpolation_Build-like constructor (values borrowed; conversion is
   None or a 1-character str). */
static JSValue qpt_interpolation_make(JSContext *ctx, QPType *type, JSValueConst value,
                                      JSValueConst expression, JSValueConst conversion,
                                      JSValueConst format_spec)
{
    JSValue obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_OBJECT);
    JSValue *slots;

    if (JS_IsException(obj))
        return JS_EXCEPTION;
    slots = qp_instance_slots(obj);
    slots[QPT_INTERP_VALUE] = JS_DupValue(ctx, value);
    slots[QPT_INTERP_EXPRESSION] = JS_DupValue(ctx, expression);
    slots[QPT_INTERP_CONVERSION] = JS_DupValue(ctx, conversion);
    slots[QPT_INTERP_FORMAT_SPEC] = JS_DupValue(ctx, format_spec);
    return obj;
}

/* BUILD_INTERPOLATION (_PyInterpolation_Build): conversion is FVC_*. */
static JSValue qp_interpolation_new(JSContext *ctx, JSValueConst value, JSValueConst expression,
                                    int conversion, JSValueConst format_spec)
{
    JSValue conv, res;

    switch (conversion) {
    case QP_FVC_NONE:
        conv = JS_UNDEFINED;
        break;
    case QP_FVC_ASCII:
        conv = qp_str_from_char(ctx, 'a');
        break;
    case QP_FVC_REPR:
        conv = qp_str_from_char(ctx, 'r');
        break;
    case QP_FVC_STR:
        conv = qp_str_from_char(ctx, 's');
        break;
    default:
        return qp_raise_system_error(ctx, "Interpolation() argument 'conversion' must be "
                                     "one of 's', 'a' or 'r'");
    }
    if (JS_IsException(conv))
        return JS_EXCEPTION;
    res = qpt_interpolation_make(ctx, QP_TYPE(ctx, Interpolation), value, expression, conv,
                                 format_spec);
    JS_FreeValue(ctx, conv);
    return res;
}

/* _PyArg_BadArgument("Interpolation", displayname, "str", arg) */
static JSValue qpt_bad_str_argument(JSContext *ctx, const char *displayname, JSValueConst arg)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    QPType *t = qp_type_of(ctx, arg);
    const char *tp_name;

    if (JS_IsUndefined(arg) || JS_IsNull(arg))
        tp_name = "None";
    else if (t->tp_name)
        tp_name = t->tp_name;
    else
        tp_name = JS_AtomGetStr(ctx, buf, sizeof(buf), t->name);
    return qp_raise_type_error(ctx, "Interpolation() %.200s must be str, not %.50s",
                               displayname, tp_name);
}

/* Interpolation.__new__(value, expression='', conversion=None,
   format_spec='') */
static JSValue qpt_interpolation_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                        size_t nargsf, JSValueConst kwnames)
{
    static const char *const keywords[] = { "value", "expression", "conversion",
                                            "format_spec", NULL };
    static const QPArgSpec spec = { "Interpolation", keywords, 1, 4, 0 };
    JSValueConst out[4];
    JSValue empty, res;
    JSValueConst expression, conversion, format_spec;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, out) < 0)
        return JS_EXCEPTION;
    empty = JS_AtomToString(ctx, JS_ATOM_empty_string);
    expression = empty;
    conversion = JS_UNDEFINED;
    format_spec = empty;
    if (!QP_IS_NULL(out[1])) {
        if (!qp_str_check(ctx, out[1])) {
            JS_FreeValue(ctx, empty);
            return qpt_bad_str_argument(ctx, "argument 'expression'", out[1]);
        }
        expression = out[1];
    }
    if (!QP_IS_NULL(out[2]) && !JS_IsUndefined(out[2]) && !JS_IsNull(out[2])) {
        /* _conversion_converter */
        Py_ssize_t len;
        int ok = 0;

        if (!qp_str_check(ctx, out[2])) {
            JS_FreeValue(ctx, empty);
            return qp_raise_type_error(ctx, "Interpolation() argument 'conversion' must be "
                                       "str, not %T", out[2]);
        }
        len = qpf_str_len_any(ctx, out[2]);
        if (len < 0) {
            JS_FreeValue(ctx, empty);
            return JS_EXCEPTION;
        }
        if (len == 1) {
            JSValue plain = qp_str_value(ctx, out[2]);
            qp_strview view;

            if (JS_IsException(plain)) {
                JS_FreeValue(ctx, empty);
                return JS_EXCEPTION;
            }
            if (qp_str_view_acquire(ctx, plain, &view) < 0) {
                JS_FreeValue(ctx, plain);
                JS_FreeValue(ctx, empty);
                return JS_EXCEPTION;
            }
            switch (QP_SL_READ(view.kind, view.data, 0)) {
            case 'a':
            case 'r':
            case 's':
                ok = 1;
                break;
            default:
                break;
            }
            qp_str_view_release(ctx, &view);
            JS_FreeValue(ctx, plain);
        }
        if (!ok) {
            JS_FreeValue(ctx, empty);
            return qp_raise_value_error(ctx, "Interpolation() argument 'conversion' must be "
                                        "one of 's', 'a' or 'r'");
        }
        conversion = out[2];
    }
    if (!QP_IS_NULL(out[3])) {
        if (!qp_str_check(ctx, out[3])) {
            JS_FreeValue(ctx, empty);
            return qpt_bad_str_argument(ctx, "argument 'format_spec'", out[3]);
        }
        format_spec = out[3];
    }
    res = qpt_interpolation_make(ctx, subtype, out[0], expression, conversion, format_spec);
    JS_FreeValue(ctx, empty);
    return res;
}

static JSValue qpt_interpolation_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSValue name = qpt_type_name(ctx, self), res;

    if (JS_IsException(name))
        return JS_EXCEPTION;
    res = qp_str_from_format(ctx, "%U(%R, %R, %R, %R)", name,
                             slots[QPT_INTERP_VALUE], slots[QPT_INTERP_EXPRESSION],
                             slots[QPT_INTERP_CONVERSION], slots[QPT_INTERP_FORMAT_SPEC]);
    JS_FreeValue(ctx, name);
    return res;
}

/* Interpolation.__reduce__() -> (type, (value, expression, conversion,
   format_spec)) */
static JSValue qpt_interpolation_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    JSValue *slots = qp_instance_slots(self);
    JSValue state, items[2];

    state = qp_tuple_pack(ctx, 4, (JSValueConst *)slots);
    if (JS_IsException(state))
        return JS_EXCEPTION;
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = state;
    return qp_tuple_new_steal(ctx, 2, items);
}

/* __class_getitem__ = Py_GenericAlias (classmethod: self is the type) */
static JSValue qpt_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

static const QPMethodDef qpt_interpolation_methods[] = {
    { "__reduce__", qpt_interpolation_reduce, QP_METH_NOARGS, 0,
      "__reduce__() -> (cls, state)" },
    { "__class_getitem__", qpt_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Interpolations are generic over the types of their values" },
    { NULL, NULL, 0, 0, NULL },
};

static const QPMemberDef qpt_interpolation_members[] = {
    { "value", QPT_INTERP_VALUE, QP_MEMBER_READONLY, "Value" },
    { "expression", QPT_INTERP_EXPRESSION, QP_MEMBER_READONLY, "Expression" },
    { "conversion", QPT_INTERP_CONVERSION, QP_MEMBER_READONLY, "Conversion" },
    { "format_spec", QPT_INTERP_FORMAT_SPEC, QP_MEMBER_READONLY, "Format specifier" },
    { NULL, 0, 0, NULL },
};

static const QPTypeSpec qpt_interpolation_spec = {
    .name = "string.templatelib.Interpolation",
    .doc = "Interpolation object",
    .flags = QP_TPFLAGS_DEFAULT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QPT_INTERP_NSLOTS,
    .slots = {
        .tp_repr = qpt_interpolation_repr,
        .tp_new = qpt_interpolation_tp_new,
    },
    .methods = qpt_interpolation_methods,
    .members = qpt_interpolation_members,
};

/* ======================================================================= */
/* TemplateIter                                                            */
/* ======================================================================= */

static JSValue qpt_templateiter_self(JSContext *ctx, JSValueConst it)
{
    return JS_DupValue(ctx, it);
}

/* templateiter_next(): strings and interpolations alternately, skipping
   empty strings. */
static JSValue qpt_templateiter_next(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSValue item;

    if (JS_ToBool(ctx, slots[QPT_ITER_FROM_STRINGS])) {
        item = qp_next(ctx, slots[QPT_ITER_STRINGS]);
        slots[QPT_ITER_FROM_STRINGS] = JS_FALSE;
        if (JS_IsException(item) || QP_IS_NULL(item))
            return item;
        Py_ssize_t len = qpf_str_len_any(ctx, item);

        if (len < 0) {
            JS_FreeValue(ctx, item);
            return JS_EXCEPTION;
        }
        if (len == 0) {
            JS_FreeValue(ctx, item);
            item = qp_next(ctx, slots[QPT_ITER_INTERPOLATIONS]);
            slots[QPT_ITER_FROM_STRINGS] = JS_TRUE;
        }
    } else {
        item = qp_next(ctx, slots[QPT_ITER_INTERPOLATIONS]);
        slots[QPT_ITER_FROM_STRINGS] = JS_TRUE;
    }
    return item;
}

static const QPTypeSpec qpt_templateiter_spec = {
    .name = "string.templatelib.TemplateIter",
    .doc = "Template iterator object",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QPT_ITER_NSLOTS,
    .slots = {
        .tp_iter = qpt_templateiter_self,
        .tp_iternext = qpt_templateiter_next,
    },
};

/* ======================================================================= */
/* Template                                                                */
/* ======================================================================= */

/* _PyTemplate_Build / BUILD_TEMPLATE: strings and interpolations are
   tuples (borrowed). */
static JSValue qp_template_new(JSContext *ctx, JSValueConst strings, JSValueConst interpolations)
{
    JSValue obj = qp_object_new_instance(ctx, QP_TYPE(ctx, Template), JS_CLASS_PY_OBJECT);
    JSValue *slots;

    if (JS_IsException(obj))
        return JS_EXCEPTION;
    slots = qp_instance_slots(obj);
    slots[QPT_TMPL_STRINGS] = JS_DupValue(ctx, strings);
    slots[QPT_TMPL_INTERPOLATIONS] = JS_DupValue(ctx, interpolations);
    return obj;
}

/* Template.__new__(*args): consecutive strings are concatenated and an
   empty string separates consecutive interpolations. */
static JSValue qpt_template_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t argslen = qp_nargs(nargsf), i;
    Py_ssize_t stringslen = 0, interpolationslen = 0, stringsidx = 0, interpolationsidx = 0;
    int last_was_str = 0;
    JSValue *strs = NULL, *interps = NULL, strings, interpolations, res;

    if (!QP_IS_NULL(kwnames) && QP_TUPLE_SIZE(kwnames) > 0)
        return qp_raise_type_error(ctx, "Template.__new__ only accepts *args arguments");

    for (i = 0; i < argslen; i++) {
        JSValueConst item = args[i];
        if (qp_str_check(ctx, item)) {
            if (!last_was_str)
                stringslen++;
            last_was_str = 1;
        } else if (qpt_interpolation_check_exact(ctx, item)) {
            if (!last_was_str)
                stringslen++;
            interpolationslen++;
            last_was_str = 0;
        } else {
            return qp_raise_type_error(ctx, "Template.__new__ *args need to be of type 'str' "
                                       "or 'Interpolation', got %T", item);
        }
    }
    if (!last_was_str)
        stringslen++;

    strs = js_mallocz(ctx, sizeof(JSValue) * (stringslen + interpolationslen + 1));
    if (!strs)
        return JS_EXCEPTION;
    interps = strs + stringslen;

    last_was_str = 0;
    for (i = 0; i < argslen; i++) {
        JSValueConst item = args[i];
        if (qp_str_check(ctx, item)) {
            if (last_was_str) {
                JSValue concat = qp_str_concat(ctx, strs[stringsidx - 1], item);
                if (JS_IsException(concat))
                    goto fail;
                JS_FreeValue(ctx, strs[stringsidx - 1]);
                strs[stringsidx - 1] = concat;
            } else {
                strs[stringsidx++] = JS_DupValue(ctx, item);
            }
            last_was_str = 1;
        } else {
            if (!last_was_str)
                strs[stringsidx++] = JS_AtomToString(ctx, JS_ATOM_empty_string);
            interps[interpolationsidx++] = JS_DupValue(ctx, item);
            last_was_str = 0;
        }
    }
    if (!last_was_str)
        strs[stringsidx++] = JS_AtomToString(ctx, JS_ATOM_empty_string);

    strings = qp_tuple_new_steal(ctx, stringslen, strs);
    if (JS_IsException(strings)) {
        for (i = 0; i < interpolationsidx; i++)
            JS_FreeValue(ctx, interps[i]);
        js_free(ctx, strs);
        return JS_EXCEPTION;
    }
    interpolations = qp_tuple_new_steal(ctx, interpolationslen, interps);
    js_free(ctx, strs);
    if (JS_IsException(interpolations)) {
        JS_FreeValue(ctx, strings);
        return JS_EXCEPTION;
    }
    res = qp_template_new(ctx, strings, interpolations);
    JS_FreeValue(ctx, strings);
    JS_FreeValue(ctx, interpolations);
    return res;

fail:
    for (i = 0; i < stringsidx; i++)
        JS_FreeValue(ctx, strs[i]);
    for (i = 0; i < interpolationsidx; i++)
        JS_FreeValue(ctx, interps[i]);
    js_free(ctx, strs);
    return JS_EXCEPTION;
}

static JSValue qpt_template_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSValue name = qpt_type_name(ctx, self), res;

    if (JS_IsException(name))
        return JS_EXCEPTION;
    res = qp_str_from_format(ctx, "%U(strings=%R, interpolations=%R)", name,
                             slots[QPT_TMPL_STRINGS], slots[QPT_TMPL_INTERPOLATIONS]);
    JS_FreeValue(ctx, name);
    return res;
}

static JSValue qpt_template_iter(JSContext *ctx, JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self), *islots;
    JSValue stringsiter, interpolationsiter, it;

    stringsiter = qp_iter(ctx, slots[QPT_TMPL_STRINGS]);
    if (JS_IsException(stringsiter))
        return JS_EXCEPTION;
    interpolationsiter = qp_iter(ctx, slots[QPT_TMPL_INTERPOLATIONS]);
    if (JS_IsException(interpolationsiter)) {
        JS_FreeValue(ctx, stringsiter);
        return JS_EXCEPTION;
    }
    it = qp_object_new_instance(ctx, QP_TYPE(ctx, TemplateIter), JS_CLASS_PY_OBJECT);
    if (JS_IsException(it)) {
        JS_FreeValue(ctx, stringsiter);
        JS_FreeValue(ctx, interpolationsiter);
        return JS_EXCEPTION;
    }
    islots = qp_instance_slots(it);
    islots[QPT_ITER_STRINGS] = stringsiter;
    islots[QPT_ITER_INTERPOLATIONS] = interpolationsiter;
    islots[QPT_ITER_FROM_STRINGS] = JS_TRUE;
    return it;
}

/* template_strings_concat(): left[:-1] + (left[-1] + right[0],) +
   right[1:] */
static JSValue qpt_strings_concat(JSContext *ctx, JSValueConst left, JSValueConst right)
{
    Py_ssize_t left_len = QP_TUPLE_SIZE(left), right_len = QP_TUPLE_SIZE(right);
    Py_ssize_t n = left_len + right_len - 1, i, index = 0;
    JSValue *items, concat, res;

    concat = qp_str_concat(ctx, QP_TUPLE_ITEMS(left)[left_len - 1], QP_TUPLE_ITEMS(right)[0]);
    if (JS_IsException(concat))
        return JS_EXCEPTION;
    items = js_malloc(ctx, sizeof(JSValue) * n);
    if (!items) {
        JS_FreeValue(ctx, concat);
        return JS_EXCEPTION;
    }
    for (i = 0; i < left_len - 1; i++)
        items[index++] = JS_DupValue(ctx, QP_TUPLE_ITEMS(left)[i]);
    items[index++] = concat;
    for (i = 1; i < right_len; i++)
        items[index++] = JS_DupValue(ctx, QP_TUPLE_ITEMS(right)[i]);
    res = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    return res;
}

/* _PyTemplate_Concat: sq_concat of Template. */
static JSValue qpt_template_concat(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValue *sslots, *oslots, newstrings, newinterpolations, res;

    if (!qpt_template_check_exact(ctx, self) || !qpt_template_check_exact(ctx, other))
        return qp_raise_type_error(ctx, "can only concatenate string.templatelib.Template "
                                   "(not \"%T\") to string.templatelib.Template", other);
    sslots = qp_instance_slots(self);
    oslots = qp_instance_slots(other);
    newstrings = qpt_strings_concat(ctx, sslots[QPT_TMPL_STRINGS], oslots[QPT_TMPL_STRINGS]);
    if (JS_IsException(newstrings))
        return JS_EXCEPTION;
    newinterpolations = qp_binop(ctx, sslots[QPT_TMPL_INTERPOLATIONS],
                                 oslots[QPT_TMPL_INTERPOLATIONS], QP_NB_ADD);
    if (JS_IsException(newinterpolations)) {
        JS_FreeValue(ctx, newstrings);
        return JS_EXCEPTION;
    }
    res = qp_template_new(ctx, newstrings, newinterpolations);
    JS_FreeValue(ctx, newstrings);
    JS_FreeValue(ctx, newinterpolations);
    return res;
}

/* Template.values: tuple of the interpolation values. */
static JSValue qpt_template_values_get(JSContext *ctx, JSValueConst self, int magic)
{
    JSValueConst interps = qp_instance_slots(self)[QPT_TMPL_INTERPOLATIONS];
    Py_ssize_t len = QP_TUPLE_SIZE(interps), i;
    JSValue values = qp_tuple_new(ctx, len);

    if (JS_IsException(values))
        return JS_EXCEPTION;
    for (i = 0; i < len; i++) {
        JSValue *islots = qp_instance_slots(QP_TUPLE_ITEMS(interps)[i]);
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(values)[i]);
        QP_TUPLE_ITEMS(values)[i] = JS_DupValue(ctx, islots[QPT_INTERP_VALUE]);
    }
    return values;
}

/* Template.__reduce__(): (string.templatelib._template_unpickle,
   (strings, interpolations)) */
static JSValue qpt_template_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    JSValue *slots = qp_instance_slots(self);
    JSValue mod, func, state, items[2];
    JSAtom atom;

    mod = qp_import_module(ctx, "string.templatelib");
    if (JS_IsException(mod))
        return JS_EXCEPTION;
    atom = JS_NewAtom(ctx, "_template_unpickle");
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, mod);
        return JS_EXCEPTION;
    }
    func = qp_getattr(ctx, mod, atom);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(func))
        return JS_EXCEPTION;
    state = qp_tuple_pack(ctx, 2, (JSValueConst *)slots);
    if (JS_IsException(state)) {
        JS_FreeValue(ctx, func);
        return JS_EXCEPTION;
    }
    items[0] = func;
    items[1] = state;
    return qp_tuple_new_steal(ctx, 2, items);
}

static const QPMethodDef qpt_template_methods[] = {
    { "__reduce__", qpt_template_reduce, QP_METH_NOARGS, 0, NULL },
    { "__class_getitem__", qpt_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "Template supports [] for generic usage" },
    { NULL, NULL, 0, 0, NULL },
};

static const QPMemberDef qpt_template_members[] = {
    { "strings", QPT_TMPL_STRINGS, QP_MEMBER_READONLY, "Strings" },
    { "interpolations", QPT_TMPL_INTERPOLATIONS, QP_MEMBER_READONLY, "Interpolations" },
    { NULL, 0, 0, NULL },
};

static const QPGetSetDef qpt_template_getsets[] = {
    { "values", qpt_template_values_get, NULL, 0, "Values of interpolations" },
    { NULL, NULL, NULL, 0, NULL },
};

static const QPTypeSpec qpt_template_spec = {
    .name = "string.templatelib.Template",
    .doc = "Template object",
    .flags = QP_TPFLAGS_DEFAULT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QPT_TMPL_NSLOTS,
    .slots = {
        .tp_repr = qpt_template_repr,
        .tp_iter = qpt_template_iter,
        .tp_new = qpt_template_tp_new,
        .sq = { .sq_concat = qpt_template_concat },
    },
    .methods = qpt_template_methods,
    .getsets = qpt_template_getsets,
    .members = qpt_template_members,
};

/* Ready Template, Interpolation (with __match_args__), TemplateIter and
   the _string iterator types. */
static int qp_init_templatelib(JSContext *ctx)
{
    static const char *const match_args[] = { "value", "expression", "conversion",
                                              "format_spec" };
    JSValue items[4], tuple;
    QPType *interp = QP_TYPE(ctx, Interpolation);
    int i, r;

    if (qp_type_ready(ctx, QP_TYPE(ctx, Template), &qpt_template_spec) < 0 ||
        qp_type_ready(ctx, interp, &qpt_interpolation_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, TemplateIter), &qpt_templateiter_spec) < 0)
        return -1;

    for (i = 0; i < 4; i++) {
        items[i] = qp_str_from_cstr(ctx, match_args[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            return -1;
        }
    }
    tuple = qp_tuple_new_steal(ctx, 4, items);
    if (JS_IsException(tuple))
        return -1;
    r = qp_dict_set_item_atom(ctx, interp->dict, QP_ATOM(ctx, __match_args__), tuple);
    JS_FreeValue(ctx, tuple);
    if (r < 0)
        return -1;
    qp_type_modified(ctx, interp);

    return qp_init_str_formatter(ctx);
}
