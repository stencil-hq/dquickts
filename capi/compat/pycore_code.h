/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_code.h used by the vendored
 * modules (capi/modules).  The runtime does not specialize
 * bytecode (no adaptive interpreter), so ENABLE_SPECIALIZATION is 0 and
 * test.support.requires_specialization skips.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CODE_H
#define Py_INTERNAL_CODE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_stats.h"

#define ENABLE_SPECIALIZATION 0

#endif /* !Py_INTERNAL_CODE_H */
