/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_compile.h used by the vendored
 * modules (capi/modules).  Implemented by capi/compat/qp_internals.c
 * (unit F) from the pycore_intrinsics.h names.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_COMPILE_H
#define Py_INTERNAL_COMPILE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Export for '_opcode' extension module
PyAPI_FUNC(PyObject *) _PyCompile_GetUnaryIntrinsicName(int index);
PyAPI_FUNC(PyObject *) _PyCompile_GetBinaryIntrinsicName(int index);

#endif /* !Py_INTERNAL_COMPILE_H */
