/*
 * _io: the I/O implementation module behind `io` and builtins.open.
 *
 * As planned for M3, the classes are CPython's pure-Python implementation
 * (pylib/_pyio.py, the reference twin of Modules/_io): the module's exec
 * creates the native parts of Modules/_io/_iomodule.c (DEFAULT_BUFFER_SIZE,
 * UnsupportedOperation, BlockingIOError, open, open_code, text_encoding)
 * and _pyio is executed in a private namespace named "_io" the first time
 * one of its classes is needed (module __getattr__, removed afterwards).
 * The classes are re-exported under CPython's names (`_io._IOBase`,
 * `_io.TextIOWrapper`, ...) with __module__ '_io'.
 *
 * Loading is lazy because importlib._bootstrap_external imports _io while
 * only the builtin importer exists, and _pyio imports os/abc/codecs/stat.
 * Until the classes are loaded, open_code() returns a minimal reader over
 * the file contents (all importlib needs from it); afterwards it returns
 * open(path, 'rb') like CPython.
 *
 * _pyio does `import io` and registers its classes with the io ABCs; io
 * itself imports _io, so _pyio runs with a stand-in `io` module in
 * sys.modules (the registrations are made by io.py itself, as with the C
 * implementation).  Not provided: _io._WindowsConsoleIO (Windows only) and
 * _io._BytesIOBuffer (an internal helper of the C BytesIO).
 *
 * qp_io_install_stdio() is CPython's init_set_builtins_open() +
 * init_sys_streams() (Python/pylifecycle.c, PSF-2.0), called by the
 * importlib boot code.
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <locale.h>
#include <langinfo.h>

/* python/mod_codecs.c */
static JSValue qp_codec_lookup(JSContext *ctx, const char *encoding);

#define QP_IOM_DEFAULT_BUFFER_SIZE (128 * 1024)

/* hidden properties of the _io module object */
#define QP_IOM_STATE_PROP   "\x01qp_io_state"     /* int: 0 not loaded, 1 loading, 2 loaded */
#define QP_IOM_PRELUDE_PROP "\x01qp_io_prelude"   /* prelude namespace dict */
#define QP_IOM_OPEN_PROP    "\x01qp_io_open"      /* _pyio open() function */
#define QP_IOM_NS_PROP      "\x01qp_io_ns"        /* _pyio namespace dict */

enum { QP_IOM_NOT_LOADED, QP_IOM_LOADING, QP_IOM_LOADED };

/* Names of the _pyio classes; accessing one triggers the load. */
static const char *const qp_iom_lazy_names[] = {
    "_IOBase", "_RawIOBase", "_BufferedIOBase", "_TextIOBase", "FileIO", "BytesIO",
    "StringIO", "BufferedReader", "BufferedWriter", "BufferedRWPair", "BufferedRandom",
    "IncrementalNewlineDecoder", "TextIOWrapper", NULL,
};

static const char qp_iom_prelude_src[] =
    "class _BootstrapCodeReader:\n"
    "    \"\"\"io.open_code() result while the _io classes are not loaded yet.\"\"\"\n"
    "\n"
    "    __slots__ = ('name', '_data', '_pos')\n"
    "\n"
    "    def __init__(self, name, data):\n"
    "        self.name = name\n"
    "        self._data = data\n"
    "        self._pos = 0\n"
    "\n"
    "    def read(self, size=-1):\n"
    "        data = self._data\n"
    "        pos = self._pos\n"
    "        if size is None or size < 0:\n"
    "            end = len(data)\n"
    "        else:\n"
    "            end = min(pos + size, len(data))\n"
    "        if end > pos:\n"
    "            self._pos = end\n"
    "        return data[pos:end]\n"
    "\n"
    "    def readall(self):\n"
    "        return self.read()\n"
    "\n"
    "    def seek(self, pos, whence=0):\n"
    "        if whence == 1:\n"
    "            pos += self._pos\n"
    "        elif whence == 2:\n"
    "            pos += len(self._data)\n"
    "        elif whence != 0:\n"
    "            raise ValueError(f\"invalid whence ({whence}, should be 0, 1 or 2)\")\n"
    "        if pos < 0:\n"
    "            raise ValueError(f\"negative seek value {pos}\")\n"
    "        self._pos = pos\n"
    "        return pos\n"
    "\n"
    "    def tell(self):\n"
    "        return self._pos\n"
    "\n"
    "    def close(self):\n"
    "        self._data = b''\n"
    "        self._pos = 0\n"
    "\n"
    "    def __enter__(self):\n"
    "        return self\n"
    "\n"
    "    def __exit__(self, *args):\n"
    "        self.close()\n"
    "\n"
    "\n"
    "_IO_ALL = [\"BlockingIOError\", \"open\", \"open_code\", \"IOBase\", \"RawIOBase\",\n"
    "           \"FileIO\", \"BytesIO\", \"StringIO\", \"BufferedIOBase\",\n"
    "           \"BufferedReader\", \"BufferedWriter\", \"BufferedRWPair\",\n"
    "           \"BufferedRandom\", \"TextIOBase\", \"TextIOWrapper\",\n"
    "           \"UnsupportedOperation\", \"SEEK_SET\", \"SEEK_CUR\", \"SEEK_END\",\n"
    "           \"DEFAULT_BUFFER_SIZE\", \"text_encoding\", \"IncrementalNewlineDecoder\",\n"
    "           \"Reader\", \"Writer\"]\n"
    "\n"
    "_EXPORTS = ((\"IOBase\", \"_IOBase\"), (\"RawIOBase\", \"_RawIOBase\"),\n"
    "            (\"BufferedIOBase\", \"_BufferedIOBase\"), (\"TextIOBase\", \"_TextIOBase\"),\n"
    "            (\"FileIO\", \"FileIO\"), (\"BytesIO\", \"BytesIO\"), (\"StringIO\", \"StringIO\"),\n"
    "            (\"BufferedReader\", \"BufferedReader\"), (\"BufferedWriter\", \"BufferedWriter\"),\n"
    "            (\"BufferedRWPair\", \"BufferedRWPair\"), (\"BufferedRandom\", \"BufferedRandom\"),\n"
    "            (\"IncrementalNewlineDecoder\", \"IncrementalNewlineDecoder\"),\n"
    "            (\"TextIOWrapper\", \"TextIOWrapper\"))\n"
    "\n"
    "\n"
    "def _load(_io, read_file):\n"
    "    import sys\n"
    "    stdlib = getattr(sys, '_stdlib_dir', None)\n"
    "    if stdlib:\n"
    "        dirs = [stdlib]\n"
    "    else:\n"
    "        dirs = [p for p in sys.path if isinstance(p, str)]\n"
    "    source = filename = None\n"
    "    for d in dirs:\n"
    "        if not d or d.endswith('/'):\n"
    "            path = d + '_pyio.py'\n"
    "        else:\n"
    "            path = d + '/_pyio.py'\n"
    "        try:\n"
    "            source = read_file(path)\n"
    "        except OSError:\n"
    "            continue\n"
    "        filename = path\n"
    "        break\n"
    "    if source is None:\n"
    "        raise ImportError(\"cannot load the _io module: _pyio.py not found \"\n"
    "                          \"in the standard library\", name='_io')\n"
    "\n"
    "    # _pyio does \"import io\" and registers its classes with the io ABCs,\n"
    "    # but io itself imports _io: give it a stand-in io module.\n"
    "    class _ABC:\n"
    "        __slots__ = ()\n"
    "\n"
    "        @staticmethod\n"
    "        def register(subclass):\n"
    "            return subclass\n"
    "\n"
    "    shim = type(sys)('io')\n"
    "    shim.__all__ = list(_IO_ALL)\n"
    "    shim.SEEK_SET, shim.SEEK_CUR, shim.SEEK_END = 0, 1, 2\n"
    "    shim.Reader = shim.Writer = None\n"
    "    shim.IOBase = shim.RawIOBase = shim.BufferedIOBase = shim.TextIOBase = _ABC\n"
    "    shim.open_code = _io.open_code\n"
    "    shim.UnsupportedOperation = _io.UnsupportedOperation\n"
    "    modules = sys.modules\n"
    "    missing = object()\n"
    "    saved = modules.get('io', missing)\n"
    "    modules['io'] = shim\n"
    "    try:\n"
    "        ns = {'__name__': '_io', '__file__': filename}\n"
    "        exec(compile(source, filename, 'exec'), ns)\n"
    "    finally:\n"
    "        if saved is missing:\n"
    "            if modules.get('io') is shim:\n"
    "                del modules['io']\n"
    "        else:\n"
    "            modules['io'] = saved\n"
    "\n"
    "    # the C text_encoding() (CPython's warning text and stack level)\n"
    "    ns['text_encoding'] = _io.text_encoding\n"
    "    exports = {}\n"
    "    for src, dst in _EXPORTS:\n"
    "        cls = ns[src]\n"
    "        if src != dst:\n"
    "            cls.__name__ = dst\n"
    "            cls.__qualname__ = dst\n"
    "        exports[dst] = cls\n"
    "    return exports, ns['open'].__func__, ns\n";

/* ========================================================================= */
/* helpers                                                                    */
/* ========================================================================= */

static inline BOOL qp_iom_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

/* _PyArg_BadArgument */
static JSValue qp_iom_bad_argument(JSContext *ctx, const char *fname, const char *displayname,
                                   const char *expected, JSValueConst arg)
{
    JSValue tn;
    QPType *t;
    if (qp_iom_is_none(arg)) {
        tn = qp_str_from_cstr(ctx, "None");
    } else {
        t = qp_type_of(ctx, arg);
        tn = t->tp_name ? qp_str_from_cstr(ctx, t->tp_name) : qp_str_from_atom(ctx, t->name);
    }
    if (JS_IsException(tn))
        return JS_EXCEPTION;
    qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50U", fname, displayname,
                        expected, tn);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* Clinic `str` / `str(accept={str, NoneType})` check (the value itself is
   passed on; only the type and embedded NULs are validated). */
static int qp_iom_check_str_arg(JSContext *ctx, const char *fname, const char *displayname,
                                JSValueConst v, BOOL accept_none)
{
    JSValue s;
    const char *p;
    size_t len;
    if (QP_IS_NULL(v) || (accept_none && qp_iom_is_none(v)))
        return 0;
    if (!qp_str_check(ctx, v)) {
        qp_iom_bad_argument(ctx, fname, displayname, accept_none ? "str or None" : "str", v);
        return -1;
    }
    s = qp_str_value(ctx, v);
    if (JS_IsException(s))
        return -1;
    p = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (!p)
        return -1;
    if (strlen(p) != len) {
        JS_FreeCString(ctx, p);
        qp_raise_value_error(ctx, "embedded null character");
        return -1;
    }
    JS_FreeCString(ctx, p);
    return 0;
}

static JSValue qp_iom_get_prop(JSContext *ctx, JSValueConst module, const char *prop)
{
    return JS_GetPropertyStr(ctx, module, prop);
}

/* Consumes v. */
static int qp_iom_set_prop(JSContext *ctx, JSValueConst module, const char *prop, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, module, prop, v,
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

static int qp_iom_state(JSContext *ctx, JSValueConst module)
{
    JSValue v = qp_iom_get_prop(ctx, module, QP_IOM_STATE_PROP);
    int32_t st = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        st = JS_VALUE_GET_INT(v);
    JS_FreeValue(ctx, v);
    return st;
}

static int qp_iom_set_state(JSContext *ctx, JSValueConst module, int st)
{
    return qp_iom_set_prop(ctx, module, QP_IOM_STATE_PROP, JS_NewInt32(ctx, st));
}

/* Value of name in d (new ref), QP_NULL if absent. */
static JSValue qp_iom_dict_get(JSContext *ctx, JSValueConst d, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue v;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, d, a, &v);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    return r ? v : QP_NULL;
}

/* obj.name = v (consumes v) */
static int qp_iom_setattr(JSContext *ctx, JSValueConst obj, const char *name, JSValue v)
{
    JSAtom a;
    int r;
    if (JS_IsException(v))
        return -1;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_setattr(ctx, obj, a, v);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue qp_iom_getattr(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return r;
}

/* Whole file as bytes (the bootstrap open_code and the _pyio source).
   path: str; OSError with the filename on failure. */
static JSValue qp_iom_read_file(JSContext *ctx, JSValueConst path)
{
    JSValue enc;
    const uint8_t *p;
    Py_ssize_t plen;
    char *cpath;
    int fd, err;
    struct stat st;
    DynBuf d;
    uint8_t chunk[65536];

    /* os.fsencode(): UTF-8 with surrogateescape */
    enc = qp_str_encode(ctx, path, "utf-8", "surrogateescape");
    if (JS_IsException(enc))
        return enc;
    p = qp_bytes_data(enc, &plen);
    if ((Py_ssize_t)strlen((const char *)p) != plen) {
        JS_FreeValue(ctx, enc);
        return qp_raise_value_error(ctx, "embedded null byte");
    }
    cpath = js_malloc(ctx, plen + 1);
    if (!cpath) {
        JS_FreeValue(ctx, enc);
        return JS_EXCEPTION;
    }
    memcpy(cpath, p, plen);
    cpath[plen] = '\0';
    JS_FreeValue(ctx, enc);
    for (;;) {
        fd = open(cpath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0 || errno != EINTR)
            break;
        if (qp_signal_check(ctx) < 0) {
            js_free(ctx, cpath);
            return JS_EXCEPTION;
        }
    }
    js_free(ctx, cpath);
    if (fd < 0)
        return qp_raise_os_error(ctx, errno, path);
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        return qp_raise_os_error(ctx, EISDIR, path);
    }
    js_dbuf_init(ctx, &d);
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                if (qp_signal_check(ctx) < 0) {
                    close(fd);
                    dbuf_free(&d);
                    return JS_EXCEPTION;
                }
                continue;
            }
            err = errno;
            close(fd);
            dbuf_free(&d);
            return qp_raise_os_error(ctx, err, path);
        }
        if (n == 0)
            break;
        if (dbuf_put(&d, chunk, n) < 0) {
            close(fd);
            dbuf_free(&d);
            return qp_raise_memory_error(ctx);
        }
    }
    close(fd);
    {
        JSValue r = qp_bytes_new(ctx, d.buf, d.size);
        dbuf_free(&d);
        return r;
    }
}

/* prelude helper _read_file(path) */
static JSValue qp_iom_m_read_file(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue s, r;
    if (!qp_str_check(ctx, args[0]))
        return qp_iom_bad_argument(ctx, "_read_file", "argument", "str", args[0]);
    s = qp_str_value(ctx, args[0]);
    if (JS_IsException(s))
        return s;
    r = qp_iom_read_file(ctx, s);
    JS_FreeValue(ctx, s);
    return r;
}

static const QPMethodDef qp_iom_read_file_def = {
    "_read_file", qp_iom_m_read_file, QP_METH_O, 0, NULL,
};

/* ========================================================================= */
/* Loading _pyio                                                              */
/* ========================================================================= */

/* Make the _pyio classes available in the module: 0 loaded (now or
   before), 1 a load is in progress (re-entrant access), -1 error. */
static int qp_iom_load(JSContext *ctx, JSValueConst module)
{
    JSValue prelude, load, reader, res = JS_UNDEFINED, args[2], dict;
    JSAtom a;
    int st = qp_iom_state(ctx, module);

    if (st == QP_IOM_LOADED)
        return 0;
    if (st == QP_IOM_LOADING)
        return 1;
    prelude = qp_iom_get_prop(ctx, module, QP_IOM_PRELUDE_PROP);
    if (JS_IsException(prelude))
        return -1;
    load = qp_iom_dict_get(ctx, prelude, "_load");
    JS_FreeValue(ctx, prelude);
    if (JS_IsException(load))
        return -1;
    if (QP_IS_NULL(load)) {
        qp_raise_system_error(ctx, "_io: prelude not initialized");
        return -1;
    }
    reader = qp_cfunction_new(ctx, &qp_iom_read_file_def, QP_NULL, JS_UNDEFINED);
    if (JS_IsException(reader)) {
        JS_FreeValue(ctx, load);
        return -1;
    }
    if (qp_iom_set_state(ctx, module, QP_IOM_LOADING) < 0) {
        JS_FreeValue(ctx, load);
        JS_FreeValue(ctx, reader);
        return -1;
    }
    args[0] = JS_DupValue(ctx, module);
    args[1] = reader;
    res = qp_call2(ctx, load, args[0], args[1]);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, load);
    if (JS_IsException(res))
        goto fail;
    if (!qp_tuple_check(ctx, res) || QP_TUPLE_SIZE(res) != 3) {
        qp_raise_system_error(ctx, "_io: bad loader result");
        goto fail;
    }
    dict = qp_module_get_dict(module);
    if (qp_dict_merge(ctx, dict, QP_TUPLE_ITEMS(res)[0], 1) < 0)
        goto fail;
    if (qp_iom_set_prop(ctx, module, QP_IOM_OPEN_PROP, JS_DupValue(ctx, QP_TUPLE_ITEMS(res)[1])) < 0 ||
        qp_iom_set_prop(ctx, module, QP_IOM_NS_PROP, JS_DupValue(ctx, QP_TUPLE_ITEMS(res)[2])) < 0)
        goto fail;
    JS_FreeValue(ctx, res);
    res = JS_UNDEFINED;
    /* the lazy loader is no longer needed */
    a = JS_NewAtom(ctx, "__getattr__");
    if (a == JS_ATOM_NULL)
        goto fail;
    if (qp_dict_del_item_atom(ctx, dict, a) < 0) {
        JS_FreeAtom(ctx, a);
        goto fail;
    }
    JS_FreeAtom(ctx, a);
    if (qp_iom_set_state(ctx, module, QP_IOM_LOADED) < 0)
        goto fail;
    return 0;
fail:
    JS_FreeValue(ctx, res);
    {
        /* back to "not loaded" so a later access retries */
        JSValue exc = JS_GetException(ctx);
        qp_iom_set_state(ctx, module, QP_IOM_NOT_LOADED);
        JS_Throw(ctx, exc);
    }
    return -1;
}

/* module __getattr__(name): load _pyio for the class names. */
static JSValue qp_iom_m_getattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst name = args[0];
    const char *cname;
    size_t len;
    int i, r;
    JSValue s, v;

    if (!qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "attribute name must be string, not '%T'", name);
    s = qp_str_value(ctx, name);
    if (JS_IsException(s))
        return s;
    cname = qp_str_utf8(ctx, s, &len);
    if (!cname) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; qp_iom_lazy_names[i]; i++) {
        if (!strcmp(cname, qp_iom_lazy_names[i]))
            break;
    }
    JS_FreeCString(ctx, cname);
    if (!qp_iom_lazy_names[i]) {
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "module '_io' has no attribute '%U'", s);
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    r = qp_iom_load(ctx, self);
    if (r < 0) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    if (r > 0) {
        qp_raise(ctx, QP_EXC(ctx, AttributeError),
                 "partially initialized module '_io' has no attribute '%U' "
                 "(most likely due to a circular import)", s);
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    r = qp_dict_get_item(ctx, qp_module_get_dict(self), s, &v);
    if (r == 0)
        qp_raise(ctx, QP_EXC(ctx, AttributeError), "module '_io' has no attribute '%U'", s);
    JS_FreeValue(ctx, s);
    return r > 0 ? v : JS_EXCEPTION;
}

/* The _pyio open() function (new ref), loading the classes if needed. */
static JSValue qp_iom_open_func(JSContext *ctx, JSValueConst module)
{
    int r = qp_iom_load(ctx, module);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0)
        return qp_raise(ctx, QP_EXC(ctx, ImportError),
                        "cannot use _io.open() while the _io module is being initialized");
    return qp_iom_get_prop(ctx, module, QP_IOM_OPEN_PROP);
}

/* ========================================================================= */
/* Module functions                                                           */
/* ========================================================================= */

/* open(file, mode='r', buffering=-1, encoding=None, errors=None,
        newline=None, closefd=True, opener=None) */
static JSValue qp_iom_m_open(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = {
        "file", "mode", "buffering", "encoding", "errors", "newline", "closefd", "opener", NULL,
    };
    QPArgSpec spec = { "open", kw, 1, 8, 0 };
    JSValueConst a[8];
    JSValue call_args[8], func, r;
    int buffering = -1, closefd = 1, i;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_iom_check_str_arg(ctx, "open", "argument 'mode'", a[1], FALSE) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2]) && qp_arg_int(ctx, a[2], &buffering) < 0)
        return JS_EXCEPTION;
    if (qp_iom_check_str_arg(ctx, "open", "argument 'encoding'", a[3], TRUE) < 0 ||
        qp_iom_check_str_arg(ctx, "open", "argument 'errors'", a[4], TRUE) < 0 ||
        qp_iom_check_str_arg(ctx, "open", "argument 'newline'", a[5], TRUE) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[6]) && qp_arg_bool(ctx, a[6], &closefd) < 0)
        return JS_EXCEPTION;
    func = qp_iom_open_func(ctx, self);
    if (JS_IsException(func))
        return func;
    call_args[0] = JS_DupValue(ctx, a[0]);
    call_args[1] = QP_IS_NULL(a[1]) ? qp_str_from_cstr(ctx, "r") : JS_DupValue(ctx, a[1]);
    call_args[2] = JS_NewInt32(ctx, buffering);
    for (i = 3; i < 6; i++)
        call_args[i] = (QP_IS_NULL(a[i]) || qp_iom_is_none(a[i])) ? JS_UNDEFINED : JS_DupValue(ctx, a[i]);
    call_args[6] = qp_bool_new(closefd);
    call_args[7] = QP_IS_NULL(a[7]) ? JS_UNDEFINED : JS_DupValue(ctx, a[7]);
    if (JS_IsException(call_args[1]))
        r = JS_EXCEPTION;
    else
        r = qp_vectorcall(ctx, func, (JSValueConst *)call_args, 8, QP_NULL);
    for (i = 0; i < 8; i++)
        JS_FreeValue(ctx, call_args[i]);
    JS_FreeValue(ctx, func);
    return r;
}

/* open_code(path) (PyFile_OpenCodeObject without a hook) */
static JSValue qp_iom_m_open_code(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "path", NULL };
    QPArgSpec spec = { "open_code", kw, 1, 1, 0 };
    JSValueConst a[1];
    JSValue func, call_args[2], r, data, prelude, cls, path;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_iom_bad_argument(ctx, "open_code", "argument 'path'", "str", a[0]);
    if (qp_iom_state(ctx, self) == QP_IOM_LOADED) {
        /* _io.open(path, "rb") */
        func = qp_iom_get_prop(ctx, self, QP_IOM_OPEN_PROP);
        if (JS_IsException(func))
            return func;
        call_args[0] = JS_DupValue(ctx, a[0]);
        call_args[1] = qp_str_from_cstr(ctx, "rb");
        r = JS_IsException(call_args[1]) ? JS_EXCEPTION : qp_call2(ctx, func, call_args[0], call_args[1]);
        JS_FreeValue(ctx, call_args[0]);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, func);
        return r;
    }
    /* bootstrap: the classes are not loaded yet */
    path = qp_str_value(ctx, a[0]);
    if (JS_IsException(path))
        return path;
    data = qp_iom_read_file(ctx, path);
    if (JS_IsException(data)) {
        JS_FreeValue(ctx, path);
        return data;
    }
    prelude = qp_iom_get_prop(ctx, self, QP_IOM_PRELUDE_PROP);
    cls = JS_IsException(prelude) ? JS_EXCEPTION : qp_iom_dict_get(ctx, prelude, "_BootstrapCodeReader");
    JS_FreeValue(ctx, prelude);
    if (JS_IsException(cls) || QP_IS_NULL(cls)) {
        if (QP_IS_NULL(cls))
            qp_raise_system_error(ctx, "_io: prelude not initialized");
        JS_FreeValue(ctx, path);
        JS_FreeValue(ctx, data);
        return JS_EXCEPTION;
    }
    r = qp_call2(ctx, cls, path, data);
    JS_FreeValue(ctx, cls);
    JS_FreeValue(ctx, path);
    JS_FreeValue(ctx, data);
    return r;
}

/* text_encoding(encoding, stacklevel=2, /) */
static JSValue qp_iom_m_text_encoding(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    int stacklevel = 2;

    if (qp_arg_check_positional(ctx, "text_encoding", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (nargs > 1 && qp_arg_int(ctx, args[1], &stacklevel) < 0)
        return JS_EXCEPTION;
    if (!qp_iom_is_none(args[0]))
        return JS_DupValue(ctx, args[0]);
    if (cfg->warn_default_encoding) {
        if (qp_warn(ctx, QP_EXC(ctx, EncodingWarning), stacklevel,
                    "'encoding' argument not specified") < 0)
            return JS_EXCEPTION;
    }
    return qp_str_from_cstr(ctx, cfg->utf8_mode ? "utf-8" : "locale");
}

static const QPMethodDef qp_io_methods[] = {
    { "open", qp_iom_m_open, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "open($module, /, file, mode=\'r\', buffering=-1, encoding=None,\n"
      "     errors=None, newline=None, closefd=True, opener=None)\n"
      "--\n"
      "\n"
      "Open file and return a stream.  Raise OSError upon failure.\n"
      "\n"
      "file is either a text or byte string giving the name (and the path\n"
      "if the file isn\'t in the current working directory) of the file to\n"
      "be opened or an integer file descriptor of the file to be\n"
      "wrapped.  (If a file descriptor is given, it is closed when the\n"
      "returned I/O object is closed, unless closefd is set to False.)\n"
      "\n"
      "mode is an optional string that specifies the mode in which the file\n"
      "is opened.  It defaults to \'r\' which means open for reading in text\n"
      "mode.  Other common values are \'w\' for writing (truncating the file if\n"
      "it already exists), \'x\' for creating and writing to a new file, and\n"
      "\'a\' for appending (which on some Unix systems, means that all writes\n"
      "append to the end of the file regardless of the current seek position).\n"
      "In text mode, if encoding is not specified the encoding used is platform\n"
      "dependent: locale.getencoding() is called to get the current locale\n"
      "encoding.  (For reading and writing raw bytes use binary mode and leave\n"
      "encoding unspecified.)  The available modes are:\n"
      "\n"
      "========= ==========================================================\n"
      "Character Meaning\n"
      "--------- ----------------------------------------------------------\n"
      "\'r\'       open for reading (default)\n"
      "\'w\'       open for writing, truncating the file first\n"
      "\'x\'       create a new file and open it for writing\n"
      "\'a\'       open for writing, appending to the end of the file if it\n"
      "          exists\n"
      "\'b\'       binary mode\n"
      "\'t\'       text mode (default)\n"
      "\'+\'       open a disk file for updating (reading and writing)\n"
      "========= ==========================================================\n"
      "\n"
      "The default mode is \'rt\' (open for reading text).  For binary random\n"
      "access, the mode \'w+b\' opens and truncates the file to 0 bytes, while\n"
      "\'r+b\' opens the file without truncation.  The \'x\' mode implies \'w\' and\n"
      "raises an `FileExistsError` if the file already exists.\n"
      "\n"
      "Python distinguishes between files opened in binary and text modes,\n"
      "even when the underlying operating system doesn\'t.  Files opened in\n"
      "binary mode (appending \'b\' to the mode argument) return contents as\n"
      "bytes objects without any decoding.  In text mode (the default, or when\n"
      "\'t\' is appended to the mode argument), the contents of the file are\n"
      "returned as strings, the bytes having been first decoded using a\n"
      "platform-dependent encoding or using the specified encoding if given.\n"
      "\n"
      "buffering is an optional integer used to set the buffering policy.\n"
      "Pass 0 to switch buffering off (only allowed in binary mode), 1 to\n"
      "select line buffering (only usable in text mode), and an integer > 1 to\n"
      "indicate the size of a fixed-size chunk buffer.   When no buffering\n"
      "argument is given, the default buffering policy works as follows:\n"
      "\n"
      "* Binary files are buffered in fixed-size chunks; the size of the buffer\n"
      "  is max(min(blocksize, 8 MiB), DEFAULT_BUFFER_SIZE) when the device\n"
      "  block size is available.\n"
      "  On most systems, the buffer will typically be 128 kilobytes long.\n"
      "\n"
      "* \"Interactive\" text files (files for which isatty() returns True)\n"
      "  use line buffering.  Other text files use the policy described above\n"
      "  for binary files.\n"
      "\n"
      "encoding is the name of the encoding used to decode or encode the\n"
      "file.  This should only be used in text mode.  The default encoding is\n"
      "platform dependent, but any encoding supported by Python can be\n"
      "passed.  See the codecs module for the list of supported encodings.\n"
      "\n"
      "errors is an optional string that specifies how encoding errors are to\n"
      "be handled---this argument should not be used in binary mode.  Pass\n"
      "\'strict\' to raise a ValueError exception if there is an encoding error\n"
      "(the default of None has the same effect), or pass \'ignore\' to ignore\n"
      "errors.  (Note that ignoring encoding errors can lead to data loss.)\n"
      "See the documentation for codecs.register or run \'help(codecs.Codec)\'\n"
      "for a list of the permitted encoding error strings.\n"
      "\n"
      "newline controls how universal newlines works (it only applies to text\n"
      "mode).  It can be None, \'\', \'\\n\', \'\\r\', and \'\\r\\n\'.  It works as\n"
      "follows:\n"
      "\n"
      "* On input, if newline is None, universal newlines mode is enabled.\n"
      "  Lines in the input can end in \'\\n\', \'\\r\', or \'\\r\\n\', and these are\n"
      "  translated into \'\\n\' before being returned to the caller.  If it is\n"
      "  \'\', universal newline mode is enabled, but line endings are returned\n"
      "  to the caller untranslated.  If it has any of the other legal values,\n"
      "  input lines are only terminated by the given string, and the line\n"
      "  ending is returned to the caller untranslated.\n"
      "\n"
      "* On output, if newline is None, any \'\\n\' characters written are\n"
      "  translated to the system default line separator, os.linesep.  If\n"
      "  newline is \'\' or \'\\n\', no translation takes place.  If newline is any\n"
      "  of the other legal values, any \'\\n\' characters written are translated\n"
      "  to the given string.\n"
      "\n"
      "If closefd is False, the underlying file descriptor will be kept open\n"
      "when the file is closed.  This does not work when a file name is given\n"
      "and must be True in that case.\n"
      "\n"
      "A custom opener can be used by passing a callable as *opener*.  The\n"
      "underlying file descriptor for the file object is then obtained by\n"
      "calling *opener* with (*file*, *flags*).  *opener* must return an open\n"
      "file descriptor (passing os.open as *opener* results in functionality\n"
      "similar to passing None).\n"
      "\n"
      "open() returns a file object whose type depends on the mode, and\n"
      "through which the standard file operations such as reading and writing\n"
      "are performed.  When open() is used to open a file in a text mode (\'w\',\n"
      "\'r\', \'wt\', \'rt\', etc.), it returns a TextIOWrapper.  When used to open\n"
      "a file in a binary mode, the returned class varies: in read binary\n"
      "mode, it returns a BufferedReader; in write binary and append binary\n"
      "modes, it returns a BufferedWriter, and in read/write mode, it returns\n"
      "a BufferedRandom.\n"
      "\n"
      "It is also possible to use a string or bytearray as a file for both\n"
      "reading and writing.  For strings StringIO can be used like a file\n"
      "opened in a text mode, and for bytes a BytesIO can be used like a file\n"
      "opened in a binary mode." },
    { "text_encoding", qp_iom_m_text_encoding, QP_METH_FASTCALL, 0,
      "text_encoding($module, encoding, stacklevel=2, /)\n"
      "--\n"
      "\n"
      "A helper function to choose the text encoding.\n"
      "\n"
      "When encoding is not None, this function returns it.\n"
      "Otherwise, this function returns the default text encoding\n"
      "(i.e. \"locale\" or \"utf-8\" depends on UTF-8 mode).\n"
      "\n"
      "This function emits an EncodingWarning if encoding is None and\n"
      "sys.flags.warn_default_encoding is true.\n"
      "\n"
      "This can be used in APIs with an encoding=None parameter.\n"
      "However, please consider using encoding=\"utf-8\" for new APIs." },
    { "open_code", qp_iom_m_open_code, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "open_code($module, /, path)\n"
      "--\n"
      "\n"
      "Opens the provided file with the intent to import the contents.\n"
      "\n"
      "This may perform extra validation beyond open(), but is otherwise\n"
      "interchangeable with calling open(path, \'rb\')." },
    { "__getattr__", qp_iom_m_getattr, QP_METH_O, 0, NULL },
    { NULL },
};

static int qp_io_exec(JSContext *ctx, JSValueConst module)
{
    JSValue dict = qp_module_get_dict(module), ns, r, name, bases, items[2], cls, prelude;
    JSAtom a;
    int rc;

    /* DEFAULT_BUFFER_SIZE */
    a = JS_NewAtom(ctx, "DEFAULT_BUFFER_SIZE");
    if (a == JS_ATOM_NULL)
        return -1;
    rc = qp_dict_set_item_atom(ctx, dict, a, JS_NewInt32(ctx, QP_IOM_DEFAULT_BUFFER_SIZE));
    JS_FreeAtom(ctx, a);
    if (rc < 0)
        return -1;

    /* UnsupportedOperation inherits from ValueError and OSError:
       type("UnsupportedOperation", (OSError, ValueError), {}) with
       __module__ = "io" */
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns))
        return -1;
    r = qp_str_from_cstr(ctx, "io");
    rc = JS_IsException(r) ? -1 : qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __module__), r);
    JS_FreeValue(ctx, r);
    if (rc < 0) {
        JS_FreeValue(ctx, ns);
        return -1;
    }
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, OSError)));
    items[1] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, ValueError)));
    bases = qp_tuple_new_steal(ctx, 2, items);
    name = qp_str_from_cstr(ctx, "UnsupportedOperation");
    if (JS_IsException(bases) || JS_IsException(name)) {
        JS_FreeValue(ctx, bases);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, ns);
        return -1;
    }
    cls = qp_type_new(ctx, QP_TYPE(ctx, type), name, bases, ns, QP_NULL);
    JS_FreeValue(ctx, bases);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, ns);
    if (JS_IsException(cls))
        return -1;
    a = JS_NewAtom(ctx, "UnsupportedOperation");
    rc = a == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, dict, a, cls);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, cls);
    if (rc < 0)
        return -1;

    /* BlockingIOError */
    a = JS_NewAtom(ctx, "BlockingIOError");
    rc = a == JS_ATOM_NULL ? -1
        : qp_dict_set_item_atom(ctx, dict, a, QP_TYPE_VALUE(QP_EXC(ctx, BlockingIOError)));
    JS_FreeAtom(ctx, a);
    if (rc < 0)
        return -1;

    /* the prelude: bootstrap open_code() reader and the _pyio loader */
    prelude = qp_dict_new(ctx);
    if (JS_IsException(prelude))
        return -1;
    r = qp_str_from_cstr(ctx, "_io");
    rc = JS_IsException(r) ? -1 : qp_dict_set_item_atom(ctx, prelude, QP_ATOM(ctx, __name__), r);
    JS_FreeValue(ctx, r);
    if (rc < 0) {
        JS_FreeValue(ctx, prelude);
        return -1;
    }
    r = qp_run_string(ctx, qp_iom_prelude_src, sizeof(qp_iom_prelude_src) - 1, "<_io prelude>",
                      257 /* Py_file_input */, prelude, prelude);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, prelude);
        return -1;
    }
    JS_FreeValue(ctx, r);
    if (qp_iom_set_prop(ctx, module, QP_IOM_PRELUDE_PROP, prelude) < 0)
        return -1;
    return qp_iom_set_state(ctx, module, QP_IOM_NOT_LOADED);
}

static const QPModuleDef qp_moddef__io = {
    "_io",
    "The io module provides the Python interfaces to stream handling. The\n"
    "builtin open function is defined in this module.\n"
    "\n"
    "At the top of the I/O hierarchy is the abstract base class IOBase. It\n"
    "defines the basic interface to a stream. Note, however, that there is no\n"
    "separation between reading and writing to streams; implementations are\n"
    "allowed to raise an OSError if they do not support a given operation.\n"
    "\n"
    "Extending IOBase is RawIOBase which deals simply with the reading and\n"
    "writing of raw bytes to a stream. FileIO subclasses RawIOBase to provide\n"
    "an interface to OS files.\n"
    "\n"
    "BufferedIOBase deals with buffering on a raw byte stream (RawIOBase). Its\n"
    "subclasses, BufferedWriter, BufferedReader, and BufferedRWPair buffer\n"
    "streams that are readable, writable, and both respectively.\n"
    "BufferedRandom provides a buffered interface to random access\n"
    "streams. BytesIO is a simple stream of in-memory bytes.\n"
    "\n"
    "Another IOBase subclass, TextIOBase, deals with the encoding and decoding\n"
    "of streams into text. TextIOWrapper, which extends it, is a buffered text\n"
    "interface to a buffered raw stream (`BufferedIOBase`). Finally, StringIO\n"
    "is an in-memory stream for text.\n"
    "\n"
    "Argument names are not part of the specification, and only the arguments\n"
    "of open() are intended to be used as keyword arguments.\n"
    "\n"
    "data:\n"
    "\n"
    "DEFAULT_BUFFER_SIZE\n"
    "\n"
    "   An int containing the default buffer size used by the module's buffered\n"
    "   I/O classes.\n",
    qp_io_methods,
    qp_io_exec,
};

/* ========================================================================= */
/* sys.std* and builtins.open (Python/pylifecycle.c)                          */
/* ========================================================================= */

/* _Py_IsValidFD */
static BOOL qp_iom_is_valid_fd(int fd)
{
    if (fd < 0)
        return FALSE;
    return fcntl(fd, F_GETFD) >= 0;
}

/* create_stdio: io.TextIOWrapper(io.open(fd, mode, buffering, closefd=False), ...) */
static JSValue qp_iom_create_stdio(JSContext *ctx, JSValueConst io, int fd, int write_mode,
                                   const char *name, const char *encoding, const char *errors)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    int buffered_stdio = !cfg->unbuffered;
    int buffering, isatty;
    JSValue buf = JS_UNDEFINED, raw = JS_UNDEFINED, stream = JS_UNDEFINED, func, res;
    JSValue a[7];
    int i;

    if (!qp_iom_is_valid_fd(fd))
        return JS_UNDEFINED;
    /* stdin is always opened in buffered mode, first because it shouldn't
       make a difference in common use cases, second because TextIOWrapper
       depends on the presence of a read1() method which only exists on
       buffered streams. */
    buffering = (!buffered_stdio && write_mode) ? 0 : -1;
    func = qp_getattr(ctx, io, QP_ATOM(ctx, open));
    if (JS_IsException(func))
        goto error;
    a[0] = JS_NewInt32(ctx, fd);
    a[1] = qp_str_from_cstr(ctx, write_mode ? "wb" : "rb");
    a[2] = JS_NewInt32(ctx, buffering);
    a[3] = JS_UNDEFINED;    /* encoding */
    a[4] = JS_UNDEFINED;    /* errors */
    a[5] = JS_UNDEFINED;    /* newline */
    a[6] = JS_FALSE;        /* closefd */
    buf = JS_IsException(a[1]) ? JS_EXCEPTION : qp_vectorcall(ctx, func, (JSValueConst *)a, 7, QP_NULL);
    JS_FreeValue(ctx, a[1]);
    JS_FreeValue(ctx, func);
    if (JS_IsException(buf))
        goto error;
    if (buffering) {
        raw = qp_iom_getattr(ctx, buf, "raw");
        if (JS_IsException(raw))
            goto error;
    } else {
        raw = JS_DupValue(ctx, buf);
    }
    if (qp_iom_setattr(ctx, raw, "name", qp_str_from_cstr(ctx, name)) < 0)
        goto error;
    {
        JSAtom at = JS_NewAtom(ctx, "isatty");
        res = at == JS_ATOM_NULL ? JS_EXCEPTION : qp_call_method(ctx, raw, at, NULL, 0, QP_NULL);
        JS_FreeAtom(ctx, at);
    }
    if (JS_IsException(res))
        goto error;
    isatty = qp_bool(ctx, res);
    JS_FreeValue(ctx, res);
    if (isatty < 0)
        goto error;
    JS_FreeValue(ctx, raw);
    raw = JS_UNDEFINED;

    func = qp_iom_getattr(ctx, io, "TextIOWrapper");
    if (JS_IsException(func))
        goto error;
    a[0] = buf;
    a[1] = qp_str_from_cstr(ctx, encoding);
    a[2] = qp_str_from_cstr(ctx, errors);
    a[3] = qp_str_from_cstr(ctx, "\n");     /* newline */
    /* line_buffering */
    a[4] = qp_bool_new(buffered_stdio && (isatty || fd == STDERR_FILENO));
    /* write_through */
    a[5] = qp_bool_new(!buffered_stdio);
    stream = JS_EXCEPTION;
    if (!JS_IsException(a[1]) && !JS_IsException(a[2]) && !JS_IsException(a[3]))
        stream = qp_vectorcall(ctx, func, (JSValueConst *)a, 6, QP_NULL);
    for (i = 1; i < 4; i++)
        JS_FreeValue(ctx, a[i]);
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, buf);
    buf = JS_UNDEFINED;
    if (JS_IsException(stream))
        goto error;
    if (qp_iom_setattr(ctx, stream, "mode", qp_str_from_cstr(ctx, write_mode ? "w" : "r")) < 0)
        goto error;
    return stream;

error:
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, raw);
    JS_FreeValue(ctx, stream);
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, OSError)) && !qp_iom_is_valid_fd(fd)) {
        /* Issue #24891: the file descriptor was closed after the first
           is-valid check.  Ignore the OSError and set the stream to None. */
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return JS_UNDEFINED;
    }
    return JS_EXCEPTION;
}

/* sys.<name> = sys.__<name>__ = std */
static int qp_iom_set_std(JSContext *ctx, const char *name, JSValueConst std)
{
    char dunder[32];
    JSValueConst sys = QP_STATE(ctx)->sys_module;
    snprintf(dunder, sizeof(dunder), "__%s__", name);
    if (qp_iom_setattr(ctx, sys, dunder, JS_DupValue(ctx, std)) < 0)
        return -1;
    return qp_iom_setattr(ctx, sys, name, JS_DupValue(ctx, std));
}

/* init_set_builtins_open() + init_sys_streams(), called by the importlib
   boot code once the import system and the codec registry are ready.
   0, or -1 with an exception pending (CPython: fatal "can't initialize
   sys standard streams"). */
static int qp_io_install_stdio(JSContext *ctx)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    JSValue io, wrapper, std, encattr;
    char encoding_buf[128], errors_buf[64];
    const char *encoding = NULL, *errors = NULL;
    struct stat sb;
    int rc;

    io = qp_import_module(ctx, "_io");
    if (JS_IsException(io))
        return -1;

    /* init_set_builtins_open: builtins.open = _io.open */
    wrapper = qp_getattr(ctx, io, QP_ATOM(ctx, open));
    if (JS_IsException(wrapper))
        goto fail;
    rc = qp_setattr(ctx, QP_STATE(ctx)->builtins_module, QP_ATOM(ctx, open), wrapper);
    JS_FreeValue(ctx, wrapper);
    if (rc < 0)
        goto fail;

    /* Check that stdin is not a directory.  Using shell redirection, you
       can redirect stdin to a directory, crashing the Python interpreter.
       Catch this common mistake here and output a useful error message. */
    if (fstat(STDIN_FILENO, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        qp_raise_runtime_error(ctx, "<stdin> is a directory, cannot continue");
        goto fail;
    }

    /* config_init_stdio_encoding: PYTHONIOENCODING=encoding[:errors] */
    if (cfg->stdio_encoding) {
        const char *opt = cfg->stdio_encoding, *colon = strchr(opt, ':');
        size_t n = colon ? (size_t)(colon - opt) : strlen(opt);
        if (n > 0 && n < sizeof(encoding_buf)) {
            memcpy(encoding_buf, opt, n);
            encoding_buf[n] = '\0';
            encoding = encoding_buf;
        }
        if (colon && colon[1] && strlen(colon + 1) < sizeof(errors_buf)) {
            strcpy(errors_buf, colon + 1);
            errors = errors_buf;
        }
        /* If the encoding is set but not the error handler, use "strict"
           (PYTHONIOENCODING=latin1 behaves as latin1:strict). */
        if (encoding && !errors)
            errors = "strict";
    }
    if (!encoding) {
        /* config_get_locale_encoding */
        if (cfg->utf8_mode) {
            encoding = "utf-8";
        } else {
            const char *cs = nl_langinfo(CODESET);
            if (!cs || !cs[0] || strlen(cs) >= sizeof(encoding_buf)) {
                encoding = "utf-8";
            } else {
                strcpy(encoding_buf, cs);
                encoding = encoding_buf;
            }
        }
    }
    if (!errors) {
        /* config_get_stdio_errors */
        if (cfg->utf8_mode) {
            errors = "surrogateescape";
        } else {
            const char *loc = setlocale(LC_CTYPE, NULL);
            errors = (loc && (!strcmp(loc, "C") || !strcmp(loc, "POSIX"))) ? "surrogateescape"
                                                                          : "strict";
        }
    }

    /* Set sys.stdin */
    std = qp_iom_create_stdio(ctx, io, STDIN_FILENO, 0, "<stdin>", encoding, errors);
    if (JS_IsException(std))
        goto fail;
    rc = qp_iom_set_std(ctx, "stdin", std);
    JS_FreeValue(ctx, std);
    if (rc < 0)
        goto fail;

    /* Set sys.stdout */
    std = qp_iom_create_stdio(ctx, io, STDOUT_FILENO, 1, "<stdout>", encoding, errors);
    if (JS_IsException(std))
        goto fail;
    rc = qp_iom_set_std(ctx, "stdout", std);
    JS_FreeValue(ctx, std);
    if (rc < 0)
        goto fail;

    /* Set sys.stderr, replaces the preliminary stderr */
    std = qp_iom_create_stdio(ctx, io, STDERR_FILENO, 1, "<stderr>", encoding, "backslashreplace");
    if (JS_IsException(std))
        goto fail;
    /* pre-import stderr's codec to avoid recursion when import.c tries to
       write to stderr in verbose mode */
    if (!qp_iom_is_none(std)) {
        encattr = qp_iom_getattr(ctx, std, "encoding");
        if (!JS_IsException(encattr) && qp_str_check(ctx, encattr)) {
            JSValue s = qp_str_value(ctx, encattr);
            size_t len;
            const char *cs = JS_IsException(s) ? NULL : qp_str_utf8(ctx, s, &len);
            if (cs) {
                JS_FreeValue(ctx, qp_codec_lookup(ctx, cs));
                JS_FreeCString(ctx, cs);
            }
            JS_FreeValue(ctx, s);
        }
        JS_FreeValue(ctx, encattr);
        /* Not a fatal error if codec isn't available */
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    rc = qp_iom_set_std(ctx, "stderr", std);
    JS_FreeValue(ctx, std);
    if (rc < 0)
        goto fail;
    JS_FreeValue(ctx, io);
    return 0;
fail:
    JS_FreeValue(ctx, io);
    return -1;
}
