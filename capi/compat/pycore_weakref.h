/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_weakref.h used by the vendored
 * modules (capi/modules).  Weak references live on the JS object
 * (capi-design.md §1.5): PyObject_ClearWeakRefs() (unit E1) is called
 * unconditionally, since the native weakref list field stays NULL.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_WEAKREF_H
#define Py_INTERNAL_WEAKREF_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_critical_section.h"
#include "pycore_lock.h"
#include "pycore_object.h"
#include "pycore_pyatomic_ft_wrappers.h"

#define FT_CLEAR_WEAKREFS(obj, weakref_list)    \
    do {                                        \
        (void)(weakref_list);                   \
        PyObject_ClearWeakRefs(obj);            \
    } while (0)

#endif /* !Py_INTERNAL_WEAKREF_H */
