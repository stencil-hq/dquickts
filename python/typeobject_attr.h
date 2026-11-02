/*
 * dquickts Python, unit h: instance storage, generic attribute access
 * (PyObject_GenericGetAttr / SetAttr), attribute inline caches,
 * load_method, special method lookup, `super`, __del__ support.
 * Private part of python/typeobject.c (included once from it).
 *
 * Derived from CPython Objects/object.c and Objects/typeobject.c
 * (PSF-2.0).
 *
 * Storage model (D4): attributes of instances whose type has
 * QP_TPFLAGS_MANAGED_DICT live in QuickJS shape properties (string atoms,
 * plain C_W_E values) until __dict__ is materialized; then
 * JSObject.py_dict_mode is set and the dict is the hidden own property
 * QPState.atom_dict_slot (a private symbol).  JS arrays (list subclass
 * instances) are always in dict mode.  Classes whose payload keeps its own
 * dict (functions, modules, classmethod/staticmethod, types) are reached
 * through their own getattro / '__dict__' descriptor instead.
 */

/* Attribute inline cache kinds (QPAttrCache.kind) */
enum {
    QP_ATTR_IC_NONE = 0,
    QP_ATTR_IC_INST_PROP,       /* own shape property, index = prop index */
    QP_ATTR_IC_CLASS_PLAIN,     /* class attribute without __get__: value */
    QP_ATTR_IC_CLASS_DESCR,     /* class descriptor: get / set through its slots */
    QP_ATTR_IC_METHOD,          /* load_method: METHOD_DESCRIPTOR class attribute */
};

/* PY_SUPER payload */
typedef struct QPSuper {
    JSValue type;               /* __thisclass__ (type) or QP_NULL */
    JSValue obj;                /* __self__ or QP_NULL */
    JSValue obj_type;           /* __self_class__ or QP_NULL */
} QPSuper;

/* ========================================================================= */
/* Instances                                                                 */
/* ========================================================================= */

/* Do objects of this class keep their instance attributes in our
   shape/dict storage?  The others keep their own dict (CPython
   tp_dictoffset != 0 without a managed dict). */
static inline BOOL qp_class_shape_storage(int class_id)
{
    switch (class_id) {
    case JS_CLASS_BYTECODE_FUNCTION:
    case JS_CLASS_PY_TYPE:
    case JS_CLASS_PY_MODULE:
    case JS_CLASS_PY_CLASSMETHOD:
    case JS_CLASS_PY_STATICMETHOD:
    case JS_CLASS_PY_FUNC_DATA:
        return FALSE;
    default:
        return TRUE;
    }
}

static inline BOOL qp_shape_storage(JSObject *p)
{
    return qp_class_shape_storage(p->class_id);
}

static inline BOOL qp_obj_dict_mode(JSObject *p)
{
    return p->py_dict_mode || p->class_id == JS_CLASS_ARRAY;
}

static JSValue qp_object_new_instance(JSContext *ctx, QPType *t, int class_id)
{
    JSValue obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), class_id);
    JSObject *p;
    if (JS_IsException(obj))
        return obj;
    p = JS_VALUE_GET_OBJ(obj);
    if (class_id == JS_CLASS_PY_BOXED) {
        p->u.object_data = JS_UNDEFINED;
    } else if (class_id == JS_CLASS_PY_OBJECT && t->nslots > 0) {
        QPInstance *in = js_malloc(ctx, sizeof(QPInstance) + t->nslots * sizeof(JSValue));
        uint32_t i;
        if (!in) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        in->nslots = t->nslots;
        for (i = 0; i < t->nslots; i++)
            in->slots[i] = QP_NULL;
        p->u.opaque = in;
    }
    return obj;
}

static JSValue *qp_instance_slots(JSValueConst obj)
{
    JSObject *p;
    QPInstance *in;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_PY_OBJECT)
        return NULL;
    in = (QPInstance *)p->u.opaque;
    return in ? in->slots : NULL;
}

static void qp_instance_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    QPInstance *in = (QPInstance *)p->u.opaque;
    uint32_t i;
    if (!in)
        return;
    for (i = 0; i < in->nslots; i++)
        JS_FreeValueRT(rt, in->slots[i]);
    js_free_rt(rt, in);
    p->u.opaque = NULL;
}

static void qp_instance_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPInstance *in = (QPInstance *)JS_VALUE_GET_OBJ(val)->u.opaque;
    uint32_t i;
    if (!in)
        return;
    for (i = 0; i < in->nslots; i++)
        JS_MarkValue(rt, in->slots[i], mark_func);
}

static void qp_boxed_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JS_FreeValueRT(rt, p->u.object_data);
    p->u.object_data = JS_UNDEFINED;
}

static void qp_boxed_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, JS_VALUE_GET_OBJ(val)->u.object_data, mark_func);
}

/* Borrowed materialized dict of p, QP_NULL if none. */
static JSValue qp_obj_dict_get(JSContext *ctx, JSObject *p)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    prs = find_own_property(&pr, p, QP_STATE(ctx)->atom_dict_slot);
    if (!prs)
        return QP_NULL;
    return pr->u.value;
}

/* Is a shape property a Python instance attribute? */
static inline BOOL qp_shape_prop_is_attr(JSContext *ctx, JSShapeProperty *prs)
{
    return prs->atom != JS_ATOM_NULL &&
        (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL &&
        JS_AtomGetKind(ctx, prs->atom) == JS_ATOM_KIND_STRING;
}

/* Switch p to dict mode (moving shape attributes into a new dict); returns
   the borrowed dict. */
static JSValue qp_obj_materialize_dict(JSContext *ctx, JSObject *p)
{
    JSValue d = qp_obj_dict_get(ctx, p), key;
    JSShape *sh;
    JSShapeProperty *prs;
    JSAtom *atoms = NULL;
    JSProperty *pr;
    int i, n = 0, cnt;

    if (!QP_IS_NULL(d))
        return d;
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return d;
    if (p->class_id != JS_CLASS_ARRAY) {
        sh = p->shape;
        cnt = sh->prop_count;
        if (cnt > 0) {
            atoms = js_malloc(ctx, cnt * sizeof(JSAtom));
            if (!atoms)
                goto fail;
        }
        for (i = 0, prs = get_shape_prop(sh); i < cnt; i++, prs++) {
            if (!qp_shape_prop_is_attr(ctx, prs))
                continue;
            key = qp_str_from_atom(ctx, prs->atom);
            if (JS_IsException(key))
                goto fail;
            if (qp_dict_set_item(ctx, d, key, p->prop[i].u.value) < 0) {
                JS_FreeValue(ctx, key);
                goto fail;
            }
            JS_FreeValue(ctx, key);
            atoms[n++] = JS_DupAtom(ctx, prs->atom);
        }
        for (i = 0; i < n; i++) {
            delete_property(ctx, p, atoms[i]);
            JS_FreeAtom(ctx, atoms[i]);
        }
        js_free(ctx, atoms);
        atoms = NULL;
    }
    pr = add_property(ctx, p, QP_STATE(ctx)->atom_dict_slot, JS_PROP_WRITABLE);
    if (!pr)
        goto fail;
    pr->u.value = d;
    p->py_dict_mode = 1;
    if (p->class_id == JS_CLASS_PY_NATIVE)
        qp_capi_native_dict_made(ctx, p, d);
    return d;
fail:
    if (atoms) {
        for (i = 0; i < n; i++)
            JS_FreeAtom(ctx, atoms[i]);
        js_free(ctx, atoms);
    }
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

/* Put a new object p (no instance attributes yet) in dict mode with the
   existing dict d (the C extension object's tp_dictoffset dict). */
static int qp_obj_adopt_dict(JSContext *ctx, JSObject *p, JSValueConst d)
{
    JSProperty *pr;
    if (!QP_IS_NULL(qp_obj_dict_get(ctx, p)))
        return 0;
    pr = add_property(ctx, p, QP_STATE(ctx)->atom_dict_slot, JS_PROP_WRITABLE);
    if (!pr)
        return -1;
    pr->u.value = JS_DupValue(ctx, d);
    p->py_dict_mode = 1;
    return 0;
}

/* Does the type of obj give its instances a __dict__ in our storage? */
static inline BOOL qp_obj_has_storage(JSContext *ctx, QPType *tp, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(obj);
    if (!qp_shape_storage(p))
        return FALSE;
    return (tp->flags & QP_TPFLAGS_MANAGED_DICT) || p->py_dict_mode;
}

static JSValue qp_object_get_dict(JSContext *ctx, JSValueConst obj)
{
    QPType *tp = qp_type_of(ctx, obj);
    JSObject *p;
    JSValue d;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && !qp_shape_storage(JS_VALUE_GET_OBJ(obj)))
        return qp_getattr(ctx, obj, QP_ATOM(ctx, __dict__));
    if (!qp_obj_has_storage(ctx, tp, obj))
        return qp_raise_attribute_error(ctx, obj, QP_ATOM(ctx, __dict__));
    p = JS_VALUE_GET_OBJ(obj);
    d = qp_obj_materialize_dict(ctx, p);
    if (JS_IsException(d))
        return d;
    return JS_DupValue(ctx, d);
}

/* get_builtin_base_with_dict + get_dict_descriptor: for an instance whose
   dict is not in our shape storage (type, module, ... subclasses), the
   __dict__ descriptor of the nearest static base defining one.  QP_NULL
   when there is none; new reference. */
static JSValue qp_subtype_builtin_dict_descr(JSContext *ctx, JSValueConst obj)
{
    QPType *t = qp_type_of(ctx, obj);
    JSValue d;
    for (; t && t->base; t = t->base) {
        if (t->flags & QP_TPFLAGS_HEAPTYPE)
            continue;
        if (qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __dict__), &d) < 0)
            return JS_EXCEPTION;
        if (!QP_IS_NULL(d))
            return d;
    }
    return QP_NULL;
}

/* subtype_dict / subtype_setdict / subtype_getweakref */
static JSValue qp_subtype_dict_get(JSContext *ctx, JSValueConst self, int magic)
{
    if (JS_VALUE_GET_TAG(self) == JS_TAG_OBJECT && !qp_shape_storage(JS_VALUE_GET_OBJ(self))) {
        JSValue descr = qp_subtype_builtin_dict_descr(ctx, self), res;
        QPType *dt;
        if (JS_IsException(descr))
            return descr;
        dt = QP_IS_NULL(descr) ? NULL : qp_type_of(ctx, descr);
        if (!dt || !dt->slots.tp_descr_get) {
            JS_FreeValue(ctx, descr);
            return qp_raise_attribute_error(ctx, self, QP_ATOM(ctx, __dict__));
        }
        res = dt->slots.tp_descr_get(ctx, descr, self, QP_TYPE_VALUE(qp_type_of(ctx, self)));
        JS_FreeValue(ctx, descr);
        return res;
    }
    return qp_object_get_dict(ctx, self);
}

static int qp_subtype_dict_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    JSObject *p;
    JSValue d, empty = QP_NULL;
    JSShapeProperty *prs;
    JSProperty *pr;
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT || !qp_shape_storage(JS_VALUE_GET_OBJ(self))) {
        qp_raise_attribute_error(ctx, self, QP_ATOM(ctx, __dict__));
        return -1;
    }
    if (QP_IS_NULL(value)) {
        /* deleting drops the dict; the next access sees a new empty one */
        empty = qp_dict_new(ctx);
        if (JS_IsException(empty))
            return -1;
        value = empty;
    } else if (!qp_dict_check(ctx, value)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, value));
        qp_raise_type_error(ctx, "__dict__ must be set to a dictionary, not a '%.200U'", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    p = JS_VALUE_GET_OBJ(self);
    d = qp_obj_materialize_dict(ctx, p);
    prs = JS_IsException(d) ? NULL :
        find_own_property(&pr, p, QP_STATE(ctx)->atom_dict_slot);
    if (prs)
        set_value(ctx, &pr->u.value, JS_DupValue(ctx, value));
    JS_FreeValue(ctx, empty);
    return prs ? 0 : -1;
}

static JSValue qp_subtype_weakref_get(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_weakref_get_first(ctx, self);
}

static const QPGetSetDef qp_subtype_getsets[2] = {
    { "__dict__", qp_subtype_dict_get, qp_subtype_dict_set, 0,
      "dictionary for instance variables" },
    { "__weakref__", qp_subtype_weakref_get, NULL, 0,
      "list of weak references to the object" },
};

/* ========================================================================= */
/* Generic getattr / setattr                                                 */
/* ========================================================================= */

/* The tp_dictoffset dict of an instance that keeps its dict in its payload
   (function, module, staticmethod, ...: a static base whose spec has
   generic_attr), read through that base's __dict__ descriptor.  New
   reference; QP_NULL when the object has none. */
static JSValue qp_obj_payload_dict(JSContext *ctx, QPType *tp, JSValueConst obj)
{
    QPType *t, *dt;
    JSValue descr, d;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT || qp_shape_storage(JS_VALUE_GET_OBJ(obj)))
        return QP_NULL;
    for (t = tp; t; t = t->base) {
        if (t->spec && t->spec->generic_attr)
            break;
    }
    if (!t)
        return QP_NULL;
    if (qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __dict__), &descr) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(descr))
        return QP_NULL;
    dt = qp_type_of(ctx, descr);
    d = dt->slots.tp_descr_get ? dt->slots.tp_descr_get(ctx, descr, obj, QP_TYPE_VALUE(tp)) :
        QP_NULL;
    JS_FreeValue(ctx, descr);
    if (!JS_IsException(d) && !QP_IS_NULL(d) && !qp_dict_check(ctx, d)) {
        JS_FreeValue(ctx, d);
        return QP_NULL;
    }
    return d;
}

/* _PyObject_GenericGetAttrWithDict(obj, name, NULL, suppress): with
   suppress, a missing attribute returns QP_NULL without exception. */
static JSValue qp_generic_getattr_ex(JSContext *ctx, JSValueConst obj, JSAtom name, int suppress)
{
    QPType *tp = qp_type_of(ctx, obj), *dt = NULL;
    JSValue descr, res;
    QPDescrGetFunc *f = NULL;

    descr = qp_type_lookup(ctx, tp, name);
    if (!QP_IS_NULL(descr)) {
        descr = JS_DupValue(ctx, descr);
        dt = qp_type_of(ctx, descr);
        f = dt->slots.tp_descr_get;
        if (f && dt->slots.tp_descr_set) {
            res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
            JS_FreeValue(ctx, descr);
            goto done;
        }
    }
    if (qp_obj_has_storage(ctx, tp, obj)) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (qp_obj_dict_mode(p)) {
            JSValue d = qp_obj_dict_get(ctx, p);
            if (!QP_IS_NULL(d)) {
                int r;
                d = JS_DupValue(ctx, d);
                r = qp_dict_get_item_atom(ctx, d, name, &res);
                JS_FreeValue(ctx, d);
                if (r != 0) {
                    JS_FreeValue(ctx, descr);
                    return r < 0 ? JS_EXCEPTION : res;
                }
            }
        } else {
            JSShapeProperty *prs;
            JSProperty *pr;
            prs = find_own_property(&pr, p, name);
            if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL) {
                JS_FreeValue(ctx, descr);
                return JS_DupValue(ctx, pr->u.value);
            }
        }
    } else {
        JSValue d = qp_obj_payload_dict(ctx, tp, obj);
        if (JS_IsException(d)) {
            JS_FreeValue(ctx, descr);
            return d;
        }
        if (!QP_IS_NULL(d)) {
            int r = qp_dict_get_item_atom(ctx, d, name, &res);
            JS_FreeValue(ctx, d);
            if (r != 0) {
                JS_FreeValue(ctx, descr);
                return r < 0 ? JS_EXCEPTION : res;
            }
        }
    }
    if (f) {
        res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, descr);
        goto done;
    }
    if (!QP_IS_NULL(descr))
        return descr;
    if (suppress)
        return QP_NULL;
    return qp_raise_attribute_error_ex(ctx, obj, name, TRUE);
done:
    if (suppress && JS_IsException(res) &&
        qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
        qp_tw_clear_exc(ctx);
        return QP_NULL;
    }
    return res;
}

static JSValue qp_generic_getattr(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    return qp_generic_getattr_ex(ctx, obj, name, 0);
}

/* Set (v) or delete (QP_NULL) an attribute in the object's storage.
   Returns 1 done, 0 missing on delete, -1 error. */
static int qp_obj_storage_set(JSContext *ctx, JSObject *p, JSAtom name, JSValueConst v)
{
    if (qp_obj_dict_mode(p)) {
        JSValue d = qp_obj_materialize_dict(ctx, p);
        int r;
        if (JS_IsException(d))
            return -1;
        d = JS_DupValue(ctx, d);
        if (QP_IS_NULL(v)) {
            r = qp_dict_del_item_atom(ctx, d, name);
        } else {
            r = qp_dict_set_item_atom(ctx, d, name, v);
            if (r == 0)
                r = 1;
        }
        JS_FreeValue(ctx, d);
        return r;
    } else {
        JSShapeProperty *prs;
        JSProperty *pr;
        prs = find_own_property(&pr, p, name);
        if (QP_IS_NULL(v)) {
            if (!prs || (prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
                return 0;
            if (delete_property(ctx, p, name) < 0)
                return -1;
            return 1;
        }
        if (prs) {
            if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
                return 0;
            set_value(ctx, &pr->u.value, JS_DupValue(ctx, v));
            return 1;
        }
        pr = add_property(ctx, p, name, JS_PROP_C_W_E);
        if (!pr)
            return -1;
        pr->u.value = JS_DupValue(ctx, v);
        return 1;
    }
}

static int qp_generic_setattr(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPType *tp = qp_type_of(ctx, obj);
    JSValue descr = qp_type_lookup(ctx, tp, name);
    int r;

    if (!QP_IS_NULL(descr)) {
        QPDescrSetFunc *f = qp_type_of(ctx, descr)->slots.tp_descr_set;
        if (f) {
            descr = JS_DupValue(ctx, descr);
            r = f(ctx, descr, obj, v);
            JS_FreeValue(ctx, descr);
            return r;
        }
    }
    if (qp_obj_has_storage(ctx, tp, obj)) {
        r = qp_obj_storage_set(ctx, JS_VALUE_GET_OBJ(obj), name, v);
        if (r < 0)
            return -1;
        if (r == 0) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        return 0;
    } else {
        JSValue d = qp_obj_payload_dict(ctx, tp, obj);
        if (JS_IsException(d))
            return -1;
        if (!QP_IS_NULL(d)) {
            if (QP_IS_NULL(v)) {
                r = qp_dict_del_item_atom(ctx, d, name);
                if (r == 0)
                    qp_raise_attribute_error_ex(ctx, obj, name, TRUE);
            } else {
                r = qp_dict_set_item_atom(ctx, d, name, v);
                if (r == 0)
                    r = 1;
            }
            JS_FreeValue(ctx, d);
            return r > 0 ? 0 : -1;
        }
    }
    {
        JSValue tn = qp_type_tp_name(ctx, tp), n = qp_str_from_atom(ctx, name);
        if (QP_IS_NULL(descr)) {
            if (tp->slots.tp_setattro == qp_generic_setattr)
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "'%.100U' object has no attribute '%U' and no __dict__ for setting new attributes",
                         tn, n);
            else
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "'%.100U' object has no attribute '%U'", tn, n);
        } else {
            qp_raise(ctx, QP_EXC(ctx, AttributeError),
                     "'%.100U' object attribute '%U' is read-only", tn, n);
        }
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, n);
    }
    return -1;
}

/* ========================================================================= */
/* Inline caches                                                             */
/* ========================================================================= */

static void qp_ic_store(JSContext *ctx, QPAttrCache *ic, JSObject *p, uint32_t version,
                        int kind, uint32_t index, JSValueConst value)
{
    JSShape *old = ic->shape;
    ic->shape = js_dup_shape(p->shape);
    if (old)
        js_free_shape(ctx->rt, old);
    ic->type_version = version;
    ic->kind = (uint8_t)kind;
    ic->index = index;
    ic->value = value;
}

/* Type of an IC-eligible receiver (proto is a PY_TYPE, shape storage, not
   dict mode, hashed shape), else NULL.  An unhashed shape (after a property
   deletion) is owned by its object and modified in place by add_property,
   so it can neither be keyed on nor have an extra reference. */
static QPType *qp_ic_receiver(JSValueConst obj)
{
    JSObject *p, *proto;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(obj);
    proto = p->shape->proto;
    if (!proto || proto->class_id != JS_CLASS_PY_TYPE || !proto->u.opaque)
        return NULL;
    if (p->py_dict_mode || !qp_shape_storage(p) || p->class_id == JS_CLASS_ARRAY ||
        !p->shape->is_hashed)
        return NULL;
    return (QPType *)proto->u.opaque;
}

static inline BOOL qp_ic_hit(QPAttrCache *ic, JSValueConst obj, QPType **ptp)
{
    JSObject *p;
    QPType *tp;
    if (!ic->shape || JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->shape != ic->shape)
        return FALSE;
    tp = (QPType *)p->shape->proto->u.opaque;
    if (!tp || tp->version_tag != ic->type_version || ic->type_version == 0)
        return FALSE;
    *ptp = tp;
    return TRUE;
}

/* Classify name for a generic-getattr receiver; returns the kind. */
static int qp_ic_classify_get(JSContext *ctx, QPType *tp, JSObject *p, JSAtom name,
                              int for_method, uint32_t *pindex, JSValue *pvalue)
{
    JSValue descr = qp_type_lookup(ctx, tp, name);
    QPType *dt = QP_IS_NULL(descr) ? NULL : qp_type_of(ctx, descr);
    JSShapeProperty *prs;
    JSProperty *pr;

    *pvalue = descr;
    if (dt && dt->slots.tp_descr_get && dt->slots.tp_descr_set)
        return QP_ATTR_IC_CLASS_DESCR;
    if (tp->flags & QP_TPFLAGS_MANAGED_DICT) {
        prs = find_own_property(&pr, p, name);
        if (prs) {
            if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
                return QP_ATTR_IC_NONE;
            *pindex = (uint32_t)(prs - get_shape_prop(p->shape));
            return QP_ATTR_IC_INST_PROP;
        }
    }
    if (!dt)
        return QP_ATTR_IC_NONE;
    if (for_method && (dt->flags & QP_TPFLAGS_METHOD_DESCRIPTOR))
        return QP_ATTR_IC_METHOD;
    if (dt->slots.tp_descr_get)
        return QP_ATTR_IC_CLASS_DESCR;
    return QP_ATTR_IC_CLASS_PLAIN;
}

static JSValue qp_ic_get_hit(JSContext *ctx, QPAttrCache *ic, JSValueConst obj, QPType *tp)
{
    JSValue v, r;
    switch (ic->kind) {
    case QP_ATTR_IC_INST_PROP:
        return JS_DupValue(ctx, JS_VALUE_GET_OBJ(obj)->prop[ic->index].u.value);
    case QP_ATTR_IC_CLASS_PLAIN:
    case QP_ATTR_IC_METHOD:
        if (ic->kind == QP_ATTR_IC_CLASS_PLAIN)
            return JS_DupValue(ctx, ic->value);
        /* fall through: bind the method */
    case QP_ATTR_IC_CLASS_DESCR:
    default:
        v = JS_DupValue(ctx, ic->value);
        r = qp_type_of(ctx, v)->slots.tp_descr_get(ctx, v, obj, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, v);
        return r;
    }
}

static JSValue qp_getattr_ic(JSContext *ctx, JSValueConst obj, JSAtom name, QPAttrCache *ic)
{
    QPType *tp;
    JSValue value;
    uint32_t index = 0, version;
    int kind;

    if (qp_ic_hit(ic, obj, &tp))
        return qp_ic_get_hit(ctx, ic, obj, tp);
    tp = qp_ic_receiver(obj);
    if (!tp || tp->slots.tp_getattro != qp_generic_getattr)
        return qp_getattr(ctx, obj, name);
    kind = qp_ic_classify_get(ctx, tp, JS_VALUE_GET_OBJ(obj), name, 0, &index, &value);
    version = qp_type_version(ctx, tp);
    if (kind == QP_ATTR_IC_NONE || version == 0)
        return qp_generic_getattr(ctx, obj, name);
    qp_ic_store(ctx, ic, JS_VALUE_GET_OBJ(obj), version, kind, index, value);
    return qp_ic_get_hit(ctx, ic, obj, tp);
}

/* _PyObject_GetMethod */
static int qp_load_method(JSContext *ctx, JSValueConst obj, JSAtom name,
                          JSValue *pmeth, JSValue *pself)
{
    QPType *tp = qp_type_of(ctx, obj), *dt;
    JSValue descr, res;
    QPDescrGetFunc *f = NULL;
    BOOL meth_found = FALSE;

    *pself = QP_NULL;
    *pmeth = QP_NULL;
    if (tp->slots.tp_getattro != qp_generic_getattr) {
        res = qp_getattr(ctx, obj, name);
        if (JS_IsException(res))
            return -1;
        *pmeth = res;
        return 0;
    }
    descr = qp_type_lookup(ctx, tp, name);
    if (!QP_IS_NULL(descr)) {
        descr = JS_DupValue(ctx, descr);
        dt = qp_type_of(ctx, descr);
        if (dt->flags & QP_TPFLAGS_METHOD_DESCRIPTOR) {
            meth_found = TRUE;
        } else {
            f = dt->slots.tp_descr_get;
            if (f && dt->slots.tp_descr_set) {
                res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
                JS_FreeValue(ctx, descr);
                if (JS_IsException(res))
                    return -1;
                *pmeth = res;
                return 0;
            }
        }
    }
    if (qp_obj_has_storage(ctx, tp, obj)) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (qp_obj_dict_mode(p)) {
            JSValue d = qp_obj_dict_get(ctx, p);
            if (!QP_IS_NULL(d)) {
                int r;
                d = JS_DupValue(ctx, d);
                r = qp_dict_get_item_atom(ctx, d, name, &res);
                JS_FreeValue(ctx, d);
                if (r != 0) {
                    JS_FreeValue(ctx, descr);
                    if (r < 0)
                        return -1;
                    *pmeth = res;
                    return 0;
                }
            }
        } else {
            JSShapeProperty *prs;
            JSProperty *pr;
            prs = find_own_property(&pr, p, name);
            if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL) {
                JS_FreeValue(ctx, descr);
                *pmeth = JS_DupValue(ctx, pr->u.value);
                return 0;
            }
        }
    }
    if (meth_found) {
        *pmeth = descr;
        *pself = JS_DupValue(ctx, obj);
        return 0;
    }
    if (f) {
        res = f(ctx, descr, obj, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, descr);
        if (JS_IsException(res))
            return -1;
        *pmeth = res;
        return 0;
    }
    if (!QP_IS_NULL(descr)) {
        *pmeth = descr;
        return 0;
    }
    qp_raise_attribute_error(ctx, obj, name);
    return -1;
}

static int qp_load_method_ic(JSContext *ctx, JSValueConst obj, JSAtom name, QPAttrCache *ic,
                             JSValue *pmeth, JSValue *pself)
{
    QPType *tp;
    JSValue value, r;
    uint32_t index = 0, version;
    int kind;

    if (!qp_ic_hit(ic, obj, &tp)) {
        tp = qp_ic_receiver(obj);
        if (!tp || tp->slots.tp_getattro != qp_generic_getattr)
            return qp_load_method(ctx, obj, name, pmeth, pself);
        kind = qp_ic_classify_get(ctx, tp, JS_VALUE_GET_OBJ(obj), name, 1, &index, &value);
        version = qp_type_version(ctx, tp);
        if (kind == QP_ATTR_IC_NONE || version == 0)
            return qp_load_method(ctx, obj, name, pmeth, pself);
        qp_ic_store(ctx, ic, JS_VALUE_GET_OBJ(obj), version, kind, index, value);
    }
    if (ic->kind == QP_ATTR_IC_METHOD) {
        *pmeth = JS_DupValue(ctx, ic->value);
        *pself = JS_DupValue(ctx, obj);
        return 0;
    }
    *pself = QP_NULL;
    r = qp_ic_get_hit(ctx, ic, obj, tp);
    if (JS_IsException(r)) {
        *pmeth = QP_NULL;
        return -1;
    }
    *pmeth = r;
    return 0;
}

static int qp_setattr_ic(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v,
                         QPAttrCache *ic)
{
    QPType *tp;
    JSValue descr;
    QPType *dt;
    uint32_t version;

    if (qp_ic_hit(ic, obj, &tp)) {
        if (ic->kind == QP_ATTR_IC_INST_PROP) {
            set_value(ctx, &JS_VALUE_GET_OBJ(obj)->prop[ic->index].u.value, JS_DupValue(ctx, v));
            return 0;
        } else {
            JSValue d = JS_DupValue(ctx, ic->value);
            int r = qp_type_of(ctx, d)->slots.tp_descr_set(ctx, d, obj, v);
            JS_FreeValue(ctx, d);
            return r;
        }
    }
    tp = qp_ic_receiver(obj);
    if (!tp || tp->slots.tp_setattro != qp_generic_setattr)
        return qp_setattr(ctx, obj, name, v);
    version = qp_type_version(ctx, tp);
    if (version) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        descr = qp_type_lookup(ctx, tp, name);
        dt = QP_IS_NULL(descr) ? NULL : qp_type_of(ctx, descr);
        if (dt && dt->slots.tp_descr_set) {
            qp_ic_store(ctx, ic, p, version, QP_ATTR_IC_CLASS_DESCR, 0, descr);
        } else if (tp->flags & QP_TPFLAGS_MANAGED_DICT) {
            JSShapeProperty *prs;
            JSProperty *pr;
            prs = find_own_property(&pr, p, name);
            if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL)
                qp_ic_store(ctx, ic, p, version, QP_ATTR_IC_INST_PROP,
                            (uint32_t)(prs - get_shape_prop(p->shape)), JS_UNDEFINED);
        }
    }
    return qp_generic_setattr(ctx, obj, name, v);
}

/* ========================================================================= */
/* Special method lookup                                                     */
/* ========================================================================= */

static JSValue qp_lookup_special(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPType *tp = qp_type_of(ctx, obj);
    JSValue res = qp_type_lookup(ctx, tp, name), r;
    QPDescrGetFunc *f;
    if (QP_IS_NULL(res))
        return QP_NULL;
    res = JS_DupValue(ctx, res);
    f = qp_type_of(ctx, res)->slots.tp_descr_get;
    if (f) {
        r = f(ctx, res, obj, QP_TYPE_VALUE(tp));
        JS_FreeValue(ctx, res);
        return r;
    }
    return res;
}

static int qp_load_special(JSContext *ctx, JSValueConst obj, int which,
                           JSValue *pmeth, JSValue *pself)
{
    static const int names[4] = { QP_ATOM___enter__, QP_ATOM___exit__,
                                  QP_ATOM___aenter__, QP_ATOM___aexit__ };
    static const char *const errs[4] = {
        "'%T' object does not support the context manager protocol (missed __enter__ method)",
        "'%T' object does not support the context manager protocol (missed __exit__ method)",
        "'%T' object does not support the asynchronous context manager protocol (missed __aenter__ method)",
        "'%T' object does not support the asynchronous context manager protocol (missed __aexit__ method)",
    };
    static const char *const sugg[4] = {
        "'%T' object does not support the context manager protocol (missed __enter__ method) "
        "but it supports the asynchronous context manager protocol. Did you mean to use 'async with'?",
        "'%T' object does not support the context manager protocol (missed __exit__ method) "
        "but it supports the asynchronous context manager protocol. Did you mean to use 'async with'?",
        "'%T' object does not support the asynchronous context manager protocol (missed __aenter__ method) "
        "but it supports the context manager protocol. Did you mean to use 'with'?",
        "'%T' object does not support the asynchronous context manager protocol (missed __aexit__ method) "
        "but it supports the context manager protocol. Did you mean to use 'with'?",
    };
    QPState *s = QP_STATE(ctx);
    JSValue func;
    int r;

    *pmeth = QP_NULL;
    *pself = QP_NULL;
    which &= 3;
    r = qp_lookup_method_ex(ctx, obj, s->atoms[names[which]], &func, 0);
    if (r == -1)
        return -1;
    if (r == -2) {
        QPType *tp = qp_type_of(ctx, obj);
        BOOL can;
        if (which < 2)
            can = !QP_IS_NULL(qp_type_lookup(ctx, tp, QP_ATOM(ctx, __aenter__))) &&
                  !QP_IS_NULL(qp_type_lookup(ctx, tp, QP_ATOM(ctx, __aexit__)));
        else
            can = !QP_IS_NULL(qp_type_lookup(ctx, tp, QP_ATOM(ctx, __enter__))) &&
                  !QP_IS_NULL(qp_type_lookup(ctx, tp, QP_ATOM(ctx, __exit__)));
        qp_raise_type_error(ctx, can ? sugg[which] : errs[which], obj);
        return -1;
    }
    *pmeth = func;
    if (r == 1)
        *pself = JS_DupValue(ctx, obj);
    return 0;
}

/* ========================================================================= */
/* super                                                                     */
/* ========================================================================= */

static QPSuper *qp_super_data(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_SUPER)
        return NULL;
    return (QPSuper *)p->u.opaque;
}

static void qp_super_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    QPSuper *su = (QPSuper *)p->u.opaque;
    if (!su)
        return;
    JS_FreeValueRT(rt, su->type);
    JS_FreeValueRT(rt, su->obj);
    JS_FreeValueRT(rt, su->obj_type);
    js_free_rt(rt, su);
    p->u.opaque = NULL;
}

static void qp_super_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPSuper *su = (QPSuper *)JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!su)
        return;
    JS_MarkValue(rt, su->type, mark_func);
    JS_MarkValue(rt, su->obj, mark_func);
    JS_MarkValue(rt, su->obj_type, mark_func);
}

static JSValue qp_super_alloc(JSContext *ctx, QPType *t)
{
    JSValue obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_SUPER);
    QPSuper *su;
    if (JS_IsException(obj))
        return obj;
    su = js_malloc(ctx, sizeof(*su));
    if (!su) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    su->type = su->obj = su->obj_type = QP_NULL;
    JS_VALUE_GET_OBJ(obj)->u.opaque = su;
    return obj;
}

/* supercheck: new reference to the type to start the MRO walk from. */
static JSValue qp_supercheck(JSContext *ctx, QPType *type, JSValueConst obj)
{
    QPType *ot = qp_type_from_value(obj);
    JSValue class_attr, tn, on;
    int r;
    const char *kind;

    if (ot && qp_type_is_subtype(ot, type))
        return JS_DupValue(ctx, obj);
    if (qp_type_is_subtype(qp_type_of(ctx, obj), type))
        return qp_type_dup(ctx, qp_type_of(ctx, obj));
    r = qp_getattr_opt(ctx, obj, QP_ATOM(ctx, __class__), &class_attr);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        QPType *ct = qp_type_from_value(class_attr);
        if (ct && ct != qp_type_of(ctx, obj) && qp_type_is_subtype(ct, type))
            return class_attr;
        JS_FreeValue(ctx, class_attr);
    }
    if (ot) {
        kind = "type";
        on = qp_type_tp_name(ctx, ot);
    } else {
        kind = "instance of";
        on = qp_type_tp_name(ctx, qp_type_of(ctx, obj));
    }
    tn = qp_type_tp_name(ctx, type);
    qp_raise_type_error(ctx, "super(type, obj): obj (%s %.200U) is not an instance or "
                        "subtype of type (%.200U).", kind, on, tn);
    JS_FreeValue(ctx, on);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* _PySuper_LookupDescr: new ref or QP_NULL (never raises). */
static JSValue qp_super_lookup_descr(JSContext *ctx, QPType *su_type, QPType *su_obj_type,
                                     JSAtom name)
{
    JSValue mro = su_obj_type->mro, v;
    Py_ssize_t i, n;
    if (JS_VALUE_GET_TAG(mro) != JS_TAG_OBJECT)
        return QP_NULL;
    n = QP_TUPLE_SIZE(mro);
    for (i = 0; i + 1 < n; i++) {
        if (qp_type_from_value(QP_TUPLE_ITEMS(mro)[i]) == su_type)
            break;
    }
    i++;
    for (; i < n; i++) {
        QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(mro)[i]);
        int r;
        if (!b || JS_VALUE_GET_TAG(b->dict) != JS_TAG_OBJECT)
            continue;
        r = qp_dict_get_item_atom(ctx, b->dict, name, &v);
        if (r > 0)
            return v;
        if (r < 0) {
            qp_tw_clear_exc(ctx);
            return QP_NULL;
        }
    }
    return QP_NULL;
}

static JSValue qp_super_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames);

/* do_super_lookup; method != NULL: *method = 1 when an unbound method
   descriptor is returned. */
static JSValue qp_do_super_lookup(JSContext *ctx, JSValueConst su, QPType *su_type,
                                  JSValueConst su_obj, QPType *su_obj_type, JSAtom name,
                                  int *method)
{
    JSValue res, res2, tmp = QP_NULL;
    if (su_obj_type) {
        res = qp_super_lookup_descr(ctx, su_type, su_obj_type, name);
        if (!QP_IS_NULL(res)) {
            QPType *rt = qp_type_of(ctx, res);
            if (method && (rt->flags & QP_TPFLAGS_METHOD_DESCRIPTOR)) {
                *method = 1;
            } else if (rt->slots.tp_descr_get) {
                JSValueConst o = (JS_VALUE_GET_TAG(su_obj) == JS_TAG_OBJECT &&
                                  qp_type_from_value(su_obj) == su_obj_type) ? QP_NULL : su_obj;
                res2 = rt->slots.tp_descr_get(ctx, res, o, QP_TYPE_VALUE(su_obj_type));
                JS_FreeValue(ctx, res);
                res = res2;
            }
            return res;
        }
    }
    if (QP_IS_NULL(su)) {
        JSValueConst a[2] = { QP_TYPE_VALUE(su_type), su_obj };
        tmp = qp_super_vectorcall(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, super)), a, 2, QP_NULL);
        if (JS_IsException(tmp))
            return tmp;
        su = tmp;
    }
    res = qp_generic_getattr(ctx, su, name);
    JS_FreeValue(ctx, tmp);
    return res;
}

static JSValue qp_super_getattro(JSContext *ctx, JSValueConst self, JSAtom name)
{
    QPSuper *su = qp_super_data(self);
    if (name == QP_ATOM(ctx, __class__) || !su)
        return qp_generic_getattr(ctx, self, name);
    return qp_do_super_lookup(ctx, self, qp_type_from_value(su->type), su->obj,
                              QP_IS_NULL(su->obj_type) ? NULL : qp_type_from_value(su->obj_type),
                              name, NULL);
}

static JSValue qp_super_repr(JSContext *ctx, JSValueConst self)
{
    QPSuper *su = qp_super_data(self);
    QPType *t = su && !QP_IS_NULL(su->type) ? qp_type_from_value(su->type) : NULL;
    JSValue tn = t ? qp_type_tp_name(ctx, t) : qp_str_from_cstr(ctx, "NULL"), on, r;
    if (su && !QP_IS_NULL(su->obj_type)) {
        on = qp_type_tp_name(ctx, qp_type_from_value(su->obj_type));
        r = qp_str_from_format(ctx, "<super: <class '%U'>, <%U object>>", tn, on);
        JS_FreeValue(ctx, on);
    } else {
        r = qp_str_from_format(ctx, "<super: <class '%U'>, NULL>", tn);
    }
    JS_FreeValue(ctx, tn);
    return r;
}

/* super_init_impl: type QP_NULL = zero-argument form. */
static int qp_super_init_impl(JSContext *ctx, JSValueConst self, JSValueConst type,
                              JSValueConst obj)
{
    QPSuper *su = qp_super_data(self);
    JSValue t, o, ot = QP_NULL;
    if (!su)
        return 0;
    if (QP_IS_NULL(type)) {
        struct JSStackFrame *sf = qp_py_frame_at(ctx, 0);
        if (!sf) {
            qp_raise_runtime_error(ctx, "super(): no current frame");
            return -1;
        }
        if (qp_frame_super_args(ctx, sf, &t, &o) < 0)
            return -1;
    } else {
        t = JS_DupValue(ctx, type);
        o = QP_IS_NULL(obj) ? QP_NULL : JS_DupValue(ctx, obj);
    }
    if (JS_IsUndefined(o) || JS_IsNull(o)) {
        JS_FreeValue(ctx, o);
        o = QP_NULL;
    }
    if (!QP_IS_NULL(o)) {
        ot = qp_supercheck(ctx, qp_type_from_value(t), o);
        if (JS_IsException(ot)) {
            JS_FreeValue(ctx, t);
            JS_FreeValue(ctx, o);
            return -1;
        }
    }
    JS_FreeValue(ctx, su->type);
    JS_FreeValue(ctx, su->obj);
    JS_FreeValue(ctx, su->obj_type);
    su->type = t;
    su->obj = o;
    su->obj_type = ot;
    return 0;
}

static int qp_super_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    if (qp_tw_no_kw(ctx, "super", kwnames) < 0)
        return -1;
    if (nargs > 2) {
        qp_raise_type_error(ctx, "super() takes at most 2 arguments (%d given)", nargs);
        return -1;
    }
    if (nargs >= 1 && !qp_type_from_value(args[0])) {
        qp_raise_type_error(ctx, "super() argument 1 must be type, not %T", args[0]);
        return -1;
    }
    return qp_super_init_impl(ctx, self, nargs >= 1 ? args[0] : QP_NULL,
                              nargs >= 2 ? args[1] : QP_NULL);
}

static JSValue qp_super_new(JSContext *ctx, QPType *t, JSValueConst *args, size_t nargsf,
                            JSValueConst kwnames)
{
    return qp_super_alloc(ctx, t);
}

static JSValue qp_super_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    QPType *t = qp_type_from_value(callable);
    JSValue su;
    if (qp_tw_no_kw(ctx, "super", kwnames) < 0)
        return JS_EXCEPTION;
    if (nargs > 2)
        return qp_raise_type_error(ctx, "super() expected at most 2 arguments, got %d", nargs);
    if (nargs >= 1 && !qp_type_from_value(args[0])) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, args[0]));
        qp_raise_type_error(ctx, "super() argument 1 must be a type, not %.200U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    su = qp_super_alloc(ctx, t);
    if (JS_IsException(su))
        return su;
    if (qp_super_init_impl(ctx, su, nargs >= 1 ? args[0] : QP_NULL,
                           nargs >= 2 ? args[1] : QP_NULL) < 0) {
        JS_FreeValue(ctx, su);
        return JS_EXCEPTION;
    }
    return su;
}

static JSValue qp_super_descr_get(JSContext *ctx, JSValueConst self, JSValueConst obj,
                                  JSValueConst type)
{
    QPSuper *su = qp_super_data(self), *nsu;
    JSValue ot, n;
    if (!su || QP_IS_NULL(obj) || JS_IsUndefined(obj) || !QP_IS_NULL(su->obj))
        return JS_DupValue(ctx, self);
    if (qp_type_of(ctx, self) != QP_TYPE(ctx, super))
        return qp_call2(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)), su->type, obj);
    ot = qp_supercheck(ctx, qp_type_from_value(su->type), obj);
    if (JS_IsException(ot))
        return ot;
    n = qp_super_alloc(ctx, QP_TYPE(ctx, super));
    if (JS_IsException(n)) {
        JS_FreeValue(ctx, ot);
        return n;
    }
    nsu = qp_super_data(n);
    nsu->type = JS_DupValue(ctx, su->type);
    nsu->obj = JS_DupValue(ctx, obj);
    nsu->obj_type = ot;
    return n;
}

static JSValue qp_super_get_member(JSContext *ctx, JSValueConst self, int magic)
{
    QPSuper *su = qp_super_data(self);
    JSValue v;
    if (!su)
        return JS_UNDEFINED;
    v = magic == 0 ? su->type : magic == 1 ? su->obj : su->obj_type;
    return QP_IS_NULL(v) ? JS_UNDEFINED : JS_DupValue(ctx, v);
}

static const QPGetSetDef qp_super_getsets[] = {
    { "__thisclass__", qp_super_get_member, NULL, 0, "the class invoking super()" },
    { "__self__", qp_super_get_member, NULL, 1, "the instance invoking super(); may be None" },
    { "__self_class__", qp_super_get_member, NULL, 2,
      "the type of the instance invoking super(); may be None" },
    { NULL }
};

static const QPTypeSpec qp_super_spec = {
    .name = "super",
    .doc = "super() -> same as super(__class__, <first argument>)\n"
           "super(type) -> unbound super object\n"
           "super(type, obj) -> bound super object; requires isinstance(obj, type)\n"
           "super(type, type2) -> bound super object; requires\n"
           "    issubclass(type2, type)\n"
           "Typical use to call a cooperative superclass method:\n"
           "class C(B):\n"
           "    def meth(self, arg):\n"
           "        super().meth(arg)\n"
           "This works for class methods too:\n"
           "class C(B):\n"
           "    @classmethod\n"
           "    def cmeth(cls, arg):\n"
           "        super().cmeth(arg)\n",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_SUPER,
    .slots = {
        .tp_getattro = qp_super_getattro,
        .tp_repr = qp_super_repr,
        .tp_descr_get = qp_super_descr_get,
        .tp_init = qp_super_init,
        .tp_new = qp_super_new,
        .tp_vectorcall = qp_super_vectorcall,
    },
    .getsets = qp_super_getsets,
};

/* load_super_attr / load_super_method */
static int qp_super_lookup(JSContext *ctx, JSValueConst global_super, JSValueConst cls,
                           JSValueConst self, JSAtom name, int flags,
                           JSValue *pres, JSValue *pself)
{
    QPType *ct = qp_type_from_value(cls);
    JSValue res;
    *pres = QP_NULL;
    if (pself)
        *pself = QP_NULL;
    if (qp_type_from_value(global_super) == QP_TYPE(ctx, super) && ct) {
        int method = 0;
        QPType *st;
        JSValue ot = qp_supercheck(ctx, ct, self);
        if (JS_IsException(ot))
            return -1;
        /* like LOAD_SUPER_ATTR_METHOD: the unbound-method form only when
           type(self) uses the generic getattr (not for class objects:
           super().__new__(cls) gets the function unbound) */
        st = qp_type_of(ctx, self);
        res = qp_do_super_lookup(ctx, QP_NULL, ct, self, qp_type_from_value(ot), name,
                                 (pself && st->slots.tp_getattro == qp_generic_getattr) ?
                                 &method : NULL);
        JS_FreeValue(ctx, ot);
        if (JS_IsException(res))
            return -1;
        *pres = res;
        if (method)
            *pself = JS_DupValue(ctx, self);
        return 0;
    } else {
        JSValueConst a[2] = { cls, self };
        JSValue su = qp_vectorcall(ctx, global_super, a, (flags & QP_SUPER_TWO_ARGS) ? 2 : 0,
                                   QP_NULL);
        if (JS_IsException(su))
            return -1;
        res = qp_getattr(ctx, su, name);
        JS_FreeValue(ctx, su);
        if (JS_IsException(res))
            return -1;
        *pres = res;
        return 0;
    }
}

/* ========================================================================= */
/* __del__                                                                   */
/* ========================================================================= */

/* the context of the Python object 'p' if its __del__ (or finalizer) is
   due: its type has one, it has not run and the finalizer queue exists */
static JSContext *qp_del_context(JSObject *p)
{
    JSObject *proto;
    QPType *t;
    JSContext *ctx;
    QPState *s;

    if (!QP_IS_PY_CLASS(p->class_id) || p->py_finalized)
        return NULL;
    proto = p->shape ? p->shape->proto : NULL;
    if (!proto || proto->class_id != JS_CLASS_PY_TYPE || !proto->u.opaque)
        return NULL;
    t = (QPType *)proto->u.opaque;
    if (!(t->flags & QP_TPFLAGS_HAS_DEL))
        return NULL;
    ctx = t->ctx;
    s = ctx ? QP_STATE(ctx) : NULL;
    if (!s || JS_VALUE_GET_TAG(s->pending_del) != JS_TAG_OBJECT)
        return NULL;
    return ctx;
}

/* PEP 442 for the cyclic garbage (JS_RunGC): queue the unreachable object
   'p' in QPState.pending_del (a new reference) if its __del__ is due.
   Returns TRUE if it was queued. */
static BOOL qp_del_queue_cyclic(JSRuntime *rt, JSObject *p)
{
    JSContext *ctx = qp_del_context(p);

    if (!ctx)
        return FALSE;
    if (qp_list_append(ctx, QP_STATE(ctx)->pending_del, JS_MKPTR(JS_TAG_OBJECT, p)) < 0) {
        /* out of memory: freed without its __del__ */
        qp_tw_clear_exc(ctx);
        return FALSE;
    }
    p->py_finalized = 1;
    return TRUE;
}

static BOOL qp_del_hook(JSRuntime *rt, JSObject *p)
{
    JSContext *ctx = qp_del_context(p);
    QPState *s;
    JSValue v;

    if (!ctx)
        return FALSE;
    s = QP_STATE(ctx);
    p->py_finalized = 1;
    /* resurrect: back to the live object list, owned by pending_del */
    list_del(&p->header.link);
    list_add_tail(&p->header.link, &rt->gc_obj_list);
    js_rc(p)->mark = 0;
    js_rc(p)->ref_count = 1;
    v = JS_MKPTR(JS_TAG_OBJECT, p);
    if (qp_list_append(ctx, s->pending_del, v) < 0) {
        /* out of memory: the object stays alive (leaked) rather than being
           freed with a pending __del__ */
        qp_tw_clear_exc(ctx);
        return TRUE;
    }
    js_rc(p)->ref_count--;
    /* run __del__ at the next poll (qp_signal_check), i.e. before the next
       call or loop iteration, like CPython's immediate finalization */
    ctx->interrupt_counter = 1;
    return TRUE;
}

static void qp_run_pending_finalizers(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    JSValue saved = QP_NULL;
    BOOL had_exc;

    if (!s || JS_VALUE_GET_TAG(s->pending_del) != JS_TAG_OBJECT ||
        qp_list_size(s->pending_del) == 0)
        return;
    had_exc = JS_HasException(ctx);
    if (had_exc)
        saved = JS_GetException(ctx);
    while (qp_list_size(s->pending_del) > 0) {
        JSValue list = s->pending_del, obj;
        Py_ssize_t i, n;
        JSValue *items;
        s->pending_del = JS_NewArray(ctx);
        if (JS_IsException(s->pending_del)) {
            s->pending_del = list;
            qp_tw_clear_exc(ctx);
            break;
        }
        items = qp_list_items(list, &n);
        for (i = 0; i < n; i++) {
            JSObject *p;
            obj = JS_DupValue(ctx, items[i]);
            p = JS_VALUE_GET_OBJ(obj);
            if (p->class_id == JS_CLASS_PY_GENERATOR || p->class_id == JS_CLASS_PY_COROUTINE ||
                p->class_id == JS_CLASS_PY_ASYNC_GEN || p->class_id == JS_CLASS_PY_GEN_AUX) {
                qp_gen_finalize(ctx, obj);
            } else {
                JSValue del, r;
                int u = qp_lookup_method_ex(ctx, obj, QP_ATOM(ctx, __del__), &del, 0);
                if (u >= 0) {
                    JSValueConst a[1] = { obj };
                    r = qp_call_unbound(ctx, u, del, a, 1);
                    if (JS_IsException(r)) {
                        JSValue exc = qp_exc_fetch(ctx);
                        qp_exc_write_unraisable(ctx, exc, del);
                        JS_FreeValue(ctx, exc);
                    } else {
                        JS_FreeValue(ctx, r);
                    }
                    JS_FreeValue(ctx, del);
                } else if (u == -1) {
                    JSValue exc = qp_exc_fetch(ctx);
                    qp_exc_write_unraisable(ctx, exc, obj);
                    JS_FreeValue(ctx, exc);
                }
            }
            JS_FreeValue(ctx, obj);
        }
        JS_FreeValue(ctx, list);
    }
    if (had_exc)
        JS_Throw(ctx, saved);
}
