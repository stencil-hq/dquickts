/*
 * Number formatting, parsing, rounding and hashing for the Python runtime.
 * See numfmt.h for the API, the representations and the design notes.
 *
 * Derived from CPython 3.15.2 (PSF-2.0):
 *   Python/dtoa.c                 -> section 1 (also David M. Gay's license, kept below)
 *   Python/pystrtod.c             -> section 2
 *   Objects/floatobject.c         -> section 2 (float_repr, float_from_string_inner,
 *                                    double_round, float___round___impl)
 *   Objects/complexobject.c       -> section 2 (complex_repr), section 4 (complex_hash)
 *   Objects/unicode_formatter.c   -> section 3
 *   Python/pyhash.c, Include/cpython/pyhash.h, Include/internal/pycore_pyhash.h,
 *   Objects/longobject.c (long_hash), Python/bootstrap_hash.c,
 *   Python/initconfig.c (config_init_hash_seed) -> section 4
 *
 * Every function is static: this file is part of the quickjs.c translation
 * unit.  CPython public names are not used; internal names are kept where
 * they are unlikely to clash (_Py_dg_*, _Py_parse_inf_or_nan), the other
 * helpers carry a qp_ prefix.
 */

#include <float.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define QP_HAVE_ARC4RANDOM 1  /* arc4random_buf() in <stdlib.h> */
#endif

#include "compat.h"
#include "unicodetype.h"
#include "numfmt.h"

/* ====================================================================== */
/* Section 1: Python/dtoa.c                                               */
/* ====================================================================== */

/****************************************************************
 *
 * The author of this software is David M. Gay.
 *
 * Copyright (c) 1991, 2000, 2001 by Lucent Technologies.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR LUCENT MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 ***************************************************************/

/****************************************************************
 * This is dtoa.c by David M. Gay, downloaded from
 * http://www.netlib.org/fp/dtoa.c on April 15, 2009 and modified for
 * inclusion into the Python core by Mark E. T. Dickinson and Eric V. Smith.
 *
 * Please remember to check http://www.netlib.org/fp regularly (and especially
 * before any Python release) for bugfixes and updates.
 *
 * The major modifications from Gay's original code are as follows:
 *
 *  0. The original code has been specialized to Python's needs by removing
 *     many of the #ifdef'd sections.  In particular, code to support VAX and
 *     IBM floating-point formats, hex NaNs, hex floats, locale-aware
 *     treatment of the decimal point, and setting of the inexact flag have
 *     been removed.
 *
 *  1. We use PyMem_Malloc and PyMem_Free in place of malloc and free.
 *
 *  2. The public functions strtod, dtoa and freedtoa all now have
 *     a _Py_dg_ prefix.
 *
 *  3. Instead of assuming that PyMem_Malloc always succeeds, we thread
 *     PyMem_Malloc failures through the code.  The functions
 *
 *       Balloc, multadd, s2b, i2b, mult, pow5mult, lshift, diff, d2b
 *
 *     of return type *Bigint all return NULL to indicate a malloc failure.
 *     Similarly, rv_alloc and nrv_alloc (return type char *) return NULL on
 *     failure.  bigcomp now has return type int (it used to be void) and
 *     returns -1 on failure and 0 otherwise.  _Py_dg_dtoa returns NULL
 *     on failure.  _Py_dg_strtod indicates failure due to malloc failure
 *     by returning -1.0, setting errno=ENOMEM and *se to s00.
 *
 *  4. The static variable dtoa_result has been removed.  Callers of
 *     _Py_dg_dtoa are expected to call _Py_dg_freedtoa to free
 *     the memory allocated by _Py_dg_dtoa.
 *
 *  5. The code has been reformatted to better fit with Python's
 *     C style guide (PEP 7).
 *
 *  6. A bug in the memory allocation has been fixed: to avoid FREEing memory
 *     that hasn't been MALLOC'ed, private_mem should only be used when k <=
 *     Kmax.
 *
 *  7. _Py_dg_strtod has been modified so that it doesn't accept strings with
 *     leading whitespace.
 *
 *  8. A corner case where _Py_dg_dtoa didn't strip trailing zeros has been
 *     fixed. (bugs.python.org/issue40780)
 *
 *  9. _Py_dg_dtoa clamps ndigits in modes 3 and 5 so that its buffer size
 *     arithmetic cannot exceed the int range, and rv_alloc's size doubling
 *     uses size_t. (gh-158446)
 *
 ***************************************************************/

/* Please send bug reports for the original dtoa.c code to David M. Gay (dmg
 * at acm dot org, with " at " changed at "@" and " dot " changed to ".").
 * Please report bugs for this modified version using the Python issue tracker
 * as detailed at (https://devguide.python.org/triage/issue-tracker/). */

/* On a machine with IEEE extended-precision registers, it is
 * necessary to specify double-precision (53-bit) rounding precision
 * before invoking strtod or dtoa.  If the machine uses (the equivalent
 * of) Intel 80x87 arithmetic, the call
 *      _control87(PC_53, MCW_PC);
 * does this with many compilers.  Whether this or another call is
 * appropriate depends on the compiler; for this to work, it may be
 * necessary to #include "float.h" or another system-dependent header
 * file.
 */

/* strtod for IEEE-, VAX-, and IBM-arithmetic machines.
 *
 * This strtod returns a nearest machine number to the input decimal
 * string (or sets errno to ERANGE).  With IEEE arithmetic, ties are
 * broken by the IEEE round-even rule.  Otherwise ties are broken by
 * biased rounding (add half and chop).
 *
 * Inspired loosely by William D. Clinger's paper "How to Read Floating
 * Point Numbers Accurately" [Proc. ACM SIGPLAN '90, pp. 92-101].
 *
 * Modifications:
 *
 *      1. We only require IEEE, IBM, or VAX double-precision
 *              arithmetic (not IEEE double-extended).
 *      2. We get by with floating-point arithmetic in a case that
 *              Clinger missed -- when we're computing d * 10^n
 *              for a small integer d and the integer n is not too
 *              much larger than 22 (the maximum integer k for which
 *              we can represent 10^k exactly), we may be able to
 *              compute (d*10^k) * 10^(e-k) with just one roundoff.
 *      3. Rather than a bit-at-a-time adjustment of the binary
 *              result in the hard case, we use floating-point
 *              arithmetic to determine the adjustment to within
 *              one bit; only in really hard cases do we need to
 *              compute a second residual.
 *      4. Because of 3., we don't need a large table of powers of 10
 *              for ten-to-e (just some small tables, e.g. of 10^k
 *              for 0 <= k <= 22).
 */

/* dquickts (numfmt.c) changes to the CPython 3.15.2 copy:
 *
 *  10. Python.h and the interpreter state are gone.  Bigints are allocated
 *      with qp_sys_malloc/qp_sys_free (libc; the variant CPython builds with
 *      Py_USING_MEMORY_DEBUGGER / Py_GIL_DISABLED), and pow5mult never caches
 *      powers of 5, so the code has no global or per-runtime state and is
 *      safe to call from any thread.  Results are bit-for-bit those of the
 *      caching variant.
 *  11. Every file-scope name of this section is mapped by a macro to a
 *      qp_dg_ prefixed name and every macro is #undef'd at the end of the
 *      section (this file is part of the quickjs.c translation unit).
 *  12. The DEBUG checks are keyed on QP_DTOA_DEBUG instead of Py_DEBUG.
 *  13. _Py_SET_53BIT_PRECISION_* are not needed: QuickJS only supports
 *      targets with IEEE double arithmetic in 53-bit precision (SSE2,
 *      AArch64, ...).
 */

/* Linking of Python's #defines to Gay's #defines starts here. */

#define Balloc qp_dg_Balloc
#define Bfree qp_dg_Bfree
#define multadd qp_dg_multadd
#define s2b qp_dg_s2b
#define hi0bits qp_dg_hi0bits
#define lo0bits qp_dg_lo0bits
#define i2b qp_dg_i2b
#define mult qp_dg_mult
#define pow5mult qp_dg_pow5mult
#define lshift qp_dg_lshift
#define cmp qp_dg_cmp
#define diff qp_dg_diff
#define ulp qp_dg_ulp
#define b2d qp_dg_b2d
#define sd2b qp_dg_sd2b
#define d2b qp_dg_d2b
#define ratio qp_dg_ratio
#define dshift qp_dg_dshift
#define quorem qp_dg_quorem
#define sulp qp_dg_sulp
#define bigcomp qp_dg_bigcomp
#define rv_alloc qp_dg_rv_alloc
#define nrv_alloc qp_dg_nrv_alloc
#define tens qp_dg_tens
#define bigtens qp_dg_bigtens
#define tinytens qp_dg_tinytens
#define U qp_dg_U
#define Long qp_dg_Long
#define ULong qp_dg_ULong
#define ULLong qp_dg_ULLong
#define Bigint qp_dg_Bigint
#define BCinfo qp_dg_BCinfo

#define MALLOC qp_sys_malloc
#define FREE qp_sys_free

/* This code should also work for ARM mixed-endian format on little-endian
   machines, where doubles have byte order 45670123 (in increasing address
   order, 0 being the least significant byte). */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define IEEE_MC68k
#else
#  define IEEE_8087
#endif

typedef uint32_t ULong;
typedef int32_t Long;
typedef uint64_t ULLong;

/* End Python #define linking */

#ifdef QP_DTOA_DEBUG
#define Bug(x) {fprintf(stderr, "%s\n", x); exit(1);}
#endif


typedef union { double d; ULong L[2]; } U;

#ifdef IEEE_8087
#define word0(x) (x)->L[1]
#define word1(x) (x)->L[0]
#else
#define word0(x) (x)->L[0]
#define word1(x) (x)->L[1]
#endif
#define dval(x) (x)->d

#ifndef STRTOD_DIGLIM
#define STRTOD_DIGLIM 40
#endif

/* maximum permitted exponent value for strtod; exponents larger than
   MAX_ABS_EXP in absolute value get truncated to +-MAX_ABS_EXP.  MAX_ABS_EXP
   should fit into an int. */
#ifndef MAX_ABS_EXP
#define MAX_ABS_EXP 1100000000U
#endif
/* Bound on length of pieces of input strings in _Py_dg_strtod; specifically,
   this is used to bound the total number of digits ignoring leading zeros and
   the number of digits that follow the decimal point.  Ideally, MAX_DIGITS
   should satisfy MAX_DIGITS + 400 < MAX_ABS_EXP; that ensures that the
   exponent clipping in _Py_dg_strtod can't affect the value of the output. */
#ifndef MAX_DIGITS
#define MAX_DIGITS 1000000000U
#endif

/* Guard against trying to use the above values on unusual platforms with ints
 * of width less than 32 bits. */
#if MAX_ABS_EXP > INT_MAX
#error "MAX_ABS_EXP should fit in an int"
#endif
#if MAX_DIGITS > INT_MAX
#error "MAX_DIGITS should fit in an int"
#endif

/* The following definition of Storeinc is appropriate for MIPS processors.
 * An alternative that might be better on some machines is
 * #define Storeinc(a,b,c) (*a++ = b << 16 | c & 0xffff)
 */
#if defined(IEEE_8087)
#define Storeinc(a,b,c) (((unsigned short *)a)[1] = (unsigned short)b,  \
                         ((unsigned short *)a)[0] = (unsigned short)c, a++)
#else
#define Storeinc(a,b,c) (((unsigned short *)a)[0] = (unsigned short)b,  \
                         ((unsigned short *)a)[1] = (unsigned short)c, a++)
#endif

/* #define P DBL_MANT_DIG */
/* Ten_pmax = floor(P*log(2)/log(5)) */
/* Bletch = (highest power of 2 < DBL_MAX_10_EXP) / 16 */
/* Quick_max = floor((P-1)*log(FLT_RADIX)/log(10) - 1) */
/* Int_max = floor(P*log(FLT_RADIX)/log(10) - 1) */

#define Exp_shift  20
#define Exp_shift1 20
#define Exp_msk1    0x100000
#define Exp_msk11   0x100000
#define Exp_mask  0x7ff00000
#define P 53
#define Nbits 53
#define Bias 1023
#define Emax 1023
#define Emin (-1022)
#define Etiny (-1074)  /* smallest denormal is 2**Etiny */
#define Exp_1  0x3ff00000
#define Exp_11 0x3ff00000
#define Ebits 11
#define Frac_mask  0xfffff
#define Frac_mask1 0xfffff
#define Ten_pmax 22
#define Bletch 0x10
#define Bndry_mask  0xfffff
#define Bndry_mask1 0xfffff
#define Sign_bit 0x80000000
#define Log2P 1
#define Tiny0 0
#define Tiny1 1
#define Quick_max 14
#define Int_max 14

#ifndef Flt_Rounds
#ifdef FLT_ROUNDS
#define Flt_Rounds FLT_ROUNDS
#else
#define Flt_Rounds 1
#endif
#endif /*Flt_Rounds*/

#define Rounding Flt_Rounds

#define Big0 (Frac_mask1 | Exp_msk1*(DBL_MAX_EXP+Bias-1))
#define Big1 0xffffffff

/* Bits of the representation of positive infinity. */

#define POSINF_WORD0 0x7ff00000
#define POSINF_WORD1 0

/* struct BCinfo is used to pass information from _Py_dg_strtod to bigcomp */

typedef struct BCinfo BCinfo;
struct
BCinfo {
    int e0, nd, nd0, scale;
};

#define FFFFFFFF 0xffffffffUL

/* struct Bigint is used to represent arbitrary-precision integers.  These
   integers are stored in sign-magnitude format, with the magnitude stored as
   an array of base 2**32 digits.  Bigints are always normalized: if x is a
   Bigint then x->wds >= 1, and either x->wds == 1 or x[wds-1] is nonzero.

   The Bigint fields are as follows:

     - next is a header used by Balloc and Bfree to keep track of lists
         of freed Bigints;  it's also used for the linked list of
         powers of 5 of the form 5**2**i used by pow5mult.
     - k indicates which pool this Bigint was allocated from
     - maxwds is the maximum number of words space was allocated for
       (usually maxwds == 2**k)
     - sign is 1 for negative Bigints, 0 for positive.  The sign is unused
       (ignored on inputs, set to 0 on outputs) in almost all operations
       involving Bigints: a notable exception is the diff function, which
       ignores signs on inputs but sets the sign of the output correctly.
     - wds is the actual number of significant words
     - x contains the vector of words (digits) for this Bigint, from least
       significant (x[0]) to most significant (x[wds-1]).
*/

/* struct Bigint (pycore_interp_structs.h in CPython) */
struct
Bigint {
    struct Bigint *next;
    int k, maxwds, sign, wds;
    ULong x[1];
};


typedef struct Bigint Bigint;


/* Alternative versions of Balloc and Bfree that use PyMem_Malloc and
   PyMem_Free directly in place of the custom memory allocation scheme above.
   These are provided for the benefit of memory debugging tools like
   Valgrind. */

/* Allocate space for a Bigint with up to 1<<k digits */

static Bigint *
Balloc(int k)
{
    int x;
    Bigint *rv;
    unsigned int len;

    x = 1 << k;
    len = (sizeof(Bigint) + (x-1)*sizeof(ULong) + sizeof(double) - 1)
        /sizeof(double);

    rv = (Bigint*)MALLOC(len*sizeof(double));
    if (rv == NULL)
        return NULL;

    rv->k = k;
    rv->maxwds = x;
    rv->sign = rv->wds = 0;
    return rv;
}

/* Free a Bigint allocated with Balloc */

static void
Bfree(Bigint *v)
{
    if (v) {
        FREE((void*)v);
    }
}


#define Bcopy(x,y) memcpy((char *)&x->sign, (char *)&y->sign,   \
                          y->wds*sizeof(Long) + 2*sizeof(int))

/* Multiply a Bigint b by m and add a.  Either modifies b in place and returns
   a pointer to the modified b, or Bfrees b and returns a pointer to a copy.
   On failure, return NULL.  In this case, b will have been already freed. */

static Bigint *
multadd(Bigint *b, int m, int a)       /* multiply by m and add a */
{
    int i, wds;
    ULong *x;
    ULLong carry, y;
    Bigint *b1;

    wds = b->wds;
    x = b->x;
    i = 0;
    carry = a;
    do {
        y = *x * (ULLong)m + carry;
        carry = y >> 32;
        *x++ = (ULong)(y & FFFFFFFF);
    }
    while(++i < wds);
    if (carry) {
        if (wds >= b->maxwds) {
            b1 = Balloc(b->k+1);
            if (b1 == NULL){
                Bfree(b);
                return NULL;
            }
            Bcopy(b1, b);
            Bfree(b);
            b = b1;
        }
        b->x[wds++] = (ULong)carry;
        b->wds = wds;
    }
    return b;
}

/* convert a string s containing nd decimal digits (possibly containing a
   decimal separator at position nd0, which is ignored) to a Bigint.  This
   function carries on where the parsing code in _Py_dg_strtod leaves off: on
   entry, y9 contains the result of converting the first 9 digits.  Returns
   NULL on failure. */

static Bigint *
s2b(const char *s, int nd0, int nd, ULong y9)
{
    Bigint *b;
    int i, k;
    Long x, y;

    x = (nd + 8) / 9;
    for(k = 0, y = 1; x > y; y <<= 1, k++) ;
    b = Balloc(k);
    if (b == NULL)
        return NULL;
    b->x[0] = y9;
    b->wds = 1;

    if (nd <= 9)
      return b;

    s += 9;
    for (i = 9; i < nd0; i++) {
        b = multadd(b, 10, *s++ - '0');
        if (b == NULL)
            return NULL;
    }
    s++;
    for(; i < nd; i++) {
        b = multadd(b, 10, *s++ - '0');
        if (b == NULL)
            return NULL;
    }
    return b;
}

/* count leading 0 bits in the 32-bit integer x. */

static int
hi0bits(ULong x)
{
    int k = 0;

    if (!(x & 0xffff0000)) {
        k = 16;
        x <<= 16;
    }
    if (!(x & 0xff000000)) {
        k += 8;
        x <<= 8;
    }
    if (!(x & 0xf0000000)) {
        k += 4;
        x <<= 4;
    }
    if (!(x & 0xc0000000)) {
        k += 2;
        x <<= 2;
    }
    if (!(x & 0x80000000)) {
        k++;
        if (!(x & 0x40000000))
            return 32;
    }
    return k;
}

/* count trailing 0 bits in the 32-bit integer y, and shift y right by that
   number of bits. */

static int
lo0bits(ULong *y)
{
    int k;
    ULong x = *y;

    if (x & 7) {
        if (x & 1)
            return 0;
        if (x & 2) {
            *y = x >> 1;
            return 1;
        }
        *y = x >> 2;
        return 2;
    }
    k = 0;
    if (!(x & 0xffff)) {
        k = 16;
        x >>= 16;
    }
    if (!(x & 0xff)) {
        k += 8;
        x >>= 8;
    }
    if (!(x & 0xf)) {
        k += 4;
        x >>= 4;
    }
    if (!(x & 0x3)) {
        k += 2;
        x >>= 2;
    }
    if (!(x & 1)) {
        k++;
        x >>= 1;
        if (!x)
            return 32;
    }
    *y = x;
    return k;
}

/* convert a small nonnegative integer to a Bigint */

static Bigint *
i2b(int i)
{
    Bigint *b;

    b = Balloc(1);
    if (b == NULL)
        return NULL;
    b->x[0] = i;
    b->wds = 1;
    return b;
}

/* multiply two Bigints.  Returns a new Bigint, or NULL on failure.  Ignores
   the signs of a and b. */

static Bigint *
mult(Bigint *a, Bigint *b)
{
    Bigint *c;
    int k, wa, wb, wc;
    ULong *x, *xa, *xae, *xb, *xbe, *xc, *xc0;
    ULong y;
    ULLong carry, z;

    if ((!a->x[0] && a->wds == 1) || (!b->x[0] && b->wds == 1)) {
        c = Balloc(0);
        if (c == NULL)
            return NULL;
        c->wds = 1;
        c->x[0] = 0;
        return c;
    }

    if (a->wds < b->wds) {
        c = a;
        a = b;
        b = c;
    }
    k = a->k;
    wa = a->wds;
    wb = b->wds;
    wc = wa + wb;
    if (wc > a->maxwds)
        k++;
    c = Balloc(k);
    if (c == NULL)
        return NULL;
    for(x = c->x, xa = x + wc; x < xa; x++)
        *x = 0;
    xa = a->x;
    xae = xa + wa;
    xb = b->x;
    xbe = xb + wb;
    xc0 = c->x;
    for(; xb < xbe; xc0++) {
        if ((y = *xb++)) {
            x = xa;
            xc = xc0;
            carry = 0;
            do {
                z = *x++ * (ULLong)y + *xc + carry;
                carry = z >> 32;
                *xc++ = (ULong)(z & FFFFFFFF);
            }
            while(x < xae);
            *xc = (ULong)carry;
        }
    }
    for(xc0 = c->x, xc = xc0 + wc; wc > 0 && !*--xc; --wc) ;
    c->wds = wc;
    return c;
}


/* Version of pow5mult that doesn't cache powers of 5. Provided for
   the benefit of memory debugging tools like Valgrind. */

static Bigint *
pow5mult(Bigint *b, int k)
{
    Bigint *b1, *p5, *p51;
    int i;
    static const int p05[3] = { 5, 25, 125 };

    if ((i = k & 3)) {
        b = multadd(b, p05[i-1], 0);
        if (b == NULL)
            return NULL;
    }

    if (!(k >>= 2))
        return b;
    p5 = i2b(625);
    if (p5 == NULL) {
        Bfree(b);
        return NULL;
    }

    for(;;) {
        if (k & 1) {
            b1 = mult(b, p5);
            Bfree(b);
            b = b1;
            if (b == NULL) {
                Bfree(p5);
                return NULL;
            }
        }
        if (!(k >>= 1))
            break;
        p51 = mult(p5, p5);
        Bfree(p5);
        p5 = p51;
        if (p5 == NULL) {
            Bfree(b);
            return NULL;
        }
    }
    Bfree(p5);
    return b;
}


/* shift a Bigint b left by k bits.  Return a pointer to the shifted result,
   or NULL on failure.  If the returned pointer is distinct from b then the
   original b will have been Bfree'd.   Ignores the sign of b. */

static Bigint *
lshift(Bigint *b, int k)
{
    int i, k1, n, n1;
    Bigint *b1;
    ULong *x, *x1, *xe, z;

    if (!k || (!b->x[0] && b->wds == 1))
        return b;

    n = k >> 5;
    k1 = b->k;
    n1 = n + b->wds + 1;
    for(i = b->maxwds; n1 > i; i <<= 1)
        k1++;
    b1 = Balloc(k1);
    if (b1 == NULL) {
        Bfree(b);
        return NULL;
    }
    x1 = b1->x;
    for(i = 0; i < n; i++)
        *x1++ = 0;
    x = b->x;
    xe = x + b->wds;
    if (k &= 0x1f) {
        k1 = 32 - k;
        z = 0;
        do {
            *x1++ = *x << k | z;
            z = *x++ >> k1;
        }
        while(x < xe);
        if ((*x1 = z))
            ++n1;
    }
    else do
             *x1++ = *x++;
        while(x < xe);
    b1->wds = n1 - 1;
    Bfree(b);
    return b1;
}

/* Do a three-way compare of a and b, returning -1 if a < b, 0 if a == b and
   1 if a > b.  Ignores signs of a and b. */

static int
cmp(Bigint *a, Bigint *b)
{
    ULong *xa, *xa0, *xb, *xb0;
    int i, j;

    i = a->wds;
    j = b->wds;
#ifdef QP_DTOA_DEBUG
    if (i > 1 && !a->x[i-1])
        Bug("cmp called with a->x[a->wds-1] == 0");
    if (j > 1 && !b->x[j-1])
        Bug("cmp called with b->x[b->wds-1] == 0");
#endif
    if (i -= j)
        return i;
    xa0 = a->x;
    xa = xa0 + j;
    xb0 = b->x;
    xb = xb0 + j;
    for(;;) {
        if (*--xa != *--xb)
            return *xa < *xb ? -1 : 1;
        if (xa <= xa0)
            break;
    }
    return 0;
}

/* Take the difference of Bigints a and b, returning a new Bigint.  Returns
   NULL on failure.  The signs of a and b are ignored, but the sign of the
   result is set appropriately. */

static Bigint *
diff(Bigint *a, Bigint *b)
{
    Bigint *c;
    int i, wa, wb;
    ULong *xa, *xae, *xb, *xbe, *xc;
    ULLong borrow, y;

    i = cmp(a,b);
    if (!i) {
        c = Balloc(0);
        if (c == NULL)
            return NULL;
        c->wds = 1;
        c->x[0] = 0;
        return c;
    }
    if (i < 0) {
        c = a;
        a = b;
        b = c;
        i = 1;
    }
    else
        i = 0;
    c = Balloc(a->k);
    if (c == NULL)
        return NULL;
    c->sign = i;
    wa = a->wds;
    xa = a->x;
    xae = xa + wa;
    wb = b->wds;
    xb = b->x;
    xbe = xb + wb;
    xc = c->x;
    borrow = 0;
    do {
        y = (ULLong)*xa++ - *xb++ - borrow;
        borrow = y >> 32 & (ULong)1;
        *xc++ = (ULong)(y & FFFFFFFF);
    }
    while(xb < xbe);
    while(xa < xae) {
        y = *xa++ - borrow;
        borrow = y >> 32 & (ULong)1;
        *xc++ = (ULong)(y & FFFFFFFF);
    }
    while(!*--xc)
        wa--;
    c->wds = wa;
    return c;
}

/* Given a positive normal double x, return the difference between x and the
   next double up.  Doesn't give correct results for subnormals. */

static double
ulp(U *x)
{
    Long L;
    U u;

    L = (word0(x) & Exp_mask) - (P-1)*Exp_msk1;
    word0(&u) = L;
    word1(&u) = 0;
    return dval(&u);
}

/* Convert a Bigint to a double plus an exponent */

static double
b2d(Bigint *a, int *e)
{
    ULong *xa, *xa0, w, y, z;
    int k;
    U d;

    xa0 = a->x;
    xa = xa0 + a->wds;
    y = *--xa;
#ifdef QP_DTOA_DEBUG
    if (!y) Bug("zero y in b2d");
#endif
    k = hi0bits(y);
    *e = 32 - k;
    if (k < Ebits) {
        word0(&d) = Exp_1 | y >> (Ebits - k);
        w = xa > xa0 ? *--xa : 0;
        word1(&d) = y << ((32-Ebits) + k) | w >> (Ebits - k);
        goto ret_d;
    }
    z = xa > xa0 ? *--xa : 0;
    if (k -= Ebits) {
        word0(&d) = Exp_1 | y << k | z >> (32 - k);
        y = xa > xa0 ? *--xa : 0;
        word1(&d) = z << k | y >> (32 - k);
    }
    else {
        word0(&d) = Exp_1 | y;
        word1(&d) = z;
    }
  ret_d:
    return dval(&d);
}

/* Convert a scaled double to a Bigint plus an exponent.  Similar to d2b,
   except that it accepts the scale parameter used in _Py_dg_strtod (which
   should be either 0 or 2*P), and the normalization for the return value is
   different (see below).  On input, d should be finite and nonnegative, and d
   / 2**scale should be exactly representable as an IEEE 754 double.

   Returns a Bigint b and an integer e such that

     dval(d) / 2**scale = b * 2**e.

   Unlike d2b, b is not necessarily odd: b and e are normalized so
   that either 2**(P-1) <= b < 2**P and e >= Etiny, or b < 2**P
   and e == Etiny.  This applies equally to an input of 0.0: in that
   case the return values are b = 0 and e = Etiny.

   The above normalization ensures that for all possible inputs d,
   2**e gives ulp(d/2**scale).

   Returns NULL on failure.
*/

static Bigint *
sd2b(U *d, int scale, int *e)
{
    Bigint *b;

    b = Balloc(1);
    if (b == NULL)
        return NULL;

    /* First construct b and e assuming that scale == 0. */
    b->wds = 2;
    b->x[0] = word1(d);
    b->x[1] = word0(d) & Frac_mask;
    *e = Etiny - 1 + (int)((word0(d) & Exp_mask) >> Exp_shift);
    if (*e < Etiny)
        *e = Etiny;
    else
        b->x[1] |= Exp_msk1;

    /* Now adjust for scale, provided that b != 0. */
    if (scale && (b->x[0] || b->x[1])) {
        *e -= scale;
        if (*e < Etiny) {
            scale = Etiny - *e;
            *e = Etiny;
            /* We can't shift more than P-1 bits without shifting out a 1. */
            assert(0 < scale && scale <= P - 1);
            if (scale >= 32) {
                /* The bits shifted out should all be zero. */
                assert(b->x[0] == 0);
                b->x[0] = b->x[1];
                b->x[1] = 0;
                scale -= 32;
            }
            if (scale) {
                /* The bits shifted out should all be zero. */
                assert(b->x[0] << (32 - scale) == 0);
                b->x[0] = (b->x[0] >> scale) | (b->x[1] << (32 - scale));
                b->x[1] >>= scale;
            }
        }
    }
    /* Ensure b is normalized. */
    if (!b->x[1])
        b->wds = 1;

    return b;
}

/* Convert a double to a Bigint plus an exponent.  Return NULL on failure.

   Given a finite nonzero double d, return an odd Bigint b and exponent *e
   such that fabs(d) = b * 2**e.  On return, *bbits gives the number of
   significant bits of b; that is, 2**(*bbits-1) <= b < 2**(*bbits).

   If d is zero, then b == 0, *e == -1010, *bbits = 0.
 */

static Bigint *
d2b(U *d, int *e, int *bits)
{
    Bigint *b;
    int de, k;
    ULong *x, y, z;
    int i;

    b = Balloc(1);
    if (b == NULL)
        return NULL;
    x = b->x;

    z = word0(d) & Frac_mask;
    word0(d) &= 0x7fffffff;   /* clear sign bit, which we ignore */
    if ((de = (int)(word0(d) >> Exp_shift)))
        z |= Exp_msk1;
    if ((y = word1(d))) {
        if ((k = lo0bits(&y))) {
            x[0] = y | z << (32 - k);
            z >>= k;
        }
        else
            x[0] = y;
        i =
            b->wds = (x[1] = z) ? 2 : 1;
    }
    else {
        k = lo0bits(&z);
        x[0] = z;
        i =
            b->wds = 1;
        k += 32;
    }
    if (de) {
        *e = de - Bias - (P-1) + k;
        *bits = P - k;
    }
    else {
        *e = de - Bias - (P-1) + 1 + k;
        *bits = 32*i - hi0bits(x[i-1]);
    }
    return b;
}

/* Compute the ratio of two Bigints, as a double.  The result may have an
   error of up to 2.5 ulps. */

static double
ratio(Bigint *a, Bigint *b)
{
    U da, db;
    int k, ka, kb;

    dval(&da) = b2d(a, &ka);
    dval(&db) = b2d(b, &kb);
    k = ka - kb + 32*(a->wds - b->wds);
    if (k > 0)
        word0(&da) += k*Exp_msk1;
    else {
        k = -k;
        word0(&db) += k*Exp_msk1;
    }
    return dval(&da) / dval(&db);
}

static const double
tens[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22
};

static const double
bigtens[] = { 1e16, 1e32, 1e64, 1e128, 1e256 };
static const double tinytens[] = { 1e-16, 1e-32, 1e-64, 1e-128,
                                   9007199254740992.*9007199254740992.e-256
                                   /* = 2^106 * 1e-256 */
};
/* The factor of 2^53 in tinytens[4] helps us avoid setting the underflow */
/* flag unnecessarily.  It leads to a song and dance at the end of strtod. */
#define Scale_Bit 0x10
#define n_bigtens 5

#define ULbits 32
#define kshift 5
#define kmask 31


static int
dshift(Bigint *b, int p2)
{
    int rv = hi0bits(b->x[b->wds-1]) - 4;
    if (p2 > 0)
        rv -= p2;
    return rv & kmask;
}

/* special case of Bigint division.  The quotient is always in the range 0 <=
   quotient < 10, and on entry the divisor S is normalized so that its top 4
   bits (28--31) are zero and bit 27 is set. */

static int
quorem(Bigint *b, Bigint *S)
{
    int n;
    ULong *bx, *bxe, q, *sx, *sxe;
    ULLong borrow, carry, y, ys;

    n = S->wds;
#ifdef QP_DTOA_DEBUG
    /*debug*/ if (b->wds > n)
        /*debug*/       Bug("oversize b in quorem");
#endif
    if (b->wds < n)
        return 0;
    sx = S->x;
    sxe = sx + --n;
    bx = b->x;
    bxe = bx + n;
    q = *bxe / (*sxe + 1);      /* ensure q <= true quotient */
#ifdef QP_DTOA_DEBUG
    /*debug*/ if (q > 9)
        /*debug*/       Bug("oversized quotient in quorem");
#endif
    if (q) {
        borrow = 0;
        carry = 0;
        do {
            ys = *sx++ * (ULLong)q + carry;
            carry = ys >> 32;
            y = *bx - (ys & FFFFFFFF) - borrow;
            borrow = y >> 32 & (ULong)1;
            *bx++ = (ULong)(y & FFFFFFFF);
        }
        while(sx <= sxe);
        if (!*bxe) {
            bx = b->x;
            while(--bxe > bx && !*bxe)
                --n;
            b->wds = n;
        }
    }
    if (cmp(b, S) >= 0) {
        q++;
        borrow = 0;
        carry = 0;
        bx = b->x;
        sx = S->x;
        do {
            ys = *sx++ + carry;
            carry = ys >> 32;
            y = *bx - (ys & FFFFFFFF) - borrow;
            borrow = y >> 32 & (ULong)1;
            *bx++ = (ULong)(y & FFFFFFFF);
        }
        while(sx <= sxe);
        bx = b->x;
        bxe = bx + n;
        if (!*bxe) {
            while(--bxe > bx && !*bxe)
                --n;
            b->wds = n;
        }
    }
    return q;
}

/* sulp(x) is a version of ulp(x) that takes bc.scale into account.

   Assuming that x is finite and nonnegative (positive zero is fine
   here) and x / 2^bc.scale is exactly representable as a double,
   sulp(x) is equivalent to 2^bc.scale * ulp(x / 2^bc.scale). */

static double
sulp(U *x, BCinfo *bc)
{
    U u;

    if (bc->scale && 2*P + 1 > (int)((word0(x) & Exp_mask) >> Exp_shift)) {
        /* rv/2^bc->scale is subnormal */
        word0(&u) = (P+2)*Exp_msk1;
        word1(&u) = 0;
        return u.d;
    }
    else {
        assert(word0(x) || word1(x)); /* x != 0.0 */
        return ulp(x);
    }
}

/* The bigcomp function handles some hard cases for strtod, for inputs
   with more than STRTOD_DIGLIM digits.  It's called once an initial
   estimate for the double corresponding to the input string has
   already been obtained by the code in _Py_dg_strtod.

   The bigcomp function is only called after _Py_dg_strtod has found a
   double value rv such that either rv or rv + 1ulp represents the
   correctly rounded value corresponding to the original string.  It
   determines which of these two values is the correct one by
   computing the decimal digits of rv + 0.5ulp and comparing them with
   the corresponding digits of s0.

   In the following, write dv for the absolute value of the number represented
   by the input string.

   Inputs:

     s0 points to the first significant digit of the input string.

     rv is a (possibly scaled) estimate for the closest double value to the
        value represented by the original input to _Py_dg_strtod.  If
        bc->scale is nonzero, then rv/2^(bc->scale) is the approximation to
        the input value.

     bc is a struct containing information gathered during the parsing and
        estimation steps of _Py_dg_strtod.  Description of fields follows:

        bc->e0 gives the exponent of the input value, such that dv = (integer
           given by the bd->nd digits of s0) * 10**e0

        bc->nd gives the total number of significant digits of s0.  It will
           be at least 1.

        bc->nd0 gives the number of significant digits of s0 before the
           decimal separator.  If there's no decimal separator, bc->nd0 ==
           bc->nd.

        bc->scale is the value used to scale rv to avoid doing arithmetic with
           subnormal values.  It's either 0 or 2*P (=106).

   Outputs:

     On successful exit, rv/2^(bc->scale) is the closest double to dv.

     Returns 0 on success, -1 on failure (e.g., due to a failed malloc call). */

static int
bigcomp(U *rv, const char *s0, BCinfo *bc)
{
    Bigint *b, *d;
    int b2, d2, dd, i, nd, nd0, odd, p2, p5;

    nd = bc->nd;
    nd0 = bc->nd0;
    p5 = nd + bc->e0;
    b = sd2b(rv, bc->scale, &p2);
    if (b == NULL)
        return -1;

    /* record whether the lsb of rv/2^(bc->scale) is odd:  in the exact halfway
       case, this is used for round to even. */
    odd = b->x[0] & 1;

    /* left shift b by 1 bit and or a 1 into the least significant bit;
       this gives us b * 2**p2 = rv/2^(bc->scale) + 0.5 ulp. */
    b = lshift(b, 1);
    if (b == NULL)
        return -1;
    b->x[0] |= 1;
    p2--;

    p2 -= p5;
    d = i2b(1);
    if (d == NULL) {
        Bfree(b);
        return -1;
    }
    /* Arrange for convenient computation of quotients:
     * shift left if necessary so divisor has 4 leading 0 bits.
     */
    if (p5 > 0) {
        d = pow5mult(d, p5);
        if (d == NULL) {
            Bfree(b);
            return -1;
        }
    }
    else if (p5 < 0) {
        b = pow5mult(b, -p5);
        if (b == NULL) {
            Bfree(d);
            return -1;
        }
    }
    if (p2 > 0) {
        b2 = p2;
        d2 = 0;
    }
    else {
        b2 = 0;
        d2 = -p2;
    }
    i = dshift(d, d2);
    if ((b2 += i) > 0) {
        b = lshift(b, b2);
        if (b == NULL) {
            Bfree(d);
            return -1;
        }
    }
    if ((d2 += i) > 0) {
        d = lshift(d, d2);
        if (d == NULL) {
            Bfree(b);
            return -1;
        }
    }

    /* Compare s0 with b/d: set dd to -1, 0, or 1 according as s0 < b/d, s0 ==
     * b/d, or s0 > b/d.  Here the digits of s0 are thought of as representing
     * a number in the range [0.1, 1). */
    if (cmp(b, d) >= 0)
        /* b/d >= 1 */
        dd = -1;
    else {
        i = 0;
        for(;;) {
            b = multadd(b, 10, 0);
            if (b == NULL) {
                Bfree(d);
                return -1;
            }
            dd = s0[i < nd0 ? i : i+1] - '0' - quorem(b, d);
            i++;

            if (dd)
                break;
            if (!b->x[0] && b->wds == 1) {
                /* b/d == 0 */
                dd = i < nd;
                break;
            }
            if (!(i < nd)) {
                /* b/d != 0, but digits of s0 exhausted */
                dd = -1;
                break;
            }
        }
    }
    Bfree(b);
    Bfree(d);
    if (dd > 0 || (dd == 0 && odd))
        dval(rv) += sulp(rv, bc);
    return 0;
}


static double
_Py_dg_strtod(const char *s00, char **se)
{
    int bb2, bb5, bbe, bd2, bd5, bs2, c, dsign, e, e1, error;
    int esign, i, j, k, lz, nd, nd0, odd, sign;
    const char *s, *s0, *s1;
    double aadj, aadj1;
    U aadj2, adj, rv, rv0;
    ULong y, z, abs_exp;
    Long L;
    BCinfo bc;
    Bigint *bb = NULL, *bd = NULL, *bd0 = NULL, *bs = NULL, *delta = NULL;
    size_t ndigits, fraclen;
    double result;

    dval(&rv) = 0.;

    /* Start parsing. */
    c = *(s = s00);

    /* Parse optional sign, if present. */
    sign = 0;
    switch (c) {
    case '-':
        sign = 1;
        /* fall through */
    case '+':
        c = *++s;
    }

    /* Skip leading zeros: lz is true iff there were leading zeros. */
    s1 = s;
    while (c == '0')
        c = *++s;
    lz = s != s1;

    /* Point s0 at the first nonzero digit (if any).  fraclen will be the
       number of digits between the decimal point and the end of the
       digit string.  ndigits will be the total number of digits ignoring
       leading zeros. */
    s0 = s1 = s;
    while ('0' <= c && c <= '9')
        c = *++s;
    ndigits = s - s1;
    fraclen = 0;

    /* Parse decimal point and following digits. */
    if (c == '.') {
        c = *++s;
        if (!ndigits) {
            s1 = s;
            while (c == '0')
                c = *++s;
            lz = lz || s != s1;
            fraclen += (s - s1);
            s0 = s;
        }
        s1 = s;
        while ('0' <= c && c <= '9')
            c = *++s;
        ndigits += s - s1;
        fraclen += s - s1;
    }

    /* Now lz is true if and only if there were leading zero digits, and
       ndigits gives the total number of digits ignoring leading zeros.  A
       valid input must have at least one digit. */
    if (!ndigits && !lz) {
        if (se)
            *se = (char *)s00;
        goto parse_error;
    }

    /* Range check ndigits and fraclen to make sure that they, and values
       computed with them, can safely fit in an int. */
    if (ndigits > MAX_DIGITS || fraclen > MAX_DIGITS) {
        if (se)
            *se = (char *)s00;
        goto parse_error;
    }
    nd = (int)ndigits;
    nd0 = (int)ndigits - (int)fraclen;

    /* Parse exponent. */
    e = 0;
    if (c == 'e' || c == 'E') {
        s00 = s;
        c = *++s;

        /* Exponent sign. */
        esign = 0;
        switch (c) {
        case '-':
            esign = 1;
            /* fall through */
        case '+':
            c = *++s;
        }

        /* Skip zeros.  lz is true iff there are leading zeros. */
        s1 = s;
        while (c == '0')
            c = *++s;
        lz = s != s1;

        /* Get absolute value of the exponent. */
        s1 = s;
        abs_exp = 0;
        while ('0' <= c && c <= '9') {
            abs_exp = 10*abs_exp + (c - '0');
            c = *++s;
        }

        /* abs_exp will be correct modulo 2**32.  But 10**9 < 2**32, so if
           there are at most 9 significant exponent digits then overflow is
           impossible. */
        if (s - s1 > 9 || abs_exp > MAX_ABS_EXP)
            e = (int)MAX_ABS_EXP;
        else
            e = (int)abs_exp;
        if (esign)
            e = -e;

        /* A valid exponent must have at least one digit. */
        if (s == s1 && !lz)
            s = s00;
    }

    /* Adjust exponent to take into account position of the point. */
    e -= nd - nd0;
    if (nd0 <= 0)
        nd0 = nd;

    /* Finished parsing.  Set se to indicate how far we parsed */
    if (se)
        *se = (char *)s;

    /* If all digits were zero, exit with return value +-0.0.  Otherwise,
       strip trailing zeros: scan back until we hit a nonzero digit. */
    if (!nd)
        goto ret;
    for (i = nd; i > 0; ) {
        --i;
        if (s0[i < nd0 ? i : i+1] != '0') {
            ++i;
            break;
        }
    }
    e += nd - i;
    nd = i;
    if (nd0 > nd)
        nd0 = nd;

    /* Summary of parsing results.  After parsing, and dealing with zero
     * inputs, we have values s0, nd0, nd, e, sign, where:
     *
     *  - s0 points to the first significant digit of the input string
     *
     *  - nd is the total number of significant digits (here, and
     *    below, 'significant digits' means the set of digits of the
     *    significand of the input that remain after ignoring leading
     *    and trailing zeros).
     *
     *  - nd0 indicates the position of the decimal point, if present; it
     *    satisfies 1 <= nd0 <= nd.  The nd significant digits are in
     *    s0[0:nd0] and s0[nd0+1:nd+1] using the usual Python half-open slice
     *    notation.  (If nd0 < nd, then s0[nd0] contains a '.'  character; if
     *    nd0 == nd, then s0[nd0] could be any non-digit character.)
     *
     *  - e is the adjusted exponent: the absolute value of the number
     *    represented by the original input string is n * 10**e, where
     *    n is the integer represented by the concatenation of
     *    s0[0:nd0] and s0[nd0+1:nd+1]
     *
     *  - sign gives the sign of the input:  1 for negative, 0 for positive
     *
     *  - the first and last significant digits are nonzero
     */

    /* put first DBL_DIG+1 digits into integer y and z.
     *
     *  - y contains the value represented by the first min(9, nd)
     *    significant digits
     *
     *  - if nd > 9, z contains the value represented by significant digits
     *    with indices in [9, min(16, nd)).  So y * 10**(min(16, nd) - 9) + z
     *    gives the value represented by the first min(16, nd) sig. digits.
     */

    bc.e0 = e1 = e;
    y = z = 0;
    for (i = 0; i < nd; i++) {
        if (i < 9)
            y = 10*y + s0[i < nd0 ? i : i+1] - '0';
        else if (i < DBL_DIG+1)
            z = 10*z + s0[i < nd0 ? i : i+1] - '0';
        else
            break;
    }

    k = nd < DBL_DIG + 1 ? nd : DBL_DIG + 1;
    dval(&rv) = y;
    if (k > 9) {
        dval(&rv) = tens[k - 9] * dval(&rv) + z;
    }
    if (nd <= DBL_DIG
        && Flt_Rounds == 1
        ) {
        if (!e)
            goto ret;
        if (e > 0) {
            if (e <= Ten_pmax) {
                dval(&rv) *= tens[e];
                goto ret;
            }
            i = DBL_DIG - nd;
            if (e <= Ten_pmax + i) {
                /* A fancier test would sometimes let us do
                 * this for larger i values.
                 */
                e -= i;
                dval(&rv) *= tens[i];
                dval(&rv) *= tens[e];
                goto ret;
            }
        }
        else if (e >= -Ten_pmax) {
            dval(&rv) /= tens[-e];
            goto ret;
        }
    }
    e1 += nd - k;

    bc.scale = 0;

    /* Get starting approximation = rv * 10**e1 */

    if (e1 > 0) {
        if ((i = e1 & 15))
            dval(&rv) *= tens[i];
        if (e1 &= ~15) {
            if (e1 > DBL_MAX_10_EXP)
                goto ovfl;
            e1 >>= 4;
            for(j = 0; e1 > 1; j++, e1 >>= 1)
                if (e1 & 1)
                    dval(&rv) *= bigtens[j];
            /* The last multiplication could overflow. */
            word0(&rv) -= P*Exp_msk1;
            dval(&rv) *= bigtens[j];
            if ((z = word0(&rv) & Exp_mask)
                > Exp_msk1*(DBL_MAX_EXP+Bias-P))
                goto ovfl;
            if (z > Exp_msk1*(DBL_MAX_EXP+Bias-1-P)) {
                /* set to largest number */
                /* (Can't trust DBL_MAX) */
                word0(&rv) = Big0;
                word1(&rv) = Big1;
            }
            else
                word0(&rv) += P*Exp_msk1;
        }
    }
    else if (e1 < 0) {
        /* The input decimal value lies in [10**e1, 10**(e1+16)).

           If e1 <= -512, underflow immediately.
           If e1 <= -256, set bc.scale to 2*P.

           So for input value < 1e-256, bc.scale is always set;
           for input value >= 1e-240, bc.scale is never set.
           For input values in [1e-256, 1e-240), bc.scale may or may
           not be set. */

        e1 = -e1;
        if ((i = e1 & 15))
            dval(&rv) /= tens[i];
        if (e1 >>= 4) {
            if (e1 >= 1 << n_bigtens)
                goto undfl;
            if (e1 & Scale_Bit)
                bc.scale = 2*P;
            for(j = 0; e1 > 0; j++, e1 >>= 1)
                if (e1 & 1)
                    dval(&rv) *= tinytens[j];
            if (bc.scale && (j = 2*P + 1 - ((word0(&rv) & Exp_mask)
                                            >> Exp_shift)) > 0) {
                /* scaled rv is denormal; clear j low bits */
                if (j >= 32) {
                    word1(&rv) = 0;
                    if (j >= 53)
                        word0(&rv) = (P+2)*Exp_msk1;
                    else
                        word0(&rv) &= 0xffffffff << (j-32);
                }
                else
                    word1(&rv) &= 0xffffffff << j;
            }
            if (!dval(&rv))
                goto undfl;
        }
    }

    /* Now the hard part -- adjusting rv to the correct value.*/

    /* Put digits into bd: true value = bd * 10^e */

    bc.nd = nd;
    bc.nd0 = nd0;       /* Only needed if nd > STRTOD_DIGLIM, but done here */
                        /* to silence an erroneous warning about bc.nd0 */
                        /* possibly not being initialized. */
    if (nd > STRTOD_DIGLIM) {
        /* ASSERT(STRTOD_DIGLIM >= 18); 18 == one more than the */
        /* minimum number of decimal digits to distinguish double values */
        /* in IEEE arithmetic. */

        /* Truncate input to 18 significant digits, then discard any trailing
           zeros on the result by updating nd, nd0, e and y suitably. (There's
           no need to update z; it's not reused beyond this point.) */
        for (i = 18; i > 0; ) {
            /* scan back until we hit a nonzero digit.  significant digit 'i'
            is s0[i] if i < nd0, s0[i+1] if i >= nd0. */
            --i;
            if (s0[i < nd0 ? i : i+1] != '0') {
                ++i;
                break;
            }
        }
        e += nd - i;
        nd = i;
        if (nd0 > nd)
            nd0 = nd;
        if (nd < 9) { /* must recompute y */
            y = 0;
            for(i = 0; i < nd0; ++i)
                y = 10*y + s0[i] - '0';
            for(; i < nd; ++i)
                y = 10*y + s0[i+1] - '0';
        }
    }
    bd0 = s2b(s0, nd0, nd, y);
    if (bd0 == NULL)
        goto failed_malloc;

    /* Notation for the comments below.  Write:

         - dv for the absolute value of the number represented by the original
           decimal input string.

         - if we've truncated dv, write tdv for the truncated value.
           Otherwise, set tdv == dv.

         - srv for the quantity rv/2^bc.scale; so srv is the current binary
           approximation to tdv (and dv).  It should be exactly representable
           in an IEEE 754 double.
    */

    for(;;) {

        /* This is the main correction loop for _Py_dg_strtod.

           We've got a decimal value tdv, and a floating-point approximation
           srv=rv/2^bc.scale to tdv.  The aim is to determine whether srv is
           close enough (i.e., within 0.5 ulps) to tdv, and to compute a new
           approximation if not.

           To determine whether srv is close enough to tdv, compute integers
           bd, bb and bs proportional to tdv, srv and 0.5 ulp(srv)
           respectively, and then use integer arithmetic to determine whether
           |tdv - srv| is less than, equal to, or greater than 0.5 ulp(srv).
        */

        bd = Balloc(bd0->k);
        if (bd == NULL) {
            goto failed_malloc;
        }
        Bcopy(bd, bd0);
        bb = sd2b(&rv, bc.scale, &bbe);   /* srv = bb * 2^bbe */
        if (bb == NULL) {
            goto failed_malloc;
        }
        /* Record whether lsb of bb is odd, in case we need this
           for the round-to-even step later. */
        odd = bb->x[0] & 1;

        /* tdv = bd * 10**e;  srv = bb * 2**bbe */
        bs = i2b(1);
        if (bs == NULL) {
            goto failed_malloc;
        }

        if (e >= 0) {
            bb2 = bb5 = 0;
            bd2 = bd5 = e;
        }
        else {
            bb2 = bb5 = -e;
            bd2 = bd5 = 0;
        }
        if (bbe >= 0)
            bb2 += bbe;
        else
            bd2 -= bbe;
        bs2 = bb2;
        bb2++;
        bd2++;

        /* At this stage bd5 - bb5 == e == bd2 - bb2 + bbe, bb2 - bs2 == 1,
           and bs == 1, so:

              tdv == bd * 10**e = bd * 2**(bbe - bb2 + bd2) * 5**(bd5 - bb5)
              srv == bb * 2**bbe = bb * 2**(bbe - bb2 + bb2)
              0.5 ulp(srv) == 2**(bbe-1) = bs * 2**(bbe - bb2 + bs2)

           It follows that:

              M * tdv = bd * 2**bd2 * 5**bd5
              M * srv = bb * 2**bb2 * 5**bb5
              M * 0.5 ulp(srv) = bs * 2**bs2 * 5**bb5

           for some constant M.  (Actually, M == 2**(bb2 - bbe) * 5**bb5, but
           this fact is not needed below.)
        */

        /* Remove factor of 2**i, where i = min(bb2, bd2, bs2). */
        i = bb2 < bd2 ? bb2 : bd2;
        if (i > bs2)
            i = bs2;
        if (i > 0) {
            bb2 -= i;
            bd2 -= i;
            bs2 -= i;
        }

        /* Scale bb, bd, bs by the appropriate powers of 2 and 5. */
        if (bb5 > 0) {
            bs = pow5mult(bs, bb5);
            if (bs == NULL) {
                goto failed_malloc;
            }
            Bigint *bb1 = mult(bs, bb);
            Bfree(bb);
            bb = bb1;
            if (bb == NULL) {
                goto failed_malloc;
            }
        }
        if (bb2 > 0) {
            bb = lshift(bb, bb2);
            if (bb == NULL) {
                goto failed_malloc;
            }
        }
        if (bd5 > 0) {
            bd = pow5mult(bd, bd5);
            if (bd == NULL) {
                goto failed_malloc;
            }
        }
        if (bd2 > 0) {
            bd = lshift(bd, bd2);
            if (bd == NULL) {
                goto failed_malloc;
            }
        }
        if (bs2 > 0) {
            bs = lshift(bs, bs2);
            if (bs == NULL) {
                goto failed_malloc;
            }
        }

        /* Now bd, bb and bs are scaled versions of tdv, srv and 0.5 ulp(srv),
           respectively.  Compute the difference |tdv - srv|, and compare
           with 0.5 ulp(srv). */

        delta = diff(bb, bd);
        if (delta == NULL) {
            goto failed_malloc;
        }
        dsign = delta->sign;
        delta->sign = 0;
        i = cmp(delta, bs);
        if (bc.nd > nd && i <= 0) {
            if (dsign)
                break;  /* Must use bigcomp(). */

            /* Here rv overestimates the truncated decimal value by at most
               0.5 ulp(rv).  Hence rv either overestimates the true decimal
               value by <= 0.5 ulp(rv), or underestimates it by some small
               amount (< 0.1 ulp(rv)); either way, rv is within 0.5 ulps of
               the true decimal value, so it's possible to exit.

               Exception: if scaled rv is a normal exact power of 2, but not
               DBL_MIN, then rv - 0.5 ulp(rv) takes us all the way down to the
               next double, so the correctly rounded result is either rv - 0.5
               ulp(rv) or rv; in this case, use bigcomp to distinguish. */

            if (!word1(&rv) && !(word0(&rv) & Bndry_mask)) {
                /* rv can't be 0, since it's an overestimate for some
                   nonzero value.  So rv is a normal power of 2. */
                j = (int)(word0(&rv) & Exp_mask) >> Exp_shift;
                /* rv / 2^bc.scale = 2^(j - 1023 - bc.scale); use bigcomp if
                   rv / 2^bc.scale >= 2^-1021. */
                if (j - bc.scale >= 2) {
                    dval(&rv) -= 0.5 * sulp(&rv, &bc);
                    break; /* Use bigcomp. */
                }
            }

            {
                bc.nd = nd;
                i = -1; /* Discarded digits make delta smaller. */
            }
        }

        if (i < 0) {
            /* Error is less than half an ulp -- check for
             * special case of mantissa a power of two.
             */
            if (dsign || word1(&rv) || word0(&rv) & Bndry_mask
                || (word0(&rv) & Exp_mask) <= (2*P+1)*Exp_msk1
                ) {
                break;
            }
            if (!delta->x[0] && delta->wds <= 1) {
                /* exact result */
                break;
            }
            delta = lshift(delta,Log2P);
            if (delta == NULL) {
                goto failed_malloc;
            }
            if (cmp(delta, bs) > 0)
                goto drop_down;
            break;
        }
        if (i == 0) {
            /* exactly half-way between */
            if (dsign) {
                if ((word0(&rv) & Bndry_mask1) == Bndry_mask1
                    &&  word1(&rv) == (
                        (bc.scale &&
                         (y = word0(&rv) & Exp_mask) <= 2*P*Exp_msk1) ?
                        (0xffffffff & (0xffffffff << (2*P+1-(y>>Exp_shift)))) :
                        0xffffffff)) {
                    /*boundary case -- increment exponent*/
                    word0(&rv) = (word0(&rv) & Exp_mask)
                        + Exp_msk1
                        ;
                    word1(&rv) = 0;
                    /* dsign = 0; */
                    break;
                }
            }
            else if (!(word0(&rv) & Bndry_mask) && !word1(&rv)) {
              drop_down:
                /* boundary case -- decrement exponent */
                if (bc.scale) {
                    L = word0(&rv) & Exp_mask;
                    if (L <= (2*P+1)*Exp_msk1) {
                        if (L > (P+2)*Exp_msk1)
                            /* round even ==> */
                            /* accept rv */
                            break;
                        /* rv = smallest denormal */
                        if (bc.nd > nd)
                            break;
                        goto undfl;
                    }
                }
                L = (word0(&rv) & Exp_mask) - Exp_msk1;
                word0(&rv) = L | Bndry_mask1;
                word1(&rv) = 0xffffffff;
                break;
            }
            if (!odd)
                break;
            if (dsign)
                dval(&rv) += sulp(&rv, &bc);
            else {
                dval(&rv) -= sulp(&rv, &bc);
                if (!dval(&rv)) {
                    if (bc.nd >nd)
                        break;
                    goto undfl;
                }
            }
            /* dsign = 1 - dsign; */
            break;
        }
        if ((aadj = ratio(delta, bs)) <= 2.) {
            if (dsign)
                aadj = aadj1 = 1.;
            else if (word1(&rv) || word0(&rv) & Bndry_mask) {
                if (word1(&rv) == Tiny1 && !word0(&rv)) {
                    if (bc.nd >nd)
                        break;
                    goto undfl;
                }
                aadj = 1.;
                aadj1 = -1.;
            }
            else {
                /* special case -- power of FLT_RADIX to be */
                /* rounded down... */

                if (aadj < 2./FLT_RADIX)
                    aadj = 1./FLT_RADIX;
                else
                    aadj *= 0.5;
                aadj1 = -aadj;
            }
        }
        else {
            aadj *= 0.5;
            aadj1 = dsign ? aadj : -aadj;
            if (Flt_Rounds == 0)
                aadj1 += 0.5;
        }
        y = word0(&rv) & Exp_mask;

        /* Check for overflow */

        if (y == Exp_msk1*(DBL_MAX_EXP+Bias-1)) {
            dval(&rv0) = dval(&rv);
            word0(&rv) -= P*Exp_msk1;
            adj.d = aadj1 * ulp(&rv);
            dval(&rv) += adj.d;
            if ((word0(&rv) & Exp_mask) >=
                Exp_msk1*(DBL_MAX_EXP+Bias-P)) {
                if (word0(&rv0) == Big0 && word1(&rv0) == Big1) {
                    goto ovfl;
                }
                word0(&rv) = Big0;
                word1(&rv) = Big1;
                goto cont;
            }
            else
                word0(&rv) += P*Exp_msk1;
        }
        else {
            if (bc.scale && y <= 2*P*Exp_msk1) {
                if (aadj <= 0x7fffffff) {
                    if ((z = (ULong)aadj) <= 0)
                        z = 1;
                    aadj = z;
                    aadj1 = dsign ? aadj : -aadj;
                }
                dval(&aadj2) = aadj1;
                word0(&aadj2) += (2*P+1)*Exp_msk1 - y;
                aadj1 = dval(&aadj2);
            }
            adj.d = aadj1 * ulp(&rv);
            dval(&rv) += adj.d;
        }
        z = word0(&rv) & Exp_mask;
        if (bc.nd == nd) {
            if (!bc.scale)
                if (y == z) {
                    /* Can we stop now? */
                    L = (Long)aadj;
                    aadj -= L;
                    /* The tolerances below are conservative. */
                    if (dsign || word1(&rv) || word0(&rv) & Bndry_mask) {
                        if (aadj < .4999999 || aadj > .5000001)
                            break;
                    }
                    else if (aadj < .4999999/FLT_RADIX)
                        break;
                }
        }
      cont:
        Bfree(bb); bb = NULL;
        Bfree(bd); bd = NULL;
        Bfree(bs); bs = NULL;
        Bfree(delta); delta = NULL;
    }
    if (bc.nd > nd) {
        error = bigcomp(&rv, s0, &bc);
        if (error)
            goto failed_malloc;
    }

    if (bc.scale) {
        word0(&rv0) = Exp_1 - 2*P*Exp_msk1;
        word1(&rv0) = 0;
        dval(&rv) *= dval(&rv0);
    }

  ret:
    result = sign ? -dval(&rv) : dval(&rv);
    goto done;

  parse_error:
    result = 0.0;
    goto done;

  failed_malloc:
    errno = ENOMEM;
    result = -1.0;
    goto done;

  undfl:
    result = sign ? -0.0 : 0.0;
    goto done;

  ovfl:
    errno = ERANGE;
    /* Can't trust HUGE_VAL */
    word0(&rv) = Exp_mask;
    word1(&rv) = 0;
    result = sign ? -dval(&rv) : dval(&rv);
    goto done;

  done:
    Bfree(bb);
    Bfree(bd);
    Bfree(bs);
    Bfree(bd0);
    Bfree(delta);
    return result;

}

static char *
rv_alloc(int i)
{
    int k, *r;
    size_t j;   /* size_t so that j <<= 1 cannot overflow for i near INT_MAX */

    j = sizeof(ULong);
    for(k = 0;
        sizeof(Bigint) - sizeof(ULong) - sizeof(int) + j <= (unsigned)i;
        j <<= 1)
        k++;
    r = (int*)Balloc(k);
    if (r == NULL)
        return NULL;
    *r = k;
    return (char *)(r+1);
}

static char *
nrv_alloc(const char *s, char **rve, int n)
{
    char *rv, *t;

    rv = rv_alloc(n);
    if (rv == NULL)
        return NULL;
    t = rv;
    while((*t = *s++)) t++;
    if (rve)
        *rve = t;
    return rv;
}

/* freedtoa(s) must be used to free values s returned by dtoa
 * when MULTIPLE_THREADS is #defined.  It should be used in all cases,
 * but for consistency with earlier versions of dtoa, it is optional
 * when MULTIPLE_THREADS is not defined.
 */

static void
_Py_dg_freedtoa(char *s)
{
    Bigint *b = (Bigint *)((int *)s - 1);
    b->maxwds = 1 << (b->k = *(int*)b);
    Bfree(b);
}

/* dtoa for IEEE arithmetic (dmg): convert double to ASCII string.
 *
 * Inspired by "How to Print Floating-Point Numbers Accurately" by
 * Guy L. Steele, Jr. and Jon L. White [Proc. ACM SIGPLAN '90, pp. 112-126].
 *
 * Modifications:
 *      1. Rather than iterating, we use a simple numeric overestimate
 *         to determine k = floor(log10(d)).  We scale relevant
 *         quantities using O(log2(k)) rather than O(k) multiplications.
 *      2. For some modes > 2 (corresponding to ecvt and fcvt), we don't
 *         try to generate digits strictly left to right.  Instead, we
 *         compute with fewer bits and propagate the carry if necessary
 *         when rounding the final digit up.  This is often faster.
 *      3. Under the assumption that input will be rounded nearest,
 *         mode 0 renders 1e23 as 1e23 rather than 9.999999999999999e22.
 *         That is, we allow equality in stopping tests when the
 *         round-nearest rule will give the same floating-point value
 *         as would satisfaction of the stopping test with strict
 *         inequality.
 *      4. We remove common factors of powers of 2 from relevant
 *         quantities.
 *      5. When converting floating-point integers less than 1e16,
 *         we use floating-point arithmetic rather than resorting
 *         to multiple-precision integers.
 *      6. When asked to produce fewer than 15 digits, we first try
 *         to get by with floating-point arithmetic; we resort to
 *         multiple-precision integer arithmetic only if we cannot
 *         guarantee that the floating-point calculation has given
 *         the correctly rounded result.  For k requested digits and
 *         "uniformly" distributed input, the probability is
 *         something like 10^(k-15) that we must resort to the Long
 *         calculation.
 */

/* Additional notes (METD): (1) returns NULL on failure.  (2) to avoid memory
   leakage, a successful call to _Py_dg_dtoa should always be matched by a
   call to _Py_dg_freedtoa. */

static char *
_Py_dg_dtoa(double dd, int mode, int ndigits,
            int *decpt, int *sign, char **rve)
{
    /*  Arguments ndigits, decpt, sign are similar to those
        of ecvt and fcvt; trailing zeros are suppressed from
        the returned string.  If not null, *rve is set to point
        to the end of the return value.  If d is +-Infinity or NaN,
        then *decpt is set to 9999.

        mode:
        0 ==> shortest string that yields d when read in
        and rounded to nearest.
        1 ==> like 0, but with Steele & White stopping rule;
        e.g. with IEEE P754 arithmetic , mode 0 gives
        1e23 whereas mode 1 gives 9.999999999999999e22.
        2 ==> max(1,ndigits) significant digits.  This gives a
        return value similar to that of ecvt, except
        that trailing zeros are suppressed.
        3 ==> through ndigits past the decimal point.  This
        gives a return value similar to that from fcvt,
        except that trailing zeros are suppressed, and
        ndigits can be negative.
        4,5 ==> similar to 2 and 3, respectively, but (in
        round-nearest mode) with the tests of mode 0 to
        possibly return a shorter string that rounds to d.
        With IEEE arithmetic and compilation with
        -DHonor_FLT_ROUNDS, modes 4 and 5 behave the same
        as modes 2 and 3 when FLT_ROUNDS != 1.
        6-9 ==> Debugging modes similar to mode - 4:  don't try
        fast floating-point estimate (if applicable).

        Values of mode other than 0-9 are treated as mode 0.

        Sufficient space is allocated to the return value
        to hold the suppressed trailing zeros.
    */

    int bbits, b2, b5, be, dig, i, ieps, ilim, ilim0, ilim1,
        j, j1, k, k0, k_check, leftright, m2, m5, s2, s5,
        spec_case, try_quick;
    Long L;
    int denorm;
    ULong x;
    Bigint *b, *b1, *delta, *mlo, *mhi, *S;
    U d2, eps, u;
    double ds;
    char *s, *s0;

    /* set pointers to NULL, to silence gcc compiler warnings and make
       cleanup easier on error */
    mlo = mhi = S = 0;
    s0 = 0;

    u.d = dd;
    if (word0(&u) & Sign_bit) {
        /* set sign for everything, including 0's and NaNs */
        *sign = 1;
        word0(&u) &= ~Sign_bit; /* clear sign bit */
    }
    else
        *sign = 0;

    /* quick return for Infinities, NaNs and zeros */
    if ((word0(&u) & Exp_mask) == Exp_mask)
    {
        /* Infinity or NaN */
        *decpt = 9999;
        if (!word1(&u) && !(word0(&u) & 0xfffff))
            return nrv_alloc("Infinity", rve, 8);
        return nrv_alloc("NaN", rve, 3);
    }
    if (!dval(&u)) {
        *decpt = 1;
        return nrv_alloc("0", rve, 1);
    }

    /* compute k = floor(log10(d)).  The computation may leave k
       one too large, but should never leave k too small. */
    b = d2b(&u, &be, &bbits);
    if (b == NULL)
        goto failed_malloc;
    if ((i = (int)(word0(&u) >> Exp_shift1 & (Exp_mask>>Exp_shift1)))) {
        dval(&d2) = dval(&u);
        word0(&d2) &= Frac_mask1;
        word0(&d2) |= Exp_11;

        /* log(x)       ~=~ log(1.5) + (x-1.5)/1.5
         * log10(x)      =  log(x) / log(10)
         *              ~=~ log(1.5)/log(10) + (x-1.5)/(1.5*log(10))
         * log10(d) = (i-Bias)*log(2)/log(10) + log10(d2)
         *
         * This suggests computing an approximation k to log10(d) by
         *
         * k = (i - Bias)*0.301029995663981
         *      + ( (d2-1.5)*0.289529654602168 + 0.176091259055681 );
         *
         * We want k to be too large rather than too small.
         * The error in the first-order Taylor series approximation
         * is in our favor, so we just round up the constant enough
         * to compensate for any error in the multiplication of
         * (i - Bias) by 0.301029995663981; since |i - Bias| <= 1077,
         * and 1077 * 0.30103 * 2^-52 ~=~ 7.2e-14,
         * adding 1e-13 to the constant term more than suffices.
         * Hence we adjust the constant term to 0.1760912590558.
         * (We could get a more accurate k by invoking log10,
         *  but this is probably not worthwhile.)
         */

        i -= Bias;
        denorm = 0;
    }
    else {
        /* d is denormalized */

        i = bbits + be + (Bias + (P-1) - 1);
        x = i > 32  ? word0(&u) << (64 - i) | word1(&u) >> (i - 32)
            : word1(&u) << (32 - i);
        dval(&d2) = x;
        word0(&d2) -= 31*Exp_msk1; /* adjust exponent */
        i -= (Bias + (P-1) - 1) + 1;
        denorm = 1;
    }
    ds = (dval(&d2)-1.5)*0.289529654602168 + 0.1760912590558 +
        i*0.301029995663981;
    k = (int)ds;
    if (ds < 0. && ds != k)
        k--;    /* want k = floor(ds) */
    k_check = 1;
    if (k >= 0 && k <= Ten_pmax) {
        if (dval(&u) < tens[k])
            k--;
        k_check = 0;
    }
    j = bbits - i - 1;
    if (j >= 0) {
        b2 = 0;
        s2 = j;
    }
    else {
        b2 = -j;
        s2 = 0;
    }
    if (k >= 0) {
        b5 = 0;
        s5 = k;
        s2 += k;
    }
    else {
        b2 -= k;
        b5 = -k;
        s5 = 0;
    }
    if (mode < 0 || mode > 9)
        mode = 0;

    try_quick = 1;

    if (mode > 5) {
        mode -= 4;
        try_quick = 0;
    }
    leftright = 1;
    ilim = ilim1 = -1;  /* Values for cases 0 and 1; done here to */
    /* silence erroneous "gcc -Wall" warning. */
    switch(mode) {
    case 0:
    case 1:
        i = 18;
        ndigits = 0;
        break;
    case 2:
        leftright = 0;
        /* fall through */
    case 4:
        if (ndigits <= 0)
            ndigits = 1;
        ilim = ilim1 = i = ndigits;
        break;
    case 3:
        leftright = 0;
        /* fall through */
    case 5:
        /* -330 < k < 330 for any finite nonzero double.  Clamp ndigits so
           that ndigits + k + 1 stays within int range; no double has
           anywhere near this many decimal digits so the digits returned
           are unaffected (*decpt saturates in the no_digits case).  Same
           bound as DOUBLE_TO_STRING_PRECISION_MAX in pystrtod.c. */
        if (ndigits > INT_MAX - 1024)
            ndigits = INT_MAX - 1024;
        else if (ndigits < -(INT_MAX - 1024))
            ndigits = -(INT_MAX - 1024);
        i = ndigits + k + 1;
        ilim = i;
        ilim1 = i - 1;
        if (i <= 0)
            i = 1;
    }
    s0 = rv_alloc(i);
    if (s0 == NULL)
        goto failed_malloc;
    s = s0;


    if (ilim >= 0 && ilim <= Quick_max && try_quick) {

        /* Try to get by with floating-point arithmetic. */

        i = 0;
        dval(&d2) = dval(&u);
        k0 = k;
        ilim0 = ilim;
        ieps = 2; /* conservative */
        if (k > 0) {
            ds = tens[k&0xf];
            j = k >> 4;
            if (j & Bletch) {
                /* prevent overflows */
                j &= Bletch - 1;
                dval(&u) /= bigtens[n_bigtens-1];
                ieps++;
            }
            for(; j; j >>= 1, i++)
                if (j & 1) {
                    ieps++;
                    ds *= bigtens[i];
                }
            dval(&u) /= ds;
        }
        else if ((j1 = -k)) {
            dval(&u) *= tens[j1 & 0xf];
            for(j = j1 >> 4; j; j >>= 1, i++)
                if (j & 1) {
                    ieps++;
                    dval(&u) *= bigtens[i];
                }
        }
        if (k_check && dval(&u) < 1. && ilim > 0) {
            if (ilim1 <= 0)
                goto fast_failed;
            ilim = ilim1;
            k--;
            dval(&u) *= 10.;
            ieps++;
        }
        dval(&eps) = ieps*dval(&u) + 7.;
        word0(&eps) -= (P-1)*Exp_msk1;
        if (ilim == 0) {
            S = mhi = 0;
            dval(&u) -= 5.;
            if (dval(&u) > dval(&eps))
                goto one_digit;
            if (dval(&u) < -dval(&eps))
                goto no_digits;
            goto fast_failed;
        }
        if (leftright) {
            /* Use Steele & White method of only
             * generating digits needed.
             */
            dval(&eps) = 0.5/tens[ilim-1] - dval(&eps);
            for(i = 0;;) {
                L = (Long)dval(&u);
                dval(&u) -= L;
                *s++ = '0' + (int)L;
                if (dval(&u) < dval(&eps))
                    goto ret1;
                if (1. - dval(&u) < dval(&eps))
                    goto bump_up;
                if (++i >= ilim)
                    break;
                dval(&eps) *= 10.;
                dval(&u) *= 10.;
            }
        }
        else {
            /* Generate ilim digits, then fix them up. */
            dval(&eps) *= tens[ilim-1];
            for(i = 1;; i++, dval(&u) *= 10.) {
                L = (Long)(dval(&u));
                if (!(dval(&u) -= L))
                    ilim = i;
                *s++ = '0' + (int)L;
                if (i == ilim) {
                    if (dval(&u) > 0.5 + dval(&eps))
                        goto bump_up;
                    else if (dval(&u) < 0.5 - dval(&eps)) {
                        while(*--s == '0');
                        s++;
                        goto ret1;
                    }
                    break;
                }
            }
        }
      fast_failed:
        s = s0;
        dval(&u) = dval(&d2);
        k = k0;
        ilim = ilim0;
    }

    /* Do we have a "small" integer? */

    if (be >= 0 && k <= Int_max) {
        /* Yes. */
        ds = tens[k];
        if (ndigits < 0 && ilim <= 0) {
            S = mhi = 0;
            if (ilim < 0 || dval(&u) <= 5*ds)
                goto no_digits;
            goto one_digit;
        }
        for(i = 1;; i++, dval(&u) *= 10.) {
            L = (Long)(dval(&u) / ds);
            dval(&u) -= L*ds;
            *s++ = '0' + (int)L;
            if (!dval(&u)) {
                break;
            }
            if (i == ilim) {
                dval(&u) += dval(&u);
                if (dval(&u) > ds || (dval(&u) == ds && L & 1)) {
                  bump_up:
                    while(*--s == '9')
                        if (s == s0) {
                            k++;
                            *s = '0';
                            break;
                        }
                    ++*s++;
                }
                else {
                    /* Strip trailing zeros. This branch was missing from the
                       original dtoa.c, leading to surplus trailing zeros in
                       some cases. See bugs.python.org/issue40780. */
                    while (s > s0 && s[-1] == '0') {
                        --s;
                    }
                }
                break;
            }
        }
        goto ret1;
    }

    m2 = b2;
    m5 = b5;
    if (leftright) {
        i =
            denorm ? be + (Bias + (P-1) - 1 + 1) :
            1 + P - bbits;
        b2 += i;
        s2 += i;
        mhi = i2b(1);
        if (mhi == NULL)
            goto failed_malloc;
    }
    if (m2 > 0 && s2 > 0) {
        i = m2 < s2 ? m2 : s2;
        b2 -= i;
        m2 -= i;
        s2 -= i;
    }
    if (b5 > 0) {
        if (leftright) {
            if (m5 > 0) {
                mhi = pow5mult(mhi, m5);
                if (mhi == NULL)
                    goto failed_malloc;
                b1 = mult(mhi, b);
                Bfree(b);
                b = b1;
                if (b == NULL)
                    goto failed_malloc;
            }
            if ((j = b5 - m5)) {
                b = pow5mult(b, j);
                if (b == NULL)
                    goto failed_malloc;
            }
        }
        else {
            b = pow5mult(b, b5);
            if (b == NULL)
                goto failed_malloc;
        }
    }
    S = i2b(1);
    if (S == NULL)
        goto failed_malloc;
    if (s5 > 0) {
        S = pow5mult(S, s5);
        if (S == NULL)
            goto failed_malloc;
    }

    /* Check for special case that d is a normalized power of 2. */

    spec_case = 0;
    if ((mode < 2 || leftright)
        ) {
        if (!word1(&u) && !(word0(&u) & Bndry_mask)
            && word0(&u) & (Exp_mask & ~Exp_msk1)
            ) {
            /* The special case */
            b2 += Log2P;
            s2 += Log2P;
            spec_case = 1;
        }
    }

    /* Arrange for convenient computation of quotients:
     * shift left if necessary so divisor has 4 leading 0 bits.
     *
     * Perhaps we should just compute leading 28 bits of S once
     * and for all and pass them and a shift to quorem, so it
     * can do shifts and ors to compute the numerator for q.
     */
#define iInc 28
    i = dshift(S, s2);
    b2 += i;
    m2 += i;
    s2 += i;
    if (b2 > 0) {
        b = lshift(b, b2);
        if (b == NULL)
            goto failed_malloc;
    }
    if (s2 > 0) {
        S = lshift(S, s2);
        if (S == NULL)
            goto failed_malloc;
    }
    if (k_check) {
        if (cmp(b,S) < 0) {
            k--;
            b = multadd(b, 10, 0);      /* we botched the k estimate */
            if (b == NULL)
                goto failed_malloc;
            if (leftright) {
                mhi = multadd(mhi, 10, 0);
                if (mhi == NULL)
                    goto failed_malloc;
            }
            ilim = ilim1;
        }
    }
    if (ilim <= 0 && (mode == 3 || mode == 5)) {
        if (ilim < 0) {
            /* no digits, fcvt style */
          no_digits:
            k = -1 - ndigits;
            goto ret;
        }
        else {
            S = multadd(S, 5, 0);
            if (S == NULL)
                goto failed_malloc;
            if (cmp(b, S) <= 0)
                goto no_digits;
        }
      one_digit:
        *s++ = '1';
        k++;
        goto ret;
    }
    if (leftright) {
        if (m2 > 0) {
            mhi = lshift(mhi, m2);
            if (mhi == NULL)
                goto failed_malloc;
        }

        /* Compute mlo -- check for special case
         * that d is a normalized power of 2.
         */

        mlo = mhi;
        if (spec_case) {
            mhi = Balloc(mhi->k);
            if (mhi == NULL)
                goto failed_malloc;
            Bcopy(mhi, mlo);
            mhi = lshift(mhi, Log2P);
            if (mhi == NULL)
                goto failed_malloc;
        }

        for(i = 1;;i++) {
            dig = quorem(b,S) + '0';
            /* Do we yet have the shortest decimal string
             * that will round to d?
             */
            j = cmp(b, mlo);
            delta = diff(S, mhi);
            if (delta == NULL)
                goto failed_malloc;
            j1 = delta->sign ? 1 : cmp(b, delta);
            Bfree(delta);
            if (j1 == 0 && mode != 1 && !(word1(&u) & 1)
                ) {
                if (dig == '9')
                    goto round_9_up;
                if (j > 0)
                    dig++;
                *s++ = dig;
                goto ret;
            }
            if (j < 0 || (j == 0 && mode != 1
                          && !(word1(&u) & 1)
                    )) {
                if (!b->x[0] && b->wds <= 1) {
                    goto accept_dig;
                }
                if (j1 > 0) {
                    b = lshift(b, 1);
                    if (b == NULL)
                        goto failed_malloc;
                    j1 = cmp(b, S);
                    if ((j1 > 0 || (j1 == 0 && dig & 1))
                        && dig++ == '9')
                        goto round_9_up;
                }
              accept_dig:
                *s++ = dig;
                goto ret;
            }
            if (j1 > 0) {
                if (dig == '9') { /* possible if i == 1 */
                  round_9_up:
                    *s++ = '9';
                    goto roundoff;
                }
                *s++ = dig + 1;
                goto ret;
            }
            *s++ = dig;
            if (i == ilim)
                break;
            b = multadd(b, 10, 0);
            if (b == NULL)
                goto failed_malloc;
            if (mlo == mhi) {
                mlo = mhi = multadd(mhi, 10, 0);
                if (mlo == NULL)
                    goto failed_malloc;
            }
            else {
                mlo = multadd(mlo, 10, 0);
                if (mlo == NULL)
                    goto failed_malloc;
                mhi = multadd(mhi, 10, 0);
                if (mhi == NULL)
                    goto failed_malloc;
            }
        }
    }
    else
        for(i = 1;; i++) {
            *s++ = dig = quorem(b,S) + '0';
            if (!b->x[0] && b->wds <= 1) {
                goto ret;
            }
            if (i >= ilim)
                break;
            b = multadd(b, 10, 0);
            if (b == NULL)
                goto failed_malloc;
        }

    /* Round off last digit */

    b = lshift(b, 1);
    if (b == NULL)
        goto failed_malloc;
    j = cmp(b, S);
    if (j > 0 || (j == 0 && dig & 1)) {
      roundoff:
        while(*--s == '9')
            if (s == s0) {
                k++;
                *s++ = '1';
                goto ret;
            }
        ++*s++;
    }
    else {
        while(*--s == '0');
        s++;
    }
  ret:
    Bfree(S);
    if (mhi) {
        if (mlo && mlo != mhi)
            Bfree(mlo);
        Bfree(mhi);
    }
  ret1:
    Bfree(b);
    *s = 0;
    *decpt = k + 1;
    if (rve)
        *rve = s;
    return s0;
  failed_malloc:
    if (S)
        Bfree(S);
    if (mlo && mlo != mhi)
        Bfree(mlo);
    if (mhi)
        Bfree(mhi);
    if (b)
        Bfree(b);
    if (s0)
        _Py_dg_freedtoa(s0);
    return NULL;
}


/* End of the dtoa.c section: drop its private names (see change 11). */
#undef Balloc
#undef Bfree
#undef multadd
#undef s2b
#undef hi0bits
#undef lo0bits
#undef i2b
#undef mult
#undef pow5mult
#undef lshift
#undef cmp
#undef diff
#undef ulp
#undef b2d
#undef sd2b
#undef d2b
#undef ratio
#undef dshift
#undef quorem
#undef sulp
#undef bigcomp
#undef rv_alloc
#undef nrv_alloc
#undef tens
#undef bigtens
#undef tinytens
#undef U
#undef Long
#undef ULong
#undef ULLong
#undef Bigint
#undef BCinfo
#undef MALLOC
#undef FREE
#undef IEEE_8087
#undef IEEE_MC68k
#undef Bug
#undef word0
#undef word1
#undef dval
#undef STRTOD_DIGLIM
#undef MAX_ABS_EXP
#undef MAX_DIGITS
#undef Storeinc
#undef Exp_shift
#undef Exp_shift1
#undef Exp_msk1
#undef Exp_msk11
#undef Exp_mask
#undef P
#undef Nbits
#undef Bias
#undef Emax
#undef Emin
#undef Etiny
#undef Exp_1
#undef Exp_11
#undef Ebits
#undef Frac_mask
#undef Frac_mask1
#undef Ten_pmax
#undef Bletch
#undef Bndry_mask
#undef Bndry_mask1
#undef Sign_bit
#undef Log2P
#undef Tiny0
#undef Tiny1
#undef Quick_max
#undef Int_max
#undef Flt_Rounds
#undef Rounding
#undef Big0
#undef Big1
#undef POSINF_WORD0
#undef POSINF_WORD1
#undef FFFFFFFF
#undef Bcopy
#undef Scale_Bit
#undef n_bigtens
#undef ULbits
#undef kshift
#undef kmask
#undef iInc


/* ====================================================================== */
/* Section 2: Python/pystrtod.c, float/complex repr, float(), round()     */
/* ====================================================================== */

/* Set err to MemoryError (empty message, like PyErr_NoMemory). */
static void qp_numfmt_nomem(qp_error *err)
{
    if (err)
        qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
}

/* Length of the longest prefix of the NUL-terminated UTF-8 string s that
   is at most max_bytes long and does not end inside a multi-byte
   sequence: the '%.200s' precision rule of PyUnicode_FromFormat
   (unicode_fromformat_write_utf8 drops a trailing incomplete sequence). */
static int qp_utf8_prefix_len(const char *s, size_t max_bytes)
{
    size_t len = 0, i;
    const unsigned char *p = (const unsigned char *)s;

    while (len < max_bytes && p[len])
        len++;
    if (len == max_bytes && len > 0) {
        /* back up over an incomplete trailing sequence */
        i = len;
        while (i > 0 && (p[i - 1] & 0xC0) == 0x80 && len - i < 3)
            i--;
        if (i > 0 && p[i - 1] >= 0xC0) {
            size_t need = p[i - 1] >= 0xF0 ? 4 : p[i - 1] >= 0xE0 ? 3 : 2;
            if (len - (i - 1) < need)
                len = i - 1;
        }
    }
    return (int)len;
}

/* Case-insensitive string match used for nan and inf detection; t should be
   lower-case.  Returns 1 for a successful match, 0 otherwise. */
static int
qp_case_insensitive_match(const char *s, const char *t)
{
    while (*t && (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + ('a' - 'A') : *s) == (unsigned char)*t) {
        s++;
        t++;
    }
    return *t ? 0 : 1;
}

/* _Py_parse_inf_or_nan: Attempt to parse a string of the form "nan", "inf" or
   "infinity", with an optional leading sign of "+" or "-".  On success,
   return the NaN or Infinity as a double and set *endptr to point just beyond
   the successfully parsed portion of the string.  On failure, return -1.0 and
   set *endptr to point to the start of the string. */
static double
_Py_parse_inf_or_nan(const char *p, char **endptr)
{
    double retval;
    const char *s;
    int negate = 0;

    s = p;
    if (*s == '-') {
        negate = 1;
        s++;
    }
    else if (*s == '+') {
        s++;
    }
    if (qp_case_insensitive_match(s, "inf")) {
        s += 3;
        if (qp_case_insensitive_match(s, "inity"))
            s += 5;
        retval = negate ? -INFINITY : INFINITY;
    }
    else if (qp_case_insensitive_match(s, "nan")) {
        s += 3;
        retval = negate ? -fabs(NAN) : fabs(NAN);
    }
    else {
        s = p;
        retval = -1.0;
    }
    *endptr = (char *)s;
    return retval;
}

/* _PyOS_ascii_strtod (the _PY_SHORT_FLOAT_REPR == 1 variant): converts a
   string to a double like strtod() in the C locale, correctly rounded, via
   _Py_dg_strtod; falls back to inf/nan parsing.  errno is reset first so
   that ERANGE (overflow/underflow) and ENOMEM can be detected. */
static double
qp_ascii_strtod(const char *nptr, char **endptr)
{
    double result;

    assert(nptr != NULL);
    /* Set errno to zero, so that we can distinguish zero results
       and underflows */
    errno = 0;

    result = _Py_dg_strtod(nptr, endptr);

    if (*endptr == nptr)
        /* string might represent an inf or nan */
        result = _Py_parse_inf_or_nan(nptr, endptr);

    return result;
}

/* PyOS_string_to_double converts a null-terminated byte string s (interpreted
   as a string of ASCII characters) to a float.  The string should not have
   leading or trailing whitespace.  The conversion is independent of the
   current locale.

   If endptr is NULL, try to convert the whole string.  Raise ValueError and
   return -1.0 if the string is not a valid representation of a floating-point
   number.

   If endptr is non-NULL, try to convert as much of the string as possible.
   If no initial segment of the string is the valid representation of a
   floating-point number then *endptr is set to point to the beginning of the
   string, -1.0 is returned and again ValueError is raised.

   On overflow (e.g., when trying to convert '1e500' on an IEEE 754 machine),
   if overflow_kind is QP_ERR_NONE then +-INFINITY is returned, and no
   error is set.  Otherwise an error of that kind is set, -1.0 is returned,
   and *endptr will point just past the end of the converted value.

   If any other failure occurs (lack of memory), -1.0 is returned and
   MemoryError is set. */
static double
qp_string_to_double(const char *s, char **endptr, qp_error_kind overflow_kind, qp_error *err)
{
    double x, result=-1.0;
    char *fail_pos;

    errno = 0;
    x = qp_ascii_strtod(s, &fail_pos);

    if (errno == ENOMEM) {
        qp_numfmt_nomem(err);
        fail_pos = (char *)s;
    }
    else if (!endptr && (fail_pos == s || *fail_pos != '\0'))
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "could not convert string to float: '%.*s'",
                     qp_utf8_prefix_len(s, 200), s);
    else if (fail_pos == s)
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "could not convert string to float: '%.*s'",
                     qp_utf8_prefix_len(s, 200), s);
    else if (errno == ERANGE && fabs(x) >= 1.0 && overflow_kind != QP_ERR_NONE)
        qp_error_set(err, overflow_kind, 0, 0, 0, 0,
                     "value too large to convert to float: '%.*s'",
                     qp_utf8_prefix_len(s, 200), s);
    else
        result = x;

    if (endptr != NULL)
        *endptr = fail_pos;
    return result;
}

/* Underscore removal of _Py_string_to_number_with_underscores(): copy
   s[0..len) to dst without underscores after checking that every '_' sits
   between two ASCII digits and that there is no embedded NUL (dst must
   have len + 1 bytes).  Returns the new length or -1 on a violation. */
static Py_ssize_t
qp_number_strip_underscores(const char *s, size_t len, char *dst)
{
    char prev;
    const char *p, *last;
    char *end;

    end = dst;
    prev = '\0';
    last = s + len;
    for (p = s; p < last && *p; p++) {
        if (*p == '_') {
            /* Underscores are only allowed after digits. */
            if (!(prev >= '0' && prev <= '9')) {
                return -1;
            }
        }
        else {
            *end++ = *p;
            /* Underscores are only allowed before digits. */
            if (prev == '_' && !(*p >= '0' && *p <= '9')) {
                return -1;
            }
        }
        prev = *p;
    }
    /* Underscores are not allowed at the end. */
    if (prev == '_') {
        return -1;
    }
    /* No embedded NULs allowed. */
    if (p != last) {
        return -1;
    }
    *end = '\0';
    return end - dst;
}

/* float_from_string_inner(): strip ASCII whitespace, then the whole rest
   must be a float literal.  0 = ok, 1 = invalid, -1 = no memory. */
static int
qp_float_from_string_inner(const char *s, Py_ssize_t len, double *out)
{
    double x;
    char *end;
    const char *last = s + len;
    qp_error err;

    /* strip leading whitespace */
    while (s < last && (*s == ' ' || (*s >= '\t' && *s <= '\r'))) {
        s++;
    }
    if (s == last) {
        return 1;
    }

    /* strip trailing whitespace */
    while (s < last - 1 && (last[-1] == ' ' || (last[-1] >= '\t' && last[-1] <= '\r'))) {
        last--;
    }

    /* We don't care about overflow or underflow.  If the platform
     * supports them, infinities and signed zeroes (on underflow) are
     * fine. */
    memset(&err, 0, sizeof(err));
    x = qp_string_to_double(s, &end, QP_ERR_NONE, &err);
    if (err.kind == QP_ERR_MEMORY) {
        qp_error_clear(&err);
        return -1;
    }
    qp_error_clear(&err);
    if (end != last) {
        return 1;
    }
    *out = x;
    return 0;
}

/* float(str/bytes): see numfmt.h.  Mirrors PyFloat_FromString's call of
   _Py_string_to_number_with_underscores(s, len, "float", v, v,
   float_from_string_inner). */
static int
qp_float_from_string(const char *s, size_t len, double *out)
{
    char *dup;
    Py_ssize_t n;
    int ret;

    assert(s[len] == '\0');
    if (strchr(s, '_') == NULL) {
        return qp_float_from_string_inner(s, (Py_ssize_t)len, out);
    }
    dup = qp_sys_malloc(len + 1);
    if (dup == NULL)
        return -1;
    n = qp_number_strip_underscores(s, len, dup);
    if (n < 0) {
        qp_sys_free(dup);
        return 1;
    }
    ret = qp_float_from_string_inner(dup, n, out);
    qp_sys_free(dup);
    return ret;
}

/* Largest precision magnitude accepted by qp_double_to_string().  The
   output buffer sizes computed below and within _Py_dg_dtoa() use int and
   Py_ssize_t arithmetic on roughly precision + (digits before the point, at
   most DBL_MAX_10_EXP + 1 == 309) + a few bytes of sign, point and exponent.
   Staying this far inside the int range keeps all of those sums in range.
   (Only C callers can pass a negative precision.)  _Py_dg_dtoa() applies
   the same bound to its ndigits argument. */
#define QP_DOUBLE_TO_STRING_PRECISION_MAX (INT_MAX - 1024)

/* The fixed set of strings used for inf, nan and the exponent marker */
#define QP_OFS_INF 0
#define QP_OFS_NAN 1
#define QP_OFS_E 2

/* The lengths of these are known to the code below, so don't change them */
static const char * const qp_lc_float_strings[] = {
    "inf",
    "nan",
    "e",
};
static const char * const qp_uc_float_strings[] = {
    "INF",
    "NAN",
    "E",
};


/* format_float_short: convert a double d to a string, and return a
   malloc'd block of memory containing the resulting string.

   Arguments:
     d is the double to be converted
     format_code is one of 'e', 'f', 'g', 'r'.  'e', 'f' and 'g'
       correspond to '%e', '%f' and '%g';  'r' corresponds to repr.
     mode is one of '0', '2' or '3', and is completely determined by
       format_code: 'e' and 'g' use mode 2; 'f' mode 3, 'r' mode 0.
     precision is the desired precision
     always_add_sign is nonzero if a '+' sign should be included for positive
       numbers
     add_dot_0_if_integer is nonzero if integers in non-exponential form
       should have ".0" added.  Only applies to format codes 'r' and 'g'.
     use_alt_formatting is nonzero if alternative formatting should be
       used.  Only applies to format codes 'e', 'f' and 'g'.  For code 'g',
       at most one of use_alt_formatting and add_dot_0_if_integer should
       be nonzero.
     type, if non-NULL, will be set to one of these constants to identify
       the type of the 'd' argument:
     QP_DTST_FINITE
     QP_DTST_INFINITE
     QP_DTST_NAN

   Returns a malloc'd block of memory containing the resulting string,
    or NULL on error (MemoryError set in err). */
static char *
qp_format_float_short(double d, char format_code,
                      int mode, int precision,
                      int always_add_sign, int add_dot_0_if_integer,
                      int use_alt_formatting, int no_negative_zero,
                      const char * const *float_strings, int *type,
                      qp_error *err)
{
    char *buf = NULL;
    char *p = NULL;
    Py_ssize_t bufsize = 0;
    char *digits, *digits_end;
    int decpt_as_int, sign, exp_len, exp = 0, use_exp = 0;
    Py_ssize_t decpt, digits_len, vdigits_start, vdigits_end;

    /* _Py_dg_dtoa returns a digit string (no decimal point or exponent).
       Must be matched by a call to _Py_dg_freedtoa. */
    digits = _Py_dg_dtoa(d, mode, precision, &decpt_as_int, &sign,
                         &digits_end);

    decpt = (Py_ssize_t)decpt_as_int;
    if (digits == NULL) {
        /* The only failure mode is no memory. */
        qp_numfmt_nomem(err);
        goto exit;
    }
    assert(digits_end != NULL && digits_end >= digits);
    digits_len = digits_end - digits;

    if (no_negative_zero && sign == 1 &&
            (digits_len == 0 || (digits_len == 1 && digits[0] == '0'))) {
        sign = 0;
    }

    if (digits_len && !(digits[0] >= '0' && digits[0] <= '9')) {
        /* Infinities and nans here; adapt Gay's output,
           so convert Infinity to inf and NaN to nan, and
           ignore sign of nan. Then return. */

        /* ignore the actual sign of a nan */
        if (digits[0] == 'n' || digits[0] == 'N')
            sign = 0;

        /* We only need 5 bytes to hold the result "+inf\0" . */
        bufsize = 5; /* Used later in an assert. */
        buf = (char *)qp_sys_malloc(bufsize);
        if (buf == NULL) {
            qp_numfmt_nomem(err);
            goto exit;
        }
        p = buf;

        if (sign == 1) {
            *p++ = '-';
        }
        else if (always_add_sign) {
            *p++ = '+';
        }
        if (digits[0] == 'i' || digits[0] == 'I') {
            memcpy(p, float_strings[QP_OFS_INF], 3);
            p += 3;

            if (type)
                *type = QP_DTST_INFINITE;
        }
        else if (digits[0] == 'n' || digits[0] == 'N') {
            memcpy(p, float_strings[QP_OFS_NAN], 3);
            p += 3;

            if (type)
                *type = QP_DTST_NAN;
        }
        else {
            /* shouldn't get here: Gay's code should always return
               something starting with a digit, an 'I',  or 'N' */
            abort();
        }
        goto exit;
    }

    /* The result must be finite (not inf or nan). */
    if (type)
        *type = QP_DTST_FINITE;


    /* We got digits back, format them.  We may need to pad 'digits'
       either on the left or right (or both) with extra zeros, so in
       general the resulting string has the form

         [<sign>]<zeros><digits><zeros>[<exponent>]

       where either of the <zeros> pieces could be empty, and there's a
       decimal point that could appear either in <digits> or in the
       leading or trailing <zeros>.

       Imagine an infinite 'virtual' string vdigits, consisting of the
       string 'digits' (starting at index 0) padded on both the left and
       right with infinite strings of zeros.  We want to output a slice

         vdigits[vdigits_start : vdigits_end]

       of this virtual string.  Thus if vdigits_start < 0 then we'll end
       up producing some leading zeros; if vdigits_end > digits_len there
       will be trailing zeros in the output.  The next section of code
       determines whether to use an exponent or not, figures out the
       position 'decpt' of the decimal point, and computes 'vdigits_start'
       and 'vdigits_end'. */
    vdigits_end = digits_len;
    switch (format_code) {
    case 'e':
        use_exp = 1;
        vdigits_end = precision;
        break;
    case 'f':
        vdigits_end = decpt + precision;
        break;
    case 'g':
        if (decpt <= -4 || decpt >
            (add_dot_0_if_integer ? precision-1 : precision))
            use_exp = 1;
        if (use_alt_formatting)
            vdigits_end = precision;
        break;
    case 'r':
        /* convert to exponential format at 1e16.  We used to convert
           at 1e17, but that gives odd-looking results for some values
           when a 16-digit 'shortest' repr is padded with bogus zeros.
           For example, repr(2e16+8) would give 20000000000000010.0;
           the true value is 20000000000000008.0. */
        if (decpt <= -4 || decpt > 16)
            use_exp = 1;
        break;
    default:
        assert(0);
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "bad argument to internal function");
        goto exit;
    }

    /* if using an exponent, reset decimal point position to 1 and adjust
       exponent accordingly.*/
    if (use_exp) {
        exp = (int)decpt - 1;
        decpt = 1;
    }
    /* ensure vdigits_start < decpt <= vdigits_end, or vdigits_start <
       decpt < vdigits_end if add_dot_0_if_integer and no exponent */
    vdigits_start = decpt <= 0 ? decpt-1 : 0;
    if (!use_exp && add_dot_0_if_integer)
        vdigits_end = vdigits_end > decpt ? vdigits_end : decpt + 1;
    else
        vdigits_end = vdigits_end > decpt ? vdigits_end : decpt;

    /* double check inequalities */
    assert(vdigits_start <= 0 &&
           0 <= digits_len &&
           digits_len <= vdigits_end);
    /* decimal point should be in (vdigits_start, vdigits_end] */
    assert(vdigits_start < decpt && decpt <= vdigits_end);

    /* Compute an upper bound how much memory we need. This might be a few
       chars too long, but no big deal. */
    bufsize =
        /* sign, decimal point and trailing 0 byte */
        3 +

        /* total digit count (including zero padding on both sides) */
        (vdigits_end - vdigits_start) +

        /* exponent "e+100", max 3 numerical digits */
        (use_exp ? 5 : 0);

    /* Now allocate the memory and initialize p to point to the start of
       it. */
    buf = (char *)qp_sys_malloc(bufsize);
    if (buf == NULL) {
        qp_numfmt_nomem(err);
        goto exit;
    }
    p = buf;

    /* Add a negative sign if negative, and a plus sign if non-negative
       and always_add_sign is true. */
    if (sign == 1)
        *p++ = '-';
    else if (always_add_sign)
        *p++ = '+';

    /* note that exactly one of the three 'if' conditions is true,
       so we include exactly one decimal point */
    /* Zero padding on left of digit string */
    if (decpt <= 0) {
        memset(p, '0', decpt-vdigits_start);
        p += decpt - vdigits_start;
        *p++ = '.';
        memset(p, '0', 0-decpt);
        p += 0-decpt;
    }
    else {
        memset(p, '0', 0-vdigits_start);
        p += 0 - vdigits_start;
    }

    /* Digits, with included decimal point */
    if (0 < decpt && decpt <= digits_len) {
        memcpy(p, digits, decpt-0);
        p += decpt-0;
        *p++ = '.';
        memcpy(p, digits+decpt, digits_len-decpt);
        p += digits_len-decpt;
    }
    else {
        memcpy(p, digits, digits_len);
        p += digits_len;
    }

    /* And zeros on the right */
    if (digits_len < decpt) {
        memset(p, '0', decpt-digits_len);
        p += decpt-digits_len;
        *p++ = '.';
        memset(p, '0', vdigits_end-decpt);
        p += vdigits_end-decpt;
    }
    else {
        memset(p, '0', vdigits_end-digits_len);
        p += vdigits_end-digits_len;
    }

    /* Delete a trailing decimal pt unless using alternative formatting. */
    if (p[-1] == '.' && !use_alt_formatting)
        p--;

    /* Now that we've done zero padding, add an exponent if needed. */
    if (use_exp) {
        *p++ = float_strings[QP_OFS_E][0];
        exp_len = snprintf(p, 5, "%+.02d", exp);
        p += exp_len;
    }
  exit:
    if (buf) {
        *p = '\0';
        /* It's too late if this fails, as we've already stepped on
           memory that isn't ours. But it's an okay debugging test. */
        assert(p-buf < bufsize);
    }
    if (digits)
        _Py_dg_freedtoa(digits);

    return buf;
}


/* PyOS_double_to_string: see numfmt.h. */
static char *
qp_double_to_string(double val, char format_code, int precision, int flags,
                    int *type, qp_error *err)
{
    const char * const *float_strings = qp_lc_float_strings;
    int mode;

    if (precision > QP_DOUBLE_TO_STRING_PRECISION_MAX
        || precision < -QP_DOUBLE_TO_STRING_PRECISION_MAX)
    {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s", "precision too big");
        return NULL;
    }

    /* Validate format_code, and map upper and lower case. Compute the
       mode and make any adjustments as needed. */
    switch (format_code) {
    /* exponent */
    case 'E':
        float_strings = qp_uc_float_strings;
        format_code = 'e';
        /* fall through */
    case 'e':
        mode = 2;
        precision++;
        break;

    /* fixed */
    case 'F':
        float_strings = qp_uc_float_strings;
        format_code = 'f';
        /* fall through */
    case 'f':
        mode = 3;
        break;

    /* general */
    case 'G':
        float_strings = qp_uc_float_strings;
        format_code = 'g';
        /* fall through */
    case 'g':
        mode = 2;
        /* precision 0 makes no sense for 'g' format; interpret as 1 */
        if (precision == 0)
            precision = 1;
        break;

    /* repr format */
    case 'r':
        mode = 0;
        /* Supplied precision is unused, must be 0. */
        if (precision != 0) {
            assert(0);
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                         "bad argument to internal function");
            return NULL;
        }
        break;

    default:
        assert(0);
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "bad argument to internal function");
        return NULL;
    }

    return qp_format_float_short(val, format_code, mode, precision,
                                 flags & QP_DTSF_SIGN,
                                 flags & QP_DTSF_ADD_DOT_0,
                                 flags & QP_DTSF_ALT,
                                 flags & QP_DTSF_NO_NEG_0,
                                 float_strings, type, err);
}

/* float_repr(): repr(x) == str(x) for a float. */
static int
qp_float_repr(double x, char *buf)
{
    qp_error err;
    char *s;
    size_t len;

    memset(&err, 0, sizeof(err));
    s = qp_double_to_string(x, 'r', 0, QP_DTSF_ADD_DOT_0, NULL, &err);
    qp_error_clear(&err);
    if (!s)
        return -1;
    len = strlen(s);
    assert(len < QP_FLOAT_REPR_MAX);
    memcpy(buf, s, len + 1);
    qp_sys_free(s);
    return (int)len;
}

/* complex_repr(): real part +0.0 -> "<imag>j", else "(<real><signed
   imag>j)", both parts in repr ('r') format without ".0". */
static int
qp_complex_repr(double re, double im, DynBuf *out)
{
    qp_error err;
    char *pre = NULL, *pim = NULL;
    int ret = -1;

    memset(&err, 0, sizeof(err));
    if (re == 0. && copysign(1.0, re) == 1.0) {
        /* Real part is +0: just output the imaginary part and do not
           include parens. */
        pim = qp_double_to_string(im, 'r', 0, 0, NULL, &err);
        if (!pim)
            goto done;
        dbuf_putstr(out, pim);
        dbuf_putc(out, 'j');
    } else {
        /* Format imaginary part with sign, real part without. Include
           parens in the result. */
        pre = qp_double_to_string(re, 'r', 0, 0, NULL, &err);
        if (!pre)
            goto done;
        pim = qp_double_to_string(im, 'r', 0, QP_DTSF_SIGN, NULL, &err);
        if (!pim)
            goto done;
        dbuf_putc(out, '(');
        dbuf_putstr(out, pre);
        dbuf_putstr(out, pim);
        dbuf_putstr(out, "j)");
    }
    ret = dbuf_error(out) ? -1 : 0;
  done:
    qp_error_clear(&err);
    qp_sys_free(pre);
    qp_sys_free(pim);
    return ret;
}

/* double_round (the _PY_SHORT_FLOAT_REPR == 1 version): rounds a finite
   double to the closest multiple of 10**-ndigits using the correctly
   rounded string<->double conversions; here ndigits is within reasonable
   bounds (-308 <= ndigits <= 323).  Returns 0 with *out set, or -1 with
   OverflowError/MemoryError in err. */
static int
qp_double_round_impl(double x, int ndigits, double *out, qp_error *err)
{
    double rounded;
    Py_ssize_t buflen, mybuflen=100;
    char *buf, *buf_end, shortbuf[100], *mybuf=shortbuf;
    int decpt, sign;
    int result = -1;

    /* round to a decimal string */
    buf = _Py_dg_dtoa(x, 3, ndigits, &decpt, &sign, &buf_end);
    if (buf == NULL) {
        qp_numfmt_nomem(err);
        return -1;
    }

    /* Get new buffer if shortbuf is too small.  Space needed <= buf_end -
    buf + 8: (1 extra for '0', 1 for sign, 5 for exp, 1 for '\0').  */
    buflen = buf_end - buf;
    if (buflen + 8 > mybuflen) {
        mybuflen = buflen+8;
        mybuf = (char *)qp_sys_malloc(mybuflen);
        if (mybuf == NULL) {
            qp_numfmt_nomem(err);
            goto exit;
        }
    }
    /* copy buf to mybuf, adding exponent, sign and leading 0 */
    snprintf(mybuf, mybuflen, "%s0%se%d", (sign ? "-" : ""),
             buf, decpt - (int)buflen);

    /* and convert the resulting string back to a double */
    errno = 0;
    rounded = _Py_dg_strtod(mybuf, NULL);
    if (errno == ERANGE && fabs(rounded) >= 1.)
        qp_error_set(err, QP_ERR_OVERFLOW, 0, 0, 0, 0, "%s",
                     "rounded value too large to represent");
    else {
        *out = rounded;
        result = 0;
    }

    /* done computing value;  now clean up */
    if (mybuf != shortbuf)
        qp_sys_free(mybuf);
  exit:
    _Py_dg_freedtoa(buf);
    return result;
}

/* float.__round__(ndigits) for an integer ndigits (float___round___impl). */
static int
qp_double_round(double x, Py_ssize_t ndigits, double *out, qp_error *err)
{
    /* nans and infinities round to themselves */
    if (!isfinite(x)) {
        *out = x;
        return 0;
    }

    /* Deal with extreme values for ndigits. For ndigits > NDIGITS_MAX, x
       always rounds to itself.  For ndigits < NDIGITS_MIN, x always
       rounds to +-0.0.  Here 0.30103 is an upper bound for log10(2). */
#define QP_NDIGITS_MAX ((int)((DBL_MANT_DIG-DBL_MIN_EXP) * 0.30103))
#define QP_NDIGITS_MIN (-(int)((DBL_MAX_EXP + 1) * 0.30103))
    if (ndigits > QP_NDIGITS_MAX) {
        /* return x */
        *out = x;
        return 0;
    }
    else if (ndigits < QP_NDIGITS_MIN) {
        /* return 0.0, but with sign of x */
        *out = 0.0*x;
        return 0;
    }
    else
        /* finite x, and ndigits is not unreasonably large */
        return qp_double_round_impl(x, (int)ndigits, out, err);
#undef QP_NDIGITS_MAX
#undef QP_NDIGITS_MIN
}

/* float.__round__(None): round half to even (the caller converts to int). */
static double
qp_double_round_half_even(double x)
{
    double rounded = round(x);
    if (fabs(x-rounded) == 0.5)
        /* halfway case: round to even */
        rounded = 2.0*round(x/2.0);
    return rounded;
}


/* ====================================================================== */
/* Section 3: Objects/unicode_formatter.c (format() mini-language)        */
/* ====================================================================== */

/* Differences from CPython's file, none of them visible from Python:
   - output goes to a DynBuf as WTF-8 instead of a _PyUnicodeWriter, so
     the "count, prepare writer, fill" structure is kept but the fill pass
     appends in order (left padding, content, right padding);
   - all digits, signs, prefixes, separators and decimal points are ASCII,
     so the grouping code works on bytes; only the fill character and str
     values can be non-ASCII and are written code point by code point;
   - the current locale ('n') is the C locale (see numfmt.h);
   - format specs are decoded to a code-point array first, so the parser
     indexes code points exactly like PyUnicode_READ does. */

/* ---- text helpers ---------------------------------------------------- */

/* Decode the code point at units[*ppos] of a (data, kind, len) text and
   advance *ppos: Latin-1 byte, UTF-16 unit (a high surrogate followed by
   a low surrogate is combined) or UCS4 value. */
static uint32_t
qp_fu_text_next(const void *data, int kind, size_t len, size_t *ppos)
{
    size_t pos = *ppos;
    uint32_t c, c2;

    switch (kind) {
    case QP_FMT_KIND_LATIN1:
        c = ((const uint8_t *)data)[pos++];
        break;
    case QP_FMT_KIND_UTF16:
        c = ((const uint16_t *)data)[pos++];
        if (c >= 0xD800 && c < 0xDC00 && pos < len) {
            c2 = ((const uint16_t *)data)[pos];
            if (c2 >= 0xDC00 && c2 < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                pos++;
            }
        }
        break;
    default:
        assert(kind == QP_FMT_KIND_UCS4);
        c = ((const uint32_t *)data)[pos++];
        break;
    }
    *ppos = pos;
    return c;
}

/* Number of code points of a text (PyUnicode_GET_LENGTH). */
static Py_ssize_t
qp_fu_text_length(const void *data, int kind, size_t len)
{
    size_t pos = 0;
    Py_ssize_t n;

    if (kind != QP_FMT_KIND_UTF16)
        return (Py_ssize_t)len;
    n = 0;
    while (pos < len) {
        qp_fu_text_next(data, kind, len, &pos);
        n++;
    }
    return n;
}

/* Append one code point as WTF-8. */
static void
qp_fu_put_cp(DynBuf *out, uint32_t c)
{
    uint8_t tmp[UTF8_CHAR_LEN_MAX];
    int n;

    if (c < 0x80) {
        dbuf_putc(out, (uint8_t)c);
        return;
    }
    n = unicode_to_utf8(tmp, c);
    dbuf_put(out, tmp, n);
}

/* Append the first n code points of a text as WTF-8. */
static void
qp_fu_put_text(DynBuf *out, const void *data, int kind, size_t len, Py_ssize_t n)
{
    size_t pos = 0;

    if (kind == QP_FMT_KIND_LATIN1) {
        const uint8_t *s = data;
        size_t i, run;
        /* copy ASCII runs in one go */
        i = 0;
        while ((Py_ssize_t)i < n) {
            run = i;
            while ((Py_ssize_t)run < n && s[run] < 0x80)
                run++;
            if (run > i)
                dbuf_put(out, s + i, run - i);
            if ((Py_ssize_t)run < n)
                qp_fu_put_cp(out, s[run++]);
            i = run;
        }
        return;
    }
    while (n-- > 0 && pos < len)
        qp_fu_put_cp(out, qp_fu_text_next(data, kind, len, &pos));
}

/* Reserve n bytes at the end of out and return a pointer to them (the
   size already includes them), or NULL (error flag set) on failure. */
static char *
qp_fu_reserve(DynBuf *out, size_t n)
{
    char *p;

    if (dbuf_claim(out, n)) {
        dbuf_set_error(out);
        return NULL;
    }
    p = (char *)out->buf + out->size;
    out->size += n;
    return p;
}

/* _PyUnicode_FastFill: append n copies of the code point c. */
static void
qp_fu_fill(DynBuf *out, uint32_t c, Py_ssize_t n)
{
    uint8_t tmp[UTF8_CHAR_LEN_MAX];
    size_t clen;
    char *p;
    Py_ssize_t i;

    if (n <= 0)
        return;
    clen = c < 0x80 ? 1 : (size_t)unicode_to_utf8(tmp, c);
    if ((size_t)n > SIZE_MAX / clen) {
        dbuf_set_error(out);
        return;
    }
    p = qp_fu_reserve(out, (size_t)n * clen);
    if (!p)
        return;
    if (clen == 1) {
        memset(p, (int)c, (size_t)n);
    } else {
        for (i = 0; i < n; i++) {
            memcpy(p, tmp, clen);
            p += clen;
        }
    }
}

/* Append the code points c[0..n) as WTF-8 to out (for error messages). */
static void
qp_fu_put_ucs4(DynBuf *out, const uint32_t *c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        qp_fu_put_cp(out, c[i]);
}

/* ---- _PyUnicode_InsertThousandsGrouping() helper functions ---------- */

typedef struct {
    const char *grouping;
    char previous;
    Py_ssize_t i; /* Where we're currently pointing in grouping. */
} qp_fu_GroupGenerator;


static void
qp_fu_GroupGenerator_init(qp_fu_GroupGenerator *self, const char *grouping)
{
    self->grouping = grouping;
    self->i = 0;
    self->previous = 0;
}


/* Returns the next grouping, or 0 to signify end. */
static Py_ssize_t
qp_fu_GroupGenerator_next(qp_fu_GroupGenerator *self)
{
    /* Note that we don't really do much error checking here. If a
       grouping string contains just CHAR_MAX, for example, then just
       terminate the generator. That shouldn't happen, but at least we
       fail gracefully. */
    switch (self->grouping[self->i]) {
    case 0:
        return self->previous;
    case CHAR_MAX:
        /* Stop the generator. */
        return 0;
    default: {
        char ch = self->grouping[self->i];
        self->previous = ch;
        self->i++;
        return (Py_ssize_t)ch;
    }
    }
}


/* Fill in some digits, leading zeros, and thousands separator. All
   are optional, depending on when we're called.  buffer == NULL means
   counting mode (nothing written). */
static void
qp_fu_InsertThousandsGrouping_fill(char *buffer, Py_ssize_t *buffer_pos,
                                   const char *digits, Py_ssize_t *digits_pos,
                                   Py_ssize_t n_chars, Py_ssize_t n_zeros,
                                   const char *thousands_sep, Py_ssize_t thousands_sep_len,
                                   int forward)
{
    if (!buffer) {
        return;
    }

    if (thousands_sep) {
        if (!forward) {
            *buffer_pos -= thousands_sep_len;
        }
        /* Copy the thousands_sep chars into the buffer. */
        memcpy(buffer + *buffer_pos, thousands_sep, thousands_sep_len);
        if (forward) {
            *buffer_pos += thousands_sep_len;
        }
    }

    if (!forward) {
        *buffer_pos -= n_chars;
        *digits_pos -= n_chars;
    }
    memcpy(buffer + *buffer_pos, digits + *digits_pos, n_chars);
    if (forward) {
        *buffer_pos += n_chars;
        *digits_pos += n_chars;
    }

    if (n_zeros) {
        if (!forward) {
            *buffer_pos -= n_zeros;
        }
        memset(buffer + *buffer_pos, '0', n_zeros);
        if (forward) {
            *buffer_pos += n_zeros;
        }
    }
}


/**
 * _PyUnicode_InsertThousandsGrouping:
 * @buffer: output area of @n_buffer chars, or NULL for counting mode.
 * @n_buffer: Number of characters in @buffer.
 * @digits: Digits we're reading from. If counting, this is unused.
 * @d_pos: Start of digits string.
 * @n_digits: The number of digits in the string, in which we want
 *            to put the grouping chars.
 * @min_width: The minimum width of the digits in the output string.
 *             Output will be zero-padded on the left to fill.
 * @grouping: see definition in localeconv().
 * @thousands_sep: see definition in localeconv() (ASCII).
 * @forward: fill from the left (fractional part) instead of the right.
 *
 * There are 2 modes: counting and filling. If @buffer is NULL,
 *  we are in counting mode, else filling mode.
 * If counting, the required buffer size is returned.
 * If filling, we know the buffer will be large enough, so we don't
 *  need to pass in the buffer size.
 * Inserts thousand grouping characters (as defined by grouping and
 *  thousands_sep) into @buffer.
 *
 * Return value: number of characters.
 **/
static Py_ssize_t
qp_fu_InsertThousandsGrouping(
    char *buffer,
    Py_ssize_t n_buffer,
    const char *digits,
    Py_ssize_t d_pos,
    Py_ssize_t n_digits,
    Py_ssize_t min_width,
    const char *grouping,
    const char *thousands_sep,
    int forward)
{
    min_width = max_int64(0, min_width);
    assert(0 <= d_pos);
    assert(0 <= n_digits);
    assert(grouping != NULL);

    Py_ssize_t count = 0;
    Py_ssize_t n_zeros;
    int loop_broken = 0;
    int use_separator = 0; /* First time through, don't append the
                              separator. They only go between
                              groups. */
    Py_ssize_t buffer_pos;
    Py_ssize_t digits_pos;
    Py_ssize_t len;
    Py_ssize_t n_chars;
    Py_ssize_t remaining = n_digits; /* Number of chars remaining to
                                        be looked at */
    /* A generator that returns all of the grouping widths, until it
       returns 0. */
    qp_fu_GroupGenerator groupgen;
    qp_fu_GroupGenerator_init(&groupgen, grouping);
    const Py_ssize_t thousands_sep_len = (Py_ssize_t)strlen(thousands_sep);

    /* if digits are not grouped, thousands separator
       should be an empty string */
    assert(!(grouping[0] == CHAR_MAX && thousands_sep_len != 0));

    digits_pos = d_pos + (forward ? 0 : n_digits);
    buffer_pos = forward ? 0 : n_buffer;

    while ((len = qp_fu_GroupGenerator_next(&groupgen)) > 0) {
        len = min_int64(len, max_int64(max_int64(remaining, min_width), 1));
        n_zeros = max_int64(0, len - remaining);
        n_chars = max_int64(0, min_int64(remaining, len));

        /* Use n_zero zero's and n_chars chars */

        /* Count only, don't do anything. */
        count += (use_separator ? thousands_sep_len : 0) + n_zeros + n_chars;

        /* Copy into the buffer. */
        qp_fu_InsertThousandsGrouping_fill(buffer, &buffer_pos,
                                           digits, &digits_pos,
                                           n_chars, n_zeros,
                                           use_separator ? thousands_sep : NULL,
                                           thousands_sep_len, forward);

        /* Use a separator next time. */
        use_separator = 1;

        remaining -= n_chars;
        min_width -= len;

        if (remaining <= 0 && min_width <= 0) {
            loop_broken = 1;
            break;
        }
        min_width -= thousands_sep_len;
    }
    if (!loop_broken) {
        /* We left the loop without using a break statement. */

        len = max_int64(max_int64(remaining, min_width), 1);
        n_zeros = max_int64(0, len - remaining);
        n_chars = max_int64(0, min_int64(remaining, len));

        /* Use n_zero zero's and n_chars chars */
        count += (use_separator ? thousands_sep_len : 0) + n_zeros + n_chars;

        /* Copy into the buffer. */
        qp_fu_InsertThousandsGrouping_fill(buffer, &buffer_pos,
                                           digits, &digits_pos,
                                           n_chars, n_zeros,
                                           use_separator ? thousands_sep : NULL,
                                           thousands_sep_len, forward);
    }
    return count;
}


/* ---- errors ---------------------------------------------------------- */

/* Raises an exception about an unknown presentation type for this
 * type. */
static void
qp_fu_unknown_presentation_type(uint32_t presentation_type,
                                const char *type_name, qp_error *err)
{
    /* %c might be out-of-range, hence the two cases. */
    if (presentation_type > 32 && presentation_type < 128)
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "Unknown format code '%c' "
                     "for object of type '%.*s'",
                     (char)presentation_type,
                     qp_utf8_prefix_len(type_name, 200), type_name);
    else
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "Unknown format code '\\x%x' "
                     "for object of type '%.*s'",
                     (unsigned int)presentation_type,
                     qp_utf8_prefix_len(type_name, 200), type_name);
}

static void
qp_fu_invalid_thousands_separator_type(char specifier, uint32_t presentation_type,
                                       qp_error *err)
{
    assert(specifier == ',' || specifier == '_');
    if (presentation_type > 32 && presentation_type < 128)
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "Cannot specify '%c' with '%c'.",
                     specifier, (char)presentation_type);
    else
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "Cannot specify '%c' with '\\x%x'.",
                     specifier, (unsigned int)presentation_type);
}

static void
qp_fu_invalid_comma_and_underscore(qp_error *err)
{
    qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                 "Cannot specify both ',' and '_'.");
}

/*
    get_integer consumes 0 or more decimal digit characters from an
    input string, updates *result with the corresponding positive
    integer, and returns the number of digits consumed.

    returns -1 on error.
*/
static int
qp_fu_get_integer(const uint32_t *str, Py_ssize_t *ppos, Py_ssize_t end,
                  Py_ssize_t *result, qp_error *err)
{
    Py_ssize_t accumulator, digitval, pos = *ppos;
    int numdigits;

    accumulator = numdigits = 0;
    for (; pos < end; pos++, numdigits++) {
        uint32_t c = str[pos];
        if (c < 128)
            digitval = (c >= '0' && c <= '9') ? (Py_ssize_t)(c - '0') : -1;
        else
            digitval = _PyUnicode_ToDecimalDigit(c);
        if (digitval < 0)
            break;
        /*
           Detect possible overflow before it happens:

              accumulator * 10 + digitval > PY_SSIZE_T_MAX if and only if
              accumulator > (PY_SSIZE_T_MAX - digitval) / 10.
        */
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                         "Too many decimal digits in format string");
            *ppos = pos;
            return -1;
        }
        accumulator = accumulator * 10 + digitval;
    }
    *ppos = pos;
    *result = accumulator;
    return numdigits;
}

/* ---- standard format specifier parsing ------------------------------ */

/* returns true if this character is a specifier alignment token */
static inline int
qp_fu_is_alignment_token(uint32_t c)
{
    switch (c) {
    case '<': case '>': case '=': case '^':
        return 1;
    default:
        return 0;
    }
}

/* returns true if this character is a sign element */
static inline int
qp_fu_is_sign_element(uint32_t c)
{
    switch (c) {
    case ' ': case '+': case '-':
        return 1;
    default:
        return 0;
    }
}

/*
  parse_internal_render_format_spec(): spec[start..end) are code points.
  fills in format with the parsed information.
  returns 1 on success, 0 on failure (err set).
*/
static int
qp_fu_parse_internal_render_format_spec(const char *type_name,
                                        const uint32_t *spec,
                                        Py_ssize_t start, Py_ssize_t end,
                                        qp_format_spec *format,
                                        char default_type,
                                        char default_align,
                                        qp_error *err)
{
    Py_ssize_t pos = start;
    /* end-pos is used throughout this code to specify the length of
       the input string */
#define READ_spec(index) (spec[index])

    Py_ssize_t consumed;
    int align_specified = 0;
    int fill_char_specified = 0;

    format->fill_char = ' ';
    format->align = (unsigned char)default_align;
    format->alternate = 0;
    format->no_neg_0 = 0;
    format->sign = '\0';
    format->width = -1;
    format->thousands_separators = QP_LT_NO_LOCALE;
    format->frac_thousands_separator = QP_LT_NO_LOCALE;
    format->precision = -1;
    format->type = (unsigned char)default_type;

    /* If the second char is an alignment token,
       then parse the fill char */
    if (end-pos >= 2 && qp_fu_is_alignment_token(READ_spec(pos+1))) {
        format->align = READ_spec(pos+1);
        format->fill_char = READ_spec(pos);
        fill_char_specified = 1;
        align_specified = 1;
        pos += 2;
    }
    else if (end-pos >= 1 && qp_fu_is_alignment_token(READ_spec(pos))) {
        format->align = READ_spec(pos);
        align_specified = 1;
        ++pos;
    }

    /* Parse the various sign options */
    if (end-pos >= 1 && qp_fu_is_sign_element(READ_spec(pos))) {
        format->sign = READ_spec(pos);
        ++pos;
    }

    /* If the next character is z, request coercion of negative 0.
       Applies only to floats. */
    if (end-pos >= 1 && READ_spec(pos) == 'z') {
        format->no_neg_0 = 1;
        ++pos;
    }

    /* If the next character is #, we're in alternate mode.  This only
       applies to integers. */
    if (end-pos >= 1 && READ_spec(pos) == '#') {
        format->alternate = 1;
        ++pos;
    }

    /* The special case for 0-padding (backwards compat) */
    if (!fill_char_specified && end-pos >= 1 && READ_spec(pos) == '0') {
        format->fill_char = '0';
        if (!align_specified && default_align == '>') {
            format->align = '=';
        }
        ++pos;
    }

    consumed = qp_fu_get_integer(spec, &pos, end, &format->width, err);
    if (consumed == -1)
        /* Overflow error. Exception already set. */
        return 0;

    /* If consumed is 0, we didn't consume any characters for the
       width. In that case, reset the width to -1, because
       get_integer() will have set it to zero. -1 is how we record
       that the width wasn't specified. */
    if (consumed == 0)
        format->width = -1;

    /* Comma signifies add thousands separators */
    if (end-pos && READ_spec(pos) == ',') {
        format->thousands_separators = QP_LT_DEFAULT_LOCALE;
        ++pos;
    }
    /* Underscore signifies add thousands separators */
    if (end-pos && READ_spec(pos) == '_') {
        if (format->thousands_separators != QP_LT_NO_LOCALE) {
            qp_fu_invalid_comma_and_underscore(err);
            return 0;
        }
        format->thousands_separators = QP_LT_UNDERSCORE_LOCALE;
        ++pos;
    }
    if (end-pos && READ_spec(pos) == ',') {
        if (format->thousands_separators == QP_LT_UNDERSCORE_LOCALE) {
            qp_fu_invalid_comma_and_underscore(err);
            return 0;
        }
    }

    /* Parse field precision */
    if (end-pos && READ_spec(pos) == '.') {
        ++pos;

        consumed = qp_fu_get_integer(spec, &pos, end, &format->precision, err);
        if (consumed == -1)
            /* Overflow error. Exception already set. */
            return 0;

        if (end-pos && READ_spec(pos) == ',') {
            if (consumed == 0) {
                format->precision = -1;
            }
            format->frac_thousands_separator = QP_LT_DEFAULT_LOCALE;
            ++pos;
            ++consumed;
        }
        if (end-pos && READ_spec(pos) == '_') {
            if (format->frac_thousands_separator != QP_LT_NO_LOCALE) {
                qp_fu_invalid_comma_and_underscore(err);
                return 0;
            }
            if (consumed == 0) {
                format->precision = -1;
            }
            format->frac_thousands_separator = QP_LT_UNDERSCORE_LOCALE;
            ++pos;
            ++consumed;
        }
        if (end-pos && READ_spec(pos) == ',') {
            if (format->frac_thousands_separator == QP_LT_UNDERSCORE_LOCALE) {
                qp_fu_invalid_comma_and_underscore(err);
                return 0;
            }
        }

        /* Not having a precision or underscore/comma after a dot
           is an error. */
        if (consumed == 0) {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                         "Format specifier missing precision");
            return 0;
        }

    }

    /* Finally, parse the type field. */

    if (end-pos > 1) {
        /* More than one char remains, so this is an invalid format
           specifier. */
        /* The message quotes the format spec we're operating on, i.e.
           format_spec[start:end] (in Python syntax). */
        DynBuf b;
        dbuf_init(&b);
        qp_fu_put_ucs4(&b, spec + start, end - start);
        if (dbuf_error(&b)) {
            qp_numfmt_nomem(err);
        } else {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                         "Invalid format specifier '%.*s' for object of type '%.*s'",
                         (int)b.size, b.size ? (const char *)b.buf : "",
                         qp_utf8_prefix_len(type_name, 200), type_name);
        }
        dbuf_free(&b);
        return 0;
    }

    if (end-pos == 1) {
        format->type = READ_spec(pos);
        ++pos;
    }

    /* Do as much validating as we can, just by looking at the format
       specifier.  Do not take into account what type of formatting
       we're doing (int, float, string). */

    if (format->thousands_separators) {
        switch (format->type) {
        case 'd':
        case 'e':
        case 'f':
        case 'g':
        case 'E':
        case 'G':
        case '%':
        case 'F':
        case '\0':
            /* These are allowed. See PEP 378.*/
            break;
        case 'b':
        case 'o':
        case 'x':
        case 'X':
            /* Underscores are allowed in bin/oct/hex. See PEP 515. */
            if (format->thousands_separators == QP_LT_UNDERSCORE_LOCALE) {
                /* Every four digits, not every three, in bin/oct/hex. */
                format->thousands_separators = QP_LT_UNDER_FOUR_LOCALE;
                break;
            }
            /* fall through */
        default:
            qp_fu_invalid_thousands_separator_type((char)format->thousands_separators,
                                                   format->type, err);
            return 0;
        }
    }

    if (format->type == 'n'
        && format->frac_thousands_separator != QP_LT_NO_LOCALE)
    {
        qp_fu_invalid_thousands_separator_type((char)format->frac_thousands_separator,
                                               format->type, err);
        return 0;
    }

    assert (format->align <= 127);
    assert (format->sign <= 127);
    return 1;
#undef READ_spec
}

/* Parse a format spec given as (data, kind, len): decode it to code
   points, then parse_internal_render_format_spec. */
static int
qp_format_spec_parse(qp_format_spec *format, const void *spec, int spec_kind,
                     size_t spec_len, char default_type, char default_align,
                     const char *type_name, qp_error *err)
{
    uint32_t stackbuf[64], *cps = stackbuf;
    size_t pos = 0, n = 0;
    int ok;

    if (spec_kind == QP_FMT_KIND_UCS4) {
        cps = (uint32_t *)spec;
        n = spec_len;
    } else {
        if (spec_len > countof(stackbuf)) {
            cps = qp_sys_malloc(spec_len * sizeof(uint32_t));
            if (!cps) {
                qp_numfmt_nomem(err);
                return -1;
            }
        }
        while (pos < spec_len)
            cps[n++] = qp_fu_text_next(spec, spec_kind, spec_len, &pos);
    }
    ok = qp_fu_parse_internal_render_format_spec(type_name, cps, 0, (Py_ssize_t)n,
                                                 format, default_type,
                                                 default_align, err);
    if (cps != stackbuf && cps != (const uint32_t *)spec)
        qp_sys_free(cps);
    return ok ? 0 : -1;
}

/* Calculate the padding needed. */
static void
qp_fu_calc_padding(Py_ssize_t nchars, Py_ssize_t width, uint32_t align,
                   Py_ssize_t *n_lpadding, Py_ssize_t *n_rpadding,
                   Py_ssize_t *n_total)
{
    if (width >= 0) {
        if (nchars > width)
            *n_total = nchars;
        else
            *n_total = width;
    }
    else {
        /* not specified, use all of the chars and no more */
        *n_total = nchars;
    }

    /* Figure out how much leading space we need, based on the
       aligning */
    if (align == '>')
        *n_lpadding = *n_total - nchars;
    else if (align == '^')
        *n_lpadding = (*n_total - nchars) / 2;
    else if (align == '<' || align == '=')
        *n_lpadding = 0;
    else {
        /* We should never have an unspecified alignment. */
        abort();
    }

    *n_rpadding = *n_total - nchars - *n_lpadding;
}

/* ---- common routines for numeric formatting ------------------------- */

/* Locale info needed for formatting integers and the part of floats
   before and including the decimal.  All strings are ASCII here. */
typedef struct {
    const char *decimal_point;
    const char *thousands_sep;
    const char *frac_thousands_sep;
    const char *grouping;
} qp_fu_LocaleInfo;

/* describes the layout for an integer, see the comment in
   calc_number_widths() for details */
typedef struct {
    Py_ssize_t n_lpadding;
    Py_ssize_t n_prefix;
    Py_ssize_t n_spadding;
    Py_ssize_t n_rpadding;
    char sign;
    Py_ssize_t n_sign;      /* number of digits needed for sign (0/1) */
    Py_ssize_t n_grouped_digits; /* Space taken up by the digits, including
                                    any grouping chars. */
    Py_ssize_t n_decimal;   /* 0 if only an integer */
    Py_ssize_t n_remainder; /* Digits in decimal and/or exponent part,
                               excluding the decimal itself, if
                               present. */
    Py_ssize_t n_frac;
    Py_ssize_t n_grouped_frac_digits;

    /* These 2 are not the widths of fields, but are needed by
       STRINGLIB_GROUPING. */
    Py_ssize_t n_digits;    /* The number of digits before a decimal
                               or exponent. */
    Py_ssize_t n_min_width; /* The min_width we used when we computed
                               the n_grouped_digits width. */
} qp_fu_NumberFieldWidths;


/* Given a number of the form:
   digits[remainder]
   where ptr points to the start and end points to the end, find where
    the integer part ends. This could be a decimal, an exponent, both,
    or neither.
   If a decimal point is present, set *has_decimal and increment
    remainder beyond it.
   Results are undefined (but shouldn't crash) for improperly
    formatted strings.
*/
static void
qp_fu_parse_number(const char *s, Py_ssize_t pos, Py_ssize_t end,
                   Py_ssize_t *n_remainder, Py_ssize_t *n_frac, int *has_decimal)
{
    Py_ssize_t frac;

    while (pos<end && s[pos] >= '0' && s[pos] <= '9') {
        ++pos;
    }
    frac = pos;

    /* Does remainder start with a decimal point? */
    *has_decimal = pos<end && s[frac] == '.';

    /* Skip the decimal point. */
    if (*has_decimal) {
        frac++;
        pos++;
    }

    while (pos<end && s[pos] >= '0' && s[pos] <= '9') {
        ++pos;
    }

    *n_frac = pos - frac;
    *n_remainder = end - pos;
}

/* not all fields of format are used.  for example, precision is
   unused.  Returns the total width in code points. */
static Py_ssize_t
qp_fu_calc_number_widths(qp_fu_NumberFieldWidths *spec, Py_ssize_t n_prefix,
                         char sign_char, Py_ssize_t n_start,
                         Py_ssize_t n_end, Py_ssize_t n_remainder, Py_ssize_t n_frac,
                         int has_decimal, const qp_fu_LocaleInfo *locale,
                         const qp_format_spec *format)
{
    Py_ssize_t n_non_digit_non_padding;
    Py_ssize_t n_padding;

    spec->n_digits = n_end - n_start - n_frac - n_remainder - (has_decimal?1:0);
    spec->n_lpadding = 0;
    spec->n_prefix = n_prefix;
    spec->n_decimal = has_decimal ? (Py_ssize_t)strlen(locale->decimal_point) : 0;
    spec->n_remainder = n_remainder;
    spec->n_frac = n_frac;
    spec->n_spadding = 0;
    spec->n_rpadding = 0;
    spec->sign = '\0';
    spec->n_sign = 0;

    /* the output will look like:
       |                                                                                         |
       | <lpadding> <sign> <prefix> <spadding> <grouped_digits> <decimal> <remainder> <rpadding> |
       |                                                                                         |

       sign is computed from format->sign and the actual
       sign of the number

       prefix is given (it's for the '0x' prefix)

       digits is already known

       the total width is either given, or computed from the
       actual digits

       only one of lpadding, spadding, and rpadding can be non-zero,
       and it's calculated from the width and other fields
    */

    /* compute the various parts we're going to write */
    switch (format->sign) {
    case '+':
        /* always put a + or - */
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : '+');
        break;
    case ' ':
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : ' ');
        break;
    default:
        /* Not specified, or the default (-) */
        if (sign_char == '-') {
            spec->n_sign = 1;
            spec->sign = '-';
        }
    }

    if (spec->n_frac == 0) {
        spec->n_grouped_frac_digits = 0;
    }
    else {
        spec->n_grouped_frac_digits = qp_fu_InsertThousandsGrouping(
            NULL, 0,
            NULL, 0, spec->n_frac,
            spec->n_frac,
            locale->grouping, locale->frac_thousands_sep, 1);
    }

    /* The number of chars used for non-digits and non-padding. */
    n_non_digit_non_padding = spec->n_sign + spec->n_prefix + spec->n_decimal +
        + spec->n_frac + spec->n_remainder;

    /* min_width can go negative, that's okay. format->width == -1 means
       we don't care. */
    if (format->fill_char == '0' && format->align == '=')
        spec->n_min_width = (format->width - n_non_digit_non_padding
                             + spec->n_frac - spec->n_grouped_frac_digits);
    else
        spec->n_min_width = 0;

    if (spec->n_digits == 0)
        /* This case only occurs when using 'c' formatting, we need
           to special case it because the grouping code always wants
           to have at least one character. */
        spec->n_grouped_digits = 0;
    else {
        spec->n_grouped_digits = qp_fu_InsertThousandsGrouping(
            NULL, 0,
            NULL, 0, spec->n_digits,
            spec->n_min_width,
            locale->grouping, locale->thousands_sep, 0);
    }

    /* Given the desired width and the total of digit and non-digit
       space we consume, see if we need any padding. format->width can
       be negative (meaning no padding), but this code still works in
       that case. */
    n_padding = format->width -
                        (n_non_digit_non_padding + spec->n_grouped_digits
                         + spec->n_grouped_frac_digits - spec->n_frac);
    if (n_padding > 0) {
        /* Some padding is needed. Determine if it's left, space, or right. */
        switch (format->align) {
        case '<':
            spec->n_rpadding = n_padding;
            break;
        case '^':
            spec->n_lpadding = n_padding / 2;
            spec->n_rpadding = n_padding - spec->n_lpadding;
            break;
        case '=':
            spec->n_spadding = n_padding;
            break;
        case '>':
            spec->n_lpadding = n_padding;
            break;
        default:
            /* Shouldn't get here */
            abort();
        }
    }

    return spec->n_lpadding + spec->n_sign + spec->n_prefix +
        spec->n_spadding + spec->n_grouped_digits + spec->n_decimal +
        spec->n_grouped_frac_digits + spec->n_remainder + spec->n_rpadding;
}

/* Fill in the digit parts of a number's string representation,
   as determined in calc_number_widths().  digits[0..digits_len) holds
   the formatted number (ASCII, except for the character of 'c'
   formatting, which is its WTF-8 encoding and always the remainder, i.e.
   the tail of digits).  Appends to out; memory errors are left in the
   DynBuf error flag. */
static void
qp_fu_fill_number(DynBuf *out, const qp_fu_NumberFieldWidths *spec,
                  const char *digits, Py_ssize_t digits_len, Py_ssize_t d_start,
                  const char *prefix, Py_ssize_t p_start,
                  uint32_t fill_char,
                  const qp_fu_LocaleInfo *locale, int toupper)
{
    /* Used to keep track of digits, decimal, and remainder. */
    Py_ssize_t d_pos = d_start;
    Py_ssize_t r;
    char *p;

    if (spec->n_lpadding) {
        qp_fu_fill(out, fill_char, spec->n_lpadding);
    }
    if (spec->n_sign == 1) {
        dbuf_putc(out, (uint8_t)spec->sign);
    }
    if (spec->n_prefix) {
        p = qp_fu_reserve(out, spec->n_prefix);
        if (p) {
            memcpy(p, prefix + p_start, spec->n_prefix);
            if (toupper) {
                Py_ssize_t t;
                for (t = 0; t < spec->n_prefix; t++) {
                    if (p[t] >= 'a' && p[t] <= 'z')
                        p[t] -= 'a' - 'A';
                }
            }
        }
    }
    if (spec->n_spadding) {
        qp_fu_fill(out, fill_char, spec->n_spadding);
    }

    p = qp_fu_reserve(out, spec->n_grouped_digits);
    if (!p)
        return;
    /* Only for type 'c' special case, it has no digits. */
    if (spec->n_digits != 0) {
        /* Fill the digits with InsertThousandsGrouping. */
        r = qp_fu_InsertThousandsGrouping(
                p, spec->n_grouped_digits,
                digits, d_pos, spec->n_digits,
                spec->n_min_width,
                locale->grouping, locale->thousands_sep, 0);
        assert(r == spec->n_grouped_digits);
        (void)r;
        d_pos += spec->n_digits;
    }
    if (toupper) {
        Py_ssize_t t;
        for (t = 0; t < spec->n_grouped_digits; t++) {
            if (p[t] >= 'a' && p[t] <= 'z')
                p[t] -= 'a' - 'A';
        }
    }

    if (spec->n_decimal) {
        dbuf_put(out, (const uint8_t *)locale->decimal_point, spec->n_decimal);
        d_pos += 1;
    }

    if (spec->n_frac) {
        p = qp_fu_reserve(out, spec->n_grouped_frac_digits);
        if (!p)
            return;
        r = qp_fu_InsertThousandsGrouping(
                p, spec->n_grouped_frac_digits,
                digits, d_pos, spec->n_frac, spec->n_frac,
                locale->grouping, locale->frac_thousands_sep, 1);
        assert(r == spec->n_grouped_frac_digits);
        d_pos += spec->n_frac;
    }

    if (spec->n_remainder) {
        dbuf_put(out, (const uint8_t *)digits + d_pos, digits_len - d_pos);
    }

    if (spec->n_rpadding) {
        qp_fu_fill(out, fill_char, spec->n_rpadding);
    }
}

static const char qp_fu_no_grouping[1] = {CHAR_MAX};

/* Find the decimal point character(s?), thousands_separator(s?), and
   grouping description, either for the current locale if type is
   LT_CURRENT_LOCALE (always the C locale here: CPython never changes
   LC_NUMERIC unless locale.setlocale() is called), a hard-coded locale if
   LT_DEFAULT_LOCALE or LT_UNDERSCORE_LOCALE/LT_UNDER_FOUR_LOCALE, or none
   if LT_NO_LOCALE. */
static void
qp_fu_get_locale_info(int type, int frac_type, qp_fu_LocaleInfo *locale_info)
{
    switch (type) {
    case QP_LT_CURRENT_LOCALE:
        /* localeconv() of the "C" locale */
        locale_info->decimal_point = ".";
        locale_info->thousands_sep = "";
        locale_info->grouping = "";
        break;
    case QP_LT_DEFAULT_LOCALE:
    case QP_LT_UNDERSCORE_LOCALE:
    case QP_LT_UNDER_FOUR_LOCALE:
        locale_info->decimal_point = ".";
        locale_info->thousands_sep = type == QP_LT_DEFAULT_LOCALE ? "," : "_";
        if (type != QP_LT_UNDER_FOUR_LOCALE)
            locale_info->grouping = "\3"; /* Group every 3 characters.  The
                                         (implicit) trailing 0 means repeat
                                         infinitely. */
        else
            locale_info->grouping = "\4"; /* Bin/oct/hex group every four. */
        break;
    default:
    case QP_LT_NO_LOCALE:
        locale_info->decimal_point = ".";
        locale_info->thousands_sep = "";
        locale_info->grouping = qp_fu_no_grouping;
        break;
    }
    if (frac_type != QP_LT_NO_LOCALE) {
        locale_info->frac_thousands_sep = frac_type == QP_LT_DEFAULT_LOCALE ? "," : "_";
        if (locale_info->grouping == qp_fu_no_grouping) {
            locale_info->grouping = "\3";
        }
    }
    else {
        locale_info->frac_thousands_sep = "";
    }
}

/* Finish a formatting call: report a DynBuf allocation failure. */
static int
qp_fu_finish(DynBuf *out, qp_error *err)
{
    if (dbuf_error(out)) {
        qp_numfmt_nomem(err);
        return -1;
    }
    return 0;
}

/* ---- string formatting ---------------------------------------------- */

static int
qp_format_string_internal(DynBuf *out, const qp_format_spec *format,
                          const void *value, int value_kind, size_t value_len,
                          qp_error *err)
{
    Py_ssize_t lpad;
    Py_ssize_t rpad;
    Py_ssize_t total;
    Py_ssize_t len;

    len = qp_fu_text_length(value, value_kind, value_len);

    /* sign is not allowed on strings */
    if (format->sign != '\0') {
        if (format->sign == ' ') {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                "Space not allowed in string format specifier");
        }
        else {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                "Sign not allowed in string format specifier");
        }
        return -1;
    }

    /* negative 0 coercion is not allowed on strings */
    if (format->no_neg_0) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Negative zero coercion (z) not allowed in string format "
                     "specifier");
        return -1;
    }

    /* alternate is not allowed on strings */
    if (format->alternate) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Alternate form (#) not allowed in string format "
                     "specifier");
        return -1;
    }

    /* '=' alignment not allowed on strings */
    if (format->align == '=') {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "'=' alignment not allowed "
                     "in string format specifier");
        return -1;
    }

    if ((format->width == -1 || format->width <= len)
        && (format->precision == -1 || format->precision >= len)) {
        /* Fast path */
        qp_fu_put_text(out, value, value_kind, value_len, len);
        return qp_fu_finish(out, err);
    }

    /* if precision is specified, output no more that format.precision
       characters */
    if (format->precision >= 0 && len >= format->precision) {
        len = format->precision;
    }

    qp_fu_calc_padding(len, format->width, format->align, &lpad, &rpad, &total);

    /* Write the padding and the source string. */
    qp_fu_fill(out, format->fill_char, lpad);
    if (len) {
        qp_fu_put_text(out, value, value_kind, value_len, len);
    }
    qp_fu_fill(out, format->fill_char, rpad);
    return qp_fu_finish(out, err);
}

/* ---- long formatting ------------------------------------------------ */

/* The checks at the top of format_long_internal(). */
static int
qp_fu_long_check(const qp_format_spec *format, qp_error *err)
{
    /* no precision allowed on integers */
    if (format->precision != -1) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Precision not allowed in integer format specifier");
        return -1;
    }
    /* no negative zero coercion on integers */
    if (format->no_neg_0) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Negative zero coercion (z) not allowed in integer"
                     " format specifier");
        return -1;
    }
    return 0;
}

/* _PyLong_FormatAdvancedWriter's type switch. */
static int
qp_format_long_classify(const qp_format_spec *format, int *pbase,
                        const char *type_name, qp_error *err)
{
    switch (format->type) {
    case 'b':
        *pbase = 2;
        return QP_FMT_LONG_DIGITS;
    case 'o':
        *pbase = 8;
        return QP_FMT_LONG_DIGITS;
    case 'x':
    case 'X':
        *pbase = 16;
        return QP_FMT_LONG_DIGITS;
    case 'd':
    case 'n':
        *pbase = 10;
        return QP_FMT_LONG_DIGITS;
    case 'c':
        return QP_FMT_LONG_CHAR;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%':
        return QP_FMT_LONG_FLOAT;
    default:
        /* unknown */
        qp_fu_unknown_presentation_type(format->type, type_name, err);
        return -1;
    }
}

/* format_long_internal() for b d n o x X; see numfmt.h. */
static int
qp_format_long(DynBuf *out, const qp_format_spec *format, int negative,
               const char *digits, size_t n_digits_in, qp_error *err)
{
    char sign_char = '\0';
    Py_ssize_t n_prefix = 0;   /* Count of prefix chars, (e.g., '0x') */
    const char *prefix = "";
    qp_fu_NumberFieldWidths spec;
    qp_fu_LocaleInfo locale;

    if (qp_fu_long_check(format, err))
        return -1;

    /* The number of prefix chars is the same as the leading chars
       PyNumber_ToBase adds */
    switch (format->type) {
    case 'b':
        prefix = "0b";
        break;
    case 'o':
        prefix = "0o";
        break;
    case 'x':
    case 'X':
        prefix = "0x";
        break;
    default:
        break;
    }
    if (format->alternate)
        n_prefix = (Py_ssize_t)strlen(prefix);

    if (negative)
        sign_char = '-';

    /* Determine the grouping, separator, and decimal point, if any. */
    qp_fu_get_locale_info(format->type == 'n' ? QP_LT_CURRENT_LOCALE :
                          format->thousands_separators, 0,
                          &locale);

    /* Calculate the widths. */
    qp_fu_calc_number_widths(&spec, n_prefix, sign_char, 0,
                             (Py_ssize_t)n_digits_in, 0, 0, 0,
                             &locale, format);

    /* Populate the memory. */
    qp_fu_fill_number(out, &spec,
                      digits, (Py_ssize_t)n_digits_in, 0,
                      prefix, 0, format->fill_char,
                      &locale, format->type == 'X');
    return qp_fu_finish(out, err);
}

/* format_long_internal() for 'c'; see numfmt.h. */
static int
qp_format_long_char(DynBuf *out, const qp_format_spec *format,
                    int64_t x, int value_fits, qp_error *err)
{
    qp_fu_NumberFieldWidths spec;
    qp_fu_LocaleInfo locale;
    uint8_t tmp[UTF8_CHAR_LEN_MAX];
    int tmp_len;

    if (qp_fu_long_check(format, err))
        return -1;

    /* error to specify a sign */
    if (format->sign != '\0') {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Sign not allowed with integer"
                     " format specifier 'c'");
        return -1;
    }
    /* error to request alternate format */
    if (format->alternate) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Alternate form (#) not allowed with integer"
                     " format specifier 'c'");
        return -1;
    }

    /* taken from unicodeobject.c formatchar() */
    /* Integer input truncated to a character */
    if (!value_fits) {
        /* PyLong_AsLong() */
        qp_error_set(err, QP_ERR_OVERFLOW, 0, 0, 0, 0, "%s",
                     "Python int too large to convert to C long");
        return -1;
    }
    if (x < 0 || x > 0x10ffff) {
        qp_error_set(err, QP_ERR_OVERFLOW, 0, 0, 0, 0, "%s",
                     "%c arg not in range(0x110000)");
        return -1;
    }
    tmp_len = unicode_to_utf8(tmp, (unsigned int)x);

    /* As a sort-of hack, we tell calc_number_widths that we only
       have "remainder" characters. calc_number_widths thinks
       these are characters that don't get formatted, only copied
       into the output string. We do this for 'c' formatting,
       because the characters are likely to be non-digits. */
    qp_fu_get_locale_info(format->thousands_separators, 0, &locale);
    qp_fu_calc_number_widths(&spec, 0, '\0', 0, 1, 1, 0, 0,
                             &locale, format);
    qp_fu_fill_number(out, &spec,
                      (const char *)tmp, tmp_len, 0,
                      "", 0, format->fill_char,
                      &locale, 0);
    return qp_fu_finish(out, err);
}

/* ---- float formatting ----------------------------------------------- */

/* much of this is taken from unicodeobject.c */
static int
qp_format_float_internal(DynBuf *out, const qp_format_spec *format,
                         double val, qp_error *err)
{
    char *buf = NULL;       /* buffer returned from qp_double_to_string */
    Py_ssize_t n_digits;
    Py_ssize_t n_remainder;
    Py_ssize_t n_frac;
    int has_decimal;
    int precision, default_precision = 6;
    uint32_t type = format->type;
    int add_pct = 0;
    Py_ssize_t index;
    qp_fu_NumberFieldWidths spec;
    int flags = 0;
    char sign_char = '\0';
    int float_type; /* Used to see if we have a nan, inf, or regular float. */
    qp_fu_LocaleInfo locale;

    if (format->precision > INT_MAX) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s", "precision too big");
        return -1;
    }
    precision = (int)format->precision;

    if (format->alternate)
        flags |= QP_DTSF_ALT;
    if (format->no_neg_0)
        flags |= QP_DTSF_NO_NEG_0;

    if (type == '\0') {
        /* Omitted type specifier.  Behaves in the same way as repr(x)
           and str(x) if no precision is given, else like 'g', but with
           at least one digit after the decimal point. */
        flags |= QP_DTSF_ADD_DOT_0;
        type = 'r';
        default_precision = 0;
    }

    if (type == 'n')
        /* 'n' is the same as 'g', except for the locale used to
           format the result. We take care of that later. */
        type = 'g';

    if (type == '%') {
        type = 'f';
        val *= 100;
        add_pct = 1;
    }

    if (precision < 0)
        precision = default_precision;
    else if (type == 'r')
        type = 'g';

    /* Cast "type", because if we're in unicode we need to pass an
       8-bit char. This is safe, because we've restricted what "type"
       can be. */
    buf = qp_double_to_string(val, (char)type, precision, flags,
                              &float_type, err);
    if (buf == NULL)
        return -1;
    n_digits = (Py_ssize_t)strlen(buf);

    if (add_pct) {
        /* We know that buf has a trailing zero (since we just called
           strlen() on it), and we don't use that fact any more. So we
           can just write over the trailing zero. */
        buf[n_digits] = '%';
        n_digits += 1;
    }

    if (format->sign != '+' && format->sign != ' '
        && format->width == -1
        && format->type != 'n'
        && !format->thousands_separators
        && !format->frac_thousands_separator)
    {
        /* Fast path */
        dbuf_put(out, (const uint8_t *)buf, n_digits);
        qp_sys_free(buf);
        return qp_fu_finish(out, err);
    }

    /* Is a sign character present in the output?  If so, remember it
       and skip it */
    index = 0;
    if (buf[index] == '-') {
        sign_char = '-';
        ++index;
        --n_digits;
    }

    /* Determine if we have any "remainder" (after the digits, might include
       decimal or exponent or both (or neither)) */
    qp_fu_parse_number(buf, index, index + n_digits,
                       &n_remainder, &n_frac, &has_decimal);

    /* Determine the grouping, separator, and decimal point, if any. */
    qp_fu_get_locale_info(format->type == 'n' ? QP_LT_CURRENT_LOCALE :
                          format->thousands_separators,
                          format->frac_thousands_separator,
                          &locale);

    /* Calculate the widths. */
    qp_fu_calc_number_widths(&spec, 0, sign_char, index,
                             index + n_digits, n_remainder, n_frac,
                             has_decimal, &locale, format);

    /* Populate the memory. */
    qp_fu_fill_number(out, &spec,
                      buf, index + n_digits, index,
                      NULL, 0, format->fill_char,
                      &locale, 0);
    qp_sys_free(buf);
    return qp_fu_finish(out, err);
}

/* ---- complex formatting --------------------------------------------- */

static int
qp_format_complex_internal(DynBuf *out, double re, double im,
                           const qp_format_spec *format, qp_error *err)
{
    char *re_buf = NULL;       /* buffer returned from qp_double_to_string */
    char *im_buf = NULL;       /* buffer returned from qp_double_to_string */

    qp_format_spec tmp_format = *format;
    Py_ssize_t n_re_digits;
    Py_ssize_t n_im_digits;
    Py_ssize_t n_re_remainder;
    Py_ssize_t n_im_remainder;
    Py_ssize_t n_re_frac;
    Py_ssize_t n_im_frac;
    Py_ssize_t n_re_total;
    Py_ssize_t n_im_total;
    int re_has_decimal;
    int im_has_decimal;
    int precision, default_precision = 6;
    uint32_t type = format->type;
    Py_ssize_t i_re;
    Py_ssize_t i_im;
    qp_fu_NumberFieldWidths re_spec;
    qp_fu_NumberFieldWidths im_spec;
    int flags = 0;
    int result = -1;
    char re_sign_char = '\0';
    char im_sign_char = '\0';
    int re_float_type; /* Used to see if we have a nan, inf, or regular float. */
    int im_float_type;
    int add_parens = 0;
    int skip_re = 0;
    Py_ssize_t lpad;
    Py_ssize_t rpad;
    Py_ssize_t total;
    qp_fu_LocaleInfo locale;

    if (format->precision > INT_MAX) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s", "precision too big");
        goto done;
    }
    precision = (int)format->precision;

    /* Zero padding is not allowed. */
    if (format->fill_char == '0') {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "Zero padding is not allowed in complex format "
                     "specifier");
        goto done;
    }

    /* Neither is '=' alignment . */
    if (format->align == '=') {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "'=' alignment flag is not allowed in complex format "
                     "specifier");
        goto done;
    }

    if (format->alternate)
        flags |= QP_DTSF_ALT;
    if (format->no_neg_0)
        flags |= QP_DTSF_NO_NEG_0;

    if (type == '\0') {
        /* Omitted type specifier. Should be like str(self). */
        type = 'r';
        default_precision = 0;
        if (re == 0.0 && copysign(1.0, re) == 1.0)
            skip_re = 1;
        else
            add_parens = 1;
    }

    if (type == 'n')
        /* 'n' is the same as 'g', except for the locale used to
           format the result. We take care of that later. */
        type = 'g';

    if (precision < 0)
        precision = default_precision;
    else if (type == 'r')
        type = 'g';

    /* Cast "type", because if we're in unicode we need to pass an
       8-bit char. This is safe, because we've restricted what "type"
       can be. */
    re_buf = qp_double_to_string(re, (char)type, precision, flags,
                                 &re_float_type, err);
    if (re_buf == NULL)
        goto done;
    im_buf = qp_double_to_string(im, (char)type, precision, flags,
                                 &im_float_type, err);
    if (im_buf == NULL)
        goto done;

    n_re_digits = (Py_ssize_t)strlen(re_buf);
    n_im_digits = (Py_ssize_t)strlen(im_buf);

    i_re = 0;
    i_im = 0;

    /* Is a sign character present in the output?  If so, remember it
       and skip it */
    if (re_buf[i_re] == '-') {
        re_sign_char = '-';
        ++i_re;
        --n_re_digits;
    }
    if (im_buf[i_im] == '-') {
        im_sign_char = '-';
        ++i_im;
        --n_im_digits;
    }

    /* Determine if we have any "remainder" (after the digits, might include
       decimal or exponent or both (or neither)) */
    qp_fu_parse_number(re_buf, i_re, i_re + n_re_digits,
                       &n_re_remainder, &n_re_frac, &re_has_decimal);
    qp_fu_parse_number(im_buf, i_im, i_im + n_im_digits,
                       &n_im_remainder, &n_im_frac, &im_has_decimal);

    /* Determine the grouping, separator, and decimal point, if any. */
    qp_fu_get_locale_info(format->type == 'n' ? QP_LT_CURRENT_LOCALE :
                          format->thousands_separators,
                          format->frac_thousands_separator,
                          &locale);

    /* Turn off any padding. We'll do it later after we've composed
       the numbers without padding. */
    tmp_format.fill_char = '\0';
    tmp_format.align = '<';
    tmp_format.width = -1;

    /* Calculate how much memory we'll need. */
    n_re_total = qp_fu_calc_number_widths(&re_spec, 0, re_sign_char,
                                          i_re, i_re + n_re_digits, n_re_remainder,
                                          n_re_frac, re_has_decimal, &locale,
                                          &tmp_format);

    /* Same formatting, but always include a sign, unless the real part is
     * going to be omitted, in which case we use whatever sign convention was
     * requested by the original format. */
    if (!skip_re)
        tmp_format.sign = '+';
    n_im_total = qp_fu_calc_number_widths(&im_spec, 0, im_sign_char,
                                          i_im, i_im + n_im_digits, n_im_remainder,
                                          n_im_frac, im_has_decimal, &locale,
                                          &tmp_format);

    if (skip_re)
        n_re_total = 0;

    /* Add 1 for the 'j', and optionally 2 for parens. */
    qp_fu_calc_padding(n_re_total + n_im_total + 1 + add_parens * 2,
                       format->width, format->align, &lpad, &rpad, &total);

    /* Populate the memory. First, the padding. */
    qp_fu_fill(out, format->fill_char, lpad);

    if (add_parens) {
        dbuf_putc(out, '(');
    }

    if (!skip_re) {
        qp_fu_fill_number(out, &re_spec,
                          re_buf, i_re + n_re_digits, i_re,
                          NULL, 0,
                          0,
                          &locale, 0);
    }
    qp_fu_fill_number(out, &im_spec,
                      im_buf, i_im + n_im_digits, i_im,
                      NULL, 0,
                      0,
                      &locale, 0);
    dbuf_putc(out, 'j');

    if (add_parens) {
        dbuf_putc(out, ')');
    }

    qp_fu_fill(out, format->fill_char, rpad);
    result = qp_fu_finish(out, err);

done:
    qp_sys_free(re_buf);
    qp_sys_free(im_buf);
    return result;
}

/* ---- built in formatters -------------------------------------------- */

/* _PyUnicode_FormatAdvancedWriter for a non-empty spec. */
static int
qp_format_str(DynBuf *out, const void *value, int value_kind, size_t value_len,
              const void *spec, int spec_kind, size_t spec_len,
              const char *type_name, qp_error *err)
{
    qp_format_spec format;

    assert(spec_len > 0);
    /* parse the format_spec */
    if (qp_format_spec_parse(&format, spec, spec_kind, spec_len, 's', '<',
                             type_name, err))
        return -1;

    /* type conversion? */
    switch (format.type) {
    case 's':
        /* no type conversion needed, already a string.  do the formatting */
        return qp_format_string_internal(out, &format, value, value_kind,
                                         value_len, err);
    default:
        /* unknown */
        qp_fu_unknown_presentation_type(format.type, type_name, err);
        return -1;
    }
}

/* _PyFloat_FormatAdvancedWriter for a non-empty spec. */
static int
qp_format_float(DynBuf *out, double val, const void *spec, int spec_kind,
                size_t spec_len, const char *type_name, qp_error *err)
{
    qp_format_spec format;

    assert(spec_len > 0);
    /* parse the format_spec */
    if (qp_format_spec_parse(&format, spec, spec_kind, spec_len, '\0', '>',
                             type_name, err))
        return -1;

    /* type conversion? */
    switch (format.type) {
    case '\0': /* No format code: like 'g', but with at least one decimal. */
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
    case '%':
        /* no conversion, already a float.  do the formatting */
        return qp_format_float_internal(out, &format, val, err);

    default:
        /* unknown */
        qp_fu_unknown_presentation_type(format.type, type_name, err);
        return -1;
    }
}

/* _PyComplex_FormatAdvancedWriter for a non-empty spec. */
static int
qp_format_complex(DynBuf *out, double re, double im, const void *spec,
                  int spec_kind, size_t spec_len, const char *type_name,
                  qp_error *err)
{
    qp_format_spec format;

    assert(spec_len > 0);
    /* parse the format_spec */
    if (qp_format_spec_parse(&format, spec, spec_kind, spec_len, '\0', '>',
                             type_name, err))
        return -1;

    /* type conversion? */
    switch (format.type) {
    case '\0': /* No format code: like 'g', but with at least one decimal. */
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
        /* no conversion, already a complex.  do the formatting */
        return qp_format_complex_internal(out, re, im, &format, err);

    default:
        /* unknown */
        qp_fu_unknown_presentation_type(format.type, type_name, err);
        return -1;
    }
}


/* ====================================================================== */
/* Section 4: Python/pyhash.c (numeric hashes, SipHash-1-3, seeding)      */
/* ====================================================================== */

/* For numeric types, the hash of a number x is based on the reduction
   of x modulo the prime P = 2**QP_HASH_BITS - 1.  It's designed so that
   hash(x) == hash(y) whenever x and y are numerically equal, even if
   x and y have different types.

   A quick summary of the hashing strategy:

   (1) First define the 'reduction of x modulo P' for any rational
   number x; this is a standard extension of the usual notion of
   reduction modulo P for integers.  If x == p/q (written in lowest
   terms), the reduction is interpreted as the reduction of p times
   the inverse of the reduction of q, all modulo P; if q is exactly
   divisible by P then define the reduction to be infinity.  So we've
   got a well-defined map

      reduce : { rational numbers } -> { 0, 1, 2, ..., P-1, infinity }.

   (2) Now for a rational number x, define hash(x) by:

      reduce(x)   if x >= 0
      -reduce(-x) if x < 0

   If the result of the reduction is infinity (this is impossible for
   integers, floats and Decimals) then use the predefined hash value
   QP_HASH_INF for x >= 0, or -QP_HASH_INF for x < 0, instead.
   QP_HASH_INF and -QP_HASH_INF are also used for the
   hashes of float and Decimal infinities.

   NaNs hash with a pointer hash.  Having distinct hash values prevents
   catastrophic pileups from distinct NaN instances which used to always
   have the same hash value but would compare unequal.

   A selling point for the above strategy is that it makes it possible
   to compute hashes of decimal and binary floating-point numbers
   efficiently, even if the exponent of the binary or decimal number
   is large.  The key point is that

      reduce(x * y) == reduce(x) * reduce(y) (modulo QP_HASH_MODULUS)

   provided that {reduce(x), reduce(y)} != {0, infinity}.  The reduction of a
   binary or decimal float is never infinity, since the denominator is a power
   of 2 (for binary) or a divisor of a power of 10 (for decimal).  So we have,
   for nonnegative x,

      reduce(x * 2**e) == reduce(x) * reduce(2**e) % QP_HASH_MODULUS

      reduce(x * 10**e) == reduce(x) * reduce(10**e) % QP_HASH_MODULUS

   and reduce(10**e) can be computed efficiently by the usual modular
   exponentiation algorithm.  For reduce(2**e) it's even better: since
   P is of the form 2**n-1, reduce(2**e) is 2**(e mod n), and multiplication
   by 2**(e mod n) modulo 2**n-1 just amounts to a rotation of bits.

   */

/* _Py_HashPointerRaw + Py_HashPointer: rotate by 4 to the right (the
   bottom 3 or 4 bits are likely to be 0) and map -1 to -2. */
static qp_hash_t
qp_hash_pointer(uintptr_t p)
{
    uint64_t x = (uint64_t)p;
    qp_hash_t hash;

    x = (x >> 4) | (x << (8 * sizeof(uint64_t) - 4));
    hash = (qp_hash_t)x;
    if (hash == -1) {
        hash = -2;
    }
    return hash;
}

/* _Py_HashDouble(inst, v): see numfmt.h. */
static qp_hash_t
qp_hash_double(double v, uintptr_t identity)
{
    int e, sign;
    double m;
    qp_uhash_t x, y;

    if (!isfinite(v)) {
        if (isinf(v))
            return v > 0 ? QP_HASH_INF : -QP_HASH_INF;
        else
            return qp_hash_pointer(identity);
    }

    m = frexp(v, &e);

    sign = 1;
    if (m < 0) {
        sign = -1;
        m = -m;
    }

    /* process 28 bits at a time;  this should work well both for binary
       and hexadecimal floating point. */
    x = 0;
    while (m) {
        x = ((x << 28) & QP_HASH_MODULUS) | x >> (QP_HASH_BITS - 28);
        m *= 268435456.0;  /* 2**28 */
        e -= 28;
        y = (qp_uhash_t)m;  /* pull out integer part */
        m -= y;
        x += y;
        if (x >= QP_HASH_MODULUS)
            x -= QP_HASH_MODULUS;
    }

    /* adjust for the exponent;  first reduce it modulo QP_HASH_BITS */
    e = e >= 0 ? e % QP_HASH_BITS : QP_HASH_BITS-1-((-1-e) % QP_HASH_BITS);
    x = ((x << e) & QP_HASH_MODULUS) | x >> (QP_HASH_BITS - e);

    x = x * sign;
    if (x == (qp_uhash_t)-1)
        x = (qp_uhash_t)-2;
    return (qp_hash_t)x;
}

/* long_hash() for |v| < 2**63 (or v == -2**63): sign * (|v| mod
   2**61-1), -1 -> -2.  For compact ints (|v| < 2**30) this is v itself,
   exactly CPython's _PyLong_CompactValue shortcut. */
static qp_hash_t
qp_hash_int64(int64_t v)
{
    qp_uhash_t a, x;

    a = v < 0 ? (qp_uhash_t)0 - (qp_uhash_t)v : (qp_uhash_t)v;
    /* a mod (2**61 - 1): 2**61 == 1, so fold the top 3 bits down */
    x = (a & QP_HASH_MODULUS) + (a >> QP_HASH_BITS);
    if (x >= QP_HASH_MODULUS)
        x -= QP_HASH_MODULUS;
    if (v < 0)
        x = (qp_uhash_t)0 - x;
    if (x == (qp_uhash_t)-1)
        x = (qp_uhash_t)-2;
    return (qp_hash_t)x;
}

/* One step of long_hash's loop for a digit of `shift` bits:
   x = (x * 2**shift + digit) mod (2**61 - 1), with 0 <= x < 2**61 - 1,
   shift < 61 and digit < 2**shift.  Multiplying by 2**shift modulo
   2**61 - 1 is a rotation of the 61 bits of x (see longobject.c). */
static inline qp_uhash_t
qp_hash_int_step(qp_uhash_t x, int shift, qp_uhash_t digit)
{
    x = ((x << shift) & QP_HASH_MODULUS) | (x >> (QP_HASH_BITS - shift));
    x += digit;
    if (x >= QP_HASH_MODULUS)
        x -= QP_HASH_MODULUS;
    return x;
}

/* long_hash() for a sign + base 2**32 magnitude (numfmt.h).  CPython walks
   30-bit digits; the result only depends on |v| mod 2**61-1, so any
   digit size gives the same value. */
static qp_hash_t
qp_hash_int_limbs32(int negative, const uint32_t *limbs, size_t n)
{
    qp_uhash_t x = 0;
    size_t i = n;

    while (i > 0) {
        i--;
        x = qp_hash_int_step(x, 32, limbs[i]);
    }
    if (negative)
        x = (qp_uhash_t)0 - x;
    if (x == (qp_uhash_t)-1)
        x = (qp_uhash_t)-2;
    return (qp_hash_t)x;
}

/* long_hash() of a QuickJS bigint given as its two's complement limbs.
   For v < 0 with n = len * JS_LIMB_BITS bits, the limbs read as an
   unsigned number T = v + 2**n, so |v| = 2**n - T and
   |v| mod M = (2**(n mod 61) - T mod M) mod M. */
static qp_hash_t
qp_hash_int_js_limbs(const js_limb_t *tab, size_t len)
{
    qp_uhash_t x = 0, p2;
    size_t i = len;
    int negative;
    uint64_t nbits;

    assert(len >= 1);
    negative = (tab[len - 1] >> (JS_LIMB_BITS - 1)) & 1;
    while (i > 0) {
        js_limb_t l;
        i--;
        l = tab[i];
#if JS_LIMB_BITS == 64
        x = qp_hash_int_step(x, 32, (qp_uhash_t)(l >> 32));
        x = qp_hash_int_step(x, 32, (qp_uhash_t)(l & 0xffffffff));
#else
        x = qp_hash_int_step(x, 32, (qp_uhash_t)l);
#endif
    }
    if (negative) {
        nbits = (uint64_t)len * JS_LIMB_BITS;
        p2 = (qp_uhash_t)1 << (nbits % QP_HASH_BITS);   /* 2**n mod M */
        /* |v| mod M = (p2 - x) mod M, both operands in [0, M] */
        x = p2 >= x ? p2 - x : p2 + QP_HASH_MODULUS - x;
        if (x >= QP_HASH_MODULUS)
            x -= QP_HASH_MODULUS;
        x = (qp_uhash_t)0 - x;
    }
    if (x == (qp_uhash_t)-1)
        x = (qp_uhash_t)-2;
    return (qp_hash_t)x;
}

/* complex_hash(): see numfmt.h. */
static qp_hash_t
qp_hash_complex(double re, double im, uintptr_t identity)
{
    qp_uhash_t hashreal, hashimag, combined;

    hashreal = (qp_uhash_t)qp_hash_double(re, identity);
    hashimag = (qp_uhash_t)qp_hash_double(im, identity);
    /* Note:  if the imaginary part is 0, hashimag is 0 now,
     * so the following returns hashreal unchanged.  This is
     * important because numbers of different types that
     * compare equal must have the same hash value, so that
     * hash(x + 0*j) must equal hash(x).
     */
    combined = hashreal + QP_HASH_IMAG * hashimag;
    if (combined == (qp_uhash_t)-1)
        combined = (qp_uhash_t)-2;
    return (qp_hash_t)combined;
}

/* **************************************************************************
 <MIT License>
 Copyright (c) 2013  Marek Majkowski <marek@popcount.org>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 </MIT License>

 Original location:
    https://github.com/majek/csiphash/

 Solution inspired by code from:
    Samuel Neves (supercop/crypto_auth/siphash24/little)
    djb (supercop/crypto_auth/siphash24/little2)
    Jean-Philippe Aumasson (https://131002.net/siphash/siphash24.c)

 Modified for Python by Christian Heimes:
    - C89 / MSVC compatibility
    - _rotl64() on Windows
    - letoh64() fallback

 Modified for dquickts: incremental form (qp_siphash13_*) so that str
 hashes can be computed over a converted representation without a
 temporary copy; bytes are always read in little-endian order, which is
 CPython's result on little-endian hosts.
*/

#define QP_SIP_ROTATE(x, b) (uint64_t)( ((x) << (b)) | ( (x) >> (64 - (b))) )

#define QP_SIP_HALF_ROUND(a,b,c,d,s,t)     \
    a += b; c += d;                 \
    b = QP_SIP_ROTATE(b, s) ^ a;           \
    d = QP_SIP_ROTATE(d, t) ^ c;           \
    a = QP_SIP_ROTATE(a, 32);

#define QP_SIP_SINGLE_ROUND(v0,v1,v2,v3)   \
    QP_SIP_HALF_ROUND(v0,v1,v2,v3,13,16);  \
    QP_SIP_HALF_ROUND(v2,v1,v0,v3,17,21);

typedef struct {
    uint64_t v0, v1, v2, v3;
    uint64_t tail;      /* pending bytes, little-endian */
    int ntail;          /* number of pending bytes (0..7) */
    uint64_t total;     /* bytes fed so far */
} qp_siphash13_state;

static inline uint64_t
qp_load_le64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
        ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
        ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline void
qp_siphash13_init(qp_siphash13_state *st, const uint8_t key[16])
{
    uint64_t k0 = qp_load_le64(key);
    uint64_t k1 = qp_load_le64(key + 8);

    st->v0 = k0 ^ 0x736f6d6570736575ULL;
    st->v1 = k1 ^ 0x646f72616e646f6dULL;
    st->v2 = k0 ^ 0x6c7967656e657261ULL;
    st->v3 = k1 ^ 0x7465646279746573ULL;
    st->tail = 0;
    st->ntail = 0;
    st->total = 0;
}

/* Compress one 8-byte message word. */
static inline void
qp_siphash13_block(qp_siphash13_state *st, uint64_t mi)
{
    uint64_t v0 = st->v0, v1 = st->v1, v2 = st->v2, v3 = st->v3;

    v3 ^= mi;
    QP_SIP_SINGLE_ROUND(v0,v1,v2,v3);
    v0 ^= mi;
    st->v0 = v0; st->v1 = v1; st->v2 = v2; st->v3 = v3;
}

/* Feed one byte. */
static inline void
qp_siphash13_byte(qp_siphash13_state *st, uint8_t b)
{
    st->tail |= (uint64_t)b << (8 * st->ntail);
    st->total++;
    if (++st->ntail == 8) {
        qp_siphash13_block(st, st->tail);
        st->tail = 0;
        st->ntail = 0;
    }
}

/* Feed a buffer. */
static void
qp_siphash13_update(qp_siphash13_state *st, const uint8_t *in, size_t n)
{
    while (n > 0 && st->ntail != 0) {
        qp_siphash13_byte(st, *in++);
        n--;
    }
    st->total += n - (n & 7);
    while (n >= 8) {
        qp_siphash13_block(st, qp_load_le64(in));
        in += 8;
        n -= 8;
    }
    while (n > 0) {
        qp_siphash13_byte(st, *in++);
        n--;
    }
}

/* Finish: siphash13() of everything fed (the final block holds the
   remaining bytes and the length in its top byte). */
static uint64_t
qp_siphash13_final(qp_siphash13_state *st)
{
    uint64_t v0 = st->v0, v1 = st->v1, v2 = st->v2, v3 = st->v3;
    uint64_t b = (st->total << 56) | st->tail;
    uint64_t t;

    v3 ^= b;
    QP_SIP_SINGLE_ROUND(v0,v1,v2,v3);
    v0 ^= b;
    v2 ^= 0xff;
    QP_SIP_SINGLE_ROUND(v0,v1,v2,v3);
    QP_SIP_SINGLE_ROUND(v0,v1,v2,v3);
    QP_SIP_SINGLE_ROUND(v0,v1,v2,v3);

    /* modified */
    t = (v0 ^ v1) ^ (v2 ^ v3);
    return t;
}

/* Py_HashBuffer's tail: -1 -> -2. */
static inline qp_hash_t
qp_hash_fix(uint64_t h)
{
    qp_hash_t x = (qp_hash_t)h;
    if (x == -1) {
        return -2;
    }
    return x;
}

/* Py_HashBuffer() (Py_HASH_CUTOFF == 0, siphash13). */
static qp_hash_t
qp_hash_buffer(const void *ptr, size_t len, const uint8_t key[16])
{
    qp_siphash13_state st;

    /*
      We make the hash of the empty string be 0, rather than using
      (prefix ^ suffix), since this slightly obfuscates the hash secret
    */
    if (len == 0) {
        return 0;
    }
    qp_siphash13_init(&st, key);
    qp_siphash13_update(&st, ptr, len);
    return qp_hash_fix(qp_siphash13_final(&st));
}

/* hash(str) of an 8-bit string: the UCS1 representation. */
static qp_hash_t
qp_hash_str_latin1(const uint8_t *s, size_t len, const uint8_t key[16])
{
    return qp_hash_buffer(s, len, key);
}

/* Feed code point c as `kind` bytes, little-endian (UCS1/UCS2/UCS4). */
static inline void
qp_siphash13_cp(qp_siphash13_state *st, uint32_t c, int kind)
{
    qp_siphash13_byte(st, (uint8_t)c);
    if (kind >= 2) {
        qp_siphash13_byte(st, (uint8_t)(c >> 8));
        if (kind == 4) {
            qp_siphash13_byte(st, (uint8_t)(c >> 16));
            qp_siphash13_byte(st, 0);
        }
    }
}

/* hash(str) of a 16-bit string: canonical UCS1/UCS2/UCS4 bytes. */
static qp_hash_t
qp_hash_str_utf16(const uint16_t *s, size_t len, const uint8_t key[16])
{
    qp_siphash13_state st;
    size_t i, pos;
    uint32_t maxchar = 0;
    int kind, has_pair = 0;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c > maxchar)
            maxchar = c;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len &&
            s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            has_pair = 1;
    }
    kind = has_pair ? 4 : maxchar < 0x100 ? 1 : 2;
    qp_siphash13_init(&st, key);
    if (kind == 2) {
        /* the UCS2 representation is the 16-bit units themselves */
        for (i = 0; i < len; i++)
            qp_siphash13_cp(&st, s[i], 2);
    } else {
        pos = 0;
        while (pos < len)
            qp_siphash13_cp(&st, qp_fu_text_next(s, QP_FMT_KIND_UTF16, len, &pos), kind);
    }
    return qp_hash_fix(qp_siphash13_final(&st));
}

/* hash(str) of a UCS4 buffer: canonical UCS1/UCS2/UCS4 bytes. */
static qp_hash_t
qp_hash_str_ucs4(const uint32_t *s, size_t len, const uint8_t key[16])
{
    qp_siphash13_state st;
    size_t i;
    uint32_t maxchar = 0;
    int kind;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (s[i] > maxchar)
            maxchar = s[i];
    }
    kind = maxchar < 0x100 ? 1 : maxchar < 0x10000 ? 2 : 4;
    qp_siphash13_init(&st, key);
    for (i = 0; i < len; i++)
        qp_siphash13_cp(&st, s[i], kind);
    return qp_hash_fix(qp_siphash13_final(&st));
}

/* lcg_urandom() of Python/bootstrap_hash.c, truncated to the 16 key
   bytes (the generator is sequential, so these are the first 16 bytes of
   the 24-byte _Py_HashSecret CPython fills). */
static void
qp_hash_key_from_seed(uint32_t seed, uint8_t key[16])
{
    size_t index;
    unsigned int x;

    if (seed == 0) {
        /* disable the randomized hash */
        memset(key, 0, 16);
        return;
    }
    x = seed;
    for (index = 0; index < 16; index++) {
        x *= 214013;
        x += 2531011;
        /* modulo 2 ^ (8 * sizeof(int)) */
        key[index] = (x >> 16) & 0xff;
    }
}

/* Fill key with random bytes.  Returns 0, or -1 if no source works. */
static int
qp_hash_key_random(uint8_t key[16])
{
#ifdef QP_HAVE_ARC4RANDOM
    arc4random_buf(key, 16);
    return 0;
#else
    FILE *f;
    size_t n = 0;

    f = fopen("/dev/urandom", "rb");
    if (f) {
        n = fread(key, 1, 16, f);
        fclose(f);
    }
    if (n == 16)
        return 0;
#if defined(_WIN32)
    {
        /* No /dev/urandom: not cryptographic, but varies per run. */
        uint64_t a = (uint64_t)time(NULL), b = (uint64_t)(uintptr_t)key;
        uint64_t c = (uint64_t)clock();
        int i;
        for (i = 0; i < 16; i++) {
            a = a * 6364136223846793005ULL + (b ^ c) + 1442695040888963407ULL;
            key[i] = (uint8_t)(a >> 56);
        }
        return 0;
    }
#else
    return -1;
#endif
#endif
}

/* config_init_hash_seed + _Py_HashRandomization_Init: see numfmt.h. */
static int
qp_hash_key_init(const char *seed_text, uint8_t key[16], qp_error *err)
{
    /* config_get_env() treats an empty variable as unset */
    if (seed_text && seed_text[0] != '\0' && strcmp(seed_text, "random") != 0) {
        char *endptr = (char *)seed_text;
        unsigned long seed;
        errno = 0;
        seed = strtoul(seed_text, &endptr, 10);
        if (*endptr != '\0'
            || seed > 4294967295UL
            || (errno == ERANGE && seed == ULONG_MAX))
        {
            qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                         "PYTHONHASHSEED must be \"random\" "
                         "or an integer in range [0; 4294967295]");
            return -1;
        }
        qp_hash_key_from_seed((uint32_t)seed, key);
        return 0;
    }
    /* use a random hash seed */
    if (qp_hash_key_random(key) < 0) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "failed to get random numbers to initialize Python");
        return -1;
    }
    return 0;
}

#undef QP_SIP_ROTATE
#undef QP_SIP_HALF_ROUND
#undef QP_SIP_SINGLE_ROUND
#undef QP_DOUBLE_TO_STRING_PRECISION_MAX
#undef QP_OFS_INF
#undef QP_OFS_NAN
#undef QP_OFS_E
#undef QP_HAVE_ARC4RANDOM
