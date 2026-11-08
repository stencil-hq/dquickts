/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * watcher limits of CPython 3.15 Include/internal/pycore_interp_structs.h,
 * used by the test-only _testcapi (Makefile capi-testmods).  The C API
 * (python/capi_*.c) keeps the same limits.  The interpreter structs
 * themselves are not provided.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_INTERP_STRUCTS_H
#define Py_INTERNAL_INTERP_STRUCTS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#define CODE_MAX_WATCHERS 8
#define CONTEXT_MAX_WATCHERS 8
#define FUNC_MAX_WATCHERS 8

#endif /* !Py_INTERNAL_INTERP_STRUCTS_H */
