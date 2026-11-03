/*
 * Python tokenizer state and API.
 *
 * Derived from CPython Parser/lexer/state.h, Parser/lexer/lexer.h,
 * Parser/lexer/buffer.h, Parser/tokenizer/tokenizer.h,
 * Parser/tokenizer/helpers.h and Include/errcode.h (PSF-2.0).
 *
 * Only the string tokenizers (_PyTokenizer_FromString for bytes input with
 * BOM / coding-cookie detection, _PyTokenizer_FromUTF8 for str input) exist;
 * the file and readline tokenizers are not ported, nor are the buffer.c
 * reallocation helpers (_PyLexer_tok_reserve_buf & co.), which only those
 * tokenizers use (string input is tokenized in place).
 *
 * Differences from CPython (the parser must follow these):
 *  - Errors go to `qp_error *tok->err` instead of the Python error indicator.
 *    `tok->err->kind != QP_ERR_NONE` is the equivalent of PyErr_Occurred().
 *    The constructors take the error sink as an extra trailing argument
 *    (construction can fail with decoding errors before a caller could set
 *    it) and store it in tok->err.  tok->ctx (given to _PyTokenizer_FromString
 *    and set by the parser; NULL otherwise) is used to emit SyntaxWarnings and
 *    to decode declared encodings through the codec registry.
 *  - Constructor failures are reported already converted the way
 *    _PyTokenizer_raise_init_error / _PyPegen_raise_tokenizer_init_error do:
 *    unknown encodings and codec decode errors become QP_ERR_SYNTAX with
 *    msg = str(exception), lineno 0, offset -1 (filename is the caller's);
 *    tokenizer SyntaxErrors ("Non-UTF-8 code ...", "encoding problem: X with
 *    BOM") keep their positions; OOM is QP_ERR_MEMORY.  Declared encodings
 *    that resolve to the utf_8, latin_1 or ascii codecs are decoded natively;
 *    any other one goes through the codec registry (PyUnicode_Decode) when
 *    tok->ctx is set, else reports "unknown encoding: <name>".  An exception
 *    other than LookupError/ValueError/SyntaxError raised by a codec is
 *    QP_ERR_PENDING.
 *  - struct token.metadata is a qp_sys_malloc'd NUL-terminated UTF-8 copy of the
 *    f-string debug / t-string expression text (CPython: a str object), with
 *    its byte length in metadata_len; NULL when absent.  _PyToken_Free frees
 *    it; a consumer may steal it by setting token.metadata = NULL.
 *  - tok->filename is an unowned `const char *` (may be NULL); tok->module
 *    is a borrowed module name (QP_NULL: derived from the filename,
 *    JS_UNDEFINED: warnings dropped).
 *  - Lexer warnings (invalid escape sequence, "invalid <kind> literal"
 *    SyntaxWarning) go through qp_syntax_warn when tok->ctx is set (else
 *    they are dropped).  A warning raised as a SyntaxWarning exception
 *    becomes the tokenizer SyntaxError; any other exception is reported as
 *    QP_ERR_PENDING.
 *  - All tokenizer memory (tok_state, buffers, metadata) uses
 *    qp_sys_malloc/qp_sys_calloc/qp_sys_realloc/qp_sys_free (libc semantics;
 *    quickjs.c forbids the raw names).  A stolen metadata pointer must be
 *    released with qp_sys_free.
 *  - Source text is NUL-terminated (as in CPython); callers holding a length
 *    must reject embedded NUL bytes themselves, as compile() does
 *    ("source code string cannot contain null bytes").
 */
#ifndef QP_LEXER_H
#define QP_LEXER_H

/* Include/errcode.h */
#define E_OK             10      /* No error */
#define E_EOF            11      /* End Of File */
#define E_INTR           12      /* Interrupted */
#define E_TOKEN          13      /* Bad token */
#define E_SYNTAX         14      /* Syntax error */
#define E_NOMEM          15      /* Ran out of memory */
#define E_DONE           16      /* Parsing complete */
#define E_ERROR          17      /* Execution error */
#define E_TABSPACE       18      /* Inconsistent mixing of tabs and spaces */
#define E_OVERFLOW       19      /* Node had too many children */
#define E_TOODEEP        20      /* Too many indentation levels */
#define E_DEDENT         21      /* No matching outer block for dedent */
#define E_DECODE         22      /* Error in decoding into Unicode */
#define E_EOFS           23      /* EOF in triple-quoted string */
#define E_EOLS           24      /* EOL in single-quoted string */
#define E_LINECONT       25      /* Unexpected characters after a line continuation */
#define E_BADSINGLE      27      /* Ill-formed single statement input */
#define E_INTERACT_STOP  28      /* Interactive mode stopped tokenization */
#define E_COLUMNOVERFLOW 29      /* Column offset overflow */

#define MAXINDENT 100       /* Max indentation level */
#define MAXLEVEL 200        /* Max parentheses level */
#define MAXFSTRINGLEVEL 150 /* Max f-string nesting level */

#define INSIDE_FSTRING(tok) (tok->tok_mode_stack_index > 0)
#define INSIDE_FSTRING_EXPR(tok) (tok->curly_bracket_expr_start_depth >= 0)
#define INSIDE_FSTRING_EXPR_AT_TOP(tok) \
    (tok->curly_bracket_depth - tok->curly_bracket_expr_start_depth == 1)

enum decoding_state {
    STATE_INIT,
    STATE_SEEK_CODING,
    STATE_NORMAL
};

enum interactive_underflow_t {
    /* Normal mode of operation: return a new token when asked in interactive mode */
    IUNDERFLOW_NORMAL,
    /* Forcefully return ENDMARKER when asked for a new token in interactive mode. */
    IUNDERFLOW_STOP,
};

struct token {
    int level;
    int lineno, col_offset, end_lineno, end_col_offset;
    const char *start, *end;
    char *metadata;             /* malloc'd UTF-8, NULL if none (see top comment) */
    Py_ssize_t metadata_len;
};

enum tokenizer_mode_kind_t {
    TOK_REGULAR_MODE,
    TOK_FSTRING_MODE,
};

enum string_kind_t {
    FSTRING,
    TSTRING,
};

#define MAX_EXPR_NESTING 3

typedef struct _tokenizer_mode {
    enum tokenizer_mode_kind_t kind;

    int curly_bracket_depth;
    int curly_bracket_expr_start_depth;

    char quote;
    int quote_size;
    int raw;
    const char* start;
    const char* multi_line_start;
    int first_line;

    Py_ssize_t start_offset;
    Py_ssize_t multi_line_start_offset;

    /* Points into tok->buf, which is retained while INSIDE_FSTRING(tok). */
    const char* last_expr_start;
    Py_ssize_t last_expr_start_offset;

    int in_debug;
    int in_format_spec;

    enum string_kind_t string_kind;
} tokenizer_mode;

/* Tokenizer state */
struct tok_state {
    /* Input state; buf <= cur <= inp <= end */
    /* NB an entire line is held in the buffer */
    char *buf;          /* Input buffer */
    char *cur;          /* Next character in buffer */
    char *inp;          /* End of data in buffer */
    int fp_interactive; /* Always 0 (no file tokenizer); kept for the parser */
    char *interactive_src_start; /* Always NULL (no interactive file input) */
    char *interactive_src_end;
    const char *end;    /* End of input buffer if buf != NULL */
    const char *start;  /* Start of current token if not NULL */
    int done;           /* E_OK normally, E_EOF at EOF, otherwise error code */
    /* NB If done != E_OK, cur must be == inp!!! */
    FILE *fp;           /* Always NULL: tokenizing a string */
    int tabsize;        /* Tab spacing */
    int indent;         /* Current indentation index */
    int indstack[MAXINDENT];            /* Stack of indents */
    int atbol;          /* Nonzero if at begin of new line */
    int pendin;         /* Pending indents (if > 0) or dedents (if < 0) */
    const char *prompt, *nextprompt;    /* Always NULL (no interactive prompting) */
    int lineno;         /* Current line number */
    int qp_nul_lineno;  /* dquickts: line of the first null byte of a source
                           file (the string ends there; tok_nextc reports it
                           when it reads that line, as for a file), 0 = none */
    int first_lineno;   /* First line of a single line or multi line string
                           expression (cf. issue 16806) */
    int starting_col_offset; /* The column offset at the beginning of a token */
    int col_offset;     /* Current col offset */
    int level;          /* () [] {} Parentheses nesting level */
            /* Used to allow free continuations inside them */
    char parenstack[MAXLEVEL];
    int parenlinenostack[MAXLEVEL];
    int parencolstack[MAXLEVEL];
    const char *filename;   /* unowned, may be NULL */
    JSValueConst module;    /* borrowed; QP_NULL if absent, JS_UNDEFINED drops warnings */
    /* Stuff for checking on different tab sizes */
    int altindstack[MAXINDENT];         /* Stack of alternate indents */
    /* Stuff for PEP 0263 */
    enum decoding_state decoding_state;
    int decoding_erred;         /* whether erred in decoding  */
    char *encoding;         /* Source encoding (malloc'd). */
    int cont_line;          /* whether we are in a continuation line. */
    const char* line_start;     /* pointer to start of current line */
    const char* multi_line_start; /* pointer to start of first line of
                                     a single line or multi line string
                                     expression (cf. issue 16806) */
    const char* enc;        /* Encoding for the current str. */
    char* str;          /* Source string being tokenized */
    char* input;       /* Tokenizer's newline translated copy of the string. */

    int type_comments;      /* Whether to look for type comments */

    /* How to proceed when asked for a new token in interactive mode */
    enum interactive_underflow_t interactive_underflow;
    int (*underflow)(struct tok_state *); /* Function to call when buffer is empty and we need to refill it*/

    int report_warnings;
    tokenizer_mode tok_mode_stack[MAXFSTRINGLEVEL];
    int tok_mode_stack_index;
    int tok_extra_tokens;
    int comment_newline;
    int implicit_newline;
    int barry_as_bdfl;

    qp_error *err;          /* error sink (set by the constructors) */
    JSContext *ctx;         /* SyntaxWarnings and codec-registry source decoding;
                               NULL = warnings dropped, native codecs only */
};

static struct tok_state *_PyTokenizer_FromString(const char *str, int exec_input,
                                                 int preserve_crlf, qp_error *err,
                                                 JSContext *ctx);
static struct tok_state *_PyTokenizer_FromUTF8(const char *str, int exec_input,
                                               int preserve_crlf, qp_error *err);
static int _PyTokenizer_Get(struct tok_state *, struct token *);
static void _PyTokenizer_Free(struct tok_state *);
static void _PyToken_Free(struct token *);
static void _PyToken_Init(struct token *);
static int _PyLexer_token_setup(struct tok_state *tok, struct token *token, int type,
                                const char *start, const char *end);
static int _PyLexer_type_comment_token_setup(struct tok_state *tok, struct token *token,
                                             int type, int col_offset, int end_col_offset,
                                             const char *start, const char *end);
static int _PyTokenizer_syntaxerror(struct tok_state *tok, const char *format, ...);
static int _PyTokenizer_syntaxerror_known_range(struct tok_state *tok, int col_offset,
                                                int end_col_offset, const char *format, ...);

#endif /* QP_LEXER_H */
