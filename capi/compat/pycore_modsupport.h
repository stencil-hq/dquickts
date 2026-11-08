/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_modsupport.h used by the vendored
 * modules (capi/modules).  Argument Clinic helpers; unit E2
 * implements the functions (python/capi_getargs.c).  _PyArg_Parser is the
 * public struct of cpython/modsupport.h.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_MODSUPPORT_H
#define Py_INTERNAL_MODSUPPORT_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyAPI_FUNC(int) _PyArg_NoKwnames(const char *funcname, PyObject *kwnames);
#define _PyArg_NoKwnames(funcname, kwnames) \
    ((kwnames) == NULL || _PyArg_NoKwnames((funcname), (kwnames)))

PyAPI_FUNC(int) _PyArg_NoPositional(const char *funcname, PyObject *args);
#define _PyArg_NoPositional(funcname, args) \
    ((args) == NULL || _PyArg_NoPositional((funcname), (args)))

PyAPI_FUNC(int) _PyArg_NoKeywords(const char *funcname, PyObject *kwargs);
#define _PyArg_NoKeywords(funcname, kwargs) \
    ((kwargs) == NULL || _PyArg_NoKeywords((funcname), (kwargs)))

PyAPI_FUNC(int) _PyArg_CheckPositional(const char *, Py_ssize_t,
                                       Py_ssize_t, Py_ssize_t);
#define _PyArg_CheckPositional(funcname, nargs, min, max) \
    (((min) <= (nargs) && (nargs) <= (max)) \
     || _PyArg_CheckPositional((funcname), (nargs), (min), (max)))

PyAPI_FUNC(void) _PyArg_BadArgument(
    const char *fname,
    const char *displayname,
    const char *expected,
    PyObject *arg);

// --- _PyArg_Parser API ---------------------------------------------------
// _dbm, _multiprocessing (clinic "format" converters)
PyAPI_FUNC(int) _PyArg_ParseStack(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *format,
    ...);
PyAPI_FUNC(int) _PyArg_ParseStackAndKeywords(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    struct _PyArg_Parser *,
    ...);


PyAPI_FUNC(PyObject * const *) _PyArg_UnpackKeywords(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwargs,
    PyObject *kwnames,
    struct _PyArg_Parser *parser,
    int minpos,
    int maxpos,
    int minkw,
    int varpos,
    PyObject **buf);
#define _PyArg_UnpackKeywords(args, nargs, kwargs, kwnames, parser, minpos, maxpos, minkw, varpos, buf) \
    (((minkw) == 0 && (kwargs) == NULL && (kwnames) == NULL && \
      (minpos) <= (nargs) && ((varpos) || (nargs) <= (maxpos)) && (args) != NULL) ? \
      (args) : \
     _PyArg_UnpackKeywords((args), (nargs), (kwargs), (kwnames), (parser), \
                           (minpos), (maxpos), (minkw), (varpos), (buf)))

#endif /* !Py_INTERNAL_MODSUPPORT_H */
