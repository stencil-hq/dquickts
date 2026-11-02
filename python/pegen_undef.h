/*
 * End of the parser unit (included at the end of the generated parser.c):
 * drop the unit-local CPython names of pegen_compat.h and the generic
 * helper macros of pegen.h so they do not leak into later python/ units.
 */
#undef QP_PEGEN_COMPAT_H

#undef PyErr_Occurred
#undef PyErr_NoMemory
#undef PyExc_SyntaxError
#undef PyExc_IndentationError
#undef PyExc_TabError
#undef PyExc_IncompleteInputError
#undef PyExc_MemoryError
#undef PyExc_ValueError
#undef PyExc_OverflowError
#undef PyMem_Malloc
#undef PyMem_Calloc
#undef PyMem_Realloc
#undef PyMem_Free
#undef PyThreadState_Get
#undef _Py_ReachedRecursionLimitWithMargin
#undef Py_None
#undef Py_True
#undef Py_False
#undef Py_Ellipsis
#undef PyBytes_AS_STRING
#undef Py_single_input
#undef Py_file_input
#undef Py_eval_input
#undef Py_func_type_input
#undef Py_UNREACHABLE

/* pegen.h helpers */
#undef PyPARSE_DONT_IMPLY_DEDENT
#undef PyPARSE_IGNORE_COOKIE
#undef PyPARSE_BARRY_AS_BDFL
#undef PyPARSE_TYPE_COMMENTS
#undef PyPARSE_ALLOW_INCOMPLETE_INPUT
#undef CURRENT_POS
#undef TOK_GET_MODE
#undef TOK_GET_STRING_PREFIX
#undef RAISE_SYNTAX_ERROR
#undef RAISE_INDENTATION_ERROR
#undef RAISE_SYNTAX_ERROR_ON_NEXT_TOKEN
#undef RAISE_SYNTAX_ERROR_KNOWN_RANGE
#undef RAISE_SYNTAX_ERROR_KNOWN_LOCATION
#undef RAISE_SYNTAX_ERROR_STARTING_FROM
#undef RAISE_SYNTAX_ERROR_INVALID_TARGET
#undef CHECK
#undef CHECK_NULL_ALLOWED
#undef CHECK_VERSION
#undef PyPegen_last_item
#undef PyPegen_first_item
#undef UNUSED
#undef EXTRA_EXPR
#undef EXTRA

/* pegen.c */
#undef QP_LONG_DEFAULT_MAX_STR_DIGITS
#undef QP_LONG_MAX_STR_DIGITS_THRESHOLD
