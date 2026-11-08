/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_tuple.h used by the vendored
 * modules (capi/modules).  Unit E1 implements the functions; the
 * tuple layout is CPython's (ob_item materialized, capi-design.md §1.4).
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_TUPLE_H
#define Py_INTERNAL_TUPLE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_object.h"

#define _PyTuple_ITEMS(op) _Py_RVALUE(_PyTuple_CAST(op)->ob_item)

PyAPI_FUNC(PyObject *) _PyTuple_FromPair(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyTuple_FromPairSteal(PyObject *, PyObject *);

#define _PyTuple_RESET_HASH_CACHE(op)       \
    do {                                    \
        assert(op != NULL);                 \
        _PyTuple_CAST(op)->ob_hash = -1;    \
    } while (0)

/* bpo-42536: If reusing a tuple object, this should be called to re-track it
   with the garbage collector and reset its hash cache.  Tuples are GC nodes
   of the runtime while referenced from C (capi-design.md §1.6): only the
   hash cache needs a reset. */
static inline void
_PyTuple_Recycle(PyObject *op)
{
    _PyTuple_RESET_HASH_CACHE(op);
}

#endif /* !Py_INTERNAL_TUPLE_H */
