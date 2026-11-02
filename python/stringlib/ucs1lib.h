/* Template parameters: str data of kind 1 (Latin-1, Py_UCS1), str semantics.
   Derived from CPython Objects/stringlib/ucs1lib.h (PSF-2.0), v3.15.2.
   Include fastsearch.h and the other templates after this, then undef.h. */
#define STRINGLIB_IS_UNICODE     1

#define FASTSEARCH               ucs1lib_fastsearch
#define STRINGLIB(F)             ucs1lib_##F
#define STRINGLIB_SIZEOF_CHAR    1
#define STRINGLIB_MAX_CHAR       0xFFu
#define STRINGLIB_CHAR           Py_UCS1
#define STRINGLIB_ISSPACE        QP_SL_UNICODE_ISSPACE
#define STRINGLIB_ISLINEBREAK    QP_SL_BLOOM_LINEBREAK
#define STRINGLIB_FAST_MEMCHR    memchr
#define STRINGLIB_MUTABLE 0
