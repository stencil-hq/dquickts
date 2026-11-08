/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_runtime.h used by the vendored
 * modules (capi/modules).  The modules include it for _Py_ID()
 * and the thread state accessors; there is no _PyRuntime.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_RUNTIME_H
#define Py_INTERNAL_RUNTIME_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include <stdbool.h>
#include "pycore_context.h"
#include "pycore_global_strings.h"
#include "pycore_pystate.h"



#endif /* !Py_INTERNAL_RUNTIME_H */
