/*
 * qbun:tls — the TLS engine of node:tls, on OpenSSL memory BIOs.
 *
 * The library (node/lib/internal/tls) moves the bytes: encrypted data
 * read from the transport goes to conn.receive(), the cleartext comes out
 * of conn.readCleartext(), cleartext written by the user goes to
 * conn.writeCleartext() and the records to send come out of
 * conn.takeEncrypted(). No libuv handle is involved: every OpenSSL call
 * runs inside a call from JS.
 *
 * Objects:
 *   SecureContext  an SSL_CTX (Node's native SecureContext methods)
 *   TLSConnection  an SSL with a memory BIO pair
 *   CertStore      an X509_STORE (the root certificates, shared by the
 *                  contexts that use the default trust)
 *
 * receive(), start(), certCbDone() and shutdown() return a bit set of
 * QB_TLS_* flags describing the state after the call.
 *
 * Errors of OpenSSL are thrown like Node's: message "error:0A00010B:SSL
 * routines::wrong version number", properties library, reason and code
 * (ERR_SSL_WRONG_VERSION_NUMBER, ERR_OSSL_PEM_NO_START_LINE...).
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "qbun.h"

/* state flags returned by the connection methods */
#define QB_TLS_HANDSHAKE_DONE 1  /* a handshake completed during this call */
#define QB_TLS_CLEAR          2  /* cleartext is waiting in readCleartext() */
#define QB_TLS_ENC            4  /* records are waiting in takeEncrypted() */
#define QB_TLS_EOF            8  /* the peer sent close_notify (reported once) */
#define QB_TLS_EVENTS         16 /* takeEvents() has keylog/session events */
#define QB_TLS_CERTCB         32 /* the handshake waits for certCbDone() */
#define QB_TLS_SECURE         64 /* the handshake is complete */

#define QB_TLS_READ_CHUNK 16384

static JSClassID qb_tls_ctx_class_id;
static JSClassID qb_tls_conn_class_id;
static JSClassID qb_tls_store_class_id;

/* ------------------------------------------------------------------ errors */

static const char *qb_ossl_lib_code(unsigned long err)
{
    if (ERR_SYSTEM_ERROR(err))
        return "SYS_";
    switch (ERR_GET_LIB(err)) {
    case ERR_LIB_SYS:
        return "SYS_";
    case ERR_LIB_BN:
        return "BN_";
    case ERR_LIB_RSA:
        return "RSA_";
    case ERR_LIB_DH:
        return "DH_";
    case ERR_LIB_EVP:
        return "EVP_";
    case ERR_LIB_BUF:
        return "BUF_";
    case ERR_LIB_OBJ:
        return "OBJ_";
    case ERR_LIB_PEM:
        return "PEM_";
    case ERR_LIB_DSA:
        return "DSA_";
    case ERR_LIB_X509:
        return "X509_";
    case ERR_LIB_ASN1:
        return "ASN1_";
    case ERR_LIB_CONF:
        return "CONF_";
    case ERR_LIB_CRYPTO:
        return "CRYPTO_";
    case ERR_LIB_EC:
        return "EC_";
    case ERR_LIB_SSL:
        return "SSL_";
    case ERR_LIB_BIO:
        return "BIO_";
    case ERR_LIB_PKCS7:
        return "PKCS7_";
    case ERR_LIB_X509V3:
        return "X509V3_";
    case ERR_LIB_PKCS12:
        return "PKCS12_";
    case ERR_LIB_RAND:
        return "RAND_";
    case ERR_LIB_DSO:
        return "DSO_";
#ifdef ERR_LIB_ENGINE
    case ERR_LIB_ENGINE:
        return "ENGINE_";
#endif
    case ERR_LIB_OCSP:
        return "OCSP_";
    case ERR_LIB_UI:
        return "UI_";
    case ERR_LIB_COMP:
        return "COMP_";
    case ERR_LIB_ECDSA:
        return "ECDSA_";
    case ERR_LIB_ECDH:
        return "ECDH_";
    case ERR_LIB_OSSL_STORE:
        return "OSSL_STORE_";
    case ERR_LIB_FIPS:
        return "FIPS_";
    case ERR_LIB_CMS:
        return "CMS_";
    case ERR_LIB_TS:
        return "TS_";
    case ERR_LIB_HMAC:
        return "HMAC_";
    case ERR_LIB_CT:
        return "CT_";
    case ERR_LIB_ASYNC:
        return "ASYNC_";
    case ERR_LIB_KDF:
        return "KDF_";
    case ERR_LIB_SM2:
        return "SM2_";
    case ERR_LIB_USER:
        return "USER_";
    default:
        return "";
    }
}

/* Node's code of an OpenSSL error: ERR_SSL_<REASON> for the SSL library,
   ERR_OSSL_<LIB>_<REASON> for the others */
static void qb_ossl_code(unsigned long err, char *buf, size_t size)
{
    const char *lib = qb_ossl_lib_code(err);
    const char *reason = ERR_reason_error_string(err);
    size_t n;

    if (!strcmp(lib, "SSL_"))
        snprintf(buf, size, "ERR_SSL_");
    else
        snprintf(buf, size, "ERR_OSSL_%s", lib);
    n = strlen(buf);
    if (!reason)
        reason = "unknown error";
    for (; *reason && n + 1 < size; reason++)
        buf[n++] = *reason == ' ' ? '_' : (char)toupper((unsigned char)*reason);
    buf[n] = '\0';
}

/* an Error for the OpenSSL error 'err' (0: 'fallback' with the code
   'fallback_code') */
static JSValue qb_tls_new_error(JSContext *ctx, unsigned long err, const char *fallback,
                                const char *fallback_code)
{
    char msg[256], code[256];
    JSValue e;

    if (err == 0) {
        e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, fallback, strlen(fallback));
        if (JS_IsException(e))
            return e;
        if (fallback_code)
            JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, fallback_code),
                                      JS_PROP_C_W_E);
        return e;
    }
    ERR_error_string_n(err, msg, sizeof(msg));
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, strlen(msg));
    if (JS_IsException(e))
        return e;
    {
        const char *ls = ERR_lib_error_string(err);
        const char *rs = ERR_reason_error_string(err);
        if (ls)
            JS_DefinePropertyValueStr(ctx, e, "library", JS_NewString(ctx, ls), JS_PROP_C_W_E);
        if (rs)
            JS_DefinePropertyValueStr(ctx, e, "reason", JS_NewString(ctx, rs), JS_PROP_C_W_E);
    }
    qb_ossl_code(err, code, sizeof(code));
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    return e;
}

/* throw the first error of the OpenSSL error queue (the others go to
   err.opensslErrorStack) and clear the queue */
static JSValue qb_tls_throw(JSContext *ctx, const char *fallback)
{
    unsigned long err = ERR_get_error();
    unsigned long more;
    JSValue e = qb_tls_new_error(ctx, err, fallback, "ERR_SSL_UNKNOWN");
    if (JS_IsException(e)) {
        ERR_clear_error();
        return e;
    }
    more = ERR_get_error();
    if (more) {
        JSValue stack = JS_NewArray(ctx);
        uint32_t i = 0;
        char msg[256];
        while (more) {
            ERR_error_string_n(more, msg, sizeof(msg));
            JS_SetPropertyUint32(ctx, stack, i++, JS_NewString(ctx, msg));
            more = ERR_get_error();
        }
        JS_DefinePropertyValueStr(ctx, e, "opensslErrorStack", stack, JS_PROP_C_W_E);
    }
    ERR_clear_error();
    return JS_Throw(ctx, e);
}

/* Node's names of the X509_V_ERR_* codes */
static const char *qb_x509_err_code(long err)
{
    switch (err) {
#define QB_X509_ERR(name)                                                                          \
    case X509_V_ERR_##name:                                                                        \
        return #name;
        QB_X509_ERR(UNABLE_TO_GET_ISSUER_CERT)
        QB_X509_ERR(UNABLE_TO_GET_CRL)
        QB_X509_ERR(UNABLE_TO_DECRYPT_CERT_SIGNATURE)
        QB_X509_ERR(UNABLE_TO_DECRYPT_CRL_SIGNATURE)
        QB_X509_ERR(UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY)
        QB_X509_ERR(CERT_SIGNATURE_FAILURE)
        QB_X509_ERR(CRL_SIGNATURE_FAILURE)
        QB_X509_ERR(CERT_NOT_YET_VALID)
        QB_X509_ERR(CERT_HAS_EXPIRED)
        QB_X509_ERR(CRL_NOT_YET_VALID)
        QB_X509_ERR(CRL_HAS_EXPIRED)
        QB_X509_ERR(ERROR_IN_CERT_NOT_BEFORE_FIELD)
        QB_X509_ERR(ERROR_IN_CERT_NOT_AFTER_FIELD)
        QB_X509_ERR(ERROR_IN_CRL_LAST_UPDATE_FIELD)
        QB_X509_ERR(ERROR_IN_CRL_NEXT_UPDATE_FIELD)
        QB_X509_ERR(OUT_OF_MEM)
        QB_X509_ERR(DEPTH_ZERO_SELF_SIGNED_CERT)
        QB_X509_ERR(SELF_SIGNED_CERT_IN_CHAIN)
        QB_X509_ERR(UNABLE_TO_GET_ISSUER_CERT_LOCALLY)
        QB_X509_ERR(UNABLE_TO_VERIFY_LEAF_SIGNATURE)
        QB_X509_ERR(CERT_CHAIN_TOO_LONG)
        QB_X509_ERR(CERT_REVOKED)
        QB_X509_ERR(INVALID_CA)
        QB_X509_ERR(PATH_LENGTH_EXCEEDED)
        QB_X509_ERR(INVALID_PURPOSE)
        QB_X509_ERR(CERT_UNTRUSTED)
        QB_X509_ERR(CERT_REJECTED)
        QB_X509_ERR(HOSTNAME_MISMATCH)
#undef QB_X509_ERR
    default:
        return "UNSPECIFIED";
    }
}

/* -------------------------------------------------------------- utilities */

/* a memory BIO holding a copy of a string or of bytes; NULL with an
   exception */
static BIO *qb_tls_bio(JSContext *ctx, JSValueConst val)
{
    const char *s;
    size_t len;
    BIO *bio;
    int ok = 1;

    if (JS_IsString(val)) {
        s = JS_ToCStringLen(ctx, &len, val);
        if (!s)
            return NULL;
        bio = BIO_new(BIO_s_mem());
        if (bio && len > 0)
            ok = len <= INT_MAX && BIO_write(bio, s, (int)len) == (int)len;
        JS_FreeCString(ctx, s);
    } else {
        uint8_t *p = qb_get_bytes(ctx, val, &len);
        if (!p)
            return NULL;
        bio = BIO_new(BIO_s_mem());
        if (bio && len > 0)
            ok = len <= INT_MAX && BIO_write(bio, p, (int)len) == (int)len;
    }
    if (!bio || !ok) {
        BIO_free(bio);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    /* reading past the end is EOF, not "retry" */
    BIO_set_mem_eof_return(bio, 0);
    return bio;
}

/* the contents of a memory BIO as a JS string */
static JSValue qb_bio_string(JSContext *ctx, BIO *bio)
{
    char *data = NULL;
    long n = BIO_get_mem_data(bio, &data);
    if (n <= 0 || !data)
        return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, data, (size_t)n);
}

static int qb_tls_no_password(char *buf, int size, int rwflag, void *u)
{
    (void)buf;
    (void)size;
    (void)rwflag;
    (void)u;
    return 0;
}

typedef struct QBPass {
    const char *data;
    size_t len;
} QBPass;

static int qb_tls_password(char *buf, int size, int rwflag, void *u)
{
    QBPass *p = u;
    size_t len;
    (void)rwflag;
    if (!p || !p->data || size < 0)
        return -1;
    len = p->len;
    if (len > (size_t)size)
        len = (size_t)size;
    memcpy(buf, p->data, len);
    return (int)len;
}

/* "AB:CD:..." digest of a certificate */
static JSValue qb_x509_fingerprint(JSContext *ctx, const X509 *cert, const EVP_MD *md)
{
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned int n = 0, i;
    char out[EVP_MAX_MD_SIZE * 3 + 1];
    static const char hex[] = "0123456789ABCDEF";

    if (!X509_digest(cert, md, d, &n) || n == 0) {
        ERR_clear_error();
        return JS_UNDEFINED;
    }
    for (i = 0; i < n; i++) {
        out[i * 3] = hex[d[i] >> 4];
        out[i * 3 + 1] = hex[d[i] & 15];
        out[i * 3 + 2] = ':';
    }
    out[n * 3 - 1] = '\0';
    return JS_NewString(ctx, out);
}

static JSValue qb_bn_hex(JSContext *ctx, const BIGNUM *bn, int lower, int prefix)
{
    char *h = BN_bn2hex(bn);
    const char *p;
    JSValue v;
    char *tmp;
    size_t len;

    if (!h)
        return JS_UNDEFINED;
    p = h;
    if (lower) {
        /* the exponent: no leading zeros */
        while (p[0] == '0' && p[1])
            p++;
    }
    len = strlen(p);
    tmp = malloc(len + 3);
    if (!tmp) {
        OPENSSL_free(h);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (prefix) {
        tmp[0] = '0';
        tmp[1] = 'x';
    }
    {
        size_t i, o = prefix ? 2 : 0;
        for (i = 0; i < len; i++)
            tmp[o + i] = lower ? (char)tolower((unsigned char)p[i]) : p[i];
        tmp[o + len] = '\0';
    }
    v = JS_NewString(ctx, tmp);
    free(tmp);
    OPENSSL_free(h);
    return v;
}

/* X509_NAME → { CN: "...", O: "..." } (repeated keys become arrays) */
static JSValue qb_x509_name(JSContext *ctx, const X509_NAME *name)
{
    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    int i, n;

    if (JS_IsException(obj) || !name)
        return obj;
    n = X509_NAME_entry_count(name);
    for (i = 0; i < n; i++) {
        const X509_NAME_ENTRY *e = X509_NAME_get_entry(name, i);
        const ASN1_OBJECT *o;
        const ASN1_STRING *d;
        char key[128];
        unsigned char *utf8 = NULL;
        int nid, ul;
        JSValue v, prev;

        if (!e)
            continue;
        o = X509_NAME_ENTRY_get_object(e);
        d = X509_NAME_ENTRY_get_data(e);
        nid = OBJ_obj2nid(o);
        if (nid != NID_undef && OBJ_nid2sn(nid))
            snprintf(key, sizeof(key), "%s", OBJ_nid2sn(nid));
        else if (OBJ_obj2txt(key, sizeof(key), o, 1) <= 0)
            continue;
        ul = d ? ASN1_STRING_to_UTF8(&utf8, d) : -1;
        if (ul >= 0)
            v = JS_NewStringLen(ctx, (const char *)utf8, (size_t)ul);
        else
            v = JS_NewString(ctx, "");
        OPENSSL_free(utf8);
        prev = JS_GetPropertyStr(ctx, obj, key);
        if (JS_IsUndefined(prev)) {
            JS_DefinePropertyValueStr(ctx, obj, key, v, JS_PROP_C_W_E);
        } else if (JS_IsArray(ctx, prev) > 0) {
            int64_t len = 0;
            JSValue l = JS_GetPropertyStr(ctx, prev, "length");
            JS_ToInt64(ctx, &len, l);
            JS_FreeValue(ctx, l);
            JS_SetPropertyUint32(ctx, prev, (uint32_t)len, v);
        } else {
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, prev));
            JS_SetPropertyUint32(ctx, arr, 1, v);
            JS_DefinePropertyValueStr(ctx, obj, key, arr, JS_PROP_C_W_E);
        }
        JS_FreeValue(ctx, prev);
    }
    ERR_clear_error();
    return obj;
}

/* Node's escaping of the subject alternative names: unsafe values become
   JSON strings */
/* ASN1_STRING_length() is deprecated since OpenSSL 4.1 */
static int qb_asn1_len(const ASN1_STRING *s)
{
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
    return ASN1_STRING_get_length(s);
#else
    return ASN1_STRING_length(s);
#endif
}

static void qb_print_alt_value(BIO *out, const unsigned char *s, int len, int utf8)
{
    int i, safe = 1;
    for (i = 0; i < len && safe; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\' || c == ',' || c == '\'')
            safe = 0;
        else if (utf8 ? (c < ' ' || c == 0x7f) : (c < ' ' || c > '~'))
            safe = 0;
    }
    if (safe) {
        BIO_write(out, s, len);
        return;
    }
    BIO_write(out, "\"", 1);
    for (i = 0; i < len; i++) {
        unsigned char c = s[i];
        if (c == '\\')
            BIO_write(out, "\\\\", 2);
        else if (c == '"')
            BIO_write(out, "\\\"", 2);
        else if (c >= ' ' && c <= '~')
            BIO_write(out, &s[i], 1);
        else if (utf8 && c >= 0x80)
            BIO_write(out, &s[i], 1);
        else
            BIO_printf(out, "\\u%04X", (unsigned int)c);
    }
    BIO_write(out, "\"", 1);
}

static void qb_print_general_name(BIO *out, const GENERAL_NAME *gen)
{
    switch (gen->type) {
    case GEN_DNS:
        BIO_puts(out, "DNS:");
        qb_print_alt_value(out, ASN1_STRING_get0_data(gen->d.dNSName),
                           qb_asn1_len(gen->d.dNSName), 0);
        break;
    case GEN_EMAIL:
        BIO_puts(out, "email:");
        qb_print_alt_value(out, ASN1_STRING_get0_data(gen->d.rfc822Name),
                           qb_asn1_len(gen->d.rfc822Name), 0);
        break;
    case GEN_URI:
        BIO_puts(out, "URI:");
        qb_print_alt_value(out, ASN1_STRING_get0_data(gen->d.uniformResourceIdentifier),
                           qb_asn1_len(gen->d.uniformResourceIdentifier), 0);
        break;
    case GEN_IPADD: {
        const unsigned char *b = ASN1_STRING_get0_data(gen->d.iPAddress);
        int len = qb_asn1_len(gen->d.iPAddress), j;
        BIO_puts(out, "IP Address:");
        if (len == 4) {
            BIO_printf(out, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
        } else if (len == 16) {
            for (j = 0; j < 8; j++) {
                unsigned int pair = ((unsigned int)b[2 * j] << 8) | b[2 * j + 1];
                BIO_printf(out, j == 0 ? "%X" : ":%X", pair);
            }
        } else {
            BIO_printf(out, "<invalid length=%d>", len);
        }
        break;
    }
    case GEN_DIRNAME: {
        char buf[1024];
        BIO_puts(out, "DirName:");
        if (X509_NAME_oneline(gen->d.directoryName, buf, sizeof(buf)))
            qb_print_alt_value(out, (const unsigned char *)buf, (int)strlen(buf), 1);
        break;
    }
    case GEN_RID: {
        char buf[256];
        BIO_puts(out, "Registered ID:");
        if (OBJ_obj2txt(buf, sizeof(buf), gen->d.registeredID, 0) > 0)
            BIO_puts(out, buf);
        break;
    }
    case GEN_OTHERNAME:
        BIO_puts(out, "othername:<unsupported>");
        break;
    case GEN_X400:
        BIO_puts(out, "X400Name:<unsupported>");
        break;
    case GEN_EDIPARTY:
        BIO_puts(out, "EdiPartyName:<unsupported>");
        break;
    default:
        break;
    }
}

static JSValue qb_x509_alt_names(JSContext *ctx, const X509 *cert)
{
    GENERAL_NAMES *names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    BIO *out;
    JSValue v;
    int i;

    if (!names)
        return JS_UNDEFINED;
    out = BIO_new(BIO_s_mem());
    if (!out) {
        GENERAL_NAMES_free(names);
        return JS_UNDEFINED;
    }
    for (i = 0; i < sk_GENERAL_NAME_num(names); i++) {
        if (i > 0)
            BIO_puts(out, ", ");
        qb_print_general_name(out, sk_GENERAL_NAME_value(names, i));
    }
    v = qb_bio_string(ctx, out);
    BIO_free(out);
    GENERAL_NAMES_free(names);
    return v;
}

static JSValue qb_x509_info_access(JSContext *ctx, const X509 *cert)
{
    AUTHORITY_INFO_ACCESS *aia = X509_get_ext_d2i(cert, NID_info_access, NULL, NULL);
    BIO *out;
    JSValue v;
    int i;

    if (!aia)
        return JS_UNDEFINED;
    out = BIO_new(BIO_s_mem());
    if (!out) {
        AUTHORITY_INFO_ACCESS_free(aia);
        return JS_UNDEFINED;
    }
    for (i = 0; i < sk_ACCESS_DESCRIPTION_num(aia); i++) {
        ACCESS_DESCRIPTION *ad = sk_ACCESS_DESCRIPTION_value(aia, i);
        char buf[256];
        if (OBJ_obj2txt(buf, sizeof(buf), ad->method, 0) <= 0)
            continue;
        BIO_puts(out, buf);
        BIO_puts(out, " - ");
        qb_print_general_name(out, ad->location);
        BIO_puts(out, "\n");
    }
    v = qb_bio_string(ctx, out);
    BIO_free(out);
    AUTHORITY_INFO_ACCESS_free(aia);
    return v;
}

static JSValue qb_asn1_time(JSContext *ctx, const ASN1_TIME *t)
{
    BIO *out = BIO_new(BIO_s_mem());
    JSValue v;
    if (!out)
        return JS_UNDEFINED;
    if (t)
        ASN1_TIME_print(out, t);
    v = qb_bio_string(ctx, out);
    BIO_free(out);
    return v;
}

/* Node's legacy certificate object (X509ToObject) */
static JSValue qb_x509_to_object(JSContext *ctx, X509 *cert)
{
    JSValue o = JS_NewObject(ctx);
    EVP_PKEY *pkey;
    int len;

    if (JS_IsException(o))
        return o;
    qb_set(ctx, o, "subject", qb_x509_name(ctx, X509_get_subject_name(cert)));
    qb_set(ctx, o, "issuer", qb_x509_name(ctx, X509_get_issuer_name(cert)));
    {
        JSValue alt = qb_x509_alt_names(ctx, cert);
        if (!JS_IsUndefined(alt))
            qb_set(ctx, o, "subjectaltname", alt);
    }
    {
        JSValue ia = qb_x509_info_access(ctx, cert);
        if (!JS_IsUndefined(ia))
            qb_set(ctx, o, "infoAccess", ia);
    }
    qb_set(ctx, o, "ca", JS_NewBool(ctx, X509_check_ca(cert) == 1));

    pkey = X509_get0_pubkey(cert);
    if (pkey && (EVP_PKEY_is_a(pkey, "RSA") || EVP_PKEY_is_a(pkey, "RSA-PSS"))) {
        BIGNUM *n = NULL, *e = NULL;
        if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) && n)
            qb_set(ctx, o, "modulus", qb_bn_hex(ctx, n, 0, 0));
        qb_set(ctx, o, "bits", JS_NewInt32(ctx, EVP_PKEY_get_bits(pkey)));
        if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) && e)
            qb_set(ctx, o, "exponent", qb_bn_hex(ctx, e, 1, 1));
        BN_free(n);
        BN_free(e);
        len = i2d_PUBKEY(pkey, NULL);
        if (len > 0) {
            uint8_t *buf = js_malloc_rt(JS_GetRuntime(ctx), (size_t)len);
            if (buf) {
                unsigned char *p = buf;
                i2d_PUBKEY(pkey, &p);
                qb_set(ctx, o, "pubkey", qb_new_buffer_owned(ctx, buf, (size_t)len));
            }
        }
    } else if (pkey && EVP_PKEY_is_a(pkey, "EC")) {
        char group[80];
        size_t glen = 0;
        unsigned char point[256];
        size_t plen = 0;
        qb_set(ctx, o, "bits", JS_NewInt32(ctx, EVP_PKEY_get_bits(pkey)));
        if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, point,
                                            sizeof(point), &plen) &&
            plen > 0)
            qb_set(ctx, o, "pubkey", qb_new_buffer(ctx, point, plen));
        if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group),
                                           &glen)) {
            int nid = OBJ_sn2nid(group);
            const char *nist;
            if (nid == NID_undef)
                nid = OBJ_txt2nid(group);
            qb_set(
                ctx, o, "asn1Curve",
                JS_NewString(ctx, nid != NID_undef && OBJ_nid2sn(nid) ? OBJ_nid2sn(nid) : group));
            nist = nid != NID_undef ? EC_curve_nid2nist(nid) : NULL;
            if (nist)
                qb_set(ctx, o, "nistCurve", JS_NewString(ctx, nist));
        }
    }

    qb_set(ctx, o, "valid_from", qb_asn1_time(ctx, X509_get0_notBefore(cert)));
    qb_set(ctx, o, "valid_to", qb_asn1_time(ctx, X509_get0_notAfter(cert)));
    qb_set(ctx, o, "fingerprint", qb_x509_fingerprint(ctx, cert, EVP_sha1()));
    qb_set(ctx, o, "fingerprint256", qb_x509_fingerprint(ctx, cert, EVP_sha256()));
    qb_set(ctx, o, "fingerprint512", qb_x509_fingerprint(ctx, cert, EVP_sha512()));

    {
        EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
        if (eku) {
            JSValue arr = JS_NewArray(ctx);
            int i;
            uint32_t k = 0;
            for (i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
                char buf[256];
                if (OBJ_obj2txt(buf, sizeof(buf), sk_ASN1_OBJECT_value(eku, i), 1) > 0)
                    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, buf));
            }
            qb_set(ctx, o, "ext_key_usage", arr);
            EXTENDED_KEY_USAGE_free(eku);
        }
    }
    {
        BIGNUM *sn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(cert), NULL);
        if (sn) {
            qb_set(ctx, o, "serialNumber", qb_bn_hex(ctx, sn, 0, 0));
            BN_free(sn);
        }
    }
    len = i2d_X509(cert, NULL);
    if (len > 0) {
        uint8_t *buf = js_malloc_rt(JS_GetRuntime(ctx), (size_t)len);
        if (buf) {
            unsigned char *p = buf;
            i2d_X509(cert, &p);
            qb_set(ctx, o, "raw", qb_new_buffer_owned(ctx, buf, (size_t)len));
        }
    }
    ERR_clear_error();
    return o;
}

static JSValue qb_x509_der(JSContext *ctx, const X509 *cert)
{
    int len;
    uint8_t *buf;
    unsigned char *p;

    if (!cert)
        return JS_NULL;
    len = i2d_X509(cert, NULL);
    if (len <= 0)
        return JS_NULL;
    buf = js_malloc_rt(JS_GetRuntime(ctx), (size_t)len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    p = buf;
    i2d_X509(cert, &p);
    return qb_new_buffer_owned(ctx, buf, (size_t)len);
}

/* --------------------------------------------------------------- CertStore */

typedef struct QBTLSStore {
    X509_STORE *store;
} QBTLSStore;

static void qb_tls_store_finalizer(JSRuntime *rt, JSValue val)
{
    QBTLSStore *s = JS_GetOpaque(val, qb_tls_store_class_id);
    if (!s)
        return;
    X509_STORE_free(s->store);
    js_free_rt(rt, s);
}

static JSClassDef qb_tls_store_class = {
    "CertStore",
    .finalizer = qb_tls_store_finalizer,
};

static int qb_is_file(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* the system bundles of the common platforms (the first one found is
   loaded on top of OpenSSL's default paths) */
static const char *const qb_system_bundles[] = {
    "/etc/ssl/cert.pem",                                 /* macOS, Alpine, BSD */
    "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Arch */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* Fedora, RHEL */
    "/etc/pki/tls/certs/ca-bundle.crt",                  /* older RHEL */
    "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
    "/usr/local/share/certs/ca-root-nss.crt",            /* FreeBSD */
    "/etc/pki/tls/cacert.pem",                           /* OpenELEC */
    NULL,
};

/* createCertStore(loadSystem: boolean): a new store, with the system
   roots when 'loadSystem' */
static JSValue qb_tls_create_store(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    int load_system = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    QBTLSStore *s;
    JSValue obj;
    X509_STORE *st = X509_STORE_new();

    if (!st)
        return qb_tls_throw(ctx, "X509_STORE_new failed");
    if (load_system) {
        int i;
        /* the default file (SSL_CERT_FILE or OPENSSLDIR/cert.pem) and the
           hashed directory (SSL_CERT_DIR or OPENSSLDIR/certs, looked up
           at verification time) */
        X509_STORE_set_default_paths(st);
        for (i = 0; qb_system_bundles[i]; i++) {
            if (qb_is_file(qb_system_bundles[i])) {
                X509_STORE_load_file(st, qb_system_bundles[i]);
                break;
            }
        }
        /* the hashed directory of the distribution (Linux), looked up
           lazily at verification time */
        {
            struct stat sb;
            if (stat("/etc/ssl/certs", &sb) == 0 && S_ISDIR(sb.st_mode))
                X509_STORE_load_path(st, "/etc/ssl/certs");
        }
        ERR_clear_error();
    }
    obj = JS_NewObjectClass(ctx, (int)qb_tls_store_class_id);
    if (JS_IsException(obj)) {
        X509_STORE_free(st);
        return obj;
    }
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        X509_STORE_free(st);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->store = st;
    JS_SetOpaque(obj, s);
    return obj;
}

static QBTLSStore *qb_tls_store_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_tls_store_class_id);
}

/* store.addFile(path): add the certificates of a PEM file (throws) */
static JSValue qb_tls_store_add_file(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    QBTLSStore *s = qb_tls_store_get(ctx, this_val);
    const char *path;
    int ok;

    if (!s)
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    ok = X509_STORE_load_file(s->store, path);
    JS_FreeCString(ctx, path);
    if (!ok)
        return qb_tls_throw(ctx, "failed to load the certificate file");
    ERR_clear_error();
    return JS_UNDEFINED;
}

/* store.addPEM(pem): add the certificates of a PEM string or buffer,
   return how many were added */
static JSValue qb_tls_store_add_pem(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    QBTLSStore *s = qb_tls_store_get(ctx, this_val);
    BIO *bio;
    X509 *x;
    int n = 0;

    if (!s)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    while ((x = PEM_read_bio_X509_AUX(bio, NULL, qb_tls_no_password, NULL))) {
        if (X509_STORE_add_cert(s->store, x))
            n++;
        X509_free(x);
    }
    BIO_free(bio);
    ERR_clear_error();
    return JS_NewInt32(ctx, n);
}

/* store.certificates(): the PEM strings of the loaded certificates */
static JSValue qb_tls_store_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv)
{
    QBTLSStore *s = qb_tls_store_get(ctx, this_val);
    STACK_OF(X509) * all;
    JSValue arr;
    int i;
    uint32_t k = 0;

    if (!s)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    all = X509_STORE_get1_all_certs(s->store);
    if (!all) {
        ERR_clear_error();
        return arr;
    }
    for (i = 0; i < sk_X509_num(all); i++) {
        BIO *out = BIO_new(BIO_s_mem());
        if (!out)
            break;
        if (PEM_write_bio_X509(out, sk_X509_value(all, i)))
            JS_SetPropertyUint32(ctx, arr, k++, qb_bio_string(ctx, out));
        BIO_free(out);
    }
    sk_X509_pop_free(all, X509_free);
    ERR_clear_error();
    return arr;
}

static const JSCFunctionListEntry qb_tls_store_proto_funcs[] = {
    JS_CFUNC_DEF("addFile", 1, qb_tls_store_add_file),
    JS_CFUNC_DEF("addPEM", 1, qb_tls_store_add_pem),
    JS_CFUNC_DEF("certificates", 0, qb_tls_store_certificates),
};

/* ----------------------------------------------------------- SecureContext */

typedef struct QBTLSCtx {
    SSL_CTX *ctx;
    /* the cert store is a CertStore shared with other contexts: copied
       before it is modified */
    int shared_store;
    /* session ticket keys: name, HMAC secret, AES key (Node's layout) */
    unsigned char ticket_keys[48];
} QBTLSCtx;

/* the SSL_CTX no longer points at its QBTLSCtx (ticket keys) */
static void qb_tls_ctx_detach(SSL_CTX *sc)
{
    if (SSL_CTX_set_app_data(sc, NULL) != 1)
        ERR_clear_error();
}

static void qb_tls_ctx_finalizer(JSRuntime *rt, JSValue val)
{
    QBTLSCtx *c = JS_GetOpaque(val, qb_tls_ctx_class_id);
    if (!c)
        return;
    if (c->ctx) {
        qb_tls_ctx_detach(c->ctx);
        SSL_CTX_free(c->ctx);
    }
    js_free_rt(rt, c);
}

static JSClassDef qb_tls_ctx_class = {
    "SecureContext",
    .finalizer = qb_tls_ctx_finalizer,
};

static QBTLSCtx *qb_tls_ctx_get(JSContext *ctx, JSValueConst obj)
{
    QBTLSCtx *c = JS_GetOpaque2(ctx, obj, qb_tls_ctx_class_id);
    if (c && !c->ctx) {
        JS_ThrowTypeError(ctx, "the secure context is closed");
        return NULL;
    }
    return c;
}

/* the store of the context, made private first if it is shared */
static X509_STORE *qb_tls_own_store(QBTLSCtx *c)
{
    X509_STORE *st = SSL_CTX_get_cert_store(c->ctx);
    if (c->shared_store) {
        X509_STORE *n = X509_STORE_new();
        STACK_OF(X509) * all;
        int i;
        if (!n)
            return NULL;
        all = st ? X509_STORE_get1_all_certs(st) : NULL;
        if (all) {
            for (i = 0; i < sk_X509_num(all); i++)
                X509_STORE_add_cert(n, sk_X509_value(all, i));
            sk_X509_pop_free(all, X509_free);
        }
        X509_STORE_load_path(n, X509_get_default_cert_dir());
        ERR_clear_error();
        SSL_CTX_set_cert_store(c->ctx, n);
        c->shared_store = 0;
        st = n;
    }
    return st;
}

static int qb_tls_verify_cb(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
    /* always continue: the result is checked by the library after the
       handshake (verifyError()), like Node */
    (void)preverify_ok;
    (void)x509_ctx;
    return 1;
}

typedef struct QBTLSConn QBTLSConn;
static void qb_tls_keylog_cb(const SSL *ssl, const char *line);
static int qb_tls_new_session_cb(SSL *ssl, SSL_SESSION *sess);
static int qb_tls_alpn_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg);
static int qb_tls_cert_cb(SSL *ssl, void *arg);

/* session tickets with Node's 48 byte keys */
static int qb_tls_ticket_cb(SSL *ssl, unsigned char *key_name, unsigned char *iv,
                            EVP_CIPHER_CTX *ectx, EVP_MAC_CTX *hctx, int enc)
{
    QBTLSCtx *c = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    OSSL_PARAM params[2];

    if (!c)
        return -1;
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)"SHA256", 0);
    params[1] = OSSL_PARAM_construct_end();
    if (enc) {
        memcpy(key_name, c->ticket_keys, 16);
        if (RAND_bytes(iv, 16) <= 0)
            return -1;
        if (EVP_EncryptInit_ex(ectx, EVP_aes_128_cbc(), NULL, c->ticket_keys + 32, iv) != 1)
            return -1;
        if (EVP_MAC_init(hctx, c->ticket_keys + 16, 16, params) != 1)
            return -1;
        return 1;
    }
    if (memcmp(key_name, c->ticket_keys, 16) != 0)
        return 0; /* unknown key: full handshake */
    if (EVP_MAC_init(hctx, c->ticket_keys + 16, 16, params) != 1)
        return -1;
    if (EVP_DecryptInit_ex(ectx, EVP_aes_128_cbc(), NULL, c->ticket_keys + 32, iv) != 1)
        return -1;
    return 1;
}

/* createSecureContext(minVersion, maxVersion) */
static JSValue qb_tls_create_ctx(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    int32_t minv = 0, maxv = 0;
    SSL_CTX *sc;
    QBTLSCtx *c;
    JSValue obj;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &minv, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &maxv, argv[1]))
        return JS_EXCEPTION;
    sc = SSL_CTX_new(TLS_method());
    if (!sc)
        return qb_tls_throw(ctx, "SSL_CTX_new failed");
    c = js_mallocz(ctx, sizeof(*c));
    if (!c) {
        SSL_CTX_free(sc);
        return JS_EXCEPTION;
    }
    c->ctx = sc;
    if (RAND_bytes(c->ticket_keys, sizeof(c->ticket_keys)) <= 0) {
        js_free(ctx, c);
        SSL_CTX_free(sc);
        return qb_tls_throw(ctx, "RAND_bytes failed");
    }
    if (SSL_CTX_set_app_data(sc, c) != 1) {
        js_free(ctx, c);
        SSL_CTX_free(sc);
        return qb_tls_throw(ctx, "SSL_CTX_set_ex_data failed");
    }
    SSL_CTX_set_options(sc, SSL_OP_NO_SSLv3);
    SSL_CTX_set_session_cache_mode(sc, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_SERVER |
                                           SSL_SESS_CACHE_NO_INTERNAL |
                                           SSL_SESS_CACHE_NO_AUTO_CLEAR);
    if (!SSL_CTX_set_min_proto_version(sc, minv) || !SSL_CTX_set_max_proto_version(sc, maxv)) {
        js_free(ctx, c);
        qb_tls_ctx_detach(sc);
        SSL_CTX_free(sc);
        return qb_tls_throw(ctx, "invalid protocol version");
    }
    SSL_CTX_set_verify(sc, SSL_VERIFY_NONE, qb_tls_verify_cb);
    SSL_CTX_set_keylog_callback(sc, qb_tls_keylog_cb);
    SSL_CTX_sess_set_new_cb(sc, qb_tls_new_session_cb);
    SSL_CTX_set_alpn_select_cb(sc, qb_tls_alpn_cb, NULL);
    SSL_CTX_set_cert_cb(sc, qb_tls_cert_cb, NULL);
    SSL_CTX_set_tlsext_ticket_key_evp_cb(sc, qb_tls_ticket_cb);

    obj = JS_NewObjectClass(ctx, (int)qb_tls_ctx_class_id);
    if (JS_IsException(obj)) {
        qb_tls_ctx_detach(sc);
        SSL_CTX_free(sc);
        js_free(ctx, c);
        return obj;
    }
    JS_SetOpaque(obj, c);
    return obj;
}

static JSValue qb_tls_ctx_set_min_proto(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    int32_t v;
    if (!c || JS_ToInt32(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    if (!SSL_CTX_set_min_proto_version(c->ctx, v))
        return qb_tls_throw(ctx, "invalid protocol version");
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_max_proto(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    int32_t v;
    if (!c || JS_ToInt32(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    if (!SSL_CTX_set_max_proto_version(c->ctx, v))
        return qb_tls_throw(ctx, "invalid protocol version");
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_get_min_proto(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)SSL_CTX_get_min_proto_version(c->ctx));
}

static JSValue qb_tls_ctx_get_max_proto(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)SSL_CTX_get_max_proto_version(c->ctx));
}

static JSValue qb_tls_ctx_set_options(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    int64_t v;
    if (!c || JS_ToInt64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    SSL_CTX_set_options(c->ctx, (uint64_t)v);
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_ciphers(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    const char *s;
    size_t len;
    int ok;
    if (!c)
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    ok = SSL_CTX_set_cipher_list(c->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok) {
        unsigned long err = ERR_peek_error();
        /* the TLSv1.2 ciphers were cleared on purpose (TLSv1.3 only) */
        if (len == 0 && ERR_GET_LIB(err) == ERR_LIB_SSL &&
            ERR_GET_REASON(err) == SSL_R_NO_CIPHER_MATCH) {
            ERR_clear_error();
            return JS_UNDEFINED;
        }
        return qb_tls_throw(ctx, "Failed to set ciphers");
    }
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_cipher_suites(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    const char *s;
    int ok;
    if (!c)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    ok = SSL_CTX_set_ciphersuites(c->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set ciphers");
    return JS_UNDEFINED;
}

/* addCACert(pem): trusted certificates (also sent as acceptable client
   CAs by servers) */
static JSValue qb_tls_ctx_add_ca_cert(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    X509_STORE *st;
    BIO *bio;
    X509 *x;

    if (!c)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    st = qb_tls_own_store(c);
    if (!st) {
        BIO_free(bio);
        return qb_tls_throw(ctx, "X509_STORE_new failed");
    }
    while ((x = PEM_read_bio_X509_AUX(bio, NULL, qb_tls_no_password, NULL))) {
        X509_STORE_add_cert(st, x);
        if (!SSL_CTX_add_client_CA(c->ctx, x))
            ERR_clear_error();
        X509_free(x);
    }
    BIO_free(bio);
    ERR_clear_error();
    return JS_UNDEFINED;
}

/* addRootCerts(store): use a shared CertStore as the trust */
static JSValue qb_tls_ctx_add_root_certs(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    QBTLSStore *s;
    if (!c)
        return JS_EXCEPTION;
    s = qb_tls_store_get(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (!X509_STORE_up_ref(s->store))
        return qb_tls_throw(ctx, "X509_STORE_up_ref failed");
    SSL_CTX_set_cert_store(c->ctx, s->store);
    c->shared_store = 1;
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_allow_partial(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    X509_STORE *st;
    if (!c)
        return JS_EXCEPTION;
    st = qb_tls_own_store(c);
    if (!st)
        return qb_tls_throw(ctx, "X509_STORE_new failed");
    X509_STORE_set_flags(st, X509_V_FLAG_PARTIAL_CHAIN);
    return JS_UNDEFINED;
}

/* setCert(pem): the certificate and its chain */
static JSValue qb_tls_ctx_set_cert(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    BIO *bio;
    X509 *x, *extra;
    unsigned long err;
    STACK_OF(X509) * chain;
    int i, ok;

    if (!c)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    ERR_clear_error();
    x = PEM_read_bio_X509_AUX(bio, NULL, qb_tls_no_password, NULL);
    if (!x) {
        BIO_free(bio);
        return qb_tls_throw(ctx, "Failed to read the certificate");
    }
    chain = sk_X509_new_null();
    while ((extra = PEM_read_bio_X509(bio, NULL, qb_tls_no_password, NULL))) {
        if (!chain || !sk_X509_push(chain, extra))
            X509_free(extra);
    }
    BIO_free(bio);
    /* the loop ends at EOF: "no start line" */
    err = ERR_peek_last_error();
    if (err && !(ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE)) {
        X509_free(x);
        sk_X509_pop_free(chain, X509_free);
        return qb_tls_throw(ctx, "Failed to read the certificate chain");
    }
    ERR_clear_error();
    ok = SSL_CTX_use_certificate(c->ctx, x);
    if (ok) {
        SSL_CTX_clear_chain_certs(c->ctx);
        for (i = 0; chain && i < sk_X509_num(chain); i++) {
            if (!SSL_CTX_add1_chain_cert(c->ctx, sk_X509_value(chain, i))) {
                ok = 0;
                break;
            }
        }
    }
    X509_free(x);
    sk_X509_pop_free(chain, X509_free);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set the certificate");
    return JS_UNDEFINED;
}

/* setKey(pem, passphrase?) */
static JSValue qb_tls_ctx_set_key(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    BIO *bio;
    EVP_PKEY *key;
    QBPass pass = {NULL, 0};
    const char *pstr = NULL;
    int ok;

    if (!c)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        pstr = JS_ToCStringLen(ctx, &pass.len, argv[1]);
        if (!pstr) {
            BIO_free(bio);
            return JS_EXCEPTION;
        }
        pass.data = pstr;
    }
    ERR_clear_error();
    key = PEM_read_bio_PrivateKey(bio, NULL, qb_tls_password, &pass);
    BIO_free(bio);
    if (pstr)
        JS_FreeCString(ctx, pstr);
    if (!key)
        return qb_tls_throw(ctx, "Failed to read the private key");
    ok = SSL_CTX_use_PrivateKey(c->ctx, key);
    EVP_PKEY_free(key);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set the private key");
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_ecdh_curve(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    const char *s;
    int ok;
    if (!c)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (!strcmp(s, "auto")) {
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    ok = SSL_CTX_set1_groups_list(c->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok) {
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                             "Failed to set ECDH curve");
    }
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_sigalgs(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    const char *s;
    int ok;
    if (!c)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    ok = SSL_CTX_set1_sigalgs_list(c->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set sigalgs");
    return JS_UNDEFINED;
}

/* setDHParam(pem | true): returns a warning string for weak parameters */
static JSValue qb_tls_ctx_set_dh_param(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    BIO *bio;
    EVP_PKEY *dh;
    int bits;

    if (!c)
        return JS_EXCEPTION;
    if (JS_IsBool(argv[0])) {
        if (JS_ToBool(ctx, argv[0]))
            SSL_CTX_set_dh_auto(c->ctx, 1);
        return JS_UNDEFINED;
    }
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    dh = PEM_read_bio_Parameters(bio, NULL);
    BIO_free(bio);
    if (!dh || !EVP_PKEY_is_a(dh, "DH")) {
        EVP_PKEY_free(dh);
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                             "Failed to parse DH parameters");
    }
    bits = EVP_PKEY_get_bits(dh);
    if (bits < 1024) {
        EVP_PKEY_free(dh);
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_INVALID_ARG_VALUE",
                             "DH parameter is less than 1024 bits");
    }
    if (!SSL_CTX_set0_tmp_dh_pkey(c->ctx, dh)) {
        EVP_PKEY_free(dh);
        return qb_tls_throw(ctx, "Error setting temp DH parameter");
    }
    if (bits < 2048)
        return JS_NewString(ctx, "DH parameter is less than 2048 bits");
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_add_crl(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    BIO *bio;
    X509_CRL *crl;
    X509_STORE *st;

    if (!c)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    crl = PEM_read_bio_X509_CRL(bio, NULL, qb_tls_no_password, NULL);
    BIO_free(bio);
    if (!crl) {
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                             "Failed to parse CRL");
    }
    st = qb_tls_own_store(c);
    if (!st) {
        X509_CRL_free(crl);
        return qb_tls_throw(ctx, "X509_STORE_new failed");
    }
    X509_STORE_add_crl(st, crl);
    X509_STORE_set_flags(st, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    X509_CRL_free(crl);
    ERR_clear_error();
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_session_id_context(JSContext *ctx, JSValueConst this_val, int argc,
                                                 JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    const char *s;
    size_t len;
    int ok;
    if (!c)
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (len > SSL_MAX_SID_CTX_LENGTH)
        len = SSL_MAX_SID_CTX_LENGTH;
    ok = SSL_CTX_set_session_id_context(c->ctx, (const unsigned char *)s, (unsigned int)len);
    JS_FreeCString(ctx, s);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set session id context");
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_session_timeout(JSContext *ctx, JSValueConst this_val, int argc,
                                              JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    int32_t v;
    if (!c || JS_ToInt32(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    if (SSL_CTX_set_timeout(c->ctx, v) < 0)
        ERR_clear_error();
    return JS_UNDEFINED;
}

/* loadPKCS12(pfx, passphrase?) */
static JSValue qb_tls_ctx_load_pkcs12(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    BIO *bio;
    PKCS12 *p12;
    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *extra = NULL;
    char *pass = NULL;
    X509_STORE *st;
    int i, ok;

    if (!c)
        return JS_EXCEPTION;
    bio = qb_tls_bio(ctx, argv[0]);
    if (!bio)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        size_t plen;
        uint8_t *p;
        if (JS_IsString(argv[1])) {
            const char *s = JS_ToCStringLen(ctx, &plen, argv[1]);
            if (!s) {
                BIO_free(bio);
                return JS_EXCEPTION;
            }
            pass = malloc(plen + 1);
            if (pass) {
                memcpy(pass, s, plen);
                pass[plen] = '\0';
            }
            JS_FreeCString(ctx, s);
        } else {
            p = qb_get_bytes(ctx, argv[1], &plen);
            if (!p) {
                BIO_free(bio);
                return JS_EXCEPTION;
            }
            pass = malloc(plen + 1);
            if (pass) {
                memcpy(pass, p, plen);
                pass[plen] = '\0';
            }
        }
        if (!pass) {
            BIO_free(bio);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    ERR_clear_error();
    p12 = d2i_PKCS12_bio(bio, NULL);
    BIO_free(bio);
    ok = p12 && PKCS12_parse(p12, pass ? pass : "", &pkey, &cert, &extra);
    PKCS12_free(p12);
    free(pass);
    if (!ok) {
        unsigned long err = ERR_peek_error();
        if (ERR_GET_LIB(err) == ERR_LIB_PKCS12 &&
            ERR_GET_REASON(err) == PKCS12_R_MAC_VERIFY_FAILURE) {
            ERR_clear_error();
            return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                                 "mac verify failure");
        }
        return qb_tls_throw(ctx, "Unable to load PFX certificate");
    }
    if (!cert) {
        EVP_PKEY_free(pkey);
        sk_X509_pop_free(extra, X509_free);
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                             "Unable to load PFX certificate");
    }
    ok = SSL_CTX_use_certificate(c->ctx, cert);
    st = ok ? qb_tls_own_store(c) : NULL;
    if (ok) {
        SSL_CTX_clear_chain_certs(c->ctx);
        for (i = 0; extra && i < sk_X509_num(extra); i++) {
            X509 *ca = sk_X509_value(extra, i);
            if (!SSL_CTX_add1_chain_cert(c->ctx, ca)) {
                ok = 0;
                break;
            }
            if (st)
                X509_STORE_add_cert(st, ca);
            if (!SSL_CTX_add_client_CA(c->ctx, ca))
                ERR_clear_error();
        }
    }
    if (ok && pkey)
        ok = SSL_CTX_use_PrivateKey(c->ctx, pkey);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    sk_X509_pop_free(extra, X509_free);
    if (!ok)
        return qb_tls_throw(ctx, "Unable to load PFX certificate");
    ERR_clear_error();
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_set_ticket_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    size_t len;
    uint8_t *p;
    if (!c)
        return JS_EXCEPTION;
    p = qb_get_bytes(ctx, argv[0], &len);
    if (!p)
        return JS_EXCEPTION;
    if (len != sizeof(c->ticket_keys))
        return qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE",
                             "Ticket keys length must be 48 bytes");
    memcpy(c->ticket_keys, p, len);
    return JS_UNDEFINED;
}

static JSValue qb_tls_ctx_get_ticket_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return qb_new_buffer(ctx, c->ticket_keys, sizeof(c->ticket_keys));
}

/* the DER of the certificate of the context (null when none) */
static JSValue qb_tls_ctx_get_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return qb_x509_der(ctx, SSL_CTX_get0_certificate(c->ctx));
}

/* the DER of the issuer of the certificate, found in its chain */
static JSValue qb_tls_ctx_get_issuer(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    QBTLSCtx *c = qb_tls_ctx_get(ctx, this_val);
    X509 *cert;
    STACK_OF(X509) *chain = NULL;
    int i;
    if (!c)
        return JS_EXCEPTION;
    cert = SSL_CTX_get0_certificate(c->ctx);
    if (!cert)
        return JS_NULL;
    SSL_CTX_get0_chain_certs(c->ctx, &chain);
    for (i = 0; chain && i < sk_X509_num(chain); i++) {
        X509 *ca = sk_X509_value(chain, i);
        if (X509_check_issued(ca, cert) == X509_V_OK)
            return qb_x509_der(ctx, ca);
    }
    return JS_NULL;
}

static JSValue qb_tls_ctx_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBTLSCtx *c = JS_GetOpaque2(ctx, this_val, qb_tls_ctx_class_id);
    if (!c)
        return JS_EXCEPTION;
    if (c->ctx) {
        qb_tls_ctx_detach(c->ctx);
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_tls_ctx_proto_funcs[] = {
    JS_CFUNC_DEF("setMinProto", 1, qb_tls_ctx_set_min_proto),
    JS_CFUNC_DEF("setMaxProto", 1, qb_tls_ctx_set_max_proto),
    JS_CFUNC_DEF("getMinProto", 0, qb_tls_ctx_get_min_proto),
    JS_CFUNC_DEF("getMaxProto", 0, qb_tls_ctx_get_max_proto),
    JS_CFUNC_DEF("setOptions", 1, qb_tls_ctx_set_options),
    JS_CFUNC_DEF("setCiphers", 1, qb_tls_ctx_set_ciphers),
    JS_CFUNC_DEF("setCipherSuites", 1, qb_tls_ctx_set_cipher_suites),
    JS_CFUNC_DEF("addCACert", 1, qb_tls_ctx_add_ca_cert),
    JS_CFUNC_DEF("addRootCerts", 1, qb_tls_ctx_add_root_certs),
    JS_CFUNC_DEF("setAllowPartialTrustChain", 0, qb_tls_ctx_set_allow_partial),
    JS_CFUNC_DEF("setCert", 1, qb_tls_ctx_set_cert),
    JS_CFUNC_DEF("setKey", 2, qb_tls_ctx_set_key),
    JS_CFUNC_DEF("setECDHCurve", 1, qb_tls_ctx_set_ecdh_curve),
    JS_CFUNC_DEF("setSigalgs", 1, qb_tls_ctx_set_sigalgs),
    JS_CFUNC_DEF("setDHParam", 1, qb_tls_ctx_set_dh_param),
    JS_CFUNC_DEF("addCRL", 1, qb_tls_ctx_add_crl),
    JS_CFUNC_DEF("setSessionIdContext", 1, qb_tls_ctx_set_session_id_context),
    JS_CFUNC_DEF("setSessionTimeout", 1, qb_tls_ctx_set_session_timeout),
    JS_CFUNC_DEF("loadPKCS12", 2, qb_tls_ctx_load_pkcs12),
    JS_CFUNC_DEF("setTicketKeys", 1, qb_tls_ctx_set_ticket_keys),
    JS_CFUNC_DEF("getTicketKeys", 0, qb_tls_ctx_get_ticket_keys),
    JS_CFUNC_DEF("getCertificate", 0, qb_tls_ctx_get_certificate),
    JS_CFUNC_DEF("getIssuer", 0, qb_tls_ctx_get_issuer),
    JS_CFUNC_DEF("close", 0, qb_tls_ctx_close),
};

/* ----------------------------------------------------------- TLSConnection */

struct QBTLSConn {
    JSContext *ctx;
    SSL *ssl;
    BIO *rbio;           /* owned by ssl */
    BIO *wbio;           /* owned by ssl */
    JSValue ctx_obj;     /* the SecureContext (its ticket keys) */
    JSValue sni_ctx_obj; /* the context selected by the SNI callback */
    JSValue events;      /* array of [type, ...payload] or undefined */
    uint32_t nevents;
    JSValue alpn_cb;     /* server ALPNCallback(protocolsBuffer) */
    uint8_t *alpn;       /* wire format protocols */
    unsigned int alpn_len;
    uint8_t *clear;      /* decrypted data not taken yet */
    size_t clear_len, clear_cap;
    int is_server;
    int handshake_done;
    int renegotiating;
    int eof, eof_reported;
    int cert_cb_enabled, cert_cb_pending, cert_cb_done;
    int keylog_enabled, session_enabled;
    int busy;   /* inside an OpenSSL call */
    int closed; /* destroy() was called while busy */
    BIO *trace; /* stderr BIO of enableTrace() */
};

static void qb_tls_conn_free_ssl(JSRuntime *rt, QBTLSConn *c)
{
    if (c->ssl) {
        SSL_free(c->ssl);
        c->ssl = NULL;
        c->rbio = c->wbio = NULL;
    }
    if (c->trace) {
        BIO_free(c->trace);
        c->trace = NULL;
    }
    JS_FreeValueRT(rt, c->sni_ctx_obj);
    c->sni_ctx_obj = JS_UNDEFINED;
    JS_FreeValueRT(rt, c->events);
    c->events = JS_UNDEFINED;
    c->nevents = 0;
    JS_FreeValueRT(rt, c->alpn_cb);
    c->alpn_cb = JS_UNDEFINED;
    js_free_rt(rt, c->alpn);
    c->alpn = NULL;
    c->alpn_len = 0;
    js_free_rt(rt, c->clear);
    c->clear = NULL;
    c->clear_len = c->clear_cap = 0;
}

static void qb_tls_conn_finalizer(JSRuntime *rt, JSValue val)
{
    QBTLSConn *c = JS_GetOpaque(val, qb_tls_conn_class_id);
    if (!c)
        return;
    qb_tls_conn_free_ssl(rt, c);
    JS_FreeValueRT(rt, c->ctx_obj);
    js_free_rt(rt, c);
}

static void qb_tls_conn_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBTLSConn *c = JS_GetOpaque(val, qb_tls_conn_class_id);
    if (!c)
        return;
    JS_MarkValue(rt, c->ctx_obj, mark_func);
    JS_MarkValue(rt, c->sni_ctx_obj, mark_func);
    JS_MarkValue(rt, c->events, mark_func);
    JS_MarkValue(rt, c->alpn_cb, mark_func);
}

static JSClassDef qb_tls_conn_class = {
    "TLSConnection",
    .finalizer = qb_tls_conn_finalizer,
    .gc_mark = qb_tls_conn_mark,
};

/* the connection of 'obj' (its ssl is NULL once destroyed) */
static QBTLSConn *qb_tls_conn_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qb_tls_conn_class_id);
}

static void qb_tls_push_event(QBTLSConn *c, JSValue ev)
{
    JSContext *ctx = c->ctx;
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (JS_IsUndefined(c->events)) {
        c->events = JS_NewArray(ctx);
        if (JS_IsException(c->events)) {
            c->events = JS_UNDEFINED;
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, ev);
            return;
        }
    }
    JS_SetPropertyUint32(ctx, c->events, c->nevents++, ev);
}

static void qb_tls_keylog_cb(const SSL *ssl, const char *line)
{
    QBTLSConn *c = SSL_get_app_data(ssl);
    JSContext *ctx;
    JSValue ev;
    size_t len;
    uint8_t *buf;

    if (!c || !c->keylog_enabled)
        return;
    ctx = c->ctx;
    len = strlen(line);
    buf = js_malloc_rt(JS_GetRuntime(ctx), len + 1);
    if (!buf)
        return;
    memcpy(buf, line, len);
    buf[len] = '\n';
    ev = JS_NewArray(ctx);
    if (JS_IsException(ev)) {
        js_free_rt(JS_GetRuntime(ctx), buf);
        qb_tls_push_event(c, ev);
        return;
    }
    JS_SetPropertyUint32(ctx, ev, 0, JS_NewString(ctx, "keylog"));
    JS_SetPropertyUint32(ctx, ev, 1, qb_new_buffer_owned(ctx, buf, len + 1));
    qb_tls_push_event(c, ev);
}

static int qb_tls_new_session_cb(SSL *ssl, SSL_SESSION *sess)
{
    QBTLSConn *c = SSL_get_app_data(ssl);
    JSContext *ctx;
    JSValue ev;
    int len;
    uint8_t *buf;
    unsigned char *p;
    const unsigned char *id;
    unsigned int idlen = 0;

    if (!c || !c->session_enabled)
        return 0;
    ctx = c->ctx;
    len = i2d_SSL_SESSION(sess, NULL);
    if (len <= 0)
        return 0;
    buf = js_malloc_rt(JS_GetRuntime(ctx), (size_t)len);
    if (!buf)
        return 0;
    p = buf;
    i2d_SSL_SESSION(sess, &p);
    id = SSL_SESSION_get_id(sess, &idlen);
    ev = JS_NewArray(ctx);
    if (JS_IsException(ev)) {
        js_free_rt(JS_GetRuntime(ctx), buf);
        qb_tls_push_event(c, ev);
        return 0;
    }
    JS_SetPropertyUint32(ctx, ev, 0, JS_NewString(ctx, "session"));
    JS_SetPropertyUint32(ctx, ev, 1, qb_new_buffer(ctx, id, idlen));
    JS_SetPropertyUint32(ctx, ev, 2, qb_new_buffer_owned(ctx, buf, (size_t)len));
    qb_tls_push_event(c, ev);
    /* the session is not kept */
    return 0;
}

static int qb_tls_alpn_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg)
{
    QBTLSConn *c = SSL_get_app_data(ssl);
    unsigned char *sel = NULL;
    unsigned char sel_len = 0;
    (void)arg;

    if (!c)
        return SSL_TLSEXT_ERR_NOACK;
    if (JS_IsFunction(c->ctx, c->alpn_cb)) {
        JSContext *ctx = c->ctx;
        JSValue buf = qb_new_buffer(ctx, in, inlen);
        JSValue func = JS_DupValue(ctx, c->alpn_cb);
        JSValue ret;
        int32_t off;
        if (JS_IsException(buf)) {
            JS_FreeValue(ctx, func);
            JS_FreeValue(ctx, JS_GetException(ctx));
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        ret = JS_Call(ctx, func, JS_UNDEFINED, 1, (JSValueConst *)&buf);
        JS_FreeValue(ctx, func);
        JS_FreeValue(ctx, buf);
        if (JS_IsException(ret)) {
            qb_report_exception(ctx);
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (!JS_IsNumber(ret)) {
            JS_FreeValue(ctx, ret);
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (JS_ToInt32(ctx, &off, ret)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (off < 0 || (unsigned int)off >= inlen || (unsigned int)off + 1 + in[off] > inlen ||
            in[off] == 0)
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        *out = in + off + 1;
        *outlen = in[off];
        return SSL_TLSEXT_ERR_OK;
    }
    if (!c->alpn || c->alpn_len == 0)
        return SSL_TLSEXT_ERR_NOACK;
    if (SSL_select_next_proto(&sel, &sel_len, c->alpn, c->alpn_len, in, inlen) !=
        OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    *out = sel;
    *outlen = sel_len;
    return SSL_TLSEXT_ERR_OK;
}

static int qb_tls_cert_cb(SSL *ssl, void *arg)
{
    QBTLSConn *c = SSL_get_app_data(ssl);
    (void)arg;
    if (!c || !c->is_server || !c->cert_cb_enabled || c->cert_cb_done)
        return 1;
    /* pause the handshake until certCbDone() (SNICallback) */
    c->cert_cb_pending = 1;
    return -1;
}

static int qb_tls_flags(QBTLSConn *c)
{
    int flags = 0;
    if (!c->ssl)
        return 0;
    if (c->clear_len)
        flags |= QB_TLS_CLEAR;
    if (BIO_ctrl_pending(c->wbio))
        flags |= QB_TLS_ENC;
    if (c->eof && !c->eof_reported) {
        c->eof_reported = 1;
        flags |= QB_TLS_EOF;
    }
    if (c->nevents)
        flags |= QB_TLS_EVENTS;
    if (c->cert_cb_pending)
        flags |= QB_TLS_CERTCB;
    if (c->handshake_done)
        flags |= QB_TLS_SECURE;
    return flags;
}

/* free the SSL if destroy() was called during the last OpenSSL call */
static int qb_tls_conn_settle(QBTLSConn *c)
{
    if (c->closed && !c->busy && c->ssl) {
        qb_tls_conn_free_ssl(JS_GetRuntime(c->ctx), c);
        return 1;
    }
    return 0;
}

static JSValue qb_tls_ssl_error(JSContext *ctx, QBTLSConn *c, int e)
{
    if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ECONNRESET", "socket hang up");
    }
    return qb_tls_throw(ctx, "TLS error");
}

static int qb_tls_grow_clear(QBTLSConn *c, size_t need)
{
    size_t cap;
    uint8_t *p;
    if (c->clear_cap - c->clear_len >= need)
        return 0;
    cap = c->clear_cap ? c->clear_cap : QB_TLS_READ_CHUNK * 2;
    while (cap - c->clear_len < need)
        cap *= 2;
    p = js_realloc_rt(JS_GetRuntime(c->ctx), c->clear, cap);
    if (!p)
        return -1;
    c->clear = p;
    c->clear_cap = cap;
    return 0;
}

/* run the handshake and decrypt what can be decrypted; return the flags
   or -1 with an exception */
static int qb_tls_cycle(JSContext *ctx, QBTLSConn *c)
{
    int r, e, flags = 0, ret;

    if (!c->ssl)
        return 0;
    c->busy++;
    ERR_clear_error();
    if (!c->handshake_done) {
        if (c->cert_cb_pending)
            goto done;
        r = SSL_do_handshake(c->ssl);
        if (c->closed)
            goto done;
        if (r == 1) {
            c->handshake_done = 1;
            flags |= QB_TLS_HANDSHAKE_DONE;
        } else {
            e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_X509_LOOKUP || c->cert_cb_pending) {
                ERR_clear_error();
                goto done;
            }
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                c->busy--;
                qb_tls_ssl_error(ctx, c, e);
                qb_tls_conn_settle(c);
                return -1;
            }
            ERR_clear_error();
            goto done;
        }
    }
    while (!c->eof) {
        size_t n = 0;
        if (qb_tls_grow_clear(c, QB_TLS_READ_CHUNK) < 0) {
            c->busy--;
            JS_ThrowOutOfMemory(ctx);
            qb_tls_conn_settle(c);
            return -1;
        }
        r = SSL_read_ex(c->ssl, c->clear + c->clear_len, QB_TLS_READ_CHUNK, &n);
        if (c->closed)
            goto done;
        if (r == 1) {
            c->clear_len += n;
            continue;
        }
        e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            ERR_clear_error();
            break;
        }
        if (e == SSL_ERROR_ZERO_RETURN) {
            c->eof = 1;
            ERR_clear_error();
            break;
        }
        c->busy--;
        qb_tls_ssl_error(ctx, c, e);
        qb_tls_conn_settle(c);
        return -1;
    }
    if (c->renegotiating && !SSL_renegotiate_pending(c->ssl)) {
        c->renegotiating = 0;
        flags |= QB_TLS_HANDSHAKE_DONE;
    }
done:
    c->busy--;
    if (qb_tls_conn_settle(c))
        return 0;
    ret = flags | qb_tls_flags(c);
    return ret;
}

static JSValue qb_tls_flags_value(JSContext *ctx, int flags)
{
    if (flags < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, flags);
}

/* createConnection(secureContext, isServer) */
static JSValue qb_tls_create_conn(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    QBTLSCtx *sc = qb_tls_ctx_get(ctx, argv[0]);
    QBTLSConn *c;
    JSValue obj;
    BIO *rb, *wb;

    if (!sc)
        return JS_EXCEPTION;
    c = js_mallocz(ctx, sizeof(*c));
    if (!c)
        return JS_EXCEPTION;
    c->ctx = ctx;
    c->ctx_obj = JS_UNDEFINED;
    c->sni_ctx_obj = JS_UNDEFINED;
    c->events = JS_UNDEFINED;
    c->alpn_cb = JS_UNDEFINED;
    c->is_server = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    c->ssl = SSL_new(sc->ctx);
    if (!c->ssl) {
        js_free(ctx, c);
        return qb_tls_throw(ctx, "SSL_new failed");
    }
    rb = BIO_new(BIO_s_mem());
    wb = BIO_new(BIO_s_mem());
    if (!rb || !wb) {
        BIO_free(rb);
        BIO_free(wb);
        SSL_free(c->ssl);
        js_free(ctx, c);
        return qb_tls_throw(ctx, "BIO_new failed");
    }
    BIO_set_mem_eof_return(rb, -1);
    BIO_set_mem_eof_return(wb, -1);
    SSL_set_bio(c->ssl, rb, wb);
    c->rbio = rb;
    c->wbio = wb;
    if (SSL_set_app_data(c->ssl, c) != 1) {
        SSL_free(c->ssl);
        js_free(ctx, c);
        return qb_tls_throw(ctx, "SSL_set_ex_data failed");
    }
    SSL_set_mode(c->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_verify(c->ssl, SSL_VERIFY_NONE, qb_tls_verify_cb);
    if (c->is_server)
        SSL_set_accept_state(c->ssl);
    else
        SSL_set_connect_state(c->ssl);

    obj = JS_NewObjectClass(ctx, (int)qb_tls_conn_class_id);
    if (JS_IsException(obj)) {
        SSL_free(c->ssl);
        js_free(ctx, c);
        return obj;
    }
    c->ctx_obj = JS_DupValue(ctx, argv[0]);
    JS_SetOpaque(obj, c);
    return obj;
}

/* methods that drive the engine refuse reentrant calls (from the ALPN
   callback) */
static QBTLSConn *qb_tls_conn_enter(JSContext *ctx, JSValueConst obj)
{
    QBTLSConn *c = qb_tls_conn_get(ctx, obj);
    if (c && c->busy) {
        JS_ThrowTypeError(ctx, "reentrant call of the TLS engine");
        return NULL;
    }
    return c;
}

/* start(): send the ClientHello (clients); return the flags */
static JSValue qb_tls_conn_start(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return qb_tls_flags_value(ctx, qb_tls_cycle(ctx, c));
}

/* receive(encrypted): feed records from the peer; return the flags */
static JSValue qb_tls_conn_receive(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    uint8_t *p;
    size_t len;

    if (!c)
        return JS_EXCEPTION;
    p = qb_get_bytes(ctx, argv[0], &len);
    if (!p)
        return JS_EXCEPTION;
    if (!c->ssl)
        return JS_NewInt32(ctx, 0);
    while (len > 0) {
        int n = len > INT_MAX ? INT_MAX : (int)len;
        int w = BIO_write(c->rbio, p, n);
        if (w <= 0)
            return qb_tls_throw(ctx, "BIO_write failed");
        p += w;
        len -= (size_t)w;
    }
    return qb_tls_flags_value(ctx, qb_tls_cycle(ctx, c));
}

/* readCleartext(): the decrypted data (Buffer) or null */
static JSValue qb_tls_conn_read_cleartext(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_get(ctx, this_val);
    JSValue buf;
    if (!c)
        return JS_EXCEPTION;
    if (!c->clear_len)
        return JS_NULL;
    buf = qb_new_buffer(ctx, c->clear, c->clear_len);
    if (!JS_IsException(buf))
        c->clear_len = 0;
    return buf;
}

/* writeCleartext(data): encrypt; return the number of bytes consumed (0
   while a renegotiation needs data from the peer) */
static JSValue qb_tls_conn_write_cleartext(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    uint8_t *p;
    size_t len, written = 0;
    int r, e;

    if (!c)
        return JS_EXCEPTION;
    if (JS_IsString(argv[0])) {
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (!c->ssl || len == 0) {
            JS_FreeCString(ctx, s);
            return JS_NewInt32(ctx, 0);
        }
        c->busy++;
        ERR_clear_error();
        r = SSL_write_ex(c->ssl, s, len, &written);
        c->busy--;
        JS_FreeCString(ctx, s);
    } else {
        p = qb_get_bytes(ctx, argv[0], &len);
        if (!p)
            return JS_EXCEPTION;
        if (!c->ssl || len == 0)
            return JS_NewInt32(ctx, 0);
        c->busy++;
        ERR_clear_error();
        r = SSL_write_ex(c->ssl, p, len, &written);
        c->busy--;
    }
    if (r != 1) {
        e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            ERR_clear_error();
            return JS_NewInt32(ctx, 0);
        }
        return qb_tls_ssl_error(ctx, c, e);
    }
    return JS_NewInt64(ctx, (int64_t)written);
}

/* takeEncrypted(): the records to send (Buffer) or null */
static JSValue qb_tls_conn_take_encrypted(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_get(ctx, this_val);
    size_t pending;
    uint8_t *buf;
    int n;

    if (!c)
        return JS_EXCEPTION;
    if (!c->ssl)
        return JS_NULL;
    pending = BIO_ctrl_pending(c->wbio);
    if (pending == 0)
        return JS_NULL;
    if (pending > INT_MAX)
        pending = INT_MAX;
    buf = js_malloc_rt(JS_GetRuntime(ctx), pending);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    n = BIO_read(c->wbio, buf, (int)pending);
    if (n <= 0) {
        js_free_rt(JS_GetRuntime(ctx), buf);
        return JS_NULL;
    }
    return qb_new_buffer_owned(ctx, buf, (size_t)n);
}

/* shutdown(): send close_notify; return the flags */
static JSValue qb_tls_conn_shutdown(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (!c->ssl)
        return JS_NewInt32(ctx, 0);
    if (SSL_is_init_finished(c->ssl)) {
        c->busy++;
        SSL_shutdown(c->ssl);
        c->busy--;
    }
    ERR_clear_error();
    if (qb_tls_conn_settle(c))
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, qb_tls_flags(c));
}

/* destroy(): free the SSL (deferred when called from a callback) */
static JSValue qb_tls_conn_destroy(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    c->closed = 1;
    qb_tls_conn_settle(c);
    return JS_UNDEFINED;
}

/* declare 'c', the connection of 'this_val'; return 'dead' when it was
   destroyed */
#define QB_TLS_SSL(c, ctx, this_val, dead)                                                         \
    QBTLSConn *c = qb_tls_conn_get(ctx, this_val);                                                 \
    if (!c)                                                                                        \
        return JS_EXCEPTION;                                                                       \
    if (!c->ssl)                                                                                   \
        return dead;

static JSValue qb_tls_conn_set_verify_mode(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    int mode = SSL_VERIFY_NONE;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    if (c->is_server && JS_ToBool(ctx, argv[0]) > 0) {
        mode = SSL_VERIFY_PEER;
        if (argc > 1 && JS_ToBool(ctx, argv[1]) > 0)
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
    SSL_set_verify(c->ssl, mode, qb_tls_verify_cb);
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_set_servername(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    const char *s;
    int ok;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    ok = (int)SSL_set_tlsext_host_name(c->ssl, s);
    JS_FreeCString(ctx, s);
    if (!ok)
        return qb_tls_throw(ctx, "Failed to set the server name");
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_get_servername(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    const char *s;
    QB_TLS_SSL(c, ctx, this_val, JS_FALSE)
    s = SSL_get_servername(c->ssl, TLSEXT_NAMETYPE_host_name);
    if (!s)
        return JS_FALSE;
    return JS_NewString(ctx, s);
}

/* setALPNProtocols(wire): the protocols offered (client) or accepted
   (server), in the wire format */
static JSValue qb_tls_conn_set_alpn(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    uint8_t *p, *copy;
    size_t len;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    p = qb_get_bytes(ctx, argv[0], &len);
    if (!p)
        return JS_EXCEPTION;
    if (len > UINT_MAX)
        return JS_ThrowRangeError(ctx, "ALPN protocols too long");
    if (c->is_server) {
        copy = len ? js_malloc_rt(JS_GetRuntime(ctx), len) : NULL;
        if (len && !copy)
            return JS_ThrowOutOfMemory(ctx);
        if (len)
            memcpy(copy, p, len);
        js_free_rt(JS_GetRuntime(ctx), c->alpn);
        c->alpn = copy;
        c->alpn_len = (unsigned int)len;
    } else if (SSL_set_alpn_protos(c->ssl, p, (unsigned int)len) != 0) {
        return qb_tls_throw(ctx, "Failed to set the ALPN protocols");
    }
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_set_alpn_callback(JSContext *ctx, JSValueConst this_val, int argc,
                                             JSValueConst *argv)
{
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    JS_FreeValue(ctx, c->alpn_cb);
    c->alpn_cb = JS_IsFunction(ctx, argv[0]) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_get_alpn(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    const unsigned char *data = NULL;
    unsigned int len = 0;
    QB_TLS_SSL(c, ctx, this_val, JS_FALSE)
    SSL_get0_alpn_selected(c->ssl, &data, &len);
    if (!data || len == 0)
        return JS_FALSE;
    return JS_NewStringLen(ctx, (const char *)data, len);
}

/* enable(kind): 0 keylog events, 1 session events, 2 the certificate
   callback (pause for SNICallback) */
static JSValue qb_tls_conn_enable(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv, int magic)
{
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    switch (magic) {
    case 0:
        c->keylog_enabled = 1;
        break;
    case 1:
        c->session_enabled = 1;
        break;
    default:
        c->cert_cb_enabled = 1;
        break;
    }
    return JS_UNDEFINED;
}

/* certCbDone(): resume the paused handshake; return the flags */
static JSValue qb_tls_conn_cert_cb_done(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    c->cert_cb_pending = 0;
    c->cert_cb_done = 1;
    return qb_tls_flags_value(ctx, qb_tls_cycle(ctx, c));
}

/* setSNIContext(secureContext): use the certificate, key and chain of
   another context (Node's UseSNIContext) */
static JSValue qb_tls_conn_set_sni_context(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    QBTLSCtx *sc;
    X509 *x;
    EVP_PKEY *pkey;
    STACK_OF(X509) *chain = NULL;
    int ok;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    sc = qb_tls_ctx_get(ctx, argv[0]);
    if (!sc)
        return JS_EXCEPTION;
    x = SSL_CTX_get0_certificate(sc->ctx);
    pkey = SSL_CTX_get0_privatekey(sc->ctx);
    ok = (int)SSL_CTX_get0_chain_certs(sc->ctx, &chain);
    if (ok == 1 && x)
        ok = SSL_use_certificate(c->ssl, x);
    if (ok == 1 && pkey)
        ok = SSL_use_PrivateKey(c->ssl, pkey);
    if (ok == 1 && chain)
        ok = (int)SSL_set1_chain(c->ssl, chain);
    if (ok != 1)
        return qb_tls_throw(ctx, "Failed to use the SNI context");
    JS_FreeValue(ctx, c->sni_ctx_obj);
    c->sni_ctx_obj = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* takeEvents(): [["keylog", line], ["session", id, session], ...] or
   undefined */
static JSValue qb_tls_conn_take_events(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_get(ctx, this_val);
    JSValue ev;
    if (!c)
        return JS_EXCEPTION;
    ev = c->events;
    c->events = JS_UNDEFINED;
    c->nevents = 0;
    return ev;
}

/* verifyError(): null or an Error with Node's X509 code */
static JSValue qb_tls_conn_verify_error(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    long err;
    const char *msg;
    JSValue e;
    QB_TLS_SSL(c, ctx, this_val, JS_NULL)
    if (SSL_get0_peer_certificate(c->ssl)) {
        err = SSL_get_verify_result(c->ssl);
    } else {
        const SSL_CIPHER *cipher = SSL_get_current_cipher(c->ssl);
        SSL_SESSION *sess = SSL_get_session(c->ssl);
        /* PSK (TLSv1.2) or TLSv1.3 resumption: no certificate */
        if ((cipher && SSL_CIPHER_get_auth_nid(cipher) == NID_auth_psk) ||
            (sess && SSL_SESSION_get_protocol_version(sess) == TLS1_3_VERSION &&
             SSL_session_reused(c->ssl)))
            return JS_NULL;
        err = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT;
    }
    if (err == X509_V_OK)
        return JS_NULL;
    msg = X509_verify_cert_error_string(err);
    if (!msg)
        msg = "certificate verification failed";
    e = JS_NewErrorLen(ctx, JS_PLAIN_ERROR, msg, strlen(msg));
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, qb_x509_err_code(err)),
                              JS_PROP_C_W_E);
    return e;
}

static JSValue qb_tls_conn_has_peer_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                                JSValueConst *argv)
{
    QB_TLS_SSL(c, ctx, this_val, JS_FALSE)
    return JS_NewBool(ctx, SSL_get0_peer_certificate(c->ssl) != NULL);
}

#define QB_TLS_MAX_CHAIN 32

/* getPeerCertificate(detailed): the certificate object of the peer; with
   'detailed', issuerCertificate links the chain (the root links to
   itself) */
static JSValue qb_tls_conn_get_peer_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                                JSValueConst *argv)
{
    X509 *leaf;
    X509 *cand[QB_TLS_MAX_CHAIN];
    int ncand = 0, used[QB_TLS_MAX_CHAIN];
    STACK_OF(X509) * chains[2];
    JSValue result, cur_obj;
    X509 *cur;
    int detailed, i, k;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)

    detailed = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    leaf = SSL_get0_peer_certificate(c->ssl);
    if (!leaf) {
        STACK_OF(X509) *pc = SSL_get_peer_cert_chain(c->ssl);
        if (!pc || sk_X509_num(pc) == 0)
            return JS_UNDEFINED;
        leaf = sk_X509_value(pc, 0);
    }
    result = qb_x509_to_object(ctx, leaf);
    if (JS_IsException(result) || !detailed)
        return result;

    chains[0] = SSL_get0_verified_chain(c->ssl);
    chains[1] = SSL_get_peer_cert_chain(c->ssl);
    for (k = 0; k < 2; k++) {
        for (i = 0; chains[k] && i < sk_X509_num(chains[k]) && ncand < QB_TLS_MAX_CHAIN; i++) {
            X509 *x = sk_X509_value(chains[k], i);
            int j, dup = 0;
            if (X509_cmp(x, leaf) == 0)
                continue;
            for (j = 0; j < ncand; j++) {
                if (X509_cmp(x, cand[j]) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                used[ncand] = 0;
                cand[ncand++] = x;
            }
        }
    }
    cur = leaf;
    cur_obj = JS_DupValue(ctx, result);
    for (;;) {
        int found = -1;
        JSValue next;
        for (i = 0; i < ncand; i++) {
            if (!used[i] && X509_check_issued(cand[i], cur) == X509_V_OK) {
                found = i;
                break;
            }
        }
        if (found < 0)
            break;
        used[found] = 1;
        next = qb_x509_to_object(ctx, cand[found]);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, cur_obj);
            JS_FreeValue(ctx, result);
            return next;
        }
        JS_SetPropertyStr(ctx, cur_obj, "issuerCertificate", JS_DupValue(ctx, next));
        JS_FreeValue(ctx, cur_obj);
        cur_obj = next;
        cur = cand[found];
    }
    if (X509_check_issued(cur, cur) == X509_V_OK)
        JS_SetPropertyStr(ctx, cur_obj, "issuerCertificate", JS_DupValue(ctx, cur_obj));
    JS_FreeValue(ctx, cur_obj);
    ERR_clear_error();
    return result;
}

/* getCertificate(): the local certificate object or null */
static JSValue qb_tls_conn_get_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    X509 *x;
    QB_TLS_SSL(c, ctx, this_val, JS_NULL)
    x = SSL_get_certificate(c->ssl);
    if (!x)
        return JS_NULL;
    return qb_x509_to_object(ctx, x);
}

static JSValue qb_tls_conn_get_protocol(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    const char *v;
    QB_TLS_SSL(c, ctx, this_val, JS_NULL)
    if (!SSL_is_init_finished(c->ssl))
        return JS_NewString(ctx, "unknown");
    v = SSL_get_version(c->ssl);
    return JS_NewString(ctx, v ? v : "unknown");
}

static JSValue qb_tls_conn_get_cipher(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    const SSL_CIPHER *cipher;
    const char *s;
    JSValue o;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    cipher = SSL_get_current_cipher(c->ssl);
    if (!cipher)
        return JS_UNDEFINED;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    s = SSL_CIPHER_get_name(cipher);
    qb_set(ctx, o, "name", JS_NewString(ctx, s ? s : ""));
    s = SSL_CIPHER_standard_name(cipher);
    qb_set(ctx, o, "standardName", JS_NewString(ctx, s ? s : ""));
    s = SSL_CIPHER_get_version(cipher);
    qb_set(ctx, o, "version", JS_NewString(ctx, s ? s : ""));
    return o;
}

static JSValue qb_tls_conn_get_shared_sigalgs(JSContext *ctx, JSValueConst this_val, int argc,
                                              JSValueConst *argv)
{
    JSValue arr;
    int i, n;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    n = SSL_get_shared_sigalgs(c->ssl, 0, NULL, NULL, NULL, NULL, NULL);
    for (i = 0; i < n; i++) {
        int sign = NID_undef, hash = NID_undef;
        char buf[128];
        const char *prefix;
        char other[80];
        SSL_get_shared_sigalgs(c->ssl, i, &sign, &hash, NULL, NULL, NULL);
        switch (sign) {
        case NID_rsaEncryption:
            prefix = "RSA+";
            break;
        case NID_rsassaPss:
            prefix = "RSA-PSS+";
            break;
        case NID_dsa:
            prefix = "DSA+";
            break;
        case NID_X9_62_id_ecPublicKey:
            prefix = "ECDSA+";
            break;
        case NID_ED25519:
            prefix = "Ed25519+";
            break;
        case NID_ED448:
            prefix = "Ed448+";
            break;
        default: {
            const char *ln = OBJ_nid2ln(sign);
            snprintf(other, sizeof(other), "%s+", ln ? ln : "UNDEF");
            prefix = other;
            break;
        }
        }
        snprintf(buf, sizeof(buf), "%s%s", prefix,
                 hash != NID_undef && OBJ_nid2sn(hash) ? OBJ_nid2sn(hash) : "UNDEF");
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, buf));
    }
    return arr;
}

static JSValue qb_tls_conn_get_ephemeral_key_info(JSContext *ctx, JSValueConst this_val, int argc,
                                                  JSValueConst *argv)
{
    JSValue o;
    EVP_PKEY *key = NULL;
    QB_TLS_SSL(c, ctx, this_val, JS_NULL)
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || c->is_server)
        return o;
    if (SSL_get_peer_tmp_key(c->ssl, &key) && key) {
        int bits = EVP_PKEY_get_bits(key);
        if (EVP_PKEY_is_a(key, "DH")) {
            qb_set(ctx, o, "type", JS_NewString(ctx, "DH"));
            qb_set(ctx, o, "size", JS_NewInt32(ctx, bits));
        } else if (EVP_PKEY_is_a(key, "EC")) {
            char group[80];
            size_t glen = 0;
            qb_set(ctx, o, "type", JS_NewString(ctx, "ECDH"));
            if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group,
                                               sizeof(group), &glen)) {
                int nid = OBJ_sn2nid(group);
                const char *nist = nid != NID_undef ? EC_curve_nid2nist(nid) : NULL;
                qb_set(ctx, o, "name", JS_NewString(ctx, nist ? nist : group));
            }
            qb_set(ctx, o, "size", JS_NewInt32(ctx, bits));
        } else if (EVP_PKEY_is_a(key, "X25519") || EVP_PKEY_is_a(key, "X448")) {
            qb_set(ctx, o, "type", JS_NewString(ctx, "ECDH"));
            qb_set(ctx, o, "name",
                   JS_NewString(ctx, EVP_PKEY_is_a(key, "X25519") ? "X25519" : "X448"));
            qb_set(ctx, o, "size", JS_NewInt32(ctx, bits));
        } else {
            const char *tn = EVP_PKEY_get0_type_name(key);
            const char *gn = SSL_get0_group_name(c->ssl);
            qb_set(ctx, o, "type", JS_NewString(ctx, "ECDH"));
            qb_set(ctx, o, "name", JS_NewString(ctx, gn ? gn : tn ? tn : "unknown"));
            qb_set(ctx, o, "size", JS_NewInt32(ctx, bits));
        }
        EVP_PKEY_free(key);
    } else if (SSL_get0_group_name(c->ssl)) {
        /* hybrid post-quantum groups have no peer key object */
        qb_set(ctx, o, "type", JS_NewString(ctx, "ECDH"));
        qb_set(ctx, o, "name", JS_NewString(ctx, SSL_get0_group_name(c->ssl)));
    }
    ERR_clear_error();
    return o;
}

/* getFinished() (magic 0) / getPeerFinished() (magic 1) */
static JSValue qb_tls_conn_get_finished(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv, int magic)
{
    unsigned char buf[EVP_MAX_MD_SIZE * 2];
    size_t n;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    n = magic ? SSL_get_peer_finished(c->ssl, buf, sizeof(buf))
              : SSL_get_finished(c->ssl, buf, sizeof(buf));
    if (n == 0)
        return JS_UNDEFINED;
    if (n > sizeof(buf))
        n = sizeof(buf);
    return qb_new_buffer(ctx, buf, n);
}

static JSValue qb_tls_conn_get_session(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    SSL_SESSION *sess;
    int len;
    uint8_t *buf;
    unsigned char *p;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    sess = SSL_get_session(c->ssl);
    if (!sess)
        return JS_UNDEFINED;
    len = i2d_SSL_SESSION(sess, NULL);
    if (len <= 0)
        return JS_UNDEFINED;
    buf = js_malloc_rt(JS_GetRuntime(ctx), (size_t)len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    p = buf;
    i2d_SSL_SESSION(sess, &p);
    return qb_new_buffer_owned(ctx, buf, (size_t)len);
}

static JSValue qb_tls_conn_set_session(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    uint8_t *p;
    size_t len;
    const unsigned char *q;
    SSL_SESSION *sess;
    int ok;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    p = qb_get_bytes(ctx, argv[0], &len);
    if (!p)
        return JS_EXCEPTION;
    if (len > LONG_MAX)
        len = LONG_MAX;
    q = p;
    sess = d2i_SSL_SESSION(NULL, &q, (long)len);
    if (!sess) {
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_TLS_INVALID_SESSION", "Bad session");
    }
    ok = SSL_set_session(c->ssl, sess);
    SSL_SESSION_free(sess);
    if (!ok)
        return qb_tls_throw(ctx, "SSL_set_session failed");
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_is_session_reused(JSContext *ctx, JSValueConst this_val, int argc,
                                             JSValueConst *argv)
{
    QB_TLS_SSL(c, ctx, this_val, JS_FALSE)
    return JS_NewBool(ctx, SSL_session_reused(c->ssl) != 0);
}

static JSValue qb_tls_conn_get_tls_ticket(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    SSL_SESSION *sess;
    const unsigned char *tick = NULL;
    size_t len = 0;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    sess = SSL_get_session(c->ssl);
    if (!sess)
        return JS_UNDEFINED;
    SSL_SESSION_get0_ticket(sess, &tick, &len);
    if (!tick || len == 0)
        return JS_UNDEFINED;
    return qb_new_buffer(ctx, tick, len);
}

/* exportKeyingMaterial(length, label, context?) */
static JSValue qb_tls_conn_export_keying_material(JSContext *ctx, JSValueConst this_val, int argc,
                                                  JSValueConst *argv)
{
    uint64_t olen;
    const char *label;
    size_t llen, clen = 0;
    uint8_t *cp = NULL, *out;
    int use_ctx = 0, ok;
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
    if (JS_ToIndex(ctx, &olen, argv[0]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        cp = qb_get_bytes(ctx, argv[2], &clen);
        if (!cp)
            return JS_EXCEPTION;
        use_ctx = 1;
    }
    label = JS_ToCStringLen(ctx, &llen, argv[1]);
    if (!label)
        return JS_EXCEPTION;
    /* the context bytes may move after a JS call: fetch them again */
    if (use_ctx) {
        cp = qb_get_bytes(ctx, argv[2], &clen);
        if (!cp) {
            JS_FreeCString(ctx, label);
            return JS_EXCEPTION;
        }
    }
    out = js_malloc_rt(JS_GetRuntime(ctx), olen ? (size_t)olen : 1);
    if (!out) {
        JS_FreeCString(ctx, label);
        return JS_ThrowOutOfMemory(ctx);
    }
    ok = SSL_export_keying_material(c->ssl, out, (size_t)olen, label, llen, cp, clen, use_ctx);
    JS_FreeCString(ctx, label);
    if (ok != 1) {
        js_free_rt(JS_GetRuntime(ctx), out);
        ERR_clear_error();
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_CRYPTO_OPERATION_FAILED",
                             "Key extraction failed");
    }
    return qb_new_buffer_owned(ctx, out, (size_t)olen);
}

/* renegotiate(): start a TLSv1.2 renegotiation (throws for TLSv1.3) */
static JSValue qb_tls_conn_renegotiate(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    QBTLSConn *c = qb_tls_conn_enter(ctx, this_val);
    int r;
    if (!c)
        return JS_EXCEPTION;
    if (!c->ssl)
        return JS_UNDEFINED;
    ERR_clear_error();
    if (SSL_renegotiate(c->ssl) != 1)
        return qb_tls_throw(ctx, "Renegotiation failed");
    c->renegotiating = 1;
    c->busy++;
    r = SSL_do_handshake(c->ssl);
    c->busy--;
    if (r != 1) {
        int e = SSL_get_error(c->ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            return qb_tls_ssl_error(ctx, c, e);
    }
    ERR_clear_error();
    qb_tls_conn_settle(c);
    return JS_UNDEFINED;
}

static JSValue qb_tls_conn_set_max_send_fragment(JSContext *ctx, JSValueConst this_val, int argc,
                                                 JSValueConst *argv)
{
    int32_t n;
    QB_TLS_SSL(c, ctx, this_val, JS_FALSE)
    if (JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    if (n < 512 || n > 16384)
        return JS_FALSE;
    return JS_NewBool(ctx, SSL_set_max_send_fragment(c->ssl, n) == 1);
}

/* enableTrace(): print the TLS messages to stderr (Node's --trace-tls) */
static JSValue qb_tls_conn_enable_trace(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    QB_TLS_SSL(c, ctx, this_val, JS_UNDEFINED)
#ifndef OPENSSL_NO_SSL_TRACE
    if (!c->trace) {
        c->trace = BIO_new_fp(stderr, BIO_NOCLOSE);
        if (!c->trace)
            return qb_tls_throw(ctx, "BIO_new_fp failed");
    }
    SSL_set_msg_callback(c->ssl, SSL_trace);
    SSL_set_msg_callback_arg(c->ssl, c->trace);
    return JS_UNDEFINED;
#else
    return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_NOT_SUPPORTED",
                         "enableTrace is not supported by this OpenSSL build");
#endif
}

static const JSCFunctionListEntry qb_tls_conn_proto_funcs[] = {
    JS_CFUNC_DEF("start", 0, qb_tls_conn_start),
    JS_CFUNC_DEF("receive", 1, qb_tls_conn_receive),
    JS_CFUNC_DEF("readCleartext", 0, qb_tls_conn_read_cleartext),
    JS_CFUNC_DEF("writeCleartext", 1, qb_tls_conn_write_cleartext),
    JS_CFUNC_DEF("takeEncrypted", 0, qb_tls_conn_take_encrypted),
    JS_CFUNC_DEF("shutdown", 0, qb_tls_conn_shutdown),
    JS_CFUNC_DEF("destroy", 0, qb_tls_conn_destroy),
    JS_CFUNC_DEF("setVerifyMode", 2, qb_tls_conn_set_verify_mode),
    JS_CFUNC_DEF("setServername", 1, qb_tls_conn_set_servername),
    JS_CFUNC_DEF("getServername", 0, qb_tls_conn_get_servername),
    JS_CFUNC_DEF("setALPNProtocols", 1, qb_tls_conn_set_alpn),
    JS_CFUNC_DEF("setALPNCallback", 1, qb_tls_conn_set_alpn_callback),
    JS_CFUNC_DEF("getALPNNegotiatedProtocol", 0, qb_tls_conn_get_alpn),
    JS_CFUNC_MAGIC_DEF("enableKeylogCallback", 0, qb_tls_conn_enable, 0),
    JS_CFUNC_MAGIC_DEF("enableSessionCallbacks", 0, qb_tls_conn_enable, 1),
    JS_CFUNC_MAGIC_DEF("enableCertCb", 0, qb_tls_conn_enable, 2),
    JS_CFUNC_DEF("certCbDone", 0, qb_tls_conn_cert_cb_done),
    JS_CFUNC_DEF("setSNIContext", 1, qb_tls_conn_set_sni_context),
    JS_CFUNC_DEF("takeEvents", 0, qb_tls_conn_take_events),
    JS_CFUNC_DEF("verifyError", 0, qb_tls_conn_verify_error),
    JS_CFUNC_DEF("hasPeerCertificate", 0, qb_tls_conn_has_peer_certificate),
    JS_CFUNC_DEF("getPeerCertificate", 1, qb_tls_conn_get_peer_certificate),
    JS_CFUNC_DEF("getCertificate", 0, qb_tls_conn_get_certificate),
    JS_CFUNC_DEF("getProtocol", 0, qb_tls_conn_get_protocol),
    JS_CFUNC_DEF("getCipher", 0, qb_tls_conn_get_cipher),
    JS_CFUNC_DEF("getSharedSigalgs", 0, qb_tls_conn_get_shared_sigalgs),
    JS_CFUNC_DEF("getEphemeralKeyInfo", 0, qb_tls_conn_get_ephemeral_key_info),
    JS_CFUNC_MAGIC_DEF("getFinished", 0, qb_tls_conn_get_finished, 0),
    JS_CFUNC_MAGIC_DEF("getPeerFinished", 0, qb_tls_conn_get_finished, 1),
    JS_CFUNC_DEF("getSession", 0, qb_tls_conn_get_session),
    JS_CFUNC_DEF("setSession", 1, qb_tls_conn_set_session),
    JS_CFUNC_DEF("isSessionReused", 0, qb_tls_conn_is_session_reused),
    JS_CFUNC_DEF("getTLSTicket", 0, qb_tls_conn_get_tls_ticket),
    JS_CFUNC_DEF("exportKeyingMaterial", 3, qb_tls_conn_export_keying_material),
    JS_CFUNC_DEF("renegotiate", 0, qb_tls_conn_renegotiate),
    JS_CFUNC_DEF("setMaxSendFragment", 1, qb_tls_conn_set_max_send_fragment),
    JS_CFUNC_DEF("enableTrace", 0, qb_tls_conn_enable_trace),
};

/* ----------------------------------------------------------------- module */

/* getSSLCiphers(): the names of the ciphers OpenSSL enables by default */
static JSValue qb_tls_get_ssl_ciphers(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv)
{
    SSL_CTX *sc = SSL_CTX_new(TLS_method());
    SSL *s;
    STACK_OF(SSL_CIPHER) * ciphers;
    JSValue arr;
    int i;

    if (!sc)
        return qb_tls_throw(ctx, "SSL_CTX_new failed");
    s = SSL_new(sc);
    if (!s) {
        SSL_CTX_free(sc);
        return qb_tls_throw(ctx, "SSL_new failed");
    }
    arr = JS_NewArray(ctx);
    ciphers = SSL_get_ciphers(s);
    for (i = 0; !JS_IsException(arr) && ciphers && i < sk_SSL_CIPHER_num(ciphers); i++) {
        const char *name = SSL_CIPHER_get_name(sk_SSL_CIPHER_value(ciphers, i));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, name ? name : ""));
    }
    SSL_free(s);
    SSL_CTX_free(sc);
    return arr;
}

/* the paths of the default trust: [file, dir, fileEnv, dirEnv] */
static JSValue qb_tls_default_paths(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, X509_get_default_cert_file()));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(ctx, X509_get_default_cert_dir()));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewString(ctx, X509_get_default_cert_file_env()));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewString(ctx, X509_get_default_cert_dir_env()));
    return arr;
}

static const JSCFunctionListEntry qb_tls_funcs[] = {
    JS_CFUNC_DEF("createSecureContext", 2, qb_tls_create_ctx),
    JS_CFUNC_DEF("createConnection", 2, qb_tls_create_conn),
    JS_CFUNC_DEF("createCertStore", 1, qb_tls_create_store),
    JS_CFUNC_DEF("getSSLCiphers", 0, qb_tls_get_ssl_ciphers),
    JS_CFUNC_DEF("defaultPaths", 0, qb_tls_default_paths),
    JS_PROP_STRING_DEF("OPENSSL_VERSION", OPENSSL_VERSION_TEXT, 0),
    JS_PROP_INT32_DEF("TLS1_VERSION", TLS1_VERSION, 0),
    JS_PROP_INT32_DEF("TLS1_1_VERSION", TLS1_1_VERSION, 0),
    JS_PROP_INT32_DEF("TLS1_2_VERSION", TLS1_2_VERSION, 0),
    JS_PROP_INT32_DEF("TLS1_3_VERSION", TLS1_3_VERSION, 0),
    JS_PROP_INT64_DEF("SSL_OP_CIPHER_SERVER_PREFERENCE", (int64_t)SSL_OP_CIPHER_SERVER_PREFERENCE,
                      0),
    JS_PROP_INT64_DEF("SSL_OP_NO_TICKET", (int64_t)SSL_OP_NO_TICKET, 0),
    JS_PROP_INT32_DEF("HANDSHAKE_DONE", QB_TLS_HANDSHAKE_DONE, 0),
    JS_PROP_INT32_DEF("CLEAR", QB_TLS_CLEAR, 0),
    JS_PROP_INT32_DEF("ENC", QB_TLS_ENC, 0),
    JS_PROP_INT32_DEF("EOF", QB_TLS_EOF, 0),
    JS_PROP_INT32_DEF("EVENTS", QB_TLS_EVENTS, 0),
    JS_PROP_INT32_DEF("CERTCB", QB_TLS_CERTCB, 0),
    JS_PROP_INT32_DEF("SECURE", QB_TLS_SECURE, 0),
};

static int qb_tls_register_class(JSContext *ctx, JSClassID *pid, const JSClassDef *def,
                                 const JSCFunctionListEntry *funcs, int nfuncs)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;

    JS_NewClassID(pid);
    if (JS_IsRegisteredClass(rt, *pid))
        return 0;
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, funcs, nfuncs) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, *pid, proto);
    return 0;
}

static int qb_tls_module_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, qb_tls_funcs, countof(qb_tls_funcs));
}

JSModuleDef *qb_init_tls(JSContext *ctx, const char *name)
{
    JSModuleDef *m;

    if (qb_tls_register_class(ctx, &qb_tls_ctx_class_id, &qb_tls_ctx_class, qb_tls_ctx_proto_funcs,
                              countof(qb_tls_ctx_proto_funcs)) < 0 ||
        qb_tls_register_class(ctx, &qb_tls_conn_class_id, &qb_tls_conn_class,
                              qb_tls_conn_proto_funcs, countof(qb_tls_conn_proto_funcs)) < 0 ||
        qb_tls_register_class(ctx, &qb_tls_store_class_id, &qb_tls_store_class,
                              qb_tls_store_proto_funcs, countof(qb_tls_store_proto_funcs)) < 0)
        return NULL;
    m = JS_NewCModule(ctx, name, qb_tls_module_init);
    if (m && JS_AddModuleExportList(ctx, m, qb_tls_funcs, countof(qb_tls_funcs)) < 0)
        return NULL;
    return m;
}
