/* stringlib: partition implementation
 *
 * Derived from CPython Objects/stringlib/partition.h (PSF-2.0), v3.15.2.
 *
 * Buffer-level port: out[0..2] receive the spans of the three parts of the
 * result tuple.  Return 1 if sep was found, 0 if not, QP_SL_EMPTY_SEP for an
 * empty separator (CPython: ValueError("empty separator")).
 *
 * CPython object identities, to be reproduced by the caller:
 *  - found: element 1 is sep_obj itself; elements 0 and 2 are new strings.
 *  - not found (immutable str/bytes): partition returns (str_obj, '', ''),
 *    rpartition ('', '', str_obj), with the shared empty object; the spans
 *    are [0, len), [len, len), [len, len) and [0, 0), [0, 0), [0, len).
 *    bytearray (STRINGLIB_MUTABLE in CPython) creates three new objects.
 */

#ifndef STRINGLIB_FASTSEARCH_H
#  error must include "stringlib/fastsearch.h" before including this module
#endif

QP_SL_LOCAL_INLINE(int)
STRINGLIB(partition)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                     const STRINGLIB_CHAR* sep, Py_ssize_t sep_len,
                     qp_span out[3])
{
    Py_ssize_t pos;

    if (sep_len == 0) {
        return QP_SL_EMPTY_SEP;
    }

    pos = FASTSEARCH(str, str_len, sep, sep_len, -1, FAST_SEARCH);

    if (pos < 0) {
        out[0].start = 0;       out[0].end = str_len;
        out[1].start = str_len; out[1].end = str_len;
        out[2].start = str_len; out[2].end = str_len;
        return 0;
    }

    out[0].start = 0;   out[0].end = pos;
    out[1].start = pos; out[1].end = pos + sep_len;
    pos += sep_len;
    out[2].start = pos; out[2].end = str_len;
    return 1;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(rpartition)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                      const STRINGLIB_CHAR* sep, Py_ssize_t sep_len,
                      qp_span out[3])
{
    Py_ssize_t pos;

    if (sep_len == 0) {
        return QP_SL_EMPTY_SEP;
    }

    pos = FASTSEARCH(str, str_len, sep, sep_len, -1, FAST_RSEARCH);

    if (pos < 0) {
        out[0].start = 0; out[0].end = 0;
        out[1].start = 0; out[1].end = 0;
        out[2].start = 0; out[2].end = str_len;
        return 0;
    }

    out[0].start = 0;   out[0].end = pos;
    out[1].start = pos; out[1].end = pos + sep_len;
    pos += sep_len;
    out[2].start = pos; out[2].end = str_len;
    return 1;
}
