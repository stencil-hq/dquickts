#ifndef Py_CPYTHON_LISTOBJECT_H
#  error "this header file must not be included directly"
#endif

/* dquickts (D5): a list is always the shared JS Array; PyListObject is
   only the C-side handle.  Its length is live (_Py_QP_LIVE_SIZE_FLAG:
   Py_SIZE()/Py_SET_SIZE() call into the runtime) and items are reached
   through calls.  PyList_GET_ITEM() returns a borrowed mirror that stays
   valid while the list mirror lives and the slot still holds that value
   (CPython's rule).  ob_item/allocated do not exist: code touching them
   must use PyList_GET_ITEM/PyList_SET_ITEM/_PyList_ITEMS (patched in the
   vendored modules, see capi/modules/PATCHES). */
typedef struct {
    PyObject_VAR_HEAD
    /* private to the runtime (borrowed-item cache) */
    PyObject **_qp_items;
    Py_ssize_t _qp_nitems;
} PyListObject;

/* Cast argument to PyListObject* type. */
#define _PyList_CAST(op) \
    (assert(PyList_Check(op)), _Py_CAST(PyListObject*, (op)))

PyAPI_FUNC(PyObject *) _PyQp_ListGetItem(PyObject *op, Py_ssize_t index);
PyAPI_FUNC(void) _PyQp_ListSetItem(PyObject *op, Py_ssize_t index, PyObject *value);

// Macros and static inline functions, trading safety for speed

static inline Py_ssize_t PyList_GET_SIZE(PyObject *op) {
    PyListObject *list = _PyList_CAST(op);
    return Py_SIZE(list);
}
#define PyList_GET_SIZE(op) PyList_GET_SIZE(_PyObject_CAST(op))

/* borrowed reference */
#define PyList_GET_ITEM(op, index) _PyQp_ListGetItem(_PyObject_CAST(op), (index))

/* Steals a reference to value: it becomes the list mirror's borrowed-item
   pin of that slot, so `value` stays valid while the list holds it.  As in
   CPython it is meant for filling new lists (PyList_New(n) slots start as
   NULL-equivalent placeholders). */
static inline void
PyList_SET_ITEM(PyObject *op, Py_ssize_t index, PyObject *value) {
    assert(0 <= index);
    _PyQp_ListSetItem(op, index, value);
}
#define PyList_SET_ITEM(op, index, value) \
    PyList_SET_ITEM(_PyObject_CAST(op), (index), _PyObject_CAST(value))

PyAPI_FUNC(int) PyList_Extend(PyObject *self, PyObject *iterable);
PyAPI_FUNC(int) PyList_Clear(PyObject *self);
