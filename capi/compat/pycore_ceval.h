/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_ceval.h used by the vendored
 * modules (capi/modules).  Unit E2 implements the functions; the
 * special-method table is built by capi/compat/qp_internals.c (unit F).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CEVAL_H
#define Py_INTERNAL_CEVAL_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_pyerrors.h"
#include "pycore_runtime.h"

/* Include/internal/pycore_ceval.h (_lsprof): C profile/trace functions of
   the calling thread (python/capi_runtime.c) */
PyAPI_FUNC(int) _PyEval_SetProfile(PyThreadState *tstate, Py_tracefunc func, PyObject *arg);
PyAPI_FUNC(int) _PyEval_SetTrace(PyThreadState *tstate, Py_tracefunc func, PyObject *arg);

/* Include/internal/pycore_instruments.h: the reserved sys.monitoring tool
   ids (python/instrumentation.c) */
#define PY_MONITORING_DEBUGGER_ID 0
#define PY_MONITORING_COVERAGE_ID 1
#define PY_MONITORING_PROFILER_ID 2
#define PY_MONITORING_OPTIMIZER_ID 5

// Helper to look up a builtin object
PyAPI_FUNC(PyObject*) _PyEval_GetBuiltin(PyObject *);

/* The recursion guard is the runtime's (the public functions, unit E2). */
static inline int _Py_EnterRecursiveCall(const char *where) {
    return Py_EnterRecursiveCall(where);
}

static inline void _Py_LeaveRecursiveCall(void)  {
    Py_LeaveRecursiveCall();
}

typedef struct _special_method {
    PyObject *name;
    const char *error;
    const char *error_suggestion;  // improved optional suggestion
} _Py_SpecialMethod;

/* CPython's table holds &_Py_ID(...), which is not a constant expression
   here: the table is filled on first use (capi/compat/qp_internals.c). */
PyAPI_FUNC(const _Py_SpecialMethod *) _PyQp_SpecialMethods(void);
#define _Py_SpecialMethods (_PyQp_SpecialMethods())

#define SPECIAL___ENTER__   0
#define SPECIAL___EXIT__    1
#define SPECIAL___AENTER__  2
#define SPECIAL___AEXIT__   3
#define SPECIAL_MAX   3

#endif /* !Py_INTERNAL_CEVAL_H */
