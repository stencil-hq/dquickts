/*
 * Unit-local CPython public-API names used by pegen.c and the generated
 * parser.c (Derived from CPython Include/ headers, PSF-2.0).  Included at
 * the top of pegen.c; every name is undefined again by pegen_undef.h at the
 * end of parser.c.  Most expand to code that needs a `Parser *p` in scope.
 */
#ifndef QP_PEGEN_COMPAT_H
#define QP_PEGEN_COMPAT_H

/* Error indicator */
#define PyErr_Occurred() (p->err->kind != QP_ERR_NONE)
#define PyErr_NoMemory() qp_pegen_no_memory(p)

/* Exception classes */
#define PyExc_SyntaxError QP_ERR_SYNTAX
#define PyExc_IndentationError QP_ERR_INDENTATION
#define PyExc_TabError QP_ERR_TAB
#define PyExc_IncompleteInputError QP_ERR_INCOMPLETE
#define PyExc_MemoryError QP_ERR_MEMORY
#define PyExc_ValueError QP_ERR_VALUE
#define PyExc_OverflowError QP_ERR_OVERFLOW

/* Memory: the QuickJS runtime allocator (counts against its memory limit) */
#define PyMem_Malloc(n) js_malloc_rt(p->ctx->rt, (n))
#define PyMem_Calloc(n, size) js_mallocz_rt(p->ctx->rt, (size_t)(n) * (size))
#define PyMem_Realloc(ptr, n) js_realloc_rt(p->ctx->rt, (ptr), (n))
#define PyMem_Free(ptr) js_free_rt(p->ctx->rt, (ptr))

/* C stack check of the generated rules: the QuickJS stack limit */
#define PyThreadState_Get() NULL
#define _Py_ReachedRecursionLimitWithMargin(tstate, margin) \
    js_check_stack_overflow(p->ctx->rt, 0)

/* Singletons */
#define Py_None (&qp_const_none)
#define Py_True (&qp_const_true)
#define Py_False (&qp_const_false)
#define Py_Ellipsis (&qp_const_ellipsis)

/* Token text */
#define PyBytes_AS_STRING(b) (b)

/* Start rules */
#define Py_single_input QP_INPUT_SINGLE
#define Py_file_input QP_INPUT_FILE
#define Py_eval_input QP_INPUT_EVAL
#define Py_func_type_input QP_INPUT_FUNC_TYPE

#define Py_UNREACHABLE() abort()

/* pegen.h tokenizer-mode helpers (see pegen.h) */
#define TOK_GET_MODE(tok) (&(tok->tok_mode_stack[tok->tok_mode_stack_index]))
#define TOK_GET_STRING_PREFIX(tok) (TOK_GET_MODE(tok)->string_kind == TSTRING ? 't' : 'f')

#endif /* QP_PEGEN_COMPAT_H */
