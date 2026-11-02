/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_list.h used by the vendored
 * modules (capi/modules).  List items live in the JS array
 * (capi-design.md §1.4): unit E1 implements the append, unit A the item
 * snapshot.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_LIST_H
#define Py_INTERNAL_LIST_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int)
_PyList_AppendTakeRefListResize(PyListObject *self, PyObject *newitem);

/* Append newitem (a stolen reference); 0 or -1 with an exception. */
static inline int
_PyList_AppendTakeRef(PyListObject *self, PyObject *newitem)
{
    return _PyList_AppendTakeRefListResize(self, newitem);
}

/* Array snapshot of the items, valid until the list is next mutated. */
PyAPI_FUNC(PyObject **) _PyQp_ListItems(PyObject *op);
#define _PyList_ITEMS(op) _PyQp_ListItems(_PyObject_CAST(op))

#endif /* !Py_INTERNAL_LIST_H */
