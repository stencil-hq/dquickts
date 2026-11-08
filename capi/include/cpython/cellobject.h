/* Cell object interface */

#ifndef Py_LIMITED_API
#ifndef Py_CELLOBJECT_H
#define Py_CELLOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PyObject_HEAD
    /* dquickts (D5): cells are shared JS cells; the content is reached
       through PyCell_Get/PyCell_Set (field renamed, not maintained) */
    PyObject *_qp_ob_ref;
} PyCellObject;

PyAPI_DATA(PyTypeObject) PyCell_Type;

#define PyCell_Check(op) Py_IS_TYPE((op), &PyCell_Type)

PyAPI_FUNC(PyObject *) PyCell_New(PyObject *);
PyAPI_FUNC(PyObject *) PyCell_Get(PyObject *);
PyAPI_FUNC(int) PyCell_Set(PyObject *, PyObject *);

/* dquickts: borrowed (pinned by the cell's mirror until the cell changes) */
PyAPI_FUNC(PyObject *) _PyQp_CellGetBorrowed(PyObject *op);
static inline PyObject* PyCell_GET(PyObject *op) {
    assert(PyCell_Check(op));
    return _PyQp_CellGetBorrowed(op);
}
#define PyCell_GET(op) PyCell_GET(_PyObject_CAST(op))

/* dquickts: CPython's PyCell_SET steals `value` and does not release the
   previous content; the runtime keeps that contract. */
PyAPI_FUNC(void) _PyQp_CellSetSteal(PyObject *op, PyObject *value);
static inline void PyCell_SET(PyObject *op, PyObject *value) {
    assert(PyCell_Check(op));
    _PyQp_CellSetSteal(op, value);
}
#define PyCell_SET(op, value) PyCell_SET(_PyObject_CAST(op), (value))

#ifdef __cplusplus
}
#endif
#endif /* !Py_TUPLEOBJECT_H */
#endif /* Py_LIMITED_API */
