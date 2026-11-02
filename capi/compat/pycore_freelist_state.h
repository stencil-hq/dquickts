/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_freelist_state.h used by the vendored
 * modules (capi/modules).  Freelists are disabled; the sizes are
 * CPython's (used as push limits, which always fail).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_FREELIST_STATE_H
#define Py_INTERNAL_FREELIST_STATE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#  define Py_futureiters_MAXFREELIST 255

#endif /* !Py_INTERNAL_FREELIST_STATE_H */
