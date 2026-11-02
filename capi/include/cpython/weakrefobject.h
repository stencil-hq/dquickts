#ifndef Py_CPYTHON_WEAKREFOBJECT_H
#  error "this header file must not be included directly"
#endif

/* PyWeakReference is the base struct for the Python ReferenceType, ProxyType,
 * and CallableProxyType.
 */
/* dquickts (D5): weak references are the runtime's _weakref objects (over
   QuickJS weak references); the struct only reserves CPython's size and its
   fields are renamed (not maintained): use PyWeakref_GetRef(). */
struct _PyWeakReference {
    PyObject_HEAD
    PyObject *_qp_wr_object;
    PyObject *_qp_wr_callback;
    Py_hash_t _qp_hash;
    PyWeakReference *_qp_wr_prev;
    PyWeakReference *_qp_wr_next;
    vectorcallfunc _qp_vectorcall;
};

PyAPI_FUNC(void) _PyWeakref_ClearRef(PyWeakReference *self);

#define _PyWeakref_CAST(op) \
    (assert(PyWeakref_Check(op)), _Py_CAST(PyWeakReference*, (op)))

// Test if a weak reference is dead.
PyAPI_FUNC(int) PyWeakref_IsDead(PyObject *ref);
