/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_bytesobject.h used by the vendored
 * modules (capi/modules).  Unit D implements the functions;
 * PyBytesWriter is the public 3.15 API (cpython/bytesobject.h).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_BYTESOBJECT_H
#define Py_INTERNAL_BYTESOBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Helper function to implement the repeat and inplace repeat methods on a
// buffer.  len_dest is assumed to be an integer multiple of len_src.
// If src equals dest, then assume the operation is inplace.
PyAPI_FUNC(void)
_PyBytes_Repeat(char* dest, Py_ssize_t len_dest,
    const char* src, Py_ssize_t len_src);

/* Substring Search.

   Returns the index of the first occurrence of
   a substring ("needle") in a larger text ("haystack").
   If the needle is not found, return -1.
   If the needle is found, add offset to the index.
   (python/capi_stable.c; mmap)
*/

PyAPI_FUNC(Py_ssize_t)
_PyBytes_Find(const char *haystack, Py_ssize_t len_haystack,
              const char *needle, Py_ssize_t len_needle,
              Py_ssize_t offset);

/* Same as above, but search right-to-left */
PyAPI_FUNC(Py_ssize_t)
_PyBytes_ReverseFind(const char *haystack, Py_ssize_t len_haystack,
                     const char *needle, Py_ssize_t len_needle,
                     Py_ssize_t offset);

/* --- PyBytesWriter ------------------------------------------------------ */

/* CPython's layout, kept by python/capi_bytes.c */
struct PyBytesWriter {
    char small_buffer[256];
    PyObject *obj;
    Py_ssize_t size;
    int use_bytearray;
    int overallocate;
};

// Export for '_testcapi' shared extension
PyAPI_FUNC(PyBytesWriter*) _PyBytesWriter_CreateByteArray(Py_ssize_t size);

#endif /* !Py_INTERNAL_BYTESOBJECT_H */
