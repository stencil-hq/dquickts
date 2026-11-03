/*
 * dquickts Python (M2): frame objects, locals(), sys._getframe(), super().
 *
 * Derived from CPython 3.15 Objects/frameobject.c, Python/ceval.c
 * (_PyEval_GetFrameLocals) and Objects/typeobject.c
 * (super_init_without_args) (PSF-2.0).
 *
 * Frame objects (PY_FRAME) are snapshots made from a QuickJS stack frame
 * of a Python function (or by the traceback unit from a (function, pc)
 * pair): f_lineno and f_locals are those at creation, f_back is the
 * snapshot of the calling Python frame.  CPython's write-through
 * FrameLocalsProxy is not provided: f_locals of a function frame is a dict
 * snapshot (as locals() returns since PEP 667) that also carries the
 * non-variable keys stored into earlier snapshots (qp_fr_live_locals).
 *
 * Frame layout of Python code (object.h QPCodeInfo): parameters are
 * arg_buf[0..arg_count), locals var_buf[0..var_count) (vardefs[arg_count +
 * i]), cells are captured vars whose value stays in the frame buffer while
 * the frame runs, free variables are the function's var_refs; unbound =
 * JS_UNINITIALIZED.  Code without CO_NEWLOCALS has one hidden parameter:
 * the locals mapping.  Locals of inlined comprehensions (PEP 709) are vars
 * with is_lexical set (CPython CO_FAST_HIDDEN).
 */

/* private helpers of funcobject.c / codeobject.c */
static JSValue qp_fo_get_code(JSContext *ctx, JSValueConst func);
static JSValue qp_co_get_name(JSContext *ctx, JSValueConst code);
static JSValue qp_co_get_filename(JSContext *ctx, JSValueConst code);
static JSValue qp_fo_type_name(JSContext *ctx, QPType *t);

typedef struct QPFrameObject {
    JSValue func;               /* the Python function */
    uint32_t pc;                /* byte offset for qp_code_addr2loc (f_lasti) */
    JSValue back;               /* f_back: frame or None */
    JSValue locals;             /* f_locals snapshot / mapping, QP_NULL = none recorded */
    JSValue trace;              /* f_trace */
    uint8_t trace_lines;        /* f_trace_lines */
    uint8_t trace_opcodes;      /* f_trace_opcodes */
    /* M6: frame object of a live activation (JSStackFrame.py_frame):
       f_lineno, f_lasti, f_locals and f_back read the frame while it
       runs (or is a suspended generator) */
    struct JSStackFrame *sf;    /* the activation, NULL once it ended or for snapshots */
    int lineno;                 /* CPython f_lineno: line of the trace event being reported, 0 = from the pc */
    /* trace state (qp_trace_*, end of this file) */
    int prev_line;              /* line of the last instruction seen by the trace hook, -1 = none yet */
    uint32_t prev_pc;           /* its offset (backward jumps), 0 after an exception */
    uint32_t loc_lo, loc_hi;    /* cached pc range [loc_lo, loc_hi) of loc_line */
    int loc_line;
    uint8_t loc_art;            /* its location is not the code's own (QP_PCLOC_IS_ART) */
    JSValue c_callable;         /* builtin called under the profiler (c_return / c_exception), an object while pending */
    uint32_t c_pc;              /* offset of that call */
    /* Artificial code (compile.c NO_LOCATION) inherits the location of the
       code emitted before it; CPython propagates the location of its
       predecessor instead (or none for exception handlers).  When an
       exception or a forward jump lands on an instruction that does not
       start a location of the table, the trace hook reports no line until
       one that does (QP_FR_NOLINE_*). */
    uint8_t noline;
    uint8_t notrace;            /* runs as C code: QP_CO_NATIVE code or called by it (qp_trace_call) */
    int exc_line;               /* line of the last exception in the frame (CPython RERAISE lasti) */
    uint32_t exc_pc;            /* offset of its instruction */
    uint32_t lasti_pc;          /* f_lasti of the event being reported + 1, 0 = from the pc */
    uint32_t next_pc;           /* offset following prev_pc's instruction */
    uint8_t jump;               /* QP_FR_JUMP_*: f_lineno setter permission of the event being reported */
    uint32_t jump_to;           /* f_lineno setter: target offset + 1, 0 = none */
    uint32_t jump_src;          /* offset of the instruction of the event (the jump's origin) */
} QPFrameObject;

enum {
    QP_FR_NOLINE_EXC = 1,       /* handler entry: no line (prev line -1), unwinds at exc_line */
    QP_FR_NOLINE_JUMP,          /* join point: the line of the predecessor */
};

/* f_lineno setter permission (CPython frame_setlineno, by event) */
enum {
    QP_FR_JUMP_NONE,            /* not in a trace function of this frame */
    QP_FR_JUMP_OK,              /* 'line' */
    QP_FR_JUMP_START,           /* 'call' of a new frame */
    QP_FR_JUMP_CALLING,         /* 'c_call', 'c_return' */
    QP_FR_JUMP_OTHER,           /* other events */
};

static void qp_frame_finalizer(JSRuntime *rt, JSValue val)
{
    QPFrameObject *f = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!f)
        return;
    JS_FreeValueRT(rt, f->func);
    JS_FreeValueRT(rt, f->back);
    JS_FreeValueRT(rt, f->locals);
    JS_FreeValueRT(rt, f->trace);
    JS_FreeValueRT(rt, f->c_callable);
    js_free_rt(rt, f);
    /* a generator frame freed by the cycle collector may still reach its
       frame object (qp_frame_detach) */
    JS_VALUE_GET_OBJ(val)->u.opaque = NULL;
}

static void qp_frame_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPFrameObject *f = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!f)
        return;
    JS_MarkValue(rt, f->func, mark_func);
    JS_MarkValue(rt, f->back, mark_func);
    JS_MarkValue(rt, f->locals, mark_func);
    JS_MarkValue(rt, f->trace, mark_func);
    JS_MarkValue(rt, f->c_callable, mark_func);
}

static QPFrameObject *qp_fr_data(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_FRAME)
        return NULL;
    return JS_VALUE_GET_OBJ(v)->u.opaque;
}

static JSValue qp_frame_new(JSContext *ctx, JSValueConst func, uint32_t pc,
                            JSValueConst back, JSValueConst locals)
{
    QPFrameObject *f;
    JSValue obj;

    if (!qp_function_data(func))
        return qp_raise_system_error(ctx, "frame of a non-Python function");
    f = js_malloc(ctx, sizeof(*f));
    if (!f)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, frame)), JS_CLASS_PY_FRAME);
    if (JS_IsException(obj)) {
        js_free(ctx, f);
        return JS_EXCEPTION;
    }
    f->func = JS_DupValue(ctx, func);
    f->pc = pc;
    f->back = QP_IS_NULL(back) ? JS_UNDEFINED : JS_DupValue(ctx, back);
    f->locals = JS_DupValue(ctx, locals);
    f->trace = JS_UNDEFINED;
    f->trace_lines = 1;
    f->trace_opcodes = 0;
    f->sf = NULL;
    f->lineno = 0;
    f->prev_line = -1;
    f->prev_pc = 0;
    f->loc_lo = f->loc_hi = 0;
    f->loc_art = 0;
    f->loc_line = -1;
    f->c_callable = JS_UNDEFINED;
    f->c_pc = 0;
    f->jump = QP_FR_JUMP_NONE;
    f->jump_to = 0;
    f->jump_src = 0;
    f->notrace = 0;
    f->noline = 0;
    f->exc_line = -1;
    f->exc_pc = 0;
    f->lasti_pc = 0;
    f->next_pc = 0;
    JS_VALUE_GET_OBJ(obj)->u.opaque = f;
    return obj;
}

/* The Python function running in sf, NULL if sf is not a Python frame. */
static JSObject *qp_fr_sf_func(JSStackFrame *sf)
{
    JSObject *p;

    if (!sf || JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION || !p->u.func.function_bytecode ||
        !p->u.func.function_bytecode->is_py || !p->u.func.function_bytecode->py_code)
        return NULL;
    return p;
}

/* Offset inside the instruction being executed by sf (cur_pc points after
   it), for qp_code_addr2loc. */
static uint32_t qp_fr_sf_pc(JSStackFrame *sf, JSFunctionBytecode *b)
{
    const uint8_t *pc = sf->cur_pc;

    if (!pc || pc <= b->byte_code_buf || pc > b->byte_code_buf + b->byte_code_len)
        return 0;
    return (uint32_t)(pc - b->byte_code_buf) - 1;
}

static int qp_fr_set(JSContext *ctx, JSValueConst dict, JSAtom name, JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED || name == JS_ATOM_NULL)
        return 0;
    return qp_dict_set_item_atom(ctx, dict, name, v);
}

/* locals() (PEP 667, _PyEval_GetFrameLocals). */
static JSValue qp_frame_locals(JSContext *ctx, struct JSStackFrame *sf)
{
    JSObject *p = qp_fr_sf_func(sf);
    JSFunctionBytecode *b;
    QPCodeInfo *ci;
    JSValue dict;
    int i;

    if (!p)
        return qp_raise_system_error(ctx, "frame does not exist");
    b = p->u.func.function_bytecode;
    ci = b->py_code;
    if (!(ci->co_flags & CO_NEWLOCALS)) {
        /* module / class body / exec: the locals mapping, unless an inlined
           comprehension is running (_PyFrame_HasHiddenLocals) */
        BOOL hidden = FALSE;
        for (i = 0; i < b->var_count; i++) {
            if (b->vardefs[b->arg_count + i].is_lexical &&
                JS_VALUE_GET_TAG(sf->var_buf[i]) != JS_TAG_UNINITIALIZED) {
                hidden = TRUE;
                break;
            }
        }
        if (!hidden) {
            if (b->arg_count < 1)
                return qp_raise_system_error(ctx, "frame has no locals mapping");
            return JS_DupValue(ctx, sf->arg_buf[0]);
        }
    }
    dict = qp_dict_new(ctx);
    if (JS_IsException(dict))
        return JS_EXCEPTION;
    for (i = 0; i < b->arg_count; i++) {
        if (qp_fr_set(ctx, dict, b->vardefs[i].var_name, sf->arg_buf[i]) < 0)
            goto fail;
    }
    for (i = 0; i < b->var_count; i++) {
        JSBytecodeVarDef *vd = &b->vardefs[b->arg_count + i];
        if (vd->is_lexical)
            continue;
        if (qp_fr_set(ctx, dict, vd->var_name, sf->var_buf[i]) < 0)
            goto fail;
    }
    /* free variables (not copied into class namespaces: CO_OPTIMIZED only) */
    if ((ci->co_flags & CO_OPTIMIZED) && p->u.func.var_refs) {
        for (i = 0; i < b->closure_var_count; i++) {
            JSVarRef *vr = p->u.func.var_refs[i];
            if (vr && qp_fr_set(ctx, dict, b->closure_var[i].var_name, *vr->pvalue) < 0)
                goto fail;
        }
    }
    /* bound inlined-comprehension locals shadow the outer variable of the
       same name (CPython stores them in the same slot) */
    for (i = 0; i < b->var_count; i++) {
        JSBytecodeVarDef *vd = &b->vardefs[b->arg_count + i];
        if (!vd->is_lexical)
            continue;
        if (qp_fr_set(ctx, dict, vd->var_name, sf->var_buf[i]) < 0)
            goto fail;
    }
    return dict;
 fail:
    JS_FreeValue(ctx, dict);
    return JS_EXCEPTION;
}

/* TRUE if name is a parameter, local, cell or free variable of b. */
static BOOL qp_fr_is_var_name(JSFunctionBytecode *b, JSAtom name)
{
    int i;

    for (i = 0; i < b->arg_count + b->var_count; i++) {
        if (b->vardefs[i].var_name == name)
            return TRUE;
    }
    for (i = 0; i < b->closure_var_count; i++) {
        if (b->closure_var[i].var_name == name)
            return TRUE;
    }
    return FALSE;
}

/* f_locals of a live function frame: a snapshot of the variables plus the
   keys that are not variable names stored into an earlier f_locals of the
   frame (CPython keeps them in the frame's f_extra_locals; pdb stores
   '__return__' and '__exception__' this way).  The last snapshot is kept
   in f->locals to carry them.  sf is the activation of f. */
static JSValue qp_fr_live_locals(JSContext *ctx, QPFrameObject *f,
                                 struct JSStackFrame *sf)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
    JSValue d, k, v;
    Py_ssize_t pos = 0;
    int ret;

    d = qp_frame_locals(ctx, sf);
    if (JS_IsException(d) || !(b->py_code->co_flags & CO_NEWLOCALS))
        return d;
    if (qp_dict_check_exact(ctx, f->locals)) {
        while (qp_dict_next(f->locals, &pos, &k, &v)) {
            if (qp_str_check_exact(k)) {
                JSAtom name = JS_ValueToAtom(ctx, k);
                BOOL is_var;
                if (name == JS_ATOM_NULL)
                    goto fail;
                is_var = qp_fr_is_var_name(b, name);
                JS_FreeAtom(ctx, name);
                if (is_var)
                    continue;
            }
            k = JS_DupValue(ctx, k);
            v = JS_DupValue(ctx, v);
            ret = qp_dict_set_item(ctx, d, k, v);
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            if (ret < 0)
                goto fail;
        }
    }
    JS_FreeValue(ctx, f->locals);
    f->locals = JS_DupValue(ctx, d);
    return d;
 fail:
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

/* Line of the instruction at offset pc of b, -1 if none. */
static int qp_fr_line_at(JSFunctionBytecode *b, uint32_t pc)
{
    QPLoc loc;

    if (qp_code_addr2loc(b, pc, &loc) < 0)
        return -1;
    return loc.lineno;
}

/* Frame object of the live Python frame sf, created on first use and kept
   in sf->py_frame for the whole activation (CPython
   _PyFrame_GetFrameObject).  Borrowed; JS_EXCEPTION on error. */
static JSValue qp_frame_of(JSContext *ctx, JSStackFrame *sf)
{
    JSFunctionBytecode *b;
    QPFrameObject *f;
    JSValue obj;
    uint32_t pc;

    if (JS_VALUE_GET_TAG(sf->py_frame) == JS_TAG_OBJECT)
        return sf->py_frame;
    b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    pc = qp_fr_sf_pc(sf, b);
    obj = qp_frame_new(ctx, sf->cur_func, pc, JS_UNDEFINED, QP_NULL);
    if (JS_IsException(obj))
        return obj;
    f = qp_fr_data(obj);
    f->sf = sf;
    if (sf->cur_pc > b->byte_code_buf) {
        /* created mid-frame: the trace hook reports the next line */
        f->prev_line = qp_fr_line_at(b, pc);
        f->prev_pc = pc;
        f->next_pc = (uint32_t)(sf->cur_pc - b->byte_code_buf);
    }
    sf->py_frame = obj;
    return obj;
}

/* TRUE if sf is on the current stack (a generator frame is not while
   suspended). */
static BOOL qp_fr_sf_running(JSContext *ctx, JSStackFrame *sf)
{
    QPThreads *thr = ctx->rt->py_threads;
    struct list_head *el;
    QPThreadState *ts;
    JSStackFrame *s;

    for (s = ctx->rt->current_stack_frame; s; s = s->prev_frame) {
        if (s == sf)
            return TRUE;
    }
    if (!thr)
        return FALSE;
    /* the stacks of the threads waiting for the GIL (sys._current_frames) */
    list_for_each(el, &thr->tstates) {
        ts = list_entry(el, QPThreadState, link);
        if (ts == thr->current)
            continue;
        for (s = ts->js.current_stack_frame; s; s = s->prev_frame) {
            if (s == sf)
                return TRUE;
        }
    }
    return FALSE;
}

/* The next older Python frame of the running frame sf, NULL if none. */
static JSStackFrame *qp_fr_sf_back(JSStackFrame *sf)
{
    for (sf = sf->prev_frame; sf; sf = sf->prev_frame) {
        if (qp_fr_sf_func(sf))
            return sf;
    }
    return NULL;
}

/* f_back of a live frame: the frame object of its Python caller, None
   for a suspended generator. */
static JSValue qp_fr_live_back(JSContext *ctx, QPFrameObject *f)
{
    JSStackFrame *back;

    if (!qp_fr_sf_running(ctx, f->sf))
        return JS_UNDEFINED;
    back = qp_fr_sf_back(f->sf);
    if (!back)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, qp_frame_of(ctx, back));
}

static void qp_frame_detach(JSRuntime *rt, JSContext *ctx, JSStackFrame *sf)
{
    JSValue obj = sf->py_frame, exc, v;
    QPFrameObject *f = qp_fr_data(obj);    /* NULL: already finalized (cycle collection) */
    JSStackFrame *back;
    BOOL uncatchable;

    sf->py_frame = JS_UNDEFINED;
    if (f) {
        JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;

        f->pc = qp_fr_sf_pc(sf, b);
        if (rt->current_exception_is_uncatchable == FALSE &&
            JS_VALUE_GET_TAG(rt->current_exception) == JS_TAG_OBJECT &&
            sf->py_raise_pc < b->byte_code_len) {
            /* ended by an exception: the last instruction that raised
               (CPython's RERAISE n != 0 restored f_lasti to it) */
            f->pc = sf->py_raise_pc;
        }
        f->sf = NULL;
        f->lineno = 0;
        JS_FreeValueRT(rt, f->c_callable);
        f->c_callable = JS_UNDEFINED;
        if (ctx && js_rc(JS_VALUE_GET_OBJ(obj))->ref_count > 1) {
            /* the frame object outlives its activation (CPython
               take_ownership): keep f_locals and f_back */
            exc = JS_GetException(ctx);
            uncatchable = rt->current_exception_is_uncatchable;
            v = qp_fr_live_locals(ctx, f, sf);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            } else {
                JS_FreeValue(ctx, f->locals);
                f->locals = v;
            }
            back = (sf->js_mode & JS_MODE_ASYNC) ? NULL : qp_fr_sf_back(sf);
            if (back) {
                v = qp_frame_of(ctx, back);
                if (JS_IsException(v)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                } else {
                    JS_FreeValue(ctx, f->back);
                    f->back = JS_DupValue(ctx, v);
                }
            }
            if (!JS_IsUninitialized(exc)) {
                rt->current_exception = exc;
                rt->current_exception_is_uncatchable = uncatchable;
            }
        }
    }
    JS_FreeValueRT(rt, obj);
}

static void qp_frame_moved(JSStackFrame *sf)
{
    QPFrameObject *f = qp_fr_data(sf->py_frame);

    if (f)
        f->sf = sf;
}

static JSValue qp_getframe(JSContext *ctx, int depth)
{
    JSStackFrame *sf;

    if (depth < 0)
        depth = 0;
    sf = qp_py_frame_at(ctx, depth);
    if (!sf)
        return qp_raise_value_error(ctx, "call stack is not deep enough");
    return JS_DupValue(ctx, qp_frame_of(ctx, sf));
}

/* super() without arguments (super_init_without_args). */
static int qp_frame_super_args(JSContext *ctx, struct JSStackFrame *sf,
                               JSValue *ptype, JSValue *pself)
{
    JSObject *p = qp_fr_sf_func(sf);
    JSFunctionBytecode *b;
    JSValue firstarg, type = QP_NULL;
    QPType *t;
    int i;

    if (!p) {
        qp_raise_runtime_error(ctx, "super(): no current frame");
        return -1;
    }
    b = p->u.func.function_bytecode;
    if (b->py_code->argcount == 0 || b->arg_count == 0 ||
        b->vardefs[0].var_name == JS_ATOM_NULL) {
        qp_raise_runtime_error(ctx, "super(): no arguments");
        return -1;
    }
    firstarg = sf->arg_buf[0];
    if (JS_VALUE_GET_TAG(firstarg) == JS_TAG_UNINITIALIZED) {
        qp_raise_runtime_error(ctx, "super(): arg[0] deleted");
        return -1;
    }
    for (i = 0; i < b->closure_var_count; i++) {
        JSVarRef *vr;

        if (b->closure_var[i].var_name != QP_ATOM(ctx, __class__))
            continue;
        vr = p->u.func.var_refs ? p->u.func.var_refs[i] : NULL;
        if (!vr) {
            qp_raise_runtime_error(ctx, "super(): bad __class__ cell");
            return -1;
        }
        type = *vr->pvalue;
        if (JS_VALUE_GET_TAG(type) == JS_TAG_UNINITIALIZED) {
            qp_raise_runtime_error(ctx, "super(): empty __class__ cell");
            return -1;
        }
        t = qp_type_from_value(type);
        if (!t) {
            JSValue tn = qp_fo_type_name(ctx, qp_type_of(ctx, type));
            if (JS_IsException(tn))
                return -1;
            qp_raise_runtime_error(ctx, "super(): __class__ is not a type (%U)", tn);
            JS_FreeValue(ctx, tn);
            return -1;
        }
        break;
    }
    if (QP_IS_NULL(type)) {
        qp_raise_runtime_error(ctx, "super(): __class__ cell not found");
        return -1;
    }
    *ptype = JS_DupValue(ctx, type);
    *pself = JS_DupValue(ctx, firstarg);
    return 0;
}

/* ---- frame type ---------------------------------------------------------- */

enum {
    QP_FRA_BACK,
    QP_FRA_LOCALS,
    QP_FRA_LINENO,
    QP_FRA_TRACE,
    QP_FRA_LASTI,
    QP_FRA_GLOBALS,
    QP_FRA_BUILTINS,
    QP_FRA_CODE,
    QP_FRA_TRACE_OPCODES,
    QP_FRA_GENERATOR,
    QP_FRA_TRACE_LINES,
};

/* f_lasti */
static uint32_t qp_fr_lasti(QPFrameObject *f)
{
    if (f->lasti_pc)
        return f->lasti_pc - 1;
    if (f->sf)
        return qp_fr_sf_pc(f->sf, JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode);
    return f->pc;
}

static int qp_code_resume_offset(JSFunctionBytecode *b, uint32_t pc);
static int qp_jump_resume_line(JSContext *ctx, JSFunctionBytecode *b);

static int qp_fr_lineno(QPFrameObject *f)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
    QPLoc loc;

    if (f->lineno > 0)
        return f->lineno;
    if (f->sf && b->realm->py->mon_event_sf == f->sf && b->realm->py->mon_event_line > 0)
        return b->realm->py->mon_event_line;   /* sys.monitoring BRANCH: the branch's line */
    if (f->sf && f->sf->cur_pc <= b->byte_code_buf)
        return qp_jump_resume_line(b->realm, b);   /* not started: the RESUME */
    if (qp_code_addr2loc(b, qp_fr_lasti(f), &loc) == 0)
        return loc.lineno;      /* (-1: no location, f_lineno is None) */
    return b->py_code->firstlineno;
}

static JSValue qp_fr_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPFrameObject *f = qp_fr_data(self);
    QPFuncData *d;

    if (!f)
        return qp_raise_type_error(ctx, "descriptor requires a 'frame' object but received '%T'", self);
    d = qp_function_data(f->func);
    switch (magic) {
    case QP_FRA_BACK:
        if (f->sf)
            return qp_fr_live_back(ctx, f);
        return JS_DupValue(ctx, f->back);
    case QP_FRA_LOCALS:
        if (f->sf)
            return qp_fr_live_locals(ctx, f, f->sf);
        if (QP_IS_NULL(f->locals)) {
            JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
            if (!(b->py_code->co_flags & CO_NEWLOCALS))
                return JS_DupValue(ctx, d->globals);
            f->locals = qp_dict_new(ctx);
            if (JS_IsException(f->locals)) {
                f->locals = QP_NULL;
                return JS_EXCEPTION;
            }
        }
        return JS_DupValue(ctx, f->locals);
    case QP_FRA_LINENO: {
        int lineno = qp_fr_lineno(f);
        return lineno < 0 ? JS_UNDEFINED : JS_NewInt32(ctx, lineno);
    }
    case QP_FRA_TRACE:
        return JS_DupValue(ctx, f->trace);
    case QP_FRA_LASTI: {
        JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
        /* not started ('call' / PY_START): the RESUME ending the prologue */
        if (f->sf && f->sf->cur_pc <= b->byte_code_buf) {
            int off = qp_code_resume_offset(b, 0);
            if (off >= 0)
                return JS_NewInt32(ctx, off);
        }
        return JS_NewInt32(ctx, qp_code_pc2lasti(b, qp_fr_lasti(f)));
    }
    case QP_FRA_GLOBALS:
        return JS_DupValue(ctx, d->globals);
    case QP_FRA_BUILTINS:
        return JS_DupValue(ctx, d->builtins);
    case QP_FRA_CODE:
        return qp_fo_get_code(ctx, f->func);
    case QP_FRA_TRACE_OPCODES:
        return qp_bool_new(f->trace_opcodes);
    case QP_FRA_GENERATOR:
        /* the generator owning the heap frame of a live activation;
           snapshots and frames of ended activations have none */
        if (f->sf && (f->sf->js_mode & JS_MODE_ASYNC)) {
            JSAsyncFunctionState *s = container_of(f->sf, JSAsyncFunctionState, frame);
            if (s->py_gen)
                return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, s->py_gen));
        }
        return JS_UNDEFINED;
    default:
        return qp_bool_new(f->trace_lines);
    }
}

/* ---- frame.f_lineno = n (CPython frame_setlineno) --------------------------
 *
 * The target is a start of the first line >= n that has code; among the
 * starts of that line, the deepest one whose operand stack is compatible
 * with the current one (CPython mark_stacks / compatible_stack: the target
 * stack is the current one or a prefix of it, kind by kind).  The stacks
 * are modeled on the QuickJS bytecode with CPython's kinds (the iterator of
 * a for loop, the exceptions of a handler, objects, NULL) plus the catch
 * offsets of the try blocks (OP_catch), which CPython has no equivalent of:
 * they are invisible to the compatibility rules (jumping into a try block
 * is allowed: in CPython a try block is an exception table range) and
 * qp_trace_jump makes them for the target.
 */
enum {
    QP_JK_ITERATOR = 1,
    QP_JK_EXCEPT = 2,
    QP_JK_OBJECT = 3,
    QP_JK_NULL = 4,
    QP_JK_LASTI = 5,
    QP_JK_MARKER = 6,       /* catch offset of a try block */
};
#define QP_JK_BITS          3
#define QP_JK_UNINITIALIZED (-2)
#define QP_JK_OVERFLOWED    (-1)
#define QP_JK_WILL_OVERFLOW (1ULL << ((63 / QP_JK_BITS - 1) * QP_JK_BITS))

typedef struct QPJumpStacks {
    uint16_t *levels;       /* depth before each pc (compute_stack_size_tabs), 0xffff unreachable */
    int32_t *catches;       /* innermost OP_catch of each pc, -1 none */
    uint8_t *kinds;         /* kinds[pc * width + i]: entry i (bottom first) before pc */
    uint8_t *seen;          /* kinds[pc * width] is set */
    int width;
} QPJumpStacks;

static void qp_jump_stacks_free(JSContext *ctx, QPJumpStacks *js)
{
    js_free(ctx, js->levels);
    js_free(ctx, js->catches);
    js_free(ctx, js->kinds);
    js_free(ctx, js->seen);
}

/* Size and operand counts of the instruction at pos (compute_stack_size_tabs). */
static int qp_jump_insn(const uint8_t *buf, uint32_t pos, int *n_pop, int *n_push)
{
    int op = buf[pos], cnt;

    if (op == OP_py) {
        const QPSubopInfo *si = &qp_subop_info[buf[pos + 1]];
        *n_pop = si->n_pop;
        *n_push = si->n_push;
        switch (si->fmt) {
        case OP_FMT_npop:
            *n_pop += get_u16(buf + pos + 2);
            break;
        case OP_FMT_npop2:
            *n_pop += 2 * get_u16(buf + pos + 2);
            break;
        case OP_FMT_npush:
            *n_push += get_u16(buf + pos + 2);
            break;
        case OP_FMT_unpack_ex:
            cnt = get_u16(buf + pos + 2);
            *n_push += (cnt & 0xff) + (cnt >> 8);
            break;
        default:
            break;
        }
        return si->size;
    } else {
        const JSOpCode *oi = &short_opcode_info(op);
        *n_pop = oi->n_pop;
        *n_push = oi->n_push;
        if (oi->fmt == OP_FMT_npop || oi->fmt == OP_FMT_npop_u16)
            *n_pop += get_u16(buf + pos + 1);
#if SHORT_OPCODES
        else if (oi->fmt == OP_FMT_npopx)
            *n_pop += op - OP_call0;
#endif
        return oi->size;
    }
}

/* kinds k[0..n) flow into pos (the first flow wins at joins) */
static int qp_jump_flow(JSContext *ctx, QPJumpStacks *js, int **work, int *nwork, int *work_size,
                        uint32_t pos, const uint8_t *k, int n)
{
    if (js->seen[pos] || n != js->levels[pos])
        return 0;
    js->seen[pos] = 1;
    memcpy(js->kinds + (size_t)pos * js->width, k, n);
    if (js_resize_array(ctx, (void **)work, sizeof(**work), work_size, *nwork + 1))
        return -1;
    (*work)[(*nwork)++] = pos;
    return 0;
}

/* The kinds of the operand stack entries before every instruction of b
   (CPython mark_stacks).  -1 on error (exception pending). */
static int qp_jump_stacks(JSContext *ctx, JSFunctionBytecode *b, QPJumpStacks *js)
{
    const uint8_t *buf = b->byte_code_buf;
    int len = b->byte_code_len, stack_size, *work = NULL, nwork = 0, work_size = 0;
    uint8_t *k = NULL, *tk;

    memset(js, 0, sizeof(*js));
    if (compute_stack_size_tabs(ctx, buf, len, &stack_size, &js->levels, &js->catches) < 0)
        return -1;
    js->width = stack_size + 1;
    js->kinds = js_malloc(ctx, (size_t)len * js->width);
    js->seen = js_mallocz(ctx, len);
    k = js_malloc(ctx, 2 * (js->width + 2));
    if (!js->kinds || !js->seen || !k)
        goto fail;
    tk = k + js->width + 2;
    if (qp_jump_flow(ctx, js, &work, &nwork, &work_size, 0, k, 0) < 0)
        goto fail;
    while (nwork > 0) {
        uint32_t pos = work[--nwork], next, target = 0;
        int n = js->levels[pos], n_pop, n_push, op = buf[pos], tn = -1, d;
        BOOL falls = TRUE;
        uint8_t c;

        memcpy(k, js->kinds + (size_t)pos * js->width, n);
        next = pos + qp_jump_insn(buf, pos, &n_pop, &n_push);
        if (op == OP_py) {
            int subop = buf[pos + 1], tdelta, flow = qp_subop_flow_kind(subop, &tdelta);
            switch (subop) {
            case QP_OP_push_null:
                k[n++] = QP_JK_NULL;
                break;
            case QP_OP_copy:
                d = buf[pos + 2];
                k[n] = k[n - d];
                n++;
                break;
            case QP_OP_swap:
                d = buf[pos + 2];
                c = k[n - 1];
                k[n - 1] = k[n - d];
                k[n - d] = c;
                break;
            case QP_OP_get_iter:
                k[n - 1] = QP_JK_ITERATOR;
                k[n++] = QP_JK_ITERATOR;
                break;
            case QP_OP_get_aiter:
                k[n - 1] = QP_JK_ITERATOR;
                break;
            case QP_OP_for_iter:
                k[n++] = QP_JK_OBJECT;      /* (exhausted: tdelta drops it) */
                break;
            case QP_OP_end_async_for:
                n -= 2;
                break;
            case QP_OP_push_exc_info:
                k[n - 1] = QP_JK_EXCEPT;
                k[n++] = QP_JK_EXCEPT;
                break;
            case QP_OP_load_method:
                k[n - 1] = QP_JK_OBJECT;
                k[n++] = QP_JK_NULL;
                break;
            default:
                n = max_int(n - n_pop, 0);
                while (n_push-- > 0)
                    k[n++] = QP_JK_OBJECT;
                break;
            }
            if (flow == QP_FLOW_EXIT) {
                falls = FALSE;
            } else if (flow == QP_FLOW_BRANCH) {
                target = pos + 2 + get_u32(buf + pos + 2);
                tn = n + tdelta;
            }
        } else {
            switch (op) {
            case OP_catch:
                /* the handler gets the exception in the marker's slot */
                target = pos + 1 + get_u32(buf + pos + 1);
                memcpy(tk, k, n);
                tk[n] = QP_JK_EXCEPT;
                if (qp_jump_flow(ctx, js, &work, &nwork, &work_size, target, tk, n + 1) < 0)
                    goto fail;
                k[n++] = QP_JK_MARKER;
                break;
            case OP_nip_catch:
                /* catch_offset ... v -> v */
                c = k[n - 1];
                n--;
                while (n > 0 && k[n - 1] != QP_JK_MARKER)
                    n--;
                if (n > 0)
                    k[n - 1] = c;
                break;
            case OP_drop:
                n--;
                break;
            case OP_nip:
                k[n - 2] = k[n - 1];
                n--;
                break;
            case OP_nip1:
                k[n - 3] = k[n - 2];
                k[n - 2] = k[n - 1];
                n--;
                break;
            case OP_dup:
                k[n] = k[n - 1];
                n++;
                break;
            case OP_swap:
                c = k[n - 1];
                k[n - 1] = k[n - 2];
                k[n - 2] = c;
                break;
            case OP_goto:
                target = pos + 1 + get_u32(buf + pos + 1);
                tn = n;
                falls = FALSE;
                break;
#if SHORT_OPCODES
            case OP_goto16:
                target = pos + 1 + (int16_t)get_u16(buf + pos + 1);
                tn = n;
                falls = FALSE;
                break;
            case OP_goto8:
                target = pos + 1 + (int8_t)buf[pos + 1];
                tn = n;
                falls = FALSE;
                break;
            case OP_if_true8:
            case OP_if_false8:
                n--;
                target = pos + 1 + (int8_t)buf[pos + 1];
                tn = n;
                break;
#endif
            case OP_if_true:
            case OP_if_false:
                n--;
                target = pos + 1 + get_u32(buf + pos + 1);
                tn = n;
                break;
            case OP_return:
            case OP_return_undef:
            case OP_return_async:
            case OP_throw:
            case OP_throw_error:
            case OP_tail_call:
            case OP_tail_call_method:
            case OP_ret:
                falls = FALSE;
                break;
            default:
                n = max_int(n - n_pop, 0);
                while (n_push-- > 0)
                    k[n++] = QP_JK_OBJECT;
                break;
            }
        }
        if (tn >= 0 && target < (uint32_t)len &&
            qp_jump_flow(ctx, js, &work, &nwork, &work_size, target, k, tn) < 0)
            goto fail;
        if (falls && next < (uint32_t)len &&
            qp_jump_flow(ctx, js, &work, &nwork, &work_size, next, k, n) < 0)
            goto fail;
    }
    js_free(ctx, work);
    js_free(ctx, k);
    return 0;
 fail:
    js_free(ctx, work);
    js_free(ctx, k);
    qp_jump_stacks_free(ctx, js);
    return -1;
}

/* CPython's int64 encoding of the visible entries of the stack before pc */
static int64_t qp_jump_visible(QPJumpStacks *js, uint32_t pc)
{
    const uint8_t *k = js->kinds + (size_t)pc * js->width;
    int64_t s = 0;
    int i;

    if (!js->seen[pc])
        return QP_JK_UNINITIALIZED;
    for (i = 0; i < js->levels[pc]; i++) {
        if (k[i] == QP_JK_MARKER)
            continue;
        if ((uint64_t)s >= QP_JK_WILL_OVERFLOW)
            return QP_JK_OVERFLOWED;
        s = (s << QP_JK_BITS) | k[i];
    }
    return s;
}

static int qp_jump_compatible_kind(int from, int to)
{
    if (to == 0)
        return 0;
    if (to == QP_JK_OBJECT)
        return from != QP_JK_NULL;
    if (to == QP_JK_NULL)
        return 1;
    return from == to;
}

static int qp_jump_compatible_stack(int64_t from_stack, int64_t to_stack)
{
    const int64_t mask = (1 << QP_JK_BITS) - 1;

    if (from_stack < 0 || to_stack < 0)
        return 0;
    while (from_stack > to_stack)
        from_stack >>= QP_JK_BITS;
    while (from_stack) {
        if (!qp_jump_compatible_kind((int)(from_stack & mask), (int)(to_stack & mask)))
            return 0;
        from_stack >>= QP_JK_BITS;
        to_stack >>= QP_JK_BITS;
    }
    return to_stack == 0;
}

static const char *qp_jump_explain_incompatible(int64_t to_stack)
{
    if (to_stack == QP_JK_OVERFLOWED)
        return "stack is too deep to analyze";
    if (to_stack == QP_JK_UNINITIALIZED)
        return "can't jump into an exception handler, or code may be unreachable";
    switch (to_stack & ((1 << QP_JK_BITS) - 1)) {
    case QP_JK_EXCEPT:
        return "can't jump into an 'except' block as there's no exception";
    case QP_JK_LASTI:
        return "can't jump into a re-raising block as there's no location";
    case QP_JK_ITERATOR:
        return "can't jump into the body of a for loop";
    default:
        return "incompatible stacks";
    }
}

/* Line of the RESUME ending the prologue (line start at pc 0): co_firstlineno,
   0 in a module (qp_co_linetable) */
static int qp_jump_resume_line(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;

    if (!(ci->co_flags & CO_NEWLOCALS) && b->func_name == QP_ATOM(ctx, anon_module))
        return 0;
    return ci->firstlineno;
}

/* The jump is made by qp_trace_jump() when the event's callbacks return
   (frame_setlineno moves instr_ptr; ours moves cur_pc and the operand
   stack then).  Allowed in the events of CPython: 'line' (LINE / JUMP /
   BRANCH), the 'return' of a yield (PY_YIELD), from the frame's trace
   function or the monitoring callbacks of these events. */
static int qp_fr_set_lineno(JSContext *ctx, QPFrameObject *f, JSValueConst value)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
    QPCodeInfo *ci = b->py_code;
    QPJumpStacks js;
    int64_t n, start_stack, target_stack, best_stack = QP_JK_OVERFLOWED;
    int new_lineno = INT_MAX, last, line, err = -1, best = -1, unbound, i;
    uint32_t k, cur;
    const char *msg = "cannot find bytecode for specified line";

    switch (!f->sf ? QP_FR_JUMP_NONE :
            f->jump == QP_FR_JUMP_NONE && qp_mon_jump_allowed(ctx, f->sf) ? QP_FR_JUMP_OK :
            f->jump) {
    case QP_FR_JUMP_OK:
        break;
    case QP_FR_JUMP_START:
        qp_raise_value_error(ctx, "can't jump from the 'call' trace event of a new frame");
        return -1;
    case QP_FR_JUMP_CALLING:
        qp_raise_value_error(ctx, "can't jump during a call");
        return -1;
    case QP_FR_JUMP_OTHER:
        qp_raise_value_error(ctx, "can only jump from a 'line' trace event");
        return -1;
    default:
        qp_raise_value_error(ctx, "f_lineno can only be set in a trace function");
        return -1;
    }
    if (qp_long_as_int64(ctx, value, &n) < 0 || n > INT_MAX || n < INT_MIN) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        qp_raise_value_error(ctx, "lineno out of range");
        return -1;
    }
    if (n < ci->firstlineno) {
        qp_raise_value_error(ctx, "line %d comes before the current code block", (int)n);
        return -1;
    }
    /* first line >= n with a line start (marklines: the RESUME, then each
       change of line, no-location code excluded) */
    last = qp_jump_resume_line(ctx, b);
    if (last >= n)
        new_lineno = last;
    for (k = 0; k < ci->pcloc_count; k++) {
        if (QP_PCLOC_IS_ART(ci->pcloc[k].loc))
            continue;   /* (no location in CPython) */
        line = ci->locs[ci->pcloc[k].loc].lineno;
        if (line != last && line >= 0) {
            if (line >= n && line < new_lineno)
                new_lineno = line;
            last = line;
        }
    }
    if (new_lineno == INT_MAX) {
        qp_raise_value_error(ctx, "line %d comes after the current code block", (int)n);
        return -1;
    }
    if (qp_jump_stacks(ctx, b, &js) < 0)
        return -1;
    /* (a second assignment in the same event starts from the first target) */
    cur = f->jump_to ? f->jump_to - 1 : qp_fr_sf_pc(f->sf, b);
    start_stack = qp_jump_visible(&js, cur);
    last = qp_jump_resume_line(ctx, b);
    for (k = 0; k <= ci->pcloc_count; k++) {
        uint32_t t;
        if (k == 0) {
            line = last;            /* the RESUME */
            t = 0;
        } else {
            if (QP_PCLOC_IS_ART(ci->pcloc[k - 1].loc))
                continue;
            line = ci->locs[ci->pcloc[k - 1].loc].lineno;
            t = ci->pcloc[k - 1].pc;
            if (line == last || line < 0)
                continue;
            last = line;
        }
        if (line != new_lineno || t >= (uint32_t)b->byte_code_len)
            continue;
        target_stack = qp_jump_visible(&js, t);
        if (qp_jump_compatible_stack(start_stack, target_stack)) {
            err = 0;
            if (target_stack > best_stack) {
                best_stack = target_stack;
                best = t;
            }
        } else if (err < 0) {
            if (start_stack == QP_JK_OVERFLOWED) {
                msg = "stack to deep to analyze";
            } else if (start_stack == QP_JK_UNINITIALIZED) {
                msg = "can't jump from unreachable code";
            } else {
                msg = qp_jump_explain_incompatible(target_stack);
                err = 1;
            }
        }
    }
    qp_jump_stacks_free(ctx, &js);
    if (err) {
        qp_raise_value_error(ctx, "%s", msg);
        return -1;
    }
    /* bind None to the unbound locals the target may use (counting every
       one, like CPython; cells always exist there) */
    unbound = 0;
    for (i = 0; i < b->arg_count + b->var_count; i++) {
        JSBytecodeVarDef *vd = &b->vardefs[i];
        JSValue *slot = i < b->arg_count ? &f->sf->arg_buf[i] : &f->sf->var_buf[i - b->arg_count];
        if (vd->var_name != JS_ATOM_NULL && !vd->is_captured &&
            JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED)
            unbound++;
    }
    if (unbound) {
        if (qp_warn(ctx, QP_EXC(ctx, RuntimeWarning), 0, "assigning None to %d unbound local%s",
                    unbound, unbound == 1 ? "" : "s") < 0)
            return -1;
        for (i = 0; i < b->arg_count + b->var_count; i++) {
            JSBytecodeVarDef *vd = &b->vardefs[i];
            JSValue *slot = i < b->arg_count ? &f->sf->arg_buf[i] : &f->sf->var_buf[i - b->arg_count];
            if (vd->var_name != JS_ATOM_NULL && !vd->is_captured &&
                JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED)
                *slot = JS_UNDEFINED;
        }
    }
    if (!f->jump_to)
        f->jump_src = cur;
    f->jump_to = (uint32_t)best + 1;
    f->lineno = new_lineno;     /* f_lineno for the rest of the callback */
    return 0;
}

static int qp_fr_set_attr(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPFrameObject *f = qp_fr_data(self);

    if (!f) {
        qp_raise_type_error(ctx, "descriptor requires a 'frame' object but received '%T'", self);
        return -1;
    }
    switch (magic) {
    case QP_FRA_LINENO:
        if (QP_IS_NULL(value)) {
            qp_raise(ctx, QP_EXC(ctx, AttributeError),
                     "attribute 'f_lineno' of 'frame' objects cannot be deleted");
            return -1;
        }
        if (!qp_long_check_exact(value)) {
            qp_raise_value_error(ctx, "lineno must be an integer");
            return -1;
        }
        return qp_fr_set_lineno(ctx, f, value);
    case QP_FRA_TRACE:
        JS_FreeValue(ctx, f->trace);
        f->trace = QP_IS_NULL(value) ? JS_UNDEFINED : JS_DupValue(ctx, value);
        return 0;
    case QP_FRA_TRACE_OPCODES:
    case QP_FRA_TRACE_LINES:
        if (QP_IS_NULL(value)) {
            /* f_trace_lines: PyMemberDef Py_T_BOOL; f_trace_opcodes: getset */
            if (magic == QP_FRA_TRACE_LINES)
                qp_raise_type_error(ctx, "can't delete numeric/char attribute");
            else
                qp_raise(ctx, QP_EXC(ctx, AttributeError),
                         "attribute 'f_trace_opcodes' of 'frame' objects cannot be deleted");
            return -1;
        }
        if (JS_VALUE_GET_TAG(value) != JS_TAG_BOOL) {
            qp_raise_type_error(ctx, "attribute value type must be bool");
            return -1;
        }
        if (magic == QP_FRA_TRACE_OPCODES)
            f->trace_opcodes = JS_VALUE_GET_BOOL(value);
        else
            f->trace_lines = JS_VALUE_GET_BOOL(value);
        return 0;
    default:
        abort();
    }
}

static JSValue qp_fr_repr(JSContext *ctx, JSValueConst self)
{
    QPFrameObject *f = qp_fr_data(self);
    JSValue code, name, filename, res;

    if (!f)
        return qp_raise_type_error(ctx, "expected a frame object");
    code = qp_fo_get_code(ctx, f->func);
    if (JS_IsException(code))
        return JS_EXCEPTION;
    name = qp_co_get_name(ctx, code);
    filename = qp_co_get_filename(ctx, code);
    JS_FreeValue(ctx, code);
    if (JS_IsException(name) || JS_IsException(filename)) {
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, filename);
        return JS_EXCEPTION;
    }
    res = qp_str_from_format(ctx, "<frame at %p, file %R, line %d, code %S>",
                             JS_VALUE_GET_PTR(self), filename, qp_fr_lineno(f), name);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, filename);
    return res;
}

/* frame.clear(): a frame whose function is executing cannot be cleared;
   otherwise the locals snapshot is dropped. */
static JSValue qp_fr_clear(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    QPFrameObject *f = qp_fr_data(self);
    JSStackFrame *sf;
    JSValue empty;

    if (f->sf) {
        if (qp_fr_sf_running(ctx, f->sf))
            return qp_raise_runtime_error(ctx, "cannot clear an executing frame");
        return qp_raise_runtime_error(ctx, "cannot clear a suspended frame");
    }
    for (sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        if (JS_VALUE_GET_TAG(sf->cur_func) == JS_TAG_OBJECT &&
            JS_VALUE_GET_PTR(sf->cur_func) == JS_VALUE_GET_PTR(f->func))
            return qp_raise_runtime_error(ctx, "cannot clear an executing frame");
    }
    empty = qp_dict_new(ctx);
    if (JS_IsException(empty))
        return JS_EXCEPTION;
    JS_FreeValue(ctx, f->locals);
    f->locals = empty;
    return JS_UNDEFINED;
}

static JSValue qp_fr_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    QPFrameObject *f = qp_fr_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;

    return qp_long_from_int64(ctx, (int64_t)sizeof(JSStackFrame) + sizeof(QPFrameObject) +
                              (int64_t)(b->arg_count + b->var_count + b->stack_size) *
                              sizeof(JSValue));
}

static const QPGetSetDef qp_fr_getsets[] = {
    { "f_back", qp_fr_get, NULL, QP_FRA_BACK, NULL },
    { "f_locals", qp_fr_get, NULL, QP_FRA_LOCALS,
      "Return the mapping used by the frame to look up local variables." },
    { "f_lineno", qp_fr_get, qp_fr_set_attr, QP_FRA_LINENO,
      "Return the current line number in the frame." },
    { "f_trace", qp_fr_get, qp_fr_set_attr, QP_FRA_TRACE,
      "Return the trace function for this frame, or None if no trace function is set." },
    { "f_lasti", qp_fr_get, NULL, QP_FRA_LASTI,
      "Return the index of the last attempted instruction in the frame." },
    { "f_globals", qp_fr_get, NULL, QP_FRA_GLOBALS,
      "Return the global variables in the frame." },
    { "f_builtins", qp_fr_get, NULL, QP_FRA_BUILTINS,
      "Return the built-in variables in the frame." },
    { "f_code", qp_fr_get, NULL, QP_FRA_CODE,
      "Return the code object being executed in this frame." },
    { "f_trace_opcodes", qp_fr_get, qp_fr_set_attr, QP_FRA_TRACE_OPCODES,
      "Return True if opcode tracing is enabled, False otherwise." },
    { "f_generator", qp_fr_get, NULL, QP_FRA_GENERATOR,
      "Return the generator or coroutine associated with this frame, or None." },
    { "f_trace_lines", qp_fr_get, qp_fr_set_attr, QP_FRA_TRACE_LINES, NULL, QP_GETSET_MEMBER },
    { NULL }
};

static const QPMethodDef qp_fr_methods[] = {
    { "clear", qp_fr_clear, QP_METH_NOARGS, 0,
      "clear($self, /)\n--\n\nClear all references held by the frame." },
    { "__sizeof__", qp_fr_sizeof, QP_METH_NOARGS, 0,
      "__sizeof__($self, /)\n--\n\nReturn the size of the frame in memory, in bytes." },
    { NULL }
};

static const QPTypeSpec qp_fr_frame_spec = {
    .name = "frame",
    .doc = NULL,
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_FRAME,
    .slots = {
        .tp_repr = qp_fr_repr,
    },
    .methods = qp_fr_methods,
    .getsets = qp_fr_getsets,
};

/* ==== M6 tracing: sys.settrace / sys.setprofile ============================
 *
 * CPython 3.12+ reports the legacy events through sys.monitoring
 * (Python/legacy_tracing.c) with the trampolines of Python/sysmodule.c.
 * The same event stream is produced here from interpreter hooks
 * (quickjs.c): 'call' at frame start and generator resumption, a hook
 * before every instruction of a traced Python frame (line, opcode,
 * return, c_call, c_return), the exception path ('exception',
 * 'c_exception') and the unwinding of a frame ('return' with None).
 * The per-instruction hook costs nothing while no context traces: the
 * interpreter dispatch table is switched to it by qp_trace_dispatch().
 * Line events follow CPython: an instruction whose line differs from the
 * line of the instruction executed before it in the frame, or any
 * backward jump.  Events are not reported while a trace or profile
 * function runs (tstate->tracing).
 */

enum {
    QP_TRACE_CALL,
    QP_TRACE_EXCEPTION,
    QP_TRACE_LINE,
    QP_TRACE_RETURN,
    QP_TRACE_C_CALL,
    QP_TRACE_C_EXCEPTION,
    QP_TRACE_C_RETURN,
    QP_TRACE_OPCODE,
};

static const char *const qp_trace_whatstrings[] = {
    "call", "exception", "line", "return", "c_call", "c_exception", "c_return", "opcode",
};

#define QP_TRACE_TO_PROFILE 1
#define QP_TRACE_TO_TRACE   2

#define QP_TRACE_IS_SET(v) (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT)

/* Export the interpreter dispatch tables (once), before the first
   qp_trace_dispatch(1). */
static int qp_trace_dispatch_export(JSContext *ctx)
{
#if DIRECT_DISPATCH
    if (!qp_dispatch_table &&
        JS_IsException(JS_CallInternal(ctx, JS_NULL, JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                                       JS_CALL_FLAG_PY_TRACE_SWITCH)))
        return -1;
#endif
    return 0;
}

/* The dispatch goes through the trace hook while it has users: contexts
   with a trace or profile function, tracemalloc (the hook keeps the
   frame's cur_pc current, python/capi_tracemalloc.c). */
static void qp_trace_dispatch(int on)
{
    if (on ? qp_trace_users++ == 0 : --qp_trace_users == 0) {
#if DIRECT_DISPATCH
        int i;
        for (i = 0; i < 256; i++)
            qp_dispatch_table[i] = on ? qp_dispatch_hook : qp_dispatch_table_orig[i];
#endif
    }
}

static int qp_trace_set(JSContext *ctx, int is_profile, JSValueConst func)
{
    QPState *qs = ctx->py;
    JSValue *slot = is_profile ? &qs->profile_func : &qs->trace_func;
    JSValue old = *slot;
    BOOL active;

    if (QP_TRACE_IS_SET(func) && qp_trace_dispatch_export(ctx) < 0)
        return -1;
    *slot = QP_TRACE_IS_SET(func) ? JS_DupValue(ctx, func) : JS_UNDEFINED;
    active = QP_TRACE_IS_SET(qs->trace_func) || QP_TRACE_IS_SET(qs->profile_func);
    if (active != qs->trace_active) {
        qs->trace_active = active;
        qp_trace_dispatch(active);
    }
    JS_FreeValue(ctx, old);
    return 0;
}

/* _PyEval_SetTraceAllThreads / _PyEval_SetProfileAllThreads: the running
   thread, then the saved state of the other threads */
static int qp_trace_set_all(JSContext *ctx, int is_profile, JSValueConst func)
{
    QPThreads *thr = ctx->rt->py_threads;
    struct list_head *el;

    if (qp_trace_set(ctx, is_profile, func) < 0)
        return -1;
    if (!thr)
        return 0;
    list_for_each(el, &thr->tstates) {
        QPThreadState *ts = list_entry(el, QPThreadState, link);
        JSValue *slot = is_profile ? &ts->profile_func : &ts->trace_func;
        JSValue old = *slot;
        BOOL active;
        if (ts == thr->current)
            continue;
        *slot = QP_TRACE_IS_SET(func) ? JS_DupValue(ctx, func) : JS_UNDEFINED;
        active = QP_TRACE_IS_SET(ts->trace_func) || QP_TRACE_IS_SET(ts->profile_func);
        if (active != ts->trace_active) {
            ts->trace_active = active;
            qp_trace_dispatch(active);
        }
        JS_FreeValue(ctx, old);
    }
    return 0;
}

/* Line of the instruction at off, with the pc range of the location
   cached in the frame object. */
static int qp_trace_line(QPFrameObject *f, JSFunctionBytecode *b, uint32_t off)
{
    QPCodeInfo *ci = b->py_code;
    int lo, hi, mid, found = -1;

    if (off >= f->loc_lo && off < f->loc_hi)
        return f->loc_line;
    lo = 0;
    hi = (int)ci->pcloc_count - 1;
    while (lo <= hi) {
        mid = (lo + hi) >> 1;
        if (ci->pcloc[mid].pc <= off) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0) {
        f->loc_lo = 0;
        f->loc_hi = ci->pcloc_count ? ci->pcloc[0].pc : UINT32_MAX;
        f->loc_line = -1;
        f->loc_art = 0;
    } else {
        f->loc_lo = ci->pcloc[found].pc;
        f->loc_hi = (uint32_t)found + 1 < ci->pcloc_count ? ci->pcloc[found + 1].pc : UINT32_MAX;
        f->loc_line = ci->locs[QP_PCLOC_IDX(ci->pcloc[found].loc)].lineno;
        f->loc_art = QP_PCLOC_IS_ART(ci->pcloc[found].loc);
    }
    return f->loc_line;
}

/* Offset of the instruction at pc (the exception paths may pass the byte
   before the first instruction). */
static uint32_t qp_trace_off(JSFunctionBytecode *b, const uint8_t *pc)
{
    return pc > b->byte_code_buf ? (uint32_t)(pc - b->byte_code_buf) : 0;
}

/* One event to the profile (is_profile) or trace function: CPython
   call_trampoline with profile_trampoline / trace_trampoline.  lineno is
   f_lineno during the call (0: from the pc); jump is the f_lineno setter
   permission (QP_FR_JUMP_*).  -1 if the function raised: it is removed
   (and f_trace cleared) like in CPython. */
static int qp_trace_emit(JSContext *ctx, JSValueConst frame, int is_profile, int what,
                         JSValueConst arg, int lineno, int jump)
{
    QPState *qs = ctx->py;
    QPFrameObject *f = qp_fr_data(frame);
    JSValue cb, res, args[3];
    int saved_jump;

    if (is_profile)
        cb = qs->profile_func;
    else
        cb = what == QP_TRACE_CALL ? qs->trace_func : f->trace;
    if (!QP_TRACE_IS_SET(cb))
        return 0;
    args[1] = qp_str_from_cstr(ctx, qp_trace_whatstrings[what]);
    if (JS_IsException(args[1]))
        return -1;
    cb = JS_DupValue(ctx, cb);
    args[0] = (JSValue)frame;
    args[2] = QP_IS_NULL(arg) ? JS_UNDEFINED : (JSValue)arg;
    saved_jump = f->jump;
    f->lineno = lineno;
    f->jump = jump;
    qs->tracing++;
    res = qp_vectorcall(ctx, cb, (JSValueConst *)args, 3, QP_NULL);
    qs->tracing--;
    f->jump = saved_jump;
    f->lineno = 0;
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, cb);
    if (JS_IsException(res)) {
        qp_trace_set(ctx, is_profile, QP_NULL);
        if (!is_profile) {
            JS_FreeValue(ctx, f->trace);
            f->trace = JS_UNDEFINED;
        }
        return -1;
    }
    if (!is_profile && !JS_IsUndefined(res)) {
        JS_FreeValue(ctx, f->trace);
        f->trace = res;
    } else {
        JS_FreeValue(ctx, res);
    }
    return 0;
}

/* The profiler (first, like sys.monitoring tool 6 before tool 7) and the
   trace function for one event. */
static int qp_trace_event(JSContext *ctx, JSValueConst frame, int what, JSValueConst arg,
                          int lineno, int jump, int to)
{
    if ((to & QP_TRACE_TO_PROFILE) &&
        qp_trace_emit(ctx, frame, 1, what, arg, lineno, jump) < 0)
        return -1;
    if ((to & QP_TRACE_TO_TRACE) &&
        qp_trace_emit(ctx, frame, 0, what, arg, lineno, jump) < 0)
        return -1;
    return 0;
}

/* sf runs as C code for tracing: QP_CO_NATIVE code (the Python
   implementation of a C module) or a frame it called, directly or through
   builtins.  Frames of such code report no events, so the stream matches
   CPython's, where the module is C.  Deviation: Python code that the C
   module would call back (codec decoders, methods overridden by a user
   subclass) reports no events either when called from such frames. */
static BOOL qp_trace_is_native(JSStackFrame *sf)
{
    JSObject *p;
    QPFrameObject *f;

    for (; sf; sf = sf->prev_frame) {
        p = qp_fr_sf_func(sf);
        if (!p)
            continue;   /* builtin */
        if (p->u.func.function_bytecode->py_code->co_flags & QP_CO_NATIVE)
            return TRUE;
        if (JS_VALUE_GET_TAG(sf->py_frame) == JS_TAG_OBJECT &&
            (f = qp_fr_data(sf->py_frame)) != NULL && f->sf == sf)
            return f->notrace;  /* decided at its 'call' */
    }
    return FALSE;
}

static int qp_trace_call(JSContext *ctx, JSStackFrame *sf)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    BOOL start = sf->cur_pc <= b->byte_code_buf;
    JSValue frame;
    QPFrameObject *f;
    int r;

    frame = qp_frame_of(ctx, sf);
    if (JS_IsException(frame))
        return -1;
    f = qp_fr_data(frame);
    f->notrace = (b->py_code->co_flags & QP_CO_NATIVE) || qp_trace_is_native(sf->prev_frame);
    if (f->notrace)
        return 0;
    frame = JS_DupValue(ctx, frame);
    /* f_lineno: the RESUME's line at start, the yield on resumption */
    r = qp_trace_event(ctx, frame, QP_TRACE_CALL, JS_UNDEFINED,
                       start ? qp_jump_resume_line(ctx, b) : 0,
                       start ? QP_FR_JUMP_START : QP_FR_JUMP_OTHER,
                       QP_TRACE_TO_PROFILE | QP_TRACE_TO_TRACE);
    JS_FreeValue(ctx, frame);
    return r;
}

/* Callable reported by 'c_call' (CPython sys_profile_call_or_return):
   builtin functions, method descriptors bound to their first argument
   (self_arg QP_NULL: none); JS_UNDEFINED for other callables. */
static JSValue qp_trace_c_callable(JSContext *ctx, JSValueConst callable, JSValueConst self_arg)
{
    QPType *t;

    if (JS_VALUE_GET_TAG(callable) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    switch (JS_VALUE_GET_OBJ(callable)->class_id) {
    case JS_CLASS_PY_CFUNCTION:
        return JS_DupValue(ctx, callable);
    case JS_CLASS_PY_DESCR:
        t = qp_type_of(ctx, callable);
        if (t != QP_TYPE(ctx, method_descriptor) || QP_IS_NULL(self_arg))
            return JS_UNDEFINED;
        return t->slots.tp_descr_get(ctx, callable, self_arg,
                                     QP_TYPE_VALUE(qp_type_of(ctx, self_arg)));
    case JS_CLASS_BYTECODE_FUNCTION: {
        /* a function of a C module implemented in Python (QP_CO_NATIVE) */
        JSFunctionBytecode *b = JS_VALUE_GET_OBJ(callable)->u.func.function_bytecode;
        if (!b || !b->is_py || !b->py_code || !(b->py_code->co_flags & QP_CO_NATIVE))
            return JS_UNDEFINED;
        if (QP_IS_NULL(self_arg))
            return JS_DupValue(ctx, callable);
        t = qp_type_of(ctx, callable);
        return t->slots.tp_descr_get(ctx, callable, self_arg,
                                     QP_TYPE_VALUE(qp_type_of(ctx, self_arg)));
    }
    default:
        return JS_UNDEFINED;
    }
}

/* 'c_call' before a call instruction of the frame; the callable stays
   in the frame object for 'c_return' / 'c_exception'. */
static int qp_trace_c_call(JSContext *ctx, JSValueConst frame, QPFrameObject *f,
                           const uint8_t *pc, uint32_t off, JSValue *sp)
{
    JSValueConst callable, self_arg = QP_NULL;
    JSValue *args, c;
    int argc;

    switch (pc[1]) {
    case QP_OP_call_method:
    case QP_OP_call_kw:
        argc = get_u16(pc + 2);
        args = sp - argc - (pc[1] == QP_OP_call_kw);
        callable = args[-2];
        if (!QP_IS_NULL(args[-1]))
            self_arg = args[-1];
        else if (argc > 0)
            self_arg = args[0];
        break;
    default: /* QP_OP_call_ex: callable, self, args, kwargs */
        callable = sp[-4];
        if (!QP_IS_NULL(sp[-3]))
            self_arg = sp[-3];
        else if (qp_tuple_check_exact(ctx, sp[-2]) && QP_TUPLE_SIZE(sp[-2]) > 0)
            self_arg = QP_TUPLE_ITEMS(sp[-2])[0];
        break;
    }
    c = qp_trace_c_callable(ctx, callable, self_arg);
    if (JS_IsException(c))
        return -1;
    if (JS_IsUndefined(c))
        return 0;
    if (qp_trace_event(ctx, frame, QP_TRACE_C_CALL, c, 0, QP_FR_JUMP_CALLING,
                       QP_TRACE_TO_PROFILE) < 0) {
        JS_FreeValue(ctx, c);
        return -1;
    }
    JS_FreeValue(ctx, f->c_callable);
    f->c_callable = c;
    f->c_pc = off;
    return 0;
}

static int qp_trace_instr(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc, JSValue *sp)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    uint32_t off = (uint32_t)(pc - b->byte_code_buf);
    BOOL profiling = QP_TRACE_IS_SET(qs->profile_func);
    int op = pc[0], subop = op == OP_py ? pc[1] : -1, line, r = 0;
    BOOL is_return = (op == OP_return || op == OP_return_async || subop == QP_OP_yield_value);
    BOOL is_call = (subop == QP_OP_call_method || subop == QP_OP_call_kw ||
                    subop == QP_OP_call_ex);
    JSValue frame = sf->py_frame, c;
    QPFrameObject *f;

    if (JS_VALUE_GET_TAG(frame) != JS_TAG_OBJECT) {
        /* a frame entered before tracing started has no f_trace; the
           profiler still reports its returns and builtin calls */
        if (!profiling || !(is_return || is_call))
            return 0;
        frame = qp_frame_of(ctx, sf);
        if (JS_IsException(frame))
            return -1;
    }
    f = qp_fr_data(frame);
    if (f->notrace)
        return 0;
    frame = JS_DupValue(ctx, frame);

    /* 'c_return' of the builtin call that completed */
    if (QP_TRACE_IS_SET(f->c_callable)) {
        c = f->c_callable;
        f->c_callable = JS_UNDEFINED;
        r = qp_trace_event(ctx, frame, QP_TRACE_C_RETURN, c, qp_fr_line_at(b, f->c_pc),
                           QP_FR_JUMP_CALLING, QP_TRACE_TO_PROFILE);
        JS_FreeValue(ctx, c);
        if (r < 0)
            goto done;
    }

    /* 'line' */
    line = qp_trace_line(f, b, off);
    if (off != f->loc_lo && off > f->prev_pc && off != f->next_pc && !f->noline)
        f->noline = QP_FR_NOLINE_JUMP;  /* forward jump to artificial code */
    else if (off <= f->prev_pc && f->noline == QP_FR_NOLINE_JUMP)
        f->noline = 0;                  /* backward jump: a loop head */
    else if (f->loc_art && off > f->prev_pc && !f->noline)
        f->noline = QP_FR_NOLINE_JUMP;  /* code without a location of its own */
    if (f->noline) {
        if (off == f->loc_lo && line >= 0 && !f->loc_art)
            f->noline = 0;      /* an instruction with its own location */
        else if (f->noline == QP_FR_NOLINE_JUMP)
            line = f->prev_line;
        else
            line = -1;
    }
    {
        JSOpCode tmp;
        int opnd;
        f->next_pc = off + js_get_op_info(b->byte_code_buf, b->byte_code_len, off, TRUE,
                                          &tmp, &opnd)->size;
    }
    /* a new line, or a backward jump except the loops of yield from /
       await back to their send (CPython JUMP_BACKWARD_NO_INTERRUPT) */
    if (line >= 0 && (line != f->prev_line ||
                      (off <= f->prev_pc && subop != QP_OP_send && !f->noline))) {
        f->prev_line = line;
        f->prev_pc = off;
        if (f->trace_lines && QP_TRACE_IS_SET(qs->trace_func) && QP_TRACE_IS_SET(f->trace)) {
            r = qp_trace_emit(ctx, frame, 0, QP_TRACE_LINE, JS_UNDEFINED, line, QP_FR_JUMP_OK);
            if (r < 0) {
                f->jump_to = 0;
                goto done;
            }
            if (f->jump_to) {
                /* frame.f_lineno = n */
                r = 1;
                goto done;
            }
        }
    } else {
        f->prev_line = line;
        f->prev_pc = off;
    }

    /* 'opcode' (QuickJS instructions) */
    if (f->trace_opcodes && QP_TRACE_IS_SET(qs->trace_func) && QP_TRACE_IS_SET(f->trace)) {
        r = qp_trace_emit(ctx, frame, 0, QP_TRACE_OPCODE, JS_UNDEFINED, 0, QP_FR_JUMP_OTHER);
        if (r < 0)
            goto done;
    }

    if (is_return) {
        /* the 'return' of a yield (PY_YIELD) can jump */
        r = qp_trace_event(ctx, frame, QP_TRACE_RETURN, sp[-1],
                           f->noline == QP_FR_NOLINE_JUMP ? f->prev_line : 0,
                           subop == QP_OP_yield_value ? QP_FR_JUMP_OK : QP_FR_JUMP_OTHER,
                           QP_TRACE_TO_PROFILE | QP_TRACE_TO_TRACE);
        if (r < 0)
            f->jump_to = 0;
        else if (f->jump_to)
            r = 1;
    } else if (is_call && profiling) {
        r = qp_trace_c_call(ctx, frame, f, pc, off, sp);
    }
 done:
    JS_FreeValue(ctx, frame);
    return r;
}

static void qp_trace_exception(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc,
                               BOOL reraise)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    QPFrameObject *f = qp_fr_data(sf->py_frame);
    JSValue frame, exc, c, tuple, items[3];
    BOOL report;
    int r = 0;

    if (!f || f->notrace)
        return;     /* no f_trace and no profiled call, or C code */
    f->prev_pc = 0; /* an exception is not a jump */
    if (!f->noline) {
        f->exc_pc = qp_trace_off(b, pc);
        f->exc_line = qp_fr_line_at(b, f->exc_pc);
    } else if (f->noline == QP_FR_NOLINE_JUMP) {
        f->exc_pc = qp_trace_off(b, pc);
        f->exc_line = f->prev_line;
    }
    f->noline = QP_FR_NOLINE_EXC;   /* entering a handler (or unwinding) */
    report = !reraise && QP_TRACE_IS_SET(qs->trace_func) && QP_TRACE_IS_SET(f->trace);
    if (!report && !QP_TRACE_IS_SET(f->c_callable))
        return;
    frame = JS_DupValue(ctx, sf->py_frame);
    exc = JS_GetException(ctx);
    if (QP_TRACE_IS_SET(f->c_callable)) {
        c = f->c_callable;
        f->c_callable = JS_UNDEFINED;
        r = qp_trace_event(ctx, frame, QP_TRACE_C_EXCEPTION, c, qp_fr_line_at(b, f->c_pc),
                           QP_FR_JUMP_OTHER, QP_TRACE_TO_PROFILE);
        JS_FreeValue(ctx, c);
    }
    if (r == 0 && report && qp_exc_check(ctx, exc)) {
        items[0] = QP_TYPE_VALUE(qp_type_of(ctx, exc));
        items[1] = exc;
        items[2] = qp_exc_get_traceback(ctx, exc);
        tuple = qp_tuple_pack(ctx, 3, (JSValueConst *)items);
        JS_FreeValue(ctx, items[2]);
        if (JS_IsException(tuple)) {
            r = -1;
        } else {
            r = qp_trace_emit(ctx, frame, 0, QP_TRACE_EXCEPTION, tuple, f->exc_line,
                              QP_FR_JUMP_OTHER);
            JS_FreeValue(ctx, tuple);
        }
    }
    if (r < 0)
        JS_FreeValue(ctx, exc);     /* replaced by the trace function's exception */
    else
        JS_Throw(ctx, exc);
    JS_FreeValue(ctx, frame);
}

static void qp_trace_unwind(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    QPFrameObject *f = qp_fr_data(sf->py_frame);
    JSValue frame, exc;
    int to = 0, r;

    if (f && f->notrace)
        return;

    if (QP_TRACE_IS_SET(qs->profile_func))
        to |= QP_TRACE_TO_PROFILE;
    if (f && QP_TRACE_IS_SET(qs->trace_func) && QP_TRACE_IS_SET(f->trace))
        to |= QP_TRACE_TO_TRACE;
    if (!to)
        return;
    exc = JS_GetException(ctx);
    frame = qp_frame_of(ctx, sf);
    if (JS_IsException(frame)) {
        JS_FreeValue(ctx, exc);
        return;
    }
    frame = JS_DupValue(ctx, frame);
    /* in a handler: the instruction of the exception (CPython RERAISE
       restores its lasti) */
    if (f && f->noline)
        f->lasti_pc = f->exc_pc + 1;
    r = qp_trace_event(ctx, frame, QP_TRACE_RETURN, JS_UNDEFINED,
                       f && f->noline ? f->exc_line : qp_fr_line_at(b, qp_trace_off(b, pc)),
                       QP_FR_JUMP_OTHER, to);
    if (f)
        f->lasti_pc = 0;
    if (r < 0)
        JS_FreeValue(ctx, exc);
    else
        JS_Throw(ctx, exc);
    JS_FreeValue(ctx, frame);
}

static JSValue *qp_trace_jump(JSContext *ctx, JSStackFrame *sf, JSValue *sp)
{
    QPFrameObject *f = qp_fr_data(sf->py_frame);
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    uint32_t t = f->jump_to - 1, src = f->jump_src;
    QPJumpStacks js;
    const uint8_t *S, *T;
    JSValue *base, *nv;
    uint8_t *used;
    int32_t *chain;
    int ls, lt, p, i, j, nm, mi;

    f->jump_to = 0;
    f->jump_src = 0;
    if (qp_jump_stacks(ctx, b, &js) < 0)
        return NULL;
    ls = js.levels[src];
    lt = js.levels[t];
    S = js.kinds + (size_t)src * js.width;
    T = js.kinds + (size_t)t * js.width;
    base = sp - ls;
    nv = js_malloc(ctx, sizeof(*nv) * (lt + 1) + ls + 1 + sizeof(*chain) * (lt + 1));
    if (!nv) {
        qp_jump_stacks_free(ctx, &js);
        return NULL;
    }
    chain = (int32_t *)(nv + lt + 1);
    used = (uint8_t *)(chain + lt + 1);
    memset(used, 0, ls);
    /* the try blocks of the target, outermost first: its markers */
    nm = 0;
    for (i = js.catches[t]; i >= 0 && nm <= lt; i = js.catches[i])
        nm++;
    mi = nm;
    for (i = js.catches[t]; i >= 0 && mi > 0; i = js.catches[i])
        chain[--mi] = i;
    /* common prefix: kept in place (the same try blocks for markers) */
    mi = 0;
    for (p = 0; p < ls && p < lt && S[p] == T[p]; p++) {
        if (T[p] == QP_JK_MARKER) {
            if (mi >= nm || JS_VALUE_GET_TAG(base[p]) != JS_TAG_CATCH_OFFSET ||
                JS_VALUE_GET_INT(base[p]) != (int)(chain[mi] + 1 + get_u32(b->byte_code_buf + chain[mi] + 1)))
                break;
            mi++;
        }
        nv[p] = base[p];
        used[p] = 1;
    }
    /* above it: the target's visible entries are the current visible ones
       in order (CPython keeps the bottom of the stack), markers are made
       for the target's try blocks */
    j = p;
    for (i = p; i < lt; i++) {
        if (T[i] == QP_JK_MARKER) {
            int32_t c = mi < nm ? chain[mi++] : -1;
            nv[i] = c < 0 ? JS_UNDEFINED :
                JS_NewCatchOffset(ctx, c + 1 + get_u32(b->byte_code_buf + c + 1));
        } else {
            while (j < ls && S[j] == QP_JK_MARKER)
                j++;
            if (j < ls) {
                nv[i] = base[j];
                used[j++] = 1;
            } else {
                nv[i] = JS_UNDEFINED;   /* (cannot happen: compatible stacks) */
            }
        }
    }
    /* the rest is popped, top first; an exception of a handler restores
       the handled exception (POP_EXCEPT) */
    for (i = ls - 1; i >= 0; i--) {
        if (used[i])
            continue;
        if (S[i] == QP_JK_EXCEPT)
            qp_exc_info_pop(ctx, base[i]);
        else
            JS_FreeValue(ctx, base[i]);
    }
    for (i = 0; i < lt; i++)
        base[i] = nv[i];
    sp = base + lt;
    js_free(ctx, nv);
    qp_jump_stacks_free(ctx, &js);
    /* no 'line' event for the target line (CPython) */
    f->prev_line = qp_trace_line(f, b, t);
    f->prev_pc = t;
    f->next_pc = t;
    f->noline = 0;
    sf->cur_pc = b->byte_code_buf + t;
    return sp;
}

/* 'exception' of an exception not raised through the exception path
   (CPython RAISE from FOR_ITER, STOP_ITERATION from END_SEND): reported
   to the trace function of sf (if any).  -1 if it raised. */
static int qp_trace_raise(JSContext *ctx, JSStackFrame *sf, JSValueConst exc)
{
    QPState *qs = ctx->py;
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    QPFrameObject *f = qp_fr_data(sf->py_frame);
    JSValue frame, tuple, items[3];
    int r;

    if (!f || qs->tracing || !QP_TRACE_IS_SET(qs->trace_func) || !QP_TRACE_IS_SET(f->trace))
        return 0;
    items[0] = QP_TYPE_VALUE(qp_type_of(ctx, exc));
    items[1] = (JSValue)exc;
    items[2] = qp_exc_get_traceback(ctx, exc);
    tuple = qp_tuple_pack(ctx, 3, (JSValueConst *)items);
    JS_FreeValue(ctx, items[2]);
    if (JS_IsException(tuple))
        return -1;
    frame = JS_DupValue(ctx, sf->py_frame);
    r = qp_trace_emit(ctx, frame, 0, QP_TRACE_EXCEPTION, tuple,
                      qp_fr_line_at(b, qp_fr_sf_pc(sf, b)), QP_FR_JUMP_OTHER);
    JS_FreeValue(ctx, frame);
    JS_FreeValue(ctx, tuple);
    return r;
}

/* send subop: the generator / coroutine receiver of `yield from` /
   `await` returned value (CPython INSTRUMENTED_END_SEND). */
static int qp_trace_send_return(JSContext *ctx, JSStackFrame *sf, JSValueConst receiver,
                                JSValueConst value)
{
    JSValue exc;
    int r;

    if (!qp_gen_check(receiver) && !qp_coro_check(receiver))
        return 0;
    /* PyErr_SetObject(PyExc_StopIteration, value) */
    if (JS_IsUndefined(value))
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 0, NULL);
    else if (qp_tuple_check(ctx, value))
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), QP_TUPLE_SIZE(value),
                         (JSValueConst *)QP_TUPLE_ITEMS(value));
    else
        exc = qp_exc_new(ctx, QP_EXC(ctx, StopIteration), 1, &value);
    if (JS_IsException(exc))
        return -1;
    r = qp_trace_raise(ctx, sf, exc);
    JS_FreeValue(ctx, exc);
    return r;
}

/* qp_next(): the iterator raised StopIteration (pending); reported when
   the running Python frame is at a for_iter (CPython FOR_ITER
   _PyEval_MonitorRaise).  -1 if the trace function raised (its exception
   replaced the StopIteration). */
static int qp_trace_for_iter_stop(JSContext *ctx)
{
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    JSFunctionBytecode *b;
    JSValue exc;
    int r;

    if (!qp_fr_sf_func(sf) || JS_VALUE_GET_TAG(sf->py_frame) != JS_TAG_OBJECT)
        return 0;
    b = JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
    if (sf->cur_pc < b->byte_code_buf + 2 || sf->cur_pc[-2] != OP_py ||
        sf->cur_pc[-1] != QP_OP_for_iter)
        return 0;
    exc = JS_GetException(ctx);
    r = qp_trace_raise(ctx, sf, exc);
    if (r < 0)
        JS_FreeValue(ctx, exc);
    else
        JS_Throw(ctx, exc);
    return r;
}
