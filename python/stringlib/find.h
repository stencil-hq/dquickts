/* stringlib: find/index implementation
 *
 * Derived from CPython Objects/stringlib/find.h (PSF-2.0), v3.15.2.
 * The PyObject-based contains_obj helper is omitted (buffer-level port).
 */

#ifndef STRINGLIB_FASTSEARCH_H
#error must include "stringlib/fastsearch.h" before including this module
#endif

QP_SL_LOCAL_INLINE(Py_ssize_t)
STRINGLIB(find)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
               const STRINGLIB_CHAR* sub, Py_ssize_t sub_len,
               Py_ssize_t offset)
{
    Py_ssize_t pos;

    assert(str_len >= 0);
    if (sub_len == 0)
        return offset;

    pos = FASTSEARCH(str, str_len, sub, sub_len, -1, FAST_SEARCH);

    if (pos >= 0)
        pos += offset;

    return pos;
}

QP_SL_LOCAL_INLINE(Py_ssize_t)
STRINGLIB(rfind)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                const STRINGLIB_CHAR* sub, Py_ssize_t sub_len,
                Py_ssize_t offset)
{
    Py_ssize_t pos;

    assert(str_len >= 0);
    if (sub_len == 0)
        return str_len + offset;

    pos = FASTSEARCH(str, str_len, sub, sub_len, -1, FAST_RSEARCH);

    if (pos >= 0)
        pos += offset;

    return pos;
}

QP_SL_LOCAL_INLINE(Py_ssize_t)
STRINGLIB(find_slice)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                     const STRINGLIB_CHAR* sub, Py_ssize_t sub_len,
                     Py_ssize_t start, Py_ssize_t end)
{
    return STRINGLIB(find)(str + start, end - start, sub, sub_len, start);
}

QP_SL_LOCAL_INLINE(Py_ssize_t)
STRINGLIB(rfind_slice)(const STRINGLIB_CHAR* str, Py_ssize_t str_len,
                      const STRINGLIB_CHAR* sub, Py_ssize_t sub_len,
                      Py_ssize_t start, Py_ssize_t end)
{
    return STRINGLIB(rfind)(str + start, end - start, sub, sub_len, start);
}
