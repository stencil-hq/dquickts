/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_stats.h used by the vendored
 * modules (capi/modules).  Py_STATS is never defined: the
 * statistics macros are no-ops and _Py_GetSpecializationStats() returns
 * None (capi/compat/qp_internals.c).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_STATS_H
#define Py_INTERNAL_STATS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_bitutils.h"

#define EVAL_CALL_STAT_INC(name) ((void)0)
#define EVAL_CALL_STAT_INC_IF_FUNCTION(name, callable) ((void)0)
#define OBJECT_STAT_INC(name) ((void)0)

// Export for '_opcode' shared extension
PyAPI_FUNC(PyObject*) _Py_GetSpecializationStats(void);

#endif /* !Py_INTERNAL_STATS_H */
