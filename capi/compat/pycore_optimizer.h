/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_optimizer.h used by the vendored
 * modules (capi/modules).  There is no tier-2 optimizer:
 * _Py_GetExecutor() always fails as CPython does without an executor at the
 * offset (capi/compat/qp_internals.c).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_OPTIMIZER_H
#define Py_INTERNAL_OPTIMIZER_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

typedef struct _PyExecutorObject _PyExecutorObject;

PyAPI_FUNC(_PyExecutorObject*) _Py_GetExecutor(PyCodeObject *code, int offset);

#endif /* !Py_INTERNAL_OPTIMIZER_H */
