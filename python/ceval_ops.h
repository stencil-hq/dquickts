/*
 * dquickts Python, milestone M2: OP_py subop handlers.
 *
 * Included by quickjs.c inside JS_CallInternal() at CASE(OP_py): the code
 * sees the interpreter locals (ctx, rt, sf, b, pc, sp, arg_buf, var_buf,
 * var_refs, ret_val, opcode, i) and leaves through BREAK-like PY_NEXT,
 * `goto exception` or `goto done_generator`.  On entry pc points at the
 * subop byte.  Operands that are still on the stack when an error is
 * raised are released by the exception unwinding, so handlers free their
 * operands only after success.  Out-of-line bodies are in ceval.c.
 */

#if DIRECT_DISPATCH
#define PYCASE(id)  py_case_ ## id
#define PY_NEXT     BREAK
            {
                static const void * const py_dispatch_table[256] = {
#define PYDEF(id, size, n_pop, n_push, f) [QP_OP_ ## id] = &&py_case_ ## id,
#include "opcode.h"
                    [QP_OP_COUNT ... 255] = &&py_case_invalid,
                };
                goto *py_dispatch_table[*pc++];
            }
#else
#define PYCASE(id)  case QP_OP_ ## id
#define PY_NEXT     goto py_next
            switch(*pc++) {
            default:
#endif

#define PY_SYNC()   (sf->cur_pc = pc)
/* _CHECK_PERIODIC_AT_END of CALL (check_periodics_at_end): a poll
   requested while the callee ran (signal, _thread.interrupt_main(), GIL
   drop request) is done before the next instruction, so that the
   exception of a Python signal handler is raised by the call (inside its
   `try`/`with`), not by the next one; but not after the call of __enter__
   (`skip`: the call follows a load_special), whose with block is not set
   up yet.  The result is already on the stack (released by the
   unwinding). */
#define PY_CALL_POLL(skip)                                              \
    do {                                                                \
        if (unlikely(ctx->interrupt_counter <= 1) && !(skip) &&         \
            js_poll_interrupts(ctx))                                    \
            goto exception;                                             \
    } while (0)
#define PY_CODE_IC(idx) (&b->py_code->ic[idx])

        PYCASE(invalid):
            JS_ThrowInternalError(ctx, "invalid python subop: pc=%u",
                                  (int)(pc - b->byte_code_buf - 2));
            goto exception;

        /* ---- stack and constants ------------------------------------- */
        PYCASE(push_null):
            *sp++ = QP_NULL;
            PY_NEXT;
        PYCASE(copy):
            {
                int n = *pc++;
                *sp = JS_DupValue(ctx, sp[-n]);
                sp++;
            }
            PY_NEXT;
        PYCASE(swap):
            {
                int n = *pc++;
                JSValue tmp = sp[-1];
                sp[-1] = sp[-n];
                sp[-n] = tmp;
            }
            PY_NEXT;
        PYCASE(load_common_const):
            {
                JSValue v = qp_load_common_const(ctx, sf, *pc++);
                if (unlikely(JS_IsException(v)))
                    goto exception;
                *sp++ = v;
            }
            PY_NEXT;

        /* ---- operators ----------------------------------------------- */
        PYCASE(binop):
            {
                JSValue r;
                int nb = *pc++;
                PY_SYNC();
                r = qp_binop(ctx, sp[-2], sp[-1], nb);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = r;
                sp--;
            }
            PY_NEXT;
        PYCASE(unop):
            {
                JSValue r;
                int op = *pc++;
                PY_SYNC();
                r = qp_unop(ctx, sp[-1], op);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = r;
            }
            PY_NEXT;
        PYCASE(compare):
            {
                JSValue r;
                int op = *pc++;
                PY_SYNC();
                r = qp_richcompare(ctx, sp[-2], sp[-1], op & QP_CMP_OP_MASK);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                if ((op & QP_CMP_TO_BOOL) && JS_VALUE_GET_TAG(r) != JS_TAG_BOOL) {
                    int t = qp_bool(ctx, r);
                    JS_FreeValue(ctx, r);
                    if (t < 0)
                        goto exception;
                    r = JS_NewBool(ctx, t);
                }
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = r;
                sp--;
            }
            PY_NEXT;
        PYCASE(is_op):
            {
                int res = qp_is(sp[-2], sp[-1]) ^ (*pc++ != 0);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = JS_NewBool(ctx, res);
                sp--;
            }
            PY_NEXT;
        PYCASE(contains):
            {
                int invert = *pc++, res;
                PY_SYNC();
                res = qp_contains(ctx, sp[-1], sp[-2]);
                if (unlikely(res < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = JS_NewBool(ctx, res ^ (invert != 0));
                sp--;
            }
            PY_NEXT;
        PYCASE(to_bool):
            if (JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_BOOL) {
                int res;
                PY_SYNC();
                res = qp_bool(ctx, sp[-1]);
                if (unlikely(res < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = JS_NewBool(ctx, res);
            }
            PY_NEXT;
        PYCASE(jump_if_false):
        PYCASE(jump_if_true):
            {
                JSValue v = sp[-1];
                int res;
                if (JS_VALUE_GET_TAG(v) == JS_TAG_BOOL) {
                    res = JS_VALUE_GET_INT(v);
                } else if (JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED ||
                           JS_VALUE_GET_TAG(v) == JS_TAG_NULL) {
                    res = 0;
                } else if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
                    res = JS_VALUE_GET_INT(v) != 0;
                } else {
                    PY_SYNC();
                    res = qp_bool(ctx, v);
                    if (unlikely(res < 0))
                        goto exception;
                    JS_FreeValue(ctx, v);
                }
                sp--;
                if (res == (pc[-1] == QP_OP_jump_if_true)) {
                    pc += (int32_t)get_u32(pc);
                    SAFEPOINT();
                } else {
                    pc += 4;
                }
            }
            PY_NEXT;

        /* ---- subscripts and slices ----------------------------------- */
        PYCASE(getitem):
            {
                JSValue r;
                PY_SYNC();
                r = qp_getitem(ctx, sp[-2], sp[-1]);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = r;
                sp--;
            }
            PY_NEXT;
        PYCASE(setitem):
            PY_SYNC();
            if (unlikely(qp_setitem(ctx, sp[-2], sp[-1], sp[-3]) < 0))
                goto exception;
            JS_FreeValue(ctx, sp[-3]);
            JS_FreeValue(ctx, sp[-2]);
            JS_FreeValue(ctx, sp[-1]);
            sp -= 3;
            PY_NEXT;
        PYCASE(delitem):
            PY_SYNC();
            if (unlikely(qp_delitem(ctx, sp[-2], sp[-1]) < 0))
                goto exception;
            JS_FreeValue(ctx, sp[-2]);
            JS_FreeValue(ctx, sp[-1]);
            sp -= 2;
            PY_NEXT;
        PYCASE(getslice):
            {
                JSValue r;
                PY_SYNC();
                r = qp_getslice(ctx, sp[-3], sp[-2], sp[-1]);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-3] = r;
                sp -= 2;
            }
            PY_NEXT;
        PYCASE(setslice):
            PY_SYNC();
            if (unlikely(qp_setslice(ctx, sp[-3], sp[-2], sp[-1], sp[-4]) < 0))
                goto exception;
            for(i = -4; i < 0; i++)
                JS_FreeValue(ctx, sp[i]);
            sp -= 4;
            PY_NEXT;
        PYCASE(build_slice):
            {
                int n = get_u16(pc);
                JSValue r;
                pc += 2;
                r = qp_slice_new(ctx, sp[-n], sp[1 - n],
                                 n == 3 ? sp[-1] : JS_UNDEFINED);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                for(i = -n; i < 0; i++)
                    JS_FreeValue(ctx, sp[i]);
                sp -= n;
                *sp++ = r;
            }
            PY_NEXT;

        /* ---- attributes ---------------------------------------------- */
        PYCASE(getattr):
            {
                JSAtom atom = get_u32(pc);
                QPAttrCache *ic = &PY_CODE_IC(get_u16(pc + 4))->attr;
                JSValue r;
                pc += 6;
                PY_SYNC();
                r = qp_getattr_ic(ctx, sp[-1], atom, ic);
                if (unlikely(JS_IsException(r)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = r;
            }
            PY_NEXT;
        PYCASE(load_method):
            {
                JSAtom atom = get_u32(pc);
                QPAttrCache *ic = &PY_CODE_IC(get_u16(pc + 4))->attr;
                JSValue meth, self;
                pc += 6;
                PY_SYNC();
                if (unlikely(qp_load_method_ic(ctx, sp[-1], atom, ic, &meth, &self) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = meth;
                *sp++ = self;
            }
            PY_NEXT;
        PYCASE(setattr):
            {
                JSAtom atom = get_u32(pc);
                QPAttrCache *ic = &PY_CODE_IC(get_u16(pc + 4))->attr;
                pc += 6;
                PY_SYNC();
                if (unlikely(qp_setattr_ic(ctx, sp[-1], atom, sp[-2], ic) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 2;
            }
            PY_NEXT;
        PYCASE(delattr):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                if (unlikely(qp_delattr(ctx, sp[-1], atom) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(load_super_attr):
        PYCASE(load_super_method):
            {
                JSAtom atom = get_u32(pc);
                int sflags = pc[4];
                BOOL is_method = (pc[-1] == QP_OP_load_super_method);
                JSValue res, self = QP_NULL;
                pc += 5;
                PY_SYNC();
                if (unlikely(sf->py_mon_flags & QP_MON_SF_SUPER_GENERIC)) {
                    sf->py_mon_flags &= ~QP_MON_SF_SUPER_GENERIC;
                    is_method = FALSE;      /* (self pushed as NULL) */
                }
                if (unlikely(qp_super_lookup(ctx, sp[-3], sp[-2], sp[-1], atom, sflags,
                                             &res, is_method ? &self : NULL) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 3;
                *sp++ = res;
                if (pc[-6] == QP_OP_load_super_method)
                    *sp++ = self;
            }
            PY_NEXT;
        PYCASE(load_special):
            {
                JSValue meth, self;
                int which = *pc++;
                PY_SYNC();
                if (unlikely(qp_load_special(ctx, sp[-1], which, &meth, &self) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = meth;
                *sp++ = self;
            }
            PY_NEXT;

        /* ---- names --------------------------------------------------- */
        PYCASE(load_global):
            {
                JSAtom atom = get_u32(pc);
                QPGlobalCache *ic = &PY_CODE_IC(get_u16(pc + 4))->global;
                JSValue v;
                pc += 6;
                PY_SYNC();
                v = qp_ceval_load_global(ctx, sf->cur_func, atom, ic);
                if (unlikely(JS_IsException(v)))
                    goto exception;
                *sp++ = v;
            }
            PY_NEXT;
        PYCASE(store_global):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                if (unlikely(qp_ceval_store_global(ctx, sf->cur_func, atom, sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(delete_global):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                if (unlikely(qp_ceval_delete_global(ctx, sf->cur_func, atom) < 0))
                    goto exception;
            }
            PY_NEXT;
        PYCASE(load_name):
            {
                JSAtom atom = get_u32(pc);
                JSValue v;
                pc += 4;
                PY_SYNC();
                v = qp_ceval_load_name(ctx, sf->cur_func, arg_buf[0], atom);
                if (unlikely(JS_IsException(v)))
                    goto exception;
                *sp++ = v;
            }
            PY_NEXT;
        PYCASE(store_name):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                if (unlikely(qp_ceval_store_name(ctx, arg_buf[0], atom, sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(delete_name):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                if (unlikely(qp_ceval_delete_name(ctx, arg_buf[0], atom) < 0))
                    goto exception;
            }
            PY_NEXT;
        PYCASE(load_locals):
            if (unlikely(b->arg_count < 1 || (b->py_code->co_flags & CO_NEWLOCALS))) {
                qp_raise_system_error(ctx, "no locals found");
                goto exception;
            }
            *sp++ = JS_DupValue(ctx, arg_buf[0]);
            PY_NEXT;
        PYCASE(load_from_dict_or_globals):
            {
                JSAtom atom = get_u32(pc);
                QPGlobalCache *ic = &PY_CODE_IC(get_u16(pc + 4))->global;
                JSValue v;
                int res;
                pc += 6;
                PY_SYNC();
                res = qp_ceval_mapping_get(ctx, sp[-1], atom, &v);
                if (unlikely(res < 0))
                    goto exception;
                if (res == 0) {
                    v = qp_ceval_load_global(ctx, sf->cur_func, atom, ic);
                    if (unlikely(JS_IsException(v)))
                        goto exception;
                }
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = v;
            }
            PY_NEXT;
        PYCASE(load_from_dict_or_deref):
            {
                JSAtom atom = get_u32(pc);
                int idx = get_u16(pc + 4);
                JSValue v;
                int res;
                pc += 6;
                PY_SYNC();
                res = qp_ceval_mapping_get(ctx, sp[-1], atom, &v);
                if (unlikely(res < 0))
                    goto exception;
                if (res == 0) {
                    if (idx & QP_DEREF_FREE)
                        v = *var_refs[idx & ~QP_DEREF_FREE]->pvalue;
                    else
                        v = var_buf[idx];
                    if (unlikely(JS_IsUninitialized(v))) {
                        qp_raise_unbound(ctx, atom, (idx & QP_DEREF_FREE) != 0);
                        goto exception;
                    }
                    v = JS_DupValue(ctx, v);
                }
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = v;
            }
            PY_NEXT;
        PYCASE(get_arg_check):
            {
                int idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(arg_buf[idx]))) {
                    qp_raise_unbound(ctx, b->vardefs ? b->vardefs[idx].var_name : JS_ATOM_NULL,
                                     FALSE);
                    goto exception;
                }
                *sp++ = JS_DupValue(ctx, arg_buf[idx]);
            }
            PY_NEXT;
        PYCASE(make_cell):
            {
                int idx = get_u16(pc);
                JSVarRef *var_ref;
                JSValue cell;
                pc += 2;
                var_ref = get_var_ref(ctx, sf, idx, FALSE);
                if (unlikely(!var_ref))
                    goto exception;
                cell = qp_cell_new(ctx, var_ref);
                free_var_ref(rt, var_ref);
                if (unlikely(JS_IsException(cell)))
                    goto exception;
                *sp++ = cell;
            }
            PY_NEXT;

        /* ---- calls --------------------------------------------------- */
        PYCASE(call_method):
            {
                JSValue *args;
                int nargs, cflags = JS_CALL_FLAG_DIRECT;
                call_argc = get_u16(pc);
                pc += 2;
                call_argv = sp - call_argc;
                if (QP_IS_NULL(call_argv[-1])) {
                    args = call_argv;
                    nargs = call_argc;
                    cflags |= JS_CALL_FLAG_PY_ARGS_OFFSET;
                } else {
                    args = call_argv - 1;
                    nargs = call_argc + 1;
                }
                sf->cur_pc = pc;
                ret_val = JS_CallInternal(ctx, call_argv[-2], JS_UNDEFINED,
                                          JS_UNDEFINED, nargs, args, cflags);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for(i = -2; i < call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 2;
                *sp++ = ret_val;
                /* pc: after this 4-byte call; a load_special is 3 bytes */
                PY_CALL_POLL(pc - 7 >= b->byte_code_buf && pc[-7] == OP_py &&
                             pc[-6] == QP_OP_load_special);
            }
            PY_NEXT;
        PYCASE(call_kw):
            {
                JSValue *args;
                int nargs, cflags = JS_CALL_FLAG_DIRECT | JS_CALL_FLAG_PY_KW;
                JSValue kwnames = sp[-1];
                call_argc = get_u16(pc);
                pc += 2;
                call_argv = sp - 1 - call_argc;
                if (QP_IS_NULL(call_argv[-1])) {
                    args = call_argv;
                    nargs = call_argc;
                    cflags |= JS_CALL_FLAG_PY_ARGS_OFFSET;
                } else {
                    args = call_argv - 1;
                    nargs = call_argc + 1;
                }
                sf->cur_pc = pc;
                if (QP_TUPLE_SIZE(kwnames) == 0)
                    cflags &= ~JS_CALL_FLAG_PY_KW;
                ret_val = JS_CallInternal(ctx, call_argv[-2], JS_UNDEFINED,
                                          (cflags & JS_CALL_FLAG_PY_KW) ? kwnames : JS_UNDEFINED,
                                          nargs, args, cflags);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for(i = -2; i <= call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 3;
                *sp++ = ret_val;
                PY_CALL_POLL(0);
            }
            PY_NEXT;
        PYCASE(call_ex):
            PY_SYNC();
            ret_val = qp_ceval_call_ex(ctx, sp[-4], sp[-3], sp[-2], sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            for(i = -4; i < 0; i++)
                JS_FreeValue(ctx, sp[i]);
            sp -= 4;
            *sp++ = ret_val;
            PY_CALL_POLL(0);
            PY_NEXT;

        /* ---- building ------------------------------------------------ */
        PYCASE(build_tuple):
            {
                int n = get_u16(pc);
                pc += 2;
                /* the items are stolen even on failure */
                sp -= n;
                ret_val = qp_tuple_new_steal(ctx, n, sp);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                *sp++ = ret_val;
            }
            PY_NEXT;
        PYCASE(build_set):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                ret_val = qp_set_from_array(ctx, n, (JSValueConst *)(sp - n));
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for(i = -n; i < 0; i++)
                    JS_FreeValue(ctx, sp[i]);
                sp -= n;
                *sp++ = ret_val;
            }
            PY_NEXT;
        PYCASE(build_map):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                ret_val = qp_dict_from_pairs(ctx, n, (JSValueConst *)(sp - 2 * n));
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for(i = -2 * n; i < 0; i++)
                    JS_FreeValue(ctx, sp[i]);
                sp -= 2 * n;
                *sp++ = ret_val;
            }
            PY_NEXT;
        PYCASE(build_string):
            {
                int n = get_u16(pc);
                JSValue sep;
                pc += 2;
                if (n == 1) {
                    PY_NEXT;
                }
                sep = JS_AtomToString(ctx, JS_ATOM_empty_string);
                ret_val = qp_str_join_array(ctx, sep, n, (JSValueConst *)(sp - n));
                JS_FreeValue(ctx, sep);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for(i = -n; i < 0; i++)
                    JS_FreeValue(ctx, sp[i]);
                sp -= n;
                *sp++ = ret_val;
            }
            PY_NEXT;
        PYCASE(list_append):
            {
                int n = get_u16(pc);
                pc += 2;
                if (unlikely(qp_list_append(ctx, sp[-n - 1], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(list_extend):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_ceval_list_extend(ctx, sp[-n - 1], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(set_add):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_set_add(ctx, sp[-n - 1], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(set_update):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_set_update(ctx, sp[-n - 1], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(map_add):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_dict_set_item(ctx, sp[-n - 2], sp[-2], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 2;
            }
            PY_NEXT;
        PYCASE(dict_update):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_ceval_dict_update(ctx, sp[-n - 1], sp[-1]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(dict_merge):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                if (unlikely(qp_dict_merge_kwargs(ctx, sp[-n - 1], sp[-1], sp[-n - 4]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp--;
            }
            PY_NEXT;
        PYCASE(unpack_seq):
            {
                int n = get_u16(pc);
                JSValue seq;
                pc += 2;
                PY_SYNC();
                seq = *--sp;
                if (unlikely(qp_unpack_iterable(ctx, seq, n, -1, sp) < 0)) {
                    JS_FreeValue(ctx, seq);
                    goto exception;
                }
                JS_FreeValue(ctx, seq);
                sp += n;
            }
            PY_NEXT;
        PYCASE(unpack_ex):
            {
                int v = get_u16(pc);
                int before = v & 0xff, after = v >> 8;
                JSValue seq;
                pc += 2;
                PY_SYNC();
                seq = *--sp;
                if (unlikely(qp_unpack_iterable(ctx, seq, before, after, sp) < 0)) {
                    JS_FreeValue(ctx, seq);
                    goto exception;
                }
                JS_FreeValue(ctx, seq);
                sp += before + 1 + after;
            }
            PY_NEXT;

        /* ---- f-strings and t-strings --------------------------------- */
        PYCASE(convert_value):
            {
                int conv = *pc++;
                PY_SYNC();
                switch(conv) {
                case QP_FVC_REPR:
                    ret_val = qp_repr(ctx, sp[-1]);
                    break;
                case QP_FVC_ASCII:
                    ret_val = qp_ascii(ctx, sp[-1]);
                    break;
                default:
                    ret_val = qp_str(ctx, sp[-1]);
                    break;
                }
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            PY_NEXT;
        PYCASE(format_simple):
            if (!qp_str_check_exact(sp[-1])) {
                PY_SYNC();
                ret_val = qp_format(ctx, sp[-1], QP_NULL);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            PY_NEXT;
        PYCASE(format_with_spec):
            PY_SYNC();
            ret_val = qp_format(ctx, sp[-2], sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            JS_FreeValue(ctx, sp[-2]);
            JS_FreeValue(ctx, sp[-1]);
            sp[-2] = ret_val;
            sp--;
            PY_NEXT;
        PYCASE(build_interpolation):
            {
                int conv = *pc++;
                ret_val = qp_interpolation_new(ctx, sp[-3], sp[-2], conv, sp[-1]);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-3] = ret_val;
                sp -= 2;
            }
            PY_NEXT;
        PYCASE(build_template):
            ret_val = qp_template_new(ctx, sp[-2], sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            JS_FreeValue(ctx, sp[-2]);
            JS_FreeValue(ctx, sp[-1]);
            sp[-2] = ret_val;
            sp--;
            PY_NEXT;

        /* ---- iteration ----------------------------------------------- */
        PYCASE(get_iter):
            {
                int kind = *pc++;
                JSValue it, idx;
                PY_SYNC();
                if (unlikely(qp_get_iter_virtual(ctx, sp[-1], kind, &it, &idx) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = it;
                *sp++ = idx;
            }
            PY_NEXT;
        PYCASE(for_iter):
            {
                JSValue item;
                int res;
                PY_SYNC();
                res = qp_iter_next_virtual(ctx, sp[-2], &sp[-1], &item);
                if (unlikely(res < 0))
                    goto exception;
                if (res) {
                    *sp++ = item;
                    pc += 4;
                } else {
                    pc += (int32_t)get_u32(pc);
                }
            }
            PY_NEXT;
        PYCASE(send):
            {
                JSValue res;
                int r;
                PY_SYNC();
                if (JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_INT && JS_IsUndefined(sp[-1])) {
                    /* virtual iteration of a sequence */
                    r = qp_iter_next_virtual(ctx, sp[-4], &sp[-3], &res);
                    if (unlikely(r < 0))
                        goto exception;
                    if (r) {
                        r = QP_SEND_NEXT;
                    } else {
                        r = QP_SEND_RETURN;
                        res = JS_UNDEFINED;
                    }
                } else {
                    r = qp_iter_send(ctx, sp[-4], sp[-1], &res);
                    if (unlikely(r == QP_SEND_ERROR))
                        goto exception;
                }
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = res;
                if (r == QP_SEND_RETURN) {
                    /* M6: sys.settrace 'exception' StopIteration (END_SEND),
                       sys.monitoring STOP_ITERATION */
                    if (unlikely(ctx->py->trace_active | ctx->py->mon_active) &&
                        qp_hook_send_return(ctx, sf, sp[-4], res) < 0)
                        goto exception;
                    pc += (int32_t)get_u32(pc);
                } else {
                    pc += 4;
                }
            }
            PY_NEXT;
        PYCASE(yield_value):
            ret_val = JS_NewInt32(ctx, *pc++ == QP_YIELD_DELEGATE ?
                                  FUNC_RET_YIELD_STAR : FUNC_RET_YIELD);
            goto done_generator;
        PYCASE(end_send):
            JS_FreeValue(ctx, sp[-3]);
            JS_FreeValue(ctx, sp[-2]);
            sp[-3] = sp[-1];
            sp -= 2;
            PY_NEXT;
        PYCASE(cleanup_throw):
            if (qp_exc_given_matches(ctx, sp[-1], QP_TYPE_VALUE(QP_EXC(ctx, StopIteration)))) {
                JSValue v = qp_exc_stopiteration_value(ctx, sp[-1]);
                if (unlikely(JS_IsException(v)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = v;
            } else {
                JS_Throw(ctx, *--sp);
                ctx->py->reraise = 1;
                goto exception;
            }
            PY_NEXT;
        PYCASE(get_awaitable):
            {
                int where = *pc++;
                PY_SYNC();
                ret_val = qp_get_awaitable(ctx, sp[-1], where);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            PY_NEXT;
        PYCASE(get_aiter):
            PY_SYNC();
            ret_val = qp_get_aiter(ctx, sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            JS_FreeValue(ctx, sp[-1]);
            sp[-1] = ret_val;
            PY_NEXT;
        PYCASE(get_anext):
            PY_SYNC();
            ret_val = qp_get_anext(ctx, sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            *sp++ = ret_val;
            PY_NEXT;
        PYCASE(end_async_for):
            if (qp_exc_given_matches(ctx, sp[-1], QP_TYPE_VALUE(QP_EXC(ctx, StopAsyncIteration)))) {
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 2;
            } else {
                JS_Throw(ctx, *--sp);
                ctx->py->reraise = 1;
                goto exception;
            }
            PY_NEXT;

        /* ---- exceptions ---------------------------------------------- */
        PYCASE(raise):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                qp_do_raise(ctx, n >= 1 ? sp[-n] : QP_NULL, n == 2 ? sp[-1] : QP_NULL);
                goto exception;
            }
        PYCASE(reraise):
            pc += 1;
            JS_Throw(ctx, *--sp);
            ctx->py->reraise = 1;
            goto exception;
        PYCASE(push_exc_info):
            {
                JSValue exc, prev;
                exc = qp_exc_normalize(ctx, sp[-1]);
                sp[-1] = JS_UNDEFINED;
                if (unlikely(JS_IsException(exc)))
                    goto exception;
                qp_exc_info_push(ctx, exc, &prev);
                sp[-1] = prev;
                *sp++ = exc;
            }
            PY_NEXT;
        PYCASE(pop_except):
            qp_exc_info_pop(ctx, *--sp);
            PY_NEXT;
        PYCASE(check_exc_match):
            {
                int res = qp_exc_check_match(ctx, sp[-2], sp[-1]);
                if (unlikely(res < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = JS_NewBool(ctx, res);
            }
            PY_NEXT;
        PYCASE(check_eg_match):
            {
                JSValue match, rest;
                PY_SYNC();
                if (unlikely(qp_exc_group_match(ctx, sp[-2], sp[-1], &match, &rest) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = rest;
                sp[-1] = match;
            }
            PY_NEXT;
        PYCASE(with_except_start):
            PY_SYNC();
            ret_val = qp_ceval_with_except_start(ctx, sp[-4], sp[-3], sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            *sp++ = ret_val;
            PY_NEXT;

        /* ---- functions, classes, annotations ------------------------- */
        PYCASE(set_func_attr):
            {
                int flag = *pc++;
                if (unlikely(qp_function_set_attr(ctx, sp[-1], flag, sp[-2]) < 0))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                sp[-2] = sp[-1];
                sp--;
            }
            PY_NEXT;
        PYCASE(load_build_class):
            ret_val = qp_ceval_load_build_class(ctx, sf->cur_func);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            *sp++ = ret_val;
            PY_NEXT;
        PYCASE(setup_annotations):
            PY_SYNC();
            if (unlikely(qp_ceval_setup_annotations(ctx, arg_buf[0]) < 0))
                goto exception;
            PY_NEXT;

        /* ---- imports ------------------------------------------------- */
        PYCASE(import_name):
            {
                JSAtom atom = get_u32(pc);
                int mode = pc[4];
                pc += 5;
                PY_SYNC();
                ret_val = qp_ceval_import_name(ctx, sf->cur_func, b, arg_buf, atom,
                                               sp[-2], sp[-1], mode);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = ret_val;
                sp--;
            }
            PY_NEXT;
        PYCASE(import_from):
            {
                JSAtom atom = get_u32(pc);
                pc += 4;
                PY_SYNC();
                ret_val = qp_import_from(ctx, sp[-1], atom);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                *sp++ = ret_val;
            }
            PY_NEXT;

        /* ---- pattern matching ---------------------------------------- */
        PYCASE(match_class):
            {
                int n = get_u16(pc);
                pc += 2;
                PY_SYNC();
                ret_val = qp_match_class(ctx, sp[-3], sp[-2], n, sp[-1]);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-3] = ret_val;
                sp -= 2;
            }
            PY_NEXT;
        PYCASE(match_mapping):
            *sp = JS_NewBool(ctx, (qp_type_of(ctx, sp[-1])->flags & QP_TPFLAGS_MAPPING) != 0);
            sp++;
            PY_NEXT;
        PYCASE(match_sequence):
            *sp = JS_NewBool(ctx, (qp_type_of(ctx, sp[-1])->flags & QP_TPFLAGS_SEQUENCE) != 0);
            sp++;
            PY_NEXT;
        PYCASE(match_keys):
            PY_SYNC();
            ret_val = qp_match_keys(ctx, sp[-2], sp[-1]);
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            *sp++ = ret_val;
            PY_NEXT;
        PYCASE(get_len):
            {
                Py_ssize_t len;
                PY_SYNC();
                len = qp_len(ctx, sp[-1]);
                if (unlikely(len < 0))
                    goto exception;
                *sp++ = qp_long_from_int64(ctx, len);
            }
            PY_NEXT;

        /* ---- intrinsics ---------------------------------------------- */
        PYCASE(intrinsic1):
            {
                int id = *pc++;
                PY_SYNC();
                ret_val = qp_ceval_intrinsic1(ctx, sf, b, arg_buf, id, sp[-1]);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            PY_NEXT;
        PYCASE(intrinsic2):
            {
                int id = *pc++;
                PY_SYNC();
                ret_val = qp_ceval_intrinsic2(ctx, id, sp[-2], sp[-1]);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp[-2] = ret_val;
                sp--;
            }
            PY_NEXT;

#if !DIRECT_DISPATCH
            }
        py_next:
            BREAK;
#endif

#undef PYCASE
#undef PY_NEXT
#undef PY_SYNC
#undef PY_CALL_POLL
#undef PY_CODE_IC
