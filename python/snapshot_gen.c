/*
 * dquickts Python heap snapshot, part `gen` (included by python/snapshot.c):
 * payloads of python/genobject.c.
 *
 *   JS_CLASS_PY_GENERATOR / PY_COROUTINE / PY_ASYNC_GEN   QPGen
 *   JS_CLASS_PY_GEN_AUX                                   QPGenAux
 *     (coroutine_wrapper, async_generator_asend, async_generator_athrow,
 *      async_generator_wrapped_value)
 *
 * A generator owns a QuickJS heap frame (JSAsyncFunctionState); the engine
 * serialises suspended frames as SNAP_CELL_FRAME cells (snap_write_frame:
 * pc, value stack incl. the yield-from delegate at cur_sp[-4] and the
 * sent-value slot cur_sp[-1], locals, closure var refs), so a suspended
 * generator resumes after restore exactly where it yielded.  A frame that
 * was never started (state CREATED, lazy start) is also a plain suspended
 * frame positioned at its first instruction.
 *
 * Payload layout (QPGen):
 *   u8 kind, u8 state, u8 started, u8 ag_closed, u8 ag_hooks_inited,
 *   u8 ag_running_async, ref FRAME s (NULL once finished), value func,
 *   value name, value qualname, value exc_state.exc_value,
 *   value ag_finalizer.
 * exc_state.previous is not written: it only links the item into
 * QPState.exc_info while the frame executes and is NULL otherwise.
 *
 * Payload layout (QPGenAux):
 *   u8 kind, u8 state, value gen, value v1, value v2, value v3.
 */

static int qp_snap_gen_write_gen(SnapWriter *w, JSObject *p)
{
    QPGen *g = p->u.opaque;

    if (!g)
        return qp_snap_reject(w, p, "uninitialised generator");
    /* A generator that is executing (gi_running) has its frame on the
       machine stack or in a suspended execution, and its exc_state is
       linked into the thread's exception-info chain (QPState.exc_info,
       exc_state.previous): neither can be described by the heap image. */
    if (g->state == QP_GEN_EXECUTING || g->exc_state.previous)
        return qp_snap_reject(w, p, "generator is running");
    bc_put_u8(&w->bc, g->kind);
    bc_put_u8(&w->bc, g->state);
    bc_put_u8(&w->bc, g->started);
    bc_put_u8(&w->bc, g->ag_closed);
    bc_put_u8(&w->bc, g->ag_hooks_inited);
    bc_put_u8(&w->bc, g->ag_running_async);
    if (snap_put_ref(w, SNAP_CELL_FRAME, g->s) ||
        snap_put_value(w, g->func) ||
        snap_put_value(w, g->name) ||
        snap_put_value(w, g->qualname) ||
        snap_put_value(w, g->exc_state.exc_value) ||
        snap_put_value(w, g->ag_finalizer))
        return -1;
    return 0;
}

static int qp_snap_gen_read_gen(SnapReader *r, JSObject *p)
{
    QPGen *g;
    JSAsyncFunctionState *s;
    uint8_t kind, state, started, ag_closed, ag_hooks_inited, ag_running_async;
    int expect_kind;

    if (bc_get_u8(&r->bc, &kind) ||
        bc_get_u8(&r->bc, &state) ||
        bc_get_u8(&r->bc, &started) ||
        bc_get_u8(&r->bc, &ag_closed) ||
        bc_get_u8(&r->bc, &ag_hooks_inited) ||
        bc_get_u8(&r->bc, &ag_running_async))
        return -1;
    switch (p->class_id) {
    case JS_CLASS_PY_COROUTINE: expect_kind = QP_GENK_CORO; break;
    case JS_CLASS_PY_ASYNC_GEN: expect_kind = QP_GENK_AGEN; break;
    default: expect_kind = QP_GENK_GEN; break;
    }
    if (kind != expect_kind)
        return snap_error(r, "invalid generator kind");
    /* EXECUTING is never written; a CREATED frame was never resumed and a
       SUSPENDED one was */
    if (state > QP_GEN_CLEARED || state == QP_GEN_EXECUTING ||
        started > 1 || ag_closed > 1 || ag_hooks_inited > 1 || ag_running_async > 1 ||
        (state == QP_GEN_CREATED && started) ||
        (QP_GEN_SUSPENDED_ANY(state) && !started))
        return snap_error(r, "invalid generator state");

    g = js_mallocz(r->ctx, sizeof(*g));
    if (!g)
        return -1;
    g->s = NULL;
    g->func = JS_UNDEFINED;
    g->name = JS_UNDEFINED;
    g->qualname = JS_UNDEFINED;
    g->exc_state.exc_value = JS_UNDEFINED;
    g->exc_state.previous = NULL;
    g->ag_finalizer = QP_NULL;
    g->kind = kind;
    g->state = state;
    g->started = started;
    g->ag_closed = ag_closed;
    g->ag_hooks_inited = ag_hooks_inited;
    g->ag_running_async = ag_running_async;
    p->u.opaque = g;
    snap_payload_ready(r);

    if (snap_get_ref(r, SNAP_CELL_FRAME, (void **)&s))
        return -1;
    /* the frame exists exactly until the generator is finished (cleared) */
    if (!s != QP_GEN_FINISHED(state))
        return snap_error(r, "invalid generator frame");
    if (s) {
        js_rc(s)->ref_count++;
        g->s = s;
    }
    if (snap_get_value(r, &g->func) ||
        snap_get_value(r, &g->name) ||
        snap_get_value(r, &g->qualname) ||
        snap_get_value(r, &g->exc_state.exc_value) ||
        snap_get_value(r, &g->ag_finalizer))
        return -1;
    /* gi_code / co_flags dereference func's bytecode */
    if (JS_VALUE_GET_TAG(g->func) != JS_TAG_OBJECT ||
        !snap_is_bytecode_function(JS_VALUE_GET_OBJ(g->func)->class_id))
        return snap_error(r, "invalid generator function");
    return 0;
}

static int qp_snap_gen_write_aux(SnapWriter *w, JSObject *p)
{
    QPGenAux *a = p->u.opaque;

    if (!a)
        return qp_snap_reject(w, p, "uninitialised generator helper");
    bc_put_u8(&w->bc, a->kind);
    bc_put_u8(&w->bc, a->state);
    if (snap_put_value(w, a->gen) ||
        snap_put_value(w, a->v1) ||
        snap_put_value(w, a->v2) ||
        snap_put_value(w, a->v3))
        return -1;
    return 0;
}

static int qp_snap_gen_read_aux(SnapReader *r, JSObject *p)
{
    QPGenAux *a;
    uint8_t kind, state;
    int gen_class;

    if (bc_get_u8(&r->bc, &kind) ||
        bc_get_u8(&r->bc, &state))
        return -1;
    if (kind > QP_AUX_WRAPPED || state > QP_AWAITABLE_STATE_CLOSED)
        return snap_error(r, "invalid generator helper");

    a = js_mallocz(r->ctx, sizeof(*a));
    if (!a)
        return -1;
    a->kind = kind;
    a->state = state;
    a->gen = JS_UNDEFINED;
    a->v1 = JS_UNDEFINED;
    a->v2 = JS_UNDEFINED;
    a->v3 = JS_UNDEFINED;
    p->u.opaque = a;
    snap_payload_ready(r);

    if (snap_get_value(r, &a->gen) ||
        snap_get_value(r, &a->v1) ||
        snap_get_value(r, &a->v2) ||
        snap_get_value(r, &a->v3))
        return -1;
    /* the methods use qp__gen(a->gen) without checks: only the class id of
       the referenced object is inspected here (its body may not be read
       yet) */
    switch (kind) {
    case QP_AUX_CORO_WRAPPER: gen_class = JS_CLASS_PY_COROUTINE; break;
    case QP_AUX_ASEND:
    case QP_AUX_ATHROW: gen_class = JS_CLASS_PY_ASYNC_GEN; break;
    default: gen_class = -1; break;
    }
    if (gen_class >= 0 ? qp__class_id(a->gen) != gen_class : !QP_IS_NULL(a->gen))
        return snap_error(r, "invalid generator helper");
    return 0;
}

static int qp_snap_write_gen(SnapWriter *w, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_GENERATOR:
    case JS_CLASS_PY_COROUTINE:
    case JS_CLASS_PY_ASYNC_GEN:
        return qp_snap_gen_write_gen(w, p);
    case JS_CLASS_PY_GEN_AUX:
        return qp_snap_gen_write_aux(w, p);
    default:
        return 1;
    }
}

static int qp_snap_read_gen(SnapReader *r, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_GENERATOR:
    case JS_CLASS_PY_COROUTINE:
    case JS_CLASS_PY_ASYNC_GEN:
        return qp_snap_gen_read_gen(r, p);
    case JS_CLASS_PY_GEN_AUX:
        return qp_snap_gen_read_aux(r, p);
    default:
        return 1;
    }
}
