/*
 * Private layout shared by python/bytesobject.c, python/bytearrayobject.c
 * and python/memoryobject.c (unit g).  Not part of the object.h contract.
 *
 * Payloads (JSObject.u.opaque), discriminated by their first byte:
 *  - bytes (class PY_BYTES): QPBytesObject with the data stored inline
 *    after the struct (immutable, NUL-terminated like CPython ob_sval).
 *  - bytearray (class PY_BYTEARRAY): QPBytesObject whose data is a separate
 *    js_malloc'd block of `alloc` bytes (+1 for a trailing NUL), resized with
 *    CPython's growth policy; resizing fails while `exports` > 0.
 *  - memoryview (class PY_BYTEARRAY, prototype `memoryview`): QPMemoryView.
 *  - managed buffer (class PY_BYTEARRAY, prototype null, never visible to
 *    Python): QPManagedBuffer, CPython's _PyManagedBufferObject: holds the
 *    one buffer export taken from the underlying object; shared by every
 *    memoryview derived from it (slices, casts, memoryview(mv)).
 * Every finalizer resets u.opaque to NULL so that objects finalized later
 * in the same GC cycle never read a freed payload.
 *
 * The data of a bytes object is not an ArrayBuffer: Python code never sees
 * the backing store, so it is kept inline (one allocation per bytes); JS
 * interop (M4) copies or wraps it.
 */
#ifndef QP_BYTESOBJECT_INTERNAL_H
#define QP_BYTESOBJECT_INTERNAL_H

enum {
    QP_BK_BYTES = 1,
    QP_BK_BYTEARRAY,
    QP_BK_MEMORYVIEW,
    QP_BK_MBUF,
};

typedef struct QPBytesObject {
    uint8_t kind;               /* QP_BK_BYTES / QP_BK_BYTEARRAY */
    Py_ssize_t len;
    uint8_t *data;              /* bytes: inline_data; bytearray: js_malloc'd or NULL when alloc == 0 */
    Py_hash_t hash;             /* bytes: cached hash, -1 = not computed */
    Py_ssize_t alloc;           /* bytearray: allocated size (CPython ob_alloc) */
    Py_ssize_t exports;         /* bytearray: buffer exports (CPython ob_exports) */
    uint8_t inline_data[];      /* bytes: len + 1 bytes */
} QPBytesObject;

typedef struct QPManagedBuffer {
    uint8_t kind;               /* QP_BK_MBUF */
    uint8_t released;
    Py_ssize_t exports;         /* registered memoryviews not yet released */
    QPBuffer master;            /* export of the underlying object (master.obj owned) */
} QPManagedBuffer;

/* memoryview flags (CPython _Py_MEMORYVIEW_*) */
#define QP_MV_RELEASED   0x001
#define QP_MV_C          0x002
#define QP_MV_FORTRAN    0x004
#define QP_MV_SCALAR     0x008

#define QP_MV_MAX_NDIM   64     /* PyBUF_MAX_NDIM */
#define QP_MV_FMT_INLINE 16     /* formats shorter than this are stored inline */

typedef struct QPMemoryView {
    uint8_t kind;               /* QP_BK_MEMORYVIEW */
    int flags;                  /* QP_MV_* */
    JSValue mbuf;               /* managed buffer object (owned), JS_UNDEFINED for typed-array views */
    Py_hash_t hash;             /* -1 = not computed */
    Py_ssize_t exports;         /* buffers exported from this view */
    /* view (CPython Py_buffer of the memoryview) */
    uint8_t *buf;
    Py_ssize_t len;             /* product(shape) * itemsize */
    Py_ssize_t itemsize;
    int readonly;
    int ndim;
    /* NUL-terminated struct format.  Heap memoryviews own it: format_inline
       or js_malloc'd (qp_mv_set_format).  Stack descriptors of typed arrays
       and QPBuffers borrow the exporter's string (qp_mv_borrow_format). */
    const char *format;
    char format_inline[QP_MV_FMT_INLINE];
    Py_ssize_t *shape;          /* ndim entries: dims1 or js_malloc'd */
    Py_ssize_t *strides;
    Py_ssize_t dims1[2];        /* inline shape[0], strides[0] for ndim <= 1 */
} QPMemoryView;

static __maybe_unused const uint8_t qp_bytes_empty_data[1] = { 0 };

/* Kind of a value's payload, 0 if it is not one of ours. */
static inline int qp_bk_kind(JSValueConst v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_PY_BYTES && p->class_id != JS_CLASS_PY_BYTEARRAY)
        return 0;
    if (!p->u.opaque)
        return 0;
    return *(uint8_t *)p->u.opaque;
}

#define QP_BYTES_OBJ(v) ((QPBytesObject *)JS_VALUE_GET_OBJ(v)->u.opaque)
#define QP_MV_OBJ(v)    ((QPMemoryView *)JS_VALUE_GET_OBJ(v)->u.opaque)
#define QP_MBUF_OBJ(v)  ((QPManagedBuffer *)JS_VALUE_GET_OBJ(v)->u.opaque)

/* Provided by typeobject.c (unit h): tp_hash of unhashable builtin types. */
static Py_hash_t qp_hash_not_implemented(JSContext *ctx, JSValueConst v);
/* Provided by iterobject.c (unit e): bytes_iterator / bytearray_iterator. */
static JSValue qp_bytes_iter_new(JSContext *ctx, JSValueConst b);

/* ---- bytesobject.c: shared helpers (CPython bytes_methods.c & co.) ---- */
static JSValue qp_bytes_new_type(JSContext *ctx, QPType *subtype, const void *data, Py_ssize_t len);
static JSValue qp_bm_new(JSContext *ctx, int bytearray, const void *data, Py_ssize_t len);
static JSValue qp_bm_tpname(JSContext *ctx, JSValueConst v);
static BOOL qp_buffer_check(JSContext *ctx, JSValueConst v);
static BOOL qp_index_check(JSContext *ctx, JSValueConst v);
static int qp_bm_exc_clear_if(JSContext *ctx, QPType *type);
static void qp_buffer_release_rt(JSRuntime *rt, QPBuffer *view);
static JSValue qp_bytes_strhex(JSContext *ctx, const uint8_t *data, Py_ssize_t len,
                               JSValueConst sep, Py_ssize_t bytes_per_sep);
static JSValue qp_bytes_from_hex(JSContext *ctx, JSValueConst string, int use_bytearray);
static JSValue qp_bytes_format(JSContext *ctx, const uint8_t *fmt, Py_ssize_t fmtlen,
                               JSValueConst args, int use_bytearray);
static int qp_bm_parse_hex_args(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                                JSValueConst kwnames, JSValueConst *psep, Py_ssize_t *pbps);
static int qp_bm_hex_sep(JSContext *ctx, JSValueConst sep, int *psep);
static JSValue qp_bm_strhex_build(JSContext *ctx, const uint8_t *argbuf, Py_ssize_t arglen,
                                  int sep_char, Py_ssize_t bytes_per_sep_group);
static JSValue qp_bm_raise_tpname(JSContext *ctx, QPType *type, const char *fmt, JSValueConst v);
static JSValue qp_bm_bad_argument(JSContext *ctx, const char *fname, const char *argdesc,
                                  const char *expected, JSValueConst v);
static BOOL qp_bm_number_check(JSContext *ctx, JSValueConst v);
static BOOL qp_bm_is_typed_array_class(int class_id);
static void qp_bm_js_buffer_info(JSObject *p, uint8_t **pbuf, Py_ssize_t *plen,
                                 int *preadonly, Py_ssize_t *pitemsize, const char **pformat);
static const char *qp_bm_arg_cstr(JSContext *ctx, const char *fname, const char *argname,
                                  JSValueConst v);
static JSValue qp_bytes_repr_cls(JSContext *ctx, const uint8_t *data, Py_ssize_t length,
                                 int smartquotes, const char *classname);
static JSValue qp_bm_cmp_result(Py_ssize_t c, int op);
static JSValue qp_bm_concat(JSContext *ctx, JSValueConst a, JSValueConst b, int bytearray);
static void qp_bm_repeat_fill(uint8_t *dest, Py_ssize_t len_dest, const uint8_t *src, Py_ssize_t len_src);
static JSValue qp_bm_slice(JSContext *ctx, int bytearray, const uint8_t *src,
                           Py_ssize_t start, Py_ssize_t step, Py_ssize_t slicelength);
static JSValue qp_bytes_tp_iter(JSContext *ctx, JSValueConst self);
static int qp_bytes_tp_iteritem(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem);
static int qp_bm_contains(JSContext *ctx, JSValueConst self, JSValueConst arg);
static JSValue qp_bm_mod(JSContext *ctx, JSValueConst self, JSValueConst arg);

/* methods shared by bytes and bytearray (QPCFunc) */
#define QP_BM_DECL(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs, \
                        JSValueConst kwnames, int magic)
QP_BM_DECL(qp_bm_case);
QP_BM_DECL(qp_bm_isx);
QP_BM_DECL(qp_bm_pad);
QP_BM_DECL(qp_bm_zfill);
QP_BM_DECL(qp_bm_expandtabs);
QP_BM_DECL(qp_bm_find);
QP_BM_DECL(qp_bm_count);
QP_BM_DECL(qp_bm_tailmatch);
QP_BM_DECL(qp_bm_decode);
QP_BM_DECL(qp_bm_hex);
QP_BM_DECL(qp_bm_join);
QP_BM_DECL(qp_bm_strip);
QP_BM_DECL(qp_bm_maketrans);
QP_BM_DECL(qp_bm_partition);
QP_BM_DECL(qp_bm_replace);
QP_BM_DECL(qp_bm_removeaffix);
QP_BM_DECL(qp_bm_split);
QP_BM_DECL(qp_bm_splitlines);
QP_BM_DECL(qp_bm_translate);
QP_BM_DECL(qp_bm_fromhex);
#undef QP_BM_DECL

/* Method table entries common to bytes and bytearray (CPython keeps one
   copy per type with the same docstrings). */
#define QP_BM_SHARED_METHODS                                                                  \
    { "capitalize", qp_bm_case, QP_METH_NOARGS, 4,                                            \
      "capitalize($self, /)\n--\n\nB.capitalize() -> copy of B\n\nReturn a copy of B with only its first character capitalized (ASCII)\nand the rest lower-cased." }, \
    { "center", qp_bm_pad, QP_METH_FASTCALL, 0,                                               \
      "center($self, width, fillchar=b' ', /)\n--\n\nReturn a centered string of length width.\n\nPadding is done using the specified fill character." }, \
    { "count", qp_bm_count, QP_METH_FASTCALL, 0,                                              \
      "count($self, sub[, start[, end]], /)\n--\n\nReturn the number of non-overlapping occurrences of subsection 'sub' in bytes B[start:end]." }, \
    { "decode", qp_bm_decode, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                        \
      "decode($self, /, encoding='utf-8', errors='strict')\n--\n\nDecode the bytes using the codec registered for encoding." }, \
    { "endswith", qp_bm_tailmatch, QP_METH_FASTCALL, 1,                                       \
      "endswith($self, suffix[, start[, end]], /)\n--\n\nReturn True if the bytes ends with the specified suffix, False otherwise." }, \
    { "expandtabs", qp_bm_expandtabs, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                \
      "expandtabs($self, /, tabsize=8)\n--\n\nReturn a copy where all tab characters are expanded using spaces.\n\nIf tabsize is not given, a tab size of 8 characters is assumed." }, \
    { "find", qp_bm_find, QP_METH_FASTCALL, 0,                                                \
      "find($self, sub[, start[, end]], /)\n--\n\nReturn the lowest index in B where subsection 'sub' is found, such that 'sub' is contained within B[start:end].\n\nReturn -1 on failure." }, \
    { "hex", qp_bm_hex, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                              \
      "hex($self, /, sep=<unrepresentable>, bytes_per_sep=1)\n--\n\nCreate a string of hexadecimal numbers from a bytes object." }, \
    { "index", qp_bm_find, QP_METH_FASTCALL, 1,                                               \
      "index($self, sub[, start[, end]], /)\n--\n\nReturn the lowest index in B where subsection 'sub' is found, such that 'sub' is contained within B[start:end].\n\nRaise ValueError if the subsection is not found." }, \
    { "isalnum", qp_bm_isx, QP_METH_NOARGS, 0,                                                \
      "isalnum($self, /)\n--\n\nB.isalnum() -> bool\n\nReturn True if all characters in B are alphanumeric\nand there is at least one character in B, False otherwise." }, \
    { "isalpha", qp_bm_isx, QP_METH_NOARGS, 1,                                                \
      "isalpha($self, /)\n--\n\nB.isalpha() -> bool\n\nReturn True if all characters in B are alphabetic\nand there is at least one character in B, False otherwise." }, \
    { "isascii", qp_bm_isx, QP_METH_NOARGS, 2,                                                \
      "isascii($self, /)\n--\n\nB.isascii() -> bool\n\nReturn True if B is empty or all characters in B are ASCII,\nFalse otherwise." }, \
    { "isdigit", qp_bm_isx, QP_METH_NOARGS, 3,                                                \
      "isdigit($self, /)\n--\n\nB.isdigit() -> bool\n\nReturn True if all characters in B are digits\nand there is at least one character in B, False otherwise." }, \
    { "islower", qp_bm_isx, QP_METH_NOARGS, 4,                                                \
      "islower($self, /)\n--\n\nB.islower() -> bool\n\nReturn True if all cased characters in B are lowercase and there is\nat least one cased character in B, False otherwise." }, \
    { "isspace", qp_bm_isx, QP_METH_NOARGS, 5,                                                \
      "isspace($self, /)\n--\n\nB.isspace() -> bool\n\nReturn True if all characters in B are whitespace\nand there is at least one character in B, False otherwise." }, \
    { "istitle", qp_bm_isx, QP_METH_NOARGS, 6,                                                \
      "istitle($self, /)\n--\n\nB.istitle() -> bool\n\nReturn True if B is a titlecased string and there is at least one\ncharacter in B, i.e. uppercase characters may only follow uncased\ncharacters and lowercase characters only cased ones. Return False\notherwise." }, \
    { "isupper", qp_bm_isx, QP_METH_NOARGS, 7,                                                \
      "isupper($self, /)\n--\n\nB.isupper() -> bool\n\nReturn True if all cased characters in B are uppercase and there is\nat least one cased character in B, False otherwise." }, \
    { "join", qp_bm_join, QP_METH_O, 0,                                                       \
      "join($self, iterable_of_bytes, /)\n--\n\nConcatenate any number of bytes objects.\n\nThe bytes whose method is called is inserted in between each pair.\n\nThe result is returned as a new bytes object.\n\nExample: b'.'.join([b'ab', b'pq', b'rs']) -> b'ab.pq.rs'." }, \
    { "ljust", qp_bm_pad, QP_METH_FASTCALL, 1,                                                \
      "ljust($self, width, fillchar=b' ', /)\n--\n\nReturn a left-justified string of length width.\n\nPadding is done using the specified fill character." }, \
    { "lower", qp_bm_case, QP_METH_NOARGS, 0,                                                 \
      "lower($self, /)\n--\n\nB.lower() -> copy of B\n\nReturn a copy of B with all ASCII characters converted to lowercase." }, \
    { "lstrip", qp_bm_strip, QP_METH_FASTCALL, QP_SL_LEFTSTRIP,                               \
      "lstrip($self, bytes=None, /)\n--\n\nStrip leading bytes contained in the argument.\n\nIf the argument is omitted or None, strip leading  ASCII whitespace." }, \
    { "maketrans", qp_bm_maketrans, QP_METH_FASTCALL | QP_METH_STATIC, 0,                     \
      "maketrans(frm, to, /)\n--\n\nReturn a translation table usable for the bytes or bytearray translate method.\n\nThe returned table will be one where each byte in frm is mapped to the byte at\nthe same position in to.\n\nThe bytes objects frm and to must be of the same length." }, \
    { "partition", qp_bm_partition, QP_METH_O, 0,                                             \
      "partition($self, sep, /)\n--\n\nPartition the bytes into three parts using the given separator." }, \
    { "replace", qp_bm_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                      \
      "replace($self, old, new, /, count=-1)\n--\n\nReturn a copy with all occurrences of substring old replaced by new." }, \
    { "removeprefix", qp_bm_removeaffix, QP_METH_O, 0,                                        \
      "removeprefix($self, prefix, /)\n--\n\nReturn a bytes object with the given prefix string removed if present.\n\nIf the bytes starts with the prefix string, return bytes[len(prefix):].\nOtherwise, return a copy of the original bytes." }, \
    { "removesuffix", qp_bm_removeaffix, QP_METH_O, 1,                                        \
      "removesuffix($self, suffix, /)\n--\n\nReturn a bytes object with the given suffix string removed if present.\n\nIf the bytes ends with the suffix string and that suffix is not empty,\nreturn bytes[:-len(prefix)].  Otherwise, return a copy of the original\nbytes." }, \
    { "rfind", qp_bm_find, QP_METH_FASTCALL, 2,                                               \
      "rfind($self, sub[, start[, end]], /)\n--\n\nReturn the highest index in B where subsection 'sub' is found, such that 'sub' is contained within B[start:end].\n\nReturn -1 on failure." }, \
    { "rindex", qp_bm_find, QP_METH_FASTCALL, 3,                                              \
      "rindex($self, sub[, start[, end]], /)\n--\n\nReturn the highest index in B where subsection 'sub' is found, such that 'sub' is contained within B[start:end].\n\nRaise ValueError if the subsection is not found." }, \
    { "rjust", qp_bm_pad, QP_METH_FASTCALL, 2,                                                \
      "rjust($self, width, fillchar=b' ', /)\n--\n\nReturn a right-justified string of length width.\n\nPadding is done using the specified fill character." }, \
    { "rpartition", qp_bm_partition, QP_METH_O, 1,                                            \
      "rpartition($self, sep, /)\n--\n\nPartition the bytes into three parts using the given separator." }, \
    { "rsplit", qp_bm_split, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,                         \
      "rsplit($self, /, sep=None, maxsplit=-1)\n--\n\nReturn a list of the sections in the bytes, using sep as the delimiter." }, \
    { "rstrip", qp_bm_strip, QP_METH_FASTCALL, QP_SL_RIGHTSTRIP,                              \
      "rstrip($self, bytes=None, /)\n--\n\nStrip trailing bytes contained in the argument.\n\nIf the argument is omitted or None, strip trailing ASCII whitespace." }, \
    { "split", qp_bm_split, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                          \
      "split($self, /, sep=None, maxsplit=-1)\n--\n\nReturn a list of the sections in the bytes, using sep as the delimiter." }, \
    { "splitlines", qp_bm_splitlines, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                \
      "splitlines($self, /, keepends=False)\n--\n\nReturn a list of the lines in the bytes, breaking at line boundaries.\n\nLine breaks are not included in the resulting list unless keepends is given and\ntrue." }, \
    { "startswith", qp_bm_tailmatch, QP_METH_FASTCALL, 0,                                     \
      "startswith($self, prefix[, start[, end]], /)\n--\n\nReturn True if the bytes starts with the specified prefix, False otherwise." }, \
    { "strip", qp_bm_strip, QP_METH_FASTCALL, QP_SL_BOTHSTRIP,                                \
      "strip($self, bytes=None, /)\n--\n\nStrip leading and trailing bytes contained in the argument.\n\nIf the argument is omitted or None, strip leading and trailing ASCII whitespace." }, \
    { "swapcase", qp_bm_case, QP_METH_NOARGS, 2,                                              \
      "swapcase($self, /)\n--\n\nB.swapcase() -> copy of B\n\nReturn a copy of B with uppercase ASCII characters converted\nto lowercase ASCII and vice versa." }, \
    { "title", qp_bm_case, QP_METH_NOARGS, 3,                                                 \
      "title($self, /)\n--\n\nB.title() -> copy of B\n\nReturn a titlecased version of B, i.e. ASCII words start with uppercase\ncharacters, all remaining cased characters have lowercase." }, \
    { "translate", qp_bm_translate, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,                  \
      "translate($self, table, /, delete=b'')\n--\n\nReturn a copy with each character mapped by the given translation table." }, \
    { "upper", qp_bm_case, QP_METH_NOARGS, 1,                                                 \
      "upper($self, /)\n--\n\nB.upper() -> copy of B\n\nReturn a copy of B with all ASCII characters converted to uppercase." }, \
    { "zfill", qp_bm_zfill, QP_METH_O, 0,                                                     \
      "zfill($self, width, /)\n--\n\nPad a numeric string with zeros on the left, to fill a field of the given width.\n\nThe original string is never truncated." }

/* ---- bytearrayobject.c ---- */
static int qp_init_bytearray_type(JSContext *ctx);
static void qp_bytearray_free_payload(JSRuntime *rt, QPBytesObject *b);
static int qp_bytearray_getbuffer(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags);

/* ---- memoryobject.c ---- */
static int qp_init_memoryview_type(JSContext *ctx);
static void qp_memoryview_finalize(JSRuntime *rt, QPMemoryView *mv);
static void qp_memoryview_mark(JSRuntime *rt, QPMemoryView *mv, JS_MarkFunc *mark_func);
static void qp_mbuf_finalize(JSRuntime *rt, QPManagedBuffer *mb);
static void qp_mbuf_mark(JSRuntime *rt, QPManagedBuffer *mb, JS_MarkFunc *mark_func);
/* bytes(memoryview) and other full-buffer copies of a possibly
   non-contiguous memoryview (or JS typed array): new bytes or bytearray. */
static JSValue qp_memoryview_to_bytes(JSContext *ctx, JSValueConst mv, int bytearray);
static BOOL qp_memoryview_check(JSValueConst v);
static int qp_memoryview_getbuffer(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags);

#endif /* QP_BYTESOBJECT_INTERNAL_H */
