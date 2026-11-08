/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_pyerrors.h used by the vendored
 * modules (capi/modules).  Unit E1 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_PYERRORS_H
#define Py_INTERNAL_PYERRORS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Like PyErr_Format(), but saves current exception as __context__ and
// __cause__.
PyAPI_FUNC(PyObject*) _PyErr_FormatFromCause(
    PyObject *exception,
    const char *format,   /* ASCII-encoded string  */
    ...
    );

PyAPI_FUNC(void) _PyErr_FormatNote(const char *format, ...);

// Like PyErr_SetString(), but the message is decoded from the locale
// encoding (surrogateescape).
PyAPI_FUNC(void) _PyErr_SetLocaleString(
    PyObject *exception,
    const char *string);

PyAPI_FUNC(int) _PyException_AddNote(
     PyObject *exc,
     PyObject *note);

#endif /* !Py_INTERNAL_PYERRORS_H */
