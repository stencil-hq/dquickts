/*
 * Module objects (unit l): the `module` type (types.ModuleType).
 *
 * Derived from CPython Objects/moduleobject.c (PSF-2.0); the repr follows
 * Lib/importlib/_bootstrap.py (_module_repr, _module_repr_from_spec), which
 * CPython's module_repr calls.
 *
 * A module is a JS_CLASS_PY_MODULE object whose payload holds md_dict (a
 * PY_DICT).  Attributes live only in md_dict: tp_getattro / tp_setattro are
 * CPython's generic algorithms over that dict (the type has
 * QP_TPFLAGS_MANAGED_DICT and a `__dict__` getset, so unit h treats modules
 * as "has a dict" without its shape-dict mechanism).
 *
 * Builtin modules (sys, builtins, and the entries of the import.c table)
 * have no ModuleSpec in M2 (importlib is M3): their `builtin` flag stands
 * for CPython's spec.origin == 'built-in' in the repr.
 */

#include <unistd.h>
#include <limits.h>

typedef struct QPModule {
    JSValue dict;       /* md_dict (PY_DICT), owned */
    uint8_t builtin;    /* created by a builtin-module init (repr "(built-in)") */
} QPModule;

/* lazy_import objects (import.c) */
static BOOL qp_lazy_import_check(JSValueConst v);
static BOOL qp_lazy_import_is_resolving(JSValueConst v);
static JSValue qp_lazy_import_reify(JSContext *ctx, JSValue v, JSAtom name, JSValueConst ns);

static QPModule *qp_module_payload(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    if (JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_MODULE)
        return NULL;
    return JS_VALUE_GET_OBJ(v)->u.opaque;
}

static void qp_module_finalizer(JSRuntime *rt, JSValue val)
{
    QPModule *m = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (m) {
        JS_FreeValueRT(rt, m->dict);
        js_free_rt(rt, m);
    }
}

static void qp_module_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPModule *m = JS_VALUE_GET_OBJ(val)->u.opaque;
    if (m)
        JS_MarkValue(rt, m->dict, mark_func);
}

static BOOL qp_module_check(JSContext *ctx, JSValueConst v)
{
    return qp_module_payload(v) != NULL;
}

static JSValue qp_module_get_dict(JSValueConst module)
{
    QPModule *m = qp_module_payload(module);
    return m ? m->dict : QP_NULL;
}

/* new_module_notrack: an instance of t with an empty md_dict. */
static JSValue qp_module_alloc(JSContext *ctx, QPType *t)
{
    JSValue obj, dict;
    QPModule *m;

    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_MODULE);
    if (JS_IsException(obj))
        return obj;
    dict = qp_dict_new(ctx);
    if (JS_IsException(dict)) {
        JS_FreeValue(ctx, obj);
        return dict;
    }
    m = js_mallocz(ctx, sizeof(*m));
    if (!m) {
        JS_FreeValue(ctx, dict);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    m->dict = dict;
    JS_VALUE_GET_OBJ(obj)->u.opaque = m;
    return obj;
}

/* module_init_dict */
static int qp_module_init_dict(JSContext *ctx, JSValueConst dict, JSValueConst name,
                               JSValueConst doc)
{
    if (QP_IS_NULL(doc))
        doc = JS_UNDEFINED;
    if (qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __name__), name) < 0 ||
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __doc__), doc) < 0 ||
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __package__), JS_UNDEFINED) < 0 ||
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __loader__), JS_UNDEFINED) < 0 ||
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __spec__), JS_UNDEFINED) < 0)
        return -1;
    return 0;
}

/* PyModule_NewObject */
static JSValue qp_module_new(JSContext *ctx, JSValueConst name)
{
    JSValue m = qp_module_alloc(ctx, QP_TYPE(ctx, module));
    if (JS_IsException(m))
        return m;
    if (qp_module_init_dict(ctx, qp_module_payload(m)->dict, name, QP_NULL) < 0) {
        JS_FreeValue(ctx, m);
        return JS_EXCEPTION;
    }
    return m;
}

/* Mark a module created by a builtin-module init function (import.c). */
static void qp_module_set_builtin(JSValueConst module)
{
    QPModule *m = qp_module_payload(module);
    if (m)
        m->builtin = 1;
}

/* PyModule_AddFunctions: module-level builtin functions with
   __self__ = module and __module__ = module.__name__. */
static int qp_module_add_functions(JSContext *ctx, JSValueConst module, const QPMethodDef *ml)
{
    JSValue name, f, key;
    int ret = 0;

    if (qp_dict_get_item_atom(ctx, qp_module_get_dict(module), QP_ATOM(ctx, __name__), &name) < 0)
        return -1;
    if (QP_IS_NULL(name))
        name = JS_UNDEFINED;
    for (; ml->name; ml++) {
        f = qp_cfunction_new(ctx, ml, module, name);
        if (JS_IsException(f)) {
            ret = -1;
            break;
        }
        key = qp_str_from_cstr(ctx, ml->name);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, f);
            ret = -1;
            break;
        }
        ret = qp_dict_set_item(ctx, qp_module_get_dict(module), key, f);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, f);
        if (ret < 0)
            break;
    }
    JS_FreeValue(ctx, name);
    return ret;
}

/* PyModule_FromDefAndSpec + PyModule_ExecDef for a builtin module: new
   module named def->name with __doc__, the def's functions bound to it and
   the exec hook run.  sys and builtins exist from qp_init_builtins on and
   are returned as they are (CPython's _imp.create_builtin also hands out
   the existing objects for them). */
static JSValue qp_module_from_def(JSContext *ctx, const QPModuleDef *def)
{
    QPState *s = QP_STATE(ctx);
    JSValue name, doc, m;

    if (!strcmp(def->name, "sys") && JS_IsObject(s->sys_module))
        return JS_DupValue(ctx, s->sys_module);
    if (!strcmp(def->name, "builtins") && JS_IsObject(s->builtins_module))
        return JS_DupValue(ctx, s->builtins_module);
    name = qp_str_from_cstr(ctx, def->name);
    if (JS_IsException(name))
        return name;
    m = qp_module_new(ctx, name);
    JS_FreeValue(ctx, name);
    if (JS_IsException(m))
        return m;
    qp_module_set_builtin(m);
    if (def->doc) {
        doc = qp_str_from_cstr(ctx, def->doc);
        if (JS_IsException(doc))
            goto fail;
        if (qp_dict_set_item_atom(ctx, qp_module_get_dict(m), QP_ATOM(ctx, __doc__), doc) < 0) {
            JS_FreeValue(ctx, doc);
            goto fail;
        }
        JS_FreeValue(ctx, doc);
    }
    if (def->methods && qp_module_add_functions(ctx, m, def->methods) < 0)
        goto fail;
    if (def->exec && def->exec(ctx, m) < 0)
        goto fail;
    return m;
 fail:
    JS_FreeValue(ctx, m);
    return JS_EXCEPTION;
}

/* ---- spec helpers (shared with import.c) ------------------------------ */

/* _PyModuleSpec_IsInitializing: 1/0/-1 (spec QP_NULL or without the
   attribute -> 0). */
static int qp_module_spec_is_initializing(JSContext *ctx, JSValueConst spec)
{
    JSValue v;
    int rc;

    if (QP_IS_NULL(spec))
        return 0;
    rc = qp_getattr_opt(ctx, spec, QP_ATOM(ctx, _initializing), &v);
    if (rc > 0) {
        rc = qp_bool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    return rc;
}

/* _PyModuleSpec_IsUninitializedSubmodule */
static int qp_module_spec_is_uninitialized_submodule(JSContext *ctx, JSValueConst spec,
                                                     JSValueConst name)
{
    JSValue v;
    int rc;

    if (QP_IS_NULL(spec))
        return 0;
    rc = qp_getattr_opt(ctx, spec, QP_ATOM(ctx, _uninitialized_submodules), &v);
    if (rc > 0) {
        rc = qp_contains(ctx, v, name);
        JS_FreeValue(ctx, v);
    }
    return rc;
}

/* _PyModuleSpec_GetFileOrigin: 1 with *porigin (str) when spec.has_location
   is true and spec.origin is a str, 0 otherwise, -1 on error. */
static int qp_module_spec_get_file_origin(JSContext *ctx, JSValueConst spec, JSValue *porigin)
{
    JSValue v;
    int rc;

    *porigin = QP_NULL;
    rc = qp_getattr_opt(ctx, spec, QP_ATOM(ctx, has_location), &v);
    if (rc <= 0)
        return rc;
    rc = qp_bool(ctx, v);
    JS_FreeValue(ctx, v);
    if (rc <= 0)
        return rc;
    rc = qp_getattr_opt(ctx, spec, QP_ATOM(ctx, origin), &v);
    if (rc <= 0)
        return rc;
    if (!qp_str_check(ctx, v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    *porigin = v;
    return 1;
}

/* _PyModule_IsPossiblyShadowing:
     root = os.path.dirname(origin.removesuffix(os.sep + "__init__.py"))
     return not sys.flags.safe_path and root == (sys.path[0] or os.getcwd())
   with sys.path[0] as computed by the launcher (QPConfig.sys_path_0). */
static int qp_module_is_possibly_shadowing(JSContext *ctx, JSValueConst origin)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    const char *s, *path0;
    char *root, *sep, cwd[PATH_MAX];
    JSValue str;
    size_t len;
    int result = 0;

    if (QP_IS_NULL(origin) || cfg->safe_path || !cfg->sys_path_0)
        return 0;
    str = qp_str_value(ctx, origin);
    if (JS_IsException(str))
        return -1;
    s = qp_str_utf8(ctx, str, &len);
    JS_FreeValue(ctx, str);
    if (!s)
        return -1;
    root = js_malloc(ctx, len + 1);
    if (!root) {
        JS_FreeCString(ctx, s);
        return -1;
    }
    memcpy(root, s, len + 1);
    JS_FreeCString(ctx, s);
    sep = strrchr(root, '/');
    if (!sep)
        goto done;
    if (strcmp(sep + 1, "__init__.py") == 0) {
        *sep = '\0';
        sep = strrchr(root, '/');
        if (!sep)
            goto done;
    }
    *sep = '\0';
    path0 = cfg->sys_path_0;
    if (path0[0] == '\0') {
        if (!getcwd(cwd, sizeof(cwd)))
            goto done;
        path0 = cwd;
    }
    result = strcmp(path0, root) == 0;
 done:
    js_free(ctx, root);
    return result;
}

/* sys.stdlib_module_names contains name: 1/0/-1. */
static int qp_module_is_stdlib_name(JSContext *ctx, JSValueConst name)
{
    JSValue names = qp_sys_get(ctx, QP_ATOM(ctx, stdlib_module_names));
    int rc = 0;

    if (JS_IsException(names))
        return -1;
    if (QP_IS_NULL(names))
        return 0;
    if (qp_anyset_check(ctx, names))
        rc = qp_set_contains(ctx, names, name);
    JS_FreeValue(ctx, names);
    return rc;
}

/* ---- attribute access ------------------------------------------------- */

/* Mapping lookup of `name` in d without KeyError: 1/0/-1. */
static int qp_module_dict_lookup(JSContext *ctx, JSValueConst d, JSAtom name, JSValue *pres)
{
    return qp_dict_get_item_atom(ctx, d, name, pres);
}

/* module_get_resolved_dict_item: lookup reifying a lazy import (a lazy
   import being resolved by this thread counts as absent). */
static int qp_module_get_resolved_dict_item(JSContext *ctx, JSValueConst dict, JSAtom name,
                                            JSValue *pres)
{
    int rc = qp_module_dict_lookup(ctx, dict, name, pres);
    if (rc <= 0 || !qp_lazy_import_check(*pres))
        return rc;
    if (qp_lazy_import_is_resolving(*pres)) {
        JS_FreeValue(ctx, *pres);
        *pres = QP_NULL;
        return 0;
    }
    *pres = qp_lazy_import_reify(ctx, *pres, name, dict);
    if (JS_IsException(*pres)) {
        *pres = QP_NULL;
        return -1;
    }
    return 1;
}

/* _PyObject_GenericGetAttrWithDict over md_dict: 1 found (*pres), 0 not
   found (no exception), -1 error. */
static int qp_module_generic_lookup(JSContext *ctx, JSValueConst obj, JSAtom name, JSValue *pres)
{
    QPModule *m = qp_module_payload(obj);
    QPType *tp = qp_type_of(ctx, obj);
    QPType *dt = NULL;
    JSValue descr;
    int rc;

    *pres = QP_NULL;
    descr = qp_type_lookup(ctx, tp, name);
    if (!QP_IS_NULL(descr)) {
        descr = JS_DupValue(ctx, descr);
        dt = qp_type_of(ctx, descr);
        if (dt->slots.tp_descr_get && dt->slots.tp_descr_set) {
            *pres = dt->slots.tp_descr_get(ctx, descr, obj, QP_TYPE_VALUE(tp));
            JS_FreeValue(ctx, descr);
            if (JS_IsException(*pres)) {
                *pres = QP_NULL;
                return -1;
            }
            return 1;
        }
    }
    if (m) {
        rc = qp_module_dict_lookup(ctx, m->dict, name, pres);
        if (rc != 0) {
            JS_FreeValue(ctx, descr);
            return rc;
        }
    }
    if (!QP_IS_NULL(descr)) {
        if (dt->slots.tp_descr_get) {
            *pres = dt->slots.tp_descr_get(ctx, descr, obj, QP_TYPE_VALUE(tp));
            JS_FreeValue(ctx, descr);
            if (JS_IsException(*pres)) {
                *pres = QP_NULL;
                return -1;
            }
            return 1;
        }
        *pres = descr;
        return 1;
    }
    return 0;
}

/* Clear a pending AttributeError: 1 cleared, 0 another exception pending. */
static int qp_module_clear_attribute_error(JSContext *ctx)
{
    if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)))
        return 0;
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
    return 1;
}

/* _Py_module_getattro_impl.  suppress: return QP_NULL without exception
   instead of raising AttributeError. */
static JSValue qp_module_getattro_impl(JSContext *ctx, JSValueConst obj, JSAtom name,
                                       int suppress)
{
    QPModule *m = qp_module_payload(obj);
    JSValue attr, getattr, mod_name, spec, origin = QP_NULL, name_str, res;
    int rc, shadowing, shadowing_stdlib = 0;

    rc = qp_module_generic_lookup(ctx, obj, name, &attr);
    if (rc < 0) {
        if (!qp_module_clear_attribute_error(ctx))
            return JS_EXCEPTION;
        rc = 0;
    }
    if (rc > 0) {
        if (qp_lazy_import_check(attr)) {
            if (suppress && qp_lazy_import_is_resolving(attr)) {
                JS_FreeValue(ctx, attr);
                return QP_NULL;
            }
            return qp_lazy_import_reify(ctx, attr, name, m ? m->dict : JS_UNDEFINED);
        }
        return attr;
    }
    if (!m)
        goto not_found;

    if (qp_module_get_resolved_dict_item(ctx, m->dict, QP_ATOM(ctx, __getattr__), &getattr) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(getattr)) {
        name_str = qp_str_from_atom(ctx, name);
        if (JS_IsException(name_str)) {
            JS_FreeValue(ctx, getattr);
            return name_str;
        }
        res = qp_call1(ctx, getattr, name_str);
        JS_FreeValue(ctx, name_str);
        JS_FreeValue(ctx, getattr);
        if (JS_IsException(res) && suppress && qp_module_clear_attribute_error(ctx))
            return QP_NULL;
        return res;
    }

 not_found:
    if (suppress)
        return QP_NULL;
    if (!m)
        return qp_raise_attribute_error(ctx, obj, name);
    if (qp_dict_get_item_atom(ctx, m->dict, QP_ATOM(ctx, __name__), &mod_name) < 0)
        return JS_EXCEPTION;
    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str)) {
        JS_FreeValue(ctx, mod_name);
        return name_str;
    }
    if (QP_IS_NULL(mod_name) || !qp_str_check(ctx, mod_name)) {
        JS_FreeValue(ctx, mod_name);
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "module has no attribute '%U'", name_str);
        JS_FreeValue(ctx, name_str);
        return JS_EXCEPTION;
    }
    if (qp_dict_get_item_atom(ctx, m->dict, QP_ATOM(ctx, __spec__), &spec) < 0)
        goto done;
    if (QP_IS_NULL(spec)) {
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "module '%U' has no attribute '%U'",
                 mod_name, name_str);
        goto done;
    }
    if (qp_module_spec_get_file_origin(ctx, spec, &origin) < 0)
        goto done;
    shadowing = qp_module_is_possibly_shadowing(ctx, origin);
    if (shadowing < 0)
        goto done;
    if (shadowing) {
        shadowing_stdlib = qp_module_is_stdlib_name(ctx, mod_name);
        if (shadowing_stdlib < 0)
            goto done;
    }
    if (shadowing_stdlib) {
        qp_raise(ctx, QP_EXC(ctx, AttributeError),
                 "module '%U' has no attribute '%U' "
                 "(consider renaming '%U' since it has the same "
                 "name as the standard library module named '%U' "
                 "and prevents importing that standard library module)",
                 mod_name, name_str, origin, mod_name);
    } else {
        rc = qp_module_spec_is_initializing(ctx, spec);
        if (rc < 0)
            goto done;
        if (rc > 0) {
            if (shadowing) {
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "module '%U' has no attribute '%U' "
                         "(consider renaming '%U' if it has the same name "
                         "as a library you intended to import)",
                         mod_name, name_str, origin);
            } else if (!QP_IS_NULL(origin)) {
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "partially initialized module '%U' from '%U' has no attribute '%U' "
                         "(most likely due to a circular import)",
                         mod_name, origin, name_str);
            } else {
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "partially initialized module '%U' has no attribute '%U' "
                         "(most likely due to a circular import)",
                         mod_name, name_str);
            }
        } else {
            rc = qp_module_spec_is_uninitialized_submodule(ctx, spec, name_str);
            if (rc > 0) {
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "cannot access submodule '%U' of module '%U' "
                         "(most likely due to a circular import)",
                         name_str, mod_name);
            } else if (rc == 0) {
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "module '%U' has no attribute '%U'", mod_name, name_str);
            }
        }
    }
 done:
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, mod_name);
    JS_FreeValue(ctx, name_str);
    return JS_EXCEPTION;
}

static JSValue qp_module_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    return qp_module_getattro_impl(ctx, obj, name, 0);
}

/* getattr(module, name) with AttributeError suppressed: 1/0/-1. */
static int qp_module_getattr_opt(JSContext *ctx, JSValueConst obj, JSAtom name, JSValue *pres)
{
    *pres = qp_module_getattro_impl(ctx, obj, name, 1);
    if (JS_IsException(*pres)) {
        *pres = QP_NULL;
        return -1;
    }
    return QP_IS_NULL(*pres) ? 0 : 1;
}

/* PyObject_GenericSetAttr with the module dict. */
static int qp_module_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPModule *m = qp_module_payload(obj);
    QPType *tp = qp_type_of(ctx, obj);
    JSValue descr;
    int rc;

    descr = qp_type_lookup(ctx, tp, name);
    if (!QP_IS_NULL(descr)) {
        QPType *dt = qp_type_of(ctx, descr);
        if (dt->slots.tp_descr_set) {
            descr = JS_DupValue(ctx, descr);
            rc = dt->slots.tp_descr_set(ctx, descr, obj, v);
            JS_FreeValue(ctx, descr);
            return rc;
        }
    }
    if (!m) {
        qp_raise_attribute_error(ctx, obj, name);
        return -1;
    }
    if (QP_IS_NULL(v)) {
        rc = qp_dict_del_item_atom(ctx, m->dict, name);
        if (rc == 0) {
            qp_raise_attribute_error(ctx, obj, name);
            return -1;
        }
        return rc < 0 ? -1 : 0;
    }
    return qp_dict_set_item_atom(ctx, m->dict, name, v);
}

/* ---- repr ------------------------------------------------------------- */

/* getattr(o, name, None) */
static JSValue qp_module_getattr_default_none(JSContext *ctx, JSValueConst o, JSAtom name)
{
    JSValue v;
    int rc = qp_getattr_opt(ctx, o, name, &v);
    if (rc < 0)
        return JS_EXCEPTION;
    return rc ? v : JS_UNDEFINED;
}

/* importlib._bootstrap._module_repr_from_spec */
static JSValue qp_module_repr_from_spec(JSContext *ctx, JSValueConst spec)
{
    JSValue name, origin = JS_UNDEFINED, loader = JS_UNDEFINED, res = JS_EXCEPTION, path, lst;
    char buf[ATOM_GET_STR_BUF_SIZE];
    int t;

    name = qp_getattr(ctx, spec, QP_ATOM(ctx, name));
    if (JS_IsException(name))
        return name;
    t = qp_bool(ctx, name);
    if (t < 0)
        goto done;
    if (!t) {
        JS_FreeValue(ctx, name);
        name = qp_str_from_cstr(ctx, "?");
        if (JS_IsException(name))
            return name;
    }
    origin = qp_getattr(ctx, spec, QP_ATOM(ctx, origin));
    if (JS_IsException(origin))
        goto done;
    if (JS_IsUndefined(origin) || JS_IsNull(origin)) {
        loader = qp_getattr(ctx, spec, QP_ATOM(ctx, loader));
        if (JS_IsException(loader))
            goto done;
        if (JS_IsUndefined(loader) || JS_IsNull(loader)) {
            res = qp_str_from_format(ctx, "<module %R>", name);
        } else if (strcmp(JS_AtomGetStr(ctx, buf, sizeof(buf), qp_type_of(ctx, loader)->name),
                          "NamespaceLoader") == 0) {
            /* isinstance(loader, _NamespaceLoader): importlib (M3) is the
               only producer of NamespaceLoader instances */
            /* f'<module {name!r} (namespace) from {list(loader._path)}>' */
            path = qp_getattr(ctx, loader, QP_ATOM(ctx, _path));
            if (JS_IsException(path))
                goto done;
            lst = qp_list_from_iterable(ctx, path);
            JS_FreeValue(ctx, path);
            if (JS_IsException(lst))
                goto done;
            res = qp_str_from_format(ctx, "<module %R (namespace) from %R>", name, lst);
            JS_FreeValue(ctx, lst);
        } else {
            res = qp_str_from_format(ctx, "<module %R (%R)>", name, loader);
        }
    } else {
        JSValue has_location = qp_getattr(ctx, spec, QP_ATOM(ctx, has_location));
        if (JS_IsException(has_location))
            goto done;
        t = qp_bool(ctx, has_location);
        JS_FreeValue(ctx, has_location);
        if (t < 0)
            goto done;
        if (t) {
            res = qp_str_from_format(ctx, "<module %R from %R>", name, origin);
        } else {
            JSValue spec_name = qp_getattr(ctx, spec, QP_ATOM(ctx, name));
            if (JS_IsException(spec_name))
                goto done;
            res = qp_str_from_format(ctx, "<module %R (%S)>", spec_name, origin);
            JS_FreeValue(ctx, spec_name);
        }
    }
 done:
    JS_FreeValue(ctx, loader);
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, name);
    return res;
}

/* module_repr -> importlib._bootstrap._module_repr */
static JSValue qp_module_repr(JSContext *ctx, JSValueConst self)
{
    QPModule *m = qp_module_payload(self);
    JSValue loader, spec, name, filename, res;
    int t, rc;

    loader = qp_module_getattr_default_none(ctx, self, QP_ATOM(ctx, __loader__));
    if (JS_IsException(loader))
        return loader;
    spec = qp_module_getattr_default_none(ctx, self, QP_ATOM(ctx, __spec__));
    if (JS_IsException(spec)) {
        JS_FreeValue(ctx, loader);
        return spec;
    }
    t = qp_bool(ctx, spec);
    if (t != 0) {
        res = t < 0 ? JS_EXCEPTION : qp_module_repr_from_spec(ctx, spec);
        JS_FreeValue(ctx, spec);
        JS_FreeValue(ctx, loader);
        return res;
    }
    JS_FreeValue(ctx, spec);
    rc = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __name__), &name);
    if (rc < 0) {
        JS_FreeValue(ctx, loader);
        return JS_EXCEPTION;
    }
    if (rc == 0) {
        name = qp_str_from_cstr(ctx, "?");
        if (JS_IsException(name)) {
            JS_FreeValue(ctx, loader);
            return name;
        }
    }
    rc = qp_getattr_opt(ctx, self, QP_ATOM(ctx, __file__), &filename);
    if (rc < 0) {
        res = JS_EXCEPTION;
    } else if (rc == 0) {
        if (JS_IsUndefined(loader) || JS_IsNull(loader)) {
            /* builtin modules: CPython's spec has origin 'built-in' */
            if (m && m->builtin)
                res = qp_str_from_format(ctx, "<module %R (built-in)>", name);
            else
                res = qp_str_from_format(ctx, "<module %R>", name);
        } else {
            res = qp_str_from_format(ctx, "<module %R (%R)>", name, loader);
        }
    } else {
        res = qp_str_from_format(ctx, "<module %R from %R>", name, filename);
        JS_FreeValue(ctx, filename);
    }
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, loader);
    return res;
}

/* ---- methods, getsets, construction ----------------------------------- */

/* module.__dir__() */
static JSValue qp_module_dir(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                             JSValueConst kwnames, int magic)
{
    QPModule *m = qp_module_payload(self);
    JSValue dict, dirfunc, result = JS_EXCEPTION;
    int rc;

    if (m && qp_type_of(ctx, self) == QP_TYPE(ctx, module))
        dict = JS_DupValue(ctx, m->dict);
    else
        dict = qp_getattr(ctx, self, QP_ATOM(ctx, __dict__));
    if (JS_IsException(dict))
        return dict;
    if (qp_dict_check(ctx, dict)) {
        rc = qp_module_get_resolved_dict_item(ctx, dict, QP_ATOM(ctx, __dir__), &dirfunc);
        if (rc > 0) {
            result = qp_call0(ctx, dirfunc);
            JS_FreeValue(ctx, dirfunc);
        } else if (rc == 0) {
            result = qp_dict_keys_list(ctx, dict);
        }
    } else {
        qp_raise_type_error(ctx, "<module>.__dict__ is not a dictionary");
    }
    JS_FreeValue(ctx, dict);
    return result;
}

/* module_load_dict */
static JSValue qp_module_load_dict(JSContext *ctx, JSValueConst self)
{
    JSValue dict = qp_getattr(ctx, self, QP_ATOM(ctx, __dict__));
    if (JS_IsException(dict))
        return dict;
    if (!qp_dict_check(ctx, dict)) {
        JS_FreeValue(ctx, dict);
        return qp_raise_type_error(ctx, "<module>.__dict__ is not a dictionary");
    }
    return dict;
}

/* PyCallable_Check */
static BOOL qp_module_callable(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.tp_call != NULL || JS_IsFunction(ctx, v);
}

static JSValue qp_module_get_dict_attr(JSContext *ctx, JSValueConst self, int magic)
{
    QPModule *m = qp_module_payload(self);
    if (!m)
        return qp_raise_type_error(ctx, "descriptor '__dict__' for 'module' objects "
                                   "doesn't apply to a '%T' object", self);
    return JS_DupValue(ctx, m->dict);
}

static JSValue qp_module_get_annotate(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue dict, annotate;
    int rc;

    dict = qp_module_load_dict(ctx, self);
    if (JS_IsException(dict))
        return dict;
    rc = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __annotate__), &annotate);
    if (rc == 0) {
        annotate = JS_UNDEFINED;
        if (qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __annotate__), annotate) < 0)
            annotate = JS_EXCEPTION;
    } else if (rc < 0) {
        annotate = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, dict);
    return annotate;
}

static int qp_module_set_annotate(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    JSValue dict;
    int ret = -1;

    if (QP_IS_NULL(value)) {
        qp_raise_type_error(ctx, "cannot delete __annotate__ attribute");
        return -1;
    }
    dict = qp_module_load_dict(ctx, self);
    if (JS_IsException(dict))
        return -1;
    if (!JS_IsUndefined(value) && !JS_IsNull(value) && !qp_module_callable(ctx, value)) {
        qp_raise_type_error(ctx, "__annotate__ must be callable or None");
        goto done;
    }
    if (qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __annotate__), value) < 0)
        goto done;
    if (!JS_IsUndefined(value) && !JS_IsNull(value) &&
        qp_dict_del_item_atom(ctx, dict, QP_ATOM(ctx, __annotations__)) < 0)
        goto done;
    ret = 0;
 done:
    JS_FreeValue(ctx, dict);
    return ret;
}

static JSValue qp_module_get_annotations(JSContext *ctx, JSValueConst self, int magic)
{
    QPModule *m = qp_module_payload(self);
    JSValue dict, annotations, spec, annotate, one;
    BOOL is_initializing = FALSE;
    int rc;

    dict = qp_module_load_dict(ctx, self);
    if (JS_IsException(dict))
        return dict;
    rc = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __annotations__), &annotations);
    if (rc < 0)
        goto fail;
    if (rc > 0) {
        JS_FreeValue(ctx, dict);
        return annotations;
    }
    if (m) {
        if (qp_dict_get_item_atom(ctx, m->dict, QP_ATOM(ctx, __spec__), &spec) < 0)
            goto fail;
        if (!QP_IS_NULL(spec)) {
            rc = qp_module_spec_is_initializing(ctx, spec);
            JS_FreeValue(ctx, spec);
            if (rc < 0)
                goto fail;
            is_initializing = rc != 0;
        }
    }
    rc = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __annotate__), &annotate);
    if (rc < 0)
        goto fail;
    if (rc > 0 && qp_module_callable(ctx, annotate)) {
        one = JS_NewInt32(ctx, 1);
        annotations = qp_call1(ctx, annotate, one);
        if (JS_IsException(annotations)) {
            JS_FreeValue(ctx, annotate);
            goto fail;
        }
        if (!qp_dict_check(ctx, annotations)) {
            qp_raise_type_error(ctx, "__annotate__() must return a dict, not %T", annotations);
            JS_FreeValue(ctx, annotations);
            JS_FreeValue(ctx, annotate);
            goto fail;
        }
    } else {
        annotations = qp_dict_new(ctx);
    }
    JS_FreeValue(ctx, annotate);
    if (!JS_IsException(annotations) && !is_initializing &&
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __annotations__), annotations) < 0) {
        JS_FreeValue(ctx, annotations);
        annotations = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, dict);
    return annotations;
 fail:
    JS_FreeValue(ctx, dict);
    return JS_EXCEPTION;
}

static int qp_module_set_annotations(JSContext *ctx, JSValueConst self, JSValueConst value,
                                     int magic)
{
    JSValue dict, name;
    int ret;

    dict = qp_module_load_dict(ctx, self);
    if (JS_IsException(dict))
        return -1;
    if (!QP_IS_NULL(value)) {
        ret = qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __annotations__), value);
    } else {
        ret = qp_dict_del_item_atom(ctx, dict, QP_ATOM(ctx, __annotations__));
        if (ret == 0) {
            /* PyErr_SetObject(PyExc_AttributeError, "__annotations__") */
            name = qp_str_from_atom(ctx, QP_ATOM(ctx, __annotations__));
            if (!JS_IsException(name)) {
                qp_raise_value(ctx, QP_EXC(ctx, AttributeError), name);
                JS_FreeValue(ctx, name);
            }
            ret = -1;
        } else if (ret > 0) {
            ret = 0;
        }
    }
    if (ret == 0 && qp_dict_del_item_atom(ctx, dict, QP_ATOM(ctx, __annotate__)) < 0)
        ret = -1;
    JS_FreeValue(ctx, dict);
    return ret;
}

/* module.__new__: new_module (arguments are ignored, as in CPython). */
static JSValue qp_module_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    return qp_module_alloc(ctx, subtype);
}

/* module.__init__(name, doc=None) */
static int qp_module_tp_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    static const char *const keywords[] = { "name", "doc", NULL };
    static const QPArgSpec spec = { "module", keywords, 1, 2, 0 };
    QPModule *m = qp_module_payload(self);
    JSValueConst out[2];

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, out) < 0)
        return -1;
    if (!qp_str_check(ctx, out[0])) {
        qp_raise_type_error(ctx, "module() argument 'name' must be str, not %T", out[0]);
        return -1;
    }
    if (!m) {
        qp_raise_type_error(ctx, "descriptor '__init__' requires a 'module' object "
                            "but received a '%T'", self);
        return -1;
    }
    return qp_module_init_dict(ctx, m->dict, out[0], out[1]);
}

static const QPMethodDef qp_module_methods[] = {
    { "__dir__", qp_module_dir, QP_METH_NOARGS, 0,
      "__dir__() -> list\nspecialized dir() implementation" },
    { NULL }
};

static const QPGetSetDef qp_module_getsets[] = {
    { "__annotations__", qp_module_get_annotations, qp_module_set_annotations, 0, NULL },
    { "__annotate__", qp_module_get_annotate, qp_module_set_annotate, 0, NULL },
    { "__dict__", qp_module_get_dict_attr, NULL, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_module_spec = {
    .name = "module",
    .doc = "module(name, doc=None)\n--\n\n"
           "Create a module object.\n\n"
           "The name must be a string; the optional doc argument can have any type.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MANAGED_DICT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_MODULE,
    .slots = {
        .tp_getattro = qp_module_getattro,
        .tp_setattro = qp_module_setattro,
        .tp_repr = qp_module_repr,
        .tp_init = qp_module_tp_init,
        .tp_new = qp_module_tp_new,
    },
    .methods = qp_module_methods,
    .getsets = qp_module_getsets,
};

static int qp_module_type_init(JSContext *ctx)
{
    return qp_type_ready(ctx, QP_TYPE(ctx, module), &qp_module_spec);
}
