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
 * snapshot (as locals() returns since PEP 667).
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
} QPFrameObject;

static void qp_frame_finalizer(JSRuntime *rt, JSValue val)
{
    QPFrameObject *f = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!f)
        return;
    JS_FreeValueRT(rt, f->func);
    JS_FreeValueRT(rt, f->back);
    JS_FreeValueRT(rt, f->locals);
    JS_FreeValueRT(rt, f->trace);
    js_free_rt(rt, f);
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

/* Frame objects for sf and every older Python frame (f_back chain). */
static JSValue qp_fr_from_stack(JSContext *ctx, JSStackFrame *sf)
{
    JSStackFrame *s, **frames;
    JSValue back = JS_UNDEFINED, f, locals;
    int n = 0, i;

    for (s = sf; s; s = s->prev_frame) {
        if (qp_fr_sf_func(s))
            n++;
    }
    if (n == 0)
        return qp_raise_value_error(ctx, "call stack is not deep enough");
    frames = js_malloc(ctx, sizeof(frames[0]) * n);
    if (!frames)
        return JS_EXCEPTION;
    n = 0;
    for (s = sf; s; s = s->prev_frame) {
        if (qp_fr_sf_func(s))
            frames[n++] = s;
    }
    for (i = n - 1; i >= 0; i--) {
        JSObject *p = qp_fr_sf_func(frames[i]);
        locals = qp_frame_locals(ctx, frames[i]);
        if (JS_IsException(locals))
            goto fail;
        f = qp_frame_new(ctx, frames[i]->cur_func,
                         qp_fr_sf_pc(frames[i], p->u.func.function_bytecode), back, locals);
        JS_FreeValue(ctx, locals);
        if (JS_IsException(f))
            goto fail;
        JS_FreeValue(ctx, back);
        back = f;
    }
    js_free(ctx, frames);
    return back;
 fail:
    js_free(ctx, frames);
    JS_FreeValue(ctx, back);
    return JS_EXCEPTION;
}

static JSValue qp_getframe(JSContext *ctx, int depth)
{
    JSStackFrame *sf;

    if (depth < 0)
        depth = 0;
    sf = qp_py_frame_at(ctx, depth);
    if (!sf)
        return qp_raise_value_error(ctx, "call stack is not deep enough");
    return qp_fr_from_stack(ctx, sf);
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

static int qp_fr_lineno(QPFrameObject *f)
{
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(f->func)->u.func.function_bytecode;
    QPLoc loc;

    if (qp_code_addr2loc(b, f->pc, &loc) == 0 && loc.lineno >= 0)
        return loc.lineno;
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
        return JS_DupValue(ctx, f->back);
    case QP_FRA_LOCALS:
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
    case QP_FRA_LINENO:
        return JS_NewInt32(ctx, qp_fr_lineno(f));
    case QP_FRA_TRACE:
        return JS_DupValue(ctx, f->trace);
    case QP_FRA_LASTI:
        return JS_NewInt32(ctx, (int32_t)f->pc);
    case QP_FRA_GLOBALS:
        return JS_DupValue(ctx, d->globals);
    case QP_FRA_BUILTINS:
        return JS_DupValue(ctx, d->builtins);
    case QP_FRA_CODE:
        return qp_fo_get_code(ctx, f->func);
    case QP_FRA_TRACE_OPCODES:
        return qp_bool_new(f->trace_opcodes);
    case QP_FRA_GENERATOR:
        /* snapshots are not linked to their generator */
        return JS_UNDEFINED;
    default:
        return qp_bool_new(f->trace_lines);
    }
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
            qp_raise_type_error(ctx, "cannot delete attribute");
            return -1;
        }
        if (!qp_long_check_exact(value)) {
            qp_raise_value_error(ctx, "lineno must be an integer");
            return -1;
        }
        /* no trace function can be running: sys.settrace is not provided */
        qp_raise_value_error(ctx, "f_lineno can only be set in a trace function");
        return -1;
    case QP_FRA_TRACE:
        JS_FreeValue(ctx, f->trace);
        f->trace = QP_IS_NULL(value) ? JS_UNDEFINED : JS_DupValue(ctx, value);
        return 0;
    case QP_FRA_TRACE_OPCODES:
    case QP_FRA_TRACE_LINES:
        if (QP_IS_NULL(value)) {
            qp_raise_type_error(ctx, "cannot delete attribute");
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
    { "f_trace_lines", qp_fr_get, qp_fr_set_attr, QP_FRA_TRACE_LINES, NULL },
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
