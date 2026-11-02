/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_setobject.h used by the vendored
 * modules (capi/modules).  Unit E1 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_SETOBJECT_H
#define Py_INTERNAL_SETOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PySet_NextEntryRef(
    PyObject *set,
    Py_ssize_t *pos,
    PyObject **key,
    Py_hash_t *hash);

PyAPI_FUNC(int) _PySet_Update(PyObject *set, PyObject *iterable);

#endif /* !Py_INTERNAL_SETOBJECT_H */
