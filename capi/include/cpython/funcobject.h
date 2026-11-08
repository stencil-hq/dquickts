/* Function object interface */

#ifndef Py_LIMITED_API
#ifndef Py_FUNCOBJECT_H
#define Py_FUNCOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


#define _Py_COMMON_FIELDS(PREFIX) \
    PyObject *PREFIX ## globals; \
    PyObject *PREFIX ## builtins; \
    PyObject *PREFIX ## name; \
    PyObject *PREFIX ## qualname; \
    PyObject *PREFIX ## code;        /* A code object, the __code__ attribute */ \
    PyObject *PREFIX ## defaults;    /* NULL or a tuple */ \
    PyObject *PREFIX ## kwdefaults;  /* NULL or a dict */ \
    PyObject *PREFIX ## closure;     /* NULL or a tuple of cell objects */

typedef struct {
    _Py_COMMON_FIELDS(fc_)
} PyFrameConstructor;

/* Function objects and code objects should not be confused with each other:
 *
 * Function objects are created by the execution of the 'def' statement.
 * They reference a code object in their __code__ attribute, which is a
 * purely syntactic object, i.e. nothing more than a compiled version of some
 * source code lines.  There is one code object per source code "fragment",
 * but each code object can be referenced by zero or many function objects
 * depending only on how many times the 'def' statement in the source was
 * executed so far.
 */

/* dquickts (D5): Python functions are shared JS bytecode functions; this
   struct only reserves CPython's size.  Its fields are not maintained and
   are renamed (direct access is a compile error): the PyFunction_GET_*
   accessors below call PyFunction_Get* (borrowed results pinned by the
   function's mirror). */
typedef struct {
    PyObject_HEAD
    _Py_COMMON_FIELDS(_qp_func_)
    PyObject *_qp_func_doc;
    PyObject *_qp_func_dict;
    PyObject *_qp_func_weakreflist;
    PyObject *_qp_func_module;
    PyObject *_qp_func_annotations;
    PyObject *_qp_func_annotate;
    PyObject *_qp_func_typeparams;
    vectorcallfunc _qp_vectorcall;
    uint32_t _qp_func_version;
} PyFunctionObject;

#undef _Py_COMMON_FIELDS

PyAPI_DATA(PyTypeObject) PyFunction_Type;

#define PyFunction_Check(op) Py_IS_TYPE((op), &PyFunction_Type)

PyAPI_FUNC(PyObject *) PyFunction_New(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_NewWithQualName(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetCode(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetGlobals(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetModule(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetDefaults(PyObject *);
PyAPI_FUNC(int) PyFunction_SetDefaults(PyObject *, PyObject *);
PyAPI_FUNC(void) PyFunction_SetVectorcall(PyFunctionObject *, vectorcallfunc);
PyAPI_FUNC(PyObject *) PyFunction_GetKwDefaults(PyObject *);
PyAPI_FUNC(int) PyFunction_SetKwDefaults(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetClosure(PyObject *);
PyAPI_FUNC(int) PyFunction_SetClosure(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetAnnotations(PyObject *);
PyAPI_FUNC(int) PyFunction_SetAnnotations(PyObject *, PyObject *);

#define _PyFunction_CAST(func) \
    (assert(PyFunction_Check(func)), _Py_CAST(PyFunctionObject*, func))

/* Static inline functions for direct access to these values.
   Type checks are *not* done, so use with care. */
static inline PyObject* PyFunction_GET_CODE(PyObject *func) {
    return PyFunction_GetCode(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_CODE(func) PyFunction_GET_CODE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_GLOBALS(PyObject *func) {
    return PyFunction_GetGlobals(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_GLOBALS(func) PyFunction_GET_GLOBALS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_MODULE(PyObject *func) {
    return PyFunction_GetModule(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_MODULE(func) PyFunction_GET_MODULE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_DEFAULTS(PyObject *func) {
    return PyFunction_GetDefaults(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_DEFAULTS(func) PyFunction_GET_DEFAULTS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_KW_DEFAULTS(PyObject *func) {
    return PyFunction_GetKwDefaults(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_KW_DEFAULTS(func) PyFunction_GET_KW_DEFAULTS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_CLOSURE(PyObject *func) {
    return PyFunction_GetClosure(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_CLOSURE(func) PyFunction_GET_CLOSURE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_ANNOTATIONS(PyObject *func) {
    return PyFunction_GetAnnotations(_PyObject_CAST(_PyFunction_CAST(func)));
}
#define PyFunction_GET_ANNOTATIONS(func) PyFunction_GET_ANNOTATIONS(_PyObject_CAST(func))

/* The classmethod and staticmethod types lives here, too */
PyAPI_DATA(PyTypeObject) PyClassMethod_Type;
PyAPI_DATA(PyTypeObject) PyStaticMethod_Type;

PyAPI_FUNC(PyObject *) PyClassMethod_New(PyObject *);
PyAPI_FUNC(PyObject *) PyStaticMethod_New(PyObject *);

#define PY_FOREACH_FUNC_EVENT(V) \
    V(CREATE)                    \
    V(DESTROY)                   \
    V(MODIFY_CODE)               \
    V(MODIFY_DEFAULTS)           \
    V(MODIFY_KWDEFAULTS)         \
    V(MODIFY_QUALNAME)

typedef enum {
    #define _Py_DEF_EVENT(EVENT) PyFunction_EVENT_##EVENT,
    PY_FOREACH_FUNC_EVENT(_Py_DEF_EVENT)
    #undef _Py_DEF_EVENT
} PyFunction_WatchEvent;

/*
 * A callback that is invoked for different events in a function's lifecycle.
 *
 * The callback is invoked with a borrowed reference to func, after it is
 * created and before it is modified or destroyed. The callback should not
 * modify func.
 *
 * When a function's code object, defaults, or kwdefaults are modified the
 * callback will be invoked with the respective event and new_value will
 * contain a borrowed reference to the new value that is about to be stored in
 * the function. Otherwise the third argument is NULL.
 *
 * If the callback returns with an exception set, it must return -1. Otherwise
 * it should return 0.
 */
typedef int (*PyFunction_WatchCallback)(
  PyFunction_WatchEvent event,
  PyFunctionObject *func,
  PyObject *new_value);

/*
 * Register a per-interpreter callback that will be invoked for function lifecycle
 * events.
 *
 * Returns a handle that may be passed to PyFunction_ClearWatcher on success,
 * or -1 and sets an error if no more handles are available.
 */
PyAPI_FUNC(int) PyFunction_AddWatcher(PyFunction_WatchCallback callback);

/*
 * Clear the watcher associated with the watcher_id handle.
 *
 * Returns 0 on success or -1 if no watcher exists for the supplied id.
 */
PyAPI_FUNC(int) PyFunction_ClearWatcher(int watcher_id);

#ifdef __cplusplus
}
#endif
#endif /* !Py_FUNCOBJECT_H */
#endif /* Py_LIMITED_API */
