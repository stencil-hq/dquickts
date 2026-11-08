/*
 * stringlib.h - buffer-level port of CPython's string algorithms for the
 * Python runtime of dquickts.
 *
 * Derived from CPython Objects/stringlib/ (fastsearch.h, find.h, count.h,
 * split.h, partition.h, replace.h, find_max_char.h, codecs.h,
 * transmogrify.h) and the str helpers of Objects/unicodeobject.c (replace(),
 * any_find_slice(), unicode_count_impl(), tailmatch(), split()/rsplit(),
 * pad(), expandtabs, center/ljust/rjust/zfill, do_strip()/_PyUnicode_XStrip(),
 * _PyUnicode_JoinArray(), the UTF-8 codec drivers) (PSF-2.0), v3.15.2.
 *
 * Part of the single quickjs.c translation unit (python/ is #included by
 * python/quickpy.c): everything is `static`.  Include order:
 * "compat.h", "unicodetype.h", "stringlib.h", ... then "stringlib.c" (after
 * "unicodetype.c").  Nothing here depends on the Python object model: the
 * functions work on code-unit buffers and report results as indices, spans
 * or freshly built buffers; the str/bytes types turn those into objects.
 *
 * ---------------------------------------------------------------------------
 * Views
 *   qp_strview { kind, data, len }: kind 1 (Py_UCS1 / Latin-1 or bytes),
 *   2 (Py_UCS2) or 4 (Py_UCS4); len and every index are in code units of that
 *   kind.  The str type hands UCS4 views for strings that contain surrogate
 *   pairs, so indices are code points.  Views need NOT be canonical (a kind-2
 *   view may hold only Latin-1 characters): every function returns the same
 *   values as CPython does for the equal str object.  For bytes/bytearray use
 *   kind-1 views and the QP_SL_BYTES flag where a function takes `flags`.
 *
 * Status codes (all functions document which they return)
 *   index/count >= 0  success
 *   QP_SL_NOT_FOUND   -1: "not found", exactly where CPython returns -1
 *   QP_SL_NOMEM       allocation failure (js_malloc_rt/js_realloc_rt failed,
 *                     or a size computation overflowed where CPython calls
 *                     PyErr_NoMemory): raise MemoryError
 *   QP_SL_OVERFLOW    result too long: raise OverflowError with the message
 *                     CPython uses for the operation (given per function)
 *   QP_SL_EMPTY_SEP   raise ValueError("empty separator")
 *   QP_SL_CODEC_ERROR UTF-8 error; details in the qp_codec_error
 *   QP_SL_UNCHANGED   0: transforming function made no change; CPython
 *                     returns the input object itself (unicode_result_unchanged
 *                     for exact str; for bytes `return_self`, which also
 *                     copies for bytearray/subclasses).  The builder is untouched.
 *   QP_SL_BUILT       1: the result is in the qp_ucsbuilder.
 *
 * Memory: every allocation goes through js_malloc_rt/js_realloc_rt/js_free_rt
 * on the JSRuntime given to the function or stored in the vector/builder.
 * ---------------------------------------------------------------------------
 */
#ifndef QP_STRINGLIB_H
#define QP_STRINGLIB_H

#include "compat.h"
#include "unicodetype.h"

#define QP_SL_NOT_FOUND    (-1)
#define QP_SL_NOMEM        (-2)
#define QP_SL_OVERFLOW     (-3)
#define QP_SL_EMPTY_SEP    (-4)
#define QP_SL_CODEC_ERROR  (-5)
#define QP_SL_UNCHANGED    0
#define QP_SL_BUILT        1

/* `flags` for the whitespace / line-break dependent functions */
#define QP_SL_STR    0  /* str: Py_UNICODE_ISSPACE, Py_UNICODE_ISLINEBREAK */
#define QP_SL_BYTES  1  /* bytes/bytearray (kind-1 views only): Py_ISSPACE,
                           line breaks '\n' and '\r' only */

/* strip types (CPython LEFTSTRIP/RIGHTSTRIP/BOTHSTRIP) */
#define QP_SL_LEFTSTRIP  0
#define QP_SL_RIGHTSTRIP 1
#define QP_SL_BOTHSTRIP  2

typedef struct qp_strview {
    int kind;               /* 1, 2 or 4 */
    const void *data;       /* len code units of `kind` bytes each */
    Py_ssize_t len;
} qp_strview;

/* Code unit i of (kind, data); PyUnicode_READ. */
#define QP_SL_READ(kind, data, i)                                         \
    ((kind) == 1 ? (Py_UCS4)((const Py_UCS1 *)(data))[i] :               \
     (kind) == 2 ? (Py_UCS4)((const Py_UCS2 *)(data))[i] :               \
                   ((const Py_UCS4 *)(data))[i])

static inline qp_strview qp_strview_make(int kind, const void *data, Py_ssize_t len)
{
    qp_strview v;
    v.kind = kind;
    v.data = data;
    v.len = len;
    return v;
}

/* ---- span vector (split / rsplit / splitlines results) ------------------ */

typedef struct qp_span {
    Py_ssize_t start, end;  /* piece = view[start:end] */
} qp_span;

typedef struct qp_spanvec {
    JSRuntime *rt;
    qp_span *items;         /* js_malloc_rt'd; NULL while cap == 0 */
    Py_ssize_t len, cap;
} qp_spanvec;

static __maybe_unused void qp_spanvec_init(qp_spanvec *v, JSRuntime *rt);
static __maybe_unused void qp_spanvec_free(qp_spanvec *v);   /* releases items; v stays usable (empty) */
/* Append [start, end). 0 or QP_SL_NOMEM. */
static __maybe_unused int qp_spanvec_push(qp_spanvec *v, Py_ssize_t start, Py_ssize_t end);
/* Reverse items[from:len] in place (PyList_Reverse of rsplit). */
static __maybe_unused void qp_spanvec_reverse(qp_spanvec *v, Py_ssize_t from);

/* ---- kind-widening output builder --------------------------------------- */

/* Growable code-unit buffer whose kind widens (1 -> 2 -> 4) on demand, like
   CPython's _PyUnicodeWriter.  Built by putc/write/fill, its kind is the
   minimal one for its content.  The bulk results of replace/expandtabs/pad/
   join use CPython's kind choice, which is minimal whenever the input views
   are minimal; qp_ucsbuilder_narrow() makes it minimal unconditionally. */
typedef struct qp_ucsbuilder {
    JSRuntime *rt;
    void *data;             /* js_malloc_rt'd; NULL while cap == 0 */
    Py_ssize_t len;         /* code units written */
    Py_ssize_t cap;         /* code units allocated */
    int kind;               /* 1, 2 or 4 */
} qp_ucsbuilder;

static __maybe_unused void qp_ucsbuilder_init(qp_ucsbuilder *b, JSRuntime *rt);
static __maybe_unused void qp_ucsbuilder_free(qp_ucsbuilder *b);  /* back to the init state */
/* Ensure room for n more units and a kind able to hold maxchar (converting
   the content when widening).  0 or QP_SL_NOMEM. */
static __maybe_unused int qp_ucsbuilder_prepare(qp_ucsbuilder *b, Py_ssize_t n, Py_UCS4 maxchar);
static __maybe_unused int qp_ucsbuilder_putc(qp_ucsbuilder *b, Py_UCS4 ch);              /* 0 / QP_SL_NOMEM */
/* Append v[start:end] (0 <= start <= end <= v.len). 0 / QP_SL_NOMEM. */
static __maybe_unused int qp_ucsbuilder_write(qp_ucsbuilder *b, qp_strview v,
                                              Py_ssize_t start, Py_ssize_t end);
static __maybe_unused int qp_ucsbuilder_fill(qp_ucsbuilder *b, Py_UCS4 ch, Py_ssize_t n); /* 0 / QP_SL_NOMEM */
/* Narrow to the minimal kind (unicode_adjust_maxchar).  0 / QP_SL_NOMEM. */
static __maybe_unused int qp_ucsbuilder_narrow(qp_ucsbuilder *b);
static __maybe_unused qp_strview qp_ucsbuilder_view(const qp_ucsbuilder *b);
/* Hand the buffer (js_free_rt it on b->rt) to the caller and reset b.
   May return NULL when *len == 0. */
static __maybe_unused void *qp_ucsbuilder_detach(qp_ucsbuilder *b, int *kind, Py_ssize_t *len);

/* ---- character classes / scans ------------------------------------------ */

/* Py_UNICODE_ISSPACE / Py_UNICODE_ISLINEBREAK (ASCII fast path + database). */
static __maybe_unused int qp_sl_unicode_isspace(Py_UCS4 ch);
static __maybe_unused int qp_sl_unicode_islinebreak(Py_UCS4 ch);

/* find_max_char over the whole view: 127 if all ASCII, else 255, 0xFFFF or
   0x10FFFF (an upper bound exact at kind granularity).  0x7F for len 0. */
static __maybe_unused Py_UCS4 qp_sl_find_max_char(qp_strview s);
/* Kind (1, 2, 4) needed to store maxchar. */
static __maybe_unused int qp_sl_kind_for_maxchar(Py_UCS4 maxchar);

/* ---- searching ----------------------------------------------------------- */

/* CPython ADJUST_INDICES: clamp slice bounds of str.find/count/... like
   s[start:end] (end > len -> len; negative -> += len, floored at 0). */
static __maybe_unused void qp_sl_adjust_indices(Py_ssize_t *start, Py_ssize_t *end, Py_ssize_t len);

/* any_find_slice / PyUnicode_Find: index of the first (direction > 0) or
   last (direction <= 0) occurrence of sub in s[start:end] (bounds adjusted),
   as an index into s.  QP_SL_NOT_FOUND or QP_SL_NOMEM (needle conversion).
   Empty sub: start for find, end for rfind (after adjustment), unless
   end - start < 0 -> QP_SL_NOT_FOUND.  Also serves bytes.find/rfind/index
   and `in` (found iff result >= 0). */
static __maybe_unused Py_ssize_t qp_sl_find(JSRuntime *rt, qp_strview s, qp_strview sub,
                                            Py_ssize_t start, Py_ssize_t end, int direction);
/* PyUnicode_FindChar: same for one code point ch; never allocates. */
static __maybe_unused Py_ssize_t qp_sl_find_char(qp_strview s, Py_UCS4 ch,
                                                 Py_ssize_t start, Py_ssize_t end, int direction);
/* unicode_count_impl: non-overlapping occurrences of sub in s[start:end].
   Empty sub counts end - start + 1 positions.  >= 0 or QP_SL_NOMEM. */
static __maybe_unused Py_ssize_t qp_sl_count(JSRuntime *rt, qp_strview s, qp_strview sub,
                                             Py_ssize_t start, Py_ssize_t end);
/* tailmatch: 1 if s[start:end] (adjusted) starts (direction < 0) or ends
   (direction > 0) with sub, else 0.  str/bytes startswith/endswith with one
   prefix; the caller loops over tuples.  Never allocates. */
static __maybe_unused int qp_sl_tailmatch(qp_strview s, qp_strview sub,
                                          Py_ssize_t start, Py_ssize_t end, int direction);

/* ---- splitting ----------------------------------------------------------- */

/* str.split / bytes.split.  sep == NULL: split on runs of whitespace
   (flags selects str or bytes whitespace).  maxsplit < 0: unlimited.
   Appends the spans of the result list, in order, to out.
   0, QP_SL_EMPTY_SEP (sep of length 0) or QP_SL_NOMEM.
   A single span [0, s.len) means CPython returns [s] (the object itself
   when it is an exact str/bytes). */
static __maybe_unused int qp_sl_split(JSRuntime *rt, qp_strview s, const qp_strview *sep,
                                      Py_ssize_t maxsplit, int flags, qp_spanvec *out);
/* str.rsplit / bytes.rsplit: as qp_sl_split, splitting from the right; the
   spans are still appended in left-to-right result order. */
static __maybe_unused int qp_sl_rsplit(JSRuntime *rt, qp_strview s, const qp_strview *sep,
                                       Py_ssize_t maxsplit, int flags, qp_spanvec *out);
/* str.splitlines / bytes.splitlines (CRLF is one break; keepends keeps the
   break in the span).  0 or QP_SL_NOMEM.  A single span [0, s.len) means the
   object itself (exact str/bytes). */
static __maybe_unused int qp_sl_splitlines(qp_strview s, int keepends, int flags, qp_spanvec *out);
/* str.partition (direction > 0) / rpartition (direction <= 0), bytes too.
   out[0..2] = spans of the 3 parts.  1 found (out[1] is the separator: CPython
   returns sep_obj itself there), 0 not found (CPython: (s, '', '') or
   ('', '', s) with s itself), QP_SL_EMPTY_SEP or QP_SL_NOMEM. */
static __maybe_unused int qp_sl_partition(JSRuntime *rt, qp_strview s, qp_strview sep,
                                          int direction, qp_span out[3]);

/* ---- stripping ------------------------------------------------------------ */

/* do_strip: span of s without leading/trailing whitespace (flags: str or
   bytes whitespace).  CPython returns s itself when the span is [0, len). */
static __maybe_unused void qp_sl_strip_ws(qp_strview s, int striptype, int flags, qp_span *out);
/* _PyUnicode_XStrip / bytes do_xstrip: strip characters contained in chars. */
static __maybe_unused void qp_sl_strip_chars(qp_strview s, qp_strview chars, int striptype,
                                             qp_span *out);

/* ---- building ------------------------------------------------------------- */

/* str.replace / bytes.replace (count < 0: all).  QP_SL_UNCHANGED (nothing to
   replace: CPython returns s), QP_SL_BUILT, QP_SL_NOMEM, or QP_SL_OVERFLOW
   (str: "replace string is too long"; bytes: "replace bytes is too long").
   Empty old inserts new before every character and at the end (at most
   count times).  The builder must be empty. */
static __maybe_unused int qp_sl_replace(qp_strview s, qp_strview old, qp_strview new_,
                                        Py_ssize_t count, qp_ucsbuilder *out);
/* str.expandtabs / bytes.expandtabs.  QP_SL_UNCHANGED when s has no tab
   (str returns s itself; bytes always builds a copy - the caller copies),
   QP_SL_BUILT, QP_SL_NOMEM, QP_SL_OVERFLOW (str: "new string is too long";
   bytes: "result too long").  Builder must be empty. */
static __maybe_unused int qp_sl_expandtabs(qp_strview s, int tabsize, qp_ucsbuilder *out);
/* pad(): left/right copies of fill around s (negative counts are 0).
   QP_SL_UNCHANGED if both 0, QP_SL_BUILT, QP_SL_NOMEM, QP_SL_OVERFLOW
   (str: "padded string is too long"; CPython's bytes pad has no such check
   and fails allocating, so raise MemoryError for bytes).  Builder must be
   empty. */
static __maybe_unused int qp_sl_pad(qp_strview s, Py_ssize_t left, Py_ssize_t right,
                                    Py_UCS4 fill, qp_ucsbuilder *out);
/* str/bytes center, ljust, rjust (fill: the fillchar) and zfill; results as
   qp_sl_pad (QP_SL_UNCHANGED when len(s) >= width). */
static __maybe_unused int qp_sl_center(qp_strview s, Py_ssize_t width, Py_UCS4 fill, qp_ucsbuilder *out);
static __maybe_unused int qp_sl_ljust(qp_strview s, Py_ssize_t width, Py_UCS4 fill, qp_ucsbuilder *out);
static __maybe_unused int qp_sl_rjust(qp_strview s, Py_ssize_t width, Py_UCS4 fill, qp_ucsbuilder *out);
static __maybe_unused int qp_sl_zfill(qp_strview s, Py_ssize_t width, qp_ucsbuilder *out);
/* _PyUnicode_JoinArray core: items[0] sep items[1] sep ... (sep == NULL: a
   single space, as for a NULL separator).  The caller handles n == 0 (empty
   result), n == 1 with an exact str/bytes item (the item itself) and item
   type checks.  QP_SL_BUILT, QP_SL_NOMEM or QP_SL_OVERFLOW (str: "join()
   result is too long for a Python string"; bytes: "join() result is too
   long").  Builder must be empty. */
static __maybe_unused int qp_sl_join(const qp_strview *sep, const qp_strview *items,
                                     Py_ssize_t n, qp_ucsbuilder *out);

/* ---- UTF-8 ('strict') ----------------------------------------------------- */

typedef struct qp_codec_error {
    Py_ssize_t start, end;  /* UnicodeError.start/.end: byte offsets (decode)
                               or code-unit indices of the view (encode) */
    const char *reason;     /* static: "invalid start byte", "invalid
                               continuation byte", "unexpected end of data",
                               "surrogates not allowed" */
} qp_codec_error;

/* Decode s[0:size] as UTF-8, appending to out (its kind widens as needed).
   consumed == NULL: final decode, a truncated sequence at the end is an
   error.  consumed != NULL: stateful decode (codecs.utf_8_decode final=False):
   stops before an incomplete trailing sequence and stores the number of
   bytes used.  0, QP_SL_NOMEM, or QP_SL_CODEC_ERROR with *err set for the
   UnicodeDecodeError('utf-8', s, err->start, err->end, err->reason); out then
   holds the characters before the error. */
static __maybe_unused int qp_sl_utf8_decode(const char *s, Py_ssize_t size, Py_ssize_t *consumed,
                                            qp_ucsbuilder *out, qp_codec_error *err);
/* Encode s as UTF-8 into a new js_malloc_rt'd buffer (*pdata, *plen; free
   with js_free_rt; *pdata may be NULL when *plen == 0).  Surrogate code
   units are an error ("surrogates not allowed", start/end = the run of
   consecutive surrogates, UnicodeEncodeError('utf-8', ...)).  0,
   QP_SL_NOMEM or QP_SL_CODEC_ERROR (*pdata = NULL). */
static __maybe_unused int qp_sl_utf8_encode(JSRuntime *rt, qp_strview s, char **pdata,
                                            Py_ssize_t *plen, qp_codec_error *err);

#endif /* QP_STRINGLIB_H */
