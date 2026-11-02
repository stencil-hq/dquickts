/*
 * Python subopcodes of the QuickJS OP_py prefix opcode (milestone M2).
 *
 * Operand numbering (NB_*, Py_LT..Py_GE, INTRINSIC_*, CONSTANT_*,
 * MAKE_FUNCTION_*, FVC_*, SPECIAL_*, GET_ITER_YIELD_FROM_*) is derived from
 * CPython 3.15 Include/opcode.h, Include/ceval.h,
 * Include/internal/pycore_intrinsics.h, pycore_opcode_utils.h and
 * pycore_ceval.h (PSF-2.0).  Stack effects follow cpython/Python/bytecodes.c.
 *
 * Encoding
 * --------
 * A Python operation is encoded as
 *
 *     OP_py <u8 subop> <operands...>
 *
 * OP_py is one QuickJS opcode (DEF(py, 2, 0, 0, u8) in quickjs-opcode.h,
 * placed just before OP_nop).  Every bytecode walker that meets OP_py reads
 * the subop byte at pos + 1 and takes size / n_pop / n_push / fmt from
 * qp_subop_info[subop] instead of opcode_info[OP_py].  Operands start at
 * pos + 2 and have exactly the layout of the QuickJS operand format named in
 * the fmt column (OP_FMT_*), plus the three PY-only formats below.  PYDEF
 * sizes INCLUDE the OP_py byte and the subop byte.
 *
 * Stack notation: [a, b] -> [c] lists the top of the stack, rightmost = TOS.
 * NULL is JS_UNINITIALIZED (CPython's NULL stack entry; see QP_NULL in
 * object.h).  M is a QuickJS catch-offset marker pushed by OP_catch.
 * "None" is JS_UNDEFINED.
 *
 * Operand formats and how every walker treats them (operand offset = pos+2)
 * -------------------------------------------------------------------------
 *  none       no operand.
 *  u8         1 byte (data).
 *  u16        2 bytes (data, byte-swapped by bc_byte_swap).
 *  npop       u16 count; actual n_pop = PYDEF n_pop + count.
 *  label      i32; target = (pos + 2) + value after resolve_labels.  Before
 *             resolve_labels it holds a label number (label_slots index) and
 *             is registered/relocated exactly like the label of OP_goto
 *             (update_label ref counts, RelocEntry, code_has_label,
 *             find_jump_target, skip_dead_code).  Whether the label is a
 *             branch target is given by the PYFLOW table.
 *  atom       u32 atom: owned by the bytecode (free_bytecode_atoms frees it,
 *             the bytecode writer/reader remap it, the dumper prints it).
 *  atom_u8    u32 atom + u8 data.
 *  atom_u16   u32 atom + u16 data (an inline-cache index unless the subop
 *             comment says otherwise; the writer stores the number, the
 *             reader keeps it; caches are always restored empty).
 *  loc        u16 local variable index (vardefs[arg_count + idx]); dumper
 *             prints the name; no other walker rewrites it (Python code
 *             never goes through resolve_variables).
 *  arg        u16 argument index (vardefs[idx]); same treatment as loc.
 *  PY-only formats (enumerators OP_FMT_npop2 = 64.. so that they never
 *  collide with OPCodeFormat; generated from PYFMT below):
 *  npop2      u16 count; actual n_pop = PYDEF n_pop + 2 * count.
 *  npush      u16 count; actual n_push = PYDEF n_push + count.
 *  unpack_ex  u16 v (before = v & 0xff, after = v >> 8); actual
 *             n_push = PYDEF n_push + before + after.
 *  All three are plain 2-byte data for every walker except
 *  compute_stack_size (pop/push arithmetic above) and the dumper.
 *
 * Depth checks: copy/swap/list_append/... read below the popped values.
 * compute_stack_size must check stack_len >= the depth they name and raise
 * "stack underflow" otherwise.  No subop pops a catch marker: markers are
 * only removed by drop / nip / nip1 / nip_catch / exception unwinding, so
 * the catch_pos bookkeeping of compute_stack_size is unchanged.
 *
 * Reused QuickJS opcodes (value-type independent, see doc m2-design.md):
 * push_i32/push_0..7/push_minus1, push_const, push_atom_value, undefined,
 * push_true, push_false, fclosure, drop, nip, dup, swap, rot*, get_loc,
 * put_loc, set_loc, get_loc_check, set_loc_uninitialized, get_arg, put_arg,
 * get_var_ref, put_var_ref, get_var_ref_check, close_loc, goto, if_false,
 * if_true (only after a subop that pushes an exact bool), catch, nip_catch,
 * call/call0..3 (positional calls without a self slot), array_from (list
 * display), return, return_async (return from generators/coroutines),
 * strict_eq is NOT reused for `is` (see is_op), is_undefined_or_null
 * (x is None / POP_JUMP_IF_NONE), lnot (after to_bool), line_num (operand =
 * index into the code's location table, see QPCodeInfo in object.h).
 */

#if defined(PYFMT) || defined(PYDEF) || defined(PYFLOW)

#ifdef PYFMT
PYFMT(npop2)
PYFMT(npush)
PYFMT(unpack_ex)
#undef PYFMT
#endif /* PYFMT */

#ifdef PYDEF
/*     id                     size pop push fmt */
PYDEF(invalid,                   2, 0, 0, none)     /* never emitted: raises InternalError */

/* ---- stack and constants ---------------------------------------------- */
/* [] -> [NULL]                                          PUSH_NULL */
PYDEF(push_null,                 2, 0, 1, none)
/* COPY n (n >= 2; COPY 1 is QuickJS dup): [x, ...(n-1 items)] -> [x, ..., x] */
PYDEF(copy,                      3, 0, 1, u8)
/* SWAP n (n >= 3; SWAP 2 is QuickJS swap): exchange TOS and the n-th item */
PYDEF(swap,                      3, 0, 0, u8)
/* [] -> [v]   LOAD_COMMON_CONSTANT; u8 = QP_CONST_* (CPython CONSTANT_*).
   The builtin objects (AssertionError, tuple, all, ...) are the real
   builtins, never looked up by name. */
PYDEF(load_common_const,         3, 0, 1, u8)

/* ---- operators -------------------------------------------------------- */
/* [a, b] -> [a op b]   BINARY_OP (u8 = QP_NB_*, never QP_NB_SUBSCR).
   In-place codes try __iop__ first like PyNumber_InPlace*. */
PYDEF(binop,                     3, 2, 1, u8)
/* [a] -> [op a]   UNARY_NEGATIVE, UNARY_INVERT, CALL_INTRINSIC_1
   INTRINSIC_UNARY_POSITIVE (u8 = QP_UNOP_*) */
PYDEF(unop,                      3, 1, 1, u8)
/* [a, b] -> [res]   COMPARE_OP; u8 = QP_CMP_LT..QP_CMP_GE, optionally
   | QP_CMP_TO_BOOL (CPython oparg bit 4: result converted to exact bool) */
PYDEF(compare,                   3, 2, 1, u8)
/* [a, b] -> [bool]   IS_OP (u8 = invert).  Python identity: same object
   for objects, bit-identical float64, equal ints, equal strs, None and JS
   null are identical (D3). */
PYDEF(is_op,                     3, 2, 1, u8)
/* [a, b] -> [bool]   CONTAINS_OP: `a in b` (u8 = invert) */
PYDEF(contains,                  3, 2, 1, u8)
/* [v] -> [bool]   TO_BOOL (exact JS_TAG_BOOL result) */
PYDEF(to_bool,                   2, 1, 1, none)
/* [v] -> []   TO_BOOL + POP_JUMP_IF_FALSE fused: jump if not truthy */
PYDEF(jump_if_false,             6, 1, 0, label)
/* [v] -> []   TO_BOOL + POP_JUMP_IF_TRUE fused: jump if truthy */
PYDEF(jump_if_true,              6, 1, 0, label)

/* ---- subscripts and slices -------------------------------------------- */
/* [c, k] -> [c[k]]   BINARY_OP NB_SUBSCR */
PYDEF(getitem,                   2, 2, 1, none)
/* [v, c, k] -> []   STORE_SUBSCR: c[k] = v */
PYDEF(setitem,                   2, 3, 0, none)
/* [c, k] -> []   DELETE_SUBSCR */
PYDEF(delitem,                   2, 2, 0, none)
/* [c, start, stop] -> [c[start:stop]]   BINARY_SLICE */
PYDEF(getslice,                  2, 3, 1, none)
/* [v, c, start, stop] -> []   STORE_SLICE */
PYDEF(setslice,                  2, 4, 0, none)
/* [start, stop(, step)] -> [slice]   BUILD_SLICE (count = 2 or 3) */
PYDEF(build_slice,               4, 0, 1, npop)

/* ---- attributes ------------------------------------------------------- */
/* [o] -> [o.name]   LOAD_ATTR (oparg & 1 == 0); u16 = attribute IC */
PYDEF(getattr,                   8, 1, 1, atom_u16)
/* [o] -> [meth, self] or [attr, NULL]   LOAD_ATTR method form (oparg & 1):
   an unbound function / method descriptor found on the type is pushed
   with self (no bound-method allocation); u16 = attribute IC */
PYDEF(load_method,               8, 1, 2, atom_u16)
/* [v, o] -> []   STORE_ATTR: o.name = v; u16 = attribute IC */
PYDEF(setattr,                   8, 2, 0, atom_u16)
/* [o] -> []   DELETE_ATTR */
PYDEF(delattr,                   6, 1, 0, atom)
/* [global_super, cls, self] -> [attr]   LOAD_SUPER_ATTR attribute form.
   u8 = QP_SUPER_TWO_ARGS if the source call had explicit arguments.  If
   global_super is not the builtin super it is called normally first. */
PYDEF(load_super_attr,           7, 3, 1, atom_u8)
/* [global_super, cls, self] -> [meth, self_or_NULL]   LOAD_SUPER_ATTR
   method form (oparg & 1); u8 as above */
PYDEF(load_super_method,         7, 3, 2, atom_u8)
/* [o] -> [meth, self]   LOAD_SPECIAL (u8 = QP_SPECIAL_*).  Raises the
   CPython 3.15 TypeError "'%T' object does not support the (asynchronous)
   context manager protocol (missed __exit__ method)" etc. */
PYDEF(load_special,              3, 1, 2, u8)

/* ---- names ------------------------------------------------------------ */
/* [] -> [v]   LOAD_GLOBAL (the NULL-push bit is never used; calls use
   OP_call or push_null).  Globals/builtins come from the running
   function's QPFuncData; u16 = global IC.  Reifies lazy-import objects. */
PYDEF(load_global,               8, 0, 1, atom_u16)
/* [v] -> []   STORE_GLOBAL */
PYDEF(store_global,              6, 1, 0, atom)
/* [] -> []   DELETE_GLOBAL (NameError if absent) */
PYDEF(delete_global,             6, 0, 0, atom)
/* [] -> [v]   LOAD_NAME: locals mapping (hidden argument 0 of code without
   CO_NEWLOCALS), then globals, then builtins */
PYDEF(load_name,                 6, 0, 1, atom)
/* [v] -> []   STORE_NAME into the locals mapping */
PYDEF(store_name,                6, 1, 0, atom)
/* [] -> []   DELETE_NAME from the locals mapping */
PYDEF(delete_name,               6, 0, 0, atom)
/* [] -> [mapping]   LOAD_LOCALS (hidden argument 0) */
PYDEF(load_locals,               2, 0, 1, none)
/* [mapping] -> [v]   LOAD_FROM_DICT_OR_GLOBALS; u16 = global IC */
PYDEF(load_from_dict_or_globals, 8, 1, 1, atom_u16)
/* [mapping] -> [v]   LOAD_FROM_DICT_OR_DEREF.  atom = variable name;
   u16 = cell: (idx | QP_DEREF_FREE) = closure variable var_refs[idx],
   idx without the bit = captured local variable idx.  NOT an IC index. */
PYDEF(load_from_dict_or_deref,   8, 1, 1, atom_u16)
/* [] -> [v]   LOAD_FAST(_CHECK) of a parameter that may be unbound (the
   function deletes it): UnboundLocalError like get_loc_check */
PYDEF(get_arg_check,             4, 0, 1, arg)
/* [] -> [cell]   LOAD_CLOSURE used to build __classcell__ /
   __classdictcell__: PY_CELL wrapping the JSVarRef of captured local idx
   (created with get_var_ref() if the frame has none yet) */
PYDEF(make_cell,                 4, 0, 1, loc)

/* ---- calls ------------------------------------------------------------ */
/* [callable, self_or_NULL, args...] -> [res]   CALL (count = argc).  With
   self present the call is callable(self, *args) using the stack slots
   in place (args start at the self slot).  Supports JS_RequestSuspend()
   like OP_call (pending_py_subop). */
PYDEF(call_method,               4, 2, 1, npop)
/* [callable, self_or_NULL, args..., kwvalues..., kwnames] -> [res]
   CALL_KW (count = positional + keyword values; kwnames = tuple of str
   from the constant pool).  Vectorcall: kwnames travels in the new_target
   slot with JS_CALL_FLAG_PY_KW. */
PYDEF(call_kw,                   4, 3, 1, npop)
/* [callable, self_or_NULL, args_tuple, kwargs_or_NULL] -> [res]
   CALL_FUNCTION_EX (args converted to a tuple if needed, kwargs must be a
   mapping: CPython's messages for both) */
PYDEF(call_ex,                   2, 4, 1, none)

/* ---- building --------------------------------------------------------- */
/* [x1..xn] -> [tuple]   BUILD_TUPLE (list displays use OP_array_from) */
PYDEF(build_tuple,               4, 0, 1, npop)
/* [x1..xn] -> [set]   BUILD_SET */
PYDEF(build_set,                 4, 0, 1, npop)
/* [k1, v1, .., kn, vn] -> [dict]   BUILD_MAP */
PYDEF(build_map,                 4, 0, 1, npop2)
/* [s1..sn] -> [str]   BUILD_STRING (all items are exact str) */
PYDEF(build_string,              4, 0, 1, npop)
/* [list, ...(i-1), v] -> [list, ...]   LIST_APPEND i (u16 = i) */
PYDEF(list_append,               4, 1, 0, u16)
/* [list, ...(i-1), iterable] -> [list, ...]   LIST_EXTEND i */
PYDEF(list_extend,               4, 1, 0, u16)
/* [set, ...(i-1), v] -> [set, ...]   SET_ADD i */
PYDEF(set_add,                   4, 1, 0, u16)
/* [set, ...(i-1), iterable] -> [set, ...]   SET_UPDATE i */
PYDEF(set_update,                4, 1, 0, u16)
/* [dict, ...(i-1), k, v] -> [dict, ...]   MAP_ADD i */
PYDEF(map_add,                   4, 2, 0, u16)
/* [dict, ...(i-1), mapping] -> [dict, ...]   DICT_UPDATE i ("'%T' object
   is not a mapping") */
PYDEF(dict_update,               4, 1, 0, u16)
/* [callable, x, y, dict, ...(i-1), mapping] -> [callable, x, y, dict, ...]
   DICT_MERGE i (** arguments; duplicate keys raise "%s got multiple
   values for keyword argument '%U'" naming callable, which is 3 slots
   below dict exactly as in CPython) */
PYDEF(dict_merge,                4, 1, 0, u16)
/* [seq] -> [x(n-1), ..., x1, x0]   UNPACK_SEQUENCE n (x0 on top) */
PYDEF(unpack_seq,                4, 1, 0, npush)
/* [seq] -> [after items (last deepest)..., rest_list, before items...]
   UNPACK_EX (before = v & 0xff, after = v >> 8; item 0 on top) */
PYDEF(unpack_ex,                 4, 1, 1, unpack_ex)

/* ---- f-strings and t-strings ------------------------------------------ */
/* [v] -> [str]   CONVERT_VALUE (u8 = QP_FVC_STR / _REPR / _ASCII) */
PYDEF(convert_value,             3, 1, 1, u8)
/* [v] -> [str]   FORMAT_SIMPLE (format(v, '') unless v is an exact str) */
PYDEF(format_simple,             2, 1, 1, none)
/* [v, spec] -> [str]   FORMAT_WITH_SPEC */
PYDEF(format_with_spec,          2, 2, 1, none)
/* [value, expr_str, format_spec] -> [Interpolation]   BUILD_INTERPOLATION.
   Codegen always pushes format_spec ('' when absent); u8 = conversion
   (QP_FVC_NONE or QP_FVC_STR/REPR/ASCII) */
PYDEF(build_interpolation,       3, 3, 1, u8)
/* [strings_tuple, interpolations_tuple] -> [Template]   BUILD_TEMPLATE */
PYDEF(build_template,            2, 2, 1, none)

/* ---- iteration -------------------------------------------------------- */
/* [obj] -> [iter, idx_or_NULL]   GET_ITER (u8 = QP_GETITER_*).  For types
   with tp_iteritem (list, tuple, range, str, bytes, bytearray) and
   QP_GETITER_NORMAL, iter = obj itself and idx = JS int 0 (CPython 3.15
   virtual iterators: no iterator allocation); otherwise iter = iter(obj)
   and idx = NULL.  QP_GETITER_YIELD_FROM* add the yield-from checks of
   _PyEval_GetIter (coroutine reuse / "cannot 'yield from' a coroutine
   object in a non-coroutine generator"). */
PYDEF(get_iter,                  3, 1, 2, u8)
/* [iter, idx] -> [iter, idx', next]; exhausted: jump to label with
   [iter, idx] (FOR_ITER; the END_FOR it skips is not emitted; POP_ITER
   after the loop is two drops) */
PYDEF(for_iter,                  6, 2, 3, label)
/* [recv, idx, M, v] -> [recv, idx, M, yielded]; when recv returns: jump to
   label with [recv, idx, M, retval]   SEND.  Virtual iteration when idx is
   an int and v is None; generators/coroutines are resumed directly; else
   recv.send(v) / next(recv).  Layout of the delegation loop emitted by
   codegen for `yield from` / `await` / async-with/for awaits:
       <recv, idx>  catch FAIL  undefined
   L:  py send EXIT ; py yield_value QP_YIELD_DELEGATE ; goto L
   FAIL: py cleanup_throw ; goto JOIN
   EXIT: nip_catch
   JOIN: py end_send */
PYDEF(send,                      6, 1, 1, label)
/* [v] -> [sent]   YIELD_VALUE (+ RESUME).  Suspends the generator frame:
   returns FUNC_RET_YIELD (u8 = QP_YIELD_PLAIN) or FUNC_RET_YIELD_STAR
   (u8 = QP_YIELD_DELEGATE: suspended inside a send loop, the delegate
   (gi_yieldfrom) is cur_sp[-4] of the suspended frame).  The resumer
   replaces cur_sp[-1] by the sent value (nothing else is pushed), or sets
   throw_flag to raise at this point. */
PYDEF(yield_value,               3, 1, 1, u8)
/* [recv, idx, v] -> [v]   END_SEND */
PYDEF(end_send,                  2, 3, 1, none)
/* [recv, idx, exc] -> [recv, idx, value]   CLEANUP_THROW: StopIteration
   gives its value, any other exception is re-raised (no new traceback
   entry) */
PYDEF(cleanup_throw,             2, 3, 3, none)
/* [obj] -> [awaitable]   GET_AWAITABLE (u8 = QP_AWAITABLE_*: where the
   await comes from, for the error messages) */
PYDEF(get_awaitable,             3, 1, 1, u8)
/* [obj] -> [aiter]   GET_AITER */
PYDEF(get_aiter,                 2, 1, 1, none)
/* [aiter] -> [aiter, awaitable]   GET_ANEXT */
PYDEF(get_anext,                 2, 1, 2, none)
/* [aiter, exc] -> []   END_ASYNC_FOR: StopAsyncIteration ends the loop,
   anything else is re-raised (no new traceback entry) */
PYDEF(end_async_for,             2, 2, 0, none)

/* ---- exceptions ------------------------------------------------------- */
/* [(exc(, cause))] -> never continues   RAISE_VARARGS (count = 0, 1, 2).
   count 0 re-raises the handled exception ("No active exception to
   reraise").  Sets __context__/__cause__/__suppress_context__. */
PYDEF(raise,                     4, 0, 0, npop)
/* [exc] -> never continues   RERAISE (no lasti: traceback entries are
   recorded at the first raise, so the re-raise adds none) */
PYDEF(reraise,                   2, 1, 0, none)
/* [exc] -> [prev_exc, exc]   PUSH_EXC_INFO.  A thrown value that is not a
   Python exception (JS error, stack overflow, ...) is converted first
   (qp_exc_normalize).  prev_exc may be None. */
PYDEF(push_exc_info,             2, 1, 2, none)
/* [prev_exc] -> []   POP_EXCEPT: restores the handled exception */
PYDEF(pop_except,                2, 1, 0, none)
/* [exc, type] -> [exc, bool]   CHECK_EXC_MATCH (TypeError "catching
   classes that do not inherit from BaseException is not allowed") */
PYDEF(check_exc_match,           2, 2, 2, none)
/* [exc, type] -> [rest, match]   CHECK_EG_MATCH (except*) */
PYDEF(check_eg_match,            2, 2, 2, none)
/* [exit, self, prev_exc, exc] -> [exit, self, prev_exc, exc, res]
   WITH_EXCEPT_START: res = exit(self, type(exc), exc, exc.__traceback__)
   (CPython has an extra lasti slot between self and prev_exc; we have
   none) */
PYDEF(with_except_start,         2, 4, 5, none)

/* ---- functions, classes, annotations ---------------------------------- */
/* [attr, func] -> [func]   SET_FUNCTION_ATTRIBUTE (u8 = QP_FUNC_ATTR_*;
   QP_FUNC_ATTR_CLOSURE is never emitted: fclosure captures the closure).
   MAKE_FUNCTION itself is OP_fclosure. */
PYDEF(set_func_attr,             3, 2, 1, u8)
/* [] -> [__build_class__]   LOAD_BUILD_CLASS (from the frame's builtins;
   NameError "__build_class__ not found") */
PYDEF(load_build_class,          2, 0, 1, none)
/* [] -> []   SETUP_ANNOTATIONS (locals mapping gets __annotations__ = {}) */
PYDEF(setup_annotations,         2, 0, 0, none)

/* ---- imports ---------------------------------------------------------- */
/* [level, fromlist] -> [module]   IMPORT_NAME (u8 = QP_IMPORT_* : CPython
   oparg & 3, PEP 810 lazy-import mode) */
PYDEF(import_name,               7, 2, 1, atom_u8)
/* [module] -> [module, attr]   IMPORT_FROM */
PYDEF(import_from,               6, 1, 2, atom)

/* ---- pattern matching ------------------------------------------------- */
/* [subject, cls, kw_names_tuple] -> [attrs_tuple or None]   MATCH_CLASS
   (u16 = number of positional sub-patterns) */
PYDEF(match_class,               4, 3, 1, u16)
/* [subject] -> [subject, bool]   MATCH_MAPPING */
PYDEF(match_mapping,             2, 1, 2, none)
/* [subject] -> [subject, bool]   MATCH_SEQUENCE */
PYDEF(match_sequence,            2, 1, 2, none)
/* [subject, keys] -> [subject, keys, values_tuple or None]   MATCH_KEYS */
PYDEF(match_keys,                2, 2, 3, none)
/* [obj] -> [obj, len(obj)]   GET_LEN */
PYDEF(get_len,                   2, 1, 2, none)

/* ---- intrinsics ------------------------------------------------------- */
/* [v] -> [res]   CALL_INTRINSIC_1 (u8 = QP_INTRINSIC1_*): print_expr
   (sys.displayhook), import *, StopIteration -> RuntimeError (PEP 479
   handler of generator bodies), async-generator value wrap, list->tuple,
   PEP 695 TypeVar/ParamSpec/TypeVarTuple/Generic[...]/TypeAliasType */
PYDEF(intrinsic1,                3, 1, 1, u8)
/* [a, b] -> [res]   CALL_INTRINSIC_2 (u8 = QP_INTRINSIC2_*) */
PYDEF(intrinsic2,                3, 2, 1, u8)
#undef PYDEF
#endif /* PYDEF */

#ifdef PYFLOW
/* Control-flow properties of subops (only subops listed here branch or end
   the instruction stream; all others fall through only).
   PYFLOW(id, kind, target_delta):
     QP_FLOW_BRANCH: falls through AND jumps to its label; the stack depth
       at the target = depth after the instruction + target_delta.
     QP_FLOW_EXIT: never falls through (like OP_throw); no target. */
PYFLOW(jump_if_false, QP_FLOW_BRANCH,  0)
PYFLOW(jump_if_true,  QP_FLOW_BRANCH,  0)
PYFLOW(for_iter,      QP_FLOW_BRANCH, -1)
PYFLOW(send,          QP_FLOW_BRANCH,  0)
PYFLOW(raise,         QP_FLOW_EXIT,    0)
PYFLOW(reraise,       QP_FLOW_EXIT,    0)
#undef PYFLOW
#endif /* PYFLOW */

#else /* !(PYFMT || PYDEF || PYFLOW): declarations */

#ifndef QP_OPCODE_H
#define QP_OPCODE_H

/* PY-only operand formats (see the table at the top). */
enum {
    QP_PYFMT_FIRST = 64,
    OP_FMT_npop2 = QP_PYFMT_FIRST,
    OP_FMT_npush,
    OP_FMT_unpack_ex,
};

/* Subop numbers: QP_OP_<id>. */
typedef enum QPSubOpcode {
#define PYDEF(id, size, n_pop, n_push, f) QP_OP_ ## id,
#include "opcode.h"
    QP_OP_COUNT,
} QPSubOpcode;

typedef struct QPSubopInfo {
    const char *name;   /* always present: used by the dumper, `dis` and errors */
    uint8_t size;   /* including OP_py and the subop byte */
    uint8_t n_pop;  /* base value, see the fmt rules */
    uint8_t n_push; /* base value, see the fmt rules */
    uint8_t fmt;    /* OPCodeFormat value or OP_FMT_npop2.. */
} QPSubopInfo;

/* The table every walker uses (indexed by QPSubOpcode). */
static const QPSubopInfo qp_subop_info[QP_OP_COUNT] = {
#define PYDEF(id, size, n_pop, n_push, f) { #id, size, n_pop, n_push, OP_FMT_ ## f },
#include "opcode.h"
};

enum {
    QP_FLOW_NONE = 0,
    QP_FLOW_BRANCH = 1,
    QP_FLOW_EXIT = 2,
};

typedef struct QPSubopFlow {
    uint8_t subop;
    uint8_t kind;          /* QP_FLOW_* */
    int8_t target_delta;
} QPSubopFlow;

static const QPSubopFlow qp_subop_flow[] = {
#define PYFLOW(id, kind, delta) { QP_OP_ ## id, kind, delta },
#include "opcode.h"
};

/* ---- operand values ---------------------------------------------------- */

/* binop: CPython NB_* (Include/opcode.h).  QP_NB_SUBSCR is only used by
   qp_binop callers that want getitem; codegen emits the getitem subop. */
enum {
    QP_NB_ADD = 0,
    QP_NB_AND = 1,
    QP_NB_FLOOR_DIVIDE = 2,
    QP_NB_LSHIFT = 3,
    QP_NB_MATRIX_MULTIPLY = 4,
    QP_NB_MULTIPLY = 5,
    QP_NB_REMAINDER = 6,
    QP_NB_OR = 7,
    QP_NB_POWER = 8,
    QP_NB_RSHIFT = 9,
    QP_NB_SUBTRACT = 10,
    QP_NB_TRUE_DIVIDE = 11,
    QP_NB_XOR = 12,
    QP_NB_INPLACE_ADD = 13,
    QP_NB_INPLACE_AND = 14,
    QP_NB_INPLACE_FLOOR_DIVIDE = 15,
    QP_NB_INPLACE_LSHIFT = 16,
    QP_NB_INPLACE_MATRIX_MULTIPLY = 17,
    QP_NB_INPLACE_MULTIPLY = 18,
    QP_NB_INPLACE_REMAINDER = 19,
    QP_NB_INPLACE_OR = 20,
    QP_NB_INPLACE_POWER = 21,
    QP_NB_INPLACE_RSHIFT = 22,
    QP_NB_INPLACE_SUBTRACT = 23,
    QP_NB_INPLACE_TRUE_DIVIDE = 24,
    QP_NB_INPLACE_XOR = 25,
    QP_NB_SUBSCR = 26,
    QP_NB_INPLACE_OFFSET = QP_NB_INPLACE_ADD - QP_NB_ADD, /* inplace = binary + 13 */
};

/* unop */
enum {
    QP_UNOP_NEG = 0,     /* UNARY_NEGATIVE */
    QP_UNOP_INVERT = 1,  /* UNARY_INVERT */
    QP_UNOP_POS = 2,     /* CALL_INTRINSIC_1 INTRINSIC_UNARY_POSITIVE */
};

/* compare: Py_LT..Py_GE (Include/object.h) + to-bool flag */
enum {
    QP_CMP_LT = 0,
    QP_CMP_LE = 1,
    QP_CMP_EQ = 2,
    QP_CMP_NE = 3,
    QP_CMP_GT = 4,
    QP_CMP_GE = 5,
    QP_CMP_OP_MASK = 0x07,
    QP_CMP_TO_BOOL = 0x10,
};

/* load_special: SPECIAL_* (pycore_ceval.h) */
enum {
    QP_SPECIAL_ENTER = 0,
    QP_SPECIAL_EXIT = 1,
    QP_SPECIAL_AENTER = 2,
    QP_SPECIAL_AEXIT = 3,
};

/* load_common_const: CONSTANT_* (pycore_opcode_utils.h) */
enum {
    QP_CONST_ASSERTIONERROR = 0,
    QP_CONST_NOTIMPLEMENTEDERROR = 1,
    QP_CONST_BUILTIN_TUPLE = 2,
    QP_CONST_BUILTIN_ALL = 3,
    QP_CONST_BUILTIN_ANY = 4,
    QP_CONST_BUILTIN_LIST = 5,
    QP_CONST_BUILTIN_SET = 6,
    QP_CONST_NONE = 7,
    QP_CONST_EMPTY_STR = 8,
    QP_CONST_TRUE = 9,
    QP_CONST_FALSE = 10,
    QP_CONST_MINUS_ONE = 11,
    QP_CONST_COUNT = 12,
};

/* intrinsic1: INTRINSIC_* (pycore_intrinsics.h); 5 (UNARY_POSITIVE) is
   emitted as unop QP_UNOP_POS instead */
enum {
    QP_INTRINSIC1_INVALID = 0,
    QP_INTRINSIC1_PRINT = 1,               /* [v] -> [None]: sys.displayhook(v) */
    QP_INTRINSIC1_IMPORT_STAR = 2,         /* [module] -> [None]: into locals mapping */
    QP_INTRINSIC1_STOPITERATION_ERROR = 3, /* [exc] -> [exc']: PEP 479 conversion */
    QP_INTRINSIC1_ASYNC_GEN_WRAP = 4,      /* [v] -> [wrapped] */
    QP_INTRINSIC1_UNARY_POSITIVE = 5,      /* not emitted (unop) */
    QP_INTRINSIC1_LIST_TO_TUPLE = 6,
    QP_INTRINSIC1_TYPEVAR = 7,
    QP_INTRINSIC1_PARAMSPEC = 8,
    QP_INTRINSIC1_TYPEVARTUPLE = 9,
    QP_INTRINSIC1_SUBSCRIPT_GENERIC = 10,
    QP_INTRINSIC1_TYPEALIAS = 11,
    QP_INTRINSIC1_COUNT = 12,
};

/* intrinsic2 */
enum {
    QP_INTRINSIC2_INVALID = 0,
    QP_INTRINSIC2_PREP_RERAISE_STAR = 1,
    QP_INTRINSIC2_TYPEVAR_WITH_BOUND = 2,
    QP_INTRINSIC2_TYPEVAR_WITH_CONSTRAINTS = 3,
    QP_INTRINSIC2_SET_FUNCTION_TYPE_PARAMS = 4,
    QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT = 5,
    QP_INTRINSIC2_COUNT = 6,
};

/* set_func_attr: MAKE_FUNCTION_* (pycore_opcode_utils.h) */
enum {
    QP_FUNC_ATTR_DEFAULTS = 0x01,    /* tuple -> __defaults__ */
    QP_FUNC_ATTR_KWDEFAULTS = 0x02,  /* dict -> __kwdefaults__ */
    QP_FUNC_ATTR_ANNOTATIONS = 0x04, /* dict -> __annotations__ (from __future__ import annotations) */
    QP_FUNC_ATTR_CLOSURE = 0x08,     /* never emitted */
    QP_FUNC_ATTR_ANNOTATE = 0x10,    /* function -> __annotate__ (PEP 649) */
};

/* convert_value / build_interpolation: FVC_* (Include/ceval.h) */
enum {
    QP_FVC_NONE = 0,
    QP_FVC_STR = 1,
    QP_FVC_REPR = 2,
    QP_FVC_ASCII = 3,
};

/* import_name: IMPORT_NAME oparg & 3 (PEP 810) */
enum {
    QP_IMPORT_DEFAULT = 0, /* lazy if the global lazy-imports mode says so */
    QP_IMPORT_LAZY = 1,    /* `lazy import` / `lazy from ... import` */
    QP_IMPORT_EAGER = 2,   /* forced eager (in try/except, not module level, import *) */
};

/* get_iter: 0 or GET_ITER_YIELD_FROM* (pycore_opcode_utils.h) */
enum {
    QP_GETITER_NORMAL = 0,
    QP_GETITER_YIELD_FROM = 1,
    QP_GETITER_YIELD_FROM_NO_CHECK = 2,
    QP_GETITER_YIELD_FROM_CORO_CHECK = 3,
};

/* get_awaitable: CPython GET_AWAITABLE oparg */
enum {
    QP_AWAITABLE_AWAIT = 0,  /* await expression */
    QP_AWAITABLE_AENTER = 1, /* "'async with' received an object from __aenter__ that does not implement __await__: %.100s" */
    QP_AWAITABLE_AEXIT = 2,  /* same for __aexit__ */
};

/* yield_value */
enum {
    QP_YIELD_PLAIN = 0,
    QP_YIELD_DELEGATE = 1,
};

/* load_super_attr / load_super_method flags (CPython oparg & 2) */
#define QP_SUPER_TWO_ARGS 0x02

/* load_from_dict_or_deref: closure-variable bit of the u16 operand */
#define QP_DEREF_FREE 0x8000

#endif /* QP_OPCODE_H */

#endif /* PYFMT || PYDEF || PYFLOW */
