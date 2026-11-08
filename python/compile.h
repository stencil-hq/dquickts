/*
 * Python code generator: AST + symbol table -> QuickJS bytecode (M2, unit b).
 *
 * Derived from CPython 3.15.2 Python/codegen.c, Python/compile.c and
 * Include/internal/pycore_compile.h (PSF-2.0).
 *
 * Private to python/compile.c (single translation unit, everything static).
 *
 * ===================== HOW CPython CONSTRUCTS MAP ===========================
 * One code unit (CPython struct compiler_unit) owns one JSFunctionDef
 * (u->u_fd) created with js_new_function_def; nested units are children of
 * the enclosing unit's fd and are referenced by OP_fclosure <cpool idx>.
 * Instructions are appended directly to u_fd->byte_code (no CFG, no
 * flowgraph): every CPython ADDOP is translated at the call site with the
 * qpc_* emitters below.  Translation rules (opcode.h is the reference for
 * the subops; READ IT):
 *
 *   NOP                        -> nothing
 *   POP_TOP                    -> qpc_op(c, loc, OP_drop)
 *   PUSH_NULL                  -> qpc_py(c, loc, QP_OP_push_null)
 *   COPY n                     -> qpc_copy(c, loc, n)    (n=1: OP_dup)
 *   SWAP n                     -> qpc_swap(c, loc, n)    (n=2: OP_swap)
 *   LOAD_CONST None            -> qpc_load_none(c, loc)
 *   LOAD_CONST / LOAD_SMALL_INT-> qpc_load_const(c, loc, qp_const *) for AST
 *                                 constants, qpc_load_small_int, qpc_load_bool,
 *                                 qpc_load_str_atom (str constant from an
 *                                 atom), qpc_load_const_value (any JSValue,
 *                                 consumed: tuples, kwnames ...)
 *   LOAD_COMMON_CONSTANT k     -> qpc_py_u8(c, loc, QP_OP_load_common_const, QP_COMMON_CONST_*)
 *   RETURN_VALUE               -> qpc_return(c, loc)  (OP_return or
 *                                 OP_return_async in generator-like units)
 *   JUMP / JUMP_NO_INTERRUPT / JUMP_BACKWARD
 *                              -> qpc_jump(c, loc, OP_goto, lbl)
 *   TO_BOOL + POP_JUMP_IF_FALSE-> qpc_py_jump(c, loc, QP_OP_jump_if_false, lbl)
 *   TO_BOOL + POP_JUMP_IF_TRUE -> qpc_py_jump(c, loc, QP_OP_jump_if_true, lbl)
 *   POP_JUMP_IF_FALSE/TRUE after an instruction that pushes an EXACT bool
 *     (COMPARE_OP with QP_CMP_TO_BOOL, IS_OP, CONTAINS_OP, CHECK_EXC_MATCH,
 *     MATCH_*, TO_BOOL)        -> qpc_jump(c, loc, OP_if_false / OP_if_true, lbl)
 *   POP_JUMP_IF_NONE / _NOT_NONE
 *                              -> qpc_op(OP_is_undefined_or_null) + OP_if_true / OP_if_false
 *   UNARY_NOT (after TO_BOOL)  -> qpc_py(QP_OP_to_bool) + qpc_op(OP_lnot)
 *   CALL n                     -> qpc_py_u16(c, loc, QP_OP_call_method, n)
 *                                 ([callable, self_or_NULL, args] like CPython)
 *   CALL_KW n                  -> qpc_py_u16(c, loc, QP_OP_call_kw, n) after
 *                                 pushing the kwnames tuple (qpc_load_kwnames)
 *   CALL_FUNCTION_EX           -> qpc_py(c, loc, QP_OP_call_ex) (always 4 inputs:
 *                                 push NULL for absent kwargs like CPython 3.15)
 *   LOAD_ATTR/STORE_ATTR/DELETE_ATTR/LOAD_METHOD/LOAD_SUPER_ATTR/IMPORT_NAME/
 *   IMPORT_FROM (ADDOP_NAME)   -> qpc_addop_name(c, loc, QP_OP_xxx, name, u8)
 *                                 (mangles, chooses the operand format from
 *                                 qp_subop_info, allocates IC indexes)
 *   LOAD_NAME/STORE_NAME/LOAD_FAST/LOAD_DEREF/... -> codegen_nameop() ONLY
 *   any other CPython opcode   -> the OP_py subop of the same name (opcode.h),
 *                                 qpc_py / qpc_py_u8 / qpc_py_u16 / qpc_py_atom.
 *   YIELD_VALUE (+RESUME)      -> codegen_addop_yield() / qpc_py_u8(QP_OP_yield_value, ...)
 *   GET_ITER                   -> qpc_py_u8(QP_OP_get_iter, QP_GETITER_*): pushes
 *                                 [iter, idx] (TWO values; CPython 3.15 too)
 *   FOR_ITER L                 -> qpc_py_jump(QP_OP_for_iter, L); END_FOR is not
 *                                 emitted; POP_ITER -> two OP_drop.
 *   RETURN_GENERATOR, RESUME, CACHE, EXTENDED_ARG, MAKE_CELL, COPY_FREE_VARS
 *                              -> nothing (frame setup does it)
 *
 * Exceptions (CPython exception table -> QuickJS catch markers):
 *   SETUP_FINALLY / SETUP_CLEANUP / SETUP_WITH L
 *                              -> qpc_setup(c, loc, L) = OP_catch L.  It pushes
 *                                 a catch marker M (+1 stack slot) that stays
 *                                 at the depth CPython's handler would unwind
 *                                 to.  On an exception the interpreter unwinds
 *                                 to M and replaces it by the exception: the
 *                                 handler starts with [..., exc] exactly like
 *                                 CPython's SETUP_FINALLY handler.  THERE IS
 *                                 NO `lasti` SLOT: for SETUP_CLEANUP and
 *                                 SETUP_WITH handlers CPython has
 *                                 [..., lasti, exc]; every COPY/SWAP/RERAISE
 *                                 operand in such handler code that counts
 *                                 lasti must be decremented (see
 *                                 codegen_pop_except_and_reraise).
 *   POP_BLOCK                  -> qpc_pop_block(c, loc) = OP_drop: the marker
 *                                 must be TOS.  When one value sits above the
 *                                 marker (preserve_tos unwinding), use
 *                                 qpc_pop_block_under(c, loc) = OP_nip.
 *   While a marker is on the stack the QuickJS stack is ONE DEEPER than
 *   CPython's: every COPY n / SWAP n / LIST_APPEND i / SET_ADD i / MAP_ADD i /
 *   DICT_MERGE i ... operand that reaches below a marker pushed after the
 *   referenced value must be incremented by the number of such markers.
 *   RERAISE n                  -> qpc_py_u8(c, loc, QP_OP_reraise, n)
 *   Handlers / exits must leave the stack exactly as compute_stack_size
 *   expects: same depth AND same catch nesting at every join point.
 *   QuickJS gosub/ret are NOT used: finally bodies are duplicated like
 *   CPython does.
 *
 * Yield-from / await: CPython's `<recv> PUSH_NULL? LOAD_CONST None
 * ADD_YIELD_FROM` becomes `<recv, idx> codegen_add_yield_from()`: the
 * caller pushes [recv, idx] (idx = NULL via push_null after GET_AWAITABLE /
 * GET_ANEXT, or the idx pushed by get_iter for yield from) and does NOT push
 * the None: codegen_add_yield_from emits the opcode.h `send` layout
 * (catch, undefined, send loop, cleanup_throw, nip_catch, end_send) and
 * leaves [value].
 *
 * Errors: every codegen function returns 0 (SUCCESS) or -1 (ERROR).  On -1
 * either c->c_err holds a compile error (qpc_error: SyntaxError & co; the
 * validator normally caught them already) or a JS exception is pending
 * (out of memory).  Bytecode buffer OOM is sticky in the DynBuf and is
 * reported by qp_create_function.  No cleanup is needed on error paths:
 * qp_compile frees every unit and fd.  CPython's *_IN_SCOPE macro variants
 * are therefore the same as the plain ones.
 *
 * Warnings (_PyCompile_Warn: SyntaxWarning "is" with a literal, "'x' object
 * is not callable; perhaps you missed a comma?", ...) are emitted by
 * qp_validate (validate.c), which runs before code generation; this unit
 * does not port the warning-only checks.
 */
#ifndef QP_COMPILE_H
#define QP_COMPILE_H

#include "object.h"

#undef SUCCESS
#undef ERROR
#define SUCCESS 0
#define ERROR -1

#define RETURN_IF_ERROR(X)  \
    do {                    \
        if ((X) == -1) {    \
            return ERROR;   \
        }                   \
    } while (0)
#define RETURN_IF_ERROR_IN_SCOPE(C, CALL) RETURN_IF_ERROR(CALL)

#define COMP_GENEXP   0
#define COMP_LISTCOMP 1
#define COMP_SETCOMP  2
#define COMP_DICTCOMP 3

#ifndef CO_MAXBLOCKS
#define CO_MAXBLOCKS 21   /* Include/cpython/code.h */
#endif
#define QPC_STACK_USE_GUIDELINE 30   /* codegen.c STACK_USE_GUIDELINE */
#define QPC_MAX_ITEMS_IN_LIST 255    /* unpack_helper limit (oparg of UNPACK_EX) */

/* `location` is also a pegen.h type in this translation unit: the code
   generator's `location` is _Py_SourceLocation through a macro that
   compile.c #undefs at its end. */
typedef _Py_SourceLocation qpc_location;
#define location qpc_location

#define LOCATION(LNO, END_LNO, COL, END_COL) \
    ((const _Py_SourceLocation){(LNO), (END_LNO), (COL), (END_COL)})
#define LOC(x) SRC_LOCATION_FROM_AST(x)

/* Jump target: a QuickJS label number of the CURRENT unit's fd. */
typedef struct {
    int id;
} jump_target_label;

#define NO_LABEL ((const jump_target_label){-1})
#define SAME_LABEL(L1, L2) ((L1).id == (L2).id)
#define IS_JUMP_TARGET_LABEL(L) (!SAME_LABEL((L), (NO_LABEL)))

#define NEW_JUMP_TARGET_LABEL(C, NAME) \
    jump_target_label NAME = qpc_new_label(C); \
    if (!IS_JUMP_TARGET_LABEL(NAME)) { \
        return ERROR; \
    }

#define USE_LABEL(C, LBL) RETURN_IF_ERROR(qpc_use_label((C), (LBL)))

/* ---- frame blocks (pycore_compile.h) ------------------------------------- */
typedef enum fblocktype {
    COMPILE_FBLOCK_WHILE_LOOP,
    COMPILE_FBLOCK_FOR_LOOP,
    COMPILE_FBLOCK_ASYNC_FOR_LOOP,
    COMPILE_FBLOCK_TRY_EXCEPT,
    COMPILE_FBLOCK_FINALLY_TRY,
    COMPILE_FBLOCK_FINALLY_END,
    COMPILE_FBLOCK_WITH,
    COMPILE_FBLOCK_ASYNC_WITH,
    COMPILE_FBLOCK_HANDLER_CLEANUP,
    COMPILE_FBLOCK_POP_VALUE,
    COMPILE_FBLOCK_EXCEPTION_HANDLER,
    COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER,
    COMPILE_FBLOCK_ASYNC_COMPREHENSION_GENERATOR,
    COMPILE_FBLOCK_INLINED_COMPREHENSION,
    COMPILE_FBLOCK_STOP_ITERATION,
} fblocktype;

typedef struct fblockinfo {
    fblocktype fb_type;
    jump_target_label fb_block;
    location fb_loc;
    /* (optional) type-specific exit or cleanup block */
    jump_target_label fb_exit;
    /* (optional) additional information required for unwinding:
       FINALLY_TRY: asdl_stmt_seq * (the finally body) */
    void *fb_datum;
    /* HANDLER_CLEANUP: the `except ... as NAME` name (CPython keeps it in
       fb_datum as a str object), JS_ATOM_NULL if none */
    JSAtom fb_name;
} fblockinfo;

/* ---- scope types (pycore_compile.h) -------------------------------------- */
enum {
    COMPILE_SCOPE_MODULE,
    COMPILE_SCOPE_CLASS,
    COMPILE_SCOPE_FUNCTION,
    COMPILE_SCOPE_ASYNC_FUNCTION,
    COMPILE_SCOPE_LAMBDA,
    COMPILE_SCOPE_COMPREHENSION,
    COMPILE_SCOPE_ANNOTATIONS,
};

typedef enum {
    COMPILE_OP_FAST,
    COMPILE_OP_GLOBAL,
    COMPILE_OP_DEREF,
    COMPILE_OP_NAME,
} _PyCompile_optype;

/* Identifiers used by the code generator (atoms owned by the compiler).
   X(id) -> c->ids.id is the atom of "id"; XS(id, "text") otherwise. */
#define QPC_IDS(X, XS) \
    X(self) X(super) X(all) X(any) X(tuple) X(list) X(set) X(None) X(True)   \
    X(False) X(format) X(join) X(__debug__) X(__class__) X(__classdict__)    \
    X(__conditional_annotations__) X(__name__) X(__module__) X(__qualname__) \
    X(__doc__) X(__annotations__) X(__annotate__) X(__annotate_func__)       \
    X(__classcell__) X(__classdictcell__) X(__static_attributes__)          \
    X(__firstlineno__) X(__type_params__) X(__orig_bases__)                  \
    XS(ret, "return") XS(empty, "")                                          \
    XS(dot_type_params, ".type_params") XS(dot_generic_base, ".generic_base")\
    XS(dot_format, ".format") XS(dot_defaults, ".defaults")                  \
    XS(dot_kwdefaults, ".kwdefaults") XS(dot_locals, ".<locals>")            \
    XS(dot_0, ".0")                                                          \
    XS(anon_module, "<module>") XS(anon_lambda, "<lambda>")                  \
    XS(anon_genexpr, "<genexpr>") XS(anon_listcomp, "<listcomp>")            \
    XS(anon_setcomp, "<setcomp>") XS(anon_dictcomp, "<dictcomp>")

typedef struct qpc_ids {
#define QPC_ID_FIELD(id) JSAtom id;
#define QPC_ID_FIELD_S(id, s) JSAtom id;
    QPC_IDS(QPC_ID_FIELD, QPC_ID_FIELD_S)
#undef QPC_ID_FIELD
#undef QPC_ID_FIELD_S
} qpc_ids;

/* Temporary slot override of an inlined comprehension (see
   codegen_push_inlined_comprehension_locals in compile.c: each name bound
   by an inlined comprehension gets its own hidden local while the
   comprehension is compiled, instead of CPython's LOAD_FAST_AND_CLEAR /
   STORE_FAST save-restore of a shared slot). */
typedef struct qpc_slot_override {
    JSAtom name;        /* borrowed (symtable) */
    int var_idx;        /* index into u_fd->vars */
    int depth;          /* u_in_inlined_comp level that pushed it */
} qpc_slot_override;

/* Deferred annotation (CPython u_deferred_annotations list item) */
typedef struct qpc_deferred_annotation {
    stmt_ty st;                 /* AnnAssign */
    JSAtom mangled;             /* owned: mangled target name */
    int cond_index;             /* conditional annotation index, -1 if none */
} qpc_deferred_annotation;

/* A table of constants keyed like CPython's _PyCode_ConstantKey (same type
   and value; -0.0 != 0.0; tuples and frozensets by their items), in
   insertion order.  Entries of code objects (is_code) are keyed by their
   cpool index. */
typedef struct qpc_const_ent {
    JSValue v;          /* owned; JS_UNDEFINED for code objects */
    int aux;            /* u_consts: cpool index of a code object (-1 others);
                           u_cpool_keys: cpool index of the value */
    uint32_t hash;
    BOOL is_code;
} qpc_const_ent;

typedef struct qpc_ctab {
    qpc_const_ent *ents;
    int n, size;
    int *tab;           /* open addressing: entry index + 1, 0 = empty */
    uint32_t mask;      /* size of tab - 1 (tab NULL while empty) */
} qpc_ctab;

/* Flowgraph constant folding (compile.c "Constant folding"): one CPython
   instruction of the current basic block that can take part in a fold.
   The emitted bytecode of the records [i..npeep) starts at peep[i].pos. */
enum {
    QPC_PI_CONST,       /* LOAD_CONST v (any constant-loading instruction) */
    QPC_PI_BUILD0,      /* BUILD_LIST 0 / BUILD_SET 0 */
    QPC_PI_APPEND,      /* LIST_APPEND 1 / SET_ADD 1 after a CONST */
    QPC_PI_BUILDN,      /* BUILD_LIST n / BUILD_SET n after n CONSTs (decided by the next instruction) */
    QPC_PI_TUPLEN,      /* BUILD_TUPLE n after n CONSTs (decided by the next instruction) */
    QPC_PI_DUPCONST,    /* COPY 1 after a phase1 CONST (JUMP_IF_FALSE/TRUE operand) */
};

typedef struct qpc_peep_insn {
    uint8_t kind;       /* QPC_PI_* */
    uint8_t is_set;     /* BUILD0/APPEND/BUILDN: set display */
    uint8_t phase1;     /* CONST: visible to flowgraph's optimize_load_const
                           (loaded by codegen or folded from a TO_BOOL) */
    int n;              /* BUILDN/TUPLEN: item count */
    uint32_t pos;       /* bytecode offset of the instruction (OP_line_num included) */
    location last_loc;  /* u_last_loc before it */
    location loc;       /* its location */
    JSValue v;          /* CONST: value (owned) */
} qpc_peep_insn;
/* The jumps to a label (compiler_unit.u_lbl_jumps) */
#define QPC_LBL_JUMPS_MAX 8
#define QPC_LBL_PENDING_MAX 8
typedef struct qpc_lbl_jumps {
    int count;                  /* jumps emitted */
    int handler;                /* an exception handler (OP_catch target) */
    struct {
        uint32_t pos;           /* bytecode position of the label operand */
        location loc;           /* location of the jump (NO_LOCATION: none) */
    } j[QPC_LBL_JUMPS_MAX];     /* the first ones */
} qpc_lbl_jumps;

typedef struct compiler_unit {
    PySTEntryObject *u_ste;
    int u_scope_type;

    JSAtom u_private;           /* owned; JS_ATOM_NULL if none */
    qp_symmap u_static_attributes; /* class units: attributes assigned via self.X (set) */
    int u_has_static_attributes;

    qpc_deferred_annotation *u_deferred_annotations;
    int u_n_deferred_annotations, u_deferred_annotations_size;
    int u_has_deferred_annotations;   /* CPython: u_deferred_annotations != NULL */
    int u_has_conditional_annotation_indices;
    int u_next_conditional_annotation_index;

    JSFunctionDef *u_fd;        /* code being generated; owned by the root fd tree */
    location u_last_loc;        /* location of the last OP_line_num emitted */
    int u_last_loc_idx;         /* its locs[] index, -1 none */
    /* the no-location entry of a join (QPC_LOC_JOIN) while its block has
       no location: position of its OP_line_num operand (0 none), the
       location before it and its index (qpc_join_end) */
    uint32_t u_join_pos;
    location u_join_prev;
    int u_join_prev_idx;        /* -1: not in locs[] yet */
    int u_join_fall;            /* the join has a fallthrough predecessor */
    /* CPython propagate_line_numbers: the NO_LOCATION code after labels
       takes the location of their predecessor when they have one (the
       code before them or a jump), else none (qpc_label_entry_loc).
       u_lbl_jumps[label id]: the jumps to the label emitted so far. */
    qpc_lbl_jumps *u_lbl_jumps;
    int u_lbl_jumps_size;
    int u_lbl_pending;          /* labels placed since the last instruction */
    int u_lbl_fall;             /* the code before them falls through */
    int u_lbl_ids[QPC_LBL_PENDING_MAX]; /* (the first ones) */
    uint32_t u_lbl_start;       /* bytecode size before them */
    int u_lbl_start_lop;        /* u_fd->last_opcode_pos before them */
    int u_loc_force;            /* qpc_emit_loc emits the location even if
                                   it is the last one: a handler entry */
    int u_loc_propagated;       /* the location given to qpc_emit_loc is the
                                   one of the only predecessor (a jump):
                                   not the code's own (QP_LOC_ARTIFICIAL) */
    int u_dead;                 /* the last instruction is unreachable */
    int u_generator_like;       /* generator / coroutine / async generator: OP_return_async */

    int u_nfblocks;
    int u_in_inlined_comp;
    int u_in_conditional_block;
    fblockinfo u_fblock[CO_MAXBLOCKS];

    /* metadata (_PyCompile_CodeUnitMetadata) */
    JSAtom u_name;              /* owned */
    JSAtom u_qualname;          /* owned */
    int u_argcount, u_posonlyargcount, u_kwonlyargcount;
    int u_firstlineno;

    /* variable slots: name -> (kind << 24 | index), kind QPC_SLOT_ARG/VAR */
    qp_symmap u_slots;
    qp_symmap u_freevars;       /* name -> closure_var index */
    qpc_slot_override *u_overrides;
    int u_n_overrides, u_overrides_size;
    /* fasthidden: names temporarily fast inside an inlined comprehension of
       a non-function scope (value 1 = active) */
    qp_symmap u_fasthidden;
    /* parameters deleted somewhere in the body (`del p`, `except E as p`):
       their loads use get_arg_check */
    qp_symmap u_deleted;

    /* generator-like function bodies: handler label of the StopIteration
       wrapper (catch emitted at body start, see codegen_emit_function_body) */
    jump_target_label u_stopiter_handler;
    /* module units: CPython ANNOTATIONS_PLACEHOLDER.  The module starts with
       `goto u_ann_label; u_ann_body_label:`; the __annotate__ setup code is
       emitted after the final return at u_ann_label and jumps back. */
    jump_target_label u_ann_label, u_ann_body_label;
    int u_module_annotations_pending;

    /* non-inlined comprehensions compiled in this unit: AST node -> cpool
       index of their code.  A node compiled twice (the optimized and the
       fallback paths of all()/any()/tuple()/list()/set() of a genexp)
       reuses its code, as CPython's constant merging does. */
    struct qpc_comp_code { void *key; int idx; } *u_comp_codes;
    int u_n_comp_codes, u_comp_codes_size;

    /* CPython's u_consts: the constants codegen adds (_PyCompile_AddConst
       order), then the ones flowgraph's folding adds (u_late_consts, in
       folding order); qpc_consts_finish turns them into
       QPCodeInfo.consts. */
    qpc_ctab u_consts;
    JSValue *u_late_consts;
    int u_n_late_consts, u_late_consts_size;
    /* nested code objects: AST node (symtable key) and cpool index of each
       closure; a node compiled twice (finally bodies) gives equal code
       objects, one constant for CPython */
    struct qpc_code_key { void *key; int idx; } *u_code_keys;
    int u_n_code_keys, u_code_keys_size;
    /* cpool de-duplication (CPython merges equal constants) */
    qpc_ctab u_cpool_keys;
    /* constant folding window (QPC_PI_* records of the current basic
       block); u_peep_raw: emitting a fold result (no folding, no records) */
    qpc_peep_insn *u_peep;
    int u_npeep, u_peep_size;
    int u_peep_raw;

    struct compiler_unit *u_parent;
} compiler_unit;

#define QPC_SLOT_ARG 1
#define QPC_SLOT_VAR 2

typedef struct _PyCompiler {
    JSContext *ctx;
    const char *c_filename;
    JSAtom c_filename_atom;
    struct symtable *c_st;
    _PyFutureFeatures c_future;     /* ff_features merged with the flags */
    int c_flags;                    /* QP_PF_* | CO_FUTURE_* */
    int c_optimize;
    int c_interactive;
    qp_error *c_err;
    compiler_unit *u;               /* current unit; parents via u_parent */
    JSFunctionDef *c_root_fd;       /* module fd while compiling (freed on error) */
    int c_nunits;                   /* stack size (CPython len(c_stack) + 1) */
    GetLineColCache c_lc_cache;     /* dummy cache required by js_new_function_def */
    /* symtable key of the code unit qpc_finish_unit finished last and its fd
       (codegen_make_closure) */
    void *c_finished_key;
    JSFunctionDef *c_finished_fd;
    qpc_ids ids;
} compiler;

typedef struct {
    int n_overrides_before;     /* overrides to drop on pop */
    qp_symmap temp_symbols;     /* name -> saved outer symbol flags */
    int has_temp_symbols;
    qp_symmap fast_hidden;      /* names set fast-hidden by this comprehension */
    int has_fast_hidden;
} _PyCompile_InlinedComprehensionState;

typedef enum {
    ITERABLE_IN_LOCAL = 0,
    ITERABLE_ON_STACK = 1,
    ITERATOR_ON_STACK = 2,
} IterStackPosition;

/* Pattern matching state (codegen.c pattern_context) */
typedef struct {
    /* Names captured so far (in order): CPython `stores` list. */
    JSAtom *stores;
    Py_ssize_t n_stores, stores_size;
    /* If 0, any name captures against our subject will raise. */
    int allow_irrefutable;
    /* fail_pop[i]: jumping there pops i items off the stack */
    jump_target_label *fail_pop;
    Py_ssize_t fail_pop_size;
    /* items on top of the stack that need to *stay* on top */
    Py_ssize_t on_top;
} pattern_context;

/* ---- accessors (CPython macros of codegen.c) ------------------------------ */
#define SYMTABLE(C) ((C)->c_st)
#define SYMTABLE_ENTRY(C) ((C)->u->u_ste)
#define FUTURE_FEATURES(C) ((C)->c_future.ff_features)
#define OPTIMIZATION_LEVEL(C) ((C)->c_optimize)
#define IS_INTERACTIVE_TOP_LEVEL(C) ((C)->c_interactive && (C)->c_nunits == 1)
#define SCOPE_TYPE(C) ((C)->u->u_scope_type)
#define QUALNAME(C) ((C)->u->u_qualname)

#define VISIT(C, TYPE, V) \
    RETURN_IF_ERROR(codegen_visit_ ## TYPE((C), (V)))
#define VISIT_IN_SCOPE(C, TYPE, V) VISIT(C, TYPE, V)
#define VISIT_SEQ(C, TYPE, SEQ)                                             \
    do {                                                                    \
        asdl_ ## TYPE ## _seq *seq__ = (SEQ); /* avoid variable capture */  \
        for (int _i = 0; _i < asdl_seq_LEN(seq__); _i++) {                  \
            TYPE ## _ty elt__ = (TYPE ## _ty)asdl_seq_GET(seq__, _i);       \
            RETURN_IF_ERROR(codegen_visit_ ## TYPE((C), elt__));            \
        }                                                                   \
    } while (0)
#define VISIT_SEQ_IN_SCOPE(C, TYPE, SEQ) VISIT_SEQ(C, TYPE, SEQ)

/* ========================================================================= */
/* Emitters and compiler services (CORE: compile.c)                           */
/* ========================================================================= */

/* Labels */
static jump_target_label qpc_new_label(compiler *c);       /* NO_LABEL on OOM */
static int qpc_use_label(compiler *c, jump_target_label lbl);

/* QuickJS opcodes.  loc: emits OP_line_num first when loc.lineno >= 0 and
   differs from the last emitted location (NO_LOCATION: inherit). */
static int qpc_op(compiler *c, location loc, int opcode);
static int qpc_op_u16(compiler *c, location loc, int opcode, uint16_t v);
static int qpc_op_u32(compiler *c, location loc, int opcode, uint32_t v);
/* OP_goto / OP_if_true / OP_if_false / OP_catch with a label */
static int qpc_jump(compiler *c, location loc, int opcode, jump_target_label lbl);

/* OP_py subops */
static int qpc_py(compiler *c, location loc, int subop);                  /* fmt none */
static int qpc_py_u8(compiler *c, location loc, int subop, int v);        /* fmt u8 */
/* fmt u16 / npop / npop2 / npush / unpack_ex / loc / arg (2-byte operand) */
static int qpc_py_u16(compiler *c, location loc, int subop, int v);
static int qpc_py_atom(compiler *c, location loc, int subop, JSAtom atom);    /* fmt atom (atom duplicated) */
static int qpc_py_atom_u8(compiler *c, location loc, int subop, JSAtom atom, int v);
static int qpc_py_atom_u16(compiler *c, location loc, int subop, JSAtom atom, int v);
static int qpc_py_atom_ic(compiler *c, location loc, int subop, JSAtom atom); /* atom_u16 with a new IC index */
static int qpc_py_jump(compiler *c, location loc, int subop, jump_target_label lbl); /* fmt label */

/* Stack helpers */
static int qpc_copy(compiler *c, location loc, int n);     /* COPY n */
static int qpc_swap(compiler *c, location loc, int n);     /* SWAP n */

/* Constants */
static int qpc_load_none(compiler *c, location loc);
static int qpc_load_bool(compiler *c, location loc, int b);
static int qpc_load_small_int(compiler *c, location loc, int32_t v);
static int qpc_load_const(compiler *c, location loc, qp_const *k);
static int qpc_load_const_value(compiler *c, location loc, JSValue v);   /* consumes v */
static int qpc_load_str_atom(compiler *c, location loc, JSAtom atom);    /* str constant */
/* Python value of an AST constant (new reference; JS_EXCEPTION on OOM). */
static JSValue qpc_const_value(compiler *c, qp_const *k);
/* Add a constant to the current cpool (consumes v), returns index or -1. */
static int qpc_add_const(compiler *c, JSValue v);
/* Push a tuple constant of the str values of `n` atoms (kwnames of
   CALL_KW, fromlist of IMPORT_NAME, keys of MATCH_CLASS ...). */
static int qpc_load_atom_tuple(compiler *c, location loc, const JSAtom *atoms, int n);
#define qpc_load_kwnames qpc_load_atom_tuple

/* RETURN_VALUE */
static int qpc_return(compiler *c, location loc);

/* Exception blocks (see header comment) */
static int qpc_setup(compiler *c, location loc, jump_target_label handler); /* SETUP_* */
static int qpc_pop_block(compiler *c, location loc);        /* POP_BLOCK, marker on TOS */
static int qpc_pop_block_under(compiler *c, location loc);  /* POP_BLOCK, one value above the marker */

/* ADDOP_NAME / ADDOP_NAME_CUSTOM: mangles `name` (_PyCompile_MaybeMangle)
   and emits subop with the operand format of qp_subop_info[subop]:
   atom -> atom; atom_u16 -> IC index (getattr, load_method, setattr,
   load_global, load_from_dict_or_globals); atom_u8 -> `v`. */
static int qpc_addop_name(compiler *c, location loc, int subop, JSAtom name, int v);

/* Errors */
static int qpc_error(compiler *c, location loc, const char *fmt, ...);  /* always returns ERROR */
/* UTF-8 text of an atom for error messages (buf of QPC_ATOM_BUF bytes). */
#define QPC_ATOM_BUF 128
static const char *qpc_atom_str(compiler *c, char *buf, JSAtom atom);
static BOOL qpc_atom_is(JSAtom a, JSAtom b) ;       /* a == b */

/* Mangling (new atom references, JS_ATOM_NULL on error) */
static JSAtom _PyCompile_MaybeMangle(compiler *c, JSAtom name);

/* Frame blocks */
static int _PyCompile_PushFBlock(compiler *c, location loc, fblocktype t,
                                 jump_target_label block_label,
                                 jump_target_label exit, void *datum);
static int qpc_push_fblock_name(compiler *c, location loc, fblocktype t,
                                jump_target_label block_label,
                                jump_target_label exit, JSAtom name); /* HANDLER_CLEANUP with a name */
static void _PyCompile_PopFBlock(compiler *c, fblocktype t, jump_target_label block_label);
static fblockinfo *_PyCompile_TopFBlock(compiler *c);
static bool _PyCompile_InExceptionHandler(compiler *c);

static int _PyCompile_IsInInlinedComp(compiler *c);
static int _PyCompile_MaybeAddStaticAttributeToClass(compiler *c, expr_ty e);
static void _PyCompile_EnterConditionalBlock(compiler *c);
static void _PyCompile_LeaveConditionalBlock(compiler *c);
static int _PyCompile_AddDeferredAnnotation(compiler *c, stmt_ty s, int *conditional_annotation_index);
static int _PyCompile_GetRefType(compiler *c, JSAtom name);

/* ========================================================================= */
/* Code generation functions (CPython codegen.c names).  Owner in brackets.   */
/* ========================================================================= */

/* [core] scopes, names, functions, classes, annotations, comprehensions */
static int codegen_nameop(compiler *c, location loc, JSAtom name, expr_context_ty ctx);
static int codegen_load_classdict_freevar(compiler *c, location loc);
static int codegen_body(compiler *c, location loc, asdl_stmt_seq *stmts, bool is_module);
static int codegen_function(compiler *c, stmt_ty s, int is_async);
static int codegen_class(compiler *c, stmt_ty s);
static int codegen_typealias(compiler *c, stmt_ty s);
static int codegen_lambda(compiler *c, expr_ty e);
static int codegen_genexp(compiler *c, expr_ty e);
static int codegen_listcomp(compiler *c, expr_ty e);
static int codegen_setcomp(compiler *c, expr_ty e);
static int codegen_dictcomp(compiler *c, expr_ty e);
static int codegen_annassign(compiler *c, stmt_ty s);     /* [stmt] */
static int codegen_visit_annexpr(compiler *c, expr_ty annotation);
/* [unparse] Python/ast_unparse.c _PyAST_ExprAsUnicode: new str or JS_EXCEPTION */
static JSValue qpc_expr_as_unicode(JSContext *ctx, expr_ty e);
static int codegen_wrap_in_stopiteration_handler(compiler *c);
static int _PyCodegen_AddReturnAtEnd(compiler *c, int addNone);

/* [stmt] statements, exception blocks, with, loops, imports */
static int codegen_visit_stmt(compiler *c, stmt_ty s);
static int codegen_call_exit_with_nones(compiler *c, location loc);
static int codegen_add_yield_from(compiler *c, location loc, int await);
static int codegen_pop_except_and_reraise(compiler *c, location loc);
static int codegen_unwind_fblock(compiler *c, location *ploc, fblockinfo *info, int preserve_tos);
static int codegen_unwind_fblock_stack(compiler *c, location *ploc, int preserve_tos, fblockinfo **loop);
static int codegen_if(compiler *c, stmt_ty s);
static int codegen_for(compiler *c, stmt_ty s);
static int codegen_async_for(compiler *c, stmt_ty s);
static int codegen_while(compiler *c, stmt_ty s);
static int codegen_return(compiler *c, stmt_ty s);
static int codegen_break(compiler *c, location loc);
static int codegen_continue(compiler *c, location loc);
static int codegen_try_finally(compiler *c, stmt_ty s);
static int codegen_try_star_finally(compiler *c, stmt_ty s);
static int codegen_try_except(compiler *c, stmt_ty s);
static int codegen_try_star_except(compiler *c, stmt_ty s);
static int codegen_try(compiler *c, stmt_ty s);
static int codegen_try_star(compiler *c, stmt_ty s);
static int codegen_import_as(compiler *c, location loc, JSAtom name, JSAtom asname);
static int codegen_import(compiler *c, stmt_ty s);
static int codegen_from_import(compiler *c, stmt_ty s);
static int codegen_assert(compiler *c, stmt_ty s);
static int codegen_stmt_expr(compiler *c, location loc, expr_ty value);
static int codegen_with(compiler *c, stmt_ty s);
static int codegen_async_with(compiler *c, stmt_ty s);
static int codegen_augassign(compiler *c, stmt_ty s);
static int codegen_check_ann_expr(compiler *c, expr_ty e);
static int codegen_check_ann_subscr(compiler *c, expr_ty e);

/* [expr] expressions, calls, f-strings, comparisons */
static int codegen_visit_expr(compiler *c, expr_ty e);
static int codegen_visit_keyword(compiler *c, keyword_ty k);
static int codegen_jump_if(compiler *c, location loc, expr_ty e, jump_target_label next, int cond);
static int codegen_addcompare(compiler *c, location loc, cmpop_ty op);
static int addop_binary(compiler *c, location loc, operator_ty binop, bool inplace);
static int codegen_addop_yield(compiler *c, location loc);
static int unpack_helper(compiler *c, location loc, asdl_expr_seq *elts);
static int assignment_helper(compiler *c, location loc, asdl_expr_seq *elts);
static int codegen_call_helper(compiler *c, location loc, int n, asdl_expr_seq *args,
                               asdl_keyword_seq *keywords);
/* injected_arg: name loaded with codegen_nameop and passed as the last
   positional argument (JS_ATOM_NULL = none) */
static int codegen_call_helper_impl(compiler *c, location loc, int n, asdl_expr_seq *args,
                                    JSAtom injected_arg, asdl_keyword_seq *keywords);
static int codegen_subscript(compiler *c, expr_ty e);
static int codegen_slice(compiler *c, expr_ty s);
static int codegen_slice_two_parts(compiler *c, expr_ty s);
static bool is_constant_slice(expr_ty s);
static bool should_apply_two_element_slice_optimization(expr_ty s);
static int codegen_validate_keywords(compiler *c, asdl_keyword_seq *keywords);

/* [pattern] structural pattern matching */
static int codegen_match(compiler *c, stmt_ty s);

#define ADDOP_BINARY(C, LOC, BINOP) RETURN_IF_ERROR(addop_binary((C), (LOC), (BINOP), false))
#define ADDOP_INPLACE(C, LOC, BINOP) RETURN_IF_ERROR(addop_binary((C), (LOC), (BINOP), true))
#define ADD_YIELD_FROM(C, LOC, await) RETURN_IF_ERROR(codegen_add_yield_from((C), (LOC), (await)))
#define POP_EXCEPT_AND_RERAISE(C, LOC) RETURN_IF_ERROR(codegen_pop_except_and_reraise((C), (LOC)))
#define ADDOP_YIELD(C, LOC) RETURN_IF_ERROR(codegen_addop_yield((C), (LOC)))
#define ADDOP_COMPARE(C, LOC, CMP) RETURN_IF_ERROR(codegen_addcompare((C), (LOC), (cmpop_ty)(CMP)))

/* ========================================================================= */
/* Entry points (object.h section (b))                                        */
/* ========================================================================= */

/* Code generation for a validated module and its symbol table (CPython
   compiler_mod + _PyCompile_OptimizeAndAssemble).  `future` holds the
   merged features (ff_features |= flags).  Returns the module-level
   bytecode value; JS_EXCEPTION with err set (compile error, no JS
   exception pending) or with a pending JS exception (OOM). */
static JSValue qp_compile(JSContext *ctx, mod_ty mod, struct symtable *st,
                          _PyFutureFeatures *future, const char *filename,
                          int mode, int flags, int optimize, qp_error *err);

#endif /* QP_COMPILE_H */
