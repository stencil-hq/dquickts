/*
 * dquickts Python C API (plan D5 Tier B, M5; Tier S, M7): dynamic loading of
 * stable-ABI extension modules (`*.abi3.so`, `*.abi3t.so` and their
 * platform-tagged forms `*.abi3-<multiarch>.so`, `*.abi3t-<multiarch>.so`)
 * and of source builds against capi/include (`*.quickpy-315-<multiarch>.so`,
 * QP_SOABI) for `_imp.create_dynamic` and `_imp.exec_dynamic`.
 *
 * Ported from CPython 3.15 Python/import.c (_imp_create_dynamic_impl,
 * import_run_extension, import_run_modexport, exec_builtin_or_dynamic, the
 * single-phase extensions cache), Python/importdl.c (get_encoded_name,
 * _PyImport_GetModuleExportHooks, _PyImport_RunModInitFunc and its error
 * messages) and Python/dynload_shlib.c (PSF-2.0).
 *
 * The module's hooks are looked up in this order: PEP 793
 * `PyModExport_<name>` (slots array -> PyModule_FromSlotsAndSpec), then
 * `PyInit_<name>` (a PyModuleDef -> multi-phase, a module -> single-phase).
 * Non-ASCII names use the `PyModExportU_`/`PyInitU_` punycode forms.
 *
 * ABI policy (plan D5, non-goal "version-specific cp315 binaries"):
 *  - only file names ending in one of qp_capi_ext_suffixes are loaded; any
 *    other file (notably CPython's `.cpython-315-<multiarch>.so`) raises
 *    ImportError naming its ABI tag before dlopen.  The import system finds
 *    such files through a rejecting ExtensionFileLoader entry that
 *    qp_capi_install_ext_reject() appends after the source and bytecode
 *    loaders (so a pure-Python fallback of the same name wins);
 *  - a stable-ABI file whose Py_mod_abi slot does not declare
 *    PyABIInfo_STABLE is rejected before it is created
 *    (qp_capi_dynload_check_abi).  Tier S files (the QP_SOABI suffix, which
 *    sysconfig reports as EXT_SUFFIX, so meson-python and setuptools
 *    produce it) were compiled against capi/include and may use the full
 *    non-limited API: their Py_mod_abi is not checked.
 *
 * dlopen flags: sys.getdlopenflags() (QPState.dlopenflags, RTLD_NOW by
 * default, sys.setdlopenflags() changes it), passed unchanged as in
 * CPython's _PyImport_FindSharedFuncptr.
 * Extensions resolve the C API from the qpy executable (-export_dynamic /
 * -rdynamic, Makefile).  Handles are never closed, as in CPython.
 */

#include <dlfcn.h>

/* _PyImport_DynLoadFiletab of a GIL build: "." SOABI ".so" (Tier S), the
   stable ABI with and without the platform tag, then abi3t.  No untagged
   ".so": such a file may have been built for CPython. */
static const char *const qp_capi_ext_suffixes[] = {
    "." QP_SOABI ".so",   /* index 0: Tier S */
#ifdef QP_MULTIARCH
    ".abi3-" QP_MULTIARCH ".so",
#endif
    ".abi3.so",
#ifdef QP_MULTIARCH
    ".abi3t-" QP_MULTIARCH ".so",
#endif
    ".abi3t.so",
    NULL
};

/* _imp.extension_suffixes() */
static JSValue qp_capi_imp_extension_suffixes(JSContext *ctx)
{
    JSValue list = qp_list_new(ctx, 0), s;
    int i;

    if (JS_IsException(list))
        return list;
    for (i = 0; qp_capi_ext_suffixes[i]; i++) {
        s = qp_str_from_cstr(ctx, qp_capi_ext_suffixes[i]);
        if (JS_IsException(s) || qp_list_append(ctx, list, s) < 0) {
            JS_FreeValue(ctx, s);
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, s);
    }
    return list;
}

/* Called from the importlib bootstrap after _install_external_importers:
   rebuild the FileFinder path hook with one more ExtensionFileLoader entry,
   after the source and bytecode loaders, for CPython's version-specific
   suffixes (the GIL and free-threaded ones), so that importing such a
   module raises create_dynamic's ImportError instead of
   ModuleNotFoundError.  importlib.machinery.EXTENSION_SUFFIXES stays the
   loadable suffixes only. */
static const char qp_capi_ext_reject_src[] =
    "import sys, _imp\n"
    "from _frozen_importlib_external import (FileFinder, ExtensionFileLoader,\n"
    "                                        _get_supported_file_loaders)\n"
    "_v = '%d%d' % sys.version_info[:2]\n"
    "_m = getattr(sys.implementation, '_multiarch', sys.platform)\n"
    "_loaders = _get_supported_file_loaders()\n"
    "_loaders.append((ExtensionFileLoader, ['.cpython-%s-%s.so' % (_v, _m),\n"
    "                                       '.cpython-%st-%s.so' % (_v, _m)]))\n"
    "sys.path_hooks[-1] = FileFinder.path_hook(*_loaders)\n";

static int qp_capi_install_ext_reject(JSContext *ctx)
{
    JSValue g = qp_dict_new(ctx), r, name;
    int rc;

    if (JS_IsException(g))
        return -1;
    name = qp_str_from_cstr(ctx, "_qp_ext_reject");
    rc = JS_IsException(name) ? -1 : qp_dict_set_item_atom(ctx, g, QP_ATOM(ctx, __name__), name);
    JS_FreeValue(ctx, name);
    if (rc < 0) {
        JS_FreeValue(ctx, g);
        return -1;
    }
    r = qp_run_string(ctx, qp_capi_ext_reject_src, sizeof(qp_capi_ext_reject_src) - 1,
                      "<frozen importlib._bootstrap_external>", 257 /* Py_file_input */, g, g);
    JS_FreeValue(ctx, g);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Single-phase extensions cache (CPython _extensions_cache, main interpreter) */
/* -------------------------------------------------------------------------- */

typedef struct QPCapiExtCache {
    struct QPCapiExtCache *next;
    char *path;
    char *name;
    PyModuleDef *def;
    PyObject *(*m_init)(void);
    PyObject *m_copy;       /* m_size == -1: dict copy (immortal: process lifetime) */
} QPCapiExtCache;

static QPCapiExtCache *qp_capi_ext_cache;

/* _extensions_cache_clear_all (Py_Finalize): the cached dict copies are
   objects of the runtime that goes away; the next runtime runs the init
   functions again (the libraries stay loaded). */
static void qp_capi_ext_cache_clear(void)
{
    QPCapiExtCache *e, *next;
    for (e = qp_capi_ext_cache; e; e = next) {
        next = e->next;
        qp_sys_free(e->path);
        qp_sys_free(e->name);
        qp_sys_free(e);
    }
    qp_capi_ext_cache = NULL;
}

static QPCapiExtCache *qp_capi_ext_cache_find(const char *path, const char *name)
{
    QPCapiExtCache *e;
    for (e = qp_capi_ext_cache; e; e = e->next) {
        if (!strcmp(e->path, path) && !strcmp(e->name, name))
            return e;
    }
    return NULL;
}

static char *qp_capi_ext_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = qp_sys_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int qp_capi_ext_cache_set(const char *path, const char *name, PyModuleDef *def,
                                 PyObject *(*m_init)(void), PyObject *m_copy)
{
    QPCapiExtCache *e = qp_capi_ext_cache_find(path, name);
    if (!e) {
        e = qp_sys_calloc(1, sizeof(*e));
        if (!e) {
            Py_XDECREF(m_copy);
            PyErr_NoMemory();
            return -1;
        }
        e->path = qp_capi_ext_strdup(path);
        e->name = qp_capi_ext_strdup(name);
        if (!e->path || !e->name) {
            qp_sys_free(e->path);
            qp_sys_free(e->name);
            qp_sys_free(e);
            Py_XDECREF(m_copy);
            PyErr_NoMemory();
            return -1;
        }
        e->next = qp_capi_ext_cache;
        qp_capi_ext_cache = e;
    }
    e->def = def;
    e->m_init = m_init;
    Py_XSETREF(e->m_copy, m_copy);
    return 0;
}

/* import_find_extension + reload_singlephase_extension: a single-phase
   module loaded before from the same file.  m_size == -1: a new module
   object initialized from the cached dict copy (the init function is not
   called again); otherwise NULL and *m_init is set, so the caller runs the
   init function again.  NULL without exception when not cached. */
static PyObject *qp_capi_ext_cache_reload(QPCapiExtCache *e, PyObject *name)
{
    QPCapiModuleObject *m;

    if (e->def->m_size != -1 || !e->m_copy)
        return NULL;
    m = (QPCapiModuleObject *)PyModule_NewObject(name);
    if (!m)
        return NULL;
    if (PyDict_Update(m->md_dict, e->m_copy) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    m->md_token = e->def;
    m->md_token_is_def = true;
    qp_capi_module_copy_deflike(m, e->def);
    return (PyObject *)m;
}

/* -------------------------------------------------------------------------- */
/* Loader                                                                     */
/* -------------------------------------------------------------------------- */

typedef PyObject *(*qp_capi_modinit_t)(void);
typedef PySlot *(*qp_capi_modexport_t)(void);

/* Name of the ABI a non-loadable extension file name carries: the text
   between the module name and ".so" ("cpython-315-darwin"), or the whole
   suffix when there is no tag. */
static PyObject *qp_capi_ext_reject(PyObject *name, PyObject *path, const char *cpath)
{
    const char *base = strrchr(cpath, '/'), *dot, *end;
    PyObject *msg;

    base = base ? base + 1 : cpath;
    dot = strchr(base, '.');
    end = strrchr(base, '.');
    if (dot && end && end > dot && !strcmp(end, ".so"))
        msg = PyUnicode_FromFormat(
            "%U: cannot load extension module %U built for the unsupported ABI '%.*s': "
            "quickpy loads only stable-ABI (abi3/abi3t) extension modules and source builds "
            "against its own headers (" QP_SOABI ", importlib.machinery.EXTENSION_SUFFIXES); "
            "install an abi3 build of the package or build it from source",
            path, name, (int)(end - dot - 1), dot + 1);
    else
        msg = PyUnicode_FromFormat(
            "%U: cannot load extension module %U with an untagged or unknown ABI (suffix '%s'): "
            "quickpy loads only stable-ABI (abi3/abi3t) extension modules and source builds "
            "against its own headers (" QP_SOABI ", importlib.machinery.EXTENSION_SUFFIXES)",
            path, name, dot ? dot : "");
    if (msg) {
        PyErr_SetImportError(msg, name, path);
        Py_DECREF(msg);
    }
    return NULL;
}

/* get_encoded_name: the hook name of the last component of `name`, ASCII
   or punycode with '-' replaced by '_'.  New bytes reference; *nonascii set. */
static PyObject *qp_capi_ext_encoded_name(PyObject *name, int *nonascii)
{
    PyObject *tmp, *encoded, *res;
    Py_ssize_t len = PyUnicode_GetLength(name), lastdot;

    if (len < 0)
        return NULL;
    lastdot = PyUnicode_FindChar(name, '.', 0, len, -1);
    if (lastdot < -1)
        return NULL;
    if (lastdot >= 0) {
        tmp = PyUnicode_Substring(name, lastdot + 1, len);
        if (!tmp)
            return NULL;
    } else {
        tmp = Py_NewRef(name);
    }
    *nonascii = 0;
    encoded = PyUnicode_AsEncodedString(tmp, "ascii", NULL);
    if (!encoded) {
        if (!PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
            Py_DECREF(tmp);
            return NULL;
        }
        PyErr_Clear();
        encoded = PyUnicode_AsEncodedString(tmp, "punycode", NULL);
        if (!encoded) {
            Py_DECREF(tmp);
            return NULL;
        }
        *nonascii = 1;
    }
    Py_DECREF(tmp);
    res = PyObject_CallMethod(encoded, "replace", "cc", '-', '_');
    Py_DECREF(encoded);
    return res;
}

/* Py_mod_abi of a dynamically loaded module must declare the stable ABI:
   the PyABIInfo of a version-specific build (no PyABIInfo_STABLE) names
   CPython's object layouts, which this runtime does not have.  Malformed
   slot arrays are left to the module creation, which reports them. */
static int qp_capi_dynload_check_abi(const void *slots, int legacy, const char *name)
{
    QPCapiSlotIter it;

    if (!slots)
        return 0;
    if (legacy)
        qp_capi_slotiter_init_legacy(&it, slots, QP_CAPI_SLOT_KIND_MOD);
    else
        qp_capi_slotiter_init(&it, slots, QP_CAPI_SLOT_KIND_MOD);
    it.name = name;
    while (qp_capi_slotiter_next(&it)) {
        const PyABIInfo *info;
        if (it.current.sl_id == Py_slot_invalid) {
            PyErr_Clear();
            return 0;
        }
        if (it.current.sl_id != Py_mod_abi)
            continue;
        info = it.current.sl_ptr;
        if (info && info->abiinfo_major_version >= 1 && !(info->flags & PyABIInfo_STABLE)) {
            PyErr_Format(PyExc_ImportError,
                         "%s: extension module built for the version-specific CPython ABI "
                         "(%d.%d, no PyABIInfo_STABLE); quickpy loads only stable-ABI "
                         "(abi3/abi3t) extension modules",
                         name, (int)((info->abi_version >> 24) & 0xff),
                         (int)((info->abi_version >> 16) & 0xff));
            return -1;
        }
    }
    return 0;
}

/* _PyImport_RunModInitFunc + import_run_extension for a loaded PyInit_*
   hook.  New reference or NULL. */
static PyObject *qp_capi_run_modinit(qp_capi_modinit_t p0, PyObject *spec, PyObject *name,
                                     PyObject *path, const char *cname, const char *cpath,
                                     int nonascii, int tier_s)
{
    const char *oldcontext;
    PyObject *m, *res, *m_copy;
    PyModuleDef *def;

    /* the package context resolves PyModule_Create's short m_name to the
       full name (capi_module.c, _PyImport_ResolveNameWithPackageContext) */
    oldcontext = qp_capi_package_context;
    qp_capi_package_context = cname;
    m = p0();
    qp_capi_package_context = oldcontext;

    if (!m) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_SystemError, "initialization of %s failed without raising an exception",
                         cname);
        return NULL;
    }
    if (PyErr_Occurred()) {
        /* CPython does not release m here either */
        _PyErr_FormatFromCause(PyExc_SystemError, "initialization of %s raised unreported exception",
                               cname);
        return NULL;
    }
    if (Py_IS_TYPE(m, NULL)) {
        PyErr_Format(PyExc_SystemError, "init function of %s returned uninitialized object", cname);
        return NULL;
    }
    if (PyObject_TypeCheck(m, &PyModuleDef_Type)) {
        /* multi-phase init: create now, exec_dynamic executes it */
        def = (PyModuleDef *)m;
        PyModuleDef_Init(def);
        if (!tier_s && qp_capi_dynload_check_abi(def->m_slots, 1, cname) < 0)
            return NULL;
        res = PyModule_FromDefAndSpec2(def, spec, PYTHON_API_VERSION);
        return res;
    }
    if (nonascii) {
        Py_DECREF(m);
        PyErr_Format(PyExc_SystemError, "initialization of %s did not return PyModuleDef", cname);
        return NULL;
    }
    /* single-phase init (legacy) */
    if (!PyModule_Check(m)) {
        Py_DECREF(m);
        PyErr_Format(PyExc_SystemError, "initialization of %s did not return an extension module",
                     cname);
        return NULL;
    }
    def = PyModule_GetDef(m);
    if (!def) {
        PyErr_Clear();
        Py_DECREF(m);
        PyErr_Format(PyExc_SystemError, "initialization of %s did not return a valid extension module",
                     cname);
        return NULL;
    }
    /* _PyImport_FixupExtensionObject: __file__, the extensions cache and
       sys.modules[name] */
    if (PyModule_AddObjectRef(m, "__file__", path) < 0) {
        PyErr_Clear();  /* CPython ignores the failure */
    }
    m_copy = NULL;
    if (def->m_size == -1) {
        m_copy = PyDict_Copy(PyModule_GetDict(m));
        if (!m_copy) {
            Py_DECREF(m);
            return NULL;
        }
    }
    if (qp_capi_ext_cache_set(cpath, cname, def, p0, m_copy) < 0 ||
        PyDict_SetItem(PyImport_GetModuleDict(), name, m) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}

/* import_run_modexport.  New reference or NULL. */
static PyObject *qp_capi_run_modexport(qp_capi_modexport_t ex0, PyObject *spec, PyObject *name,
                                       int tier_s)
{
    PySlot *slots = ex0();
    PyObject *res;

    if (!slots) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_SystemError,
                         "module export hook for module %R failed without setting an exception", name);
        return NULL;
    }
    if (PyErr_Occurred()) {
        PyErr_Format(PyExc_SystemError,
                     "module export hook for module %R raised unreported exception", name);
        return NULL;
    }
    if (!tier_s && qp_capi_dynload_check_abi(slots, 0, PyUnicode_AsUTF8(name)) < 0)
        return NULL;
    res = PyModule_FromSlotsAndSpec(slots, spec);
    if (res && PyModule_Check(res)) {
        QPCapiModuleObject *mod = (QPCapiModuleObject *)res;
        if (!mod->md_token)
            mod->md_token = slots;
    }
    return res;
}

/* _imp_create_dynamic_impl.  New reference or NULL. */
static PyObject *qp_capi_create_dynamic(PyObject *spec)
{
    PyObject *name = NULL, *path = NULL, *encoded = NULL, *mod = NULL;
    const char *cname, *cpath, *ename;
    char funcname[256];
    QPCapiExtCache *cached;
    qp_capi_modinit_t p0 = NULL;
    qp_capi_modexport_t ex0 = NULL;
    void *handle;
    int i, nonascii = 0, ok;
    size_t plen;

    name = PyObject_GetAttrString(spec, "name");
    if (!name)
        return NULL;
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "spec.name must be a string");
        goto done;
    }
    path = PyObject_GetAttrString(spec, "origin");
    if (!path)
        goto done;
    if (!PyUnicode_Check(path)) {
        PyErr_Format(PyExc_TypeError, "spec.origin must be a string, not %T", path);
        goto done;
    }
    cname = PyUnicode_AsUTF8(name);
    cpath = PyUnicode_AsUTF8(path);
    if (!cname || !cpath)
        goto done;

    /* import_find_extension */
    cached = qp_capi_ext_cache_find(cpath, cname);
    if (cached) {
        mod = qp_capi_ext_cache_reload(cached, name);
        if (mod || PyErr_Occurred())
            goto done;
    }

    /* only Tier S and the stable ABI are loadable */
    ok = 0;
    plen = strlen(cpath);
    for (i = 0; qp_capi_ext_suffixes[i]; i++) {
        size_t sl = strlen(qp_capi_ext_suffixes[i]);
        if (plen > sl && !strcmp(cpath + plen - sl, qp_capi_ext_suffixes[i])) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        qp_capi_ext_reject(name, path, cpath);
        goto done;
    }

    encoded = qp_capi_ext_encoded_name(name, &nonascii);
    if (!encoded)
        goto done;
    ename = PyBytes_AsString(encoded);
    if (!ename)
        goto done;

    if (PySys_Audit("import", "OOOOO", name, path, Py_None, Py_None, Py_None) < 0)
        goto done;
    handle = dlopen(cpath, __atomic_load_n(&QP_STATE(QP_CAPI_CTX())->dlopenflags, __ATOMIC_RELAXED));
    if (!handle) {
        const char *err = dlerror();
        PyObject *msg = PyUnicode_DecodeFSDefault(err ? err : "dlopen() failed");
        if (msg) {
            PyErr_SetImportError(msg, name, path);
            Py_DECREF(msg);
        }
        goto done;
    }
    snprintf(funcname, sizeof(funcname), "%s_%.200s", nonascii ? "PyModExportU" : "PyModExport",
             ename);
    ex0 = (qp_capi_modexport_t)dlsym(handle, funcname);
    if (!ex0) {
        snprintf(funcname, sizeof(funcname), "%s_%.200s", nonascii ? "PyInitU" : "PyInit", ename);
        p0 = (qp_capi_modinit_t)dlsym(handle, funcname);
    }
    if (!ex0 && !p0) {
        PyObject *msg = PyUnicode_FromFormat(
            "dynamic module does not define module export function (%s_%s or %s_%s)",
            nonascii ? "PyModExportU" : "PyModExport", ename, nonascii ? "PyInitU" : "PyInit", ename);
        if (msg) {
            PyErr_SetImportError(msg, name, path);
            Py_DECREF(msg);
        }
        goto done;
    }
    if (ex0)
        mod = qp_capi_run_modexport(ex0, spec, name, i == 0);
    else
        mod = qp_capi_run_modinit(p0, spec, name, path, cname, cpath, nonascii, i == 0);
done:
    Py_XDECREF(encoded);
    Py_XDECREF(path);
    Py_XDECREF(name);
    return mod;
}

/* _imp.create_dynamic(spec, file=None, /) */
static JSValue qp_capi_imp_create_dynamic(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    PyObject *spec, *mod;
    JSValue r;

    if (qp_arg_check_positional(ctx, "create_dynamic", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_capi_init(ctx) < 0)
        return JS_EXCEPTION;
    spec = qp_capi_new_ref(ctx, args[0]);
    if (!spec)
        return JS_EXCEPTION;
    mod = qp_capi_create_dynamic(spec);
    Py_DECREF(spec);
    if (!mod)
        return JS_EXCEPTION;
    r = qp_capi_to_js(ctx, mod);
    Py_DECREF(mod);
    return r;
}

/* _imp.exec_dynamic(mod, /) -> int: exec_builtin_or_dynamic */
static JSValue qp_capi_imp_exec_dynamic(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    PyObject *mod;
    int r = 0;

    if (!qp_module_check(ctx, args[0]) || !qp_capi.ctx)
        return JS_NewInt32(ctx, 0);
    mod = qp_capi_new_ref(ctx, args[0]);
    if (!mod)
        return JS_EXCEPTION;
    if (!PyModule_GetState(mod))
        r = PyModule_Exec(mod);
    Py_DECREF(mod);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, r);
}
