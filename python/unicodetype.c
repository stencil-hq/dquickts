/*
 * Unicode character type helpers (character database queries).
 *
 * Derived from CPython Objects/unicodectype.c and Python/pyctype.c
 * (PSF-2.0); original unicodectype.c by Marc-Andre Lemburg, modified for
 * Python 2.0 by Fredrik Lundh. Copyright (c) Corporation for National
 * Research Initiatives.
 *
 * Every function is a pure lookup over the generated tables in
 * unicodetype_db.h (copied verbatim from CPython 3.15.2, so the Unicode
 * version matches exactly). None can fail; out-of-range code points
 * (>= 0x110000) map to the "unassigned" record. See unicodetype.h for the
 * API contract and how the str type must use these.
 */

#include "unicodetype.h"

#define ALPHA_MASK 0x01
#define DECIMAL_MASK 0x02
#define DIGIT_MASK 0x04
#define LOWER_MASK 0x08
#define TITLE_MASK 0x40
#define UPPER_MASK 0x80
#define XID_START_MASK 0x100
#define XID_CONTINUE_MASK 0x200
#define PRINTABLE_MASK 0x400
#define NUMERIC_MASK 0x800
#define CASE_IGNORABLE_MASK 0x1000
#define CASED_MASK 0x2000
#define EXTENDED_CASE_MASK 0x4000

typedef struct {
    /*
       These are either deltas to the character or offsets in
       _PyUnicode_ExtendedCase.
    */
    const int upper;
    const int lower;
    const int title;
    /* Note if more flag space is needed, decimal and digit could be unified. */
    const unsigned char decimal;
    const unsigned char digit;
    const unsigned short flags;
} _PyUnicode_TypeRecord;

/* The generated DB defines _PyUnicode_TypeRecords/_PyUnicode_ExtendedCase
   without `static`; it is included verbatim, so the arrays keep external
   linkage under their CPython-internal names (no libpython is linked, so no
   clash).  Its three functions (_PyUnicode_ToNumeric, _PyUnicode_IsWhitespace,
   _PyUnicode_IsLinebreak) get internal linkage from the static prototypes in
   unicodetype.h, which are visible before their definitions (C11 6.2.2p5). */
#include "unicodetype_db.h"

static const _PyUnicode_TypeRecord *
gettyperecord(Py_UCS4 code)
{
    int index;

    if (code >= 0x110000)
        index = 0;
    else
    {
        index = index1[(code>>SHIFT)];
        index = index2[(index<<SHIFT)+(code&((1<<SHIFT)-1))];
    }

    return &_PyUnicode_TypeRecords[index];
}

/* Returns the titlecase Unicode characters corresponding to ch or just
   ch if no titlecase mapping is known. */

static Py_UCS4 _PyUnicode_ToTitlecase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK)
        return _PyUnicode_ExtendedCase[ctype->title & 0xFFFF];
    return ch + ctype->title;
}

/* Returns 1 for Unicode characters having the category 'Lt', 0
   otherwise. */

static int _PyUnicode_IsTitlecase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & TITLE_MASK) != 0;
}

/* Returns 1 for Unicode characters having the XID_Start property, 0
   otherwise. */

static int _PyUnicode_IsXidStart(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & XID_START_MASK) != 0;
}

/* Returns 1 for Unicode characters having the XID_Continue property,
   0 otherwise. */

static int _PyUnicode_IsXidContinue(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & XID_CONTINUE_MASK) != 0;
}

/* Returns the integer decimal (0-9) for Unicode characters having
   this property, -1 otherwise. */

static int _PyUnicode_ToDecimalDigit(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & DECIMAL_MASK) ? ctype->decimal : -1;
}

static int _PyUnicode_IsDecimalDigit(Py_UCS4 ch)
{
    if (_PyUnicode_ToDecimalDigit(ch) < 0)
        return 0;
    return 1;
}

/* Returns the integer digit (0-9) for Unicode characters having
   this property, -1 otherwise. */

static int _PyUnicode_ToDigit(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & DIGIT_MASK) ? ctype->digit : -1;
}

static int _PyUnicode_IsDigit(Py_UCS4 ch)
{
    if (_PyUnicode_ToDigit(ch) < 0)
        return 0;
    return 1;
}

/* Returns the numeric value as double for Unicode characters having
   this property, -1.0 otherwise. */

static int _PyUnicode_IsNumeric(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & NUMERIC_MASK) != 0;
}

/* Returns 1 for Unicode characters that repr() may use in its output,
   and 0 for characters to be hex-escaped.

   See documentation of `str.isprintable` for details.
*/
static int _PyUnicode_IsPrintable(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & PRINTABLE_MASK) != 0;
}

/* Returns 1 for Unicode characters having the category 'Ll', 0
   otherwise. */

static int _PyUnicode_IsLowercase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & LOWER_MASK) != 0;
}

/* Returns 1 for Unicode characters having the category 'Lu', 0
   otherwise. */

static int _PyUnicode_IsUppercase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & UPPER_MASK) != 0;
}

/* Returns the uppercase Unicode characters corresponding to ch or just
   ch if no uppercase mapping is known. */

static Py_UCS4 _PyUnicode_ToUppercase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK)
        return _PyUnicode_ExtendedCase[ctype->upper & 0xFFFF];
    return ch + ctype->upper;
}

/* Returns the lowercase Unicode characters corresponding to ch or just
   ch if no lowercase mapping is known. */

static Py_UCS4 _PyUnicode_ToLowercase(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK)
        return _PyUnicode_ExtendedCase[ctype->lower & 0xFFFF];
    return ch + ctype->lower;
}

static int _PyUnicode_ToLowerFull(Py_UCS4 ch, Py_UCS4 *res)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK) {
        int index = ctype->lower & 0xFFFF;
        int n = ctype->lower >> 24;
        int i;
        for (i = 0; i < n; i++)
            res[i] = _PyUnicode_ExtendedCase[index + i];
        return n;
    }
    res[0] = ch + ctype->lower;
    return 1;
}

static int _PyUnicode_ToTitleFull(Py_UCS4 ch, Py_UCS4 *res)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK) {
        int index = ctype->title & 0xFFFF;
        int n = ctype->title >> 24;
        int i;
        for (i = 0; i < n; i++)
            res[i] = _PyUnicode_ExtendedCase[index + i];
        return n;
    }
    res[0] = ch + ctype->title;
    return 1;
}

static int _PyUnicode_ToUpperFull(Py_UCS4 ch, Py_UCS4 *res)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK) {
        int index = ctype->upper & 0xFFFF;
        int n = ctype->upper >> 24;
        int i;
        for (i = 0; i < n; i++)
            res[i] = _PyUnicode_ExtendedCase[index + i];
        return n;
    }
    res[0] = ch + ctype->upper;
    return 1;
}

static int _PyUnicode_ToFoldedFull(Py_UCS4 ch, Py_UCS4 *res)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    if (ctype->flags & EXTENDED_CASE_MASK && (ctype->lower >> 20) & 7) {
        int index = (ctype->lower & 0xFFFF) + (ctype->lower >> 24);
        int n = (ctype->lower >> 20) & 7;
        int i;
        for (i = 0; i < n; i++)
            res[i] = _PyUnicode_ExtendedCase[index + i];
        return n;
    }
    return _PyUnicode_ToLowerFull(ch, res);
}

static int _PyUnicode_IsCased(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & CASED_MASK) != 0;
}

static int _PyUnicode_IsCaseIgnorable(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & CASE_IGNORABLE_MASK) != 0;
}

/* Returns 1 for Unicode characters having the category 'Ll', 'Lu', 'Lt',
   'Lo' or 'Lm',  0 otherwise. */

static int _PyUnicode_IsAlpha(Py_UCS4 ch)
{
    const _PyUnicode_TypeRecord *ctype = gettyperecord(ch);

    return (ctype->flags & ALPHA_MASK) != 0;
}

/* ---- Locale-independent ASCII ctype tables (Python/pyctype.c) ---- */

/* Our own locale-independent ctype.h-like macros */

static const unsigned int _Py_ctype_table[256] = {
    0, /* 0x0 '\x00' */
    0, /* 0x1 '\x01' */
    0, /* 0x2 '\x02' */
    0, /* 0x3 '\x03' */
    0, /* 0x4 '\x04' */
    0, /* 0x5 '\x05' */
    0, /* 0x6 '\x06' */
    0, /* 0x7 '\x07' */
    0, /* 0x8 '\x08' */
    QP_CTF_SPACE, /* 0x9 '\t' */
    QP_CTF_SPACE, /* 0xa '\n' */
    QP_CTF_SPACE, /* 0xb '\v' */
    QP_CTF_SPACE, /* 0xc '\f' */
    QP_CTF_SPACE, /* 0xd '\r' */
    0, /* 0xe '\x0e' */
    0, /* 0xf '\x0f' */
    0, /* 0x10 '\x10' */
    0, /* 0x11 '\x11' */
    0, /* 0x12 '\x12' */
    0, /* 0x13 '\x13' */
    0, /* 0x14 '\x14' */
    0, /* 0x15 '\x15' */
    0, /* 0x16 '\x16' */
    0, /* 0x17 '\x17' */
    0, /* 0x18 '\x18' */
    0, /* 0x19 '\x19' */
    0, /* 0x1a '\x1a' */
    0, /* 0x1b '\x1b' */
    0, /* 0x1c '\x1c' */
    0, /* 0x1d '\x1d' */
    0, /* 0x1e '\x1e' */
    0, /* 0x1f '\x1f' */
    QP_CTF_SPACE, /* 0x20 ' ' */
    0, /* 0x21 '!' */
    0, /* 0x22 '"' */
    0, /* 0x23 '#' */
    0, /* 0x24 '$' */
    0, /* 0x25 '%' */
    0, /* 0x26 '&' */
    0, /* 0x27 "'" */
    0, /* 0x28 '(' */
    0, /* 0x29 ')' */
    0, /* 0x2a '*' */
    0, /* 0x2b '+' */
    0, /* 0x2c ',' */
    0, /* 0x2d '-' */
    0, /* 0x2e '.' */
    0, /* 0x2f '/' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x30 '0' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x31 '1' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x32 '2' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x33 '3' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x34 '4' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x35 '5' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x36 '6' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x37 '7' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x38 '8' */
    QP_CTF_DIGIT|QP_CTF_XDIGIT, /* 0x39 '9' */
    0, /* 0x3a ':' */
    0, /* 0x3b ';' */
    0, /* 0x3c '<' */
    0, /* 0x3d '=' */
    0, /* 0x3e '>' */
    0, /* 0x3f '?' */
    0, /* 0x40 '@' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x41 'A' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x42 'B' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x43 'C' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x44 'D' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x45 'E' */
    QP_CTF_UPPER|QP_CTF_XDIGIT, /* 0x46 'F' */
    QP_CTF_UPPER, /* 0x47 'G' */
    QP_CTF_UPPER, /* 0x48 'H' */
    QP_CTF_UPPER, /* 0x49 'I' */
    QP_CTF_UPPER, /* 0x4a 'J' */
    QP_CTF_UPPER, /* 0x4b 'K' */
    QP_CTF_UPPER, /* 0x4c 'L' */
    QP_CTF_UPPER, /* 0x4d 'M' */
    QP_CTF_UPPER, /* 0x4e 'N' */
    QP_CTF_UPPER, /* 0x4f 'O' */
    QP_CTF_UPPER, /* 0x50 'P' */
    QP_CTF_UPPER, /* 0x51 'Q' */
    QP_CTF_UPPER, /* 0x52 'R' */
    QP_CTF_UPPER, /* 0x53 'S' */
    QP_CTF_UPPER, /* 0x54 'T' */
    QP_CTF_UPPER, /* 0x55 'U' */
    QP_CTF_UPPER, /* 0x56 'V' */
    QP_CTF_UPPER, /* 0x57 'W' */
    QP_CTF_UPPER, /* 0x58 'X' */
    QP_CTF_UPPER, /* 0x59 'Y' */
    QP_CTF_UPPER, /* 0x5a 'Z' */
    0, /* 0x5b '[' */
    0, /* 0x5c '\\' */
    0, /* 0x5d ']' */
    0, /* 0x5e '^' */
    0, /* 0x5f '_' */
    0, /* 0x60 '`' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x61 'a' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x62 'b' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x63 'c' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x64 'd' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x65 'e' */
    QP_CTF_LOWER|QP_CTF_XDIGIT, /* 0x66 'f' */
    QP_CTF_LOWER, /* 0x67 'g' */
    QP_CTF_LOWER, /* 0x68 'h' */
    QP_CTF_LOWER, /* 0x69 'i' */
    QP_CTF_LOWER, /* 0x6a 'j' */
    QP_CTF_LOWER, /* 0x6b 'k' */
    QP_CTF_LOWER, /* 0x6c 'l' */
    QP_CTF_LOWER, /* 0x6d 'm' */
    QP_CTF_LOWER, /* 0x6e 'n' */
    QP_CTF_LOWER, /* 0x6f 'o' */
    QP_CTF_LOWER, /* 0x70 'p' */
    QP_CTF_LOWER, /* 0x71 'q' */
    QP_CTF_LOWER, /* 0x72 'r' */
    QP_CTF_LOWER, /* 0x73 's' */
    QP_CTF_LOWER, /* 0x74 't' */
    QP_CTF_LOWER, /* 0x75 'u' */
    QP_CTF_LOWER, /* 0x76 'v' */
    QP_CTF_LOWER, /* 0x77 'w' */
    QP_CTF_LOWER, /* 0x78 'x' */
    QP_CTF_LOWER, /* 0x79 'y' */
    QP_CTF_LOWER, /* 0x7a 'z' */
    0, /* 0x7b '{' */
    0, /* 0x7c '|' */
    0, /* 0x7d '}' */
    0, /* 0x7e '~' */
    0, /* 0x7f '\x7f' */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};


static const unsigned char _Py_ctype_tolower[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const unsigned char _Py_ctype_toupper[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};
