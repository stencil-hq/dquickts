/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_symtable.h used by the vendored
 * modules (capi/modules).  Unit D exports the functions over the
 * compiler's mangling (python/symtable.c, capi-design.md P1).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_SYMTABLE_H
#define Py_INTERNAL_SYMTABLE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(PyObject *)
_Py_Mangle(PyObject *, PyObject *);

PyAPI_FUNC(int)
_Py_IsPrivateName(PyObject *);

#endif /* !Py_INTERNAL_SYMTABLE_H */
