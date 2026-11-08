/*
 * Unicode character database queries and ASCII ctype macros for the Python
 * front end / runtime.
 *
 * Derived from CPython Include/cpython/unicodeobject.h,
 * Include/internal/pycore_unicodectype.h and Include/cpython/pyctype.h
 * (PSF-2.0).
 *
 * This file is the single place the Py_UCS1/2/4 typedefs live. Everything is
 * `static` (the python/ .c files are #included into quickjs.c). Implementation is in
 * unicodetype.c; data in unicodetype_db.h (verbatim CPython 3.15.2 copy).
 *
 * Usage contract for the str type (mirrors Objects/unicodeobject.c):
 *  - str.lower(): for each char use _PyUnicode_ToLowerFull(ch, buf) (buf of
 *    at least 3 Py_UCS4). The caller handles final sigma as do_lower/
 *    handle_capital_sigma do: for U+03A3, emit U+03C2 if the sigma is preceded
 *    by a cased char and not followed by one, skipping case-ignorable chars in
 *    both directions (_PyUnicode_IsCased/_PyUnicode_IsCaseIgnorable); else
 *    U+03C3.
 *  - str.upper(): _PyUnicode_ToUpperFull. str.casefold(): _PyUnicode_ToFoldedFull.
 *  - str.capitalize(): first char _PyUnicode_ToTitleFull, rest lower (with
 *    final sigma). str.title(): ToTitleFull after a non-cased char, else
 *    lower (with final sigma); "previous is cased" tracked via IsCased.
 *  - str.swapcase(): IsUppercase -> lower (with final sigma), IsLowercase ->
 *    ToUpperFull, else unchanged.
 *  - str.islower/isupper/istitle use IsLowercase/IsUppercase/IsTitlecase;
 *    str.isalpha uses IsAlpha; isdecimal IsDecimalDigit; isdigit IsDigit;
 *    isnumeric IsNumeric; isalnum = IsAlpha||IsDecimalDigit||IsDigit||
 *    IsNumeric (Py_UNICODE_ISALNUM); isspace IsWhitespace (ASCII fast path:
 *    _Py_ascii_whitespace in unicodeobject.c equals QP_ISSPACE plus
 *    0x1C-0x1F); isprintable/repr IsPrintable; isidentifier IsXidStart/
 *    IsXidContinue (plus '_' as start).
 *  - str.splitlines uses IsLinebreak; int()/float() parsing of non-ASCII
 *    digits uses ToDecimalDigit; unicodedata-like numeric uses ToNumeric.
 */
#ifndef QP_UNICODETYPE_H
#define QP_UNICODETYPE_H

#include <stdint.h>

/* CPython exports these names (C API, D5): the static implementations are
   renamed by macro so the exported wrappers of python/capi_unicode.c can
   use the CPython names (capi-design.md P1; python/capi.h #undefs these
   macros).  Users keep the CPython spelling. */
#define _PyUnicode_IsAlpha qp_unicode_is_alpha
#define _PyUnicode_IsDecimalDigit qp_unicode_is_decimal_digit
#define _PyUnicode_IsDigit qp_unicode_is_digit
#define _PyUnicode_IsLinebreak qp_unicode_is_linebreak
#define _PyUnicode_IsLowercase qp_unicode_is_lowercase
#define _PyUnicode_IsNumeric qp_unicode_is_numeric
#define _PyUnicode_IsPrintable qp_unicode_is_printable
#define _PyUnicode_IsTitlecase qp_unicode_is_titlecase
#define _PyUnicode_IsUppercase qp_unicode_is_uppercase
#define _PyUnicode_IsWhitespace qp_unicode_is_whitespace
#define _PyUnicode_IsXidStart qp_unicode_is_xid_start
#define _PyUnicode_IsXidContinue qp_unicode_is_xid_continue
#define _PyUnicode_ToDecimalDigit qp_unicode_to_decimal_digit
#define _PyUnicode_ToDigit qp_unicode_to_digit
#define _PyUnicode_ToLowercase qp_unicode_to_lowercase
#define _PyUnicode_ToNumeric qp_unicode_to_numeric
#define _PyUnicode_ToTitlecase qp_unicode_to_titlecase
#define _PyUnicode_ToUppercase qp_unicode_to_uppercase
#define _Py_ctype_table qp_ctype_table
#define _Py_ctype_tolower qp_ctype_tolower
#define _Py_ctype_toupper qp_ctype_toupper

typedef uint8_t Py_UCS1;
typedef uint16_t Py_UCS2;
typedef uint32_t Py_UCS4;

/* Simple (1:1) predicates: return nonzero iff ch has the property. */
static int _PyUnicode_IsLowercase(Py_UCS4 ch);      /* category Ll */
static int _PyUnicode_IsUppercase(Py_UCS4 ch);      /* category Lu */
static int _PyUnicode_IsTitlecase(Py_UCS4 ch);      /* category Lt */
static int _PyUnicode_IsXidStart(Py_UCS4 ch);       /* XID_Start */
static int _PyUnicode_IsXidContinue(Py_UCS4 ch);    /* XID_Continue */
static int _PyUnicode_IsWhitespace(const Py_UCS4 ch); /* bidi WS/B/S or Zs */
static int _PyUnicode_IsLinebreak(const Py_UCS4 ch);  /* BK/CR/LF/NL or bidi B */
static int _PyUnicode_IsDecimalDigit(Py_UCS4 ch);
static int _PyUnicode_IsDigit(Py_UCS4 ch);
static int _PyUnicode_IsNumeric(Py_UCS4 ch);
static int _PyUnicode_IsPrintable(Py_UCS4 ch);      /* repr() need not escape */
static int _PyUnicode_IsAlpha(Py_UCS4 ch);          /* Ll Lu Lt Lo Lm */
static int _PyUnicode_IsCased(Py_UCS4 ch);
static int _PyUnicode_IsCaseIgnorable(Py_UCS4 ch);

/* Simple case mappings: ch itself if no mapping. */
static Py_UCS4 _PyUnicode_ToLowercase(Py_UCS4 ch);
static Py_UCS4 _PyUnicode_ToUppercase(Py_UCS4 ch);
static Py_UCS4 _PyUnicode_ToTitlecase(Py_UCS4 ch);

/* Full (SpecialCasing/CaseFolding) mappings: write 1..3 code points to res
   (caller provides >= 3 slots) and return the count. No final-sigma or
   locale logic: that is the caller's job (see top comment). */
static int _PyUnicode_ToLowerFull(Py_UCS4 ch, Py_UCS4 *res);
static int _PyUnicode_ToUpperFull(Py_UCS4 ch, Py_UCS4 *res);
static int _PyUnicode_ToTitleFull(Py_UCS4 ch, Py_UCS4 *res);
static int _PyUnicode_ToFoldedFull(Py_UCS4 ch, Py_UCS4 *res);

/* Numeric values: digit 0..9 or -1; ToNumeric returns -1.0 if none. */
static int _PyUnicode_ToDecimalDigit(Py_UCS4 ch);
static int _PyUnicode_ToDigit(Py_UCS4 ch);
static double _PyUnicode_ToNumeric(Py_UCS4 ch);

/* Locale-independent ASCII ctype (pyctype.h). Argument must be a
   (signed/unsigned) char value; QP_CHARMASK maps it to 0..255. */
#define QP_CTF_LOWER  0x01
#define QP_CTF_UPPER  0x02
#define QP_CTF_ALPHA  (QP_CTF_LOWER|QP_CTF_UPPER)
#define QP_CTF_DIGIT  0x04
#define QP_CTF_ALNUM  (QP_CTF_ALPHA|QP_CTF_DIGIT)
#define QP_CTF_SPACE  0x08
#define QP_CTF_XDIGIT 0x10

#define QP_CHARMASK(c) ((unsigned char)((c) & 0xff))

static const unsigned int _Py_ctype_table[256];
static const unsigned char _Py_ctype_tolower[256];
static const unsigned char _Py_ctype_toupper[256];

#define QP_ISLOWER(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_LOWER)
#define QP_ISUPPER(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_UPPER)
#define QP_ISALPHA(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_ALPHA)
#define QP_ISDIGIT(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_DIGIT)
#define QP_ISXDIGIT(c) (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_XDIGIT)
#define QP_ISALNUM(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_ALNUM)
#define QP_ISSPACE(c)  (_Py_ctype_table[QP_CHARMASK(c)] & QP_CTF_SPACE)
#define QP_TOLOWER(c)  (_Py_ctype_tolower[QP_CHARMASK(c)])
#define QP_TOUPPER(c)  (_Py_ctype_toupper[QP_CHARMASK(c)])

#endif /* QP_UNICODETYPE_H */
