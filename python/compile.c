/*
 * Python code generator (M2, unit b): validated AST + symbol table ->
 * QuickJS bytecode.
 *
 * Derived from CPython 3.15.2 Python/codegen.c and Python/compile.c
 * (PSF-2.0).  See compile.h for how CPython's instruction stream is mapped
 * onto QuickJS opcodes and OP_py subops.
 *
 * Differences from CPython (forced by the target, behaviour preserved):
 * - No CFG/flowgraph: instructions are emitted straight into the
 *   JSFunctionDef bytecode; resolve_labels/compute_stack_size of quickjs.c
 *   (with the OP_py walkers) do label resolution and stack sizing.
 *   LOAD_FAST vs LOAD_FAST_CHECK (flowgraph's definite-assignment pass) is
 *   approximated: locals always use get_loc_check; parameters use get_arg
 *   unless the body deletes them (`del p`, `except ... as p`) or they are
 *   cells (a nested scope may `del` them through nonlocal).
 * - Code that CPython inserts at the start of a finished sequence
 *   (generator StopIteration wrapper SETUP_CLEANUP, genexp GET_ITER prefix,
 *   module ANNOTATIONS_PLACEHOLDER) is emitted up front or reached through
 *   a jump, see codegen_emit_function_body, codegen_comprehension and
 *   compiler_codegen.
 * - The genexp's outermost iterable is converted with iter() by the caller
 *   (get_iter/get_aiter before the call) instead of in the genexp frame
 *   before RETURN_GENERATOR; the observable effect (iter() errors raised at
 *   creation time) is the same.
 * - PEP 709 inlined comprehensions give every name they bind its own
 *   hidden local (JSVarDef.is_lexical = 1) instead of CPython's
 *   LOAD_FAST_AND_CLEAR / STORE_FAST save-restore of a shared slot (which
 *   cannot preserve cell identity with QuickJS var refs).  The hidden
 *   slots are closed (close_loc) and cleared when the comprehension ends.
 * - ast_preprocess.c: only the behaviour-relevant parts are applied here:
 *   `__debug__` folding (codegen_nameop) and -OO docstring removal.
 *   Constant folding of `%`-formatting, constant tuples and match-pattern
 *   numbers is not done (same results at run time).
 * - SyntaxWarnings are emitted by qp_validate (validate.c), which walks the
 *   AST in code generation order before this unit runs: _PyCompile_Warn,
 *   infer_type, check_caller, check_subscripter, check_index,
 *   codegen_check_compare and the codegen_assert warning live there, and the
 *   ast_preprocess.c control-flow-in-finally warnings in
 *   qp_preprocess_warnings.
 *
 * CPython functions without a same-named counterpart here:
 * - codegen_addop_* / ADDOP* (codegen.c), _PyCompile_AddConst,
 *   _PyCompile_DictAddObj: the qpc_* emitters (compile.h table);
 *   co_names/co_varnames tables do not exist (atoms are operands, locals
 *   are JSVarDefs), constants go to the fd cpool (qpc_add_const).
 * - _Py_CArray_*: C arrays use js_resize_array.
 * - codegen_push/pop_inlined_comprehension_locals,
 *   restore_inlined_comprehension_locals: folded into
 *   push/pop_inlined_comprehension_state (hidden slots, see above).
 * - _PyCodegen_EnterAnonymousScope, _PyCodegen_Expression,
 *   compiler_mod, _PyAST_Compile, new_compiler, compiler_free,
 *   compiler_setup: compiler_codegen, qp_compile, qp_compile_ast.
 * - _PyCompile_OptimizeAndAssemble, optimize_and_assemble_code_unit,
 *   _PyCompile_Assemble, consts_dict_keys_inorder, const cache merging
 *   (const_cache_insert, merge_consts_recursive,
 *   _PyCompile_ConstCacheMergeOne), PyCode_Optimize: qpc_finish_unit +
 *   qp_create_function (resolve_labels, compute_stack_size); no constant
 *   de-duplication across code objects.
 * - _PyCompile_StartAnnotationSetup/EndAnnotationSetup,
 *   _PyCompile_DeferredAnnotations: module annotations are emitted at the
 *   ANNOTATIONS_PLACEHOLDER jump target (compiler_codegen); the deferred
 *   list is read from the unit directly.
 * - _PyCompile_LookupCellvar, dict_lookup_arg, _PyCompile_LookupArg:
 *   qpc_slot_find / qpc_freevar_index and the closure-variable binding in
 *   _PyCompile_EnterScope.
 * - _PyCompile_Error: qpc_error (_PyCompile_Warn: see above).
 * - _PyCompile_FutureFeatures, _PyCompile_Symtable(Entry),
 *   _PyCompile_OptimizationLevel, _PyCompile_IsInteractiveTopLevel,
 *   _PyCompile_ScopeType, _PyCompile_Qualname, _PyCompile_Metadata,
 *   _PyCompile_InstrSequence: accessor macros in compile.h / unit fields.
 * - _PyCompile_AstPreprocess (ast_preprocess.c): see above.
 * - _PyCompile_CodeGen: CPython test hook (instruction listing), no
 *   counterpart.
 */
#include "compile.h"

static const uint8_t qpc_dummy_source[1];

#define CO_FUTURE_MASK (CO_FUTURE_DIVISION | CO_FUTURE_ABSOLUTE_IMPORT | \
                        CO_FUTURE_WITH_STATEMENT | CO_FUTURE_PRINT_FUNCTION | \
                        CO_FUTURE_UNICODE_LITERALS | CO_FUTURE_BARRY_AS_BDFL | \
                        CO_FUTURE_GENERATOR_STOP | CO_FUTURE_ANNOTATIONS)

/* _Py_ANNOTATE_FORMAT_VALUE_WITH_FAKE_GLOBALS */
#define QPC_ANNOTATE_FORMAT_VALUE_WITH_FAKE_GLOBALS 2

typedef struct {
    int u_argcount;
    int u_posonlyargcount;
    int u_kwonlyargcount;
} _PyCompile_CodeUnitMetadata;

/* ======================================================================== */
/* Identifiers                                                               */
/* ======================================================================== */

static int qpc_ids_init(compiler *c)
{
#define QPC_ID_INIT(id) \
    if ((c->ids.id = JS_NewAtom(c->ctx, #id)) == JS_ATOM_NULL) return -1;
#define QPC_ID_INIT_S(id, s) \
    if ((c->ids.id = JS_NewAtom(c->ctx, s)) == JS_ATOM_NULL) return -1;
    QPC_IDS(QPC_ID_INIT, QPC_ID_INIT_S)
#undef QPC_ID_INIT
#undef QPC_ID_INIT_S
    return 0;
}

static void qpc_ids_free(compiler *c)
{
#define QPC_ID_FREE(id) JS_FreeAtom(c->ctx, c->ids.id);
#define QPC_ID_FREE_S(id, s) JS_FreeAtom(c->ctx, c->ids.id);
    QPC_IDS(QPC_ID_FREE, QPC_ID_FREE_S)
#undef QPC_ID_FREE
#undef QPC_ID_FREE_S
}

static BOOL qpc_atom_is(JSAtom a, JSAtom b)
{
    return a == b;
}

static const char *qpc_atom_str(compiler *c, char *buf, JSAtom atom)
{
    return JS_AtomGetStr(c->ctx, buf, QPC_ATOM_BUF, atom);
}

/* New atom "<a><mid><b>" (b may be JS_ATOM_NULL). */
static JSAtom qpc_atom_concat(compiler *c, JSAtom a, const char *mid, JSAtom b)
{
    StringBuffer sb;
    JSValue s;
    if (string_buffer_init(c->ctx, &sb, 16))
        return JS_ATOM_NULL;
    if (a != JS_ATOM_NULL &&
        string_buffer_concat_value_free(&sb, JS_AtomToString(c->ctx, a)))
        goto fail;
    if (mid && string_buffer_puts8(&sb, mid))
        goto fail;
    if (b != JS_ATOM_NULL &&
        string_buffer_concat_value_free(&sb, JS_AtomToString(c->ctx, b)))
        goto fail;
    s = string_buffer_end(&sb);
    if (JS_IsException(s))
        return JS_ATOM_NULL;
    return JS_NewAtomStr(c->ctx, JS_VALUE_GET_STRING(s));
 fail:
    string_buffer_free(&sb);
    return JS_ATOM_NULL;
}

/* "<generic parameters of NAME>" */
static JSAtom qpc_generic_params_name(compiler *c, JSAtom name)
{
    JSAtom a = qpc_atom_concat(c, JS_ATOM_NULL, "<generic parameters of ", name);
    JSAtom r;
    if (a == JS_ATOM_NULL)
        return JS_ATOM_NULL;
    r = qpc_atom_concat(c, a, ">", JS_ATOM_NULL);
    JS_FreeAtom(c->ctx, a);
    return r;
}

/* Sort atoms by their string value (CPython sorts str keys). */
static int qpc_sort_atoms(compiler *c, JSAtom *atoms, int n)
{
    JSValue *strs;
    int i, j;
    if (n < 2)
        return 0;
    strs = js_malloc(c->ctx, sizeof(strs[0]) * n);
    if (!strs)
        return -1;
    for (i = 0; i < n; i++) {
        strs[i] = JS_AtomToString(c->ctx, atoms[i]);
        if (JS_IsException(strs[i])) {
            while (--i >= 0)
                JS_FreeValue(c->ctx, strs[i]);
            js_free(c->ctx, strs);
            return -1;
        }
    }
    for (i = 1; i < n; i++) {
        JSAtom ka = atoms[i];
        JSValue ks = strs[i];
        j = i - 1;
        while (j >= 0 && js_string_compare(c->ctx, JS_VALUE_GET_STRING(strs[j]),
                                           JS_VALUE_GET_STRING(ks)) > 0) {
            atoms[j + 1] = atoms[j];
            strs[j + 1] = strs[j];
            j--;
        }
        atoms[j + 1] = ka;
        strs[j + 1] = ks;
    }
    for (i = 0; i < n; i++)
        JS_FreeValue(c->ctx, strs[i]);
    js_free(c->ctx, strs);
    return 0;
}

/* ======================================================================== */
/* Errors                                                                    */
/* ======================================================================== */

static int qpc_error(compiler *c, location loc, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qp_error_set(c->c_err, QP_ERR_SYNTAX, loc.lineno, loc.col_offset + 1,
                 loc.end_lineno, loc.end_col_offset + 1, "%s", buf);
    return ERROR;
}

/* Internal compiler error (CPython SystemError) */
static int qpc_internal_error(compiler *c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qp_error_set(c->c_err, QP_ERR_VALUE, 0, 0, 0, 0, "%s", buf);
    return ERROR;
}

/* ======================================================================== */
/* Emitters                                                                  */
/* ======================================================================== */

static DynBuf *qpc_bc(compiler *c)
{
    return &c->u->u_fd->byte_code;
}

static int qpc_emit_loc(compiler *c, location loc)
{
    compiler_unit *u = c->u;
    QPLoc ql;
    int idx;
    DynBuf *bc;

    if (loc.lineno < 0)
        return SUCCESS;     /* NO_LOCATION / NEXT_LOCATION: inherit */
    if (u->u_last_loc.lineno == loc.lineno &&
        u->u_last_loc.end_lineno == loc.end_lineno &&
        u->u_last_loc.col_offset == loc.col_offset &&
        u->u_last_loc.end_col_offset == loc.end_col_offset)
        return SUCCESS;
    ql.lineno = loc.lineno;
    ql.end_lineno = loc.end_lineno;
    ql.col = loc.col_offset;
    ql.end_col = loc.end_col_offset;
    idx = qp_fd_add_loc(c->ctx, u->u_fd, &ql);
    if (idx < 0)
        return ERROR;
    bc = qpc_bc(c);
    dbuf_putc(bc, OP_line_num);
    dbuf_put_u32(bc, idx);
    u->u_last_loc = loc;
    return SUCCESS;
}

static int qpc_op(compiler *c, location loc, int opcode)
{
    RETURN_IF_ERROR(qpc_emit_loc(c, loc));
    c->u->u_fd->last_opcode_pos = qpc_bc(c)->size;
    dbuf_putc(qpc_bc(c), opcode);
    return SUCCESS;
}

static int qpc_op_u8(compiler *c, location loc, int opcode, uint8_t v)
{
    RETURN_IF_ERROR(qpc_op(c, loc, opcode));
    dbuf_putc(qpc_bc(c), v);
    return SUCCESS;
}

static int qpc_op_u16(compiler *c, location loc, int opcode, uint16_t v)
{
    RETURN_IF_ERROR(qpc_op(c, loc, opcode));
    dbuf_put_u16(qpc_bc(c), v);
    return SUCCESS;
}

static int qpc_op_u32(compiler *c, location loc, int opcode, uint32_t v)
{
    RETURN_IF_ERROR(qpc_op(c, loc, opcode));
    dbuf_put_u32(qpc_bc(c), v);
    return SUCCESS;
}

static jump_target_label qpc_new_label(compiler *c)
{
    jump_target_label l;
    l.id = new_label_fd(c->u->u_fd);
    if (l.id < 0)
        return NO_LABEL;
    return l;
}

static int qpc_use_label(compiler *c, jump_target_label lbl)
{
    JSFunctionDef *fd = c->u->u_fd;
    if (!IS_JUMP_TARGET_LABEL(lbl))
        return SUCCESS;
    fd->last_opcode_pos = fd->byte_code.size;
    dbuf_putc(&fd->byte_code, OP_label);
    dbuf_put_u32(&fd->byte_code, lbl.id);
    fd->label_slots[lbl.id].pos = fd->byte_code.size;
    return SUCCESS;
}

static int qpc_jump(compiler *c, location loc, int opcode, jump_target_label lbl)
{
    assert(IS_JUMP_TARGET_LABEL(lbl));
    RETURN_IF_ERROR(qpc_op_u32(c, loc, opcode, lbl.id));
    c->u->u_fd->label_slots[lbl.id].ref_count++;
    return SUCCESS;
}

static int qpc_py(compiler *c, location loc, int subop)
{
    assert(subop >= 0 && subop < QP_OP_COUNT);
    return qpc_op_u8(c, loc, OP_py, subop);
}

static int qpc_py_u8(compiler *c, location loc, int subop, int v)
{
    RETURN_IF_ERROR(qpc_py(c, loc, subop));
    dbuf_putc(qpc_bc(c), (uint8_t)v);
    return SUCCESS;
}

static int qpc_py_u16(compiler *c, location loc, int subop, int v)
{
    if (v < 0 || v > 0xffff)
        return qpc_error(c, loc, "too many operands in an expression");
    RETURN_IF_ERROR(qpc_py(c, loc, subop));
    dbuf_put_u16(qpc_bc(c), (uint16_t)v);
    return SUCCESS;
}

static void qpc_put_atom(compiler *c, JSAtom atom)
{
    DynBuf *bc = qpc_bc(c);
    if (dbuf_claim(bc, 4))
        return; /* not enough memory: don't duplicate the atom */
    put_u32(bc->buf + bc->size, JS_DupAtom(c->ctx, atom));
    bc->size += 4;
}

static int qpc_py_atom(compiler *c, location loc, int subop, JSAtom atom)
{
    RETURN_IF_ERROR(qpc_py(c, loc, subop));
    qpc_put_atom(c, atom);
    return SUCCESS;
}

static int qpc_py_atom_u8(compiler *c, location loc, int subop, JSAtom atom, int v)
{
    RETURN_IF_ERROR(qpc_py_atom(c, loc, subop, atom));
    dbuf_putc(qpc_bc(c), (uint8_t)v);
    return SUCCESS;
}

static int qpc_py_atom_u16(compiler *c, location loc, int subop, JSAtom atom, int v)
{
    RETURN_IF_ERROR(qpc_py_atom(c, loc, subop, atom));
    dbuf_put_u16(qpc_bc(c), (uint16_t)v);
    return SUCCESS;
}

static int qpc_py_atom_ic(compiler *c, location loc, int subop, JSAtom atom)
{
    QPCodeInfo *ci = c->u->u_fd->py_code;
    int ic;
    if (ci->ic_count == 0xffff)
        return qpc_error(c, loc, "too many attribute and global references in one code block");
    ic = ci->ic_count++;
    return qpc_py_atom_u16(c, loc, subop, atom, ic);
}

static int qpc_py_jump(compiler *c, location loc, int subop, jump_target_label lbl)
{
    assert(IS_JUMP_TARGET_LABEL(lbl));
    RETURN_IF_ERROR(qpc_py(c, loc, subop));
    dbuf_put_u32(qpc_bc(c), lbl.id);
    c->u->u_fd->label_slots[lbl.id].ref_count++;
    return SUCCESS;
}

static int qpc_copy(compiler *c, location loc, int n)
{
    assert(n >= 1);
    if (n == 1)
        return qpc_op(c, loc, OP_dup);
    if (n > 255)
        return qpc_error(c, loc, "too many operands in an expression");
    return qpc_py_u8(c, loc, QP_OP_copy, n);
}

static int qpc_swap(compiler *c, location loc, int n)
{
    assert(n >= 2);
    if (n == 2)
        return qpc_op(c, loc, OP_swap);
    if (n > 255)
        return qpc_error(c, loc, "too many operands in an expression");
    return qpc_py_u8(c, loc, QP_OP_swap, n);
}

static int qpc_return(compiler *c, location loc)
{
    return qpc_op(c, loc, c->u->u_generator_like ? OP_return_async : OP_return);
}

static int qpc_setup(compiler *c, location loc, jump_target_label handler)
{
    return qpc_jump(c, loc, OP_catch, handler);
}

static int qpc_pop_block(compiler *c, location loc)
{
    return qpc_op(c, loc, OP_drop);
}

static int qpc_pop_block_under(compiler *c, location loc)
{
    return qpc_op(c, loc, OP_nip);
}

/* ======================================================================== */
/* Constants                                                                 */
/* ======================================================================== */

static int qpc_add_const(compiler *c, JSValue v)
{
    JSFunctionDef *fd = c->u->u_fd;
    if (JS_IsException(v))
        return -1;
    if (js_resize_array(c->ctx, (void *)&fd->cpool, sizeof(fd->cpool[0]),
                        &fd->cpool_size, fd->cpool_count + 1)) {
        JS_FreeValue(c->ctx, v);
        return -1;
    }
    fd->cpool[fd->cpool_count++] = v;
    return fd->cpool_count - 1;
}

static int qpc_load_none(compiler *c, location loc)
{
    return qpc_op(c, loc, OP_undefined);
}

static int qpc_load_bool(compiler *c, location loc, int b)
{
    return qpc_op(c, loc, b ? OP_push_true : OP_push_false);
}

static int qpc_load_small_int(compiler *c, location loc, int32_t v)
{
    return qpc_op_u32(c, loc, OP_push_i32, (uint32_t)v);
}

static int qpc_load_const_value(compiler *c, location loc, JSValue v)
{
    int idx;
    if (JS_IsException(v))
        return ERROR;
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_UNDEFINED:
        return qpc_load_none(c, loc);
    case JS_TAG_BOOL:
        return qpc_load_bool(c, loc, JS_VALUE_GET_BOOL(v));
    case JS_TAG_INT:
        return qpc_load_small_int(c, loc, JS_VALUE_GET_INT(v));
    default:
        break;
    }
    idx = qpc_add_const(c, v);
    if (idx < 0)
        return ERROR;
    return qpc_op_u32(c, loc, OP_push_const, idx);
}

static JSValue qpc_const_value(compiler *c, qp_const *k)
{
    JSContext *ctx = c->ctx;
    switch (k->kind) {
    case QP_CONST_NONE:
        return JS_UNDEFINED;
    case QP_CONST_TRUE:
        return JS_TRUE;
    case QP_CONST_FALSE:
        return JS_FALSE;
    case QP_CONST_ELLIPSIS:
        return JS_DupValue(ctx, QP_STATE(ctx)->ellipsis);
    case QP_CONST_INT:
        return qp_long_normalize(ctx, JS_DupValue(ctx, k->v));
    case QP_CONST_FLOAT:
        return JS_DupValue(ctx, k->v);
    case QP_CONST_COMPLEX:
        return qp_complex_new(ctx, 0.0, JS_VALUE_GET_FLOAT64(k->v));
    case QP_CONST_STR:
        return JS_DupValue(ctx, k->v);
    case QP_CONST_BYTES: {
        JSString *p = JS_VALUE_GET_STRING(k->v);
        if (!p->is_wide_char)
            return qp_bytes_new(ctx, p->u.str8, p->len);
        else {
            uint8_t *tmp = js_malloc(ctx, p->len ? p->len : 1);
            JSValue r;
            uint32_t i;
            if (!tmp)
                return JS_EXCEPTION;
            for (i = 0; i < p->len; i++)
                tmp[i] = (uint8_t)p->u.str16[i];
            r = qp_bytes_new(ctx, tmp, p->len);
            js_free(ctx, tmp);
            return r;
        }
    }
    case QP_CONST_OBJECT:
        return JS_DupValue(ctx, k->v);
    }
    return JS_UNDEFINED;
}

static int qpc_load_const(compiler *c, location loc, qp_const *k)
{
    switch (k->kind) {
    case QP_CONST_NONE:
        return qpc_load_none(c, loc);
    case QP_CONST_TRUE:
        return qpc_load_bool(c, loc, 1);
    case QP_CONST_FALSE:
        return qpc_load_bool(c, loc, 0);
    default:
        return qpc_load_const_value(c, loc, qpc_const_value(c, k));
    }
}

static int qpc_load_str_atom(compiler *c, location loc, JSAtom atom)
{
    return qpc_load_const_value(c, loc, JS_AtomToString(c->ctx, atom));
}

static int qpc_load_atom_tuple(compiler *c, location loc, const JSAtom *atoms, int n)
{
    JSValue *items, t;
    int i;
    items = js_malloc(c->ctx, sizeof(items[0]) * (n ? n : 1));
    if (!items)
        return ERROR;
    for (i = 0; i < n; i++) {
        items[i] = JS_AtomToString(c->ctx, atoms[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(c->ctx, items[i]);
            js_free(c->ctx, items);
            return ERROR;
        }
    }
    t = qp_tuple_new_steal(c->ctx, n, items);
    js_free(c->ctx, items);
    return qpc_load_const_value(c, loc, t);
}

/* ======================================================================== */
/* Mangling, frame blocks, small compiler services                          */
/* ======================================================================== */

static JSAtom _PyCompile_MaybeMangle(compiler *c, JSAtom name)
{
    return _Py_MaybeMangle(c->u->u_private, c->u->u_ste, name, c->ctx, c->c_err);
}

static int qpc_addop_name(compiler *c, location loc, int subop, JSAtom name, int v)
{
    JSAtom mangled = _PyCompile_MaybeMangle(c, name);
    int ret;
    if (mangled == JS_ATOM_NULL)
        return ERROR;
    switch (qp_subop_info[subop].fmt) {
    case OP_FMT_atom:
        ret = qpc_py_atom(c, loc, subop, mangled);
        break;
    case OP_FMT_atom_u8:
        ret = qpc_py_atom_u8(c, loc, subop, mangled, v);
        break;
    case OP_FMT_atom_u16:
        ret = qpc_py_atom_ic(c, loc, subop, mangled);
        break;
    default:
        ret = qpc_internal_error(c, "qpc_addop_name: bad subop %d", subop);
        break;
    }
    JS_FreeAtom(c->ctx, mangled);
    return ret;
}

static int _PyCompile_PushFBlock(compiler *c, location loc, fblocktype t,
                                 jump_target_label block_label,
                                 jump_target_label exit, void *datum)
{
    fblockinfo *f;
    if (c->u->u_nfblocks >= CO_MAXBLOCKS) {
        return qpc_error(c, loc, "too many statically nested blocks");
    }
    f = &c->u->u_fblock[c->u->u_nfblocks++];
    f->fb_type = t;
    f->fb_block = block_label;
    f->fb_loc = loc;
    f->fb_exit = exit;
    f->fb_datum = datum;
    f->fb_name = JS_ATOM_NULL;
    return SUCCESS;
}

static int qpc_push_fblock_name(compiler *c, location loc, fblocktype t,
                                jump_target_label block_label,
                                jump_target_label exit, JSAtom name)
{
    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, loc, t, block_label, exit, NULL));
    c->u->u_fblock[c->u->u_nfblocks - 1].fb_name = name;
    return SUCCESS;
}

static void _PyCompile_PopFBlock(compiler *c, fblocktype t, jump_target_label block_label)
{
    compiler_unit *u = c->u;
    assert(u->u_nfblocks > 0);
    u->u_nfblocks--;
    assert(u->u_fblock[u->u_nfblocks].fb_type == t);
    assert(SAME_LABEL(u->u_fblock[u->u_nfblocks].fb_block, block_label));
}

static fblockinfo *_PyCompile_TopFBlock(compiler *c)
{
    if (c->u->u_nfblocks == 0) {
        return NULL;
    }
    return &c->u->u_fblock[c->u->u_nfblocks - 1];
}

static bool _PyCompile_InExceptionHandler(compiler *c)
{
    for (int i = 0; i < c->u->u_nfblocks; i++) {
        fblockinfo *block = &c->u->u_fblock[i];
        switch (block->fb_type) {
        case COMPILE_FBLOCK_TRY_EXCEPT:
        case COMPILE_FBLOCK_FINALLY_TRY:
        case COMPILE_FBLOCK_FINALLY_END:
        case COMPILE_FBLOCK_EXCEPTION_HANDLER:
        case COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER:
        case COMPILE_FBLOCK_HANDLER_CLEANUP:
            return true;
        default:
            break;
        }
    }
    return false;
}

static int _PyCompile_IsInInlinedComp(compiler *c)
{
    return c->u->u_in_inlined_comp;
}

static void _PyCompile_EnterConditionalBlock(compiler *c)
{
    c->u->u_in_conditional_block++;
}

static void _PyCompile_LeaveConditionalBlock(compiler *c)
{
    assert(c->u->u_in_conditional_block > 0);
    c->u->u_in_conditional_block--;
}

static int _PyCompile_MaybeAddStaticAttributeToClass(compiler *c, expr_ty e)
{
    expr_ty attr_value;
    compiler_unit *u;
    assert(e->kind == Attribute_kind);
    attr_value = e->v.Attribute.value;
    if (attr_value->kind != Name_kind ||
        e->v.Attribute.ctx != Store ||
        attr_value->v.Name.id != c->ids.self) {
        return SUCCESS;
    }
    for (u = c->u; u; u = u->u_parent) {
        if (u->u_scope_type == COMPILE_SCOPE_CLASS) {
            assert(u->u_has_static_attributes);
            if (qp_symmap_set(&u->u_static_attributes, e->v.Attribute.attr, 0) < 0) {
                JS_ThrowOutOfMemory(c->ctx);
                return ERROR;
            }
            break;
        }
    }
    return SUCCESS;
}

static int _PyCompile_AddDeferredAnnotation(compiler *c, stmt_ty s,
                                            int *conditional_annotation_index)
{
    compiler_unit *u = c->u;
    qpc_deferred_annotation *d;
    *conditional_annotation_index = -1;
    if (js_resize_array(c->ctx, (void **)&u->u_deferred_annotations,
                        sizeof(u->u_deferred_annotations[0]),
                        &u->u_deferred_annotations_size,
                        u->u_n_deferred_annotations + 1))
        return ERROR;
    u->u_has_deferred_annotations = 1;
    d = &u->u_deferred_annotations[u->u_n_deferred_annotations++];
    d->st = s;
    d->mangled = JS_ATOM_NULL;
    if (u->u_scope_type == COMPILE_SCOPE_MODULE || u->u_in_conditional_block) {
        d->cond_index = u->u_next_conditional_annotation_index++;
        *conditional_annotation_index = d->cond_index;
    }
    else {
        d->cond_index = -1;
    }
    return SUCCESS;
}

static int _PyCompile_GetRefType(compiler *c, JSAtom name)
{
    if (c->u->u_scope_type == COMPILE_SCOPE_CLASS &&
        (name == c->ids.__class__ || name == c->ids.__classdict__ ||
         name == c->ids.__conditional_annotations__)) {
        return CELL;
    }
    int scope = _PyST_GetScope(c->u->u_ste, name);
    if (scope == 0) {
        char buf[QPC_ATOM_BUF];
        return qpc_internal_error(c, "_PyST_GetScope(name='%s') failed: unknown scope",
                                  qpc_atom_str(c, buf, name));
    }
    return scope;
}

/* ======================================================================== */
/* Variable slots                                                            */
/* ======================================================================== */

#define QPC_SLOT_ENCODE(kind, idx) (((long)(kind) << 24) | (long)(idx))
#define QPC_SLOT_KIND(v) ((int)((v) >> 24))
#define QPC_SLOT_IDX(v) ((int)((v) & 0xffffff))

static JSVarDef *qpc_slot_vardef(compiler_unit *u, int kind, int idx)
{
    return kind == QPC_SLOT_ARG ? &u->u_fd->args[idx] : &u->u_fd->vars[idx];
}

/* Slot of `name` in unit u (inlined comprehension overrides first).
   Returns 1 if found. */
static int qpc_slot_find(compiler_unit *u, JSAtom name, int *kind, int *idx)
{
    long v;
    for (int i = u->u_n_overrides - 1; i >= 0; i--) {
        if (u->u_overrides[i].name == name) {
            *kind = QPC_SLOT_VAR;
            *idx = u->u_overrides[i].var_idx;
            return 1;
        }
    }
    if (qp_symmap_lookup(&u->u_slots, name, &v)) {
        *kind = QPC_SLOT_KIND(v);
        *idx = QPC_SLOT_IDX(v);
        return 1;
    }
    return 0;
}

/* Slot of `name`, creating a local variable if needed. */
static int qpc_slot_get(compiler *c, compiler_unit *u, JSAtom name, int *kind, int *idx)
{
    int i;
    if (qpc_slot_find(u, name, kind, idx))
        return SUCCESS;
    i = add_var(c->ctx, u->u_fd, name);
    if (i < 0)
        return ERROR;
    if (qp_symmap_set(&u->u_slots, u->u_fd->vars[i].var_name,
                      QPC_SLOT_ENCODE(QPC_SLOT_VAR, i)) < 0) {
        JS_ThrowOutOfMemory(c->ctx);
        return ERROR;
    }
    *kind = QPC_SLOT_VAR;
    *idx = i;
    return SUCCESS;
}

/* Make a slot a cell (QuickJS captured variable). */
static void qpc_slot_capture(compiler_unit *u, int kind, int idx)
{
    JSVarDef *vd = qpc_slot_vardef(u, kind, idx);
    if (!vd->is_captured) {
        vd->is_captured = 1;
        vd->var_ref_idx = u->u_fd->var_ref_count++;
    }
}

/* Index of free variable `name` in unit u's closure, -1 if absent. */
static int qpc_freevar_index(compiler_unit *u, JSAtom name)
{
    long v;
    if (qp_symmap_lookup(&u->u_freevars, name, &v))
        return (int)v;
    return -1;
}

/* Load / store / delete of a cell owned by the current unit (slot kind,idx) */
static int qpc_emit_own_cell(compiler *c, location loc, int kind, int idx,
                             expr_context_ty ctx)
{
    switch (ctx) {
    case Load:
        if (kind == QPC_SLOT_ARG)
            return qpc_py_u16(c, loc, QP_OP_get_arg_check, idx);
        return qpc_op_u16(c, loc, OP_get_loc_check, idx);
    case Store:
        return qpc_op_u16(c, loc, kind == QPC_SLOT_ARG ? OP_put_arg : OP_put_loc, idx);
    case Del:
        if (kind == QPC_SLOT_ARG) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_get_arg_check, idx));
            RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
            return qpc_op_u16(c, loc, OP_put_arg, idx);
        }
        RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_loc_check, idx));
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
        return qpc_op_u16(c, loc, OP_set_loc_uninitialized, idx);
    }
    return SUCCESS;
}

static int qpc_emit_free_var(compiler *c, location loc, int idx, expr_context_ty ctx)
{
    switch (ctx) {
    case Load:
        return qpc_op_u16(c, loc, OP_get_var_ref_check, idx);
    case Store:
        return qpc_op_u16(c, loc, OP_put_var_ref, idx);
    case Del:
        RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_var_ref_check, idx));
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
        return qpc_op_u16(c, loc, OP_put_var_ref, idx);
    }
    return SUCCESS;
}

/* LOAD_DEREF / STORE_DEREF of `name` resolved through the cell of the
   current unit if it has one, else through its free variables. */
static int qpc_deref_op(compiler *c, location loc, JSAtom name, expr_context_ty ctx)
{
    int kind, idx;
    if (qpc_slot_find(c->u, name, &kind, &idx)) {
        qpc_slot_capture(c->u, kind, idx);
        return qpc_emit_own_cell(c, loc, kind, idx, ctx);
    }
    idx = qpc_freevar_index(c->u, name);
    if (idx < 0) {
        char buf[QPC_ATOM_BUF];
        return qpc_internal_error(c, "compiler: no cell or free variable '%s'",
                                  qpc_atom_str(c, buf, name));
    }
    return qpc_emit_free_var(c, loc, idx, ctx);
}

/* u16 operand of load_from_dict_or_deref for `name` */
static int qpc_deref_operand(compiler *c, JSAtom name, int scope, int *operand)
{
    int kind, idx;
    if (scope == CELL && qpc_slot_find(c->u, name, &kind, &idx)) {
        if (kind == QPC_SLOT_ARG)
            return qpc_internal_error(c, "compiler: class cell is an argument");
        qpc_slot_capture(c->u, kind, idx);
        *operand = idx;
        return SUCCESS;
    }
    idx = qpc_freevar_index(c->u, name);
    if (idx < 0) {
        if (qpc_slot_find(c->u, name, &kind, &idx) && kind == QPC_SLOT_VAR) {
            qpc_slot_capture(c->u, kind, idx);
            *operand = idx;
            return SUCCESS;
        }
        char buf[QPC_ATOM_BUF];
        return qpc_internal_error(c, "compiler: no cell or free variable '%s'",
                                  qpc_atom_str(c, buf, name));
    }
    *operand = idx | QP_DEREF_FREE;
    return SUCCESS;
}

/* ======================================================================== */
/* Deleted-parameter scan (LOAD_FAST_CHECK approximation)                   */
/* ======================================================================== */

static int qpc_scan_del_target(compiler *c, expr_ty e)
{
    switch (e->kind) {
    case Name_kind:
        if (qp_symmap_set(&c->u->u_deleted, e->v.Name.id, 1) < 0)
            return ERROR;
        break;
    case Tuple_kind:
        for (int i = 0; i < asdl_seq_LEN(e->v.Tuple.elts); i++)
            RETURN_IF_ERROR(qpc_scan_del_target(c, asdl_seq_GET(e->v.Tuple.elts, i)));
        break;
    case List_kind:
        for (int i = 0; i < asdl_seq_LEN(e->v.List.elts); i++)
            RETURN_IF_ERROR(qpc_scan_del_target(c, asdl_seq_GET(e->v.List.elts, i)));
        break;
    case Starred_kind:
        return qpc_scan_del_target(c, e->v.Starred.value);
    default:
        break;
    }
    return SUCCESS;
}

static int qpc_scan_deletes(compiler *c, asdl_stmt_seq *body);

static int qpc_scan_handlers(compiler *c, asdl_excepthandler_seq *handlers)
{
    for (int i = 0; i < asdl_seq_LEN(handlers); i++) {
        excepthandler_ty h = asdl_seq_GET(handlers, i);
        if (h->v.ExceptHandler.name != JS_ATOM_NULL &&
            qp_symmap_set(&c->u->u_deleted, h->v.ExceptHandler.name, 1) < 0)
            return ERROR;
        RETURN_IF_ERROR(qpc_scan_deletes(c, h->v.ExceptHandler.body));
    }
    return SUCCESS;
}

static int qpc_scan_deletes(compiler *c, asdl_stmt_seq *body)
{
    for (int i = 0; i < asdl_seq_LEN(body); i++) {
        stmt_ty s = asdl_seq_GET(body, i);
        switch (s->kind) {
        case Delete_kind:
            for (int j = 0; j < asdl_seq_LEN(s->v.Delete.targets); j++)
                RETURN_IF_ERROR(qpc_scan_del_target(c, asdl_seq_GET(s->v.Delete.targets, j)));
            break;
        case For_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.For.body));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.For.orelse));
            break;
        case AsyncFor_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.AsyncFor.body));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.AsyncFor.orelse));
            break;
        case While_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.While.body));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.While.orelse));
            break;
        case If_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.If.body));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.If.orelse));
            break;
        case With_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.With.body));
            break;
        case AsyncWith_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.AsyncWith.body));
            break;
        case Match_kind:
            for (int j = 0; j < asdl_seq_LEN(s->v.Match.cases); j++) {
                match_case_ty mc = asdl_seq_GET(s->v.Match.cases, j);
                RETURN_IF_ERROR(qpc_scan_deletes(c, mc->body));
            }
            break;
        case Try_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.Try.body));
            RETURN_IF_ERROR(qpc_scan_handlers(c, s->v.Try.handlers));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.Try.orelse));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.Try.finalbody));
            break;
        case TryStar_kind:
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.TryStar.body));
            RETURN_IF_ERROR(qpc_scan_handlers(c, s->v.TryStar.handlers));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.TryStar.orelse));
            RETURN_IF_ERROR(qpc_scan_deletes(c, s->v.TryStar.finalbody));
            break;
        default:
            break;
        }
    }
    return SUCCESS;
}

/* ======================================================================== */
/* Code units (_PyCompile_EnterScope / ExitScope / SetQualname)             */
/* ======================================================================== */

static void compiler_unit_free(compiler *c, compiler_unit *u)
{
    JSContext *ctx = c->ctx;
    JS_FreeAtom(ctx, u->u_private);
    JS_FreeAtom(ctx, u->u_name);
    JS_FreeAtom(ctx, u->u_qualname);
    qp_symmap_free(&u->u_static_attributes);
    qp_symmap_free(&u->u_slots);
    qp_symmap_free(&u->u_freevars);
    qp_symmap_free(&u->u_fasthidden);
    qp_symmap_free(&u->u_deleted);
    for (int i = 0; i < u->u_n_deferred_annotations; i++)
        JS_FreeAtom(ctx, u->u_deferred_annotations[i].mangled);
    js_free(ctx, u->u_deferred_annotations);
    js_free(ctx, u->u_overrides);
    js_free(ctx, u->u_comp_codes);
    js_free(ctx, u);
}

static int _PyCompile_SetQualname(compiler *c)
{
    compiler_unit *u = c->u;
    JSAtom base = JS_ATOM_NULL, name;

    if (u->u_scope_type == COMPILE_SCOPE_MODULE) {
        u->u_qualname = JS_DupAtom(c->ctx, u->u_name);
        return SUCCESS;
    }
    /* CPython: stack_size > 1, i.e. the parent is not the module unit */
    if (u->u_parent && u->u_parent->u_parent) {
        compiler_unit *parent = u->u_parent;
        int force_global = 0;

        if (parent->u_scope_type == COMPILE_SCOPE_ANNOTATIONS) {
            /* The parent is an annotation scope, so we need to
               look at the grandparent. */
            if (parent->u_parent == NULL || parent->u_parent->u_parent == NULL) {
                /* immediately within the module */
                u->u_qualname = JS_DupAtom(c->ctx, u->u_name);
                return SUCCESS;
            }
            parent = parent->u_parent;
        }

        if (u->u_scope_type == COMPILE_SCOPE_FUNCTION
            || u->u_scope_type == COMPILE_SCOPE_ASYNC_FUNCTION
            || u->u_scope_type == COMPILE_SCOPE_CLASS) {
            JSAtom mangled = _Py_Mangle(parent->u_private, u->u_name, c->ctx, c->c_err);
            int scope;
            if (mangled == JS_ATOM_NULL)
                return ERROR;
            scope = _PyST_GetScope(parent->u_ste, mangled);
            JS_FreeAtom(c->ctx, mangled);
            assert(scope != GLOBAL_IMPLICIT);
            if (scope == GLOBAL_EXPLICIT)
                force_global = 1;
        }

        if (!force_global) {
            if (parent->u_scope_type == COMPILE_SCOPE_FUNCTION
                || parent->u_scope_type == COMPILE_SCOPE_ASYNC_FUNCTION
                || parent->u_scope_type == COMPILE_SCOPE_LAMBDA)
            {
                base = qpc_atom_concat(c, parent->u_qualname, NULL, c->ids.dot_locals);
                if (base == JS_ATOM_NULL)
                    return ERROR;
            }
            else {
                base = JS_DupAtom(c->ctx, parent->u_qualname);
            }
        }
        if (u->u_ste->ste_function_name != JS_ATOM_NULL) {
            JSAtom tmp = base;
            base = qpc_atom_concat(c, tmp, tmp != JS_ATOM_NULL ? "." : NULL,
                                   u->u_ste->ste_function_name);
            JS_FreeAtom(c->ctx, tmp);
            if (base == JS_ATOM_NULL)
                return ERROR;
        }
    }
    else if (u->u_ste->ste_function_name != JS_ATOM_NULL) {
        base = JS_DupAtom(c->ctx, u->u_ste->ste_function_name);
    }

    if (base != JS_ATOM_NULL) {
        name = qpc_atom_concat(c, base, ".", u->u_name);
        JS_FreeAtom(c->ctx, base);
        if (name == JS_ATOM_NULL)
            return ERROR;
    }
    else {
        name = JS_DupAtom(c->ctx, u->u_name);
    }
    u->u_qualname = name;
    return SUCCESS;
}

/* Collect the names of `ste` with scope `scope_type` or flag `flag`
   (CPython dictbytype), sorted. */
static int qpc_names_by_type(compiler *c, PySTEntryObject *ste, int scope_type,
                             long flag, JSAtom **pnames, int *pn)
{
    Py_ssize_t pos = 0;
    JSAtom k;
    long v;
    JSAtom *names = NULL;
    int n = 0, size = 0;
    while (qp_symmap_next(&ste->ste_symbols, &pos, &k, &v)) {
        long scope = SYMBOL_TO_SCOPE(v);
        if (scope == scope_type || (v & flag)) {
            if (js_resize_array(c->ctx, (void **)&names, sizeof(names[0]), &size, n + 1)) {
                js_free(c->ctx, names);
                return ERROR;
            }
            names[n++] = k;
        }
    }
    if (qpc_sort_atoms(c, names, n) < 0) {
        js_free(c->ctx, names);
        return ERROR;
    }
    *pnames = names;
    *pn = n;
    return SUCCESS;
}

/* Implicit cell of a class/module unit (__class__, __classdict__,
   __conditional_annotations__) */
static int qpc_add_implicit_cell(compiler *c, compiler_unit *u, JSAtom name)
{
    int kind, idx;
    RETURN_IF_ERROR(qpc_slot_get(c, u, name, &kind, &idx));
    qpc_slot_capture(u, kind, idx);
    return SUCCESS;
}

static int _PyCompile_EnterScope(compiler *c, JSAtom name, int scope_type,
                                 void *key, int lineno, JSAtom private,
                                 const _PyCompile_CodeUnitMetadata *umd)
{
    JSContext *ctx = c->ctx;
    compiler_unit *u, *parent = c->u;
    PySTEntryObject *ste;
    JSFunctionDef *fd;
    JSAtom *names = NULL;
    int n, i;

    u = js_mallocz(ctx, sizeof(*u));
    if (!u)
        return ERROR;
    qp_symmap_init(&u->u_static_attributes, ctx->rt);
    qp_symmap_init(&u->u_slots, ctx->rt);
    qp_symmap_init(&u->u_freevars, ctx->rt);
    qp_symmap_init(&u->u_fasthidden, ctx->rt);
    qp_symmap_init(&u->u_deleted, ctx->rt);
    u->u_private = JS_ATOM_NULL;
    u->u_name = JS_ATOM_NULL;
    u->u_qualname = JS_ATOM_NULL;
    u->u_last_loc = NO_LOCATION;
    u->u_stopiter_handler = NO_LABEL;
    u->u_ann_label = NO_LABEL;
    u->u_ann_body_label = NO_LABEL;
    u->u_scope_type = scope_type;
    if (umd) {
        u->u_argcount = umd->u_argcount;
        u->u_posonlyargcount = umd->u_posonlyargcount;
        u->u_kwonlyargcount = umd->u_kwonlyargcount;
    }
    u->u_ste = ste = _PySymtable_Lookup(c->c_st, key);
    if (!ste) {
        compiler_unit_free(c, u);
        return ERROR;
    }
    u->u_name = JS_DupAtom(ctx, name);
    u->u_firstlineno = lineno;
    u->u_has_static_attributes = (scope_type == COMPILE_SCOPE_CLASS);

    fd = js_new_function_def(ctx, parent ? parent->u_fd : NULL, FALSE, FALSE,
                             c->c_filename, qpc_dummy_source, &c->c_lc_cache);
    if (!fd) {
        compiler_unit_free(c, u);
        return ERROR;
    }
    u->u_fd = fd;
    fd->is_py = TRUE;
    fd->func_name = JS_DupAtom(ctx, name);
    fd->py_code = qp_code_info_new(ctx);
    if (!fd->py_code) {
        if (!parent)
            js_free_function_def(ctx, fd);
        compiler_unit_free(c, u);
        return ERROR;
    }
    u->u_generator_like = (ste->ste_generator || ste->ste_coroutine);
    fd->func_kind = u->u_generator_like ? JS_FUNC_GENERATOR : JS_FUNC_NORMAL;

    /* from here the unit is on the stack: failures are cleaned up by the
       top-level error path */
    u->u_parent = parent;
    c->u = u;
    c->c_nunits++;
    if (!parent)
        c->c_root_fd = fd;
    if (parent && private == JS_ATOM_NULL)
        private = parent->u_private;
    u->u_private = JS_DupAtom(ctx, private);

    /* parameters (co_varnames prefix) or the hidden locals mapping */
    if (_PyST_IsFunctionLike(ste)) {
        for (i = 0; i < ste->ste_varnames.len; i++) {
            JSAtom pn = ste->ste_varnames.items[i];
            int idx = add_arg(ctx, fd, pn);
            if (idx < 0)
                return ERROR;
            if (qp_symmap_set(&u->u_slots, fd->args[idx].var_name,
                              QPC_SLOT_ENCODE(QPC_SLOT_ARG, idx)) < 0) {
                JS_ThrowOutOfMemory(ctx);
                return ERROR;
            }
        }
    }
    else {
        if (add_arg(ctx, fd, JS_ATOM_NULL) < 0)
            return ERROR;
    }
    fd->defined_arg_count = fd->arg_count;

    /* cell variables (co_cellvars): captured slots */
    RETURN_IF_ERROR(qpc_names_by_type(c, ste, CELL, 0, &names, &n));
    for (i = 0; i < n; i++) {
        int kind, idx;
        if (qpc_slot_get(c, u, names[i], &kind, &idx) < 0) {
            js_free(ctx, names);
            return ERROR;
        }
        qpc_slot_capture(u, kind, idx);
    }
    js_free(ctx, names);
    names = NULL;
    if (ste->ste_needs_class_closure) {
        /* Cook up an implicit __class__ cell. */
        assert(scope_type == COMPILE_SCOPE_CLASS);
        RETURN_IF_ERROR(qpc_add_implicit_cell(c, u, c->ids.__class__));
    }
    if (ste->ste_needs_classdict) {
        /* Cook up an implicit __classdict__ cell. */
        assert(scope_type == COMPILE_SCOPE_CLASS);
        RETURN_IF_ERROR(qpc_add_implicit_cell(c, u, c->ids.__classdict__));
    }
    if (ste->ste_has_conditional_annotations && scope_type == COMPILE_SCOPE_CLASS) {
        /* Cook up an implicit __conditional_annotations__ cell (module
           units keep it in the namespace only: STORE_NAME/LOAD_GLOBAL) */
        RETURN_IF_ERROR(qpc_add_implicit_cell(c, u, c->ids.__conditional_annotations__));
    }

    /* free variables (co_freevars): closure vars bound to the parent */
    RETURN_IF_ERROR(qpc_names_by_type(c, ste, FREE, DEF_FREE_CLASS, &names, &n));
    for (i = 0; i < n; i++) {
        JSAtom fname = names[i];
        int ctype, var_idx, cidx, reftype, kind;
        if (!parent) {
            js_free(ctx, names);
            return qpc_internal_error(c, "compiler: free variable at module level");
        }
        /* _PyCompile_LookupArg, evaluated in the parent */
        c->u = parent;
        reftype = _PyCompile_GetRefType(c, fname);
        if (reftype == CELL && qpc_slot_find(parent, fname, &kind, &var_idx)) {
            qpc_slot_capture(parent, kind, var_idx);
            ctype = (kind == QPC_SLOT_ARG) ? JS_CLOSURE_ARG : JS_CLOSURE_LOCAL;
        }
        else if (reftype == CELL) {
            if (qpc_slot_get(c, parent, fname, &kind, &var_idx) < 0) {
                c->u = u;
                js_free(ctx, names);
                return ERROR;
            }
            qpc_slot_capture(parent, kind, var_idx);
            ctype = (kind == QPC_SLOT_ARG) ? JS_CLOSURE_ARG : JS_CLOSURE_LOCAL;
        }
        else {
            var_idx = qpc_freevar_index(parent, fname);
            ctype = JS_CLOSURE_REF;
        }
        c->u = u;
        if (reftype < 0 || var_idx < 0) {
            char buf[QPC_ATOM_BUF];
            js_free(ctx, names);
            if (reftype < 0)
                return ERROR;
            return qpc_internal_error(c, "compiler_lookup_arg(name='%s') failed",
                                      qpc_atom_str(c, buf, fname));
        }
        cidx = add_closure_var(ctx, fd, ctype, var_idx, fname, FALSE, FALSE, JS_VAR_NORMAL);
        if (cidx < 0 ||
            qp_symmap_set(&u->u_freevars, fd->closure_var[cidx].var_name, cidx) < 0) {
            js_free(ctx, names);
            if (cidx >= 0)
                JS_ThrowOutOfMemory(ctx);
            return ERROR;
        }
    }
    js_free(ctx, names);

    RETURN_IF_ERROR(_PyCompile_SetQualname(c));
    return SUCCESS;
}

static void _PyCompile_ExitScope(compiler *c)
{
    compiler_unit *u = c->u;
    c->u = u->u_parent;
    c->c_nunits--;
    compiler_unit_free(c, u);
}

/* compute_code_flags */
static int compute_code_flags(compiler *c)
{
    PySTEntryObject *ste = c->u->u_ste;
    int flags = 0;
    if (_PyST_IsFunctionLike(ste)) {
        flags |= CO_NEWLOCALS | CO_OPTIMIZED;
        if (ste->ste_nested)
            flags |= CO_NESTED;
        if (ste->ste_generator && !ste->ste_coroutine)
            flags |= CO_GENERATOR;
        if (ste->ste_generator && ste->ste_coroutine)
            flags |= CO_ASYNC_GENERATOR;
        if (ste->ste_varargs)
            flags |= CO_VARARGS;
        if (ste->ste_varkeywords)
            flags |= CO_VARKEYWORDS;
        if (ste->ste_has_docstring && c->c_optimize < 2)
            flags |= CO_HAS_DOCSTRING;
        if (ste->ste_method)
            flags |= CO_METHOD;
    }
    if (ste->ste_coroutine && !ste->ste_generator) {
        flags |= CO_COROUTINE;
    }
    /* (Only) inherit compilerflags in PyCF_MASK */
    flags |= (c->c_future.ff_features & CO_FUTURE_MASK);
    return flags;
}

/* _PyCompile_OptimizeAndAssemble: finish the current unit (add_return:
   1 = `return None` at the end, 0 = `return TOS`, -1 = nothing) and fill
   its code metadata.  The fd stays in the parent's child list (or is the
   root); the caller then calls _PyCompile_ExitScope. */
static JSFunctionDef *qpc_finish_unit(compiler *c, int add_return)
{
    compiler_unit *u = c->u;
    QPCodeInfo *ci = u->u_fd->py_code;
    if (add_return >= 0 && _PyCodegen_AddReturnAtEnd(c, add_return) < 0)
        return NULL;
    ci->co_flags = compute_code_flags(c);
    ci->argcount = u->u_argcount + u->u_posonlyargcount;
    ci->posonlyargcount = u->u_posonlyargcount;
    ci->kwonlyargcount = u->u_kwonlyargcount;
    ci->firstlineno = u->u_firstlineno;
    JS_FreeAtom(c->ctx, ci->qualname);
    ci->qualname = JS_DupAtom(c->ctx, u->u_qualname);
    return u->u_fd;
}

static int _PyCodegen_AddReturnAtEnd(compiler *c, int addNone)
{
    /* Make sure every instruction stream that falls off the end returns None. */
    if (addNone) {
        RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
    }
    return qpc_return(c, NO_LOCATION);
}

/* MAKE_FUNCTION + SET_FUNCTION_ATTRIBUTE: `fd` is a finished child of the
   current unit. */
static int codegen_make_closure(compiler *c, location loc, JSFunctionDef *fd, int flags)
{
    int idx;
    assert(fd->parent == c->u->u_fd);
    idx = qpc_add_const(c, JS_UNDEFINED);
    if (idx < 0)
        return ERROR;
    fd->parent_cpool_idx = idx;
    RETURN_IF_ERROR(qpc_op_u32(c, loc, OP_fclosure, idx));
    if (flags & QP_FUNC_ATTR_ANNOTATIONS) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_set_func_attr, QP_FUNC_ATTR_ANNOTATIONS));
    }
    if (flags & QP_FUNC_ATTR_ANNOTATE) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_set_func_attr, QP_FUNC_ATTR_ANNOTATE));
    }
    if (flags & QP_FUNC_ATTR_KWDEFAULTS) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_set_func_attr, QP_FUNC_ATTR_KWDEFAULTS));
    }
    if (flags & QP_FUNC_ATTR_DEFAULTS) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_set_func_attr, QP_FUNC_ATTR_DEFAULTS));
    }
    return SUCCESS;
}

/* Finish the current unit, leave it and create its function. */
static int qpc_finish_and_make_closure(compiler *c, int add_return, location loc, int flags)
{
    JSFunctionDef *fd = qpc_finish_unit(c, add_return);
    if (!fd)
        return ERROR;
    _PyCompile_ExitScope(c);
    return codegen_make_closure(c, loc, fd, flags);
}

static int codegen_enter_scope(compiler *c, JSAtom name, int scope_type,
                               void *key, int lineno, JSAtom private,
                               const _PyCompile_CodeUnitMetadata *umd)
{
    RETURN_IF_ERROR(
        _PyCompile_EnterScope(c, name, scope_type, key, lineno, private, umd));
    /* RETURN_GENERATOR + POP_TOP and RESUME: not needed (qp_gen_new at
       call time, no instrumentation). */
    if (scope_type == COMPILE_SCOPE_MODULE) {
        /* ANNOTATIONS_PLACEHOLDER */
        compiler_unit *u = c->u;
        u->u_ann_label = qpc_new_label(c);
        u->u_ann_body_label = qpc_new_label(c);
        if (!IS_JUMP_TARGET_LABEL(u->u_ann_label) || !IS_JUMP_TARGET_LABEL(u->u_ann_body_label))
            return ERROR;
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, u->u_ann_label));
        USE_LABEL(c, u->u_ann_body_label);
    }
    return SUCCESS;
}

/* ======================================================================== */
/* Names (codegen_nameop, _PyCompile_ResolveNameop)                         */
/* ======================================================================== */

static int _PyCompile_ResolveNameop(compiler *c, JSAtom mangled, int scope,
                                    _PyCompile_optype *optype)
{
    long v;
    *optype = COMPILE_OP_NAME;
    switch (scope) {
    case FREE:
    case CELL:
        *optype = COMPILE_OP_DEREF;
        break;
    case LOCAL:
        if (_PyST_IsFunctionLike(c->u->u_ste)) {
            *optype = COMPILE_OP_FAST;
        }
        else if (qp_symmap_lookup(&c->u->u_fasthidden, mangled, &v) && v) {
            *optype = COMPILE_OP_FAST;
        }
        break;
    case GLOBAL_IMPLICIT:
        if (_PyST_IsFunctionLike(c->u->u_ste)) {
            *optype = COMPILE_OP_GLOBAL;
        }
        break;
    case GLOBAL_EXPLICIT:
        *optype = COMPILE_OP_GLOBAL;
        break;
    default:
        /* scope can be 0 */
        break;
    }
    return SUCCESS;
}

static int codegen_load_classdict_freevar(compiler *c, location loc)
{
    int idx = qpc_freevar_index(c->u, c->ids.__classdict__);
    if (idx < 0)
        return qpc_internal_error(c, "compiler: __classdict__ is not a free variable");
    return qpc_op_u16(c, loc, OP_get_var_ref_check, idx);
}

static int codegen_nameop(compiler *c, location loc, JSAtom name, expr_context_ty ctx)
{
    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    JSAtom mangled;
    int scope, ret = SUCCESS;
    _PyCompile_optype optype;

    assert(name != c->ids.None && name != c->ids.True && name != c->ids.False);

    /* ast_preprocess.c: `__debug__` is a constant */
    if (ctx == Load && name == c->ids.__debug__) {
        return qpc_load_bool(c, loc, c->c_optimize == 0);
    }

    mangled = _PyCompile_MaybeMangle(c, name);
    if (mangled == JS_ATOM_NULL)
        return ERROR;

    scope = _PyST_GetScope(ste, mangled);
    _PyCompile_ResolveNameop(c, mangled, scope, &optype);

    switch (optype) {
    case COMPILE_OP_DEREF:
        if (ctx == Load &&
            ((ste->ste_type == ClassBlock && !_PyCompile_IsInInlinedComp(c)) ||
             ste->ste_can_see_class_scope)) {
            int operand;
            if (ste->ste_type == ClassBlock && !_PyCompile_IsInInlinedComp(c)) {
                /* First load the locals */
                ret = qpc_py(c, loc, QP_OP_load_locals);
            }
            else {
                /* First load the classdict */
                ret = codegen_load_classdict_freevar(c, loc);
            }
            if (ret == SUCCESS)
                ret = qpc_deref_operand(c, mangled, scope, &operand);
            if (ret == SUCCESS)
                ret = qpc_py_atom_u16(c, loc, QP_OP_load_from_dict_or_deref,
                                      mangled, operand);
        }
        else if (scope == CELL) {
            ret = qpc_deref_op(c, loc, mangled, ctx);
        }
        else {
            int idx = qpc_freevar_index(c->u, mangled);
            if (idx < 0)
                ret = qpc_deref_op(c, loc, mangled, ctx);
            else
                ret = qpc_emit_free_var(c, loc, idx, ctx);
        }
        break;
    case COMPILE_OP_FAST: {
        int kind, idx;
        ret = qpc_slot_get(c, c->u, mangled, &kind, &idx);
        if (ret < 0)
            break;
        switch (ctx) {
        case Load:
            if (kind == QPC_SLOT_ARG) {
                if (qpc_slot_vardef(c->u, kind, idx)->is_captured ||
                    qp_symmap_contains(&c->u->u_deleted, name) ||
                    qp_symmap_contains(&c->u->u_deleted, mangled))
                    ret = qpc_py_u16(c, loc, QP_OP_get_arg_check, idx);
                else
                    ret = qpc_op_u16(c, loc, OP_get_arg, idx);
            }
            else {
                ret = qpc_op_u16(c, loc, OP_get_loc_check, idx);
            }
            break;
        case Store:
            ret = qpc_op_u16(c, loc, kind == QPC_SLOT_ARG ? OP_put_arg : OP_put_loc, idx);
            break;
        case Del:
            ret = qpc_emit_own_cell(c, loc, kind, idx, Del);
            break;
        }
        break;
    }
    case COMPILE_OP_GLOBAL:
        switch (ctx) {
        case Load:
            if (ste->ste_can_see_class_scope && scope == GLOBAL_IMPLICIT) {
                /* First load the classdict */
                ret = codegen_load_classdict_freevar(c, loc);
                if (ret == SUCCESS)
                    ret = qpc_py_atom_ic(c, loc, QP_OP_load_from_dict_or_globals, mangled);
            }
            else {
                ret = qpc_py_atom_ic(c, loc, QP_OP_load_global, mangled);
            }
            break;
        case Store:
            ret = qpc_py_atom(c, loc, QP_OP_store_global, mangled);
            break;
        case Del:
            ret = qpc_py_atom(c, loc, QP_OP_delete_global, mangled);
            break;
        }
        break;
    case COMPILE_OP_NAME:
        switch (ctx) {
        case Load:
            if (ste->ste_type == ClassBlock && _PyCompile_IsInInlinedComp(c))
                ret = qpc_py_atom_ic(c, loc, QP_OP_load_global, mangled);
            else
                ret = qpc_py_atom(c, loc, QP_OP_load_name, mangled);
            break;
        case Store:
            ret = qpc_py_atom(c, loc, QP_OP_store_name, mangled);
            break;
        case Del:
            ret = qpc_py_atom(c, loc, QP_OP_delete_name, mangled);
            break;
        }
        break;
    }
    JS_FreeAtom(c->ctx, mangled);
    return ret;
}

/* STORE_DEREF / LOAD_CLOSURE of an implicit class cell */
static int qpc_store_own_cell(compiler *c, location loc, JSAtom name)
{
    int kind, idx;
    if (!qpc_slot_find(c->u, name, &kind, &idx))
        return qpc_internal_error(c, "compiler: missing implicit cell");
    return qpc_emit_own_cell(c, loc, kind, idx, Store);
}

static int qpc_load_closure(compiler *c, location loc, JSAtom name)
{
    int kind, idx;
    if (!qpc_slot_find(c->u, name, &kind, &idx) || kind != QPC_SLOT_VAR)
        return qpc_internal_error(c, "compiler: missing implicit cell");
    qpc_slot_capture(c->u, kind, idx);
    return qpc_py_u16(c, loc, QP_OP_make_cell, idx);
}

/* ======================================================================== */
/* Docstrings                                                                */
/* ======================================================================== */

/* _PyAST_GetDocString */
static qp_const *qpc_get_docstring(asdl_stmt_seq *body)
{
    stmt_ty st;
    expr_ty e;
    if (!asdl_seq_LEN(body))
        return NULL;
    st = (stmt_ty)asdl_seq_GET(body, 0);
    if (st->kind != Expr_kind)
        return NULL;
    e = st->v.Expr.value;
    if (e->kind == Constant_kind && e->v.Constant.value->kind == QP_CONST_STR)
        return e->v.Constant.value;
    return NULL;
}

/* _PyCompile_CleanDoc: expandtabs() then remove the common indentation of
   the lines after the first one (works on UTF-16 code units; only ' ',
   '\t', '\n' and '\r' are inspected). */
static JSValue _PyCompile_CleanDoc(compiler *c, JSValueConst doc_val)
{
    JSContext *ctx = c->ctx;
    JSString *doc = JS_VALUE_GET_STRING(doc_val);
    StringBuffer sb;
    uint32_t *buf;
    int len = 0, size, i, col;
    int doc_size, p, pend, margin;
    JSValue r;

    /* expandtabs(8) */
    size = doc->len + 16;
    buf = js_malloc(ctx, sizeof(buf[0]) * size);
    if (!buf)
        return JS_EXCEPTION;
    col = 0;
    for (i = 0; i < (int)doc->len; i++) {
        uint32_t ch = string_get(doc, i);
        if (ch == '\t') {
            int incr = 8 - (col % 8);
            if (len + incr + 1 > size) {
                uint32_t *nb;
                size = (len + incr + 1) * 3 / 2 + 16;
                nb = js_realloc(ctx, buf, sizeof(buf[0]) * size);
                if (!nb) {
                    js_free(ctx, buf);
                    return JS_EXCEPTION;
                }
                buf = nb;
            }
            for (int k = 0; k < incr; k++)
                buf[len++] = ' ';
            col += incr;
            continue;
        }
        if (len + 1 > size) {
            uint32_t *nb;
            size = size * 3 / 2 + 16;
            nb = js_realloc(ctx, buf, sizeof(buf[0]) * size);
            if (!nb) {
                js_free(ctx, buf);
                return JS_EXCEPTION;
            }
            buf = nb;
        }
        buf[len++] = ch;
        if (ch == '\n' || ch == '\r')
            col = 0;
        else
            col++;
    }
    doc_size = len;

    /* First pass: find minimum indentation of any non-blank lines
       after first line. */
    p = 0;
    pend = doc_size;
    while (p < pend && buf[p++] != '\n') {
    }
    margin = INT_MAX;
    while (p < pend) {
        int s = p;
        while (p < pend && buf[p] == ' ') p++;
        if (p < pend && buf[p] != '\n') {
            if (p - s < margin)
                margin = p - s;
        }
        while (p < pend && buf[p++] != '\n') {
        }
    }
    if (margin == INT_MAX) {
        margin = 0;
    }

    /* Second pass: copy first line without leading spaces, then the
       subsequent lines without margin. */
    if (string_buffer_init(ctx, &sb, doc_size))
        goto fail;
    p = 0;
    while (p < pend && buf[p] == ' ') {
        p++;
    }
    while (p < pend) {
        uint32_t ch = buf[p++];
        if (string_buffer_putc16(&sb, ch))
            goto fail_sb;
        if (ch == '\n') {
            break;
        }
    }
    while (p < pend) {
        for (i = 0; i < margin && p < pend; i++, p++) {
            if (buf[p] != ' ') {
                break;
            }
        }
        while (p < pend) {
            uint32_t ch = buf[p++];
            if (string_buffer_putc16(&sb, ch))
                goto fail_sb;
            if (ch == '\n') {
                break;
            }
        }
    }
    js_free(ctx, buf);
    r = string_buffer_end(&sb);
    return r;
 fail_sb:
    string_buffer_free(&sb);
 fail:
    js_free(ctx, buf);
    return JS_EXCEPTION;
}

/* ======================================================================== */
/* Annotations (PEP 649)                                                     */
/* ======================================================================== */

static int codegen_emit_annotations_prologue(compiler *c, location loc)
{
    /* if .format > VALUE_WITH_FAKE_GLOBALS: raise NotImplementedError */
    NEW_JUMP_TARGET_LABEL(c, body);
    assert(!SYMTABLE_ENTRY(c)->ste_has_docstring);
    RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_arg, 0));
    RETURN_IF_ERROR(qpc_load_small_int(c, loc, QPC_ANNOTATE_FORMAT_VALUE_WITH_FAKE_GLOBALS));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_compare, QP_CMP_GT | QP_CMP_TO_BOOL));
    RETURN_IF_ERROR(qpc_jump(c, loc, OP_if_false, body));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_common_const,
                              QP_COMMON_CONST_NOTIMPLEMENTEDERROR));
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_raise, 1));
    USE_LABEL(c, body);
    return SUCCESS;
}

static int codegen_setup_annotations_scope(compiler *c, location loc,
                                           void *key, JSAtom name)
{
    _PyCompile_CodeUnitMetadata umd = {
        .u_posonlyargcount = 1,
    };
    RETURN_IF_ERROR(
        codegen_enter_scope(c, name, COMPILE_SCOPE_ANNOTATIONS,
                            key, loc.lineno, JS_ATOM_NULL, &umd));
    RETURN_IF_ERROR(codegen_emit_annotations_prologue(c, loc));
    return SUCCESS;
}

static int codegen_finish_annotations_scope(compiler *c, location loc)
{
    JSFunctionDef *fd;
    RETURN_IF_ERROR(qpc_return(c, loc));
    fd = qpc_finish_unit(c, 1);
    if (!fd)
        return ERROR;
    /* We want the parameter to __annotate__ to be named "format" in the
       signature shown by inspect.signature(), but we need to use a
       different name (.format) in the symtable. */
    if (fd->arg_count > 0) {
        JS_FreeAtom(c->ctx, fd->args[0].var_name);
        fd->args[0].var_name = JS_DupAtom(c->ctx, c->ids.format);
    }
    _PyCompile_ExitScope(c);
    return codegen_make_closure(c, loc, fd, 0);
}

static int codegen_deferred_annotations_body(compiler *c, location loc, int scope_type)
{
    compiler_unit *outer = c->u->u_parent;
    int annotations_len = outer->u_n_deferred_annotations;

    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0)); /* stack now contains <annos> */

    for (int i = 0; i < annotations_len; i++) {
        stmt_ty st = outer->u_deferred_annotations[i].st;
        long idx = outer->u_deferred_annotations[i].cond_index;
        JSAtom mangled;
        int ret;
        NEW_JUMP_TARGET_LABEL(c, not_set);

        if (idx != -1) {
            RETURN_IF_ERROR(qpc_load_small_int(c, LOC(st), (int32_t)idx));
            if (scope_type == COMPILE_SCOPE_CLASS) {
                int fidx = qpc_freevar_index(c->u, c->ids.__conditional_annotations__);
                if (fidx < 0)
                    return qpc_internal_error(c, "compiler: __conditional_annotations__ is not free");
                RETURN_IF_ERROR(qpc_op_u16(c, LOC(st), OP_get_var_ref_check, fidx));
            }
            else {
                RETURN_IF_ERROR(qpc_py_atom_ic(c, LOC(st), QP_OP_load_global,
                                               c->ids.__conditional_annotations__));
            }
            RETURN_IF_ERROR(qpc_py_u8(c, LOC(st), QP_OP_contains, 0));
            RETURN_IF_ERROR(qpc_jump(c, LOC(st), OP_if_false, not_set));
        }

        VISIT(c, expr, st->v.AnnAssign.annotation);
        RETURN_IF_ERROR(qpc_copy(c, LOC(st), 2));
        /* the outer unit's private name applies */
        mangled = _Py_Mangle(outer->u_private, st->v.AnnAssign.target->v.Name.id,
                             c->ctx, c->c_err);
        if (mangled == JS_ATOM_NULL)
            return ERROR;
        ret = qpc_load_str_atom(c, LOC(st), mangled);
        JS_FreeAtom(c->ctx, mangled);
        RETURN_IF_ERROR(ret);
        /* stack now contains <annos> <value> <annos> <name> */
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setitem));
        /* stack now contains <annos> */
        USE_LABEL(c, not_set);
    }
    return SUCCESS;
}

/* Emit the __annotate__ function of the current (module or class) unit
   and store it. */
static int qpc_emit_deferred_annotations(compiler *c, location loc)
{
    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    int scope_type = SCOPE_TYPE(c);
    void *key;

    /* It's possible that ste_annotations_block is set but
       u_deferred_annotations is not, because the former is still
       set if there are only non-simple annotations. */
    assert(ste->ste_annotation_block != NULL);
    key = (void *)((uintptr_t)ste->ste_id + 1);
    RETURN_IF_ERROR(codegen_setup_annotations_scope(c, loc, key,
                                                    ste->ste_annotation_block->ste_name));
    RETURN_IF_ERROR(codegen_deferred_annotations_body(c, loc, scope_type));
    RETURN_IF_ERROR(codegen_finish_annotations_scope(c, loc));
    RETURN_IF_ERROR(codegen_nameop(
        c, loc,
        ste->ste_type == ClassBlock ? c->ids.__annotate_func__ : c->ids.__annotate__,
        Store));
    return SUCCESS;
}

static int codegen_process_deferred_annotations(compiler *c, location loc)
{
    if (!c->u->u_has_deferred_annotations) {
        return SUCCESS;
    }
    if (SCOPE_TYPE(c) == COMPILE_SCOPE_MODULE) {
        /* emitted at the ANNOTATIONS_PLACEHOLDER (compiler_codegen) */
        c->u->u_module_annotations_pending = 1;
        return SUCCESS;
    }
    return qpc_emit_deferred_annotations(c, loc);
}

/* Compile a sequence of statements, checking for a docstring
   and for annotations. */
static int codegen_body(compiler *c, location loc, asdl_stmt_seq *stmts, bool is_interactive)
{
    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    Py_ssize_t first_instr = 0;

    /* If from __future__ import annotations is active,
     * every annotated class and module should have __annotations__.
     * Else __annotate__ is created when necessary. */
    if ((FUTURE_FEATURES(c) & CO_FUTURE_ANNOTATIONS) && ste->ste_annotations_used) {
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setup_annotations));
    }
    if (!asdl_seq_LEN(stmts)) {
        return SUCCESS;
    }
    if (!is_interactive) { /* A string literal on REPL prompt is not a docstring */
        if (ste->ste_has_docstring) {
            qp_const *docstring = qpc_get_docstring(stmts);
            stmt_ty st = (stmt_ty)asdl_seq_GET(stmts, 0);
            first_instr = 1;
            /* -OO: ast_preprocess removed the docstring */
            if (docstring && c->c_optimize < 2) {
                JSValue cleandoc = _PyCompile_CleanDoc(c, docstring->v);
                location dloc = LOC(st->v.Expr.value);
                RETURN_IF_ERROR(qpc_load_const_value(c, dloc, cleandoc));
                RETURN_IF_ERROR(codegen_nameop(c, NO_LOCATION, c->ids.__doc__, Store));
            }
        }
    }
    for (Py_ssize_t i = first_instr; i < asdl_seq_LEN(stmts); i++) {
        VISIT(c, stmt, (stmt_ty)asdl_seq_GET(stmts, i));
    }
    /* If there are annotations and the future import is not on, we
       collect the annotations in a separate pass and generate an
       __annotate__ function. See PEP 649. */
    if (!(FUTURE_FEATURES(c) & CO_FUTURE_ANNOTATIONS)) {
        RETURN_IF_ERROR(codegen_process_deferred_annotations(c, loc));
    }
    return SUCCESS;
}

static int _PyCodegen_Module(compiler *c, location loc, asdl_stmt_seq *stmts,
                             bool is_interactive)
{
    if (SYMTABLE_ENTRY(c)->ste_has_conditional_annotations) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_set, 0));
        RETURN_IF_ERROR(qpc_py_atom(c, loc, QP_OP_store_name,
                                    c->ids.__conditional_annotations__));
    }
    return codegen_body(c, loc, stmts, is_interactive);
}

/* ======================================================================== */
/* Functions, decorators, defaults, annotations                             */
/* ======================================================================== */

static int codegen_decorators(compiler *c, asdl_expr_seq *decos)
{
    if (!decos) {
        return SUCCESS;
    }
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(decos); i++) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(decos, i));
    }
    return SUCCESS;
}

static int codegen_apply_decorators(compiler *c, asdl_expr_seq *decos)
{
    if (!decos) {
        return SUCCESS;
    }
    for (Py_ssize_t i = asdl_seq_LEN(decos) - 1; i > -1; i--) {
        location loc = LOC((expr_ty)asdl_seq_GET(decos, i));
        /* [deco, func]: CALL 0 with func in the self slot */
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
    }
    return SUCCESS;
}

/* Push a dict of keyword-only default values.
   Return -1 on error, 0 if no dict pushed, 1 if a dict is pushed. */
static int codegen_kwonlydefaults(compiler *c, location loc,
                                  asdl_arg_seq *kwonlyargs, asdl_expr_seq *kw_defaults)
{
    int default_count = 0;
    for (int i = 0; i < asdl_seq_LEN(kwonlyargs); i++) {
        arg_ty arg = asdl_seq_GET(kwonlyargs, i);
        expr_ty default_ = asdl_seq_GET(kw_defaults, i);
        if (default_) {
            JSAtom mangled;
            int ret;
            default_count++;
            mangled = _PyCompile_MaybeMangle(c, arg->arg);
            if (mangled == JS_ATOM_NULL)
                return ERROR;
            ret = qpc_load_str_atom(c, loc, mangled);
            JS_FreeAtom(c->ctx, mangled);
            RETURN_IF_ERROR(ret);
            VISIT(c, expr, default_);
        }
    }
    if (default_count) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, default_count));
        return 1;
    }
    return 0;
}

static int codegen_visit_annexpr(compiler *c, expr_ty annotation)
{
    location loc = LOC(annotation);
    return qpc_load_const_value(c, loc, qpc_expr_as_unicode(c->ctx, annotation));
}

static int codegen_argannotation(compiler *c, JSAtom id, expr_ty annotation,
                                 int *annotations_len, location loc)
{
    JSAtom mangled;
    int ret;
    if (!annotation) {
        return SUCCESS;
    }
    mangled = _PyCompile_MaybeMangle(c, id);
    if (mangled == JS_ATOM_NULL)
        return ERROR;
    ret = qpc_load_str_atom(c, loc, mangled);
    JS_FreeAtom(c->ctx, mangled);
    RETURN_IF_ERROR(ret);

    if (FUTURE_FEATURES(c) & CO_FUTURE_ANNOTATIONS) {
        RETURN_IF_ERROR(codegen_visit_annexpr(c, annotation));
    }
    else {
        if (annotation->kind == Starred_kind) {
            /* *args: *Ts (where Ts is a TypeVarTuple).
               Do [annotation_value] = [*Ts]. */
            VISIT(c, expr, annotation->v.Starred.value);
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_unpack_seq, 1));
        }
        else {
            VISIT(c, expr, annotation);
        }
    }
    *annotations_len += 1;
    return SUCCESS;
}

static int codegen_argannotations(compiler *c, asdl_arg_seq *args,
                                  int *annotations_len, location loc)
{
    for (int i = 0; i < asdl_seq_LEN(args); i++) {
        arg_ty arg = (arg_ty)asdl_seq_GET(args, i);
        RETURN_IF_ERROR(codegen_argannotation(c, arg->arg, arg->annotation,
                                              annotations_len, loc));
    }
    return SUCCESS;
}

static int codegen_annotations_in_scope(compiler *c, location loc,
                                        arguments_ty args, expr_ty returns,
                                        int *annotations_len)
{
    RETURN_IF_ERROR(codegen_argannotations(c, args->posonlyargs, annotations_len, loc));
    RETURN_IF_ERROR(codegen_argannotations(c, args->args, annotations_len, loc));
    if (args->vararg && args->vararg->annotation) {
        RETURN_IF_ERROR(codegen_argannotation(c, args->vararg->arg,
                                              args->vararg->annotation, annotations_len, loc));
    }
    RETURN_IF_ERROR(codegen_argannotations(c, args->kwonlyargs, annotations_len, loc));
    if (args->kwarg && args->kwarg->annotation) {
        RETURN_IF_ERROR(codegen_argannotation(c, args->kwarg->arg,
                                              args->kwarg->annotation, annotations_len, loc));
    }
    RETURN_IF_ERROR(codegen_argannotation(c, c->ids.ret, returns, annotations_len, loc));
    return SUCCESS;
}

/* Return -1 on error, or a combination of flags to add to the function. */
static int codegen_function_annotations(compiler *c, location loc,
                                        arguments_ty args, expr_ty returns)
{
    int annotations_len = 0;
    PySTEntryObject *ste;

    _PySymtable_LookupOptional(SYMTABLE(c), args, &ste);
    if (ste == NULL)
        return qpc_internal_error(c, "unknown symbol table entry");
    if (ste->ste_annotations_used) {
        RETURN_IF_ERROR(codegen_setup_annotations_scope(c, loc, (void *)args, ste->ste_name));
        RETURN_IF_ERROR(codegen_annotations_in_scope(c, loc, args, returns, &annotations_len));
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, annotations_len));
        RETURN_IF_ERROR(codegen_finish_annotations_scope(c, loc));
        return QP_FUNC_ATTR_ANNOTATE;
    }
    return 0;
}

static int codegen_defaults(compiler *c, arguments_ty args, location loc)
{
    VISIT_SEQ(c, expr, args->defaults);
    return qpc_py_u16(c, loc, QP_OP_build_tuple, asdl_seq_LEN(args->defaults));
}

static int codegen_default_arguments(compiler *c, location loc, arguments_ty args)
{
    int funcflags = 0;
    if (args->defaults && asdl_seq_LEN(args->defaults) > 0) {
        RETURN_IF_ERROR(codegen_defaults(c, args, loc));
        funcflags |= QP_FUNC_ATTR_DEFAULTS;
    }
    if (args->kwonlyargs) {
        int res = codegen_kwonlydefaults(c, loc, args->kwonlyargs, args->kw_defaults);
        RETURN_IF_ERROR(res);
        if (res > 0) {
            funcflags |= QP_FUNC_ATTR_KWDEFAULTS;
        }
    }
    return funcflags;
}

/* CPython inserts SETUP_CLEANUP right after RETURN_GENERATOR; POP_TOP at
   the very start of the finished sequence.  Here the catch is emitted when
   the body starts (qpc_stopiteration_begin) and the handler at the end
   (codegen_wrap_in_stopiteration_handler). */
static int qpc_stopiteration_begin(compiler *c)
{
    NEW_JUMP_TARGET_LABEL(c, handler);
    c->u->u_stopiter_handler = handler;
    return qpc_setup(c, NO_LOCATION, handler);
}

static int codegen_wrap_in_stopiteration_handler(compiler *c)
{
    jump_target_label handler = c->u->u_stopiter_handler;
    assert(IS_JUMP_TARGET_LABEL(handler));
    RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_return(c, NO_LOCATION));
    USE_LABEL(c, handler);
    RETURN_IF_ERROR(qpc_py_u8(c, NO_LOCATION, QP_OP_intrinsic1,
                              QP_INTRINSIC1_STOPITERATION_ERROR));
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_reraise));
    c->u->u_stopiter_handler = NO_LABEL;
    return SUCCESS;
}

static int codegen_type_param_bound_or_default(compiler *c, expr_ty e,
                                               JSAtom name, void *key,
                                               bool allow_starred)
{
    JSValue one = JS_NewInt32(c->ctx, 1);
    RETURN_IF_ERROR(qpc_load_const_value(c, LOC(e), qp_tuple_new_steal(c->ctx, 1, &one)));
    RETURN_IF_ERROR(codegen_setup_annotations_scope(c, LOC(e), key, name));
    if (allow_starred && e->kind == Starred_kind) {
        VISIT(c, expr, e->v.Starred.value);
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(e), QP_OP_unpack_seq, 1));
    }
    else {
        VISIT(c, expr, e);
    }
    RETURN_IF_ERROR(qpc_return(c, LOC(e)));
    return qpc_finish_and_make_closure(c, 1, LOC(e), QP_FUNC_ATTR_DEFAULTS);
}

static int codegen_type_params(compiler *c, asdl_type_param_seq *type_params)
{
    if (!type_params) {
        return SUCCESS;
    }
    Py_ssize_t n = asdl_seq_LEN(type_params);
    bool seen_default = false;
    char buf[QPC_ATOM_BUF];

    for (Py_ssize_t i = 0; i < n; i++) {
        type_param_ty typeparam = asdl_seq_GET(type_params, i);
        location loc = LOC(typeparam);
        switch (typeparam->kind) {
        case TypeVar_kind:
            RETURN_IF_ERROR(qpc_load_str_atom(c, loc, typeparam->v.TypeVar.name));
            if (typeparam->v.TypeVar.bound) {
                expr_ty bound = typeparam->v.TypeVar.bound;
                RETURN_IF_ERROR(
                    codegen_type_param_bound_or_default(c, bound, typeparam->v.TypeVar.name,
                                                        (void *)typeparam, false));
                int intrinsic = bound->kind == Tuple_kind
                    ? QP_INTRINSIC2_TYPEVAR_WITH_CONSTRAINTS
                    : QP_INTRINSIC2_TYPEVAR_WITH_BOUND;
                RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic2, intrinsic));
            }
            else {
                RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_TYPEVAR));
            }
            if (typeparam->v.TypeVar.default_value) {
                seen_default = true;
                expr_ty default_ = typeparam->v.TypeVar.default_value;
                RETURN_IF_ERROR(
                    codegen_type_param_bound_or_default(c, default_, typeparam->v.TypeVar.name,
                                                        (void *)((uintptr_t)typeparam + 1), false));
                RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic2,
                                          QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT));
            }
            else if (seen_default) {
                return qpc_error(c, loc, "non-default type parameter '%s' "
                                 "follows default type parameter",
                                 qpc_atom_str(c, buf, typeparam->v.TypeVar.name));
            }
            RETURN_IF_ERROR(qpc_copy(c, loc, 1));
            RETURN_IF_ERROR(codegen_nameop(c, loc, typeparam->v.TypeVar.name, Store));
            break;
        case TypeVarTuple_kind:
            RETURN_IF_ERROR(qpc_load_str_atom(c, loc, typeparam->v.TypeVarTuple.name));
            RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_TYPEVARTUPLE));
            if (typeparam->v.TypeVarTuple.default_value) {
                expr_ty default_ = typeparam->v.TypeVarTuple.default_value;
                RETURN_IF_ERROR(
                    codegen_type_param_bound_or_default(c, default_, typeparam->v.TypeVarTuple.name,
                                                        (void *)typeparam, true));
                RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic2,
                                          QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT));
                seen_default = true;
            }
            else if (seen_default) {
                return qpc_error(c, loc, "non-default type parameter '%s' "
                                 "follows default type parameter",
                                 qpc_atom_str(c, buf, typeparam->v.TypeVarTuple.name));
            }
            RETURN_IF_ERROR(qpc_copy(c, loc, 1));
            RETURN_IF_ERROR(codegen_nameop(c, loc, typeparam->v.TypeVarTuple.name, Store));
            break;
        case ParamSpec_kind:
            RETURN_IF_ERROR(qpc_load_str_atom(c, loc, typeparam->v.ParamSpec.name));
            RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_PARAMSPEC));
            if (typeparam->v.ParamSpec.default_value) {
                expr_ty default_ = typeparam->v.ParamSpec.default_value;
                RETURN_IF_ERROR(
                    codegen_type_param_bound_or_default(c, default_, typeparam->v.ParamSpec.name,
                                                        (void *)typeparam, false));
                RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic2,
                                          QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT));
                seen_default = true;
            }
            else if (seen_default) {
                return qpc_error(c, loc, "non-default type parameter '%s' "
                                 "follows default type parameter",
                                 qpc_atom_str(c, buf, typeparam->v.ParamSpec.name));
            }
            RETURN_IF_ERROR(qpc_copy(c, loc, 1));
            RETURN_IF_ERROR(codegen_nameop(c, loc, typeparam->v.ParamSpec.name, Store));
            break;
        }
    }
    return qpc_py_u16(c, LOC(asdl_seq_GET(type_params, 0)), QP_OP_build_tuple, n);
}

static int codegen_emit_function_body(compiler *c, asdl_stmt_seq *body)
{
    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    Py_ssize_t first_instr = 0;
    if (ste->ste_has_docstring) {
        qp_const *docstring = qpc_get_docstring(body);
        first_instr = 1;
        if (docstring && c->c_optimize < 2) {
            int idx = qpc_add_const(c, _PyCompile_CleanDoc(c, docstring->v));
            if (idx < 0)
                return ERROR;
            c->u->u_fd->py_code->doc_cpool_idx = idx;
        }
    }

    NEW_JUMP_TARGET_LABEL(c, start);
    USE_LABEL(c, start);
    bool add_stopiteration_handler = ste->ste_coroutine || ste->ste_generator;
    if (add_stopiteration_handler) {
        /* codegen_wrap_in_stopiteration_handler will push a block, so we
           need to account for that */
        RETURN_IF_ERROR(
            _PyCompile_PushFBlock(c, NO_LOCATION, COMPILE_FBLOCK_STOP_ITERATION,
                                  start, NO_LABEL, NULL));
        RETURN_IF_ERROR(qpc_stopiteration_begin(c));
    }

    for (Py_ssize_t i = first_instr; i < asdl_seq_LEN(body); i++) {
        VISIT(c, stmt, (stmt_ty)asdl_seq_GET(body, i));
    }
    if (add_stopiteration_handler) {
        RETURN_IF_ERROR(codegen_wrap_in_stopiteration_handler(c));
        _PyCompile_PopFBlock(c, COMPILE_FBLOCK_STOP_ITERATION, start);
    }
    return SUCCESS;
}

static int codegen_function_body(compiler *c, stmt_ty s, int is_async, int funcflags,
                                 int firstlineno)
{
    arguments_ty args;
    JSAtom name;
    asdl_stmt_seq *body;
    int scope_type;

    if (is_async) {
        assert(s->kind == AsyncFunctionDef_kind);
        args = s->v.AsyncFunctionDef.args;
        name = s->v.AsyncFunctionDef.name;
        body = s->v.AsyncFunctionDef.body;
        scope_type = COMPILE_SCOPE_ASYNC_FUNCTION;
    } else {
        assert(s->kind == FunctionDef_kind);
        args = s->v.FunctionDef.args;
        name = s->v.FunctionDef.name;
        body = s->v.FunctionDef.body;
        scope_type = COMPILE_SCOPE_FUNCTION;
    }

    _PyCompile_CodeUnitMetadata umd = {
        .u_argcount = asdl_seq_LEN(args->args),
        .u_posonlyargcount = asdl_seq_LEN(args->posonlyargs),
        .u_kwonlyargcount = asdl_seq_LEN(args->kwonlyargs),
    };
    RETURN_IF_ERROR(
        codegen_enter_scope(c, name, scope_type, (void *)s, firstlineno, JS_ATOM_NULL, &umd));
    RETURN_IF_ERROR(qpc_scan_deletes(c, body));
    RETURN_IF_ERROR(codegen_emit_function_body(c, body));
    return qpc_finish_and_make_closure(c, 1, LOC(s), funcflags);
}

static int codegen_function(compiler *c, stmt_ty s, int is_async)
{
    arguments_ty args;
    expr_ty returns;
    JSAtom name;
    asdl_expr_seq *decos;
    asdl_type_param_seq *type_params;
    int funcflags;
    int firstlineno;

    if (is_async) {
        assert(s->kind == AsyncFunctionDef_kind);
        args = s->v.AsyncFunctionDef.args;
        returns = s->v.AsyncFunctionDef.returns;
        decos = s->v.AsyncFunctionDef.decorator_list;
        name = s->v.AsyncFunctionDef.name;
        type_params = s->v.AsyncFunctionDef.type_params;
    } else {
        assert(s->kind == FunctionDef_kind);
        args = s->v.FunctionDef.args;
        returns = s->v.FunctionDef.returns;
        decos = s->v.FunctionDef.decorator_list;
        name = s->v.FunctionDef.name;
        type_params = s->v.FunctionDef.type_params;
    }

    RETURN_IF_ERROR(codegen_decorators(c, decos));

    firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }

    location loc = LOC(s);
    int is_generic = asdl_seq_LEN(type_params) > 0;

    funcflags = codegen_default_arguments(c, loc, args);
    RETURN_IF_ERROR(funcflags);

    int num_typeparam_args = 0;

    if (is_generic) {
        if (funcflags & QP_FUNC_ATTR_DEFAULTS) {
            num_typeparam_args += 1;
        }
        if (funcflags & QP_FUNC_ATTR_KWDEFAULTS) {
            num_typeparam_args += 1;
        }
        if (num_typeparam_args == 2) {
            RETURN_IF_ERROR(qpc_swap(c, loc, 2));
        }
        JSAtom type_params_name = qpc_generic_params_name(c, name);
        if (type_params_name == JS_ATOM_NULL)
            return ERROR;
        _PyCompile_CodeUnitMetadata umd = {
            .u_argcount = num_typeparam_args,
        };
        int ret = codegen_enter_scope(c, type_params_name, COMPILE_SCOPE_ANNOTATIONS,
                                      (void *)type_params, firstlineno, JS_ATOM_NULL, &umd);
        JS_FreeAtom(c->ctx, type_params_name);
        RETURN_IF_ERROR(ret);
        RETURN_IF_ERROR(codegen_type_params(c, type_params));
        for (int i = 0; i < num_typeparam_args; i++) {
            RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_arg, i));
        }
    }

    int annotations_flag = codegen_function_annotations(c, loc, args, returns);
    RETURN_IF_ERROR(annotations_flag);
    funcflags |= annotations_flag;

    RETURN_IF_ERROR(codegen_function_body(c, s, is_async, funcflags, firstlineno));

    if (is_generic) {
        RETURN_IF_ERROR(qpc_swap(c, loc, 2));
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic2,
                                  QP_INTRINSIC2_SET_FUNCTION_TYPE_PARAMS));
        RETURN_IF_ERROR(qpc_finish_and_make_closure(c, 0, loc, 0));
        if (num_typeparam_args > 0) {
            RETURN_IF_ERROR(qpc_swap(c, loc, num_typeparam_args + 1));
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, num_typeparam_args - 1));
        }
        else {
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
        }
    }

    RETURN_IF_ERROR(codegen_apply_decorators(c, decos));
    return codegen_nameop(c, loc, name, Store);
}

/* ======================================================================== */
/* Classes                                                                   */
/* ======================================================================== */

static int codegen_set_type_params_in_class(compiler *c, location loc)
{
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.dot_type_params, Load));
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.__type_params__, Store));
    return SUCCESS;
}

/* _PyCompile_StaticAttributesAsTuple */
static int qpc_load_static_attributes(compiler *c, location loc)
{
    compiler_unit *u = c->u;
    Py_ssize_t pos = 0;
    JSAtom k;
    long v;
    JSAtom *names = NULL;
    int n = 0, size = 0, ret;
    while (qp_symmap_next(&u->u_static_attributes, &pos, &k, &v)) {
        if (js_resize_array(c->ctx, (void **)&names, sizeof(names[0]), &size, n + 1)) {
            js_free(c->ctx, names);
            return ERROR;
        }
        names[n++] = k;
    }
    if (qpc_sort_atoms(c, names, n) < 0) {
        js_free(c->ctx, names);
        return ERROR;
    }
    ret = qpc_load_atom_tuple(c, loc, names, n);
    js_free(c->ctx, names);
    return ret;
}

static int codegen_class_body(compiler *c, stmt_ty s, int firstlineno)
{
    /* ultimately generate code for:
         <name> = __build_class__(<func>, <name>, *<bases>, **<keywords>)
       where:
         <func> is a zero arg function/closure created from the class body.
            It mutates its locals to build the class namespace.
         <name> is the class name
         <bases> is the positional arguments and *varargs argument
         <keywords> is the keyword arguments and **kwds argument
       This borrows from codegen_call. */
    JSFunctionDef *fd;

    /* 1. compile the class body into a code object */
    RETURN_IF_ERROR(
        codegen_enter_scope(c, s->v.ClassDef.name, COMPILE_SCOPE_CLASS,
                            (void *)s, firstlineno, s->v.ClassDef.name, NULL));

    location loc = LOCATION(firstlineno, firstlineno, 0, 0);
    /* load (global) __name__ ... */
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.__name__, Load));
    /* ... and store it as __module__ */
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.__module__, Store));
    RETURN_IF_ERROR(qpc_load_str_atom(c, loc, QUALNAME(c)));
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.__qualname__, Store));
    RETURN_IF_ERROR(qpc_load_small_int(c, loc, c->u->u_firstlineno));
    RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.__firstlineno__, Store));
    asdl_type_param_seq *type_params = s->v.ClassDef.type_params;
    if (asdl_seq_LEN(type_params) > 0) {
        RETURN_IF_ERROR(codegen_set_type_params_in_class(c, loc));
    }
    if (SYMTABLE_ENTRY(c)->ste_needs_classdict) {
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_load_locals));
        /* STORE_DEREF __classdict__ (a cell of the class body) */
        RETURN_IF_ERROR(qpc_store_own_cell(c, loc, c->ids.__classdict__));
    }
    if (SYMTABLE_ENTRY(c)->ste_has_conditional_annotations) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_set, 0));
        RETURN_IF_ERROR(qpc_store_own_cell(c, loc, c->ids.__conditional_annotations__));
    }
    /* compile the body proper */
    RETURN_IF_ERROR(codegen_body(c, loc, s->v.ClassDef.body, false));
    RETURN_IF_ERROR(qpc_load_static_attributes(c, NO_LOCATION));
    RETURN_IF_ERROR(codegen_nameop(c, NO_LOCATION, c->ids.__static_attributes__, Store));
    /* The following code is artificial */
    /* Set __classdictcell__ if necessary */
    if (SYMTABLE_ENTRY(c)->ste_needs_classdict) {
        /* Store __classdictcell__ into class namespace */
        RETURN_IF_ERROR(qpc_load_closure(c, NO_LOCATION, c->ids.__classdict__));
        RETURN_IF_ERROR(codegen_nameop(c, NO_LOCATION, c->ids.__classdictcell__, Store));
    }
    /* Return __classcell__ if it is referenced, otherwise return None */
    if (SYMTABLE_ENTRY(c)->ste_needs_class_closure) {
        /* Store __classcell__ into class namespace & return it */
        RETURN_IF_ERROR(qpc_load_closure(c, NO_LOCATION, c->ids.__class__));
        RETURN_IF_ERROR(qpc_copy(c, NO_LOCATION, 1));
        RETURN_IF_ERROR(codegen_nameop(c, NO_LOCATION, c->ids.__classcell__, Store));
    }
    else {
        /* No methods referenced __class__, so just return None */
        RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
    }
    RETURN_IF_ERROR(qpc_return(c, NO_LOCATION));
    /* create the code object */
    fd = qpc_finish_unit(c, 1);
    if (!fd)
        return ERROR;
    /* leave the new scope */
    _PyCompile_ExitScope(c);

    /* 2. load the 'build_class' function */
    /* these instructions should be attributed to the class line,
       not a decorator line */
    loc = LOC(s);
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_load_build_class));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));

    /* 3. load a function (or closure) made from the code object */
    RETURN_IF_ERROR(codegen_make_closure(c, loc, fd, 0));

    /* 4. load class name */
    RETURN_IF_ERROR(qpc_load_str_atom(c, loc, s->v.ClassDef.name));
    return SUCCESS;
}

static int codegen_class(compiler *c, stmt_ty s)
{
    asdl_expr_seq *decos = s->v.ClassDef.decorator_list;

    RETURN_IF_ERROR(codegen_decorators(c, decos));

    int firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }
    location loc = LOC(s);

    asdl_type_param_seq *type_params = s->v.ClassDef.type_params;
    int is_generic = asdl_seq_LEN(type_params) > 0;
    if (is_generic) {
        JSAtom type_params_name = qpc_generic_params_name(c, s->v.ClassDef.name);
        if (type_params_name == JS_ATOM_NULL)
            return ERROR;
        int ret = codegen_enter_scope(c, type_params_name, COMPILE_SCOPE_ANNOTATIONS,
                                      (void *)type_params, firstlineno, s->v.ClassDef.name,
                                      NULL);
        JS_FreeAtom(c->ctx, type_params_name);
        RETURN_IF_ERROR(ret);
        RETURN_IF_ERROR(codegen_type_params(c, type_params));
        RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.dot_type_params, Store));
    }

    RETURN_IF_ERROR(codegen_class_body(c, s, firstlineno));

    /* generate the rest of the code for the call */
    if (is_generic) {
        RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.dot_type_params, Load));
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_SUBSCRIPT_GENERIC));
        RETURN_IF_ERROR(codegen_nameop(c, loc, c->ids.dot_generic_base, Store));

        RETURN_IF_ERROR(codegen_call_helper_impl(c, loc, 2,
                                                 s->v.ClassDef.bases,
                                                 c->ids.dot_generic_base,
                                                 s->v.ClassDef.keywords));
        RETURN_IF_ERROR(qpc_finish_and_make_closure(c, 0, loc, 0));
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
    } else {
        RETURN_IF_ERROR(codegen_call_helper(c, loc, 2,
                                            s->v.ClassDef.bases,
                                            s->v.ClassDef.keywords));
    }

    /* 6. apply decorators */
    RETURN_IF_ERROR(codegen_apply_decorators(c, decos));

    /* 7. store into <name> */
    return codegen_nameop(c, loc, s->v.ClassDef.name, Store);
}

/* ======================================================================== */
/* Type aliases, lambdas                                                     */
/* ======================================================================== */

static int codegen_typealias_body(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    JSAtom name = s->v.TypeAlias.name->v.Name.id;
    JSValue one = JS_NewInt32(c->ctx, 1);
    RETURN_IF_ERROR(qpc_load_const_value(c, loc, qp_tuple_new_steal(c->ctx, 1, &one)));
    RETURN_IF_ERROR(codegen_setup_annotations_scope(c, LOC(s), s, name));

    assert(!SYMTABLE_ENTRY(c)->ste_has_docstring);
    VISIT(c, expr, s->v.TypeAlias.value);
    RETURN_IF_ERROR(qpc_return(c, loc));
    RETURN_IF_ERROR(qpc_finish_and_make_closure(c, 0, loc, QP_FUNC_ATTR_DEFAULTS));

    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_tuple, 3));
    return qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_TYPEALIAS);
}

static int codegen_typealias(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    asdl_type_param_seq *type_params = s->v.TypeAlias.type_params;
    int is_generic = asdl_seq_LEN(type_params) > 0;
    JSAtom name = s->v.TypeAlias.name->v.Name.id;
    if (is_generic) {
        JSAtom type_params_name = qpc_generic_params_name(c, name);
        if (type_params_name == JS_ATOM_NULL)
            return ERROR;
        int ret = codegen_enter_scope(c, type_params_name, COMPILE_SCOPE_ANNOTATIONS,
                                      (void *)type_params, loc.lineno, JS_ATOM_NULL, NULL);
        JS_FreeAtom(c->ctx, type_params_name);
        RETURN_IF_ERROR(ret);
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, name));
        RETURN_IF_ERROR(codegen_type_params(c, type_params));
    }
    else {
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, name));
        RETURN_IF_ERROR(qpc_load_none(c, loc));
    }

    RETURN_IF_ERROR(codegen_typealias_body(c, s));

    if (is_generic) {
        RETURN_IF_ERROR(qpc_finish_and_make_closure(c, 0, loc, 0));
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
    }
    return codegen_nameop(c, loc, name, Store);
}

static int codegen_lambda(compiler *c, expr_ty e)
{
    int funcflags;
    arguments_ty args = e->v.Lambda.args;
    assert(e->kind == Lambda_kind);

    location loc = LOC(e);
    funcflags = codegen_default_arguments(c, loc, args);
    RETURN_IF_ERROR(funcflags);

    _PyCompile_CodeUnitMetadata umd = {
        .u_argcount = asdl_seq_LEN(args->args),
        .u_posonlyargcount = asdl_seq_LEN(args->posonlyargs),
        .u_kwonlyargcount = asdl_seq_LEN(args->kwonlyargs),
    };
    RETURN_IF_ERROR(
        codegen_enter_scope(c, c->ids.anon_lambda, COMPILE_SCOPE_LAMBDA,
                            (void *)e, e->lineno, JS_ATOM_NULL, &umd));

    assert(!SYMTABLE_ENTRY(c)->ste_has_docstring);

    VISIT(c, expr, e->v.Lambda.body);
    if (SYMTABLE_ENTRY(c)->ste_generator) {
        return qpc_finish_and_make_closure(c, 0, loc, funcflags);
    }
    RETURN_IF_ERROR(qpc_return(c, LOC(e->v.Lambda.body)));
    return qpc_finish_and_make_closure(c, 1, loc, funcflags);
}

/* ======================================================================== */
/* Comprehensions                                                            */
/* ======================================================================== */

static int codegen_sync_comprehension_generator(
    compiler *c, location loc, asdl_comprehension_seq *generators, int gen_index,
    int depth, expr_ty elt, expr_ty val, int type, IterStackPosition iter_pos);
static int codegen_async_comprehension_generator(
    compiler *c, location loc, asdl_comprehension_seq *generators, int gen_index,
    int depth, expr_ty elt, expr_ty val, int type, IterStackPosition iter_pos);

static int codegen_comprehension_generator(compiler *c, location loc,
                                           asdl_comprehension_seq *generators, int gen_index,
                                           int depth, expr_ty elt, expr_ty val, int type,
                                           IterStackPosition iter_pos)
{
    comprehension_ty gen;
    gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);
    if (gen->is_async) {
        return codegen_async_comprehension_generator(
            c, loc, generators, gen_index, depth, elt, val, type, iter_pos);
    } else {
        return codegen_sync_comprehension_generator(
            c, loc, generators, gen_index, depth, elt, val, type, iter_pos);
    }
}

/* Element handling of the innermost generator (shared by sync/async). */
static int qpc_comprehension_element(compiler *c, expr_ty elt, expr_ty val,
                                     int type, int depth)
{
    location elt_loc = LOC(elt);
    switch (type) {
    case COMP_GENEXP:
        if (elt->kind == Starred_kind) {
            NEW_JUMP_TARGET_LABEL(c, unpack_start);
            NEW_JUMP_TARGET_LABEL(c, unpack_end);
            VISIT(c, expr, elt->v.Starred.value);
            RETURN_IF_ERROR(qpc_py_u8(c, elt_loc, QP_OP_get_iter, QP_GETITER_NORMAL));
            USE_LABEL(c, unpack_start);
            RETURN_IF_ERROR(qpc_py_jump(c, elt_loc, QP_OP_for_iter, unpack_end));
            ADDOP_YIELD(c, elt_loc);
            RETURN_IF_ERROR(qpc_op(c, elt_loc, OP_drop));
            RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, unpack_start));
            USE_LABEL(c, unpack_end);
            /* END_FOR not emitted; POP_ITER */
            RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
            RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        }
        else {
            VISIT(c, expr, elt);
            ADDOP_YIELD(c, elt_loc);
            RETURN_IF_ERROR(qpc_op(c, elt_loc, OP_drop));
        }
        break;
    case COMP_LISTCOMP:
        if (elt->kind == Starred_kind) {
            VISIT(c, expr, elt->v.Starred.value);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_list_extend, depth + 1));
        }
        else {
            VISIT(c, expr, elt);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_list_append, depth + 1));
        }
        break;
    case COMP_SETCOMP:
        if (elt->kind == Starred_kind) {
            VISIT(c, expr, elt->v.Starred.value);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_set_update, depth + 1));
        }
        else {
            VISIT(c, expr, elt);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_set_add, depth + 1));
        }
        break;
    case COMP_DICTCOMP:
        if (val == NULL) {
            /* unpacking (**) case */
            VISIT(c, expr, elt);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_dict_update, depth + 1));
        }
        else {
            /* With '{k: v}', k is evaluated before v, so we do the same. */
            VISIT(c, expr, elt);
            VISIT(c, expr, val);
            elt_loc = LOCATION(elt->lineno, val->end_lineno,
                               elt->col_offset, val->end_col_offset);
            RETURN_IF_ERROR(qpc_py_u16(c, elt_loc, QP_OP_map_add, depth + 1));
        }
        break;
    default:
        return qpc_internal_error(c, "unknown comprehension type %d", type);
    }
    return SUCCESS;
}

static int codegen_sync_comprehension_generator(compiler *c, location loc,
                                                asdl_comprehension_seq *generators,
                                                int gen_index, int depth,
                                                expr_ty elt, expr_ty val, int type,
                                                IterStackPosition iter_pos)
{
    /* generate code for the iterator, then each of the ifs,
       and then write to the element */
    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, if_cleanup);
    NEW_JUMP_TARGET_LABEL(c, anchor);

    comprehension_ty gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);

    if (iter_pos == ITERABLE_IN_LOCAL) {
        if (gen_index == 0) {
            assert(c->u->u_argcount == 1);
            RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_arg, 0));
        }
        else {
            /* Sub-iter - calculate on the fly */
            /* Fast path for the temporary variable assignment idiom:
                for y in [f(x)]
            */
            asdl_expr_seq *elts;
            switch (gen->iter->kind) {
            case List_kind:
                elts = gen->iter->v.List.elts;
                break;
            case Tuple_kind:
                elts = gen->iter->v.Tuple.elts;
                break;
            default:
                elts = NULL;
            }
            if (asdl_seq_LEN(elts) == 1) {
                expr_ty elt1 = asdl_seq_GET(elts, 0);
                if (elt1->kind != Starred_kind) {
                    VISIT(c, expr, elt1);
                    start = NO_LABEL;
                }
            }
            if (IS_JUMP_TARGET_LABEL(start)) {
                VISIT(c, expr, gen->iter);
            }
        }
    }

    if (IS_JUMP_TARGET_LABEL(start)) {
        if (iter_pos != ITERATOR_ON_STACK) {
            RETURN_IF_ERROR(qpc_py_u8(c, LOC(gen->iter), QP_OP_get_iter, QP_GETITER_NORMAL));
            depth += 1;
        }
        USE_LABEL(c, start);
        depth += 1;
        RETURN_IF_ERROR(qpc_py_jump(c, LOC(gen->iter), QP_OP_for_iter, anchor));
    }
    VISIT(c, expr, gen->target);

    Py_ssize_t n = asdl_seq_LEN(gen->ifs);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
        RETURN_IF_ERROR(codegen_jump_if(c, loc, e, if_cleanup, 0));
    }

    if (++gen_index < asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(
            codegen_comprehension_generator(c, loc, generators, gen_index, depth,
                                            elt, val, type, ITERABLE_IN_LOCAL));
    }

    location elt_loc = LOC(elt);

    /* only append after the last for generator */
    if (gen_index >= asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(qpc_comprehension_element(c, elt, val, type, depth));
        if (type == COMP_DICTCOMP && val != NULL)
            elt_loc = LOCATION(elt->lineno, val->end_lineno,
                               elt->col_offset, val->end_col_offset);
    }

    USE_LABEL(c, if_cleanup);
    if (IS_JUMP_TARGET_LABEL(start)) {
        RETURN_IF_ERROR(qpc_jump(c, elt_loc, OP_goto, start));
        USE_LABEL(c, anchor);
        /* END_FOR is not emitted; POP_ITER = two drops */
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    }
    return SUCCESS;
}

static int codegen_async_comprehension_generator(compiler *c, location loc,
                                                 asdl_comprehension_seq *generators,
                                                 int gen_index, int depth,
                                                 expr_ty elt, expr_ty val, int type,
                                                 IterStackPosition iter_pos)
{
    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, if_cleanup);

    comprehension_ty gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);

    if (iter_pos == ITERABLE_IN_LOCAL) {
        if (gen_index == 0) {
            assert(c->u->u_argcount == 1);
            RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_get_arg, 0));
        }
        else {
            /* Sub-iter - calculate on the fly */
            VISIT(c, expr, gen->iter);
        }
    }
    if (iter_pos != ITERATOR_ON_STACK) {
        RETURN_IF_ERROR(qpc_py(c, LOC(gen->iter), QP_OP_get_aiter));
    }

    USE_LABEL(c, start);
    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_ASYNC_COMPREHENSION_GENERATOR,
                              start, NO_LABEL, NULL));

    /* [aiter] catch -> [aiter, M]; the marker hides aiter from get_anext:
       copy it above the marker, then drop the copy:
       [aiter, M, aiter] get_anext -> [aiter, M, aiter, awaitable]
       swap, drop -> [aiter, M, awaitable]; push_null; yield from ->
       [aiter, M, value]; POP_BLOCK under the value -> [aiter, value].
       Handler `except` sees [aiter, exc] (END_ASYNC_FOR). */
    RETURN_IF_ERROR(qpc_setup(c, loc, except));
    RETURN_IF_ERROR(qpc_copy(c, loc, 2));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_get_anext));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_swap));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    ADD_YIELD_FROM(c, loc, 1);
    RETURN_IF_ERROR(qpc_pop_block_under(c, loc));
    VISIT(c, expr, gen->target);

    Py_ssize_t n = asdl_seq_LEN(gen->ifs);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
        RETURN_IF_ERROR(codegen_jump_if(c, loc, e, if_cleanup, 0));
    }

    depth++;
    if (++gen_index < asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(
            codegen_comprehension_generator(c, loc, generators, gen_index, depth,
                                            elt, val, type, ITERABLE_IN_LOCAL));
    }

    location elt_loc = LOC(elt);
    /* only append after the last for generator */
    if (gen_index >= asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(qpc_comprehension_element(c, elt, val, type, depth));
        if (type == COMP_DICTCOMP && val != NULL)
            elt_loc = LOCATION(elt->lineno, val->end_lineno,
                               elt->col_offset, val->end_col_offset);
    }

    USE_LABEL(c, if_cleanup);
    RETURN_IF_ERROR(qpc_jump(c, elt_loc, OP_goto, start));

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_ASYNC_COMPREHENSION_GENERATOR, start);

    USE_LABEL(c, except);
    /* [aiter, exc] -> [] (StopAsyncIteration ends the loop) */
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_end_async_for));
    return SUCCESS;
}

/* _PyCompile_TweakInlinedComprehensionScopes + push of the comprehension
   locals (own hidden slots instead of LOAD_FAST_AND_CLEAR). */
static int push_inlined_comprehension_state(compiler *c, location loc,
                                            PySTEntryObject *comp,
                                            _PyCompile_InlinedComprehensionState *state)
{
    compiler_unit *u = c->u;
    PySTEntryObject *outer = u->u_ste;
    int in_class_block = (outer->ste_type == ClassBlock) && !u->u_in_inlined_comp;
    Py_ssize_t pos = 0;
    JSAtom k;
    long symbol;

    qp_symmap_init(&state->temp_symbols, c->ctx->rt);
    qp_symmap_init(&state->fast_hidden, c->ctx->rt);
    state->has_temp_symbols = 1;
    state->has_fast_hidden = 1;
    state->n_overrides_before = u->u_n_overrides;
    u->u_in_inlined_comp++;

    while (qp_symmap_next(&comp->ste_symbols, &pos, &k, &symbol)) {
        long scope = SYMBOL_TO_SCOPE(symbol);
        long outsymbol = _PyST_GetSymbol(outer, k);
        long outsc = SYMBOL_TO_SCOPE(outsymbol);

        /* If a name has different scope inside than outside the
           comprehension, we need to temporarily handle it with the right
           scope while compiling the comprehension. */
        if ((scope != outsc && scope != FREE && !(scope == CELL && outsc == FREE))
                || in_class_block) {
            if (!qp_symmap_contains(&state->temp_symbols, k)) {
                if (qp_symmap_set(&state->temp_symbols, k, outsymbol) < 0)
                    goto oom;
            }
            if (qp_symmap_set(&outer->ste_symbols, k, symbol) < 0)
                goto oom;
        }
        /* locals handling for names bound in comprehension (DEF_LOCAL |
           DEF_NONLOCAL occurs in assignment expression to nonlocal) */
        if (((symbol & DEF_LOCAL) && !(symbol & DEF_NONLOCAL)) || in_class_block) {
            if (!_PyST_IsFunctionLike(outer)) {
                /* non-function scope: override this name to use fast locals */
                long orig = 0;
                qp_symmap_lookup(&u->u_fasthidden, k, &orig);
                if (!orig) {
                    if (qp_symmap_set(&u->u_fasthidden, k, 1) < 0 ||
                        qp_symmap_set(&state->fast_hidden, k, 1) < 0)
                        goto oom;
                }
            }
            /* isolate the comprehension's own binding of k: a fresh hidden
               local (the comprehension parameter .0 is never used) */
            if (!(symbol & DEF_PARAM) && (scope == LOCAL || scope == CELL)) {
                qpc_slot_override *o;
                int idx = add_var(c->ctx, u->u_fd, k);
                if (idx < 0)
                    return ERROR;
                u->u_fd->vars[idx].is_lexical = 1;
                if (scope == CELL)
                    qpc_slot_capture(u, QPC_SLOT_VAR, idx);
                if (js_resize_array(c->ctx, (void **)&u->u_overrides,
                                    sizeof(u->u_overrides[0]),
                                    &u->u_overrides_size, u->u_n_overrides + 1))
                    return ERROR;
                o = &u->u_overrides[u->u_n_overrides++];
                o->name = u->u_fd->vars[idx].var_name;
                o->var_idx = idx;
                o->depth = u->u_in_inlined_comp;
            }
        }
    }
    return SUCCESS;
 oom:
    JS_ThrowOutOfMemory(c->ctx);
    return ERROR;
}

/* Close and clear the comprehension's hidden locals, then
   _PyCompile_RevertInlinedComprehensionScopes. */
static int pop_inlined_comprehension_state(compiler *c, location loc,
                                           _PyCompile_InlinedComprehensionState *state)
{
    compiler_unit *u = c->u;
    Py_ssize_t pos = 0;
    JSAtom k;
    long v;

    for (int i = state->n_overrides_before; i < u->u_n_overrides; i++) {
        int idx = u->u_overrides[i].var_idx;
        if (u->u_fd->vars[idx].is_captured)
            RETURN_IF_ERROR(qpc_op_u16(c, NO_LOCATION, OP_close_loc, idx));
        RETURN_IF_ERROR(qpc_op_u16(c, NO_LOCATION, OP_set_loc_uninitialized, idx));
    }
    u->u_n_overrides = state->n_overrides_before;
    u->u_in_inlined_comp--;
    if (state->has_temp_symbols) {
        while (qp_symmap_next(&state->temp_symbols, &pos, &k, &v)) {
            if (qp_symmap_set(&u->u_ste->ste_symbols, k, v) < 0) {
                JS_ThrowOutOfMemory(c->ctx);
                return ERROR;
            }
        }
        qp_symmap_free(&state->temp_symbols);
        state->has_temp_symbols = 0;
    }
    if (state->has_fast_hidden) {
        pos = 0;
        while (qp_symmap_next(&state->fast_hidden, &pos, &k, &v)) {
            /* set to 0 instead of clearing (CPython: False) */
            if (qp_symmap_set(&u->u_fasthidden, k, 0) < 0) {
                JS_ThrowOutOfMemory(c->ctx);
                return ERROR;
            }
        }
        qp_symmap_free(&state->fast_hidden);
        state->has_fast_hidden = 0;
    }
    return SUCCESS;
}

static void qpc_inline_state_free(_PyCompile_InlinedComprehensionState *state)
{
    if (state->has_temp_symbols)
        qp_symmap_free(&state->temp_symbols);
    if (state->has_fast_hidden)
        qp_symmap_free(&state->fast_hidden);
    state->has_temp_symbols = state->has_fast_hidden = 0;
}

static int codegen_comprehension_init_container(compiler *c, location loc, int type,
                                                int is_inlined)
{
    switch (type) {
    case COMP_LISTCOMP:
        RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_array_from, 0));
        break;
    case COMP_SETCOMP:
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_set, 0));
        break;
    case COMP_DICTCOMP:
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0));
        break;
    default:
        return qpc_internal_error(c, "unknown comprehension type %d", type);
    }
    if (is_inlined) {
        RETURN_IF_ERROR(qpc_swap(c, loc, 2));
    }
    return SUCCESS;
}

static int codegen_comprehension(compiler *c, expr_ty e, int type,
                                 JSAtom name, asdl_comprehension_seq *generators,
                                 expr_ty elt, expr_ty val)
{
    _PyCompile_InlinedComprehensionState inline_state;
    comprehension_ty outermost;
    PySTEntryObject *entry = _PySymtable_Lookup(SYMTABLE(c), (void *)e);
    JSFunctionDef *fd;
    int ret;

    memset(&inline_state, 0, sizeof(inline_state));
    if (entry == NULL) {
        return ERROR;
    }
    int is_inlined = entry->ste_comp_inlined;
    int is_async_comprehension = entry->ste_coroutine;

    location loc = LOC(e);

    outermost = (comprehension_ty)asdl_seq_GET(generators, 0);
    IterStackPosition iter_state;
    if (!is_inlined) {
        compiler_unit *u = c->u;
        for (int i = 0; i < u->u_n_comp_codes; i++) {
            if (u->u_comp_codes[i].key == (void *)e) {
                RETURN_IF_ERROR(qpc_op_u32(c, loc, OP_fclosure, u->u_comp_codes[i].idx));
                goto make_call;
            }
        }
    }
    if (is_inlined) {
        VISIT(c, expr, outermost->iter);
        ret = push_inlined_comprehension_state(c, loc, entry, &inline_state);
        if (ret < 0) {
            qpc_inline_state_free(&inline_state);
            return ERROR;
        }
        iter_state = ITERABLE_ON_STACK;
    }
    else {
        /* Receive outermost iter as an implicit argument */
        _PyCompile_CodeUnitMetadata umd = {
            .u_argcount = 1,
        };
        RETURN_IF_ERROR(codegen_enter_scope(c, name, COMPILE_SCOPE_COMPREHENSION,
                                            (void *)e, e->lineno, JS_ATOM_NULL, &umd));
        if (type == COMP_GENEXP) {
            /* codegen_wrap_in_stopiteration_handler: CPython's SETUP_CLEANUP
               pushes nothing; our catch marker must sit BELOW the iterator
               pair, so it comes first: [M, iter, idx]. */
            RETURN_IF_ERROR(qpc_stopiteration_begin(c));
            /* CPython inserts `LOAD_FAST 0; GET_ITER/GET_AITER` before
               RETURN_GENERATOR.  The caller already applied iter() (resp.
               aiter()) to the argument (see below); a sync genexp gets
               its [iter, idx] pair here. */
            RETURN_IF_ERROR(qpc_op_u16(c, LOC(outermost->iter), OP_get_arg, 0));
            if (!outermost->is_async) {
                RETURN_IF_ERROR(qpc_py_u8(c, LOC(outermost->iter), QP_OP_get_iter,
                                          QP_GETITER_NORMAL));
            }
            iter_state = ITERATOR_ON_STACK;
        }
        else {
            iter_state = ITERABLE_IN_LOCAL;
        }
    }

    if (type != COMP_GENEXP) {
        if (codegen_comprehension_init_container(c, loc, type, is_inlined) < 0)
            goto error;
    }
    if (codegen_comprehension_generator(c, loc, generators, 0, 0,
                                        elt, val, type, iter_state) < 0) {
        goto error;
    }

    if (is_inlined) {
        ret = pop_inlined_comprehension_state(c, loc, &inline_state);
        qpc_inline_state_free(&inline_state);
        return ret;
    }

    if (type != COMP_GENEXP) {
        RETURN_IF_ERROR(qpc_return(c, LOC(e)));
    }
    if (type == COMP_GENEXP) {
        RETURN_IF_ERROR(codegen_wrap_in_stopiteration_handler(c));
    }

    fd = qpc_finish_unit(c, 1);
    if (!fd)
        return ERROR;
    _PyCompile_ExitScope(c);

    loc = LOC(e);
    RETURN_IF_ERROR(codegen_make_closure(c, loc, fd, 0));
    {
        compiler_unit *u = c->u;
        if (js_resize_array(c->ctx, (void **)&u->u_comp_codes, sizeof(u->u_comp_codes[0]),
                            &u->u_comp_codes_size, u->u_n_comp_codes + 1))
            return ERROR;
        u->u_comp_codes[u->u_n_comp_codes].key = (void *)e;
        u->u_comp_codes[u->u_n_comp_codes++].idx = fd->parent_cpool_idx;
    }

make_call:
    VISIT(c, expr, outermost->iter);
    if (type == COMP_GENEXP) {
        /* the genexp's GET_ITER / GET_AITER runs at creation time */
        if (outermost->is_async) {
            RETURN_IF_ERROR(qpc_py(c, LOC(outermost->iter), QP_OP_get_aiter));
        }
        else {
            RETURN_IF_ERROR(qpc_py_u8(c, LOC(outermost->iter), QP_OP_get_iter,
                                      QP_GETITER_NORMAL));
            RETURN_IF_ERROR(qpc_op(c, LOC(outermost->iter), OP_drop));
        }
    }
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));

    if (is_async_comprehension && type != COMP_GENEXP) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_awaitable, QP_AWAITABLE_AWAIT));
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
        ADD_YIELD_FROM(c, loc, 1);
    }
    return SUCCESS;

 error:
    qpc_inline_state_free(&inline_state);
    return ERROR;
}

static int codegen_genexp(compiler *c, expr_ty e)
{
    assert(e->kind == GeneratorExp_kind);
    return codegen_comprehension(c, e, COMP_GENEXP, c->ids.anon_genexpr,
                                 e->v.GeneratorExp.generators,
                                 e->v.GeneratorExp.elt, NULL);
}

static int codegen_listcomp(compiler *c, expr_ty e)
{
    assert(e->kind == ListComp_kind);
    return codegen_comprehension(c, e, COMP_LISTCOMP, c->ids.anon_listcomp,
                                 e->v.ListComp.generators,
                                 e->v.ListComp.elt, NULL);
}

static int codegen_setcomp(compiler *c, expr_ty e)
{
    assert(e->kind == SetComp_kind);
    return codegen_comprehension(c, e, COMP_SETCOMP, c->ids.anon_setcomp,
                                 e->v.SetComp.generators,
                                 e->v.SetComp.elt, NULL);
}

static int codegen_dictcomp(compiler *c, expr_ty e)
{
    assert(e->kind == DictComp_kind);
    return codegen_comprehension(c, e, COMP_DICTCOMP, c->ids.anon_dictcomp,
                                 e->v.DictComp.generators,
                                 e->v.DictComp.key, e->v.DictComp.value);
}

/* ======================================================================== */
/* ast_unparse, expressions, statements, pattern matching                   */
/* ======================================================================== */

/* ---------------------------------------------------------------------- */
/*
 * Port of CPython Python/ast_unparse.c (3.15): _PyAST_ExprAsUnicode, used
 * for `from __future__ import annotations` (stringified annotations).
 * Output is byte-identical to CPython.  The writer is a QuickJS StringBuffer
 * so lone surrogates in identifiers/strings survive.  Functions return 0 on
 * success, -1 on error (exception pending in ctx).
 */

/* Priority levels */
enum {
    QPC_PR_TUPLE,
    QPC_PR_TEST,            /* 'if'-'else', 'lambda' */
    QPC_PR_OR,              /* 'or' */
    QPC_PR_AND,             /* 'and' */
    QPC_PR_NOT,             /* 'not' */
    QPC_PR_CMP,             /* '<', '>', '==', '>=', '<=', '!=',
                               'in', 'not in', 'is', 'is not' */
    QPC_PR_EXPR,
    QPC_PR_BOR = QPC_PR_EXPR, /* '|' */
    QPC_PR_BXOR,            /* '^' */
    QPC_PR_BAND,            /* '&' */
    QPC_PR_SHIFT,           /* '<<', '>>' */
    QPC_PR_ARITH,           /* '+', '-' */
    QPC_PR_TERM,            /* '*', '@', '/', '%', '//' */
    QPC_PR_FACTOR,          /* unary '+', '-', '~' */
    QPC_PR_POWER,           /* '**' */
    QPC_PR_AWAIT,           /* 'await' */
    QPC_PR_ATOM,
};

static JSValue qpc_unparse_expr_as_unicode(JSContext *ctx, expr_ty e, int level);
static int qpc_unparse_expr(StringBuffer *w, expr_ty e, int level);
static int qpc_unparse_templatestr(StringBuffer *w, expr_ty e);
static int qpc_unparse_joinedstr(StringBuffer *w, expr_ty e, BOOL is_format_spec);
static int qpc_unparse_interpolation(StringBuffer *w, expr_ty e);
static int qpc_unparse_formattedvalue(StringBuffer *w, expr_ty e);

#define QPC_U_CHAR(ch)  do { \
        if (string_buffer_putc(w, (ch))) return -1; \
    } while (0)
#define QPC_U_STR(str)  do { \
        if (string_buffer_puts8(w, (str))) return -1; \
    } while (0)
#define QPC_U_STR_IF(cond, str)  do { \
        if ((cond) && string_buffer_puts8(w, (str))) return -1; \
    } while (0)
#define QPC_U_STR_IF_NOT_FIRST(str)  do { \
        QPC_U_STR_IF(!first, (str)); \
        first = FALSE; \
    } while (0)
#define QPC_U_EXPR(expr, pr)  do { \
        if (qpc_unparse_expr(w, (expr), (pr))) return -1; \
    } while (0)

static int qpc_unparse_system_error(StringBuffer *w, const char *msg)
{
    JS_ThrowInternalError(w->ctx, "%s", msg);
    return string_buffer_set_error(w);
}

/* Append the text of an identifier/string atom. */
static int qpc_unparse_atom(StringBuffer *w, JSAtom atom)
{
    return string_buffer_concat_value_free(w, JS_AtomToString(w->ctx, atom));
}

/* Append ASCII text, replacing every "inf" by "1e309" (append_repr rule
   for float and complex constants). */
static int qpc_unparse_ascii_noinf(StringBuffer *w, const char *s, size_t len)
{
    size_t i = 0, start = 0;
    while (i < len) {
        if (i + 3 <= len && s[i] == 'i' && s[i + 1] == 'n' && s[i + 2] == 'f') {
            if (string_buffer_write8(w, (const uint8_t *)s + start, i - start) ||
                string_buffer_puts8(w, "1e309"))
                return -1;
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    return string_buffer_write8(w, (const uint8_t *)s + start, len - start);
}

/* repr() of a constant (append_ast_constant + append_repr). */
static int qpc_unparse_constant(StringBuffer *w, qp_const *c)
{
    JSContext *ctx = w->ctx;
    switch (c->kind) {
    case QP_CONST_NONE:
        return string_buffer_puts8(w, "None");
    case QP_CONST_TRUE:
        return string_buffer_puts8(w, "True");
    case QP_CONST_FALSE:
        return string_buffer_puts8(w, "False");
    case QP_CONST_ELLIPSIS:
        return string_buffer_puts8(w, "Ellipsis");
    case QP_CONST_INT:
        /* decimal text of int / short bigint / bigint */
        return string_buffer_concat_value(w, c->v);
    case QP_CONST_FLOAT: {
        char buf[QP_FLOAT_REPR_MAX];
        int len = qp_float_repr(JS_VALUE_GET_FLOAT64(c->v), buf);
        if (len < 0) {
            JS_ThrowOutOfMemory(ctx);
            return string_buffer_set_error(w);
        }
        return qpc_unparse_ascii_noinf(w, buf, len);
    }
    case QP_CONST_COMPLEX: {
        DynBuf db;
        int ret;
        js_dbuf_init(ctx, &db);
        if (qp_complex_repr(0.0, JS_VALUE_GET_FLOAT64(c->v), &db) < 0 ||
            dbuf_error(&db)) {
            dbuf_free(&db);
            JS_ThrowOutOfMemory(ctx);
            return string_buffer_set_error(w);
        }
        ret = qpc_unparse_ascii_noinf(w, (const char *)db.buf, db.size);
        dbuf_free(&db);
        return ret;
    }
    case QP_CONST_STR: {
        JSValue r = qp_str_repr(ctx, c->v);
        if (JS_IsException(r))
            return string_buffer_set_error(w);
        return string_buffer_concat_value_free(w, r);
    }
    case QP_CONST_BYTES: {
        JSString *p = JS_VALUE_GET_STRING(c->v);
        JSValue r = qp_bytes_repr(ctx, p->u.str8, p->len, 1);
        if (JS_IsException(r))
            return string_buffer_set_error(w);
        return string_buffer_concat_value_free(w, r);
    }
    case QP_CONST_OBJECT: {
        JSValue r = qp_repr(ctx, c->v);
        if (JS_IsException(r))
            return string_buffer_set_error(w);
        return string_buffer_concat_value_free(w, r);
    }
    }
    return qpc_unparse_system_error(w, "unknown constant kind");
}

static int qpc_unparse_boolop(StringBuffer *w, expr_ty e, int level)
{
    Py_ssize_t i, value_count;
    asdl_expr_seq *values;
    const char *op = (e->v.BoolOp.op == And) ? " and " : " or ";
    int pr = (e->v.BoolOp.op == And) ? QPC_PR_AND : QPC_PR_OR;

    QPC_U_STR_IF(level > pr, "(");

    values = e->v.BoolOp.values;
    value_count = asdl_seq_LEN(values);

    for (i = 0; i < value_count; ++i) {
        QPC_U_STR_IF(i > 0, op);
        QPC_U_EXPR((expr_ty)asdl_seq_GET(values, i), pr + 1);
    }

    QPC_U_STR_IF(level > pr, ")");
    return 0;
}

static int qpc_unparse_binop(StringBuffer *w, expr_ty e, int level)
{
    const char *op;
    int pr;
    BOOL rassoc = FALSE;  /* is right-associative? */

    switch (e->v.BinOp.op) {
    case Add: op = " + "; pr = QPC_PR_ARITH; break;
    case Sub: op = " - "; pr = QPC_PR_ARITH; break;
    case Mult: op = " * "; pr = QPC_PR_TERM; break;
    case MatMult: op = " @ "; pr = QPC_PR_TERM; break;
    case Div: op = " / "; pr = QPC_PR_TERM; break;
    case Mod: op = " % "; pr = QPC_PR_TERM; break;
    case LShift: op = " << "; pr = QPC_PR_SHIFT; break;
    case RShift: op = " >> "; pr = QPC_PR_SHIFT; break;
    case BitOr: op = " | "; pr = QPC_PR_BOR; break;
    case BitXor: op = " ^ "; pr = QPC_PR_BXOR; break;
    case BitAnd: op = " & "; pr = QPC_PR_BAND; break;
    case FloorDiv: op = " // "; pr = QPC_PR_TERM; break;
    case Pow: op = " ** "; pr = QPC_PR_POWER; rassoc = TRUE; break;
    default:
        return qpc_unparse_system_error(w, "unknown binary operator");
    }

    QPC_U_STR_IF(level > pr, "(");
    QPC_U_EXPR(e->v.BinOp.left, pr + rassoc);
    QPC_U_STR(op);
    QPC_U_EXPR(e->v.BinOp.right, pr + !rassoc);
    QPC_U_STR_IF(level > pr, ")");
    return 0;
}

static int qpc_unparse_unaryop(StringBuffer *w, expr_ty e, int level)
{
    const char *op;
    int pr;

    switch (e->v.UnaryOp.op) {
    case Invert: op = "~"; pr = QPC_PR_FACTOR; break;
    case Not: op = "not "; pr = QPC_PR_NOT; break;
    case UAdd: op = "+"; pr = QPC_PR_FACTOR; break;
    case USub: op = "-"; pr = QPC_PR_FACTOR; break;
    default:
        return qpc_unparse_system_error(w, "unknown unary operator");
    }

    QPC_U_STR_IF(level > pr, "(");
    QPC_U_STR(op);
    QPC_U_EXPR(e->v.UnaryOp.operand, pr);
    QPC_U_STR_IF(level > pr, ")");
    return 0;
}

static int qpc_unparse_arg(StringBuffer *w, arg_ty arg)
{
    if (qpc_unparse_atom(w, arg->arg))
        return -1;
    if (arg->annotation) {
        QPC_U_STR(": ");
        QPC_U_EXPR(arg->annotation, QPC_PR_TEST);
    }
    return 0;
}

static int qpc_unparse_args(StringBuffer *w, arguments_ty args)
{
    BOOL first;
    Py_ssize_t i, di, arg_count, posonlyarg_count, default_count;

    first = TRUE;

    /* positional-only and positional arguments with defaults */
    posonlyarg_count = asdl_seq_LEN(args->posonlyargs);
    arg_count = asdl_seq_LEN(args->args);
    default_count = asdl_seq_LEN(args->defaults);
    for (i = 0; i < posonlyarg_count + arg_count; i++) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        if (i < posonlyarg_count) {
            if (qpc_unparse_arg(w, (arg_ty)asdl_seq_GET(args->posonlyargs, i)))
                return -1;
        } else {
            if (qpc_unparse_arg(w, (arg_ty)asdl_seq_GET(args->args, i - posonlyarg_count)))
                return -1;
        }

        di = i - posonlyarg_count - arg_count + default_count;
        if (di >= 0) {
            QPC_U_CHAR('=');
            QPC_U_EXPR((expr_ty)asdl_seq_GET(args->defaults, di), QPC_PR_TEST);
        }
        if (posonlyarg_count && i + 1 == posonlyarg_count) {
            QPC_U_STR(", /");
        }
    }

    /* vararg, or bare '*' if no varargs but keyword-only arguments present */
    if (args->vararg || asdl_seq_LEN(args->kwonlyargs)) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        QPC_U_STR("*");
        if (args->vararg) {
            if (qpc_unparse_arg(w, args->vararg))
                return -1;
        }
    }

    /* keyword-only arguments */
    arg_count = asdl_seq_LEN(args->kwonlyargs);
    default_count = asdl_seq_LEN(args->kw_defaults);
    for (i = 0; i < arg_count; i++) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        if (qpc_unparse_arg(w, (arg_ty)asdl_seq_GET(args->kwonlyargs, i)))
            return -1;

        di = i - arg_count + default_count;
        if (di >= 0) {
            expr_ty default_ = (expr_ty)asdl_seq_GET(args->kw_defaults, di);
            if (default_) {
                QPC_U_CHAR('=');
                QPC_U_EXPR(default_, QPC_PR_TEST);
            }
        }
    }

    /* **kwargs */
    if (args->kwarg) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        QPC_U_STR("**");
        if (qpc_unparse_arg(w, args->kwarg))
            return -1;
    }

    return 0;
}

static int qpc_unparse_lambda(StringBuffer *w, expr_ty e, int level)
{
    Py_ssize_t n_positional;
    QPC_U_STR_IF(level > QPC_PR_TEST, "(");
    n_positional = (asdl_seq_LEN(e->v.Lambda.args->args) +
                    asdl_seq_LEN(e->v.Lambda.args->posonlyargs));
    QPC_U_STR(n_positional ? "lambda " : "lambda");
    if (qpc_unparse_args(w, e->v.Lambda.args))
        return -1;
    QPC_U_STR(": ");
    QPC_U_EXPR(e->v.Lambda.body, QPC_PR_TEST);
    QPC_U_STR_IF(level > QPC_PR_TEST, ")");
    return 0;
}

static int qpc_unparse_ifexp(StringBuffer *w, expr_ty e, int level)
{
    QPC_U_STR_IF(level > QPC_PR_TEST, "(");
    QPC_U_EXPR(e->v.IfExp.body, QPC_PR_TEST + 1);
    QPC_U_STR(" if ");
    QPC_U_EXPR(e->v.IfExp.test, QPC_PR_TEST + 1);
    QPC_U_STR(" else ");
    QPC_U_EXPR(e->v.IfExp.orelse, QPC_PR_TEST);
    QPC_U_STR_IF(level > QPC_PR_TEST, ")");
    return 0;
}

static int qpc_unparse_dict(StringBuffer *w, expr_ty e)
{
    Py_ssize_t i, value_count;
    expr_ty key_node;

    QPC_U_CHAR('{');
    value_count = asdl_seq_LEN(e->v.Dict.values);

    for (i = 0; i < value_count; i++) {
        QPC_U_STR_IF(i > 0, ", ");
        key_node = (expr_ty)asdl_seq_GET(e->v.Dict.keys, i);
        if (key_node != NULL) {
            QPC_U_EXPR(key_node, QPC_PR_TEST);
            QPC_U_STR(": ");
            QPC_U_EXPR((expr_ty)asdl_seq_GET(e->v.Dict.values, i), QPC_PR_TEST);
        } else {
            QPC_U_STR("**");
            QPC_U_EXPR((expr_ty)asdl_seq_GET(e->v.Dict.values, i), QPC_PR_EXPR);
        }
    }

    QPC_U_CHAR('}');
    return 0;
}

/* Shared by Set ('{'), List ('['). */
static int qpc_unparse_elts(StringBuffer *w, asdl_expr_seq *elts, int open, int close)
{
    Py_ssize_t i, elem_count;

    QPC_U_CHAR(open);
    elem_count = asdl_seq_LEN(elts);
    for (i = 0; i < elem_count; i++) {
        QPC_U_STR_IF(i > 0, ", ");
        QPC_U_EXPR((expr_ty)asdl_seq_GET(elts, i), QPC_PR_TEST);
    }
    QPC_U_CHAR(close);
    return 0;
}

static int qpc_unparse_tuple(StringBuffer *w, expr_ty e, int level)
{
    Py_ssize_t i, elem_count;

    elem_count = asdl_seq_LEN(e->v.Tuple.elts);

    if (elem_count == 0)
        return string_buffer_puts8(w, "()");

    QPC_U_STR_IF(level > QPC_PR_TUPLE, "(");

    for (i = 0; i < elem_count; i++) {
        QPC_U_STR_IF(i > 0, ", ");
        QPC_U_EXPR((expr_ty)asdl_seq_GET(e->v.Tuple.elts, i), QPC_PR_TEST);
    }

    QPC_U_STR_IF(elem_count == 1, ",");
    QPC_U_STR_IF(level > QPC_PR_TUPLE, ")");
    return 0;
}

static int qpc_unparse_comprehensions(StringBuffer *w, asdl_comprehension_seq *comprehensions)
{
    Py_ssize_t i, j, gen_count, if_count;
    gen_count = asdl_seq_LEN(comprehensions);

    for (i = 0; i < gen_count; i++) {
        comprehension_ty gen = (comprehension_ty)asdl_seq_GET(comprehensions, i);
        QPC_U_STR(gen->is_async ? " async for " : " for ");
        QPC_U_EXPR(gen->target, QPC_PR_TUPLE);
        QPC_U_STR(" in ");
        QPC_U_EXPR(gen->iter, QPC_PR_TEST + 1);

        if_count = asdl_seq_LEN(gen->ifs);
        for (j = 0; j < if_count; j++) {
            QPC_U_STR(" if ");
            QPC_U_EXPR((expr_ty)asdl_seq_GET(gen->ifs, j), QPC_PR_TEST + 1);
        }
    }
    return 0;
}

/* GeneratorExp / ListComp / SetComp */
static int qpc_unparse_comp(StringBuffer *w, expr_ty elt,
                            asdl_comprehension_seq *gens, int open, int close)
{
    QPC_U_CHAR(open);
    QPC_U_EXPR(elt, QPC_PR_TEST);
    if (qpc_unparse_comprehensions(w, gens))
        return -1;
    QPC_U_CHAR(close);
    return 0;
}

static int qpc_unparse_dictcomp(StringBuffer *w, expr_ty e)
{
    QPC_U_CHAR('{');
    if (e->v.DictComp.value) {
        QPC_U_EXPR(e->v.DictComp.key, QPC_PR_TEST);
        QPC_U_STR(": ");
        QPC_U_EXPR(e->v.DictComp.value, QPC_PR_TEST);
    } else {
        QPC_U_STR("**");
        QPC_U_EXPR(e->v.DictComp.key, QPC_PR_TEST);
    }
    if (qpc_unparse_comprehensions(w, e->v.DictComp.generators))
        return -1;
    QPC_U_CHAR('}');
    return 0;
}

static int qpc_unparse_compare(StringBuffer *w, expr_ty e, int level)
{
    const char *op;
    Py_ssize_t i, comparator_count;
    asdl_expr_seq *comparators;
    asdl_int_seq *ops;

    QPC_U_STR_IF(level > QPC_PR_CMP, "(");

    comparators = e->v.Compare.comparators;
    ops = e->v.Compare.ops;
    comparator_count = asdl_seq_LEN(comparators);

    QPC_U_EXPR(e->v.Compare.left, QPC_PR_CMP + 1);

    for (i = 0; i < comparator_count; i++) {
        switch ((cmpop_ty)asdl_seq_GET(ops, i)) {
        case Eq: op = " == "; break;
        case NotEq: op = " != "; break;
        case Lt: op = " < "; break;
        case LtE: op = " <= "; break;
        case Gt: op = " > "; break;
        case GtE: op = " >= "; break;
        case Is: op = " is "; break;
        case IsNot: op = " is not "; break;
        case In: op = " in "; break;
        case NotIn: op = " not in "; break;
        default:
            return qpc_unparse_system_error(w, "unexpected comparison kind");
        }

        QPC_U_STR(op);
        QPC_U_EXPR((expr_ty)asdl_seq_GET(comparators, i), QPC_PR_CMP + 1);
    }

    QPC_U_STR_IF(level > QPC_PR_CMP, ")");
    return 0;
}

static int qpc_unparse_keyword(StringBuffer *w, keyword_ty kw)
{
    if (kw->arg == JS_ATOM_NULL) {
        QPC_U_STR("**");
    } else {
        if (qpc_unparse_atom(w, kw->arg))
            return -1;
        QPC_U_CHAR('=');
    }
    QPC_U_EXPR(kw->value, QPC_PR_TEST);
    return 0;
}

static int qpc_unparse_call(StringBuffer *w, expr_ty e)
{
    BOOL first;
    Py_ssize_t i, arg_count, kw_count;
    expr_ty expr;

    QPC_U_EXPR(e->v.Call.func, QPC_PR_ATOM);

    arg_count = asdl_seq_LEN(e->v.Call.args);
    kw_count = asdl_seq_LEN(e->v.Call.keywords);
    if (arg_count == 1 && kw_count == 0) {
        expr = (expr_ty)asdl_seq_GET(e->v.Call.args, 0);
        if (expr->kind == GeneratorExp_kind) {
            /* Special case: a single generator expression. */
            return qpc_unparse_comp(w, expr->v.GeneratorExp.elt,
                                    expr->v.GeneratorExp.generators, '(', ')');
        }
    }

    QPC_U_CHAR('(');

    first = TRUE;
    for (i = 0; i < arg_count; i++) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        QPC_U_EXPR((expr_ty)asdl_seq_GET(e->v.Call.args, i), QPC_PR_TEST);
    }

    for (i = 0; i < kw_count; i++) {
        QPC_U_STR_IF_NOT_FIRST(", ");
        if (qpc_unparse_keyword(w, (keyword_ty)asdl_seq_GET(e->v.Call.keywords, i)))
            return -1;
    }

    QPC_U_CHAR(')');
    return 0;
}

/* append_fstring_unicode: the str with '{' -> '{{' and '}' -> '}}'. */
static int qpc_unparse_fstring_unicode(StringBuffer *w, qp_const *c)
{
    JSString *p;
    uint32_t i;

    if (c->kind != QP_CONST_STR)
        return qpc_unparse_system_error(w, "unknown expression kind inside f-string or t-string");
    p = JS_VALUE_GET_STRING(c->v);
    for (i = 0; i < p->len; i++) {
        int ch = string_get(p, i);
        if (ch == '{' || ch == '}')
            QPC_U_CHAR(ch);
        QPC_U_CHAR(ch);
    }
    return 0;
}

static int qpc_unparse_fstring_element(StringBuffer *w, expr_ty e, BOOL is_format_spec)
{
    switch (e->kind) {
    case Constant_kind:
        return qpc_unparse_fstring_unicode(w, e->v.Constant.value);
    case JoinedStr_kind:
        return qpc_unparse_joinedstr(w, e, is_format_spec);
    case TemplateStr_kind:
        return qpc_unparse_templatestr(w, e);
    case FormattedValue_kind:
        return qpc_unparse_formattedvalue(w, e);
    case Interpolation_kind:
        return qpc_unparse_interpolation(w, e);
    default:
        return qpc_unparse_system_error(w, "unknown expression kind inside f-string or t-string");
    }
}

/* Build body separately to enable wrapping the entire stream of Strs,
   Constants and FormattedValues in one opening and one closing quote. */
static JSValue qpc_unparse_build_ftstring_body(JSContext *ctx, asdl_expr_seq *values,
                                               BOOL is_format_spec)
{
    StringBuffer body;
    Py_ssize_t i, value_count;

    if (string_buffer_init(ctx, &body, 64))
        return JS_EXCEPTION;
    value_count = asdl_seq_LEN(values);
    for (i = 0; i < value_count; ++i) {
        if (qpc_unparse_fstring_element(&body, (expr_ty)asdl_seq_GET(values, i),
                                        is_format_spec)) {
            string_buffer_free(&body);
            return JS_EXCEPTION;
        }
    }
    return string_buffer_end(&body);
}

/* prefix + repr(body) */
static int qpc_unparse_prefixed_repr(StringBuffer *w, const char *prefix, JSValue body)
{
    JSValue r;
    if (JS_IsException(body))
        return string_buffer_set_error(w);
    r = qp_str_repr(w->ctx, body);
    JS_FreeValue(w->ctx, body);
    if (JS_IsException(r))
        return string_buffer_set_error(w);
    if (string_buffer_puts8(w, prefix)) {
        JS_FreeValue(w->ctx, r);
        return -1;
    }
    return string_buffer_concat_value_free(w, r);
}

static int qpc_unparse_templatestr(StringBuffer *w, expr_ty e)
{
    JSValue body = qpc_unparse_build_ftstring_body(w->ctx, e->v.TemplateStr.values, FALSE);
    return qpc_unparse_prefixed_repr(w, "t", body);
}

static int qpc_unparse_joinedstr(StringBuffer *w, expr_ty e, BOOL is_format_spec)
{
    JSValue body = qpc_unparse_build_ftstring_body(w->ctx, e->v.JoinedStr.values,
                                                   is_format_spec);
    if (!is_format_spec)
        return qpc_unparse_prefixed_repr(w, "f", body);
    if (JS_IsException(body))
        return string_buffer_set_error(w);
    return string_buffer_concat_value_free(w, body);
}

/* append_interpolation_str: '{' (or '{ ' if str starts with '{') + str.
   Takes ownership of str. */
static int qpc_unparse_interpolation_str(StringBuffer *w, JSValue str)
{
    const char *outer_brace = "{";
    JSString *p = JS_VALUE_GET_STRING(str);
    if (p->len > 0 && string_get(p, 0) == '{') {
        /* Expression starts with a brace, split it with a space from the outer
           one. */
        outer_brace = "{ ";
    }
    if (string_buffer_puts8(w, outer_brace)) {
        JS_FreeValue(w->ctx, str);
        return -1;
    }
    return string_buffer_concat_value_free(w, str);
}

static int qpc_unparse_interpolation_value(StringBuffer *w, expr_ty e)
{
    /* Grammar allows PR_TUPLE, but use >PR_TEST for adding parenthesis
       around a lambda with ':' */
    JSValue s = qpc_unparse_expr_as_unicode(w->ctx, e, QPC_PR_TEST + 1);
    if (JS_IsException(s))
        return string_buffer_set_error(w);
    return qpc_unparse_interpolation_str(w, s);
}

static int qpc_unparse_interpolation_conversion(StringBuffer *w, int conversion)
{
    if (conversion < 0)
        return 0;
    switch (conversion) {
    case 'a': QPC_U_STR("!a"); break;
    case 'r': QPC_U_STR("!r"); break;
    case 's': QPC_U_STR("!s"); break;
    default:
        return qpc_unparse_system_error(w, "unknown f-value conversion kind");
    }
    return 0;
}

static int qpc_unparse_interpolation_format_spec(StringBuffer *w, expr_ty e)
{
    if (e) {
        QPC_U_CHAR(':');
        if (qpc_unparse_fstring_element(w, e, TRUE))
            return -1;
    }
    return 0;
}

static int qpc_unparse_interpolation(StringBuffer *w, expr_ty e)
{
    qp_const *c = e->v.Interpolation.str;
    if (c->kind != QP_CONST_STR)
        return qpc_unparse_system_error(w, "unknown expression kind inside f-string or t-string");
    if (qpc_unparse_interpolation_str(w, JS_DupValue(w->ctx, c->v)) ||
        qpc_unparse_interpolation_conversion(w, e->v.Interpolation.conversion) ||
        qpc_unparse_interpolation_format_spec(w, e->v.Interpolation.format_spec))
        return -1;
    QPC_U_STR("}");
    return 0;
}

static int qpc_unparse_formattedvalue(StringBuffer *w, expr_ty e)
{
    if (qpc_unparse_interpolation_value(w, e->v.FormattedValue.value) ||
        qpc_unparse_interpolation_conversion(w, e->v.FormattedValue.conversion) ||
        qpc_unparse_interpolation_format_spec(w, e->v.FormattedValue.format_spec))
        return -1;
    QPC_U_CHAR('}');
    return 0;
}

static int qpc_unparse_attribute(StringBuffer *w, expr_ty e)
{
    expr_ty v = e->v.Attribute.value;
    QPC_U_EXPR(v, QPC_PR_ATOM);

    /* Special case: integers require a space for attribute access to be
       unambiguous. */
    if (v->kind == Constant_kind && v->v.Constant.value->kind == QP_CONST_INT)
        QPC_U_STR(" .");
    else
        QPC_U_CHAR('.');

    return qpc_unparse_atom(w, e->v.Attribute.attr);
}

static int qpc_unparse_slice(StringBuffer *w, expr_ty e)
{
    if (e->v.Slice.lower)
        QPC_U_EXPR(e->v.Slice.lower, QPC_PR_TEST);
    QPC_U_CHAR(':');
    if (e->v.Slice.upper)
        QPC_U_EXPR(e->v.Slice.upper, QPC_PR_TEST);
    if (e->v.Slice.step) {
        QPC_U_CHAR(':');
        QPC_U_EXPR(e->v.Slice.step, QPC_PR_TEST);
    }
    return 0;
}

static int qpc_unparse_named_expr(StringBuffer *w, expr_ty e, int level)
{
    QPC_U_STR_IF(level > QPC_PR_TUPLE, "(");
    QPC_U_EXPR(e->v.NamedExpr.target, QPC_PR_ATOM);
    QPC_U_STR(" := ");
    QPC_U_EXPR(e->v.NamedExpr.value, QPC_PR_ATOM);
    QPC_U_STR_IF(level > QPC_PR_TUPLE, ")");
    return 0;
}

static int qpc_unparse_expr(StringBuffer *w, expr_ty e, int level)
{
    switch (e->kind) {
    case BoolOp_kind:
        return qpc_unparse_boolop(w, e, level);
    case BinOp_kind:
        return qpc_unparse_binop(w, e, level);
    case UnaryOp_kind:
        return qpc_unparse_unaryop(w, e, level);
    case Lambda_kind:
        return qpc_unparse_lambda(w, e, level);
    case IfExp_kind:
        return qpc_unparse_ifexp(w, e, level);
    case Dict_kind:
        return qpc_unparse_dict(w, e);
    case Set_kind:
        return qpc_unparse_elts(w, e->v.Set.elts, '{', '}');
    case GeneratorExp_kind:
        return qpc_unparse_comp(w, e->v.GeneratorExp.elt, e->v.GeneratorExp.generators, '(', ')');
    case ListComp_kind:
        return qpc_unparse_comp(w, e->v.ListComp.elt, e->v.ListComp.generators, '[', ']');
    case SetComp_kind:
        return qpc_unparse_comp(w, e->v.SetComp.elt, e->v.SetComp.generators, '{', '}');
    case DictComp_kind:
        return qpc_unparse_dictcomp(w, e);
    case Yield_kind:
        if (!e->v.Yield.value)
            return string_buffer_puts8(w, "(yield)");
        QPC_U_STR("(yield ");
        QPC_U_EXPR(e->v.Yield.value, QPC_PR_TEST);
        QPC_U_CHAR(')');
        return 0;
    case YieldFrom_kind:
        QPC_U_STR("(yield from ");
        QPC_U_EXPR(e->v.YieldFrom.value, QPC_PR_TEST);
        QPC_U_CHAR(')');
        return 0;
    case Await_kind:
        QPC_U_STR_IF(level > QPC_PR_AWAIT, "(");
        QPC_U_STR("await ");
        QPC_U_EXPR(e->v.Await.value, QPC_PR_ATOM);
        QPC_U_STR_IF(level > QPC_PR_AWAIT, ")");
        return 0;
    case Compare_kind:
        return qpc_unparse_compare(w, e, level);
    case Call_kind:
        return qpc_unparse_call(w, e);
    case Constant_kind:
        if (e->v.Constant.value->kind == QP_CONST_ELLIPSIS)
            return string_buffer_puts8(w, "...");
        if (e->v.Constant.kind != JS_ATOM_NULL && qpc_unparse_atom(w, e->v.Constant.kind))
            return -1;
        return qpc_unparse_constant(w, e->v.Constant.value);
    case JoinedStr_kind:
        return qpc_unparse_joinedstr(w, e, FALSE);
    case TemplateStr_kind:
        return qpc_unparse_templatestr(w, e);
    case FormattedValue_kind:
        return qpc_unparse_formattedvalue(w, e);
    case Interpolation_kind:
        return qpc_unparse_interpolation(w, e);
    /* The following exprs can be assignment targets. */
    case Attribute_kind:
        return qpc_unparse_attribute(w, e);
    case Subscript_kind:
        QPC_U_EXPR(e->v.Subscript.value, QPC_PR_ATOM);
        QPC_U_CHAR('[');
        QPC_U_EXPR(e->v.Subscript.slice, QPC_PR_TUPLE);
        QPC_U_CHAR(']');
        return 0;
    case Starred_kind:
        QPC_U_CHAR('*');
        QPC_U_EXPR(e->v.Starred.value, QPC_PR_EXPR);
        return 0;
    case Slice_kind:
        return qpc_unparse_slice(w, e);
    case Name_kind:
        return qpc_unparse_atom(w, e->v.Name.id);
    case List_kind:
        return qpc_unparse_elts(w, e->v.List.elts, '[', ']');
    case Tuple_kind:
        return qpc_unparse_tuple(w, e, level);
    case NamedExpr_kind:
        return qpc_unparse_named_expr(w, e, level);
    default:
        break;
    }
    return qpc_unparse_system_error(w, "unknown expression kind");
}

static JSValue qpc_unparse_expr_as_unicode(JSContext *ctx, expr_ty e, int level)
{
    StringBuffer w;
    if (string_buffer_init(ctx, &w, 64))
        return JS_EXCEPTION;
    if (qpc_unparse_expr(&w, e, level)) {
        string_buffer_free(&w);
        return JS_EXCEPTION;
    }
    return string_buffer_end(&w);
}

/* _PyAST_ExprAsUnicode(): new str, or JS_EXCEPTION. */
static JSValue qpc_expr_as_unicode(JSContext *ctx, expr_ty e)
{
    return qpc_unparse_expr_as_unicode(ctx, e, QPC_PR_TEST);
}

#undef QPC_U_CHAR
#undef QPC_U_STR
#undef QPC_U_STR_IF
#undef QPC_U_STR_IF_NOT_FIRST
#undef QPC_U_EXPR
/* ---------------------------------------------------------------------- */
/*
 * Expressions.
 *
 * Port of CPython 3.15.2 Python/codegen.c: comparisons and conditional
 * jumps, unary/binary operators, yield, bool ops, displays and unpacking,
 * calls (incl. the super()/method/builtin-genexp optimisations), f-strings
 * and t-strings, attribute/subscript/slice access and codegen_visit_expr.
 * Derived from CPython (PSF-2.0).
 */

/* ---- comparisons ------------------------------------------------------ */

/* (codegen_check_compare: SyntaxWarning only, emitted by qp_validate) */

/* COMPARE_OP / IS_OP / CONTAINS_OP.  `to_bool` = CPython's oparg bit 4
   (set by the flowgraph when a TO_BOOL follows): only meaningful for rich
   comparisons, is_op and contains always push an exact bool. */
static int
codegen_addcompare_ex(compiler *c, location loc, cmpop_ty op, int to_bool)
{
    int cmp;
    switch (op) {
    case Eq:
        cmp = QP_CMP_EQ;
        break;
    case NotEq:
        cmp = QP_CMP_NE;
        break;
    case Lt:
        cmp = QP_CMP_LT;
        break;
    case LtE:
        cmp = QP_CMP_LE;
        break;
    case Gt:
        cmp = QP_CMP_GT;
        break;
    case GtE:
        cmp = QP_CMP_GE;
        break;
    case Is:
        return qpc_py_u8(c, loc, QP_OP_is_op, 0);
    case IsNot:
        return qpc_py_u8(c, loc, QP_OP_is_op, 1);
    case In:
        return qpc_py_u8(c, loc, QP_OP_contains, 0);
    case NotIn:
        return qpc_py_u8(c, loc, QP_OP_contains, 1);
    default:
        return qpc_error(c, loc, "unknown comparison operator %d", (int)op);
    }
    return qpc_py_u8(c, loc, QP_OP_compare, cmp | (to_bool ? QP_CMP_TO_BOOL : 0));
}

static int
codegen_addcompare(compiler *c, location loc, cmpop_ty op)
{
    return codegen_addcompare_ex(c, loc, op, 0);
}

/* COMPARE_OP + TO_BOOL + POP_JUMP_IF_FALSE/TRUE: the result is an exact
   bool, so the plain QuickJS if_false/if_true are used. */
static int
codegen_compare_jump(compiler *c, location loc, cmpop_ty op, int jump_if_true,
                     jump_target_label target)
{
    RETURN_IF_ERROR(codegen_addcompare_ex(c, loc, op, 1));
    return qpc_jump(c, loc, jump_if_true ? OP_if_true : OP_if_false, target);
}

static int
codegen_jump_if(compiler *c, location loc,
                expr_ty e, jump_target_label next, int cond)
{
    switch (e->kind) {
    case UnaryOp_kind:
        if (e->v.UnaryOp.op == Not) {
            return codegen_jump_if(c, loc, e->v.UnaryOp.operand, next, !cond);
        }
        /* fallback to general implementation */
        break;
    case BoolOp_kind: {
        asdl_expr_seq *s = e->v.BoolOp.values;
        Py_ssize_t i, n = asdl_seq_LEN(s) - 1;
        assert(n >= 0);
        int cond2 = e->v.BoolOp.op == Or;
        jump_target_label next2 = next;
        if (!cond2 != !cond) {
            NEW_JUMP_TARGET_LABEL(c, new_next2);
            next2 = new_next2;
        }
        for (i = 0; i < n; ++i) {
            RETURN_IF_ERROR(
                codegen_jump_if(c, loc, (expr_ty)asdl_seq_GET(s, i), next2, cond2));
        }
        RETURN_IF_ERROR(
            codegen_jump_if(c, loc, (expr_ty)asdl_seq_GET(s, n), next, cond));
        if (!SAME_LABEL(next2, next)) {
            USE_LABEL(c, next2);
        }
        return SUCCESS;
    }
    case IfExp_kind: {
        NEW_JUMP_TARGET_LABEL(c, end);
        NEW_JUMP_TARGET_LABEL(c, next2);
        RETURN_IF_ERROR(
            codegen_jump_if(c, loc, e->v.IfExp.test, next2, 0));
        RETURN_IF_ERROR(
            codegen_jump_if(c, loc, e->v.IfExp.body, next, cond));
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

        USE_LABEL(c, next2);
        RETURN_IF_ERROR(
            codegen_jump_if(c, loc, e->v.IfExp.orelse, next, cond));

        USE_LABEL(c, end);
        return SUCCESS;
    }
    case Compare_kind: {
        Py_ssize_t n = asdl_seq_LEN(e->v.Compare.ops) - 1;
        if (n > 0) {
            NEW_JUMP_TARGET_LABEL(c, cleanup);
            VISIT(c, expr, e->v.Compare.left);
            for (Py_ssize_t i = 0; i < n; i++) {
                VISIT(c, expr,
                    (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
                RETURN_IF_ERROR(qpc_swap(c, LOC(e), 2));
                RETURN_IF_ERROR(qpc_copy(c, LOC(e), 2));
                RETURN_IF_ERROR(codegen_compare_jump(
                    c, LOC(e), (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, i), 0, cleanup));
            }
            VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
            RETURN_IF_ERROR(codegen_compare_jump(
                c, LOC(e), (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, n), cond, next));
            NEW_JUMP_TARGET_LABEL(c, end);
            RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

            USE_LABEL(c, cleanup);
            RETURN_IF_ERROR(qpc_op(c, LOC(e), OP_drop));
            if (!cond) {
                RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, next));
            }

            USE_LABEL(c, end);
            return SUCCESS;
        }
        /* fallback to general implementation */
        break;
    }
    default:
        /* fallback to general implementation */
        break;
    }

    /* general implementation */
    VISIT(c, expr, e);
    return qpc_py_jump(c, LOC(e), cond ? QP_OP_jump_if_true : QP_OP_jump_if_false, next);
}

static int
codegen_ifexp(compiler *c, expr_ty e)
{
    assert(e->kind == IfExp_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, next);

    RETURN_IF_ERROR(
        codegen_jump_if(c, LOC(e), e->v.IfExp.test, next, 0));

    VISIT(c, expr, e->v.IfExp.body);
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

    USE_LABEL(c, next);
    VISIT(c, expr, e->v.IfExp.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

/* ---- operators -------------------------------------------------------- */

/* QP_UNOP_* of UNARY_INVERT / UNARY_NEGATIVE, -1 if impossible */
static int
unaryop(unaryop_ty op)
{
    switch (op) {
    case Invert:
        return QP_UNOP_INVERT;
    case USub:
        return QP_UNOP_NEG;
    default:
        return -1;
    }
}

static int
addop_binary(compiler *c, location loc, operator_ty binop,
             bool inplace)
{
    int oparg;
    switch (binop) {
        case Add:
            oparg = inplace ? QP_NB_INPLACE_ADD : QP_NB_ADD;
            break;
        case Sub:
            oparg = inplace ? QP_NB_INPLACE_SUBTRACT : QP_NB_SUBTRACT;
            break;
        case Mult:
            oparg = inplace ? QP_NB_INPLACE_MULTIPLY : QP_NB_MULTIPLY;
            break;
        case MatMult:
            oparg = inplace ? QP_NB_INPLACE_MATRIX_MULTIPLY : QP_NB_MATRIX_MULTIPLY;
            break;
        case Div:
            oparg = inplace ? QP_NB_INPLACE_TRUE_DIVIDE : QP_NB_TRUE_DIVIDE;
            break;
        case Mod:
            oparg = inplace ? QP_NB_INPLACE_REMAINDER : QP_NB_REMAINDER;
            break;
        case Pow:
            oparg = inplace ? QP_NB_INPLACE_POWER : QP_NB_POWER;
            break;
        case LShift:
            oparg = inplace ? QP_NB_INPLACE_LSHIFT : QP_NB_LSHIFT;
            break;
        case RShift:
            oparg = inplace ? QP_NB_INPLACE_RSHIFT : QP_NB_RSHIFT;
            break;
        case BitOr:
            oparg = inplace ? QP_NB_INPLACE_OR : QP_NB_OR;
            break;
        case BitXor:
            oparg = inplace ? QP_NB_INPLACE_XOR : QP_NB_XOR;
            break;
        case BitAnd:
            oparg = inplace ? QP_NB_INPLACE_AND : QP_NB_AND;
            break;
        case FloorDiv:
            oparg = inplace ? QP_NB_INPLACE_FLOOR_DIVIDE : QP_NB_FLOOR_DIVIDE;
            break;
        default:
            return qpc_error(c, loc, "%s op %d should not be possible",
                             inplace ? "inplace" : "binary", (int)binop);
    }
    return qpc_py_u8(c, loc, QP_OP_binop, oparg);
}

static int
codegen_addop_yield(compiler *c, location loc) {
    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    if (ste->ste_generator && ste->ste_coroutine) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_ASYNC_GEN_WRAP));
    }
    /* YIELD_VALUE + RESUME */
    return qpc_py_u8(c, loc, QP_OP_yield_value, QP_YIELD_PLAIN);
}

/* ---- bool ops, displays, unpacking ------------------------------------ */

static int
codegen_boolop(compiler *c, expr_ty e)
{
    int jumpi;
    Py_ssize_t i, n;
    asdl_expr_seq *s;

    location loc = LOC(e);
    assert(e->kind == BoolOp_kind);
    if (e->v.BoolOp.op == And)
        jumpi = QP_OP_jump_if_false;
    else
        jumpi = QP_OP_jump_if_true;
    NEW_JUMP_TARGET_LABEL(c, end);
    s = e->v.BoolOp.values;
    n = asdl_seq_LEN(s) - 1;
    assert(n >= 0);
    for (i = 0; i < n; ++i) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(s, i));
        /* JUMP_IF_FALSE/TRUE pseudo op = COPY 1; TO_BOOL; POP_JUMP_IF_* */
        RETURN_IF_ERROR(qpc_copy(c, loc, 1));
        RETURN_IF_ERROR(qpc_py_jump(c, loc, jumpi, end));
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    }
    VISIT(c, expr, (expr_ty)asdl_seq_GET(s, n));

    USE_LABEL(c, end);
    return SUCCESS;
}

/* `build` argument of starunpack_helper_impl for lists (CPython
   BUILD_LIST): lists are JS arrays built with OP_array_from.  Any other
   value is the OP_py subop (build_set). */
#define QPC_BUILD_LIST (-1)

static int
codegen_build_seq(compiler *c, location loc, int build, int n)
{
    if (build == QPC_BUILD_LIST) {
        return qpc_op_u16(c, loc, OP_array_from, n);
    }
    return qpc_py_u16(c, loc, build, n);
}

static int
starunpack_helper_impl(compiler *c, location loc,
                       asdl_expr_seq *elts, JSAtom injected_arg, int pushed,
                       int build, int add, int extend, int tuple)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    int big = n + pushed + (injected_arg != JS_ATOM_NULL ? 1 : 0) > QPC_STACK_USE_GUIDELINE;
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            seen_star = 1;
            break;
        }
    }
    if (!seen_star && !big) {
        for (Py_ssize_t i = 0; i < n; i++) {
            expr_ty elt = asdl_seq_GET(elts, i);
            VISIT(c, expr, elt);
        }
        if (injected_arg != JS_ATOM_NULL) {
            RETURN_IF_ERROR(codegen_nameop(c, loc, injected_arg, Load));
            n++;
        }
        if (tuple) {
            return qpc_py_u16(c, loc, QP_OP_build_tuple, n + pushed);
        }
        return codegen_build_seq(c, loc, build, n + pushed);
    }
    int sequence_built = 0;
    if (big) {
        RETURN_IF_ERROR(codegen_build_seq(c, loc, build, pushed));
        sequence_built = 1;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            if (sequence_built == 0) {
                RETURN_IF_ERROR(codegen_build_seq(c, loc, build, i + pushed));
                sequence_built = 1;
            }
            VISIT(c, expr, elt->v.Starred.value);
            RETURN_IF_ERROR(qpc_py_u16(c, loc, extend, 1));
        }
        else {
            VISIT(c, expr, elt);
            if (sequence_built) {
                RETURN_IF_ERROR(qpc_py_u16(c, loc, add, 1));
            }
        }
    }
    assert(sequence_built);
    if (injected_arg != JS_ATOM_NULL) {
        RETURN_IF_ERROR(codegen_nameop(c, loc, injected_arg, Load));
        RETURN_IF_ERROR(qpc_py_u16(c, loc, add, 1));
    }
    if (tuple) {
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_LIST_TO_TUPLE));
    }
    return SUCCESS;
}

static int
starunpack_helper(compiler *c, location loc,
                  asdl_expr_seq *elts, int pushed,
                  int build, int add, int extend, int tuple)
{
    return starunpack_helper_impl(c, loc, elts, JS_ATOM_NULL, pushed,
                                  build, add, extend, tuple);
}

static int
unpack_helper(compiler *c, location loc, asdl_expr_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind && !seen_star) {
            /* CPython limits `after` to INT_MAX >> 8; the unpack_ex
               operand is u16 (before | after << 8), so after < 256. */
            if ((i >= (1 << 8)) ||
                (n-i-1 > QPC_MAX_ITEMS_IN_LIST)) {
                return qpc_error(c, loc,
                    "too many expressions in "
                    "star-unpacking assignment");
            }
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_unpack_ex,
                                       (int)(i + ((n-i-1) << 8))));
            seen_star = 1;
        }
        else if (elt->kind == Starred_kind) {
            return qpc_error(c, loc,
                "multiple starred expressions in assignment");
        }
    }
    if (!seen_star) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_unpack_seq, (int)n));
    }
    return SUCCESS;
}

static int
assignment_helper(compiler *c, location loc, asdl_expr_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    RETURN_IF_ERROR(unpack_helper(c, loc, elts));
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        VISIT(c, expr, elt->kind != Starred_kind ? elt : elt->v.Starred.value);
    }
    return SUCCESS;
}

static int
codegen_list(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    asdl_expr_seq *elts = e->v.List.elts;
    if (e->v.List.ctx == Store) {
        return assignment_helper(c, loc, elts);
    }
    else if (e->v.List.ctx == Load) {
        return starunpack_helper(c, loc, elts, 0, QPC_BUILD_LIST,
                                 QP_OP_list_append, QP_OP_list_extend, 0);
    }
    else {
        VISIT_SEQ(c, expr, elts);
    }
    return SUCCESS;
}

static int
codegen_tuple(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    asdl_expr_seq *elts = e->v.Tuple.elts;
    if (e->v.Tuple.ctx == Store) {
        return assignment_helper(c, loc, elts);
    }
    else if (e->v.Tuple.ctx == Load) {
        return starunpack_helper(c, loc, elts, 0, QPC_BUILD_LIST,
                                 QP_OP_list_append, QP_OP_list_extend, 1);
    }
    else {
        VISIT_SEQ(c, expr, elts);
    }
    return SUCCESS;
}

static int
codegen_set(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    return starunpack_helper(c, loc, e->v.Set.elts, 0, QP_OP_build_set,
                             QP_OP_set_add, QP_OP_set_update, 0);
}

static int
codegen_subdict(compiler *c, expr_ty e, Py_ssize_t begin, Py_ssize_t end)
{
    Py_ssize_t i, n = end - begin;
    int big = n*2 > QPC_STACK_USE_GUIDELINE;
    location loc = LOC(e);
    if (big) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0));
    }
    for (i = begin; i < end; i++) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.keys, i));
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
        if (big) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_map_add, 1));
        }
    }
    if (!big) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, (int)n));
    }
    return SUCCESS;
}

static int
codegen_dict(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t i, n, elements;
    int have_dict;
    int is_unpacking = 0;
    n = asdl_seq_LEN(e->v.Dict.values);
    have_dict = 0;
    elements = 0;
    for (i = 0; i < n; i++) {
        is_unpacking = (expr_ty)asdl_seq_GET(e->v.Dict.keys, i) == NULL;
        if (is_unpacking) {
            if (elements) {
                RETURN_IF_ERROR(codegen_subdict(c, e, i - elements, i));
                if (have_dict) {
                    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_update, 1));
                }
                have_dict = 1;
                elements = 0;
            }
            if (have_dict == 0) {
                RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0));
                have_dict = 1;
            }
            VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_update, 1));
        }
        else {
            if (elements*2 > QPC_STACK_USE_GUIDELINE) {
                RETURN_IF_ERROR(codegen_subdict(c, e, i - elements, i + 1));
                if (have_dict) {
                    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_update, 1));
                }
                have_dict = 1;
                elements = 0;
            }
            else {
                elements++;
            }
        }
    }
    if (elements) {
        RETURN_IF_ERROR(codegen_subdict(c, e, n - elements, n));
        if (have_dict) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_update, 1));
        }
        have_dict = 1;
    }
    if (!have_dict) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0));
    }
    return SUCCESS;
}

static int
codegen_compare(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t i, n;

    VISIT(c, expr, e->v.Compare.left);
    assert(asdl_seq_LEN(e->v.Compare.ops) > 0);
    n = asdl_seq_LEN(e->v.Compare.ops) - 1;
    if (n == 0) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, 0));
        ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, 0));
    }
    else {
        NEW_JUMP_TARGET_LABEL(c, cleanup);
        for (i = 0; i < n; i++) {
            VISIT(c, expr,
                (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
            RETURN_IF_ERROR(qpc_swap(c, loc, 2));
            RETURN_IF_ERROR(qpc_copy(c, loc, 2));
            ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, i));
            /* [.., x_i, res] -> COPY 1; TO_BOOL; POP_JUMP_IF_FALSE */
            RETURN_IF_ERROR(qpc_copy(c, loc, 1));
            RETURN_IF_ERROR(qpc_py_jump(c, loc, QP_OP_jump_if_false, cleanup));
            RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
        }
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
        ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, n));
        NEW_JUMP_TARGET_LABEL(c, end);
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

        USE_LABEL(c, cleanup);
        RETURN_IF_ERROR(qpc_swap(c, loc, 2));
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));

        USE_LABEL(c, end);
    }
    return SUCCESS;
}

/* infer_type, check_caller, check_subscripter, check_index: SyntaxWarnings
   only, emitted by qp_validate. */

/* ---- calls ------------------------------------------------------------ */

static int
is_import_originated(compiler *c, expr_ty e)
{
    /* Check whether the global scope has an import named
     e, if it is a Name object. For not traversing all the
     scope stack every time this function is called, it will
     only check the global scope to determine whether something
     is imported or not. */

    if (e->kind != Name_kind) {
        return 0;
    }

    long flags = _PyST_GetSymbol(SYMTABLE(c)->st_top, e->v.Name.id);
    return (flags & DEF_IMPORT) != 0;
}

static int
can_optimize_super_call(compiler *c, expr_ty attr)
{
    expr_ty e = attr->v.Attribute.value;
    if (e->kind != Call_kind ||
        e->v.Call.func->kind != Name_kind ||
        !qpc_atom_is(e->v.Call.func->v.Name.id, c->ids.super) ||
        qpc_atom_is(attr->v.Attribute.attr, c->ids.__class__) ||
        asdl_seq_LEN(e->v.Call.keywords) != 0) {
        return 0;
    }
    Py_ssize_t num_args = asdl_seq_LEN(e->v.Call.args);

    JSAtom super_name = e->v.Call.func->v.Name.id;
    // detect statically-visible shadowing of 'super' name
    int scope = _PyST_GetScope(SYMTABLE_ENTRY(c), super_name);
    RETURN_IF_ERROR(scope);
    if (scope != GLOBAL_IMPLICIT) {
        return 0;
    }
    scope = _PyST_GetScope(SYMTABLE(c)->st_top, super_name);
    RETURN_IF_ERROR(scope);
    if (scope != 0) {
        return 0;
    }

    if (num_args == 2) {
        for (Py_ssize_t i = 0; i < num_args; i++) {
            expr_ty elt = asdl_seq_GET(e->v.Call.args, i);
            if (elt->kind == Starred_kind) {
                return 0;
            }
        }
        // exactly two non-starred args; we can just load
        // the provided args
        return 1;
    }

    if (num_args != 0) {
        return 0;
    }
    // we need the following for zero-arg super():

    // enclosing function should have at least one argument
    if (c->u->u_argcount == 0 &&
        c->u->u_posonlyargcount == 0) {
        return 0;
    }
    // __class__ cell should be available
    if (_PyCompile_GetRefType(c, c->ids.__class__) == FREE) {
        return 1;
    }
    return 0;
}

static int
load_args_for_super(compiler *c, expr_ty e) {
    location loc = LOC(e);

    // load super() global
    JSAtom super_name = e->v.Call.func->v.Name.id;
    RETURN_IF_ERROR(codegen_nameop(c, LOC(e->v.Call.func), super_name, Load));

    if (asdl_seq_LEN(e->v.Call.args) == 2) {
        VISIT(c, expr, asdl_seq_GET(e->v.Call.args, 0));
        VISIT(c, expr, asdl_seq_GET(e->v.Call.args, 1));
        return SUCCESS;
    }

    // load __class__ cell
    JSAtom name = c->ids.__class__;
    assert(_PyCompile_GetRefType(c, name) == FREE);
    RETURN_IF_ERROR(codegen_nameop(c, loc, name, Load));

    // load self (first argument)
    qp_atomlist *varnames = &SYMTABLE_ENTRY(c)->ste_varnames;
    if (varnames->len == 0) {
        return qpc_error(c, loc, "super(): no arguments");
    }
    RETURN_IF_ERROR(codegen_nameop(c, loc, varnames->items[0], Load));

    return SUCCESS;
}

/* PyUnicode_GET_LENGTH of an attribute name (UTF-16 length of the atom) */
static int
codegen_atom_length(compiler *c, JSAtom atom)
{
    JSValue s = JS_AtomToString(c->ctx, atom);
    int len;
    if (JS_IsException(s)) {
        JS_FreeValue(c->ctx, JS_GetException(c->ctx));
        return 0;
    }
    len = JS_VALUE_GET_STRING(s)->len;
    JS_FreeValue(c->ctx, s);
    return len;
}

// If an attribute access spans multiple lines, update the current start
// location to point to the attribute name.
static location
update_start_location_to_match_attr(compiler *c, location loc,
                                    expr_ty attr)
{
    assert(attr->kind == Attribute_kind);
    if (loc.lineno != attr->end_lineno) {
        loc.lineno = attr->end_lineno;
        int len = codegen_atom_length(c, attr->v.Attribute.attr);
        if (len <= attr->end_col_offset) {
            loc.col_offset = attr->end_col_offset - len;
        }
        else {
            // GH-94694: Somebody's compiling weird ASTs. Just drop the columns:
            loc.col_offset = -1;
            loc.end_col_offset = -1;
        }
        // Make sure the end position still follows the start position, even for
        // weird ASTs:
        if (loc.end_lineno < loc.lineno) {
            loc.end_lineno = loc.lineno;
        }
        if (loc.lineno == loc.end_lineno && loc.end_col_offset < loc.col_offset) {
            loc.end_col_offset = loc.col_offset;
        }
    }
    return loc;
}

/* Stack: [func] on entry.  Optimized path (func is the real builtin):
   [list|set]? [gen, NULL] loop with for_iter (list_append/set_add 3 reach
   the container below [gen, idx]); the exhausted branch of for_iter lands
   with [gen, idx] (no END_FOR), POP_ITER = two drops; jumps to `end` with
   [result].  Not the builtin: falls through to skip_optimization with
   [func]. */
static int
maybe_optimize_function_call(compiler *c, expr_ty e, jump_target_label end)
{
    asdl_expr_seq *args = e->v.Call.args;
    asdl_keyword_seq *kwds = e->v.Call.keywords;
    expr_ty func = e->v.Call.func;

    if (! (func->kind == Name_kind &&
           asdl_seq_LEN(args) == 1 &&
           asdl_seq_LEN(kwds) == 0 &&
           asdl_seq_GET(args, 0)->kind == GeneratorExp_kind))
    {
        return 0;
    }

    expr_ty generator_exp = asdl_seq_GET(args, 0);
    PySTEntryObject *generator_entry = _PySymtable_Lookup(SYMTABLE(c), (void *)generator_exp);
    if (generator_entry == NULL) {
        return ERROR;
    }
    if (generator_entry->ste_coroutine) {
        return 0;
    }

    location loc = LOC(func);

    int optimized = 0;
    NEW_JUMP_TARGET_LABEL(c, skip_optimization);

    int const_oparg = -1;
    int initial_res = -1;   /* Py_True: 1, Py_False: 0 */
    int continue_jump_opcode = -1;
    JSAtom name = func->v.Name.id;
    if (qpc_atom_is(name, c->ids.all)) {
        const_oparg = QP_COMMON_CONST_BUILTIN_ALL;
        initial_res = 1;
        continue_jump_opcode = QP_OP_jump_if_true;
    }
    else if (qpc_atom_is(name, c->ids.any)) {
        const_oparg = QP_COMMON_CONST_BUILTIN_ANY;
        initial_res = 0;
        continue_jump_opcode = QP_OP_jump_if_false;
    }
    else if (qpc_atom_is(name, c->ids.tuple)) {
        const_oparg = QP_COMMON_CONST_BUILTIN_TUPLE;
    }
    else if (qpc_atom_is(name, c->ids.list)) {
        const_oparg = QP_COMMON_CONST_BUILTIN_LIST;
    }
    else if (qpc_atom_is(name, c->ids.set)) {
        const_oparg = QP_COMMON_CONST_BUILTIN_SET;
    }
    if (const_oparg != -1) {
        RETURN_IF_ERROR(qpc_copy(c, loc, 1)); // the function
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_common_const, const_oparg));
        ADDOP_COMPARE(c, loc, Is);
        RETURN_IF_ERROR(qpc_jump(c, loc, OP_if_false, skip_optimization));
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));

        if (const_oparg == QP_COMMON_CONST_BUILTIN_TUPLE || const_oparg == QP_COMMON_CONST_BUILTIN_LIST) {
            RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_array_from, 0));
        } else if (const_oparg == QP_COMMON_CONST_BUILTIN_SET) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_set, 0));
        }
        VISIT(c, expr, generator_exp);

        NEW_JUMP_TARGET_LABEL(c, loop);
        NEW_JUMP_TARGET_LABEL(c, cleanup);

        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null)); // Push NULL index for loop
        USE_LABEL(c, loop);
        RETURN_IF_ERROR(qpc_py_jump(c, loc, QP_OP_for_iter, cleanup));
        if (const_oparg == QP_COMMON_CONST_BUILTIN_TUPLE || const_oparg == QP_COMMON_CONST_BUILTIN_LIST) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_list_append, 3));
            RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, loop));
        } else if (const_oparg == QP_COMMON_CONST_BUILTIN_SET) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_set_add, 3));
            RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, loop));
        }
        else {
            RETURN_IF_ERROR(qpc_py_jump(c, loc, continue_jump_opcode, loop));
        }

        /* POP_ITER */
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        if (const_oparg != QP_COMMON_CONST_BUILTIN_TUPLE &&
            const_oparg != QP_COMMON_CONST_BUILTIN_LIST &&
            const_oparg != QP_COMMON_CONST_BUILTIN_SET) {
            RETURN_IF_ERROR(qpc_load_bool(c, loc, !initial_res));
        }
        RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, end));

        USE_LABEL(c, cleanup);
        /* END_FOR is not emitted; POP_ITER */
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
        if (const_oparg == QP_COMMON_CONST_BUILTIN_TUPLE) {
            RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_LIST_TO_TUPLE));
        } else if (const_oparg == QP_COMMON_CONST_BUILTIN_LIST) {
            // result is already a list
        } else if (const_oparg == QP_COMMON_CONST_BUILTIN_SET) {
            // result is already a set
        }
        else {
            RETURN_IF_ERROR(qpc_load_bool(c, loc, initial_res));
        }

        optimized = 1;
        RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, end));
    }
    USE_LABEL(c, skip_optimization);
    return optimized;
}

static int codegen_call_simple_kw_helper(compiler *c, location loc,
                                         asdl_keyword_seq *keywords,
                                         Py_ssize_t nkwelts);

// Return 1 if the method call was optimized, 0 if not, and -1 on error.
static int
maybe_optimize_method_call(compiler *c, expr_ty e)
{
    Py_ssize_t argsl, i, kwdsl;
    expr_ty meth = e->v.Call.func;
    asdl_expr_seq *args = e->v.Call.args;
    asdl_keyword_seq *kwds = e->v.Call.keywords;

    /* Check that the call node is an attribute access */
    if (meth->kind != Attribute_kind || meth->v.Attribute.ctx != Load) {
        return 0;
    }

    /* Check that the base object is not something that is imported */
    int ret = is_import_originated(c, meth->v.Attribute.value);
    RETURN_IF_ERROR(ret);
    if (ret) {
        return 0;
    }

    /* Check that there aren't too many arguments */
    argsl = asdl_seq_LEN(args);
    kwdsl = asdl_seq_LEN(kwds);
    if (argsl + kwdsl + (kwdsl != 0) >= QPC_STACK_USE_GUIDELINE) {
        return 0;
    }
    /* Check that there are no *varargs types of arguments. */
    for (i = 0; i < argsl; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        if (elt->kind == Starred_kind) {
            return 0;
        }
    }

    for (i = 0; i < kwdsl; i++) {
        keyword_ty kw = asdl_seq_GET(kwds, i);
        if (kw->arg == JS_ATOM_NULL) {
            return 0;
        }
    }

    /* Alright, we can optimize the code. */
    location loc = LOC(meth);

    ret = can_optimize_super_call(c, meth);
    RETURN_IF_ERROR(ret);
    if (ret) {
        RETURN_IF_ERROR(load_args_for_super(c, meth->v.Attribute.value));
        /* LOAD_SUPER_METHOD / LOAD_ZERO_SUPER_METHOD; the trailing NOP
           only carries a location: not emitted */
        int flags = asdl_seq_LEN(meth->v.Attribute.value->v.Call.args) ?
            QP_SUPER_TWO_ARGS : 0;
        RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_load_super_method,
                                       meth->v.Attribute.attr, flags));
    } else {
        VISIT(c, expr, meth->v.Attribute.value);
        loc = update_start_location_to_match_attr(c, loc, meth);
        RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_load_method,
                                       meth->v.Attribute.attr, 0));
    }

    VISIT_SEQ(c, expr, e->v.Call.args);

    if (kwdsl) {
        VISIT_SEQ(c, keyword, kwds);
        RETURN_IF_ERROR(
            codegen_call_simple_kw_helper(c, loc, kwds, kwdsl));
        loc = update_start_location_to_match_attr(c, LOC(e), meth);
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_kw, (int)(argsl + kwdsl)));
    }
    else {
        loc = update_start_location_to_match_attr(c, LOC(e), meth);
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, (int)argsl));
    }
    return 1;
}

static int
codegen_validate_keywords(compiler *c, asdl_keyword_seq *keywords)
{
    Py_ssize_t nkeywords = asdl_seq_LEN(keywords);
    for (Py_ssize_t i = 0; i < nkeywords; i++) {
        keyword_ty key = ((keyword_ty)asdl_seq_GET(keywords, i));
        if (key->arg == JS_ATOM_NULL) {
            continue;
        }
        for (Py_ssize_t j = i + 1; j < nkeywords; j++) {
            keyword_ty other = ((keyword_ty)asdl_seq_GET(keywords, j));
            if (other->arg != JS_ATOM_NULL && qpc_atom_is(key->arg, other->arg)) {
                char buf[QPC_ATOM_BUF];
                return qpc_error(c, LOC(other), "keyword argument repeated: %s",
                                 qpc_atom_str(c, buf, key->arg));
            }
        }
    }
    return SUCCESS;
}

static int
codegen_call(compiler *c, expr_ty e)
{
    RETURN_IF_ERROR(codegen_validate_keywords(c, e->v.Call.keywords));
    int ret = maybe_optimize_method_call(c, e);
    if (ret < 0) {
        return ERROR;
    }
    if (ret == 1) {
        return SUCCESS;
    }
    NEW_JUMP_TARGET_LABEL(c, skip_normal_call);
    /* check_caller: warning only, emitted by qp_validate */
    VISIT(c, expr, e->v.Call.func);
    RETURN_IF_ERROR(maybe_optimize_function_call(c, e, skip_normal_call));
    location loc = LOC(e->v.Call.func);
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    loc = LOC(e);
    ret = codegen_call_helper(c, loc, 0,
                              e->v.Call.args,
                              e->v.Call.keywords);
    USE_LABEL(c, skip_normal_call);
    return ret;
}

/* ---- f-strings and t-strings ------------------------------------------ */

static int
codegen_template_str(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    expr_ty value;

    Py_ssize_t value_count = asdl_seq_LEN(e->v.TemplateStr.values);
    int last_was_interpolation = 1;
    Py_ssize_t stringslen = 0;
    for (Py_ssize_t i = 0; i < value_count; i++) {
        value = asdl_seq_GET(e->v.TemplateStr.values, i);
        if (value->kind == Interpolation_kind) {
            if (last_was_interpolation) {
                RETURN_IF_ERROR(qpc_load_str_atom(c, loc, c->ids.empty));
                stringslen++;
            }
            last_was_interpolation = 1;
        }
        else {
            VISIT(c, expr, value);
            stringslen++;
            last_was_interpolation = 0;
        }
    }
    if (last_was_interpolation) {
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, c->ids.empty));
        stringslen++;
    }
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_tuple, (int)stringslen));

    Py_ssize_t interpolationslen = 0;
    for (Py_ssize_t i = 0; i < value_count; i++) {
        value = asdl_seq_GET(e->v.TemplateStr.values, i);
        if (value->kind == Interpolation_kind) {
            VISIT(c, expr, value);
            interpolationslen++;
        }
    }
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_tuple, (int)interpolationslen));
    return qpc_py(c, loc, QP_OP_build_template);
}

static int
codegen_joined_str(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t value_count = asdl_seq_LEN(e->v.JoinedStr.values);
    if (value_count > QPC_STACK_USE_GUIDELINE) {
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, c->ids.empty));
        RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_load_method, c->ids.join, 0));
        RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_array_from, 0));
        for (Py_ssize_t i = 0; i < asdl_seq_LEN(e->v.JoinedStr.values); i++) {
            VISIT(c, expr, asdl_seq_GET(e->v.JoinedStr.values, i));
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_list_append, 1));
        }
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 1));
    }
    else {
        VISIT_SEQ(c, expr, e->v.JoinedStr.values);
        if (value_count > 1) {
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_string, (int)value_count));
        }
        else if (value_count == 0) {
            RETURN_IF_ERROR(qpc_load_str_atom(c, loc, c->ids.empty));
        }
    }
    return SUCCESS;
}

/* FVC_* of a conversion character, -1 (after setting the error) if bad */
static int
codegen_fvc(compiler *c, location loc, int conversion)
{
    switch (conversion) {
    case 's': return QP_FVC_STR;
    case 'r': return QP_FVC_REPR;
    case 'a': return QP_FVC_ASCII;
    default:
        qpc_error(c, loc, "Unrecognized conversion character %d", conversion);
        return -1;
    }
}

static int
codegen_interpolation(compiler *c, expr_ty e)
{
    location loc = LOC(e);

    VISIT(c, expr, e->v.Interpolation.value);
    RETURN_IF_ERROR(qpc_load_const(c, loc, e->v.Interpolation.str));

    /* build_interpolation always takes a format_spec ('' when absent) and
       the conversion as its operand (CPython packs both in the oparg) */
    if (e->v.Interpolation.format_spec) {
        VISIT(c, expr, e->v.Interpolation.format_spec);
    }
    else {
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, c->ids.empty));
    }

    int conversion = e->v.Interpolation.conversion;
    int fvc = QP_FVC_NONE;
    if (conversion != -1) {
        fvc = codegen_fvc(c, loc, conversion);
        RETURN_IF_ERROR(fvc);
    }

    return qpc_py_u8(c, loc, QP_OP_build_interpolation, fvc);
}

/* Used to implement f-strings. Format a single value. */
static int
codegen_formatted_value(compiler *c, expr_ty e)
{
    int conversion = e->v.FormattedValue.conversion;
    int oparg;

    /* The expression to be formatted. */
    VISIT(c, expr, e->v.FormattedValue.value);

    location loc = LOC(e);
    if (conversion != -1) {
        oparg = codegen_fvc(c, loc, conversion);
        RETURN_IF_ERROR(oparg);
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_convert_value, oparg));
    }
    if (e->v.FormattedValue.format_spec) {
        /* Evaluate the format spec, and update our opcode arg. */
        VISIT(c, expr, e->v.FormattedValue.format_spec);
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_format_with_spec));
    } else {
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_format_simple));
    }
    return SUCCESS;
}

/* ---- call helpers ----------------------------------------------------- */

static int
codegen_subkwargs(compiler *c, location loc,
                  asdl_keyword_seq *keywords,
                  Py_ssize_t begin, Py_ssize_t end)
{
    Py_ssize_t i, n = end - begin;
    keyword_ty kw;
    assert(n > 0);
    int big = n*2 > QPC_STACK_USE_GUIDELINE;
    if (big) {
        RETURN_IF_ERROR(qpc_py_u16(c, NO_LOCATION, QP_OP_build_map, 0));
    }
    for (i = begin; i < end; i++) {
        kw = asdl_seq_GET(keywords, i);
        RETURN_IF_ERROR(qpc_load_str_atom(c, loc, kw->arg));
        VISIT(c, expr, kw->value);
        if (big) {
            RETURN_IF_ERROR(qpc_py_u16(c, NO_LOCATION, QP_OP_map_add, 1));
        }
    }
    if (!big) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, (int)n));
    }
    return SUCCESS;
}

/* Used by codegen_call_helper and maybe_optimize_method_call to emit
 * a tuple of keyword names before CALL.
 * Both callers keep nkwelts below QPC_STACK_USE_GUIDELINE.
 */
static int
codegen_call_simple_kw_helper(compiler *c, location loc,
                              asdl_keyword_seq *keywords, Py_ssize_t nkwelts)
{
    JSAtom names[QPC_STACK_USE_GUIDELINE];
    assert(nkwelts <= QPC_STACK_USE_GUIDELINE);
    for (Py_ssize_t i = 0; i < nkwelts; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        names[i] = kw->arg;
    }
    return qpc_load_kwnames(c, loc, names, (int)nkwelts);
}

/* shared code between codegen_call and codegen_class */
static int
codegen_call_helper_impl(compiler *c, location loc,
                         int n, /* Args already pushed */
                         asdl_expr_seq *args,
                         JSAtom injected_arg,
                         asdl_keyword_seq *keywords)
{
    Py_ssize_t i, nseen, nelts, nkwelts;

    RETURN_IF_ERROR(codegen_validate_keywords(c, keywords));

    nelts = asdl_seq_LEN(args);
    nkwelts = asdl_seq_LEN(keywords);

    if (nelts + nkwelts*2 > QPC_STACK_USE_GUIDELINE) {
         goto ex_call;
    }
    for (i = 0; i < nelts; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        if (elt->kind == Starred_kind) {
            goto ex_call;
        }
    }
    for (i = 0; i < nkwelts; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        if (kw->arg == JS_ATOM_NULL) {
            goto ex_call;
        }
    }

    /* No * or ** args, so can use faster calling sequence */
    for (i = 0; i < nelts; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        assert(elt->kind != Starred_kind);
        VISIT(c, expr, elt);
    }
    if (injected_arg != JS_ATOM_NULL) {
        RETURN_IF_ERROR(codegen_nameop(c, loc, injected_arg, Load));
        nelts++;
    }
    if (nkwelts) {
        VISIT_SEQ(c, keyword, keywords);
        RETURN_IF_ERROR(
            codegen_call_simple_kw_helper(c, loc, keywords, nkwelts));
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_kw, (int)(n + nelts + nkwelts)));
    }
    else {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, (int)(n + nelts)));
    }
    return SUCCESS;

ex_call:

    /* Do positional arguments. */
    if (n == 0 && nelts == 1 && ((expr_ty)asdl_seq_GET(args, 0))->kind == Starred_kind) {
        VISIT(c, expr, ((expr_ty)asdl_seq_GET(args, 0))->v.Starred.value);
    }
    else {
        RETURN_IF_ERROR(starunpack_helper_impl(c, loc, args, injected_arg, n,
                                               QPC_BUILD_LIST, QP_OP_list_append,
                                               QP_OP_list_extend, 1));
    }
    /* Then keyword arguments */
    if (nkwelts) {
        /* Has a new dict been pushed */
        int have_dict = 0;

        nseen = 0;  /* the number of keyword arguments on the stack following */
        for (i = 0; i < nkwelts; i++) {
            keyword_ty kw = asdl_seq_GET(keywords, i);
            if (kw->arg == JS_ATOM_NULL) {
                /* A keyword argument unpacking. */
                if (nseen) {
                    RETURN_IF_ERROR(codegen_subkwargs(c, loc, keywords, i - nseen, i));
                    if (have_dict) {
                        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_merge, 1));
                    }
                    have_dict = 1;
                    nseen = 0;
                }
                if (!have_dict) {
                    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_build_map, 0));
                    have_dict = 1;
                }
                VISIT(c, expr, kw->value);
                RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_merge, 1));
            }
            else {
                nseen++;
            }
        }
        if (nseen) {
            /* Pack up any trailing keyword arguments. */
            RETURN_IF_ERROR(codegen_subkwargs(c, loc, keywords, nkwelts - nseen, nkwelts));
            if (have_dict) {
                RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_dict_merge, 1));
            }
            have_dict = 1;
        }
        assert(have_dict);
    }
    if (nkwelts == 0) {
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    }
    return qpc_py(c, loc, QP_OP_call_ex);
}

static int
codegen_call_helper(compiler *c, location loc,
                    int n, /* Args already pushed */
                    asdl_expr_seq *args,
                    asdl_keyword_seq *keywords)
{
    return codegen_call_helper_impl(c, loc, n, args, JS_ATOM_NULL, keywords);
}

static int
codegen_visit_keyword(compiler *c, keyword_ty k)
{
    VISIT(c, expr, k->value);
    return SUCCESS;
}

/* ---- expressions ------------------------------------------------------ */

static int
codegen_visit_expr(compiler *c, expr_ty e)
{
    /* Py_EnterRecursiveCall(" during compilation") */
    if (js_check_stack_overflow(c->ctx->rt, 0)) {
        JS_ThrowStackOverflow(c->ctx);
        return ERROR;
    }
    location loc = LOC(e);
    switch (e->kind) {
    case NamedExpr_kind:
        VISIT(c, expr, e->v.NamedExpr.value);
        RETURN_IF_ERROR(qpc_copy(c, loc, 1));
        VISIT(c, expr, e->v.NamedExpr.target);
        break;
    case BoolOp_kind:
        return codegen_boolop(c, e);
    case BinOp_kind:
        VISIT(c, expr, e->v.BinOp.left);
        VISIT(c, expr, e->v.BinOp.right);
        ADDOP_BINARY(c, loc, e->v.BinOp.op);
        break;
    case UnaryOp_kind:
        VISIT(c, expr, e->v.UnaryOp.operand);
        if (e->v.UnaryOp.op == UAdd) {
            RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_unop, QP_UNOP_POS));
        }
        else if (e->v.UnaryOp.op == Not) {
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_to_bool));
            RETURN_IF_ERROR(qpc_op(c, loc, OP_lnot));
        }
        else {
            int op = unaryop(e->v.UnaryOp.op);
            if (op < 0) {
                return qpc_error(c, loc, "unary op %d should not be possible",
                                 (int)e->v.UnaryOp.op);
            }
            RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_unop, op));
        }
        break;
    case Lambda_kind:
        return codegen_lambda(c, e);
    case IfExp_kind:
        return codegen_ifexp(c, e);
    case Dict_kind:
        return codegen_dict(c, e);
    case Set_kind:
        return codegen_set(c, e);
    case GeneratorExp_kind:
        return codegen_genexp(c, e);
    case ListComp_kind:
        return codegen_listcomp(c, e);
    case SetComp_kind:
        return codegen_setcomp(c, e);
    case DictComp_kind:
        return codegen_dictcomp(c, e);
    case Yield_kind:
        if (!_PyST_IsFunctionLike(SYMTABLE_ENTRY(c))) {
            return qpc_error(c, loc, "'yield' outside function");
        }
        if (e->v.Yield.value) {
            VISIT(c, expr, e->v.Yield.value);
        }
        else {
            RETURN_IF_ERROR(qpc_load_none(c, loc));
        }
        ADDOP_YIELD(c, loc);
        break;
    case YieldFrom_kind:
        if (!_PyST_IsFunctionLike(SYMTABLE_ENTRY(c))) {
            return qpc_error(c, loc, "'yield from' outside function");
        }
        if (SCOPE_TYPE(c) == COMPILE_SCOPE_ASYNC_FUNCTION) {
            return qpc_error(c, loc, "'yield from' inside async function");
        }
        VISIT(c, expr, e->v.YieldFrom.value);
        /* [recv, idx]; the None sent first is pushed by codegen_add_yield_from */
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_iter, QP_GETITER_YIELD_FROM));
        ADD_YIELD_FROM(c, loc, 0);
        break;
    case Await_kind:
        VISIT(c, expr, e->v.Await.value);
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_awaitable, QP_AWAITABLE_AWAIT));
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
        ADD_YIELD_FROM(c, loc, 1);
        break;
    case Compare_kind:
        return codegen_compare(c, e);
    case Call_kind:
        return codegen_call(c, e);
    case Constant_kind:
        RETURN_IF_ERROR(qpc_load_const(c, loc, e->v.Constant.value));
        break;
    case JoinedStr_kind:
        return codegen_joined_str(c, e);
    case TemplateStr_kind:
        return codegen_template_str(c, e);
    case FormattedValue_kind:
        return codegen_formatted_value(c, e);
    case Interpolation_kind:
        return codegen_interpolation(c, e);
    /* The following exprs can be assignment targets. */
    case Attribute_kind:
        if (e->v.Attribute.ctx == Load) {
            int ret = can_optimize_super_call(c, e);
            RETURN_IF_ERROR(ret);
            if (ret) {
                RETURN_IF_ERROR(load_args_for_super(c, e->v.Attribute.value));
                /* LOAD_SUPER_ATTR / LOAD_ZERO_SUPER_ATTR; the trailing NOP
                   only carries a location: not emitted */
                int flags = asdl_seq_LEN(e->v.Attribute.value->v.Call.args) ?
                    QP_SUPER_TWO_ARGS : 0;
                return qpc_addop_name(c, loc, QP_OP_load_super_attr,
                                      e->v.Attribute.attr, flags);
            }
        }
        RETURN_IF_ERROR(_PyCompile_MaybeAddStaticAttributeToClass(c, e));
        VISIT(c, expr, e->v.Attribute.value);
        loc = LOC(e);
        loc = update_start_location_to_match_attr(c, loc, e);
        switch (e->v.Attribute.ctx) {
        case Load:
            RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_getattr, e->v.Attribute.attr, 0));
            break;
        case Store:
            RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_setattr, e->v.Attribute.attr, 0));
            break;
        case Del:
            RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_delattr, e->v.Attribute.attr, 0));
            break;
        }
        break;
    case Subscript_kind:
        return codegen_subscript(c, e);
    case Starred_kind:
        switch (e->v.Starred.ctx) {
        case Store:
            /* In all legitimate cases, the Starred node was already replaced
             * by codegen_list/codegen_tuple. XXX: is that okay? */
            return qpc_error(c, loc,
                "starred assignment target must be in a list or tuple");
        default:
            return qpc_error(c, loc,
                "can't use starred expression here");
        }
        break;
    case Slice_kind:
        RETURN_IF_ERROR(codegen_slice(c, e));
        break;
    case Name_kind:
        return codegen_nameop(c, loc, e->v.Name.id, e->v.Name.ctx);
    /* child nodes of List and Tuple will have expr_context set */
    case List_kind:
        return codegen_list(c, e);
    case Tuple_kind:
        return codegen_tuple(c, e);
    }
    return SUCCESS;
}

static bool
is_constant_slice(expr_ty s)
{
    return s->kind == Slice_kind &&
        (s->v.Slice.lower == NULL ||
         s->v.Slice.lower->kind == Constant_kind) &&
        (s->v.Slice.upper == NULL ||
         s->v.Slice.upper->kind == Constant_kind) &&
        (s->v.Slice.step == NULL ||
         s->v.Slice.step->kind == Constant_kind);
}

static bool
should_apply_two_element_slice_optimization(expr_ty s)
{
    return !is_constant_slice(s) &&
           s->kind == Slice_kind &&
           s->v.Slice.step == NULL;
}

/* ---- subscripts and slices -------------------------------------------- */

static int
codegen_subscript(compiler *c, expr_ty e)
{
    location loc = LOC(e);
    expr_context_ty ctx = e->v.Subscript.ctx;

    /* check_subscripter / check_index: warnings only, emitted by qp_validate */

    VISIT(c, expr, e->v.Subscript.value);
    if (should_apply_two_element_slice_optimization(e->v.Subscript.slice) &&
        ctx != Del
    ) {
        RETURN_IF_ERROR(codegen_slice_two_parts(c, e->v.Subscript.slice));
        if (ctx == Load) {
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_getslice));
        }
        else {
            assert(ctx == Store);
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setslice));
        }
    }
    else {
        VISIT(c, expr, e->v.Subscript.slice);
        switch (ctx) {
            case Load:
                RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_getitem));
                break;
            case Store:
                RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setitem));
                break;
            case Del:
                RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_delitem));
                break;
        }
    }
    return SUCCESS;
}

static int
codegen_slice_two_parts(compiler *c, expr_ty s)
{
    if (s->v.Slice.lower) {
        VISIT(c, expr, s->v.Slice.lower);
    }
    else {
        RETURN_IF_ERROR(qpc_load_none(c, LOC(s)));
    }

    if (s->v.Slice.upper) {
        VISIT(c, expr, s->v.Slice.upper);
    }
    else {
        RETURN_IF_ERROR(qpc_load_none(c, LOC(s)));
    }

    return 0;
}

static int
codegen_slice(compiler *c, expr_ty s)
{
    int n = 2;
    assert(s->kind == Slice_kind);

    /* CPython folds constant slices (is_constant_slice) into a slice
       constant; slices are never constants here: built at runtime. */
    RETURN_IF_ERROR(codegen_slice_two_parts(c, s));

    if (s->v.Slice.step) {
        n++;
        VISIT(c, expr, s->v.Slice.step);
    }

    return qpc_py_u16(c, LOC(s), QP_OP_build_slice, n);
}
/* ---------------------------------------------------------------------- */
/*
 * Statements.
 *
 * Port of CPython 3.15.2 Python/codegen.c: frame-block unwinding,
 * yield-from loop, if/for/async for/while, return/break/continue,
 * try/except/finally and except*, imports, assert, expression statements,
 * with/async with, augmented and annotated assignment, codegen_visit_stmt.
 * Derived from CPython (PSF-2.0).
 *
 * ===================== EXCEPTION HANDLER LAYOUT =============================
 * CPython's `SETUP_CLEANUP L; PUSH_EXC_INFO` cannot be emitted in that order
 * here: the catch marker M would hide exc from push_exc_info.  Instead
 * (codegen_enter_except_handler):
 *
 *     [exc]  py push_exc_info  [prev, exc]  catch L  [prev, exc_s, M]
 *            dup above M (py copy 2)        [prev, exc_s, M, exc]
 *
 * exc_s (the saved exception) sits in the slot of CPython's `lasti`, so
 * the cleanup handler L starts with [prev, exc_s, exc2] = CPython's
 * [prev, lasti, exc2] and codegen_pop_except_and_reraise is CPython's
 * COPY 3 / POP_EXCEPT / RERAISE.  Above M the handler code sees CPython's
 * [prev, exc] shape ([prev, exc_s, M, exc]).  A marker never moves (rule of
 * compute_stack_size), so exc_s is dropped explicitly right after the
 * POP_BLOCK of M (normal exits and codegen_unwind_fblock HANDLER_CLEANUP /
 * FINALLY_END paths).
 *
 * `with` uses the same trick for SETUP_WITH: after __enter__
 *     [exit, self, value]  catch FINAL  [exit, self, value_s, M]
 *     py copy 2                         [exit, self, value_s, M, value]
 * so FINAL starts with [exit, self, value_s, exc] (value_s in lasti's
 * slot).  with_except_start takes the opcode.h layout [exit, self, prev,
 * exc] contiguous, so after `push_exc_info; catch CLEANUP` the four values
 * are copied above the cleanup marker (see codegen_with_handler_start).
 */

/* Defined in the expression slice (not in compile.h). */
static location update_start_location_to_match_attr(compiler *c, location loc,
                                                    expr_ty attr);

/* ---- helpers -------------------------------------------------------------- */

/* [exc] -> [prev, exc_s, M(cleanup), exc]  (see the header comment) */
static int
codegen_enter_except_handler(compiler *c, location loc, jump_target_label cleanup)
{
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_exc_info));
    RETURN_IF_ERROR(qpc_setup(c, loc, cleanup));
    return qpc_copy(c, loc, 2);
}

/* Normal exit of a handler region: [prev, exc_s, M] -> [prev]
   (CPython POP_BLOCK; the saved exception is dropped with it). */
static int
codegen_pop_except_handler_block(compiler *c, location loc)
{
    RETURN_IF_ERROR(qpc_pop_block(c, loc));
    return qpc_op(c, loc, OP_drop);
}

/* 1 if the atom's text starts with '*', 0 otherwise, -1 on OOM */
static int
codegen_atom_is_star(compiler *c, JSAtom name)
{
    const char *str = JS_AtomToCString(c->ctx, name);
    int r;
    if (!str) {
        return ERROR;
    }
    r = str[0] == '*';
    JS_FreeCString(c->ctx, str);
    return r;
}

/* ---- yield from / exits / unwinding ---------------------------------------- */

static int
codegen_call_exit_with_nones(compiler *c, location loc)
{
    RETURN_IF_ERROR(qpc_load_none(c, loc));
    RETURN_IF_ERROR(qpc_load_none(c, loc));
    RETURN_IF_ERROR(qpc_load_none(c, loc));
    return qpc_py_u16(c, loc, QP_OP_call_method, 3);
}

/* Caller pushed [recv, idx].  opcode.h `send` layout:
       catch FAIL; undefined            [recv, idx, M, v]
   L:  py send EXIT; py yield_value QP_YIELD_DELEGATE; goto L
   FAIL: [recv, idx, exc] py cleanup_throw; goto JOIN   [recv, idx, value]
   EXIT: [recv, idx, M, retval] nip_catch               [recv, idx, retval]
   JOIN: py end_send                                   [value]
   (`await` only selects CPython's RESUME oparg: nothing to emit.) */
static int
codegen_add_yield_from(compiler *c, location loc, int await)
{
    (void)await;
    NEW_JUMP_TARGET_LABEL(c, send);
    NEW_JUMP_TARGET_LABEL(c, fail);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, join);

    RETURN_IF_ERROR(qpc_setup(c, loc, fail));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_undefined));

    USE_LABEL(c, send);
    RETURN_IF_ERROR(qpc_py_jump(c, loc, QP_OP_send, exit));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_yield_value, QP_YIELD_DELEGATE));
    RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, send));

    USE_LABEL(c, fail);
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_cleanup_throw));
    RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, join));

    USE_LABEL(c, exit);
    RETURN_IF_ERROR(qpc_op(c, loc, OP_nip_catch));

    USE_LABEL(c, join);
    return qpc_py(c, loc, QP_OP_end_send);
}

static int
codegen_pop_except_and_reraise(compiler *c, location loc)
{
    /* Stack contents (exc_s occupies CPython's lasti slot, see top)
     * [exc_info, exc_s, exc]            COPY        3
     * [exc_info, exc_s, exc, exc_info]  POP_EXCEPT
     * [exc_info, exc_s, exc]            RERAISE
     * (exception_unwind clears the stack)
     */
    RETURN_IF_ERROR(qpc_copy(c, loc, 3));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_pop_except));
    return qpc_py(c, loc, QP_OP_reraise);
}

/* Unwind a frame block.  If preserve_tos is true, the TOS before
 * popping the blocks will be restored afterwards, unless another
 * return, break or continue is found. In which case, the TOS will
 * be popped.
 * POP_BLOCK with the preserved value above the marker is
 * qpc_pop_block_under (nip).
 */
static int
codegen_unwind_fblock(compiler *c, location *ploc,
                      fblockinfo *info, int preserve_tos)
{
    switch (info->fb_type) {
        case COMPILE_FBLOCK_WHILE_LOOP:
        case COMPILE_FBLOCK_EXCEPTION_HANDLER:
        case COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER:
        case COMPILE_FBLOCK_ASYNC_COMPREHENSION_GENERATOR:
        case COMPILE_FBLOCK_INLINED_COMPREHENSION:
        case COMPILE_FBLOCK_STOP_ITERATION:
            return SUCCESS;

        case COMPILE_FBLOCK_FOR_LOOP:
            /* Pop the iterator: [iter, idx(, v)] */
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 3));
            }
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            return SUCCESS;

        case COMPILE_FBLOCK_ASYNC_FOR_LOOP:
            /* Pop the iterator: [aiter(, v)] */
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            return SUCCESS;

        case COMPILE_FBLOCK_TRY_EXCEPT:
            /* [M(, v)] */
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
            }
            else {
                RETURN_IF_ERROR(qpc_pop_block(c, *ploc));
            }
            return SUCCESS;

        case COMPILE_FBLOCK_FINALLY_TRY:
            /* This POP_BLOCK gets the line number of the unwinding statement */
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
                RETURN_IF_ERROR(
                    _PyCompile_PushFBlock(c, *ploc, COMPILE_FBLOCK_POP_VALUE,
                                          NO_LABEL, NO_LABEL, NULL));
            }
            else {
                RETURN_IF_ERROR(qpc_pop_block(c, *ploc));
            }
            /* Emit the finally block */
            VISIT_SEQ(c, stmt, (asdl_stmt_seq *)info->fb_datum);
            if (preserve_tos) {
                _PyCompile_PopFBlock(c, COMPILE_FBLOCK_POP_VALUE, NO_LABEL);
            }
            /* The finally block should appear to execute after the
             * statement causing the unwinding, so make the unwinding
             * instruction artificial */
            *ploc = NO_LOCATION;
            return SUCCESS;

        case COMPILE_FBLOCK_FINALLY_END:
            /* [prev, exc_s, M, exc(, v)] */
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop)); /* exc_value */
            if (preserve_tos) {
                /* [prev, exc_s, M, v] -> [prev, v] -> [v, prev] */
                RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
                RETURN_IF_ERROR(qpc_op(c, *ploc, OP_nip));
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            else {
                RETURN_IF_ERROR(codegen_pop_except_handler_block(c, *ploc));
            }
            RETURN_IF_ERROR(qpc_py(c, *ploc, QP_OP_pop_except));
            return SUCCESS;

        case COMPILE_FBLOCK_WITH:
        case COMPILE_FBLOCK_ASYNC_WITH:
            /* [exit, self, value_s, M(, v)] */
            *ploc = info->fb_loc;
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
                RETURN_IF_ERROR(qpc_op(c, *ploc, OP_nip));
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 3));
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            else {
                RETURN_IF_ERROR(qpc_pop_block(c, *ploc));
                RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            }
            RETURN_IF_ERROR(codegen_call_exit_with_nones(c, *ploc));
            if (info->fb_type == COMPILE_FBLOCK_ASYNC_WITH) {
                RETURN_IF_ERROR(qpc_py_u8(c, *ploc, QP_OP_get_awaitable, QP_AWAITABLE_AEXIT));
                RETURN_IF_ERROR(qpc_py(c, *ploc, QP_OP_push_null));
                ADD_YIELD_FROM(c, *ploc, 1);
            }
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            /* The exit block should appear to execute after the
             * statement causing the unwinding, so make the unwinding
             * instruction artificial */
            *ploc = NO_LOCATION;
            return SUCCESS;

        case COMPILE_FBLOCK_HANDLER_CLEANUP: {
            /* unnamed: [prev, exc_s, M(, v)]
               named:   [prev, exc_s, M, M_name(, v)] */
            if (info->fb_name != JS_ATOM_NULL) {
                if (preserve_tos) {
                    RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
                }
                else {
                    RETURN_IF_ERROR(qpc_pop_block(c, *ploc));
                }
            }
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_pop_block_under(c, *ploc));
                RETURN_IF_ERROR(qpc_op(c, *ploc, OP_nip));   /* exc_s */
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            else {
                RETURN_IF_ERROR(codegen_pop_except_handler_block(c, *ploc));
            }
            RETURN_IF_ERROR(qpc_py(c, *ploc, QP_OP_pop_except));
            if (info->fb_name != JS_ATOM_NULL) {
                RETURN_IF_ERROR(qpc_load_none(c, *ploc));
                RETURN_IF_ERROR(codegen_nameop(c, *ploc, info->fb_name, Store));
                RETURN_IF_ERROR(codegen_nameop(c, *ploc, info->fb_name, Del));
            }
            return SUCCESS;
        }
        case COMPILE_FBLOCK_POP_VALUE: {
            if (preserve_tos) {
                RETURN_IF_ERROR(qpc_swap(c, *ploc, 2));
            }
            RETURN_IF_ERROR(qpc_op(c, *ploc, OP_drop));
            return SUCCESS;
        }
    }
    abort();  /* Py_UNREACHABLE */
}

/** Unwind block stack. If loop is not NULL, then stop when the first loop is encountered. */
static int
codegen_unwind_fblock_stack(compiler *c, location *ploc,
                            int preserve_tos, fblockinfo **loop)
{
    fblockinfo *top = _PyCompile_TopFBlock(c);
    if (top == NULL) {
        return SUCCESS;
    }
    if (top->fb_type == COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER) {
        return qpc_error(
            c, *ploc, "'break', 'continue' and 'return' cannot appear in an except* block");
    }
    if (loop != NULL && (top->fb_type == COMPILE_FBLOCK_WHILE_LOOP ||
                         top->fb_type == COMPILE_FBLOCK_FOR_LOOP ||
                         top->fb_type == COMPILE_FBLOCK_ASYNC_FOR_LOOP)) {
        *loop = top;
        return SUCCESS;
    }
    fblockinfo copy = *top;
    _PyCompile_PopFBlock(c, top->fb_type, top->fb_block);
    RETURN_IF_ERROR(codegen_unwind_fblock(c, ploc, &copy, preserve_tos));
    RETURN_IF_ERROR(codegen_unwind_fblock_stack(c, ploc, preserve_tos, loop));
    if (copy.fb_name != JS_ATOM_NULL) {
        RETURN_IF_ERROR(qpc_push_fblock_name(c, copy.fb_loc, copy.fb_type, copy.fb_block,
                                             copy.fb_exit, copy.fb_name));
    }
    else {
        RETURN_IF_ERROR(_PyCompile_PushFBlock(c, copy.fb_loc, copy.fb_type, copy.fb_block,
                                              copy.fb_exit, copy.fb_datum));
    }
    return SUCCESS;
}

/* ---- control flow ------------------------------------------------------------ */

static int
codegen_if(compiler *c, stmt_ty s)
{
    jump_target_label next;
    assert(s->kind == If_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    if (asdl_seq_LEN(s->v.If.orelse)) {
        NEW_JUMP_TARGET_LABEL(c, orelse);
        next = orelse;
    }
    else {
        next = end;
    }
    RETURN_IF_ERROR(
        codegen_jump_if(c, LOC(s), s->v.If.test, next, 0));

    VISIT_SEQ(c, stmt, s->v.If.body);
    if (asdl_seq_LEN(s->v.If.orelse)) {
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

        USE_LABEL(c, next);
        VISIT_SEQ(c, stmt, s->v.If.orelse);
    }

    USE_LABEL(c, end);
    return SUCCESS;
}

/* Stack: [iter, idx] during the loop; for_iter exhausted lands on cleanup
   with [iter, idx] (no END_FOR), POP_ITER = two drops. */
static int
codegen_for(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    NEW_JUMP_TARGET_LABEL(c, end);

    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_FOR_LOOP, start, end, NULL));

    VISIT(c, expr, s->v.For.iter);

    loc = LOC(s->v.For.iter);
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_iter, QP_GETITER_NORMAL));

    USE_LABEL(c, start);
    RETURN_IF_ERROR(qpc_py_jump(c, loc, QP_OP_for_iter, cleanup));

    /* (CPython's line-tracing NOP for the target is not emitted) */
    USE_LABEL(c, body);
    VISIT(c, expr, s->v.For.target);
    VISIT_SEQ(c, stmt, s->v.For.body);
    /* Mark jump as artificial */
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, start));

    USE_LABEL(c, cleanup);
    /* END_FOR is not emitted; POP_ITER */
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_FOR_LOOP, start);

    VISIT_SEQ(c, stmt, s->v.For.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

/* Stack: [aiter] at `start`.  The marker of the SETUP_FINALLY hides aiter,
   so it is copied above it for get_anext:
     [aiter] catch EXCEPT  [aiter, M]  copy 2  [aiter, M, aiter]
     get_anext  [aiter, M, aiter, awaitable]  swap; drop  [aiter, M, awaitable]
     push_null; yield from  [aiter, M, value]  nip (POP_BLOCK)  [aiter, value]
   EXCEPT starts with [aiter, exc]; end_async_for -> [] (StopAsyncIteration
   falls through to the else block, anything else is re-raised). */
static int
codegen_async_for(compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, end);

    VISIT(c, expr, s->v.AsyncFor.iter);
    RETURN_IF_ERROR(qpc_py(c, LOC(s->v.AsyncFor.iter), QP_OP_get_aiter));

    USE_LABEL(c, start);
    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_ASYNC_FOR_LOOP, start, end, NULL));

    /* SETUP_FINALLY to guard the __anext__ call */
    RETURN_IF_ERROR(qpc_setup(c, loc, except));
    RETURN_IF_ERROR(qpc_copy(c, loc, 2));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_get_anext));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_swap));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    /* (CPython's `send` label only serves END_ASYNC_FOR instrumentation) */
    ADD_YIELD_FROM(c, loc, 1);
    RETURN_IF_ERROR(qpc_pop_block_under(c, loc));  /* for SETUP_FINALLY */

    /* Success block for __anext__ */
    VISIT(c, expr, s->v.AsyncFor.target);
    VISIT_SEQ(c, stmt, s->v.AsyncFor.body);
    /* Mark jump as artificial */
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, start));

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_ASYNC_FOR_LOOP, start);

    /* Except block for __anext__ */
    USE_LABEL(c, except);

    /* Use same line number as the iterator,
     * as the END_ASYNC_FOR succeeds the `for`, not the body. */
    loc = LOC(s->v.AsyncFor.iter);
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_end_async_for));

    /* `else` block */
    VISIT_SEQ(c, stmt, s->v.AsyncFor.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
codegen_while(compiler *c, stmt_ty s)
{
    NEW_JUMP_TARGET_LABEL(c, loop);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, anchor);

    USE_LABEL(c, loop);

    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, LOC(s), COMPILE_FBLOCK_WHILE_LOOP, loop, end, NULL));
    RETURN_IF_ERROR(codegen_jump_if(c, LOC(s), s->v.While.test, anchor, 0));

    VISIT_SEQ(c, stmt, s->v.While.body);
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, loop));

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_WHILE_LOOP, loop);

    USE_LABEL(c, anchor);
    if (s->v.While.orelse) {
        VISIT_SEQ(c, stmt, s->v.While.orelse);
    }

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
codegen_return(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    int preserve_tos = ((s->v.Return.value != NULL) &&
                        (s->v.Return.value->kind != Constant_kind));

    PySTEntryObject *ste = SYMTABLE_ENTRY(c);
    if (!_PyST_IsFunctionLike(ste)) {
        return qpc_error(c, loc, "'return' outside function");
    }
    if (s->v.Return.value != NULL && ste->ste_coroutine && ste->ste_generator) {
        return qpc_error(c, loc, "'return' with value in async generator");
    }

    if (preserve_tos) {
        VISIT(c, expr, s->v.Return.value);
    } else {
        /* (CPython emits a NOP with the value's line number) */
        if (s->v.Return.value != NULL) {
            loc = LOC(s->v.Return.value);
        }
    }
    if (s->v.Return.value == NULL || s->v.Return.value->lineno != s->lineno) {
        loc = LOC(s);
    }

    RETURN_IF_ERROR(codegen_unwind_fblock_stack(c, &loc, preserve_tos, NULL));
    if (s->v.Return.value == NULL) {
        RETURN_IF_ERROR(qpc_load_none(c, loc));
    }
    else if (!preserve_tos) {
        RETURN_IF_ERROR(qpc_load_const(c, loc, s->v.Return.value->v.Constant.value));
    }
    return qpc_return(c, loc);
}

static int
codegen_break(compiler *c, location loc)
{
    fblockinfo *loop = NULL;
    location origin_loc = loc;
    /* (CPython emits a NOP with the line number) */
    RETURN_IF_ERROR(codegen_unwind_fblock_stack(c, &loc, 0, &loop));
    if (loop == NULL) {
        return qpc_error(c, origin_loc, "'break' outside loop");
    }
    RETURN_IF_ERROR(codegen_unwind_fblock(c, &loc, loop, 0));
    return qpc_jump(c, loc, OP_goto, loop->fb_exit);
}

static int
codegen_continue(compiler *c, location loc)
{
    fblockinfo *loop = NULL;
    location origin_loc = loc;
    /* (CPython emits a NOP with the line number) */
    RETURN_IF_ERROR(codegen_unwind_fblock_stack(c, &loc, 0, &loop));
    if (loop == NULL) {
        return qpc_error(c, origin_loc, "'continue' not properly in loop");
    }
    return qpc_jump(c, loc, OP_goto, loop->fb_block);
}

/* ---- try ---------------------------------------------------------------------- */

/* Code generated for "try: <body> finally: <finalbody>":

        catch            L          [M]
        <code for body>
        POP_BLOCK                   []
        <code for finalbody>
        JUMP E
    L:                              [exc]
        push_exc_info; catch C; copy 2   [prev, exc_s, M2, exc]
        <code for finalbody>
        RERAISE
    C:  [prev, exc_s, exc2]  COPY 3; POP_EXCEPT; RERAISE
    E:
*/
static int
codegen_try_finally(compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* `try` block */
    RETURN_IF_ERROR(qpc_setup(c, loc, end));

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_FINALLY_TRY, body, end,
                              s->v.Try.finalbody));

    if (s->v.Try.handlers && asdl_seq_LEN(s->v.Try.handlers)) {
        RETURN_IF_ERROR(codegen_try_except(c, s));
    }
    else {
        VISIT_SEQ(c, stmt, s->v.Try.body);
    }
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_FINALLY_TRY, body);
    VISIT_SEQ(c, stmt, s->v.Try.finalbody);

    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, exit));
    /* `finally` block */

    USE_LABEL(c, end);

    loc = NO_LOCATION;
    RETURN_IF_ERROR(codegen_enter_except_handler(c, loc, cleanup));
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_FINALLY_END, end, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.Try.finalbody);
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_FINALLY_END, end);

    loc = NO_LOCATION;
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_reraise));

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, loc);

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
codegen_try_star_finally(compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    /* `try` block */
    RETURN_IF_ERROR(qpc_setup(c, loc, end));

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_FINALLY_TRY, body, end,
                              s->v.TryStar.finalbody));

    if (s->v.TryStar.handlers && asdl_seq_LEN(s->v.TryStar.handlers)) {
        RETURN_IF_ERROR(codegen_try_star_except(c, s));
    }
    else {
        VISIT_SEQ(c, stmt, s->v.TryStar.body);
    }
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_FINALLY_TRY, body);
    VISIT_SEQ(c, stmt, s->v.TryStar.finalbody);

    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, exit));

    /* `finally` block */
    USE_LABEL(c, end);

    loc = NO_LOCATION;
    /* [exc] -> [prev, exc_s, M, exc] */
    RETURN_IF_ERROR(codegen_enter_except_handler(c, loc, cleanup));
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_FINALLY_END, end, NO_LABEL, NULL));

    VISIT_SEQ(c, stmt, s->v.TryStar.finalbody);

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_FINALLY_END, end);
    loc = NO_LOCATION;
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_reraise));

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, loc);

    USE_LABEL(c, exit);
    return SUCCESS;
}

/*
   Code generated for "try: S except E1 as V1: S1 except E2 as V2: S2 ...":
   (Value stack in [], top at the right.)

   []                           catch           L1      [M]
   [M]                          <code for S>
   []                           POP_BLOCK; <else>; JUMP L0

   [exc]                L1:     push_exc_info; catch C; copy 2
   [prev, exc_s, M, exc]        <evaluate E1>; CHECK_EXC_MATCH; if_false L2
   [prev, exc_s, M, exc]        <assign to V1> (or POP if no V1)
   [prev, exc_s, M(, M1)]       <code for S1> (named: inside catch R1 with
                                the `name = None; del name` cleanup)
   []                           POP_BLOCK(s); drop exc_s; POP_EXCEPT; JUMP L0

   [prev, exc_s, M, exc]  L2:   <evaluate E2> ... etc.
   [prev, exc_s, M, exc]  Ln+1: RERAISE
   [prev, exc_s, exc2]    C:    COPY 3; POP_EXCEPT; RERAISE
   []                     L0:   <next statement>
*/
static int
codegen_try_except(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    Py_ssize_t i, n;

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    RETURN_IF_ERROR(qpc_setup(c, loc, except));

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_TRY_EXCEPT, body, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.Try.body);
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_TRY_EXCEPT, body);
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    if (s->v.Try.orelse && asdl_seq_LEN(s->v.Try.orelse)) {
        VISIT_SEQ(c, stmt, s->v.Try.orelse);
    }
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));
    n = asdl_seq_LEN(s->v.Try.handlers);

    USE_LABEL(c, except);

    /* [exc] -> [prev, exc_s, M, exc] */
    RETURN_IF_ERROR(codegen_enter_except_handler(c, NO_LOCATION, cleanup));

    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_EXCEPTION_HANDLER,
                              NO_LABEL, NO_LABEL, NULL));

    for (i = 0; i < n; i++) {
        excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
            s->v.Try.handlers, i);
        location loc = LOC(handler);
        if (!handler->v.ExceptHandler.type && i < n-1) {
            return qpc_error(c, loc, "default 'except:' must be last");
        }
        NEW_JUMP_TARGET_LABEL(c, next_except);
        except = next_except;
        if (handler->v.ExceptHandler.type) {
            VISIT(c, expr, handler->v.ExceptHandler.type);
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_check_exc_match));
            RETURN_IF_ERROR(qpc_jump(c, loc, OP_if_false, except));
        }
        if (handler->v.ExceptHandler.name) {
            NEW_JUMP_TARGET_LABEL(c, cleanup_end);
            NEW_JUMP_TARGET_LABEL(c, cleanup_body);

            /* [prev, exc_s, M, exc] -> [prev, exc_s, M] (exc is a copy
               above the marker: it may be consumed) */
            RETURN_IF_ERROR(
                codegen_nameop(c, loc, handler->v.ExceptHandler.name, Store));

            /*
              try:
                  # body
              except type as name:
                  try:
                      # body
                  finally:
                      name = None # in case body contains "del name"
                      del name
            */

            /* second try: [prev, exc_s, M, M1] */
            RETURN_IF_ERROR(qpc_setup(c, loc, cleanup_end));

            USE_LABEL(c, cleanup_body);
            RETURN_IF_ERROR(
                qpc_push_fblock_name(c, loc, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body,
                                     NO_LABEL, handler->v.ExceptHandler.name));

            /* second # body */
            VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
            _PyCompile_PopFBlock(c, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body);
            /* name = None; del name; # Mark as artificial */
            RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
            RETURN_IF_ERROR(codegen_pop_except_handler_block(c, NO_LOCATION));
            RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_pop_except));
            RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
            RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

            /* except: [prev, exc_s, M, exc1] (no lasti) */
            USE_LABEL(c, cleanup_end);

            /* name = None; del name; # artificial */
            RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));

            RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_reraise));
        }
        else {
            NEW_JUMP_TARGET_LABEL(c, cleanup_body);

            /* [prev, exc_s, M, exc] -> [prev, exc_s, M] */
            RETURN_IF_ERROR(qpc_op(c, loc, OP_drop)); /* exc_value */

            USE_LABEL(c, cleanup_body);
            RETURN_IF_ERROR(
                _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body,
                                      NO_LABEL, NULL));

            VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
            _PyCompile_PopFBlock(c, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body);
            RETURN_IF_ERROR(codegen_pop_except_handler_block(c, NO_LOCATION));
            RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_pop_except));
            RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));
        }

        USE_LABEL(c, except);
    }
    /* artificial */
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_EXCEPTION_HANDLER, NO_LABEL);
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_reraise));

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, end);
    return SUCCESS;
}

/*
   Code generated for "try: S except* E1 as V1: S1 except* E2 as V2: S2 ...":
   (Value stack in [], top at the right; P = [prev, orig_s, M] is the
   handler base, CPython's [prev].)

   []                                   catch           L1
   []                                   <code for S>
   []                                   POP_BLOCK; JUMP L0

   [orig]                       L1:     push_exc_info; catch C; copy 2
   [P, orig]                            BUILD_LIST 0 (array_from)
   [P, orig, res]                       COPY 2

   [P, orig, res, exc]                  <evaluate E1>
   [P, orig, res, exc, E1]              CHECK_EG_MATCH
   [P, orig, res, rest/exc, match?]     COPY 1
   [P, orig, res, rest/exc, match?, match?] POP_JUMP_IF_NONE C1

   [P, orig, res, rest, match]          <assign to V1>  (or POP if no V1)

   [P, orig, res, rest]                 catch           R1
   [P, orig, res, rest, M1]             <code for S1>
   [P, orig, res, rest]                 POP_BLOCK; JUMP L2

   [P, orig, res, rest, v]      R1:     LIST_APPEND 2 (no lasti) ; JUMP LE2

   [P, orig, res, rest]         L2:     JUMP LE2

   [P, orig, res, rest/exc, None] C1:   POP

   [P, orig, res, rest]        LE2:     <evaluate E2>
   .............................etc.......................

   [P, orig, res, rest]        Ln+1:    LIST_APPEND 1

   [P, orig, res]                       CALL_INTRINSIC_2 PREP_RERAISE_STAR
   [P, exc]                             COPY 1; POP_JUMP_IF_NOT_NONE RER
   [P, exc]                             POP_TOP
   [prev, orig_s, M]                    POP_BLOCK; drop orig_s; POP_EXCEPT; JUMP L0

   [prev, orig_s, M, exc]       RER:    nip (POP_BLOCK); nip orig_s
   [prev, exc]                          SWAP 2; POP_EXCEPT; RERAISE

   [prev, orig_s, exc2]         C:      COPY 3; POP_EXCEPT; RERAISE
   []                           L0:     <next statement>
*/
static int
codegen_try_star_except(compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, orelse);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    NEW_JUMP_TARGET_LABEL(c, reraise_star);

    RETURN_IF_ERROR(qpc_setup(c, loc, except));

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_TRY_EXCEPT, body, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.TryStar.body);
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_TRY_EXCEPT, body);
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, orelse));
    Py_ssize_t n = asdl_seq_LEN(s->v.TryStar.handlers);

    USE_LABEL(c, except);

    /* [orig] -> [prev, orig_s, M, orig] */
    RETURN_IF_ERROR(codegen_enter_except_handler(c, NO_LOCATION, cleanup));

    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER,
                              NO_LABEL, NO_LABEL, NULL));

    for (Py_ssize_t i = 0; i < n; i++) {
        excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
            s->v.TryStar.handlers, i);
        location loc = LOC(handler);
        NEW_JUMP_TARGET_LABEL(c, next_except);
        except = next_except;
        NEW_JUMP_TARGET_LABEL(c, except_with_error);
        NEW_JUMP_TARGET_LABEL(c, no_match);
        if (i == 0) {
            /* create empty list for exceptions raised/reraise in the except* blocks */
            /*
               [orig]       BUILD_LIST
            */
            /* Create a copy of the original EG */
            /*
               [orig, []]   COPY 2
               [orig, [], exc]
            */
            RETURN_IF_ERROR(qpc_op_u16(c, loc, OP_array_from, 0));
            RETURN_IF_ERROR(qpc_copy(c, loc, 2));
        }
        if (handler->v.ExceptHandler.type) {
            VISIT(c, expr, handler->v.ExceptHandler.type);
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_check_eg_match));
            RETURN_IF_ERROR(qpc_copy(c, loc, 1));
            RETURN_IF_ERROR(qpc_op(c, loc, OP_is_undefined_or_null));
            RETURN_IF_ERROR(qpc_jump(c, loc, OP_if_true, no_match));
        }

        NEW_JUMP_TARGET_LABEL(c, cleanup_end);
        NEW_JUMP_TARGET_LABEL(c, cleanup_body);

        if (handler->v.ExceptHandler.name) {
            RETURN_IF_ERROR(
                codegen_nameop(c, loc, handler->v.ExceptHandler.name, Store));
        }
        else {
            RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));  // match
        }

        /*
          try:
              # body
          except type as name:
              try:
                  # body
              finally:
                  name = None # in case body contains "del name"
                  del name
        */
        /* second try: */
        RETURN_IF_ERROR(qpc_setup(c, loc, cleanup_end));

        USE_LABEL(c, cleanup_body);
        if (handler->v.ExceptHandler.name) {
            RETURN_IF_ERROR(
                qpc_push_fblock_name(c, loc, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body,
                                     NO_LABEL, handler->v.ExceptHandler.name));
        }
        else {
            RETURN_IF_ERROR(
                _PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body,
                                      NO_LABEL, NULL));
        }

        /* second # body */
        VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
        _PyCompile_PopFBlock(c, COMPILE_FBLOCK_HANDLER_CLEANUP, cleanup_body);
        /* name = None; del name; # artificial */
        RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
        if (handler->v.ExceptHandler.name) {
            RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
        }
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, except));

        /* except: [P, orig, res, rest, exc1] (no lasti slot) */
        USE_LABEL(c, cleanup_end);

        /* name = None; del name; # artificial */
        if (handler->v.ExceptHandler.name) {
            RETURN_IF_ERROR(qpc_load_none(c, NO_LOCATION));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                codegen_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
        }

        /* add exception raised to the res list (CPython LIST_APPEND 3 +
           POP_TOP of lasti) */
        RETURN_IF_ERROR(qpc_py_u16(c, NO_LOCATION, QP_OP_list_append, 2)); // exc
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, except_with_error));

        USE_LABEL(c, except);
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, except_with_error));

        USE_LABEL(c, no_match);
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));  // match (None)

        USE_LABEL(c, except_with_error);

        if (i == n - 1) {
            /* Add exc to the list (if not None it's the unhandled part of the EG) */
            RETURN_IF_ERROR(qpc_py_u16(c, NO_LOCATION, QP_OP_list_append, 1));
            RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, reraise_star));
        }
    }
    /* artificial */
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER, NO_LABEL);
    NEW_JUMP_TARGET_LABEL(c, reraise);

    USE_LABEL(c, reraise_star);
    RETURN_IF_ERROR(qpc_py_u8(c, NO_LOCATION, QP_OP_intrinsic2,
                              QP_INTRINSIC2_PREP_RERAISE_STAR));
    RETURN_IF_ERROR(qpc_copy(c, NO_LOCATION, 1));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_is_undefined_or_null));
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_if_false, reraise));

    /* Nothing to reraise */
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(codegen_pop_except_handler_block(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_pop_except));
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));

    USE_LABEL(c, reraise);
    /* [prev, orig_s, M, exc] -> [prev, exc] */
    RETURN_IF_ERROR(qpc_pop_block_under(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_nip));
    RETURN_IF_ERROR(qpc_swap(c, NO_LOCATION, 2));
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_pop_except));
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_reraise));

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, orelse);
    VISIT_SEQ(c, stmt, s->v.TryStar.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
codegen_try(compiler *c, stmt_ty s) {
    if (s->v.Try.finalbody && asdl_seq_LEN(s->v.Try.finalbody))
        return codegen_try_finally(c, s);
    else
        return codegen_try_except(c, s);
}

static int
codegen_try_star(compiler *c, stmt_ty s)
{
    if (s->v.TryStar.finalbody && asdl_seq_LEN(s->v.TryStar.finalbody)) {
        return codegen_try_star_finally(c, s);
    }
    else {
        return codegen_try_star_except(c, s);
    }
}

/* ---- imports ------------------------------------------------------------------ */

static int
codegen_import_as(compiler *c, location loc,
                  JSAtom name, JSAtom asname)
{
    /* The IMPORT_NAME opcode was already generated.  This function
       merely needs to bind the result to a name.

       If there is a dot in name, we need to split it and emit a
       IMPORT_FROM for each name.
    */
    const char *str = JS_AtomToCString(c->ctx, name);
    if (!str) {
        return ERROR;
    }
    const char *dot = strchr(str, '.');
    if (dot != NULL) {
        /* Consume the base module name to get the first attribute */
        while (1) {
            const char *pos = dot + 1;
            JSAtom attr;
            int r;
            dot = strchr(pos, '.');
            attr = JS_NewAtomLen(c->ctx, pos,
                                 dot != NULL ? (size_t)(dot - pos) : strlen(pos));
            if (attr == JS_ATOM_NULL) {
                JS_FreeCString(c->ctx, str);
                return ERROR;
            }
            /* ADDOP_N: not mangled */
            r = qpc_py_atom(c, loc, QP_OP_import_from, attr);
            JS_FreeAtom(c->ctx, attr);
            if (r < 0) {
                JS_FreeCString(c->ctx, str);
                return ERROR;
            }
            if (dot == NULL) {
                break;
            }
            if (qpc_swap(c, loc, 2) < 0 || qpc_op(c, loc, OP_drop) < 0) {
                JS_FreeCString(c->ctx, str);
                return ERROR;
            }
        }
        JS_FreeCString(c->ctx, str);
        RETURN_IF_ERROR(codegen_nameop(c, loc, asname, Store));
        return qpc_op(c, loc, OP_drop);
    }
    JS_FreeCString(c->ctx, str);
    return codegen_nameop(c, loc, asname, Store);
}

static int
codegen_validate_lazy_import(compiler *c, location loc)
{
    if (SCOPE_TYPE(c) != COMPILE_SCOPE_MODULE) {
        return qpc_error(
            c, loc, "lazy imports only allowed in module scope");
    }

    return SUCCESS;
}

static int
codegen_import(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    /* The Import node stores a module name like a.b.c as a single
       string.  This is convenient for all cases except
         import a.b.c as d
       where we need to parse that string to extract the individual
       module names.
     */
    Py_ssize_t i, n = asdl_seq_LEN(s->v.Import.names);

    for (i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.Import.names, i);
        int r;

        RETURN_IF_ERROR(qpc_load_small_int(c, loc, 0));
        RETURN_IF_ERROR(qpc_load_none(c, loc));
        if (s->v.Import.is_lazy) {
            RETURN_IF_ERROR(codegen_validate_lazy_import(c, loc));
            RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_import_name, alias->name,
                                           QP_IMPORT_LAZY));
        } else {
            if (_PyCompile_InExceptionHandler(c) ||
                SCOPE_TYPE(c) != COMPILE_SCOPE_MODULE) {
                // force eager import in try/except block
                RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_import_name, alias->name,
                                               QP_IMPORT_EAGER));
            } else {
                RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_import_name, alias->name,
                                               QP_IMPORT_DEFAULT));
            }
        }

        if (alias->asname) {
            r = codegen_import_as(c, loc, alias->name, alias->asname);
            RETURN_IF_ERROR(r);
        }
        else {
            JSAtom tmp = alias->name;
            const char *str = JS_AtomToCString(c->ctx, alias->name);
            const char *dot;
            if (!str) {
                return ERROR;
            }
            dot = strchr(str, '.');
            if (dot != NULL) {
                tmp = JS_NewAtomLen(c->ctx, str, (size_t)(dot - str));
            }
            JS_FreeCString(c->ctx, str);
            if (tmp == JS_ATOM_NULL) {
                return ERROR;
            }
            r = codegen_nameop(c, loc, tmp, Store);
            if (dot != NULL) {
                JS_FreeAtom(c->ctx, tmp);
            }
            RETURN_IF_ERROR(r);
        }
    }
    return SUCCESS;
}

static int
codegen_from_import(compiler *c, stmt_ty s)
{
    Py_ssize_t n = asdl_seq_LEN(s->v.ImportFrom.names);
    JSAtom *names;
    int r, is_star;

    RETURN_IF_ERROR(qpc_load_small_int(c, LOC(s), s->v.ImportFrom.level));

    names = js_malloc(c->ctx, sizeof(JSAtom) * (n ? n : 1));
    if (!names) {
        return ERROR;
    }

    /* build up the names */
    for (Py_ssize_t i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        names[i] = alias->name;
    }

    r = qpc_load_atom_tuple(c, LOC(s), names, (int)n);
    js_free(c->ctx, names);
    RETURN_IF_ERROR(r);

    JSAtom from = c->ids.empty;
    if (s->v.ImportFrom.module) {
        from = s->v.ImportFrom.module;
    }
    alias_ty alias0 = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, 0);
    is_star = codegen_atom_is_star(c, alias0->name);
    RETURN_IF_ERROR(is_star);
    if (s->v.ImportFrom.is_lazy) {
        if (is_star) {
            return qpc_error(c, LOC(s), "cannot lazy import *");
        }
        RETURN_IF_ERROR(codegen_validate_lazy_import(c, LOC(s)));
        RETURN_IF_ERROR(qpc_addop_name(c, LOC(s), QP_OP_import_name, from,
                                       QP_IMPORT_LAZY));
    } else {
        if (_PyCompile_InExceptionHandler(c) ||
            SCOPE_TYPE(c) != COMPILE_SCOPE_MODULE ||
            is_star) {
            // forced non-lazy import due to try/except or import *
            RETURN_IF_ERROR(qpc_addop_name(c, LOC(s), QP_OP_import_name, from,
                                           QP_IMPORT_EAGER));
        } else {
            RETURN_IF_ERROR(qpc_addop_name(c, LOC(s), QP_OP_import_name, from,
                                           QP_IMPORT_DEFAULT));
        }
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        JSAtom store_name;

        if (i == 0 && is_star) {
            assert(n == 1);
            RETURN_IF_ERROR(qpc_py_u8(c, LOC(s), QP_OP_intrinsic1,
                                      QP_INTRINSIC1_IMPORT_STAR));
            return qpc_op(c, NO_LOCATION, OP_drop);
        }

        RETURN_IF_ERROR(qpc_addop_name(c, LOC(s), QP_OP_import_from, alias->name, 0));
        store_name = alias->name;
        if (alias->asname) {
            store_name = alias->asname;
        }

        RETURN_IF_ERROR(codegen_nameop(c, LOC(s), store_name, Store));
    }
    /* remove imported module */
    return qpc_op(c, LOC(s), OP_drop);
}

/* ---- simple statements ------------------------------------------------------ */

static int
codegen_assert(compiler *c, stmt_ty s)
{
    /* (the "assertion is always true" SyntaxWarning is emitted by qp_validate) */
    if (OPTIMIZATION_LEVEL(c)) {
        return SUCCESS;
    }
    NEW_JUMP_TARGET_LABEL(c, end);
    RETURN_IF_ERROR(codegen_jump_if(c, LOC(s), s->v.Assert.test, end, 1));
    RETURN_IF_ERROR(qpc_py_u8(c, LOC(s), QP_OP_load_common_const,
                              QP_COMMON_CONST_ASSERTIONERROR));
    if (s->v.Assert.msg) {
        VISIT(c, expr, s->v.Assert.msg);
        /* [AssertionError, msg] CALL 0: msg in the self slot */
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(s), QP_OP_call_method, 0));
    }
    RETURN_IF_ERROR(qpc_py_u16(c, LOC(s->v.Assert.test), QP_OP_raise, 1));

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
codegen_stmt_expr(compiler *c, location loc, expr_ty value)
{
    if (IS_INTERACTIVE_TOP_LEVEL(c)) {
        VISIT(c, expr, value);
        RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_intrinsic1, QP_INTRINSIC1_PRINT));
        return qpc_op(c, NO_LOCATION, OP_drop);
    }

    if (value->kind == Constant_kind) {
        /* ignore constant statement (CPython's NOP is not emitted) */
        return SUCCESS;
    }

    VISIT(c, expr, value);
    return qpc_op(c, NO_LOCATION, OP_drop); /* artificial */
}

#define CODEGEN_COND_BLOCK(FUNC, C, S) \
    do { \
        _PyCompile_EnterConditionalBlock((C)); \
        int result = FUNC((C), (S)); \
        _PyCompile_LeaveConditionalBlock((C)); \
        return result; \
    } while(0)

static int
codegen_visit_stmt(compiler *c, stmt_ty s)
{

    switch (s->kind) {
    case FunctionDef_kind:
        return codegen_function(c, s, 0);
    case ClassDef_kind:
        return codegen_class(c, s);
    case TypeAlias_kind:
        return codegen_typealias(c, s);
    case Return_kind:
        return codegen_return(c, s);
    case Delete_kind:
        VISIT_SEQ(c, expr, s->v.Delete.targets);
        break;
    case Assign_kind:
    {
        Py_ssize_t n = asdl_seq_LEN(s->v.Assign.targets);
        VISIT(c, expr, s->v.Assign.value);
        for (Py_ssize_t i = 0; i < n; i++) {
            if (i < n - 1) {
                RETURN_IF_ERROR(qpc_copy(c, LOC(s), 1));
            }
            VISIT(c, expr,
                  (expr_ty)asdl_seq_GET(s->v.Assign.targets, i));
        }
        break;
    }
    case AugAssign_kind:
        return codegen_augassign(c, s);
    case AnnAssign_kind:
        return codegen_annassign(c, s);
    case For_kind:
        CODEGEN_COND_BLOCK(codegen_for, c, s);
        break;
    case While_kind:
        CODEGEN_COND_BLOCK(codegen_while, c, s);
        break;
    case If_kind:
        CODEGEN_COND_BLOCK(codegen_if, c, s);
        break;
    case Match_kind:
        CODEGEN_COND_BLOCK(codegen_match, c, s);
        break;
    case Raise_kind:
    {
        Py_ssize_t n = 0;
        if (s->v.Raise.exc) {
            VISIT(c, expr, s->v.Raise.exc);
            n++;
            if (s->v.Raise.cause) {
                VISIT(c, expr, s->v.Raise.cause);
                n++;
            }
        }
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(s), QP_OP_raise, (int)n));
        break;
    }
    case Try_kind:
        CODEGEN_COND_BLOCK(codegen_try, c, s);
        break;
    case TryStar_kind:
        CODEGEN_COND_BLOCK(codegen_try_star, c, s);
        break;
    case Assert_kind:
        return codegen_assert(c, s);
    case Import_kind:
        return codegen_import(c, s);
    case ImportFrom_kind:
        return codegen_from_import(c, s);
    case Global_kind:
    case Nonlocal_kind:
        break;
    case Expr_kind:
    {
        return codegen_stmt_expr(c, LOC(s), s->v.Expr.value);
    }
    case Pass_kind:
    {
        /* CPython's NOP: the line of `pass` is reported by sys.settrace */
        return qpc_op(c, LOC(s), OP_nop);
    }
    case Break_kind:
    {
        return codegen_break(c, LOC(s));
    }
    case Continue_kind:
    {
        return codegen_continue(c, LOC(s));
    }
    case With_kind:
        CODEGEN_COND_BLOCK(codegen_with, c, s);
        break;
    case AsyncFunctionDef_kind:
        return codegen_function(c, s, 1);
    case AsyncWith_kind:
        CODEGEN_COND_BLOCK(codegen_async_with, c, s);
        break;
    case AsyncFor_kind:
        CODEGEN_COND_BLOCK(codegen_async_for, c, s);
        break;
    }

    return SUCCESS;
}

/* ---- with ------------------------------------------------------------------- */

/* FINAL handler entry: [exit, self, value_s, exc]
     push_exc_info          [exit, self, value_s, prev, exc]
     catch CLEANUP          [exit, self, value_s, prev, exc_s, M2]
     copy 6, 6, 5, 5        [exit, self, value_s, prev, exc_s, M2, exit, self, prev, exc]
     with_except_start      [..., M2, exit, self, prev, exc, res]
   CLEANUP starts with [exit, self, value_s, prev, exc_s, exc2]. */
static int
codegen_with_handler_start(compiler *c, location loc, jump_target_label cleanup)
{
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_exc_info));
    RETURN_IF_ERROR(qpc_setup(c, loc, cleanup));
    RETURN_IF_ERROR(qpc_copy(c, loc, 6));   /* exit */
    RETURN_IF_ERROR(qpc_copy(c, loc, 6));   /* self */
    RETURN_IF_ERROR(qpc_copy(c, loc, 5));   /* prev */
    RETURN_IF_ERROR(qpc_copy(c, loc, 5));   /* exc */
    return qpc_py(c, loc, QP_OP_with_except_start);
}

/* [exit, self, value_s, prev, exc_s, M2, exit', self', prev', exc', res] */
static int
codegen_with_except_finish(compiler *c, jump_target_label cleanup) {
    NEW_JUMP_TARGET_LABEL(c, suppress);
    RETURN_IF_ERROR(qpc_py_jump(c, NO_LOCATION, QP_OP_jump_if_true, suppress));
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_reraise));

    USE_LABEL(c, suppress);
    /* the four copies of with_except_start's inputs */
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop)); /* exc_value */
    RETURN_IF_ERROR(qpc_py(c, NO_LOCATION, QP_OP_pop_except));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));  /* value_s */
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    NEW_JUMP_TARGET_LABEL(c, exit);
    RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, exit));

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, exit);
    return SUCCESS;
}

/*
   Implements the async with statement.

   The semantics outlined in that PEP are as follows:

   async with EXPR as VAR:
       BLOCK

   It is implemented roughly as:

   context = EXPR
   exit = context.__aexit__  # not calling it
   value = await context.__aenter__()
   try:
       VAR = value  # if VAR present in the syntax
       BLOCK
   finally:
       if an exception was raised:
           exc = copy of (exception, instance, traceback)
       else:
           exc = (None, None, None)
       if not (await exit(*exc)):
           raise
 */
static int
codegen_async_with_inner(compiler *c, stmt_ty s, int pos)
{
    location loc = LOC(s);
    withitem_ty item = asdl_seq_GET(s->v.AsyncWith.items, pos);

    assert(s->kind == AsyncWith_kind);

    NEW_JUMP_TARGET_LABEL(c, block);
    NEW_JUMP_TARGET_LABEL(c, final);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* Evaluate EXPR */
    VISIT(c, expr, item->context_expr);
    loc = LOC(item->context_expr);
    RETURN_IF_ERROR(qpc_copy(c, loc, 1));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_special, QP_SPECIAL_AEXIT));
    RETURN_IF_ERROR(qpc_swap(c, loc, 2));
    RETURN_IF_ERROR(qpc_swap(c, loc, 3));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_special, QP_SPECIAL_AENTER));
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_awaitable, QP_AWAITABLE_AENTER));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    ADD_YIELD_FROM(c, loc, 1);

    /* SETUP_WITH: [exit, self, value] -> [exit, self, value_s, M, value] */
    RETURN_IF_ERROR(qpc_setup(c, loc, final));
    RETURN_IF_ERROR(qpc_copy(c, loc, 2));

    /* SETUP_WITH pushes a finally block. */
    USE_LABEL(c, block);
    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_ASYNC_WITH, block, final, s));

    if (item->optional_vars) {
        VISIT(c, expr, item->optional_vars);
    }
    else {
        /* Discard result from context.__aenter__() */
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    }

    pos++;
    if (pos == asdl_seq_LEN(s->v.AsyncWith.items)) {
        /* BLOCK code */
        VISIT_SEQ(c, stmt, s->v.AsyncWith.body);
    }
    else {
        RETURN_IF_ERROR(codegen_async_with_inner(c, s, pos));
    }

    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_ASYNC_WITH, block);

    /* POP_BLOCK: [exit, self, value_s, M] -> [exit, self] */
    RETURN_IF_ERROR(qpc_pop_block(c, loc));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    /* End of body; start the cleanup */

    /* For successful outcome:
     * call __exit__(None, None, None)
     */
    RETURN_IF_ERROR(codegen_call_exit_with_nones(c, loc));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_awaitable, QP_AWAITABLE_AEXIT));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    ADD_YIELD_FROM(c, loc, 1);

    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));

    RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, exit));

    /* For exceptional outcome: [exit, self, value_s, exc] */
    USE_LABEL(c, final);

    RETURN_IF_ERROR(codegen_with_handler_start(c, loc, cleanup));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_get_awaitable, QP_AWAITABLE_AEXIT));
    RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_push_null));
    ADD_YIELD_FROM(c, loc, 1);
    RETURN_IF_ERROR(codegen_with_except_finish(c, cleanup));

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
codegen_async_with(compiler *c, stmt_ty s)
{
    return codegen_async_with_inner(c, s, 0);
}


/*
   Implements the with statement from PEP 343.
   with EXPR as VAR:
       BLOCK
   is implemented as:
        <code for EXPR>                 [exit, self, value]
        catch E; copy 2                 [exit, self, value_s, M, value]
        <code to store to VAR> or POP_TOP
        <code for BLOCK>
        POP_BLOCK; drop value_s         [exit, self]
        exit(None, None, None); POP_TOP
        JUMP  EXIT
    E:  [exit, self, value_s, exc]  push_exc_info; catch C; copies
        WITH_EXCEPT_START (calls EXPR.__exit__)
        POP_JUMP_IF_TRUE T:
        RERAISE
    T:  pop copies, POP_BLOCK, POP_TOP (exc), POP_EXCEPT, POP_TOP x3
    EXIT:
 */

static int
codegen_with_inner(compiler *c, stmt_ty s, int pos)
{
    withitem_ty item = asdl_seq_GET(s->v.With.items, pos);

    assert(s->kind == With_kind);

    NEW_JUMP_TARGET_LABEL(c, block);
    NEW_JUMP_TARGET_LABEL(c, final);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* Evaluate EXPR */
    VISIT(c, expr, item->context_expr);
    /* Will push bound __exit__ */
    location loc = LOC(item->context_expr);
    RETURN_IF_ERROR(qpc_copy(c, loc, 1));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_special, QP_SPECIAL_EXIT));
    RETURN_IF_ERROR(qpc_swap(c, loc, 2));
    RETURN_IF_ERROR(qpc_swap(c, loc, 3));
    RETURN_IF_ERROR(qpc_py_u8(c, loc, QP_OP_load_special, QP_SPECIAL_ENTER));
    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_call_method, 0));
    /* SETUP_WITH: [exit, self, value] -> [exit, self, value_s, M, value] */
    RETURN_IF_ERROR(qpc_setup(c, loc, final));
    RETURN_IF_ERROR(qpc_copy(c, loc, 2));

    /* SETUP_WITH pushes a finally block. */
    USE_LABEL(c, block);
    RETURN_IF_ERROR(_PyCompile_PushFBlock(c, loc, COMPILE_FBLOCK_WITH, block, final, s));

    if (item->optional_vars) {
        VISIT(c, expr, item->optional_vars);
    }
    else {
    /* Discard result from context.__enter__() */
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    }

    pos++;
    if (pos == asdl_seq_LEN(s->v.With.items)) {
        /* BLOCK code */
        VISIT_SEQ(c, stmt, s->v.With.body);
    }
    else {
        RETURN_IF_ERROR(codegen_with_inner(c, s, pos));
    }

    /* POP_BLOCK: [exit, self, value_s, M] -> [exit, self] */
    RETURN_IF_ERROR(qpc_pop_block(c, NO_LOCATION));
    RETURN_IF_ERROR(qpc_op(c, NO_LOCATION, OP_drop));
    _PyCompile_PopFBlock(c, COMPILE_FBLOCK_WITH, block);

    /* End of body; start the cleanup. */

    /* For successful outcome:
     * call __exit__(None, None, None)
     */
    RETURN_IF_ERROR(codegen_call_exit_with_nones(c, loc));
    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    RETURN_IF_ERROR(qpc_jump(c, loc, OP_goto, exit));

    /* For exceptional outcome: [exit, self, value_s, exc] */
    USE_LABEL(c, final);

    RETURN_IF_ERROR(codegen_with_handler_start(c, loc, cleanup));
    RETURN_IF_ERROR(codegen_with_except_finish(c, cleanup));

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
codegen_with(compiler *c, stmt_ty s)
{
    return codegen_with_inner(c, s, 0);
}

/* ---- augmented / annotated assignment ------------------------------------- */

static int
codegen_augassign(compiler *c, stmt_ty s)
{
    assert(s->kind == AugAssign_kind);
    expr_ty e = s->v.AugAssign.target;

    location loc = LOC(e);

    switch (e->kind) {
    case Attribute_kind:
        VISIT(c, expr, e->v.Attribute.value);
        RETURN_IF_ERROR(qpc_copy(c, loc, 1));
        loc = update_start_location_to_match_attr(c, loc, e);
        RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_getattr, e->v.Attribute.attr, 0));
        break;
    case Subscript_kind:
        VISIT(c, expr, e->v.Subscript.value);
        if (should_apply_two_element_slice_optimization(e->v.Subscript.slice)) {
            RETURN_IF_ERROR(codegen_slice_two_parts(c, e->v.Subscript.slice));
            RETURN_IF_ERROR(qpc_copy(c, loc, 3));
            RETURN_IF_ERROR(qpc_copy(c, loc, 3));
            RETURN_IF_ERROR(qpc_copy(c, loc, 3));
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_getslice));
        }
        else {
            VISIT(c, expr, e->v.Subscript.slice);
            RETURN_IF_ERROR(qpc_copy(c, loc, 2));
            RETURN_IF_ERROR(qpc_copy(c, loc, 2));
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_getitem));
        }
        break;
    case Name_kind:
        RETURN_IF_ERROR(codegen_nameop(c, loc, e->v.Name.id, Load));
        break;
    default:
        return qpc_error(c, loc,
            "invalid node type (%d) for augmented assignment",
            (int)e->kind);
    }

    loc = LOC(s);

    VISIT(c, expr, s->v.AugAssign.value);
    ADDOP_INPLACE(c, loc, s->v.AugAssign.op);

    loc = LOC(e);

    switch (e->kind) {
    case Attribute_kind:
        loc = update_start_location_to_match_attr(c, loc, e);
        RETURN_IF_ERROR(qpc_swap(c, loc, 2));
        RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_setattr, e->v.Attribute.attr, 0));
        break;
    case Subscript_kind:
        if (should_apply_two_element_slice_optimization(e->v.Subscript.slice)) {
            RETURN_IF_ERROR(qpc_swap(c, loc, 4));
            RETURN_IF_ERROR(qpc_swap(c, loc, 3));
            RETURN_IF_ERROR(qpc_swap(c, loc, 2));
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setslice));
        }
        else {
            RETURN_IF_ERROR(qpc_swap(c, loc, 3));
            RETURN_IF_ERROR(qpc_swap(c, loc, 2));
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setitem));
        }
        break;
    case Name_kind:
        return codegen_nameop(c, loc, e->v.Name.id, Store);
    default:
        abort();  /* Py_UNREACHABLE */
    }
    return SUCCESS;
}

static int
codegen_check_ann_expr(compiler *c, expr_ty e)
{
    VISIT(c, expr, e);
    return qpc_op(c, LOC(e), OP_drop);
}

static int
codegen_check_ann_subscr(compiler *c, expr_ty e)
{
    /* We check that everything in a subscript is defined at runtime. */
    switch (e->kind) {
    case Slice_kind:
        if (e->v.Slice.lower && codegen_check_ann_expr(c, e->v.Slice.lower) < 0) {
            return ERROR;
        }
        if (e->v.Slice.upper && codegen_check_ann_expr(c, e->v.Slice.upper) < 0) {
            return ERROR;
        }
        if (e->v.Slice.step && codegen_check_ann_expr(c, e->v.Slice.step) < 0) {
            return ERROR;
        }
        return SUCCESS;
    case Tuple_kind: {
        /* extended slice */
        asdl_expr_seq *elts = e->v.Tuple.elts;
        Py_ssize_t i, n = asdl_seq_LEN(elts);
        for (i = 0; i < n; i++) {
            RETURN_IF_ERROR(codegen_check_ann_subscr(c, asdl_seq_GET(elts, i)));
        }
        return SUCCESS;
    }
    default:
        return codegen_check_ann_expr(c, e);
    }
}

static int
codegen_annassign(compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    expr_ty targ = s->v.AnnAssign.target;
    bool future_annotations = FUTURE_FEATURES(c) & CO_FUTURE_ANNOTATIONS;
    JSAtom mangled;

    assert(s->kind == AnnAssign_kind);

    /* We perform the actual assignment first. */
    if (s->v.AnnAssign.value) {
        VISIT(c, expr, s->v.AnnAssign.value);
        VISIT(c, expr, targ);
    }
    switch (targ->kind) {
    case Name_kind:
        /* If we have a simple name in a module or class, store annotation. */
        if (s->v.AnnAssign.simple &&
            (SCOPE_TYPE(c) == COMPILE_SCOPE_MODULE ||
             SCOPE_TYPE(c) == COMPILE_SCOPE_CLASS)) {
            if (future_annotations) {
                int r;
                VISIT(c, annexpr, s->v.AnnAssign.annotation);
                RETURN_IF_ERROR(qpc_addop_name(c, loc, QP_OP_load_name,
                                               c->ids.__annotations__, 0));
                mangled = _PyCompile_MaybeMangle(c, targ->v.Name.id);
                if (mangled == JS_ATOM_NULL) {
                    return ERROR;
                }
                r = qpc_load_str_atom(c, loc, mangled);
                JS_FreeAtom(c->ctx, mangled);
                RETURN_IF_ERROR(r);
                /* [annotation, __annotations__, name] */
                RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_setitem));
            }
            else {
                int conditional_annotation_index = -1;
                RETURN_IF_ERROR(_PyCompile_AddDeferredAnnotation(
                    c, s, &conditional_annotation_index));
                if (conditional_annotation_index != -1) {
                    /* LOAD_DEREF (class cell) in class scope, LOAD_NAME at
                       module level */
                    if (SCOPE_TYPE(c) == COMPILE_SCOPE_CLASS) {
                        RETURN_IF_ERROR(qpc_deref_op(c, loc,
                                                     c->ids.__conditional_annotations__,
                                                     Load));
                    }
                    else {
                        RETURN_IF_ERROR(qpc_py_atom(c, loc, QP_OP_load_name,
                                                    c->ids.__conditional_annotations__));
                    }
                    RETURN_IF_ERROR(qpc_load_small_int(c, loc, conditional_annotation_index));
                    RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_set_add, 1));
                    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
                }
            }
        }
        break;
    case Attribute_kind:
        if (!s->v.AnnAssign.value &&
            codegen_check_ann_expr(c, targ->v.Attribute.value) < 0) {
            return ERROR;
        }
        break;
    case Subscript_kind:
        if (!s->v.AnnAssign.value &&
            (codegen_check_ann_expr(c, targ->v.Subscript.value) < 0 ||
             codegen_check_ann_subscr(c, targ->v.Subscript.slice) < 0)) {
                return ERROR;
        }
        break;
    default:
        return qpc_error(c, loc,
                         "invalid node type (%d) for annotated assignment",
                         (int)targ->kind);
    }
    return SUCCESS;
}
/* ---------------------------------------------------------------------- */
/*
 * Structural pattern matching (CPython 3.15.2 Python/codegen.c, PSF-2.0).
 * Fragment of python/compile.c (included after the core declarations).
 *
 * No exception markers are pushed here: every COPY/SWAP depth is the
 * CPython one (only enclosing code may hold markers, and pattern code never
 * reaches below the subject).  pc->stores / pc->fail_pop are js_malloc'ed
 * arrays; stored atoms are borrowed from the AST.
 */

#define WILDCARD_CHECK(N) \
    ((N)->kind == MatchAs_kind && !(N)->v.MatchAs.name)

#define WILDCARD_STAR_CHECK(N) \
    ((N)->kind == MatchStar_kind && !(N)->v.MatchStar.name)

// Limit permitted subexpressions, even if the parser & AST validator let them through
/* CPython folds signed and complex number patterns (-1, 1+2j: the only
   UnaryOp/BinOp shapes the parser accepts in patterns) into constants
   first; they are evaluated at run time here (see the header comment). */
#define MATCH_VALUE_EXPR(N) \
    ((N)->kind == Constant_kind || (N)->kind == Attribute_kind || \
     (N)->kind == UnaryOp_kind || (N)->kind == BinOp_kind)

static int codegen_pattern(compiler *c, pattern_ty p, pattern_context *pc);
static int codegen_pattern_subpattern(compiler *c, pattern_ty p, pattern_context *pc);

/* ---- pc->stores (CPython list of names) ---------------------------------- */

static int
qpc_pattern_stores_contains(pattern_context *pc, JSAtom name)
{
    for (Py_ssize_t i = 0; i < pc->n_stores; i++) {
        if (qpc_atom_is(pc->stores[i], name)) {
            return 1;
        }
    }
    return 0;
}

static Py_ssize_t
qpc_pattern_stores_index(pattern_context *pc, JSAtom name)
{
    for (Py_ssize_t i = 0; i < pc->n_stores; i++) {
        if (qpc_atom_is(pc->stores[i], name)) {
            return i;
        }
    }
    return -1;
}

static int
qpc_pattern_stores_append(compiler *c, pattern_context *pc, JSAtom name)
{
    if (pc->n_stores >= pc->stores_size) {
        Py_ssize_t size = pc->stores_size ? pc->stores_size * 2 : 8;
        JSAtom *resized = js_realloc(c->ctx, pc->stores, sizeof(JSAtom) * size);
        if (resized == NULL) {
            return ERROR;
        }
        pc->stores = resized;
        pc->stores_size = size;
    }
    pc->stores[pc->n_stores++] = name;
    return SUCCESS;
}

// Allocate or resize pc->fail_pop to allow for n items to be popped on failure.
static int
ensure_fail_pop(compiler *c, pattern_context *pc, Py_ssize_t n)
{
    Py_ssize_t size = n + 1;
    if (size <= pc->fail_pop_size) {
        return SUCCESS;
    }
    size_t needed = sizeof(jump_target_label) * size;
    jump_target_label *resized = js_realloc(c->ctx, pc->fail_pop, needed);
    if (resized == NULL) {
        return ERROR;
    }
    pc->fail_pop = resized;
    while (pc->fail_pop_size < size) {
        NEW_JUMP_TARGET_LABEL(c, new_block);
        pc->fail_pop[pc->fail_pop_size++] = new_block;
    }
    return SUCCESS;
}

// Use op (OP_if_false / OP_goto) to jump to the correct fail_pop block.
static int
jump_to_fail_pop(compiler *c, location loc,
                 pattern_context *pc, int op)
{
    // Pop any items on the top of the stack, plus any objects we were going to
    // capture on success:
    Py_ssize_t pops = pc->on_top + pc->n_stores;
    RETURN_IF_ERROR(ensure_fail_pop(c, pc, pops));
    RETURN_IF_ERROR(qpc_jump(c, loc, op, pc->fail_pop[pops]));
    return SUCCESS;
}

// Build all of the fail_pop blocks and reset fail_pop.
static int
emit_and_reset_fail_pop(compiler *c, location loc,
                        pattern_context *pc)
{
    if (!pc->fail_pop_size) {
        assert(pc->fail_pop == NULL);
        return SUCCESS;
    }
    while (--pc->fail_pop_size) {
        if (qpc_use_label(c, pc->fail_pop[pc->fail_pop_size]) < 0 ||
            qpc_op(c, loc, OP_drop) < 0) {
            pc->fail_pop_size = 0;
            js_free(c->ctx, pc->fail_pop);
            pc->fail_pop = NULL;
            return ERROR;
        }
    }
    int res = qpc_use_label(c, pc->fail_pop[0]);
    js_free(c->ctx, pc->fail_pop);
    pc->fail_pop = NULL;
    return res;
}

static int
codegen_error_duplicate_store(compiler *c, location loc, identifier n)
{
    char buf[QPC_ATOM_BUF];
    return qpc_error(c, loc,
        "multiple assignments to name '%s' in pattern", qpc_atom_str(c, buf, n));
}

// Duplicate the effect of 3.10's ROT_* instructions using SWAPs.
static int
codegen_pattern_helper_rotate(compiler *c, location loc, Py_ssize_t count)
{
    while (1 < count) {
        RETURN_IF_ERROR(qpc_swap(c, loc, (int)count--));
    }
    return SUCCESS;
}

static int
codegen_pattern_helper_store_name(compiler *c, location loc,
                                  identifier n, pattern_context *pc)
{
    if (n == JS_ATOM_NULL) {
        RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
        return SUCCESS;
    }
    // Can't assign to the same name twice:
    if (qpc_pattern_stores_contains(pc, n)) {
        return codegen_error_duplicate_store(c, loc, n);
    }
    // Rotate this object underneath any items we need to preserve:
    Py_ssize_t rotations = pc->on_top + pc->n_stores + 1;
    RETURN_IF_ERROR(codegen_pattern_helper_rotate(c, loc, rotations));
    RETURN_IF_ERROR(qpc_pattern_stores_append(c, pc, n));
    return SUCCESS;
}


static int
codegen_pattern_unpack_helper(compiler *c, location loc,
                              asdl_pattern_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        pattern_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == MatchStar_kind && !seen_star) {
            if ((i >= (1 << 8)) ||
                (n-i-1 >= (INT_MAX >> 8))) {
                return qpc_error(c, loc,
                    "too many expressions in "
                    "star-unpacking sequence pattern");
            }
            RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_unpack_ex,
                                       (int)(i + ((n-i-1) << 8))));
            seen_star = 1;
        }
        else if (elt->kind == MatchStar_kind) {
            return qpc_error(c, loc,
                "multiple starred expressions in sequence pattern");
        }
    }
    if (!seen_star) {
        RETURN_IF_ERROR(qpc_py_u16(c, loc, QP_OP_unpack_seq, (int)n));
    }
    return SUCCESS;
}

static int
pattern_helper_sequence_unpack(compiler *c, location loc,
                               asdl_pattern_seq *patterns, Py_ssize_t star,
                               pattern_context *pc)
{
    RETURN_IF_ERROR(codegen_pattern_unpack_helper(c, loc, patterns));
    Py_ssize_t size = asdl_seq_LEN(patterns);
    // We've now got a bunch of new subjects on the stack. They need to remain
    // there after each subpattern match:
    pc->on_top += size;
    for (Py_ssize_t i = 0; i < size; i++) {
        // One less item to keep track of each time we loop through:
        pc->on_top--;
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        RETURN_IF_ERROR(codegen_pattern_subpattern(c, pattern, pc));
    }
    return SUCCESS;
}

// Like pattern_helper_sequence_unpack, but uses getitem (BINARY_OP/NB_SUBSCR)
// instead of UNPACK_SEQUENCE / UNPACK_EX. This is more efficient for patterns
// with a starred wildcard like [first, *_] / [first, *_, last] / [*_, last].
static int
pattern_helper_sequence_subscr(compiler *c, location loc,
                               asdl_pattern_seq *patterns, Py_ssize_t star,
                               pattern_context *pc)
{
    // We need to keep the subject around for extracting elements:
    pc->on_top++;
    Py_ssize_t size = asdl_seq_LEN(patterns);
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        if (WILDCARD_CHECK(pattern)) {
            continue;
        }
        if (i == star) {
            assert(WILDCARD_STAR_CHECK(pattern));
            continue;
        }
        RETURN_IF_ERROR(qpc_copy(c, loc, 1));
        if (i < star) {
            RETURN_IF_ERROR(qpc_load_small_int(c, loc, (int32_t)i));
        }
        else {
            // The subject may not support negative indexing! Compute a
            // nonnegative index ([subject, subject, len] after get_len):
            RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_get_len));
            RETURN_IF_ERROR(qpc_load_small_int(c, loc, (int32_t)(size - i)));
            ADDOP_BINARY(c, loc, Sub);
        }
        RETURN_IF_ERROR(qpc_py(c, loc, QP_OP_getitem));
        RETURN_IF_ERROR(codegen_pattern_subpattern(c, pattern, pc));
    }
    // Pop the subject, we're done with it:
    pc->on_top--;
    RETURN_IF_ERROR(qpc_op(c, loc, OP_drop));
    return SUCCESS;
}

// Like codegen_pattern, but turn off checks for irrefutability.
static int
codegen_pattern_subpattern(compiler *c,
                            pattern_ty p, pattern_context *pc)
{
    int allow_irrefutable = pc->allow_irrefutable;
    pc->allow_irrefutable = 1;
    RETURN_IF_ERROR(codegen_pattern(c, p, pc));
    pc->allow_irrefutable = allow_irrefutable;
    return SUCCESS;
}

static int
codegen_pattern_as(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchAs_kind);
    if (p->v.MatchAs.pattern == NULL) {
        // An irrefutable match:
        if (!pc->allow_irrefutable) {
            if (p->v.MatchAs.name) {
                char buf[QPC_ATOM_BUF];
                const char *e = "name capture '%s' makes remaining patterns unreachable";
                return qpc_error(c, LOC(p), e, qpc_atom_str(c, buf, p->v.MatchAs.name));
            }
            const char *e = "wildcard makes remaining patterns unreachable";
            return qpc_error(c, LOC(p), "%s", e);
        }
        return codegen_pattern_helper_store_name(c, LOC(p), p->v.MatchAs.name, pc);
    }
    // Need to make a copy for (possibly) storing later:
    pc->on_top++;
    RETURN_IF_ERROR(qpc_copy(c, LOC(p), 1));
    RETURN_IF_ERROR(codegen_pattern(c, p->v.MatchAs.pattern, pc));
    // Success! Store it:
    pc->on_top--;
    RETURN_IF_ERROR(codegen_pattern_helper_store_name(c, LOC(p), p->v.MatchAs.name, pc));
    return SUCCESS;
}

static int
codegen_pattern_star(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchStar_kind);
    RETURN_IF_ERROR(
        codegen_pattern_helper_store_name(c, LOC(p), p->v.MatchStar.name, pc));
    return SUCCESS;
}

static int
validate_kwd_attrs(compiler *c, asdl_identifier_seq *attrs, asdl_pattern_seq* patterns)
{
    // Any errors will point to the pattern rather than the arg name as the
    // parser is only supplying identifiers rather than Name or keyword nodes
    Py_ssize_t nattrs = asdl_seq_LEN(attrs);
    for (Py_ssize_t i = 0; i < nattrs; i++) {
        identifier attr = ((identifier)asdl_seq_GET(attrs, i));
        for (Py_ssize_t j = i + 1; j < nattrs; j++) {
            identifier other = ((identifier)asdl_seq_GET(attrs, j));
            if (qpc_atom_is(attr, other)) {
                char buf[QPC_ATOM_BUF];
                location loc = LOC((pattern_ty) asdl_seq_GET(patterns, j));
                return qpc_error(c, loc, "attribute name repeated "
                                         "in class pattern: %s",
                                 qpc_atom_str(c, buf, attr));
            }
        }
    }
    return SUCCESS;
}

static int
codegen_pattern_class(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchClass_kind);
    asdl_pattern_seq *patterns = p->v.MatchClass.patterns;
    asdl_identifier_seq *kwd_attrs = p->v.MatchClass.kwd_attrs;
    asdl_pattern_seq *kwd_patterns = p->v.MatchClass.kwd_patterns;
    Py_ssize_t nargs = asdl_seq_LEN(patterns);
    Py_ssize_t nattrs = asdl_seq_LEN(kwd_attrs);
    Py_ssize_t nkwd_patterns = asdl_seq_LEN(kwd_patterns);
    if (nattrs != nkwd_patterns) {
        // AST validator shouldn't let this happen, but if it does,
        // just fail, don't crash out of the interpreter
        const char * e = "kwd_attrs (%d) / kwd_patterns (%d) length mismatch in class pattern";
        return qpc_error(c, LOC(p), e, (int)nattrs, (int)nkwd_patterns);
    }
    // CPython: INT_MAX limit; the match_class / unpack_seq operands are u16
    // here.  (%R of the cls expression is not printable: CPython passes the
    // expr node; the message keeps the text.)
    if (0xffff < nargs || 0xffff < nargs + nattrs) {
        return qpc_error(c, LOC(p), "too many sub-patterns in class pattern");
    }
    if (nattrs) {
        RETURN_IF_ERROR(validate_kwd_attrs(c, kwd_attrs, kwd_patterns));
    }
    VISIT(c, expr, p->v.MatchClass.cls);
    RETURN_IF_ERROR(qpc_load_atom_tuple(c, LOC(p),
                                        nattrs ? kwd_attrs->typed_elements : NULL,
                                        (int)nattrs));
    // [subject, cls, names] -> [attrs or None]
    RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_match_class, (int)nargs));
    RETURN_IF_ERROR(qpc_copy(c, LOC(p), 1));
    RETURN_IF_ERROR(qpc_load_none(c, LOC(p)));
    RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_is_op, 1));
    // TOS is now a tuple of (nargs + nattrs) attributes (or None):
    pc->on_top++;
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_unpack_seq, (int)(nargs + nattrs)));
    pc->on_top += nargs + nattrs - 1;
    for (Py_ssize_t i = 0; i < nargs + nattrs; i++) {
        pc->on_top--;
        pattern_ty pattern;
        if (i < nargs) {
            // Positional:
            pattern = asdl_seq_GET(patterns, i);
        }
        else {
            // Keyword:
            pattern = asdl_seq_GET(kwd_patterns, i - nargs);
        }
        if (WILDCARD_CHECK(pattern)) {
            RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));
            continue;
        }
        RETURN_IF_ERROR(codegen_pattern_subpattern(c, pattern, pc));
    }
    // Success! Pop the tuple of attributes:
    return SUCCESS;
}

/* Numeric view of a constant for duplicate-key detection (Python numeric
   equality: True == 1 == 1.0 == 1+0j).  Returns 0 if k is not a number. */
static int
qpc_pattern_const_numeric(qp_const *k, JSValueConst *re, double *im)
{
    *im = 0.0;
    switch (k->kind) {
    case QP_CONST_TRUE:
        *re = JS_NewInt32(NULL, 1);
        return 1;
    case QP_CONST_FALSE:
        *re = JS_NewInt32(NULL, 0);
        return 1;
    case QP_CONST_INT:
    case QP_CONST_FLOAT:
        *re = k->v;
        return 1;
    case QP_CONST_COMPLEX:
        *re = JS_NewInt32(NULL, 0);
        *im = JS_VALUE_GET_FLOAT64(k->v);
        return 1;
    default:
        return 0;
    }
}

/* Python `a == b` for two AST constants (CPython: membership in the set of
   seen keys).  1 equal, 0 different, -1 error. */
static int
qpc_pattern_const_equal(compiler *c, qp_const *a, qp_const *b)
{
    JSValueConst ra, rb;
    double ia, ib;
    int na, nb;
    if (a->kind == QP_CONST_OBJECT || b->kind == QP_CONST_OBJECT) {
        /* constants of AST objects (e.g. a complex with a real part) */
        JSValue va = qpc_const_value(c, a), vb;
        int r;
        if (JS_IsException(va))
            return -1;
        vb = qpc_const_value(c, b);
        if (JS_IsException(vb)) {
            JS_FreeValue(c->ctx, va);
            return -1;
        }
        r = qp_richcompare_bool(c->ctx, va, vb, QP_CMP_EQ);
        JS_FreeValue(c->ctx, va);
        JS_FreeValue(c->ctx, vb);
        return r;
    }
    na = qpc_pattern_const_numeric(a, &ra, &ia);
    nb = qpc_pattern_const_numeric(b, &rb, &ib);
    if (na || nb) {
        if (!(na && nb) || ia != ib) {
            return 0;
        }
        return JS_IsEqual(c->ctx, ra, rb);
    }
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case QP_CONST_STR:
    case QP_CONST_BYTES:
        return JS_IsEqual(c->ctx, a->v, b->v);
    default:    /* None, Ellipsis */
        return 1;
    }
}

static int
qpc_pattern_error_duplicate_key(compiler *c, location loc, qp_const *k)
{
    JSValue v = qpc_const_value(c, k);
    if (JS_IsException(v)) {
        return ERROR;
    }
    JSValue r = qp_repr(c->ctx, v);
    JS_FreeValue(c->ctx, v);
    if (JS_IsException(r)) {
        return ERROR;
    }
    const char *s = JS_ToCString(c->ctx, r);
    JS_FreeValue(c->ctx, r);
    if (s == NULL) {
        return ERROR;
    }
    qpc_error(c, loc, "mapping pattern checks duplicate key (%s)", s);
    JS_FreeCString(c->ctx, s);
    return ERROR;
}

/* seen: CPython set of Constant keys, here an array of *n_seen constants. */
static int
codegen_pattern_mapping_key(compiler *c, qp_const **seen, Py_ssize_t *n_seen,
                            pattern_ty p, Py_ssize_t i)
{
    asdl_expr_seq *keys = p->v.MatchMapping.keys;
    asdl_pattern_seq *patterns = p->v.MatchMapping.patterns;
    expr_ty key = asdl_seq_GET(keys, i);
    if (key == NULL) {
        const char *e = "can't use NULL keys in MatchMapping "
                        "(set 'rest' parameter instead)";
        location loc = LOC((pattern_ty) asdl_seq_GET(patterns, i));
        return qpc_error(c, loc, "%s", e);
    }

    if (key->kind == Constant_kind) {
        qp_const *k = key->v.Constant.value;
        for (Py_ssize_t j = 0; j < *n_seen; j++) {
            int in_seen = qpc_pattern_const_equal(c, seen[j], k);
            RETURN_IF_ERROR(in_seen);
            if (in_seen) {
                return qpc_pattern_error_duplicate_key(c, LOC(p), k);
            }
        }
        seen[(*n_seen)++] = k;
    }
    else if (!MATCH_VALUE_EXPR(key)) {
        const char *e = "mapping pattern keys may only match literals and attribute lookups";
        return qpc_error(c, LOC(p), "%s", e);
    }
    VISIT(c, expr, key);
    return SUCCESS;
}

static int
codegen_pattern_mapping(compiler *c, pattern_ty p,
                        pattern_context *pc)
{
    assert(p->kind == MatchMapping_kind);
    asdl_expr_seq *keys = p->v.MatchMapping.keys;
    asdl_pattern_seq *patterns = p->v.MatchMapping.patterns;
    Py_ssize_t size = asdl_seq_LEN(keys);
    Py_ssize_t npatterns = asdl_seq_LEN(patterns);
    if (size != npatterns) {
        // AST validator shouldn't let this happen, but if it does,
        // just fail, don't crash out of the interpreter
        const char * e = "keys (%d) / patterns (%d) length mismatch in mapping pattern";
        return qpc_error(c, LOC(p), e, (int)size, (int)npatterns);
    }
    // We have a double-star target if "rest" is set
    JSAtom star_target = p->v.MatchMapping.rest;
    // We need to keep the subject on top during the mapping and length checks:
    pc->on_top++;
    RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_match_mapping));
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    if (!size && !star_target) {
        // If the pattern is just "{}", we're done! Pop the subject:
        pc->on_top--;
        RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));
        return SUCCESS;
    }
    if (size) {
        // If the pattern has any keys in it, perform a length check:
        RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_get_len));
        RETURN_IF_ERROR(qpc_load_small_int(c, LOC(p), (int32_t)size));
        RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_compare, QP_CMP_GE | QP_CMP_TO_BOOL));
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    }
    // CPython: INT_MAX; build_tuple / unpack_seq operands are u16 here.
    if (0xffff < size) {
        return qpc_error(c, LOC(p), "too many sub-patterns in mapping pattern");
    }
    // Collect all of the keys into a tuple for MATCH_KEYS and
    // **rest. They can either be dotted names or literals:

    // Maintaining a set of Constant_kind kind keys allows us to raise a
    // SyntaxError in the case of duplicates.
    if (size) {
        qp_const **seen = js_malloc(c->ctx, sizeof(qp_const *) * size);
        Py_ssize_t n_seen = 0;
        if (seen == NULL) {
            return ERROR;
        }
        for (Py_ssize_t i = 0; i < size; i++) {
            if (codegen_pattern_mapping_key(c, seen, &n_seen, p, i) < 0) {
                js_free(c->ctx, seen);
                return ERROR;
            }
        }
        js_free(c->ctx, seen);
    }

    // all keys have been checked; there are no duplicates

    RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_build_tuple, (int)size));
    RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_match_keys));
    // There's now a tuple of keys and a tuple of values on top of the subject:
    pc->on_top += 2;
    RETURN_IF_ERROR(qpc_copy(c, LOC(p), 1));
    RETURN_IF_ERROR(qpc_load_none(c, LOC(p)));
    RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_is_op, 1));
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    // So far so good. Use that tuple of values on the stack to match
    // sub-patterns against:
    RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_unpack_seq, (int)size));
    pc->on_top += size - 1;
    for (Py_ssize_t i = 0; i < size; i++) {
        pc->on_top--;
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        RETURN_IF_ERROR(codegen_pattern_subpattern(c, pattern, pc));
    }
    // If we get this far, it's a match! Whatever happens next should consume
    // the tuple of keys and the subject:
    pc->on_top -= 2;
    if (star_target) {
        // If we have a starred name, bind a dict of remaining items to it (this may
        // seem a bit inefficient, but keys is rarely big enough to actually impact
        // runtime):
        // rest = dict(TOS1)
        // for key in TOS:
        //     del rest[key]
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_build_map, 0));      // [subject, keys, empty]
        RETURN_IF_ERROR(qpc_swap(c, LOC(p), 3));                          // [empty, keys, subject]
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_dict_update, 2));    // [copy, keys]
        RETURN_IF_ERROR(qpc_py_u16(c, LOC(p), QP_OP_unpack_seq, (int)size)); // [copy, keys...]
        while (size) {
            RETURN_IF_ERROR(qpc_copy(c, LOC(p), (int)(1 + size--)));      // [copy, keys..., copy]
            RETURN_IF_ERROR(qpc_swap(c, LOC(p), 2));                      // [copy, keys..., copy, key]
            RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_delitem));            // [copy, keys...]
        }
        RETURN_IF_ERROR(codegen_pattern_helper_store_name(c, LOC(p), star_target, pc));
    }
    else {
        RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));  // Tuple of keys.
        RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));  // Subject.
    }
    return SUCCESS;
}

static int
codegen_pattern_or(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchOr_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    Py_ssize_t size = asdl_seq_LEN(p->v.MatchOr.patterns);
    assert(size > 1);
    // We're going to be messing with pc. Keep the original info handy:
    pattern_context old_pc = *pc;
    // control is the list of names bound by the first alternative. It is used
    // for checking different name bindings in alternatives, and for correcting
    // the order in which extracted elements are placed on the stack.
    JSAtom *control = NULL;
    Py_ssize_t ncontrol = 0;
    // NOTE: We can't use returning macros anymore! goto error on error.
    // On error, *pc is restored to old_pc so that the caller still owns (and
    // frees) the outer stores / fail_pop; the alternative's arrays are freed
    // here.
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty alt = asdl_seq_GET(p->v.MatchOr.patterns, i);
        pc->stores = NULL;
        pc->n_stores = 0;
        pc->stores_size = 0;
        // An irrefutable sub-pattern must be last, if it is allowed at all:
        pc->allow_irrefutable = (i == size - 1) && old_pc.allow_irrefutable;
        pc->fail_pop = NULL;
        pc->fail_pop_size = 0;
        pc->on_top = 0;
        if (qpc_copy(c, LOC(alt), 1) < 0 ||
            codegen_pattern(c, alt, pc) < 0) {
            goto error;
        }
        // Success!
        Py_ssize_t nstores = pc->n_stores;
        if (!i) {
            // This is the first alternative, so save its stores as a "control"
            // for the others (they can't bind a different set of names, and
            // might need to be reordered):
            assert(control == NULL);
            control = js_malloc(c->ctx, sizeof(JSAtom) * (nstores ? nstores : 1));
            if (control == NULL) {
                goto error;
            }
            if (nstores) {
                memcpy(control, pc->stores, sizeof(JSAtom) * nstores);
            }
            ncontrol = nstores;
        }
        else if (nstores != ncontrol) {
            goto diff;
        }
        else if (nstores) {
            // There were captures. Check to see if we differ from control:
            Py_ssize_t icontrol = nstores;
            while (icontrol--) {
                JSAtom name = control[icontrol];
                Py_ssize_t istores = qpc_pattern_stores_index(pc, name);
                if (istores < 0) {
                    goto diff;
                }
                if (icontrol != istores) {
                    // Reorder the names on the stack to match the order of the
                    // names in control. There's probably a better way of doing
                    // this; the current solution is potentially very
                    // inefficient when each alternative subpattern binds lots
                    // of names in different orders. It's fine for reasonable
                    // cases, though.
                    assert(istores < icontrol);
                    Py_ssize_t rotations = istores + 1;
                    // Perform the same rotation on pc->stores:
                    // rotated = pc_stores[:rotations]
                    // del pc_stores[:rotations]
                    // pc_stores[icontrol-istores:icontrol-istores] = rotated
                    // i.e. rotate pc_stores[0:icontrol+1] left by `rotations`
                    for (Py_ssize_t r = 0; r < rotations; r++) {
                        JSAtom first = pc->stores[0];
                        memmove(pc->stores, pc->stores + 1, sizeof(JSAtom) * icontrol);
                        pc->stores[icontrol] = first;
                    }
                    // Do the same thing to the stack, using several
                    // rotations:
                    while (rotations--) {
                        if (codegen_pattern_helper_rotate(c, LOC(alt), icontrol + 1) < 0) {
                            goto error;
                        }
                    }
                }
            }
        }
        assert(control);
        if (qpc_jump(c, LOC(alt), OP_goto, end) < 0 ||
            emit_and_reset_fail_pop(c, LOC(alt), pc) < 0)
        {
            goto error;
        }
        js_free(c->ctx, pc->stores);
        pc->stores = NULL;
    }
    *pc = old_pc;
    // No match. Pop the remaining copy of the subject and fail:
    if (qpc_op(c, LOC(p), OP_drop) < 0 ||
        jump_to_fail_pop(c, LOC(p), pc, OP_goto) < 0) {
        goto error_restored;
    }

    if (qpc_use_label(c, end) < 0) {
        goto error_restored;
    }
    Py_ssize_t nstores = ncontrol;
    // There's a bunch of stuff on the stack between where the new stores
    // are and where they need to be:
    // - The other stores.
    // - A copy of the subject.
    // - Anything else that may be on top of the stack.
    // - Any previous stores we've already stashed away on the stack.
    Py_ssize_t nrots = nstores + 1 + pc->on_top + pc->n_stores;
    for (Py_ssize_t i = 0; i < nstores; i++) {
        // Rotate this capture to its proper place on the stack:
        if (codegen_pattern_helper_rotate(c, LOC(p), nrots) < 0) {
            goto error_restored;
        }
        // Update the list of previous stores with this new name, checking for
        // duplicates:
        JSAtom name = control[i];
        if (qpc_pattern_stores_contains(pc, name)) {
            codegen_error_duplicate_store(c, LOC(p), name);
            goto error_restored;
        }
        if (qpc_pattern_stores_append(c, pc, name) < 0) {
            goto error_restored;
        }
    }
    js_free(c->ctx, control);
    // NOTE: Returning macros are safe again.
    // Pop the copy of the subject:
    RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));
    return SUCCESS;
diff:
    qpc_error(c, LOC(p), "alternative patterns bind different names");
error:
    js_free(c->ctx, pc->stores);
    js_free(c->ctx, pc->fail_pop);
    *pc = old_pc;
error_restored:
    js_free(c->ctx, control);
    return ERROR;
}


static int
codegen_pattern_sequence(compiler *c, pattern_ty p,
                         pattern_context *pc)
{
    assert(p->kind == MatchSequence_kind);
    asdl_pattern_seq *patterns = p->v.MatchSequence.patterns;
    Py_ssize_t size = asdl_seq_LEN(patterns);
    Py_ssize_t star = -1;
    int only_wildcard = 1;
    int star_wildcard = 0;
    // Find a starred name, if it exists. There may be at most one:
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        if (pattern->kind == MatchStar_kind) {
            if (star >= 0) {
                const char *e = "multiple starred names in sequence pattern";
                return qpc_error(c, LOC(p), "%s", e);
            }
            star_wildcard = WILDCARD_STAR_CHECK(pattern);
            only_wildcard &= star_wildcard;
            star = i;
            continue;
        }
        only_wildcard &= WILDCARD_CHECK(pattern);
    }
    // We need to keep the subject on top during the sequence and length checks:
    pc->on_top++;
    RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_match_sequence));
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    if (star < 0) {
        // No star: len(subject) == size
        RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_get_len));
        RETURN_IF_ERROR(qpc_load_small_int(c, LOC(p), (int32_t)size));
        RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_compare, QP_CMP_EQ | QP_CMP_TO_BOOL));
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    }
    else if (size > 1) {
        // Star: len(subject) >= size - 1
        RETURN_IF_ERROR(qpc_py(c, LOC(p), QP_OP_get_len));
        RETURN_IF_ERROR(qpc_load_small_int(c, LOC(p), (int32_t)(size - 1)));
        RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_compare, QP_CMP_GE | QP_CMP_TO_BOOL));
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    }
    // Whatever comes next should consume the subject:
    pc->on_top--;
    if (only_wildcard) {
        // Patterns like: [] / [_] / [_, _] / [*_] / [_, *_] / [_, _, *_] / etc.
        RETURN_IF_ERROR(qpc_op(c, LOC(p), OP_drop));
    }
    else if (star_wildcard) {
        RETURN_IF_ERROR(pattern_helper_sequence_subscr(c, LOC(p), patterns, star, pc));
    }
    else {
        RETURN_IF_ERROR(pattern_helper_sequence_unpack(c, LOC(p), patterns, star, pc));
    }
    return SUCCESS;
}

static int
codegen_pattern_value(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchValue_kind);
    expr_ty value = p->v.MatchValue.value;
    if (!MATCH_VALUE_EXPR(value)) {
        const char *e = "patterns may only match literals and attribute lookups";
        return qpc_error(c, LOC(p), "%s", e);
    }
    VISIT(c, expr, value);
    // COMPARE_OP Eq + TO_BOOL fused into the to-bool flag
    RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_compare, QP_CMP_EQ | QP_CMP_TO_BOOL));
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    return SUCCESS;
}

static int
codegen_pattern_singleton(compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchSingleton_kind);
    RETURN_IF_ERROR(qpc_load_const(c, LOC(p), p->v.MatchSingleton.value));
    RETURN_IF_ERROR(qpc_py_u8(c, LOC(p), QP_OP_is_op, 0));
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, OP_if_false));
    return SUCCESS;
}

static int
codegen_pattern(compiler *c, pattern_ty p, pattern_context *pc)
{
    switch (p->kind) {
        case MatchValue_kind:
            return codegen_pattern_value(c, p, pc);
        case MatchSingleton_kind:
            return codegen_pattern_singleton(c, p, pc);
        case MatchSequence_kind:
            return codegen_pattern_sequence(c, p, pc);
        case MatchMapping_kind:
            return codegen_pattern_mapping(c, p, pc);
        case MatchClass_kind:
            return codegen_pattern_class(c, p, pc);
        case MatchStar_kind:
            return codegen_pattern_star(c, p, pc);
        case MatchAs_kind:
            return codegen_pattern_as(c, p, pc);
        case MatchOr_kind:
            return codegen_pattern_or(c, p, pc);
    }
    // AST validator shouldn't let this happen, but if it does,
    // just fail, don't crash out of the interpreter
    const char *e = "invalid match pattern node in AST (kind=%d)";
    return qpc_error(c, LOC(p), e, (int)p->kind);
}

static int
codegen_match_inner(compiler *c, stmt_ty s, pattern_context *pc)
{
    VISIT(c, expr, s->v.Match.subject);
    NEW_JUMP_TARGET_LABEL(c, end);
    Py_ssize_t cases = asdl_seq_LEN(s->v.Match.cases);
    assert(cases > 0);
    match_case_ty m = asdl_seq_GET(s->v.Match.cases, cases - 1);
    int has_default = WILDCARD_CHECK(m->pattern) && 1 < cases;
    for (Py_ssize_t i = 0; i < cases - has_default; i++) {
        m = asdl_seq_GET(s->v.Match.cases, i);
        // Only copy the subject if we're *not* on the last case:
        if (i != cases - has_default - 1) {
            RETURN_IF_ERROR(qpc_copy(c, LOC(m->pattern), 1));
        }
        pc->stores = NULL;
        pc->n_stores = 0;
        pc->stores_size = 0;
        // Irrefutable cases must be either guarded, last, or both:
        pc->allow_irrefutable = m->guard != NULL || i == cases - 1;
        pc->fail_pop = NULL;
        pc->fail_pop_size = 0;
        pc->on_top = 0;
        // pc->stores is freed by codegen_match on error.
        RETURN_IF_ERROR(codegen_pattern(c, m->pattern, pc));
        assert(!pc->on_top);
        // It's a match! Store all of the captured names (they're on the stack).
        Py_ssize_t nstores = pc->n_stores;
        for (Py_ssize_t n = 0; n < nstores; n++) {
            JSAtom name = pc->stores[n];
            RETURN_IF_ERROR(codegen_nameop(c, LOC(m->pattern), name, Store));
        }
        js_free(c->ctx, pc->stores);
        pc->stores = NULL;
        pc->n_stores = pc->stores_size = 0;
        if (m->guard) {
            RETURN_IF_ERROR(ensure_fail_pop(c, pc, 0));
            RETURN_IF_ERROR(codegen_jump_if(c, LOC(m->pattern), m->guard, pc->fail_pop[0], 0));
        }
        // Success! Pop the subject off, we're done with it:
        if (i != cases - has_default - 1) {
            /* Use the next location to give better locations for branch events */
            RETURN_IF_ERROR(qpc_op(c, NEXT_LOCATION, OP_drop));
        }
        VISIT_SEQ(c, stmt, m->body);
        RETURN_IF_ERROR(qpc_jump(c, NO_LOCATION, OP_goto, end));
        // If the pattern fails to match, we want the line number of the
        // cleanup to be associated with the failed pattern, not the last line
        // of the body
        RETURN_IF_ERROR(emit_and_reset_fail_pop(c, LOC(m->pattern), pc));
    }
    if (has_default) {
        // A trailing "case _" is common, and lets us save a bit of redundant
        // pushing and popping in the loop above:
        m = asdl_seq_GET(s->v.Match.cases, cases - 1);
        if (cases == 1) {
            // No matches. Done with the subject:
            RETURN_IF_ERROR(qpc_op(c, LOC(m->pattern), OP_drop));
        }
        else {
            // Show line coverage for default case (it doesn't create bytecode)
            RETURN_IF_ERROR(qpc_op(c, LOC(m->pattern), OP_nop));
        }
        if (m->guard) {
            RETURN_IF_ERROR(codegen_jump_if(c, LOC(m->pattern), m->guard, end, 0));
        }
        VISIT_SEQ(c, stmt, m->body);
    }
    USE_LABEL(c, end);
    return SUCCESS;
}

static int
codegen_match(compiler *c, stmt_ty s)
{
    pattern_context pc;
    pc.stores = NULL;
    pc.n_stores = pc.stores_size = 0;
    pc.fail_pop = NULL;
    pc.fail_pop_size = 0;
    int result = codegen_match_inner(c, s, &pc);
    js_free(c->ctx, pc.stores);
    js_free(c->ctx, pc.fail_pop);
    return result;
}

#undef WILDCARD_CHECK
#undef WILDCARD_STAR_CHECK
#undef MATCH_VALUE_EXPR

/* ======================================================================== */
/* Module compilation and entry points                                      */
/* ======================================================================== */

static location start_location(asdl_stmt_seq *stmts)
{
    if (asdl_seq_LEN(stmts) > 0) {
        /* Set current line number to the line number of first statement. */
        stmt_ty st = (stmt_ty)asdl_seq_GET(stmts, 0);
        return SRC_LOCATION_FROM_AST(st);
    }
    return LOCATION(1, 1, 0, 0);
}

/* compiler_codegen + compiler_mod: returns the finished root fd */
static JSFunctionDef *compiler_codegen(compiler *c, mod_ty mod)
{
    compiler_unit *u;
    location loc = NO_LOCATION;
    int addNone = mod->kind != Expression_kind;

    if (codegen_enter_scope(c, c->ids.anon_module, COMPILE_SCOPE_MODULE,
                            mod, 1, JS_ATOM_NULL, NULL) < 0)
        return NULL;
    assert(c->u->u_scope_type == COMPILE_SCOPE_MODULE);
    switch (mod->kind) {
    case Module_kind: {
        asdl_stmt_seq *stmts = mod->v.Module.body;
        loc = start_location(stmts);
        if (_PyCodegen_Module(c, loc, stmts, false) < 0)
            return NULL;
        break;
    }
    case Interactive_kind: {
        asdl_stmt_seq *stmts = mod->v.Interactive.body;
        c->c_interactive = 1;
        loc = start_location(stmts);
        if (_PyCodegen_Module(c, loc, stmts, true) < 0)
            return NULL;
        break;
    }
    case Expression_kind:
        if (codegen_visit_expr(c, mod->v.Expression.body) < 0)
            return NULL;
        break;
    default:
        qpc_internal_error(c, "this compiler does not handle FunctionTypes");
        return NULL;
    }
    if (_PyCodegen_AddReturnAtEnd(c, addNone) < 0)
        return NULL;

    /* ANNOTATIONS_PLACEHOLDER: the module starts with a jump here */
    u = c->u;
    if (qpc_use_label(c, u->u_ann_label) < 0)
        return NULL;
    if (u->u_module_annotations_pending) {
        if (qpc_emit_deferred_annotations(c, loc) < 0)
            return NULL;
    }
    if (qpc_jump(c, NO_LOCATION, OP_goto, u->u_ann_body_label) < 0)
        return NULL;
    return qpc_finish_unit(c, -1);
}

static JSValue qp_compile(JSContext *ctx, mod_ty mod, struct symtable *st,
                          _PyFutureFeatures *future, const char *filename,
                          int mode, int flags, int optimize, qp_error *err)
{
    compiler cs, *c = &cs;
    JSFunctionDef *fd;
    JSValue ret = JS_EXCEPTION;

    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    c->c_filename = filename;
    c->c_st = st;
    c->c_future = *future;
    c->c_flags = future->ff_features | flags;
    c->c_optimize = optimize;
    c->c_err = err;
    c->c_lc_cache.buf_start = qpc_dummy_source;
    c->c_lc_cache.ptr = qpc_dummy_source;
    c->c_lc_cache.line_num = 1;
    c->c_lc_cache.col_num = 0;

    if (mode == QP_INPUT_FUNC_TYPE) {
        qp_error_set(err, QP_ERR_VALUE, 0, 0, 0, 0,
                     "compile() mode 'func_type' requires flag PyCF_ONLY_AST");
        return JS_EXCEPTION;
    }
    if (qpc_ids_init(c) < 0)
        goto done;
    fd = compiler_codegen(c, mod);
    if (fd) {
        /* leave the module unit; the fd tree is consumed below */
        _PyCompile_ExitScope(c);
        c->c_root_fd = NULL;
        ret = qp_create_function(ctx, fd);
    }
 done:
    /* error: release the units and the whole function tree */
    while (c->u)
        _PyCompile_ExitScope(c);
    if (c->c_root_fd)
        js_free_function_def(ctx, c->c_root_fd);
    qpc_ids_free(c);
    return ret;
}

static JSValue qp_compile_ast(JSContext *ctx, mod_ty mod, const char *filename,
                              JSValueConst module, int mode, int flags, int optimize,
                              qp_arena *arena, qp_error *err)
{
    _PyFutureFeatures future;
    struct symtable *st;
    JSValue ret;

    if (optimize == -1)
        optimize = QP_STATE(ctx)->config.optimize;
    if (!_PyFuture_FromAST(mod, filename, &future, arena, err))
        return JS_EXCEPTION;
    /* compiler_setup(): merge the __future__ features with the flags */
    future.ff_features |= flags;
    /* _PyAST_Preprocess (enable_warnings = 1) */
    if (qp_preprocess_warnings(ctx, mod, filename, module, err) < 0)
        return JS_EXCEPTION;
    st = _PySymtable_Build(mod, filename, &future, arena, err);
    if (!st)
        return JS_EXCEPTION;
    if (qp_validate(st, mod, future.ff_features, filename, module, err) < 0) {
        _PySymtable_Free(st);
        return JS_EXCEPTION;
    }
    ret = qp_compile(ctx, mod, st, &future, filename, mode, flags, optimize, err);
    _PySymtable_Free(st);
    return ret;
}

/* compile(source, filename, mode, flags) without the AST-only path.
   src[len] must be '\0'.  With err == NULL a compile error is raised as a
   Python exception (qp_raise_qp_error). */
static JSValue qp_compile_source(JSContext *ctx, const char *src, size_t len,
                                 const char *filename, JSValueConst module, int mode,
                                 int flags, int optimize, qp_error *err)
{
    qp_error local_err;
    qp_arena *arena;
    mod_ty mod;
    JSValue ret = JS_EXCEPTION;
    qp_error *e = err;

    if (!e) {
        memset(&local_err, 0, sizeof(local_err));
        e = &local_err;
    }
    if (mode == QP_INPUT_FUNC_TYPE) {
        qp_error_set(e, QP_ERR_VALUE, 0, 0, 0, 0,
                     "compile() mode 'func_type' requires flag PyCF_ONLY_AST");
        goto done;
    }
    if (memchr(src, '\0', len) != NULL) {
        qp_error_set(e, QP_ERR_SYNTAX, 0, 0, 0, 0,
                     "source code string cannot contain null bytes");
        goto done;
    }
    arena = qp_arena_new(ctx, e);
    if (!arena) {
        qp_error_set(e, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
        goto done;
    }
    mod = qp_parse_string(ctx, arena, src, len, filename, module, mode, flags, e);
    if (mod)
        ret = qp_compile_ast(ctx, mod, filename, module, mode, flags, optimize, arena, e);
    qp_arena_free(arena);
 done:
    if (!err) {
        if (JS_IsException(ret) && e->kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, e, filename);
        qp_error_clear(&local_err);
    }
    return ret;
}

#undef location
