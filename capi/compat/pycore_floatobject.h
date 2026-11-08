/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_floatobject.h used by the vendored
 * modules (capi/modules).  Header-only.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_FLOATOBJECT_H
#define Py_INTERNAL_FLOATOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#ifdef DOUBLE_IS_BIG_ENDIAN_IEEE754
#  define _PY_FLOAT_BIG_ENDIAN 1
#  define _PY_FLOAT_LITTLE_ENDIAN 0
#else
#  define _PY_FLOAT_BIG_ENDIAN 0
#  define _PY_FLOAT_LITTLE_ENDIAN 1
#endif

#endif /* !Py_INTERNAL_FLOATOBJECT_H */
