/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_function.h used by the
 * test-only _testcapi (Makefile capi-testmods).  The limit is the one of
 * python/capi_type.c (QP_CAPI_FUNC_MAX_WATCHERS).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_FUNCTION_H
#define Py_INTERNAL_FUNCTION_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_interp_structs.h"  // FUNC_MAX_WATCHERS

#endif /* !Py_INTERNAL_FUNCTION_H */
