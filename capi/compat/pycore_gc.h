/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_gc.h used by the vendored
 * modules (capi/modules).  GC tracking is the runtime's (unit A,
 * capi-design.md §1.6): the macros call the public functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#define _PyObject_GC_IS_TRACKED(op) PyObject_GC_IsTracked(_PyObject_CAST(op))
#define _PyObject_GC_TRACK(op) PyObject_GC_Track(op)
#define _PyObject_GC_UNTRACK(op) PyObject_GC_UnTrack(op)

#endif /* !Py_INTERNAL_GC_H */
