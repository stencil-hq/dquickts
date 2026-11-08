/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_critical_section.h used by the vendored
 * modules (capi/modules).  GIL build: critical sections are no-ops
 * (capi-design.md §5); the public Py_BEGIN_CRITICAL_SECTION* of
 * cpython/critical_section.h are no-ops too.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CRITICAL_SECTION_H
#define Py_INTERNAL_CRITICAL_SECTION_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_lock.h"
#include "pycore_pystate.h"

# define Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(original) {
# define Py_END_CRITICAL_SECTION_SEQUENCE_FAST() }
# define _Py_CRITICAL_SECTION_ASSERT_MUTEX_LOCKED(mutex)
# define _Py_CRITICAL_SECTION_ASSERT_OBJECT_LOCKED(op)

#endif /* !Py_INTERNAL_CRITICAL_SECTION_H */
