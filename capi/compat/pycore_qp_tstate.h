/*
 * dquickts C API compat header (plan D5; capi-design.md §3, §5, §8 _asyncio):
 * the private layout of the thread state and of the interpreter state.
 * Shared by the vendored modules (capi/modules, through pycore_pystate.h and
 * pycore_tstate.h) and the runtime: python/capi_runtime.c (unit E2, in the
 * quickjs.c TU) includes it and allocates one of each.  Only the members the
 * modules touch exist; keep the order (E2 relies on it).
 * Owner of this file: unit F.
 */
#ifndef Py_INTERNAL_QP_TSTATE_H
#define Py_INTERNAL_QP_TSTATE_H

#ifdef Py_BUILD_CORE
#  include "pycore_llist.h"         // struct llist_node
#else
/* quickjs.c TU (QP_CAPI_IMPL): CPython's struct llist_node layout. */
struct llist_node {
    struct llist_node *next;
    struct llist_node *prev;
};
#endif

/* _PyThreadStateImpl (CPython pycore_tstate.h): the public PyThreadState
   first, so (_PyThreadStateImpl *)PyThreadState_Get() is valid. */
typedef struct _PyThreadStateImpl {
    PyThreadState base;
    PyObject *asyncio_running_loop;     // strong reference
    PyObject *asyncio_running_task;     // strong reference
    /* Head of the circular list of the tasks (_asyncio TaskObj.task_node)
       created by this thread. */
    struct llist_node asyncio_tasks_head;
} _PyThreadStateImpl;

/* struct _is = PyInterpreterState (CPython pycore_interp_structs.h): the
   members the modules use, then the runtime's own. */
struct _is {
    struct {
        int world_stopped;
    } stoptheworld;
    /* Tasks whose creating thread has exited (_asyncio). */
    struct llist_node asyncio_tasks_head;
    struct {
        /* The thread states of this interpreter, linked by
           PyThreadState.next (one in M3). */
        PyThreadState *head;
    } threads;
    PyObject *qp_dict;                  /* PyInterpreterState_GetDict() */
};

/* Iterate over the thread states of an interpreter (pycore_pystate.h). */
#define _Py_FOR_EACH_TSTATE_UNLOCKED(interp, t) \
    for (PyThreadState *t = (interp)->threads.head; t; t = t->next)
#define _Py_FOR_EACH_TSTATE_BEGIN(interp, t) \
    _Py_FOR_EACH_TSTATE_UNLOCKED(interp, t)
#define _Py_FOR_EACH_TSTATE_END(interp) ((void)(interp))

#endif /* !Py_INTERNAL_QP_TSTATE_H */
