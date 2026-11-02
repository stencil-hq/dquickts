/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_unicodectype.h used by the vendored
 * modules (capi/modules).  Unit D exports the functions over
 * python/unicodetype.c (capi-design.md P1).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_UNICODECTYPE_H
#define Py_INTERNAL_UNICODECTYPE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyUnicode_IsXidStart(Py_UCS4 ch);
PyAPI_FUNC(int) _PyUnicode_IsXidContinue(Py_UCS4 ch);

#endif /* !Py_INTERNAL_UNICODECTYPE_H */
