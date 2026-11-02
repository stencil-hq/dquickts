/*
 * dquickts Python, milestone M2: interpreter support (unit a1).
 *
 * Out-of-line bodies of the OP_py subops (python/ceval_ops.h), the calling
 * convention (vectorcall over JS_CallInternal, CPython initialize_locals
 * argument binding), QPState creation/teardown, code metadata (inline
 * caches, location table) and the compile-time emit API used by the code
 * generator.  Error messages follow CPython 3.15 Python/ceval.c,
 * Python/bytecodes.c and Objects/call.c (PSF-2.0).
 */

/* ======================================================================= */
/* Python class registration (QP_CLASS rows of object.h)                    */
/* ======================================================================= */

/* finalizers and gc_mark functions are defined by their owner units, which
   are included after this file */
static JSClassFinalizer qp_type_finalizer, qp_instance_finalizer, qp_boxed_finalizer,
    qp_cfunction_finalizer, qp_descr_finalizer, qp_funcwrap_finalizer,
    qp_property_finalizer, qp_super_finalizer, qp_funcdata_finalizer,
    qp_method_finalizer, qp_cell_finalizer, qp_code_finalizer, qp_frame_finalizer,
    qp_complex_finalizer, qp_tuple_finalizer, qp_range_finalizer, qp_slice_finalizer,
    qp_iter_finalizer, qp_dict_finalizer, qp_set_finalizer, qp_dictiter_finalizer,
    qp_dictview_finalizer, qp_bytes_finalizer, qp_exc_finalizer, qp_tb_finalizer,
    qp_gen_finalizer, qp_genaux_finalizer, qp_module_finalizer;
static JSClassGCMark qp_type_mark, qp_instance_mark, qp_boxed_mark, qp_cfunction_mark,
    qp_descr_mark, qp_funcwrap_mark, qp_property_mark, qp_super_mark, qp_funcdata_mark,
    qp_method_mark, qp_cell_mark, qp_code_mark, qp_frame_mark, qp_tuple_mark,
    qp_range_mark, qp_slice_mark, qp_iter_mark, qp_dict_mark, qp_set_mark,
    qp_dictiter_mark, qp_dictview_mark, qp_bytes_mark, qp_exc_mark, qp_tb_mark,
    qp_gen_mark, qp_genaux_mark, qp_module_mark;

static JSClassShortDef const qp_class_def[] = {
#define QP_CLASS(id, finalizer, gc_mark) { JS_ATOM_Object, finalizer, gc_mark },
#include "object.h"
#undef QP_CLASS
};

/* ======================================================================= */
/* QPState                                                                  */
/* ======================================================================= */

/* M6 tracing (frameobject.c): leave / enter the interpreter trace hook */
static void qp_trace_dispatch(int on);
/* mod_misc.c: the signal handlers stop polling a freed context */
static void qp_signal_forget_ctx(JSContext *ctx);

static const char * const qp_atom_names[QP_ATOM__COUNT] = {
#define QP_ATOM_NAME(id) #id,
#define QP_ATOM_NAME_S(id, str) str,
    QP_ATOMS(QP_ATOM_NAME, QP_ATOM_NAME_S)
#undef QP_ATOM_NAME
#undef QP_ATOM_NAME_S
};

#ifdef CONFIG_PYTHON_CAPI
static int qp_capi_register_core_modules(void);
#endif

static int qp_state_init(JSContext *ctx, const QPConfig *cfg)
{
    JSRuntime *rt = ctx->rt;
    QPState *s;
    int i;

    if (ctx->py)
        return 0;
#ifdef CONFIG_PYTHON_CAPI
    /* CPython's self-contained C modules (capi/Setup.qp) */
    if (qp_capi_register_core_modules() < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
#endif
    if (!JS_IsRegisteredClass(rt, JS_CLASS_PY_FIRST)) {
        if (init_class_range(rt, qp_class_def, JS_CLASS_PY_FIRST, countof(qp_class_def)))
            return -1;
    }
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return -1;
    ctx->py = s;
    s->ctx = ctx;
    if (cfg) {
        s->config = *cfg;
    } else {
        s->config.int_max_str_digits = -1;
        s->config.lazy_imports = -1;
    }
    s->not_implemented = JS_UNDEFINED;
    s->ellipsis = JS_UNDEFINED;
    s->empty_tuple = JS_UNDEFINED;
    s->empty_frozenset = JS_UNDEFINED;
    s->empty_bytes = JS_UNDEFINED;
    s->builtins_module = JS_UNDEFINED;
    s->builtins_dict = JS_UNDEFINED;
    s->sys_module = JS_UNDEFINED;
    s->sys_dict = JS_UNDEFINED;
    s->typing_nodefault = JS_UNDEFINED;
    s->audit_hooks = JS_UNDEFINED;
    s->sys_structseq_types = JS_UNDEFINED;
    s->modules = JS_UNDEFINED;
    s->import_func = JS_UNDEFINED;
    s->importlib = JS_UNDEFINED;
    s->repr_stack = JS_UNDEFINED;
    s->pending_del = JS_UNDEFINED;
    s->asyncgen_firstiter = JS_UNDEFINED;
    s->asyncgen_finalizer = JS_UNDEFINED;
    s->trace_func = JS_UNDEFINED;
    s->profile_func = JS_UNDEFINED;
    s->recursion_depth = 0;
    s->recursion_limit = s->config.recursion_limit > 0 ? s->config.recursion_limit : 1000;
    s->exc_info_base.exc_value = JS_UNDEFINED;
    s->exc_info_base.previous = NULL;
    s->exc_info = &s->exc_info_base;
    s->next_type_version = 1;
    s->next_dict_uid = 1;
    s->lazy_imports_mode = s->config.lazy_imports;
    for(i = 0; i < QP_ATOM__COUNT; i++) {
        s->atoms[i] = JS_NewAtom(ctx, qp_atom_names[i]);
        if (s->atoms[i] == JS_ATOM_NULL)
            goto fail;
    }
    /* the str hash key must exist before the first dict is built (type
       dicts in qp_init_typeobject cache str hashes) */
    {
        qp_error err;
        memset(&err, 0, sizeof(err));
        if (qp_hash_key_init(s->config.hash_seed, s->hash_key, &err) < 0) {
            JS_ThrowInternalError(ctx, "%s", err.msg ? err.msg : "failed to initialize the hash key");
            qp_error_clear(&err);
            goto fail;
        }
    }
    /* init order of m2-design.md section 7 */
    if (qp_init_types_phase1(ctx) ||
        qp_init_typeobject(ctx) ||
        qp_init_numbers(ctx) ||
        qp_init_str(ctx) ||
        qp_init_sequences(ctx) ||
        qp_init_dicts(ctx) ||
        qp_init_bytes(ctx) ||
        qp_init_exceptions(ctx) ||
        qp_init_funcobject(ctx) ||
        qp_init_generators(ctx) ||
        qp_init_modules(ctx) ||
        qp_init_builtins(ctx))
        goto fail;
    return 0;
 fail:
    qp_state_free(ctx);
    ctx->py = NULL;
    return -1;
}

static void qp_state_free(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    QPState *s = ctx->py;
    QPExcStackItem *item;
    JSValue pending_del;
    int i;

    if (!s)
        return;
    qp_signal_forget_ctx(ctx);
    /* the objects freed from now on, cycles included, are not resurrected
       for their __del__ (qp_del_hook, qp_del_queue_cyclic) */
    pending_del = s->pending_del;
    s->pending_del = JS_UNDEFINED;
    JS_FreeValueRT(rt, pending_del);
    qp_threads_free(ctx);
    qp_str_state_free(ctx);
    qp_modstate_free(ctx);
    JS_FreeValueRT(rt, s->not_implemented);
    JS_FreeValueRT(rt, s->ellipsis);
    JS_FreeValueRT(rt, s->empty_tuple);
    JS_FreeValueRT(rt, s->empty_frozenset);
    JS_FreeValueRT(rt, s->empty_bytes);
    JS_FreeValueRT(rt, s->builtins_module);
    JS_FreeValueRT(rt, s->builtins_dict);
    JS_FreeValueRT(rt, s->sys_module);
    JS_FreeValueRT(rt, s->sys_dict);
    JS_FreeValueRT(rt, s->typing_nodefault);
    JS_FreeValueRT(rt, s->audit_hooks);
    JS_FreeValueRT(rt, s->sys_structseq_types);
    JS_FreeValueRT(rt, s->modules);
    JS_FreeValueRT(rt, s->import_func);
    JS_FreeValueRT(rt, s->importlib);
    JS_FreeValueRT(rt, s->repr_stack);
    JS_FreeValueRT(rt, s->asyncgen_firstiter);
    JS_FreeValueRT(rt, s->asyncgen_finalizer);
    /* M6 tracing: the dispatch leaves the trace hook with the last user */
    if (s->trace_active) {
        s->trace_active = 0;
        qp_trace_dispatch(0);
    }
    JS_FreeValueRT(rt, s->trace_func);
    JS_FreeValueRT(rt, s->profile_func);
    s->trace_func = s->profile_func = JS_UNDEFINED;
    for(item = s->exc_info; item; item = item->previous) {
        if (item == &s->exc_info_base)
            break;
    }
    JS_FreeValueRT(rt, s->exc_info_base.exc_value);
    s->exc_info_base.exc_value = JS_UNDEFINED;
    for(i = 0; i < QP_EXC__COUNT; i++) {
        if (s->exc_types[i]) {
            JS_FreeValueRT(rt, QP_TYPE_VALUE(s->exc_types[i]));
            s->exc_types[i] = NULL;
        }
    }
    for(i = 0; i < QP_T__COUNT; i++) {
        if (s->types[i]) {
            JS_FreeValueRT(rt, QP_TYPE_VALUE(s->types[i]));
            s->types[i] = NULL;
        }
    }
    for(i = 0; i < QP_ATOM__COUNT; i++)
        JS_FreeAtomRT(rt, s->atoms[i]);
    if (s->atom_dict_slot != JS_ATOM_NULL)
        JS_FreeAtomRT(rt, s->atom_dict_slot);
    js_free_rt(rt, s);
    ctx->py = NULL;
}

static void qp_state_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func)
{
    QPExcStackItem *item;
    int i;

    qp_modstate_mark(rt, s, mark_func);
    JS_MarkValue(rt, s->not_implemented, mark_func);
    JS_MarkValue(rt, s->ellipsis, mark_func);
    JS_MarkValue(rt, s->empty_tuple, mark_func);
    JS_MarkValue(rt, s->empty_frozenset, mark_func);
    JS_MarkValue(rt, s->empty_bytes, mark_func);
    JS_MarkValue(rt, s->builtins_module, mark_func);
    JS_MarkValue(rt, s->builtins_dict, mark_func);
    JS_MarkValue(rt, s->sys_module, mark_func);
    JS_MarkValue(rt, s->sys_dict, mark_func);
    JS_MarkValue(rt, s->typing_nodefault, mark_func);
    JS_MarkValue(rt, s->audit_hooks, mark_func);
    JS_MarkValue(rt, s->sys_structseq_types, mark_func);
    JS_MarkValue(rt, s->modules, mark_func);
    JS_MarkValue(rt, s->import_func, mark_func);
    JS_MarkValue(rt, s->importlib, mark_func);
    JS_MarkValue(rt, s->repr_stack, mark_func);
    JS_MarkValue(rt, s->pending_del, mark_func);
    JS_MarkValue(rt, s->asyncgen_firstiter, mark_func);
    JS_MarkValue(rt, s->asyncgen_finalizer, mark_func);
    JS_MarkValue(rt, s->trace_func, mark_func);
    JS_MarkValue(rt, s->profile_func, mark_func);
    /* the generator items are marked by their generators */
    for(item = s->exc_info; item; item = item->previous) {
        if (item == &s->exc_info_base)
            break;
    }
    JS_MarkValue(rt, s->exc_info_base.exc_value, mark_func);
    for(i = 0; i < QP_EXC__COUNT; i++) {
        if (s->exc_types[i])
            JS_MarkValue(rt, QP_TYPE_VALUE(s->exc_types[i]), mark_func);
    }
    for(i = 0; i < QP_T__COUNT; i++) {
        if (s->types[i])
            JS_MarkValue(rt, QP_TYPE_VALUE(s->types[i]), mark_func);
    }
}

/* ======================================================================= */
/* Code metadata                                                            */
/* ======================================================================= */

enum {
    QP_IC_KIND_NONE = 0,
    QP_IC_KIND_ATTR = 1,
    QP_IC_KIND_GLOBAL = 2,
};

/* kind of each inline cache, stored after ic[ic_count] */
#define QP_IC_KINDS(ci) ((uint8_t *)((ci)->ic + (ci)->ic_count))

static QPCodeInfo *qp_code_info(JSFunctionBytecode *b)
{
    return b->is_py ? b->py_code : NULL;
}

static QPCodeInfo *qp_code_info_new(JSContext *ctx)
{
    QPCodeInfo *ci = js_mallocz(ctx, sizeof(*ci));
    if (!ci)
        return NULL;
    ci->firstlineno = 1;
    ci->qualname = JS_ATOM_NULL;
    ci->doc_cpool_idx = -1;
    return ci;
}

static void qp_code_info_free(JSRuntime *rt, QPCodeInfo *ci)
{
    if (!ci)
        return;
    if (ci->ic) {
        uint8_t *kinds = QP_IC_KINDS(ci);
        int i;
        for(i = 0; i < ci->ic_count; i++) {
            if (kinds[i] == QP_IC_KIND_ATTR && ci->ic[i].attr.shape)
                js_free_shape(rt, ci->ic[i].attr.shape);
        }
        js_free_rt(rt, ci->ic);
    }
    if (ci->qualname != JS_ATOM_NULL)
        JS_FreeAtomRT(rt, ci->qualname);
    js_free_rt(rt, ci->locs);
    js_free_rt(rt, ci->pcloc);
    js_free_rt(rt, ci);
}

static void qp_code_info_mark(JSRuntime *rt, QPCodeInfo *ci, JS_MarkFunc *mark_func)
{
    if (ci->ic) {
        uint8_t *kinds = QP_IC_KINDS(ci);
        int i;
        for(i = 0; i < ci->ic_count; i++) {
            if (kinds[i] == QP_IC_KIND_ATTR && ci->ic[i].attr.shape)
                mark_func(rt, &ci->ic[i].attr.shape->header);
        }
    }
}

static int qp_code_info_setup_ic(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    const uint8_t *bc = b->byte_code_buf;
    int pos, len = b->byte_code_len, opnd, idx;
    const JSOpCode *oi;
    JSOpCode oi_tmp;
    uint8_t *kinds;

    if (!ci || ci->ic || ci->ic_count == 0)
        return 0;
    ci->ic = js_mallocz(ctx, (sizeof(QPInlineCache) + 1) * ci->ic_count);
    if (!ci->ic)
        return -1;
    kinds = QP_IC_KINDS(ci);
    for(pos = 0; pos < len; pos += oi->size) {
        oi = js_get_op_info(bc, len, pos, TRUE, &oi_tmp, &opnd);
        if (bc[pos] != OP_py)
            continue;
        switch(bc[pos + 1]) {
        case QP_OP_getattr:
        case QP_OP_load_method:
        case QP_OP_setattr:
            idx = get_u16(bc + opnd + 4);
            if (idx < ci->ic_count)
                kinds[idx] = QP_IC_KIND_ATTR;
            break;
        case QP_OP_load_global:
        case QP_OP_load_from_dict_or_globals:
            idx = get_u16(bc + opnd + 4);
            if (idx < ci->ic_count)
                kinds[idx] = QP_IC_KIND_GLOBAL;
            break;
        default:
            break;
        }
    }
    return 0;
}

static int qp_code_addr2loc(JSFunctionBytecode *b, uint32_t pc, QPLoc *loc)
{
    QPCodeInfo *ci = qp_code_info(b);
    int lo, hi, mid, found = -1;

    loc->lineno = loc->end_lineno = loc->col = loc->end_col = -1;
    if (!ci || ci->pcloc_count == 0)
        return -1;
    lo = 0;
    hi = ci->pcloc_count - 1;
    while (lo <= hi) {
        mid = (lo + hi) >> 1;
        if (ci->pcloc[mid].pc <= pc) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0)
        return -1;
    *loc = ci->locs[ci->pcloc[found].loc];
    return 0;
}

/* ======================================================================= */
/* Compile-time API (unit b)                                                */
/* ======================================================================= */

static QPCodeInfo *qp_fd_code_info(JSContext *ctx, JSFunctionDef *fd)
{
    if (!fd->py_code) {
        fd->py_code = qp_code_info_new(ctx);
        if (fd->py_code)
            fd->is_py = TRUE;
    }
    return fd->py_code;
}

static int qp_fd_add_loc(JSContext *ctx, JSFunctionDef *fd, const QPLoc *loc)
{
    QPCodeInfo *ci = qp_fd_code_info(ctx, fd);
    if (!ci)
        return -1;
    if (ci->loc_count >= ci->loc_size) {
        uint32_t new_size = max_int(16, ci->loc_size * 3 / 2);
        QPLoc *p = js_realloc(ctx, ci->locs, sizeof(QPLoc) * new_size);
        if (!p)
            return -1;
        ci->locs = p;
        ci->loc_size = new_size;
    }
    ci->locs[ci->loc_count] = *loc;
    return ci->loc_count++;
}

static void qp_emit_py(JSParseState *s, int subop)
{
    emit_op(s, OP_py);
    emit_u8(s, subop);
}

static void qp_emit_py_u8(JSParseState *s, int subop, uint8_t v)
{
    qp_emit_py(s, subop);
    emit_u8(s, v);
}

static void qp_emit_py_u16(JSParseState *s, int subop, uint16_t v)
{
    qp_emit_py(s, subop);
    emit_u16(s, v);
}

static void qp_emit_py_atom(JSParseState *s, int subop, JSAtom atom)
{
    qp_emit_py(s, subop);
    emit_atom(s, atom);
}

static void qp_emit_py_atom_u8(JSParseState *s, int subop, JSAtom atom, uint8_t v)
{
    qp_emit_py(s, subop);
    emit_atom(s, atom);
    emit_u8(s, v);
}

static void qp_emit_py_atom_u16(JSParseState *s, int subop, JSAtom atom, uint16_t v)
{
    qp_emit_py(s, subop);
    emit_atom(s, atom);
    emit_u16(s, v);
}

static void qp_emit_py_atom_ic(JSParseState *s, int subop, JSAtom atom)
{
    QPCodeInfo *ci = qp_fd_code_info(s->ctx, s->cur_func);
    int idx = 0;
    if (!ci || ci->ic_count == 0xffff) {
        dbuf_set_error(&s->cur_func->byte_code);
    } else {
        idx = ci->ic_count++;
    }
    qp_emit_py_atom_u16(s, subop, atom, idx);
}

static int qp_emit_py_label(JSParseState *s, int subop, int label)
{
    if (label < 0) {
        label = new_label(s);
        if (label < 0)
            return -1;
    }
    qp_emit_py(s, subop);
    emit_u32(s, label);
    s->cur_func->label_slots[label].ref_count++;
    return label;
}

static JSValue qp_create_function(JSContext *ctx, JSFunctionDef *fd)
{
    fd->is_py = TRUE;
    return js_create_function(ctx, fd);
}

/* ======================================================================= */
/* Frames                                                                   */
/* ======================================================================= */

static JSFunctionBytecode *qp_frame_py_bytecode(JSStackFrame *sf)
{
    JSObject *p;
    if (!sf || JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION)
        return NULL;
    return p->u.func.function_bytecode->is_py ? p->u.func.function_bytecode : NULL;
}

static JSStackFrame *qp_py_frame_at(JSContext *ctx, int depth)
{
    JSStackFrame *sf;
    for(sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        if (qp_frame_py_bytecode(sf)) {
            if (depth == 0)
                return sf;
            depth--;
        }
    }
    return NULL;
}

static JSValue qp_frame_globals(JSStackFrame *sf)
{
    QPFuncData *fd;
    if (!qp_frame_py_bytecode(sf))
        return QP_NULL;
    fd = qp_function_data(sf->cur_func);
    return fd ? fd->globals : QP_NULL;
}

static JSValue qp_frame_builtins(JSStackFrame *sf)
{
    QPFuncData *fd;
    if (!qp_frame_py_bytecode(sf))
        return QP_NULL;
    fd = qp_function_data(sf->cur_func);
    return fd ? fd->builtins : QP_NULL;
}

/* ======================================================================= */
/* Calls                                                                    */
/* ======================================================================= */

static JSValue qp_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    int nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    int flags = JS_CALL_FLAG_COPY_ARGV;

    if (JS_VALUE_GET_TAG(callable) != JS_TAG_OBJECT ||
        (!QP_IS_PY_CLASS(JS_VALUE_GET_OBJ(callable)->class_id) &&
         !JS_IsFunction(ctx, callable)))
        return qp_raise_type_error(ctx, "'%T' object is not callable", callable);
    if (nkw)
        flags |= JS_CALL_FLAG_PY_KW;
    if (nargsf & QP_VECTORCALL_ARGUMENTS_OFFSET)
        flags |= JS_CALL_FLAG_PY_ARGS_OFFSET;
    return JS_CallInternal(ctx, callable, JS_UNDEFINED, nkw ? kwnames : JS_UNDEFINED,
                           nargs + nkw, (JSValue *)args, flags);
}

static JSValue qp_call0(JSContext *ctx, JSValueConst callable)
{
    return qp_vectorcall(ctx, callable, NULL, 0, QP_NULL);
}

static JSValue qp_call1(JSContext *ctx, JSValueConst callable, JSValueConst a)
{
    JSValueConst args[2];
    args[1] = a;
    return qp_vectorcall(ctx, callable, args + 1, 1 | QP_VECTORCALL_ARGUMENTS_OFFSET, QP_NULL);
}

static JSValue qp_call2(JSContext *ctx, JSValueConst callable, JSValueConst a, JSValueConst b)
{
    JSValueConst args[3];
    args[1] = a;
    args[2] = b;
    return qp_vectorcall(ctx, callable, args + 1, 2 | QP_VECTORCALL_ARGUMENTS_OFFSET, QP_NULL);
}

static JSValue qp_call_tuple(JSContext *ctx, JSValueConst callable, JSValueConst args_tuple,
                             JSValueConst kwargs)
{
    Py_ssize_t n = QP_TUPLE_SIZE(args_tuple), nk = 0, pos, i;
    JSValue *arr, *keys = NULL, kwnames = QP_NULL, ret, key, value;

    if (!QP_IS_NULL(kwargs) && !JS_IsUndefined(kwargs))
        nk = qp_dict_size(kwargs);
    arr = js_malloc(ctx, sizeof(JSValue) * (n + nk + 1));
    if (!arr)
        return JS_EXCEPTION;
    memcpy(arr + 1, QP_TUPLE_ITEMS(args_tuple), sizeof(JSValue) * n);
    if (nk) {
        keys = js_malloc(ctx, sizeof(JSValue) * nk);
        if (!keys) {
            js_free(ctx, arr);
            return JS_EXCEPTION;
        }
        pos = 0;
        i = 0;
        while (i < nk && qp_dict_next(kwargs, &pos, &key, &value)) {
            if (!qp_str_check(ctx, key)) {
                js_free(ctx, keys);
                js_free(ctx, arr);
                return qp_raise_type_error(ctx, "keywords must be strings");
            }
            keys[i] = key;
            arr[1 + n + i] = value;
            i++;
        }
        kwnames = qp_tuple_pack(ctx, nk, (JSValueConst *)keys);
        js_free(ctx, keys);
        if (JS_IsException(kwnames)) {
            js_free(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    ret = qp_vectorcall(ctx, callable, (JSValueConst *)arr + 1,
                        n | QP_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    JS_FreeValue(ctx, kwnames);
    js_free(ctx, arr);
    return ret;
}

static JSValue qp_call_object(JSContext *ctx, JSValueConst callable, JSValueConst *argv,
                              int argc, JSValueConst kwnames, int flags)
{
    QPType *t = qp_type_of(ctx, callable);
    size_t nargsf;
    int nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);

    if (!t->slots.tp_call)
        return qp_raise_type_error(ctx, "'%T' object is not callable", callable);
    /* _PyObject_MakeTpCall: _Py_EnterRecursiveCall(" while calling a Python
       object"); slot recursions (A.__call__ = A(), __repr__ = __str__) have
       no Python frame to hit the recursion limit */
    if (js_check_stack_overflow(ctx->rt, 0))
        return qp_raise_recursion_error(ctx);
    nargsf = argc - nkw;
    if (flags & JS_CALL_FLAG_PY_ARGS_OFFSET)
        nargsf |= QP_VECTORCALL_ARGUMENTS_OFFSET;
    return t->slots.tp_call(ctx, callable, argv, nargsf, nkw ? kwnames : QP_NULL);
}

static JSValue qp_call_js_kw(JSContext *ctx, JSValueConst func, JSValueConst this_obj,
                             int argc, JSValueConst *argv, JSValueConst kwnames, int flags)
{
    int nkw = (int)QP_TUPLE_SIZE(kwnames), npos = argc - nkw, i;
    JSValue obj, *arr, ret;
    JSAtom atom;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    for(i = 0; i < nkw; i++) {
        atom = qp_str_to_atom(ctx, QP_TUPLE_ITEMS(kwnames)[i]);
        if (atom == JS_ATOM_NULL)
            goto fail;
        if (JS_DefinePropertyValue(ctx, obj, atom, JS_DupValue(ctx, argv[npos + i]),
                                   JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        JS_FreeAtom(ctx, atom);
    }
    arr = js_malloc(ctx, sizeof(JSValue) * (npos + 1));
    if (!arr)
        goto fail;
    for(i = 0; i < npos; i++)
        arr[i] = (JSValue)argv[i];  /* borrowed: JS_CALL_FLAG_COPY_ARGV */
    arr[npos] = obj;
    flags &= ~(JS_CALL_FLAG_PY_KW | JS_CALL_FLAG_PY_ARGS_OFFSET | JS_CALL_FLAG_DIRECT);
    ret = JS_CallInternal(ctx, func, this_obj, JS_UNDEFINED, npos + 1, arr,
                          flags | JS_CALL_FLAG_COPY_ARGV);
    js_free(ctx, arr);
    JS_FreeValue(ctx, obj);
    return ret;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* ---- argument binding (CPython initialize_locals) --------------------- */

static JSValue qp_bind_qualname(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b)
{
    QPFuncData *fd = qp_function_data(func);
    if (fd && qp_str_check(ctx, fd->qualname))
        return JS_DupValue(ctx, fd->qualname);
    return qp_str_from_atom(ctx, b->func_name);
}

/* missing_arguments() + format_missing() */
static void qp_bind_missing(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                            JSValue *arg_buf, int missing, int defcount)
{
    QPCodeInfo *ci = b->py_code;
    BOOL positional = (defcount != -1);
    int start, end, i, j = 0;
    JSValue *names, qualname, name_str = JS_EXCEPTION, tmp, sep;

    names = js_mallocz(ctx, sizeof(JSValue) * missing);
    if (!names)
        return;
    if (positional) {
        start = 0;
        end = ci->argcount - defcount;
    } else {
        start = ci->argcount;
        end = start + ci->kwonlyargcount;
    }
    for(i = start; i < end && j < missing; i++) {
        if (QP_IS_NULL(arg_buf[i])) {
            JSValue raw = qp_str_from_atom(ctx, b->vardefs[i].var_name);
            if (JS_IsException(raw))
                goto done;
            names[j] = qp_repr(ctx, raw);
            JS_FreeValue(ctx, raw);
            if (JS_IsException(names[j]))
                goto done;
            j++;
        }
    }
    switch(j) {
    case 1:
        name_str = JS_DupValue(ctx, names[0]);
        break;
    case 2:
        name_str = qp_str_from_format(ctx, "%U and %U", names[0], names[1]);
        break;
    default:
        sep = JS_NewString(ctx, ", ");
        if (JS_IsException(sep))
            goto done;
        tmp = qp_str_join_array(ctx, sep, j - 2, (JSValueConst *)names);
        JS_FreeValue(ctx, sep);
        if (JS_IsException(tmp))
            goto done;
        name_str = qp_str_from_format(ctx, "%U, %U, and %U", tmp, names[j - 2], names[j - 1]);
        JS_FreeValue(ctx, tmp);
        break;
    }
    if (JS_IsException(name_str))
        goto done;
    qualname = qp_bind_qualname(ctx, func, b);
    if (!JS_IsException(qualname)) {
        qp_raise_type_error(ctx, "%U() missing %d required %s argument%s: %U",
                            qualname, j, positional ? "positional" : "keyword-only",
                            j == 1 ? "" : "s", name_str);
        JS_FreeValue(ctx, qualname);
    }
    JS_FreeValue(ctx, name_str);
 done:
    for(i = 0; i < missing; i++)
        JS_FreeValue(ctx, names[i]);
    js_free(ctx, names);
}

static void qp_bind_too_many(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                             JSValue *arg_buf, int given, int defcount)
{
    QPCodeInfo *ci = b->py_code;
    int co_argcount = ci->argcount, kwonly_given = 0, i, plural;
    JSValue sig, kwonly_sig, qualname;

    for(i = co_argcount; i < co_argcount + ci->kwonlyargcount; i++) {
        if (!QP_IS_NULL(arg_buf[i]))
            kwonly_given++;
    }
    if (defcount) {
        plural = 1;
        sig = qp_str_from_format(ctx, "from %d to %d", co_argcount - defcount, co_argcount);
    } else {
        plural = (co_argcount != 1);
        sig = qp_str_from_format(ctx, "%d", co_argcount);
    }
    if (JS_IsException(sig))
        return;
    if (kwonly_given) {
        kwonly_sig = qp_str_from_format(ctx, " positional argument%s (and %d keyword-only argument%s)",
                                        given != 1 ? "s" : "", kwonly_given,
                                        kwonly_given != 1 ? "s" : "");
    } else {
        kwonly_sig = JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    if (JS_IsException(kwonly_sig)) {
        JS_FreeValue(ctx, sig);
        return;
    }
    qualname = qp_bind_qualname(ctx, func, b);
    if (!JS_IsException(qualname)) {
        qp_raise_type_error(ctx, "%U() takes %U positional argument%s but %d%U %s given",
                            qualname, sig, plural ? "s" : "", given, kwonly_sig,
                            given == 1 && !kwonly_given ? "was" : "were");
        JS_FreeValue(ctx, qualname);
    }
    JS_FreeValue(ctx, sig);
    JS_FreeValue(ctx, kwonly_sig);
}

/* positional_only_passed_as_keyword(): 1 if the error was raised (or
   another error occurred), 0 if there is no conflict */
static int qp_bind_posonly_conflict(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                                    JSValueConst kwnames)
{
    QPCodeInfo *ci = b->py_code;
    int kwcount = (int)QP_TUPLE_SIZE(kwnames), k, k2, n = 0;
    JSValue *names, sep, joined, qualname;
    JSAtom atom;

    names = js_mallocz(ctx, sizeof(JSValue) * (ci->posonlyargcount * kwcount + 1));
    if (!names)
        return 1;
    for(k = 0; k < ci->posonlyargcount; k++) {
        for(k2 = 0; k2 < kwcount; k2++) {
            JSValueConst kw = QP_TUPLE_ITEMS(kwnames)[k2];
            atom = qp_str_to_atom(ctx, kw);
            if (atom == JS_ATOM_NULL)
                goto fail;
            if (atom == b->vardefs[k].var_name)
                names[n++] = JS_DupValue(ctx, kw);
            JS_FreeAtom(ctx, atom);
        }
    }
    if (n == 0) {
        js_free(ctx, names);
        return 0;
    }
    sep = JS_NewString(ctx, ", ");
    if (JS_IsException(sep))
        goto fail;
    joined = qp_str_join_array(ctx, sep, n, (JSValueConst *)names);
    JS_FreeValue(ctx, sep);
    if (JS_IsException(joined))
        goto fail;
    qualname = qp_bind_qualname(ctx, func, b);
    if (!JS_IsException(qualname)) {
        qp_raise_type_error(ctx, "%U() got some positional-only arguments passed"
                            " as keyword arguments: '%U'", qualname, joined);
        JS_FreeValue(ctx, qualname);
    }
    JS_FreeValue(ctx, joined);
 fail:
    for(k = 0; k < n; k++)
        JS_FreeValue(ctx, names[k]);
    js_free(ctx, names);
    return 1;
}

static void qp_bind_unexpected_kw(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                                  JSValueConst keyword)
{
    QPCodeInfo *ci = b->py_code;
    int total_args = ci->argcount + ci->kwonlyargcount;
    int ncands = total_args - ci->posonlyargcount, i, sug = -1;
    const char **cands = NULL;
    const char *kwstr;
    JSValue qualname;

    kwstr = JS_ToCString(ctx, keyword);
    if (ncands > 0 && kwstr) {
        cands = js_mallocz(ctx, sizeof(cands[0]) * ncands);
        if (cands) {
            for(i = 0; i < ncands; i++) {
                cands[i] = JS_AtomToCString(ctx, b->vardefs[ci->posonlyargcount + i].var_name);
                if (!cands[i])
                    break;
            }
            if (i == ncands)
                sug = qp_suggest_keyword(kwstr, cands, ncands);
        }
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); /* suggestion errors are ignored */
    qualname = qp_bind_qualname(ctx, func, b);
    if (!JS_IsException(qualname)) {
        if (sug >= 0) {
            JSValue s = qp_str_from_atom(ctx, b->vardefs[ci->posonlyargcount + sug].var_name);
            if (!JS_IsException(s)) {
                qp_raise_type_error(ctx, "%U() got an unexpected keyword argument '%S'."
                                    " Did you mean '%S'?", qualname, keyword, s);
                JS_FreeValue(ctx, s);
            }
        } else {
            qp_raise_type_error(ctx, "%U() got an unexpected keyword argument '%S'",
                                qualname, keyword);
        }
        JS_FreeValue(ctx, qualname);
    }
    if (cands) {
        for(i = 0; i < ncands; i++) {
            if (cands[i])
                JS_FreeCString(ctx, cands[i]);
        }
        js_free(ctx, cands);
    }
    if (kwstr)
        JS_FreeCString(ctx, kwstr);
}

static int qp_bind_args(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                        JSValueConst *args, int nargs, JSValueConst kwnames, JSValue *arg_buf)
{
    QPCodeInfo *ci = b->py_code;
    QPFuncData *fdata = qp_function_data(func);
    int co_argcount = ci->argcount;
    int total_args = co_argcount + ci->kwonlyargcount;
    int i, j, n, kwcount, defcount;
    JSValue kwdict = QP_NULL;
    JSAtom atom;

    for(i = 0; i < b->arg_count; i++)
        arg_buf[i] = QP_NULL;
    if (ci->co_flags & CO_VARKEYWORDS) {
        kwdict = qp_dict_new(ctx);
        if (JS_IsException(kwdict))
            goto fail;
        arg_buf[total_args + ((ci->co_flags & CO_VARARGS) ? 1 : 0)] = kwdict;
    }
    /* positional arguments */
    n = min_int(nargs, co_argcount);
    for(j = 0; j < n; j++)
        arg_buf[j] = JS_DupValue(ctx, args[j]);
    if (ci->co_flags & CO_VARARGS) {
        JSValue u;
        if (nargs == n)
            u = JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
        else
            u = qp_tuple_pack(ctx, nargs - n, args + n);
        if (JS_IsException(u))
            goto fail;
        arg_buf[total_args] = u;
    }
    /* keyword arguments */
    kwcount = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    for(i = 0; i < kwcount; i++) {
        JSValueConst keyword = QP_TUPLE_ITEMS(kwnames)[i];
        JSValueConst value = args[nargs + i];
        if (!qp_str_check(ctx, keyword)) {
            JSValue qualname = qp_bind_qualname(ctx, func, b);
            if (!JS_IsException(qualname)) {
                qp_raise_type_error(ctx, "%U() keywords must be strings", qualname);
                JS_FreeValue(ctx, qualname);
            }
            goto fail;
        }
        atom = qp_str_to_atom(ctx, keyword);
        if (atom == JS_ATOM_NULL)
            goto fail;
        for(j = ci->posonlyargcount; j < total_args; j++) {
            if (b->vardefs[j].var_name == atom)
                break;
        }
        JS_FreeAtom(ctx, atom);
        if (j < total_args) {
            if (!QP_IS_NULL(arg_buf[j])) {
                JSValue qualname = qp_bind_qualname(ctx, func, b);
                if (!JS_IsException(qualname)) {
                    qp_raise_type_error(ctx, "%U() got multiple values for argument '%S'",
                                        qualname, keyword);
                    JS_FreeValue(ctx, qualname);
                }
                goto fail;
            }
            arg_buf[j] = JS_DupValue(ctx, value);
            continue;
        }
        if (QP_IS_NULL(kwdict)) {
            if (ci->posonlyargcount &&
                qp_bind_posonly_conflict(ctx, func, b, kwnames))
                goto fail;
            qp_bind_unexpected_kw(ctx, func, b, keyword);
            goto fail;
        }
        if (qp_dict_set_item(ctx, kwdict, keyword, value) < 0)
            goto fail;
    }
    /* number of positional arguments */
    defcount = 0;
    if (fdata && qp_tuple_check(ctx, fdata->defaults))
        defcount = (int)QP_TUPLE_SIZE(fdata->defaults);
    if (nargs > co_argcount && !(ci->co_flags & CO_VARARGS)) {
        qp_bind_too_many(ctx, func, b, arg_buf, nargs, defcount);
        goto fail;
    }
    /* missing positional arguments: defaults */
    if (nargs < co_argcount) {
        int m = co_argcount - defcount, missing = 0;
        for(i = nargs; i < m; i++) {
            if (QP_IS_NULL(arg_buf[i]))
                missing++;
        }
        if (missing) {
            qp_bind_missing(ctx, func, b, arg_buf, missing, defcount);
            goto fail;
        }
        i = n > m ? n - m : 0;
        for(; i < defcount; i++) {
            if (QP_IS_NULL(arg_buf[m + i]))
                arg_buf[m + i] = JS_DupValue(ctx, QP_TUPLE_ITEMS(fdata->defaults)[i]);
        }
    }
    /* keyword-only arguments: kwdefaults */
    if (ci->kwonlyargcount > 0) {
        int missing = 0;
        for(i = co_argcount; i < total_args; i++) {
            JSValue def;
            int r;
            if (!QP_IS_NULL(arg_buf[i]))
                continue;
            if (fdata && qp_anydict_check(ctx, fdata->kwdefaults)) {
                r = qp_dict_get_item_atom(ctx, fdata->kwdefaults, b->vardefs[i].var_name, &def);
                if (r < 0)
                    goto fail;
                if (r) {
                    arg_buf[i] = def;
                    continue;
                }
            }
            missing++;
        }
        if (missing) {
            qp_bind_missing(ctx, func, b, arg_buf, missing, -1);
            goto fail;
        }
    }
    /* _PyFunction_Vectorcall: a function over module/class code (not
       CO_OPTIMIZED, its hidden argument is the locals mapping) runs with
       its globals as locals */
    if (!(ci->co_flags & CO_OPTIMIZED) && fdata && b->arg_count > 0 &&
        QP_IS_NULL(arg_buf[0]))
        arg_buf[0] = JS_DupValue(ctx, fdata->globals);
    return 0;
 fail:
    for(i = 0; i < b->arg_count; i++) {
        JS_FreeValue(ctx, arg_buf[i]);
        arg_buf[i] = QP_NULL;
    }
    return -1;
}

static JSValue qp_call_with_locals(JSContext *ctx, JSValueConst func, JSValueConst locals)
{
    JSValueConst arg = locals;
    return JS_CallInternal(ctx, func, JS_UNDEFINED, JS_UNDEFINED, 1, (JSValue *)&arg,
                           JS_CALL_FLAG_COPY_ARGV);
}

static JSValue qp_eval_code_closure(JSContext *ctx, JSValueConst code, JSValueConst globals,
                                    JSValueConst locals, JSValueConst closure)
{
    JSFunctionBytecode *b = qp_code_bytecode(code);
    QPState *qs = QP_STATE(ctx);
    JSValue func, ret;
    JSObject *p;
    int r, i;

    if (!b || !b->is_py)
        return qp_raise_type_error(ctx, "expected a code object");
    /* insert __builtins__ like CPython when missing */
    if (qp_dict_check_exact(ctx, globals)) {
        JSValue v;
        r = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__), &v);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0) {
            if (qp_dict_set_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__),
                                      qs->builtins_module) < 0)
                return JS_EXCEPTION;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    func = qp_function_new(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b), globals);
    if (JS_IsException(func))
        return func;
    if (!QP_IS_NULL(closure) && !JS_IsUndefined(closure)) {
        p = JS_VALUE_GET_OBJ(func);
        for(i = 0; i < b->closure_var_count && i < QP_TUPLE_SIZE(closure); i++) {
            QPCell *cell = JS_GetOpaque(QP_TUPLE_ITEMS(closure)[i], JS_CLASS_PY_CELL);
            if (!cell || !cell->var_ref)
                continue;
            if (p->u.func.var_refs[i])
                free_var_ref(ctx->rt, p->u.func.var_refs[i]);
            js_rc(cell->var_ref)->ref_count++;
            p->u.func.var_refs[i] = cell->var_ref;
        }
    }
    if (b->py_code->co_flags & CO_NEWLOCALS) {
        ret = JS_CallInternal(ctx, func, JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                              JS_CALL_FLAG_COPY_ARGV);
    } else {
        ret = qp_call_with_locals(ctx, func,
                                  (QP_IS_NULL(locals) || JS_IsUndefined(locals)) ?
                                  globals : locals);
    }
    JS_FreeValue(ctx, func);
    return ret;
}

static JSValue qp_eval_code(JSContext *ctx, JSValueConst code, JSValueConst globals,
                            JSValueConst locals)
{
    return qp_eval_code_closure(ctx, code, globals, locals, QP_NULL);
}

/* ======================================================================= */
/* Subop helpers (python/ceval_ops.h)                                       */
/* ======================================================================= */

static BOOL qp_ceval_clear_if(JSContext *ctx, QPType *type)
{
    if (qp_exc_pending_matches(ctx, type)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return TRUE;
    }
    return FALSE;
}

static JSValue qp_eval_get_builtin(JSContext *ctx, JSAtom name)
{
    JSValue builtins = qp_frame_builtins(qp_py_frame_at(ctx, 0));
    JSValue key, v;
    int r;

    if (QP_IS_NULL(builtins) || JS_IsUndefined(builtins))
        builtins = QP_STATE(ctx)->builtins_dict;
    /* PyMapping_GetOptionalItem */
    if (qp_type_of(ctx, builtins) == QP_TYPE(ctx, dict)) {
        r = qp_dict_get_item_atom(ctx, builtins, name, &v);
        if (r != 0)
            return r > 0 ? v : JS_EXCEPTION;
    } else {
        key = qp_str_from_atom(ctx, name);
        if (JS_IsException(key))
            return key;
        v = qp_getitem(ctx, builtins, key);
        JS_FreeValue(ctx, key);
        if (!JS_IsException(v))
            return v;
        if (!qp_ceval_clear_if(ctx, QP_EXC(ctx, KeyError)))
            return JS_EXCEPTION;
    }
    key = qp_str_from_atom(ctx, name);
    if (JS_IsException(key))
        return key;
    qp_raise_value(ctx, QP_EXC(ctx, AttributeError), key);
    JS_FreeValue(ctx, key);
    return JS_EXCEPTION;
}

static JSValue qp_ceval_builtin(JSContext *ctx, const char *name)
{
    QPState *qs = QP_STATE(ctx);
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue v;
    int r;
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, qs->builtins_dict, atom, &v);
    if (r == 0)
        qp_raise_name_error(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return r > 0 ? v : JS_EXCEPTION;
}

static JSValue qp_load_common_const(JSContext *ctx, JSStackFrame *sf, int id)
{
    switch(id) {
    case QP_COMMON_CONST_ASSERTIONERROR:
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, AssertionError)));
    case QP_COMMON_CONST_NOTIMPLEMENTEDERROR:
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, NotImplementedError)));
    case QP_COMMON_CONST_BUILTIN_TUPLE:
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, tuple)));
    case QP_COMMON_CONST_BUILTIN_LIST:
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, list)));
    case QP_COMMON_CONST_BUILTIN_SET:
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, set)));
    case QP_COMMON_CONST_BUILTIN_ALL:
        return qp_ceval_builtin(ctx, "all");
    case QP_COMMON_CONST_BUILTIN_ANY:
        return qp_ceval_builtin(ctx, "any");
    case QP_COMMON_CONST_NONE:
        return JS_UNDEFINED;
    case QP_COMMON_CONST_EMPTY_STR:
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    case QP_COMMON_CONST_TRUE:
        return JS_TRUE;
    case QP_COMMON_CONST_FALSE:
        return JS_FALSE;
    case QP_COMMON_CONST_MINUS_ONE:
        return JS_NewInt32(ctx, -1);
    default:
        return qp_raise_system_error(ctx, "invalid common constant %d", id);
    }
}

static int qp_ceval_mapping_get(JSContext *ctx, JSValueConst mapping, JSAtom atom,
                                JSValue *pv)
{
    JSValue key, v;

    if (qp_dict_check_exact(ctx, mapping))
        return qp_dict_get_item_atom(ctx, mapping, atom, pv);
    *pv = QP_NULL;
    key = qp_str_from_atom(ctx, atom);
    if (JS_IsException(key))
        return -1;
    v = qp_getitem(ctx, mapping, key);
    JS_FreeValue(ctx, key);
    if (JS_IsException(v)) {
        if (qp_ceval_clear_if(ctx, QP_EXC(ctx, KeyError)))
            return 0;
        return -1;
    }
    *pv = v;
    return 1;
}

static int qp_ceval_mapping_set(JSContext *ctx, JSValueConst mapping, JSAtom atom,
                                JSValueConst v)
{
    JSValue key;
    int r;
    if (qp_dict_check_exact(ctx, mapping))
        return qp_dict_set_item_atom(ctx, mapping, atom, v);
    key = qp_str_from_atom(ctx, atom);
    if (JS_IsException(key))
        return -1;
    r = qp_setitem(ctx, mapping, key, v);
    JS_FreeValue(ctx, key);
    return r;
}

static JSValue qp_ceval_resolve_lazy(JSContext *ctx, JSValue v)
{
    if (qp_lazy_import_check(v))
        return qp_lazy_import_resolve(ctx, v);
    return v;
}

static JSValue qp_ceval_load_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                    QPGlobalCache *ic)
{
    QPFuncData *fd = qp_function_data(func);
    JSValueConst g, bi;
    JSValue v;
    int r;
    BOOL cacheable;

    if (!fd)
        return qp_raise_system_error(ctx, "no globals");
    g = fd->globals;
    bi = fd->builtins;
    cacheable = qp_dict_check_exact(ctx, g) && qp_dict_check_exact(ctx, bi);
    if (cacheable && ic) {
        QPDict *gd = QP_DICT(g), *bd = QP_DICT(bi);
        /* uids are stored + 1 so that a zeroed cache never matches */
        if (ic->globals_uid == gd->uid + 1 && ic->globals_version == gd->core.version &&
            ic->builtins_uid == bd->uid + 1 && ic->builtins_version == bd->core.version)
            return JS_DupValue(ctx, ic->value);
    }
    r = qp_ceval_mapping_get(ctx, g, atom, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0) {
        r = qp_ceval_mapping_get(ctx, bi, atom, &v);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0)
            return qp_raise_name_error(ctx, atom);
    }
    if (qp_lazy_import_check(v))
        return qp_lazy_import_resolve(ctx, v);
    if (cacheable && ic) {
        QPDict *gd = QP_DICT(g), *bd = QP_DICT(bi);
        ic->globals_uid = gd->uid + 1;
        ic->globals_version = gd->core.version;
        ic->builtins_uid = bd->uid + 1;
        ic->builtins_version = bd->core.version;
        ic->value = v; /* borrowed: owned by the dict while the versions match */
    }
    return v;
}

static int qp_ceval_store_global(JSContext *ctx, JSValueConst func, JSAtom atom,
                                 JSValueConst v)
{
    QPFuncData *fd = qp_function_data(func);
    if (!fd) {
        qp_raise_system_error(ctx, "no globals");
        return -1;
    }
    return qp_ceval_mapping_set(ctx, fd->globals, atom, v);
}

static int qp_ceval_delete_global(JSContext *ctx, JSValueConst func, JSAtom atom)
{
    QPFuncData *fd = qp_function_data(func);
    JSValue key;
    int r;
    if (!fd) {
        qp_raise_system_error(ctx, "no globals");
        return -1;
    }
    if (qp_dict_check_exact(ctx, fd->globals)) {
        r = qp_dict_del_item_atom(ctx, fd->globals, atom);
        if (r < 0)
            return -1;
        if (r == 0) {
            qp_raise_name_error(ctx, atom);
            return -1;
        }
        return 0;
    }
    key = qp_str_from_atom(ctx, atom);
    if (JS_IsException(key))
        return -1;
    r = qp_delitem(ctx, fd->globals, key);
    JS_FreeValue(ctx, key);
    if (r < 0) {
        if (qp_ceval_clear_if(ctx, QP_EXC(ctx, KeyError)))
            qp_raise_name_error(ctx, atom);
        return -1;
    }
    return 0;
}

static int qp_ceval_check_locals(JSContext *ctx, JSValueConst locals, const char *what,
                                 JSAtom atom)
{
    if (QP_IS_NULL(locals) || JS_IsUndefined(locals)) {
        JSValue name = qp_str_from_atom(ctx, atom);
        if (!JS_IsException(name)) {
            qp_raise_system_error(ctx, "no locals found when %s %R", what, name);
            JS_FreeValue(ctx, name);
        }
        return -1;
    }
    return 0;
}

static JSValue qp_ceval_load_name(JSContext *ctx, JSValueConst func, JSValueConst locals,
                                  JSAtom atom)
{
    QPFuncData *fd = qp_function_data(func);
    JSValue v;
    int r;

    if (qp_ceval_check_locals(ctx, locals, "loading", atom))
        return JS_EXCEPTION;
    r = qp_ceval_mapping_get(ctx, locals, atom, &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0 && fd) {
        r = qp_ceval_mapping_get(ctx, fd->globals, atom, &v);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0) {
            r = qp_ceval_mapping_get(ctx, fd->builtins, atom, &v);
            if (r < 0)
                return JS_EXCEPTION;
        }
    }
    if (r == 0)
        return qp_raise_name_error(ctx, atom);
    return qp_ceval_resolve_lazy(ctx, v);
}

static int qp_ceval_store_name(JSContext *ctx, JSValueConst locals, JSAtom atom,
                               JSValueConst v)
{
    if (qp_ceval_check_locals(ctx, locals, "storing", atom))
        return -1;
    return qp_ceval_mapping_set(ctx, locals, atom, v);
}

static int qp_ceval_delete_name(JSContext *ctx, JSValueConst locals, JSAtom atom)
{
    JSValue key;
    int r;
    if (qp_ceval_check_locals(ctx, locals, "deleting", atom))
        return -1;
    if (qp_dict_check_exact(ctx, locals)) {
        r = qp_dict_del_item_atom(ctx, locals, atom);
        if (r < 0)
            return -1;
        if (r == 0) {
            qp_raise_name_error(ctx, atom);
            return -1;
        }
        return 0;
    }
    key = qp_str_from_atom(ctx, atom);
    if (JS_IsException(key))
        return -1;
    r = qp_delitem(ctx, locals, key);
    JS_FreeValue(ctx, key);
    if (r < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        qp_raise_name_error(ctx, atom);
        return -1;
    }
    return 0;
}

/* Py_TYPE(o)->tp_iter == NULL && !PySequence_Check(o) */
static BOOL qp_ceval_not_iterable(JSContext *ctx, JSValueConst v)
{
    QPType *t = qp_type_of(ctx, v);
    if (t->slots.tp_iter)
        return FALSE;
    if (t->slots.sq.sq_item && !qp_dict_check(ctx, v))
        return FALSE;
    return TRUE;
}

static void qp_ceval_raise_not_iterable_star(JSContext *ctx, JSValueConst v)
{
    JSValue tname = qp_str_from_atom(ctx, qp_type_of(ctx, v)->name);
    if (JS_IsException(tname))
        return;
    qp_raise_type_error(ctx, "Value after * must be an iterable, not %.200U", tname);
    JS_FreeValue(ctx, tname);
}

static JSValue qp_ceval_call_ex(JSContext *ctx, JSValueConst callable, JSValueConst self,
                                JSValueConst args, JSValueConst kwargs)
{
    JSValue tup, ret;

    if (qp_tuple_check_exact(ctx, args)) {
        tup = JS_DupValue(ctx, args);
    } else {
        if (qp_ceval_not_iterable(ctx, args)) {
            qp_ceval_raise_not_iterable_star(ctx, args);
            return JS_EXCEPTION;
        }
        tup = qp_tuple_from_iterable(ctx, args);
        if (JS_IsException(tup))
            return tup;
    }
    if (!QP_IS_NULL(self)) {
        /* method form: self is the first argument */
        Py_ssize_t n = QP_TUPLE_SIZE(tup), i;
        JSValueConst *items = js_malloc(ctx, sizeof(JSValue) * (n + 1));
        JSValue t2;
        if (!items) {
            JS_FreeValue(ctx, tup);
            return JS_EXCEPTION;
        }
        items[0] = self;
        for(i = 0; i < n; i++)
            items[i + 1] = QP_TUPLE_ITEMS(tup)[i];
        t2 = qp_tuple_pack(ctx, n + 1, items);
        js_free(ctx, items);
        JS_FreeValue(ctx, tup);
        if (JS_IsException(t2))
            return t2;
        tup = t2;
    }
    ret = qp_call_tuple(ctx, callable, tup, kwargs);
    JS_FreeValue(ctx, tup);
    return ret;
}

static int qp_ceval_list_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable)
{
    if (qp_list_extend(ctx, list, iterable) < 0) {
        if (qp_ceval_not_iterable(ctx, iterable) &&
            qp_ceval_clear_if(ctx, QP_EXC(ctx, TypeError)))
            qp_ceval_raise_not_iterable_star(ctx, iterable);
        return -1;
    }
    return 0;
}

static int qp_ceval_dict_update(JSContext *ctx, JSValueConst dict, JSValueConst update)
{
    if (qp_dict_merge(ctx, dict, update, 1) < 0) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, AttributeError))) {
            JSValue exc = JS_GetException(ctx), keys;
            int has_keys = qp_getattr_opt(ctx, update, QP_ATOM(ctx, keys), &keys);
            if (has_keys == 0) {
                JS_FreeValue(ctx, exc);
                qp_raise_type_error(ctx, "'%T' object is not a mapping", update);
            } else if (has_keys > 0) {
                JS_FreeValue(ctx, keys);
                JS_Throw(ctx, exc);
            } else {
                JS_FreeValue(ctx, exc);
            }
        }
        return -1;
    }
    return 0;
}

static JSValue qp_ceval_with_except_start(JSContext *ctx, JSValueConst exit_func,
                                          JSValueConst self, JSValueConst exc)
{
    JSValueConst args[5];
    JSValue tb, ret;
    int n = 0;

    tb = qp_exc_get_traceback(ctx, exc);
    if (JS_IsException(tb))
        return tb;
    args[0] = JS_UNDEFINED; /* scratch slot */
    if (!QP_IS_NULL(self))
        args[1 + n++] = self;
    args[1 + n++] = QP_TYPE_VALUE(qp_type_of(ctx, exc));
    args[1 + n++] = exc;
    args[1 + n++] = tb;
    ret = qp_vectorcall(ctx, exit_func, args + 1, n | QP_VECTORCALL_ARGUMENTS_OFFSET, QP_NULL);
    JS_FreeValue(ctx, tb);
    return ret;
}

static JSValue qp_ceval_load_build_class(JSContext *ctx, JSValueConst func)
{
    QPFuncData *fd = qp_function_data(func);
    JSValue v;
    int r;
    if (!fd)
        return qp_raise_system_error(ctx, "no builtins");
    r = qp_ceval_mapping_get(ctx, fd->builtins, QP_ATOM(ctx, __build_class__), &v);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise(ctx, QP_EXC(ctx, NameError), "__build_class__ not found");
    return v;
}

static int qp_ceval_setup_annotations(JSContext *ctx, JSValueConst locals)
{
    JSValue v, d;
    int r;
    if (QP_IS_NULL(locals) || JS_IsUndefined(locals)) {
        qp_raise_system_error(ctx, "no locals found when setting up annotations");
        return -1;
    }
    r = qp_ceval_mapping_get(ctx, locals, QP_ATOM(ctx, __annotations__), &v);
    if (r < 0)
        return -1;
    if (r > 0) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return -1;
    r = qp_ceval_mapping_set(ctx, locals, QP_ATOM(ctx, __annotations__), d);
    JS_FreeValue(ctx, d);
    return r;
}

static JSValue qp_ceval_import_name(JSContext *ctx, JSValueConst func, JSFunctionBytecode *b,
                                    JSValue *arg_buf, JSAtom name, JSValueConst level,
                                    JSValueConst fromlist, int mode)
{
    QPFuncData *fd = qp_function_data(func);
    JSValueConst locals = JS_UNDEFINED;
    JSValue name_str, ret;
    int ilevel = 0;

    if (!fd)
        return qp_raise_system_error(ctx, "no globals");
    if (!(b->py_code->co_flags & CO_NEWLOCALS) && b->arg_count >= 1)
        locals = arg_buf[0];
    if (JS_VALUE_GET_TAG(level) == JS_TAG_INT)
        ilevel = JS_VALUE_GET_INT(level);
    name_str = qp_str_from_atom(ctx, name);
    if (JS_IsException(name_str))
        return name_str;
    ret = qp_import_name(ctx, name_str, fd->globals, locals, fromlist, ilevel, mode);
    JS_FreeValue(ctx, name_str);
    return ret;
}

static JSValue qp_ceval_intrinsic1(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                                   JSValue *arg_buf, int id, JSValueConst v)
{
    switch(id) {
    case QP_INTRINSIC1_PRINT:
        if (qp_displayhook(ctx, v) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    case QP_INTRINSIC1_IMPORT_STAR:
        if ((b->py_code->co_flags & CO_NEWLOCALS) || b->arg_count < 1)
            return qp_raise_system_error(ctx, "no locals found during 'import *'");
        if (qp_import_star(ctx, v, arg_buf[0]) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    case QP_INTRINSIC1_STOPITERATION_ERROR:
        return qp_gen_stopiteration_error(ctx, v, b->py_code->co_flags);
    case QP_INTRINSIC1_ASYNC_GEN_WRAP:
        return qp_asyncgen_wrap_value(ctx, v);
    case QP_INTRINSIC1_UNARY_POSITIVE:
        return qp_unop(ctx, v, QP_UNOP_POS);
    case QP_INTRINSIC1_LIST_TO_TUPLE:
        return qp_list_as_tuple(ctx, v);
    case QP_INTRINSIC1_TYPEVAR:
    case QP_INTRINSIC1_PARAMSPEC:
    case QP_INTRINSIC1_TYPEVARTUPLE:
    case QP_INTRINSIC1_SUBSCRIPT_GENERIC:
    case QP_INTRINSIC1_TYPEALIAS:
        return qp_typing_intrinsic1(ctx, id, v);
    default:
        return qp_raise_system_error(ctx, "invalid intrinsic function");
    }
}

static JSValue qp_ceval_intrinsic2(JSContext *ctx, int id, JSValueConst a, JSValueConst b)
{
    switch(id) {
    case QP_INTRINSIC2_PREP_RERAISE_STAR:
        return qp_exc_prep_reraise_star(ctx, a, b);
    case QP_INTRINSIC2_SET_FUNCTION_TYPE_PARAMS:
        if (qp_function_set_attr(ctx, a, 0, b) < 0)
            return JS_EXCEPTION;
        return JS_DupValue(ctx, a);
    case QP_INTRINSIC2_TYPEVAR_WITH_BOUND:
    case QP_INTRINSIC2_TYPEVAR_WITH_CONSTRAINTS:
    case QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT:
        return qp_typing_intrinsic2(ctx, id, a, b);
    default:
        return qp_raise_system_error(ctx, "invalid intrinsic function");
    }
}

/* ======================================================================= */
/* Threads and the GIL (M5)                                                 */
/* ======================================================================= */

/*
 * Python threads are OS threads (pthreads) sharing the runtime under a
 * global interpreter lock: CPython's Python/ceval_gil.c ("new GIL" with
 * FORCE_SWITCHING) and the thread states of Python/pystate.c (PSF-2.0).
 *
 * - QPThreads (JSRuntime.py_threads) is created when the first thread is
 *   started (python/mod_thread.c); until then none of this runs and
 *   QP_BEGIN/END_ALLOW_THREADS are no-ops.  The thread running Python code
 *   at that moment becomes the main thread and holds the GIL.
 * - The GIL protects the whole JSRuntime: JS code, Python code and the GC
 *   run only in the thread holding it; objects are shared.  Each thread
 *   has a QPThreadState that keeps its execution state while another
 *   thread runs: the JSRuntime/JSContext fields of quickjs.c
 *   (JSThreadExecState: native stack top/limit/size, frame chain, pending
 *   exception, native call nesting, interrupt counter), the per-thread
 *   fields of QPState (recursion depth, handled-exception stack, repr
 *   stack, async generator hooks) and of QPModState (current contextvars
 *   Context).  qp_tstate_save/restore move them on every handoff, so the
 *   interpreter keeps reading QPState and JSRuntime directly.
 * - Switching: a thread waiting for the GIL longer than the switch
 *   interval (sys.setswitchinterval, default 5 ms) sets the drop request
 *   and makes the holder poll soon (interrupt counter 1); the holder's
 *   periodic poll (__js_poll_interrupts / js_poll_safepoint ->
 *   qp_gil_poll) drops the GIL and waits until another thread took it.
 * - Blocking calls release the GIL with QP_BEGIN/END_ALLOW_THREADS; the
 *   code in between must not touch the runtime (no JSValue, no js_malloc).
 * - Waits on lock objects, thread handles and the interpreter's recursive
 *   mutexes (qp_thread_wait): their state lives in objects changed under
 *   the GIL; waiters sleep on one condition variable with the GIL released
 *   and qp_thread_notify() wakes them.
 * - The JS job queue, timers and host hooks run in the main thread (the
 *   host's event loop is the main thread's); JS code called from another
 *   thread runs in that thread.  Suspension (JS_RequestSuspend,
 *   JS_SuspendCall) applies to the main thread only.
 * - One Python context per runtime uses threads (QPThreads.ctx).
 * - Finalization (qp_threads_finalize, after threading._shutdown and
 *   atexit): the other threads hang when they try to take the GIL (CPython
 *   _PyThreadState_MustExit); the runtime must then not be freed.
 */
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include "modstate.h"


static uint64_t qp_thread_self_ident(void)
{
    return (uint64_t)(uintptr_t)pthread_self();
}

static void qp_cond_init(pthread_cond_t *c)
{
#if defined(__APPLE__)
    pthread_cond_init(c, NULL);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

/* wait at most ns nanoseconds: 0 (signalled or spurious) or ETIMEDOUT */
static int qp_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns)
{
    struct timespec ts;
    if (ns < 0)
        ns = 0;
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    return pthread_cond_timedwait_relative_np(c, m, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns += ts.tv_nsec;
    ts.tv_sec += (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

static double qp_thread_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void qp_thread_sync_init(QPThreads *thr)
{
    pthread_mutex_init(&thr->mutex, NULL);
    qp_cond_init(&thr->cond);
    pthread_mutex_init(&thr->switch_mutex, NULL);
    qp_cond_init(&thr->switch_cond);
    pthread_mutex_init(&thr->wait_mutex, NULL);
    qp_cond_init(&thr->wait_cond);
}

/* A thread state that has not run yet. */
static void qp_tstate_init(QPThreads *thr, QPThreadState *ts, uintptr_t stack_size)
{
    ts->thr = thr;
    js_thread_exec_state_init(&ts->js, stack_size);
    ts->recursion_depth = 0;
    ts->reraise = 0;
    ts->exc_info_base.exc_value = JS_UNDEFINED;
    ts->exc_info_base.previous = NULL;
    ts->exc_info = &thr->ctx->py->exc_info_base;
    ts->repr_stack = JS_UNDEFINED;
    ts->asyncgen_firstiter = JS_UNDEFINED;
    ts->asyncgen_finalizer = JS_UNDEFINED;
    ts->coro_origin_depth = 0;
    ts->context = QP_NULL;
    ts->trace_func = JS_UNDEFINED;
    ts->profile_func = JS_UNDEFINED;
    ts->trace_active = 0;
    ts->tracing = 0;
    ts->async_exc = QP_NULL;
    ts->local_refs = QP_NULL;
    ts->func = QP_NULL;
    ts->args = QP_NULL;
    ts->kwargs = QP_NULL;
    ts->handle = QP_NULL;
}

/* New thread state (not linked), NULL on memory error (no exception). */
static QPThreadState *qp_tstate_new(QPThreads *thr, uintptr_t stack_size)
{
    QPThreadState *ts = js_mallocz_rt(thr->rt, sizeof(*ts));
    if (!ts)
        return NULL;
    qp_tstate_init(thr, ts, stack_size);
    ts->serial = thr->next_serial++;
    return ts;
}

static void qp_tstate_save(QPThreads *thr, QPThreadState *ts)
{
    JSContext *ctx = thr->ctx;
    QPState *s = ctx->py;
    QPModState *ms = s->mod_state;

    js_thread_exec_state_save(thr->rt, ctx, &ts->js);
    ts->recursion_depth = s->recursion_depth;
    ts->reraise = s->reraise;
    ts->exc_info_base = s->exc_info_base;
    ts->exc_info = s->exc_info;
    ts->repr_stack = s->repr_stack;
    ts->asyncgen_firstiter = s->asyncgen_firstiter;
    ts->asyncgen_finalizer = s->asyncgen_finalizer;
    ts->coro_origin_depth = s->coro_origin_depth;
    ts->context = ms ? ms->ctx_current : QP_NULL;
    ts->async_exc = ms ? ms->async_exc : QP_NULL;
    ts->trace_func = s->trace_func;
    ts->profile_func = s->profile_func;
    ts->trace_active = s->trace_active;
    ts->tracing = s->tracing;
    if (qp_tstate_capi_hook)
        qp_tstate_capi_hook(ts, QP_TSTATE_SAVE);
}

static void qp_tstate_restore(QPThreads *thr, QPThreadState *ts)
{
    JSContext *ctx = thr->ctx;
    QPState *s = ctx->py;
    QPModState *ms = s->mod_state;

    js_thread_exec_state_restore(thr->rt, ctx, &ts->js);
    s->recursion_depth = ts->recursion_depth;
    s->reraise = ts->reraise;
    s->exc_info_base = ts->exc_info_base;
    s->exc_info = ts->exc_info;
    s->repr_stack = ts->repr_stack;
    s->asyncgen_firstiter = ts->asyncgen_firstiter;
    s->asyncgen_finalizer = ts->asyncgen_finalizer;
    s->coro_origin_depth = ts->coro_origin_depth;
    if (ms) {
        ms->ctx_current = ts->context;
        ms->async_exc = ts->async_exc;
    }
    s->trace_func = ts->trace_func;
    s->profile_func = ts->profile_func;
    s->trace_active = ts->trace_active;
    s->tracing = ts->tracing;
    if (qp_tstate_capi_hook)
        qp_tstate_capi_hook(ts, QP_TSTATE_RESTORE);
}

/* PyThread_hang_thread: a thread that must not run Python code anymore */
static void qp_thread_hang(void)
{
    for (;;)
        pause();
}

/* take_gil() */
static void qp_gil_take(QPThreads *thr, QPThreadState *ts)
{
    int err = errno;

    pthread_mutex_lock(&thr->mutex);
    for (;;) {
        unsigned long saved_switchnum;
        unsigned long interval;
        int r;
        if (thr->finalizing && ts != thr->finalizing_ts) {
            pthread_mutex_unlock(&thr->mutex);
            qp_thread_hang();
        }
        if (!thr->locked)
            break;
        saved_switchnum = thr->switch_number;
        interval = __atomic_load_n(&qp_gil_interval_us, __ATOMIC_RELAXED);
        if (interval < 1)
            interval = 1;
        r = qp_cond_timedwait(&thr->cond, &thr->mutex, (int64_t)interval * 1000);
        /* timed out without a switch: ask the holder to drop the GIL */
        if (r == ETIMEDOUT && thr->locked && thr->switch_number == saved_switchnum &&
            !thr->finalizing) {
            __atomic_store_n(&thr->drop_request, 1, __ATOMIC_RELAXED);
            /* poll at the next instruction (racy store, like a signal) */
            __atomic_store_n(&thr->ctx->interrupt_counter, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_lock(&thr->switch_mutex);
    thr->locked = 1;
    if (ts != thr->last_holder) {
        thr->last_holder = ts;
        thr->switch_number++;
    }
    pthread_cond_signal(&thr->switch_cond);
    pthread_mutex_unlock(&thr->switch_mutex);
    __atomic_store_n(&thr->drop_request, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&thr->mutex);
    errno = err;
}

/* drop_gil(): ts NULL for the final release of an exiting thread */
static void qp_gil_drop(QPThreads *thr, QPThreadState *ts)
{
    pthread_mutex_lock(&thr->mutex);
    thr->last_holder = ts;
    thr->locked = 0;
    pthread_cond_signal(&thr->cond);
    pthread_mutex_unlock(&thr->mutex);
    /* FORCE_SWITCHING: on a drop request, wait until another thread took
       the GIL, so that the dropping thread does not take it back at once */
    if (ts && __atomic_load_n(&thr->drop_request, __ATOMIC_RELAXED) && !thr->finalizing) {
        pthread_mutex_lock(&thr->switch_mutex);
        if (thr->last_holder == ts) {
            __atomic_store_n(&thr->drop_request, 0, __ATOMIC_RELAXED);
            pthread_cond_wait(&thr->switch_cond, &thr->switch_mutex);
        }
        pthread_mutex_unlock(&thr->switch_mutex);
    }
}

/* the thread ts took the GIL: make its state current */
static void qp_gil_switch_in(QPThreads *thr, QPThreadState *ts)
{
    thr->current = ts;
    qp_tstate_tls = ts;
    qp_tstate_restore(thr, ts);
    /* the main thread runs the signal handlers of signals that arrived
       while another thread held the GIL: poll now */
    if (ts == thr->main_ts)
        thr->ctx->interrupt_counter = 1;
}

/* PyEval_SaveThread: release the GIL around a blocking call.  NULL (and
   nothing done) while the runtime has a single Python thread. */
static QPThreadState *qp_gil_release(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *ts;

    if (likely(!thr))
        return NULL;
    ts = thr->current;
    assert(ts != NULL);
    qp_tstate_save(thr, ts);
    thr->current = NULL;
    qp_gil_drop(thr, ts);
    return ts;
}

/* PyEval_RestoreThread (ts from qp_gil_release, NULL: no-op); errno is
   preserved. */
static void qp_gil_acquire(JSContext *ctx, QPThreadState *ts)
{
    if (!ts)
        return;
    qp_gil_take(ts->thr, ts);
    qp_gil_switch_in(ts->thr, ts);
}

/* The periodic poll of the running code (JSRuntime.py_threads != NULL):
   give the GIL to the thread that requested it (CPython _Py_HandlePending
   gil_drop_request). */
static void qp_gil_poll(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *ts = thr->current;

    if (!__atomic_load_n(&thr->drop_request, __ATOMIC_RELAXED) || !ts)
        return;
    qp_tstate_save(thr, ts);
    thr->current = NULL;
    qp_gil_drop(thr, ts);
    qp_gil_take(thr, ts);
    qp_gil_switch_in(thr, ts);
}

/* Is the running thread (the GIL holder) the main thread?  TRUE while the
   runtime has a single Python thread. */
static BOOL qp_thread_is_main_rt(JSRuntime *rt)
{
    QPThreads *thr = rt->py_threads;
    return !thr || thr->current == thr->main_ts;
}

/* The running thread's state, NULL while the runtime has a single Python
   thread. */
static QPThreadState *qp_thread_current(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    return thr ? thr->current : NULL;
}

/* Unique number of the running thread (_thread._local keys); 0 for the
   main thread before threads exist. */
static uint64_t qp_thread_serial(JSContext *ctx)
{
    QPThreadState *ts = qp_thread_current(ctx);
    return ts ? ts->serial : 0;
}

/* _thread._get_main_thread_ident() */
static uint64_t qp_thread_main_ident(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    return thr ? thr->main_ts->ident : qp_thread_self_ident();
}

/* The thread structure of ctx's runtime, created on first use with the
   calling thread as main thread holding the GIL.  NULL with an exception
   when another Python context of the runtime uses threads. */
static QPThreads *qp_threads_get(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    QPThreads *thr = rt->py_threads;
    QPThreadState *m;

    if (thr) {
        if (thr->ctx != ctx) {
            qp_raise_runtime_error(ctx, "threads are used by another Python context "
                                   "of this runtime");
            return NULL;
        }
        return thr;
    }
    thr = js_mallocz_rt(rt, sizeof(*thr));
    if (!thr) {
        qp_raise_memory_error(ctx);
        return NULL;
    }
    thr->rt = rt;
    thr->ctx = ctx;
    init_list_head(&thr->tstates);
    m = qp_tstate_new(thr, rt->stack_size);
    if (!m) {
        js_free_rt(rt, thr);
        qp_raise_memory_error(ctx);
        return NULL;
    }
    m->ident = qp_thread_self_ident();
    list_add_tail(&m->link, &thr->tstates);
    thr->main_ts = m;
    thr->current = m;
    qp_tstate_tls = m;
    thr->last_holder = m;
    thr->locked = 1;
    qp_thread_sync_init(thr);
    rt->py_threads = thr;
    return thr;
}

/* Called by qp_state_free.  The structure is kept (leaked) if other
   threads still exist: they hang on its GIL. */
static void qp_threads_free(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    QPThreads *thr = rt->py_threads;
    QPThreadState *m;

    if (!thr || thr->ctx != ctx)
        return;
    rt->py_threads = NULL;
    m = thr->current ? thr->current : thr->main_ts;
    if (!QP_IS_NULL(m->local_refs)) {
        JS_FreeValueRT(rt, m->local_refs);
        m->local_refs = QP_NULL;
    }
    if (thr->tstates.next != &m->link || thr->tstates.prev != &m->link)
        return;
    pthread_mutex_destroy(&thr->mutex);
    pthread_cond_destroy(&thr->cond);
    pthread_mutex_destroy(&thr->switch_mutex);
    pthread_cond_destroy(&thr->switch_cond);
    pthread_mutex_destroy(&thr->wait_mutex);
    pthread_cond_destroy(&thr->wait_cond);
    js_free_rt(rt, m);
    js_free_rt(rt, thr);
}

/* Py_FinalizeEx after threading._shutdown and atexit
   (_PyRuntimeState_SetFinalizing): the other threads hang when they try
   to take the GIL.  Returns the number of other threads still existing
   (the runtime must then not be freed). */
static int qp_threads_finalize(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    struct list_head *el;
    int n = 0;

    if (!thr)
        return 0;
    pthread_mutex_lock(&thr->mutex);
    thr->finalizing = TRUE;
    thr->finalizing_ts = thr->current;
    __atomic_store_n(&thr->drop_request, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&thr->mutex);
    list_for_each(el, &thr->tstates) {
        if (list_entry(el, QPThreadState, link) != thr->current)
            n++;
    }
    return n;
}

/* Wake the threads sleeping in qp_thread_wait_change() (called under the
   GIL after a change of a waitable state: lock released, thread done). */
static void qp_thread_notify(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;

    if (!thr || thr->nwaiters == 0)
        return;
    pthread_mutex_lock(&thr->wait_mutex);
    thr->wait_gen++;
    pthread_cond_broadcast(&thr->wait_cond);
    pthread_mutex_unlock(&thr->wait_mutex);
}

/* Sleep with the GIL released until qp_thread_notify() is called or
   timeout_ns elapsed (-1: no limit). */
static void qp_thread_wait_change(JSContext *ctx, QPThreads *thr, int64_t timeout_ns)
{
    uint64_t gen = thr->wait_gen;
    QPThreadState *ts;

    thr->nwaiters++;
    ts = qp_gil_release(ctx);
    pthread_mutex_lock(&thr->wait_mutex);
    if (thr->wait_gen == gen) {
        if (timeout_ns < 0)
            pthread_cond_wait(&thr->wait_cond, &thr->wait_mutex);
        else
            qp_cond_timedwait(&thr->wait_cond, &thr->wait_mutex, timeout_ns);
    }
    pthread_mutex_unlock(&thr->wait_mutex);
    qp_gil_acquire(ctx, ts);
    thr->nwaiters--;
}

/* Wait until ready(ctx, obj) (tested under the GIL), at most timeout_ns
   (-1: no limit).  With handle_signals, the main thread runs the Python
   signal handlers while waiting (CPython _PY_LOCK_HANDLE_SIGNALS).  1
   ready, 0 timed out, -1 exception (raised by a signal handler, or
   PythonFinalizationError when only exited threads could make it ready).
   With a single Python thread only a signal handler can make it ready. */
static int qp_thread_wait(JSContext *ctx, QPThreadReadyFunc *ready, JSValueConst obj,
                          int64_t timeout_ns, BOOL handle_signals)
{
    double deadline = timeout_ns >= 0 ? qp_thread_monotonic_ns() + (double)timeout_ns : 0;

    for (;;) {
        QPThreads *thr;
        int64_t chunk = -1;
        if (ready(ctx, obj))
            return 1;
        if (timeout_ns >= 0) {
            double rem = deadline - qp_thread_monotonic_ns();
            if (rem <= 0)
                return 0;
            chunk = (int64_t)rem;
        }
        thr = ctx->rt->py_threads;
        if (!thr || (handle_signals && thr->current == thr->main_ts)) {
            /* poll the signal handlers every 50 ms */
            if (chunk < 0 || chunk > 50000000)
                chunk = 50000000;
        }
        if (!thr) {
            struct timespec ts;
            ts.tv_sec = (time_t)(chunk / 1000000000);
            ts.tv_nsec = (long)(chunk % 1000000000);
            nanosleep(&ts, NULL);   /* EINTR: a signal arrived, poll now */
        } else {
            if (thr->finalizing && timeout_ns < 0) {
                qp_raise(ctx, QP_EXC(ctx, PythonFinalizationError),
                         "cannot wait for another thread at interpreter shutdown");
                return -1;
            }
            qp_thread_wait_change(ctx, thr, chunk);
        }
        if (handle_signals && qp_signal_check(ctx) < 0)
            return -1;
    }
}

/* ---- recursive mutexes of the interpreter (_PyRecursiveMutex) ---- */

static QPThreadState *qp_rmutex_owner(QPThreads *thr, int which)
{
    QPThreadState *o = thr->rmutex_owner[which];
    return o ? o : thr->main_ts;
}

static int *qp_rmutex_level_ptr(JSContext *ctx, int which)
{
    QPState *s = QP_STATE(ctx);
    if (which == QP_RMUTEX_IMPORT)
        return &s->import_lock_level;
    return s->mod_state ? &s->mod_state->warn_lock_count : NULL;
}

static BOOL qp_rmutex_ready(JSContext *ctx, JSValueConst obj)
{
    int *plevel = qp_rmutex_level_ptr(ctx, JS_VALUE_GET_INT(obj));
    return !plevel || *plevel == 0;
}

/* _PyRecursiveMutex_Lock: blocks (GIL released) while another thread owns
   it; never fails. */
static void qp_rmutex_lock(JSContext *ctx, int which)
{
    QPThreads *thr = ctx->rt->py_threads;
    int *plevel = qp_rmutex_level_ptr(ctx, which);

    if (!plevel)
        return;
    if (thr && *plevel > 0 && qp_rmutex_owner(thr, which) != thr->current) {
        while (qp_thread_wait(ctx, qp_rmutex_ready, JS_NewInt32(ctx, which), -1, FALSE) <= 0) {
            /* PythonFinalizationError: nobody can release it anymore */
            JS_FreeValue(ctx, JS_GetException(ctx));
            *plevel = 0;
        }
    }
    if (thr && *plevel == 0)
        thr->rmutex_owner[which] = thr->current;
    (*plevel)++;
}

/* _PyRecursiveMutex_TryUnlock: -1 when the running thread does not own it
   (no exception set). */
static int qp_rmutex_unlock(JSContext *ctx, int which)
{
    QPThreads *thr = ctx->rt->py_threads;
    int *plevel = qp_rmutex_level_ptr(ctx, which);

    if (!plevel || *plevel <= 0)
        return -1;
    if (thr && qp_rmutex_owner(thr, which) != thr->current)
        return -1;
    if (--(*plevel) == 0) {
        if (thr)
            thr->rmutex_owner[which] = NULL;
        qp_thread_notify(ctx);
    }
    return 0;
}

/* Does another Python thread exist (os.fork() warning)? */
static BOOL qp_thread_others_exist(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    return thr && thr->tstates.next != thr->tstates.prev;
}

#ifdef CONFIG_PYTHON_CAPI
/* foreign threads (PyGILState_Ensure, python/capi_runtime.c) */

/* Bytes of native stack below the current stack pointer, 0 if unknown. */
static size_t qp_thread_stack_room(void)
{
    uintptr_t sp = js_get_stack_pointer(), low = 0;
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    low = (uintptr_t)pthread_get_stackaddr_np(self) - pthread_get_stacksize_np(self);
#elif defined(__linux__) && defined(_GNU_SOURCE)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *addr;
        size_t size;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0)
            low = (uintptr_t)addr;
        pthread_attr_destroy(&attr);
    }
#endif
    return low && sp > low ? sp - low : 0;
}

/* PyEval_AcquireThread for a thread that does not hold the GIL: take it
   with the calling thread's state, creating one for a thread that never
   ran Python code (a foreign thread calling PyGILState_Ensure).  The
   runtime must have its QPThreads.  NULL on memory error. */
static QPThreadState *qp_thread_attach(JSContext *ctx)
{
    QPThreads *thr = ctx->rt->py_threads;
    QPThreadState *ts = qp_tstate_tls;
    size_t room;

    if (ts && ts->thr == thr) {
        qp_gil_acquire(ctx, ts);
        return ts;
    }
    /* libc memory: the runtime allocator needs the GIL */
    ts = qp_sys_calloc(1, sizeof(*ts));
    if (!ts)
        return NULL;
    ts->foreign = TRUE;
    ts->ident = qp_thread_self_ident();
    qp_gil_take(thr, ts);
    room = qp_thread_stack_room();
    qp_tstate_init(thr, ts, room > 2 * 1024 * 1024 ? room - 512 * 1024 :
                   room ? room * 3 / 4 : 256 * 1024);
    if (thr->rt->stack_size == 0)
        ts->js.stack_size = 0;
    ts->serial = thr->next_serial++;
    list_add_tail(&ts->link, &thr->tstates);
    qp_gil_switch_in(thr, ts);
    JS_UpdateStackTop(thr->rt);
    return ts;
}

/* The running foreign thread state ts (from qp_thread_attach) is deleted
   (PyThreadState_DeleteCurrent): release what it holds, drop the GIL for
   good.  clear: the Python-level part (python/mod_thread.c). */
static void qp_thread_detach_foreign(JSContext *ctx, QPThreadState *ts,
                                     void (*clear)(JSContext *ctx, QPThreadState *ts))
{
    QPThreads *thr = ts->thr;

    clear(ctx, ts);
    list_del(&ts->link);
    thr->current = NULL;
    if (qp_tstate_tls == ts)
        qp_tstate_tls = NULL;
    qp_gil_drop(thr, NULL);
    qp_sys_free(ts);
}
#endif /* CONFIG_PYTHON_CAPI */
