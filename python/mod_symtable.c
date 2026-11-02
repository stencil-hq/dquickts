/*
 * The _symtable module: symbol tables of the compiler for Lib/symtable.py.
 *
 * Derived from CPython Modules/symtablemodule.c and the Python-visible
 * parts of Python/symtable.c (PySTEntry_Type, ste_repr, ste_memberlist,
 * _Py_SymtableStringObjectFlags) (PSF-2.0), v3.15.2.
 *
 * The M1 symbol table (python/symtable.c) is plain C owned by its struct
 * symtable and refers to atoms of the AST arena, so symtable() converts
 * the finished table into "symtable entry" objects before freeing it: one
 * PY_OBJECT instance per block whose slots hold exactly CPython's members
 * (id, name, symbols, varnames, children, nested, type, lineno).  `id` is
 * the AST key address (CPython: PyLong_FromVoidPtr of the same pointer),
 * `symbols` an insertion-ordered dict name -> flags, `children` the list
 * of child entries.  The entry type is created by the module's exec and
 * kept on the module object (hidden property, not in the module dict:
 * CPython does not export PySTEntry_Type either).
 */

enum {
    QP_STE_ID, QP_STE_NAME, QP_STE_SYMBOLS, QP_STE_VARNAMES, QP_STE_CHILDREN,
    QP_STE_NESTED, QP_STE_TYPE, QP_STE_LINENO, QP_STE_NSLOTS
};

#define QP_SYMTABLE_PROP_ENTRY "\x01qp_symtable_entry"

static JSValue qp_ste_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    if (!s)
        return qp_raise_type_error(ctx, "uninitialized symtable entry");
    return qp_str_from_format(ctx, "<symtable entry %U(%R), line %R>",
                              s[QP_STE_NAME], s[QP_STE_ID], s[QP_STE_LINENO]);
}

static const QPMemberDef qp_ste_members[] = {
    { "id", QP_STE_ID, QP_MEMBER_READONLY, NULL },
    { "name", QP_STE_NAME, QP_MEMBER_READONLY, NULL },
    { "symbols", QP_STE_SYMBOLS, QP_MEMBER_READONLY, NULL },
    { "varnames", QP_STE_VARNAMES, QP_MEMBER_READONLY, NULL },
    { "children", QP_STE_CHILDREN, QP_MEMBER_READONLY, NULL },
    { "nested", QP_STE_NESTED, QP_MEMBER_READONLY, NULL },
    { "type", QP_STE_TYPE, QP_MEMBER_READONLY, NULL },
    { "lineno", QP_STE_LINENO, QP_MEMBER_READONLY, NULL },
    { NULL }
};

static const QPTypeSpec qp_ste_spec = {
    .name = "symtable entry",
    .flags = QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_STE_NSLOTS,
    .slots = { .tp_getattro = qp_generic_getattr, .tp_repr = qp_ste_repr },
    .members = qp_ste_members,
};

/* The "symtable entry" object of ste and (recursively) its children. */
static JSValue qp_ste_to_object(JSContext *ctx, QPType *t, PySTEntryObject *ste)
{
    JSValue obj, v, *s;
    Py_ssize_t i, pos;
    JSAtom key;
    long flags;

    obj = qp_object_new_instance(ctx, t, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = qp_instance_slots(obj);
    if (!s) {
        JS_FreeValue(ctx, obj);
        return qp_raise_system_error(ctx, "symtable entry has no field storage");
    }
    /* every slot is QP_NULL until set; a failure leaves the rest unset */
    s[QP_STE_ID] = qp_long_from_uint64(ctx, (uint64_t)(uintptr_t)ste->ste_id);
    if (JS_IsException(s[QP_STE_ID]))
        goto fail;
    s[QP_STE_NAME] = qp_str_from_atom(ctx, ste->ste_name);
    if (JS_IsException(s[QP_STE_NAME]))
        goto fail;
    s[QP_STE_SYMBOLS] = qp_dict_new(ctx);
    if (JS_IsException(s[QP_STE_SYMBOLS]))
        goto fail;
    pos = 0;
    while (qp_symmap_next(&ste->ste_symbols, &pos, &key, &flags)) {
        int r;
        v = qp_long_from_int64(ctx, flags);
        if (JS_IsException(v))
            goto fail;
        r = qp_dict_set_item_atom(ctx, s[QP_STE_SYMBOLS], key, v);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    s[QP_STE_VARNAMES] = qp_list_new(ctx, 0);
    if (JS_IsException(s[QP_STE_VARNAMES]))
        goto fail;
    for (i = 0; i < ste->ste_varnames.len; i++) {
        int r;
        v = qp_str_from_atom(ctx, ste->ste_varnames.items[i]);
        if (JS_IsException(v))
            goto fail;
        r = qp_list_append(ctx, s[QP_STE_VARNAMES], v);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    s[QP_STE_CHILDREN] = qp_list_new(ctx, 0);
    if (JS_IsException(s[QP_STE_CHILDREN]))
        goto fail;
    for (i = 0; i < ste->ste_children.len; i++) {
        int r;
        v = qp_ste_to_object(ctx, t, ste->ste_children.items[i]);
        if (JS_IsException(v))
            goto fail;
        r = qp_list_append(ctx, s[QP_STE_CHILDREN], v);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    s[QP_STE_NESTED] = qp_long_from_int64(ctx, ste->ste_nested);
    if (JS_IsException(s[QP_STE_NESTED]))
        goto fail;
    s[QP_STE_TYPE] = qp_long_from_int64(ctx, ste->ste_type);
    if (JS_IsException(s[QP_STE_TYPE]))
        goto fail;
    s[QP_STE_LINENO] = qp_long_from_int64(ctx, ste->ste_loc.lineno);
    if (JS_IsException(s[QP_STE_LINENO]))
        goto fail;
    return obj;
 fail:
    for (i = 0; i < QP_STE_NSLOTS; i++) {
        if (JS_IsException(s[i]))
            s[i] = QP_NULL;
    }
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* _Py_SymtableStringObjectFlags + the entry conversion: parse, collect
   the __future__ features (merged with the compiler flags), build. */
static JSValue qp_symtable_from_string(JSContext *ctx, QPType *t, const char *src, size_t len,
                                       const char *filename, int start, int flags,
                                       JSValueConst module)
{
    qp_error err;
    qp_arena *arena;
    _PyFutureFeatures future;
    struct symtable *st;
    mod_ty mod;
    JSValue result = JS_EXCEPTION;

    memset(&err, 0, sizeof(err));
    arena = qp_arena_new(ctx, &err);
    if (!arena) {
        qp_error_clear(&err);
        return qp_raise_memory_error(ctx);
    }
    mod = qp_parse_string(ctx, arena, src, len, filename, module, start, flags, &err);
    st = NULL;
    if (mod && _PyFuture_FromAST(mod, filename, &future, arena, &err)) {
        future.ff_features |= flags;
        st = _PySymtable_Build(mod, filename, &future, arena, &err);
    }
    if (!st) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        else
            qp_raise_memory_error(ctx);
    } else {
        result = qp_ste_to_object(ctx, t, st->st_top);
        _PySymtable_Free(st);
    }
    qp_arena_free(arena);
    qp_error_clear(&err);
    return result;
}

/* _symtable.symtable(source, filename, startstr, /, *, module=None) */
static JSValue qp_symtable_symtable(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "", "", "module", NULL };
    static const QPArgSpec spec = { "symtable", kw, 3, 3, 0 };
    JSValueConst a[4];
    JSValueConst modname = QP_NULL;
    JSValue filename = JS_UNDEFINED, start_s = JS_UNDEFINED, entry_type, result = JS_EXCEPTION;
    const char *fname_utf8 = NULL, *startstr = NULL;
    size_t start_len;
    char *src = NULL;
    size_t srclen;
    /* CPython starts from PyCF_SOURCE_IS_UTF8, which its parser ignores
       (only PyCF_IGNORE_COOKIE, added for str sources, skips the coding
       cookie); qp_parse_string treats both alike, so it is left out. */
    int flags = 0, start;
    QPType *t;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    filename = qp_bi_fs_decode(ctx, a[1]);
    if (JS_IsException(filename))
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[2])) {
        qp_bi_bad_argument(ctx, "symtable", "argument 3", "str", a[2]);
        goto done;
    }
    start_s = qp_str_value(ctx, a[2]);
    if (JS_IsException(start_s))
        goto done;
    startstr = qp_str_utf8(ctx, start_s, &start_len);
    if (!startstr)
        goto done;
    if (strlen(startstr) != start_len) {
        qp_raise_value_error(ctx, "embedded null character");
        goto done;
    }
    if (!QP_IS_NULL(a[3]) && !qp_bi_is_none(a[3]))
        modname = a[3];

    if (qp_bi_source_as_string(ctx, a[0], "symtable", "string or bytes", &flags,
                               &src, &srclen) < 0)
        goto done;
    if (strcmp(startstr, "exec") == 0) {
        start = QP_INPUT_FILE;
    } else if (strcmp(startstr, "eval") == 0) {
        start = QP_INPUT_EVAL;
    } else if (strcmp(startstr, "single") == 0) {
        start = QP_INPUT_SINGLE;
    } else {
        qp_raise_value_error(ctx, "symtable() arg 3 must be 'exec' or 'eval' or 'single'");
        goto done;
    }
    if (!QP_IS_NULL(modname) && !qp_str_check(ctx, modname)) {
        qp_raise_type_error(ctx, "symtable() argument 'module' must be str or None, not %T",
                            modname);
        goto done;
    }
    fname_utf8 = qp_str_utf8(ctx, filename, NULL);
    if (!fname_utf8)
        goto done;
    entry_type = JS_GetPropertyStr(ctx, self, QP_SYMTABLE_PROP_ENTRY);
    if (JS_IsException(entry_type))
        goto done;
    t = qp_type_from_value(entry_type);
    if (!t)
        qp_raise_system_error(ctx, "_symtable module state is missing");
    else
        result = qp_symtable_from_string(ctx, t, src, srclen, fname_utf8, start, flags,
                                         modname);
    JS_FreeValue(ctx, entry_type);
 done:
    if (src)
        js_free(ctx, src);
    if (fname_utf8)
        JS_FreeCString(ctx, fname_utf8);
    if (startstr)
        JS_FreeCString(ctx, startstr);
    JS_FreeValue(ctx, start_s);
    JS_FreeValue(ctx, filename);
    return result;
}

static const QPMethodDef qp_symtable_methods[] = {
    { "symtable", qp_symtable_symtable, QP_METH_FASTCALL | QP_METH_KEYWORDS, 0,
      "symtable($module, source, filename, startstr, /, *, module=None)\n--\n\n"
      "Return symbol and scope dictionaries used internally by compiler." },
    { NULL }
};

static int qp_symtable_add_int(JSContext *ctx, JSValueConst module, const char *name, long v)
{
    JSAtom at = JS_NewAtom(ctx, name);
    JSValue val;
    int r;
    if (at == JS_ATOM_NULL)
        return -1;
    val = qp_long_from_int64(ctx, v);
    r = JS_IsException(val) ? -1 : qp_dict_set_item_atom(ctx, qp_module_get_dict(module), at, val);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, val);
    return r;
}

static int qp_symtable_exec(JSContext *ctx, JSValueConst module)
{
    static const struct { const char *name; long value; } constants[] = {
#define QP_ST_C(x) { #x, x }
        QP_ST_C(USE), QP_ST_C(DEF_GLOBAL), QP_ST_C(DEF_NONLOCAL), QP_ST_C(DEF_LOCAL),
        QP_ST_C(DEF_PARAM), QP_ST_C(DEF_TYPE_PARAM), QP_ST_C(DEF_FREE_CLASS),
        QP_ST_C(DEF_IMPORT), QP_ST_C(DEF_BOUND), QP_ST_C(DEF_ANNOT), QP_ST_C(DEF_COMP_ITER),
        QP_ST_C(DEF_COMP_CELL),
        { "TYPE_FUNCTION", FunctionBlock }, { "TYPE_CLASS", ClassBlock },
        { "TYPE_MODULE", ModuleBlock }, { "TYPE_ANNOTATION", AnnotationBlock },
        { "TYPE_TYPE_ALIAS", TypeAliasBlock },
        { "TYPE_TYPE_PARAMETERS", TypeParametersBlock },
        { "TYPE_TYPE_VARIABLE", TypeVariableBlock },
        QP_ST_C(LOCAL), QP_ST_C(GLOBAL_EXPLICIT), QP_ST_C(GLOBAL_IMPLICIT), QP_ST_C(FREE),
        QP_ST_C(CELL),
        { "SCOPE_OFF", SCOPE_OFFSET }, QP_ST_C(SCOPE_MASK),
#undef QP_ST_C
    };
    JSValue t;
    int i;

    for (i = 0; i < (int)countof(constants); i++) {
        if (qp_symtable_add_int(ctx, module, constants[i].name, constants[i].value) < 0)
            return -1;
    }
    t = qp_type_from_spec(ctx, &qp_ste_spec);
    if (JS_IsException(t))
        return -1;
    return JS_DefinePropertyValueStr(ctx, module, QP_SYMTABLE_PROP_ENTRY, t, 0) < 0 ? -1 : 0;
}

static const QPModuleDef qp_moddef__symtable = {
    "_symtable",
    NULL,
    qp_symtable_methods,
    qp_symtable_exec,
};
