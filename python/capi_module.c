/*
 * dquickts Python C API (plan D5, unit B): module objects, PyModuleDef
 * (single- and multi-phase init, PEP 793 slots), module state, the
 * PyModule_Add* helpers and the inittab bridge that turns
 * PyImport_ExtendInittab entries into builtin modules.
 *
 * Contract: python/capi.h, local://capi-design.md ("Module init").
 * Ported from CPython 3.15 Objects/moduleobject.c, Python/modsupport.c and
 * Python/import.c (PSF-2.0).  Uses the private parts of capi_type.c (same
 * translation unit, included first).
 *
 * The module mirror (kind MODULE, QPCapiModuleObject) is PERSIST: it lives
 * as long as the module object, so md_state lives with the module.
 * Deviations: a single-phase module (PyModule_Create2) or a module made by
 * Py_mod_create is merged into the module object the import system
 * created (dict copied, state shared, the created module kept alive by
 * it); Py_mod_create must return a module object.
 */

/* ========================================================================= */
/* Kind MODULE                                                               */
/* ========================================================================= */

static size_t qp_capi_module_kind_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(QPCapiModuleObject) - sizeof(PyObject);
}

static int qp_capi_module_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    QPCapiModuleObject *m = (QPCapiModuleObject *)op;
    JSValue d = qp_module_get_dict(v), name;
    int r;

    QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    m->md_state_size = -1;
    if (QP_IS_NULL(d)) {
        qp_raise_system_error(ctx, "module object without a dict");
        return -1;
    }
    m->md_dict = qp_capi_new_ref(ctx, d);
    if (!m->md_dict)
        return -1;
    r = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __name__), &name);
    if (r < 0)
        return -1;
    if (r > 0) {
        m->md_name = qp_capi_steal(ctx, name);
        if (!m->md_name)
            return -1;
    }
    return 0;
}

static int qp_capi_module_kind_traverse(PyObject *op, visitproc visit, void *arg)
{
    QPCapiModuleObject *m = (QPCapiModuleObject *)op;
    Py_VISIT(m->md_dict);
    Py_VISIT(m->md_name);
    Py_VISIT(m->qp_state_owner);
    if (m->md_state && m->md_state_traverse && !m->qp_state_owner && !m->qp_state_freed) {
        int r = m->md_state_traverse(op, visit, arg);
        if (r)
            return r;
    }
    return 0;
}

static void qp_capi_module_kind_clear(JSRuntime *rt, PyObject *op)
{
    QPCapiModuleObject *m = (QPCapiModuleObject *)op;
    if (!m->qp_state_freed) {
        m->qp_state_freed = 1;
        if (m->qp_state_owner) {
            m->md_state = NULL;     /* owned by qp_state_owner */
        } else {
            if (m->md_state_clear && m->md_state)
                m->md_state_clear(op);
            if (m->md_state_free)
                m->md_state_free(op);
            if (m->md_state) {
                PyMem_Free(m->md_state);
                m->md_state = NULL;
            }
        }
    }
    Py_CLEAR(m->qp_state_owner);
    Py_CLEAR(m->md_dict);
    Py_CLEAR(m->md_name);
}

/* ========================================================================= */
/* Module objects                                                            */
/* ========================================================================= */

PyObject *PyModule_NewObject(PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue n = qp_capi_value(ctx, name);
    if (JS_IsException(n))
        return NULL;
    return qp_capi_steal(ctx, qp_module_new(ctx, n));
}

PyObject *PyModule_New(const char *name)
{
    PyObject *n = PyUnicode_FromString(name), *m;
    if (!n)
        return NULL;
    m = PyModule_NewObject(n);
    Py_DECREF(n);
    return m;
}

PyObject *PyModule_GetDict(PyObject *m)
{
    if (!PyModule_Check(m)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((QPCapiModuleObject *)m)->md_dict;
}

PyObject *PyModule_GetNameObject(PyObject *mod)
{
    PyObject *dict, *name;
    if (!PyModule_Check(mod)) {
        PyErr_BadArgument();
        return NULL;
    }
    dict = ((QPCapiModuleObject *)mod)->md_dict;
    if (!dict || !PyDict_Check(dict))
        goto error;
    if (PyDict_GetItemStringRef(dict, "__name__", &name) <= 0)
        goto error;
    if (!PyUnicode_Check(name)) {
        Py_DECREF(name);
        goto error;
    }
    return name;
error:
    if (!PyErr_Occurred())
        PyErr_SetString(PyExc_SystemError, "nameless module");
    return NULL;
}

const char *PyModule_GetName(PyObject *m)
{
    PyObject *name = PyModule_GetNameObject(m);
    if (!name)
        return NULL;
    Py_XSETREF(((QPCapiModuleObject *)m)->md_name, name);   /* keeps the UTF-8 alive */
    return PyUnicode_AsUTF8(name);
}

PyObject *PyModule_GetFilenameObject(PyObject *mod)
{
    PyObject *dict, *fileobj;
    if (!PyModule_Check(mod)) {
        PyErr_BadArgument();
        return NULL;
    }
    dict = ((QPCapiModuleObject *)mod)->md_dict;
    if (!dict || PyDict_GetItemStringRef(dict, "__file__", &fileobj) <= 0) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError, "module filename missing");
        return NULL;
    }
    if (!PyUnicode_Check(fileobj)) {
        Py_DECREF(fileobj);
        PyErr_SetString(PyExc_SystemError, "module filename missing");
        return NULL;
    }
    return fileobj;
}

const char *PyModule_GetFilename(PyObject *m)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *f = PyModule_GetFilenameObject(m), *b;
    JSValue v;
    if (!f)
        return NULL;
    v = qp_capi_value(ctx, f);
    b = JS_IsException(v) ? NULL
        : qp_capi_borrow(ctx, m, QP_CAPI_PIN_HASHED(QP_ATOM(ctx, __file__)), v);
    Py_DECREF(f);
    return b ? PyUnicode_AsUTF8(b) : NULL;
}

PyModuleDef *PyModule_GetDef(PyObject *m)
{
    QPCapiModuleObject *mo;
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    mo = (QPCapiModuleObject *)m;
    return mo->md_token_is_def ? (PyModuleDef *)mo->md_token : NULL;
}

void *PyModule_GetState_DuringGC(PyObject *m)
{
    return ((QPCapiModuleObject *)m)->md_state;
}

void *PyModule_GetState(PyObject *m)
{
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    return ((QPCapiModuleObject *)m)->md_state;
}

int PyModule_GetStateSize(PyObject *m, Py_ssize_t *size_p)
{
    *size_p = -1;
    if (!PyModule_Check(m)) {
        PyErr_Format(PyExc_TypeError, "expected module, got %T", m);
        return -1;
    }
    *size_p = ((QPCapiModuleObject *)m)->md_state_size;
    return 0;
}

int PyModule_GetToken_DuringGC(PyObject *m, void **token_p)
{
    *token_p = NULL;
    if (!PyModule_Check(m))
        return -1;
    *token_p = ((QPCapiModuleObject *)m)->md_token;
    return 0;
}

int PyModule_GetToken(PyObject *m, void **token_p)
{
    *token_p = NULL;
    if (!PyModule_Check(m)) {
        PyErr_Format(PyExc_TypeError, "expected module, got %T", m);
        return -1;
    }
    *token_p = ((QPCapiModuleObject *)m)->md_token;
    return 0;
}

/* ========================================================================= */
/* PyModuleDef                                                               */
/* ========================================================================= */

PyTypeObject PyModuleDef_Type = {
    .ob_base = PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "moduledef",
    .tp_basicsize = sizeof(PyModuleDef),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

static Py_ssize_t qp_capi_next_module_index;

PyObject *PyModuleDef_Init(PyModuleDef *def)
{
    if (Py_TYPE(def) != &PyModuleDef_Type)
        Py_SET_TYPE(def, &PyModuleDef_Type);
    if (def->m_base.m_index == 0)
        def->m_base.m_index = ++qp_capi_next_module_index;
    return (PyObject *)def;
}

int PyABIInfo_Check(PyABIInfo *info, const char *module_name)
{
    const char *pre = module_name ? module_name : "", *sep = module_name ? ": " : "";
#define QP_CAPI_XY_MASK 0xffff0000u
    if (!info) {
        PyErr_Format(PyExc_ImportError, "%s%sNULL PyABIInfo", pre, sep);
        return -1;
    }
    if (info->abiinfo_major_version == 0)
        return 0;
    if (info->abiinfo_major_version > 1) {
        PyErr_Format(PyExc_ImportError, "%s%sPyABIInfo version too high", pre, sep);
        return -1;
    }
    if ((info->flags & PyABIInfo_INTERNAL) && info->abi_version &&
        info->abi_version != PY_VERSION_HEX) {
        PyErr_Format(PyExc_ImportError, "%s%sincompatible internal ABI (0x%x != 0x%x)", pre, sep,
                     (unsigned int)info->abi_version, (unsigned int)PY_VERSION_HEX);
        return -1;
    }
    if (info->flags & PyABIInfo_STABLE) {
        if (info->abi_version) {
            if ((info->abi_version & QP_CAPI_XY_MASK) > ((uint32_t)PY_VERSION_HEX & QP_CAPI_XY_MASK)) {
                PyErr_Format(PyExc_ImportError, "%s%sincompatible future stable ABI version (%d.%d)",
                             pre, sep, (int)((info->abi_version >> 24) & 0xff),
                             (int)((info->abi_version >> 16) & 0xff));
                return -1;
            }
            if (info->abi_version < _Py_PACK_VERSION(3, 2)) {
                PyErr_Format(PyExc_ImportError, "%s%sinvalid stable ABI version (%d.%d)",
                             pre, sep, (int)((info->abi_version >> 24) & 0xff),
                             (int)((info->abi_version >> 16) & 0xff));
                return -1;
            }
        }
        if (info->flags & PyABIInfo_INTERNAL) {
            PyErr_Format(PyExc_ImportError, "%s%scannot use both internal and stable ABI", pre, sep);
            return -1;
        }
    } else if (info->abi_version &&
               (info->abi_version & QP_CAPI_XY_MASK) != ((uint32_t)PY_VERSION_HEX & QP_CAPI_XY_MASK)) {
        PyErr_Format(PyExc_ImportError, "%s%sincompatible ABI version (%d.%d)", pre, sep,
                     (int)((info->abi_version >> 24) & 0xff), (int)((info->abi_version >> 16) & 0xff));
        return -1;
    }
#undef QP_CAPI_XY_MASK
    if ((info->flags & (PyABIInfo_GIL | PyABIInfo_FREETHREADED)) == PyABIInfo_FREETHREADED) {
        PyErr_Format(PyExc_ImportError, "%s%sonly compatible with free-threaded CPython", pre, sep);
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Functions, docstring, PyModule_Add*                                       */
/* ========================================================================= */

static int qp_capi_add_methods(PyObject *module, PyObject *name, PyMethodDef *functions)
{
    PyMethodDef *fdef;
    for (fdef = functions; fdef->ml_name; fdef++) {
        PyObject *func;
        if (fdef->ml_flags & (METH_CLASS | METH_STATIC)) {
            PyErr_SetString(PyExc_ValueError, "module functions cannot set METH_CLASS or METH_STATIC");
            return -1;
        }
        func = PyCMethod_New(fdef, module, name, NULL);
        if (!func)
            return -1;
        if (PyObject_SetAttrString(module, fdef->ml_name, func) < 0) {
            Py_DECREF(func);
            return -1;
        }
        Py_DECREF(func);
    }
    return 0;
}

int PyModule_AddFunctions(PyObject *m, PyMethodDef *functions)
{
    PyObject *name = PyModule_GetNameObject(m);
    int r;
    if (!name)
        return -1;
    r = qp_capi_add_methods(m, name, functions);
    Py_DECREF(name);
    return r;
}

int PyModule_SetDocString(PyObject *m, const char *doc)
{
    PyObject *v = PyUnicode_FromString(doc);
    int r;
    if (!v)
        return -1;
    r = PyObject_SetAttrString(m, "__doc__", v);
    Py_DECREF(v);
    return r;
}

int PyModule_AddObjectRef(PyObject *mod, const char *name, PyObject *value)
{
    PyObject *dict;
    if (!PyModule_Check(mod)) {
        PyErr_SetString(PyExc_TypeError, "PyModule_AddObjectRef() first argument must be a module");
        return -1;
    }
    if (!value) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError,
                            "PyModule_AddObjectRef() must be called with an exception raised if value is NULL");
        return -1;
    }
    dict = PyModule_GetDict(mod);
    if (!dict) {
        PyErr_Format(PyExc_SystemError, "module '%s' has no __dict__", PyModule_GetName(mod));
        return -1;
    }
    return PyDict_SetItemString(dict, name, value);
}

int PyModule_Add(PyObject *mod, const char *name, PyObject *value)
{
    int r = PyModule_AddObjectRef(mod, name, value);
    Py_XDECREF(value);
    return r;
}

int PyModule_AddObject(PyObject *mod, const char *name, PyObject *value)
{
    int r = PyModule_AddObjectRef(mod, name, value);
    if (r == 0)
        Py_DECREF(value);
    return r;
}

int PyModule_AddIntConstant(PyObject *m, const char *name, long value)
{
    return PyModule_Add(m, name, PyLong_FromLong(value));
}

int PyModule_AddStringConstant(PyObject *m, const char *name, const char *value)
{
    return PyModule_Add(m, name, PyUnicode_FromString(value));
}

int PyModule_AddType(PyObject *module, PyTypeObject *type)
{
    if (!(type->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(type) < 0)
        return -1;
    return PyModule_AddObjectRef(module, _PyType_Name(type), (PyObject *)type);
}

/* ========================================================================= */
/* Single- and multi-phase creation, execution                                */
/* ========================================================================= */

static int qp_capi_check_api_version(const char *name, int module_api_version)
{
    if (module_api_version != PYTHON_API_VERSION && module_api_version != PYTHON_ABI_VERSION) {
        if (PyErr_WarnFormat(PyExc_RuntimeWarning, 1,
                             "Python C API version mismatch for module %.100s: "
                             "This Python has API version %d, module %.100s has version %d.",
                             name, PYTHON_API_VERSION, name, module_api_version))
            return 0;
    }
    return 1;
}

static void qp_capi_module_copy_deflike(QPCapiModuleObject *md, PyModuleDef *def_like)
{
    md->md_state_size = def_like->m_size;
    md->md_state_traverse = def_like->m_traverse;
    md->md_state_clear = def_like->m_clear;
    md->md_state_free = def_like->m_free;
}

/* _Py_PackageContext: the full name of the extension module whose
   single-phase PyInit_* runs (capi_dynload.c), so PyModule_Create2 can
   name a module "pkg.mod" when its PyModuleDef says "mod". */
static const char *qp_capi_package_context;

/* _PyImport_ResolveNameWithPackageContext */
static const char *qp_capi_resolve_package_context(const char *name)
{
    const char *ctxname = qp_capi_package_context, *p;
    if (ctxname) {
        p = strrchr(ctxname, '.');
        if (p && !strcmp(name, p + 1)) {
            qp_capi_package_context = NULL;
            return ctxname;
        }
    }
    return name;
}

PyObject *PyModule_Create2(PyModuleDef *module, int module_api_version)
{
    QPCapiModuleObject *m;
    const char *name;

    if (!qp_capi.ctx) {
        PyErr_SetString(PyExc_SystemError, "Python import machinery not initialized");
        return NULL;
    }
    PyModuleDef_Init(module);
    name = module->m_name;
    if (!qp_capi_check_api_version(name, module_api_version))
        return NULL;
    if (module->m_slots) {
        PyErr_Format(PyExc_SystemError, "module %s: PyModule_Create is incompatible with m_slots", name);
        return NULL;
    }
    name = qp_capi_resolve_package_context(name);
    m = (QPCapiModuleObject *)PyModule_New(name);
    if (!m)
        return NULL;
    if (module->m_size > 0) {
        m->md_state = PyMem_Malloc(module->m_size);
        if (!m->md_state) {
            PyErr_NoMemory();
            Py_DECREF(m);
            return NULL;
        }
        memset(m->md_state, 0, module->m_size);
    }
    if (module->m_methods && PyModule_AddFunctions((PyObject *)m, module->m_methods) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    if (module->m_doc && PyModule_SetDocString((PyObject *)m, module->m_doc) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    m->md_token = module;
    m->md_token_is_def = true;
    qp_capi_module_copy_deflike(m, module);
    return (PyObject *)m;
}

typedef PyObject *(*qp_capi_createfunc_t)(PyObject *, PyModuleDef *);

/* module_from_slots_and_spec.  `into` (may be NULL): a module object the
   import system already created; it is used instead of PyModule_NewObject
   when no Py_mod_create slot is present.  New reference. */
static PyObject *qp_capi_module_from_slots(const PySlot *slots, PyObject *spec, int module_api_version,
                                           PyModuleDef *original_def, PyObject *into)
{
    qp_capi_createfunc_t create = NULL;
    PyObject *nameobj, *m = NULL;
    const char *name;
    void *token = NULL;
    int (*m_exec)(PyObject *) = NULL;
    PyModuleDef dummy_def, *def_like;
    QPCapiSlotIter it;

    nameobj = PyObject_GetAttrString(spec, "name");
    if (!nameobj)
        return NULL;
    name = PyUnicode_AsUTF8(nameobj);
    if (!name || !qp_capi_check_api_version(name, module_api_version))
        goto error;
    memset(&dummy_def, 0, sizeof(dummy_def));
    if (slots) {
        def_like = &dummy_def;
        qp_capi_slotiter_init(&it, slots, QP_CAPI_SLOT_KIND_MOD);
    } else {
        def_like = original_def;
        qp_capi_slotiter_init_legacy(&it, def_like->m_slots, QP_CAPI_SLOT_KIND_MOD);
    }
    it.name = name;
    while (qp_capi_slotiter_next(&it)) {
        switch (it.current.sl_id) {
        case Py_slot_invalid:
            goto error;
        case Py_mod_create:
            create = (qp_capi_createfunc_t)it.current.sl_func;
            break;
        case Py_mod_exec:
            if (!original_def) {
                if (m_exec) {
                    PyErr_Format(PyExc_SystemError, "module %s has multiple Py_mod_exec slots", name);
                    goto error;
                }
                m_exec = (int (*)(PyObject *))it.current.sl_func;
            }
            break;
        case Py_mod_multiple_interpreters:
        case Py_mod_gil:
            /* one interpreter, GIL build: accepted (capi-design 5) */
            break;
        case Py_mod_abi:
            if (PyABIInfo_Check(it.current.sl_ptr, name) < 0)
                goto error;
            break;
        case Py_mod_token:
            if (original_def && original_def != it.current.sl_ptr) {
                PyErr_Format(PyExc_SystemError,
                             "module %s: arbitrary Py_mod_token not allowed with PyModuleDef", name);
                goto error;
            }
            token = it.current.sl_ptr;
            break;
#define QP_CAPI_DEF_SLOT_CASE(SLOT, TYPE, SL_MEMBER, MEMBER)                                \
        case SLOT:                                                                          \
            if (original_def && (TYPE)original_def->MEMBER != (TYPE)it.current.SL_MEMBER) {  \
                PyErr_Format(PyExc_SystemError, "module %s: %s conflicts with PyModuleDef." #MEMBER, \
                             name, qp_capi_slot_get_name(it.current.sl_id));                 \
                goto error;                                                                 \
            }                                                                               \
            def_like->MEMBER = (TYPE)it.current.SL_MEMBER;                                  \
            break;
        QP_CAPI_DEF_SLOT_CASE(Py_mod_name, const char *, sl_ptr, m_name)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_doc, const char *, sl_ptr, m_doc)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_state_size, Py_ssize_t, sl_size, m_size)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_methods, PyMethodDef *, sl_ptr, m_methods)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_state_traverse, traverseproc, sl_func, m_traverse)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_state_clear, inquiry, sl_func, m_clear)
        QP_CAPI_DEF_SLOT_CASE(Py_mod_state_free, freefunc, sl_func, m_free)
#undef QP_CAPI_DEF_SLOT_CASE
        default:
            break;
        }
    }
    if (!original_def && !qp_capi_slotiter_saw(&it, Py_mod_abi)) {
        PyErr_Format(PyExc_SystemError,
                     "module %s does not define Py_mod_abi, which is mandatory for modules defined from slots only.",
                     name);
        goto error;
    }
    if (def_like->m_size < 0) {
        PyErr_Format(PyExc_SystemError,
                     "module %s: m_size may not be negative for multi-phase initialization", name);
        goto error;
    }
    if (create) {
        m = create(spec, original_def);
        if (!m) {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_SystemError,
                             "creation of module %s failed without setting an exception", name);
            goto error;
        }
        if (PyErr_Occurred()) {
            _PyErr_FormatFromCause(PyExc_SystemError,
                                   "creation of module %s raised unreported exception", name);
            goto error;
        }
    } else if (into) {
        m = Py_NewRef(into);
    } else {
        m = PyModule_NewObject(nameobj);
        if (!m)
            goto error;
    }
    if (PyModule_Check(m)) {
        QPCapiModuleObject *mod = (QPCapiModuleObject *)m;
        mod->md_state = NULL;
        mod->qp_state_freed = 0;
        qp_capi_module_copy_deflike(mod, def_like);
        if (original_def) {
            mod->md_token = original_def;
            mod->md_token_is_def = 1;
        } else {
            mod->md_token = token;
        }
        mod->md_exec = m_exec;
    } else {
        if (def_like->m_size > 0 || def_like->m_traverse || def_like->m_clear || def_like->m_free) {
            PyErr_Format(PyExc_SystemError, "module %s is not a module object, but requests module state",
                         name);
            goto error;
        }
        if (qp_capi_slotiter_saw(&it, Py_mod_exec)) {
            PyErr_Format(PyExc_SystemError,
                         "module %s specifies execution slots, but did not create a ModuleType instance",
                         name);
            goto error;
        }
        if (token) {
            PyErr_Format(PyExc_SystemError,
                         "module %s specifies a token, but did not create a ModuleType instance", name);
            goto error;
        }
    }
    if (def_like->m_methods && qp_capi_add_methods(m, nameobj, def_like->m_methods) < 0)
        goto error;
    if (def_like->m_doc && PyModule_SetDocString(m, def_like->m_doc) < 0)
        goto error;
    Py_DECREF(nameobj);
    return m;
error:
    Py_DECREF(nameobj);
    Py_XDECREF(m);
    return NULL;
}

PyObject *PyModule_FromDefAndSpec2(PyModuleDef *def, PyObject *spec, int module_api_version)
{
    PyModuleDef_Init(def);
    return qp_capi_module_from_slots(NULL, spec, module_api_version, def, NULL);
}

PyObject *PyModule_FromSlotsAndSpec(const PySlot *slots, PyObject *spec)
{
    if (!slots) {
        PyErr_SetString(PyExc_SystemError, "PyModule_FromSlotsAndSpec called with NULL slots");
        return NULL;
    }
    return qp_capi_module_from_slots(slots, spec, PYTHON_API_VERSION, NULL, NULL);
}

static int qp_capi_run_exec_func(PyObject *module, int (*exec)(PyObject *))
{
    int ret = exec(module);
    if (ret != 0) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_SystemError, "execution of %R failed without setting an exception",
                         module);
        return -1;
    }
    if (PyErr_Occurred()) {
        _PyErr_FormatFromCause(PyExc_SystemError, "execution of module %R raised unreported exception",
                               module);
        return -1;
    }
    return 0;
}

static int qp_capi_alloc_state(PyObject *module)
{
    QPCapiModuleObject *md;
    if (!PyModule_Check(module)) {
        PyErr_Format(PyExc_TypeError, "expected module, got %T", module);
        return -1;
    }
    md = (QPCapiModuleObject *)module;
    if (md->md_state_size >= 0 && !md->md_state && !md->qp_state_owner) {
        md->md_state = PyMem_Malloc(md->md_state_size ? md->md_state_size : 1);
        if (!md->md_state) {
            PyErr_NoMemory();
            return -1;
        }
        memset(md->md_state, 0, md->md_state_size ? md->md_state_size : 1);
    }
    return 0;
}

int PyModule_ExecDef(PyObject *module, PyModuleDef *def)
{
    QPCapiSlotIter it;
    if (qp_capi_alloc_state(module) < 0)
        return -1;
    if (!def->m_slots)
        return 0;
    qp_capi_slotiter_init_legacy(&it, def->m_slots, QP_CAPI_SLOT_KIND_MOD);
    while (qp_capi_slotiter_next(&it)) {
        switch (it.current.sl_id) {
        case Py_slot_invalid:
            return -1;
        case Py_mod_exec:
            if (qp_capi_run_exec_func(module, (int (*)(PyObject *))it.current.sl_func) < 0)
                return -1;
            break;
        default:
            break;
        }
    }
    return 0;
}

int PyModule_Exec(PyObject *module)
{
    QPCapiModuleObject *md;
    if (qp_capi_alloc_state(module) < 0)
        return -1;
    md = (QPCapiModuleObject *)module;
    if (md->md_exec)
        return qp_capi_run_exec_func(module, md->md_exec);
    if (md->md_token_is_def && md->md_token)
        return PyModule_ExecDef(module, (PyModuleDef *)md->md_token);
    return 0;
}

/* Merge a module created by C (single-phase init, Py_mod_create) into the
   module object of the import: attributes copied, state shared (the
   created module stays alive and owns it). */
static int qp_capi_module_merge(PyObject *into, PyObject *created)
{
    QPCapiModuleObject *dst = (QPCapiModuleObject *)into, *src;
    PyObject *name;
    int r;
    if (into == created)
        return 0;
    if (!PyModule_Check(created)) {
        PyErr_Format(PyExc_SystemError,
                     "module %R: creating a non-module object is not supported for builtin modules", into);
        return -1;
    }
    src = (QPCapiModuleObject *)created;
    name = PyModule_GetNameObject(into);
    if (!name)
        return -1;
    r = PyDict_Update(dst->md_dict, src->md_dict);
    if (r == 0)
        r = PyDict_SetItemString(dst->md_dict, "__name__", name);
    Py_DECREF(name);
    if (r < 0)
        return -1;
    dst->md_state = src->md_state;
    dst->md_state_size = src->md_state_size;
    dst->md_state_traverse = NULL;
    dst->md_state_clear = NULL;
    dst->md_state_free = NULL;
    dst->md_token = src->md_token;
    dst->md_token_is_def = src->md_token_is_def;
    dst->md_exec = NULL;
    Py_XSETREF(dst->qp_state_owner, Py_NewRef(created));
    return 0;
}

/* Spec given to Py_mod_create of a builtin module: the module's __spec__
   when set, else a namespace with name and origin 'built-in'. */
static PyObject *qp_capi_builtin_spec(PyObject *module, PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *spec, *kw, *args, *ns;
    if (PyObject_GetOptionalAttrString(module, "__spec__", &spec) < 0)
        return NULL;
    if (spec && spec != Py_None)
        return spec;
    Py_XDECREF(spec);
    kw = Py_BuildValue("{s:O,s:s}", "name", name, "origin", "built-in");
    if (!kw)
        return NULL;
    args = PyTuple_New(0);
    ns = qp_capi_type_from_qp(ctx, QP_TYPE(ctx, SimpleNamespace)) && args
        ? PyObject_Call((PyObject *)qp_capi_type_from_qp(ctx, QP_TYPE(ctx, SimpleNamespace)), args, kw)
        : NULL;
    Py_XDECREF(args);
    Py_DECREF(kw);
    return ns;
}

/* Run an init function's result into `module` (import's module object).
   0 / -1. */
static int qp_capi_module_init_into(PyObject *module, PyObject *name, PyObject *res, const char *cname)
{
    PyObject *spec, *m;
    PyModuleDef *def;
    int r;

    if (!res) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_SystemError,
                         "initialization of %s failed without raising an exception", cname);
        return -1;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(res);
        _PyErr_FormatFromCause(PyExc_SystemError,
                               "initialization of %s raised unreported exception", cname);
        return -1;
    }
    if (!PyObject_TypeCheck(res, &PyModuleDef_Type)) {
        /* single-phase init */
        if (!PyModule_Check(res)) {
            Py_DECREF(res);
            PyErr_Format(PyExc_SystemError,
                         "initialization of %s did not return an extension module", cname);
            return -1;
        }
        r = qp_capi_module_merge(module, res);
        Py_DECREF(res);
        return r;
    }
    /* multi-phase init */
    def = (PyModuleDef *)res;
    PyModuleDef_Init(def);
    spec = qp_capi_builtin_spec(module, name);
    if (!spec)
        return -1;
    m = qp_capi_module_from_slots(NULL, spec, PYTHON_API_VERSION, def, module);
    Py_DECREF(spec);
    if (!m)
        return -1;
    r = PyModule_Check(m) ? PyModule_ExecDef(m, def) : 0;
    if (r == 0)
        r = qp_capi_module_merge(module, m);
    Py_DECREF(m);
    return r;
}

PyObject *PyImport_CreateModuleFromInitfunc(PyObject *spec, PyObject *(*initfunc)(void))
{
    PyObject *res = initfunc(), *m;
    PyModuleDef *def;
    if (!res) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError, "initialization failed without raising an exception");
        return NULL;
    }
    if (!PyObject_TypeCheck(res, &PyModuleDef_Type))
        return res;
    def = (PyModuleDef *)res;
    m = PyModule_FromDefAndSpec2(def, spec, PYTHON_API_VERSION);
    if (m && PyModule_Check(m) && PyModule_ExecDef(m, def) < 0)
        Py_CLEAR(m);
    return m;
}

/* ========================================================================= */
/* Inittab bridge                                                            */
/* ========================================================================= */

static struct _inittab qp_capi_inittab_empty[1] = { { NULL, NULL } };
struct _inittab *PyImport_Inittab = qp_capi_inittab_empty;
static int qp_capi_inittab_n;

int PyImport_ExtendInittab(struct _inittab *newtab)
{
    struct _inittab *tab;
    QPModuleDef **defs;
    int i, n;

    for (n = 0; newtab[n].name; n++)
        ;
    if (n == 0)
        return 0;
    tab = qp_sys_realloc(qp_capi.inittab, (qp_capi_inittab_n + n + 1) * sizeof(*tab));
    if (!tab)
        return -1;
    qp_capi.inittab = tab;
    PyImport_Inittab = tab;
    defs = qp_sys_realloc(qp_capi.qp_moddefs, (qp_capi_inittab_n + n) * sizeof(*defs));
    if (!defs) {
        tab[qp_capi_inittab_n].name = NULL;
        tab[qp_capi_inittab_n].initfunc = NULL;
        return -1;
    }
    qp_capi.qp_moddefs = defs;
    for (i = 0; i < n; i++) {
        QPModuleDef *d = qp_sys_calloc(1, sizeof(*d));
        if (!d) {
            tab[qp_capi_inittab_n].name = NULL;
            tab[qp_capi_inittab_n].initfunc = NULL;
            qp_capi.qp_moddef_count = qp_capi_inittab_n;
            return -1;
        }
        d->name = newtab[i].name;
        d->doc = NULL;
        d->methods = NULL;
        d->exec = qp_capi_module_exec;
        tab[qp_capi_inittab_n] = newtab[i];
        defs[qp_capi_inittab_n] = d;
        qp_capi_inittab_n++;
    }
    tab[qp_capi_inittab_n].name = NULL;
    tab[qp_capi_inittab_n].initfunc = NULL;
    qp_capi.qp_moddef_count = qp_capi_inittab_n;
    return 0;
}

int PyImport_AppendInittab(const char *name, PyObject *(*initfunc)(void))
{
    struct _inittab newtab[2];
    newtab[0].name = name;
    newtab[0].initfunc = initfunc;
    newtab[1].name = NULL;
    newtab[1].initfunc = NULL;
    return PyImport_ExtendInittab(newtab);
}

/* The self-contained CPython modules of capi/Setup.qp are part of the engine
   library (capi_core_inittab.c, capi/qpcapi.h): register them once per
   process, on the first Python context, so every embedder has them.  The
   launcher's own entries (PyImport_ExtendInittab before the first context)
   keep precedence. */
extern struct _inittab _PyQp_CoreInittab[];
static pthread_once_t qp_capi_core_once = PTHREAD_ONCE_INIT;
static int qp_capi_core_status;

static void qp_capi_core_register(void)
{
    qp_capi_core_status = PyImport_ExtendInittab(_PyQp_CoreInittab);
}

static int qp_capi_register_core_modules(void)
{
    pthread_once(&qp_capi_core_once, qp_capi_core_register);
    return qp_capi_core_status;
}

static int qp_capi_inittab_index(const char *name, size_t len)
{
    int i;
    for (i = 0; i < qp_capi_inittab_n; i++) {
        const char *n = qp_capi.inittab[i].name;
        if (strlen(n) == len && !memcmp(n, name, len))
            return i;
    }
    return -1;
}

static const QPModuleDef *qp_capi_inittab_find(const char *name, size_t len)
{
    int i = qp_capi_inittab_index(name, len);
    return i < 0 ? NULL : qp_capi.qp_moddefs[i];
}

static int qp_capi_inittab_count(void)
{
    return qp_capi_inittab_n;
}

static const QPModuleDef *qp_capi_inittab_get(int i)
{
    return i >= 0 && i < qp_capi_inittab_n ? qp_capi.qp_moddefs[i] : NULL;
}

static void qp_capi_snap_module_ready(JSContext *ctx, JSValueConst module,
                                      const char *iname, uint32_t type_serial);  /* capi_snapshot.c */

/* exec of the synthesized QPModuleDef entries: bind the C API, run
   PyInit_<name> and execute its result into `module`.  iname: the inittab
   entry, NULL for the module's __name__ (a snapshot restore names it,
   since a module may rename itself). */
static int qp_capi_module_exec_as(JSContext *ctx, JSValueConst module, const char *iname)
{
    JSValue d = qp_module_get_dict(module), namev;
    const char *cname;
    size_t len;
    PyObject *m = NULL, *name = NULL, *res;
    uint32_t type_serial = qp_type_alloc_serial;
    int i, r = -1;

    if (QP_IS_NULL(d)) {
        qp_raise_system_error(ctx, "builtin module without a dict");
        return -1;
    }
    if (qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __name__), &namev) <= 0) {
        if (!JS_HasException(ctx))
            qp_raise_system_error(ctx, "nameless module");
        return -1;
    }
    cname = qp_str_utf8(ctx, namev, &len);
    if (!cname) {
        JS_FreeValue(ctx, namev);
        return -1;
    }
    i = iname ? qp_capi_inittab_index(iname, strlen(iname)) : qp_capi_inittab_index(cname, len);
    if (i < 0) {
        qp_raise(ctx, QP_EXC(ctx, ImportError), "no C extension module named '%s'",
                 iname ? iname : cname);
        goto done;
    }
    if (qp_capi_init(ctx) < 0)
        goto done;
    m = qp_capi_new_ref(ctx, module);
    if (!m)
        goto done;
    name = qp_capi_new_ref(ctx, namev);
    if (!name)
        goto done;
    res = qp_capi.inittab[i].initfunc();
    r = qp_capi_module_init_into(m, name, res, cname);
    if (r == 0)
        qp_capi_snap_module_ready(ctx, module, qp_capi.inittab[i].name, type_serial);
done:
    Py_XDECREF(name);
    Py_XDECREF(m);
    JS_FreeCString(ctx, cname);
    JS_FreeValue(ctx, namev);
    return r;
}

static int qp_capi_module_exec(JSContext *ctx, JSValueConst module)
{
    return qp_capi_module_exec_as(ctx, module, NULL);
}

/* ========================================================================= */
/* Init (called by qp_capi_init_types)                                        */
/* ========================================================================= */

static int qp_capi_init_modules(JSContext *ctx)
{
    qp_capi.kind_ops[QP_CAPI_K_MODULE] = (QPCapiKindOps){
        .name = "module", .size = qp_capi_module_kind_size, .init = qp_capi_module_kind_init,
        .traverse = qp_capi_module_kind_traverse, .clear = qp_capi_module_kind_clear };
    return PyType_Ready(&PyModuleDef_Type);
}
