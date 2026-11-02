/*
 * The _tokenize module: tokenize.py's C tokenizer iterator.
 *
 * Derived from CPython Python/Python-tokenize.c,
 * Parser/tokenizer/readline_tokenizer.c and Parser/lexer/buffer.c
 * (PSF-2.0), v3.15.2.
 *
 * TokenizerIter(readline, *, extra_tokens, encoding=None) runs the M1 lexer
 * (python/lexer.c) over the lines returned by readline(), fetched lazily
 * like CPython's readline tokenizer: the lexer's underflow hook calls
 * readline (str lines, or bytes lines decoded with `encoding` and the
 * "replace" error handler) and appends the line to a growable buffer.
 *
 * Layout: the iterator is a PY_OBJECT instance with three slots: the C
 * state (an ArrayBuffer whose data is the QPTokIter record, freed with it),
 * the readline callable and the cached current line (str).  The record
 * starts with the struct tok_state, so _PyTokenizer_Free releases it.
 * Lexer errors (tok->err) are raised as SyntaxError & co. with filename
 * "<string>"; an exception raised by readline() is propagated.
 */

#define QP_TOKITER_SLOT_STATE    0
#define QP_TOKITER_SLOT_READLINE 1
#define QP_TOKITER_SLOT_LINE     2
#define QP_TOKITER_NSLOTS        3

typedef struct QPTokIter {
    struct tok_state tok;       /* first member: _PyTokenizer_Free frees the record */
    qp_error err;               /* tok.err */
    JSContext *ctx;
    JSValueConst readline;      /* borrowed: owned by QP_TOKITER_SLOT_READLINE */
    char *bufmem;               /* the line buffer (tok.buf may be cleared by error_ret) */
    int py_error;               /* readline() raised: the exception is pending */
    int done;
    /* cached line (CPython it->last_line): its UTF-8 bytes */
    char *line_utf8;
    Py_ssize_t line_len;
    Py_ssize_t last_lineno;
    Py_ssize_t last_end_lineno;
    Py_ssize_t byte_col_offset_diff;
} QPTokIter;

static const char qp_tokiter_filename[] = "<string>";

static void qp_tokiter_release(QPTokIter *it)
{
    qp_sys_free(it->bufmem);
    qp_sys_free(it->line_utf8);
    qp_error_clear(&it->err);
    _PyTokenizer_Free(&it->tok);
}

static void qp_tokiter_free_buffer(JSRuntime *rt, void *opaque, void *ptr)
{
    qp_tokiter_release((QPTokIter *)ptr);
}

static QPTokIter *qp_tokiter_get(JSValueConst self)
{
    JSValue *slots = qp_instance_slots(self);
    JSObject *p;
    if (!slots || JS_VALUE_GET_TAG(slots[QP_TOKITER_SLOT_STATE]) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(slots[QP_TOKITER_SLOT_STATE]);
    if (p->class_id != JS_CLASS_ARRAY_BUFFER || !p->u.array_buffer)
        return NULL;
    return (QPTokIter *)p->u.array_buffer->data;
}

/* ---- readline tokenizer --------------------------------------------------- */

/* _PyLexer_remember_fstring_buffers / _PyLexer_restore_fstring_buffers */
static void qp_tokiter_remember_fstring_buffers(struct tok_state *tok)
{
    int index;
    tokenizer_mode *mode;
    for (index = tok->tok_mode_stack_index; index >= 0; --index) {
        mode = &(tok->tok_mode_stack[index]);
        mode->start_offset = mode->start == NULL ? -1 : mode->start - tok->buf;
        mode->multi_line_start_offset = mode->multi_line_start == NULL ? -1 : mode->multi_line_start - tok->buf;
        mode->last_expr_start_offset = mode->last_expr_start == NULL
            ? -1 : mode->last_expr_start - tok->buf;
    }
}

static void qp_tokiter_restore_fstring_buffers(struct tok_state *tok)
{
    int index;
    tokenizer_mode *mode;
    for (index = tok->tok_mode_stack_index; index >= 0; --index) {
        mode = &(tok->tok_mode_stack[index]);
        mode->start = mode->start_offset < 0 ? NULL : tok->buf + mode->start_offset;
        mode->multi_line_start = mode->multi_line_start_offset < 0 ? NULL : tok->buf + mode->multi_line_start_offset;
        mode->last_expr_start = mode->last_expr_start_offset < 0
            ? NULL : tok->buf + mode->last_expr_start_offset;
    }
}

/* _PyLexer_tok_reserve_buf */
static int qp_tokiter_reserve_buf(QPTokIter *it, Py_ssize_t size)
{
    struct tok_state *tok = &it->tok;
    Py_ssize_t cur = tok->cur - tok->buf;
    Py_ssize_t oldsize = tok->inp - tok->buf;
    Py_ssize_t newsize = oldsize + (size > (oldsize >> 1) ? size : (oldsize >> 1));
    if (newsize > tok->end - tok->buf) {
        char *newbuf;
        Py_ssize_t start = tok->start == NULL ? -1 : tok->start - tok->buf;
        Py_ssize_t line_start = tok->start == NULL ? -1 : tok->line_start - tok->buf;
        Py_ssize_t multi_line_start = tok->multi_line_start == NULL
            ? -1 : tok->multi_line_start - tok->buf;
        qp_tokiter_remember_fstring_buffers(tok);
        newbuf = qp_sys_realloc(tok->buf, newsize);
        if (newbuf == NULL) {
            tok->done = E_NOMEM;
            return 0;
        }
        it->bufmem = newbuf;
        tok->buf = newbuf;
        tok->cur = tok->buf + cur;
        tok->inp = tok->buf + oldsize;
        tok->end = tok->buf + newsize;
        tok->start = start < 0 ? NULL : tok->buf + start;
        tok->line_start = line_start < 0 ? NULL : tok->buf + line_start;
        tok->multi_line_start = multi_line_start < 0 ? NULL : tok->buf + multi_line_start;
        qp_tokiter_restore_fstring_buffers(tok);
    }
    return 1;
}

/* A Python exception is pending: stop like _PyTokenizer_error_ret. */
static int qp_tokiter_py_error(QPTokIter *it)
{
    it->py_error = 1;
    _PyTokenizer_error_ret(&it->tok);
    return 0;
}

/* tok_readline_string */
static int qp_tokiter_readline_string(QPTokIter *it)
{
    struct tok_state *tok = &it->tok;
    JSContext *ctx = it->ctx;
    JSValue raw_line, line, s;
    const char *buf;
    size_t buflen;
    int ok;

    raw_line = qp_call0(ctx, it->readline);
    if (JS_IsException(raw_line)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return 1;
        }
        return qp_tokiter_py_error(it);
    }
    if (tok->encoding != NULL) {
        JSValue args[2];
        JSAtom decode;
        if (!qp_bytes_check(ctx, raw_line)) {
            JS_FreeValue(ctx, raw_line);
            qp_raise_type_error(ctx, "readline() returned a non-bytes object");
            return qp_tokiter_py_error(it);
        }
        /* PyUnicode_Decode(bytes, tok->encoding, "replace") */
        args[0] = qp_str_from_cstr(ctx, tok->encoding);
        args[1] = qp_str_from_cstr(ctx, "replace");
        decode = JS_NewAtom(ctx, "decode");
        if (JS_IsException(args[0]) || JS_IsException(args[1]) || decode == JS_ATOM_NULL)
            line = JS_EXCEPTION;
        else
            line = qp_call_method(ctx, raw_line, decode, (JSValueConst *)args, 2, QP_NULL);
        JS_FreeAtom(ctx, decode);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, raw_line);
        if (JS_IsException(line))
            return qp_tokiter_py_error(it);
        if (!qp_str_check(ctx, line)) {
            qp_raise_type_error(ctx, "decoder should return a string result, not '%T'", line);
            JS_FreeValue(ctx, line);
            return qp_tokiter_py_error(it);
        }
    } else {
        if (!qp_str_check(ctx, raw_line)) {
            JS_FreeValue(ctx, raw_line);
            qp_raise_type_error(ctx, "readline() returned a non-string object");
            return qp_tokiter_py_error(it);
        }
        line = raw_line;
    }
    s = qp_str_value(ctx, line);
    JS_FreeValue(ctx, line);
    if (JS_IsException(s))
        return qp_tokiter_py_error(it);
    buf = qp_str_utf8(ctx, s, &buflen);
    if (buf == NULL) {
        JS_FreeValue(ctx, s);
        return qp_tokiter_py_error(it);
    }
    /* Room for the null terminator *and* a newline we may need to add. */
    ok = qp_tokiter_reserve_buf(it, (Py_ssize_t)buflen + 2);
    if (ok) {
        memcpy(tok->inp, buf, buflen);
        tok->inp += buflen;
        *tok->inp = '\0';
        tok->line_start = tok->cur;
    }
    JS_FreeCString(ctx, buf);
    JS_FreeValue(ctx, s);
    return ok;
}

/* tok_underflow_readline */
static int qp_tokiter_underflow(struct tok_state *tok)
{
    QPTokIter *it = (QPTokIter *)tok;
    if (tok->start == NULL && !INSIDE_FSTRING(tok))
        tok->cur = tok->inp = tok->buf;
    if (!qp_tokiter_readline_string(it))
        return 0;
    if (tok->inp == tok->cur) {
        tok->done = E_EOF;
        return 0;
    }
    tok->implicit_newline = 0;
    if (tok->inp[-1] != '\n') {
        /* Last line does not end in \n, fake one */
        *tok->inp++ = '\n';
        *tok->inp = '\0';
        tok->implicit_newline = 1;
    }
    tok->lineno++;      /* ADVANCE_LINENO() */
    tok->col_offset = 0;
    /* The default encoding is UTF-8: no non-UTF-8 sequences (a str line
       always encodes to valid UTF-8). */
    if (!tok->encoding && !_PyTokenizer_ensure_utf8(tok->cur, tok, tok->lineno)) {
        _PyTokenizer_error_ret(tok);
        return 0;
    }
    return tok->done == E_OK;
}

/* _PyTokenizer_FromReadline(readline, enc, 1, 1): NULL with an exception
   pending on failure. */
static QPTokIter *qp_tokiter_new_state(JSContext *ctx, JSValueConst readline, const char *enc)
{
    qp_error err;
    struct tok_state *t;
    QPTokIter *it;

    memset(&err, 0, sizeof(err));
    t = _PyTokenizer_tok_new(&err);
    if (!t) {
        qp_error_clear(&err);
        qp_raise_memory_error(ctx);
        return NULL;
    }
    it = qp_sys_calloc(1, sizeof(*it));
    if (!it) {
        _PyTokenizer_Free(t);
        qp_raise_memory_error(ctx);
        return NULL;
    }
    it->tok = *t;
    qp_sys_free(t);     /* the copy is the tokenizer now (tok_new allocates nothing else) */
    it->tok.err = &it->err;
    it->ctx = ctx;
    it->readline = readline;
    it->bufmem = qp_sys_malloc(BUFSIZ);
    if (!it->bufmem) {
        qp_tokiter_release(it);
        qp_raise_memory_error(ctx);
        return NULL;
    }
    it->tok.buf = it->tok.cur = it->tok.inp = it->bufmem;
    it->tok.end = it->tok.buf + BUFSIZ;
    it->tok.fp = NULL;
    if (enc != NULL) {
        it->tok.encoding = _PyTokenizer_new_string(enc, strlen(enc), &it->tok);
        if (!it->tok.encoding) {
            qp_tokiter_release(it);
            qp_raise_memory_error(ctx);
            return NULL;
        }
    }
    it->tok.decoding_state = STATE_NORMAL;
    it->tok.underflow = &qp_tokiter_underflow;
    return it;
}

/* ---- TokenizerIter -------------------------------------------------------- */

/* tokenizeriter_new(readline, /, *, extra_tokens, encoding='utf-8') */
static JSValue qp_tokiter_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "", "extra_tokens", "encoding", NULL };
    static const QPArgSpec spec = { "tokenizeriter", kw, 2, 1, 0 };
    JSValueConst a[3];
    JSValue self, state, enc_s = JS_UNDEFINED;
    const char *encoding = NULL;
    size_t enc_len;
    int extra_tokens;
    QPTokIter *it;
    JSValue *slots;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    extra_tokens = qp_bool(ctx, a[1]);
    if (extra_tokens < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[2])) {
        if (!qp_str_check(ctx, a[2])) {
            /* _PyArg_BadArgument */
            char buf[64];
            QPType *t = qp_type_of(ctx, a[2]);
            const char *tn = t->tp_name ? t->tp_name : JS_AtomGetStr(ctx, buf, sizeof(buf), t->name);
            return qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50s",
                                       "tokenizeriter", "argument 'encoding'", "str",
                                       JS_IsUndefined(a[2]) || JS_IsNull(a[2]) ? "None" : tn);
        }
        enc_s = qp_str_value(ctx, a[2]);
        if (JS_IsException(enc_s))
            return enc_s;
        encoding = qp_str_utf8(ctx, enc_s, &enc_len);
        if (!encoding) {
            JS_FreeValue(ctx, enc_s);
            return JS_EXCEPTION;
        }
        if (strlen(encoding) != enc_len) {
            JS_FreeCString(ctx, encoding);
            JS_FreeValue(ctx, enc_s);
            return qp_raise_value_error(ctx, "embedded null character");
        }
    }
    self = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_OBJECT);
    if (JS_IsException(self))
        goto fail;
    slots = qp_instance_slots(self);
    if (!slots) {
        qp_raise_system_error(ctx, "TokenizerIter instance without slots");
        goto fail;
    }
    it = qp_tokiter_new_state(ctx, a[0], encoding);
    if (!it)
        goto fail;
    state = JS_NewArrayBuffer(ctx, (uint8_t *)it, sizeof(*it), qp_tokiter_free_buffer, NULL, FALSE);
    if (JS_IsException(state)) {
        qp_tokiter_release(it);
        goto fail;
    }
    slots[QP_TOKITER_SLOT_STATE] = state;
    slots[QP_TOKITER_SLOT_READLINE] = JS_DupValue(ctx, a[0]);
    slots[QP_TOKITER_SLOT_LINE] = JS_UNDEFINED;
    it->tok.filename = qp_tokiter_filename;
    if (extra_tokens)
        it->tok.tok_extra_tokens = 1;
    it->done = 0;
    it->byte_col_offset_diff = 0;
    it->last_lineno = 0;
    it->last_end_lineno = 0;
    if (encoding)
        JS_FreeCString(ctx, encoding);
    JS_FreeValue(ctx, enc_s);
    return self;
fail:
    if (encoding)
        JS_FreeCString(ctx, encoding);
    JS_FreeValue(ctx, enc_s);
    if (!JS_IsException(self))
        JS_FreeValue(ctx, self);
    return JS_EXCEPTION;
}

/* Code points in UTF-8 bytes [a, b) of s (n bytes valid; bytes past the
   end count as one character each) -- _PyPegen_byte_offset_to_character_offset_line. */
static Py_ssize_t qp_tokiter_char_count(const char *s, Py_ssize_t n, Py_ssize_t a, Py_ssize_t b)
{
    Py_ssize_t len = 0;
    while (a < b) {
        unsigned char ch = a < n ? (unsigned char)s[a] : 0;
        if (ch < 0x80)
            a += 1;
        else if ((ch & 0xe0) == 0xc0)
            a += 2;
        else if ((ch & 0xf0) == 0xe0)
            a += 3;
        else
            a += 4;
        len++;
    }
    return len;
}

/* _PyPegen_byte_offset_to_character_offset_raw on a NUL-terminated line */
static Py_ssize_t qp_tokiter_char_offset_raw(const char *str, Py_ssize_t col_offset)
{
    Py_ssize_t len = (Py_ssize_t)strlen(str), i, n = 0;
    if (col_offset > len + 1)
        col_offset = len + 1;
    for (i = 0; i < col_offset; i++) {
        if (((unsigned char)str[i] & 0xc0) != 0x80)
            n++;
    }
    return n;
}

/* _tokenizer_error: the error of a tokenizer stopped without exception. */
static JSValue qp_tokiter_error(JSContext *ctx, QPTokIter *it)
{
    struct tok_state *tok = &it->tok;
    const char *msg = NULL;
    QPType *errtype = QP_EXC(ctx, SyntaxError);
    JSValue error_line, filename, tmp[6], value, args[2];
    Py_ssize_t size, offset;

    switch (tok->done) {
    case E_TOKEN:
        msg = "invalid token";
        break;
    case E_EOF: {
        JSValue m = qp_str_from_cstr(ctx, "unexpected EOF in multi-line statement"), exc;
        int col = tok->inp - tok->buf < 0 ? 0 : (int)(tok->inp - tok->buf);
        if (JS_IsException(m))
            return m;
        exc = qp_exc_new(ctx, QP_EXC(ctx, SyntaxError), 1, (JSValueConst *)&m);
        JS_FreeValue(ctx, m);
        if (JS_IsException(exc))
            return exc;
        /* PyErr_SyntaxLocationObject(filename, lineno, col): errors setting
           the attributes are ignored */
        filename = qp_str_from_cstr(ctx, qp_tokiter_filename);
        if (qp_setattr(ctx, exc, QP_ATOM(ctx, lineno), JS_NewInt32(ctx, tok->lineno)) < 0 ||
            qp_setattr(ctx, exc, QP_ATOM(ctx, offset), JS_NewInt32(ctx, col)) < 0 ||
            qp_setattr(ctx, exc, QP_ATOM(ctx, end_lineno), JS_NewInt32(ctx, tok->lineno)) < 0 ||
            qp_setattr(ctx, exc, QP_ATOM(ctx, end_offset), JS_UNDEFINED) < 0 ||
            JS_IsException(filename) ||
            qp_setattr(ctx, exc, QP_ATOM(ctx, filename), filename) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        JS_FreeValue(ctx, filename);
        return qp_raise_object(ctx, exc);
    }
    case E_DEDENT:
        msg = "unindent does not match any outer indentation level";
        errtype = QP_EXC(ctx, IndentationError);
        break;
    case E_INTR:
        return qp_raise_value(ctx, QP_EXC(ctx, KeyboardInterrupt), JS_UNDEFINED);
    case E_NOMEM:
        return qp_raise_memory_error(ctx);
    case E_TABSPACE:
        errtype = QP_EXC(ctx, TabError);
        msg = "inconsistent use of tabs and spaces in indentation";
        break;
    case E_TOODEEP:
        errtype = QP_EXC(ctx, IndentationError);
        msg = "too many levels of indentation";
        break;
    case E_LINECONT:
        msg = "unexpected character after line continuation character";
        break;
    default:
        msg = "unknown tokenization error";
    }

    size = tok->buf ? tok->inp - tok->buf : 0;
    if (size > 0)
        size -= 1;  /* Remove the newline character from the end of the line */
    error_line = qp_str_decode(ctx, tok->buf ? tok->buf : "", size, "utf-8", "replace");
    if (JS_IsException(error_line))
        return error_line;
    offset = tok->buf ? qp_tokiter_char_offset_raw(tok->buf, tok->inp - tok->buf) : 0;
    tmp[0] = qp_str_from_cstr(ctx, qp_tokiter_filename);
    tmp[1] = JS_NewInt32(ctx, tok->lineno);
    tmp[2] = qp_long_from_int64(ctx, offset);
    tmp[3] = error_line;
    tmp[4] = JS_UNDEFINED;
    tmp[5] = JS_UNDEFINED;
    if (JS_IsException(tmp[0]) || JS_IsException(tmp[2])) {
        JS_FreeValue(ctx, tmp[0]);
        JS_FreeValue(ctx, tmp[2]);
        JS_FreeValue(ctx, error_line);
        return JS_EXCEPTION;
    }
    args[1] = qp_tuple_new_steal(ctx, 6, tmp);
    if (JS_IsException(args[1]))
        return JS_EXCEPTION;
    args[0] = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(args[0])) {
        JS_FreeValue(ctx, args[1]);
        return JS_EXCEPTION;
    }
    value = qp_tuple_new_steal(ctx, 2, args);
    if (JS_IsException(value))
        return JS_EXCEPTION;
    qp_raise_value(ctx, errtype, value);
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
}

/* _get_current_line: the line as str (new reference) */
static JSValue qp_tokiter_current_line(JSContext *ctx, QPTokIter *it, JSValue *slots,
                                       const char *line_start, Py_ssize_t size,
                                       int *line_changed)
{
    if (it->tok.lineno != it->last_lineno) {
        /* Line has changed since last token: fetch the new line and cache it */
        JSValue line = qp_str_decode(ctx, line_start, size, "utf-8", "replace");
        char *copy;
        if (JS_IsException(line))
            return line;
        copy = qp_sys_malloc(size + 1);
        if (!copy) {
            JS_FreeValue(ctx, line);
            return qp_raise_memory_error(ctx);
        }
        memcpy(copy, line_start, size);
        copy[size] = '\0';
        qp_sys_free(it->line_utf8);
        it->line_utf8 = copy;
        it->line_len = size;
        JS_FreeValue(ctx, slots[QP_TOKITER_SLOT_LINE]);
        slots[QP_TOKITER_SLOT_LINE] = JS_DupValue(ctx, line);
        it->byte_col_offset_diff = 0;
        return line;
    }
    *line_changed = 0;
    return JS_DupValue(ctx, slots[QP_TOKITER_SLOT_LINE]);
}

/* _get_col_offsets */
static void qp_tokiter_col_offsets(QPTokIter *it, const struct token *token,
                                   const char *line_start, int line_changed,
                                   Py_ssize_t lineno, Py_ssize_t end_lineno,
                                   Py_ssize_t *col_offset, Py_ssize_t *end_col_offset)
{
    Py_ssize_t byte_offset = -1;
    if (token->start != NULL && token->start >= line_start) {
        byte_offset = token->start - line_start;
        if (line_changed) {
            *col_offset = qp_tokiter_char_count(it->line_utf8, it->line_len, 0, byte_offset);
            it->byte_col_offset_diff = byte_offset - *col_offset;
        } else {
            *col_offset = byte_offset - it->byte_col_offset_diff;
        }
    }
    if (token->end != NULL && token->end >= it->tok.line_start) {
        Py_ssize_t end_byte_offset = token->end - it->tok.line_start;
        if (lineno == end_lineno) {
            /* The whole token is on the same line: count from the start */
            Py_ssize_t token_col_offset = qp_tokiter_char_count(it->line_utf8, it->line_len,
                                                                byte_offset, end_byte_offset);
            *end_col_offset = *col_offset + token_col_offset;
            it->byte_col_offset_diff += token->end - token->start - token_col_offset;
        } else {
            *end_col_offset = qp_tokiter_char_offset_raw(it->tok.line_start, end_byte_offset);
            it->byte_col_offset_diff += end_byte_offset - *end_col_offset;
        }
    }
    it->last_lineno = lineno;
    it->last_end_lineno = end_lineno;
}

/* tokenizeriter_next */
static JSValue qp_tokiter_next(JSContext *ctx, JSValueConst self)
{
    QPTokIter *it = qp_tokiter_get(self);
    JSValue *slots = qp_instance_slots(self);
    struct tok_state *tok;
    struct token token;
    JSValue result = JS_EXCEPTION, str = JS_UNDEFINED, line = JS_UNDEFINED, items[5], pos[2];
    const char *line_start;
    Py_ssize_t lineno, end_lineno, col_offset = -1, end_col_offset = -1;
    int type, is_trailing_token, line_changed = 1;

    if (!it)
        return qp_raise_system_error(ctx, "uninitialized TokenizerIter");
    tok = &it->tok;
    it->ctx = ctx;
    it->readline = slots[QP_TOKITER_SLOT_READLINE];
    _PyToken_Init(&token);
    type = _PyTokenizer_Get(tok, &token);
    if (type == ERRORTOKEN) {
        if (it->py_error) {
            it->py_error = 0;   /* readline()'s exception is pending */
        } else if (it->err.kind != QP_ERR_NONE) {
            qp_raise_qp_error(ctx, &it->err, qp_tokiter_filename);
            qp_error_clear(&it->err);
        } else {
            qp_tokiter_error(ctx, it);
        }
        goto exit;
    }
    if (it->done) {
        /* StopIteration */
        it->done = 1;
        result = QP_NULL;
        goto exit;
    }
    if (token.start == NULL || token.end == NULL)
        str = qp_str_from_cstr(ctx, "");
    else
        str = qp_str_from_utf8(ctx, token.start, token.end - token.start);
    if (JS_IsException(str)) {
        str = JS_UNDEFINED;
        goto exit;
    }
    is_trailing_token = type == ENDMARKER || (type == DEDENT && tok->done == E_EOF);
    line_start = ISSTRINGLIT(type) ? tok->multi_line_start : tok->line_start;
    if (tok->tok_extra_tokens && is_trailing_token) {
        line = qp_str_from_cstr(ctx, "");
    } else {
        Py_ssize_t size = tok->inp - line_start;
        if (size >= 1 && tok->implicit_newline)
            size -= 1;
        line = qp_tokiter_current_line(ctx, it, slots, line_start, size, &line_changed);
    }
    if (JS_IsException(line)) {
        line = JS_UNDEFINED;
        goto exit;
    }
    lineno = ISSTRINGLIT(type) ? tok->first_lineno : tok->lineno;
    end_lineno = tok->lineno;
    if (it->line_utf8)
        qp_tokiter_col_offsets(it, &token, line_start, line_changed, lineno, end_lineno,
                               &col_offset, &end_col_offset);
    if (tok->tok_extra_tokens) {
        if (is_trailing_token) {
            lineno = end_lineno = lineno + 1;
            col_offset = end_col_offset = 0;
        }
        /* Adjustments to match the original Python tokenize implementation */
        if (type > DEDENT && type < OP) {
            type = OP;
        } else if (type == NEWLINE) {
            JS_FreeValue(ctx, str);
            str = JS_UNDEFINED;
            if (!tok->implicit_newline) {
                if (tok->start[0] == '\r')
                    str = qp_str_from_cstr(ctx, "\r\n");
                else
                    str = qp_str_from_cstr(ctx, "\n");
            } else {
                /* the token text: '\n' left out of the (fake) newline */
                str = qp_str_from_cstr(ctx, "");
            }
            end_col_offset++;
        } else if (type == NL) {
            if (tok->implicit_newline) {
                JS_FreeValue(ctx, str);
                str = qp_str_from_cstr(ctx, "");
            }
        }
        if (JS_IsException(str)) {
            str = JS_UNDEFINED;
            goto exit;
        }
    }
    pos[0] = qp_long_from_int64(ctx, lineno);
    pos[1] = qp_long_from_int64(ctx, col_offset);
    items[2] = qp_tuple_new_steal(ctx, 2, pos);
    if (JS_IsException(items[2]))
        goto exit;
    pos[0] = qp_long_from_int64(ctx, end_lineno);
    pos[1] = qp_long_from_int64(ctx, end_col_offset);
    items[3] = qp_tuple_new_steal(ctx, 2, pos);
    if (JS_IsException(items[3])) {
        JS_FreeValue(ctx, items[2]);
        goto exit;
    }
    items[0] = JS_NewInt32(ctx, type);
    items[1] = str;
    items[4] = line;
    str = line = JS_UNDEFINED;
    result = qp_tuple_new_steal(ctx, 5, items);
exit:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, line);
    _PyToken_Free(&token);
    if (type == ENDMARKER)
        it->done = 1;
    return result;
}

static JSValue qp_tokiter_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

static const QPTypeSpec qp_tokiter_spec = {
    .name = "_tokenize.TokenizerIter",
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_TOKITER_NSLOTS,
    .slots = {
        .tp_getattro = qp_generic_getattr,
        .tp_iter = qp_tokiter_iter,
        .tp_iternext = qp_tokiter_next,
        .tp_new = qp_tokiter_new,
    },
};

static int qp_tokenize_exec(JSContext *ctx, JSValueConst module)
{
    JSValue t = qp_type_from_spec(ctx, &qp_tokiter_spec);
    int r;
    if (JS_IsException(t))
        return -1;
    /* PyModule_AddType */
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), qp_type_from_value(t)->name, t);
    JS_FreeValue(ctx, t);
    return r;
}

static const QPModuleDef qp_moddef__tokenize = {
    "_tokenize",
    NULL,
    NULL,
    qp_tokenize_exec,
};
