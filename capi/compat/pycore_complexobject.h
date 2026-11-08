/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_complexobject.h used by the vendored
 * modules (capi/modules).  Included by cmath only; the
 * _Py_c_* functions are public (cpython/complexobject.h, unit C).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_COMPLEXOBJECT_H
#define Py_INTERNAL_COMPLEXOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif



#endif /* !Py_INTERNAL_COMPLEXOBJECT_H */
