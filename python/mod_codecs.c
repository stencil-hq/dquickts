/*
 * _codecs: the codec registry, the error handler registry with the builtin
 * error handlers, and the builtin codecs.
 *
 * Port of CPython 3.15.2 Modules/_codecsmodule.c, Python/codecs.c and the
 * codec section of Objects/unicodeobject.c (UTF-7, UTF-8, UTF-16, UTF-32,
 * Latin-1, ASCII, charmap, unicode-escape, raw-unicode-escape) plus
 * _PyBytes_DecodeEscape2 (Objects/bytesobject.c) (PSF-2.0).
 *
 * The registry (search functions, lookup cache, error handlers) lives in
 * QPModState like CPython's interp->codecs; it is created on first use, so
 * the str type can reach it before `_codecs` is imported.  The
 * Windows-only functions (mbcs_*, oem_*, code_page_*) are absent as in a
 * POSIX build of CPython.
 *
 * Entry points for the str type (python/unicodeobject.c) beyond its builtin
 * fast paths: qp_codec_encode_text / qp_codec_decode_text
 * (_PyCodec_EncodeText / _PyCodec_DecodeText) and qp_codec_lookup_error
 * (PyCodec_LookupError).
 */
#include "modstate.h"

/* python/unicodename.c */
static int qp_unicode_lookup_name(const char *name, size_t len, uint32_t *cp);
static int qp_unicode_get_name(uint32_t code, char *buffer, int buflen);

/* _PyUnicode_GetNameCAPI: the first use of the character name database
   (\N{...} escapes, the namereplace handler) imports the unicodedata
   module, as CPython's PyCapsule_Import("unicodedata._ucnhash_CAPI"); the
   names themselves come from the built-in table.  Before the import system
   is set up (boot code), or in a parser-only context without Python state
   (--dump-ast-json-batch), the table is used without importing.  0, or -1
   with the import error pending. */
static int qp_unicode_name_capi(JSContext *ctx)
{
    QPModState *ms;
    JSValue m;

    if (!QP_STATE(ctx) || !JS_IsObject(QP_STATE(ctx)->importlib))
        return 0;
    ms = qp_modstate(ctx);
    if (!ms)
        return -1;
    if (ms->ucnhash_loaded)
        return 0;
    m = qp_import_module(ctx, "unicodedata");
    if (JS_IsException(m))
        return -1;
    JS_FreeValue(ctx, m);
    ms->ucnhash_loaded = TRUE;
    return 0;
}

#define QP_CD_MAX_UNICODE 0x10FFFF
#define QP_CD_IS_SURR(ch)      ((ch) >= 0xD800 && (ch) <= 0xDFFF)
#define QP_CD_IS_HIGH_SURR(ch) ((ch) >= 0xD800 && (ch) <= 0xDBFF)
#define QP_CD_IS_LOW_SURR(ch)  ((ch) >= 0xDC00 && (ch) <= 0xDFFF)
#define QP_CD_JOIN_SURR(hi, lo) (((((Py_UCS4)(hi) & 0x3FF) << 10) | ((Py_UCS4)(lo) & 0x3FF)) + 0x10000)

static const char qp_cd_hexdigits[] = "0123456789abcdef";

/* CPython codecs_builtin_error_handlers order (QPModState.codec_builtin_handlers) */
enum {
    QP_CEH_STRICT, QP_CEH_IGNORE, QP_CEH_REPLACE, QP_CEH_XMLCHARREFREPLACE,
    QP_CEH_BACKSLASHREPLACE, QP_CEH_NAMEREPLACE, QP_CEH_SURROGATEPASS,
    QP_CEH_SURROGATEESCAPE, QP_CEH_COUNT
};
static const char *const qp_cd_eh_names[QP_CEH_COUNT] = {
    "strict", "ignore", "replace", "xmlcharrefreplace", "backslashreplace",
    "namereplace", "surrogatepass", "surrogateescape",
};

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

static inline BOOL qp_cd_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static JSValue qp_cd_getattr(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return r;
}

/* Consumes v. */
static int qp_cd_setattr(JSContext *ctx, JSValueConst obj, const char *name, JSValue v)
{
    JSAtom a;
    int r;
    if (JS_IsException(v))
        return -1;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_setattr(ctx, obj, a, v);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return r;
}

/* PyCallable_Check */
static BOOL qp_cd_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* _PyArg_BadArgument: "%s() %s must be %s, not %s" with Py_TYPE(arg)->tp_name. */
static JSValue qp_cd_bad_argument(JSContext *ctx, const char *fname, const char *displayname,
                                  const char *expected, JSValueConst arg)
{
    JSValue tn;
    QPType *t;
    if (qp_cd_is_none(arg)) {
        tn = qp_str_from_cstr(ctx, "None");
    } else {
        t = qp_type_of(ctx, arg);
        tn = t->tp_name ? qp_str_from_cstr(ctx, t->tp_name) : qp_str_from_atom(ctx, t->name);
    }
    if (JS_IsException(tn))
        return JS_EXCEPTION;
    qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50U", fname, displayname,
                        expected, tn);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* UTF-8 C string of a str (or str subclass); free with JS_FreeCString.
   ValueError "embedded null character" like PyUnicode_AsUTF8AndSize users
   of Argument Clinic's str converter. */
static const char *qp_cd_utf8(JSContext *ctx, JSValueConst v, BOOL check_nul)
{
    JSValue s = qp_str_value(ctx, v);
    const char *p;
    size_t len;
    if (JS_IsException(s))
        return NULL;
    p = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (!p)
        return NULL;
    if (check_nul && strlen(p) != len) {
        JS_FreeCString(ctx, p);
        qp_raise_value_error(ctx, "embedded null character");
        return NULL;
    }
    return p;
}

/* Clinic `str` / `str(accept={str, NoneType})` converter.  *out = NULL for
   None (accept_none) or an absent argument (QP_NULL). */
static int qp_cd_str_arg(JSContext *ctx, const char *fname, const char *argdesc,
                         JSValueConst v, BOOL accept_none, const char **out)
{
    *out = NULL;
    if (QP_IS_NULL(v) || (accept_none && qp_cd_is_none(v)))
        return 0;
    if (!qp_str_check(ctx, v)) {
        qp_cd_bad_argument(ctx, fname, argdesc, accept_none ? "str or None" : "str", v);
        return -1;
    }
    *out = qp_cd_utf8(ctx, v, TRUE);
    return *out ? 0 : -1;
}

static void qp_cd_free_cstr(JSContext *ctx, const char *s)
{
    if (s)
        JS_FreeCString(ctx, s);
}

/* (obj, n) result tuple; consumes obj. */
static JSValue qp_cd_tuple2(JSContext *ctx, JSValue obj, Py_ssize_t n)
{
    JSValue items[2];
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    items[0] = obj;
    items[1] = qp_long_from_int64(ctx, n);
    if (JS_IsException(items[1])) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

/* str from a builder (freed). */
static JSValue qp_cd_str_from_builder(JSContext *ctx, qp_ucsbuilder *b)
{
    JSValue r = qp_str_from_view(ctx, qp_ucsbuilder_view(b));
    qp_ucsbuilder_free(b);
    return r;
}

/* bytes from a DynBuf (freed). */
static JSValue qp_cd_bytes_from_dbuf(JSContext *ctx, DynBuf *d)
{
    JSValue r;
    if (d->error) {
        dbuf_free(d);
        return qp_raise_memory_error(ctx);
    }
    r = qp_bytes_new(ctx, d->buf, d->size);
    dbuf_free(d);
    return r;
}

static JSValue qp_cd_str_ascii(JSContext *ctx, const char *s, size_t len)
{
    return qp_str_from_utf8(ctx, s, len);
}

/* ========================================================================= */
/* Codec registry state (CPython interp->codecs)                              */
/* ========================================================================= */

static JSValue qp_cd_eh_strict(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_ignore(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_replace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_xmlcharrefreplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_backslashreplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_namereplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_surrogatepass(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic);
static JSValue qp_cd_eh_surrogateescape(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic);

/* _PyCodec_InitRegistry methods (registered under qp_cd_eh_names[i]) */
static const QPMethodDef qp_cd_eh_defs[QP_CEH_COUNT] = {
    { "strict_errors", qp_cd_eh_strict, QP_METH_O, 0,
      "Implements the 'strict' error handling, which raises a UnicodeError on coding errors." },
    { "ignore_errors", qp_cd_eh_ignore, QP_METH_O, 0,
      "Implements the 'ignore' error handling, which ignores malformed data and continues." },
    { "replace_errors", qp_cd_eh_replace, QP_METH_O, 0,
      "Implements the 'replace' error handling, which replaces malformed data with a replacement marker." },
    { "xmlcharrefreplace_errors", qp_cd_eh_xmlcharrefreplace, QP_METH_O, 0,
      "Implements the 'xmlcharrefreplace' error handling, which replaces an unencodable "
      "character with the appropriate XML character reference." },
    { "backslashreplace_errors", qp_cd_eh_backslashreplace, QP_METH_O, 0,
      "Implements the 'backslashreplace' error handling, which replaces malformed data "
      "with a backslashed escape sequence." },
    { "namereplace_errors", qp_cd_eh_namereplace, QP_METH_O, 0,
      "Implements the 'namereplace' error handling, which replaces an unencodable "
      "character with a \\N{...} escape sequence." },
    { "surrogatepass", qp_cd_eh_surrogatepass, QP_METH_O, 0, NULL },
    { "surrogateescape", qp_cd_eh_surrogateescape, QP_METH_O, 0, NULL },
};

/* The registry of ctx, created on first use (_PyCodec_InitRegistry minus
   the `encodings` import, which the boot code performs). */
static QPModState *qp_codec_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);
    JSValue path, cache, reg, f, key;
    int i, r;

    if (!ms)
        return NULL;
    if (!QP_IS_NULL(ms->codec_search_path))
        return ms;
    path = qp_list_new(ctx, 0);
    if (JS_IsException(path))
        return NULL;
    cache = qp_dict_new(ctx);
    if (JS_IsException(cache)) {
        JS_FreeValue(ctx, path);
        return NULL;
    }
    reg = qp_dict_new(ctx);
    if (JS_IsException(reg)) {
        JS_FreeValue(ctx, path);
        JS_FreeValue(ctx, cache);
        return NULL;
    }
    for (i = 0; i < QP_CEH_COUNT; i++) {
        f = qp_cfunction_new(ctx, &qp_cd_eh_defs[i], QP_NULL, JS_UNDEFINED);
        if (JS_IsException(f))
            goto fail;
        key = qp_str_from_cstr(ctx, qp_cd_eh_names[i]);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, f);
            goto fail;
        }
        r = qp_dict_set_item(ctx, reg, key, f);
        JS_FreeValue(ctx, key);
        if (r < 0) {
            JS_FreeValue(ctx, f);
            goto fail;
        }
        JS_FreeValue(ctx, ms->codec_builtin_handlers[i]);
        ms->codec_builtin_handlers[i] = f;
    }
    ms->codec_search_path = path;
    ms->codec_search_cache = cache;
    ms->codec_error_registry = reg;
    return ms;
fail:
    JS_FreeValue(ctx, path);
    JS_FreeValue(ctx, cache);
    JS_FreeValue(ctx, reg);
    return NULL;
}

/* PyCodec_Register */
static int qp_codec_register(JSContext *ctx, JSValueConst search_function)
{
    QPModState *ms = qp_codec_state(ctx);
    if (!ms)
        return -1;
    if (!qp_cd_callable(ctx, search_function)) {
        qp_raise_type_error(ctx, "argument must be callable");
        return -1;
    }
    return qp_list_append(ctx, ms->codec_search_path, search_function);
}

/* PyCodec_Unregister */
static int qp_codec_unregister(JSContext *ctx, JSValueConst search_function)
{
    QPModState *ms = qp_codec_state(ctx);
    Py_ssize_t i, n;
    JSValue *items, idx, cache;
    int r;

    if (!ms)
        return -1;
    n = qp_list_size(ms->codec_search_path);
    for (i = 0; i < n; i++) {
        JSValue item;
        items = qp_list_items(ms->codec_search_path, &n);
        if (items) {
            if (i >= n)
                break;
            item = JS_DupValue(ctx, items[i]);
        } else {
            JSValue k = qp_long_from_int64(ctx, i);
            item = qp_getitem(ctx, ms->codec_search_path, k);
            JS_FreeValue(ctx, k);
            if (JS_IsException(item))
                return -1;
        }
        if (qp_is(item, search_function)) {
            idx = qp_long_from_int64(ctx, i);
            r = qp_delitem(ctx, ms->codec_search_path, idx);
            JS_FreeValue(ctx, idx);
            JS_FreeValue(ctx, item);
            if (r < 0)
                return -1;
            /* PyDict_Clear(search_cache) */
            cache = qp_dict_new(ctx);
            if (JS_IsException(cache))
                return -1;
            JS_FreeValue(ctx, ms->codec_search_cache);
            ms->codec_search_cache = cache;
            return 0;
        }
        JS_FreeValue(ctx, item);
        n = qp_list_size(ms->codec_search_path);
    }
    return 0;
}

/* normalizestring(): ASCII letters lowered, spaces -> hyphens. */
static JSValue qp_cd_normalizestring(JSContext *ctx, const char *string)
{
    size_t i, len = strlen(string);
    char *p = js_malloc(ctx, len + 1);
    JSValue v;
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)string[i];
        if (ch == ' ')
            ch = '-';
        else if (ch >= 'A' && ch <= 'Z')
            ch = ch - 'A' + 'a';
        p[i] = (char)ch;
    }
    p[len] = '\0';
    v = qp_str_from_utf8(ctx, p, len);
    js_free(ctx, p);
    return v;
}

/* list[i] (new reference), IndexError like PyList_GetItemRef. */
static JSValue qp_cd_list_get(JSContext *ctx, JSValueConst list, Py_ssize_t i)
{
    Py_ssize_t n;
    JSValue *items = qp_list_items(list, &n), k, r;
    if (items) {
        if (i < 0 || i >= n)
            return qp_raise_index_error(ctx, "list index out of range");
        return JS_DupValue(ctx, items[i]);
    }
    k = qp_long_from_int64(ctx, i);
    if (JS_IsException(k))
        return k;
    r = qp_getitem(ctx, list, k);
    JS_FreeValue(ctx, k);
    return r;
}

/* _PyCodec_Lookup: the CodecInfo (4-tuple) registered for encoding. */
static JSValue qp_codec_lookup(JSContext *ctx, const char *encoding)
{
    QPModState *ms;
    JSValue v, result = QP_NULL, func;
    Py_ssize_t i, len;
    int r;

    if (!encoding) {
        qp_raise_type_error(ctx, "bad argument type for built-in operation");
        return JS_EXCEPTION;
    }
    ms = qp_codec_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    v = qp_cd_normalizestring(ctx, encoding);
    if (JS_IsException(v))
        return v;
    r = qp_dict_get_item(ctx, ms->codec_search_cache, v, &result);
    if (r < 0)
        goto fail;
    if (r > 0) {
        JS_FreeValue(ctx, v);
        return result;
    }
    len = qp_list_size(ms->codec_search_path);
    if (len == 0) {
        qp_raise(ctx, QP_EXC(ctx, LookupError),
                 "no codec search functions registered: can't find encoding");
        goto fail;
    }
    result = QP_NULL;
    for (i = 0; i < len; i++) {
        func = qp_cd_list_get(ctx, ms->codec_search_path, i);
        if (JS_IsException(func))
            goto fail;
        result = qp_call1(ctx, func, v);
        JS_FreeValue(ctx, func);
        if (JS_IsException(result))
            goto fail;
        if (qp_cd_is_none(result)) {
            JS_FreeValue(ctx, result);
            result = QP_NULL;
            continue;
        }
        if (!qp_tuple_check(ctx, result) || QP_TUPLE_SIZE(result) != 4) {
            JS_FreeValue(ctx, result);
            qp_raise_type_error(ctx, "codec search functions must return 4-tuples");
            goto fail;
        }
        break;
    }
    if (QP_IS_NULL(result)) {
        qp_raise(ctx, QP_EXC(ctx, LookupError), "unknown encoding: %s", encoding);
        goto fail;
    }
    if (qp_dict_set_item(ctx, ms->codec_search_cache, v, result) < 0) {
        JS_FreeValue(ctx, result);
        goto fail;
    }
    JS_FreeValue(ctx, v);
    return result;
fail:
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

/* codec_getitem: item `index` of the CodecInfo for encoding. */
static JSValue qp_cd_codec_getitem(JSContext *ctx, const char *encoding, int index)
{
    JSValue codec = qp_codec_lookup(ctx, encoding), v;
    if (JS_IsException(codec))
        return codec;
    v = JS_DupValue(ctx, QP_TUPLE_ITEMS(codec)[index]);
    JS_FreeValue(ctx, codec);
    return v;
}

/* _PyErr_FormatNote(fmt, what, encoding) on the pending exception. */
static void qp_cd_add_note(JSContext *ctx, const char *what, const char *encoding)
{
    JSValue exc, note, r, arg;
    if (!JS_HasException(ctx))
        return;
    exc = qp_exc_fetch(ctx);
    note = qp_str_from_format(ctx, "%s with '%s' codec failed", what, encoding);
    if (JS_IsException(note))
        goto error;
    if (!qp_exc_check(ctx, exc)) {
        JS_FreeValue(ctx, note);
        qp_raise_type_error(ctx, "exc must be an exception, not '%T'", exc);
        goto error;
    }
    arg = note;
    r = qp_call_method(ctx, exc, QP_ATOM(ctx, add_note), (JSValueConst *)&arg, 1, QP_NULL);
    JS_FreeValue(ctx, note);
    if (JS_IsException(r))
        goto error;
    JS_FreeValue(ctx, r);
    JS_Throw(ctx, exc);
    return;
error:
    {
        /* _PyErr_ChainExceptions1(exc) */
        JSValue cur = qp_exc_fetch(ctx);
        qp_cd_setattr(ctx, cur, "__context__", exc);
        JS_Throw(ctx, cur);
    }
}

/* args_tuple + call + "(object, integer)" result check
   (_PyCodec_EncodeInternal / _PyCodec_DecodeInternal); consumes coder. */
static JSValue qp_cd_call_coder(JSContext *ctx, JSValueConst object, JSValue coder,
                                const char *encoding, const char *errors, BOOL decode)
{
    JSValue args[2], result, v;
    int nargs = 1;

    args[0] = JS_DupValue(ctx, object);
    if (errors) {
        args[1] = qp_str_from_cstr(ctx, errors);
        if (JS_IsException(args[1])) {
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, coder);
            return JS_EXCEPTION;
        }
        nargs = 2;
    }
    result = qp_vectorcall(ctx, coder, (JSValueConst *)args, nargs, QP_NULL);
    JS_FreeValue(ctx, args[0]);
    if (nargs == 2)
        JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, coder);
    if (JS_IsException(result)) {
        qp_cd_add_note(ctx, decode ? "decoding" : "encoding", encoding);
        return JS_EXCEPTION;
    }
    if (!qp_tuple_check(ctx, result) || QP_TUPLE_SIZE(result) != 2) {
        JS_FreeValue(ctx, result);
        qp_raise_type_error(ctx, decode ? "decoder must return a tuple (object,integer)"
                                        : "encoder must return a tuple (object, integer)");
        return JS_EXCEPTION;
    }
    v = JS_DupValue(ctx, QP_TUPLE_ITEMS(result)[0]);
    JS_FreeValue(ctx, result);
    return v;
}

/* PyCodec_Encode / PyCodec_Decode */
static JSValue qp_codec_encode(JSContext *ctx, JSValueConst object, const char *encoding,
                               const char *errors)
{
    JSValue coder = qp_cd_codec_getitem(ctx, encoding, 0);
    if (JS_IsException(coder))
        return coder;
    return qp_cd_call_coder(ctx, object, coder, encoding, errors, FALSE);
}

static JSValue qp_codec_decode(JSContext *ctx, JSValueConst object, const char *encoding,
                               const char *errors)
{
    JSValue coder = qp_cd_codec_getitem(ctx, encoding, 1);
    if (JS_IsException(coder))
        return coder;
    return qp_cd_call_coder(ctx, object, coder, encoding, errors, TRUE);
}

/* _PyCodec_LookupTextEncoding */
static JSValue qp_codec_lookup_text_encoding(JSContext *ctx, const char *encoding,
                                             const char *alternate_command)
{
    JSValue codec = qp_codec_lookup(ctx, encoding), attr;
    JSAtom a;
    int r, is_text;

    if (JS_IsException(codec))
        return codec;
    if (!qp_tuple_check_exact(ctx, codec)) {
        a = JS_NewAtom(ctx, "_is_text_encoding");
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, codec);
            return JS_EXCEPTION;
        }
        r = qp_getattr_opt(ctx, codec, a, &attr);
        JS_FreeAtom(ctx, a);
        if (r < 0) {
            JS_FreeValue(ctx, codec);
            return JS_EXCEPTION;
        }
        if (r > 0) {
            is_text = qp_bool(ctx, attr);
            JS_FreeValue(ctx, attr);
            if (is_text <= 0) {
                JS_FreeValue(ctx, codec);
                if (is_text == 0) {
                    if (alternate_command)
                        qp_raise(ctx, QP_EXC(ctx, LookupError),
                                 "'%.400s' is not a text encoding; use %s to handle arbitrary codecs",
                                 encoding, alternate_command);
                    else
                        qp_raise(ctx, QP_EXC(ctx, LookupError),
                                 "'%.400s' is not a text encoding", encoding);
                }
                return JS_EXCEPTION;
            }
        }
    }
    return codec;
}

static JSValue qp_cd_text_coder(JSContext *ctx, const char *encoding, BOOL decode)
{
    JSValue codec = qp_codec_lookup_text_encoding(ctx, encoding,
                                                  decode ? "codecs.decode()" : "codecs.encode()");
    JSValue v;
    if (JS_IsException(codec))
        return codec;
    v = JS_DupValue(ctx, QP_TUPLE_ITEMS(codec)[decode ? 1 : 0]);
    JS_FreeValue(ctx, codec);
    return v;
}

/* _PyCodec_EncodeText / _PyCodec_DecodeText: the codec call of
   PyUnicode_AsEncodedString / PyUnicode_Decode for encodings the str type
   does not implement itself (or error handlers it does not know), with
   the result type checks of those functions. */
static JSValue qp_codec_encode_text(JSContext *ctx, JSValueConst s, const char *encoding,
                                    const char *errors)
{
    JSValue coder, v, b;
    if (!encoding)
        encoding = "utf-8";
    coder = qp_cd_text_coder(ctx, encoding, FALSE);
    if (JS_IsException(coder))
        return coder;
    v = qp_cd_call_coder(ctx, s, coder, encoding, errors, FALSE);
    if (JS_IsException(v))
        return v;
    if (qp_bytes_check(ctx, v))
        return v;
    if (qp_bytearray_check(ctx, v)) {
        /* PyUnicode_AsEncodedString: bytearray results are accepted with a
           RuntimeWarning and converted to bytes */
        if (qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 1,
                    "encoder %s returned bytearray instead of bytes; use codecs.encode() "
                    "to encode to arbitrary types", encoding) < 0) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        b = qp_bytes_from_object(ctx, v);
        JS_FreeValue(ctx, v);
        return b;
    }
    qp_raise_type_error(ctx, "'%.400s' encoder returned '%T' instead of 'bytes'; "
                        "use codecs.encode() to encode to arbitrary types", encoding, v);
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

static JSValue qp_codec_decode_text(JSContext *ctx, JSValueConst obj, const char *encoding,
                                    const char *errors)
{
    JSValue coder, v;
    if (!encoding)
        encoding = "utf-8";
    coder = qp_cd_text_coder(ctx, encoding, TRUE);
    if (JS_IsException(coder))
        return coder;
    v = qp_cd_call_coder(ctx, obj, coder, encoding, errors, TRUE);
    if (JS_IsException(v))
        return v;
    if (!qp_str_check(ctx, v)) {
        qp_raise_type_error(ctx, "'%.400s' decoder returned '%T' instead of 'str'; "
                            "use codecs.decode() to decode to arbitrary types", encoding, v);
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

/* PyCodec_RegisterError */
static int qp_codec_register_error(JSContext *ctx, const char *name, JSValueConst error)
{
    QPModState *ms = qp_codec_state(ctx);
    JSValue key;
    int r;
    if (!ms)
        return -1;
    if (!qp_cd_callable(ctx, error)) {
        qp_raise_type_error(ctx, "handler must be callable");
        return -1;
    }
    key = qp_str_from_cstr(ctx, name);
    if (JS_IsException(key))
        return -1;
    r = qp_dict_set_item(ctx, ms->codec_error_registry, key, error);
    JS_FreeValue(ctx, key);
    return r;
}

/* _PyCodec_UnregisterError: 1 removed, 0 not registered, -1 error. */
static int qp_codec_unregister_error(JSContext *ctx, const char *name)
{
    QPModState *ms;
    JSValue key;
    int i, r;
    for (i = 0; i < QP_CEH_COUNT; i++) {
        if (!strcmp(name, qp_cd_eh_names[i])) {
            qp_raise_value_error(ctx, "cannot un-register built-in error handler '%s'", name);
            return -1;
        }
    }
    ms = qp_codec_state(ctx);
    if (!ms)
        return -1;
    key = qp_str_from_cstr(ctx, name);
    if (JS_IsException(key))
        return -1;
    r = qp_dict_del_item(ctx, ms->codec_error_registry, key);
    JS_FreeValue(ctx, key);
    return r;
}

/* PyCodec_LookupError (name NULL = "strict"). */
static JSValue qp_codec_lookup_error(JSContext *ctx, const char *name)
{
    QPModState *ms = qp_codec_state(ctx);
    JSValue key, handler;
    int r;
    if (!ms)
        return JS_EXCEPTION;
    if (!name)
        name = "strict";
    key = qp_str_from_cstr(ctx, name);
    if (JS_IsException(key))
        return key;
    r = qp_dict_get_item(ctx, ms->codec_error_registry, key, &handler);
    JS_FreeValue(ctx, key);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise(ctx, QP_EXC(ctx, LookupError), "unknown error handler name '%.400s'", name);
    return handler;
}

/* unicode_check_encoding_errors (Objects/unicodeobject.c): in development
   mode (-X dev), check that encoding and errors name a codec and an error
   handler even when they are not used.  Not before the `encodings` search
   function is registered (CPython: before the filesystem codec is set) nor
   during finalization.  0 or -1 (LookupError). */
static int qp_codec_check_encoding_errors(JSContext *ctx, const char *encoding, const char *errors)
{
    QPState *st = QP_STATE(ctx);
    QPModState *ms = st->mod_state;
    JSValue h;

    if (!encoding && !errors)
        return 0;
    if (!st->config.dev_mode || st->finalizing)
        return 0;
    if (!ms || QP_IS_NULL(ms->codec_search_path) || qp_list_size(ms->codec_search_path) == 0)
        return 0;
    if (encoding && strcmp(encoding, "utf-8") && strcmp(encoding, "utf8") &&
        strcmp(encoding, "ascii")) {
        h = qp_codec_lookup(ctx, encoding);
        if (JS_IsException(h))
            return -1;
        JS_FreeValue(ctx, h);
    }
    if (errors && strcmp(errors, "strict") && strcmp(errors, "ignore") &&
        strcmp(errors, "replace") && strcmp(errors, "surrogateescape") &&
        strcmp(errors, "surrogatepass")) {
        h = qp_codec_lookup_error(ctx, errors);
        if (JS_IsException(h))
            return -1;
        JS_FreeValue(ctx, h);
    }
    return 0;
}

/* ========================================================================= */
/* Builtin error handlers (Python/codecs.c)                                   */
/* ========================================================================= */

typedef struct QPUeParams {
    JSValue obj;            /* exception object (exact str or bytes) */
    BOOL as_bytes;
    qp_strview v;           /* str: code points */
    const uint8_t *bytes;   /* bytes data */
    Py_ssize_t objlen, start, end, slen;
} QPUeParams;

static void qp_cd_ue_params_free(JSContext *ctx, QPUeParams *p)
{
    if (!p->as_bytes && p->v.data)
        qp_str_view_release(ctx, &p->v);
    JS_FreeValue(ctx, p->obj);
    p->obj = JS_UNDEFINED;
}

static int qp_cd_ue_ssize_attr(JSContext *ctx, JSValueConst exc, const char *name, Py_ssize_t *out)
{
    JSValue v = qp_cd_getattr(ctx, exc, name), iv;
    int r;
    if (JS_IsException(v))
        return -1;
    iv = qp_index(ctx, v);
    JS_FreeValue(ctx, v);
    if (JS_IsException(iv))
        return -1;
    r = qp_as_ssize(ctx, iv, QP_EXC(ctx, OverflowError), out);
    JS_FreeValue(ctx, iv);
    return r;
}

/* _PyUnicodeError_GetParams */
static int qp_cd_ue_params(JSContext *ctx, JSValueConst exc, BOOL as_bytes, QPUeParams *p)
{
    JSValue obj;
    Py_ssize_t n, start, end;

    memset(p, 0, sizeof(*p));
    p->obj = JS_UNDEFINED;
    p->as_bytes = as_bytes;
    obj = qp_cd_getattr(ctx, exc, "object");
    if (JS_IsException(obj))
        return -1;
    if (qp_cd_is_none(obj)) {
        JS_FreeValue(ctx, obj);
        qp_raise_type_error(ctx, "UnicodeError 'object' attribute is not set");
        return -1;
    }
    if (as_bytes ? !qp_bytes_check(ctx, obj) : !qp_str_check(ctx, obj)) {
        JS_FreeValue(ctx, obj);
        qp_raise_type_error(ctx, "UnicodeError 'object' attribute must be a %s",
                            as_bytes ? "bytes" : "string");
        return -1;
    }
    if (as_bytes) {
        p->obj = obj;
        p->bytes = qp_bytes_data(obj, &n);
    } else {
        p->obj = qp_str_value(ctx, obj);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(p->obj))
            return -1;
        if (qp_str_view_acquire(ctx, p->obj, &p->v) < 0) {
            p->v.data = NULL;
            qp_cd_ue_params_free(ctx, p);
            return -1;
        }
        n = p->v.len;
    }
    if (qp_cd_ue_ssize_attr(ctx, exc, "start", &start) < 0 ||
        qp_cd_ue_ssize_attr(ctx, exc, "end", &end) < 0) {
        qp_cd_ue_params_free(ctx, p);
        return -1;
    }
    /* unicode_error_adjust_start / _end / _len */
    if (start < 0)
        start = 0;
    if (start >= n)
        start = n == 0 ? 0 : n - 1;
    if (end < 1)
        end = 1;
    if (end > n)
        end = n;
    p->objlen = n;
    p->start = start;
    p->end = end;
    p->slen = end - start < 0 ? 0 : end - start;
    return 0;
}

static inline Py_UCS4 qp_cd_ue_char(const QPUeParams *p, Py_ssize_t i)
{
    return QP_SL_READ(p->v.kind, p->v.data, i);
}

#define QP_CD_EXC_IS(ctx, exc, name) qp_typecheck((ctx), (exc), QP_EXC(ctx, name))

/* wrong_exception_type */
static JSValue qp_cd_wrong_exception_type(JSContext *ctx, JSValueConst exc)
{
    return qp_raise_type_error(ctx, "don't know how to handle %T in error callback", exc);
}

static JSValue qp_cd_eh_strict(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    if (qp_exc_check(ctx, exc))
        return qp_raise_value(ctx, qp_type_of(ctx, exc), exc);
    return qp_raise_type_error(ctx, "codec must pass exception instance");
}

static JSValue qp_cd_eh_ignore(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    Py_ssize_t end;
    BOOL as_bytes;
    if (QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError) || QP_CD_EXC_IS(ctx, exc, UnicodeTranslateError))
        as_bytes = FALSE;
    else if (QP_CD_EXC_IS(ctx, exc, UnicodeDecodeError))
        as_bytes = TRUE;
    else
        return qp_cd_wrong_exception_type(ctx, exc);
    if (qp_cd_ue_params(ctx, exc, as_bytes, &p) < 0)
        return JS_EXCEPTION;
    end = p.end;
    qp_cd_ue_params_free(ctx, &p);
    return qp_cd_tuple2(ctx, JS_AtomToString(ctx, JS_ATOM_empty_string), end);
}

static JSValue qp_cd_eh_replace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    JSValue res;
    qp_ucsbuilder b;
    Py_ssize_t end;

    if (QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError)) {
        char *buf;
        if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
            return JS_EXCEPTION;
        buf = js_malloc(ctx, p.slen + 1);
        if (!buf) {
            qp_cd_ue_params_free(ctx, &p);
            return JS_EXCEPTION;
        }
        memset(buf, '?', p.slen);
        res = qp_cd_str_ascii(ctx, buf, p.slen);
        js_free(ctx, buf);
        end = p.end;
        qp_cd_ue_params_free(ctx, &p);
        return qp_cd_tuple2(ctx, res, end);
    }
    if (QP_CD_EXC_IS(ctx, exc, UnicodeDecodeError)) {
        if (qp_cd_ue_params(ctx, exc, TRUE, &p) < 0)
            return JS_EXCEPTION;
        end = p.end;
        qp_cd_ue_params_free(ctx, &p);
        return qp_cd_tuple2(ctx, qp_str_from_char(ctx, 0xFFFD), end);
    }
    if (QP_CD_EXC_IS(ctx, exc, UnicodeTranslateError)) {
        if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
            return JS_EXCEPTION;
        qp_ucsbuilder_init(&b, ctx->rt);
        if (qp_ucsbuilder_fill(&b, 0xFFFD, p.slen) < 0) {
            qp_ucsbuilder_free(&b);
            qp_cd_ue_params_free(ctx, &p);
            return qp_raise_memory_error(ctx);
        }
        end = p.end;
        qp_cd_ue_params_free(ctx, &p);
        return qp_cd_tuple2(ctx, qp_cd_str_from_builder(ctx, &b), end);
    }
    return qp_cd_wrong_exception_type(ctx, exc);
}

static int qp_cd_n_decimal_digits(Py_UCS4 ch)
{
    if (ch < 10) return 1;
    if (ch < 100) return 2;
    if (ch < 1000) return 3;
    if (ch < 10000) return 4;
    if (ch < 100000) return 5;
    if (ch < 1000000) return 6;
    return 7;
}

static JSValue qp_cd_eh_xmlcharrefreplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    DynBuf d;
    Py_ssize_t i, end;
    JSValue res;
    char tmp[16];

    if (!QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError))
        return qp_cd_wrong_exception_type(ctx, exc);
    if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    for (i = p.start; i < p.end; i++) {
        Py_UCS4 ch = qp_cd_ue_char(&p, i);
        int k = qp_cd_n_decimal_digits(ch), j;
        dbuf_putc(&d, '&');
        dbuf_putc(&d, '#');
        for (j = k - 1; j >= 0; j--) {
            tmp[j] = (char)('0' + ch % 10);
            ch /= 10;
        }
        dbuf_put(&d, (const uint8_t *)tmp, k);
        dbuf_putc(&d, ';');
    }
    end = p.end;
    qp_cd_ue_params_free(ctx, &p);
    if (d.error) {
        dbuf_free(&d);
        return qp_raise_memory_error(ctx);
    }
    res = qp_cd_str_ascii(ctx, (const char *)d.buf, d.size);
    dbuf_free(&d);
    return qp_cd_tuple2(ctx, res, end);
}

/* codec_handler_write_unicode_hex */
static void qp_cd_put_unicode_hex(DynBuf *d, Py_UCS4 ch)
{
    dbuf_putc(d, '\\');
    if (ch >= 0x10000) {
        dbuf_putc(d, 'U');
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 28) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 24) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 20) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 16) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 12) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 8) & 0xf]);
    } else if (ch >= 0x100) {
        dbuf_putc(d, 'u');
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 12) & 0xf]);
        dbuf_putc(d, qp_cd_hexdigits[(ch >> 8) & 0xf]);
    } else {
        dbuf_putc(d, 'x');
    }
    dbuf_putc(d, qp_cd_hexdigits[(ch >> 4) & 0xf]);
    dbuf_putc(d, qp_cd_hexdigits[ch & 0xf]);
}

static JSValue qp_cd_eh_backslashreplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    DynBuf d;
    Py_ssize_t i, end;
    JSValue res;
    BOOL as_bytes;

    if (QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError) || QP_CD_EXC_IS(ctx, exc, UnicodeTranslateError))
        as_bytes = FALSE;
    else if (QP_CD_EXC_IS(ctx, exc, UnicodeDecodeError))
        as_bytes = TRUE;
    else
        return qp_cd_wrong_exception_type(ctx, exc);
    if (qp_cd_ue_params(ctx, exc, as_bytes, &p) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    for (i = p.start; i < p.end; i++) {
        if (as_bytes) {
            uint8_t ch = p.bytes[i];
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'x');
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[ch & 0xf]);
        } else {
            qp_cd_put_unicode_hex(&d, qp_cd_ue_char(&p, i));
        }
    }
    end = p.end;
    qp_cd_ue_params_free(ctx, &p);
    if (d.error) {
        dbuf_free(&d);
        return qp_raise_memory_error(ctx);
    }
    res = qp_cd_str_ascii(ctx, (const char *)d.buf, d.size);
    dbuf_free(&d);
    return qp_cd_tuple2(ctx, res, end);
}

static JSValue qp_cd_eh_namereplace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    DynBuf d;
    Py_ssize_t i, end;
    JSValue res;
    char buffer[256];   /* NAME_MAXLEN */

    if (!QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError))
        return qp_cd_wrong_exception_type(ctx, exc);
    if (qp_unicode_name_capi(ctx) < 0)
        return JS_EXCEPTION;
    if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    for (i = p.start; i < p.end; i++) {
        Py_UCS4 c = qp_cd_ue_char(&p, i);
        if (qp_unicode_get_name(c, buffer, sizeof(buffer))) {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'N');
            dbuf_putc(&d, '{');
            dbuf_putstr(&d, buffer);
            dbuf_putc(&d, '}');
        } else {
            qp_cd_put_unicode_hex(&d, c);
        }
    }
    end = p.end;
    qp_cd_ue_params_free(ctx, &p);
    if (d.error) {
        dbuf_free(&d);
        return qp_raise_memory_error(ctx);
    }
    res = qp_cd_str_ascii(ctx, (const char *)d.buf, d.size);
    dbuf_free(&d);
    return qp_cd_tuple2(ctx, res, end);
}

#define QP_CD_ENC_UNKNOWN  (-1)
#define QP_CD_ENC_UTF8     0
#define QP_CD_ENC_UTF16BE  1
#define QP_CD_ENC_UTF16LE  2
#define QP_CD_ENC_UTF32BE  3
#define QP_CD_ENC_UTF32LE  4

static inline int qp_cd_tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* get_standard_encoding_impl */
static int qp_cd_standard_encoding(const char *encoding, int *bytelength)
{
    if (qp_cd_tolower(encoding[0]) == 'u' && qp_cd_tolower(encoding[1]) == 't' &&
        qp_cd_tolower(encoding[2]) == 'f') {
        encoding += 3;
        if (*encoding == '-' || *encoding == '_')
            encoding++;
        if (encoding[0] == '8' && encoding[1] == '\0') {
            *bytelength = 3;
            return QP_CD_ENC_UTF8;
        } else if (encoding[0] == '1' && encoding[1] == '6') {
            encoding += 2;
            *bytelength = 2;
            if (*encoding == '\0')
                return QP_CD_ENC_UTF16LE;
            if (*encoding == '-' || *encoding == '_')
                encoding++;
            if (encoding[0] != '\0' && qp_cd_tolower(encoding[1]) == 'e' && encoding[2] == '\0') {
                if (qp_cd_tolower(encoding[0]) == 'b')
                    return QP_CD_ENC_UTF16BE;
                if (qp_cd_tolower(encoding[0]) == 'l')
                    return QP_CD_ENC_UTF16LE;
            }
        } else if (encoding[0] == '3' && encoding[1] == '2') {
            encoding += 2;
            *bytelength = 4;
            if (*encoding == '\0')
                return QP_CD_ENC_UTF32LE;
            if (*encoding == '-' || *encoding == '_')
                encoding++;
            if (encoding[0] != '\0' && qp_cd_tolower(encoding[1]) == 'e' && encoding[2] == '\0') {
                if (qp_cd_tolower(encoding[0]) == 'b')
                    return QP_CD_ENC_UTF32BE;
                if (qp_cd_tolower(encoding[0]) == 'l')
                    return QP_CD_ENC_UTF32LE;
            }
        }
    } else if (strcmp(encoding, "cp65001") == 0) {
        *bytelength = 3;
        return QP_CD_ENC_UTF8;
    }
    return QP_CD_ENC_UNKNOWN;
}

/* Encoding attribute of a Unicode{En,De}codeError as a standard encoding
   code; -1 on error. */
static int qp_cd_exc_standard_encoding(JSContext *ctx, JSValueConst exc, int *code, int *bytelength)
{
    JSValue enc = qp_cd_getattr(ctx, exc, "encoding");
    const char *s;
    if (JS_IsException(enc))
        return -1;
    if (qp_cd_is_none(enc)) {
        JS_FreeValue(ctx, enc);
        qp_raise_type_error(ctx, "UnicodeError 'encoding' attribute is not set");
        return -1;
    }
    if (!qp_str_check(ctx, enc)) {
        JS_FreeValue(ctx, enc);
        qp_raise_type_error(ctx, "UnicodeError 'encoding' attribute must be a string");
        return -1;
    }
    s = qp_cd_utf8(ctx, enc, FALSE);
    JS_FreeValue(ctx, enc);
    if (!s)
        return -1;
    *code = qp_cd_standard_encoding(s, bytelength);
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue qp_cd_reraise(JSContext *ctx, JSValueConst exc)
{
    return qp_raise_value(ctx, qp_type_of(ctx, exc), exc);
}

static JSValue qp_cd_eh_surrogatepass(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    int code, bytelength;
    Py_ssize_t i;

    if (QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError)) {
        uint8_t *out, *outp;
        JSValue res;
        Py_ssize_t end;
        if (qp_cd_exc_standard_encoding(ctx, exc, &code, &bytelength) < 0)
            return JS_EXCEPTION;
        if (code == QP_CD_ENC_UNKNOWN)
            return qp_cd_reraise(ctx, exc);
        if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
            return JS_EXCEPTION;
        out = js_malloc(ctx, bytelength * p.slen + 1);
        if (!out) {
            qp_cd_ue_params_free(ctx, &p);
            return JS_EXCEPTION;
        }
        outp = out;
        for (i = p.start; i < p.end; i++) {
            Py_UCS4 ch = qp_cd_ue_char(&p, i);
            if (!QP_CD_IS_SURR(ch)) {
                js_free(ctx, out);
                qp_cd_ue_params_free(ctx, &p);
                return qp_cd_reraise(ctx, exc);
            }
            switch (code) {
            case QP_CD_ENC_UTF8:
                *outp++ = (uint8_t)(0xe0 | (ch >> 12));
                *outp++ = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
                *outp++ = (uint8_t)(0x80 | (ch & 0x3f));
                break;
            case QP_CD_ENC_UTF16LE:
                *outp++ = (uint8_t)ch;
                *outp++ = (uint8_t)(ch >> 8);
                break;
            case QP_CD_ENC_UTF16BE:
                *outp++ = (uint8_t)(ch >> 8);
                *outp++ = (uint8_t)ch;
                break;
            case QP_CD_ENC_UTF32LE:
                *outp++ = (uint8_t)ch;
                *outp++ = (uint8_t)(ch >> 8);
                *outp++ = (uint8_t)(ch >> 16);
                *outp++ = (uint8_t)(ch >> 24);
                break;
            case QP_CD_ENC_UTF32BE:
                *outp++ = (uint8_t)(ch >> 24);
                *outp++ = (uint8_t)(ch >> 16);
                *outp++ = (uint8_t)(ch >> 8);
                *outp++ = (uint8_t)ch;
                break;
            }
        }
        end = p.end;
        qp_cd_ue_params_free(ctx, &p);
        res = qp_bytes_new(ctx, out, outp - out);
        js_free(ctx, out);
        return qp_cd_tuple2(ctx, res, end);
    }
    if (QP_CD_EXC_IS(ctx, exc, UnicodeDecodeError)) {
        Py_UCS4 ch = 0;
        const uint8_t *q;
        Py_ssize_t start;
        if (qp_cd_exc_standard_encoding(ctx, exc, &code, &bytelength) < 0)
            return JS_EXCEPTION;
        if (code == QP_CD_ENC_UNKNOWN)
            return qp_cd_reraise(ctx, exc);
        if (qp_cd_ue_params(ctx, exc, TRUE, &p) < 0)
            return JS_EXCEPTION;
        /* Try decoding a single surrogate character.  If there are more,
           let the codec call us again. */
        q = p.bytes + p.start;
        if (p.objlen - p.start >= bytelength) {
            switch (code) {
            case QP_CD_ENC_UTF8:
                if ((q[0] & 0xf0) == 0xe0 && (q[1] & 0xc0) == 0x80 && (q[2] & 0xc0) == 0x80)
                    ch = ((q[0] & 0x0f) << 12) + ((q[1] & 0x3f) << 6) + (q[2] & 0x3f);
                break;
            case QP_CD_ENC_UTF16LE:
                ch = q[1] << 8 | q[0];
                break;
            case QP_CD_ENC_UTF16BE:
                ch = q[0] << 8 | q[1];
                break;
            case QP_CD_ENC_UTF32LE:
                ch = ((Py_UCS4)q[3] << 24) | (q[2] << 16) | (q[1] << 8) | q[0];
                break;
            case QP_CD_ENC_UTF32BE:
                ch = ((Py_UCS4)q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
                break;
            }
        }
        start = p.start;
        qp_cd_ue_params_free(ctx, &p);
        if (!QP_CD_IS_SURR(ch))
            return qp_cd_reraise(ctx, exc);
        return qp_cd_tuple2(ctx, qp_str_from_char(ctx, ch), start + bytelength);
    }
    return qp_cd_wrong_exception_type(ctx, exc);
}

static JSValue qp_cd_eh_surrogateescape(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst exc = args[0];
    QPUeParams p;
    Py_ssize_t i, end, start;

    if (QP_CD_EXC_IS(ctx, exc, UnicodeEncodeError)) {
        uint8_t *out;
        JSValue res;
        if (qp_cd_ue_params(ctx, exc, FALSE, &p) < 0)
            return JS_EXCEPTION;
        out = js_malloc(ctx, p.slen + 1);
        if (!out) {
            qp_cd_ue_params_free(ctx, &p);
            return JS_EXCEPTION;
        }
        for (i = p.start; i < p.end; i++) {
            Py_UCS4 ch = qp_cd_ue_char(&p, i);
            if (ch < 0xdc80 || ch > 0xdcff) {
                /* Not a UTF-8b surrogate, fail with original exception. */
                js_free(ctx, out);
                qp_cd_ue_params_free(ctx, &p);
                return qp_cd_reraise(ctx, exc);
            }
            out[i - p.start] = (uint8_t)(ch - 0xdc00);
        }
        end = p.end;
        res = qp_bytes_new(ctx, out, p.slen);
        js_free(ctx, out);
        qp_cd_ue_params_free(ctx, &p);
        return qp_cd_tuple2(ctx, res, end);
    }
    if (QP_CD_EXC_IS(ctx, exc, UnicodeDecodeError)) {
        uint32_t ch[4];
        int consumed = 0;
        if (qp_cd_ue_params(ctx, exc, TRUE, &p) < 0)
            return JS_EXCEPTION;
        while (consumed < 4 && consumed < p.slen) {
            /* Refuse to escape ASCII bytes. */
            if (p.bytes[p.start + consumed] < 128)
                break;
            ch[consumed] = 0xdc00 + p.bytes[p.start + consumed];
            consumed++;
        }
        start = p.start;
        qp_cd_ue_params_free(ctx, &p);
        if (consumed == 0)
            return qp_cd_reraise(ctx, exc);
        return qp_cd_tuple2(ctx, qp_str_from_ucs4(ctx, ch, consumed), start + consumed);
    }
    return qp_cd_wrong_exception_type(ctx, exc);
}

/* ========================================================================= */
/* Calling error handlers from the codecs (Objects/unicodeobject.c)           */
/* ========================================================================= */

typedef struct QPCodecErrors {
    const char *errors;     /* handler name, NULL = "strict" */
    JSValue handler;        /* looked up at the first error, else QP_NULL */
    JSValue exc;            /* exception object reused across errors, else QP_NULL */
} QPCodecErrors;

static void qp_cd_errors_init(QPCodecErrors *ce, const char *errors)
{
    ce->errors = errors;
    ce->handler = QP_NULL;
    ce->exc = QP_NULL;
}

static void qp_cd_errors_free(JSContext *ctx, QPCodecErrors *ce)
{
    JS_FreeValue(ctx, ce->handler);
    JS_FreeValue(ctx, ce->exc);
    ce->handler = QP_NULL;
    ce->exc = QP_NULL;
}

static int qp_cd_errors_handler(JSContext *ctx, QPCodecErrors *ce)
{
    if (QP_IS_NULL(ce->handler)) {
        JSValue h = qp_codec_lookup_error(ctx, ce->errors);
        if (JS_IsException(h))
            return -1;
        ce->handler = h;
    }
    return 0;
}

/* make_decode_exception: create the UnicodeDecodeError (object = a copy of
   data[0:len]) or update start/end/reason of the existing one. */
static int qp_cd_make_decode_exc(JSContext *ctx, QPCodecErrors *ce, const char *encoding,
                                 const uint8_t *data, Py_ssize_t len, Py_ssize_t start,
                                 Py_ssize_t end, const char *reason)
{
    if (QP_IS_NULL(ce->exc)) {
        JSValue v[5], exc;
        int i;
        v[0] = qp_str_from_cstr(ctx, encoding);
        v[1] = qp_bytes_new(ctx, data, len);
        v[2] = qp_long_from_int64(ctx, start);
        v[3] = qp_long_from_int64(ctx, end);
        v[4] = qp_str_from_cstr(ctx, reason);
        exc = JS_UNDEFINED;
        for (i = 0; i < 5; i++) {
            if (JS_IsException(v[i]))
                exc = JS_EXCEPTION;
        }
        if (!JS_IsException(exc))
            exc = qp_exc_new(ctx, QP_EXC(ctx, UnicodeDecodeError), 5, (JSValueConst *)v);
        for (i = 0; i < 5; i++)
            JS_FreeValue(ctx, v[i]);
        if (JS_IsException(exc))
            return -1;
        ce->exc = exc;
        return 0;
    }
    if (qp_cd_setattr(ctx, ce->exc, "start", qp_long_from_int64(ctx, start)) < 0 ||
        qp_cd_setattr(ctx, ce->exc, "end", qp_long_from_int64(ctx, end)) < 0 ||
        qp_cd_setattr(ctx, ce->exc, "reason", qp_str_from_cstr(ctx, reason)) < 0) {
        JS_FreeValue(ctx, ce->exc);
        ce->exc = QP_NULL;
        return -1;
    }
    return 0;
}

/* make_encode_exception */
static int qp_cd_make_encode_exc(JSContext *ctx, QPCodecErrors *ce, const char *encoding,
                                 JSValueConst str, Py_ssize_t start, Py_ssize_t end,
                                 const char *reason)
{
    if (QP_IS_NULL(ce->exc)) {
        JSValue v[5], exc;
        int i;
        v[0] = qp_str_from_cstr(ctx, encoding);
        v[1] = JS_DupValue(ctx, str);
        v[2] = qp_long_from_int64(ctx, start);
        v[3] = qp_long_from_int64(ctx, end);
        v[4] = qp_str_from_cstr(ctx, reason);
        exc = JS_UNDEFINED;
        for (i = 0; i < 5; i++) {
            if (JS_IsException(v[i]))
                exc = JS_EXCEPTION;
        }
        if (!JS_IsException(exc))
            exc = qp_exc_new(ctx, QP_EXC(ctx, UnicodeEncodeError), 5, (JSValueConst *)v);
        for (i = 0; i < 5; i++)
            JS_FreeValue(ctx, v[i]);
        if (JS_IsException(exc))
            return -1;
        ce->exc = exc;
        return 0;
    }
    if (qp_cd_setattr(ctx, ce->exc, "start", qp_long_from_int64(ctx, start)) < 0 ||
        qp_cd_setattr(ctx, ce->exc, "end", qp_long_from_int64(ctx, end)) < 0 ||
        qp_cd_setattr(ctx, ce->exc, "reason", qp_str_from_cstr(ctx, reason)) < 0) {
        JS_FreeValue(ctx, ce->exc);
        ce->exc = QP_NULL;
        return -1;
    }
    return 0;
}

/* raise_encode_exception */
static int qp_cd_raise_encode(JSContext *ctx, QPCodecErrors *ce, const char *encoding,
                              JSValueConst str, Py_ssize_t start, Py_ssize_t end,
                              const char *reason)
{
    if (qp_cd_make_encode_exc(ctx, ce, encoding, str, start, end, reason) == 0)
        qp_cd_reraise(ctx, ce->exc);
    return -1;
}

/* Check the (replacement, newpos) tuple of a handler: *prep (new ref) and
   *pnewpos (not yet adjusted). */
static int qp_cd_handler_result(JSContext *ctx, JSValueConst restuple, BOOL decode,
                                JSValue *prep, Py_ssize_t *pnewpos)
{
    const char *msg = decode ? "decoding error handler must return (str, int) tuple"
                             : "encoding error handler must return (str/bytes, int) tuple";
    JSValueConst rep;
    JSValue iv;
    int r;

    if (!qp_tuple_check(ctx, restuple) || QP_TUPLE_SIZE(restuple) != 2) {
        qp_raise_type_error(ctx, "%s", msg);
        return -1;
    }
    rep = QP_TUPLE_ITEMS(restuple)[0];
    if (decode ? !qp_str_check(ctx, rep) : !(qp_str_check(ctx, rep) || qp_bytes_check(ctx, rep))) {
        qp_raise_type_error(ctx, "%s", msg);
        return -1;
    }
    iv = qp_index(ctx, QP_TUPLE_ITEMS(restuple)[1]);
    if (JS_IsException(iv))
        return -1;
    r = qp_as_ssize(ctx, iv, QP_EXC(ctx, OverflowError), pnewpos);
    JS_FreeValue(ctx, iv);
    if (r < 0)
        return -1;
    if (qp_str_check(ctx, rep))
        *prep = qp_str_value(ctx, rep);
    else
        *prep = qp_bytes_check(ctx, rep) ? JS_DupValue(ctx, rep) : JS_UNDEFINED;
    return JS_IsException(*prep) ? -1 : 0;
}

/* Decoder input: the caller's buffer until the first error, then the
   `object` of the exception (which the handler may replace). */
typedef struct QPDecIn {
    const uint8_t *data;
    Py_ssize_t len;
    JSValue obj;            /* owner of data once replaced, else QP_NULL */
} QPDecIn;

static void qp_cd_decin_init(QPDecIn *in, const void *data, Py_ssize_t len)
{
    in->data = (const uint8_t *)data;
    in->len = len;
    in->obj = QP_NULL;
}

static void qp_cd_decin_free(JSContext *ctx, QPDecIn *in)
{
    JS_FreeValue(ctx, in->obj);
    in->obj = QP_NULL;
}

/* Append str to the builder. */
static int qp_cd_builder_write_str(JSContext *ctx, qp_ucsbuilder *out, JSValueConst s)
{
    qp_strview v;
    int r;
    if (qp_str_view_acquire(ctx, s, &v) < 0)
        return -1;
    r = qp_ucsbuilder_write(out, v, 0, v.len);
    qp_str_view_release(ctx, &v);
    if (r < 0) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    return 0;
}

static int qp_cd_putc(JSContext *ctx, qp_ucsbuilder *out, Py_UCS4 ch)
{
    if (qp_ucsbuilder_putc(out, ch) < 0) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    return 0;
}

/* unicode_decode_call_errorhandler_writer: calls the handler for
   in->data[start:end], appends the replacement and stores the position to
   resume at in *pnewpos. */
static int qp_cd_decode_error(JSContext *ctx, QPCodecErrors *ce, const char *encoding,
                              const char *reason, QPDecIn *in, Py_ssize_t start,
                              Py_ssize_t end, Py_ssize_t *pnewpos, qp_ucsbuilder *out)
{
    JSValue restuple, rep = JS_UNDEFINED, obj;
    Py_ssize_t newpos, insize;

    if (qp_cd_errors_handler(ctx, ce) < 0)
        return -1;
    if (qp_cd_make_decode_exc(ctx, ce, encoding, in->data, in->len, start, end, reason) < 0)
        return -1;
    restuple = qp_call1(ctx, ce->handler, ce->exc);
    if (JS_IsException(restuple))
        return -1;
    if (qp_cd_handler_result(ctx, restuple, TRUE, &rep, &newpos) < 0) {
        JS_FreeValue(ctx, restuple);
        return -1;
    }
    JS_FreeValue(ctx, restuple);
    /* Copy back the bytes variables, which might have been modified by the
       callback */
    obj = qp_cd_getattr(ctx, ce->exc, "object");
    if (JS_IsException(obj))
        goto fail;
    if (qp_cd_is_none(obj) || !qp_bytes_check(ctx, obj)) {
        qp_raise_type_error(ctx, qp_cd_is_none(obj) ? "UnicodeError 'object' attribute is not set"
                                                    : "UnicodeError 'object' attribute must be a bytes");
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    JS_FreeValue(ctx, in->obj);
    in->obj = obj;
    in->data = qp_bytes_data(obj, &insize);
    in->len = insize;
    if (newpos < 0)
        newpos = insize + newpos;
    if (newpos < 0 || newpos > insize) {
        qp_raise(ctx, QP_EXC(ctx, IndexError), "position %zd from error handler out of bounds", newpos);
        goto fail;
    }
    if (qp_cd_builder_write_str(ctx, out, rep) < 0)
        goto fail;
    JS_FreeValue(ctx, rep);
    *pnewpos = newpos;
    return 0;
fail:
    JS_FreeValue(ctx, rep);
    return -1;
}

/* unicode_encode_call_errorhandler: the replacement (str or bytes) for
   str[start:end] and the position to resume at. */
static JSValue qp_cd_encode_error(JSContext *ctx, QPCodecErrors *ce, const char *encoding,
                                  const char *reason, JSValueConst str, Py_ssize_t len,
                                  Py_ssize_t start, Py_ssize_t end, Py_ssize_t *pnewpos)
{
    JSValue restuple, rep;
    Py_ssize_t newpos;

    if (qp_cd_errors_handler(ctx, ce) < 0)
        return JS_EXCEPTION;
    if (qp_cd_make_encode_exc(ctx, ce, encoding, str, start, end, reason) < 0)
        return JS_EXCEPTION;
    restuple = qp_call1(ctx, ce->handler, ce->exc);
    if (JS_IsException(restuple))
        return JS_EXCEPTION;
    if (qp_cd_handler_result(ctx, restuple, FALSE, &rep, &newpos) < 0) {
        JS_FreeValue(ctx, restuple);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, restuple);
    if (newpos < 0)
        newpos = len + newpos;
    if (newpos < 0 || newpos > len) {
        JS_FreeValue(ctx, rep);
        return qp_raise(ctx, QP_EXC(ctx, IndexError), "position %zd from error handler out of bounds",
                        newpos);
    }
    *pnewpos = newpos;
    return rep;
}

/* Encoder input: exact str + code point view. */
typedef struct QPEncIn {
    JSValue str;
    qp_strview v;
} QPEncIn;

static int qp_cd_encin_init(JSContext *ctx, QPEncIn *in, JSValueConst s)
{
    in->str = qp_str_value(ctx, s);
    if (JS_IsException(in->str))
        return -1;
    if (qp_str_view_acquire(ctx, in->str, &in->v) < 0) {
        JS_FreeValue(ctx, in->str);
        in->str = JS_UNDEFINED;
        return -1;
    }
    return 0;
}

static void qp_cd_encin_free(JSContext *ctx, QPEncIn *in)
{
    qp_str_view_release(ctx, &in->v);
    JS_FreeValue(ctx, in->str);
}

#define QP_CD_READ(in, i) QP_SL_READ((in)->v.kind, (in)->v.data, (i))

/* ========================================================================= */
/* UTF-8                                                                      */
/* ========================================================================= */

/* PyUnicode_DecodeUTF8Stateful (consumed NULL = final) */
static JSValue qp_cd_decode_utf8(JSContext *ctx, const void *s, Py_ssize_t size,
                                 const char *errors, Py_ssize_t *consumed)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    qp_codec_error err;
    Py_ssize_t pos = 0, used, newpos;
    int r;

    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, s, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    for (;;) {
        used = 0;
        r = qp_sl_utf8_decode((const char *)in.data + pos, in.len - pos,
                              consumed ? &used : NULL, &out, &err);
        if (r == QP_SL_NOMEM) {
            qp_raise_memory_error(ctx);
            goto fail;
        }
        if (r != QP_SL_CODEC_ERROR) {
            pos = consumed ? pos + used : in.len;
            break;
        }
        if (qp_cd_decode_error(ctx, &ce, "utf-8", err.reason, &in, pos + err.start,
                               pos + err.end, &newpos, &out) < 0)
            goto fail;
        pos = newpos;
    }
    if (consumed)
        *consumed = pos;
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

static BOOL qp_cd_is_builtin_eh_name(const char *errors)
{
    int i;
    if (!errors)
        return TRUE;
    for (i = 0; i < QP_CEH_COUNT; i++) {
        if (!strcmp(errors, qp_cd_eh_names[i]))
            return TRUE;
    }
    return FALSE;
}

/* Encoders whose CPython implementation (unicode_encode_utf8 /
   unicode_encode_ucs1) handles unencodable runs: limit = 0x110000 (UTF-8:
   surrogates unencodable), 256 or 128.  Custom error handler path; the
   builtin handler names go through qp_str_encode (same results). */
static JSValue qp_cd_encode_runs(JSContext *ctx, JSValueConst s, const char *errors, Py_UCS4 limit)
{
    const char *encoding = limit == 256 ? "latin-1" : limit == 128 ? "ascii" : "utf-8";
    const char *reason = limit == 256 ? "ordinal not in range(256)"
                       : limit == 128 ? "ordinal not in range(128)" : "surrogates not allowed";
    QPCodecErrors ce;
    QPEncIn in;
    DynBuf d;
    Py_ssize_t pos, n, collend, newpos, i;
    JSValue rep;

    if (qp_cd_encin_init(ctx, &in, s) < 0)
        return JS_EXCEPTION;
    qp_cd_errors_init(&ce, errors);
    js_dbuf_init(ctx, &d);
    n = in.v.len;
#define QP_CD_ENCODABLE(ch) (limit > 0x10FFFF ? !QP_CD_IS_SURR(ch) : (ch) < limit)
    for (pos = 0; pos < n;) {
        Py_UCS4 ch = QP_CD_READ(&in, pos);
        if (QP_CD_ENCODABLE(ch)) {
            if (limit > 0x10FFFF) {
                uint8_t buf[4];
                int k;
                if (ch < 0x80) {
                    buf[0] = (uint8_t)ch;
                    k = 1;
                } else if (ch < 0x800) {
                    buf[0] = (uint8_t)(0xc0 | (ch >> 6));
                    buf[1] = (uint8_t)(0x80 | (ch & 0x3f));
                    k = 2;
                } else if (ch < 0x10000) {
                    buf[0] = (uint8_t)(0xe0 | (ch >> 12));
                    buf[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
                    buf[2] = (uint8_t)(0x80 | (ch & 0x3f));
                    k = 3;
                } else {
                    buf[0] = (uint8_t)(0xf0 | (ch >> 18));
                    buf[1] = (uint8_t)(0x80 | ((ch >> 12) & 0x3f));
                    buf[2] = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
                    buf[3] = (uint8_t)(0x80 | (ch & 0x3f));
                    k = 4;
                }
                dbuf_put(&d, buf, k);
            } else {
                dbuf_putc(&d, (uint8_t)ch);
            }
            pos++;
            continue;
        }
        collend = pos + 1;
        while (collend < n && !QP_CD_ENCODABLE(QP_CD_READ(&in, collend)))
            collend++;
        rep = qp_cd_encode_error(ctx, &ce, encoding, reason, in.str, n, pos, collend, &newpos);
        if (JS_IsException(rep))
            goto fail;
        if (qp_bytes_check(ctx, rep)) {
            Py_ssize_t rlen;
            const uint8_t *rd = qp_bytes_data(rep, &rlen);
            dbuf_put(&d, rd, rlen);
        } else {
            qp_strview rv;
            Py_UCS4 rmax = limit > 0x10FFFF ? 128 : limit;
            if (qp_str_view_acquire(ctx, rep, &rv) < 0) {
                JS_FreeValue(ctx, rep);
                goto fail;
            }
            for (i = 0; i < rv.len; i++) {
                if (QP_SL_READ(rv.kind, rv.data, i) >= rmax)
                    break;
            }
            if (i < rv.len) {
                qp_str_view_release(ctx, &rv);
                JS_FreeValue(ctx, rep);
                qp_cd_raise_encode(ctx, &ce, encoding, in.str, pos, collend, reason);
                goto fail;
            }
            for (i = 0; i < rv.len; i++)
                dbuf_putc(&d, (uint8_t)QP_SL_READ(rv.kind, rv.data, i));
            qp_str_view_release(ctx, &rv);
        }
        JS_FreeValue(ctx, rep);
        pos = newpos;
    }
#undef QP_CD_ENCODABLE
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    dbuf_free(&d);
    return JS_EXCEPTION;
}

/* _PyUnicode_AsUTF8String / _PyUnicode_AsLatin1String / _PyUnicode_AsASCIIString */
static JSValue qp_cd_encode_ucs(JSContext *ctx, JSValueConst s, const char *errors, Py_UCS4 limit)
{
    JSValue str, r;
    if (qp_cd_is_builtin_eh_name(errors)) {
        str = qp_str_value(ctx, s);
        if (JS_IsException(str))
            return str;
        r = qp_str_encode(ctx, str, limit == 256 ? "latin-1" : limit == 128 ? "ascii" : "utf-8",
                          errors);
        JS_FreeValue(ctx, str);
        return r;
    }
    return qp_cd_encode_runs(ctx, s, errors, limit);
}

/* ========================================================================= */
/* Latin-1, ASCII                                                             */
/* ========================================================================= */

static JSValue qp_cd_decode_latin1(JSContext *ctx, const void *s, Py_ssize_t size)
{
    return qp_str_from_view(ctx, qp_strview_make(1, s, size));
}

/* PyUnicode_DecodeASCII */
static JSValue qp_cd_decode_ascii(JSContext *ctx, const void *s, Py_ssize_t size, const char *errors)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t pos, newpos;

    for (pos = 0; pos < size; pos++) {
        if (((const uint8_t *)s)[pos] >= 0x80)
            break;
    }
    if (pos == size)
        return qp_str_from_view(ctx, qp_strview_make(1, s, size));
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, s, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    if (qp_ucsbuilder_write(&out, qp_strview_make(1, s, size), 0, pos) < 0) {
        qp_raise_memory_error(ctx);
        goto fail;
    }
    while (pos < in.len) {
        uint8_t c = in.data[pos];
        if (c < 0x80) {
            if (qp_cd_putc(ctx, &out, c) < 0)
                goto fail;
            pos++;
            continue;
        }
        if (qp_cd_decode_error(ctx, &ce, "ascii", "ordinal not in range(128)", &in, pos, pos + 1,
                               &newpos, &out) < 0)
            goto fail;
        pos = newpos;
    }
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* UTF-7                                                                      */
/* ========================================================================= */

#define QP_CD_IS_BASE64(c) \
    (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || \
     ((c) >= '0' && (c) <= '9') || (c) == '+' || (c) == '/')
#define QP_CD_FROM_BASE64(c) \
    (((c) >= 'A' && (c) <= 'Z') ? (c) - 'A' : \
     ((c) >= 'a' && (c) <= 'z') ? (c) - 'a' + 26 : \
     ((c) >= '0' && (c) <= '9') ? (c) - '0' + 52 : \
     (c) == '+' ? 62 : 63)
#define QP_CD_TO_BASE64(n) \
    ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(n) & 0x3f])
#define QP_CD_DECODE_DIRECT(c) ((c) <= 127 && (c) != '+')

static const char qp_cd_utf7_category[128] = {
/* nul soh stx etx eot enq ack bel bs  ht  nl  vt  np  cr  so  si  */
    3,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  3,  3,  2,  3,  3,
/* dle dc1 dc2 dc3 dc4 nak syn etb can em  sub esc fs  gs  rs  us  */
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
/* sp   !   "   #   $   %   &   '   (   )   *   +   ,   -   .   /  */
    2,  1,  1,  1,  1,  1,  1,  0,  0,  0,  1,  3,  0,  0,  0,  0,
/*  0   1   2   3   4   5   6   7   8   9   :   ;   <   =   >   ?  */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  0,
/*  @   A   B   C   D   E   F   G   H   I   J   K   L   M   N   O  */
    1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*  P   Q   R   S   T   U   V   W   X   Y   Z   [   \   ]   ^   _  */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  1,  1,  1,
/*  `   a   b   c   d   e   f   g   h   i   j   k   l   m   n   o  */
    1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*  p   q   r   s   t   u   v   w   x   y   z   {   |   }   ~  del */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  3,  3,
};

#define QP_CD_ENCODE_DIRECT(c) ((c) < 128 && (c) > 0 && (qp_cd_utf7_category[(c)] != 3))

/* PyUnicode_DecodeUTF7Stateful */
static JSValue qp_cd_decode_utf7(JSContext *ctx, const void *data, Py_ssize_t size,
                                 const char *errors, Py_ssize_t *consumed)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t s = 0, startinpos = 0, endinpos, shiftOutStart = 0, newpos;
    const char *errmsg = "";
    int inShift = 0;
    unsigned int base64bits = 0;
    unsigned long base64buffer = 0;
    Py_UCS4 surrogate = 0, ch;

    if (size == 0) {
        if (consumed)
            *consumed = 0;
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);

#define QP_CD_U7_PUT(c) do { \
        if (qp_cd_putc(ctx, &out, (c)) < 0) goto fail; \
    } while (0)

    while (s < in.len) {
      restart:
        ch = in.data[s];
        if (inShift) { /* in a base-64 section */
            if (QP_CD_IS_BASE64(ch)) { /* consume a base-64 character */
                base64buffer = (base64buffer << 6) | QP_CD_FROM_BASE64(ch);
                base64bits += 6;
                s++;
                if (base64bits >= 16) {
                    /* we have enough bits for a UTF-16 value */
                    Py_UCS4 outCh = (Py_UCS4)(base64buffer >> (base64bits - 16));
                    base64bits -= 16;
                    base64buffer &= (1 << base64bits) - 1; /* clear high bits */
                    if (surrogate) {
                        /* expecting a second surrogate */
                        if (QP_CD_IS_LOW_SURR(outCh)) {
                            QP_CD_U7_PUT(QP_CD_JOIN_SURR(surrogate, outCh));
                            surrogate = 0;
                            continue;
                        } else {
                            QP_CD_U7_PUT(surrogate);
                            surrogate = 0;
                        }
                    }
                    if (QP_CD_IS_HIGH_SURR(outCh)) {
                        /* first surrogate */
                        surrogate = outCh;
                    } else {
                        QP_CD_U7_PUT(outCh);
                    }
                }
            } else { /* now leaving a base-64 section */
                inShift = 0;
                if (base64bits > 0) { /* left-over bits */
                    if (base64bits >= 6) {
                        /* We've seen at least one base-64 character */
                        s++;
                        errmsg = "partial character in shift sequence";
                        goto utf7Error;
                    } else {
                        /* Some bits remain; they should be zero */
                        if (base64buffer != 0) {
                            s++;
                            errmsg = "non-zero padding bits in shift sequence";
                            goto utf7Error;
                        }
                    }
                }
                if (surrogate && QP_CD_DECODE_DIRECT(ch))
                    QP_CD_U7_PUT(surrogate);
                surrogate = 0;
                if (ch == '-') {
                    /* '-' is absorbed; other terminating characters are
                       preserved */
                    s++;
                }
            }
        } else if (ch == '+') {
            startinpos = s;
            s++; /* consume '+' */
            if (s < in.len && in.data[s] == '-') { /* '+-' encodes '+' */
                s++;
                QP_CD_U7_PUT('+');
            } else if (s < in.len && !QP_CD_IS_BASE64(in.data[s])) {
                s++;
                errmsg = "ill-formed sequence";
                goto utf7Error;
            } else { /* begin base64-encoded section */
                inShift = 1;
                surrogate = 0;
                shiftOutStart = out.len;
                base64bits = 0;
                base64buffer = 0;
            }
        } else if (QP_CD_DECODE_DIRECT(ch)) { /* character decodes as itself */
            s++;
            QP_CD_U7_PUT(ch);
        } else {
            startinpos = s;
            s++;
            errmsg = "unexpected special character";
            goto utf7Error;
        }
        continue;
utf7Error:
        endinpos = s;
        if (qp_cd_decode_error(ctx, &ce, "utf7", errmsg, &in, startinpos, endinpos, &newpos, &out) < 0)
            goto fail;
        s = newpos;
    }

    /* end of string */
    if (inShift && !consumed) { /* in shift sequence, no more to follow */
        /* if we're in an inconsistent state, that's an error */
        inShift = 0;
        if (surrogate || (base64bits >= 6) || (base64bits > 0 && base64buffer != 0)) {
            endinpos = size;
            if (qp_cd_decode_error(ctx, &ce, "utf7", "unterminated shift sequence", &in, startinpos,
                                   endinpos, &newpos, &out) < 0)
                goto fail;
            s = newpos;
            if (s < in.len)
                goto restart;
        }
    }

    /* return state */
    if (consumed) {
        if (inShift) {
            *consumed = startinpos;
            out.len = shiftOutStart; /* back off output */
        } else {
            *consumed = s;
        }
    }
#undef QP_CD_U7_PUT
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

/* _PyUnicode_EncodeUTF7 */
static JSValue qp_cd_encode_utf7(JSContext *ctx, JSValueConst str)
{
    QPEncIn in;
    DynBuf d;
    Py_ssize_t i, len;
    int inShift = 0;
    unsigned int base64bits = 0;
    unsigned long base64buffer = 0;

    if (qp_cd_encin_init(ctx, &in, str) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    len = in.v.len;
    for (i = 0; i < len; ++i) {
        Py_UCS4 ch = QP_CD_READ(&in, i);
        if (inShift) {
            if (QP_CD_ENCODE_DIRECT(ch)) {
                /* shifting out */
                if (base64bits) { /* output remaining bits */
                    dbuf_putc(&d, QP_CD_TO_BASE64(base64buffer << (6 - base64bits)));
                    base64buffer = 0;
                    base64bits = 0;
                }
                inShift = 0;
                /* Characters not in the BASE64 set implicitly unshift the
                   sequence so no '-' is required, except if the character
                   is itself a '-' */
                if (QP_CD_IS_BASE64(ch) || ch == '-')
                    dbuf_putc(&d, '-');
                dbuf_putc(&d, (uint8_t)ch);
            } else {
                goto encode_char;
            }
        } else { /* not in a shift sequence */
            if (ch == '+') {
                dbuf_putc(&d, '+');
                dbuf_putc(&d, '-');
            } else if (QP_CD_ENCODE_DIRECT(ch)) {
                dbuf_putc(&d, (uint8_t)ch);
            } else {
                dbuf_putc(&d, '+');
                inShift = 1;
                goto encode_char;
            }
        }
        continue;
encode_char:
        if (ch >= 0x10000) {
            /* code first surrogate */
            base64bits += 16;
            base64buffer = (base64buffer << 16) | (0xD800 | ((ch - 0x10000) >> 10));
            while (base64bits >= 6) {
                dbuf_putc(&d, QP_CD_TO_BASE64(base64buffer >> (base64bits - 6)));
                base64bits -= 6;
            }
            /* prepare second surrogate */
            ch = 0xDC00 | ((ch - 0x10000) & 0x3FF);
        }
        base64bits += 16;
        base64buffer = (base64buffer << 16) | ch;
        while (base64bits >= 6) {
            dbuf_putc(&d, QP_CD_TO_BASE64(base64buffer >> (base64bits - 6)));
            base64bits -= 6;
        }
    }
    if (base64bits)
        dbuf_putc(&d, QP_CD_TO_BASE64(base64buffer << (6 - base64bits)));
    if (inShift)
        dbuf_putc(&d, '-');
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
}

/* ========================================================================= */
/* UTF-16 / UTF-32                                                            */
/* ========================================================================= */

/* PyUnicode_DecodeUTF16Stateful (little-endian host order: bo <= 0 is LE) */
static JSValue qp_cd_decode_utf16(JSContext *ctx, const void *data, Py_ssize_t size,
                                  const char *errors, int *byteorder, Py_ssize_t *consumed)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t q = 0, startinpos, endinpos, newpos;
    int bo = 0, ihi, ilo;
    const char *errmsg, *encoding;
    const uint8_t *p = data;

    if (byteorder)
        bo = *byteorder;
    /* Check for BOM marks (U+FEFF) in the input and adjust current byte
       order setting accordingly.  In native mode, the leading BOM mark is
       skipped, in all other modes, it is copied to the output stream as-is
       (giving a ZWNBSP character). */
    if (bo == 0 && size >= 2) {
        const Py_UCS4 bom = (p[1] << 8) | p[0];
        if (bom == 0xFEFF) {
            q += 2;
            bo = -1;
        } else if (bom == 0xFFFE) {
            q += 2;
            bo = 1;
        }
        if (byteorder)
            *byteorder = bo;
    }
    if (q == size) {
        if (consumed)
            *consumed = size;
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    /* native_ordering on a little-endian host */
    if (bo <= 0) {
        ihi = 1, ilo = 0;
        encoding = "utf-16-le";
    } else {
        ihi = 0, ilo = 1;
        encoding = "utf-16-be";
    }
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    for (;;) {
        Py_UCS4 ch, ch2;
        if (in.len - q < 2) {
            /* remaining byte at the end? (size should be even) */
            if (q == in.len || consumed)
                break;
            errmsg = "truncated data";
            startinpos = q;
            endinpos = in.len;
            goto error;
        }
        ch = (in.data[q + ihi] << 8) | in.data[q + ilo];
        q += 2;
        if (!QP_CD_IS_SURR(ch)) {
            if (qp_cd_putc(ctx, &out, ch) < 0)
                goto fail;
            continue;
        }
        /* UTF-16 code pair: */
        if (!QP_CD_IS_HIGH_SURR(ch)) {
            errmsg = "illegal encoding";
            startinpos = q - 2;
            endinpos = startinpos + 2;
            goto error;
        }
        if (in.len - q < 2) {
            q -= 2;
            if (consumed)
                break;
            errmsg = "unexpected end of data";
            startinpos = q;
            endinpos = in.len;
            goto error;
        }
        ch2 = (in.data[q + ihi] << 8) | in.data[q + ilo];
        q += 2;
        if (!QP_CD_IS_LOW_SURR(ch2)) {
            errmsg = "illegal UTF-16 surrogate";
            startinpos = q - 4;
            endinpos = startinpos + 2;
            goto error;
        }
        if (qp_cd_putc(ctx, &out, QP_CD_JOIN_SURR(ch, ch2)) < 0)
            goto fail;
        continue;
      error:
        /* The remaining input chars are ignored if the callback chooses to
           skip the input */
        if (qp_cd_decode_error(ctx, &ce, encoding, errmsg, &in, startinpos, endinpos, &newpos,
                               &out) < 0)
            goto fail;
        q = newpos;
    }
    if (consumed)
        *consumed = q;
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

/* PyUnicode_DecodeUTF32Stateful */
static JSValue qp_cd_decode_utf32(JSContext *ctx, const void *data, Py_ssize_t size,
                                  const char *errors, int *byteorder, Py_ssize_t *consumed)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t q = 0, startinpos, endinpos, newpos;
    int bo = 0, le;
    const char *errmsg, *encoding;
    const uint8_t *p = data;

    if (byteorder)
        bo = *byteorder;
    if (bo == 0 && size >= 4) {
        Py_UCS4 bom = ((Py_UCS4)p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        if (bom == 0x0000FEFF) {
            bo = -1;
            q += 4;
        } else if (bom == 0xFFFE0000) {
            bo = 1;
            q += 4;
        }
        if (byteorder)
            *byteorder = bo;
    }
    if (q == size) {
        if (consumed)
            *consumed = size;
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    le = bo <= 0;
    encoding = le ? "utf-32-le" : "utf-32-be";
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    for (;;) {
        Py_UCS4 ch;
        const uint8_t *b;
        if (in.len - q < 4) {
            if (q == in.len || consumed)
                break;
            /* remaining bytes at the end? (size should be divisible by 4) */
            errmsg = "truncated data";
            startinpos = q;
            endinpos = in.len;
            goto error;
        }
        b = in.data + q;
        if (le)
            ch = ((Py_UCS4)b[3] << 24) | (b[2] << 16) | (b[1] << 8) | b[0];
        else
            ch = ((Py_UCS4)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        if (QP_CD_IS_SURR(ch)) {
            errmsg = "code point in surrogate code point range(0xd800, 0xe000)";
            startinpos = q;
            endinpos = startinpos + 4;
            goto error;
        }
        if (ch >= 0x110000) {
            errmsg = "code point not in range(0x110000)";
            startinpos = q;
            endinpos = startinpos + 4;
            goto error;
        }
        if (qp_cd_putc(ctx, &out, ch) < 0)
            goto fail;
        q += 4;
        continue;
      error:
        if (qp_cd_decode_error(ctx, &ce, encoding, errmsg, &in, startinpos, endinpos, &newpos,
                               &out) < 0)
            goto fail;
        q = newpos;
    }
    if (consumed)
        *consumed = q;
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

static void qp_cd_put_unit(DynBuf *d, uint32_t u, int width, int native_le)
{
    uint8_t b[4];
    int i;
    for (i = 0; i < width; i++) {
        int shift = native_le ? 8 * i : 8 * (width - 1 - i);
        b[i] = (uint8_t)(u >> shift);
    }
    dbuf_put(d, b, width);
}

/* _PyUnicode_EncodeUTF16 (width 2) / _PyUnicode_EncodeUTF32 (width 4):
   byteorder -1 little, 1 big, 0 native with a BOM. */
static JSValue qp_cd_encode_utf16_32(JSContext *ctx, JSValueConst s, const char *errors,
                                     int byteorder, int width)
{
    QPCodecErrors ce;
    QPEncIn in;
    DynBuf d;
    Py_ssize_t pos, len, newpos, i;
    int le = byteorder <= 0;
    const char *encoding;
    JSValue rep;

    if (width == 2)
        encoding = byteorder == -1 ? "utf-16-le" : byteorder == 1 ? "utf-16-be" : "utf-16";
    else
        encoding = byteorder == -1 ? "utf-32-le" : byteorder == 1 ? "utf-32-be" : "utf-32";
    if (qp_cd_encin_init(ctx, &in, s) < 0)
        return JS_EXCEPTION;
    qp_cd_errors_init(&ce, errors);
    js_dbuf_init(ctx, &d);
    if (byteorder == 0)
        qp_cd_put_unit(&d, 0xFEFF, width, le);
    len = in.v.len;
    for (pos = 0; pos < len;) {
        Py_UCS4 ch = QP_CD_READ(&in, pos);
        if (!QP_CD_IS_SURR(ch)) {
            if (width == 2 && ch >= 0x10000) {
                qp_cd_put_unit(&d, 0xD800 | ((ch - 0x10000) >> 10), 2, le);
                qp_cd_put_unit(&d, 0xDC00 | ((ch - 0x10000) & 0x3FF), 2, le);
            } else {
                qp_cd_put_unit(&d, ch, width, le);
            }
            pos++;
            continue;
        }
        rep = qp_cd_encode_error(ctx, &ce, encoding, "surrogates not allowed", in.str, len, pos,
                                 pos + 1, &newpos);
        if (JS_IsException(rep))
            goto fail;
        if (qp_bytes_check(ctx, rep)) {
            Py_ssize_t rlen;
            const uint8_t *rd = qp_bytes_data(rep, &rlen);
            if (rlen & (width - 1)) {
                JS_FreeValue(ctx, rep);
                qp_cd_raise_encode(ctx, &ce, encoding, in.str, pos, pos + 1, "surrogates not allowed");
                goto fail;
            }
            dbuf_put(&d, rd, rlen);
        } else {
            qp_strview rv;
            if (qp_str_view_acquire(ctx, rep, &rv) < 0) {
                JS_FreeValue(ctx, rep);
                goto fail;
            }
            for (i = 0; i < rv.len; i++) {
                if (QP_SL_READ(rv.kind, rv.data, i) >= 128)
                    break;
            }
            if (i < rv.len) {
                qp_str_view_release(ctx, &rv);
                JS_FreeValue(ctx, rep);
                qp_cd_raise_encode(ctx, &ce, encoding, in.str, pos, pos + 1, "surrogates not allowed");
                goto fail;
            }
            for (i = 0; i < rv.len; i++)
                qp_cd_put_unit(&d, QP_SL_READ(rv.kind, rv.data, i), width, le);
            qp_str_view_release(ctx, &rv);
        }
        JS_FreeValue(ctx, rep);
        pos = newpos;
    }
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    dbuf_free(&d);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* unicode-escape, raw-unicode-escape                                         */
/* ========================================================================= */

static int qp_cd_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - ('a' - 10);
    if (c >= 'A' && c <= 'F')
        return c - ('A' - 10);
    return -1;
}

/* _PyUnicode_DecodeUnicodeEscapeInternal2 */
static JSValue qp_cd_decode_unicode_escape_internal(JSContext *ctx, const void *data,
                                                    Py_ssize_t size, const char *errors,
                                                    Py_ssize_t *consumed,
                                                    int *first_invalid_escape_char)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t s = 0, startinpos, endinpos, newpos;

    *first_invalid_escape_char = -1;
    if (size == 0) {
        if (consumed)
            *consumed = 0;
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    while (s < in.len) {
        unsigned char c = in.data[s++];
        Py_UCS4 ch;
        int count, v;
        const char *message;

        /* Non-escape characters are interpreted as Unicode ordinals */
        if (c != '\\') {
            if (qp_cd_putc(ctx, &out, c) < 0)
                goto fail;
            continue;
        }
        startinpos = s - 1;
        /* \ - Escapes */
        if (s >= in.len) {
            message = "\\ at end of string";
            goto incomplete;
        }
        c = in.data[s++];
        switch (c) {
        /* \x escapes */
        case '\n': continue;
        case '\\': ch = '\\'; goto write_char;
        case '\'': ch = '\''; goto write_char;
        case '\"': ch = '\"'; goto write_char;
        case 'b': ch = '\b'; goto write_char;
        case 'f': ch = '\014'; goto write_char; /* FF */
        case 't': ch = '\t'; goto write_char;
        case 'n': ch = '\n'; goto write_char;
        case 'r': ch = '\r'; goto write_char;
        case 'v': ch = '\013'; goto write_char; /* VT */
        case 'a': ch = '\007'; goto write_char; /* BEL, not classic C */

        /* \OOO (octal) escapes */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
            ch = c - '0';
            if (s < in.len && '0' <= in.data[s] && in.data[s] <= '7') {
                ch = (ch << 3) + in.data[s++] - '0';
                if (s < in.len && '0' <= in.data[s] && in.data[s] <= '7')
                    ch = (ch << 3) + in.data[s++] - '0';
            }
            if (ch > 0377) {
                if (*first_invalid_escape_char == -1)
                    *first_invalid_escape_char = ch;
            }
            goto write_char;

        /* hex escapes */
        case 'x':
            count = 2;
            message = "truncated \\xXX escape";
            goto hexescape;
        case 'u':
            count = 4;
            message = "truncated \\uXXXX escape";
            goto hexescape;
        case 'U':
            count = 8;
            message = "truncated \\UXXXXXXXX escape";
        hexescape:
            for (ch = 0; count; ++s, --count) {
                if (s >= in.len)
                    goto incomplete;
                v = qp_cd_hexval(in.data[s]);
                if (v < 0)
                    goto error;
                ch = (ch << 4) + v;
            }
            /* when we get here, ch is a 32-bit unicode character */
            if (ch > QP_CD_MAX_UNICODE) {
                message = "illegal Unicode character";
                goto error;
            }
            goto write_char;

        /* \N{name} */
        case 'N':
            if (qp_unicode_name_capi(ctx) < 0) {
                JS_FreeValue(ctx, qp_exc_fetch(ctx));
                qp_raise(ctx, QP_EXC(ctx, UnicodeError),
                         "\\N escapes not supported (can't load unicodedata module)");
                goto fail;
            }
            message = "malformed \\N character escape";
            if (s >= in.len)
                goto incomplete;
            if (in.data[s] == '{') {
                Py_ssize_t start = ++s;
                size_t namelen;
                /* look for the closing brace */
                while (s < in.len && in.data[s] != '}')
                    s++;
                if (s >= in.len)
                    goto incomplete;
                namelen = s - start;
                if (namelen) {
                    /* found a name.  look it up in the unicode database */
                    char name[512];
                    uint32_t cp;
                    s++;
                    if (namelen < sizeof(name)) {
                        memcpy(name, in.data + start, namelen);
                        name[namelen] = '\0';
                        if (qp_unicode_lookup_name(name, namelen, &cp)) {
                            ch = cp;
                            goto write_char;
                        }
                    }
                    message = "unknown Unicode character name";
                }
            }
            goto error;

        default:
            if (*first_invalid_escape_char == -1)
                *first_invalid_escape_char = c;
            if (qp_cd_putc(ctx, &out, '\\') < 0)
                goto fail;
            ch = c;
            goto write_char;
        }
      write_char:
        if (qp_cd_putc(ctx, &out, ch) < 0)
            goto fail;
        continue;

      incomplete:
        if (consumed) {
            *consumed = startinpos;
            break;
        }
      error:
        endinpos = s;
        if (qp_cd_decode_error(ctx, &ce, "unicodeescape", message, &in, startinpos, endinpos,
                               &newpos, &out) < 0)
            goto fail;
        s = newpos;
    }
    /* *consumed keeps the caller's value (the input size) unless the
       input ended inside an escape */
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

/* _PyUnicode_DecodeUnicodeEscapeStateful: DeprecationWarning for invalid
   escapes. */
static JSValue qp_cd_decode_unicode_escape(JSContext *ctx, const void *data, Py_ssize_t size,
                                           const char *errors, Py_ssize_t *consumed)
{
    int first_invalid_escape_char;
    JSValue result = qp_cd_decode_unicode_escape_internal(ctx, data, size, errors, consumed,
                                                          &first_invalid_escape_char);
    if (JS_IsException(result))
        return result;
    if (first_invalid_escape_char != -1) {
        int r;
        if (first_invalid_escape_char > 0xff)
            r = qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "\"\\%o\" is an invalid octal escape sequence. "
                        "Such sequences will not work in the future. ", first_invalid_escape_char);
        else
            r = qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "\"\\%c\" is an invalid escape sequence. "
                        "Such sequences will not work in the future. ", first_invalid_escape_char);
        if (r < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

/* PyUnicode_AsUnicodeEscapeString */
static JSValue qp_cd_encode_unicode_escape(JSContext *ctx, JSValueConst str)
{
    QPEncIn in;
    DynBuf d;
    Py_ssize_t i;

    if (qp_cd_encin_init(ctx, &in, str) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    for (i = 0; i < in.v.len; i++) {
        Py_UCS4 ch = QP_CD_READ(&in, i);
        /* U+0000-U+00ff range */
        if (ch < 0x100) {
            if (ch >= ' ' && ch < 127) {
                if (ch != '\\') {
                    /* Copy printable US ASCII as-is */
                    dbuf_putc(&d, (uint8_t)ch);
                } else {
                    /* Escape backslashes */
                    dbuf_putc(&d, '\\');
                    dbuf_putc(&d, '\\');
                }
            } else if (ch == '\t') {
                /* Map special whitespace to '\t', \n', '\r' */
                dbuf_putc(&d, '\\');
                dbuf_putc(&d, 't');
            } else if (ch == '\n') {
                dbuf_putc(&d, '\\');
                dbuf_putc(&d, 'n');
            } else if (ch == '\r') {
                dbuf_putc(&d, '\\');
                dbuf_putc(&d, 'r');
            } else {
                /* Map non-printable US ASCII and 8-bit characters to '\xHH' */
                dbuf_putc(&d, '\\');
                dbuf_putc(&d, 'x');
                dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0x000F]);
                dbuf_putc(&d, qp_cd_hexdigits[ch & 0x000F]);
            }
        } else if (ch < 0x10000) {
            /* U+0100-U+ffff range: Map 16-bit characters to '\uHHHH' */
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'u');
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 12) & 0x000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 8) & 0x000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0x000F]);
            dbuf_putc(&d, qp_cd_hexdigits[ch & 0x000F]);
        } else {
            /* U+010000-U+10ffff range: Map 21-bit characters to '\U00HHHHHH' */
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'U');
            dbuf_putc(&d, '0');
            dbuf_putc(&d, '0');
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 20) & 0x0000000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 16) & 0x0000000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 12) & 0x0000000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 8) & 0x0000000F]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0x0000000F]);
            dbuf_putc(&d, qp_cd_hexdigits[ch & 0x0000000F]);
        }
    }
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
}

/* _PyUnicode_DecodeRawUnicodeEscapeStateful */
static JSValue qp_cd_decode_raw_unicode_escape(JSContext *ctx, const void *data, Py_ssize_t size,
                                               const char *errors, Py_ssize_t *consumed)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t s = 0, startinpos, endinpos, newpos;

    if (size == 0) {
        if (consumed)
            *consumed = 0;
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    while (s < in.len) {
        unsigned char c = in.data[s++];
        Py_UCS4 ch;
        int count, v;
        const char *message;

        /* Non-escape characters are interpreted as Unicode ordinals */
        if (c != '\\' || (s >= in.len && !consumed)) {
            if (qp_cd_putc(ctx, &out, c) < 0)
                goto fail;
            continue;
        }
        startinpos = s - 1;
        /* \ - Escapes */
        if (s >= in.len) {
            message = "\\ at end of string";
            goto incomplete;
        }
        c = in.data[s++];
        if (c == 'u') {
            count = 4;
            message = "truncated \\uXXXX escape";
        } else if (c == 'U') {
            count = 8;
            message = "truncated \\UXXXXXXXX escape";
        } else {
            if (qp_cd_putc(ctx, &out, '\\') < 0 || qp_cd_putc(ctx, &out, c) < 0)
                goto fail;
            continue;
        }
        /* \uHHHH with 4 hex digits, \U00HHHHHH with 8 */
        for (ch = 0; count; ++s, --count) {
            if (s >= in.len)
                goto incomplete;
            v = qp_cd_hexval(in.data[s]);
            if (v < 0)
                goto error;
            ch = (ch << 4) + v;
        }
        if (ch > QP_CD_MAX_UNICODE) {
            message = "\\Uxxxxxxxx out of range";
            goto error;
        }
        if (qp_cd_putc(ctx, &out, ch) < 0)
            goto fail;
        continue;

      incomplete:
        if (consumed) {
            *consumed = startinpos;
            break;
        }
      error:
        endinpos = s;
        if (qp_cd_decode_error(ctx, &ce, "rawunicodeescape", message, &in, startinpos, endinpos,
                               &newpos, &out) < 0)
            goto fail;
        s = newpos;
    }
    /* *consumed keeps the caller's value unless the input ended inside an
       escape */
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

/* PyUnicode_AsRawUnicodeEscapeString */
static JSValue qp_cd_encode_raw_unicode_escape(JSContext *ctx, JSValueConst str)
{
    QPEncIn in;
    DynBuf d;
    Py_ssize_t i;

    if (qp_cd_encin_init(ctx, &in, str) < 0)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &d);
    for (i = 0; i < in.v.len; i++) {
        Py_UCS4 ch = QP_CD_READ(&in, i);
        if (ch < 0x100) {
            /* U+0000-U+00ff range: Copy 8-bit characters as-is */
            dbuf_putc(&d, (uint8_t)ch);
        } else if (ch < 0x10000) {
            /* U+0100-U+ffff range: Map 16-bit characters to '\uHHHH' */
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'u');
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 12) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 8) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[ch & 15]);
        } else {
            /* U+010000-U+10ffff range: Map 32-bit characters to '\U00HHHHHH' */
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'U');
            dbuf_putc(&d, '0');
            dbuf_putc(&d, '0');
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 20) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 16) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 12) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 8) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[(ch >> 4) & 0xf]);
            dbuf_putc(&d, qp_cd_hexdigits[ch & 15]);
        }
    }
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
}

/* ========================================================================= */
/* bytes escape codec (_PyBytes_DecodeEscape2, _codecs.escape_encode)         */
/* ========================================================================= */

static JSValue qp_cd_decode_escape_bytes(JSContext *ctx, const uint8_t *s, Py_ssize_t len,
                                         const char *errors)
{
    DynBuf d;
    Py_ssize_t i = 0;
    int first_invalid_escape_char = -1;
    JSValue result;

    js_dbuf_init(ctx, &d);
    while (i < len) {
        int c;
        if (s[i] != '\\') {
            dbuf_putc(&d, s[i++]);
            continue;
        }
        i++;
        if (i == len) {
            dbuf_free(&d);
            return qp_raise_value_error(ctx, "Trailing \\ in string");
        }
        switch (s[i++]) {
        /* XXX This assumes ASCII! */
        case '\n': break;
        case '\\': dbuf_putc(&d, '\\'); break;
        case '\'': dbuf_putc(&d, '\''); break;
        case '\"': dbuf_putc(&d, '\"'); break;
        case 'b': dbuf_putc(&d, '\b'); break;
        case 'f': dbuf_putc(&d, '\014'); break; /* FF */
        case 't': dbuf_putc(&d, '\t'); break;
        case 'n': dbuf_putc(&d, '\n'); break;
        case 'r': dbuf_putc(&d, '\r'); break;
        case 'v': dbuf_putc(&d, '\013'); break; /* VT */
        case 'a': dbuf_putc(&d, '\007'); break; /* BEL, not classic C */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
            c = s[i - 1] - '0';
            if (i < len && '0' <= s[i] && s[i] <= '7') {
                c = (c << 3) + s[i++] - '0';
                if (i < len && '0' <= s[i] && s[i] <= '7')
                    c = (c << 3) + s[i++] - '0';
            }
            if (c > 0377) {
                if (first_invalid_escape_char == -1)
                    first_invalid_escape_char = c;
            }
            dbuf_putc(&d, (uint8_t)c);
            break;
        case 'x':
            if (i + 1 < len) {
                int digit1 = qp_cd_hexval(s[i]), digit2 = qp_cd_hexval(s[i + 1]);
                if (digit1 >= 0 && digit2 >= 0) {
                    dbuf_putc(&d, (uint8_t)((digit1 << 4) + digit2));
                    i += 2;
                    break;
                }
            }
            /* invalid hexadecimal digits */
            if (!errors || strcmp(errors, "strict") == 0) {
                dbuf_free(&d);
                return qp_raise_value_error(ctx, "invalid \\x escape at position %zd", i - 2);
            }
            if (strcmp(errors, "replace") == 0) {
                dbuf_putc(&d, '?');
            } else if (strcmp(errors, "ignore") == 0) {
                /* do nothing */
            } else {
                dbuf_free(&d);
                return qp_raise_value_error(ctx, "decoding error; unknown error handling code: %.400s",
                                            errors);
            }
            /* skip \x */
            if (i < len && qp_cd_hexval(s[i]) >= 0)
                i++; /* and a hexdigit */
            break;
        default:
            if (first_invalid_escape_char == -1)
                first_invalid_escape_char = s[i - 1];
            dbuf_putc(&d, '\\');
            i--;
        }
    }
    result = qp_cd_bytes_from_dbuf(ctx, &d);
    if (JS_IsException(result))
        return result;
    if (first_invalid_escape_char != -1) {
        int r;
        if (first_invalid_escape_char > 0xff)
            r = qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "b\"\\%o\" is an invalid octal escape sequence. "
                        "Such sequences will not work in the future. ", first_invalid_escape_char);
        else
            r = qp_warn(ctx, QP_EXC(ctx, DeprecationWarning), 1,
                        "b\"\\%c\" is an invalid escape sequence. "
                        "Such sequences will not work in the future. ", first_invalid_escape_char);
        if (r < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
    }
    return result;
}

/* ========================================================================= */
/* charmap                                                                    */
/* ========================================================================= */

/* EncodingMap (PyUnicode_BuildEncodingMap's three-level trie).  The trie
   lives in instance slot 0 as bytes: level1[32] | count2 | count3 |
   level2[16*count2] | level3[128*count3]. */
#define QP_CD_EMAP_NSLOTS 1

static JSValue qp_cd_emap_size(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValue *slots = qp_instance_slots(self);
    Py_ssize_t len;
    const uint8_t *t;
    if (!slots || QP_IS_NULL(slots[0]))
        return qp_long_from_int64(ctx, 63);
    t = qp_bytes_data(slots[0], &len);
    /* (sizeof(struct encoding_map) - 1) + 16*count2 + 128*count3 */
    return qp_long_from_int64(ctx, 63 + 16 * t[32] + 128 * t[33]);
}

static const QPMethodDef qp_cd_emap_methods[] = {
    { "size", qp_cd_emap_size, QP_METH_NOARGS, 0,
      "size($self, /)\n--\n\nReturn the size (in bytes) of this object." },
    { NULL },
};

static const QPTypeSpec qp_cd_emap_spec = {
    .name = "EncodingMap",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_CD_EMAP_NSLOTS,
    .methods = qp_cd_emap_methods,
};

static QPType *qp_cd_emap_type(JSContext *ctx)
{
    QPModState *ms = qp_codec_state(ctx);
    JSValue t;
    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->codec_encoding_map_type)) {
        t = qp_type_from_spec(ctx, &qp_cd_emap_spec);
        if (JS_IsException(t))
            return NULL;
        ms->codec_encoding_map_type = t;
    }
    return qp_type_from_value(ms->codec_encoding_map_type);
}

/* Trie data of an EncodingMap instance, NULL if mapping is not one. */
static const uint8_t *qp_cd_emap_data(JSContext *ctx, JSValueConst mapping)
{
    QPModState *ms = QP_STATE(ctx)->mod_state;
    JSValue *slots;
    Py_ssize_t len;
    if (!ms || QP_IS_NULL(ms->codec_encoding_map_type) ||
        qp_type_of(ctx, mapping) != qp_type_from_value(ms->codec_encoding_map_type))
        return NULL;
    slots = qp_instance_slots(mapping);
    if (!slots || QP_IS_NULL(slots[0]))
        return NULL;
    return qp_bytes_data(slots[0], &len);
}

/* encoding_map_lookup: byte value or -1 */
static int qp_cd_emap_lookup(Py_UCS4 c, const uint8_t *map)
{
    int l1 = c >> 11, l2 = (c >> 7) & 0xF, l3 = c & 0x7F, i;
    const uint8_t *level23 = map + 34;
    if (c > 0xFFFF)
        return -1;
    if (c == 0)
        return 0;
    i = map[l1];
    if (i == 0xFF)
        return -1;
    i = level23[16 * i + l2];
    if (i == 0xFF)
        return -1;
    i = level23[16 * map[32] + 128 * i + l3];
    if (i == 0)
        return -1;
    return i;
}

/* PyUnicode_BuildEncodingMap */
static JSValue qp_cd_build_encoding_map(JSContext *ctx, JSValueConst string)
{
    QPEncIn in;
    int i, need_dict = 0, length, count2 = 0, count3 = 0;
    uint8_t level1[32], level2[512], *buf;
    Py_UCS4 ch;
    QPType *t;
    JSValue result, trie;
    size_t size;

    if (qp_cd_encin_init(ctx, &in, string) < 0)
        return JS_EXCEPTION;
    if (in.v.len == 0) {
        qp_cd_encin_free(ctx, &in);
        return qp_raise_type_error(ctx, "bad argument type for built-in operation");
    }
    length = (int)(in.v.len < 256 ? in.v.len : 256);
    memset(level1, 0xFF, sizeof level1);
    memset(level2, 0xFF, sizeof level2);
    /* If there isn't a one-to-one mapping of NULL to \0, or if there are
       non-BMP characters, we need to use a mapping dictionary. */
    if (QP_CD_READ(&in, 0) != 0)
        need_dict = 1;
    for (i = 1; i < length; i++) {
        int l1, l2;
        ch = QP_CD_READ(&in, i);
        if (ch == 0 || ch > 0xFFFF) {
            need_dict = 1;
            break;
        }
        if (ch == 0xFFFE)
            /* unmapped character */
            continue;
        l1 = ch >> 11;
        l2 = ch >> 7;
        if (level1[l1] == 0xFF)
            level1[l1] = count2++;
        if (level2[l2] == 0xFF)
            level2[l2] = count3++;
    }
    if (count2 >= 0xFF || count3 >= 0xFF)
        need_dict = 1;

    if (need_dict) {
        result = qp_dict_new(ctx);
        if (JS_IsException(result))
            goto fail;
        for (i = 0; i < length; i++) {
            JSValue key = qp_long_from_int64(ctx, QP_CD_READ(&in, i));
            JSValue value = qp_long_from_int64(ctx, i);
            int r = -1;
            if (!JS_IsException(key) && !JS_IsException(value))
                r = qp_dict_set_item(ctx, result, key, value);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, value);
            if (r < 0) {
                JS_FreeValue(ctx, result);
                goto fail;
            }
        }
        qp_cd_encin_free(ctx, &in);
        return result;
    }

    /* Create a three-level trie */
    size = 34 + 16 * count2 + 128 * count3;
    buf = js_malloc(ctx, size);
    if (!buf)
        goto fail;
    {
        uint8_t *mlevel1 = buf, *mlevel2 = buf + 34, *mlevel3 = buf + 34 + 16 * count2;
        memcpy(mlevel1, level1, 32);
        buf[32] = (uint8_t)count2;
        buf[33] = (uint8_t)count3;
        memset(mlevel2, 0xFF, 16 * count2);
        memset(mlevel3, 0, 128 * count3);
        count3 = 0;
        for (i = 1; i < length; i++) {
            int o1, o2, o3, i2, i3;
            ch = QP_CD_READ(&in, i);
            if (ch == 0xFFFE)
                /* unmapped character */
                continue;
            o1 = ch >> 11;
            o2 = (ch >> 7) & 0xF;
            i2 = 16 * mlevel1[o1] + o2;
            if (mlevel2[i2] == 0xFF)
                mlevel2[i2] = count3++;
            o3 = ch & 0x7F;
            i3 = 128 * mlevel2[i2] + o3;
            mlevel3[i3] = (uint8_t)i;
        }
    }
    trie = qp_bytes_new(ctx, buf, size);
    js_free(ctx, buf);
    qp_cd_encin_free(ctx, &in);
    if (JS_IsException(trie))
        return trie;
    t = qp_cd_emap_type(ctx);
    if (!t) {
        JS_FreeValue(ctx, trie);
        return JS_EXCEPTION;
    }
    result = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, trie);
        return result;
    }
    qp_instance_slots(result)[0] = trie;
    return result;
fail:
    qp_cd_encin_free(ctx, &in);
    return JS_EXCEPTION;
}

/* PyUnicode_DecodeCharmap (mapping QP_NULL = Latin-1) */
static JSValue qp_cd_decode_charmap(JSContext *ctx, const void *data, Py_ssize_t size,
                                    JSValueConst mapping, const char *errors)
{
    QPCodecErrors ce;
    QPDecIn in;
    qp_ucsbuilder out;
    Py_ssize_t s = 0, newpos;
    JSValue item = JS_UNDEFINED;

    if (QP_IS_NULL(mapping))
        return qp_cd_decode_latin1(ctx, data, size);
    if (size == 0)
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    qp_cd_errors_init(&ce, errors);
    qp_cd_decin_init(&in, data, size);
    qp_ucsbuilder_init(&out, ctx->rt);
    if (qp_str_check_exact(mapping)) {
        /* charmap_decode_string */
        qp_strview mv;
        if (qp_str_view_acquire(ctx, mapping, &mv) < 0)
            goto fail;
        while (s < in.len) {
            unsigned char ch = in.data[s];
            Py_UCS4 x = ch < mv.len ? QP_SL_READ(mv.kind, mv.data, ch) : 0xfffe;
            if (x == 0xfffe) {
                /* undefined mapping */
                if (qp_cd_decode_error(ctx, &ce, "charmap", "character maps to <undefined>", &in,
                                       s, s + 1, &newpos, &out) < 0) {
                    qp_str_view_release(ctx, &mv);
                    goto fail;
                }
                s = newpos;
                continue;
            }
            if (qp_cd_putc(ctx, &out, x) < 0) {
                qp_str_view_release(ctx, &mv);
                goto fail;
            }
            ++s;
        }
        qp_str_view_release(ctx, &mv);
    } else {
        /* charmap_decode_mapping */
        while (s < in.len) {
            unsigned char ch = in.data[s];
            JSValue key = qp_long_from_int64(ctx, ch);
            if (JS_IsException(key))
                goto fail;
            item = qp_getitem(ctx, mapping, key);
            JS_FreeValue(ctx, key);
            if (JS_IsException(item)) {
                item = JS_UNDEFINED;
                if (qp_exc_pending_matches(ctx, QP_EXC(ctx, LookupError))) {
                    /* No mapping found means: mapping is undefined. */
                    JS_FreeValue(ctx, qp_exc_fetch(ctx));
                    goto undefined;
                }
                goto fail;
            }
            /* Apply mapping */
            if (qp_cd_is_none(item))
                goto undefined;
            if (qp_long_check(ctx, item)) {
                int64_t value;
                int overflow;
                JSValue iv = qp_long_value(ctx, item);
                if (JS_IsException(iv))
                    goto fail;
                qp_long_as_int64_overflow(iv, &value, &overflow);
                JS_FreeValue(ctx, iv);
                if (!overflow && value == 0xFFFE)
                    goto undefined;
                if (overflow || value < 0 || value > QP_CD_MAX_UNICODE) {
                    qp_raise_type_error(ctx, "character mapping must be in range(0x%lx)",
                                        (unsigned long)QP_CD_MAX_UNICODE + 1);
                    goto fail;
                }
                if (qp_cd_putc(ctx, &out, (Py_UCS4)value) < 0)
                    goto fail;
            } else if (qp_str_check(ctx, item)) {
                JSValue sv = qp_str_value(ctx, item);
                qp_strview iv;
                if (JS_IsException(sv))
                    goto fail;
                if (qp_str_view_acquire(ctx, sv, &iv) < 0) {
                    JS_FreeValue(ctx, sv);
                    goto fail;
                }
                if (iv.len == 1 && QP_SL_READ(iv.kind, iv.data, 0) == 0xFFFE) {
                    qp_str_view_release(ctx, &iv);
                    JS_FreeValue(ctx, sv);
                    goto undefined;
                }
                if (qp_ucsbuilder_write(&out, iv, 0, iv.len) < 0) {
                    qp_str_view_release(ctx, &iv);
                    JS_FreeValue(ctx, sv);
                    qp_raise_memory_error(ctx);
                    goto fail;
                }
                qp_str_view_release(ctx, &iv);
                JS_FreeValue(ctx, sv);
            } else {
                /* wrong return value */
                qp_raise_type_error(ctx, "character mapping must return integer, None or str");
                goto fail;
            }
            JS_FreeValue(ctx, item);
            item = JS_UNDEFINED;
            ++s;
            continue;
          undefined:
            /* undefined mapping */
            JS_FreeValue(ctx, item);
            item = JS_UNDEFINED;
            if (qp_cd_decode_error(ctx, &ce, "charmap", "character maps to <undefined>", &in, s,
                                   s + 1, &newpos, &out) < 0)
                goto fail;
            s = newpos;
        }
    }
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    return qp_cd_str_from_builder(ctx, &out);
fail:
    JS_FreeValue(ctx, item);
    qp_cd_errors_free(ctx, &ce);
    qp_cd_decin_free(ctx, &in);
    qp_ucsbuilder_free(&out);
    return JS_EXCEPTION;
}

enum { QP_CD_ENC_SUCCESS, QP_CD_ENC_FAILED, QP_CD_ENC_EXCEPTION };

/* charmapencode_output: look c up in mapping and append the result. */
static int qp_cd_charmap_output(JSContext *ctx, Py_UCS4 c, JSValueConst mapping,
                                const uint8_t *emap, DynBuf *d)
{
    JSValue key, x;
    if (emap) {
        int res = qp_cd_emap_lookup(c, emap);
        if (res == -1)
            return QP_CD_ENC_FAILED;
        dbuf_putc(d, (uint8_t)res);
        return QP_CD_ENC_SUCCESS;
    }
    /* charmapencode_lookup */
    key = qp_long_from_int64(ctx, c);
    if (JS_IsException(key))
        return QP_CD_ENC_EXCEPTION;
    x = qp_getitem(ctx, mapping, key);
    JS_FreeValue(ctx, key);
    if (JS_IsException(x)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, LookupError))) {
            /* No mapping found means: mapping is undefined. */
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return QP_CD_ENC_FAILED;
        }
        return QP_CD_ENC_EXCEPTION;
    }
    if (qp_cd_is_none(x)) {
        JS_FreeValue(ctx, x);
        return QP_CD_ENC_FAILED;
    }
    if (qp_long_check(ctx, x)) {
        int64_t value;
        int overflow;
        JSValue iv = qp_long_value(ctx, x);
        JS_FreeValue(ctx, x);
        if (JS_IsException(iv))
            return QP_CD_ENC_EXCEPTION;
        qp_long_as_int64_overflow(iv, &value, &overflow);
        JS_FreeValue(ctx, iv);
        if (overflow || value < 0 || value > 255) {
            qp_raise_type_error(ctx, "character mapping must be in range(256)");
            return QP_CD_ENC_EXCEPTION;
        }
        dbuf_putc(d, (uint8_t)value);
        return QP_CD_ENC_SUCCESS;
    }
    if (qp_bytes_check(ctx, x)) {
        Py_ssize_t len;
        const uint8_t *p = qp_bytes_data(x, &len);
        dbuf_put(d, p, len);
        JS_FreeValue(ctx, x);
        return QP_CD_ENC_SUCCESS;
    }
    {
        /* wrong return value */
        QPType *t = qp_type_of(ctx, x);
        JSValue tn = t->tp_name ? qp_str_from_cstr(ctx, t->tp_name) : qp_str_from_atom(ctx, t->name);
        JS_FreeValue(ctx, x);
        if (JS_IsException(tn))
            return QP_CD_ENC_EXCEPTION;
        qp_raise_type_error(ctx, "character mapping must return integer, bytes or None, not %.400U", tn);
        JS_FreeValue(ctx, tn);
        return QP_CD_ENC_EXCEPTION;
    }
}

/* _Py_GetErrorHandler subset used by the charmap encoder */
enum { QP_CD_EH_UNKNOWN, QP_CD_EH_STRICT, QP_CD_EH_REPLACE, QP_CD_EH_IGNORE,
       QP_CD_EH_XMLCHARREFREPLACE, QP_CD_EH_OTHER };

static int qp_cd_get_error_handler(const char *errors)
{
    if (errors == NULL || strcmp(errors, "strict") == 0)
        return QP_CD_EH_STRICT;
    if (strcmp(errors, "replace") == 0)
        return QP_CD_EH_REPLACE;
    if (strcmp(errors, "ignore") == 0)
        return QP_CD_EH_IGNORE;
    if (strcmp(errors, "xmlcharrefreplace") == 0)
        return QP_CD_EH_XMLCHARREFREPLACE;
    return QP_CD_EH_OTHER;
}

/* charmap_encoding_error */
static int qp_cd_charmap_encoding_error(JSContext *ctx, QPEncIn *in, Py_ssize_t *inpos,
                                        JSValueConst mapping, const uint8_t *emap,
                                        QPCodecErrors *ce, int *error_handler, DynBuf *d)
{
    const char *encoding = "charmap";
    const char *reason = "character maps to <undefined>";
    Py_ssize_t size = in->v.len, collstartpos = *inpos, collendpos = *inpos + 1, collpos, newpos, i;
    int x;
    JSValue rep;
    DynBuf scratch;

    /* find all unencodable characters */
    js_dbuf_init(ctx, &scratch);
    while (collendpos < size) {
        Py_UCS4 ch = QP_CD_READ(in, collendpos);
        if (emap) {
            if (qp_cd_emap_lookup(ch, emap) != -1)
                break;
            ++collendpos;
            continue;
        }
        scratch.size = 0;
        x = qp_cd_charmap_output(ctx, ch, mapping, NULL, &scratch);
        if (x == QP_CD_ENC_EXCEPTION) {
            dbuf_free(&scratch);
            return -1;
        }
        if (x == QP_CD_ENC_SUCCESS)
            break;
        ++collendpos;
    }
    dbuf_free(&scratch);
    /* cache callback name lookup (if not done yet, i.e. it's the first error) */
    if (*error_handler == QP_CD_EH_UNKNOWN)
        *error_handler = qp_cd_get_error_handler(ce->errors);

    switch (*error_handler) {
    case QP_CD_EH_STRICT:
        return qp_cd_raise_encode(ctx, ce, encoding, in->str, collstartpos, collendpos, reason);
    case QP_CD_EH_REPLACE:
        for (collpos = collstartpos; collpos < collendpos; ++collpos) {
            x = qp_cd_charmap_output(ctx, '?', mapping, emap, d);
            if (x == QP_CD_ENC_EXCEPTION)
                return -1;
            if (x == QP_CD_ENC_FAILED)
                return qp_cd_raise_encode(ctx, ce, encoding, in->str, collstartpos, collendpos, reason);
        }
        /* fall through */
    case QP_CD_EH_IGNORE:
        *inpos = collendpos;
        break;
    case QP_CD_EH_XMLCHARREFREPLACE:
        /* generate replacement (temporarily (mis)uses p) */
        for (collpos = collstartpos; collpos < collendpos; ++collpos) {
            char buffer[2 + 29 + 1 + 1];
            char *cp;
            snprintf(buffer, sizeof(buffer), "&#%d;", (int)QP_CD_READ(in, collpos));
            for (cp = buffer; *cp; ++cp) {
                x = qp_cd_charmap_output(ctx, (unsigned char)*cp, mapping, emap, d);
                if (x == QP_CD_ENC_EXCEPTION)
                    return -1;
                if (x == QP_CD_ENC_FAILED)
                    return qp_cd_raise_encode(ctx, ce, encoding, in->str, collstartpos, collendpos,
                                              reason);
            }
        }
        *inpos = collendpos;
        break;
    default:
        rep = qp_cd_encode_error(ctx, ce, encoding, reason, in->str, size, collstartpos, collendpos,
                                 &newpos);
        if (JS_IsException(rep))
            return -1;
        if (qp_bytes_check(ctx, rep)) {
            /* Directly copy bytes result to output. */
            Py_ssize_t rlen;
            const uint8_t *rd = qp_bytes_data(rep, &rlen);
            dbuf_put(d, rd, rlen);
            *inpos = newpos;
            JS_FreeValue(ctx, rep);
            break;
        }
        {
            /* generate replacement */
            qp_strview rv;
            if (qp_str_view_acquire(ctx, rep, &rv) < 0) {
                JS_FreeValue(ctx, rep);
                return -1;
            }
            for (i = 0; i < rv.len; i++) {
                x = qp_cd_charmap_output(ctx, QP_SL_READ(rv.kind, rv.data, i), mapping, emap, d);
                if (x == QP_CD_ENC_EXCEPTION) {
                    qp_str_view_release(ctx, &rv);
                    JS_FreeValue(ctx, rep);
                    return -1;
                }
                if (x == QP_CD_ENC_FAILED) {
                    qp_str_view_release(ctx, &rv);
                    JS_FreeValue(ctx, rep);
                    return qp_cd_raise_encode(ctx, ce, encoding, in->str, collstartpos, collendpos,
                                              reason);
                }
            }
            qp_str_view_release(ctx, &rv);
            JS_FreeValue(ctx, rep);
            *inpos = newpos;
        }
        break;
    }
    return 0;
}

/* _PyUnicode_EncodeCharmap (mapping QP_NULL = Latin-1) */
static JSValue qp_cd_encode_charmap(JSContext *ctx, JSValueConst str, JSValueConst mapping,
                                    const char *errors)
{
    QPCodecErrors ce;
    QPEncIn in;
    DynBuf d;
    Py_ssize_t inpos = 0;
    int error_handler = QP_CD_EH_UNKNOWN, x;
    const uint8_t *emap;

    if (QP_IS_NULL(mapping))
        return qp_cd_encode_ucs(ctx, str, errors, 256);
    emap = qp_cd_emap_data(ctx, mapping);
    if (qp_cd_encin_init(ctx, &in, str) < 0)
        return JS_EXCEPTION;
    qp_cd_errors_init(&ce, errors);
    js_dbuf_init(ctx, &d);
    while (inpos < in.v.len) {
        Py_UCS4 ch = QP_CD_READ(&in, inpos);
        /* try to encode it */
        x = qp_cd_charmap_output(ctx, ch, mapping, emap, &d);
        if (x == QP_CD_ENC_EXCEPTION)
            goto fail;
        if (x == QP_CD_ENC_FAILED) { /* unencodable character */
            if (qp_cd_charmap_encoding_error(ctx, &in, &inpos, mapping, emap, &ce, &error_handler,
                                             &d) < 0)
                goto fail;
        } else {
            /* done with this character => adjust input position */
            ++inpos;
        }
    }
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    return qp_cd_bytes_from_dbuf(ctx, &d);
fail:
    qp_cd_errors_free(ctx, &ce);
    qp_cd_encin_free(ctx, &in);
    dbuf_free(&d);
    return JS_EXCEPTION;
}

/* ========================================================================= */
/* The _codecs module (Modules/_codecsmodule.c)                               */
/* ========================================================================= */

/* Argument Clinic data converters: Py_buffer (accept_str: also str, as its
   UTF-8 encoding). */
typedef struct QPCdData {
    const uint8_t *data;
    Py_ssize_t len;
    QPBuffer view;
    BOOL has_view;
    const char *cstr;       /* JS_FreeCString */
} QPCdData;

static int qp_cd_data_arg(JSContext *ctx, JSValueConst v, BOOL accept_str, QPCdData *d)
{
    memset(d, 0, sizeof(*d));
    if (accept_str && qp_str_check(ctx, v)) {
        JSValue s = qp_str_value(ctx, v);
        size_t len;
        if (JS_IsException(s))
            return -1;
        d->cstr = qp_str_utf8(ctx, s, &len);
        JS_FreeValue(ctx, s);
        if (!d->cstr)
            return -1;
        d->data = (const uint8_t *)d->cstr;
        d->len = (Py_ssize_t)len;
        return 0;
    }
    if (qp_buffer_get(ctx, v, &d->view, QP_BUF_SIMPLE) < 0)
        return -1;
    d->has_view = TRUE;
    d->data = d->view.buf;
    d->len = d->view.len;
    return 0;
}

static void qp_cd_data_free(JSContext *ctx, QPCdData *d)
{
    if (d->has_view)
        qp_buffer_release(ctx, &d->view);
    if (d->cstr)
        JS_FreeCString(ctx, d->cstr);
    d->has_view = FALSE;
    d->cstr = NULL;
}

/* clinic `str: unicode` converter */
static JSValue qp_cd_unicode_arg(JSContext *ctx, const char *fname, const char *argdesc,
                                 JSValueConst v)
{
    if (!qp_str_check(ctx, v))
        return qp_cd_bad_argument(ctx, fname, argdesc, "str", v);
    return JS_DupValue(ctx, v);
}

static JSValue qp_cd_m_register(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    if (qp_codec_register(ctx, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_cd_m_unregister(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    if (qp_codec_unregister(ctx, args[0]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue qp_cd_m_lookup(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    const char *encoding;
    JSValue r;
    if (qp_cd_str_arg(ctx, "lookup", "argument", args[0], FALSE, &encoding) < 0)
        return JS_EXCEPTION;
    r = qp_codec_lookup(ctx, encoding);
    JS_FreeCString(ctx, encoding);
    return r;
}

/* encode(obj, encoding='utf-8', errors='strict') / decode(...) (magic 1) */
static JSValue qp_cd_m_encode_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "obj", "encoding", "errors", NULL };
    const char *fname = magic ? "decode" : "encode";
    QPArgSpec spec = { fname, kw, 1, 3, 0 };
    JSValueConst a[3];
    const char *encoding = NULL, *errors = NULL;
    JSValue r = JS_EXCEPTION;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_cd_str_arg(ctx, fname, "argument 'encoding'", a[1], FALSE, &encoding) < 0)
        return JS_EXCEPTION;
    if (qp_cd_str_arg(ctx, fname, "argument 'errors'", a[2], FALSE, &errors) < 0)
        goto done;
    /* PyUnicode_GetDefaultEncoding() */
    r = magic ? qp_codec_decode(ctx, a[0], encoding ? encoding : "utf-8", errors)
              : qp_codec_encode(ctx, a[0], encoding ? encoding : "utf-8", errors);
done:
    qp_cd_free_cstr(ctx, encoding);
    qp_cd_free_cstr(ctx, errors);
    return r;
}

/* Positional (data, errors=None) prefix shared by the codec functions. */
static int qp_cd_errors_arg(JSContext *ctx, const char *fname, int nargs, JSValueConst *args,
                            int index, const char **errors)
{
    char desc[32];
    *errors = NULL;
    if (nargs <= index)
        return 0;
    snprintf(desc, sizeof(desc), "argument %d", index + 1);
    return qp_cd_str_arg(ctx, fname, desc, args[index], TRUE, errors);
}

static JSValue qp_cd_m_escape_encode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const char *errors;
    const uint8_t *p;
    Py_ssize_t size, i;
    DynBuf d;

    if (qp_arg_check_positional(ctx, "escape_encode", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_bytes_check(ctx, args[0]))
        return qp_cd_bad_argument(ctx, "escape_encode", "argument 1", "bytes", args[0]);
    if (qp_cd_errors_arg(ctx, "escape_encode", nargs, args, 1, &errors) < 0)
        return JS_EXCEPTION;
    qp_cd_free_cstr(ctx, errors);
    p = qp_bytes_data(args[0], &size);
    js_dbuf_init(ctx, &d);
    for (i = 0; i < size; i++) {
        uint8_t c = p[i];
        if (c == '\'' || c == '\\') {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, c);
        } else if (c == '\t') {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 't');
        } else if (c == '\n') {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'n');
        } else if (c == '\r') {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'r');
        } else if (c < ' ' || c >= 0x7f) {
            dbuf_putc(&d, '\\');
            dbuf_putc(&d, 'x');
            dbuf_putc(&d, qp_cd_hexdigits[(c & 0xf0) >> 4]);
            dbuf_putc(&d, qp_cd_hexdigits[c & 0xf]);
        } else {
            dbuf_putc(&d, c);
        }
    }
    return qp_cd_tuple2(ctx, qp_cd_bytes_from_dbuf(ctx, &d), size);
}

static JSValue qp_cd_m_escape_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const char *errors = NULL;
    QPCdData d;
    JSValue r;

    if (qp_arg_check_positional(ctx, "escape_decode", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], TRUE, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, "escape_decode", nargs, args, 1, &errors) < 0) {
        qp_cd_data_free(ctx, &d);
        return JS_EXCEPTION;
    }
    r = qp_cd_tuple2(ctx, qp_cd_decode_escape_bytes(ctx, d.data, d.len, errors), d.len);
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return r;
}

/* codec ids (magic) */
enum {
    QP_CD_UTF8, QP_CD_UTF7, QP_CD_UTF16, QP_CD_UTF16_LE, QP_CD_UTF16_BE,
    QP_CD_UTF32, QP_CD_UTF32_LE, QP_CD_UTF32_BE, QP_CD_UNICODE_ESCAPE,
    QP_CD_RAW_UNICODE_ESCAPE, QP_CD_LATIN1, QP_CD_ASCII,
};

static const char *const qp_cd_names[] = {
    "utf_8", "utf_7", "utf_16", "utf_16_le", "utf_16_be", "utf_32", "utf_32_le",
    "utf_32_be", "unicode_escape", "raw_unicode_escape", "latin_1", "ascii",
};

/* <codec>_decode(data, errors=None, final=False) (final=True for the
   escape codecs, which also accept str) */
static JSValue qp_cd_m_stateful_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    char fname[32];
    BOOL escape = magic == QP_CD_UNICODE_ESCAPE || magic == QP_CD_RAW_UNICODE_ESCAPE;
    const char *errors = NULL;
    int final = escape, byteorder;
    QPCdData d;
    Py_ssize_t consumed;
    JSValue decoded;

    snprintf(fname, sizeof(fname), "%s_decode", qp_cd_names[magic]);
    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], escape, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, fname, nargs, args, 1, &errors) < 0)
        goto fail;
    if (nargs > 2 && qp_arg_bool(ctx, args[2], &final) < 0)
        goto fail;
    consumed = d.len;
    switch (magic) {
    case QP_CD_UTF8:
        decoded = qp_cd_decode_utf8(ctx, d.data, d.len, errors, final ? NULL : &consumed);
        break;
    case QP_CD_UTF7:
        decoded = qp_cd_decode_utf7(ctx, d.data, d.len, errors, final ? NULL : &consumed);
        break;
    case QP_CD_UTF16: case QP_CD_UTF16_LE: case QP_CD_UTF16_BE:
        byteorder = magic == QP_CD_UTF16 ? 0 : magic == QP_CD_UTF16_LE ? -1 : 1;
        decoded = qp_cd_decode_utf16(ctx, d.data, d.len, errors, &byteorder,
                                     final ? NULL : &consumed);
        break;
    case QP_CD_UTF32: case QP_CD_UTF32_LE: case QP_CD_UTF32_BE:
        byteorder = magic == QP_CD_UTF32 ? 0 : magic == QP_CD_UTF32_LE ? -1 : 1;
        decoded = qp_cd_decode_utf32(ctx, d.data, d.len, errors, &byteorder,
                                     final ? NULL : &consumed);
        break;
    case QP_CD_UNICODE_ESCAPE:
        decoded = qp_cd_decode_unicode_escape(ctx, d.data, d.len, errors, final ? NULL : &consumed);
        break;
    default:
        decoded = qp_cd_decode_raw_unicode_escape(ctx, d.data, d.len, errors,
                                                  final ? NULL : &consumed);
        break;
    }
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return qp_cd_tuple2(ctx, decoded, consumed);
fail:
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return JS_EXCEPTION;
}

/* utf_16_ex_decode / utf_32_ex_decode (magic 1)(data, errors=None,
   byteorder=0, final=False) -> (str, consumed, byteorder) */
static JSValue qp_cd_m_utf_ex_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const char *fname = magic ? "utf_32_ex_decode" : "utf_16_ex_decode";
    const char *errors = NULL;
    int final = 0, byteorder = 0;
    QPCdData d;
    Py_ssize_t consumed;
    JSValue decoded, items[3];

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 4) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], FALSE, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, fname, nargs, args, 1, &errors) < 0)
        goto fail;
    if (nargs > 2 && qp_arg_int(ctx, args[2], &byteorder) < 0)
        goto fail;
    if (nargs > 3 && qp_arg_bool(ctx, args[3], &final) < 0)
        goto fail;
    consumed = d.len;
    if (magic)
        decoded = qp_cd_decode_utf32(ctx, d.data, d.len, errors, &byteorder, final ? NULL : &consumed);
    else
        decoded = qp_cd_decode_utf16(ctx, d.data, d.len, errors, &byteorder, final ? NULL : &consumed);
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    if (JS_IsException(decoded))
        return decoded;
    items[0] = decoded;
    items[1] = qp_long_from_int64(ctx, consumed);
    items[2] = qp_long_from_int64(ctx, byteorder);
    return qp_tuple_new_steal(ctx, 3, items);
fail:
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return JS_EXCEPTION;
}

/* latin_1_decode / ascii_decode (data, errors=None) */
static JSValue qp_cd_m_simple_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    char fname[32];
    const char *errors = NULL;
    QPCdData d;
    JSValue decoded;

    snprintf(fname, sizeof(fname), "%s_decode", qp_cd_names[magic]);
    if (qp_arg_check_positional(ctx, fname, nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], FALSE, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, fname, nargs, args, 1, &errors) < 0) {
        qp_cd_data_free(ctx, &d);
        return JS_EXCEPTION;
    }
    if (magic == QP_CD_LATIN1)
        decoded = qp_cd_decode_latin1(ctx, d.data, d.len);
    else
        decoded = qp_cd_decode_ascii(ctx, d.data, d.len, errors);
    decoded = qp_cd_tuple2(ctx, decoded, d.len);
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return decoded;
}

static JSValue qp_cd_m_charmap_decode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    const char *errors = NULL;
    QPCdData d;
    JSValueConst mapping = QP_NULL;
    JSValue decoded;

    if (qp_arg_check_positional(ctx, "charmap_decode", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], FALSE, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, "charmap_decode", nargs, args, 1, &errors) < 0) {
        qp_cd_data_free(ctx, &d);
        return JS_EXCEPTION;
    }
    if (nargs > 2 && !qp_cd_is_none(args[2]))
        mapping = args[2];
    decoded = qp_cd_tuple2(ctx, qp_cd_decode_charmap(ctx, d.data, d.len, mapping, errors), d.len);
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return decoded;
}

static JSValue qp_cd_m_readbuffer_encode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    const char *errors = NULL;
    QPCdData d;
    JSValue r;

    if (qp_arg_check_positional(ctx, "readbuffer_encode", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_cd_data_arg(ctx, args[0], TRUE, &d) < 0)
        return JS_EXCEPTION;
    if (qp_cd_errors_arg(ctx, "readbuffer_encode", nargs, args, 1, &errors) < 0) {
        qp_cd_data_free(ctx, &d);
        return JS_EXCEPTION;
    }
    r = qp_cd_tuple2(ctx, qp_bytes_new(ctx, d.data, d.len), d.len);
    qp_cd_free_cstr(ctx, errors);
    qp_cd_data_free(ctx, &d);
    return r;
}

/* <codec>_encode(str, errors=None) -> (bytes, len(str)) */
static JSValue qp_cd_m_str_encode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    char fname[32];
    const char *errors = NULL;
    JSValue str, encoded;
    Py_ssize_t len;

    snprintf(fname, sizeof(fname), "%s_encode", qp_cd_names[magic]);
    if (qp_arg_check_positional(ctx, fname, nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    str = qp_cd_unicode_arg(ctx, fname, "argument 1", args[0]);
    if (JS_IsException(str))
        return str;
    if (qp_cd_errors_arg(ctx, fname, nargs, args, 1, &errors) < 0) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    len = qp_str_len(ctx, str);
    switch (magic) {
    case QP_CD_UTF8:
        encoded = qp_cd_encode_ucs(ctx, str, errors, 0x110000);
        break;
    case QP_CD_UTF7:
        encoded = qp_cd_encode_utf7(ctx, str);
        break;
    case QP_CD_UTF16_LE:
        encoded = qp_cd_encode_utf16_32(ctx, str, errors, -1, 2);
        break;
    case QP_CD_UTF16_BE:
        encoded = qp_cd_encode_utf16_32(ctx, str, errors, 1, 2);
        break;
    case QP_CD_UTF32_LE:
        encoded = qp_cd_encode_utf16_32(ctx, str, errors, -1, 4);
        break;
    case QP_CD_UTF32_BE:
        encoded = qp_cd_encode_utf16_32(ctx, str, errors, 1, 4);
        break;
    case QP_CD_UNICODE_ESCAPE:
        encoded = qp_cd_encode_unicode_escape(ctx, str);
        break;
    case QP_CD_RAW_UNICODE_ESCAPE:
        encoded = qp_cd_encode_raw_unicode_escape(ctx, str);
        break;
    case QP_CD_LATIN1:
        encoded = qp_cd_encode_ucs(ctx, str, errors, 256);
        break;
    default:
        encoded = qp_cd_encode_ucs(ctx, str, errors, 128);
        break;
    }
    qp_cd_free_cstr(ctx, errors);
    JS_FreeValue(ctx, str);
    return qp_cd_tuple2(ctx, encoded, len);
}

/* utf_16_encode / utf_32_encode (magic 1)(str, errors=None, byteorder=0) */
static JSValue qp_cd_m_utf_ex_encode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const char *fname = magic ? "utf_32_encode" : "utf_16_encode";
    const char *errors = NULL;
    int byteorder = 0;
    JSValue str, encoded;
    Py_ssize_t len;

    if (qp_arg_check_positional(ctx, fname, nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    str = qp_cd_unicode_arg(ctx, fname, "argument 1", args[0]);
    if (JS_IsException(str))
        return str;
    if (qp_cd_errors_arg(ctx, fname, nargs, args, 1, &errors) < 0)
        goto fail;
    if (nargs > 2 && qp_arg_int(ctx, args[2], &byteorder) < 0)
        goto fail;
    len = qp_str_len(ctx, str);
    encoded = qp_cd_encode_utf16_32(ctx, str, errors, byteorder, magic ? 4 : 2);
    qp_cd_free_cstr(ctx, errors);
    JS_FreeValue(ctx, str);
    return qp_cd_tuple2(ctx, encoded, len);
fail:
    qp_cd_free_cstr(ctx, errors);
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}

static JSValue qp_cd_m_charmap_encode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    const char *errors = NULL;
    JSValueConst mapping = QP_NULL;
    JSValue str, encoded;
    Py_ssize_t len;

    if (qp_arg_check_positional(ctx, "charmap_encode", nargs, 1, 3) < 0)
        return JS_EXCEPTION;
    str = qp_cd_unicode_arg(ctx, "charmap_encode", "argument 1", args[0]);
    if (JS_IsException(str))
        return str;
    if (qp_cd_errors_arg(ctx, "charmap_encode", nargs, args, 1, &errors) < 0) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    if (nargs > 2 && !qp_cd_is_none(args[2]))
        mapping = args[2];
    len = qp_str_len(ctx, str);
    encoded = qp_cd_encode_charmap(ctx, str, mapping, errors);
    qp_cd_free_cstr(ctx, errors);
    JS_FreeValue(ctx, str);
    return qp_cd_tuple2(ctx, encoded, len);
}

static JSValue qp_cd_m_charmap_build(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    if (!qp_str_check(ctx, args[0]))
        return qp_cd_bad_argument(ctx, "charmap_build", "argument", "str", args[0]);
    return qp_cd_build_encoding_map(ctx, args[0]);
}

static JSValue qp_cd_m_register_error(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    const char *errors;
    int r;
    if (qp_arg_check_positional(ctx, "register_error", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_cd_str_arg(ctx, "register_error", "argument 1", args[0], FALSE, &errors) < 0)
        return JS_EXCEPTION;
    r = qp_codec_register_error(ctx, errors, args[1]);
    JS_FreeCString(ctx, errors);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue qp_cd_m_unregister_error(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    const char *errors;
    int r;
    if (qp_cd_str_arg(ctx, "_unregister_error", "argument", args[0], FALSE, &errors) < 0)
        return JS_EXCEPTION;
    r = qp_codec_unregister_error(ctx, errors);
    JS_FreeCString(ctx, errors);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

static JSValue qp_cd_m_lookup_error(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    const char *name;
    JSValue r;
    if (qp_cd_str_arg(ctx, "lookup_error", "argument", args[0], FALSE, &name) < 0)
        return JS_EXCEPTION;
    r = qp_codec_lookup_error(ctx, name);
    JS_FreeCString(ctx, name);
    return r;
}

/* _Py_normalize_encoding(encoding, buf, len, to_lower=0): runs of
   non-alphanumeric, non-'.' characters become one '_' (none at the start
   or the end). */
static JSValue qp_cd_m_normalize_encoding(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                          int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "encoding", NULL };
    QPArgSpec spec = { "_normalize_encoding", kw, 1, 1, 0 };
    JSValueConst a[1];
    JSValue str, result;
    const char *cstr, *e;
    char *normalized, *l;
    size_t len;
    int punct = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_cd_bad_argument(ctx, "_normalize_encoding", "argument 'encoding'", "str", a[0]);
    str = qp_str_value(ctx, a[0]);
    if (JS_IsException(str))
        return str;
    cstr = qp_str_utf8(ctx, str, &len);
    JS_FreeValue(ctx, str);
    if (!cstr)
        return JS_EXCEPTION;
    normalized = js_malloc(ctx, len + 1);
    if (!normalized) {
        JS_FreeCString(ctx, cstr);
        return JS_EXCEPTION;
    }
    l = normalized;
    for (e = cstr; *e; e++) {
        char c = *e;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.') {
            if (punct && l != normalized)
                *l++ = '_';
            punct = 0;
            *l++ = c;
        } else {
            punct = 1;
        }
    }
    *l = '\0';
    result = qp_str_from_utf8(ctx, normalized, l - normalized);
    js_free(ctx, normalized);
    JS_FreeCString(ctx, cstr);
    return result;
}

static const QPMethodDef qp_codecs_methods[] = {
    { "register", qp_cd_m_register, QP_METH_O, 0,
      "register($module, search_function, /)\n"
      "--\n"
      "\n"
      "Register a codec search function.\n"
      "\n"
      "Search functions are expected to take one argument, the encoding\n"
      "name in all lower case letters, and either return None, or a tuple\n"
      "of functions (encoder, decoder, stream_reader, stream_writer) (or\n"
      "a CodecInfo object)." },
    { "unregister", qp_cd_m_unregister, QP_METH_O, 0,
      "unregister($module, search_function, /)\n"
      "--\n"
      "\n"
      "Unregister a codec search function and clear the registry\'s cache.\n"
      "\n"
      "If the search function is not registered, do nothing." },
    { "lookup", qp_cd_m_lookup, QP_METH_O, 0,
      "lookup($module, encoding, /)\n"
      "--\n"
      "\n"
      "Looks up a codec tuple in the Python codec registry and returns a CodecInfo object." },
    { "encode", qp_cd_m_encode_decode, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "encode($module, /, obj, encoding=\'utf-8\', errors=\'strict\')\n"
      "--\n"
      "\n"
      "Encodes obj using the codec registered for encoding.\n"
      "\n"
      "The default encoding is \'utf-8\'.  errors may be given to set a\n"
      "different error handling scheme.  Default is \'strict\' meaning that\n"
      "encoding errors raise a ValueError.  Other possible values are \'ignore\',\n"
      "\'replace\' and \'backslashreplace\' as well as any other name registered\n"
      "with codecs.register_error that can handle ValueErrors." },
    { "decode", qp_cd_m_encode_decode, QP_METH_FASTCALL | QP_METH_KEYWORDS, 1,
      "decode($module, /, obj, encoding=\'utf-8\', errors=\'strict\')\n"
      "--\n"
      "\n"
      "Decodes obj using the codec registered for encoding.\n"
      "\n"
      "Default encoding is \'utf-8\'.  errors may be given to set a\n"
      "different error handling scheme.  Default is \'strict\' meaning that\n"
      "encoding errors raise a ValueError.  Other possible values are \'ignore\',\n"
      "\'replace\' and \'backslashreplace\' as well as any other name registered\n"
      "with codecs.register_error that can handle ValueErrors." },
    { "escape_encode", qp_cd_m_escape_encode, QP_METH_FASTCALL, 0,
      "escape_encode($module, data, errors=None, /)\n"
      "--\n"
      "\n" },
    { "escape_decode", qp_cd_m_escape_decode, QP_METH_FASTCALL, 0,
      "escape_decode($module, data, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_8_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF8,
      "utf_8_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_8_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF8,
      "utf_8_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_7_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF7,
      "utf_7_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_7_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF7,
      "utf_7_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_16_encode", qp_cd_m_utf_ex_encode, QP_METH_FASTCALL, 0,
      "utf_16_encode($module, str, errors=None, byteorder=0, /)\n"
      "--\n"
      "\n" },
    { "utf_16_le_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF16_LE,
      "utf_16_le_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_16_be_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF16_BE,
      "utf_16_be_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_16_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF16,
      "utf_16_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_16_le_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF16_LE,
      "utf_16_le_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_16_be_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF16_BE,
      "utf_16_be_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_16_ex_decode", qp_cd_m_utf_ex_decode, QP_METH_FASTCALL, 0,
      "utf_16_ex_decode($module, data, errors=None, byteorder=0, final=False,\n"
      "                 /)\n"
      "--\n"
      "\n" },
    { "utf_32_encode", qp_cd_m_utf_ex_encode, QP_METH_FASTCALL, 1,
      "utf_32_encode($module, str, errors=None, byteorder=0, /)\n"
      "--\n"
      "\n" },
    { "utf_32_le_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF32_LE,
      "utf_32_le_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_32_be_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UTF32_BE,
      "utf_32_be_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "utf_32_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF32,
      "utf_32_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_32_le_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF32_LE,
      "utf_32_le_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_32_be_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UTF32_BE,
      "utf_32_be_decode($module, data, errors=None, final=False, /)\n"
      "--\n"
      "\n" },
    { "utf_32_ex_decode", qp_cd_m_utf_ex_decode, QP_METH_FASTCALL, 1,
      "utf_32_ex_decode($module, data, errors=None, byteorder=0, final=False,\n"
      "                 /)\n"
      "--\n"
      "\n" },
    { "unicode_escape_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_UNICODE_ESCAPE,
      "unicode_escape_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "unicode_escape_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_UNICODE_ESCAPE,
      "unicode_escape_decode($module, data, errors=None, final=True, /)\n"
      "--\n"
      "\n" },
    { "raw_unicode_escape_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_RAW_UNICODE_ESCAPE,
      "raw_unicode_escape_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "raw_unicode_escape_decode", qp_cd_m_stateful_decode, QP_METH_FASTCALL, QP_CD_RAW_UNICODE_ESCAPE,
      "raw_unicode_escape_decode($module, data, errors=None, final=True, /)\n"
      "--\n"
      "\n" },
    { "latin_1_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_LATIN1,
      "latin_1_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "latin_1_decode", qp_cd_m_simple_decode, QP_METH_FASTCALL, QP_CD_LATIN1,
      "latin_1_decode($module, data, errors=None, /)\n"
      "--\n"
      "\n" },
    { "ascii_encode", qp_cd_m_str_encode, QP_METH_FASTCALL, QP_CD_ASCII,
      "ascii_encode($module, str, errors=None, /)\n"
      "--\n"
      "\n" },
    { "ascii_decode", qp_cd_m_simple_decode, QP_METH_FASTCALL, QP_CD_ASCII,
      "ascii_decode($module, data, errors=None, /)\n"
      "--\n"
      "\n" },
    { "charmap_encode", qp_cd_m_charmap_encode, QP_METH_FASTCALL, 0,
      "charmap_encode($module, str, errors=None, mapping=None, /)\n"
      "--\n"
      "\n" },
    { "charmap_decode", qp_cd_m_charmap_decode, QP_METH_FASTCALL, 0,
      "charmap_decode($module, data, errors=None, mapping=None, /)\n"
      "--\n"
      "\n" },
    { "charmap_build", qp_cd_m_charmap_build, QP_METH_O, 0,
      "charmap_build($module, map, /)\n"
      "--\n"
      "\n" },
    { "readbuffer_encode", qp_cd_m_readbuffer_encode, QP_METH_FASTCALL, 0,
      "readbuffer_encode($module, data, errors=None, /)\n"
      "--\n"
      "\n" },
    { "register_error", qp_cd_m_register_error, QP_METH_FASTCALL, 0,
      "register_error($module, errors, handler, /)\n"
      "--\n"
      "\n"
      "Register the specified error handler under the name errors.\n"
      "\n"
      "handler must be a callable object, that will be called with an exception\n"
      "instance containing information about the location of the\n"
      "encoding/decoding error and must return a (replacement, new position)\n"
      "tuple." },
    { "_unregister_error", qp_cd_m_unregister_error, QP_METH_O, 0,
      "_unregister_error($module, errors, /)\n"
      "--\n"
      "\n"
      "Un-register the specified error handler for the error handling `errors\'.\n"
      "\n"
      "Only custom error handlers can be un-registered. An exception is raised\n"
      "if the error handling is a built-in one (e.g., \'strict\'), or if an error\n"
      "occurs.\n"
      "\n"
      "Otherwise, this returns True if a custom handler has been successfully\n"
      "un-registered, and False if no custom handler for the specified error\n"
      "handling exists." },
    { "lookup_error", qp_cd_m_lookup_error, QP_METH_O, 0,
      "lookup_error($module, name, /)\n"
      "--\n"
      "\n"
      "lookup_error(errors) -> handler\n"
      "\n"
      "Return the error handler for the specified error handling name or raise\n"
      "a LookupError, if no handler exists under this name." },
    { "_normalize_encoding", qp_cd_m_normalize_encoding, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "_normalize_encoding($module, /, encoding)\n"
      "--\n"
      "\n"
      "Normalize an encoding name *encoding*.\n"
      "\n"
      "Used for encodings.normalize_encoding. Does not convert to lower case." },
    { NULL },
};

static int qp_codecs_exec(JSContext *ctx, JSValueConst module)
{
    /* the registry is per interpreter and normally exists already (the str
       type and the boot code use it before `_codecs` is imported) */
    return qp_codec_state(ctx) ? 0 : -1;
}

static const QPModuleDef qp_moddef__codecs = {
    "_codecs",
    NULL,
    qp_codecs_methods,
    qp_codecs_exec,
};
