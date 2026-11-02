/*
 * sys.monitoring (PEP 669; CPython Python/instrumentation.c, the module
 * part): the tool-id registry, per-tool callbacks, global and per-code
 * event sets, and the events namespace.
 *
 * Deviation: the interpreter does not fire monitoring events (no
 * instrumented instructions), so registered callbacks are stored and
 * returned by register_callback() but never called.
 *
 * State lives in QPModState (modstate.h): mon_tool_names[] (QP_NULL = free),
 * mon_callbacks (dict (tool_id << 8 | event_id) -> callable), mon_events[]
 * (global event set per tool) and mon_local_events (dict
 * (code, tool_id) -> event set).
 */

#define QP_MON_TOOLS             6   /* PY_MONITORING_TOOL_IDS: 0..5 usable, 6/7 sys.settrace/setprofile */
#define QP_MON_EVENTS            19  /* _PY_MONITORING_EVENTS */
#define QP_MON_UNGROUPED_EVENTS  18  /* _PY_MONITORING_UNGROUPED_EVENTS */
#define QP_MON_EV_C_RETURN       16
#define QP_MON_EV_C_RAISE        17
#define QP_MON_EV_BRANCH         18
#define QP_MON_EV_BRANCH_LEFT    8
#define QP_MON_EV_BRANCH_RIGHT   9
#define QP_MON_EV_CALL           4
#define QP_MON_C_RETURN_EVENTS   ((1 << QP_MON_EV_C_RETURN) | (1 << QP_MON_EV_C_RAISE))
#define QP_MON_C_CALL_EVENTS     (QP_MON_C_RETURN_EVENTS | (1 << QP_MON_EV_CALL))

static const char *const qp_mon_event_names[QP_MON_EVENTS] = {
    "PY_START", "PY_RESUME", "PY_RETURN", "PY_YIELD", "CALL", "LINE",
    "INSTRUCTION", "JUMP", "BRANCH_LEFT", "BRANCH_RIGHT", "STOP_ITERATION",
    "RAISE", "EXCEPTION_HANDLED", "PY_UNWIND", "PY_THROW", "RERAISE",
    "C_RETURN", "C_RAISE", "BRANCH",
};

#define QP_MON_FN(name) \
    static JSValue name(JSContext *ctx, JSValueConst self, JSValueConst *args, \
                        int nargs, JSValueConst kwnames, int magic)

/* Positional int arguments args[0..n) of fname into out[]. */
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

static QPModState *qp_mon_state(JSContext *ctx)
{
    QPModState *ms = qp_modstate(ctx);

    if (!ms)
        return NULL;
    if (QP_IS_NULL(ms->mon_callbacks)) {
        JSValue d = qp_dict_new(ctx);
        if (JS_IsException(d))
            return NULL;
        ms->mon_callbacks = d;
    }
    if (QP_IS_NULL(ms->mon_local_events)) {
        JSValue d = qp_dict_new(ctx);
        if (JS_IsException(d))
            return NULL;
        ms->mon_local_events = d;
    }
    return ms;
}

/* _PyMonitoring_ClearToolId: events, local events and callbacks of tool. */
static int qp_mon_clear_tool(JSContext *ctx, QPModState *ms, int tool_id)
{
    JSValue keys, key, tup;
    Py_ssize_t pos = 0;
    JSValue k, v;
    int e, n = 0, i, ret = 0;

    ms->mon_events[tool_id] = 0;
    for (e = 0; e < QP_MON_EVENTS; e++) {
        key = JS_NewInt32(ctx, (tool_id << 8) | e);
        if (qp_dict_del_item(ctx, ms->mon_callbacks, key) < 0)
            return -1;
    }
    /* local event sets of this tool */
    keys = qp_list_new(ctx, 0);
    if (JS_IsException(keys))
        return -1;
    while (qp_dict_next(ms->mon_local_events, &pos, &k, &v)) {
        int t;
        if (qp_arg_int(ctx, QP_TUPLE_ITEMS(k)[1], &t) == 0 && t == tool_id) {
            if (qp_list_append(ctx, keys, k) < 0) {
                ret = -1;
                break;
            }
            n++;
        }
    }
    tup = ret == 0 ? qp_list_as_tuple(ctx, keys) : JS_EXCEPTION;
    JS_FreeValue(ctx, keys);
    if (JS_IsException(tup))
        return -1;
    for (i = 0; ret == 0 && i < n; i++) {
        if (qp_dict_del_item(ctx, ms->mon_local_events, QP_TUPLE_ITEMS(tup)[i]) < 0)
            ret = -1;
    }
    JS_FreeValue(ctx, tup);
    return ret;
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
    ms = qp_mon_state(ctx);
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
    ms = qp_mon_state(ctx);
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
    ms = qp_mon_state(ctx);
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
    JSValue key, old;

    if (qp_mon_ints(ctx, "register_callback", args, nargs, 3, which, v) < 0 ||
        qp_mon_check_tool(ctx, v[0]) < 0)
        return JS_EXCEPTION;
    if (v[1] <= 0 || (v[1] & (v[1] - 1)) != 0)
        return qp_raise_value_error(ctx, "The callback can only be set for one event at a time");
    for (event_id = 0; (1 << event_id) != v[1]; event_id++)
        ;
    if (event_id >= QP_MON_EVENTS)
        return qp_raise_value_error(ctx, "invalid event %d", v[1]);
    ms = qp_mon_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    key = JS_NewInt32(ctx, (v[0] << 8) | event_id);
    if (qp_dict_pop(ctx, ms->mon_callbacks, key, &old) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(args[2]) &&
        qp_dict_set_item(ctx, ms->mon_callbacks, key, args[2]) < 0) {
        JS_FreeValue(ctx, old);
        return JS_EXCEPTION;
    }
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
    ms = qp_mon_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)ms->mon_events[tool_id]);
}

/* C_RETURN/C_RAISE only with CALL; BRANCH = BRANCH_LEFT | BRANCH_RIGHT. */
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
    ms = qp_mon_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (QP_IS_NULL(ms->mon_tool_names[v[0]]))
        return qp_raise_value_error(ctx, "tool %d is not in use", v[0]);
    ms->mon_events[v[0]] = (uint32_t)v[1];
    return JS_UNDEFINED;
}

/* (code, tool_id) key of mon_local_events; code must be a code object. */
static JSValue qp_mon_local_key(JSContext *ctx, JSValueConst code, int tool_id)
{
    JSValue items[2];

    items[0] = JS_DupValue(ctx, code);
    items[1] = JS_NewInt32(ctx, tool_id);
    return qp_tuple_new_steal(ctx, 2, items);
}

/* get_local_events(tool_id, code, /) -> int */
QP_MON_FN(qp_mon_get_local_events)
{
    static const int which[] = { 0, -1 };
    QPModState *ms;
    JSValue key, val;
    int tool_id, r, e = 0;

    if (qp_mon_ints(ctx, "get_local_events", args, nargs, 2, which, &tool_id) < 0)
        return JS_EXCEPTION;
    if (!qp_co_check(args[1]))
        return qp_raise_type_error(ctx, "code must be a code object");
    if (qp_mon_check_tool(ctx, tool_id) < 0)
        return JS_EXCEPTION;
    ms = qp_mon_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    key = qp_mon_local_key(ctx, args[1], tool_id);
    if (JS_IsException(key))
        return key;
    r = qp_dict_get_item(ctx, ms->mon_local_events, key, &val);
    JS_FreeValue(ctx, key);
    if (r < 0)
        return JS_EXCEPTION;
    if (r > 0) {
        r = qp_arg_int(ctx, val, &e);
        JS_FreeValue(ctx, val);
        if (r < 0)
            return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, e);
}

/* set_local_events(tool_id, code, event_set, /) */
QP_MON_FN(qp_mon_set_local_events)
{
    static const int which[] = { 0, 2, -1 };
    QPModState *ms;
    JSValue key, val;
    int v[2], r;

    if (qp_mon_ints(ctx, "set_local_events", args, nargs, 3, which, v) < 0)
        return JS_EXCEPTION;
    if (!qp_co_check(args[1]))
        return qp_raise_type_error(ctx, "code must be a code object");
    if (qp_mon_check_tool(ctx, v[0]) < 0 || qp_mon_normalize_events(ctx, &v[1]) < 0)
        return JS_EXCEPTION;
    if (v[1] < 0 || v[1] >= (1 << QP_MON_UNGROUPED_EVENTS))
        return qp_raise_value_error(ctx, "invalid local event set 0x%x", v[1]);
    ms = qp_mon_state(ctx);
    if (!ms)
        return JS_EXCEPTION;
    if (QP_IS_NULL(ms->mon_tool_names[v[0]]))
        return qp_raise_value_error(ctx, "tool %d is not in use", v[0]);
    key = qp_mon_local_key(ctx, args[1], v[0]);
    if (JS_IsException(key))
        return key;
    if (v[1] == 0) {
        r = qp_dict_del_item(ctx, ms->mon_local_events, key);
    } else {
        val = JS_NewInt32(ctx, v[1]);
        r = qp_dict_set_item(ctx, ms->mon_local_events, key, val);
    }
    JS_FreeValue(ctx, key);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* restart_events(): no event is ever disabled (events are not fired). */
QP_MON_FN(qp_mon_restart_events)
{
    return JS_UNDEFINED;
}

/* _all_events() -> {event name: bitmask of tools} for the global sets */
QP_MON_FN(qp_mon_all_events)
{
    QPModState *ms = qp_mon_state(ctx);
    JSValue res;
    int e, t;

    if (!ms)
        return JS_EXCEPTION;
    res = qp_dict_new(ctx);
    if (JS_IsException(res))
        return res;
    for (e = 0; e < QP_MON_UNGROUPED_EVENTS; e++) {
        int tools = 0;
        JSValue name;
        for (t = 0; t < QP_MON_TOOLS; t++) {
            if (ms->mon_events[t] & (1u << e))
                tools |= 1 << t;
        }
        if (!tools)
            continue;
        name = qp_str_from_cstr(ctx, qp_mon_event_names[e]);
        if (JS_IsException(name) ||
            qp_dict_set_item(ctx, res, name, JS_NewInt32(ctx, tools)) < 0) {
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

static int qp_mon_exec(JSContext *ctx, JSValueConst mod)
{
    JSValue events;
    int e;

    if (qp_mon_set_obj(ctx, mod, "DISABLE",
                       qp_object_new_instance(ctx, QP_TYPE(ctx, object), JS_CLASS_PY_OBJECT)) < 0 ||
        qp_mon_set_obj(ctx, mod, "MISSING",
                       qp_object_new_instance(ctx, QP_TYPE(ctx, object), JS_CLASS_PY_OBJECT)) < 0)
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
