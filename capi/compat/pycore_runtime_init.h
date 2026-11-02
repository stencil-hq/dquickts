/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_runtime_init.h used by the vendored
 * modules (capi/modules).  Static initializers of _PyRuntime do not
 * exist here; GENERATE_DEBUG_SECTION (pycore_debug_offsets.h) keeps the
 * declaration without the debugger section.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_RUNTIME_INIT_H
#define Py_INTERNAL_RUNTIME_INIT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_runtime.h"

/* CPython places the declaration in a named section read by external
   debuggers (Tools/inspection); dquickts has no such tooling. */
#define GENERATE_DEBUG_SECTION(name, declaration) declaration

#endif /* !Py_INTERNAL_RUNTIME_INIT_H */
