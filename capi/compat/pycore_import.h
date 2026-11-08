/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_import.h used by the vendored
 * modules (capi/modules).  Unit E2 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_IMPORT_H
#define Py_INTERNAL_IMPORT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyImport_SetModule(PyObject *name, PyObject *module);
PyAPI_FUNC(int) _PyImport_SetModuleString(const char *name, PyObject* module);

#endif /* !Py_INTERNAL_IMPORT_H */
