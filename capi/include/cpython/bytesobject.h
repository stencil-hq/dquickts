#ifndef Py_CPYTHON_BYTESOBJECT_H
#  error "this header file must not be included directly"
#endif

/* dquickts (D5): bytes are immutable JS objects over an ArrayBuffer; the
   mirror points at that storage (zero copy) instead of embedding ob_sval.
   The pointer is stable for the life of the object, NUL-terminated
   (data[ob_size] == 0) and writable only for bytes created by C code that
   have not crossed into Python yet (PyBytes_FromStringAndSize(NULL, n)).
   sizeof(PyBytesObject) equals CPython's on 64-bit hosts. */
typedef struct {
    PyObject_VAR_HEAD
    Py_DEPRECATED(3.11) Py_hash_t ob_shash;
    char *_qp_sval;
} PyBytesObject;

PyAPI_FUNC(int) _PyBytes_Resize(PyObject **, Py_ssize_t);

/* Macros and static inline functions, trading safety for speed */
#define _PyBytes_CAST(op) \
    (assert(PyBytes_Check(op)), _Py_CAST(PyBytesObject*, op))

static inline char* PyBytes_AS_STRING(PyObject *op)
{
    return _PyBytes_CAST(op)->_qp_sval;
}
#define PyBytes_AS_STRING(op) PyBytes_AS_STRING(_PyObject_CAST(op))

static inline Py_ssize_t PyBytes_GET_SIZE(PyObject *op) {
    PyBytesObject *self = _PyBytes_CAST(op);
    return Py_SIZE(self);
}
#define PyBytes_GET_SIZE(self) PyBytes_GET_SIZE(_PyObject_CAST(self))

PyAPI_FUNC(PyObject*) PyBytes_Join(PyObject *sep, PyObject *iterable);

// Deprecated alias kept for backward compatibility
Py_DEPRECATED(3.14) static inline PyObject*
_PyBytes_Join(PyObject *sep, PyObject *iterable)
{
    return PyBytes_Join(sep, iterable);
}


// --- PyBytesWriter API -----------------------------------------------------

typedef struct PyBytesWriter PyBytesWriter;

PyAPI_FUNC(PyBytesWriter *) PyBytesWriter_Create(
    Py_ssize_t size);
PyAPI_FUNC(void) PyBytesWriter_Discard(
    PyBytesWriter *writer);
PyAPI_FUNC(PyObject*) PyBytesWriter_Finish(
    PyBytesWriter *writer);
PyAPI_FUNC(PyObject*) PyBytesWriter_FinishWithSize(
    PyBytesWriter *writer,
    Py_ssize_t size);
PyAPI_FUNC(PyObject*) PyBytesWriter_FinishWithPointer(
    PyBytesWriter *writer,
    void *buf);

PyAPI_FUNC(void*) PyBytesWriter_GetData(
    PyBytesWriter *writer);
PyAPI_FUNC(Py_ssize_t) PyBytesWriter_GetSize(
    PyBytesWriter *writer);

PyAPI_FUNC(int) PyBytesWriter_WriteBytes(
    PyBytesWriter *writer,
    const void *bytes,
    Py_ssize_t size);
PyAPI_FUNC(int) PyBytesWriter_Format(
    PyBytesWriter *writer,
    const char *format,
    ...);

PyAPI_FUNC(int) PyBytesWriter_Resize(
    PyBytesWriter *writer,
    Py_ssize_t size);
PyAPI_FUNC(int) PyBytesWriter_Grow(
    PyBytesWriter *writer,
    Py_ssize_t size);
PyAPI_FUNC(void*) PyBytesWriter_GrowAndUpdatePointer(
    PyBytesWriter *writer,
    Py_ssize_t size,
    void *buf);
