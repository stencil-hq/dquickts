/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_pylifecycle.h used by the vendored
 * modules (capi/modules).  Unit E2 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_LIFECYCLE_H
#define Py_INTERNAL_LIFECYCLE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyOS_URandomNonblock(void *buffer, Py_ssize_t size);
PyAPI_FUNC(int) _Py_IsInterpreterFinalizing(PyInterpreterState *interp);
/* readline: setlocale(category, "") (POSIX hosts) */
PyAPI_FUNC(char *) _Py_SetLocaleFromEnv(int category);

#endif /* !Py_INTERNAL_LIFECYCLE_H */
