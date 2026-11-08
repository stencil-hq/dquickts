/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_signal.h used by the
 * vendored modules (capi/modules).  _multiprocessing includes it for
 * _PyOS_IsMainThread() and _PyOS_SigintEvent(), which only its Windows code
 * calls; the C API targets POSIX hosts (CONFIG_PYTHON_CAPI), so nothing
 * here is implemented.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_SIGNAL_H
#define Py_INTERNAL_SIGNAL_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <signal.h>                // NSIG

#ifdef MS_WINDOWS
PyAPI_FUNC(int) _PyOS_IsMainThread(void);
PyAPI_FUNC(void *) _PyOS_SigintEvent(void);
#endif

#endif /* !Py_INTERNAL_SIGNAL_H */
