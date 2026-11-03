/*
 * The `_imp` builtin module: low-level import machinery used by
 * importlib._bootstrap and importlib._bootstrap_external.
 *
 * Derived from CPython 3.15 Python/import.c (the `_imp` functions,
 * is_builtin, create_builtin, exec_builtin_or_dynamic, update_compiled_module)
 * and Python/clinic/import.c.h (PSF-2.0).  The frozen modules are
 * python/frozen.c.
 *
 * Differences that follow from this interpreter's configuration, each
 * behaving exactly like CPython built the same way:
 *  - Dynamic loading exists only with the C API (CONFIG_PYTHON_CAPI,
 *    python/capi_dynload.c) and only for the stable ABI: create_dynamic /
 *    exec_dynamic load `*.abi3.so` / `*.abi3t.so`, which are all that
 *    extension_suffixes() lists.  Without the C API (HAVE_DYNAMIC_LOADING
 *    undefined) both are absent and extension_suffixes() is [], so
 *    importlib installs no ExtensionFileLoader
 *    (_bootstrap_external._get_supported_file_loaders tests
 *    hasattr(_imp, 'create_dynamic')).
 *  - One interpreter, one thread (M5 brings threads): the import lock is a
 *    recursion counter in QPState; release_lock() without the lock raises
 *    CPython's RuntimeError.  The interpreter is the main interpreter, so
 *    _override_multi_interp_extensions_check() raises its RuntimeError.
 *  - Native modules are created and executed in one step by
 *    qp_module_from_def, so exec_builtin() finds them initialized and
 *    returns 0 like CPython's exec_builtin_or_dynamic for an initialized
 *    module.
 *  - Bytecode caches hold QuickJS bytecode (marshal of code objects, see
 *    python/mod_misc.c): pyc_magic_number_token is this build's own
 *    (qp_pyc_magic_number_token, python/codeobject.c), never CPython's,
 *    and sys.implementation.cache_tag is "quickpy-315", so importlib
 *    reads and writes __pycache__/<name>.quickpy-315.pyc like CPython
 *    does its own.  check_hash_based_pycs is --check-hash-based-pycs
 *    ("default" unless given).
 */

/* Argument Clinic "unicode" converter: "%s() argument[ N] must be str, not %T". */
static int qp_imp_check_str(JSContext *ctx, const char *fname, const char *argname,
                            JSValueConst v)
{
    if (qp_str_check(ctx, v))
        return 0;
    qp_raise_type_error(ctx, "%s() %s must be str, not %T", fname, argname, v);
    return -1;
}

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
    qp_rmutex_lock(ctx, QP_RMUTEX_IMPORT);
    return JS_UNDEFINED;
}

/* release_lock() */
static JSValue qp_imp_release_lock(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    if (qp_rmutex_unlock(ctx, QP_RMUTEX_IMPORT) < 0)
        return qp_raise_runtime_error(ctx, "not holding the import lock");
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
       (importlib._bootstrap._load_unlocked), which then calls
       exec_builtin() to run its exec hook */
    return qp_module_create_from_def(ctx, def, FALSE);
}

/* extension_suffixes() */
static JSValue qp_imp_extension_suffixes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
#ifdef CONFIG_PYTHON_CAPI
    return qp_capi_imp_extension_suffixes(ctx);
#else
    return qp_list_new(ctx, 0);
#endif
}

/* init_frozen(name, /) */
static JSValue qp_imp_init_frozen(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    int ret;

    if (qp_imp_check_str(ctx, "init_frozen", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    ret = qp_import_frozen_module(ctx, args[0]);
    if (ret < 0)
        return JS_EXCEPTION;
    if (ret == 0)
        return JS_UNDEFINED;
    return qp_frozen_add_module(ctx, args[0]);
}

/* find_frozen(name, /, *, withdata=False) */
static JSValue qp_imp_find_frozen(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "", "withdata", NULL };
    static const QPArgSpec spec = { "find_frozen", kwlist, 1, 1, 0 };
    JSValueConst a[2];
    JSValue items[3];
    QPFrozenInfo info;
    QPFrozenStatus status;
    int withdata = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_imp_check_str(ctx, "find_frozen", "argument 1", a[0]) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[1]) && qp_arg_bool(ctx, a[1], &withdata) < 0)
        return JS_EXCEPTION;
    status = qp_frozen_find(ctx, a[0], &info);
    if (status == QP_FROZEN_NOT_FOUND || status == QP_FROZEN_BAD_NAME)
        return JS_UNDEFINED;
    if (status != QP_FROZEN_OKAY)
        return qp_frozen_set_error(ctx, status, a[0]);
    items[0] = qp_none();
    if (withdata) {
        /* PyMemoryView_FromMemory(data, size, PyBUF_READ) */
        JSValue mbuf = qp_mbuf_raw(ctx, (void *)info.data, (Py_ssize_t)info.size, 1, 1, "B");
        if (JS_IsException(mbuf))
            return mbuf;
        items[0] = qp_mv_from_mbuf(ctx, mbuf);
        if (JS_IsException(items[0]))
            return items[0];
    }
    items[1] = qp_bool_new(info.is_package);
    items[2] = info.has_origname && info.origname[0] ?
        qp_str_from_cstr(ctx, info.origname) : qp_none();
    if (JS_IsException(items[2])) {
        JS_FreeValue(ctx, items[0]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 3, items);
}

/* get_frozen_object(name, data=None, /) */
static JSValue qp_imp_get_frozen_object(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst name, dataobj;
    JSValue code;
    QPFrozenInfo info;
    QPFrozenStatus status;
    QPBuffer buf;
    BOOL have_buf = FALSE;

    if (qp_arg_check_positional(ctx, "get_frozen_object", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    name = args[0];
    if (qp_imp_check_str(ctx, "get_frozen_object", "argument 1", name) < 0)
        return JS_EXCEPTION;
    dataobj = nargs > 1 ? args[1] : qp_none();
    memset(&info, 0, sizeof(info));
    info.nameobj = QP_NULL;
    if (!JS_IsUndefined(dataobj) && !QP_IS_NULL(dataobj)) {
        if (qp_buffer_get(ctx, dataobj, &buf, QP_BUF_SIMPLE) < 0) {
            /* PyObject_CheckBuffer failed: _PyArg_BadArgument */
            if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
                return JS_EXCEPTION;
            JS_FreeValue(ctx, JS_GetException(ctx));
            return qp_raise_type_error(ctx,
                                       "get_frozen_object() argument 2 must be bytes, not %T",
                                       dataobj);
        }
        have_buf = TRUE;
        info.data = buf.buf;
        info.size = (size_t)buf.len;
    } else {
        status = qp_frozen_find(ctx, name, &info);
        if (status != QP_FROZEN_OKAY)
            return qp_frozen_set_error(ctx, status, name);
    }
    if (QP_IS_NULL(info.nameobj))
        info.nameobj = name;
    if (info.size == 0) {
        /* Does not contain executable code. */
        if (have_buf)
            qp_buffer_release(ctx, &buf);
        return qp_frozen_set_error(ctx, QP_FROZEN_INVALID, name);
    }
    code = qp_frozen_unmarshal(ctx, info.data, info.size, info.nameobj);
    if (have_buf)
        qp_buffer_release(ctx, &buf);
    return code;
}

/* is_frozen_package(name, /) */
static JSValue qp_imp_is_frozen_package(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    QPFrozenInfo info;
    QPFrozenStatus status;

    if (qp_imp_check_str(ctx, "is_frozen_package", "argument", args[0]) < 0)
        return JS_EXCEPTION;
    status = qp_frozen_find(ctx, args[0], &info);
    if (status != QP_FROZEN_OKAY)
        return qp_frozen_set_error(ctx, status, args[0]);
    return qp_bool_new(info.is_package);
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
    return qp_bool_new(qp_frozen_find(ctx, args[0], NULL) == QP_FROZEN_OKAY);
}

/* _frozen_module_names() */
static JSValue qp_imp_frozen_module_names(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    return qp_frozen_module_names(ctx);
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

/* exec_builtin(mod, /) -> int: exec_builtin_or_dynamic, the exec hook of a
   module from create_builtin() (0 for non-modules and initialized modules
   alike) */
static JSValue qp_imp_exec_builtin(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    if (qp_module_exec_pending(ctx, args[0]) < 0)
        return JS_EXCEPTION;
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
   from sys.lazy_modules (_PyImport_DiscardLazyModule on
   QPState.lazy_modules).  The pending-submodule declarations
   (_PyImport_ClearLazySubmodule) are not recorded (python/import.c). */
static JSValue qp_imp_set_lazy_attributes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "_set_lazy_attributes", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_imp_check_str(ctx, "_set_lazy_attributes", "argument 2", args[1]) < 0)
        return JS_EXCEPTION;
    if (qp_import_discard_lazy_module(ctx, args[1]) < 0)
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
#ifdef CONFIG_PYTHON_CAPI
    { "create_dynamic", qp_capi_imp_create_dynamic, QP_METH_FASTCALL, 0,
      "create_dynamic($module, spec, file=<unrepresentable>, /)\n--\n\n"
      "Create an extension module." },
    { "exec_dynamic", qp_capi_imp_exec_dynamic, QP_METH_O, 0,
      "exec_dynamic($module, mod, /)\n--\n\n"
      "Initialize an extension module." },
#endif
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

    v = qp_str_from_cstr(ctx, QP_STATE(ctx)->config.check_hash_based_pycs ?
                         QP_STATE(ctx)->config.check_hash_based_pycs : "default");
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
                              qp_long_from_int64(ctx, qp_pyc_magic_number_token()));
    JS_FreeAtom(ctx, atom);
    return r;
}

static const QPModuleDef qp_moddef__imp = {
    "_imp",
    "(Extremely) low-level import machinery bits as used by importlib.",
    qp_imp_methods,
    qp_imp_exec,
};
