/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_moduleobject.h used by the vendored
 * modules (capi/modules).  The struct is CPython's (GIL build);
 * the MODULE mirror (python/capi_module.c, unit B) materializes it, so the
 * inline accessors stay CPython's.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_MODULEOBJECT_H
#define Py_INTERNAL_MODULEOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdbool.h>

typedef int (*_Py_modexecfunc)(PyObject *);

typedef struct {
    PyObject_HEAD
    PyObject *md_dict;
    void *md_state;
    PyObject *md_weaklist;
    // for logging purposes after md_dict is cleared
    PyObject *md_name;
    bool md_token_is_def;  /* if true, `md_token` is the PyModuleDef */
    Py_ssize_t md_state_size;
    traverseproc md_state_traverse;
    inquiry md_state_clear;
    freefunc md_state_free;
    void *md_token;
    _Py_modexecfunc md_exec;  /* only set if md_token_is_def is true */
} PyModuleObject;

#define _PyModule_CAST(op) \
    (assert(PyModule_Check(op)), _Py_CAST(PyModuleObject*, (op)))

static inline PyModuleDef *_PyModule_GetDefOrNull(PyObject *arg) {
    PyModuleObject *mod = _PyModule_CAST(arg);
    if (mod->md_token_is_def) {
        return (PyModuleDef *)mod->md_token;
    }
    return NULL;
}

// Get md_token. Used in _DuringGC functions; must have no side effects.
static inline PyModuleDef *_PyModule_GetToken(PyObject *arg) {
    PyModuleObject *mod = _PyModule_CAST(arg);
    return (PyModuleDef *)mod->md_token;
}

// Get md_state. Used in _DuringGC functions; must have no side effects.
static inline void* _PyModule_GetState(PyObject* mod) {
    return _PyModule_CAST(mod)->md_state;
}

static inline PyObject* _PyModule_GetDict(PyObject *mod) {
    PyObject *dict = _PyModule_CAST(mod)->md_dict;
    // _PyModule_GetDict(mod) must not be used after calling module_clear(mod)
    assert(dict != NULL);
    return dict;  // borrowed reference
}

#endif /* !Py_INTERNAL_MODULEOBJECT_H */
