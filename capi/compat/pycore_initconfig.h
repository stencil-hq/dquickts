/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_initconfig.h used by the vendored
 * modules (capi/modules).  Header-only (PyStatus is public,
 * cpython/initconfig.h).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_CORECONFIG_H
#define Py_INTERNAL_CORECONFIG_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* --- PyStatus ----------------------------------------------- */

/* Almost all errors causing Python initialization to fail */
#ifdef _MSC_VER
   /* Visual Studio 2015 doesn't implement C99 __func__ in C */
#  define _PyStatus_GET_FUNC() __FUNCTION__
#else
#  define _PyStatus_GET_FUNC() __func__
#endif

#define _PyStatus_OK() \
    (PyStatus){._type = _PyStatus_TYPE_OK}
    /* other fields are set to 0 */
#define _PyStatus_ERR(ERR_MSG) \
    (PyStatus){ \
        ._type = _PyStatus_TYPE_ERROR, \
        .func = _PyStatus_GET_FUNC(), \
        .err_msg = (ERR_MSG)}
        /* other fields are set to 0 */
#define _PyStatus_NO_MEMORY_ERRMSG "memory allocation failed"
#define _PyStatus_NO_MEMORY() _PyStatus_ERR(_PyStatus_NO_MEMORY_ERRMSG)
#define _PyStatus_EXCEPTION(err) \
    ((err)._type != _PyStatus_TYPE_OK)

#endif /* !Py_INTERNAL_CORECONFIG_H */
