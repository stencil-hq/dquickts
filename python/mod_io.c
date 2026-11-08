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
 * open(path, 'rb') like CPython.  While _pyio runs, open(fd, 'wb') (what
 * importlib's _write_atomic does to write the .pyc of a module _pyio
 * imports) returns a minimal writer over the descriptor.
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
    "class _BootstrapFileWriter:\n"
    "    \"\"\"io.open(fd, 'wb') result while the _io classes are being loaded.\"\"\"\n"
    "\n"
    "    __slots__ = ('name', '_fd')\n"
    "\n"
    "    def __init__(self, fd):\n"
    "        self.name = fd\n"
    "        self._fd = fd\n"
    "\n"
    "    def write(self, data):\n"
    "        import posix\n"
    "        if self._fd < 0:\n"
    "            raise ValueError('I/O operation on closed file.')\n"
    "        data = memoryview(data).cast('B')\n"
    "        pos = 0\n"
    "        while pos < len(data):\n"
    "            pos += posix.write(self._fd, data[pos:])\n"
    "        return pos\n"
    "\n"
    "    def close(self):\n"
    "        import posix\n"
    "        fd = self._fd\n"
    "        if fd >= 0:\n"
    "            self._fd = -1\n"
    "            posix.close(fd)\n"
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
    "# _pyio raises ValueError/UnsupportedOperation with its own wording; the C\n"
    "# _io module (what CPython users see) words them differently and sometimes\n"
    "# checks for a closed file earlier.  Each entry is (scope, old, new, count):\n"
    "# old must occur exactly count times inside the source of the module-level\n"
    "# class or function named scope (old None: new replaces that whole scope).\n"
    "_IO_CLOSED = 'I/O operation on closed file'\n"
    "_IO_REWRITES = (\n"
    "    ('open', 'raise TypeError(\"invalid mode: %r\" % mode)',\n"
    "     'raise TypeError(\"open() argument \\'mode\\' must be str, not %s\"\\n'\n"
    "     '                        % type(mode).__name__)', 1),\n"
    "    ('open', 'raise TypeError(\"invalid buffering: %r\" % buffering)',\n"
    "     'raise TypeError(\"\\'%s\\' object cannot be interpreted as an integer\"\\n'\n"
    "     '                        % type(buffering).__name__)', 1),\n"
    "    ('open', 'raise TypeError(\"invalid encoding: %r\" % encoding)',\n"
    "     'raise TypeError(\"open() argument \\'encoding\\' must be str or None, not %s\"\\n'\n"
    "     '                        % type(encoding).__name__)', 1),\n"
    "    ('open',\n"
    "     '        raise TypeError(\"invalid errors: %r\" % errors)\\n',\n"
    "     '        raise TypeError(\"open() argument \\'errors\\' must be str or None, not %s\"\\n'\n"
    "     '                        % type(errors).__name__)\\n'\n"
    "     '    if newline is not None and not isinstance(newline, str):\\n'\n"
    "     '        raise TypeError(\"open() argument \\'newline\\' must be str or None, not %s\"\\n'\n"
    "     '                        % type(newline).__name__)\\n', 1),\n"
    "    ('open', '\"invalid mode: %r\" % mode', '\"invalid mode: \\'%s\\'\" % mode', 1),\n"
    "    ('open', '\"can\\'t have read/write/append mode at once\"',\n"
    "     '\"must have exactly one of create/read/write/append mode\"', 1),\n"
    "    # C leaves a missing mode to FileIO, after the binary-mode checks.\n"
    "    ('open',\n"
    "     '    if not (creating or reading or writing or appending):\\n'\n"
    "     '        raise ValueError(\"must have exactly one of read/write/append mode\")\\n',\n"
    "     '', 1),\n"
    "    ('IOBase', 'raise TypeError(f\"{size!r} is not an integer\")',\n"
    "     'raise TypeError(f\"argument should be integer or None, not \"\\n'\n"
    "     '                                f\"\\'{type(size).__name__}\\'\")', 1),\n"
    "    ('IOBase',\n"
    "     'raise UnsupportedOperation(\"%s.%s() not supported\" %\\n'\n"
    "     '                                   (self.__class__.__name__, name))\\n',\n"
    "     'raise UnsupportedOperation(name)\\n', 1),\n"
    "    ('_BufferedIOMixin',\n"
    "     '        self._checkClosed()\\n'\n"
    "     '        self._checkWritable()\\n',\n"
    "     '        self._checkClosed(\"truncate of closed file\")\\n'\n"
    "     '        self._checkWritable(\"truncate\")\\n', 1),\n"
    "    ('_BufferedIOMixin', '\"flush on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO',\n"
    "     '            buf += initial_bytes\\n',\n"
    "     '            try:\\n'\n"
    "     '                buf += initial_bytes\\n'\n"
    "     '            except TypeError:\\n'\n"
    "     '                raise TypeError(\"a bytes-like object is required, not \\'%s\\'\"\\n'\n"
    "     '                                % type(initial_bytes).__name__) from None\\n', 1),\n"
    "    ('BytesIO',\n"
    "     '        if isinstance(b, str):\\n'\n"
    "     '            raise TypeError(\"can\\'t write str to binary stream\")\\n'\n"
    "     '        with memoryview(b) as view:\\n',\n"
    "     '        try:\\n'\n"
    "     '            view = memoryview(b)\\n'\n"
    "     '        except TypeError:\\n'\n"
    "     '            raise TypeError(\"a bytes-like object is required, not \\'%s\\'\"\\n'\n"
    "     '                            % type(b).__name__) from None\\n'\n"
    "     '        with view:\\n', 1),\n"
    "    ('BytesIO', 'raise TypeError(f\"{size!r} is not an integer\")',\n"
    "     'raise TypeError(f\"argument should be integer or None, not \"\\n'\n"
    "     '                                f\"\\'{type(size).__name__}\\'\")', 1),\n"
    "    ('BytesIO', 'raise TypeError(f\"{pos!r} is not an integer\")',\n"
    "     'raise TypeError(f\"\\'{type(pos).__name__}\\' object cannot be \"\\n'\n"
    "     '                                 \"interpreted as an integer\")', 2),\n"
    "    ('BytesIO', '\"__getstate__ on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"getvalue on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"getbuffer on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"read from closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"write to closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"seek on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"tell on closed file\"', '\"' + _IO_CLOSED + '.\"', 1),\n"
    "    ('BytesIO', '\"negative seek position %r\"', '\"negative seek value %r\"', 1),\n"
    "    ('BytesIO', 'raise ValueError(\"unsupported whence value\")',\n"
    "     'raise ValueError(\"invalid whence (%d, should be 0, 1 or 2)\" % (whence,))', 1),\n"
    "    ('BufferedReader',\n"
    "     '            raise ValueError(\"invalid number of bytes to read\")\\n'\n"
    "     '        with self._read_lock:\\n',\n"
    "     '            raise ValueError(\"read length must be non-negative or -1\")\\n'\n"
    "     '        self._checkClosed(\"read of closed file\")\\n'\n"
    "     '        with self._read_lock:\\n', 1),\n"
    "    ('BufferedReader',\n"
    "     '    def _peek_unlocked(self, n=0):\\n',\n"
    "     '    def readline(self, size=-1):\\n'\n"
    "     '        self._checkClosed(\"readline of closed file\")\\n'\n"
    "     '        return super().readline(size)\\n'\n"
    "     '\\n'\n"
    "     '    def _peek_unlocked(self, n=0):\\n', 1),\n"
    "    # peek: Modules/_io returns the buffered bytes when there are any and\n"
    "    # reads the raw stream only when the buffer is empty (a pipe reader,\n"
    "    # e.g. pickle.load prefetching through peek, must not block)\n"
    "    ('BufferedReader', '        if have < want or have <= 0:\\n',\n"
    "     '        if have <= 0:\\n', 1),\n"
    "    ('BufferedReader', '\"invalid buffer size\"', '\"buffer size must be strictly positive\"', 1),\n"
    "    ('BufferedReader',\n"
    "     '        if size is not None and size < -1:\\n',\n"
    "     '        if size is not None and not isinstance(size, int):\\n'\n"
    "     '            try:\\n'\n"
    "     '                size = size.__index__()\\n'\n"
    "     '            except AttributeError:\\n'\n"
    "     '                raise TypeError(\"argument should be integer or None, not \\'%s\\'\"\\n'\n"
    "     '                                % type(size).__name__) from None\\n'\n"
    "     '        if size is not None and size < -1:\\n', 1),\n"
    "    ('BufferedWriter', '\"invalid buffer size\"', '\"buffer size must be strictly positive\"', 1),\n"
    "    ('BufferedWriter',\n"
    "     '        if isinstance(b, str):\\n'\n"
    "     '            raise TypeError(\"can\\'t write str to binary stream\")\\n',\n"
    "     '        if isinstance(b, str):\\n'\n"
    "     '            raise TypeError(\"a bytes-like object is required, not \\'str\\'\")\\n', 1),\n"
    "    ('BufferedWriter',\n"
    "     '            self._write_buf.extend(b)\\n',\n"
    "     '            try:\\n'\n"
    "     '                self._write_buf.extend(b)\\n'\n"
    "     '            except TypeError:\\n'\n"
    "     '                raise TypeError(\"a bytes-like object is required, not \\'%s\\'\"\\n'\n"
    "     '                                % type(b).__name__) from None\\n', 1),\n"
    "    ('BufferedReader', 'raise ValueError(\"invalid whence value\")',\n"
    "     'raise ValueError(\"whence value %d unsupported\" % (whence,))', 1),\n"
    "    # seek: Modules/_io moves inside the read buffer without seeking the\n"
    "    # raw stream (_io__Buffered_seek_impl); a DecompressReader (lzma, gzip,\n"
    "    # bz2 files) seeks backwards by decompressing again from the start, so\n"
    "    # tarfile's member walk over a .tar.xz would be quadratic\n"
    "    ('BufferedReader',\n"
    "     '            pos = _BufferedIOMixin.seek(self, pos, whence)\\n',\n"
    "     '            if whence != 2 and self._read_buf:\\n'\n"
    "     '                raw_pos = _BufferedIOMixin.tell(self)\\n'\n"
    "     '                start = raw_pos - len(self._read_buf)\\n'\n"
    "     '                target = pos if whence == 0 else raw_pos + pos\\n'\n"
    "     '                if start <= target <= raw_pos:\\n'\n"
    "     '                    self._read_pos = target - start\\n'\n"
    "     '                    return max(target, 0)\\n'\n"
    "     '            pos = _BufferedIOMixin.seek(self, pos, whence)\\n', 1),\n"
    "    ('BufferedWriter',\n"
    "     '    def truncate(self, pos=None):\\n'\n"
    "     '        with self._write_lock:\\n',\n"
    "     '    def truncate(self, pos=None):\\n'\n"
    "     '        self._checkClosed(\"truncate of closed file\")\\n'\n"
    "     '        with self._write_lock:\\n', 1),\n"
    "    ('BufferedWriter', '\"flush on closed file\"', '\"flush of closed file\"', 1),\n"
    "    ('BufferedWriter',\n"
    "     '            raise ValueError(\"invalid whence value\")\\n'\n"
    "     '        with self._write_lock:\\n',\n"
    "     '            raise ValueError(\"whence value %d unsupported\" % (whence,))\\n'\n"
    "     '        self._checkClosed(\"seek of closed file\")\\n'\n"
    "     '        with self._write_lock:\\n', 1),\n"
    "    ('BufferedRandom',\n"
    "     '            raise ValueError(\"invalid whence value\")\\n'\n"
    "     '        self.flush()\\n',\n"
    "     '            raise ValueError(\"whence value %d unsupported\" % (whence,))\\n'\n"
    "     '        self._checkClosed(\"seek of closed file\")\\n'\n"
    "     '        self.flush()\\n', 1),\n"
    "    ('BufferedRandom',\n"
    "     '    def truncate(self, pos=None):\\n',\n"
    "     '    def truncate(self, pos=None):\\n'\n"
    "     '        self._checkClosed(\"truncate of closed file\")\\n', 1),\n"
    "    ('BufferedRandom',\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.read(',\n"
    "     '        self._checkClosed(\"read of closed file\")\\n'\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.read(', 1),\n"
    "    ('BufferedRandom',\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.read1(',\n"
    "     '        self._checkClosed(\"read of closed file\")\\n'\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.read1(', 1),\n"
    "    ('BufferedRandom',\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.readinto',\n"
    "     '        self._checkClosed(\"readinto of closed file\")\\n'\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.readinto', 2),\n"
    "    ('BufferedRandom',\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.peek(',\n"
    "     '        self._checkClosed(\"peek of closed file\")\\n'\n"
    "     '        self.flush()\\n'\n"
    "     '        return BufferedReader.peek(', 1),\n"
    "    ('FileIO',\n"
    "     '        Return an empty bytes object at EOF.\\n'\n"
    "     '        \"\"\"\\n'\n"
    "     '        self._checkClosed()\\n',\n"
    "     '        Return an empty bytes object at EOF.\\n'\n"
    "     '        \"\"\"\\n'\n"
    "     '        if size is not None and not isinstance(size, int):\\n'\n"
    "     '            try:\\n'\n"
    "     '                size = size.__index__()\\n'\n"
    "     '            except AttributeError:\\n'\n"
    "     '                raise TypeError(\"argument should be integer or None, not \\'%s\\'\"\\n'\n"
    "     '                                % type(size).__name__) from None\\n'\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('FileIO',\n"
    "     '        else:\\n'\n"
    "     '            fd = -1\\n',\n"
    "     '        else:\\n'\n"
    "     '            fd = -1\\n'\n"
    "     '            os.fspath(file)  # C wording for a bad path type\\n', 1),\n"
    "    ('FileIO', 'self._checkClosed()', 'self._checkClosed(\"' + _IO_CLOSED + '\")', 12),\n"
    "    ('FileIO', \"raise TypeError('integer argument expected, got float')\",\n"
    "     \"raise TypeError('expected str, bytes or os.PathLike object, not float')\", 1),\n"
    "    ('FileIO', \"raise TypeError('invalid mode: %s' % (mode,))\",\n"
    "     \"raise TypeError(\\\"FileIO() argument 'mode' must be str, not %s\\\"\\n\"\n"
    "     \"                            % type(mode).__name__)\", 1),\n"
    "    ('TextIOWrapper', 'raise ValueError(\"invalid encoding: %r\" % encoding)',\n"
    "     'raise TypeError(\"TextIOWrapper() argument \\'encoding\\' must be \"\\n'\n"
    "     '                            \"str or None, not %s\" % type(encoding).__name__)', 1),\n"
    "    ('TextIOWrapper', 'raise ValueError(\"invalid errors: %r\" % errors)',\n"
    "     'raise TypeError(\"TextIOWrapper() argument \\'errors\\' must be \"\\n'\n"
    "     '                                \"str or None, not %s\" % type(errors).__name__)', 1),\n"
    "    ('TextIOWrapper', 'raise TypeError(\"illegal newline type: %r\" % (type(newline),))',\n"
    "     'raise TypeError(\"TextIOWrapper() argument \\'newline\\' must be \"\\n'\n"
    "     '                            \"str or None, not %s\" % type(newline).__name__)', 1),\n"
    "    ('TextIOWrapper', '\"illegal newline value: %r\" % (newline,)',\n"
    "     '\"illegal newline value: %s\" % (newline,)', 1),\n"
    "    ('TextIOWrapper', 'raise TypeError(\"invalid errors: %r\" % errors)',\n"
    "     'raise TypeError(\"reconfigure() argument \\'errors\\' must be \"\\n'\n"
    "     '                            \"str or None, not %s\" % type(errors).__name__)', 1),\n"
    "    ('TextIOWrapper', 'raise TypeError(\"invalid encoding: %r\" % encoding)',\n"
    "     'raise TypeError(\"reconfigure() argument \\'encoding\\' must be \"\\n'\n"
    "     '                                \"str or None, not %s\" % type(encoding).__name__)', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        if newline is Ellipsis:\\n'\n"
    "     '            newline = self._readnl\\n',\n"
    "     '        if newline is Ellipsis:\\n'\n"
    "     '            newline = self._readnl\\n'\n"
    "     '        elif newline is not None and not isinstance(newline, str):\\n'\n"
    "     '            raise TypeError(\"reconfigure() argument \\'newline\\' must be \"\\n'\n"
    "     '                            \"str or None, not %s\" % type(newline).__name__)\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        if self.closed:\\n'\n"
    "     '            raise ValueError(\"read from closed file\")\\n'\n"
    "     '        if size is None:\\n'\n"
    "     '            size = -1\\n'\n"
    "     '        else:\\n'\n"
    "     '            try:\\n'\n"
    "     '                size_index = size.__index__\\n'\n"
    "     '            except AttributeError:\\n'\n"
    "     '                raise TypeError(f\"{size!r} is not an integer\")\\n'\n"
    "     '            else:\\n'\n"
    "     '                size = size_index()\\n',\n"
    "     '        if size is None:\\n'\n"
    "     '            size = -1\\n'\n"
    "     '        else:\\n'\n"
    "     '            try:\\n'\n"
    "     '                size_index = size.__index__\\n'\n"
    "     '            except AttributeError:\\n'\n"
    "     '                raise TypeError(f\"\\'{type(size).__name__}\\' object cannot be \"\\n'\n"
    "     '                                 \"interpreted as an integer\")\\n'\n"
    "     '            else:\\n'\n"
    "     '                size = size_index()\\n'\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('TextIOWrapper', 'raise TypeError(f\"{size!r} is not an integer\")',\n"
    "     'raise TypeError(f\"argument should be integer or None, not \"\\n'\n"
    "     '                                f\"\\'{type(size).__name__}\\'\")', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '            raise ValueError(\"I/O operation on closed file.\")\\n'\n"
    "     '        return self._seekable\\n',\n"
    "     '            return self.buffer.seekable()\\n'\n"
    "     '        return self._seekable\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '    def flush(self):\\n',\n"
    "     '    def flush(self):\\n'\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        if self.closed:\\n'\n"
    "     '            raise ValueError(\"write to closed file\")\\n'\n"
    "     '        if not isinstance(s, str):\\n'\n"
    "     '            raise TypeError(\"can\\'t write %s to text stream\" %\\n'\n"
    "     '                            s.__class__.__name__)\\n',\n"
    "     '        if not isinstance(s, str):\\n'\n"
    "     '            raise TypeError(\"write() argument must be str, not %s\"\\n'\n"
    "     '                            % type(s).__name__)\\n'\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '    def tell(self):\\n',\n"
    "     '    def tell(self):\\n'\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        if self.closed:\\n'\n"
    "     '            raise ValueError(\"tell on closed file\")\\n',\n"
    "     '        self._checkClosed()\\n', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        if self._decoder is None:\\n'\n"
    "     '            raise ValueError(\"no decoder\")\\n',\n"
    "     '        self._checkReadable(\"not readable\")\\n'\n"
    "     '        if self._decoder is None:\\n'\n"
    "     '            raise ValueError(\"no decoder\")\\n', 1),\n"
    "    ('TextIOWrapper', '\"unsupported whence (%r)\"',\n"
    "     '\"invalid whence (%r, should be 0, 1 or 2)\"', 1),\n"
    "    ('TextIOWrapper',\n"
    "     '        self._checkReadable()\\n',\n"
    "     '        self._checkClosed()\\n'\n"
    "     '        self._checkReadable(\"not readable\")\\n', 1),\n"
    "    ('IOBase', '            if n >= hint:\\n', '            if n > hint:\\n', 1),\n"
    "    # C _IOBase.flush only looks at what _IOBase.close() set\n"
    "    ('IOBase',\n"
    "     '        self._checkClosed()\\n'\n"
    "     '        # XXX Should this return the number of bytes written???\\n',\n"
    "     '        if self.__closed:\\n'\n"
    "     '            raise ValueError(\"' + _IO_CLOSED + '.\")\\n', 1),\n"
    "    # iobase_finalize ignores any error from reading `closed`\n"
    "    ('IOBase',\n"
    "     '        except AttributeError:\\n'\n"
    "     '            # If getting closed fails, then the object is probably\\n',\n"
    "     '        except Exception:\\n'\n"
    "     '            # If getting closed fails, then the object is probably\\n', 1),\n"
    ")\n"
    "\n"
    "\n"
    "# C-like instance layout: the C BytesIO and TextIOWrapper/StringIO keep their\n"
    "# state in the object, so __dict__ holds only user attributes (and exists\n"
    "# only once used: _qp_dict_made, for the __getstate__ dict-or-None).\n"
    "# _pyio's class defaults (`__closed = False`, `_buffer = None`, read by\n"
    "# __del__/close before __init__ ran) become a __new__ that sets the slot.\n"
    "def _slots_block(names, init, dict_property=True):\n"
    "    block = ('    __slots__ = %r\\n'\n"
    "             '\\n'\n"
    "             '    def __new__(cls, *args, **kwargs):\\n'\n"
    "             '        self = super().__new__(cls)\\n'\n"
    "             '%s'\n"
    "             '        return self\\n' % (names, init))\n"
    "    if dict_property:\n"
    "        block += ('\\n'\n"
    "                  '    @property\\n'\n"
    "                  '    def __dict__(self):\\n'\n"
    "                  '        self._qp_dict_made = True\\n'\n"
    "                  '        return _qp_real_dict.__get__(self)\\n'\n"
    "                  '\\n'\n"
    "                  '    @__dict__.setter\\n'\n"
    "                  '    def __dict__(self, value):\\n'\n"
    "                  '        _qp_real_dict.__set__(self, value)\\n'\n"
    "                  '        self._qp_dict_made = True\\n')\n"
    "    return block\n"
    "\n"
    "\n"
    "# Replaces _pyio's StringIO: Modules/_io/stringio.c (initial value, newline\n"
    "# translation on write, character positions, argument and error messages).\n"
    "_IO_STRINGIO = r'''\n"
    "class StringIO(TextIOBase):\n"
    "    \"\"\"Text I/O implementation using an in-memory buffer.\n"
    "\n"
    "    The initial_value argument sets the value of object.  The newline\n"
    "    argument is like the one of TextIOWrapper's constructor.\"\"\"\n"
    "\n"
    "    # The state (_QPStringIOState) is in a slot whose descriptor is removed\n"
    "    # from the class below: the C object keeps it out of reach.\n"
    "    __slots__ = ('_qp_sio',)\n"
    "\n"
    "    def __new__(cls, *args, **kwargs):\n"
    "        self = super().__new__(cls)\n"
    "        _qp_sio_set(self, _QPStringIOState())\n"
    "        return self\n"
    "\n"
    "    @property\n"
    "    def __dict__(self):\n"
    "        self._qp_dict_made = True\n"
    "        return _qp_real_dict.__get__(self)\n"
    "\n"
    "    @__dict__.setter\n"
    "    def __dict__(self, value):\n"
    "        _qp_real_dict.__set__(self, value)\n"
    "        self._qp_dict_made = True\n"
    "\n"
    "    def __init__(self, *args, **kwargs):\n"
    "        \"\"\"Initialize self.  See help(type(self)) for accurate signature.\"\"\"\n"
    "        # _PyArg_UnpackKeywords for (initial_value='', newline='\\n')\n"
    "        nargs = len(args)\n"
    "        if nargs + len(kwargs) > 2:\n"
    "            raise TypeError('StringIO() takes at most 2 %sarguments (%d given)'\n"
    "                            % ('keyword ' if nargs == 0 else '', nargs + len(kwargs)))\n"
    "        value = args[0] if nargs > 0 else None\n"
    "        newline = args[1] if nargs > 1 else '\\n'\n"
    "        if kwargs:\n"
    "            for i, key in enumerate(('initial_value', 'newline')):\n"
    "                if i < nargs and key in kwargs:\n"
    "                    raise TypeError(\"argument for StringIO() given by name ('%s') \"\n"
    "                                    \"and position (%d)\" % (key, i + 1))\n"
    "            for key in kwargs:\n"
    "                if key == 'initial_value':\n"
    "                    value = kwargs[key]\n"
    "                elif key == 'newline':\n"
    "                    newline = kwargs[key]\n"
    "                else:\n"
    "                    raise TypeError(\"StringIO() got an unexpected keyword argument '%s'\"\n"
    "                                    % (key,))\n"
    "        _qp_sio_init(self, _qp_sio_get(self), value, newline)\n"
    "\n"
    "    def getvalue(self, *args, **kwargs):\n"
    "        \"\"\"Retrieve the entire contents of the object.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('getvalue', args, kwargs)\n"
    "        return _qp_sio_getvalue(_qp_sio_ready(self))\n"
    "\n"
    "    def tell(self, *args, **kwargs):\n"
    "        \"\"\"Tell the current file position.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('tell', args, kwargs)\n"
    "        return _qp_sio_ready(self).pos\n"
    "\n"
    "    def read(self, *args, **kwargs):\n"
    "        \"\"\"Read at most size characters, returned as a string.\n"
    "\n"
    "        If the argument is negative or omitted, read until EOF\n"
    "        is reached. Return an empty string at EOF.\"\"\"\n"
    "        size = _qp_sio_size_arg('read', args, kwargs)\n"
    "        st = _qp_sio_ready(self)\n"
    "        n = st.size - st.pos\n"
    "        if size < 0 or size > n:\n"
    "            size = n if n > 0 else 0\n"
    "        if st.chunks is not None and st.pos == 0 and size == n:\n"
    "            # optimization for seek(0); read()\n"
    "            st.pos = st.size\n"
    "            return _qp_sio_getvalue(st)\n"
    "        _qp_sio_realize(st)\n"
    "        pos = st.pos\n"
    "        st.pos = pos + size\n"
    "        return st.value[pos:pos + size]\n"
    "\n"
    "    def readline(self, *args, **kwargs):\n"
    "        \"\"\"Read until newline or EOF.\n"
    "\n"
    "        Returns an empty string if EOF is hit immediately.\"\"\"\n"
    "        size = _qp_sio_size_arg('readline', args, kwargs)\n"
    "        return _qp_sio_readline(_qp_sio_ready(self), size)\n"
    "\n"
    "    def __next__(self):\n"
    "        st = _qp_sio_ready(self)\n"
    "        if type(self) is StringIO:\n"
    "            line = _qp_sio_readline(st, -1)\n"
    "        else:\n"
    "            line = self.readline()\n"
    "            if not isinstance(line, str):\n"
    "                raise OSError(\"readline() should have returned a str object, not '%s'\"\n"
    "                              % (type(line).__name__,))\n"
    "        if not line:\n"
    "            raise StopIteration\n"
    "        return line\n"
    "\n"
    "    def truncate(self, *args, **kwargs):\n"
    "        \"\"\"Truncate size to pos.\n"
    "\n"
    "        The pos argument defaults to the current file position, as\n"
    "        returned by tell().  The current file position is unchanged.\n"
    "        Returns the new absolute position.\"\"\"\n"
    "        if kwargs:\n"
    "            _qp_sio_nokw('truncate')\n"
    "        if len(args) > 1:\n"
    "            raise TypeError('truncate expected at most 1 argument, got %d' % len(args))\n"
    "        st = _qp_sio_ready(self)\n"
    "        pos = args[0] if args else None\n"
    "        if pos is None:\n"
    "            size = st.pos\n"
    "        else:\n"
    "            size = _qp_sio_index(pos, 'long')\n"
    "            if size < 0:\n"
    "                raise ValueError('negative pos value %d' % (size,))\n"
    "        if size < st.size:\n"
    "            _qp_sio_realize(st)\n"
    "            st.value = st.value[:size]\n"
    "            st.size = size\n"
    "        return size\n"
    "\n"
    "    def seek(self, *args, **kwargs):\n"
    "        \"\"\"Change stream position.\n"
    "\n"
    "        Seek to character offset pos relative to position indicated by\n"
    "        whence:\n"
    "            0  Start of stream (the default).  pos should be >= 0;\n"
    "            1  Current position - pos must be 0;\n"
    "            2  End of stream - pos must be 0.\n"
    "        Returns the new absolute position.\"\"\"\n"
    "        if kwargs:\n"
    "            _qp_sio_nokw('seek')\n"
    "        nargs = len(args)\n"
    "        if nargs < 1:\n"
    "            raise TypeError('seek expected at least 1 argument, got %d' % nargs)\n"
    "        if nargs > 2:\n"
    "            raise TypeError('seek expected at most 2 arguments, got %d' % nargs)\n"
    "        pos = _qp_sio_index(args[0], 'ssize_t')\n"
    "        whence = _qp_sio_index(args[1], 'int') if nargs > 1 else 0\n"
    "        st = _qp_sio_ready(self)\n"
    "        if whence != 0 and whence != 1 and whence != 2:\n"
    "            raise ValueError('Invalid whence (%d, should be 0, 1 or 2)' % (whence,))\n"
    "        if pos < 0 and whence == 0:\n"
    "            raise ValueError('Negative seek position %d' % (pos,))\n"
    "        if whence != 0 and pos != 0:\n"
    "            raise OSError(\"Can't do nonzero cur-relative seeks\")\n"
    "        if whence == 1:\n"
    "            pos = st.pos\n"
    "        elif whence == 2:\n"
    "            pos = st.size\n"
    "        st.pos = pos\n"
    "        return pos\n"
    "\n"
    "    def write(self, *args, **kwargs):\n"
    "        \"\"\"Write string to file.\n"
    "\n"
    "        Returns the number of characters written, which is always equal to\n"
    "        the length of the string.\"\"\"\n"
    "        if kwargs:\n"
    "            _qp_sio_nokw('write')\n"
    "        if len(args) != 1:\n"
    "            raise TypeError('StringIO.write() takes exactly one argument (%d given)'\n"
    "                            % len(args))\n"
    "        s = args[0]\n"
    "        st = _qp_sio_get(self)\n"
    "        if not st.ok:\n"
    "            raise ValueError('I/O operation on uninitialized object')\n"
    "        if not isinstance(s, str):\n"
    "            raise TypeError(\"string argument expected, got '%s'\" % (type(s).__name__,))\n"
    "        if st.closed:\n"
    "            raise ValueError('I/O operation on closed file')\n"
    "        n = len(s)\n"
    "        if n:\n"
    "            _qp_sio_write(st, s)\n"
    "        return n\n"
    "\n"
    "    def close(self, *args, **kwargs):\n"
    "        \"\"\"Close the IO object.\n"
    "\n"
    "        Attempting any further operation after the object is closed\n"
    "        will raise a ValueError.\n"
    "\n"
    "        This method has no effect if the file is already closed.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('close', args, kwargs)\n"
    "        st = _qp_sio_get(self)\n"
    "        st.closed = True\n"
    "        st.chunks = None\n"
    "        st.value = ''\n"
    "        st.readnl = st.writenl = st.decoder = None\n"
    "\n"
    "    def readable(self, *args, **kwargs):\n"
    "        \"\"\"Returns True if the IO object can be read.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('readable', args, kwargs)\n"
    "        _qp_sio_ready(self)\n"
    "        return True\n"
    "\n"
    "    def writable(self, *args, **kwargs):\n"
    "        \"\"\"Returns True if the IO object can be written.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('writable', args, kwargs)\n"
    "        _qp_sio_ready(self)\n"
    "        return True\n"
    "\n"
    "    def seekable(self, *args, **kwargs):\n"
    "        \"\"\"Returns True if the IO object can be seeked.\"\"\"\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('seekable', args, kwargs)\n"
    "        _qp_sio_ready(self)\n"
    "        return True\n"
    "\n"
    "    def __getstate__(self, *args, **kwargs):\n"
    "        if args or kwargs:\n"
    "            _qp_sio_noargs('__getstate__', args, kwargs)\n"
    "        st = _qp_sio_ready(self)\n"
    "        return (_qp_sio_getvalue(st), st.readnl, st.pos, _qp_state_dict(self))\n"
    "\n"
    "    def __setstate__(self, *args, **kwargs):\n"
    "        if kwargs:\n"
    "            _qp_sio_nokw('__setstate__')\n"
    "        if len(args) != 1:\n"
    "            raise TypeError('StringIO.__setstate__() takes exactly one argument '\n"
    "                            '(%d given)' % len(args))\n"
    "        state = args[0]\n"
    "        st = _qp_sio_get(self)\n"
    "        if st.closed:\n"
    "            raise ValueError('I/O operation on closed file')\n"
    "        if not isinstance(state, tuple) or len(state) < 4:\n"
    "            raise TypeError('%s.__setstate__ argument should be 4-tuple, got %s'\n"
    "                            % (_qp_tp_name(self), _qp_tp_name(state)))\n"
    "        _qp_sio_init(self, st, state[0], state[1])\n"
    "        # the state holds the already translated text: store it unchanged\n"
    "        value = state[0]\n"
    "        st.chunks = None\n"
    "        st.value = str.__str__(value) if isinstance(value, str) else ''\n"
    "        st.size = len(st.value)\n"
    "        pos = state[2]\n"
    "        if not isinstance(pos, int):\n"
    "            raise TypeError('third item of state must be an integer, got %s'\n"
    "                            % (type(pos).__name__,))\n"
    "        if not -0x8000000000000000 <= pos <= 0x7fffffffffffffff:\n"
    "            raise OverflowError('Python int too large to convert to C ssize_t')\n"
    "        if pos < 0:\n"
    "            raise ValueError('position value cannot be negative')\n"
    "        st.pos = int(pos)\n"
    "        d = state[3]\n"
    "        if d is not None:\n"
    "            if not isinstance(d, dict):\n"
    "                raise TypeError('fourth item of state should be a dict, got a %s'\n"
    "                                % (type(d).__name__,))\n"
    "            _qp_real_dict.__get__(self).update(d)\n"
    "            self._qp_dict_made = True\n"
    "\n"
    "    @property\n"
    "    def closed(self):\n"
    "        st = _qp_sio_get(self)\n"
    "        if not st.ok:\n"
    "            raise ValueError('I/O operation on uninitialized object')\n"
    "        return st.closed\n"
    "\n"
    "    @property\n"
    "    def line_buffering(self):\n"
    "        _qp_sio_ready(self)\n"
    "        return False\n"
    "\n"
    "    @property\n"
    "    def newlines(self):\n"
    "        st = _qp_sio_ready(self)\n"
    "        if st.decoder is None:\n"
    "            return None\n"
    "        return st.decoder.newlines\n"
    "\n"
    "\n"
    "class _QPStringIOState:\n"
    "    # the stringio struct: the text is in `chunks` while it is only appended\n"
    "    # to (STATE_ACCUMULATING), else in `value` (STATE_REALIZED); positions\n"
    "    # and sizes count characters\n"
    "    __slots__ = ('ok', 'closed', 'chunks', 'value', 'size', 'pos', 'readnl', 'writenl',\n"
    "                 'readuniversal', 'readtranslate', 'decoder')\n"
    "\n"
    "    def __init__(self):\n"
    "        self.ok = False\n"
    "        self.closed = False\n"
    "        self.chunks = None\n"
    "        self.value = ''\n"
    "        self.size = self.pos = 0\n"
    "        self.readnl = self.writenl = self.decoder = None\n"
    "        self.readuniversal = self.readtranslate = False\n"
    "\n"
    "\n"
    "_qp_sio_get = StringIO.__dict__['_qp_sio'].__get__\n"
    "_qp_sio_set = StringIO.__dict__['_qp_sio'].__set__\n"
    "# the C type has neither the state slot nor the class-body bookkeeping\n"
    "for _qp_name in ('_qp_sio', '__slots__', '__firstlineno__', '__static_attributes__'):\n"
    "    if _qp_name in StringIO.__dict__:\n"
    "        delattr(StringIO, _qp_name)\n"
    "del _qp_name\n"
    "\n"
    "\n"
    "def _qp_sio_ready(self):\n"
    "    # CHECK_INITIALIZED + CHECK_CLOSED\n"
    "    st = _qp_sio_get(self)\n"
    "    if not st.ok:\n"
    "        raise ValueError('I/O operation on uninitialized object')\n"
    "    if st.closed:\n"
    "        raise ValueError('I/O operation on closed file')\n"
    "    return st\n"
    "\n"
    "\n"
    "def _qp_sio_init(self, st, value, newline):\n"
    "    # _io_StringIO___init___impl\n"
    "    if newline is None:\n"
    "        nl = None\n"
    "    else:\n"
    "        if not isinstance(newline, str):\n"
    "            raise TypeError('newline must be str or None, not %s' % (type(newline).__name__,))\n"
    "        nl = newline.partition('\\0')[0]     # the C string ends at a NUL\n"
    "        if nl not in ('', '\\n', '\\r', '\\r\\n'):\n"
    "            raise ValueError('illegal newline value: %r' % (newline,))\n"
    "    if value is not None and not isinstance(value, str):\n"
    "        raise TypeError('initial_value must be str or None, not %s' % (type(value).__name__,))\n"
    "    st.ok = False\n"
    "    st.readnl = nl\n"
    "    st.readuniversal = nl is None or nl == ''\n"
    "    st.readtranslate = nl is None\n"
    "    st.writenl = nl if nl is not None and nl[:1] == '\\r' else None\n"
    "    st.decoder = (IncrementalNewlineDecoder(None, st.readtranslate)\n"
    "                  if st.readuniversal else None)\n"
    "    st.size = st.pos = 0\n"
    "    if value:\n"
    "        st.chunks = None\n"
    "        st.value = ''\n"
    "        _qp_sio_write(st, value)\n"
    "    else:\n"
    "        st.chunks = []\n"
    "        st.value = ''\n"
    "    st.pos = 0\n"
    "    st.closed = False\n"
    "    st.ok = True\n"
    "\n"
    "\n"
    "def _qp_sio_realize(st):\n"
    "    if st.chunks is not None:\n"
    "        st.value = ''.join(st.chunks)\n"
    "        st.chunks = None\n"
    "\n"
    "\n"
    "def _qp_sio_getvalue(st):\n"
    "    # make_intermediate while accumulating\n"
    "    chunks = st.chunks\n"
    "    if chunks is None:\n"
    "        return st.value\n"
    "    if len(chunks) > 1:\n"
    "        chunks[:] = [''.join(chunks)]\n"
    "    return chunks[0] if chunks else ''\n"
    "\n"
    "\n"
    "def _qp_sio_write(st, s):\n"
    "    # write_str: newline translation of each write on its own (final)\n"
    "    if st.decoder is not None:\n"
    "        s = st.decoder.decode(s, final=True)\n"
    "    if st.writenl is not None:\n"
    "        s = s.replace('\\n', st.writenl)\n"
    "    if type(s) is not str:\n"
    "        s = str.__str__(s)\n"
    "    n = len(s)\n"
    "    pos = st.pos\n"
    "    if st.chunks is not None:\n"
    "        if st.size == pos:\n"
    "            st.chunks.append(s)\n"
    "            st.pos = st.size = pos + n\n"
    "            return\n"
    "        _qp_sio_realize(st)\n"
    "    value = st.value\n"
    "    if pos > st.size:\n"
    "        # overseek: pad with NULs up to the position\n"
    "        value += '\\0' * (pos - st.size)\n"
    "    st.value = value = value[:pos] + s + value[pos + n:]\n"
    "    st.pos = pos + n\n"
    "    st.size = len(value)\n"
    "\n"
    "\n"
    "def _qp_sio_readline(st, limit):\n"
    "    # _stringio_readline / _PyIO_find_line_ending\n"
    "    _qp_sio_realize(st)\n"
    "    pos = st.pos\n"
    "    if pos >= st.size:\n"
    "        return ''\n"
    "    avail = st.size - pos\n"
    "    if limit < 0 or limit > avail:\n"
    "        limit = avail\n"
    "    end = pos + limit\n"
    "    value = st.value\n"
    "    if st.readtranslate:\n"
    "        i = value.find('\\n', pos, end)\n"
    "        n = limit if i < 0 else i + 1 - pos\n"
    "    elif st.readuniversal:\n"
    "        i = value.find('\\n', pos, end)\n"
    "        j = value.find('\\r', pos, i if i >= 0 else end)\n"
    "        if j >= 0:\n"
    "            n = (j + 2 if j + 1 < end and value[j + 1] == '\\n' else j + 1) - pos\n"
    "        elif i >= 0:\n"
    "            n = i + 1 - pos\n"
    "        else:\n"
    "            n = limit\n"
    "    else:\n"
    "        i = value.find(st.readnl, pos, end)\n"
    "        n = limit if i < 0 else i + len(st.readnl) - pos\n"
    "    st.pos = pos + n\n"
    "    return value[pos:pos + n]\n"
    "\n"
    "\n"
    "def _qp_sio_nokw(name):\n"
    "    raise TypeError('StringIO.%s() takes no keyword arguments' % (name,))\n"
    "\n"
    "\n"
    "def _qp_sio_noargs(name, args, kwargs):\n"
    "    if kwargs:\n"
    "        _qp_sio_nokw(name)\n"
    "    raise TypeError('StringIO.%s() takes no arguments (%d given)' % (name, len(args)))\n"
    "\n"
    "\n"
    "_QP_SIO_C_RANGES = {\n"
    "    'int': (-0x80000000, 0x7fffffff, 'Python int too large to convert to C int'),\n"
    "    'long': (-0x8000000000000000, 0x7fffffffffffffff,\n"
    "             'Python int too large to convert to C long'),\n"
    "    'ssize_t': (-0x8000000000000000, 0x7fffffffffffffff,\n"
    "                'Python int too large to convert to C ssize_t'),\n"
    "}\n"
    "\n"
    "\n"
    "def _qp_sio_index(v, ctype):\n"
    "    # PyNumber_Index, then the C integer conversion\n"
    "    if type(v) is not int:\n"
    "        try:\n"
    "            index = type(v).__index__\n"
    "        except AttributeError:\n"
    "            raise TypeError(\"'%s' object cannot be interpreted as an integer\"\n"
    "                            % (type(v).__name__,)) from None\n"
    "        v = index(v)\n"
    "    lo, hi, msg = _QP_SIO_C_RANGES[ctype]\n"
    "    if not lo <= v <= hi:\n"
    "        raise OverflowError(msg)\n"
    "    return int(v)\n"
    "\n"
    "\n"
    "def _qp_sio_size_arg(name, args, kwargs):\n"
    "    # Py_ssize_t(accept={int, NoneType}) = -1 (_Py_convert_optional_to_ssize_t)\n"
    "    if kwargs:\n"
    "        _qp_sio_nokw(name)\n"
    "    if not args:\n"
    "        return -1\n"
    "    if len(args) > 1:\n"
    "        raise TypeError('%s expected at most 1 argument, got %d' % (name, len(args)))\n"
    "    v = args[0]\n"
    "    if v is None:\n"
    "        return -1\n"
    "    if type(v) is not int:\n"
    "        try:\n"
    "            index = type(v).__index__\n"
    "        except AttributeError:\n"
    "            raise TypeError(\"argument should be integer or None, not '%s'\"\n"
    "                            % (type(v).__name__,)) from None\n"
    "        v = index(v)\n"
    "    if not -0x8000000000000000 <= v <= 0x7fffffffffffffff:\n"
    "        raise OverflowError(\"cannot fit 'int' into an index-sized integer\")\n"
    "    return int(v)\n"
    "\n"
    "\n"
    "'''\n"
    "\n"
    "\n"
    "_IO_REWRITES += (\n"
    "    # _pyio's StringIO is a TextIOWrapper over a UTF-8 BytesIO; the C one\n"
    "    # (what CPython users see) is a _TextIOBase over a character buffer\n"
    "    ('StringIO', None, _IO_STRINGIO, 1),\n"
    "    ('IOBase', '    __closed = False\\n',\n"
    "     _slots_block(('__dict__', '__weakref__', '_IOBase__closed', '_qp_dict_made'),\n"
    "                  # C state, not attributes: a __setattr__ override does not see it\n"
    "                  '        object.__setattr__(self, \"_IOBase__closed\", False)\\n'\n"
    "                  '        object.__setattr__(self, \"_qp_dict_made\", False)\\n', False), 1),\n"
    "    # C BytesIO.__init__ refuses to drop an exported buffer and reopens a\n"
    "    # closed object\n"
    "    ('BytesIO', '        # Use to keep self._buffer and self._pos consistent.\\n',\n"
    "     '        _qp_bytesio_check_exports(self)\\n'\n"
    "     '        self._IOBase__closed = False\\n', 1),\n"
    "    ('BytesIO', '    _buffer = None\\n',\n"
    "     _slots_block(('_buffer', '_pos', '_lock'),\n"
    "                  '        self._lock = Lock()\\n'\n"
    "                  '        self._buffer = bytearray()\\n'\n"
    "                  '        self._pos = 0\\n'), 1),\n"
    "    ('TextIOWrapper', '    _buffer = None\\n',\n"
    "     _slots_block(('_b2cratio', '_buffer', '_decoded_chars', '_decoded_chars_used',\n"
    "                   '_decoder', '_encoder', '_encoding', '_errors', '_has_read1',\n"
    "                   '_line_buffering', '_readnl', '_readtranslate', '_readuniversal',\n"
    "                   '_seekable', '_snapshot', '_telling', '_write_through', '_writenl',\n"
    "                   '_writetranslate', '_qp_pending', '_qp_pending_count',\n"
    "                   '_qp_chunk_size', '_qp_writable'),\n"
    "                  '        self._buffer = None\\n'\n"
    "                  '        self._qp_pending = None\\n'\n"
    "                  '        self._qp_pending_count = 0\\n'\n"
    "                  '        self._qp_chunk_size = 8192\\n'\n"
    "                  '        self._qp_writable = False\\n'), 1),\n"
    ")\n"
    "\n"
    "# Modules/_io/bufferedio.c and textio.c object states and buffering (see\n"
    "# the helpers appended by _IO_STATE).\n"
    "_IO_REWRITES += (\n"
    "    # CHECK_INITIALIZED / CHECK_ATTACHED: a failed __init__ or detach()\n"
    "    # leaves a raw stream / buffer whose every use raises ValueError\n"
    "    ('_BufferedIOMixin', r'''    def raw(self):\n"
    "        return self._raw\n"
    "''', r'''    def raw(self):\n"
    "        raw = self._raw\n"
    "        if type(raw) is _QPUnusable:\n"
    "            raw._qp_raise\n"
    "        return raw\n"
    "''', 1),\n"
    "    ('_BufferedIOMixin', r'''        raw = self._raw\n"
    "        self._raw = None\n"
    "''', r'''        raw = self._raw\n"
    "        self._raw = _qp_raw_detached\n"
    "''', 1),\n"
    "    ('_BufferedIOMixin', r'''        except AttributeError:\n"
    "            return \"<{}.{}>\".format(modname, clsname)\n"
    "''', r'''        except (AttributeError, ValueError):\n"
    "            return \"<{}.{}>\".format(modname, clsname)\n"
    "''', 1),\n"
    "    ('BufferedReader', r'''        if not raw.readable():\n"
    "''', r'''        self._raw = _qp_uninitialized\n"
    "        if not raw.readable():\n"
    "''', 1),\n"
    "    ('BufferedWriter', r'''        if not raw.writable():\n"
    "''', r'''        self._raw = _qp_uninitialized\n"
    "        if not raw.writable():\n"
    "''', 1),\n"
    "    ('BufferedReader', r'''        if buffer_size <= 0:\n"
    "''', r'''        if buffer_size <= 0:\n"
    "            self._raw = _qp_uninitialized\n"
    "''', 1),\n"
    "    ('BufferedWriter', r'''        if buffer_size <= 0:\n"
    "''', r'''        if buffer_size <= 0:\n"
    "            self._raw = _qp_uninitialized\n"
    "''', 1),\n"
    "    ('BufferedReader', r'''        self._read_lock = Lock()\n"
    "''', r'''        self._read_lock = _QPBufferedLock(self)\n"
    "''', 1),\n"
    "    ('BufferedWriter', r'''        self._write_lock = Lock()\n"
    "''', r'''        self._write_lock = _QPBufferedLock(self)\n"
    "''', 1),\n"
    "    # the C reader fills its buffer with raw.readinto() and checks the result\n"
    "    ('BufferedReader', r'''            chunk = self.raw.read(wanted)\n"
    "''', r'''            chunk = _qp_raw_read(self.raw, wanted)\n"
    "''', 1),\n"
    "    ('BufferedReader', r'''            current = self.raw.read(to_read)\n"
    "''', r'''            current = _qp_raw_read(self.raw, to_read)\n"
    "''', 1),\n"
    "    ('BufferedReader', r'''                    n = self.raw.readinto(buf[written:])\n"
    "''', r'''                    n = _qp_raw_readinto(self.raw, buf[written:])\n"
    "''', 1),\n"
    "    # read1: the buffered bytes, else one raw read of up to size bytes\n"
    "    ('BufferedReader', r'''        with self._read_lock:\n"
    "            self._peek_unlocked(1)\n"
    "            return self._read_unlocked(\n"
    "                min(size, len(self._read_buf) - self._read_pos))\n"
    "''', r'''        have = len(self._read_buf) - self._read_pos\n"
    "        if have > 0:\n"
    "            with self._read_lock:\n"
    "                return self._read_unlocked(min(size, have))\n"
    "        buf = bytearray(size)\n"
    "        with self._read_lock:\n"
    "            self._reset_read_buf()\n"
    "            with memoryview(buf) as view:\n"
    "                n = _qp_raw_readinto(self.raw, view)\n"
    "        if not n:\n"
    "            return b\"\"\n"
    "        del buf[n:]\n"
    "        return bytes(buf)\n"
    "''', 1),\n"
    "    # C BytesIO.read1 does not dispatch to an overridden read()\n"
    "    ('BytesIO', r'''        return self.read(size)\n"
    "''', r'''        return BytesIO.read(self, size)\n"
    "''', 1),\n"
    "    ('FileIO', r'''    _fd = -1\n"
    "''', r'''    __slots__ = ('_qp_finalizing',)\n"
    "    _fd = -1\n"
    "''', 1),\n"
    "    # fileio_dealloc_warn uses the C warnings module (also at shutdown)\n"
    "    ('FileIO', r'''            import warnings\n"
    "            warnings.warn(''', r'''            self._qp_warn(''', 1),\n"
    "    ('TextIOWrapper', r'''    _CHUNK_SIZE = 2048\n"
    "''', '', 1),\n"
    "    # Argument Clinic str converters, then self->ok = 0 until __init__ ends\n"
    "    ('TextIOWrapper', r'''        self._check_newline(newline)\n"
    "        encoding = text_encoding(encoding)\n"
    "''', r'''        if encoding is not None:\n"
    "            if not isinstance(encoding, str):\n"
    "                raise TypeError(\"TextIOWrapper() argument 'encoding' must be \"\n"
    "                                \"str or None, not %s\" % type(encoding).__name__)\n"
    "            _qp_cstring(encoding)\n"
    "        if newline is not None:\n"
    "            if not isinstance(newline, str):\n"
    "                raise TypeError(\"TextIOWrapper() argument 'newline' must be \"\n"
    "                                \"str or None, not %s\" % type(newline).__name__)\n"
    "            _qp_cstring(newline)\n"
    "        self._buffer = _qp_uninitialized\n"
    "        self._qp_pending = None\n"
    "        self._qp_pending_count = 0\n"
    "        if errors is not None:\n"
    "            if not isinstance(errors, str):\n"
    "                raise TypeError(\"TextIOWrapper() argument 'errors' must be \"\n"
    "                                \"str or None, not %s\" % type(errors).__name__)\n"
    "            _qp_cstring(errors)\n"
    "        self._check_newline(newline)\n"
    "        encoding = text_encoding(encoding)\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''    def buffer(self):\n"
    "        return self._buffer\n"
    "''', r'''    def buffer(self):\n"
    "        buffer = self._buffer\n"
    "        if type(buffer) is _QPUnusable:\n"
    "            buffer._qp_raise\n"
    "        return buffer\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        buffer = self._buffer\n"
    "        self._buffer = None\n"
    "''', r'''        buffer = self._buffer\n"
    "        self._buffer = _qp_buffer_detached\n"
    "''', 1),\n"
    "    # gh-142594: `closed` may detach the wrapper\n"
    "    ('TextIOWrapper', r'''        if self.buffer is not None and not self.closed:\n"
    "''', r'''        if (self.buffer is not None and not self.closed and\n"
    "                type(self._buffer) is not _QPUnusable):\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''            name = self.name\n"
    "        except AttributeError:\n"
    "''', r'''            name = self.name\n"
    "        except (AttributeError, ValueError):\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        self._write_through = write_through\n"
    "''', r'''        self._write_through = write_through\n"
    "        self._qp_writable = self.buffer.writable()\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''            if encoding == \"locale\":\n"
    "                encoding = self._get_locale_encoding()\n"
    "\n"
    "        if newline is Ellipsis:\n"
    "''', r'''            if encoding == \"locale\":\n"
    "                encoding = self._get_locale_encoding()\n"
    "            else:\n"
    "                _qp_lookup_text_encoding(encoding)\n"
    "        errors.encode('utf-8')\n"
    "\n"
    "        if newline is Ellipsis:\n"
    "''', 1),\n"
    "    # pending_bytes: written text is kept until _CHUNK_SIZE bytes, a flush,\n"
    "    # a read or a tell\n"
    "    ('TextIOWrapper', r'''        self.buffer.flush()\n"
    "        self._telling = self._seekable\n"
    "''', r'''        self._telling = self._seekable\n"
    "        self._qp_writeflush()\n"
    "        self.buffer.flush()\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        encoder = self._encoder or self._get_encoder()\n"
    "        # XXX What if we were just reading?\n"
    "''', r'''        if not self._qp_writable:\n"
    "            raise UnsupportedOperation(\"not writable\")\n"
    "        encoder = self._encoder or self._get_encoder()\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        b = encoder.encode(s)\n"
    "        self.buffer.write(b)\n"
    "''', r'''        b = encoder.encode(s)\n"
    "        if not isinstance(b, bytes):\n"
    "            raise TypeError(\"encoder should return a bytes object, not '%s'\"\n"
    "                            % type(b).__name__)\n"
    "        if len(b) >= self._qp_chunk_size:\n"
    "            while self._qp_pending is not None:\n"
    "                self._qp_writeflush()\n"
    "        if b:\n"
    "            if self._qp_pending is None:\n"
    "                self._qp_pending = [b]\n"
    "            else:\n"
    "                self._qp_pending.append(b)\n"
    "            self._qp_pending_count += len(b)\n"
    "        if (self._qp_pending_count >= self._qp_chunk_size or self._write_through or\n"
    "                (self._line_buffering and (haslf or \"\\r\" in s))):\n"
    "            self._qp_writeflush()\n"
    "''', 1),\n"
    "    # convert_optional_bool: PyLong_AsLong(value) != 0\n"
    "    ('TextIOWrapper', r'''            line_buffering = self.line_buffering\n"
    "        if write_through is None:\n"
    "            write_through = self.write_through\n"
    "''', r'''            line_buffering = self.line_buffering\n"
    "        else:\n"
    "            line_buffering = _qp_long_bool(line_buffering)\n"
    "        if write_through is None:\n"
    "            write_through = self.write_through\n"
    "        else:\n"
    "            write_through = _qp_long_bool(write_through)\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        decoder = self._decoder or self._get_decoder()\n"
    "        if size < 0:\n"
    "''', r'''        self._qp_writeflush()\n"
    "        decoder = self._decoder or self._get_decoder()\n"
    "        if size < 0:\n"
    "''', 1),\n"
    "    ('TextIOWrapper', r'''        # Grab all the decoded text (we will rewind any extra bits later).\n"
    "''', r'''        self._qp_writeflush()\n"
    "        # Grab all the decoded text (we will rewind any extra bits later).\n"
    "''', 1),\n"
    ")\n"
    "\n"
    "# Appended to the _pyio source: the C BytesIO / StringIO pickle state\n"
    "# (Modules/_io/bytesio.c, stringio.c __getstate__ / __setstate__).\n"
    "_IO_STATE = '''\n"
    "\n"
    "_qp_real_dict = IOBase.__dict__['__dict__']\n"
    "\n"
    "\n"
    "def _qp_state_dict(self):\n"
    "    d = _qp_real_dict.__get__(self)\n"
    "    return d.copy() if d or self._qp_dict_made else None\n"
    "\n"
    "\n"
    "def _qp_tp_name(obj):\n"
    "    t = type(obj)\n"
    "    return '_io.' + t.__name__ if t.__module__ == '_io' else t.__name__\n"
    "\n"
    "\n"
    "def _qp_method(cls, func):\n"
    "    func.__qualname__ = cls.__name__ + '.' + func.__name__\n"
    "    func.__module__ = cls.__module__\n"
    "    setattr(cls, func.__name__, func)\n"
    "\n"
    "\n"
    "def __getstate__(self):\n"
    "    value = BytesIO.getvalue(self)\n"
    "    return (value, self._pos, _qp_state_dict(self))\n"
    "\n"
    "\n"
    "def __setstate__(self, *args, **kwargs):\n"
    "    if kwargs:\n"
    "        raise TypeError('BytesIO.__setstate__() takes no keyword arguments')\n"
    "    if len(args) != 1:\n"
    "        raise TypeError('BytesIO.__setstate__() takes exactly one argument '\n"
    "                        f'({len(args)} given)')\n"
    "    state = args[0]\n"
    "    if not isinstance(state, tuple) or len(state) < 3:\n"
    "        raise TypeError(f'{_qp_tp_name(self)}.__setstate__ argument should be '\n"
    "                        f'3-tuple, got {_qp_tp_name(state)}')\n"
    "    self._buffer[:] = b''\n"
    "    self._pos = 0\n"
    "    BytesIO.write(self, state[0])\n"
    "    pos = state[1]\n"
    "    if not isinstance(pos, int):\n"
    "        raise TypeError('second item of state must be an integer, not '\n"
    "                        f'{type(pos).__name__}')\n"
    "    if not -0x8000000000000000 <= pos <= 0x7fffffffffffffff:\n"
    "        raise OverflowError('Python int too large to convert to C ssize_t')\n"
    "    if pos < 0:\n"
    "        raise ValueError('position value cannot be negative')\n"
    "    self._pos = pos\n"
    "    d = state[2]\n"
    "    if d is not None:\n"
    "        if not isinstance(d, dict):\n"
    "            raise TypeError('third item of state should be a dict, got a '\n"
    "                            f'{type(d).__name__}')\n"
    "        _qp_real_dict.__get__(self).update(d)\n"
    "        self._qp_dict_made = True\n"
    "\n"
    "\n"
    "def _qp_bytesio_check_exports(self):\n"
    "    # CHECK_EXPORTS: a buffer exported by getbuffer() cannot be resized\n"
    "    buf = self._buffer\n"
    "    try:\n"
    "        buf.append(0)\n"
    "    except BufferError:\n"
    "        raise BufferError('Existing exports of data: object cannot be re-sized') from None\n"
    "    del buf[-1]\n"
    "\n"
    "\n"
    "def truncate(self, size=None, /):\n"
    "    \"\"\"Truncate the file to at most size bytes.\n"
    "\n"
    "    Size defaults to the current file position, as returned by tell().\n"
    "    The current file position is unchanged.  Returns the new size.\"\"\"\n"
    "    if self.closed:\n"
    "        raise ValueError('I/O operation on closed file.')\n"
    "    _qp_bytesio_check_exports(self)\n"
    "    if size is None:\n"
    "        size = self._pos\n"
    "    else:\n"
    "        # PyLong_AsLong: __index__ may close or export the stream\n"
    "        if type(size) is not int:\n"
    "            try:\n"
    "                index = type(size).__index__\n"
    "            except AttributeError:\n"
    "                raise TypeError(f\"'{type(size).__name__}' object cannot be \"\n"
    "                                \"interpreted as an integer\") from None\n"
    "            size = index(size)\n"
    "        if not -0x8000000000000000 <= size <= 0x7fffffffffffffff:\n"
    "            raise OverflowError('Python int too large to convert to C long')\n"
    "        size = int(size)\n"
    "        if size < 0:\n"
    "            raise ValueError('negative size value %d' % (size,))\n"
    "        if self.closed:\n"
    "            raise ValueError('I/O operation on closed file.')\n"
    "        _qp_bytesio_check_exports(self)\n"
    "    with self._lock:\n"
    "        if size < len(self._buffer):\n"
    "            del self._buffer[size:]\n"
    "    return size\n"
    "\n"
    "\n"
    "# C BytesIO.readline scans its buffer: IOBase.readline would call a\n"
    "# subclass's peek() (pickletester UnseekableIO raises from it)\n"
    "def readline(self, size=-1, /):\n"
    "    if self.closed:\n"
    "        raise ValueError('I/O operation on closed file.')\n"
    "    if size is None:\n"
    "        size = -1\n"
    "    else:\n"
    "        try:\n"
    "            size_index = size.__index__\n"
    "        except AttributeError:\n"
    "            raise TypeError('argument should be integer or None, not '\n"
    "                            f'{type(size).__name__!r}') from None\n"
    "        size = size_index()\n"
    "    with self._lock:\n"
    "        buf = self._buffer\n"
    "        pos = self._pos\n"
    "        n = len(buf)\n"
    "        if pos >= n:\n"
    "            return b''\n"
    "        end = buf.find(b'\\\\n', pos)\n"
    "        end = n if end < 0 else end + 1\n"
    "        if 0 <= size < end - pos:\n"
    "            end = pos + size\n"
    "        self._pos = end\n"
    "        return bytes(buf[pos:end])\n"
    "\n"
    "\n"
    "import _thread as _qp_thread\n"
    "import _weakref as _qp_weakref\n"
    "import _warnings as _qp_warnings\n"
    "\n"
    "\n"
    "def _qp_index(value):\n"
    "    # PyNumber_Index, through range(): _operator is a C API module, which\n"
    "    # the _io of every interpreter (a second one included) must not import\n"
    "    return range(value).stop\n"
    "\n"
    "\n"
    "class _QPUnusable:\n"
    "    # What a buffered object holds as its raw stream and a TextIOWrapper as\n"
    "    # its buffer while the C object refuses any use: CHECK_INITIALIZED (an\n"
    "    # __init__ that failed) and CHECK_ATTACHED (after detach()).  Any\n"
    "    # attribute access but __class__ (isinstance() of the objects that\n"
    "    # gc.get_objects() lists) raises the ValueError.\n"
    "    __slots__ = ('_qp_msg',)\n"
    "\n"
    "    def __init__(self, msg):\n"
    "        object.__setattr__(self, '_qp_msg', msg)\n"
    "\n"
    "    def __getattribute__(self, name):\n"
    "        if name == '__class__':\n"
    "            return _QPUnusable\n"
    "        raise ValueError(object.__getattribute__(self, '_qp_msg'))\n"
    "\n"
    "\n"
    "_qp_uninitialized = _QPUnusable('I/O operation on uninitialized object')\n"
    "_qp_raw_detached = _QPUnusable('raw stream has been detached')\n"
    "_qp_buffer_detached = _QPUnusable('underlying buffer has been detached')\n"
    "\n"
    "\n"
    "class _QPBufferedLock(_qp_thread.RLock):\n"
    "    # ENTER_BUFFERED (Modules/_io/bufferedio.c): the lock of a buffered\n"
    "    # object; the thread holding it entering again (a signal handler or a\n"
    "    # raw stream method calling back) is a RuntimeError, not a deadlock.\n"
    "    # The owner is the C RLock's and __exit__ is RLock's C release: a\n"
    "    # signal handler runs at Python call boundaries (the post-call poll,\n"
    "    # the entry of a Python __exit__), where an owner field or a Python\n"
    "    # release would still show this thread holding the lock, so the\n"
    "    # handler would block on it or leave it held.  No module globals:\n"
    "    # buffered objects are still flushed at shutdown, after the module\n"
    "    # dicts were cleared.\n"
    "    __slots__ = ('_obj',)\n"
    "\n"
    "    def __init__(self, obj):\n"
    "        self._obj = _qp_weakref.ref(obj)\n"
    "\n"
    "    def __enter__(self):\n"
    "        if self._is_owned():\n"
    "            raise RuntimeError('reentrant call inside %r' % (self._obj(),))\n"
    "        try:\n"
    "            self.acquire()\n"
    "        except BaseException:\n"
    "            # a signal handler raised as acquire() returned\n"
    "            if self._is_owned():\n"
    "                self.release()\n"
    "            raise\n"
    "        return self\n"
    "\n"
    "\n"
    "def _qp_ssize(value, exc):\n"
    "    # PyNumber_AsSsize_t(value, exc)\n"
    "    value = _qp_index(value)\n"
    "    if not -0x8000000000000000 <= value <= 0x7fffffffffffffff:\n"
    "        raise exc(f\"cannot fit '{type(value).__name__}' into an index-sized integer\")\n"
    "    return value\n"
    "\n"
    "\n"
    "def _qp_raw_readinto(raw, buf):\n"
    "    # _bufferedreader_raw_read: raw.readinto() (retried on EINTR) returns\n"
    "    # None (would block) or a length between 0 and len(buf)\n"
    "    while True:\n"
    "        try:\n"
    "            res = raw.readinto(buf)\n"
    "        except InterruptedError:\n"
    "            continue\n"
    "        break\n"
    "    if res is None:\n"
    "        return None\n"
    "    try:\n"
    "        n = _qp_ssize(res, ValueError)\n"
    "    except Exception as exc:\n"
    "        raise OSError('raw readinto() failed') from exc\n"
    "    if not 0 <= n <= len(buf):\n"
    "        raise OSError(f'raw readinto() returned invalid length {n} '\n"
    "                      f'(should have been between 0 and {len(buf)})')\n"
    "    return n\n"
    "\n"
    "\n"
    "def _qp_raw_read(raw, size):\n"
    "    buf = bytearray(size)\n"
    "    with memoryview(buf) as view:\n"
    "        n = _qp_raw_readinto(raw, view)\n"
    "    if n is None:\n"
    "        return None\n"
    "    del buf[n:]\n"
    "    return bytes(buf)\n"
    "\n"
    "\n"
    "def _qp_long_bool(value):\n"
    "    value = _qp_index(value)\n"
    "    if not -0x8000000000000000 <= value <= 0x7fffffffffffffff:\n"
    "        raise OverflowError('Python int too large to convert to C long')\n"
    "    return value != 0\n"
    "\n"
    "\n"
    "def _qp_cstring(s):\n"
    "    # a str argument converted to a C string (PyUnicode_AsUTF8AndSize and\n"
    "    # the embedded NUL check of Argument Clinic's str converter)\n"
    "    s.encode('utf-8')\n"
    "    if chr(0) in s:\n"
    "        raise ValueError('embedded null character')\n"
    "\n"
    "\n"
    "def _qp_lookup_text_encoding(encoding):\n"
    "    # _PyCodec_LookupTextEncoding(PyUnicode_AsUTF8(encoding), ...)\n"
    "    encoding.encode('utf-8')\n"
    "    name = encoding.partition(chr(0))[0]\n"
    "    if not codecs.lookup(name)._is_text_encoding:\n"
    "        raise LookupError(f\"{name!r} is not a text encoding; \"\n"
    "                          \"use codecs.open() to handle arbitrary codecs\")\n"
    "\n"
    "\n"
    "def _qp_writeflush(self):\n"
    "    # _textiowrapper_writeflush: hand the pending encoded text (C keeps up\n"
    "    # to _CHUNK_SIZE bytes of it) to buffer.write()\n"
    "    pending = self._qp_pending\n"
    "    if pending is None:\n"
    "        return\n"
    "    b = pending[0] if len(pending) == 1 else b''.join(pending)\n"
    "    self._qp_pending = None\n"
    "    self._qp_pending_count = 0\n"
    "    while True:\n"
    "        try:\n"
    "            self.buffer.write(b)\n"
    "        except InterruptedError:\n"
    "            continue\n"
    "        break\n"
    "\n"
    "\n"
    "def _CHUNK_SIZE_get(self):\n"
    "    self.buffer\n"
    "    return self._qp_chunk_size\n"
    "\n"
    "\n"
    "def _CHUNK_SIZE_set(self, value):\n"
    "    self.buffer\n"
    "    n = _qp_ssize(value, ValueError)\n"
    "    if n <= 0:\n"
    "        raise ValueError('a strictly positive integer is required')\n"
    "    self._qp_chunk_size = n\n"
    "\n"
    "\n"
    "def _CHUNK_SIZE_del(self):\n"
    "    raise AttributeError(f\"attribute '_CHUNK_SIZE' of '{_qp_tp_name(self)}' \"\n"
    "                         \"objects cannot be deleted\")\n"
    "\n"
    "\n"
    "# methods, not module globals: flushing sys.stdout at shutdown runs after\n"
    "# the module dicts were cleared\n"
    "TextIOWrapper._qp_writeflush = _qp_writeflush\n"
    "FileIO._qp_warn = staticmethod(_qp_warnings.warn)\n"
    "del _qp_writeflush\n"
    "TextIOWrapper._CHUNK_SIZE = property(_CHUNK_SIZE_get, _CHUNK_SIZE_set, _CHUNK_SIZE_del)\n"
    "del _CHUNK_SIZE_get, _CHUNK_SIZE_set, _CHUNK_SIZE_del\n"
    "\n"
    "\n"
    "def _finalizing_get(self):\n"
    "    try:\n"
    "        return self._qp_finalizing\n"
    "    except AttributeError:\n"
    "        return False\n"
    "\n"
    "\n"
    "def _finalizing_set(self, value):\n"
    "    if type(value) is not bool:\n"
    "        raise TypeError('attribute value type must be bool')\n"
    "    self._qp_finalizing = value\n"
    "\n"
    "\n"
    "def _finalizing_del(self):\n"
    "    raise TypeError(\"can't delete numeric/char attribute\")\n"
    "\n"
    "\n"
    "FileIO._finalizing = property(_finalizing_get, _finalizing_set, _finalizing_del)\n"
    "del _finalizing_get, _finalizing_set, _finalizing_del\n"
    "\n"
    "\n"
    "def _qp_cannot_pickle():\n"
    "    # _PyIOBase_cannot_pickle\n"
    "    def __getstate__(self):\n"
    "        raise TypeError(f'cannot pickle {type(self).__name__!r} instances')\n"
    "    return __getstate__\n"
    "\n"
    "\n"
    "def _qp_bytesio_readinto():\n"
    "    # C BytesIO.readinto does not dispatch to an overridden read()\n"
    "    def readinto(self, buffer, /):\n"
    "        with memoryview(buffer) as view, view.cast('B') as b:\n"
    "            data = BytesIO.read(self, len(b))\n"
    "            n = len(data)\n"
    "            b[:n] = data\n"
    "        return n\n"
    "    return readinto\n"
    "\n"
    "\n"
    "_qp_method(BytesIO, _qp_bytesio_readinto())\n"
    "del _qp_bytesio_readinto\n"
    "_qp_method(TextIOWrapper, _qp_cannot_pickle())\n"
    "del _qp_cannot_pickle\n"
    "\n"
    "_qp_method(BytesIO, __getstate__)\n"
    "_qp_method(BytesIO, __setstate__)\n"
    "_qp_method(BytesIO, truncate)\n"
    "_qp_method(BytesIO, readline)\n"
    "del __getstate__, __setstate__, truncate, readline, _qp_method\n"
    "'''\n"
    "\n"
    "\n"
    "def _scope_span(source, name):\n"
    "    start = -1\n"
    "    for kind in ('\\nclass ', '\\ndef '):\n"
    "        head = kind + name + '('\n"
    "        pos = source.find(head)\n"
    "        if pos >= 0:\n"
    "            if start >= 0 or source.find(head, pos + 1) >= 0:\n"
    "                return None\n"
    "            start = pos\n"
    "    if start < 0:\n"
    "        return None\n"
    "    start += 1\n"
    "    end = len(source)\n"
    "    pos = source.find('\\n', start)\n"
    "    while pos >= 0:\n"
    "        pos += 1\n"
    "        if pos < end and source[pos] not in ' \\t\\n#':\n"
    "            end = pos\n"
    "            break\n"
    "        pos = source.find('\\n', pos)\n"
    "    return start, end\n"
    "\n"
    "\n"
    "def _rewrite_messages(source, filename):\n"
    "    for scope, old, new, count in _IO_REWRITES:\n"
    "        span = _scope_span(source, scope)\n"
    "        if span is None:\n"
    "            raise ImportError(f\"cannot load the _io module: {filename}: \"\n"
    "                              f\"class or function {scope} not found exactly once\",\n"
    "                              name='_io')\n"
    "        start, end = span\n"
    "        if old is None:\n"
    "            # the whole class or function is replaced\n"
    "            source = source[:start] + new + source[end:]\n"
    "            continue\n"
    "        body = source[start:end]\n"
    "        found = body.count(old)\n"
    "        if found != count:\n"
    "            raise ImportError(f\"cannot load the _io module: {filename}: \"\n"
    "                              f\"expected {count} occurrence(s) of {old!r} in \"\n"
    "                              f\"{scope}, found {found}\", name='_io')\n"
    "        source = source[:start] + body.replace(old, new) + source[end:]\n"
    "    return source\n"
    "\n"
    "\n"
    "def _load(_io, read_file, mark_native):\n"
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
    "    source = _rewrite_messages(source.decode('utf-8'), filename) + _IO_STATE\n"
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
    "        code = compile(source, filename, 'exec')\n"
    "        mark_native(code)\n"
    "        exec(code, ns)\n"
    "    finally:\n"
    "        if saved is missing:\n"
    "            if modules.get('io') is shim:\n"
    "                del modules['io']\n"
    "        else:\n"
    "            modules['io'] = saved\n"
    "\n"
    "    # the C _io calls the system functions directly: file objects keep\n"
    "    # the os functions of this load (a test's os.close = ... or os.dup =\n"
    "    # ... does not reach FileIO.close)\n"
    "    os = ns['os']\n"
    "    private_os = type(os)(os.__name__, os.__doc__)\n"
    "    private_os.__dict__.update(os.__dict__)\n"
    "    ns['os'] = private_os\n"
    "    # the C text_encoding() (CPython's warning text and stack level)\n"
    "    ns['text_encoding'] = _io.text_encoding\n"
    "    # fileio_getsetlist has its own 'closed' (with a docstring) where _pyio\n"
    "    # inherits IOBase.closed\n"
    "    ns['FileIO'].closed = property(ns['IOBase'].closed.fget,\n"
    "                                   doc='True if the file is closed')\n"
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

/* prelude helper _mark_native(code): the _pyio code runs as the C _io
   module for sys.settrace / sys.setprofile (QP_CO_NATIVE) */
static JSValue qp_iom_m_mark_native(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    if (!qp_code_bytecode(args[0]))
        return qp_iom_bad_argument(ctx, "_mark_native", "argument", "code", args[0]);
    qp_code_mark_native(args[0]);
    return JS_UNDEFINED;
}

static const QPMethodDef qp_iom_mark_native_def = {
    "_mark_native", qp_iom_m_mark_native, QP_METH_O, 0, NULL,
};

/* ========================================================================= */
/* Loading _pyio                                                              */
/* ========================================================================= */

/* Make the _pyio classes available in the module: 0 loaded (now or
   before), 1 a load is in progress (re-entrant access), -1 error. */
static int qp_iom_load(JSContext *ctx, JSValueConst module)
{
    JSValue prelude, load, reader, mark, res = JS_UNDEFINED, args[3], dict;
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
    mark = qp_cfunction_new(ctx, &qp_iom_mark_native_def, QP_NULL, JS_UNDEFINED);
    if (JS_IsException(mark)) {
        JS_FreeValue(ctx, load);
        JS_FreeValue(ctx, reader);
        return -1;
    }
    if (qp_iom_set_state(ctx, module, QP_IOM_LOADING) < 0) {
        JS_FreeValue(ctx, load);
        JS_FreeValue(ctx, reader);
        JS_FreeValue(ctx, mark);
        return -1;
    }
    args[0] = JS_DupValue(ctx, module);
    args[1] = reader;
    args[2] = mark;
    res = qp_vectorcall(ctx, load, (JSValueConst *)args, 3, QP_NULL);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
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

/* str value == s */
static BOOL qp_iom_str_equals(JSContext *ctx, JSValueConst v, const char *s)
{
    const char *c;
    size_t len;
    JSValue sv;
    BOOL r = FALSE;

    if (!qp_str_check(ctx, v))
        return FALSE;
    sv = qp_str_value(ctx, v);
    if (JS_IsException(sv)) {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        return FALSE;
    }
    c = qp_str_utf8(ctx, sv, &len);
    if (c) {
        r = len == strlen(s) && !memcmp(c, s, len);
        JS_FreeCString(ctx, c);
    } else {
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
    }
    JS_FreeValue(ctx, sv);
    return r;
}

/* _BootstrapFileWriter(fd) of the prelude */
static JSValue qp_iom_bootstrap_writer(JSContext *ctx, JSValueConst module, JSValueConst fd)
{
    JSValue prelude, cls, r;

    prelude = qp_iom_get_prop(ctx, module, QP_IOM_PRELUDE_PROP);
    cls = JS_IsException(prelude) ? JS_EXCEPTION : qp_iom_dict_get(ctx, prelude, "_BootstrapFileWriter");
    JS_FreeValue(ctx, prelude);
    if (JS_IsException(cls))
        return cls;
    if (QP_IS_NULL(cls))
        return qp_raise_system_error(ctx, "_io: prelude not initialized");
    r = qp_call1(ctx, cls, fd);
    JS_FreeValue(ctx, cls);
    return r;
}

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
    if (qp_iom_state(ctx, self) == QP_IOM_LOADING && JS_VALUE_GET_TAG(a[0]) == JS_TAG_INT &&
        !QP_IS_NULL(a[1]) && qp_iom_str_equals(ctx, a[1], "wb") && buffering == -1 && closefd) {
        for (i = 3; i < 6; i++) {
            if (!QP_IS_NULL(a[i]) && !qp_iom_is_none(a[i]))
                break;
        }
        if (i == 6 && (QP_IS_NULL(a[7]) || qp_iom_is_none(a[7])))
            return qp_iom_bootstrap_writer(ctx, self, a[0]);
    }
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
    qp_error err;
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
    memset(&err, 0, sizeof(err));
    r = qp_compile_source(ctx, qp_iom_prelude_src, sizeof(qp_iom_prelude_src) - 1,
                          "<_io prelude>", JS_UNDEFINED, 257 /* Py_file_input */,
                          QP_PF_IGNORE_COOKIE, -1,
                          &err);
    if (JS_IsException(r) && err.kind != QP_ERR_NONE)
        qp_raise_qp_error(ctx, &err, "<_io prelude>");
    qp_error_clear(&err);
    if (!JS_IsException(r)) {
        qp_code_mark_native(r);
        r = qp_run_code(ctx, r, prelude, QP_NULL);
    }
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
    if (buffering < 0 && cfg->detached) {
        /* not from the fstat() of the descriptor (raw._blksize) */
        JSValue size = qp_iom_getattr(ctx, io, "DEFAULT_BUFFER_SIZE");
        if (JS_IsException(size))
            goto error;
        if (JS_ToInt32(ctx, &buffering, size) < 0) {
            JS_FreeValue(ctx, size);
            goto error;
        }
        JS_FreeValue(ctx, size);
    }
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
    /* QPConfig.detached: nothing of the descriptor in the state (its
       fstat() result and seekability vary with the process), the stream
       is not seekable */
    if (cfg->detached &&
        (qp_iom_setattr(ctx, raw, "_stat_atopen", qp_none()) < 0 ||
         qp_iom_setattr(ctx, raw, "_seekable", JS_FALSE) < 0))
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

#define QP_IO_ENCODING_BUF 128
#define QP_IO_ERRORS_BUF 64

/* config_init_stdio_encoding + config_get_stdio_errors +
   config_get_codec_name: PyConfig.stdio_encoding and stdio_errors, the
   encoding and error handler of the standard streams.  *pencoding points
   to encoding_buf (QP_IO_ENCODING_BUF bytes) or a constant, *perrors to
   errors_buf (QP_IO_ERRORS_BUF bytes) or a constant.  0, or -1 with an
   exception pending. */
static int qp_io_stdio_config(JSContext *ctx, const QPConfig *cfg, char *encoding_buf,
                              char *errors_buf, const char **pencoding, const char **perrors)
{
    const char *encoding = NULL, *errors = NULL;

    /* config_init_stdio_encoding: PYTHONIOENCODING=encoding[:errors] */
    if (cfg->stdio_encoding) {
        const char *opt = cfg->stdio_encoding, *colon = strchr(opt, ':');
        size_t n = colon ? (size_t)(colon - opt) : strlen(opt);
        if (n > 0 && n < QP_IO_ENCODING_BUF) {
            memcpy(encoding_buf, opt, n);
            encoding_buf[n] = '\0';
            encoding = encoding_buf;
        }
        if (colon && colon[1] && strlen(colon + 1) < QP_IO_ERRORS_BUF) {
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
            if (!cs || !cs[0] || strlen(cs) >= QP_IO_ENCODING_BUF) {
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
    /* config_get_codec_name: normalize to the codec's name ("UTF-8" -> "utf-8") */
    {
        JSValue codec = qp_codec_lookup(ctx, encoding), name;
        const char *s;
        size_t len;
        if (JS_IsException(codec))
            return -1;
        name = qp_getattr(ctx, codec, QP_ATOM(ctx, name));
        JS_FreeValue(ctx, codec);
        if (JS_IsException(name))
            return -1;
        if (!qp_str_check(ctx, name)) {
            JS_FreeValue(ctx, name);
            qp_raise_type_error(ctx, "codec name must be a string");
            return -1;
        }
        s = qp_str_utf8(ctx, name, &len);
        if (!s) {
            JS_FreeValue(ctx, name);
            return -1;
        }
        if (len < QP_IO_ENCODING_BUF) {
            memmove(encoding_buf, s, len + 1);
            encoding = encoding_buf;
        }
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, name);
    }
    *pencoding = encoding;
    *perrors = errors;
    return 0;
}

/* init_set_builtins_open() + init_sys_streams(), called by the importlib
   boot code once the import system and the codec registry are ready.
   0, or -1 with an exception pending (CPython: fatal "can't initialize
   sys standard streams"). */
static int qp_io_install_stdio(JSContext *ctx)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    JSValue io, wrapper, std, encattr;
    char encoding_buf[QP_IO_ENCODING_BUF], errors_buf[QP_IO_ERRORS_BUF];
    const char *encoding, *errors;
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

    if (qp_io_stdio_config(ctx, cfg, encoding_buf, errors_buf, &encoding, &errors) < 0)
        goto fail;

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
