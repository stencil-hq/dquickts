/*
 * Python int and bool types over QuickJS ints and bigints.
 *
 * Derived from CPython 3.15.2 Objects/longobject.c, Objects/boolobject.c,
 * Objects/clinic/longobject.c.h and the PyNumber_Long part of
 * Objects/abstract.c (PSF-2.0).
 *
 * Representation (object.h conventions): an int is JS_TAG_INT (fits
 * int32), JS_TAG_SHORT_BIG_INT (fits a short bigint) or JS_TAG_BIG_INT (a
 * JSBigInt: two's complement limbs, minimal length).  Every int produced
 * here is canonical (qp_long_normalize rules).  bool is JS_TAG_BOOL and
 * instances of int subclasses are JS_CLASS_PY_BOXED objects whose
 * u.object_data holds the plain int.
 *
 * Arithmetic works on "plain ints" (any int tag or a bool, borrowed): a
 * fast path on int64 values and a slow path on the QuickJS JSBigInt
 * helpers (js_bigint_add/mul/divrem/logic/shl/shr...).  QuickJS limits a
 * bigint to JS_BIGINT_MAX_SIZE limbs (1M bits); larger results raise
 * MemoryError, which is what CPython raises when it cannot allocate one.
 *
 * This file also owns qp_init_numbers (int, bool, then float and complex
 * through qp_init_float / qp_init_complex of floatobject.c and
 * complexobject.c).
 */

static int qp_init_float(JSContext *ctx);
static int qp_init_complex(JSContext *ctx);

/* CPython limits (Include/internal/pycore_long.h) */
#define QP_LONG_DEFAULT_MAX_STR_DIGITS 4300
#define QP_LONG_MAX_STR_DIGITS_THRESHOLD 640
#define QP_LONG_CPY_SHIFT 30            /* PyLong_SHIFT, for CPython-exact size computations */
#define QP_LONG_MAX_STR_DIGITS_ERROR_TO_INT \
    "Exceeds the limit (%d digits) for integer string conversion: value has %zd digits; " \
    "use sys.set_int_max_str_digits() to increase the limit"
#define QP_LONG_MAX_STR_DIGITS_ERROR_TO_STR \
    "Exceeds the limit (%d digits) for integer string conversion; " \
    "use sys.set_int_max_str_digits() to increase the limit"

/* Largest magnitude bit length a QuickJS bigint can hold (one bit is the
   sign of the two's complement representation). */
#define QP_LONG_MAX_BITS ((uint64_t)JS_BIGINT_MAX_SIZE * JS_LIMB_BITS - 1)

#define QP_DBL_MANT_DIG 53
#define QP_DBL_MAX_EXP 1024
#define QP_DBL_MIN_EXP (-1021)

static const char qp_long_digit_chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* ========================================================================= */
/* Representation helpers                                                     */
/* ========================================================================= */

static inline BOOL qp_long_tag_is_int(int tag)
{
    return tag == JS_TAG_INT || tag == JS_TAG_SHORT_BIG_INT || tag == JS_TAG_BIG_INT;
}

/* Plain int (borrowed: an int tag or a bool) of an int, bool or int
   subclass instance; QP_NULL if v is not an int. */
static JSValueConst qp_long_unwrap(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (qp_long_tag_is_int(tag) || tag == JS_TAG_BOOL)
        return v;
    if (tag == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (p->class_id == JS_CLASS_PY_BOXED &&
            qp_long_tag_is_int(JS_VALUE_GET_TAG(p->u.object_data)))
            return p->u.object_data;
    }
    return QP_NULL;
}

/* TRUE with *pv if the plain int v fits int64. */
static inline BOOL qp_li_small(JSValueConst v, int64_t *pv)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:
        *pv = JS_VALUE_GET_INT(v);
        return TRUE;
    case JS_TAG_BOOL:
        *pv = JS_VALUE_GET_BOOL(v) != 0;
        return TRUE;
    case JS_TAG_SHORT_BIG_INT:
        *pv = JS_VALUE_GET_SHORT_BIG_INT(v);
        return TRUE;
    case JS_TAG_BIG_INT: {
        JSBigInt *p = JS_VALUE_GET_PTR(v);
        if (p->len == 1) {
            *pv = (js_slimb_t)p->tab[0];
            return TRUE;
        }
#if JS_LIMB_BITS == 32
        if (p->len == 2) {
            *pv = (int64_t)(((uint64_t)p->tab[1] << 32) | p->tab[0]);
            return TRUE;
        }
#endif
        return FALSE;
    }
    default:
        return FALSE;
    }
}

/* JSBigInt view of a plain int (buf is used for small values). */
static JSBigInt *qp_li_big(JSValueConst v, JSBigIntBuf *buf)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_BIG_INT:
        return JS_VALUE_GET_PTR(v);
    case JS_TAG_SHORT_BIG_INT:
        return js_bigint_set_short(buf, v);
    case JS_TAG_BOOL:
        return js_bigint_set_si(buf, JS_VALUE_GET_BOOL(v) != 0);
    default:
        return js_bigint_set_si(buf, JS_VALUE_GET_INT(v));
    }
}

/* A bigint helper returned NULL: keep uncatchable exceptions (interrupts),
   turn the JS RangeError "BigInt is too large" / out of memory into
   MemoryError. */
static JSValue qp_long_bigerr(JSContext *ctx)
{
    if (ctx->rt->current_exception_is_uncatchable)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return qp_raise_memory_error(ctx);
}

static JSValue qp_long_from_int64(JSContext *ctx, int64_t v)
{
    if (v == (int32_t)v)
        return JS_NewInt32(ctx, (int32_t)v);
#if JS_SHORT_BIG_INT_BITS == 64
    return __JS_NewShortBigInt(ctx, v);
#else
    {
        JSBigInt *r = js_bigint_new_si64(ctx, v);
        if (!r)
            return qp_long_bigerr(ctx);
        return JS_MKPTR(JS_TAG_BIG_INT, r);
    }
#endif
}

static JSValue qp_long_from_uint64(JSContext *ctx, uint64_t v)
{
    JSBigInt *r;
    if (v <= INT64_MAX)
        return qp_long_from_int64(ctx, (int64_t)v);
    r = js_bigint_new_ui64(ctx, v);
    if (!r)
        return qp_long_bigerr(ctx);
    return JS_MKPTR(JS_TAG_BIG_INT, r);
}

/* Canonical int from a fresh JSBigInt (consumed; NULL = failed op). */
static JSValue qp_long_from_big(JSContext *ctx, JSBigInt *r)
{
    int64_t v;
    if (!r)
        return qp_long_bigerr(ctx);
    if (r->len == 1) {
        v = (js_slimb_t)r->tab[0];
        js_free(ctx, r);
        return qp_long_from_int64(ctx, v);
    }
#if JS_LIMB_BITS == 32 && JS_SHORT_BIG_INT_BITS == 64
    if (r->len == 2) {
        v = (int64_t)(((uint64_t)r->tab[1] << 32) | r->tab[0]);
        js_free(ctx, r);
        return qp_long_from_int64(ctx, v);
    }
#endif
    return JS_MKPTR(JS_TAG_BIG_INT, r);
}

static JSValue qp_long_normalize(JSContext *ctx, JSValue v)
{
    int64_t x;
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_BOOL:
        return JS_NewInt32(ctx, JS_VALUE_GET_BOOL(v) != 0);
    case JS_TAG_SHORT_BIG_INT:
        x = JS_VALUE_GET_SHORT_BIG_INT(v);
        if (x == (int32_t)x)
            return JS_NewInt32(ctx, (int32_t)x);
        return v;
    case JS_TAG_BIG_INT:
        if (qp_li_small(v, &x)) {
            JS_FreeValue(ctx, v);
            return qp_long_from_int64(ctx, x);
        }
        return v;
    default:
        return v;
    }
}

/* New canonical copy of a plain int (bool -> int). */
static JSValue qp_li_copy(JSContext *ctx, JSValueConst v)
{
    return qp_long_normalize(ctx, JS_DupValue(ctx, v));
}

static BOOL qp_long_check(JSContext *ctx, JSValueConst v)
{
    return !QP_IS_NULL(qp_long_unwrap(v));
}

static BOOL qp_long_check_exact(JSValueConst v)
{
    return qp_long_tag_is_int(JS_VALUE_GET_TAG(v));
}

static JSValue qp_long_value(JSContext *ctx, JSValueConst v)
{
    JSValueConst u = qp_long_unwrap(v);
    if (QP_IS_NULL(u))
        return qp_raise_type_error(ctx, "an integer is required (got type %T)", v);
    return qp_li_copy(ctx, u);
}

/* tp_name of a type as C text (builtin spec name or __name__). */
static const char *qp_long_tp_name(JSContext *ctx, QPType *t, char *buf, int size)
{
    if (t->tp_name)
        return t->tp_name;
    return JS_AtomGetStr(ctx, buf, size, t->name);
}

/* ========================================================================= */
/* Plain-int primitives (arguments: plain ints, borrowed; results new and    */
/* canonical)                                                                 */
/* ========================================================================= */

static int qp_li_sign(JSValueConst v)
{
    int64_t x;
    JSBigInt *p;
    if (qp_li_small(v, &x))
        return (x > 0) - (x < 0);
    p = JS_VALUE_GET_PTR(v);
    if (js_bigint_sign(p))
        return -1;
    return !(p->len == 1 && p->tab[0] == 0);
}

static BOOL qp_li_is_odd(JSValueConst v)
{
    int64_t x;
    if (qp_li_small(v, &x))
        return x & 1;
    return ((JSBigInt *)JS_VALUE_GET_PTR(v))->tab[0] & 1;
}

/* Order of two normalized JSBigInts (two's complement). */
static int qp_big_cmp(const JSBigInt *a, const JSBigInt *b)
{
    int sa = js_bigint_sign(a), sb = js_bigint_sign(b), i;
    if (sa != sb)
        return sa ? -1 : 1;
    if (a->len != b->len)
        return ((a->len > b->len) ^ sa) ? 1 : -1;
    for (i = a->len - 1; i >= 0; i--) {
        if (a->tab[i] != b->tab[i])
            return a->tab[i] > b->tab[i] ? 1 : -1;
    }
    return 0;
}

static int qp_li_cmp(JSValueConst a, JSValueConst b)
{
    int64_t x, y;
    JSBigIntBuf ba, bb;
    if (qp_li_small(a, &x) && qp_li_small(b, &y))
        return (x > y) - (x < y);
    return qp_big_cmp(qp_li_big(a, &ba), qp_li_big(b, &bb));
}

/* Bit length of p ^ (sign mask), i.e. of p for p >= 0 and of ~p = |p| - 1
   for p < 0. */
static uint64_t qp_big_bitlen_xor(const JSBigInt *p)
{
    js_limb_t m = js_bigint_sign(p) ? (js_limb_t)-1 : 0;
    int i = p->len - 1;
    while (i >= 0 && (p->tab[i] ^ m) == 0)
        i--;
    if (i < 0)
        return 0;
    return (uint64_t)i * JS_LIMB_BITS + (JS_LIMB_BITS - js_limb_clz(p->tab[i] ^ m));
}

/* Number of bits of |v| (_PyLong_NumBits). */
static uint64_t qp_li_numbits(JSValueConst v)
{
    int64_t x;
    JSBigIntBuf buf;
    const JSBigInt *p;
    uint64_t b;
    int i, w;

    if (qp_li_small(v, &x)) {
        uint64_t m = x < 0 ? -(uint64_t)x : (uint64_t)x;
        return m ? 64 - clz64(m) : 0;
    }
    p = qp_li_big(v, &buf);
    b = qp_big_bitlen_xor(p);
    if (!js_bigint_sign(p))
        return b;
    /* |p| = ~p + 1 has one more bit iff ~p == 2**b - 1, i.e. the low b bits
       of p are all zero */
    if (b == 0)
        return 1;
    i = (int)((b - 1) / JS_LIMB_BITS);
    w = (int)(b - (uint64_t)i * JS_LIMB_BITS);
    {
        int j;
        js_limb_t mask = w == JS_LIMB_BITS ? (js_limb_t)-1 : (((js_limb_t)1 << w) - 1);
        if (p->tab[i] & mask)
            return b;
        for (j = 0; j < i; j++) {
            if (p->tab[j])
                return b;
        }
    }
    return b + 1;
}

static JSValue qp_li_add(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    JSBigIntBuf ba, bb;
    if (qp_li_small(a, &x) && qp_li_small(b, &y) && !__builtin_add_overflow(x, y, &r))
        return qp_long_from_int64(ctx, r);
    return qp_long_from_big(ctx, js_bigint_add(ctx, qp_li_big(a, &ba), qp_li_big(b, &bb), 0));
}

static JSValue qp_li_sub(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    JSBigIntBuf ba, bb;
    if (qp_li_small(a, &x) && qp_li_small(b, &y) && !__builtin_sub_overflow(x, y, &r))
        return qp_long_from_int64(ctx, r);
    return qp_long_from_big(ctx, js_bigint_add(ctx, qp_li_big(a, &ba), qp_li_big(b, &bb), 1));
}

static JSValue qp_li_mul(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y, r;
    JSBigIntBuf ba, bb;
    if (qp_li_small(a, &x) && qp_li_small(b, &y) && !__builtin_mul_overflow(x, y, &r))
        return qp_long_from_int64(ctx, r);
    return qp_long_from_big(ctx, js_bigint_mul(ctx, qp_li_big(a, &ba), qp_li_big(b, &bb)));
}

static JSValue qp_li_neg(JSContext *ctx, JSValueConst a)
{
    int64_t x;
    JSBigIntBuf ba;
    if (qp_li_small(a, &x) && x != INT64_MIN)
        return qp_long_from_int64(ctx, -x);
    return qp_long_from_big(ctx, js_bigint_neg(ctx, qp_li_big(a, &ba)));
}

static JSValue qp_li_abs(JSContext *ctx, JSValueConst a)
{
    if (qp_li_sign(a) < 0)
        return qp_li_neg(ctx, a);
    return qp_li_copy(ctx, a);
}

static JSValue qp_li_invert(JSContext *ctx, JSValueConst a)
{
    int64_t x;
    JSBigIntBuf ba;
    if (qp_li_small(a, &x))
        return qp_long_from_int64(ctx, ~x);
    return qp_long_from_big(ctx, js_bigint_not(ctx, qp_li_big(a, &ba)));
}

/* op = OP_and, OP_or or OP_xor */
static JSValue qp_li_logic(JSContext *ctx, JSValueConst a, JSValueConst b, OPCodeEnum op)
{
    int64_t x, y;
    JSBigIntBuf ba, bb;
    if (qp_li_small(a, &x) && qp_li_small(b, &y)) {
        switch (op) {
        case OP_and: return qp_long_from_int64(ctx, x & y);
        case OP_or:  return qp_long_from_int64(ctx, x | y);
        default:     return qp_long_from_int64(ctx, x ^ y);
        }
    }
    return qp_long_from_big(ctx, js_bigint_logic(ctx, qp_li_big(a, &ba), qp_li_big(b, &bb), op));
}

/* Floor division and modulo (l_divmod).  pq / pr may be NULL. */
static int qp_li_divmod(JSContext *ctx, JSValueConst a, JSValueConst b, JSValue *pq, JSValue *pr)
{
    int64_t x, y;
    JSBigIntBuf ba, bb;
    JSBigInt *A, *B;
    JSValue q = JS_UNDEFINED, r, t;

    if (qp_li_sign(b) == 0) {
        qp_raise_zero_division(ctx, "division by zero");
        return -1;
    }
    if (qp_li_small(a, &x) && qp_li_small(b, &y) && !(x == INT64_MIN && y == -1)) {
        int64_t qq = x / y, rr = x % y;
        if (rr != 0 && ((rr < 0) != (y < 0))) {
            rr += y;
            qq -= 1;
        }
        if (pq) {
            *pq = qp_long_from_int64(ctx, qq);
            if (JS_IsException(*pq))
                return -1;
        }
        if (pr) {
            *pr = qp_long_from_int64(ctx, rr);
            if (JS_IsException(*pr)) {
                if (pq)
                    JS_FreeValue(ctx, *pq);
                return -1;
            }
        }
        return 0;
    }
    A = qp_li_big(a, &ba);
    B = qp_li_big(b, &bb);
    r = qp_long_from_big(ctx, js_bigint_divrem(ctx, A, B, TRUE));
    if (JS_IsException(r))
        return -1;
    if (pq) {
        q = qp_long_from_big(ctx, js_bigint_divrem(ctx, A, B, FALSE));
        if (JS_IsException(q)) {
            JS_FreeValue(ctx, r);
            return -1;
        }
    }
    /* truncated -> floored */
    if (qp_li_sign(r) != 0 && (qp_li_sign(r) < 0) != (qp_li_sign(b) < 0)) {
        t = qp_li_add(ctx, r, b);
        JS_FreeValue(ctx, r);
        r = t;
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, q);
            return -1;
        }
        if (pq) {
            JSValue one = JS_NewInt32(ctx, 1);
            t = qp_li_sub(ctx, q, one);
            JS_FreeValue(ctx, q);
            q = t;
            if (JS_IsException(q)) {
                JS_FreeValue(ctx, r);
                return -1;
            }
        }
    }
    if (pq)
        *pq = q;
    if (pr)
        *pr = r;
    else
        JS_FreeValue(ctx, r);
    return 0;
}

static JSValue qp_li_mod(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue r;
    if (qp_li_divmod(ctx, a, b, NULL, &r) < 0)
        return JS_EXCEPTION;
    return r;
}

/* a << n, n >= 0 */
static JSValue qp_li_lshift(JSContext *ctx, JSValueConst a, uint64_t n)
{
    int64_t x;
    JSBigIntBuf ba;
    if (qp_li_sign(a) == 0)
        return JS_NewInt32(ctx, 0);
    if (qp_li_small(a, &x) && n < 63) {
        int64_t r = (int64_t)((uint64_t)x << n);
        if ((r >> n) == x)
            return qp_long_from_int64(ctx, r);
    }
    if (n > QP_LONG_MAX_BITS || qp_li_numbits(a) + n > QP_LONG_MAX_BITS)
        return qp_raise_memory_error(ctx);
    return qp_long_from_big(ctx, js_bigint_shl(ctx, qp_li_big(a, &ba), (unsigned int)n));
}

/* a >> n (floor), n >= 0 */
static JSValue qp_li_rshift(JSContext *ctx, JSValueConst a, uint64_t n)
{
    int64_t x;
    JSBigInt *p;
    if (qp_li_small(a, &x)) {
        if (n >= 64)
            return JS_NewInt32(ctx, x < 0 ? -1 : 0);
        return qp_long_from_int64(ctx, x >> n);
    }
    p = JS_VALUE_GET_PTR(a);
    if (n >= (uint64_t)p->len * JS_LIMB_BITS)
        return JS_NewInt32(ctx, js_bigint_sign(p) ? -1 : 0);
    return qp_long_from_big(ctx, js_bigint_shr(ctx, p, (unsigned int)n));
}

/* TRUE if any of the low n bits of the non-negative plain int a is set. */
static BOOL qp_li_low_bits_nonzero(JSValueConst a, uint64_t n)
{
    int64_t x;
    JSBigInt *p;
    uint64_t i, nl;
    if (qp_li_small(a, &x)) {
        if (n >= 64)
            return x != 0;
        return (x & (((int64_t)1 << n) - 1)) != 0;
    }
    p = JS_VALUE_GET_PTR(a);
    nl = n / JS_LIMB_BITS;
    for (i = 0; i < nl && i < p->len; i++) {
        if (p->tab[i])
            return TRUE;
    }
    if (nl < p->len && (n % JS_LIMB_BITS) != 0)
        return (p->tab[nl] & (((js_limb_t)1 << (n % JS_LIMB_BITS)) - 1)) != 0;
    return FALSE;
}

/* a ** e, e >= 0 */
static JSValue qp_li_pow(JSContext *ctx, JSValueConst a, uint64_t e)
{
    int64_t x;
    uint64_t nb;
    JSValue result, base, t;

    if (qp_li_small(a, &x)) {
        int64_t r = 1, bs = x;
        uint64_t k = e;
        if (x == 0)
            return JS_NewInt32(ctx, e == 0);
        if (x == 1)
            return JS_NewInt32(ctx, 1);
        if (x == -1)
            return JS_NewInt32(ctx, (e & 1) ? -1 : 1);
        for (;;) {
            if ((k & 1) && __builtin_mul_overflow(r, bs, &r))
                break;
            k >>= 1;
            if (!k)
                return qp_long_from_int64(ctx, r);
            if (__builtin_mul_overflow(bs, bs, &bs))
                break;
        }
    }
    nb = qp_li_numbits(a);
    if (nb > 1 && e > QP_LONG_MAX_BITS / (nb - 1))
        return qp_raise_memory_error(ctx);
    result = JS_NewInt32(ctx, 1);
    base = qp_li_copy(ctx, a);
    if (JS_IsException(base))
        return base;
    for (;;) {
        if (e & 1) {
            t = qp_li_mul(ctx, result, base);
            JS_FreeValue(ctx, result);
            result = t;
            if (JS_IsException(result))
                break;
        }
        e >>= 1;
        if (!e)
            break;
        t = qp_li_mul(ctx, base, base);
        JS_FreeValue(ctx, base);
        base = t;
        if (JS_IsException(base)) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, base);
    return result;
}

/* Bit i of the non-negative plain int v. */
static int qp_li_bit(JSValueConst v, uint64_t i)
{
    int64_t x;
    JSBigInt *p;
    if (qp_li_small(v, &x))
        return i >= 63 ? 0 : (int)((x >> i) & 1);
    p = JS_VALUE_GET_PTR(v);
    if (i / JS_LIMB_BITS >= p->len)
        return 0;
    return (int)((p->tab[i / JS_LIMB_BITS] >> (i % JS_LIMB_BITS)) & 1);
}

/* a ** b % c for 0 <= a < c, b >= 0, c > 1 (right-to-left binary). */
static JSValue qp_li_powmod(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c)
{
    uint64_t nb = qp_li_numbits(b), i;
    JSValue result = JS_NewInt32(ctx, 1), base, t;

    base = qp_li_copy(ctx, a);
    for (i = 0; i < nb; i++) {
        if (qp_li_bit(b, i)) {
            t = qp_li_mul(ctx, result, base);
            JS_FreeValue(ctx, result);
            if (JS_IsException(t))
                goto fail;
            result = qp_li_mod(ctx, t, c);
            JS_FreeValue(ctx, t);
            if (JS_IsException(result))
                goto fail;
        }
        if (i + 1 < nb) {
            t = qp_li_mul(ctx, base, base);
            JS_FreeValue(ctx, base);
            base = JS_UNDEFINED;
            if (JS_IsException(t))
                goto fail1;
            base = qp_li_mod(ctx, t, c);
            JS_FreeValue(ctx, t);
            if (JS_IsException(base))
                goto fail1;
        }
    }
    JS_FreeValue(ctx, base);
    return result;
 fail1:
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
 fail:
    JS_FreeValue(ctx, base);
    return JS_EXCEPTION;
}

/* Inverse of a modulo n > 0 (long_invmod: extended Euclid). */
static JSValue qp_li_invmod(JSContext *ctx, JSValueConst a0, JSValueConst n0)
{
    JSValue a = qp_li_copy(ctx, a0), n = qp_li_copy(ctx, n0);
    JSValue b = JS_NewInt32(ctx, 1), c = JS_NewInt32(ctx, 0);
    JSValue q, r, t, s;

    while (qp_li_sign(n) != 0) {
        if (qp_li_divmod(ctx, a, n, &q, &r) < 0)
            goto fail;
        JS_FreeValue(ctx, a);
        a = n;
        n = r;
        t = qp_li_mul(ctx, q, c);
        JS_FreeValue(ctx, q);
        if (JS_IsException(t))
            goto fail;
        s = qp_li_sub(ctx, b, t);
        JS_FreeValue(ctx, t);
        if (JS_IsException(s))
            goto fail;
        JS_FreeValue(ctx, b);
        b = c;
        c = s;
    }
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, n);
    if (!(JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_INT(a) == 1)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return qp_raise_value_error(ctx, "base is not invertible for the given modulus");
    }
    JS_FreeValue(ctx, a);
    return b;
 fail:
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, b);
    JS_FreeValue(ctx, c);
    return JS_EXCEPTION;
}

/* int / int, correctly rounded (port of long_true_divide). */
static JSValue qp_li_truediv(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t x, y;
    int sa = qp_li_sign(a), sb = qp_li_sign(b), negate, inexact = 0;
    int64_t a_bits, b_bits, diff, shift, x_bits, extra_bits;
    JSValue A, B, X, q, r;
    uint64_t qq, mask, low;
    double dx, result;

    if (sb == 0)
        return qp_raise_zero_division(ctx, "division by zero");
    negate = (sa < 0) != (sb < 0);
    if (sa == 0)
        return qp_float_new(ctx, negate ? -0.0 : 0.0);
    if (qp_li_small(a, &x) && qp_li_small(b, &y)) {
        uint64_t ax = x < 0 ? -(uint64_t)x : (uint64_t)x;
        uint64_t ay = y < 0 ? -(uint64_t)y : (uint64_t)y;
        if (ax < ((uint64_t)1 << QP_DBL_MANT_DIG) && ay < ((uint64_t)1 << QP_DBL_MANT_DIG)) {
            result = (double)ax / (double)ay;
            return qp_float_new(ctx, negate ? -result : result);
        }
    }
    a_bits = (int64_t)qp_li_numbits(a);
    b_bits = (int64_t)qp_li_numbits(b);
    diff = a_bits - b_bits;
    if (diff > QP_DBL_MAX_EXP)
        goto overflow;
    if (diff < QP_DBL_MIN_EXP - QP_DBL_MANT_DIG - 1)
        return qp_float_new(ctx, negate ? -0.0 : 0.0);
    shift = (diff > QP_DBL_MIN_EXP ? diff : QP_DBL_MIN_EXP) - QP_DBL_MANT_DIG - 2;

    A = qp_li_abs(ctx, a);
    if (JS_IsException(A))
        return A;
    if (shift <= 0) {
        X = qp_li_lshift(ctx, A, (uint64_t)-shift);
    } else {
        X = qp_li_rshift(ctx, A, (uint64_t)shift);
        inexact = qp_li_low_bits_nonzero(A, (uint64_t)shift);
    }
    JS_FreeValue(ctx, A);
    if (JS_IsException(X))
        return X;
    B = qp_li_abs(ctx, b);
    if (JS_IsException(B)) {
        JS_FreeValue(ctx, X);
        return B;
    }
    if (qp_li_divmod(ctx, X, B, &q, &r) < 0) {
        JS_FreeValue(ctx, X);
        JS_FreeValue(ctx, B);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, X);
    JS_FreeValue(ctx, B);
    if (qp_li_sign(r) != 0)
        inexact = 1;
    JS_FreeValue(ctx, r);
    /* q < 2**(DBL_MANT_DIG + 3) */
    x_bits = (int64_t)qp_li_numbits(q);
    if (!qp_li_small(q, &x)) {
        JS_FreeValue(ctx, q);
        return qp_raise_system_error(ctx, "bad internal state in int true division");
    }
    JS_FreeValue(ctx, q);
    qq = (uint64_t)x;
    extra_bits = (x_bits > QP_DBL_MIN_EXP - shift ? x_bits : QP_DBL_MIN_EXP - shift) - QP_DBL_MANT_DIG;
    mask = (uint64_t)1 << (extra_bits - 1);
    low = qq | (uint64_t)inexact;
    if ((low & mask) && (low & (3U * mask - 1U)))
        low += mask;
    qq = low & ~(2U * mask - 1U);
    dx = (double)qq;
    if (shift + x_bits >= QP_DBL_MAX_EXP &&
        (shift + x_bits > QP_DBL_MAX_EXP || dx == ldexp(1.0, (int)x_bits)))
        goto overflow;
    result = ldexp(dx, (int)shift);
    return qp_float_new(ctx, negate ? -result : result);
 overflow:
    return qp_raise_overflow_error(ctx, "integer division result too large for a float");
}

/* round(a / b) with ties to even, returning a - round(a/b) * b's
   remainder form: a - divmod_near(a, b)[1] (b > 0). */
static JSValue qp_li_round_to_multiple(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue q, r, twice, t, res;
    int cmp;

    if (qp_li_divmod(ctx, a, b, &q, &r) < 0)
        return JS_EXCEPTION;
    twice = qp_li_lshift(ctx, r, 1);
    if (JS_IsException(twice))
        goto fail;
    cmp = qp_li_cmp(twice, b);
    JS_FreeValue(ctx, twice);
    if (cmp > 0 || (cmp == 0 && qp_li_is_odd(q))) {
        t = qp_li_sub(ctx, r, b);
        JS_FreeValue(ctx, r);
        r = t;
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, q);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, q);
    res = qp_li_sub(ctx, a, r);
    JS_FreeValue(ctx, r);
    return res;
 fail:
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* Conversions                                                                */
/* ========================================================================= */

static JSValue qp_long_from_double(JSContext *ctx, double d)
{
    JSBigInt *r;
    int res;
    if (isnan(d))
        return qp_raise_value_error(ctx, "cannot convert float NaN to integer");
    if (isinf(d))
        return qp_raise_overflow_error(ctx, "cannot convert float infinity to integer");
    d = trunc(d);
    if (d > -9223372036854775808.0 && d < 9223372036854775808.0)
        return qp_long_from_int64(ctx, (int64_t)d);
    r = js_bigint_from_float64(ctx, &res, d);
    if (!r)
        return qp_long_bigerr(ctx);
    return qp_long_from_big(ctx, r);
}

static int qp_long_as_int64_overflow(JSValueConst v, int64_t *out, int *overflow)
{
    JSValueConst u = qp_long_unwrap(v);
    *overflow = 0;
    if (QP_IS_NULL(u)) {
        *out = -1;
        return -1;
    }
    if (qp_li_small(u, out))
        return 0;
    *overflow = qp_li_sign(u) < 0 ? -1 : 1;
    *out = -1;
    return 0;
}

static int qp_long_as_int64(JSContext *ctx, JSValueConst v, int64_t *out)
{
    int overflow;
    if (qp_long_as_int64_overflow(v, out, &overflow) < 0) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    if (overflow) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
        return -1;
    }
    return 0;
}

static int qp_long_as_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    int64_t x;
    int overflow;
    if (qp_long_as_int64_overflow(v, &x, &overflow) < 0) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    if (overflow || x < PY_SSIZE_T_MIN || x > PY_SSIZE_T_MAX) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C ssize_t");
        return -1;
    }
    *out = (Py_ssize_t)x;
    return 0;
}

static int qp_long_as_double(JSContext *ctx, JSValueConst v, double *out)
{
    JSValueConst u = qp_long_unwrap(v);
    int64_t x;
    double d;
    if (QP_IS_NULL(u)) {
        qp_raise_type_error(ctx, "an integer is required");
        return -1;
    }
    if (qp_li_small(u, &x)) {
        *out = (double)x;
        return 0;
    }
    d = js_bigint_to_float64(ctx, JS_VALUE_GET_PTR(u));
    if (isinf(d)) {
        qp_raise_overflow_error(ctx, "int too large to convert to float");
        return -1;
    }
    *out = d;
    return 0;
}

static int qp_long_sign(JSValueConst v)
{
    JSValueConst u = qp_long_unwrap(v);
    return QP_IS_NULL(u) ? 0 : qp_li_sign(u);
}

static int qp_long_compare(JSValueConst a, JSValueConst b)
{
    return qp_li_cmp(qp_long_unwrap(a), qp_long_unwrap(b));
}

static Py_hash_t qp_long_hash(JSValueConst v)
{
    JSValueConst u = qp_long_unwrap(v);
    int64_t x;
    JSBigInt *p;
    if (QP_IS_NULL(u))
        return -1;
    if (qp_li_small(u, &x))
        return qp_hash_int64(x);
    p = JS_VALUE_GET_PTR(u);
    return qp_hash_int_js_limbs(p->tab, p->len);
}

/* ---- int -> digits ------------------------------------------------------ */

/* Magnitude digits of a plain int in base 2..36: lowercase, most
   significant first, no leading zero ("0" for zero).  js_malloc'd and
   NUL-terminated (*plen digits); *pneg = v < 0.  NULL with an exception
   (memory, interrupt). */
static char *qp_long_mag_digits(JSContext *ctx, JSValueConst v, int base, size_t *plen, int *pneg)
{
    int64_t x;
    char *buf, *q;
    JSBigInt *a;
    js_limb_t *t;
    int n;
    uint64_t nbits;

    if (qp_li_small(v, &x)) {
        char tmp[66];
        uint64_t m = x < 0 ? -(uint64_t)x : (uint64_t)x;
        size_t len;
        q = tmp + sizeof(tmp);
        do {
            *--q = qp_long_digit_chars[m % (unsigned)base];
            m /= (unsigned)base;
        } while (m);
        len = tmp + sizeof(tmp) - q;
        buf = js_malloc(ctx, len + 1);
        if (!buf)
            return NULL;
        memcpy(buf, q, len);
        buf[len] = '\0';
        *plen = len;
        *pneg = x < 0;
        return buf;
    }
    a = JS_VALUE_GET_PTR(v);
    n = a->len;
    *pneg = js_bigint_sign(a);
    t = js_malloc(ctx, n * sizeof(js_limb_t));
    if (!t)
        return NULL;
    if (*pneg)
        mp_neg(t, a->tab, n);
    else
        memcpy(t, a->tab, n * sizeof(js_limb_t));
    while (n > 1 && t[n - 1] == 0)
        n--;
    nbits = (uint64_t)n * JS_LIMB_BITS - js_limb_safe_clz(t[n - 1]);
    if ((base & (base - 1)) == 0) {
        int lg = ctz32(base);
        size_t nd = (nbits + lg - 1) / lg, i;
        buf = js_malloc(ctx, nd + 1);
        if (!buf) {
            js_free(ctx, t);
            return NULL;
        }
        for (i = 0; i < nd; i++) {
            uint64_t bit_pos = (uint64_t)i * lg;
            size_t pos = bit_pos / JS_LIMB_BITS;
            int sh = bit_pos % JS_LIMB_BITS;
            js_limb_t c = t[pos] >> sh;
            if (sh + lg > JS_LIMB_BITS && pos + 1 < (size_t)n)
                c |= t[pos + 1] << (JS_LIMB_BITS - sh);
            buf[nd - 1 - i] = qp_long_digit_chars[c & (base - 1)];
        }
        buf[nd] = '\0';
        *plen = nd;
    } else {
        int lg = 31 - clz32(base), dpl = digits_per_limb_table[base - 2], k;
        js_limb_t rb = radix_base_table[base - 2], r;
        size_t nd_max = (nbits + lg - 1) / lg, len;
        buf = js_malloc(ctx, nd_max + 1);
        if (!buf) {
            js_free(ctx, t);
            return NULL;
        }
        q = buf + nd_max;
        *q = '\0';
        for (;;) {
            while (n > 1 && t[n - 1] == 0)
                n--;
            if (n == 1 && t[0] < rb) {
                r = t[0];
                while (r) {
                    *--q = qp_long_digit_chars[r % base];
                    r /= base;
                }
                break;
            }
            if (js_poll_interrupts_cost(ctx, js_bigint_poll_cost(16 * n))) {
                js_free(ctx, t);
                js_free(ctx, buf);
                return NULL;
            }
            r = mp_div1(t, t, n, rb, 0);
            for (k = 0; k < dpl; k++) {
                *--q = qp_long_digit_chars[r % base];
                r /= base;
            }
        }
        len = buf + nd_max - q;
        memmove(buf, q, len + 1);
        *plen = len;
    }
    js_free(ctx, t);
    return buf;
}

/* Digits of a plain int with the int_max_str_digits rules of
   long_to_decimal_string_internal applied for base 10. */
static char *qp_long_digits_checked(JSContext *ctx, JSValueConst u, int base, size_t *plen,
                                    int *pneg)
{
    int max_str_digits = QP_STATE(ctx)->int_max_str_digits;
    char *buf;

    if (base == 10 && max_str_digits > 0) {
        int64_t size_a = (int64_t)((qp_li_numbits(u) + QP_LONG_CPY_SHIFT - 1) / QP_LONG_CPY_SHIFT);
        if (size_a >= 10 * QP_LONG_MAX_STR_DIGITS_THRESHOLD / (3 * QP_LONG_CPY_SHIFT) + 2 &&
            max_str_digits / (3 * QP_LONG_CPY_SHIFT) <= (size_a - 11) / 10) {
            qp_raise_value_error(ctx, QP_LONG_MAX_STR_DIGITS_ERROR_TO_STR, max_str_digits);
            return NULL;
        }
    }
    buf = qp_long_mag_digits(ctx, u, base, plen, pneg);
    if (!buf) {
        if (!JS_HasException(ctx))
            qp_raise_memory_error(ctx);
        return NULL;
    }
    if (base == 10 && max_str_digits > 0 &&
        *plen + *pneg > QP_LONG_MAX_STR_DIGITS_THRESHOLD && *plen > (size_t)max_str_digits) {
        js_free(ctx, buf);
        qp_raise_value_error(ctx, QP_LONG_MAX_STR_DIGITS_ERROR_TO_STR, max_str_digits);
        return NULL;
    }
    return buf;
}

static JSValue qp_long_to_str(JSContext *ctx, JSValueConst v, int base, int alternate)
{
    JSValueConst u = qp_long_unwrap(v);
    char *digs, *out;
    size_t n, len;
    int neg;
    JSValue res;
    const char *prefix = "";

    if (QP_IS_NULL(u))
        return qp_raise_type_error(ctx, "an integer is required (got type %T)", v);
    if (base == 10 && JS_VALUE_GET_TAG(u) == JS_TAG_INT) {
        char tmp[16];
        int l = snprintf(tmp, sizeof(tmp), "%d", JS_VALUE_GET_INT(u));
        return js_new_string8_len(ctx, tmp, l);
    }
    digs = qp_long_digits_checked(ctx, u, base, &n, &neg);
    if (!digs)
        return JS_EXCEPTION;
    if (alternate) {
        if (base == 16)
            prefix = "0x";
        else if (base == 8)
            prefix = "0o";
        else if (base == 2)
            prefix = "0b";
    }
    len = n + neg + strlen(prefix);
    if (len > JS_STRING_LEN_MAX) {
        js_free(ctx, digs);
        return qp_raise_memory_error(ctx);
    }
    out = js_malloc(ctx, len + 1);
    if (!out) {
        js_free(ctx, digs);
        return JS_EXCEPTION;
    }
    snprintf(out, len + 1, "%s%s%s", neg ? "-" : "", prefix, digs);
    js_free(ctx, digs);
    res = js_new_string8_len(ctx, out, (int)len);
    js_free(ctx, out);
    return res;
}

/* ---- text -> int -------------------------------------------------------- */

static inline int qp_long_digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 37;
}

static inline BOOL qp_long_isspace(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* Non-negative int from n digit values (most significant first). */
static JSValue qp_long_from_digit_values(JSContext *ctx, const uint8_t *dv, size_t n, int base)
{
    uint64_t acc = 0;
    size_t i = 0, nl, used = 0;
    js_limb_t *t, mult_max = 1;
    int k_max = 0, bpd;
    JSBigInt *r;

    /* fast path: fits uint64 */
    for (i = 0; i < n; i++) {
        uint64_t nacc;
        if (__builtin_mul_overflow(acc, (uint64_t)base, &nacc) ||
            __builtin_add_overflow(nacc, (uint64_t)dv[i], &nacc))
            break;
        acc = nacc;
    }
    if (i == n)
        return qp_long_from_uint64(ctx, acc);

    bpd = 32 - clz32(base - 1);              /* ceil(log2(base)) */
    if ((uint64_t)(n - 1) * (31 - clz32(base)) > QP_LONG_MAX_BITS)
        return qp_raise_memory_error(ctx);
    nl = (n * (uint64_t)bpd) / JS_LIMB_BITS + 2;
    t = js_mallocz(ctx, nl * sizeof(js_limb_t));
    if (!t)
        return JS_EXCEPTION;
    while (mult_max <= (js_limb_t)-1 / (js_limb_t)base) {
        mult_max *= base;
        k_max++;
    }
    i = 0;
    while (i < n) {
        size_t m = n - i < (size_t)k_max ? n - i : (size_t)k_max, j;
        js_limb_t chunk = 0, mult = 1, carry;
        for (j = 0; j < m; j++) {
            chunk = chunk * base + dv[i + j];
            mult *= base;
        }
        i += m;
        carry = mp_mul1(t, t, used, mult, chunk);
        if (carry)
            t[used++] = carry;
        if (used > 64 && js_poll_interrupts_cost(ctx, js_bigint_poll_cost(used))) {
            js_free(ctx, t);
            return JS_EXCEPTION;
        }
    }
    if (used == 0) {
        js_free(ctx, t);
        return JS_NewInt32(ctx, 0);
    }
    if (t[used - 1] >> (JS_LIMB_BITS - 1))
        t[used++] = 0;  /* room for the sign bit */
    r = js_bigint_new(ctx, used);
    if (!r) {
        js_free(ctx, t);
        return qp_long_bigerr(ctx);
    }
    memcpy(r->tab, t, used * sizeof(js_limb_t));
    js_free(ctx, t);
    return qp_long_from_big(ctx, js_bigint_normalize(ctx, r));
}

/* PyLong_FromString over s[0..len) (a NUL byte ends the text, as in C):
   returns the int, QP_NULL for a syntax error (no exception: the caller
   raises "invalid literal ..." with its own repr), or JS_EXCEPTION
   (int_max_str_digits, memory). */
static JSValue qp_long_parse_ascii(JSContext *ctx, const char *s, size_t len, int base)
{
    const char *p = s, *end = s + len, *start;
    int neg = 0, error_if_nonzero = 0;
    char prev = 0, c0, c1;
    size_t digits = 0, i;
    uint8_t *dv;
    JSValue v, t;

    while (p < end && qp_long_isspace(*p))
        p++;
    if (p < end && *p == '+') {
        p++;
    } else if (p < end && *p == '-') {
        neg = 1;
        p++;
    }
    c0 = p < end ? p[0] : 0;
    c1 = p + 1 < end ? p[1] : 0;
    if (base == 0) {
        if (c0 != '0')
            base = 10;
        else if (c1 == 'x' || c1 == 'X')
            base = 16;
        else if (c1 == 'o' || c1 == 'O')
            base = 8;
        else if (c1 == 'b' || c1 == 'B')
            base = 2;
        else {
            /* "old" (C-style) octal literal, now invalid; it might still be zero */
            error_if_nonzero = 1;
            base = 10;
        }
    }
    if (c0 == '0' &&
        ((base == 16 && (c1 == 'x' || c1 == 'X')) ||
         (base == 8 && (c1 == 'o' || c1 == 'O')) ||
         (base == 2 && (c1 == 'b' || c1 == 'B')))) {
        p += 2;
        /* one underscore allowed here */
        if (p < end && *p == '_')
            p++;
    }
    start = p;
    if (p < end && *p == '_')
        return QP_NULL;
    while (p < end && (qp_long_digit_value(*p) < base || *p == '_')) {
        if (*p == '_') {
            if (prev == '_')
                return QP_NULL;
        } else {
            digits++;
        }
        prev = *p;
        p++;
    }
    if (prev == '_' || p == start)
        return QP_NULL;
    {
        const char *dend = p;
        while (p < end && qp_long_isspace(*p))
            p++;
        if (p != end)
            return QP_NULL;
        end = dend;
    }
    if ((base & (base - 1)) != 0 && digits > QP_LONG_MAX_STR_DIGITS_THRESHOLD) {
        int max_str_digits = QP_STATE(ctx)->int_max_str_digits;
        if (max_str_digits > 0 && digits > (size_t)max_str_digits)
            return qp_raise_value_error(ctx, QP_LONG_MAX_STR_DIGITS_ERROR_TO_INT,
                                        max_str_digits, (Py_ssize_t)digits);
    }
    dv = js_malloc(ctx, digits ? digits : 1);
    if (!dv)
        return JS_EXCEPTION;
    for (i = 0, p = start; p < end; p++) {
        if (*p != '_')
            dv[i++] = (uint8_t)qp_long_digit_value(*p);
    }
    v = qp_long_from_digit_values(ctx, dv, digits, base);
    js_free(ctx, dv);
    if (JS_IsException(v))
        return v;
    if (error_if_nonzero && qp_li_sign(v) != 0) {
        JS_FreeValue(ctx, v);
        return QP_NULL;
    }
    if (neg) {
        t = qp_li_neg(ctx, v);
        JS_FreeValue(ctx, v);
        v = t;
    }
    return v;
}

/* _PyLong_FromBytes: int from bytes-like text, invalid literal reported
   as a bytes repr. */
static JSValue qp_long_from_bytes_text(JSContext *ctx, const uint8_t *s, size_t len, int base)
{
    JSValue v = qp_long_parse_ascii(ctx, (const char *)s, len, base), b;
    if (!QP_IS_NULL(v))
        return v;
    b = qp_bytes_new(ctx, s, len < 200 ? (Py_ssize_t)len : 200);
    if (JS_IsException(b))
        return b;
    qp_raise_value_error(ctx, "invalid literal for int() with base %d: %.200R", base, b);
    JS_FreeValue(ctx, b);
    return JS_EXCEPTION;
}

/* PyLong_FromUnicodeObject (with _PyUnicode_TransformDecimalAndSpaceToASCII). */
static JSValue qp_long_from_str(JSContext *ctx, JSValueConst s, int base)
{
    JSValue plain, v;
    qp_strview view;
    char *buf;
    Py_ssize_t i, n = 0;

    if ((base != 0 && base < 2) || base > 36)
        return qp_raise_value_error(ctx, "int() arg 2 must be >= 2 and <= 36");
    plain = qp_str_value(ctx, s);
    if (JS_IsException(plain))
        return plain;
    if (qp_str_view_acquire(ctx, plain, &view) < 0) {
        JS_FreeValue(ctx, plain);
        return JS_EXCEPTION;
    }
    buf = js_malloc(ctx, view.len + 1);
    if (!buf) {
        qp_str_view_release(ctx, &view);
        JS_FreeValue(ctx, plain);
        return JS_EXCEPTION;
    }
    for (i = 0; i < view.len; i++) {
        Py_UCS4 ch = QP_SL_READ(view.kind, view.data, i);
        if (ch < 127) {
            buf[n++] = (char)ch;
        } else if (_PyUnicode_IsWhitespace(ch)) {
            buf[n++] = ' ';
        } else {
            int d = _PyUnicode_ToDecimalDigit(ch);
            if (d < 0) {
                buf[n++] = '?';
                break;
            }
            buf[n++] = (char)('0' + d);
        }
    }
    buf[n] = '\0';
    qp_str_view_release(ctx, &view);
    JS_FreeValue(ctx, plain);
    v = qp_long_parse_ascii(ctx, buf, n, base);
    js_free(ctx, buf);
    if (!QP_IS_NULL(v))
        return v;
    return qp_raise_value_error(ctx, "invalid literal for int() with base %d: %.200R", base, s);
}

/* ---- bytes <-> int ------------------------------------------------------ */

/* _PyLong_FromByteArray */
static JSValue qp_long_from_byte_array(JSContext *ctx, const uint8_t *data, size_t n,
                                       int little_endian, int is_signed)
{
    const size_t lb = sizeof(js_limb_t);
    size_t nl, i;
    JSBigInt *r;
    int neg;

    if (n == 0)
        return JS_NewInt32(ctx, 0);
    if (n <= 7) {
        uint64_t acc = 0;
        for (i = 0; i < n; i++)
            acc = (acc << 8) | data[little_endian ? n - 1 - i : i];
        if (is_signed && (acc >> (8 * n - 1)) & 1)
            return qp_long_from_int64(ctx, (int64_t)(acc - ((uint64_t)1 << (8 * n))));
        return qp_long_from_int64(ctx, (int64_t)acc);
    }
    nl = (n + lb - 1) / lb + 1;
    if (nl > JS_BIGINT_MAX_SIZE)
        return qp_raise_memory_error(ctx);
    r = js_bigint_new(ctx, (int)nl);
    if (!r)
        return qp_long_bigerr(ctx);
    memset(r->tab, 0, nl * lb);
    for (i = 0; i < n; i++) {
        uint8_t byte = data[little_endian ? i : n - 1 - i];
        r->tab[i / lb] |= (js_limb_t)byte << (8 * (i % lb));
    }
    neg = is_signed && (data[little_endian ? n - 1 : 0] & 0x80);
    if (neg) {
        for (i = n; i < nl * lb; i++)
            r->tab[i / lb] |= (js_limb_t)0xff << (8 * (i % lb));
    }
    return qp_long_from_big(ctx, js_bigint_normalize(ctx, r));
}

/* _PyLong_AsByteArray with CPython's OverflowError messages. */
static JSValue qp_long_to_byte_array(JSContext *ctx, JSValueConst u, Py_ssize_t length,
                                     int little_endian, int is_signed)
{
    JSBigIntBuf buf;
    JSBigInt *p = qp_li_big(u, &buf);
    const size_t lb = sizeof(js_limb_t);
    int neg = js_bigint_sign(p);
    uint64_t need;
    uint8_t *out;
    Py_ssize_t i;
    JSValue res;

    if (neg && !is_signed)
        return qp_raise_overflow_error(ctx, "can't convert negative int to unsigned");
    /* bits needed: magnitude bits (unsigned) or bits of p ^ sign + 1 (signed) */
    need = qp_big_bitlen_xor(p);
    if (is_signed && (need != 0 || neg))
        need++;
    if (need > (uint64_t)length * 8)
        return qp_raise_overflow_error(ctx, "int too big to convert");
    out = js_malloc(ctx, length ? length : 1);
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < length; i++) {
        uint8_t byte;
        if ((size_t)i / lb < p->len)
            byte = (uint8_t)(p->tab[i / lb] >> (8 * (i % lb)));
        else
            byte = neg ? 0xff : 0;
        out[little_endian ? i : length - 1 - i] = byte;
    }
    res = qp_bytes_new(ctx, out, length);
    js_free(ctx, out);
    return res;
}

/* PyBytes_FromObject for int.from_bytes (buffer, iterable of ints;
   "cannot convert '%.200s' object to bytes" otherwise).  On success
   *pbytes is a new bytes object, or the buffer is in *view (*pbytes =
   QP_NULL). */
static int qp_long_bytes_arg(JSContext *ctx, JSValueConst obj, JSValue *pbytes, QPBuffer *view)
{
    QPType *t = qp_type_of(ctx, obj);
    JSValue meth, it, item, res;
    uint8_t *data = NULL;
    size_t len = 0, size = 0;
    char namebuf[ATOM_GET_STR_BUF_SIZE];

    *pbytes = QP_NULL;
    if (qp_bytes_check(ctx, obj) || qp_bytearray_check(ctx, obj)) {
        /* read through qp_bytes_data before any Python code runs */
        *pbytes = JS_DupValue(ctx, obj);
        return 0;
    }
    if (t->slots.bf.bf_getbuffer)
        return qp_buffer_get(ctx, obj, view, QP_BUF_SIMPLE);
    /* PyObject_Bytes: __bytes__ first */
    meth = qp_lookup_special(ctx, obj, QP_ATOM(ctx, __bytes__));
    if (JS_IsException(meth))
        return -1;
    if (!QP_IS_NULL(meth)) {
        res = qp_call0(ctx, meth);
        JS_FreeValue(ctx, meth);
        if (JS_IsException(res))
            return -1;
        if (!qp_bytes_check(ctx, res)) {
            qp_raise_type_error(ctx, "__bytes__ returned non-bytes (type %s)",
                                qp_long_tp_name(ctx, qp_type_of(ctx, res), namebuf, sizeof(namebuf)));
            JS_FreeValue(ctx, res);
            return -1;
        }
        *pbytes = res;
        return 0;
    }
    if (!qp_str_check(ctx, obj)) {
        it = qp_iter(ctx, obj);
        if (!JS_IsException(it)) {
            for (;;) {
                Py_ssize_t value;
                item = qp_next(ctx, it);
                if (JS_IsException(item))
                    goto fail;
                if (QP_IS_NULL(item))
                    break;
                if (qp_as_ssize(ctx, item, NULL, &value) < 0) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                JS_FreeValue(ctx, item);
                if (value < 0 || value >= 256) {
                    qp_raise_value_error(ctx, "bytes must be in range(0, 256)");
                    goto fail;
                }
                if (len == size) {
                    size_t nsize = size ? size * 2 : 16;
                    uint8_t *nd = js_realloc(ctx, data, nsize);
                    if (!nd)
                        goto fail;
                    data = nd;
                    size = nsize;
                }
                data[len++] = (uint8_t)value;
            }
            JS_FreeValue(ctx, it);
            *pbytes = qp_bytes_new(ctx, data, (Py_ssize_t)len);
            js_free(ctx, data);
            return JS_IsException(*pbytes) ? -1 : 0;
        fail:
            JS_FreeValue(ctx, it);
            js_free(ctx, data);
            return -1;
        }
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError)))
            return -1;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    qp_raise_type_error(ctx, "cannot convert '%.200s' object to bytes",
                        qp_long_tp_name(ctx, t, namebuf, sizeof(namebuf)));
    return -1;
}

/* ========================================================================= */
/* int(x) / int(x, base)                                                      */
/* ========================================================================= */

static JSValue qp_number_long(JSContext *ctx, JSValueConst v)
{
    QPType *t;
    JSValue res;
    char namebuf[ATOM_GET_STR_BUF_SIZE];

    if (qp_long_check_exact(v))
        return qp_li_copy(ctx, v);
    t = qp_type_of(ctx, v);
    if (t->slots.nb.nb_int) {
        res = t->slots.nb.nb_int(ctx, v);
        if (JS_IsException(res) || qp_long_check_exact(res))
            return res;
        if (!qp_long_check(ctx, res)) {
            qp_raise_type_error(ctx, "%T.__int__() must return an int, not %T", v, res);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        /* Issue #17576: warn if 'result' not of exact type int. */
        if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                    "%T.__int__() must return an int, not %T.  "
                    "The ability to return an instance of a strict subclass of int "
                    "is deprecated, and may be removed in a future version of Python.",
                    v, res) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        {
            JSValue plain = qp_long_value(ctx, res);
            JS_FreeValue(ctx, res);
            return plain;
        }
    }
    if (t->slots.nb.nb_index)
        return qp_index(ctx, v);
    if (qp_str_check(ctx, v))
        return qp_long_from_str(ctx, v, 10);
    if (qp_bytes_check(ctx, v) || qp_bytearray_check(ctx, v)) {
        Py_ssize_t n;
        const uint8_t *d = qp_bytes_data(v, &n);
        return qp_long_from_bytes_text(ctx, d, n, 10);
    }
    if (t->slots.bf.bf_getbuffer) {
        QPBuffer view;
        if (qp_buffer_get(ctx, v, &view, QP_BUF_SIMPLE) < 0)
            return JS_EXCEPTION;
        res = qp_long_from_bytes_text(ctx, view.buf, view.len, 10);
        qp_buffer_release(ctx, &view);
        return res;
    }
    return qp_raise_type_error(ctx, "int() argument must be a string, a bytes-like object "
                               "or a real number, not '%.200s'",
                               qp_long_tp_name(ctx, t, namebuf, sizeof(namebuf)));
}

/* long_new_impl for exact int (x / obase QP_NULL when absent). */
static JSValue qp_long_new_impl(JSContext *ctx, JSValueConst x, JSValueConst obase)
{
    Py_ssize_t base;

    if (QP_IS_NULL(x)) {
        if (!QP_IS_NULL(obase))
            return qp_raise_type_error(ctx, "int() missing string argument");
        return JS_NewInt32(ctx, 0);
    }
    if (QP_IS_NULL(obase))
        return qp_number_long(ctx, x);
    if (qp_as_ssize(ctx, obase, NULL, &base) < 0)
        return JS_EXCEPTION;
    if ((base != 0 && base < 2) || base > 36)
        return qp_raise_value_error(ctx, "int() base must be >= 2 and <= 36, or 0");
    if (qp_str_check(ctx, x))
        return qp_long_from_str(ctx, x, (int)base);
    if (qp_bytes_check(ctx, x) || qp_bytearray_check(ctx, x)) {
        Py_ssize_t n;
        const uint8_t *d = qp_bytes_data(x, &n);
        return qp_long_from_bytes_text(ctx, d, n, (int)base);
    }
    return qp_raise_type_error(ctx, "int() can't convert non-string with explicit base");
}

/* Instance of an int subclass holding the plain int v (consumed). */
static JSValue qp_long_subtype_wrap(JSContext *ctx, QPType *type, JSValue v)
{
    JSValue obj;
    if (JS_IsException(v))
        return v;
    obj = qp_object_new_instance(ctx, type, JS_CLASS_PY_BOXED);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, v);
        return obj;
    }
    JS_VALUE_GET_OBJ(obj)->u.object_data = v;
    return obj;
}

/* int.__new__(type, x=0, /, base=10) */
static JSValue qp_long_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    static const char *const keywords[] = { "", "base", NULL };
    static const QPArgSpec spec = { "int", keywords, 0, 2, 0 };
    JSValueConst out[2];
    JSValue v;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, out) < 0)
        return JS_EXCEPTION;
    v = qp_long_new_impl(ctx, out[0], out[1]);
    if (subtype == QP_TYPE(ctx, int))
        return v;
    return qp_long_subtype_wrap(ctx, subtype, v);
}

static BOOL qp_long_has_kwnames(JSValueConst kwnames)
{
    return JS_VALUE_GET_TAG(kwnames) == JS_TAG_OBJECT && QP_TUPLE_SIZE(kwnames) > 0;
}

/* long_vectorcall: int(...) */
static JSValue qp_long_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                  size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    QPType *t = qp_type_from_value(callable);

    if (!t)
        t = QP_TYPE(ctx, int);
    if (qp_long_has_kwnames(kwnames) || t != QP_TYPE(ctx, int))
        return qp_long_tp_new(ctx, t, args, nargsf, kwnames);
    switch (nargs) {
    case 0:
        return JS_NewInt32(ctx, 0);
    case 1:
        return qp_number_long(ctx, args[0]);
    case 2:
        return qp_long_new_impl(ctx, args[0], args[1]);
    default:
        return qp_raise_type_error(ctx, "int expected at most 2 arguments, got %zd", nargs);
    }
}

/* ========================================================================= */
/* int slots                                                                  */
/* ========================================================================= */

#define QP_LONG_BINOP(ctx, a, b, ua, ub)                       \
    JSValueConst ua = qp_long_unwrap(a), ub = qp_long_unwrap(b); \
    if (QP_IS_NULL(ua) || QP_IS_NULL(ub))                      \
        return qp_not_implemented(ctx)

static JSValue qp_long_add_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_add(ctx, ua, ub);
}

static JSValue qp_long_sub_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_sub(ctx, ua, ub);
}

static JSValue qp_long_mul_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_mul(ctx, ua, ub);
}

static JSValue qp_long_floordiv_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue q;
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    if (qp_li_divmod(ctx, ua, ub, &q, NULL) < 0)
        return JS_EXCEPTION;
    return q;
}

static JSValue qp_long_mod_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_mod(ctx, ua, ub);
}

static JSValue qp_long_divmod_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue qr[2];
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    if (qp_li_divmod(ctx, ua, ub, &qr[0], &qr[1]) < 0)
        return JS_EXCEPTION;
    return qp_tuple_new_steal(ctx, 2, qr);
}

static JSValue qp_long_truediv_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_truediv(ctx, ua, ub);
}

/* long_pow: a ** b [% c]; x = None for the 2-argument form. */
static JSValue qp_long_pow_slot(JSContext *ctx, JSValueConst v, JSValueConst w, JSValueConst x)
{
    JSValueConst c = QP_NULL;
    JSValue C, A, B, t, z;
    int neg_out;
    int64_t e;
    QP_LONG_BINOP(ctx, v, w, a, b);

    if (!JS_IsUndefined(x) && !JS_IsNull(x)) {
        c = qp_long_unwrap(x);
        if (QP_IS_NULL(c))
            return qp_not_implemented(ctx);
    }
    if (QP_IS_NULL(c)) {
        if (qp_li_sign(b) < 0) {
            /* negative exponent without modulus: float result */
            QPType *ft = QP_TYPE(ctx, float);
            return ft->slots.nb.nb_power(ctx, v, w, x);
        }
        if (!qp_li_small(b, &e)) {
            int64_t av;
            if (qp_li_small(a, &av) && av >= -1 && av <= 1)
                return JS_NewInt32(ctx, av == -1 ? (qp_li_is_odd(b) ? -1 : 1) : (int32_t)av);
            return qp_raise_memory_error(ctx);
        }
        return qp_li_pow(ctx, a, (uint64_t)e);
    }

    if (qp_li_sign(c) == 0)
        return qp_raise_value_error(ctx, "pow() 3rd argument cannot be 0");
    neg_out = qp_li_sign(c) < 0;
    C = qp_li_abs(ctx, c);
    if (JS_IsException(C))
        return C;
    if (JS_VALUE_GET_TAG(C) == JS_TAG_INT && JS_VALUE_GET_INT(C) == 1) {
        JS_FreeValue(ctx, C);
        return JS_NewInt32(ctx, 0);
    }
    A = qp_li_copy(ctx, a);
    B = qp_li_copy(ctx, b);
    if (qp_li_sign(B) < 0) {
        t = qp_li_neg(ctx, B);
        JS_FreeValue(ctx, B);
        B = t;
        if (JS_IsException(B))
            goto fail;
        t = qp_li_invmod(ctx, A, C);
        JS_FreeValue(ctx, A);
        A = t;
        if (JS_IsException(A))
            goto fail;
    }
    t = qp_li_mod(ctx, A, C);
    JS_FreeValue(ctx, A);
    A = t;
    if (JS_IsException(A))
        goto fail;
    z = qp_li_powmod(ctx, A, B, C);
    if (!JS_IsException(z) && neg_out && qp_li_sign(z) != 0) {
        t = qp_li_sub(ctx, z, C);
        JS_FreeValue(ctx, z);
        z = t;
    }
    JS_FreeValue(ctx, A);
    JS_FreeValue(ctx, B);
    JS_FreeValue(ctx, C);
    return z;
 fail:
    JS_FreeValue(ctx, A);
    JS_FreeValue(ctx, B);
    JS_FreeValue(ctx, C);
    return JS_EXCEPTION;
}

static JSValue qp_long_neg_slot(JSContext *ctx, JSValueConst a)
{
    JSValueConst u = qp_long_unwrap(a);
    if (QP_IS_NULL(u))
        return qp_not_implemented(ctx);
    return qp_li_neg(ctx, u);
}

/* long_long: int(self) for nb_int, nb_index, nb_positive, real, ... */
static JSValue qp_long_long_slot(JSContext *ctx, JSValueConst a)
{
    return qp_long_value(ctx, a);
}

static JSValue qp_long_abs_slot(JSContext *ctx, JSValueConst a)
{
    JSValueConst u = qp_long_unwrap(a);
    if (QP_IS_NULL(u))
        return qp_not_implemented(ctx);
    return qp_li_abs(ctx, u);
}

static int qp_long_bool_slot(JSContext *ctx, JSValueConst a)
{
    return qp_long_sign(a) != 0;
}

static JSValue qp_long_invert_slot(JSContext *ctx, JSValueConst a)
{
    JSValueConst u = qp_long_unwrap(a);
    if (QP_IS_NULL(u))
        return qp_not_implemented(ctx);
    return qp_li_invert(ctx, u);
}

static JSValue qp_long_lshift_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t n;
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    if (qp_li_sign(ub) < 0)
        return qp_raise_value_error(ctx, "negative shift count");
    if (qp_li_sign(ua) == 0)
        return JS_NewInt32(ctx, 0);
    if (!qp_li_small(ub, &n))
        return qp_raise_overflow_error(ctx, "too many digits in integer");
    return qp_li_lshift(ctx, ua, (uint64_t)n);
}

static JSValue qp_long_rshift_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int64_t n;
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    if (qp_li_sign(ub) < 0)
        return qp_raise_value_error(ctx, "negative shift count");
    if (qp_li_sign(ua) == 0)
        return JS_NewInt32(ctx, 0);
    if (!qp_li_small(ub, &n))
        return JS_NewInt32(ctx, qp_li_sign(ua) < 0 ? -1 : 0);
    return qp_li_rshift(ctx, ua, (uint64_t)n);
}

static JSValue qp_long_and_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_logic(ctx, ua, ub, OP_and);
}

static JSValue qp_long_or_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_logic(ctx, ua, ub, OP_or);
}

static JSValue qp_long_xor_slot(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    return qp_li_logic(ctx, ua, ub, OP_xor);
}

static JSValue qp_long_float_slot(JSContext *ctx, JSValueConst a)
{
    double d;
    if (qp_long_as_double(ctx, a, &d) < 0)
        return JS_EXCEPTION;
    return qp_float_new(ctx, d);
}

static JSValue qp_long_repr_slot(JSContext *ctx, JSValueConst a)
{
    return qp_long_to_str(ctx, a, 10, 0);
}

static Py_hash_t qp_long_hash_slot(JSContext *ctx, JSValueConst a)
{
    return qp_long_hash(a);
}

static JSValue qp_long_richcompare_slot(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    int c, r;
    QP_LONG_BINOP(ctx, a, b, ua, ub);
    c = qp_li_cmp(ua, ub);
    switch (op & QP_CMP_OP_MASK) {
    case QP_CMP_LT: r = c < 0; break;
    case QP_CMP_LE: r = c <= 0; break;
    case QP_CMP_EQ: r = c == 0; break;
    case QP_CMP_NE: r = c != 0; break;
    case QP_CMP_GT: r = c > 0; break;
    default:        r = c >= 0; break;
    }
    return qp_bool_new(r);
}

/* ========================================================================= */
/* int methods and getters                                                    */
/* ========================================================================= */

static JSValue qp_long_meth_long(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_value(ctx, self);
}

static JSValue qp_long_meth_bit_length(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return qp_long_from_uint64(ctx, qp_li_numbits(qp_long_unwrap(self)));
}

static JSValue qp_long_meth_bit_count(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst u = qp_long_unwrap(self);
    int64_t x;
    uint64_t count = 0;
    JSValue m;
    JSBigInt *p;
    uint32_t i;

    if (qp_li_small(u, &x)) {
        uint64_t mg = x < 0 ? -(uint64_t)x : (uint64_t)x;
        return JS_NewInt32(ctx, __builtin_popcountll(mg));
    }
    m = qp_li_abs(ctx, u);
    if (JS_IsException(m))
        return m;
    p = JS_VALUE_GET_PTR(m);
    for (i = 0; i < p->len; i++)
        count += __builtin_popcountll((unsigned long long)p->tab[i]);
    JS_FreeValue(ctx, m);
    return qp_long_from_uint64(ctx, count);
}

static JSValue qp_long_meth_as_integer_ratio(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue items[2];
    items[0] = qp_long_value(ctx, self);
    if (JS_IsException(items[0]))
        return items[0];
    items[1] = JS_NewInt32(ctx, 1);
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_long_meth_getnewargs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    JSValue v = qp_long_value(ctx, self);
    if (JS_IsException(v))
        return v;
    return qp_tuple_new_steal(ctx, 1, &v);
}

static JSValue qp_long_meth_is_integer(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    return JS_TRUE;
}

static JSValue qp_long_meth_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    uint64_t nd = (qp_li_numbits(qp_long_unwrap(self)) + QP_LONG_CPY_SHIFT - 1) / QP_LONG_CPY_SHIFT;
    if (nd < 1)
        nd = 1;
    /* offsetof(PyLongObject, long_value.ob_digit) + sizeof(digit) * ndigits */
    return qp_long_from_uint64(ctx, 24 + 4 * nd);
}

/* int.__round__(ndigits=None, /) */
static JSValue qp_long_meth_round(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue nd, neg_nd, p, res, val;
    int64_t e;

    if (qp_arg_check_positional(ctx, "__round__", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0 || JS_IsUndefined(args[0]) || JS_IsNull(args[0]))
        return qp_long_value(ctx, self);
    nd = qp_index(ctx, args[0]);
    if (JS_IsException(nd))
        return nd;
    if (qp_li_sign(nd) >= 0) {
        JS_FreeValue(ctx, nd);
        return qp_long_value(ctx, self);
    }
    neg_nd = qp_li_neg(ctx, nd);
    JS_FreeValue(ctx, nd);
    if (JS_IsException(neg_nd))
        return neg_nd;
    if (!qp_li_small(neg_nd, &e)) {
        JS_FreeValue(ctx, neg_nd);
        return qp_raise_memory_error(ctx);
    }
    JS_FreeValue(ctx, neg_nd);
    /* |self| < 2**nbits <= 10**(e-1) < 10**e / 2: rounds to 0 */
    if ((double)qp_li_numbits(qp_long_unwrap(self)) * 0.30103 + 2 < (double)e)
        return JS_NewInt32(ctx, 0);
    p = qp_li_pow(ctx, JS_NewInt32(ctx, 10), (uint64_t)e);
    if (JS_IsException(p))
        return p;
    val = qp_long_value(ctx, self);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, p);
        return val;
    }
    res = qp_li_round_to_multiple(ctx, val, p);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, p);
    return res;
}

static JSValue qp_long_raise_fmt_error(JSContext *ctx, qp_error *err)
{
    QPType *t;
    switch (err->kind) {
    case QP_ERR_MEMORY:
        qp_error_clear(err);
        return qp_raise_memory_error(ctx);
    case QP_ERR_OVERFLOW:
        t = QP_EXC(ctx, OverflowError);
        break;
    default:
        t = QP_EXC(ctx, ValueError);
        break;
    }
    qp_raise(ctx, t, "%s", err->msg ? err->msg : "");
    qp_error_clear(err);
    return JS_EXCEPTION;
}

/* _PyLong_FormatAdvancedWriter: format(int, spec). */
static JSValue qp_long_format(JSContext *ctx, JSValueConst self, JSValueConst spec)
{
    JSValueConst u = qp_long_unwrap(self);
    JSValue plain;
    qp_strview sv;
    qp_format_spec f;
    qp_error err;
    DynBuf db;
    char namebuf[ATOM_GET_STR_BUF_SIZE];
    const char *tname;
    int kind, cls, base = 10, r;
    JSValue res;

    memset(&err, 0, sizeof(err));
    plain = qp_str_value(ctx, spec);
    if (JS_IsException(plain))
        return plain;
    if (qp_str_view_acquire(ctx, plain, &sv) < 0) {
        JS_FreeValue(ctx, plain);
        return JS_EXCEPTION;
    }
    if (sv.len == 0) {
        qp_str_view_release(ctx, &sv);
        JS_FreeValue(ctx, plain);
        return qp_str(ctx, self);
    }
    kind = sv.kind == 1 ? QP_FMT_KIND_LATIN1 : sv.kind == 2 ? QP_FMT_KIND_UTF16 : QP_FMT_KIND_UCS4;
    tname = qp_long_tp_name(ctx, qp_type_of(ctx, self), namebuf, sizeof(namebuf));
    r = qp_format_spec_parse(&f, sv.data, kind, sv.len, 'd', '>', tname, &err);
    qp_str_view_release(ctx, &sv);
    JS_FreeValue(ctx, plain);
    if (r < 0)
        return qp_long_raise_fmt_error(ctx, &err);
    cls = qp_format_long_classify(&f, &base, tname, &err);
    if (cls < 0)
        return qp_long_raise_fmt_error(ctx, &err);
    js_dbuf_init(ctx, &db);
    switch (cls) {
    case QP_FMT_LONG_DIGITS: {
        size_t n;
        int neg;
        char *digs = qp_long_digits_checked(ctx, u, base, &n, &neg);
        if (!digs) {
            dbuf_free(&db);
            return JS_EXCEPTION;
        }
        r = qp_format_long(&db, &f, neg, digs, n, &err);
        js_free(ctx, digs);
        break;
    }
    case QP_FMT_LONG_CHAR: {
        int64_t val;
        int overflow;
        qp_long_as_int64_overflow(u, &val, &overflow);
        r = qp_format_long_char(&db, &f, val, !overflow, &err);
        break;
    }
    default: {
        double d;
        if (qp_long_as_double(ctx, u, &d) < 0) {
            dbuf_free(&db);
            return JS_EXCEPTION;
        }
        r = qp_format_float_internal(&db, &f, d, &err);
        break;
    }
    }
    if (r < 0) {
        dbuf_free(&db);
        return qp_long_raise_fmt_error(ctx, &err);
    }
    res = qp_str_from_utf8(ctx, (const char *)db.buf, db.size);
    dbuf_free(&db);
    return res;
}

/* int.__format__(format_spec, /) */
static JSValue qp_long_meth_format(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    char namebuf[ATOM_GET_STR_BUF_SIZE];
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "__format__() argument must be str, not %s",
                                   qp_long_tp_name(ctx, qp_type_of(ctx, args[0]), namebuf,
                                                   sizeof(namebuf)));
    return qp_long_format(ctx, self, args[0]);
}

/* byteorder argument of to_bytes/from_bytes: 0 big, 1 little, -1 error. */
static int qp_long_byteorder(JSContext *ctx, JSValueConst v, const char *fname)
{
    const char *s;
    size_t len;
    int r;
    char namebuf[ATOM_GET_STR_BUF_SIZE];

    if (QP_IS_NULL(v))
        return 0;
    if (!qp_str_check(ctx, v)) {
        qp_raise_type_error(ctx, "%s() argument 'byteorder' must be str, not %s", fname,
                            JS_IsUndefined(v) ? "None" :
                            qp_long_tp_name(ctx, qp_type_of(ctx, v), namebuf, sizeof(namebuf)));
        return -1;
    }
    {
        JSValue plain = qp_str_value(ctx, v);
        if (JS_IsException(plain))
            return -1;
        s = qp_str_utf8(ctx, plain, &len);
        JS_FreeValue(ctx, plain);
    }
    if (!s) {
        /* lone surrogates cannot spell 'little' or 'big' */
        JS_FreeValue(ctx, JS_GetException(ctx));
        r = -2;
    } else if (len == 6 && !memcmp(s, "little", 6)) {
        r = 1;
    } else if (len == 3 && !memcmp(s, "big", 3)) {
        r = 0;
    } else {
        r = -2;
    }
    if (s)
        JS_FreeCString(ctx, s);
    if (r == -2) {
        qp_raise_value_error(ctx, "byteorder must be either 'little' or 'big'");
        return -1;
    }
    return r;
}

/* int.to_bytes(length=1, byteorder='big', *, signed=False) */
static JSValue qp_long_meth_to_bytes(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    static const char *const keywords[] = { "length", "byteorder", "signed", NULL };
    static const QPArgSpec spec = { "to_bytes", keywords, 0, 2, 0 };
    JSValueConst out[3];
    Py_ssize_t length = 1;
    int little, is_signed = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(out[0])) {
        if (qp_arg_ssize(ctx, out[0], &length) < 0)
            return JS_EXCEPTION;
        if (length < 0)
            return qp_raise_value_error(ctx, "length cannot be negative");
    }
    little = qp_long_byteorder(ctx, out[1], "to_bytes");
    if (little < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(out[2]) && qp_arg_bool(ctx, out[2], &is_signed) < 0)
        return JS_EXCEPTION;
    return qp_long_to_byte_array(ctx, qp_long_unwrap(self), length, little, is_signed);
}

/* int.from_bytes(bytes, byteorder='big', *, signed=False) (classmethod) */
static JSValue qp_long_meth_from_bytes(JSContext *ctx, JSValueConst type, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    static const char *const keywords[] = { "bytes", "byteorder", "signed", NULL };
    static const QPArgSpec spec = { "from_bytes", keywords, 1, 2, 0 };
    JSValueConst out[3];
    int little, is_signed = 0;
    JSValue bytes, v, res;
    QPBuffer view;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, out) < 0)
        return JS_EXCEPTION;
    little = qp_long_byteorder(ctx, out[1], "from_bytes");
    if (little < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(out[2]) && qp_arg_bool(ctx, out[2], &is_signed) < 0)
        return JS_EXCEPTION;
    if (qp_long_bytes_arg(ctx, out[0], &bytes, &view) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(bytes)) {
        v = qp_long_from_byte_array(ctx, view.buf, view.len, little, is_signed);
        qp_buffer_release(ctx, &view);
    } else {
        Py_ssize_t n;
        const uint8_t *d = qp_bytes_data(bytes, &n);
        v = qp_long_from_byte_array(ctx, d, n, little, is_signed);
        JS_FreeValue(ctx, bytes);
    }
    if (JS_IsException(v) || qp_type_from_value(type) == QP_TYPE(ctx, int))
        return v;
    res = qp_call1(ctx, type, v);
    JS_FreeValue(ctx, v);
    return res;
}

static JSValue qp_long_get_self(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_long_value(ctx, self);
}

static JSValue qp_long_get_const(JSContext *ctx, JSValueConst self, int magic)
{
    return JS_NewInt32(ctx, magic);
}

#define QP_LONG_DOC_getnewargs \
    "__getnewargs__($self, /)\n" \
    "--\n" \
    "\n"
#define QP_LONG_DOC_format \
    "__format__($self, format_spec, /)\n" \
    "--\n" \
    "\n" \
    "Convert to a string according to format_spec."
#define QP_LONG_DOC_round \
    "__round__($self, ndigits=None, /)\n" \
    "--\n" \
    "\n" \
    "Rounding an Integral returns itself.\n" \
    "\n" \
    "Rounding with an ndigits argument also returns an integer."
#define QP_LONG_DOC_sizeof \
    "__sizeof__($self, /)\n" \
    "--\n" \
    "\n" \
    "Returns size in memory, in bytes."
#define QP_LONG_DOC_bit_length \
    "bit_length($self, /)\n" \
    "--\n" \
    "\n" \
    "Number of bits necessary to represent self in binary.\n" \
    "\n" \
    ">>> bin(37)\n" \
    "'0b100101'\n" \
    ">>> (37).bit_length()\n" \
    "6"
#define QP_LONG_DOC_bit_count \
    "bit_count($self, /)\n" \
    "--\n" \
    "\n" \
    "Number of ones in the binary representation of the absolute value of self.\n" \
    "\n" \
    "Also known as the population count.\n" \
    "\n" \
    ">>> bin(13)\n" \
    "'0b1101'\n" \
    ">>> (13).bit_count()\n" \
    "3"
#define QP_LONG_DOC_as_integer_ratio \
    "as_integer_ratio($self, /)\n" \
    "--\n" \
    "\n" \
    "Return a pair of integers, whose ratio is equal to the original int.\n" \
    "\n" \
    "The ratio is in lowest terms and has a positive denominator.\n" \
    "\n" \
    ">>> (10).as_integer_ratio()\n" \
    "(10, 1)\n" \
    ">>> (-10).as_integer_ratio()\n" \
    "(-10, 1)\n" \
    ">>> (0).as_integer_ratio()\n" \
    "(0, 1)"
#define QP_LONG_DOC_to_bytes \
    "to_bytes($self, /, length=1, byteorder='big', *, signed=False)\n" \
    "--\n" \
    "\n" \
    "Return an array of bytes representing an integer.\n" \
    "\n" \
    "  length\n" \
    "    Length of bytes object to use.  An OverflowError is raised if\n" \
    "    the integer is not representable with the given number of bytes.\n" \
    "    Default is length 1.\n" \
    "  byteorder\n" \
    "    The byte order used to represent the integer.  If byteorder is\n" \
    "    'big', the most significant byte is at the beginning of the byte\n" \
    "    array.  If byteorder is 'little', the most significant byte is at\n" \
    "    the end of the byte array.  To request the native byte order of\n" \
    "    the host system, use sys.byteorder as the byte order value.\n" \
    "    Default is to use 'big'.\n" \
    "  signed\n" \
    "    Determines whether two's complement is used to represent the\n" \
    "    integer.  If signed is False and a negative integer is given,\n" \
    "    an OverflowError is raised."
#define QP_LONG_DOC_from_bytes \
    "from_bytes($type, /, bytes, byteorder='big', *, signed=False)\n" \
    "--\n" \
    "\n" \
    "Return the integer represented by the given array of bytes.\n" \
    "\n" \
    "  bytes\n" \
    "    Holds the array of bytes to convert.  The argument must either\n" \
    "    support the buffer protocol or be an iterable object producing\n" \
    "    bytes.  Bytes and bytearray are examples of built-in objects that\n" \
    "    support the buffer protocol.\n" \
    "  byteorder\n" \
    "    The byte order used to represent the integer.  If byteorder is\n" \
    "    'big', the most significant byte is at the beginning of the byte\n" \
    "    array.  If byteorder is 'little', the most significant byte is at\n" \
    "    the end of the byte array.  To request the native byte order of\n" \
    "    the host system, use sys.byteorder as the byte order value.\n" \
    "    Default is to use 'big'.\n" \
    "  signed\n" \
    "    Indicates whether two's complement is used to represent the\n" \
    "    integer."
#define QP_LONG_DOC_is_integer \
    "is_integer($self, /)\n" \
    "--\n" \
    "\n" \
    "Returns True. Exists for duck type compatibility with float.is_integer."

static const QPMethodDef qp_long_methods[] = {
    { "conjugate", qp_long_meth_long, QP_METH_NOARGS, 0,
      "Returns self, the complex conjugate of any int." },
    { "bit_length", qp_long_meth_bit_length, QP_METH_NOARGS, 0, QP_LONG_DOC_bit_length },
    { "bit_count", qp_long_meth_bit_count, QP_METH_NOARGS, 0, QP_LONG_DOC_bit_count },
    { "to_bytes", qp_long_meth_to_bytes, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      QP_LONG_DOC_to_bytes },
    { "from_bytes", qp_long_meth_from_bytes,
      QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 0, QP_LONG_DOC_from_bytes },
    { "as_integer_ratio", qp_long_meth_as_integer_ratio, QP_METH_NOARGS, 0,
      QP_LONG_DOC_as_integer_ratio },
    { "__trunc__", qp_long_meth_long, QP_METH_NOARGS, 0,
      "Truncating an Integral returns itself." },
    { "__floor__", qp_long_meth_long, QP_METH_NOARGS, 0,
      "Flooring an Integral returns itself." },
    { "__ceil__", qp_long_meth_long, QP_METH_NOARGS, 0,
      "Ceiling of an Integral returns itself." },
    { "__round__", qp_long_meth_round, QP_METH_FASTCALL, 0, QP_LONG_DOC_round },
    { "__getnewargs__", qp_long_meth_getnewargs, QP_METH_NOARGS, 0, QP_LONG_DOC_getnewargs },
    { "__format__", qp_long_meth_format, QP_METH_O, 0, QP_LONG_DOC_format },
    { "__sizeof__", qp_long_meth_sizeof, QP_METH_NOARGS, 0, QP_LONG_DOC_sizeof },
    { "is_integer", qp_long_meth_is_integer, QP_METH_NOARGS, 0, QP_LONG_DOC_is_integer },
    { NULL },
};

static const QPGetSetDef qp_long_getsets[] = {
    { "real", qp_long_get_self, NULL, 0, "the real part of a complex number" },
    { "imag", qp_long_get_const, NULL, 0, "the imaginary part of a complex number" },
    { "numerator", qp_long_get_self, NULL, 0,
      "the numerator of a rational number in lowest terms" },
    { "denominator", qp_long_get_const, NULL, 1,
      "the denominator of a rational number in lowest terms" },
    { NULL },
};

static const QPTypeSpec qp_long_spec = {
    .name = "int",
    .doc = "int([x]) -> integer\n"
           "int(x, base=10) -> integer\n"
           "\n"
           "Convert a number or string to an integer, or return 0 if no arguments\n"
           "are given.  If x is a number, return x.__int__().  For floating-point\n"
           "numbers, this truncates towards zero.\n"
           "\n"
           "If x is not a number or if base is given, then x must be a string,\n"
           "bytes, or bytearray instance representing an integer literal in the\n"
           "given base.  The literal can be preceded by '+' or '-' and be surrounded\n"
           "by whitespace.  The base defaults to 10.  Valid bases are 0 and 2-36.\n"
           "Base 0 means to interpret the base from the string as an integer\n"
           "iteral.\n"
           ">>> int('0b100', base=0)\n"
           "4",
    .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_LONG_SUBCLASS | QP_TPFLAGS_MATCH_SELF |
             QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BOXED,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_long_repr_slot,
        .tp_hash = qp_long_hash_slot,
        .tp_vectorcall = qp_long_vectorcall,
        .tp_richcompare = qp_long_richcompare_slot,
        .tp_new = qp_long_tp_new,
        .nb = {
            .nb_add = qp_long_add_slot,
            .nb_subtract = qp_long_sub_slot,
            .nb_multiply = qp_long_mul_slot,
            .nb_remainder = qp_long_mod_slot,
            .nb_divmod = qp_long_divmod_slot,
            .nb_power = qp_long_pow_slot,
            .nb_negative = qp_long_neg_slot,
            .nb_positive = qp_long_long_slot,
            .nb_absolute = qp_long_abs_slot,
            .nb_bool = qp_long_bool_slot,
            .nb_invert = qp_long_invert_slot,
            .nb_lshift = qp_long_lshift_slot,
            .nb_rshift = qp_long_rshift_slot,
            .nb_and = qp_long_and_slot,
            .nb_xor = qp_long_xor_slot,
            .nb_or = qp_long_or_slot,
            .nb_int = qp_long_long_slot,
            .nb_float = qp_long_float_slot,
            .nb_floor_divide = qp_long_floordiv_slot,
            .nb_true_divide = qp_long_truediv_slot,
            .nb_index = qp_long_long_slot,
        },
    },
    .methods = qp_long_methods,
    .getsets = qp_long_getsets,
    .members = NULL,
};

/* ========================================================================= */
/* bool (Objects/boolobject.c)                                                */
/* ========================================================================= */

static JSValue qp_boolobj_repr(JSContext *ctx, JSValueConst self)
{
    return js_new_string8(ctx, JS_VALUE_GET_BOOL(self) ? "True" : "False");
}

static JSValue qp_boolobj_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                     size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t nargs = qp_nargs(nargsf);
    int ok = 0;
    if (qp_arg_no_keywords(ctx, "bool", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "bool", (int)nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs) {
        ok = qp_bool(ctx, args[0]);
        if (ok < 0)
            return JS_EXCEPTION;
    }
    return qp_bool_new(ok);
}

static JSValue qp_boolobj_tp_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames)
{
    return qp_boolobj_vectorcall(ctx, QP_TYPE_VALUE(subtype), args, nargsf, kwnames);
}

static JSValue qp_boolobj_invert(JSContext *ctx, JSValueConst a)
{
    if (qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1, "%s",
                "Bitwise inversion '~' on bool is deprecated and will be removed in "
                "Python 3.16. This returns the bitwise inversion of the underlying int "
                "object and is usually not what you expect from negating "
                "a bool. Use the 'not' operator for boolean negation or "
                "~int(x) if you really want the bitwise inversion of the "
                "underlying int.") < 0)
        return JS_EXCEPTION;
    return qp_long_invert_slot(ctx, a);
}

static JSValue qp_boolobj_and(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_TAG_BOOL || JS_VALUE_GET_TAG(b) != JS_TAG_BOOL)
        return qp_long_and_slot(ctx, a, b);
    return qp_bool_new(JS_VALUE_GET_BOOL(a) && JS_VALUE_GET_BOOL(b));
}

static JSValue qp_boolobj_or(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_TAG_BOOL || JS_VALUE_GET_TAG(b) != JS_TAG_BOOL)
        return qp_long_or_slot(ctx, a, b);
    return qp_bool_new(JS_VALUE_GET_BOOL(a) || JS_VALUE_GET_BOOL(b));
}

static JSValue qp_boolobj_xor(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_TAG_BOOL || JS_VALUE_GET_TAG(b) != JS_TAG_BOOL)
        return qp_long_xor_slot(ctx, a, b);
    return qp_bool_new((JS_VALUE_GET_BOOL(a) != 0) != (JS_VALUE_GET_BOOL(b) != 0));
}

static const QPTypeSpec qp_bool_spec = {
    .name = "bool",
    .doc = "bool(object=False, /)\n"
           "--\n"
           "\n"
           "Returns True when the argument is true, False otherwise.\n"
           "The builtins True and False are the only two instances of the class bool.\n"
           "The class bool is a subclass of the class int, and cannot be subclassed.",
    .flags = QP_TPFLAGS_LONG_SUBCLASS | QP_TPFLAGS_MATCH_SELF | QP_TPFLAGS_IMMUTABLETYPE,
    .base = QP_T_int,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_BOXED,
    .nslots = 0,
    .slots = {
        .tp_repr = qp_boolobj_repr,
        .tp_vectorcall = qp_boolobj_vectorcall,
        .tp_new = qp_boolobj_tp_new,
        .nb = {
            .nb_invert = qp_boolobj_invert,
            .nb_and = qp_boolobj_and,
            .nb_xor = qp_boolobj_xor,
            .nb_or = qp_boolobj_or,
        },
    },
    .methods = NULL,
    .getsets = NULL,
    .members = NULL,
};

/* ========================================================================= */
/* Init                                                                       */
/* ========================================================================= */

static int qp_init_numbers(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    int digits = s->config.int_max_str_digits;

    s->int_max_str_digits = digits < 0 ? QP_LONG_DEFAULT_MAX_STR_DIGITS : digits;
    if (qp_type_ready(ctx, QP_TYPE(ctx, int), &qp_long_spec) < 0)
        return -1;
    if (qp_type_ready(ctx, QP_TYPE(ctx, bool), &qp_bool_spec) < 0)
        return -1;
    if (qp_init_float(ctx) < 0)
        return -1;
    if (qp_init_complex(ctx) < 0)
        return -1;
    return 0;
}
