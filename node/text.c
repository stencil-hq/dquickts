/* Text codecs and Buffer fast paths. */
#include "qbun.h"
#include <string.h>
#include <limits.h>

static int put_utf8(uint8_t *p, uint32_t c)
{
    if (c < 128) {
        p[0] = c;
        return 1;
    }
    if (c < 2048) {
        p[0] = 192 | (c >> 6);
        p[1] = 128 | (c & 63);
        return 2;
    }
    if (c < 65536) {
        p[0] = 224 | (c >> 12);
        p[1] = 128 | ((c >> 6) & 63);
        p[2] = 128 | (c & 63);
        return 3;
    }
    p[0] = 240 | (c >> 18);
    p[1] = 128 | ((c >> 12) & 63);
    p[2] = 128 | ((c >> 6) & 63);
    p[3] = 128 | (c & 63);
    return 4;
}
static size_t encode_utf8(const uint16_t *s, size_t n, uint8_t *out, size_t cap, size_t *read)
{
    size_t i = 0, w = 0;
    while (i < n) {
        uint32_t c = s[i];
        size_t units = 1;
        uint8_t tmp[4];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            units = 2;
        } else if (c >= 0xd800 && c <= 0xdfff)
            c = 0xfffd;
        int k = put_utf8(tmp, c);
        if ((size_t)k > cap - w)
            break;
        if (out)
            memcpy(out + w, tmp, k);
        w += k;
        i += units;
    }
    if (read)
        *read = i;
    return w;
}
/* WHATWG decoder: invalid continuation bytes are reprocessed. */
static JSValue decode_utf8(JSContext *ctx, const uint8_t *p, size_t n, int fatal, int validate)
{
    uint16_t *out = validate ? NULL : js_malloc(ctx, (n + 1) * sizeof(uint16_t));
    if (!validate && !out)
        return JS_EXCEPTION;
    size_t i = 0, w = 0;
    int bad = 0;
    while (i < n) {
        uint32_t c = p[i++];
        int need = 0;
        uint8_t lo = 128, hi = 191;
        if (c < 128) {
            if (out)
                out[w] = c;
            w++;
            continue;
        }
        if (c >= 194 && c <= 223) {
            need = 1;
            c &= 31;
        } else if (c >= 224 && c <= 239) {
            if (c == 224)
                lo = 160;
            if (c == 237)
                hi = 159;
            need = 2;
            c &= 15;
        } else if (c >= 240 && c <= 244) {
            if (c == 240)
                lo = 144;
            if (c == 244)
                hi = 143;
            need = 3;
            c &= 7;
        } else {
            bad = 1;
            if (out)
                out[w] = 0xfffd;
            w++;
            continue;
        }
        int ok = 1;
        for (int j = 0; j < need; j++) {
            if (i == n || p[i] < lo || p[i] > hi) {
                ok = 0;
                break;
            }
            c = (c << 6) | (p[i++] & 63);
            lo = 128;
            hi = 191;
        }
        if (!ok) {
            bad = 1;
            if (out)
                out[w] = 0xfffd;
            w++;
        } else if (c > 65535) {
            c -= 65536;
            if (out) {
                out[w] = 0xd800 + (c >> 10);
                out[w + 1] = 0xdc00 + (c & 1023);
            }
            w += 2;
        } else {
            if (out)
                out[w] = c;
            w++;
        }
    }
    if (validate)
        return JS_NewBool(ctx, !bad);
    if (bad && fatal) {
        js_free(ctx, out);
        return qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_ENCODING_INVALID_ENCODED_DATA",
                             "The encoded data was not valid for encoding utf-8");
    }
    JSValue r = JS_NewStringUTF16(ctx, out, w);
    js_free(ctx, out);
    return r;
}
static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}
static JSValue text_encode(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    size_t n;
    const uint16_t *s = JS_ToCStringLenUTF16(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    const char *enc = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (argc > 1 && !enc) {
        JS_FreeCStringUTF16(ctx, s);
        return JS_EXCEPTION;
    }
    size_t cap = n > SIZE_MAX / 3 ? 0 : n * 3;
    uint8_t *p = js_malloc(ctx, cap ? cap : 1);
    if (!p) {
        JS_FreeCStringUTF16(ctx, s);
        if (enc)
            JS_FreeCString(ctx, enc);
        return JS_EXCEPTION;
    }
    size_t w = 0;
    if (!enc || !strcmp(enc, "utf8"))
        w = encode_utf8(s, n, p, cap, NULL);
    else if (!strcmp(enc, "utf16le")) {
        for (size_t i = 0; i < n; i++) {
            p[w++] = s[i] & 255;
            p[w++] = s[i] >> 8;
        }
    } else if (!strcmp(enc, "hex")) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            int a = hexval(s[i]), b = hexval(s[i + 1]);
            if (a < 0 || b < 0)
                break;
            p[w++] = (a << 4) | b;
        }
    } else if (!strcmp(enc, "base64") || !strcmp(enc, "base64url")) {
        unsigned acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '=')
                break;
            int v = b64val(s[i]);
            if (v < 0)
                continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                p[w++] = (acc >> bits) & 255;
            }
        }
    } else {
        for (size_t i = 0; i < n; i++)
            p[w++] = s[i] & 255;
    }
    JS_FreeCStringUTF16(ctx, s);
    if (enc)
        JS_FreeCString(ctx, enc);
    return qb_new_buffer_owned(ctx, p, w);
}
static JSValue text_into(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    size_t n, cap, read;
    const uint16_t *s = JS_ToCStringLenUTF16(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    uint8_t *p = qb_get_bytes(ctx, argv[1], &cap);
    if (!p) {
        JS_FreeCStringUTF16(ctx, s);
        return JS_EXCEPTION;
    }
    size_t w = encode_utf8(s, n, p, cap, &read);
    JS_FreeCStringUTF16(ctx, s);
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt64(ctx, read));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt64(ctx, w));
    return r;
}
static JSValue text_length(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    size_t n;
    const uint16_t *s = JS_ToCStringLenUTF16(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    size_t w = encode_utf8(s, n, NULL, SIZE_MAX, NULL);
    JS_FreeCStringUTF16(ctx, s);
    return JS_NewInt64(ctx, w);
}
static JSValue text_decode(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    size_t n;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    const char *enc = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (argc > 1 && !enc)
        return JS_EXCEPTION;
    int64_t a = 0, b = n;
    if (argc > 2 && JS_ToInt64(ctx, &a, argv[2]) < 0)
        goto fail;
    if (argc > 3 && JS_ToInt64(ctx, &b, argv[3]) < 0)
        goto fail;
    if (a < 0)
        a = 0;
    if ((uint64_t)a > n)
        a = n;
    if (b < a)
        b = a;
    if ((uint64_t)b > n)
        b = n;
    p += a;
    n = b - a;
    JSValue r;
    if (!enc || !strcmp(enc, "utf8"))
        r = decode_utf8(ctx, p, n, 0, 0);
    else if (!strcmp(enc, "latin1"))
        r = JS_NewStringLatin1Len(ctx, p, n);
    else if (!strcmp(enc, "utf16le") || !strcmp(enc, "ascii")) {
        size_t count = !strcmp(enc, "ascii") ? n : n / 2;
        uint16_t *s = js_malloc(ctx, (count + 1) * 2);
        if (!s)
            goto fail;
        for (size_t i = 0; i < count; i++)
            s[i] = !strcmp(enc, "ascii") ? (p[i] & 127) : (p[i * 2] | (p[i * 2 + 1] << 8));
        r = JS_NewStringUTF16(ctx, s, count);
        js_free(ctx, s);
    } else {
        int hex = !strcmp(enc, "hex"), url = !strcmp(enc, "base64url");
        size_t cap = hex ? n * 2 : ((n + 2) / 3) * 4;
        char *s = js_malloc(ctx, cap + 1);
        if (!s)
            goto fail;
        size_t w = 0;
        const char *table =
            url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        if (hex) {
            for (size_t i = 0; i < n; i++) {
                s[w++] = "0123456789abcdef"[p[i] >> 4];
                s[w++] = "0123456789abcdef"[p[i] & 15];
            }
        } else
            for (size_t i = 0; i < n; i += 3) {
                uint32_t v = (uint32_t)p[i] << 16;
                if (i + 1 < n)
                    v |= (uint32_t)p[i + 1] << 8;
                if (i + 2 < n)
                    v |= p[i + 2];
                s[w++] = table[v >> 18];
                s[w++] = table[(v >> 12) & 63];
                if (i + 1 < n)
                    s[w++] = table[(v >> 6) & 63];
                else if (!url)
                    s[w++] = '=';
                if (i + 2 < n)
                    s[w++] = table[v & 63];
                else if (!url)
                    s[w++] = '=';
            }
        r = JS_NewStringLen(ctx, s, w);
        js_free(ctx, s);
    }
    if (enc)
        JS_FreeCString(ctx, enc);
    return r;
fail:
    if (enc)
        JS_FreeCString(ctx, enc);
    return JS_EXCEPTION;
}
static JSValue text_utf8decode(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    size_t n;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    return decode_utf8(ctx, p, n, argc > 1 ? JS_ToBool(ctx, argv[1]) : 0, 0);
}
static JSValue text_check(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                          int magic)
{
    (void)self;
    (void)argc;
    size_t n;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    if (magic)
        return decode_utf8(ctx, p, n, 0, 1);
    for (size_t i = 0; i < n; i++)
        if (p[i] & 128)
            return JS_FALSE;
    return JS_TRUE;
}
static JSValue text_compare(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    size_t a, b;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &a);
    if (!p)
        return JS_EXCEPTION;
    uint8_t *q = qb_get_bytes(ctx, argv[1], &b);
    if (!q)
        return JS_EXCEPTION;
    int r = memcmp(p, q, a < b ? a : b);
    return JS_NewInt32(ctx, r ? (r < 0 ? -1 : 1) : (a < b ? -1 : a > b ? 1 : 0));
}
static JSValue text_index(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    size_t n, m;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    uint8_t *q = qb_get_bytes(ctx, argv[1], &m);
    if (!q)
        return JS_EXCEPTION;
    int64_t off;
    if (JS_ToInt64(ctx, &off, argv[2]) < 0)
        return JS_EXCEPTION;
    int back = JS_ToBool(ctx, argv[3]);
    if (off < 0)
        off = 0;
    if ((uint64_t)off > n)
        off = n;
    if (!m)
        return JS_NewInt64(ctx, off);
    if (m > n)
        return JS_NewInt32(ctx, -1);
    if (back) {
        size_t i = (uint64_t)off > n - m ? n - m : (size_t)off;
        do {
            if (!memcmp(p + i, q, m))
                return JS_NewInt64(ctx, i);
        } while (i--);
    } else
        for (size_t i = off; i <= n - m; i++)
            if (!memcmp(p + i, q, m))
                return JS_NewInt64(ctx, i);
    return JS_NewInt32(ctx, -1);
}
static JSValue text_fill(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    size_t n, m;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    uint8_t *q = qb_get_bytes(ctx, argv[1], &m);
    if (!q)
        return JS_EXCEPTION;
    int64_t a, b;
    if (JS_ToInt64(ctx, &a, argv[2]) < 0 || JS_ToInt64(ctx, &b, argv[3]) < 0)
        return JS_EXCEPTION;
    if (a < 0 || b < a || (uint64_t)b > n)
        return qb_throw_code(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE",
                             "Fill range is outside buffer bounds");
    if (!m)
        return qb_throw_code(
            ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE",
            "The argument 'value' is invalid"); /* Only overlapping patterns require a snapshot. */
    uint8_t *copy = NULL;
    if ((uintptr_t)q < (uintptr_t)(p + b) && (uintptr_t)(q + m) > (uintptr_t)(p + a)) {
        copy = js_malloc(ctx, m);
        if (!copy)
            return JS_EXCEPTION;
        memcpy(copy, q, m);
        q = copy;
    }
    if (m == 1)
        memset(p + a, q[0], b - a);
    else
        for (size_t i = a; i < (size_t)b; i++)
            p[i] = q[(i - a) % m];
    js_free(ctx, copy);
    return JS_DupValue(ctx, argv[0]);
}
static JSValue text_swap(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int width)
{
    (void)self;
    (void)argc;
    size_t n;
    uint8_t *p = qb_get_bytes(ctx, argv[0], &n);
    if (!p)
        return JS_EXCEPTION;
    if (n % width)
        return qb_throw_code(ctx, JS_RANGE_ERROR, "ERR_INVALID_BUFFER_SIZE",
                             "Buffer size must be a multiple of %d-bits", width * 8);
    for (size_t i = 0; i < n; i += width)
        for (int j = 0; j < width / 2; j++) {
            uint8_t t = p[i + j];
            p[i + j] = p[i + width - j - 1];
            p[i + width - j - 1] = t;
        }
    return JS_DupValue(ctx, argv[0]);
}
static const JSCFunctionListEntry qb_text_funcs[] = {
    JS_CFUNC_DEF("utf8Encode", 1, text_encode),
    JS_CFUNC_DEF("utf8EncodeInto", 2, text_into),
    JS_CFUNC_DEF("utf8Decode", 2, text_utf8decode),
    JS_CFUNC_DEF("byteLengthUtf8", 1, text_length),
    JS_CFUNC_DEF("encode", 2, text_encode),
    JS_CFUNC_DEF("decode", 4, text_decode),
    JS_CFUNC_MAGIC_DEF("isUtf8", 1, text_check, 1),
    JS_CFUNC_MAGIC_DEF("isAscii", 1, text_check, 0),
    JS_CFUNC_DEF("compare", 2, text_compare),
    JS_CFUNC_DEF("indexOf", 4, text_index),
    JS_CFUNC_DEF("fill", 4, text_fill),
    JS_CFUNC_MAGIC_DEF("swap16", 1, text_swap, 2),
    JS_CFUNC_MAGIC_DEF("swap32", 1, text_swap, 4),
    JS_CFUNC_MAGIC_DEF("swap64", 1, text_swap, 8)};
QB_SIMPLE_MODULE(text, qb_text_funcs)
