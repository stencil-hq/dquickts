/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_pystate.h used by the vendored
 * modules (capi/modules).  M3 runs one interpreter and one thread
 * state (capi-design.md §5); unit E2 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_PYSTATE_H
#define Py_INTERNAL_PYSTATE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_qp_tstate.h"

static inline PyThreadState *
_PyThreadState_GET(void)
{
    return PyThreadState_GetUnchecked();
}

static inline PyInterpreterState *
_PyInterpreterState_GET(void)
{
    return PyInterpreterState_Get();
}

/* Debug assertion of CPython (holding the GIL): always true here. */
#define _Py_AssertHoldsTstate() ((void)0)

static inline int
_Py_IsMainInterpreter(PyInterpreterState *interp)
{
    (void)interp;
    return 1;
}

/* Stop-the-world pauses: no-ops in the GIL build (unit E2). */
PyAPI_FUNC(void) _PyEval_StopTheWorld(PyInterpreterState *interp);
PyAPI_FUNC(void) _PyEval_StartTheWorld(PyInterpreterState *interp);

#endif /* !Py_INTERNAL_PYSTATE_H */
