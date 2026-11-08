/*
 * dquickts Python (M2): code objects.
 *
 * Derived from CPython 3.15 Objects/codeobject.c (PSF-2.0).
 *
 * A code object (PY_CODE) is a facade over a Python JSFunctionBytecode and
 * its QPCodeInfo (object.h).  Field mapping (m2-design.md "Code metadata"):
 *   co_name = func_name, co_qualname/co_flags/co_argcount/... = QPCodeInfo,
 *   co_varnames = parameters (vardefs[0..arg_count), minus the hidden locals
 *   argument of code without CO_NEWLOCALS) + non-captured locals,
 *   co_cellvars = captured parameters and locals (CPython localsplus order:
 *   parameters, sorted names, then the implicit __class__ / __classdict__ /
 *   __conditional_annotations__ cells), co_freevars = closure_var names,
 *   co_consts = CPython's constant table (QPCodeInfo.consts, built by the
 *   compiler: cpool indexes in CPython's order) without the entries the
 *   bytecode does not load, except the first (remove_unused_consts);
 *   nested bytecodes as code objects,
 *   co_names = atoms of the name subops (getattr, load_global, ...).
 * co_code, co_linetable and co_exceptiontable are a CPython 3.15
 * instruction stream and its tables synthesized from the bytecode (see
 * "co_code" below), and every Python-visible offset is an offset in it.
 * replace() can change co_name, co_qualname, co_filename, co_firstlineno,
 * co_linetable and co_exceptiontable (the facade keeps the overrides; the
 * last two are only reported: tracebacks keep the real locations);
 * changing any other field would require recompiling and raises
 * ValueError.
 *
 * The code() constructor (used by Cython's limited-API modules for the
 * __code__ of their functions) builds an "explicit" code object: CPython
 * bytecode cannot be run by this VM, so it reports exactly the fields it
 * was given (QPCodeObject.fields) over a synthesized bytecode with the
 * same calling convention, function kind and free variables, whose body
 * raises SystemError when it runs.  replace() of an explicit code object
 * calls the constructor again with the changed fields.
 */

/* private helpers of funcobject.c */
static JSValue qp_fo_type_name(JSContext *ctx, QPType *t);
static Py_hash_t qp_fo_hash_pointer(const void *ptr);

typedef struct QPCodeObject {
    JSValue bytecode;           /* JS_TAG_FUNCTION_BYTECODE (owned) */
    JSValue name;               /* replace() overrides (str) or QP_NULL */
    JSValue qualname;
    JSValue filename;
    int firstlineno;
    BOOL has_firstlineno;
    uint32_t flags_add;         /* replace(co_flags=...) override: CO_ITERABLE_COROUTINE */
    /* replace() overrides (bytes) or QP_NULL, reported by co_linetable /
       co_lines() / co_positions() and co_exceptiontable; metadata only,
       not saved by snapshots and marshal */
    JSValue linetable;
    JSValue exctable;
    JSValue fields;             /* explicit code object: tuple of the 18 code() fields, else QP_NULL */
} QPCodeObject;

static void qp_code_finalizer(JSRuntime *rt, JSValue val)
{
    QPCodeObject *c = JS_VALUE_GET_OBJ(val)->u.opaque;
    JSFunctionBytecode *b;

    if (!c)
        return;
    /* (a bytecode freed first by the cycle collector has py_code NULL) */
    b = JS_VALUE_GET_PTR(c->bytecode);
    if (b->py_code && b->py_code->co_facade == JS_VALUE_GET_OBJ(val))
        b->py_code->co_facade = NULL;
    JS_FreeValueRT(rt, c->bytecode);
    JS_FreeValueRT(rt, c->name);
    JS_FreeValueRT(rt, c->qualname);
    JS_FreeValueRT(rt, c->filename);
    JS_FreeValueRT(rt, c->linetable);
    JS_FreeValueRT(rt, c->exctable);
    JS_FreeValueRT(rt, c->fields);
    js_free_rt(rt, c);
}

static void qp_code_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QPCodeObject *c = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!c)
        return;
    JS_MarkValue(rt, c->bytecode, mark_func);
    JS_MarkValue(rt, c->name, mark_func);
    JS_MarkValue(rt, c->qualname, mark_func);
    JS_MarkValue(rt, c->filename, mark_func);
    JS_MarkValue(rt, c->linetable, mark_func);
    JS_MarkValue(rt, c->exctable, mark_func);
    JS_MarkValue(rt, c->fields, mark_func);
}

static BOOL qp_co_check(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT &&
        JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_PY_CODE &&
        JS_VALUE_GET_OBJ(v)->u.opaque != NULL;
}

static QPCodeObject *qp_co_data(JSValueConst v)
{
    return qp_co_check(v) ? JS_VALUE_GET_OBJ(v)->u.opaque : NULL;
}

static JSValue qp_co_alloc(JSContext *ctx, JSValueConst bytecode, const QPCodeObject *from)
{
    QPCodeObject *c;
    JSValue obj;

    c = js_malloc(ctx, sizeof(*c));
    if (!c)
        return JS_EXCEPTION;
    obj = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, code)), JS_CLASS_PY_CODE);
    if (JS_IsException(obj)) {
        js_free(ctx, c);
        return JS_EXCEPTION;
    }
    c->bytecode = JS_DupValue(ctx, bytecode);
    if (from) {
        c->name = JS_DupValue(ctx, from->name);
        c->qualname = JS_DupValue(ctx, from->qualname);
        c->filename = JS_DupValue(ctx, from->filename);
        c->firstlineno = from->firstlineno;
        c->has_firstlineno = from->has_firstlineno;
        c->flags_add = from->flags_add;
        c->linetable = JS_DupValue(ctx, from->linetable);
        c->exctable = JS_DupValue(ctx, from->exctable);
        c->fields = JS_DupValue(ctx, from->fields);
    } else {
        c->name = QP_NULL;
        c->qualname = QP_NULL;
        c->filename = QP_NULL;
        c->firstlineno = 0;
        c->has_firstlineno = FALSE;
        c->flags_add = 0;
        c->linetable = QP_NULL;
        c->exctable = QP_NULL;
        c->fields = QP_NULL;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = c;
#ifdef CONFIG_PYTHON_CAPI
    if (unlikely(qp_capi_code_watchers_set))
        qp_capi_code_created(ctx, obj);
#endif
    return obj;
}

static JSValue qp_code_new(JSContext *ctx, JSValueConst bytecode)
{
    JSFunctionBytecode *b;
    JSValue obj;

    if (JS_VALUE_GET_TAG(bytecode) != JS_TAG_FUNCTION_BYTECODE ||
        !((JSFunctionBytecode *)JS_VALUE_GET_PTR(bytecode))->is_py)
        return qp_raise_system_error(ctx, "code object over non-Python bytecode");
    b = JS_VALUE_GET_PTR(bytecode);
    if (b->py_code && b->py_code->co_facade)
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, b->py_code->co_facade));
    obj = qp_co_alloc(ctx, bytecode, NULL);
    if (!JS_IsException(obj) && b->py_code)
        b->py_code->co_facade = JS_VALUE_GET_OBJ(obj);
    return obj;
}

/* co_flags bits a code object adds to those of its bytecode
   (types.coroutine: CO_ITERABLE_COROUTINE); 0 for other values */
static uint32_t qp_code_extra_flags(JSValueConst code)
{
    QPCodeObject *c = qp_co_data(code);
    return c ? c->flags_add : 0;
}

static struct JSFunctionBytecode *qp_code_bytecode(JSValueConst code_or_func)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(code_or_func) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(code_or_func);
    if (p->class_id == JS_CLASS_PY_CODE) {
        QPCodeObject *c = p->u.opaque;
        return c ? JS_VALUE_GET_PTR(c->bytecode) : NULL;
    }
    if (p->class_id == JS_CLASS_BYTECODE_FUNCTION && p->u.func.function_bytecode &&
        p->u.func.function_bytecode->is_py)
        return p->u.func.function_bytecode;
    return NULL;
}

static void qp_code_mark_native_bc(JSFunctionBytecode *b)
{
    int i;

    if (!b->is_py || !b->py_code || (b->py_code->co_flags & QP_CO_NATIVE))
        return;
    b->py_code->co_flags |= QP_CO_NATIVE;
    for (i = 0; i < b->cpool_count; i++) {
        if (JS_VALUE_GET_TAG(b->cpool[i]) == JS_TAG_FUNCTION_BYTECODE)
            qp_code_mark_native_bc(JS_VALUE_GET_PTR(b->cpool[i]));
    }
}

static void qp_code_mark_native(JSValueConst code_or_func)
{
    JSFunctionBytecode *b;

    if (JS_VALUE_GET_TAG(code_or_func) == JS_TAG_FUNCTION_BYTECODE)
        b = JS_VALUE_GET_PTR(code_or_func);
    else
        b = qp_code_bytecode(code_or_func);
    if (b)
        qp_code_mark_native_bc(b);
}

/* ---- field accessors ----------------------------------------------------- */

static JSValue qp_co_get_name(JSContext *ctx, JSValueConst code)
{
    QPCodeObject *c = qp_co_data(code);
    JSFunctionBytecode *b;

    if (!c)
        return qp_raise_type_error(ctx, "expected a code object");
    if (!QP_IS_NULL(c->name))
        return JS_DupValue(ctx, c->name);
    b = JS_VALUE_GET_PTR(c->bytecode);
    return qp_str_from_atom(ctx, b->func_name != JS_ATOM_NULL ? b->func_name
                            : JS_ATOM_empty_string);
}

static JSValue qp_co_get_qualname(JSContext *ctx, JSValueConst code)
{
    QPCodeObject *c = qp_co_data(code);
    JSFunctionBytecode *b;

    if (!c)
        return qp_raise_type_error(ctx, "expected a code object");
    if (!QP_IS_NULL(c->qualname))
        return JS_DupValue(ctx, c->qualname);
    b = JS_VALUE_GET_PTR(c->bytecode);
    if (b->py_code && b->py_code->qualname != JS_ATOM_NULL)
        return qp_str_from_atom(ctx, b->py_code->qualname);
    return qp_co_get_name(ctx, code);
}

/* co_filename: str, or None when the bytecode has no file name */
static JSValue qp_co_get_filename(JSContext *ctx, JSValueConst code)
{
    QPCodeObject *c = qp_co_data(code);
    JSFunctionBytecode *b;

    if (!c)
        return qp_raise_type_error(ctx, "expected a code object");
    if (!QP_IS_NULL(c->filename))
        return JS_DupValue(ctx, c->filename);
    b = JS_VALUE_GET_PTR(c->bytecode);
    if (!b->has_debug || b->debug.filename == JS_ATOM_NULL)
        return JS_UNDEFINED;
    return qp_str_from_atom(ctx, b->debug.filename);
}

static int qp_co_get_firstlineno(JSValueConst code)
{
    QPCodeObject *c = qp_co_data(code);
    JSFunctionBytecode *b;

    if (c->has_firstlineno)
        return c->firstlineno;
    b = JS_VALUE_GET_PTR(c->bytecode);
    return b->py_code ? b->py_code->firstlineno : 0;
}

/* Tuple of str from n atoms. */
static JSValue qp_co_atoms_tuple(JSContext *ctx, const JSAtom *atoms, int n)
{
    JSValue t;
    int i;

    t = qp_tuple_new(ctx, n);
    if (JS_IsException(t))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        JSValue s = qp_str_from_atom(ctx, atoms[i]);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, t);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(t)[i]);
        QP_TUPLE_ITEMS(t)[i] = s;
    }
    return t;
}

static BOOL qp_co_atom_in(const JSAtom *atoms, int n, JSAtom a)
{
    int i;

    for (i = 0; i < n; i++) {
        if (atoms[i] == a)
            return TRUE;
    }
    return FALSE;
}

enum { QP_CO_VARNAMES, QP_CO_CELLVARS, QP_CO_FREEVARS };

static BOOL qp_co_is_implicit_cell(JSContext *ctx, JSAtom a)
{
    return a == QP_ATOM(ctx, __class__) || a == QP_ATOM(ctx, __classdict__) ||
        a == QP_ATOM(ctx, __conditional_annotations__);
}

/* Names of one localsplus kind, in CPython's order (see the file comment).
   *patoms is js_malloc'ed (free with js_free); returns the count, -1 on
   error. */
static int qp_co_kind_names(JSContext *ctx, JSFunctionBytecode *b, int kind, JSAtom **patoms)
{
    int total = b->arg_count + b->var_count + b->closure_var_count;
    JSAtom *atoms;
    int i, j, n = 0;

    atoms = js_malloc(ctx, sizeof(JSAtom) * (total + 1));
    if (!atoms)
        return -1;
    switch (kind) {
    case QP_CO_VARNAMES:
        for (i = 0; i < b->arg_count; i++) {
            if (b->vardefs[i].var_name != JS_ATOM_NULL)
                atoms[n++] = b->vardefs[i].var_name;
        }
        for (i = 0; i < b->var_count; i++) {
            JSBytecodeVarDef *vd = &b->vardefs[b->arg_count + i];
            if (vd->is_captured || vd->var_name == JS_ATOM_NULL)
                continue;
            if (vd->is_lexical && qp_co_atom_in(atoms, n, vd->var_name))
                continue;   /* hidden comprehension slot sharing an outer name */
            atoms[n++] = vd->var_name;
        }
        break;
    case QP_CO_CELLVARS:
        {
            static const int implicit[3] = {
                QP_ATOM___class__, QP_ATOM___classdict__, QP_ATOM___conditional_annotations__
            };
            int start;

            for (i = 0; i < b->arg_count; i++) {
                if (b->vardefs[i].is_captured && b->vardefs[i].var_name != JS_ATOM_NULL)
                    atoms[n++] = b->vardefs[i].var_name;
            }
            start = n;
            for (i = 0; i < b->var_count; i++) {
                JSBytecodeVarDef *vd = &b->vardefs[b->arg_count + i];
                if (!vd->is_captured || vd->var_name == JS_ATOM_NULL ||
                    qp_co_is_implicit_cell(ctx, vd->var_name) ||
                    qp_co_atom_in(atoms, n, vd->var_name))
                    continue;
                atoms[n++] = vd->var_name;
            }
            /* sort the non-parameter cells by name (code point order) */
            for (i = start + 1; i < n; i++) {
                JSAtom a = atoms[i];
                JSValue sa = JS_AtomToString(ctx, a);
                if (JS_IsException(sa)) {
                    js_free(ctx, atoms);
                    return -1;
                }
                for (j = i - 1; j >= start; j--) {
                    JSValue sb = JS_AtomToString(ctx, atoms[j]);
                    int cmp;
                    if (JS_IsException(sb)) {
                        JS_FreeValue(ctx, sa);
                        js_free(ctx, atoms);
                        return -1;
                    }
                    cmp = qp_str_compare(ctx, sb, sa);
                    JS_FreeValue(ctx, sb);
                    if (cmp <= 0)
                        break;
                    atoms[j + 1] = atoms[j];
                }
                atoms[j + 1] = a;
                JS_FreeValue(ctx, sa);
            }
            for (j = 0; j < 3; j++) {
                JSAtom a = QP_STATE(ctx)->atoms[implicit[j]];
                for (i = 0; i < b->var_count; i++) {
                    JSBytecodeVarDef *vd = &b->vardefs[b->arg_count + i];
                    if (vd->is_captured && vd->var_name == a) {
                        if (!qp_co_atom_in(atoms, n, a))
                            atoms[n++] = a;
                        break;
                    }
                }
            }
        }
        break;
    default:
        for (i = 0; i < b->closure_var_count; i++)
            atoms[n++] = b->closure_var[i].var_name;
        break;
    }
    *patoms = atoms;
    return n;
}

static JSValue qp_co_kind_tuple(JSContext *ctx, JSFunctionBytecode *b, int kind)
{
    JSAtom *atoms;
    JSValue t;
    int n;

    n = qp_co_kind_names(ctx, b, kind, &atoms);
    if (n < 0)
        return JS_EXCEPTION;
    t = qp_co_atoms_tuple(ctx, atoms, n);
    js_free(ctx, atoms);
    return t;
}

/* co_varnames + co_cellvars (non-parameter part) + co_freevars: CPython's
   co_localsplusnames, used by eq/hash. */
static JSValue qp_co_localsplus_names(JSContext *ctx, JSFunctionBytecode *b)
{
    JSAtom *v = NULL, *c = NULL, *f = NULL, *all;
    int nv, nc, nf, n = 0, i;
    JSValue t = JS_EXCEPTION;

    nv = qp_co_kind_names(ctx, b, QP_CO_VARNAMES, &v);
    if (nv < 0)
        return JS_EXCEPTION;
    nc = qp_co_kind_names(ctx, b, QP_CO_CELLVARS, &c);
    if (nc < 0)
        goto done;
    nf = qp_co_kind_names(ctx, b, QP_CO_FREEVARS, &f);
    if (nf < 0)
        goto done;
    all = js_malloc(ctx, sizeof(JSAtom) * (nv + nc + nf + 1));
    if (!all)
        goto done;
    for (i = 0; i < nv; i++)
        all[n++] = v[i];
    for (i = 0; i < nc; i++) {
        if (!qp_co_atom_in(v, nv, c[i]))
            all[n++] = c[i];
    }
    for (i = 0; i < nf; i++)
        all[n++] = f[i];
    t = qp_co_atoms_tuple(ctx, all, n);
    js_free(ctx, all);
 done:
    js_free(ctx, v);
    js_free(ctx, c);
    js_free(ctx, f);
    return t;
}

static int qp_co_nlocals(JSContext *ctx, JSFunctionBytecode *b)
{
    JSAtom *atoms;
    int n = qp_co_kind_names(ctx, b, QP_CO_VARNAMES, &atoms);

    if (n >= 0)
        js_free(ctx, atoms);
    return n;
}

/* ---- co_code: the synthesized CPython instruction stream ------------------
 *
 * Our runtime executes QuickJS bytecode (OP_py subops + reused QuickJS
 * opcodes, python/opcode.h), never CPython bytecode.  For the tools that
 * read code objects (dis, inspect.getclosurevars, opcode-based analysers,
 * coverage tools, traceback's co_positions()), co_code is a CPython 3.15
 * instruction stream synthesized from it:
 *
 *  - the stream starts with CPython's prologue: COPY_FREE_VARS (free
 *    variables), MAKE_CELL (each cell, localsplus order), RETURN_GENERATOR +
 *    POP_TOP (generators, coroutines, async generators) and RESUME 0;
 *  - every QuickJS instruction i then becomes a "group" of one or more
 *    CPython instructions (qp_co_emit_insn: the CPython instruction each
 *    subop replaces, e.g. getattr -> LOAD_ATTR namei << 1, py jump_if_false
 *    -> TO_BOOL + POP_JUMP_IF_FALSE, nip -> SWAP 2 + POP_TOP, QuickJS-only
 *    instructions such as catch -> NOP), each with its EXTENDED_ARG prefixes
 *    (args > 255) and _PyOpcode_Caches inline CACHE entries, so dis decodes
 *    it.  Arguments index co_names (name subops, first-use order),
 *    co_consts (push_const / fclosure of a cpool entry, push_i32 of a
 *    value CPython keeps as a constant: not 0..255 (LOAD_SMALL_INT) nor -1
 *    (LOAD_COMMON_CONSTANT)) and localsplus (co_varnames, then the cells
 *    that are not parameters, then co_freevars) exactly like CPython's;
 *  - jumps are relative in code units to the target group's first unit;
 *    backward conditional jumps (CPython's are forward only) become the
 *    inverted POP_JUMP_IF_* over a JUMP_BACKWARD.
 *
 * Offsets.  Every Python-visible offset is a byte offset in that stream:
 * co_positions() (one entry per code unit, caches and EXTENDED_ARG
 * included), co_lines(), co_branches(), co_exceptiontable, f_lasti and
 * tb_lasti.  The offset of instruction i (qp_code_pc2lasti) is the offset
 * of its group's first CPython opcode (after its EXTENDED_ARGs; CPython's
 * lasti); every offset inside group i maps back to instruction i
 * (qp_code_lasti2pc), the prologue to instruction 0.  The group sizes
 * depend on the EXTENDED_ARG counts of the jumps, which depend on the
 * distances: QPCoLayout is the monotone fixpoint (a jump's EXTENDED_ARG
 * count never decreases between passes, so the passes converge; a count
 * larger than needed is encoded as EXTENDED_ARG 0 prefixes).  The layout
 * is a pure function of the bytecode, built on first use and cached in
 * QPCodeInfo.co_layout (freed with the bytecode, never serialized).
 *
 * co_linetable uses CPython's location table format (locations.md) over
 * the same units (prologue: CPython's locations, groups: the location of
 * their instruction), relative to the code object's co_firstlineno; co_lines()
 * and co_positions() decode it, like CPython, so explicit code objects
 * (code()) report their own table.  co_exceptiontable is CPython's format:
 * the handler of each group is the innermost active `catch` of the stack
 * analysis (compute_stack_size_tabs), depth = the stack level under its
 * marker (never "lasti").
 *
 * code.replace(co_code=...) accepts only the synthesized stream itself
 * (the runtime cannot execute CPython bytecode); any other value raises
 * ValueError.  code() objects (explicit) report the co_code they were given.
 */

#define QP_CPY_CACHE                        0
#define QP_CPY_EXTENDED_ARG                67
#define QP_CPY_NOP                         25
#define QP_CPY_RESUME                     128
#define QP_CPY_RETURN_GENERATOR            32
#define QP_CPY_POP_TOP                     29
#define QP_CPY_COPY_FREE_VARS              58
#define QP_CPY_MAKE_CELL                   96
#define QP_CPY_LOAD_SMALL_INT              93
#define QP_CPY_LOAD_CONST                  81
#define QP_CPY_LOAD_COMMON_CONSTANT        80
#define QP_CPY_MAKE_FUNCTION               21
#define QP_CPY_SWAP                       116
#define QP_CPY_COPY                        57
#define QP_CPY_LOAD_FAST                   83
#define QP_CPY_LOAD_FAST_CHECK             87
#define QP_CPY_LOAD_DEREF                  82
#define QP_CPY_STORE_FAST                 111
#define QP_CPY_STORE_DEREF                110
#define QP_CPY_DELETE_FAST                 61
#define QP_CPY_DELETE_DEREF                60
#define QP_CPY_JUMP_FORWARD                76
#define QP_CPY_JUMP_BACKWARD               74
#define QP_CPY_POP_JUMP_IF_FALSE           99
#define QP_CPY_POP_JUMP_IF_TRUE           102
#define QP_CPY_UNARY_NOT                   40
#define QP_CPY_IS_OP                       73
#define QP_CPY_RETURN_VALUE                33
#define QP_CPY_RAISE_VARARGS              103
#define QP_CPY_BUILD_LIST                  44
#define QP_CPY_CALL                        50
#define QP_CPY_PUSH_NULL                   31
#define QP_CPY_BINARY_OP                   42
#define QP_CPY_UNARY_NEGATIVE              39
#define QP_CPY_UNARY_INVERT                38
#define QP_CPY_CALL_INTRINSIC_1            51
#define QP_CPY_CALL_INTRINSIC_2            52
#define QP_CPY_COMPARE_OP                  54
#define QP_CPY_CONTAINS_OP                 55
#define QP_CPY_TO_BOOL                     37
#define QP_CPY_STORE_SUBSCR                36
#define QP_CPY_DELETE_SUBSCR                8
#define QP_CPY_BINARY_SLICE                 1
#define QP_CPY_STORE_SLICE                 35
#define QP_CPY_BUILD_SLICE                 47
#define QP_CPY_LOAD_ATTR                   79
#define QP_CPY_STORE_ATTR                 109
#define QP_CPY_DELETE_ATTR                 59
#define QP_CPY_LOAD_SUPER_ATTR             95
#define QP_CPY_LOAD_SPECIAL                94
#define QP_CPY_LOAD_GLOBAL                 91
#define QP_CPY_STORE_GLOBAL               114
#define QP_CPY_DELETE_GLOBAL               62
#define QP_CPY_LOAD_NAME                   92
#define QP_CPY_STORE_NAME                 115
#define QP_CPY_DELETE_NAME                 63
#define QP_CPY_LOAD_LOCALS                 20
#define QP_CPY_LOAD_FROM_DICT_OR_GLOBALS   90
#define QP_CPY_LOAD_FROM_DICT_OR_DEREF     89
#define QP_CPY_CALL_KW                     53
#define QP_CPY_CALL_FUNCTION_EX             4
#define QP_CPY_BUILD_TUPLE                 49
#define QP_CPY_BUILD_SET                   46
#define QP_CPY_BUILD_MAP                   45
#define QP_CPY_BUILD_STRING                48
#define QP_CPY_LIST_APPEND                 77
#define QP_CPY_LIST_EXTEND                 78
#define QP_CPY_SET_ADD                    106
#define QP_CPY_SET_UPDATE                 108
#define QP_CPY_MAP_ADD                     97
#define QP_CPY_DICT_UPDATE                 65
#define QP_CPY_DICT_MERGE                  64
#define QP_CPY_UNPACK_SEQUENCE            118
#define QP_CPY_UNPACK_EX                  117
#define QP_CPY_CONVERT_VALUE               56
#define QP_CPY_FORMAT_SIMPLE               12
#define QP_CPY_FORMAT_WITH_SPEC            13
#define QP_CPY_BUILD_INTERPOLATION         43
#define QP_CPY_BUILD_TEMPLATE               2
#define QP_CPY_GET_ITER                    70
#define QP_CPY_FOR_ITER                    68
#define QP_CPY_SEND                       105
#define QP_CPY_YIELD_VALUE                119
#define QP_CPY_END_SEND                    10
#define QP_CPY_CLEANUP_THROW                7
#define QP_CPY_GET_AWAITABLE               69
#define QP_CPY_GET_AITER                   14
#define QP_CPY_GET_ANEXT                   15
#define QP_CPY_END_ASYNC_FOR               66
#define QP_CPY_RERAISE                    104
#define QP_CPY_PUSH_EXC_INFO               30
#define QP_CPY_POP_EXCEPT                  27
#define QP_CPY_CHECK_EXC_MATCH              6
#define QP_CPY_CHECK_EG_MATCH               5
#define QP_CPY_WITH_EXCEPT_START           41
#define QP_CPY_SET_FUNCTION_ATTRIBUTE     107
#define QP_CPY_LOAD_BUILD_CLASS            19
#define QP_CPY_SETUP_ANNOTATIONS           34
#define QP_CPY_IMPORT_NAME                 72
#define QP_CPY_IMPORT_FROM                 71
#define QP_CPY_MATCH_CLASS                 98
#define QP_CPY_MATCH_MAPPING               23
#define QP_CPY_MATCH_SEQUENCE              24
#define QP_CPY_MATCH_KEYS                  22
#define QP_CPY_GET_LEN                     16

/* _PyOpcode_Caches of the instructions above */
static int qp_cpy_caches(int op)
{
    switch (op) {
    case QP_CPY_RESUME:
    case QP_CPY_JUMP_BACKWARD:
    case QP_CPY_POP_JUMP_IF_FALSE:
    case QP_CPY_POP_JUMP_IF_TRUE:
    case QP_CPY_COMPARE_OP:
    case QP_CPY_CONTAINS_OP:
    case QP_CPY_STORE_SUBSCR:
    case QP_CPY_LOAD_SUPER_ATTR:
    case QP_CPY_CALL_FUNCTION_EX:
    case QP_CPY_UNPACK_SEQUENCE:
    case QP_CPY_GET_ITER:
    case QP_CPY_FOR_ITER:
    case QP_CPY_SEND:
        return 1;
    case QP_CPY_CALL:
    case QP_CPY_TO_BOOL:
    case QP_CPY_CALL_KW:
        return 3;
    case QP_CPY_STORE_ATTR:
    case QP_CPY_LOAD_GLOBAL:
        return 4;
    case QP_CPY_BINARY_OP:
        return 5;
    case QP_CPY_LOAD_ATTR:
        return 9;
    default:
        return 0;
    }
}

/* Size of the instruction at pos. */
static int qp_co_insn_size(const uint8_t *buf, int pos)
{
    int op = buf[pos];

    if (op == OP_py)
        return qp_subop_info[buf[pos + 1]].size;
    return short_opcode_info(op).size;
}

/* uint32 key -> index map (open addressing; slots hold index + 1) */
typedef struct QPCoHash {
    uint32_t *tab;
    uint32_t mask;
} QPCoHash;

static uint32_t qp_co_hash_start(const QPCoHash *h, uint32_t key)
{
    key *= 0x9e3779b1u;
    return (key ^ (key >> 16)) & h->mask;
}

/* index of key in keys[], -1 if absent */
static int qp_co_hash_find(const QPCoHash *h, const uint32_t *keys, uint32_t key)
{
    uint32_t i;

    if (!h->tab)
        return -1;
    for (i = qp_co_hash_start(h, key); h->tab[i]; i = (i + 1) & h->mask) {
        if (keys[h->tab[i] - 1] == key)
            return h->tab[i] - 1;
    }
    return -1;
}

/* intern key: its index in keys[0..*pn), appended if absent */
static uint32_t qp_co_hash_intern(QPCoHash *h, uint32_t *keys, uint32_t *pn, uint32_t key)
{
    uint32_t i;

    for (i = qp_co_hash_start(h, key); h->tab[i]; i = (i + 1) & h->mask) {
        if (keys[h->tab[i] - 1] == key)
            return h->tab[i] - 1;
    }
    keys[*pn] = key;
    h->tab[i] = ++*pn;
    return *pn - 1;
}

/* table for at most n keys (load <= 1/2); 0, -1 on OOM */
static int qp_co_hash_init(JSContext *ctx, QPCoHash *h, uint32_t n)
{
    uint32_t size = 4;

    while (size < 2 * n + 1)
        size *= 2;
    h->tab = js_mallocz(ctx, sizeof(uint32_t) * size);
    h->mask = size - 1;
    return h->tab ? 0 : -1;
}

typedef struct QPCoLayout QPCoLayout;

struct QPCoLayout {
    uint32_t n;            /* QuickJS instructions */
    uint32_t *pc;          /* [n + 1] byte offset of each instruction, pc[n] = byte_code_len */
    uint32_t *unit;        /* [n + 1] first code unit of each group, unit[n] = len(co_code) / 2 */
    uint32_t *lasti;       /* [n] code unit of the main CPython opcode of each group */
    uint8_t *jext;         /* [n] EXTENDED_ARG count of the group's jump (fixpoint floor) */
    uint32_t pro_noloc;    /* prologue units without location (COPY_FREE_VARS, MAKE_CELL) */
    uint32_t pro_gen;      /* then RETURN_GENERATOR + POP_TOP units; RESUME (2 units) ends it */
    uint32_t *names;       /* co_names: atoms (owned by the bytecode) */
    uint32_t nnames;
    QPCoHash name_hash;
    uint32_t *ints;        /* int32 values of the push_i32 that are LOAD_CONST */
    uint32_t nints;
    QPCoHash int_hash;
    int32_t *int_co;       /* [nints] co_consts index of ints[k] */
    uint32_t *co;          /* co_consts items: cpool index, or QP_CO_INT | k for ints[k] */
    uint32_t nco;
    int32_t *cpool_co;     /* [cpool_count] co_consts index of each cpool entry, -1 none */
    int32_t *slot;         /* [arg_count + var_count] localsplus index, -1 none */
    uint32_t free_base;    /* localsplus index of co_freevars[0] */
    uint32_t *cells;       /* localsplus indexes of the cells, ascending (MAKE_CELL) */
    uint32_t ncells;
    uint8_t *exctab;       /* co_exceptiontable, computed on first use */
    uint32_t exctab_len;
    BOOL has_exctab;
};

#define QP_CO_INT 0x80000000u

static void qp_co_layout_free(JSRuntime *rt, QPCoLayout *L)
{
    if (!L)
        return;
    js_free_rt(rt, L->pc);
    js_free_rt(rt, L->unit);
    js_free_rt(rt, L->lasti);
    js_free_rt(rt, L->jext);
    js_free_rt(rt, L->names);
    js_free_rt(rt, L->name_hash.tab);
    js_free_rt(rt, L->ints);
    js_free_rt(rt, L->int_hash.tab);
    js_free_rt(rt, L->int_co);
    js_free_rt(rt, L->co);
    js_free_rt(rt, L->cpool_co);
    js_free_rt(rt, L->slot);
    js_free_rt(rt, L->cells);
    js_free_rt(rt, L->exctab);
    js_free_rt(rt, L);
}

/* Index of the instruction containing byte offset pc (L->n > 0). */
static uint32_t qp_co_pc_index(const QPCoLayout *L, uint32_t pc)
{
    uint32_t lo = 0, hi = L->n;

    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) >> 1;
        if (L->pc[mid] <= pc)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* Index of the group containing code unit u (L->n > 0; the prologue
   belongs to group 0). */
static uint32_t qp_co_unit_index(const QPCoLayout *L, uint32_t u)
{
    uint32_t lo = 0, hi = L->n;

    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) >> 1;
        if (L->unit[mid] <= u)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* co_consts index of cpool entry idx (qp_co_layout_consts maps every
   loaded entry) */
static uint32_t qp_co_const_index(const QPCoLayout *L, uint32_t idx)
{
    return L->cpool_co[idx] < 0 ? 0 : L->cpool_co[idx];
}

/* atom of a co_names subop at pos (OP_py), JS_ATOM_NULL for the others */
static JSAtom qp_co_name_operand(const uint8_t *buf, int pos)
{
    int sub, fmt;

    if (buf[pos] != OP_py)
        return JS_ATOM_NULL;
    sub = buf[pos + 1];
    fmt = qp_subop_info[sub].fmt;
    if ((fmt == OP_FMT_atom || fmt == OP_FMT_atom_u8 || fmt == OP_FMT_atom_u16) &&
        sub != QP_OP_load_from_dict_or_deref)
        return get_u32(buf + pos + 2);
    return JS_ATOM_NULL;
}

/* Integer pushed by a QuickJS push instruction at pos; FALSE for others. */
static BOOL qp_co_push_int(const uint8_t *buf, int pos, int32_t *pv)
{
    int op = buf[pos];

    switch (op) {
    case OP_push_i32:
        *pv = (int32_t)get_u32(buf + pos + 1);
        return TRUE;
#if SHORT_OPCODES
    case OP_push_i8:
        *pv = (int8_t)buf[pos + 1];
        return TRUE;
    case OP_push_i16:
        *pv = (int16_t)get_u16(buf + pos + 1);
        return TRUE;
    case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
    case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
        *pv = op - OP_push_0;
        return TRUE;
#endif
    default:
        return FALSE;
    }
}

/* push_i32 values that CPython loads with LOAD_CONST */
static BOOL qp_co_int_is_const(int32_t v)
{
    return v != -1 && (v < 0 || v > 255);
}

typedef struct QPCoEmit {
    QPCoLayout *L;
    JSFunctionBytecode *b;
    uint8_t *out;          /* emitted code units, or NULL */
    BOOL readonly;         /* not a layout pass: L is final */
    uint32_t u;            /* next code unit */
    uint32_t i;            /* current group */
    BOOL first;            /* the next opcode is the main opcode of group i */
    BOOL changed;          /* layout pass: a unit or jump size changed */
    DynBuf *branches;      /* co_branches(): int32 (src, not_taken, target) units */
    int32_t branch_src;    /* code unit of the last branch, -1 none */
    int *ops;              /* qp_code_insn_offsets(): code units of the opcodes */
    int nops, maxops;
    int32_t branch_back;   /* code unit of the JUMP_BACKWARD of the last
                              branch (a backward conditional jump), -1 none */
} QPCoEmit;

static int qp_co_ext_count(uint32_t arg)
{
    return (arg > 0xff) + (arg > 0xffff) + (arg > 0xffffff);
}

static void qp_co_unit(QPCoEmit *e, int op, int arg)
{
    if (e->out) {
        e->out[2 * e->u] = op;
        e->out[2 * e->u + 1] = arg;
    }
    e->u++;
}

/* op with ext (>= the needed count) EXTENDED_ARG prefixes and its caches */
static void qp_co_put_ext(QPCoEmit *e, int op, uint32_t arg, int ext)
{
    int k;

    for (k = ext; k > 0; k--)
        qp_co_unit(e, QP_CPY_EXTENDED_ARG, (arg >> (8 * k)) & 0xff);
    if (e->first) {
        if (!e->readonly)
            e->L->lasti[e->i] = e->u;
        e->first = FALSE;
    }
    if (e->ops && e->nops < e->maxops)
        e->ops[e->nops++] = e->u;
    qp_co_unit(e, op, arg & 0xff);
    for (k = qp_cpy_caches(op); k > 0; k--)
        qp_co_unit(e, QP_CPY_CACHE, 0);
}

static void qp_co_put(QPCoEmit *e, int op, uint32_t arg)
{
    qp_co_put_ext(e, op, arg, qp_co_ext_count(arg));
}

static void qp_co_branch(QPCoEmit *e, uint32_t src, uint32_t not_taken, uint32_t target)
{
    int32_t v[3];

    e->branch_src = src;
    if (!e->branches)
        return;
    v[0] = 2 * src;
    v[1] = 2 * not_taken;
    v[2] = 2 * target;
    dbuf_put(e->branches, (const uint8_t *)v, sizeof(v));
}

/* EXTENDED_ARG count of the jump of group e->i (the fixpoint floor);
   records a larger need. */
static int qp_co_jump_ext(QPCoEmit *e, int need)
{
    uint8_t *jext = &e->L->jext[e->i];

    if (need <= *jext)
        return *jext;
    if (!e->readonly) {
        *jext = need;
        e->changed = TRUE;
    }
    return need;
}

/* Jump of group e->i to the instruction at byte offset target: op is
   JUMP_FORWARD (direction chosen here), POP_JUMP_IF_FALSE/TRUE, FOR_ITER
   or SEND. */
static void qp_co_jump(QPCoEmit *e, int op, uint32_t target)
{
    QPCoLayout *L = e->L;
    uint32_t tunit, start, end, arg = 0;
    int ext;

    if (target >= (uint32_t)e->b->byte_code_len) {
        qp_co_put(e, QP_CPY_NOP, 0);
        return;
    }
    /* forward targets: the previous pass's units (the fixpoint fixes them) */
    tunit = L->unit[qp_co_pc_index(L, target)];
    if (target <= L->pc[e->i] && op != QP_CPY_FOR_ITER && op != QP_CPY_SEND) {
        BOOL cond = op != QP_CPY_JUMP_FORWARD;
        uint32_t src = e->u;
        /* JUMP_BACKWARD, after the inverted conditional jump over it */
        start = e->u + (cond ? 1 + qp_cpy_caches(QP_CPY_POP_JUMP_IF_TRUE) : 0);
        ext = e->L->jext[e->i];
        for (;;) {
            end = start + ext + 1 + qp_cpy_caches(QP_CPY_JUMP_BACKWARD);
            arg = end > tunit ? end - tunit : 0;
            if (qp_co_ext_count(arg) <= ext)
                break;
            ext = qp_co_ext_count(arg);
        }
        ext = qp_co_jump_ext(e, ext);
        if (cond) {
            qp_co_put(e, op == QP_CPY_POP_JUMP_IF_FALSE ? QP_CPY_POP_JUMP_IF_TRUE :
                      QP_CPY_POP_JUMP_IF_FALSE,
                      ext + 1 + qp_cpy_caches(QP_CPY_JUMP_BACKWARD));
        }
        qp_co_put_ext(e, QP_CPY_JUMP_BACKWARD, arg, ext);
        if (cond) {
            /* not taken: the JUMP_BACKWARD; taken: past it */
            qp_co_branch(e, src, start, e->u);
            e->branch_back = start;
        }
        return;
    }
    start = e->u;
    ext = e->L->jext[e->i];
    for (;;) {
        end = start + ext + 1 + qp_cpy_caches(op);
        arg = tunit > end ? tunit - end : 0;
        if (qp_co_ext_count(arg) <= ext)
            break;
        ext = qp_co_ext_count(arg);
    }
    ext = qp_co_jump_ext(e, ext);
    qp_co_put_ext(e, op, arg, ext);
    if (op == QP_CPY_FOR_ITER) {
        /* branchesiter_next: the FOR_ITER branch target skips END_FOR +
           POP_ITER; ours is the exit label: skip the two drops of POP_ITER */
        const uint8_t *buf = e->b->byte_code_buf;
        uint32_t t = target;
        int k;
        for (k = 0; k < 2 && t < (uint32_t)e->b->byte_code_len && buf[t] == OP_drop; k++)
            t++;
        if (t < (uint32_t)e->b->byte_code_len)
            tunit = L->unit[qp_co_pc_index(L, t)];
        else
            tunit = L->unit[L->n];
        qp_co_branch(e, start + ext, e->u, tunit);
    } else if (op == QP_CPY_POP_JUMP_IF_FALSE || op == QP_CPY_POP_JUMP_IF_TRUE) {
        qp_co_branch(e, start + ext, e->u, tunit);
    }
}

static uint32_t qp_co_name_index(QPCoEmit *e, const uint8_t *p)
{
    int k = qp_co_hash_find(&e->L->name_hash, e->L->names, get_u32(p));

    return k < 0 ? 0 : k;
}

static void qp_co_load_int(QPCoEmit *e, int32_t v)
{
    if (v == -1) {
        qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_MINUS_ONE);
    } else if (!qp_co_int_is_const(v)) {
        qp_co_put(e, QP_CPY_LOAD_SMALL_INT, v);
    } else {
        int k = qp_co_hash_find(&e->L->int_hash, e->L->ints, (uint32_t)v);
        qp_co_put(e, QP_CPY_LOAD_CONST, k < 0 ? 0 : e->L->int_co[k]);
    }
}

static BOOL qp_co_is_empty_str(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING && JS_VALUE_GET_STRING(v)->len == 0;
}

/* push of cpool entry idx */
static void qp_co_load_const(QPCoEmit *e, uint32_t idx)
{
    if (idx < (uint32_t)e->b->cpool_count && qp_co_is_empty_str(e->b->cpool[idx]))
        qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_EMPTY_STR);
    else
        qp_co_put(e, QP_CPY_LOAD_CONST, qp_co_const_index(e->L, idx));
}

enum { QP_CO_LOAD, QP_CO_LOAD_CHECK, QP_CO_STORE, QP_CO_SET, QP_CO_DELETE };

/* access of parameter (is_arg) or local variable idx */
static void qp_co_slot_op(QPCoEmit *e, BOOL is_arg, int idx, int action)
{
    JSFunctionBytecode *b = e->b;
    int k = is_arg ? idx : b->arg_count + idx;
    int ls;
    BOOL cell;

    if (k < 0 || k >= b->arg_count + b->var_count || (ls = e->L->slot[k]) < 0)
        return;     /* hidden slot: NOP */
    cell = b->vardefs[k].is_captured;
    switch (action) {
    case QP_CO_LOAD:
    case QP_CO_LOAD_CHECK:
        qp_co_put(e, cell ? QP_CPY_LOAD_DEREF :
                  action == QP_CO_LOAD ? QP_CPY_LOAD_FAST : QP_CPY_LOAD_FAST_CHECK, ls);
        break;
    case QP_CO_SET:
        qp_co_put(e, QP_CPY_COPY, 1);
        /* fall through */
    case QP_CO_STORE:
        qp_co_put(e, cell ? QP_CPY_STORE_DEREF : QP_CPY_STORE_FAST, ls);
        break;
    default:
        qp_co_put(e, cell ? QP_CPY_DELETE_DEREF : QP_CPY_DELETE_FAST, ls);
        break;
    }
}

static void qp_co_free_op(QPCoEmit *e, int idx, int action)
{
    uint32_t ls = e->L->free_base + idx;

    if (idx < 0 || idx >= e->b->closure_var_count)
        return;
    if (action == QP_CO_SET)
        qp_co_put(e, QP_CPY_COPY, 1);
    qp_co_put(e, action == QP_CO_LOAD ? QP_CPY_LOAD_DEREF : QP_CPY_STORE_DEREF, ls);
}

/* co_branches() entry of an async for loop (branchesiter_next END_ASYNC_FOR
   case), recorded at its end_async_for at byte offset h: the loop's catch
   targets h; its END_SEND (the first end_send after that catch) is the
   src, the instruction after END_SEND not taken, after h the target. */
static void qp_co_async_for_branch(QPCoEmit *e, uint32_t h)
{
    QPCoLayout *L = e->L;
    const uint8_t *buf = e->b->byte_code_buf;
    uint32_t i, k, hi = qp_co_pc_index(L, h);

    for (i = hi; i-- > 0;) {
        uint32_t pc = L->pc[i];
        if (buf[pc] != OP_catch || pc + 1 + (int32_t)get_u32(buf + pc + 1) != h)
            continue;
        for (k = i + 1; k < hi; k++) {
            uint32_t p = L->pc[k];
            if (buf[p] == OP_py && buf[p + 1] == QP_OP_end_send) {
                qp_co_branch(e, L->lasti[k], L->unit[k + 1], L->unit[hi + 1]);
                return;
            }
        }
        return;
    }
}

/* CPython instructions of the OP_py subop at pos */
static void qp_co_emit_py(QPCoEmit *e, const uint8_t *buf, uint32_t pos)
{
    const uint8_t *p = buf + pos + 2;
    int sub = buf[pos + 1];
    /* the operands that the instruction has: it may end the buffer */
    int size = qp_subop_info[sub].size;
    uint32_t u8 = size >= 3 ? p[0] : 0, u16 = size >= 4 ? get_u16(p) : 0;

    switch (sub) {
    case QP_OP_push_null: qp_co_put(e, QP_CPY_PUSH_NULL, 0); break;
    case QP_OP_copy: qp_co_put(e, QP_CPY_COPY, u8); break;
    case QP_OP_swap: qp_co_put(e, QP_CPY_SWAP, u8); break;
    case QP_OP_load_common_const: qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, u8); break;
    case QP_OP_binop: qp_co_put(e, QP_CPY_BINARY_OP, u8); break;
    case QP_OP_unop:
        if (u8 == QP_UNOP_NEG)
            qp_co_put(e, QP_CPY_UNARY_NEGATIVE, 0);
        else if (u8 == QP_UNOP_INVERT)
            qp_co_put(e, QP_CPY_UNARY_INVERT, 0);
        else
            qp_co_put(e, QP_CPY_CALL_INTRINSIC_1, QP_INTRINSIC1_UNARY_POSITIVE);
        break;
    case QP_OP_compare:
        qp_co_put(e, QP_CPY_COMPARE_OP, ((u8 & QP_CMP_OP_MASK) << 5) | (u8 & QP_CMP_TO_BOOL));
        break;
    case QP_OP_is_op: qp_co_put(e, QP_CPY_IS_OP, u8); break;
    case QP_OP_contains: qp_co_put(e, QP_CPY_CONTAINS_OP, u8); break;
    case QP_OP_to_bool: qp_co_put(e, QP_CPY_TO_BOOL, 0); break;
    case QP_OP_jump_if_false:
    case QP_OP_jump_if_true:
        qp_co_put(e, QP_CPY_TO_BOOL, 0);
        qp_co_jump(e, sub == QP_OP_jump_if_false ? QP_CPY_POP_JUMP_IF_FALSE : QP_CPY_POP_JUMP_IF_TRUE,
                   pos + 2 + (int32_t)get_u32(p));
        break;
    case QP_OP_getitem: qp_co_put(e, QP_CPY_BINARY_OP, QP_NB_SUBSCR); break;
    case QP_OP_setitem: qp_co_put(e, QP_CPY_STORE_SUBSCR, 0); break;
    case QP_OP_delitem: qp_co_put(e, QP_CPY_DELETE_SUBSCR, 0); break;
    case QP_OP_getslice: qp_co_put(e, QP_CPY_BINARY_SLICE, 0); break;
    case QP_OP_setslice: qp_co_put(e, QP_CPY_STORE_SLICE, 0); break;
    case QP_OP_build_slice: qp_co_put(e, QP_CPY_BUILD_SLICE, u16); break;
    case QP_OP_getattr: qp_co_put(e, QP_CPY_LOAD_ATTR, qp_co_name_index(e, p) << 1); break;
    case QP_OP_load_method: qp_co_put(e, QP_CPY_LOAD_ATTR, qp_co_name_index(e, p) << 1 | 1); break;
    case QP_OP_setattr: qp_co_put(e, QP_CPY_STORE_ATTR, qp_co_name_index(e, p)); break;
    case QP_OP_delattr: qp_co_put(e, QP_CPY_DELETE_ATTR, qp_co_name_index(e, p)); break;
    case QP_OP_load_super_attr:
    case QP_OP_load_super_method:
        qp_co_put(e, QP_CPY_LOAD_SUPER_ATTR, qp_co_name_index(e, p) << 2 |
                  (p[4] & QP_SUPER_TWO_ARGS) | (sub == QP_OP_load_super_method));
        break;
    case QP_OP_load_special: qp_co_put(e, QP_CPY_LOAD_SPECIAL, u8); break;
    case QP_OP_load_global: qp_co_put(e, QP_CPY_LOAD_GLOBAL, qp_co_name_index(e, p) << 1); break;
    case QP_OP_store_global: qp_co_put(e, QP_CPY_STORE_GLOBAL, qp_co_name_index(e, p)); break;
    case QP_OP_delete_global: qp_co_put(e, QP_CPY_DELETE_GLOBAL, qp_co_name_index(e, p)); break;
    case QP_OP_load_name: qp_co_put(e, QP_CPY_LOAD_NAME, qp_co_name_index(e, p)); break;
    case QP_OP_store_name: qp_co_put(e, QP_CPY_STORE_NAME, qp_co_name_index(e, p)); break;
    case QP_OP_delete_name: qp_co_put(e, QP_CPY_DELETE_NAME, qp_co_name_index(e, p)); break;
    case QP_OP_load_locals: qp_co_put(e, QP_CPY_LOAD_LOCALS, 0); break;
    case QP_OP_load_from_dict_or_globals:
        qp_co_put(e, QP_CPY_LOAD_FROM_DICT_OR_GLOBALS, qp_co_name_index(e, p));
        break;
    case QP_OP_load_from_dict_or_deref:
        {
            uint32_t v = get_u16(p + 4);
            int k = (v & QP_DEREF_FREE) ? -1 : e->b->arg_count + (int)v;
            if (v & QP_DEREF_FREE) {
                if ((v & ~QP_DEREF_FREE) < (uint32_t)e->b->closure_var_count)
                    qp_co_put(e, QP_CPY_LOAD_FROM_DICT_OR_DEREF,
                              e->L->free_base + (v & ~QP_DEREF_FREE));
            } else if (k < e->b->arg_count + e->b->var_count && e->L->slot[k] >= 0) {
                qp_co_put(e, QP_CPY_LOAD_FROM_DICT_OR_DEREF, e->L->slot[k]);
            }
        }
        break;
    case QP_OP_get_arg_check: qp_co_slot_op(e, TRUE, u16, QP_CO_LOAD_CHECK); break;
    case QP_OP_make_cell:
        /* LOAD_CLOSURE: a LOAD_FAST of the cell in CPython 3.13+ */
        if (u16 < e->b->var_count && e->L->slot[e->b->arg_count + u16] >= 0)
            qp_co_put(e, QP_CPY_LOAD_FAST, e->L->slot[e->b->arg_count + u16]);
        break;
    case QP_OP_call_method: qp_co_put(e, QP_CPY_CALL, u16); break;
    case QP_OP_call_kw: qp_co_put(e, QP_CPY_CALL_KW, u16); break;
    case QP_OP_call_ex: qp_co_put(e, QP_CPY_CALL_FUNCTION_EX, 0); break;
    case QP_OP_build_tuple: qp_co_put(e, QP_CPY_BUILD_TUPLE, u16); break;
    case QP_OP_build_set: qp_co_put(e, QP_CPY_BUILD_SET, u16); break;
    case QP_OP_build_map: qp_co_put(e, QP_CPY_BUILD_MAP, u16); break;
    case QP_OP_build_string: qp_co_put(e, QP_CPY_BUILD_STRING, u16); break;
    case QP_OP_list_append: qp_co_put(e, QP_CPY_LIST_APPEND, u16); break;
    case QP_OP_list_extend: qp_co_put(e, QP_CPY_LIST_EXTEND, u16); break;
    case QP_OP_set_add: qp_co_put(e, QP_CPY_SET_ADD, u16); break;
    case QP_OP_set_update: qp_co_put(e, QP_CPY_SET_UPDATE, u16); break;
    case QP_OP_map_add: qp_co_put(e, QP_CPY_MAP_ADD, u16); break;
    case QP_OP_dict_update: qp_co_put(e, QP_CPY_DICT_UPDATE, u16); break;
    case QP_OP_dict_merge: qp_co_put(e, QP_CPY_DICT_MERGE, u16); break;
    case QP_OP_unpack_seq: qp_co_put(e, QP_CPY_UNPACK_SEQUENCE, u16); break;
    case QP_OP_unpack_ex: qp_co_put(e, QP_CPY_UNPACK_EX, u16); break;
    case QP_OP_convert_value: qp_co_put(e, QP_CPY_CONVERT_VALUE, u8); break;
    case QP_OP_format_simple: qp_co_put(e, QP_CPY_FORMAT_SIMPLE, 0); break;
    case QP_OP_format_with_spec: qp_co_put(e, QP_CPY_FORMAT_WITH_SPEC, 0); break;
    case QP_OP_build_interpolation:
        /* the format spec is always pushed: oparg bit 0 */
        qp_co_put(e, QP_CPY_BUILD_INTERPOLATION, u8 << 2 | 1);
        break;
    case QP_OP_build_template: qp_co_put(e, QP_CPY_BUILD_TEMPLATE, 0); break;
    case QP_OP_get_iter: qp_co_put(e, QP_CPY_GET_ITER, u8); break;
    case QP_OP_for_iter: qp_co_jump(e, QP_CPY_FOR_ITER, pos + 2 + (int32_t)get_u32(p)); break;
    case QP_OP_send: qp_co_jump(e, QP_CPY_SEND, pos + 2 + (int32_t)get_u32(p)); break;
    case QP_OP_yield_value:
        /* YIELD_VALUE oparg 1 inside yield from / await; then CPython's RESUME
           (RESUME_AFTER_YIELD / _YIELD_FROM / _AWAIT) */
        qp_co_put(e, QP_CPY_YIELD_VALUE, u8 == QP_YIELD_DELEGATE);
        qp_co_put(e, QP_CPY_RESUME, u8 != QP_YIELD_DELEGATE ? 1 :
                  (e->b->py_code->co_flags & CO_COROUTINE) ? 3 : 2);
        break;
    case QP_OP_end_send: qp_co_put(e, QP_CPY_END_SEND, 0); break;
    case QP_OP_cleanup_throw: qp_co_put(e, QP_CPY_CLEANUP_THROW, 0); break;
    case QP_OP_get_awaitable: qp_co_put(e, QP_CPY_GET_AWAITABLE, u8); break;
    case QP_OP_get_aiter: qp_co_put(e, QP_CPY_GET_AITER, 0); break;
    case QP_OP_get_anext: qp_co_put(e, QP_CPY_GET_ANEXT, 0); break;
    case QP_OP_end_async_for:
        qp_co_put(e, QP_CPY_END_ASYNC_FOR, 0);
        if (e->readonly)
            qp_co_async_for_branch(e, pos);
        break;
    case QP_OP_raise: qp_co_put(e, QP_CPY_RAISE_VARARGS, u16); break;
    case QP_OP_reraise: qp_co_put(e, QP_CPY_RERAISE, u8); break;
    case QP_OP_push_exc_info: qp_co_put(e, QP_CPY_PUSH_EXC_INFO, 0); break;
    case QP_OP_pop_except: qp_co_put(e, QP_CPY_POP_EXCEPT, 0); break;
    case QP_OP_check_exc_match: qp_co_put(e, QP_CPY_CHECK_EXC_MATCH, 0); break;
    case QP_OP_check_eg_match: qp_co_put(e, QP_CPY_CHECK_EG_MATCH, 0); break;
    case QP_OP_with_except_start: qp_co_put(e, QP_CPY_WITH_EXCEPT_START, 0); break;
    case QP_OP_set_func_attr: qp_co_put(e, QP_CPY_SET_FUNCTION_ATTRIBUTE, u8); break;
    case QP_OP_load_build_class: qp_co_put(e, QP_CPY_LOAD_BUILD_CLASS, 0); break;
    case QP_OP_setup_annotations: qp_co_put(e, QP_CPY_SETUP_ANNOTATIONS, 0); break;
    case QP_OP_import_name:
        qp_co_put(e, QP_CPY_IMPORT_NAME, qp_co_name_index(e, p) << 2 | (p[4] & 3));
        break;
    case QP_OP_import_from: qp_co_put(e, QP_CPY_IMPORT_FROM, qp_co_name_index(e, p)); break;
    case QP_OP_match_class: qp_co_put(e, QP_CPY_MATCH_CLASS, u16); break;
    case QP_OP_match_mapping: qp_co_put(e, QP_CPY_MATCH_MAPPING, 0); break;
    case QP_OP_match_sequence: qp_co_put(e, QP_CPY_MATCH_SEQUENCE, 0); break;
    case QP_OP_match_keys: qp_co_put(e, QP_CPY_MATCH_KEYS, 0); break;
    case QP_OP_get_len: qp_co_put(e, QP_CPY_GET_LEN, 0); break;
    case QP_OP_intrinsic1: qp_co_put(e, QP_CPY_CALL_INTRINSIC_1, u8); break;
    case QP_OP_intrinsic2: qp_co_put(e, QP_CPY_CALL_INTRINSIC_2, u8); break;
    default:
        break;      /* invalid: NOP */
    }
}

/* Whether instruction i, which has no CPython equivalent, needs a NOP: it
   has a line and is the last instruction or the next one is on another
   line. */
static BOOL qp_co_nop_needed(JSFunctionBytecode *b, const QPCoLayout *L, uint32_t i)
{
    QPLoc a, n;

    if (qp_code_addr2loc(b, L->pc[i], &a) < 0 || a.lineno < 0)
        return FALSE;
    if (i + 1 >= L->n)
        return TRUE;
    qp_code_addr2loc(b, L->pc[i + 1], &n);
    return n.lineno != a.lineno;
}

/* CPython instructions of group e->i */
static void qp_co_emit_insn(QPCoEmit *e)
{
    JSFunctionBytecode *b = e->b;
    const uint8_t *buf = b->byte_code_buf;
    uint32_t pos = e->L->pc[e->i];
    int op = buf[pos];
    int32_t v;

    e->first = TRUE;
    if (op == OP_py) {
        qp_co_emit_py(e, buf, pos);
    } else if (qp_co_push_int(buf, pos, &v)) {
        qp_co_load_int(e, v);
    } else {
        switch (op) {
        case OP_push_const:
            qp_co_load_const(e, get_u32(buf + pos + 1));
            break;
        case OP_fclosure:
            qp_co_put(e, QP_CPY_LOAD_CONST, qp_co_const_index(e->L, get_u32(buf + pos + 1)));
            qp_co_put(e, QP_CPY_MAKE_FUNCTION, 0);
            break;
#if SHORT_OPCODES
        case OP_push_const8:
            qp_co_load_const(e, buf[pos + 1]);
            break;
        case OP_fclosure8:
            qp_co_put(e, QP_CPY_LOAD_CONST, qp_co_const_index(e->L, buf[pos + 1]));
            qp_co_put(e, QP_CPY_MAKE_FUNCTION, 0);
            break;
        case OP_push_empty_string:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_EMPTY_STR);
            break;
        case OP_get_loc8: qp_co_slot_op(e, FALSE, buf[pos + 1], QP_CO_LOAD); break;
        case OP_put_loc8: qp_co_slot_op(e, FALSE, buf[pos + 1], QP_CO_STORE); break;
        case OP_set_loc8: qp_co_slot_op(e, FALSE, buf[pos + 1], QP_CO_SET); break;
        case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
            qp_co_slot_op(e, FALSE, op - OP_get_loc0, QP_CO_LOAD);
            break;
        case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
            qp_co_slot_op(e, FALSE, op - OP_put_loc0, QP_CO_STORE);
            break;
        case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
            qp_co_slot_op(e, FALSE, op - OP_set_loc0, QP_CO_SET);
            break;
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
            qp_co_slot_op(e, TRUE, op - OP_get_arg0, QP_CO_LOAD);
            break;
        case OP_put_arg0: case OP_put_arg1: case OP_put_arg2: case OP_put_arg3:
            qp_co_slot_op(e, TRUE, op - OP_put_arg0, QP_CO_STORE);
            break;
        case OP_set_arg0: case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
            qp_co_slot_op(e, TRUE, op - OP_set_arg0, QP_CO_SET);
            break;
        case OP_get_var_ref0: case OP_get_var_ref1: case OP_get_var_ref2: case OP_get_var_ref3:
            qp_co_free_op(e, op - OP_get_var_ref0, QP_CO_LOAD);
            break;
        case OP_put_var_ref0: case OP_put_var_ref1: case OP_put_var_ref2: case OP_put_var_ref3:
            qp_co_free_op(e, op - OP_put_var_ref0, QP_CO_STORE);
            break;
        case OP_set_var_ref0: case OP_set_var_ref1: case OP_set_var_ref2: case OP_set_var_ref3:
            qp_co_free_op(e, op - OP_set_var_ref0, QP_CO_SET);
            break;
        case OP_goto8:
            qp_co_jump(e, QP_CPY_JUMP_FORWARD, pos + 1 + (int8_t)buf[pos + 1]);
            break;
        case OP_goto16:
            qp_co_jump(e, QP_CPY_JUMP_FORWARD, pos + 1 + (int16_t)get_u16(buf + pos + 1));
            break;
        case OP_if_false8:
            qp_co_jump(e, QP_CPY_POP_JUMP_IF_FALSE, pos + 1 + (int8_t)buf[pos + 1]);
            break;
        case OP_if_true8:
            qp_co_jump(e, QP_CPY_POP_JUMP_IF_TRUE, pos + 1 + (int8_t)buf[pos + 1]);
            break;
        case OP_call0: case OP_call1: case OP_call2: case OP_call3:
            qp_co_put(e, QP_CPY_CALL, op - OP_call0);
            break;
#endif
        case OP_undefined:
        case OP_null:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_NONE);
            break;
        case OP_push_true:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_TRUE);
            break;
        case OP_push_false:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_FALSE);
            break;
        case OP_drop:
            qp_co_put(e, QP_CPY_POP_TOP, 0);
            break;
        case OP_nip:
            qp_co_put(e, QP_CPY_SWAP, 2);
            qp_co_put(e, QP_CPY_POP_TOP, 0);
            break;
        case OP_dup:
            qp_co_put(e, QP_CPY_COPY, 1);
            break;
        case OP_swap:
            qp_co_put(e, QP_CPY_SWAP, 2);
            break;
        case OP_get_loc:
        case OP_get_loc_check:
            qp_co_slot_op(e, FALSE, get_u16(buf + pos + 1), QP_CO_LOAD);
            break;
        case OP_put_loc:
            qp_co_slot_op(e, FALSE, get_u16(buf + pos + 1), QP_CO_STORE);
            break;
        case OP_set_loc:
            qp_co_slot_op(e, FALSE, get_u16(buf + pos + 1), QP_CO_SET);
            break;
        case OP_set_loc_uninitialized:
            qp_co_slot_op(e, FALSE, get_u16(buf + pos + 1), QP_CO_DELETE);
            break;
        case OP_get_arg:
            qp_co_slot_op(e, TRUE, get_u16(buf + pos + 1), QP_CO_LOAD);
            break;
        case OP_put_arg:
            qp_co_slot_op(e, TRUE, get_u16(buf + pos + 1), QP_CO_STORE);
            break;
        case OP_set_arg:
            qp_co_slot_op(e, TRUE, get_u16(buf + pos + 1), QP_CO_SET);
            break;
        case OP_get_var_ref:
        case OP_get_var_ref_check:
            qp_co_free_op(e, get_u16(buf + pos + 1), QP_CO_LOAD);
            break;
        case OP_put_var_ref:
            qp_co_free_op(e, get_u16(buf + pos + 1), QP_CO_STORE);
            break;
        case OP_set_var_ref:
            qp_co_free_op(e, get_u16(buf + pos + 1), QP_CO_SET);
            break;
        case OP_goto:
            qp_co_jump(e, QP_CPY_JUMP_FORWARD, pos + 1 + (int32_t)get_u32(buf + pos + 1));
            break;
        case OP_if_false:
            qp_co_jump(e, QP_CPY_POP_JUMP_IF_FALSE, pos + 1 + (int32_t)get_u32(buf + pos + 1));
            break;
        case OP_if_true:
            qp_co_jump(e, QP_CPY_POP_JUMP_IF_TRUE, pos + 1 + (int32_t)get_u32(buf + pos + 1));
            break;
        case OP_lnot:
            qp_co_put(e, QP_CPY_UNARY_NOT, 0);
            break;
        case OP_is_undefined_or_null:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_NONE);
            qp_co_put(e, QP_CPY_IS_OP, 0);
            break;
        case OP_return:
        case OP_return_async:
            qp_co_put(e, QP_CPY_RETURN_VALUE, 0);
            break;
        case OP_return_undef:
            qp_co_put(e, QP_CPY_LOAD_COMMON_CONSTANT, QP_COMMON_CONST_NONE);
            qp_co_put(e, QP_CPY_RETURN_VALUE, 0);
            break;
        case OP_throw:
            qp_co_put(e, QP_CPY_RAISE_VARARGS, 1);
            break;
        case OP_array_from:
            qp_co_put(e, QP_CPY_BUILD_LIST, get_u16(buf + pos + 1));
            break;
        case OP_call:
            qp_co_put(e, QP_CPY_CALL, get_u16(buf + pos + 1));
            break;
        case OP_tail_call:
            qp_co_put(e, QP_CPY_CALL, get_u16(buf + pos + 1));
            qp_co_put(e, QP_CPY_RETURN_VALUE, 0);
            break;
        default:
            break;  /* catch, nip_catch, close_loc, nop, ...: NOP */
        }
    }
    if (e->first) {
        /* no CPython equivalent: a NOP, kept (like CPython's optimizer)
           only to carry a line of its own; otherwise the group is empty
           (its offset is the next group's) */
        if (qp_co_nop_needed(b, e->L, e->i))
            qp_co_put(e, QP_CPY_NOP, 0);
        else if (!e->readonly)
            e->L->lasti[e->i] = e->u;
    }
}

/* code flags of a generator-like code (RETURN_GENERATOR prologue) */
#define QP_CO_GENERATOR_FLAGS (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)

/* CPython's prologue (COPY_FREE_VARS, MAKE_CELL, RETURN_GENERATOR +
   POP_TOP, RESUME) from code unit 0 */
static void qp_co_emit_prologue(QPCoEmit *e)
{
    QPCoLayout *L = e->L;
    JSFunctionBytecode *b = e->b;
    uint32_t i;

    e->u = 0;
    e->first = FALSE;
    if (b->closure_var_count)
        qp_co_put(e, QP_CPY_COPY_FREE_VARS, b->closure_var_count);
    for (i = 0; i < L->ncells; i++)
        qp_co_put(e, QP_CPY_MAKE_CELL, L->cells[i]);
    if (!e->readonly)
        L->pro_noloc = e->u;
    if (b->py_code->co_flags & QP_CO_GENERATOR_FLAGS) {
        qp_co_put(e, QP_CPY_RETURN_GENERATOR, 0);
        qp_co_put(e, QP_CPY_POP_TOP, 0);
    }
    if (!e->readonly)
        L->pro_gen = e->u - L->pro_noloc;
    qp_co_put(e, QP_CPY_RESUME, 0);
}

/* One pass over the code: layout pass (!e->readonly) or emission. */
static void qp_co_emit_all(QPCoEmit *e)
{
    QPCoLayout *L = e->L;
    uint32_t i;

    e->changed = FALSE;
    qp_co_emit_prologue(e);
    for (i = 0; i < L->n; i++) {
        if (!e->readonly && L->unit[i] != e->u) {
            L->unit[i] = e->u;
            e->changed = TRUE;
        }
        e->i = i;
        qp_co_emit_insn(e);
    }
    if (!e->readonly && L->unit[L->n] != e->u) {
        L->unit[L->n] = e->u;
        e->changed = TRUE;
    }
}

/* localsplus index of every parameter / local variable, of the cells and
   of the first free variable (qp_co_localsplus_names order: co_varnames,
   the cells that are not parameters, co_freevars) */
static int qp_co_layout_slots(JSContext *ctx, JSFunctionBytecode *b, QPCoLayout *L)
{
    JSAtom *v = NULL, *c = NULL;
    int nv, nc, i, j, k, extra = 0, ret = -1;
    int total = b->arg_count + b->var_count;

    nv = qp_co_kind_names(ctx, b, QP_CO_VARNAMES, &v);
    if (nv < 0)
        return -1;
    nc = qp_co_kind_names(ctx, b, QP_CO_CELLVARS, &c);
    if (nc < 0)
        goto done;
    L->slot = js_malloc(ctx, sizeof(int32_t) * (total + 1));
    L->cells = js_malloc(ctx, sizeof(uint32_t) * (nc + 1));
    if (!L->slot || !L->cells)
        goto done;
    /* cells[j]: index of cell c[j] (a parameter keeps its co_varnames index) */
    for (j = 0; j < nc; j++) {
        for (k = 0; k < nv && v[k] != c[j]; k++)
            continue;
        L->cells[j] = k < nv ? k : nv + extra++;
    }
    L->ncells = nc;
    L->free_base = nv + extra;
    for (i = 0; i < total; i++) {
        JSBytecodeVarDef *vd = &b->vardefs[i];
        L->slot[i] = -1;
        if (vd->var_name == JS_ATOM_NULL)
            continue;
        if (vd->is_captured) {
            for (j = 0; j < nc; j++) {
                if (c[j] == vd->var_name) {
                    L->slot[i] = L->cells[j];
                    break;
                }
            }
        } else {
            for (j = 0; j < nv; j++) {
                if (v[j] == vd->var_name) {
                    L->slot[i] = j;
                    break;
                }
            }
        }
    }
    /* MAKE_CELL order: ascending localsplus index */
    for (i = 1; i < nc; i++) {
        uint32_t x = L->cells[i];
        for (j = i - 1; j >= 0 && L->cells[j] > x; j--)
            L->cells[j + 1] = L->cells[j];
        L->cells[j + 1] = x;
    }
    ret = 0;
 done:
    js_free(ctx, v);
    js_free(ctx, c);
    return ret;
}

/* co_consts (CPython's remove_unused_consts over QPCodeInfo.consts): the
   first entry, then the entries the bytecode loads (push_const / fclosure
   of their cpool entry, push_i32 of an int).  A loaded cpool entry or int
   outside of the table is appended (not expected: the compiler adds every
   constant it loads). */
static int qp_co_layout_consts(JSContext *ctx, JSFunctionBytecode *b, QPCoLayout *L)
{
    QPCodeInfo *ci = b->py_code;
    const uint8_t *buf = b->byte_code_buf;
    uint32_t pos, len = b->byte_code_len, i, idx, ncpool = b->cpool_count;
    uint8_t *used;
    int ret = -1;

    used = js_mallocz(ctx, ncpool + 1);
    L->cpool_co = js_malloc(ctx, sizeof(int32_t) * (ncpool + 1));
    L->co = js_malloc(ctx, sizeof(L->co[0]) * (ci->nconsts + ncpool + L->nints + 1));
    if (!used || !L->cpool_co || !L->co)
        goto done;
    for (i = 0; i < ncpool; i++)
        L->cpool_co[i] = -1;
    for (i = 0; i < L->nints; i++)
        L->int_co[i] = -1;
    for (pos = 0; pos < len; pos += qp_co_insn_size(buf, pos)) {
        switch (buf[pos]) {
        case OP_push_const:
        case OP_fclosure:
            idx = get_u32(buf + pos + 1);
            break;
#if SHORT_OPCODES
        case OP_push_const8:
        case OP_fclosure8:
            idx = buf[pos + 1];
            break;
#endif
        default:
            continue;
        }
        if (idx < ncpool)
            used[idx] = 1;
    }
    for (i = 0; i < ci->nconsts; ) {
        /* entry i and its aliases [i + 1, j) (closures of one code object) */
        uint32_t j, item = ncpool;
        BOOL use = i == 0;
        int k = -1;
        for (j = i; j < ci->nconsts && (j == i || (ci->consts[j] & QP_CODE_CONST_ALIAS)); j++) {
            idx = ci->consts[j] & ~QP_CODE_CONST_ALIAS;
            if (idx >= ncpool)
                continue;
            if (item == ncpool || (used[idx] && !used[item]))
                item = idx;
            use |= used[idx];
        }
        if (item < ncpool && JS_VALUE_GET_TAG(b->cpool[item]) == JS_TAG_INT)
            k = qp_co_hash_find(&L->int_hash, L->ints, (uint32_t)JS_VALUE_GET_INT(b->cpool[item]));
        if (item < ncpool && (use || k >= 0)) {
            for (; i < j; i++) {
                idx = ci->consts[i] & ~QP_CODE_CONST_ALIAS;
                if (idx < ncpool && L->cpool_co[idx] < 0)
                    L->cpool_co[idx] = L->nco;
            }
            if (k >= 0 && L->int_co[k] < 0)
                L->int_co[k] = L->nco;
            L->co[L->nco++] = item;
        }
        i = j;
    }
    for (i = 0; i < ncpool; i++) {
        if (used[i] && L->cpool_co[i] < 0 && !qp_co_is_empty_str(b->cpool[i])) {
            L->cpool_co[i] = L->nco;
            L->co[L->nco++] = i;
        }
    }
    for (i = 0; i < L->nints; i++) {
        if (L->int_co[i] < 0) {
            L->int_co[i] = L->nco;
            L->co[L->nco++] = QP_CO_INT | i;
        }
    }
    ret = 0;
 done:
    js_free(ctx, used);
    return ret;
}

/* The co_code layout of b (cached in b->py_code), NULL with an exception. */
static QPCoLayout *qp_co_layout(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    const uint8_t *buf = b->byte_code_buf;
    uint32_t n = 0, nname = 0, nint = 0, pos, len = b->byte_code_len;
    QPCoLayout *L;
    QPCoEmit e;
    int32_t v;

    if (ci->co_layout)
        return ci->co_layout;
    for (pos = 0; pos < len; pos += qp_co_insn_size(buf, pos)) {
        n++;
        if (qp_co_name_operand(buf, pos) != JS_ATOM_NULL)
            nname++;
        else if (qp_co_push_int(buf, pos, &v) && qp_co_int_is_const(v))
            nint++;
    }
    L = js_mallocz(ctx, sizeof(*L));
    if (!L)
        return NULL;
    L->n = n;
    L->pc = js_malloc(ctx, sizeof(uint32_t) * (n + 1));
    L->unit = js_mallocz(ctx, sizeof(uint32_t) * (n + 1));
    L->lasti = js_mallocz(ctx, sizeof(uint32_t) * (n + 1));
    L->jext = js_mallocz(ctx, n + 1);
    L->names = js_malloc(ctx, sizeof(uint32_t) * (nname + 1));
    L->ints = js_malloc(ctx, sizeof(uint32_t) * (nint + 1));
    L->int_co = js_mallocz(ctx, sizeof(int32_t) * (nint + 1));
    if (!L->pc || !L->unit || !L->lasti || !L->jext || !L->names || !L->ints ||
        !L->int_co ||
        qp_co_hash_init(ctx, &L->name_hash, nname) < 0 ||
        qp_co_hash_init(ctx, &L->int_hash, nint) < 0 ||
        qp_co_layout_slots(ctx, b, L) < 0)
        goto fail;
    n = 0;
    for (pos = 0; pos < len; pos += qp_co_insn_size(buf, pos)) {
        JSAtom a = qp_co_name_operand(buf, pos);
        L->pc[n++] = pos;
        if (a != JS_ATOM_NULL)
            qp_co_hash_intern(&L->name_hash, L->names, &L->nnames, a);
        else if (qp_co_push_int(buf, pos, &v) && qp_co_int_is_const(v))
            qp_co_hash_intern(&L->int_hash, L->ints, &L->nints, (uint32_t)v);
    }
    L->pc[n] = len;
    if (qp_co_layout_consts(ctx, b, L) < 0)
        goto fail;
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    do {
        qp_co_emit_all(&e);
    } while (e.changed);
    ci->co_layout = L;
    return L;
 fail:
    qp_co_layout_free(ctx->rt, L);
    return NULL;
}

/* qp_co_layout for the offset conversions (no context argument: the
   bytecode's realm; an allocation failure gives NULL and no exception) */
static QPCoLayout *qp_co_layout_quiet(JSFunctionBytecode *b)
{
    JSContext *ctx = b->realm;
    QPCoLayout *L;
    BOOL had;

    if (!b->py_code)
        return NULL;
    if (b->py_code->co_layout)
        return b->py_code->co_layout;
    if (!ctx)
        return NULL;
    had = JS_HasException(ctx);
    L = qp_co_layout(ctx, b);
    if (!L && !had)
        JS_FreeValue(ctx, JS_GetException(ctx));
    return L;
}

static int qp_code_pc2lasti(JSFunctionBytecode *b, uint32_t pc)
{
    QPCoLayout *L;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    return 2 * L->lasti[qp_co_pc_index(L, pc)];
}

static int qp_code_lasti2pc(JSFunctionBytecode *b, int lasti)
{
    QPCoLayout *L;

    if (lasti < 0 || !(L = qp_co_layout_quiet(b)) || L->n == 0 ||
        (uint32_t)lasti / 2 >= L->unit[L->n])
        return -1;
    return L->pc[qp_co_unit_index(L, (uint32_t)lasti / 2)];
}

/* Python offset of the group of the instruction starting at byte offset pc
   (its first code unit, EXTENDED_ARG prefixes included: CPython's jump
   target offset, the not_taken / target offsets of co_branches()), -1 if
   pc is outside the bytecode. */
static __maybe_unused int qp_code_target_offset(JSFunctionBytecode *b, uint32_t pc)
{
    QPCoLayout *L;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    return 2 * L->unit[qp_co_pc_index(L, pc)];
}

/* Python offset of a RESUME instruction (sys.monitoring PY_START /
   PY_RESUME): pc 0 gives the RESUME ending the prologue, the byte offset of
   a yield_value subop the RESUME after its YIELD_VALUE; -1 otherwise. */
static __maybe_unused int qp_code_resume_offset(JSFunctionBytecode *b, uint32_t pc)
{
    QPCoLayout *L;
    const uint8_t *buf = b->byte_code_buf;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    if (pc == 0)
        return 2 * (L->unit[0] - 1 - qp_cpy_caches(QP_CPY_RESUME));
    if (buf[pc] != OP_py || buf[pc + 1] != QP_OP_yield_value || L->pc[qp_co_pc_index(L, pc)] != pc)
        return -1;
    /* group: YIELD_VALUE 0/1 (no prefix, no cache), RESUME */
    return 2 * (L->lasti[qp_co_pc_index(L, pc)] + 1);
}

/* Python offsets (execution order, without EXTENDED_ARG and CACHE units)
   of the CPython instructions of the instruction at byte offset pc, the
   prologue's first for pc 0: up to max into out; returns their count, -1
   if pc is outside the bytecode (sys.monitoring INSTRUCTION events). */
static __maybe_unused int qp_code_insn_offsets(JSFunctionBytecode *b, uint32_t pc, int *out, int max)
{
    QPCoLayout *L;
    QPCoEmit e;
    int k;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    e.readonly = TRUE;
    e.ops = out;
    e.maxops = max;
    e.i = qp_co_pc_index(L, pc);
    if (pc == 0)
        qp_co_emit_prologue(&e);
    e.u = L->unit[e.i];
    qp_co_emit_insn(&e);
    for (k = 0; k < e.nops && k < max; k++)
        out[k] *= 2;
    return e.nops;
}

/* Python offset of the branch instruction (POP_JUMP_IF_* / FOR_ITER) of the
   conditional jump or for_iter at byte offset pc: the src of its
   co_branches() entry; -1 if the instruction is not a branch. */
static __maybe_unused int qp_code_branch_src(JSFunctionBytecode *b, uint32_t pc)
{
    QPCoLayout *L;
    QPCoEmit e;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    e.readonly = TRUE;
    e.branch_src = -1;
    e.branch_back = -1;
    e.i = qp_co_pc_index(L, pc);
    e.u = L->unit[e.i];
    qp_co_emit_insn(&e);
    return e.branch_src < 0 ? -1 : 2 * e.branch_src;
}

/* Python offset of the JUMP_BACKWARD of the conditional jump at byte
   offset pc when its target is backward (co_code: the inverted
   POP_JUMP_IF_* over a JUMP_BACKWARD, as CPython's normalize_jumps; the
   jump is taken through the not_taken entry of co_branches()), -1 if it
   is not such a jump. */
static __maybe_unused int qp_code_branch_back(JSFunctionBytecode *b, uint32_t pc)
{
    QPCoLayout *L;
    QPCoEmit e;

    if (pc >= (uint32_t)b->byte_code_len || !(L = qp_co_layout_quiet(b)) || L->n == 0)
        return -1;
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    e.readonly = TRUE;
    e.branch_src = -1;
    e.branch_back = -1;
    e.i = qp_co_pc_index(L, pc);
    e.u = L->unit[e.i];
    qp_co_emit_insn(&e);
    return e.branch_back < 0 ? -1 : 2 * e.branch_back;
}

/* co_code (see above) */
static JSValue qp_co_code(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCoLayout *L = qp_co_layout(ctx, b);
    QPCoEmit e;
    JSValue res;

    if (!L)
        return JS_EXCEPTION;
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    e.readonly = TRUE;
    e.out = js_malloc(ctx, 2 * L->unit[L->n] + 1);
    if (!e.out)
        return JS_EXCEPTION;
    qp_co_emit_all(&e);
    assert(e.u == L->unit[L->n]);
    res = qp_bytes_new(ctx, e.out, 2 * e.u);
    js_free(ctx, e.out);
    return res;
}

/* co_names: operands of the name subops, first-use order, no duplicates */
static JSValue qp_co_names(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCoLayout *L = qp_co_layout(ctx, b);

    if (!L)
        return JS_EXCEPTION;
    return qp_co_atoms_tuple(ctx, L->names, L->nnames);
}

/* co_consts (qp_co_layout_consts), nested bytecodes as code objects */
static JSValue qp_co_consts(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCoLayout *L = qp_co_layout(ctx, b);
    JSValue t, v;
    uint32_t i;

    if (!L)
        return JS_EXCEPTION;
    t = qp_tuple_new(ctx, L->nco);
    if (JS_IsException(t))
        return JS_EXCEPTION;
    for (i = 0; i < L->nco; i++) {
        JSValueConst item;
        if (L->co[i] & QP_CO_INT) {
            v = JS_NewInt32(ctx, (int32_t)L->ints[L->co[i] & ~QP_CO_INT]);
        } else if (item = b->cpool[L->co[i]],
                   JS_VALUE_GET_TAG(item) == JS_TAG_FUNCTION_BYTECODE &&
                   ((JSFunctionBytecode *)JS_VALUE_GET_PTR(item))->is_py) {
            v = qp_code_new(ctx, item);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, t);
                return JS_EXCEPTION;
            }
        } else {
            v = JS_DupValue(ctx, item);
        }
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(t)[i]);
        QP_TUPLE_ITEMS(t)[i] = v;
    }
    return t;
}

/* ---- co_linetable (CPython's location table, Objects/locations.md) ------- */

enum {
    QP_LT_SHORT0 = 0,
    QP_LT_ONE_LINE0 = 10,
    QP_LT_NO_COLUMNS = 13,
    QP_LT_LONG = 14,
    QP_LT_NONE = 15,
};

static void qp_lt_varint(DynBuf *db, uint32_t val)
{
    while (val >= 64) {
        dbuf_putc(db, 64 | (val & 63));
        val >>= 6;
    }
    dbuf_putc(db, val);
}

static void qp_lt_svarint(DynBuf *db, int val)
{
    qp_lt_varint(db, val < 0 ? ((0 - (uint32_t)val) << 1) | 1 : (uint32_t)val << 1);
}

/* one entry of 1..8 units (assemble.c write_location_info_entry) */
static void qp_lt_entry(DynBuf *db, int *pline, const QPLoc *loc, int isize)
{
    int line_delta = loc->lineno - *pline;
    int col = loc->col, end_col = loc->end_col, end_line = loc->end_lineno;

#define QP_LT_FIRST(code) dbuf_putc(db, 128 | ((code) << 3) | (isize - 1))
    if (loc->lineno < 0) {
        QP_LT_FIRST(QP_LT_NONE);
        return;
    }
    if (end_line < loc->lineno)
        end_line = loc->lineno;     /* not representable: the line only */
    if (col < 0 || end_col < 0) {
        if (end_line == loc->lineno) {
            QP_LT_FIRST(QP_LT_NO_COLUMNS);
            qp_lt_svarint(db, line_delta);
            *pline = loc->lineno;
            return;
        }
    } else if (end_line == loc->lineno) {
        if (line_delta == 0 && col < 80 && end_col - col < 16 && end_col >= col) {
            QP_LT_FIRST(QP_LT_SHORT0 + (col >> 3));
            dbuf_putc(db, ((col & 7) << 4) | (end_col - col));
            return;
        }
        if (line_delta >= 0 && line_delta < 3 && col < 128 && end_col < 128) {
            QP_LT_FIRST(QP_LT_ONE_LINE0 + line_delta);
            dbuf_putc(db, col);
            dbuf_putc(db, end_col);
            *pline = loc->lineno;
            return;
        }
    }
    QP_LT_FIRST(QP_LT_LONG);
    qp_lt_svarint(db, line_delta);
    qp_lt_varint(db, end_line - loc->lineno);
    qp_lt_varint(db, col + 1);
    qp_lt_varint(db, end_col + 1);
    *pline = loc->lineno;
#undef QP_LT_FIRST
}

static void qp_lt_emit(DynBuf *db, int *pline, const QPLoc *loc, uint32_t isize)
{
    for (; isize > 8; isize -= 8)
        qp_lt_entry(db, pline, loc, 8);
    if (isize > 0)
        qp_lt_entry(db, pline, loc, isize);
}

static BOOL qp_loc_eq(const QPLoc *a, const QPLoc *b)
{
    return a->lineno == b->lineno && a->end_lineno == b->end_lineno &&
        a->col == b->col && a->end_col == b->end_col;
}

/* co_linetable of a non-explicit code object (firstlineno: its
   co_firstlineno, the base of the line deltas) */
static JSValue qp_co_linetable(JSContext *ctx, JSFunctionBytecode *b, int firstlineno)
{
    QPCoLayout *L = qp_co_layout(ctx, b);
    QPCodeInfo *ci = b->py_code;
    BOOL module = !(ci->co_flags & CO_NEWLOCALS) && b->func_name == QP_ATOM(ctx, anon_module);
    QPLoc cur, loc;
    uint32_t i, size = 0;
    int line = firstlineno;
    DynBuf db;
    JSValue res;

    if (!L)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &db);
    /* prologue: COPY_FREE_VARS / MAKE_CELL have no location, RETURN_GENERATOR
       + POP_TOP the definition line, RESUME (firstlineno, firstlineno, 0, 0)
       ((0, 1, 0, 0) in a module) */
    cur.lineno = cur.end_lineno = cur.col = cur.end_col = -1;
    qp_lt_emit(&db, &line, &cur, L->pro_noloc);
    cur.lineno = cur.end_lineno = firstlineno;
    qp_lt_emit(&db, &line, &cur, L->pro_gen);
    cur.lineno = module ? 0 : firstlineno;
    cur.end_lineno = module ? 1 : firstlineno;
    cur.col = cur.end_col = 0;
    if (ctx->py && ctx->py->config.no_debug_ranges) {
        /* _PyCode_New: remove_column_info */
        cur.end_lineno = cur.lineno;
        cur.col = cur.end_col = -1;
    }
    size = 1 + qp_cpy_caches(QP_CPY_RESUME);
    for (i = 0; i < L->n; i++) {
        if (qp_code_addr2loc(b, L->pc[i], &loc) < 0)
            loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
        if (!qp_loc_eq(&loc, &cur)) {
            qp_lt_emit(&db, &line, &cur, size);
            cur = loc;
            size = 0;
        }
        size += L->unit[i + 1] - L->unit[i];
    }
    qp_lt_emit(&db, &line, &cur, size);
    if (dbuf_error(&db)) {
        dbuf_free(&db);
        return JS_ThrowOutOfMemory(ctx);
    }
    res = qp_bytes_new(ctx, db.buf, db.size);
    dbuf_free(&db);
    return res;
}

/* Decoder (Objects/codeobject.c advance_with_locations): calls fn for each
   entry with its code unit range [start, end) and location; stops at a
   malformed entry.  0, or fn's negative result. */
static int qp_lt_decode(const uint8_t *p, size_t len, int firstlineno,
                        int (*fn)(void *opaque, uint32_t start, uint32_t end, const QPLoc *loc),
                        void *opaque)
{
    const uint8_t *end = p + len;
    uint32_t unit = 0;
    int line = firstlineno;

#define QP_LT_BYTE(var) do { if (p >= end) return 0; (var) = *p++; } while (0)
    while (p < end) {
        int first = *p++, code = (first >> 3) & 15, r;
        uint32_t n = (first & 7) + 1;
        QPLoc loc;

        if (!(first & 128))
            return 0;
        switch (code) {
        case QP_LT_NONE:
            loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
            break;
        case QP_LT_LONG:
        case QP_LT_NO_COLUMNS:
            {
                uint32_t vals[4];
                int k, nv = code == QP_LT_LONG ? 4 : 1;
                for (k = 0; k < nv; k++) {
                    uint32_t b, val, shift = 0;
                    QP_LT_BYTE(b);
                    val = b & 63;
                    while (b & 64) {
                        QP_LT_BYTE(b);
                        shift += 6;
                        val |= (b & 63) << shift;
                    }
                    vals[k] = val;
                }
                line += (vals[0] & 1) ? -(int)(vals[0] >> 1) : (int)(vals[0] >> 1);
                loc.lineno = line;
                if (code == QP_LT_LONG) {
                    loc.end_lineno = line + (int)vals[1];
                    loc.col = (int)vals[2] - 1;
                    loc.end_col = (int)vals[3] - 1;
                } else {
                    loc.end_lineno = line;
                    loc.col = loc.end_col = -1;
                }
            }
            break;
        case QP_LT_ONE_LINE0:
        case QP_LT_ONE_LINE0 + 1:
        case QP_LT_ONE_LINE0 + 2:
            line += code - QP_LT_ONE_LINE0;
            loc.lineno = loc.end_lineno = line;
            QP_LT_BYTE(loc.col);
            QP_LT_BYTE(loc.end_col);
            break;
        default:
            {
                int second;
                QP_LT_BYTE(second);
                loc.lineno = loc.end_lineno = line;
                loc.col = code << 3 | (second >> 4);
                loc.end_col = loc.col + (second & 15);
            }
            break;
        }
        r = fn(opaque, unit, unit + n, &loc);
        if (r < 0)
            return r;
        unit += n;
    }
#undef QP_LT_BYTE
    return 0;
}

/* ---- co_exceptiontable (InternalDocs/exception_handling.md) ------------- */

/* assemble.c assemble_emit_exception_table_item */
static void qp_et_item(DynBuf *db, uint32_t value, int msb)
{
    int shift;

    for (shift = 24; shift > 0; shift -= 6) {
        if (value >= (1u << shift)) {
            dbuf_putc(db, ((value >> shift) & 0x3f) | 64 | msb);
            msb = 0;
        }
    }
    dbuf_putc(db, (value & 0x3f) | msb);
}

/* The handler of every group is the innermost catch active when its
   instruction starts (compute_stack_size_tabs); consecutive groups with
   the same (target, depth) form one entry. */
static int qp_co_exctab_build(JSContext *ctx, JSFunctionBytecode *b, QPCoLayout *L)
{
    const uint8_t *buf = b->byte_code_buf;
    uint32_t len = b->byte_code_len, i, start = 0;
    uint16_t *level = NULL;
    int32_t *catchpos = NULL, cur_target = -1, cur_depth = 0;
    int ss;
    DynBuf db;

    if (L->n > 0 && compute_stack_size_tabs(ctx, buf, len, &ss, &level, &catchpos) < 0)
        return -1;
    js_dbuf_init(ctx, &db);
    for (i = 0; i <= L->n; i++) {
        int32_t target = -1, depth = 0;
        if (i < L->n) {
            uint32_t pc = L->pc[i];
            int32_t cp;
            if (level[pc] != 0xffff && (cp = catchpos[pc]) >= 0 && buf[cp] == OP_catch &&
                level[cp] != 0xffff) {
                uint32_t tpc = cp + 1 + (int32_t)get_u32(buf + cp + 1);
                if (tpc < len) {
                    target = L->unit[qp_co_pc_index(L, tpc)];
                    depth = level[cp];
                }
            }
        }
        if (target == cur_target && depth == cur_depth)
            continue;
        if (cur_target >= 0 && L->unit[i] > start) {
            qp_et_item(&db, start, 128);
            qp_et_item(&db, L->unit[i] - start, 0);
            qp_et_item(&db, cur_target, 0);
            qp_et_item(&db, (uint32_t)cur_depth << 1, 0);
        }
        cur_target = target;
        cur_depth = depth;
        start = L->unit[i];
    }
    js_free(ctx, level);
    js_free(ctx, catchpos);
    if (dbuf_error(&db)) {
        dbuf_free(&db);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    L->exctab = db.size ? js_malloc(ctx, db.size) : NULL;
    if (db.size && !L->exctab) {
        dbuf_free(&db);
        return -1;
    }
    if (db.size)
        memcpy(L->exctab, db.buf, db.size);
    L->exctab_len = db.size;
    L->has_exctab = TRUE;
    dbuf_free(&db);
    return 0;
}

static JSValue qp_co_exceptiontable(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCoLayout *L = qp_co_layout(ctx, b);

    if (!L || (!L->has_exctab && qp_co_exctab_build(ctx, b, L) < 0))
        return JS_EXCEPTION;
    return qp_bytes_new(ctx, L->exctab ? (const char *)L->exctab : "", L->exctab_len);
}

/* ---- getters ------------------------------------------------------------- */

enum {
    QP_CA_ARGCOUNT,
    QP_CA_POSONLYARGCOUNT,
    QP_CA_KWONLYARGCOUNT,
    QP_CA_STACKSIZE,
    QP_CA_FLAGS,
    QP_CA_NLOCALS,
    QP_CA_CONSTS,
    QP_CA_NAMES,
    QP_CA_FILENAME,
    QP_CA_NAME,
    QP_CA_QUALNAME,
    QP_CA_FIRSTLINENO,
    QP_CA_LINETABLE,
    QP_CA_EXCEPTIONTABLE,
    QP_CA_VARNAMES,
    QP_CA_CELLVARS,
    QP_CA_FREEVARS,
    QP_CA_CODE,
    QP_CA_CODE_ADAPTIVE,
};

/* code() fields, in argument order (QPCodeObject.fields) */
enum {
    QP_CF_ARGCOUNT, QP_CF_POSONLYARGCOUNT, QP_CF_KWONLYARGCOUNT, QP_CF_NLOCALS,
    QP_CF_STACKSIZE, QP_CF_FLAGS, QP_CF_CODE, QP_CF_CONSTS, QP_CF_NAMES,
    QP_CF_VARNAMES, QP_CF_FILENAME, QP_CF_NAME, QP_CF_QUALNAME, QP_CF_FIRSTLINENO,
    QP_CF_LINETABLE, QP_CF_EXCEPTIONTABLE, QP_CF_FREEVARS, QP_CF_CELLVARS,
    QP_CF__COUNT
};

/* field of a getter (QP_CA_*) */
static int qp_co_field_index(int magic)
{
    switch (magic) {
    case QP_CA_ARGCOUNT: return QP_CF_ARGCOUNT;
    case QP_CA_POSONLYARGCOUNT: return QP_CF_POSONLYARGCOUNT;
    case QP_CA_KWONLYARGCOUNT: return QP_CF_KWONLYARGCOUNT;
    case QP_CA_STACKSIZE: return QP_CF_STACKSIZE;
    case QP_CA_FLAGS: return QP_CF_FLAGS;
    case QP_CA_NLOCALS: return QP_CF_NLOCALS;
    case QP_CA_CONSTS: return QP_CF_CONSTS;
    case QP_CA_NAMES: return QP_CF_NAMES;
    case QP_CA_FILENAME: return QP_CF_FILENAME;
    case QP_CA_NAME: return QP_CF_NAME;
    case QP_CA_QUALNAME: return QP_CF_QUALNAME;
    case QP_CA_FIRSTLINENO: return QP_CF_FIRSTLINENO;
    case QP_CA_LINETABLE: return QP_CF_LINETABLE;
    case QP_CA_EXCEPTIONTABLE: return QP_CF_EXCEPTIONTABLE;
    case QP_CA_VARNAMES: return QP_CF_VARNAMES;
    case QP_CA_CELLVARS: return QP_CF_CELLVARS;
    case QP_CA_FREEVARS: return QP_CF_FREEVARS;
    default: return QP_CF_CODE;   /* QP_CA_CODE, QP_CA_CODE_ADAPTIVE */
    }
}

static JSValue qp_co_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b;
    QPCodeInfo *ci;
    int n;

    if (!c)
        return qp_raise_type_error(ctx, "descriptor requires a 'code' object but received '%T'", self);
    if (!QP_IS_NULL(c->fields))
        return JS_DupValue(ctx, QP_TUPLE_ITEMS(c->fields)[qp_co_field_index(magic)]);
    b = JS_VALUE_GET_PTR(c->bytecode);
    ci = b->py_code;
    switch (magic) {
    case QP_CA_ARGCOUNT:
        return JS_NewInt32(ctx, ci->argcount);
    case QP_CA_POSONLYARGCOUNT:
        return JS_NewInt32(ctx, ci->posonlyargcount);
    case QP_CA_KWONLYARGCOUNT:
        return JS_NewInt32(ctx, ci->kwonlyargcount);
    case QP_CA_STACKSIZE:
        return JS_NewInt32(ctx, b->stack_size);
    case QP_CA_FLAGS:
        return JS_NewInt32(ctx, (int32_t)((ci->co_flags & ~QP_CO_NATIVE) | c->flags_add));
    case QP_CA_NLOCALS:
        n = qp_co_nlocals(ctx, b);
        if (n < 0)
            return JS_EXCEPTION;
        return JS_NewInt32(ctx, n);
    case QP_CA_CONSTS:
        return qp_co_consts(ctx, b);
    case QP_CA_NAMES:
        return qp_co_names(ctx, b);
    case QP_CA_FILENAME:
        return qp_co_get_filename(ctx, self);
    case QP_CA_NAME:
        return qp_co_get_name(ctx, self);
    case QP_CA_QUALNAME:
        return qp_co_get_qualname(ctx, self);
    case QP_CA_FIRSTLINENO:
        return JS_NewInt32(ctx, qp_co_get_firstlineno(self));
    case QP_CA_LINETABLE:
        if (!QP_IS_NULL(c->linetable))
            return JS_DupValue(ctx, c->linetable);
        return qp_co_linetable(ctx, b, qp_co_get_firstlineno(self));
    case QP_CA_EXCEPTIONTABLE:
        if (!QP_IS_NULL(c->exctable))
            return JS_DupValue(ctx, c->exctable);
        return qp_co_exceptiontable(ctx, b);
    case QP_CA_VARNAMES:
        return qp_co_kind_tuple(ctx, b, QP_CO_VARNAMES);
    case QP_CA_CELLVARS:
        return qp_co_kind_tuple(ctx, b, QP_CO_CELLVARS);
    case QP_CA_FREEVARS:
        return qp_co_kind_tuple(ctx, b, QP_CO_FREEVARS);
    default:
        return qp_co_code(ctx, b);
    }
}

/* ---- methods ------------------------------------------------------------- */

/* int, or None for -1 (_source_offset_converter). */
static JSValue qp_co_int_or_none(JSContext *ctx, int v)
{
    return v < 0 ? JS_UNDEFINED : JS_NewInt32(ctx, v);
}

/* Append the tuple of n (<= 4) ints-or-None (-1) to list: 0 or -1. */
static int qp_co_append_ints(JSContext *ctx, JSValueConst list, int n, const int *vals)
{
    JSValue items[4], tup;
    int i, r;

    for (i = 0; i < n; i++)
        items[i] = qp_co_int_or_none(ctx, vals[i]);
    tup = qp_tuple_new_steal(ctx, n, items);
    if (JS_IsException(tup))
        return -1;
    r = qp_list_append(ctx, list, tup);
    JS_FreeValue(ctx, tup);
    return r;
}

typedef struct QPCoLinesState {
    JSContext *ctx;
    JSValue list;
    BOOL have;
    int v[3];               /* start, end, line of the pending range */
} QPCoLinesState;

static int qp_co_lines_cb(void *opaque, uint32_t start, uint32_t end, const QPLoc *loc)
{
    QPCoLinesState *s = opaque;

    if (s->have && loc->lineno == s->v[2]) {
        s->v[1] = 2 * end;  /* merge (lineiter_next) */
        return 0;
    }
    if (s->have && qp_co_append_ints(s->ctx, s->list, 3, s->v) < 0)
        return -1;
    s->have = TRUE;
    s->v[0] = 2 * start;
    s->v[1] = 2 * end;
    s->v[2] = loc->lineno;
    return 0;
}

/* co_lines(): iterator of (start, end, line) ranges of co_linetable,
   consecutive ranges with the same line merged (lineiter_next). */
static JSValue qp_co_lines(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    QPCoLinesState s;
    JSValue table, it;
    Py_ssize_t len;
    const uint8_t *p;

    table = qp_co_get(ctx, self, QP_CA_LINETABLE);
    if (JS_IsException(table))
        return JS_EXCEPTION;
    s.ctx = ctx;
    s.have = FALSE;
    s.list = qp_list_new(ctx, 0);
    if (JS_IsException(s.list))
        goto fail;
    p = (const uint8_t *)qp_bytes_data(table, &len);
    if (qp_lt_decode(p, len, qp_co_get_firstlineno(self), qp_co_lines_cb, &s) < 0 ||
        (s.have && qp_co_append_ints(ctx, s.list, 3, s.v) < 0))
        goto fail;
    JS_FreeValue(ctx, table);
    it = qp_iter(ctx, s.list);
    JS_FreeValue(ctx, s.list);
    return it;
 fail:
    JS_FreeValue(ctx, table);
    JS_FreeValue(ctx, s.list);
    return JS_EXCEPTION;
}

/* The branches of the async for loops, inserted by src into the sorted
   entries of db: END_ASYNC_FOR branches from the END_SEND of the __anext__
   await (catch(handler) ... end_send ... handler: end_async_for) to the
   next instruction or past END_ASYNC_FOR (the sys.monitoring BRANCH events
   of instrumentation.c).  -1 with MemoryError. */
static int qp_co_afor_branches(JSFunctionBytecode *b, const QPCoLayout *L, DynBuf *db)
{
    const uint8_t *buf = b->byte_code_buf;
    uint32_t i, k, j, n;
    int32_t v[3], w[3];

    for (i = 0; i < L->n; i++) {
        uint32_t pos = L->pc[i], target, t;
        if (buf[pos] != OP_catch)
            continue;
        target = pos + 1 + (int32_t)get_u32(buf + pos + 1);
        if (target <= pos || target >= (uint32_t)b->byte_code_len ||
            buf[target] != OP_py || buf[target + 1] != QP_OP_end_async_for)
            continue;
        t = qp_co_pc_index(L, target);
        for (k = i + 1; k < t; k++) {
            if (buf[L->pc[k]] == OP_py && buf[L->pc[k] + 1] == QP_OP_end_send)
                break;
        }
        if (k >= t || t + 1 >= L->n)
            continue;
        v[0] = 2 * L->lasti[k];
        v[1] = 2 * L->unit[k + 1];
        v[2] = 2 * L->unit[t + 1];
        /* insert by src */
        n = db->size / sizeof(v);
        for (j = 0; j < n; j++) {
            memcpy(w, db->buf + j * sizeof(v), sizeof(w));
            if (w[0] > v[0])
                break;
        }
        if (dbuf_put(db, (const uint8_t *)v, sizeof(v)))
            return -1;
        memmove(db->buf + (j + 1) * sizeof(v), db->buf + j * sizeof(v), (n - j) * sizeof(v));
        memcpy(db->buf + j * sizeof(v), v, sizeof(v));
    }
    return 0;
}

/* co_branches(): iterator of (src, not_taken, target) offsets of the
   conditional branches (_PyInstrumentation_BranchesIterator): the
   POP_JUMP_IF_* and FOR_ITER instructions of co_code, not_taken = the
   instruction after the branch (the JUMP_BACKWARD of an inverted backward
   jump, the target past it), FOR_ITER's target past END_FOR / POP_ITER,
   and the async for loops (qp_co_afor_branches). */
static JSValue qp_co_branches(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(c->bytecode);
    QPCoLayout *L = qp_co_layout(ctx, b);
    QPCoEmit e;
    DynBuf db;
    JSValue list = JS_UNDEFINED, it = JS_EXCEPTION;
    size_t k;

    if (!L)
        return JS_EXCEPTION;
    js_dbuf_init(ctx, &db);
    memset(&e, 0, sizeof(e));
    e.L = L;
    e.b = b;
    e.readonly = TRUE;
    e.branches = &db;
    qp_co_emit_all(&e);
    if (dbuf_error(&db)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        goto done;
    if (qp_co_afor_branches(b, L, &db) < 0) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (k = 0; k + 3 * sizeof(int32_t) <= db.size; k += 3 * sizeof(int32_t)) {
        int32_t v[3];
        memcpy(v, db.buf + k, sizeof(v));
        if (qp_co_append_ints(ctx, list, 3, v) < 0)
            goto done;
    }
    it = qp_iter(ctx, list);
 done:
    dbuf_free(&db);
    JS_FreeValue(ctx, list);
    return it;
}

typedef struct QPCoPositionsState {
    JSContext *ctx;
    JSValue list;
} QPCoPositionsState;

static int qp_co_positions_cb(void *opaque, uint32_t start, uint32_t end, const QPLoc *loc)
{
    QPCoPositionsState *s = opaque;
    JSValue items[4], tup;
    uint32_t u;
    int r = 0;

    items[0] = qp_co_int_or_none(s->ctx, loc->lineno);
    items[1] = qp_co_int_or_none(s->ctx, loc->end_lineno);
    items[2] = qp_co_int_or_none(s->ctx, loc->col);
    items[3] = qp_co_int_or_none(s->ctx, loc->end_col);
    tup = qp_tuple_new_steal(s->ctx, 4, items);
    if (JS_IsException(tup))
        return -1;
    /* one (shared, immutable) tuple per code unit of the entry */
    for (u = start; u < end && r == 0; u++)
        r = qp_list_append(s->ctx, s->list, tup);
    JS_FreeValue(s->ctx, tup);
    return r;
}

/* co_positions(): one (lineno, end_lineno, col, end_col) per code unit of
   co_code (caches and EXTENDED_ARG included), from co_linetable */
static JSValue qp_co_positions(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPCoPositionsState s;
    JSValue table, it;
    Py_ssize_t len;
    const uint8_t *p;

    table = qp_co_get(ctx, self, QP_CA_LINETABLE);
    if (JS_IsException(table))
        return JS_EXCEPTION;
    s.ctx = ctx;
    s.list = qp_list_new(ctx, 0);
    if (JS_IsException(s.list)) {
        JS_FreeValue(ctx, table);
        return JS_EXCEPTION;
    }
    p = (const uint8_t *)qp_bytes_data(table, &len);
    if (qp_lt_decode(p, len, qp_co_get_firstlineno(self), qp_co_positions_cb, &s) < 0) {
        JS_FreeValue(ctx, table);
        JS_FreeValue(ctx, s.list);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, table);
    it = qp_iter(ctx, s.list);
    JS_FreeValue(ctx, s.list);
    return it;
}

/* ---- code() -------------------------------------------------------------- */

/* the first Python bytecode in the constant pool of b, NULL if none */
static JSFunctionBytecode *qp_co_first_nested(JSFunctionBytecode *b)
{
    int i;
    for (i = 0; i < b->cpool_count; i++) {
        if (JS_VALUE_GET_TAG(b->cpool[i]) == JS_TAG_FUNCTION_BYTECODE &&
            ((JSFunctionBytecode *)JS_VALUE_GET_PTR(b->cpool[i]))->is_py)
            return JS_VALUE_GET_PTR(b->cpool[i]);
    }
    return NULL;
}

/* name tuples (names, varnames, freevars, cellvars): str items only */
static int qp_co_check_names(JSContext *ctx, JSValueConst t)
{
    Py_ssize_t i;
    for (i = 0; i < QP_TUPLE_SIZE(t); i++) {
        JSValueConst v = QP_TUPLE_ITEMS(t)[i];
        if (!qp_str_check(ctx, v)) {
            qp_raise_type_error(ctx, "name tuples must contain only strings, not '%T'", v);
            return -1;
        }
    }
    return 0;
}

/* Name i of a name tuple for the synthesized source: the name itself
   (real) or `prefix`i. */
static int qp_co_synth_name(JSContext *ctx, DynBuf *db, JSValueConst names, int i, BOOL real,
                            char prefix)
{
    const char *s;
    size_t len;
    int r;

    if (!real)
        return dbuf_printf(db, "%c%d", prefix, i);
    s = qp_str_utf8(ctx, QP_TUPLE_ITEMS(names)[i], &len);
    if (!s)
        return -1;
    r = dbuf_put(db, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return r;
}

/* The bytecode of an explicit code object: a function with the
   parameters of varnames in CPython's localsplus order (positional,
   keyword-only, *args, **kwargs) and the free variables of an enclosing
   function, whose body raises SystemError.  real: the given names (so that
   keyword arguments bind), else generated ones (a0.., f0..).  Returns the
   function bytecode, owned by *pbc (the compiled module); NULL with *pbc
   JS_EXCEPTION on error, NULL with no exception pending when the real
   names cannot be used (not identifiers, keywords, duplicates, shadowing
   SystemError, ...). */
static JSFunctionBytecode *qp_co_synth(JSContext *ctx, const int *iv, JSValueConst varnames,
                                       JSValueConst freevars, BOOL real, JSValue *pbc)
{
    int argcount = iv[QP_CF_ARGCOUNT], flags = iv[QP_CF_FLAGS];
    int nargs = argcount + iv[QP_CF_KWONLYARGCOUNT];
    int total = nargs + ((flags & CO_VARARGS) != 0) + ((flags & CO_VARKEYWORDS) != 0);
    int nfree = (int)QP_TUPLE_SIZE(freevars), i, r = 0;
    JSFunctionBytecode *mb, *fb;
    const char *indent = nfree ? "    " : "";
    DynBuf db;
    JSValue bc;

    *pbc = JS_UNDEFINED;
    if (real) {
        /* the body refers to the builtin SystemError */
        for (i = 0; i < total + nfree; i++) {
            JSValueConst n = i < total ? QP_TUPLE_ITEMS(varnames)[i]
                                       : QP_TUPLE_ITEMS(freevars)[i - total];
            const char *s = qp_str_utf8(ctx, n, NULL);
            BOOL shadows;
            if (!s) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return NULL;
            }
            shadows = !strcmp(s, "SystemError");
            JS_FreeCString(ctx, s);
            if (shadows)
                return NULL;
        }
    }
    js_dbuf_init(ctx, &db);
    if (nfree) {
        dbuf_putstr(&db, "def __qp_outer():\n    ");
        for (i = 0; i < nfree; i++) {
            r |= qp_co_synth_name(ctx, &db, freevars, i, real, 'f');
            r |= dbuf_putstr(&db, " = ");
        }
        dbuf_putstr(&db, "None\n");
    }
    dbuf_printf(&db, "%s%sdef __qp_code(", indent,
                (flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)) ? "async " : "");
    for (i = 0; i < argcount; i++) {
        r |= qp_co_synth_name(ctx, &db, varnames, i, real, 'a');
        dbuf_putstr(&db, i + 1 == iv[QP_CF_POSONLYARGCOUNT] ? ", /, " : ", ");
    }
    if (flags & CO_VARARGS) {
        /* localsplus order: *args follows the keyword-only names */
        dbuf_putc(&db, '*');
        r |= qp_co_synth_name(ctx, &db, varnames, nargs, real, 'a');
        dbuf_putstr(&db, ", ");
    } else if (nargs > argcount) {
        dbuf_putstr(&db, "*, ");
    }
    for (i = argcount; i < nargs; i++) {
        r |= qp_co_synth_name(ctx, &db, varnames, i, real, 'a');
        dbuf_putstr(&db, ", ");
    }
    if (flags & CO_VARKEYWORDS) {
        dbuf_putstr(&db, "**");
        r |= qp_co_synth_name(ctx, &db, varnames, total - 1, real, 'a');
        dbuf_putstr(&db, ", ");
    }
    dbuf_printf(&db, "):\n%s    raise SystemError('code object created by code() cannot be "
                "executed: CPython bytecode is not supported')\n", indent);
    if (flags & (CO_GENERATOR | CO_ASYNC_GENERATOR))
        dbuf_printf(&db, "%s    yield\n", indent);
    for (i = 0; i < nfree; i++) {
        dbuf_printf(&db, "%s    ", indent);
        r |= qp_co_synth_name(ctx, &db, freevars, i, real, 'f');
        dbuf_putc(&db, '\n');
    }
    if (r < 0 && real) {
        /* a name without UTF-8 form */
        dbuf_free(&db);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    if (r || dbuf_putc(&db, '\0')) {
        dbuf_free(&db);
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        *pbc = JS_EXCEPTION;
        return NULL;
    }
    bc = qp_compile_source(ctx, (const char *)db.buf, db.size - 1, "<code>", JS_UNDEFINED,
                           257 /* Py_file_input */, 0, 0, NULL);
    dbuf_free(&db);
    if (JS_IsException(bc)) {
        if (!real) {
            *pbc = JS_EXCEPTION;
            return NULL;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    mb = qp_co_first_nested(JS_VALUE_GET_PTR(bc));
    fb = mb && nfree ? qp_co_first_nested(mb) : mb;
    if (!fb || fb->closure_var_count != nfree) {
        JS_FreeValue(ctx, bc);
        return NULL;
    }
    *pbc = bc;
    return fb;
}

/* code(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize, flags,
        codestring, constants, names, varnames, filename, name, qualname,
        firstlineno, linetable, exceptiontable, freevars=(), cellvars=())
   from its QP_CF__COUNT arguments (QP_NULL: freevars / cellvars omitted).
   Validation as code_new_impl + _PyCode_Validate. */
static JSValue qp_co_from_fields(JSContext *ctx, JSValueConst *a)
{
    static const char *const neg_names[4] = {
        "argcount", "posonlyargcount", "kwonlyargcount", "nlocals"
    };
    int iv[QP_CF_FIRSTLINENO + 1], i, total;
    Py_ssize_t codelen;
    JSValueConst varnames, freevars;
    JSValue empty = JS_UNDEFINED, fields = JS_UNDEFINED, bc = JS_UNDEFINED, res = JS_EXCEPTION;
    JSFunctionBytecode *fb;
    QPCodeObject nc;

    for (i = 0; i < QP_CF__COUNT; i++) {
        JSValueConst v = a[i];
        const char *want = NULL;
        BOOL ok = TRUE;

        switch (i) {
        case QP_CF_ARGCOUNT: case QP_CF_POSONLYARGCOUNT: case QP_CF_KWONLYARGCOUNT:
        case QP_CF_NLOCALS: case QP_CF_STACKSIZE: case QP_CF_FLAGS: case QP_CF_FIRSTLINENO:
            if (qp_arg_int(ctx, v, &iv[i]) < 0)
                return JS_EXCEPTION;
            break;
        case QP_CF_CODE: case QP_CF_LINETABLE: case QP_CF_EXCEPTIONTABLE:
            want = "bytes";
            ok = qp_bytes_check(ctx, v);
            break;
        case QP_CF_FILENAME: case QP_CF_NAME: case QP_CF_QUALNAME:
            want = "str";
            ok = qp_str_check(ctx, v);
            break;
        default:
            if (QP_IS_NULL(v))
                break;
            want = "tuple";
            ok = qp_tuple_check(ctx, v);
            break;
        }
        if (!ok)
            return qp_raise_type_error(ctx, "code() argument %d must be %s, not %T", i + 1,
                                       want, v);
    }
    for (i = 0; i < 4; i++) {
        if (iv[i] < 0)
            return qp_raise_value_error(ctx, "code: %s must not be negative", neg_names[i]);
    }
    empty = qp_tuple_new(ctx, 0);
    if (JS_IsException(empty))
        return JS_EXCEPTION;
    varnames = a[QP_CF_VARNAMES];
    freevars = QP_IS_NULL(a[QP_CF_FREEVARS]) ? empty : a[QP_CF_FREEVARS];
    if (qp_co_check_names(ctx, a[QP_CF_NAMES]) < 0 || qp_co_check_names(ctx, varnames) < 0 ||
        qp_co_check_names(ctx, freevars) < 0 ||
        (!QP_IS_NULL(a[QP_CF_CELLVARS]) && qp_co_check_names(ctx, a[QP_CF_CELLVARS]) < 0))
        goto done;
    if (iv[QP_CF_ARGCOUNT] < iv[QP_CF_POSONLYARGCOUNT] || iv[QP_CF_STACKSIZE] < 0 ||
        iv[QP_CF_FLAGS] < 0) {
        qp_raise_system_error(ctx, "bad argument to internal function");
        goto done;
    }
    qp_bytes_data(a[QP_CF_CODE], &codelen);
    if (codelen % 2) {
        qp_raise_value_error(ctx, "code: co_code is malformed");
        goto done;
    }
    total = iv[QP_CF_ARGCOUNT] + iv[QP_CF_KWONLYARGCOUNT] + ((iv[QP_CF_FLAGS] & CO_VARARGS) != 0) +
        ((iv[QP_CF_FLAGS] & CO_VARKEYWORDS) != 0);
    if (QP_TUPLE_SIZE(varnames) < total) {
        qp_raise_value_error(ctx, "code: co_varnames is too small");
        goto done;
    }

    /* the stored fields: co_nlocals is len(co_varnames) (CPython derives
       it from the local kinds), omitted free/cell variables are () */
    fields = qp_tuple_new(ctx, QP_CF__COUNT);
    if (JS_IsException(fields))
        goto done;
    for (i = 0; i < QP_CF__COUNT; i++) {
        JSValue v;
        if (i == QP_CF_NLOCALS)
            v = JS_NewInt32(ctx, (int32_t)QP_TUPLE_SIZE(varnames));
        else if (i <= QP_CF_FLAGS || i == QP_CF_FIRSTLINENO)
            v = JS_NewInt32(ctx, iv[i]);
        else if (QP_IS_NULL(a[i]))
            v = JS_DupValue(ctx, empty);
        else
            v = JS_DupValue(ctx, a[i]);
        QP_TUPLE_ITEMS(fields)[i] = v;
    }

    fb = qp_co_synth(ctx, iv, varnames, freevars, TRUE, &bc);
    if (!fb && !JS_IsException(bc))
        fb = qp_co_synth(ctx, iv, varnames, freevars, FALSE, &bc);
    if (!fb) {
        if (!JS_IsException(bc))
            qp_raise_system_error(ctx, "code(): cannot build the code object");
        goto done;
    }
    memset(&nc, 0, sizeof(nc));
    nc.linetable = QP_NULL;
    nc.exctable = QP_NULL;
    nc.name = QP_TUPLE_ITEMS(fields)[QP_CF_NAME];
    nc.qualname = QP_TUPLE_ITEMS(fields)[QP_CF_QUALNAME];
    nc.filename = QP_TUPLE_ITEMS(fields)[QP_CF_FILENAME];
    nc.firstlineno = iv[QP_CF_FIRSTLINENO];
    nc.has_firstlineno = TRUE;
    nc.fields = fields;
    res = qp_co_alloc(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, fb), &nc);
 done:
    JS_FreeValue(ctx, bc);
    JS_FreeValue(ctx, fields);
    JS_FreeValue(ctx, empty);
    return res;
}

/* code.__new__ (positional-only: Argument Clinic signature of code_new) */
static JSValue qp_co_new(JSContext *ctx, QPType *subtype, JSValueConst *args, size_t nargsf,
                         JSValueConst kwnames)
{
    static const char *const kwlist[QP_CF__COUNT + 1] = {
        "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", NULL
    };
    static const QPArgSpec spec = { "code", kwlist, QP_CF__COUNT - 2, QP_CF__COUNT, 0 };
    JSValueConst a[QP_CF__COUNT];

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    return qp_co_from_fields(ctx, a);
}

/* Compare a replace() argument with the current value of an unsupported
   field: 0 equal, -1 error (ValueError when different). */
static int qp_co_replace_same(JSContext *ctx, JSValueConst self, int magic, JSValueConst v,
                              const char *field)
{
    JSValue cur = qp_co_get(ctx, self, magic);
    int eq;

    if (JS_IsException(cur))
        return -1;
    eq = qp_richcompare_bool(ctx, cur, v, QP_CMP_EQ);
    JS_FreeValue(ctx, cur);
    if (eq < 0)
        return -1;
    if (!eq) {
        if (magic == QP_CA_CODE)
            qp_raise_value_error(ctx, "code.replace(): co_code can only be the code object's "
                                 "own (synthesized) co_code: this runtime cannot execute "
                                 "CPython bytecode");
        else
            qp_raise_value_error(ctx, "code.replace(): changing %s is not supported "
                                 "(the code would have to be recompiled)", field);
        return -1;
    }
    return 0;
}

/* replace() argument type check (fields 7..17 of the replace() keyword
   list): 0, -1 TypeError */
static int qp_co_replace_check(JSContext *ctx, const char *const *kwlist, int i, JSValueConst v)
{
    const char *want;
    BOOL ok;

    if (i == 7 || i >= 16) {
        want = "bytes";
        ok = qp_bytes_check(ctx, v);
    } else if (i <= 12) {
        want = "tuple";
        ok = qp_tuple_check(ctx, v);
    } else {
        want = "str";
        ok = qp_str_check(ctx, v);
    }
    if (ok)
        return 0;
    qp_raise_type_error(ctx, "replace() argument '%s' must be %s, not %T", kwlist[i], want, v);
    return -1;
}

/* replace() of an explicit code object: code() with the changed fields */
static JSValue qp_co_replace_explicit(JSContext *ctx, QPCodeObject *c, const char *const *kwlist,
                                      JSValueConst *a)
{
    /* code() field of each replace() keyword */
    static const uint8_t field_of[18] = {
        QP_CF_ARGCOUNT, QP_CF_POSONLYARGCOUNT, QP_CF_KWONLYARGCOUNT, QP_CF_NLOCALS,
        QP_CF_STACKSIZE, QP_CF_FLAGS, QP_CF_FIRSTLINENO, QP_CF_CODE, QP_CF_CONSTS,
        QP_CF_NAMES, QP_CF_VARNAMES, QP_CF_FREEVARS, QP_CF_CELLVARS, QP_CF_FILENAME,
        QP_CF_NAME, QP_CF_QUALNAME, QP_CF_LINETABLE, QP_CF_EXCEPTIONTABLE
    };
    JSValueConst f[QP_CF__COUNT];
    int i;

    for (i = 0; i < QP_CF__COUNT; i++)
        f[i] = QP_TUPLE_ITEMS(c->fields)[i];
    for (i = 0; i < 18; i++) {
        if (QP_IS_NULL(a[i]))
            continue;
        if (i >= 7 && qp_co_replace_check(ctx, kwlist, i, a[i]) < 0)
            return JS_EXCEPTION;
        f[field_of[i]] = a[i];
    }
    return qp_co_from_fields(ctx, f);
}

/* code.replace(*, co_argcount=..., ...) */
static JSValue qp_co_replace(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = {
        "co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals",
        "co_stacksize", "co_flags", "co_firstlineno", "co_code", "co_consts",
        "co_names", "co_varnames", "co_freevars", "co_cellvars", "co_filename",
        "co_name", "co_qualname", "co_linetable", "co_exceptiontable", NULL
    };
    static const int int_magic[7] = {
        QP_CA_ARGCOUNT, QP_CA_POSONLYARGCOUNT, QP_CA_KWONLYARGCOUNT, QP_CA_NLOCALS,
        QP_CA_STACKSIZE, QP_CA_FLAGS, QP_CA_FIRSTLINENO
    };
    static const int obj_magic[11] = {
        QP_CA_CODE, QP_CA_CONSTS, QP_CA_NAMES, QP_CA_VARNAMES, QP_CA_FREEVARS,
        QP_CA_CELLVARS, QP_CA_FILENAME, QP_CA_NAME, QP_CA_QUALNAME, QP_CA_LINETABLE,
        QP_CA_EXCEPTIONTABLE
    };
    static const QPArgSpec spec = { "replace", kwlist, 0, 0, 0 };
    QPCodeObject *c = qp_co_data(self), nc;
    JSValueConst a[18];
    JSValue res;
    int i, iv;
    uint32_t base_flags = ((JSFunctionBytecode *)JS_VALUE_GET_PTR(c->bytecode))->py_code->co_flags &
        ~QP_CO_NATIVE;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(c->fields))
        return qp_co_replace_explicit(ctx, c, kwlist, a);
    nc = *c;
    for (i = 0; i < 7; i++) {
        if (QP_IS_NULL(a[i]))
            continue;
        if (qp_arg_int(ctx, a[i], &iv) < 0)
            return JS_EXCEPTION;
        if (iv < 0)
            return qp_raise_value_error(ctx, "%s must be a positive integer", kwlist[i]);
        if (i == 6) {
            nc.firstlineno = iv;
            nc.has_firstlineno = TRUE;
        } else if (i == 5 && (base_flags & CO_GENERATOR) &&
                   ((uint32_t)iv & ~(uint32_t)CO_ITERABLE_COROUTINE) == base_flags) {
            /* types.coroutine: a generator becomes awaitable; the flag only
               changes how its generators are awaited, not the bytecode */
            nc.flags_add = (uint32_t)iv & CO_ITERABLE_COROUTINE;
        } else {
            JSValue cur = qp_co_get(ctx, self, int_magic[i]);
            int same;
            if (JS_IsException(cur))
                return JS_EXCEPTION;
            same = JS_VALUE_GET_TAG(cur) == JS_TAG_INT && JS_VALUE_GET_INT(cur) == iv;
            if (!same)
                return qp_raise_value_error(ctx, "code.replace(): changing %s is not supported "
                                            "(the code would have to be recompiled)", kwlist[i]);
        }
    }
    for (i = 7; i < 18; i++) {
        JSValueConst v = a[i];

        if (QP_IS_NULL(v))
            continue;
        if (qp_co_replace_check(ctx, kwlist, i, v) < 0)
            return JS_EXCEPTION;
        switch (i) {
        case 13:
            nc.filename = (JSValue)v;   /* borrowed: qp_co_alloc dups */
            break;
        case 14:
            nc.name = (JSValue)v;
            break;
        case 15:
            nc.qualname = (JSValue)v;
            break;
        case 16:
            nc.linetable = (JSValue)v;  /* metadata only: tracebacks keep the real lines */
            break;
        case 17:
            nc.exctable = (JSValue)v;
            break;
        default:
            if (qp_co_replace_same(ctx, self, obj_magic[i - 7], v, kwlist[i]) < 0)
                return JS_EXCEPTION;
            break;
        }
    }
    res = qp_co_alloc(ctx, c->bytecode, &nc);
    return res;
}

static JSValue qp_co_varname_from_oparg(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kwlist[] = { "oparg", NULL };
    static const QPArgSpec spec = { "_varname_from_oparg", kwlist, 1, 1, 0 };
    QPCodeObject *c = qp_co_data(self);
    JSValueConst a[1];
    JSValue names, res;
    int oparg;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (qp_arg_int(ctx, a[0], &oparg) < 0)
        return JS_EXCEPTION;
    names = qp_co_localsplus_names(ctx, JS_VALUE_GET_PTR(c->bytecode));
    if (JS_IsException(names))
        return JS_EXCEPTION;
    if (oparg < 0 || oparg >= QP_TUPLE_SIZE(names)) {
        JS_FreeValue(ctx, names);
        return qp_raise_index_error(ctx, "tuple index out of range");
    }
    res = JS_DupValue(ctx, QP_TUPLE_ITEMS(names)[oparg]);
    JS_FreeValue(ctx, names);
    return res;
}

static JSValue qp_co_sizeof(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(c->bytecode);
    int64_t size = sizeof(JSFunctionBytecode) + b->byte_code_len +
        (int64_t)(b->arg_count + b->var_count) * sizeof(JSBytecodeVarDef) +
        (int64_t)b->closure_var_count * sizeof(JSClosureVar) +
        (int64_t)b->cpool_count * sizeof(JSValue);

    if (b->py_code) {
        size += sizeof(QPCodeInfo) + (int64_t)b->py_code->loc_size * sizeof(QPLoc) +
            (int64_t)b->py_code->pcloc_count * 8 +
            (int64_t)b->py_code->ic_count * sizeof(QPInlineCache);
    }
    return qp_long_from_int64(ctx, size);
}

/* ---- slots --------------------------------------------------------------- */

static JSValue qp_co_repr(JSContext *ctx, JSValueConst self)
{
    JSValue name, filename, res;
    int lineno;

    if (!qp_co_check(self))
        return qp_raise_type_error(ctx, "expected a code object");
    lineno = qp_co_get_firstlineno(self);
    if (lineno == 0)
        lineno = -1;
    name = qp_co_get_name(ctx, self);
    if (JS_IsException(name))
        return JS_EXCEPTION;
    filename = qp_co_get_filename(ctx, self);
    if (JS_IsException(filename)) {
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    if (qp_str_check(ctx, filename))
        res = qp_str_from_format(ctx, "<code object %U at %p, file \"%U\", line %d>",
                                 name, JS_VALUE_GET_PTR(self), filename, lineno);
    else
        res = qp_str_from_format(ctx, "<code object %U at %p, file ???, line %d>",
                                 name, JS_VALUE_GET_PTR(self), lineno);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, filename);
    return res;
}

/* Compare one field of two code objects with ==: 1 equal, 0 not, -1 error. */
static int qp_co_field_eq(JSContext *ctx, JSValueConst a, JSValueConst b, int magic)
{
    JSValue va, vb;
    int eq;

    va = qp_co_get(ctx, a, magic);
    if (JS_IsException(va))
        return -1;
    vb = qp_co_get(ctx, b, magic);
    if (JS_IsException(vb)) {
        JS_FreeValue(ctx, va);
        return -1;
    }
    eq = qp_richcompare_bool(ctx, va, vb, QP_CMP_EQ);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    return eq;
}

/* code_richcompare: structural equality */
static JSValue qp_co_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    static const int fields[] = {
        QP_CA_NAME, QP_CA_ARGCOUNT, QP_CA_POSONLYARGCOUNT, QP_CA_KWONLYARGCOUNT,
        QP_CA_FLAGS, QP_CA_FIRSTLINENO, QP_CA_CODE, QP_CA_CONSTS, QP_CA_NAMES,
        QP_CA_LINETABLE, QP_CA_EXCEPTIONTABLE
    };
    QPCodeObject *ca = qp_co_data(a), *cb = qp_co_data(b);
    JSFunctionBytecode *ba, *bb;
    int eq = 1;
    size_t i;

    if ((op != QP_CMP_EQ && op != QP_CMP_NE) || !ca || !cb)
        return qp_not_implemented(ctx);
    ba = JS_VALUE_GET_PTR(ca->bytecode);
    bb = JS_VALUE_GET_PTR(cb->bytecode);
    if (ba->byte_code_len != bb->byte_code_len)
        eq = 0;
    for (i = 0; eq == 1 && i < countof(fields); i++)
        eq = qp_co_field_eq(ctx, a, b, fields[i]);
    if (eq == 1) {
        JSValue na = qp_co_localsplus_names(ctx, ba), nb;
        if (JS_IsException(na))
            return JS_EXCEPTION;
        nb = qp_co_localsplus_names(ctx, bb);
        if (JS_IsException(nb)) {
            JS_FreeValue(ctx, na);
            return JS_EXCEPTION;
        }
        eq = qp_richcompare_bool(ctx, na, nb, QP_CMP_EQ);
        JS_FreeValue(ctx, na);
        JS_FreeValue(ctx, nb);
    }
    if (eq < 0)
        return JS_EXCEPTION;
    return qp_bool_new(op == QP_CMP_EQ ? eq : !eq);
}

#define QP_CO_HASH_MULTIPLIER 1000003UL

static Py_hash_t qp_co_hash(JSContext *ctx, JSValueConst self)
{
    static const int fields[] = {
        QP_CA_NAME, QP_CA_CONSTS, QP_CA_NAMES, -1, QP_CA_LINETABLE, QP_CA_EXCEPTIONTABLE
    };
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(c->bytecode);
    QPCodeInfo *ci = b->py_code;
    qp_uhash_t uhash = 20221211;
    size_t i;
    int k;

#define QP_SCRAMBLE_IN(H) do {                   \
        uhash ^= (qp_uhash_t)(H);                \
        uhash *= QP_CO_HASH_MULTIPLIER;          \
    } while (0)

    for (i = 0; i < countof(fields); i++) {
        JSValue v = fields[i] < 0 ? qp_co_localsplus_names(ctx, b)
            : qp_co_get(ctx, self, fields[i]);
        Py_hash_t h;
        if (JS_IsException(v))
            return -1;
        h = qp_hash(ctx, v);
        JS_FreeValue(ctx, v);
        if (h == -1)
            return -1;
        QP_SCRAMBLE_IN(h);
    }
    QP_SCRAMBLE_IN(ci->argcount);
    QP_SCRAMBLE_IN(ci->posonlyargcount);
    QP_SCRAMBLE_IN(ci->kwonlyargcount);
    QP_SCRAMBLE_IN(ci->co_flags);
    QP_SCRAMBLE_IN(qp_co_get_firstlineno(self));
    QP_SCRAMBLE_IN(b->byte_code_len);
    for (k = 0; k < b->byte_code_len; k++)
        QP_SCRAMBLE_IN(b->byte_code_buf[k]);
#undef QP_SCRAMBLE_IN
    if ((Py_hash_t)uhash == -1)
        return -2;
    return (Py_hash_t)uhash;
}

static const QPGetSetDef qp_co_getsets[] = {
    /* code_memberlist */
    { "co_argcount", qp_co_get, NULL, QP_CA_ARGCOUNT, NULL, QP_GETSET_MEMBER },
    { "co_posonlyargcount", qp_co_get, NULL, QP_CA_POSONLYARGCOUNT, NULL, QP_GETSET_MEMBER },
    { "co_kwonlyargcount", qp_co_get, NULL, QP_CA_KWONLYARGCOUNT, NULL, QP_GETSET_MEMBER },
    { "co_stacksize", qp_co_get, NULL, QP_CA_STACKSIZE, NULL, QP_GETSET_MEMBER },
    { "co_flags", qp_co_get, NULL, QP_CA_FLAGS, NULL, QP_GETSET_MEMBER },
    { "co_nlocals", qp_co_get, NULL, QP_CA_NLOCALS, NULL, QP_GETSET_MEMBER },
    { "co_consts", qp_co_get, NULL, QP_CA_CONSTS, NULL, QP_GETSET_MEMBER },
    { "co_names", qp_co_get, NULL, QP_CA_NAMES, NULL, QP_GETSET_MEMBER },
    { "co_filename", qp_co_get, NULL, QP_CA_FILENAME, NULL, QP_GETSET_MEMBER },
    { "co_name", qp_co_get, NULL, QP_CA_NAME, NULL, QP_GETSET_MEMBER },
    { "co_qualname", qp_co_get, NULL, QP_CA_QUALNAME, NULL, QP_GETSET_MEMBER },
    { "co_firstlineno", qp_co_get, NULL, QP_CA_FIRSTLINENO, NULL, QP_GETSET_MEMBER },
    { "co_linetable", qp_co_get, NULL, QP_CA_LINETABLE, NULL, QP_GETSET_MEMBER },
    { "co_exceptiontable", qp_co_get, NULL, QP_CA_EXCEPTIONTABLE, NULL, QP_GETSET_MEMBER },
    { "_co_code_adaptive", qp_co_get, NULL, QP_CA_CODE_ADAPTIVE, NULL },
    { "co_varnames", qp_co_get, NULL, QP_CA_VARNAMES, NULL },
    { "co_cellvars", qp_co_get, NULL, QP_CA_CELLVARS, NULL },
    { "co_freevars", qp_co_get, NULL, QP_CA_FREEVARS, NULL },
    { "co_code", qp_co_get, NULL, QP_CA_CODE, NULL },
    { NULL }
};

static const QPMethodDef qp_co_methods[] = {
    { "__sizeof__", qp_co_sizeof, QP_METH_NOARGS, 0, NULL },
    { "co_lines", qp_co_lines, QP_METH_NOARGS, 0, NULL },
    { "co_positions", qp_co_positions, QP_METH_NOARGS, 0, NULL },
    { "co_branches", qp_co_branches, QP_METH_NOARGS, 0, NULL },
    { "replace", qp_co_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "replace($self, /, **changes)\n--\n\n"
      "Return a copy of the code object with new values for the specified fields." },
    { "_varname_from_oparg", qp_co_varname_from_oparg, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "_varname_from_oparg($self, /, oparg)\n--\n\n"
      "(internal-only) Return the local variable name for the given oparg.\n\n"
      "WARNING: this method is for internal use only and may change or go\naway." },
    { "__replace__", qp_co_replace, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "__replace__($self, /, **changes)\n--\n\nThe same as replace()." },
    { NULL }
};

static const QPTypeSpec qp_co_code_spec = {
    .name = "code",
    .doc = "code(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize,\n"
           "     flags, codestring, constants, names, varnames, filename, name,\n"
           "     qualname, firstlineno, linetable, exceptiontable, freevars=(),\n"
           "     cellvars=(), /)\n--\n\nCreate a code object.  Not for the faint of heart.",
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_CODE,
    .slots = {
        .tp_new = qp_co_new,
        .tp_repr = qp_co_repr,
        .tp_hash = qp_co_hash,
        .tp_richcompare = qp_co_richcompare,
    },
    .methods = qp_co_methods,
    .getsets = qp_co_getsets,
};

/* ------------------------------------------------------------------------ */
/* Serialisation of the Python code metadata (JSFunctionBytecode.py_code)   */
/* ------------------------------------------------------------------------ */

/* Shared by the bytecode writer (JS_WriteObject with
   JS_WRITE_OBJ_BYTECODE: marshal of code objects, .pyc files) and the
   heap snapshots (qp_snap_write_code): a presence byte (bit 0: is_py,
   bit 1: py_code present), then the QPCodeInfo fields; the inline caches
   are restored empty (only their count is written). */
static int qp_bc_write_code(BCWriterState *s, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    uint32_t i;

    bc_put_u8(s, b->is_py | ((ci != NULL) << 1));
    if (!ci)
        return 0;
    bc_put_leb128(s, ci->co_flags);
    bc_put_leb128(s, ci->argcount);
    bc_put_leb128(s, ci->posonlyargcount);
    bc_put_leb128(s, ci->kwonlyargcount);
    bc_put_sleb128(s, ci->firstlineno);
    if (bc_put_atom(s, ci->qualname))
        return -1;
    bc_put_sleb128(s, ci->doc_cpool_idx);
    bc_put_leb128(s, ci->nconsts);
    for (i = 0; i < ci->nconsts; i++)
        bc_put_leb128(s, ci->consts[i]);
    bc_put_leb128(s, ci->loc_count);
    for (i = 0; i < ci->loc_count; i++) {
        bc_put_sleb128(s, ci->locs[i].lineno);
        bc_put_sleb128(s, ci->locs[i].end_lineno);
        bc_put_sleb128(s, ci->locs[i].col);
        bc_put_sleb128(s, ci->locs[i].end_col);
    }
    bc_put_leb128(s, ci->pcloc_count);
    for (i = 0; i < ci->pcloc_count; i++) {
        bc_put_leb128(s, ci->pcloc[i].pc);
        bc_put_leb128(s, ci->pcloc[i].loc);
    }
    bc_put_leb128(s, ci->ic_count);
    return 0;
}

/* invalid input: the snapshot reader records the message, the bytecode
   reader throws a SyntaxError like its other checks */
static int qp_bc_read_invalid(BCReaderState *s, const char *what)
{
#ifdef QP_SNAPSHOT
    if (s->is_snapshot)
        return snap_error((SnapReader *)s, "invalid Python %s", what);
#endif
    if (!s->error_state)
        JS_ThrowSyntaxError(s->ctx, "invalid Python %s", what);
    return s->error_state = -1;
}

static int qp_bc_get_int(BCReaderState *s, int *pv)
{
    int32_t v;
    if (bc_get_sleb128(s, &v))
        return -1;
    *pv = v;
    return 0;
}

/* qp_bc_write_code's data; b->py_code is attached before it is filled
   (freed with the bytecode on error) */
static int qp_bc_read_code(BCReaderState *s, JSFunctionBytecode *b)
{
    JSContext *ctx = s->ctx;
    QPCodeInfo *ci;
    uint8_t bits;
    uint32_t v, i, n;

    if (bc_get_u8(s, &bits))
        return -1;
    b->is_py = bits & 1;
    if (!(bits & 2))
        return 0;
    ci = qp_code_info_new(ctx);
    if (!ci)
        return -1;
    b->py_code = ci;
    if (bc_get_leb128(s, &ci->co_flags))
        return -1;
    if (bc_get_leb128(s, &v)) return -1;
    ci->argcount = v;
    if (bc_get_leb128(s, &v)) return -1;
    ci->posonlyargcount = v;
    if (bc_get_leb128(s, &v)) return -1;
    ci->kwonlyargcount = v;
    if (qp_bc_get_int(s, &ci->firstlineno) ||
        bc_get_atom(s, &ci->qualname) ||
        qp_bc_get_int(s, &ci->doc_cpool_idx) ||
        bc_get_leb128(s, &n))
        return -1;
    if (n > s->buf_end - s->ptr)
        return qp_bc_read_invalid(s, "constant table");
    if (n) {
        /* indexes checked against cpool_count by qp_co_layout_consts */
        ci->consts = js_malloc(ctx, sizeof(ci->consts[0]) * n);
        if (!ci->consts)
            return -1;
        for (i = 0; i < n; i++) {
            if (bc_get_leb128(s, &ci->consts[i]))
                return -1;
            ci->nconsts = i + 1;
        }
    }
    if (bc_get_leb128(s, &n))
        return -1;
    if (n > s->buf_end - s->ptr)
        return qp_bc_read_invalid(s, "location table");
    if (n) {
        ci->locs = js_malloc(ctx, sizeof(ci->locs[0]) * n);
        if (!ci->locs)
            return -1;
        ci->loc_size = n;
        for (i = 0; i < n; i++) {
            QPLoc *l = &ci->locs[i];
            if (qp_bc_get_int(s, &l->lineno) || qp_bc_get_int(s, &l->end_lineno) ||
                qp_bc_get_int(s, &l->col) || qp_bc_get_int(s, &l->end_col))
                return -1;
            ci->loc_count = i + 1;
        }
    }
    if (bc_get_leb128(s, &n))
        return -1;
    if (n > s->buf_end - s->ptr)
        return qp_bc_read_invalid(s, "location map");
    if (n) {
        ci->pcloc = js_malloc(ctx, sizeof(ci->pcloc[0]) * n);
        if (!ci->pcloc)
            return -1;
        for (i = 0; i < n; i++) {
            if (bc_get_leb128(s, &ci->pcloc[i].pc) ||
                bc_get_leb128(s, &ci->pcloc[i].loc))
                return -1;
            if (QP_PCLOC_IDX(ci->pcloc[i].loc) >= ci->loc_count)
                return qp_bc_read_invalid(s, "location map");
            ci->pcloc_count = i + 1;
        }
    }
    if (bc_get_leb128(s, &v))
        return -1;
    if (v > UINT16_MAX)
        return qp_bc_read_invalid(s, "inline cache count");
    ci->ic_count = v;
    /* inline caches restored empty */
    return qp_code_info_setup_ic(ctx, b);
}

/* Bump when the meaning of the code the compiler emits changes without
   a change of the opcode tables (part of qp_pyc_format_hash). */
#define QP_PYC_FORMAT_VERSION 2

static void qp_pyc_hash_bytes(uint64_t *h, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;
    for (i = 0; i < len; i++) {
        *h ^= p[i];
        *h *= 0x100000001b3ULL;
    }
}

static void qp_pyc_hash_u32(uint64_t *h, uint32_t v)
{
    uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
    qp_pyc_hash_bytes(h, b, 4);
}

/* Identity of the serialised code of this build (FNV-1a): bytecode
   format, opcode and subop tables, predefined atoms and the build of the
   compiler and the interpreter (quickjs.c with python/ is one unit, so
   __DATE__/__TIME__ change with any of them).  Code objects written by
   marshal carry it and the .pyc magic number is derived from it, so that
   importlib never runs bytecode of another build. */
static uint64_t qp_pyc_format_hash(void)
{
    static uint64_t hash;
    uint64_t h;
    int i;

    if (hash)
        return hash;
    h = 0xcbf29ce484222325ULL;
    qp_pyc_hash_bytes(&h, CONFIG_VERSION, strlen(CONFIG_VERSION));
    qp_pyc_hash_bytes(&h, __DATE__ " " __TIME__, strlen(__DATE__ " " __TIME__));
    qp_pyc_hash_u32(&h, QP_PYC_FORMAT_VERSION);
    qp_pyc_hash_u32(&h, BC_VERSION);
    qp_pyc_hash_u32(&h, JS_ATOM_END);
    qp_pyc_hash_bytes(&h, js_atom_init, sizeof(js_atom_init));
    qp_pyc_hash_u32(&h, countof(opcode_info));
    for (i = 0; i < (int)countof(opcode_info); i++) {
        const JSOpCode *oi = &opcode_info[i];
        qp_pyc_hash_u32(&h, oi->size | (oi->n_pop << 8) | (oi->n_push << 16) |
                        ((uint32_t)oi->fmt << 24));
    }
    qp_pyc_hash_u32(&h, QP_OP_COUNT);
    for (i = 0; i < QP_OP_COUNT; i++) {
        const QPSubopInfo *si = &qp_subop_info[i];
        qp_pyc_hash_bytes(&h, si->name, strlen(si->name));
        qp_pyc_hash_u32(&h, si->size | (si->n_pop << 8) | (si->n_push << 16) |
                        ((uint32_t)si->fmt << 24));
    }
    if (!h)
        h = 1;
    hash = h;
    return h;
}

/* importlib's MAGIC_NUMBER (_imp.pyc_magic_number_token): 2 bytes from
   qp_pyc_format_hash(), then "\r\n" like CPython's.  The 16-bit number is
   >= 0x8000, outside the range CPython 3 uses (3000-3999), so a CPython
   .pyc is never taken for one of this interpreter. */
static uint32_t qp_pyc_magic_number_token(void)
{
    uint64_t h = qp_pyc_format_hash();
    uint32_t n = 0x8000 | (uint32_t)((h ^ (h >> 16) ^ (h >> 32) ^ (h >> 48)) & 0x7fff);
    return n | ((uint32_t)'\r' << 16) | ((uint32_t)'\n' << 24);
}
