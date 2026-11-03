/*
 * Python tokenizer (string input only).
 *
 * Derived from CPython Parser/lexer/lexer.c, Parser/lexer/state.c,
 * Parser/tokenizer/helpers.c,
 * Parser/tokenizer/string_tokenizer.c and Parser/tokenizer/utf8_tokenizer.c
 * (PSF-2.0).  See lexer.h for the differences from CPython.
 */

#include "lexer_unicode.h"

/* ---- unit-local CPython compat (all #undef'd at the end of this file) ---- */
#define Py_CHARMASK(c) ((unsigned char)((c) & 0xff))
#define Py_ISDIGIT(c) ((c) >= '0' && (c) <= '9')
#define Py_ISXDIGIT(c) (Py_ISDIGIT(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define Py_ISALPHA(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define Py_ISALNUM(c) (Py_ISALPHA(c) || Py_ISDIGIT(c))
#define Py_TOLOWER(c) (((c) >= 'A' && (c) <= 'Z') ? (c) - 'A' + 'a' : (c))
#define Py_MAX(a, b) ((a) > (b) ? (a) : (b))
#define Py_UNICODE_ISPRINTABLE(c) qp_lex_is_printable((uint32_t)(c))
#define Py_UNREACHABLE() abort()

/* Never change this */
#define TABSIZE 8
/* Alternate tab spacing */
#define ALTTABSIZE 1

#define ADVANCE_LINENO() \
            tok->lineno++; \
            tok->col_offset = 0;

/* ############## UNICODE / UTF-8 HELPERS ############## */

/* Binary search of a sorted inclusive range table. */
static int
qp_lex_in_ranges(uint32_t c, const uint32_t (*r)[2], size_t n)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid][0]) {
            hi = mid;
        }
        else if (c > r[mid][1]) {
            lo = mid + 1;
        }
        else {
            return 1;
        }
    }
    return 0;
}

/* Py_UNICODE_ISPRINTABLE */
static int
qp_lex_is_printable(uint32_t c)
{
    return qp_lex_in_ranges(c, qp_printable_ranges,
                            sizeof(qp_printable_ranges) / sizeof(qp_printable_ranges[0]));
}

/* _PyUnicode_IsXidStart */
static int
qp_lex_is_xid_start(uint32_t c)
{
    return qp_lex_in_ranges(c, qp_xid_start_ranges,
                            sizeof(qp_xid_start_ranges) / sizeof(qp_xid_start_ranges[0]));
}

/* _PyUnicode_IsXidContinue */
static int
qp_lex_is_xid_continue(uint32_t c)
{
    return qp_lex_in_ranges(c, qp_xid_continue_ranges,
                            sizeof(qp_xid_continue_ranges) / sizeof(qp_xid_continue_ranges[0]));
}

#define QP_LEX_IS_CONT(b) ((b) >= 0x80 && (b) < 0xC0)

/* One step of CPython's strict UTF-8 decoder (Objects/stringlib/codecs.h
   utf8_decode + unicode_decode_utf8 error handling) over [s, end).
   Returns the length of the valid sequence at s (storing its code point in
   *cp), or 0 on error with *errlen = length of the error range and *reason
   = the UnicodeDecodeError reason CPython reports. */
static int
qp_lex_utf8_step(const unsigned char *s, const unsigned char *end, uint32_t *cp,
                 Py_ssize_t *errlen, const char **reason)
{
    unsigned int ch = s[0];
    if (ch < 0x80) {
        *cp = ch;
        return 1;
    }
    if (ch < 0xC2 || ch > 0xF4) {
        *errlen = 1;
        *reason = "invalid start byte";
        return 0;
    }
    if (ch < 0xE0) {
        if (end - s < 2) {
            goto end_of_data;
        }
        if (!QP_LEX_IS_CONT(s[1])) {
            goto invalid1;
        }
        *cp = ((ch & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (ch < 0xF0) {
        if (end - s < 3) {
            if (end - s < 2) {
                goto end_of_data;
            }
            if (!QP_LEX_IS_CONT(s[1]) || (s[1] < 0xA0 ? ch == 0xE0 : ch == 0xED)) {
                goto invalid1;
            }
            goto end_of_data;
        }
        if (!QP_LEX_IS_CONT(s[1]) || (s[1] < 0xA0 ? ch == 0xE0 : ch == 0xED)) {
            goto invalid1;
        }
        if (!QP_LEX_IS_CONT(s[2])) {
            goto invalid2;
        }
        *cp = ((ch & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (end - s < 4) {
        if (end - s < 2) {
            goto end_of_data;
        }
        if (!QP_LEX_IS_CONT(s[1]) || (s[1] < 0x90 ? ch == 0xF0 : ch == 0xF4)) {
            goto invalid1;
        }
        if (end - s < 3) {
            goto end_of_data;
        }
        if (!QP_LEX_IS_CONT(s[2])) {
            goto invalid2;
        }
        goto end_of_data;
    }
    if (!QP_LEX_IS_CONT(s[1]) || (s[1] < 0x90 ? ch == 0xF0 : ch == 0xF4)) {
        goto invalid1;
    }
    if (!QP_LEX_IS_CONT(s[2])) {
        goto invalid2;
    }
    if (!QP_LEX_IS_CONT(s[3])) {
        *errlen = 3;
        *reason = "invalid continuation byte";
        return 0;
    }
    *cp = ((ch & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return 4;

invalid1:
    *errlen = 1;
    *reason = "invalid continuation byte";
    return 0;
invalid2:
    *errlen = 2;
    *reason = "invalid continuation byte";
    return 0;
end_of_data:
    *errlen = end - s;
    *reason = "unexpected end of data";
    return 0;
}

/* Encode a code point as UTF-8 into out (>= 5 bytes), NUL-terminated. */
static void
qp_lex_utf8_encode(uint32_t c, char *out)
{
    unsigned char *p = (unsigned char *)out;
    if (c < 0x80) {
        *p++ = c;
    }
    else if (c < 0x800) {
        *p++ = 0xC0 | (c >> 6);
        *p++ = 0x80 | (c & 0x3F);
    }
    else if (c < 0x10000) {
        *p++ = 0xE0 | (c >> 12);
        *p++ = 0x80 | ((c >> 6) & 0x3F);
        *p++ = 0x80 | (c & 0x3F);
    }
    else {
        *p++ = 0xF0 | (c >> 18);
        *p++ = 0x80 | ((c >> 12) & 0x3F);
        *p++ = 0x80 | ((c >> 6) & 0x3F);
        *p++ = 0x80 | (c & 0x3F);
    }
    *p = '\0';
}

/* PyUnicode_DecodeUTF8(s, len, "replace") re-encoded as UTF-8: every error
   range becomes U+FFFD.  Returns a malloc'd NUL-terminated string and the
   number of code points in *nchars, or NULL on OOM. */
static char *
qp_lex_utf8_replace(const char *s, Py_ssize_t len, Py_ssize_t *nchars)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;
    /* each invalid byte expands to at most 3 bytes */
    char *out = qp_sys_malloc((size_t)len * 3 + 1);
    char *q = out;
    Py_ssize_t n = 0;
    if (out == NULL) {
        return NULL;
    }
    while (p < end) {
        uint32_t cp;
        Py_ssize_t errlen;
        const char *reason;
        int k = qp_lex_utf8_step(p, end, &cp, &errlen, &reason);
        if (k) {
            memcpy(q, p, k);
            q += k;
            p += k;
        }
        else {
            *q++ = (char)0xEF;
            *q++ = (char)0xBF;
            *q++ = (char)0xBD;
            p += errlen;
        }
        n++;
    }
    *q = '\0';
    *nchars = n;
    return out;
}

/* Format the message of the UnicodeDecodeError CPython's utf-8/ascii codecs
   raise for the error range [start, start+errlen) of the input. */
static void
qp_lex_set_decode_error(qp_error *err, qp_error_kind kind, int lineno, int offset,
                        const char *codec, const unsigned char *input,
                        Py_ssize_t start, Py_ssize_t errlen, const char *reason)
{
    if (errlen == 1) {
        qp_error_set(err, kind, lineno, offset, 0, 0,
                     "'%s' codec can't decode byte 0x%02x in position %lld: %s",
                     codec, input[start], (long long)start, reason);
    }
    else {
        qp_error_set(err, kind, lineno, offset, 0, 0,
                     "'%s' codec can't decode bytes in position %lld-%lld: %s",
                     codec, (long long)start, (long long)(start + errlen - 1), reason);
    }
}

/* Record a MemoryError. */
static void
qp_lex_nomem(qp_error *err)
{
    if (err) {
        qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
    }
}

/* PyErr_Occurred() equivalent for the tokenizer's error sink. */
static int
qp_lex_err_occurred(struct tok_state *tok)
{
    return tok->err != NULL && tok->err->kind != QP_ERR_NONE;
}

/* ############## STATE ############## */

/* Create and initialize a new tok_state structure */
static struct tok_state *
_PyTokenizer_tok_new(qp_error *err)
{
    struct tok_state *tok = (struct tok_state *)qp_sys_calloc(1, sizeof(struct tok_state));
    if (tok == NULL) {
        qp_lex_nomem(err);
        return NULL;
    }

    tok->err = err;
    tok->buf = tok->cur = tok->inp = NULL;
    tok->fp_interactive = 0;
    tok->interactive_src_start = NULL;
    tok->interactive_src_end = NULL;
    tok->start = NULL;
    tok->end = NULL;
    tok->done = E_OK;
    tok->fp = NULL;
    tok->input = NULL;
    tok->tabsize = TABSIZE;
    tok->indent = 0;
    tok->indstack[0] = 0;
    tok->atbol = 1;
    tok->pendin = 0;
    tok->prompt = tok->nextprompt = NULL;
    tok->lineno = 0;
    tok->starting_col_offset = -1;
    tok->col_offset = -1;
    tok->level = 0;
    tok->altindstack[0] = 0;
    tok->decoding_state = STATE_INIT;
    tok->decoding_erred = 0;
    tok->enc = NULL;
    tok->encoding = NULL;
    tok->cont_line = 0;
    tok->filename = NULL;
    tok->module = QP_NULL;
    tok->ctx = NULL;
    tok->type_comments = 0;
    tok->interactive_underflow = IUNDERFLOW_NORMAL;
    tok->underflow = NULL;
    tok->str = NULL;
    tok->report_warnings = 1;
    tok->tok_extra_tokens = 0;
    tok->comment_newline = 0;
    tok->implicit_newline = 0;
    tok->tok_mode_stack[0] = (tokenizer_mode){.kind =TOK_REGULAR_MODE, .quote='\0', .quote_size = 0, .in_debug=0};
    tok->tok_mode_stack_index = 0;
    return tok;
}

/* Free a tok_state structure */
static void
_PyTokenizer_Free(struct tok_state *tok)
{
    qp_sys_free(tok->encoding);
    qp_sys_free(tok->input);
    qp_sys_free(tok->interactive_src_start);
    qp_sys_free(tok);
}

/* Release what a token owns (its metadata). */
static void
_PyToken_Free(struct token *token) {
    qp_sys_free(token->metadata);
    token->metadata = NULL;
    token->metadata_len = 0;
}

static void
_PyToken_Init(struct token *token) {
    token->metadata = NULL;
    token->metadata_len = 0;
}

/* Fill a TYPE_COMMENT / TYPE_IGNORE token with explicit columns. */
static int
_PyLexer_type_comment_token_setup(struct tok_state *tok, struct token *token, int type, int col_offset,
                         int end_col_offset, const char *start, const char *end)
{
    token->level = tok->level;
    token->lineno = token->end_lineno = tok->lineno;
    token->col_offset = col_offset;
    token->end_col_offset = end_col_offset;
    token->start = start;
    token->end = end;
    return type;
}

/* Fill a token from the current tokenizer position. */
static int
_PyLexer_token_setup(struct tok_state *tok, struct token *token, int type, const char *start, const char *end)
{
    assert((start == NULL && end == NULL) || (start != NULL && end != NULL));
    token->level = tok->level;
    if (ISSTRINGLIT(type)) {
        token->lineno = tok->first_lineno;
    }
    else {
        token->lineno = tok->lineno;
    }
    token->end_lineno = tok->lineno;
    token->col_offset = token->end_col_offset = -1;
    token->start = start;
    token->end = end;

    if (start != NULL && end != NULL) {
        token->col_offset = tok->starting_col_offset;
        token->end_col_offset = tok->col_offset;
    }
    return type;
}

/* ############## ERRORS ############## */

/* Convert a 1-based column in bytes into a 1-based column in characters.
   The line is UTF-8 encoded, so it is enough to skip continuation bytes. */
static int
byte_col_to_char_col(const char *line, int byte_col)
{
    int char_col = 1;
    for (int i = 0; i < byte_col - 1; i++) {
        if ((line[i] & 0xC0) != 0x80) {
            char_col++;
        }
    }
    return char_col;
}

/* Raise SyntaxError at the current line with the given (1-based byte, or -1
   = "up to tok->cur") column range; sets tok->done = E_ERROR. */
static int
_syntaxerror_range(struct tok_state *tok, const char *format,
                   int col_offset, int end_col_offset,
                   va_list vargs)
{
    char *errmsg = NULL, *errtext = NULL;
    Py_ssize_t nchars;
    va_list copy;
    int n;

    if (tok->done == E_ERROR) {
        return ERRORTOKEN;
    }
    va_copy(copy, vargs);
    n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (n < 0 || (errmsg = qp_sys_malloc((size_t)n + 1)) == NULL) {
        qp_lex_nomem(tok->err);
        goto error;
    }
    vsnprintf(errmsg, (size_t)n + 1, format, vargs);

    errtext = qp_lex_utf8_replace(tok->line_start, tok->cur - tok->line_start, &nchars);
    if (!errtext) {
        qp_lex_nomem(tok->err);
        goto error;
    }

    if (col_offset == -1) {
        col_offset = (int)nchars;
    }
    else if (col_offset > 0) {
        col_offset = byte_col_to_char_col(tok->line_start, col_offset);
    }
    if (end_col_offset == -1) {
        end_col_offset = col_offset;
    }
    else if (end_col_offset > 0) {
        end_col_offset = byte_col_to_char_col(tok->line_start, end_col_offset);
    }

    Py_ssize_t line_len = strcspn(tok->line_start, "\n");
    if (line_len != tok->cur - tok->line_start) {
        qp_sys_free(errtext);
        errtext = qp_lex_utf8_replace(tok->line_start, line_len, &nchars);
    }
    if (!errtext) {
        qp_lex_nomem(tok->err);
        goto error;
    }

    if (tok->err && tok->err->kind == QP_ERR_NONE) {
        qp_error_set(tok->err, QP_ERR_SYNTAX, tok->lineno, col_offset,
                     tok->lineno, end_col_offset, "%s", errmsg);
        qp_error_set_text(tok->err, errtext, strlen(errtext));
    }

error:
    qp_sys_free(errmsg);
    qp_sys_free(errtext);
    tok->done = E_ERROR;
    return ERRORTOKEN;
}

static int
_PyTokenizer_syntaxerror(struct tok_state *tok, const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    int ret = _syntaxerror_range(tok, format, -1, -1, vargs);
    va_end(vargs);
    return ret;
}

/* Tokenizer/helpers.c _PyTokenizer_warn_invalid_escape_sequence */
static int
_PyTokenizer_warn_invalid_escape_sequence(struct tok_state *tok, int first_invalid_escape_char)
{
    if (!tok->report_warnings || !tok->ctx) {
        return 0;
    }

    char msg[160];
    snprintf(msg, sizeof(msg),
             "\"\\%c\" is an invalid escape sequence. "
             "Such sequences will not work in the future. "
             "Did you mean \"\\\\%c\"? A raw string is also an option.",
             (char) first_invalid_escape_char,
             (char) first_invalid_escape_char);

    int r = qp_syntax_warn(tok->ctx, tok->filename, tok->module, tok->lineno, msg);
    if (r > 0) {
        /* Replace the SyntaxWarning exception with a SyntaxError
           to get a more accurate error report */
        return _PyTokenizer_syntaxerror(tok,
            "\"\\%c\" is an invalid escape sequence. "
            "Did you mean \"\\\\%c\"? A raw string is also an option.",
            (char) first_invalid_escape_char,
            (char) first_invalid_escape_char);
    }
    if (r < 0) {
        qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
        return -1;
    }
    return 0;
}

/* Tokenizer/helpers.c _PyTokenizer_parser_warn (category SyntaxWarning) */
static int
_PyTokenizer_parser_warn(struct tok_state *tok, const char *format, ...)
{
    if (!tok->report_warnings || !tok->ctx) {
        return 0;
    }

    char msg[200];
    va_list vargs;
    va_start(vargs, format);
    vsnprintf(msg, sizeof(msg), format, vargs);
    va_end(vargs);

    int r = qp_syntax_warn(tok->ctx, tok->filename, tok->module, tok->lineno, msg);
    if (r == 0) {
        return 0;
    }
    if (r > 0) {
        /* Replace the SyntaxWarning exception with a SyntaxError
           to get a more accurate error report */
        _PyTokenizer_syntaxerror(tok, "%s", msg);
    }
    else {
        qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
    }
    tok->done = E_ERROR;
    return -1;
}

static int
_PyTokenizer_syntaxerror_known_range(struct tok_state *tok,
                        int col_offset, int end_col_offset,
                        const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    int ret = _syntaxerror_range(tok, format, col_offset, end_col_offset, vargs);
    va_end(vargs);
    return ret;
}

/* Inconsistent tabs/spaces: the parser turns E_TABSPACE into TabError. */
static int
_PyTokenizer_indenterror(struct tok_state *tok)
{
    tok->done = E_TABSPACE;
    tok->cur = tok->inp;
    return ERRORTOKEN;
}

/* Mark a decoding failure; the tokenizer then behaves as at EOF. */
static char *
_PyTokenizer_error_ret(struct tok_state *tok)
{
    tok->decoding_erred = 1;
    tok->buf = tok->cur = tok->inp = NULL;
    tok->start = NULL;
    tok->end = NULL;
    tok->done = E_DECODE;
    return NULL;                /* as if it were EOF */
}

/* ############## STRING MANIPULATION ############## */

static char *
_PyTokenizer_new_string(const char *s, Py_ssize_t len, struct tok_state *tok)
{
    char* result = (char *)qp_sys_malloc(len + 1);
    if (!result) {
        tok->done = E_NOMEM;
        qp_lex_nomem(tok->err);
        return NULL;
    }
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

/* Resolve a declared source encoding like codecs.lookup() does
   (normalizestring + encodings.normalize_encoding + encodings.aliases),
   restricted to the codecs this tokenizer implements.  Returns "utf_8",
   "latin_1", "ascii" or NULL for any other name. */
static const char *
qp_lex_lookup_codec(const char *enc)
{
    char norm[64], alt[64];
    size_t j = 0;
    int punct = 0;
    for (const char *p = enc; *p; p++) {
        int c = Py_TOLOWER(Py_CHARMASK(*p));
        if (Py_ISALNUM(c) || c == '.') {
            if (punct && j > 0) {
                if (j + 1 >= sizeof(norm)) {
                    return NULL;
                }
                norm[j++] = '_';
            }
            if (j + 1 >= sizeof(norm)) {
                return NULL;
            }
            norm[j++] = (char)c;
            punct = 0;
        }
        else {
            punct = 1;
        }
    }
    norm[j] = '\0';
    for (size_t i = 0; i <= j; i++) {
        alt[i] = norm[i] == '.' ? '_' : norm[i];
    }
    size_t n = sizeof(qp_codec_names) / sizeof(qp_codec_names[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(qp_codec_names[i].name, norm) == 0) {
            return qp_codec_names[i].codec;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(qp_codec_names[i].name, alt) == 0) {
            return qp_codec_names[i].codec;
        }
    }
    return NULL;
}

/* translate_into_utf8 for an encoding the tokenizer does not implement
   natively: PyUnicode_Decode through the codec registry (tok->ctx), then
   UTF-8.  LookupError, ValueError (UnicodeDecodeError) and SyntaxError are
   converted like _PyTokenizer_raise_init_error: SyntaxError(str(exc),
   (filename, 0, -1, None)); any other exception is QP_ERR_PENDING. */
static char *
qp_lex_translate_with_codec(struct tok_state *tok, const char *str, size_t len,
                            const char *enc)
{
    JSContext *ctx = tok->ctx;
    JSValue u, exc, msg;
    const char *utf8, *m;
    size_t n;
    char *out;

    u = qp_str_decode(ctx, str, (Py_ssize_t)len, enc, "strict");
    if (JS_IsException(u)) {
        if (!(qp_exc_pending_matches(ctx, QP_EXC(ctx, LookupError)) ||
              qp_exc_pending_matches(ctx, QP_EXC(ctx, SyntaxError)) ||
              qp_exc_pending_matches(ctx, QP_EXC(ctx, ValueError)))) {
            qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
            return NULL;
        }
        exc = qp_exc_fetch(ctx);
        msg = qp_str(ctx, exc);
        JS_FreeValue(ctx, exc);
        if (JS_IsException(msg)) {
            qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
            return NULL;
        }
        m = qp_str_utf8(ctx, msg, NULL);
        if (m) {
            qp_error_set(tok->err, QP_ERR_SYNTAX, 0, -1, 0, 0, "%s", m);
            JS_FreeCString(ctx, m);
        }
        else {
            qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
        }
        JS_FreeValue(ctx, msg);
        return NULL;
    }
    utf8 = qp_str_utf8(ctx, u, &n);
    if (!utf8) {
        JS_FreeValue(ctx, u);
        qp_error_set(tok->err, QP_ERR_PENDING, 0, 0, 0, 0, "%s", "");
        return NULL;
    }
    out = qp_sys_malloc(n + 1);
    if (out == NULL) {
        qp_lex_nomem(tok->err);
    }
    else {
        memcpy(out, utf8, n + 1);
    }
    JS_FreeCString(ctx, utf8);
    JS_FreeValue(ctx, u);
    return out;
}

/* PyUnicode_Decode(str, strlen(str), enc) followed by UTF-8 encoding.
   Returns a malloc'd UTF-8 string, or NULL with the error recorded already
   converted like _PyTokenizer_raise_init_error (SyntaxError, lineno 0,
   offset -1). */
static char *
_PyTokenizer_translate_into_utf8(struct tok_state *tok, const char* str, const char* enc)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t len = strlen(str);
    const char *codec = qp_lex_lookup_codec(enc);
    char *out;

    if (codec == NULL && tok->ctx != NULL) {
        return qp_lex_translate_with_codec(tok, str, len, enc);
    }
    if (codec == NULL) {
        /* LookupError, converted like _PyTokenizer_raise_init_error:
           SyntaxError(str(exc), (filename, 0, -1, None)) */
        if (tok->err) {
            qp_error_set(tok->err, QP_ERR_SYNTAX, 0, -1, 0, 0, "unknown encoding: %s", enc);
        }
        return NULL;
    }
    if (strcmp(codec, "latin_1") == 0) {
        char *q = out = qp_sys_malloc(len * 2 + 1);
        if (out == NULL) {
            qp_lex_nomem(tok->err);
            return NULL;
        }
        for (size_t i = 0; i < len; i++) {
            if (s[i] < 0x80) {
                *q++ = (char)s[i];
            }
            else {
                *q++ = (char)(0xC0 | (s[i] >> 6));
                *q++ = (char)(0x80 | (s[i] & 0x3F));
            }
        }
        *q = '\0';
        return out;
    }
    if (strcmp(codec, "ascii") == 0) {
        for (size_t i = 0; i < len; i++) {
            if (s[i] >= 0x80) {
                if (tok->err) {
                    qp_lex_set_decode_error(tok->err, QP_ERR_SYNTAX, 0, -1, "ascii", s,
                                            (Py_ssize_t)i, 1, "ordinal not in range(128)");
                }
                return NULL;
            }
        }
    }
    else {
        const unsigned char *p = s, *end = s + len;
        while (p < end) {
            uint32_t cp;
            Py_ssize_t errlen;
            const char *reason;
            int k = qp_lex_utf8_step(p, end, &cp, &errlen, &reason);
            if (!k) {
                if (tok->err) {
                    qp_lex_set_decode_error(tok->err, QP_ERR_SYNTAX, 0, -1, "utf-8", s,
                                            p - s, errlen, reason);
                }
                return NULL;
            }
            p += k;
        }
    }
    out = qp_sys_malloc(len + 1);
    if (out == NULL) {
        qp_lex_nomem(tok->err);
        return NULL;
    }
    memcpy(out, str, len + 1);
    return out;
}

/* Copy s translating \r\n and \r to \n (unless preserve_crlf) and, for exec
   input, append a final newline if missing. */
static char *
_PyTokenizer_translate_newlines(const char *s, int exec_input, int preserve_crlf,
                   struct tok_state *tok) {
    int skip_next_lf = 0;
    size_t needed_length = strlen(s) + 2, final_length;
    char *buf, *current;
    char c = '\0';
    buf = qp_sys_malloc(needed_length);
    if (buf == NULL) {
        tok->done = E_NOMEM;
        qp_lex_nomem(tok->err);
        return NULL;
    }
    for (current = buf; *s; s++, current++) {
        c = *s;
        if (skip_next_lf) {
            skip_next_lf = 0;
            if (c == '\n') {
                c = *++s;
                if (!c)
                    break;
            }
        }
        if (!preserve_crlf && c == '\r') {
            skip_next_lf = 1;
            c = '\n';
        }
        *current = c;
    }
    /* If this is exec input, add a newline to the end of the string if
       there isn't one already. */
    if (exec_input && c != '\n' && c != '\0') {
        *current = '\n';
        current++;
    }
    *current = '\0';
    final_length = current - buf + 1;
    if (final_length < needed_length && final_length) {
        /* should never fail */
        char* result = qp_sys_realloc(buf, final_length);
        if (result == NULL) {
            qp_sys_free(buf);
            tok->done = E_NOMEM;
            qp_lex_nomem(tok->err);
        }
        buf = result;
    }
    return buf;
}

/* ############## ENCODING STUFF ############## */

/* Fetch a byte from TOK, using the string buffer. */
static int
buf_getc(struct tok_state *tok) {
    return Py_CHARMASK(*tok->str++);
}

/* Unfetch a byte from TOK, using the string buffer. */
static void
buf_ungetc(int c, struct tok_state *tok) {
    tok->str--;
    assert(Py_CHARMASK(*tok->str) == c);
    (void)c;
}

/* Set the readline function for TOK to ENC. For the string-based
   tokenizer, this means to just record the encoding. */
static int
buf_setreadl(struct tok_state *tok, const char* enc) {
    tok->enc = enc;
    return 1;
}

/* See whether the string starts with a UTF-8 BOM; if so skip it and record
   "utf-8" as the encoding.  Return 1 on success, 0 on failure.  */
static int
_PyTokenizer_check_bom(struct tok_state *tok)
{
    int ch1, ch2, ch3;
    ch1 = buf_getc(tok);
    tok->decoding_state = STATE_SEEK_CODING;
    if (ch1 == EOF) {
        return 1;
    } else if (ch1 == 0xEF) {
        ch2 = buf_getc(tok);
        if (ch2 != 0xBB) {
            buf_ungetc(ch2, tok);
            buf_ungetc(ch1, tok);
            return 1;
        }
        ch3 = buf_getc(tok);
        if (ch3 != 0xBF) {
            buf_ungetc(ch3, tok);
            buf_ungetc(ch2, tok);
            buf_ungetc(ch1, tok);
            return 1;
        }
    } else {
        buf_ungetc(ch1, tok);
        return 1;
    }
    qp_sys_free(tok->encoding);
    tok->encoding = _PyTokenizer_new_string("utf-8", 5, tok);
    if (!tok->encoding)
        return 0;
    /* No need to set_readline: input is already utf-8 */
    return 1;
}

static const char *
get_normal_name(const char *s)  /* for utf-8 and latin-1 */
{
    char buf[13];
    int i;
    for (i = 0; i < 12; i++) {
        int c = s[i];
        if (c == '\0')
            break;
        else if (c == '_')
            buf[i] = '-';
        else
            buf[i] = Py_TOLOWER(c);
    }
    buf[i] = '\0';
    if (strcmp(buf, "utf-8") == 0 ||
        strncmp(buf, "utf-8-", 6) == 0)
        return "utf-8";
    else if (strcmp(buf, "latin-1") == 0 ||
             strcmp(buf, "iso-8859-1") == 0 ||
             strcmp(buf, "iso-latin-1") == 0 ||
             strncmp(buf, "latin-1-", 8) == 0 ||
             strncmp(buf, "iso-8859-1-", 11) == 0 ||
             strncmp(buf, "iso-latin-1-", 12) == 0)
        return "iso-8859-1";
    else
        return s;
}

/* Return the coding spec in S, or NULL if none is found.  */
static int
get_coding_spec(const char *s, char **spec, Py_ssize_t size, struct tok_state *tok)
{
    Py_ssize_t i;
    *spec = NULL;
    /* Coding spec must be in a comment, and that comment must be
     * the only statement on the source code line. */
    for (i = 0; i < size - 6; i++) {
        if (s[i] == '#')
            break;
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\014')
            return 1;
    }
    for (; i < size - 6; i++) { /* XXX inefficient search */
        const char* t = s + i;
        if (memcmp(t, "coding", 6) == 0) {
            const char* begin = NULL;
            t += 6;
            if (t[0] != ':' && t[0] != '=')
                continue;
            do {
                t++;
            } while (t[0] == ' ' || t[0] == '\t');

            begin = t;
            while (Py_ISALNUM(t[0]) ||
                   t[0] == '-' || t[0] == '_' || t[0] == '.')
                t++;

            if (begin < t) {
                char* r = _PyTokenizer_new_string(begin, t - begin, tok);
                const char* q;
                if (!r)
                    return 0;
                q = get_normal_name(r);
                if (r != q) {
                    qp_sys_free(r);
                    r = _PyTokenizer_new_string(q, strlen(q), tok);
                    if (!r)
                        return 0;
                }
                *spec = r;
                break;
            }
        }
    }
    return 1;
}

/* Check whether the line contains a coding spec. If it does, record the
   new encoding.  Return 1 on success, 0 on failure.  */
static int
_PyTokenizer_check_coding_spec(const char* line, Py_ssize_t size, struct tok_state *tok)
{
    char *cs;
    if (tok->cont_line) {
        /* It's a continuation line, so it can't be a coding spec. */
        tok->decoding_state = STATE_NORMAL;
        return 1;
    }
    if (!get_coding_spec(line, &cs, size, tok)) {
        return 0;
    }
    if (!cs) {
        Py_ssize_t i;
        for (i = 0; i < size; i++) {
            if (line[i] == '#' || line[i] == '\n' || line[i] == '\r')
                break;
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\014') {
                /* Stop checking coding spec after a line containing
                 * anything except a comment. */
                tok->decoding_state = STATE_NORMAL;
                break;
            }
        }
        return 1;
    }
    tok->decoding_state = STATE_NORMAL;
    if (tok->encoding == NULL) {
        if (strcmp(cs, "utf-8") != 0) {
            buf_setreadl(tok, cs);
        }
        tok->encoding = cs;
    } else {                /* then, compare cs with BOM */
        if (strcmp(tok->encoding, cs) != 0) {
            tok->line_start = line;
            tok->cur = (char *)line;
            assert(size <= INT_MAX);
            _PyTokenizer_syntaxerror_known_range(tok, 0, (int)size,
                        "encoding problem: %s with BOM", cs);
            qp_sys_free(cs);
            _PyTokenizer_error_ret(tok);
            return 0;
        }
        qp_sys_free(cs);
    }
    return 1;
}

/* Check whether the characters at s start a valid
   UTF-8 sequence. Return the number of characters forming
   the sequence if yes, 0 if not.  The special cases match
   those in stringlib/codecs.h:utf8_decode.
*/
static int
valid_utf8(const unsigned char* s)
{
    int expected = 0;
    int length;
    if (*s < 0x80) {
        /* single-byte code */
        return 1;
    }
    else if (*s < 0xE0) {
        /* \xC2\x80-\xDF\xBF -- 0080-07FF */
        if (*s < 0xC2) {
            /* invalid sequence
               \x80-\xBF -- continuation byte
               \xC0-\xC1 -- fake 0000-007F */
            return 0;
        }
        expected = 1;
    }
    else if (*s < 0xF0) {
        /* \xE0\xA0\x80-\xEF\xBF\xBF -- 0800-FFFF */
        if (*s == 0xE0 && *(s + 1) < 0xA0) {
            /* invalid sequence
               \xE0\x80\x80-\xE0\x9F\xBF -- fake 0000-0800 */
            return 0;
        }
        else if (*s == 0xED && *(s + 1) >= 0xA0) {
            /* Surrogates (\xED\xA0\x80-\xED\xBF\xBF) are not valid UTF-8. */
            return 0;
        }
        expected = 2;
    }
    else if (*s < 0xF5) {
        /* \xF0\x90\x80\x80-\xF4\x8F\xBF\xBF -- 10000-10FFFF */
        if (*(s + 1) < 0x90 ? *s == 0xF0 : *s == 0xF4) {
            /* invalid sequence -- one of:
               \xF0\x80\x80\x80-\xF0\x8F\xBF\xBF -- fake 0000-FFFF
               \xF4\x90\x80\x80- -- 110000- overflow */
            return 0;
        }
        expected = 3;
    }
    else {
        /* invalid start byte */
        return 0;
    }
    length = expected + 1;
    for (int i = 1; i <= expected; i++) {
        if (s[i] < 0x80 || s[i] >= 0xC0) {
            return 0;
        }
    }
    return length;
}

/* Raise CPython's "Non-UTF-8 code ..." SyntaxError for the first invalid
   UTF-8 sequence of a source without encoding declaration. */
static int
_PyTokenizer_ensure_utf8(const char *line, struct tok_state *tok, int lineno)
{
    const char *badchar = NULL;
    const char *c;
    int length;
    const char *line_start = line;
    for (c = line; *c; c += length) {
        if (!(length = valid_utf8((const unsigned char *)c))) {
            badchar = c;
            break;
        }
        if (*c == '\n') {
            lineno++;
            line_start = c + 1;
        }
    }
    if (badchar) {
        tok->lineno = lineno;
        tok->line_start = line_start;
        tok->cur = (char *)badchar;
        _PyTokenizer_syntaxerror_known_range(tok,
                (int)(badchar - line_start) + 1,
                (int)(badchar - line_start) + 1,
                "Non-UTF-8 code starting with '\\x%.2x'"
                "%s%s on line %i, "
                "but no encoding declared; "
                "see https://peps.python.org/pep-0263/ for details",
                (unsigned char)*badchar,
                tok->filename ? " in file " : "", tok->filename ? tok->filename : "",
                lineno);
        return 0;
    }
    return 1;
}

/* ############## STRING TOKENIZERS ############## */

/* Make the next line of the in-memory source available in [tok->cur, tok->inp). */
static int
tok_underflow_string(struct tok_state *tok) {
    char *end = strchr(tok->inp, '\n');
    if (end != NULL) {
        end++;
    }
    else {
        end = strchr(tok->inp, '\0');
        /* the null byte line starts with it: an empty line, not EOF */
        if (end == tok->inp && tok->lineno + 1 != tok->qp_nul_lineno) {
            tok->done = E_EOF;
            return 0;
        }
    }
    if (tok->start == NULL) {
        tok->buf = tok->cur;
    }
    tok->line_start = tok->cur;
    ADVANCE_LINENO();
    tok->inp = end;
    return 1;
}

/* Decode a byte string STR for use as the buffer of TOK.
   Look for encoding declarations inside STR, and record them
   inside TOK.  */
static char *
decode_str(const char *input, int single, struct tok_state *tok, int preserve_crlf)
{
    char *utf8 = NULL;
    char *str;
    const char *s;
    const char *newl[2] = {NULL, NULL};
    int lineno = 0;
    tok->input = str = _PyTokenizer_translate_newlines(input, single, preserve_crlf, tok);
    if (str == NULL)
        return NULL;
    tok->enc = NULL;
    tok->str = str;
    if (!_PyTokenizer_check_bom(tok))
        return _PyTokenizer_error_ret(tok);
    str = tok->str;             /* string after BOM if any */
    assert(str);
    if (tok->enc != NULL) {
        utf8 = _PyTokenizer_translate_into_utf8(tok, str, tok->enc);
        if (utf8 == NULL)
            return _PyTokenizer_error_ret(tok);
        str = utf8;
    }
    for (s = str;; s++) {
        if (*s == '\0') break;
        else if (*s == '\n') {
            assert(lineno < 2);
            newl[lineno] = s;
            lineno++;
            if (lineno == 2) break;
        }
    }
    tok->enc = NULL;
    /* need to check line 1 and 2 separately since check_coding_spec
       assumes a single line as input */
    if (newl[0]) {
        tok->lineno = 1;
        if (!_PyTokenizer_check_coding_spec(str, newl[0] - str, tok)) {
            qp_sys_free(utf8);
            return NULL;
        }
        if (tok->enc == NULL && tok->decoding_state != STATE_NORMAL && newl[1]) {
            tok->lineno = 2;
            if (!_PyTokenizer_check_coding_spec(newl[0]+1, newl[1] - newl[0], tok)) {
                qp_sys_free(utf8);
                return NULL;
            }
        }
    }
    tok->lineno = 0;
    if (tok->enc != NULL) {
        assert(utf8 == NULL);
        utf8 = _PyTokenizer_translate_into_utf8(tok, str, tok->enc);
        if (utf8 == NULL)
            return _PyTokenizer_error_ret(tok);
        str = utf8;
    }
    else if (!_PyTokenizer_ensure_utf8(str, tok, 1)) {
        qp_sys_free(utf8);
        return _PyTokenizer_error_ret(tok);
    }
    if (utf8 != NULL) {
        char *translated = _PyTokenizer_translate_newlines(
            str, single, preserve_crlf, tok);
        qp_sys_free(utf8);
        if (translated == NULL) {
            return _PyTokenizer_error_ret(tok);
        }
        qp_sys_free(tok->input);
        tok->input = translated;
        str = translated;
    }
    tok->str = str;
    return str;
}

/* Set up tokenizer for a byte string (BOM / coding cookie detection). */
static struct tok_state *
_PyTokenizer_FromString(const char *str, int exec_input, int preserve_crlf, qp_error *err,
                        JSContext *ctx)
{
    struct tok_state *tok = _PyTokenizer_tok_new(err);
    char *decoded;

    if (tok == NULL)
        return NULL;
    tok->ctx = ctx;
    decoded = decode_str(str, exec_input, tok, preserve_crlf);
    if (decoded == NULL) {
        _PyTokenizer_Free(tok);
        return NULL;
    }

    tok->buf = tok->cur = tok->inp = decoded;
    tok->end = decoded;
    tok->underflow = &tok_underflow_string;
    return tok;
}

/* Set up tokenizer for a UTF-8 string (str source; no cookie handling). */
static struct tok_state *
_PyTokenizer_FromUTF8(const char *str, int exec_input, int preserve_crlf, qp_error *err)
{
    struct tok_state *tok = _PyTokenizer_tok_new(err);
    char *translated;
    if (tok == NULL)
        return NULL;
    tok->input = translated = _PyTokenizer_translate_newlines(str, exec_input, preserve_crlf, tok);
    if (translated == NULL) {
        _PyTokenizer_Free(tok);
        return NULL;
    }
    tok->decoding_state = STATE_NORMAL;
    tok->enc = NULL;
    tok->str = translated;
    tok->encoding = _PyTokenizer_new_string("utf-8", 5, tok);
    if (!tok->encoding) {
        _PyTokenizer_Free(tok);
        return NULL;
    }

    tok->buf = tok->cur = tok->inp = translated;
    tok->end = translated;
    tok->underflow = &tok_underflow_string;
    return tok;
}

/* ############## LEXER ############## */

#define is_potential_identifier_start(c) (\
              (c >= 'a' && c <= 'z')\
               || (c >= 'A' && c <= 'Z')\
               || c == '_'\
               || (c >= 128))

#define is_potential_identifier_char(c) (\
              (c >= 'a' && c <= 'z')\
               || (c >= 'A' && c <= 'Z')\
               || (c >= '0' && c <= '9')\
               || c == '_'\
               || (c >= 128))

#define TOK_GET_MODE(tok) (&(tok->tok_mode_stack[tok->tok_mode_stack_index]))
#define TOK_NEXT_MODE(tok) (&(tok->tok_mode_stack[++tok->tok_mode_stack_index]))

#define FTSTRING_MIDDLE(tok_mode) (tok_mode->string_kind == TSTRING ? TSTRING_MIDDLE : FSTRING_MIDDLE)
#define FTSTRING_END(tok_mode) (tok_mode->string_kind == TSTRING ? TSTRING_END : FSTRING_END)
#define TOK_GET_STRING_PREFIX(tok) (TOK_GET_MODE(tok)->string_kind == TSTRING ? 't' : 'f')
#define MAKE_TOKEN(token_type) _PyLexer_token_setup(tok, token, token_type, p_start, p_end)
#define MAKE_TYPE_COMMENT_TOKEN(token_type, col_offset, end_col_offset) (\
                _PyLexer_type_comment_token_setup(tok, token, token_type, col_offset, end_col_offset, p_start, p_end))

/* Spaces in this constant are treated as "zero or more spaces or tabs" when
   tokenizing. */
static const char* type_comment_prefix = "# type: ";

static inline int
contains_null_bytes(const char* str, size_t size)
{
    return memchr(str, 0, size) != NULL;
}

/* Get next char, updating state; error code goes into tok->done */
static int
tok_nextc(struct tok_state *tok)
{
    int rc;
    for (;;) {
        if (tok->cur != tok->inp) {
            if ((unsigned int) tok->col_offset >= (unsigned int) INT_MAX) {
                tok->done = E_COLUMNOVERFLOW;
                return EOF;
            }
            tok->col_offset++;
            return Py_CHARMASK(*tok->cur++); /* Fast path */
        }
        if (tok->done != E_OK) {
            return EOF;
        }
        rc = tok->underflow(tok);
        if (!rc) {
            tok->cur = tok->inp;
            return EOF;
        }
        tok->line_start = tok->cur;

        if (contains_null_bytes(tok->line_start, tok->inp - tok->line_start) ||
            (tok->qp_nul_lineno && tok->lineno == tok->qp_nul_lineno)) {
            _PyTokenizer_syntaxerror(tok, "source code cannot contain null bytes");
            tok->cur = tok->inp;
            return EOF;
        }
    }
    Py_UNREACHABLE();
}

/* Back-up one character */
static void
tok_backup(struct tok_state *tok, int c)
{
    if (c != EOF) {
        if (--tok->cur < tok->buf) {
            fprintf(stderr, "Fatal Python error: tokenizer beginning of buffer\n");
            abort();
        }
        if ((int)(unsigned char)*tok->cur != Py_CHARMASK(c)) {
            fprintf(stderr, "Fatal Python error: tok_backup: wrong character\n");
            abort();
        }
        tok->col_offset--;
    }
}

/* Store the source text of the f-string debug expression / t-string
   interpolation in token->metadata (comments stripped). */
static int
set_ftstring_expr(struct tok_state* tok, struct token *token, char c) {
    assert(token != NULL);
    assert(c == '}' || c == ':' || c == '!');
    (void)c;
    tokenizer_mode *tok_mode = TOK_GET_MODE(tok);

    if (!(tok_mode->in_debug || tok_mode->string_kind == TSTRING) || token->metadata) {
        return 0;
    }
    const char *expression = tok_mode->last_expr_start;
    assert(expression != NULL);
    assert(expression <= tok->start);
    Py_ssize_t expression_size = tok->start - expression;
    char *res = NULL;
    Py_ssize_t res_len;

    // Look for a # character outside of string literals
    int hash_detected = 0;
    int in_string = 0;
    char quote_char = 0;

    for (Py_ssize_t i = 0; i < expression_size; i++) {
        char ch = expression[i];

        // Skip escaped characters
        if (ch == '\\') {
            i++;
            continue;
        }

        // Handle quotes
        if (ch == '"' || ch == '\'') {
            // There is an odd number of quotes in STRING tokens and the lexer
            // only ever reaches this function with valid STRING tokens.
            if (!in_string) {
                in_string = 1;
                quote_char = ch;
            }
            else if (ch == quote_char) {
                in_string = 0;
            }
            continue;
        }

        // Check for # outside strings
        if (ch == '#' && !in_string) {
            hash_detected = 1;
            break;
        }
    }
    // If we found a # character in the expression, we need to handle comments
    if (hash_detected) {
        // Allocate buffer for processed result
        char *result = (char *)qp_sys_malloc((expression_size + 1) * sizeof(char));
        if (!result) {
            qp_lex_nomem(tok->err);
            return -1;
        }

        Py_ssize_t i = 0;  // Input position
        Py_ssize_t j = 0;  // Output position
        in_string = 0;     // Whether we're in a string
        quote_char = 0;    // Current string quote char

        // Process each character
        while (i < expression_size) {
            char ch = expression[i];

            // Copy escaped characters without interpreting the escaped
            // character as a quote or comment marker.
            if (ch == '\\') {
                result[j++] = ch;
                i++;
                if (i < expression_size) {
                    result[j++] = expression[i];
                }
            }
            // Handle string quotes
            else if (ch == '"' || ch == '\'') {
                if (!in_string) {
                    in_string = 1;
                    quote_char = ch;
                } else if (ch == quote_char) {
                    in_string = 0;
                }
                result[j++] = ch;
            }
            // Skip comments
            else if (ch == '#' && !in_string) {
                while (i < expression_size &&
                       expression[i] != '\n') {
                    i++;
                }
                if (i < expression_size) {
                    result[j++] = '\n';
                }
            }
            // Copy other chars
            else {
                result[j++] = ch;
            }
            i++;
        }

        result[j] = '\0';  // Null-terminate the result string
        res = result;
        res_len = j;
    } else {
        res = _PyTokenizer_new_string(expression, expression_size, tok);
        if (!res) {
            return -1;
        }
        res_len = expression_size;
    }

    token->metadata = res;
    token->metadata_len = res_len;
    return 0;
}

static int
lookahead(struct tok_state *tok, const char *test)
{
    const char *s = test;
    int res = 0;
    while (1) {
        int c = tok_nextc(tok);
        if (*s == 0) {
            res = !is_potential_identifier_char(c);
        }
        else if (c == *s) {
            s++;
            continue;
        }

        tok_backup(tok, c);
        while (s != test) {
            tok_backup(tok, *--s);
        }
        return res;
    }
}

/* Check what follows a numeric literal: a keyword that may legally follow a
   number gets the "invalid <kind> literal" SyntaxWarning; any other
   identifier character is a SyntaxError. */
static int
verify_end_of_number(struct tok_state *tok, int c, const char *kind) {
    if (tok->tok_extra_tokens) {
        // When we are parsing extra tokens, we don't want to emit warnings
        // about invalid literals, because we want to be a bit more liberal.
        return 1;
    }
    int r = 0;
    if (c == 'a') {
        r = lookahead(tok, "nd");
    }
    else if (c == 'e') {
        r = lookahead(tok, "lse");
    }
    else if (c == 'f') {
        r = lookahead(tok, "or");
    }
    else if (c == 'i') {
        int c2 = tok_nextc(tok);
        if (c2 == 'f' || c2 == 'n' || c2 == 's') {
            r = 1;
        }
        tok_backup(tok, c2);
    }
    else if (c == 'o') {
        r = lookahead(tok, "r");
    }
    else if (c == 'n') {
        r = lookahead(tok, "ot");
    }
    if (r) {
        tok_backup(tok, c);
        if (_PyTokenizer_parser_warn(tok, "invalid %s literal", kind))
        {
            return 0;
        }
        tok_nextc(tok);
    }
    else /* In future releases, only error will remain. */
    if (c < 128 && is_potential_identifier_char(c)) {
        tok_backup(tok, c);
        _PyTokenizer_syntaxerror(tok, "invalid %s literal", kind);
        return 0;
    }
    return 1;
}

/* Verify that the identifier follows PEP 3131. */
static int
verify_identifier(struct tok_state *tok)
{
    if (tok->tok_extra_tokens) {
        return 1;
    }
    if (tok->decoding_erred)
        return 0;

    const unsigned char *s = (const unsigned char *)tok->start;
    const unsigned char *end = (const unsigned char *)tok->cur;
    const unsigned char *p = s;
    const unsigned char *after_invalid = NULL;
    Py_ssize_t index = 0, invalid = -1, len = 0;
    uint32_t ch = 0;

    /* PyUnicode_DecodeUTF8 + _PyUnicode_ScanIdentifier in one pass */
    while (p < end) {
        uint32_t cp;
        Py_ssize_t errlen;
        const char *reason;
        int k = qp_lex_utf8_step(p, end, &cp, &errlen, &reason);
        if (!k) {
            if (tok->err) {
                qp_lex_set_decode_error(tok->err, QP_ERR_UNICODE_DECODE, 0, 0, "utf-8", s,
                                        p - s, errlen, reason);
            }
            tok->done = E_DECODE;
            return 0;
        }
        if (invalid < 0) {
            int ok = index == 0 ? (qp_lex_is_xid_start(cp) || cp == 0x5F)
                                : qp_lex_is_xid_continue(cp);
            if (!ok) {
                invalid = index;
                ch = cp;
                after_invalid = p + k;
            }
        }
        p += k;
        index++;
    }
    len = index;
    assert(len > 0);
    if (invalid >= 0) {
        char buf[8];
        if (invalid + 1 < len) {
            /* Determine the offset in UTF-8 encoded input */
            tok->cur = (char *)after_invalid;
        }
        if (Py_UNICODE_ISPRINTABLE(ch)) {
            qp_lex_utf8_encode(ch, buf);
            _PyTokenizer_syntaxerror(tok, "invalid character '%s' (U+%04X)", buf, (unsigned)ch);
        }
        else {
            _PyTokenizer_syntaxerror(tok, "invalid non-printable character U+%04X", (unsigned)ch);
        }
        return 0;
    }
    return 1;
}

static int
tok_decimal_tail(struct tok_state *tok)
{
    int c;

    while (1) {
        do {
            c = tok_nextc(tok);
        } while (Py_ISDIGIT(c));
        if (c != '_') {
            break;
        }
        c = tok_nextc(tok);
        if (!Py_ISDIGIT(c)) {
            tok_backup(tok, c);
            _PyTokenizer_syntaxerror(tok, "invalid decimal literal");
            return 0;
        }
    }
    return c;
}

static inline int
tok_continuation_line(struct tok_state *tok) {
    int c = tok_nextc(tok);
    if (c == '\r') {
        c = tok_nextc(tok);
    }
    if (c != '\n') {
        tok->done = E_LINECONT;
        return -1;
    }
    c = tok_nextc(tok);
    if (c == EOF) {
        tok->done = E_EOF;
        tok->cur = tok->inp;
        return -1;
    } else {
        tok_backup(tok, c);
    }
    return c;
}

static int
maybe_raise_syntax_error_for_string_prefixes(struct tok_state *tok,
                                             int saw_b, int saw_r, int saw_u,
                                             int saw_f, int saw_t) {
    // Supported: rb, rf, rt (in any order)
    // Unsupported: ub, ur, uf, ut, bf, bt, ft (in any order)

#define RETURN_SYNTAX_ERROR(PREFIX1, PREFIX2)                             \
    do {                                                                  \
        (void)_PyTokenizer_syntaxerror_known_range(                       \
            tok, (int)(tok->start + 1 - tok->line_start),                 \
            (int)(tok->cur - tok->line_start),                            \
            "'" PREFIX1 "' and '" PREFIX2 "' prefixes are incompatible"); \
        return -1;                                                        \
    } while (0)

    if (saw_u && saw_b) {
        RETURN_SYNTAX_ERROR("u", "b");
    }
    if (saw_u && saw_r) {
        RETURN_SYNTAX_ERROR("u", "r");
    }
    if (saw_u && saw_f) {
        RETURN_SYNTAX_ERROR("u", "f");
    }
    if (saw_u && saw_t) {
        RETURN_SYNTAX_ERROR("u", "t");
    }

    if (saw_b && saw_f) {
        RETURN_SYNTAX_ERROR("b", "f");
    }
    if (saw_b && saw_t) {
        RETURN_SYNTAX_ERROR("b", "t");
    }

    if (saw_f && saw_t) {
        RETURN_SYNTAX_ERROR("f", "t");
    }

#undef RETURN_SYNTAX_ERROR

    return 0;
}

static int
tok_get_normal_mode(struct tok_state *tok, tokenizer_mode* current_tok, struct token *token)
{
    int c;
    int blankline, nonascii;

    const char *p_start = NULL;
    const char *p_end = NULL;
  nextline:
    tok->start = NULL;
    tok->starting_col_offset = -1;
    blankline = 0;


    /* Get indentation level */
    if (tok->atbol) {
        int col = 0;
        int altcol = 0;
        tok->atbol = 0;
        int cont_line_col = 0;
        for (;;) {
            c = tok_nextc(tok);
            if (c == ' ') {
                col++, altcol++;
            }
            else if (c == '\t') {
                col = (col / tok->tabsize + 1) * tok->tabsize;
                altcol = (altcol / ALTTABSIZE + 1) * ALTTABSIZE;
            }
            else if (c == '\014')  {/* Control-L (formfeed) */
                col = altcol = 0; /* For Emacs users */
            }
            else if (c == '\\') {
                // Indentation cannot be split over multiple physical lines
                // using backslashes. This means that if we found a backslash
                // preceded by whitespace, **the first one we find** determines
                // the level of indentation of whatever comes next.
                cont_line_col = cont_line_col ? cont_line_col : col;
                if ((c = tok_continuation_line(tok)) == -1) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
            else if (c == EOF && qp_lex_err_occurred(tok)) {
                return MAKE_TOKEN(ERRORTOKEN);
            }
            else {
                break;
            }
        }
        tok_backup(tok, c);
        if (c == '#' || c == '\n' || c == '\r') {
            /* Lines with only whitespace and/or comments
               shouldn't affect the indentation and are
               not passed to the parser as NEWLINE tokens,
               except *totally* empty lines in interactive
               mode, which signal the end of a command group. */
            if (col == 0 && c == '\n' && tok->prompt != NULL) {
                blankline = 0; /* Let it through */
            }
            else if (tok->prompt != NULL && tok->lineno == 1) {
                /* In interactive mode, if the first line contains
                   only spaces and/or a comment, let it through. */
                blankline = 0;
                col = altcol = 0;
            }
            else {
                blankline = 1; /* Ignore completely */
            }
            /* We can't jump back right here since we still
               may need to skip to the end of a comment */
        }
        if (!blankline && tok->level == 0) {
            col = cont_line_col ? cont_line_col : col;
            altcol = cont_line_col ? cont_line_col : altcol;
            if (col == tok->indstack[tok->indent]) {
                /* No change */
                if (altcol != tok->altindstack[tok->indent]) {
                    return MAKE_TOKEN(_PyTokenizer_indenterror(tok));
                }
            }
            else if (col > tok->indstack[tok->indent]) {
                /* Indent -- always one */
                if (tok->indent+1 >= MAXINDENT) {
                    tok->done = E_TOODEEP;
                    tok->cur = tok->inp;
                    return MAKE_TOKEN(ERRORTOKEN);
                }
                if (altcol <= tok->altindstack[tok->indent]) {
                    return MAKE_TOKEN(_PyTokenizer_indenterror(tok));
                }
                tok->pendin++;
                tok->indstack[++tok->indent] = col;
                tok->altindstack[tok->indent] = altcol;
            }
            else /* col < tok->indstack[tok->indent] */ {
                /* Dedent -- any number, must be consistent */
                while (tok->indent > 0 &&
                    col < tok->indstack[tok->indent]) {
                    tok->pendin--;
                    tok->indent--;
                }
                if (col != tok->indstack[tok->indent]) {
                    tok->done = E_DEDENT;
                    tok->cur = tok->inp;
                    return MAKE_TOKEN(ERRORTOKEN);
                }
                if (altcol != tok->altindstack[tok->indent]) {
                    return MAKE_TOKEN(_PyTokenizer_indenterror(tok));
                }
            }
        }
    }

    tok->start = tok->cur;
    tok->starting_col_offset = tok->col_offset;

    /* Return pending indents/dedents */
    if (tok->pendin != 0) {
        if (tok->pendin < 0) {
            if (tok->tok_extra_tokens) {
                p_start = tok->cur;
                p_end = tok->cur;
            }
            tok->pendin++;
            return MAKE_TOKEN(DEDENT);
        }
        else {
            if (tok->tok_extra_tokens) {
                p_start = tok->buf;
                p_end = tok->cur;
            }
            tok->pendin--;
            return MAKE_TOKEN(INDENT);
        }
    }

    /* Peek ahead at the next character */
    c = tok_nextc(tok);
    tok_backup(tok, c);

 again:
    tok->start = NULL;
    /* Skip spaces */
    do {
        c = tok_nextc(tok);
    } while (c == ' ' || c == '\t' || c == '\014');

    /* Set start of current token */
    tok->start = tok->cur == NULL ? NULL : tok->cur - 1;
    tok->starting_col_offset = tok->col_offset - 1;

    /* Skip comment, unless it's a type comment */
    if (c == '#') {

        const char* p = NULL;
        const char *prefix, *type_start;
        int current_starting_col_offset;

        while (c != EOF && c != '\n' && c != '\r') {
            c = tok_nextc(tok);
        }

        if (tok->tok_extra_tokens) {
            p = tok->start;
        }

        if (tok->type_comments) {
            p = tok->start;
            current_starting_col_offset = tok->starting_col_offset;
            prefix = type_comment_prefix;
            while (*prefix && p < tok->cur) {
                if (*prefix == ' ') {
                    while (*p == ' ' || *p == '\t') {
                        p++;
                        current_starting_col_offset++;
                    }
                } else if (*prefix == *p) {
                    p++;
                    current_starting_col_offset++;
                } else {
                    break;
                }

                prefix++;
            }

            /* This is a type comment if we matched all of type_comment_prefix. */
            if (!*prefix) {
                int is_type_ignore = 1;
                // +6 in order to skip the word 'ignore'
                const char *ignore_end = p + 6;
                const int ignore_end_col_offset = current_starting_col_offset + 6;
                tok_backup(tok, c);  /* don't eat the newline or EOF */

                type_start = p;

                /* A TYPE_IGNORE is "type: ignore" followed by the end of the token
                 * or anything ASCII and non-alphanumeric. */
                is_type_ignore = (
                    tok->cur >= ignore_end && memcmp(p, "ignore", 6) == 0
                    && !(tok->cur > ignore_end
                         && ((unsigned char)ignore_end[0] >= 128 || Py_ISALNUM(ignore_end[0]))));

                if (is_type_ignore) {
                    p_start = ignore_end;
                    p_end = tok->cur;

                    /* If this type ignore is the only thing on the line, consume the newline also. */
                    if (blankline) {
                        tok_nextc(tok);
                        tok->atbol = 1;
                    }
                    return MAKE_TYPE_COMMENT_TOKEN(TYPE_IGNORE, ignore_end_col_offset, tok->col_offset);
                } else {
                    p_start = type_start;
                    p_end = tok->cur;
                    return MAKE_TYPE_COMMENT_TOKEN(TYPE_COMMENT, current_starting_col_offset, tok->col_offset);
                }
            }
        }
        if (tok->tok_extra_tokens) {
            tok_backup(tok, c);  /* don't eat the newline or EOF */
            p_start = p;
            p_end = tok->cur;
            tok->comment_newline = blankline;
            return MAKE_TOKEN(COMMENT);
        }
    }

    if (tok->done == E_INTERACT_STOP) {
        return MAKE_TOKEN(ENDMARKER);
    }

    /* Check for EOF and errors now */
    if (c == EOF) {
        if (tok->level) {
            return MAKE_TOKEN(ERRORTOKEN);
        }
        return MAKE_TOKEN(tok->done == E_EOF ? ENDMARKER : ERRORTOKEN);
    }

    /* Identifier (most frequent token!) */
    nonascii = 0;
    if (is_potential_identifier_start(c)) {
        /* Process the various legal combinations of b"", r"", u"", and f"". */
        int saw_b = 0, saw_r = 0, saw_u = 0, saw_f = 0, saw_t = 0;
        while (1) {
            if (!saw_b && (c == 'b' || c == 'B')) {
                saw_b = 1;
            }
            /* Since this is a backwards compatibility support literal we don't
               want to support it in arbitrary order like byte literals. */
            else if (!saw_u && (c == 'u'|| c == 'U')) {
                saw_u = 1;
            }
            /* ur"" and ru"" are not supported */
            else if (!saw_r && (c == 'r' || c == 'R')) {
                saw_r = 1;
            }
            else if (!saw_f && (c == 'f' || c == 'F')) {
                saw_f = 1;
            }
            else if (!saw_t && (c == 't' || c == 'T')) {
                saw_t = 1;
            }
            else {
                break;
            }
            c = tok_nextc(tok);
            if (c == '"' || c == '\'') {
                // Raise error on incompatible string prefixes:
                int status = maybe_raise_syntax_error_for_string_prefixes(
                    tok, saw_b, saw_r, saw_u, saw_f, saw_t);
                if (status < 0) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }

                // Handle valid f or t string creation:
                if (saw_f || saw_t) {
                    goto f_string_quote;
                }
                goto letter_quote;
            }
        }
        while (is_potential_identifier_char(c)) {
            if (c >= 128) {
                nonascii = 1;
            }
            c = tok_nextc(tok);
        }
        tok_backup(tok, c);
        if (nonascii && !verify_identifier(tok)) {
            return MAKE_TOKEN(ERRORTOKEN);
        }

        p_start = tok->start;
        p_end = tok->cur;

        return MAKE_TOKEN(NAME);
    }

    if (c == '\r') {
        c = tok_nextc(tok);
    }

    /* Newline */
    if (c == '\n') {
        tok->atbol = 1;
        if (blankline || tok->level > 0) {
            if (tok->tok_extra_tokens) {
                if (tok->comment_newline) {
                    tok->comment_newline = 0;
                }
                p_start = tok->start;
                p_end = tok->cur;
                return MAKE_TOKEN(NL);
            }
            goto nextline;
        }
        if (tok->comment_newline && tok->tok_extra_tokens) {
            tok->comment_newline = 0;
            p_start = tok->start;
            p_end = tok->cur;
            return MAKE_TOKEN(NL);
        }
        p_start = tok->start;
        p_end = tok->cur - 1; /* Leave '\n' out of the string */
        tok->cont_line = 0;
        return MAKE_TOKEN(NEWLINE);
    }

    /* Period or number starting with period? */
    if (c == '.') {
        c = tok_nextc(tok);
        if (Py_ISDIGIT(c)) {
            goto fraction;
        } else if (c == '.') {
            c = tok_nextc(tok);
            if (c == '.') {
                p_start = tok->start;
                p_end = tok->cur;
                return MAKE_TOKEN(ELLIPSIS);
            }
            else {
                tok_backup(tok, c);
            }
            tok_backup(tok, '.');
        }
        else {
            tok_backup(tok, c);
        }
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(DOT);
    }

    /* Number */
    if (Py_ISDIGIT(c)) {
        if (c == '0') {
            /* Hex, octal or binary -- maybe. */
            c = tok_nextc(tok);
            if (c == 'x' || c == 'X') {
                /* Hex */
                c = tok_nextc(tok);
                do {
                    if (c == '_') {
                        c = tok_nextc(tok);
                    }
                    if (!Py_ISXDIGIT(c)) {
                        tok_backup(tok, c);
                        return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid hexadecimal literal"));
                    }
                    do {
                        c = tok_nextc(tok);
                    } while (Py_ISXDIGIT(c));
                } while (c == '_');
                if (!verify_end_of_number(tok, c, "hexadecimal")) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
            else if (c == 'o' || c == 'O') {
                /* Octal */
                c = tok_nextc(tok);
                do {
                    if (c == '_') {
                        c = tok_nextc(tok);
                    }
                    if (c < '0' || c >= '8') {
                        if (Py_ISDIGIT(c)) {
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                                    "invalid digit '%c' in octal literal", c));
                        }
                        else {
                            tok_backup(tok, c);
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid octal literal"));
                        }
                    }
                    do {
                        c = tok_nextc(tok);
                    } while ('0' <= c && c < '8');
                } while (c == '_');
                if (Py_ISDIGIT(c)) {
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                            "invalid digit '%c' in octal literal", c));
                }
                if (!verify_end_of_number(tok, c, "octal")) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
            else if (c == 'b' || c == 'B') {
                /* Binary */
                c = tok_nextc(tok);
                do {
                    if (c == '_') {
                        c = tok_nextc(tok);
                    }
                    if (c != '0' && c != '1') {
                        if (Py_ISDIGIT(c)) {
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid digit '%c' in binary literal", c));
                        }
                        else {
                            tok_backup(tok, c);
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid binary literal"));
                        }
                    }
                    do {
                        c = tok_nextc(tok);
                    } while (c == '0' || c == '1');
                } while (c == '_');
                if (Py_ISDIGIT(c)) {
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid digit '%c' in binary literal", c));
                }
                if (!verify_end_of_number(tok, c, "binary")) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
            else {
                int nonzero = 0;
                /* maybe old-style octal; c is first char of it */
                /* in any case, allow '0' as a literal */
                while (1) {
                    if (c == '_') {
                        c = tok_nextc(tok);
                        if (!Py_ISDIGIT(c)) {
                            tok_backup(tok, c);
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid decimal literal"));
                        }
                    }
                    if (c != '0') {
                        break;
                    }
                    c = tok_nextc(tok);
                }
                char* zeros_end = tok->cur;
                if (Py_ISDIGIT(c)) {
                    nonzero = 1;
                    c = tok_decimal_tail(tok);
                    if (c == 0) {
                        return MAKE_TOKEN(ERRORTOKEN);
                    }
                }
                if (c == '.') {
                    c = tok_nextc(tok);
                    goto fraction;
                }
                else if (c == 'e' || c == 'E') {
                    goto exponent;
                }
                else if (c == 'j' || c == 'J') {
                    goto imaginary;
                }
                else if (nonzero && !tok->tok_extra_tokens) {
                    /* Old-style octal: now disallowed. */
                    tok_backup(tok, c);
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror_known_range(
                            tok, (int)(tok->start + 1 - tok->line_start),
                            (int)(zeros_end - tok->line_start),
                            "leading zeros in decimal integer "
                            "literals are not permitted; "
                            "use an 0o prefix for octal integers"));
                }
                if (!verify_end_of_number(tok, c, "decimal")) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
        }
        else {
            /* Decimal */
            c = tok_decimal_tail(tok);
            if (c == 0) {
                return MAKE_TOKEN(ERRORTOKEN);
            }
            {
                /* Accept floating-point numbers. */
                if (c == '.') {
                    c = tok_nextc(tok);
        fraction:
                    /* Fraction */
                    if (Py_ISDIGIT(c)) {
                        c = tok_decimal_tail(tok);
                        if (c == 0) {
                            return MAKE_TOKEN(ERRORTOKEN);
                        }
                    }
                }
                if (c == 'e' || c == 'E') {
                    int e;
                  exponent:
                    e = c;
                    /* Exponent part */
                    c = tok_nextc(tok);
                    if (c == '+' || c == '-') {
                        c = tok_nextc(tok);
                        if (!Py_ISDIGIT(c)) {
                            tok_backup(tok, c);
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid decimal literal"));
                        }
                    } else if (!Py_ISDIGIT(c)) {
                        tok_backup(tok, c);
                        if (!verify_end_of_number(tok, e, "decimal")) {
                            return MAKE_TOKEN(ERRORTOKEN);
                        }
                        tok_backup(tok, e);
                        p_start = tok->start;
                        p_end = tok->cur;
                        return MAKE_TOKEN(NUMBER);
                    }
                    c = tok_decimal_tail(tok);
                    if (c == 0) {
                        return MAKE_TOKEN(ERRORTOKEN);
                    }
                }
                if (c == 'j' || c == 'J') {
                    /* Imaginary part */
        imaginary:
                    c = tok_nextc(tok);
                    if (!verify_end_of_number(tok, c, "imaginary")) {
                        return MAKE_TOKEN(ERRORTOKEN);
                    }
                }
                else if (!verify_end_of_number(tok, c, "decimal")) {
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
        }
        tok_backup(tok, c);
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(NUMBER);
    }

  f_string_quote:
    if (((Py_TOLOWER(*tok->start) == 'f' || Py_TOLOWER(*tok->start) == 'r' || Py_TOLOWER(*tok->start) == 't')
        && (c == '\'' || c == '"'))) {

        int quote = c;
        int quote_size = 1;             /* 1 or 3 */

        /* Nodes of type STRING, especially multi line strings
           must be handled differently in order to get both
           the starting line number and the column offset right.
           (cf. issue 16806) */
        tok->first_lineno = tok->lineno;
        tok->multi_line_start = tok->line_start;

        /* Find the quote size and start of string */
        int after_quote = tok_nextc(tok);
        if (after_quote == quote) {
            int after_after_quote = tok_nextc(tok);
            if (after_after_quote == quote) {
                quote_size = 3;
            }
            else {
                tok_backup(tok, after_after_quote);
                tok_backup(tok, after_quote);
            }
        }
        if (after_quote != quote) {
            tok_backup(tok, after_quote);
        }


        p_start = tok->start;
        p_end = tok->cur;
        if (tok->tok_mode_stack_index + 1 >= MAXFSTRINGLEVEL) {
            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "too many nested f-strings or t-strings"));
        }
        tokenizer_mode *the_current_tok = TOK_NEXT_MODE(tok);
        the_current_tok->kind = TOK_FSTRING_MODE;
        the_current_tok->quote = quote;
        the_current_tok->quote_size = quote_size;
        the_current_tok->start = tok->start;
        the_current_tok->multi_line_start = tok->line_start;
        the_current_tok->first_line = tok->lineno;
        the_current_tok->start_offset = -1;
        the_current_tok->multi_line_start_offset = -1;
        the_current_tok->last_expr_start = NULL;
        the_current_tok->last_expr_start_offset = -1;
        the_current_tok->in_format_spec = 0;
        the_current_tok->in_debug = 0;

        enum string_kind_t string_kind = FSTRING;
        switch (*tok->start) {
            case 'T':
            case 't':
                the_current_tok->raw = Py_TOLOWER(*(tok->start + 1)) == 'r';
                string_kind = TSTRING;
                break;
            case 'F':
            case 'f':
                the_current_tok->raw = Py_TOLOWER(*(tok->start + 1)) == 'r';
                break;
            case 'R':
            case 'r':
                the_current_tok->raw = 1;
                if (Py_TOLOWER(*(tok->start + 1)) == 't') {
                    string_kind = TSTRING;
                }
                break;
            default:
                Py_UNREACHABLE();
        }

        the_current_tok->string_kind = string_kind;
        the_current_tok->curly_bracket_depth = 0;
        the_current_tok->curly_bracket_expr_start_depth = -1;
        return string_kind == TSTRING ? MAKE_TOKEN(TSTRING_START) : MAKE_TOKEN(FSTRING_START);
    }

  letter_quote:
    /* String */
    if (c == '\'' || c == '"') {
        int quote = c;
        int quote_size = 1;             /* 1 or 3 */
        int end_quote_size = 0;
        int has_escaped_quote = 0;

        /* Nodes of type STRING, especially multi line strings
           must be handled differently in order to get both
           the starting line number and the column offset right.
           (cf. issue 16806) */
        tok->first_lineno = tok->lineno;
        tok->multi_line_start = tok->line_start;

        /* Find the quote size and start of string */
        c = tok_nextc(tok);
        if (c == quote) {
            c = tok_nextc(tok);
            if (c == quote) {
                quote_size = 3;
            }
            else {
                end_quote_size = 1;     /* empty string found */
            }
        }
        if (c != quote) {
            tok_backup(tok, c);
        }

        /* Get rest of string */
        while (end_quote_size != quote_size) {
            c = tok_nextc(tok);
            if (tok->done == E_ERROR) {
                return MAKE_TOKEN(ERRORTOKEN);
            }
            if (tok->done == E_DECODE) {
                break;
            }
            if (c == EOF || (quote_size == 1 && c == '\n')) {
                assert(tok->multi_line_start != NULL);
                // shift the tok_state's location into
                // the start of string, and report the error
                // from the initial quote character
                tok->cur = (char *)tok->start;
                tok->cur++;
                tok->line_start = tok->multi_line_start;
                int start = tok->lineno;
                tok->lineno = tok->first_lineno;

                if (INSIDE_FSTRING(tok)) {
                    /* When we are in an f-string, before raising the
                     * unterminated string literal error, check whether
                     * does the initial quote matches with f-strings quotes
                     * and if it is, then this must be a missing '}' token
                     * so raise the proper error */
                    tokenizer_mode *the_current_tok = TOK_GET_MODE(tok);
                    if (the_current_tok->quote == quote &&
                        the_current_tok->quote_size == quote_size) {
                        int level = tok->level - the_current_tok->curly_bracket_depth
                                    + the_current_tok->curly_bracket_expr_start_depth;
                        assert(level >= 0 && level < tok->level);
                        assert(tok->parenstack[level] == '{');
                        int lineno = tok->parenlinenostack[level];
                        if (lineno != tok->lineno) {
                            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                                "%c-string: expecting '}' to close '{' on line %d",
                                TOK_GET_STRING_PREFIX(tok), lineno));
                        }
                        return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                            "%c-string: expecting '}'", TOK_GET_STRING_PREFIX(tok)));
                    }
                }

                if (quote_size == 3) {
                    _PyTokenizer_syntaxerror(tok, "unterminated triple-quoted string literal"
                                     " (detected at line %d)", start);
                    if (c != '\n') {
                        tok->done = E_EOFS;
                    }
                    return MAKE_TOKEN(ERRORTOKEN);
                }
                else {
                    if (has_escaped_quote) {
                        _PyTokenizer_syntaxerror(
                            tok,
                            "unterminated string literal (detected at line %d); "
                            "perhaps you escaped the end quote?",
                            start
                        );
                    } else {
                        _PyTokenizer_syntaxerror(
                            tok, "unterminated string literal (detected at line %d)", start
                        );
                    }
                    if (c != '\n') {
                        tok->done = E_EOLS;
                    }
                    return MAKE_TOKEN(ERRORTOKEN);
                }
            }
            if (c == quote) {
                end_quote_size += 1;
            }
            else {
                end_quote_size = 0;
                if (c == '\\') {
                    c = tok_nextc(tok);  /* skip escaped char */
                    if (c == quote) {  /* but record whether the escaped char was a quote */
                        has_escaped_quote = 1;
                    }
                    if (c == '\r') {
                        c = tok_nextc(tok);
                    }
                }
            }
        }

        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(STRING);
    }

    /* Line continuation */
    if (c == '\\') {
        if ((c = tok_continuation_line(tok)) == -1) {
            return MAKE_TOKEN(ERRORTOKEN);
        }
        tok->cont_line = 1;
        goto again; /* Read next line */
    }

    /* Punctuation character */
    int is_punctuation = (c == ':' || c == '}' || c == '!' || c == '{');
    if (is_punctuation && INSIDE_FSTRING(tok) && INSIDE_FSTRING_EXPR(current_tok)) {
        /* This code block gets executed before the curly_bracket_depth is incremented
         * by the `{` case, so for ensuring that we are on the 0th level, we need
         * to adjust it manually */
        int cursor = current_tok->curly_bracket_depth - (c != '{');
        int in_format_spec = current_tok->in_format_spec;
        int cursor_in_format_with_debug =
            cursor == 1 && (current_tok->in_debug || in_format_spec);
        int cursor_valid = cursor == 0 || cursor_in_format_with_debug;
        if ((cursor_valid) && c != '{' && set_ftstring_expr(tok, token, c)) {
            return MAKE_TOKEN(ERRORTOKEN);
        }

        if (c == ':' && cursor == current_tok->curly_bracket_expr_start_depth) {
            current_tok->kind = TOK_FSTRING_MODE;
            current_tok->in_format_spec = 1;
            p_start = tok->start;
            p_end = tok->cur;
            return MAKE_TOKEN(_PyToken_OneChar(c));
        }
    }

    /* Check for two-character token */
    {
        int c2 = tok_nextc(tok);
        int current_token = _PyToken_TwoChars(c, c2);
        if (c == '<' && c2 == '>' && !tok->barry_as_bdfl) {
            current_token = OP;
        }
        if (current_token != OP) {
            int c3 = tok_nextc(tok);
            int current_token3 = _PyToken_ThreeChars(c, c2, c3);
            if (current_token3 != OP) {
                current_token = current_token3;
            }
            else {
                tok_backup(tok, c3);
            }
            p_start = tok->start;
            p_end = tok->cur;
            return MAKE_TOKEN(current_token);
        }
        tok_backup(tok, c2);
    }

    /* Keep track of parentheses nesting level */
    switch (c) {
    case '(':
    case '[':
    case '{':
        if (tok->level >= MAXLEVEL) {
            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "too many nested parentheses"));
        }
        tok->parenstack[tok->level] = c;
        tok->parenlinenostack[tok->level] = tok->lineno;
        tok->parencolstack[tok->level] = (int)(tok->start - tok->line_start);
        tok->level++;
        if (INSIDE_FSTRING(tok)) {
            current_tok->curly_bracket_depth++;
        }
        break;
    case ')':
    case ']':
    case '}':
        if (INSIDE_FSTRING(tok) && !current_tok->curly_bracket_depth && c == '}') {
            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                "%c-string: single '}' is not allowed", TOK_GET_STRING_PREFIX(tok)));
        }
        if (!tok->tok_extra_tokens && !tok->level) {
            return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "unmatched '%c'", c));
        }
        if (tok->level > 0) {
            tok->level--;
            int opening = tok->parenstack[tok->level];
            if (!((opening == '(' && c == ')') ||
                  (opening == '[' && c == ']') ||
                  (opening == '{' && c == '}'))) {
                /* If the opening bracket belongs to an f-string's expression
                part (e.g. f"{)}") and the closing bracket is an arbitrary
                nested expression, then instead of matching a different
                syntactical construct with it; we'll throw an unmatched
                parentheses error. */
                if (INSIDE_FSTRING(tok) && opening == '{') {
                    assert(current_tok->curly_bracket_depth >= 0);
                    int previous_bracket = current_tok->curly_bracket_depth - 1;
                    if (previous_bracket == current_tok->curly_bracket_expr_start_depth) {
                        return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                            "%c-string: unmatched '%c'", TOK_GET_STRING_PREFIX(tok), c));
                    }
                }
                if (tok->tok_extra_tokens) {
                    /* Allow other mismatched brackets. */
                }
                else if (tok->parenlinenostack[tok->level] != tok->lineno) {
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                            "closing parenthesis '%c' does not match "
                            "opening parenthesis '%c' on line %d",
                            c, opening, tok->parenlinenostack[tok->level]));
                }
                else {
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                            "closing parenthesis '%c' does not match "
                            "opening parenthesis '%c'",
                            c, opening));
                }
            }
        }

        if (INSIDE_FSTRING(tok)) {
            current_tok->curly_bracket_depth--;
            if (current_tok->curly_bracket_depth < 0) {
                return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "%c-string: unmatched '%c'",
                    TOK_GET_STRING_PREFIX(tok), c));
            }
            if (c == '}' && current_tok->curly_bracket_depth == current_tok->curly_bracket_expr_start_depth) {
                current_tok->curly_bracket_expr_start_depth--;
                current_tok->kind = TOK_FSTRING_MODE;
                current_tok->in_format_spec = 0;
                current_tok->in_debug = 0;
            }
        }
        break;
    default:
        break;
    }

    if (!Py_UNICODE_ISPRINTABLE(c)) {
        return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok, "invalid non-printable character U+%04X", c));
    }

    if( c == '=' && INSIDE_FSTRING_EXPR_AT_TOP(current_tok)) {
        current_tok->in_debug = 1;
    }

    /* Punctuation character */
    p_start = tok->start;
    p_end = tok->cur;
    return MAKE_TOKEN(_PyToken_OneChar(c));
}

static int
tok_get_fstring_mode(struct tok_state *tok, tokenizer_mode* current_tok, struct token *token)
{
    const char *p_start = NULL;
    const char *p_end = NULL;
    int end_quote_size = 0;
    int unicode_escape = 0;

    tok->start = tok->cur;
    tok->first_lineno = tok->lineno;
    tok->starting_col_offset = tok->col_offset;

    // If we start with a bracket, we defer to the normal mode as there is nothing for us to tokenize
    // before it.
    int start_char = tok_nextc(tok);
    if (start_char == '{') {
        int peek1 = tok_nextc(tok);
        tok_backup(tok, peek1);
        if (peek1 != '{') {
            current_tok->last_expr_start = tok->cur;
        }
        tok_backup(tok, start_char);
        if (peek1 != '{') {
            current_tok->curly_bracket_expr_start_depth++;
            if (current_tok->curly_bracket_expr_start_depth >= MAX_EXPR_NESTING) {
                return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                    "%c-string: expressions nested too deeply", TOK_GET_STRING_PREFIX(tok)));
            }
            TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
            return tok_get_normal_mode(tok, current_tok, token);
        }
    }
    else {
        tok_backup(tok, start_char);
    }

    // Check if we are at the end of the string
    for (int i = 0; i < current_tok->quote_size; i++) {
        int quote = tok_nextc(tok);
        if (quote != current_tok->quote) {
            tok_backup(tok, quote);
            goto f_string_middle;
        }
    }

    p_start = tok->start;
    p_end = tok->cur;
    tok->tok_mode_stack_index--;
    return MAKE_TOKEN(FTSTRING_END(current_tok));

f_string_middle:

    tok->multi_line_start = tok->line_start;
    while (end_quote_size != current_tok->quote_size) {
        int c = tok_nextc(tok);
        if (tok->done == E_ERROR || tok->done == E_DECODE) {
            return MAKE_TOKEN(ERRORTOKEN);
        }
        int in_format_spec = (
                current_tok->in_format_spec
                &&
                INSIDE_FSTRING_EXPR(current_tok)
        );

       if (c == EOF || (current_tok->quote_size == 1 && c == '\n')) {
            if (tok->decoding_erred) {
                return MAKE_TOKEN(ERRORTOKEN);
            }

            // If we are in a format spec and we found a newline,
            // it means that the format spec ends here and we should
            // return to the regular mode.
            if (in_format_spec && c == '\n') {
                if (current_tok->quote_size == 1) {
                    return MAKE_TOKEN(
                        _PyTokenizer_syntaxerror(
                            tok,
                            "%c-string: newlines are not allowed in format specifiers for single quoted %c-strings",
                            TOK_GET_STRING_PREFIX(tok), TOK_GET_STRING_PREFIX(tok)
                        )
                    );
                }
                tok_backup(tok, c);
                TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
                current_tok->in_format_spec = 0;
                p_start = tok->start;
                p_end = tok->cur;
                return MAKE_TOKEN(FTSTRING_MIDDLE(current_tok));
            }

            assert(tok->multi_line_start != NULL);
            // shift the tok_state's location into
            // the start of string, and report the error
            // from the initial quote character
            tok->cur = (char *)current_tok->start;
            tok->cur++;
            tok->line_start = current_tok->multi_line_start;
            int start = tok->lineno;

            tokenizer_mode *the_current_tok = TOK_GET_MODE(tok);
            tok->lineno = the_current_tok->first_line;

            if (current_tok->quote_size == 3) {
                _PyTokenizer_syntaxerror(tok,
                                    "unterminated triple-quoted %c-string literal"
                                    " (detected at line %d)",
                                    TOK_GET_STRING_PREFIX(tok), start);
                if (c != '\n') {
                    tok->done = E_EOFS;
                }
                return MAKE_TOKEN(ERRORTOKEN);
            }
            else {
                return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                                    "unterminated %c-string literal (detected at"
                                    " line %d)", TOK_GET_STRING_PREFIX(tok), start));
            }
        }

        if (c == current_tok->quote) {
            end_quote_size += 1;
            continue;
        } else {
            end_quote_size = 0;
        }

        if (c == '{') {
            int peek = tok_nextc(tok);
            if (peek != '{' || in_format_spec) {
                tok_backup(tok, peek);
                current_tok->last_expr_start = tok->cur;
                tok_backup(tok, c);
                current_tok->curly_bracket_expr_start_depth++;
                if (current_tok->curly_bracket_expr_start_depth >= MAX_EXPR_NESTING) {
                    return MAKE_TOKEN(_PyTokenizer_syntaxerror(tok,
                        "%c-string: expressions nested too deeply", TOK_GET_STRING_PREFIX(tok)));
                }
                TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
                current_tok->in_format_spec = 0;
                p_start = tok->start;
                p_end = tok->cur;
            } else {
                p_start = tok->start;
                p_end = tok->cur - 1;
            }
            return MAKE_TOKEN(FTSTRING_MIDDLE(current_tok));
        } else if (c == '}') {
            if (unicode_escape) {
                p_start = tok->start;
                p_end = tok->cur;
                return MAKE_TOKEN(FTSTRING_MIDDLE(current_tok));
            }
            int peek = tok_nextc(tok);

            // The tokenizer can only be in the format spec if we have already completed the expression
            // scanning (indicated by the end of the expression being set) and we are not at the top level
            // of the bracket stack (-1 is the top level). Since format specifiers can't legally use double
            // brackets, we can bypass it here.
            int cursor = current_tok->curly_bracket_depth;
            if (peek == '}' && !in_format_spec && cursor == 0) {
                p_start = tok->start;
                p_end = tok->cur - 1;
            } else {
                tok_backup(tok, peek);
                tok_backup(tok, c);
                TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
                current_tok->in_format_spec = 0;
                p_start = tok->start;
                p_end = tok->cur;
            }
            return MAKE_TOKEN(FTSTRING_MIDDLE(current_tok));
        } else if (c == '\\') {
            int peek = tok_nextc(tok);
            if (peek == '\r') {
                peek = tok_nextc(tok);
            }
            // Special case when the backslash is right before a curly
            // brace. We have to restore and return the control back
            // to the loop for the next iteration.
            if (peek == '{' || peek == '}') {
                if (!current_tok->raw) {
                    if (_PyTokenizer_warn_invalid_escape_sequence(tok, peek)) {
                        return MAKE_TOKEN(ERRORTOKEN);
                    }
                }
                tok_backup(tok, peek);
                continue;
            }

            if (!current_tok->raw) {
                if (peek == 'N') {
                    /* Handle named unicode escapes (\N{BULLET}) */
                    peek = tok_nextc(tok);
                    if (peek == '{') {
                        unicode_escape = 1;
                    } else {
                        tok_backup(tok, peek);
                    }
                }
            } /* else {
                skip the escaped character
            }*/
        }
    }

    // Backup the f-string quotes to emit a final FSTRING_MIDDLE and
    // add the quotes to the FSTRING_END in the next tokenizer iteration.
    for (int i = 0; i < current_tok->quote_size; i++) {
        tok_backup(tok, current_tok->quote);
    }
    p_start = tok->start;
    p_end = tok->cur;
    return MAKE_TOKEN(FTSTRING_MIDDLE(current_tok));
}

static int
tok_get(struct tok_state *tok, struct token *token)
{
    tokenizer_mode *current_tok = TOK_GET_MODE(tok);
    if (current_tok->kind == TOK_REGULAR_MODE) {
        return tok_get_normal_mode(tok, current_tok, token);
    } else {
        return tok_get_fstring_mode(tok, current_tok, token);
    }
}

/* Return the next token type and fill *token; ERRORTOKEN on error (details
   in tok->done and tok->err). */
static int
_PyTokenizer_Get(struct tok_state *tok, struct token *token)
{
    int result = tok_get(tok, token);
    if (tok->decoding_erred) {
        result = ERRORTOKEN;
        tok->done = E_DECODE;
    }
    return result;
}

#undef is_potential_identifier_start
#undef is_potential_identifier_char
#undef TOK_GET_MODE
#undef TOK_NEXT_MODE
#undef FTSTRING_MIDDLE
#undef FTSTRING_END
#undef TOK_GET_STRING_PREFIX
#undef MAKE_TOKEN
#undef MAKE_TYPE_COMMENT_TOKEN
#undef QP_LEX_IS_CONT
#undef ADVANCE_LINENO
#undef TABSIZE
#undef ALTTABSIZE
#undef Py_CHARMASK
#undef Py_ISDIGIT
#undef Py_ISXDIGIT
#undef Py_ISALPHA
#undef Py_ISALNUM
#undef Py_TOLOWER
#undef Py_MAX
#undef Py_UNICODE_ISPRINTABLE
#undef Py_UNREACHABLE
