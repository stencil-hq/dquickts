#ifndef Py_CPYTHON_BYTEARRAYOBJECT_H
#  error "this header file must not be included directly"
#endif

/* Object layout.
   dquickts (D5): a bytearray is the shared JS object over a resizable
   ArrayBuffer.  Its storage moves when Python code resizes it, so the data
   pointer and the size (_Py_QP_LIVE_SIZE_FLAG) are read through calls; the
   CPython fields are private placeholders (same sizeof as CPython). */
typedef struct {
    PyObject_VAR_HEAD
    Py_ssize_t _qp_private[5];
} PyByteArrayObject;

PyAPI_FUNC(char *) _PyQp_ByteArrayData(PyObject *op);

/* Macros and static inline functions, trading safety for speed */
#define _PyByteArray_CAST(op) \
    (assert(PyByteArray_Check(op)), _Py_CAST(PyByteArrayObject*, op))

static inline char* PyByteArray_AS_STRING(PyObject *op)
{
    return _PyQp_ByteArrayData(_PyObject_CAST(_PyByteArray_CAST(op)));
}
#define PyByteArray_AS_STRING(self) PyByteArray_AS_STRING(_PyObject_CAST(self))

static inline Py_ssize_t PyByteArray_GET_SIZE(PyObject *op) {
    PyByteArrayObject *self = _PyByteArray_CAST(op);
#ifdef Py_GIL_DISABLED
    return _Py_atomic_load_ssize_relaxed(&(_PyVarObject_CAST(self)->ob_size));
#else
    return Py_SIZE(self);
#endif
}
#define PyByteArray_GET_SIZE(self) PyByteArray_GET_SIZE(_PyObject_CAST(self))
