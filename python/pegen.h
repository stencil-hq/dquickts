/*
 * PEG parser support: Parser/Token state, error helpers, action helpers.
 *
 * Derived from CPython Parser/pegen.h and Parser/string_parser.h (PSF-2.0).
 *
 * Differences from CPython (python/pegen.c implements them):
 *  - Parser gains `JSContext *ctx` and `qp_error *err` (err is tok->err and
 *    must also be qp_arena_error(arena)).  The Python error indicator is
 *    `p->err->kind != QP_ERR_NONE`; exception classes are qp_error_kind
 *    values.  Raising a parser error replaces any pending error, as
 *    PyErr_SetObject does.
 *  - Token.bytes is an arena-allocated NUL-terminated copy of the token text
 *    (bytes_len bytes); Token.metadata (f-string debug / t-string expression
 *    text) is a QP_CONST_STR constant or NULL.
 *  - identifier/string values are JSAtom (JS_ATOM_NULL = absent), constants
 *    are qp_const *.  Py_None/Py_True/Py_False/Py_Ellipsis are the qp_const_*
 *    singletons.  _PyPegen_new_identifier / _PyPegen_new_type_comment return
 *    JS_ATOM_NULL on error.
 *  - Message formats of the RAISE_* helpers follow PyUnicode_FromFormat for
 *    %s %c %d %i %zd %%; %U and %R take a JSAtom (str() / repr() of it).
 *  - SyntaxWarnings (invalid escape sequences, "from . lazy import") go
 *    through qp_syntax_warn with p->tok->filename / p->tok->module; one
 *    raised as an exception that is not a SyntaxWarning is reported as
 *    QP_ERR_PENDING (the exception stays pending in p->ctx).
 *  - SyntaxError.metadata (_PyPegen_set_syntax_error_metadata) and the
 *    error text read back from the source file by filename
 *    (_PyErr_ProgramDecodedTextObject) have no qp_error equivalent; the text
 *    always comes from the tokenizer buffers, as for "<string>" sources.
 *  - Only the string entry point exists (no file/interactive tokenizer).
 *
 * The CPython public-API names needed by pegen.c and the generated parser.c
 * are unit-local macros in pegen_compat.h (included by pegen.c) and are
 * undefined by pegen_undef.h at the end of parser.c.
 */
#ifndef QP_PEGEN_H
#define QP_PEGEN_H

/* compile() flags understood by qp_parse_string (CPython PyCF_* values). */
#define QP_PF_SOURCE_IS_UTF8           0x0100  /* PyCF_SOURCE_IS_UTF8: src is str text */
#define QP_PF_DONT_IMPLY_DEDENT        0x0200  /* PyCF_DONT_IMPLY_DEDENT */
#define QP_PF_IGNORE_COOKIE            0x0800  /* PyCF_IGNORE_COOKIE (same effect as SOURCE_IS_UTF8) */
#define QP_PF_TYPE_COMMENTS            0x1000  /* PyCF_TYPE_COMMENTS */
#define QP_PF_TOP_LEVEL_AWAIT          0x2000  /* PyCF_ALLOW_TOP_LEVEL_AWAIT */
#define QP_PF_ALLOW_INCOMPLETE_INPUT   0x4000  /* PyCF_ALLOW_INCOMPLETE_INPUT */
/* Private (not a PyCF_* value): src is the contents of a source file run
   by the interpreter (_PyParser_ASTFromFile): a null byte is the
   tokenizer's SyntaxError "source code cannot contain null bytes" at its
   line, not compile()'s "source code string ..." up front. */
#define QP_PF_SOURCE_FILE          0x20000000
#define QP_PF_BARRY_AS_BDFL        0x400000    /* CO_FUTURE_BARRY_AS_BDFL */

/* Start rules (mode argument of qp_parse_string). */
#define QP_INPUT_SINGLE     256  /* Py_single_input */
#define QP_INPUT_FILE       257  /* Py_file_input */
#define QP_INPUT_EVAL       258  /* Py_eval_input */
#define QP_INPUT_FUNC_TYPE  345  /* Py_func_type_input */

/* Parse a whole source string (compile()/ast.parse front half).
   mode: QP_INPUT_*; flags: QP_PF_*.  Without QP_PF_SOURCE_IS_UTF8 /
   QP_PF_IGNORE_COOKIE, src is bytes and the BOM / coding cookie decide the
   encoding.  The source is parsed with feature_version 15.  `module` is the
   module name SyntaxWarnings are reported for (QP_NULL = derived from
   filename, JS_UNDEFINED = warnings dropped).  `err` must be the arena's
   error sink.  Returns NULL with err set on failure. */
static __maybe_unused mod_ty qp_parse_string(JSContext *ctx, qp_arena *arena, const char *src,
                                             size_t len, const char *filename,
                                             JSValueConst module, int mode,
                                             int flags, qp_error *err);

#define PyPARSE_DONT_IMPLY_DEDENT       0x0002
#define PyPARSE_IGNORE_COOKIE 0x0010
#define PyPARSE_BARRY_AS_BDFL 0x0020
#define PyPARSE_TYPE_COMMENTS 0x0040
#define PyPARSE_ALLOW_INCOMPLETE_INPUT 0x0100

#define CURRENT_POS (-5)

/* TOK_GET_MODE / TOK_GET_STRING_PREFIX are defined in pegen_compat.h:
   lexer.c (compiled between this header and pegen.c) defines and
   undefines its own copies. */

typedef struct _memo {
    int type;
    void *node;
    int mark;
    struct _memo *next;
} Memo;

typedef struct {
    int type;
    char *bytes;            /* token text, NUL-terminated, in the arena */
    Py_ssize_t bytes_len;
    int level;
    int lineno, col_offset, end_lineno, end_col_offset;
    Memo *memo;
    qp_const *metadata;     /* QP_CONST_STR or NULL */
} Token;

typedef struct {
    const char *str;
    int type;
} KeywordToken;


typedef struct {
    struct {
        int lineno;
        char *comment;  // The " <tag>" in "# type: ignore <tag>"
    } *items;
    size_t size;
    size_t num_items;
} growable_comment_array;

typedef struct {
    int lineno;
    int col_offset;
    int end_lineno;
    int end_col_offset;
} location;

typedef struct {
    struct tok_state *tok;
    Token **tokens;
    int mark;
    int fill, size;
    PyArena *arena;
    KeywordToken **keywords;
    const char **soft_keywords;
    int n_keyword_lists;
    int start_rule;
    int *errcode;
    int parsing_started;
    int starting_lineno;
    int starting_col_offset;
    int error_indicator;
    int flags;
    int feature_version;
    growable_comment_array type_ignore_comments;
    Token *known_err_token;
    int level;
    int call_invalid_rules;
    int debug;
    location last_stmt_location;
    JSContext *ctx;
    qp_error *err;
} Parser;

typedef struct {
    cmpop_ty cmpop;
    expr_ty expr;
} CmpopExprPair;

typedef struct {
    expr_ty key;
    expr_ty value;
} KeyValuePair;

typedef struct {
    expr_ty key;
    pattern_ty pattern;
} KeyPatternPair;

typedef struct {
    arg_ty arg;
    expr_ty value;
} NameDefaultPair;

typedef struct {
    asdl_arg_seq *plain_names;
    asdl_seq *names_with_defaults; // asdl_seq* of NameDefaultsPair's
} SlashWithDefault;

typedef struct {
    arg_ty vararg;
    asdl_seq *kwonlyargs; // asdl_seq* of NameDefaultsPair's
    arg_ty kwarg;
} StarEtc;

typedef struct { operator_ty kind; } AugOperator;
typedef struct {
    void *element;
    int is_keyword;
} KeywordOrStarred;

typedef struct {
    void *result;
    qp_const *metadata;
} ResultTokenWithMetadata;

// Internal parser functions

static __maybe_unused int _PyPegen_insert_memo(Parser *p, int mark, int type, void *node);
static __maybe_unused int _PyPegen_update_memo(Parser *p, int mark, int type, void *node);
static __maybe_unused int _PyPegen_is_memoized(Parser *p, int type, void *pres);

static __maybe_unused int _PyPegen_lookahead(int, void *(func)(Parser *), Parser *);
static __maybe_unused int _PyPegen_lookahead_for_expr(int, expr_ty (func)(Parser *), Parser *);
static __maybe_unused int _PyPegen_lookahead_for_stmt(int, stmt_ty (func)(Parser *), Parser *);
static __maybe_unused int _PyPegen_lookahead_with_int(int, Token *(func)(Parser *, int), Parser *, int);
static __maybe_unused int _PyPegen_lookahead_with_string(int, expr_ty (func)(Parser *, const char*), Parser *, const char*);

static __maybe_unused Token *_PyPegen_expect_token(Parser *p, int type);
static __maybe_unused void* _PyPegen_expect_forced_result(Parser *p, void* result, const char* expected);
static __maybe_unused Token *_PyPegen_expect_forced_token(Parser *p, int type, const char* expected);
static __maybe_unused expr_ty _PyPegen_expect_soft_keyword(Parser *p, const char *keyword);
static __maybe_unused expr_ty _PyPegen_soft_keyword_token(Parser *p);
static __maybe_unused Token *_PyPegen_get_last_nonnwhitespace_token(Parser *);
static __maybe_unused int _PyPegen_fill_token(Parser *p);
static __maybe_unused expr_ty _PyPegen_name_token(Parser *p);
static __maybe_unused expr_ty _PyPegen_number_token(Parser *p);
static __maybe_unused void *_PyPegen_string_token(Parser *p);
static __maybe_unused Py_ssize_t _PyPegen_byte_offset_to_character_offset(const char *line, Py_ssize_t col_offset);
static __maybe_unused Py_ssize_t _PyPegen_byte_offset_to_character_offset_raw(const char*, Py_ssize_t col_offset);

// Error handling functions and APIs
typedef enum {
    STAR_TARGETS,
    DEL_TARGETS,
    FOR_TARGETS
} TARGETS_TYPE;

static __maybe_unused int _Pypegen_raise_decode_error(Parser *p);
static __maybe_unused int _Pypegen_raise_decode_error_known_location(Parser *p, expr_ty a);
static __maybe_unused int _Pypegen_tokenizer_error(Parser *p);
static __maybe_unused void *_PyPegen_raise_error(Parser *p, qp_error_kind errtype, int use_mark, const char *errmsg, ...);
static __maybe_unused void *_PyPegen_raise_error_known_location(Parser *p, qp_error_kind errtype,
                                          Py_ssize_t lineno, Py_ssize_t col_offset,
                                          Py_ssize_t end_lineno, Py_ssize_t end_col_offset,
                                          const char *errmsg, va_list va);
static __maybe_unused void _Pypegen_set_syntax_error(Parser* p, Token* last_token);
static __maybe_unused void _Pypegen_stack_overflow(Parser *p);

static inline void *
RAISE_ERROR_KNOWN_LOCATION(Parser *p, qp_error_kind errtype,
                           Py_ssize_t lineno, Py_ssize_t col_offset,
                           Py_ssize_t end_lineno, Py_ssize_t end_col_offset,
                           const char *errmsg, ...)
{
    va_list va;
    va_start(va, errmsg);
    Py_ssize_t _col_offset = (col_offset == CURRENT_POS ? CURRENT_POS : col_offset + 1);
    Py_ssize_t _end_col_offset = (end_col_offset == CURRENT_POS ? CURRENT_POS : end_col_offset + 1);
    _PyPegen_raise_error_known_location(p, errtype, lineno, _col_offset, end_lineno, _end_col_offset, errmsg, va);
    va_end(va);
    return NULL;
}
#define RAISE_SYNTAX_ERROR(msg, ...) _PyPegen_raise_error(p, QP_ERR_SYNTAX, 0, msg, ##__VA_ARGS__)
#define RAISE_INDENTATION_ERROR(msg, ...) _PyPegen_raise_error(p, QP_ERR_INDENTATION, 0, msg, ##__VA_ARGS__)
#define RAISE_SYNTAX_ERROR_ON_NEXT_TOKEN(msg, ...) _PyPegen_raise_error(p, QP_ERR_SYNTAX, 1, msg, ##__VA_ARGS__)
#define RAISE_SYNTAX_ERROR_KNOWN_RANGE(a, b, msg, ...) \
    RAISE_ERROR_KNOWN_LOCATION(p, QP_ERR_SYNTAX, (a)->lineno, (a)->col_offset, (b)->end_lineno, (b)->end_col_offset, msg, ##__VA_ARGS__)
#define RAISE_SYNTAX_ERROR_KNOWN_LOCATION(a, msg, ...) \
    RAISE_ERROR_KNOWN_LOCATION(p, QP_ERR_SYNTAX, (a)->lineno, (a)->col_offset, (a)->end_lineno, (a)->end_col_offset, msg, ##__VA_ARGS__)
#define RAISE_SYNTAX_ERROR_STARTING_FROM(a, msg, ...) \
    RAISE_ERROR_KNOWN_LOCATION(p, QP_ERR_SYNTAX, (a)->lineno, (a)->col_offset, CURRENT_POS, CURRENT_POS, msg, ##__VA_ARGS__)
#define RAISE_SYNTAX_ERROR_INVALID_TARGET(type, e) _RAISE_SYNTAX_ERROR_INVALID_TARGET(p, type, e)

static inline int
_PyPegen_tokens_are_adjacent(Token *a, Token *b)
{
    return (a->end_lineno == b->lineno) && (a->end_col_offset == b->col_offset);
}

static inline void *
CHECK_CALL(Parser *p, void *result)
{
    if (result == NULL) {
        assert(p->err->kind != QP_ERR_NONE);
        p->error_indicator = 1;
    }
    return result;
}

/* This is needed for helper functions that are allowed to
   return NULL without an error. Example: _PyPegen_seq_extract_starred_exprs */
static inline void *
CHECK_CALL_NULL_ALLOWED(Parser *p, void *result)
{
    if (result == NULL && p->err->kind != QP_ERR_NONE) {
        p->error_indicator = 1;
    }
    return result;
}

#define CHECK(type, result) ((type) CHECK_CALL(p, result))
#define CHECK_NULL_ALLOWED(type, result) ((type) CHECK_CALL_NULL_ALLOWED(p, result))

static __maybe_unused expr_ty _PyPegen_get_invalid_target(expr_ty e, TARGETS_TYPE targets_type);
static __maybe_unused const char *_PyPegen_get_expr_name(expr_ty);
static inline void *
_RAISE_SYNTAX_ERROR_INVALID_TARGET(Parser *p, TARGETS_TYPE type, void *e)
{
    expr_ty invalid_target = CHECK_NULL_ALLOWED(expr_ty, _PyPegen_get_invalid_target(e, type));
    if (invalid_target != NULL) {
        const char *msg;
        if (type == STAR_TARGETS || type == FOR_TARGETS) {
            msg = "cannot assign to %s";
        }
        else {
            msg = "cannot delete %s";
        }
        return RAISE_SYNTAX_ERROR_KNOWN_LOCATION(
            invalid_target,
            msg,
            _PyPegen_get_expr_name(invalid_target)
        );
        return RAISE_SYNTAX_ERROR_KNOWN_LOCATION(invalid_target, "invalid syntax");
    }
    return NULL;
}

// Action utility functions

static __maybe_unused void *_PyPegen_dummy_name(Parser *p, ...);
static __maybe_unused void * _PyPegen_seq_last_item(asdl_seq *seq);
#define PyPegen_last_item(seq, type) ((type)_PyPegen_seq_last_item((asdl_seq*)seq))
static __maybe_unused void * _PyPegen_seq_first_item(asdl_seq *seq);
#define PyPegen_first_item(seq, type) ((type)_PyPegen_seq_first_item((asdl_seq*)seq))
#define UNUSED(expr) do { (void)(expr); } while (0)
#define EXTRA_EXPR(head, tail) head->lineno, (head)->col_offset, (tail)->end_lineno, (tail)->end_col_offset, p->arena
#define EXTRA _start_lineno, _start_col_offset, _end_lineno, _end_col_offset, p->arena
static __maybe_unused JSAtom _PyPegen_new_type_comment(Parser *, const char *);

static inline JSAtom
NEW_TYPE_COMMENT(Parser *p, Token *tc)
{
    if (tc == NULL) {
        return JS_ATOM_NULL;
    }
    const char *bytes = tc->bytes;
    if (bytes == NULL) {
        goto error;
    }
    JSAtom tco = _PyPegen_new_type_comment(p, bytes);
    if (tco == JS_ATOM_NULL) {
        goto error;
    }
    return tco;
 error:
    p->error_indicator = 1;  // Inline CHECK_CALL
    return JS_ATOM_NULL;
}

static inline void *
INVALID_VERSION_CHECK(Parser *p, int version, const char *msg, void *node)
{
    if (node == NULL) {
        p->error_indicator = 1;  // Inline CHECK_CALL
        return NULL;
    }
    if (p->feature_version < version) {
        p->error_indicator = 1;
        return RAISE_SYNTAX_ERROR("%s only supported in Python 3.%i and greater",
                                  msg, version);
    }
    return node;
}

#define CHECK_VERSION(type, version, msg, node) ((type) INVALID_VERSION_CHECK(p, version, msg, node))

static __maybe_unused arg_ty _PyPegen_add_type_comment_to_arg(Parser *, arg_ty, Token *);
static __maybe_unused JSAtom _PyPegen_new_identifier(Parser *, const char *);
static __maybe_unused asdl_seq *_PyPegen_singleton_seq(Parser *, void *);
static __maybe_unused asdl_seq *_PyPegen_seq_insert_in_front(Parser *, void *, asdl_seq *);
static __maybe_unused asdl_seq *_PyPegen_seq_append_to_end(Parser *, asdl_seq *, void *);
static __maybe_unused asdl_seq *_PyPegen_seq_flatten(Parser *, asdl_seq *);
static __maybe_unused expr_ty _PyPegen_join_names_with_dot(Parser *, expr_ty, expr_ty);
static __maybe_unused int _PyPegen_seq_count_dots(asdl_seq *);
static __maybe_unused alias_ty _PyPegen_alias_for_star(Parser *, int, int, int, int, PyArena *);
static __maybe_unused asdl_identifier_seq *_PyPegen_map_names_to_ids(Parser *, asdl_expr_seq *);
static __maybe_unused CmpopExprPair *_PyPegen_cmpop_expr_pair(Parser *, cmpop_ty, expr_ty);
static __maybe_unused asdl_int_seq *_PyPegen_get_cmpops(Parser *p, asdl_seq *);
static __maybe_unused asdl_expr_seq *_PyPegen_get_exprs(Parser *, asdl_seq *);
static __maybe_unused expr_ty _PyPegen_set_expr_context(Parser *, expr_ty, expr_context_ty);
static __maybe_unused KeyValuePair *_PyPegen_key_value_pair(Parser *, expr_ty, expr_ty);
static __maybe_unused asdl_expr_seq *_PyPegen_get_keys(Parser *, asdl_seq *);
static __maybe_unused asdl_expr_seq *_PyPegen_get_values(Parser *, asdl_seq *);
static __maybe_unused KeyPatternPair *_PyPegen_key_pattern_pair(Parser *, expr_ty, pattern_ty);
static __maybe_unused asdl_expr_seq *_PyPegen_get_pattern_keys(Parser *, asdl_seq *);
static __maybe_unused asdl_pattern_seq *_PyPegen_get_patterns(Parser *, asdl_seq *);
static __maybe_unused NameDefaultPair *_PyPegen_name_default_pair(Parser *, arg_ty, expr_ty, Token *);
static __maybe_unused SlashWithDefault *_PyPegen_slash_with_default(Parser *, asdl_arg_seq *, asdl_seq *);
static __maybe_unused StarEtc *_PyPegen_star_etc(Parser *, arg_ty, asdl_seq *, arg_ty);
static __maybe_unused arguments_ty _PyPegen_make_arguments(Parser *, asdl_arg_seq *, SlashWithDefault *,
                                     asdl_arg_seq *, asdl_seq *, StarEtc *);
static __maybe_unused arguments_ty _PyPegen_empty_arguments(Parser *);
static __maybe_unused expr_ty _PyPegen_template_str(Parser *p, Token *a, asdl_expr_seq *raw_expressions, Token *b);
static __maybe_unused expr_ty _PyPegen_joined_str(Parser *p, Token *a, asdl_expr_seq *raw_expressions, Token *b);
static __maybe_unused expr_ty _PyPegen_interpolation(Parser *, expr_ty, Token *, ResultTokenWithMetadata *, ResultTokenWithMetadata *, Token *,
                                 int, int, int, int, PyArena *);
static __maybe_unused expr_ty _PyPegen_formatted_value(Parser *, expr_ty, Token *, ResultTokenWithMetadata *, ResultTokenWithMetadata *, Token *,
                                 int, int, int, int, PyArena *);
static __maybe_unused AugOperator *_PyPegen_augoperator(Parser*, operator_ty type);
static __maybe_unused stmt_ty _PyPegen_function_def_decorators(Parser *, asdl_expr_seq *, stmt_ty);
static __maybe_unused stmt_ty _PyPegen_class_def_decorators(Parser *, asdl_expr_seq *, stmt_ty);
static __maybe_unused KeywordOrStarred *_PyPegen_keyword_or_starred(Parser *, void *, int);
static __maybe_unused asdl_expr_seq *_PyPegen_seq_extract_starred_exprs(Parser *, asdl_seq *);
static __maybe_unused asdl_keyword_seq *_PyPegen_seq_delete_starred_exprs(Parser *, asdl_seq *);
static __maybe_unused expr_ty _PyPegen_collect_call_seqs(Parser *, asdl_expr_seq *, asdl_seq *,
                     int lineno, int col_offset, int end_lineno,
                     int end_col_offset, PyArena *arena);
static __maybe_unused expr_ty _PyPegen_constant_from_token(Parser* p, Token* tok);
static __maybe_unused expr_ty _PyPegen_decoded_constant_from_token(Parser* p, Token* tok);
static __maybe_unused expr_ty _PyPegen_constant_from_string(Parser* p, Token* tok);
static __maybe_unused expr_ty _PyPegen_concatenate_tstrings(Parser *p, asdl_expr_seq *, int, int, int, int, PyArena *);
static __maybe_unused expr_ty _PyPegen_concatenate_strings(Parser *p, asdl_expr_seq *, int, int, int, int, PyArena *);
static __maybe_unused expr_ty _PyPegen_ensure_imaginary(Parser *p, expr_ty);
static __maybe_unused expr_ty _PyPegen_ensure_real(Parser *p, expr_ty);
static __maybe_unused asdl_seq *_PyPegen_join_sequences(Parser *, asdl_seq *, asdl_seq *);
static __maybe_unused int _PyPegen_check_barry_as_flufl(Parser *, Token *);
static __maybe_unused int _PyPegen_check_legacy_stmt(Parser *p, expr_ty t);
static __maybe_unused void *_PyPegen_raise_error_for_missing_comma(Parser *p, expr_ty a, expr_ty b);
static __maybe_unused ResultTokenWithMetadata *_PyPegen_check_fstring_conversion(Parser *p, Token *, expr_ty t);
static __maybe_unused ResultTokenWithMetadata *_PyPegen_setup_full_format_spec(Parser *, Token *, asdl_expr_seq *, int, int,
                                                         int, int, PyArena *);
static __maybe_unused mod_ty _PyPegen_make_module(Parser *, asdl_stmt_seq *);
static __maybe_unused void *_PyPegen_arguments_parsing_error(Parser *, expr_ty);
static __maybe_unused expr_ty _PyPegen_get_last_comprehension_item(comprehension_ty comprehension);
static __maybe_unused void *_PyPegen_nonparen_genexp_in_call(Parser *p, expr_ty args, asdl_comprehension_seq *comprehensions);
static __maybe_unused stmt_ty _PyPegen_checked_from_import(Parser *p, asdl_seq *dots, expr_ty module_name,
                                     asdl_alias_seq *names, expr_ty lazy_token, int lineno,
                                     int col_offset, int end_lineno, int end_col_offset,
                                     PyArena *arena);
static __maybe_unused asdl_stmt_seq* _PyPegen_register_stmts(Parser *p, asdl_stmt_seq* stmts);

// string_parser.h
static __maybe_unused qp_const *_PyPegen_parse_string(Parser *, Token *);
static __maybe_unused qp_const *_PyPegen_decode_string(Parser *, int, const char *, size_t, Token *);

// Parser API

static __maybe_unused Parser *_PyPegen_Parser_New(struct tok_state *, int, int, int, int *, const char*, PyArena *);
static __maybe_unused void _PyPegen_Parser_Free(Parser *);
static __maybe_unused void *_PyPegen_run_parser(Parser *);
static __maybe_unused mod_ty _PyPegen_run_parser_from_string(const char *, int, const char *,
                                                             JSValueConst, int, PyArena *, int);
static __maybe_unused asdl_stmt_seq *_PyPegen_interactive_exit(Parser *);

// Generated function in parse.c - function definition in python.gram
static __maybe_unused void *_PyPegen_parse(Parser *);

#endif /* QP_PEGEN_H */
