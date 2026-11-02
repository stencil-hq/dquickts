/*
 * dquickts Python C API (plan D5 Tier B, M5): the rest of the stable ABI.
 *
 * Every function and data symbol of CPython 3.15 Misc/stable_abi.toml
 * (abi3 and abi3t, including the `abi_only` entries kept for old
 * binaries) that the other capi_*.c units do not define, so any abi3
 * extension links against the qpy executable: file objects, the legacy
 * import and sys entry points, the deprecated buffer and call functions,
 * module-by-index state (PyState_*), the embedding API (Py_Initialize,
 * Py_FinalizeEx, Py_BytesMain, path getters/setters, Py_Decode/
 * EncodeLocale), version strings, async exceptions and the PEP 788
 * interpreter guards/views.  Windows-only entries (MBCS, code pages,
 * PyExc_WindowsError), PyOS_CheckStack (USE_STACKCHECK) and _Py_RefTotal
 * (Py_REF_DEBUG) do not apply to this build.
 *
 * Ported from CPython 3.15 Objects/fileobject.c, Objects/abstract.c,
 * Objects/object.c, Objects/call.c, Objects/unicodeobject.c,
 * Python/import.c, Python/sysmodule.c, Python/ceval.c, Python/ceval_gil.c,
 * Python/pystate.c, Python/pylifecycle.c, Python/getversion.c,
 * Python/getplatform.c, Python/getcompiler.c, Python/getcopyright.c,
 * Python/fileutils.c, Python/modsupport.c and Python/marshal.c (PSF-2.0).
 *
 * Deviations (capi-design.md §9):
 *  - PyEval_EvalFrame/EvalFrameEx raise SystemError: frame objects are
 *    views of QuickJS stack frames and cannot be re-entered.
 *  - Py_InitializeEx in a process without Python creates a runtime and a
 *    context with JS_InitPython; Py_FinalizeEx releases them.  The C API
 *    is bound to one context per process, so initializing again after
 *    Py_FinalizeEx is a fatal error.  Inside qpy (already initialized)
 *    Py_FinalizeEx runs the atexit callbacks and flushes the standard
 *    streams, like the first steps of CPython's finalization.
 *  - Py_BytesMain/Py_Main run the qpy command line (qp_main) in a new
 *    runtime.
 *  - PyImport_GetMagicTag returns "quickpy-315" although
 *    sys.implementation.cache_tag is None (no bytecode caches).
 */

#include <langinfo.h>
#include <wchar.h>

#define QP_CAPI_STABLE_NO_TSTATE \
    "the function must be called with the GIL held, but the current Python thread state is NULL"

/* ========================================================================= */
/* Data                                                                       */
/* ========================================================================= */

int (*PyOS_InputHook)(void) = NULL;
const char *Py_FileSystemDefaultEncoding = "utf-8";
const char *Py_FileSystemDefaultEncodeErrors = "surrogateescape";
int Py_HasFileSystemDefaultEncoding = 1;
int Py_UTF8Mode = 0;
/* the deprecated global configuration variables (cpython/pydebug.h,
   Python/initconfig.c config_set_global_vars): set from the configuration
   at init, read by extensions and ctypes (c_int.in_dll(pythonapi, ...)) */
int Py_DebugFlag = 0;
int Py_VerboseFlag = 0;
int Py_QuietFlag = 0;
int Py_InteractiveFlag = 0;
int Py_InspectFlag = 0;
int Py_OptimizeFlag = 0;
int Py_NoSiteFlag = 0;
int Py_BytesWarningFlag = 0;
int Py_FrozenFlag = 0;
int Py_IgnoreEnvironmentFlag = 0;
int Py_DontWriteBytecodeFlag = 0;
int Py_NoUserSiteDirectory = 0;
int Py_UnbufferedStdioFlag = 0;
int Py_HashRandomizationFlag = 0;
int Py_IsolatedFlag = 0;
/* abi_only: Objects/object.c */
PyAPI_DATA(int) _Py_SwappedOp[];
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };

/* ========================================================================= */
/* File objects (fileobject.h)                                                */
/* ========================================================================= */

PyObject *PyFile_FromFd(int fd, const char *name, const char *mode, int buffering,
                        const char *encoding, const char *errors, const char *newline,
                        int closefd)
{
    PyObject *open, *stream;

    open = PyImport_ImportModuleAttrString("_io", "open");
    if (!open)
        return NULL;
    stream = PyObject_CallFunction(open, "isisssO", fd, mode, buffering, encoding, errors,
                                   newline, closefd ? Py_True : Py_False);
    Py_DECREF(open);
    /* name is ignored: the name attribute of the stream is read-only */
    return stream;
}

PyObject *PyFile_GetLine(PyObject *f, int n)
{
    PyObject *result;

    if (f == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (n <= 0)
        result = PyObject_CallMethod(f, "readline", NULL);
    else
        result = PyObject_CallMethod(f, "readline", "i", n);
    if (result && !PyBytes_Check(result) && !PyUnicode_Check(result)) {
        Py_SETREF(result, NULL);
        PyErr_SetString(PyExc_TypeError, "object.readline() returned non-string");
    }
    if (n < 0 && result && PyBytes_Check(result)) {
        const char *s = PyBytes_AS_STRING(result);
        Py_ssize_t len = PyBytes_GET_SIZE(result);
        if (len == 0) {
            Py_SETREF(result, NULL);
            PyErr_SetString(PyExc_EOFError, "EOF when reading a line");
        } else if (s[len - 1] == '\n') {
            Py_SETREF(result, PyBytes_FromStringAndSize(s, len - 1));
        }
    }
    if (n < 0 && result && PyUnicode_Check(result)) {
        Py_ssize_t len = PyUnicode_GetLength(result);
        if (len == 0) {
            Py_SETREF(result, NULL);
            PyErr_SetString(PyExc_EOFError, "EOF when reading a line");
        } else if (PyUnicode_ReadChar(result, len - 1) == '\n') {
            Py_SETREF(result, PyUnicode_Substring(result, 0, len - 1));
        }
    }
    return result;
}

int PyFile_WriteObject(PyObject *v, PyObject *f, int flags)
{
    PyObject *writer, *value, *result;

    if (f == NULL) {
        PyErr_SetString(PyExc_TypeError, "writeobject with NULL file");
        return -1;
    }
    writer = PyObject_GetAttrString(f, "write");
    if (!writer)
        return -1;
    value = (flags & Py_PRINT_RAW) ? PyObject_Str(v) : PyObject_Repr(v);
    if (!value) {
        Py_DECREF(writer);
        return -1;
    }
    result = PyObject_CallOneArg(writer, value);
    Py_DECREF(value);
    Py_DECREF(writer);
    if (!result)
        return -1;
    Py_DECREF(result);
    return 0;
}

int PyFile_WriteString(const char *s, PyObject *f)
{
    PyObject *v;
    int err;

    if (f == NULL) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError, "null file for PyFile_WriteString");
        return -1;
    }
    if (PyErr_Occurred())
        return -1;
    v = PyUnicode_FromString(s);
    if (!v)
        return -1;
    err = PyFile_WriteObject(v, f, Py_PRINT_RAW);
    Py_DECREF(v);
    return err;
}

int PyObject_AsFileDescriptor(PyObject *o)
{
    PyObject *meth;
    int fd;

    if (PyLong_Check(o)) {
        if (PyBool_Check(o) &&
            PyErr_WarnEx(PyExc_RuntimeWarning, "bool is used as a file descriptor", 1))
            return -1;
        fd = PyLong_AsInt(o);
    } else if (PyObject_GetOptionalAttrString(o, "fileno", &meth) < 0) {
        return -1;
    } else if (meth) {
        PyObject *fno = PyObject_CallNoArgs(meth);
        Py_DECREF(meth);
        if (!fno)
            return -1;
        if (!PyLong_Check(fno)) {
            PyErr_Format(PyExc_TypeError, "%T.fileno() must return an int, not %T", o, fno);
            Py_DECREF(fno);
            return -1;
        }
        fd = PyLong_AsInt(fno);
        Py_DECREF(fno);
    } else {
        PyErr_SetString(PyExc_TypeError, "argument must be an int, or have a fileno() method.");
        return -1;
    }
    if (fd == -1 && PyErr_Occurred())
        return -1;
    if (fd < 0) {
        PyErr_Format(PyExc_ValueError, "file descriptor cannot be a negative integer (%i)", fd);
        return -1;
    }
    return fd;
}

/* ========================================================================= */
/* Objects, calls, buffers (abi_only legacy entries)                          */
/* ========================================================================= */

PyObject *PyObject_Bytes(PyObject *v)
{
    PyObject *result, *func, *name;

    if (v == NULL)
        return PyBytes_FromString("<NULL>");
    if (PyBytes_CheckExact(v))
        return Py_NewRef(v);
    name = PyUnicode_InternFromString("__bytes__");
    if (!name)
        return NULL;
    func = _PyObject_LookupSpecial(v, name);
    Py_DECREF(name);
    if (func) {
        result = PyObject_CallNoArgs(func);
        Py_DECREF(func);
        if (!result)
            return NULL;
        if (!PyBytes_Check(result)) {
            PyErr_Format(PyExc_TypeError, "%T.__bytes__() must return a bytes, not %T", v, result);
            Py_DECREF(result);
            return NULL;
        }
        return result;
    }
    if (PyErr_Occurred())
        return NULL;
    return PyBytes_FromObject(v);
}

PyAPI_FUNC(PyObject *) PyCFunction_Call(PyObject *callable, PyObject *args, PyObject *kwargs);
PyObject *PyCFunction_Call(PyObject *callable, PyObject *args, PyObject *kwargs)
{
    return PyObject_Call(callable, args, kwargs);
}

PyAPI_FUNC(PyObject *) PyEval_CallObjectWithKeywords(PyObject *callable, PyObject *args,
                                                     PyObject *kwargs);
PyObject *PyEval_CallObjectWithKeywords(PyObject *callable, PyObject *args, PyObject *kwargs)
{
    if (args && !PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    }
    if (kwargs && !PyDict_Check(kwargs)) {
        PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary");
        return NULL;
    }
    if (!args)
        return PyObject_VectorcallDict(callable, NULL, 0, kwargs);
    return PyObject_Call(callable, args, kwargs);
}

PyAPI_FUNC(PyObject *) PyEval_CallFunction(PyObject *callable, const char *format, ...);
PyObject *PyEval_CallFunction(PyObject *callable, const char *format, ...)
{
    va_list va;
    PyObject *r;
    va_start(va, format);
    r = qp_capi_call_va(callable, format, va);
    va_end(va);
    return r;
}

PyAPI_FUNC(PyObject *) PyEval_CallMethod(PyObject *obj, const char *name, const char *format, ...);
PyObject *PyEval_CallMethod(PyObject *obj, const char *name, const char *format, ...)
{
    va_list va;
    PyObject *r;
    if (obj == NULL || name == NULL)
        return qp_capi_null_error();
    va_start(va, format);
    r = qp_capi_callmethod_va(obj, PyObject_GetAttrString(obj, name), format, va);
    va_end(va);
    return r;
}

PyAPI_FUNC(int) PyObject_CheckReadBuffer(PyObject *obj);
int PyObject_CheckReadBuffer(PyObject *obj)
{
    PyBufferProcs *pb = Py_TYPE(obj)->tp_as_buffer;
    Py_buffer view;

    if (!pb || !pb->bf_getbuffer)
        return 0;
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) == -1) {
        PyErr_Clear();
        return 0;
    }
    PyBuffer_Release(&view);
    return 1;
}

static int qp_capi_as_read_buffer(PyObject *obj, const void **buffer, Py_ssize_t *buffer_len)
{
    Py_buffer view;

    if (!obj || !buffer || !buffer_len) {
        qp_capi_null_error();
        return -1;
    }
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) != 0)
        return -1;
    *buffer = view.buf;
    *buffer_len = view.len;
    PyBuffer_Release(&view);
    return 0;
}

PyAPI_FUNC(int) PyObject_AsCharBuffer(PyObject *obj, const char **buffer, Py_ssize_t *buffer_len);
int PyObject_AsCharBuffer(PyObject *obj, const char **buffer, Py_ssize_t *buffer_len)
{
    return qp_capi_as_read_buffer(obj, (const void **)buffer, buffer_len);
}

PyAPI_FUNC(int) PyObject_AsReadBuffer(PyObject *obj, const void **buffer, Py_ssize_t *buffer_len);
int PyObject_AsReadBuffer(PyObject *obj, const void **buffer, Py_ssize_t *buffer_len)
{
    return qp_capi_as_read_buffer(obj, buffer, buffer_len);
}

PyAPI_FUNC(int) PyObject_AsWriteBuffer(PyObject *obj, void **buffer, Py_ssize_t *buffer_len);
int PyObject_AsWriteBuffer(PyObject *obj, void **buffer, Py_ssize_t *buffer_len)
{
    PyBufferProcs *pb;
    Py_buffer view;

    if (!obj || !buffer || !buffer_len) {
        qp_capi_null_error();
        return -1;
    }
    pb = Py_TYPE(obj)->tp_as_buffer;
    if (!pb || !pb->bf_getbuffer || PyObject_GetBuffer(obj, &view, PyBUF_WRITABLE) != 0) {
        PyErr_Clear();
        PyErr_SetString(PyExc_TypeError, "expected a writable bytes-like object");
        return -1;
    }
    *buffer = view.buf;
    *buffer_len = view.len;
    PyBuffer_Release(&view);
    return 0;
}

const char *PyExceptionClass_Name(PyObject *ob)
{
    return ((PyTypeObject *)ob)->tp_name;
}

/* ========================================================================= */
/* Unicode legacy entries (abi_only)                                          */
/* ========================================================================= */

PyAPI_FUNC(Py_ssize_t) PyUnicode_GetSize(PyObject *unicode);
Py_ssize_t PyUnicode_GetSize(PyObject *unicode)
{
    PyErr_SetString(PyExc_RuntimeError, "PyUnicode_GetSize has been removed.");
    return -1;
}

PyAPI_FUNC(void) PyUnicode_InternImmortal(PyObject **p);
void PyUnicode_InternImmortal(PyObject **p)
{
    PyUnicode_InternInPlace(p);
}

PyAPI_FUNC(PyObject *) PyUnicode_AsDecodedObject(PyObject *unicode, const char *encoding,
                                                 const char *errors);
PyObject *PyUnicode_AsDecodedObject(PyObject *unicode, const char *encoding, const char *errors)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyErr_WarnEx(PyExc_DeprecationWarning, "PyUnicode_AsDecodedObject() is deprecated "
                     "and will be removed in 3.15; use PyCodec_Decode() to decode from str", 1) < 0)
        return NULL;
    if (encoding == NULL)
        encoding = PyUnicode_GetDefaultEncoding();
    return PyCodec_Decode(unicode, encoding, errors);
}

PyAPI_FUNC(PyObject *) PyUnicode_AsDecodedUnicode(PyObject *unicode, const char *encoding,
                                                  const char *errors);
PyObject *PyUnicode_AsDecodedUnicode(PyObject *unicode, const char *encoding, const char *errors)
{
    PyObject *v;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyErr_WarnEx(PyExc_DeprecationWarning, "PyUnicode_AsDecodedUnicode() is deprecated "
                     "and will be removed in 3.15; use PyCodec_Decode() to decode from str to str",
                     1) < 0)
        return NULL;
    if (encoding == NULL)
        encoding = PyUnicode_GetDefaultEncoding();
    v = PyCodec_Decode(unicode, encoding, errors);
    if (v && !PyUnicode_Check(v)) {
        PyErr_Format(PyExc_TypeError, "'%.400s' decoder returned '%.400s' instead of 'str'; "
                     "use codecs.decode() to decode to arbitrary types",
                     encoding, Py_TYPE(unicode)->tp_name);
        Py_SETREF(v, NULL);
    }
    return v;
}

PyAPI_FUNC(PyObject *) PyUnicode_AsEncodedObject(PyObject *unicode, const char *encoding,
                                                 const char *errors);
PyObject *PyUnicode_AsEncodedObject(PyObject *unicode, const char *encoding, const char *errors)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyErr_WarnEx(PyExc_DeprecationWarning, "PyUnicode_AsEncodedObject() is deprecated "
                     "and will be removed in 3.15; use PyUnicode_AsEncodedString() to encode from "
                     "str to bytes or PyCodec_Encode() for generic encoding", 1) < 0)
        return NULL;
    if (encoding == NULL)
        encoding = PyUnicode_GetDefaultEncoding();
    return PyCodec_Encode(unicode, encoding, errors);
}

PyAPI_FUNC(PyObject *) PyUnicode_AsEncodedUnicode(PyObject *unicode, const char *encoding,
                                                  const char *errors);
PyObject *PyUnicode_AsEncodedUnicode(PyObject *unicode, const char *encoding, const char *errors)
{
    PyObject *v;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    if (PyErr_WarnEx(PyExc_DeprecationWarning, "PyUnicode_AsEncodedUnicode() is deprecated "
                     "and will be removed in 3.15; use PyCodec_Encode() to encode from str to str",
                     1) < 0)
        return NULL;
    if (encoding == NULL)
        encoding = PyUnicode_GetDefaultEncoding();
    v = PyCodec_Encode(unicode, encoding, errors);
    if (v && !PyUnicode_Check(v)) {
        PyErr_Format(PyExc_TypeError, "'%.400s' encoder returned '%.400s' instead of 'str'; "
                     "use codecs.encode() to encode to arbitrary types",
                     encoding, Py_TYPE(v)->tp_name);
        Py_SETREF(v, NULL);
    }
    return v;
}

/* ========================================================================= */
/* marshal (abi_only string forms)                                            */
/* ========================================================================= */

PyObject *PyMarshal_ReadObjectFromString(const char *data, Py_ssize_t len)
{
    PyObject *loads, *b, *r;

    b = PyBytes_FromStringAndSize(data, len);
    if (!b)
        return NULL;
    loads = PyImport_ImportModuleAttrString("marshal", "loads");
    r = loads ? PyObject_CallOneArg(loads, b) : NULL;
    Py_XDECREF(loads);
    Py_DECREF(b);
    return r;
}

PyObject *PyMarshal_WriteObjectToString(PyObject *x, int version)
{
    PyObject *dumps = PyImport_ImportModuleAttrString("marshal", "dumps");
    PyObject *r = dumps ? PyObject_CallFunction(dumps, "Oi", x, version) : NULL;
    Py_XDECREF(dumps);
    return r;
}

/* ========================================================================= */
/* Import                                                                     */
/* ========================================================================= */

long PyImport_GetMagicNumber(void)
{
    return (long)qp_pyc_magic_number_token();
}

const char *PyImport_GetMagicTag(void)
{
    return QP_CACHE_TAG;
}

PyAPI_FUNC(PyObject *) PyImport_ImportModuleNoBlock(const char *name);
PyObject *PyImport_ImportModuleNoBlock(const char *name)
{
    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "PyImport_ImportModuleNoBlock() is deprecated and scheduled for "
                     "removal in Python 3.15. Use PyImport_ImportModule() instead.", 1))
        return NULL;
    return PyImport_ImportModule(name);
}

/* No frozen modules exist (mod_imp.c): find_frozen() reports
   FROZEN_NOT_FOUND for every name, and CPython returns 0 then. */
int PyImport_ImportFrozenModuleObject(PyObject *name)
{
    if (!PyUnicode_Check(name)) {
        PyErr_BadArgument();
        return -1;
    }
    return 0;
}

int PyImport_ImportFrozenModule(const char *name)
{
    PyObject *nameobj = PyUnicode_InternFromString(name);
    int r;
    if (!nameobj)
        return -1;
    r = PyImport_ImportFrozenModuleObject(nameobj);
    Py_DECREF(nameobj);
    return r;
}

/* remove_module */
static void qp_capi_remove_module(PyObject *name)
{
    PyObject *exc = PyErr_GetRaisedException();
    PyObject *modules = PyImport_GetModuleDict();
    if (PyDict_Check(modules)) {
        if (PyDict_Pop(modules, name, NULL) < 0)
            PyErr_Clear();
    } else if (PyMapping_DelItem(modules, name) < 0) {
        if (PyErr_ExceptionMatches(PyExc_KeyError))
            PyErr_Clear();
    }
    if (exc) {
        if (PyErr_Occurred()) {
            PyObject *exc2 = PyErr_GetRaisedException();
            PyException_SetContext(exc2, exc);
            PyErr_SetRaisedException(exc2);
        } else {
            PyErr_SetRaisedException(exc);
        }
    }
}

/* module_dict_for_exec.  New reference. */
static PyObject *qp_capi_module_dict_for_exec(PyObject *name)
{
    PyObject *m, *d;
    const char *cname = PyUnicode_AsUTF8(name);
    int r;

    if (!cname)
        return NULL;
    m = PyImport_AddModuleRef(cname);
    if (!m)
        return NULL;
    d = PyModule_GetDict(m);
    r = PyDict_ContainsString(d, "__builtins__");
    if (r == 0)
        r = PyDict_SetItemString(d, "__builtins__", PyEval_GetBuiltins());
    if (r < 0) {
        qp_capi_remove_module(name);
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(d);
    Py_DECREF(m);
    return d;
}

PyObject *PyImport_ExecCodeModuleObject(PyObject *name, PyObject *co, PyObject *pathname,
                                        PyObject *cpathname)
{
    PyObject *d, *external, *res, *v;

    d = qp_capi_module_dict_for_exec(name);
    if (!d)
        return NULL;
    external = PyImport_ImportModule("_frozen_importlib_external");
    if (!external) {
        Py_DECREF(d);
        return NULL;
    }
    if (pathname)
        Py_INCREF(pathname);
    else
        pathname = PyObject_GetAttrString(co, "co_filename");
    res = pathname ? PyObject_CallMethod(external, "_fix_up_module", "OOOO", d, name, pathname,
                                         cpathname ? cpathname : Py_None)
                   : NULL;
    Py_XDECREF(pathname);
    Py_DECREF(external);
    if (!res) {
        Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(res);
    /* exec_code_in_module */
    v = PyEval_EvalCode(co, d, d);
    Py_DECREF(d);
    if (!v) {
        qp_capi_remove_module(name);
        return NULL;
    }
    Py_DECREF(v);
    res = PyImport_GetModule(name);
    if (!res && !PyErr_Occurred())
        PyErr_Format(PyExc_ImportError, "Loaded module %R not found in sys.modules", name);
    return res;
}

PyObject *PyImport_ExecCodeModuleWithPathnames(const char *name, PyObject *co, const char *pathname,
                                               const char *cpathname)
{
    PyObject *m = NULL, *nameobj, *pathobj = NULL, *cpathobj = NULL, *external;

    nameobj = PyUnicode_FromString(name);
    if (!nameobj)
        return NULL;
    if (cpathname) {
        cpathobj = PyUnicode_DecodeFSDefault(cpathname);
        if (!cpathobj)
            goto error;
    }
    if (pathname) {
        pathobj = PyUnicode_DecodeFSDefault(pathname);
        if (!pathobj)
            goto error;
    } else if (cpathobj) {
        external = PyImport_ImportModule("_frozen_importlib_external");
        if (external) {
            pathobj = PyObject_CallMethod(external, "_get_sourcefile", "O", cpathobj);
            Py_DECREF(external);
        }
        if (!pathobj)
            PyErr_Clear();
    }
    m = PyImport_ExecCodeModuleObject(nameobj, co, pathobj, cpathobj);
error:
    Py_DECREF(nameobj);
    Py_XDECREF(pathobj);
    Py_XDECREF(cpathobj);
    return m;
}

PyObject *PyImport_ExecCodeModuleEx(const char *name, PyObject *co, const char *pathname)
{
    return PyImport_ExecCodeModuleWithPathnames(name, co, pathname, NULL);
}

PyObject *PyImport_ExecCodeModule(const char *name, PyObject *co)
{
    return PyImport_ExecCodeModuleWithPathnames(name, co, NULL, NULL);
}

PyObject *PyImport_GetImporter(PyObject *path)
{
    PyObject *cache, *hooks, *importer = NULL;
    Py_ssize_t j, nhooks;

    cache = PySys_GetAttrString("path_importer_cache");
    if (!cache)
        return NULL;
    hooks = PySys_GetAttrString("path_hooks");
    if (!hooks) {
        Py_DECREF(cache);
        return NULL;
    }
    /* get_path_importer */
    if (!PyList_Check(hooks)) {
        PyErr_SetString(PyExc_RuntimeError, "sys.path_hooks is not a list");
        goto done;
    }
    if (!PyDict_Check(cache)) {
        PyErr_SetString(PyExc_RuntimeError, "sys.path_importer_cache is not a dict");
        goto done;
    }
    nhooks = PyList_Size(hooks);
    if (nhooks < 0)
        goto done;
    if (PyDict_GetItemRef(cache, path, &importer) != 0)
        goto done;      /* found or error */
    /* set path_importer_cache[p] to None to avoid recursion */
    if (PyDict_SetItem(cache, path, Py_None) != 0)
        goto done;
    for (j = 0; j < nhooks; j++) {
        PyObject *hook = PyList_GetItem(hooks, j);
        if (!hook)
            goto done;
        importer = PyObject_CallOneArg(hook, path);
        if (importer)
            break;
        if (!PyErr_ExceptionMatches(PyExc_ImportError))
            goto done;
        PyErr_Clear();
    }
    if (!importer) {
        importer = Py_NewRef(Py_None);
    } else if (PyDict_SetItem(cache, path, importer) < 0) {
        Py_CLEAR(importer);
    }
done:
    Py_DECREF(hooks);
    Py_DECREF(cache);
    return importer;
}

/* ---- PyState_*: single-phase modules by PyModuleDef index ---------------- */

static PyObject **qp_capi_modules_by_index;     /* borrowed: CPython keeps them in a list */
static Py_ssize_t qp_capi_modules_by_index_len;

static int qp_capi_modules_by_index_set(PyModuleDef *def, PyObject *module)
{
    Py_ssize_t index = def->m_base.m_index;
    if (index <= 0) {
        PyErr_SetString(PyExc_SystemError, "module index not set");
        return -1;
    }
    if (index >= qp_capi_modules_by_index_len) {
        Py_ssize_t n = index + 16, i;
        PyObject **a = qp_sys_realloc(qp_capi_modules_by_index, n * sizeof(*a));
        if (!a) {
            PyErr_NoMemory();
            return -1;
        }
        for (i = qp_capi_modules_by_index_len; i < n; i++)
            a[i] = NULL;
        qp_capi_modules_by_index = a;
        qp_capi_modules_by_index_len = n;
    }
    /* the list holds a strong reference in CPython */
    Py_XINCREF(module);
    Py_XSETREF(qp_capi_modules_by_index[index], module);
    return 0;
}

/* qp_capi_context_free (CPython clears interp->modules_by_index in
   finalize_modules): the references go while the objects work */
static void qp_capi_modules_by_index_clear(void)
{
    Py_ssize_t i;
    for (i = 0; i < qp_capi_modules_by_index_len; i++)
        Py_CLEAR(qp_capi_modules_by_index[i]);
    qp_sys_free(qp_capi_modules_by_index);
    qp_capi_modules_by_index = NULL;
    qp_capi_modules_by_index_len = 0;
}

PyObject *PyState_FindModule(PyModuleDef *module)
{
    Py_ssize_t index = module->m_base.m_index;
    if (module->m_slots)
        return NULL;
    if (index <= 0 || index >= qp_capi_modules_by_index_len)
        return NULL;
    return qp_capi_modules_by_index[index];
}

PyAPI_FUNC(int) _PyState_AddModule(PyThreadState *tstate, PyObject *module, PyModuleDef *def);
int _PyState_AddModule(PyThreadState *tstate, PyObject *module, PyModuleDef *def)
{
    if (!def)
        return -1;
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError, "PyState_AddModule called on module with slots");
        return -1;
    }
    return qp_capi_modules_by_index_set(def, module);
}

int PyState_AddModule(PyObject *module, PyModuleDef *def)
{
    Py_ssize_t index;
    if (!def)
        Py_FatalError("module definition is NULL");
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError, "PyState_AddModule called on module with slots");
        return -1;
    }
    index = def->m_base.m_index;
    if (index > 0 && index < qp_capi_modules_by_index_len &&
        qp_capi_modules_by_index[index] == module)
        _Py_FatalErrorFormat("PyState_AddModule", "module %p already added", module);
    return qp_capi_modules_by_index_set(def, module);
}

int PyState_RemoveModule(PyModuleDef *def)
{
    Py_ssize_t index;
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError, "PyState_RemoveModule called on module with slots");
        return -1;
    }
    index = def->m_base.m_index;
    if (index == 0)
        Py_FatalError("PyState_RemoveModule: Module index invalid");
    if (index >= qp_capi_modules_by_index_len) {
        PyErr_SetString(PyExc_SystemError, "PyState_RemoveModule: Module index out of bounds.");
        return -1;
    }
    Py_CLEAR(qp_capi_modules_by_index[index]);
    return 0;
}

/* ========================================================================= */
/* sys (legacy setters)                                                       */
/* ========================================================================= */

PyAPI_FUNC(void) PySys_ResetWarnOptions(void);
void PySys_ResetWarnOptions(void)
{
    PyObject *warnoptions;
    if (!qp_capi.tstate_current)
        return;
    if (PySys_GetOptionalAttrString("warnoptions", &warnoptions) < 0) {
        PyErr_Clear();
        return;
    }
    if (warnoptions && PyList_Check(warnoptions))
        PyList_SetSlice(warnoptions, 0, PyList_Size(warnoptions), NULL);
    Py_XDECREF(warnoptions);
}

/* get_warnoptions: sys.warnoptions, created when missing.  New reference. */
static PyObject *qp_capi_get_list_attr(const char *name)
{
    PyObject *v;
    if (PySys_GetOptionalAttrString(name, &v) < 0)
        return NULL;
    if (v && PyList_Check(v))
        return v;
    Py_XDECREF(v);
    v = PyList_New(0);
    if (v && PySys_SetObject(name, v) < 0)
        Py_CLEAR(v);
    return v;
}

PyAPI_FUNC(void) PySys_AddWarnOptionUnicode(PyObject *option);
void PySys_AddWarnOptionUnicode(PyObject *option)
{
    PyObject *warnoptions;
    if (!qp_capi.tstate_current)
        _Py_FatalErrorFunc("PySys_AddWarnOptionUnicode", QP_CAPI_STABLE_NO_TSTATE);
    warnoptions = qp_capi_get_list_attr("warnoptions");
    if (!warnoptions || PyList_Append(warnoptions, option) < 0)
        PyErr_Clear();
    Py_XDECREF(warnoptions);
}

PyAPI_FUNC(void) PySys_AddWarnOption(const wchar_t *s);
void PySys_AddWarnOption(const wchar_t *s)
{
    PyObject *unicode;
    if (!qp_capi.tstate_current)
        return;     /* no pre-initialization state to append to */
    unicode = PyUnicode_FromWideChar(s, -1);
    if (!unicode)
        return;
    PySys_AddWarnOptionUnicode(unicode);
    Py_DECREF(unicode);
}

PyAPI_FUNC(int) PySys_HasWarnOptions(void);
int PySys_HasWarnOptions(void)
{
    PyObject *warnoptions;
    int r;
    if (PySys_GetOptionalAttrString("warnoptions", &warnoptions) < 0) {
        PyErr_Clear();
        return 0;
    }
    r = warnoptions && PyList_Check(warnoptions) && PyList_Size(warnoptions) > 0;
    Py_XDECREF(warnoptions);
    return r;
}

PyAPI_FUNC(void) PySys_AddXOption(const wchar_t *s);
void PySys_AddXOption(const wchar_t *s)
{
    PyObject *opts = NULL, *name = NULL, *value = NULL;
    const wchar_t *name_end;

    if (!qp_capi.tstate_current)
        return;
    if (PySys_GetOptionalAttrString("_xoptions", &opts) < 0)
        goto error;
    if (!opts || !PyDict_Check(opts)) {
        Py_XDECREF(opts);
        opts = PyDict_New();
        if (!opts || PySys_SetObject("_xoptions", opts) < 0)
            goto error;
    }
    name_end = wcschr(s, L'=');
    if (!name_end) {
        name = PyUnicode_FromWideChar(s, -1);
        value = Py_NewRef(Py_True);
    } else {
        name = PyUnicode_FromWideChar(s, name_end - s);
        value = name ? PyUnicode_FromWideChar(name_end + 1, -1) : NULL;
    }
    if (!name || !value || PyDict_SetItem(opts, name, value) < 0)
        goto error;
    Py_DECREF(name);
    Py_DECREF(value);
    Py_DECREF(opts);
    return;
error:
    Py_XDECREF(name);
    Py_XDECREF(value);
    Py_XDECREF(opts);
    PyErr_Clear();
}

void PySys_SetArgvEx(int argc, wchar_t **argv, int updatepath)
{
    static wchar_t empty[1] = { 0 };
    wchar_t *empty_argv[1] = { empty };
    PyObject *av, *sys_path;
    int i;

    if (argc < 1 || argv == NULL) {
        argv = empty_argv;
        argc = 1;
    }
    av = PyList_New(argc);
    if (!av)
        Py_FatalError("no mem for sys.argv");
    for (i = 0; i < argc; i++) {
        PyObject *v = PyUnicode_FromWideChar(argv[i], -1);
        if (!v)
            Py_FatalError("no mem for sys.argv");
        PyList_SetItem(av, i, v);
    }
    if (PySys_SetObject("argv", av) != 0)
        Py_FatalError("can't assign sys.argv");
    Py_DECREF(av);
    if (!updatepath)
        return;
    /* _PyPathConfig_ComputeSysPath0: '' for "-c"/"-m"/"" argv[0] handling,
       else the directory of argv[0] (symlinks resolved) */
    {
        const wchar_t *arg0 = argv[0];
        PyObject *path0;
        if (wcscmp(arg0, L"-c") == 0)
            path0 = PyUnicode_FromString("");
        else if (wcscmp(arg0, L"-m") == 0) {
            char cwd[PATH_MAX];
            path0 = getcwd(cwd, sizeof(cwd)) ? PyUnicode_DecodeFSDefault(cwd)
                                             : PyUnicode_FromString("");
        } else {
            PyObject *a = PyUnicode_FromWideChar(arg0, -1), *b = NULL;
            char *full = NULL, buf[PATH_MAX];
            const char *s, *slash;
            path0 = NULL;
            if (a)
                b = PyUnicode_EncodeFSDefault(a);
            Py_XDECREF(a);
            if (b) {
                s = PyBytes_AsString(b);
                full = s ? realpath(s, buf) : NULL;
                if (full)
                    s = full;
                slash = s ? strrchr(s, '/') : NULL;
                if (!s)
                    path0 = NULL;
                else if (!slash)
                    path0 = PyUnicode_FromString("");
                else
                    path0 = PyUnicode_DecodeFSDefaultAndSize(s, slash == s ? 1 : slash - s);
                Py_DECREF(b);
            }
        }
        if (!path0)
            Py_FatalError("can't compute path0 from argv");
        if (PySys_GetOptionalAttrString("path", &sys_path) < 0)
            Py_FatalError("can't get sys.path");
        if (sys_path) {
            if (PyList_Insert(sys_path, 0, path0) < 0)
                Py_FatalError("can't prepend path0 to sys.path");
            Py_DECREF(sys_path);
        }
        Py_DECREF(path0);
    }
}

void PySys_SetArgv(int argc, wchar_t **argv)
{
    PySys_SetArgvEx(argc, argv, 0);
}

/* makepathobject: split at ':' */
static PyObject *qp_capi_make_path_list(const wchar_t *path)
{
    PyObject *v = PyList_New(0), *w;
    const wchar_t *p = path, *e;

    if (!v)
        return NULL;
    for (;;) {
        e = wcschr(p, L':');
        if (!e)
            e = p + wcslen(p);
        w = PyUnicode_FromWideChar(p, e - p);
        if (!w || PyList_Append(v, w) < 0) {
            Py_XDECREF(w);
            Py_DECREF(v);
            return NULL;
        }
        Py_DECREF(w);
        if (*e == L'\0')
            break;
        p = e + 1;
    }
    return v;
}

PyAPI_FUNC(void) PySys_SetPath(const wchar_t *path);
void PySys_SetPath(const wchar_t *path)
{
    PyObject *v = qp_capi_make_path_list(path);
    if (!v)
        Py_FatalError("can't create sys.path");
    if (PySys_SetObject("path", v) != 0)
        Py_FatalError("can't assign sys.path");
    Py_DECREF(v);
}

/* ========================================================================= */
/* ceval, thread state                                                        */
/* ========================================================================= */

PyObject *PyEval_EvalFrame(PyFrameObject *f)
{
    PyErr_SetString(PyExc_SystemError,
                    "PyEval_EvalFrame: frame objects cannot be re-entered in this runtime");
    return NULL;
}

PyObject *PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    return PyEval_EvalFrame(f);
}

/* The GIL of this runtime is the current thread state (capi_runtime.c,
   capi-design.md §5): taking or dropping "the lock" without changing the
   thread state has nothing to do once the thread state check passed. */
PyAPI_FUNC(void) PyEval_AcquireLock(void);
void PyEval_AcquireLock(void)
{
    if (!qp_capi.tstate_current)
        _Py_FatalErrorFunc("PyEval_AcquireLock", QP_CAPI_STABLE_NO_TSTATE);
}

PyAPI_FUNC(void) PyEval_ReleaseLock(void);
void PyEval_ReleaseLock(void)
{
    if (!qp_capi.tstate_current)
        _Py_FatalErrorFunc("PyEval_ReleaseLock", QP_CAPI_STABLE_NO_TSTATE);
}

PyAPI_FUNC(int) PyEval_ThreadsInitialized(void);
int PyEval_ThreadsInitialized(void)
{
    return Py_IsInitialized();
}

PyAPI_FUNC(int) _Py_CheckRecursiveCall(PyThreadState *tstate, const char *where);
int _Py_CheckRecursiveCall(PyThreadState *tstate, const char *where)
{
    return Py_EnterRecursiveCall(where);
}

PyAPI_FUNC(PyThreadState *) _PyThreadState_Prealloc(PyInterpreterState *interp);
PyThreadState *_PyThreadState_Prealloc(PyInterpreterState *interp)
{
    return PyThreadState_New(interp);
}

PyAPI_FUNC(void) _PyThreadState_Init(PyThreadState *tstate);
void _PyThreadState_Init(PyThreadState *tstate)
{
    Py_FatalError("_PyThreadState_Init() is for internal use only");
}

/* Async exceptions: the target thread state's async_exc is raised by the
   next interpreter poll (qp_signal_check -> qp_capi_async_exc_check), as
   CPython's eval breaker does with _PY_ASYNC_EXCEPTION_BIT. */
static volatile int qp_capi_async_exc_pending;

int PyThreadState_SetAsyncExc(unsigned long id, PyObject *exc)
{
    PyInterpreterState *interp = qp_capi.interp;
    PyThreadState *t;

    if (!interp)
        return 0;
    for (t = interp->threads.head; t; t = t->next) {
        if (t->thread_id == id) {
            PyObject *old = t->async_exc;
            t->async_exc = Py_XNewRef(exc);
            Py_XDECREF(old);
            if (exc) {
                qp_capi_async_exc_pending = 1;
                if (qp_capi.ctx)
                    JS_TriggerInterruptHandler(qp_capi.ctx);
            }
            return 1;
        }
    }
    return 0;
}

static int qp_capi_async_exc_check(JSContext *ctx)
{
    PyThreadState *t;
    PyObject *exc;

    if (!qp_capi_async_exc_pending || ctx != qp_capi.ctx)
        return 0;
    t = qp_capi.tstate_current;
    if (!t || !t->async_exc)
        return 0;
    qp_capi_async_exc_pending = 0;
    exc = t->async_exc;
    t->async_exc = NULL;
    PyErr_SetNone(exc);
    Py_DECREF(exc);
    return -1;
}

/* ---- PEP 788: interpreter guards and views (one interpreter) ------------- */

struct PyInterpreterGuard {
    PyInterpreterState *interp;
};

struct PyInterpreterView {
    int64_t id;
};

static intptr_t qp_capi_guard_count;

static PyInterpreterGuard *qp_capi_guard_new(void)
{
    PyInterpreterGuard *guard;
    if (!qp_capi.interp || qp_capi_finalizing || qp_capi_dead)
        return NULL;
    guard = PyMem_RawMalloc(sizeof(*guard));
    if (!guard)
        return NULL;
    guard->interp = qp_capi.interp;
    qp_capi_guard_count++;
    return guard;
}

PyInterpreterGuard *PyInterpreterGuard_FromCurrent(void)
{
    PyInterpreterGuard *guard;
    if (!qp_capi.tstate_current)
        _Py_FatalErrorFunc("PyInterpreterGuard_FromCurrent", QP_CAPI_STABLE_NO_TSTATE);
    if (qp_capi_finalizing) {
        PyErr_SetString(PyExc_PythonFinalizationError, "cannot acquire finalization guard anymore");
        return NULL;
    }
    guard = qp_capi_guard_new();
    if (!guard)
        PyErr_NoMemory();
    return guard;
}

void PyInterpreterGuard_Close(PyInterpreterGuard *guard)
{
    qp_capi_guard_count--;
    PyMem_RawFree(guard);
}

PyInterpreterGuard *PyInterpreterGuard_FromView(PyInterpreterView *view)
{
    if (view->id != 0)
        return NULL;
    return qp_capi_guard_new();
}

PyInterpreterView *PyInterpreterView_FromCurrent(void)
{
    PyInterpreterView *view;
    if (!qp_capi.tstate_current)
        _Py_FatalErrorFunc("PyInterpreterView_FromCurrent", QP_CAPI_STABLE_NO_TSTATE);
    view = PyMem_RawMalloc(sizeof(*view));
    if (!view) {
        PyErr_NoMemory();
        return NULL;
    }
    view->id = 0;
    return view;
}

PyInterpreterView *PyInterpreterView_FromMain(void)
{
    PyInterpreterView *view = PyMem_RawMalloc(sizeof(*view));
    if (view)
        view->id = 0;
    return view;
}

void PyInterpreterView_Close(PyInterpreterView *view)
{
    PyMem_RawFree(view);
}

/* PyThreadState_Ensure attaches the thread state of this (only) thread
   like PyGILState_Ensure; the token records whether it was attached
   before, and Release restores that.  Foreign threads are rejected by
   PyGILState_Ensure (capi-design.md §5). */
static const char qp_capi_ensure_tokens[2];
#define QP_CAPI_TOKEN_LOCKED   ((PyThreadStateToken *)&qp_capi_ensure_tokens[0])
#define QP_CAPI_TOKEN_UNLOCKED ((PyThreadStateToken *)&qp_capi_ensure_tokens[1])

PyThreadStateToken *PyThreadState_Ensure(PyInterpreterGuard *guard)
{
    if (guard == NULL || guard->interp != qp_capi.interp)
        return NULL;
    return PyGILState_Ensure() == PyGILState_LOCKED ? QP_CAPI_TOKEN_LOCKED : QP_CAPI_TOKEN_UNLOCKED;
}

PyThreadStateToken *PyThreadState_EnsureFromView(PyInterpreterView *view)
{
    PyInterpreterGuard *guard = PyInterpreterGuard_FromView(view);
    PyThreadStateToken *token;
    if (!guard)
        return NULL;
    token = PyThreadState_Ensure(guard);
    /* the interpreter cannot be finalized while this thread state is
       attached: the guard only had to cover the attach */
    PyInterpreterGuard_Close(guard);
    return token;
}

void PyThreadState_Release(PyThreadStateToken *token)
{
    if (token != QP_CAPI_TOKEN_LOCKED && token != QP_CAPI_TOKEN_UNLOCKED)
        Py_FatalError("PyThreadState_Release() called with an invalid token");
    PyGILState_Release(token == QP_CAPI_TOKEN_LOCKED ? PyGILState_LOCKED : PyGILState_UNLOCKED);
}

/* ========================================================================= */
/* Version strings (getversion.c, getplatform.c, getcompiler.c, ...)          */
/* ========================================================================= */

const char *Py_GetVersion(void)
{
    return QP_PY_VERSION " (main, " __DATE__ ", " __TIME__ ") " QP_COMPILER;
}

const char *Py_GetBuildInfo(void)
{
    return "main, " __DATE__ ", " __TIME__;
}

const char *Py_GetCompiler(void)
{
    return QP_COMPILER;
}

const char *Py_GetPlatform(void)
{
    return QP_PLATFORM;
}

const char *Py_GetCopyright(void)
{
    return qp_sys_copyright;
}

/* Modules/getbuildinfo.c: whether the C API implementation was compiled
   with assert() enabled; exported for _testlimitedcapi only
   (test.support.built_with_c_assertions) */
int _Py_GetBuiltWithAssert(void);
int _Py_GetBuiltWithAssert(void)
{
#ifdef NDEBUG
    return 0;
#else
    return 1;
#endif
}

/* exported equivalents of the macros (pymacro.h) */
#undef Py_PACK_FULL_VERSION
#undef Py_PACK_VERSION
uint32_t Py_PACK_FULL_VERSION(int x, int y, int z, int level, int serial)
{
    return _Py_PACK_FULL_VERSION(x, y, z, level, serial);
}

uint32_t Py_PACK_VERSION(int x, int y)
{
    return _Py_PACK_VERSION(x, y);
}

/* ========================================================================= */
/* Locale encoding (fileutils.c)                                              */
/* ========================================================================= */

/* The locale encoding is UTF-8 on macOS (_Py_FORCE_UTF8_LOCALE) and when
   the LC_CTYPE codeset is UTF-8; otherwise the C library converts. */
static int qp_capi_locale_is_utf8(void)
{
#if defined(__APPLE__)
    return 1;
#else
    const char *cs = nl_langinfo(CODESET);
    return Py_UTF8Mode || (cs && (!strcmp(cs, "UTF-8") || !strcmp(cs, "utf8")));
#endif
}

/* UTF-8 (strict, surrogates rejected) with surrogateescape: undecodable
   bytes become U+DC80..U+DCFF. */
static wchar_t *qp_capi_decode_utf8_se(const char *s, size_t *wlen)
{
    size_t n = strlen(s), i = 0, o = 0;
    wchar_t *w = PyMem_RawMalloc((n + 1) * sizeof(wchar_t));

    if (!w)
        return NULL;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len, k;
        if (c < 0x80) {
            w[o++] = c;
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            goto escape;
        }
        if (i + len > n)
            goto escape;
        for (k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80)
                goto escape;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            goto escape;
        w[o++] = (wchar_t)cp;
        i += len;
        continue;
    escape:
        w[o++] = (wchar_t)(0xDC00 + c);
        i++;
    }
    w[o] = 0;
    if (wlen)
        *wlen = o;
    return w;
}

/* decode_current_locale: mbrtowc with surrogateescape */
static wchar_t *qp_capi_decode_locale_se(const char *s, size_t *wlen)
{
    size_t n = strlen(s), o = 0, r;
    wchar_t *w = PyMem_RawMalloc((n + 1) * sizeof(wchar_t));
    mbstate_t mbs;
    const char *p = s, *end = s + n;

    if (!w)
        return NULL;
    memset(&mbs, 0, sizeof(mbs));
    while (p < end) {
        r = mbrtowc(&w[o], p, end - p, &mbs);
        if (r == 0)
            break;
        if (r == (size_t)-1 || r == (size_t)-2) {
            w[o++] = (wchar_t)(0xDC00 + (unsigned char)*p++);
            memset(&mbs, 0, sizeof(mbs));
            continue;
        }
        if (w[o] >= 0xD800 && w[o] <= 0xDFFF) {
            /* a surrogate from the C library: escape its bytes */
            size_t k;
            for (k = 0; k < r; k++)
                w[o++] = (wchar_t)(0xDC00 + (unsigned char)p[k]);
        } else {
            o++;
        }
        p += r;
    }
    w[o] = 0;
    if (wlen)
        *wlen = o;
    return w;
}

wchar_t *Py_DecodeLocale(const char *arg, size_t *wlen)
{
    wchar_t *w = qp_capi_locale_is_utf8() ? qp_capi_decode_utf8_se(arg, wlen)
                                          : qp_capi_decode_locale_se(arg, wlen);
    if (!w && wlen)
        *wlen = (size_t)-1;
    return w;
}

char *Py_EncodeLocale(const wchar_t *text, size_t *error_pos)
{
    size_t n = wcslen(text), i, o = 0;
    int utf8 = qp_capi_locale_is_utf8();
    char *s = PyMem_Malloc(n * (utf8 ? 4 : MB_LEN_MAX) + 1);
    mbstate_t mbs;

    if (error_pos)
        *error_pos = (size_t)-1;
    if (!s)
        return NULL;
    memset(&mbs, 0, sizeof(mbs));
    for (i = 0; i < n; i++) {
        uint32_t c = (uint32_t)text[i];
        if (c >= 0xDC80 && c <= 0xDCFF) {
            /* surrogateescape */
            s[o++] = (char)(c - 0xDC00);
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF)
            goto error;
        if (utf8) {
            if (c < 0x80) {
                s[o++] = (char)c;
            } else if (c < 0x800) {
                s[o++] = (char)(0xC0 | (c >> 6));
                s[o++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                s[o++] = (char)(0xE0 | (c >> 12));
                s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                s[o++] = (char)(0x80 | (c & 0x3F));
            } else if (c <= 0x10FFFF) {
                s[o++] = (char)(0xF0 | (c >> 18));
                s[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
                s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                s[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                goto error;
            }
        } else {
            size_t r = wcrtomb(s + o, (wchar_t)c, &mbs);
            if (r == (size_t)-1)
                goto error;
            o += r;
        }
    }
    s[o] = '\0';
    return s;
error:
    PyMem_Free(s);
    if (error_pos)
        *error_pos = i;
    return NULL;
}

/* ========================================================================= */
/* Embedding: initialization, paths, main                                     */
/* ========================================================================= */

static JSRuntime *qp_capi_embed_rt;     /* runtime created by Py_InitializeEx */
static JSContext *qp_capi_embed_ctx;

void Py_InitializeEx(int install_sigs)
{
    JSRuntime *rt;
    JSContext *ctx;

    if (qp_capi.initialized == 2)
        return;
    setlocale(LC_CTYPE, "");
    rt = JS_NewRuntime();
    if (!rt)
        Py_FatalError("Py_InitializeEx: cannot allocate the runtime");
    qp_main_set_stack(rt);
    ctx = JS_NewContext(rt);
    if (!ctx)
        Py_FatalError("Py_InitializeEx: cannot allocate the context");
    if (JS_InitPython(ctx, NULL) < 0 || qp_capi_init(ctx) < 0)
        Py_FatalError("Py_InitializeEx: Python initialization failed");
    qp_capi_embed_rt = rt;
    qp_capi_embed_ctx = ctx;
}

void Py_Initialize(void)
{
    Py_InitializeEx(1);
}

int Py_FinalizeEx(void)
{
    JSContext *ctx = qp_capi.ctx;
    int status = 0;

    if (!Py_IsInitialized() || ctx == NULL)
        return 0;
    qp_capi_finalizing = 1;
    if (ctx == qp_capi_embed_ctx) {
        /* the interpreter of Py_InitializeEx: Py_FinalizeEx, then the
           runtime goes (the C API can be initialized again) */
        JSRuntime *rt = qp_capi_embed_rt;
        int nthreads;
        if (qp_finalize(ctx, &nthreads) < 0)
            status = -1;
        qp_capi_embed_ctx = NULL;
        qp_capi_embed_rt = NULL;
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    } else {
        qp_atexit_run(ctx);
        if (qp_sys_flush_std_files(ctx) < 0)
            status = -1;
        fflush(stdout);
        fflush(stderr);
    }
    return status;
}

void Py_Finalize(void)
{
    (void)Py_FinalizeEx();
}

int Py_BytesMain(int argc, char **argv)
{
    return qp_main(argc, argv);
}

int Py_Main(int argc, wchar_t **argv)
{
    char **bargv = PyMem_RawCalloc(argc + 1, sizeof(char *));
    int i, r;

    if (!bargv) {
        fprintf(stderr, "Fatal Python error: Py_Main: out of memory\n");
        return 1;
    }
    for (i = 0; i < argc; i++) {
        bargv[i] = Py_EncodeLocale(argv[i], NULL);
        if (!bargv[i]) {
            fprintf(stderr, "Fatal Python error: Py_Main: cannot encode argument %d\n", i);
            r = 1;
            goto done;
        }
    }
    r = qp_main(argc, bargv);
done:
    for (i = 0; i < argc; i++)
        PyMem_Free(bargv[i]);
    PyMem_RawFree(bargv);
    return r;
}

/* The wide-string getters return buffers owned here (CPython returns its
   path configuration's strings).  A value given to the matching setter
   wins, else the value comes from the running interpreter. */
enum {
    QP_CAPI_WS_PROGRAM_NAME,
    QP_CAPI_WS_PYTHON_HOME,
    QP_CAPI_WS_PATH,
    QP_CAPI_WS_PREFIX,
    QP_CAPI_WS_EXEC_PREFIX,
    QP_CAPI_WS_EXECUTABLE,
    QP_CAPI_WS_COUNT
};
static wchar_t *qp_capi_ws_set[QP_CAPI_WS_COUNT];    /* given to a setter */
static wchar_t *qp_capi_ws_cache[QP_CAPI_WS_COUNT];  /* last value returned */

static wchar_t *qp_capi_wcsdup(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = PyMem_RawMalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static void qp_capi_ws_store(int slot, const wchar_t *s)
{
    PyMem_RawFree(qp_capi_ws_set[slot]);
    qp_capi_ws_set[slot] = s && *s ? qp_capi_wcsdup(s) : NULL;
}

/* str (or list of str joined with ':') attribute of sys as a wide string
   kept in the cache slot; `dflt` when Python is not running. */
static wchar_t *qp_capi_ws_from_sys(int slot, const char *attr, const wchar_t *dflt)
{
    PyObject *v, *s = NULL, *sep;
    wchar_t *w;

    if (qp_capi_ws_set[slot])
        return qp_capi_ws_set[slot];
    if (!Py_IsInitialized() || !qp_capi.tstate_current)
        return (wchar_t *)dflt;
    if (PySys_GetOptionalAttrString(attr, &v) <= 0) {
        PyErr_Clear();
        return (wchar_t *)dflt;
    }
    if (PyList_Check(v)) {
        sep = PyUnicode_FromString(":");
        s = sep ? PyUnicode_Join(sep, v) : NULL;
        Py_XDECREF(sep);
    } else if (PyUnicode_Check(v)) {
        s = Py_NewRef(v);
    }
    Py_DECREF(v);
    if (!s) {
        PyErr_Clear();
        return (wchar_t *)dflt;
    }
    w = PyUnicode_AsWideCharString(s, NULL);
    Py_DECREF(s);
    if (!w) {
        PyErr_Clear();
        return (wchar_t *)dflt;
    }
    PyMem_RawFree(qp_capi_ws_cache[slot]);
    qp_capi_ws_cache[slot] = qp_capi_wcsdup(w);
    PyMem_Free(w);
    return qp_capi_ws_cache[slot] ? qp_capi_ws_cache[slot] : (wchar_t *)dflt;
}

PyAPI_FUNC(wchar_t *) Py_GetProgramName(void);
wchar_t *Py_GetProgramName(void)
{
    if (qp_capi_ws_set[QP_CAPI_WS_PROGRAM_NAME])
        return qp_capi_ws_set[QP_CAPI_WS_PROGRAM_NAME];
    if (Py_IsInitialized() && qp_capi.ctx) {
        QPConfig *c = &QP_STATE(qp_capi.ctx)->config;
        if (c->orig_argc > 0 && c->orig_argv[0]) {
            wchar_t *w = Py_DecodeLocale(c->orig_argv[0], NULL);
            if (w) {
                PyMem_RawFree(qp_capi_ws_cache[QP_CAPI_WS_PROGRAM_NAME]);
                qp_capi_ws_cache[QP_CAPI_WS_PROGRAM_NAME] = w;
                return w;
            }
        }
    }
    {
        static wchar_t dflt[] = L"python3";
        return dflt;
    }
}

void Py_SetProgramName(const wchar_t *name)
{
    qp_capi_ws_store(QP_CAPI_WS_PROGRAM_NAME, name);
}

PyAPI_FUNC(wchar_t *) Py_GetPythonHome(void);
wchar_t *Py_GetPythonHome(void)
{
    const char *home;
    if (qp_capi_ws_set[QP_CAPI_WS_PYTHON_HOME])
        return qp_capi_ws_set[QP_CAPI_WS_PYTHON_HOME];
    home = getenv("QPYHOME");
    if (!home || !*home)
        return NULL;
    PyMem_RawFree(qp_capi_ws_cache[QP_CAPI_WS_PYTHON_HOME]);
    qp_capi_ws_cache[QP_CAPI_WS_PYTHON_HOME] = Py_DecodeLocale(home, NULL);
    return qp_capi_ws_cache[QP_CAPI_WS_PYTHON_HOME];
}

void Py_SetPythonHome(const wchar_t *home)
{
    qp_capi_ws_store(QP_CAPI_WS_PYTHON_HOME, home);
}

PyAPI_FUNC(void) Py_SetPath(const wchar_t *path);
void Py_SetPath(const wchar_t *path)
{
    qp_capi_ws_store(QP_CAPI_WS_PATH, path);
}

PyAPI_FUNC(wchar_t *) Py_GetPath(void);
wchar_t *Py_GetPath(void)
{
    return qp_capi_ws_from_sys(QP_CAPI_WS_PATH, "path", L"");
}

PyAPI_FUNC(wchar_t *) Py_GetPrefix(void);
wchar_t *Py_GetPrefix(void)
{
    return qp_capi_ws_from_sys(QP_CAPI_WS_PREFIX, "base_prefix", L"");
}

PyAPI_FUNC(wchar_t *) Py_GetExecPrefix(void);
wchar_t *Py_GetExecPrefix(void)
{
    return qp_capi_ws_from_sys(QP_CAPI_WS_EXEC_PREFIX, "base_exec_prefix", L"");
}

PyAPI_FUNC(wchar_t *) Py_GetProgramFullPath(void);
wchar_t *Py_GetProgramFullPath(void)
{
    return qp_capi_ws_from_sys(QP_CAPI_WS_EXECUTABLE, "executable", L"");
}

PyAPI_FUNC(void) Py_GetArgcArgv(int *argc, wchar_t ***argv);
void Py_GetArgcArgv(int *argc, wchar_t ***argv)
{
    static wchar_t **wargv;
    static int wargc;

    if (!wargv && qp_capi.ctx) {
        QPConfig *c = &QP_STATE(qp_capi.ctx)->config;
        int i;
        wargv = PyMem_RawCalloc(c->orig_argc + 1, sizeof(wchar_t *));
        if (wargv) {
            for (i = 0; i < c->orig_argc; i++)
                wargv[i] = Py_DecodeLocale(c->orig_argv[i], NULL);
            wargc = c->orig_argc;
        }
    }
    *argc = wargc;
    *argv = wargv;
}

/* ========================================================================= */
/* Internal API of the M5 vendored modules (mmap; capi/compat shims)          */
/* ========================================================================= */

PyAPI_FUNC(Py_ssize_t) _PyBytes_Find(const char *haystack, Py_ssize_t len_haystack,
                                     const char *needle, Py_ssize_t len_needle,
                                     Py_ssize_t offset);
PyAPI_FUNC(Py_ssize_t) _PyBytes_ReverseFind(const char *haystack, Py_ssize_t len_haystack,
                                            const char *needle, Py_ssize_t len_needle,
                                            Py_ssize_t offset);
PyAPI_FUNC(int) _Py_fstat_noraise(int fd, struct stat *status);
PyAPI_FUNC(int) _Py_fstat(int fd, struct stat *status);

/* stringlib_find / stringlib_rfind on bytes: the position (plus offset)
   of the first / last occurrence, -1 when absent; an empty needle is
   found at 0 / len_haystack. */
Py_ssize_t _PyBytes_Find(const char *haystack, Py_ssize_t len_haystack, const char *needle,
                         Py_ssize_t len_needle, Py_ssize_t offset)
{
    const char *p;
    if (len_needle == 0)
        return offset;
    if (len_needle > len_haystack)
        return -1;
    p = memmem(haystack, (size_t)len_haystack, needle, (size_t)len_needle);
    return p ? (Py_ssize_t)(p - haystack) + offset : -1;
}

Py_ssize_t _PyBytes_ReverseFind(const char *haystack, Py_ssize_t len_haystack,
                                const char *needle, Py_ssize_t len_needle, Py_ssize_t offset)
{
    Py_ssize_t i;
    if (len_needle == 0)
        return len_haystack + offset;
    for (i = len_haystack - len_needle; i >= 0; i--) {
        if (haystack[i] == needle[0] && !memcmp(haystack + i, needle, (size_t)len_needle))
            return i + offset;
    }
    return -1;
}

int _Py_fstat_noraise(int fd, struct stat *status)
{
    return fstat(fd, status);
}

int _Py_fstat(int fd, struct stat *status)
{
    int res;
    Py_BEGIN_ALLOW_THREADS
    res = _Py_fstat_noraise(fd, status);
    Py_END_ALLOW_THREADS
    if (res != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Init (called by qp_capi_init after the unit inits)                        */
/* ========================================================================= */

static int qp_capi_init_stable(JSContext *ctx)
{
_Py_COMP_DIAG_PUSH
_Py_COMP_DIAG_IGNORE_DEPR_DECLS
    QPConfig *c = &QP_STATE(ctx)->config;
    Py_UTF8Mode = c->utf8_mode > 0;
    /* the values of sys.flags (python/sysmodule.c qp_sys_make_flags) */
    Py_VerboseFlag = c->verbose;
    Py_QuietFlag = c->quiet;
    Py_InteractiveFlag = c->interactive;
    Py_InspectFlag = c->inspect;
    Py_OptimizeFlag = c->optimize;
    Py_NoSiteFlag = c->no_site;
    Py_BytesWarningFlag = c->bytes_warning;
    Py_IgnoreEnvironmentFlag = c->ignore_environment;
    Py_DontWriteBytecodeFlag = c->dont_write_bytecode;
    Py_NoUserSiteDirectory = c->no_user_site;
    Py_UnbufferedStdioFlag = c->unbuffered;
    Py_HashRandomizationFlag = c->hash_seed == NULL || strcmp(c->hash_seed, "random") == 0 ||
                               strtoul(c->hash_seed, NULL, 10) != 0;
    Py_IsolatedFlag = c->isolated;
_Py_COMP_DIAG_POP
    return 0;
}
