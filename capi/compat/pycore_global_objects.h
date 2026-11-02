/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_global_objects.h used by the
 * vendored modules (capi/modules; _ctypes includes it for _Py_ID()).  There
 * is no _PyRuntime: the identifiers and singletons are the immortal objects
 * of pycore_global_strings.h.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_GLOBAL_OBJECTS_H
#define Py_INTERNAL_GLOBAL_OBJECTS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_global_strings.h"

#endif /* !Py_INTERNAL_GLOBAL_OBJECTS_H */
