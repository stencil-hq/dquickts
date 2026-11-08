/*
 * dquickts C API (plan D5): the CPython internals that unit F provides
 * (capi-design.md §6 F row), compiled with the vendored modules (part of
 * the engine library, see capi/Setup.qp):
 *
 *  - the opcode metadata tables (pycore_opcode_metadata.h with
 *    NEED_OPCODE_METADATA) and PyCompile_OpcodeStackEffect[WithJump]
 *    computed from them, as Python/flowgraph.c does;
 *  - _PyCompile_GetUnaryIntrinsicName / _PyCompile_GetBinaryIntrinsicName
 *    (Python/intrinsics.c names);
 *  - _Py_GetSpecializationStats (no Py_STATS: None) and _Py_GetExecutor
 *    (no tier-2 executors);
 *  - the special-method table of pycore_ceval.h (_Py_SpecialMethods).
 *
 * tracemalloc (pycore_tracemalloc.h) is python/capi_tracemalloc.c.
 *
 * Ported from CPython 3.15.2 Python/flowgraph.c, Python/intrinsics.c,
 * Python/optimizer.c, Python/ceval.c (PSF-2.0).
 */
#ifndef Py_BUILD_CORE_MODULE
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "opcode.h"
#include "pycore_ceval.h"           // _Py_SpecialMethod
#include "pycore_compile.h"         // _PyCompile_Get*IntrinsicName()
#include "pycore_intrinsics.h"      // MAX_INTRINSIC_1
#include "pycore_optimizer.h"       // _Py_GetExecutor()
#include "pycore_stats.h"           // _Py_GetSpecializationStats()
#include "pycore_opcode_utils.h"    // IS_BLOCK_PUSH_OPCODE

#define NEED_OPCODE_METADATA
#include "pycore_opcode_metadata.h" // _PyOpcode_num_popped(), ...
#undef NEED_OPCODE_METADATA

/* ---- stack effects (Python/flowgraph.c) --------------------------------- */

/* Return the stack effect of opcode with argument oparg.

   Some opcodes have different stack effect when jump to the target and
   when not jump. The 'jump' parameter specifies the case:

   * 0 -- when not jump
   * 1 -- when jump
   * -1 -- maximal
 */
static int
get_stack_effect(int opcode, int oparg, int jump, int *net)
{
    if (opcode < 0) {
        return -1;
    }
    if ((opcode <= MAX_REAL_OPCODE) && (_PyOpcode_Deopt[opcode] != opcode)) {
        // Specialized instructions are not supported.
        return -1;
    }
    int popped = _PyOpcode_num_popped(opcode, oparg);
    int pushed = _PyOpcode_num_pushed(opcode, oparg);
    if (popped < 0 || pushed < 0) {
        return -1;
    }
    if (IS_BLOCK_PUSH_OPCODE(opcode) && !jump) {
        *net = 0;
        return 0;
    }
    *net = pushed - popped;
    return 0;
}

int
PyCompile_OpcodeStackEffectWithJump(int opcode, int oparg, int jump)
{
    int net;
    if (get_stack_effect(opcode, oparg, jump, &net) < 0) {
        return PY_INVALID_STACK_EFFECT;
    }
    return net;
}

int
PyCompile_OpcodeStackEffect(int opcode, int oparg)
{
    int net;
    if (get_stack_effect(opcode, oparg, -1, &net) < 0) {
        return PY_INVALID_STACK_EFFECT;
    }
    return net;
}

/* ---- intrinsic names (Python/intrinsics.c) ------------------------------ */

#define INTRINSIC_NAME(N) [N] = #N

static const char *const unary_intrinsic_names[MAX_INTRINSIC_1 + 1] = {
    INTRINSIC_NAME(INTRINSIC_1_INVALID),
    INTRINSIC_NAME(INTRINSIC_PRINT),
    INTRINSIC_NAME(INTRINSIC_IMPORT_STAR),
    INTRINSIC_NAME(INTRINSIC_STOPITERATION_ERROR),
    INTRINSIC_NAME(INTRINSIC_ASYNC_GEN_WRAP),
    INTRINSIC_NAME(INTRINSIC_UNARY_POSITIVE),
    INTRINSIC_NAME(INTRINSIC_LIST_TO_TUPLE),
    INTRINSIC_NAME(INTRINSIC_TYPEVAR),
    INTRINSIC_NAME(INTRINSIC_PARAMSPEC),
    INTRINSIC_NAME(INTRINSIC_TYPEVARTUPLE),
    INTRINSIC_NAME(INTRINSIC_SUBSCRIPT_GENERIC),
    INTRINSIC_NAME(INTRINSIC_TYPEALIAS),
};

static const char *const binary_intrinsic_names[MAX_INTRINSIC_2 + 1] = {
    INTRINSIC_NAME(INTRINSIC_2_INVALID),
    INTRINSIC_NAME(INTRINSIC_PREP_RERAISE_STAR),
    INTRINSIC_NAME(INTRINSIC_TYPEVAR_WITH_BOUND),
    INTRINSIC_NAME(INTRINSIC_TYPEVAR_WITH_CONSTRAINTS),
    INTRINSIC_NAME(INTRINSIC_SET_FUNCTION_TYPE_PARAMS),
    INTRINSIC_NAME(INTRINSIC_SET_TYPEPARAM_DEFAULT),
};

#undef INTRINSIC_NAME

PyObject *
_PyCompile_GetUnaryIntrinsicName(int index)
{
    if (index < 0 || index > MAX_INTRINSIC_1) {
        return NULL;
    }
    return PyUnicode_FromString(unary_intrinsic_names[index]);
}

PyObject *
_PyCompile_GetBinaryIntrinsicName(int index)
{
    if (index < 0 || index > MAX_INTRINSIC_2) {
        return NULL;
    }
    return PyUnicode_FromString(binary_intrinsic_names[index]);
}

/* ---- specialization statistics and executors ---------------------------- */

PyObject *
_Py_GetSpecializationStats(void)
{
    /* Built without Py_STATS. */
    Py_RETURN_NONE;
}

_PyExecutorObject *
_Py_GetExecutor(PyCodeObject *code, int offset)
{
    /* No tier-2 optimizer: no ENTER_EXECUTOR at any offset. */
    (void)code;
    (void)offset;
    PyErr_SetString(PyExc_ValueError, "no executor at given byte offset");
    return NULL;
}

/* ---- special methods (Python/ceval.c) ----------------------------------- */

static _Py_SpecialMethod special_methods[SPECIAL_MAX + 1] = {
    [SPECIAL___ENTER__] = {
        .name = NULL,
        .error = (
            "'%T' object does not support the context manager protocol "
            "(missed __enter__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the context manager protocol "
            "(missed __enter__ method) but it supports the asynchronous "
            "context manager protocol. Did you mean to use 'async with'?"
        )
    },
    [SPECIAL___EXIT__] = {
        .name = NULL,
        .error = (
            "'%T' object does not support the context manager protocol "
            "(missed __exit__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the context manager protocol "
            "(missed __exit__ method) but it supports the asynchronous "
            "context manager protocol. Did you mean to use 'async with'?"
        )
    },
    [SPECIAL___AENTER__] = {
        .name = NULL,
        .error = (
            "'%T' object does not support the asynchronous "
            "context manager protocol (missed __aenter__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the asynchronous context manager "
            "protocol (missed __aenter__ method) but it supports the context "
            "manager protocol. Did you mean to use 'with'?"
        )
    },
    [SPECIAL___AEXIT__] = {
        .name = NULL,
        .error = (
            "'%T' object does not support the asynchronous "
            "context manager protocol (missed __aexit__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the asynchronous context manager "
            "protocol (missed __aexit__ method) but it supports the context "
            "manager protocol. Did you mean to use 'with'?"
        )
    }
};

/* The names are immortal interned strings (_Py_ID), bound on first use
   (the C API is single-threaded in M3; the stores are idempotent). */
const _Py_SpecialMethod *
_PyQp_SpecialMethods(void)
{
    if (special_methods[SPECIAL_MAX].name == NULL) {
        special_methods[SPECIAL___ENTER__].name = &_Py_ID(__enter__);
        special_methods[SPECIAL___EXIT__].name = &_Py_ID(__exit__);
        special_methods[SPECIAL___AENTER__].name = &_Py_ID(__aenter__);
        special_methods[SPECIAL___AEXIT__].name = &_Py_ID(__aexit__);
    }
    return special_methods;
}
