#ifndef Py_CPYTHON_ERRORS_H
#  error "this header file must not be included directly"
#endif

/* Error objects */

/* PyException_HEAD defines the initial segment of every exception class. */
/* dquickts (D5): exceptions are shared JS PY_EXCEPTION objects; these
   structs only give C subtypes their sizes (tp_basicsize).  The fields are
   not maintained by the runtime and are renamed so that direct access is a
   compile error: use PyException_GetArgs/GetTraceback/GetCause/GetContext,
   PyObject_GetAttr (e.g. "strerror") or the PyUnicode*Error_* accessors. */
#define PyException_HEAD PyObject_HEAD PyObject *_qp_dict;\
             PyObject *_qp_args; PyObject *_qp_notes; PyObject *_qp_traceback;\
             PyObject *_qp_context; PyObject *_qp_cause;\
             char _qp_suppress_context;

typedef struct {
    PyException_HEAD
} PyBaseExceptionObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_msg;
    PyObject *_qp_excs;
    PyObject *_qp_excs_str;
} PyBaseExceptionGroupObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_msg;
    PyObject *_qp_filename;
    PyObject *_qp_lineno;
    PyObject *_qp_offset;
    PyObject *_qp_end_lineno;
    PyObject *_qp_end_offset;
    PyObject *_qp_text;
    PyObject *_qp_print_file_and_line;
    PyObject *_qp_metadata;
} PySyntaxErrorObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_msg;
    PyObject *_qp_name;
    PyObject *_qp_path;
    PyObject *_qp_name_from;
} PyImportErrorObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_encoding;
    PyObject *_qp_object;
    Py_ssize_t _qp_start;
    Py_ssize_t _qp_end;
    PyObject *_qp_reason;
} PyUnicodeErrorObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_code;
} PySystemExitObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_myerrno;
    PyObject *_qp_strerror;
    PyObject *_qp_filename;
    PyObject *_qp_filename2;
#ifdef MS_WINDOWS
    PyObject *_qp_winerror;
#endif
    Py_ssize_t _qp_written;   /* only for BlockingIOError, -1 otherwise */
} PyOSErrorObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_value;
} PyStopIterationObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_name;
} PyNameErrorObject;

typedef struct {
    PyException_HEAD
    PyObject *_qp_obj;
    PyObject *_qp_name;
} PyAttributeErrorObject;

/* Compatibility typedefs */
typedef PyOSErrorObject PyEnvironmentErrorObject;
#ifdef MS_WINDOWS
typedef PyOSErrorObject PyWindowsErrorObject;
#endif

/* Context manipulation (PEP 3134) */

PyAPI_FUNC(void) _PyErr_ChainExceptions1(PyObject *);

/* In exceptions.c */

PyAPI_FUNC(PyObject*) PyUnstable_Exc_PrepReraiseStar(
     PyObject *orig,
     PyObject *excs);

/* In signalmodule.c */

PyAPI_FUNC(int) PySignal_SetWakeupFd(int fd);

/* Support for adding program text to SyntaxErrors */

PyAPI_FUNC(void) PyErr_SyntaxLocationObject(
    PyObject *filename,
    int lineno,
    int col_offset);

PyAPI_FUNC(void) PyErr_RangedSyntaxLocationObject(
    PyObject *filename,
    int lineno,
    int col_offset,
    int end_lineno,
    int end_col_offset);

PyAPI_FUNC(PyObject *) PyErr_ProgramTextObject(
    PyObject *filename,
    int lineno);

PyAPI_FUNC(void) _Py_NO_RETURN _Py_FatalErrorFunc(
    const char *func,
    const char *message);

PyAPI_FUNC(void) PyErr_FormatUnraisable(const char *, ...);

PyAPI_DATA(PyObject *) PyExc_PythonFinalizationError;

#define Py_FatalError(message) _Py_FatalErrorFunc(__func__, (message))
