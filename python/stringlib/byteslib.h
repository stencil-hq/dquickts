/* Template parameters: bytes/bytearray data (one byte per item) with the
   bytes semantics of CPython's Objects/stringlib/stringdefs.h: whitespace is
   Py_ISSPACE (ASCII " \t\n\v\f\r") and line breaks are only '\n' and '\r'.
   Derived from CPython Objects/stringlib/stringdefs.h (PSF-2.0), v3.15.2.
   Only the whitespace/line-break dependent part of split.h is instantiated
   with it (STRINGLIB_WS_ONLY); search-based operations on bytes use ucs1lib,
   whose algorithms are byte-for-byte the ones CPython runs for bytes. */
#define STRINGLIB_IS_UNICODE     0

#define FASTSEARCH               byteslib_fastsearch
#define STRINGLIB(F)             byteslib_##F
#define STRINGLIB_SIZEOF_CHAR    1
#define STRINGLIB_MAX_CHAR       0xFFu
#define STRINGLIB_CHAR           Py_UCS1
#define STRINGLIB_ISSPACE(x)     QP_ISSPACE(x)
#define STRINGLIB_ISLINEBREAK(x) (((x) == '\n') || ((x) == '\r'))
#define STRINGLIB_FAST_MEMCHR    memchr
#define STRINGLIB_MUTABLE 0
#define STRINGLIB_WS_ONLY 1
