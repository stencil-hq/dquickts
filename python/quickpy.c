/*
 * QuickPy front end: unity aggregator and entry points.
 *
 * This file is #included at the end of quickjs.c when CONFIG_PYTHON is
 * defined, so every python/ source below is part of the quickjs.c
 * translation unit and may use QuickJS internals. Only the functions
 * declared in quickpy.h are non-static.
 *
 * Derived from CPython Python/bltinmodule.c (builtin_compile_impl),
 * Python/pythonrun.c (_Py_SourceAsString, _Py_CompileString) and
 * Python/compile.c (compiler_setup) (PSF-2.0); the batch protocol follows
 * tests/py/ref_dump_ast.py.
 */
#define QUICKPY_INTERNAL

/* Headers (compat.h, ast.h, object.h, ceval.h are already included by
   quickjs.c before the interpreter). */
#include "compat.h"
#include "unicodetype.h"
#include "stringlib.h"
#include "sort.h"
#include "numfmt.h"
#include "dictcore.h"
#include "ast.h"
#include "token.h"
#include "lexer.h"
#include "pegen.h"
#include "symtable.h"
#include "validate.h"
#include "quickpy.h"

/* M1 front end */
#include "ast.c"
#include "ast_json.c"
#include "token.c"
#include "lexer.c"
#include "unicodename.c"
#include "pegen.c"
#include "parser.c"
#include "symtable.c"
#include "validate.c"

/* M2 leaf libraries */
#include "unicodetype.c"
#include "stringlib.c"
#include "sort.c"
#include "numfmt.c"
#include "dictcore.c"

/* M2 object model (python/object.h sections a..l) */
#include "ceval.c"
#include "funcobject.c"
#include "codeobject.c"
#include "frameobject.c"
/* compile.h defines codegen-private VISIT/LOCATION macros that symtable.c
   also defines and #undefs, so it must come after symtable.c. */
#include "compile.h"
#include "compile.c"
#include "longobject.c"
#include "floatobject.c"
#include "complexobject.c"
#include "unicodeobject.c"
#include "unicode_format.c"
#include "templateobject.c"
#include "tupleobject.c"
#include "listobject.c"
#include "rangeobject.c"
#include "sliceobject.c"
#include "iterobject.c"
#include "dictobject.c"
#include "setobject.c"
#include "bytesobject.c"
#include "bytearrayobject.c"
#include "memoryobject.c"
#include "typeobject.c"
#include "descrobject.c"
#include "object.c"
#include "abstract.c"
#include "exceptions.c"
#include "errors.c"
#include "traceback.c"
#include "genobject.c"
#include "getargs.c"
#include "bltinmodule.c"
#include "sysmodule.c"
#include "typevarobject.c"
#include "genericaliasobject.c"
#include "structseq.c"
#include "moduleobject.c"
#include "import.c"
#include "pythonrun.c"

/* native modules: inittab entries live in config.c */
#include "modstate.h"
#include "mod_misc.c"
#include "mod_weakref.c"
#include "mod_thread.c"
#include "mod_time.c"
#include "mod_errno.c"
#include "mod_stat.c"
#include "mod_posix.c"
#include "mod_select.c"
#include "mod_posixsubprocess.c"
#include "mod_codecs.c"
#include "mod_locale.c"
#include "mod_io.c"
#include "mod_imp.c"
/* Boot is activated by including importlib_boot.h before pythonrun.c
   (QP_IMPORTLIB_BOOT); enabled once the M2 core passes L0. */
#include "importlib_boot.h"
#include "importlib_boot.c"
#include "config.c"
/* heap snapshot hooks of quickjs-snapshot.c (M4Snapshot); after config.c */
#include "snapshot.c"

/* _Py_SourceAsString(): compile() rejects sources with embedded NUL bytes
   before parsing. */
static int qpy_check_null_bytes(const char *src, size_t len, qp_error *err)
{
    if (memchr(src, '\0', len) != NULL) {
        qp_error_set(err, QP_ERR_SYNTAX, 0, 0, 0, 0,
                     "source code string cannot contain null bytes");
        return -1;
    }
    return 0;
}

/* Make sure a failed stage leaves an error behind when it ran out of
   memory before it could record one. */
static void qpy_set_memory_error(qp_error *err)
{
    qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
}

int qp_parse(JSContext *ctx, qp_arena *arena, const char *src, size_t len,
             const char *filename, int mode, int flags, mod_ty *out, qp_error *err)
{
    _PyFutureFeatures future;
    mod_ty mod;

    *out = NULL;
    if (qpy_check_null_bytes(src, len, err) < 0)
        return -1;
    mod = qp_parse_string(ctx, arena, src, len, filename, mode, flags, err);
    if (!mod)
        return -1;
    /* _Py_CompileString -> _PyCompile_AstPreprocess runs the __future__
       checks even when only the AST is requested */
    if (!_PyFuture_FromAST(mod, filename, &future, arena, err))
        return -1;
    *out = mod;
    return 0;
}

int qp_check(JSContext *ctx, const char *src, size_t len, const char *filename,
             int mode, int flags, struct qp_error *err)
{
    qp_error local_err;
    qp_arena *arena;
    _PyFutureFeatures future;
    struct symtable *st;
    mod_ty mod;
    int ret = -1;

    if (!err) {
        memset(&local_err, 0, sizeof(local_err));
        err = &local_err;
    }
    if (mode == QP_INPUT_FUNC_TYPE) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "compile() mode 'func_type' requires flag PyCF_ONLY_AST");
        goto done;
    }
    if (qpy_check_null_bytes(src, len, err) < 0)
        goto done;
    arena = qp_arena_new(ctx, err);
    if (!arena) {
        qpy_set_memory_error(err);
        goto done;
    }
    mod = qp_parse_string(ctx, arena, src, len, filename, mode, flags, err);
    if (mod && _PyFuture_FromAST(mod, filename, &future, arena, err)) {
        /* compiler_setup(): merge the __future__ features with the flags */
        future.ff_features |= flags;
        st = _PySymtable_Build(mod, filename, &future, arena, err);
        if (st) {
            ret = qp_validate(st, mod, future.ff_features, err);
            _PySymtable_Free(st);
        }
    }
    qp_arena_free(arena);
 done:
    if (err == &local_err)
        qp_error_clear(&local_err);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* JSON-lines AST protocol (qpy --dump-ast-json-batch) */

/* Append s (UTF-8, lone surrogates allowed as 3-byte sequences) to b,
   writing every non-ASCII code point as a \uXXXX escape (a surrogate pair
   above U+FFFF) like Python's json.dumps(ensure_ascii=True). If
   `string_body` is set, s is the body of a JSON string literal and quotes,
   backslashes and control characters are escaped too; otherwise s is
   already JSON text and only non-ASCII code points are rewritten. */
static void qpy_json_put_ascii(DynBuf *b, const char *s, size_t len, BOOL string_body)
{
    const uint8_t *p = (const uint8_t *)s, *end = p + len, *next;
    int c;

    while (p < end) {
        c = *p;
        if (c < 0x80) {
            p++;
            if (string_body) {
                switch (c) {
                case '"': dbuf_putstr(b, "\\\""); continue;
                case '\\': dbuf_putstr(b, "\\\\"); continue;
                case '\n': dbuf_putstr(b, "\\n"); continue;
                case '\r': dbuf_putstr(b, "\\r"); continue;
                case '\t': dbuf_putstr(b, "\\t"); continue;
                case '\b': dbuf_putstr(b, "\\b"); continue;
                case '\f': dbuf_putstr(b, "\\f"); continue;
                default:
                    if (c < 0x20) {
                        dbuf_printf(b, "\\u%04x", c);
                        continue;
                    }
                    break;
                }
            }
            dbuf_putc(b, c);
            continue;
        }
        c = unicode_from_utf8(p, end - p < 6 ? (int)(end - p) : 6, &next);
        if (c < 0) {
            c = 0xfffd;
            next = p + 1;
        }
        p = next;
        if (c >= 0x10000) {
            c -= 0x10000;
            dbuf_printf(b, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
        } else {
            dbuf_printf(b, "\\u%04x", c);
        }
    }
}

static void qpy_json_put_str(DynBuf *b, const char *s)
{
    dbuf_putc(b, '"');
    qpy_json_put_ascii(b, s, strlen(s), TRUE);
    dbuf_putc(b, '"');
}

/* Append `"name":v`, with null for an unknown (0) position. */
static void qpy_json_put_pos(DynBuf *b, const char *name, int v)
{
    if (v != 0)
        dbuf_printf(b, ",\"%s\":%d", name, v);
    else
        dbuf_printf(b, ",\"%s\":null", name);
}

/* Write an error response; `id` is the request id as JSON text; positions
   equal to 0 are reported as null. */
static void qpy_put_error(DynBuf *b, const char *id, const char *type, const char *msg,
                          int lineno, int offset, int end_lineno, int end_offset)
{
    dbuf_putstr(b, "{\"id\":");
    dbuf_putstr(b, id);
    dbuf_putstr(b, ",\"ok\":false,\"error\":{\"type\":");
    qpy_json_put_str(b, type);
    dbuf_putstr(b, ",\"msg\":");
    qpy_json_put_str(b, msg ? msg : "");
    qpy_json_put_pos(b, "lineno", lineno);
    qpy_json_put_pos(b, "offset", offset);
    qpy_json_put_pos(b, "end_lineno", end_lineno);
    qpy_json_put_pos(b, "end_offset", end_offset);
    dbuf_putstr(b, "}}");
}

/* Write the error response for a failed qp_parse()/qp_check(). Only the
   SyntaxError family carries positions, as in the reference. */
static void qpy_put_qp_error(DynBuf *b, const char *id, const qp_error *err)
{
    const char *type;
    BOOL has_pos = FALSE;

    switch (err->kind) {
    case QP_ERR_SYNTAX: type = "SyntaxError"; has_pos = TRUE; break;
    case QP_ERR_INDENTATION: type = "IndentationError"; has_pos = TRUE; break;
    case QP_ERR_TAB: type = "TabError"; has_pos = TRUE; break;
    case QP_ERR_INCOMPLETE: type = "_IncompleteInputError"; has_pos = TRUE; break;
    case QP_ERR_MEMORY: type = "MemoryError"; break;
    case QP_ERR_VALUE: type = "ValueError"; break;
    case QP_ERR_UNICODE_DECODE: type = "UnicodeDecodeError"; break;
    case QP_ERR_RECURSION: type = "RecursionError"; break;
    case QP_ERR_OVERFLOW: type = "OverflowError"; break;
    default:
        /* a stage failed without recording an error: CPython's message */
        qpy_put_error(b, id, "SystemError", "error return without exception set",
                      0, 0, 0, 0);
        return;
    }
    if (has_pos)
        qpy_put_error(b, id, type, err->msg, err->lineno, err->offset,
                      err->end_lineno, err->end_offset);
    else
        qpy_put_error(b, id, type, err->msg, 0, 0, 0, 0);
}

/* Python truthiness of a JSON-decoded value (bool(req.get(key))). */
static int qpy_json_truthy(JSContext *ctx, JSValueConst v)
{
    uint32_t count;

    if (JS_IsObject(v)) {
        if (JS_GetOwnPropertyCount(ctx, &count, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
        return count != 0;
    }
    return JS_ToBool(ctx, v);
}

/* Python type name of a JSON-decoded value, for TypeError messages. */
static const char *qpy_json_type_name(JSContext *ctx, JSValueConst v)
{
    double d;

    if (JS_IsNull(v) || JS_IsUndefined(v))
        return "NoneType";
    if (JS_IsBool(v))
        return "bool";
    if (JS_IsString(v))
        return "str";
    if (JS_IsNumber(v)) {
        if (JS_ToFloat64(ctx, &d, v) == 0 && isfinite(d) && d == trunc(d))
            return "int";
        return "float";
    }
    if (JS_IsArray(ctx, v) > 0)
        return "list";
    return "dict";
}

/* Convert a JSON string argument of compile() (filename or mode) to UTF-8
   (free with JS_FreeCString). Returns NULL with the exception type in
   *ptype and CPython's message in msgbuf on an argument error. */
static const char *qpy_get_str_arg(JSContext *ctx, JSValueConst v, BOOL is_mode,
                                   const char **ptype, char *msgbuf, size_t msgbuf_size)
{
    const char *s;
    size_t len;
    const char *tname;

    if (!JS_IsString(v)) {
        *ptype = "TypeError";
        tname = qpy_json_type_name(ctx, v);
        if (is_mode)
            snprintf(msgbuf, msgbuf_size, "compile() argument 'mode' must be str, not %s",
                     strcmp(tname, "NoneType") == 0 ? "None" : tname);
        else
            snprintf(msgbuf, msgbuf_size,
                     "expected str, bytes or os.PathLike object, not %s", tname);
        return NULL;
    }
    s = JS_ToCStringLen2(ctx, &len, v, FALSE);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        *ptype = "MemoryError";
        msgbuf[0] = '\0';
        return NULL;
    }
    if (memchr(s, '\0', len) != NULL) {
        JS_FreeCString(ctx, s);
        *ptype = "ValueError";
        snprintf(msgbuf, msgbuf_size, "embedded null character");
        return NULL;
    }
    return s;
}

/* PyUnicode_AsUTF8AndSize() of a str with lone surrogates raises
   UnicodeEncodeError for the first run of surrogate code points. `s` is the
   WTF-8 conversion of the source (valid pairs already combined). Returns 1
   and writes CPython's message to msgbuf if a lone surrogate is found. */
static int qpy_find_lone_surrogates(const char *s, size_t len, char *msgbuf, size_t msgbuf_size)
{
    const uint8_t *p = (const uint8_t *)s, *end = p + len;
    size_t index = 0, start;
    uint32_t first;

    while (p < end) {
        if (end - p >= 3 && p[0] == 0xed && p[1] >= 0xa0) {
            first = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            start = index;
            while (end - p >= 3 && p[0] == 0xed && p[1] >= 0xa0) {
                p += 3;
                index++;
            }
            if (index == start + 1)
                snprintf(msgbuf, msgbuf_size,
                         "'utf-8' codec can't encode character '\\u%04x' in position %zu: "
                         "surrogates not allowed", (unsigned)first, start);
            else
                snprintf(msgbuf, msgbuf_size,
                         "'utf-8' codec can't encode characters in position %zu-%zu: "
                         "surrogates not allowed", start, index - 1);
            return 1;
        }
        p++;
        while (p < end && (*p & 0xc0) == 0x80)
            p++;
        index++;
    }
    return 0;
}

/* Answer one request line (NUL-terminated at line[len]) into resp,
   following tests/py/ref_dump_ast.py: compile(source, filename, mode,
   flags, dont_inherit=True) with PyCF_ONLY_AST unless "check" is set. */
static void qpy_handle_request(JSContext *ctx, const char *line, size_t len, DynBuf *resp)
{
    JSValue req, val = JS_UNDEFINED, src_val = JS_UNDEFINED;
    char *id = NULL;
    DynBuf id_buf;
    const char *filename = NULL, *mode_str = NULL, *src = NULL, *s;
    const char *err_type = "TypeError";
    char msgbuf[256];
    size_t src_len;
    int check, tla, mode, flags;
    qp_error err;
    qp_arena *arena;
    mod_ty mod;
    size_t start;

    dbuf_init(&id_buf);
    memset(&err, 0, sizeof(err));

    req = JS_ParseJSON(ctx, line, len, "<stdin>");
    if (JS_IsException(req)) {
        JSValue exc = JS_GetException(ctx);
        s = JS_ToCString(ctx, exc);
        qpy_put_error(resp, "null", "ProtocolError", s ? s : "invalid JSON", 0, 0, 0, 0);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        return;
    }
    if (JS_IsObject(req) && JS_IsArray(ctx, req) == 0)
        src_val = JS_GetPropertyStr(ctx, req, "source");
    if (JS_IsException(src_val))
        goto js_fail_noid;
    if (!JS_IsString(src_val)) {
        qpy_put_error(resp, "null", "ProtocolError",
                      "request must be a JSON object with a string 'source'", 0, 0, 0, 0);
        goto done;
    }

    /* id: echoed as JSON text */
    val = JS_GetPropertyStr(ctx, req, "id");
    if (JS_IsException(val))
        goto js_fail_noid;
    if (!JS_IsUndefined(val)) {
        JSValue str = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(str))
            goto js_fail_noid;
        if (JS_IsString(str)) {
            s = JS_ToCStringLen2(ctx, &src_len, str, FALSE);
            JS_FreeValue(ctx, str);
            if (!s)
                goto js_fail_noid;
            qpy_json_put_ascii(&id_buf, s, src_len, FALSE);
            JS_FreeCString(ctx, s);
        } else {
            JS_FreeValue(ctx, str);
        }
    }
    JS_FreeValue(ctx, val);
    val = JS_UNDEFINED;
    if (id_buf.size == 0)
        dbuf_putstr(&id_buf, "null");
    if (dbuf_putc(&id_buf, '\0'))
        goto js_fail_noid;
    id = (char *)id_buf.buf;

    val = JS_GetPropertyStr(ctx, req, "check");
    check = JS_IsException(val) ? -1 : qpy_json_truthy(ctx, val);
    JS_FreeValue(ctx, val);
    val = JS_GetPropertyStr(ctx, req, "top_level_await");
    tla = JS_IsException(val) ? -1 : qpy_json_truthy(ctx, val);
    JS_FreeValue(ctx, val);
    val = JS_UNDEFINED;
    if (check < 0 || tla < 0)
        goto js_fail;

    /* builtin_compile_impl argument conversion order: filename, mode,
       mode validation, then the source text */
    val = JS_GetPropertyStr(ctx, req, "filename");
    if (JS_IsException(val))
        goto js_fail;
    if (!JS_IsUndefined(val)) {
        filename = qpy_get_str_arg(ctx, val, FALSE, &err_type, msgbuf, sizeof(msgbuf));
        if (!filename)
            goto arg_error;
    }
    JS_FreeValue(ctx, val);
    val = JS_GetPropertyStr(ctx, req, "mode");
    if (JS_IsException(val))
        goto js_fail;
    if (!JS_IsUndefined(val)) {
        mode_str = qpy_get_str_arg(ctx, val, TRUE, &err_type, msgbuf, sizeof(msgbuf));
        if (!mode_str)
            goto arg_error;
    }
    JS_FreeValue(ctx, val);
    val = JS_UNDEFINED;

    s = mode_str ? mode_str : "exec";
    if (strcmp(s, "exec") == 0) {
        mode = QP_INPUT_FILE;
    } else if (strcmp(s, "eval") == 0) {
        mode = QP_INPUT_EVAL;
    } else if (strcmp(s, "single") == 0) {
        mode = QP_INPUT_SINGLE;
    } else if (strcmp(s, "func_type") == 0) {
        if (check) {
            qpy_put_error(resp, id, "ValueError",
                          "compile() mode 'func_type' requires flag PyCF_ONLY_AST",
                          0, 0, 0, 0);
            goto done;
        }
        mode = QP_INPUT_FUNC_TYPE;
    } else {
        qpy_put_error(resp, id, "ValueError",
                      check ? "compile() mode must be 'exec', 'eval' or 'single'"
                      : "compile() mode must be 'exec', 'eval', 'single' or 'func_type'",
                      0, 0, 0, 0);
        goto done;
    }

    src = JS_ToCStringLen2(ctx, &src_len, src_val, FALSE);
    if (!src)
        goto js_fail;
    if (qpy_find_lone_surrogates(src, src_len, msgbuf, sizeof(msgbuf))) {
        qpy_put_error(resp, id, "UnicodeEncodeError", msgbuf, 0, 0, 0, 0);
        goto done;
    }

    flags = QP_PF_SOURCE_IS_UTF8 | (tla ? QP_PF_TOP_LEVEL_AWAIT : 0);
    if (!filename)
        s = "<unknown>";
    else
        s = filename;
    if (check) {
        if (qp_check(ctx, src, src_len, s, mode, flags, &err) < 0) {
            qpy_put_qp_error(resp, id, &err);
        } else {
            dbuf_putstr(resp, "{\"id\":");
            dbuf_putstr(resp, id);
            dbuf_putstr(resp, ",\"ok\":true}");
        }
    } else {
        arena = qp_arena_new(ctx, &err);
        if (!arena) {
            qpy_set_memory_error(&err);
            qpy_put_qp_error(resp, id, &err);
            goto done;
        }
        if (qp_parse(ctx, arena, src, src_len, s, mode, flags, &mod, &err) < 0) {
            qpy_put_qp_error(resp, id, &err);
        } else {
            start = resp->size;
            dbuf_putstr(resp, "{\"id\":");
            dbuf_putstr(resp, id);
            dbuf_putstr(resp, ",\"ok\":true,\"ast\":");
            if (qp_ast_dump_json(ctx, resp, mod) < 0 || dbuf_error(resp)) {
                resp->size = start;
                resp->error = FALSE;
                qpy_set_memory_error(&err);
                qpy_put_qp_error(resp, id, &err);
            } else {
                dbuf_putc(resp, '}');
            }
        }
        qp_arena_free(arena);
    }
    goto done;

 arg_error:
    qpy_put_error(resp, id, err_type, msgbuf, 0, 0, 0, 0);
    goto done;
 js_fail_noid:
    id = NULL;
 js_fail:
    /* only reachable on memory exhaustion while reading the request */
    JS_FreeValue(ctx, JS_GetException(ctx));
    qpy_put_error(resp, id ? id : "null", "MemoryError", "", 0, 0, 0, 0);
 done:
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, mode_str);
    JS_FreeCString(ctx, filename);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, src_val);
    JS_FreeValue(ctx, req);
    qp_error_clear(&err);
    dbuf_free(&id_buf);
}

int qp_dump_ast_json_batch(JSContext *ctx, FILE *in, FILE *out)
{
    static const char oom_resp[] =
        "{\"id\":null,\"ok\":false,\"error\":{\"type\":\"MemoryError\",\"msg\":\"\","
        "\"lineno\":null,\"offset\":null,\"end_lineno\":null,\"end_offset\":null}}";
    DynBuf line, resp;
    size_t i;
    int c, ret = 0;
    uint8_t ch;
    BOOL blank;

    dbuf_init(&line);
    js_dbuf_init(ctx, &resp);
    for (;;) {
        line.size = 0;
        while ((c = getc(in)) != EOF && c != '\n')
            dbuf_putc(&line, c);
        if (c == EOF && (ferror(in) || line.size == 0)) {
            if (ferror(in))
                ret = -1;
            break;
        }
        if (dbuf_putc(&line, '\0')) {
            ret = -1;
            break;
        }
        line.size--;
        /* `if not line.strip(): continue` */
        blank = TRUE;
        for (i = 0; i < line.size; i++) {
            ch = line.buf[i];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\f' && ch != '\v') {
                blank = FALSE;
                break;
            }
        }
        if (!blank) {
            resp.size = 0;
            resp.error = FALSE;
            qpy_handle_request(ctx, (const char *)line.buf, line.size, &resp);
            if (dbuf_error(&resp))
                fputs(oom_resp, out);
            else
                fwrite(resp.buf, 1, resp.size, out);
            fputc('\n', out);
            if (fflush(out) != 0 || ferror(out)) {
                ret = -1;
                break;
            }
        }
        if (feof(in))
            break;
    }
    dbuf_free(&resp);
    dbuf_free(&line);
    return ret;
}

#undef QUICKPY_INTERNAL
