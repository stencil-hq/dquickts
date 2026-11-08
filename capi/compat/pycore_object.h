/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_object.h used by the vendored
 * modules (capi/modules).  Allocation, reference counts and GC are
 * the runtime's (units A and B, capi-design.md §1): the inline helpers that
 * would touch CPython's object internals call the public functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_OBJECT_H
#define Py_INTERNAL_OBJECT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_pymem.h"
#include "pycore_stats.h"
#include "pycore_gc.h"
#include "pycore_pyatomic_ft_wrappers.h"
#include "pycore_pystate.h"
#include "pycore_runtime.h"
#include "pycore_typeobject.h"

#include <stdbool.h>

// Export these 2 symbols for '_pickle' shared extension
PyAPI_DATA(PyTypeObject) _PyNone_Type;
PyAPI_DATA(PyTypeObject) _PyNotImplemented_Type;

// Fast inlined version of PyType_HasFeature()
static inline int
_PyType_HasFeature(PyTypeObject *type, unsigned long feature) {
    return ((type->tp_flags) & feature) != 0;
}

// Checks if an object has a single, unique reference. If the caller holds a
// unique reference, it may be able to safely modify the object in-place.
// Py_REFCNT counts the JS references of bound objects (capi-design.md §1.7).
// A unique bound tuple drops its JS object, so that items written in place
// through _PyTuple_ITEMS (functools.reduce) are seen at the next crossing.
PyAPI_FUNC(void) _PyQp_Unshare(PyObject *ob);

static inline int
_PyObject_IsUniquelyReferenced(PyObject *ob)
{
    if (Py_REFCNT(ob) != 1)
        return 0;
    if (_PyQp_HasFlag(ob, _Py_QP_BOUND_FLAG))
        _PyQp_Unshare(ob);
    return 1;
}

/* _PyObject_Init() is the fast version of PyObject_Init(): here the same
   function, which turns a PyObject_Malloc() block into an object (A). */
static inline void
_PyObject_Init(PyObject *op, PyTypeObject *typeobj)
{
    (void)PyObject_Init(op, typeobj);
}

static inline void
_PyObject_InitVar(PyVarObject *op, PyTypeObject *typeobj, Py_ssize_t size)
{
    (void)PyObject_InitVar(op, typeobj, size);
}

/* Free-threading only in CPython: nothing to do in the GIL build. */
static inline void
_PyObject_SetMaybeWeakref(PyObject *op)
{
    (void)op;
}

/* Tries to incref op and returns 1 if successful or 0 otherwise. */
static inline int
_Py_TryIncref(PyObject *op)
{
    if (Py_REFCNT(op) > 0) {
        Py_INCREF(op);
        return 1;
    }
    return 0;
}

// Fast inlined version of PyObject_Hash()
static inline Py_hash_t
_PyObject_HashFast(PyObject *op)
{
    if (PyUnicode_CheckExact(op)) {
        Py_hash_t hash = PyUnstable_Unicode_GET_CACHED_HASH(op);
        if (hash != -1) {
            return hash;
        }
    }
    return PyObject_Hash(op);
}

PyAPI_FUNC(PyObject*) _PyType_AllocNoTrack(PyTypeObject *type, Py_ssize_t nitems);

// Calls the method named `attr` on `self`, but does not set an exception if
// the attribute does not exist.
PyAPI_FUNC(PyObject *)
_PyObject_MaybeCallSpecialNoArgs(PyObject *self, PyObject *attr);

// Pickle support.
PyAPI_FUNC(PyObject*) _PyObject_GetState(PyObject *);

/* Utility for the tp_traverse slot of mutable heap types that have no other
 * references. */
PyAPI_FUNC(int) _PyObject_VisitType(PyObject *op, visitproc visit, void *arg);

#endif /* !Py_INTERNAL_OBJECT_H */
