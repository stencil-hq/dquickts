/*
 * Number formatting, parsing, rounding and hashing for the Python runtime.
 *
 * Derived from CPython 3.15.2 Python/dtoa.c, Python/pystrtod.c,
 * Objects/unicode_formatter.c (formerly Python/formatter_unicode.c),
 * Objects/floatobject.c, Objects/complexobject.c, Objects/longobject.c,
 * Python/pyhash.c, Python/bootstrap_hash.c and Include/cpython/pyhash.h
 * (PSF-2.0; dtoa.c is also under David M. Gay's license, siphash13 under
 * the MIT license of csiphash; both notices are kept in numfmt.c).
 *
 * Everything here is object-model free: it works on doubles, ASCII/UTF-8
 * buffers, code-point arrays and QuickJS bigint limbs.  The python/ .c files are
 * #included into quickjs.c, so every function is `static`.
 *
 * Include order: numfmt.c includes compat.h (qp_error, Py_ssize_t),
 * unicodetype.h (_PyUnicode_ToDecimalDigit, used for width/precision
 * digits in format specs like CPython's Py_UNICODE_TODECIMAL) and this
 * header itself; unicodetype.c must also be part of the translation unit.
 * It needs QuickJS internals (DynBuf, js_limb_t, JS_LIMB_BITS), so it is
 * #included from quickpy.c like the other python/ .c files.
 *
 * Digit engine
 * ------------
 * numfmt.c contains a port of CPython's own Python/dtoa.c (_Py_dg_dtoa,
 * _Py_dg_strtod), not QuickJS's js_dtoa/js_atod.  Reason: CPython's
 * format/%/round results come from Gay's modes 2 and 3 with arbitrarily
 * many digits ('%.500f' % 0.1, '{:.1000e}', round(x, 323) all print/use
 * the exact decimal expansion), while js_dtoa caps FIXED/FRAC output at
 * JS_DTOA_MAX_DIGITS (101) digits and has no "digits after the point,
 * shortest if fewer" mode-3 contract with Gay's trailing-zero stripping.
 * _Py_dg_strtod is also what float() and round() feed back through, so a
 * single engine keeps repr/str/format/round/float() byte-identical to
 * CPython.
 *
 * Errors
 * ------
 * Functions that can fail take a `qp_error *err` (compat.h; pass a cleared
 * one: qp_error_set keeps the first error) and set it
 * with CPython's exception type and exact message: QP_ERR_VALUE
 * (ValueError), QP_ERR_OVERFLOW (OverflowError) or QP_ERR_MEMORY
 * (MemoryError, empty message).  Messages are UTF-8; text copied from a
 * format spec may contain lone surrogates, which are written as 3-byte
 * sequences (WTF-8, the form produced by unicode_to_utf8() in cutils.c).
 * Temporary memory comes from qp_sys_malloc/qp_sys_free (compat.h; libc,
 * not tied to a JSContext; CPython uses PyMem_Malloc).
 *
 * Text representation for format()
 * --------------------------------
 * Input strings (format specs, str values) are passed as
 *   (const void *data, int kind, size_t len)
 * with kind QP_FMT_KIND_LATIN1 (len uint8_t, 8-bit JSString), 
 * QP_FMT_KIND_UTF16 (len uint16_t, 16-bit JSString; a high surrogate
 * followed by a low surrogate is ONE code point, lone surrogates are code
 * points of their own) or QP_FMT_KIND_UCS4 (len uint32_t).  Lengths
 * passed in are in units of the representation; widths, precisions and
 * padding are always counted in code points as in CPython.
 * Output is appended to a DynBuf as WTF-8: UTF-8, with lone surrogates as
 * 3-byte sequences.  The str unit converts it to a JSString by decoding
 * with unicode_from_utf8() (cutils.c) and emitting UTF-16 (pairs for
 * code points >= 0x10000, a lone surrogate as itself).  Numeric output is
 * pure ASCII except for a non-ASCII fill character.  On error, the bytes
 * already appended to the DynBuf must be discarded by the caller (restore
 * the size it had before the call).
 *
 * Hashing
 * -------
 * 64-bit CPython semantics (PyHASH_BITS = 61) on every host.  See the
 * qp_hash_* declarations for the exact input each type unit must supply.
 */
#ifndef QP_NUMFMT_H
#define QP_NUMFMT_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------- */
/* double <-> string (Python/pystrtod.c) */

/* flags for qp_double_to_string (= Py_DTSF_*) */
#define QP_DTSF_SIGN      0x01 /* always add the sign */
#define QP_DTSF_ADD_DOT_0 0x02 /* if the result is an integer add ".0" */
#define QP_DTSF_ALT       0x04 /* "alternate" formatting ('#' flag) */
#define QP_DTSF_NO_NEG_0  0x08 /* negative zero result is coerced to 0 */

/* *type out values (= Py_DTST_*) */
#define QP_DTST_FINITE   0
#define QP_DTST_INFINITE 1
#define QP_DTST_NAN      2

/* PyOS_double_to_string(): format `val` with format_code 'e', 'E', 'f',
   'F', 'g', 'G' (C printf semantics with Python's twists: no '-' for nan,
   "inf"/"nan" or "INF"/"NAN") or 'r' (repr: shortest round-trip digits,
   exponent when the decimal exponent is < -4 or >= 16; precision must be
   0).  Returns a qp_sys_malloc'd NUL-terminated ASCII string (free with
   qp_sys_free()),
   or NULL with err set: ValueError "precision too big" for
   |precision| > INT_MAX - 1024, MemoryError.  An invalid format_code or a
   nonzero precision with 'r' is a caller bug (asserted; ValueError "bad
   argument to internal function" in release builds). */
static __maybe_unused char *qp_double_to_string(double val, char format_code, int precision,
                                                int flags, int *type, qp_error *err);

/* repr(float) == str(float): PyOS_double_to_string(x, 'r', 0,
   Py_DTSF_ADD_DOT_0).  Writes at most QP_FLOAT_REPR_MAX bytes including
   the terminating NUL into buf; returns the length, or -1 on memory
   exhaustion (caller raises MemoryError). */
#define QP_FLOAT_REPR_MAX 32
static __maybe_unused int qp_float_repr(double x, char *buf);

/* repr(complex) == str(complex) (complex_repr): "1j", "(1+2j)", "(-0-0j)",
   "(nan+infj)".  Appends ASCII to out; returns 0 or -1 on memory
   exhaustion (MemoryError). */
static __maybe_unused int qp_complex_repr(double re, double im, DynBuf *out);

/* _Py_parse_inf_or_nan(): parse [+-](inf|infinity|nan), case-insensitive.
   On success returns the value and sets *endptr past it; else returns
   -1.0 with *endptr = p. */
static __maybe_unused double _Py_parse_inf_or_nan(const char *p, char **endptr);

/* _PyOS_ascii_strtod(): correctly rounded strtod in the C locale over a
   NUL-terminated string: optional sign, digits with optional '.', optional
   exponent, or inf/nan (no leading whitespace, no hex, no underscores).
   *endptr = first unparsed char (= nptr if nothing parsed, then -1.0 is
   returned).  errno: ERANGE on overflow (±HUGE_VAL) or underflow (±0),
   ENOMEM on allocation failure (*endptr = nptr, result -1.0). */
static __maybe_unused double qp_ascii_strtod(const char *nptr, char **endptr);

/* PyOS_string_to_double(s, endptr, overflow_exception).  s must be
   NUL-terminated.  If endptr is NULL the whole string must parse; else
   as much as possible is parsed and *endptr set.  Errors (return -1.0,
   err set): ValueError "could not convert string to float: '%.200s'",
   MemoryError, and, when overflow_kind != QP_ERR_NONE, an error of that
   kind with "value too large to convert to float: '%.200s'" for
   |value| >= 1 out of range (with QP_ERR_NONE, ±inf is returned). */
static __maybe_unused double qp_string_to_double(const char *s, char **endptr,
                                                 qp_error_kind overflow_kind, qp_error *err);

/* _Py_string_to_number_with_underscores() helper: validate the underscore
   placement rule (an '_' only between two ASCII digits) of s[0..len) and
   copy it without underscores to dst (len + 1 bytes, NUL-terminated).
   Also rejects an embedded NUL.  Returns the new length, or -1 if the
   rule is violated (caller raises ValueError "could not convert string to
   <what>: %R" with repr of the original object). */
static __maybe_unused Py_ssize_t qp_number_strip_underscores(const char *s, size_t len, char *dst);

/* float(x) for a str/bytes argument (PyFloat_FromString after
   _PyUnicode_TransformDecimalAndSpaceToASCII, which the str unit does
   first: non-ASCII decimal digits -> ASCII digits, Unicode whitespace ->
   ' ', any other non-ASCII char -> '?').  s[0..len) with s[len] == '\0';
   leading/trailing ASCII whitespace stripped, underscores as in Python
   literals, inf/nan.  Returns 0 and *out on success, 1 if the text is not
   a float (caller raises ValueError "could not convert string to float:
   %R" with repr of the argument), -1 on memory exhaustion. */
static __maybe_unused int qp_float_from_string(const char *s, size_t len, double *out);

/* ---------------------------------------------------------------------- */
/* round() */

/* float.__round__(ndigits) for an int ndigits (already clipped to
   Py_ssize_t as PyNumber_AsSsize_t(o, NULL) does): nan/inf round to
   themselves, ndigits > 323 returns x, ndigits < -308 returns ±0.0, else
   correctly rounded (round-half-even on the exact decimal value) via
   _Py_dg_dtoa mode 3 and _Py_dg_strtod.  Returns 0 with *out set, or -1
   with err: OverflowError "rounded value too large to represent",
   MemoryError. */
static __maybe_unused int qp_double_round(double x, Py_ssize_t ndigits, double *out, qp_error *err);

/* round(x) / float.__round__(None) before PyLong_FromDouble: nearest
   integer, halfway cases to even.  The caller converts the result to int
   (OverflowError "cannot convert float infinity to integer" / ValueError
   "cannot convert float NaN to integer" come from that conversion). */
static __maybe_unused double qp_double_round_half_even(double x);

/* ---------------------------------------------------------------------- */
/* format() mini-language (Objects/unicode_formatter.c) */

#define QP_FMT_KIND_LATIN1 1
#define QP_FMT_KIND_UTF16  2
#define QP_FMT_KIND_UCS4   4

/* thousands separator selection (= enum LocaleType) */
enum {
    QP_LT_NO_LOCALE = 0,
    QP_LT_DEFAULT_LOCALE = ',',
    QP_LT_UNDERSCORE_LOCALE = '_',
    QP_LT_UNDER_FOUR_LOCALE,
    QP_LT_CURRENT_LOCALE,
};

/* Parsed standard format spec (= InternalFormatSpec).
   [[fill]align][sign]['z']['#']['0'][width][grouping]['.' [precision][frac grouping]][type] */
typedef struct qp_format_spec {
    uint32_t fill_char;
    uint32_t align;             /* '<' '>' '=' '^' */
    int alternate;              /* '#' */
    int no_neg_0;               /* 'z' */
    uint32_t sign;              /* '\0', '+', '-', ' ' */
    Py_ssize_t width;           /* -1 = not given */
    int thousands_separators;   /* QP_LT_* */
    Py_ssize_t precision;       /* -1 = not given */
    int frac_thousands_separator; /* QP_LT_NO/DEFAULT/UNDERSCORE_LOCALE */
    uint32_t type;              /* code point; '\0' = not given */
} qp_format_spec;

/* parse_internal_render_format_spec(): parse spec[0..spec_len) (kind as
   described above; must be non-empty: an empty spec means str(obj), which
   the caller does itself because str may be overridden by a subclass).
   default_type/default_align: 's','<' for str, 'd','>' for int, '\0','>'
   for float and complex.  type_name = Py_TYPE(obj)->tp_name (UTF-8) for
   the "Invalid format specifier" message.  Returns 0, or -1 with err set
   (ValueError with CPython's messages: "Too many decimal digits in format
   string", "Cannot specify both ',' and '_'.", "Format specifier missing
   precision", "Invalid format specifier '...' for object of type '...'",
   "Cannot specify ',' with 'c'." ...; MemoryError). */
static __maybe_unused int qp_format_spec_parse(qp_format_spec *format, const void *spec, int spec_kind,
                                               size_t spec_len, char default_type, char default_align,
                                               const char *type_name, qp_error *err);

/* What int.__format__ must do for a parsed spec
   (_PyLong_FormatAdvancedWriter's type switch). */
#define QP_FMT_LONG_DIGITS 0  /* b d n o x X: call qp_format_long with digits in *pbase */
#define QP_FMT_LONG_CHAR   1  /* c: call qp_format_long_char */
#define QP_FMT_LONG_FLOAT  2  /* e E f F g G %: convert with float(obj) (its OverflowError
                                 "int too large to convert to float" first), then
                                 qp_format_float_internal */
/* Returns one of the above (and the base 2/8/10/16 for DIGITS), or -1
   with ValueError "Unknown format code 'q' for object of type '...'". */
static __maybe_unused int qp_format_long_classify(const qp_format_spec *format, int *pbase,
                                                  const char *type_name, qp_error *err);

/* format_long_internal() for b d n o x X.  The int is given as its sign
   and the lowercase magnitude digits in the base returned by
   qp_format_long_classify (no sign, no prefix, no leading zeros except
   "0" itself), e.g. from QuickJS bigint toString(base).  Handles '#'
   prefixes 0b/0o/0x (0X for 'X'), grouping ',' '_' (every 4 digits for
   b/o/x/X), zero padding, alignment, sign.  'n' uses the C locale
   (CPython's default: LC_NUMERIC is never changed at startup), i.e. no
   grouping.  Errors: ValueError "Precision not allowed in integer format
   specifier", "Negative zero coercion (z) not allowed in integer format
   specifier"; MemoryError. */
static __maybe_unused int qp_format_long(DynBuf *out, const qp_format_spec *format, int negative,
                                         const char *digits, size_t n_digits, qp_error *err);

/* format_long_internal() for 'c'.  value_fits: the int fits a C long
   (int64); else OverflowError "Python int too large to convert to C long"
   is raised (after the spec checks, as PyLong_AsLong does in CPython).
   Errors: the two above, ValueError "Sign not allowed with integer format
   specifier 'c'", "Alternate form (#) not allowed with integer format
   specifier 'c'", OverflowError "%c arg not in range(0x110000)". */
static __maybe_unused int qp_format_long_char(DynBuf *out, const qp_format_spec *format,
                                              int64_t value, int value_fits, qp_error *err);

/* format_float_internal(): float formatting of a parsed spec whose type
   is '\0' e E f F g G n % (also used by int.__format__ after float()).
   Errors: ValueError "precision too big"; MemoryError. */
static __maybe_unused int qp_format_float_internal(DynBuf *out, const qp_format_spec *format,
                                                   double val, qp_error *err);

/* float.__format__(spec) for a non-empty spec (_PyFloat_FormatAdvancedWriter):
   parse + type check ("Unknown format code ...") + format. */
static __maybe_unused int qp_format_float(DynBuf *out, double val, const void *spec, int spec_kind,
                                          size_t spec_len, const char *type_name, qp_error *err);

/* complex.__format__(spec) for a non-empty spec
   (_PyComplex_FormatAdvancedWriter).  Extra errors: "Zero padding is not
   allowed in complex format specifier", "'=' alignment flag is not
   allowed in complex format specifier". */
static __maybe_unused int qp_format_complex(DynBuf *out, double re, double im, const void *spec,
                                            int spec_kind, size_t spec_len, const char *type_name,
                                            qp_error *err);

/* format_string_internal(): str formatting of value[0..value_len) (kind
   as above) with a parsed spec of type 's'.  Errors: "Sign not allowed in
   string format specifier", "Space not allowed in string format
   specifier", "Negative zero coercion (z) not allowed in string format
   specifier", "Alternate form (#) not allowed in string format
   specifier", "'=' alignment not allowed in string format specifier";
   MemoryError. */
static __maybe_unused int qp_format_string_internal(DynBuf *out, const qp_format_spec *format,
                                                    const void *value, int value_kind,
                                                    size_t value_len, qp_error *err);

/* str.__format__(spec) for a non-empty spec (_PyUnicode_FormatAdvancedWriter). */
static __maybe_unused int qp_format_str(DynBuf *out, const void *value, int value_kind,
                                        size_t value_len, const void *spec, int spec_kind,
                                        size_t spec_len, const char *type_name, qp_error *err);

/* ---------------------------------------------------------------------- */
/* Hashing (Python/pyhash.c, Include/cpython/pyhash.h) */

typedef int64_t qp_hash_t;    /* Py_hash_t on 64-bit hosts */
typedef uint64_t qp_uhash_t;  /* Py_uhash_t */

#define QP_HASH_MULTIPLIER 1000003UL  /* PyHASH_MULTIPLIER */
#define QP_HASH_BITS 61               /* PyHASH_BITS */
#define QP_HASH_MODULUS (((uint64_t)1 << QP_HASH_BITS) - 1) /* 2**61 - 1 */
#define QP_HASH_INF 314159            /* PyHASH_INF */
#define QP_HASH_IMAG QP_HASH_MULTIPLIER /* PyHASH_IMAG */

/* Numeric hashes: hash(x) = sign(x) * (|x| mod 2**61-1) for every rational
   x, with -1 mapped to -2, so 1 == 1.0 == True == 1+0j hash equally.
   None of them can fail. */

/* Py_HashPointer(): identity hash of an object address (rotate right by
   4 bits; -1 -> -2).  Also object.__hash__ for plain objects. */
static __maybe_unused qp_hash_t qp_hash_pointer(uintptr_t p);

/* _Py_HashDouble(): ±inf -> ±314159; nan -> qp_hash_pointer(identity)
   (CPython hashes nan by object identity: pass the address of the float's
   heap cell or, for an unboxed JS_TAG_FLOAT64 value that has no identity,
   any value the float unit documents); finite -> numeric hash. */
static __maybe_unused qp_hash_t qp_hash_double(double v, uintptr_t identity);

/* hash(int) for an int that fits int64_t (also bool: hash(True) == 1). */
static __maybe_unused qp_hash_t qp_hash_int64(int64_t v);

/* hash(int) for a sign + magnitude: limbs[0..n) little-endian base 2**32
   digits of |v| (leading zero limbs allowed, n == 0 means 0); negative
   is nonzero for v < 0.  This is the format the int unit supplies for
   big ints that are not already QuickJS bigints. */
static __maybe_unused qp_hash_t qp_hash_int_limbs32(int negative, const uint32_t *limbs, size_t n);

/* hash(int) straight from a QuickJS bigint: tab[0..len) is the two's
   complement little-endian limb array of JSBigInt (js_limb_t is 32 or 64
   bits depending on JS_LIMB_BITS), len >= 1. */
static __maybe_unused qp_hash_t qp_hash_int_js_limbs(const js_limb_t *tab, size_t len);

/* complex_hash(): hash(re) + 1000003 * hash(im) (wrapping), -1 -> -2.
   identity is used for nan parts as in qp_hash_double. */
static __maybe_unused qp_hash_t qp_hash_complex(double re, double im, uintptr_t identity);

/* Py_HashBuffer() with Python's default algorithm, SipHash-1-3, keyed by
   key[16] (k0 = little-endian key[0..8), k1 = key[8..16), i.e. the first
   16 bytes of CPython's _Py_HashSecret).  Empty buffer -> 0; -1 -> -2.
   This is hash(bytes) (and memoryview/bytearray-derived hashes). */
static __maybe_unused qp_hash_t qp_hash_buffer(const void *ptr, size_t len, const uint8_t key[16]);
/* _Py_KeyedHash(): raw SipHash-1-3 with k0 = key, k1 = 0 (no empty-input
   or -1 adjustment); _imp.source_hash. */
static __maybe_unused uint64_t qp_keyed_hash(uint64_t key, const void *ptr, size_t len);

/* hash(str).  CPython hashes the bytes of the string's canonical compact
   representation: UCS1 if every code point < 0x100, else UCS2 if every
   code point < 0x10000, else UCS4 (native = little-endian byte order).
   A Python str here is a JSString, so the str unit calls:
   - qp_hash_str_latin1 for an 8-bit JSString (bytes hashed as is);
   - qp_hash_str_utf16 for a 16-bit JSString: it computes the canonical
     form itself (narrowing to UCS1 when all units < 0x100, UCS4 when a
     surrogate PAIR is present, pairs combined into one code point, lone
     surrogates kept), so equal strs hash equally whatever the storage;
   - qp_hash_str_ucs4 for a UCS4 side buffer (also narrowed as needed).
   len is in units of the representation.  No allocation, cannot fail. */
static __maybe_unused qp_hash_t qp_hash_str_latin1(const uint8_t *s, size_t len, const uint8_t key[16]);
static __maybe_unused qp_hash_t qp_hash_str_utf16(const uint16_t *s, size_t len, const uint8_t key[16]);
static __maybe_unused qp_hash_t qp_hash_str_ucs4(const uint32_t *s, size_t len, const uint8_t key[16]);

/* Hash key seeding (_Py_HashRandomization_Init + config_init_hash_seed).
   The runtime calls qp_hash_key_init once with getenv("PYTHONHASHSEED")
   (or NULL with -E/-I) and stores the 16-byte key per JSRuntime:
   - NULL, "" or "random": random key (arc4random_buf on Apple/BSD,
     /dev/urandom elsewhere; a time/address mix if neither is available);
   - "0": all-zero key, CPython's PYTHONHASHSEED=0 ("disable the
     randomized hash"), giving the same str/bytes hashes as CPython;
   - "N" (strtoul, N <= 4294967295): CPython's lcg_urandom(N) bytes, i.e.
     the same hashes as CPython run with PYTHONHASHSEED=N.
   Returns 0, or -1 with ValueError 'PYTHONHASHSEED must be "random" or an
   integer in range [0; 4294967295]' (or "failed to get random numbers to
   initialize Python"). */
static __maybe_unused int qp_hash_key_init(const char *pythonhashseed, uint8_t key[16], qp_error *err);

/* key for a numeric seed (0 = all zeros, else lcg_urandom). */
static __maybe_unused void qp_hash_key_from_seed(uint32_t seed, uint8_t key[16]);

#endif /* QP_NUMFMT_H */
