/*
 * dquickts Python, unit h: type objects, slots, `object`, `super`, generic
 * attribute access, method cache.
 *
 * Derived from CPython Objects/typeobject.c and Objects/object.c
 * (PyObject_GenericGetAttr/SetAttr, NoneType, NotImplementedType)
 * (PSF-2.0).
 *
 * Layout:
 *   typeobject.c        core: type_of, allocation, version tags, method
 *                       cache, MRO, type readying, type_new, `type`,
 *                       `object`, None/NotImplemented, init
 *   typeobject_slots.h  slotdefs, slot wrappers, slot_* dispatchers,
 *                       update_one_slot
 *   typeobject_attr.h   instance attribute storage, generic getattr /
 *                       setattr, inline caches, load_method, special
 *                       lookup, `super`, __del__ support
 */

#include "typeobject_internal.h"

#define QP_TEXT(t) ((QPTypeExt *)(t))

/* ---- forward declarations (definitions below / in the private headers) */
static int qp_find_name_in_mro(JSContext *ctx, QPType *t, JSAtom name, JSValue *pres);
static int qp_fixup_slot_dispatchers(JSContext *ctx, QPType *t);
static int qp_update_slot(JSContext *ctx, QPType *t, JSAtom name);
static int qp_add_operators(JSContext *ctx, QPType *t);
static void qp_slotdefs_init(void);
static int qp_is_slot_name(JSContext *ctx, JSAtom name);
static JSValue qp_vectorcall_method(JSContext *ctx, JSAtom name, JSValueConst *args,
                                    int nargs);
static int qp_lookup_method_ex(JSContext *ctx, JSValueConst self, JSAtom attr,
                               JSValue *out, int raise);
static JSValue qp_type_getattro(JSContext *ctx, JSValueConst obj, JSAtom name);
static int qp_type_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v);
static void qp_obj_set_attribute_error_context(JSContext *ctx, JSValueConst v, JSAtom name);
static JSValue qp_type_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                            size_t nargsf, JSValueConst kwnames);
static JSValue qp_type_tp_new(JSContext *ctx, QPType *meta, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames);
static JSValue qp_generic_getattr_ex(JSContext *ctx, JSValueConst obj, JSAtom name,
                                     int suppress);
static Py_hash_t qp_object_hash(JSContext *ctx, JSValueConst v);
static JSValue qp_object_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op);
static JSValue qp_object_repr(JSContext *ctx, JSValueConst v);
static JSValue qp_object_str(JSContext *ctx, JSValueConst v);
static JSValue qp_slot_tp_new(JSContext *ctx, QPType *t, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames);
static const QPMethodDef qp_tp_new_methoddef;
static const QPGetSetDef qp_subtype_getsets[2];

/* ========================================================================= */
/* Small helpers                                                             */
/* ========================================================================= */

static inline JSValue qp_type_dup(JSContext *ctx, QPType *t)
{
    return JS_DupValue(ctx, QP_TYPE_VALUE(t));
}

static inline Py_ssize_t qp_kwcount(JSValueConst kwnames)
{
    if (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames))
        return 0;
    return QP_TUPLE_SIZE(kwnames);
}

static inline int qp_tw_is_null_or_none(JSValueConst v)
{
    return QP_IS_NULL(v) || JS_IsUndefined(v) || JS_IsNull(v);
}

static void qp_tw_clear_exc(JSContext *ctx);

/* Is exact str s equal to the ASCII C string c? */
static int qp_tw_str_is(JSContext *ctx, JSValueConst s, const char *c)
{
    const char *x = JS_ToCString(ctx, s);
    int r = x && strcmp(x, c) == 0;
    if (x)
        JS_FreeCString(ctx, x);
    else
        qp_tw_clear_exc(ctx);
    return r;
}

/* Drop the pending exception. */
static void qp_tw_clear_exc(JSContext *ctx)
{
    JS_FreeValue(ctx, JS_GetException(ctx));
}

/* Address used by "%p" in default reprs (object identity). */
static const void *qp_obj_addr(JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v))
        return JS_VALUE_GET_PTR(v);
    return (const void *)(uintptr_t)(JS_VALUE_GET_TAG(v) * 0x1000 + 0x100);
}

/* PyArg_UnpackTuple-style count check: "%s expected %s%zd argument%s, got %zd". */
static int qp_tw_unpack_check(JSContext *ctx, const char *name, int nargs, int min, int max)
{
    if (nargs < min) {
        qp_raise_type_error(ctx, "%s expected %s%d argument%s, got %d", name,
                            min == max ? "" : "at least ", min, min == 1 ? "" : "s", nargs);
        return -1;
    }
    if (nargs > max) {
        qp_raise_type_error(ctx, "%s expected %s%d argument%s, got %d", name,
                            min == max ? "" : "at most ", max, max == 1 ? "" : "s", nargs);
        return -1;
    }
    return 0;
}

/* "%s() takes no keyword arguments" check for kwnames. */
static int qp_tw_no_kw(JSContext *ctx, const char *fname, JSValueConst kwnames)
{
    if (qp_kwcount(kwnames) > 0) {
        qp_raise_type_error(ctx, "%s() takes no keyword arguments", fname);
        return -1;
    }
    return 0;
}

/* Atom of an attribute name object ("attribute name must be string, not
   '%.200s'").  JS_ATOM_NULL on error. */
static JSAtom qp_tw_name_to_atom(JSContext *ctx, JSValueConst name)
{
    JSValue s;
    JSAtom a;
    if (!qp_str_check(ctx, name)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, name));
        qp_raise_type_error(ctx, "attribute name must be string, not '%.200U'", tn);
        JS_FreeValue(ctx, tn);
        return JS_ATOM_NULL;
    }
    s = qp_str_value(ctx, name);
    if (JS_IsException(s))
        return JS_ATOM_NULL;
    a = qp_str_to_atom(ctx, s);
    JS_FreeValue(ctx, s);
    return a;
}

/* Build a kwnames tuple + values from a dict (borrowed values appended to
   vals[n..]); returns the number of keyword values or -1. */
static int qp_tw_dict_to_kw(JSContext *ctx, JSValueConst kwds, JSValueConst *vals,
                            JSValue *pkwnames)
{
    Py_ssize_t pos = 0, n, i = 0;
    JSValue k, v, names;
    *pkwnames = QP_NULL;
    if (QP_IS_NULL(kwds) || JS_IsUndefined(kwds))
        return 0;
    n = qp_dict_size(kwds);
    if (n == 0)
        return 0;
    names = qp_tuple_new(ctx, n);
    if (JS_IsException(names))
        return -1;
    while (qp_dict_next(kwds, &pos, &k, &v)) {
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(names)[i]);
        QP_TUPLE_ITEMS(names)[i] = JS_DupValue(ctx, k);
        vals[i] = v;
        i++;
    }
    *pkwnames = names;
    return (int)n;
}

/* ========================================================================= */
/* Type identity                                                             */
/* ========================================================================= */

static QPType *qp_type_from_value(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_TYPE)
        return NULL;
    return (QPType *)p->u.opaque;
}

static QPType *qp_type_of(JSContext *ctx, JSValueConst v)
{
    QPState *s = QP_STATE(ctx);
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        return s->types[QP_T_int];
    case JS_TAG_BOOL:
        return s->types[QP_T_bool];
    case JS_TAG_FLOAT64:
        return s->types[QP_T_float];
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return s->types[QP_T_str];
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL:
        return s->types[QP_T_NoneType];
    case JS_TAG_OBJECT: {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        JSObject *proto = p->shape ? p->shape->proto : NULL;
        if (proto && proto->class_id == JS_CLASS_PY_TYPE && proto->u.opaque)
            return (QPType *)proto->u.opaque;
        switch (p->class_id) {
        case JS_CLASS_PY_TYPE:
            return s->types[QP_T_type];
        case JS_CLASS_ARRAY:
            return s->types[QP_T_list];
        case JS_CLASS_BYTECODE_FUNCTION:
            if (p->u.func.function_bytecode && p->u.func.function_bytecode->is_py)
                return s->types[QP_T_function];
            break;
        default:
            if (p->class_id >= JS_CLASS_UINT8C_ARRAY && p->class_id <= JS_CLASS_FLOAT64_ARRAY)
                return s->types[QP_T_memoryview];
            break;
        }
        return s->types[QP_T_JsObject];
    }
    default:
        return s->types[QP_T_JsObject];
    }
}

/* PyType_IsSubtype: MRO walk (bases walk before the MRO exists). */
static int qp_type_is_subtype(QPType *a, QPType *b)
{
    if (a == b)
        return 1;
    if (JS_VALUE_GET_TAG(a->mro) == JS_TAG_OBJECT) {
        Py_ssize_t i, n = QP_TUPLE_SIZE(a->mro);
        JSValue *items = QP_TUPLE_ITEMS(a->mro);
        for (i = 0; i < n; i++) {
            if (qp_type_from_value(items[i]) == b)
                return 1;
        }
        return 0;
    }
    /* not ready: walk tp_base (and object at the end) */
    for (a = a->base; a; a = a->base) {
        if (a == b)
            return 1;
    }
    return b->base == NULL && b->tp_name && strcmp(b->tp_name, "object") == 0;
}

static BOOL qp_typecheck(JSContext *ctx, JSValueConst v, QPType *t)
{
    QPType *vt = qp_type_of(ctx, v);
    return vt == t || qp_type_is_subtype(vt, t);
}

/* ---- names --------------------------------------------------------------- */

static JSValue qp_type_tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    if (t->name == JS_ATOM_NULL)
        return qp_str_from_cstr(ctx, "?");
    return qp_str_from_atom(ctx, t->name);
}

static JSValue qp_type_short_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name && !(t->flags & QP_TPFLAGS_HEAPTYPE)) {
        const char *s = strrchr(t->tp_name, '.');
        return qp_str_from_cstr(ctx, s ? s + 1 : t->tp_name);
    }
    if (t->name == JS_ATOM_NULL)
        return qp_str_from_cstr(ctx, "?");
    return qp_str_from_atom(ctx, t->name);
}

static JSValue qp_type_qualname(JSContext *ctx, QPType *t)
{
    if ((t->flags & QP_TPFLAGS_HEAPTYPE) && JS_IsString(t->qualname))
        return JS_DupValue(ctx, t->qualname);
    return qp_type_short_name(ctx, t);
}

/* type_module(): heap types: dict['__module__'] (AttributeError
   "__module__" when missing); builtins: tp_name prefix or "builtins". */
static JSValue qp_type_module(JSContext *ctx, QPType *t)
{
    if (t->flags & QP_TPFLAGS_HEAPTYPE) {
        JSValue mod;
        int r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), &mod);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0) {
            JSValue n = qp_str_from_cstr(ctx, "__module__");
            qp_raise_value(ctx, QP_EXC(ctx, AttributeError), n);
            JS_FreeValue(ctx, n);
            return JS_EXCEPTION;
        }
        return mod;
    }
    if (t->tp_name) {
        const char *s = strrchr(t->tp_name, '.');
        if (s)
            return qp_str_from_utf8(ctx, t->tp_name, s - t->tp_name);
    }
    return qp_str_from_cstr(ctx, "builtins");
}

/* ========================================================================= */
/* Allocation, finalization, subclass links                                  */
/* ========================================================================= */

static uint32_t qp_type_alloc_serial;

#ifdef CONFIG_PYTHON_CAPI
/* python/capi_snapshot.c: while a snapshot restores C modules, the type
   object saved by reference under `key` (a placeholder, NULL if none) */
static JSObject *qp_capi_snap_adopt_take(JSContext *ctx, const char *key);
static BOOL qp_capi_snap_adopting(void);
static const char *qp_capi_snap_adopt_module(void);
#endif

/* New empty type object of metatype `meta` (NULL: no prototype, used for
   `type` itself).  Returns the QPType, the JSValue is QP_TYPE_VALUE.
   `key` (C-API snapshot restore, may be NULL) names the type: a type
   object of the snapshot saved under that key becomes the new type. */
static QPType *qp_type_alloc_key(JSContext *ctx, QPType *meta, const char *key)
{
    JSValue obj;
    QPTypeExt *e = NULL;
    QPType *t;

#ifdef CONFIG_PYTHON_CAPI
    JSObject *p = key ? qp_capi_snap_adopt_take(ctx, key) : NULL;
    if (p) {
        /* the placeholder keeps its identity (and the QPType that
           references to it allocated) */
        obj = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
        if (JS_SetPrototypeInternal(ctx, obj, meta ? QP_TYPE_VALUE(meta) : JS_NULL, FALSE) < 0) {
            JS_FreeValue(ctx, obj);
            return NULL;
        }
        if (p->u.opaque)
            e = QP_TEXT((QPType *)p->u.opaque);
    } else
#endif
    {
        obj = JS_NewObjectProtoClass(ctx, meta ? QP_TYPE_VALUE(meta) : JS_NULL, JS_CLASS_PY_TYPE);
        if (JS_IsException(obj))
            return NULL;
    }
    if (!e) {
        e = js_mallocz(ctx, sizeof(*e));
        if (!e) {
            JS_FreeValue(ctx, obj);
            return NULL;
        }
    }
    e->alloc_serial = ++qp_type_alloc_serial;
    t = &e->t;
    t->obj = JS_VALUE_GET_OBJ(obj);
    t->ctx = ctx;
    t->name = JS_ATOM_NULL;
    t->qualname = JS_UNDEFINED;
    t->bases = JS_UNDEFINED;
    t->mro = JS_UNDEFINED;
    t->dict = JS_UNDEFINED;
    t->slot_names = JS_UNDEFINED;
    t->instance_class_id = JS_CLASS_PY_OBJECT;
    e->doc = QP_NULL;
    JS_VALUE_GET_OBJ(obj)->u.opaque = t;
    return t;
}

static QPType *qp_type_alloc(JSContext *ctx, QPType *meta)
{
    return qp_type_alloc_key(ctx, meta, NULL);
}

static int qp_type_add_subclass(JSContext *ctx, QPType *base, QPType *sub)
{
    QPTypeExt *se = QP_TEXT(sub);
    QPType **nb;
    if (base->subclass_count >= base->subclass_size) {
        int ns = base->subclass_size ? base->subclass_size * 2 : 4;
        QPType **a = js_realloc(ctx, base->subclasses, ns * sizeof(*a));
        if (!a)
            return -1;
        base->subclasses = a;
        base->subclass_size = ns;
    }
    nb = js_realloc(ctx, se->bases_raw, (se->nbases_raw + 1) * sizeof(*nb));
    if (!nb)
        return -1;
    se->bases_raw = nb;
    se->bases_raw[se->nbases_raw++] = base;
    base->subclasses[base->subclass_count++] = sub;
    return 0;
}

static void qp_type_remove_subclass(QPType *base, QPType *sub)
{
    int i;
    for (i = 0; i < base->subclass_count; i++) {
        if (base->subclasses[i] == sub) {
            memmove(&base->subclasses[i], &base->subclasses[i + 1],
                    (base->subclass_count - i - 1) * sizeof(QPType *));
            base->subclass_count--;
            return;
        }
    }
}

/* Unlink t from all its bases (and the bases from t). */
static void qp_type_unlink_bases(QPType *t)
{
    QPTypeExt *e = QP_TEXT(t);
    int i;
    for (i = 0; i < e->nbases_raw; i++) {
        if (e->bases_raw[i])
            qp_type_remove_subclass(e->bases_raw[i], t);
    }
    e->nbases_raw = 0;
}

/* A type held by QPState.types/exc_types (ExceptionGroup is a heap type)
   is finalized while the state still references it only when the GC frees
   a cycle that also holds the context (e.g. JS_FreeRuntime): the state's
   reference is then part of the garbage, so drop the raw QPType pointer
   instead of letting qp_state_free() read the freed payload.  The zombie
   JSObject itself is released at the end of gc_free_cycles(). */
static void qp_type_forget_in_states(JSRuntime *rt, QPType *t)
{
    struct list_head *el;
    int i;

    list_for_each(el, &rt->context_list) {
        JSContext *ctx = list_entry(el, JSContext, link);
        QPState *s = ctx->py;
        if (!s)
            continue;
        for (i = 0; i < QP_T__COUNT; i++) {
            if (s->types[i] == t)
                s->types[i] = NULL;
        }
        for (i = 0; i < QP_EXC__COUNT; i++) {
            if (s->exc_types[i] == t)
                s->exc_types[i] = NULL;
        }
    }
}

static void qp_type_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    QPType *t = (QPType *)p->u.opaque;
    QPTypeExt *e;
    int i, j;

    if (!t)
        return;
    qp_type_forget_in_states(rt, t);
    e = QP_TEXT(t);
    qp_type_unlink_bases(t);
    /* subclasses that are still alive (same GC cycle) forget us */
    for (i = 0; i < t->subclass_count; i++) {
        QPTypeExt *se = QP_TEXT(t->subclasses[i]);
        for (j = 0; j < se->nbases_raw; j++) {
            if (se->bases_raw[j] == t)
                se->bases_raw[j] = NULL;
        }
        if (se->t.base == t)
            se->t.base = NULL;
    }
    if (t->name != JS_ATOM_NULL)
        JS_FreeAtomRT(rt, t->name);
    if (t->flags & QP_TPFLAGS_PRIV_OWNED_NAME)
        js_free_rt(rt, (char *)t->tp_name);
    JS_FreeValueRT(rt, t->qualname);
    JS_FreeValueRT(rt, t->bases);
    JS_FreeValueRT(rt, t->mro);
    JS_FreeValueRT(rt, t->dict);
    JS_FreeValueRT(rt, t->slot_names);
    JS_FreeValueRT(rt, e->doc);
    if (t->ic_shape)
        js_free_shape(rt, t->ic_shape);
    js_free_rt(rt, t->subclasses);
    js_free_rt(rt, e->bases_raw);
    js_free_rt(rt, e);
    p->u.opaque = NULL;
}

static void qp_type_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPType *t = (QPType *)JS_VALUE_GET_OBJ(val)->u.opaque;
    if (!t)
        return;
    JS_MarkValue(rt, t->qualname, mark_func);
    JS_MarkValue(rt, t->bases, mark_func);
    JS_MarkValue(rt, t->mro, mark_func);
    JS_MarkValue(rt, t->dict, mark_func);
    JS_MarkValue(rt, t->slot_names, mark_func);
    JS_MarkValue(rt, QP_TEXT(t)->doc, mark_func);
    if (t->ic_shape)
        mark_func(rt, &t->ic_shape->header);
}

/* ========================================================================= */
/* Version tags and the method cache                                         */
/* ========================================================================= */

static uint32_t qp_type_version(JSContext *ctx, QPType *t)
{
    QPState *s = QP_STATE(ctx);
    QPTypeExt *e = QP_TEXT(t);
    int i;

    if (t->version_tag)
        return t->version_tag;
    if (s->next_type_version == 0 || !(t->flags & QP_TPFLAGS_READY) || e->no_version)
        return 0;
    /* invariant: a tagged type has tagged bases, so that invalidating a
       base reaches every tagged subclass */
    for (i = 0; i < e->nbases_raw; i++) {
        if (e->bases_raw[i] && qp_type_version(ctx, e->bases_raw[i]) == 0)
            return 0;
    }
    t->version_tag = s->next_type_version++;
    t->flags |= QP_TPFLAGS_VALID_VERSION_TAG;
    return t->version_tag;
}

static void qp_type_modified(JSContext *ctx, QPType *t)
{
    int i;
#ifdef CONFIG_PYTHON_CAPI
    qp_capi_type_modified(ctx, t);  /* D5 (CapiB) R5: tp_flags / slots of the C mirror */
#endif
    if (t->version_tag == 0)
        return;
    t->version_tag = 0;
    t->flags &= ~(uint64_t)QP_TPFLAGS_VALID_VERSION_TAG;
    for (i = 0; i < t->subclass_count; i++)
        qp_type_modified(ctx, t->subclasses[i]);
}

static inline uint32_t qp_mcache_hash(uint32_t version, JSAtom name)
{
    uint32_t h = version ^ (name * 2654435761u);
    return (h ^ (h >> 16)) & ((1u << QP_MCACHE_SIZE_EXP) - 1);
}

/* find_name_in_mro: 1 found (*pres borrowed: the dict keeps it alive),
   0 absent, -1 error (cleared; *pres = QP_NULL); never raises. */
static int qp_find_name_in_mro(JSContext *ctx, QPType *t, JSAtom name, JSValue *pres)
{
    JSValue v, mro;
    Py_ssize_t i, n;
    JSValue *items;
    int r = 0;

    *pres = QP_NULL;
    if (JS_VALUE_GET_TAG(t->mro) != JS_TAG_OBJECT) {
        /* not ready yet: own dict only */
        if (JS_VALUE_GET_TAG(t->dict) != JS_TAG_OBJECT)
            return 0;
        r = qp_dict_get_item_atom(ctx, t->dict, name, &v);
        if (r > 0) {
            JS_FreeValue(ctx, v);
            *pres = v;
        } else if (r < 0) {
            qp_tw_clear_exc(ctx);
        }
        return r;
    }
    /* Keep a strong reference to the MRO: t->mro can be replaced during the
       dict lookup, e.g. when comparing to a non-str key whose __eq__ assigns
       __bases__ (bpo-14199). */
    mro = JS_DupValue(ctx, t->mro);
    n = QP_TUPLE_SIZE(mro);
    items = QP_TUPLE_ITEMS(mro);
    for (i = 0; i < n; i++) {
        QPType *b = qp_type_from_value(items[i]);
        if (!b || JS_VALUE_GET_TAG(b->dict) != JS_TAG_OBJECT)
            continue;
        r = qp_dict_get_item_atom(ctx, b->dict, name, &v);
        if (r > 0) {
            JS_FreeValue(ctx, v);
            *pres = v;
            break;
        }
        if (r < 0) {
            qp_tw_clear_exc(ctx);
            break;
        }
    }
    JS_FreeValue(ctx, mro);
    return r;
}

static JSValue qp_type_lookup(JSContext *ctx, QPType *t, JSAtom name)
{
    QPState *s = QP_STATE(ctx);
    QPMethodCacheEntry *ent;
    uint32_t v = t->version_tag;
    JSValue res;

    if (v) {
        ent = &s->mcache[qp_mcache_hash(v, name)];
        if (ent->version == v && ent->name == name)
            return ent->value;
    }
    /* The tag is taken before the walk (CPython assigns it first): when user
       code run by the walk modifies the type, the result lands under the
       stale tag, which is never reused and so never hit. */
    v = qp_type_version(ctx, t);
    /* only results without an error are cached */
    if (qp_find_name_in_mro(ctx, t, name, &res) < 0)
        return QP_NULL;
    if (v) {
        ent = &s->mcache[qp_mcache_hash(v, name)];
        ent->version = v;
        ent->name = name;
        ent->value = res;
    }
    return res;
}

/* ========================================================================= */
/* MRO (C3)                                                                  */
/* ========================================================================= */

/* class_name(): __name__ or repr. */
static JSValue qp_tw_class_name(JSContext *ctx, JSValueConst cls)
{
    JSValue name;
    int r = qp_getattr_opt(ctx, cls, QP_ATOM(ctx, __name__), &name);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_repr(ctx, cls);
    return name;
}

static int qp_tail_contains(JSValueConst tup, Py_ssize_t whence, JSValueConst o)
{
    Py_ssize_t j, n = QP_TUPLE_SIZE(tup);
    for (j = whence + 1; j < n; j++) {
        if (JS_VALUE_GET_PTR(QP_TUPLE_ITEMS(tup)[j]) == JS_VALUE_GET_PTR(o))
            return 1;
    }
    return 0;
}

static void qp_set_mro_error(JSContext *ctx, JSValue *to_merge, int n, Py_ssize_t *remain)
{
    JSValue seen[64];
    int nseen = 0, i, j;
    JSValue msg, parts;

    for (i = 0; i < n; i++) {
        if (remain[i] < QP_TUPLE_SIZE(to_merge[i])) {
            JSValue c = QP_TUPLE_ITEMS(to_merge[i])[remain[i]];
            for (j = 0; j < nseen; j++)
                if (JS_VALUE_GET_PTR(seen[j]) == JS_VALUE_GET_PTR(c))
                    break;
            if (j == nseen && nseen < 64)
                seen[nseen++] = c;
        }
    }
    msg = qp_str_from_cstr(ctx, "Cannot create a consistent method resolution order (MRO) for bases");
    for (i = 0; i < nseen && !JS_IsException(msg); i++) {
        JSValue name = qp_tw_class_name(ctx, seen[i]);
        if (JS_IsException(name)) {
            JS_FreeValue(ctx, msg);
            return;
        }
        if (!qp_str_check(ctx, name)) {
            JS_FreeValue(ctx, name);
            name = qp_str_from_cstr(ctx, "?");
        }
        parts = qp_str_from_format(ctx, "%U %U%s", msg, name, i + 1 < nseen ? "," : "");
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, msg);
        msg = parts;
    }
    if (JS_IsException(msg))
        return;
    qp_raise_value(ctx, QP_EXC(ctx, TypeError), msg);
    JS_FreeValue(ctx, msg);
}

/* mro_implementation: new tuple. */
static JSValue qp_mro_implementation(JSContext *ctx, QPType *t)
{
    Py_ssize_t n = QP_TUPLE_SIZE(t->bases), i, j, k;
    JSValue *bases = QP_TUPLE_ITEMS(t->bases);
    JSValue *to_merge = NULL, *acc = NULL, res;
    Py_ssize_t *remain = NULL, nacc = 0, cap = 1;
    int empty_cnt;

    for (i = 0; i < n; i++) {
        QPType *b = qp_type_from_value(bases[i]);
        if (JS_VALUE_GET_TAG(b->mro) != JS_TAG_OBJECT) {
            JSValue tn = qp_type_tp_name(ctx, b);
            qp_raise_type_error(ctx, "Cannot extend an incomplete type '%.100U'", tn);
            JS_FreeValue(ctx, tn);
            return JS_EXCEPTION;
        }
    }
    if (n == 1) {
        QPType *b = qp_type_from_value(bases[0]);
        Py_ssize_t m = QP_TUPLE_SIZE(b->mro);
        res = qp_tuple_new(ctx, m + 1);
        if (JS_IsException(res))
            return res;
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(res)[0]);
        QP_TUPLE_ITEMS(res)[0] = qp_type_dup(ctx, t);
        for (i = 0; i < m; i++) {
            JS_FreeValue(ctx, QP_TUPLE_ITEMS(res)[i + 1]);
            QP_TUPLE_ITEMS(res)[i + 1] = JS_DupValue(ctx, QP_TUPLE_ITEMS(b->mro)[i]);
        }
        return res;
    }
    /* check_duplicates */
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (JS_VALUE_GET_PTR(bases[i]) == JS_VALUE_GET_PTR(bases[j])) {
                JSValue name = qp_tw_class_name(ctx, bases[i]);
                if (JS_IsException(name))
                    return JS_EXCEPTION;
                if (qp_str_check(ctx, name))
                    qp_raise_type_error(ctx, "duplicate base class %U", name);
                else
                    qp_raise_type_error(ctx, "duplicate base class");
                JS_FreeValue(ctx, name);
                return JS_EXCEPTION;
            }
        }
    }
    to_merge = js_malloc(ctx, (n + 1) * sizeof(JSValue));
    remain = js_mallocz(ctx, (n + 1) * sizeof(Py_ssize_t));
    for (i = 0; i < n; i++)
        cap += QP_TUPLE_SIZE(qp_type_from_value(bases[i])->mro);
    acc = js_malloc(ctx, cap * sizeof(JSValue));
    if (!to_merge || !remain || !acc)
        goto fail;
    for (i = 0; i < n; i++)
        to_merge[i] = qp_type_from_value(bases[i])->mro;
    to_merge[n] = t->bases;
    acc[nacc++] = QP_TYPE_VALUE(t);
again:
    empty_cnt = 0;
    for (i = 0; i <= n; i++) {
        JSValue cand;
        if (remain[i] >= QP_TUPLE_SIZE(to_merge[i])) {
            empty_cnt++;
            continue;
        }
        cand = QP_TUPLE_ITEMS(to_merge[i])[remain[i]];
        for (j = 0; j <= n; j++) {
            if (qp_tail_contains(to_merge[j], remain[j], cand))
                goto skip;
        }
        if (nacc < cap)
            acc[nacc++] = cand;
        for (j = 0; j <= n; j++) {
            if (remain[j] < QP_TUPLE_SIZE(to_merge[j]) &&
                JS_VALUE_GET_PTR(QP_TUPLE_ITEMS(to_merge[j])[remain[j]]) == JS_VALUE_GET_PTR(cand))
                remain[j]++;
        }
        goto again;
    skip:;
    }
    if (empty_cnt != n + 1) {
        qp_set_mro_error(ctx, to_merge, (int)(n + 1), remain);
        goto fail;
    }
    res = qp_tuple_pack(ctx, nacc, (JSValueConst *)acc);
    js_free(ctx, to_merge);
    js_free(ctx, remain);
    js_free(ctx, acc);
    (void)k;
    return res;
fail:
    js_free(ctx, to_merge);
    js_free(ctx, remain);
    js_free(ctx, acc);
    return JS_EXCEPTION;
}

/* solid_base: the most derived base with a different instance layout. */
static QPType *qp_solid_base(JSContext *ctx, QPType *t)
{
    QPType *base = t->base ? qp_solid_base(ctx, t->base) : QP_TYPE(ctx, object);
    if (t->instance_class_id != base->instance_class_id || t->nslots != base->nslots) {
#ifdef CONFIG_PYTHON_CAPI
        /* D5: a C type that adds no C fields to object (numpy.generic,
           numpy.floating) keeps object's layout (CPython shape_differs), so
           numpy.float64(numpy.floating, float) has float as its best base */
        if (t->instance_class_id == JS_CLASS_PY_NATIVE && base == QP_TYPE(ctx, object) &&
            t->nslots == base->nslots && qp_capi_native_same_layout(t, base))
            return base;
#endif
        return t;
    }
#ifdef CONFIG_PYTHON_CAPI
    /* D5 (CapiB) R4: a native type that adds C fields is a solid base */
    if (t->instance_class_id == JS_CLASS_PY_NATIVE && t->base &&
        !qp_capi_native_same_layout(t, t->base))
        return t;
#endif
    return base;
}

static int qp_mro_check(JSContext *ctx, QPType *t, JSValueConst mro)
{
    QPType *solid = qp_solid_base(ctx, t);
    Py_ssize_t i, n = QP_TUPLE_SIZE(mro);
    for (i = 0; i < n; i++) {
        JSValue o = QP_TUPLE_ITEMS(mro)[i];
        QPType *b = qp_type_from_value(o);
        if (!b) {
            qp_raise_type_error(ctx, "%N.mro() returned a non-class ('%T')", t, o);
            return -1;
        }
        if (!qp_type_is_subtype(solid, qp_solid_base(ctx, b))) {
            qp_raise_type_error(ctx, "%N.mro() returned base with unsuitable layout ('%N')", t, b);
            return -1;
        }
    }
    return 0;
}

/* type_mro_modified: the attribute cache relies on the invalidation of a
   type reaching its subclasses through tp_bases; a type whose metatype
   overrides mro(), or whose MRO lacks one of its bases, is never given a
   version tag (CPython: version 0, tp_versions_used = _Py_ATTR_CACHE_UNUSED). */
static void qp_type_mro_modified(JSContext *ctx, QPType *t, BOOL custom_meta)
{
    QPTypeExt *e = QP_TEXT(t);
    Py_ssize_t i, j, nb, nm;
    JSValue m1, m2;
    int r1, r2;

    if (custom_meta) {
        /* has_custom_mro: type(t).mro is not type.mro */
        r1 = qp_find_name_in_mro(ctx, qp_type_of(ctx, QP_TYPE_VALUE(t)), QP_ATOM(ctx, mro), &m1);
        r2 = qp_find_name_in_mro(ctx, QP_TYPE(ctx, type), QP_ATOM(ctx, mro), &m2);
        if (r1 != r2 || (r1 > 0 && JS_VALUE_GET_PTR(m1) != JS_VALUE_GET_PTR(m2)))
            goto clear;
    }
    if (!qp_tuple_check(ctx, t->bases))
        return;
    nb = QP_TUPLE_SIZE(t->bases);
    nm = QP_TUPLE_SIZE(t->mro);
    for (i = 0; i < nb; i++) {
        JSValueConst b = QP_TUPLE_ITEMS(t->bases)[i];
        for (j = 0; j < nm; j++) {
            if (JS_VALUE_GET_PTR(QP_TUPLE_ITEMS(t->mro)[j]) == JS_VALUE_GET_PTR(b))
                break;
        }
        if (j == nm)
            goto clear;
    }
    return;
clear:
    e->no_version = 1;
    qp_type_modified(ctx, t);
}

/* mro_invoke + mro_internal: compute and store t->mro. */
static int qp_mro_internal(JSContext *ctx, QPType *t)
{
    JSValue res, mro;
    int custom = qp_type_of(ctx, QP_TYPE_VALUE(t)) != QP_TYPE(ctx, type);

    if (custom) {
        JSValueConst a[1] = { QP_TYPE_VALUE(t) };
        res = qp_vectorcall_method(ctx, QP_ATOM(ctx, mro), a, 1);
    } else {
        res = qp_mro_implementation(ctx, t);
    }
    if (JS_IsException(res))
        return -1;
    if (qp_tuple_check_exact(ctx, res)) {
        mro = res;
    } else {
        mro = qp_tuple_from_iterable(ctx, res);
        JS_FreeValue(ctx, res);
    }
    if (JS_IsException(mro))
        return -1;
    if (QP_TUPLE_SIZE(mro) == 0) {
        JS_FreeValue(ctx, mro);
        qp_raise_type_error(ctx, "type MRO must not be empty");
        return -1;
    }
    if (custom && qp_mro_check(ctx, t, mro) < 0) {
        JS_FreeValue(ctx, mro);
        return -1;
    }
    JS_FreeValue(ctx, t->mro);
    t->mro = mro;
    qp_type_modified(ctx, t);
    qp_type_mro_modified(ctx, t, custom);
    return 0;
}

/* find_best_base; check_basetype FALSE for PyType_Ready of a static type,
   whose bases CPython's type_ready never checks for Py_TPFLAGS_BASETYPE
   (numpy's numpy.character extends numpy.flexible, which lacks it). */
static QPType *qp_best_base_ex(JSContext *ctx, JSValueConst bases, BOOL check_basetype)
{
    Py_ssize_t i, n = QP_TUPLE_SIZE(bases);
    QPType *base = NULL, *winner = NULL;
    for (i = 0; i < n; i++) {
        JSValue o = QP_TUPLE_ITEMS(bases)[i];
        QPType *bi = qp_type_from_value(o), *cand;
        if (!bi) {
            qp_raise_type_error(ctx, "bases must be types; got '%T'", o);
            return NULL;
        }
        if (check_basetype && !(bi->flags & QP_TPFLAGS_BASETYPE)) {
            JSValue tn = qp_type_tp_name(ctx, bi);
            qp_raise_type_error(ctx, "type '%.100U' is not an acceptable base type", tn);
            JS_FreeValue(ctx, tn);
            return NULL;
        }
        cand = qp_solid_base(ctx, bi);
        if (!winner) {
            winner = cand;
            base = bi;
        } else if (qp_type_is_subtype(winner, cand)) {
            ;
        } else if (qp_type_is_subtype(cand, winner)) {
            winner = cand;
            base = bi;
        } else {
            qp_raise_type_error(ctx, "multiple bases have instance lay-out conflict");
            return NULL;
        }
    }
    return base;
}

static QPType *qp_best_base(JSContext *ctx, JSValueConst bases)
{
    return qp_best_base_ex(ctx, bases, TRUE);
}

/* _PyType_CalculateMetaclass */
static QPType *qp_calculate_metaclass(JSContext *ctx, QPType *meta, JSValueConst bases)
{
    Py_ssize_t i, n = QP_TUPLE_SIZE(bases);
    QPType *winner = meta;
    for (i = 0; i < n; i++) {
        QPType *tt = qp_type_of(ctx, QP_TUPLE_ITEMS(bases)[i]);
        if (qp_type_is_subtype(winner, tt))
            continue;
        if (qp_type_is_subtype(tt, winner)) {
            winner = tt;
            continue;
        }
        qp_raise_type_error(ctx, "metaclass conflict: the metaclass of a derived class must be "
                            "a (non-strict) subclass of the metaclasses of all its bases");
        return NULL;
    }
    return winner;
}

#include "typeobject_slots.h"
#include "typeobject_attr.h"

/* ========================================================================= */
/* Readying (PyType_Ready)                                                    */
/* ========================================================================= */

/* dict.setdefault(name, v) for readying (consumes v). */
static int qp_tw_dict_setdefault(JSContext *ctx, JSValueConst d, JSAtom name, JSValue v)
{
    JSValue tmp;
    int r = qp_dict_get_item_atom(ctx, d, name, &tmp);
    if (r != 0) {
        JS_FreeValue(ctx, v);
        if (r > 0)
            JS_FreeValue(ctx, tmp);
        return r < 0 ? -1 : 0;
    }
    r = qp_dict_set_item_atom(ctx, d, name, v);
    JS_FreeValue(ctx, v);
    return r;
}

static int qp_tw_dict_has(JSContext *ctx, JSValueConst d, JSAtom name)
{
    JSValue tmp;
    int r = qp_dict_get_item_atom(ctx, d, name, &tmp);
    if (r > 0)
        JS_FreeValue(ctx, tmp);
    return r;
}

/* type_add_methods / members / getsets / doc for a builtin spec. */
static int qp_type_fill_dict(JSContext *ctx, QPType *t, const QPTypeSpec *spec)
{
    const QPMethodDef *ml;
    const QPGetSetDef *gs;
    const QPMemberDef *mb;
    JSAtom a;
    JSValue d;
    int r;

    if (qp_add_operators(ctx, t) < 0)
        return -1;
    for (ml = spec->methods; ml && ml->name; ml++) {
        d = qp_descr_new_method(ctx, t, ml);
        if (JS_IsException(d))
            return -1;
        a = JS_NewAtom(ctx, ml->name);
        if (ml->flags & QP_METH_COEXIST) {
            r = qp_dict_set_item_atom(ctx, t->dict, a, d);
            JS_FreeValue(ctx, d);
        } else {
            r = qp_tw_dict_setdefault(ctx, t->dict, a, d);
        }
        JS_FreeAtom(ctx, a);
        if (r < 0)
            return -1;
    }
    for (mb = spec->members; mb && mb->name; mb++) {
        a = JS_NewAtom(ctx, mb->name);
        d = qp_descr_new_member(ctx, t, a, mb->slot, mb->flags);
        if (JS_IsException(d)) {
            JS_FreeAtom(ctx, a);
            return -1;
        }
        r = qp_tw_dict_setdefault(ctx, t->dict, a, d);
        JS_FreeAtom(ctx, a);
        if (r < 0)
            return -1;
    }
    for (gs = spec->getsets; gs && gs->name; gs++) {
        d = (gs->flags & QP_GETSET_MEMBER) ? qp_descr_new_member_getset(ctx, t, gs) :
            qp_descr_new_getset(ctx, t, gs);
        if (JS_IsException(d))
            return -1;
        a = JS_NewAtom(ctx, gs->name);
        r = qp_tw_dict_setdefault(ctx, t->dict, a, d);
        JS_FreeAtom(ctx, a);
        if (r < 0)
            return -1;
    }
    /* type_dict_set_doc */
    r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __doc__));
    if (r < 0)
        return -1;
    if (r == 0) {
        JSValue sn = qp_type_short_name(ctx, t);
        const char *s;
        if (JS_IsException(sn))
            return -1;
        s = JS_ToCString(ctx, sn);
        JS_FreeValue(ctx, sn);
        if (!s)
            return -1;
        d = spec->doc ? qp_doc_from_internal(ctx, s, spec->doc) : JS_UNDEFINED;
        JS_FreeCString(ctx, s);
        if (JS_IsException(d))
            return -1;
        r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __doc__), d);
        JS_FreeValue(ctx, d);
        if (r < 0)
            return -1;
    }
    return 0;
}

/* inherit_special */
static void qp_inherit_special(JSContext *ctx, QPType *t, QPType *base)
{
    QPState *s = QP_STATE(ctx);
    uint64_t f = 0;
    if (qp_type_is_subtype(base, s->exc_types[QP_EXC_BaseException]))
        f |= QP_TPFLAGS_BASE_EXC_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_type]))
        f |= QP_TPFLAGS_TYPE_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_int]))
        f |= QP_TPFLAGS_LONG_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_bytes]))
        f |= QP_TPFLAGS_BYTES_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_str]))
        f |= QP_TPFLAGS_UNICODE_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_tuple]))
        f |= QP_TPFLAGS_TUPLE_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_list]))
        f |= QP_TPFLAGS_LIST_SUBCLASS;
    else if (qp_type_is_subtype(base, s->types[QP_T_dict]))
        f |= QP_TPFLAGS_DICT_SUBCLASS;
    f |= base->flags & (QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF |
                        QP_TPFLAGS_PRIV_VARSIZE | QP_TPFLAGS_HAS_DEL);
    t->flags |= f;
}

/* inherit_slots(type, base) */
static int qp_inherit_slots(JSContext *ctx, QPType *t, QPType *b)
{
    QPTypeSlots *ts = &t->slots, *bs = &b->slots;
#define QP_COPY(F) do { if (!ts->F && bs->F) ts->F = bs->F; } while (0)
    QP_COPY(nb.nb_add); QP_COPY(nb.nb_subtract); QP_COPY(nb.nb_multiply);
    QP_COPY(nb.nb_remainder); QP_COPY(nb.nb_divmod); QP_COPY(nb.nb_power);
    QP_COPY(nb.nb_negative); QP_COPY(nb.nb_positive); QP_COPY(nb.nb_absolute);
    QP_COPY(nb.nb_bool); QP_COPY(nb.nb_invert); QP_COPY(nb.nb_lshift);
    QP_COPY(nb.nb_rshift); QP_COPY(nb.nb_and); QP_COPY(nb.nb_xor); QP_COPY(nb.nb_or);
    QP_COPY(nb.nb_int); QP_COPY(nb.nb_float); QP_COPY(nb.nb_inplace_add);
    QP_COPY(nb.nb_inplace_subtract); QP_COPY(nb.nb_inplace_multiply);
    QP_COPY(nb.nb_inplace_remainder); QP_COPY(nb.nb_inplace_power);
    QP_COPY(nb.nb_inplace_lshift); QP_COPY(nb.nb_inplace_rshift);
    QP_COPY(nb.nb_inplace_and); QP_COPY(nb.nb_inplace_xor); QP_COPY(nb.nb_inplace_or);
    QP_COPY(nb.nb_true_divide); QP_COPY(nb.nb_floor_divide);
    QP_COPY(nb.nb_inplace_true_divide); QP_COPY(nb.nb_inplace_floor_divide);
    QP_COPY(nb.nb_index); QP_COPY(nb.nb_matrix_multiply);
    QP_COPY(nb.nb_inplace_matrix_multiply);
    QP_COPY(am.am_await); QP_COPY(am.am_aiter); QP_COPY(am.am_anext); QP_COPY(am.am_send);
    QP_COPY(sq.sq_length); QP_COPY(sq.sq_concat); QP_COPY(sq.sq_repeat);
    QP_COPY(sq.sq_item); QP_COPY(sq.sq_ass_item); QP_COPY(sq.sq_contains);
    QP_COPY(sq.sq_inplace_concat); QP_COPY(sq.sq_inplace_repeat);
    QP_COPY(mp.mp_length); QP_COPY(mp.mp_subscript); QP_COPY(mp.mp_ass_subscript);
    QP_COPY(bf.bf_getbuffer); QP_COPY(bf.bf_releasebuffer);
    if (!ts->tp_getattro)
        ts->tp_getattro = bs->tp_getattro;
    if (!ts->tp_setattro)
        ts->tp_setattro = bs->tp_setattro;
    QP_COPY(tp_repr);
    QP_COPY(tp_call);
    QP_COPY(tp_str);
    if (!ts->tp_richcompare && !ts->tp_hash) {
        int r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __eq__));
        if (r == 0)
            r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __hash__));
        if (r < 0)
            return -1;
        if (!r) {
            ts->tp_richcompare = bs->tp_richcompare;
            ts->tp_hash = bs->tp_hash;
        }
    }
    QP_COPY(tp_iter);
    QP_COPY(tp_iternext);
    QP_COPY(tp_iteritem);
    QP_COPY(tp_descr_get);
    if (bs->tp_descr_get && ts->tp_descr_get == bs->tp_descr_get &&
        (t->flags & QP_TPFLAGS_IMMUTABLETYPE) && (b->flags & QP_TPFLAGS_METHOD_DESCRIPTOR))
        t->flags |= QP_TPFLAGS_METHOD_DESCRIPTOR;
    QP_COPY(tp_descr_set);
    QP_COPY(tp_init);
    QP_COPY(tp_finalize);
#undef QP_COPY
    return 0;
}

/* Common part of PyType_Ready for builtin (spec != NULL) and heap types:
   t->bases, t->base, layout and slots set by the caller. */
static int qp_type_ready_common(JSContext *ctx, QPType *t, const QPTypeSpec *spec)
{
    QPType *base = t->base;
    Py_ssize_t i, n;
    JSValue nw;
    int r;

    t->flags |= QP_TPFLAGS_READYING;
    if (JS_VALUE_GET_TAG(t->dict) != JS_TAG_OBJECT) {
        t->dict = qp_dict_new(ctx);
        if (JS_IsException(t->dict)) {
            t->dict = JS_UNDEFINED;
            goto fail;
        }
    }
    /* MRO */
    if (!base) {
        JSValueConst self = QP_TYPE_VALUE(t);
        JS_FreeValue(ctx, t->mro);
        t->mro = qp_tuple_pack(ctx, 1, &self);
        if (JS_IsException(t->mro)) {
            t->mro = JS_UNDEFINED;
            goto fail;
        }
    } else if (qp_mro_internal(ctx, t) < 0) {
        goto fail;
    }
    /* type_ready_set_new */
    if (!t->slots.tp_new && base == QP_TYPE(ctx, object) && !(t->flags & QP_TPFLAGS_HEAPTYPE))
        t->flags |= QP_TPFLAGS_DISALLOW_INSTANTIATION;
    if (!(t->flags & QP_TPFLAGS_DISALLOW_INSTANTIATION)) {
        if (t->slots.tp_new) {
            r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __new__));
            if (r < 0)
                goto fail;
            if (r == 0) {
                nw = qp_cfunction_new(ctx, &qp_tp_new_methoddef, QP_TYPE_VALUE(t), JS_UNDEFINED);
                if (JS_IsException(nw))
                    goto fail;
                r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __new__), nw);
                JS_FreeValue(ctx, nw);
                if (r < 0)
                    goto fail;
            }
        } else if (base) {
            t->slots.tp_new = base->slots.tp_new;
        }
    } else {
        t->slots.tp_new = NULL;
    }
    /* type_ready_fill_dict */
    if (spec) {
        if (qp_type_fill_dict(ctx, t, spec) < 0)
            goto fail;
    } else {
        r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __doc__));
        if (r < 0)
            goto fail;
        if (r == 0 && qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __doc__), JS_UNDEFINED) < 0)
            goto fail;
    }
    /* type_ready_inherit */
    if (base) {
        qp_inherit_special(ctx, t, base);
        n = QP_TUPLE_SIZE(t->mro);
        for (i = 1; i < n; i++) {
            QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(t->mro)[i]);
            if (!b)
                continue;
            if (qp_inherit_slots(ctx, t, b) < 0)
                goto fail;
            /* inherit_patma_flags */
            if (!(t->flags & (QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_MAPPING)))
                t->flags |= b->flags & (QP_TPFLAGS_SEQUENCE | QP_TPFLAGS_MAPPING);
        }
    }
    /* type_ready_set_hash */
    if (!t->slots.tp_hash) {
        r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __hash__));
        if (r < 0)
            goto fail;
        if (r == 0) {
            if (qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __hash__), JS_UNDEFINED) < 0)
                goto fail;
            t->slots.tp_hash = qp_hash_not_implemented;
        }
    }
    /* type_ready_add_subclasses */
    n = QP_TUPLE_SIZE(t->bases);
    for (i = 0; i < n; i++) {
        QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(t->bases)[i]);
        if (b && qp_type_add_subclass(ctx, b, t) < 0)
            goto fail;
    }
    t->flags &= ~(uint64_t)QP_TPFLAGS_READYING;
    t->flags |= QP_TPFLAGS_READY;
    return 0;
fail:
    t->flags &= ~(uint64_t)QP_TPFLAGS_READYING;
    return -1;
}

static int qp_type_ready(JSContext *ctx, QPType *t, const QPTypeSpec *spec)
{
    QPState *s = QP_STATE(ctx);
    QPType *base;
    const char *dot;
    JSValue tup;

    if (t->flags & QP_TPFLAGS_READY)
        return 0;
    t->spec = spec;
    t->tp_name = spec->name;
    dot = strrchr(spec->name, '.');
    if (t->name != JS_ATOM_NULL)
        JS_FreeAtom(ctx, t->name);
    t->name = JS_NewAtom(ctx, dot ? dot + 1 : spec->name);
    JS_FreeValue(ctx, t->qualname);
    t->qualname = qp_str_from_atom(ctx, t->name);
    if (t->name == JS_ATOM_NULL || JS_IsException(t->qualname)) {
        t->qualname = JS_UNDEFINED;
        return -1;
    }
    t->flags = (t->flags & QP_TPFLAGS_WEAKREF) |
        (spec->flags & ~(uint64_t)(QP_TPFLAGS_READY | QP_TPFLAGS_READYING | QP_TPFLAGS_HEAPTYPE));
    if (t == s->types[QP_T_object])
        base = NULL;
    else if (spec->exc_base >= 0)
        base = s->exc_types[spec->exc_base];
    else if (spec->base >= 0)
        base = s->types[spec->base];
    else
        base = s->types[QP_T_object];
    if (base && !(base->flags & QP_TPFLAGS_READY)) {
        qp_raise_system_error(ctx, "type '%s' readied before its base '%s'", spec->name,
                              base->tp_name ? base->tp_name : "?");
        return -1;
    }
    t->base = base;
    t->instance_class_id = spec->instance_class_id ? spec->instance_class_id
        : (base ? base->instance_class_id : JS_CLASS_PY_OBJECT);
    t->nslots = (base ? base->nslots : 0) + spec->nslots;
    t->slots = spec->slots;
    if (base) {
        JSValueConst b = QP_TYPE_VALUE(base);
        tup = qp_tuple_pack(ctx, 1, &b);
    } else {
        tup = qp_tuple_pack(ctx, 0, NULL);
    }
    if (JS_IsException(tup))
        return -1;
    JS_FreeValue(ctx, t->bases);
    t->bases = tup;
    return qp_type_ready_common(ctx, t, spec);
}

/* PyType_FromSpec for native-module types. */
static JSValue qp_type_from_spec(JSContext *ctx, const QPTypeSpec *spec)
{
    QPType *t = qp_type_alloc(ctx, QP_TYPE(ctx, type));
    JSValue v, mod;
    if (!t)
        return JS_EXCEPTION;
    v = QP_TYPE_VALUE(t);
    if (qp_type_ready(ctx, t, spec) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    mod = qp_type_module(ctx, t);
    if (JS_IsException(mod) ||
        qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), mod) < 0) {
        JS_FreeValue(ctx, mod);
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, mod);
    return v;
}

/* D5 (CapiB) R3: PyType_Ready core for C-API types (see object.h). */
static JSValue qp_type_new_from_spec_bases(JSContext *ctx, QPType *meta, const QPTypeSpec *spec,
                                           JSValueConst bases, JSValueConst dict)
{
    QPType *t, *base;
    const char *dot;
    JSValue v, mod;
    int r;

    if (JS_VALUE_GET_TAG(bases) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(bases)->class_id != JS_CLASS_PY_TUPLE || QP_TUPLE_SIZE(bases) == 0) {
        qp_raise_system_error(ctx, "type '%s': bases must be a non-empty tuple", spec->name);
        return JS_EXCEPTION;
    }
    /* heap types had their bases checked by PyType_FromMetaclass */
    base = qp_best_base_ex(ctx, bases, FALSE);
    if (!base)
        return JS_EXCEPTION;
#ifdef CONFIG_PYTHON_CAPI
    if (qp_capi_snap_adopting()) {
        char key[256];
        snprintf(key, sizeof(key), "T:%s", spec->name);
        t = qp_type_alloc_key(ctx, meta, key);
    } else
#endif
        t = qp_type_alloc(ctx, meta);
    if (!t)
        return JS_EXCEPTION;
    v = QP_TYPE_VALUE(t);
    t->spec = spec;
    t->tp_name = spec->name;
    dot = strrchr(spec->name, '.');
    t->name = JS_NewAtom(ctx, dot ? dot + 1 : spec->name);
    if (t->name == JS_ATOM_NULL)
        goto fail;
    t->qualname = qp_str_from_atom(ctx, t->name);
    if (JS_IsException(t->qualname)) {
        t->qualname = JS_UNDEFINED;
        goto fail;
    }
    t->flags = spec->flags & ~(uint64_t)(QP_TPFLAGS_READY | QP_TPFLAGS_READYING);
    t->base = base;
    t->instance_class_id = spec->instance_class_id ? spec->instance_class_id
                                                   : base->instance_class_id;
    t->nslots = base->nslots + spec->nslots;
    t->slots = spec->slots;
    t->bases = JS_DupValue(ctx, bases);
    if (!QP_IS_NULL(dict) && !JS_IsUndefined(dict)) {
        t->dict = qp_dict_copy(ctx, dict);
        if (JS_IsException(t->dict)) {
            t->dict = JS_UNDEFINED;
            goto fail;
        }
    }
    if (qp_type_ready_common(ctx, t, spec) < 0)
        goto fail;
    /* __module__ from the dotted name unless the dict sets it */
    r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __module__));
    if (r < 0)
        goto fail;
    if (r == 0 && dot) {
        mod = qp_str_from_utf8(ctx, spec->name, dot - spec->name);
        if (JS_IsException(mod))
            goto fail;
        r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), mod);
        JS_FreeValue(ctx, mod);
        if (r < 0)
            goto fail;
    }
    return v;
fail:
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* type_new                                                                  */
/* ========================================================================= */

/* str.isidentifier() */
static int qp_tw_is_identifier(JSContext *ctx, JSValueConst s)
{
    qp_strview v;
    Py_ssize_t i;
    int ok = 1;
    if (qp_str_view_acquire(ctx, s, &v) < 0)
        return -1;
    if (v.len == 0) {
        ok = 0;
    } else {
        Py_UCS4 c = QP_SL_READ(v.kind, v.data, 0);
        if (c != '_' && !_PyUnicode_IsXidStart(c))
            ok = 0;
        for (i = 1; ok && i < v.len; i++) {
            if (!_PyUnicode_IsXidContinue(QP_SL_READ(v.kind, v.data, i)))
                ok = 0;
        }
    }
    qp_str_view_release(ctx, &v);
    return ok;
}

static int qp_tw_str_has_nul(JSContext *ctx, JSValueConst s)
{
    /* PyUnicode_AsUTF8AndSize (UnicodeEncodeError for lone surrogates),
       then the strlen() check */
    size_t len;
    const char *u = qp_str_utf8(ctx, s, &len);
    int r;
    if (!u)
        return -1;
    r = strlen(u) != len;
    JS_FreeCString(ctx, u);
    return r;
}

/* Wrap dict[attr] in staticmethod/classmethod when it is a plain function. */
static int qp_type_new_wrap(JSContext *ctx, JSValueConst dict, JSAtom attr, int classmeth)
{
    JSValue f, w;
    int r = qp_dict_get_item_atom(ctx, dict, attr, &f);
    if (r <= 0)
        return r;
    if (!qp_is_py_function(f)) {
        JS_FreeValue(ctx, f);
        return 0;
    }
    w = classmeth ? qp_classmethod_new(ctx, f) : qp_staticmethod_new(ctx, f);
    JS_FreeValue(ctx, f);
    if (JS_IsException(w))
        return -1;
    r = qp_dict_set_item_atom(ctx, dict, attr, w);
    JS_FreeValue(ctx, w);
    return r;
}

static int qp_type_new_set_cell(JSContext *ctx, JSValueConst dict, JSAtom attr,
                                JSValueConst value, const char *what)
{
    JSValue cell;
    int r = qp_dict_get_item_atom(ctx, dict, attr, &cell);
    if (r <= 0)
        return r;
    if (!qp_typecheck(ctx, cell, QP_TYPE(ctx, cell))) {
        JSValueConst ct = QP_TYPE_VALUE(qp_type_of(ctx, cell));
        qp_raise_type_error(ctx, "%s must be a nonlocal cell, not %.200R", what, ct);
        JS_FreeValue(ctx, cell);
        return -1;
    }
    r = qp_cell_set(ctx, cell, value);
    JS_FreeValue(ctx, cell);
    if (r < 0)
        return -1;
    return qp_dict_del_item_atom(ctx, dict, attr) < 0 ? -1 : 0;
}

/* type_new_set_names: __set_name__ on every descriptor of the new class. */
static int qp_type_new_set_names(JSContext *ctx, QPType *t)
{
    JSValue names = qp_dict_copy(ctx, t->dict), k, v, set_name, res;
    Py_ssize_t pos = 0;
    if (JS_IsException(names))
        return -1;
    while (qp_dict_next(names, &pos, &k, &v)) {
        set_name = qp_lookup_special(ctx, v, QP_ATOM(ctx, __set_name__));
        if (QP_IS_NULL(set_name))
            continue;
        if (JS_IsException(set_name))
            goto fail;
        res = qp_call2(ctx, set_name, QP_TYPE_VALUE(t), k);
        JS_FreeValue(ctx, set_name);
        if (JS_IsException(res)) {
            JSValue exc = qp_exc_fetch(ctx), vn = qp_type_tp_name(ctx, qp_type_of(ctx, v));
            JSValue tn = qp_type_tp_name(ctx, t), note, nr;
            note = qp_str_from_format(ctx, "Error calling __set_name__ on '%.100U' instance %R in '%.100U'",
                                      vn, k, tn);
            JS_FreeValue(ctx, vn);
            JS_FreeValue(ctx, tn);
            if (!JS_IsException(note)) {
                JSValueConst a[1] = { note };
                nr = qp_call_method(ctx, exc, QP_ATOM(ctx, add_note), a, 1, QP_NULL);
                JS_FreeValue(ctx, note);
                if (JS_IsException(nr)) {
                    JS_FreeValue(ctx, exc);
                    goto fail;
                }
                JS_FreeValue(ctx, nr);
            } else {
                JS_FreeValue(ctx, exc);
                goto fail;
            }
            qp_raise_object(ctx, exc);
            goto fail;
        }
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, names);
    return 0;
fail:
    JS_FreeValue(ctx, names);
    return -1;
}

/* type_new_init_subclass: super(type, type).__init_subclass__(**kwds) */
static int qp_type_new_init_subclass(JSContext *ctx, QPType *t, JSValueConst kwds)
{
    JSValue func, res, empty;
    func = qp_do_super_lookup(ctx, QP_NULL, t, QP_TYPE_VALUE(t), t,
                              QP_ATOM(ctx, __init_subclass__), NULL);
    if (JS_IsException(func))
        return -1;
    empty = qp_tuple_new(ctx, 0);
    if (JS_IsException(empty)) {
        JS_FreeValue(ctx, func);
        return -1;
    }
    res = qp_call_tuple(ctx, func, empty, kwds);
    JS_FreeValue(ctx, empty);
    JS_FreeValue(ctx, func);
    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
    return 0;
}

/* Parse __slots__ (type_new_get_slots / type_new_slots_impl): returns the
   sorted mangled tuple (new ref) and updates add_dict/add_weak. */
static JSValue qp_type_new_slots(JSContext *ctx, JSValueConst name, JSValueConst dict,
                                 JSValueConst slots_in, int may_add_dict, int may_add_weak,
                                 int *add_dict, int *add_weak)
{
    JSValue slots, list = QP_NULL, res;
    Py_ssize_t i, n;
    JSAtom cls_atom = JS_ATOM_NULL;

    if (qp_str_check(ctx, slots_in)) {
        slots = qp_tuple_pack(ctx, 1, &slots_in);
    } else {
        slots = qp_tuple_from_iterable(ctx, slots_in);
    }
    if (JS_IsException(slots))
        return slots;
    n = QP_TUPLE_SIZE(slots);
    /* type_new_visit_slots */
    for (i = 0; i < n; i++) {
        JSValue s = QP_TUPLE_ITEMS(slots)[i];
        int r;
        if (!qp_str_check(ctx, s)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, s));
            qp_raise_type_error(ctx, "__slots__ items must be strings, not '%.200U'", tn);
            JS_FreeValue(ctx, tn);
            goto fail;
        }
        r = qp_tw_is_identifier(ctx, s);
        if (r < 0)
            goto fail;
        if (!r) {
            qp_raise_type_error(ctx, "__slots__ must be identifiers");
            goto fail;
        }
        {
            JSAtom a = qp_tw_name_to_atom(ctx, s);
            if (a == JS_ATOM_NULL)
                goto fail;
            if (a == QP_ATOM(ctx, __dict__)) {
                if (!may_add_dict || *add_dict != 0) {
                    JS_FreeAtom(ctx, a);
                    qp_raise_type_error(ctx, "__dict__ slot disallowed: we already got one");
                    goto fail;
                }
                (*add_dict)++;
            }
            if (a == QP_ATOM(ctx, __weakref__)) {
                if (!may_add_weak || *add_weak != 0) {
                    JS_FreeAtom(ctx, a);
                    qp_raise_type_error(ctx, "__weakref__ slot disallowed: we already got one");
                    goto fail;
                }
                (*add_weak)++;
            }
            JS_FreeAtom(ctx, a);
        }
    }
    /* type_new_copy_slots */
    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        goto fail;
    cls_atom = qp_tw_name_to_atom(ctx, name);
    if (cls_atom == JS_ATOM_NULL)
        goto fail;
    for (i = 0; i < n; i++) {
        JSAtom a = qp_tw_name_to_atom(ctx, QP_TUPLE_ITEMS(slots)[i]), m;
        qp_error err;
        JSValue ms;
        int r;
        if (a == JS_ATOM_NULL)
            goto fail;
        if ((*add_dict && a == QP_ATOM(ctx, __dict__)) ||
            (*add_weak && a == QP_ATOM(ctx, __weakref__))) {
            JS_FreeAtom(ctx, a);
            continue;
        }
        memset(&err, 0, sizeof(err));
        m = _Py_Mangle(cls_atom, a, ctx, &err);
        JS_FreeAtom(ctx, a);
        if (m == JS_ATOM_NULL) {
            if (!JS_HasException(ctx))
                qp_raise_overflow_error(ctx, "private identifier too large to be mangled");
            goto fail;
        }
        ms = qp_str_from_atom(ctx, m);
        if (JS_IsException(ms)) {
            JS_FreeAtom(ctx, m);
            goto fail;
        }
        r = qp_tw_dict_has(ctx, dict, m);
        if (r > 0 && m != QP_ATOM(ctx, __qualname__) && m != QP_ATOM(ctx, __classcell__) &&
            m != QP_ATOM(ctx, __classdictcell__)) {
            qp_raise_value_error(ctx, "%R in __slots__ conflicts with class variable", ms);
            r = -1;
        }
        JS_FreeAtom(ctx, m);
        if (r < 0 || qp_list_append(ctx, list, ms) < 0) {
            JS_FreeValue(ctx, ms);
            goto fail;
        }
        JS_FreeValue(ctx, ms);
    }
    JS_FreeAtom(ctx, cls_atom);
    cls_atom = JS_ATOM_NULL;
    if (qp_list_sort(ctx, list, QP_NULL, 0) < 0)
        goto fail;
    res = qp_list_as_tuple(ctx, list);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);
    return res;
fail:
    if (cls_atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, cls_atom);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);
    return JS_EXCEPTION;
}

static JSValue qp_type_new(JSContext *ctx, QPType *meta, JSValueConst name, JSValueConst bases_in,
                           JSValueConst ns, JSValueConst kwds)
{
    QPState *s = QP_STATE(ctx);
    QPType *base, *winner, *t = NULL;
    JSValue bases = QP_NULL, dict = QP_NULL, slots = QP_NULL, sv, tval = QP_NULL, tmp;
    int add_dict = 0, add_weak = 0, may_add_dict, may_add_weak, r;
    Py_ssize_t nbases, nslot = 0, i;

    if (!qp_str_check(ctx, name)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, name));
        qp_raise_type_error(ctx, "type.__new__() argument 1 must be str, not %U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    if (!qp_tuple_check(ctx, bases_in)) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, bases_in));
        qp_raise_type_error(ctx, "type.__new__() argument 2 must be tuple, not %U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    if (!qp_anydict_check(ctx, ns))
        return qp_raise_type_error(ctx, "type.__new__() argument 3 must be dict or frozendict, not %T", ns);

    /* type_new_get_bases */
    nbases = QP_TUPLE_SIZE(bases_in);
    if (nbases == 0) {
        JSValueConst o = QP_TYPE_VALUE(s->types[QP_T_object]);
        base = s->types[QP_T_object];
        bases = qp_tuple_pack(ctx, 1, &o);
        if (JS_IsException(bases))
            return bases;
    } else {
        for (i = 0; i < nbases; i++) {
            JSValue b = QP_TUPLE_ITEMS(bases_in)[i];
            if (qp_type_from_value(b))
                continue;
            r = qp_getattr_opt(ctx, b, QP_ATOM(ctx, __mro_entries__), &tmp);
            if (r < 0)
                return JS_EXCEPTION;
            if (r > 0) {
                JS_FreeValue(ctx, tmp);
                return qp_raise_type_error(ctx, "type() doesn't support MRO entry resolution; "
                                           "use types.new_class()");
            }
        }
        winner = qp_calculate_metaclass(ctx, meta, bases_in);
        if (!winner)
            return JS_EXCEPTION;
        if (winner != meta) {
            if (winner->slots.tp_new != qp_type_tp_new) {
                JSValueConst args[3 + 64];
                JSValue kwnames = QP_NULL, res;
                int nkw;
                if (!winner->slots.tp_new) {
                    JSValue tn = qp_type_tp_name(ctx, winner);
                    qp_raise_type_error(ctx, "cannot create '%.400U' instances", tn);
                    JS_FreeValue(ctx, tn);
                    return JS_EXCEPTION;
                }
                args[0] = name;
                args[1] = bases_in;
                args[2] = ns;
                if (!QP_IS_NULL(kwds) && qp_dict_size(kwds) > 64)
                    return qp_raise_type_error(ctx, "too many keyword arguments");
                nkw = qp_tw_dict_to_kw(ctx, kwds, args + 3, &kwnames);
                if (nkw < 0)
                    return JS_EXCEPTION;
                res = winner->slots.tp_new(ctx, winner, args, 3, kwnames);
                JS_FreeValue(ctx, kwnames);
                return res;
            }
            meta = winner;
        }
        base = qp_best_base(ctx, bases_in);
        if (!base)
            return JS_EXCEPTION;
        bases = JS_DupValue(ctx, bases_in);
    }

    /* type_new_init: copy of the namespace */
    dict = qp_dict_new(ctx);
    if (JS_IsException(dict))
        goto fail;
    if (qp_dict_merge(ctx, dict, ns, 1) < 0)
        goto fail;

    /* type_new_slots */
    /* CPython: base->tp_dictoffset == 0 (types, classmethod, ... keep
       their own dict) */
    may_add_dict = !(base->flags & QP_TPFLAGS_MANAGED_DICT) &&
        qp_class_shape_storage(base->instance_class_id);
    may_add_weak = !(base->flags & QP_TPFLAGS_WEAKREF);
    r = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __slots__), &sv);
    if (r < 0)
        goto fail;
    if (r == 0) {
        if (may_add_dict)
            add_dict++;
        if (may_add_weak && !(base->flags & QP_TPFLAGS_PRIV_VARSIZE))
            add_weak++;
    } else {
        slots = qp_type_new_slots(ctx, name, dict, sv, may_add_dict, may_add_weak,
                                  &add_dict, &add_weak);
        JS_FreeValue(ctx, sv);
        if (JS_IsException(slots))
            goto fail;
        nslot = QP_TUPLE_SIZE(slots);
        /* type_new_slots_bases: secondary bases may provide dict/weakref */
        if (nbases > 1 && ((may_add_dict && !add_dict) || (may_add_weak && !add_weak))) {
            for (i = 0; i < nbases; i++) {
                QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(bases)[i]);
                if (b == base)
                    continue;
                if (may_add_dict && !add_dict && (b->flags & QP_TPFLAGS_MANAGED_DICT))
                    add_dict++;
                if (may_add_weak && !add_weak && (b->flags & QP_TPFLAGS_WEAKREF))
                    add_weak++;
                if (may_add_dict && !add_dict)
                    continue;
                if (may_add_weak && !add_weak)
                    continue;
                break;
            }
        }
    }
    if (nslot > 0 && (base->flags & QP_TPFLAGS_PRIV_VARSIZE) &&
        !(base->flags & QP_TPFLAGS_TUPLE_SUBCLASS)) {
        JSValue tn = qp_type_tp_name(ctx, base);
        qp_raise_type_error(ctx, "arbitrary __slots__ not supported for subtype of '%U'", tn);
        JS_FreeValue(ctx, tn);
        goto fail;
    }

    /* type_new_alloc */
#ifdef CONFIG_PYTHON_CAPI
    if (qp_capi_snap_adopting() && qp_capi_snap_adopt_module()) {
        /* class made by the exec of the C module being restored */
        JSValue qn = QP_NULL;
        const char *s = NULL;
        char key[256];
        if (qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __qualname__), &qn) < 0)
            goto fail;
        s = qp_str_utf8(ctx, qp_str_check(ctx, qn) ? qn : name, NULL);
        if (!s) {
            JS_FreeValue(ctx, qn);
            goto fail;
        }
        snprintf(key, sizeof(key), "C:%s:%s", qp_capi_snap_adopt_module(), s);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, qn);
        t = qp_type_alloc_key(ctx, meta, key);
    } else
#endif
        t = qp_type_alloc(ctx, meta);
    if (!t)
        goto fail;
    tval = QP_TYPE_VALUE(t);
    t->flags = QP_TPFLAGS_HEAPTYPE | QP_TPFLAGS_BASETYPE;
    t->bases = bases;
    bases = QP_NULL;
    t->base = base;
    t->dict = dict;
    dict = QP_NULL;
    if (nslot > 0) {
        t->slot_names = slots;
        slots = QP_NULL;
    }
    /* type_new_set_name */
    r = qp_tw_str_has_nul(ctx, name);
    if (r < 0)
        goto fail;
    if (r) {
        qp_raise_value_error(ctx, "type name must not contain null characters");
        goto fail;
    }
    t->name = qp_tw_name_to_atom(ctx, name);
    if (t->name == JS_ATOM_NULL)
        goto fail;
    /* type_new_set_module */
    r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __module__));
    if (r < 0)
        goto fail;
    if (r == 0) {
        JSValue g = qp_frame_globals(qp_py_frame_at(ctx, 0));
        if (!QP_IS_NULL(g) && JS_VALUE_GET_TAG(g) == JS_TAG_OBJECT) {
            JSValue mod;
            r = qp_dict_get_item_atom(ctx, g, QP_ATOM(ctx, __name__), &mod);
            if (r < 0)
                goto fail;
            if (r > 0) {
                r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), mod);
                JS_FreeValue(ctx, mod);
                if (r < 0)
                    goto fail;
            }
        }
    }
    /* type_new_set_ht_name */
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __qualname__), &tmp);
    if (r < 0)
        goto fail;
    if (r > 0) {
        if (!qp_str_check(ctx, tmp)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, tmp));
            qp_raise_type_error(ctx, "type __qualname__ must be a str, not %U", tn);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, tmp);
            goto fail;
        }
        t->qualname = qp_str_value(ctx, tmp);
        JS_FreeValue(ctx, tmp);
        if (qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __qualname__)) < 0)
            goto fail;
    } else {
        t->qualname = JS_DupValue(ctx, name);
        if (!qp_str_check_exact(name)) {
            JS_FreeValue(ctx, t->qualname);
            t->qualname = qp_str_value(ctx, name);
        }
    }
    /* type_new_set_doc: a str __doc__ must be encodable to UTF-8 */
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __doc__), &tmp);
    if (r < 0)
        goto fail;
    if (r > 0) {
        if (qp_str_check(ctx, tmp)) {
            size_t len;
            const char *u = qp_str_utf8(ctx, tmp, &len);
            if (!u) {
                JS_FreeValue(ctx, tmp);
                goto fail;
            }
            JS_FreeCString(ctx, u);
        }
        JS_FreeValue(ctx, tmp);
    }
    /* special-case __new__, __init_subclass__, __class_getitem__ */
    if (qp_type_new_wrap(ctx, t->dict, QP_ATOM(ctx, __new__), 0) < 0 ||
        qp_type_new_wrap(ctx, t->dict, QP_ATOM(ctx, __init_subclass__), 1) < 0 ||
        qp_type_new_wrap(ctx, t->dict, QP_ATOM(ctx, __class_getitem__), 1) < 0)
        goto fail;
    /* type_new_descriptors */
    t->instance_class_id = base->instance_class_id;
    t->nslots = base->nslots + (uint16_t)nslot;
    for (i = 0; i < nslot; i++) {
        JSAtom a = qp_tw_name_to_atom(ctx, QP_TUPLE_ITEMS(t->slot_names)[i]);
        JSValue d;
        if (a == JS_ATOM_NULL)
            goto fail;
        d = qp_descr_new_member(ctx, t, a, (uint16_t)(base->nslots + i), 0);
        if (JS_IsException(d)) {
            JS_FreeAtom(ctx, a);
            goto fail;
        }
        r = qp_tw_dict_setdefault(ctx, t->dict, a, d);
        JS_FreeAtom(ctx, a);
        if (r < 0)
            goto fail;
    }
    if (add_weak)
        t->flags |= QP_TPFLAGS_WEAKREF;
    if (add_dict)
        t->flags |= QP_TPFLAGS_MANAGED_DICT;
    if (add_dict) {
        JSValue d = qp_descr_new_getset(ctx, s->types[QP_T_object], &qp_subtype_getsets[0]);
        if (JS_IsException(d) || qp_tw_dict_setdefault(ctx, t->dict, QP_ATOM(ctx, __dict__), d) < 0)
            goto fail;
    }
    if (add_weak) {
        JSValue d = qp_descr_new_getset(ctx, s->types[QP_T_object], &qp_subtype_getsets[1]);
        if (JS_IsException(d) || qp_tw_dict_setdefault(ctx, t->dict, QP_ATOM(ctx, __weakref__), d) < 0)
            goto fail;
    }
    /* __classcell__ / __classdictcell__ */
    if (qp_type_new_set_cell(ctx, t->dict, QP_ATOM(ctx, __classcell__), tval, "__classcell__") < 0)
        goto fail;
    {
        JSValue dd = JS_DupValue(ctx, t->dict);
        r = qp_type_new_set_cell(ctx, t->dict, QP_ATOM(ctx, __classdictcell__), dd,
                                 "__classdictcell__");
        JS_FreeValue(ctx, dd);
        if (r < 0)
            goto fail;
    }
    /* type_ready_publish(type, fix_slots=1) */
    if (qp_type_ready_common(ctx, t, NULL) < 0)
        goto fail;
    qp_fixup_slot_dispatchers(ctx, t);
    /* non-string keys */
    {
        Py_ssize_t pos = 0;
        JSValue k, v;
        while (qp_dict_next(t->dict, &pos, &k, &v)) {
            if (!qp_str_check(ctx, k)) {
                JSValue tn = qp_type_tp_name(ctx, t);
                const char *cs = JS_ToCString(ctx, tn);
                JS_FreeValue(ctx, tn);
                r = qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1,
                            "non-string key in the __dict__ of class %.200s", cs ? cs : "?");
                JS_FreeCString(ctx, cs);
                if (r < 0)
                    goto fail;
                break;
            }
        }
    }
    if (qp_type_new_set_names(ctx, t) < 0)
        goto fail;
    if (qp_type_new_init_subclass(ctx, t, kwds) < 0)
        goto fail;
    /* still set when __slots__ declared no slot (e.g. __slots__ = ()) */
    JS_FreeValue(ctx, slots);
    return tval;
fail:
    JS_FreeValue(ctx, bases);
    JS_FreeValue(ctx, dict);
    JS_FreeValue(ctx, slots);
    if (t)
        JS_FreeValue(ctx, tval);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* __class__ / __bases__ assignment compatibility                            */
/* ========================================================================= */

static BOOL qp_compatible_with_base(QPType *child)
{
    QPType *parent = child->base;
    if (!parent || child->instance_class_id != parent->instance_class_id ||
        child->nslots != parent->nslots)
        return FALSE;
#ifdef CONFIG_PYTHON_CAPI
    /* D5 (CapiB) R4: native instances: same C layout (compatible_with_tp_base) */
    if (child->instance_class_id == JS_CLASS_PY_NATIVE &&
        !qp_capi_native_same_layout(child, parent))
        return FALSE;
#endif
    return TRUE;
}

static int qp_same_slots_added(JSContext *ctx, QPType *a, QPType *b)
{
    int r;
    if (!(a->flags & QP_TPFLAGS_HEAPTYPE) || !(b->flags & QP_TPFLAGS_HEAPTYPE))
        return 0;
    if (a->nslots != b->nslots)
        return 0;
#ifdef CONFIG_PYTHON_CAPI
    /* D5 (CapiB) R4: native layouts must not add C fields (same_slots_added) */
    if (a->instance_class_id == JS_CLASS_PY_NATIVE &&
        (!a->base || !b->base || !qp_capi_native_same_layout(a, a->base) ||
         !qp_capi_native_same_layout(b, b->base)))
        return 0;
#endif
    if (JS_IsUndefined(a->slot_names) != JS_IsUndefined(b->slot_names))
        return 0;
    if (!JS_IsUndefined(a->slot_names)) {
        r = qp_richcompare_bool(ctx, a->slot_names, b->slot_names, QP_CMP_EQ);
        if (r != 1) {
            if (r < 0)
                qp_tw_clear_exc(ctx);
            return 0;
        }
    }
    return 1;
}

/* compatible_for_assignment: 1 ok, 0 error raised */
static int qp_compatible_for_assignment(JSContext *ctx, QPType *origto, QPType *newto,
                                        const char *attr, int setclass)
{
    QPType *oldto = setclass ? origto : origto->base, *newbase, *oldbase;
    const uint64_t layout = QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF;
    JSValue nn, on;

    if (setclass && newto->instance_class_id != oldto->instance_class_id) {
        nn = qp_type_tp_name(ctx, newto);
        on = qp_type_tp_name(ctx, oldto);
        qp_raise_type_error(ctx, "%s assignment: '%U' deallocator differs from '%U'", attr, nn, on);
        goto err;
    }
    if (setclass ? ((origto->flags & layout) != (newto->flags & layout))
                 : ((newto->flags & layout) & ~(origto->flags & layout)) != 0)
        goto differs;
    newbase = newto;
    oldbase = oldto;
    while (qp_compatible_with_base(newbase))
        newbase = newbase->base;
    while (qp_compatible_with_base(oldbase))
        oldbase = oldbase->base;
    if (newbase != oldbase &&
        (newbase->base != oldbase->base || !qp_same_slots_added(ctx, newbase, oldbase)))
        goto differs;
    return 1;
differs:
    nn = qp_type_tp_name(ctx, newto);
    on = qp_type_tp_name(ctx, oldto);
    qp_raise_type_error(ctx, "%s assignment: '%U' object layout differs from '%U'", attr, nn, on);
err:
    JS_FreeValue(ctx, nn);
    JS_FreeValue(ctx, on);
    return 0;
}

/* ========================================================================= */
/* The `type` type                                                           */
/* ========================================================================= */

/* check_set_special_type_attr */
static int qp_check_set_special(JSContext *ctx, QPType *t, JSValueConst value, const char *name)
{
    JSValue tn;
    if (t->flags & QP_TPFLAGS_IMMUTABLETYPE) {
        tn = qp_type_tp_name(ctx, t);
        qp_raise_type_error(ctx, "cannot set '%s' attribute of immutable type '%U'", name, tn);
        JS_FreeValue(ctx, tn);
        return 0;
    }
    if (QP_IS_NULL(value)) {
        tn = qp_type_tp_name(ctx, t);
        qp_raise_type_error(ctx, "cannot delete '%s' attribute of type '%U'", name, tn);
        JS_FreeValue(ctx, tn);
        return 0;
    }
    return 1;
}

static JSValue qp_type_get_name(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_type_short_name(ctx, qp_type_from_value(self));
}

static int qp_type_set_name(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSAtom a;
    int r;
    if (!qp_check_set_special(ctx, t, value, magic ? "__qualname__" : "__name__"))
        return -1;
    if (!qp_str_check(ctx, value)) {
        JSValue tn = qp_type_tp_name(ctx, t), vn = qp_type_tp_name(ctx, qp_type_of(ctx, value));
        qp_raise_type_error(ctx, "can only assign string to %U.%s, not '%U'", tn,
                            magic ? "__qualname__" : "__name__", vn);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, vn);
        return -1;
    }
    if (magic) {
        JSValue q = qp_str_value(ctx, value);
        if (JS_IsException(q))
            return -1;
        JS_FreeValue(ctx, t->qualname);
        t->qualname = q;
        return 0;
    }
    r = qp_tw_str_has_nul(ctx, value);
    if (r < 0)
        return -1;
    if (r) {
        qp_raise_value_error(ctx, "type name must not contain null characters");
        return -1;
    }
    a = qp_tw_name_to_atom(ctx, value);
    if (a == JS_ATOM_NULL)
        return -1;
    JS_FreeAtom(ctx, t->name);
    t->name = a;
    return 0;
}

static JSValue qp_type_get_qualname(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_type_qualname(ctx, qp_type_from_value(self));
}

static JSValue qp_type_get_bases(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, qp_type_from_value(self)->bases);
}

static JSValue qp_type_get_mro(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    if (JS_VALUE_GET_TAG(t->mro) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, t->mro);
}

/* Recompute the MRO and slots of t and its subclasses after __bases__
   assignment. */
static int qp_mro_hierarchy(JSContext *ctx, QPType *t)
{
    int i;
    if (qp_mro_internal(ctx, t) < 0)
        return -1;
    if (t->flags & QP_TPFLAGS_HEAPTYPE)
        qp_fixup_slot_dispatchers(ctx, t);
    for (i = 0; i < t->subclass_count; i++) {
        if (qp_mro_hierarchy(ctx, t->subclasses[i]) < 0)
            return -1;
    }
    return 0;
}

/* type_is_subtype_base_chain: reentrant __bases__ assignments (a custom
   mro() assigning __bases__) update tp_base before the MRO, so a cycle
   check through the MRO alone misses cycles of the tp_base chain. */
static BOOL qp_type_base_chain_has(QPType *b, QPType *t)
{
    if (JS_IsUndefined(b->mro))
        return FALSE;
    for (; b; b = b->base) {
        if (b == t)
            return TRUE;
    }
    return FALSE;
}

static int qp_type_set_bases(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self), *new_base, *old_base;
    JSValue old_bases, tn, vn;
    Py_ssize_t i, n;

    if (!qp_check_set_special(ctx, t, value, "__bases__"))
        return -1;
    if (!qp_tuple_check(ctx, value)) {
        tn = qp_type_tp_name(ctx, t);
        vn = qp_type_tp_name(ctx, qp_type_of(ctx, value));
        qp_raise_type_error(ctx, "can only assign tuple to %U.__bases__, not %U", tn, vn);
        goto err2;
    }
    n = QP_TUPLE_SIZE(value);
    if (n == 0) {
        tn = qp_type_tp_name(ctx, t);
        qp_raise_type_error(ctx, "can only assign non-empty tuple to %U.__bases__, not ()", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue o = QP_TUPLE_ITEMS(value)[i];
        QPType *b = qp_type_from_value(o);
        if (!b) {
            tn = qp_type_tp_name(ctx, t);
            vn = qp_type_tp_name(ctx, qp_type_of(ctx, o));
            qp_raise_type_error(ctx, "%U.__bases__ must be tuple of classes, not '%U'", tn, vn);
            goto err2;
        }
        if (qp_type_is_subtype(b, t) || qp_type_base_chain_has(b, t)) {
            qp_raise_type_error(ctx, "a __bases__ item causes an inheritance cycle");
            return -1;
        }
    }
    new_base = qp_best_base(ctx, value);
    if (!new_base)
        return -1;
    if (!qp_compatible_for_assignment(ctx, t, new_base, "__bases__", 0))
        return -1;
    old_bases = t->bases;
    old_base = t->base;
    t->bases = JS_DupValue(ctx, value);
    t->base = new_base;
    /* A custom mro() may assign __bases__ again (reentrance): the
       subclass links and the rollback then belong to the inner
       assignment, detected by t->bases no longer being `value`
       (type_set_bases_unlocked). */
    if (qp_mro_hierarchy(ctx, t) < 0) {
        JSValue exc = qp_exc_fetch(ctx);
        if (JS_VALUE_GET_PTR(t->bases) == JS_VALUE_GET_PTR(value)) {
            JS_FreeValue(ctx, t->bases);
            t->bases = old_bases;
            t->base = old_base;
            if (qp_mro_hierarchy(ctx, t) < 0)
                qp_tw_clear_exc(ctx);
        } else {
            JS_FreeValue(ctx, old_bases);
        }
        qp_raise_object(ctx, exc);
        return -1;
    }
    if (JS_VALUE_GET_PTR(t->bases) == JS_VALUE_GET_PTR(value)) {
        qp_type_unlink_bases(t);
        for (i = 0; i < n; i++) {
            if (qp_type_add_subclass(ctx, qp_type_from_value(QP_TUPLE_ITEMS(value)[i]), t) < 0) {
                JS_FreeValue(ctx, old_bases);
                return -1;
            }
        }
        qp_type_modified(ctx, t);
    }
    JS_FreeValue(ctx, old_bases);
    return 0;
err2:
    JS_FreeValue(ctx, tn);
    JS_FreeValue(ctx, vn);
    return -1;
}

static JSValue qp_type_get_module(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_type_module(ctx, qp_type_from_value(self));
}

/* __module__ / __doc__ setters: plain dict store. */
static int qp_type_set_dict_attr(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSAtom a = magic ? QP_ATOM(ctx, __doc__) : QP_ATOM(ctx, __module__);
    if (!qp_check_set_special(ctx, t, value, magic ? "__doc__" : "__module__"))
        return -1;
    qp_type_modified(ctx, t);
    if (!magic) {
        /* type_set_module: the class moved, its __firstlineno__ is stale */
        JSValue key = qp_str_from_atom(ctx, QP_ATOM(ctx, __firstlineno__)), old;
        int r;
        if (JS_IsException(key))
            return -1;
        r = qp_dict_pop(ctx, t->dict, key, &old);
        JS_FreeValue(ctx, key);
        if (r < 0)
            return -1;
        if (r > 0)
            JS_FreeValue(ctx, old);
    }
    return qp_dict_set_item_atom(ctx, t->dict, a, value);
}

static JSValue qp_type_get_abstractmethods(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue v;
    int r = 0;
    if (t != QP_TYPE(ctx, type))
        r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __abstractmethods__), &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        JSValue n = qp_str_from_atom(ctx, QP_ATOM(ctx, __abstractmethods__));
        qp_raise_value(ctx, QP_EXC(ctx, AttributeError), n);
        JS_FreeValue(ctx, n);
        return JS_EXCEPTION;
    }
    return v;
}

static int qp_type_set_abstractmethods(JSContext *ctx, JSValueConst self, JSValueConst value,
                                       int magic)
{
    QPType *t = qp_type_from_value(self);
    int abstract, r;
    if (!QP_IS_NULL(value)) {
        abstract = qp_bool(ctx, value);
        if (abstract < 0)
            return -1;
        r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __abstractmethods__), value);
    } else {
        abstract = 0;
        r = qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __abstractmethods__));
        if (r == 0) {
            JSValue n = qp_str_from_atom(ctx, QP_ATOM(ctx, __abstractmethods__));
            qp_raise_value(ctx, QP_EXC(ctx, AttributeError), n);
            JS_FreeValue(ctx, n);
            return -1;
        }
    }
    if (r < 0)
        return -1;
    qp_type_modified(ctx, t);
    if (abstract)
        t->flags |= QP_TPFLAGS_IS_ABSTRACT;
    else
        t->flags &= ~(uint64_t)QP_TPFLAGS_IS_ABSTRACT;
    return 0;
}

static JSValue qp_type_get_dict(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_mappingproxy_new(ctx, qp_type_from_value(self)->dict);
}

static JSValue qp_type_get_doc(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue res;
    QPDescrGetFunc *f;
    const char *doc = (t->flags & QP_TPFLAGS_HEAPTYPE) ? NULL : qp_capi_type_doc(t);
    int r;
    if (doc) {
        JSValue sn = qp_type_short_name(ctx, t);
        const char *s = JS_IsException(sn) ? NULL : JS_ToCString(ctx, sn);
        JS_FreeValue(ctx, sn);
        if (!s)
            return JS_EXCEPTION;
        res = qp_doc_from_internal(ctx, s, doc);
        JS_FreeCString(ctx, s);
        return res;
    }
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __doc__), &res);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    f = qp_type_of(ctx, res)->slots.tp_descr_get;
    if (f) {
        JSValue r2 = f(ctx, res, QP_NULL, self);
        JS_FreeValue(ctx, res);
        return r2;
    }
    return res;
}

static JSValue qp_type_get_text_signature(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue sn, res, hdoc = JS_UNDEFINED;
    const char *s, *doc = NULL, *heap_doc = NULL;
    int r;

    /* tp_doc: the C doc of builtin and C API types (PyType_FromSpec heap
       types keep the signature there, their __doc__ is stripped) */
    doc = qp_capi_type_doc(t);
    if (!doc && (t->flags & QP_TPFLAGS_HEAPTYPE)) {
        /* type_new_set_doc: a class's tp_doc is its str __doc__, which may
           carry a "Name(...)\n--\n\n" signature too */
        r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __doc__), &hdoc);
        if (r < 0)
            return JS_EXCEPTION;
        if (r > 0 && qp_str_check(ctx, hdoc)) {
            heap_doc = qp_str_utf8(ctx, hdoc, NULL);
            if (!heap_doc) {
                JS_FreeValue(ctx, hdoc);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, hdoc);
        doc = heap_doc;
    }
    if (!doc)
        return JS_UNDEFINED;
    sn = qp_type_short_name(ctx, t);
    s = JS_IsException(sn) ? NULL : JS_ToCString(ctx, sn);
    JS_FreeValue(ctx, sn);
    if (!s)
        res = JS_EXCEPTION;
    else
        res = qp_text_signature_from_internal(ctx, s, doc);
    if (s)
        JS_FreeCString(ctx, s);
    if (heap_doc)
        JS_FreeCString(ctx, heap_doc);
    return res;
}

static JSValue qp_type_no_attr(JSContext *ctx, QPType *t, const char *name)
{
    JSValue tn = qp_type_tp_name(ctx, t);
    qp_raise(ctx, QP_EXC(ctx, AttributeError), "type object '%U' has no attribute '%s'", tn, name);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

static JSValue qp_type_get_annotate(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue a;
    QPDescrGetFunc *f;
    int r;
    if (!(t->flags & QP_TPFLAGS_HEAPTYPE))
        return qp_type_no_attr(ctx, t, "__annotate__");
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate__), &a);
    if (r == 0)
        r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate_func__), &a);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        f = qp_type_of(ctx, a)->slots.tp_descr_get;
        if (f) {
            JSValue a2 = f(ctx, a, QP_NULL, self);
            JS_FreeValue(ctx, a);
            return a2;
        }
        return a;
    }
    if (qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate_func__), JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static int qp_tw_callable(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->slots.tp_call)
        return 1;
    return JS_IsFunction(ctx, v);
}

static int qp_type_set_annotate(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self);
    int r;
    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "cannot delete __annotate__ attribute");
        return -1;
    }
    if (t->flags & QP_TPFLAGS_IMMUTABLETYPE) {
        JSValue tn = qp_type_tp_name(ctx, t);
        qp_raise_type_error(ctx, "cannot set '__annotate__' attribute of immutable type '%U'", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    if (!JS_IsUndefined(value) && !qp_tw_callable(ctx, value)) {
        qp_raise_type_error(ctx, "__annotate__ must be callable or None");
        return -1;
    }
    r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate_func__), value);
    if (r >= 0 && !JS_IsUndefined(value))
        r = qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__));
    qp_type_modified(ctx, t);
    return r < 0 ? -1 : 0;
}

static JSValue qp_type_get_annotations(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue ann, annotate;
    QPDescrGetFunc *f;
    int r;
    if (!(t->flags & QP_TPFLAGS_HEAPTYPE))
        return qp_type_no_attr(ctx, t, "__annotations__");
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations__), &ann);
    if (r == 0)
        r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__), &ann);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        f = qp_type_of(ctx, ann)->slots.tp_descr_get;
        if (f) {
            JSValue a2 = f(ctx, ann, QP_NULL, self);
            JS_FreeValue(ctx, ann);
            return a2;
        }
        return ann;
    }
    annotate = qp_getattr(ctx, self, QP_ATOM(ctx, __annotate__));
    if (JS_IsException(annotate))
        return annotate;
    if (qp_tw_callable(ctx, annotate)) {
        JSValue one = JS_NewInt32(ctx, 1);
        ann = qp_call1(ctx, annotate, one);
        if (JS_IsException(ann)) {
            JS_FreeValue(ctx, annotate);
            return ann;
        }
        if (!qp_dict_check(ctx, ann)) {
            qp_raise_type_error(ctx, "__annotate__() must return a dict, not %T", ann);
            JS_FreeValue(ctx, ann);
            JS_FreeValue(ctx, annotate);
            return JS_EXCEPTION;
        }
    } else {
        ann = qp_dict_new(ctx);
    }
    JS_FreeValue(ctx, annotate);
    if (JS_IsException(ann))
        return ann;
    if (qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__), ann) < 0) {
        JS_FreeValue(ctx, ann);
        return JS_EXCEPTION;
    }
    qp_type_modified(ctx, t);
    return ann;
}

static int qp_type_set_annotations(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self);
    int r;
    if (t->flags & QP_TPFLAGS_IMMUTABLETYPE) {
        JSValue tn = qp_type_tp_name(ctx, t);
        qp_raise_type_error(ctx, "cannot set '__annotations__' attribute of immutable type '%U'", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    r = qp_tw_dict_has(ctx, t->dict, QP_ATOM(ctx, __annotations__));
    if (r < 0)
        return -1;
    if (r) {
        if (!QP_IS_NULL(value)) {
            r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations__), value);
        } else {
            r = qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations__));
            if (r == 0)
                goto missing;
        }
        if (r < 0)
            return -1;
        r = qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__));
    } else {
        if (!QP_IS_NULL(value)) {
            r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__), value);
        } else {
            r = qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotations_cache__));
            if (r == 0)
                goto missing;
        }
    }
    if (r < 0)
        return -1;
    if (qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate_func__)) < 0 ||
        qp_dict_del_item_atom(ctx, t->dict, QP_ATOM(ctx, __annotate__)) < 0)
        return -1;
    qp_type_modified(ctx, t);
    return 0;
missing:
    {
        JSValue n = qp_str_from_cstr(ctx, "__annotations__");
        qp_raise_value(ctx, QP_EXC(ctx, AttributeError), n);
        JS_FreeValue(ctx, n);
    }
    return -1;
}

static JSValue qp_type_get_type_params(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue v;
    int r;
    if (t == QP_TYPE(ctx, type))
        return qp_tuple_new(ctx, 0);
    r = qp_dict_get_item_atom(ctx, t->dict, QP_ATOM(ctx, __type_params__), &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_tuple_new(ctx, 0);
    return v;
}

static int qp_type_set_type_params(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *t = qp_type_from_value(self);
    if (!qp_check_set_special(ctx, t, value, "__type_params__"))
        return -1;
    qp_type_modified(ctx, t);
    return qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __type_params__), value);
}

static JSValue qp_type_get_base(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    return t->base ? qp_type_dup(ctx, t->base) : JS_UNDEFINED;
}

static JSValue qp_type_get_flags(JSContext *ctx, JSValueConst self, int magic)
{
    QPType *t = qp_type_from_value(self);
    return qp_long_from_int64(ctx, (int64_t)(t->flags & 0xFFFFFFFFu));
}

static void qp_type_layout(JSContext *ctx, QPType *t, QPTypeLayout *l)
{
    QPType *b = t;
    /* the layout of the nearest type with a known one (spec sizes, C-API
       PyTypeObject, else object) plus one pointer per slot added below
       it, as class statements do for __slots__ (type_new_descriptors) */
    for (;;) {
        if (b->spec && b->spec->basicsize) {
            l->basicsize = b->spec->basicsize;
            l->itemsize = b->spec->itemsize;
            l->dictoffset = l->weaklistoffset = 0;
            break;
        }
#ifdef CONFIG_PYTHON_CAPI
        if (qp_capi_type_layout(ctx, b, l))
            break;
#endif
        if (!b->base) {
            l->basicsize = 2 * sizeof(void *);   /* sizeof(PyObject) */
            l->itemsize = l->dictoffset = l->weaklistoffset = 0;
            break;
        }
        b = b->base;
    }
    l->basicsize += (Py_ssize_t)sizeof(void *) * (t->nslots - b->nslots);
    /* a dict / weakref list not at a known C offset is managed, as
       type_new_descriptors adds them: tp_dictoffset -1,
       tp_weaklistoffset MANAGED_WEAKREF_OFFSET */
    if (!l->dictoffset && (t->flags & QP_TPFLAGS_MANAGED_DICT))
        l->dictoffset = -1;
    if (!l->weaklistoffset && (t->flags & QP_TPFLAGS_WEAKREF))
        l->weaklistoffset = -4 * (Py_ssize_t)sizeof(void *);
}

static JSValue qp_type_get_layout(JSContext *ctx, JSValueConst self, int magic)
{
    QPTypeLayout l;
    Py_ssize_t v;
    qp_type_layout(ctx, qp_type_from_value(self), &l);
    switch (magic) {
    case 0: v = l.basicsize; break;
    case 1: v = l.itemsize; break;
    case 2: v = l.dictoffset; break;
    default: v = l.weaklistoffset; break;
    }
    return qp_long_from_int64(ctx, v);
}

static const QPGetSetDef qp_type_getsets[] = {
    { "__name__", qp_type_get_name, qp_type_set_name, 0, NULL },
    { "__qualname__", qp_type_get_qualname, qp_type_set_name, 1, NULL },
    { "__bases__", qp_type_get_bases, qp_type_set_bases, 0, NULL },
    { "__mro__", qp_type_get_mro, NULL, 0, NULL },
    { "__module__", qp_type_get_module, qp_type_set_dict_attr, 0, NULL },
    { "__abstractmethods__", qp_type_get_abstractmethods, qp_type_set_abstractmethods, 0, NULL },
    { "__dict__", qp_type_get_dict, NULL, 0, NULL },
    { "__doc__", qp_type_get_doc, qp_type_set_dict_attr, 1, NULL },
    { "__text_signature__", qp_type_get_text_signature, NULL, 0, NULL },
    { "__annotations__", qp_type_get_annotations, qp_type_set_annotations, 0, NULL },
    { "__annotate__", qp_type_get_annotate, qp_type_set_annotate, 0, NULL },
    { "__type_params__", qp_type_get_type_params, qp_type_set_type_params, 0, NULL },
    { "__base__", qp_type_get_base, NULL, 0, NULL },
    { "__flags__", qp_type_get_flags, NULL, 0, NULL },
    { "__basicsize__", qp_type_get_layout, NULL, 0, NULL },
    { "__itemsize__", qp_type_get_layout, NULL, 1, NULL },
    { "__dictoffset__", qp_type_get_layout, NULL, 2, NULL },
    { "__weakrefoffset__", qp_type_get_layout, NULL, 3, NULL },
    { NULL }
};

static JSValue qp_type_mro_meth(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    JSValue seq = qp_mro_implementation(ctx, qp_type_from_value(self)), l;
    if (JS_IsException(seq))
        return seq;
    l = qp_list_from_iterable(ctx, seq);
    JS_FreeValue(ctx, seq);
    return l;
}

static JSValue qp_type_subclasses_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_from_value(self);
    JSValue l = qp_list_new(ctx, 0);
    int i;
    if (JS_IsException(l))
        return l;
    for (i = 0; i < t->subclass_count; i++) {
        if (qp_list_append(ctx, l, QP_TYPE_VALUE(t->subclasses[i])) < 0) {
            JS_FreeValue(ctx, l);
            return JS_EXCEPTION;
        }
    }
    return l;
}

static JSValue qp_type_instancecheck_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    int r = magic ? qp_object_real_issubclass(ctx, args[0], self)
                  : qp_object_real_isinstance(ctx, args[0], self);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

static JSValue qp_type_prepare_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    return qp_dict_new(ctx);
}

/* merge_class_dict: dict |= aclass.__dict__ and recursively its bases. */
static int qp_merge_class_dict(JSContext *ctx, JSValueConst dict, JSValueConst aclass)
{
    JSValue cd, bases, base, nv;
    Py_ssize_t i, n;
    int r = qp_getattr_opt(ctx, aclass, QP_ATOM(ctx, __dict__), &cd);
    if (r < 0)
        return -1;
    if (r > 0) {
        r = qp_dict_update(ctx, dict, cd);
        JS_FreeValue(ctx, cd);
        if (r < 0)
            return -1;
    }
    r = qp_getattr_opt(ctx, aclass, QP_ATOM(ctx, __bases__), &bases);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    n = qp_len(ctx, bases);
    if (n < 0) {
        JS_FreeValue(ctx, bases);
        return -1;
    }
    for (i = 0; i < n; i++) {
        nv = JS_NewInt32(ctx, (int32_t)i);
        base = qp_getitem(ctx, bases, nv);
        if (JS_IsException(base)) {
            JS_FreeValue(ctx, bases);
            return -1;
        }
        r = qp_merge_class_dict(ctx, dict, base);
        JS_FreeValue(ctx, base);
        if (r < 0) {
            JS_FreeValue(ctx, bases);
            return -1;
        }
    }
    JS_FreeValue(ctx, bases);
    return 0;
}

static JSValue qp_type_dir_meth(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    JSValue d = qp_dict_new(ctx), res;
    if (JS_IsException(d))
        return d;
    if (qp_merge_class_dict(ctx, d, self) < 0) {
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    res = qp_dict_keys_list(ctx, d);
    JS_FreeValue(ctx, d);
    return res;
}

static JSValue qp_type_sizeof_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_from_value(self);
    return JS_NewInt32(ctx, (t->flags & QP_TPFLAGS_HEAPTYPE) ? 928 : 416);
}

static const QPMethodDef qp_type_methods[] = {
    { "mro", qp_type_mro_meth, QP_METH_NOARGS, 0,
      "mro($self, /)\n--\n\nReturn a type's method resolution order." },
    { "__subclasses__", qp_type_subclasses_meth, QP_METH_NOARGS, 0,
      "__subclasses__($self, /)\n--\n\nReturn a list of immediate subclasses." },
    { "__prepare__", qp_type_prepare_meth, QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 0,
      "__prepare__($cls, name, bases, /, **kwds)\n--\n\n"
      "Create the namespace for the class statement" },
    { "__instancecheck__", qp_type_instancecheck_meth, QP_METH_O, 0,
      "__instancecheck__($self, instance, /)\n--\n\nCheck if an object is an instance." },
    { "__subclasscheck__", qp_type_instancecheck_meth, QP_METH_O, 1,
      "__subclasscheck__($self, subclass, /)\n--\n\nCheck if a class is a subclass." },
    { "__dir__", qp_type_dir_meth, QP_METH_NOARGS, 0,
      "__dir__($self, /)\n--\n\nSpecialized __dir__ implementation for types." },
    { "__sizeof__", qp_type_sizeof_meth, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nReturn memory consumption of the type object." },
    { NULL }
};

static JSValue qp_type_repr(JSContext *ctx, JSValueConst self)
{
    QPType *t = qp_type_from_value(self);
    JSValue mod, name, res;
    if (!t || (!t->tp_name && t->name == JS_ATOM_NULL))
        return qp_str_from_format(ctx, "<class at %p>", qp_obj_addr(self));
    mod = qp_type_module(ctx, t);
    if (JS_IsException(mod)) {
        qp_tw_clear_exc(ctx);
        mod = QP_NULL;
    } else if (!qp_str_check(ctx, mod)) {
        JS_FreeValue(ctx, mod);
        mod = QP_NULL;
    }
    name = qp_type_qualname(ctx, t);
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, mod);
        return name;
    }
    if (!QP_IS_NULL(mod) && !qp_str_check_exact(mod)) {
        JSValue m2 = qp_str_value(ctx, mod);
        JS_FreeValue(ctx, mod);
        mod = m2;
    }
    if (!QP_IS_NULL(mod) && qp_str_check_exact(mod) && !qp_tw_str_is(ctx, mod, "builtins")) {
        res = qp_str_from_format(ctx, "<class '%U.%U'>", mod, name);
    } else {
        JSValue tn = qp_type_tp_name(ctx, t);
        res = qp_str_from_format(ctx, "<class '%U'>", tn);
        JS_FreeValue(ctx, tn);
    }
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    return res;
}

static JSValue qp_type_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                            size_t nargsf, JSValueConst kwnames)
{
    QPType *type = qp_type_from_value(callable), *ot;
    Py_ssize_t nargs = qp_nargs(nargsf), nkw = qp_kwcount(kwnames);
    JSValue obj;

    if (!type)
        return qp_raise_type_error(ctx, "'%T' object is not callable", callable);
    if (type->slots.tp_vectorcall)
        return type->slots.tp_vectorcall(ctx, callable, args, nargsf, kwnames);
    if (type == QP_TYPE(ctx, type)) {
        if (nargs == 1 && nkw == 0)
            return qp_type_dup(ctx, qp_type_of(ctx, args[0]));
        if (nargs != 3)
            return qp_raise_type_error(ctx, "type() takes 1 or 3 arguments");
    }
    if (!type->slots.tp_new) {
        JSValue tn = qp_type_tp_name(ctx, type);
        qp_raise_type_error(ctx, "cannot create '%U' instances", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    obj = type->slots.tp_new(ctx, type, args, nargs, nkw ? kwnames : QP_NULL);
    if (JS_IsException(obj))
        return obj;
    if (!qp_typecheck(ctx, obj, type))
        return obj;
    ot = qp_type_of(ctx, obj);
    if (ot->slots.tp_init) {
        if (ot->slots.tp_init(ctx, obj, args, nargs, nkw ? kwnames : QP_NULL) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

static int qp_type_init(JSContext *ctx, JSValueConst self, JSValueConst *args, size_t nargsf,
                        JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    if (qp_kwcount(kwnames) > 0 && nargs == 1) {
        qp_raise_type_error(ctx, "type.__init__() takes no keyword arguments");
        return -1;
    }
    if (nargs != 1 && nargs != 3) {
        qp_raise_type_error(ctx, "type.__init__() takes 1 or 3 arguments");
        return -1;
    }
    return 0;
}

static JSValue qp_type_tp_new(JSContext *ctx, QPType *meta, JSValueConst *args, size_t nargsf,
                              JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf), nkw = qp_kwcount(kwnames), i;
    JSValue kwds = QP_NULL, res;
    if (nargs != 3)
        return qp_raise_type_error(ctx, "type.__new__() takes exactly 3 arguments (%d given)",
                                   (int)nargs);
    if (nkw > 0) {
        kwds = qp_dict_new(ctx);
        if (JS_IsException(kwds))
            return kwds;
        for (i = 0; i < nkw; i++) {
            if (qp_dict_set_item(ctx, kwds, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0) {
                JS_FreeValue(ctx, kwds);
                return JS_EXCEPTION;
            }
        }
    }
    res = qp_type_new(ctx, meta, args[0], args[1], args[2], kwds);
    JS_FreeValue(ctx, kwds);
    return res;
}

static JSValue qp_type_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPType *type = qp_type_from_value(obj), *meta = qp_type_of(ctx, obj), *mt;
    JSValue meta_attr, attr, res;
    QPDescrGetFunc *meta_get = NULL, *local_get;

    meta_attr = qp_type_lookup(ctx, meta, name);
    if (!QP_IS_NULL(meta_attr)) {
        meta_attr = JS_DupValue(ctx, meta_attr);
        mt = qp_type_of(ctx, meta_attr);
        meta_get = mt->slots.tp_descr_get;
        if (meta_get && mt->slots.tp_descr_set) {
            res = meta_get(ctx, meta_attr, obj, QP_TYPE_VALUE(meta));
            JS_FreeValue(ctx, meta_attr);
            return res;
        }
    }
    attr = qp_type_lookup(ctx, type, name);
    if (!QP_IS_NULL(attr)) {
        attr = JS_DupValue(ctx, attr);
        JS_FreeValue(ctx, meta_attr);
        local_get = qp_type_of(ctx, attr)->slots.tp_descr_get;
        if (local_get) {
            res = local_get(ctx, attr, QP_NULL, obj);
            JS_FreeValue(ctx, attr);
            return res;
        }
        return attr;
    }
    if (meta_get) {
        res = meta_get(ctx, meta_attr, obj, QP_TYPE_VALUE(meta));
        JS_FreeValue(ctx, meta_attr);
        return res;
    }
    if (!QP_IS_NULL(meta_attr))
        return meta_attr;
    return qp_raise_attribute_error(ctx, obj, name);
}

static int qp_type_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPType *type = qp_type_from_value(obj), *meta = qp_type_of(ctx, obj);
    JSValue descr;
    int r;

    if (type->flags & QP_TPFLAGS_IMMUTABLETYPE) {
        JSValue n = qp_str_from_atom(ctx, name), tn = qp_type_tp_name(ctx, type);
        qp_raise_type_error(ctx, "cannot set %R attribute of immutable type '%U'", n, tn);
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    descr = qp_type_lookup(ctx, meta, name);
    if (!QP_IS_NULL(descr)) {
        QPDescrSetFunc *f = qp_type_of(ctx, descr)->slots.tp_descr_set;
        if (f) {
            descr = JS_DupValue(ctx, descr);
            r = f(ctx, descr, obj, v);
            JS_FreeValue(ctx, descr);
            return r;
        }
    }
    if (JS_VALUE_GET_TAG(type->dict) != JS_TAG_OBJECT) {
        type->dict = qp_dict_new(ctx);
        if (JS_IsException(type->dict)) {
            type->dict = JS_UNDEFINED;
            return -1;
        }
    }
    /* type_update_dict: invalidate caches before the old value can go */
    qp_type_modified(ctx, type);
    if (QP_IS_NULL(v)) {
        r = qp_dict_del_item_atom(ctx, type->dict, name);
        if (r == 0) {
            JSValue tn = qp_type_tp_name(ctx, type), n = qp_str_from_atom(ctx, name);
            qp_raise(ctx, QP_EXC(ctx, AttributeError), "type object '%.50U' has no attribute '%U'",
                     tn, n);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, n);
            qp_obj_set_attribute_error_context(ctx, obj, name);
            return -1;
        }
    } else {
        r = qp_dict_set_item_atom(ctx, type->dict, name, v);
    }
    if (r < 0)
        return -1;
    qp_type_modified(ctx, type);
    if (qp_is_slot_name(ctx, name))
        qp_update_slot(ctx, type, name);
    return 0;
}

static JSValue qp_type_or(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return qp_union_new(ctx, a, b);
}

static const QPTypeSpec qp_type_spec = {
    .name = "type",
    .doc = "type(object) -> the object's type\n"
           "type(name, bases, dict, **kwds) -> a new type",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_TYPE_SUBCLASS |
             QP_TPFLAGS_WEAKREF | QP_TPFLAGS_PRIV_VARSIZE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_TYPE,
    .slots = {
        .tp_getattro = qp_type_getattro,
        .tp_setattro = qp_type_setattro,
        .tp_repr = qp_type_repr,
        .tp_call = qp_type_call,
        .tp_init = qp_type_init,
        .tp_new = qp_type_tp_new,
        .nb = { .nb_or = qp_type_or },
    },
    .methods = qp_type_methods,
    .getsets = qp_type_getsets,
};

/* ========================================================================= */
/* The `object` type                                                         */
/* ========================================================================= */

static inline int qp_excess_args(Py_ssize_t nargs, JSValueConst kwnames)
{
    return nargs > 0 || qp_kwcount(kwnames) > 0;
}

static int qp_object_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    QPType *type = qp_type_of(ctx, self);
    if (qp_excess_args(qp_nargs(nargsf), kwnames)) {
        if (type->slots.tp_init != qp_object_init) {
            qp_raise_type_error(ctx, "object.__init__() takes exactly one argument "
                                "(the instance to initialize)");
            return -1;
        }
        if (type->slots.tp_new == qp_object_new) {
            JSValue tn = qp_type_tp_name(ctx, type);
            qp_raise_type_error(ctx, "%.200U.__init__() takes exactly one argument "
                                "(the instance to initialize)", tn);
            JS_FreeValue(ctx, tn);
            return -1;
        }
    }
    return 0;
}

static JSValue qp_object_new(JSContext *ctx, QPType *type, JSValueConst *args, size_t nargsf,
                             JSValueConst kwnames)
{
    if (qp_excess_args(qp_nargs(nargsf), kwnames)) {
        if (type->slots.tp_new != qp_object_new)
            return qp_raise_type_error(ctx, "object.__new__() takes exactly one argument "
                                       "(the type to instantiate)");
        if (type->slots.tp_init == qp_object_init) {
            JSValue tn = qp_type_tp_name(ctx, type);
            qp_raise_type_error(ctx, "%.200U() takes no arguments", tn);
            JS_FreeValue(ctx, tn);
            return JS_EXCEPTION;
        }
    }
    if (type->flags & QP_TPFLAGS_IS_ABSTRACT) {
        JSValue am, lst, sep, joined, tn;
        Py_ssize_t n;
        am = qp_type_get_abstractmethods(ctx, QP_TYPE_VALUE(type), 0);
        if (JS_IsException(am))
            return am;
        lst = qp_list_from_iterable(ctx, am);
        JS_FreeValue(ctx, am);
        if (JS_IsException(lst))
            return lst;
        if (qp_list_sort(ctx, lst, QP_NULL, 0) < 0) {
            JS_FreeValue(ctx, lst);
            return JS_EXCEPTION;
        }
        sep = qp_str_from_cstr(ctx, "', '");
        {
            JSValue *items = qp_list_items(lst, &n);
            joined = qp_str_join_array(ctx, sep, (int)n, (JSValueConst *)items);
        }
        JS_FreeValue(ctx, sep);
        JS_FreeValue(ctx, lst);
        if (JS_IsException(joined))
            return joined;
        tn = qp_type_tp_name(ctx, type);
        qp_raise_type_error(ctx, "Can't instantiate abstract class %U without an implementation "
                            "for abstract method%s '%U'", tn, n > 1 ? "s" : "", joined);
        JS_FreeValue(ctx, tn);
        JS_FreeValue(ctx, joined);
        return JS_EXCEPTION;
    }
    return qp_object_new_instance(ctx, type, type->instance_class_id);
}

static JSValue qp_object_repr(JSContext *ctx, JSValueConst self)
{
    QPType *type = qp_type_of(ctx, self);
    JSValue mod = qp_type_module(ctx, type), name, res;
    if (JS_IsException(mod)) {
        qp_tw_clear_exc(ctx);
        mod = QP_NULL;
    } else if (!qp_str_check(ctx, mod)) {
        JS_FreeValue(ctx, mod);
        mod = QP_NULL;
    }
    name = qp_type_qualname(ctx, type);
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, mod);
        return name;
    }
    if (!QP_IS_NULL(mod) && qp_str_check_exact(mod) && !qp_tw_str_is(ctx, mod, "builtins")) {
        res = qp_str_from_format(ctx, "<%U.%U object at %p>", mod, name, qp_obj_addr(self));
    } else {
        JSValue tn = qp_type_tp_name(ctx, type);
        res = qp_str_from_format(ctx, "<%U object at %p>", tn, qp_obj_addr(self));
        JS_FreeValue(ctx, tn);
    }
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    return res;
}

static JSValue qp_object_str(JSContext *ctx, JSValueConst self)
{
    QPUnaryFunc *f = qp_type_of(ctx, self)->slots.tp_repr;
    if (!f)
        f = qp_object_repr;
    return f(ctx, self);
}

static Py_hash_t qp_object_hash(JSContext *ctx, JSValueConst v)
{
    return (Py_hash_t)qp_hash_pointer((uintptr_t)qp_obj_addr(v));
}

static JSValue qp_object_richcompare(JSContext *ctx, JSValueConst self, JSValueConst other, int op)
{
    JSValue res;
    QPRichCmpFunc *f;
    int ok;
    switch (op & QP_CMP_OP_MASK) {
    case QP_CMP_EQ:
        if (qp_is(self, other))
            return JS_TRUE;
        return qp_not_implemented(ctx);
    case QP_CMP_NE:
        f = qp_type_of(ctx, self)->slots.tp_richcompare;
        if (!f)
            return qp_not_implemented(ctx);
        res = f(ctx, self, other, QP_CMP_EQ);
        if (!JS_IsException(res) && !qp_is_notimpl(ctx, res)) {
            ok = qp_bool(ctx, res);
            JS_FreeValue(ctx, res);
            if (ok < 0)
                return JS_EXCEPTION;
            return JS_NewBool(ctx, !ok);
        }
        return res;
    default:
        return qp_not_implemented(ctx);
    }
}

static JSValue qp_object_get_class(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_type_dup(ctx, qp_type_of(ctx, self));
}

static int qp_object_set_class(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPType *newto, *oldto;
    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "can't delete __class__ attribute");
        return -1;
    }
    newto = qp_type_from_value(value);
    if (!newto) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, value));
        qp_raise_type_error(ctx, "__class__ must be set to a class, not '%U' object", tn);
        JS_FreeValue(ctx, tn);
        return -1;
    }
    oldto = qp_type_of(ctx, self);
    if (!(qp_type_is_subtype(newto, QP_TYPE(ctx, module)) &&
          qp_type_is_subtype(oldto, QP_TYPE(ctx, module))) &&
        ((newto->flags & QP_TPFLAGS_IMMUTABLETYPE) || (oldto->flags & QP_TPFLAGS_IMMUTABLETYPE))) {
        qp_raise_type_error(ctx, "__class__ assignment only supported for mutable types "
                            "or ModuleType subclasses");
        return -1;
    }
    if (JS_VALUE_GET_TAG(self) != JS_TAG_OBJECT) {
        qp_raise_type_error(ctx, "__class__ assignment only supported for mutable types "
                            "or ModuleType subclasses");
        return -1;
    }
    if (!qp_compatible_for_assignment(ctx, oldto, newto, "__class__", 1))
        return -1;
    if (JS_SetPrototypeInternal(ctx, self, QP_TYPE_VALUE(newto), FALSE) < 0)
        return -1;
    return 0;
}

static const QPGetSetDef qp_object_getsets[] = {
    { "__class__", qp_object_get_class, qp_object_set_class, 0,
      "the object's class" },
    { NULL }
};

/* object.__getstate__ default (object_getstate_default) */
static JSValue qp_object_getstate_default(JSContext *ctx, JSValueConst obj, int required)
{
    QPType *tp = qp_type_of(ctx, obj);
    JSValue state, slots = QP_NULL, v;
    Py_ssize_t i, j, n;
    int r, nfound = 0;

    if (required && (tp->instance_class_id != JS_CLASS_PY_OBJECT ||
                     (!(tp->flags & QP_TPFLAGS_HEAPTYPE) && tp != QP_TYPE(ctx, object)))) {
        JSValue tn = qp_type_tp_name(ctx, tp);
        qp_raise_type_error(ctx, "cannot pickle '%.200U' object", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    state = JS_UNDEFINED;
    if (qp_obj_has_storage(ctx, tp, obj)) {
        JSValue d = qp_object_get_dict(ctx, obj);
        if (JS_IsException(d))
            return d;
        if (qp_dict_size(d) > 0) {
            state = qp_dict_copy(ctx, d);
            JS_FreeValue(ctx, d);
            if (JS_IsException(state))
                return state;
        } else {
            JS_FreeValue(ctx, d);
        }
    }
    /* copyreg._slotnames(cls): __slots__ of the MRO (mangled, no __dict__/__weakref__) */
    if (JS_VALUE_GET_TAG(tp->mro) == JS_TAG_OBJECT) {
        n = QP_TUPLE_SIZE(tp->mro);
        for (i = 0; i < n; i++) {
            QPType *b = qp_type_from_value(QP_TUPLE_ITEMS(tp->mro)[i]);
            if (!b || JS_IsUndefined(b->slot_names))
                continue;
            for (j = 0; j < QP_TUPLE_SIZE(b->slot_names); j++) {
                JSValue nm = QP_TUPLE_ITEMS(b->slot_names)[j];
                JSAtom a = qp_tw_name_to_atom(ctx, nm);
                if (a == JS_ATOM_NULL)
                    goto fail;
                r = qp_getattr_opt(ctx, obj, a, &v);
                JS_FreeAtom(ctx, a);
                if (r < 0)
                    goto fail;
                if (r == 0)
                    continue;
                if (QP_IS_NULL(slots)) {
                    slots = qp_dict_new(ctx);
                    if (JS_IsException(slots)) {
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                }
                r = qp_dict_set_item(ctx, slots, nm, v);
                JS_FreeValue(ctx, v);
                if (r < 0)
                    goto fail;
                nfound++;
            }
        }
    }
    if (nfound > 0) {
        JSValueConst pair[2] = { state, slots };
        JSValue t2 = qp_tuple_pack(ctx, 2, pair);
        JS_FreeValue(ctx, state);
        JS_FreeValue(ctx, slots);
        return t2;
    }
    JS_FreeValue(ctx, slots);
    return state;
fail:
    JS_FreeValue(ctx, state);
    JS_FreeValue(ctx, slots);
    return JS_EXCEPTION;
}

static JSValue qp_object_getstate_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_object_getstate_default(ctx, self, 0);
}

static JSValue qp_object_getstate(JSContext *ctx, JSValueConst obj, int required)
{
    JSAtom a = JS_NewAtom(ctx, "__getstate__");
    JSValue gs = qp_getattr(ctx, obj, a), st;
    JS_FreeAtom(ctx, a);
    if (JS_IsException(gs))
        return gs;
    if (JS_VALUE_GET_TAG(gs) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(gs)->class_id == JS_CLASS_PY_CFUNCTION &&
        ((QPCFunctionObject *)JS_VALUE_GET_OBJ(gs)->u.opaque)->ml->meth == qp_object_getstate_meth &&
        qp_is(((QPCFunctionObject *)JS_VALUE_GET_OBJ(gs)->u.opaque)->self, obj)) {
        st = qp_object_getstate_default(ctx, obj, required);
    } else {
        st = qp_call0(ctx, gs);
    }
    JS_FreeValue(ctx, gs);
    return st;
}

/* _PyObject_GetNewArguments: *pargs and *pkwargs are QP_NULL when absent. */
static int qp_get_new_arguments(JSContext *ctx, JSValueConst obj, JSValue *pargs, JSValue *pkwargs)
{
    JSAtom a;
    JSValue f, r;
    *pargs = *pkwargs = QP_NULL;
    a = JS_NewAtom(ctx, "__getnewargs_ex__");
    f = qp_lookup_special(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(f))
        return -1;
    if (!QP_IS_NULL(f)) {
        r = qp_call0(ctx, f);
        JS_FreeValue(ctx, f);
        if (JS_IsException(r))
            return -1;
        if (!qp_tuple_check(ctx, r)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, r));
            qp_raise_type_error(ctx, "__getnewargs_ex__ should return a tuple, not '%.200U'", tn);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, r);
            return -1;
        }
        if (QP_TUPLE_SIZE(r) != 2) {
            qp_raise_value_error(ctx, "__getnewargs_ex__ should return a tuple of length 2, not %d",
                                 (int)QP_TUPLE_SIZE(r));
            JS_FreeValue(ctx, r);
            return -1;
        }
        *pargs = JS_DupValue(ctx, QP_TUPLE_ITEMS(r)[0]);
        *pkwargs = JS_DupValue(ctx, QP_TUPLE_ITEMS(r)[1]);
        JS_FreeValue(ctx, r);
        if (!qp_tuple_check(ctx, *pargs)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, *pargs));
            qp_raise_type_error(ctx, "first item of the tuple returned by __getnewargs_ex__ must "
                                "be a tuple, not '%.200U'", tn);
            JS_FreeValue(ctx, tn);
            goto clear;
        }
        if (!qp_dict_check(ctx, *pkwargs)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, *pkwargs));
            qp_raise_type_error(ctx, "second item of the tuple returned by __getnewargs_ex__ must "
                                "be a dict, not '%.200U'", tn);
            JS_FreeValue(ctx, tn);
            goto clear;
        }
        return 0;
    }
    a = JS_NewAtom(ctx, "__getnewargs__");
    f = qp_lookup_special(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(f))
        return -1;
    if (!QP_IS_NULL(f)) {
        r = qp_call0(ctx, f);
        JS_FreeValue(ctx, f);
        if (JS_IsException(r))
            return -1;
        if (!qp_tuple_check(ctx, r)) {
            JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, r));
            qp_raise_type_error(ctx, "__getnewargs__ should return a tuple, not '%.200U'", tn);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, r);
            return -1;
        }
        *pargs = r;
    }
    return 0;
clear:
    JS_FreeValue(ctx, *pargs);
    JS_FreeValue(ctx, *pkwargs);
    *pargs = *pkwargs = QP_NULL;
    return -1;
}

static JSValue qp_reduce_newobj(JSContext *ctx, JSValueConst obj)
{
    QPType *tp = qp_type_of(ctx, obj);
    JSValue args, kwargs, copyreg, newobj, newargs, state, li = JS_UNDEFINED, di = JS_UNDEFINED, res;
    JSValueConst five[5];
    int hasargs;
    JSAtom a;

    if (!tp->slots.tp_new) {
        JSValue tn = qp_type_tp_name(ctx, tp);
        qp_raise_type_error(ctx, "cannot pickle '%.200U' object", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    if (qp_get_new_arguments(ctx, obj, &args, &kwargs) < 0)
        return JS_EXCEPTION;
    copyreg = qp_import_module(ctx, "copyreg");
    if (JS_IsException(copyreg)) {
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, kwargs);
        return copyreg;
    }
    hasargs = !QP_IS_NULL(args);
    if (QP_IS_NULL(kwargs) || qp_dict_size(kwargs) == 0) {
        Py_ssize_t i, n = hasargs ? QP_TUPLE_SIZE(args) : 0;
        JS_FreeValue(ctx, kwargs);
        a = JS_NewAtom(ctx, "__newobj__");
        newobj = qp_getattr(ctx, copyreg, a);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, copyreg);
        if (JS_IsException(newobj)) {
            JS_FreeValue(ctx, args);
            return newobj;
        }
        newargs = qp_tuple_new(ctx, n + 1);
        if (JS_IsException(newargs)) {
            JS_FreeValue(ctx, args);
            JS_FreeValue(ctx, newobj);
            return newargs;
        }
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(newargs)[0]);
        QP_TUPLE_ITEMS(newargs)[0] = qp_type_dup(ctx, tp);
        for (i = 0; i < n; i++) {
            JS_FreeValue(ctx, QP_TUPLE_ITEMS(newargs)[i + 1]);
            QP_TUPLE_ITEMS(newargs)[i + 1] = JS_DupValue(ctx, QP_TUPLE_ITEMS(args)[i]);
        }
        JS_FreeValue(ctx, args);
    } else {
        JSValueConst three[3] = { QP_TYPE_VALUE(tp), args, kwargs };
        a = JS_NewAtom(ctx, "__newobj_ex__");
        newobj = qp_getattr(ctx, copyreg, a);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, copyreg);
        if (JS_IsException(newobj)) {
            JS_FreeValue(ctx, args);
            JS_FreeValue(ctx, kwargs);
            return newobj;
        }
        newargs = qp_tuple_pack(ctx, 3, three);
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, kwargs);
        if (JS_IsException(newargs)) {
            JS_FreeValue(ctx, newobj);
            return newargs;
        }
    }
    state = qp_object_getstate(ctx, obj, !(hasargs || qp_list_check(ctx, obj) || qp_dict_check(ctx, obj)));
    if (JS_IsException(state))
        goto fail;
    if (qp_list_check(ctx, obj)) {
        li = qp_iter(ctx, obj);
        if (JS_IsException(li))
            goto fail2;
    }
    if (qp_dict_check(ctx, obj)) {
        JSValue items = qp_call_method(ctx, obj, QP_ATOM(ctx, items), NULL, 0, QP_NULL);
        if (JS_IsException(items))
            goto fail2;
        di = qp_iter(ctx, items);
        JS_FreeValue(ctx, items);
        if (JS_IsException(di))
            goto fail2;
    }
    five[0] = newobj;
    five[1] = newargs;
    five[2] = state;
    five[3] = li;
    five[4] = di;
    res = qp_tuple_pack(ctx, 5, five);
    JS_FreeValue(ctx, newobj);
    JS_FreeValue(ctx, newargs);
    JS_FreeValue(ctx, state);
    JS_FreeValue(ctx, li);
    JS_FreeValue(ctx, di);
    return res;
fail2:
    JS_FreeValue(ctx, state);
    JS_FreeValue(ctx, li);
fail:
    JS_FreeValue(ctx, newobj);
    JS_FreeValue(ctx, newargs);
    return JS_EXCEPTION;
}

static JSValue qp_common_reduce(JSContext *ctx, JSValueConst self, int proto)
{
    JSValue copyreg, res, p;
    JSValueConst a[2];
    JSAtom m;
    if (proto >= 2)
        return qp_reduce_newobj(ctx, self);
    copyreg = qp_import_module(ctx, "copyreg");
    if (JS_IsException(copyreg))
        return copyreg;
    p = JS_NewInt32(ctx, proto);
    a[0] = self;
    a[1] = p;
    m = JS_NewAtom(ctx, "_reduce_ex");
    res = qp_call_method(ctx, copyreg, m, a, 2, QP_NULL);
    JS_FreeAtom(ctx, m);
    JS_FreeValue(ctx, copyreg);
    return res;
}

static JSValue qp_object_reduce_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    return qp_common_reduce(ctx, self, 0);
}

static JSValue qp_object_reduce_ex_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    JSValue reduce, clsreduce, objreduce;
    int protocol, r, override;
    if (qp_arg_int(ctx, args[0], &protocol) < 0)
        return JS_EXCEPTION;
    r = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __reduce__), &reduce);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        clsreduce = qp_getattr(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)), QP_ATOM(ctx, __reduce__));
        if (JS_IsException(clsreduce)) {
            JS_FreeValue(ctx, reduce);
            return clsreduce;
        }
        r = qp_dict_get_item_atom(ctx, QP_TYPE(ctx, object)->dict, QP_ATOM(ctx, __reduce__),
                                  &objreduce);
        override = !(r > 0 && qp_is(clsreduce, objreduce));
        if (r > 0)
            JS_FreeValue(ctx, objreduce);
        JS_FreeValue(ctx, clsreduce);
        if (override) {
            JSValue res = qp_call0(ctx, reduce);
            JS_FreeValue(ctx, reduce);
            return res;
        }
        JS_FreeValue(ctx, reduce);
    }
    return qp_common_reduce(ctx, self, protocol);
}

static JSValue qp_object_subclasshook(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    return qp_not_implemented(ctx);
}

static JSValue qp_object_init_subclass(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return JS_UNDEFINED;
}

static JSValue qp_object_format_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst spec = args[0];
    Py_ssize_t n;
    if (!qp_str_check(ctx, spec)) {
        JSValue tn = JS_IsUndefined(spec) ? qp_str_from_cstr(ctx, "None")
                                          : qp_type_tp_name(ctx, qp_type_of(ctx, spec));
        qp_raise_type_error(ctx, "__format__() argument must be str, not %.50U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    n = qp_str_len(ctx, spec);
    if (n < 0)
        return JS_EXCEPTION;
    if (n > 0) {
        JSValue tn = qp_type_tp_name(ctx, qp_type_of(ctx, self));
        qp_raise_type_error(ctx, "unsupported format string passed to %.200U.__format__", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    return qp_str(ctx, self);
}

static JSValue qp_object_sizeof_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_of(ctx, self);
    return JS_NewInt32(ctx, 16 + 8 * (int)t->nslots);
}

static JSValue qp_object_dir_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue dict, itsclass, res;
    int r = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __dict__), &dict);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0 || !qp_dict_check(ctx, dict)) {
        if (r > 0)
            JS_FreeValue(ctx, dict);
        dict = qp_dict_new(ctx);
    } else {
        JSValue tmp = qp_dict_copy(ctx, dict);
        JS_FreeValue(ctx, dict);
        dict = tmp;
    }
    if (JS_IsException(dict))
        return dict;
    r = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __class__), &itsclass);
    if (r < 0)
        goto fail;
    if (r > 0) {
        r = qp_merge_class_dict(ctx, dict, itsclass);
        JS_FreeValue(ctx, itsclass);
        if (r < 0)
            goto fail;
    }
    res = qp_dict_keys_list(ctx, dict);
    JS_FreeValue(ctx, dict);
    return res;
fail:
    JS_FreeValue(ctx, dict);
    return JS_EXCEPTION;
}

static const QPMethodDef qp_object_methods[] = {
    { "__reduce_ex__", qp_object_reduce_ex_meth, QP_METH_O, 0,
      "__reduce_ex__($self, protocol, /)\n--\n\nHelper for pickle." },
    { "__reduce__", qp_object_reduce_meth, QP_METH_NOARGS, 0,
      "__reduce__($self, /)\n--\n\nHelper for pickle." },
    { "__getstate__", qp_object_getstate_meth, QP_METH_NOARGS, 0,
      "__getstate__($self, /)\n--\n\nHelper for pickle." },
    { "__subclasshook__", qp_object_subclasshook, QP_METH_CLASS | QP_METH_O, 0,
      "Abstract classes can override this to customize issubclass().\n\n"
      "This is invoked early on by abc.ABCMeta.__subclasscheck__().\n"
      "It should return True, False or NotImplemented.  If it returns\n"
      "NotImplemented, the normal algorithm is used.  Otherwise, it\n"
      "overrides the normal algorithm (and the outcome is cached).\n" },
    { "__init_subclass__", qp_object_init_subclass, QP_METH_CLASS | QP_METH_NOARGS, 0,
      "This method is called when a class is subclassed.\n\n"
      "The default implementation does nothing. It may be\n"
      "overridden to extend subclasses.\n" },
    { "__format__", qp_object_format_meth, QP_METH_O, 0,
      "__format__($self, format_spec, /)\n--\n\nDefault object formatter.\n\n"
      "Return str(self) if format_spec is empty. Raise TypeError otherwise." },
    { "__sizeof__", qp_object_sizeof_meth, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nSize of object in memory, in bytes." },
    { "__dir__", qp_object_dir_meth, QP_METH_NOARGS, 0,
      "__dir__($self, /)\n--\n\nDefault dir() implementation." },
    { NULL }
};

static const QPTypeSpec qp_object_spec = {
    .name = "object",
    .doc = "object()\n--\n\n"
           "The base class of the class hierarchy.\n\n"
           "When called, it accepts no arguments and returns a new featureless\n"
           "instance that has no instance attributes and cannot be given any.\n",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .slots = {
        .tp_getattro = qp_generic_getattr,
        .tp_setattro = qp_generic_setattr,
        .tp_repr = qp_object_repr,
        .tp_str = qp_object_str,
        .tp_hash = qp_object_hash,
        .tp_richcompare = qp_object_richcompare,
        .tp_init = qp_object_init,
        .tp_new = qp_object_new,
    },
    .methods = qp_object_methods,
    .getsets = qp_object_getsets,
};

/* ========================================================================= */
/* NoneType, NotImplementedType, JsObject                                    */
/* ========================================================================= */

static JSValue qp_none_repr(JSContext *ctx, JSValueConst v)
{
    return qp_str_from_cstr(ctx, "None");
}

static int qp_none_bool(JSContext *ctx, JSValueConst v)
{
    return 0;
}

static Py_hash_t qp_none_hash(JSContext *ctx, JSValueConst v)
{
    return 0xFCA86420;
}

static JSValue qp_none_new(JSContext *ctx, QPType *t, JSValueConst *args, size_t nargsf,
                           JSValueConst kwnames)
{
    if (qp_excess_args(qp_nargs(nargsf), kwnames))
        return qp_raise_type_error(ctx, "NoneType takes no arguments");
    return JS_UNDEFINED;
}

static const QPTypeSpec qp_none_type_spec = {
    .name = "NoneType",
    .doc = "NoneType()\n--\n\nThe type of the None singleton.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .slots = {
        .tp_repr = qp_none_repr,
        .tp_hash = qp_none_hash,
        .tp_richcompare = qp_object_richcompare,
        .tp_new = qp_none_new,
        .nb = { .nb_bool = qp_none_bool },
    },
};

static JSValue qp_notimpl_repr(JSContext *ctx, JSValueConst v)
{
    return qp_str_from_cstr(ctx, "NotImplemented");
}

static JSValue qp_notimpl_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                 JSValueConst kwnames, int magic)
{
    return qp_str_from_cstr(ctx, "NotImplemented");
}

static int qp_notimpl_bool(JSContext *ctx, JSValueConst v)
{
    qp_raise_type_error(ctx, "NotImplemented should not be used in a boolean context");
    return -1;
}

static JSValue qp_notimpl_new(JSContext *ctx, QPType *t, JSValueConst *args, size_t nargsf,
                              JSValueConst kwnames)
{
    if (qp_excess_args(qp_nargs(nargsf), kwnames))
        return qp_raise_type_error(ctx, "NotImplementedType takes no arguments");
    return qp_not_implemented(ctx);
}

static const QPMethodDef qp_notimpl_methods[] = {
    { "__reduce__", qp_notimpl_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_notimpl_type_spec = {
    .name = "NotImplementedType",
    .doc = "NotImplementedType()\n--\n\nThe type of the NotImplemented singleton.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .slots = {
        .tp_repr = qp_notimpl_repr,
        .tp_new = qp_notimpl_new,
        .nb = { .nb_bool = qp_notimpl_bool },
    },
    .methods = qp_notimpl_methods,
};

/* js.JsObject (Python view of foreign JS values): python/jsinterop.c,
   qp_jsobject_type_spec(). */

/* ========================================================================= */
/* Init                                                                      */
/* ========================================================================= */

static int qp_init_types_phase1(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    JSValue sym;
    QPType *tt;
    int i;

    qp_slotdefs_init();
    tt = qp_type_alloc(ctx, NULL);
    if (!tt)
        return -1;
    s->types[QP_T_type] = tt;
    for (i = 0; i < QP_T__COUNT; i++) {
        if (i == QP_T_type)
            continue;
        s->types[i] = qp_type_alloc(ctx, tt);
        if (!s->types[i])
            return -1;
    }
    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (i == QP_EXC_ExceptionGroup)
            continue;
        s->exc_types[i] = qp_type_alloc(ctx, tt);
        if (!s->exc_types[i])
            return -1;
    }
    sym = JS_NewSymbolFromAtom(ctx, QP_ATOM(ctx, __dict__), JS_ATOM_TYPE_PRIVATE);
    if (JS_IsException(sym))
        return -1;
    s->atom_dict_slot = JS_DupAtom(ctx, js_symbol_to_atom(ctx, sym));
    JS_FreeValue(ctx, sym);
    if (JS_VALUE_GET_TAG(s->pending_del) != JS_TAG_OBJECT) {
        s->pending_del = JS_NewArray(ctx);
        if (JS_IsException(s->pending_del))
            return -1;
    }
    if (JS_VALUE_GET_TAG(s->repr_stack) != JS_TAG_OBJECT) {
        s->repr_stack = JS_NewArray(ctx);
        if (JS_IsException(s->repr_stack))
            return -1;
    }
    memset(s->mcache, 0, sizeof(s->mcache));
    return 0;
}

static int qp_init_typeobject(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    if (qp_type_ready(ctx, s->types[QP_T_object], &qp_object_spec) < 0 ||
        qp_type_ready(ctx, s->types[QP_T_type], &qp_type_spec) < 0 ||
        qp_init_descr_types(ctx) < 0 ||
        qp_type_ready(ctx, s->types[QP_T_super], &qp_super_spec) < 0 ||
        qp_type_ready(ctx, s->types[QP_T_NoneType], &qp_none_type_spec) < 0 ||
        qp_type_ready(ctx, s->types[QP_T_NotImplementedType], &qp_notimpl_type_spec) < 0 ||
        qp_type_ready(ctx, s->types[QP_T_JsObject], qp_jsobject_type_spec()) < 0)
        return -1;
    JS_FreeValue(ctx, s->not_implemented);
    s->not_implemented = qp_object_new_instance(ctx, s->types[QP_T_NotImplementedType],
                                                JS_CLASS_PY_OBJECT);
    if (JS_IsException(s->not_implemented)) {
        s->not_implemented = JS_UNDEFINED;
        return -1;
    }
    return 0;
}
