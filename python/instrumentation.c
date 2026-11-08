/*
 * sys.monitoring (PEP 669; CPython 3.15 Python/instrumentation.c): the
 * tool-id registry, per-tool callbacks, global and per-code event sets,
 * the events namespace, and the delivery of the events by the interpreter
 * hooks of quickjs.c.
 *
 * State: QPModState (modstate.h) holds the tool names, the callbacks
 * (mon_callbacks[tool * QP_MON_EVENTS + event]), the global event sets
 * (mon_events[tool]) and the DISABLE / MISSING sentinels.  QPState
 * (object.h) holds mon_active (some event set is non-empty: the interpreter
 * calls the qp_hook_* functions below), the versions that invalidate the
 * per-code data, and the pending builtin calls.  Each code object that
 * runs while a tool monitors it has a QPMonCode (QPCodeInfo.mon): its
 * local event sets, the active tools per event (global | local) and, per
 * event with a location, the tools that returned DISABLE at each byte pc
 * (CPython's per-instruction tool arrays).
 *
 * The interpreter hooks cost nothing while no tool has events: the
 * dispatch table is switched to the trace hook by qp_trace_dispatch() (the
 * hook of sys.settrace) only while mon_active.  Events are reported for
 * the QuickJS instructions of Python frames with the offsets of the
 * synthesized co_code (qp_code_pc2lasti and co): PY_START / PY_RESUME /
 * PY_THROW when a frame starts or resumes, LINE / INSTRUCTION / CALL /
 * JUMP / PY_RETURN / PY_YIELD before an instruction, BRANCH_LEFT /
 * BRANCH_RIGHT and C_RETURN before the next instruction of the frame (the
 * direction of a branch and the completion of a builtin call are known
 * then), RAISE / RERAISE / C_RAISE / EXCEPTION_HANDLED / PY_UNWIND on the
 * exception path, STOP_ITERATION at the end of `yield from` / `await` of a
 * generator.  A LINE event is reported at an instruction whose line differs
 * from the line of the instruction executed before it in the frame (CPython
 * instruments the line starts, jump targets and handlers and compares with
 * the previous instruction).  Callbacks run with ctx->py->tracing set: no
 * events (monitoring or legacy) while one runs.  Code of QP_CO_NATIVE
 * functions (Python implementations of C modules) reports no events; calls
 * to it are builtin calls (C_RETURN / C_RAISE).
 *
 * Deviations: an INSTRUCTION event is one QuickJS instruction (a group of
 * CPython instructions); `code` is the code object of the running function
 * (func.__code__).
 */

#define QP_MON_TOOLS             6   /* PY_MONITORING_TOOL_IDS: 0..5 usable, 6/7 sys.settrace/setprofile */
#define QP_MON_EVENTS            19  /* _PY_MONITORING_EVENTS */
#define QP_MON_UNGROUPED_EVENTS  16  /* _PY_MONITORING_UNGROUPED_EVENTS */
#define QP_MON_LOC_EVENTS        11  /* PY_MONITORING_IS_INSTRUMENTED_EVENT: events with a location */

#define QP_MON_EV_PY_START          0
#define QP_MON_EV_PY_RESUME         1
#define QP_MON_EV_PY_RETURN         2
#define QP_MON_EV_PY_YIELD          3
#define QP_MON_EV_CALL              4
#define QP_MON_EV_LINE              5
#define QP_MON_EV_INSTRUCTION       6
#define QP_MON_EV_JUMP              7
#define QP_MON_EV_BRANCH_LEFT       8
#define QP_MON_EV_BRANCH_RIGHT      9
#define QP_MON_EV_STOP_ITERATION    10
#define QP_MON_EV_RAISE             11
#define QP_MON_EV_EXCEPTION_HANDLED 12
#define QP_MON_EV_PY_UNWIND         13
#define QP_MON_EV_PY_THROW          14
#define QP_MON_EV_RERAISE           15
#define QP_MON_EV_C_RETURN          16
#define QP_MON_EV_C_RAISE           17
#define QP_MON_EV_BRANCH            18

#define QP_MON_C_RETURN_EVENTS   ((1 << QP_MON_EV_C_RETURN) | (1 << QP_MON_EV_C_RAISE))
#define QP_MON_C_CALL_EVENTS     (QP_MON_C_RETURN_EVENTS | (1 << QP_MON_EV_CALL))

/* JSStackFrame.py_mon_flags: events of the previous instruction (at
   py_mon_prev - 1) reported before the next one */
#define QP_MON_SF_BRANCH  1     /* a conditional branch: BRANCH_LEFT / BRANCH_RIGHT */
#define QP_MON_SF_CCALL   2     /* a builtin call: C_RETURN (QPState.mon_ccalls entry) */
/* an exception raised by a send subop: the cleanup_throw handler that
   catches it is not there in CPython (SEND is outside the try of
   CLEANUP_THROW), its EXCEPTION_HANDLED and RERAISE are not reported;
   kept until the next exception of the frame */
#define QP_MON_SF_SEND_RAISE 4
/* resumed (after a yield, also by throw()) and no instruction reported
   yet: CPython's previous instruction is the RESUME, the next line start
   / jump target / handler reports its LINE even on the same line */
#define QP_MON_SF_RESUMED    8
/* (QP_MON_SF_SUPER_GENERIC 16: object.h) */
/* called by QP_CO_NATIVE code (directly or through builtins and frames
   with this flag): runs as C code, no events (qp_trace_is_native) */
#define QP_MON_SF_NATIVE     32
#define QP_MON_SF_KEEP       (QP_MON_SF_SEND_RAISE | QP_MON_SF_RESUMED | QP_MON_SF_NATIVE)

static const char *const qp_mon_event_names[QP_MON_EVENTS] = {
    "PY_START", "PY_RESUME", "PY_RETURN", "PY_YIELD", "CALL", "LINE",
    "INSTRUCTION", "JUMP", "BRANCH_LEFT", "BRANCH_RIGHT", "STOP_ITERATION",
    "RAISE", "EXCEPTION_HANDLED", "PY_UNWIND", "PY_THROW", "RERAISE",
    "C_RETURN", "C_RAISE", "BRANCH",
};

typedef struct QPMonCode {
    struct list_head link;      /* in QPState.mon_codes while some local[] != 0 */
    uint32_t version;           /* QPState.mon_version of active[] */
    uint32_t restart_version;   /* QPState.mon_restart_version of dis[] */
    uint32_t len;               /* byte_code_len */
    BOOL native;                /* QP_CO_NATIVE code: no events */
    BOOL any;                   /* some active[] != 0 */
    uint8_t local[QP_MON_UNGROUPED_EVENTS];     /* tools with the event in their local set */
    uint8_t active[QP_MON_UNGROUPED_EVENTS];    /* tools with the event (global or local) */
    uint8_t *dis[QP_MON_LOC_EVENTS];            /* [byte pc]: tools that disabled the event there */
    uint8_t *flow;              /* [byte pc] QP_MON_FLOW_* (qp_mon_flow), NULL until needed */
    uint32_t *afor;             /* async for loops: (end_send pc, end_async_for pc) pairs */
    int afor_count;
} QPMonCode;

/* builtin call waiting for its C_RETURN / C_RAISE */
typedef struct QPMonCCall {
    JSStackFrame *sf;
    uint32_t pc;                /* byte pc of the call instruction */
    JSValue callable, arg0;
} QPMonCCall;

/* ---- state ---------------------------------------------------------------- */

static void qp_mon_state_init(QPState *s)
{
    init_list_head(&s->mon_codes);
    s->mon_active = 0;
    s->mon_version = 1;
    s->mon_restart_version = 1;
    s->mon_ccalls = NULL;
    s->mon_ccall_count = s->mon_ccall_size = 0;
    s->mon_event_sf = NULL;
}

static void qp_mon_ccalls_clear(JSRuntime *rt, QPState *s, int from)
{
    while (s->mon_ccall_count > from) {
        QPMonCCall *e = &s->mon_ccalls[--s->mon_ccall_count];
        JS_FreeValueRT(rt, e->callable);
        JS_FreeValueRT(rt, e->arg0);
    }
}

static void qp_mon_state_free(JSRuntime *rt, QPState *s)
{
    struct list_head *el, *el1;

    if (s->mon_active) {
        s->mon_active = 0;
        qp_trace_dispatch(0);
    }
    qp_mon_ccalls_clear(rt, s, 0);
    js_free_rt(rt, s->mon_ccalls);
    s->mon_ccalls = NULL;
    s->mon_ccall_size = 0;
    /* the code objects may outlive the state: unlink them */
    list_for_each_safe(el, el1, &s->mon_codes) {
        list_del(el);
        init_list_head(el);
    }
}

static void qp_mon_state_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func)
{
    int i;

    for (i = 0; i < s->mon_ccall_count; i++) {
        JS_MarkValue(rt, s->mon_ccalls[i].callable, mark_func);
        JS_MarkValue(rt, s->mon_ccalls[i].arg0, mark_func);
    }
}

static void qp_mon_code_free(JSRuntime *rt, QPMonCode *m)
{
    int e;

    if (!m)
        return;
    list_del(&m->link);
    for (e = 0; e < QP_MON_LOC_EVENTS; e++)
        js_free_rt(rt, m->dis[e]);
    js_free_rt(rt, m->flow);
    js_free_rt(rt, m->afor);
    js_free_rt(rt, m);
}

/* Global tools of each event (from the QPModState event sets). */
static void qp_mon_global_tools(QPModState *ms, uint8_t *tools)
{
    int e, t;

    memset(tools, 0, QP_MON_UNGROUPED_EVENTS);
    if (!ms)
        return;
    for (t = 0; t < QP_MON_TOOLS; t++) {
        uint32_t ev = ms->mon_events[t];
        for (e = 0; ev && e < QP_MON_UNGROUPED_EVENTS; e++) {
            if (ev & (1u << e))
                tools[e] |= 1 << t;
        }
    }
}

/* The event sets changed: invalidate the per-code data, switch the
   interpreter hooks on or off. */
static int qp_mon_changed(JSContext *ctx)
{
    QPState *qs = ctx->py;
    QPModState *ms = qs->mod_state;
    BOOL active = !list_empty(&qs->mon_codes);
    int t;

    qs->mon_version++;
    for (t = 0; !active && ms && t < QP_MON_TOOLS; t++)
        active = ms->mon_events[t] != 0;
    if (active == qs->mon_active)
        return 0;
    if (active) {
        JSStackFrame *sf;
        if (qp_trace_dispatch_export(ctx) < 0)
            return -1;
        /* the running Python frames continue after their current (call)
           instruction (instrument_all_executing_code_objects) */
        for (sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
            JSObject *p = qp_fr_sf_func(sf);
            if (!p || !sf->cur_pc)
                continue;
            sf->py_mon_flags = qp_trace_is_native(sf->prev_frame) ? QP_MON_SF_NATIVE : 0;
            sf->py_mon_prev = sf->cur_pc > p->u.func.function_bytecode->byte_code_buf ?
                (uint32_t)(sf->cur_pc - p->u.func.function_bytecode->byte_code_buf) : 0;
        }
    } else {
        qp_mon_ccalls_clear(ctx->rt, qs, 0);
    }
    qs->mon_active = active;
    qp_trace_dispatch(active);
    return 0;
}

/* Monitoring data of b, created if needed; NULL with MemoryError. */
static QPMonCode *qp_mon_code_new(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    QPMonCode *m = ci->mon;

    if (m)
        return m;
    m = js_mallocz(ctx, sizeof(*m));
    if (!m)
        return NULL;
    init_list_head(&m->link);
    m->len = b->byte_code_len;
    m->native = (ci->co_flags & QP_CO_NATIVE) != 0;
    ci->mon = m;
    return m;
}

/* Monitoring data of b with active[] current; NULL with MemoryError. */
static QPMonCode *qp_mon_code(JSContext *ctx, JSFunctionBytecode *b)
{
    QPState *qs = ctx->py;
    QPMonCode *m = b->py_code->mon;
    uint8_t g[QP_MON_UNGROUPED_EVENTS];
    int e, any = 0;

    if (likely(m && m->version == qs->mon_version))
        return m;
    m = qp_mon_code_new(ctx, b);
    if (!m)
        return NULL;
    if (m->restart_version != qs->mon_restart_version) {
        /* restart_events(): the DISABLEd locations are monitored again */
        for (e = 0; e < QP_MON_LOC_EVENTS; e++) {
            js_free(ctx, m->dis[e]);
            m->dis[e] = NULL;
        }
        m->restart_version = qs->mon_restart_version;
    }
    qp_mon_global_tools(qs->mod_state, g);
    for (e = 0; e < QP_MON_UNGROUPED_EVENTS; e++) {
        int tools = m->native ? 0 : g[e] | m->local[e];
        int added = tools & ~m->active[e];
        if (added && e < QP_MON_LOC_EVENTS && m->dis[e]) {
            /* a tool that starts monitoring the event sees every location */
            uint32_t i;
            for (i = 0; i < m->len; i++)
                m->dis[e][i] &= ~added;
        }
        m->active[e] = tools;
        any |= tools;
    }
    m->any = any != 0;
    m->version = qs->mon_version;
    return m;
}

/* Tools of event e (< QP_MON_LOC_EVENTS) at byte pc. */
static inline int qp_mon_tools_at(QPMonCode *m, int e, uint32_t pc)
{
    int tools = m->active[e];

    if (tools && m->dis[e] && pc < m->len)
        tools &= ~m->dis[e][pc];
    return tools;
}

static void qp_mon_disable_at(JSContext *ctx, QPMonCode *m, int e, uint32_t pc, int tool)
{
    if (pc >= m->len)
        return;
    if (!m->dis[e]) {
        m->dis[e] = js_mallocz(ctx, m->len);
        if (!m->dis[e]) {
            JS_FreeValue(ctx, JS_GetException(ctx));   /* best effort (PEP 669) */
            return;
        }
    }
    m->dis[e][pc] |= 1 << tool;
}

/* ---- event delivery ------------------------------------------------------- */

/* One event: the callbacks of tools (highest tool first) are called with
   args[0..nargs) = (code, offset or line, ...).  pc is the location of
   location events (DISABLE removes the tool there).  -1 if a callback
   raised. */
static int qp_mon_fire(JSContext *ctx, QPMonCode *m, int event, uint32_t pc,
                       int tools, JSValueConst *args, int nargs)
{
    QPState *qs = ctx->py;
    QPModState *ms = qs->mod_state;
    int t;

    if (!ms)
        return 0;
    for (t = QP_MON_TOOLS - 1; t >= 0; t--) {
        JSValue cb, res;
        if (!(tools & (1 << t)))
            continue;
        cb = ms->mon_callbacks[t * QP_MON_EVENTS + event];
        if (QP_IS_NULL(cb))
            continue;
        cb = JS_DupValue(ctx, cb);
        qs->tracing++;
        res = qp_vectorcall(ctx, cb, args, nargs, QP_NULL);
        qs->tracing--;
        JS_FreeValue(ctx, cb);
        if (JS_IsException(res))
            return -1;
        if (JS_VALUE_GET_TAG(res) == JS_TAG_OBJECT && !QP_IS_NULL(ms->mon_disable) &&
            JS_VALUE_GET_OBJ(res) == JS_VALUE_GET_OBJ(ms->mon_disable)) {
            JS_FreeValue(ctx, res);
            if (event < QP_MON_LOC_EVENTS) {
                qp_mon_disable_at(ctx, m, event, pc, t);
                if ((event == QP_MON_EV_BRANCH_LEFT || event == QP_MON_EV_BRANCH_RIGHT) &&
                    (ms->mon_branch_legacy[t] & (1 << (event - QP_MON_EV_BRANCH_LEFT))))
                    /* a BRANCH callback: both directions (branch_handler_vectorcall) */
                    qp_mon_disable_at(ctx, m, QP_MON_EV_BRANCH_LEFT + QP_MON_EV_BRANCH_RIGHT - event,
                                      pc, t);
            } else if (event < QP_MON_UNGROUPED_EVENTS) {
                /* other event: removed from the local set of this code */
                m->local[event] &= ~(1 << t);
                {
                    uint8_t g[QP_MON_UNGROUPED_EVENTS];
                    qp_mon_global_tools(ms, g);
                    m->active[event] = g[event] | m->local[event];
                }
            } else {
                JS_FreeValue(ctx, ms->mon_callbacks[t * QP_MON_EVENTS + event]);
                ms->mon_callbacks[t * QP_MON_EVENTS + event] = QP_NULL;
                qp_raise_value_error(ctx, "Cannot disable %s events. Callback removed.",
                                     qp_mon_event_names[event]);
                return -1;
            }
        } else {
            JS_FreeValue(ctx, res);
        }
    }
    return 0;
}

/* qp_mon_fire with the pending exception kept (or replaced by the one of
   a callback, then -1): CPython call_instrumentation_vector_protected. */
static int qp_mon_fire_protected(JSContext *ctx, QPMonCode *m, int event, uint32_t pc,
                                 int tools, JSValueConst *args, int nargs)
{
    JSValue exc = JS_GetException(ctx);

    if (qp_mon_fire(ctx, m, event, pc, tools, args, nargs) < 0) {
        JS_FreeValue(ctx, exc);
        return -1;
    }
    JS_Throw(ctx, exc);
    return 0;
}

static inline JSFunctionBytecode *qp_mon_sf_bytecode(JSStackFrame *sf)
{
    return JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
}

/* args[0] = the code object of sf, args[1] = Python offset of byte pc */
static int qp_mon_args(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b, int offset,
                       JSValue *args)
{
    args[0] = qp_fo_get_code(ctx, sf->cur_func);
    if (JS_IsException(args[0]))
        return -1;
    args[1] = JS_NewInt32(ctx, offset);
    return 0;
}

static int qp_mon_offset(JSFunctionBytecode *b, uint32_t pc)
{
    int off = qp_code_pc2lasti(b, pc);
    return off < 0 ? 0 : off;
}

/* Fire event (with a location at byte pc) with args (code, offset, extra...). */
static int qp_mon_event(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b, QPMonCode *m,
                        int event, uint32_t pc, int offset, int tools, JSValueConst *extra,
                        int nextra)
{
    JSValue args[4];
    int i, r;

    if (sf->py_mon_flags & QP_MON_SF_NATIVE)
        return 0;

    if (qp_mon_args(ctx, sf, b, offset, args) < 0)
        return -1;
    for (i = 0; i < nextra; i++)
        args[2 + i] = (JSValue)extra[i];
    r = qp_mon_fire(ctx, m, event, pc, tools, (JSValueConst *)args, 2 + nextra);
    JS_FreeValue(ctx, args[0]);
    return r;
}

/* -1 if a callback raised (its exception replaced the pending one) */
static int qp_mon_event_protected(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                                  QPMonCode *m, int event, uint32_t pc, int offset, int tools,
                                  int nextra, JSValueConst *extra)
{
    JSValue args[4];
    int i, r;

    if (sf->py_mon_flags & QP_MON_SF_NATIVE)
        return 0;

    args[0] = qp_fo_get_code(ctx, sf->cur_func);
    if (JS_IsException(args[0]))
        return -1;  /* the new exception replaced the pending one */
    args[1] = JS_NewInt32(ctx, offset);
    for (i = 0; i < nextra; i++)
        args[2 + i] = (JSValue)extra[i];
    r = qp_mon_fire_protected(ctx, m, event, pc, tools, (JSValueConst *)args, 2 + nextra);
    JS_FreeValue(ctx, args[0]);
    return r;
}

/* Python function (no C_RETURN / C_RAISE events for a call to it) */
static BOOL qp_mon_is_py_function(JSValueConst v)
{
    JSFunctionBytecode *fb;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_BYTECODE_FUNCTION)
        return FALSE;
    fb = JS_VALUE_GET_OBJ(v)->u.func.function_bytecode;
    return fb && fb->is_py && fb->py_code && !(fb->py_code->co_flags & QP_CO_NATIVE);
}

/* The entry of sf in the pending builtin calls (stale entries above it
   are dropped); -1 if none. */
static int qp_mon_ccall_find(JSContext *ctx, JSStackFrame *sf)
{
    QPState *qs = ctx->py;
    int i;

    for (i = qs->mon_ccall_count - 1; i >= 0; i--) {
        if (qs->mon_ccalls[i].sf == sf) {
            qp_mon_ccalls_clear(ctx->rt, qs, i + 1);
            return i;
        }
    }
    return -1;
}

/* C_RETURN (is_raise = 0) or C_RAISE of the builtin call of sf. */
static int qp_mon_c_result(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                           QPMonCode *m, BOOL is_raise)
{
    QPState *qs = ctx->py;
    int i = qp_mon_ccall_find(ctx, sf), tools, r = 0;
    QPMonCCall e;
    JSValue extra[2];

    if (i < 0)
        return 0;
    e = qs->mon_ccalls[i];
    qs->mon_ccall_count = i;
    tools = qp_mon_tools_at(m, QP_MON_EV_CALL, e.pc);
    if (tools) {
        extra[0] = e.callable;
        extra[1] = e.arg0;
        if (is_raise) {
            qp_mon_event_protected(ctx, sf, b, m, QP_MON_EV_C_RAISE, e.pc,
                                   qp_mon_offset(b, e.pc), tools, 2, (JSValueConst *)extra);
        } else {
            r = qp_mon_event(ctx, sf, b, m, QP_MON_EV_C_RETURN, e.pc, qp_mon_offset(b, e.pc),
                             tools, (JSValueConst *)extra, 2);
        }
    }
    JS_FreeValue(ctx, e.callable);
    JS_FreeValue(ctx, e.arg0);
    return r;
}

/* A callback of a LINE / JUMP / BRANCH event set frame.f_lineno: 1 (the
   jump is made by qp_trace_jump()). */
static int qp_mon_jump_requested(JSStackFrame *sf)
{
    QPFrameObject *f;

    if (JS_VALUE_GET_TAG(sf->py_frame) != JS_TAG_OBJECT)
        return 0;
    f = qp_fr_data(sf->py_frame);
    f->lineno = 0;
    return f->jump_to != 0;
}

static BOOL qp_mon_jump_allowed(JSContext *ctx, JSStackFrame *sf)
{
    return ctx->py->mon_event_sf == sf;
}

/* Jump-capable event (LINE, JUMP, BRANCH_*): frame.f_lineno may be set by
   the callbacks; line is f_lineno during the call (0: the current
   instruction's).  -1 error, 1 jump requested, else 0. */
static int qp_mon_jump_event(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                             QPMonCode *m, int event, uint32_t pc, JSValue *args, int nargs,
                             int tools, int line)
{
    QPState *qs = ctx->py;
    JSStackFrame *saved_sf = qs->mon_event_sf;
    int saved_line = qs->mon_event_line, r;

    if (sf->py_mon_flags & QP_MON_SF_NATIVE)
        return 0;

    qs->mon_event_sf = sf;
    qs->mon_event_line = line;
    r = qp_mon_fire(ctx, m, event, pc, tools, (JSValueConst *)args, nargs);
    qs->mon_event_sf = saved_sf;
    qs->mon_event_line = saved_line;
    if (r < 0) {
        if (JS_VALUE_GET_TAG(sf->py_frame) == JS_TAG_OBJECT)
            qp_fr_data(sf->py_frame)->jump_to = 0;
        return -1;
    }
    return qp_mon_jump_requested(sf);
}

/* Byte pc of the target of the jump at pc (goto*, if_*, py jump_if_*,
   for_iter, send); -1 if not a jump. */
static int qp_mon_jump_target(const uint8_t *buf, uint32_t pc)
{
    const uint8_t *p = buf + pc;

    switch (p[0]) {
    case OP_goto:
    case OP_if_false:
    case OP_if_true:
        return (int)pc + 1 + (int32_t)get_u32(p + 1);
#if SHORT_OPCODES
    case OP_goto8:
    case OP_if_false8:
    case OP_if_true8:
        return (int)pc + 1 + (int8_t)p[1];
    case OP_goto16:
        return (int)pc + 1 + (int16_t)get_u16(p + 1);
#endif
    case OP_py:
        switch (p[1]) {
        case QP_OP_jump_if_false:
        case QP_OP_jump_if_true:
        case QP_OP_for_iter:
        case QP_OP_send:
            return (int)pc + 2 + (int32_t)get_u32(p + 2);
        }
        break;
    }
    return -1;
}

/* Start of the instruction containing byte p of b. */
static const uint8_t *qp_mon_insn_start(JSFunctionBytecode *b, const uint8_t *p)
{
    int pc = qp_code_lasti2pc(b, qp_code_pc2lasti(b, (uint32_t)(p - b->byte_code_buf)));

    return pc < 0 ? p : b->byte_code_buf + pc;
}

static uint32_t qp_mon_insn_size(JSFunctionBytecode *b, uint32_t pc)
{
    JSOpCode tmp;
    int opnd;

    return js_get_op_info(b->byte_code_buf, b->byte_code_len, pc, TRUE, &tmp, &opnd)->size;
}

/* qp_mon_flow() bits of an instruction start */
#define QP_MON_FLOW_WARM    1   /* reachable from the start without exception edges */
#define QP_MON_FLOW_TARGET  2   /* a jump target or an exception handler */
#define QP_MON_FLOW_EXIT    4   /* leaves the frame: return, raise */
#define QP_MON_FLOW_JUMP    8   /* ends a basic block: jump, branch, OP_catch */
#define QP_MON_FLOW_START   32  /* an instruction starts here */
#define QP_MON_FLOW_HANDLER 64  /* an exception handler (OP_catch target) */
#define QP_MON_FLOW_AFOR    128 /* the end_send of the __anext__ await of an async for: the
                                   source of its branches (QPMonCode.afor) */
#define QP_MON_FLOW_NOLINE  16  /* no line number in CPython: an exception handler (and the
                                   rest of its block) whose location is inherited from the
                                   code before it, not from a predecessor */

/* a location entry of b starts at pc (else the instruction inherits the
   location of the code before it) */
static BOOL qp_mon_has_loc(JSFunctionBytecode *b, uint32_t pc)
{
    QPCodeInfo *ci = b->py_code;
    int lo = 0, hi = (int)ci->pcloc_count - 1, mid;

    while (lo <= hi) {
        mid = (lo + hi) >> 1;
        if (ci->pcloc[mid].pc == pc)
            return TRUE;
        if (ci->pcloc[mid].pc < pc)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return FALSE;
}

/* QP_MON_FLOW_EXIT of the instruction at pos, or QP_MON_FLOW_JUMP, or 0 */
static int qp_mon_flow_kind(const uint8_t *buf, int pos)
{
    switch (buf[pos]) {
    case OP_return:
    case OP_return_undef:
    case OP_return_async:
    case OP_throw:
        return QP_MON_FLOW_EXIT;
    case OP_catch:
        return QP_MON_FLOW_JUMP;
    case OP_py:
        if (qp_subop_flow_kind(buf[pos + 1], NULL) == QP_FLOW_EXIT)
            return QP_MON_FLOW_EXIT;
        break;
    }
    return qp_mon_jump_target(buf, pos) >= 0 ? QP_MON_FLOW_JUMP : 0;
}

static BOOL qp_mon_is_goto(int op)
{
#if SHORT_OPCODES
    if (op == OP_goto8 || op == OP_goto16)
        return TRUE;
#endif
    return op == OP_goto;
}

/* Control flow facts of the instructions of b (CPython flowgraph.c
   notions: warm blocks of push_cold_blocks_to_end, block boundaries);
   NULL with MemoryError. */
static const uint8_t *qp_mon_flow(JSContext *ctx, QPMonCode *m, JSFunctionBytecode *b)
{
    const uint8_t *buf = b->byte_code_buf;
    int len = b->byte_code_len, n = 0, pos, prev, target, next, kind;
    uint8_t *flow;
    uint32_t *work;

    if (m->flow)
        return m->flow;
    flow = js_mallocz(ctx, len + 1);
    work = js_malloc(ctx, (2 * (size_t)len + 1) * sizeof(*work));
    if (!flow || !work) {
        js_free(ctx, flow);
        js_free(ctx, work);
        return NULL;
    }
    for (pos = 0; pos < len; pos += qp_mon_insn_size(b, pos)) {
        flow[pos] |= qp_mon_flow_kind(buf, pos) | QP_MON_FLOW_START;
        target = buf[pos] == OP_catch ? pos + 1 + (int32_t)get_u32(buf + pos + 1) :
            qp_mon_jump_target(buf, pos);
        if (target >= 0 && target < len) {
            flow[target] |= QP_MON_FLOW_TARGET;
            if (buf[pos] == OP_catch)
                flow[target] |= QP_MON_FLOW_HANDLER;
        }
        if (buf[pos] == OP_catch && target > pos && target < len && buf[target] == OP_py &&
            buf[target + 1] == QP_OP_end_async_for) {
            /* async for: catch(except) ... yield from of __anext__ ...
               end_send ... except: end_async_for (CPython END_ASYNC_FOR's
               branch from the END_SEND) */
            int p;
            for (p = pos; p < target; p += qp_mon_insn_size(b, p)) {
                if (buf[p] == OP_py && buf[p + 1] == QP_OP_end_send)
                    break;
            }
            if (p < target) {
                uint32_t *na = js_realloc(ctx, m->afor, (m->afor_count + 1) * 2 * sizeof(*na));
                if (!na) {
                    js_free(ctx, flow);
                    js_free(ctx, work);
                    return NULL;
                }
                m->afor = na;
                na[2 * m->afor_count] = p;
                na[2 * m->afor_count + 1] = target;
                m->afor_count++;
                flow[p] |= QP_MON_FLOW_AFOR;
            }
        }
    }
    /* handlers entered only by exceptions, without their own location
       (codegen NO_LOCATION, e.g. the StopIteration handler of generators) */
    for (pos = 0, prev = -1; pos < len; prev = pos, pos += qp_mon_insn_size(b, pos)) {
        int p;
        /* (cleanup_throw is emitted with the location of its yield from /
           await, the same as the code before it: no location entry) */
        if (!(flow[pos] & QP_MON_FLOW_HANDLER) ||
            (buf[pos] == OP_py && buf[pos + 1] == QP_OP_cleanup_throw))
            continue;
        if (qp_mon_has_loc(b, pos) || prev < 0 ||
            (!(flow[prev] & QP_MON_FLOW_EXIT) && !qp_mon_is_goto(buf[prev])))
            continue;
        for (p = pos; p < len && (p == pos || !qp_mon_has_loc(b, p)); p += qp_mon_insn_size(b, p)) {
            flow[p] |= QP_MON_FLOW_NOLINE;
            if ((flow[p] & QP_MON_FLOW_EXIT) || qp_mon_jump_target(buf, p) >= 0)
                break;  /* end of the block (OP_catch falls through) */
        }
    }
    work[n++] = 0;
    while (n > 0) {
        pos = work[--n];
        if (pos >= len || (flow[pos] & QP_MON_FLOW_WARM))
            continue;
        flow[pos] |= QP_MON_FLOW_WARM;
        next = pos + qp_mon_insn_size(b, pos);
        kind = flow[pos] & (QP_MON_FLOW_EXIT | QP_MON_FLOW_JUMP);
        target = qp_mon_jump_target(buf, pos);   /* not OP_catch: its handler is cold */
        if (target >= 0 && target < len && !(flow[target] & QP_MON_FLOW_WARM))
            work[n++] = target;
        if (kind != QP_MON_FLOW_EXIT && !qp_mon_is_goto(buf[pos]) && next < len &&
            !(flow[next] & QP_MON_FLOW_WARM))
            work[n++] = next;
    }
    js_free(ctx, work);
    m->flow = flow;
    return flow;
}

/* Line of the instruction containing byte pc for LINE events (-1: none). */
static int qp_mon_line_of(JSFunctionBytecode *b, const uint8_t *flow, uint32_t pc)
{
    uint32_t start = pc;

    if (!(flow[pc] & QP_MON_FLOW_START))
        start = (uint32_t)(qp_mon_insn_start(b, b->byte_code_buf + pc) - b->byte_code_buf);
    if (flow[start] & QP_MON_FLOW_NOLINE)
        return -1;
    return qp_fr_line_at(b, pc);
}

/* The unconditional jump at off to target is not a jump in CPython's
   code: a jump to the next instruction once the cold blocks (exception
   handlers) are moved to the end, or a jump to a small exit block that
   the CPython compiler copies (inline_small_or_no_lineno_blocks,
   MAX_COPY_SIZE 4), or the exit of an exception handler (a
   JUMP_BACKWARD_NO_INTERRUPT).  -1 with MemoryError. */
static int qp_mon_jump_elided(JSContext *ctx, QPMonCode *m, JSFunctionBytecode *b,
                              uint32_t off, int target)
{
    const uint8_t *flow = qp_mon_flow(ctx, m, b);
    int pos, n;

    if (!flow)
        return -1;
    /* the exit of a cold block (exception handler) without location to the
       warm code: CPython's JUMP_NO_INTERRUPT, backward once the cold
       blocks are moved to the end, is a JUMP_BACKWARD_NO_INTERRUPT (no
       JUMP event) */
    if (!(flow[off] & QP_MON_FLOW_WARM) && target >= 0 && target < (int)b->byte_code_len &&
        (flow[target] & QP_MON_FLOW_WARM) && !qp_mon_has_loc(b, off))
        return 1;
    if (target > (int)off && (flow[off] & QP_MON_FLOW_WARM)) {
        for (pos = off + 1; pos < target && !(flow[pos] & QP_MON_FLOW_WARM); pos++)
            ;
        if (pos == target)
            return 1;
    }
    for (pos = target, n = 0; n < 4 && pos < (int)b->byte_code_len; n++) {
        if (pos != target && (flow[pos] & QP_MON_FLOW_TARGET))
            return 0;   /* another block */
        if (flow[pos] & QP_MON_FLOW_EXIT)
            return 1;
        if (flow[pos] & QP_MON_FLOW_JUMP)
            return 0;
        pos += qp_mon_insn_size(b, pos);
    }
    return 0;
}

/* end_send of the async for whose end_async_for is at pc, -1 if none */
static int qp_mon_afor_src(QPMonCode *m, uint32_t pc)
{
    int i;

    for (i = 0; i < m->afor_count; i++) {
        if (m->afor[2 * i + 1] == pc)
            return m->afor[2 * i];
    }
    return -1;
}

/* BRANCH_LEFT / BRANCH_RIGHT of the branch at src, now at dest. */
static int qp_mon_branch(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                         QPMonCode *m, uint32_t src, uint32_t dest)
{
    const uint8_t *buf = b->byte_code_buf;
    int event, tools, r, back = -1, dest_off;
    JSValue args[3];

    if (buf[src] == OP_py && buf[src + 1] == QP_OP_end_async_for) {
        /* the loop ends (StopAsyncIteration): from the END_SEND */
        r = qp_mon_afor_src(m, src);
        if (r < 0)
            return 0;
        src = r;
        event = QP_MON_EV_BRANCH_RIGHT;
    } else {
        event = dest == src + qp_mon_insn_size(b, src) ? QP_MON_EV_BRANCH_LEFT :
            QP_MON_EV_BRANCH_RIGHT;
        if (event == QP_MON_EV_BRANCH_RIGHT && buf[src] == OP_py &&
            buf[src + 1] == QP_OP_for_iter) {
            /* past END_FOR / POP_ITER (two drops) */
            int i;
            for (i = 0; i < 2 && dest < b->byte_code_len && buf[dest] == OP_drop; i++)
                dest++;
        }
        /* a backward target: CPython's inverted POP_JUMP_IF_* over a
           JUMP_BACKWARD (normalize_jumps), taken through BRANCH_LEFT to
           the JUMP_BACKWARD and its JUMP */
        back = qp_code_branch_back(b, src);
        if (back >= 0)
            event = event == QP_MON_EV_BRANCH_LEFT ? QP_MON_EV_BRANCH_RIGHT :
                QP_MON_EV_BRANCH_LEFT;
    }
    dest_off = qp_code_target_offset(b, dest);
    tools = qp_mon_tools_at(m, event, src);
    if (tools) {
        r = qp_code_branch_src(b, src);
        if (qp_mon_args(ctx, sf, b, r >= 0 ? r : qp_mon_offset(b, src), args) < 0)
            return -1;
        args[2] = JS_NewInt32(ctx, back >= 0 && event == QP_MON_EV_BRANCH_LEFT ? back : dest_off);
        r = qp_mon_jump_event(ctx, sf, b, m, event, src, args, 3, tools, qp_fr_line_at(b, src));
        JS_FreeValue(ctx, args[0]);
        if (r)
            return r;
    }
    if (back < 0 || event != QP_MON_EV_BRANCH_LEFT)
        return 0;
    tools = qp_mon_tools_at(m, QP_MON_EV_JUMP, src);
    if (!tools)
        return 0;
    if (qp_mon_args(ctx, sf, b, back, args) < 0)
        return -1;
    args[2] = JS_NewInt32(ctx, dest_off);
    r = qp_mon_jump_event(ctx, sf, b, m, QP_MON_EV_JUMP, src, args, 3, tools, qp_fr_line_at(b, src));
    JS_FreeValue(ctx, args[0]);
    return r;
}

/* Events reported by the instruction at pc (byte offset off) of sf before
   it executes.  phase 0: the pending events of the previous instruction
   (C_RETURN, BRANCH_*); phase 1: LINE, INSTRUCTION, then CALL / JUMP /
   PY_RETURN / PY_YIELD.  -1 error, 1 f_lineno jump requested. */
static int qp_mon_instr(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc, JSValue *sp,
                        int phase)
{
    JSFunctionBytecode *b = qp_mon_sf_bytecode(sf);
    uint32_t off = (uint32_t)(pc - b->byte_code_buf);
    QPMonCode *m = qp_mon_code(ctx, b);
    int op = pc[0], subop = op == OP_py ? pc[1] : -1, tools, r = 0;
    JSValue args[4];

    if (!m)
        return -1;
    if (phase == 0) {
        uint32_t prev = sf->py_mon_prev;
        int flags = sf->py_mon_flags;
        sf->py_mon_flags = flags & QP_MON_SF_KEEP;
        flags &= ~QP_MON_SF_KEEP;
        if (!flags)
            return 0;
        if (!m->any) {
            if (flags & QP_MON_SF_CCALL)
                qp_mon_ccall_find(ctx, sf);
            return 0;
        }
        if ((flags & QP_MON_SF_CCALL) && qp_mon_c_result(ctx, sf, b, m, FALSE) < 0)
            return -1;
        if ((flags & QP_MON_SF_BRANCH) && prev)
            return qp_mon_branch(ctx, sf, b, m, prev - 1, off);
        return 0;
    }

    {
        uint32_t prev = sf->py_mon_prev;
        BOOL resumed = (sf->py_mon_flags & QP_MON_SF_RESUMED) != 0;
        sf->py_mon_prev = off + 1;
        sf->py_mon_flags &= ~QP_MON_SF_RESUMED;
        if (!m->any)
            return 0;

        /* LINE */
        tools = qp_mon_tools_at(m, QP_MON_EV_LINE, off);
        if (tools && subop != QP_OP_end_send && subop != QP_OP_end_async_for) {
            const uint8_t *flow = qp_mon_flow(ctx, m, b);
            int line;
            BOOL fire;
            if (!flow)
                return -1;
            line = (flow[off] & QP_MON_FLOW_NOLINE) ? -1 : qp_fr_line_at(b, off);
            fire = line >= 0 && (!prev || qp_mon_line_of(b, flow, prev - 1) != line);
            if (!fire && line >= 0 && resumed)
                fire = (flow[off] & QP_MON_FLOW_TARGET) != 0;
            if (fire) {
                args[0] = qp_fo_get_code(ctx, sf->cur_func);
                if (JS_IsException(args[0]))
                    return -1;
                args[1] = JS_NewInt32(ctx, line);
                r = qp_mon_jump_event(ctx, sf, b, m, QP_MON_EV_LINE, off, args, 2, tools, 0);
                JS_FreeValue(ctx, args[0]);
                if (r)
                    return r;
            }
        }

        /* INSTRUCTION */
        tools = qp_mon_tools_at(m, QP_MON_EV_INSTRUCTION, off);
        if (tools) {
            int ioff = qp_mon_offset(b, off);
            /* (once per co_code instruction: the instructions without a
               CPython equivalent have the offset of the one before) */
            if ((!prev || prev - 1 == off || qp_mon_offset(b, prev - 1) != ioff) &&
                qp_mon_event(ctx, sf, b, m, QP_MON_EV_INSTRUCTION, off, ioff, tools, NULL, 0) < 0)
                return -1;
        }
    }

    switch (op) {
    case OP_return:
    case OP_return_async:
        tools = qp_mon_tools_at(m, QP_MON_EV_PY_RETURN, off);
        if (tools)
            return qp_mon_event(ctx, sf, b, m, QP_MON_EV_PY_RETURN, off, qp_mon_offset(b, off),
                                tools, (JSValueConst *)&sp[-1], 1);
        return 0;
    case OP_goto:
#if SHORT_OPCODES
    case OP_goto8:
    case OP_goto16:
#endif
        tools = qp_mon_tools_at(m, QP_MON_EV_JUMP, off);
        if (tools) {
            int target = qp_mon_jump_target(b->byte_code_buf, off);
            const uint8_t *t = b->byte_code_buf + target;
            /* the loop of yield from / await back to its send is not a
               jump (JUMP_BACKWARD_NO_INTERRUPT) */
            if (target < 0 || (t[0] == OP_py && t[1] == QP_OP_send))
                return 0;
            r = qp_mon_jump_elided(ctx, m, b, off, target);
            if (r)
                return r < 0 ? -1 : 0;
            if (qp_mon_args(ctx, sf, b, qp_mon_offset(b, off), args) < 0)
                return -1;
            args[2] = JS_NewInt32(ctx, qp_code_target_offset(b, target));
            r = qp_mon_jump_event(ctx, sf, b, m, QP_MON_EV_JUMP, off, args, 3, tools, 0);
            JS_FreeValue(ctx, args[0]);
            return r;
        }
        return 0;
    case OP_if_false:
    case OP_if_true:
#if SHORT_OPCODES
    case OP_if_false8:
    case OP_if_true8:
#endif
        goto branch;
    case OP_py:
        break;
    default:
        return 0;
    }

    switch (subop) {
    case QP_OP_jump_if_false:
    case QP_OP_jump_if_true:
    case QP_OP_for_iter:
    branch:
        if (qp_mon_tools_at(m, QP_MON_EV_BRANCH_LEFT, off) |
            qp_mon_tools_at(m, QP_MON_EV_BRANCH_RIGHT, off) |
            qp_mon_tools_at(m, QP_MON_EV_JUMP, off))    /* (qp_code_branch_back) */
            sf->py_mon_flags |= QP_MON_SF_BRANCH;
        return 0;
    case QP_OP_end_send:
    case QP_OP_end_async_for: {
        int src = off;
        const uint8_t *flow;
        if (subop == QP_OP_end_send) {
            flow = qp_mon_flow(ctx, m, b);
            if (!flow)
                return -1;
            if (!(flow[off] & QP_MON_FLOW_AFOR))
                return 0;
        } else {
            if (!qp_mon_flow(ctx, m, b))
                return -1;
            src = qp_mon_afor_src(m, off);
            if (src < 0)
                return 0;
        }
        if (qp_mon_tools_at(m, QP_MON_EV_BRANCH_LEFT, src) |
            qp_mon_tools_at(m, QP_MON_EV_BRANCH_RIGHT, src))
            sf->py_mon_flags |= QP_MON_SF_BRANCH;
        return 0;
    }
    case QP_OP_yield_value:
        tools = qp_mon_tools_at(m, QP_MON_EV_PY_YIELD, off);
        if (tools)
            return qp_mon_event(ctx, sf, b, m, QP_MON_EV_PY_YIELD, off, qp_mon_offset(b, off),
                                tools, (JSValueConst *)&sp[-1], 1);
        return 0;
    case QP_OP_call_method:
    case QP_OP_call_kw:
    case QP_OP_call_ex:
    case QP_OP_load_super_attr:
    case QP_OP_load_super_method:
        break;
    default:
        return 0;
    }

    /* CALL */
    tools = qp_mon_tools_at(m, QP_MON_EV_CALL, off);
    if (!tools)
        return 0;
    {
        QPState *qs = ctx->py;
        QPModState *ms = qs->mod_state;
        JSValueConst callable, arg0 = ms->mon_missing, *cargs;
        JSValue extra[2];
        int argc, t;
        BOOL c_events = FALSE;

        switch (subop) {
        case QP_OP_call_method:
        case QP_OP_call_kw:
            argc = get_u16(pc + 2);
            cargs = (JSValueConst *)(sp - argc - (subop == QP_OP_call_kw));
            callable = cargs[-2];
            if (!QP_IS_NULL(cargs[-1])) {
                arg0 = cargs[-1];
            } else if (JS_VALUE_GET_TAG(callable) == JS_TAG_OBJECT &&
                       JS_VALUE_GET_OBJ(callable)->class_id == JS_CLASS_PY_METHOD) {
                /* _MAYBE_EXPAND_METHOD */
                QPMethodObject *mo = JS_VALUE_GET_OBJ(callable)->u.opaque;
                callable = mo->func;
                arg0 = mo->self;
            } else if (argc > 0) {
                arg0 = cargs[0];
            }
            break;
        case QP_OP_call_ex:
            callable = sp[-4];
            if (qp_tuple_check_exact(ctx, sp[-2]) && QP_TUPLE_SIZE(sp[-2]) > 0)
                arg0 = QP_TUPLE_ITEMS(sp[-2])[0];
            break;
        default: /* load_super_attr / load_super_method: global super, class, self */
            callable = sp[-3];
            if (pc[6] & QP_SUPER_TWO_ARGS)
                arg0 = sp[-2];
            if (subop == QP_OP_load_super_method)
                sf->py_mon_flags |= QP_MON_SF_SUPER_GENERIC;
            break;
        }
        extra[0] = (JSValue)callable;
        extra[1] = (JSValue)arg0;
        r = qp_mon_event(ctx, sf, b, m, QP_MON_EV_CALL, off, qp_mon_offset(b, off), tools,
                         (JSValueConst *)extra, 2);
        if (r < 0)
            return -1;
        /* C_RETURN / C_RAISE of a builtin callable: before the next
           instruction / on the exception path */
        if (qp_mon_is_py_function(callable))
            return 0;
        tools = qp_mon_tools_at(m, QP_MON_EV_CALL, off);
        for (t = 0; t < QP_MON_TOOLS; t++) {
            if ((tools & (1 << t)) &&
                (!QP_IS_NULL(ms->mon_callbacks[t * QP_MON_EVENTS + QP_MON_EV_C_RETURN]) ||
                 !QP_IS_NULL(ms->mon_callbacks[t * QP_MON_EVENTS + QP_MON_EV_C_RAISE])))
                c_events = TRUE;
        }
        if (!c_events)
            return 0;
        if (qs->mon_ccall_count >= qs->mon_ccall_size) {
            int n = qs->mon_ccall_size ? 2 * qs->mon_ccall_size : 16;
            QPMonCCall *nc = js_realloc(ctx, qs->mon_ccalls, n * sizeof(*nc));
            if (!nc)
                return -1;
            qs->mon_ccalls = nc;
            qs->mon_ccall_size = n;
        }
        qs->mon_ccalls[qs->mon_ccall_count++] = (QPMonCCall){
            sf, off, JS_DupValue(ctx, callable), JS_DupValue(ctx, arg0)
        };
        sf->py_mon_flags |= QP_MON_SF_CCALL;
    }
    return 0;
}

/* ---- interpreter hooks ---------------------------------------------------- */

/* sf is called by QP_CO_NATIVE code: the first Python frame before it
   (builtins skipped) is native or has QP_MON_SF_NATIVE */
static BOOL qp_mon_called_by_native(JSStackFrame *sf)
{
    JSObject *p;

    for (sf = sf->prev_frame; sf; sf = sf->prev_frame) {
        p = qp_fr_sf_func(sf);
        if (!p)
            continue;
        return (p->u.func.function_bytecode->py_code->co_flags & QP_CO_NATIVE) ||
            (sf->py_mon_flags & QP_MON_SF_NATIVE);
    }
    return FALSE;
}

static int qp_hook_call(JSContext *ctx, JSStackFrame *sf, BOOL throw_flag)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b = qp_mon_sf_bytecode(sf);
    BOOL start = sf->cur_pc <= b->byte_code_buf;
    QPMonCode *m;
    uint32_t ypc;
    int event, tools, offset;

    if (qs->trace_active && qp_trace_call(ctx, sf) < 0)
        return -1;
    if (!qs->mon_active)
        return 0;
    m = qp_mon_code(ctx, b);
    if (!m)
        return -1;
    if (start) {
        sf->py_mon_flags = qp_mon_called_by_native(sf) ? QP_MON_SF_NATIVE : 0;
        sf->py_mon_prev = 0;
        ypc = 0;
    } else {
        /* resumption after a yield_value (3 bytes) */
        ypc = (uint32_t)(sf->cur_pc - b->byte_code_buf);
        if (ypc >= 3 && sf->cur_pc[-3] == OP_py && sf->cur_pc[-2] == QP_OP_yield_value)
            ypc -= 3;
        else
            ypc--;
        sf->py_mon_flags = (sf->py_mon_flags & ~QP_MON_SF_KEEP) | QP_MON_SF_RESUMED;
    }
    if (!m->any)
        return 0;
    offset = qp_code_resume_offset(b, ypc);
    if (offset < 0)
        offset = qp_mon_offset(b, ypc);
    if (throw_flag) {
        tools = m->active[QP_MON_EV_PY_THROW];
        if (tools)
            qp_mon_event_protected(ctx, sf, b, m, QP_MON_EV_PY_THROW, ypc, offset, tools, 1,
                                   (JSValueConst *)&ctx->rt->current_exception);
        return 0;
    }
    event = start ? QP_MON_EV_PY_START : QP_MON_EV_PY_RESUME;
    tools = qp_mon_tools_at(m, event, ypc);
    if (!tools)
        return 0;
    return qp_mon_event(ctx, sf, b, m, event, ypc, offset, tools, NULL, 0);
}

static int qp_hook_instr(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc, JSValue *sp)
{
    QPState *qs = ctx->py;
    int r;

    if (qs->mon_active && (r = qp_mon_instr(ctx, sf, pc, sp, 0)) != 0)
        return r;
    if (qs->trace_active && (r = qp_trace_instr(ctx, sf, pc, sp)) != 0)
        return r;
    if (qs->mon_active)
        return qp_mon_instr(ctx, sf, pc, sp, 1);
    return 0;
}

static void qp_hook_exception(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc,
                              BOOL reraise)
{
    QPState *qs = ctx->py;

    if (qs->trace_active)
        qp_trace_exception(ctx, sf, pc, reraise);
    if (qs->mon_active) {
        JSFunctionBytecode *b = qp_mon_sf_bytecode(sf);
        QPMonCode *m;
        int flags = sf->py_mon_flags, event, tools;
        uint32_t off;

        sf->py_mon_flags = flags & (QP_MON_SF_RESUMED | QP_MON_SF_NATIVE);
        if (pc < b->byte_code_buf)
            return;     /* before the first instruction (PY_START raised) */
        off = (uint32_t)(pc - b->byte_code_buf);
        sf->py_mon_prev = off + 1;
        m = qp_mon_code(ctx, b);
        if (!m) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return;
        }
        if (!m->any) {
            if (flags & QP_MON_SF_CCALL)
                qp_mon_ccall_find(ctx, sf);
            return;
        }
        if (flags & QP_MON_SF_CCALL)
            qp_mon_c_result(ctx, sf, b, m, TRUE);
        {
            const uint8_t *p = qp_mon_insn_start(b, pc);
            if (p[0] == OP_py && p[1] == QP_OP_send) {
                sf->py_mon_flags |= QP_MON_SF_SEND_RAISE;
            } else if (reraise && p[0] == OP_py && p[1] == QP_OP_cleanup_throw &&
                       (flags & QP_MON_SF_SEND_RAISE)) {
                return;
            }
        }
        event = reraise ? QP_MON_EV_RERAISE : QP_MON_EV_RAISE;
        tools = m->active[event];
        if (tools)
            qp_mon_event_protected(ctx, sf, b, m, event, off, qp_mon_offset(b, off), tools, 1,
                                   (JSValueConst *)&ctx->rt->current_exception);
    }
}

static void qp_hook_handled(JSContext *ctx, JSStackFrame *sf, const uint8_t *handler_pc)
{
    JSFunctionBytecode *b = qp_mon_sf_bytecode(sf);
    uint32_t off = (uint32_t)(handler_pc - b->byte_code_buf);
    QPMonCode *m = qp_mon_code(ctx, b);
    JSValue exc;
    int tools;

    if (!m) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    tools = m->active[QP_MON_EV_EXCEPTION_HANDLED];
    if (!tools || ((sf->py_mon_flags & QP_MON_SF_SEND_RAISE) &&
                   handler_pc[0] == OP_py && handler_pc[1] == QP_OP_cleanup_throw))
        return;
    /* the exception is not pending during the callbacks (monitor_handled);
       an exception they raise replaces it */
    exc = JS_GetException(ctx);
    if (qp_mon_event(ctx, sf, b, m, QP_MON_EV_EXCEPTION_HANDLED, off, qp_mon_offset(b, off),
                     tools, (JSValueConst *)&exc, 1) < 0)
        JS_FreeValue(ctx, exc);
    else
        JS_Throw(ctx, exc);
}

static void qp_hook_unwind(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc)
{
    QPState *qs = ctx->py;

    if (qs->trace_active)
        qp_trace_unwind(ctx, sf, pc);
    if (qs->mon_active) {
        JSFunctionBytecode *b = qp_mon_sf_bytecode(sf);
        uint32_t off = pc > b->byte_code_buf ? (uint32_t)(pc - b->byte_code_buf) : 0;
        QPMonCode *m = qp_mon_code(ctx, b);
        int tools;

        if (!m) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return;
        }
        if (sf->py_mon_flags & QP_MON_SF_CCALL)
            qp_mon_ccall_find(ctx, sf);
        sf->py_mon_flags = 0;
        tools = m->active[QP_MON_EV_PY_UNWIND];
        if (tools)
            qp_mon_event_protected(ctx, sf, b, m, QP_MON_EV_PY_UNWIND, off,
                                   qp_mon_offset(b, off), tools, 1,
                                   (JSValueConst *)&ctx->rt->current_exception);
    }
}

static int qp_hook_send_return(JSContext *ctx, JSStackFrame *sf, JSValueConst receiver,
                               JSValueConst value)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b;
    QPMonCode *m;
    const uint8_t *p;
    uint32_t target;
    JSValue exc;
    int tools, r;

    if (qs->tracing)
        return 0;
    if (qs->trace_active && qp_trace_send_return(ctx, sf, receiver, value) < 0)
        return -1;
    if (!qs->mon_active || (!qp_gen_check(receiver) && !qp_coro_check(receiver)))
        return 0;
    b = qp_mon_sf_bytecode(sf);
    m = qp_mon_code(ctx, b);
    if (!m)
        return -1;
    /* sf->cur_pc is at the label operand of send: the event is at its
       target (END_SEND) */
    p = sf->cur_pc;
    target = (uint32_t)(p - b->byte_code_buf) + (int32_t)get_u32(p);
    tools = qp_mon_tools_at(m, QP_MON_EV_STOP_ITERATION, target);
    if (!tools)
        return 0;
    /* PyErr_SetObject(PyExc_StopIteration, value) */
    if (JS_IsUndefined(value))
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 0, NULL);
    else
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 1, &value);
    if (JS_IsException(exc))
        return -1;
    r = qp_mon_event(ctx, sf, b, m, QP_MON_EV_STOP_ITERATION, target,
                     qp_code_target_offset(b, target), tools, (JSValueConst *)&exc, 1);
    JS_FreeValue(ctx, exc);
    return r;
}

static int qp_hook_for_iter_stop(JSContext *ctx)
{
    QPState *qs = ctx->py;
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    JSFunctionBytecode *b;
    QPMonCode *m;
    uint32_t off;
    int tools;

    if (qs->trace_active && qp_trace_for_iter_stop(ctx) < 0)
        return -1;
    if (!qs->mon_active || qs->tracing || !qp_fr_sf_func(sf))
        return 0;
    b = qp_mon_sf_bytecode(sf);
    if (sf->cur_pc < b->byte_code_buf + 2 || sf->cur_pc[-2] != OP_py ||
        sf->cur_pc[-1] != QP_OP_for_iter)
        return 0;
    m = qp_mon_code(ctx, b);
    if (!m)
        return -1;
    tools = m->active[QP_MON_EV_RAISE];
    if (!tools)
        return 0;
    off = (uint32_t)(sf->cur_pc - b->byte_code_buf) - 2;
    return qp_mon_event_protected(ctx, sf, b, m, QP_MON_EV_RAISE, off, qp_mon_offset(b, off),
                                  tools, 1, (JSValueConst *)&ctx->rt->current_exception);
}

#ifdef CONFIG_PYTHON_CAPI
/* ---- C API (capi_runtime.c PyMonitoring_*) ------------------------------- */

/* global monitoring version (PyMonitoring_EnterScope): never 0, changes
   with every event set change and restart_events() */
static uint64_t qp_mon_capi_version(JSContext *ctx)
{
    return ctx->py->mon_version;
}

/* tools with event in their global set (C_RETURN / C_RAISE follow CALL) */
static int qp_mon_capi_tools(JSContext *ctx, int event)
{
    uint8_t g[QP_MON_UNGROUPED_EVENTS];

    if (event == QP_MON_EV_C_RETURN || event == QP_MON_EV_C_RAISE)
        event = QP_MON_EV_CALL;
    if (event < 0 || event >= QP_MON_UNGROUPED_EVENTS)
        return 0;
    qp_mon_global_tools(ctx->py->mod_state, g);
    return g[event];
}

/* capi_call_instrumentation: the callbacks of the tools in *active,
   highest first; DISABLE removes the tool from *active (events with a
   location) or is a ValueError.  -1 if a callback raised. */
static int qp_mon_capi_fire(JSContext *ctx, uint8_t *active, int event, JSValueConst *args,
                            int nargs)
{
    QPState *qs = ctx->py;
    QPModState *ms = qs->mod_state;
    int t;

    if (!ms || event < 0 || event >= QP_MON_EVENTS)
        return 0;
    for (t = QP_MON_TOOLS - 1; t >= 0; t--) {
        JSValue cb, res;
        if (!(*active & (1 << t)))
            continue;
        cb = ms->mon_callbacks[t * QP_MON_EVENTS + event];
        if (QP_IS_NULL(cb))
            continue;
        cb = JS_DupValue(ctx, cb);
        qs->tracing++;
        res = qp_vectorcall(ctx, cb, args, nargs, QP_NULL);
        qs->tracing--;
        JS_FreeValue(ctx, cb);
        if (JS_IsException(res))
            return -1;
        if (JS_VALUE_GET_TAG(res) == JS_TAG_OBJECT && !QP_IS_NULL(ms->mon_disable) &&
            JS_VALUE_GET_OBJ(res) == JS_VALUE_GET_OBJ(ms->mon_disable)) {
            JS_FreeValue(ctx, res);
            if (event < QP_MON_LOC_EVENTS) {
                *active &= ~(1 << t);
            } else {
                JS_FreeValue(ctx, ms->mon_callbacks[t * QP_MON_EVENTS + event]);
                ms->mon_callbacks[t * QP_MON_EVENTS + event] = QP_NULL;
                qp_raise_value_error(ctx, "Cannot disable %s events. Callback removed.",
                                     qp_mon_event_names[event]);
                return -1;
            }
        } else {
            JS_FreeValue(ctx, res);
        }
    }
    return 0;
}
#endif

/* ---- the sys.monitoring module -------------------------------------------- */

#define QP_MON_FN(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                        int nargs, JSValueConst kwnames, int magic)

/* Positional int arguments args[which[i]] of fname into out[]. */
static int qp_mon_ints(JSContext *ctx, const char *fname, JSValueConst *args, int nargs,
                       int n, const int *which, int *out)
{
    int i;

    if (qp_arg_check_positional(ctx, fname, nargs, n, n) < 0)
        return -1;
    for (i = 0; which[i] >= 0; i++) {
        if (qp_arg_int(ctx, args[which[i]], &out[i]) < 0)
            return -1;
    }
    return 0;
}

static int qp_mon_check_tool(JSContext *ctx, int tool_id)
{
    if (tool_id < 0 || tool_id >= QP_MON_TOOLS) {
        qp_raise_value_error(ctx, "invalid tool %d (must be between 0 and 5)", tool_id);
        return -1;
    }
    return 0;
}

/* check_tool: the tool must be in use */
static int qp_mon_check_in_use(JSContext *ctx, QPModState *ms, int tool_id)
{
    if (QP_IS_NULL(ms->mon_tool_names[tool_id])) {
        qp_raise_value_error(ctx, "tool %d is not in use", tool_id);
        return -1;
    }
    return 0;
}

/* C_RETURN / C_RAISE only with CALL (and dropped: they follow CALL);
   BRANCH = BRANCH_LEFT | BRANCH_RIGHT. */
static int qp_mon_normalize_events(JSContext *ctx, int *event_set)
{
    int e = *event_set;

    if ((e & QP_MON_C_RETURN_EVENTS) && (e & QP_MON_C_CALL_EVENTS) != QP_MON_C_CALL_EVENTS) {
        qp_raise_value_error(ctx, "cannot set C_RETURN or C_RAISE events independently");
        return -1;
    }
    e &= ~QP_MON_C_RETURN_EVENTS;
    if (e & (1 << QP_MON_EV_BRANCH)) {
        e &= ~(1 << QP_MON_EV_BRANCH);
        e |= (1 << QP_MON_EV_BRANCH_RIGHT) | (1 << QP_MON_EV_BRANCH_LEFT);
    }
    *event_set = e;
    return 0;
}

/* Local event set of tool_id in m. */
static int qp_mon_local_set(QPMonCode *m, int tool_id)
{
    int e, set = 0;

    for (e = 0; m && e < QP_MON_UNGROUPED_EVENTS; e++) {
        if (m->local[e] & (1 << tool_id))
            set |= 1 << e;
    }
    return set;
}

static void qp_mon_set_local(QPState *qs, QPMonCode *m, int tool_id, int set)
{
    int e, any = 0;

    for (e = 0; e < QP_MON_UNGROUPED_EVENTS; e++) {
        if (set & (1 << e))
            m->local[e] |= 1 << tool_id;
        else
            m->local[e] &= ~(1 << tool_id);
        any |= m->local[e];
    }
    list_del(&m->link);
    if (any)
        list_add_tail(&m->link, &qs->mon_codes);
    else
        init_list_head(&m->link);
}

/* _PyMonitoring_ClearToolId: callbacks, global and local events of the tool. */
static int qp_mon_clear_tool(JSContext *ctx, QPModState *ms, int tool_id)
{
    QPState *qs = ctx->py;
    struct list_head *el, *el1;
    int e;

    ms->mon_events[tool_id] = 0;
    for (e = 0; e < QP_MON_EVENTS; e++) {
        JSValue *slot = &ms->mon_callbacks[tool_id * QP_MON_EVENTS + e];
        JSValue old = *slot;
        *slot = QP_NULL;
        JS_FreeValue(ctx, old);
    }
    ms->mon_branch_legacy[tool_id] = 0;
    list_for_each_safe(el, el1, &qs->mon_codes) {
        QPMonCode *m = list_entry(el, QPMonCode, link);
        qp_mon_set_local(qs, m, tool_id, 0);
    }
    return qp_mon_changed(ctx);
}

/* use_tool_id(tool_id, name, /) */
QP_MON_FN(qp_mon_use_tool_id)
{
    static const int which[] = { 0, -1 };
    QPModState *ms;
    int tool_id;

    if (qp_mon_ints(ctx, "use_tool_id", args, nargs, 2, which, &tool_id) < 0 ||
        qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[1]))
        return qp_raise_value_error(ctx, "tool name must be a str");
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(ms->mon_tool_names[tool_id]))
        return qp_raise_value_error(ctx, "tool %d is already in use", tool_id);
    ms->mon_tool_names[tool_id] = JS_DupValue(ctx, args[1]);
    return JS_UNDEFINED;
}

/* clear_tool_id(tool_id, /) and free_tool_id(tool_id, /) (magic 1) */
QP_MON_FN(qp_mon_clear_tool_id)
{
    static const int which[] = { 0, -1 };
    QPModState *ms;
    int tool_id;

    if (qp_mon_ints(ctx, magic ? "free_tool_id" : "clear_tool_id", args, nargs, 1, which,
                    &tool_id) < 0 ||
        qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(ms->mon_tool_names[tool_id])) {
        if (qp_mon_clear_tool(ctx, ms, tool_id) < 0)
            return JS_EXCEPTION;
        if (magic) {
            JS_FreeValue(ctx, ms->mon_tool_names[tool_id]);
            ms->mon_tool_names[tool_id] = QP_NULL;
        }
    }
    return JS_UNDEFINED;
}

/* get_tool(tool_id, /) */
QP_MON_FN(qp_mon_get_tool)
{
    static const int which[] = { 0, -1 };
    QPModState *ms;
    int tool_id;

    if (qp_mon_ints(ctx, "get_tool", args, nargs, 1, which, &tool_id) < 0 ||
        qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (QP_IS_NULL(ms->mon_tool_names[tool_id]))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ms->mon_tool_names[tool_id]);
}

/* register_callback(tool_id, event, func, /) -> previous callback or None */
QP_MON_FN(qp_mon_register_callback)
{
    static const int which[] = { 0, 1, -1 };
    QPModState *ms;
    int v[2], event_id;
    JSValue func, old, *slot;

    if (qp_mon_ints(ctx, "register_callback", args, nargs, 3, which, v) < 0 ||
        qp_mon_check_tool(ctx, v[0]) < 0)
        return JS_EXCEPTION;
    if (v[1] <= 0 || (v[1] & (v[1] - 1)) != 0)
        return qp_raise_value_error(ctx, "The callback can only be set for one event at a time");
    for (event_id = 0; (1 << event_id) != v[1]; event_id++)
        ;
    if (event_id >= QP_MON_EVENTS)
        return qp_raise_value_error(ctx, "invalid event %d", v[1]);
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    func = JS_IsUndefined(args[2]) ? QP_NULL : JS_DupValue(ctx, args[2]);
    if (event_id == QP_MON_EV_BRANCH) {
        /* both directions (_PyMonitoring_RegisterCallback): the previous
           BRANCH_LEFT callback is returned */
        slot = &ms->mon_callbacks[v[0] * QP_MON_EVENTS + QP_MON_EV_BRANCH_RIGHT];
        JS_FreeValue(ctx, *slot);
        *slot = JS_DupValue(ctx, func);
        event_id = QP_MON_EV_BRANCH_LEFT;
        ms->mon_branch_legacy[v[0]] = QP_IS_NULL(func) ? 0 : 3;
    } else if (event_id == QP_MON_EV_BRANCH_LEFT || event_id == QP_MON_EV_BRANCH_RIGHT) {
        ms->mon_branch_legacy[v[0]] &= ~(1 << (event_id - QP_MON_EV_BRANCH_LEFT));
    }
    slot = &ms->mon_callbacks[v[0] * QP_MON_EVENTS + event_id];
    old = *slot;
    *slot = func;
    return QP_IS_NULL(old) ? JS_UNDEFINED : old;
}

/* get_events(tool_id, /) -> int */
QP_MON_FN(qp_mon_get_events)
{
    static const int which[] = { 0, -1 };
    QPModState *ms;
    int tool_id;

    if (qp_mon_ints(ctx, "get_events", args, nargs, 1, which, &tool_id) < 0 ||
        qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    ms = qp_modstate(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)ms->mon_events[tool_id]);
}

/* set_events(tool_id, event_set, /) */
QP_MON_FN(qp_mon_set_events)
{
    static const int which[] = { 0, 1, -1 };
    QPModState *ms;
    int v[2];

    if (qp_mon_ints(ctx, "set_events", args, nargs, 2, which, v) < 0 ||
        qp_mon_check_tool(ctx, v[0]) < 0)
        return JS_EXCEPTION;
    if (v[1] < 0 || v[1] >= (1 << QP_MON_EVENTS))
        return qp_raise_value_error(ctx, "invalid event set 0x%x", v[1]);
    if (qp_mon_normalize_events(ctx, &v[1]) < 0)
        return JS_EXCEPTION;
    ms = qp_modstate(ctx);
    if (!ms || qp_mon_check_in_use(ctx, ms, v[0]) < 0)
        return JS_EXCEPTION;
    if (ms->mon_events[v[0]] == (uint32_t)v[1])
        return JS_UNDEFINED;
    ms->mon_events[v[0]] = (uint32_t)v[1];
    if (qp_mon_changed(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* code object argument: its bytecode (TypeError if not a code object) */
static JSFunctionBytecode *qp_mon_code_arg(JSContext *ctx, JSValueConst code)
{
    JSFunctionBytecode *b;

    if (!qp_co_check(code) || !(b = qp_code_bytecode(code)) || !b->py_code) {
        qp_raise_type_error(ctx, "code must be a code object");
        return NULL;
    }
    return b;
}

/* get_local_events(tool_id, code, /) -> int */
QP_MON_FN(qp_mon_get_local_events)
{
    static const int which[] = { 0, -1 };
    JSFunctionBytecode *b;
    int tool_id;

    if (qp_mon_ints(ctx, "get_local_events", args, nargs, 2, which, &tool_id) < 0)
        return JS_EXCEPTION;
    b = qp_mon_code_arg(ctx, args[1]);
    if (!b || qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, qp_mon_local_set(b->py_code->mon, tool_id));
}

/* set_local_events(tool_id, code, event_set, /) */
QP_MON_FN(qp_mon_set_local_events)
{
    static const int which[] = { 0, 2, -1 };
    JSFunctionBytecode *b;
    QPModState *ms;
    QPMonCode *m;
    int v[2];

    if (qp_mon_ints(ctx, "set_local_events", args, nargs, 3, which, v) < 0)
        return JS_EXCEPTION;
    b = qp_mon_code_arg(ctx, args[1]);
    if (!b || qp_mon_check_tool(ctx, v[0]) < 0 || qp_mon_normalize_events(ctx, &v[1]) < 0)
        return JS_EXCEPTION;
    if (v[1] < 0 || v[1] >= (1 << QP_MON_UNGROUPED_EVENTS))
        return qp_raise_value_error(ctx, "invalid local event set 0x%x", v[1]);
    ms = qp_modstate(ctx);
    if (!ms || qp_mon_check_in_use(ctx, ms, v[0]) < 0)
        return JS_EXCEPTION;
    if (qp_mon_local_set(b->py_code->mon, v[0]) == v[1])
        return JS_UNDEFINED;
    m = qp_mon_code_new(ctx, b);
    if (!m)
        return JS_EXCEPTION;
    qp_mon_set_local(ctx->py, m, v[0], v[1]);
    if (qp_mon_changed(ctx) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* restart_events(): the DISABLEd locations report their events again */
QP_MON_FN(qp_mon_restart_events)
{
    ctx->py->mon_restart_version++;
    ctx->py->mon_version++;
    return JS_UNDEFINED;
}

/* _all_events() -> {event name: bitmask of tools} for the global sets */
QP_MON_FN(qp_mon_all_events)
{
    QPModState *ms = qp_modstate(ctx);
    uint8_t g[QP_MON_UNGROUPED_EVENTS];
    JSValue res;
    int e;

    if (!ms)
        return JS_EXCEPTION;
    res = qp_dict_new(ctx);
    if (JS_IsException(res))
        return res;
    qp_mon_global_tools(ms, g);
    for (e = 0; e < QP_MON_UNGROUPED_EVENTS; e++) {
        JSValue name;
        if (!g[e])
            continue;
        name = qp_str_from_cstr(ctx, qp_mon_event_names[e]);
        if (JS_IsException(name) ||
            qp_dict_set_item(ctx, res, name, JS_NewInt32(ctx, g[e])) < 0) {
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, name);
    }
    return res;
}

static const QPMethodDef qp_mon_methods[] = {
    { "use_tool_id", qp_mon_use_tool_id, QP_METH_FASTCALL, 0,
      "use_tool_id($module, tool_id, name, /)\n--\n\n" },
    { "clear_tool_id", qp_mon_clear_tool_id, QP_METH_FASTCALL, 0,
      "clear_tool_id($module, tool_id, /)\n--\n\n" },
    { "free_tool_id", qp_mon_clear_tool_id, QP_METH_FASTCALL, 1,
      "free_tool_id($module, tool_id, /)\n--\n\n" },
    { "get_tool", qp_mon_get_tool, QP_METH_FASTCALL, 0,
      "get_tool($module, tool_id, /)\n--\n\n" },
    { "register_callback", qp_mon_register_callback, QP_METH_FASTCALL, 0,
      "register_callback($module, tool_id, event, func, /)\n--\n\n" },
    { "get_events", qp_mon_get_events, QP_METH_FASTCALL, 0,
      "get_events($module, tool_id, /)\n--\n\n" },
    { "set_events", qp_mon_set_events, QP_METH_FASTCALL, 0,
      "set_events($module, tool_id, event_set, /)\n--\n\n" },
    { "get_local_events", qp_mon_get_local_events, QP_METH_FASTCALL, 0,
      "get_local_events($module, tool_id, code, /)\n--\n\n" },
    { "set_local_events", qp_mon_set_local_events, QP_METH_FASTCALL, 0,
      "set_local_events($module, tool_id, code, event_set, /)\n--\n\n" },
    { "restart_events", qp_mon_restart_events, QP_METH_NOARGS, 0,
      "restart_events($module, /)\n--\n\n" },
    { "_all_events", qp_mon_all_events, QP_METH_NOARGS, 0,
      "_all_events($module, /)\n--\n\n" },
    { NULL },
};

static int qp_mon_set_int(JSContext *ctx, JSValueConst obj, const char *name, int v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;

    if (a == JS_ATOM_NULL)
        return -1;
    r = qp_setattr(ctx, obj, a, JS_NewInt32(ctx, v));
    JS_FreeAtom(ctx, a);
    return r;
}

static int qp_mon_set_obj(JSContext *ctx, JSValueConst obj, const char *name, JSValue v)
{
    JSAtom a;
    int r;

    if (JS_IsException(v))
        return -1;
    a = JS_NewAtom(ctx, name);
    r = a == JS_ATOM_NULL ? -1 : qp_setattr(ctx, obj, a, v);
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return r;
}

/* DISABLE / MISSING: one object each per interpreter (compared by identity) */
static JSValue qp_mon_sentinel(JSContext *ctx, JSValue *slot)
{
    if (QP_IS_NULL(*slot)) {
        JSValue v = qp_object_new_instance(ctx, QP_TYPE(ctx, object), JS_CLASS_PY_OBJECT);
        if (JS_IsException(v))
            return v;
        *slot = v;
    }
    return JS_DupValue(ctx, *slot);
}

static int qp_mon_exec(JSContext *ctx, JSValueConst mod)
{
    QPModState *ms = qp_modstate(ctx);
    JSValue events;
    int e;

    if (!ms ||
        qp_mon_set_obj(ctx, mod, "DISABLE", qp_mon_sentinel(ctx, &ms->mon_disable)) < 0 ||
        qp_mon_set_obj(ctx, mod, "MISSING", qp_mon_sentinel(ctx, &ms->mon_missing)) < 0)
        return -1;
    events = qp_object_new_instance(ctx, QP_TYPE(ctx, SimpleNamespace), JS_CLASS_PY_OBJECT);
    if (JS_IsException(events))
        return -1;
    for (e = 0; e < QP_MON_EVENTS; e++) {
        if (qp_mon_set_int(ctx, events, qp_mon_event_names[e], 1 << e) < 0)
            goto fail;
    }
    if (qp_mon_set_int(ctx, events, "NO_EVENTS", 0) < 0 ||
        qp_mon_set_obj(ctx, mod, "events", JS_DupValue(ctx, events)) < 0)
        goto fail;
    JS_FreeValue(ctx, events);
    if (qp_mon_set_int(ctx, mod, "DEBUGGER_ID", 0) < 0 ||
        qp_mon_set_int(ctx, mod, "COVERAGE_ID", 1) < 0 ||
        qp_mon_set_int(ctx, mod, "PROFILER_ID", 2) < 0 ||
        qp_mon_set_int(ctx, mod, "OPTIMIZER_ID", 5) < 0)
        return -1;
    return 0;
 fail:
    JS_FreeValue(ctx, events);
    return -1;
}

static const QPModuleDef qp_moddef_sys_monitoring = {
    "sys.monitoring",
    NULL,
    qp_mon_methods,
    qp_mon_exec,
};

/* sys.monitoring (_Py_CreateMonitoringObject). */
static JSValue qp_monitoring_create(JSContext *ctx)
{
    return qp_module_from_def(ctx, &qp_moddef_sys_monitoring);
}
