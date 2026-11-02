/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_dict.h used by the vendored
 * modules (capi/modules).  Unit E1 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_DICT_H
#define Py_INTERNAL_DICT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyDict_GetItemRef_KnownHash(PyDictObject *op, PyObject *key, Py_hash_t hash, PyObject **result);
PyAPI_FUNC(int) _PyDict_GetItemRef_KnownHash_LockHeld(PyDictObject *op, PyObject *key, Py_hash_t hash, PyObject **result);

// "KnownHash" variants
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                          PyObject *item, Py_hash_t hash);
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash_LockHeld(PyDictObject *mp, PyObject *key,
                                                   PyObject *value, Py_hash_t hash);

/* Exported here (extern in CPython, where _sre and _functools are built
   into libpython). */
PyAPI_FUNC(int) _PyDict_Next(
    PyObject *mp, Py_ssize_t *pos, PyObject **key, PyObject **value, Py_hash_t *hash);

PyAPI_FUNC(int) _PyDict_Pop_KnownHash(
    PyDictObject *dict,
    PyObject *key,
    Py_hash_t hash,
    PyObject **result);

PyAPI_FUNC(void) _PyDict_Clear_LockHeld(PyObject *op);

// _elementtree
PyAPI_FUNC(PyObject*) _PyDict_CopyAsDict(PyObject *op);

#endif /* !Py_INTERNAL_DICT_H */
