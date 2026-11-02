/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_tracemalloc.h used by the
 * vendored _tracemalloc module.
 *
 * The functions are python/capi_tracemalloc.c, a port of CPython's
 * Python/tracemalloc.c over the runtime's allocators (js_malloc_rt and co.,
 * PyMem_*) and its Python frames.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_TRACEMALLOC_H
#define Py_INTERNAL_TRACEMALLOC_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* Is tracemalloc tracing memory allocations? */
PyAPI_FUNC(int) _PyTraceMalloc_IsTracing(void);

/* Clear the tracemalloc traces */
PyAPI_FUNC(void) _PyTraceMalloc_ClearTraces(void);

/* The traces: a list of (domain, size, traceback) tuples */
PyAPI_FUNC(PyObject *) _PyTraceMalloc_GetTraces(void);

/* The traceback of the allocation of obj, or None */
PyAPI_FUNC(PyObject *) _PyTraceMalloc_GetObjectTraceback(PyObject *obj);

/* Start tracemalloc */
PyAPI_FUNC(int) _PyTraceMalloc_Start(int max_nframe);

/* Stop tracemalloc */
PyAPI_FUNC(void) _PyTraceMalloc_Stop(void);

/* Get the tracemalloc traceback limit */
PyAPI_FUNC(int) _PyTraceMalloc_GetTracebackLimit(void);

/* Get the memory usage of tracemalloc in bytes */
PyAPI_FUNC(size_t) _PyTraceMalloc_GetMemory(void);

/* Get the current size and peak size of traced memory blocks as a 2-tuple */
PyAPI_FUNC(PyObject *) _PyTraceMalloc_GetTracedMemory(void);

/* Set the peak size of traced memory blocks to the current size */
PyAPI_FUNC(void) _PyTraceMalloc_ResetPeak(void);

#endif /* !Py_INTERNAL_TRACEMALLOC_H */
