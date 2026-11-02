/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_typeobject.h used by the vendored
 * modules (capi/modules).  Unit B implements the functions; heap
 * type mirrors fill ht_module, so _PyType_GetModuleState() is CPython's.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_TYPEOBJECT_H
#define Py_INTERNAL_TYPEOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_moduleobject.h"
#include "pycore_global_strings.h"  // _Py_ID(), reached through CPython's pycore_interp_structs.h

PyAPI_FUNC(int) _PyStaticType_InitForExtension(
    PyInterpreterState *interp,
     PyTypeObject *self);

static inline void *
_PyType_GetModuleState(PyTypeObject *type)
{
    assert(PyType_Check(type));
    assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    PyHeapTypeObject *et = (PyHeapTypeObject *)type;
    assert(et->ht_module);
    PyModuleObject *mod = (PyModuleObject *)(et->ht_module);
    assert(mod != NULL);
    return mod->md_state;
}

PyAPI_FUNC(PyObject *) _PyType_GetDict(PyTypeObject *);

#endif /* !Py_INTERNAL_TYPEOBJECT_H */
