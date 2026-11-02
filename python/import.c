/*
 * Import machinery (unit l): IMPORT_NAME / IMPORT_FROM / import *, the
 * builtin-module path of importlib._bootstrap._find_and_load, and PEP 810
 * lazy imports (the lazy_import type).
 *
 * Derived from CPython Python/import.c (PyImport_ImportModuleLevelObject,
 * resolve_name, _PyImport_LazyImportModuleLevelObject, PyImport_Import),
 * Python/ceval.c (_PyEval_ImportName, _PyEval_LazyImportName,
 * _PyEval_ImportFrom), Python/intrinsics.c (import_all_from),
 * Objects/lazyimportobject.c and Lib/importlib/_bootstrap.py
 * (_find_and_load_unlocked, _handle_fromlist) (PSF-2.0).
 *
 * Module search: sys.modules (QPState.modules, the dict created at startup
 * like CPython's interp->imports.modules), then
 *  - QPState.importlib set (M3 boot code: importlib._bootstrap): its
 *    _find_and_load(abs_name, __import__), exactly like CPython
 *    import_find_and_load;
 *  - otherwise the C port of _find_and_load_unlocked restricted to the
 *    BuiltinImporter: builtin modules of the inittab (python/config.c), and
 *    ModuleNotFoundError "No module named %R" for anything else (file-system
 *    imports are M3).
 *
 * Lazy imports: sys.lazy_modules bookkeeping, pending-submodule
 * registration on parent packages and sys.set_lazy_imports_filter are
 * package/file-system features that come with importlib (M3); the
 * lazy_import objects, their resolution, reification in namespaces,
 * `lazy from` projections, cycle detection and error causes are complete.
 */

/* ---- small helpers ---------------------------------------------------- */

/* PyMapping_GetOptionalItem(mapping, name): 1/0/-1. */
static int qp_import_mapping_get(JSContext *ctx, JSValueConst mapping, JSAtom name, JSValue *pres)
{
    JSValue key, v;

    *pres = QP_NULL;
    if (qp_anydict_check(ctx, mapping))
        return qp_dict_get_item_atom(ctx, mapping, name, pres);
    key = qp_str_from_atom(ctx, name);
    if (JS_IsException(key))
        return -1;
    v = qp_getitem(ctx, mapping, key);
    JS_FreeValue(ctx, key);
    if (JS_IsException(v)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, KeyError))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return 0;
        }
        return -1;
    }
    *pres = v;
    return 1;
}

/* UTF-8 text of a str or str subclass instance (free with JS_FreeCString). */
static const char *qp_import_utf8(JSContext *ctx, JSValueConst s, size_t *plen)
{
    JSValue v = qp_str_value(ctx, s);
    const char *p;

    if (JS_IsException(v))
        return NULL;
    p = qp_str_utf8(ctx, v, plen);
    JS_FreeValue(ctx, v);
    return p;
}

/* Index of the first (dir > 0) or last (dir < 0) '.' in s[0..end), -1 if
   none.  Byte offsets on UTF-8 text ('.' is ASCII). */
static ssize_t qp_import_find_dot(const char *s, size_t end, int dir)
{
    size_t i;

    if (dir > 0) {
        for (i = 0; i < end; i++)
            if (s[i] == '.')
                return (ssize_t)i;
    } else {
        for (i = end; i-- > 0;)
            if (s[i] == '.')
                return (ssize_t)i;
    }
    return -1;
}

/* The builtins mapping of the running Python frame (borrowed), else the
   builtins module dict. */
static JSValue qp_import_current_builtins(JSContext *ctx)
{
    JSValue b = qp_frame_builtins(qp_py_frame_at(ctx, 0));
    return QP_IS_NULL(b) ? QP_STATE(ctx)->builtins_dict : b;
}

/* sys.modules[name] or QP_NULL (import_get_module): borrowed lookups are
   unsafe across calls, so a new reference is returned. */
static int qp_import_get_module(JSContext *ctx, JSValueConst name, JSValue *pmod)
{
    JSValue modules = QP_STATE(ctx)->modules;

    *pmod = QP_NULL;
    if (!JS_IsObject(modules)) {
        qp_raise_runtime_error(ctx, "unable to get sys.modules");
        return -1;
    }
    if (qp_dict_check(ctx, modules))
        return qp_dict_get_item(ctx, modules, name, pmod);
    *pmod = qp_getitem(ctx, modules, name);
    if (JS_IsException(*pmod)) {
        *pmod = QP_NULL;
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, KeyError))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return 0;
        }
        return -1;
    }
    return 1;
}

/* exc.<attr> = v for the ImportError family (skipped when v is QP_NULL). */
static int qp_import_set_exc_attr(JSContext *ctx, JSValueConst exc, JSAtom attr, JSValueConst v)
{
    if (QP_IS_NULL(v))
        return 0;
    return qp_setattr(ctx, exc, attr, v);
}

/* PyErr_SetImportErrorSubclass / _PyErr_SetImportErrorWithNameFrom:
   raise type(msg) with name, path and name_from set.  Consumes msg. */
static JSValue qp_import_raise_error(JSContext *ctx, QPType *type, JSValue msg,
                                     JSValueConst name, JSValueConst path, JSValueConst name_from)
{
    JSValue exc;

    if (JS_IsException(msg))
        return msg;
    exc = qp_exc_new(ctx, type, 1, (JSValueConst *)&msg);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return exc;
    if (qp_import_set_exc_attr(ctx, exc, QP_ATOM(ctx, name), name) < 0 ||
        qp_import_set_exc_attr(ctx, exc, QP_ATOM(ctx, path), path) < 0 ||
        qp_import_set_exc_attr(ctx, exc, QP_ATOM(ctx, name_from), name_from) < 0) {
        JS_FreeValue(ctx, exc);
        return JS_EXCEPTION;
    }
    return qp_raise_object(ctx, exc);
}

/* ModuleNotFoundError(msg, name=name) */
static JSValue qp_import_raise_not_found(JSContext *ctx, JSValue msg, JSValueConst name)
{
    return qp_import_raise_error(ctx, QP_EXC(ctx, ModuleNotFoundError), msg, name, QP_NULL, QP_NULL);
}

/* ---- name resolution -------------------------------------------------- */

/* resolve_name: absolute name of a relative import. */
static JSValue qp_import_resolve_name(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      int level)
{
    JSValue package = QP_NULL, spec = QP_NULL, parent, base = JS_EXCEPTION, res;
    const char *pkg = NULL;
    size_t pkg_len;
    ssize_t last_dot, dot;
    int level_up, rc;

    if (QP_IS_NULL(globals)) {
        qp_raise(ctx, QP_EXC(ctx, KeyError), "'__name__' not in globals");
        goto error;
    }
    if (!qp_anydict_check(ctx, globals)) {
        qp_raise_type_error(ctx, "globals must be a dict or a frozendict");
        goto error;
    }
    if (qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __package__), &package) < 0)
        goto error;
    if (JS_IsUndefined(package) || JS_IsNull(package)) {
        JS_FreeValue(ctx, package);
        package = QP_NULL;
    }
    if (qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __spec__), &spec) < 0)
        goto error;

    if (!QP_IS_NULL(package)) {
        if (!qp_str_check(ctx, package)) {
            qp_raise_type_error(ctx, "package must be a string");
            goto error;
        } else if (!QP_IS_NULL(spec) && !JS_IsUndefined(spec) && !JS_IsNull(spec)) {
            parent = qp_getattr(ctx, spec, QP_ATOM(ctx, parent));
            if (JS_IsException(parent))
                goto error;
            rc = qp_richcompare_bool(ctx, package, parent, QP_CMP_EQ);
            JS_FreeValue(ctx, parent);
            if (rc < 0)
                goto error;
            if (rc == 0 &&
                qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "__package__ != __spec__.parent") < 0)
                goto error;
        }
    } else if (!QP_IS_NULL(spec) && !JS_IsUndefined(spec) && !JS_IsNull(spec)) {
        package = qp_getattr(ctx, spec, QP_ATOM(ctx, parent));
        if (JS_IsException(package)) {
            package = QP_NULL;
            goto error;
        }
        if (!qp_str_check(ctx, package)) {
            qp_raise_type_error(ctx, "__spec__.parent must be a string");
            goto error;
        }
    } else {
        if (qp_warn(ctx, QP_EXC(ctx, ImportWarning), 1,
                    "can't resolve package from __spec__ or __package__, "
                    "falling back on __name__ and __path__") < 0)
            goto error;
        if (qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __name__), &package) < 0)
            goto error;
        if (QP_IS_NULL(package)) {
            qp_raise(ctx, QP_EXC(ctx, KeyError), "'__name__' not in globals");
            goto error;
        }
        if (!qp_str_check(ctx, package)) {
            qp_raise_type_error(ctx, "__name__ must be a string");
            goto error;
        }
        {
            JSValue haspath;
            rc = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __path__), &haspath);
            if (rc < 0)
                goto error;
            JS_FreeValue(ctx, haspath);
        }
        if (!rc) {
            pkg = qp_import_utf8(ctx, package, &pkg_len);
            if (!pkg)
                goto error;
            dot = qp_import_find_dot(pkg, pkg_len, -1);
            if (dot < 0)
                goto no_parent_error;
            parent = qp_str_from_utf8(ctx, pkg, dot);
            JS_FreeCString(ctx, pkg);
            pkg = NULL;
            if (JS_IsException(parent))
                goto error;
            JS_FreeValue(ctx, package);
            package = parent;
        }
    }

    pkg = qp_import_utf8(ctx, package, &pkg_len);
    if (!pkg)
        goto error;
    last_dot = (ssize_t)pkg_len;
    if (last_dot == 0)
        goto no_parent_error;
    for (level_up = 1; level_up < level; level_up += 1) {
        last_dot = qp_import_find_dot(pkg, last_dot, -1);
        if (last_dot < 0) {
            qp_raise(ctx, QP_EXC(ctx, ImportError),
                     "attempted relative import beyond top-level package");
            goto error;
        }
    }
    base = qp_str_from_utf8(ctx, pkg, last_dot);
    if (JS_IsException(base) || qp_str_len(ctx, name) == 0)
        goto done;
    res = qp_str_from_format(ctx, "%U.%U", base, name);
    JS_FreeValue(ctx, base);
    base = res;
    goto done;

 no_parent_error:
    qp_raise(ctx, QP_EXC(ctx, ImportError),
             "attempted relative import with no known parent package");
 error:
    base = JS_EXCEPTION;
 done:
    if (pkg)
        JS_FreeCString(ctx, pkg);
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, package);
    return base;
}

/* _PyImport_GetAbsName */
static JSValue qp_import_get_abs_name(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      int level)
{
    if (level > 0)
        return qp_import_resolve_name(ctx, name, globals, level);
    if (qp_str_len(ctx, name) == 0)
        return qp_raise_value_error(ctx, "Empty module name");
    return JS_DupValue(ctx, name);
}

/* ---- finding and loading ---------------------------------------------- */

static JSValue qp_import_module_level(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      JSValueConst locals, JSValueConst fromlist, int level);

/* Load a builtin module of the inittab into sys.modules
   (BuiltinImporter.create_module/exec_module + _load_unlocked). */
static JSValue qp_import_load_builtin(JSContext *ctx, const QPModuleDef *def, JSValueConst name)
{
    JSValue mod = qp_module_create_from_def(ctx, def, FALSE);
    if (JS_IsException(mod))
        return mod;
    if (qp_dict_set_item(ctx, QP_STATE(ctx)->modules, name, mod) < 0) {
        JS_FreeValue(ctx, mod);
        return JS_EXCEPTION;
    }
    if (qp_module_exec_pending(ctx, mod) < 0) {
        /* _load_unlocked: a failed module leaves sys.modules */
        JSValue exc = qp_exc_fetch(ctx);
        if (qp_dict_del_item(ctx, QP_STATE(ctx)->modules, name) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        qp_raise_object(ctx, exc);
        JS_FreeValue(ctx, mod);
        return JS_EXCEPTION;
    }
    return mod;
}

/* importlib._bootstrap._find_and_load_unlocked without importlib: only
   the BuiltinImporter finds anything. */
static JSValue qp_import_find_and_load_unlocked(JSContext *ctx, JSValueConst name)
{
    JSValue parent = QP_NULL, parent_module = QP_NULL, child = QP_NULL, path, mod = JS_EXCEPTION;
    const char *s;
    const QPModuleDef *def;
    size_t len;
    ssize_t dot;
    int rc;

    s = qp_import_utf8(ctx, name, &len);
    if (!s)
        return JS_EXCEPTION;
    dot = qp_import_find_dot(s, len, -1);
    if (dot >= 0) {
        parent = qp_str_from_utf8(ctx, s, dot);
        child = qp_str_from_utf8(ctx, s + dot + 1, len - dot - 1);
        if (JS_IsException(parent) || JS_IsException(child))
            goto done;
        rc = qp_import_get_module(ctx, parent, &parent_module);
        if (rc < 0)
            goto done;
        if (rc == 0) {
            /* _call_with_frames_removed(import_, parent) */
            parent_module = qp_import_module_level(ctx, parent, QP_NULL, QP_NULL, QP_NULL, 0);
            if (JS_IsException(parent_module)) {
                parent_module = QP_NULL;
                goto done;
            }
            JS_FreeValue(ctx, parent_module);
            parent_module = QP_NULL;
        }
        /* Crazy side-effects! */
        rc = qp_import_get_module(ctx, name, &mod);
        if (rc < 0) {
            mod = JS_EXCEPTION;
            goto done;
        }
        if (rc > 0)
            goto done;
        mod = JS_EXCEPTION;
        JS_FreeValue(ctx, parent_module);
        rc = qp_import_get_module(ctx, parent, &parent_module);
        if (rc < 0)
            goto done;
        if (rc == 0) {
            parent_module = QP_NULL;
            qp_raise_key_error(ctx, parent);
            goto done;
        }
        rc = qp_getattr_opt(ctx, parent_module, QP_ATOM(ctx, __path__), &path);
        if (rc < 0)
            goto done;
        if (rc == 0) {
            /* raise ModuleNotFoundError(msg, name=name) from None */
            JSValue exc, msg = qp_str_from_format(ctx, "No module named %R; %R is not a package",
                                                  name, parent);
            if (JS_IsException(msg))
                goto done;
            exc = qp_exc_new(ctx, QP_EXC(ctx, ModuleNotFoundError), 1, (JSValueConst *)&msg);
            JS_FreeValue(ctx, msg);
            if (JS_IsException(exc))
                goto done;
            if (qp_setattr(ctx, exc, QP_ATOM(ctx, name), name) < 0 ||
                qp_setattr(ctx, exc, QP_ATOM(ctx, __cause__), JS_UNDEFINED) < 0) {
                JS_FreeValue(ctx, exc);
                goto done;
            }
            qp_raise_object(ctx, exc);
            goto done;
        }
        JS_FreeValue(ctx, path);
    }
    /* _find_spec(name, path): BuiltinImporter.find_spec */
    def = qp_inittab_find(s, len);
    if (!def) {
        qp_import_raise_not_found(ctx, qp_str_from_format(ctx, "No module named %R", name), name);
        goto done;
    }
    mod = qp_import_load_builtin(ctx, def, name);
    if (!JS_IsException(mod) && !QP_IS_NULL(parent_module)) {
        JSAtom a = qp_str_to_atom(ctx, child);
        rc = a == JS_ATOM_NULL ? -1 : qp_setattr(ctx, parent_module, a, mod);
        JS_FreeAtom(ctx, a);
        if (rc < 0) {
            JS_FreeValue(ctx, mod);
            mod = JS_EXCEPTION;
        }
    }
 done:
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, parent_module);
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, child);
    return mod;
}

/* -X importtime (_IMPORT_TIME_HEADER) */
static void qp_import_time_header(QPState *s)
{
    if (!s->import_time_header_done) {
        fputs("import time: self [us] | cumulative | imported package\n", stderr);
        s->import_time_header_done = 1;
    }
}

/* get_importtime_name: the UTF-8 bytes of the name (backslashreplace),
   freed with JS_FreeValue; the pending exception is kept */
static JSValue qp_import_time_name(JSContext *ctx, JSValueConst name)
{
    JSValue exc = JS_HasException(ctx) ? JS_GetException(ctx) : JS_UNINITIALIZED;
    JSValue b = qp_str_encode(ctx, name, "utf-8", "backslashreplace");

    if (JS_IsException(b)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        b = JS_UNDEFINED;
    }
    if (!JS_IsUninitialized(exc))
        JS_Throw(ctx, exc);
    return b;
}

/* the bytes of qp_import_time_name(), "?" if it failed */
static void qp_import_time_bytes(JSValueConst b, const char **pdata, Py_ssize_t *plen)
{
    const uint8_t *d = JS_IsUndefined(b) ? NULL : qp_bytes_data(b, plen);
    if (!d) {
        *pdata = "?";
        *plen = 1;
    } else {
        *pdata = (const char *)d;
    }
}

static int64_t qp_import_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* _PyTime_AsMicroseconds(ns, _PyTime_ROUND_CEILING) */
static long qp_import_time_us(int64_t ns)
{
    return (long)(ns >= 0 ? (ns + 999) / 1000 : -((-ns) / 1000));
}

/* -X importtime=2: a module found in sys.modules */
static void qp_import_time_cached(JSContext *ctx, JSValueConst name)
{
    QPState *s = QP_STATE(ctx);
    const char *data;
    Py_ssize_t len;
    JSValue b;

    qp_import_time_header(s);
    b = qp_import_time_name(ctx, name);
    qp_import_time_bytes(b, &data, &len);
    fprintf(stderr, "import time: cached    | cached     | %*s%.*s\n",
            s->import_time_level * 2, "", (int)len, data);
    JS_FreeValue(ctx, b);
}

/* import_find_and_load: importlib._bootstrap._find_and_load(abs_name,
   __import__) when importlib is booted, else the builtin-only port. */
static JSValue qp_import_find_and_load(JSContext *ctx, JSValueConst abs_name)
{
    QPState *s = QP_STATE(ctx);
    JSValue mod;
    JSValueConst args[2];
    JSAtom fn;
    int rc;

    if (JS_IsObject(s->importlib)) {
        int import_time = s->config.import_time;
        int64_t t1 = 0, accumulated_copy = s->import_time_accumulated;

        if (import_time) {
            qp_import_time_header(s);
            s->import_time_level++;
            t1 = qp_import_time_now();
            s->import_time_accumulated = 0;
        }
        args[0] = abs_name;
        args[1] = JS_IsObject(s->import_func) ? s->import_func : JS_UNDEFINED;
        fn = JS_NewAtom(ctx, "_find_and_load");
        if (fn == JS_ATOM_NULL)
            mod = JS_EXCEPTION;
        else
            mod = qp_call_method(ctx, s->importlib, fn, args, 2, QP_NULL);
        JS_FreeAtom(ctx, fn);
        if (import_time) {
            int64_t cum = qp_import_time_now() - t1;
            const char *data;
            Py_ssize_t len;
            JSValue b;

            s->import_time_level--;
            b = qp_import_time_name(ctx, abs_name);
            qp_import_time_bytes(b, &data, &len);
            fprintf(stderr, "import time: %9ld | %10ld | %*s%.*s\n",
                    qp_import_time_us(cum - s->import_time_accumulated),
                    qp_import_time_us(cum),
                    s->import_time_level * 2, "", (int)len, data);
            JS_FreeValue(ctx, b);
            s->import_time_accumulated = accumulated_copy + cum;
        }
        return mod;
    }
    /* _find_and_load */
    rc = qp_import_get_module(ctx, abs_name, &mod);
    if (rc < 0)
        return JS_EXCEPTION;
    if (rc > 0) {
        if (JS_IsUndefined(mod) || JS_IsNull(mod)) {
            JS_FreeValue(ctx, mod);
            return qp_import_raise_not_found(
                ctx, qp_str_from_format(ctx, "import of %U halted; None in sys.modules", abs_name),
                abs_name);
        }
        return mod;
    }
    return qp_import_find_and_load_unlocked(ctx, abs_name);
}

/* importlib._bootstrap._handle_fromlist (package modules: has __path__). */
static int qp_import_handle_fromlist(JSContext *ctx, JSValueConst module, JSValueConst fromlist,
                                     int recursive)
{
    JSValue it, x, v, all, mod_name, from_name, r, exc, exc_name, cur;
    int rc, ret = -1;

    it = qp_iter(ctx, fromlist);
    if (JS_IsException(it))
        return -1;
    for (;;) {
        x = qp_next(ctx, it);
        if (JS_IsException(x))
            goto done;
        if (QP_IS_NULL(x))
            break;
        if (!qp_str_check(ctx, x)) {
            if (recursive) {
                mod_name = qp_getattr(ctx, module, QP_ATOM(ctx, __name__));
                if (!JS_IsException(mod_name)) {
                    qp_raise_type_error(ctx, "Item in %S.__all__ must be str, not %s",
                                        mod_name, qp_type_of(ctx, x)->tp_name ?
                                        qp_type_of(ctx, x)->tp_name : "object");
                    JS_FreeValue(ctx, mod_name);
                }
            } else {
                qp_raise_type_error(ctx, "Item in ``from list'' must be str, not %T", x);
            }
            JS_FreeValue(ctx, x);
            goto done;
        }
        rc = qp_str_len(ctx, x) == 1 ? 0 : 1;
        if (rc == 0) {
            const char *p = qp_import_utf8(ctx, x, NULL);
            if (!p) {
                JS_FreeValue(ctx, x);
                goto done;
            }
            rc = strcmp(p, "*") != 0;
            JS_FreeCString(ctx, p);
        }
        if (rc == 0) {
            /* '*' */
            if (!recursive) {
                rc = qp_getattr_opt(ctx, module, QP_ATOM(ctx, __all__), &all);
                if (rc < 0) {
                    JS_FreeValue(ctx, x);
                    goto done;
                }
                if (rc > 0) {
                    rc = qp_import_handle_fromlist(ctx, module, all, 1);
                    JS_FreeValue(ctx, all);
                    if (rc < 0) {
                        JS_FreeValue(ctx, x);
                        goto done;
                    }
                }
            }
            JS_FreeValue(ctx, x);
            continue;
        }
        {
            JSAtom a = qp_str_to_atom(ctx, x);
            if (a == JS_ATOM_NULL) {
                JS_FreeValue(ctx, x);
                goto done;
            }
            rc = qp_getattr_opt(ctx, module, a, &v);
            JS_FreeAtom(ctx, a);
        }
        if (rc < 0) {
            JS_FreeValue(ctx, x);
            goto done;
        }
        if (rc > 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, x);
            continue;
        }
        mod_name = qp_getattr(ctx, module, QP_ATOM(ctx, __name__));
        if (JS_IsException(mod_name)) {
            JS_FreeValue(ctx, x);
            goto done;
        }
        from_name = qp_str_from_format(ctx, "%S.%U", mod_name, x);
        JS_FreeValue(ctx, mod_name);
        JS_FreeValue(ctx, x);
        if (JS_IsException(from_name))
            goto done;
        if (JS_IsObject(QP_STATE(ctx)->import_func))
            r = qp_call1(ctx, QP_STATE(ctx)->import_func, from_name);
        else
            r = qp_import_module_level(ctx, from_name, QP_NULL, QP_NULL, QP_NULL, 0);
        if (JS_IsException(r)) {
            if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, ModuleNotFoundError))) {
                JS_FreeValue(ctx, from_name);
                goto done;
            }
            exc = qp_exc_fetch(ctx);
            exc_name = qp_getattr(ctx, exc, QP_ATOM(ctx, name));
            rc = JS_IsException(exc_name) ? -1 : qp_richcompare_bool(ctx, exc_name, from_name, QP_CMP_EQ);
            JS_FreeValue(ctx, exc_name);
            if (rc > 0) {
                /* sys.modules.get(from_name, _NEEDS_LOADING) is not None */
                rc = qp_import_get_module(ctx, from_name, &cur);
                if (rc > 0) {
                    rc = !(JS_IsUndefined(cur) || JS_IsNull(cur));
                    JS_FreeValue(ctx, cur);
                } else if (rc == 0) {
                    rc = 1;
                }
            }
            JS_FreeValue(ctx, from_name);
            if (rc > 0) {
                JS_FreeValue(ctx, exc);
                continue;
            }
            if (rc == 0)
                qp_raise_object(ctx, exc);
            else
                JS_FreeValue(ctx, exc);
            goto done;
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, from_name);
    }
    ret = 0;
 done:
    JS_FreeValue(ctx, it);
    return ret;
}

/* remove_importlib_frames: on a failed import, drop the frames of the
   (frozen) importlib from the pending exception's traceback: every
   importlib chunk for ImportError, else only the chunks that end with
   _call_with_frames_removed.  Not done with -v.  Errors while editing the
   traceback leave it unchanged. */
static void qp_import_remove_importlib_frames(JSContext *ctx)
{
    static const char *const names[5] = { "tb_next", "tb_frame", "f_code", "co_filename", "co_name" };
    JSAtom atoms[5] = { JS_ATOM_NULL, JS_ATOM_NULL, JS_ATOM_NULL, JS_ATOM_NULL, JS_ATOM_NULL };
    JSValue exc, base = JS_UNDEFINED, tb, *tbs = NULL, frame, code, s, next;
    int n = 0, size = 0, i, always_trim, in_importlib = 0, now, prev = -1, outer = -1, ok = 0;
    const char *p;

    if (!JS_HasException(ctx) || QP_STATE(ctx)->config.verbose)
        return;
    exc = qp_exc_fetch(ctx);
    if (!qp_exc_check(ctx, exc))
        goto restore;
    always_trim = qp_typecheck(ctx, exc, QP_EXC(ctx, ImportError));
    for (i = 0; i < 5; i++) {
        atoms[i] = JS_NewAtom(ctx, names[i]);
        if (atoms[i] == JS_ATOM_NULL)
            goto cleanup;
    }
    base = qp_exc_get_traceback(ctx, exc);
    if (JS_IsException(base)) {
        base = JS_UNDEFINED;
        goto cleanup;
    }
    /* collect the chain */
    for (tb = JS_DupValue(ctx, base); !JS_IsUndefined(tb) && !JS_IsNull(tb);) {
        if (n == size) {
            JSValue *nt = js_realloc(ctx, tbs, sizeof(*tbs) * (size ? size * 2 : 16));
            if (!nt) {
                JS_FreeValue(ctx, tb);
                goto cleanup;
            }
            tbs = nt;
            size = size ? size * 2 : 16;
        }
        tbs[n++] = tb;
        tb = qp_getattr(ctx, tbs[n - 1], atoms[0]);
        if (JS_IsException(tb))
            goto cleanup;
    }
    for (i = 0; i < n; i++) {
        next = i + 1 < n ? tbs[i + 1] : JS_UNDEFINED;
        frame = qp_getattr(ctx, tbs[i], atoms[1]);
        if (JS_IsException(frame))
            goto cleanup;
        code = qp_getattr(ctx, frame, atoms[2]);
        JS_FreeValue(ctx, frame);
        if (JS_IsException(code))
            goto cleanup;
        s = qp_getattr(ctx, code, atoms[3]);
        now = 0;
        if (!JS_IsException(s) && qp_str_check(ctx, s) && (p = qp_import_utf8(ctx, s, NULL))) {
            now = !strcmp(p, "<frozen importlib._bootstrap>") ||
                !strcmp(p, "<frozen importlib._bootstrap_external>");
            JS_FreeCString(ctx, p);
        }
        JS_FreeValue(ctx, s);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, code);
            goto cleanup;
        }
        if (now && !in_importlib)
            outer = prev;   /* the link to this chunk of importlib frames */
        in_importlib = now;
        if (in_importlib && !always_trim) {
            s = qp_getattr(ctx, code, atoms[4]);
            now = 0;
            if (!JS_IsException(s) && qp_str_check(ctx, s) && (p = qp_import_utf8(ctx, s, NULL))) {
                now = !strcmp(p, "_call_with_frames_removed");
                JS_FreeCString(ctx, p);
            }
            JS_FreeValue(ctx, s);
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, code);
                goto cleanup;
            }
        } else {
            now = always_trim;
        }
        JS_FreeValue(ctx, code);
        if (in_importlib && now) {
            if (outer < 0) {
                JS_FreeValue(ctx, base);
                base = JS_DupValue(ctx, next);
            } else if (qp_setattr(ctx, tbs[outer], atoms[0], next) < 0) {
                goto cleanup;
            }
            prev = outer;
        } else {
            prev = i;
        }
    }
    if (qp_exc_set_traceback(ctx, exc, base) < 0)
        goto cleanup;
    ok = 1;
 cleanup:
    if (!ok && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, tbs[i]);
    js_free(ctx, tbs);
    JS_FreeValue(ctx, base);
    for (i = 0; i < 5; i++)
        JS_FreeAtom(ctx, atoms[i]);
 restore:
    JS_Throw(ctx, exc);
}

/* PyImport_ImportModuleLevelObject (builtins.__import__ core).  globals,
   locals and fromlist may be QP_NULL. */
static JSValue qp_import_module_level(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      JSValueConst locals, JSValueConst fromlist, int level)
{
    JSValue abs_name = JS_EXCEPTION, mod = QP_NULL, final_mod = JS_EXCEPTION, front, to_return, path;
    const char *s = NULL, *abs_s = NULL;
    size_t len, abs_len;
    ssize_t dot;
    int has_from, rc;

    if (QP_IS_NULL(name))
        return qp_raise_value_error(ctx, "Empty module name");
    if (!qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "module name must be a string");
    if (level < 0)
        return qp_raise_value_error(ctx, "level must be >= 0");

    abs_name = qp_import_get_abs_name(ctx, name, globals, level);
    if (JS_IsException(abs_name))
        return abs_name;
    rc = qp_import_get_module(ctx, abs_name, &mod);
    if (rc < 0)
        goto error;
    if (rc == 0 || JS_IsUndefined(mod) || JS_IsNull(mod)) {
        /* (import_ensure_initialized needs module locks only with threads) */
        JS_FreeValue(ctx, mod);
        mod = qp_import_find_and_load(ctx, abs_name);
        if (JS_IsException(mod)) {
            mod = QP_NULL;
            goto error;
        }
    } else if (QP_STATE(ctx)->config.import_time == 2) {
        /* import_ensure_initialized: -X importtime=2 reports the modules
           already loaded */
        qp_import_time_cached(ctx, abs_name);
    }

    has_from = 0;
    if (!QP_IS_NULL(fromlist) && !JS_IsUndefined(fromlist) && !JS_IsNull(fromlist)) {
        has_from = qp_bool(ctx, fromlist);
        if (has_from < 0)
            goto error;
    }
    if (!has_from) {
        s = qp_import_utf8(ctx, name, &len);
        if (!s)
            goto error;
        if (level == 0 || len > 0) {
            dot = qp_import_find_dot(s, len, 1);
            if (dot < 0) {
                final_mod = JS_DupValue(ctx, mod);
                goto error;
            }
            if (level == 0) {
                front = qp_str_from_utf8(ctx, s, dot);
                if (JS_IsException(front))
                    goto error;
                final_mod = qp_import_module_level(ctx, front, QP_NULL, QP_NULL, QP_NULL, 0);
                JS_FreeValue(ctx, front);
            } else {
                size_t cut_off = len - dot;
                abs_s = qp_import_utf8(ctx, abs_name, &abs_len);
                if (!abs_s)
                    goto error;
                to_return = qp_str_from_utf8(ctx, abs_s, abs_len - cut_off);
                if (JS_IsException(to_return))
                    goto error;
                rc = qp_import_get_module(ctx, to_return, &final_mod);
                if (rc <= 0) {
                    if (rc == 0)
                        qp_raise(ctx, QP_EXC(ctx, KeyError), "%R not in sys.modules as expected",
                                 to_return);
                    final_mod = JS_EXCEPTION;
                }
                JS_FreeValue(ctx, to_return);
            }
        } else {
            final_mod = JS_DupValue(ctx, mod);
        }
    } else {
        rc = qp_getattr_opt(ctx, mod, QP_ATOM(ctx, __path__), &path);
        if (rc < 0)
            goto error;
        if (rc > 0) {
            JS_FreeValue(ctx, path);
            if (JS_IsObject(QP_STATE(ctx)->importlib)) {
                JSValueConst args[3] = { mod, fromlist, QP_STATE(ctx)->import_func };
                if (!JS_IsObject(args[2]))
                    args[2] = JS_UNDEFINED;
                final_mod = qp_call_method(ctx, QP_STATE(ctx)->importlib,
                                           QP_ATOM(ctx, _handle_fromlist), args, 3, QP_NULL);
            } else if (qp_import_handle_fromlist(ctx, mod, fromlist, 0) == 0) {
                final_mod = JS_DupValue(ctx, mod);
            }
        } else {
            final_mod = JS_DupValue(ctx, mod);
        }
    }
 error:
    if (s)
        JS_FreeCString(ctx, s);
    if (abs_s)
        JS_FreeCString(ctx, abs_s);
    JS_FreeValue(ctx, abs_name);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(final_mod))
        qp_import_remove_importlib_frames(ctx);
    return final_mod;
}

/* PyImport_Import (and PyImport_ImportModule): __import__ of the current
   builtins with fromlist ['__doc__'], then sys.modules[name]. */
static JSValue qp_import_module(JSContext *ctx, const char *name)
{
    JSValue name_str, import_func, from_list, doc, globals, r, args[5];
    int rc;

    name_str = qp_str_from_cstr(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    globals = qp_frame_globals(qp_py_frame_at(ctx, 0));
    rc = qp_import_mapping_get(ctx, qp_import_current_builtins(ctx), QP_ATOM(ctx, __import__),
                               &import_func);
    if (rc <= 0) {
        if (rc == 0) {
            JSValue key = qp_str_from_atom(ctx, QP_ATOM(ctx, __import__));
            if (!JS_IsException(key)) {
                qp_raise_key_error(ctx, key);
                JS_FreeValue(ctx, key);
            }
        }
        JS_FreeValue(ctx, name_str);
        return JS_EXCEPTION;
    }
    from_list = qp_list_new(ctx, 0);
    doc = qp_str_from_atom(ctx, QP_ATOM(ctx, __doc__));
    if (JS_IsException(from_list) || JS_IsException(doc) ||
        qp_list_append(ctx, from_list, doc) < 0) {
        JS_FreeValue(ctx, from_list);
        JS_FreeValue(ctx, doc);
        JS_FreeValue(ctx, import_func);
        JS_FreeValue(ctx, name_str);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, doc);
    args[0] = name_str;
    args[1] = QP_IS_NULL(globals) ? JS_UNDEFINED : globals;
    args[2] = args[1];
    args[3] = from_list;
    args[4] = JS_NewInt32(ctx, 0);
    if (JS_VALUE_GET_PTR(import_func) == JS_VALUE_GET_PTR(QP_STATE(ctx)->import_func) &&
        JS_IsObject(import_func))
        r = qp_import_module_level(ctx, name_str, QP_IS_NULL(globals) ? QP_NULL : globals,
                                   QP_NULL, from_list, 0);
    else
        r = qp_vectorcall(ctx, import_func, (JSValueConst *)args, 5, QP_NULL);
    JS_FreeValue(ctx, from_list);
    JS_FreeValue(ctx, import_func);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, name_str);
        return r;
    }
    JS_FreeValue(ctx, r);
    rc = qp_import_get_module(ctx, name_str, &r);
    if (rc == 0)
        qp_raise_key_error(ctx, name_str);
    JS_FreeValue(ctx, name_str);
    return rc > 0 ? r : JS_EXCEPTION;
}

/* ---- lazy imports (PEP 810) ------------------------------------------- */

/* lazy_import instances are PY_OBJECT objects with these QPInstance slots */
enum {
    QP_LZ_BUILTINS,     /* root: builtins mapping; projection: QP_NULL */
    QP_LZ_FROM,         /* root: absolute name (str); projection: source lazy_import */
    QP_LZ_ATTR,         /* root: fromlist (tuple/str) or QP_NULL; projection: attribute name (str) */
    QP_LZ_FILENAME,     /* declaration location: co_filename (str) or QP_NULL */
    QP_LZ_FUNCNAME,     /* co_name (str) */
    QP_LZ_LINENO,       /* int */
    QP_LZ_STATE,        /* int: QP_LZ_RESOLVING | QP_LZ_ACTIVE */
    QP_LZ_NSLOTS
};
#define QP_LZ_RESOLVING 1
#define QP_LZ_ACTIVE    2

static BOOL qp_lazy_import_check(JSValueConst v)
{
    JSObject *p, *proto;
    QPType *t;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_OBJECT)
        return FALSE;
    proto = p->shape->proto;
    if (!proto || proto->class_id != JS_CLASS_PY_TYPE)
        return FALSE;
    t = proto->u.opaque;
    return t && t->ctx->py && t == t->ctx->py->types[QP_T_lazy_import];
}

static JSValue *qp_lz_slots(JSValueConst v)
{
    return qp_instance_slots(v);
}

static BOOL qp_lazy_import_is_resolving(JSValueConst v)
{
    JSValue *sl = qp_lz_slots(v);
    return sl && (JS_VALUE_GET_INT(sl[QP_LZ_STATE]) & QP_LZ_RESOLVING) != 0;
}

static void qp_lz_set_state(JSValueConst v, int flag, int on)
{
    JSValue *sl = qp_lz_slots(v);
    int st = JS_VALUE_GET_INT(sl[QP_LZ_STATE]);
    sl[QP_LZ_STATE] = JS_NewInt32(NULL, on ? (st | flag) : (st & ~flag));
}

/* _PyLazyImport_New: builtins QP_NULL for a projection (name is then a
   lazy_import and fromlist the attribute name). */
static JSValue qp_lazy_import_new(JSContext *ctx, JSValueConst builtins, JSValueConst name,
                                  JSValueConst fromlist)
{
    JSValue obj, *sl;
    struct JSStackFrame *sf;
    JSFunctionBytecode *b;
    QPLoc loc;

    if (QP_IS_NULL(name) || !(qp_str_check(ctx, name) || qp_lazy_import_check(name)))
        return qp_raise_type_error(ctx, "expected str or lazy_import for name");
    if (JS_IsUndefined(fromlist) || JS_IsNull(fromlist))
        fromlist = QP_NULL;
    else if (!QP_IS_NULL(fromlist) && !qp_str_check(ctx, fromlist) && !qp_tuple_check(ctx, fromlist))
        return qp_raise_type_error(ctx, "lazy_import: fromlist must be None, a string, or a tuple");
    obj = qp_object_new_instance(ctx, QP_TYPE(ctx, lazy_import), JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    sl = qp_lz_slots(obj);
    sl[QP_LZ_BUILTINS] = JS_DupValue(ctx, builtins);
    sl[QP_LZ_FROM] = JS_DupValue(ctx, name);
    sl[QP_LZ_ATTR] = JS_DupValue(ctx, fromlist);
    sl[QP_LZ_FILENAME] = QP_NULL;
    sl[QP_LZ_FUNCNAME] = QP_NULL;
    sl[QP_LZ_LINENO] = JS_NewInt32(ctx, -1);
    sl[QP_LZ_STATE] = JS_NewInt32(ctx, 0);

    /* declaration location: the running Python frame */
    sf = qp_py_frame_at(ctx, 0);
    if (sf && JS_VALUE_GET_TAG(sf->cur_func) == JS_TAG_OBJECT) {
        b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
        if (b && b->has_debug && sf->cur_pc) {
            if (qp_code_addr2loc(b, (uint32_t)(sf->cur_pc - b->byte_code_buf), &loc) < 0)
                loc.lineno = -1;
            sl[QP_LZ_FILENAME] = qp_str_from_atom(ctx, b->debug.filename);
            sl[QP_LZ_FUNCNAME] = qp_str_from_atom(ctx, b->func_name);
            sl[QP_LZ_LINENO] = JS_NewInt32(ctx, loc.lineno);
            if (JS_IsException(sl[QP_LZ_FILENAME]) || JS_IsException(sl[QP_LZ_FUNCNAME])) {
                sl[QP_LZ_FILENAME] = QP_NULL;
                sl[QP_LZ_FUNCNAME] = QP_NULL;
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
        }
    }
    return obj;
}

/* lazy_import_path: dotted name of the object resolution returns. */
static JSValue qp_lazy_import_path(JSContext *ctx, JSValueConst v)
{
    JSValue *sl = qp_lz_slots(v), base, res;
    const char *s;
    size_t len;
    ssize_t dot;

    if (qp_lazy_import_check(sl[QP_LZ_FROM])) {
        base = qp_lazy_import_path(ctx, sl[QP_LZ_FROM]);
        if (JS_IsException(base))
            return base;
        res = qp_str_from_format(ctx, "%U.%U", base, sl[QP_LZ_ATTR]);
        JS_FreeValue(ctx, base);
        return res;
    }
    if (!QP_IS_NULL(sl[QP_LZ_ATTR]) &&
        (!qp_tuple_check(ctx, sl[QP_LZ_ATTR]) || QP_TUPLE_SIZE(sl[QP_LZ_ATTR]) > 0))
        return JS_DupValue(ctx, sl[QP_LZ_FROM]);
    /* __import__("a.b") returns the top-level package `a`. */
    s = qp_import_utf8(ctx, sl[QP_LZ_FROM], &len);
    if (!s)
        return JS_EXCEPTION;
    dot = qp_import_find_dot(s, len, 1);
    res = dot < 0 ? JS_DupValue(ctx, sl[QP_LZ_FROM]) : qp_str_from_utf8(ctx, s, dot);
    JS_FreeCString(ctx, s);
    return res;
}

/* lazy_import_name */
static JSValue qp_lazy_import_name(JSContext *ctx, JSValueConst v)
{
    JSValue *sl = qp_lz_slots(v);

    if (qp_lazy_import_check(sl[QP_LZ_FROM]))
        return qp_lazy_import_path(ctx, v);
    if (!QP_IS_NULL(sl[QP_LZ_ATTR]) &&
        (!qp_tuple_check(ctx, sl[QP_LZ_ATTR]) || QP_TUPLE_SIZE(sl[QP_LZ_ATTR]) > 0))
        return qp_str_from_format(ctx, "%U...", sl[QP_LZ_FROM]);
    return JS_DupValue(ctx, sl[QP_LZ_FROM]);
}

static JSValue qp_lazy_import_repr(JSContext *ctx, JSValueConst self)
{
    JSValue name = qp_lazy_import_name(ctx, self), res;
    if (JS_IsException(name))
        return name;
    res = qp_str_from_format(ctx, "<%T '%U'>", self, name);
    JS_FreeValue(ctx, name);
    return res;
}

static JSValue qp_lazy_import_getattro(JSContext *ctx, JSValueConst self, JSAtom name)
{
    JSValue v = qp_generic_getattr(ctx, self, name), lz_name, name_str;

    if (!JS_IsException(v) || !qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)))
        return v;
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
    lz_name = qp_lazy_import_name(ctx, self);
    if (JS_IsException(lz_name))
        return lz_name;
    name_str = qp_str_from_atom(ctx, name);
    if (!JS_IsException(name_str))
        qp_raise(ctx, QP_EXC(ctx, AttributeError),
                 "cannot access attribute %R on unresolved lazy import %R", name_str, lz_name);
    JS_FreeValue(ctx, name_str);
    JS_FreeValue(ctx, lz_name);
    return JS_EXCEPTION;
}

static JSValue qp_import_name_with_import(JSContext *ctx, JSValueConst import_func,
                                          JSValueConst globals, JSValueConst locals,
                                          JSValueConst name, JSValueConst fromlist, int level);
static JSValue qp_import_from_eager(JSContext *ctx, JSValueConst v, JSAtom name);
static JSValue qp_lazy_import_load(JSContext *ctx, JSValueConst lazy_import);

/* lazy_import_resolve_result (consumes obj) */
static JSValue qp_lazy_import_resolve_result(JSContext *ctx, JSValue obj)
{
    JSValue r;
    if (JS_IsException(obj) || !qp_lazy_import_check(obj))
        return obj;
    r = qp_lazy_import_load(ctx, obj);
    JS_FreeValue(ctx, obj);
    return r;
}

/* lazy_import_replay_from */
static JSValue qp_lazy_import_replay_from(JSContext *ctx, JSValueConst mod, JSValueConst lz)
{
    JSValue *sl = qp_lz_slots(lz), from, obj;
    JSAtom a;

    if (!qp_lazy_import_check(sl[QP_LZ_FROM]))
        return JS_DupValue(ctx, mod);
    from = qp_lazy_import_replay_from(ctx, mod, sl[QP_LZ_FROM]);
    if (JS_IsException(from))
        return from;
    a = qp_str_to_atom(ctx, sl[QP_LZ_ATTR]);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, from);
        return JS_EXCEPTION;
    }
    obj = qp_import_from_eager(ctx, from, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, from);
    return qp_lazy_import_resolve_result(ctx, obj);
}

/* lazy_import_add_exception_cause: attach the declaration location to the
   pending resolution error. */
static void qp_lazy_import_add_exception_cause(JSContext *ctx, JSValueConst lz)
{
    JSValue *sl = qp_lz_slots(lz), exc, name, note, notes, msg, cause, v;
    int has_chain = 0, rc;

    if (QP_IS_NULL(sl[QP_LZ_FILENAME]) || JS_VALUE_GET_INT(sl[QP_LZ_LINENO]) < 0)
        return;
    exc = qp_exc_fetch(ctx);
    if (!qp_exc_check(ctx, exc)) {
        JS_Throw(ctx, exc);
        return;
    }
    name = qp_lazy_import_name(ctx, lz);
    if (JS_IsException(name))
        goto done;
    v = qp_getattr(ctx, exc, QP_ATOM(ctx, __cause__));
    if (!JS_IsException(v)) {
        has_chain |= !JS_IsUndefined(v) && !JS_IsNull(v);
        JS_FreeValue(ctx, v);
    }
    v = qp_getattr(ctx, exc, QP_ATOM(ctx, __context__));
    if (!JS_IsException(v)) {
        has_chain |= !JS_IsUndefined(v) && !JS_IsNull(v);
        JS_FreeValue(ctx, v);
    }
    v = qp_getattr(ctx, exc, QP_ATOM(ctx, __suppress_context__));
    if (!JS_IsException(v)) {
        has_chain |= JS_VALUE_GET_TAG(v) == JS_TAG_BOOL && JS_VALUE_GET_BOOL(v);
        JS_FreeValue(ctx, v);
    }
    if (has_chain) {
        /* preserve the original chain, including `raise ... from None` */
        note = qp_str_from_format(ctx, "lazy import of '%U' declared in %U at %U:%d",
                                  name, sl[QP_LZ_FUNCNAME], sl[QP_LZ_FILENAME],
                                  JS_VALUE_GET_INT(sl[QP_LZ_LINENO]));
        if (!JS_IsException(note)) {
            rc = qp_getattr_opt(ctx, exc, QP_ATOM(ctx, __notes__), &notes);
            if (rc >= 0) {
                if (rc == 0 || qp_contains(ctx, notes, note) == 0) {
                    v = qp_call_method(ctx, exc, QP_ATOM(ctx, add_note), (JSValueConst *)&note, 1,
                                       QP_NULL);
                    JS_FreeValue(ctx, v);
                }
                JS_FreeValue(ctx, notes);
            }
            JS_FreeValue(ctx, note);
        }
    } else {
        msg = qp_str_from_format(ctx, "lazy import of '%U' raised an exception during resolution",
                                 name);
        if (!JS_IsException(msg)) {
            cause = qp_exc_new(ctx, QP_EXC(ctx, ImportError), 1, (JSValueConst *)&msg);
            JS_FreeValue(ctx, msg);
            if (!JS_IsException(cause)) {
                qp_setattr(ctx, exc, QP_ATOM(ctx, __cause__), cause);
                JS_FreeValue(ctx, cause);
            }
        }
    }
    JS_FreeValue(ctx, name);
 done:
    /* errors while decorating are dropped: the resolution error wins */
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_Throw(ctx, exc);
}

/* lazy_import_resolve_impl / _PyImport_LoadLazyImportTstate */
static JSValue qp_lazy_import_load(JSContext *ctx, JSValueConst lazy_import)
{
    JSValueConst root = lazy_import, first = QP_NULL, globals, item;
    JSValue *rsl, fromlist = QP_NULL, import_func = QP_NULL;
    JSValue obj = JS_EXCEPTION, name, errmsg, from;
    int rc;

    while (qp_lazy_import_check(qp_lz_slots(root)[QP_LZ_FROM])) {
        first = root;
        root = qp_lz_slots(root)[QP_LZ_FROM];
    }
    rsl = qp_lz_slots(root);
    if (qp_lazy_import_is_resolving(lazy_import)) {
        name = qp_lazy_import_name(ctx, lazy_import);
        if (JS_IsException(name))
            return name;
        errmsg = qp_str_from_format(ctx, "cannot import name %R (most likely due to a circular import)",
                                    name);
        JS_FreeValue(ctx, name);
        return qp_import_raise_error(ctx, QP_EXC(ctx, ImportCycleError), errmsg,
                                     rsl[QP_LZ_FROM], QP_NULL, QP_NULL);
    }
    qp_lz_set_state(lazy_import, QP_LZ_RESOLVING, 1);

    /* `from a import b, c`: import only the name being resolved; keep an
       empty tuple intact for custom __import__ hooks. */
    if (!QP_IS_NULL(first) && !QP_IS_NULL(rsl[QP_LZ_ATTR]) && qp_tuple_check(ctx, rsl[QP_LZ_ATTR]) &&
        QP_TUPLE_SIZE(rsl[QP_LZ_ATTR]) > 0) {
        item = qp_lz_slots(first)[QP_LZ_ATTR];
        fromlist = qp_tuple_pack(ctx, 1, &item);
    } else {
        fromlist = JS_DupValue(ctx, QP_IS_NULL(rsl[QP_LZ_ATTR]) ? JS_UNDEFINED : rsl[QP_LZ_ATTR]);
    }
    if (JS_IsException(fromlist)) {
        fromlist = QP_NULL;
        goto done;
    }
    globals = qp_frame_globals(qp_py_frame_at(ctx, 0));
    if (QP_IS_NULL(globals))
        globals = JS_UNDEFINED;
    rc = qp_import_mapping_get(ctx, rsl[QP_LZ_BUILTINS], QP_ATOM(ctx, __import__), &import_func);
    if (rc < 0)
        goto done;
    if (rc == 0) {
        qp_raise(ctx, QP_EXC(ctx, ImportError), "__import__ not found");
        goto done;
    }
    obj = qp_import_name_with_import(ctx, import_func, globals, globals, rsl[QP_LZ_FROM], fromlist, 0);
    obj = qp_lazy_import_resolve_result(ctx, obj);
    if (!JS_IsException(obj) && !QP_IS_NULL(first)) {
        from = obj;
        obj = qp_lazy_import_replay_from(ctx, from, lazy_import);
        JS_FreeValue(ctx, from);
    }
 done:
    if (JS_IsException(obj))
        qp_lazy_import_add_exception_cause(ctx, lazy_import);
    qp_lz_set_state(lazy_import, QP_LZ_RESOLVING, 0);
    JS_FreeValue(ctx, fromlist);
    JS_FreeValue(ctx, import_func);
    return obj;
}

/* _PyLazyImport_Reify: resolve v (consumed) and replace the placeholder
   stored under name in ns (when ns still holds it).  Non-lazy values are
   returned unchanged. */
static JSValue qp_lazy_import_reify(JSContext *ctx, JSValue v, JSAtom name, JSValueConst ns)
{
    JSValue value, current, key;
    int rc = 0;

    if (!qp_lazy_import_check(v))
        return v;
    value = qp_lazy_import_load(ctx, v);
    if (JS_IsException(value)) {
        JS_FreeValue(ctx, v);
        return value;
    }
    if (qp_dict_check_exact(ctx, ns)) {
        rc = qp_dict_get_item_atom(ctx, ns, name, &current);
        if (rc > 0) {
            if (JS_VALUE_GET_PTR(current) == JS_VALUE_GET_PTR(v) &&
                JS_VALUE_GET_TAG(current) == JS_TAG_OBJECT)
                rc = qp_dict_set_item_atom(ctx, ns, name, value);
            JS_FreeValue(ctx, current);
        }
    } else if (JS_IsObject(ns) && qp_type_of(ctx, ns)->slots.mp.mp_ass_subscript) {
        /* custom namespaces keep their mapping protocol */
        rc = qp_import_mapping_get(ctx, ns, name, &current);
        if (rc > 0) {
            if (JS_VALUE_GET_PTR(current) == JS_VALUE_GET_PTR(v) &&
                JS_VALUE_GET_TAG(current) == JS_TAG_OBJECT) {
                key = qp_str_from_atom(ctx, name);
                rc = JS_IsException(key) ? -1 : qp_setitem(ctx, ns, key, value);
                JS_FreeValue(ctx, key);
            }
            JS_FreeValue(ctx, current);
        }
    }
    /* read-only namespaces resolve without caching */
    JS_FreeValue(ctx, v);
    if (rc < 0) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    return value;
}

/* Value of a global after reification (consumes v).  The interpreter's
   load_global does not pass the namespace, so the placeholder stays in
   place and is resolved again on the next access (an import that is
   already in sys.modules). */
static JSValue qp_lazy_import_resolve(JSContext *ctx, JSValue v)
{
    JSValue r;
    if (!qp_lazy_import_check(v))
        return v;
    r = qp_lazy_import_load(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

/* lazy_import.resolve() */
static JSValue qp_lazy_import_resolve_meth(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                           int nargs, JSValueConst kwnames, int magic)
{
    return qp_lazy_import_load(ctx, self);
}

/* lazy_import_get_loaded_attr: concrete attribute of an initialized module
   already in sys.modules, QP_NULL otherwise (exceptions swallowed). */
static JSValue qp_lazy_import_get_loaded_attr(JSContext *ctx, JSValueConst name, JSAtom attr_name)
{
    JSValue mod = QP_NULL, spec = QP_NULL, attr = QP_NULL;
    int rc;

    rc = qp_import_get_module(ctx, name, &mod);
    if (rc <= 0 || !qp_module_check(ctx, mod))
        goto done;
    if (qp_getattr_opt(ctx, mod, QP_ATOM(ctx, __spec__), &spec) < 0 ||
        qp_module_spec_is_initializing(ctx, spec) != 0)
        goto done;
    if (qp_dict_get_item_atom(ctx, qp_module_get_dict(mod), attr_name, &attr) < 0)
        goto done;
    if (!QP_IS_NULL(attr) && qp_lazy_import_check(attr)) {
        JS_FreeValue(ctx, attr);
        attr = QP_NULL;
    }
 done:
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, mod);
    if (JS_HasException(ctx) && qp_exc_pending_matches(ctx, QP_EXC(ctx, Exception)))
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    return attr;
}

/* _PyEval_LazyImportFrom */
static JSValue qp_lazy_import_from(JSContext *ctx, JSValueConst v, JSAtom name)
{
    JSValue *sl = qp_lz_slots(v), attr, name_str, res;

    if (!QP_IS_NULL(sl[QP_LZ_ATTR]) && qp_tuple_check(ctx, sl[QP_LZ_ATTR]) &&
        QP_TUPLE_SIZE(sl[QP_LZ_ATTR]) > 0) {
        attr = qp_lazy_import_get_loaded_attr(ctx, sl[QP_LZ_FROM], name);
        if (!QP_IS_NULL(attr) || JS_HasException(ctx))
            return QP_IS_NULL(attr) ? JS_EXCEPTION : attr;
    }
    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    res = qp_lazy_import_new(ctx, QP_NULL, v, name_str);
    JS_FreeValue(ctx, name_str);
    return res;
}

/* _PyImport_LazyImportModuleLevelObject (default builtins.__lazy_import__
   core).  Module level means the import runs with globals as its locals. */
static JSValue qp_lazy_import_module_level(JSContext *ctx, JSValueConst name, JSValueConst builtins,
                                           JSValueConst globals, JSValueConst locals,
                                           JSValueConst fromlist, int level)
{
    JSValue abs_name, res = JS_EXCEPTION, fl, item;
    Py_ssize_t i;

    if (QP_IS_NULL(name) || !qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "module name must be a string, got %T",
                                   QP_IS_NULL(name) ? JS_UNDEFINED : name);
    if (level < 0)
        return qp_raise_value_error(ctx, "level must be >= 0");
    abs_name = qp_import_get_abs_name(ctx, name, globals, level);
    if (JS_IsException(abs_name))
        return abs_name;
    if (QP_IS_NULL(globals) || QP_IS_NULL(locals) || !JS_IsObject(globals) ||
        JS_VALUE_GET_PTR(globals) != JS_VALUE_GET_PTR(locals)) {
        JS_FreeValue(ctx, abs_name);
        return qp_raise(ctx, QP_EXC(ctx, SyntaxError), "'lazy import' is only allowed at module level");
    }
    if (!QP_IS_NULL(fromlist) && qp_str_check(ctx, fromlist))
        fl = qp_tuple_pack(ctx, 1, &fromlist);
    else
        fl = JS_DupValue(ctx, QP_IS_NULL(fromlist) ? JS_UNDEFINED : fromlist);
    if (JS_IsException(fl))
        goto done;
    /* validate the entire fromlist before creating the placeholder */
    if (qp_tuple_check(ctx, fl)) {
        for (i = 0; i < QP_TUPLE_SIZE(fl); i++) {
            item = QP_TUPLE_ITEMS(fl)[i];
            if (!qp_str_check(ctx, item)) {
                qp_raise_type_error(ctx, "Item in ``from list'' must be str, not %.200s",
                                    qp_type_of(ctx, item)->tp_name ? qp_type_of(ctx, item)->tp_name
                                    : "object");
                goto done;
            }
        }
    }
    res = qp_lazy_import_new(ctx, QP_IS_NULL(builtins) ? qp_import_current_builtins(ctx) : builtins,
                             abs_name, fl);
 done:
    JS_FreeValue(ctx, fl);
    JS_FreeValue(ctx, abs_name);
    return res;
}

static const QPMethodDef qp_lazy_import_methods[] = {
    { "resolve", qp_lazy_import_resolve_meth, QP_METH_NOARGS, 0,
      "Resolve the lazy import and return the imported object." },
    { NULL }
};

static const QPTypeSpec qp_lazy_import_spec = {
    .name = "lazy_import",
    .doc = "lazy_import(builtins, name, fromlist=None, /)\n--\n\n"
           "Represents a lazy import that will be resolved on first use.\n\n"
           "Instances of this object accessed from the global scope will be\n"
           "automatically imported based upon their name and then replaced with\n"
           "the imported value.",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_LZ_NSLOTS,
    .slots = {
        .tp_repr = qp_lazy_import_repr,
        .tp_getattro = qp_lazy_import_getattro,
    },
    .methods = qp_lazy_import_methods,
};

/* ---- IMPORT_NAME / IMPORT_FROM / import * ----------------------------- */

/* _PyEval_ImportNameWithImport */
static JSValue qp_import_name_with_import(JSContext *ctx, JSValueConst import_func,
                                          JSValueConst globals, JSValueConst locals,
                                          JSValueConst name, JSValueConst fromlist, int level)
{
    JSValueConst args[5];

    if (QP_IS_NULL(locals))
        locals = JS_UNDEFINED;
    /* fast path for the original builtins.__import__ */
    if (JS_IsObject(import_func) &&
        JS_VALUE_GET_PTR(import_func) == JS_VALUE_GET_PTR(QP_STATE(ctx)->import_func))
        return qp_import_module_level(ctx, name, globals, locals, fromlist, level);
    args[0] = name;
    args[1] = QP_IS_NULL(globals) ? JS_UNDEFINED : globals;
    args[2] = locals;
    args[3] = QP_IS_NULL(fromlist) ? JS_UNDEFINED : fromlist;
    args[4] = JS_NewInt32(ctx, level);
    return qp_vectorcall(ctx, import_func, args, 5, QP_NULL);
}

/* _PyEval_ImportName */
static JSValue qp_import_name_eager(JSContext *ctx, JSValueConst builtins, JSValueConst name,
                                    JSValueConst globals, JSValueConst locals,
                                    JSValueConst fromlist, int level)
{
    JSValue import_func, res;
    int rc = qp_import_mapping_get(ctx, builtins, QP_ATOM(ctx, __import__), &import_func);

    if (rc < 0)
        return JS_EXCEPTION;
    if (rc == 0)
        return qp_raise(ctx, QP_EXC(ctx, ImportError), "__import__ not found");
    res = qp_import_name_with_import(ctx, import_func, globals, locals, name, fromlist, level);
    JS_FreeValue(ctx, import_func);
    return res;
}

/* check_lazy_import_compatibility: globals['__lazy_modules__'] contains the
   absolute name.  1/0/-1. */
static int qp_import_check_lazy_modules(JSContext *ctx, JSValueConst globals, JSValueConst name,
                                        int level)
{
    JSValue lazy_modules, abs_name;
    int res;

    if (QP_IS_NULL(globals) || !JS_IsObject(globals))
        return 0;
    if (qp_import_mapping_get(ctx, globals, QP_ATOM(ctx, __lazy_modules__), &lazy_modules) < 0)
        return -1;
    if (QP_IS_NULL(lazy_modules))
        return 0;
    abs_name = qp_import_get_abs_name(ctx, name, globals, level);
    if (JS_IsException(abs_name)) {
        JS_FreeValue(ctx, lazy_modules);
        return -1;
    }
    res = qp_contains(ctx, lazy_modules, abs_name);
    JS_FreeValue(ctx, abs_name);
    JS_FreeValue(ctx, lazy_modules);
    return res;
}

/* IMPORT_NAME (bytecodes.c + _PyEval_LazyImportName). */
static JSValue qp_import_name(JSContext *ctx, JSValueConst name, JSValueConst globals,
                              JSValueConst locals, JSValueConst fromlist, int level, int mode)
{
    JSValue builtins = qp_import_current_builtins(ctx), lazy_import_func, res;
    JSValueConst args[5];
    int lazy, module_level, rc;

    if (mode == QP_IMPORT_EAGER)
        return qp_import_name_eager(ctx, builtins, name, globals, locals, fromlist, level);

    module_level = !QP_IS_NULL(globals) && !QP_IS_NULL(locals) && JS_IsObject(globals) &&
        JS_VALUE_GET_PTR(globals) == JS_VALUE_GET_PTR(locals);
    lazy = mode == QP_IMPORT_LAZY;
    /* the global policy overrides the local syntax */
    if (QP_STATE(ctx)->lazy_imports_mode == 1 && !lazy)
        lazy = module_level;
    if (!lazy && module_level) {
        /* __lazy_modules__ only applies at module level */
        lazy = qp_import_check_lazy_modules(ctx, globals, name, level);
        if (lazy < 0)
            return JS_EXCEPTION;
    }
    if (!lazy)
        return qp_import_name_eager(ctx, builtins, name, globals, locals, fromlist, level);

    rc = qp_import_mapping_get(ctx, builtins, QP_ATOM(ctx, __lazy_import__), &lazy_import_func);
    if (rc < 0)
        return JS_EXCEPTION;
    if (rc == 0)
        return qp_raise(ctx, QP_EXC(ctx, ImportError), "__lazy_import__ not found");
    args[0] = name;
    args[1] = QP_IS_NULL(globals) ? JS_UNDEFINED : globals;
    args[2] = QP_IS_NULL(locals) ? JS_UNDEFINED : locals;
    args[3] = QP_IS_NULL(fromlist) ? JS_UNDEFINED : fromlist;
    args[4] = JS_NewInt32(ctx, level);
    res = qp_vectorcall(ctx, lazy_import_func, args, 5, QP_NULL);
    JS_FreeValue(ctx, lazy_import_func);
    return res;
}

/* PyModule_GetFilenameObject: __file__ when it is a str, else QP_NULL. */
static JSValue qp_import_module_filename(JSContext *ctx, JSValueConst mod)
{
    JSValue f;
    JSValueConst d = qp_module_get_dict(mod);

    if (QP_IS_NULL(d) || qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __file__), &f) <= 0)
        return QP_NULL;
    if (!qp_str_check(ctx, f)) {
        JS_FreeValue(ctx, f);
        return QP_NULL;
    }
    return f;
}

/* _PyEval_ImportFrom */
static JSValue qp_import_from_eager(JSContext *ctx, JSValueConst v, JSAtom name)
{
    JSValue x, mod_name = QP_NULL, fullmodname, name_str = QP_NULL, mod_name_or_unknown = QP_NULL;
    JSValue spec = QP_NULL, origin = QP_NULL, errmsg = QP_NULL;
    int rc, shadowing, shadowing_stdlib = 0;

    rc = qp_getattr_opt(ctx, v, name, &x);
    if (rc != 0)
        return rc > 0 ? x : JS_EXCEPTION;
    /* circular relative import: fall back on sys.modules */
    if (qp_getattr_opt(ctx, v, QP_ATOM(ctx, __name__), &mod_name) < 0)
        return JS_EXCEPTION;
    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str)) {
        JS_FreeValue(ctx, mod_name);
        return name_str;
    }
    if (!QP_IS_NULL(mod_name) && qp_str_check(ctx, mod_name)) {
        fullmodname = qp_str_from_format(ctx, "%U.%U", mod_name, name_str);
        if (JS_IsException(fullmodname))
            goto fail;
        rc = qp_import_get_module(ctx, fullmodname, &x);
        JS_FreeValue(ctx, fullmodname);
        if (rc < 0)
            goto fail;
        if (rc > 0) {
            JS_FreeValue(ctx, mod_name);
            JS_FreeValue(ctx, name_str);
            return x;
        }
    } else {
        JS_FreeValue(ctx, mod_name);
        mod_name = QP_NULL;
    }

    mod_name_or_unknown = QP_IS_NULL(mod_name) ? qp_str_from_cstr(ctx, "<unknown module name>")
        : JS_DupValue(ctx, mod_name);
    if (JS_IsException(mod_name_or_unknown)) {
        mod_name_or_unknown = QP_NULL;
        goto fail;
    }
    if (qp_getattr_opt(ctx, v, QP_ATOM(ctx, __spec__), &spec) < 0)
        goto fail;
    if (QP_IS_NULL(spec)) {
        errmsg = qp_str_from_format(ctx, "cannot import name %R from %R (unknown location)",
                                    name_str, mod_name_or_unknown);
        goto raise;
    }
    if (qp_module_spec_get_file_origin(ctx, spec, &origin) < 0)
        goto fail;
    shadowing = qp_module_is_possibly_shadowing(ctx, origin);
    if (shadowing < 0)
        goto fail;
    if (shadowing) {
        shadowing_stdlib = qp_module_is_stdlib_name(ctx, mod_name_or_unknown);
        if (shadowing_stdlib < 0)
            goto fail;
    }
    if (QP_IS_NULL(origin) && qp_module_check(ctx, v))
        origin = qp_import_module_filename(ctx, v);

    if (shadowing_stdlib) {
        errmsg = qp_str_from_format(ctx, "cannot import name %R from %R "
                                    "(consider renaming %R since it has the same "
                                    "name as the standard library module named %R "
                                    "and prevents importing that standard library module)",
                                    name_str, mod_name_or_unknown, origin, mod_name_or_unknown);
    } else {
        rc = qp_module_spec_is_initializing(ctx, spec);
        if (rc < 0)
            goto fail;
        if (rc > 0) {
            if (shadowing)
                errmsg = qp_str_from_format(ctx, "cannot import name %R from %R "
                                            "(consider renaming %R if it has the same name "
                                            "as a library you intended to import)",
                                            name_str, mod_name_or_unknown, origin);
            else if (!QP_IS_NULL(origin))
                errmsg = qp_str_from_format(ctx, "cannot import name %R from partially initialized module %R "
                                            "(most likely due to a circular import) (%S)",
                                            name_str, mod_name_or_unknown, origin);
            else
                errmsg = qp_str_from_format(ctx, "cannot import name %R from partially initialized module %R "
                                            "(most likely due to a circular import)",
                                            name_str, mod_name_or_unknown);
        } else if (!QP_IS_NULL(origin)) {
            errmsg = qp_str_from_format(ctx, "cannot import name %R from %R (%S)",
                                        name_str, mod_name_or_unknown, origin);
        } else {
            errmsg = qp_str_from_format(ctx, "cannot import name %R from %R (unknown location)",
                                        name_str, mod_name_or_unknown);
        }
    }
 raise:
    qp_import_raise_error(ctx, QP_EXC(ctx, ImportError), errmsg, mod_name, origin, name_str);
 fail:
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, mod_name_or_unknown);
    JS_FreeValue(ctx, mod_name);
    JS_FreeValue(ctx, name_str);
    return JS_EXCEPTION;
}

/* IMPORT_FROM */
static JSValue qp_import_from(JSContext *ctx, JSValueConst module, JSAtom name)
{
    if (qp_lazy_import_check(module))
        return qp_lazy_import_from(ctx, module, name);
    return qp_import_from_eager(ctx, module, name);
}

/* import_all_from: INTRINSIC_IMPORT_STAR into the locals mapping. */
static int qp_import_star(JSContext *ctx, JSValueConst v, JSValueConst locals)
{
    JSValue all, dict, keys, name, value, modname, idx;
    JSAtom a;
    int skip_leading_underscores = 0, err = 0, rc;
    int64_t pos;

    if (QP_IS_NULL(locals) || JS_IsUndefined(locals)) {
        qp_raise_system_error(ctx, "no locals found during 'import *'");
        return -1;
    }
    if (qp_getattr_opt(ctx, v, QP_ATOM(ctx, __all__), &all) < 0)
        return -1;
    if (QP_IS_NULL(all)) {
        if (qp_getattr_opt(ctx, v, QP_ATOM(ctx, __dict__), &dict) < 0)
            return -1;
        if (QP_IS_NULL(dict)) {
            qp_raise(ctx, QP_EXC(ctx, ImportError), "from-import-* object has no __dict__ and no __all__");
            return -1;
        }
        /* PyMapping_Keys */
        if (qp_anydict_check(ctx, dict)) {
            all = qp_dict_keys_list(ctx, dict);
        } else {
            keys = qp_call_method(ctx, dict, QP_ATOM(ctx, keys), NULL, 0, QP_NULL);
            all = JS_IsException(keys) ? keys : qp_list_from_iterable(ctx, keys);
            JS_FreeValue(ctx, keys);
        }
        JS_FreeValue(ctx, dict);
        if (JS_IsException(all))
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0;; pos++) {
        idx = qp_long_from_int64(ctx, pos);
        name = qp_getitem(ctx, all, idx);
        JS_FreeValue(ctx, idx);
        if (JS_IsException(name)) {
            if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, IndexError)))
                err = -1;
            else
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
            break;
        }
        if (!qp_str_check(ctx, name)) {
            modname = qp_getattr(ctx, v, QP_ATOM(ctx, __name__));
            if (!JS_IsException(modname)) {
                if (!qp_str_check(ctx, modname))
                    qp_raise_type_error(ctx, "module __name__ must be a string, not %.100s",
                                        qp_type_of(ctx, modname)->tp_name ?
                                        qp_type_of(ctx, modname)->tp_name : "object");
                else
                    qp_raise_type_error(ctx, "%s in %U.%s must be str, not %.100s",
                                        skip_leading_underscores ? "Key" : "Item", modname,
                                        skip_leading_underscores ? "__dict__" : "__all__",
                                        qp_type_of(ctx, name)->tp_name ?
                                        qp_type_of(ctx, name)->tp_name : "object");
                JS_FreeValue(ctx, modname);
            }
            JS_FreeValue(ctx, name);
            err = -1;
            break;
        }
        if (skip_leading_underscores) {
            JSValue first = qp_str_substring(ctx, name, 0, 1);
            const char *p;
            if (JS_IsException(first)) {
                JS_FreeValue(ctx, name);
                err = -1;
                break;
            }
            p = qp_import_utf8(ctx, first, NULL);
            JS_FreeValue(ctx, first);
            if (!p) {
                JS_FreeValue(ctx, name);
                err = -1;
                break;
            }
            rc = p[0] == '_';
            JS_FreeCString(ctx, p);
            if (rc) {
                JS_FreeValue(ctx, name);
                continue;
            }
        }
        a = qp_str_to_atom(ctx, name);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, name);
            err = -1;
            break;
        }
        value = qp_getattr(ctx, v, a);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(value))
            err = -1;
        else if (qp_dict_check_exact(ctx, locals))
            err = qp_dict_set_item(ctx, locals, name, value);
        else
            err = qp_setitem(ctx, locals, name, value);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, value);
        if (err < 0)
            break;
    }
    JS_FreeValue(ctx, all);
    return err;
}

/* ---- builtin modules defined by unit l and init ----------------------- */

/* _string (Modules/_stringmodule.c in CPython: string helper module); the
   functions are in unicode_format.c (unit d). */
static const QPModuleDef qp_moddef__string = {
    "_string", "string helper module", qp_string_module_methods, NULL,
};

/* Init of unit l (step 12): module and lazy_import types, sys.modules. */
static int qp_init_modules(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);

    if (qp_module_type_init(ctx) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, lazy_import), &qp_lazy_import_spec) < 0)
        return -1;
    s->modules = qp_dict_new(ctx);
    if (JS_IsException(s->modules)) {
        s->modules = JS_UNDEFINED;
        return -1;
    }
    s->importlib = JS_UNDEFINED;
    s->lazy_imports_mode = s->config.lazy_imports == 1 ? 1 : 0;
    return 0;
}
