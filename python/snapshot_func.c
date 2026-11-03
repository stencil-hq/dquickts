/*
 * dquickts Python heap snapshot: function companion, bound method, cell,
 * code and frame objects (part `func`, included by python/snapshot.c).
 *
 * Classes: PY_FUNC_DATA (QPFuncDataPriv, funcobject.c), PY_METHOD
 * (QPMethodObject), PY_CELL (QPCell), PY_CODE (QPCodeObject, codeobject.c),
 * PY_FRAME (QPFrameObject, frameobject.c).
 *
 * Every payload starts with a presence byte: the finalizers accept a NULL
 * u.opaque, so that state round-trips too.  Then exactly the fields the
 * class finalizer frees / gc_mark marks, plus the plain scalar fields of
 * the struct.
 *
 * Nothing here is tied to a running frame:
 * - frame objects (PY_FRAME) are value snapshots made from a JSStackFrame
 *   (function, pc, f_back frame object, f_locals dict) and never point at
 *   the JSStackFrame or a JSAsyncFunctionState, so a frame object whose
 *   function is still executing is saved like any other (f_lineno/f_lasti
 *   are those recorded at creation, as before the snapshot);
 * - a cell's JSVarRef may still be attached to a live or suspended frame:
 *   the var ref cell itself is written by the engine (SNAP_CELL_VAR_REF),
 *   which owns the decision for attached references.
 * The QPCodeInfo of a code object is written by python/snapshot.c with its
 * bytecode cell; a code object only references the bytecode.
 */

/* ---- deferred validation ------------------------------------------------ */

/* A code object must wrap Python bytecode (qp_code_new); the bytecode body
   and its QPCodeInfo are only complete once every cell is read. */
static int qp_snap_func_check_code(JSContext *ctx, JSObject *p)
{
    QPCodeObject *c = p->u.opaque;
    JSFunctionBytecode *b;

    if (!c)
        return 0;
    b = JS_VALUE_GET_PTR(c->bytecode);
    if (!b->is_py || !b->py_code) {
        JS_ThrowInternalError(ctx, "snapshot: code object over non-Python bytecode");
        return -1;
    }
    if (!QP_IS_NULL(c->fields) &&
        (!qp_tuple_check(ctx, c->fields) || QP_TUPLE_SIZE(c->fields) != QP_CF__COUNT)) {
        JS_ThrowInternalError(ctx, "snapshot: invalid code object fields");
        return -1;
    }
    return 0;
}

/* f_lineno / f_code / f_globals dereference the function's QPFuncData and
   bytecode (qp_frame_new requires a Python function); f_back is a frame
   or None. */
static int qp_snap_func_check_frame(JSContext *ctx, JSObject *p)
{
    QPFrameObject *f = p->u.opaque;
    JSFunctionBytecode *b;

    if (!f)
        return 0;
    if (!qp_function_data(f->func))
        goto fail;
    b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
    if (!b->is_py || !b->py_code || f->pc > (uint32_t)b->byte_code_len)
        goto fail;
    if (!JS_IsUndefined(f->back) && !qp_fr_data(f->back))
        goto fail;
    return 0;
 fail:
    JS_ThrowInternalError(ctx, "snapshot: invalid frame object");
    return -1;
}

/* ---- writer ------------------------------------------------------------- */

static int qp_snap_func_write_funcdata(SnapWriter *w, QPFuncDataPriv *pd)
{
    QPFuncData *d = &pd->d;
    int i;

    if (snap_put_value(w, d->globals) ||
        snap_put_value(w, d->builtins) ||
        snap_put_value(w, d->name) ||
        snap_put_value(w, d->qualname) ||
        snap_put_value(w, d->module) ||
        snap_put_value(w, d->doc) ||
        snap_put_value(w, d->defaults) ||
        snap_put_value(w, d->kwdefaults) ||
        snap_put_value(w, d->annotations) ||
        snap_put_value(w, d->annotate) ||
        snap_put_value(w, d->type_params) ||
        snap_put_value(w, d->dict) ||
        /* code_obj is not a pure cache: it carries replace() overrides
           and `f.__code__ is f.__code__` identity */
        snap_put_value(w, d->code_obj))
        return -1;
    bc_put_leb128(&w->bc, d->version);
    /* bytecodes retired by `f.__code__ = ...` (always FUNCTION_BYTECODE) */
    bc_put_leb128(&w->bc, pd->retired_count);
    for (i = 0; i < pd->retired_count; i++) {
        if (snap_put_ref(w, SNAP_CELL_BYTECODE, JS_VALUE_GET_PTR(pd->retired[i])))
            return -1;
    }
    return 0;
}

static int qp_snap_write_func(SnapWriter *w, JSObject *p)
{
    void *opaque;

    switch (p->class_id) {
    case JS_CLASS_PY_FUNC_DATA:
    case JS_CLASS_PY_METHOD:
    case JS_CLASS_PY_CELL:
    case JS_CLASS_PY_CODE:
    case JS_CLASS_PY_FRAME:
        break;
    default:
        return 1;
    }
    opaque = p->u.opaque;
    bc_put_u8(&w->bc, opaque != NULL);
    if (!opaque)
        return 0;
    switch (p->class_id) {
    case JS_CLASS_PY_FUNC_DATA:
        return qp_snap_func_write_funcdata(w, opaque);
    case JS_CLASS_PY_METHOD:
        {
            QPMethodObject *m = opaque;
            if (snap_put_value(w, m->func) ||
                snap_put_value(w, m->self))
                return -1;
        }
        return 0;
    case JS_CLASS_PY_CELL:
        {
            QPCell *c = opaque;
            /* the reference counted by the cell; NULL never happens
               (qp_cell_new) and is rejected by the reader */
            return snap_put_ref(w, SNAP_CELL_VAR_REF, c->var_ref);
        }
    case JS_CLASS_PY_CODE:
        {
            QPCodeObject *c = opaque;
            if (snap_put_ref(w, SNAP_CELL_BYTECODE, JS_VALUE_GET_PTR(c->bytecode)) ||
                snap_put_value(w, c->name) ||
                snap_put_value(w, c->qualname) ||
                snap_put_value(w, c->filename))
                return -1;
            bc_put_sleb128(&w->bc, c->firstlineno);
            bc_put_u8(&w->bc, c->has_firstlineno != 0);
            bc_put_leb128(&w->bc, c->flags_add);
            if (snap_put_value(w, c->fields))
                return -1;
        }
        return 0;
    default: /* JS_CLASS_PY_FRAME */
        {
            QPFrameObject *f = opaque;
            if (snap_put_value(w, f->func))
                return -1;
            bc_put_leb128(&w->bc, f->pc);
            if (snap_put_value(w, f->back) ||
                snap_put_value(w, f->locals) ||
                snap_put_value(w, f->trace))
                return -1;
            bc_put_u8(&w->bc, f->trace_lines);
            bc_put_u8(&w->bc, f->trace_opcodes);
        }
        return 0;
    }
}

/* ---- reader ------------------------------------------------------------- */

static int qp_snap_func_read_funcdata(SnapReader *r, JSObject *p)
{
    QPFuncDataPriv *pd;
    QPFuncData *d;
    uint32_t n;
    void *ptr;

    pd = js_mallocz(r->ctx, sizeof(*pd));
    if (!pd)
        return -1;
    d = &pd->d;
    d->globals = JS_UNDEFINED;
    d->builtins = JS_UNDEFINED;
    d->name = JS_UNDEFINED;
    d->qualname = JS_UNDEFINED;
    d->module = JS_UNDEFINED;
    d->doc = JS_UNDEFINED;
    d->defaults = JS_UNDEFINED;
    d->kwdefaults = JS_UNDEFINED;
    d->annotations = JS_UNDEFINED;
    d->annotate = JS_UNDEFINED;
    d->type_params = JS_UNDEFINED;
    d->dict = QP_NULL;
    d->code_obj = QP_NULL;
    p->u.opaque = pd;
    snap_payload_ready(r);
    if (snap_get_value(r, &d->globals) ||
        snap_get_value(r, &d->builtins) ||
        snap_get_value(r, &d->name) ||
        snap_get_value(r, &d->qualname) ||
        snap_get_value(r, &d->module) ||
        snap_get_value(r, &d->doc) ||
        snap_get_value(r, &d->defaults) ||
        snap_get_value(r, &d->kwdefaults) ||
        snap_get_value(r, &d->annotations) ||
        snap_get_value(r, &d->annotate) ||
        snap_get_value(r, &d->type_params) ||
        snap_get_value(r, &d->dict) ||
        snap_get_value(r, &d->code_obj) ||
        bc_get_leb128(&r->bc, &d->version) ||
        bc_get_leb128(&r->bc, &n))
        return -1;
    if (!QP_IS_NULL(d->code_obj) &&
        (JS_VALUE_GET_TAG(d->code_obj) != JS_TAG_OBJECT ||
         JS_VALUE_GET_OBJ(d->code_obj)->class_id != JS_CLASS_PY_CODE))
        return snap_error(r, "invalid function code object");
    /* every retired entry takes at least one input byte */
    if (n > INT32_MAX / sizeof(JSValue) || n > (size_t)(r->bc.buf_end - r->bc.ptr))
        return snap_error(r, "invalid retired bytecode count");
    if (n == 0)
        return 0;
    pd->retired = js_malloc(r->ctx, sizeof(JSValue) * n);
    if (!pd->retired)
        return -1;
    pd->retired_size = n;
    while (pd->retired_count < (int)n) {
        if (snap_get_ref(r, SNAP_CELL_BYTECODE, &ptr))
            return -1;
        if (!ptr)
            return snap_error(r, "invalid retired bytecode");
        pd->retired[pd->retired_count++] =
            JS_DupValue(r->ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, ptr));
    }
    return 0;
}

static int qp_snap_read_func(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    uint8_t present;

    switch (p->class_id) {
    case JS_CLASS_PY_FUNC_DATA:
    case JS_CLASS_PY_METHOD:
    case JS_CLASS_PY_CELL:
    case JS_CLASS_PY_CODE:
    case JS_CLASS_PY_FRAME:
        break;
    default:
        return 1;
    }
    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (present > 1)
        return snap_error(r, "invalid payload flag");
    if (!present) {
        snap_payload_ready(r); /* the finalizers accept a NULL payload */
        return 0;
    }
    switch (p->class_id) {
    case JS_CLASS_PY_FUNC_DATA:
        return qp_snap_func_read_funcdata(r, p);
    case JS_CLASS_PY_METHOD:
        {
            QPMethodObject *m = js_mallocz(ctx, sizeof(*m));
            if (!m)
                return -1;
            m->func = JS_UNDEFINED;
            m->self = JS_UNDEFINED;
            p->u.opaque = m;
            snap_payload_ready(r);
            if (snap_get_value(r, &m->func) ||
                snap_get_value(r, &m->self))
                return -1;
            if (QP_IS_NULL(m->self)) /* qp_method_new rejects NULL self */
                return snap_error(r, "invalid bound method");
        }
        return 0;
    case JS_CLASS_PY_CELL:
        {
            JSVarRef *var_ref;
            QPCell *c;
            /* read the reference before allocating: the finalizer calls
               free_var_ref(), which accepts NULL, but the cell accessors
               do not */
            if (snap_get_ref(r, SNAP_CELL_VAR_REF, (void **)&var_ref))
                return -1;
            if (!var_ref)
                return snap_error(r, "invalid cell");
            c = js_mallocz(ctx, sizeof(*c));
            if (!c)
                return -1;
            js_rc(var_ref)->ref_count++; /* the cell's own reference */
            c->var_ref = var_ref;
            p->u.opaque = c;
            snap_payload_ready(r);
        }
        return 0;
    case JS_CLASS_PY_CODE:
        {
            QPCodeObject *c;
            void *ptr;
            uint8_t has_firstlineno;
            if (snap_get_ref(r, SNAP_CELL_BYTECODE, &ptr))
                return -1;
            if (!ptr)
                return snap_error(r, "invalid code bytecode");
            c = js_mallocz(ctx, sizeof(*c));
            if (!c)
                return -1;
            c->bytecode = JS_DupValue(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, ptr));
            c->name = QP_NULL;
            c->qualname = QP_NULL;
            c->filename = QP_NULL;
            c->linetable = QP_NULL;     /* replace() overrides: not saved */
            c->exctable = QP_NULL;
            c->fields = QP_NULL;
            p->u.opaque = c;
            snap_payload_ready(r);
            if (snap_get_value(r, &c->name) ||
                snap_get_value(r, &c->qualname) ||
                snap_get_value(r, &c->filename) ||
                bc_get_sleb128(&r->bc, &c->firstlineno) ||
                bc_get_u8(&r->bc, &has_firstlineno) ||
                bc_get_leb128(&r->bc, &c->flags_add) ||
                snap_get_value(r, &c->fields))
                return -1;
            if (has_firstlineno > 1)
                return snap_error(r, "invalid code object");
            c->has_firstlineno = has_firstlineno;
            return qp_snap_defer(r, p, qp_snap_func_check_code);
        }
    default: /* JS_CLASS_PY_FRAME */
        {
            QPFrameObject *f = js_mallocz(ctx, sizeof(*f));
            if (!f)
                return -1;
            f->func = JS_UNDEFINED;
            f->back = JS_UNDEFINED;
            f->locals = QP_NULL;
            f->trace = JS_UNDEFINED;
            p->u.opaque = f;
            snap_payload_ready(r);
            if (snap_get_value(r, &f->func) ||
                bc_get_leb128(&r->bc, &f->pc) ||
                snap_get_value(r, &f->back) ||
                snap_get_value(r, &f->locals) ||
                snap_get_value(r, &f->trace) ||
                bc_get_u8(&r->bc, &f->trace_lines) ||
                bc_get_u8(&r->bc, &f->trace_opcodes))
                return -1;
            if (f->trace_lines > 1 || f->trace_opcodes > 1)
                return snap_error(r, "invalid frame object");
            return qp_snap_defer(r, p, qp_snap_func_check_frame);
        }
    }
}
