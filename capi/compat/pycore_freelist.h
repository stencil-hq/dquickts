/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_freelist.h used by the vendored
 * modules (capi/modules).  Freelists are disabled (capi-design.md
 * §6 F): POP returns NULL, PUSH refuses (the caller frees the object), FREE
 * frees at once.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_FREELIST_H
#define Py_INTERNAL_FREELIST_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_stats.h"
#include "pycore_freelist_state.h"
#include "pycore_pystate.h"

// Pushes `op` to the freelist, calls `freefunc` if the freelist is full
#define _Py_FREELIST_FREE(NAME, op, freefunc) \
    ((void)(Py_ ## NAME ## _MAXFREELIST), (freefunc)(op))
// Pushes `op` to the freelist, returns 1 if successful, 0 if the freelist is full
#define _Py_FREELIST_PUSH(NAME, op, limit) \
    ((void)(op), (void)(limit), 0)
// Pops a PyObject from the freelist, returns NULL if the freelist is empty.
#define _Py_FREELIST_POP(TYPE, NAME) \
    _Py_CAST(TYPE*, NULL)
// Pops a non-PyObject data structure from the freelist, returns NULL if the
// freelist is empty.
#define _Py_FREELIST_POP_MEM(NAME) ((void *)NULL)
#define _Py_FREELIST_SIZE(NAME) 0

#endif /* !Py_INTERNAL_FREELIST_H */
