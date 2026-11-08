/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_pymem.h used by the vendored
 * modules (capi/modules).  Unit A implements the function.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_PYMEM_H
#define Py_INTERNAL_PYMEM_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// strdup() using PyMem_Malloc().
PyAPI_FUNC(char*) _PyMem_Strdup(const char *str);

#endif /* !Py_INTERNAL_PYMEM_H */
