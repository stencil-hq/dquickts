/* stringlib: split implementation
 *
 * Derived from CPython Objects/stringlib/split.h (PSF-2.0), v3.15.2.
 *
 * Buffer-level port: instead of building a list of new string objects, each
 * function appends the [start, end) index spans of the pieces, in the order
 * of CPython's resulting list, to a qp_spanvec (see python/stringlib.h).
 * Where CPython reuses `str_obj` itself as the only list item (exact str,
 * nothing to split), the span is simply [0, str_len); callers can detect it
 * to preserve the identity.  Return 0, or QP_SL_NOMEM if the span vector
 * could not grow.  The empty-separator ValueError is raised by the
 * dispatchers in python/stringlib.c before these are called.
 *
 * With STRINGLIB_WS_ONLY defined (byteslib), only the whitespace/line-break
 * functions are instantiated.
 */

#ifndef STRINGLIB_FASTSEARCH_H
#error must include "stringlib/fastsearch.h" before including this module
#endif

/* Append the span [left, right) of the current piece. */
#define SPLIT_ADD(left, right) do {                          \
        if (qp_spanvec_push(out, (left), (right)) < 0)       \
            return QP_SL_NOMEM;                              \
    } while (0)

QP_SL_LOCAL_INLINE(int)
STRINGLIB(split_whitespace)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                            Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t i, j;

    i = j = 0;
    while (maxcount-- > 0) {
        while (i < str_len && STRINGLIB_ISSPACE(str[i]))
            i++;
        if (i == str_len) break;
        j = i; i++;
        while (i < str_len && !STRINGLIB_ISSPACE(str[i]))
            i++;
        /* CPython: when j == 0 && i == str_len the str object itself is
           used as list[0]; that is the span [0, str_len). */
        SPLIT_ADD(j, i);
    }

    if (i < str_len) {
        /* Only occurs when maxcount was reached */
        /* Skip any remaining whitespace and copy to end of string */
        while (i < str_len && STRINGLIB_ISSPACE(str[i]))
            i++;
        if (i != str_len)
            SPLIT_ADD(i, str_len);
    }
    return 0;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(rsplit_whitespace)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                             Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t i, j;
    Py_ssize_t first = out->len;

    i = j = str_len - 1;
    while (maxcount-- > 0) {
        while (i >= 0 && STRINGLIB_ISSPACE(str[i]))
            i--;
        if (i < 0) break;
        j = i; i--;
        while (i >= 0 && !STRINGLIB_ISSPACE(str[i]))
            i--;
        SPLIT_ADD(i + 1, j + 1);
    }

    if (i >= 0) {
        /* Only occurs when maxcount was reached */
        /* Skip any remaining whitespace and copy to beginning of string */
        while (i >= 0 && STRINGLIB_ISSPACE(str[i]))
            i--;
        if (i >= 0)
            SPLIT_ADD(0, i + 1);
    }
    /* CPython: PyList_Reverse(list) */
    qp_spanvec_reverse(out, first);
    return 0;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(splitlines)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                      int keepends, qp_spanvec *out)
{
    Py_ssize_t i;
    Py_ssize_t j;

    for (i = j = 0; i < str_len; ) {
        Py_ssize_t eol;

        /* Find a line and append it */
        while (i < str_len && !STRINGLIB_ISLINEBREAK(str[i]))
            i++;

        /* Skip the line break reading CRLF as one line break */
        eol = i;
        if (i < str_len) {
            if (str[i] == '\r' && i + 1 < str_len && str[i+1] == '\n')
                i += 2;
            else
                i++;
            if (keepends)
                eol = i;
        }
        /* CPython: when j == 0 && eol == str_len the str object itself is
           appended; that is the span [0, str_len). */
        SPLIT_ADD(j, eol);
        j = i;
    }
    return 0;
}

#ifndef STRINGLIB_WS_ONLY

QP_SL_LOCAL_INLINE(int)
STRINGLIB(split_char)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                      const STRINGLIB_CHAR ch,
                      Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t i, j;

    i = j = 0;
    while ((j < str_len) && (maxcount-- > 0)) {
        for(; j < str_len; j++) {
            /* I found that using memchr makes no difference */
            if (str[j] == ch) {
                SPLIT_ADD(i, j);
                i = j = j + 1;
                break;
            }
        }
    }
    /* CPython: with no match the str object itself is list[0]; i == 0 then,
       so the span below is [0, str_len). */
    if (i <= str_len) {
        SPLIT_ADD(i, str_len);
    }
    return 0;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(split)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                 const STRINGLIB_CHAR* sep, Py_ssize_t sep_len,
                 Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t i, j, pos;

    if (sep_len == 0) {
        return QP_SL_EMPTY_SEP;
    }
    else if (sep_len == 1)
        return STRINGLIB(split_char)(str, str_len, sep[0], maxcount, out);

    i = j = 0;
    while (maxcount-- > 0) {
        pos = FASTSEARCH(str+i, str_len-i, sep, sep_len, -1, FAST_SEARCH);
        if (pos < 0)
            break;
        j = i + pos;
        SPLIT_ADD(i, j);
        i = j + sep_len;
    }
    SPLIT_ADD(i, str_len);
    return 0;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(rsplit_char)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                       const STRINGLIB_CHAR ch,
                       Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t i, j;
    Py_ssize_t first = out->len;

    i = j = str_len - 1;
    while ((i >= 0) && (maxcount-- > 0)) {
        for(; i >= 0; i--) {
            if (str[i] == ch) {
                SPLIT_ADD(i + 1, j + 1);
                j = i = i - 1;
                break;
            }
        }
    }
    if (j >= -1) {
        SPLIT_ADD(0, j + 1);
    }
    /* CPython: PyList_Reverse(list) */
    qp_spanvec_reverse(out, first);
    return 0;
}

QP_SL_LOCAL_INLINE(int)
STRINGLIB(rsplit)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                  const STRINGLIB_CHAR* sep, Py_ssize_t sep_len,
                  Py_ssize_t maxcount, qp_spanvec *out)
{
    Py_ssize_t j, pos;
    Py_ssize_t first = out->len;

    if (sep_len == 0) {
        return QP_SL_EMPTY_SEP;
    }
    else if (sep_len == 1)
        return STRINGLIB(rsplit_char)(str, str_len, sep[0], maxcount, out);

    j = str_len;
    while (maxcount-- > 0) {
        pos = FASTSEARCH(str, j, sep, sep_len, -1, FAST_RSEARCH);
        if (pos < 0)
            break;
        SPLIT_ADD(pos + sep_len, j);
        j = pos;
    }
    SPLIT_ADD(0, j);
    /* CPython: PyList_Reverse(list) */
    qp_spanvec_reverse(out, first);
    return 0;
}

#endif /* !STRINGLIB_WS_ONLY */

#undef SPLIT_ADD
