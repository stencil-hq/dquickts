/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_context.h used by the vendored
 * modules (capi/modules).  Contexts are our _contextvars module;
 * unit E2 implements the functions (capi-design.md §8 _asyncio).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CONTEXT_H
#define Py_INTERNAL_CONTEXT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyContext_Enter(PyThreadState *ts, PyObject *octx);
PyAPI_FUNC(int) _PyContext_Exit(PyThreadState *ts, PyObject *octx);

#endif /* !Py_INTERNAL_CONTEXT_H */
