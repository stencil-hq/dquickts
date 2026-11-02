/*
 * dquickts Python, milestone M2: the `sys` module, the pre-io standard
 * stream objects, struct sequences and types.SimpleNamespace.
 *
 * Derived from CPython Python/sysmodule.c, Objects/structseq.c,
 * Objects/namespaceobject.c, Python/thread.c (thread_info),
 * Objects/floatobject.c (float_info), Objects/longobject.c (int_info),
 * Python/errors.c (UnraisableHookArgs), Modules/_io/textio.c (text stream
 * behaviour of sys.std*) and Python/stdlib_module_names.h (PSF-2.0).
 *
 * Unit k.  qp_init_sys() is called by qp_init_builtins() (bltinmodule.c)
 * once the builtins module exists; it readies every k type that is not a
 * builtins type (typing objects through qp_init_typevar /
 * qp_init_genericalias), builds the sys module and registers `sys` and
 * `builtins` in sys.modules.
 *
 * settrace / setprofile are M6 (frameobject.c qp_trace_*); sys.monitoring
 * is python/instrumentation.c (tool ids, callbacks and event sets are
 * kept; events are not fired).
 *
 * Threads (M5): getswitchinterval/setswitchinterval drive the GIL of
 * python/ceval.c; _current_frames/_current_exceptions read the saved
 * state of the other threads.
 *
 * Gaps: remote debugging, lazy-import filters and other C-runtime
 * introspection hooks are absent (AttributeError, as on a CPython build
 * without the feature).
 */

#include <float.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dlfcn.h>

/* ---- functions of other k files / units not declared in object.h -------- */
static int qp_init_typevar(JSContext *ctx);                 /* typevarobject.c */
static int qp_init_genericalias(JSContext *ctx);            /* genericaliasobject.c */
static JSObject *qp_fr_sf_func(JSStackFrame *sf);             /* a: frameobject.c */
static JSValue qp_frame_of(JSContext *ctx, JSStackFrame *sf);  /* a: frameobject.c */
static int qp_unraisable_default_hook(JSContext *ctx, JSValueConst exc_type,
                                      JSValueConst exc_value, JSValueConst exc_tb,
                                      JSValueConst err_msg, JSValueConst obj); /* i: errors.c */

/* ---- exported to other units (forward-declared by them) ---------------- */
static int qp_init_sys(JSContext *ctx);
static int qp_sys_flush_std_files(JSContext *ctx);
static JSValue qp_unraisable_hook_args_new(JSContext *ctx, JSValueConst exc_type,
                                           JSValueConst exc_value, JSValueConst exc_tb,
                                           JSValueConst err_msg, JSValueConst obj);

#define QP_PY_VERSION       "3.15.2"
#define QP_PY_HEXVERSION    0x030F02F0
#define QP_PY_API_VERSION   1013
/* sys.implementation.cache_tag: the .pyc files hold QuickJS bytecode
   (python/mod_misc.c marshal), never mistaken for CPython's cpython-315 */
#define QP_CACHE_TAG        "quickpy-315"
#define QP_LONG_MAX_STR_DIGITS_THRESHOLD 640   /* _PY_LONG_MAX_STR_DIGITS_THRESHOLD */
#define QP_LONG_DEFAULT_MAX_STR_DIGITS 4300    /* _PY_LONG_DEFAULT_MAX_STR_DIGITS */
#define QP_IMMORTAL_REFCNT  3221225472LL       /* _Py_IMMORTAL_INITIAL_REFCNT (3 << 30) */

#if defined(__clang__)
#define QP_COMPILER "[Clang " __clang_version__ "]"
#elif defined(__GNUC__)
#define QP_COMPILER "[GCC " __VERSION__ "]"
#else
#define QP_COMPILER "[C]"
#endif

#if defined(__APPLE__)
#define QP_PLATFORM "darwin"
#elif defined(__linux__)
#define QP_PLATFORM "linux"
#elif defined(_WIN32)
#define QP_PLATFORM "win32"
#elif defined(__FreeBSD__)
#define QP_PLATFORM "freebsd"
#elif defined(__OpenBSD__)
#define QP_PLATFORM "openbsd"
#elif defined(__NetBSD__)
#define QP_PLATFORM "netbsd"
#elif defined(__EMSCRIPTEN__)
#define QP_PLATFORM "emscripten"
#elif defined(__wasi__)
#define QP_PLATFORM "wasi"
#else
#define QP_PLATFORM "unknown"
#endif

/* sys.implementation._multiarch (CPython's PLATFORM_TRIPLET), only defined
   where CPython sets it. */
#if defined(__APPLE__)
#define QP_MULTIARCH "darwin"
#elif defined(__linux__)
#if defined(__x86_64__)
#define QP_MULTIARCH "x86_64-linux-gnu"
#elif defined(__aarch64__)
#define QP_MULTIARCH "aarch64-linux-gnu"
#elif defined(__i386__)
#define QP_MULTIARCH "i386-linux-gnu"
#elif defined(__arm__)
#define QP_MULTIARCH "arm-linux-gnueabihf"
#elif defined(__riscv)
#define QP_MULTIARCH "riscv64-linux-gnu"
#else
#define QP_MULTIARCH "linux"
#endif
#endif

/* SOABI of extension modules built from source against this interpreter's
   C API headers (plan D5 Tier S, capi/include): sysconfig's SOABI and
   EXT_SUFFIX ("." QP_SOABI ".so") and the first entry of
   _imp.extension_suffixes(), as CPython's "cpython-315-<multiarch>". */
#ifdef QP_MULTIARCH
#define QP_SOABI "quickpy-315-" QP_MULTIARCH
#else
#define QP_SOABI "quickpy-315"
#endif

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

/* d[name] = v (consumes v; JS_EXCEPTION input propagates). */
static int qp_sys_dict_set(JSContext *ctx, JSValueConst d, const char *name, JSValue v)
{
    JSAtom atom;
    int ret;

    if (JS_IsException(v))
        return -1;
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    ret = qp_dict_set_item_atom(ctx, d, atom, v);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, v);
    return ret;
}

/* CPython tp_name of a type as a str: the spec name of builtin types, the
   bare __name__ of heap types. */
static JSValue qp_sys_tp_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* PyCallable_Check */
static BOOL qp_sys_callable(JSContext *ctx, JSValueConst v)
{
    if (JS_IsFunction(ctx, v))
        return TRUE;
    return qp_type_of(ctx, v)->slots.tp_call != NULL;
}

/* str from a C string that may hold undecodable bytes (argv, environment):
   UTF-8 with surrogateescape, like CPython's filesystem decoding. */
static JSValue qp_sys_fs_str(JSContext *ctx, const char *s)
{
    return qp_str_decode(ctx, s, (Py_ssize_t)strlen(s), "utf-8", "surrogateescape");
}

/* getenv honouring -E / -I (Py_GETENV). */
static const char *qp_sys_getenv(JSContext *ctx, const char *name)
{
    QPConfig *cfg = &QP_STATE(ctx)->config;
    if (cfg->ignore_environment || cfg->isolated)
        return NULL;
    return getenv(name);
}

static JSValue qp_sys_new_str_tuple(JSContext *ctx, int n, const char *const *names)
{
    JSValue *items;
    JSValue res;
    int i;

    items = js_malloc(ctx, sizeof(JSValue) * (n > 0 ? n : 1));
    if (!items)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        items[i] = qp_str_from_cstr(ctx, names[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            js_free(ctx, items);
            return JS_EXCEPTION;
        }
    }
    res = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    return res;
}

/* ========================================================================= */
/* Struct sequences of sys (shared machinery: python/structseq.c)             */
/* ========================================================================= */

/* The type objects live in QPState.sys_structseq_types (a tuple indexed by
   QP_SYS_SS_*), created by qp_sys_init_structseq_types. */
enum {
    QP_SYS_SS_VERSION_INFO,
    QP_SYS_SS_FLAGS,
    QP_SYS_SS_FLOAT_INFO,
    QP_SYS_SS_INT_INFO,
    QP_SYS_SS_HASH_INFO,
    QP_SYS_SS_THREAD_INFO,
    QP_SYS_SS_ASYNCGEN_HOOKS,
    QP_SYS_SS_UNRAISABLE_ARGS,
    QP_SYS_SS__COUNT
};

static const char *const qp_version_info_fields[] = {
    "major", "minor", "micro", "releaselevel", "serial", NULL
};
static const QPStructSeqDesc qp_version_info_desc = {
    "sys.version_info", "sys.version_info\n\nVersion information as a named tuple.",
    qp_version_info_fields, NULL, 5, QP_STRUCTSEQ_DISALLOW_INSTANTIATION
};

/* The last 4 fields are only usable by attribute name, not index. */
static const char *const qp_flags_fields[] = {
    "debug", "inspect", "interactive", "optimize", "dont_write_bytecode",
    "no_user_site", "no_site", "ignore_environment", "verbose", "bytes_warning",
    "quiet", "hash_randomization", "isolated", "dev_mode", "utf8_mode",
    "warn_default_encoding", "safe_path", "int_max_str_digits",
    "gil", "thread_inherit_context", "context_aware_warnings", "lazy_imports", NULL
};
static const QPStructSeqDesc qp_flags_desc = {
    "sys.flags", "sys.flags\n\nFlags provided through command line arguments or environment vars.",
    qp_flags_fields, NULL, 18, QP_STRUCTSEQ_DISALLOW_INSTANTIATION
};

static const char *const qp_float_info_fields[] = {
    "max", "max_exp", "max_10_exp", "min", "min_exp", "min_10_exp", "dig",
    "mant_dig", "epsilon", "radix", "rounds", NULL
};
static const QPStructSeqDesc qp_float_info_desc = {
    "sys.float_info",
    "sys.float_info\n\nA named tuple holding information about the float type. It contains low level\n"
    "information about the precision and internal representation. Please study\n"
    "your system's :file:`float.h` for more information.",
    qp_float_info_fields, NULL, 11, 0
};

static const char *const qp_int_info_fields[] = {
    "bits_per_digit", "sizeof_digit", "default_max_str_digits", "str_digits_check_threshold", NULL
};
static const QPStructSeqDesc qp_int_info_desc = {
    "sys.int_info",
    "sys.int_info\n\nA named tuple that holds information about Python's\n"
    "internal representation of integers.  The attributes are read only.",
    qp_int_info_fields, NULL, 4, 0
};

static const char *const qp_hash_info_fields[] = {
    "width", "modulus", "inf", "nan", "imag", "algorithm", "hash_bits", "seed_bits", "cutoff", NULL
};
static const QPStructSeqDesc qp_hash_info_desc = {
    "sys.hash_info",
    "hash_info\n\nA named tuple providing parameters used for computing\n"
    "hashes. The attributes are read only.",
    qp_hash_info_fields, NULL, 9, 0
};

static const char *const qp_thread_info_fields[] = { "name", "lock", "version", NULL };
static const QPStructSeqDesc qp_thread_info_desc = {
    "sys.thread_info",
    "sys.thread_info\n\nA named tuple holding information about the thread implementation.",
    qp_thread_info_fields, NULL, 3, 0
};

static const char *const qp_asyncgen_hooks_fields[] = { "firstiter", "finalizer", NULL };
static const QPStructSeqDesc qp_asyncgen_hooks_desc = {
    "asyncgen_hooks",
    "asyncgen_hooks\n\nA named tuple providing information about asynchronous\n"
    "generators hooks.  The attributes are read only.",
    qp_asyncgen_hooks_fields, NULL, 2, 0
};

static const char *const qp_unraisable_args_fields[] = {
    "exc_type", "exc_value", "exc_traceback", "err_msg", "object", NULL
};
static const QPStructSeqDesc qp_unraisable_args_desc = {
    "UnraisableHookArgs",
    "UnraisableHookArgs\n\nType used to pass arguments to sys.unraisablehook.",
    qp_unraisable_args_fields, NULL, 5, 0
};

static const QPStructSeqDesc *const qp_sys_ss_descs[QP_SYS_SS__COUNT] = {
    &qp_version_info_desc, &qp_flags_desc, &qp_float_info_desc, &qp_int_info_desc,
    &qp_hash_info_desc, &qp_thread_info_desc, &qp_asyncgen_hooks_desc,
    &qp_unraisable_args_desc,
};

static int qp_sys_init_structseq_types(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSValue items[QP_SYS_SS__COUNT], tup;
    int i;

    for (i = 0; i < QP_SYS_SS__COUNT; i++) {
        items[i] = qp_structseq_new_type(ctx, qp_sys_ss_descs[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            return -1;
        }
    }
    tup = qp_tuple_new_steal(ctx, QP_SYS_SS__COUNT, items);
    if (JS_IsException(tup))
        return -1;
    JS_FreeValue(ctx, st->sys_structseq_types);
    st->sys_structseq_types = tup;
    return 0;
}

static QPType *qp_sys_ss_type(JSContext *ctx, int which)
{
    return qp_type_from_value(QP_TUPLE_ITEMS(QP_STATE(ctx)->sys_structseq_types)[which]);
}

/* Instance of sys struct sequence `which`; steals its n_fields items. */
static JSValue qp_sys_ss_make(JSContext *ctx, int which, JSValue *items)
{
    return qp_structseq_new(ctx, qp_sys_ss_type(ctx, which), items);
}

static JSValue qp_none_if_null(JSContext *ctx, JSValueConst v)
{
    return QP_IS_NULL(v) ? JS_UNDEFINED : JS_DupValue(ctx, v);
}

static JSValue qp_unraisable_hook_args_new(JSContext *ctx, JSValueConst exc_type,
                                           JSValueConst exc_value, JSValueConst exc_tb,
                                           JSValueConst err_msg, JSValueConst obj)
{
    JSValue items[5];
    items[0] = qp_none_if_null(ctx, exc_type);
    items[1] = qp_none_if_null(ctx, exc_value);
    items[2] = qp_none_if_null(ctx, exc_tb);
    items[3] = qp_none_if_null(ctx, err_msg);
    items[4] = qp_none_if_null(ctx, obj);
    return qp_sys_ss_make(ctx, QP_SYS_SS_UNRAISABLE_ARGS, items);
}

/* ========================================================================= */
/* types.SimpleNamespace (Objects/namespaceobject.c)                          */
/* ========================================================================= */

static JSValue qp_namespace_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    return qp_object_new_instance(ctx, type, type->instance_class_id);
}

/* PyArg_ValidateKeywordArguments */
static int qp_namespace_check_keys(JSContext *ctx, JSValueConst dict)
{
    Py_ssize_t pos = 0;
    JSValue k, v;
    while (qp_dict_next(dict, &pos, &k, &v)) {
        if (!qp_str_check(ctx, k)) {
            qp_raise_type_error(ctx, "keywords must be strings");
            return -1;
        }
    }
    return 0;
}

/* SimpleNamespace(mapping_or_iterable=(), /, **kwargs) */
static int qp_namespace_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    Py_ssize_t nkw = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames), i;
    JSValue nsdict;
    const char *tname;
    int ret = -1;

    tname = JS_AtomToCString(ctx, qp_type_of(ctx, self)->name);
    if (!tname)
        return -1;
    if (qp_arg_check_positional(ctx, tname, nargs, 0, 1) < 0) {
        JS_FreeCString(ctx, tname);
        return -1;
    }
    JS_FreeCString(ctx, tname);
    nsdict = qp_object_get_dict(ctx, self);
    if (JS_IsException(nsdict))
        return -1;
    if (nargs == 1) {
        JSValue d;
        if (qp_dict_check_exact(ctx, args[0]))
            d = JS_DupValue(ctx, args[0]);
        else
            d = qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, dict)), args[0]);
        if (JS_IsException(d))
            goto done;
        if (qp_namespace_check_keys(ctx, d) < 0 || qp_dict_update(ctx, nsdict, d) < 0) {
            JS_FreeValue(ctx, d);
            goto done;
        }
        JS_FreeValue(ctx, d);
    }
    for (i = 0; i < nkw; i++) {
        if (qp_dict_set_item(ctx, nsdict, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0)
            goto done;
    }
    ret = 0;
done:
    JS_FreeValue(ctx, nsdict);
    return ret;
}

static JSValue qp_namespace_repr(JSContext *ctx, JSValueConst self)
{
    QPType *t = qp_type_of(ctx, self);
    JSValue name, nsdict = JS_UNDEFINED, pairs = JS_UNDEFINED, res = JS_EXCEPTION;
    JSValue sep, joined;
    Py_ssize_t pos = 0;
    JSValue k, v;
    int r;

    if (t == QP_TYPE(ctx, SimpleNamespace))
        name = qp_str_from_cstr(ctx, "namespace");
    else
        name = qp_sys_tp_name(ctx, t);
    if (JS_IsException(name))
        return name;
    r = qp_repr_enter(ctx, self);
    if (r != 0) {
        if (r > 0)
            res = qp_str_from_format(ctx, "%U(...)", name);
        JS_FreeValue(ctx, name);
        return res;
    }
    nsdict = qp_object_get_dict(ctx, self);
    if (JS_IsException(nsdict))
        goto done;
    pairs = qp_list_new(ctx, 0);
    if (JS_IsException(pairs))
        goto done;
    /* iterate over a snapshot of the keys: values' reprs may mutate the dict */
    {
        JSValue keys = qp_dict_keys_list(ctx, nsdict);
        Py_ssize_t i, n;
        if (JS_IsException(keys))
            goto done;
        n = qp_list_size(keys);
        for (i = 0; i < n; i++) {
            JSValue key = JS_GetPropertyInt64(ctx, keys, i);
            JSValue item;
            int has;
            if (JS_IsException(key)) {
                JS_FreeValue(ctx, keys);
                goto done;
            }
            if (qp_str_check(ctx, key) && qp_str_len(ctx, key) > 0) {
                has = qp_dict_get_item(ctx, nsdict, key, &v);
                if (has < 0) {
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, keys);
                    goto done;
                }
                if (has) {
                    item = qp_str_from_format(ctx, "%U=%R", key, v);
                    JS_FreeValue(ctx, v);
                    if (JS_IsException(item) || qp_list_append(ctx, pairs, item) < 0) {
                        JS_FreeValue(ctx, item);
                        JS_FreeValue(ctx, key);
                        JS_FreeValue(ctx, keys);
                        goto done;
                    }
                    JS_FreeValue(ctx, item);
                }
            }
            JS_FreeValue(ctx, key);
        }
        JS_FreeValue(ctx, keys);
    }
    (void)pos; (void)k;
    sep = qp_str_from_cstr(ctx, ", ");
    if (JS_IsException(sep))
        goto done;
    {
        Py_ssize_t n;
        JSValue *items = qp_list_items(pairs, &n);
        if (items) {
            joined = qp_str_join_array(ctx, sep, (int)n, (JSValueConst *)items);
        } else {
            JSValue tup = qp_list_as_tuple(ctx, pairs);
            if (JS_IsException(tup)) {
                JS_FreeValue(ctx, sep);
                goto done;
            }
            joined = qp_str_join_array(ctx, sep, (int)QP_TUPLE_SIZE(tup),
                                       (JSValueConst *)QP_TUPLE_ITEMS(tup));
            JS_FreeValue(ctx, tup);
        }
    }
    JS_FreeValue(ctx, sep);
    if (JS_IsException(joined))
        goto done;
    res = qp_str_from_format(ctx, "%U(%U)", name, joined);
    JS_FreeValue(ctx, joined);
done:
    qp_repr_leave(ctx, self);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, nsdict);
    JS_FreeValue(ctx, pairs);
    return res;
}

static JSValue qp_namespace_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    QPType *ns = QP_TYPE(ctx, SimpleNamespace);
    JSValue da, db, res;

    if ((op == QP_CMP_EQ || op == QP_CMP_NE) && qp_typecheck(ctx, a, ns) && qp_typecheck(ctx, b, ns)) {
        da = qp_object_get_dict(ctx, a);
        if (JS_IsException(da))
            return da;
        db = qp_object_get_dict(ctx, b);
        if (JS_IsException(db)) {
            JS_FreeValue(ctx, da);
            return db;
        }
        res = qp_richcompare(ctx, da, db, op);
        JS_FreeValue(ctx, da);
        JS_FreeValue(ctx, db);
        return res;
    }
    return qp_not_implemented(ctx);
}

/* __reduce__: (type, (), dict) */
static JSValue qp_namespace_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    JSValue items[3];
    items[2] = qp_object_get_dict(ctx, self);
    if (JS_IsException(items[2]))
        return items[2];
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    return qp_tuple_new_steal(ctx, 3, items);
}

/* __replace__(**kwargs): type(self)() updated with self.__dict__ and kwargs */
static JSValue qp_namespace_replace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_of(ctx, self);
    JSValue res, rd, sd;
    Py_ssize_t nkw = QP_IS_NULL(kwnames) ? 0 : QP_TUPLE_SIZE(kwnames), i;

    if (nargs > 0)
        return qp_raise_type_error(ctx, "__replace__() takes no positional arguments");
    res = qp_call0(ctx, QP_TYPE_VALUE(t));
    if (JS_IsException(res))
        return res;
    if (!qp_typecheck(ctx, res, QP_TYPE(ctx, SimpleNamespace))) {
        qp_raise_type_error(ctx, "expect %N type, but %T() returned '%T' object",
                            QP_TYPE(ctx, SimpleNamespace), self, res);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    rd = qp_object_get_dict(ctx, res);
    if (JS_IsException(rd))
        goto fail;
    sd = qp_object_get_dict(ctx, self);
    if (JS_IsException(sd)) {
        JS_FreeValue(ctx, rd);
        goto fail;
    }
    if (qp_dict_update(ctx, rd, sd) < 0) {
        JS_FreeValue(ctx, rd);
        JS_FreeValue(ctx, sd);
        goto fail;
    }
    JS_FreeValue(ctx, sd);
    for (i = 0; i < nkw; i++) {
        if (qp_dict_set_item(ctx, rd, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0) {
            JS_FreeValue(ctx, rd);
            goto fail;
        }
    }
    JS_FreeValue(ctx, rd);
    return res;
fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static const QPMethodDef qp_namespace_methods[] = {
    { "__reduce__", qp_namespace_reduce, QP_METH_NOARGS, 0, "Return state information for pickling" },
    { "__replace__", qp_namespace_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "__replace__($self, /, **changes)\n--\n\nReturn a new namespace object replacing specified fields with new values." },
    { NULL }
};

static JSValue qp_namespace_get_dict(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_object_get_dict(ctx, self);
}

/* namespace_members: __dict__ (T_OBJECT, READONLY) */
static const QPGetSetDef qp_namespace_getsets[] = {
    { "__dict__", qp_namespace_get_dict, NULL, 0, NULL, QP_GETSET_MEMBER },
    { NULL }
};

static const QPTypeSpec qp_namespace_spec = {
    .name = "types.SimpleNamespace",
    .doc = "SimpleNamespace(mapping_or_iterable=(), /, **kwargs)\n--\n\n"
           "A simple attribute-based namespace.",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_MANAGED_DICT,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_namespace_repr,
        .tp_richcompare = qp_namespace_richcompare,
        .tp_init = qp_namespace_init,
        .tp_new = qp_namespace_new,
    },
    .methods = qp_namespace_methods,
    .getsets = qp_namespace_getsets,
};

/* SimpleNamespace from (name, value) C pairs: values consumed. */
static JSValue qp_namespace_from_pairs(JSContext *ctx, int n, const char *const *names, JSValue *vals)
{
    JSValue ns, d;
    int i, failed = 0;

    ns = qp_object_new_instance(ctx, QP_TYPE(ctx, SimpleNamespace), JS_CLASS_PY_OBJECT);
    if (JS_IsException(ns))
        failed = 1;
    d = failed ? JS_EXCEPTION : qp_object_get_dict(ctx, ns);
    if (JS_IsException(d))
        failed = 1;
    for (i = 0; i < n; i++) {
        if (failed || JS_IsException(vals[i]) ||
            qp_sys_dict_set(ctx, d, names[i], JS_DupValue(ctx, vals[i])) < 0)
            failed = 1;
        JS_FreeValue(ctx, vals[i]);
    }
    if (!JS_IsException(d))
        JS_FreeValue(ctx, d);
    if (failed) {
        JS_FreeValue(ctx, ns);
        return JS_EXCEPTION;
    }
    return ns;
}

/* ========================================================================= */
/* Standard streams (pre-io sys.stdin / sys.stdout / sys.stderr)              */
/* ========================================================================= */

/* A small text stream over a file descriptor with TextIOWrapper's
   observable behaviour: str-only write() returning the number of code
   points, encoding at write time (errors raised by write), output buffered
   until flush() / 8 KiB / newline (line_buffering) / every write
   (write_through), universal newlines on input.  Fixed fields live in the
   QPInstance slots below. */
enum {
    QP_SP_FD,           /* int */
    QP_SP_NAME,         /* str: '<stdout>' */
    QP_SP_MODE,         /* str: 'r' / 'w' */
    QP_SP_ENCODING,     /* str */
    QP_SP_ERRORS,       /* str */
    QP_SP_NEWLINE,      /* None or str (output translation of '\n') */
    QP_SP_BUF,          /* output: pending str; input: bytes read ahead; QP_NULL = empty */
    QP_SP_POS,          /* output: pending code points; input: read offset in QP_SP_BUF */
    QP_SP_FLAGS,        /* int, QP_SPF_* */
    QP_SP_NSLOTS
};
#define QP_SPF_LINE_BUFFERING 0x01
#define QP_SPF_WRITE_THROUGH  0x02
#define QP_SPF_CLOSED         0x04
#define QP_SPF_SURROGATES     0x08   /* output buffer may hold lone surrogates */
#define QP_SPF_PENDING_CR     0x10   /* input: last byte was '\r' translated to '\n' */
#define QP_SPF_READABLE       0x20
#define QP_SP_CHUNK           8192

static int qp_sp_flags(JSValue *sl)
{
    return JS_VALUE_GET_INT(sl[QP_SP_FLAGS]);
}

static void qp_sp_set_flags(JSValue *sl, int flags)
{
    sl[QP_SP_FLAGS] = JS_NewInt32(NULL, flags);
}

static JSValue *qp_sp_slots(JSContext *ctx, JSValueConst self)
{
    return qp_instance_slots(self);
}

/* Encoding kind: 0 utf-8, 1 single byte ASCII-compatible (latin-1,
   ascii), 2 other (utf-16/32). */
static int qp_sp_enc_kind(JSContext *ctx, JSValueConst enc)
{
    char buf[32];
    const char *s = qp_str_utf8(ctx, enc, NULL);
    size_t i, j = 0;
    int kind = 2;

    if (!s) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return 2;
    }
    for (i = 0; s[i] && j < sizeof(buf) - 1; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c == '_' || c == ' ')
            c = '-';
        buf[j++] = c;
    }
    buf[j] = '\0';
    if (!strcmp(buf, "utf-8") || !strcmp(buf, "utf8") || !strcmp(buf, "u8") || !strcmp(buf, "utf"))
        kind = 0;
    else if (!strcmp(buf, "latin-1") || !strcmp(buf, "latin1") || !strcmp(buf, "iso-8859-1") ||
             !strcmp(buf, "iso8859-1") || !strcmp(buf, "8859") || !strcmp(buf, "l1") ||
             !strcmp(buf, "ascii") || !strcmp(buf, "us-ascii") || !strcmp(buf, "646"))
        kind = 1;
    JS_FreeCString(ctx, s);
    return kind;
}

static int qp_sp_check_open(JSContext *ctx, JSValue *sl)
{
    if (qp_sp_flags(sl) & QP_SPF_CLOSED) {
        qp_raise_value_error(ctx, "I/O operation on closed file.");
        return -1;
    }
    return 0;
}

/* write(2) the whole buffer (EINTR retried). */
static int qp_sp_write_fd(JSContext *ctx, int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            qp_raise_os_error(ctx, errno, QP_NULL);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Encode and write the pending output; the buffer is dropped even when the
   write fails (no retry loop on a broken stream). */
static int qp_sp_flush_buffer(JSContext *ctx, JSValueConst self)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    JSValue buf = sl[QP_SP_BUF];
    int flags = qp_sp_flags(sl), fd = JS_VALUE_GET_INT(sl[QP_SP_FD]), ret;

    if ((flags & QP_SPF_READABLE) || QP_IS_NULL(buf))
        return 0;
    sl[QP_SP_BUF] = QP_NULL;
    sl[QP_SP_POS] = JS_NewInt32(ctx, 0);
    qp_sp_set_flags(sl, flags & ~QP_SPF_SURROGATES);
    if (!(flags & QP_SPF_SURROGATES) && qp_sp_enc_kind(ctx, sl[QP_SP_ENCODING]) == 0) {
        size_t len;
        const char *s = qp_str_utf8(ctx, buf, &len);
        JS_FreeValue(ctx, buf);
        if (!s)
            return -1;
        ret = qp_sp_write_fd(ctx, fd, (const uint8_t *)s, len);
        JS_FreeCString(ctx, s);
        return ret;
    } else {
        const char *enc = qp_str_utf8(ctx, sl[QP_SP_ENCODING], NULL);
        const char *err;
        JSValue bytes;
        const uint8_t *data;
        Py_ssize_t len;

        if (!enc) {
            JS_FreeValue(ctx, buf);
            return -1;
        }
        err = qp_str_utf8(ctx, sl[QP_SP_ERRORS], NULL);
        if (!err) {
            JS_FreeCString(ctx, enc);
            JS_FreeValue(ctx, buf);
            return -1;
        }
        bytes = qp_str_encode(ctx, buf, enc, err);
        JS_FreeCString(ctx, enc);
        JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, buf);
        if (JS_IsException(bytes))
            return -1;
        data = qp_bytes_data(bytes, &len);
        ret = qp_sp_write_fd(ctx, fd, data, (size_t)len);
        JS_FreeValue(ctx, bytes);
        return ret;
    }
}

/* Scan an exact str for lone surrogates and newlines.  *has_nl: '\n' or
   '\r' present.  Returns 1 if a lone surrogate may be present. */
static int qp_sp_scan(JSContext *ctx, JSValueConst s, int *has_nl)
{
    qp_strview v;
    Py_ssize_t i;
    int surr = 0;

    *has_nl = 0;
    if (JS_VALUE_GET_TAG(s) == JS_TAG_STRING) {
        JSString *p = JS_VALUE_GET_STRING(s);
        uint32_t n = p->len, k;
        if (!p->is_wide_char) {
            const uint8_t *d = p->u.str8;
            *has_nl = memchr(d, '\n', n) != NULL || memchr(d, '\r', n) != NULL;
            return 0;
        }
        for (k = 0; k < n; k++) {
            uint16_t c = p->u.str16[k];
            if (c == '\n' || c == '\r') {
                *has_nl = 1;
            } else if (c >= 0xD800 && c <= 0xDBFF && k + 1 < n &&
                       p->u.str16[k + 1] >= 0xDC00 && p->u.str16[k + 1] <= 0xDFFF) {
                k++;
            } else if (c >= 0xD800 && c <= 0xDFFF) {
                surr = 1;
            }
        }
        return surr;
    }
    /* rope or other: code point view */
    if (qp_str_view_acquire(ctx, s, &v) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        *has_nl = 1;
        return 1;
    }
    for (i = 0; i < v.len; i++) {
        Py_UCS4 c = QP_SL_READ(v.kind, v.data, i);
        if (c == '\n' || c == '\r')
            *has_nl = 1;
        else if (c >= 0xD800 && c <= 0xDFFF)
            surr = 1;
    }
    qp_str_view_release(ctx, &v);
    return surr;
}

/* write(s) */
static JSValue qp_sp_write(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    JSValue text, nb;
    Py_ssize_t len, pending;
    int flags, has_nl, surr, kind, need_flush;

    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    flags = qp_sp_flags(sl);
    if (flags & QP_SPF_READABLE)
        return qp_raise(ctx, QP_EXC(ctx, OSError), "not writable");
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "write() argument must be str, not %T", args[0]);
    text = qp_str_value(ctx, args[0]);
    if (JS_IsException(text))
        return text;
    len = qp_str_len(ctx, text);
    if (len < 0) {
        JS_FreeValue(ctx, text);
        return JS_EXCEPTION;
    }
    /* output newline translation (newline='\r' or '\r\n') */
    if (qp_str_check_exact(sl[QP_SP_NEWLINE])) {
        JSValue nl = qp_str_from_cstr(ctx, "\n");
        JSValueConst rargs[2];
        JSValue r;
        JSAtom replace_atom;
        if (JS_IsException(nl)) {
            JS_FreeValue(ctx, text);
            return nl;
        }
        replace_atom = JS_NewAtom(ctx, "replace");
        if (replace_atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, nl);
            JS_FreeValue(ctx, text);
            return JS_EXCEPTION;
        }
        rargs[0] = nl;
        rargs[1] = sl[QP_SP_NEWLINE];
        r = qp_call_method(ctx, text, replace_atom, rargs, 2, QP_NULL);
        JS_FreeAtom(ctx, replace_atom);
        JS_FreeValue(ctx, nl);
        JS_FreeValue(ctx, text);
        if (JS_IsException(r))
            return r;
        text = r;
        sl = qp_sp_slots(ctx, self);
    }
    surr = qp_sp_scan(ctx, text, &has_nl);
    kind = qp_sp_enc_kind(ctx, sl[QP_SP_ENCODING]);
    if (surr || kind != 0) {
        /* encode now so that errors are raised by write() like TextIOWrapper */
        const char *enc = qp_str_utf8(ctx, sl[QP_SP_ENCODING], NULL);
        const char *err = enc ? qp_str_utf8(ctx, sl[QP_SP_ERRORS], NULL) : NULL;
        JSValue b = JS_EXCEPTION;
        if (enc && err)
            b = qp_str_encode(ctx, text, enc, err);
        if (enc)
            JS_FreeCString(ctx, enc);
        if (err)
            JS_FreeCString(ctx, err);
        if (JS_IsException(b)) {
            JS_FreeValue(ctx, text);
            return b;
        }
        JS_FreeValue(ctx, b);
        sl = qp_sp_slots(ctx, self);
    }
    flags = qp_sp_flags(sl);
    if (QP_IS_NULL(sl[QP_SP_BUF])) {
        nb = text;
    } else {
        nb = qp_str_concat(ctx, sl[QP_SP_BUF], text);
        JS_FreeValue(ctx, text);
        if (JS_IsException(nb))
            return nb;
        JS_FreeValue(ctx, sl[QP_SP_BUF]);
    }
    sl[QP_SP_BUF] = nb;
    pending = JS_VALUE_GET_INT(sl[QP_SP_POS]) + len;
    sl[QP_SP_POS] = JS_NewInt32(ctx, pending > INT32_MAX ? INT32_MAX : (int32_t)pending);
    if (surr)
        flags |= QP_SPF_SURROGATES;
    qp_sp_set_flags(sl, flags);
    need_flush = (flags & QP_SPF_WRITE_THROUGH) || pending > QP_SP_CHUNK ||
                 ((flags & QP_SPF_LINE_BUFFERING) && has_nl);
    if (need_flush && qp_sp_flush_buffer(ctx, self) < 0)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, len);
}

static JSValue qp_sp_flush(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    if (qp_sp_flush_buffer(ctx, self) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_sp_fileno(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, sl[QP_SP_FD]);
}

static JSValue qp_sp_isatty(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    return qp_bool_new(isatty(JS_VALUE_GET_INT(sl[QP_SP_FD])) != 0);
}

/* readable() / writable() / seekable() (magic 0/1/2) */
static JSValue qp_sp_able(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    int readable;
    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    readable = (qp_sp_flags(sl) & QP_SPF_READABLE) != 0;
    switch (magic) {
    case 0: return qp_bool_new(readable);
    case 1: return qp_bool_new(!readable);
    default: {
        off_t r = lseek(JS_VALUE_GET_INT(sl[QP_SP_FD]), 0, SEEK_CUR);
        return qp_bool_new(r >= 0);
    }
    }
}

/* close(): flush, then mark closed (the descriptor is not closed:
   closefd=False like CPython's standard streams). */
static JSValue qp_sp_close(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    int ret;
    if (qp_sp_flags(sl) & QP_SPF_CLOSED)
        return JS_UNDEFINED;
    ret = qp_sp_flush_buffer(ctx, self);
    sl = qp_sp_slots(ctx, self);
    qp_sp_set_flags(sl, qp_sp_flags(sl) | QP_SPF_CLOSED);
    if (ret < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_sp_enter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    if (qp_sp_check_open(ctx, sl) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

static JSValue qp_sp_exit(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    return qp_sp_close(ctx, self, args, 0, QP_NULL, 0);
}

/* ---- input side ---------------------------------------------------------- */

/* Read one chunk from the descriptor and append it (newline-translated for
   ASCII-compatible encodings) to the read-ahead buffer.  Returns the number
   of bytes read (0 = EOF) or -1. */
static int qp_sp_fill(JSContext *ctx, JSValueConst self)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    uint8_t chunk[QP_SP_CHUNK];
    const uint8_t *old = NULL;
    Py_ssize_t oldlen = 0, pos = JS_VALUE_GET_INT(sl[QP_SP_POS]);
    ssize_t n;
    uint8_t *nbuf;
    size_t j = 0, k;
    int flags = qp_sp_flags(sl), translate;
    JSValue b;

    for (;;) {
        n = read(JS_VALUE_GET_INT(sl[QP_SP_FD]), chunk, sizeof(chunk));
        if (n >= 0 || errno != EINTR)
            break;
    }
    if (n < 0) {
        qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    if (n == 0)
        return 0;
    if (!QP_IS_NULL(sl[QP_SP_BUF]))
        old = qp_bytes_data(sl[QP_SP_BUF], &oldlen);
    if (pos > oldlen)
        pos = oldlen;
    nbuf = js_malloc(ctx, (size_t)(oldlen - pos) + (size_t)n + 1);
    if (!nbuf)
        return -1;
    if (oldlen > pos)
        memcpy(nbuf, old + pos, (size_t)(oldlen - pos));
    j = (size_t)(oldlen - pos);
    translate = qp_sp_enc_kind(ctx, sl[QP_SP_ENCODING]) != 2;
    for (k = 0; k < (size_t)n; k++) {
        uint8_t c = chunk[k];
        if (!translate) {
            nbuf[j++] = c;
            continue;
        }
        if (flags & QP_SPF_PENDING_CR) {
            flags &= ~QP_SPF_PENDING_CR;
            if (c == '\n')
                continue;
        }
        if (c == '\r') {
            nbuf[j++] = '\n';
            flags |= QP_SPF_PENDING_CR;
        } else {
            nbuf[j++] = c;
        }
    }
    b = qp_bytes_new(ctx, nbuf, (Py_ssize_t)j);
    js_free(ctx, nbuf);
    if (JS_IsException(b))
        return -1;
    sl = qp_sp_slots(ctx, self);
    JS_FreeValue(ctx, sl[QP_SP_BUF]);
    sl[QP_SP_BUF] = b;
    sl[QP_SP_POS] = JS_NewInt32(ctx, 0);
    qp_sp_set_flags(sl, flags);
    return (int)n;
}

/* Take nbytes of the read-ahead buffer and decode them. */
static JSValue qp_sp_take(JSContext *ctx, JSValueConst self, Py_ssize_t nbytes)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    const uint8_t *data;
    Py_ssize_t len = 0, pos = JS_VALUE_GET_INT(sl[QP_SP_POS]);
    const char *enc, *err;
    JSValue res;

    data = QP_IS_NULL(sl[QP_SP_BUF]) ? NULL : qp_bytes_data(sl[QP_SP_BUF], &len);
    if (nbytes > len - pos)
        nbytes = len - pos;
    enc = qp_str_utf8(ctx, sl[QP_SP_ENCODING], NULL);
    if (!enc)
        return JS_EXCEPTION;
    err = qp_str_utf8(ctx, sl[QP_SP_ERRORS], NULL);
    if (!err) {
        JS_FreeCString(ctx, enc);
        return JS_EXCEPTION;
    }
    res = qp_str_decode(ctx, data ? (const char *)data + pos : "", nbytes, enc, err);
    JS_FreeCString(ctx, enc);
    JS_FreeCString(ctx, err);
    if (JS_IsException(res))
        return res;
    sl = qp_sp_slots(ctx, self);
    sl[QP_SP_POS] = JS_NewInt32(ctx, (int32_t)(pos + nbytes));
    return res;
}

/* Byte offset after `nchars` characters of buf[pos..len), or -1 when the
   buffer holds fewer complete characters. */
static Py_ssize_t qp_sp_char_bytes(JSContext *ctx, JSValue *sl, const uint8_t *d, Py_ssize_t pos,
                                   Py_ssize_t len, Py_ssize_t nchars)
{
    int kind = qp_sp_enc_kind(ctx, sl[QP_SP_ENCODING]);
    Py_ssize_t i, count = 0;

    if (kind == 1)
        return len - pos >= nchars ? nchars : -1;
    if (kind == 2) {
        const char *enc = qp_str_utf8(ctx, sl[QP_SP_ENCODING], NULL);
        int unit = (enc && strstr(enc, "32")) ? 4 : 2;
        if (enc)
            JS_FreeCString(ctx, enc);
        return len - pos >= nchars * unit ? nchars * unit : -1;
    }
    for (i = pos; i < len; i++) {
        if ((d[i] & 0xC0) != 0x80) {
            if (count == nchars)
                return i - pos;
            count++;
        }
    }
    return -1;
}

/* readline(size=-1) core: QP_NULL never; "" at EOF. */
static JSValue qp_sp_readline_impl(JSContext *ctx, JSValueConst self, Py_ssize_t limit)
{
    Py_ssize_t scanned = 0;

    for (;;) {
        JSValue *sl = qp_sp_slots(ctx, self);
        const uint8_t *d = NULL;
        Py_ssize_t len = 0, pos = JS_VALUE_GET_INT(sl[QP_SP_POS]), i, cut;
        int r;

        if (!QP_IS_NULL(sl[QP_SP_BUF]))
            d = qp_bytes_data(sl[QP_SP_BUF], &len);
        for (i = pos + scanned; i < len; i++) {
            if (d[i] == '\n')
                break;
        }
        if (i < len) {
            Py_ssize_t nbytes = i + 1 - pos;
            if (limit >= 0) {
                cut = qp_sp_char_bytes(ctx, sl, d, pos, len, limit);
                if (cut >= 0 && cut < nbytes)
                    nbytes = cut;
            }
            return qp_sp_take(ctx, self, nbytes);
        }
        scanned = len - pos;
        if (limit >= 0) {
            cut = qp_sp_char_bytes(ctx, sl, d, pos, len, limit);
            if (cut >= 0)
                return qp_sp_take(ctx, self, cut);
        }
        r = qp_sp_fill(ctx, self);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0)
            return qp_sp_take(ctx, self, scanned);
    }
}

static int qp_sp_check_readable(JSContext *ctx, JSValue *sl)
{
    if (qp_sp_check_open(ctx, sl) < 0)
        return -1;
    if (!(qp_sp_flags(sl) & QP_SPF_READABLE)) {
        qp_raise(ctx, QP_EXC(ctx, OSError), "not readable");
        return -1;
    }
    return 0;
}

/* Optional size argument: None or int (__index__). */
static int qp_sp_size_arg(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                          Py_ssize_t *out)
{
    *out = -1;
    if (qp_arg_check_positional(ctx, fname, nargs, 0, 1) < 0)
        return -1;
    if (nargs == 1 && !JS_IsUndefined(args[0]) && !JS_IsNull(args[0])) {
        if (!qp_long_check(ctx, args[0]) && QP_IS_NULL(qp_lookup_special(ctx, args[0], QP_ATOM(ctx, __index__)))) {
            qp_raise_type_error(ctx, "argument should be integer or None, not '%s'",
                                qp_type_of(ctx, args[0])->tp_name ? qp_type_of(ctx, args[0])->tp_name : "object");
            return -1;
        }
        if (qp_arg_ssize(ctx, args[0], out) < 0)
            return -1;
    }
    return 0;
}

static JSValue qp_sp_readline(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t size;
    if (qp_sp_size_arg(ctx, "readline", args, nargs, &size) < 0)
        return JS_EXCEPTION;
    if (qp_sp_check_readable(ctx, qp_sp_slots(ctx, self)) < 0)
        return JS_EXCEPTION;
    return qp_sp_readline_impl(ctx, self, size);
}

static JSValue qp_sp_read(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t size;

    if (qp_sp_size_arg(ctx, "read", args, nargs, &size) < 0)
        return JS_EXCEPTION;
    if (qp_sp_check_readable(ctx, qp_sp_slots(ctx, self)) < 0)
        return JS_EXCEPTION;
    for (;;) {
        JSValue *sl = qp_sp_slots(ctx, self);
        const uint8_t *d = NULL;
        Py_ssize_t len = 0, pos = JS_VALUE_GET_INT(sl[QP_SP_POS]);
        int r;

        if (!QP_IS_NULL(sl[QP_SP_BUF]))
            d = qp_bytes_data(sl[QP_SP_BUF], &len);
        if (size >= 0) {
            Py_ssize_t cut = qp_sp_char_bytes(ctx, sl, d, pos, len, size);
            if (cut >= 0)
                return qp_sp_take(ctx, self, cut);
        }
        r = qp_sp_fill(ctx, self);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0) {
            sl = qp_sp_slots(ctx, self);
            len = 0;
            if (!QP_IS_NULL(sl[QP_SP_BUF]))
                qp_bytes_data(sl[QP_SP_BUF], &len);
            return qp_sp_take(ctx, self, len);
        }
    }
}

static JSValue qp_sp_readlines(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t hint, total = 0;
    JSValue list;

    if (qp_sp_size_arg(ctx, "readlines", args, nargs, &hint) < 0)
        return JS_EXCEPTION;
    if (qp_sp_check_readable(ctx, qp_sp_slots(ctx, self)) < 0)
        return JS_EXCEPTION;
    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    for (;;) {
        JSValue line = qp_sp_readline_impl(ctx, self, -1);
        Py_ssize_t n;
        if (JS_IsException(line))
            goto fail;
        n = qp_str_len(ctx, line);
        if (n == 0) {
            JS_FreeValue(ctx, line);
            break;
        }
        if (qp_list_append(ctx, list, line) < 0) {
            JS_FreeValue(ctx, line);
            goto fail;
        }
        JS_FreeValue(ctx, line);
        total += n;
        if (hint > 0 && total >= hint)
            break;
    }
    return list;
fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

static JSValue qp_sp_iter(JSContext *ctx, JSValueConst self)
{
    if (qp_sp_check_open(ctx, qp_sp_slots(ctx, self)) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, self);
}

static JSValue qp_sp_iternext(JSContext *ctx, JSValueConst self)
{
    JSValue line;
    if (qp_sp_check_readable(ctx, qp_sp_slots(ctx, self)) < 0)
        return JS_EXCEPTION;
    line = qp_sp_readline_impl(ctx, self, -1);
    if (JS_IsException(line))
        return line;
    if (qp_str_len(ctx, line) == 0) {
        JS_FreeValue(ctx, line);
        return QP_NULL;
    }
    return line;
}

/* reconfigure(*, encoding=None, errors=None, newline=None,
               line_buffering=None, write_through=None) */
static JSValue qp_sp_reconfigure(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "encoding", "errors", "newline", "line_buffering",
                                          "write_through", NULL };
    static const QPArgSpec spec = { "reconfigure", kwlist, 0, 0, 0 };
    JSValueConst out[5];
    JSValue *sl;
    int flags, b;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    for (b = 0; b < 3; b++) {
        if (!QP_IS_NULL(out[b]) && !JS_IsUndefined(out[b]) && !qp_str_check(ctx, out[b]))
            return qp_raise_type_error(ctx, "reconfigure() argument '%s' must be str or None, not %T",
                                       kwlist[b], out[b]);
    }
    if (!QP_IS_NULL(out[2]) && !JS_IsUndefined(out[2])) {
        const char *nl = qp_str_utf8(ctx, out[2], NULL);
        int ok;
        if (!nl)
            return JS_EXCEPTION;
        ok = !strcmp(nl, "") || !strcmp(nl, "\n") || !strcmp(nl, "\r") || !strcmp(nl, "\r\n");
        JS_FreeCString(ctx, nl);
        if (!ok)
            return qp_raise_value_error(ctx, "illegal newline value: %R", out[2]);
    }
    if (qp_sp_flush_buffer(ctx, self) < 0)
        return JS_EXCEPTION;
    sl = qp_sp_slots(ctx, self);
    if (!QP_IS_NULL(out[0]) && !JS_IsUndefined(out[0])) {
        JSValue e = qp_str_value(ctx, out[0]);
        JSValue err;
        if (JS_IsException(e))
            return e;
        if (!QP_IS_NULL(out[1]) && !JS_IsUndefined(out[1]))
            err = qp_str_value(ctx, out[1]);
        else
            err = qp_str_from_cstr(ctx, "strict");
        if (JS_IsException(err)) {
            JS_FreeValue(ctx, e);
            return err;
        }
        sl = qp_sp_slots(ctx, self);
        JS_FreeValue(ctx, sl[QP_SP_ENCODING]);
        sl[QP_SP_ENCODING] = e;
        JS_FreeValue(ctx, sl[QP_SP_ERRORS]);
        sl[QP_SP_ERRORS] = err;
    } else if (!QP_IS_NULL(out[1]) && !JS_IsUndefined(out[1])) {
        JSValue err = qp_str_value(ctx, out[1]);
        if (JS_IsException(err))
            return err;
        sl = qp_sp_slots(ctx, self);
        JS_FreeValue(ctx, sl[QP_SP_ERRORS]);
        sl[QP_SP_ERRORS] = err;
    }
    if (!QP_IS_NULL(out[2]) && !JS_IsUndefined(out[2])) {
        const char *nl = qp_str_utf8(ctx, out[2], NULL);
        JSValue v;
        if (!nl)
            return JS_EXCEPTION;
        /* output translation only matters for '\r' and '\r\n' */
        v = (!strcmp(nl, "\r") || !strcmp(nl, "\r\n")) ? qp_str_value(ctx, out[2]) : JS_UNDEFINED;
        JS_FreeCString(ctx, nl);
        if (JS_IsException(v))
            return v;
        sl = qp_sp_slots(ctx, self);
        JS_FreeValue(ctx, sl[QP_SP_NEWLINE]);
        sl[QP_SP_NEWLINE] = v;
    }
    flags = qp_sp_flags(sl);
    if (!QP_IS_NULL(out[3]) && !JS_IsUndefined(out[3])) {
        if (qp_arg_bool(ctx, out[3], &b) < 0)
            return JS_EXCEPTION;
        flags = b ? (flags | QP_SPF_LINE_BUFFERING) : (flags & ~QP_SPF_LINE_BUFFERING);
    }
    if (!QP_IS_NULL(out[4]) && !JS_IsUndefined(out[4])) {
        if (qp_arg_bool(ctx, out[4], &b) < 0)
            return JS_EXCEPTION;
        flags = b ? (flags | QP_SPF_WRITE_THROUGH) : (flags & ~QP_SPF_WRITE_THROUGH);
    }
    sl = qp_sp_slots(ctx, self);
    qp_sp_set_flags(sl, flags);
    return JS_UNDEFINED;
}

static JSValue qp_sp_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *sl = qp_sp_slots(ctx, self);
    return qp_str_from_format(ctx, "<_io.TextIOWrapper name=%R mode=%R encoding=%R>",
                              sl[QP_SP_NAME], sl[QP_SP_MODE], sl[QP_SP_ENCODING]);
}

/* name / mode / encoding / errors (magic = slot) */
static JSValue qp_sp_get_slot(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_DupValue(ctx, qp_sp_slots(ctx, self)[magic]);
}

/* closed / line_buffering / write_through (magic = flag bit) */
static JSValue qp_sp_get_flag(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_bool_new((qp_sp_flags(qp_sp_slots(ctx, self)) & magic) != 0);
}

static JSValue qp_sp_get_none(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_UNDEFINED;
}

static const QPMethodDef qp_sp_methods[] = {
    { "write", qp_sp_write, QP_METH_O, 0, NULL },
    { "flush", qp_sp_flush, QP_METH_NOARGS, 0, NULL },
    { "fileno", qp_sp_fileno, QP_METH_NOARGS, 0, NULL },
    { "isatty", qp_sp_isatty, QP_METH_NOARGS, 0, NULL },
    { "readable", qp_sp_able, QP_METH_NOARGS, 0, NULL },
    { "writable", qp_sp_able, QP_METH_NOARGS, 1, NULL },
    { "seekable", qp_sp_able, QP_METH_NOARGS, 2, NULL },
    { "close", qp_sp_close, QP_METH_NOARGS, 0, NULL },
    { "read", qp_sp_read, QP_METH_FASTCALL, 0, NULL },
    { "readline", qp_sp_readline, QP_METH_FASTCALL, 0, NULL },
    { "readlines", qp_sp_readlines, QP_METH_FASTCALL, 0, NULL },
    { "reconfigure", qp_sp_reconfigure, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0, NULL },
    { "__enter__", qp_sp_enter, QP_METH_NOARGS, 0, NULL },
    { "__exit__", qp_sp_exit, QP_METH_FASTCALL, 0, NULL },
    { NULL }
};

static const QPGetSetDef qp_sp_getsets[] = {
    { "name", qp_sp_get_slot, NULL, QP_SP_NAME, NULL },
    { "mode", qp_sp_get_slot, NULL, QP_SP_MODE, NULL },
    { "encoding", qp_sp_get_slot, NULL, QP_SP_ENCODING, "Encoding of the text stream." },
    { "errors", qp_sp_get_slot, NULL, QP_SP_ERRORS, "The error setting of the decoder or encoder." },
    { "closed", qp_sp_get_flag, NULL, QP_SPF_CLOSED, NULL },
    { "line_buffering", qp_sp_get_flag, NULL, QP_SPF_LINE_BUFFERING, NULL },
    { "write_through", qp_sp_get_flag, NULL, QP_SPF_WRITE_THROUGH, NULL },
    { "newlines", qp_sp_get_none, NULL, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_stdprinter_spec = {
    .name = "_io.TextIOWrapper",
    .doc = "Character and line based layer over a file descriptor (sys.std* before io).",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_SP_NSLOTS,
    .slots = {
        .tp_repr = qp_sp_repr,
        .tp_iter = qp_sp_iter,
        .tp_iternext = qp_sp_iternext,
    },
    .methods = qp_sp_methods,
    .getsets = qp_sp_getsets,
};

/* New standard stream over fd. */
static JSValue qp_stdprinter_new(JSContext *ctx, int fd, const char *name, int readable,
                                 const char *encoding, const char *errors, int flags)
{
    JSValue obj, *sl;

    obj = qp_object_new_instance(ctx, QP_TYPE(ctx, StdPrinter), JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return obj;
    sl = qp_instance_slots(obj);
    sl[QP_SP_FD] = JS_NewInt32(ctx, fd);
    sl[QP_SP_NAME] = qp_str_from_cstr(ctx, name);
    sl[QP_SP_MODE] = qp_str_from_cstr(ctx, readable ? "r" : "w");
    sl[QP_SP_ENCODING] = qp_str_from_cstr(ctx, encoding);
    sl[QP_SP_ERRORS] = qp_str_from_cstr(ctx, errors);
    sl[QP_SP_NEWLINE] = JS_UNDEFINED;
    sl[QP_SP_BUF] = QP_NULL;
    sl[QP_SP_POS] = JS_NewInt32(ctx, 0);
    qp_sp_set_flags(sl, flags | (readable ? QP_SPF_READABLE : 0));
    if (JS_IsException(sl[QP_SP_NAME]) || JS_IsException(sl[QP_SP_MODE]) ||
        JS_IsException(sl[QP_SP_ENCODING]) || JS_IsException(sl[QP_SP_ERRORS])) {
        int i;
        for (i = QP_SP_NAME; i <= QP_SP_ERRORS; i++) {
            if (JS_IsException(sl[i]))
                sl[i] = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static BOOL qp_is_stdprinter(JSContext *ctx, JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && qp_type_of(ctx, v) == QP_TYPE(ctx, StdPrinter);
}

/* ========================================================================= */
/* C-level access to sys                                                      */
/* ========================================================================= */

static JSValue qp_sys_get(JSContext *ctx, JSAtom name)
{
    QPState *st = QP_STATE(ctx);
    JSValue v;
    int r;

    if (!st || JS_VALUE_GET_TAG(st->sys_dict) != JS_TAG_OBJECT)
        return QP_NULL;
    r = qp_dict_get_item_atom(ctx, st->sys_dict, name, &v);
    if (r < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return QP_NULL;
    }
    return r ? v : QP_NULL;
}

/* PySys_WriteStdout / PySys_WriteStderr: at most 1000 formatted bytes
   written through sys.std*.write(); C stdio fallback; the pending
   exception (if any) is preserved. */
static void qp_sys_write_v(JSContext *ctx, JSAtom name, FILE *fp, const char *fmt, va_list ap)
{
    char buffer[1001];
    int written;
    JSValue saved, file;
    int ok = 0;

    written = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    saved = JS_GetException(ctx);
    file = qp_sys_get(ctx, name);
    if (!QP_IS_NULL(file)) {
        JSValue s = qp_str_decode(ctx, buffer, (Py_ssize_t)strlen(buffer), "utf-8", "surrogateescape");
        if (!JS_IsException(s)) {
            JSValueConst a[1] = { s };
            JSValue r = qp_call_method(ctx, file, QP_ATOM(ctx, write), a, 1, QP_NULL);
            JS_FreeValue(ctx, s);
            if (!JS_IsException(r)) {
                ok = 1;
                JS_FreeValue(ctx, r);
            }
        }
        if (ok && (written < 0 || (size_t)written >= sizeof(buffer))) {
            const char *truncated = "... truncated";
            JSValue t = qp_str_from_cstr(ctx, truncated);
            if (!JS_IsException(t)) {
                JSValueConst a[1] = { t };
                JSValue r = qp_call_method(ctx, file, QP_ATOM(ctx, write), a, 1, QP_NULL);
                JS_FreeValue(ctx, t);
                if (JS_IsException(r))
                    ok = 0;
                else
                    JS_FreeValue(ctx, r);
            }
        }
        JS_FreeValue(ctx, file);
    }
    if (!ok) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        fputs(buffer, fp);
        if (written < 0 || (size_t)written >= sizeof(buffer))
            fputs("... truncated", fp);
        fflush(fp);
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    if (!JS_IsUninitialized(saved))
        JS_Throw(ctx, saved);
}

static void qp_sys_write_stdout(JSContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qp_sys_write_v(ctx, QP_ATOM(ctx, stdout), stdout, fmt, ap);
    va_end(ap);
}

static void qp_sys_write_stderr(JSContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qp_sys_write_v(ctx, QP_ATOM(ctx, stderr), stderr, fmt, ap);
    va_end(ap);
}

/* PyErr_FormatUnraisable(msg) on the pending exception: through
   sys.unraisablehook, falling back to the default hook. */
static void qp_sys_format_unraisable(JSContext *ctx, const char *msg)
{
    JSValue exc = qp_exc_fetch(ctx), tb, m, args, hook;
    QPType *t = qp_type_of(ctx, exc);
    int done = 0;

    tb = qp_exc_get_traceback(ctx, exc);
    if (JS_IsException(tb)) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        tb = JS_UNDEFINED;
    }
    m = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(m)) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        m = JS_UNDEFINED;
    }
    hook = qp_sys_get(ctx, QP_ATOM(ctx, unraisablehook));
    if (!QP_IS_NULL(hook) && !JS_IsUndefined(hook)) {
        args = qp_unraisable_hook_args_new(ctx, QP_TYPE_VALUE(t), exc, tb, m, JS_UNDEFINED);
        if (!JS_IsException(args)) {
            JSValue r = qp_call1(ctx, hook, args);
            JS_FreeValue(ctx, args);
            if (!JS_IsException(r)) {
                JS_FreeValue(ctx, r);
                done = 1;
            }
        }
        if (!done)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
    }
    JS_FreeValue(ctx, hook);
    if (!done) {
        if (qp_unraisable_default_hook(ctx, QP_TYPE_VALUE(t), exc, tb, m, JS_UNDEFINED) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
    }
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, tb);
    JS_FreeValue(ctx, exc);
}

/* CPython file_is_closed (pylifecycle.c): file.closed is true; errors are
   cleared and mean "not closed" */
static BOOL qp_sys_file_is_closed(JSContext *ctx, JSValueConst file)
{
    JSAtom name = JS_NewAtom(ctx, "closed");
    JSValue v;
    int r;

    if (name == JS_ATOM_NULL) {
        qp_err_clear(ctx);
        return FALSE;
    }
    v = qp_getattr(ctx, file, name);
    JS_FreeAtom(ctx, name);
    if (JS_IsException(v)) {
        qp_err_clear(ctx);
        return FALSE;
    }
    r = qp_bool(ctx, v);
    JS_FreeValue(ctx, v);
    if (r < 0)
        qp_err_clear(ctx);
    return r > 0;
}

/* Call file.flush() if file is usable (not None, not closed); -1 (exception
   pending) on failure. */
static int qp_sys_flush_file(JSContext *ctx, JSValueConst file)
{
    JSValue r;
    if (QP_IS_NULL(file) || JS_IsUndefined(file) || JS_IsNull(file))
        return 0;
    if (qp_is_stdprinter(ctx, file) && (qp_sp_flags(qp_sp_slots(ctx, file)) & QP_SPF_CLOSED))
        return 0;
    if (qp_sys_file_is_closed(ctx, file))
        return 0;
    r = qp_call_method(ctx, file, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* CPython flush_std_files (+ the original streams, which CPython's io
   flushes when they are deallocated). */
static int qp_sys_flush_std_files(JSContext *ctx)
{
    JSValue out = qp_sys_get(ctx, QP_ATOM(ctx, stdout));
    JSValue err = qp_sys_get(ctx, QP_ATOM(ctx, stderr));
    JSValue orig_out = qp_sys_get(ctx, QP_ATOM(ctx, __stdout__));
    JSValue orig_err = qp_sys_get(ctx, QP_ATOM(ctx, __stderr__));
    int status = 0;

    if (qp_sys_flush_file(ctx, out) < 0) {
        qp_sys_format_unraisable(ctx, "Exception ignored while flushing sys.stdout");
        status = -1;
    }
    if (!qp_is(orig_out, out) && qp_is_stdprinter(ctx, orig_out) &&
        qp_sys_flush_file(ctx, orig_out) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        status = -1;
    }
    if (qp_sys_flush_file(ctx, err) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        status = -1;
    }
    if (!qp_is(orig_err, err) && qp_is_stdprinter(ctx, orig_err) &&
        qp_sys_flush_file(ctx, orig_err) < 0) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        status = -1;
    }
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, orig_out);
    JS_FreeValue(ctx, orig_err);
    return status;
}

/* INTRINSIC_PRINT (CPython print_expr): sys.displayhook(v). */
static int qp_displayhook(JSContext *ctx, JSValueConst v)
{
    JSValue hook = qp_sys_get(ctx, QP_ATOM(ctx, displayhook)), r;

    if (QP_IS_NULL(hook)) {
        qp_raise_runtime_error(ctx, "lost sys.displayhook");
        return -1;
    }
    r = qp_call1(ctx, hook, v);
    JS_FreeValue(ctx, hook);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* ========================================================================= */
/* sys functions                                                              */
/* ========================================================================= */

#define QP_SYS_FN(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                        int nargs, JSValueConst kwnames, int magic)

/* sys.modules['builtins'] (borrowed lookups through a new ref). */
static JSValue qp_sys_builtins_module(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSAtom a = JS_NewAtom(ctx, "builtins");
    JSValue m;
    int r;

    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, st->modules, a, &m);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_runtime_error(ctx, "lost builtins module");
    return m;
}

/* displayhook(object, /): print repr(object) to sys.stdout, store in
   builtins._ */
QP_SYS_FN(qp_sys_displayhook)
{
    JSValueConst o = args[0];
    JSValue builtins, outf, r, nl;

    builtins = qp_sys_builtins_module(ctx);
    if (JS_IsException(builtins))
        return builtins;
    if (JS_IsUndefined(o) || JS_IsNull(o)) {
        JS_FreeValue(ctx, builtins);
        return JS_UNDEFINED;
    }
    if (qp_setattr(ctx, builtins, QP_ATOM(ctx, _), JS_UNDEFINED) < 0)
        goto fail;
    outf = qp_sys_get(ctx, QP_ATOM(ctx, stdout));
    if (QP_IS_NULL(outf) || JS_IsUndefined(outf)) {
        JS_FreeValue(ctx, outf);
        qp_raise_runtime_error(ctx, "lost sys.stdout");
        goto fail;
    }
    r = qp_repr(ctx, o);
    if (!JS_IsException(r)) {
        JSValueConst a[1] = { r };
        JSValue w = qp_call_method(ctx, outf, QP_ATOM(ctx, write), a, 1, QP_NULL);
        JS_FreeValue(ctx, r);
        r = w;
    }
    if (JS_IsException(r)) {
        /* repr(o) not encodable: sys_displayhook_unencodable */
        JSValue enc, encoded, text, w;
        const char *encs;
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, UnicodeEncodeError))) {
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        {
            JSAtom enc_atom = JS_NewAtom(ctx, "encoding");
            enc = enc_atom == JS_ATOM_NULL ? JS_EXCEPTION : qp_getattr(ctx, outf, enc_atom);
            JS_FreeAtom(ctx, enc_atom);
        }
        if (JS_IsException(enc)) {
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        encs = qp_str_utf8(ctx, enc, NULL);
        JS_FreeValue(ctx, enc);
        if (!encs) {
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        r = qp_repr(ctx, o);
        if (JS_IsException(r)) {
            JS_FreeCString(ctx, encs);
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        encoded = qp_str_encode(ctx, r, encs, "backslashreplace");
        JS_FreeValue(ctx, r);
        if (JS_IsException(encoded)) {
            JS_FreeCString(ctx, encs);
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        {
            Py_ssize_t blen;
            const uint8_t *bd = qp_bytes_data(encoded, &blen);
            text = qp_str_decode(ctx, (const char *)bd, blen, encs, "strict");
        }
        JS_FreeCString(ctx, encs);
        JS_FreeValue(ctx, encoded);
        if (JS_IsException(text)) {
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        {
            JSValueConst a[1] = { text };
            w = qp_call_method(ctx, outf, QP_ATOM(ctx, write), a, 1, QP_NULL);
        }
        JS_FreeValue(ctx, text);
        if (JS_IsException(w)) {
            JS_FreeValue(ctx, outf);
            goto fail;
        }
        r = w;
    }
    JS_FreeValue(ctx, r);
    nl = qp_str_from_cstr(ctx, "\n");
    if (JS_IsException(nl)) {
        JS_FreeValue(ctx, outf);
        goto fail;
    }
    {
        JSValueConst a[1] = { nl };
        r = qp_call_method(ctx, outf, QP_ATOM(ctx, write), a, 1, QP_NULL);
    }
    JS_FreeValue(ctx, nl);
    JS_FreeValue(ctx, outf);
    if (JS_IsException(r))
        goto fail;
    JS_FreeValue(ctx, r);
    if (qp_setattr(ctx, builtins, QP_ATOM(ctx, _), o) < 0)
        goto fail;
    JS_FreeValue(ctx, builtins);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, builtins);
    return JS_EXCEPTION;
}

/* excepthook(exctype, value, traceback, /) */
QP_SYS_FN(qp_sys_excepthook)
{
    JSValueConst value, tb;

    if (qp_arg_check_positional(ctx, "excepthook", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    value = args[1];
    tb = args[2];
    if (!qp_exc_check(ctx, value)) {
        JSValue tn = qp_sys_tp_name(ctx, qp_type_of(ctx, value));
        const char *s;
        if (JS_IsException(tn))
            return tn;
        s = qp_str_utf8(ctx, tn, NULL);
        JS_FreeValue(ctx, tn);
        if (!s)
            return JS_EXCEPTION;
        qp_sys_write_stderr(ctx, "TypeError: print_exception(): Exception expected for value, %s found\n", s);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    if (qp_typecheck(ctx, tb, QP_TYPE(ctx, traceback))) {
        JSValue cur = qp_exc_get_traceback(ctx, value);
        if (JS_IsException(cur))
            return cur;
        if (JS_IsUndefined(cur) && qp_exc_set_traceback(ctx, value, tb) < 0)
            return JS_EXCEPTION;
        JS_FreeValue(ctx, cur);
    }
    if (qp_exc_display(ctx, value, QP_NULL) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* unraisablehook(unraisable, /) */
QP_SYS_FN(qp_sys_unraisablehook)
{
    JSValueConst a = args[0];
    JSValueConst *f;

    if (qp_type_of(ctx, a) != qp_sys_ss_type(ctx, QP_SYS_SS_UNRAISABLE_ARGS))
        return qp_raise_type_error(ctx, "sys.unraisablehook argument type must be UnraisableHookArgs");
    if (QP_TUPLE_SIZE(a) < 5)
        return qp_raise_type_error(ctx, "sys.unraisablehook argument type must be UnraisableHookArgs");
    f = (JSValueConst *)QP_TUPLE_ITEMS(a);
    if (qp_unraisable_default_hook(ctx, f[0], f[1], f[2], f[3], f[4]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* exc_info() */
QP_SYS_FN(qp_sys_exc_info)
{
    JSValue exc = qp_exc_info_get(ctx), items[3];

    if (JS_IsException(exc))
        return exc;
    if (JS_IsUndefined(exc) || JS_IsNull(exc)) {
        items[0] = JS_UNDEFINED;
        items[1] = JS_UNDEFINED;
        items[2] = JS_UNDEFINED;
        return qp_tuple_new_steal(ctx, 3, items);
    }
    items[2] = qp_exc_get_traceback(ctx, exc);
    if (JS_IsException(items[2])) {
        JS_FreeValue(ctx, exc);
        return JS_EXCEPTION;
    }
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, exc)));
    items[1] = exc;
    return qp_tuple_new_steal(ctx, 3, items);
}

/* exception() */
QP_SYS_FN(qp_sys_exception)
{
    return qp_exc_info_get(ctx);
}

/* exit(status=None, /) */
QP_SYS_FN(qp_sys_exit)
{
    if (qp_arg_check_positional(ctx, "exit", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    qp_raise_value(ctx, QP_EXC(ctx, SystemExit), nargs ? args[0] : JS_UNDEFINED);
    return JS_EXCEPTION;
}

QP_SYS_FN(qp_sys_getrecursionlimit)
{
    return JS_NewInt32(ctx, QP_STATE(ctx)->recursion_limit);
}

/* setrecursionlimit(limit, /) */
QP_SYS_FN(qp_sys_setrecursionlimit)
{
    QPState *st = QP_STATE(ctx);
    int new_limit, depth;

    if (qp_arg_int(ctx, args[0], &new_limit) < 0)
        return JS_EXCEPTION;
    if (new_limit < 1)
        return qp_raise_value_error(ctx, "recursion limit must be greater or equal than 1");
    depth = st->recursion_depth;
    if (depth >= new_limit)
        return qp_raise(ctx, QP_EXC(ctx, RecursionError),
                        "cannot set the recursion limit to %d at the recursion depth %d: the limit is too low",
                        new_limit, depth);
    st->recursion_limit = new_limit;
    return JS_UNDEFINED;
}

/* sys.flags instance from the configuration. */
static JSValue qp_sys_make_flags(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    QPConfig *c = &st->config;
    JSValue items[22];
    int i = 0;

#define QP_FLAG(expr) (items[i++] = JS_NewInt32(ctx, (expr)))
    QP_FLAG(0);                                     /* debug (parser debug) */
    QP_FLAG(c->inspect);
    QP_FLAG(c->interactive);
    QP_FLAG(c->optimize);
    QP_FLAG(c->dont_write_bytecode);
    QP_FLAG(c->no_user_site);
    QP_FLAG(c->no_site);
    QP_FLAG(c->ignore_environment);
    QP_FLAG(c->verbose);
    QP_FLAG(c->bytes_warning);
    QP_FLAG(c->quiet);
    QP_FLAG(c->hash_seed == NULL || strcmp(c->hash_seed, "random") == 0 || strtoul(c->hash_seed, NULL, 10) != 0);
    QP_FLAG(c->isolated);
    items[i++] = qp_bool_new(c->dev_mode != 0);
    QP_FLAG(c->utf8_mode);
    QP_FLAG(c->warn_default_encoding);
    items[i++] = qp_bool_new(c->safe_path != 0);
    QP_FLAG(st->int_max_str_digits);
    /* attribute-only fields */
    QP_FLAG(1);                                     /* gil: build with a GIL */
    QP_FLAG(0);                                     /* thread_inherit_context */
    QP_FLAG(0);                                     /* context_aware_warnings */
    QP_FLAG(c->lazy_imports);
#undef QP_FLAG
    return qp_sys_ss_make(ctx, QP_SYS_SS_FLAGS, items);
}

QP_SYS_FN(qp_sys_get_int_max_str_digits)
{
    return JS_NewInt32(ctx, QP_STATE(ctx)->int_max_str_digits);
}

/* set_int_max_str_digits(maxdigits) */
QP_SYS_FN(qp_sys_set_int_max_str_digits)
{
    static const char *const kwlist[] = { "maxdigits", NULL };
    static const QPArgSpec spec = { "set_int_max_str_digits", kwlist, 1, 1, 0 };
    JSValueConst out[1];
    int maxdigits;
    JSValue flags;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (qp_arg_int(ctx, out[0], &maxdigits) < 0)
        return JS_EXCEPTION;
    if (maxdigits != 0 && maxdigits < QP_LONG_MAX_STR_DIGITS_THRESHOLD)
        return qp_raise_value_error(ctx, "maxdigits must be >= %d or 0 for unlimited",
                                    QP_LONG_MAX_STR_DIGITS_THRESHOLD);
    QP_STATE(ctx)->int_max_str_digits = maxdigits;
    /* sys.flags.int_max_str_digits follows (CPython _PySys_SetFlagInt) */
    flags = qp_sys_make_flags(ctx);
    if (JS_IsException(flags))
        return flags;
    if (qp_sys_dict_set(ctx, QP_STATE(ctx)->sys_dict, "flags", flags) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* intern(string, /) */
QP_SYS_FN(qp_sys_intern)
{
    JSValueConst s = args[0];
    JSAtom atom;
    JSValue res;

    if (!qp_str_check(ctx, s))
        return qp_raise_type_error(ctx, "intern() argument must be str, not %T", s);
    if (!qp_str_check_exact(s)) {
        JSValue tn = qp_sys_tp_name(ctx, qp_type_of(ctx, s));
        if (JS_IsException(tn))
            return tn;
        qp_raise_type_error(ctx, "can't intern %.400U", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    atom = qp_str_to_atom(ctx, s);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    res = JS_AtomToString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return res;
}

/* _is_interned(string, /) */
QP_SYS_FN(qp_sys_is_interned)
{
    JSValueConst s = args[0];
    if (!qp_str_check(ctx, s))
        return qp_raise_type_error(ctx, "_is_interned() argument must be str, not %T", s);
    return qp_bool_new(JS_VALUE_GET_TAG(s) == JS_TAG_STRING &&
                       JS_VALUE_GET_STRING(s)->atom_type != 0);
}

/* getsizeof(object [, default]) */
QP_SYS_FN(qp_sys_getsizeof)
{
    static const char *const kwlist[] = { "object", "default", NULL };
    static const QPArgSpec spec = { "getsizeof", kwlist, 1, 2, 0 };
    JSValueConst out[2];
    JSValue method, res;
    QPType *t;
    Py_ssize_t size, presize = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    t = qp_type_of(ctx, out[0]);
    method = qp_lookup_special(ctx, out[0], QP_ATOM(ctx, __sizeof__));
    if (QP_IS_NULL(method)) {
        if (!JS_HasException(ctx)) {
            JSValue tn = qp_sys_tp_name(ctx, t);
            if (!JS_IsException(tn)) {
                qp_raise_type_error(ctx, "Type %.100U doesn't define __sizeof__", tn);
                JS_FreeValue(ctx, tn);
            }
        }
        goto error;
    }
    res = qp_call0(ctx, method);
    JS_FreeValue(ctx, method);
    if (JS_IsException(res))
        goto error;
    if (qp_as_ssize(ctx, res, QP_EXC(ctx, OverflowError), &size) < 0) {
        JS_FreeValue(ctx, res);
        goto error;
    }
    JS_FreeValue(ctx, res);
    if (size < 0) {
        qp_raise_value_error(ctx, "__sizeof__() should return >= 0");
        goto error;
    }
    /* _PyType_PreHeaderSize: GC header for container types, managed
       dict/weakref pointers for instances with a __dict__. */
    if (t != QP_TYPE(ctx, int) && t != QP_TYPE(ctx, bool) && t != QP_TYPE(ctx, float) &&
        t != QP_TYPE(ctx, complex) && t != QP_TYPE(ctx, str) && t != QP_TYPE(ctx, bytes) &&
        t != QP_TYPE(ctx, bytearray) && t != QP_TYPE(ctx, NoneType) &&
        t != QP_TYPE(ctx, NotImplementedType) && t != QP_TYPE(ctx, ellipsis) &&
        t != QP_TYPE(ctx, range))
        presize += 16;
    if (t->flags & QP_TPFLAGS_MANAGED_DICT)
        presize += 2 * (Py_ssize_t)sizeof(void *);
    return qp_long_from_int64(ctx, size + presize);
error:
    if (!QP_IS_NULL(out[1]) && qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return JS_DupValue(ctx, out[1]);
    }
    return JS_EXCEPTION;
}

/* getrefcount(object, /): approximate.  Heap values (objects, str, big
   ints, ...) report the QuickJS reference count of the value, which lives
   in the malloc block header just before the payload (js_rc()); like
   CPython it includes the reference held by the call's argument.  It counts
   QuickJS references, not CPython ones: references held by shapes, atoms,
   caches or C-extension mirrors are not CPython-equivalent, so only use it
   for relative comparisons.  Unboxed values (small int, float, bool, None,
   ...) have no count and report the immortal value like CPython's immortal
   objects. */
QP_SYS_FN(qp_sys_getrefcount)
{
    JSValueConst o = args[0];
    if (JS_VALUE_HAS_REF_COUNT(o))
        return qp_long_from_int64(ctx, js_rc(JS_VALUE_GET_PTR(o))->ref_count +
                                       qp_capi_c_refs(ctx, o));
    return qp_long_from_int64(ctx, QP_IMMORTAL_REFCNT);
}

/* ---- dlopen flags (CPython Python/import.c _PyImport_{Get,Set}DLOpenFlags);
   used by python/capi_dynload.c ------------------------------------------ */

/* getdlopenflags() */
QP_SYS_FN(qp_sys_getdlopenflags)
{
    return JS_NewInt32(ctx, __atomic_load_n(&QP_STATE(ctx)->dlopenflags, __ATOMIC_RELAXED));
}

/* setdlopenflags(flags, /) */
QP_SYS_FN(qp_sys_setdlopenflags)
{
    int flags;
    if (qp_arg_int(ctx, args[0], &flags) < 0)
        return JS_EXCEPTION;
    __atomic_store_n(&QP_STATE(ctx)->dlopenflags, flags, __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}

/* ---- threads (python/ceval.c "Threads and the GIL") -------------------- */

/* getswitchinterval() */
QP_SYS_FN(qp_sys_getswitchinterval)
{
    return qp_float_new(ctx, 1e-6 * (double)__atomic_load_n(&qp_gil_interval_us,
                                                           __ATOMIC_RELAXED));
}

/* setswitchinterval(interval, /) */
QP_SYS_FN(qp_sys_setswitchinterval)
{
    double interval;
    if (qp_arg_check_positional(ctx, "setswitchinterval", nargs, 1, 1) < 0 ||
        qp_float_as_double(ctx, args[0], &interval) < 0)
        return JS_EXCEPTION;
    if (interval <= 0.0)
        return qp_raise_value_error(ctx, "switch interval must be strictly positive");
    __atomic_store_n(&qp_gil_interval_us, (unsigned long)(1e6 * interval), __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}

/* The top frame of a thread's frame chain that is a Python frame (NULL if
   none): the running thread's or a saved one. */
static JSStackFrame *qp_sys_thread_top_frame(JSContext *ctx, QPThreadState *ts)
{
    QPThreads *thr = ctx->rt->py_threads;
    JSStackFrame *sf;
    if (!thr || ts == thr->current)
        return qp_py_frame_at(ctx, 0);
    for (sf = ts->js.current_stack_frame; sf; sf = sf->prev_frame) {
        if (qp_fr_sf_func(sf))
            return sf;
    }
    return NULL;
}

/* _PyErr_GetTopmostException of a thread (borrowed, None if none).  The
   bottom item of every chain is QPState.exc_info_base, which holds the
   running thread's value: a saved thread's is in its state. */
static JSValue qp_sys_thread_topmost_exc(JSContext *ctx, QPThreadState *ts)
{
    QPState *s = QP_STATE(ctx);
    QPThreads *thr = ctx->rt->py_threads;
    QPExcStackItem *it;
    if (!thr || ts == thr->current) {
        it = s->exc_info;
        while ((JS_IsUndefined(it->exc_value) || QP_IS_NULL(it->exc_value)) && it->previous)
            it = it->previous;
        return QP_IS_NULL(it->exc_value) ? JS_UNDEFINED : it->exc_value;
    }
    for (it = ts->exc_info;; it = it->previous) {
        JSValue v = it == &s->exc_info_base ? ts->exc_info_base.exc_value : it->exc_value;
        if (!JS_IsUndefined(v) && !QP_IS_NULL(v))
            return v;
        if (it == &s->exc_info_base || !it->previous)
            return JS_UNDEFINED;
    }
}

/* _current_frames() / _current_exceptions() (magic 1):
   _PyThread_CurrentFrames / _PyThread_CurrentExceptions */
QP_SYS_FN(qp_sys_current_frames)
{
    QPThreads *thr = ctx->rt->py_threads;
    struct list_head *el;
    JSValue d = qp_dict_new(ctx);

    if (JS_IsException(d))
        return d;
    for (el = thr ? thr->tstates.next : NULL; ; el = el->next) {
        QPThreadState *ts = NULL;
        uint64_t ident;
        JSValue key, val;
        int r;
        if (thr) {
            if (el == &thr->tstates)
                break;
            ts = list_entry(el, QPThreadState, link);
            ident = ts->ident;
        } else {
            ident = (uint64_t)(uintptr_t)pthread_self();
        }
        if (magic) {
            val = JS_DupValue(ctx, qp_sys_thread_topmost_exc(ctx, ts));
        } else {
            JSStackFrame *sf = qp_sys_thread_top_frame(ctx, ts);
            val = sf ? JS_DupValue(ctx, qp_frame_of(ctx, sf)) : QP_NULL;
        }
        if (JS_IsException(val))
            goto fail;
        if (!QP_IS_NULL(val)) {
            key = qp_long_from_uint64(ctx, ident);
            if (JS_IsException(key)) {
                JS_FreeValue(ctx, val);
                goto fail;
            }
            r = qp_dict_set_item(ctx, d, key, val);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, val);
            if (r < 0)
                goto fail;
        }
        if (!thr)
            break;
    }
    return d;
 fail:
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

/* _getframe(depth=0, /) */
QP_SYS_FN(qp_sys_getframe)
{
    int depth = 0;
    if (qp_arg_check_positional(ctx, "_getframe", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 1 && qp_arg_int(ctx, args[0], &depth) < 0)
        return JS_EXCEPTION;
    return qp_getframe(ctx, depth);
}

/* _getframemodulename(depth=0) */
QP_SYS_FN(qp_sys_getframemodulename)
{
    static const char *const kwlist[] = { "depth", NULL };
    static const QPArgSpec spec = { "_getframemodulename", kwlist, 0, 1, 0 };
    JSValueConst out[1];
    int depth = 0;
    struct JSStackFrame *sf;
    QPFuncData *fd;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(out[0]) && qp_arg_int(ctx, out[0], &depth) < 0)
        return JS_EXCEPTION;
    if (depth < 0)
        depth = 0;
    sf = qp_py_frame_at(ctx, depth);
    if (!sf)
        return JS_UNDEFINED;
    fd = qp_function_data(sf->cur_func);
    if (!fd)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, fd->module);
}

QP_SYS_FN(qp_sys_getdefaultencoding)
{
    return qp_str_from_cstr(ctx, "utf-8");
}

QP_SYS_FN(qp_sys_getfilesystemencoding)
{
    return qp_str_from_cstr(ctx, "utf-8");
}

QP_SYS_FN(qp_sys_getfilesystemencodeerrors)
{
    return qp_str_from_cstr(ctx, "surrogateescape");
}

/* audit(event, *args) */
QP_SYS_FN(qp_sys_audit)
{
    QPState *st = QP_STATE(ctx);
    Py_ssize_t n, i;
    JSValue hooks, tup;

    if (nargs == 0)
        return qp_raise_type_error(ctx, "audit() missing 1 required positional argument: 'event'");
    if (JS_VALUE_GET_TAG(st->audit_hooks) != JS_TAG_OBJECT || qp_list_size(st->audit_hooks) == 0)
        return JS_UNDEFINED;
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "expected str for argument 'event', not %.200s",
                                   qp_type_of(ctx, args[0])->tp_name ? qp_type_of(ctx, args[0])->tp_name : "object");
    tup = qp_tuple_pack(ctx, nargs - 1, args + 1);
    if (JS_IsException(tup))
        return tup;
    /* snapshot: hooks may add hooks */
    hooks = qp_list_as_tuple(ctx, st->audit_hooks);
    if (JS_IsException(hooks)) {
        JS_FreeValue(ctx, tup);
        return hooks;
    }
    n = QP_TUPLE_SIZE(hooks);
    for (i = 0; i < n; i++) {
        JSValue r = qp_call2(ctx, QP_TUPLE_ITEMS(hooks)[i], args[0], tup);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, hooks);
            JS_FreeValue(ctx, tup);
            return r;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, hooks);
    JS_FreeValue(ctx, tup);
    return JS_UNDEFINED;
}

/* addaudithook(hook) */
QP_SYS_FN(qp_sys_addaudithook)
{
    static const char *const kwlist[] = { "hook", NULL };
    static const QPArgSpec spec = { "addaudithook", kwlist, 1, 1, 0 };
    QPState *st = QP_STATE(ctx);
    JSValueConst out[1];
    JSValue ev, r;
    JSValueConst a[1];

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    /* Invoke existing audit hooks to allow them an opportunity to abort
       the addition; a RuntimeError cancels it silently. */
    ev = qp_str_from_cstr(ctx, "sys.addaudithook");
    if (JS_IsException(ev))
        return ev;
    a[0] = ev;
    r = qp_sys_audit(ctx, self, a, 1, QP_NULL, 0);
    JS_FreeValue(ctx, ev);
    if (JS_IsException(r)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, RuntimeError))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return JS_UNDEFINED;
        }
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, r);
    if (JS_VALUE_GET_TAG(st->audit_hooks) != JS_TAG_OBJECT) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return l;
        JS_FreeValue(ctx, st->audit_hooks);
        st->audit_hooks = l;
    }
    if (qp_list_append(ctx, st->audit_hooks, out[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* breakpointhook(*args, **kws) */
QP_SYS_FN(qp_sys_breakpointhook)
{
    const char *env = qp_sys_getenv(ctx, "PYTHONBREAKPOINT");
    char *envar;
    const char *last_dot, *attrname;
    char *modulepath;
    JSValue module, hook, res;
    JSAtom attr;
    size_t len;

    if (env == NULL || env[0] == '\0')
        env = "pdb.set_trace";
    else if (!strcmp(env, "0"))
        return JS_UNDEFINED;
    len = strlen(env);
    envar = js_malloc(ctx, len + 1);
    if (!envar)
        return JS_EXCEPTION;
    memcpy(envar, env, len + 1);
    last_dot = strrchr(envar, '.');
    if (last_dot == NULL) {
        modulepath = js_malloc(ctx, sizeof("builtins"));
        if (!modulepath) {
            js_free(ctx, envar);
            return JS_EXCEPTION;
        }
        memcpy(modulepath, "builtins", sizeof("builtins"));
        attrname = envar;
    } else if (last_dot != envar) {
        modulepath = js_malloc(ctx, (size_t)(last_dot - envar) + 1);
        if (!modulepath) {
            js_free(ctx, envar);
            return JS_EXCEPTION;
        }
        memcpy(modulepath, envar, (size_t)(last_dot - envar));
        modulepath[last_dot - envar] = '\0';
        attrname = last_dot + 1;
    } else {
        goto warn;
    }
    module = qp_import_module(ctx, modulepath);
    js_free(ctx, modulepath);
    if (JS_IsException(module)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, ImportError)))
            goto warn;
        js_free(ctx, envar);
        return JS_EXCEPTION;
    }
    attr = JS_NewAtom(ctx, attrname);
    if (attr == JS_ATOM_NULL) {
        JS_FreeValue(ctx, module);
        js_free(ctx, envar);
        return JS_EXCEPTION;
    }
    hook = qp_getattr(ctx, module, attr);
    JS_FreeAtom(ctx, attr);
    JS_FreeValue(ctx, module);
    if (JS_IsException(hook)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError)))
            goto warn;
        js_free(ctx, envar);
        return JS_EXCEPTION;
    }
    js_free(ctx, envar);
    res = qp_vectorcall(ctx, hook, args, (size_t)nargs, kwnames);
    JS_FreeValue(ctx, hook);
    return res;
warn:
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    {
        int status = qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 0,
                             "Ignoring unimportable $PYTHONBREAKPOINT: \"%s\"", envar);
        js_free(ctx, envar);
        if (status < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* get_asyncgen_hooks() */
QP_SYS_FN(qp_sys_get_asyncgen_hooks)
{
    QPState *st = QP_STATE(ctx);
    JSValue items[2];
    items[0] = qp_none_if_null(ctx, st->asyncgen_firstiter);
    items[1] = qp_none_if_null(ctx, st->asyncgen_finalizer);
    return qp_sys_ss_make(ctx, QP_SYS_SS_ASYNCGEN_HOOKS, items);
}

/* set_asyncgen_hooks([firstiter] [, finalizer]) */
QP_SYS_FN(qp_sys_set_asyncgen_hooks)
{
    static const char *const kwlist[] = { "firstiter", "finalizer", NULL };
    static const QPArgSpec spec = { "set_asyncgen_hooks", kwlist, 0, 2, 0 };
    QPState *st = QP_STATE(ctx);
    JSValueConst out[2];
    JSValueConst firstiter, finalizer;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    firstiter = out[0];
    finalizer = out[1];
    if (!QP_IS_NULL(finalizer) && !JS_IsUndefined(finalizer) && !qp_sys_callable(ctx, finalizer))
        return qp_raise_type_error(ctx, "callable finalizer expected, got %.50s",
                                   qp_type_of(ctx, finalizer)->tp_name ? qp_type_of(ctx, finalizer)->tp_name : "object");
    if (!QP_IS_NULL(firstiter) && !JS_IsUndefined(firstiter) && !qp_sys_callable(ctx, firstiter))
        return qp_raise_type_error(ctx, "callable firstiter expected, got %.50s",
                                   qp_type_of(ctx, firstiter)->tp_name ? qp_type_of(ctx, firstiter)->tp_name : "object");
    if (!QP_IS_NULL(finalizer)) {
        JS_FreeValue(ctx, st->asyncgen_finalizer);
        st->asyncgen_finalizer = JS_DupValue(ctx, finalizer);
    }
    if (!QP_IS_NULL(firstiter)) {
        JS_FreeValue(ctx, st->asyncgen_firstiter);
        st->asyncgen_firstiter = JS_DupValue(ctx, firstiter);
    }
    return JS_UNDEFINED;
}

/* set_coroutine_origin_tracking_depth(depth): per thread, like CPython */
QP_SYS_FN(qp_sys_set_coroutine_origin_tracking_depth)
{
    static const char *const kwlist[] = { "depth", NULL };
    static const QPArgSpec spec = { "set_coroutine_origin_tracking_depth", kwlist, 1, 1, 0 };
    JSValueConst out[1];
    int depth;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (qp_arg_int(ctx, out[0], &depth) < 0)
        return JS_EXCEPTION;
    if (depth < 0)
        return qp_raise_value_error(ctx, "depth must be >= 0");
    QP_STATE(ctx)->coro_origin_depth = depth;
    return JS_UNDEFINED;
}

/* get_coroutine_origin_tracking_depth() */
QP_SYS_FN(qp_sys_get_coroutine_origin_tracking_depth)
{
    return JS_NewInt32(ctx, QP_STATE(ctx)->coro_origin_depth);
}

/* call_tracing(func, args, /): the call is traced even from a trace
   function (_PyEval_CallTracing) */
QP_SYS_FN(qp_sys_call_tracing)
{
    QPState *st = QP_STATE(ctx);
    JSValue r;
    int saved;

    if (qp_arg_check_positional(ctx, "call_tracing", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_tuple_check(ctx, args[1]))
        return qp_raise_type_error(ctx, "call_tracing() argument 2 must be tuple, not %T", args[1]);
    saved = st->tracing;
    st->tracing = 0;
    r = qp_call_tuple(ctx, args[0], args[1], QP_NULL);
    st->tracing = saved;
    return r;
}

/* ---- M6: settrace / setprofile (frameobject.c qp_trace_*) --------------- */

static int qp_sys_audit_event(JSContext *ctx, const char *event)
{
    JSValueConst a[1];
    JSValue ev, r;

    ev = qp_str_from_cstr(ctx, event);
    if (JS_IsException(ev))
        return -1;
    a[0] = ev;
    r = qp_sys_audit(ctx, JS_UNDEFINED, a, 1, QP_NULL, 0);
    JS_FreeValue(ctx, ev);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* settrace(function, /), setprofile(function, /) and the *allthreads
   variants (one thread): magic = is_profile */
QP_SYS_FN(qp_sys_settrace)
{
    if (qp_sys_audit_event(ctx, magic ? "sys.setprofile" : "sys.settrace") < 0 ||
        qp_trace_set(ctx, magic, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* gettrace(), getprofile(): magic = is_profile */
QP_SYS_FN(qp_sys_gettrace)
{
    QPState *st = QP_STATE(ctx);
    JSValue f = magic ? st->profile_func : st->trace_func;

    return JS_VALUE_GET_TAG(f) == JS_TAG_OBJECT ? JS_DupValue(ctx, f) : JS_UNDEFINED;
}

QP_SYS_FN(qp_sys_is_finalizing)
{
    return qp_bool_new(QP_STATE(ctx)->finalizing);
}

QP_SYS_FN(qp_sys_is_gil_enabled)
{
    return qp_bool_new(TRUE);
}

QP_SYS_FN(qp_sys_is_remote_debug_enabled)
{
    return qp_bool_new(FALSE);
}

/* _clear_type_cache() / _clear_internal_caches(): drop the method cache. */
QP_SYS_FN(qp_sys_clear_type_cache)
{
    QPState *st = QP_STATE(ctx);
    memset(st->mcache, 0, sizeof(st->mcache));
    return JS_UNDEFINED;
}

/* set_lazy_imports(mode) */
QP_SYS_FN(qp_sys_set_lazy_imports)
{
    static const char *const kwlist[] = { "mode", NULL };
    static const QPArgSpec spec = { "set_lazy_imports", kwlist, 1, 1, 0 };
    JSValueConst out[1];
    const char *s;
    int mode;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, out[0]))
        return qp_raise_type_error(ctx, "mode must be a string: 'normal' or 'all'");
    s = qp_str_utf8(ctx, out[0], NULL);
    if (!s)
        return JS_EXCEPTION;
    if (!strcmp(s, "normal"))
        mode = 0;
    else if (!strcmp(s, "all"))
        mode = 1;
    else
        mode = -1;
    JS_FreeCString(ctx, s);
    if (mode < 0)
        return qp_raise_value_error(ctx, "mode must be 'normal' or 'all'");
    QP_STATE(ctx)->lazy_imports_mode = mode;
    return JS_UNDEFINED;
}

QP_SYS_FN(qp_sys_get_lazy_imports)
{
    switch (QP_STATE(ctx)->lazy_imports_mode) {
    case 0: return qp_str_from_cstr(ctx, "normal");
    case 1: return qp_str_from_cstr(ctx, "all");
    default: return qp_raise_runtime_error(ctx, "unknown lazy imports mode");
    }
}

static const QPMethodDef qp_sys_methods[] = {
    { "addaudithook", qp_sys_addaudithook, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "addaudithook($module, /, hook)\n--\n\nAdds a new audit hook callback." },
    { "audit", qp_sys_audit, QP_METH_FASTCALL, 0,
      "audit($module, event, /, *args)\n--\n\nPasses the event to any audit hooks that are attached." },
    { "breakpointhook", qp_sys_breakpointhook, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "breakpointhook($module, /, *args, **kwargs)\n--\n\nThis hook function is called by built-in breakpoint()." },
    { "_clear_internal_caches", qp_sys_clear_type_cache, QP_METH_NOARGS, 0,
      "_clear_internal_caches($module, /)\n--\n\nClear all internal performance-related caches." },
    { "_clear_type_cache", qp_sys_clear_type_cache, QP_METH_NOARGS, 0,
      "_clear_type_cache($module, /)\n--\n\nClear the internal type lookup cache." },
    { "displayhook", qp_sys_displayhook, QP_METH_O, 0,
      "displayhook($module, object, /)\n--\n\nPrint an object to sys.stdout and also save it in builtins._" },
    { "exception", qp_sys_exception, QP_METH_NOARGS, 0,
      "exception($module, /)\n--\n\nReturn the current exception." },
    { "exc_info", qp_sys_exc_info, QP_METH_NOARGS, 0,
      "exc_info($module, /)\n--\n\nReturn current exception information: (type, value, traceback)." },
    { "excepthook", qp_sys_excepthook, QP_METH_FASTCALL, 0,
      "excepthook($module, exctype, value, traceback, /)\n--\n\nHandle an exception by displaying it with a traceback on sys.stderr." },
    { "exit", qp_sys_exit, QP_METH_FASTCALL, 0,
      "exit($module, status=None, /)\n--\n\nExit the interpreter by raising SystemExit(status)." },
    { "getdefaultencoding", qp_sys_getdefaultencoding, QP_METH_NOARGS, 0,
      "getdefaultencoding($module, /)\n--\n\nReturn the current default encoding used by the Unicode implementation." },
    { "getfilesystemencoding", qp_sys_getfilesystemencoding, QP_METH_NOARGS, 0,
      "getfilesystemencoding($module, /)\n--\n\nReturn the encoding used to convert Unicode filenames to OS filenames." },
    { "getfilesystemencodeerrors", qp_sys_getfilesystemencodeerrors, QP_METH_NOARGS, 0,
      "getfilesystemencodeerrors($module, /)\n--\n\nReturn the error mode used Unicode to OS filename conversion." },
    { "getrefcount", qp_sys_getrefcount, QP_METH_O, 0,
      "getrefcount($module, object, /)\n--\n\nReturn the reference count of object." },
    { "getrecursionlimit", qp_sys_getrecursionlimit, QP_METH_NOARGS, 0,
      "getrecursionlimit($module, /)\n--\n\nReturn the current value of the recursion limit." },
    { "getsizeof", qp_sys_getsizeof, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "getsizeof(object [, default]) -> int\n\nReturn the size of object in bytes." },
    { "_getframe", qp_sys_getframe, QP_METH_FASTCALL, 0,
      "_getframe($module, depth=0, /)\n--\n\nReturn a frame object from the call stack." },
    { "_current_frames", qp_sys_current_frames, QP_METH_NOARGS, 0,
      "_current_frames($module, /)\n--\n\n"
      "Return a dict mapping each thread's thread id to its current stack frame.\n\n"
      "This function should be used for specialized purposes only." },
    { "_current_exceptions", qp_sys_current_frames, QP_METH_NOARGS, 1,
      "_current_exceptions($module, /)\n--\n\n"
      "Return a dict mapping each thread's identifier to its current raised exception.\n\n"
      "This function should be used for specialized purposes only." },
    { "getdlopenflags", qp_sys_getdlopenflags, QP_METH_NOARGS, 0,
      "getdlopenflags($module, /)\n--\n\n"
      "Return the current value of the flags that are used for dlopen calls.\n\n"
      "The flag constants are defined in the os module." },
    { "setdlopenflags", qp_sys_setdlopenflags, QP_METH_O, 0,
      "setdlopenflags($module, flags, /)\n--\n\n"
      "Set the flags used by the interpreter for dlopen calls.\n\n"
      "This is used, for example, when the interpreter loads extension\n"
      "modules. Among other things, this will enable a lazy resolving of\n"
      "symbols when importing a module, if called as sys.setdlopenflags(0).\n"
      "To share symbols across extension modules, call as\n"
      "sys.setdlopenflags(os.RTLD_GLOBAL).  Symbolic names for the flag\n"
      "modules can be found in the os module (RTLD_xxx constants, e.g.\n"
      "os.RTLD_LAZY)." },
    { "getswitchinterval", qp_sys_getswitchinterval, QP_METH_NOARGS, 0,
      "getswitchinterval($module, /)\n--\n\n"
      "Return the current thread switch interval; see sys.setswitchinterval()." },
    { "setswitchinterval", qp_sys_setswitchinterval, QP_METH_FASTCALL, 0,
      "setswitchinterval($module, interval, /)\n--\n\n"
      "Set the ideal thread switching delay inside the Python interpreter.\n\n"
      "The actual frequency of switching threads can be lower if the\n"
      "interpreter executes long sequences of uninterruptible code\n"
      "(this is implementation-specific and workload-dependent).\n\n"
      "The parameter must represent the desired switching delay in seconds\n"
      "A typical value is 0.005 (5 milliseconds)." },
    { "_getframemodulename", qp_sys_getframemodulename, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "_getframemodulename($module, /, depth=0)\n--\n\nReturn the name of the module for a calling frame." },
    { "settrace", qp_sys_settrace, QP_METH_O, 0,
      "settrace($module, function, /)\n--\n\nSet the global debug tracing function.\n\n"
      "It will be called on each function call.  See the debugger chapter\nin the library manual." },
    { "gettrace", qp_sys_gettrace, QP_METH_NOARGS, 0,
      "gettrace($module, /)\n--\n\nReturn the global debug tracing function set with sys.settrace.\n\n"
      "See the debugger chapter in the library manual." },
    { "setprofile", qp_sys_settrace, QP_METH_O, 1,
      "setprofile($module, function, /)\n--\n\nSet the profiling function.\n\n"
      "It will be called on each function call and return.  See the profiler\nchapter in the library manual." },
    { "getprofile", qp_sys_gettrace, QP_METH_NOARGS, 1,
      "getprofile($module, /)\n--\n\nReturn the profiling function set with sys.setprofile.\n\n"
      "See the profiler chapter in the library manual." },
    { "_settraceallthreads", qp_sys_settrace, QP_METH_O, 0,
      "_settraceallthreads($module, function, /)\n--\n\nSet the global debug tracing function in all running threads belonging to the current interpreter.\n\n"
      "It will be called on each function call. See the debugger chapter\nin the library manual." },
    { "_setprofileallthreads", qp_sys_settrace, QP_METH_O, 1,
      "_setprofileallthreads($module, function, /)\n--\n\nSet the profiling function in all running threads belonging to the current interpreter.\n\n"
      "It will be called on each function call and return.  See the profiler\nchapter in the library manual." },
    { "intern", qp_sys_intern, QP_METH_O, 0,
      "intern($module, string, /)\n--\n\n``Intern'' the given string." },
    { "_is_interned", qp_sys_is_interned, QP_METH_O, 0,
      "_is_interned($module, string, /)\n--\n\nReturn True if the given string is \"interned\"." },
    { "is_finalizing", qp_sys_is_finalizing, QP_METH_NOARGS, 0,
      "is_finalizing($module, /)\n--\n\nReturn True if Python is exiting." },
    { "setrecursionlimit", qp_sys_setrecursionlimit, QP_METH_O, 0,
      "setrecursionlimit($module, limit, /)\n--\n\nSet the maximum depth of the Python interpreter stack to n." },
    { "call_tracing", qp_sys_call_tracing, QP_METH_FASTCALL, 0,
      "call_tracing($module, func, args, /)\n--\n\nCall func(*args), while tracing is enabled." },
    { "set_asyncgen_hooks", qp_sys_set_asyncgen_hooks, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_asyncgen_hooks([firstiter] [, finalizer])\n\nSet a finalizer for async generators objects." },
    { "set_coroutine_origin_tracking_depth", qp_sys_set_coroutine_origin_tracking_depth,
      QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_coroutine_origin_tracking_depth($module, /, depth)\n--\n\n"
      "Enable or disable origin tracking for coroutine objects in this thread.\n\n"
      "Coroutine objects will track 'depth' frames of traceback information\n"
      "about where they came from, available in their cr_origin attribute.\n\n"
      "Set a depth of 0 to disable." },
    { "get_coroutine_origin_tracking_depth", qp_sys_get_coroutine_origin_tracking_depth,
      QP_METH_NOARGS, 0,
      "get_coroutine_origin_tracking_depth($module, /)\n--\n\n"
      "Check status of origin tracking for coroutine objects in this thread." },
    { "get_asyncgen_hooks", qp_sys_get_asyncgen_hooks, QP_METH_NOARGS, 0,
      "get_asyncgen_hooks($module, /)\n--\n\nReturn the installed asynchronous generators hooks." },
    { "is_remote_debug_enabled", qp_sys_is_remote_debug_enabled, QP_METH_NOARGS, 0,
      "is_remote_debug_enabled($module, /)\n--\n\nReturn True if remote debugging is enabled, False otherwise." },
    { "unraisablehook", qp_sys_unraisablehook, QP_METH_O, 0,
      "unraisablehook($module, unraisable, /)\n--\n\nHandle an unraisable exception." },
    { "get_int_max_str_digits", qp_sys_get_int_max_str_digits, QP_METH_NOARGS, 0,
      "get_int_max_str_digits($module, /)\n--\n\nReturn the maximum string digits limit for non-binary int<->str conversions." },
    { "set_int_max_str_digits", qp_sys_set_int_max_str_digits, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_int_max_str_digits($module, /, maxdigits)\n--\n\nSet the maximum string digits limit for non-binary int<->str conversions." },
    { "get_lazy_imports", qp_sys_get_lazy_imports, QP_METH_NOARGS, 0,
      "get_lazy_imports($module, /)\n--\n\nGets the global lazy imports mode." },
    { "set_lazy_imports", qp_sys_set_lazy_imports, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "set_lazy_imports($module, /, mode)\n--\n\nSets the global lazy imports mode." },
    { "_is_gil_enabled", qp_sys_is_gil_enabled, QP_METH_NOARGS, 0,
      "_is_gil_enabled($module, /)\n--\n\nReturn True if the GIL is currently enabled and False otherwise." },
    { NULL }
};

/* ========================================================================= */
/* sys attributes                                                             */
/* ========================================================================= */

static const char *const qp_stdlib_module_names[] = {
"__future__", "_abc", "_aix_support", "_android_support", "_apple_support", "_ast", "_ast_unparse", "_asyncio", "_bisect", "_blake2", "_bz2",
"_codecs", "_codecs_cn", "_codecs_hk", "_codecs_iso2022", "_codecs_jp", "_codecs_kr", "_codecs_tw", "_collections", "_collections_abc", "_colorize",
"_compat_pickle", "_contextvars", "_csv", "_ctypes", "_curses", "_curses_panel", "_datetime", "_dbm", "_decimal", "_elementtree",
"_frozen_importlib", "_frozen_importlib_external", "_functools", "_gdbm", "_hashlib", "_heapq", "_hmac", "_imp", "_interpchannels", "_interpqueues",
"_interpreters", "_io", "_ios_support", "_json", "_locale", "_lsprof", "_lzma", "_markupbase", "_math_integer", "_md5", "_multibytecodec",
"_multiprocessing", "_opcode", "_opcode_metadata", "_operator", "_osx_support", "_overlapped", "_pickle", "_posixshmem", "_posixsubprocess",
"_py_abc", "_py_warnings", "_pydatetime", "_pydecimal", "_pyio", "_pylong", "_pyrepl", "_queue", "_random", "_remote_debugging", "_scproxy", "_sha1",
"_sha2", "_sha3", "_signal", "_sitebuiltins", "_socket", "_sqlite3", "_sre", "_ssl", "_stat", "_statistics", "_string", "_strptime", "_struct",
"_suggestions", "_symtable", "_sysconfig", "_thread", "_threading_local", "_tkinter", "_tokenize", "_tracemalloc", "_types", "_typing", "_uuid",
"_warnings", "_weakref", "_weakrefset", "_winapi", "_wmi", "_zoneinfo", "_zstd", "abc", "annotationlib", "antigravity", "argparse", "array", "ast",
"asyncio", "atexit", "base64", "bdb", "binascii", "bisect", "builtins", "bz2", "cProfile", "calendar", "cmath", "cmd", "code", "codecs", "codeop",
"collections", "colorsys", "compileall", "compression", "concurrent", "configparser", "contextlib", "contextvars", "copy", "copyreg", "csv",
"ctypes", "curses", "dataclasses", "datetime", "dbm", "decimal", "difflib", "dis", "doctest", "email", "encodings", "ensurepip", "enum", "errno",
"faulthandler", "fcntl", "filecmp", "fileinput", "fnmatch", "fractions", "ftplib", "functools", "gc", "genericpath", "getopt", "getpass", "gettext",
"glob", "graphlib", "grp", "gzip", "hashlib", "heapq", "hmac", "html", "http", "idlelib", "imaplib", "importlib", "inspect", "io", "ipaddress",
"itertools", "json", "keyword", "linecache", "locale", "logging", "lzma", "mailbox", "marshal", "math", "mimetypes", "mmap", "modulefinder",
"msvcrt", "multiprocessing", "netrc", "nt", "ntpath", "nturl2path", "numbers", "opcode", "operator", "optparse", "os", "pathlib", "pdb", "pickle",
"pickletools", "pkgutil", "platform", "plistlib", "poplib", "posix", "posixpath", "pprint", "profile", "profiling", "pstats", "pty", "pwd",
"py_compile", "pyclbr", "pydoc", "pydoc_data", "pyexpat", "queue", "quopri", "random", "re", "readline", "reprlib", "resource", "rlcompleter",
"runpy", "sched", "secrets", "select", "selectors", "shelve", "shlex", "shutil", "signal", "site", "smtplib", "socket", "socketserver", "sqlite3",
"ssl", "stat", "statistics", "string", "stringprep", "struct", "subprocess", "symtable", "sys", "sysconfig", "syslog", "tabnanny", "tarfile",
"tempfile", "termios", "textwrap", "this", "threading", "time", "timeit", "tkinter", "token", "tokenize", "tomllib", "trace", "traceback",
"tracemalloc", "tty", "turtle", "turtledemo", "types", "typing", "unicodedata", "unittest", "urllib", "uuid", "venv", "warnings", "wave", "weakref",
"webbrowser", "winreg", "winsound", "wsgiref", "xml", "xmlrpc", "zipapp", "zipfile", "zipimport", "zlib", "zoneinfo",
};

static JSValue qp_sys_stdlib_module_names(JSContext *ctx)
{
    JSValue list, res;
    size_t i;

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    for (i = 0; i < countof(qp_stdlib_module_names); i++) {
        JSValue s = qp_str_from_cstr(ctx, qp_stdlib_module_names[i]);
        if (JS_IsException(s) || qp_list_append(ctx, list, s) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, s);
    }
    res = qp_frozenset_new(ctx, list);
    JS_FreeValue(ctx, list);
    return res;
}

/* List of str from argv-like C strings (filesystem decoding). */
static JSValue qp_sys_str_list(JSContext *ctx, int n, char **v)
{
    JSValue list = qp_list_new(ctx, 0);
    int i;

    if (JS_IsException(list))
        return list;
    for (i = 0; i < n; i++) {
        JSValue s = qp_sys_fs_str(ctx, v[i] ? v[i] : "");
        if (JS_IsException(s) || qp_list_append(ctx, list, s) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, s);
    }
    return list;
}

static int qp_sys_list_append_cstr(JSContext *ctx, JSValueConst list, const char *s)
{
    JSValue v = qp_sys_fs_str(ctx, s);
    int r;
    if (JS_IsException(v))
        return -1;
    r = qp_list_append(ctx, list, v);
    JS_FreeValue(ctx, v);
    return r;
}

static BOOL qp_sys_isdir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Installation layout: exe_dir = directory of the real executable;
   prefix = its parent when exe_dir is a "bin" directory, else exe_dir;
   stdlib = exe_dir/pylib (source tree) or prefix/lib/python3.15. */
typedef struct QPSysPaths {
    char exe_dir[PATH_MAX];
    char prefix[PATH_MAX];
    char stdlib[PATH_MAX];
    BOOL has_stdlib;
} QPSysPaths;

static void qp_sys_compute_paths(JSContext *ctx, QPSysPaths *p)
{
    const char *exe = QP_STATE(ctx)->config.executable;
    const char *home = qp_sys_getenv(ctx, "PYTHONHOME");
    char real[PATH_MAX], *slash;
    size_t n;

    p->exe_dir[0] = p->prefix[0] = p->stdlib[0] = '\0';
    p->has_stdlib = FALSE;
    if (exe && exe[0]) {
        if (!realpath(exe, real)) {
            n = strlen(exe);
            if (n >= sizeof(real))
                n = sizeof(real) - 1;
            memcpy(real, exe, n);
            real[n] = '\0';
        }
        slash = strrchr(real, '/');
        if (slash) {
            if (slash == real)
                slash[1] = '\0';
            else
                *slash = '\0';
            snprintf(p->exe_dir, sizeof(p->exe_dir), "%s", real);
        }
    }
    if (home && home[0]) {
        const char *colon = strchr(home, ':');
        n = colon ? (size_t)(colon - home) : strlen(home);
        if (n >= sizeof(p->prefix))
            n = sizeof(p->prefix) - 1;
        memcpy(p->prefix, home, n);
        p->prefix[n] = '\0';
    } else if (p->exe_dir[0]) {
        const char *base = strrchr(p->exe_dir, '/');
        base = base ? base + 1 : p->exe_dir;
        if (!strcmp(base, "bin") && base > p->exe_dir + 1) {
            n = (size_t)(base - 1 - p->exe_dir);
            memcpy(p->prefix, p->exe_dir, n);
            p->prefix[n] = '\0';
        } else {
            snprintf(p->prefix, sizeof(p->prefix), "%s", p->exe_dir);
        }
    }
    if (p->exe_dir[0]) {
        snprintf(p->stdlib, sizeof(p->stdlib), "%s/pylib", p->exe_dir);
        if (qp_sys_isdir(p->stdlib)) {
            p->has_stdlib = TRUE;
            return;
        }
    }
    if (p->prefix[0]) {
        snprintf(p->stdlib, sizeof(p->stdlib), "%s/lib/python3.15", p->prefix);
        if (qp_sys_isdir(p->stdlib))
            p->has_stdlib = TRUE;
    }
}

/* Provisional sys.path = PYTHONPATH entries (absolute) + stdlib (the
   importlib boot rebuilds it; the launcher inserts sys.path[0]). */
static JSValue qp_sys_make_path(JSContext *ctx, const QPSysPaths *p)
{
    QPConfig *c = &QP_STATE(ctx)->config;
    JSValue list = qp_list_new(ctx, 0);
    char cwd[PATH_MAX], entry[PATH_MAX * 2];

    if (JS_IsException(list))
        return list;
    if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
    if (c->pythonpath && c->pythonpath[0]) {
        const char *s = c->pythonpath;
        for (;;) {
            const char *e = strchr(s, ':');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n == 0)
                snprintf(entry, sizeof(entry), "%s", cwd);
            else if (s[0] == '/')
                snprintf(entry, sizeof(entry), "%.*s", (int)n, s);
            else
                snprintf(entry, sizeof(entry), "%s/%.*s", cwd, (int)n, s);
            if (qp_sys_list_append_cstr(ctx, list, entry) < 0)
                goto fail;
            if (!e)
                break;
            s = e + 1;
        }
    }
    if (p->has_stdlib && qp_sys_list_append_cstr(ctx, list, p->stdlib) < 0)
        goto fail;
    return list;
fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* sys._xoptions from "-X name[=value]" strings. */
static JSValue qp_sys_make_xoptions(JSContext *ctx)
{
    QPConfig *c = &QP_STATE(ctx)->config;
    JSValue d = qp_dict_new(ctx);
    int i;

    if (JS_IsException(d))
        return d;
    for (i = 0; i < c->nxoptions; i++) {
        const char *opt = c->xoptions[i];
        const char *eq = strchr(opt, '=');
        JSValue k, v;
        int r;
        if (eq) {
            k = qp_str_decode(ctx, opt, (Py_ssize_t)(eq - opt), "utf-8", "surrogateescape");
            v = qp_sys_fs_str(ctx, eq + 1);
        } else {
            k = qp_sys_fs_str(ctx, opt);
            v = qp_bool_new(TRUE);
        }
        if (JS_IsException(k) || JS_IsException(v)) {
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, d);
            return JS_EXCEPTION;
        }
        r = qp_dict_set_item(ctx, d, k, v);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        if (r < 0) {
            JS_FreeValue(ctx, d);
            return JS_EXCEPTION;
        }
    }
    return d;
}

static JSValue qp_sys_make_version_info(JSContext *ctx)
{
    JSValue items[5];
    items[0] = JS_NewInt32(ctx, 3);
    items[1] = JS_NewInt32(ctx, 15);
    items[2] = JS_NewInt32(ctx, 2);
    items[3] = qp_str_from_cstr(ctx, "final");
    items[4] = JS_NewInt32(ctx, 0);
    if (JS_IsException(items[3]))
        return JS_EXCEPTION;
    return qp_sys_ss_make(ctx, QP_SYS_SS_VERSION_INFO, items);
}

static JSValue qp_sys_make_impl_info(JSContext *ctx, JSValueConst version_info)
{
    static const char *const names[] = {
        "name", "cache_tag", "version", "hexversion",
#ifdef QP_MULTIARCH
        "_multiarch",
#endif
        "supports_isolated_interpreters",
    };
    JSValue vals[countof(names)];
    int i = 0;

    vals[i++] = qp_str_from_cstr(ctx, "quickpy");
    vals[i++] = qp_str_from_cstr(ctx, QP_CACHE_TAG); /* __pycache__/<name>.quickpy-315.pyc */
    vals[i++] = JS_DupValue(ctx, version_info);
    vals[i++] = JS_NewInt32(ctx, QP_PY_HEXVERSION);
#ifdef QP_MULTIARCH
    vals[i++] = qp_str_from_cstr(ctx, QP_MULTIARCH);
#endif
    vals[i++] = qp_bool_new(FALSE); /* PEP 734 subinterpreters are not supported */
    return qp_namespace_from_pairs(ctx, i, names, vals);
}

static JSValue qp_sys_make_abi_info(JSContext *ctx)
{
    static const char *const names[] = { "pointer_bits", "free_threaded", "debug", "byteorder" };
    JSValue vals[4];
    uint16_t probe = 1;

    vals[0] = JS_NewInt32(ctx, (int)sizeof(void *) * 8);
    vals[1] = qp_bool_new(FALSE);
    vals[2] = qp_bool_new(FALSE);
    vals[3] = qp_str_from_cstr(ctx, *(uint8_t *)&probe ? "little" : "big");
    return qp_namespace_from_pairs(ctx, 4, names, vals);
}

static JSValue qp_sys_make_float_info(JSContext *ctx)
{
    JSValue items[11];
    items[0] = qp_float_new(ctx, DBL_MAX);
    items[1] = JS_NewInt32(ctx, DBL_MAX_EXP);
    items[2] = JS_NewInt32(ctx, DBL_MAX_10_EXP);
    items[3] = qp_float_new(ctx, DBL_MIN);
    items[4] = JS_NewInt32(ctx, DBL_MIN_EXP);
    items[5] = JS_NewInt32(ctx, DBL_MIN_10_EXP);
    items[6] = JS_NewInt32(ctx, DBL_DIG);
    items[7] = JS_NewInt32(ctx, DBL_MANT_DIG);
    items[8] = qp_float_new(ctx, DBL_EPSILON);
    items[9] = JS_NewInt32(ctx, FLT_RADIX);
    items[10] = JS_NewInt32(ctx, 1);  /* FLT_ROUNDS: round to nearest */
    return qp_sys_ss_make(ctx, QP_SYS_SS_FLOAT_INFO, items);
}

static JSValue qp_sys_make_int_info(JSContext *ctx)
{
    JSValue items[4];
    items[0] = JS_NewInt32(ctx, 30);
    items[1] = JS_NewInt32(ctx, 4);
    items[2] = JS_NewInt32(ctx, QP_LONG_DEFAULT_MAX_STR_DIGITS);
    items[3] = JS_NewInt32(ctx, QP_LONG_MAX_STR_DIGITS_THRESHOLD);
    return qp_sys_ss_make(ctx, QP_SYS_SS_INT_INFO, items);
}

/* Consistent with numfmt.h hashing (SipHash-1-3, modulus 2**61-1). */
static JSValue qp_sys_make_hash_info(JSContext *ctx)
{
    JSValue items[9];
    items[0] = JS_NewInt32(ctx, 64);
    items[1] = qp_long_from_int64(ctx, (int64_t)QP_HASH_MODULUS);
    items[2] = JS_NewInt32(ctx, QP_HASH_INF);
    items[3] = JS_NewInt32(ctx, 0);
    items[4] = JS_NewInt32(ctx, (int32_t)QP_HASH_IMAG);
    items[5] = qp_str_from_cstr(ctx, "siphash13");
    items[6] = JS_NewInt32(ctx, 64);
    items[7] = JS_NewInt32(ctx, 128);
    items[8] = JS_NewInt32(ctx, 0);
    if (JS_IsException(items[1]) || JS_IsException(items[5])) {
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[5]);
        return JS_EXCEPTION;
    }
    return qp_sys_ss_make(ctx, QP_SYS_SS_HASH_INFO, items);
}

static JSValue qp_sys_make_thread_info(JSContext *ctx)
{
    JSValue items[3];
    items[0] = qp_str_from_cstr(ctx, "pthread");
#if defined(__APPLE__)
    items[1] = qp_str_from_cstr(ctx, "mutex+cond");
    items[2] = JS_UNDEFINED;
#else
    items[1] = qp_str_from_cstr(ctx, "semaphore");
#if defined(_CS_GNU_LIBPTHREAD_VERSION)
    {
        char buffer[255];
        size_t len = confstr(_CS_GNU_LIBPTHREAD_VERSION, buffer, sizeof(buffer));
        if (len > 1 && len < sizeof(buffer))
            items[2] = qp_str_from_cstr(ctx, buffer);
        else
            items[2] = JS_UNDEFINED;
    }
#else
    items[2] = JS_UNDEFINED;
#endif
#endif
    if (JS_IsException(items[0]) || JS_IsException(items[1]) || JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        JS_FreeValue(ctx, items[1]);
        JS_FreeValue(ctx, items[2]);
        return JS_EXCEPTION;
    }
    return qp_sys_ss_make(ctx, QP_SYS_SS_THREAD_INFO, items);
}

/* Create sys.stdin / stdout / stderr (CPython create_stdio rules). */
static int qp_sys_init_stdio(JSContext *ctx, JSValueConst d)
{
    QPConfig *c = &QP_STATE(ctx)->config;
    char encoding[64] = "utf-8", errors[64];
    int buffered = !c->unbuffered;
    int out_flags, err_flags, in_flags;
    JSValue in, out, err;

    snprintf(errors, sizeof(errors), "%s", c->utf8_mode ? "surrogateescape" : "strict");
    if (c->stdio_encoding && c->stdio_encoding[0]) {
        const char *colon = strchr(c->stdio_encoding, ':');
        size_t n = colon ? (size_t)(colon - c->stdio_encoding) : strlen(c->stdio_encoding);
        if (n > 0 && n < sizeof(encoding)) {
            memcpy(encoding, c->stdio_encoding, n);
            encoding[n] = '\0';
        }
        if (colon && colon[1])
            snprintf(errors, sizeof(errors), "%s", colon + 1);
    }
    in_flags = (buffered && isatty(0)) ? QP_SPF_LINE_BUFFERING : 0;
    out_flags = buffered ? (isatty(1) ? QP_SPF_LINE_BUFFERING : 0) : QP_SPF_WRITE_THROUGH;
    err_flags = buffered ? QP_SPF_LINE_BUFFERING : QP_SPF_WRITE_THROUGH;
    in = qp_stdprinter_new(ctx, 0, "<stdin>", 1, encoding, errors, in_flags);
    if (JS_IsException(in))
        return -1;
    out = qp_stdprinter_new(ctx, 1, "<stdout>", 0, encoding, errors, out_flags);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, in);
        return -1;
    }
    /* stderr: same encoding, always backslashreplace */
    err = qp_stdprinter_new(ctx, 2, "<stderr>", 0, encoding, "backslashreplace", err_flags);
    if (JS_IsException(err)) {
        JS_FreeValue(ctx, in);
        JS_FreeValue(ctx, out);
        return -1;
    }
    if (qp_sys_dict_set(ctx, d, "__stdin__", JS_DupValue(ctx, in)) < 0 ||
        qp_sys_dict_set(ctx, d, "__stdout__", JS_DupValue(ctx, out)) < 0 ||
        qp_sys_dict_set(ctx, d, "__stderr__", JS_DupValue(ctx, err)) < 0) {
        JS_FreeValue(ctx, in);
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, err);
        return -1;
    }
    if (qp_sys_dict_set(ctx, d, "stdin", in) < 0 ||
        qp_sys_dict_set(ctx, d, "stdout", out) < 0 ||
        qp_sys_dict_set(ctx, d, "stderr", err) < 0)
        return -1;
    return 0;
}

static const char qp_sys_doc[] =
    "This module provides access to some objects used or maintained by the\n"
    "interpreter and to functions that interact strongly with the interpreter.\n"
    "\n"
    "Dynamic objects:\n"
    "\n"
    "argv -- command line arguments; argv[0] is the script pathname if known\n"
    "path -- module search path; path[0] is the script directory, else ''\n"
    "modules -- dictionary of loaded modules\n"
    "\n"
    "displayhook -- called to show results in an interactive session\n"
    "excepthook -- called to handle any uncaught exception other than SystemExit\n"
    "  To customize printing in an interactive session or to install a custom\n"
    "  top-level exception handler, assign other functions to replace these.\n"
    "\n"
    "stdin -- standard input file object; used by input()\n"
    "stdout -- standard output file object; used by print()\n"
    "stderr -- standard error object; used for error messages\n"
    "  By assigning other file objects (or objects that behave like files)\n"
    "  to these, it is possible to redirect all of the interpreter's I/O.\n"
    "\n"
    "last_exc - the last uncaught exception\n"
    "  Only available in an interactive session after a\n"
    "  traceback has been printed.\n"
    "\n"
    "Static objects:\n"
    "\n"
    "builtin_module_names -- tuple of module names built into this interpreter\n"
    "copyright -- copyright notice pertaining to this interpreter\n"
    "exec_prefix -- prefix used to find the machine-specific Python library\n"
    "executable -- absolute path of the executable binary of the Python interpreter\n"
    "float_info -- a named tuple with information about the float implementation.\n"
    "float_repr_style -- string indicating the style of repr() output for floats\n"
    "hash_info -- a named tuple with information about the hash algorithm.\n"
    "hexversion -- version information encoded as a single integer\n"
    "implementation -- Python implementation information.\n"
    "int_info -- a named tuple with information about the int implementation.\n"
    "maxsize -- the largest supported length of containers.\n"
    "maxunicode -- the value of the largest Unicode code point\n"
    "platform -- platform identifier\n"
    "prefix -- prefix used to find the Python library\n"
    "thread_info -- a named tuple with information about the thread implementation.\n"
    "version -- the version of this interpreter as a string\n"
    "version_info -- version information as a named tuple\n"
    "__stdin__ -- the original stdin; don't touch!\n"
    "__stdout__ -- the original stdout; don't touch!\n"
    "__stderr__ -- the original stderr; don't touch!\n"
    "__displayhook__ -- the original displayhook; don't touch!\n"
    "__excepthook__ -- the original excepthook; don't touch!\n";

static const char qp_sys_copyright[] =
    "Copyright (c) 2001 Python Software Foundation.\n"
    "All Rights Reserved.\n"
    "\n"
    "Copyright (c) 2000 BeOpen.com.\n"
    "All Rights Reserved.\n"
    "\n"
    "Copyright (c) 1995-2001 Corporation for National Research Initiatives.\n"
    "All Rights Reserved.\n"
    "\n"
    "Copyright (c) 1991-1995 Stichting Mathematisch Centrum, Amsterdam.\n"
    "All Rights Reserved.";

/* sys.builtin_module_names: sorted names of the inittab. */
static JSValue qp_sys_builtin_module_names(JSContext *ctx)
{
    JSValue list, res;
    int i, n = qp_inittab_count();

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return list;
    for (i = 0; i < n; i++) {
        if (qp_sys_list_append_cstr(ctx, list, qp_inittab_get(i)->name) < 0)
            goto fail;
    }
    if (qp_list_sort(ctx, list, QP_NULL, 0) < 0)
        goto fail;
    res = qp_list_as_tuple(ctx, list);
    JS_FreeValue(ctx, list);
    return res;
fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* sys._jit: there is no JIT compiler (CPython builds without
   --enable-experimental-jit). */
QP_SYS_FN(qp_sys_jit_false)
{
    return qp_bool_new(FALSE);
}

static const QPMethodDef qp_sys_jit_methods[] = {
    { "is_available", qp_sys_jit_false, QP_METH_NOARGS, 0,
      "is_available($module, /)\n--\n\nReturn True if the current Python executable supports JIT compilation, and False otherwise." },
    { "is_enabled", qp_sys_jit_false, QP_METH_NOARGS, 0,
      "is_enabled($module, /)\n--\n\nReturn True if JIT compilation is enabled for the current Python process (implies sys._jit.is_available()), and False otherwise." },
    { "is_active", qp_sys_jit_false, QP_METH_NOARGS, 0,
      "is_active($module, /)\n--\n\nReturn True if the topmost Python frame is currently executing JIT code (implies sys._jit.is_enabled()), and False otherwise." },
    { NULL },
};

static const QPModuleDef qp_moddef_sys__jit = {
    "sys._jit",
    "Utilities for observing just-in-time compilation.\n\n"
    "WARNING: This API is an implementation detail and may change between "
    "Python releases.",
    qp_sys_jit_methods,
    NULL,
};

/* sys.monitoring (python/instrumentation.c) */
static JSValue qp_monitoring_create(JSContext *ctx);

/* The sys attributes that come from the configuration of the process:
   executable, the command line, the -X/-W options, sys.flags (only when
   'flags', the population sets it with the other struct sequences) and
   the bytecode options.  Part of qp_sys_populate, and applied again to a
   state restored from the base snapshot of qpy (pythonrun.c). */
static int qp_sys_set_config_attrs(JSContext *ctx, JSValueConst d, BOOL flags)
{
    QPConfig *c = &QP_STATE(ctx)->config;
    const char *exe = c->executable ? c->executable : "";
    char exe_abs[PATH_MAX];
    JSValue v;

    if (exe[0] && exe[0] != '/' && strchr(exe, '/') && realpath(exe, exe_abs))
        exe = exe_abs;
    if (qp_sys_dict_set(ctx, d, "executable", qp_sys_fs_str(ctx, exe)) < 0 ||
        qp_sys_dict_set(ctx, d, "_base_executable", qp_sys_fs_str(ctx, exe)) < 0 ||
        qp_sys_dict_set(ctx, d, "pycache_prefix",
                        c->pycache_prefix ? qp_sys_fs_str(ctx, c->pycache_prefix) : JS_UNDEFINED) < 0)
        return -1;
    if (flags && qp_sys_dict_set(ctx, d, "flags", qp_sys_make_flags(ctx)) < 0)
        return -1;

    /* command line */
    if (c->argc > 0)
        v = qp_sys_str_list(ctx, c->argc, c->argv);
    else {
        char empty[1] = "";
        char *one[1] = { empty };
        v = qp_sys_str_list(ctx, 1, one);
    }
    if (qp_sys_dict_set(ctx, d, "argv", v) < 0 ||
        qp_sys_dict_set(ctx, d, "orig_argv", qp_sys_str_list(ctx, c->orig_argc, c->orig_argv)) < 0 ||
        qp_sys_dict_set(ctx, d, "warnoptions", qp_sys_str_list(ctx, c->nwarnoptions, c->warnoptions)) < 0 ||
        qp_sys_dict_set(ctx, d, "_xoptions", qp_sys_make_xoptions(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "dont_write_bytecode", qp_bool_new(c->dont_write_bytecode != 0)) < 0)
        return -1;
    return 0;
}

/* Fill the sys module dict (functions are already there: added from
   qp_moddef_sys.methods by qp_module_from_def). */
static int qp_sys_populate(JSContext *ctx, JSValueConst d)
{
    QPState *st = QP_STATE(ctx);
    QPSysPaths paths;
    JSValue version_info;
    uint16_t probe = 1;

    /* hooks and their originals */
#define QP_SYS_ALIAS(dst, src)                                                 \
    do {                                                                       \
        JSValue _v = qp_sys_get(ctx, QP_ATOM(ctx, src));                       \
        if (QP_IS_NULL(_v) || qp_sys_dict_set(ctx, d, dst, _v) < 0)            \
            return -1;                                                         \
    } while (0)
    QP_SYS_ALIAS("__displayhook__", displayhook);
    QP_SYS_ALIAS("__excepthook__", excepthook);
    QP_SYS_ALIAS("__unraisablehook__", unraisablehook);
    QP_SYS_ALIAS("__breakpointhook__", breakpointhook);
#undef QP_SYS_ALIAS

    if (qp_sys_init_stdio(ctx, d) < 0)
        return -1;

    /* version */
    version_info = qp_sys_make_version_info(ctx);
    if (JS_IsException(version_info))
        return -1;
    if (qp_sys_dict_set(ctx, d, "version_info", JS_DupValue(ctx, version_info)) < 0 ||
        qp_sys_dict_set(ctx, d, "implementation", qp_sys_make_impl_info(ctx, version_info)) < 0) {
        JS_FreeValue(ctx, version_info);
        return -1;
    }
    JS_FreeValue(ctx, version_info);
    if (qp_sys_dict_set(ctx, d, "version",
                        qp_str_from_cstr(ctx, QP_PY_VERSION " (main, " __DATE__ ", " __TIME__ ") " QP_COMPILER)) < 0 ||
        qp_sys_dict_set(ctx, d, "hexversion", JS_NewInt32(ctx, QP_PY_HEXVERSION)) < 0 ||
        qp_sys_dict_set(ctx, d, "api_version", JS_NewInt32(ctx, QP_PY_API_VERSION)) < 0 ||
        qp_sys_dict_set(ctx, d, "copyright", qp_str_from_cstr(ctx, qp_sys_copyright)) < 0 ||
        qp_sys_dict_set(ctx, d, "platform", qp_str_from_cstr(ctx, QP_PLATFORM)) < 0 ||
        qp_sys_dict_set(ctx, d, "maxsize", qp_long_from_int64(ctx, PY_SSIZE_T_MAX)) < 0 ||
        qp_sys_dict_set(ctx, d, "float_info", qp_sys_make_float_info(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "int_info", qp_sys_make_int_info(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "hash_info", qp_sys_make_hash_info(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "maxunicode", JS_NewInt32(ctx, 0x10FFFF)) < 0 ||
        qp_sys_dict_set(ctx, d, "builtin_module_names", qp_sys_builtin_module_names(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "stdlib_module_names", qp_sys_stdlib_module_names(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "byteorder", qp_str_from_cstr(ctx, *(uint8_t *)&probe ? "little" : "big")) < 0 ||
        qp_sys_dict_set(ctx, d, "abiflags", qp_str_from_cstr(ctx, "")) < 0 ||
        qp_sys_dict_set(ctx, d, "abi_info", qp_sys_make_abi_info(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "float_repr_style", qp_str_from_cstr(ctx, "short")) < 0 ||
        qp_sys_dict_set(ctx, d, "thread_info", qp_sys_make_thread_info(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "flags", qp_sys_make_flags(ctx)) < 0)
        return -1;
    {
        static const char *const git[] = { "quickpy", "", "" };
        if (qp_sys_dict_set(ctx, d, "_git", qp_sys_new_str_tuple(ctx, 3, git)) < 0)
            return -1;
    }
#ifdef __APPLE__
    /* _PYTHONFRAMEWORK: empty for a non-framework build */
    if (qp_sys_dict_set(ctx, d, "_framework", qp_str_from_cstr(ctx, "")) < 0)
        return -1;
#endif

    if (qp_sys_dict_set(ctx, d, "_jit", qp_module_from_def(ctx, &qp_moddef_sys__jit)) < 0 ||
        qp_sys_dict_set(ctx, d, "monitoring", qp_monitoring_create(ctx)) < 0)
        return -1;

    /* paths */
    qp_sys_compute_paths(ctx, &paths);
    if (qp_sys_dict_set(ctx, d, "prefix", qp_sys_fs_str(ctx, paths.prefix)) < 0 ||
        qp_sys_dict_set(ctx, d, "base_prefix", qp_sys_fs_str(ctx, paths.prefix)) < 0 ||
        qp_sys_dict_set(ctx, d, "exec_prefix", qp_sys_fs_str(ctx, paths.prefix)) < 0 ||
        qp_sys_dict_set(ctx, d, "base_exec_prefix", qp_sys_fs_str(ctx, paths.prefix)) < 0 ||
        qp_sys_dict_set(ctx, d, "platlibdir", qp_str_from_cstr(ctx, "lib")) < 0 ||
        qp_sys_dict_set(ctx, d, "_stdlib_dir",
                        paths.has_stdlib ? qp_sys_fs_str(ctx, paths.stdlib) : JS_UNDEFINED) < 0 ||
        qp_sys_dict_set(ctx, d, "path", qp_sys_make_path(ctx, &paths)) < 0 ||
        qp_sys_dict_set(ctx, d, "meta_path", qp_list_new(ctx, 0)) < 0 ||
        qp_sys_dict_set(ctx, d, "path_hooks", qp_list_new(ctx, 0)) < 0 ||
        qp_sys_dict_set(ctx, d, "path_importer_cache", qp_dict_new(ctx)) < 0 ||
        qp_sys_dict_set(ctx, d, "modules", JS_DupValue(ctx, st->modules)) < 0)
        return -1;

    return qp_sys_set_config_attrs(ctx, d, FALSE);
}

/* ========================================================================= */
/* Init                                                                       */
/* ========================================================================= */
/* QPModuleDef exec of sys: publishes the module in QPState first (the
   population reads sys attributes back through qp_sys_get). */
static int qp_sys_exec(JSContext *ctx, JSValueConst module)
{
    QPState *st = QP_STATE(ctx);
    JSValue d = JS_DupValue(ctx, qp_module_get_dict(module));

    JS_FreeValue(ctx, st->sys_module);
    JS_FreeValue(ctx, st->sys_dict);
    st->sys_module = JS_DupValue(ctx, module);
    st->sys_dict = d;
    return qp_sys_populate(ctx, d);
}

static const QPModuleDef qp_moddef_sys = {
    "sys",
    qp_sys_doc,
    qp_sys_methods,
    qp_sys_exec,
};


static int qp_init_sys(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSValue module;

    /* k types that are not builtins */
    if (qp_type_ready(ctx, QP_TYPE(ctx, SimpleNamespace), &qp_namespace_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, StdPrinter), &qp_stdprinter_spec) < 0)
        return -1;
    if (qp_sys_init_structseq_types(ctx) < 0)
        return -1;
    if (qp_init_genericalias(ctx) < 0 || qp_init_typevar(ctx) < 0)
        return -1;

    /* CPython's interpreter default (pycore_runtime_init.h) */
    st->dlopenflags = RTLD_NOW;

    if (JS_VALUE_GET_TAG(st->audit_hooks) != JS_TAG_OBJECT) {
        JSValue l = qp_list_new(ctx, 0);
        if (JS_IsException(l))
            return -1;
        st->audit_hooks = l;
    }

    /* the sys module (qp_sys_exec stores it in QPState) */
    module = qp_module_from_def(ctx, &qp_moddef_sys);
    if (JS_IsException(module))
        return -1;

    /* sys.modules entries */
    if (qp_sys_dict_set(ctx, st->modules, "sys", module) < 0)
        return -1;
    if (JS_VALUE_GET_TAG(st->builtins_module) == JS_TAG_OBJECT &&
        qp_sys_dict_set(ctx, st->modules, "builtins", JS_DupValue(ctx, st->builtins_module)) < 0)
        return -1;
    return 0;
}
