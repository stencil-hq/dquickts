/*
 * The `_imp` builtin module: low-level import machinery used by
 * importlib._bootstrap and importlib._bootstrap_external.
 *
 * Derived from CPython 3.15 Python/import.c (the `_imp` functions,
 * is_builtin, create_builtin, exec_builtin_or_dynamic, set_frozen_error,
 * update_compiled_module) and Python/clinic/import.c.h (PSF-2.0).
 *
 * Differences that follow from this interpreter's configuration, each
 * behaving exactly like CPython built the same way:
 *  - No frozen modules exist (the importlib bootstrap modules are compiled
 *    from pylib at startup, see importlib_boot.c): is_frozen() is False,
 *    find_frozen() is None, init_frozen() is None, _frozen_module_names()
 *    is [], and get_frozen_object() / is_frozen_package() raise CPython's
 *    ImportError "No such frozen object named %R".
 *  - No dynamic loading (HAVE_DYNAMIC_LOADING undefined, until the C-API
 *    extension loader exists): create_dynamic/exec_dynamic are absent and
 *    extension_suffixes() is [], so importlib installs no
 *    ExtensionFileLoader (_bootstrap_external._get_supported_file_loaders
 *    tests hasattr(_imp, 'create_dynamic')).
 *  - One interpreter, one thread (M5 brings threads): the import lock is a
 *    recursion counter in QPState; release_lock() without the lock raises
 *    CPython's RuntimeError.  The interpreter is the main interpreter, so
 *    _override_multi_interp_extensions_check() raises its RuntimeError.
 *  - Native modules are created and executed in one step by
 *    qp_module_from_def, so exec_builtin() finds them initialized and
 *    returns 0 like CPython's exec_builtin_or_dynamic for an initialized
 *    module.
 *  - check_hash_based_pycs is 'never': QuickJS bytecode is not CPython
 *    .pyc, sys.implementation.cache_tag is None, so importlib never reads
 *    or writes bytecode caches.
 */

/* Include/internal/pycore_magic_number.h: PYC_MAGIC_NUMBER 3666 */
#define QP_PYC_MAGIC_NUMBER 3666
#define QP_PYC_MAGIC_NUMBER_TOKEN \
    ((uint32_t)QP_PYC_MAGIC_NUMBER | ((uint32_t)'\r' << 16) | ((uint32_t)'\n' << 24))

/* Argument Clinic "unicode" converter: "%s() argument[ N] must be str, not %T". */
static int qp_imp_check_str(JSContext *ctx, const char *fname, const char *argname,
                            JSValueConst v)
{
    if (qp_str_check(ctx, v))
        return 0;
    qp_raise_type_error(ctx, "%s() %s must be str, not %T", fname, argname, v);
    return -1;
}

/* set_frozen_error: ImportError(fmt % name, name=name). */
static JSValue qp_imp_frozen_error(JSContext *ctx, const char *fmt, JSValueConst name)
{
    JSValue msg, exc;

    msg = qp_str_from_format(ctx, fmt, name);
    if (JS_IsException(msg))
        return msg;
    exc = qp_exc_new(ctx, QP_EXC(ctx, ImportError), 1, (JSValueConst *)&msg);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(exc))
        return exc;
    if (qp_setattr(ctx, exc, QP_ATOM(ctx, name), name) < 0) {
        JS_FreeValue(ctx, exc);
        return JS_EXCEPTION;
    }
    return qp_raise_object(ctx, exc);
}

#define QP_IMP_FROZEN_NOT_FOUND "No such frozen object named %R"
#define QP_IMP_FROZEN_INVALID   "Frozen object named %R is invalid"

/* is_builtin(): 1 for an inittab module, -1 for the core modules that
   cannot be re-initialized (CPython's NULL initfunc entries "sys" and
   "builtins"), 0 otherwise.  -2 on error. */
static int qp_imp_is_builtin_name(JSContext *ctx, JSValueConst name)
{
    JSValue s;
    const char *u;
    size_t len;
    int r = 0;

    s = qp_str_value(ctx, name);
    if (JS_IsException(s))
        return -2;
    u = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (!u) {
        /* a name with lone surrogates matches no inittab entry */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (strlen(u) == len && qp_inittab_find(u, len)) {
        if (!strcmp(u, "sys") || !strcmp(u, "builtins"))
            r = -1;
        else
            r = 1;
    }
    JS_FreeCString(ctx, u);
    return r;
}

/* lock_held() */
static JSValue qp_imp_lock_held(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(QP_STATE(ctx)->import_lock_level > 0);
}

/* acquire_lock() */
static JSValue qp_imp_acquire_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QP_STATE(ctx)->import_lock_level++;
    return JS_UNDEFINED;
}

/* release_lock() */
static JSValue qp_imp_release_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    QPState *s = QP_STATE(ctx);

    if (s->import_lock_level <= 0)
        return qp_raise_runtime_error(ctx, "not holding the import lock");
    s->import_lock_level--;
    return JS_UNDEFINED;
}

/* update_code_filenames: co_filename of code and of the nested code
   constants that share the old file name. */
static void qp_imp_update_code_filenames(JSContext *ctx, JSFunctionBytecode *b,
                                         JSAtom oldname, JSAtom newname)
{
    int i;

    if (!b->has_debug || b->debug.filename != oldname)
        return;
    b->debug.filename = JS_DupAtom(ctx, newname);
    JS_FreeAtom(ctx, oldname);
    for (i = 0; i < b->cpool_count; i++) {
        JSValueConst c = b->cpool[i];
        if (JS_VALUE_GET_TAG(c) == JS_TAG_FUNCTION_BYTECODE) {
            JSFunctionBytecode *cb = JS_VALUE_GET_PTR(c);
            if (cb->is_py)
                qp_imp_update_code_filenames(ctx, cb, oldname, newname);
        }
    }
}

/* _fix_co_filename(code, path, /): update_compiled_module.  The file name
   lives in the bytecode, so every code object sharing it sees the change,
   as in CPython where the code object itself is mutated. */
static JSValue qp_imp_fix_co_filename(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    JSFunctionBytecode *b;
    JSValue path;
    JSAtom oldname, newname;

    if (qp_arg_check_positional(ctx, "_fix_co_filename", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_typecheck(ctx, args[0], QP_TYPE(ctx, code)))
        return qp_raise_type_error(ctx, "_fix_co_filename() argument 1 must be code, not %T",
                                   args[0]);
    if (qp_imp_check_str(ctx, "_fix_co_filename", "argument 2", args[1]) < 0)
        return JS_EXCEPTION;
    b = qp_code_bytecode(args[0]);
    if (!b || !b->has_debug || b->debug.filename == JS_ATOM_NULL)
        return JS_UNDEFINED;
    path = qp_str_value(ctx, args[1]);
    if (JS_IsException(path))
        return path;
    newname = qp_str_to_atom(ctx, path);
    JS_FreeValue(ctx, path);
    if (newname == JS_ATOM_NULL)
        return JS_EXCEPTION;
    oldname = JS_DupAtom(ctx, b->debug.filename);
    if (oldname != newname)
        qp_imp_update_code_filenames(ctx, b, oldname, newname);
    JS_FreeAtom(ctx, oldname);
    JS_FreeAtom(ctx, newname);
    return JS_UNDEFINED;
}

/* create_builtin(spec, /) */
static JSValue qp_imp_create_builtin(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const QPModuleDef *def = NULL;
    JSValue name, s, msg, exc;
    const char *u;
    size_t len;

    name = qp_getattr(ctx, args[0], QP_ATOM(ctx, name));
    if (JS_IsException(name))
        return name;
    if (!qp_str_check(ctx, name)) {
        QPType *t = qp_type_of(ctx, name);
        JSValue tname = qp_getattr(ctx, QP_TYPE_VALUE(t), QP_ATOM(ctx, __name__));
        JS_FreeValue(ctx, name);
        if (JS_IsException(tname))
            return tname;
        qp_raise_type_error(ctx, "name must be string, not %.200S", tname);
        JS_FreeValue(ctx, tname);
        return JS_EXCEPTION;
    }
    if (qp_str_len(ctx, name) == 0) {
        JS_FreeValue(ctx, name);
        return qp_raise_value_error(ctx, "name must not be empty");
    }
    s = qp_str_value(ctx, name);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, name);
        return s;
    }
    u = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (u) {
        if (strlen(u) == len)
            def = qp_inittab_find(u, len);
        JS_FreeCString(ctx, u);
    } else {
        /* lone surrogates: no inittab entry has such a name */
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (!def) {
        /* _PyErr_SetModuleNotFoundError */
        msg = qp_str_from_format(ctx, "%S module not found", name);
        if (JS_IsException(msg)) {
            JS_FreeValue(ctx, name);
            return msg;
        }
        exc = qp_exc_new(ctx, QP_EXC(ctx, ModuleNotFoundError), 1, (JSValueConst *)&msg);
        JS_FreeValue(ctx, msg);
        if (JS_IsException(exc) || qp_setattr(ctx, exc, QP_ATOM(ctx, name), name) < 0) {
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, name);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, name);
        return qp_raise_object(ctx, exc);
    }
    JS_FreeValue(ctx, name);
    /* "sys" and "builtins" are returned as they are (import_add_module of
       a core module); every other native module is multi-phase in CPython
       terms: a new module per call, sys.modules is left to the caller
       (importlib._bootstrap._load_unlocked). */
    return qp_module_from_def(ctx, def);
}

/* extension_suffixes() */
static JSValue qp_imp_extension_suffixes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    return qp_list_new(ctx, 0);
}

/* init_frozen(name, /): PyImport_ImportFrozenModuleObject finds nothing. */
static JSValue qp_imp_init_frozen(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    if (qp_imp_check_str(ctx, "init_frozen", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* find_frozen(name, /, *, withdata=False) */
static JSValue qp_imp_find_frozen(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "", "withdata", NULL };
    static const QPArgSpec spec = { "find_frozen", kwlist, 1, 1, 0 };
    JSValueConst a[2];
    int withdata;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_imp_check_str(ctx, "find_frozen", "argument 1", a[0]) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_bool(ctx, a[1], &withdata) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* get_frozen_object(name, data=None, /) */
static JSValue qp_imp_get_frozen_object(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst name, dataobj;
    JSValue marshal, code;
    QPBuffer buf;
    Py_ssize_t size;
    JSAtom loads;

    if (qp_arg_check_positional(ctx, "get_frozen_object", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    name = args[0];
    if (qp_imp_check_str(ctx, "get_frozen_object", "argument 1", name) < 0)
        return JS_EXCEPTION;
    dataobj = nargs > 1 ? args[1] : JS_UNDEFINED;
    if (JS_IsUndefined(dataobj) || JS_IsNull(dataobj))
        return qp_imp_frozen_error(ctx, QP_IMP_FROZEN_NOT_FOUND, name);
    if (qp_buffer_get(ctx, dataobj, &buf, QP_BUF_SIMPLE) < 0) {
        /* PyObject_CheckBuffer failed: _PyArg_BadArgument */
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            return JS_EXCEPTION;
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qp_raise_type_error(ctx, "get_frozen_object() argument 2 must be bytes, not %T",
                                   dataobj);
    }
    size = buf.len;
    qp_buffer_release(ctx, &buf);
    if (size == 0)   /* does not contain executable code */
        return qp_imp_frozen_error(ctx, QP_IMP_FROZEN_INVALID, name);
    /* unmarshal_frozen_code: PyMarshal_ReadObjectFromString(data) */
    marshal = qp_import_module(ctx, "marshal");
    if (JS_IsException(marshal))
        return marshal;
    loads = JS_NewAtom(ctx, "loads");
    if (loads == JS_ATOM_NULL) {
        JS_FreeValue(ctx, marshal);
        return JS_EXCEPTION;
    }
    code = qp_call_method(ctx, marshal, loads, &dataobj, 1, QP_NULL);
    JS_FreeAtom(ctx, loads);
    JS_FreeValue(ctx, marshal);
    if (JS_IsException(code)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return qp_imp_frozen_error(ctx, QP_IMP_FROZEN_INVALID, name);
    }
    if (!qp_typecheck(ctx, code, QP_TYPE(ctx, code))) {
        JS_FreeValue(ctx, code);
        return qp_raise_type_error(ctx, "frozen object %R is not a code object", name);
    }
    return code;
}

/* is_frozen_package(name, /) */
static JSValue qp_imp_is_frozen_package(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    if (qp_imp_check_str(ctx, "is_frozen_package", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    return qp_imp_frozen_error(ctx, QP_IMP_FROZEN_NOT_FOUND, args[0]);
}

/* is_builtin(name, /) */
static JSValue qp_imp_is_builtin(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    int r;

    if (qp_imp_check_str(ctx, "is_builtin", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    r = qp_imp_is_builtin_name(ctx, args[0]);
    if (r == -2)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, r);
}

/* is_frozen(name, /) */
static JSValue qp_imp_is_frozen(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (qp_imp_check_str(ctx, "is_frozen", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    return qp_bool_new(FALSE);
}

/* _frozen_module_names() */
static JSValue qp_imp_frozen_module_names(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    return qp_list_new(ctx, 0);
}

/* _override_frozen_modules_for_tests(override, /) */
static JSValue qp_imp_override_frozen_modules_for_tests(JSContext *ctx, JSValueConst self,
                                                        JSValueConst *args, int nargs,
                                                        JSValueConst kwnames, int magic)
{
    int override;

    if (qp_arg_int(ctx, args[0], &override) < 0)
        return JS_EXCEPTION;
    QP_STATE(ctx)->override_frozen_modules = override;
    return JS_UNDEFINED;
}

/* _override_multi_interp_extensions_check(override, /): only usable in a
   subinterpreter; this interpreter is always the main one. */
static JSValue qp_imp_override_multi_interp_extensions_check(JSContext *ctx, JSValueConst self,
                                                             JSValueConst *args, int nargs,
                                                             JSValueConst kwnames, int magic)
{
    int override;

    if (qp_arg_int(ctx, args[0], &override) < 0)
        return JS_EXCEPTION;
    return qp_raise_runtime_error(ctx, "_imp._override_multi_interp_extensions_check() "
                                  "cannot be used in the main interpreter");
}

/* exec_builtin(mod, /) -> int: qp_module_from_def already ran the exec
   hook, so the module is initialized (exec_builtin_or_dynamic returns 0
   for non-modules and initialized modules alike). */
static JSValue qp_imp_exec_builtin(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return JS_NewInt32(ctx, 0);
}

/* source_hash(key, source): _Py_KeyedHash(key, source) as 8 little-endian
   bytes. */
static JSValue qp_imp_source_hash(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "key", "source", NULL };
    static const QPArgSpec spec = { "source_hash", kwlist, 2, 2, 0 };
    JSValueConst a[2];
    JSValue key_obj;
    int64_t key;
    QPBuffer buf;
    uint64_t h;
    uint8_t out[8];
    int i, r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    /* "long" converter: PyLong_AsLong */
    key_obj = qp_index(ctx, a[0]);
    if (JS_IsException(key_obj))
        return key_obj;
    r = qp_long_as_int64(ctx, key_obj, &key);
    JS_FreeValue(ctx, key_obj);
    if (r < 0)
        return JS_EXCEPTION;
#if LONG_MAX < INT64_MAX
    if (key > LONG_MAX || key < LONG_MIN)
        return qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
#endif
    if (qp_buffer_get(ctx, a[1], &buf, QP_BUF_SIMPLE) < 0)
        return JS_EXCEPTION;
    h = qp_keyed_hash((uint64_t)key, buf.buf, (size_t)buf.len);
    qp_buffer_release(ctx, &buf);
    for (i = 0; i < 8; i++)
        out[i] = (uint8_t)(h >> (8 * i));
    return qp_bytes_new(ctx, out, 8);
}

/* _set_lazy_attributes(modobj, name, /): remove the resolved module name
   from sys.lazy_modules (_PyImport_DiscardLazyModule) and release its
   pending-submodule declaration (_PyImport_ClearLazySubmodule).  Both
   registries are absent until lazy imports record them (CPython returns
   0 when LAZY_MODULES / LAZY_PENDING_SUBMODULES are NULL); the
   sys.lazy_modules set is used when present. */
static JSValue qp_imp_set_lazy_attributes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    JSValue lazy;
    JSAtom atom;
    int r;

    if (qp_arg_check_positional(ctx, "_set_lazy_attributes", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_imp_check_str(ctx, "_set_lazy_attributes", "argument 2", args[1]) < 0)
        return JS_EXCEPTION;
    atom = JS_NewAtom(ctx, "lazy_modules");
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    lazy = qp_sys_get(ctx, atom);
    JS_FreeAtom(ctx, atom);
    if (QP_IS_NULL(lazy))
        return JS_UNDEFINED;
    if (JS_IsException(lazy))
        return lazy;
    r = 0;
    if (qp_set_check(ctx, lazy))
        r = qp_set_discard(ctx, lazy, args[1]);
    JS_FreeValue(ctx, lazy);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const QPMethodDef qp_imp_methods[] = {
    { "extension_suffixes", qp_imp_extension_suffixes, QP_METH_NOARGS, 0,
      "extension_suffixes($module, /)\n--\n\n"
      "Returns the list of file suffixes used to identify extension modules." },
    { "lock_held", qp_imp_lock_held, QP_METH_NOARGS, 0,
      "lock_held($module, /)\n--\n\n"
      "Return True if the import lock is currently held, else False.\n\n"
      "On platforms without threads, return False." },
    { "acquire_lock", qp_imp_acquire_lock, QP_METH_NOARGS, 0,
      "acquire_lock($module, /)\n--\n\n"
      "Acquires the interpreter's import lock for the current thread.\n\n"
      "This lock should be used by import hooks to ensure thread-safety when\n"
      "importing modules.  On platforms without threads, this function does\n"
      "nothing." },
    { "release_lock", qp_imp_release_lock, QP_METH_NOARGS, 0,
      "release_lock($module, /)\n--\n\n"
      "Release the interpreter's import lock.\n\n"
      "On platforms without threads, this function does nothing." },
    { "find_frozen", qp_imp_find_frozen, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "find_frozen($module, name, /, *, withdata=False)\n--\n\n"
      "Return info about the corresponding frozen module (if there is one) or None.\n\n"
      "The returned info (a 2-tuple):\n\n"
      " * data         the raw marshalled bytes\n"
      " * is_package   whether or not it is a package\n"
      " * origname     the originally frozen module's name, or None if not\n"
      "                a stdlib module (this will usually be the same as\n"
      "                the module's current name)" },
    { "get_frozen_object", qp_imp_get_frozen_object, QP_METH_FASTCALL, 0,
      "get_frozen_object($module, name, data=None, /)\n--\n\n"
      "Create a code object for a frozen module." },
    { "is_frozen_package", qp_imp_is_frozen_package, QP_METH_O, 0,
      "is_frozen_package($module, name, /)\n--\n\n"
      "Returns True if the module name is of a frozen package." },
    { "create_builtin", qp_imp_create_builtin, QP_METH_O, 0,
      "create_builtin($module, spec, /)\n--\n\n"
      "Create an extension module." },
    { "init_frozen", qp_imp_init_frozen, QP_METH_O, 0,
      "init_frozen($module, name, /)\n--\n\n"
      "Initializes a frozen module." },
    { "is_builtin", qp_imp_is_builtin, QP_METH_O, 0,
      "is_builtin($module, name, /)\n--\n\n"
      "Returns True if the module name corresponds to a built-in module." },
    { "is_frozen", qp_imp_is_frozen, QP_METH_O, 0,
      "is_frozen($module, name, /)\n--\n\n"
      "Returns True if the module name corresponds to a frozen module." },
    { "_frozen_module_names", qp_imp_frozen_module_names, QP_METH_NOARGS, 0,
      "_frozen_module_names($module, /)\n--\n\n"
      "Returns the list of available frozen modules." },
    { "_override_frozen_modules_for_tests", qp_imp_override_frozen_modules_for_tests,
      QP_METH_O, 0,
      "_override_frozen_modules_for_tests($module, override, /)\n--\n\n"
      "(internal-only) Override PyConfig.use_frozen_modules.\n\n"
      "(-1: \"off\", 1: \"on\", 0: no override)\n"
      "See frozen_modules() in Lib/test/support/import_helper.py." },
    { "_override_multi_interp_extensions_check",
      qp_imp_override_multi_interp_extensions_check, QP_METH_O, 0,
      "_override_multi_interp_extensions_check($module, override, /)\n--\n\n"
      "(internal-only) Override PyInterpreterConfig.check_multi_interp_extensions.\n\n"
      "(-1: \"never\", 1: \"always\", 0: no override)" },
    { "exec_builtin", qp_imp_exec_builtin, QP_METH_O, 0,
      "exec_builtin($module, mod, /)\n--\n\n"
      "Initialize a built-in module." },
    { "_fix_co_filename", qp_imp_fix_co_filename, QP_METH_FASTCALL, 0,
      "_fix_co_filename($module, code, path, /)\n--\n\n"
      "Changes code.co_filename to specify the passed-in file path.\n\n"
      "  code\n"
      "    Code object to change.\n"
      "  path\n"
      "    File path to use." },
    { "source_hash", qp_imp_source_hash, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "source_hash($module, /, key, source)\n--\n\n" },
    { "_set_lazy_attributes", qp_imp_set_lazy_attributes, QP_METH_FASTCALL, 0,
      "_set_lazy_attributes($module, modobj, name, /)\n--\n\n"
      "Remove the resolved module name from sys.lazy_modules." },
    { NULL },
};

/* imp_module_exec */
static int qp_imp_exec(JSContext *ctx, JSValueConst module)
{
    JSValue d = qp_module_get_dict(module), v;
    JSAtom atom;
    int r;

    v = qp_str_from_cstr(ctx, "never");
    if (JS_IsException(v))
        return -1;
    atom = JS_NewAtom(ctx, "check_hash_based_pycs");
    r = atom == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, d, atom, v);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, v);
    if (r < 0)
        return -1;
    atom = JS_NewAtom(ctx, "pyc_magic_number_token");
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_dict_set_item_atom(ctx, d, atom,
                              JS_NewInt32(ctx, (int32_t)QP_PYC_MAGIC_NUMBER_TOKEN));
    JS_FreeAtom(ctx, atom);
    return r;
}

static const QPModuleDef qp_moddef__imp = {
    "_imp",
    "(Extremely) low-level import machinery bits as used by importlib.",
    qp_imp_methods,
    qp_imp_exec,
};
