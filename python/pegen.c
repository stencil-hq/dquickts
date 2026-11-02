/*
 * PEG parser runtime used by the generated parser (parser.c): token stream
 * and memoization, error reporting, AST action helpers, number/string
 * literal decoding and the string entry point.
 *
 * Derived from CPython Parser/pegen.c, Parser/pegen_errors.c,
 * Parser/action_helpers.c, Parser/string_parser.c and Parser/peg_api.c,
 * plus the pieces of the object layer the parser relies on:
 * _PyUnicode_DecodeUnicodeEscapeInternal2 (Objects/unicodeobject.c),
 * _PyBytes_DecodeEscape2 (Objects/bytesobject.c), the integer string
 * conversion limit of long_from_string_base (Objects/longobject.c) and
 * UnicodeDecodeError.__str__ (Objects/exceptions.c) (PSF-2.0).
 *
 * See pegen.h for the differences from CPython.
 */

#include "pegen_compat.h"

/* ---------------------------------------------------------------------- */
/* Error indicator and text helpers */

/* PyErr_NoMemory(): MemoryError replaces any pending error. */
static void *qp_pegen_no_memory(Parser *p)
{
    qp_error_clear(p->err);
    qp_error_set(p->err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
    return NULL;
}

/* A QuickJS allocation failed and left its exception pending in p->ctx:
   report it as MemoryError. */
static void *qp_pegen_js_error(Parser *p)
{
    JS_FreeValue(p->ctx, JS_GetException(p->ctx));
    return qp_pegen_no_memory(p);
}

/* Decode the UTF-8 sequence starting at s (s < end) like CPython's strict
   UTF-8 decoder.  Returns the sequence length (code point in *pc), or minus
   the length of the maximal invalid subpart with *reason set to the
   UnicodeDecodeError reason. */
static int qp_utf8_decode1(const uint8_t *s, const uint8_t *end, uint32_t *pc,
                           const char **reason)
{
    uint32_t c = s[0];
    uint8_t lo = 0x80, hi = 0xBF;
    int n, i;

    if (c < 0x80) {
        *pc = c;
        return 1;
    }
    if (c < 0xC2 || c > 0xF4) {
        *reason = "invalid start byte";
        return -1;
    }
    if (c < 0xE0) {
        n = 2;
        c &= 0x1F;
    } else if (c < 0xF0) {
        n = 3;
        if (c == 0xE0)
            lo = 0xA0;
        else if (c == 0xED)
            hi = 0x9F;
        c &= 0x0F;
    } else {
        n = 4;
        if (c == 0xF0)
            lo = 0x90;
        else if (c == 0xF4)
            hi = 0x8F;
        c &= 0x07;
    }
    for (i = 1; i < n; i++) {
        if (s + i >= end) {
            *reason = "unexpected end of data";
            return -i;
        }
        if (s[i] < lo || s[i] > hi) {
            *reason = "invalid continuation byte";
            return -i;
        }
        lo = 0x80;
        hi = 0xBF;
        c = (c << 6) | (s[i] & 0x3F);
    }
    *pc = c;
    return n;
}

/* Number of characters PyUnicode_DecodeUTF8(s, len, "replace") yields. */
static Py_ssize_t qp_utf8_char_count(const char *s, Py_ssize_t len)
{
    const uint8_t *q = (const uint8_t *)s, *end = q + len;
    const char *reason;
    uint32_t c;
    Py_ssize_t n = 0;
    int k;

    while (q < end) {
        k = qp_utf8_decode1(q, end, &c, &reason);
        q += k > 0 ? k : -k;
        n++;
    }
    return n;
}

/* Append PyUnicode_DecodeUTF8(s, len, "replace") re-encoded as UTF-8 to b
   and NUL-terminate it (the NUL is not counted in b->size). */
static int qp_utf8_replace(DynBuf *b, const char *s, Py_ssize_t len)
{
    const uint8_t *q = (const uint8_t *)s, *end = q + len;
    const char *reason;
    uint32_t c;
    int k;

    while (q < end) {
        k = qp_utf8_decode1(q, end, &c, &reason);
        if (k > 0) {
            dbuf_put(b, q, k);
            q += k;
        } else {
            dbuf_put(b, (const uint8_t *)"\xef\xbf\xbd", 3);
            q += -k;
        }
    }
    dbuf_putc(b, '\0');
    b->size--;
    return dbuf_error(b) ? -1 : 0;
}

/* Raise UnicodeDecodeError(encoding, obj[0:objlen], start, end, reason),
   with the message UnicodeDecodeError.__str__ produces. */
static void qp_pegen_set_decode_error(Parser *p, const char *encoding, const char *obj,
                                      Py_ssize_t objlen, Py_ssize_t start, Py_ssize_t end,
                                      const char *reason)
{
    qp_error_clear(p->err);
    if ((start >= 0 && start < objlen) && (end >= 0 && end <= objlen) && end == start + 1) {
        qp_error_set(p->err, QP_ERR_UNICODE_DECODE, 0, 0, 0, 0,
                     "'%s' codec can't decode byte 0x%02x in position %lld: %s",
                     encoding, (int)(obj[start] & 0xff), (long long)start, reason);
    } else {
        qp_error_set(p->err, QP_ERR_UNICODE_DECODE, 0, 0, 0, 0,
                     "'%s' codec can't decode bytes in position %lld-%lld: %s",
                     encoding, (long long)start, (long long)(end - 1), reason);
    }
}

/* Strict UTF-8 validation of s[0:len] (PyUnicode_DecodeUTF8(s, len, NULL)):
   returns -1 with UnicodeDecodeError set if it is not valid UTF-8. */
static int qp_utf8_check(Parser *p, const char *s, Py_ssize_t len)
{
    const uint8_t *q = (const uint8_t *)s, *end = q + len;
    const char *reason;
    uint32_t c;
    int k;

    while (q < end) {
        k = qp_utf8_decode1(q, end, &c, &reason);
        if (k < 0) {
            Py_ssize_t start = (const char *)q - s;
            qp_pegen_set_decode_error(p, "utf-8", s, len, start, start - k, reason);
            return -1;
        }
        q += k;
    }
    return 0;
}

/* Py_UNICODE_ISPRINTABLE for the characters an identifier can contain:
   the only non-printable XID_Continue characters are U+200C and U+200D
   (format characters); C0/C1 controls and NBSP are covered for safety. */
static int qp_pegen_ident_char_printable(uint32_t c)
{
    if (c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0xAD)
        return 0;
    return c != 0x200C && c != 0x200D;
}

/* Append repr(s) for the str whose UTF-8 text is s[0:len] (unicode_repr). */
static void qp_pegen_put_repr(DynBuf *b, const char *s, size_t len)
{
    const uint8_t *q = (const uint8_t *)s, *end = q + len;
    const char *reason;
    uint32_t c;
    int k, quote = '\'';

    if (memchr(s, '\'', len) && !memchr(s, '"', len))
        quote = '"';
    dbuf_putc(b, quote);
    while (q < end) {
        k = qp_utf8_decode1(q, end, &c, &reason);
        if (k < 0) {
            c = 0xFFFD;
            k = -k;
        }
        if (c == (uint32_t)quote || c == '\\') {
            dbuf_putc(b, '\\');
            dbuf_putc(b, c);
        } else if (c == '\t') {
            dbuf_putstr(b, "\\t");
        } else if (c == '\n') {
            dbuf_putstr(b, "\\n");
        } else if (c == '\r') {
            dbuf_putstr(b, "\\r");
        } else if (c < ' ' || c == 0x7F) {
            dbuf_printf(b, "\\x%02x", (unsigned)c);
        } else if (c < 0x7F) {
            dbuf_putc(b, c);
        } else if (qp_pegen_ident_char_printable(c)) {
            uint8_t buf[UTF8_CHAR_LEN_MAX];
            dbuf_put(b, buf, unicode_to_utf8(buf, c));
        } else if (c <= 0xFF) {
            dbuf_printf(b, "\\x%02x", (unsigned)c);
        } else if (c <= 0xFFFF) {
            dbuf_printf(b, "\\u%04x", (unsigned)c);
        } else {
            dbuf_printf(b, "\\U%08x", (unsigned)c);
        }
        q += k;
    }
    dbuf_putc(b, quote);
}

/* Append str(atom) (repr = 0) or repr(atom) (repr = 1) to b. */
static int qp_pegen_put_atom(Parser *p, DynBuf *b, JSAtom atom, int repr)
{
    size_t len;
    const char *s = JS_AtomToCStringLen(p->ctx, &len, atom);
    if (s == NULL) {
        qp_pegen_js_error(p);
        return -1;
    }
    if (repr)
        qp_pegen_put_repr(b, s, len);
    else
        dbuf_put(b, (const uint8_t *)s, len);
    JS_FreeCString(p->ctx, s);
    return 0;
}

/* The PyUnicode_FromFormatV subset used by parser messages: %% %s %c %d %i
   %ld %zd, and %U / %R taking a JSAtom (str() / repr() of it). */
static int qp_pegen_vformat(Parser *p, DynBuf *b, const char *fmt, va_list va)
{
    const char *f;
    int size;

    for (f = fmt; *f; f++) {
        if (*f != '%') {
            dbuf_putc(b, *f);
            continue;
        }
        f++;
        size = 0;
        if (*f == 'l') {
            size = 1;
            f++;
        } else if (*f == 'z') {
            size = 2;
            f++;
        }
        switch (*f) {
        case '%':
            dbuf_putc(b, '%');
            break;
        case 's': {
            const char *s = va_arg(va, const char *);
            dbuf_putstr(b, s ? s : "(null)");
            break;
        }
        case 'c': {
            uint8_t buf[UTF8_CHAR_LEN_MAX];
            int c = va_arg(va, int);
            dbuf_put(b, buf, unicode_to_utf8(buf, (unsigned int)c));
            break;
        }
        case 'd':
        case 'i':
            if (size == 1)
                dbuf_printf(b, "%ld", va_arg(va, long));
            else if (size == 2)
                dbuf_printf(b, "%lld", (long long)va_arg(va, Py_ssize_t));
            else
                dbuf_printf(b, "%d", va_arg(va, int));
            break;
        case 'U':
        case 'R':
            if (qp_pegen_put_atom(p, b, va_arg(va, JSAtom), *f == 'R') < 0)
                return -1;
            break;
        case '\0':
            dbuf_putc(b, '%');
            f--;
            break;
        default:
            dbuf_putc(b, '%');
            dbuf_putc(b, *f);
            break;
        }
    }
    dbuf_putc(b, '\0');
    b->size--;
    if (dbuf_error(b)) {
        qp_pegen_no_memory(p);
        return -1;
    }
    return 0;
}

/* Text of a str constant (qp_const QP_CONST_STR). */
static inline JSString *qp_pegen_str(qp_const *c)
{
    return JS_VALUE_GET_STRING(c->v);
}

/* PyUnicode_GET_LENGTH(c->v) == 0 for a str constant. */
static inline int qp_pegen_str_is_empty(qp_const *c)
{
    return qp_pegen_str(c)->len == 0;
}

/* Wrap a new JSString (or JS_EXCEPTION) as an arena-owned constant. */
static qp_const *qp_pegen_new_const(Parser *p, qp_const_kind kind, JSValue v)
{
    if (JS_IsException(v)) {
        qp_pegen_js_error(p);
        return NULL;
    }
    return qp_const_new(p->arena, kind, v);
}

/* identifier == ascii (PyUnicode_CompareWithASCIIString(id, ascii) == 0). */
static int qp_pegen_atom_eq(Parser *p, JSAtom atom, const char *ascii)
{
    char buf[64];
    size_t n = strlen(ascii);
    if (n + 1 >= sizeof(buf))
        return 0;
    return strcmp(JS_AtomGetStr(p->ctx, buf, sizeof(buf), atom), ascii) == 0;
}

/* ---------------------------------------------------------------------- */
/* Parser/pegen.c */

// Internal parser functions

asdl_stmt_seq*
_PyPegen_interactive_exit(Parser *p)
{
    if (p->errcode) {
        *(p->errcode) = E_EOF;
    }
    return NULL;
}

/* `str` is the UTF-8 text of the error line (PyUnicode_AsUTF8 of the
   decoded line object in CPython). */
Py_ssize_t
_PyPegen_byte_offset_to_character_offset_raw(const char* str, Py_ssize_t col_offset)
{
    Py_ssize_t len = (Py_ssize_t)strlen(str);
    if (col_offset > len + 1) {
        col_offset = len + 1;
    }
    assert(col_offset >= 0);
    /* PyUnicode_DecodeUTF8(str, col_offset, "replace"); the terminating NUL
       counts as a character when col_offset == len + 1. */
    return qp_utf8_char_count(str, col_offset);
}

Py_ssize_t
_PyPegen_byte_offset_to_character_offset(const char *line, Py_ssize_t col_offset)
{
    return _PyPegen_byte_offset_to_character_offset_raw(line, col_offset);
}

// Here, mark is the start of the node, while p->mark is the end.
// If node==NULL, they should be the same.
int
_PyPegen_insert_memo(Parser *p, int mark, int type, void *node)
{
    // Insert in front
    Memo *m = _PyArena_Malloc(p->arena, sizeof(Memo));
    if (m == NULL) {
        return -1;
    }
    m->type = type;
    m->node = node;
    m->mark = p->mark;
    m->next = p->tokens[mark]->memo;
    p->tokens[mark]->memo = m;
    return 0;
}

// Like _PyPegen_insert_memo(), but updates an existing node if found.
int
_PyPegen_update_memo(Parser *p, int mark, int type, void *node)
{
    for (Memo *m = p->tokens[mark]->memo; m != NULL; m = m->next) {
        if (m->type == type) {
            // Update existing node.
            m->node = node;
            m->mark = p->mark;
            return 0;
        }
    }
    // Insert new node.
    return _PyPegen_insert_memo(p, mark, type, node);
}

static int
growable_comment_array_init(Parser *p, growable_comment_array *arr, size_t initial_size) {
    assert(initial_size > 0);
    arr->items = PyMem_Malloc(initial_size * sizeof(*arr->items));
    arr->size = initial_size;
    arr->num_items = 0;

    return arr->items != NULL;
}

static int
growable_comment_array_add(Parser *p, growable_comment_array *arr, int lineno, char *comment) {
    if (arr->num_items >= arr->size) {
        size_t new_size = arr->size * 2;
        void *new_items_array = PyMem_Realloc(arr->items, new_size * sizeof(*arr->items));
        if (!new_items_array) {
            return 0;
        }
        arr->items = new_items_array;
        arr->size = new_size;
    }

    arr->items[arr->num_items].lineno = lineno;
    arr->items[arr->num_items].comment = comment;  // Take ownership
    arr->num_items++;
    return 1;
}

static void
growable_comment_array_deallocate(Parser *p, growable_comment_array *arr) {
    for (unsigned i = 0; i < arr->num_items; i++) {
        PyMem_Free(arr->items[i].comment);
    }
    PyMem_Free(arr->items);
}

static int
_get_keyword_or_name_type(Parser *p, struct token *new_token)
{
    Py_ssize_t name_len = new_token->end_col_offset - new_token->col_offset;
    assert(name_len > 0);

    if (name_len >= p->n_keyword_lists ||
        p->keywords[name_len] == NULL ||
        p->keywords[name_len]->type == -1) {
        return NAME;
    }
    for (KeywordToken *k = p->keywords[name_len]; k != NULL && k->type != -1; k++) {
        if (strncmp(k->str, new_token->start, (size_t)name_len) == 0) {
            return k->type;
        }
    }
    return NAME;
}

static int
initialize_token(Parser *p, Token *parser_token, struct token *new_token, int token_type) {
    assert(parser_token != NULL);

    parser_token->type = (token_type == NAME) ? _get_keyword_or_name_type(p, new_token) : token_type;
    /* PyBytes_FromStringAndSize(start, end - start), kept in the arena */
    Py_ssize_t size = new_token->start && new_token->end > new_token->start
                      ? new_token->end - new_token->start : 0;
    char *bytes = _PyArena_Malloc(p->arena, (size_t)size + 1);
    if (bytes == NULL) {
        return -1;
    }
    if (size > 0) {
        memcpy(bytes, new_token->start, (size_t)size);
    }
    bytes[size] = '\0';
    parser_token->bytes = bytes;
    parser_token->bytes_len = size;

    parser_token->metadata = NULL;
    if (new_token->metadata != NULL) {
        JSValue text = JS_NewStringLen(p->ctx, new_token->metadata,
                                       (size_t)new_token->metadata_len);
        _PyToken_Free(new_token);
        _PyToken_Init(new_token);
        qp_const *metadata = qp_pegen_new_const(p, QP_CONST_STR, text);
        if (metadata == NULL) {
            return -1;
        }
        parser_token->metadata = metadata;
    }

    parser_token->level = new_token->level;
    parser_token->lineno = new_token->lineno;
    parser_token->col_offset = p->tok->lineno == p->starting_lineno ? p->starting_col_offset + new_token->col_offset
                                                                    : new_token->col_offset;
    parser_token->end_lineno = new_token->end_lineno;
    parser_token->end_col_offset = p->tok->lineno == p->starting_lineno ? p->starting_col_offset + new_token->end_col_offset
                                                                 : new_token->end_col_offset;

    p->fill += 1;

    if (token_type == ERRORTOKEN && p->tok->done == E_DECODE) {
        return _Pypegen_raise_decode_error(p);
    }

    return (token_type == ERRORTOKEN ? _Pypegen_tokenizer_error(p) : 0);
}

static int
_resize_tokens_array(Parser *p) {
    int newsize = p->size * 2;
    Token **new_tokens = PyMem_Realloc(p->tokens, (size_t)newsize * sizeof(Token *));
    if (new_tokens == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    p->tokens = new_tokens;

    for (int i = p->size; i < newsize; i++) {
        p->tokens[i] = PyMem_Calloc(1, sizeof(Token));
        if (p->tokens[i] == NULL) {
            p->size = i; // Needed, in order to cleanup correctly after parser fails
            PyErr_NoMemory();
            return -1;
        }
    }
    p->size = newsize;
    return 0;
}

int
_PyPegen_fill_token(Parser *p)
{
    struct token new_token;
    _PyToken_Init(&new_token);
    int type = _PyTokenizer_Get(p->tok, &new_token);

    // Record and skip '# type: ignore' comments
    while (type == TYPE_IGNORE) {
        Py_ssize_t len = new_token.end_col_offset - new_token.col_offset;
        char *tag = PyMem_Malloc((size_t)len + 1);
        if (tag == NULL) {
            PyErr_NoMemory();
            goto error;
        }
        strncpy(tag, new_token.start, (size_t)len);
        tag[len] = '\0';
        // Ownership of tag passes to the growable array
        if (!growable_comment_array_add(p, &p->type_ignore_comments, p->tok->lineno, tag)) {
            PyMem_Free(tag);
            PyErr_NoMemory();
            goto error;
        }
        _PyToken_Free(&new_token);
        _PyToken_Init(&new_token);
        type = _PyTokenizer_Get(p->tok, &new_token);
    }

    // If we have reached the end and we are in single input mode we need to insert a newline and reset the parsing
    if (p->start_rule == Py_single_input && type == ENDMARKER && p->parsing_started) {
        type = NEWLINE; /* Add an extra newline */
        p->parsing_started = 0;

        if (p->tok->indent && !(p->flags & PyPARSE_DONT_IMPLY_DEDENT)) {
            p->tok->pendin = -p->tok->indent;
            p->tok->indent = 0;
        }
    }
    else {
        p->parsing_started = 1;
    }

    // Check if we are at the limit of the token array capacity and resize if needed
    if ((p->fill == p->size) && (_resize_tokens_array(p) != 0)) {
        goto error;
    }

    Token *t = p->tokens[p->fill];
    int ret = initialize_token(p, t, &new_token, type);
    _PyToken_Free(&new_token);
    return ret;
error:
    _PyToken_Free(&new_token);
    return -1;
}

int  // bool
_PyPegen_is_memoized(Parser *p, int type, void *pres)
{
    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            p->error_indicator = 1;
            return -1;
        }
    }

    Token *t = p->tokens[p->mark];

    for (Memo *m = t->memo; m != NULL; m = m->next) {
        if (m->type == type) {
            p->mark = m->mark;
            *(void **)(pres) = m->node;
            return 1;
        }
    }
    return 0;
}

#define LOOKAHEAD1(NAME, RES_TYPE)                                  \
    int                                                             \
    NAME (int positive, RES_TYPE (func)(Parser *), Parser *p)       \
    {                                                               \
        int mark = p->mark;                                         \
        void *res = func(p);                                        \
        p->mark = mark;                                             \
        return (res != NULL) == positive;                           \
    }

LOOKAHEAD1(_PyPegen_lookahead, void *)
LOOKAHEAD1(_PyPegen_lookahead_for_expr, expr_ty)
LOOKAHEAD1(_PyPegen_lookahead_for_stmt, stmt_ty)
#undef LOOKAHEAD1

#define LOOKAHEAD2(NAME, RES_TYPE, T)                                   \
    int                                                                 \
    NAME (int positive, RES_TYPE (func)(Parser *, T), Parser *p, T arg) \
    {                                                                   \
        int mark = p->mark;                                             \
        void *res = func(p, arg);                                       \
        p->mark = mark;                                                 \
        return (res != NULL) == positive;                               \
    }

LOOKAHEAD2(_PyPegen_lookahead_with_int, Token *, int)
LOOKAHEAD2(_PyPegen_lookahead_with_string, expr_ty, const char *)
#undef LOOKAHEAD2

Token *
_PyPegen_expect_token(Parser *p, int type)
{
    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            p->error_indicator = 1;
            return NULL;
        }
    }
    Token *t = p->tokens[p->mark];
    if (t->type != type) {
       return NULL;
    }
    p->mark += 1;
    return t;
}

void*
_PyPegen_expect_forced_result(Parser *p, void* result, const char* expected) {

    if (p->error_indicator == 1) {
        return NULL;
    }
    if (result == NULL) {
        RAISE_SYNTAX_ERROR("expected (%s)", expected);
        return NULL;
    }
    return result;
}

Token *
_PyPegen_expect_forced_token(Parser *p, int type, const char* expected) {

    if (p->error_indicator == 1) {
        return NULL;
    }

    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            p->error_indicator = 1;
            return NULL;
        }
    }
    Token *t = p->tokens[p->mark];
    if (t->type != type) {
        RAISE_SYNTAX_ERROR_KNOWN_LOCATION(t, "expected '%s'", expected);
        return NULL;
    }
    p->mark += 1;
    return t;
}

expr_ty
_PyPegen_expect_soft_keyword(Parser *p, const char *keyword)
{
    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            p->error_indicator = 1;
            return NULL;
        }
    }
    Token *t = p->tokens[p->mark];
    if (t->type != NAME) {
        return NULL;
    }
    const char *s = t->bytes;
    if (!s) {
        p->error_indicator = 1;
        return NULL;
    }
    if (strcmp(s, keyword) != 0) {
        return NULL;
    }
    return _PyPegen_name_token(p);
}

Token *
_PyPegen_get_last_nonnwhitespace_token(Parser *p)
{
    assert(p->mark >= 0);
    Token *token = NULL;
    for (int m = p->mark - 1; m >= 0; m--) {
        token = p->tokens[m];
        if (token->type != ENDMARKER && (token->type < NEWLINE || token->type > DEDENT)) {
            break;
        }
    }
    return token;
}

/* Turn the UTF-8 text of a NAME token into an identifier: non-ASCII names
   are NFKC-normalized (PEP 3131); the result is an arena-owned JSAtom. */
JSAtom
_PyPegen_new_identifier(Parser *p, const char *n)
{
    static const char * const forbidden[] = {
        "None",
        "True",
        "False",
        NULL
    };
    JSRuntime *rt = p->ctx->rt;
    size_t len = strlen(n);
    const char *text = n;
    size_t text_len = len;
    DynBuf nfkc;
    uint32_t *cps = NULL, *norm = NULL;
    int i, ncps, nnorm;
    JSAtom id;

    dbuf_init(&nfkc);
    if (qp_utf8_check(p, n, (Py_ssize_t)len) < 0) {
        goto error;
    }
    /* Check whether there are non-ASCII characters in the
       identifier; if so, normalize to NFKC. */
    for (i = 0; (size_t)i < len; i++) {
        if ((unsigned char)n[i] >= 0x80) {
            break;
        }
    }
    if ((size_t)i < len) {
        const uint8_t *q = (const uint8_t *)n, *end = q + len;
        const char *reason;
        uint8_t buf[UTF8_CHAR_LEN_MAX];

        if (len > INT_MAX) {
            qp_pegen_no_memory(p);
            goto error;
        }
        cps = js_malloc_rt(rt, len * sizeof(uint32_t) + 1);
        if (cps == NULL) {
            qp_pegen_no_memory(p);
            goto error;
        }
        ncps = 0;
        while (q < end) {
            q += qp_utf8_decode1(q, end, &cps[ncps++], &reason);
        }
        nnorm = unicode_normalize(&norm, cps, ncps, UNICODE_NFKC, rt,
                                  (void *(*)(void *, void *, size_t))js_realloc_rt);
        if (nnorm < 0) {
            qp_pegen_no_memory(p);
            goto error;
        }
        for (i = 0; i < nnorm; i++) {
            dbuf_put(&nfkc, buf, unicode_to_utf8(buf, norm[i]));
        }
        if (dbuf_error(&nfkc)) {
            qp_pegen_no_memory(p);
            goto error;
        }
        text = (const char *)nfkc.buf;
        text_len = nfkc.size;
    }
    for (i = 0; forbidden[i] != NULL; i++) {
        if (text_len == strlen(forbidden[i]) &&
            memcmp(text, forbidden[i], text_len) == 0) {
            qp_error_clear(p->err);
            qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0,
                         "identifier field can't represent '%s' constant",
                         forbidden[i]);
            goto error;
        }
    }
    id = JS_NewAtomLen(p->ctx, text_len ? text : "", text_len);
    if (id == JS_ATOM_NULL) {
        qp_pegen_js_error(p);
        goto error;
    }
    if (qp_arena_add_atom(p->arena, id) < 0) {
        goto error;
    }
    js_free_rt(rt, cps);
    js_free_rt(rt, norm);
    dbuf_free(&nfkc);
    return id;

error:
    js_free_rt(rt, cps);
    js_free_rt(rt, norm);
    dbuf_free(&nfkc);
    p->error_indicator = 1;
    return JS_ATOM_NULL;
}

static expr_ty
_PyPegen_name_from_token(Parser *p, Token* t)
{
    if (t == NULL) {
        return NULL;
    }
    const char *s = t->bytes;
    if (!s) {
        p->error_indicator = 1;
        return NULL;
    }
    JSAtom id = _PyPegen_new_identifier(p, s);
    if (id == JS_ATOM_NULL) {
        p->error_indicator = 1;
        return NULL;
    }
    return _PyAST_Name(id, Load, t->lineno, t->col_offset, t->end_lineno,
                       t->end_col_offset, p->arena);
}

expr_ty
_PyPegen_name_token(Parser *p)
{
    Token *t = _PyPegen_expect_token(p, NAME);
    return _PyPegen_name_from_token(p, t);
}

void *
_PyPegen_string_token(Parser *p)
{
    return _PyPegen_expect_token(p, STRING);
}

expr_ty _PyPegen_soft_keyword_token(Parser *p) {
    Token *t = _PyPegen_expect_token(p, NAME);
    if (t == NULL) {
        return NULL;
    }
    char *the_token = t->bytes;
    Py_ssize_t size = t->bytes_len;
    for (const char **keyword = p->soft_keywords; *keyword != NULL; keyword++) {
        if (strlen(*keyword) == (size_t)size &&
            strncmp(*keyword, the_token, (size_t)size) == 0) {
            return _PyPegen_name_from_token(p, t);
        }
    }
    return NULL;
}

/* Default sys.int_info.default_max_str_digits / str_digits_check_threshold
   (Include/internal/pycore_long.h). */
#define QP_LONG_DEFAULT_MAX_STR_DIGITS 4300
#define QP_LONG_MAX_STR_DIGITS_THRESHOLD 640

/* parsenumber_raw(): s is a NUMBER token without underscores.  Integers are
   exact (any size, any base), floats and imaginary parts are correctly
   rounded doubles (PyOS_string_to_double: overflow gives inf).  Returns NULL
   with MemoryError, or with ValueError for the decimal digit limit of
   PyLong_FromString. */
static int qp_parse_float(Parser *p, const char *s, double *res);

static qp_const *
parsenumber_raw(Parser *p, const char *s)
{
    JSContext *ctx = p->ctx;
    size_t len = strlen(s);
    const char *digits = s;
    int radix = 10;
    int i;

    assert(s != NULL && len > 0);
    if (s[len - 1] == 'j' || s[len - 1] == 'J') {
        /* compl.real = 0.; compl.imag = PyOS_string_to_double(s, &end, NULL); */
        double imag;
        char *body = PyMem_Malloc(len);
        if (body == NULL) {
            return PyErr_NoMemory();
        }
        memcpy(body, s, len - 1);
        body[len - 1] = '\0';
        int r = qp_parse_float(p, body, &imag);
        PyMem_Free(body);
        if (r < 0) {
            return PyErr_NoMemory();
        }
        return qp_const_new(p->arena, QP_CONST_COMPLEX, __JS_NewFloat64(ctx, imag));
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        radix = 16;
        digits = s + 2;
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        radix = 8;
        digits = s + 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        radix = 2;
        digits = s + 2;
    } else {
        for (i = 0; s[i] != '\0'; i++) {
            if (!(s[i] >= '0' && s[i] <= '9')) {
                /* PyFloat_FromDouble(PyOS_string_to_double(s, NULL, NULL)) */
                double d;
                if (qp_parse_float(p, s, &d) < 0) {
                    return PyErr_NoMemory();
                }
                return qp_const_new(p->arena, QP_CONST_FLOAT, __JS_NewFloat64(ctx, d));
            }
        }
    }

    /* Integer.  Small values are made directly; others go through the
       QuickJS bigint parser, like PyLong_FromString. */
    size_t ndigits = strlen(digits);
    uint64_t v = 0;
    int overflow = 0;
    for (i = 0; digits[i] != '\0'; i++) {
        int d = digits[i] <= '9' ? digits[i] - '0' : (digits[i] | 0x20) - 'a' + 10;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)radix) {
            overflow = 1;
            break;
        }
        v = v * (uint64_t)radix + (uint64_t)d;
    }
    if (!overflow) {
        JSValue val;
        if (v <= INT32_MAX) {
            val = JS_NewInt32(ctx, (int32_t)v);
        } else if (v <= INT64_MAX) {
            val = JS_NewBigInt64(ctx, (int64_t)v);
        } else {
            val = JS_NewBigUint64(ctx, v);
        }
        if (JS_IsException(val)) {
            qp_pegen_js_error(p);
            return NULL;
        }
        return qp_const_new(p->arena, QP_CONST_INT, val);
    }
    if (radix == 10 && ndigits > QP_LONG_MAX_STR_DIGITS_THRESHOLD &&
        ndigits > QP_LONG_DEFAULT_MAX_STR_DIGITS) {
        qp_error_clear(p->err);
        qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "Exceeds the limit (%d digits) for integer string conversion: "
                     "value has %lld digits; use sys.set_int_max_str_digits() "
                     "to increase the limit",
                     QP_LONG_DEFAULT_MAX_STR_DIGITS, (long long)ndigits);
        return NULL;
    }
    JSBigInt *r = js_bigint_from_string(ctx, digits, radix);
    if (r == NULL) {
        qp_pegen_js_error(p);
        return NULL;
    }
    return qp_const_new(p->arena, QP_CONST_INT, JS_CompactBigInt(ctx, r));
}

static qp_const *
parsenumber(Parser *p, const char *s)
{
    char *dup;
    char *end;
    qp_const *res = NULL;

    assert(s != NULL);

    if (strchr(s, '_') == NULL) {
        return parsenumber_raw(p, s);
    }
    /* Create a duplicate without underscores. */
    dup = PyMem_Malloc(strlen(s) + 1);
    if (dup == NULL) {
        return PyErr_NoMemory();
    }
    end = dup;
    for (; *s; s++) {
        if (*s != '_') {
            *end++ = *s;
        }
    }
    *end = '\0';
    res = parsenumber_raw(p, dup);
    PyMem_Free(dup);
    return res;
}

expr_ty
_PyPegen_number_token(Parser *p)
{
    Token *t = _PyPegen_expect_token(p, NUMBER);
    if (t == NULL) {
        return NULL;
    }

    const char *num_raw = t->bytes;
    if (num_raw == NULL) {
        p->error_indicator = 1;
        return NULL;
    }

    if (p->feature_version < 6 && strchr(num_raw, '_') != NULL) {
        p->error_indicator = 1;
        return RAISE_SYNTAX_ERROR("Underscores in numeric literals are only supported "
                                  "in Python 3.6 and greater");
    }

    qp_const *c = parsenumber(p, num_raw);

    if (c == NULL) {
        p->error_indicator = 1;
        // The only way a ValueError should happen in _this_ code is via
        // PyLong_FromString hitting a length limit.
        if (p->err->kind == QP_ERR_VALUE) {
            char *exc = p->err->msg;
            p->err->msg = NULL;
            qp_error_clear(p->err);
            /* Intentionally omitting columns to avoid a wall of 1000s of '^'s
             * on the error message. Nobody is going to overlook their huge
             * numeric literal once given the line. */
            RAISE_ERROR_KNOWN_LOCATION(
                p, QP_ERR_SYNTAX,
                t->lineno, -1 /* col_offset */,
                t->end_lineno, -1 /* end_col_offset */,
                "%s - Consider hexadecimal for huge integer literals "
                "to avoid decimal conversion limits.",
                exc ? exc : "");
            qp_sys_free(exc);
        }
        return NULL;
    }

    return _PyAST_Constant(c, JS_ATOM_NULL, t->lineno, t->col_offset, t->end_lineno,
                           t->end_col_offset, p->arena);
}

/* Check that the source for a single input statement really is a single
   statement by looking at what is left in the buffer after parsing.
   Trailing whitespace and comments are OK. */
static int // bool
bad_single_statement(Parser *p)
{
    char *cur = p->tok->cur;
    char c = *cur;

    for (;;) {
        while (c == ' ' || c == '\t' || c == '\n' || c == '\014') {
            c = *++cur;
        }

        if (!c) {
            return 0;
        }

        if (c != '#') {
            return 1;
        }

        /* Suck up comment. */
        while (c && c != '\n') {
            c = *++cur;
        }
    }
}

static int
compute_parser_flags(int cf_flags)
{
    int parser_flags = 0;
    if (cf_flags & QP_PF_DONT_IMPLY_DEDENT) {
        parser_flags |= PyPARSE_DONT_IMPLY_DEDENT;
    }
    if (cf_flags & (QP_PF_IGNORE_COOKIE | QP_PF_SOURCE_IS_UTF8)) {
        parser_flags |= PyPARSE_IGNORE_COOKIE;
    }
    if (cf_flags & QP_PF_BARRY_AS_BDFL) {
        parser_flags |= PyPARSE_BARRY_AS_BDFL;
    }
    if (cf_flags & QP_PF_TYPE_COMMENTS) {
        parser_flags |= PyPARSE_TYPE_COMMENTS;
    }
    if (cf_flags & QP_PF_ALLOW_INCOMPLETE_INPUT) {
        parser_flags |= PyPARSE_ALLOW_INCOMPLETE_INPUT;
    }
    return parser_flags;
}

// Parser API

Parser *
_PyPegen_Parser_New(struct tok_state *tok, int start_rule, int flags,
                    int feature_version, int *errcode, const char* source, PyArena *arena)
{
    JSRuntime *rt = qp_arena_ctx(arena)->rt;
    Parser *p = js_malloc_rt(rt, sizeof(Parser));
    if (p == NULL) {
        qp_error_clear(tok->err);
        qp_error_set(tok->err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
        return NULL;
    }
    assert(tok != NULL);
    (void)source;
    p->ctx = qp_arena_ctx(arena);
    p->err = tok->err;
    tok->type_comments = (flags & PyPARSE_TYPE_COMMENTS) > 0;
    p->tok = tok;
    p->keywords = NULL;
    p->n_keyword_lists = -1;
    p->soft_keywords = NULL;
    p->tokens = PyMem_Malloc(sizeof(Token *));
    if (!p->tokens) {
        goto nomem;
    }
    p->tokens[0] = PyMem_Calloc(1, sizeof(Token));
    if (!p->tokens[0]) {
        PyMem_Free(p->tokens);
        goto nomem;
    }
    if (!growable_comment_array_init(p, &p->type_ignore_comments, 10)) {
        PyMem_Free(p->tokens[0]);
        PyMem_Free(p->tokens);
        goto nomem;
    }

    p->mark = 0;
    p->fill = 0;
    p->size = 1;

    p->errcode = errcode;
    p->arena = arena;
    p->start_rule = start_rule;
    p->parsing_started = 0;
    p->error_indicator = 0;

    p->starting_lineno = 0;
    p->starting_col_offset = 0;
    p->flags = flags;
    p->feature_version = feature_version;
    p->known_err_token = NULL;
    p->level = 0;
    p->call_invalid_rules = 0;
    p->last_stmt_location.lineno = 0;
    p->last_stmt_location.col_offset = 0;
    p->last_stmt_location.end_lineno = 0;
    p->last_stmt_location.end_col_offset = 0;
    p->debug = 0;
    return p;

nomem:
    PyErr_NoMemory();
    js_free_rt(rt, p);
    return NULL;
}

void
_PyPegen_Parser_Free(Parser *p)
{
    for (int i = 0; i < p->size; i++) {
        PyMem_Free(p->tokens[i]);
    }
    PyMem_Free(p->tokens);
    growable_comment_array_deallocate(p, &p->type_ignore_comments);
    PyMem_Free(p);
}

static void
reset_parser_state_for_error_pass(Parser *p)
{
    p->last_stmt_location.lineno = 0;
    p->last_stmt_location.col_offset = 0;
    p->last_stmt_location.end_lineno = 0;
    p->last_stmt_location.end_col_offset = 0;
    for (int i = 0; i < p->fill; i++) {
        p->tokens[i]->memo = NULL;
    }
    p->mark = 0;
    p->call_invalid_rules = 1;
    // Don't try to get extra tokens in interactive mode when trying to
    // raise specialized errors in the second pass.
    p->tok->interactive_underflow = IUNDERFLOW_STOP;
}

static inline int
_is_end_of_source(Parser *p) {
    int err = p->tok->done;
    return err == E_EOF || err == E_EOFS || err == E_EOLS;
}

/* PyErr_ExceptionMatches(PyExc_SyntaxError): the pending error is a
   SyntaxError or one of its subclasses. */
static inline int
qp_pegen_is_syntax_error(Parser *p)
{
    qp_error_kind k = p->err->kind;
    return k == QP_ERR_SYNTAX || k == QP_ERR_INDENTATION || k == QP_ERR_TAB ||
           k == QP_ERR_INCOMPLETE;
}

void *
_PyPegen_run_parser(Parser *p)
{
    void *res = _PyPegen_parse(p);
    assert(p->level == 0);
    if (res != NULL && PyErr_Occurred()) {
        // Discard a result returned with an exception still pending
        // (e.g. a MemoryError from a recovered-from allocation failure).
        return NULL;
    }
    if (res == NULL) {
        if ((p->flags & PyPARSE_ALLOW_INCOMPLETE_INPUT) &&  _is_end_of_source(p)) {
            qp_error_clear(p->err);
            return _PyPegen_raise_error(p, QP_ERR_INCOMPLETE, 0, "incomplete input");
        }
        if (PyErr_Occurred() && !qp_pegen_is_syntax_error(p)) {
            return NULL;
        }
       // Make a second parser pass. In this pass we activate heavier and slower checks
        // to produce better error messages and more complete diagnostics. Extra "invalid_*"
        // rules will be active during parsing.
        Token *last_token = p->tokens[p->fill - 1];
        reset_parser_state_for_error_pass(p);
        _PyPegen_parse(p);

        // Set SyntaxErrors accordingly depending on the parser/tokenizer status at the failure
        // point.
        _Pypegen_set_syntax_error(p, last_token);

        // CPython attaches p->last_stmt_location and the source to the
        // SyntaxError here (_PyPegen_set_syntax_error_metadata); qp_error
        // carries no such metadata.
       return NULL;
    }

    if (p->start_rule == Py_single_input && bad_single_statement(p)) {
        p->tok->done = E_BADSINGLE; // This is not necessary for now, but might be in the future
        return RAISE_SYNTAX_ERROR("multiple statements found while compiling a single statement");
    }

    return res;
}

mod_ty
_PyPegen_run_parser_from_string(const char *str, int start_rule, const char *filename,
                                int cf_flags, PyArena *arena)
{
    qp_error *err = qp_arena_error(arena);
    int exec_input = start_rule == Py_file_input;
    int parser_flags = compute_parser_flags(cf_flags);

    struct tok_state *tok;
    if (parser_flags & PyPARSE_IGNORE_COOKIE) {
        tok = _PyTokenizer_FromUTF8(str, exec_input, 0, err);
    } else {
        tok = _PyTokenizer_FromString(str, exec_input, 0, err);
    }
    if (tok == NULL) {
        // Initialization errors are already converted by the tokenizer
        // (_PyTokenizer_raise_init_error); the only silent failure is a
        // failed allocation.
        if (err->kind == QP_ERR_NONE) {
            qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
        }
        return NULL;
    }
    tok->barry_as_bdfl = parser_flags & PyPARSE_BARRY_AS_BDFL;
    tok->filename = filename;
    tok->module = NULL;

    // We need to clear up from here on
    mod_ty result = NULL;

    // compile() and ast.parse() always parse with the current grammar
    // (cf_feature_version defaults to PY_MINOR_VERSION).
    int feature_version = 15;
    Parser *p = _PyPegen_Parser_New(tok, start_rule, parser_flags, feature_version,
                                    NULL, str, arena);
    if (p == NULL) {
        goto error;
    }

    result = _PyPegen_run_parser(p);
    _PyPegen_Parser_Free(p);

error:
    _PyTokenizer_Free(tok);
    return result;
}

/* ---------------------------------------------------------------------- */
/* Parser/peg_api.c */

mod_ty qp_parse_string(JSContext *ctx, qp_arena *arena, const char *src, size_t len,
                       const char *filename, int mode, int flags, qp_error *err)
{
    mod_ty result;
    char *str;

    assert(qp_arena_error(arena) == err);
    assert(qp_arena_ctx(arena) == ctx);
    /* The tokenizer works on a NUL-terminated string. */
    str = js_malloc(ctx, len + 1);
    if (str == NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
        return NULL;
    }
    memcpy(str, src, len);
    str[len] = '\0';
    result = _PyPegen_run_parser_from_string(str, mode, filename, flags, arena);
    js_free(ctx, str);
    if (result == NULL && err->kind == QP_ERR_NONE) {
        qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
    }
    return result;
}

/* ---------------------------------------------------------------------- */
/* Correctly rounded decimal -> double conversion (PyOS_string_to_double)  */

/* js_atod() is exact for up to 38 significant decimal digits and truncates
   longer inputs; longer literals are corrected with an exact comparison
   against the rounding midpoint, using this minimal bignum. */
typedef struct {
    uint32_t *d;        /* little-endian base 2^32 limbs */
    int len, cap;
} qp_bignum;

static int qp_bignum_reserve(JSRuntime *rt, qp_bignum *a, int cap)
{
    if (cap > a->cap) {
        uint32_t *d = js_realloc_rt(rt, a->d, (size_t)cap * sizeof(uint32_t));
        if (d == NULL)
            return -1;
        a->d = d;
        a->cap = cap;
    }
    return 0;
}

/* a = a * m + add */
static int qp_bignum_mul_add(JSRuntime *rt, qp_bignum *a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    for (int i = 0; i < a->len; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (qp_bignum_reserve(rt, a, a->len + 1) < 0)
            return -1;
        a->d[a->len++] = (uint32_t)carry;
    }
    return 0;
}

/* a = a * 10^n */
static int qp_bignum_mul_pow10(JSRuntime *rt, qp_bignum *a, int n)
{
    static const uint32_t pow10[10] = {
        1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000
    };
    while (n >= 9) {
        if (qp_bignum_mul_add(rt, a, pow10[9], 0) < 0)
            return -1;
        n -= 9;
    }
    return n > 0 ? qp_bignum_mul_add(rt, a, pow10[n], 0) : 0;
}

/* a = a * 2^n */
static int qp_bignum_shl(JSRuntime *rt, qp_bignum *a, int n)
{
    int limbs = n / 32, bits = n % 32, i;
    if (a->len == 0)
        return 0;
    if (qp_bignum_reserve(rt, a, a->len + limbs + 1) < 0)
        return -1;
    a->d[a->len + limbs] = 0;
    for (i = a->len - 1; i >= 0; i--) {
        uint32_t v = a->d[i];
        if (bits) {
            a->d[i + limbs + 1] |= v >> (32 - bits);
            a->d[i + limbs] = v << bits;
        } else {
            a->d[i + limbs] = v;
        }
    }
    for (i = 0; i < limbs; i++)
        a->d[i] = 0;
    a->len += limbs + 1;
    while (a->len > 0 && a->d[a->len - 1] == 0)
        a->len--;
    return 0;
}

static int qp_bignum_cmp(const qp_bignum *a, const qp_bignum *b)
{
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    for (int i = a->len - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* Split a finite positive double (or +inf, as 2^1024) into m * 2^e with an
   integer m < 2^54. */
static void qp_double_split(double x, uint64_t *m, int *e)
{
    uint64_t bits;
    memcpy(&bits, &x, sizeof(bits));
    int biased = (int)(bits >> 52) & 0x7FF;
    uint64_t frac = bits & (((uint64_t)1 << 52) - 1);
    if (biased == 0x7FF) {          /* +inf */
        *m = (uint64_t)1 << 52;
        *e = 1024 - 52;
    } else if (biased == 0) {       /* zero or subnormal */
        *m = frac;
        *e = -1074;
    } else {
        *m = frac | ((uint64_t)1 << 52);
        *e = biased - 1075;
    }
}

/* PyOS_string_to_double(s, NULL, NULL) for a Python float literal body
   (digits, optional '.', optional exponent; no sign, no underscores):
   correctly rounded, round-half-even, overflow to inf. Returns -1 on OOM. */
static int qp_parse_float(Parser *p, const char *s, double *res)
{
    JSRuntime *rt = p->ctx->rt;
    JSATODTempMem atod_mem;
    const char *q, *first = NULL, *exp_part = NULL;
    Py_ssize_t ndigits = 0, int_digits = 0, i;
    long long exp10 = 0;
    int seen_dot = 0, ret = -1;
    qp_bignum lhs = { NULL, 0, 0 }, rhs = { NULL, 0, 0 };

    /* Collect the significant digits: value = D * 10^exp10. */
    for (q = s; *q; q++) {
        if (*q == '.') {
            seen_dot = 1;
        } else if (*q >= '0' && *q <= '9') {
            if (first == NULL && *q == '0') {
                continue;
            }
            if (first == NULL)
                first = q;
            ndigits++;
            if (!seen_dot)
                int_digits++;
        } else {
            exp_part = q;
            break;
        }
    }
    if (ndigits <= 38) {
        *res = js_atod(s, NULL, 10, 0, &atod_mem);
        return 0;
    }
    if (exp_part != NULL) {
        const char *e = exp_part + 1;
        int neg = 0;
        if (*e == '+' || *e == '-') {
            neg = *e == '-';
            e++;
        }
        for (; *e >= '0' && *e <= '9'; e++) {
            if (exp10 < 1000000000LL)
                exp10 = exp10 * 10 + (*e - '0');
        }
        if (neg)
            exp10 = -exp10;
    }

    /* Digits of D (without the dot), and its decimal exponent. */
    char *digits = js_malloc_rt(rt, (size_t)ndigits + 64);
    if (digits == NULL)
        return -1;
    i = 0;
    for (q = first; i < ndigits; q++) {
        if (*q != '.')
            digits[i++] = *q;
    }
    /* first significant digit is at 10^(int_digits - 1) if it is in the
       integer part; otherwise the fraction digits before it were zeros. */
    if (int_digits > 0) {
        exp10 += int_digits - ndigits;
    } else {
        Py_ssize_t frac_zeros = 0;
        const char *dot = strchr(s, '.');
        for (q = dot + 1; q < first; q++)
            frac_zeros++;
        exp10 -= frac_zeros + ndigits;
    }
    while (ndigits > 0 && digits[ndigits - 1] == '0') {
        ndigits--;
        exp10++;
    }
    if (ndigits <= 38) {
        snprintf(digits + ndigits, 64, "e%lld", exp10);
        *res = js_atod(digits, NULL, 10, 0, &atod_mem);
        ret = 0;
        goto done;
    }
    /* magnitude: 10^(ndigits + exp10 - 1) <= v < 10^(ndigits + exp10) */
    if (ndigits + exp10 > 310) {
        *res = INFINITY;
        ret = 0;
        goto done;
    }
    if (ndigits + exp10 < -330) {
        *res = 0.0;
        ret = 0;
        goto done;
    }

    /* Candidate: the correctly rounded value of the 38-digit truncation T
       (T <= v < T + 10^-37 * T), so the result is x or its successor. */
    {
        char tbuf[64];
        double x;
        uint64_t xbits, mx, mxp, mid;
        int ex, exp_, k, cmp;

        memcpy(tbuf, digits, 38);
        snprintf(tbuf + 38, sizeof(tbuf) - 38, "e%lld", exp10 + (ndigits - 38));
        x = js_atod(tbuf, NULL, 10, 0, &atod_mem);
        if (isinf(x)) {
            *res = x;
            ret = 0;
            goto done;
        }
        memcpy(&xbits, &x, sizeof(xbits));
        double xp;
        uint64_t xpbits = xbits + 1;
        memcpy(&xp, &xpbits, sizeof(xp));
        qp_double_split(x, &mx, &ex);
        qp_double_split(xp, &mxp, &exp_);
        /* align to the smaller exponent: midpoint = (mx + mxp') * 2^(ex - 1) */
        if (exp_ > ex)
            mxp <<= (exp_ - ex);
        mid = mx + mxp;
        k = ex - 1;

        /* compare D * 10^exp10 with mid * 2^k */
        for (i = 0; i < ndigits; i++) {
            if (qp_bignum_mul_add(rt, &lhs, 10, (uint32_t)(digits[i] - '0')) < 0)
                goto done;
        }
        if (qp_bignum_reserve(rt, &rhs, 2) < 0)
            goto done;
        rhs.d[0] = (uint32_t)mid;
        rhs.d[1] = (uint32_t)(mid >> 32);
        rhs.len = rhs.d[1] ? 2 : 1;
        if (exp10 > 0) {
            if (qp_bignum_mul_pow10(rt, &lhs, (int)exp10) < 0)
                goto done;
        } else if (exp10 < 0) {
            if (qp_bignum_mul_pow10(rt, &rhs, (int)-exp10) < 0)
                goto done;
        }
        if (k > 0) {
            if (qp_bignum_shl(rt, &rhs, k) < 0)
                goto done;
        } else if (k < 0) {
            if (qp_bignum_shl(rt, &lhs, -k) < 0)
                goto done;
        }
        cmp = qp_bignum_cmp(&lhs, &rhs);
        if (cmp > 0 || (cmp == 0 && (xbits & 1)))
            *res = xp;
        else
            *res = x;
        ret = 0;
    }
done:
    js_free_rt(rt, digits);
    js_free_rt(rt, lhs.d);
    js_free_rt(rt, rhs.d);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Parser/pegen_errors.c */

// TOKENIZER ERRORS

static inline void
raise_unclosed_parentheses_error(Parser *p) {
       int error_lineno = p->tok->parenlinenostack[p->tok->level-1];
       int error_col = p->tok->parencolstack[p->tok->level-1];
       RAISE_ERROR_KNOWN_LOCATION(p, PyExc_SyntaxError,
                                  error_lineno, error_col, error_lineno, -1,
                                  "'%c' was never closed",
                                  p->tok->parenstack[p->tok->level-1]);
}

int
_Pypegen_tokenizer_error(Parser *p)
{
    if (PyErr_Occurred()) {
        return -1;
    }

    const char *msg = NULL;
    qp_error_kind errtype = PyExc_SyntaxError;
    Py_ssize_t col_offset = -1;
    p->error_indicator = 1;
    switch (p->tok->done) {
        case E_TOKEN:
            msg = "invalid token";
            break;
        case E_EOF:
            if (p->tok->level) {
                raise_unclosed_parentheses_error(p);
            } else {
                RAISE_SYNTAX_ERROR("unexpected EOF while parsing");
            }
            return -1;
        case E_DEDENT:
            RAISE_INDENTATION_ERROR("unindent does not match any outer indentation level");
            return -1;
        case E_NOMEM:
            PyErr_NoMemory();
            return -1;
        case E_TABSPACE:
            errtype = PyExc_TabError;
            msg = "inconsistent use of tabs and spaces in indentation";
            break;
        case E_TOODEEP:
            errtype = PyExc_IndentationError;
            msg = "too many levels of indentation";
            break;
        case E_LINECONT: {
            col_offset = p->tok->cur - p->tok->buf - 1;
            msg = "unexpected character after line continuation character";
            break;
        }
        case E_COLUMNOVERFLOW:
            qp_error_clear(p->err);
            qp_error_set(p->err, PyExc_OverflowError, 0, 0, 0, 0, "%s",
                         "Parser column offset overflow - source line is too big");
            return -1;
        default:
            /* includes E_INTR (KeyboardInterrupt), which the string
               tokenizer never reports */
            msg = "unknown parsing error";
    }

    RAISE_ERROR_KNOWN_LOCATION(p, errtype, p->tok->lineno,
                               col_offset >= 0 ? col_offset : 0,
                               p->tok->lineno, -1, msg);
    return -1;
}

int
_Pypegen_raise_decode_error(Parser *p)
{
    return _Pypegen_raise_decode_error_known_location(p, NULL);
}

/* A UnicodeError / ValueError raised while decoding becomes
   SyntaxError("(unicode error) ...") / SyntaxError("(value error) ...");
   other pending errors are left alone. */
int
_Pypegen_raise_decode_error_known_location(Parser *p, expr_ty a)
{
    assert(PyErr_Occurred());
    const char *errtype = NULL;
    if (p->err->kind == QP_ERR_UNICODE_DECODE) {
        errtype = "unicode error";
    }
    else if (p->err->kind == QP_ERR_VALUE) {
        errtype = "value error";
    }
    if (errtype) {
        char *errstr = p->err->msg;
        p->err->msg = NULL;
        qp_error_clear(p->err);
        if (errstr) {
            if (a != NULL) {
                RAISE_SYNTAX_ERROR_KNOWN_LOCATION(a, "(%s) %s", errtype, errstr);
            }
            else {
                RAISE_SYNTAX_ERROR("(%s) %s", errtype, errstr);
            }
            qp_sys_free(errstr);
        }
        else {
            if (a != NULL) {
                RAISE_SYNTAX_ERROR_KNOWN_LOCATION(a, "(%s) unknown error", errtype);
            }
            else {
                RAISE_SYNTAX_ERROR("(%s) unknown error", errtype);
            }
        }
    }

    return -1;
}

static int
_PyPegen_tokenize_full_source_to_check_for_errors(Parser *p) {
    // Tokenize the whole input to see if there are any tokenization
    // errors such as mismatching parentheses. These will get priority
    // over generic syntax errors only if the line number of the error is
    // before the one that we had for the generic error.

    // We don't want to tokenize to the end for interactive input
    if (p->tok->prompt != NULL) {
        return 0;
    }

    /* PyErr_Fetch */
    qp_error saved = *p->err;
    memset(p->err, 0, sizeof(*p->err));

    Token *current_token = p->known_err_token != NULL ? p->known_err_token : p->tokens[p->fill - 1];
    Py_ssize_t current_err_line = current_token->lineno;

    int ret = 0;
    struct token new_token;
    _PyToken_Init(&new_token);

    for (;;) {
        _PyToken_Free(&new_token);
        _PyToken_Init(&new_token);
        switch (_PyTokenizer_Get(p->tok, &new_token)) {
            case ERRORTOKEN:
                if (PyErr_Occurred()) {
                    ret = -1;
                    goto exit;
                }
                if (p->tok->level != 0) {
                    int error_lineno = p->tok->parenlinenostack[p->tok->level-1];
                    if (current_err_line > error_lineno) {
                        raise_unclosed_parentheses_error(p);
                        ret = -1;
                        goto exit;
                    }
                }
                break;
            case ENDMARKER:
                break;
            default:
                continue;
        }
        break;
    }


exit:
    _PyToken_Free(&new_token);
    // If we're in an f-string, we want the syntax error in the expression part
    // to propagate, so that tokenizer errors (like expecting '}') that happen afterwards
    // do not swallow it.
    if (PyErr_Occurred() && p->tok->tok_mode_stack_index <= 0) {
        qp_error_clear(&saved);
    } else {
        /* PyErr_Restore */
        qp_error_clear(p->err);
        *p->err = saved;
    }
    return ret;
}

// PARSER ERRORS

void *
_PyPegen_raise_error(Parser *p, qp_error_kind errtype, int use_mark, const char *errmsg, ...)
{
    // Bail out if we already have an error set.
    if (p->error_indicator && PyErr_Occurred()) {
        return NULL;
    }
    if (p->fill == 0) {
        va_list va;
        va_start(va, errmsg);
        _PyPegen_raise_error_known_location(p, errtype, 0, 0, 0, -1, errmsg, va);
        va_end(va);
        return NULL;
    }
    if (use_mark && p->mark == p->fill && _PyPegen_fill_token(p) < 0) {
        p->error_indicator = 1;
        return NULL;
    }
    Token *t = p->known_err_token != NULL
                   ? p->known_err_token
                   : p->tokens[use_mark ? p->mark : p->fill - 1];
    Py_ssize_t col_offset;
    Py_ssize_t end_col_offset = -1;
    if (t->col_offset == -1) {
        if (p->tok->cur == p->tok->buf) {
            col_offset = 0;
        } else {
            const char* start = p->tok->buf  ? p->tok->line_start : p->tok->buf;
            col_offset = (int)(p->tok->cur - start);
        }
    } else {
        col_offset = t->col_offset + 1;
    }

    if (t->end_col_offset != -1) {
        end_col_offset = t->end_col_offset + 1;
    }

    va_list va;
    va_start(va, errmsg);
    _PyPegen_raise_error_known_location(p, errtype, t->lineno, col_offset, t->end_lineno, end_col_offset, errmsg, va);
    va_end(va);

    return NULL;
}

/* Source line `lineno` from the tokenizer's copy of the whole input,
   without its newline, appended to b as UTF-8 (decoded with "replace"). */
static int
get_error_line_from_tokenizer_buffers(Parser *p, Py_ssize_t lineno, DynBuf *b)
{
    /* If the file descriptor is interactive, the source lines of the current
     * (multi-line) statement are stored in p->tok->interactive_src_start.
     * If not, we're parsing from a string, which means that the whole source
     * is stored in p->tok->str. */
    assert((p->tok->fp == NULL && p->tok->str != NULL) || p->tok->fp != NULL);

    char *cur_line = p->tok->fp_interactive ? p->tok->interactive_src_start : p->tok->str;
    if (cur_line == NULL) {
        assert(p->tok->fp_interactive);
        // We can reach this point if the tokenizer buffers for interactive source have not been
        // initialized because we failed to decode the original source with the given locale.
        return qp_utf8_replace(b, "", 0);
    }

    Py_ssize_t relative_lineno = p->starting_lineno ? lineno - p->starting_lineno + 1 : lineno;
    const char* buf_end = p->tok->fp_interactive ? p->tok->interactive_src_end : p->tok->inp;

    if (buf_end < cur_line) {
        buf_end = cur_line + strlen(cur_line);
    }

    for (int i = 0; i < relative_lineno - 1; i++) {
        char *new_line = strchr(cur_line, '\n');
        // The assert is here for debug builds but the conditional that
        // follows is there so in release builds we do not crash at the cost
        // to report a potentially wrong line.
        if (new_line == NULL || new_line + 1 > buf_end) {
            break;
        }
        cur_line = new_line + 1;
    }

    char *next_newline;
    if ((next_newline = strchr(cur_line, '\n')) == NULL) { // This is the last line
        next_newline = cur_line + strlen(cur_line);
    }
    return qp_utf8_replace(b, cur_line, next_newline - cur_line);
}

void *
_PyPegen_raise_error_known_location(Parser *p, qp_error_kind errtype,
                                    Py_ssize_t lineno, Py_ssize_t col_offset,
                                    Py_ssize_t end_lineno, Py_ssize_t end_col_offset,
                                    const char *errmsg, va_list va)
{
    // Bail out if we already have an error set.
    if (p->error_indicator && PyErr_Occurred()) {
        return NULL;
    }
    DynBuf errstr, error_line;
    dbuf_init(&errstr);
    dbuf_init(&error_line);
    p->error_indicator = 1;

    if (end_lineno == CURRENT_POS) {
        end_lineno = p->tok->lineno;
    }
    if (end_col_offset == CURRENT_POS) {
        end_col_offset = p->tok->cur - p->tok->line_start;
    }

    if (qp_pegen_vformat(p, &errstr, errmsg, va) < 0) {
        goto error;
    }

    /* CPython first tries the interactive source buffer, then (for
       Py_file_input) re-reads the line from the file named by the filename
       (_PyErr_ProgramDecodedTextObject).  There is no interactive input
       here and no file is read: string sources fall through to the
       tokenizer buffers, as they do in CPython. */
    if (p->tok->lineno <= lineno && p->tok->inp > p->tok->buf) {
        Py_ssize_t size = p->tok->inp - p->tok->line_start;
        if (qp_utf8_replace(&error_line, p->tok->line_start, size) < 0) {
            goto oom;
        }
    }
    else {
        if (get_error_line_from_tokenizer_buffers(p, lineno, &error_line) < 0) {
            goto oom;
        }
    }

    Py_ssize_t col_number = col_offset;
    Py_ssize_t end_col_number = end_col_offset;

    col_number = _PyPegen_byte_offset_to_character_offset((const char *)error_line.buf, col_offset);
    if (col_number < 0) {
        goto error;
    }

    if (end_col_offset > 0) {
        end_col_number = _PyPegen_byte_offset_to_character_offset((const char *)error_line.buf, end_col_offset);
        if (end_col_number < 0) {
            goto error;
        }
    }

    /* PyErr_SetObject(errtype, (msg, (filename, lineno, offset, text, end_lineno, end_offset))) */
    qp_error_clear(p->err);
    qp_error_set(p->err, errtype, (int)lineno, (int)col_number, (int)end_lineno,
                 (int)end_col_number, "%s", (const char *)errstr.buf);
    qp_error_set_text(p->err, (const char *)error_line.buf, error_line.size);

    dbuf_free(&errstr);
    dbuf_free(&error_line);
    return NULL;

oom:
    PyErr_NoMemory();
error:
    dbuf_free(&errstr);
    dbuf_free(&error_line);
    return NULL;
}

void
_Pypegen_set_syntax_error(Parser* p, Token* last_token) {
    // Existing syntax error
    if (PyErr_Occurred()) {
        // Prioritize tokenizer errors to custom syntax errors raised
        // on the second phase only if the errors come from the parser.
        int is_tok_ok = (p->tok->done == E_DONE || p->tok->done == E_OK);
        if (is_tok_ok && qp_pegen_is_syntax_error(p)) {
            _PyPegen_tokenize_full_source_to_check_for_errors(p);
        }
        // Propagate the existing syntax error.
        return;
    }
    // Initialization error
    if (p->fill == 0) {
        RAISE_SYNTAX_ERROR("error at start before reading any input");
    }
    // Parser encountered EOF (End of File) unexpectedtly
    if (last_token->type == ERRORTOKEN && p->tok->done == E_EOF) {
        if (p->tok->level) {
            raise_unclosed_parentheses_error(p);
        } else {
            RAISE_SYNTAX_ERROR("unexpected EOF while parsing");
        }
        return;
    }
    // Indentation error in the tokenizer
    if (last_token->type == INDENT || last_token->type == DEDENT) {
        RAISE_INDENTATION_ERROR(last_token->type == INDENT ? "unexpected indent" : "unexpected unindent");
        return;
    }
    // Unknown error (generic case)

    // Use the last token we found on the first pass to avoid reporting
    // incorrect locations for generic syntax errors just because we reached
    // further away when trying to find specific syntax errors in the second
    // pass.
    RAISE_SYNTAX_ERROR_KNOWN_LOCATION(last_token, "invalid syntax");
    // _PyPegen_tokenize_full_source_to_check_for_errors will override the existing
    // generic SyntaxError we just raised if errors are found.
    _PyPegen_tokenize_full_source_to_check_for_errors(p);
}

void
_Pypegen_stack_overflow(Parser *p)
{
    p->error_indicator = 1;
    qp_error_clear(p->err);
    qp_error_set(p->err, PyExc_MemoryError, 0, 0, 0, 0, "%s",
                 "Parser stack overflowed - Python source too complex to parse");
}

/* ---------------------------------------------------------------------- */
/* Parser/action_helpers.c */

/* The placeholder Name node (_PyRuntime.parser.dummy_name).  Grammar
   alternatives without an action (e.g. the second invalid_def_raw
   alternative) return it in place of any node type, and actions of the
   enclosing rule then read it as that type: _PyPegen_function_def_decorators
   builds a FunctionDef from its "name" and "args" fields.  In CPython those
   reads land on the Name's id pointer and ctx value, which are non-NULL, so
   the required-field checks pass.  With JSAtom identifiers the layouts no
   longer overlap that way, so the storage is shared with a stmt whose
   FunctionDef/ClassDef name (= the Name id) and args are non-NULL; it is
   also large enough for any node view. */
static union {
    struct _expr e;
    struct _stmt s;
} qp_pegen_dummy;
static struct _arguments qp_pegen_dummy_arguments;

void *
_PyPegen_dummy_name(Parser *p, ...)
{
    if (qp_pegen_dummy.e.kind != Name_kind) {
        qp_pegen_dummy.s.v.FunctionDef.args = &qp_pegen_dummy_arguments;
        qp_pegen_dummy.e.v.Name.id = JS_ATOM_empty_string;
        qp_pegen_dummy.e.v.Name.ctx = Load;
        qp_pegen_dummy.e.lineno = 1;
        qp_pegen_dummy.e.col_offset = 0;
        qp_pegen_dummy.e.end_lineno = 1;
        qp_pegen_dummy.e.end_col_offset = 0;
        qp_pegen_dummy.e.kind = Name_kind;
    }
    return &qp_pegen_dummy.e;
}

/* Creates a single-element asdl_seq* that contains a */
asdl_seq *
_PyPegen_singleton_seq(Parser *p, void *a)
{
    assert(a != NULL);
    asdl_seq *seq = (asdl_seq*)_Py_asdl_generic_seq_new(1, p->arena);
    if (!seq) {
        return NULL;
    }
    asdl_seq_SET_UNTYPED(seq, 0, a);
    return seq;
}

/* Creates a copy of seq and prepends a to it */
asdl_seq *
_PyPegen_seq_insert_in_front(Parser *p, void *a, asdl_seq *seq)
{
    assert(a != NULL);
    if (!seq) {
        return _PyPegen_singleton_seq(p, a);
    }

    asdl_seq *new_seq = (asdl_seq*)_Py_asdl_generic_seq_new(asdl_seq_LEN(seq) + 1, p->arena);
    if (!new_seq) {
        return NULL;
    }

    asdl_seq_SET_UNTYPED(new_seq, 0, a);
    for (Py_ssize_t i = 1, l = asdl_seq_LEN(new_seq); i < l; i++) {
        asdl_seq_SET_UNTYPED(new_seq, i, asdl_seq_GET_UNTYPED(seq, i - 1));
    }
    return new_seq;
}

/* Creates a copy of seq and appends a to it */
asdl_seq *
_PyPegen_seq_append_to_end(Parser *p, asdl_seq *seq, void *a)
{
    assert(a != NULL);
    if (!seq) {
        return _PyPegen_singleton_seq(p, a);
    }

    asdl_seq *new_seq = (asdl_seq*)_Py_asdl_generic_seq_new(asdl_seq_LEN(seq) + 1, p->arena);
    if (!new_seq) {
        return NULL;
    }

    for (Py_ssize_t i = 0, l = asdl_seq_LEN(new_seq); i + 1 < l; i++) {
        asdl_seq_SET_UNTYPED(new_seq, i, asdl_seq_GET_UNTYPED(seq, i));
    }
    asdl_seq_SET_UNTYPED(new_seq, asdl_seq_LEN(new_seq) - 1, a);
    return new_seq;
}

static Py_ssize_t
_get_flattened_seq_size(asdl_seq *seqs)
{
    Py_ssize_t size = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seqs); i < l; i++) {
        asdl_seq *inner_seq = asdl_seq_GET_UNTYPED(seqs, i);
        size += asdl_seq_LEN(inner_seq);
    }
    return size;
}

/* Flattens an asdl_seq* of asdl_seq*s */
asdl_seq *
_PyPegen_seq_flatten(Parser *p, asdl_seq *seqs)
{
    Py_ssize_t flattened_seq_size = _get_flattened_seq_size(seqs);
    assert(flattened_seq_size > 0);

    asdl_seq *flattened_seq = (asdl_seq*)_Py_asdl_generic_seq_new(flattened_seq_size, p->arena);
    if (!flattened_seq) {
        return NULL;
    }

    int flattened_seq_idx = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seqs); i < l; i++) {
        asdl_seq *inner_seq = asdl_seq_GET_UNTYPED(seqs, i);
        for (Py_ssize_t j = 0, li = asdl_seq_LEN(inner_seq); j < li; j++) {
            asdl_seq_SET_UNTYPED(flattened_seq, flattened_seq_idx++, asdl_seq_GET_UNTYPED(inner_seq, j));
        }
    }
    assert(flattened_seq_idx == flattened_seq_size);

    return flattened_seq;
}

void *
_PyPegen_seq_last_item(asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    return asdl_seq_GET_UNTYPED(seq, len - 1);
}

void *
_PyPegen_seq_first_item(asdl_seq *seq)
{
    return asdl_seq_GET_UNTYPED(seq, 0);
}

/* Creates a new name of the form <first_name>.<second_name> */
expr_ty
_PyPegen_join_names_with_dot(Parser *p, expr_ty first_name, expr_ty second_name)
{
    assert(first_name != NULL && second_name != NULL);
    DynBuf b;
    dbuf_init(&b);
    if (qp_pegen_put_atom(p, &b, first_name->v.Name.id, 0) < 0) {
        dbuf_free(&b);
        return NULL;
    }
    dbuf_putc(&b, '.');
    if (qp_pegen_put_atom(p, &b, second_name->v.Name.id, 0) < 0) {
        dbuf_free(&b);
        return NULL;
    }
    if (dbuf_error(&b)) {
        dbuf_free(&b);
        return PyErr_NoMemory();
    }
    JSAtom uni = JS_NewAtomLen(p->ctx, (const char *)b.buf, b.size);
    dbuf_free(&b);
    if (uni == JS_ATOM_NULL) {
        return qp_pegen_js_error(p);
    }
    if (qp_arena_add_atom(p->arena, uni) < 0) {
        return NULL;
    }

    return _PyAST_Name(uni, Load, EXTRA_EXPR(first_name, second_name));
}

/* Counts the total number of dots in seq's tokens */
int
_PyPegen_seq_count_dots(asdl_seq *seq)
{
    int number_of_dots = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seq); i < l; i++) {
        Token *current_expr = asdl_seq_GET_UNTYPED(seq, i);
        switch (current_expr->type) {
            case ELLIPSIS:
                number_of_dots += 3;
                break;
            case DOT:
                number_of_dots += 1;
                break;
            default:
                Py_UNREACHABLE();
        }
    }

    return number_of_dots;
}

/* Creates an alias with '*' as the identifier name */
alias_ty
_PyPegen_alias_for_star(Parser *p, int lineno, int col_offset, int end_lineno,
                        int end_col_offset, PyArena *arena) {
    JSAtom str = JS_NewAtomLen(p->ctx, "*", 1);
    if (str == JS_ATOM_NULL) {
        return qp_pegen_js_error(p);
    }
    if (qp_arena_add_atom(p->arena, str) < 0) {
        return NULL;
    }
    return _PyAST_alias(str, JS_ATOM_NULL, lineno, col_offset, end_lineno, end_col_offset, arena);
}

/* Creates a new asdl_seq* with the identifiers of all the names in seq */
asdl_identifier_seq *
_PyPegen_map_names_to_ids(Parser *p, asdl_expr_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_identifier_seq *new_seq = _Py_asdl_identifier_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty e = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, e->v.Name.id);
    }
    return new_seq;
}

/* Constructs a CmpopExprPair */
CmpopExprPair *
_PyPegen_cmpop_expr_pair(Parser *p, cmpop_ty cmpop, expr_ty expr)
{
    assert(expr != NULL);
    CmpopExprPair *a = _PyArena_Malloc(p->arena, sizeof(CmpopExprPair));
    if (!a) {
        return NULL;
    }
    a->cmpop = cmpop;
    a->expr = expr;
    return a;
}

asdl_int_seq *
_PyPegen_get_cmpops(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_int_seq *new_seq = _Py_asdl_int_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        CmpopExprPair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->cmpop);
    }
    return new_seq;
}

asdl_expr_seq *
_PyPegen_get_exprs(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        CmpopExprPair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->expr);
    }
    return new_seq;
}

/* Creates an asdl_seq* where all the elements have been changed to have ctx as context */
static asdl_expr_seq *
_set_seq_context(Parser *p, asdl_expr_seq *seq, expr_context_ty ctx)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    if (len == 0) {
        return NULL;
    }

    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty e = asdl_seq_GET(seq, i);
        expr_ty new_e = _PyPegen_set_expr_context(p, e, ctx);
        if (!new_e) {
            return NULL;
        }
        asdl_seq_SET(new_seq, i, new_e);
    }
    return new_seq;
}

static expr_ty
_set_name_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _PyAST_Name(e->v.Name.id, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_tuple_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    asdl_expr_seq *seq = _set_seq_context(p, e->v.Tuple.elts, ctx);
    if (!seq && PyErr_Occurred()) {
        return NULL;
    }
    return _PyAST_Tuple(seq, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_list_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    asdl_expr_seq *seq = _set_seq_context(p, e->v.List.elts, ctx);
    if (!seq && PyErr_Occurred()) {
        return NULL;
    }
    return _PyAST_List(seq, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_subscript_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _PyAST_Subscript(e->v.Subscript.value, e->v.Subscript.slice,
                            ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_attribute_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _PyAST_Attribute(e->v.Attribute.value, e->v.Attribute.attr,
                            ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_starred_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    expr_ty inner = _PyPegen_set_expr_context(p, e->v.Starred.value, ctx);
    if (!inner) {
        return NULL;
    }
    return _PyAST_Starred(inner, ctx, EXTRA_EXPR(e, e));
}

/* Creates an `expr_ty` equivalent to `expr` but with `ctx` as context */
expr_ty
_PyPegen_set_expr_context(Parser *p, expr_ty expr, expr_context_ty ctx)
{
    assert(expr != NULL);

    expr_ty new = NULL;
    switch (expr->kind) {
        case Name_kind:
            new = _set_name_context(p, expr, ctx);
            break;
        case Tuple_kind:
            new = _set_tuple_context(p, expr, ctx);
            break;
        case List_kind:
            new = _set_list_context(p, expr, ctx);
            break;
        case Subscript_kind:
            new = _set_subscript_context(p, expr, ctx);
            break;
        case Attribute_kind:
            new = _set_attribute_context(p, expr, ctx);
            break;
        case Starred_kind:
            new = _set_starred_context(p, expr, ctx);
            break;
        default:
            new = expr;
    }
    return new;
}

/* Constructs a KeyValuePair that is used when parsing a dict's key value pairs */
KeyValuePair *
_PyPegen_key_value_pair(Parser *p, expr_ty key, expr_ty value)
{
    KeyValuePair *a = _PyArena_Malloc(p->arena, sizeof(KeyValuePair));
    if (!a) {
        return NULL;
    }
    a->key = key;
    a->value = value;
    return a;
}

/* Extracts all keys from an asdl_seq* of KeyValuePair*'s */
asdl_expr_seq *
_PyPegen_get_keys(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyValuePair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->key);
    }
    return new_seq;
}

/* Extracts all values from an asdl_seq* of KeyValuePair*'s */
asdl_expr_seq *
_PyPegen_get_values(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyValuePair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->value);
    }
    return new_seq;
}

/* Constructs a KeyPatternPair that is used when parsing mapping & class patterns */
KeyPatternPair *
_PyPegen_key_pattern_pair(Parser *p, expr_ty key, pattern_ty pattern)
{
    KeyPatternPair *a = _PyArena_Malloc(p->arena, sizeof(KeyPatternPair));
    if (!a) {
        return NULL;
    }
    a->key = key;
    a->pattern = pattern;
    return a;
}

/* Extracts all keys from an asdl_seq* of KeyPatternPair*'s */
asdl_expr_seq *
_PyPegen_get_pattern_keys(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyPatternPair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->key);
    }
    return new_seq;
}

/* Extracts all patterns from an asdl_seq* of KeyPatternPair*'s */
asdl_pattern_seq *
_PyPegen_get_patterns(Parser *p, asdl_seq *seq)
{
    Py_ssize_t len = asdl_seq_LEN(seq);
    asdl_pattern_seq *new_seq = _Py_asdl_pattern_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyPatternPair *pair = asdl_seq_GET_UNTYPED(seq, i);
        asdl_seq_SET(new_seq, i, pair->pattern);
    }
    return new_seq;
}

/* Constructs a NameDefaultPair */
NameDefaultPair *
_PyPegen_name_default_pair(Parser *p, arg_ty arg, expr_ty value, Token *tc)
{
    NameDefaultPair *a = _PyArena_Malloc(p->arena, sizeof(NameDefaultPair));
    if (!a) {
        return NULL;
    }
    a->arg = _PyPegen_add_type_comment_to_arg(p, arg, tc);
    if (!a->arg) {
        return NULL;
    }
    a->value = value;
    return a;
}

/* Constructs a SlashWithDefault */
SlashWithDefault *
_PyPegen_slash_with_default(Parser *p, asdl_arg_seq *plain_names, asdl_seq *names_with_defaults)
{
    SlashWithDefault *a = _PyArena_Malloc(p->arena, sizeof(SlashWithDefault));
    if (!a) {
        return NULL;
    }
    a->plain_names = plain_names;
    a->names_with_defaults = names_with_defaults;
    return a;
}

/* Constructs a StarEtc */
StarEtc *
_PyPegen_star_etc(Parser *p, arg_ty vararg, asdl_seq *kwonlyargs, arg_ty kwarg)
{
    StarEtc *a = _PyArena_Malloc(p->arena, sizeof(StarEtc));
    if (!a) {
        return NULL;
    }
    a->vararg = vararg;
    a->kwonlyargs = kwonlyargs;
    a->kwarg = kwarg;
    return a;
}

asdl_seq *
_PyPegen_join_sequences(Parser *p, asdl_seq *a, asdl_seq *b)
{
    Py_ssize_t first_len = asdl_seq_LEN(a);
    Py_ssize_t second_len = asdl_seq_LEN(b);
    asdl_seq *new_seq = (asdl_seq*)_Py_asdl_generic_seq_new(first_len + second_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int k = 0;
    for (Py_ssize_t i = 0; i < first_len; i++) {
        asdl_seq_SET_UNTYPED(new_seq, k++, asdl_seq_GET_UNTYPED(a, i));
    }
    for (Py_ssize_t i = 0; i < second_len; i++) {
        asdl_seq_SET_UNTYPED(new_seq, k++, asdl_seq_GET_UNTYPED(b, i));
    }

    return new_seq;
}

static asdl_arg_seq*
_get_names(Parser *p, asdl_seq *names_with_defaults)
{
    Py_ssize_t len = asdl_seq_LEN(names_with_defaults);
    asdl_arg_seq *seq = _Py_asdl_arg_seq_new(len, p->arena);
    if (!seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        NameDefaultPair *pair = asdl_seq_GET_UNTYPED(names_with_defaults, i);
        asdl_seq_SET(seq, i, pair->arg);
    }
    return seq;
}

static asdl_expr_seq *
_get_defaults(Parser *p, asdl_seq *names_with_defaults)
{
    Py_ssize_t len = asdl_seq_LEN(names_with_defaults);
    asdl_expr_seq *seq = _Py_asdl_expr_seq_new(len, p->arena);
    if (!seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        NameDefaultPair *pair = asdl_seq_GET_UNTYPED(names_with_defaults, i);
        asdl_seq_SET(seq, i, pair->value);
    }
    return seq;
}

static int
_make_posonlyargs(Parser *p,
                  asdl_arg_seq *slash_without_default,
                  SlashWithDefault *slash_with_default,
                  asdl_arg_seq **posonlyargs) {
    if (slash_without_default != NULL) {
        *posonlyargs = slash_without_default;
    }
    else if (slash_with_default != NULL) {
        asdl_arg_seq *slash_with_default_names =
                _get_names(p, slash_with_default->names_with_defaults);
        if (!slash_with_default_names) {
            return -1;
        }
        *posonlyargs = (asdl_arg_seq*)_PyPegen_join_sequences(
                p,
                (asdl_seq*)slash_with_default->plain_names,
                (asdl_seq*)slash_with_default_names);
    }
    else {
        *posonlyargs = _Py_asdl_arg_seq_new(0, p->arena);
    }
    return *posonlyargs == NULL ? -1 : 0;
}

static int
_make_posargs(Parser *p,
              asdl_arg_seq *plain_names,
              asdl_seq *names_with_default,
              asdl_arg_seq **posargs) {

    if (names_with_default != NULL) {
        if (plain_names != NULL) {
            asdl_arg_seq *names_with_default_names = _get_names(p, names_with_default);
            if (!names_with_default_names) {
                return -1;
            }
            *posargs = (asdl_arg_seq*)_PyPegen_join_sequences(
                    p,(asdl_seq*)plain_names, (asdl_seq*)names_with_default_names);
        }
        else {
            *posargs = _get_names(p, names_with_default);
        }
    }
    else {
        if (plain_names != NULL) {
            // With the current grammar, we never get here.
            // If that has changed, remove the assert, and test thoroughly.
            assert(0);
            *posargs = plain_names;
        }
        else {
            *posargs = _Py_asdl_arg_seq_new(0, p->arena);
        }
    }
    return *posargs == NULL ? -1 : 0;
}

static int
_make_posdefaults(Parser *p,
                  SlashWithDefault *slash_with_default,
                  asdl_seq *names_with_default,
                  asdl_expr_seq **posdefaults) {
    if (slash_with_default != NULL && names_with_default != NULL) {
        asdl_expr_seq *slash_with_default_values =
                _get_defaults(p, slash_with_default->names_with_defaults);
        if (!slash_with_default_values) {
            return -1;
        }
        asdl_expr_seq *names_with_default_values = _get_defaults(p, names_with_default);
        if (!names_with_default_values) {
            return -1;
        }
        *posdefaults = (asdl_expr_seq*)_PyPegen_join_sequences(
                p,
                (asdl_seq*)slash_with_default_values,
                (asdl_seq*)names_with_default_values);
    }
    else if (slash_with_default == NULL && names_with_default != NULL) {
        *posdefaults = _get_defaults(p, names_with_default);
    }
    else if (slash_with_default != NULL && names_with_default == NULL) {
        *posdefaults = _get_defaults(p, slash_with_default->names_with_defaults);
    }
    else {
        *posdefaults = _Py_asdl_expr_seq_new(0, p->arena);
    }
    return *posdefaults == NULL ? -1 : 0;
}

static int
_make_kwargs(Parser *p, StarEtc *star_etc,
             asdl_arg_seq **kwonlyargs,
             asdl_expr_seq **kwdefaults) {
    if (star_etc != NULL && star_etc->kwonlyargs != NULL) {
        *kwonlyargs = _get_names(p, star_etc->kwonlyargs);
    }
    else {
        *kwonlyargs = _Py_asdl_arg_seq_new(0, p->arena);
    }

    if (*kwonlyargs == NULL) {
        return -1;
    }

    if (star_etc != NULL && star_etc->kwonlyargs != NULL) {
        *kwdefaults = _get_defaults(p, star_etc->kwonlyargs);
    }
    else {
        *kwdefaults = _Py_asdl_expr_seq_new(0, p->arena);
    }

    if (*kwdefaults == NULL) {
        return -1;
    }

    return 0;
}

/* Constructs an arguments_ty object out of all the parsed constructs in the parameters rule */
arguments_ty
_PyPegen_make_arguments(Parser *p, asdl_arg_seq *slash_without_default,
                        SlashWithDefault *slash_with_default, asdl_arg_seq *plain_names,
                        asdl_seq *names_with_default, StarEtc *star_etc)
{
    asdl_arg_seq *posonlyargs;
    if (_make_posonlyargs(p, slash_without_default, slash_with_default, &posonlyargs) == -1) {
        return NULL;
    }

    asdl_arg_seq *posargs;
    if (_make_posargs(p, plain_names, names_with_default, &posargs) == -1) {
        return NULL;
    }

    asdl_expr_seq *posdefaults;
    if (_make_posdefaults(p,slash_with_default, names_with_default, &posdefaults) == -1) {
        return NULL;
    }

    arg_ty vararg = NULL;
    if (star_etc != NULL && star_etc->vararg != NULL) {
        vararg = star_etc->vararg;
    }

    asdl_arg_seq *kwonlyargs;
    asdl_expr_seq *kwdefaults;
    if (_make_kwargs(p, star_etc, &kwonlyargs, &kwdefaults) == -1) {
        return NULL;
    }

    arg_ty kwarg = NULL;
    if (star_etc != NULL && star_etc->kwarg != NULL) {
        kwarg = star_etc->kwarg;
    }

    return _PyAST_arguments(posonlyargs, posargs, vararg, kwonlyargs,
                            kwdefaults, kwarg, posdefaults, p->arena);
}


/* Constructs an empty arguments_ty object, that gets used when a function accepts no
 * arguments. */
arguments_ty
_PyPegen_empty_arguments(Parser *p)
{
    asdl_arg_seq *posonlyargs = _Py_asdl_arg_seq_new(0, p->arena);
    if (!posonlyargs) {
        return NULL;
    }
    asdl_arg_seq *posargs = _Py_asdl_arg_seq_new(0, p->arena);
    if (!posargs) {
        return NULL;
    }
    asdl_expr_seq *posdefaults = _Py_asdl_expr_seq_new(0, p->arena);
    if (!posdefaults) {
        return NULL;
    }
    asdl_arg_seq *kwonlyargs = _Py_asdl_arg_seq_new(0, p->arena);
    if (!kwonlyargs) {
        return NULL;
    }
    asdl_expr_seq *kwdefaults = _Py_asdl_expr_seq_new(0, p->arena);
    if (!kwdefaults) {
        return NULL;
    }

    return _PyAST_arguments(posonlyargs, posargs, NULL, kwonlyargs,
                            kwdefaults, NULL, posdefaults, p->arena);
}

/* Encapsulates the value of an operator_ty into an AugOperator struct */
AugOperator *
_PyPegen_augoperator(Parser *p, operator_ty kind)
{
    AugOperator *a = _PyArena_Malloc(p->arena, sizeof(AugOperator));
    if (!a) {
        return NULL;
    }
    a->kind = kind;
    return a;
}

/* Construct a FunctionDef equivalent to function_def, but with decorators */
stmt_ty
_PyPegen_function_def_decorators(Parser *p, asdl_expr_seq *decorators, stmt_ty function_def)
{
    assert(function_def != NULL);
    if (function_def->kind == AsyncFunctionDef_kind) {
        return _PyAST_AsyncFunctionDef(
            function_def->v.AsyncFunctionDef.name,
            function_def->v.AsyncFunctionDef.args,
            function_def->v.AsyncFunctionDef.body, decorators,
            function_def->v.AsyncFunctionDef.returns,
            function_def->v.AsyncFunctionDef.type_comment,
            function_def->v.AsyncFunctionDef.type_params,
            function_def->lineno, function_def->col_offset,
            function_def->end_lineno, function_def->end_col_offset, p->arena);
    }

    return _PyAST_FunctionDef(
        function_def->v.FunctionDef.name,
        function_def->v.FunctionDef.args,
        function_def->v.FunctionDef.body, decorators,
        function_def->v.FunctionDef.returns,
        function_def->v.FunctionDef.type_comment,
        function_def->v.FunctionDef.type_params,
        function_def->lineno, function_def->col_offset,
        function_def->end_lineno, function_def->end_col_offset, p->arena);
}

/* Construct a ClassDef equivalent to class_def, but with decorators */
stmt_ty
_PyPegen_class_def_decorators(Parser *p, asdl_expr_seq *decorators, stmt_ty class_def)
{
    assert(class_def != NULL);
    return _PyAST_ClassDef(
        class_def->v.ClassDef.name,
        class_def->v.ClassDef.bases, class_def->v.ClassDef.keywords,
        class_def->v.ClassDef.body, decorators,
        class_def->v.ClassDef.type_params,
        class_def->lineno, class_def->col_offset, class_def->end_lineno,
        class_def->end_col_offset, p->arena);
}

/* Construct a KeywordOrStarred */
KeywordOrStarred *
_PyPegen_keyword_or_starred(Parser *p, void *element, int is_keyword)
{
    KeywordOrStarred *a = _PyArena_Malloc(p->arena, sizeof(KeywordOrStarred));
    if (!a) {
        return NULL;
    }
    a->element = element;
    a->is_keyword = is_keyword;
    return a;
}

/* Get the number of starred expressions in an asdl_seq* of KeywordOrStarred*s */
static int
_seq_number_of_starred_exprs(asdl_seq *seq)
{
    int n = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seq); i < l; i++) {
        KeywordOrStarred *k = asdl_seq_GET_UNTYPED(seq, i);
        if (!k->is_keyword) {
            n++;
        }
    }
    return n;
}

/* Extract the starred expressions of an asdl_seq* of KeywordOrStarred*s */
asdl_expr_seq *
_PyPegen_seq_extract_starred_exprs(Parser *p, asdl_seq *kwargs)
{
    int new_len = _seq_number_of_starred_exprs(kwargs);
    if (new_len == 0) {
        return NULL;
    }
    asdl_expr_seq *new_seq = _Py_asdl_expr_seq_new(new_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int idx = 0;
    for (Py_ssize_t i = 0, len = asdl_seq_LEN(kwargs); i < len; i++) {
        KeywordOrStarred *k = asdl_seq_GET_UNTYPED(kwargs, i);
        if (!k->is_keyword) {
            asdl_seq_SET(new_seq, idx++, k->element);
        }
    }
    return new_seq;
}

/* Return a new asdl_seq* with only the keywords in kwargs */
asdl_keyword_seq*
_PyPegen_seq_delete_starred_exprs(Parser *p, asdl_seq *kwargs)
{
    Py_ssize_t len = asdl_seq_LEN(kwargs);
    Py_ssize_t new_len = len - _seq_number_of_starred_exprs(kwargs);
    if (new_len == 0) {
        return NULL;
    }
    asdl_keyword_seq *new_seq = _Py_asdl_keyword_seq_new(new_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int idx = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        KeywordOrStarred *k = asdl_seq_GET_UNTYPED(kwargs, i);
        if (k->is_keyword) {
            asdl_seq_SET(new_seq, idx++, k->element);
        }
    }
    return new_seq;
}

expr_ty
_PyPegen_ensure_imaginary(Parser *p, expr_ty exp)
{
    if (exp->kind != Constant_kind || exp->v.Constant.value->kind != QP_CONST_COMPLEX) {
        RAISE_SYNTAX_ERROR_KNOWN_LOCATION(exp, "imaginary number required in complex literal");
        return NULL;
    }
    return exp;
}

expr_ty
_PyPegen_ensure_real(Parser *p, expr_ty exp)
{
    if (exp->kind != Constant_kind || exp->v.Constant.value->kind == QP_CONST_COMPLEX) {
        RAISE_SYNTAX_ERROR_KNOWN_LOCATION(exp, "real number required in complex literal");
        return NULL;
    }
    return exp;
}

mod_ty
_PyPegen_make_module(Parser *p, asdl_stmt_seq *a) {
    asdl_type_ignore_seq *type_ignores = NULL;
    Py_ssize_t num = p->type_ignore_comments.num_items;
    if (num > 0) {
        // Turn the raw (comment, lineno) pairs into TypeIgnore objects in the arena
        type_ignores = _Py_asdl_type_ignore_seq_new(num, p->arena);
        if (type_ignores == NULL) {
            return NULL;
        }
        for (Py_ssize_t i = 0; i < num; i++) {
            JSAtom tag = _PyPegen_new_type_comment(p, p->type_ignore_comments.items[i].comment);
            if (tag == JS_ATOM_NULL) {
                return NULL;
            }
            type_ignore_ty ti = _PyAST_TypeIgnore(p->type_ignore_comments.items[i].lineno,
                                                  tag, p->arena);
            if (ti == NULL) {
                return NULL;
            }
            asdl_seq_SET(type_ignores, i, ti);
        }
    }
    return _PyAST_Module(a, type_ignores, p->arena);
}

/* A type comment / type: ignore tag as an arena-owned string (JSAtom). */
JSAtom
_PyPegen_new_type_comment(Parser *p, const char *s)
{
    size_t len = strlen(s);
    if (qp_utf8_check(p, s, (Py_ssize_t)len) < 0) {
        return JS_ATOM_NULL;
    }
    JSAtom res = JS_NewAtomLen(p->ctx, len ? s : "", len);
    if (res == JS_ATOM_NULL) {
        qp_pegen_js_error(p);
        return JS_ATOM_NULL;
    }
    if (qp_arena_add_atom(p->arena, res) < 0) {
        return JS_ATOM_NULL;
    }
    return res;
}

arg_ty
_PyPegen_add_type_comment_to_arg(Parser *p, arg_ty a, Token *tc)
{
    if (tc == NULL) {
        return a;
    }
    const char *bytes = tc->bytes;
    if (bytes == NULL) {
        return NULL;
    }
    JSAtom tco = _PyPegen_new_type_comment(p, bytes);
    if (tco == JS_ATOM_NULL) {
        return NULL;
    }
    return _PyAST_arg(a->arg, a->annotation, tco,
                      a->lineno, a->col_offset, a->end_lineno, a->end_col_offset,
                      p->arena);
}

/* Checks if the NOTEQUAL token is valid given the current parser flags
0 indicates success and nonzero indicates failure (an exception may be set) */
int
_PyPegen_check_barry_as_flufl(Parser *p, Token* t) {
    assert(t->bytes != NULL);
    assert(t->type == NOTEQUAL);

    const char* tok_str = PyBytes_AS_STRING(t->bytes);
    if (p->flags & PyPARSE_BARRY_AS_BDFL && strcmp(tok_str, "<>") != 0) {
        RAISE_SYNTAX_ERROR("with Barry as BDFL, use '<>' instead of '!='");
        return -1;
    }
    if (!(p->flags & PyPARSE_BARRY_AS_BDFL)) {
        return strcmp(tok_str, "!=");
    }
    return 0;
}

int
_PyPegen_check_legacy_stmt(Parser *p, expr_ty name) {
    if (name->kind != Name_kind) {
        return 0;
    }
    const char* candidates[2] = {"print", "exec"};
    for (int i=0; i<2; i++) {
        if (qp_pegen_atom_eq(p, name->v.Name.id, candidates[i])) {
            return 1;
        }
    }
    return 0;
}

void *
_PyPegen_raise_error_for_missing_comma(Parser *p, expr_ty a, expr_ty b)
{
    // Don't raise for legacy statements like "print x" or "exec x"
    if (_PyPegen_check_legacy_stmt(p, a)) {
        return NULL;
    }
    // Only raise inside parentheses/brackets (level > 0)
    if (p->tokens[p->mark - 1]->level == 0) {
        return NULL;
    }
    // For multi-line expressions (like string concatenations), point to the
    // last line instead of the first for a more helpful error message.
    // Use a->col_offset as the starting column since all strings in the
    // concatenation typically share the same indentation.
    if (a->end_lineno > a->lineno) {
        return RAISE_ERROR_KNOWN_LOCATION(
            p, PyExc_SyntaxError, a->end_lineno, a->col_offset,
            a->end_lineno, a->end_col_offset,
            "invalid syntax. Perhaps you forgot a comma?"
        );
    }
    return RAISE_ERROR_KNOWN_LOCATION(
        p, PyExc_SyntaxError, a->lineno, a->col_offset,
        b->end_lineno, b->end_col_offset,
        "invalid syntax. Perhaps you forgot a comma?"
    );
}

static ResultTokenWithMetadata *
result_token_with_metadata(Parser *p, void *result, qp_const *metadata)
{
    ResultTokenWithMetadata *res = _PyArena_Malloc(p->arena, sizeof(ResultTokenWithMetadata));
    if (res == NULL) {
        return NULL;
    }
    res->metadata = metadata;
    res->result = result;
    return res;
}

ResultTokenWithMetadata *
_PyPegen_check_fstring_conversion(Parser *p, Token* conv_token, expr_ty conv)
{
    if (conv_token->lineno != conv->lineno || conv_token->end_col_offset != conv->col_offset) {
        return RAISE_SYNTAX_ERROR_KNOWN_RANGE(
            conv_token, conv,
            "%c-string: conversion type must come right after the exclamation mark",
            TOK_GET_STRING_PREFIX(p->tok)
        );
    }

    /* first = PyUnicode_READ_CHAR(id, 0); PyUnicode_GET_LENGTH(id) > 1 */
    char buf[8];
    const char *id = JS_AtomGetStr(p->ctx, buf, sizeof(buf), conv->v.Name.id);
    char first = id[0];
    if (strlen(id) > 1 ||
            !(first == 's' || first == 'r' || first == 'a')) {
        RAISE_SYNTAX_ERROR_KNOWN_LOCATION(conv,
                                            "%c-string: invalid conversion character %R: expected 's', 'r', or 'a'",
                                            TOK_GET_STRING_PREFIX(p->tok),
                                            conv->v.Name.id);
        return NULL;
    }

    return result_token_with_metadata(p, conv, conv_token->metadata);
}

/* An empty str constant (PyUnicode_CheckExact(v) && PyUnicode_GET_LENGTH(v) == 0). */
static inline int
qp_pegen_is_empty_str_constant(expr_ty item)
{
    return item->kind == Constant_kind &&
           item->v.Constant.value->kind == QP_CONST_STR &&
           qp_pegen_str_is_empty(item->v.Constant.value);
}

ResultTokenWithMetadata *
_PyPegen_setup_full_format_spec(Parser *p, Token *colon, asdl_expr_seq *spec, int lineno, int col_offset,
                                int end_lineno, int end_col_offset, PyArena *arena)
{
    if (!spec) {
        return NULL;
    }

    // This is needed to keep compatibility with 3.11, where an empty format
    // spec is parsed as an *empty* JoinedStr node, instead of having an empty
    // constant in it.
    Py_ssize_t n_items = asdl_seq_LEN(spec);
    Py_ssize_t non_empty_count = 0;
    for (Py_ssize_t i = 0; i < n_items; i++) {
        expr_ty item = asdl_seq_GET(spec, i);
        non_empty_count += !qp_pegen_is_empty_str_constant(item);
    }
    if (non_empty_count != n_items) {
        asdl_expr_seq *resized_spec =
            _Py_asdl_expr_seq_new(non_empty_count, p->arena);
        if (resized_spec == NULL) {
            return NULL;
        }
        Py_ssize_t j = 0;
        for (Py_ssize_t i = 0; i < n_items; i++) {
            expr_ty item = asdl_seq_GET(spec, i);
            if (qp_pegen_is_empty_str_constant(item)) {
                continue;
            }
            asdl_seq_SET(resized_spec, j++, item);
        }
        assert(j == non_empty_count);
        spec = resized_spec;
    }
    expr_ty res;
    Py_ssize_t n = asdl_seq_LEN(spec);
    if (n == 0 || (n == 1 && asdl_seq_GET(spec, 0)->kind == Constant_kind)) {
        res = _PyAST_JoinedStr(spec, lineno, col_offset, end_lineno,
                                    end_col_offset, p->arena);
    } else {
        res = _PyPegen_concatenate_strings(p, spec,
                             lineno, col_offset, end_lineno,
                             end_col_offset, arena);
    }
    if (!res) {
        return NULL;
    }
    return result_token_with_metadata(p, res, colon->metadata);
}

const char *
_PyPegen_get_expr_name(expr_ty e)
{
    assert(e != NULL);
    switch (e->kind) {
        case Attribute_kind:
            return "attribute";
        case Subscript_kind:
            return "subscript";
        case Starred_kind:
            return "starred";
        case Name_kind:
            return "name";
        case List_kind:
            return "list";
        case Tuple_kind:
            return "tuple";
        case Lambda_kind:
            return "lambda";
        case Call_kind:
            return "function call";
        case BoolOp_kind:
        case BinOp_kind:
        case UnaryOp_kind:
            return "expression";
        case GeneratorExp_kind:
            return "generator expression";
        case Yield_kind:
        case YieldFrom_kind:
            return "yield expression";
        case Await_kind:
            return "await expression";
        case ListComp_kind:
            return "list comprehension";
        case SetComp_kind:
            return "set comprehension";
        case DictComp_kind:
            return "dict comprehension";
        case Dict_kind:
            return "dict literal";
        case Set_kind:
            return "set display";
        case JoinedStr_kind:
        case FormattedValue_kind:
            return "f-string expression";
        case TemplateStr_kind:
        case Interpolation_kind:
            return "t-string expression";
        case Constant_kind: {
            qp_const *value = e->v.Constant.value;
            if (value == Py_None) {
                return "None";
            }
            if (value == Py_False) {
                return "False";
            }
            if (value == Py_True) {
                return "True";
            }
            if (value == Py_Ellipsis) {
                return "ellipsis";
            }
            return "literal";
        }
        case Compare_kind:
            return "comparison";
        case IfExp_kind:
            return "conditional expression";
        case NamedExpr_kind:
            return "named expression";
        default:
            /* CPython raises SystemError("unexpected expression in
               assignment %d (line %d)"); no parser rule reaches this. */
            return NULL;
    }
}

expr_ty
_PyPegen_get_last_comprehension_item(comprehension_ty comprehension) {
    if (comprehension->ifs == NULL || asdl_seq_LEN(comprehension->ifs) == 0) {
        return comprehension->iter;
    }
    return PyPegen_last_item(comprehension->ifs, expr_ty);
}

expr_ty _PyPegen_collect_call_seqs(Parser *p, asdl_expr_seq *a, asdl_seq *b,
                     int lineno, int col_offset, int end_lineno,
                     int end_col_offset, PyArena *arena) {
    Py_ssize_t args_len = asdl_seq_LEN(a);
    Py_ssize_t total_len = args_len;

    if (b == NULL) {
        return _PyAST_Call(_PyPegen_dummy_name(p), a, NULL, lineno, col_offset,
                        end_lineno, end_col_offset, arena);

    }

    asdl_expr_seq *starreds = _PyPegen_seq_extract_starred_exprs(p, b);
    if (!starreds && PyErr_Occurred()) {
        return NULL;
    }

    asdl_keyword_seq *keywords = _PyPegen_seq_delete_starred_exprs(p, b);
    if (!keywords && PyErr_Occurred()) {
        return NULL;
    }

    if (starreds) {
        total_len += asdl_seq_LEN(starreds);
    }

    asdl_expr_seq *args = _Py_asdl_expr_seq_new(total_len, arena);
    if (args == NULL) {
        return NULL;
    }

    Py_ssize_t i = 0;
    for (i = 0; i < args_len; i++) {
        asdl_seq_SET(args, i, asdl_seq_GET(a, i));
    }
    for (; i < total_len; i++) {
        asdl_seq_SET(args, i, asdl_seq_GET(starreds, i - args_len));
    }

    return _PyAST_Call(_PyPegen_dummy_name(p), args, keywords, lineno,
                       col_offset, end_lineno, end_col_offset, arena);
}

// AST Error reporting helpers

expr_ty
_PyPegen_get_invalid_target(expr_ty e, TARGETS_TYPE targets_type)
{
    if (e == NULL) {
        return NULL;
    }

#define VISIT_CONTAINER(CONTAINER, TYPE) do { \
        Py_ssize_t len = asdl_seq_LEN((CONTAINER)->v.TYPE.elts);\
        for (Py_ssize_t i = 0; i < len; i++) {\
            expr_ty other = asdl_seq_GET((CONTAINER)->v.TYPE.elts, i);\
            expr_ty child = _PyPegen_get_invalid_target(other, targets_type);\
            if (child != NULL) {\
                return child;\
            }\
        }\
    } while (0)

    // We only need to visit List and Tuple nodes recursively as those
    // are the only ones that can contain valid names in targets when
    // they are parsed as expressions. Any other kind of expression
    // that is a container (like Sets or Dicts) is directly invalid and
    // we don't need to visit it recursively.

    switch (e->kind) {
        case List_kind:
            VISIT_CONTAINER(e, List);
            return NULL;
        case Tuple_kind:
            VISIT_CONTAINER(e, Tuple);
            return NULL;
        case Starred_kind:
            if (targets_type == DEL_TARGETS) {
                return e;
            }
            return _PyPegen_get_invalid_target(e->v.Starred.value, targets_type);
        case Compare_kind:
            // This is needed, because the `a in b` in `for a in b` gets parsed
            // as a comparison, and so we need to search the left side of the comparison
            // for invalid targets.
            if (targets_type == FOR_TARGETS) {
                cmpop_ty cmpop = (cmpop_ty) asdl_seq_GET(e->v.Compare.ops, 0);
                if (cmpop == In) {
                    return _PyPegen_get_invalid_target(e->v.Compare.left, targets_type);
                }
                return NULL;
            }
            return e;
        case Name_kind:
        case Subscript_kind:
        case Attribute_kind:
            return NULL;
        default:
            return e;
    }
}

void *_PyPegen_arguments_parsing_error(Parser *p, expr_ty e) {
    int kwarg_unpacking = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(e->v.Call.keywords); i < l; i++) {
        keyword_ty keyword = asdl_seq_GET(e->v.Call.keywords, i);
        if (!keyword->arg) {
            kwarg_unpacking = 1;
        }
    }

    const char *msg = NULL;
    if (kwarg_unpacking) {
        msg = "positional argument follows keyword argument unpacking";
    } else {
        msg = "positional argument follows keyword argument";
    }

    return RAISE_SYNTAX_ERROR(msg);
}

void *
_PyPegen_nonparen_genexp_in_call(Parser *p, expr_ty args, asdl_comprehension_seq *comprehensions)
{
    /* The rule that calls this function is 'args for_if_clauses'.
       For the input f(L, x for x in y), L and x are in args and
       the for is parsed as a for_if_clause. We have to check if
       len <= 1, so that input like dict((a, b) for a, b in x)
       gets successfully parsed and then we pass the last
       argument (x in the above example) as the location of the
       error */
    Py_ssize_t len = asdl_seq_LEN(args->v.Call.args);
    if (len <= 1) {
        return NULL;
    }

    comprehension_ty last_comprehension = PyPegen_last_item(comprehensions, comprehension_ty);

    return RAISE_SYNTAX_ERROR_KNOWN_RANGE(
        (expr_ty) asdl_seq_GET(args->v.Call.args, len - 1),
        _PyPegen_get_last_comprehension_item(last_comprehension),
        "Generator expression must be parenthesized"
    );
}

// Fstring stuff

static expr_ty
_PyPegen_decode_fstring_part(Parser* p, int is_raw, expr_ty constant, Token* token) {
    assert(constant->v.Constant.value->kind == QP_CONST_STR);

    size_t bsize;
    const char* bstr = JS_ToCStringLen2(p->ctx, &bsize, constant->v.Constant.value->v, FALSE);
    if (bstr == NULL) {
        return qp_pegen_js_error(p);
    }

    size_t len;
    if (strcmp(bstr, "{{") == 0 || strcmp(bstr, "}}") == 0) {
        len = 1;
    } else {
        len = strlen(bstr);
    }

    is_raw = is_raw || strchr(bstr, '\\') == NULL;
    qp_const *str = _PyPegen_decode_string(p, is_raw, bstr, len, token);
    JS_FreeCString(p->ctx, bstr);
    if (str == NULL) {
        _Pypegen_raise_decode_error_known_location(p, constant);
        return NULL;
    }
    return _PyAST_Constant(str, JS_ATOM_NULL, constant->lineno, constant->col_offset,
                           constant->end_lineno, constant->end_col_offset,
                           p->arena);
}

static asdl_expr_seq *
_get_resized_exprs(Parser *p, Token *a, asdl_expr_seq *raw_expressions, Token *b, enum string_kind_t string_kind)
{
    Py_ssize_t n_items = asdl_seq_LEN(raw_expressions);
    Py_ssize_t total_items = n_items;
    for (Py_ssize_t i = 0; i < n_items; i++) {
        expr_ty item = asdl_seq_GET(raw_expressions, i);
        if (item->kind == JoinedStr_kind) {
            total_items += asdl_seq_LEN(item->v.JoinedStr.values) - 1;
        }
    }

    const char* quote_str = a->bytes;
    if (quote_str == NULL) {
        return NULL;
    }
    int is_raw = strpbrk(quote_str, "rR") != NULL;

    asdl_expr_seq *seq = _Py_asdl_expr_seq_new(total_items, p->arena);
    if (seq == NULL) {
        return NULL;
    }

    Py_ssize_t index = 0;
    for (Py_ssize_t i = 0; i < n_items; i++) {
        expr_ty item = asdl_seq_GET(raw_expressions, i);

        // This should correspond to a JoinedStr node of two elements
        // created _PyPegen_formatted_value. This situation can only be the result of
        // a (f|t)-string debug expression where the first element is a constant with the text and the second
        // a formatted value with the expression.
        if (item->kind == JoinedStr_kind) {
            asdl_expr_seq *values = item->v.JoinedStr.values;
            if (asdl_seq_LEN(values) != 2) {
                /* PyExc_SystemError: unreachable for grammar-built nodes */
                qp_error_clear(p->err);
                qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0,
                             string_kind == TSTRING
                             ? "unexpected TemplateStr node without debug data in t-string at line %d"
                             : "unexpected JoinedStr node without debug data in f-string at line %d",
                             item->lineno);
                return NULL;
            }

            expr_ty first = asdl_seq_GET(values, 0);
            assert(first->kind == Constant_kind);
            asdl_seq_SET(seq, index++, first);

            expr_ty second = asdl_seq_GET(values, 1);
            assert((string_kind == TSTRING && second->kind == Interpolation_kind) || second->kind == FormattedValue_kind);
            asdl_seq_SET(seq, index++, second);

            continue;
        }

        if (item->kind == Constant_kind) {
            item = _PyPegen_decode_fstring_part(p, is_raw, item, b);
            if (item == NULL) {
                return NULL;
            }

            /* Tokenizer emits string parts even when the underlying string
            might become an empty value (e.g. FSTRING_MIDDLE with the value \\n)
            so we need to check for them and simplify it here. */
            if (qp_pegen_is_empty_str_constant(item)) {
                continue;
            }
        }
        asdl_seq_SET(seq, index++, item);
    }

    asdl_expr_seq *resized_exprs;
    if (index != total_items) {
        resized_exprs = _Py_asdl_expr_seq_new(index, p->arena);
        if (resized_exprs == NULL) {
            return NULL;
        }
        for (Py_ssize_t i = 0; i < index; i++) {
            asdl_seq_SET(resized_exprs, i, asdl_seq_GET(seq, i));
        }
    }
    else {
        resized_exprs = seq;
    }
    return resized_exprs;
}

expr_ty
_PyPegen_template_str(Parser *p, Token *a, asdl_expr_seq *raw_expressions, Token *b) {

    asdl_expr_seq *resized_exprs = _get_resized_exprs(p, a, raw_expressions, b, TSTRING);
    if (resized_exprs == NULL) {
        return NULL;
    }
    return _PyAST_TemplateStr(resized_exprs, a->lineno, a->col_offset,
                              b->end_lineno, b->end_col_offset,
                              p->arena);
}

expr_ty
_PyPegen_joined_str(Parser *p, Token* a, asdl_expr_seq* raw_expressions, Token*b) {

    asdl_expr_seq *resized_exprs = _get_resized_exprs(p, a, raw_expressions, b, FSTRING);
    if (resized_exprs == NULL) {
        return NULL;
    }
    return _PyAST_JoinedStr(resized_exprs, a->lineno, a->col_offset,
                            b->end_lineno, b->end_col_offset,
                            p->arena);
}

expr_ty _PyPegen_decoded_constant_from_token(Parser* p, Token* tok) {
    Py_ssize_t bsize = tok->bytes_len;
    char* bstr = tok->bytes;

    // Check if we're inside a raw f-string for format spec decoding
    int is_raw = 0;
    if (INSIDE_FSTRING(p->tok)) {
        tokenizer_mode *mode = TOK_GET_MODE(p->tok);
        is_raw = mode->raw;
    }

    qp_const* str = _PyPegen_decode_string(p, is_raw, bstr, bsize, tok);
    if (str == NULL) {
        return NULL;
    }
    return _PyAST_Constant(str, JS_ATOM_NULL, tok->lineno, tok->col_offset,
                           tok->end_lineno, tok->end_col_offset,
                           p->arena);
}

expr_ty _PyPegen_constant_from_token(Parser* p, Token* tok) {
    char* bstr = tok->bytes;
    if (bstr == NULL) {
        return NULL;
    }
    /* PyUnicode_FromString(bstr) */
    size_t len = strlen(bstr);
    if (qp_utf8_check(p, bstr, (Py_ssize_t)len) < 0) {
        return NULL;
    }
    qp_const *str = qp_pegen_new_const(p, QP_CONST_STR, JS_NewStringLen(p->ctx, bstr, len));
    if (str == NULL) {
        return NULL;
    }
    return _PyAST_Constant(str, JS_ATOM_NULL, tok->lineno, tok->col_offset,
                           tok->end_lineno, tok->end_col_offset,
                           p->arena);
}

expr_ty _PyPegen_constant_from_string(Parser* p, Token* tok) {
    char* the_str = tok->bytes;
    if (the_str == NULL) {
        return NULL;
    }
    qp_const *s = _PyPegen_parse_string(p, tok);
    if (s == NULL) {
        _Pypegen_raise_decode_error(p);
        return NULL;
    }
    JSAtom kind = JS_ATOM_NULL;
    if (the_str && the_str[0] == 'u') {
        kind = _PyPegen_new_identifier(p, "u");
        if (kind == JS_ATOM_NULL) {
            return NULL;
        }
    }
    return _PyAST_Constant(s, kind, tok->lineno, tok->col_offset, tok->end_lineno, tok->end_col_offset, p->arena);
}

static int
_get_interpolation_conversion(Parser *p, Token *debug, ResultTokenWithMetadata *conversion,
                              ResultTokenWithMetadata *format)
{
    if (conversion != NULL) {
        expr_ty conversion_expr = (expr_ty) conversion->result;
        assert(conversion_expr->kind == Name_kind);
        char buf[8];
        const char *id = JS_AtomGetStr(p->ctx, buf, sizeof(buf), conversion_expr->v.Name.id);
        return (unsigned char)id[0];
    }
    else if (debug && !format) {
        /* If no conversion is specified, use !r for debug expressions */
        return (int)'r';
    }
    return -1;
}

/* Python str.isspace() for one character (_PyUnicode_IsWhitespace). */
static int
qp_pegen_is_whitespace(uint32_t c)
{
    switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
    case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return 1;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

static qp_const *
_strip_interpolation_debug_expr(Parser *p, qp_const *exprstr)
{
    JSString *s = qp_pegen_str(exprstr);
    int len = s->len;

    /* Discard whitespace and explicit line continuations after the debug "="
       but preserve whitespace before it. */
    while (len > 0) {
        int has_newline = 0;
        while (len > 0) {
            uint32_t c = string_get(s, len - 1);
            if (!qp_pegen_is_whitespace(c)) {
                break;
            }
            if (c == '\r' || c == '\n') {
                has_newline = 1;
            }
            len--;
        }
        if (!has_newline || len == 0 ||
            string_get(s, len - 1) != '\\')
        {
            break;
        }
        len--;
    }

    /* Preserve unexpected metadata instead of dropping source text. */
    if (len == 0 || string_get(s, len - 1) != '=') {
        return exprstr;
    }

    return qp_pegen_new_const(p, QP_CONST_STR, js_sub_string(p->ctx, s, 0, len - 1));
}

expr_ty _PyPegen_interpolation(Parser *p, expr_ty expression, Token *debug, ResultTokenWithMetadata *conversion,
                                 ResultTokenWithMetadata *format, Token *closing_brace, int lineno, int col_offset,
                                 int end_lineno, int end_col_offset, PyArena *arena) {

    int conversion_val = _get_interpolation_conversion(p, debug, conversion, format);

    /* Find the non whitespace token after the "=" */
    int debug_end_line, debug_end_offset;
    qp_const *debug_metadata;
    constant exprstr;

    if (conversion) {
        debug_end_line = ((expr_ty) conversion->result)->lineno;
        debug_end_offset = ((expr_ty) conversion->result)->col_offset;
        debug_metadata = exprstr = conversion->metadata;
    }
    else if (format) {
        debug_end_line = ((expr_ty) format->result)->lineno;
        debug_end_offset = ((expr_ty) format->result)->col_offset + 1;
        debug_metadata = exprstr = format->metadata;
    }
    else {
        debug_end_line = end_lineno;
        debug_end_offset = end_col_offset;
        debug_metadata = exprstr = closing_brace->metadata;
    }

    assert(exprstr != NULL);
    qp_const *final_exprstr = debug
        ? _strip_interpolation_debug_expr(p, exprstr)
        : exprstr;
    if (!final_exprstr) {
        return NULL;
    }

    expr_ty interpolation = _PyAST_Interpolation(
        expression, final_exprstr, conversion_val, format ? (expr_ty) format->result : NULL,
        lineno, col_offset, end_lineno,
        end_col_offset, arena
    );

    if (!interpolation || !debug) {
        return interpolation;
    }

    expr_ty debug_text = _PyAST_Constant(debug_metadata, JS_ATOM_NULL, lineno, col_offset + 1, debug_end_line,
                                            debug_end_offset - 1, p->arena);
    if (!debug_text) {
        return NULL;
    }

    asdl_expr_seq *values = _Py_asdl_expr_seq_new(2, arena);
    if (!values) {
        return NULL;
    }
    asdl_seq_SET(values, 0, debug_text);
    asdl_seq_SET(values, 1, interpolation);
    return _PyAST_JoinedStr(values, lineno, col_offset, debug_end_line, debug_end_offset, p->arena);
}

expr_ty _PyPegen_formatted_value(Parser *p, expr_ty expression, Token *debug, ResultTokenWithMetadata *conversion,
                                 ResultTokenWithMetadata *format, Token *closing_brace, int lineno, int col_offset,
                                 int end_lineno, int end_col_offset, PyArena *arena) {
    int conversion_val = _get_interpolation_conversion(p, debug, conversion, format);

    expr_ty formatted_value = _PyAST_FormattedValue(
        expression, conversion_val, format ? (expr_ty) format->result : NULL,
        lineno, col_offset, end_lineno,
        end_col_offset, arena
    );

    if (!formatted_value || !debug) {
        return formatted_value;
    }

    /* Find the non whitespace token after the "=" */
    int debug_end_line, debug_end_offset;
    qp_const *debug_metadata;

    if (conversion) {
        debug_end_line = ((expr_ty) conversion->result)->lineno;
        debug_end_offset = ((expr_ty) conversion->result)->col_offset;
        debug_metadata = conversion->metadata;
    }
    else if (format) {
        debug_end_line = ((expr_ty) format->result)->lineno;
        debug_end_offset = ((expr_ty) format->result)->col_offset + 1;
        debug_metadata = format->metadata;
    }
    else {
        debug_end_line = end_lineno;
        debug_end_offset = end_col_offset;
        debug_metadata = closing_brace->metadata;
    }
    expr_ty debug_text = _PyAST_Constant(debug_metadata, JS_ATOM_NULL, lineno, col_offset + 1, debug_end_line,
                                            debug_end_offset - 1, p->arena);
    if (!debug_text) {
        return NULL;
    }

    asdl_expr_seq *values = _Py_asdl_expr_seq_new(2, arena);
    if (!values) {
        return NULL;
    }
    asdl_seq_SET(values, 0, debug_text);
    asdl_seq_SET(values, 1, formatted_value);
    return _PyAST_JoinedStr(values, lineno, col_offset, debug_end_line, debug_end_offset, p->arena);
}

/* Concatenate the str (or bytes) values of the Constant nodes
   strings[first..last] into one new constant of the same kind. */
static qp_const *
qp_pegen_concat_constants(Parser *p, asdl_expr_seq *strings, Py_ssize_t first,
                          Py_ssize_t last, qp_const_kind kind)
{
    StringBuffer b;
    Py_ssize_t total = 0;

    for (Py_ssize_t i = first; i <= last; i++) {
        total += qp_pegen_str(asdl_seq_GET(strings, i)->v.Constant.value)->len;
        if (total > JS_STRING_LEN_MAX) {
            return PyErr_NoMemory();
        }
    }
    if (string_buffer_init(p->ctx, &b, (int)total)) {
        string_buffer_free(&b);
        return qp_pegen_js_error(p);
    }
    for (Py_ssize_t i = first; i <= last; i++) {
        expr_ty current_elem = asdl_seq_GET(strings, i);
        assert(current_elem->kind == Constant_kind);
        if (string_buffer_concat_value(&b, current_elem->v.Constant.value->v)) {
            string_buffer_free(&b);
            return qp_pegen_js_error(p);
        }
    }
    return qp_pegen_new_const(p, kind, string_buffer_end(&b));
}

static expr_ty
_build_concatenated_bytes(Parser *p, asdl_expr_seq *strings, int lineno,
                        int col_offset, int end_lineno, int end_col_offset,
                        PyArena *arena)
{
    Py_ssize_t len = asdl_seq_LEN(strings);
    assert(len > 0);

    /* Bytes literals never get a kind, but just for consistency
        since they are represented as Constant nodes, we'll mirror
        the same behavior as unicode strings for determining the
        kind. */
    JSAtom kind = asdl_seq_GET(strings, 0)->v.Constant.kind;

    qp_const *res = qp_pegen_concat_constants(p, strings, 0, len - 1, QP_CONST_BYTES);
    if (res == NULL) {
        return NULL;
    }
    return _PyAST_Constant(res, kind, lineno, col_offset, end_lineno, end_col_offset, p->arena);
}

static expr_ty
_build_concatenated_unicode(Parser *p, asdl_expr_seq *strings, int lineno,
                        int col_offset, int end_lineno, int end_col_offset,
                        PyArena *arena)
{
    Py_ssize_t len = asdl_seq_LEN(strings);
    assert(len > 1);

    expr_ty first = asdl_seq_GET(strings, 0);

    /* When a string is getting concatenated, the kind of the string
        is determined by the first string in the concatenation
        sequence.

        u"abc" "def" -> u"abcdef"
        "abc" u"abc" ->  "abcabc" */
    JSAtom kind = first->v.Constant.kind;

    qp_const *final = qp_pegen_concat_constants(p, strings, 0, len - 1, QP_CONST_STR);
    if (final == NULL) {
        return NULL;
    }
    return _PyAST_Constant(final, kind, lineno, col_offset,
                           end_lineno, end_col_offset, arena);
}

static asdl_expr_seq *
_build_concatenated_str(Parser *p, asdl_expr_seq *strings,
                               int lineno, int col_offset, int end_lineno,
                               int end_col_offset, PyArena *arena)
{
    Py_ssize_t len = asdl_seq_LEN(strings);
    assert(len > 0);

    Py_ssize_t n_flattened_elements = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty elem = asdl_seq_GET(strings, i);
        switch(elem->kind) {
            case JoinedStr_kind:
                n_flattened_elements += asdl_seq_LEN(elem->v.JoinedStr.values);
                break;
            case TemplateStr_kind:
                n_flattened_elements += asdl_seq_LEN(elem->v.TemplateStr.values);
                break;
            default:
                n_flattened_elements++;
                break;
        }
    }


    asdl_expr_seq* flattened = _Py_asdl_expr_seq_new(n_flattened_elements, p->arena);
    if (flattened == NULL) {
        return NULL;
    }

    /* build flattened list */
    Py_ssize_t current_pos = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty elem = asdl_seq_GET(strings, i);
        switch(elem->kind) {
            case JoinedStr_kind:
                for (Py_ssize_t j = 0; j < asdl_seq_LEN(elem->v.JoinedStr.values); j++) {
                    expr_ty subvalue = asdl_seq_GET(elem->v.JoinedStr.values, j);
                    if (subvalue == NULL) {
                        return NULL;
                    }
                    asdl_seq_SET(flattened, current_pos++, subvalue);
                }
                break;
            case TemplateStr_kind:
                for (Py_ssize_t j = 0; j < asdl_seq_LEN(elem->v.TemplateStr.values); j++) {
                    expr_ty subvalue = asdl_seq_GET(elem->v.TemplateStr.values, j);
                    if (subvalue == NULL) {
                        return NULL;
                    }
                    asdl_seq_SET(flattened, current_pos++, subvalue);
                }
                break;
            default:
                asdl_seq_SET(flattened, current_pos++, elem);
                break;
        }
    }

    /* calculate folded element count */
    Py_ssize_t n_elements = 0;
    int prev_is_constant = 0;
    for (Py_ssize_t i = 0; i < n_flattened_elements; i++) {
        expr_ty elem = asdl_seq_GET(flattened, i);

        /* The concatenation of a FormattedValue and an empty Constant should
           lead to the FormattedValue itself. Thus, we will not take any empty
           constants into account, just as in `_PyPegen_joined_str` */
        if (qp_pegen_is_empty_str_constant(elem))
            continue;

        if (!prev_is_constant || elem->kind != Constant_kind) {
            n_elements++;
        }
        prev_is_constant = elem->kind == Constant_kind;
    }

    asdl_expr_seq* values = _Py_asdl_expr_seq_new(n_elements, p->arena);
    if (values == NULL) {
        return NULL;
    }

    /* build folded list */
    current_pos = 0;
    for (Py_ssize_t i = 0; i < n_flattened_elements; i++) {
        expr_ty elem = asdl_seq_GET(flattened, i);

        /* if the current elem and the following are constants,
           fold them and all consequent constants */
        if (elem->kind == Constant_kind) {
            if (i + 1 < n_flattened_elements &&
                asdl_seq_GET(flattened, i + 1)->kind == Constant_kind) {
                expr_ty first_elem = elem;

                /* When a string is getting concatenated, the kind of the string
                   is determined by the first string in the concatenation
                   sequence.

                   u"abc" "def" -> u"abcdef"
                   "abc" u"abc" ->  "abcabc" */
                JSAtom kind = elem->v.Constant.kind;

                expr_ty last_elem = elem;
                Py_ssize_t j;
                for (j = i; j < n_flattened_elements; j++) {
                    expr_ty current_elem = asdl_seq_GET(flattened, j);
                    if (current_elem->kind == Constant_kind) {
                        last_elem = current_elem;
                    } else {
                        break;
                    }
                }

                qp_const *concat_str = qp_pegen_concat_constants(p, flattened, i, j - 1,
                                                                 QP_CONST_STR);
                if (concat_str == NULL) {
                    return NULL;
                }
                i = j - 1;
                elem = _PyAST_Constant(concat_str, kind, first_elem->lineno,
                                       first_elem->col_offset,
                                       last_elem->end_lineno,
                                       last_elem->end_col_offset, p->arena);
                if (elem == NULL) {
                    return NULL;
                }
            }

            /* Drop all empty contanst strings */
            if (qp_pegen_is_empty_str_constant(elem)) {
                continue;
            }
        }

        asdl_seq_SET(values, current_pos++, elem);
    }

    assert(current_pos == n_elements);
    return values;
}

static expr_ty
_build_concatenated_joined_str(Parser *p, asdl_expr_seq *strings,
                               int lineno, int col_offset, int end_lineno,
                               int end_col_offset, PyArena *arena)
{
    asdl_expr_seq *values = _build_concatenated_str(p, strings, lineno,
        col_offset, end_lineno, end_col_offset, arena);
    if (!values) {
        return NULL;
    }
    return _PyAST_JoinedStr(values, lineno, col_offset, end_lineno, end_col_offset, p->arena);
}

expr_ty
_PyPegen_concatenate_tstrings(Parser *p, asdl_expr_seq *strings,
                               int lineno, int col_offset, int end_lineno,
                               int end_col_offset, PyArena *arena)
{
    asdl_expr_seq *values = _build_concatenated_str(p, strings, lineno,
        col_offset, end_lineno, end_col_offset, arena);
    if (!values) {
        return NULL;
    }
    return _PyAST_TemplateStr(values, lineno, col_offset, end_lineno,
        end_col_offset, arena);
}

expr_ty
_PyPegen_concatenate_strings(Parser *p, asdl_expr_seq *strings,
                             int lineno, int col_offset, int end_lineno,
                             int end_col_offset, PyArena *arena)
{
    Py_ssize_t len = asdl_seq_LEN(strings);
    assert(len > 0);

    int f_string_found = 0;
    int unicode_string_found = 0;
    int bytes_found = 0;

    Py_ssize_t i = 0;
    for (i = 0; i < len; i++) {
        expr_ty elem = asdl_seq_GET(strings, i);
        switch (elem->kind) {
            case Constant_kind:
                if (elem->v.Constant.value->kind == QP_CONST_BYTES) {
                    bytes_found = 1;
                } else {
                    unicode_string_found = 1;
                }
                break;
            case JoinedStr_kind:
                f_string_found = 1;
                break;
            case TemplateStr_kind:
                // python.gram handles this; we should never get here
                assert(0);
                break;
            default:
                f_string_found = 1;
                break;
        }
    }

    // Cannot mix unicode and bytes
    if ((unicode_string_found || f_string_found) && bytes_found) {
        RAISE_SYNTAX_ERROR("cannot mix bytes and nonbytes literals");
        return NULL;
    }

    // If it's only bytes or only unicode string, do a simple concat
    if (!f_string_found) {
        if (len == 1) {
            return asdl_seq_GET(strings, 0);
        }
        else if (bytes_found) {
            return _build_concatenated_bytes(p, strings, lineno, col_offset,
                end_lineno, end_col_offset, arena);
        }
        else {
            return _build_concatenated_unicode(p, strings, lineno, col_offset,
                end_lineno, end_col_offset, arena);
        }
    }

    return _build_concatenated_joined_str(p, strings, lineno,
        col_offset, end_lineno, end_col_offset, arena);
}

/* CPython emits a SyntaxWarning for `from . lazy import x` here (outside
   the error pass); warnings are not emitted (see pegen.h), so the only
   observable effect is the absence of an error. */
static int
_warn_relative_import_of_lazy(Parser *p, asdl_seq *dots, expr_ty module)
{
    (void)p;
    (void)dots;
    (void)module;
    return 0;
}

stmt_ty
_PyPegen_checked_from_import(Parser *p, asdl_seq *dots, expr_ty module_name,
                                     asdl_alias_seq *names, expr_ty lazy_token, int lineno,
                                     int col_offset, int end_lineno, int end_col_offset,
                                     PyArena *arena)
{
    identifier module = module_name->v.Name.id;
    int level = _PyPegen_seq_count_dots(dots);
    if (level == 0 && qp_pegen_atom_eq(p, module, "__future__")) {
        if (lazy_token) {
            RAISE_SYNTAX_ERROR_KNOWN_LOCATION(lazy_token,
                "lazy from __future__ import is not allowed");
            return NULL;
        }
        for (Py_ssize_t i = 0; i < asdl_seq_LEN(names); i++) {
            alias_ty alias = asdl_seq_GET(names, i);
            if (qp_pegen_atom_eq(p, alias->name, "barry_as_FLUFL")) {
                p->flags |= PyPARSE_BARRY_AS_BDFL;
                p->tok->barry_as_bdfl = 1;
            }
        }
    }
    else if (
        level > 0
        && lazy_token == NULL
        && qp_pegen_atom_eq(p, module, "lazy")
    ) {
        if (_warn_relative_import_of_lazy(p, dots, module_name) < 0) {
            return NULL;
        }
    }
    return _PyAST_ImportFrom(module, names, level, lazy_token ? 1 : 0, lineno,
                             col_offset, end_lineno, end_col_offset, arena);
}

asdl_stmt_seq*
_PyPegen_register_stmts(Parser *p, asdl_stmt_seq* stmts) {
    if (!p->call_invalid_rules) {
        return stmts;
    }
    Py_ssize_t len = asdl_seq_LEN(stmts);
    if (len == 0) {
        return stmts;
    }
    stmt_ty last_stmt = asdl_seq_GET(stmts, len - 1);
    if (p->last_stmt_location.lineno > last_stmt->lineno) {
        return stmts;
    }
    p->last_stmt_location.lineno = last_stmt->lineno;
    p->last_stmt_location.col_offset = last_stmt->col_offset;
    p->last_stmt_location.end_lineno = last_stmt->end_lineno;
    p->last_stmt_location.end_col_offset = last_stmt->end_col_offset;
    return stmts;
}

/* ---------------------------------------------------------------------- */
/* Escape decoders (Objects/unicodeobject.c, Objects/bytesobject.c) */

/* _PyUnicode_DecodeUnicodeEscapeInternal2(s, size, NULL, NULL, ...): decode
   the "unicode_escape" codec input s[0:size] (strict errors).  Sets *res to
   the str value, or returns -1 with the UnicodeDecodeError set (positions
   relative to s).  Invalid escapes are kept literally; their warning is not
   emitted (see pegen.h). */
static int
qp_decode_unicode_escape(Parser *p, const char *s, Py_ssize_t size, JSValue *res)
{
    const char *starts = s;
    const char *end;
    StringBuffer writer;

    if (size == 0) {
        *res = JS_AtomToString(p->ctx, JS_ATOM_empty_string);
        if (JS_IsException(*res)) {
            qp_pegen_js_error(p);
            return -1;
        }
        return 0;
    }
    if (size > JS_STRING_LEN_MAX) {
        qp_pegen_no_memory(p);
        return -1;
    }
    /* Escaped strings will always be longer than the resulting
       Unicode string, so we start with size here. */
    if (string_buffer_init(p->ctx, &writer, (int)size)) {
        goto onError;
    }

    end = s + size;
    while (s < end) {
        unsigned char c = (unsigned char) *s++;
        uint32_t ch;
        int count;
        const char *message;

#define WRITE_CHAR(ch)                                                        \
            do {                                                              \
                if (string_buffer_putc(&writer, (ch)))                        \
                    goto onError;                                             \
            } while(0)

        /* Non-escape characters are interpreted as Unicode ordinals */
        if (c != '\\') {
            WRITE_CHAR(c);
            continue;
        }

        Py_ssize_t startinpos = s - starts - 1;
        /* \ - Escapes */
        if (s >= end) {
            message = "\\ at end of string";
            goto incomplete;
        }
        c = (unsigned char) *s++;

        switch (c) {

            /* \x escapes */
        case '\n': continue;
        case '\\': WRITE_CHAR('\\'); continue;
        case '\'': WRITE_CHAR('\''); continue;
        case '\"': WRITE_CHAR('\"'); continue;
        case 'b': WRITE_CHAR('\b'); continue;
        /* FF */
        case 'f': WRITE_CHAR('\014'); continue;
        case 't': WRITE_CHAR('\t'); continue;
        case 'n': WRITE_CHAR('\n'); continue;
        case 'r': WRITE_CHAR('\r'); continue;
        /* VT */
        case 'v': WRITE_CHAR('\013'); continue;
        /* BEL, not classic C */
        case 'a': WRITE_CHAR('\007'); continue;

            /* \OOO (octal) escapes */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
            ch = c - '0';
            if (s < end && '0' <= *s && *s <= '7') {
                ch = (ch<<3) + *s++ - '0';
                if (s < end && '0' <= *s && *s <= '7') {
                    ch = (ch<<3) + *s++ - '0';
                }
            }
            WRITE_CHAR(ch);
            continue;

            /* hex escapes */
            /* \xXX */
        case 'x':
            count = 2;
            message = "truncated \\xXX escape";
            goto hexescape;

            /* \uXXXX */
        case 'u':
            count = 4;
            message = "truncated \\uXXXX escape";
            goto hexescape;

            /* \UXXXXXXXX */
        case 'U':
            count = 8;
            message = "truncated \\UXXXXXXXX escape";
        hexescape:
            for (ch = 0; count; ++s, --count) {
                if (s >= end) {
                    goto incomplete;
                }
                c = (unsigned char)*s;
                ch <<= 4;
                if (c >= '0' && c <= '9') {
                    ch += c - '0';
                }
                else if (c >= 'a' && c <= 'f') {
                    ch += c - ('a' - 10);
                }
                else if (c >= 'A' && c <= 'F') {
                    ch += c - ('A' - 10);
                }
                else {
                    goto error;
                }
            }

            /* when we get here, ch is a 32-bit unicode character */
            if (ch > 0x10ffff) {
                message = "illegal Unicode character";
                goto error;
            }

            WRITE_CHAR(ch);
            continue;

            /* \N{name} */
        case 'N':
            message = "malformed \\N character escape";
            if (s >= end) {
                goto incomplete;
            }
            if (*s == '{') {
                const char *start = ++s;
                size_t namelen;
                /* look for the closing brace */
                while (s < end && *s != '}')
                    s++;
                if (s >= end) {
                    goto incomplete;
                }
                namelen = s - start;
                if (namelen) {
                    /* found a name.  look it up in the unicode database */
                    s++;
                    ch = 0xffffffff; /* in case 'getcode' messes up */
                    if (namelen <= INT_MAX &&
                        qp_unicode_lookup_name(start, namelen, &ch)) {
                        assert(ch <= 0x10ffff);
                        WRITE_CHAR(ch);
                        continue;
                    }
                    message = "unknown Unicode character name";
                }
            }
            goto error;

        default:
            WRITE_CHAR('\\');
            WRITE_CHAR(c);
            continue;
        }

      incomplete:
      error:;
        Py_ssize_t endinpos = s-starts;
        string_buffer_free(&writer);
        qp_pegen_set_decode_error(p, "unicodeescape", starts, size,
                                  startinpos, endinpos, message);
        return -1;

#undef WRITE_CHAR
    }

    *res = string_buffer_end(&writer);
    if (JS_IsException(*res)) {
        qp_pegen_js_error(p);
        return -1;
    }
    return 0;

  onError:
    string_buffer_free(&writer);
    qp_pegen_js_error(p);
    return -1;
}

/* _PyBytes_DecodeEscape2(s, len, NULL, ...): unescape a bytes literal body
   (strict errors).  Returns the bytes constant, or NULL with ValueError set.
   Invalid escapes are kept literally (their warning is not emitted). */
static qp_const *
qp_decode_bytes_escape(Parser *p, const char *s, Py_ssize_t len)
{
    char *buf = PyMem_Malloc((size_t)len + 1);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    char *q = buf;

    const char *end = s + len;
    while (s < end) {
        if (*s != '\\') {
            *q++ = *s++;
            continue;
        }

        s++;
        if (s == end) {
            qp_error_clear(p->err);
            qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                         "Trailing \\ in string");
            goto failed;
        }

        switch (*s++) {
        /* XXX This assumes ASCII! */
        case '\n': break;
        case '\\': *q++ = '\\'; break;
        case '\'': *q++ = '\''; break;
        case '\"': *q++ = '\"'; break;
        case 'b': *q++ = '\b'; break;
        case 'f': *q++ = '\014'; break; /* FF */
        case 't': *q++ = '\t'; break;
        case 'n': *q++ = '\n'; break;
        case 'r': *q++ = '\r'; break;
        case 'v': *q++ = '\013'; break; /* VT */
        case 'a': *q++ = '\007'; break; /* BEL, not classic C */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int c = s[-1] - '0';
            if (s < end && '0' <= *s && *s <= '7') {
                c = (c<<3) + *s++ - '0';
                if (s < end && '0' <= *s && *s <= '7')
                    c = (c<<3) + *s++ - '0';
            }
            *q++ = c;
            break;
        }
        case 'x':
            if (s+1 < end) {
                int digit1, digit2;
                digit1 = from_hex((unsigned char)s[0]);
                digit2 = from_hex((unsigned char)s[1]);
                if (digit1 >= 0 && digit2 >= 0) {
                    *q++ = (unsigned char)((digit1 << 4) + digit2);
                    s += 2;
                    break;
                }
            }
            /* invalid hexadecimal digits */
            qp_error_clear(p->err);
            qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0,
                         "invalid \\x escape at position %lld",
                         (long long)(s - 2 - (end - len)));
            goto failed;

        default:
            *q++ = '\\';
            s--;
        }
    }

    qp_const *res = qp_pegen_new_const(p, QP_CONST_BYTES,
                                       js_new_string8_len(p->ctx, buf, (int)(q - buf)));
    PyMem_Free(buf);
    return res;

  failed:
    PyMem_Free(buf);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Parser/string_parser.c */

//// STRING HANDLING FUNCTIONS ////

/* Invalid escape sequences produce a SyntaxWarning in CPython
   (warn_invalid_escape_sequence); with no warnings machinery they are
   accepted silently, which is what the default warning filters amount to. */

static qp_const *
decode_unicode_with_escapes(Parser *parser, const char *s, size_t len, Token *t)
{
    char *buf;
    char *p;
    const char *end;
    JSValue v;

    (void)t;
    /* check for integer overflow */
    if (len > (size_t)PY_SSIZE_T_MAX / 6) {
        return NULL;
    }
    /* "ä" (2 bytes) may become "\U000000E4" (10 bytes), or 1:5
       "\ä" (3 bytes) may become "\u005c\U000000E4" (16 bytes), or ~1:6 */
    buf = js_malloc_rt(parser->ctx->rt, len * 6 + 1);
    if (buf == NULL) {
        return qp_pegen_no_memory(parser);
    }
    p = buf;
    end = s + len;
    while (s < end) {
        if (*s == '\\') {
            *p++ = *s++;
            if (s >= end || *s & 0x80) {
                strcpy(p, "u005c");
                p += 5;
                if (s >= end) {
                    break;
                }
            }
        }
        if (*s & 0x80) {
            /* decode_utf8(): the run of non-ASCII bytes, written back as
               \UXXXXXXXX escapes */
            const char *run = s;
            while (s < end && (*s & 0x80)) {
                s++;
            }
            if (qp_utf8_check(parser, run, s - run) < 0) {
                js_free_rt(parser->ctx->rt, buf);
                return NULL;
            }
            const uint8_t *r = (const uint8_t *)run, *rend = (const uint8_t *)s;
            while (r < rend) {
                uint32_t chr;
                const char *reason;
                r += qp_utf8_decode1(r, rend, &chr, &reason);
                snprintf(p, 11, "\\U%08x", (unsigned)chr);
                p += 10;
            }
        }
        else {
            *p++ = *s++;
        }
    }
    *p = '\0';
    len = (size_t)(p - buf);

    if (qp_decode_unicode_escape(parser, buf, (Py_ssize_t)len, &v) < 0) {
        js_free_rt(parser->ctx->rt, buf);
        return NULL;
    }
    js_free_rt(parser->ctx->rt, buf);
    return qp_const_new(parser->arena, QP_CONST_STR, v);
}

static qp_const *
decode_bytes_with_escapes(Parser *p, const char *s, Py_ssize_t len, Token *t)
{
    (void)t;
    return qp_decode_bytes_escape(p, s, len);
}

qp_const *
_PyPegen_decode_string(Parser *p, int raw, const char *s, size_t len, Token *t)
{
    if (raw) {
        /* PyUnicode_DecodeUTF8Stateful(s, len, NULL, NULL) */
        if (qp_utf8_check(p, s, (Py_ssize_t)len) < 0) {
            return NULL;
        }
        return qp_pegen_new_const(p, QP_CONST_STR, JS_NewStringLen(p->ctx, s, len));
    }
    return decode_unicode_with_escapes(p, s, len, t);
}

/* PyErr_BadInternalCall() */
static void *
qp_pegen_bad_internal_call(Parser *p)
{
    qp_error_clear(p->err);
    qp_error_set(p->err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                 "bad argument to internal function");
    return NULL;
}

/* s must include the bracketing quote characters, and r, b &/or f prefixes
    (if any), and embedded escape sequences (if any). (f-strings are handled by the parser)
   _PyPegen_parse_string parses it, and returns the decoded Python string object. */
qp_const *
_PyPegen_parse_string(Parser *p, Token *t)
{
    const char *s = t->bytes;
    if (s == NULL) {
        return NULL;
    }

    size_t len;
    int quote = (unsigned char)*s;
    int bytesmode = 0;
    int rawmode = 0;

    if ((quote >= 'a' && quote <= 'z') || (quote >= 'A' && quote <= 'Z')) {
        while (!bytesmode || !rawmode) {
            if (quote == 'b' || quote == 'B') {
                quote =(unsigned char)*++s;
                bytesmode = 1;
            }
            else if (quote == 'u' || quote == 'U') {
                quote = (unsigned char)*++s;
            }
            else if (quote == 'r' || quote == 'R') {
                quote = (unsigned char)*++s;
                rawmode = 1;
            }
            else {
                break;
            }
        }
    }

    if (quote != '\'' && quote != '\"') {
        return qp_pegen_bad_internal_call(p);
    }

    /* Skip the leading quote char. */
    s++;
    len = strlen(s);
    // gh-120155: 's' contains at least the trailing quote,
    // so the code '--len' below is safe.
    assert(len >= 1);

    if (len > INT_MAX) {
        qp_error_clear(p->err);
        qp_error_set(p->err, QP_ERR_OVERFLOW, 0, 0, 0, 0, "%s",
                     "string to parse is too long");
        return NULL;
    }
    if (s[--len] != quote) {
        /* Last quote char must match the first. */
        return qp_pegen_bad_internal_call(p);
    }
    if (len >= 4 && s[0] == quote && s[1] == quote) {
        /* A triple quoted string. We've already skipped one quote at
           the start and one at the end of the string. Now skip the
           two at the start. */
        s += 2;
        len -= 2;
        /* And check that the last two match. */
        if (s[--len] != quote || s[--len] != quote) {
            return qp_pegen_bad_internal_call(p);
        }
    }

    /* Avoid invoking escape decoding routines if possible. */
    rawmode = rawmode || strchr(s, '\\') == NULL;
    if (bytesmode) {
        /* Disallow non-ASCII characters. */
        const char *ch;
        for (ch = s; *ch; ch++) {
            if ((unsigned char)*ch >= 0x80) {
                RAISE_SYNTAX_ERROR_KNOWN_LOCATION(
                                   t,
                                   "bytes can only contain ASCII "
                                   "literal characters");
                return NULL;
            }
        }
        if (rawmode) {
            return qp_pegen_new_const(p, QP_CONST_BYTES,
                                      js_new_string8_len(p->ctx, s, (int)len));
        }
        return decode_bytes_with_escapes(p, s, (Py_ssize_t)len, t);
    }
    return _PyPegen_decode_string(p, rawmode, s, len, t);
}
