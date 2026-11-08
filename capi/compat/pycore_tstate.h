/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_tstate.h used by the vendored
 * modules (capi/modules).  _PyThreadStateImpl comes from
 * pycore_qp_tstate.h (layout shared with python/capi_runtime.c).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_TSTATE_H
#define Py_INTERNAL_TSTATE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_qp_tstate.h"



#endif /* !Py_INTERNAL_TSTATE_H */
