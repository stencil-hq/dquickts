/*
 * qbun:zlib: per-runtime compression handles. Worker threads never touch JS.
 *
 * createEngine(constants.<mode>, options) creates an owned native stream.
 * processSync(bytes|null, flush) and process(bytes|null, flush, callback)
 * return { output: Buffer, consumed, done, pending }. An async call snapshots
 * at most chunkSize input bytes; callers must resubmit the unconsumed suffix.
 * Drain pending output even when consumed equals the input length. A completed
 * member may leave input for a subsequent gzip/zstd member; stop on a completed
 * step with no progress. close() safely defers disposal during queued work.
 *
 * One-shot helpers accept bytes plus the same options and return a Buffer.
 * Node/Bun policy (input strings, option validation, callbacks and streams)
 * belongs in their respective TypeScript modules.
 */
#include "qbun.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <zlib.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#include <zstd.h>
#include <zstd_errors.h>
#include <math.h>

enum {
    QB_DEFLATE = 1,
    QB_INFLATE,
    QB_GZIP,
    QB_GUNZIP,
    QB_DEFLATERAW,
    QB_INFLATERAW,
    QB_UNZIP,
    QB_BROTLI_DECODE,
    QB_BROTLI_ENCODE,
    QB_ZSTD_COMPRESS,
    QB_ZSTD_DECOMPRESS
};
typedef struct {
    int key, value;
} QBZParam;
typedef struct QBZlib {
    int mode, initialized, closed, busy, close_pending, done, boundary;
    int level, window_bits, mem_level, strategy, reject_garbage;
    size_t chunk_size, max_output, total_output, dictionary_len;
    uint8_t *dictionary;
    QBZParam *params;
    size_t nparams;
    unsigned char header[2];
    size_t header_len;
    uint64_t pledged_size;
    int has_pledged_size;
    z_stream z;
    BrotliEncoderState *be;
    BrotliDecoderState *bd;
    BrotliEncoderPreparedDictionary *b_dictionary;
    ZSTD_CStream *zc;
    ZSTD_DStream *zd;
    int error_number;
    char error_code[96], error_message[256];
} QBZlib;
typedef struct {
    size_t consumed, produced;
    int done, pending;
} QBZResult;
static JSClassID qb_zlib_class_id;
static uv_once_t qb_zlib_once = UV_ONCE_INIT;
static void qb_zlib_class_once(void)
{
    JS_NewClassID(&qb_zlib_class_id);
}

static int qb_zfail(QBZlib *e, int number, const char *code, const char *message)
{
    e->error_number = number;
    snprintf(e->error_code, sizeof(e->error_code), "%s", code);
    snprintf(e->error_message, sizeof(e->error_message), "%s", message);
    return -1;
}
static const char *qb_zcode(int status)
{
    switch (status) {
    case Z_OK:
        return "Z_OK";
    case Z_STREAM_END:
        return "Z_STREAM_END";
    case Z_NEED_DICT:
        return "Z_NEED_DICT";
    case Z_ERRNO:
        return "Z_ERRNO";
    case Z_STREAM_ERROR:
        return "Z_STREAM_ERROR";
    case Z_DATA_ERROR:
        return "Z_DATA_ERROR";
    case Z_MEM_ERROR:
        return "Z_MEM_ERROR";
    case Z_BUF_ERROR:
        return "Z_BUF_ERROR";
    case Z_VERSION_ERROR:
        return "Z_VERSION_ERROR";
    default:
        return "Z_STREAM_ERROR";
    }
}
static int qb_zstatus(QBZlib *e, int status)
{
    return qb_zfail(e, status, qb_zcode(status), e->z.msg ? e->z.msg : zError(status));
}
static int qb_zstd_status(QBZlib *e, size_t status)
{
    const char *code = "ZSTD_error_GENERIC";
    ZSTD_ErrorCode err = ZSTD_getErrorCode(status);
#define ZE(name)                                                                                   \
    case ZSTD_error_##name:                                                                        \
        code = "ZSTD_error_" #name;                                                                \
        break
    switch (err) {
        ZE(GENERIC);
        ZE(prefix_unknown);
        ZE(version_unsupported);
        ZE(frameParameter_unsupported);
        ZE(frameParameter_windowTooLarge);
        ZE(corruption_detected);
        ZE(checksum_wrong);
        ZE(literals_headerWrong);
        ZE(dictionary_corrupted);
        ZE(dictionary_wrong);
        ZE(dictionaryCreation_failed);
        ZE(parameter_unsupported);
        ZE(parameter_combination_unsupported);
        ZE(parameter_outOfBound);
        ZE(tableLog_tooLarge);
        ZE(maxSymbolValue_tooLarge);
        ZE(maxSymbolValue_tooSmall);
        ZE(stage_wrong);
        ZE(init_missing);
        ZE(memory_allocation);
        ZE(workSpace_tooSmall);
        ZE(dstSize_tooSmall);
        ZE(srcSize_wrong);
        ZE(dstBuffer_null);
        ZE(noForwardProgress_destFull);
        ZE(noForwardProgress_inputEmpty);
    default:
        break;
    }
#undef ZE
    return qb_zfail(e, -(int)err, code, ZSTD_getErrorName(status));
}
static JSValue qb_zerror(JSContext *ctx, QBZlib *e)
{
    JSValue error = JS_NewErrorLen(
        ctx, !strcmp(e->error_code, "ERR_BUFFER_TOO_LARGE") ? JS_RANGE_ERROR : JS_PLAIN_ERROR,
        e->error_message, strlen(e->error_message));
    qb_set(ctx, error, "code", JS_NewString(ctx, e->error_code));
    qb_set(ctx, error, "errno", JS_NewInt32(ctx, e->error_number));
    return error;
}
static void qb_zdispose(QBZlib *e)
{
    if (e->initialized && e->mode <= QB_UNZIP) {
        if (e->mode == QB_DEFLATE || e->mode == QB_GZIP || e->mode == QB_DEFLATERAW)
            deflateEnd(&e->z);
        else
            inflateEnd(&e->z);
    }
    if (e->be)
        BrotliEncoderDestroyInstance(e->be);
    if (e->bd)
        BrotliDecoderDestroyInstance(e->bd);
    if (e->b_dictionary)
        BrotliEncoderDestroyPreparedDictionary(e->b_dictionary);
    if (e->zc)
        ZSTD_freeCStream(e->zc);
    if (e->zd)
        ZSTD_freeDStream(e->zd);
    e->be = NULL;
    e->bd = NULL;
    e->b_dictionary = NULL;
    e->zc = NULL;
    e->zd = NULL;
    e->initialized = 0;
}
static void qb_zfree(QBZlib *e)
{
    qb_zdispose(e);
    free(e->dictionary);
    free(e->params);
    free(e);
}
static void qb_zfinalizer(JSRuntime *rt, JSValue value)
{
    QBZlib *e = JS_GetOpaque(value, qb_zlib_class_id);
    if (e)
        qb_zfree(e);
}
static JSClassDef qb_zclass = {"CompressionEngine", .finalizer = qb_zfinalizer};

static int qb_zinit(QBZlib *e)
{
    size_t i, status;
    int r, bits = e->window_bits;
    e->done = e->boundary = 0;
    e->header_len = 0;
    e->total_output = 0;
    e->error_code[0] = 0;
    if (e->mode <= QB_UNZIP) {
        memset(&e->z, 0, sizeof(e->z));
        if (e->mode == QB_GZIP || e->mode == QB_GUNZIP)
            bits += 16;
        if (e->mode == QB_UNZIP)
            bits += 32;
        if (e->mode == QB_DEFLATERAW || e->mode == QB_INFLATERAW)
            bits = -bits;
        if (e->mode == QB_DEFLATE || e->mode == QB_GZIP || e->mode == QB_DEFLATERAW)
            r = deflateInit2(&e->z, e->level, Z_DEFLATED, bits, e->mem_level, e->strategy);
        else
            r = inflateInit2(&e->z, bits);
        if (r != Z_OK)
            return qb_zstatus(e, r);
        e->initialized = 1;
        if (e->dictionary_len && (e->mode == QB_DEFLATE || e->mode == QB_DEFLATERAW))
            r = deflateSetDictionary(&e->z, e->dictionary, (uInt)e->dictionary_len);
        else if (e->dictionary_len && e->mode == QB_INFLATERAW)
            r = inflateSetDictionary(&e->z, e->dictionary, (uInt)e->dictionary_len);
        if (r != Z_OK)
            return qb_zstatus(e, r);
    } else if (e->mode == QB_BROTLI_ENCODE || e->mode == QB_BROTLI_DECODE) {
        if (e->mode == QB_BROTLI_ENCODE)
            e->be = BrotliEncoderCreateInstance(NULL, NULL, NULL);
        else
            e->bd = BrotliDecoderCreateInstance(NULL, NULL, NULL);
        if (!e->be && !e->bd)
            return qb_zfail(e, Z_MEM_ERROR, "Z_MEM_ERROR", "Out of memory");
        for (i = 0; i < e->nparams; i++) {
            r = e->be ? BrotliEncoderSetParameter(e->be, (BrotliEncoderParameter)e->params[i].key,
                                                  (uint32_t)e->params[i].value)
                      : BrotliDecoderSetParameter(e->bd, (BrotliDecoderParameter)e->params[i].key,
                                                  (uint32_t)e->params[i].value);
            if (!r)
                return qb_zfail(e, Z_STREAM_ERROR, "ERR_BROTLI_PARAM_SET_FAILED",
                                "Setting Brotli parameter failed");
        }
        if (e->dictionary_len) {
            if (e->be) {
                e->b_dictionary = BrotliEncoderPrepareDictionary(
                    BROTLI_SHARED_DICTIONARY_RAW, e->dictionary_len, e->dictionary,
                    BROTLI_MAX_QUALITY, NULL, NULL, NULL);
                r = e->b_dictionary &&
                    BrotliEncoderAttachPreparedDictionary(e->be, e->b_dictionary);
            } else
                r = BrotliDecoderAttachDictionary(e->bd, BROTLI_SHARED_DICTIONARY_RAW,
                                                  e->dictionary_len, e->dictionary);
            if (!r)
                return qb_zfail(e, Z_STREAM_ERROR, "ERR_ZLIB_INITIALIZATION_FAILED",
                                "Brotli dictionary initialization failed");
        }
        e->initialized = 1;
    } else {
        if (e->mode == QB_ZSTD_COMPRESS)
            e->zc = ZSTD_createCStream();
        else
            e->zd = ZSTD_createDStream();
        if (!e->zc && !e->zd)
            return qb_zfail(e, Z_MEM_ERROR, "Z_MEM_ERROR", "Out of memory");
        for (i = 0; i < e->nparams; i++) {
            status = e->zc ? ZSTD_CCtx_setParameter(e->zc, (ZSTD_cParameter)e->params[i].key,
                                                    e->params[i].value)
                           : ZSTD_DCtx_setParameter(e->zd, (ZSTD_dParameter)e->params[i].key,
                                                    e->params[i].value);
            if (ZSTD_isError(status))
                return qb_zstd_status(e, status);
        }
        if (e->zc && e->has_pledged_size) {
            status = ZSTD_CCtx_setPledgedSrcSize(e->zc, e->pledged_size);
            if (ZSTD_isError(status))
                return qb_zstd_status(e, status);
        }
        if (e->dictionary_len) {
            status = e->zc ? ZSTD_CCtx_loadDictionary(e->zc, e->dictionary, e->dictionary_len)
                           : ZSTD_DCtx_loadDictionary(e->zd, e->dictionary, e->dictionary_len);
            if (ZSTD_isError(status))
                return qb_zstd_status(e, status);
        }
        e->initialized = 1;
    }
    return 0;
}
static int qb_zoption(JSContext *ctx, JSValueConst opts, const char *key, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    int r = JS_IsException(v) ? -1 : JS_IsUndefined(v) ? 0 : JS_ToInt32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r;
}
static int qb_zsize(JSContext *ctx, JSValueConst opts, const char *key, size_t *out)
{
    uint64_t size;
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    int r = 0;
    if (JS_IsException(v))
        r = -1;
    else if (!JS_IsUndefined(v)) {
        r = JS_ToIndex(ctx, &size, v);
        if (!r) {
            if (size > SIZE_MAX) {
                JS_ThrowRangeError(ctx, "%s is too large", key);
                r = -1;
            } else
                *out = (size_t)size;
        }
    }
    JS_FreeValue(ctx, v);
    return r;
}
static QBZlib *qb_znew(JSContext *ctx, int mode, JSValueConst opts)
{
    QBZlib *e = calloc(1, sizeof(*e));
    JSValue v, params = JS_UNDEFINED;
    JSPropertyEnum *keys = NULL;
    uint32_t count = 0, i;
    if (!e) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    e->mode = mode;
    e->level = -1;
    e->window_bits = 15;
    e->mem_level = 8;
    e->strategy = 0;
    e->chunk_size = 16384;
    e->max_output = (size_t)9007199254740991ULL;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts)) {
        if (!JS_IsObject(opts)) {
            JS_ThrowTypeError(ctx, "options must be an object");
            goto fail;
        }
        if (qb_zoption(ctx, opts, "level", &e->level) ||
            qb_zoption(ctx, opts, "windowBits", &e->window_bits) ||
            qb_zoption(ctx, opts, "memLevel", &e->mem_level) ||
            qb_zoption(ctx, opts, "strategy", &e->strategy) ||
            qb_zsize(ctx, opts, "chunkSize", &e->chunk_size) ||
            qb_zsize(ctx, opts, "maxOutputLength", &e->max_output))
            goto fail;
        v = JS_GetPropertyStr(ctx, opts, "rejectGarbageAfterEnd");
        if (JS_IsException(v))
            goto fail;
        e->reject_garbage = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "pledgedSrcSize");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            e->has_pledged_size = 1;
            if (JS_ToIndex(ctx, &e->pledged_size, v)) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "dictionary");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            size_t len;
            uint8_t *data = qb_get_bytes(ctx, v, &len);
            if (!data) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            if (len > UINT_MAX && mode <= QB_UNZIP) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "dictionary is too large");
                goto fail;
            }
            e->dictionary = malloc(len ? len : 1);
            if (!e->dictionary) {
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            memcpy(e->dictionary, data, len);
            e->dictionary_len = len;
        }
        JS_FreeValue(ctx, v);
        params = JS_GetPropertyStr(ctx, opts, "params");
        if (JS_IsException(params))
            goto fail;
        if (!JS_IsUndefined(params)) {
            if (!JS_IsObject(params)) {
                JS_ThrowTypeError(ctx, "params must be an object");
                goto fail;
            }
            if (JS_GetOwnPropertyNames(ctx, &keys, &count, params,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
                goto fail;
            e->params = calloc(count ? count : 1, sizeof(*e->params));
            if (!e->params) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            for (i = 0; i < count; i++) {
                const char *key = JS_AtomToCString(ctx, keys[i].atom);
                char *end;
                long id;
                if (!key)
                    goto fail;
                id = strtol(key, &end, 10);
                if (!*key || *end || id < 0 || id > INT_MAX) {
                    JS_FreeCString(ctx, key);
                    qb_throw_code(ctx, JS_TYPE_ERROR,
                                  mode <= QB_BROTLI_ENCODE ? "ERR_BROTLI_INVALID_PARAM"
                                                           : "ERR_ZSTD_INVALID_PARAM",
                                  "Invalid compression parameter");
                    goto fail;
                }
                JS_FreeCString(ctx, key);
                v = JS_GetProperty(ctx, params, keys[i].atom);
                if (JS_IsException(v))
                    goto fail;
                if (JS_IsUndefined(v) || (JS_IsNumber(v) && JS_VALUE_GET_TAG(v) == JS_TAG_INT &&
                                          JS_VALUE_GET_INT(v) == -1 && JS_IsArray(ctx, params))) {
                    JS_FreeValue(ctx, v);
                    continue;
                }
                e->params[e->nparams].key = (int)id;
                if (JS_ToInt32(ctx, &e->params[e->nparams].value, v)) {
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                JS_FreeValue(ctx, v);
                e->nparams++;
            }
        }
    }
    for (i = 0; i < count; i++)
        JS_FreeAtom(ctx, keys[i].atom);
    js_free(ctx, keys);
    JS_FreeValue(ctx, params);
    if (mode < QB_DEFLATE || mode > QB_ZSTD_DECOMPRESS || e->chunk_size < 64 ||
        e->chunk_size > UINT_MAX || !e->max_output) {
        qb_throw_code(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE",
                      "Invalid compression mode or buffer size");
        qb_zfree(e);
        return NULL;
    }
    if (e->window_bits == 8 && (mode == QB_DEFLATERAW || mode == QB_DEFLATE))
        e->window_bits = 9;
    if (qb_zinit(e)) {
        JS_Throw(ctx, qb_zerror(ctx, e));
        qb_zfree(e);
        return NULL;
    }
    return e;
fail:
    for (i = 0; i < count; i++)
        JS_FreeAtom(ctx, keys[i].atom);
    js_free(ctx, keys);
    JS_FreeValue(ctx, params);
    qb_zfree(e);
    return NULL;
}

/* One bounded output block per call permits the Transform to respect backpressure. */
static int qb_zstep(QBZlib *e, const uint8_t *input, size_t len, int flush, uint8_t *output,
                    size_t cap, QBZResult *result)
{
    size_t in_left = len, out_left = cap, status = 0;
    const uint8_t *next_in = input;
    uint8_t *next_out = output;
    int r = 0, finishing = flush == (e->mode <= QB_UNZIP ? Z_FINISH : 2);
    memset(result, 0, sizeof(*result));
    if (e->error_code[0])
        return -1;
    if (flush < 0 || flush > (e->mode <= QB_UNZIP           ? Z_TREES
                              : e->mode <= QB_BROTLI_ENCODE ? BROTLI_OPERATION_EMIT_METADATA
                                                            : ZSTD_e_end))
        return qb_zfail(e, Z_STREAM_ERROR, "Z_STREAM_ERROR", "Invalid flush operation");
    if (e->done) {
        int multi = e->mode == QB_GUNZIP ||
                    (e->mode == QB_UNZIP && e->header_len == 2 && e->header[0] == 31 &&
                     e->header[1] == 139) ||
                    e->mode == QB_ZSTD_DECOMPRESS;
        if (!multi || !len) {
            if (len && e->reject_garbage)
                return qb_zfail(e, Z_DATA_ERROR, "Z_DATA_ERROR",
                                "Trailing junk after the end of compressed stream");
            result->done = 1;
            return 0;
        }
        if (e->mode <= QB_UNZIP) {
            if (!input[0]) {
                while (result->consumed < len && input[result->consumed] == 0)
                    result->consumed++;
                result->done = 1;
                if (result->consumed < len && e->reject_garbage)
                    return qb_zfail(e, Z_DATA_ERROR, "Z_DATA_ERROR",
                                    "Trailing junk after the end of compressed stream");
                return 0;
            }
            r = inflateReset(&e->z);
            if (r != Z_OK)
                return qb_zstatus(e, r);
        }
        e->done = 0;
    }
    if (e->mode <= QB_UNZIP) {
        size_t in_size = len > UINT_MAX ? UINT_MAX : len;
        size_t out_size = cap > UINT_MAX ? UINT_MAX : cap;
        int encoding = e->mode == QB_DEFLATE || e->mode == QB_GZIP || e->mode == QB_DEFLATERAW;
        if (e->mode == QB_UNZIP) {
            size_t h = 0;
            while (e->header_len < 2 && h < len)
                e->header[e->header_len++] = input[h++];
        }
        e->z.next_in = (Bytef *)input;
        e->z.avail_in = (uInt)in_size;
        e->z.next_out = output;
        e->z.avail_out = (uInt)out_size;
        r = encoding ? deflate(&e->z, in_size < len ? Z_NO_FLUSH : flush) : inflate(&e->z, flush);
        if (r == Z_NEED_DICT) {
            if (!e->dictionary_len)
                return qb_zfail(e, Z_NEED_DICT, "Z_NEED_DICT", "Missing dictionary");
            r = inflateSetDictionary(&e->z, e->dictionary, (uInt)e->dictionary_len);
            if (r != Z_OK)
                return qb_zfail(e, Z_NEED_DICT, "Z_NEED_DICT", "Bad dictionary");
            r = inflate(&e->z, flush);
        }
        result->consumed = in_size - e->z.avail_in;
        result->produced = out_size - e->z.avail_out;
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
            return qb_zstatus(e, r);
        e->done = r == Z_STREAM_END;
        result->pending = !e->done && (e->z.avail_out == 0 || in_size < len);
        if (finishing && !e->done && !result->pending && result->consumed == len)
            return qb_zfail(e, Z_BUF_ERROR, "Z_BUF_ERROR", "unexpected end of file");
    } else if (e->be) {
        if (!BrotliEncoderCompressStream(e->be, (BrotliEncoderOperation)flush, &in_left, &next_in,
                                         &out_left, &next_out, NULL))
            return qb_zfail(e, Z_STREAM_ERROR, "ERR_BROTLI_COMPRESSION_FAILED",
                            "Brotli compression failed");
        result->consumed = len - in_left;
        result->produced = cap - out_left;
        e->done = BrotliEncoderIsFinished(e->be);
        result->pending = !e->done && (in_left || BrotliEncoderHasMoreOutput(e->be));
    } else if (e->bd) {
        r = BrotliDecoderDecompressStream(e->bd, &in_left, &next_in, &out_left, &next_out, NULL);
        result->consumed = len - in_left;
        result->produced = cap - out_left;
        if (r == BROTLI_DECODER_RESULT_ERROR) {
            BrotliDecoderErrorCode err = BrotliDecoderGetErrorCode(e->bd);
            const char *code = "ERR_BROTLI_DECOMPRESSION_FAILED";
#define BE(prefix, name, value)                                                                    \
    case BROTLI_DECODER##prefix##name:                                                             \
        code = "BROTLI_DECODER" #prefix #name;                                                     \
        break;
            switch (err) {
                BROTLI_DECODER_ERROR_CODES_LIST(BE, ) default : break;
            }
#undef BE
            return qb_zfail(e, err, code, BrotliDecoderErrorString(err));
        }
        e->done = r == BROTLI_DECODER_RESULT_SUCCESS;
        result->pending = r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
        if (finishing && !e->done && !result->pending && !in_left)
            return qb_zfail(e, Z_BUF_ERROR, "Z_BUF_ERROR", "unexpected end of file");
    } else {
        ZSTD_inBuffer in = {input, len, 0};
        ZSTD_outBuffer out = {output, cap, 0};
        if (e->zc)
            status = ZSTD_compressStream2(e->zc, &out, &in, (ZSTD_EndDirective)flush);
        else
            status = ZSTD_decompressStream(e->zd, &out, &in);
        if (ZSTD_isError(status))
            return qb_zstd_status(e, status);
        result->consumed = in.pos;
        result->produced = out.pos;
        e->done = status == 0 && (e->zd || finishing);
        result->pending =
            !e->done &&
            (in.pos < len || (e->zc && flush != ZSTD_e_continue && status != 0) || out.pos == cap);
        if (e->zd && finishing && !e->done && !result->pending && in.pos == len)
            return qb_zfail(e, Z_BUF_ERROR, "Z_BUF_ERROR", "unexpected end of file");
    }
    if (result->produced > e->max_output - e->total_output)
        return qb_zfail(e, Z_MEM_ERROR, "ERR_BUFFER_TOO_LARGE",
                        "Cannot create a Buffer larger than maxOutputLength");
    e->total_output += result->produced;
    result->done = e->done;
    return 0;
}
static QBZlib *qb_zget(JSContext *ctx, JSValueConst obj)
{
    QBZlib *e = JS_GetOpaque2(ctx, obj, qb_zlib_class_id);
    if (e && e->closed) {
        qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_STREAM_DESTROYED", "Compression binding is closed");
        return NULL;
    }
    if (e && e->busy) {
        qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_INVALID_STATE",
                      "Compression operation is in progress");
        return NULL;
    }
    return e;
}
static JSValue qb_zresult(JSContext *ctx, QBZResult *r, uint8_t *output)
{
    JSValue obj = JS_NewObject(ctx);
    qb_set(ctx, obj, "output", qb_new_buffer_owned(ctx, output, r->produced));
    qb_set(ctx, obj, "consumed", JS_NewInt64(ctx, r->consumed));
    qb_set(ctx, obj, "done", JS_NewBool(ctx, r->done));
    qb_set(ctx, obj, "pending", JS_NewBool(ctx, r->pending));
    return obj;
}
static int qb_zinput(JSContext *ctx, int argc, JSValueConst *argv, const uint8_t **data,
                     size_t *len, int *flush)
{
    *data = (const uint8_t *)"";
    *len = 0;
    *flush = 0;
    if (argc > 1 && JS_ToInt32(ctx, flush, argv[1]))
        return -1;
    if (argc && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        *data = qb_get_bytes(ctx, argv[0], len);
        if (!*data)
            return -1;
    }
    return 0;
}
static JSValue qb_zprocess_sync(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    QBZlib *e = qb_zget(ctx, obj);
    QBZResult result;
    const uint8_t *input;
    size_t len;
    int flush;
    uint8_t *output;
    if (!e || qb_zinput(ctx, argc, argv, &input, &len, &flush))
        return JS_EXCEPTION;
    output = js_malloc(ctx, e->chunk_size);
    if (!output)
        return JS_EXCEPTION;
    if (qb_zstep(e, input, len, flush, output, e->chunk_size, &result)) {
        js_free(ctx, output);
        return JS_Throw(ctx, qb_zerror(ctx, e));
    }
    return qb_zresult(ctx, &result, output);
}
typedef struct {
    uv_work_t work;
    QBRuntime *qr;
    QBZlib *engine;
    JSValue self, callback;
    uint8_t *input, *output;
    size_t len;
    int flush, failed;
    QBZResult result;
} QBZWork;
static void qb_zwork(uv_work_t *req)
{
    QBZWork *w = req->data;
    w->failed = qb_zstep(w->engine, w->input, w->len, w->flush, w->output, w->engine->chunk_size,
                         &w->result);
}
static void qb_zafter(uv_work_t *req, int status)
{
    QBZWork *w = req->data;
    JSContext *ctx = w->qr->ctx;
    JSValue args[2], ret;
    w->engine->busy = 0;
    if (status < 0 || w->failed) {
        args[0] =
            status < 0 ? qb_uv_error(ctx, status, "zlib", NULL, NULL) : qb_zerror(ctx, w->engine);
        args[1] = JS_UNDEFINED;
        js_free(ctx, w->output);
    } else {
        args[0] = JS_NULL;
        args[1] = qb_zresult(ctx, &w->result, w->output);
    }
    if (w->engine->close_pending) {
        qb_zdispose(w->engine);
        w->engine->closed = 1;
    }
    free(w->input);
    ret = qb_call(ctx, w->callback, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, w->callback);
    JS_FreeValue(ctx, w->self);
    free(w);
}
static JSValue qb_zprocess(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    QBZlib *e = qb_zget(ctx, obj);
    const uint8_t *input;
    size_t len;
    int flush, status;
    QBZWork *w;
    if (!e || qb_zinput(ctx, argc, argv, &input, &len, &flush))
        return JS_EXCEPTION;
    if (argc < 3 || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    w = calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    /* A bounded snapshot avoids recopying the entire remainder for every
       output block, while keeping detached/mutated JS storage off workers. */
    w->len = len > e->chunk_size ? e->chunk_size : len;
    w->flush = w->len < len ? 0 : flush;
    w->input = malloc(w->len ? w->len : 1);
    w->output = js_malloc(ctx, e->chunk_size);
    if (!w->input || !w->output) {
        free(w->input);
        js_free(ctx, w->output);
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(w->input, input, w->len);
    w->engine = e;
    w->qr = qb_get(ctx);
    w->self = JS_DupValue(ctx, obj);
    w->callback = JS_DupValue(ctx, argv[2]);
    w->work.data = w;
    e->busy = 1;
    status = uv_queue_work(w->qr->loop, &w->work, qb_zwork, qb_zafter);
    if (status < 0) {
        e->busy = 0;
        free(w->input);
        js_free(ctx, w->output);
        JS_FreeValue(ctx, w->self);
        JS_FreeValue(ctx, w->callback);
        free(w);
        return qb_throw_uv(ctx, status, "zlib", NULL, NULL);
    }
    return JS_UNDEFINED;
}
static JSValue qb_zclose(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    QBZlib *e = JS_GetOpaque2(ctx, obj, qb_zlib_class_id);
    if (!e)
        return JS_EXCEPTION;
    if (e->busy)
        e->close_pending = 1;
    else if (!e->closed) {
        qb_zdispose(e);
        e->closed = 1;
    }
    return JS_UNDEFINED;
}
static JSValue qb_zreset(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    QBZlib *e = qb_zget(ctx, obj);
    if (!e)
        return JS_EXCEPTION;
    qb_zdispose(e);
    if (qb_zinit(e))
        return JS_Throw(ctx, qb_zerror(ctx, e));
    return JS_UNDEFINED;
}
static JSValue qb_zparams(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    QBZlib *e = qb_zget(ctx, obj);
    int level, strategy, status;
    uint8_t *output;
    size_t produced;
    if (!e)
        return JS_EXCEPTION;
    if (e->mode != QB_DEFLATE && e->mode != QB_GZIP && e->mode != QB_DEFLATERAW)
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_INVALID_STATE",
                             "params() requires a deflate engine");
    if (argc < 2 || JS_ToInt32(ctx, &level, argv[0]) || JS_ToInt32(ctx, &strategy, argv[1]))
        return JS_EXCEPTION;
    output = js_malloc(ctx, e->chunk_size);
    if (!output)
        return JS_EXCEPTION;
    e->z.next_in = Z_NULL;
    e->z.avail_in = 0;
    e->z.next_out = output;
    e->z.avail_out = (uInt)e->chunk_size;
    status = deflateParams(&e->z, level, strategy);
    produced = e->chunk_size - e->z.avail_out;
    if (status != Z_OK) {
        js_free(ctx, output);
        qb_zstatus(e, status);
        return JS_Throw(ctx, qb_zerror(ctx, e));
    }
    if (produced > e->max_output - e->total_output) {
        js_free(ctx, output);
        qb_zfail(e, Z_MEM_ERROR, "ERR_BUFFER_TOO_LARGE",
                 "Cannot create a Buffer larger than maxOutputLength");
        return JS_Throw(ctx, qb_zerror(ctx, e));
    }
    e->total_output += produced;
    e->level = level;
    e->strategy = strategy;
    return qb_new_buffer_owned(ctx, output, produced);
}
static JSValue qb_zcreate(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    int mode;
    QBZlib *e;
    JSValue result;
    if (!argc || JS_ToInt32(ctx, &mode, argv[0]))
        return JS_EXCEPTION;
    e = qb_znew(ctx, mode, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (!e)
        return JS_EXCEPTION;
    result = JS_NewObjectClass(ctx, qb_zlib_class_id);
    if (JS_IsException(result)) {
        qb_zfree(e);
        return result;
    }
    JS_SetOpaque(result, e);
    return result;
}
static JSValue qb_zone_shot(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv,
                            int mode)
{
    QBZlib *e;
    const uint8_t *input;
    uint8_t *output = NULL;
    size_t len, offset = 0, used = 0, cap = 0;
    QBZResult r;
    int flush = mode <= QB_UNZIP ? Z_FINISH : 2;
    if (!argc)
        return JS_ThrowTypeError(ctx, "input is required");
    input = qb_get_bytes(ctx, argv[0], &len);
    if (!input)
        return JS_EXCEPTION;
    e = qb_znew(ctx, mode, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (!e)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
        qb_zoption(ctx, argv[1], "finishFlush", &flush))
        goto fail;
    /* Option getters may execute JS and detach or resize the input buffer. */
    input = qb_get_bytes(ctx, argv[0], &len);
    if (!input)
        goto fail;
    do {
        size_t wanted = e->chunk_size;
        uint8_t *next;
        /* Keep one extra byte to distinguish an exactly full output from overflow. */
        if (wanted > e->max_output - used + 1)
            wanted = e->max_output - used + 1;
        if (wanted > SIZE_MAX - used) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (cap - used < wanted) {
            size_t new_cap = cap ? cap : wanted;
            while (new_cap - used < wanted) {
                if (new_cap > SIZE_MAX / 2) {
                    new_cap = used + wanted;
                    break;
                }
                new_cap *= 2;
            }
            next = js_realloc(ctx, output, new_cap);
            if (!next)
                goto fail;
            output = next;
            cap = new_cap;
        }
        if (qb_zstep(e, input + offset, len - offset, flush, output + used, wanted, &r)) {
            JS_Throw(ctx, qb_zerror(ctx, e));
            goto fail;
        }
        offset += r.consumed;
        used += r.produced;
        if (!r.pending && (offset == len || (r.done && !r.consumed && !r.produced)))
            break;
    } while (1);
    qb_zfree(e);
    return qb_new_buffer_owned(ctx, output, used);
fail:
    js_free(ctx, output);
    qb_zfree(e);
    return JS_EXCEPTION;
}
static JSValue qb_zcrc32(JSContext *ctx, JSValueConst obj, int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    uint32_t initial = 0;
    const char *text = NULL;
    uLong sum;
    if (!argc)
        return JS_ThrowTypeError(ctx, "data is required");
    if (argc > 1 && JS_ToUint32(ctx, &initial, argv[1]))
        return JS_EXCEPTION;
    if (JS_IsString(argv[0])) {
        text = JS_ToCStringLen(ctx, &len, argv[0]);
        data = (const uint8_t *)text;
    } else
        data = qb_get_bytes(ctx, argv[0], &len);
    if (!data)
        return JS_EXCEPTION;
    sum = initial;
    while (len) {
        uInt n = len > UINT_MAX ? UINT_MAX : (uInt)len;
        sum = crc32(sum, data, n);
        data += n;
        len -= n;
    }
    if (text)
        JS_FreeCString(ctx, text);
    return JS_NewUint32(ctx, (uint32_t)sum);
}

static const JSCFunctionListEntry qb_zmethods[] = {
    JS_CFUNC_DEF("processSync", 2, qb_zprocess_sync),
    JS_CFUNC_DEF("process", 3, qb_zprocess),
    JS_CFUNC_DEF("close", 0, qb_zclose),
    JS_CFUNC_DEF("reset", 0, qb_zreset),
    JS_CFUNC_DEF("params", 2, qb_zparams),
};
static const JSCFunctionListEntry qb_zfunctions[] = {
    JS_CFUNC_DEF("createEngine", 2, qb_zcreate),
    JS_CFUNC_DEF("crc32", 1, qb_zcrc32),
    JS_CFUNC_MAGIC_DEF("deflateSync", 2, qb_zone_shot, QB_DEFLATE),
    JS_CFUNC_MAGIC_DEF("inflateSync", 2, qb_zone_shot, QB_INFLATE),
    JS_CFUNC_MAGIC_DEF("gzipSync", 2, qb_zone_shot, QB_GZIP),
    JS_CFUNC_MAGIC_DEF("gunzipSync", 2, qb_zone_shot, QB_GUNZIP),
    JS_CFUNC_MAGIC_DEF("deflateRawSync", 2, qb_zone_shot, QB_DEFLATERAW),
    JS_CFUNC_MAGIC_DEF("inflateRawSync", 2, qb_zone_shot, QB_INFLATERAW),
    JS_CFUNC_MAGIC_DEF("unzipSync", 2, qb_zone_shot, QB_UNZIP),
    JS_CFUNC_MAGIC_DEF("brotliCompress", 2, qb_zone_shot, QB_BROTLI_ENCODE),
    JS_CFUNC_MAGIC_DEF("brotliDecompress", 2, qb_zone_shot, QB_BROTLI_DECODE),
    JS_CFUNC_MAGIC_DEF("zstdCompress", 2, qb_zone_shot, QB_ZSTD_COMPRESS),
    JS_CFUNC_MAGIC_DEF("zstdDecompress", 2, qb_zone_shot, QB_ZSTD_DECOMPRESS),
};
#define ZC(name) JS_PROP_INT32_DEF(#name, name, JS_PROP_ENUMERABLE)
static const JSCFunctionListEntry qb_zconstants[] = {
    JS_PROP_INT32_DEF("DEFLATE", 1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("INFLATE", 2, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("GZIP", 3, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("GUNZIP", 4, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DEFLATERAW", 5, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("INFLATERAW", 6, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("UNZIP", 7, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("BROTLI_DECODE", 8, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("BROTLI_ENCODE", 9, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("ZSTD_COMPRESS", 10, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("ZSTD_DECOMPRESS", 11, JS_PROP_ENUMERABLE),
    ZC(Z_NO_FLUSH),
    ZC(Z_PARTIAL_FLUSH),
    ZC(Z_SYNC_FLUSH),
    ZC(Z_FULL_FLUSH),
    ZC(Z_FINISH),
    ZC(Z_BLOCK),
    ZC(Z_TREES),
    ZC(Z_OK),
    ZC(Z_STREAM_END),
    ZC(Z_NEED_DICT),
    ZC(Z_ERRNO),
    ZC(Z_STREAM_ERROR),
    ZC(Z_DATA_ERROR),
    ZC(Z_MEM_ERROR),
    ZC(Z_BUF_ERROR),
    ZC(Z_VERSION_ERROR),
    ZC(Z_NO_COMPRESSION),
    ZC(Z_BEST_SPEED),
    ZC(Z_BEST_COMPRESSION),
    ZC(Z_DEFAULT_COMPRESSION),
    ZC(Z_FILTERED),
    ZC(Z_HUFFMAN_ONLY),
    ZC(Z_RLE),
    ZC(Z_FIXED),
    ZC(Z_DEFAULT_STRATEGY),
    ZC(Z_BINARY),
    ZC(Z_TEXT),
    ZC(Z_ASCII),
    ZC(Z_UNKNOWN),
    ZC(Z_DEFLATED),
    ZC(Z_NULL),
    ZC(ZLIB_VERNUM),
    JS_PROP_INT32_DEF("Z_DEFAULT_LEVEL", -1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MIN_WINDOWBITS", 8, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MAX_WINDOWBITS", 15, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_DEFAULT_WINDOWBITS", 15, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MIN_CHUNK", 64, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_DEFAULT_CHUNK", 16384, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MIN_MEMLEVEL", 1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MAX_MEMLEVEL", 9, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_DEFAULT_MEMLEVEL", 8, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MIN_LEVEL", -1, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("Z_MAX_LEVEL", 9, JS_PROP_ENUMERABLE),
    ZC(BROTLI_OPERATION_PROCESS),
    ZC(BROTLI_OPERATION_FLUSH),
    ZC(BROTLI_OPERATION_FINISH),
    ZC(BROTLI_OPERATION_EMIT_METADATA),
    ZC(BROTLI_PARAM_MODE),
    ZC(BROTLI_PARAM_QUALITY),
    ZC(BROTLI_PARAM_LGWIN),
    ZC(BROTLI_PARAM_LGBLOCK),
    ZC(BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING),
    ZC(BROTLI_PARAM_SIZE_HINT),
    ZC(BROTLI_PARAM_LARGE_WINDOW),
    ZC(BROTLI_PARAM_NPOSTFIX),
    ZC(BROTLI_PARAM_NDIRECT),
    ZC(BROTLI_PARAM_STREAM_OFFSET),
    ZC(BROTLI_MODE_GENERIC),
    ZC(BROTLI_MODE_TEXT),
    ZC(BROTLI_MODE_FONT),
    ZC(BROTLI_MIN_QUALITY),
    ZC(BROTLI_MAX_QUALITY),
    ZC(BROTLI_DEFAULT_QUALITY),
    ZC(BROTLI_MIN_WINDOW_BITS),
    ZC(BROTLI_MAX_WINDOW_BITS),
    ZC(BROTLI_LARGE_MAX_WINDOW_BITS),
    ZC(BROTLI_DEFAULT_WINDOW),
    ZC(BROTLI_DEFAULT_MODE),
    ZC(BROTLI_MIN_INPUT_BLOCK_BITS),
    ZC(BROTLI_MAX_INPUT_BLOCK_BITS),
    JS_PROP_INT32_DEF("BROTLI_MAX_NPOSTFIX", 3, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("BROTLI_MAX_NDIRECT", 120, JS_PROP_ENUMERABLE),
    ZC(BROTLI_DECODER_RESULT_ERROR),
    ZC(BROTLI_DECODER_RESULT_SUCCESS),
    ZC(BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT),
    ZC(BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT),
    ZC(BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION),
    ZC(BROTLI_DECODER_PARAM_LARGE_WINDOW),
#define BC(prefix, name, value) ZC(BROTLI_DECODER##prefix##name),
    BROTLI_DECODER_ERROR_CODES_LIST(BC, )
#undef BC
        ZC(ZSTD_c_compressionLevel),
    ZC(ZSTD_c_windowLog),
    ZC(ZSTD_c_hashLog),
    ZC(ZSTD_c_chainLog),
    ZC(ZSTD_c_searchLog),
    ZC(ZSTD_c_minMatch),
    ZC(ZSTD_c_targetLength),
    ZC(ZSTD_c_strategy),
    ZC(ZSTD_c_targetCBlockSize),
    ZC(ZSTD_c_enableLongDistanceMatching),
    ZC(ZSTD_c_ldmHashLog),
    ZC(ZSTD_c_ldmMinMatch),
    ZC(ZSTD_c_ldmBucketSizeLog),
    ZC(ZSTD_c_ldmHashRateLog),
    ZC(ZSTD_c_contentSizeFlag),
    ZC(ZSTD_c_checksumFlag),
    ZC(ZSTD_c_dictIDFlag),
    ZC(ZSTD_c_nbWorkers),
    ZC(ZSTD_c_jobSize),
    ZC(ZSTD_c_overlapLog),
    ZC(ZSTD_d_windowLogMax),
    ZC(ZSTD_CLEVEL_DEFAULT),
    ZC(ZSTD_VERSION_MAJOR),
    ZC(ZSTD_VERSION_MINOR),
    ZC(ZSTD_VERSION_RELEASE),
    ZC(ZSTD_e_continue),
    ZC(ZSTD_e_flush),
    ZC(ZSTD_e_end),
    ZC(ZSTD_fast),
    ZC(ZSTD_dfast),
    ZC(ZSTD_greedy),
    ZC(ZSTD_lazy),
    ZC(ZSTD_lazy2),
    ZC(ZSTD_btlazy2),
    ZC(ZSTD_btopt),
    ZC(ZSTD_btultra),
    ZC(ZSTD_btultra2),
#define ZEC(name) ZC(ZSTD_error_##name)
    ZEC(no_error),
    ZEC(GENERIC),
    ZEC(prefix_unknown),
    ZEC(version_unsupported),
    ZEC(frameParameter_unsupported),
    ZEC(frameParameter_windowTooLarge),
    ZEC(corruption_detected),
    ZEC(checksum_wrong),
    ZEC(literals_headerWrong),
    ZEC(dictionary_corrupted),
    ZEC(dictionary_wrong),
    ZEC(dictionaryCreation_failed),
    ZEC(parameter_unsupported),
    ZEC(parameter_combination_unsupported),
    ZEC(parameter_outOfBound),
    ZEC(tableLog_tooLarge),
    ZEC(maxSymbolValue_tooLarge),
    ZEC(maxSymbolValue_tooSmall),
    ZEC(cannotProduce_uncompressedBlock),
    ZEC(stabilityCondition_notRespected),
    ZEC(stage_wrong),
    ZEC(init_missing),
    ZEC(memory_allocation),
    ZEC(workSpace_tooSmall),
    ZEC(dstSize_tooSmall),
    ZEC(srcSize_wrong),
    ZEC(dstBuffer_null),
    ZEC(noForwardProgress_destFull),
    ZEC(noForwardProgress_inputEmpty),
    ZEC(frameIndex_tooLarge),
    ZEC(seekableIO),
    ZEC(dstBuffer_wrong),
    ZEC(srcBuffer_wrong),
    ZEC(sequenceProducer_failed),
    ZEC(externalSequences_invalid),
    ZEC(maxCode),
#undef ZEC
};
#undef ZC
static int qb_zlib_module_init(JSContext *ctx, JSModuleDef *module)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, constants;
    uv_once(&qb_zlib_once, qb_zlib_class_once);
    if (!JS_IsRegisteredClass(rt, qb_zlib_class_id) &&
        JS_NewClass(rt, qb_zlib_class_id, &qb_zclass))
        return -1;
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, qb_zmethods, countof(qb_zmethods));
    JS_SetClassProto(ctx, qb_zlib_class_id, proto);
    constants = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, constants, qb_zconstants, countof(qb_zconstants));
    qb_set(ctx, constants, "Z_MAX_CHUNK", JS_NewFloat64(ctx, INFINITY));
    qb_set(ctx, constants, "ZSTD_MIN_CLEVEL", JS_NewInt32(ctx, ZSTD_minCLevel()));
    qb_set(ctx, constants, "ZSTD_MAX_CLEVEL", JS_NewInt32(ctx, ZSTD_maxCLevel()));
    qb_set(ctx, constants, "ZSTD_DEFAULT_CLEVEL", JS_NewInt32(ctx, ZSTD_defaultCLevel()));
    if (JS_SetModuleExport(ctx, module, "constants", constants))
        return -1;
    return JS_SetModuleExportList(ctx, module, qb_zfunctions, countof(qb_zfunctions));
}
JSModuleDef *qb_init_zlib(JSContext *ctx, const char *name)
{
    JSModuleDef *module = JS_NewCModule(ctx, name, qb_zlib_module_init);
    if (!module || JS_AddModuleExportList(ctx, module, qb_zfunctions, countof(qb_zfunctions)) ||
        JS_AddModuleExport(ctx, module, "constants"))
        return NULL;
    return module;
}
