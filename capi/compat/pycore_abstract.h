/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_abstract.h used by the vendored
 * modules (capi/modules).  Unit C implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_ABSTRACT_H
#define Py_INTERNAL_ABSTRACT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Fast inlined version of PyIndex_Check()
static inline int
_PyIndex_Check(PyObject *obj)
{
    PyNumberMethods *tp_as_number = Py_TYPE(obj)->tp_as_number;
    return (tp_as_number != NULL && tp_as_number->nb_index != NULL);
}

// Convert Python int to Py_ssize_t. Do nothing if the argument is None.
PyAPI_FUNC(int) _Py_convert_optional_to_ssize_t(PyObject *, void *);

// Same as PyNumber_Index() but can return an instance of a subclass of int.
PyAPI_FUNC(PyObject*) _PyNumber_Index(PyObject *o);

#endif /* !Py_INTERNAL_ABSTRACT_H */
