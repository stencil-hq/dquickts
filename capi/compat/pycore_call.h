/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_call.h used by the vendored
 * modules (capi/modules).  The vectorcall helpers go through the
 * exported PyObject_Vectorcall() (unit A), which checks the result like
 * CPython's _Py_CheckFunctionResult().
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CALL_H
#define Py_INTERNAL_CALL_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_stats.h"
#include "pycore_pystate.h"

/* Suggested size (number of positional arguments) for arrays of PyObject*
   allocated on a C stack to avoid allocating memory on the heap memory. */
#define _PY_FASTCALL_SMALL_STACK 5

PyAPI_FUNC(PyObject*) _PyObject_CallMethod(
    PyObject *obj,
    PyObject *name,
    const char *format, ...);

static inline PyObject *
_PyObject_VectorcallTstate(PyThreadState *tstate, PyObject *callable,
                           PyObject *const *args, size_t nargsf,
                           PyObject *kwnames)
{
    (void)tstate;
    return PyObject_Vectorcall(callable, args, nargsf, kwnames);
}

static inline PyObject *
_PyObject_CallNoArgs(PyObject *func)
{
    return PyObject_Vectorcall(func, NULL, 0, NULL);
}

#endif /* !Py_INTERNAL_CALL_H */
