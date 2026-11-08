/* Template parameters: str data of kind 2 (Py_UCS2), str semantics.
   Derived from CPython Objects/stringlib/ucs2lib.h (PSF-2.0), v3.15.2.
   Include fastsearch.h and the other templates after this, then undef.h. */
#define STRINGLIB_IS_UNICODE     1

#define FASTSEARCH               ucs2lib_fastsearch
#define STRINGLIB(F)             ucs2lib_##F
#define STRINGLIB_SIZEOF_CHAR    2
#define STRINGLIB_MAX_CHAR       0xFFFFu
#define STRINGLIB_CHAR           Py_UCS2
#define STRINGLIB_ISSPACE        QP_SL_UNICODE_ISSPACE
#define STRINGLIB_ISLINEBREAK    QP_SL_BLOOM_LINEBREAK
#define STRINGLIB_MUTABLE 0
#if QP_SL_SIZEOF_WCHAR_T == 2
#define STRINGLIB_FAST_MEMCHR(s, c, n)              \
    (Py_UCS2 *)wmemchr((const wchar_t *)(s), c, n)
#endif
