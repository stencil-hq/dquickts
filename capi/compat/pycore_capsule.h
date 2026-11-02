/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_capsule.h used by the vendored
 * modules (capi/modules).  Unit E1 implements the function.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_PYCAPSULE_H
#define Py_INTERNAL_PYCAPSULE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyCapsule_SetTraverse(PyObject *op, traverseproc traverse_func, inquiry clear_func);

#endif /* !Py_INTERNAL_PYCAPSULE_H */
