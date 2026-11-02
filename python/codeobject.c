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
 *   co_consts = cpool (docstring first; nested bytecodes as code objects),
 *   co_names = atoms of the name subops (getattr, load_global, ...).
 * co_code is the QuickJS bytecode, co_linetable an encoding of the
 * pc -> location table, co_exceptiontable is empty (handlers are `catch`
 * opcodes).  replace() can change co_name, co_qualname, co_filename and
 * co_firstlineno (the facade keeps the overrides); changing any other
 * field would require recompiling and raises ValueError.  The code()
 * constructor is not provided: CPython bytecode cannot be run by this VM,
 * so type(code)(...) raises TypeError "cannot create 'code' instances".
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
} QPCodeObject;

static void qp_code_finalizer(JSRuntime *rt, JSValue val)
{
    QPCodeObject *c = JS_VALUE_GET_OBJ(val)->u.opaque;

    if (!c)
        return;
    JS_FreeValueRT(rt, c->bytecode);
    JS_FreeValueRT(rt, c->name);
    JS_FreeValueRT(rt, c->qualname);
    JS_FreeValueRT(rt, c->filename);
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
    } else {
        c->name = QP_NULL;
        c->qualname = QP_NULL;
        c->filename = QP_NULL;
        c->firstlineno = 0;
        c->has_firstlineno = FALSE;
    }
    JS_VALUE_GET_OBJ(obj)->u.opaque = c;
    return obj;
}

static JSValue qp_code_new(JSContext *ctx, JSValueConst bytecode)
{
    if (JS_VALUE_GET_TAG(bytecode) != JS_TAG_FUNCTION_BYTECODE ||
        !((JSFunctionBytecode *)JS_VALUE_GET_PTR(bytecode))->is_py)
        return qp_raise_system_error(ctx, "code object over non-Python bytecode");
    return qp_co_alloc(ctx, bytecode, NULL);
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

/* Size of the instruction at pos. */
static int qp_co_insn_size(const uint8_t *buf, int pos)
{
    int op = buf[pos];

    if (op == OP_py)
        return qp_subop_info[buf[pos + 1]].size;
    return short_opcode_info(op).size;
}

/* co_names: operands of the name subops, first-use order, no duplicates */
static JSValue qp_co_names(JSContext *ctx, JSFunctionBytecode *b)
{
    const uint8_t *buf = b->byte_code_buf;
    int pos = 0, len = b->byte_code_len, n = 0, size = 0;
    JSAtom *atoms = NULL;
    JSValue t;

    while (pos < len) {
        if (buf[pos] == OP_py) {
            int sub = buf[pos + 1];
            int fmt = qp_subop_info[sub].fmt;
            if ((fmt == OP_FMT_atom || fmt == OP_FMT_atom_u8 || fmt == OP_FMT_atom_u16) &&
                sub != QP_OP_load_from_dict_or_deref) {
                JSAtom a = get_u32(buf + pos + 2);
                if (!qp_co_atom_in(atoms, n, a)) {
                    if (n >= size) {
                        int nsize = size ? size * 2 : 8;
                        JSAtom *na = js_realloc(ctx, atoms, sizeof(JSAtom) * nsize);
                        if (!na) {
                            js_free(ctx, atoms);
                            return JS_EXCEPTION;
                        }
                        atoms = na;
                        size = nsize;
                    }
                    atoms[n++] = a;
                }
            }
        }
        pos += qp_co_insn_size(buf, pos);
    }
    t = qp_co_atoms_tuple(ctx, atoms, n);
    js_free(ctx, atoms);
    return t;
}

/* co_consts: docstring first, nested bytecodes as code objects */
static JSValue qp_co_consts(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    int doc = (ci && (ci->co_flags & CO_HAS_DOCSTRING) && ci->doc_cpool_idx >= 0 &&
               ci->doc_cpool_idx < b->cpool_count) ? ci->doc_cpool_idx : -1;
    JSValue t, v;
    int i, k = 0;

    t = qp_tuple_new(ctx, b->cpool_count);
    if (JS_IsException(t))
        return JS_EXCEPTION;
    for (i = -1; i < b->cpool_count; i++) {
        int idx = i < 0 ? doc : i;
        if (idx < 0 || (i >= 0 && idx == doc))
            continue;
        if (JS_VALUE_GET_TAG(b->cpool[idx]) == JS_TAG_FUNCTION_BYTECODE &&
            ((JSFunctionBytecode *)JS_VALUE_GET_PTR(b->cpool[idx]))->is_py) {
            v = qp_code_new(ctx, b->cpool[idx]);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, t);
                return JS_EXCEPTION;
            }
        } else {
            v = JS_DupValue(ctx, b->cpool[idx]);
        }
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(t)[k]);
        QP_TUPLE_ITEMS(t)[k++] = v;
    }
    return t;
}

/* co_linetable: the pc -> location table as little-endian int32 records
   (pc, lineno, end_lineno, col, end_col). */
static JSValue qp_co_linetable(JSContext *ctx, JSFunctionBytecode *b)
{
    QPCodeInfo *ci = b->py_code;
    uint32_t i, n = ci ? ci->pcloc_count : 0;
    uint8_t *buf;
    JSValue res;

    if (n == 0)
        return qp_bytes_new(ctx, "", 0);
    buf = js_malloc(ctx, n * 20);
    if (!buf)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        QPLoc loc;
        int32_t rec[5];
        int k;
        if (qp_code_addr2loc(b, ci->pcloc[i].pc, &loc) < 0)
            loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
        rec[0] = (int32_t)ci->pcloc[i].pc;
        rec[1] = loc.lineno;
        rec[2] = loc.end_lineno;
        rec[3] = loc.col;
        rec[4] = loc.end_col;
        for (k = 0; k < 5; k++)
            put_u32(buf + i * 20 + k * 4, (uint32_t)rec[k]);
    }
    res = qp_bytes_new(ctx, buf, n * 20);
    js_free(ctx, buf);
    return res;
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

static JSValue qp_co_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b;
    QPCodeInfo *ci;
    int n;

    if (!c)
        return qp_raise_type_error(ctx, "descriptor requires a 'code' object but received '%T'", self);
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
        return JS_NewInt32(ctx, (int32_t)ci->co_flags);
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
        return qp_co_linetable(ctx, b);
    case QP_CA_EXCEPTIONTABLE:
        return qp_bytes_new(ctx, "", 0);
    case QP_CA_VARNAMES:
        return qp_co_kind_tuple(ctx, b, QP_CO_VARNAMES);
    case QP_CA_CELLVARS:
        return qp_co_kind_tuple(ctx, b, QP_CO_CELLVARS);
    case QP_CA_FREEVARS:
        return qp_co_kind_tuple(ctx, b, QP_CO_FREEVARS);
    default:
        return qp_bytes_new(ctx, b->byte_code_buf, b->byte_code_len);
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

static int qp_co_pcloc_line(JSFunctionBytecode *b, uint32_t i)
{
    QPLoc loc;

    if (qp_code_addr2loc(b, b->py_code->pcloc[i].pc, &loc) < 0)
        return -1;
    return loc.lineno;
}

/* co_lines(): iterator of (start, end, line) ranges, consecutive ranges with
   the same line merged (lineiter_next). */
static JSValue qp_co_lines(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(c->bytecode);
    QPCodeInfo *ci = b->py_code;
    JSValue list, it;
    uint32_t i, n = ci->pcloc_count;
    int v[3];

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return JS_EXCEPTION;
    if (n == 0 && b->byte_code_len > 0) {
        v[0] = 0;
        v[1] = b->byte_code_len;
        v[2] = -1;
        if (qp_co_append_ints(ctx, list, 3, v) < 0)
            goto fail;
    }
    i = 0;
    while (i < n) {
        v[0] = i == 0 ? 0 : (int)ci->pcloc[i].pc;
        v[2] = qp_co_pcloc_line(b, i);
        i++;
        while (i < n && qp_co_pcloc_line(b, i) == v[2])
            i++;
        v[1] = i < n ? (int)ci->pcloc[i].pc : b->byte_code_len;
        if (v[1] > v[0]) {
            /* start and end are never negative */
            if (qp_co_append_ints(ctx, list, 3, v) < 0)
                goto fail;
        }
    }
    it = qp_iter(ctx, list);
    JS_FreeValue(ctx, list);
    return it;
 fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* co_positions(): one (lineno, end_lineno, col, end_col) per instruction */
static JSValue qp_co_positions(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    QPCodeObject *c = qp_co_data(self);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(c->bytecode);
    JSValue list, it;
    int pos = 0;

    list = qp_list_new(ctx, 0);
    if (JS_IsException(list))
        return JS_EXCEPTION;
    while (pos < b->byte_code_len) {
        QPLoc loc;
        int v[4];

        if (qp_code_addr2loc(b, pos, &loc) < 0)
            loc.lineno = loc.end_lineno = loc.col = loc.end_col = -1;
        v[0] = loc.lineno;
        v[1] = loc.end_lineno;
        v[2] = loc.col;
        v[3] = loc.end_col;
        if (qp_co_append_ints(ctx, list, 4, v) < 0) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        pos += qp_co_insn_size(b->byte_code_buf, pos);
    }
    it = qp_iter(ctx, list);
    JS_FreeValue(ctx, list);
    return it;
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
        qp_raise_value_error(ctx, "code.replace(): changing %s is not supported "
                             "(the code would have to be recompiled)", field);
        return -1;
    }
    return 0;
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

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
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
        const char *want;
        BOOL ok;

        if (QP_IS_NULL(v))
            continue;
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
        if (!ok)
            return qp_raise_type_error(ctx, "replace() argument '%s' must be %s, not %T",
                                       kwlist[i], want, v);
        switch (i) {
        case 13:
            nc.filename = v;
            break;
        case 14:
            nc.name = v;
            break;
        case 15:
            nc.qualname = v;
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
    { "co_argcount", qp_co_get, NULL, QP_CA_ARGCOUNT, NULL },
    { "co_posonlyargcount", qp_co_get, NULL, QP_CA_POSONLYARGCOUNT, NULL },
    { "co_kwonlyargcount", qp_co_get, NULL, QP_CA_KWONLYARGCOUNT, NULL },
    { "co_stacksize", qp_co_get, NULL, QP_CA_STACKSIZE, NULL },
    { "co_flags", qp_co_get, NULL, QP_CA_FLAGS, NULL },
    { "co_nlocals", qp_co_get, NULL, QP_CA_NLOCALS, NULL },
    { "co_consts", qp_co_get, NULL, QP_CA_CONSTS, NULL },
    { "co_names", qp_co_get, NULL, QP_CA_NAMES, NULL },
    { "co_filename", qp_co_get, NULL, QP_CA_FILENAME, NULL },
    { "co_name", qp_co_get, NULL, QP_CA_NAME, NULL },
    { "co_qualname", qp_co_get, NULL, QP_CA_QUALNAME, NULL },
    { "co_firstlineno", qp_co_get, NULL, QP_CA_FIRSTLINENO, NULL },
    { "co_linetable", qp_co_get, NULL, QP_CA_LINETABLE, NULL },
    { "co_exceptiontable", qp_co_get, NULL, QP_CA_EXCEPTIONTABLE, NULL },
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
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_CODE,
    .slots = {
        .tp_repr = qp_co_repr,
        .tp_hash = qp_co_hash,
        .tp_richcompare = qp_co_richcompare,
    },
    .methods = qp_co_methods,
    .getsets = qp_co_getsets,
};
