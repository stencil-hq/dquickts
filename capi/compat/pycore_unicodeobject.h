/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_unicodeobject.h used by the vendored
 * modules (capi/modules).  Unit D implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_UNICODEOBJECT_H
#define Py_INTERNAL_UNICODEOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_ucnhash.h"

// Like PyUnicode_AsUTF8(), but check for embedded null characters.
PyAPI_FUNC(const char *) _PyUnicode_AsUTF8NoNUL(PyObject *);

PyAPI_FUNC(int) _PyUnicode_CheckConsistency(
    PyObject *op,
    int check_content);

// Get a copy of a Unicode string.
PyAPI_FUNC(PyObject*) _PyUnicode_Copy(
    PyObject *unicode);

// Fast equality check when the inputs are known to be exact unicode types.
PyAPI_FUNC(int) _PyUnicode_Equal(PyObject *, PyObject *);

// Test whether a unicode is equal to ASCII string.  Return 1 if true,
// 0 otherwise.  The right argument must be ASCII-encoded string.
// Any error occurs inside will be cleared before return.
PyAPI_FUNC(int) _PyUnicode_EqualToASCIIString(
    PyObject *left,
    const char *right           /* ASCII-encoded string */
    );

// Explicit interning routines:
PyAPI_FUNC(void) _PyUnicode_InternMortal(PyInterpreterState *interp, PyObject **);

PyAPI_FUNC(PyObject*) _PyUnicode_JoinArray(
    PyObject *separator,
    PyObject *const *items,
    Py_ssize_t seqlen
    );

#endif /* !Py_INTERNAL_UNICODEOBJECT_H */
