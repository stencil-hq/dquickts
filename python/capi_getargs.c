/*
 * dquickts C API: argument parsing and value building.
 *
 * Port of CPython 3.15.2 Python/getargs.c and the value-building half of
 * Python/modsupport.c (Py_BuildValue & co.).  Copyright (c) Python Software
 * Foundation; licensed under the PSF License Agreement version 2 (PSF-2.0).
 *
 * Compiled inside the quickjs.c translation unit after python/capi.h, using
 * only the public C API declared by capi/include.  Deviations from CPython:
 * see the "qp" comments (internal helpers replaced by public equivalents).
 */
/* Internal exports (CPython Include/internal/pycore_modsupport.h and
   pycore_pylifecycle.h); that header is not included in this TU, so the
   plain functions are declared and defined here (no fast-path macros). */
PyAPI_FUNC(int) _PyArg_NoKwnames(const char *funcname, PyObject *kwnames);
PyAPI_FUNC(int) _PyArg_NoPositional(const char *funcname, PyObject *args);
PyAPI_FUNC(int) _PyArg_NoKeywords(const char *funcname, PyObject *kwargs);
PyAPI_FUNC(int) _PyArg_CheckPositional(const char *, Py_ssize_t,
                                       Py_ssize_t, Py_ssize_t);
PyAPI_FUNC(PyObject **) _Py_VaBuildStack(
    PyObject **small_stack,
    Py_ssize_t small_stack_len,
    const char *format,
    va_list va,
    Py_ssize_t *p_nargs);
PyAPI_FUNC(int) _PyArg_ParseStack(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *format,
    ...);
PyAPI_FUNC(int) _PyArg_UnpackStack(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *name,
    Py_ssize_t min,
    Py_ssize_t max,
    ...);
PyAPI_FUNC(void) _PyArg_BadArgument(
    const char *fname,
    const char *displayname,
    const char *expected,
    PyObject *arg);
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
PyAPI_FUNC(void) _PyArg_Fini(void);

/* Defined by the numbers unit (CPython Include/internal/pycore_abstract.h). */
PyAPI_FUNC(PyObject *) _PyNumber_Index(PyObject *);

/* Defined (non-static) by CPython 3.15 getargs.c for old binaries of the
   PY_SSIZE_T_CLEAN era; no header declares them any more. */
PyAPI_FUNC(int) _PyArg_ParseTupleAndKeywordsFast_SizeT(
    PyObject *, PyObject *, struct _PyArg_Parser *, ...);
PyAPI_FUNC(int) _PyArg_ParseStackAndKeywords_SizeT(
    PyObject *const *, Py_ssize_t, PyObject *, struct _PyArg_Parser *, ...);

/* Export of Python/modsupport.c (abi only). */
PyAPI_FUNC(PyObject *) _Py_BuildValue_SizeT(const char *, ...);
PyAPI_FUNC(PyObject *) _Py_VaBuildValue_SizeT(const char *, va_list);

/* Export Stable ABIs (abi only) */
PyAPI_FUNC(int) _PyArg_Parse_SizeT(PyObject *, const char *, ...);
PyAPI_FUNC(int) _PyArg_ParseTuple_SizeT(PyObject *, const char *, ...);
PyAPI_FUNC(int) _PyArg_ParseTupleAndKeywords_SizeT(PyObject *, PyObject *,
                                                  const char *, const char * const *, ...);
PyAPI_FUNC(int) _PyArg_VaParse_SizeT(PyObject *, const char *, va_list);
PyAPI_FUNC(int) _PyArg_VaParseTupleAndKeywords_SizeT(PyObject *, PyObject *,
                                              const char *, const char * const *, va_list);

/* qp: CPython's _PyRuntime.getargs.static_parsers (parsers whose kwtuple
   was initialized, released by _PyArg_Fini). */
static struct _PyArg_Parser *qp_capi_static_arg_parsers;

#define QP_CAPI_FLAG_COMPAT 1

typedef int (*qp_capi_destr_t)(PyObject *, void *);


/* Keep track of "objects" that have been allocated or initialized and
   which will need to be deallocated or cleaned up somehow if overall
   parsing fails.
*/
typedef struct {
  void *item;
  qp_capi_destr_t destructor;
} qp_capi_freelistentry_t;

typedef struct {
  qp_capi_freelistentry_t *entries;
  int first_available;
  int entries_malloced;
} qp_capi_freelist_t;

#define QP_CAPI_STATIC_FREELIST_ENTRIES 8

/* Forward */
static int qp_capi_vgetargs1_impl(PyObject *args, PyObject *const *stack, Py_ssize_t nargs,
                          const char *format, va_list *p_va, int flags);
static int qp_capi_vgetargs1(PyObject *, const char *, va_list *, int);
static void qp_capi_seterror(Py_ssize_t, const char *, int *, const char *, const char *);
static const char *qp_capi_convertitem(PyObject *, const char **, va_list *, int, int *,
                               char *, size_t, qp_capi_freelist_t *);
static const char *qp_capi_converttuple(PyObject *, const char **, va_list *, int,
                                int *, char *, size_t, qp_capi_freelist_t *);
static const char *qp_capi_convertsimple(PyObject *, const char **, va_list *, int,
                                 char *, size_t, qp_capi_freelist_t *);
static Py_ssize_t qp_capi_convertbuffer(PyObject *, const void **p, const char **);
static int qp_capi_getbuffer(PyObject *, Py_buffer *, const char**);

static int
qp_capi_vgetargskeywords(PyObject *args, PyObject *kwargs,
                 const char *format, const char * const *kwlist,
                 va_list *p_va, int flags);
static int
qp_capi_vgetargskeywords_impl(PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwargs, PyObject *kwnames,
                      const char *format, const char * const *kwlist,
                      va_list *p_va, int flags);
static int qp_capi_vgetargskeywordsfast(PyObject *, PyObject *,
                            struct _PyArg_Parser *, va_list *, int);
static int qp_capi_vgetargskeywordsfast_impl(PyObject *const *args, Py_ssize_t nargs,
                          PyObject *keywords, PyObject *kwnames,
                          struct _PyArg_Parser *parser,
                          va_list *p_va, int flags);
static const char *qp_capi_skipitem(const char **, va_list *, int);

int
PyArg_Parse(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = qp_capi_vgetargs1(args, format, &va, QP_CAPI_FLAG_COMPAT);
    va_end(va);
    return retval;
}

PyAPI_FUNC(int)
_PyArg_Parse_SizeT(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = qp_capi_vgetargs1(args, format, &va, QP_CAPI_FLAG_COMPAT);
    va_end(va);
    return retval;
}


int
PyArg_ParseTuple(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = qp_capi_vgetargs1(args, format, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseTuple_SizeT(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = qp_capi_vgetargs1(args, format, &va, 0);
    va_end(va);
    return retval;
}


int
_PyArg_ParseStack(PyObject *const *args, Py_ssize_t nargs, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = qp_capi_vgetargs1_impl(NULL, args, nargs, format, &va, 0);
    va_end(va);
    return retval;
}

int
PyArg_ParseArray(PyObject *const *args, Py_ssize_t nargs, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    int retval = qp_capi_vgetargs1_impl(NULL, args, nargs, format, &va, 0);
    va_end(va);
    return retval;
}

int
PyArg_ParseArrayAndKeywords(PyObject *const *args, Py_ssize_t nargs,
                            PyObject *kwnames,
                            const char *format,
                            const char * const *kwlist, ...)
{
    if ((args == NULL && nargs != 0) ||
        (kwnames != NULL && !PyTuple_Check(kwnames)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_list va;
    va_start(va, kwlist);
    int retval = qp_capi_vgetargskeywords_impl(args, nargs, NULL, kwnames, format,
                                       kwlist, &va, 0);
    va_end(va);
    return retval;
}


int
PyArg_VaParse(PyObject *args, const char *format, va_list va)
{
    va_list lva;
    int retval;

    va_copy(lva, va);

    retval = qp_capi_vgetargs1(args, format, &lva, 0);
    va_end(lva);
    return retval;
}

int
_PyArg_VaParse_SizeT(PyObject *args, const char *format, va_list va)
{
    va_list lva;
    int retval;

    va_copy(lva, va);

    retval = qp_capi_vgetargs1(args, format, &lva, 0);
    va_end(lva);
    return retval;
}


/* Handle cleanup of allocated memory in case of exception */

static int
qp_capi_cleanup_ptr(PyObject *self, void *ptr)
{
    void **pptr = (void **)ptr;
    PyMem_Free(*pptr);
    *pptr = NULL;
    return 0;
}

static int
qp_capi_cleanup_buffer(PyObject *self, void *ptr)
{
    Py_buffer *buf = (Py_buffer *)ptr;
    if (buf) {
        PyBuffer_Release(buf);
    }
    return 0;
}

static int
qp_capi_addcleanup(void *ptr, qp_capi_freelist_t *freelist, qp_capi_destr_t destructor)
{
    int index;

    index = freelist->first_available;
    freelist->first_available += 1;

    freelist->entries[index].item = ptr;
    freelist->entries[index].destructor = destructor;

    return 0;
}

static int
qp_capi_cleanreturn(int retval, qp_capi_freelist_t *freelist)
{
    int index;

    if (retval == 0) {
      /* A failure occurred, therefore execute all of the cleanup
         functions.
      */
      for (index = 0; index < freelist->first_available; ++index) {
          freelist->entries[index].destructor(NULL,
                                              freelist->entries[index].item);
      }
    }
    if (freelist->entries_malloced)
        PyMem_Free(freelist->entries);
    return retval;
}


static int
qp_capi_vgetargs1_impl(PyObject *compat_args, PyObject *const *stack, Py_ssize_t nargs, const char *format,
               va_list *p_va, int flags)
{
    char msgbuf[256];
    int levels[32];
    const char *fname = NULL;
    const char *message = NULL;
    int min = -1;
    int max = 0;
    int level = 0;
    int endfmt = 0;
    const char *formatsave = format;
    Py_ssize_t i;
    const char *msg;
    int compat = flags & QP_CAPI_FLAG_COMPAT;
    qp_capi_freelistentry_t static_entries[QP_CAPI_STATIC_FREELIST_ENTRIES];
    qp_capi_freelist_t freelist;

    assert(nargs == 0 || stack != NULL);

    freelist.entries = static_entries;
    freelist.first_available = 0;
    freelist.entries_malloced = 0;

    flags = flags & ~QP_CAPI_FLAG_COMPAT;

    while (endfmt == 0) {
        int c = *format++;
        switch (c) {
        case '(':
            if (level == 0)
                max++;
            level++;
            if (level >= 30)
                Py_FatalError("too many tuple nesting levels "
                              "in argument format string");
            break;
        case ')':
            if (level == 0)
                Py_FatalError("excess ')' in getargs format");
            else
                level--;
            break;
        case '\0':
            endfmt = 1;
            break;
        case ':':
            fname = format;
            endfmt = 1;
            break;
        case ';':
            message = format;
            endfmt = 1;
            break;
        case '|':
            if (level == 0)
                min = max;
            break;
        default:
            if (level == 0) {
                if (Py_ISALPHA(c))
                    if (c != 'e') /* skip encoded */
                        max++;
            }
            break;
        }
    }

    if (level != 0)
        Py_FatalError(/* '(' */ "missing ')' in getargs format");

    if (min < 0)
        min = max;

    format = formatsave;

    if (max > QP_CAPI_STATIC_FREELIST_ENTRIES) {
        freelist.entries = PyMem_NEW(qp_capi_freelistentry_t, max);
        if (freelist.entries == NULL) {
            PyErr_NoMemory();
            return 0;
        }
        freelist.entries_malloced = 1;
    }

    if (compat) {
        if (max == 0) {
            if (compat_args == NULL)
                return 1;
            PyErr_Format(PyExc_TypeError,
                         "%.200s%s takes no arguments",
                         fname==NULL ? "function" : fname,
                         fname==NULL ? "" : "()");
            return qp_capi_cleanreturn(0, &freelist);
        }
        else if (min == 1 && max == 1) {
            if (compat_args == NULL) {
                PyErr_Format(PyExc_TypeError,
                             "%.200s%s takes at least one argument",
                             fname==NULL ? "function" : fname,
                             fname==NULL ? "" : "()");
                return qp_capi_cleanreturn(0, &freelist);
            }
            msg = qp_capi_convertitem(compat_args, &format, p_va, flags, levels,
                              msgbuf, sizeof(msgbuf), &freelist);
            if (msg == NULL)
                return qp_capi_cleanreturn(1, &freelist);
            qp_capi_seterror(levels[0], msg, levels+1, fname, message);
            return qp_capi_cleanreturn(0, &freelist);
        }
        else {
            PyErr_SetString(PyExc_SystemError,
                "old style getargs format uses new features");
            return qp_capi_cleanreturn(0, &freelist);
        }
    }

    if (nargs < min || max < nargs) {
        if (message == NULL)
            PyErr_Format(PyExc_TypeError,
                         "%.150s%s takes %s %d argument%s (%zd given)",
                         fname==NULL ? "function" : fname,
                         fname==NULL ? "" : "()",
                         min==max ? "exactly"
                         : nargs < min ? "at least" : "at most",
                         nargs < min ? min : max,
                         (nargs < min ? min : max) == 1 ? "" : "s",
                         nargs);
        else
            PyErr_SetString(PyExc_TypeError, message);
        return qp_capi_cleanreturn(0, &freelist);
    }

    for (i = 0; i < nargs; i++) {
        if (*format == '|')
            format++;
        msg = qp_capi_convertitem(stack[i], &format, p_va,
                          flags, levels, msgbuf,
                          sizeof(msgbuf), &freelist);
        if (msg) {
            qp_capi_seterror(i+1, msg, levels, fname, message);
            return qp_capi_cleanreturn(0, &freelist);
        }
    }

    if (*format != '\0' && !Py_ISALPHA(*format) &&
        *format != '(' &&
        *format != '|' && *format != ':' && *format != ';') {
        PyErr_Format(PyExc_SystemError,
                     "bad format string: %.200s", formatsave);
        return qp_capi_cleanreturn(0, &freelist);
    }

    return qp_capi_cleanreturn(1, &freelist);
}

static int
qp_capi_vgetargs1(PyObject *args, const char *format, va_list *p_va, int flags)
{
    PyObject **stack;
    Py_ssize_t nargs;

    if (!(flags & QP_CAPI_FLAG_COMPAT)) {
        assert(args != NULL);

        if (!PyTuple_Check(args)) {
            PyErr_SetString(PyExc_SystemError,
                "new style getargs format but argument is not a tuple");
            return 0;
        }

        stack = &PyTuple_GET_ITEM(args, 0);
        nargs = PyTuple_GET_SIZE(args);
    }
    else {
        stack = NULL;
        nargs = 0;
    }

    return qp_capi_vgetargs1_impl(args, stack, nargs, format, p_va, flags);
}


static void
qp_capi_seterror(Py_ssize_t iarg, const char *msg, int *levels, const char *fname,
         const char *message)
{
    char buf[512];
    int i;
    char *p = buf;

    if (PyErr_Occurred())
        return;
    else if (message == NULL) {
        if (fname != NULL) {
            PyOS_snprintf(p, sizeof(buf), "%.200s() ", fname);
            p += strlen(p);
        }
        if (iarg != 0) {
            PyOS_snprintf(p, sizeof(buf) - (p - buf),
                          "argument %zd", iarg);
            i = 0;
            p += strlen(p);
            while (i < 32 && levels[i] > 0 && (int)(p-buf) < 220) {
                PyOS_snprintf(p, sizeof(buf) - (p - buf),
                              ", item %d", levels[i]-1);
                p += strlen(p);
                i++;
            }
        }
        else {
            PyOS_snprintf(p, sizeof(buf) - (p - buf), "argument");
            p += strlen(p);
        }
        PyOS_snprintf(p, sizeof(buf) - (p - buf), " %.256s", msg);
        message = buf;
    }
    if (msg[0] == '(') {
        PyErr_SetString(PyExc_SystemError, message);
    }
    else {
        PyErr_SetString(PyExc_TypeError, message);
    }
}


/* Convert a tuple argument.
   On entry, *p_format points to the character _after_ the opening '('.
   On successful exit, *p_format points to the closing ')'.
   If successful:
      *p_format and *p_va are updated,
      *levels and *msgbuf are untouched,
      and NULL is returned.
   If the argument is invalid:
      *p_format is unchanged,
      *p_va is undefined,
      *levels is a 0-terminated list of item numbers,
      *msgbuf contains an error message, whose format is:
     "must be <typename1>, not <typename2>", where:
        <typename1> is the name of the expected type, and
        <typename2> is the name of the actual type,
      and msgbuf is returned.
*/

static const char *
qp_capi_converttuple(PyObject *arg, const char **p_format, va_list *p_va, int flags,
             int *levels, char *msgbuf, size_t bufsize,
             qp_capi_freelist_t *freelist)
{
    int level = 0;
    int n = 0;
    const char *format = *p_format;
    int i;
    Py_ssize_t len;
    int istuple = PyTuple_Check(arg);
    int mustbetuple = istuple;

    for (;;) {
        int c = *format++;
        if (c == '(') {
            if (level == 0)
                n++;
            level++;
        }
        else if (c == ')') {
            if (level == 0)
                break;
            level--;
        }
        else if (c == ':' || c == ';' || c == '\0')
            break;
        else {
            if (level == 0 && Py_ISALPHA(c)) {
                n++;
            }
            if (c == 'e' && (*format == 's' || *format == 't')) {
                format++;
                continue;
            }
            if (!mustbetuple) {
                switch (c) {
                    case 'y':
                    case 's':
                    case 'z':
                        if (*format != '*') {
                            mustbetuple = 1;
                        }
                        break;
                    case 'S':
                    case 'Y':
                    case 'U':
                        mustbetuple = 1;
                        break;
                    case 'O':
                        if (*format != '&') {
                            mustbetuple = 1;
                        }
                        break;
                }
            }
        }
    }

    if (istuple) {
        /* fallthrough */
    }
    else if (!PySequence_Check(arg) ||
        PyUnicode_Check(arg) || PyBytes_Check(arg) || PyByteArray_Check(arg))
    {
        levels[0] = 0;
        PyOS_snprintf(msgbuf, bufsize,
                      "must be %d-item tuple, not %.50s",
                  n,
                  arg == Py_None ? "None" : Py_TYPE(arg)->tp_name);
        return msgbuf;
    }
    else {
        if (mustbetuple) {
            if (PyErr_WarnFormat(PyExc_DeprecationWarning, 0,
                    "argument must be %d-item tuple, not %T", n, arg))
            {
                return msgbuf;
            }
        }
        len = PySequence_Size(arg);
        if (len != n) {
            levels[0] = 0;
            PyOS_snprintf(msgbuf, bufsize,
                          "must be %s of length %d, not %zd",
                          mustbetuple ? "tuple" : "sequence", n, len);
            return msgbuf;
        }
        arg = PySequence_Tuple(arg);
        if (arg == NULL) {
            return msgbuf;
        }
    }

    len = PyTuple_GET_SIZE(arg);
    if (len != n) {
        levels[0] = 0;
        PyOS_snprintf(msgbuf, bufsize,
                      "must be tuple of length %d, not %zd",
                      n, len);
        if (!istuple) {
            Py_DECREF(arg);
        }
        return msgbuf;
    }

    format = *p_format;
    for (i = 0; i < n; i++) {
        const char *msg;
        PyObject *item = PyTuple_GET_ITEM(arg, i);
        msg = qp_capi_convertitem(item, &format, p_va, flags, levels+1,
                          msgbuf, bufsize, freelist);
        if (msg != NULL) {
            levels[0] = i+1;
            if (!istuple) {
                Py_DECREF(arg);
            }
            return msg;
        }
    }

    *p_format = format;
    if (!istuple) {
        Py_DECREF(arg);
    }
    return NULL;
}


/* Convert a single item. */

static const char *
qp_capi_convertitem(PyObject *arg, const char **p_format, va_list *p_va, int flags,
            int *levels, char *msgbuf, size_t bufsize, qp_capi_freelist_t *freelist)
{
    const char *msg;
    const char *format = *p_format;

    if (*format == '(' /* ')' */) {
        format++;
        msg = qp_capi_converttuple(arg, &format, p_va, flags, levels, msgbuf,
                           bufsize, freelist);
        if (msg == NULL)
            format++;
    }
    else {
        msg = qp_capi_convertsimple(arg, &format, p_va, flags,
                            msgbuf, bufsize, freelist);
        if (msg != NULL)
            levels[0] = 0;
    }
    if (msg == NULL)
        *p_format = format;
    return msg;
}



/* Format an error message generated by qp_capi_convertsimple().
   displayname must be UTF-8 encoded.
*/

void
_PyArg_BadArgument(const char *fname, const char *displayname,
                   const char *expected, PyObject *arg)
{
    PyErr_Format(PyExc_TypeError,
                 "%.200s() %.200s must be %.50s, not %.50s",
                 fname, displayname, expected,
                 arg == Py_None ? "None" : Py_TYPE(arg)->tp_name);
}

static const char *
qp_capi_converterr(const char *expected, PyObject *arg, char *msgbuf, size_t bufsize)
{
    assert(expected != NULL);
    assert(arg != NULL);
    if (expected[0] == '(') {
        PyOS_snprintf(msgbuf, bufsize,
                      "%.100s", expected);
    }
    else {
        PyOS_snprintf(msgbuf, bufsize,
                      "must be %.50s, not %.50s", expected,
                      arg == Py_None ? "None" : Py_TYPE(arg)->tp_name);
    }
    return msgbuf;
}

static const char *
qp_capi_convertcharerr(const char *expected, const char *what, Py_ssize_t size,
               char *msgbuf, size_t bufsize)
{
    assert(expected != NULL);
    PyOS_snprintf(msgbuf, bufsize,
                  "must be %.50s, not %.50s of length %zd",
                  expected, what, size);
    return msgbuf;
}

#define QP_CAPI_CONV_UNICODE "(unicode conversion error)"

/* Convert a non-tuple argument.  Return NULL if conversion went OK,
   or a string with a message describing the failure.  The message is
   formatted as "must be <desired type>, not <actual type>".
   When failing, an exception may or may not have been raised.
   Don't call if a tuple is expected.

   When you add new format codes, please don't forget poor qp_capi_skipitem() below.
*/

static const char *
qp_capi_convertsimple(PyObject *arg, const char **p_format, va_list *p_va, int flags,
              char *msgbuf, size_t bufsize, qp_capi_freelist_t *freelist)
{
#define QP_CAPI_RETURN_ERR_OCCURRED return msgbuf

    const char *format = *p_format;
    char c = *format++;
    const char *sarg;

    switch (c) {

    case 'b': { /* unsigned byte -- very short int */
        unsigned char *p = va_arg(*p_va, unsigned char *);
        long ival = PyLong_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else if (ival < 0) {
            PyErr_SetString(PyExc_OverflowError,
                            "unsigned byte integer is less than minimum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else if (ival > UCHAR_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "unsigned byte integer is greater than maximum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else
            *p = (unsigned char) ival;
        break;
    }

    case 'B': {/* byte sized bitfield - both signed and unsigned
                  values allowed */
        unsigned char *p = va_arg(*p_va, unsigned char *);
        Py_ssize_t bytes = PyLong_AsNativeBytes(arg, p, sizeof(unsigned char),
                Py_ASNATIVEBYTES_NATIVE_ENDIAN |
                Py_ASNATIVEBYTES_ALLOW_INDEX |
                Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        if (bytes < 0) {
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        if ((size_t)bytes > sizeof(unsigned char)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                "integer value out of range", 1) < 0)
            {
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 'h': {/* signed short int */
        short *p = va_arg(*p_va, short *);
        long ival = PyLong_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else if (ival < SHRT_MIN) {
            PyErr_SetString(PyExc_OverflowError,
                            "signed short integer is less than minimum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else if (ival > SHRT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "signed short integer is greater than maximum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else
            *p = (short) ival;
        break;
    }

    case 'H': { /* short int sized bitfield, both signed and
                   unsigned allowed */
        unsigned short *p = va_arg(*p_va, unsigned short *);
        Py_ssize_t bytes = PyLong_AsNativeBytes(arg, p, sizeof(unsigned short),
                Py_ASNATIVEBYTES_NATIVE_ENDIAN |
                Py_ASNATIVEBYTES_ALLOW_INDEX |
                Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        if (bytes < 0) {
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        if ((size_t)bytes > sizeof(unsigned short)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                "integer value out of range", 1) < 0)
            {
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 'i': {/* signed int */
        int *p = va_arg(*p_va, int *);
        long ival = PyLong_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else if (ival > INT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "signed integer is greater than maximum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else if (ival < INT_MIN) {
            PyErr_SetString(PyExc_OverflowError,
                            "signed integer is less than minimum");
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        else
            *p = ival;
        break;
    }

    case 'I': { /* int sized bitfield, both signed and
                   unsigned allowed */
        unsigned int *p = va_arg(*p_va, unsigned int *);
        Py_ssize_t bytes = PyLong_AsNativeBytes(arg, p, sizeof(unsigned int),
                Py_ASNATIVEBYTES_NATIVE_ENDIAN |
                Py_ASNATIVEBYTES_ALLOW_INDEX |
                Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        if (bytes < 0) {
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        if ((size_t)bytes > sizeof(unsigned int)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                "integer value out of range", 1) < 0)
            {
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 'n': /* Py_ssize_t */
    {
        PyObject *iobj;
        Py_ssize_t *p = va_arg(*p_va, Py_ssize_t *);
        Py_ssize_t ival = -1;
        iobj = _PyNumber_Index(arg);
        if (iobj != NULL) {
            ival = PyLong_AsSsize_t(iobj);
            Py_DECREF(iobj);
        }
        if (ival == -1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        *p = ival;
        break;
    }
    case 'l': {/* long int */
        long *p = va_arg(*p_va, long *);
        long ival = PyLong_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else
            *p = ival;
        break;
    }

    case 'k': { /* long sized bitfield */
        unsigned long *p = va_arg(*p_va, unsigned long *);
        if (!PyIndex_Check(arg)) {
            return qp_capi_converterr("int", arg, msgbuf, bufsize);
        }
        Py_ssize_t bytes = PyLong_AsNativeBytes(arg, p, sizeof(unsigned long),
                Py_ASNATIVEBYTES_NATIVE_ENDIAN |
                Py_ASNATIVEBYTES_ALLOW_INDEX |
                Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        if (bytes < 0) {
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        if ((size_t)bytes > sizeof(unsigned long)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                "integer value out of range", 1) < 0)
            {
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 'L': {/* long long */
        long long *p = va_arg( *p_va, long long * );
        long long ival = PyLong_AsLongLong(arg);
        if (ival == (long long)-1 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else
            *p = ival;
        break;
    }

    case 'K': { /* long long sized bitfield */
        unsigned long long *p = va_arg(*p_va, unsigned long long *);
        if (!PyIndex_Check(arg)) {
            return qp_capi_converterr("int", arg, msgbuf, bufsize);
        }
        Py_ssize_t bytes = PyLong_AsNativeBytes(arg, p, sizeof(unsigned long long),
                Py_ASNATIVEBYTES_NATIVE_ENDIAN |
                Py_ASNATIVEBYTES_ALLOW_INDEX |
                Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        if (bytes < 0) {
            QP_CAPI_RETURN_ERR_OCCURRED;
        }
        if ((size_t)bytes > sizeof(unsigned long long)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                "integer value out of range", 1) < 0)
            {
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 'f': {/* float */
        float *p = va_arg(*p_va, float *);
        double dval = PyFloat_AsDouble(arg);
        if (dval == -1.0 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else
            *p = (float) dval;
        break;
    }

    case 'd': {/* double */
        double *p = va_arg(*p_va, double *);
        double dval = PyFloat_AsDouble(arg);
        if (dval == -1.0 && PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else
            *p = dval;
        break;
    }

    case 'D': {/* complex double */
        Py_complex *p = va_arg(*p_va, Py_complex *);
        Py_complex cval;
        cval = PyComplex_AsCComplex(arg);
        if (PyErr_Occurred())
            QP_CAPI_RETURN_ERR_OCCURRED;
        else
            *p = cval;
        break;
    }

    case 'c': {/* char */
        char *p = va_arg(*p_va, char *);
        if (PyBytes_Check(arg)) {
            if (PyBytes_GET_SIZE(arg) != 1) {
                return qp_capi_convertcharerr("a byte string of length 1",
                                      "a bytes object", PyBytes_GET_SIZE(arg),
                                      msgbuf, bufsize);
            }
            *p = PyBytes_AS_STRING(arg)[0];
        }
        else if (PyByteArray_Check(arg)) {
            if (PyByteArray_GET_SIZE(arg) != 1) {
                return qp_capi_convertcharerr("a byte string of length 1",
                                      "a bytearray object", PyByteArray_GET_SIZE(arg),
                                      msgbuf, bufsize);
            }
            *p = PyByteArray_AS_STRING(arg)[0];
        }
        else
            return qp_capi_converterr("a byte string of length 1", arg, msgbuf, bufsize);
        break;
    }

    case 'C': {/* unicode char */
        int *p = va_arg(*p_va, int *);
        int kind;
        const void *data;

        if (!PyUnicode_Check(arg))
            return qp_capi_converterr("a unicode character", arg, msgbuf, bufsize);

        if (PyUnicode_GET_LENGTH(arg) != 1) {
            return qp_capi_convertcharerr("a unicode character",
                                  "a string", PyUnicode_GET_LENGTH(arg),
                                  msgbuf, bufsize);
        }

        kind = PyUnicode_KIND(arg);
        data = PyUnicode_DATA(arg);
        *p = PyUnicode_READ(kind, data, 0);
        break;
    }

    case 'p': {/* boolean *p*redicate */
        int *p = va_arg(*p_va, int *);
        int val = PyObject_IsTrue(arg);
        if (val > 0)
            *p = 1;
        else if (val == 0)
            *p = 0;
        else
            QP_CAPI_RETURN_ERR_OCCURRED;
        break;
    }

    /* XXX WAAAAH!  's', 'y', 'z', 'u', 'Z', 'e', 'w' codes all
       need to be cleaned up! */

    case 'y': {/* any bytes-like object */
        void **p = (void **)va_arg(*p_va, char **);
        const char *buf;
        Py_ssize_t count;
        if (*format == '*') {
            if (qp_capi_getbuffer(arg, (Py_buffer*)p, &buf) < 0)
                return qp_capi_converterr(buf, arg, msgbuf, bufsize);
            format++;
            if (qp_capi_addcleanup(p, freelist, qp_capi_cleanup_buffer)) {
                return qp_capi_converterr(
                    "(cleanup problem)",
                    arg, msgbuf, bufsize);
            }
            break;
        }
        count = qp_capi_convertbuffer(arg, (const void **)p, &buf);
        if (count < 0)
            return qp_capi_converterr(buf, arg, msgbuf, bufsize);
        if (*format == '#') {
            Py_ssize_t *psize = va_arg(*p_va, Py_ssize_t*);
            *psize = count;
            format++;
        } else {
            if (strlen(*p) != (size_t)count) {
                PyErr_SetString(PyExc_ValueError, "embedded null byte");
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
        }
        break;
    }

    case 's': /* text string or bytes-like object */
    case 'z': /* text string, bytes-like object or None */
    {
        if (*format == '*') {
            /* "s*" or "z*" */
            Py_buffer *p = (Py_buffer *)va_arg(*p_va, Py_buffer *);

            if (c == 'z' && arg == Py_None)
                PyBuffer_FillInfo(p, NULL, NULL, 0, 1, 0);
            else if (PyUnicode_Check(arg)) {
                Py_ssize_t len;
                sarg = PyUnicode_AsUTF8AndSize(arg, &len);
                if (sarg == NULL)
                    return qp_capi_converterr(QP_CAPI_CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                PyBuffer_FillInfo(p, arg, (void *)sarg, len, 1, 0);
            }
            else { /* any bytes-like object */
                const char *buf;
                if (qp_capi_getbuffer(arg, p, &buf) < 0)
                    return qp_capi_converterr(buf, arg, msgbuf, bufsize);
            }
            if (qp_capi_addcleanup(p, freelist, qp_capi_cleanup_buffer)) {
                return qp_capi_converterr(
                    "(cleanup problem)",
                    arg, msgbuf, bufsize);
            }
            format++;
        } else if (*format == '#') { /* a string or read-only bytes-like object */
            /* "s#" or "z#" */
            const void **p = (const void **)va_arg(*p_va, const char **);
            Py_ssize_t *psize = va_arg(*p_va, Py_ssize_t*);

            if (c == 'z' && arg == Py_None) {
                *p = NULL;
                *psize = 0;
            }
            else if (PyUnicode_Check(arg)) {
                Py_ssize_t len;
                sarg = PyUnicode_AsUTF8AndSize(arg, &len);
                if (sarg == NULL)
                    return qp_capi_converterr(QP_CAPI_CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                *p = sarg;
                *psize = len;
            }
            else { /* read-only bytes-like object */
                /* XXX Really? */
                const char *buf;
                Py_ssize_t count = qp_capi_convertbuffer(arg, p, &buf);
                if (count < 0)
                    return qp_capi_converterr(buf, arg, msgbuf, bufsize);
                *psize = count;
            }
            format++;
        } else {
            /* "s" or "z" */
            const char **p = va_arg(*p_va, const char **);
            Py_ssize_t len;
            sarg = NULL;

            if (c == 'z' && arg == Py_None)
                *p = NULL;
            else if (PyUnicode_Check(arg)) {
                sarg = PyUnicode_AsUTF8AndSize(arg, &len);
                if (sarg == NULL)
                    return qp_capi_converterr(QP_CAPI_CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                if (strlen(sarg) != (size_t)len) {
                    PyErr_SetString(PyExc_ValueError, "embedded null character");
                    QP_CAPI_RETURN_ERR_OCCURRED;
                }
                *p = sarg;
            }
            else
                return qp_capi_converterr(c == 'z' ? "str or None" : "str",
                                  arg, msgbuf, bufsize);
        }
        break;
    }

    case 'e': {/* encoded string */
        char **buffer;
        const char *encoding;
        PyObject *s;
        int recode_strings;
        Py_ssize_t size;
        const char *ptr;

        /* Get 'e' parameter: the encoding name */
        encoding = (const char *)va_arg(*p_va, const char *);
        if (encoding == NULL)
            encoding = PyUnicode_GetDefaultEncoding();

        /* Get output buffer parameter:
           's' (recode all objects via Unicode) or
           't' (only recode non-string objects)
        */
        if (*format == 's')
            recode_strings = 1;
        else if (*format == 't')
            recode_strings = 0;
        else
            return qp_capi_converterr(
                "(unknown parser marker combination)",
                arg, msgbuf, bufsize);
        buffer = (char **)va_arg(*p_va, char **);
        format++;
        if (buffer == NULL)
            return qp_capi_converterr("(buffer is NULL)",
                              arg, msgbuf, bufsize);

        /* Encode object */
        if (!recode_strings &&
            (PyBytes_Check(arg) || PyByteArray_Check(arg))) {
            s = Py_NewRef(arg);
            if (PyBytes_Check(arg)) {
                size = PyBytes_GET_SIZE(s);
                ptr = PyBytes_AS_STRING(s);
            }
            else {
                size = PyByteArray_GET_SIZE(s);
                ptr = PyByteArray_AS_STRING(s);
            }
        }
        else if (PyUnicode_Check(arg)) {
            /* Encode object; use default error handling */
            s = PyUnicode_AsEncodedString(arg,
                                          encoding,
                                          NULL);
            if (s == NULL)
                return qp_capi_converterr("(encoding failed)",
                                  arg, msgbuf, bufsize);
            assert(PyBytes_Check(s));
            size = PyBytes_GET_SIZE(s);
            ptr = PyBytes_AS_STRING(s);
            if (ptr == NULL)
                ptr = "";
        }
        else {
            return qp_capi_converterr(
                recode_strings ? "str" : "str, bytes or bytearray",
                arg, msgbuf, bufsize);
        }

        /* Write output; output is guaranteed to be 0-terminated */
        if (*format == '#') {
            /* Using buffer length parameter '#':

               - if *buffer is NULL, a new buffer of the
               needed size is allocated and the data
               copied into it; *buffer is updated to point
               to the new buffer; the caller is
               responsible for PyMem_Free()ing it after
               usage

               - if *buffer is not NULL, the data is
               copied to *buffer; *buffer_len has to be
               set to the size of the buffer on input;
               buffer overflow is signalled with an error;
               buffer has to provide enough room for the
               encoded string plus the trailing 0-byte

               - in both cases, *buffer_len is updated to
               the size of the buffer /excluding/ the
               trailing 0-byte

            */
            Py_ssize_t *psize = va_arg(*p_va, Py_ssize_t*);

            format++;
            if (psize == NULL) {
                Py_DECREF(s);
                return qp_capi_converterr(
                    "(buffer_len is NULL)",
                    arg, msgbuf, bufsize);
            }
            if (*buffer == NULL) {
                *buffer = PyMem_NEW(char, size + 1);
                if (*buffer == NULL) {
                    Py_DECREF(s);
                    PyErr_NoMemory();
                    QP_CAPI_RETURN_ERR_OCCURRED;
                }
                if (qp_capi_addcleanup(buffer, freelist, qp_capi_cleanup_ptr)) {
                    Py_DECREF(s);
                    return qp_capi_converterr(
                        "(cleanup problem)",
                        arg, msgbuf, bufsize);
                }
            } else {
                if (size + 1 > *psize) {
                    Py_DECREF(s);
                    PyErr_Format(PyExc_ValueError,
                                 "encoded string too long "
                                 "(%zd, maximum length %zd)",
                                 (Py_ssize_t)size, (Py_ssize_t)(*psize - 1));
                    QP_CAPI_RETURN_ERR_OCCURRED;
                }
            }
            memcpy(*buffer, ptr, size+1);

            *psize = size;
        }
        else {
            /* Using a 0-terminated buffer:

               - the encoded string has to be 0-terminated
               for this variant to work; if it is not, an
               error raised

               - a new buffer of the needed size is
               allocated and the data copied into it;
               *buffer is updated to point to the new
               buffer; the caller is responsible for
               PyMem_Free()ing it after usage

            */
            if ((Py_ssize_t)strlen(ptr) != size) {
                Py_DECREF(s);
                return qp_capi_converterr(
                    "encoded string without null bytes",
                    arg, msgbuf, bufsize);
            }
            *buffer = PyMem_NEW(char, size + 1);
            if (*buffer == NULL) {
                Py_DECREF(s);
                PyErr_NoMemory();
                QP_CAPI_RETURN_ERR_OCCURRED;
            }
            if (qp_capi_addcleanup(buffer, freelist, qp_capi_cleanup_ptr)) {
                Py_DECREF(s);
                return qp_capi_converterr("(cleanup problem)",
                                arg, msgbuf, bufsize);
            }
            memcpy(*buffer, ptr, size+1);
        }
        Py_DECREF(s);
        break;
    }

    case 'S': { /* PyBytes object */
        PyObject **p = va_arg(*p_va, PyObject **);
        if (PyBytes_Check(arg))
            *p = arg;
        else
            return qp_capi_converterr("bytes", arg, msgbuf, bufsize);
        break;
    }

    case 'Y': { /* PyByteArray object */
        PyObject **p = va_arg(*p_va, PyObject **);
        if (PyByteArray_Check(arg))
            *p = arg;
        else
            return qp_capi_converterr("bytearray", arg, msgbuf, bufsize);
        break;
    }

    case 'U': { /* PyUnicode object */
        PyObject **p = va_arg(*p_va, PyObject **);
        if (PyUnicode_Check(arg)) {
            *p = arg;
        }
        else
            return qp_capi_converterr("str", arg, msgbuf, bufsize);
        break;
    }

    case 'O': { /* object */
        PyTypeObject *type;
        PyObject **p;
        if (*format == '!') {
            type = va_arg(*p_va, PyTypeObject*);
            p = va_arg(*p_va, PyObject **);
            format++;
            if (PyType_IsSubtype(Py_TYPE(arg), type))
                *p = arg;
            else
                return qp_capi_converterr(type->tp_name, arg, msgbuf, bufsize);

        }
        else if (*format == '&') {
            typedef int (*converter)(PyObject *, void *);
            converter convert = va_arg(*p_va, converter);
            void *addr = va_arg(*p_va, void *);
            int res;
            format++;
            if (! (res = (*convert)(arg, addr)))
                return qp_capi_converterr("(unspecified)",
                                  arg, msgbuf, bufsize);
            if (res == Py_CLEANUP_SUPPORTED &&
                qp_capi_addcleanup(addr, freelist, convert) == -1)
                return qp_capi_converterr("(cleanup problem)",
                                arg, msgbuf, bufsize);
        }
        else {
            p = va_arg(*p_va, PyObject **);
            *p = arg;
        }
        break;
    }


    case 'w': { /* "w*": memory buffer, read-write access */
        void **p = va_arg(*p_va, void **);

        if (*format != '*')
            return qp_capi_converterr(
                "(invalid use of 'w' format character)",
                arg, msgbuf, bufsize);
        format++;

        /* Caller is interested in Py_buffer, and the object supports it
           directly. The request implicitly asks for PyBUF_SIMPLE, so the
           result is C-contiguous with format 'B'. */
        if (PyObject_GetBuffer(arg, (Py_buffer*)p, PyBUF_WRITABLE) < 0) {
            PyErr_Clear();
            return qp_capi_converterr("read-write bytes-like object",
                              arg, msgbuf, bufsize);
        }
        assert(PyBuffer_IsContiguous((Py_buffer *)p, 'C'));
        if (qp_capi_addcleanup(p, freelist, qp_capi_cleanup_buffer)) {
            return qp_capi_converterr(
                "(cleanup problem)",
                arg, msgbuf, bufsize);
        }
        break;
    }

    default:
        return qp_capi_converterr("(impossible<bad format char>)", arg, msgbuf, bufsize);

    }

    *p_format = format;
    return NULL;

#undef QP_CAPI_RETURN_ERR_OCCURRED
}

static Py_ssize_t
qp_capi_convertbuffer(PyObject *arg, const void **p, const char **errmsg)
{
    PyBufferProcs *pb = Py_TYPE(arg)->tp_as_buffer;
    Py_ssize_t count;
    Py_buffer view;

    *errmsg = NULL;
    *p = NULL;
    if (pb != NULL && pb->bf_releasebuffer != NULL) {
        *errmsg = "read-only bytes-like object";
        return -1;
    }

    if (qp_capi_getbuffer(arg, &view, errmsg) < 0)
        return -1;
    count = view.len;
    *p = view.buf;
    PyBuffer_Release(&view);
    return count;
}

static int
qp_capi_getbuffer(PyObject *arg, Py_buffer *view, const char **errmsg)
{
    /* PyBUF_SIMPLE implies C-contiguous */
    if (PyObject_GetBuffer(arg, view, PyBUF_SIMPLE) != 0) {
        *errmsg = "bytes-like object";
        return -1;
    }
    assert(PyBuffer_IsContiguous(view, 'C'));
    return 0;
}

/* Support for keyword arguments donated by
   Geoff Philbrick <philbric@delphi.hks.com> */

/* Return false (0) for error, else true. */
int
PyArg_ParseTupleAndKeywords(PyObject *args,
                            PyObject *keywords,
                            const char *format,
                            PY_CXX_CONST char * const *kwlist, ...)
{
    int retval;
    va_list va;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_start(va, kwlist);
    /* qp: PY_CXX_CONST is empty in C; CPython compiles this file with it
       defined as const. */
    retval = qp_capi_vgetargskeywords(args, keywords, format,
                                      (const char * const *)kwlist, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseTupleAndKeywords_SizeT(PyObject *args,
                                  PyObject *keywords,
                                  const char *format,
                                  const char * const *kwlist, ...)
{
    int retval;
    va_list va;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_start(va, kwlist);
    retval = qp_capi_vgetargskeywords(args, keywords, format,
                              kwlist, &va, 0);
    va_end(va);
    return retval;
}


int
PyArg_VaParseTupleAndKeywords(PyObject *args,
                              PyObject *keywords,
                              const char *format,
                              PY_CXX_CONST char * const *kwlist, va_list va)
{
    int retval;
    va_list lva;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_copy(lva, va);

    retval = qp_capi_vgetargskeywords(args, keywords, format,
                                      (const char * const *)kwlist, &lva, 0);
    va_end(lva);
    return retval;
}

int
_PyArg_VaParseTupleAndKeywords_SizeT(PyObject *args,
                                    PyObject *keywords,
                                    const char *format,
                                    const char * const *kwlist, va_list va)
{
    int retval;
    va_list lva;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_copy(lva, va);

    retval = qp_capi_vgetargskeywords(args, keywords, format,
                              kwlist, &lva, 0);
    va_end(lva);
    return retval;
}

PyAPI_FUNC(int)
_PyArg_ParseTupleAndKeywordsFast(PyObject *args, PyObject *keywords,
                            struct _PyArg_Parser *parser, ...)
{
    int retval;
    va_list va;

    va_start(va, parser);
    retval = qp_capi_vgetargskeywordsfast(args, keywords, parser, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseTupleAndKeywordsFast_SizeT(PyObject *args, PyObject *keywords,
                            struct _PyArg_Parser *parser, ...)
{
    int retval;
    va_list va;

    va_start(va, parser);
    retval = qp_capi_vgetargskeywordsfast(args, keywords, parser, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseStackAndKeywords(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames,
                  struct _PyArg_Parser *parser, ...)
{
    int retval;
    va_list va;

    va_start(va, parser);
    retval = qp_capi_vgetargskeywordsfast_impl(args, nargs, NULL, kwnames, parser, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseStackAndKeywords_SizeT(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames,
                        struct _PyArg_Parser *parser, ...)
{
    int retval;
    va_list va;

    va_start(va, parser);
    retval = qp_capi_vgetargskeywordsfast_impl(args, nargs, NULL, kwnames, parser, &va, 0);
    va_end(va);
    return retval;
}


/* TU-static, defined in python/errors.c: new reference to the suggested
   str, QP_NULL when there is none, JS_EXCEPTION on error. */
static JSValue qp_calculate_suggestions(JSContext *ctx, JSValueConst dir_list,
                                        JSValueConst name);

/* qp: CPython's _Py_CalculateSuggestions (Python/suggestions.c), through
   the runtime's port of the same algorithm.  Returns a new reference to
   the suggested name, or NULL (with or without an exception set, as
   CPython). */
static PyObject *
qp_capi_calculate_suggestions(PyObject *dir, PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue jdir, jname, res;

    jdir = qp_capi_value(ctx, dir);
    if (JS_IsException(jdir)) {
        return NULL;
    }
    jname = qp_capi_value(ctx, name);
    if (JS_IsException(jname)) {
        return NULL;
    }
    res = qp_calculate_suggestions(ctx, jdir, jname);
    if (JS_IsException(res) || QP_IS_NULL(res) || JS_IsUndefined(res)) {
        return NULL;
    }
    return qp_capi_steal(ctx, res);
}

static void
qp_capi_error_unexpected_keyword_arg(PyObject *kwargs, PyObject *kwnames, PyObject *kwtuple, const char *fname)
{
    /* make sure there are no extraneous keyword arguments */
    Py_ssize_t j = 0;
    while (1) {
        PyObject *keyword;
        if (kwargs != NULL) {
            if (!PyDict_Next(kwargs, &j, &keyword, NULL))
                break;
        }
        else {
            if (j >= PyTuple_GET_SIZE(kwnames))
                break;
            keyword = PyTuple_GET_ITEM(kwnames, j);
            j++;
        }
        if (!PyUnicode_Check(keyword)) {
            PyErr_SetString(PyExc_TypeError,
                            "keywords must be strings");
            return;
        }

        int match = PySequence_Contains(kwtuple, keyword);
        if (match <= 0) {
            if (!match) {
                PyObject *kwlist = PySequence_List(kwtuple);
                if (!kwlist) {
                    return;
                }
                PyObject *suggestion_keyword = qp_capi_calculate_suggestions(kwlist, keyword);
                Py_DECREF(kwlist);

                if (suggestion_keyword) {
                    PyErr_Format(PyExc_TypeError,
                                "%.200s%s got an unexpected keyword argument '%S'."
                                " Did you mean '%S'?",
                                (fname == NULL) ? "this function" : fname,
                                (fname == NULL) ? "" : "()",
                                keyword,
                                suggestion_keyword);
                    Py_DECREF(suggestion_keyword);
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                "%.200s%s got an unexpected keyword argument '%S'",
                                (fname == NULL) ? "this function" : fname,
                                (fname == NULL) ? "" : "()",
                                keyword);
                }

            }
            return;
        }
    }
    /* Something wrong happened. There are extraneous keyword arguments,
     * but we don't know what. And we don't bother. */
    PyErr_Format(PyExc_TypeError,
                 "invalid keyword argument for %.200s%s",
                 (fname == NULL) ? "this function" : fname,
                 (fname == NULL) ? "" : "()");
}

/* qp: CPython's _PyDict_HasOnlyStringKeys. */
static int
qp_capi_dict_has_only_str_keys(PyObject *dict)
{
    Py_ssize_t pos = 0;
    PyObject *key;

    while (PyDict_Next(dict, &pos, &key, NULL)) {
        if (!PyUnicode_Check(key)) {
            return 0;
        }
    }
    return 1;
}

int
PyArg_ValidateKeywordArguments(PyObject *kwargs)
{
    if (!PyDict_Check(kwargs)) {
        PyErr_BadInternalCall();
        return 0;
    }
    if (!qp_capi_dict_has_only_str_keys(kwargs)) {
        PyErr_SetString(PyExc_TypeError,
                        "keywords must be strings");
        return 0;
    }
    return 1;
}

static PyObject *
qp_capi_new_kwtuple(const char * const *keywords, int total, int pos);

static PyObject*
qp_capi_find_keyword_str(PyObject *kwnames, PyObject *const *kwstack, const char *key)
{
    Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (PyUnicode_EqualToUTF8(kwname, key)) {
            return Py_NewRef(kwstack[i]);
        }
    }
    return NULL;
}

#define QP_CAPI_IS_END_OF_FORMAT(c) (c == '\0' || c == ';' || c == ':')

static int
qp_capi_vgetargskeywords_impl(PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwargs, PyObject *kwnames,
                      const char *format, const char * const *kwlist,
                      va_list *p_va, int flags)
{
    char msgbuf[512];
    int levels[32];
    const char *fname, *msg, *custom_msg;
    int min = INT_MAX;
    int max = INT_MAX;
    int i, pos, len;
    int skip = 0;
    Py_ssize_t nkwargs;
    qp_capi_freelistentry_t static_entries[QP_CAPI_STATIC_FREELIST_ENTRIES];
    qp_capi_freelist_t freelist;
    PyObject * const *kwstack = NULL;

    freelist.entries = static_entries;
    freelist.first_available = 0;
    freelist.entries_malloced = 0;

    assert(args != NULL || nargs == 0);
    assert(kwargs == NULL || PyDict_Check(kwargs));
    assert(kwnames == NULL || PyTuple_Check(kwnames));
    assert(format != NULL);
    assert(kwlist != NULL);
    assert(p_va != NULL);

    /* grab the function name or custom error msg first (mutually exclusive) */
    fname = strchr(format, ':');
    if (fname) {
        fname++;
        custom_msg = NULL;
    }
    else {
        custom_msg = strchr(format,';');
        if (custom_msg)
            custom_msg++;
    }

    /* scan kwlist and count the number of positional-only parameters */
    for (pos = 0; kwlist[pos] && !*kwlist[pos]; pos++) {
    }
    /* scan kwlist and get greatest possible nbr of args */
    for (len = pos; kwlist[len]; len++) {
        if (!*kwlist[len]) {
            PyErr_SetString(PyExc_SystemError,
                            "Empty keyword parameter name");
            return qp_capi_cleanreturn(0, &freelist);
        }
    }

    if (len > QP_CAPI_STATIC_FREELIST_ENTRIES) {
        freelist.entries = PyMem_NEW(qp_capi_freelistentry_t, len);
        if (freelist.entries == NULL) {
            PyErr_NoMemory();
            return 0;
        }
        freelist.entries_malloced = 1;
    }

    if (kwargs != NULL) {
        nkwargs = PyDict_GET_SIZE(kwargs);
    }
    else if (kwnames != NULL) {
        nkwargs = PyTuple_GET_SIZE(kwnames);
        kwstack = args + nargs;
    }
    else {
        nkwargs = 0;
    }
    if (nargs + nkwargs > len) {
        /* Adding "keyword" (when nargs == 0) prevents producing wrong error
           messages in some special cases (see bpo-31229). */
        PyErr_Format(PyExc_TypeError,
                     "%.200s%s takes at most %d %sargument%s (%zd given)",
                     (fname == NULL) ? "function" : fname,
                     (fname == NULL) ? "" : "()",
                     len,
                     (nargs == 0) ? "keyword " : "",
                     (len == 1) ? "" : "s",
                     nargs + nkwargs);
        return qp_capi_cleanreturn(0, &freelist);
    }

    /* convert tuple args and keyword args in same loop, using kwlist to drive process */
    for (i = 0; i < len; i++) {
        if (*format == '|') {
            if (min != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string (| specified twice)");
                return qp_capi_cleanreturn(0, &freelist);
            }

            min = i;
            format++;

            if (max != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string ($ before |)");
                return qp_capi_cleanreturn(0, &freelist);
            }
        }
        if (*format == '$') {
            if (max != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string ($ specified twice)");
                return qp_capi_cleanreturn(0, &freelist);
            }

            max = i;
            format++;

            if (max < pos) {
                PyErr_SetString(PyExc_SystemError,
                                "Empty parameter name after $");
                return qp_capi_cleanreturn(0, &freelist);
            }
            if (skip) {
                /* Now we know the minimal and the maximal numbers of
                 * positional arguments and can raise an exception with
                 * informative message (see below). */
                break;
            }
            if (max < nargs) {
                if (max == 0) {
                    PyErr_Format(PyExc_TypeError,
                                 "%.200s%s takes no positional arguments",
                                 (fname == NULL) ? "function" : fname,
                                 (fname == NULL) ? "" : "()");
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                 "%.200s%s takes %s %d positional argument%s"
                                 " (%zd given)",
                                 (fname == NULL) ? "function" : fname,
                                 (fname == NULL) ? "" : "()",
                                 (min != INT_MAX) ? "at most" : "exactly",
                                 max,
                                 max == 1 ? "" : "s",
                                 nargs);
                }
                return qp_capi_cleanreturn(0, &freelist);
            }
        }
        if (QP_CAPI_IS_END_OF_FORMAT(*format)) {
            PyErr_Format(PyExc_SystemError,
                         "More keyword list entries (%d) than "
                         "format specifiers (%d)", len, i);
            return qp_capi_cleanreturn(0, &freelist);
        }
        if (!skip) {
            PyObject *current_arg;
            if (i < nargs) {
                current_arg = Py_NewRef(args[i]);
            }
            else if (nkwargs && i >= pos) {
                if (kwargs != NULL) {
                    if (PyDict_GetItemStringRef(kwargs, kwlist[i], &current_arg) < 0) {
                        return qp_capi_cleanreturn(0, &freelist);
                    }
                }
                else {
                    current_arg = qp_capi_find_keyword_str(kwnames, kwstack, kwlist[i]);
                }
                if (current_arg) {
                    --nkwargs;
                }
            }
            else {
                current_arg = NULL;
            }

            if (current_arg) {
                msg = qp_capi_convertitem(current_arg, &format, p_va, flags,
                    levels, msgbuf, sizeof(msgbuf), &freelist);
                Py_DECREF(current_arg);
                if (msg) {
                    qp_capi_seterror(i+1, msg, levels, fname, custom_msg);
                    return qp_capi_cleanreturn(0, &freelist);
                }
                continue;
            }

            if (i < min) {
                if (i < pos) {
                    assert (min == INT_MAX);
                    assert (max == INT_MAX);
                    skip = 1;
                    /* At that moment we still don't know the minimal and
                     * the maximal numbers of positional arguments.  Raising
                     * an exception is deferred until we encounter | and $
                     * or the end of the format. */
                }
                else {
                    PyErr_Format(PyExc_TypeError,  "%.200s%s missing required "
                                 "argument '%s' (pos %d)",
                                 (fname == NULL) ? "function" : fname,
                                 (fname == NULL) ? "" : "()",
                                 kwlist[i], i+1);
                    return qp_capi_cleanreturn(0, &freelist);
                }
            }
            /* current code reports success when all required args
             * fulfilled and no keyword args left, with no further
             * validation. XXX Maybe skip this in debug build ?
             */
            if (!nkwargs && !skip) {
                return qp_capi_cleanreturn(1, &freelist);
            }
        }

        /* We are into optional args, skip through to any remaining
         * keyword args */
        msg = qp_capi_skipitem(&format, p_va, flags);
        if (msg) {
            PyErr_Format(PyExc_SystemError, "%s: '%s'", msg,
                         format);
            return qp_capi_cleanreturn(0, &freelist);
        }
    }

    if (skip) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s%s takes %s %d positional argument%s"
                     " (%zd given)",
                     (fname == NULL) ? "function" : fname,
                     (fname == NULL) ? "" : "()",
                     (Py_MIN(pos, min) < i) ? "at least" : "exactly",
                     Py_MIN(pos, min),
                     Py_MIN(pos, min) == 1 ? "" : "s",
                     nargs);
        return qp_capi_cleanreturn(0, &freelist);
    }

    if (!QP_CAPI_IS_END_OF_FORMAT(*format) && (*format != '|') && (*format != '$')) {
        PyErr_Format(PyExc_SystemError,
            "more argument specifiers than keyword list entries "
            "(remaining format:'%s')", format);
        return qp_capi_cleanreturn(0, &freelist);
    }

    if (nkwargs > 0) {
        PyObject *key;
        Py_ssize_t j;
        /* make sure there are no arguments given by name and position */
        for (i = pos; i < nargs; i++) {
            PyObject *current_arg;
            if (kwargs != NULL) {
                if (PyDict_GetItemStringRef(kwargs, kwlist[i], &current_arg) < 0) {
                    return qp_capi_cleanreturn(0, &freelist);
                }
            }
            else {
                current_arg = qp_capi_find_keyword_str(kwnames, kwstack, kwlist[i]);
            }
            if (current_arg) {
                Py_DECREF(current_arg);
                /* arg present in tuple and in dict */
                PyErr_Format(PyExc_TypeError,
                             "argument for %.200s%s given by name ('%s') "
                             "and position (%d)",
                             (fname == NULL) ? "function" : fname,
                             (fname == NULL) ? "" : "()",
                             kwlist[i], i+1);
                return qp_capi_cleanreturn(0, &freelist);
            }
        }
        /* make sure there are no extraneous keyword arguments */
        j = 0;
        while (1) {
            if (kwargs != NULL) {
                if (!PyDict_Next(kwargs, &j, &key, NULL)) {
                    break;
                }
            }
            else {
                if (j >= nkwargs) {
                    break;
                }
                key = PyTuple_GET_ITEM(kwnames, j);
                j++;
            }

            int match = 0;
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError,
                                "keywords must be strings");
                return qp_capi_cleanreturn(0, &freelist);
            }
            for (i = pos; i < len; i++) {
                if (PyUnicode_EqualToUTF8(key, kwlist[i])) {
                    match = 1;
                    break;
                }
            }
            if (!match) {
                PyObject *_pykwtuple = qp_capi_new_kwtuple(kwlist, len, pos);
                if (!_pykwtuple) {
                    return qp_capi_cleanreturn(0, &freelist);
                }
                PyObject *pykwlist = PySequence_List(_pykwtuple);
                Py_DECREF(_pykwtuple);
                if (!pykwlist) {
                    return qp_capi_cleanreturn(0, &freelist);
                }
                PyObject *suggestion_keyword = qp_capi_calculate_suggestions(pykwlist, key);
                Py_DECREF(pykwlist);

                if (suggestion_keyword) {
                    PyErr_Format(PyExc_TypeError,
                                "%.200s%s got an unexpected keyword argument '%S'."
                                " Did you mean '%S'?",
                                (fname == NULL) ? "this function" : fname,
                                (fname == NULL) ? "" : "()",
                                key,
                                suggestion_keyword);
                    Py_DECREF(suggestion_keyword);
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                "%.200s%s got an unexpected keyword argument '%S'",
                                (fname == NULL) ? "this function" : fname,
                                (fname == NULL) ? "" : "()",
                                key);
                }
                return qp_capi_cleanreturn(0, &freelist);
            }
        }
        /* Something wrong happened. There are extraneous keyword arguments,
         * but we don't know what. And we don't bother. */
        PyErr_Format(PyExc_TypeError,
                     "invalid keyword argument for %.200s%s",
                     (fname == NULL) ? "this function" : fname,
                     (fname == NULL) ? "" : "()");
        return qp_capi_cleanreturn(0, &freelist);
    }

    return qp_capi_cleanreturn(1, &freelist);
}

static int
qp_capi_vgetargskeywords(PyObject *argstuple, PyObject *kwargs,
                 const char *format, const char * const *kwlist,
                 va_list *p_va, int flags)
{
    PyObject *const *args = &PyTuple_GET_ITEM(argstuple, 0);
    Py_ssize_t nargs = PyTuple_GET_SIZE(argstuple);
    return qp_capi_vgetargskeywords_impl(args, nargs, kwargs, NULL,
                                 format, kwlist, p_va, flags);
}

static int
qp_capi_scan_keywords(const char * const *keywords, int *ptotal, int *pposonly)
{
    /* scan keywords and count the number of positional-only parameters */
    int i;
    for (i = 0; keywords[i] && !*keywords[i]; i++) {
    }
    *pposonly = i;

    /* scan keywords and get greatest possible nbr of args */
    for (; keywords[i]; i++) {
        if (!*keywords[i]) {
            PyErr_SetString(PyExc_SystemError,
                            "Empty keyword parameter name");
            return -1;
        }
    }
    *ptotal = i;
    return 0;
}

static int
qp_capi_parse_format(const char *format, int total, int npos,
             const char **pfname, const char **pcustommsg,
             int *pmin, int *pmax)
{
    /* grab the function name or custom error msg first (mutually exclusive) */
    const char *custommsg;
    const char *fname = strchr(format, ':');
    if (fname) {
        fname++;
        custommsg = NULL;
    }
    else {
        custommsg = strchr(format,';');
        if (custommsg) {
            custommsg++;
        }
    }

    int min = INT_MAX;
    int max = INT_MAX;
    for (int i = 0; i < total; i++) {
        if (*format == '|') {
            if (min != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string (| specified twice)");
                return -1;
            }
            if (max != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string ($ before |)");
                return -1;
            }
            min = i;
            format++;
        }
        if (*format == '$') {
            if (max != INT_MAX) {
                PyErr_SetString(PyExc_SystemError,
                                "Invalid format string ($ specified twice)");
                return -1;
            }
            if (i < npos) {
                PyErr_SetString(PyExc_SystemError,
                                "Empty parameter name after $");
                return -1;
            }
            max = i;
            format++;
        }
        if (QP_CAPI_IS_END_OF_FORMAT(*format)) {
            PyErr_Format(PyExc_SystemError,
                        "More keyword list entries (%d) than "
                        "format specifiers (%d)", total, i);
            return -1;
        }

        const char *msg = qp_capi_skipitem(&format, NULL, 0);
        if (msg) {
            PyErr_Format(PyExc_SystemError, "%s: '%s'", msg,
                        format);
            return -1;
        }
    }
    min = Py_MIN(min, total);
    max = Py_MIN(max, total);

    if (!QP_CAPI_IS_END_OF_FORMAT(*format) && (*format != '|') && (*format != '$')) {
        PyErr_Format(PyExc_SystemError,
            "more argument specifiers than keyword list entries "
            "(remaining format:'%s')", format);
        return -1;
    }

    *pfname = fname;
    *pcustommsg = custommsg;
    *pmin = min;
    *pmax = max;
    return 0;
}

static PyObject *
qp_capi_new_kwtuple(const char * const *keywords, int total, int pos)
{
    int nkw = total - pos;
    PyObject *kwtuple = PyTuple_New(nkw);
    if (kwtuple == NULL) {
        return NULL;
    }
    keywords += pos;
    for (int i = 0; i < nkw; i++) {
        /* qp: _PyUnicode_InternImmortal; interned names are immortal
           here. */
        PyObject *str = PyUnicode_InternFromString(keywords[i]);
        if (str == NULL) {
            Py_DECREF(kwtuple);
            return NULL;
        }
        PyTuple_SET_ITEM(kwtuple, i, str);
    }
    return kwtuple;
}

static int
qp_capi_parser_init_once(void *arg)
{
    struct _PyArg_Parser *parser = (struct _PyArg_Parser *)arg;
    const char * const *keywords = parser->keywords;
    assert(keywords != NULL);
    assert(parser->pos == 0 &&
           (parser->format == NULL || parser->fname == NULL) &&
           parser->custom_msg == NULL &&
           parser->min == 0 &&
           parser->max == 0);

    int len, pos;
    if (qp_capi_scan_keywords(keywords, &len, &pos) < 0) {
        return -1;
    }

    const char *fname, *custommsg = NULL;
    int min = 0, max = 0;
    if (parser->format) {
        assert(parser->fname == NULL);
        if (qp_capi_parse_format(parser->format, len, pos,
                         &fname, &custommsg, &min, &max) < 0) {
            return -1;
        }
    }
    else {
        assert(parser->fname != NULL);
        fname = parser->fname;
    }

    int owned;
    PyObject *kwtuple = parser->kwtuple;
    if (kwtuple == NULL) {
        /* qp: one C-API interpreter per process, which is the main one, so
           CPython's switch to the main interpreter never happens. */
        kwtuple = qp_capi_new_kwtuple(keywords, len, pos);
        if (kwtuple == NULL) {
            return -1;
        }
        owned = 1;
    }
    else {
        owned = 0;
    }

    parser->pos = pos;
    parser->fname = fname;
    parser->custom_msg = custommsg;
    parser->min = min;
    parser->max = max;
    parser->kwtuple = kwtuple;
    parser->is_kwtuple_owned = owned;

    assert(parser->next == NULL);
    parser->next = qp_capi_static_arg_parsers;
    qp_capi_static_arg_parsers = parser;
    return 0;
}

static int
qp_capi_parser_init(struct _PyArg_Parser *parser)
{
    /* qp: _PyOnceFlag_CallOnce; the C API is single-threaded, so the flag
       is tested directly and set once initialization succeeded (a failed
       initialization is retried by the next call, as CPython). */
    if (parser->once.v) {
        return 0;
    }
    if (qp_capi_parser_init_once(parser) < 0) {
        return -1;
    }
    parser->once.v = 1;
    return 0;
}

static void
qp_capi_parser_clear(struct _PyArg_Parser *parser)
{
    if (parser->is_kwtuple_owned) {
        Py_CLEAR(parser->kwtuple);
    }

    if (parser->format) {
        parser->fname = NULL;
    }
    else {
        assert(parser->fname != NULL);
    }
    parser->custom_msg = NULL;
    parser->pos = 0;
    parser->min = 0;
    parser->max = 0;
    parser->is_kwtuple_owned = 0;
    parser->once.v = 0;
}

static PyObject*
qp_capi_find_keyword(PyObject *kwnames, PyObject *const *kwstack, PyObject *key)
{
    Py_ssize_t i, nkwargs;

    nkwargs = PyTuple_GET_SIZE(kwnames);
    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* kwname == key will normally find a match in since keyword keys
           should be interned strings; if not retry below in a new loop. */
        if (kwname == key) {
            return Py_NewRef(kwstack[i]);
        }
    }

    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (PyUnicode_Equal(kwname, key) == 1) {
            return Py_NewRef(kwstack[i]);
        }
    }
    return NULL;
}

static int
qp_capi_vgetargskeywordsfast_impl(PyObject *const *args, Py_ssize_t nargs,
                          PyObject *kwargs, PyObject *kwnames,
                          struct _PyArg_Parser *parser,
                          va_list *p_va, int flags)
{
    PyObject *kwtuple;
    char msgbuf[512];
    int levels[32];
    const char *format;
    const char *msg;
    PyObject *keyword;
    Py_ssize_t i;
    int pos, len;
    Py_ssize_t nkwargs;
    qp_capi_freelistentry_t static_entries[QP_CAPI_STATIC_FREELIST_ENTRIES];
    qp_capi_freelist_t freelist;
    PyObject *const *kwstack = NULL;

    freelist.entries = static_entries;
    freelist.first_available = 0;
    freelist.entries_malloced = 0;

    assert(kwargs == NULL || PyDict_Check(kwargs));
    assert(kwargs == NULL || kwnames == NULL);
    assert(p_va != NULL);

    if (parser == NULL) {
        PyErr_BadInternalCall();
        return 0;
    }

    if (kwnames != NULL && !PyTuple_Check(kwnames)) {
        PyErr_BadInternalCall();
        return 0;
    }

    if (qp_capi_parser_init(parser) < 0) {
        return 0;
    }

    kwtuple = parser->kwtuple;
    pos = parser->pos;
    len = pos + (int)PyTuple_GET_SIZE(kwtuple);

    if (len > QP_CAPI_STATIC_FREELIST_ENTRIES) {
        freelist.entries = PyMem_NEW(qp_capi_freelistentry_t, len);
        if (freelist.entries == NULL) {
            PyErr_NoMemory();
            return 0;
        }
        freelist.entries_malloced = 1;
    }

    if (kwargs != NULL) {
        nkwargs = PyDict_GET_SIZE(kwargs);
    }
    else if (kwnames != NULL) {
        nkwargs = PyTuple_GET_SIZE(kwnames);
        kwstack = args + nargs;
    }
    else {
        nkwargs = 0;
    }
    if (nargs + nkwargs > len) {
        /* Adding "keyword" (when nargs == 0) prevents producing wrong error
           messages in some special cases (see bpo-31229). */
        PyErr_Format(PyExc_TypeError,
                     "%.200s%s takes at most %d %sargument%s (%zd given)",
                     (parser->fname == NULL) ? "function" : parser->fname,
                     (parser->fname == NULL) ? "" : "()",
                     len,
                     (nargs == 0) ? "keyword " : "",
                     (len == 1) ? "" : "s",
                     nargs + nkwargs);
        return qp_capi_cleanreturn(0, &freelist);
    }
    if (parser->max < nargs) {
        if (parser->max == 0) {
            PyErr_Format(PyExc_TypeError,
                         "%.200s%s takes no positional arguments",
                         (parser->fname == NULL) ? "function" : parser->fname,
                         (parser->fname == NULL) ? "" : "()");
        }
        else {
            PyErr_Format(PyExc_TypeError,
                         "%.200s%s takes %s %d positional argument%s (%zd given)",
                         (parser->fname == NULL) ? "function" : parser->fname,
                         (parser->fname == NULL) ? "" : "()",
                         (parser->min < parser->max) ? "at most" : "exactly",
                         parser->max,
                         parser->max == 1 ? "" : "s",
                         nargs);
        }
        return qp_capi_cleanreturn(0, &freelist);
    }

    format = parser->format;
    assert(format != NULL || len == 0);
    /* convert tuple args and keyword args in same loop, using kwtuple to drive process */
    for (i = 0; i < len; i++) {
        if (*format == '|') {
            format++;
        }
        if (*format == '$') {
            format++;
        }
        assert(!QP_CAPI_IS_END_OF_FORMAT(*format));

        PyObject *current_arg;
        if (i < nargs) {
            current_arg = Py_NewRef(args[i]);
        }
        else if (nkwargs && i >= pos) {
            keyword = PyTuple_GET_ITEM(kwtuple, i - pos);
            if (kwargs != NULL) {
                if (PyDict_GetItemRef(kwargs, keyword, &current_arg) < 0) {
                    return qp_capi_cleanreturn(0, &freelist);
                }
            }
            else {
                current_arg = qp_capi_find_keyword(kwnames, kwstack, keyword);
            }
            if (current_arg) {
                --nkwargs;
            }
        }
        else {
            current_arg = NULL;
        }

        if (current_arg) {
            msg = qp_capi_convertitem(current_arg, &format, p_va, flags,
                levels, msgbuf, sizeof(msgbuf), &freelist);
            Py_DECREF(current_arg);
            if (msg) {
                qp_capi_seterror(i+1, msg, levels, parser->fname, parser->custom_msg);
                return qp_capi_cleanreturn(0, &freelist);
            }
            continue;
        }

        if (i < parser->min) {
            /* Less arguments than required */
            if (i < pos) {
                int min = Py_MIN(pos, parser->min);
                PyErr_Format(PyExc_TypeError,
                             "%.200s%s takes %s %d positional argument%s"
                             " (%zd given)",
                             (parser->fname == NULL) ? "function" : parser->fname,
                             (parser->fname == NULL) ? "" : "()",
                             min < parser->max ? "at least" : "exactly",
                             min,
                             min == 1 ? "" : "s",
                             nargs);
            }
            else {
                keyword = PyTuple_GET_ITEM(kwtuple, i - pos);
                PyErr_Format(PyExc_TypeError,  "%.200s%s missing required "
                             "argument '%U' (pos %zd)",
                             (parser->fname == NULL) ? "function" : parser->fname,
                             (parser->fname == NULL) ? "" : "()",
                             keyword, i+1);
            }
            return qp_capi_cleanreturn(0, &freelist);
        }
        /* current code reports success when all required args
         * fulfilled and no keyword args left, with no further
         * validation. XXX Maybe skip this in debug build ?
         */
        if (!nkwargs) {
            return qp_capi_cleanreturn(1, &freelist);
        }

        /* We are into optional args, skip through to any remaining
         * keyword args */
        msg = qp_capi_skipitem(&format, p_va, flags);
        assert(msg == NULL);
    }

    assert(QP_CAPI_IS_END_OF_FORMAT(*format) || (*format == '|') || (*format == '$'));

    if (nkwargs > 0) {
        /* make sure there are no arguments given by name and position */
        for (i = pos; i < nargs; i++) {
            PyObject *current_arg;
            keyword = PyTuple_GET_ITEM(kwtuple, i - pos);
            if (kwargs != NULL) {
                if (PyDict_GetItemRef(kwargs, keyword, &current_arg) < 0) {
                    return qp_capi_cleanreturn(0, &freelist);
                }
            }
            else {
                current_arg = qp_capi_find_keyword(kwnames, kwstack, keyword);
            }
            if (current_arg) {
                Py_DECREF(current_arg);
                /* arg present in tuple and in dict */
                PyErr_Format(PyExc_TypeError,
                             "argument for %.200s%s given by name ('%U') "
                             "and position (%zd)",
                             (parser->fname == NULL) ? "function" : parser->fname,
                             (parser->fname == NULL) ? "" : "()",
                             keyword, i+1);
                return qp_capi_cleanreturn(0, &freelist);
            }
        }

        qp_capi_error_unexpected_keyword_arg(kwargs, kwnames, kwtuple, parser->fname);
        return qp_capi_cleanreturn(0, &freelist);
    }

    return qp_capi_cleanreturn(1, &freelist);
}

static int
qp_capi_vgetargskeywordsfast(PyObject *args, PyObject *keywords,
                     struct _PyArg_Parser *parser, va_list *p_va, int flags)
{
    PyObject **stack;
    Py_ssize_t nargs;

    if (args == NULL
        || !PyTuple_Check(args)
        || (keywords != NULL && !PyDict_Check(keywords)))
    {
        PyErr_BadInternalCall();
        return 0;
    }

    stack = &PyTuple_GET_ITEM(args, 0);
    nargs = PyTuple_GET_SIZE(args);
    return qp_capi_vgetargskeywordsfast_impl(stack, nargs, keywords, NULL,
                                     parser, p_va, flags);
}


PyObject * const *
_PyArg_UnpackKeywords(PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwargs, PyObject *kwnames,
                      struct _PyArg_Parser *parser,
                      int minpos, int maxpos, int minkw, int varpos,
                      PyObject **buf)
{
    PyObject *kwtuple;
    PyObject *keyword;
    int i, posonly, minposonly, maxargs;
    int reqlimit = minkw ? maxpos + minkw : minpos;
    Py_ssize_t nkwargs;
    PyObject * const *kwstack = NULL;

    assert(kwargs == NULL || PyDict_Check(kwargs));
    assert(kwargs == NULL || kwnames == NULL);

    if (parser == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }

    if (kwnames != NULL && !PyTuple_Check(kwnames)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    if (args == NULL && nargs == 0) {
        args = buf;
    }

    if (qp_capi_parser_init(parser) < 0) {
        return NULL;
    }

    kwtuple = parser->kwtuple;
    posonly = parser->pos;
    minposonly = Py_MIN(posonly, minpos);
    maxargs = posonly + (int)PyTuple_GET_SIZE(kwtuple);

    if (kwargs != NULL) {
        nkwargs = PyDict_GET_SIZE(kwargs);
    }
    else if (kwnames != NULL) {
        nkwargs = PyTuple_GET_SIZE(kwnames);
        kwstack = args + nargs;
    }
    else {
        nkwargs = 0;
    }
    if (nkwargs == 0 && minkw == 0 && minpos <= nargs && (varpos || nargs <= maxpos)) {
        /* Fast path. */
        return args;
    }
    if (!varpos && nargs + nkwargs > maxargs) {
        /* Adding "keyword" (when nargs == 0) prevents producing wrong error
           messages in some special cases (see bpo-31229). */
        PyErr_Format(PyExc_TypeError,
                     "%.200s%s takes at most %d %sargument%s (%zd given)",
                     (parser->fname == NULL) ? "function" : parser->fname,
                     (parser->fname == NULL) ? "" : "()",
                     maxargs,
                     (nargs == 0) ? "keyword " : "",
                     (maxargs == 1) ? "" : "s",
                     nargs + nkwargs);
        return NULL;
    }
    if (!varpos && nargs > maxpos) {
        if (maxpos == 0) {
            PyErr_Format(PyExc_TypeError,
                         "%.200s%s takes no positional arguments",
                         (parser->fname == NULL) ? "function" : parser->fname,
                         (parser->fname == NULL) ? "" : "()");
        }
        else {
            PyErr_Format(PyExc_TypeError,
                         "%.200s%s takes %s %d positional argument%s (%zd given)",
                         (parser->fname == NULL) ? "function" : parser->fname,
                         (parser->fname == NULL) ? "" : "()",
                         (minpos < maxpos) ? "at most" : "exactly",
                         maxpos,
                         (maxpos == 1) ? "" : "s",
                         nargs);
        }
        return NULL;
    }
    if (nargs < minposonly) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s%s takes %s %d positional argument%s"
                     " (%zd given)",
                     (parser->fname == NULL) ? "function" : parser->fname,
                     (parser->fname == NULL) ? "" : "()",
                     (varpos || minposonly < maxpos) ? "at least" : "exactly",
                     minposonly,
                     minposonly == 1 ? "" : "s",
                     nargs);
        return NULL;
    }

    if (varpos) {
        nargs = Py_MIN(maxpos, nargs);
    }
    /* copy tuple args */
    for (i = 0; i < nargs; i++) {
        buf[i] = args[i];
    }

    /* copy keyword args using kwtuple to drive process */
    for (i = Py_MAX((int)nargs, posonly); i < maxargs; i++) {
        PyObject *current_arg;
        if (nkwargs) {
            keyword = PyTuple_GET_ITEM(kwtuple, i - posonly);
            if (kwargs != NULL) {
                if (PyDict_GetItemRef(kwargs, keyword, &current_arg) < 0) {
                    return NULL;
                }
            }
            else {
                current_arg = qp_capi_find_keyword(kwnames, kwstack, keyword);
            }
        }
        else if (i >= reqlimit) {
            break;
        }
        else {
            current_arg = NULL;
        }

        buf[i] = current_arg;

        if (current_arg) {
            Py_DECREF(current_arg);
            --nkwargs;
        }
        else if (i < minpos || (maxpos <= i && i < reqlimit)) {
            /* Less arguments than required */
            keyword = PyTuple_GET_ITEM(kwtuple, i - posonly);
            PyErr_Format(PyExc_TypeError,  "%.200s%s missing required "
                         "argument '%U' (pos %d)",
                         (parser->fname == NULL) ? "function" : parser->fname,
                         (parser->fname == NULL) ? "" : "()",
                         keyword, i+1);
            return NULL;
        }
    }

    if (nkwargs > 0) {
        /* make sure there are no arguments given by name and position */
        for (i = posonly; i < nargs; i++) {
            PyObject *current_arg;
            keyword = PyTuple_GET_ITEM(kwtuple, i - posonly);
            if (kwargs != NULL) {
                if (PyDict_GetItemRef(kwargs, keyword, &current_arg) < 0) {
                    return NULL;
                }
            }
            else {
                current_arg = qp_capi_find_keyword(kwnames, kwstack, keyword);
            }
            if (current_arg) {
                Py_DECREF(current_arg);
                /* arg present in tuple and in dict */
                PyErr_Format(PyExc_TypeError,
                             "argument for %.200s%s given by name ('%U') "
                             "and position (%d)",
                             (parser->fname == NULL) ? "function" : parser->fname,
                             (parser->fname == NULL) ? "" : "()",
                             keyword, i+1);
                return NULL;
            }
        }

        qp_capi_error_unexpected_keyword_arg(kwargs, kwnames, kwtuple, parser->fname);
        return NULL;
    }

    return buf;
}

static const char *
qp_capi_skipitem(const char **p_format, va_list *p_va, int flags)
{
    const char *format = *p_format;
    char c = *format++;

    switch (c) {

    /*
     * codes that take a single data pointer as an argument
     * (the type of the pointer is irrelevant)
     */

    case 'b': /* byte -- very short int */
    case 'B': /* byte as bitfield */
    case 'h': /* short int */
    case 'H': /* short int as bitfield */
    case 'i': /* int */
    case 'I': /* int sized bitfield */
    case 'l': /* long int */
    case 'k': /* long int sized bitfield */
    case 'L': /* long long */
    case 'K': /* long long sized bitfield */
    case 'n': /* Py_ssize_t */
    case 'f': /* float */
    case 'd': /* double */
    case 'D': /* complex double */
    case 'c': /* char */
    case 'C': /* unicode char */
    case 'p': /* boolean predicate */
    case 'S': /* string object */
    case 'Y': /* string object */
    case 'U': /* unicode string object */
        {
            if (p_va != NULL) {
                (void) va_arg(*p_va, void *);
            }
            break;
        }

    /* string codes */

    case 'e': /* string with encoding */
        {
            if (p_va != NULL) {
                (void) va_arg(*p_va, const char *);
            }
            if (!(*format == 's' || *format == 't'))
                /* after 'e', only 's' and 't' is allowed */
                goto err;
            format++;
        }
        _Py_FALLTHROUGH;

    case 's': /* string */
    case 'z': /* string or None */
    case 'y': /* bytes */
    case 'w': /* buffer, read-write */
        {
            if (p_va != NULL) {
                (void) va_arg(*p_va, char **);
            }
            if (c == 'w' && *format != '*')
            {
                /* after 'w', only '*' is allowed */
                goto err;
            }
            if (*format == '#') {
                if (p_va != NULL) {
                    (void) va_arg(*p_va, Py_ssize_t *);
                }
                format++;
            } else if ((c == 's' || c == 'z' || c == 'y' || c == 'w')
                       && *format == '*')
            {
                format++;
            }
            break;
        }

    case 'O': /* object */
        {
            if (*format == '!') {
                format++;
                if (p_va != NULL) {
                    (void) va_arg(*p_va, PyTypeObject*);
                    (void) va_arg(*p_va, PyObject **);
                }
            }
            else if (*format == '&') {
                typedef int (*converter)(PyObject *, void *);
                if (p_va != NULL) {
                    (void) va_arg(*p_va, converter);
                    (void) va_arg(*p_va, void *);
                }
                format++;
            }
            else {
                if (p_va != NULL) {
                    (void) va_arg(*p_va, PyObject **);
                }
            }
            break;
        }

    case '(':           /* bypass tuple, not handled at all previously */
        {
            const char *msg;
            for (;;) {
                if (*format==')')
                    break;
                if (QP_CAPI_IS_END_OF_FORMAT(*format))
                    return "Unmatched left paren in format "
                           "string";
                msg = qp_capi_skipitem(&format, p_va, flags);
                if (msg)
                    return msg;
            }
            format++;
            break;
        }

    case ')':
        return "Unmatched right paren in format string";

    default:
err:
        return "impossible<bad format char>";

    }

    *p_format = format;
    return NULL;
}


int
_PyArg_CheckPositional(const char *name, Py_ssize_t nargs,
                       Py_ssize_t min, Py_ssize_t max)
{
    assert(min >= 0);
    assert(min <= max);

    if (nargs < min) {
        if (name != NULL)
            PyErr_Format(
                PyExc_TypeError,
                "%.200s expected %s%zd argument%s, got %zd",
                name, (min == max ? "" : "at least "), min, min == 1 ? "" : "s", nargs);
        else
            PyErr_Format(
                PyExc_TypeError,
                "unpacked tuple should have %s%zd element%s,"
                " but has %zd",
                (min == max ? "" : "at least "), min, min == 1 ? "" : "s", nargs);
        return 0;
    }

    if (nargs == 0) {
        return 1;
    }

    if (nargs > max) {
        if (name != NULL)
            PyErr_Format(
                PyExc_TypeError,
                "%.200s expected %s%zd argument%s, got %zd",
                name, (min == max ? "" : "at most "), max, max == 1 ? "" : "s", nargs);
        else
            PyErr_Format(
                PyExc_TypeError,
                "unpacked tuple should have %s%zd element%s,"
                " but has %zd",
                (min == max ? "" : "at most "), max, max == 1 ? "" : "s", nargs);
        return 0;
    }

    return 1;
}

static int
qp_capi_unpack_stack(PyObject *const *args, Py_ssize_t nargs, const char *name,
             Py_ssize_t min, Py_ssize_t max, va_list vargs)
{
    Py_ssize_t i;
    PyObject **o;

    if (!_PyArg_CheckPositional(name, nargs, min, max)) {
        return 0;
    }

    for (i = 0; i < nargs; i++) {
        o = va_arg(vargs, PyObject **);
        *o = args[i];
    }
    return 1;
}

int
PyArg_UnpackTuple(PyObject *args, const char *name, Py_ssize_t min, Py_ssize_t max, ...)
{
    PyObject **stack;
    Py_ssize_t nargs;
    int retval;
    va_list vargs;

    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_SystemError,
            "PyArg_UnpackTuple() argument list is not a tuple");
        return 0;
    }
    stack = &PyTuple_GET_ITEM(args, 0);
    nargs = PyTuple_GET_SIZE(args);

    va_start(vargs, max);
    retval = qp_capi_unpack_stack(stack, nargs, name, min, max, vargs);
    va_end(vargs);
    return retval;
}

int
_PyArg_UnpackStack(PyObject *const *args, Py_ssize_t nargs, const char *name,
                   Py_ssize_t min, Py_ssize_t max, ...)
{
    int retval;
    va_list vargs;

    va_start(vargs, max);
    retval = qp_capi_unpack_stack(args, nargs, name, min, max, vargs);
    va_end(vargs);
    return retval;
}


/* For type constructors that don't take keyword args
 *
 * Sets a TypeError and returns 0 if the args/kwargs is
 * not empty, returns 1 otherwise
 */
int
_PyArg_NoKeywords(const char *funcname, PyObject *kwargs)
{
    if (kwargs == NULL) {
        return 1;
    }
    if (!PyDict_CheckExact(kwargs)) {
        PyErr_BadInternalCall();
        return 0;
    }
    if (PyDict_GET_SIZE(kwargs) == 0) {
        return 1;
    }

    PyErr_Format(PyExc_TypeError, "%.200s() takes no keyword arguments",
                    funcname);
    return 0;
}

int
_PyArg_NoPositional(const char *funcname, PyObject *args)
{
    if (args == NULL)
        return 1;
    if (!PyTuple_CheckExact(args)) {
        PyErr_BadInternalCall();
        return 0;
    }
    if (PyTuple_GET_SIZE(args) == 0)
        return 1;

    PyErr_Format(PyExc_TypeError, "%.200s() takes no positional arguments",
                    funcname);
    return 0;
}

int
_PyArg_NoKwnames(const char *funcname, PyObject *kwnames)
{
    if (kwnames == NULL) {
        return 1;
    }

    assert(PyTuple_CheckExact(kwnames));

    if (PyTuple_GET_SIZE(kwnames) == 0) {
        return 1;
    }

    PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", funcname);
    return 0;
}

void
_PyArg_Fini(void)
{
    struct _PyArg_Parser *tmp, *s = qp_capi_static_arg_parsers;
    while (s) {
        tmp = s->next;
        s->next = NULL;
        qp_capi_parser_clear(s);
        s = tmp;
    }
    qp_capi_static_arg_parsers = NULL;
}


/* ======== Value building: port of Python/modsupport.c ======== */

typedef double qp_capi_va_double;

static PyObject *qp_capi_va_build_value(const char *, va_list);

/* Helper for mkvalue() to scan the length of a format */

static Py_ssize_t
qp_capi_countformat(const char *format, char endchar)
{
    Py_ssize_t count = 0;
    int level = 0;
    while (level > 0 || *format != endchar) {
        switch (*format) {
        case '\0':
            /* Premature end */
            PyErr_SetString(PyExc_SystemError,
                            "unmatched paren in format");
            return -1;
        case '(':
        case '[':
        case '{':
            if (level == 0) {
                count++;
            }
            level++;
            break;
        case ')':
        case ']':
        case '}':
            level--;
            break;
        case '#':
        case '&':
        case ',':
        case ':':
        case ' ':
        case '\t':
            break;
        default:
            if (level == 0) {
                count++;
            }
        }
        format++;
    }
    return count;
}


/* Generic function to create a value -- the inverse of getargs() */
/* After an original idea and first implementation by Steven Miale */

static PyObject *qp_capi_do_mktuple(const char**, va_list *, char, Py_ssize_t);
static int qp_capi_do_mkstack(PyObject **, const char**, va_list *, char, Py_ssize_t);
static PyObject *qp_capi_do_mklist(const char**, va_list *, char, Py_ssize_t);
static PyObject *qp_capi_do_mkdict(const char**, va_list *, char, Py_ssize_t);
static PyObject *qp_capi_do_mkvalue(const char**, va_list *);

static int
qp_capi_check_end(const char **p_format, char endchar)
{
    const char *f = *p_format;
    while (*f != endchar) {
        if (*f != ' ' && *f != '\t' && *f != ',' && *f != ':') {
            PyErr_SetString(PyExc_SystemError,
                            "Unmatched paren in format");
            return 0;
        }
        f++;
    }
    if (endchar) {
        f++;
    }
    *p_format = f;
    return 1;
}

static void
qp_capi_do_ignore(const char **p_format, va_list *p_va, char endchar, Py_ssize_t n)
{
    assert(PyErr_Occurred());
    PyObject *v = PyTuple_New(n);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *exc = PyErr_GetRaisedException();
        PyObject *w = qp_capi_do_mkvalue(p_format, p_va);
        PyErr_SetRaisedException(exc);
        if (w != NULL) {
            if (v != NULL) {
                PyTuple_SET_ITEM(v, i, w);
            }
            else {
                Py_DECREF(w);
            }
        }
    }
    Py_XDECREF(v);
    if (!qp_capi_check_end(p_format, endchar)) {
        return;
    }
}

static PyObject *
qp_capi_do_mkdict(const char **p_format, va_list *p_va, char endchar, Py_ssize_t n)
{
    PyObject *d;
    Py_ssize_t i;
    if (n < 0)
        return NULL;
    if (n % 2) {
        PyErr_SetString(PyExc_SystemError,
                        "Bad dict format");
        qp_capi_do_ignore(p_format, p_va, endchar, n);
        return NULL;
    }
    /* Note that we can't bail immediately on error as this will leak
       refcounts on any 'N' arguments. */
    if ((d = PyDict_New()) == NULL) {
        qp_capi_do_ignore(p_format, p_va, endchar, n);
        return NULL;
    }
    for (i = 0; i < n; i+= 2) {
        PyObject *k, *v;

        k = qp_capi_do_mkvalue(p_format, p_va);
        if (k == NULL) {
            qp_capi_do_ignore(p_format, p_va, endchar, n - i - 1);
            Py_DECREF(d);
            return NULL;
        }
        v = qp_capi_do_mkvalue(p_format, p_va);
        if (v == NULL || PyDict_SetItem(d, k, v) < 0) {
            qp_capi_do_ignore(p_format, p_va, endchar, n - i - 2);
            Py_DECREF(k);
            Py_XDECREF(v);
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(k);
        Py_DECREF(v);
    }
    if (!qp_capi_check_end(p_format, endchar)) {
        Py_DECREF(d);
        return NULL;
    }
    return d;
}

static PyObject *
qp_capi_do_mklist(const char **p_format, va_list *p_va, char endchar, Py_ssize_t n)
{
    PyObject *v;
    Py_ssize_t i;
    if (n < 0)
        return NULL;
    /* Note that we can't bail immediately on error as this will leak
       refcounts on any 'N' arguments. */
    v = PyList_New(n);
    if (v == NULL) {
        qp_capi_do_ignore(p_format, p_va, endchar, n);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        PyObject *w = qp_capi_do_mkvalue(p_format, p_va);
        if (w == NULL) {
            qp_capi_do_ignore(p_format, p_va, endchar, n - i - 1);
            Py_DECREF(v);
            return NULL;
        }
        PyList_SET_ITEM(v, i, w);
    }
    if (!qp_capi_check_end(p_format, endchar)) {
        Py_DECREF(v);
        return NULL;
    }
    return v;
}

static int
qp_capi_do_mkstack(PyObject **stack, const char **p_format, va_list *p_va,
           char endchar, Py_ssize_t n)
{
    Py_ssize_t i;

    if (n < 0) {
        return -1;
    }
    /* Note that we can't bail immediately on error as this will leak
       refcounts on any 'N' arguments. */
    for (i = 0; i < n; i++) {
        PyObject *w = qp_capi_do_mkvalue(p_format, p_va);
        if (w == NULL) {
            qp_capi_do_ignore(p_format, p_va, endchar, n - i - 1);
            goto error;
        }
        stack[i] = w;
    }
    if (!qp_capi_check_end(p_format, endchar)) {
        goto error;
    }
    return 0;

error:
    n = i;
    for (i=0; i < n; i++) {
        Py_DECREF(stack[i]);
    }
    return -1;
}

static PyObject *
qp_capi_do_mktuple(const char **p_format, va_list *p_va, char endchar, Py_ssize_t n)
{
    PyObject *v;
    Py_ssize_t i;
    if (n < 0)
        return NULL;
    /* Note that we can't bail immediately on error as this will leak
       refcounts on any 'N' arguments. */
    if ((v = PyTuple_New(n)) == NULL) {
        qp_capi_do_ignore(p_format, p_va, endchar, n);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        PyObject *w = qp_capi_do_mkvalue(p_format, p_va);
        if (w == NULL) {
            qp_capi_do_ignore(p_format, p_va, endchar, n - i - 1);
            Py_DECREF(v);
            return NULL;
        }
        PyTuple_SET_ITEM(v, i, w);
    }
    if (!qp_capi_check_end(p_format, endchar)) {
        Py_DECREF(v);
        return NULL;
    }
    return v;
}

static PyObject *
qp_capi_do_mkvalue(const char **p_format, va_list *p_va)
{
    for (;;) {
        switch (*(*p_format)++) {
        case '(':
            return qp_capi_do_mktuple(p_format, p_va, ')',
                              qp_capi_countformat(*p_format, ')'));

        case '[':
            return qp_capi_do_mklist(p_format, p_va, ']',
                             qp_capi_countformat(*p_format, ']'));

        case '{':
            return qp_capi_do_mkdict(p_format, p_va, '}',
                             qp_capi_countformat(*p_format, '}'));

        case 'b':
        case 'B':
        case 'h':
        case 'i':
            return PyLong_FromLong((long)va_arg(*p_va, int));

        case 'H':
            return PyLong_FromLong((long)va_arg(*p_va, unsigned int));

        case 'I':
        {
            unsigned int n;
            n = va_arg(*p_va, unsigned int);
            return PyLong_FromUnsignedLong(n);
        }

        case 'n':
#if SIZEOF_SIZE_T!=SIZEOF_LONG
            return PyLong_FromSsize_t(va_arg(*p_va, Py_ssize_t));
#endif
            /* Fall through from 'n' to 'l' if Py_ssize_t is long */
            _Py_FALLTHROUGH;
        case 'l':
            return PyLong_FromLong(va_arg(*p_va, long));

        case 'k':
        {
            unsigned long n;
            n = va_arg(*p_va, unsigned long);
            return PyLong_FromUnsignedLong(n);
        }

        case 'L':
            return PyLong_FromLongLong((long long)va_arg(*p_va, long long));

        case 'K':
            return PyLong_FromUnsignedLongLong(
                va_arg(*p_va, unsigned long long));

        case 'u':
        {
            PyObject *v;
            const wchar_t *u = va_arg(*p_va, wchar_t*);
            Py_ssize_t n;
            if (**p_format == '#') {
                ++*p_format;
                n = va_arg(*p_va, Py_ssize_t);
            }
            else
                n = -1;
            if (u == NULL) {
                v = Py_NewRef(Py_None);
            }
            else {
                if (n < 0)
                    n = wcslen(u);
                v = PyUnicode_FromWideChar(u, n);
            }
            return v;
        }
        case 'f':
        case 'd':
            return PyFloat_FromDouble(
                (double)va_arg(*p_va, qp_capi_va_double));

        case 'D':
            return PyComplex_FromCComplex(
                *((Py_complex *)va_arg(*p_va, Py_complex *)));

        case 'c':
        {
            char p[1];
            p[0] = (char)va_arg(*p_va, int);
            return PyBytes_FromStringAndSize(p, 1);
        }
        case 'C':
        {
            int i = va_arg(*p_va, int);
            return PyUnicode_FromOrdinal(i);
        }
        case 'p':
        {
            int i = va_arg(*p_va, int);
            return PyBool_FromLong(i);
        }

        case 's':
        case 'z':
        case 'U':   /* XXX deprecated alias */
        {
            PyObject *v;
            const char *str = va_arg(*p_va, const char *);
            Py_ssize_t n;
            if (**p_format == '#') {
                ++*p_format;
                n = va_arg(*p_va, Py_ssize_t);
            }
            else
                n = -1;
            if (str == NULL) {
                v = Py_NewRef(Py_None);
            }
            else {
                if (n < 0) {
                    size_t m = strlen(str);
                    if (m > PY_SSIZE_T_MAX) {
                        PyErr_SetString(PyExc_OverflowError,
                            "string too long for Python string");
                        return NULL;
                    }
                    n = (Py_ssize_t)m;
                }
                v = PyUnicode_FromStringAndSize(str, n);
            }
            return v;
        }

        case 'y':
        {
            PyObject *v;
            const char *str = va_arg(*p_va, const char *);
            Py_ssize_t n;
            if (**p_format == '#') {
                ++*p_format;
                n = va_arg(*p_va, Py_ssize_t);
            }
            else
                n = -1;
            if (str == NULL) {
                v = Py_NewRef(Py_None);
            }
            else {
                if (n < 0) {
                    size_t m = strlen(str);
                    if (m > PY_SSIZE_T_MAX) {
                        PyErr_SetString(PyExc_OverflowError,
                            "string too long for Python bytes");
                        return NULL;
                    }
                    n = (Py_ssize_t)m;
                }
                v = PyBytes_FromStringAndSize(str, n);
            }
            return v;
        }

        case 'N':
        case 'S':
        case 'O':
        if (**p_format == '&') {
            typedef PyObject *(*converter)(void *);
            converter func = va_arg(*p_va, converter);
            void *arg = va_arg(*p_va, void *);
            ++*p_format;
            return (*func)(arg);
        }
        else {
            PyObject *v;
            v = va_arg(*p_va, PyObject *);
            if (v != NULL) {
                if (*(*p_format - 1) != 'N')
                    Py_INCREF(v);
            }
            else if (!PyErr_Occurred())
                /* If a NULL was passed
                 * because a call that should
                 * have constructed a value
                 * failed, that's OK, and we
                 * pass the error on; but if
                 * no error occurred it's not
                 * clear that the caller knew
                 * what she was doing. */
                PyErr_SetString(PyExc_SystemError,
                    "NULL object passed to Py_BuildValue");
            return v;
        }

        case ':':
        case ',':
        case ' ':
        case '\t':
            break;

        default:
            PyErr_SetString(PyExc_SystemError,
                "bad format char passed to Py_BuildValue");
            return NULL;

        }
    }
}


PyObject *
Py_BuildValue(const char *format, ...)
{
    va_list va;
    PyObject* retval;
    va_start(va, format);
    retval = qp_capi_va_build_value(format, va);
    va_end(va);
    return retval;
}

PyAPI_FUNC(PyObject *) /* abi only */
_Py_BuildValue_SizeT(const char *format, ...)
{
    va_list va;
    PyObject* retval;
    va_start(va, format);
    retval = qp_capi_va_build_value(format, va);
    va_end(va);
    return retval;
}

PyObject *
Py_VaBuildValue(const char *format, va_list va)
{
    return qp_capi_va_build_value(format, va);
}

PyAPI_FUNC(PyObject *) /* abi only */
_Py_VaBuildValue_SizeT(const char *format, va_list va)
{
    return qp_capi_va_build_value(format, va);
}

static PyObject *
qp_capi_va_build_value(const char *format, va_list va)
{
    const char *f = format;
    Py_ssize_t n = qp_capi_countformat(f, '\0');
    va_list lva;
    PyObject *retval;

    if (n < 0)
        return NULL;
    if (n == 0) {
        Py_RETURN_NONE;
    }
    va_copy(lva, va);
    if (n == 1) {
        retval = qp_capi_do_mkvalue(&f, &lva);
    } else {
        retval = qp_capi_do_mktuple(&f, &lva, '\0', n);
    }
    va_end(lva);
    return retval;
}

PyObject **
_Py_VaBuildStack(PyObject **small_stack, Py_ssize_t small_stack_len,
                const char *format, va_list va, Py_ssize_t *p_nargs)
{
    const char *f;
    Py_ssize_t n;
    va_list lva;
    PyObject **stack;
    int res;

    n = qp_capi_countformat(format, '\0');
    if (n < 0) {
        *p_nargs = 0;
        return NULL;
    }

    if (n == 0) {
        *p_nargs = 0;
        return small_stack;
    }

    if (n <= small_stack_len) {
        stack = small_stack;
    }
    else {
        stack = PyMem_Malloc(n * sizeof(stack[0]));
        if (stack == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
    }

    va_copy(lva, va);
    f = format;
    res = qp_capi_do_mkstack(stack, &f, &lva, '\0', n);
    va_end(lva);

    if (res < 0) {
        if (stack != small_stack) {
            PyMem_Free(stack);
        }
        return NULL;
    }

    *p_nargs = n;
    return stack;
}

#undef QP_CAPI_FLAG_COMPAT
#undef QP_CAPI_STATIC_FREELIST_ENTRIES
#undef QP_CAPI_CONV_UNICODE
#undef QP_CAPI_IS_END_OF_FORMAT

