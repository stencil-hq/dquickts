/*
 * qbun:crypto: OpenSSL (EVP API) bindings for node:crypto, the WebCrypto
 * API, Bun.CryptoHasher and Bun.password, plus the non-cryptographic
 * hashes of Bun.hash (bit-exact ports of the Zig std.hash functions Bun
 * uses and of rapidhash).
 *
 * Conventions:
 *   - "data" arguments are strings (UTF-8, lone surrogates become
 *     U+FFFD like Bun) or ArrayBuffer / typed array / DataView bytes;
 *   - functions taking a trailing callback run on the libuv threadpool
 *     and call cb(err | null, result); without a callback they run
 *     synchronously and return the result or throw;
 *   - OpenSSL failures become Errors shaped like Node's: message
 *     "error:1C800064:Provider routines::bad decrypt", code
 *     "ERR_OSSL_BAD_DECRYPT", library and reason properties.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <zlib.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/param_build.h>
#include <openssl/provider.h>
#include <openssl/decoder.h>
#include <openssl/encoder.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/rsa.h>
#include <openssl/dh.h>
#include <openssl/dsa.h>
#include <openssl/objects.h>
#include <openssl/crypto.h>
#include "qbun.h"

/* ------------------------------------------------------------ inputs */

typedef struct {
    const uint8_t *p;
    size_t len;
    uint8_t *alloc; /* js_malloc()ed UTF-8 conversion, or NULL */
} QCBuf;

/* UTF-8 of a string, unpaired surrogates replaced by U+FFFD */
static int qc_utf8(JSContext *ctx, JSValueConst s, QCBuf *b)
{
    size_t len, i, n;
    JS_BOOL wide;
    const void *data;
    uint8_t *q;

    b->alloc = NULL;
    data = JS_GetStringData(ctx, s, &len, &wide);
    if (!data)
        return -1;
    if (!wide) {
        const uint8_t *src = data;
        for (n = 0, i = 0; i < len; i++)
            n += src[i] >> 7;
        if (n == 0) {
            b->p = src;
            b->len = len;
            return 0;
        }
        q = js_malloc(ctx, len + n + 1);
        if (!q)
            return -1;
        b->alloc = q;
        b->p = q;
        for (i = 0; i < len; i++) {
            unsigned c = src[i];
            if (c < 0x80) {
                *q++ = c;
            } else {
                *q++ = 0xc0 | (c >> 6);
                *q++ = 0x80 | (c & 0x3f);
            }
        }
        b->len = q - b->p;
        return 0;
    } else {
        const uint16_t *src = data;
        q = js_malloc(ctx, len * 3 + 1);
        if (!q)
            return -1;
        b->alloc = q;
        b->p = q;
        for (i = 0; i < len; i++) {
            uint32_t c = src[i];
            if (c < 0x80) {
                *q++ = c;
            } else if (c < 0x800) {
                *q++ = 0xc0 | (c >> 6);
                *q++ = 0x80 | (c & 0x3f);
            } else {
                if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && src[i + 1] >= 0xdc00 &&
                    src[i + 1] <= 0xdfff) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (src[i + 1] - 0xdc00);
                    i++;
                    *q++ = 0xf0 | (c >> 18);
                    *q++ = 0x80 | ((c >> 12) & 0x3f);
                    *q++ = 0x80 | ((c >> 6) & 0x3f);
                    *q++ = 0x80 | (c & 0x3f);
                    continue;
                }
                if (c >= 0xd800 && c <= 0xdfff)
                    c = 0xfffd;
                *q++ = 0xe0 | (c >> 12);
                *q++ = 0x80 | ((c >> 6) & 0x3f);
                *q++ = 0x80 | (c & 0x3f);
            }
        }
        b->len = q - b->p;
        return 0;
    }
}

/* a string or bytes argument (borrowed: valid until the next JS call) */
static int qc_in(JSContext *ctx, JSValueConst v, QCBuf *b)
{
    b->alloc = NULL;
    if (JS_IsString(v))
        return qc_utf8(ctx, v, b);
    b->p = qb_get_bytes(ctx, v, &b->len);
    return b->p ? 0 : -1;
}

static void qc_in_free(JSContext *ctx, QCBuf *b)
{
    if (b->alloc)
        js_free(ctx, b->alloc);
    b->alloc = NULL;
}

/* a malloc()ed copy of a string or bytes argument; undefined and null
   give NULL when 'nullable' (*pnull set) */
static int qc_copy(JSContext *ctx, JSValueConst v, uint8_t **pout, size_t *plen, int nullable,
                   int *pnull)
{
    QCBuf b;
    uint8_t *out;

    *pout = NULL;
    *plen = 0;
    if (pnull)
        *pnull = 0;
    if (nullable && (JS_IsUndefined(v) || JS_IsNull(v))) {
        if (pnull)
            *pnull = 1;
        return 0;
    }
    if (qc_in(ctx, v, &b))
        return -1;
    out = malloc(b.len + 1);
    if (!out) {
        qc_in_free(ctx, &b);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(out, b.p, b.len);
    out[b.len] = 0;
    *pout = out;
    *plen = b.len;
    qc_in_free(ctx, &b);
    return 0;
}

static void qc_clear_free(uint8_t *p, size_t len)
{
    if (p) {
        OPENSSL_cleanse(p, len);
        free(p);
    }
}

static void qc_cstr_free(JSContext *ctx, const char *s)
{
    if (s)
        JS_FreeCString(ctx, s);
}

static int qc_int(JSContext *ctx, JSValueConst v, int def, int *pres)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *pres = def;
        return 0;
    }
    return JS_ToInt32(ctx, pres, v);
}

static int qc_size(JSContext *ctx, JSValueConst v, size_t *pres)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0) || d > (double)INT_MAX) {
        JS_ThrowRangeError(ctx, "invalid size");
        return -1;
    }
    *pres = (size_t)d;
    return 0;
}

/* ------------------------------------------------------------ errors */

static JSValue qc_new_error(JSContext *ctx, JSErrorEnum kind, const char *code, const char *msg)
{
    JSValue e = JS_NewErrorLen(ctx, kind, msg, strlen(msg));
    if (!JS_IsException(e) && code)
        JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return e;
}

static JSValue qc_throw(JSContext *ctx, JSErrorEnum kind, const char *code, const char *msg)
{
    JSValue e = qc_new_error(ctx, kind, code, msg);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

static const char *qc_lib_prefix(unsigned long e)
{
    switch (ERR_GET_LIB(e)) {
#define QC_LIB(n)                                                                                  \
    case ERR_LIB_##n:                                                                              \
        return #n "_";
#ifdef ERR_LIB_SYS
        QC_LIB(SYS)
#endif
#ifdef ERR_LIB_BN
        QC_LIB(BN)
#endif
#ifdef ERR_LIB_RSA
        QC_LIB(RSA)
#endif
#ifdef ERR_LIB_DH
        QC_LIB(DH)
#endif
#ifdef ERR_LIB_EVP
        QC_LIB(EVP)
#endif
#ifdef ERR_LIB_BUF
        QC_LIB(BUF)
#endif
#ifdef ERR_LIB_OBJ
        QC_LIB(OBJ)
#endif
#ifdef ERR_LIB_PEM
        QC_LIB(PEM)
#endif
#ifdef ERR_LIB_DSA
        QC_LIB(DSA)
#endif
#ifdef ERR_LIB_X509
        QC_LIB(X509)
#endif
#ifdef ERR_LIB_ASN1
        QC_LIB(ASN1)
#endif
#ifdef ERR_LIB_CONF
        QC_LIB(CONF)
#endif
#ifdef ERR_LIB_CRYPTO
        QC_LIB(CRYPTO)
#endif
#ifdef ERR_LIB_EC
        QC_LIB(EC)
#endif
#ifdef ERR_LIB_SSL
    case ERR_LIB_SSL:
        return "SSL_";
#endif
#ifdef ERR_LIB_BIO
        QC_LIB(BIO)
#endif
#ifdef ERR_LIB_PKCS7
        QC_LIB(PKCS7)
#endif
#ifdef ERR_LIB_X509V3
        QC_LIB(X509V3)
#endif
#ifdef ERR_LIB_PKCS12
        QC_LIB(PKCS12)
#endif
#ifdef ERR_LIB_RAND
        QC_LIB(RAND)
#endif
#ifdef ERR_LIB_OCSP
        QC_LIB(OCSP)
#endif
#ifdef ERR_LIB_UI
        QC_LIB(UI)
#endif
#ifdef ERR_LIB_OSSL_STORE
        QC_LIB(OSSL_STORE)
#endif
#ifdef ERR_LIB_CMS
        QC_LIB(CMS)
#endif
#ifdef ERR_LIB_KDF
        QC_LIB(KDF)
#endif
#undef QC_LIB
    default:
        return "";
    }
}

/* a Node-style error for the OpenSSL error 'e' ('fallback' message when
   e == 0) */
static JSValue qc_ossl_error(JSContext *ctx, unsigned long e, const char *fallback)
{
    char msg[256], code[160];
    const char *lib, *reason;
    size_t i, n;
    JSValue err;

    if (e == 0)
        return qc_new_error(ctx, JS_PLAIN_ERROR, NULL,
                            fallback ? fallback : "Unknown OpenSSL error");
    ERR_error_string_n(e, msg, sizeof(msg));
    err = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, strlen(msg));
    if (JS_IsException(err))
        return err;
    lib = ERR_lib_error_string(e);
    reason = ERR_reason_error_string(e);
    if (lib)
        JS_DefinePropertyValueStr(ctx, err, "library", JS_NewString(ctx, lib), JS_PROP_C_W_E);
    if (reason) {
        JS_DefinePropertyValueStr(ctx, err, "reason", JS_NewString(ctx, reason), JS_PROP_C_W_E);
        n = snprintf(code, sizeof(code), "ERR_%s%s", ERR_GET_LIB(e) == ERR_LIB_SSL ? "" : "OSSL_",
                     qc_lib_prefix(e));
        for (i = 0; reason[i] && n + 1 < sizeof(code); i++, n++) {
            char c = reason[i];
            code[n] = c == ' ' ? '_' : (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        code[n] = 0;
        JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    }
    return err;
}

static unsigned long qc_take_error(void)
{
    unsigned long e = ERR_peek_last_error();
    ERR_clear_error();
    return e;
}

static JSValue qc_throw_ossl(JSContext *ctx, const char *fallback)
{
    JSValue e = qc_ossl_error(ctx, qc_take_error(), fallback);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

/* ------------------------------------------------------------ results */

/* a Buffer that takes the malloc()ed 'p' (freed here) */
static JSValue qc_buffer_take(JSContext *ctx, uint8_t *p, size_t len)
{
    JSValue v = qb_new_buffer(ctx, p ? p : (const uint8_t *)"", len);
    if (p) {
        OPENSSL_cleanse(p, len);
        free(p);
    }
    return v;
}

static JSValue qc_bn_buffer(JSContext *ctx, const BIGNUM *bn, int pad)
{
    int n = BN_num_bytes(bn);
    uint8_t *p;
    if (pad > n)
        n = pad;
    p = malloc(n + 1);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    BN_bn2binpad(bn, p, n);
    return qc_buffer_take(ctx, p, n);
}

static BIGNUM *qc_bn_arg(JSContext *ctx, JSValueConst v)
{
    QCBuf b;
    BIGNUM *bn;
    if (qc_in(ctx, v, &b))
        return NULL;
    bn = BN_bin2bn(b.p, (int)b.len, NULL);
    qc_in_free(ctx, &b);
    if (!bn)
        JS_ThrowOutOfMemory(ctx);
    return bn;
}

/* --------------------------------------------------------------- jobs */

/* work that runs synchronously or on the threadpool: run() executes on
   either thread (no JS), done() builds the result on the JS thread,
   fini() frees the job specific data */
typedef struct QCJob QCJob;
struct QCJob {
    uv_work_t req;
    QBRuntime *qr;
    JSValue cb;
    int (*run)(QCJob *j);
    JSValue (*done)(JSContext *ctx, QCJob *j);
    void (*fini)(QCJob *j);
    int failed;
    unsigned long ossl_err;
    const char *err_msg;  /* static message (used when no OpenSSL error) */
    const char *err_code; /* code of err_msg errors */
    JSErrorEnum err_kind;
    uint8_t *out;         /* malloc()ed result bytes */
    size_t out_len;
    int result;           /* boolean / integer result */
};

static void *qc_job_new(JSContext *ctx, size_t size, int (*run)(QCJob *),
                        JSValue (*done)(JSContext *, QCJob *), void (*fini)(QCJob *))
{
    QCJob *j = calloc(1, size);
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    j->cb = JS_UNDEFINED;
    j->run = run;
    j->done = done;
    j->fini = fini;
    j->err_kind = JS_PLAIN_ERROR;
    return j;
}

static void qc_job_free(JSRuntime *rt, QCJob *j)
{
    if (j->fini)
        j->fini(j);
    if (j->out) {
        OPENSSL_cleanse(j->out, j->out_len);
        free(j->out);
    }
    JS_FreeValueRT(rt, j->cb);
    free(j);
}

static void qc_job_exec(QCJob *j)
{
    ERR_clear_error();
    if (j->run(j) < 0) {
        j->failed = 1;
        j->ossl_err = ERR_peek_last_error();
    }
    ERR_clear_error();
}

/* an error set by run() with a static message */
static int qc_job_fail(QCJob *j, JSErrorEnum kind, const char *code, const char *msg)
{
    j->err_kind = kind;
    j->err_code = code;
    j->err_msg = msg;
    ERR_clear_error();
    return -1;
}

static JSValue qc_job_error(JSContext *ctx, QCJob *j)
{
    if (j->err_msg)
        return qc_new_error(ctx, j->err_kind, j->err_code, j->err_msg);
    return qc_ossl_error(ctx, j->ossl_err, "Operation failed");
}

static void qc_job_work(uv_work_t *req)
{
    qc_job_exec(req->data);
}

static void qc_job_after(uv_work_t *req, int status)
{
    QCJob *j = req->data;
    JSContext *ctx = j->qr->ctx;
    JSValue args[2], ret;

    args[1] = JS_UNDEFINED;
    if (status < 0) {
        args[0] = qb_uv_error(ctx, status, "crypto", NULL, NULL);
    } else if (j->failed) {
        args[0] = qc_job_error(ctx, j);
    } else {
        args[1] = j->done(ctx, j);
        if (JS_IsException(args[1])) {
            args[0] = JS_GetException(ctx);
            args[1] = JS_UNDEFINED;
        } else {
            args[0] = JS_NULL;
        }
    }
    ret = qb_call(ctx, j->cb, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    qc_job_free(JS_GetRuntime(ctx), j);
}

/* run 'j' now (no callback) or on the threadpool; takes 'j' */
static JSValue qc_job_start(JSContext *ctx, QCJob *j, JSValueConst cb)
{
    JSValue v;
    int r;

    if (JS_IsFunction(ctx, cb)) {
        j->qr = qb_get(ctx);
        j->cb = JS_DupValue(ctx, cb);
        j->req.data = j;
        r = uv_queue_work(j->qr->loop, &j->req, qc_job_work, qc_job_after);
        if (r < 0) {
            qc_job_free(JS_GetRuntime(ctx), j);
            return qb_throw_uv(ctx, r, "crypto", NULL, NULL);
        }
        return JS_UNDEFINED;
    }
    qc_job_exec(j);
    if (j->failed) {
        v = qc_job_error(ctx, j);
        if (!JS_IsException(v))
            v = JS_Throw(ctx, v);
    } else {
        v = j->done(ctx, j);
    }
    qc_job_free(JS_GetRuntime(ctx), j);
    return v;
}

static JSValue qc_done_buffer(JSContext *ctx, QCJob *j)
{
    JSValue v = qb_new_buffer(ctx, j->out ? j->out : (const uint8_t *)"", j->out_len);
    return v;
}

static JSValue qc_done_bool(JSContext *ctx, QCJob *j)
{
    return JS_NewBool(ctx, j->result);
}

static JSValueConst qc_arg(int argc, JSValueConst *argv, int i)
{
    return i < argc ? argv[i] : JS_UNDEFINED;
}

/* the legacy provider (md4, bf, rc4...) is loaded on demand */
static uv_once_t qc_legacy_once = UV_ONCE_INIT;

static void qc_load_legacy(void)
{
    OSSL_PROVIDER_try_load(NULL, "legacy", 1);
    ERR_clear_error();
}

static void qc_lower(char *dst, const char *src, size_t size)
{
    size_t i;
    for (i = 0; src[i] && i + 1 < size; i++)
        dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
    dst[i] = 0;
}

/* ---------------------------------------------- Bun.hash (non-crypto) */

static inline uint64_t qc_rd64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline uint32_t qc_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t qc_bswap64(uint64_t x)
{
    x = ((x & 0x00ff00ff00ff00ffULL) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffULL);
    x = ((x & 0x0000ffff0000ffffULL) << 16) | ((x >> 16) & 0x0000ffff0000ffffULL);
    return (x << 32) | (x >> 32);
}

static inline uint32_t qc_bswap32(uint32_t x)
{
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}

static inline uint64_t qc_rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t qc_rotr64(uint64_t x, int r)
{
    return (x >> r) | (x << (64 - r));
}

static inline uint32_t qc_rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t qc_rotr32(uint32_t x, int r)
{
    return (x >> r) | (x << (32 - r));
}

/* 64x64 -> 128 bit multiplication: *a = low, *b = high */
static inline void qc_mum(uint64_t *a, uint64_t *b)
{
#if defined(__SIZEOF_INT128__)
    unsigned __int128 r = (unsigned __int128)*a * *b;
    *a = (uint64_t)r;
    *b = (uint64_t)(r >> 64);
#else
    uint64_t ha = *a >> 32, hb = *b >> 32, la = (uint32_t)*a, lb = (uint32_t)*b;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32), c = t < rl, lo;
    lo = t + (rm1 << 32);
    c += lo < t;
    *a = lo;
    *b = rh + (rm0 >> 32) + (rm1 >> 32) + c;
#endif
}

static inline uint64_t qc_mix(uint64_t a, uint64_t b)
{
    qc_mum(&a, &b);
    return a ^ b;
}

/* std.hash.Wyhash.hash(seed, input) */
static const uint64_t qc_wy_secret[4] = {
    0xa0761d6478bd642fULL,
    0xe7037ed1a0b428dbULL,
    0x8ebc6af09c88c6e3ULL,
    0x589965cc75374cc3ULL,
};

static uint64_t qc_wyhash(uint64_t seed, const uint8_t *in, size_t len)
{
    uint64_t s0, s1, s2, a, b;

    s0 = seed ^ qc_mix(seed ^ qc_wy_secret[0], qc_wy_secret[1]);
    s1 = s2 = s0;
    if (len <= 16) {
        if (len >= 4) {
            size_t end = len - 4, quarter = (len >> 3) << 2;
            a = ((uint64_t)qc_rd32(in) << 32) | qc_rd32(in + quarter);
            b = ((uint64_t)qc_rd32(in + end) << 32) | qc_rd32(in + end - quarter);
        } else if (len > 0) {
            a = ((uint64_t)in[0] << 16) | ((uint64_t)in[len >> 1] << 8) | in[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = 0, k = 0, n;
        const uint8_t *p;
        if (len >= 48) {
            while (i + 48 < len) {
                s0 = qc_mix(qc_rd64(in + i) ^ qc_wy_secret[1], qc_rd64(in + i + 8) ^ s0);
                s1 = qc_mix(qc_rd64(in + i + 16) ^ qc_wy_secret[2], qc_rd64(in + i + 24) ^ s1);
                s2 = qc_mix(qc_rd64(in + i + 32) ^ qc_wy_secret[3], qc_rd64(in + i + 40) ^ s2);
                i += 48;
            }
            s0 ^= s1 ^ s2;
        }
        p = in + i;
        n = len - i;
        while (k + 16 < n) {
            s0 = qc_mix(qc_rd64(p + k) ^ qc_wy_secret[1], qc_rd64(p + k + 8) ^ s0);
            k += 16;
        }
        a = qc_rd64(in + len - 16);
        b = qc_rd64(in + len - 8);
    }
    a ^= qc_wy_secret[1];
    b ^= s0;
    qc_mum(&a, &b);
    return qc_mix(a ^ qc_wy_secret[0] ^ (uint64_t)len, b ^ qc_wy_secret[1]);
}

/* std.hash.CityHash32.hash */
#define QC_C1 0xcc9e2d51U
#define QC_C2 0x1b873593U

static uint32_t qc_fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bU;
    h ^= h >> 13;
    h *= 0xc2b2ae35U;
    h ^= h >> 16;
    return h;
}

static uint32_t qc_mur(uint32_t a, uint32_t h)
{
    a *= QC_C1;
    a = qc_rotr32(a, 17);
    a *= QC_C2;
    h ^= a;
    h = qc_rotr32(h, 19);
    return h * 5 + 0xe6546b64U;
}

static uint32_t qc_city32(const uint8_t *s, size_t len)
{
    uint32_t h, g, f, a0, a1, a2, a3, a4;
    size_t iters;
    const uint8_t *p;

    if (len <= 4) {
        uint32_t b = 0, c = 9;
        size_t i;
        for (i = 0; i < len; i++) {
            b = b * QC_C1 + (uint32_t)(int32_t)(int8_t)s[i];
            c ^= b;
        }
        return qc_fmix32(qc_mur(b, qc_mur((uint32_t)len, c)));
    }
    if (len <= 12) {
        uint32_t a = (uint32_t)len, b = a * 5, c = 9, d = b;
        a += qc_rd32(s);
        b += qc_rd32(s + len - 4);
        c += qc_rd32(s + ((len >> 1) & 4));
        return qc_fmix32(qc_mur(c, qc_mur(b, qc_mur(a, d))));
    }
    if (len <= 24) {
        uint32_t a = qc_rd32(s + (len >> 1) - 4), b = qc_rd32(s + 4), c = qc_rd32(s + len - 8),
                 d = qc_rd32(s + (len >> 1)), e = qc_rd32(s), ff = qc_rd32(s + len - 4);
        return qc_fmix32(
            qc_mur(ff, qc_mur(e, qc_mur(d, qc_mur(c, qc_mur(b, qc_mur(a, (uint32_t)len)))))));
    }
    h = (uint32_t)len;
    g = QC_C1 * (uint32_t)len;
    f = g;
    a0 = qc_rotr32(qc_rd32(s + len - 4) * QC_C1, 17) * QC_C2;
    a1 = qc_rotr32(qc_rd32(s + len - 8) * QC_C1, 17) * QC_C2;
    a2 = qc_rotr32(qc_rd32(s + len - 16) * QC_C1, 17) * QC_C2;
    a3 = qc_rotr32(qc_rd32(s + len - 12) * QC_C1, 17) * QC_C2;
    a4 = qc_rotr32(qc_rd32(s + len - 20) * QC_C1, 17) * QC_C2;
    h ^= a0;
    h = qc_rotr32(h, 19);
    h = h * 5 + 0xe6546b64U;
    h ^= a2;
    h = qc_rotr32(h, 19);
    h = h * 5 + 0xe6546b64U;
    g ^= a1;
    g = qc_rotr32(g, 19);
    g = g * 5 + 0xe6546b64U;
    g ^= a3;
    g = qc_rotr32(g, 19);
    g = g * 5 + 0xe6546b64U;
    f += a4;
    f = qc_rotr32(f, 19);
    f = f * 5 + 0xe6546b64U;
    iters = (len - 1) / 20;
    p = s;
    while (iters != 0) {
        uint32_t b0 = qc_rotr32(qc_rd32(p) * QC_C1, 17) * QC_C2;
        uint32_t b1 = qc_rd32(p + 4);
        uint32_t b2 = qc_rotr32(qc_rd32(p + 8) * QC_C1, 17) * QC_C2;
        uint32_t b3 = qc_rotr32(qc_rd32(p + 12) * QC_C1, 17) * QC_C2;
        uint32_t b4 = qc_rd32(p + 16);
        uint32_t t;
        h ^= b0;
        h = qc_rotr32(h, 18);
        h = h * 5 + 0xe6546b64U;
        f += b1;
        f = qc_rotr32(f, 19);
        f = f * QC_C1;
        g += b2;
        g = qc_rotr32(g, 18);
        g = g * 5 + 0xe6546b64U;
        h ^= b3 + b1;
        h = qc_rotr32(h, 19);
        h = h * 5 + 0xe6546b64U;
        g ^= b4;
        g = qc_bswap32(g) * 5;
        h += b4 * 5;
        h = qc_bswap32(h);
        f += b0;
        t = h;
        h = f;
        f = g;
        g = t;
        p += 20;
        iters--;
    }
    g = qc_rotr32(g, 11) * QC_C1;
    g = qc_rotr32(g, 17) * QC_C1;
    f = qc_rotr32(f, 11) * QC_C1;
    f = qc_rotr32(f, 17) * QC_C1;
    h = qc_rotr32(h + g, 19);
    h = h * 5 + 0xe6546b64U;
    h = qc_rotr32(h, 17) * QC_C1;
    h = qc_rotr32(h + f, 19);
    h = h * 5 + 0xe6546b64U;
    h = qc_rotr32(h, 17) * QC_C1;
    return h;
}

/* std.hash.CityHash64.hash / hashWithSeed */
#define QC_K0 0xc3a5c85c97cb3127ULL
#define QC_K1 0xb492b66fbe98f273ULL
#define QC_K2 0x9ae16a3b2f90404fULL

static inline uint64_t qc_shiftmix(uint64_t v)
{
    return v ^ (v >> 47);
}

static inline uint64_t qc_len16mul(uint64_t low, uint64_t high, uint64_t mul)
{
    uint64_t a = (low ^ high) * mul, b;
    a ^= a >> 47;
    b = (high ^ a) * mul;
    b ^= b >> 47;
    b *= mul;
    return b;
}

static inline uint64_t qc_len16(uint64_t u, uint64_t v)
{
    return qc_len16mul(u, v, 0x9ddfea08eb382d69ULL);
}

typedef struct {
    uint64_t first, second;
} QCPair;

static QCPair qc_weak32(const uint8_t *p, uint64_t a, uint64_t b)
{
    uint64_t w = qc_rd64(p), x = qc_rd64(p + 8), y = qc_rd64(p + 16), z = qc_rd64(p + 24), c;
    QCPair r;
    a += w;
    b = qc_rotr64(b + a + z, 21);
    c = a;
    a += x;
    a += y;
    b += qc_rotr64(a, 44);
    r.first = a + z;
    r.second = b + c;
    return r;
}

static uint64_t qc_city64(const uint8_t *s, size_t slen)
{
    uint64_t len = slen, x, y, z, mul, t;
    QCPair v, w;
    const uint8_t *p;

    if (slen <= 16) {
        if (slen >= 8) {
            uint64_t a, b, c, d;
            mul = QC_K2 + len * 2;
            a = qc_rd64(s) + QC_K2;
            b = qc_rd64(s + slen - 8);
            c = qc_rotr64(b, 37) * mul + a;
            d = (qc_rotr64(a, 25) + b) * mul;
            return qc_len16mul(c, d, mul);
        }
        if (slen >= 4) {
            uint64_t a = qc_rd32(s);
            mul = QC_K2 + len * 2;
            return qc_len16mul(len + (a << 3), qc_rd32(s + slen - 4), mul);
        }
        if (slen > 0) {
            uint32_t yy = (uint32_t)s[0] + ((uint32_t)s[slen >> 1] << 8);
            uint32_t zz = (uint32_t)slen + ((uint32_t)s[slen - 1] << 2);
            return qc_shiftmix((uint64_t)yy * QC_K2 ^ (uint64_t)zz * QC_K0) * QC_K2;
        }
        return QC_K2;
    }
    if (slen <= 32) {
        uint64_t a, b, c, d;
        mul = QC_K2 + len * 2;
        a = qc_rd64(s) * QC_K1;
        b = qc_rd64(s + 8);
        c = qc_rd64(s + slen - 8) * mul;
        d = qc_rd64(s + slen - 16) * QC_K2;
        return qc_len16mul(qc_rotr64(a + b, 43) + qc_rotr64(c, 30) + d,
                           a + qc_rotr64(b + QC_K2, 18) + c, mul);
    }
    if (slen <= 64) {
        uint64_t a, b, c, d, e, f, g, h, u, vv, ww, xx, yy, zz, a1, b1;
        mul = QC_K2 + len * 2;
        a = qc_rd64(s) * QC_K2;
        b = qc_rd64(s + 8);
        c = qc_rd64(s + slen - 24);
        d = qc_rd64(s + slen - 32);
        e = qc_rd64(s + 16) * QC_K2;
        f = qc_rd64(s + 24) * 9;
        g = qc_rd64(s + slen - 8);
        h = qc_rd64(s + slen - 16) * mul;
        u = qc_rotr64(a + g, 43) + (qc_rotr64(b, 30) + c) * 9;
        vv = ((a + g) ^ d) + f + 1;
        ww = qc_bswap64((u + vv) * mul) + h;
        xx = qc_rotr64(e + f, 42) + c;
        yy = (qc_bswap64((vv + ww) * mul) + g) * mul;
        zz = e + f + c;
        a1 = qc_bswap64((xx + zz) * mul + yy) + b;
        b1 = qc_shiftmix((zz + a1) * mul + d + h) * mul;
        return b1 + xx;
    }
    x = qc_rd64(s + slen - 40);
    y = qc_rd64(s + slen - 16) + qc_rd64(s + slen - 56);
    z = qc_len16(qc_rd64(s + slen - 48) + len, qc_rd64(s + slen - 24));
    v = qc_weak32(s + slen - 64, len, z);
    w = qc_weak32(s + slen - 32, y + QC_K1, x);
    x = x * QC_K1 + qc_rd64(s);
    len = (len - 1) & ~(uint64_t)63;
    p = s;
    for (;;) {
        x = qc_rotr64(x + y + v.first + qc_rd64(p + 8), 37) * QC_K1;
        y = qc_rotr64(y + v.second + qc_rd64(p + 48), 42) * QC_K1;
        x ^= w.second;
        y += v.first + qc_rd64(p + 40);
        z = qc_rotr64(z + w.first, 33) * QC_K1;
        v = qc_weak32(p, v.second * QC_K1, x + w.first);
        w = qc_weak32(p + 32, z + w.second, y + qc_rd64(p + 16));
        t = z;
        z = x;
        x = t;
        p += 64;
        len -= 64;
        if (len == 0)
            break;
    }
    return qc_len16(qc_len16(v.first, w.first) + qc_shiftmix(y) * QC_K1 + z,
                    qc_len16(v.second, w.second) + x);
}

/* std.hash.murmur */
static uint32_t qc_murmur2_32(const uint8_t *s, size_t slen, uint32_t seed)
{
    const uint32_t m = 0x5bd1e995U;
    uint32_t len = (uint32_t)slen, h1 = seed ^ len, offset, rest, i;
    for (i = 0; i < (len >> 2); i++) {
        uint32_t k1 = qc_rd32(s + 4 * i);
        k1 *= m;
        k1 ^= k1 >> 24;
        k1 *= m;
        h1 *= m;
        h1 ^= k1;
    }
    offset = len & 0xfffffffcU;
    rest = len & 3;
    if (rest >= 3)
        h1 ^= (uint32_t)s[offset + 2] << 16;
    if (rest >= 2)
        h1 ^= (uint32_t)s[offset + 1] << 8;
    if (rest >= 1) {
        h1 ^= (uint32_t)s[offset];
        h1 *= m;
    }
    h1 ^= h1 >> 13;
    h1 *= m;
    h1 ^= h1 >> 15;
    return h1;
}

static uint64_t qc_murmur2_64(const uint8_t *s, size_t len, uint64_t seed)
{
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    uint64_t h1 = seed ^ ((uint64_t)len * m);
    size_t i, rest = len & 7, offset = len - rest;
    for (i = 0; i < len / 8; i++) {
        uint64_t k1 = qc_rd64(s + 8 * i);
        k1 *= m;
        k1 ^= k1 >> 47;
        k1 *= m;
        h1 ^= k1;
        h1 *= m;
    }
    if (rest > 0) {
        uint64_t k1 = 0;
        for (i = 0; i < rest; i++)
            k1 |= (uint64_t)s[offset + i] << (8 * i);
        h1 ^= k1;
        h1 *= m;
    }
    h1 ^= h1 >> 47;
    h1 *= m;
    h1 ^= h1 >> 47;
    return h1;
}

static uint32_t qc_murmur3_32(const uint8_t *s, size_t slen, uint32_t seed)
{
    uint32_t len = (uint32_t)slen, h1 = seed, i, k1, offset, rest;
    for (i = 0; i < (len >> 2); i++) {
        k1 = qc_rd32(s + 4 * i);
        k1 *= QC_C1;
        k1 = qc_rotl32(k1, 15);
        k1 *= QC_C2;
        h1 ^= k1;
        h1 = qc_rotl32(h1, 13);
        h1 *= 5;
        h1 += 0xe6546b64U;
    }
    k1 = 0;
    offset = len & 0xfffffffcU;
    rest = len & 3;
    if (rest == 3)
        k1 ^= (uint32_t)s[offset + 2] << 16;
    if (rest >= 2)
        k1 ^= (uint32_t)s[offset + 1] << 8;
    if (rest >= 1) {
        k1 ^= (uint32_t)s[offset];
        k1 *= QC_C1;
        k1 = qc_rotl32(k1, 15);
        k1 *= QC_C2;
        h1 ^= k1;
    }
    h1 ^= len;
    return qc_fmix32(h1);
}

/* std.hash.XxHash32 / XxHash64 */
#define QC_P32_1 0x9E3779B1U
#define QC_P32_2 0x85EBCA77U
#define QC_P32_3 0xC2B2AE3DU
#define QC_P32_4 0x27D4EB2FU
#define QC_P32_5 0x165667B1U
#define QC_P64_1 0x9E3779B185EBCA87ULL
#define QC_P64_2 0xC2B2AE3D27D4EB4FULL
#define QC_P64_3 0x165667B19E3779F9ULL
#define QC_P64_4 0x85EBCA77C2B2AE63ULL
#define QC_P64_5 0x27D4EB2F165667C5ULL

static inline uint32_t qc_xx32_round(uint32_t acc, uint32_t lane)
{
    acc += lane * QC_P32_2;
    acc = qc_rotl32(acc, 13);
    return acc * QC_P32_1;
}

static uint32_t qc_xxhash32(const uint8_t *p, size_t len, uint32_t seed)
{
    const uint8_t *end = p + len;
    uint32_t h;
    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + QC_P32_1 + QC_P32_2, v2 = seed + QC_P32_2, v3 = seed,
                 v4 = seed - QC_P32_1;
        do {
            v1 = qc_xx32_round(v1, qc_rd32(p));
            v2 = qc_xx32_round(v2, qc_rd32(p + 4));
            v3 = qc_xx32_round(v3, qc_rd32(p + 8));
            v4 = qc_xx32_round(v4, qc_rd32(p + 12));
            p += 16;
        } while (p <= limit);
        h = qc_rotl32(v1, 1) + qc_rotl32(v2, 7) + qc_rotl32(v3, 12) + qc_rotl32(v4, 18);
    } else {
        h = seed + QC_P32_5;
    }
    h += (uint32_t)len;
    while (p + 4 <= end) {
        h += qc_rd32(p) * QC_P32_3;
        h = qc_rotl32(h, 17) * QC_P32_4;
        p += 4;
    }
    while (p < end) {
        h += (*p) * QC_P32_5;
        h = qc_rotl32(h, 11) * QC_P32_1;
        p++;
    }
    h ^= h >> 15;
    h *= QC_P32_2;
    h ^= h >> 13;
    h *= QC_P32_3;
    h ^= h >> 16;
    return h;
}

static inline uint64_t qc_xx64_round(uint64_t acc, uint64_t lane)
{
    acc += lane * QC_P64_2;
    acc = qc_rotl64(acc, 31);
    return acc * QC_P64_1;
}

static inline uint64_t qc_xx64_merge(uint64_t acc, uint64_t v)
{
    acc ^= qc_xx64_round(0, v);
    return acc * QC_P64_1 + QC_P64_4;
}

static inline uint64_t qc_xx64_avalanche(uint64_t h)
{
    h ^= h >> 33;
    h *= QC_P64_2;
    h ^= h >> 29;
    h *= QC_P64_3;
    h ^= h >> 32;
    return h;
}

static uint64_t qc_xxhash64(const uint8_t *p, size_t len, uint64_t seed)
{
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + QC_P64_1 + QC_P64_2, v2 = seed + QC_P64_2, v3 = seed,
                 v4 = seed - QC_P64_1;
        do {
            v1 = qc_xx64_round(v1, qc_rd64(p));
            v2 = qc_xx64_round(v2, qc_rd64(p + 8));
            v3 = qc_xx64_round(v3, qc_rd64(p + 16));
            v4 = qc_xx64_round(v4, qc_rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = qc_rotl64(v1, 1) + qc_rotl64(v2, 7) + qc_rotl64(v3, 12) + qc_rotl64(v4, 18);
        h = qc_xx64_merge(h, v1);
        h = qc_xx64_merge(h, v2);
        h = qc_xx64_merge(h, v3);
        h = qc_xx64_merge(h, v4);
    } else {
        h = seed + QC_P64_5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= qc_xx64_round(0, qc_rd64(p));
        h = qc_rotl64(h, 27) * QC_P64_1 + QC_P64_4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)qc_rd32(p) * QC_P64_1;
        h = qc_rotl64(h, 23) * QC_P64_2 + QC_P64_3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p) * QC_P64_5;
        h = qc_rotl64(h, 11) * QC_P64_1;
        p++;
    }
    return qc_xx64_avalanche(h);
}

/* std.hash.XxHash3 (64-bit XXH3 with seed) */
static const uint8_t qc_xxh3_secret[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

static inline uint64_t qc_xxh3_avalanche(uint64_t h)
{
    h ^= h >> 37;
    h *= 0x165667919E3779F9ULL;
    h ^= h >> 32;
    return h;
}

static inline uint64_t qc_xxh3_rrmxmx(uint64_t h, uint64_t len)
{
    h ^= qc_rotl64(h, 49) ^ qc_rotl64(h, 24);
    h *= 0x9FB21C651E98DF25ULL;
    h ^= (h >> 35) + len;
    h *= 0x9FB21C651E98DF25ULL;
    return h ^ (h >> 28);
}

static inline uint64_t qc_fold64(uint64_t a, uint64_t b)
{
    qc_mum(&a, &b);
    return a ^ b;
}

static inline uint64_t qc_xxh3_mix16(const uint8_t *in, const uint8_t *sec, uint64_t seed)
{
    return qc_fold64(qc_rd64(in) ^ (qc_rd64(sec) + seed),
                     qc_rd64(in + 8) ^ (qc_rd64(sec + 8) - seed));
}

static void qc_xxh3_accumulate512(uint64_t *acc, const uint8_t *in, const uint8_t *sec)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t v = qc_rd64(in + 8 * i);
        uint64_t k = v ^ qc_rd64(sec + 8 * i);
        acc[i ^ 1] += v;
        acc[i] += (uint64_t)(uint32_t)k * (k >> 32);
    }
}

static void qc_xxh3_scramble(uint64_t *acc, const uint8_t *sec)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= qc_rd64(sec + 8 * i);
        a *= QC_P32_1;
        acc[i] = a;
    }
}

static uint64_t qc_xxh3_long(const uint8_t *in, size_t len, uint64_t seed)
{
    uint8_t secret[192];
    uint64_t acc[8] = {QC_P32_3, QC_P64_1, QC_P64_2, QC_P64_3,
                       QC_P64_4, QC_P32_2, QC_P64_5, QC_P32_1};
    const size_t stripes_per_block = (192 - 64) / 8, block_len = 64 * stripes_per_block;
    size_t nb_blocks, n, s, nb_stripes, i;
    uint64_t result;

    for (i = 0; i < 192 / 16; i++) {
        uint64_t lo = qc_rd64(qc_xxh3_secret + 16 * i) + seed;
        uint64_t hi = qc_rd64(qc_xxh3_secret + 16 * i + 8) - seed;
        int k;
        for (k = 0; k < 8; k++) {
            secret[16 * i + k] = (uint8_t)(lo >> (8 * k));
            secret[16 * i + 8 + k] = (uint8_t)(hi >> (8 * k));
        }
    }
    nb_blocks = (len - 1) / block_len;
    for (n = 0; n < nb_blocks; n++) {
        for (s = 0; s < stripes_per_block; s++)
            qc_xxh3_accumulate512(acc, in + n * block_len + s * 64, secret + s * 8);
        qc_xxh3_scramble(acc, secret + 192 - 64);
    }
    nb_stripes = ((len - 1) - block_len * nb_blocks) / 64;
    for (s = 0; s < nb_stripes; s++)
        qc_xxh3_accumulate512(acc, in + nb_blocks * block_len + s * 64, secret + s * 8);
    qc_xxh3_accumulate512(acc, in + len - 64, secret + 192 - 64 - 7);
    result = (uint64_t)len * QC_P64_1;
    for (i = 0; i < 4; i++)
        result += qc_fold64(acc[2 * i] ^ qc_rd64(secret + 11 + 16 * i),
                            acc[2 * i + 1] ^ qc_rd64(secret + 11 + 16 * i + 8));
    return qc_xxh3_avalanche(result);
}

static uint64_t qc_xxh3(const uint8_t *in, size_t len, uint64_t seed)
{
    const uint8_t *sec = qc_xxh3_secret;
    if (len == 0)
        return qc_xx64_avalanche(seed ^ (qc_rd64(sec + 56) ^ qc_rd64(sec + 64)));
    if (len < 4) {
        uint32_t combined = ((uint32_t)in[0] << 16) | ((uint32_t)in[len >> 1] << 24) |
                            (uint32_t)in[len - 1] | ((uint32_t)len << 8);
        uint64_t flip = (uint64_t)(qc_rd32(sec) ^ qc_rd32(sec + 4)) + seed;
        return qc_xx64_avalanche((uint64_t)combined ^ flip);
    }
    if (len <= 8) {
        uint64_t s2 = seed ^ ((uint64_t)qc_bswap32((uint32_t)seed) << 32);
        uint64_t in1 = qc_rd32(in), in2 = qc_rd32(in + len - 4);
        uint64_t flip = (qc_rd64(sec + 8) ^ qc_rd64(sec + 16)) - s2;
        uint64_t in64 = in2 + (in1 << 32);
        return qc_xxh3_rrmxmx(in64 ^ flip, len);
    }
    if (len <= 16) {
        uint64_t fl = (qc_rd64(sec + 24) ^ qc_rd64(sec + 32)) + seed;
        uint64_t fh = (qc_rd64(sec + 40) ^ qc_rd64(sec + 48)) - seed;
        uint64_t lo = qc_rd64(in) ^ fl, hi = qc_rd64(in + len - 8) ^ fh;
        uint64_t acc = (uint64_t)len + qc_bswap64(lo) + hi + qc_fold64(lo, hi);
        return qc_xxh3_avalanche(acc);
    }
    if (len <= 128) {
        uint64_t acc = (uint64_t)len * QC_P64_1;
        if (len > 32) {
            if (len > 64) {
                if (len > 96) {
                    acc += qc_xxh3_mix16(in + 48, sec + 96, seed);
                    acc += qc_xxh3_mix16(in + len - 64, sec + 112, seed);
                }
                acc += qc_xxh3_mix16(in + 32, sec + 64, seed);
                acc += qc_xxh3_mix16(in + len - 48, sec + 80, seed);
            }
            acc += qc_xxh3_mix16(in + 16, sec + 32, seed);
            acc += qc_xxh3_mix16(in + len - 32, sec + 48, seed);
        }
        acc += qc_xxh3_mix16(in, sec, seed);
        acc += qc_xxh3_mix16(in + len - 16, sec + 16, seed);
        return qc_xxh3_avalanche(acc);
    }
    if (len <= 240) {
        uint64_t acc = (uint64_t)len * QC_P64_1;
        size_t i, rounds = len / 16;
        for (i = 0; i < 8; i++)
            acc += qc_xxh3_mix16(in + 16 * i, sec + 16 * i, seed);
        acc = qc_xxh3_avalanche(acc);
        for (i = 8; i < rounds; i++)
            acc += qc_xxh3_mix16(in + 16 * i, sec + 16 * (i - 8) + 3, seed);
        acc += qc_xxh3_mix16(in + len - 16, sec + 136 - 17, seed);
        return qc_xxh3_avalanche(acc);
    }
    return qc_xxh3_long(in, len, seed);
}

/* rapidhash v1 (Bun's bun.deprecated.RapidHash) */
static const uint64_t qc_rapid_secret[3] = {
    0x2d358dccaa6c78a5ULL,
    0x8bb84b93962eacc9ULL,
    0x4b33a62ed433d4a3ULL,
};

static uint64_t qc_rapidhash(const uint8_t *p, size_t len, uint64_t seed)
{
    const uint64_t *sc = qc_rapid_secret;
    uint64_t a, b;

    seed ^= qc_mix(seed ^ sc[0], sc[1]) ^ (uint64_t)len;
    if (len <= 16) {
        if (len >= 4) {
            const uint8_t *plast = p + len - 4;
            uint64_t delta = (len & 24) >> (len >> 3);
            a = ((uint64_t)qc_rd32(p) << 32) | qc_rd32(plast);
            b = ((uint64_t)qc_rd32(p + delta) << 32) | qc_rd32(plast - delta);
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 56) | ((uint64_t)p[len >> 1] << 32) | p[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = len;
        if (i > 48) {
            uint64_t see1 = seed, see2 = seed;
            while (i >= 96) {
                seed = qc_mix(qc_rd64(p) ^ sc[0], qc_rd64(p + 8) ^ seed);
                see1 = qc_mix(qc_rd64(p + 16) ^ sc[1], qc_rd64(p + 24) ^ see1);
                see2 = qc_mix(qc_rd64(p + 32) ^ sc[2], qc_rd64(p + 40) ^ see2);
                seed = qc_mix(qc_rd64(p + 48) ^ sc[0], qc_rd64(p + 56) ^ seed);
                see1 = qc_mix(qc_rd64(p + 64) ^ sc[1], qc_rd64(p + 72) ^ see1);
                see2 = qc_mix(qc_rd64(p + 80) ^ sc[2], qc_rd64(p + 88) ^ see2);
                p += 96;
                i -= 96;
            }
            if (i >= 48) {
                seed = qc_mix(qc_rd64(p) ^ sc[0], qc_rd64(p + 8) ^ seed);
                see1 = qc_mix(qc_rd64(p + 16) ^ sc[1], qc_rd64(p + 24) ^ see1);
                see2 = qc_mix(qc_rd64(p + 32) ^ sc[2], qc_rd64(p + 40) ^ see2);
                p += 48;
                i -= 48;
            }
            seed ^= see1 ^ see2;
        }
        if (i > 16) {
            seed = qc_mix(qc_rd64(p) ^ sc[2], qc_rd64(p + 8) ^ seed ^ sc[1]);
            if (i > 32)
                seed = qc_mix(qc_rd64(p + 16) ^ sc[2], qc_rd64(p + 24) ^ seed);
        }
        a = qc_rd64(p + i - 16);
        b = qc_rd64(p + i - 8);
    }
    a ^= sc[1];
    b ^= seed;
    qc_mum(&a, &b);
    return qc_mix(a ^ sc[0] ^ (uint64_t)len, b ^ sc[1]);
}

enum {
    QC_H_WYHASH,
    QC_H_ADLER32,
    QC_H_CRC32,
    QC_H_CITY32,
    QC_H_CITY64,
    QC_H_XX32,
    QC_H_XX64,
    QC_H_XX3,
    QC_H_MUR32V2,
    QC_H_MUR32V3,
    QC_H_MUR64V2,
    QC_H_RAPID,
};

static JSValue qc_fast_hash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic)
{
    QCBuf b;
    uint64_t seed64 = 0, r64 = 0;
    uint32_t seed32 = 0, r32 = 0;
    int is64;
    JSValueConst sv = qc_arg(argc, argv, 1);

    is64 = magic == QC_H_WYHASH || magic == QC_H_CITY64 || magic == QC_H_XX64 ||
           magic == QC_H_XX3 || magic == QC_H_MUR64V2 || magic == QC_H_RAPID;
    if (!JS_IsUndefined(sv)) {
        if (is64) {
            if (JS_ToBigUint64(ctx, &seed64, sv))
                return JS_EXCEPTION;
        } else {
            if (JS_ToUint32(ctx, &seed32, sv))
                return JS_EXCEPTION;
        }
    }
    if (qc_in(ctx, qc_arg(argc, argv, 0), &b))
        return JS_EXCEPTION;
    switch (magic) {
    case QC_H_WYHASH:
        r64 = qc_wyhash(seed64, b.p, b.len);
        break;
    case QC_H_ADLER32: {
        uLong a = adler32(0L, Z_NULL, 0);
        size_t off = 0;
        while (off < b.len) {
            uInt n = b.len - off > 0x40000000 ? 0x40000000 : (uInt)(b.len - off);
            a = adler32(a, b.p + off, n);
            off += n;
        }
        r32 = (uint32_t)a;
        break;
    }
    case QC_H_CRC32: {
        uLong c = seed32;
        size_t off = 0;
        while (off < b.len) {
            uInt n = b.len - off > 0x40000000 ? 0x40000000 : (uInt)(b.len - off);
            c = crc32(c, b.p + off, n);
            off += n;
        }
        r32 = (uint32_t)c;
        break;
    }
    case QC_H_CITY32:
        r32 = qc_city32(b.p, b.len);
        break;
    case QC_H_CITY64:
        r64 = qc_len16(qc_city64(b.p, b.len) - QC_K2, seed64);
        break;
    case QC_H_XX32:
        r32 = qc_xxhash32(b.p, b.len, seed32);
        break;
    case QC_H_XX64:
        r64 = qc_xxhash64(b.p, b.len, seed64);
        break;
    case QC_H_XX3:
        r64 = qc_xxh3(b.p, b.len, seed64);
        break;
    case QC_H_MUR32V2:
        r32 = qc_murmur2_32(b.p, b.len, seed32);
        break;
    case QC_H_MUR32V3:
        r32 = qc_murmur3_32(b.p, b.len, seed32);
        break;
    case QC_H_MUR64V2:
        r64 = qc_murmur2_64(b.p, b.len, seed64);
        break;
    case QC_H_RAPID:
        r64 = qc_rapidhash(b.p, b.len, seed64);
        break;
    }
    qc_in_free(ctx, &b);
    return is64 ? JS_NewBigUint64(ctx, r64) : JS_NewUint32(ctx, r32);
}

/* ------------------------------------------------------------ digests */

static JSClassID qc_digest_class_id;

typedef struct {
    EVP_MD_CTX *md;   /* hash */
    EVP_MAC_CTX *mac; /* hmac / generic MAC */
    size_t out_len;   /* output size (XOF: requested length) */
    int xof;
} QCDigest;

static void qc_digest_finalizer(JSRuntime *rt, JSValue val)
{
    QCDigest *d = JS_GetOpaque(val, qc_digest_class_id);
    if (d) {
        EVP_MD_CTX_free(d->md);
        EVP_MAC_CTX_free(d->mac);
        js_free_rt(rt, d);
    }
}

static JSClassDef qc_digest_class = {"Digest", .finalizer = qc_digest_finalizer};

/* fetch a digest by OpenSSL name or legacy alias ("RSA-SHA256"); NULL
   if unknown. *psize is set to a forced output size (blake2b256) or 0 */
static EVP_MD *qc_fetch_md(const char *name, size_t *psize)
{
    EVP_MD *md;
    const EVP_MD *legacy;
    char lower[64];
    int pass;

    *psize = 0;
    qc_lower(lower, name, sizeof(lower));
    if (!strcmp(lower, "blake2b256")) {
        *psize = 32;
        name = "BLAKE2B-512";
    }
    for (pass = 0; pass < 2; pass++) {
        md = EVP_MD_fetch(NULL, name, NULL);
        if (!md) {
            legacy = EVP_get_digestbyname(name);
            if (legacy && EVP_MD_get0_name(legacy))
                md = EVP_MD_fetch(NULL, EVP_MD_get0_name(legacy), NULL);
        }
        if (md || pass)
            break;
        uv_once(&qc_legacy_once, qc_load_legacy);
    }
    ERR_clear_error();
    return md;
}

static int qc_md_is_xof(const EVP_MD *md)
{
    return (EVP_MD_get_flags(md) & EVP_MD_FLAG_XOF) != 0;
}

/* initialize 'ctx' for 'md' (output size 'force' if non zero) */
static int qc_md_init(EVP_MD_CTX *ctx, const EVP_MD *md, size_t force)
{
    if (force) {
        unsigned int sz = (unsigned int)force;
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_uint(OSSL_DIGEST_PARAM_SIZE, &sz);
        params[1] = OSSL_PARAM_construct_end();
        return EVP_DigestInit_ex2(ctx, md, params);
    }
    return EVP_DigestInit_ex2(ctx, md, NULL);
}

static JSValue qc_digest_wrap(JSContext *ctx, QCDigest *d)
{
    JSValue obj = JS_NewObjectClass(ctx, qc_digest_class_id);
    if (JS_IsException(obj)) {
        EVP_MD_CTX_free(d->md);
        EVP_MAC_CTX_free(d->mac);
        js_free(ctx, d);
        return obj;
    }
    JS_SetOpaque(obj, d);
    return obj;
}

/* hashNew(name, outputLength) */
static JSValue qc_hash_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    EVP_MD *md;
    size_t force;
    int outlen;
    QCDigest *d;

    if (qc_int(ctx, qc_arg(argc, argv, 1), -1, &outlen))
        return JS_EXCEPTION;
    name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!name)
        return JS_EXCEPTION;
    md = qc_fetch_md(name, &force);
    JS_FreeCString(ctx, name);
    if (!md)
        return JS_NULL;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) {
        EVP_MD_free(md);
        return JS_EXCEPTION;
    }
    d->md = EVP_MD_CTX_new();
    d->xof = qc_md_is_xof(md);
    if (!d->md || !qc_md_init(d->md, md, force)) {
        EVP_MD_free(md);
        EVP_MD_CTX_free(d->md);
        js_free(ctx, d);
        ERR_clear_error();
        return JS_NULL;
    }
    d->out_len = force ? force : (size_t)EVP_MD_get_size(md);
    if (d->xof && outlen >= 0)
        d->out_len = outlen;
    EVP_MD_free(md);
    return qc_digest_wrap(ctx, d);
}

static EVP_MAC_CTX *qc_hmac_ctx(const char *digest, const uint8_t *key, size_t keylen)
{
    EVP_MAC *mac;
    EVP_MAC_CTX *mctx;
    EVP_MD *md;
    size_t force;
    OSSL_PARAM params[2];
    char mdname[64];

    md = qc_fetch_md(digest, &force);
    if (!md)
        return NULL;
    if (force || qc_md_is_xof(md)) {
        EVP_MD_free(md);
        return NULL;
    }
    snprintf(mdname, sizeof(mdname), "%s", EVP_MD_get0_name(md));
    EVP_MD_free(md);
    mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac)
        return NULL;
    mctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!mctx)
        return NULL;
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, mdname, 0);
    params[1] = OSSL_PARAM_construct_end();
    if (!EVP_MAC_init(mctx, key ? key : (const uint8_t *)"", keylen, params)) {
        EVP_MAC_CTX_free(mctx);
        return NULL;
    }
    return mctx;
}

/* hmacNew(digest, key) */
static JSValue qc_hmac_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    QCBuf key;
    EVP_MAC_CTX *mctx;
    QCDigest *d;

    if (qc_in(ctx, qc_arg(argc, argv, 1), &key))
        return JS_EXCEPTION;
    name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!name) {
        qc_in_free(ctx, &key);
        return JS_EXCEPTION;
    }
    mctx = qc_hmac_ctx(name, key.p, key.len);
    JS_FreeCString(ctx, name);
    qc_in_free(ctx, &key);
    ERR_clear_error();
    if (!mctx)
        return JS_NULL;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) {
        EVP_MAC_CTX_free(mctx);
        return JS_EXCEPTION;
    }
    d->mac = mctx;
    d->out_len = EVP_MAC_CTX_get_mac_size(mctx);
    return qc_digest_wrap(ctx, d);
}

/* macNew(algorithm, key, { digest, cipher, iv, customization, salt, outputLength }) */
static JSValue qc_mac_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *alg = NULL, *digest = NULL, *cipher = NULL;
    QCBuf key = {0}, iv = {0}, custom = {0}, salt = {0};
    int has_iv = 0, has_custom = 0, has_salt = 0, has_size = 0, n = 0;
    size_t size = 0;
    JSValueConst opts = qc_arg(argc, argv, 2);
    JSValue v;
    EVP_MAC *mac = NULL;
    EVP_MAC_CTX *mctx = NULL;
    const OSSL_PARAM *settable;
    OSSL_PARAM params[8];
    QCDigest *d;
    JSValue ret = JS_EXCEPTION;
    const char *bad = NULL;

    alg = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!alg)
        goto done;
    if (qc_in(ctx, qc_arg(argc, argv, 1), &key))
        goto done;
    if (JS_IsObject(opts)) {
        v = JS_GetPropertyStr(ctx, opts, "digest");
        if (!JS_IsUndefined(v) && !(digest = JS_ToCString(ctx, v))) {
            JS_FreeValue(ctx, v);
            goto done;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "cipher");
        if (!JS_IsUndefined(v) && !(cipher = JS_ToCString(ctx, v))) {
            JS_FreeValue(ctx, v);
            goto done;
        }
        JS_FreeValue(ctx, v);
#define QC_OPT_BYTES(prop, buf, flag)                                                              \
    v = JS_GetPropertyStr(ctx, opts, prop);                                                        \
    if (!JS_IsUndefined(v)) {                                                                      \
        if (qc_in(ctx, v, &buf)) {                                                                 \
            JS_FreeValue(ctx, v);                                                                  \
            goto done;                                                                             \
        }                                                                                          \
        flag = 1;                                                                                  \
    }                                                                                              \
    JS_FreeValue(ctx, v);
        QC_OPT_BYTES("iv", iv, has_iv)
        QC_OPT_BYTES("customization", custom, has_custom)
        QC_OPT_BYTES("salt", salt, has_salt)
#undef QC_OPT_BYTES
        v = JS_GetPropertyStr(ctx, opts, "outputLength");
        if (!JS_IsUndefined(v)) {
            if (qc_size(ctx, v, &size)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            has_size = 1;
        }
        JS_FreeValue(ctx, v);
    }
    mac = EVP_MAC_fetch(NULL, alg, NULL);
    if (!mac) {
        ERR_clear_error();
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_MAC", "Invalid MAC algorithm");
        goto done;
    }
    settable = EVP_MAC_settable_ctx_params(mac);
#define QC_CHECK(name, label)                                                                      \
    if (!bad && !OSSL_PARAM_locate_const(settable, name))                                          \
        bad = label;
    if (digest) {
        QC_CHECK(OSSL_MAC_PARAM_DIGEST, "digest")
        params[n++] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)digest, 0);
    }
    if (cipher) {
        QC_CHECK(OSSL_MAC_PARAM_CIPHER, "cipher")
        params[n++] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, (char *)cipher, 0);
    }
    if (has_iv) {
        QC_CHECK(OSSL_MAC_PARAM_IV, "iv")
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_IV, (void *)iv.p, iv.len);
    }
    if (has_custom) {
        QC_CHECK(OSSL_MAC_PARAM_CUSTOM, "customization")
        params[n++] =
            OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_CUSTOM, (void *)custom.p, custom.len);
    }
    if (has_salt) {
        QC_CHECK(OSSL_MAC_PARAM_SALT, "salt")
        params[n++] =
            OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_SALT, (void *)salt.p, salt.len);
    }
    if (has_size) {
        QC_CHECK(OSSL_MAC_PARAM_SIZE, "outputLength")
        params[n++] = OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &size);
    }
#undef QC_CHECK
    params[n] = OSSL_PARAM_construct_end();
    if (bad) {
        char msg[128];
        snprintf(msg, sizeof(msg), "The %s option is not supported by the %s MAC", bad, alg);
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_MAC_OPTION", msg);
        goto done;
    }
    mctx = EVP_MAC_CTX_new(mac);
    if (!mctx || !EVP_MAC_init(mctx, key.p ? key.p : (const uint8_t *)"", key.len, params)) {
        qc_throw_ossl(ctx, "MAC initialization failed");
        goto done;
    }
    d = js_mallocz(ctx, sizeof(*d));
    if (!d)
        goto done;
    d->mac = mctx;
    d->out_len = EVP_MAC_CTX_get_mac_size(mctx);
    mctx = NULL;
    ret = qc_digest_wrap(ctx, d);
done:
    EVP_MAC_CTX_free(mctx);
    EVP_MAC_free(mac);
    qc_cstr_free(ctx, alg);
    qc_cstr_free(ctx, digest);
    qc_cstr_free(ctx, cipher);
    qc_in_free(ctx, &key);
    qc_in_free(ctx, &iv);
    qc_in_free(ctx, &custom);
    qc_in_free(ctx, &salt);
    return ret;
}

static QCDigest *qc_digest_get(JSContext *ctx, JSValueConst obj)
{
    QCDigest *d = JS_GetOpaque2(ctx, obj, qc_digest_class_id);
    if (d && !d->md && !d->mac) {
        qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_HASH_FINALIZED", "Digest already called");
        return NULL;
    }
    return d;
}

static JSValue qc_digest_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCDigest *d = qc_digest_get(ctx, this_val);
    QCBuf b;
    int ok;
    if (!d || qc_in(ctx, qc_arg(argc, argv, 0), &b))
        return JS_EXCEPTION;
    ok = d->md ? EVP_DigestUpdate(d->md, b.p, b.len) : EVP_MAC_update(d->mac, b.p, b.len);
    qc_in_free(ctx, &b);
    if (!ok)
        return qc_throw_ossl(ctx, "Digest update failed");
    return JS_UNDEFINED;
}

static JSValue qc_digest_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCDigest *d = qc_digest_get(ctx, this_val);
    uint8_t *out;
    size_t n = 0;
    unsigned int un = 0;
    int ok;

    if (!d)
        return JS_EXCEPTION;
    out = malloc(d->out_len + EVP_MAX_MD_SIZE + 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    if (d->md) {
        if (d->xof) {
            ok = d->out_len == 0 || EVP_DigestFinalXOF(d->md, out, d->out_len);
            n = d->out_len;
        } else {
            ok = EVP_DigestFinal_ex(d->md, out, &un);
            n = un;
        }
        EVP_MD_CTX_free(d->md);
        d->md = NULL;
    } else {
        ok = EVP_MAC_final(d->mac, out, &n, d->out_len + EVP_MAX_MD_SIZE);
        EVP_MAC_CTX_free(d->mac);
        d->mac = NULL;
    }
    if (!ok) {
        free(out);
        return qc_throw_ossl(ctx, "Digest failed");
    }
    return qc_buffer_take(ctx, out, n);
}

static JSValue qc_digest_copy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCDigest *d = qc_digest_get(ctx, this_val), *c;
    int outlen;
    if (!d || qc_int(ctx, qc_arg(argc, argv, 0), -1, &outlen))
        return JS_EXCEPTION;
    c = js_mallocz(ctx, sizeof(*c));
    if (!c)
        return JS_EXCEPTION;
    c->xof = d->xof;
    c->out_len = d->out_len;
    if (d->xof && outlen >= 0)
        c->out_len = outlen;
    if (d->md) {
        c->md = EVP_MD_CTX_new();
        if (!c->md || !EVP_MD_CTX_copy_ex(c->md, d->md)) {
            EVP_MD_CTX_free(c->md);
            js_free(ctx, c);
            return qc_throw_ossl(ctx, "Digest copy failed");
        }
    } else {
        c->mac = EVP_MAC_CTX_dup(d->mac);
        if (!c->mac) {
            js_free(ctx, c);
            return qc_throw_ossl(ctx, "Digest copy failed");
        }
    }
    return qc_digest_wrap(ctx, c);
}

static JSValue qc_digest_size(JSContext *ctx, JSValueConst this_val)
{
    QCDigest *d = JS_GetOpaque2(ctx, this_val, qc_digest_class_id);
    if (!d)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)d->out_len);
}

static const JSCFunctionListEntry qc_digest_proto_funcs[] = {
    JS_CFUNC_DEF("update", 1, qc_digest_update),
    JS_CFUNC_DEF("digest", 0, qc_digest_digest),
    JS_CFUNC_DEF("copy", 1, qc_digest_copy),
    JS_CGETSET_DEF("size", qc_digest_size, NULL),
};

/* hashOnce(name, data, outputLength) */
static JSValue qc_hash_once(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    EVP_MD *md;
    EVP_MD_CTX *mctx;
    size_t force, n;
    int outlen, ok;
    unsigned int un = 0;
    QCBuf b;
    uint8_t *out;

    if (qc_int(ctx, qc_arg(argc, argv, 2), -1, &outlen))
        return JS_EXCEPTION;
    name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!name)
        return JS_EXCEPTION;
    md = qc_fetch_md(name, &force);
    JS_FreeCString(ctx, name);
    if (!md)
        return JS_NULL;
    if (qc_in(ctx, qc_arg(argc, argv, 1), &b)) {
        EVP_MD_free(md);
        return JS_EXCEPTION;
    }
    n = force ? force : (size_t)EVP_MD_get_size(md);
    if (qc_md_is_xof(md) && outlen >= 0)
        n = outlen;
    out = malloc(n + EVP_MAX_MD_SIZE + 1);
    mctx = EVP_MD_CTX_new();
    ok = out && mctx && qc_md_init(mctx, md, force) && EVP_DigestUpdate(mctx, b.p, b.len);
    if (ok) {
        if (qc_md_is_xof(md)) {
            ok = n == 0 || EVP_DigestFinalXOF(mctx, out, n);
        } else {
            ok = EVP_DigestFinal_ex(mctx, out, &un);
            n = un;
        }
    }
    EVP_MD_CTX_free(mctx);
    EVP_MD_free(md);
    qc_in_free(ctx, &b);
    if (!ok) {
        free(out);
        return qc_throw_ossl(ctx, "Digest failed");
    }
    return qc_buffer_take(ctx, out, n);
}

/* digestInfo(name) */
static JSValue qc_digest_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    EVP_MD *md;
    size_t force;
    JSValue obj;
    char lower[64];

    name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!name)
        return JS_EXCEPTION;
    md = qc_fetch_md(name, &force);
    JS_FreeCString(ctx, name);
    if (!md)
        return JS_NULL;
    obj = JS_NewObject(ctx);
    qc_lower(lower, EVP_MD_get0_name(md), sizeof(lower));
    qb_set(ctx, obj, "name", JS_NewString(ctx, lower));
    qb_set(ctx, obj, "size", JS_NewInt32(ctx, force ? (int)force : EVP_MD_get_size(md)));
    qb_set(ctx, obj, "blockSize", JS_NewInt32(ctx, EVP_MD_get_block_size(md)));
    qb_set(ctx, obj, "xof", JS_NewBool(ctx, qc_md_is_xof(md)));
    EVP_MD_free(md);
    return obj;
}

/* name lists */
typedef struct {
    JSContext *ctx;
    JSValue arr;
    uint32_t n;
    int kind; /* 0 digests, 1 ciphers */
} QCList;

static void qc_list_push(QCList *l, const char *name)
{
    JS_SetPropertyUint32(l->ctx, l->arr, l->n++, JS_NewString(l->ctx, name));
}

static void qc_md_list_cb(const EVP_MD *m, const char *from, const char *to, void *arg)
{
    QCList *l = arg;
    const EVP_MD *real;
    EVP_MD *fetched;
    if (!from)
        return;
    real = EVP_get_digestbyname(from);
    if (!real || !EVP_MD_get0_name(real))
        return;
    fetched = EVP_MD_fetch(NULL, EVP_MD_get0_name(real), NULL);
    if (!fetched) {
        ERR_clear_error();
        return;
    }
    EVP_MD_free(fetched);
    qc_list_push(l, from);
}

static void qc_cipher_list_cb(const EVP_CIPHER *c, const char *from, const char *to, void *arg)
{
    QCList *l = arg;
    const EVP_CIPHER *real;
    EVP_CIPHER *fetched;
    if (!from)
        return;
    real = EVP_get_cipherbyname(from);
    if (!real || !EVP_CIPHER_get0_name(real))
        return;
    fetched = EVP_CIPHER_fetch(NULL, EVP_CIPHER_get0_name(real), NULL);
    if (!fetched) {
        ERR_clear_error();
        return;
    }
    EVP_CIPHER_free(fetched);
    qc_list_push(l, from);
}

static JSValue qc_get_hashes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCList l = {ctx, JS_NewArray(ctx), 0, 0};
    if (JS_IsException(l.arr))
        return l.arr;
    EVP_MD_do_all_sorted(qc_md_list_cb, &l);
    return l.arr;
}

static JSValue qc_get_ciphers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCList l = {ctx, JS_NewArray(ctx), 0, 1};
    if (JS_IsException(l.arr))
        return l.arr;
    EVP_CIPHER_do_all_sorted(qc_cipher_list_cb, &l);
    return l.arr;
}

static JSValue qc_get_curves(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t n = EC_get_builtin_curves(NULL, 0), i;
    EC_builtin_curve *curves;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    curves = malloc(sizeof(*curves) * (n ? n : 1));
    if (!curves) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowOutOfMemory(ctx);
    }
    n = EC_get_builtin_curves(curves, n);
    for (i = 0; i < n; i++) {
        const char *sn = OBJ_nid2sn(curves[i].nid);
        if (sn)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, sn));
    }
    free(curves);
    return arr;
}

static void qc_mac_name_cb(const char *name, void *arg)
{
    QCList *l = arg;
    char lower[64];
    if (name[0] >= '0' && name[0] <= '9')
        return; /* numeric OID alias */
    qc_lower(lower, name, sizeof(lower));
    qc_list_push(l, lower);
}

static void qc_mac_list_cb(EVP_MAC *mac, void *arg)
{
    EVP_MAC_names_do_all(mac, qc_mac_name_cb, arg);
}

static JSValue qc_get_macs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCList l = {ctx, JS_NewArray(ctx), 0, 2};
    if (JS_IsException(l.arr))
        return l.arr;
    EVP_MAC_do_all_provided(NULL, qc_mac_list_cb, &l);
    return l.arr;
}

/* ------------------------------------------------------------- random */

static int qc_rand(uint8_t *p, size_t n)
{
    while (n > 0) {
        int chunk = n > (size_t)INT_MAX ? INT_MAX : (int)n;
        if (RAND_bytes(p, chunk) != 1)
            return -1;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

/* randomFill(target, offset, size) */
static JSValue qc_random_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uint8_t *p;
    size_t len, off, size;
    p = qb_get_bytes(ctx, qc_arg(argc, argv, 0), &len);
    if (!p || qc_size(ctx, qc_arg(argc, argv, 1), &off) ||
        qc_size(ctx, qc_arg(argc, argv, 2), &size))
        return JS_EXCEPTION;
    if (off > len || size > len - off)
        return qc_throw(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE",
                        "The value of \"size + offset\" is out of range.");
    if (qc_rand(p + off, size))
        return qc_throw_ossl(ctx, "Random bytes generation failed");
    return JS_UNDEFINED;
}

static int qc_random_run(QCJob *j)
{
    j->out = malloc(j->out_len + 1);
    if (!j->out)
        return qc_job_fail(j, JS_PLAIN_ERROR, NULL, "out of memory");
    return qc_rand(j->out, j->out_len);
}

/* randomBytes(size, cb?) */
static JSValue qc_random_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t size;
    QCJob *j;
    if (qc_size(ctx, qc_arg(argc, argv, 0), &size))
        return JS_EXCEPTION;
    j = qc_job_new(ctx, sizeof(*j), qc_random_run, qc_done_buffer, NULL);
    if (!j)
        return JS_EXCEPTION;
    j->out_len = size;
    return qc_job_start(ctx, j, qc_arg(argc, argv, 1));
}

static JSValue qc_timing_safe_equal(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    uint8_t *a, *b;
    size_t la, lb;
    a = qb_get_bytes(ctx, qc_arg(argc, argv, 0), &la);
    if (!a)
        return JS_EXCEPTION;
    b = qb_get_bytes(ctx, qc_arg(argc, argv, 1), &lb);
    if (!b)
        return JS_EXCEPTION;
    if (la != lb)
        return JS_FALSE;
    return JS_NewBool(ctx, CRYPTO_memcmp(a, b, la) == 0);
}

/* ------------------------------------------------------------ ciphers */

static JSClassID qc_cipher_class_id;

typedef struct {
    EVP_CIPHER_CTX *ctx;
    int encrypt;
    int mode;
    int aead;
    int chacha;
    int auth_tag_len;   /* -1: not set */
    int auth_tag_state; /* 0 unknown, 1 known, 2 passed to OpenSSL */
    uint8_t auth_tag[EVP_GCM_TLS_TAG_LEN];
    int pending_auth_failed;
    int64_t max_message_size; /* CCM */
} QCCipher;

static void qc_cipher_finalizer(JSRuntime *rt, JSValue val)
{
    QCCipher *c = JS_GetOpaque(val, qc_cipher_class_id);
    if (c) {
        EVP_CIPHER_CTX_free(c->ctx);
        js_free_rt(rt, c);
    }
}

static JSClassDef qc_cipher_class = {"Cipher", .finalizer = qc_cipher_finalizer};

static EVP_CIPHER *qc_fetch_cipher(const char *name)
{
    EVP_CIPHER *c;
    const EVP_CIPHER *legacy;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        c = EVP_CIPHER_fetch(NULL, name, NULL);
        if (!c) {
            legacy = EVP_get_cipherbyname(name);
            if (legacy && EVP_CIPHER_get0_name(legacy))
                c = EVP_CIPHER_fetch(NULL, EVP_CIPHER_get0_name(legacy), NULL);
        }
        if (c || pass)
            break;
        uv_once(&qc_legacy_once, qc_load_legacy);
    }
    ERR_clear_error();
    return c;
}

static int qc_valid_gcm_tag(int n)
{
    return n == 4 || n == 8 || (n >= 12 && n <= 16);
}

static int qc_cipher_is_aead(const EVP_CIPHER *cipher, int mode)
{
    return mode == EVP_CIPH_GCM_MODE || mode == EVP_CIPH_CCM_MODE || mode == EVP_CIPH_OCB_MODE ||
           (EVP_CIPHER_get_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER) != 0;
}

/* cipherNew(name, key, iv, encrypt, authTagLength) */
static JSValue qc_cipher_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    EVP_CIPHER *cipher = NULL;
    QCBuf key = {0}, iv = {0};
    int has_iv, encrypt, tag_len, mode, ivlen, expected_iv, aead, chacha;
    QCCipher *c = NULL;
    JSValue obj = JS_EXCEPTION;
    char msg[128];

    if ((encrypt = JS_ToBool(ctx, qc_arg(argc, argv, 3))) < 0 ||
        qc_int(ctx, qc_arg(argc, argv, 4), -1, &tag_len))
        return JS_EXCEPTION;
    name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!name)
        return JS_EXCEPTION;
    if (qc_in(ctx, qc_arg(argc, argv, 1), &key))
        goto fail;
    has_iv = !JS_IsNull(qc_arg(argc, argv, 2)) && !JS_IsUndefined(qc_arg(argc, argv, 2));
    if (has_iv && qc_in(ctx, qc_arg(argc, argv, 2), &iv))
        goto fail;
    cipher = qc_fetch_cipher(name);
    if (!cipher) {
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
        goto fail;
    }
    mode = EVP_CIPHER_get_mode(cipher);
    aead = qc_cipher_is_aead(cipher, mode);
    chacha = EVP_CIPHER_is_a(cipher, "ChaCha20-Poly1305");
    expected_iv = EVP_CIPHER_get_iv_length(cipher);
    ivlen = has_iv ? (int)iv.len : 0;
    /* IV checks (Node's CipherBase::InitIv) */
    if (aead) {
        int ok = has_iv && ivlen > 0;
        if (mode == EVP_CIPH_CCM_MODE)
            ok = ok && ivlen >= 7 && ivlen <= 13;
        else if (mode == EVP_CIPH_OCB_MODE)
            ok = ok && ivlen <= 15;
        else if (chacha)
            ok = ok && ivlen <= 12;
        if (!ok) {
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_IV", "Invalid initialization vector");
            goto fail;
        }
    } else if (!(expected_iv == 0 && !has_iv) && ivlen != expected_iv) {
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_IV", "Invalid initialization vector");
        goto fail;
    }
    /* auth tag length checks (Node's InitAuthenticated) */
    if (aead) {
        if (mode == EVP_CIPH_GCM_MODE) {
            if (tag_len != -1 && !qc_valid_gcm_tag(tag_len)) {
                snprintf(msg, sizeof(msg), "Invalid authentication tag length: %d", tag_len);
                qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_AUTH_TAG", msg);
                goto fail;
            }
        } else {
            if (tag_len == -1) {
                if (mode == EVP_CIPH_CCM_MODE) {
                    snprintf(msg, sizeof(msg), "authTagLength required for %s", name);
                    qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_AUTH_TAG", msg);
                    goto fail;
                }
                tag_len = 16;
            }
            if ((mode == EVP_CIPH_CCM_MODE && (tag_len < 4 || tag_len > 16 || (tag_len & 1))) ||
                ((mode == EVP_CIPH_OCB_MODE || chacha) && (tag_len < 1 || tag_len > 16))) {
                snprintf(msg, sizeof(msg), "Invalid authentication tag length: %d", tag_len);
                qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_AUTH_TAG", msg);
                goto fail;
            }
        }
    }
    c = js_mallocz(ctx, sizeof(*c));
    if (!c)
        goto fail;
    c->ctx = EVP_CIPHER_CTX_new();
    c->encrypt = encrypt;
    c->mode = mode;
    c->aead = aead;
    c->chacha = chacha;
    c->auth_tag_len = tag_len;
    c->max_message_size = -1;
    if (!c->ctx)
        goto ossl_fail;
    if (mode == EVP_CIPH_WRAP_MODE)
        EVP_CIPHER_CTX_set_flags(c->ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    if (!EVP_CipherInit_ex2(c->ctx, cipher, NULL, NULL, encrypt, NULL))
        goto ossl_fail;
    if (aead && ivlen != expected_iv &&
        EVP_CIPHER_CTX_ctrl(c->ctx, EVP_CTRL_AEAD_SET_IVLEN, ivlen, NULL) <= 0) {
        ERR_clear_error();
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_IV", "Invalid initialization vector");
        goto fail;
    }
    if (aead && mode != EVP_CIPH_GCM_MODE) {
        /* CCM, OCB and ChaCha20-Poly1305 need the tag length up front */
        if (EVP_CIPHER_CTX_ctrl(c->ctx, EVP_CTRL_AEAD_SET_TAG, tag_len, NULL) <= 0) {
            ERR_clear_error();
            snprintf(msg, sizeof(msg), "Invalid authentication tag length: %d", tag_len);
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_AUTH_TAG", msg);
            goto fail;
        }
        if (mode == EVP_CIPH_CCM_MODE) {
            int l = 15 - ivlen;
            c->max_message_size = l >= 8 ? INT_MAX : ((int64_t)1 << (8 * l)) - 1;
        }
    }
    if ((int)key.len != EVP_CIPHER_get_key_length(cipher) &&
        EVP_CIPHER_CTX_set_key_length(c->ctx, (int)key.len) <= 0) {
        ERR_clear_error();
        qc_throw(ctx, JS_RANGE_ERROR, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
        goto fail;
    }
    if (!EVP_CipherInit_ex2(c->ctx, NULL, key.p, has_iv && ivlen ? iv.p : NULL, encrypt, NULL))
        goto ossl_fail;
    obj = JS_NewObjectClass(ctx, qc_cipher_class_id);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, c);
    c = NULL;
    goto fail;
ossl_fail:
    qc_throw_ossl(ctx, "Cipher initialization failed");
fail:
    if (c) {
        EVP_CIPHER_CTX_free(c->ctx);
        js_free(ctx, c);
    }
    EVP_CIPHER_free(cipher);
    JS_FreeCString(ctx, name);
    qc_in_free(ctx, &key);
    qc_in_free(ctx, &iv);
    return obj;
}

static QCCipher *qc_cipher_get(JSContext *ctx, JSValueConst obj)
{
    QCCipher *c = JS_GetOpaque2(ctx, obj, qc_cipher_class_id);
    if (c && !c->ctx) {
        qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_STATE", "Invalid state for operation");
        return NULL;
    }
    return c;
}

static int qc_cipher_pass_tag(QCCipher *c)
{
    if (c->auth_tag_state == 1) {
        if (EVP_CIPHER_CTX_ctrl(c->ctx, EVP_CTRL_AEAD_SET_TAG, c->auth_tag_len, c->auth_tag) <= 0)
            return 0;
        c->auth_tag_state = 2;
    }
    return 1;
}

static JSValue qc_cipher_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCCipher *c = qc_cipher_get(ctx, this_val);
    QCBuf in;
    uint8_t *out;
    int outl = 0, r;
    size_t cap;

    if (!c || qc_in(ctx, qc_arg(argc, argv, 0), &in))
        return JS_EXCEPTION;
    if (in.len > INT_MAX) {
        qc_in_free(ctx, &in);
        return qc_throw(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE", "Input is too large");
    }
    if (c->mode == EVP_CIPH_CCM_MODE && (int64_t)in.len > c->max_message_size) {
        qc_in_free(ctx, &in);
        return qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_MESSAGELEN",
                        "Invalid message length");
    }
    if (!c->encrypt && c->aead && !qc_cipher_pass_tag(c)) {
        qc_in_free(ctx, &in);
        return qc_throw_ossl(ctx, "Trying to add data in unsupported state");
    }
    cap = in.len + 2 * EVP_MAX_BLOCK_LENGTH + 16;
    out = malloc(cap);
    if (!out) {
        qc_in_free(ctx, &in);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = EVP_CipherUpdate(c->ctx, out, &outl, in.p, (int)in.len);
    qc_in_free(ctx, &in);
    if (!r) {
        free(out);
        if (!c->encrypt && c->mode == EVP_CIPH_CCM_MODE) {
            /* reported by final() */
            ERR_clear_error();
            c->pending_auth_failed = 1;
            return qb_new_buffer(ctx, (const uint8_t *)"", 0);
        }
        return qc_throw_ossl(ctx, "Trying to add data in unsupported state");
    }
    return qc_buffer_take(ctx, out, outl);
}

static JSValue qc_cipher_final(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCCipher *c = qc_cipher_get(ctx, this_val);
    uint8_t out[2 * EVP_MAX_BLOCK_LENGTH + 16];
    int outl = 0, ok;

    if (!c)
        return JS_EXCEPTION;
    if (!c->encrypt && c->aead)
        qc_cipher_pass_tag(c);
    if (!c->encrypt && c->mode == EVP_CIPH_CCM_MODE) {
        ok = !c->pending_auth_failed;
    } else {
        ok = EVP_CipherFinal_ex(c->ctx, out, &outl);
        if (ok && c->encrypt && c->aead) {
            if (c->auth_tag_len == -1)
                c->auth_tag_len = EVP_GCM_TLS_TAG_LEN;
            ok = EVP_CIPHER_CTX_ctrl(c->ctx, EVP_CTRL_AEAD_GET_TAG, c->auth_tag_len, c->auth_tag) >
                 0;
            if (ok)
                c->auth_tag_state = 1;
        }
    }
    EVP_CIPHER_CTX_free(c->ctx);
    c->ctx = NULL;
    if (!ok)
        return qc_throw_ossl(ctx, c->aead ? "Unsupported state or unable to authenticate data"
                                          : "Unsupported state");
    return qb_new_buffer(ctx, out, outl);
}

/* setAAD(aad, plaintextLength) */
static JSValue qc_cipher_set_aad(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QCCipher *c = qc_cipher_get(ctx, this_val);
    QCBuf aad;
    int plen, outl, ok;
    if (!c || qc_int(ctx, qc_arg(argc, argv, 1), -1, &plen))
        return JS_EXCEPTION;
    if (!c->aead)
        return JS_FALSE;
    if (qc_in(ctx, qc_arg(argc, argv, 0), &aad))
        return JS_EXCEPTION;
    if (c->mode == EVP_CIPH_CCM_MODE) {
        if (plen < 0) {
            qc_in_free(ctx, &aad);
            return qc_throw(ctx, JS_TYPE_ERROR, "ERR_MISSING_ARGS",
                            "options.plaintextLength required for CCM mode with AAD");
        }
        if ((int64_t)plen > c->max_message_size) {
            qc_in_free(ctx, &aad);
            return qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_MESSAGELEN",
                            "Invalid message length");
        }
        if (!c->encrypt && !qc_cipher_pass_tag(c)) {
            qc_in_free(ctx, &aad);
            ERR_clear_error();
            return JS_FALSE;
        }
        if (!EVP_CipherUpdate(c->ctx, NULL, &outl, NULL, plen)) {
            qc_in_free(ctx, &aad);
            ERR_clear_error();
            return JS_FALSE;
        }
    }
    ok = EVP_CipherUpdate(c->ctx, NULL, &outl, aad.p, (int)aad.len) == 1;
    qc_in_free(ctx, &aad);
    ERR_clear_error();
    return JS_NewBool(ctx, ok);
}

static JSValue qc_cipher_set_auth_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QCCipher *c = qc_cipher_get(ctx, this_val);
    QCBuf tag;
    int valid, n;
    char msg[96];
    if (!c)
        return JS_EXCEPTION;
    if (!c->aead || c->encrypt || c->auth_tag_state != 0)
        return JS_FALSE;
    if (qc_in(ctx, qc_arg(argc, argv, 0), &tag))
        return JS_EXCEPTION;
    n = tag.len > 1024 ? 1024 : (int)tag.len;
    if (c->mode == EVP_CIPH_GCM_MODE)
        valid = (c->auth_tag_len == -1 || c->auth_tag_len == n) && qc_valid_gcm_tag(n);
    else
        valid = c->auth_tag_len == n;
    if (!valid) {
        qc_in_free(ctx, &tag);
        snprintf(msg, sizeof(msg), "Invalid authentication tag length: %d", n);
        return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_AUTH_TAG", msg);
    }
    c->auth_tag_len = n;
    memcpy(c->auth_tag, tag.p, n);
    c->auth_tag_state = 1;
    qc_in_free(ctx, &tag);
    return JS_TRUE;
}

static JSValue qc_cipher_get_auth_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QCCipher *c = JS_GetOpaque2(ctx, this_val, qc_cipher_class_id);
    if (!c)
        return JS_EXCEPTION;
    if (c->ctx || !c->encrypt || c->auth_tag_state != 1)
        return JS_UNDEFINED;
    return qb_new_buffer(ctx, c->auth_tag, c->auth_tag_len);
}

static JSValue qc_cipher_set_auto_padding(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QCCipher *c = qc_cipher_get(ctx, this_val);
    int on;
    if (!c)
        return JS_EXCEPTION;
    on = JS_IsUndefined(qc_arg(argc, argv, 0)) ? 1 : JS_ToBool(ctx, argv[0]);
    if (on < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, EVP_CIPHER_CTX_set_padding(c->ctx, on) == 1);
}

static const JSCFunctionListEntry qc_cipher_proto_funcs[] = {
    JS_CFUNC_DEF("update", 1, qc_cipher_update),
    JS_CFUNC_DEF("final", 0, qc_cipher_final),
    JS_CFUNC_DEF("setAAD", 2, qc_cipher_set_aad),
    JS_CFUNC_DEF("setAuthTag", 1, qc_cipher_set_auth_tag),
    JS_CFUNC_DEF("getAuthTag", 0, qc_cipher_get_auth_tag),
    JS_CFUNC_DEF("setAutoPadding", 1, qc_cipher_set_auto_padding),
};

static const char *qc_mode_name(const EVP_CIPHER *cipher)
{
    switch (EVP_CIPHER_get_mode(cipher)) {
    case EVP_CIPH_CCM_MODE:
        return "ccm";
    case EVP_CIPH_CFB_MODE:
        return "cfb";
    case EVP_CIPH_CBC_MODE:
        return "cbc";
    case EVP_CIPH_CTR_MODE:
        return "ctr";
    case EVP_CIPH_ECB_MODE:
        return "ecb";
    case EVP_CIPH_GCM_MODE:
        return "gcm";
    case EVP_CIPH_OCB_MODE:
        return "ocb";
    case EVP_CIPH_OFB_MODE:
        return "ofb";
    case EVP_CIPH_STREAM_CIPHER:
        return "stream";
    case EVP_CIPH_WRAP_MODE:
        return "wrap";
    case EVP_CIPH_XTS_MODE:
        return "xts";
#ifdef EVP_CIPH_SIV_MODE
    case EVP_CIPH_SIV_MODE:
        return "siv";
#endif
    default:
        return NULL;
    }
}

/* cipherInfo(nameOrNid, keyLength, ivLength) */
static JSValue qc_cipher_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValueConst id = qc_arg(argc, argv, 0);
    EVP_CIPHER *cipher = NULL;
    int keylen, ivlen, mode, nid;
    JSValue obj = JS_UNDEFINED;
    const char *mname, *sn;
    char lower[80];

    if (qc_int(ctx, qc_arg(argc, argv, 1), -1, &keylen) ||
        qc_int(ctx, qc_arg(argc, argv, 2), -1, &ivlen))
        return JS_EXCEPTION;
    if (JS_IsNumber(id)) {
        int n;
        if (JS_ToInt32(ctx, &n, id))
            return JS_EXCEPTION;
        sn = OBJ_nid2sn(n);
        if (sn)
            cipher = qc_fetch_cipher(sn);
    } else {
        const char *name = JS_ToCString(ctx, id);
        if (!name)
            return JS_EXCEPTION;
        cipher = qc_fetch_cipher(name);
        JS_FreeCString(ctx, name);
    }
    if (!cipher)
        return JS_UNDEFINED;
    mode = EVP_CIPHER_get_mode(cipher);
    if (keylen != -1 && keylen != EVP_CIPHER_get_key_length(cipher)) {
        EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
        int ok = cctx && EVP_CipherInit_ex2(cctx, cipher, NULL, NULL, 1, NULL) &&
                 EVP_CIPHER_CTX_set_key_length(cctx, keylen) > 0;
        EVP_CIPHER_CTX_free(cctx);
        ERR_clear_error();
        if (!ok)
            goto done;
    }
    if (ivlen != -1) {
        int ok;
        if (mode == EVP_CIPH_CCM_MODE)
            ok = ivlen >= 7 && ivlen <= 13;
        else if (mode == EVP_CIPH_GCM_MODE)
            ok = ivlen > 0;
        else if (mode == EVP_CIPH_OCB_MODE)
            ok = ivlen > 0 && ivlen <= 15;
        else
            ok = ivlen == EVP_CIPHER_get_iv_length(cipher);
        if (!ok)
            goto done;
    }
    nid = EVP_CIPHER_get_nid(cipher);
    sn = OBJ_nid2sn(nid);
    obj = JS_NewObject(ctx);
    qc_lower(lower, sn ? sn : EVP_CIPHER_get0_name(cipher), sizeof(lower));
    qb_set(ctx, obj, "name", JS_NewString(ctx, lower));
    qb_set(ctx, obj, "nid", JS_NewInt32(ctx, nid));
    if (mode != EVP_CIPH_STREAM_CIPHER)
        qb_set(ctx, obj, "blockSize", JS_NewInt32(ctx, EVP_CIPHER_get_block_size(cipher)));
    if (EVP_CIPHER_get_iv_length(cipher) != 0)
        qb_set(ctx, obj, "ivLength",
               JS_NewInt32(ctx, ivlen != -1 ? ivlen : EVP_CIPHER_get_iv_length(cipher)));
    qb_set(ctx, obj, "keyLength",
           JS_NewInt32(ctx, keylen != -1 ? keylen : EVP_CIPHER_get_key_length(cipher)));
    mname = qc_mode_name(cipher);
    if (mname)
        qb_set(ctx, obj, "mode", JS_NewString(ctx, mname));
done:
    EVP_CIPHER_free(cipher);
    return obj;
}

/* --------------------------------------------------------------- KDFs */

typedef struct {
    QCJob j;
    uint8_t *a, *b, *c, *d; /* password/ikm, salt, secret/info, associated data */
    size_t al, bl, cl, dl;
    int has_c, has_d;
    char name[64]; /* digest / argon2 variant */
    uint64_t n1, n2, n3, n4;
    size_t keylen;
} QCKdfJob;

static void qc_kdf_fini(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    qc_clear_free(k->a, k->al);
    qc_clear_free(k->b, k->bl);
    qc_clear_free(k->c, k->cl);
    qc_clear_free(k->d, k->dl);
}

static int qc_kdf_alloc_out(QCKdfJob *k)
{
    k->j.out = malloc(k->keylen + 1);
    k->j.out_len = k->keylen;
    if (!k->j.out)
        return qc_job_fail(&k->j, JS_PLAIN_ERROR, NULL, "out of memory");
    return 0;
}

static int qc_pbkdf2_run(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    size_t force;
    EVP_MD *md;
    int ok;
    md = qc_fetch_md(k->name, &force);
    if (!md || force) {
        EVP_MD_free(md);
        return qc_job_fail(j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    }
    if (qc_kdf_alloc_out(k)) {
        EVP_MD_free(md);
        return -1;
    }
    ok = k->keylen == 0 || PKCS5_PBKDF2_HMAC((const char *)k->a, (int)k->al, k->b, (int)k->bl,
                                             (int)k->n1, md, (int)k->keylen, j->out);
    EVP_MD_free(md);
    return ok ? 0 : -1;
}

static int qc_double_arg(JSContext *ctx, JSValueConst v, double min, double max, double *pres)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= min && d <= max)) {
        JS_ThrowRangeError(ctx, "argument out of range");
        return -1;
    }
    *pres = d;
    return 0;
}

/* pbkdf2(password, salt, iterations, keylen, digest, cb) */
static JSValue qc_pbkdf2(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKdfJob *k;
    const char *digest;
    double iter;
    size_t keylen;

    if (qc_double_arg(ctx, qc_arg(argc, argv, 2), 1, INT_MAX, &iter) ||
        qc_size(ctx, qc_arg(argc, argv, 3), &keylen))
        return JS_EXCEPTION;
    k = qc_job_new(ctx, sizeof(*k), qc_pbkdf2_run, qc_done_buffer, qc_kdf_fini);
    if (!k)
        return JS_EXCEPTION;
    k->n1 = (uint64_t)iter;
    k->keylen = keylen;
    digest = JS_ToCString(ctx, qc_arg(argc, argv, 4));
    if (!digest)
        goto fail;
    snprintf(k->name, sizeof(k->name), "%s", digest);
    JS_FreeCString(ctx, digest);
    if (qc_copy(ctx, qc_arg(argc, argv, 0), &k->a, &k->al, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 1), &k->b, &k->bl, 0, NULL))
        goto fail;
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 5));
fail:
    qc_job_free(JS_GetRuntime(ctx), &k->j);
    return JS_EXCEPTION;
}

static int qc_scrypt_run(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    if (qc_kdf_alloc_out(k))
        return -1;
    if (k->keylen == 0)
        return 0;
    return EVP_PBE_scrypt((const char *)k->a, k->al, k->b, k->bl, k->n1, k->n2, k->n3, k->n4,
                          j->out, k->keylen) == 1
               ? 0
               : -1;
}

/* scrypt(password, salt, keylen, N, r, p, maxmem, cb) */
static JSValue qc_scrypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKdfJob *k;
    double N, r, p, maxmem;
    size_t keylen;

    if (qc_size(ctx, qc_arg(argc, argv, 2), &keylen) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 3), 0, 9007199254740991.0, &N) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 4), 0, 9007199254740991.0, &r) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 5), 0, 9007199254740991.0, &p) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 6), 0, 9007199254740991.0, &maxmem))
        return JS_EXCEPTION;
    ERR_clear_error();
    if (EVP_PBE_scrypt(NULL, 0, NULL, 0, (uint64_t)N, (uint64_t)r, (uint64_t)p, (uint64_t)maxmem,
                       NULL, 0) != 1) {
        unsigned long e = qc_take_error();
        char msg[320], reason[256];
        if (e) {
            ERR_error_string_n(e, reason, sizeof(reason));
            snprintf(msg, sizeof(msg), "Invalid scrypt params: %s", reason);
        } else {
            snprintf(msg, sizeof(msg), "Invalid scrypt params");
        }
        return qc_throw(ctx, JS_RANGE_ERROR, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", msg);
    }
    k = qc_job_new(ctx, sizeof(*k), qc_scrypt_run, qc_done_buffer, qc_kdf_fini);
    if (!k)
        return JS_EXCEPTION;
    k->keylen = keylen;
    k->n1 = (uint64_t)N;
    k->n2 = (uint64_t)r;
    k->n3 = (uint64_t)p;
    k->n4 = (uint64_t)maxmem;
    if (qc_copy(ctx, qc_arg(argc, argv, 0), &k->a, &k->al, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 1), &k->b, &k->bl, 0, NULL)) {
        qc_job_free(JS_GetRuntime(ctx), &k->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 7));
}

/* RFC 5869 HKDF on HMAC (also handles an empty key, unlike EVP_KDF) */
static int qc_hkdf_run(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    uint8_t zeros[EVP_MAX_MD_SIZE], prk[EVP_MAX_MD_SIZE], t[EVP_MAX_MD_SIZE], ctr;
    size_t hlen, n = 0, done = 0, force;
    EVP_MAC_CTX *ext = NULL, *base = NULL, *m = NULL;
    EVP_MD *md;
    int ok = 0;

    md = qc_fetch_md(k->name, &force);
    if (!md || force || qc_md_is_xof(md)) {
        EVP_MD_free(md);
        return qc_job_fail(j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    }
    hlen = EVP_MD_get_size(md);
    EVP_MD_free(md);
    if (k->keylen > 255 * hlen)
        return qc_job_fail(j, JS_RANGE_ERROR, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
    if (qc_kdf_alloc_out(k))
        return -1;
    memset(zeros, 0, sizeof(zeros));
    ext = qc_hmac_ctx(k->name, k->bl ? k->b : zeros, k->bl ? k->bl : hlen);
    if (!ext || !EVP_MAC_update(ext, k->a, k->al) || !EVP_MAC_final(ext, prk, &n, sizeof(prk)))
        goto done;
    base = qc_hmac_ctx(k->name, prk, n);
    if (!base)
        goto done;
    for (ctr = 1; done < k->keylen; ctr++) {
        size_t tl = 0, take;
        m = EVP_MAC_CTX_dup(base);
        if (!m || (ctr > 1 && !EVP_MAC_update(m, t, hlen)) || !EVP_MAC_update(m, k->c, k->cl) ||
            !EVP_MAC_update(m, &ctr, 1) || !EVP_MAC_final(m, t, &tl, sizeof(t)))
            goto done;
        EVP_MAC_CTX_free(m);
        m = NULL;
        take = k->keylen - done < hlen ? k->keylen - done : hlen;
        memcpy(j->out + done, t, take);
        done += take;
    }
    ok = 1;
done:
    OPENSSL_cleanse(prk, sizeof(prk));
    OPENSSL_cleanse(t, sizeof(t));
    EVP_MAC_CTX_free(ext);
    EVP_MAC_CTX_free(base);
    EVP_MAC_CTX_free(m);
    return ok ? 0 : -1;
}

/* hkdf(digest, ikm, salt, info, keylen, cb) */
static JSValue qc_hkdf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKdfJob *k;
    const char *digest;
    size_t keylen;

    if (qc_size(ctx, qc_arg(argc, argv, 4), &keylen))
        return JS_EXCEPTION;
    k = qc_job_new(ctx, sizeof(*k), qc_hkdf_run, qc_done_buffer, qc_kdf_fini);
    if (!k)
        return JS_EXCEPTION;
    k->keylen = keylen;
    digest = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!digest)
        goto fail;
    snprintf(k->name, sizeof(k->name), "%s", digest);
    JS_FreeCString(ctx, digest);
    if (qc_copy(ctx, qc_arg(argc, argv, 1), &k->a, &k->al, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 2), &k->b, &k->bl, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 3), &k->c, &k->cl, 0, NULL))
        goto fail;
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 5));
fail:
    qc_job_free(JS_GetRuntime(ctx), &k->j);
    return JS_EXCEPTION;
}

static int qc_argon2_run(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    EVP_KDF *kdf;
    EVP_KDF_CTX *kctx = NULL;
    OSSL_PARAM params[10];
    uint32_t iter = (uint32_t)k->n3, lanes = (uint32_t)k->n1, mem = (uint32_t)k->n2, threads = 1;
    int n = 0, ok;
    char upper[16];
    size_t i;

    for (i = 0; k->name[i] && i + 1 < sizeof(upper); i++)
        upper[i] = (k->name[i] >= 'a' && k->name[i] <= 'z') ? k->name[i] - 32 : k->name[i];
    upper[i] = 0;
    kdf = EVP_KDF_fetch(NULL, upper, NULL);
    if (!kdf)
        return qc_job_fail(j, JS_PLAIN_ERROR, "ERR_CRYPTO_UNSUPPORTED_OPERATION",
                           "Argon2 is not supported by this OpenSSL build");
    kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx || qc_kdf_alloc_out(k)) {
        EVP_KDF_CTX_free(kctx);
        return -1;
    }
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, k->a, k->al);
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, k->b, k->bl);
    if (k->has_c)
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, k->c, k->cl);
    if (k->has_d)
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD, k->d, k->dl);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iter);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &mem);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads);
    params[n] = OSSL_PARAM_construct_end();
    ok = EVP_KDF_derive(kctx, j->out, k->keylen, params) == 1;
    EVP_KDF_CTX_free(kctx);
    return ok ? 0 : -1;
}

/* argon2(variant, message, nonce, secret, ad, parallelism, memory, passes, tagLength, cb) */
static JSValue qc_argon2(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKdfJob *k;
    const char *variant;
    double lanes, mem, passes;
    size_t taglen;

    if (qc_double_arg(ctx, qc_arg(argc, argv, 5), 1, 16777215, &lanes) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 6), 1, 4294967295.0, &mem) ||
        qc_double_arg(ctx, qc_arg(argc, argv, 7), 1, 4294967295.0, &passes) ||
        qc_size(ctx, qc_arg(argc, argv, 8), &taglen))
        return JS_EXCEPTION;
    k = qc_job_new(ctx, sizeof(*k), qc_argon2_run, qc_done_buffer, qc_kdf_fini);
    if (!k)
        return JS_EXCEPTION;
    k->n1 = (uint64_t)lanes;
    k->n2 = (uint64_t)mem;
    k->n3 = (uint64_t)passes;
    k->keylen = taglen;
    variant = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!variant)
        goto fail;
    snprintf(k->name, sizeof(k->name), "%s", variant);
    JS_FreeCString(ctx, variant);
    if (qc_copy(ctx, qc_arg(argc, argv, 1), &k->a, &k->al, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 2), &k->b, &k->bl, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 3), &k->c, &k->cl, 1, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 4), &k->d, &k->dl, 1, NULL))
        goto fail;
    k->has_c = k->c != NULL;
    k->has_d = k->d != NULL;
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 9));
fail:
    qc_job_free(JS_GetRuntime(ctx), &k->j);
    return JS_EXCEPTION;
}

/* bcrypt (eksblowfish, as in Zig's std.crypto.bcrypt used by Bun) */
static const uint32_t bf_init_s[4][256] = {
    {0xd1310ba6, 0x98dfb5ac, 0x2ffd72db, 0xd01adfb7, 0xb8e1afed, 0x6a267e96, 0xba7c9045,
     0xf12c7f99, 0x24a19947, 0xb3916cf7, 0x0801f2e2, 0x858efc16, 0x636920d8, 0x71574e69,
     0xa458fea3, 0xf4933d7e, 0x0d95748f, 0x728eb658, 0x718bcd58, 0x82154aee, 0x7b54a41d,
     0xc25a59b5, 0x9c30d539, 0x2af26013, 0xc5d1b023, 0x286085f0, 0xca417918, 0xb8db38ef,
     0x8e79dcb0, 0x603a180e, 0x6c9e0e8b, 0xb01e8a3e, 0xd71577c1, 0xbd314b27, 0x78af2fda,
     0x55605c60, 0xe65525f3, 0xaa55ab94, 0x57489862, 0x63e81440, 0x55ca396a, 0x2aab10b6,
     0xb4cc5c34, 0x1141e8ce, 0xa15486af, 0x7c72e993, 0xb3ee1411, 0x636fbc2a, 0x2ba9c55d,
     0x741831f6, 0xce5c3e16, 0x9b87931e, 0xafd6ba33, 0x6c24cf5c, 0x7a325381, 0x28958677,
     0x3b8f4898, 0x6b4bb9af, 0xc4bfe81b, 0x66282193, 0x61d809cc, 0xfb21a991, 0x487cac60,
     0x5dec8032, 0xef845d5d, 0xe98575b1, 0xdc262302, 0xeb651b88, 0x23893e81, 0xd396acc5,
     0x0f6d6ff3, 0x83f44239, 0x2e0b4482, 0xa4842004, 0x69c8f04a, 0x9e1f9b5e, 0x21c66842,
     0xf6e96c9a, 0x670c9c61, 0xabd388f0, 0x6a51a0d2, 0xd8542f68, 0x960fa728, 0xab5133a3,
     0x6eef0b6c, 0x137a3be4, 0xba3bf050, 0x7efb2a98, 0xa1f1651d, 0x39af0176, 0x66ca593e,
     0x82430e88, 0x8cee8619, 0x456f9fb4, 0x7d84a5c3, 0x3b8b5ebe, 0xe06f75d8, 0x85c12073,
     0x401a449f, 0x56c16aa6, 0x4ed3aa62, 0x363f7706, 0x1bfedf72, 0x429b023d, 0x37d0d724,
     0xd00a1248, 0xdb0fead3, 0x49f1c09b, 0x075372c9, 0x80991b7b, 0x25d479d8, 0xf6e8def7,
     0xe3fe501a, 0xb6794c3b, 0x976ce0bd, 0x04c006ba, 0xc1a94fb6, 0x409f60c4, 0x5e5c9ec2,
     0x196a2463, 0x68fb6faf, 0x3e6c53b5, 0x1339b2eb, 0x3b52ec6f, 0x6dfc511f, 0x9b30952c,
     0xcc814544, 0xaf5ebd09, 0xbee3d004, 0xde334afd, 0x660f2807, 0x192e4bb3, 0xc0cba857,
     0x45c8740f, 0xd20b5f39, 0xb9d3fbdb, 0x5579c0bd, 0x1a60320a, 0xd6a100c6, 0x402c7279,
     0x679f25fe, 0xfb1fa3cc, 0x8ea5e9f8, 0xdb3222f8, 0x3c7516df, 0xfd616b15, 0x2f501ec8,
     0xad0552ab, 0x323db5fa, 0xfd238760, 0x53317b48, 0x3e00df82, 0x9e5c57bb, 0xca6f8ca0,
     0x1a87562e, 0xdf1769db, 0xd542a8f6, 0x287effc3, 0xac6732c6, 0x8c4f5573, 0x695b27b0,
     0xbbca58c8, 0xe1ffa35d, 0xb8f011a0, 0x10fa3d98, 0xfd2183b8, 0x4afcb56c, 0x2dd1d35b,
     0x9a53e479, 0xb6f84565, 0xd28e49bc, 0x4bfb9790, 0xe1ddf2da, 0xa4cb7e33, 0x62fb1341,
     0xcee4c6e8, 0xef20cada, 0x36774c01, 0xd07e9efe, 0x2bf11fb4, 0x95dbda4d, 0xae909198,
     0xeaad8e71, 0x6b93d5a0, 0xd08ed1d0, 0xafc725e0, 0x8e3c5b2f, 0x8e7594b7, 0x8ff6e2fb,
     0xf2122b64, 0x8888b812, 0x900df01c, 0x4fad5ea0, 0x688fc31c, 0xd1cff191, 0xb3a8c1ad,
     0x2f2f2218, 0xbe0e1777, 0xea752dfe, 0x8b021fa1, 0xe5a0cc0f, 0xb56f74e8, 0x18acf3d6,
     0xce89e299, 0xb4a84fe0, 0xfd13e0b7, 0x7cc43b81, 0xd2ada8d9, 0x165fa266, 0x80957705,
     0x93cc7314, 0x211a1477, 0xe6ad2065, 0x77b5fa86, 0xc75442f5, 0xfb9d35cf, 0xebcdaf0c,
     0x7b3e89a0, 0xd6411bd3, 0xae1e7e49, 0x00250e2d, 0x2071b35e, 0x226800bb, 0x57b8e0af,
     0x2464369b, 0xf009b91e, 0x5563911d, 0x59dfa6aa, 0x78c14389, 0xd95a537f, 0x207d5ba2,
     0x02e5b9c5, 0x83260376, 0x6295cfa9, 0x11c81968, 0x4e734a41, 0xb3472dca, 0x7b14a94a,
     0x1b510052, 0x9a532915, 0xd60f573f, 0xbc9bc6e4, 0x2b60a476, 0x81e67400, 0x08ba6fb5,
     0x571be91f, 0xf296ec6b, 0x2a0dd915, 0xb6636521, 0xe7b9f9b6, 0xff34052e, 0xc5855664,
     0x53b02d5d, 0xa99f8fa1, 0x08ba4799, 0x6e85076a},
    {0x4b7a70e9, 0xb5b32944, 0xdb75092e, 0xc4192623, 0xad6ea6b0, 0x49a7df7d, 0x9cee60b8,
     0x8fedb266, 0xecaa8c71, 0x699a17ff, 0x5664526c, 0xc2b19ee1, 0x193602a5, 0x75094c29,
     0xa0591340, 0xe4183a3e, 0x3f54989a, 0x5b429d65, 0x6b8fe4d6, 0x99f73fd6, 0xa1d29c07,
     0xefe830f5, 0x4d2d38e6, 0xf0255dc1, 0x4cdd2086, 0x8470eb26, 0x6382e9c6, 0x021ecc5e,
     0x09686b3f, 0x3ebaefc9, 0x3c971814, 0x6b6a70a1, 0x687f3584, 0x52a0e286, 0xb79c5305,
     0xaa500737, 0x3e07841c, 0x7fdeae5c, 0x8e7d44ec, 0x5716f2b8, 0xb03ada37, 0xf0500c0d,
     0xf01c1f04, 0x0200b3ff, 0xae0cf51a, 0x3cb574b2, 0x25837a58, 0xdc0921bd, 0xd19113f9,
     0x7ca92ff6, 0x94324773, 0x22f54701, 0x3ae5e581, 0x37c2dadc, 0xc8b57634, 0x9af3dda7,
     0xa9446146, 0x0fd0030e, 0xecc8c73e, 0xa4751e41, 0xe238cd99, 0x3bea0e2f, 0x3280bba1,
     0x183eb331, 0x4e548b38, 0x4f6db908, 0x6f420d03, 0xf60a04bf, 0x2cb81290, 0x24977c79,
     0x5679b072, 0xbcaf89af, 0xde9a771f, 0xd9930810, 0xb38bae12, 0xdccf3f2e, 0x5512721f,
     0x2e6b7124, 0x501adde6, 0x9f84cd87, 0x7a584718, 0x7408da17, 0xbc9f9abc, 0xe94b7d8c,
     0xec7aec3a, 0xdb851dfa, 0x63094366, 0xc464c3d2, 0xef1c1847, 0x3215d908, 0xdd433b37,
     0x24c2ba16, 0x12a14d43, 0x2a65c451, 0x50940002, 0x133ae4dd, 0x71dff89e, 0x10314e55,
     0x81ac77d6, 0x5f11199b, 0x043556f1, 0xd7a3c76b, 0x3c11183b, 0x5924a509, 0xf28fe6ed,
     0x97f1fbfa, 0x9ebabf2c, 0x1e153c6e, 0x86e34570, 0xeae96fb1, 0x860e5e0a, 0x5a3e2ab3,
     0x771fe71c, 0x4e3d06fa, 0x2965dcb9, 0x99e71d0f, 0x803e89d6, 0x5266c825, 0x2e4cc978,
     0x9c10b36a, 0xc6150eba, 0x94e2ea78, 0xa5fc3c53, 0x1e0a2df4, 0xf2f74ea7, 0x361d2b3d,
     0x1939260f, 0x19c27960, 0x5223a708, 0xf71312b6, 0xebadfe6e, 0xeac31f66, 0xe3bc4595,
     0xa67bc883, 0xb17f37d1, 0x018cff28, 0xc332ddef, 0xbe6c5aa5, 0x65582185, 0x68ab9802,
     0xeecea50f, 0xdb2f953b, 0x2aef7dad, 0x5b6e2f84, 0x1521b628, 0x29076170, 0xecdd4775,
     0x619f1510, 0x13cca830, 0xeb61bd96, 0x0334fe1e, 0xaa0363cf, 0xb5735c90, 0x4c70a239,
     0xd59e9e0b, 0xcbaade14, 0xeecc86bc, 0x60622ca7, 0x9cab5cab, 0xb2f3846e, 0x648b1eaf,
     0x19bdf0ca, 0xa02369b9, 0x655abb50, 0x40685a32, 0x3c2ab4b3, 0x319ee9d5, 0xc021b8f7,
     0x9b540b19, 0x875fa099, 0x95f7997e, 0x623d7da8, 0xf837889a, 0x97e32d77, 0x11ed935f,
     0x16681281, 0x0e358829, 0xc7e61fd6, 0x96dedfa1, 0x7858ba99, 0x57f584a5, 0x1b227263,
     0x9b83c3ff, 0x1ac24696, 0xcdb30aeb, 0x532e3054, 0x8fd948e4, 0x6dbc3128, 0x58ebf2ef,
     0x34c6ffea, 0xfe28ed61, 0xee7c3c73, 0x5d4a14d9, 0xe864b7e3, 0x42105d14, 0x203e13e0,
     0x45eee2b6, 0xa3aaabea, 0xdb6c4f15, 0xfacb4fd0, 0xc742f442, 0xef6abbb5, 0x654f3b1d,
     0x41cd2105, 0xd81e799e, 0x86854dc7, 0xe44b476a, 0x3d816250, 0xcf62a1f2, 0x5b8d2646,
     0xfc8883a0, 0xc1c7b6a3, 0x7f1524c3, 0x69cb7492, 0x47848a0b, 0x5692b285, 0x095bbf00,
     0xad19489d, 0x1462b174, 0x23820e00, 0x58428d2a, 0x0c55f5ea, 0x1dadf43e, 0x233f7061,
     0x3372f092, 0x8d937e41, 0xd65fecf1, 0x6c223bdb, 0x7cde3759, 0xcbee7460, 0x4085f2a7,
     0xce77326e, 0xa6078084, 0x19f8509e, 0xe8efd855, 0x61d99735, 0xa969a7aa, 0xc50c06c2,
     0x5a04abfc, 0x800bcadc, 0x9e447a2e, 0xc3453484, 0xfdd56705, 0x0e1e9ec9, 0xdb73dbd3,
     0x105588cd, 0x675fda79, 0xe3674340, 0xc5c43465, 0x713e38d8, 0x3d28f89e, 0xf16dff20,
     0x153e21e7, 0x8fb03d4a, 0xe6e39f2b, 0xdb83adf7},
    {0xe93d5a68, 0x948140f7, 0xf64c261c, 0x94692934, 0x411520f7, 0x7602d4f7, 0xbcf46b2e,
     0xd4a20068, 0xd4082471, 0x3320f46a, 0x43b7d4b7, 0x500061af, 0x1e39f62e, 0x97244546,
     0x14214f74, 0xbf8b8840, 0x4d95fc1d, 0x96b591af, 0x70f4ddd3, 0x66a02f45, 0xbfbc09ec,
     0x03bd9785, 0x7fac6dd0, 0x31cb8504, 0x96eb27b3, 0x55fd3941, 0xda2547e6, 0xabca0a9a,
     0x28507825, 0x530429f4, 0x0a2c86da, 0xe9b66dfb, 0x68dc1462, 0xd7486900, 0x680ec0a4,
     0x27a18dee, 0x4f3ffea2, 0xe887ad8c, 0xb58ce006, 0x7af4d6b6, 0xaace1e7c, 0xd3375fec,
     0xce78a399, 0x406b2a42, 0x20fe9e35, 0xd9f385b9, 0xee39d7ab, 0x3b124e8b, 0x1dc9faf7,
     0x4b6d1856, 0x26a36631, 0xeae397b2, 0x3a6efa74, 0xdd5b4332, 0x6841e7f7, 0xca7820fb,
     0xfb0af54e, 0xd8feb397, 0x454056ac, 0xba489527, 0x55533a3a, 0x20838d87, 0xfe6ba9b7,
     0xd096954b, 0x55a867bc, 0xa1159a58, 0xcca92963, 0x99e1db33, 0xa62a4a56, 0x3f3125f9,
     0x5ef47e1c, 0x9029317c, 0xfdf8e802, 0x04272f70, 0x80bb155c, 0x05282ce3, 0x95c11548,
     0xe4c66d22, 0x48c1133f, 0xc70f86dc, 0x07f9c9ee, 0x41041f0f, 0x404779a4, 0x5d886e17,
     0x325f51eb, 0xd59bc0d1, 0xf2bcc18f, 0x41113564, 0x257b7834, 0x602a9c60, 0xdff8e8a3,
     0x1f636c1b, 0x0e12b4c2, 0x02e1329e, 0xaf664fd1, 0xcad18115, 0x6b2395e0, 0x333e92e1,
     0x3b240b62, 0xeebeb922, 0x85b2a20e, 0xe6ba0d99, 0xde720c8c, 0x2da2f728, 0xd0127845,
     0x95b794fd, 0x647d0862, 0xe7ccf5f0, 0x5449a36f, 0x877d48fa, 0xc39dfd27, 0xf33e8d1e,
     0x0a476341, 0x992eff74, 0x3a6f6eab, 0xf4f8fd37, 0xa812dc60, 0xa1ebddf8, 0x991be14c,
     0xdb6e6b0d, 0xc67b5510, 0x6d672c37, 0x2765d43b, 0xdcd0e804, 0xf1290dc7, 0xcc00ffa3,
     0xb5390f92, 0x690fed0b, 0x667b9ffb, 0xcedb7d9c, 0xa091cf0b, 0xd9155ea3, 0xbb132f88,
     0x515bad24, 0x7b9479bf, 0x763bd6eb, 0x37392eb3, 0xcc115979, 0x8026e297, 0xf42e312d,
     0x6842ada7, 0xc66a2b3b, 0x12754ccc, 0x782ef11c, 0x6a124237, 0xb79251e7, 0x06a1bbe6,
     0x4bfb6350, 0x1a6b1018, 0x11caedfa, 0x3d25bdd8, 0xe2e1c3c9, 0x44421659, 0x0a121386,
     0xd90cec6e, 0xd5abea2a, 0x64af674e, 0xda86a85f, 0xbebfe988, 0x64e4c3fe, 0x9dbc8057,
     0xf0f7c086, 0x60787bf8, 0x6003604d, 0xd1fd8346, 0xf6381fb0, 0x7745ae04, 0xd736fccc,
     0x83426b33, 0xf01eab71, 0xb0804187, 0x3c005e5f, 0x77a057be, 0xbde8ae24, 0x55464299,
     0xbf582e61, 0x4e58f48f, 0xf2ddfda2, 0xf474ef38, 0x8789bdc2, 0x5366f9c3, 0xc8b38e74,
     0xb475f255, 0x46fcd9b9, 0x7aeb2661, 0x8b1ddf84, 0x846a0e79, 0x915f95e2, 0x466e598e,
     0x20b45770, 0x8cd55591, 0xc902de4c, 0xb90bace1, 0xbb8205d0, 0x11a86248, 0x7574a99e,
     0xb77f19b6, 0xe0a9dc09, 0x662d09a1, 0xc4324633, 0xe85a1f02, 0x09f0be8c, 0x4a99a025,
     0x1d6efe10, 0x1ab93d1d, 0x0ba5a4df, 0xa186f20f, 0x2868f169, 0xdcb7da83, 0x573906fe,
     0xa1e2ce9b, 0x4fcd7f52, 0x50115e01, 0xa70683fa, 0xa002b5c4, 0x0de6d027, 0x9af88c27,
     0x773f8641, 0xc3604c06, 0x61a806b5, 0xf0177a28, 0xc0f586e0, 0x006058aa, 0x30dc7d62,
     0x11e69ed7, 0x2338ea63, 0x53c2dd94, 0xc2c21634, 0xbbcbee56, 0x90bcb6de, 0xebfc7da1,
     0xce591d76, 0x6f05e409, 0x4b7c0188, 0x39720a3d, 0x7c927c24, 0x86e3725f, 0x724d9db9,
     0x1ac15bb4, 0xd39eb8fc, 0xed545578, 0x08fca5b5, 0xd83d7cd3, 0x4dad0fc4, 0x1e50ef5e,
     0xb161e6f8, 0xa28514d9, 0x6c51133c, 0x6fd5c7e7, 0x56e14ec4, 0x362abfce, 0xddc6c837,
     0xd79a3234, 0x92638212, 0x670efa8e, 0x406000e0},
    {0x3a39ce37, 0xd3faf5cf, 0xabc27737, 0x5ac52d1b, 0x5cb0679e, 0x4fa33742, 0xd3822740,
     0x99bc9bbe, 0xd5118e9d, 0xbf0f7315, 0xd62d1c7e, 0xc700c47b, 0xb78c1b6b, 0x21a19045,
     0xb26eb1be, 0x6a366eb4, 0x5748ab2f, 0xbc946e79, 0xc6a376d2, 0x6549c2c8, 0x530ff8ee,
     0x468dde7d, 0xd5730a1d, 0x4cd04dc6, 0x2939bbdb, 0xa9ba4650, 0xac9526e8, 0xbe5ee304,
     0xa1fad5f0, 0x6a2d519a, 0x63ef8ce2, 0x9a86ee22, 0xc089c2b8, 0x43242ef6, 0xa51e03aa,
     0x9cf2d0a4, 0x83c061ba, 0x9be96a4d, 0x8fe51550, 0xba645bd6, 0x2826a2f9, 0xa73a3ae1,
     0x4ba99586, 0xef5562e9, 0xc72fefd3, 0xf752f7da, 0x3f046f69, 0x77fa0a59, 0x80e4a915,
     0x87b08601, 0x9b09e6ad, 0x3b3ee593, 0xe990fd5a, 0x9e34d797, 0x2cf0b7d9, 0x022b8b51,
     0x96d5ac3a, 0x017da67d, 0xd1cf3ed6, 0x7c7d2d28, 0x1f9f25cf, 0xadf2b89b, 0x5ad6b472,
     0x5a88f54c, 0xe029ac71, 0xe019a5e6, 0x47b0acfd, 0xed93fa9b, 0xe8d3c48d, 0x283b57cc,
     0xf8d56629, 0x79132e28, 0x785f0191, 0xed756055, 0xf7960e44, 0xe3d35e8c, 0x15056dd4,
     0x88f46dba, 0x03a16125, 0x0564f0bd, 0xc3eb9e15, 0x3c9057a2, 0x97271aec, 0xa93a072a,
     0x1b3f6d9b, 0x1e6321f5, 0xf59c66fb, 0x26dcf319, 0x7533d928, 0xb155fdf5, 0x03563482,
     0x8aba3cbb, 0x28517711, 0xc20ad9f8, 0xabcc5167, 0xccad925f, 0x4de81751, 0x3830dc8e,
     0x379d5862, 0x9320f991, 0xea7a90c2, 0xfb3e7bce, 0x5121ce64, 0x774fbe32, 0xa8b6e37e,
     0xc3293d46, 0x48de5369, 0x6413e680, 0xa2ae0810, 0xdd6db224, 0x69852dfd, 0x09072166,
     0xb39a460a, 0x6445c0dd, 0x586cdecf, 0x1c20c8ae, 0x5bbef7dd, 0x1b588d40, 0xccd2017f,
     0x6bb4e3bb, 0xdda26a7e, 0x3a59ff45, 0x3e350a44, 0xbcb4cdd5, 0x72eacea8, 0xfa6484bb,
     0x8d6612ae, 0xbf3c6f47, 0xd29be463, 0x542f5d9e, 0xaec2771b, 0xf64e6370, 0x740e0d8d,
     0xe75b1357, 0xf8721671, 0xaf537d5d, 0x4040cb08, 0x4eb4e2cc, 0x34d2466a, 0x0115af84,
     0xe1b00428, 0x95983a1d, 0x06b89fb4, 0xce6ea048, 0x6f3f3b82, 0x3520ab82, 0x011a1d4b,
     0x277227f8, 0x611560b1, 0xe7933fdc, 0xbb3a792b, 0x344525bd, 0xa08839e1, 0x51ce794b,
     0x2f32c9b7, 0xa01fbac9, 0xe01cc87e, 0xbcc7d1f6, 0xcf0111c3, 0xa1e8aac7, 0x1a908749,
     0xd44fbd9a, 0xd0dadecb, 0xd50ada38, 0x0339c32a, 0xc6913667, 0x8df9317c, 0xe0b12b4f,
     0xf79e59b7, 0x43f5bb3a, 0xf2d519ff, 0x27d9459c, 0xbf97222c, 0x15e6fc2a, 0x0f91fc71,
     0x9b941525, 0xfae59361, 0xceb69ceb, 0xc2a86459, 0x12baa8d1, 0xb6c1075e, 0xe3056a0c,
     0x10d25065, 0xcb03a442, 0xe0ec6e0e, 0x1698db3b, 0x4c98a0be, 0x3278e964, 0x9f1f9532,
     0xe0d392df, 0xd3a0342b, 0x8971f21e, 0x1b0a7441, 0x4ba3348c, 0xc5be7120, 0xc37632d8,
     0xdf359f8d, 0x9b992f2e, 0xe60b6f47, 0x0fe3f11d, 0xe54cda54, 0x1edad891, 0xce6279cf,
     0xcd3e7e6f, 0x1618b166, 0xfd2c1d05, 0x848fd2c5, 0xf6fb2299, 0xf523f357, 0xa6327623,
     0x93a83531, 0x56cccd02, 0xacf08162, 0x5a75ebb5, 0x6e163697, 0x88d273cc, 0xde966292,
     0x81b949d0, 0x4c50901b, 0x71c65614, 0xe6c6c7bd, 0x327a140a, 0x45e1d006, 0xc3f27b9a,
     0xc9aa53fd, 0x62a80f00, 0xbb25bfe2, 0x35bdd2f6, 0x71126905, 0xb2040222, 0xb6cbcf7c,
     0xcd769c2b, 0x53113ec0, 0x1640e3d3, 0x38abbd60, 0x2547adf0, 0xba38209c, 0xf746ce76,
     0x77afa1c5, 0x20756060, 0x85cbfe4e, 0x8ae88dd8, 0x7aaaf9b0, 0x4cf9aa7e, 0x1948c25c,
     0x02fb8a8c, 0x01c36ae4, 0xd6ebe1f9, 0x90d4f869, 0xa65cdea0, 0x3f09252d, 0xc208e69f,
     0xb74e6132, 0xce77e25b, 0x578fdfe3, 0x3ac372e6},
};
static const uint32_t bf_init_p[18] = {0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344, 0xa4093822,
                                       0x299f31d0, 0x082efa98, 0xec4e6c89, 0x452821e6, 0x38d01377,
                                       0xbe5466cf, 0x34e90c6c, 0xc0ac29b7, 0xc97c50dd, 0x3f84d5b5,
                                       0xb5470917, 0x9216d5d9, 0x8979fb1b};

typedef struct {
    uint32_t S[4][256];
    uint32_t P[18];
} QCBlowfish;

static uint32_t qc_bf_word(const uint8_t *data, size_t len, size_t *j)
{
    uint32_t t = 0;
    int i;
    for (i = 0; i < 4; i++) {
        if (*j >= len)
            *j = 0;
        t = (t << 8) | data[*j];
        (*j)++;
    }
    return t;
}

static inline uint32_t qc_bf_f(const QCBlowfish *s, uint32_t x)
{
    uint32_t r = s->S[0][x >> 24];
    r += s->S[1][(x >> 16) & 0xff];
    r ^= s->S[2][(x >> 8) & 0xff];
    r += s->S[3][x & 0xff];
    return r;
}

static void qc_bf_encipher(const QCBlowfish *s, uint32_t *pl, uint32_t *pr)
{
    uint32_t l = *pl, r = *pr;
    int i;
    l ^= s->P[0];
    for (i = 1; i < 16; i += 2) {
        r ^= qc_bf_f(s, l) ^ s->P[i];
        l ^= qc_bf_f(s, r) ^ s->P[i + 1];
    }
    *pl = r ^ s->P[17];
    *pr = l;
}

/* key schedule; data == NULL: expand0 */
static void qc_bf_expand(QCBlowfish *s, const uint8_t *data, size_t dlen, const uint8_t *key,
                         size_t klen)
{
    size_t j = 0, i, k;
    uint32_t l = 0, r = 0;
    for (i = 0; i < 18; i++)
        s->P[i] ^= qc_bf_word(key, klen, &j);
    j = 0;
    for (i = 0; i < 18; i += 2) {
        if (data) {
            l ^= qc_bf_word(data, dlen, &j);
            r ^= qc_bf_word(data, dlen, &j);
        }
        qc_bf_encipher(s, &l, &r);
        s->P[i] = l;
        s->P[i + 1] = r;
    }
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 256; k += 2) {
            if (data) {
                l ^= qc_bf_word(data, dlen, &j);
                r ^= qc_bf_word(data, dlen, &j);
            }
            qc_bf_encipher(s, &l, &r);
            s->S[i][k] = l;
            s->S[i][k + 1] = r;
        }
    }
}

/* 23 byte bcrypt hash of 'password' (truncated to 72 bytes) */
static int qc_bcrypt_hash(const uint8_t *password, size_t plen, const uint8_t salt[16], int cost,
                          uint8_t out[23])
{
    QCBlowfish *s;
    uint8_t pw[73];
    uint32_t cdata[6] = {0x4f727068, 0x65616e42, 0x65686f6c, 0x64657253, 0x63727944, 0x6f756274};
    uint64_t rounds, k;
    size_t n = plen > 72 ? 72 : plen;
    int i;

    s = malloc(sizeof(*s));
    if (!s)
        return -1;
    memcpy(s->S, bf_init_s, sizeof(s->S));
    memcpy(s->P, bf_init_p, sizeof(s->P));
    memcpy(pw, password, n);
    pw[n] = 0;
    qc_bf_expand(s, salt, 16, pw, n + 1);
    rounds = (uint64_t)1 << cost;
    for (k = 0; k < rounds; k++) {
        qc_bf_expand(s, NULL, 0, pw, n + 1);
        qc_bf_expand(s, NULL, 0, salt, 16);
    }
    for (k = 0; k < 64; k++)
        for (i = 0; i < 6; i += 2)
            qc_bf_encipher(s, &cdata[i], &cdata[i + 1]);
    for (i = 0; i < 6; i++) {
        uint8_t b[4] = {(uint8_t)(cdata[i] >> 24), (uint8_t)(cdata[i] >> 16),
                        (uint8_t)(cdata[i] >> 8), (uint8_t)cdata[i]};
        memcpy(out + 4 * i, b, i == 5 ? 3 : 4);
    }
    OPENSSL_cleanse(pw, sizeof(pw));
    OPENSSL_cleanse(s, sizeof(*s));
    free(s);
    return 0;
}

static int qc_bcrypt_run(QCJob *j)
{
    QCKdfJob *k = (QCKdfJob *)j;
    k->keylen = 23;
    if (qc_kdf_alloc_out(k))
        return -1;
    if (qc_bcrypt_hash(k->a, k->al, k->b, (int)k->n1, j->out))
        return qc_job_fail(j, JS_PLAIN_ERROR, NULL, "out of memory");
    return 0;
}

/* bcrypt(password, salt16, cost, cb) */
static JSValue qc_bcrypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKdfJob *k;
    double cost;
    if (qc_double_arg(ctx, qc_arg(argc, argv, 2), 4, 31, &cost))
        return JS_EXCEPTION;
    k = qc_job_new(ctx, sizeof(*k), qc_bcrypt_run, qc_done_buffer, qc_kdf_fini);
    if (!k)
        return JS_EXCEPTION;
    k->n1 = (uint64_t)cost;
    if (qc_copy(ctx, qc_arg(argc, argv, 0), &k->a, &k->al, 0, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 1), &k->b, &k->bl, 0, NULL))
        goto fail;
    if (k->bl != 16) {
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE", "bcrypt salt must be 16 bytes");
        goto fail;
    }
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 3));
fail:
    qc_job_free(JS_GetRuntime(ctx), &k->j);
    return JS_EXCEPTION;
}

/* ------------------------------------------------------ asymmetric keys */

static JSClassID qc_key_class_id;

typedef struct {
    EVP_PKEY *pkey;
    int priv;
} QCKey;

static void qc_key_finalizer(JSRuntime *rt, JSValue val)
{
    QCKey *k = JS_GetOpaque(val, qc_key_class_id);
    if (k) {
        EVP_PKEY_free(k->pkey);
        js_free_rt(rt, k);
    }
}

static JSClassDef qc_key_class = {"PKey", .finalizer = qc_key_finalizer};

/* wrap 'pkey' (taken) in a PKey object */
static JSValue qc_key_wrap(JSContext *ctx, EVP_PKEY *pkey, int priv)
{
    QCKey *k;
    JSValue obj = JS_NewObjectClass(ctx, qc_key_class_id);
    if (JS_IsException(obj)) {
        EVP_PKEY_free(pkey);
        return obj;
    }
    k = js_mallocz(ctx, sizeof(*k));
    if (!k) {
        EVP_PKEY_free(pkey);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    k->pkey = pkey;
    k->priv = priv;
    JS_SetOpaque(obj, k);
    return obj;
}

static QCKey *qc_key_get(JSContext *ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, qc_key_class_id);
}

static int qc_pkey_has_private(EVP_PKEY *pkey)
{
    OSSL_PARAM *params = NULL;
    int r = 0;
    if (EVP_PKEY_todata(pkey, EVP_PKEY_KEYPAIR, &params) == 1 && params) {
        r = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PRIV_KEY) != NULL ||
            OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_RSA_D) != NULL;
        OSSL_PARAM_free(params);
    } else {
        size_t len = 0;
        r = EVP_PKEY_get_raw_private_key(pkey, NULL, &len) == 1 && len > 0;
    }
    ERR_clear_error();
    return r;
}

/* a public-only copy of 'pkey' (or a new reference if that fails) */
static EVP_PKEY *qc_pkey_public(EVP_PKEY *pkey)
{
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    EVP_PKEY *pub = NULL;

    if (EVP_PKEY_todata(pkey, EVP_PKEY_PUBLIC_KEY, &params) == 1 &&
        (pctx = EVP_PKEY_CTX_new_from_name(NULL, EVP_PKEY_get0_type_name(pkey), NULL)) &&
        EVP_PKEY_fromdata_init(pctx) == 1 &&
        EVP_PKEY_fromdata(pctx, &pub, EVP_PKEY_PUBLIC_KEY, params) == 1) {
        /* done */
    } else {
        EVP_PKEY_free(pub);
        pub = NULL;
    }
    OSSL_PARAM_free(params);
    EVP_PKEY_CTX_free(pctx);
    ERR_clear_error();
    if (!pub && EVP_PKEY_up_ref(pkey) == 1)
        pub = pkey;
    return pub;
}

static const char *qc_find(const uint8_t *hay, size_t hlen, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n > hlen)
        return NULL;
    for (i = 0; i + n <= hlen; i++)
        if (hay[i] == (uint8_t)needle[0] && !memcmp(hay + i, needle, n))
            return (const char *)hay + i;
    return NULL;
}

static int qc_pass_cb(char *pass, size_t pass_size, size_t *pass_len, const OSSL_PARAM params[],
                      void *arg)
{
    *(int *)arg = 1;
    return 0;
}

static X509 *qc_x509_from_pem(const uint8_t *p, size_t len)
{
    BIO *bio = BIO_new_mem_buf(p, (int)len);
    X509 *x;
    if (!bio)
        return NULL;
    x = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return x;
}

/* keyParse(data, format, kind, type, passphrase) */
static JSValue qc_key_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCBuf data = {0}, pass = {0};
    const char *format = NULL, *kind = NULL, *type = NULL;
    int has_pass, is_pem, want_priv, want_pub, attempt, needed = 0;
    int selections[2], nsel = 0;
    const char *in_struct = NULL, *keytype = NULL;
    EVP_PKEY *pkey = NULL;
    JSValue ret = JS_EXCEPTION;

    if (qc_in(ctx, qc_arg(argc, argv, 0), &data))
        return JS_EXCEPTION;
    format = JS_ToCString(ctx, qc_arg(argc, argv, 1));
    kind = JS_ToCString(ctx, qc_arg(argc, argv, 2));
    if (!format || !kind)
        goto done;
    if (!JS_IsNull(qc_arg(argc, argv, 3)) && !JS_IsUndefined(qc_arg(argc, argv, 3)) &&
        !(type = JS_ToCString(ctx, argv[3])))
        goto done;
    has_pass = !JS_IsNull(qc_arg(argc, argv, 4)) && !JS_IsUndefined(qc_arg(argc, argv, 4));
    if (has_pass && qc_in(ctx, argv[4], &pass))
        goto done;
    is_pem = !strcmp(format, "pem");
    want_priv = !strcmp(kind, "private");
    want_pub = !strcmp(kind, "public");

    if (!want_priv && is_pem &&
        (qc_find(data.p, data.len, "-----BEGIN CERTIFICATE-----") ||
         qc_find(data.p, data.len, "-----BEGIN TRUSTED CERTIFICATE-----") ||
         qc_find(data.p, data.len, "-----BEGIN X509 CERTIFICATE-----"))) {
        X509 *x = qc_x509_from_pem(data.p, data.len);
        if (x) {
            pkey = X509_get0_pubkey(x);
            if (pkey)
                EVP_PKEY_up_ref(pkey);
            X509_free(x);
            if (pkey) {
                ret = qc_key_wrap(ctx, pkey, 0);
                goto done;
            }
        }
        ERR_clear_error();
    }
    if (!is_pem && type) {
        if (!strcmp(type, "spki")) {
            in_struct = "SubjectPublicKeyInfo";
        } else if (!strcmp(type, "pkcs1")) {
            in_struct = "type-specific";
            keytype = "RSA";
        } else if (!strcmp(type, "sec1")) {
            in_struct = "type-specific";
            keytype = "EC";
        }
    }
    if (want_pub) {
        selections[nsel++] = EVP_PKEY_PUBLIC_KEY;
        selections[nsel++] = EVP_PKEY_KEYPAIR;
    } else if (want_priv) {
        selections[nsel++] = EVP_PKEY_KEYPAIR;
    } else {
        selections[nsel++] = EVP_PKEY_KEYPAIR;
        selections[nsel++] = EVP_PKEY_PUBLIC_KEY;
    }
    ERR_clear_error();
    for (attempt = 0; attempt < nsel && !pkey; attempt++) {
        const unsigned char *p = data.p;
        size_t len = data.len;
        const char *st = in_struct;
        OSSL_DECODER_CTX *dctx;
        if (want_priv && type && !strcmp(type, "spki"))
            break;
        if (selections[attempt] == EVP_PKEY_KEYPAIR && st && !strcmp(st, "SubjectPublicKeyInfo"))
            continue;
        dctx = OSSL_DECODER_CTX_new_for_pkey(&pkey, is_pem ? "PEM" : "DER", st, keytype,
                                             selections[attempt], NULL, NULL);
        if (!dctx)
            continue;
        if (has_pass)
            OSSL_DECODER_CTX_set_passphrase(dctx, pass.p, pass.len);
        else
            OSSL_DECODER_CTX_set_passphrase_cb(dctx, qc_pass_cb, &needed);
        if (!OSSL_DECODER_from_data(dctx, &p, &len)) {
            EVP_PKEY_free(pkey);
            pkey = NULL;
        }
        OSSL_DECODER_CTX_free(dctx);
        if (needed)
            break;
    }
    if (!pkey) {
        if (needed) {
            ERR_clear_error();
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_MISSING_PASSPHRASE",
                     "Passphrase required for encrypted key");
        } else if (ERR_peek_last_error()) {
            qc_throw_ossl(ctx, NULL);
        } else {
            qc_throw(ctx, JS_PLAIN_ERROR, "ERR_OSSL_UNSUPPORTED",
                     "error:1E08010C:DECODER routines::unsupported");
        }
        goto done;
    }
    ERR_clear_error();
    if (want_priv) {
        if (!qc_pkey_has_private(pkey)) {
            EVP_PKEY_free(pkey);
            qc_throw(ctx, JS_PLAIN_ERROR, "ERR_OSSL_UNSUPPORTED",
                     "error:1E08010C:DECODER routines::unsupported");
            goto done;
        }
        ret = qc_key_wrap(ctx, pkey, 1);
    } else if (want_pub) {
        EVP_PKEY *pub = qc_pkey_public(pkey);
        EVP_PKEY_free(pkey);
        ret = qc_key_wrap(ctx, pub, 0);
    } else {
        ret = qc_key_wrap(ctx, pkey, qc_pkey_has_private(pkey));
    }
done:
    qc_in_free(ctx, &data);
    qc_in_free(ctx, &pass);
    qc_cstr_free(ctx, format);
    qc_cstr_free(ctx, kind);
    qc_cstr_free(ctx, type);
    return ret;
}

static const char *qc_pkey_type(EVP_PKEY *pkey, char *buf, size_t size)
{
    if (EVP_PKEY_is_a(pkey, "RSA-PSS"))
        return "rsa-pss";
    if (EVP_PKEY_is_a(pkey, "RSA"))
        return "rsa";
    if (EVP_PKEY_is_a(pkey, "DSA"))
        return "dsa";
    if (EVP_PKEY_is_a(pkey, "DH") || EVP_PKEY_is_a(pkey, "DHX"))
        return "dh";
    if (EVP_PKEY_is_a(pkey, "EC"))
        return "ec";
    if (EVP_PKEY_is_a(pkey, "ED25519"))
        return "ed25519";
    if (EVP_PKEY_is_a(pkey, "ED448"))
        return "ed448";
    if (EVP_PKEY_is_a(pkey, "X25519"))
        return "x25519";
    if (EVP_PKEY_is_a(pkey, "X448"))
        return "x448";
    qc_lower(buf, EVP_PKEY_get0_type_name(pkey) ? EVP_PKEY_get0_type_name(pkey) : "unknown", size);
    return buf;
}

static JSValue qc_key_type_get(JSContext *ctx, JSValueConst this_val)
{
    QCKey *k = qc_key_get(ctx, this_val);
    if (!k)
        return JS_EXCEPTION;
    return JS_NewString(ctx, k->priv ? "private" : "public");
}

static JSValue qc_key_asym_type(JSContext *ctx, JSValueConst this_val)
{
    QCKey *k = qc_key_get(ctx, this_val);
    char buf[64];
    if (!k)
        return JS_EXCEPTION;
    return JS_NewString(ctx, qc_pkey_type(k->pkey, buf, sizeof(buf)));
}

static JSValue qc_key_bits(JSContext *ctx, JSValueConst this_val)
{
    QCKey *k = qc_key_get(ctx, this_val);
    if (!k)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, EVP_PKEY_get_bits(k->pkey));
}

static int qc_group_name(EVP_PKEY *pkey, char *buf, size_t size)
{
    size_t len = 0;
    if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, buf, size, &len) != 1) {
        ERR_clear_error();
        return -1;
    }
    return 0;
}

static JSValue qc_key_details(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    JSValue obj;
    BIGNUM *bn = NULL;
    char buf[80], lower[80];
    size_t len;
    int ival;

    if (!k)
        return JS_EXCEPTION;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (EVP_PKEY_is_a(k->pkey, "RSA") || EVP_PKEY_is_a(k->pkey, "RSA-PSS")) {
        qb_set(ctx, obj, "modulusLength", JS_NewInt32(ctx, EVP_PKEY_get_bits(k->pkey)));
        if (EVP_PKEY_get_bn_param(k->pkey, OSSL_PKEY_PARAM_RSA_E, &bn) == 1) {
            if (BN_num_bytes(bn) <= 8) {
                uint8_t b[8] = {0};
                uint64_t e = 0;
                int i, n = BN_num_bytes(bn);
                BN_bn2binpad(bn, b, n);
                for (i = 0; i < n; i++)
                    e = (e << 8) | b[i];
                qb_set(ctx, obj, "publicExponent", JS_NewBigUint64(ctx, e));
            }
            BN_free(bn);
            bn = NULL;
        }
        if (EVP_PKEY_is_a(k->pkey, "RSA-PSS")) {
            if (EVP_PKEY_get_utf8_string_param(k->pkey, OSSL_PKEY_PARAM_RSA_DIGEST, buf,
                                               sizeof(buf), &len) == 1) {
                qc_lower(lower, buf, sizeof(lower));
                qb_set(ctx, obj, "hashAlgorithm", JS_NewString(ctx, lower));
                if (EVP_PKEY_get_utf8_string_param(k->pkey, OSSL_PKEY_PARAM_RSA_MGF1_DIGEST, buf,
                                                   sizeof(buf), &len) == 1)
                    qc_lower(lower, buf, sizeof(lower));
                qb_set(ctx, obj, "mgf1HashAlgorithm", JS_NewString(ctx, lower));
                if (EVP_PKEY_get_int_param(k->pkey, OSSL_PKEY_PARAM_RSA_PSS_SALTLEN, &ival) == 1)
                    qb_set(ctx, obj, "saltLength", JS_NewInt32(ctx, ival));
            }
        }
    } else if (EVP_PKEY_is_a(k->pkey, "DSA")) {
        qb_set(ctx, obj, "modulusLength", JS_NewInt32(ctx, EVP_PKEY_get_bits(k->pkey)));
        if (EVP_PKEY_get_bn_param(k->pkey, OSSL_PKEY_PARAM_FFC_Q, &bn) == 1) {
            qb_set(ctx, obj, "divisorLength", JS_NewInt32(ctx, BN_num_bits(bn)));
            BN_free(bn);
            bn = NULL;
        }
    } else if (EVP_PKEY_is_a(k->pkey, "EC")) {
        if (!qc_group_name(k->pkey, buf, sizeof(buf)))
            qb_set(ctx, obj, "namedCurve", JS_NewString(ctx, buf));
    }
    ERR_clear_error();
    return obj;
}

/* export(format, type, cipher, passphrase) */
static JSValue qc_key_export(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    const char *format = NULL, *type = NULL, *cipher = NULL, *st;
    QCBuf pass = {0};
    int has_pass, selection, is_pem;
    OSSL_ENCODER_CTX *ectx = NULL;
    unsigned char *out = NULL;
    size_t len = 0;
    JSValue ret = JS_EXCEPTION;

    if (!k)
        return JS_EXCEPTION;
    format = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    type = JS_ToCString(ctx, qc_arg(argc, argv, 1));
    if (!format || !type)
        goto done;
    if (!JS_IsNull(qc_arg(argc, argv, 2)) && !JS_IsUndefined(qc_arg(argc, argv, 2)) &&
        !(cipher = JS_ToCString(ctx, argv[2])))
        goto done;
    has_pass = !JS_IsNull(qc_arg(argc, argv, 3)) && !JS_IsUndefined(qc_arg(argc, argv, 3));
    if (has_pass && qc_in(ctx, argv[3], &pass))
        goto done;
    is_pem = !strcmp(format, "pem");
    if (!strcmp(type, "spki")) {
        selection = EVP_PKEY_PUBLIC_KEY;
        st = "SubjectPublicKeyInfo";
    } else if (!strcmp(type, "pkcs8")) {
        selection = EVP_PKEY_KEYPAIR;
        st = cipher ? "EncryptedPrivateKeyInfo" : "PrivateKeyInfo";
    } else {
        selection = k->priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
        st = "type-specific";
    }
    ectx = OSSL_ENCODER_CTX_new_for_pkey(k->pkey, selection, is_pem ? "PEM" : "DER", st, NULL);
    if ((!ectx || OSSL_ENCODER_CTX_get_num_encoders(ectx) == 0) && cipher &&
        !strcmp(type, "pkcs8")) {
        OSSL_ENCODER_CTX_free(ectx);
        ectx = OSSL_ENCODER_CTX_new_for_pkey(k->pkey, selection, is_pem ? "PEM" : "DER",
                                             "PrivateKeyInfo", NULL);
    }
    if (!ectx || OSSL_ENCODER_CTX_get_num_encoders(ectx) == 0) {
        ERR_clear_error();
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS",
                 "The selected key encoding is not supported for this key type");
        goto done;
    }
    if (cipher) {
        if (!OSSL_ENCODER_CTX_set_cipher(ectx, cipher, NULL)) {
            ERR_clear_error();
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
            goto done;
        }
        if (!OSSL_ENCODER_CTX_set_passphrase(ectx, pass.p ? pass.p : (const uint8_t *)"",
                                             pass.len)) {
            qc_throw_ossl(ctx, "Failed to set the passphrase");
            goto done;
        }
    }
    if (!OSSL_ENCODER_to_data(ectx, &out, &len)) {
        qc_throw_ossl(ctx, "Failed to encode the key");
        goto done;
    }
    if (is_pem)
        ret = JS_NewStringLen(ctx, (const char *)out, len);
    else
        ret = qb_new_buffer(ctx, out, len);
    OPENSSL_clear_free(out, len);
done:
    OSSL_ENCODER_CTX_free(ectx);
    qc_cstr_free(ctx, format);
    qc_cstr_free(ctx, type);
    qc_cstr_free(ctx, cipher);
    qc_in_free(ctx, &pass);
    return ret;
}

static void qc_set_bn(JSContext *ctx, JSValueConst obj, const char *prop, EVP_PKEY *pkey,
                      const char *param, int pad)
{
    BIGNUM *bn = NULL;
    if (EVP_PKEY_get_bn_param(pkey, param, &bn) == 1) {
        qb_set(ctx, obj, prop, qc_bn_buffer(ctx, bn, pad));
        BN_clear_free(bn);
    }
    ERR_clear_error();
}

static const char *qc_jwk_crv(const char *group)
{
    if (!strcmp(group, "prime256v1") || !strcmp(group, "P-256"))
        return "P-256";
    if (!strcmp(group, "secp384r1") || !strcmp(group, "P-384"))
        return "P-384";
    if (!strcmp(group, "secp521r1") || !strcmp(group, "P-521"))
        return "P-521";
    if (!strcmp(group, "secp256k1"))
        return "secp256k1";
    return NULL;
}

static const char *qc_crv_group(const char *crv)
{
    if (!strcmp(crv, "P-256"))
        return "prime256v1";
    if (!strcmp(crv, "P-384"))
        return "secp384r1";
    if (!strcmp(crv, "P-521"))
        return "secp521r1";
    if (!strcmp(crv, "secp256k1"))
        return "secp256k1";
    return NULL;
}

static JSValue qc_raw_key_buffer(JSContext *ctx, EVP_PKEY *pkey, int priv)
{
    size_t len = 0;
    uint8_t *p;
    int ok = priv ? EVP_PKEY_get_raw_private_key(pkey, NULL, &len)
                  : EVP_PKEY_get_raw_public_key(pkey, NULL, &len);
    if (ok != 1)
        return JS_UNDEFINED;
    p = malloc(len + 1);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    ok = priv ? EVP_PKEY_get_raw_private_key(pkey, p, &len)
              : EVP_PKEY_get_raw_public_key(pkey, p, &len);
    if (ok != 1) {
        free(p);
        return JS_UNDEFINED;
    }
    return qc_buffer_take(ctx, p, len);
}

static int qc_ec_field_bytes(EVP_PKEY *pkey)
{
    char group[80];
    EC_GROUP *g;
    int deg;
    if (qc_group_name(pkey, group, sizeof(group)))
        return (EVP_PKEY_get_bits(pkey) + 7) / 8;
    g = EC_GROUP_new_by_curve_name(OBJ_sn2nid(group));
    if (!g) {
        ERR_clear_error();
        return (EVP_PKEY_get_bits(pkey) + 7) / 8;
    }
    deg = EC_GROUP_get_degree(g);
    EC_GROUP_free(g);
    return (deg + 7) / 8;
}

/* jwk(): the raw JWK components (Buffers) */
static JSValue qc_key_jwk(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    JSValue obj;
    char group[80];
    const char *crv;
    EVP_PKEY *p;

    if (!k)
        return JS_EXCEPTION;
    p = k->pkey;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (EVP_PKEY_is_a(p, "RSA")) {
        qb_set(ctx, obj, "kty", JS_NewString(ctx, "RSA"));
        qc_set_bn(ctx, obj, "n", p, OSSL_PKEY_PARAM_RSA_N, 0);
        qc_set_bn(ctx, obj, "e", p, OSSL_PKEY_PARAM_RSA_E, 0);
        if (k->priv) {
            qc_set_bn(ctx, obj, "d", p, OSSL_PKEY_PARAM_RSA_D, 0);
            qc_set_bn(ctx, obj, "p", p, OSSL_PKEY_PARAM_RSA_FACTOR1, 0);
            qc_set_bn(ctx, obj, "q", p, OSSL_PKEY_PARAM_RSA_FACTOR2, 0);
            qc_set_bn(ctx, obj, "dp", p, OSSL_PKEY_PARAM_RSA_EXPONENT1, 0);
            qc_set_bn(ctx, obj, "dq", p, OSSL_PKEY_PARAM_RSA_EXPONENT2, 0);
            qc_set_bn(ctx, obj, "qi", p, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, 0);
        }
    } else if (EVP_PKEY_is_a(p, "EC")) {
        int fb;
        if (qc_group_name(p, group, sizeof(group)) || !(crv = qc_jwk_crv(group))) {
            char msg[128];
            JS_FreeValue(ctx, obj);
            snprintf(msg, sizeof(msg), "Unsupported JWK EC curve: %s.", group);
            return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE", msg);
        }
        fb = qc_ec_field_bytes(p);
        qb_set(ctx, obj, "kty", JS_NewString(ctx, "EC"));
        qb_set(ctx, obj, "crv", JS_NewString(ctx, crv));
        qc_set_bn(ctx, obj, "x", p, OSSL_PKEY_PARAM_EC_PUB_X, fb);
        qc_set_bn(ctx, obj, "y", p, OSSL_PKEY_PARAM_EC_PUB_Y, fb);
        if (k->priv)
            qc_set_bn(ctx, obj, "d", p, OSSL_PKEY_PARAM_PRIV_KEY, fb);
    } else if (EVP_PKEY_is_a(p, "ED25519") || EVP_PKEY_is_a(p, "ED448") ||
               EVP_PKEY_is_a(p, "X25519") || EVP_PKEY_is_a(p, "X448")) {
        qb_set(ctx, obj, "kty", JS_NewString(ctx, "OKP"));
        qb_set(ctx, obj, "crv",
               JS_NewString(ctx, EVP_PKEY_is_a(p, "ED25519")  ? "Ed25519"
                                 : EVP_PKEY_is_a(p, "ED448")  ? "Ed448"
                                 : EVP_PKEY_is_a(p, "X25519") ? "X25519"
                                                              : "X448"));
        qb_set(ctx, obj, "x", qc_raw_key_buffer(ctx, p, 0));
        if (k->priv)
            qb_set(ctx, obj, "d", qc_raw_key_buffer(ctx, p, 1));
    } else {
        JS_FreeValue(ctx, obj);
        return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE",
                        "Unsupported JWK Key Type.");
    }
    return obj;
}

/* convert an encoded EC point of curve 'nid' to 'form' */
static int qc_ec_convert(int nid, const uint8_t *in, size_t inlen, point_conversion_form_t form,
                         uint8_t **pout, size_t *plen)
{
    EC_GROUP *g = EC_GROUP_new_by_curve_name(nid);
    EC_POINT *pt = NULL;
    size_t n;
    int ok = 0;
    *pout = NULL;
    if (!g)
        return 0;
    pt = EC_POINT_new(g);
    if (pt && EC_POINT_oct2point(g, pt, in, inlen, NULL) == 1) {
        n = EC_POINT_point2oct(g, pt, form, NULL, 0, NULL);
        if (n > 0 && (*pout = malloc(n)) && EC_POINT_point2oct(g, pt, form, *pout, n, NULL) == n) {
            *plen = n;
            ok = 1;
        }
    }
    if (!ok) {
        free(*pout);
        *pout = NULL;
    }
    EC_POINT_free(pt);
    EC_GROUP_free(g);
    return ok;
}

static point_conversion_form_t qc_point_form(const char *s)
{
    if (s && !strcmp(s, "compressed"))
        return POINT_CONVERSION_COMPRESSED;
    if (s && !strcmp(s, "hybrid"))
        return POINT_CONVERSION_HYBRID;
    return POINT_CONVERSION_UNCOMPRESSED;
}

static JSValue qc_ec_point_of(JSContext *ctx, EVP_PKEY *pkey, point_conversion_form_t form)
{
    uint8_t buf[256], *out;
    size_t len = 0, olen;
    char group[80];
    if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, buf, sizeof(buf),
                                        &len) != 1) {
        ERR_clear_error();
        return JS_UNDEFINED;
    }
    if (qc_group_name(pkey, group, sizeof(group)) ||
        !qc_ec_convert(OBJ_sn2nid(group), buf, len, form, &out, &olen)) {
        ERR_clear_error();
        return qb_new_buffer(ctx, buf, len);
    }
    return qc_buffer_take(ctx, out, olen);
}

static JSValue qc_key_raw_public(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    JSValue v;
    if (!k)
        return JS_EXCEPTION;
    if (EVP_PKEY_is_a(k->pkey, "EC"))
        return qc_ec_point_of(ctx, k->pkey, POINT_CONVERSION_UNCOMPRESSED);
    v = qc_raw_key_buffer(ctx, k->pkey, 0);
    if (JS_IsUndefined(v)) {
        uint8_t *pub = NULL;
        size_t len = EVP_PKEY_get1_encoded_public_key(k->pkey, &pub);
        ERR_clear_error();
        if (len > 0 && pub) {
            v = qb_new_buffer(ctx, pub, len);
            OPENSSL_free(pub);
        }
    }
    ERR_clear_error();
    return v;
}

static JSValue qc_key_raw_private(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    JSValue v;
    if (!k)
        return JS_EXCEPTION;
    if (!k->priv)
        return JS_UNDEFINED;
    if (EVP_PKEY_is_a(k->pkey, "EC")) {
        BIGNUM *bn = NULL;
        BIGNUM *order = NULL;
        char group[80];
        int pad = qc_ec_field_bytes(k->pkey);
        if (!qc_group_name(k->pkey, group, sizeof(group))) {
            EC_GROUP *g = EC_GROUP_new_by_curve_name(OBJ_sn2nid(group));
            order = BN_new();
            if (g && order && EC_GROUP_get_order(g, order, NULL))
                pad = BN_num_bytes(order);
            EC_GROUP_free(g);
            BN_free(order);
        }
        if (EVP_PKEY_get_bn_param(k->pkey, OSSL_PKEY_PARAM_PRIV_KEY, &bn) != 1) {
            ERR_clear_error();
            return JS_UNDEFINED;
        }
        v = qc_bn_buffer(ctx, bn, pad);
        BN_clear_free(bn);
        return v;
    }
    v = qc_raw_key_buffer(ctx, k->pkey, 1);
    ERR_clear_error();
    return v;
}

static JSValue qc_key_ec_point(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    const char *form;
    JSValue v;
    if (!k)
        return JS_EXCEPTION;
    form = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!form)
        return JS_EXCEPTION;
    v = qc_ec_point_of(ctx, k->pkey, qc_point_form(form));
    JS_FreeCString(ctx, form);
    return v;
}

static JSValue qc_key_curve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    char group[80];
    if (!k)
        return JS_EXCEPTION;
    if (qc_group_name(k->pkey, group, sizeof(group)))
        return JS_UNDEFINED;
    return JS_NewString(ctx, group);
}

static JSValue qc_key_to_public(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *k = qc_key_get(ctx, this_val);
    EVP_PKEY *pub;
    if (!k)
        return JS_EXCEPTION;
    pub = k->priv ? qc_pkey_public(k->pkey) : (EVP_PKEY_up_ref(k->pkey) == 1 ? k->pkey : NULL);
    if (!pub)
        return qc_throw_ossl(ctx, "Failed to derive the public key");
    return qc_key_wrap(ctx, pub, 0);
}

static JSValue qc_key_equals(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCKey *a = qc_key_get(ctx, this_val), *b;
    int r;
    if (!a)
        return JS_EXCEPTION;
    b = qc_key_get(ctx, qc_arg(argc, argv, 0));
    if (!b)
        return JS_EXCEPTION;
    if (a->priv != b->priv)
        return JS_FALSE;
    r = EVP_PKEY_eq(a->pkey, b->pkey);
    ERR_clear_error();
    return JS_NewBool(ctx, r == 1);
}

static const JSCFunctionListEntry qc_key_proto_funcs[] = {
    JS_CGETSET_DEF("type", qc_key_type_get, NULL),
    JS_CGETSET_DEF("asymmetricKeyType", qc_key_asym_type, NULL),
    JS_CGETSET_DEF("bits", qc_key_bits, NULL),
    JS_CFUNC_DEF("details", 0, qc_key_details),
    JS_CFUNC_DEF("export", 4, qc_key_export),
    JS_CFUNC_DEF("jwk", 0, qc_key_jwk),
    JS_CFUNC_DEF("rawPublic", 0, qc_key_raw_public),
    JS_CFUNC_DEF("rawPrivate", 0, qc_key_raw_private),
    JS_CFUNC_DEF("ecPoint", 1, qc_key_ec_point),
    JS_CFUNC_DEF("curve", 0, qc_key_curve),
    JS_CFUNC_DEF("toPublic", 0, qc_key_to_public),
    JS_CFUNC_DEF("equals", 1, qc_key_equals),
};

/* EVP_PKEY_fromdata() helper: 'selection' from 'bld' (freed) */
static EVP_PKEY *qc_pkey_fromdata(const char *type, OSSL_PARAM_BLD *bld, int selection)
{
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    EVP_PKEY *pkey = NULL;
    params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    if (!params)
        return NULL;
    pctx = EVP_PKEY_CTX_new_from_name(NULL, type, NULL);
    if (!pctx || EVP_PKEY_fromdata_init(pctx) != 1 ||
        EVP_PKEY_fromdata(pctx, &pkey, selection, params) != 1) {
        EVP_PKEY_free(pkey);
        pkey = NULL;
    }
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    return pkey;
}

static int qc_pkey_check(EVP_PKEY *pkey, int priv)
{
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    int ok = c && EVP_PKEY_public_check(c) == 1 && (!priv || EVP_PKEY_pairwise_check(c) == 1);
    EVP_PKEY_CTX_free(c);
    ERR_clear_error();
    return ok;
}

/* the bytes of obj[prop] (NULL if absent) */
static int qc_prop_bytes(JSContext *ctx, JSValueConst obj, const char *prop, QCBuf *b, int *present)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    int r = 0;
    b->alloc = NULL;
    b->p = NULL;
    b->len = 0;
    *present = 0;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        r = qc_in(ctx, v, b);
        if (!r && b->alloc == NULL && b->len) {
            /* copy: the value is freed below */
            uint8_t *c = js_malloc(ctx, b->len);
            if (!c)
                r = -1;
            else {
                memcpy(c, b->p, b->len);
                b->alloc = c;
                b->p = c;
            }
        }
        *present = !r;
    }
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue qc_invalid_jwk(JSContext *ctx, const char *what)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "Invalid JWK %s key", what);
    return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_JWK", msg);
}

/* keyFromJwk(kty, parts, isPrivate) */
static JSValue qc_key_from_jwk(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *kty = NULL, *crv = NULL;
    JSValueConst parts = qc_arg(argc, argv, 1);
    int priv, present;
    JSValue ret = JS_EXCEPTION, v;
    EVP_PKEY *pkey = NULL;
    OSSL_PARAM_BLD *bld = NULL;
    BIGNUM *bns[8] = {0};
    QCBuf b[8];
    int i;

    memset(b, 0, sizeof(b));
    if ((priv = JS_ToBool(ctx, qc_arg(argc, argv, 2))) < 0)
        return JS_EXCEPTION;
    kty = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!kty)
        return JS_EXCEPTION;
    if (!strcmp(kty, "RSA")) {
        static const char *props[8] = {"n", "e", "d", "p", "q", "dp", "dq", "qi"};
        static const char *names[8] = {
            OSSL_PKEY_PARAM_RSA_N,         OSSL_PKEY_PARAM_RSA_E,
            OSSL_PKEY_PARAM_RSA_D,         OSSL_PKEY_PARAM_RSA_FACTOR1,
            OSSL_PKEY_PARAM_RSA_FACTOR2,   OSSL_PKEY_PARAM_RSA_EXPONENT1,
            OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1,
        };
        int n = priv ? 8 : 2;
        bld = OSSL_PARAM_BLD_new();
        if (!bld)
            goto oom;
        for (i = 0; i < n; i++) {
            if (qc_prop_bytes(ctx, parts, props[i], &b[i], &present))
                goto done;
            if (!present) {
                if (i >= 3)
                    continue; /* CRT parameters are optional */
                qc_invalid_jwk(ctx, "RSA");
                goto done;
            }
            bns[i] = BN_bin2bn(b[i].p, (int)b[i].len, NULL);
            if (!bns[i] || !OSSL_PARAM_BLD_push_BN(bld, names[i], bns[i]))
                goto oom;
        }
        pkey = qc_pkey_fromdata("RSA", bld, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY);
        bld = NULL;
        if (!pkey) {
            ERR_clear_error();
            qc_invalid_jwk(ctx, "RSA");
            goto done;
        }
    } else if (!strcmp(kty, "EC")) {
        const char *group;
        int fb, nid;
        EC_GROUP *g;
        uint8_t *pub;
        v = JS_GetPropertyStr(ctx, parts, "crv");
        crv = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        JS_FreeValue(ctx, v);
        if (!crv || !(group = qc_crv_group(crv))) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Unsupported JWK EC curve: %s.", crv ? crv : "undefined");
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE", msg);
            goto done;
        }
        nid = OBJ_sn2nid(group);
        g = EC_GROUP_new_by_curve_name(nid);
        if (!g)
            goto oom;
        fb = (EC_GROUP_get_degree(g) + 7) / 8;
        EC_GROUP_free(g);
        if (qc_prop_bytes(ctx, parts, "x", &b[0], &present) || !present ||
            qc_prop_bytes(ctx, parts, "y", &b[1], &present) || !present) {
            if (!JS_HasException(ctx))
                qc_invalid_jwk(ctx, "EC");
            goto done;
        }
        if (b[0].len != (size_t)fb || b[1].len != (size_t)fb) {
            qc_invalid_jwk(ctx, "EC");
            goto done;
        }
        if (priv) {
            if (qc_prop_bytes(ctx, parts, "d", &b[2], &present) || !present ||
                b[2].len != (size_t)fb) {
                if (!JS_HasException(ctx))
                    qc_invalid_jwk(ctx, "EC");
                goto done;
            }
        }
        pub = malloc(2 * fb + 1);
        bld = OSSL_PARAM_BLD_new();
        if (!pub || !bld) {
            free(pub);
            goto oom;
        }
        pub[0] = 4;
        memcpy(pub + 1, b[0].p, fb);
        memcpy(pub + 1 + fb, b[1].p, fb);
        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0) ||
            !OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, 2 * fb + 1)) {
            free(pub);
            goto oom;
        }
        free(pub);
        if (priv) {
            bns[0] = BN_bin2bn(b[2].p, (int)b[2].len, NULL);
            if (!bns[0] || !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bns[0]))
                goto oom;
        }
        pkey = qc_pkey_fromdata("EC", bld, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY);
        bld = NULL;
        if (!pkey || !qc_pkey_check(pkey, priv)) {
            ERR_clear_error();
            qc_invalid_jwk(ctx, "EC");
            goto done;
        }
    } else if (!strcmp(kty, "OKP")) {
        const char *type;
        v = JS_GetPropertyStr(ctx, parts, "crv");
        crv = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        JS_FreeValue(ctx, v);
        type = !crv                      ? NULL
               : !strcmp(crv, "Ed25519") ? "ED25519"
               : !strcmp(crv, "Ed448")   ? "ED448"
               : !strcmp(crv, "X25519")  ? "X25519"
               : !strcmp(crv, "X448")    ? "X448"
                                         : NULL;
        if (!type) {
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK OKP key");
            goto done;
        }
        if (qc_prop_bytes(ctx, parts, priv ? "d" : "x", &b[0], &present) || !present) {
            if (!JS_HasException(ctx))
                qc_invalid_jwk(ctx, "OKP");
            goto done;
        }
        pkey = priv ? EVP_PKEY_new_raw_private_key_ex(NULL, type, NULL, b[0].p, b[0].len)
                    : EVP_PKEY_new_raw_public_key_ex(NULL, type, NULL, b[0].p, b[0].len);
        if (!pkey) {
            ERR_clear_error();
            qc_invalid_jwk(ctx, "OKP");
            goto done;
        }
    } else {
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE",
                 "Unsupported JWK Key Type.");
        goto done;
    }
    ret = qc_key_wrap(ctx, pkey, priv);
    pkey = NULL;
    goto done;
oom:
    JS_ThrowOutOfMemory(ctx);
done:
    EVP_PKEY_free(pkey);
    OSSL_PARAM_BLD_free(bld);
    for (i = 0; i < 8; i++)
        BN_clear_free(bns[i]);
    for (i = 0; i < 8; i++) {
        if (b[i].alloc)
            OPENSSL_cleanse(b[i].alloc, b[i].len);
        qc_in_free(ctx, &b[i]);
    }
    qc_cstr_free(ctx, kty);
    qc_cstr_free(ctx, crv);
    return ret;
}

/* keyFromRaw(type, data, isPrivate) */
static JSValue qc_key_from_raw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *type;
    QCBuf b;
    int priv;
    EVP_PKEY *pkey;
    if ((priv = JS_ToBool(ctx, qc_arg(argc, argv, 2))) < 0)
        return JS_EXCEPTION;
    type = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!type)
        return JS_EXCEPTION;
    if (qc_in(ctx, qc_arg(argc, argv, 1), &b)) {
        JS_FreeCString(ctx, type);
        return JS_EXCEPTION;
    }
    pkey = priv ? EVP_PKEY_new_raw_private_key_ex(NULL, type, NULL, b.p, b.len)
                : EVP_PKEY_new_raw_public_key_ex(NULL, type, NULL, b.p, b.len);
    JS_FreeCString(ctx, type);
    qc_in_free(ctx, &b);
    if (!pkey)
        return qc_throw_ossl(ctx, "Invalid key data");
    return qc_key_wrap(ctx, pkey, priv);
}

static int qc_curve_nid(const char *name)
{
    int nid = EC_curve_nist2nid(name);
    if (nid == NID_undef)
        nid = OBJ_sn2nid(name);
    if (nid == NID_undef)
        nid = OBJ_ln2nid(name);
    return nid;
}

/* keyFromEcPoint(curve, point) */
static JSValue qc_key_from_ec_point(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    const char *curve;
    QCBuf b;
    int nid;
    OSSL_PARAM_BLD *bld;
    EVP_PKEY *pkey = NULL;

    curve = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!curve)
        return JS_EXCEPTION;
    nid = qc_curve_nid(curve);
    JS_FreeCString(ctx, curve);
    if (nid == NID_undef)
        return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_CURVE", "Invalid EC curve name");
    if (qc_in(ctx, qc_arg(argc, argv, 1), &b))
        return JS_EXCEPTION;
    bld = OSSL_PARAM_BLD_new();
    if (bld &&
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, OBJ_nid2sn(nid), 0) &&
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, b.p, b.len)) {
        pkey = qc_pkey_fromdata("EC", bld, EVP_PKEY_PUBLIC_KEY);
    } else {
        OSSL_PARAM_BLD_free(bld);
    }
    qc_in_free(ctx, &b);
    if (!pkey || !qc_pkey_check(pkey, 0)) {
        EVP_PKEY_free(pkey);
        ERR_clear_error();
        return qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_KEYTYPE", "Invalid EC public key");
    }
    return qc_key_wrap(ctx, pkey, 0);
}

/* the encoded public point of the private scalar 'd' */
static uint8_t *qc_ec_pub_from_priv(int nid, const BIGNUM *d, size_t *plen)
{
    EC_GROUP *g = EC_GROUP_new_by_curve_name(nid);
    EC_POINT *pt = NULL;
    BIGNUM *order = BN_new();
    uint8_t *out = NULL;
    size_t n;
    if (!g || !order || !EC_GROUP_get_order(g, order, NULL) || BN_is_zero(d) ||
        BN_cmp(d, order) >= 0)
        goto done;
    pt = EC_POINT_new(g);
    if (!pt || !EC_POINT_mul(g, pt, d, NULL, NULL, NULL))
        goto done;
    n = EC_POINT_point2oct(g, pt, POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);
    out = n ? malloc(n) : NULL;
    if (out && EC_POINT_point2oct(g, pt, POINT_CONVERSION_UNCOMPRESSED, out, n, NULL) == n) {
        *plen = n;
    } else {
        free(out);
        out = NULL;
    }
done:
    EC_POINT_free(pt);
    EC_GROUP_free(g);
    BN_free(order);
    return out;
}

/* keyFromEcPrivate(curve, d) */
static JSValue qc_key_from_ec_private(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    const char *curve;
    int nid;
    BIGNUM *d;
    uint8_t *pub;
    size_t plen = 0;
    OSSL_PARAM_BLD *bld;
    EVP_PKEY *pkey = NULL;

    curve = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!curve)
        return JS_EXCEPTION;
    nid = qc_curve_nid(curve);
    JS_FreeCString(ctx, curve);
    if (nid == NID_undef)
        return qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_CURVE", "Invalid EC curve name");
    d = qc_bn_arg(ctx, qc_arg(argc, argv, 1));
    if (!d)
        return JS_EXCEPTION;
    pub = qc_ec_pub_from_priv(nid, d, &plen);
    if (!pub) {
        BN_clear_free(d);
        ERR_clear_error();
        return qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_KEYTYPE",
                        "Private key is not valid for specified curve.");
    }
    bld = OSSL_PARAM_BLD_new();
    if (bld &&
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, OBJ_nid2sn(nid), 0) &&
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, plen) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, d)) {
        pkey = qc_pkey_fromdata("EC", bld, EVP_PKEY_KEYPAIR);
    } else {
        OSSL_PARAM_BLD_free(bld);
    }
    free(pub);
    BN_clear_free(d);
    if (!pkey)
        return qc_throw_ossl(ctx, "Invalid EC private key");
    return qc_key_wrap(ctx, pkey, 1);
}

/* dhGroup primes (RFC 2409 / RFC 3526) */
static BIGNUM *qc_dh_group_prime(const char *name)
{
    if (!strcmp(name, "modp1"))
        return BN_get_rfc2409_prime_768(NULL);
    if (!strcmp(name, "modp2"))
        return BN_get_rfc2409_prime_1024(NULL);
    if (!strcmp(name, "modp5"))
        return BN_get_rfc3526_prime_1536(NULL);
    if (!strcmp(name, "modp14"))
        return BN_get_rfc3526_prime_2048(NULL);
    if (!strcmp(name, "modp15"))
        return BN_get_rfc3526_prime_3072(NULL);
    if (!strcmp(name, "modp16"))
        return BN_get_rfc3526_prime_4096(NULL);
    if (!strcmp(name, "modp17"))
        return BN_get_rfc3526_prime_6144(NULL);
    if (!strcmp(name, "modp18"))
        return BN_get_rfc3526_prime_8192(NULL);
    return NULL;
}

/* key generation */
typedef struct {
    QCJob j;
    char type[48];
    int modulus_bits;
    uint64_t pub_exp;
    char hash[64], mgf1[64];
    int salt_len;     /* -1: unset */
    int divisor_bits; /* -1: unset */
    char curve[64];
    int param_enc;    /* 1 named, 0 explicit */
    int prime_bits, generator;
    uint8_t *prime;
    size_t prime_len;
    char group[16];
    EVP_PKEY *pub, *priv;
} QCKeygenJob;

static void qc_keygen_fini(QCJob *j)
{
    QCKeygenJob *k = (QCKeygenJob *)j;
    EVP_PKEY_free(k->pub);
    EVP_PKEY_free(k->priv);
    free(k->prime);
}

static EVP_PKEY *qc_dh_params(const BIGNUM *p, const BIGNUM *g)
{
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    if (!bld || !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_P, p) ||
        !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_G, g)) {
        OSSL_PARAM_BLD_free(bld);
        return NULL;
    }
    return qc_pkey_fromdata("DH", bld, EVP_PKEY_KEY_PARAMETERS);
}

static int qc_keygen_run(QCJob *j)
{
    QCKeygenJob *k = (QCKeygenJob *)j;
    EVP_PKEY_CTX *pctx = NULL, *kctx = NULL;
    EVP_PKEY *params = NULL;
    BIGNUM *e = NULL, *p = NULL, *g = NULL;
    int ok = 0;
    const char *t = k->type;

    if (!strcmp(t, "rsa") || !strcmp(t, "rsa-pss")) {
        int pss = !strcmp(t, "rsa-pss");
        kctx = EVP_PKEY_CTX_new_from_name(NULL, pss ? "RSA-PSS" : "RSA", NULL);
        e = BN_new();
        if (!kctx || !e || EVP_PKEY_keygen_init(kctx) != 1 ||
            !BN_set_word(e, (BN_ULONG)k->pub_exp) ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, k->modulus_bits) != 1 ||
            EVP_PKEY_CTX_set1_rsa_keygen_pubexp(kctx, e) != 1)
            goto done;
        if (pss) {
            if (k->hash[0] && EVP_PKEY_CTX_set_rsa_pss_keygen_md_name(kctx, k->hash, NULL) != 1) {
                qc_job_fail(j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
                goto done;
            }
            if (k->mgf1[0] && EVP_PKEY_CTX_set_rsa_pss_keygen_mgf1_md_name(kctx, k->mgf1) != 1) {
                qc_job_fail(j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_DIGEST", "Invalid MGF1 digest");
                goto done;
            }
            if (k->salt_len >= 0 && EVP_PKEY_CTX_set_rsa_pss_keygen_saltlen(kctx, k->salt_len) != 1)
                goto done;
        }
    } else if (!strcmp(t, "ec")) {
        int nid = qc_curve_nid(k->curve);
        EC_GROUP *grp = nid == NID_undef ? NULL : EC_GROUP_new_by_curve_name(nid);
        if (!grp) {
            qc_job_fail(j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_CURVE", "Invalid EC curve name");
            goto done;
        }
        EC_GROUP_free(grp);
        kctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
            EVP_PKEY_CTX_set_group_name(kctx, OBJ_nid2sn(nid)) != 1 ||
            EVP_PKEY_CTX_set_ec_param_enc(kctx, k->param_enc ? OPENSSL_EC_NAMED_CURVE
                                                             : OPENSSL_EC_EXPLICIT_CURVE) != 1)
            goto done;
    } else if (!strcmp(t, "dsa")) {
        pctx = EVP_PKEY_CTX_new_from_name(NULL, "DSA", NULL);
        if (!pctx || EVP_PKEY_paramgen_init(pctx) != 1 ||
            EVP_PKEY_CTX_set_dsa_paramgen_bits(pctx, k->modulus_bits) != 1 ||
            (k->divisor_bits >= 0 &&
             EVP_PKEY_CTX_set_dsa_paramgen_q_bits(pctx, k->divisor_bits) != 1) ||
            EVP_PKEY_generate(pctx, &params) != 1)
            goto done;
        kctx = EVP_PKEY_CTX_new_from_pkey(NULL, params, NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1)
            goto done;
    } else if (!strcmp(t, "dh")) {
        if (k->group[0]) {
            p = qc_dh_group_prime(k->group);
            if (!p) {
                qc_job_fail(j, JS_PLAIN_ERROR, "ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group");
                goto done;
            }
            g = BN_new();
            if (!g || !BN_set_word(g, 2))
                goto done;
            params = qc_dh_params(p, g);
        } else if (k->prime) {
            p = BN_bin2bn(k->prime, (int)k->prime_len, NULL);
            g = BN_new();
            if (!p || !g || !BN_set_word(g, k->generator))
                goto done;
            params = qc_dh_params(p, g);
        } else {
            pctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
            if (!pctx || EVP_PKEY_paramgen_init(pctx) != 1 ||
                EVP_PKEY_CTX_set_dh_paramgen_prime_len(pctx, k->prime_bits) != 1 ||
                EVP_PKEY_CTX_set_dh_paramgen_generator(pctx, k->generator) != 1 ||
                EVP_PKEY_generate(pctx, &params) != 1)
                goto done;
        }
        if (!params)
            goto done;
        kctx = EVP_PKEY_CTX_new_from_pkey(NULL, params, NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1)
            goto done;
    } else {
        /* ed25519, ed448, x25519, x448 and provider key types (ml-dsa-44...) */
        kctx = EVP_PKEY_CTX_new_from_name(NULL, t, NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1) {
            qc_job_fail(j, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE", "Unsupported key type");
            goto done;
        }
    }
    if (EVP_PKEY_generate(kctx, &k->priv) != 1)
        goto done;
    k->pub = qc_pkey_public(k->priv);
    ok = k->pub != NULL;
done:
    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_CTX_free(kctx);
    EVP_PKEY_free(params);
    BN_free(e);
    BN_free(p);
    BN_free(g);
    return ok ? 0 : -1;
}

static JSValue qc_keygen_done(JSContext *ctx, QCJob *j)
{
    QCKeygenJob *k = (QCKeygenJob *)j;
    JSValue vals[2];
    vals[0] = qc_key_wrap(ctx, k->pub, 0);
    k->pub = NULL;
    if (JS_IsException(vals[0]))
        return vals[0];
    vals[1] = qc_key_wrap(ctx, k->priv, 1);
    k->priv = NULL;
    if (JS_IsException(vals[1])) {
        JS_FreeValue(ctx, vals[0]);
        return vals[1];
    }
    return JS_NewArrayFrom(ctx, 2, vals);
}

static int qc_opt_str(JSContext *ctx, JSValueConst obj, const char *prop, char *buf, size_t size)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    const char *s;
    buf[0] = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    snprintf(buf, size, "%s", s);
    JS_FreeCString(ctx, s);
    return 0;
}

static int qc_opt_int(JSContext *ctx, JSValueConst obj, const char *prop, int def, int *pres)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    int r;
    if (JS_IsException(v))
        return -1;
    r = qc_int(ctx, v, def, pres);
    JS_FreeValue(ctx, v);
    return r;
}

/* generateKeyPair(type, opts, cb) */
static JSValue qc_generate_key_pair(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    QCKeygenJob *k;
    JSValueConst opts = qc_arg(argc, argv, 1);
    const char *type;
    char enc[16];
    JSValue v;

    k = qc_job_new(ctx, sizeof(*k), qc_keygen_run, qc_keygen_done, qc_keygen_fini);
    if (!k)
        return JS_EXCEPTION;
    type = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    if (!type)
        goto fail;
    snprintf(k->type, sizeof(k->type), "%s", type);
    JS_FreeCString(ctx, type);
    k->pub_exp = 0x10001;
    k->salt_len = -1;
    k->divisor_bits = -1;
    k->param_enc = 1;
    k->generator = 2;
    if (JS_IsObject(opts)) {
        if (qc_opt_int(ctx, opts, "modulusLength", 0, &k->modulus_bits) ||
            qc_opt_str(ctx, opts, "hashAlgorithm", k->hash, sizeof(k->hash)) ||
            qc_opt_str(ctx, opts, "mgf1HashAlgorithm", k->mgf1, sizeof(k->mgf1)) ||
            qc_opt_int(ctx, opts, "saltLength", -1, &k->salt_len) ||
            qc_opt_int(ctx, opts, "divisorLength", -1, &k->divisor_bits) ||
            qc_opt_str(ctx, opts, "namedCurve", k->curve, sizeof(k->curve)) ||
            qc_opt_int(ctx, opts, "primeLength", 0, &k->prime_bits) ||
            qc_opt_int(ctx, opts, "generator", 2, &k->generator) ||
            qc_opt_str(ctx, opts, "group", k->group, sizeof(k->group)) ||
            qc_opt_str(ctx, opts, "paramEncoding", enc, sizeof(enc)))
            goto fail;
        if (!strcmp(enc, "explicit"))
            k->param_enc = 0;
        v = JS_GetPropertyStr(ctx, opts, "publicExponent");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            double d;
            if (JS_IsBigInt(ctx, v)) {
                if (JS_ToBigUint64(ctx, &k->pub_exp, v)) {
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
            } else {
                if (JS_ToFloat64(ctx, &d, v)) {
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                k->pub_exp = (uint64_t)d;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "prime");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && qc_copy(ctx, v, &k->prime, &k->prime_len, 1, NULL)) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
        JS_FreeValue(ctx, v);
    }
    return qc_job_start(ctx, &k->j, qc_arg(argc, argv, 2));
fail:
    qc_job_free(JS_GetRuntime(ctx), &k->j);
    return JS_EXCEPTION;
}

/* ------------------------------------------------- signatures and more */

/* size of one IEEE-P1363 signature half for EC and DSA keys, 0 otherwise */
static int qc_sig_half_bytes(EVP_PKEY *pkey)
{
    int n = 0;
    if (EVP_PKEY_is_a(pkey, "EC")) {
        char group[80];
        EC_GROUP *g;
        BIGNUM *order = BN_new();
        if (!qc_group_name(pkey, group, sizeof(group)) &&
            (g = EC_GROUP_new_by_curve_name(OBJ_sn2nid(group)))) {
            if (order && EC_GROUP_get_order(g, order, NULL))
                n = BN_num_bytes(order);
            EC_GROUP_free(g);
        }
        BN_free(order);
        if (!n)
            n = (EVP_PKEY_get_bits(pkey) + 7) / 8;
    } else if (EVP_PKEY_is_a(pkey, "DSA")) {
        BIGNUM *q = NULL;
        if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_Q, &q) == 1)
            n = BN_num_bytes(q);
        BN_free(q);
    }
    ERR_clear_error();
    return n;
}

static int qc_der_len(const uint8_t **pp, const uint8_t *end, size_t *plen)
{
    const uint8_t *p = *pp;
    size_t v = 0;
    int n;
    if (p >= end)
        return -1;
    if (*p < 0x80) {
        *plen = *p++;
    } else {
        n = *p++ & 0x7f;
        if (n == 0 || n > 4)
            return -1;
        while (n--) {
            if (p >= end)
                return -1;
            v = (v << 8) | *p++;
        }
        *plen = v;
    }
    if (*plen > (size_t)(end - p))
        return -1;
    *pp = p;
    return 0;
}

/* DER ECDSA/DSA signature -> r || s (each 'n' bytes) */
static int qc_der_to_p1363(const uint8_t *der, size_t dlen, int n, uint8_t *out)
{
    const uint8_t *p = der, *end = der + dlen;
    size_t len;
    int i;
    if (p >= end || *p++ != 0x30 || qc_der_len(&p, end, &len))
        return -1;
    end = p + len;
    memset(out, 0, 2 * n);
    for (i = 0; i < 2; i++) {
        if (p >= end || *p++ != 0x02 || qc_der_len(&p, end, &len))
            return -1;
        while (len > 0 && *p == 0) {
            p++;
            len--;
        }
        if (len > (size_t)n)
            return -1;
        memcpy(out + i * n + (n - len), p, len);
        p += len;
    }
    return 0;
}

static size_t qc_der_put_len(uint8_t *q, size_t len)
{
    if (len < 0x80) {
        q[0] = (uint8_t)len;
        return 1;
    }
    if (len < 0x100) {
        q[0] = 0x81;
        q[1] = (uint8_t)len;
        return 2;
    }
    q[0] = 0x82;
    q[1] = (uint8_t)(len >> 8);
    q[2] = (uint8_t)len;
    return 3;
}

/* r || s -> DER (out has room for 2 * n + 16 bytes) */
static size_t qc_p1363_to_der(const uint8_t *sig, int n, uint8_t *out)
{
    const uint8_t *parts[2];
    size_t lens[2], pads[2], body = 0, pos = 0;
    int i;
    for (i = 0; i < 2; i++) {
        const uint8_t *p = sig + i * n;
        size_t len = n;
        while (len > 1 && *p == 0) {
            p++;
            len--;
        }
        parts[i] = p;
        lens[i] = len;
        pads[i] = (*p & 0x80) ? 1 : 0;
        body += 1 +
                (lens[i] + pads[i] < 0x80    ? 1
                 : lens[i] + pads[i] < 0x100 ? 2
                                             : 3) +
                lens[i] + pads[i];
    }
    out[pos++] = 0x30;
    pos += qc_der_put_len(out + pos, body);
    for (i = 0; i < 2; i++) {
        out[pos++] = 0x02;
        pos += qc_der_put_len(out + pos, lens[i] + pads[i]);
        if (pads[i])
            out[pos++] = 0;
        memcpy(out + pos, parts[i], lens[i]);
        pos += lens[i];
    }
    return pos;
}

typedef struct {
    QCJob j;
    EVP_PKEY *pkey;
    uint8_t *data, *sig, *context;
    size_t dlen, slen, clen;
    char digest[64];
    int has_digest, has_context, padding, salt_len, has_salt, dsa_enc;
} QCSignJob;

static void qc_sign_fini(QCJob *j)
{
    QCSignJob *s = (QCSignJob *)j;
    EVP_PKEY_free(s->pkey);
    free(s->data);
    free(s->sig);
    free(s->context);
}

static int qc_sign_init(QCSignJob *s, EVP_MD_CTX *mctx, int verify)
{
    EVP_PKEY_CTX *pctx = NULL;
    OSSL_PARAM params[2];
    char mdname[64];
    const char *md = NULL;
    int r;

    if (s->has_digest) {
        size_t force;
        EVP_MD *m = qc_fetch_md(s->digest, &force);
        if (!m)
            return qc_job_fail(&s->j, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
        snprintf(mdname, sizeof(mdname), "%s", EVP_MD_get0_name(m));
        EVP_MD_free(m);
        md = mdname;
    }
    if (s->has_context) {
        params[0] = OSSL_PARAM_construct_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING,
                                                      s->context, s->clen);
        params[1] = OSSL_PARAM_construct_end();
    }
    if (verify)
        r = EVP_DigestVerifyInit_ex(mctx, &pctx, md, NULL, NULL, s->pkey,
                                    s->has_context ? params : NULL);
    else
        r = EVP_DigestSignInit_ex(mctx, &pctx, md, NULL, NULL, s->pkey,
                                  s->has_context ? params : NULL);
    if (r != 1)
        return -1;
    if (EVP_PKEY_is_a(s->pkey, "RSA") || EVP_PKEY_is_a(s->pkey, "RSA-PSS")) {
        if (s->padding >= 0 && EVP_PKEY_CTX_set_rsa_padding(pctx, s->padding) != 1)
            return -1;
        if (s->has_salt &&
            (s->padding == RSA_PKCS1_PSS_PADDING || EVP_PKEY_is_a(s->pkey, "RSA-PSS")) &&
            EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, s->salt_len) != 1)
            return -1;
    }
    return 0;
}

static int qc_sign_run(QCJob *j)
{
    QCSignJob *s = (QCSignJob *)j;
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    size_t len = 0;
    int ok = 0, n;

    if (!mctx || qc_sign_init(s, mctx, 0))
        goto done;
    if (EVP_DigestSign(mctx, NULL, &len, s->data, s->dlen) != 1)
        goto done;
    j->out = malloc(len + 1);
    if (!j->out)
        goto done;
    if (EVP_DigestSign(mctx, j->out, &len, s->data, s->dlen) != 1)
        goto done;
    j->out_len = len;
    if (s->dsa_enc == 1 && (n = qc_sig_half_bytes(s->pkey)) > 0) {
        uint8_t *p1363 = malloc(2 * n);
        if (!p1363 || qc_der_to_p1363(j->out, len, n, p1363)) {
            free(p1363);
            goto done;
        }
        free(j->out);
        j->out = p1363;
        j->out_len = 2 * n;
    }
    ok = 1;
done:
    EVP_MD_CTX_free(mctx);
    return ok ? 0 : -1;
}

static int qc_verify_run(QCJob *j)
{
    QCSignJob *s = (QCSignJob *)j;
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    const uint8_t *sig = s->sig;
    size_t slen = s->slen;
    uint8_t *der = NULL;
    int n, r = -1;

    if (!mctx || qc_sign_init(s, mctx, 1))
        goto done;
    if (s->dsa_enc == 1 && (n = qc_sig_half_bytes(s->pkey)) > 0) {
        if (slen != (size_t)(2 * n)) {
            j->result = 0;
            r = 0;
            goto done;
        }
        der = malloc(2 * n + 16);
        if (!der)
            goto done;
        slen = qc_p1363_to_der(sig, n, der);
        sig = der;
    }
    j->result = EVP_DigestVerify(mctx, sig, slen, s->data, s->dlen) == 1;
    r = 0;
done:
    free(der);
    EVP_MD_CTX_free(mctx);
    return r;
}

static int qc_key_up(JSContext *ctx, JSValueConst v, EVP_PKEY **pp)
{
    QCKey *k = qc_key_get(ctx, v);
    if (!k)
        return -1;
    EVP_PKEY_up_ref(k->pkey);
    *pp = k->pkey;
    return 0;
}

/* sign(key, data, digest, padding, saltLength, dsaEncoding, context, cb) and
   verify(key, data, signature, digest, padding, saltLength, dsaEncoding, context, cb) */
static JSValue qc_sign_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                              int verify)
{
    QCSignJob *s;
    int o = verify ? 1 : 0;
    JSValueConst dv = qc_arg(argc, argv, 2 + o), sv = qc_arg(argc, argv, 4 + o);
    const char *digest;

    s = qc_job_new(ctx, sizeof(*s), verify ? qc_verify_run : qc_sign_run,
                   verify ? qc_done_bool : qc_done_buffer, qc_sign_fini);
    if (!s)
        return JS_EXCEPTION;
    if (qc_key_up(ctx, qc_arg(argc, argv, 0), &s->pkey) ||
        qc_copy(ctx, qc_arg(argc, argv, 1), &s->data, &s->dlen, 0, NULL))
        goto fail;
    if (verify && qc_copy(ctx, qc_arg(argc, argv, 2), &s->sig, &s->slen, 0, NULL))
        goto fail;
    if (!JS_IsNull(dv) && !JS_IsUndefined(dv)) {
        digest = JS_ToCString(ctx, dv);
        if (!digest)
            goto fail;
        snprintf(s->digest, sizeof(s->digest), "%s", digest);
        JS_FreeCString(ctx, digest);
        s->has_digest = 1;
    }
    if (qc_int(ctx, qc_arg(argc, argv, 3 + o), -1, &s->padding))
        goto fail;
    if (!JS_IsNull(sv) && !JS_IsUndefined(sv)) {
        if (JS_ToInt32(ctx, &s->salt_len, sv))
            goto fail;
        s->has_salt = 1;
    }
    if (qc_int(ctx, qc_arg(argc, argv, 5 + o), 0, &s->dsa_enc))
        goto fail;
    if (qc_copy(ctx, qc_arg(argc, argv, 6 + o), &s->context, &s->clen, 1, NULL))
        goto fail;
    s->has_context = s->context != NULL;
    return qc_job_start(ctx, &s->j, qc_arg(argc, argv, 7 + o));
fail:
    qc_job_free(JS_GetRuntime(ctx), &s->j);
    return JS_EXCEPTION;
}

static JSValue qc_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qc_sign_verify(ctx, this_val, argc, argv, 0);
}

static JSValue qc_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qc_sign_verify(ctx, this_val, argc, argv, 1);
}

/* pkeyCrypt(op, key, data, padding, oaepHash, oaepLabel) */
static JSValue qc_pkey_crypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int op, padding, r = 0;
    QCKey *k;
    QCBuf data = {0}, label = {0};
    const char *hash = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    uint8_t *out = NULL;
    size_t outlen = 0;
    JSValue ret = JS_EXCEPTION;
    char mdname[64];

    if (JS_ToInt32(ctx, &op, qc_arg(argc, argv, 0)))
        return JS_EXCEPTION;
    k = qc_key_get(ctx, qc_arg(argc, argv, 1));
    if (!k || qc_int(ctx, qc_arg(argc, argv, 3), -1, &padding))
        return JS_EXCEPTION;
    if (!JS_IsNull(qc_arg(argc, argv, 4)) && !JS_IsUndefined(qc_arg(argc, argv, 4)) &&
        !(hash = JS_ToCString(ctx, argv[4])))
        return JS_EXCEPTION;
    if (qc_in(ctx, qc_arg(argc, argv, 2), &data))
        goto done;
    if (!JS_IsNull(qc_arg(argc, argv, 5)) && !JS_IsUndefined(qc_arg(argc, argv, 5)) &&
        qc_in(ctx, argv[5], &label))
        goto done;
    pctx = EVP_PKEY_CTX_new_from_pkey(NULL, k->pkey, NULL);
    if (!pctx)
        goto ossl;
    switch (op) {
    case 0:
        r = EVP_PKEY_encrypt_init(pctx);
        break;
    case 1:
        r = EVP_PKEY_decrypt_init(pctx);
        break;
    case 2:
        r = EVP_PKEY_sign_init(pctx);
        break;
    default:
        r = EVP_PKEY_verify_recover_init(pctx);
        break;
    }
    if (r != 1)
        goto ossl;
    if (padding >= 0 && EVP_PKEY_CTX_set_rsa_padding(pctx, padding) != 1)
        goto ossl;
    if (padding == RSA_PKCS1_OAEP_PADDING) {
        size_t force;
        EVP_MD *m = qc_fetch_md(hash ? hash : "sha1", &force);
        if (!m) {
            qc_throw(ctx, JS_TYPE_ERROR, "ERR_OSSL_EVP_INVALID_DIGEST", "Invalid digest used");
            goto done;
        }
        snprintf(mdname, sizeof(mdname), "%s", EVP_MD_get0_name(m));
        EVP_MD_free(m);
        if (EVP_PKEY_CTX_set_rsa_oaep_md_name(pctx, mdname, NULL) != 1 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md_name(pctx, mdname, NULL) != 1)
            goto ossl;
        if (label.len > 0) {
            void *l = OPENSSL_memdup(label.p, label.len);
            if (!l || EVP_PKEY_CTX_set0_rsa_oaep_label(pctx, l, (int)label.len) != 1) {
                OPENSSL_free(l);
                goto ossl;
            }
        }
    }
    switch (op) {
    case 0:
        r = EVP_PKEY_encrypt(pctx, NULL, &outlen, data.p, data.len);
        break;
    case 1:
        r = EVP_PKEY_decrypt(pctx, NULL, &outlen, data.p, data.len);
        break;
    case 2:
        r = EVP_PKEY_sign(pctx, NULL, &outlen, data.p, data.len);
        break;
    default:
        r = EVP_PKEY_verify_recover(pctx, NULL, &outlen, data.p, data.len);
        break;
    }
    if (r != 1)
        goto ossl;
    out = malloc(outlen + 1);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    switch (op) {
    case 0:
        r = EVP_PKEY_encrypt(pctx, out, &outlen, data.p, data.len);
        break;
    case 1:
        r = EVP_PKEY_decrypt(pctx, out, &outlen, data.p, data.len);
        break;
    case 2:
        r = EVP_PKEY_sign(pctx, out, &outlen, data.p, data.len);
        break;
    default:
        r = EVP_PKEY_verify_recover(pctx, out, &outlen, data.p, data.len);
        break;
    }
    if (r != 1)
        goto ossl;
    ret = qc_buffer_take(ctx, out, outlen);
    out = NULL;
    goto done;
ossl:
    qc_throw_ossl(ctx, "Operation failed");
done:
    free(out);
    EVP_PKEY_CTX_free(pctx);
    qc_cstr_free(ctx, hash);
    qc_in_free(ctx, &data);
    qc_in_free(ctx, &label);
    return ret;
}

typedef struct {
    QCJob j;
    EVP_PKEY *a, *b;
    uint8_t *in;
    size_t in_len;
    uint8_t *out2;
    size_t out2_len;
} QCPkeyJob;

static void qc_pkey_job_fini(QCJob *j)
{
    QCPkeyJob *p = (QCPkeyJob *)j;
    EVP_PKEY_free(p->a);
    EVP_PKEY_free(p->b);
    free(p->in);
    if (p->out2) {
        OPENSSL_cleanse(p->out2, p->out2_len);
        free(p->out2);
    }
}

static int qc_derive_run(QCJob *j)
{
    QCPkeyJob *p = (QCPkeyJob *)j;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_pkey(NULL, p->a, NULL);
    size_t len = 0;
    int ok = 0;
    if (!pctx || EVP_PKEY_derive_init(pctx) != 1)
        goto done;
    if ((EVP_PKEY_is_a(p->a, "DH") || EVP_PKEY_is_a(p->a, "DHX")) &&
        EVP_PKEY_CTX_set_dh_pad(pctx, 1) != 1)
        goto done;
    if (EVP_PKEY_derive_set_peer(pctx, p->b) != 1 || EVP_PKEY_derive(pctx, NULL, &len) != 1)
        goto done;
    j->out = malloc(len + 1);
    if (!j->out || EVP_PKEY_derive(pctx, j->out, &len) != 1)
        goto done;
    j->out_len = len;
    ok = 1;
done:
    EVP_PKEY_CTX_free(pctx);
    return ok ? 0 : -1;
}

/* pkeyDerive(priv, pub, cb) */
static JSValue qc_pkey_derive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCPkeyJob *p = qc_job_new(ctx, sizeof(*p), qc_derive_run, qc_done_buffer, qc_pkey_job_fini);
    if (!p)
        return JS_EXCEPTION;
    if (qc_key_up(ctx, qc_arg(argc, argv, 0), &p->a) ||
        qc_key_up(ctx, qc_arg(argc, argv, 1), &p->b)) {
        qc_job_free(JS_GetRuntime(ctx), &p->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &p->j, qc_arg(argc, argv, 2));
}

static int qc_kem_op(EVP_PKEY_CTX *pctx, EVP_PKEY *pkey)
{
    if (EVP_PKEY_is_a(pkey, "RSA"))
        return EVP_PKEY_CTX_set_kem_op(pctx, "RSASVE") == 1;
    if (EVP_PKEY_is_a(pkey, "EC") || EVP_PKEY_is_a(pkey, "X25519") || EVP_PKEY_is_a(pkey, "X448"))
        return EVP_PKEY_CTX_set_kem_op(pctx, "DHKEM") == 1;
    return 1;
}

static int qc_encap_run(QCJob *j)
{
    QCPkeyJob *p = (QCPkeyJob *)j;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_pkey(NULL, p->a, NULL);
    size_t wl = 0, gl = 0;
    int ok = 0;
    if (!pctx || EVP_PKEY_encapsulate_init(pctx, NULL) != 1 || !qc_kem_op(pctx, p->a) ||
        EVP_PKEY_encapsulate(pctx, NULL, &wl, NULL, &gl) != 1)
        goto done;
    p->out2 = malloc(wl + 1);
    j->out = malloc(gl + 1);
    if (!p->out2 || !j->out || EVP_PKEY_encapsulate(pctx, p->out2, &wl, j->out, &gl) != 1)
        goto done;
    p->out2_len = wl;
    j->out_len = gl;
    ok = 1;
done:
    EVP_PKEY_CTX_free(pctx);
    return ok ? 0 : -1;
}

static JSValue qc_encap_done(JSContext *ctx, QCJob *j)
{
    QCPkeyJob *p = (QCPkeyJob *)j;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    qb_set(ctx, obj, "sharedKey", qb_new_buffer(ctx, j->out, j->out_len));
    qb_set(ctx, obj, "ciphertext", qb_new_buffer(ctx, p->out2, p->out2_len));
    return obj;
}

static int qc_decap_run(QCJob *j)
{
    QCPkeyJob *p = (QCPkeyJob *)j;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_pkey(NULL, p->a, NULL);
    size_t len = 0;
    int ok = 0;
    if (!pctx || EVP_PKEY_decapsulate_init(pctx, NULL) != 1 || !qc_kem_op(pctx, p->a) ||
        EVP_PKEY_decapsulate(pctx, NULL, &len, p->in, p->in_len) != 1)
        goto done;
    j->out = malloc(len + 1);
    if (!j->out || EVP_PKEY_decapsulate(pctx, j->out, &len, p->in, p->in_len) != 1)
        goto done;
    j->out_len = len;
    ok = 1;
done:
    EVP_PKEY_CTX_free(pctx);
    return ok ? 0 : -1;
}

/* kemEncapsulate(pub, cb) */
static JSValue qc_kem_encapsulate(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QCPkeyJob *p = qc_job_new(ctx, sizeof(*p), qc_encap_run, qc_encap_done, qc_pkey_job_fini);
    if (!p)
        return JS_EXCEPTION;
    if (qc_key_up(ctx, qc_arg(argc, argv, 0), &p->a)) {
        qc_job_free(JS_GetRuntime(ctx), &p->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &p->j, qc_arg(argc, argv, 1));
}

/* kemDecapsulate(priv, ciphertext, cb) */
static JSValue qc_kem_decapsulate(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QCPkeyJob *p = qc_job_new(ctx, sizeof(*p), qc_decap_run, qc_done_buffer, qc_pkey_job_fini);
    if (!p)
        return JS_EXCEPTION;
    if (qc_key_up(ctx, qc_arg(argc, argv, 0), &p->a) ||
        qc_copy(ctx, qc_arg(argc, argv, 1), &p->in, &p->in_len, 0, NULL)) {
        qc_job_free(JS_GetRuntime(ctx), &p->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &p->j, qc_arg(argc, argv, 2));
}

/* ------------------------------------------------- classic DH and ECDH */

static JSValue qc_dh_group(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name = JS_ToCString(ctx, qc_arg(argc, argv, 0));
    BIGNUM *p;
    JSValue obj;
    uint8_t two = 2;
    if (!name)
        return JS_EXCEPTION;
    p = qc_dh_group_prime(name);
    JS_FreeCString(ctx, name);
    if (!p)
        return JS_NULL;
    obj = JS_NewObject(ctx);
    qb_set(ctx, obj, "prime", qc_bn_buffer(ctx, p, 0));
    qb_set(ctx, obj, "generator", qb_new_buffer(ctx, &two, 1));
    BN_free(p);
    return obj;
}

/* dhGeneratePrime(bits, generator): a safe prime suitable for 'generator' */
static JSValue qc_dh_generate_prime(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    int bits, gen;
    BIGNUM *p = NULL, *add = NULL, *rem = NULL;
    BN_CTX *bctx = NULL;
    JSValue ret = JS_EXCEPTION;
    if (JS_ToInt32(ctx, &bits, qc_arg(argc, argv, 0)) ||
        qc_int(ctx, qc_arg(argc, argv, 1), 2, &gen))
        return JS_EXCEPTION;
    if (bits < 2 || bits > 16384)
        return qc_throw(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE", "Invalid prime length");
    if (gen < 2)
        return qc_throw(ctx, JS_RANGE_ERROR, "ERR_OSSL_DH_BAD_GENERATOR",
                        "error:05000076:Diffie-Hellman routines::bad generator");
    p = BN_new();
    add = BN_new();
    rem = BN_new();
    bctx = BN_CTX_new();
    if (!p || !add || !rem || !bctx)
        goto ossl;
    if (gen == 2) {
        BN_set_word(add, 24);
        BN_set_word(rem, 23);
    } else if (gen == 5) {
        BN_set_word(add, 60);
        BN_set_word(rem, 59);
    } else {
        BN_set_word(add, 12);
        BN_set_word(rem, 11);
    }
    if (BN_generate_prime_ex2(p, bits, 1, add, rem, NULL, bctx) != 1)
        goto ossl;
    ret = qc_bn_buffer(ctx, p, 0);
    goto done;
ossl:
    qc_throw_ossl(ctx, "Prime generation failed");
done:
    BN_free(p);
    BN_free(add);
    BN_free(rem);
    BN_CTX_free(bctx);
    return ret;
}

#define QC_DH_CHECK_P_NOT_PRIME      0x01
#define QC_DH_CHECK_P_NOT_SAFE_PRIME 0x02
#define QC_DH_NOT_SUITABLE_GENERATOR 0x08
#define QC_DH_MODULUS_TOO_SMALL      0x80
#define QC_DH_MODULUS_TOO_LARGE      0x100

/* dhCheck(prime, generator): DH_check() flags (OpenSSL 3 semantics) */
static JSValue qc_dh_check(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    BIGNUM *p, *g, *t = NULL;
    BN_CTX *bctx = NULL;
    int flags = 0, r;

    p = qc_bn_arg(ctx, qc_arg(argc, argv, 0));
    if (!p)
        return JS_EXCEPTION;
    g = qc_bn_arg(ctx, qc_arg(argc, argv, 1));
    if (!g) {
        BN_free(p);
        return JS_EXCEPTION;
    }
    t = BN_new();
    bctx = BN_CTX_new();
    if (!t || !bctx) {
        flags = -1;
        goto done;
    }
    if (!BN_is_odd(p))
        flags |= QC_DH_CHECK_P_NOT_PRIME;
    if (BN_is_negative(g) || BN_is_zero(g) || BN_is_one(g))
        flags |= QC_DH_NOT_SUITABLE_GENERATOR;
    BN_copy(t, p);
    BN_sub_word(t, 1);
    if (BN_cmp(g, t) >= 0)
        flags |= QC_DH_NOT_SUITABLE_GENERATOR;
    if (BN_num_bits(p) < 512)
        flags |= QC_DH_MODULUS_TOO_SMALL;
    if (BN_num_bits(p) > 10000)
        flags |= QC_DH_MODULUS_TOO_LARGE;
    else {
        r = BN_check_prime(p, bctx, NULL);
        if (r == 0) {
            flags |= QC_DH_CHECK_P_NOT_PRIME;
        } else if (r == 1) {
            BN_rshift1(t, t);
            if (BN_check_prime(t, bctx, NULL) != 1)
                flags |= QC_DH_CHECK_P_NOT_SAFE_PRIME;
        }
    }
done:
    ERR_clear_error();
    BN_free(p);
    BN_free(g);
    BN_free(t);
    BN_CTX_free(bctx);
    return JS_NewInt32(ctx, flags);
}

/* dhGenerateKey(prime, generator, priv|null) -> { priv, pub } */
static JSValue qc_dh_generate_key(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    BIGNUM *p = NULL, *g = NULL, *priv = NULL, *pub = NULL, *range = NULL;
    BN_CTX *bctx = NULL;
    JSValue obj = JS_EXCEPTION;
    JSValueConst pv = qc_arg(argc, argv, 2);

    p = qc_bn_arg(ctx, qc_arg(argc, argv, 0));
    if (!p)
        return JS_EXCEPTION;
    g = qc_bn_arg(ctx, qc_arg(argc, argv, 1));
    if (!g)
        goto done;
    if (!JS_IsNull(pv) && !JS_IsUndefined(pv)) {
        priv = qc_bn_arg(ctx, pv);
        if (!priv)
            goto done;
    } else {
        priv = BN_new();
        range = BN_dup(p);
        if (!priv || !range || !BN_sub_word(range, 3))
            goto ossl;
        /* priv in [2, p - 2] */
        if (BN_is_negative(range) || BN_is_zero(range) || !BN_priv_rand_range(priv, range) ||
            !BN_add_word(priv, 2))
            goto ossl;
    }
    pub = BN_new();
    bctx = BN_CTX_new();
    if (!pub || !bctx)
        goto ossl;
    BN_set_flags(priv, BN_FLG_CONSTTIME);
    if (!BN_mod_exp(pub, g, priv, p, bctx))
        goto ossl;
    obj = JS_NewObject(ctx);
    qb_set(ctx, obj, "priv", qc_bn_buffer(ctx, priv, 0));
    qb_set(ctx, obj, "pub", qc_bn_buffer(ctx, pub, 0));
    goto done;
ossl:
    qc_throw_ossl(ctx, "Key generation failed");
done:
    BN_free(p);
    BN_free(g);
    BN_clear_free(priv);
    BN_free(pub);
    BN_free(range);
    BN_CTX_free(bctx);
    return obj;
}

/* dhCompute(prime, priv, peer) */
static JSValue qc_dh_compute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    BIGNUM *p = NULL, *priv = NULL, *peer = NULL, *t = NULL, *s = NULL;
    BN_CTX *bctx = NULL;
    JSValue ret = JS_EXCEPTION;

    p = qc_bn_arg(ctx, qc_arg(argc, argv, 0));
    if (!p)
        return JS_EXCEPTION;
    if (!(priv = qc_bn_arg(ctx, qc_arg(argc, argv, 1))) ||
        !(peer = qc_bn_arg(ctx, qc_arg(argc, argv, 2))))
        goto done;
    t = BN_dup(p);
    s = BN_new();
    bctx = BN_CTX_new();
    if (!t || !s || !bctx || !BN_sub_word(t, 1))
        goto ossl;
    if (BN_cmp(peer, BN_value_one()) <= 0) {
        qc_throw(ctx, JS_RANGE_ERROR, "ERR_CRYPTO_INVALID_KEYLEN", "Supplied key is too small");
        goto done;
    }
    if (BN_cmp(peer, t) >= 0) {
        qc_throw(ctx, JS_RANGE_ERROR, "ERR_CRYPTO_INVALID_KEYLEN", "Supplied key is too large");
        goto done;
    }
    BN_set_flags(priv, BN_FLG_CONSTTIME);
    if (!BN_mod_exp(s, peer, priv, p, bctx))
        goto ossl;
    ret = qc_bn_buffer(ctx, s, BN_num_bytes(p));
    goto done;
ossl:
    qc_throw_ossl(ctx, "Failed to compute the secret");
done:
    BN_free(p);
    BN_clear_free(priv);
    BN_free(peer);
    BN_free(t);
    BN_clear_free(s);
    BN_CTX_free(bctx);
    return ret;
}

typedef struct {
    EC_GROUP *g;
    BN_CTX *bctx;
    BIGNUM *order;
} QCEc;

static int qc_ec_open(JSContext *ctx, JSValueConst curve, QCEc *e)
{
    const char *name = JS_ToCString(ctx, curve);
    int nid;
    memset(e, 0, sizeof(*e));
    if (!name)
        return -1;
    nid = qc_curve_nid(name);
    JS_FreeCString(ctx, name);
    if (nid == NID_undef || !(e->g = EC_GROUP_new_by_curve_name(nid))) {
        ERR_clear_error();
        qc_throw(ctx, JS_TYPE_ERROR, "ERR_CRYPTO_INVALID_CURVE", "Invalid EC curve name");
        return -1;
    }
    e->bctx = BN_CTX_new();
    e->order = BN_new();
    if (!e->bctx || !e->order || !EC_GROUP_get_order(e->g, e->order, e->bctx)) {
        qc_throw_ossl(ctx, "EC initialization failed");
        return -1;
    }
    return 0;
}

static void qc_ec_close(QCEc *e)
{
    EC_GROUP_free(e->g);
    BN_CTX_free(e->bctx);
    BN_free(e->order);
}

static JSValue qc_ec_point_buffer(JSContext *ctx, QCEc *e, const EC_POINT *pt,
                                  point_conversion_form_t form)
{
    size_t n = EC_POINT_point2oct(e->g, pt, form, NULL, 0, e->bctx);
    uint8_t *out;
    if (n == 0)
        return qc_throw_ossl(ctx, "Failed to encode the EC point");
    out = malloc(n);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    if (EC_POINT_point2oct(e->g, pt, form, out, n, e->bctx) != n) {
        free(out);
        return qc_throw_ossl(ctx, "Failed to encode the EC point");
    }
    return qc_buffer_take(ctx, out, n);
}

/* ecdhGenerate(curve) -> [priv, pub] */
static JSValue qc_ecdh_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCEc e;
    BIGNUM *priv = NULL;
    EC_POINT *pt = NULL;
    JSValue vals[2], ret = JS_EXCEPTION;
    if (qc_ec_open(ctx, qc_arg(argc, argv, 0), &e))
        goto done;
    priv = BN_new();
    pt = EC_POINT_new(e.g);
    if (!priv || !pt)
        goto ossl;
    do {
        if (!BN_priv_rand_range(priv, e.order))
            goto ossl;
    } while (BN_is_zero(priv));
    if (!EC_POINT_mul(e.g, pt, priv, NULL, NULL, e.bctx))
        goto ossl;
    vals[0] = qc_bn_buffer(ctx, priv, 0);
    if (JS_IsException(vals[0]))
        goto done;
    vals[1] = qc_ec_point_buffer(ctx, &e, pt, POINT_CONVERSION_UNCOMPRESSED);
    if (JS_IsException(vals[1])) {
        JS_FreeValue(ctx, vals[0]);
        goto done;
    }
    ret = JS_NewArrayFrom(ctx, 2, vals);
    goto done;
ossl:
    qc_throw_ossl(ctx, "EC key generation failed");
done:
    BN_clear_free(priv);
    EC_POINT_free(pt);
    qc_ec_close(&e);
    return ret;
}

/* ecdhPublic(curve, priv) */
static JSValue qc_ecdh_public(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCEc e;
    BIGNUM *priv = NULL;
    EC_POINT *pt = NULL;
    JSValue ret = JS_EXCEPTION;
    if (qc_ec_open(ctx, qc_arg(argc, argv, 0), &e))
        goto done;
    priv = qc_bn_arg(ctx, qc_arg(argc, argv, 1));
    if (!priv)
        goto done;
    if (BN_is_zero(priv) || BN_cmp(priv, e.order) >= 0) {
        qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_INVALID_KEYTYPE",
                 "Private key is not valid for specified curve.");
        goto done;
    }
    pt = EC_POINT_new(e.g);
    if (!pt || !EC_POINT_mul(e.g, pt, priv, NULL, NULL, e.bctx)) {
        qc_throw_ossl(ctx, "Failed to generate ECDH public key");
        goto done;
    }
    ret = qc_ec_point_buffer(ctx, &e, pt, POINT_CONVERSION_UNCOMPRESSED);
done:
    BN_clear_free(priv);
    EC_POINT_free(pt);
    qc_ec_close(&e);
    return ret;
}

/* ecdhCompute(curve, priv, peer) */
static JSValue qc_ecdh_compute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCEc e;
    BIGNUM *priv = NULL, *x = NULL;
    EC_POINT *peer = NULL, *r = NULL;
    QCBuf pb = {0};
    JSValue ret = JS_EXCEPTION;
    if (qc_ec_open(ctx, qc_arg(argc, argv, 0), &e))
        goto done;
    priv = qc_bn_arg(ctx, qc_arg(argc, argv, 1));
    if (!priv || qc_in(ctx, qc_arg(argc, argv, 2), &pb))
        goto done;
    peer = EC_POINT_new(e.g);
    r = EC_POINT_new(e.g);
    x = BN_new();
    if (!peer || !r || !x)
        goto done;
    if (EC_POINT_oct2point(e.g, peer, pb.p, pb.len, e.bctx) != 1 ||
        EC_POINT_is_on_curve(e.g, peer, e.bctx) != 1 || EC_POINT_is_at_infinity(e.g, peer)) {
        ERR_clear_error();
        qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY",
                 "Public key is not valid for specified curve");
        goto done;
    }
    if (!EC_POINT_mul(e.g, r, NULL, peer, priv, e.bctx) ||
        !EC_POINT_get_affine_coordinates(e.g, r, x, NULL, e.bctx)) {
        qc_throw_ossl(ctx, "Failed to compute ECDH key");
        goto done;
    }
    ret = qc_bn_buffer(ctx, x, (EC_GROUP_get_degree(e.g) + 7) / 8);
done:
    qc_in_free(ctx, &pb);
    BN_clear_free(priv);
    BN_clear_free(x);
    EC_POINT_free(peer);
    EC_POINT_free(r);
    qc_ec_close(&e);
    return ret;
}

/* ecdhConvert(curve, point, form) */
static JSValue qc_ecdh_convert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCEc e;
    EC_POINT *pt = NULL;
    QCBuf pb = {0};
    const char *form = NULL;
    JSValue ret = JS_EXCEPTION;
    if (qc_ec_open(ctx, qc_arg(argc, argv, 0), &e))
        goto done;
    if (qc_in(ctx, qc_arg(argc, argv, 1), &pb))
        goto done;
    form = JS_ToCString(ctx, qc_arg(argc, argv, 2));
    if (!form)
        goto done;
    pt = EC_POINT_new(e.g);
    if (!pt || EC_POINT_oct2point(e.g, pt, pb.p, pb.len, e.bctx) != 1) {
        ERR_clear_error();
        qc_throw(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY",
                 "Public key is not valid for specified curve");
        goto done;
    }
    ret = qc_ec_point_buffer(ctx, &e, pt, qc_point_form(form));
done:
    qc_cstr_free(ctx, form);
    qc_in_free(ctx, &pb);
    EC_POINT_free(pt);
    qc_ec_close(&e);
    return ret;
}

/* --------------------------------------------------------------- X.509 */

static JSClassID qc_x509_class_id;

typedef struct {
    X509 *x;
} QCX509;

static void qc_x509_finalizer(JSRuntime *rt, JSValue val)
{
    QCX509 *c = JS_GetOpaque(val, qc_x509_class_id);
    if (c) {
        X509_free(c->x);
        js_free_rt(rt, c);
    }
}

static JSClassDef qc_x509_class = {"X509", .finalizer = qc_x509_finalizer};

/* wrap 'x' (taken) */
static JSValue qc_x509_wrap(JSContext *ctx, X509 *x)
{
    QCX509 *c;
    JSValue obj = JS_NewObjectClass(ctx, qc_x509_class_id);
    if (JS_IsException(obj)) {
        X509_free(x);
        return obj;
    }
    c = js_mallocz(ctx, sizeof(*c));
    if (!c) {
        X509_free(x);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    c->x = x;
    JS_SetOpaque(obj, c);
    return obj;
}

static X509 *qc_x509_get(JSContext *ctx, JSValueConst v)
{
    QCX509 *c = JS_GetOpaque2(ctx, v, qc_x509_class_id);
    return c ? c->x : NULL;
}

/* x509Parse(data) */
static JSValue qc_x509_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCBuf b;
    X509 *x = NULL;
    if (qc_in(ctx, qc_arg(argc, argv, 0), &b))
        return JS_EXCEPTION;
    ERR_clear_error();
    if (qc_find(b.p, b.len, "-----BEGIN"))
        x = qc_x509_from_pem(b.p, b.len);
    if (!x) {
        const unsigned char *p = b.p;
        ERR_clear_error();
        x = d2i_X509(NULL, &p, (long)b.len);
    }
    qc_in_free(ctx, &b);
    if (!x)
        return qc_throw_ossl(ctx, "Failed to parse the certificate");
    return qc_x509_wrap(ctx, x);
}

static JSValue qc_bio_string(JSContext *ctx, BIO *bio)
{
    char *data = NULL;
    long n = BIO_get_mem_data(bio, &data);
    return JS_NewStringLen(ctx, data ? data : "", n > 0 ? (size_t)n : 0);
}

static JSValue qc_name_string(JSContext *ctx, const X509_NAME *name)
{
    BIO *bio = BIO_new(BIO_s_mem());
    JSValue v;
    if (!bio)
        return JS_ThrowOutOfMemory(ctx);
    X509_NAME_print_ex(bio, name, 0,
                       ASN1_STRFLGS_ESC_2253 | ASN1_STRFLGS_ESC_CTRL | ASN1_STRFLGS_UTF8_CONVERT |
                           XN_FLAG_SEP_MULTILINE | XN_FLAG_FN_SN);
    v = qc_bio_string(ctx, bio);
    BIO_free(bio);
    return v;
}

static int qc_is_string_type(int t)
{
    switch (t) {
    case V_ASN1_UTF8STRING:
    case V_ASN1_PRINTABLESTRING:
    case V_ASN1_IA5STRING:
    case V_ASN1_T61STRING:
    case V_ASN1_BMPSTRING:
    case V_ASN1_UNIVERSALSTRING:
    case V_ASN1_VISIBLESTRING:
    case V_ASN1_NUMERICSTRING:
        return 1;
    default:
        return 0;
    }
}

static JSValue qc_asn1_utf8(JSContext *ctx, const ASN1_STRING *s)
{
    unsigned char *u = NULL;
    int n = ASN1_STRING_to_UTF8(&u, s);
    JSValue v;
    if (n < 0) {
        ERR_clear_error();
        return JS_UNDEFINED;
    }
    v = JS_NewStringLen(ctx, (const char *)u, n);
    OPENSSL_free(u);
    return v;
}

/* { raw, rdns } and the legacy { SN: value | [values] } object of a name */
static void qc_name_objects(JSContext *ctx, const X509_NAME *name, JSValue *pinfo, JSValue *plegacy)
{
    const unsigned char *der = NULL;
    size_t derlen = 0;
    int i, n = X509_NAME_entry_count(name), last_set = -1;
    JSValue info = JS_NewObject(ctx), rdns = JS_NewArray(ctx), rdn = JS_UNDEFINED;
    JSValue legacy = JS_NewObject(ctx);
    uint32_t nrdn = 0, nent = 0;
    char oid[128];

    if (X509_NAME_get0_der(name, &der, &derlen) == 1)
        qb_set(ctx, info, "raw", qb_new_buffer(ctx, der, derlen));
    for (i = 0; i < n; i++) {
        const X509_NAME_ENTRY *ne = X509_NAME_get_entry(name, i);
        const ASN1_OBJECT *obj = X509_NAME_ENTRY_get_object(ne);
        const ASN1_STRING *data = X509_NAME_ENTRY_get_data(ne);
        int set = X509_NAME_ENTRY_set(ne);
        JSValue ent, val = JS_UNDEFINED;
        const char *sn;
        if (set != last_set || JS_IsUndefined(rdn)) {
            if (!JS_IsUndefined(rdn))
                JS_SetPropertyUint32(ctx, rdns, nrdn++, rdn);
            rdn = JS_NewArray(ctx);
            nent = 0;
            last_set = set;
        }
        OBJ_obj2txt(oid, sizeof(oid), obj, 1);
        ent = JS_NewObject(ctx);
        qb_set(ctx, ent, "oid", JS_NewString(ctx, oid));
        if (qc_is_string_type(ASN1_STRING_type(data))) {
            val = qc_asn1_utf8(ctx, data);
            if (!JS_IsUndefined(val))
                qb_set(ctx, ent, "value", JS_DupValue(ctx, val));
        }
        JS_SetPropertyUint32(ctx, rdn, nent++, ent);
        /* legacy object, keyed by short name */
        sn = OBJ_nid2sn(OBJ_obj2nid(obj));
        if (!JS_IsUndefined(val)) {
            const char *key = sn ? sn : oid;
            JSValue prev = JS_GetPropertyStr(ctx, legacy, key);
            if (JS_IsUndefined(prev)) {
                JS_SetPropertyStr(ctx, legacy, key, JS_DupValue(ctx, val));
            } else if (JS_IsArray(ctx, prev) > 0) {
                int64_t len = 0;
                JS_GetLength(ctx, prev, &len);
                JS_SetPropertyUint32(ctx, prev, (uint32_t)len, JS_DupValue(ctx, val));
            } else {
                JSValue arr[2];
                arr[0] = JS_DupValue(ctx, prev);
                arr[1] = JS_DupValue(ctx, val);
                JS_SetPropertyStr(ctx, legacy, key, JS_NewArrayFrom(ctx, 2, arr));
            }
            JS_FreeValue(ctx, prev);
        }
        JS_FreeValue(ctx, val);
    }
    if (!JS_IsUndefined(rdn))
        JS_SetPropertyUint32(ctx, rdns, nrdn++, rdn);
    qb_set(ctx, info, "rdns", rdns);
    *pinfo = info;
    *plegacy = legacy;
}

static JSValue qc_ext_string(JSContext *ctx, X509 *x, int nid)
{
    int idx = X509_get_ext_by_NID(x, nid, -1);
    const X509_EXTENSION *ext;
    BIO *bio;
    JSValue v;
    if (idx < 0)
        return JS_UNDEFINED;
    ext = X509_get_ext(x, idx);
    bio = BIO_new(BIO_s_mem());
    if (!bio)
        return JS_ThrowOutOfMemory(ctx);
    if (X509V3_EXT_print(bio, ext, 0, 0) != 1) {
        BIO_free(bio);
        ERR_clear_error();
        return JS_UNDEFINED;
    }
    if (nid == NID_info_access) {
        char *data = NULL;
        long n = BIO_get_mem_data(bio, &data);
        if (n > 0 && data[n - 1] != '\n')
            BIO_write(bio, "\n", 1);
    }
    v = qc_bio_string(ctx, bio);
    BIO_free(bio);
    return v;
}

static JSValue qc_time_string(JSContext *ctx, const ASN1_TIME *t)
{
    BIO *bio = BIO_new(BIO_s_mem());
    JSValue v;
    if (!bio)
        return JS_ThrowOutOfMemory(ctx);
    ASN1_TIME_print(bio, t);
    v = qc_bio_string(ctx, bio);
    BIO_free(bio);
    return v;
}

static double qc_time_ms(const ASN1_TIME *t)
{
    struct tm tm;
    int64_t y, m, d, era, yoe, doy, doe, days;
    memset(&tm, 0, sizeof(tm));
    if (ASN1_TIME_to_tm(t, &tm) != 1)
        return 0;
    /* days from civil (proleptic Gregorian) */
    y = tm.tm_year + 1900;
    m = tm.tm_mon + 1;
    d = tm.tm_mday;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return ((double)days * 86400.0 + tm.tm_hour * 3600.0 + tm.tm_min * 60.0 + tm.tm_sec) * 1000.0;
}

static JSValue qc_fingerprint(JSContext *ctx, X509 *x, const char *mdname)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0, i;
    char hex[EVP_MAX_MD_SIZE * 3 + 1];
    static const char digits[] = "0123456789ABCDEF";
    EVP_MD *type = EVP_MD_fetch(NULL, mdname, NULL);
    int ok = type && X509_digest(x, type, md, &n) == 1;
    EVP_MD_free(type);
    if (!ok) {
        ERR_clear_error();
        return JS_UNDEFINED;
    }
    for (i = 0; i < n; i++) {
        hex[3 * i] = digits[md[i] >> 4];
        hex[3 * i + 1] = digits[md[i] & 15];
        hex[3 * i + 2] = ':';
    }
    hex[n ? 3 * n - 1 : 0] = 0;
    return JS_NewString(ctx, hex);
}

/* the dotted OID of a DER encoded OBJECT IDENTIFIER body */
static void qc_oid_text(const uint8_t *p, size_t len, char *buf, size_t size)
{
    size_t i, pos = 0;
    uint64_t v = 0;
    int first = 1;
    buf[0] = 0;
    for (i = 0; i < len; i++) {
        v = (v << 7) | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) {
            int n;
            if (first) {
                uint64_t a = v < 40 ? 0 : v < 80 ? 1 : 2;
                n = snprintf(buf + pos, size - pos, "%llu.%llu", (unsigned long long)a,
                             (unsigned long long)(v - a * 40));
                first = 0;
            } else {
                n = snprintf(buf + pos, size - pos, ".%llu", (unsigned long long)v);
            }
            if (n < 0 || (size_t)n >= size - pos)
                return;
            pos += n;
            v = 0;
        }
    }
}

/* tbs, signature algorithm and signature value from the DER encoding */
static void qc_x509_der_parts(JSContext *ctx, JSValueConst info, const uint8_t *der, size_t len)
{
    const uint8_t *p = der, *end = der + len, *tbs, *alg, *algend, *q;
    size_t l;
    JSValue sig, salg;
    char oid[256];

    if (p >= end || *p++ != 0x30 || qc_der_len(&p, end, &l))
        return;
    end = p + l;
    /* TBSCertificate */
    tbs = p;
    if (p >= end || *p++ != 0x30 || qc_der_len(&p, end, &l))
        return;
    p += l;
    qb_set(ctx, info, "tbs", qb_new_buffer(ctx, tbs, p - tbs));
    /* signatureAlgorithm */
    if (p >= end || *p++ != 0x30 || qc_der_len(&p, end, &l))
        return;
    alg = p;
    algend = p + l;
    p = algend;
    q = alg;
    salg = JS_NewObject(ctx);
    if (q < algend && *q == 0x06) {
        q++;
        if (!qc_der_len(&q, algend, &l)) {
            qc_oid_text(q, l, oid, sizeof(oid));
            qb_set(ctx, salg, "oid", JS_NewString(ctx, oid));
            qb_set(ctx, info, "signatureAlgorithmOid", JS_NewString(ctx, oid));
            q += l;
            if (q < algend)
                qb_set(ctx, salg, "parameters", qb_new_buffer(ctx, q, algend - q));
        }
    }
    sig = JS_NewObject(ctx);
    qb_set(ctx, sig, "algorithm", salg);
    /* signatureValue BIT STRING */
    if (p < end && *p == 0x03) {
        p++;
        if (!qc_der_len(&p, end, &l) && l >= 1)
            qb_set(ctx, sig, "value", qb_new_buffer(ctx, p + 1, l - 1));
    }
    qb_set(ctx, info, "signature", sig);
}

static JSValue qc_x509_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    X509 *x = qc_x509_get(ctx, this_val);
    JSValue info, v, sn_info, sn_legacy, in_info, in_legacy;
    unsigned char *der = NULL;
    int derlen, i, n, crit = 0;
    BIO *bio;
    BIGNUM *serial;
    EXTENDED_KEY_USAGE *eku;
    int signid;
    char oid[128];

    if (!x)
        return JS_EXCEPTION;
    info = JS_NewObject(ctx);
    if (JS_IsException(info))
        return info;
    qb_set(ctx, info, "subject", qc_name_string(ctx, X509_get_subject_name(x)));
    qb_set(ctx, info, "issuer", qc_name_string(ctx, X509_get_issuer_name(x)));
    qc_name_objects(ctx, X509_get_subject_name(x), &sn_info, &sn_legacy);
    qc_name_objects(ctx, X509_get_issuer_name(x), &in_info, &in_legacy);
    qb_set(ctx, info, "subjectName", sn_info);
    qb_set(ctx, info, "issuerName", in_info);
    qb_set(ctx, info, "subjectObject", sn_legacy);
    qb_set(ctx, info, "issuerObject", in_legacy);
    v = qc_ext_string(ctx, x, NID_subject_alt_name);
    if (!JS_IsUndefined(v))
        qb_set(ctx, info, "subjectAltName", v);
    v = qc_ext_string(ctx, x, NID_info_access);
    if (!JS_IsUndefined(v))
        qb_set(ctx, info, "infoAccess", v);
    qb_set(ctx, info, "validFrom", qc_time_string(ctx, X509_get0_notBefore(x)));
    qb_set(ctx, info, "validTo", qc_time_string(ctx, X509_get0_notAfter(x)));
    qb_set(ctx, info, "validFromDate", JS_NewFloat64(ctx, qc_time_ms(X509_get0_notBefore(x))));
    qb_set(ctx, info, "validToDate", JS_NewFloat64(ctx, qc_time_ms(X509_get0_notAfter(x))));
    qb_set(ctx, info, "fingerprint", qc_fingerprint(ctx, x, "SHA1"));
    qb_set(ctx, info, "fingerprint256", qc_fingerprint(ctx, x, "SHA256"));
    qb_set(ctx, info, "fingerprint512", qc_fingerprint(ctx, x, "SHA512"));
    serial = ASN1_INTEGER_to_BN(X509_get0_serialNumber(x), NULL);
    if (serial) {
        char *hex = BN_bn2hex(serial);
        if (hex) {
            qb_set(ctx, info, "serialNumber", JS_NewString(ctx, hex));
            OPENSSL_free(hex);
        }
        BN_free(serial);
    }
    eku = X509_get_ext_d2i(x, NID_ext_key_usage, &crit, NULL);
    if (eku) {
        JSValue arr = JS_NewArray(ctx);
        n = sk_ASN1_OBJECT_num(eku);
        for (i = 0; i < n; i++) {
            OBJ_obj2txt(oid, sizeof(oid), sk_ASN1_OBJECT_value(eku, i), 1);
            JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, oid));
        }
        sk_ASN1_OBJECT_pop_free(eku, ASN1_OBJECT_free);
        qb_set(ctx, info, "keyUsage", arr);
    }
    ERR_clear_error();
    qb_set(ctx, info, "ca", JS_NewBool(ctx, X509_check_ca(x) == 1));
    derlen = i2d_X509(x, &der);
    if (derlen > 0) {
        qb_set(ctx, info, "raw", qb_new_buffer(ctx, der, derlen));
        qc_x509_der_parts(ctx, info, der, derlen);
        OPENSSL_free(der);
    }
    bio = BIO_new(BIO_s_mem());
    if (bio) {
        PEM_write_bio_X509(bio, x);
        qb_set(ctx, info, "pem", qc_bio_string(ctx, bio));
        BIO_free(bio);
    }
    signid = X509_get_signature_nid(x);
    if (signid != NID_undef && OBJ_nid2ln(signid))
        qb_set(ctx, info, "signatureAlgorithm", JS_NewString(ctx, OBJ_nid2ln(signid)));
    qb_set(ctx, info, "version", JS_NewInt32(ctx, (int)X509_get_version(x) + 1));
    /* extensions */
    {
        JSValue arr = JS_NewArray(ctx);
        n = X509_get_ext_count(x);
        for (i = 0; i < n; i++) {
            const X509_EXTENSION *ext = X509_get_ext(x, i);
            const ASN1_OCTET_STRING *data = X509_EXTENSION_get_data(ext);
            JSValue e = JS_NewObject(ctx);
            OBJ_obj2txt(oid, sizeof(oid), X509_EXTENSION_get_object(ext), 1);
            qb_set(ctx, e, "oid", JS_NewString(ctx, oid));
            qb_set(ctx, e, "critical", JS_NewBool(ctx, X509_EXTENSION_get_critical(ext) > 0));
            qb_set(ctx, e, "value",
                   qb_new_buffer(ctx, ASN1_STRING_get0_data(data),
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
                                 ASN1_STRING_get_length(data)));
#else
                                 ASN1_STRING_length(data)));
#endif
            JS_SetPropertyUint32(ctx, arr, i, e);
        }
        qb_set(ctx, info, "extensions", arr);
    }
    ERR_clear_error();
    return info;
}

static JSValue qc_x509_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    X509 *x = qc_x509_get(ctx, this_val);
    EVP_PKEY *pkey;
    if (!x)
        return JS_EXCEPTION;
    pkey = X509_get0_pubkey(x);
    if (!pkey)
        return qc_throw_ossl(ctx, "Failed to get the public key");
    EVP_PKEY_up_ref(pkey);
    return qc_key_wrap(ctx, pkey, 0);
}

static JSValue qc_x509_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    X509 *x = qc_x509_get(ctx, this_val);
    QCKey *k;
    int r;
    if (!x || !(k = qc_key_get(ctx, qc_arg(argc, argv, 0))))
        return JS_EXCEPTION;
    r = X509_verify(x, k->pkey);
    ERR_clear_error();
    return JS_NewBool(ctx, r == 1);
}

static JSValue qc_x509_check_issued(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    X509 *x = qc_x509_get(ctx, this_val), *issuer;
    int r;
    if (!x || !(issuer = qc_x509_get(ctx, qc_arg(argc, argv, 0))))
        return JS_EXCEPTION;
    r = X509_check_issued(issuer, x);
    ERR_clear_error();
    return JS_NewBool(ctx, r == X509_V_OK);
}

static JSValue qc_x509_check_private_key(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv)
{
    X509 *x = qc_x509_get(ctx, this_val);
    QCKey *k;
    int r;
    if (!x || !(k = qc_key_get(ctx, qc_arg(argc, argv, 0))))
        return JS_EXCEPTION;
    r = X509_check_private_key(x, k->pkey);
    ERR_clear_error();
    return JS_NewBool(ctx, r == 1);
}

static const JSCFunctionListEntry qc_x509_proto_funcs[] = {
    JS_CFUNC_DEF("info", 0, qc_x509_info),
    JS_CFUNC_DEF("publicKey", 0, qc_x509_public_key),
    JS_CFUNC_DEF("verify", 1, qc_x509_verify),
    JS_CFUNC_DEF("checkIssued", 1, qc_x509_check_issued),
    JS_CFUNC_DEF("checkPrivateKey", 1, qc_x509_check_private_key),
};

/* parsePKCS12(der, passphrase) */
static JSValue qc_parse_pkcs12(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCBuf b;
    uint8_t *pass = NULL;
    size_t plen = 0;
    int is_null;
    BIO *bio;
    PKCS12 *p12 = NULL;
    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *ca = NULL;
    JSValue obj = JS_EXCEPTION, arr;
    int i, n;

    if (qc_copy(ctx, qc_arg(argc, argv, 1), &pass, &plen, 1, &is_null))
        return JS_EXCEPTION;
    if (qc_in(ctx, qc_arg(argc, argv, 0), &b)) {
        qc_clear_free(pass, plen);
        return JS_EXCEPTION;
    }
    bio = BIO_new_mem_buf(b.p, (int)b.len);
    if (bio)
        p12 = d2i_PKCS12_bio(bio, NULL);
    BIO_free(bio);
    qc_in_free(ctx, &b);
    if (!p12 || !PKCS12_parse(p12, pass ? (const char *)pass : NULL, &pkey, &cert, &ca)) {
        qc_throw_ossl(ctx, "Failed to parse the PKCS#12 bundle");
        goto done;
    }
    obj = JS_NewObject(ctx);
    qb_set(ctx, obj, "key", pkey ? qc_key_wrap(ctx, pkey, 1) : JS_NULL);
    pkey = NULL;
    qb_set(ctx, obj, "cert", cert ? qc_x509_wrap(ctx, cert) : JS_NULL);
    cert = NULL;
    arr = JS_NewArray(ctx);
    n = ca ? sk_X509_num(ca) : 0;
    for (i = 0; i < n; i++) {
        X509 *c = sk_X509_value(ca, i);
        X509_up_ref(c);
        JS_SetPropertyUint32(ctx, arr, i, qc_x509_wrap(ctx, c));
    }
    qb_set(ctx, obj, "ca", arr);
done:
    ERR_clear_error();
    EVP_PKEY_free(pkey);
    X509_free(cert);
    if (ca)
        sk_X509_pop_free(ca, X509_free);
    PKCS12_free(p12);
    qc_clear_free(pass, plen);
    return obj;
}

/* SPKAC (Netscape SPKI, base64 text) */
static NETSCAPE_SPKI *qc_spkac(JSContext *ctx, JSValueConst v)
{
    QCBuf b;
    size_t len;
    NETSCAPE_SPKI *spki;
    if (qc_in(ctx, v, &b))
        return NULL;
    len = b.len;
    while (len > 0 && (b.p[len - 1] == '\n' || b.p[len - 1] == '\r' || b.p[len - 1] == ' ' ||
                       b.p[len - 1] == '\t'))
        len--;
    spki = len ? NETSCAPE_SPKI_b64_decode((const char *)b.p, (int)len) : NULL;
    qc_in_free(ctx, &b);
    ERR_clear_error();
    return spki;
}

static JSValue qc_spkac_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NETSCAPE_SPKI *spki;
    EVP_PKEY *pkey;
    int r = 0;
    spki = qc_spkac(ctx, qc_arg(argc, argv, 0));
    if (!spki)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
    pkey = NETSCAPE_SPKI_get_pubkey(spki);
    if (pkey) {
        r = NETSCAPE_SPKI_verify(spki, pkey) > 0;
        EVP_PKEY_free(pkey);
    }
    NETSCAPE_SPKI_free(spki);
    ERR_clear_error();
    return JS_NewBool(ctx, r);
}

static JSValue qc_spkac_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    NETSCAPE_SPKI *spki = qc_spkac(ctx, qc_arg(argc, argv, 0));
    EVP_PKEY *pkey;
    BIO *bio;
    JSValue v = JS_NULL;
    char *data = NULL;
    long n;
    if (!spki)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NULL;
    pkey = NETSCAPE_SPKI_get_pubkey(spki);
    NETSCAPE_SPKI_free(spki);
    if (!pkey) {
        ERR_clear_error();
        return JS_NULL;
    }
    bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_PUBKEY(bio, pkey) == 1) {
        n = BIO_get_mem_data(bio, &data);
        v = qb_new_buffer(ctx, (const uint8_t *)data, n > 0 ? n : 0);
    }
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    ERR_clear_error();
    return v;
}

static JSValue qc_spkac_challenge(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    NETSCAPE_SPKI *spki = qc_spkac(ctx, qc_arg(argc, argv, 0));
    JSValue v = JS_NULL;
    unsigned char *u = NULL;
    int n;
    if (!spki)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NULL;
    if (spki->spkac && spki->spkac->challenge) {
        n = ASN1_STRING_to_UTF8(&u, spki->spkac->challenge);
        if (n >= 0) {
            v = qb_new_buffer(ctx, u, n);
            OPENSSL_free(u);
        }
    }
    NETSCAPE_SPKI_free(spki);
    ERR_clear_error();
    return v;
}

/* ------------------------------------------------------------- primes */

typedef struct {
    QCJob j;
    int bits, safe, checks;
    uint8_t *add, *rem, *cand;
    size_t add_len, rem_len, cand_len;
} QCPrimeJob;

static void qc_prime_fini(QCJob *j)
{
    QCPrimeJob *p = (QCPrimeJob *)j;
    free(p->add);
    free(p->rem);
    free(p->cand);
}

static int qc_gen_prime_run(QCJob *j)
{
    QCPrimeJob *p = (QCPrimeJob *)j;
    BIGNUM *prime = BN_new(), *add = NULL, *rem = NULL;
    BN_CTX *bctx = BN_CTX_new();
    int ok = 0, n;
    if (!prime || !bctx)
        goto done;
    if (p->add && !(add = BN_bin2bn(p->add, (int)p->add_len, NULL)))
        goto done;
    if (p->rem && !(rem = BN_bin2bn(p->rem, (int)p->rem_len, NULL)))
        goto done;
    if (BN_generate_prime_ex2(prime, p->bits, p->safe, add, rem, NULL, bctx) != 1) {
        qc_job_fail(j, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE", "invalid options.add / options.rem");
        goto done;
    }
    n = BN_num_bytes(prime);
    j->out = malloc(n + 1);
    if (!j->out)
        goto done;
    BN_bn2binpad(prime, j->out, n);
    j->out_len = n;
    ok = 1;
done:
    BN_free(prime);
    BN_free(add);
    BN_free(rem);
    BN_CTX_free(bctx);
    return ok ? 0 : -1;
}

/* generatePrime(bits, safe, add, rem, cb) */
static JSValue qc_generate_prime(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QCPrimeJob *p = qc_job_new(ctx, sizeof(*p), qc_gen_prime_run, qc_done_buffer, qc_prime_fini);
    if (!p)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &p->bits, qc_arg(argc, argv, 0)) ||
        (p->safe = JS_ToBool(ctx, qc_arg(argc, argv, 1))) < 0 ||
        qc_copy(ctx, qc_arg(argc, argv, 2), &p->add, &p->add_len, 1, NULL) ||
        qc_copy(ctx, qc_arg(argc, argv, 3), &p->rem, &p->rem_len, 1, NULL)) {
        qc_job_free(JS_GetRuntime(ctx), &p->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &p->j, qc_arg(argc, argv, 4));
}

static int qc_check_prime_run(QCJob *j)
{
    QCPrimeJob *p = (QCPrimeJob *)j;
    BIGNUM *c = BN_bin2bn(p->cand, (int)p->cand_len, NULL);
    BN_CTX *bctx = BN_CTX_new();
    int r = -1;
    if (c && bctx)
        r = BN_check_prime(c, bctx, NULL);
    BN_free(c);
    BN_CTX_free(bctx);
    if (r < 0)
        return -1;
    j->result = r == 1;
    return 0;
}

/* checkPrime(candidate, checks, cb) */
static JSValue qc_check_prime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QCPrimeJob *p = qc_job_new(ctx, sizeof(*p), qc_check_prime_run, qc_done_bool, qc_prime_fini);
    if (!p)
        return JS_EXCEPTION;
    if (qc_copy(ctx, qc_arg(argc, argv, 0), &p->cand, &p->cand_len, 0, NULL) ||
        qc_int(ctx, qc_arg(argc, argv, 1), 0, &p->checks)) {
        qc_job_free(JS_GetRuntime(ctx), &p->j);
        return JS_EXCEPTION;
    }
    return qc_job_start(ctx, &p->j, qc_arg(argc, argv, 2));
}

/* ------------------------------------------------------------- module */

static const JSCFunctionListEntry qc_crypto_funcs[] = {
    /* digests and MACs */
    JS_CFUNC_DEF("hashNew", 2, qc_hash_new),
    JS_CFUNC_DEF("hmacNew", 2, qc_hmac_new),
    JS_CFUNC_DEF("macNew", 3, qc_mac_new),
    JS_CFUNC_DEF("hashOnce", 3, qc_hash_once),
    JS_CFUNC_DEF("digestInfo", 1, qc_digest_info),
    JS_CFUNC_DEF("getHashes", 0, qc_get_hashes),
    JS_CFUNC_DEF("getCiphers", 0, qc_get_ciphers),
    JS_CFUNC_DEF("getCurves", 0, qc_get_curves),
    JS_CFUNC_DEF("getMacs", 0, qc_get_macs),
    /* random */
    JS_CFUNC_DEF("randomFill", 3, qc_random_fill),
    JS_CFUNC_DEF("randomBytes", 2, qc_random_bytes),
    JS_CFUNC_DEF("timingSafeEqual", 2, qc_timing_safe_equal),
    /* ciphers */
    JS_CFUNC_DEF("cipherNew", 5, qc_cipher_new),
    JS_CFUNC_DEF("cipherInfo", 3, qc_cipher_info),
    /* KDFs */
    JS_CFUNC_DEF("pbkdf2", 6, qc_pbkdf2),
    JS_CFUNC_DEF("scrypt", 8, qc_scrypt),
    JS_CFUNC_DEF("hkdf", 6, qc_hkdf),
    JS_CFUNC_DEF("argon2", 10, qc_argon2),
    JS_CFUNC_DEF("bcrypt", 4, qc_bcrypt),
    /* asymmetric keys */
    JS_CFUNC_DEF("keyParse", 5, qc_key_parse),
    JS_CFUNC_DEF("keyFromJwk", 3, qc_key_from_jwk),
    JS_CFUNC_DEF("keyFromRaw", 3, qc_key_from_raw),
    JS_CFUNC_DEF("keyFromEcPoint", 2, qc_key_from_ec_point),
    JS_CFUNC_DEF("keyFromEcPrivate", 2, qc_key_from_ec_private),
    JS_CFUNC_DEF("generateKeyPair", 3, qc_generate_key_pair),
    JS_CFUNC_DEF("sign", 8, qc_sign),
    JS_CFUNC_DEF("verify", 9, qc_verify),
    JS_CFUNC_DEF("pkeyCrypt", 6, qc_pkey_crypt),
    JS_CFUNC_DEF("pkeyDerive", 3, qc_pkey_derive),
    JS_CFUNC_DEF("kemEncapsulate", 2, qc_kem_encapsulate),
    JS_CFUNC_DEF("kemDecapsulate", 3, qc_kem_decapsulate),
    /* classic DH / ECDH */
    JS_CFUNC_DEF("dhGroup", 1, qc_dh_group),
    JS_CFUNC_DEF("dhGeneratePrime", 2, qc_dh_generate_prime),
    JS_CFUNC_DEF("dhCheck", 2, qc_dh_check),
    JS_CFUNC_DEF("dhGenerateKey", 3, qc_dh_generate_key),
    JS_CFUNC_DEF("dhCompute", 3, qc_dh_compute),
    JS_CFUNC_DEF("ecdhGenerate", 1, qc_ecdh_generate),
    JS_CFUNC_DEF("ecdhPublic", 2, qc_ecdh_public),
    JS_CFUNC_DEF("ecdhCompute", 3, qc_ecdh_compute),
    JS_CFUNC_DEF("ecdhConvert", 3, qc_ecdh_convert),
    /* X.509 */
    JS_CFUNC_DEF("x509Parse", 1, qc_x509_parse),
    JS_CFUNC_DEF("parsePKCS12", 2, qc_parse_pkcs12),
    JS_CFUNC_DEF("spkacVerify", 1, qc_spkac_verify),
    JS_CFUNC_DEF("spkacPublicKey", 1, qc_spkac_public_key),
    JS_CFUNC_DEF("spkacChallenge", 1, qc_spkac_challenge),
    /* primes */
    JS_CFUNC_DEF("generatePrime", 5, qc_generate_prime),
    JS_CFUNC_DEF("checkPrime", 3, qc_check_prime),
    /* Bun.hash */
    JS_CFUNC_MAGIC_DEF("wyhash", 2, qc_fast_hash, QC_H_WYHASH),
    JS_CFUNC_MAGIC_DEF("adler32", 1, qc_fast_hash, QC_H_ADLER32),
    JS_CFUNC_MAGIC_DEF("crc32", 2, qc_fast_hash, QC_H_CRC32),
    JS_CFUNC_MAGIC_DEF("cityHash32", 1, qc_fast_hash, QC_H_CITY32),
    JS_CFUNC_MAGIC_DEF("cityHash64", 2, qc_fast_hash, QC_H_CITY64),
    JS_CFUNC_MAGIC_DEF("xxHash32", 2, qc_fast_hash, QC_H_XX32),
    JS_CFUNC_MAGIC_DEF("xxHash64", 2, qc_fast_hash, QC_H_XX64),
    JS_CFUNC_MAGIC_DEF("xxHash3", 2, qc_fast_hash, QC_H_XX3),
    JS_CFUNC_MAGIC_DEF("murmur32v2", 2, qc_fast_hash, QC_H_MUR32V2),
    JS_CFUNC_MAGIC_DEF("murmur32v3", 2, qc_fast_hash, QC_H_MUR32V3),
    JS_CFUNC_MAGIC_DEF("murmur64v2", 2, qc_fast_hash, QC_H_MUR64V2),
    JS_CFUNC_MAGIC_DEF("rapidhash", 2, qc_fast_hash, QC_H_RAPID),
    JS_PROP_STRING_DEF("opensslVersion", OPENSSL_VERSION_STR, 0),
};

static void qc_register_class(JSContext *ctx, JSClassID *pid, const JSClassDef *def,
                              const JSCFunctionListEntry *funcs, int n)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;
    JS_NewClassID(pid);
    if (!JS_IsRegisteredClass(rt, *pid))
        JS_NewClass(rt, *pid, def);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, n);
    JS_SetClassProto(ctx, *pid, proto);
}

static int qc_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, qc_crypto_funcs, countof(qc_crypto_funcs));
}

JSModuleDef *qb_init_crypto(JSContext *ctx, const char *name)
{
    JSModuleDef *m;
    qc_register_class(ctx, &qc_digest_class_id, &qc_digest_class, qc_digest_proto_funcs,
                      countof(qc_digest_proto_funcs));
    qc_register_class(ctx, &qc_cipher_class_id, &qc_cipher_class, qc_cipher_proto_funcs,
                      countof(qc_cipher_proto_funcs));
    qc_register_class(ctx, &qc_key_class_id, &qc_key_class, qc_key_proto_funcs,
                      countof(qc_key_proto_funcs));
    qc_register_class(ctx, &qc_x509_class_id, &qc_x509_class, qc_x509_proto_funcs,
                      countof(qc_x509_proto_funcs));
    m = JS_NewCModule(ctx, name, qc_crypto_module_init);
    if (m && JS_AddModuleExportList(ctx, m, qc_crypto_funcs, countof(qc_crypto_funcs)) < 0)
        return NULL;
    return m;
}
