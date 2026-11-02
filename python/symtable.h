/*
 * Python symbol table and __future__ feature detection.
 *
 * Derived from CPython Include/internal/pycore_symtable.h, Python/symtable.c
 * and Python/future.c (PSF-2.0), v3.15.2.
 *
 * Differences from CPython (all forced by the absence of Python objects):
 * - PySTEntryObject is a plain C struct owned by its struct symtable; it is
 *   not reference counted.  _PySymtable_Lookup/_PySymtable_LookupOptional
 *   return BORROWED pointers that stay valid until _PySymtable_Free (CPython
 *   returns new references; ported callers drop their Py_DECREF).
 * - Identifiers are JSAtoms.  ste_symbols / scope dicts are qp_symmap
 *   (insertion-ordered JSAtom -> long map); sets are qp_symmap with unused
 *   values; ste_varnames is a qp_atomlist; ste_children a qp_stelist;
 *   ste_directives a qp_directives array; ste_id is the AST key pointer itself
 *   (CPython: a PyLong of it), so "(uintptr_t)ste->ste_id + 1" keys still work.
 * - Atom ownership: atoms coming from the AST are borrowed (the arena must
 *   outlive the symtable).  Every atom the symtable creates (block names such
 *   as "<lambda>", ".format", ".0", mangled names, import store names) is owned
 *   by the symtable and released by _PySymtable_Free; such atoms appear as keys
 *   of ste_symbols/ste_varnames and stay valid until then.
 *   _Py_Mangle/_Py_MaybeMangle return a NEW atom reference the caller must
 *   JS_FreeAtom (CPython: new reference).
 * - Errors go to a qp_error.  SyntaxError locations are exactly CPython's:
 *   offset = AST col_offset + 1 (UTF-8 byte based, as PyErr_RangedSyntaxLocationObject
 *   receives it), end_offset = end_col_offset + 1.  Python exception types
 *   without a qp_error kind (KeyError "unknown symbol table entry",
 *   RuntimeError "BUG: ..."/"this compiler does not handle FunctionTypes")
 *   are reported as QP_ERR_VALUE with CPython's message.
 * - Py_EnterRecursiveCall(" during compilation") is a QuickJS stack check
 *   producing QP_ERR_RECURSION "maximum recursion depth exceeded during
 *   compilation".
 * - _PyFuture_FromAST takes the arena (for the JSContext) and the qp_error.
 *   As in CPython's compile.c, the caller merges compiler flags into
 *   ff_features (ff.ff_features |= flags) before _PySymtable_Build; the
 *   symtable tests QP_PF_TOP_LEVEL_AWAIT (= PyCF_ALLOW_TOP_LEVEL_AWAIT) there.
 * - Not ported (Python-visible object machinery only): PySTEntry_Type,
 *   ste_repr, ste_memberlist, PySTEntry_Check, and
 *   _Py_SymtableStringObjectFlags (used only by the `symtable` module).
 */
#ifndef QP_SYMTABLE_H
#define QP_SYMTABLE_H

#ifndef QP_PF_TOP_LEVEL_AWAIT
#define QP_PF_TOP_LEVEL_AWAIT 0x2000   /* = PyCF_ALLOW_TOP_LEVEL_AWAIT */
#endif

/* __future__ feature flags (Include/cpython/code.h) */
#ifndef CO_FUTURE_DIVISION
#define CO_FUTURE_DIVISION      0x20000
#endif
#ifndef CO_FUTURE_ABSOLUTE_IMPORT
#define CO_FUTURE_ABSOLUTE_IMPORT 0x40000
#endif
#ifndef CO_FUTURE_WITH_STATEMENT
#define CO_FUTURE_WITH_STATEMENT  0x80000
#endif
#ifndef CO_FUTURE_PRINT_FUNCTION
#define CO_FUTURE_PRINT_FUNCTION  0x100000
#endif
#ifndef CO_FUTURE_UNICODE_LITERALS
#define CO_FUTURE_UNICODE_LITERALS 0x200000
#endif
#ifndef CO_FUTURE_BARRY_AS_BDFL
#define CO_FUTURE_BARRY_AS_BDFL  0x400000
#endif
#ifndef CO_FUTURE_GENERATOR_STOP
#define CO_FUTURE_GENERATOR_STOP  0x800000
#endif
#ifndef CO_FUTURE_ANNOTATIONS
#define CO_FUTURE_ANNOTATIONS    0x1000000
#endif

/* Future feature names (Include/cpython/compile.h) */
#ifndef FUTURE_NESTED_SCOPES
#define FUTURE_NESTED_SCOPES "nested_scopes"
#define FUTURE_GENERATORS "generators"
#define FUTURE_DIVISION "division"
#define FUTURE_ABSOLUTE_IMPORT "absolute_import"
#define FUTURE_WITH_STATEMENT "with_statement"
#define FUTURE_PRINT_FUNCTION "print_function"
#define FUTURE_UNICODE_LITERALS "unicode_literals"
#define FUTURE_BARRY_AS_BDFL "barry_as_FLUFL"
#define FUTURE_GENERATOR_STOP "generator_stop"
#define FUTURE_ANNOTATIONS "annotations"
#endif

/* ---- containers ------------------------------------------------------- */

/* Insertion-ordered open-addressing hash map JSAtom -> long (CPython dict
   str -> int).  Also used as an insertion-ordered set of atoms (values
   ignored).  Keys are borrowed.  Deleted entries leave a hole (key ==
   JS_ATOM_NULL) in `entries` until the next rebuild, so iteration with
   qp_symmap_next stays valid while values of existing keys are updated. */
typedef struct qp_symmap_entry {
    JSAtom key;
    long value;
} qp_symmap_entry;

typedef struct qp_symmap {
    JSRuntime *rt;
    qp_symmap_entry *entries;   /* insertion order, holes have key JS_ATOM_NULL */
    int32_t *index;             /* hash slots: -1 empty, -2 deleted, else entry index */
    uint32_t index_size;        /* power of two, 0 when nothing allocated */
    uint32_t n_entries;         /* used slots of entries[] (including holes) */
    uint32_t size;              /* number of live keys */
} qp_symmap;

static void qp_symmap_init(qp_symmap *m, JSRuntime *rt);
static void qp_symmap_free(qp_symmap *m);
static __maybe_unused Py_ssize_t qp_symmap_size(const qp_symmap *m);
/* 1 and *pval set if present, else 0 */
static int qp_symmap_lookup(const qp_symmap *m, JSAtom key, long *pval);
static int qp_symmap_contains(const qp_symmap *m, JSAtom key);
/* insert or update; 0 or -1 on OOM */
static int qp_symmap_set(qp_symmap *m, JSAtom key, long value);
/* 1 if removed, 0 if absent */
static int qp_symmap_del(qp_symmap *m, JSAtom key);
/* PyDict_Next: *pos starts at 0; returns 0 when exhausted */
static int qp_symmap_next(const qp_symmap *m, Py_ssize_t *pos, JSAtom *key, long *value);
/* dict.update / set |= : copy every (key, value) of src into dst; 0 or -1 */
static int qp_symmap_update(qp_symmap *dst, const qp_symmap *src);

/* Growable array of atoms (CPython list of str).  Atoms are borrowed. */
typedef struct qp_atomlist {
    JSAtom *items;
    Py_ssize_t len, cap;
} qp_atomlist;

struct _symtable_entry;

/* Growable array of symbol table entries (CPython list of PySTEntryObject). */
typedef struct qp_stelist {
    struct _symtable_entry **items;
    Py_ssize_t len, cap;
} qp_stelist;

/* source location information */
typedef struct {
    int lineno;
    int end_lineno;
    int col_offset;
    int end_col_offset;
} _Py_SourceLocation;

#define SRC_LOCATION_FROM_AST(n) \
    (_Py_SourceLocation){ \
               .lineno = (n)->lineno, \
               .end_lineno = (n)->end_lineno, \
               .col_offset = (n)->col_offset, \
               .end_col_offset = (n)->end_col_offset }

static const _Py_SourceLocation NO_LOCATION __maybe_unused = {-1, -1, -1, -1};
static const _Py_SourceLocation NEXT_LOCATION __maybe_unused = {-2, -2, -2, -2};

/* One global/nonlocal directive (CPython tuple (name, lineno, col, end_lineno, end_col)) */
typedef struct qp_directive {
    JSAtom name;                /* mangled name, owned by the symtable or AST */
    _Py_SourceLocation loc;
} qp_directive;

typedef struct qp_directives {
    qp_directive *items;
    Py_ssize_t len, cap;
} qp_directives;

/* Map AST node address (or address + 1) -> entry (CPython st_blocks dict). */
typedef struct qp_ptrmap_entry {
    void *key;
    struct _symtable_entry *value;
} qp_ptrmap_entry;

typedef struct qp_ptrmap {
    qp_ptrmap_entry *slots;     /* key NULL = empty */
    uint32_t mask;              /* slots - 1, slots is a power of two */
    uint32_t count;
} qp_ptrmap;

/* ---- symbol table ----------------------------------------------------- */

typedef enum _block_type {
    FunctionBlock, ClassBlock, ModuleBlock,
    // Used for annotations. If 'from __future__ import annotations' is active,
    // annotation blocks cannot bind names and are not evaluated. Otherwise, they
    // are lazily evaluated (see PEP 649).
    AnnotationBlock,

    // The following blocks are used for generics and type aliases. These work
    // mostly like functions (see PEP 695 for details). The three different
    // blocks function identically; they are different enum entries only so
    // that error messages can be more precise.

    // The block to enter when processing a "type" (PEP 695) construction,
    // e.g., "type MyGeneric[T] = list[T]".
    TypeAliasBlock,
    // The block to enter when processing a "generic" (PEP 695) object,
    // e.g., "def foo[T](): pass" or "class A[T]: pass".
    TypeParametersBlock,
    // The block to enter when processing the bound, the constraint tuple
    // or the default value of a single "type variable" in the formal sense,
    // i.e., a TypeVar, a TypeVarTuple or a ParamSpec object (the latter two
    // do not support a bound or a constraint tuple).
    TypeVariableBlock,
} _Py_block_ty;

typedef enum _comprehension_type {
    NoComprehension = 0,
    ListComprehension = 1,
    DictComprehension = 2,
    SetComprehension = 3,
    GeneratorExpression = 4 } _Py_comprehension_ty;

/* __future__ information */
typedef struct {
    int ff_features;                    /* flags set by future statements */
    _Py_SourceLocation ff_location;     /* location of last future statement */
} _PyFutureFeatures;

/* Atoms created and owned by the symtable (CPython _Py_ID/_Py_STR strings). */
typedef struct qp_st_ids {
    JSAtom top;                         /* "top" */
    JSAtom dot_format;                  /* ".format" */
    JSAtom dot_type_params;             /* ".type_params" */
    JSAtom dot_generic_base;            /* ".generic_base" */
    JSAtom dot_defaults;                /* ".defaults" */
    JSAtom dot_kwdefaults;              /* ".kwdefaults" */
    JSAtom dunder_class;                /* "__class__" */
    JSAtom dunder_classdict;            /* "__classdict__" */
    JSAtom dunder_conditional_annotations; /* "__conditional_annotations__" */
    JSAtom dunder_type_params;          /* "__type_params__" */
    JSAtom dunder_annotate;             /* "__annotate__" */
    JSAtom anon_lambda;                 /* "<lambda>" */
    JSAtom anon_genexpr;                /* "<genexpr>" */
    JSAtom anon_listcomp;               /* "<listcomp>" */
    JSAtom anon_setcomp;                /* "<setcomp>" */
    JSAtom anon_dictcomp;               /* "<dictcomp>" */
} qp_st_ids;

struct symtable {
    const char *st_filename;        /* name of file being compiled (borrowed) */
    struct _symtable_entry *st_cur; /* current symbol table entry */
    struct _symtable_entry *st_top; /* symbol table entry for module */
    qp_ptrmap st_blocks;            /* map AST node addresses to symbol table entries */
    qp_stelist st_stack;            /* stack of namespace info */
    qp_symmap *st_global;           /* borrowed: &st_top->ste_symbols */
    int st_nblocks;                 /* number of blocks used. kept for
                                       consistency with the corresponding
                                       compiler structure */
    JSAtom st_private;              /* name of current class or JS_ATOM_NULL */
    _PyFutureFeatures *st_future;   /* module's future features that affect
                                       the symbol table */
    /* qp additions */
    JSContext *st_ctx;
    qp_arena *st_arena;
    qp_error *st_err;
    qp_stelist st_entries;          /* owns every entry created */
    qp_symmap st_owned_atoms;       /* set of atoms the symtable holds one reference to */
    qp_symmap **st_sets;            /* owned ste_mangled_names sets */
    Py_ssize_t st_nsets, st_sets_cap;
    qp_st_ids st_ids;
};

typedef struct _symtable_entry {
    void *ste_id;            /* key in ste_table->st_blocks (AST node address) */
    qp_symmap ste_symbols;   /* variable names to flags */
    JSAtom ste_name;         /* name of current block */
    JSAtom ste_function_name;  /* JS_ATOM_NULL or, for annotation blocks: name of the corresponding functions */
    qp_atomlist ste_varnames;  /* list of function parameters */
    qp_stelist ste_children;   /* list of child blocks */
    qp_directives ste_directives; /* locations of global and nonlocal statements */
    qp_symmap *ste_mangled_names; /* NULL or set of names for which mangling should be applied (shared) */

    _Py_block_ty ste_type;
    // Optional string set by symtable.c and used when reporting errors.
    // The content of that string is a description of the current "context".
    //
    // For instance, if we are processing the default value of the type
    // variable "T" in "def foo[T = int](): pass", `ste_scope_info` is
    // set to "a TypeVar default".
    const char *ste_scope_info;

    int ste_nested;      /* true if block is nested */
    unsigned ste_generator : 1;   /* true if namespace is a generator */
    unsigned ste_coroutine : 1;   /* true if namespace is a coroutine */
    unsigned ste_annotations_used : 1;  /* true if there are any annotations in this scope */
    _Py_comprehension_ty ste_comprehension;  /* Kind of comprehension (if any) */
    unsigned ste_varargs : 1;     /* true if block has varargs */
    unsigned ste_varkeywords : 1; /* true if block has varkeywords */
    unsigned ste_returns_value : 1;  /* true if namespace uses return with
                                        an argument */
    unsigned ste_needs_class_closure : 1; /* for class scopes, true if a
                                             closure over __class__
                                             should be created */
    unsigned ste_needs_classdict : 1; /* for class scopes, true if a closure
                                         over the class dict should be created */
    unsigned ste_comp_inlined : 1; /* true if this comprehension is inlined */
    unsigned ste_comp_iter_target : 1; /* true if visiting comprehension target */
    unsigned ste_can_see_class_scope : 1; /* true if this block can see names bound in an
                                             enclosing class scope */
    unsigned ste_has_docstring : 1; /* true if docstring present */
    unsigned ste_method : 1; /* true if block is a function block defined in class scope */
    unsigned ste_has_conditional_annotations : 1; /* true if block has conditionally executed annotations */
    unsigned ste_in_conditional_block : 1; /* set while we are inside a conditionally executed block */
    unsigned ste_in_try_block : 1; /* set while we are inside a try/except block */
    unsigned ste_in_unevaluated_annotation : 1; /* set while we are processing an annotation that will not be evaluated */
    int ste_comp_iter_expr; /* non-zero if visiting a comprehension range expression */
    _Py_SourceLocation ste_loc; /* source location of block */
    struct _symtable_entry *ste_annotation_block; /* symbol table entry for this entry's annotations */
    struct symtable *ste_table;
} PySTEntryObject;

/* Flags for def-use information */

#define DEF_GLOBAL 1             /* global stmt */
#define DEF_LOCAL 2              /* assignment in code block */
#define DEF_PARAM (2<<1)         /* formal parameter */
#define DEF_NONLOCAL (2<<2)      /* nonlocal stmt */
#define USE (2<<3)               /* name is used */
#define DEF_FREE_CLASS (2<<5)    /* free variable from class's method */
#define DEF_IMPORT (2<<6)        /* assignment occurred via import */
#define DEF_ANNOT (2<<7)         /* this name is annotated */
#define DEF_COMP_ITER (2<<8)     /* this name is a comprehension iteration variable */
#define DEF_TYPE_PARAM (2<<9)    /* this name is a type parameter */
#define DEF_COMP_CELL (2<<10)    /* this name is a cell in an inlined comprehension */

#define DEF_BOUND (DEF_LOCAL | DEF_PARAM | DEF_IMPORT)

/* GLOBAL_EXPLICIT and GLOBAL_IMPLICIT are used internally by the symbol
   table.  GLOBAL is returned from _PyST_GetScope() for either of them.
   It is stored in ste_symbols at bits 13-16.
*/
#define SCOPE_OFFSET 12
#define SCOPE_MASK (DEF_GLOBAL | DEF_LOCAL | DEF_PARAM | DEF_NONLOCAL)
#define SYMBOL_TO_SCOPE(S) (((S) >> SCOPE_OFFSET) & SCOPE_MASK)

#define LOCAL 1
#define GLOBAL_EXPLICIT 2
#define GLOBAL_IMPLICIT 3
#define FREE 4
#define CELL 5

/* Flags of `name` in `ste` (0 if absent).  Never fails. */
static __maybe_unused long _PyST_GetSymbol(PySTEntryObject *ste, JSAtom name);
/* Scope (LOCAL, GLOBAL_EXPLICIT, ...) of `name` in `ste`, 0 if absent. */
static __maybe_unused int _PyST_GetScope(PySTEntryObject *ste, JSAtom name);
static __maybe_unused int _PyST_IsFunctionLike(PySTEntryObject *ste);

/* Build the symbol table of `mod`.  `future` must stay valid while the
   symtable lives.  `arena` (which owns `mod`) must outlive the result.
   Returns NULL with `err` set on failure. */
static __maybe_unused struct symtable *_PySymtable_Build(mod_ty mod, const char *filename,
                                          _PyFutureFeatures *future,
                                          qp_arena *arena, qp_error *err);
/* Entry for AST key, borrowed; NULL with QP_ERR_VALUE
   "unknown symbol table entry" (CPython KeyError) if absent. */
static __maybe_unused PySTEntryObject *_PySymtable_Lookup(struct symtable *st, void *key);
/* 1 and *out set if found, 0 and *out = NULL if not (never -1). */
static __maybe_unused int _PySymtable_LookupOptional(struct symtable *st, void *key,
                                      PySTEntryObject **out);
/* Free the symtable, its entries and every atom it owns.  NULL is allowed. */
static __maybe_unused void _PySymtable_Free(struct symtable *st);

/* Mangling (new atom reference or JS_ATOM_NULL + err on OOM/overflow). */
static __maybe_unused JSAtom _Py_MaybeMangle(JSAtom privateobj, PySTEntryObject *ste, JSAtom name,
                              JSContext *ctx, qp_error *err);
static __maybe_unused JSAtom _Py_Mangle(JSAtom privateobj, JSAtom ident,
                         JSContext *ctx, qp_error *err);
static __maybe_unused int _Py_IsPrivateName(JSContext *ctx, JSAtom ident);

/* Collect __future__ features of `mod` into `ff` (1 ok, 0 error with `err`
   set: "not a chance", "future feature X is not defined").  Does not merge
   compiler flags (callers do, like compile.c). */
static __maybe_unused int _PyFuture_FromAST(mod_ty mod, const char *filename,
                             _PyFutureFeatures *ff, qp_arena *arena, qp_error *err);

#endif /* QP_SYMTABLE_H */
