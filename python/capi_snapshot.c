/*
 * dquickts Python C API (plan D5): heap snapshots of C-extension state
 * (doc/python-plan.md §4 "Snapshots").  Included after the other capi
 * units; python/snapshot.c calls the qp_capi_snap_* hooks it declares.
 *
 * C memory (module state, PyTypeObjects, extension instances, mirrors) is
 * never written.  What the heap holds is restored as follows.
 *
 *   C modules       written as module objects (dict included) with their
 *                   execution order; the reader executes each C module
 *                   again into its restored module object (CPython-style
 *                   re-import: the module init rebuilds the state), in
 *                   the original order.
 *   types made by C C types (static or PyType_FromSpec & co: key
 *                   "T:<tp_name>") and the classes a C module's exec put
 *                   in its namespace (exceptions, namedtuples: key
 *                   "C:<module>:<qualname>") are written by reference.
 *                   While the modules run again, the type creation that
 *                   produces the same key reuses the restored type object
 *                   (qp_type_alloc_key), so identity holds everywhere.
 *   functions and   their objects are written as usual, but the C
 *   descriptors     definition they point to (registry QPMethodDef /
 *                   QPGetSetDef, wrapped slot function) is written as the
 *                   (module or type, attribute name) holding it; the
 *                   reader takes the definition of that attribute once
 *                   the owner ran again.
 *   instances       (JS_CLASS_PY_NATIVE companions, and objects of any
 *                   class whose type has a C type in its base chain, e.g.
 *                   defaultdict) are records written instead of the object
 *                   (header class PY_NATIVE): either an attribute of a C
 *                   module or C type namespace (the object its exec makes
 *                   again), or the __reduce_ex__(4) value (with
 *                   copyreg.dispatch_table first, like pickle).  The reader
 *                   allocates an empty PY_NATIVE placeholder, calls the
 *                   reconstructor once the modules ran, moves the new
 *                   object into the placeholder (shape, payload, mirror:
 *                   qp_capi_snap_bind) and applies state, list items and
 *                   dict items like pickle's BUILD/APPENDS/SETITEMS.
 *                   Objects pickle rejects make the write fail with a
 *                   TypeError naming their type.
 *
 * Order on read (qp_capi_snap_finish): C-API init, C modules in their
 * order (after each: definitions of its functions and types, its
 * attribute records, identity of its namespace), unresolved checks, then
 * the reduce records (reconstructors first resolve the records their
 * arguments reference).  Any failure aborts the read with an error naming
 * the object.
 *
 * Mirrors, pins and GC nodes: none is restored; they are made again by
 * the module execs and by later crossings.  A reconstructed object keeps
 * its mirror (rebound to the placeholder).  When the reconstructor
 * returns an object that other holders keep (a cached or immortal one),
 * those holders keep the new JS object as an alias of the same C object.
 *
 * C types without pickle support in CPython 3.15 whose state is public
 * get a snapshot reducer here (itertools.count, _json.Scanner,
 * functools._lru_cache_wrapper, _csv.Dialect), so that `import json` /
 * `import re` / `import csv` sessions can be saved.
 */

/* ========================================================================= */
/* Natives and type classification                                           */
/* ========================================================================= */

/* forward bridges are stored in the QPTypeSlots of Python subclasses of C
   types (written by value) */
static int qp_capi_snap_add_natives(JSRuntime *rt, SnapNatives *n)
{
    size_t i;

    snap_hash_str(&n->signature, "capi-snapshot-1");
    snap_hash_u32(&n->signature, countof(qp_capi_slot_map));
    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        if (qp_capi_slot_map[i].fwd && snap_natives_push(rt, n, qp_capi_slot_map[i].fwd))
            return -1;
    }
    return 0;
}

static BOOL qp_capi_snap_active(void)
{
    return qp_capi.initialized == 2 && qp_capi.type_map != NULL;
}

/* QP slot implemented by C code (forward bridge) */
static BOOL qp_capi_snap_is_c_slot(const void *fn)
{
    size_t i;
    if (!fn)
        return FALSE;
    if (qp_capi_fwd_set.count)
        return qp_capi_is_fwd(fn);
    /* before qp_capi_init_types (restore of a new process) */
    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        if (qp_capi_slot_map[i].fwd == fn)
            return TRUE;
    }
    return FALSE;
}

/* PyTypeObject of a C type (static or heap, not a builtin or a mirror) */
static PyTypeObject *qp_capi_snap_ctype(QPType *t)
{
    PyTypeObject *tp;
    if (!t || !qp_capi_snap_active())
        return NULL;
    tp = qp_capi_type_map_get(t);
    return tp && qp_capi_is_c_defined(tp) ? tp : NULL;
}

/* instances of type t carry C state */
static BOOL qp_capi_snap_ctype_chain(QPType *t)
{
    for (; t; t = t->base) {
        if (qp_capi_snap_ctype(t))
            return TRUE;
    }
    return FALSE;
}

static QPType *qp_capi_snap_type_of_obj(JSObject *p)
{
    JSObject *proto = p->shape ? p->shape->proto : NULL;
    if (proto && proto->class_id == JS_CLASS_PY_TYPE)
        return proto->u.opaque;
    return NULL;
}

/* objects written as records */
static BOOL qp_capi_snap_is_record(JSObject *p)
{
    if (p->class_id == JS_CLASS_PY_NATIVE)
        return TRUE;
    if (!QP_IS_PY_CLASS(p->class_id) || p->class_id == JS_CLASS_PY_TYPE ||
        p->class_id == JS_CLASS_PY_MODULE || !qp_capi_snap_active())
        return FALSE;
    return qp_capi_snap_ctype_chain(qp_capi_snap_type_of_obj(p));
}

static BOOL qp_capi_snap_is_capi_module(JSValueConst v)
{
    QPModule *m = qp_module_payload(v);
    return m && m->capi_seq != 0;
}

/* capi_module.c: after the exec of a C module, its order and the classes
   the exec put in its namespace */
static uint32_t qp_capi_snap_module_counter;

static void qp_capi_snap_module_ready(JSContext *ctx, JSValueConst module, const char *iname,
                                      uint32_t type_serial)
{
    QPModule *m = qp_module_payload(module);
    Py_ssize_t pos = 0;
    JSValue k, v;

    if (!m)
        return;
    m->capi_seq = ++qp_capi_snap_module_counter;
    m->capi_name = iname;
    while (qp_dict_next(m->dict, &pos, &k, &v)) {
        QPType *t;
        if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||
            JS_VALUE_GET_OBJ(v)->class_id != JS_CLASS_PY_TYPE)
            continue;
        t = JS_VALUE_GET_OBJ(v)->u.opaque;
        if (t && QP_TEXT(t)->alloc_serial > type_serial && !qp_capi_snap_ctype(t))
            QP_TEXT(t)->capi_class = 1;
    }
}

/* C definition held by attribute value v: cfunction, method / getset /
   member / wrapper descriptor, or a staticmethod / classmethod of one */
static const void *qp_capi_snap_def_of(JSValueConst v, int kind)
{
    JSObject *p;
    int depth;

    for (depth = 0; depth < 2; depth++) {
        if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
            return NULL;
        p = JS_VALUE_GET_OBJ(v);
        if (!p->u.opaque)
            return NULL;
        switch (p->class_id) {
        case JS_CLASS_PY_CFUNCTION:
            return kind == QP_SNAP_DEF_METHOD ? ((QPCFunctionObject *)p->u.opaque)->ml : NULL;
        case JS_CLASS_PY_DESCR:
            {
                QPDescr *d = p->u.opaque;
                switch (d->kind) {
                case QP_DESCR_METHOD:
                case QP_DESCR_CLASSMETHOD:
                    return kind == QP_SNAP_DEF_METHOD ? d->u.method : NULL;
                case QP_DESCR_GETSET:
                    return kind == QP_SNAP_DEF_GETSET ? d->u.getset : NULL;
                case QP_DESCR_MEMBER:
                    return kind == QP_SNAP_DEF_GETSET ? d->u.member.getset : NULL;
                case QP_DESCR_WRAPPER:
                    return kind == QP_SNAP_DEF_WRAPPED ? d->u.wrapper.wrapped : NULL;
                default:
                    return NULL;
                }
            }
        case JS_CLASS_PY_STATICMETHOD:
        case JS_CLASS_PY_CLASSMETHOD:
            v = ((QPFuncWrap *)p->u.opaque)->callable;
            break;
        default:
            return NULL;
        }
    }
    return NULL;
}

/* ========================================================================= */
/* Python helpers (pickle's reduce and BUILD semantics)                      */
/* ========================================================================= */

static const char qp_capi_snap_helper_src[] =
    "import sys as _sys\n"
    "_ns = type(_sys.implementation)\n"
    "def _name(cls):\n"
    "    m = getattr(cls, '__module__', None)\n"
    "    q = getattr(cls, '__qualname__', cls.__name__)\n"
    "    return q if m in (None, 'builtins') else m + '.' + q\n"
    "def reduce(obj, special, pre, unpicklable):\n"
    "    cls = type(obj)\n"
    "    rv = None\n"
    "    cr = _sys.modules.get('copyreg')\n"
    "    f = cr.dispatch_table.get(cls) if cr is not None else None\n"
    "    re = _sys.modules.get('re')\n"
    "    if special == 3 and re is not None and f is getattr(re, '_pickle', None):\n"
    "        f = None\n"
    "    if f is not None:\n"
    "        rv = f(obj)\n"
    "    elif pre is not None:\n"
    "        rv = pre\n"
    "    elif special == 1:\n"
    "        rv = (cls, (_ns(strict=obj.strict, object_hook=obj.object_hook,\n"
    "                        object_pairs_hook=obj.object_pairs_hook,\n"
    "                        array_hook=obj.array_hook, parse_float=obj.parse_float,\n"
    "                        parse_int=obj.parse_int,\n"
    "                        parse_constant=obj.parse_constant),))\n"
    "    elif special == 3 and re is not None:\n"
    "        # re._compile would return the cached pattern: the object being\n"
    "        # restored when re._cache is part of the snapshot\n"
    "        rv = (re._compiler.compile, (obj.pattern, obj.flags))\n"
    "    elif special == 2:\n"
    "        p = obj.cache_parameters()\n"
    "        ft = _sys.modules.get('functools')\n"
    "        if ft is None:\n"
    "            raise TypeError(f'cannot pickle {_name(cls)!r} object')\n"
    "        rv = (cls, (obj.__wrapped__, p['maxsize'], p['typed'], ft._CacheInfo),\n"
    "              dict(obj.__dict__))\n"
    "    elif special == 4:\n"
    "        # _csv.Dialect(dialect): the parameters read from an object\n"
    "        rv = (cls, (_ns(delimiter=obj.delimiter, doublequote=obj.doublequote,\n"
    "                        escapechar=obj.escapechar,\n"
    "                        lineterminator=obj.lineterminator,\n"
    "                        quotechar=obj.quotechar, quoting=obj.quoting,\n"
    "                        skipinitialspace=obj.skipinitialspace,\n"
    "                        strict=obj.strict),))\n"
    "    elif unpicklable:\n"
    "        raise TypeError(f'cannot pickle {_name(cls)!r} object')\n"
    "    else:\n"
    "        rv = obj.__reduce_ex__(4)\n"
    "    if isinstance(rv, str):\n"
    "        raise TypeError(f'cannot pickle {_name(cls)!r} object: '\n"
    "                        f'it is saved by name ({rv!r}), as a global')\n"
    "    if type(rv) is not tuple or not 2 <= len(rv) <= 6:\n"
    "        raise TypeError(f'{_name(cls)}.__reduce__ must return a string or '\n"
    "                        f'a tuple of 2 to 6 items')\n"
    "    func, args, state, li, di, setter = rv + (None,) * (6 - len(rv))\n"
    "    if not callable(func):\n"
    "        raise TypeError(f'{_name(cls)}.__reduce__: first item must be callable')\n"
    "    if type(args) is not tuple:\n"
    "        raise TypeError(f'{_name(cls)}.__reduce__: second item must be a tuple')\n"
    "    if li is not None:\n"
    "        li = list(li)\n"
    "    if di is not None:\n"
    "        di = [(k, v) for k, v in di]\n"
    "    return (func, args, state, li, di, setter)\n"
    "def build(obj, state, li, di, setter):\n"
    "    if li is not None:\n"
    "        ext = getattr(obj, 'extend', None)\n"
    "        if ext is not None:\n"
    "            ext(li)\n"
    "        else:\n"
    "            app = obj.append\n"
    "            for x in li:\n"
    "                app(x)\n"
    "    if di is not None:\n"
    "        for k, v in di:\n"
    "            obj[k] = v\n"
    "    if state is None:\n"
    "        return\n"
    "    if setter is not None:\n"
    "        setter(obj, state)\n"
    "        return\n"
    "    ss = getattr(obj, '__setstate__', None)\n"
    "    if ss is not None:\n"
    "        ss(state)\n"
    "        return\n"
    "    slotstate = None\n"
    "    if isinstance(state, tuple) and len(state) == 2:\n"
    "        state, slotstate = state\n"
    "    if state:\n"
    "        d = obj.__dict__\n"
    "        for k, v in state.items():\n"
    "            d[_sys.intern(k) if type(k) is str else k] = v\n"
    "    if slotstate:\n"
    "        for k, v in slotstate.items():\n"
    "            setattr(obj, k, v)\n";

/* helper function `name` (new reference), compiled into *pns on first use */
static JSValue qp_capi_snap_helper(JSContext *ctx, JSValue *pns, const char *name)
{
    JSValue res, f;
    JSAtom atom;
    int r;

    if (QP_IS_NULL(*pns)) {
        JSValue ns = qp_dict_new(ctx);
        if (JS_IsException(ns))
            return ns;
        if (qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __builtins__),
                                  QP_STATE(ctx)->builtins_module) < 0) {
            JS_FreeValue(ctx, ns);
            return JS_EXCEPTION;
        }
        res = qp_run_string(ctx, qp_capi_snap_helper_src, sizeof(qp_capi_snap_helper_src) - 1,
                            "<snapshot>", QP_INPUT_FILE, ns, ns);
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, ns);
            return res;
        }
        JS_FreeValue(ctx, res);
        *pns = ns;
    }
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_dict_get_item_atom(ctx, *pns, atom, &f);
    JS_FreeAtom(ctx, atom);
    if (r <= 0) {
        if (r == 0)
            qp_raise_system_error(ctx, "snapshot helper %s missing", name);
        return JS_EXCEPTION;
    }
    return f;
}

/* ========================================================================= */
/* Writer                                                                    */
/* ========================================================================= */

typedef struct QPCapiSnapEntry {
    JSValue owner;              /* module or type object (owned) */
    JSAtom name;                /* attribute name (owned) */
} QPCapiSnapEntry;

typedef struct QPCapiSnapTypeKey {
    char *key;
    JSObject *obj;
} QPCapiSnapTypeKey;

typedef struct QPCapiSnapWrite {
    BOOL scanned;
    QPCapiSnapEntry *entries;
    int entry_count, entry_size;
    SnapMap attr_map;           /* attribute value (JSObject *) -> entry */
    SnapMap def_map;            /* C definition -> entry */
    QPCapiSnapTypeKey *keys;    /* types written by reference */
    int key_count, key_size;
    JSValue helpers;            /* helper namespace or QP_NULL */
} QPCapiSnapWrite;

static QPCapiSnapWrite *qp_capi_snap_write_state(SnapWriter *w)
{
    QPCapiSnapWrite *sw = w->py;
    if (!sw) {
        sw = js_mallocz_rt(w->rt, sizeof(*sw));
        if (!sw) {
            snap_writer_oom(w);
            return NULL;
        }
        sw->helpers = QP_NULL;
        w->py = sw;
    }
    return sw;
}

static void qp_capi_snap_writer_free(SnapWriter *w)
{
    QPCapiSnapWrite *sw = w->py;
    int i;

    if (!sw)
        return;
    for (i = 0; i < sw->entry_count; i++) {
        JS_FreeValueRT(w->rt, sw->entries[i].owner);
        JS_FreeAtomRT(w->rt, sw->entries[i].name);
    }
    for (i = 0; i < sw->key_count; i++)
        js_free_rt(w->rt, sw->keys[i].key);
    js_free_rt(w->rt, sw->entries);
    js_free_rt(w->rt, sw->keys);
    snap_map_free(w->rt, &sw->attr_map);
    snap_map_free(w->rt, &sw->def_map);
    JS_FreeValueRT(w->rt, sw->helpers);
    js_free_rt(w->rt, sw);
    w->py = NULL;
}

static int qp_capi_snap_scan_value(SnapWriter *w, QPCapiSnapWrite *sw, JSValueConst owner,
                                   JSValueConst key, JSValueConst v)
{
    static const int kinds[] = { QP_SNAP_DEF_METHOD, QP_SNAP_DEF_GETSET, QP_SNAP_DEF_WRAPPED };
    const void *defs[countof(kinds)];
    BOOL attr;
    int i, n = 0, idx;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || !JS_IsString(key))
        return 0;
    attr = !snap_map_find(&sw->attr_map, JS_VALUE_GET_OBJ(v));
    for (i = 0; i < (int)countof(kinds); i++) {
        defs[i] = qp_capi_snap_def_of(v, kinds[i]);
        if (defs[i] && !snap_map_find(&sw->def_map, defs[i]))
            n++;
        else
            defs[i] = NULL;
    }
    if (!attr && !n)
        return 0;
    if (snap_resize(w->rt, (void **)&sw->entries, sizeof(sw->entries[0]), &sw->entry_size,
                    sw->entry_count + 1))
        return snap_writer_oom(w);
    idx = sw->entry_count;
    sw->entries[idx].name = JS_ValueToAtom(w->ctx, key);
    if (sw->entries[idx].name == JS_ATOM_NULL)
        return -1;
    sw->entries[idx].owner = JS_DupValue(w->ctx, owner);
    sw->entry_count++;
    if (attr && snap_map_add(w->rt, &sw->attr_map, JS_VALUE_GET_OBJ(v), idx))
        return snap_writer_oom(w);
    for (i = 0; i < (int)countof(kinds); i++) {
        if (defs[i] && snap_map_add(w->rt, &sw->def_map, defs[i], idx))
            return snap_writer_oom(w);
    }
    return 0;
}

static int qp_capi_snap_scan_dict(SnapWriter *w, QPCapiSnapWrite *sw, JSValueConst owner,
                                  JSValueConst d)
{
    Py_ssize_t pos = 0;
    JSValue k, v;

    if (JS_VALUE_GET_TAG(d) != JS_TAG_OBJECT || JS_VALUE_GET_OBJ(d)->class_id != JS_CLASS_PY_DICT)
        return 0;
    while (qp_dict_next(d, &pos, &k, &v)) {
        if (qp_capi_snap_scan_value(w, sw, owner, k, v))
            return -1;
    }
    return 0;
}

/* namespaces of the C modules (sys.modules, in execution order) and of
   the C types */
static int qp_capi_snap_scan(SnapWriter *w, QPCapiSnapWrite *sw)
{
    JSContext *ctx = w->ctx;
    QPState *s = ctx->py;
    QPCapiPMap *tm = &qp_capi_type_pmap;
    JSValue *mods = NULL, k, v;
    Py_ssize_t pos = 0;
    int i, j, n = 0, size = 0, ret = -1;
    size_t ti;

    if (sw->scanned)
        return 0;
    sw->scanned = TRUE;
    if (!s || !qp_capi_snap_active() || JS_VALUE_GET_TAG(s->modules) != JS_TAG_OBJECT)
        return 0;
    while (qp_dict_next(s->modules, &pos, &k, &v)) {
        if (!qp_capi_snap_is_capi_module(v))
            continue;
        if (snap_resize(w->rt, (void **)&mods, sizeof(mods[0]), &size, n + 1)) {
            snap_writer_oom(w);
            goto done;
        }
        mods[n++] = v;
    }
    /* creation order: a module re-exporting an object of an earlier one
       does not own it */
    for (i = 1; i < n; i++) {
        JSValue m = mods[i];
        for (j = i; j > 0 && qp_module_payload(mods[j - 1])->capi_seq >
                 qp_module_payload(m)->capi_seq; j--)
            mods[j] = mods[j - 1];
        mods[j] = m;
    }
    for (i = 0; i < n; i++) {
        if (qp_capi_snap_scan_dict(w, sw, mods[i], qp_module_get_dict(mods[i])))
            goto done;
    }
    for (ti = 0; tm->tab && ti < tm->size; ti++) {
        QPCapiPMapEntry *e = &tm->tab[ti];
        QPType *t;
        if (!e->a || e->a == QP_CAPI_PMAP_DELETED || !e->value ||
            !qp_capi_is_c_defined(e->value))
            continue;
        t = (QPType *)e->a;
        if (!t->obj || !t->obj->u.opaque)
            continue;
        if (qp_capi_snap_scan_dict(w, sw, QP_TYPE_VALUE(t), t->dict))
            goto done;
    }
    ret = 0;
done:
    js_free_rt(w->rt, mods);
    return ret;
}

static void qp_capi_snap_mark_cell(SnapWriter *w)
{
    snap_writer_cell(w, w->cur_idx)->py_record = 1;
}

static int qp_capi_snap_put_key(SnapWriter *w, const char *key)
{
    snap_put_bytes(&w->bc, (const uint8_t *)key, strlen(key));
    return 0;
}

/* *pkey (owned): the reference of a type saved by reference; 0 for a type
   saved by value */
static int qp_capi_snap_type_key(SnapWriter *w, JSObject *p, char **pkey)
{
    JSContext *ctx = w->ctx;
    QPType *t = p->u.opaque;
    QPCapiSnapWrite *sw;
    PyTypeObject *tp;
    char *key = NULL;

    *pkey = NULL;
    if (!t || !qp_capi_snap_active())
        return 0;
    tp = qp_capi_snap_ctype(t);
    if (tp) {
        size_t len = strlen(tp->tp_name) + 3;
        key = js_malloc(ctx, len);
        if (!key)
            return -1;
        snprintf(key, len, "T:%s", tp->tp_name);
    } else if (QP_TEXT(t)->capi_class) {
        SnapMapEntry *me;
        QPCapiSnapEntry *en;
        const char *mod, *qn;
        JSValue modname;
        size_t len;
        if (!(sw = qp_capi_snap_write_state(w)) || qp_capi_snap_scan(w, sw))
            return -1;
        me = snap_map_find(&sw->attr_map, p);
        if (!me)
            return 0;   /* no longer in the namespace of its module: by value */
        en = &sw->entries[me->val];
        if (!qp_capi_snap_is_capi_module(en->owner))
            return 0;
        modname = JS_UNDEFINED;
        if (qp_dict_get_item_atom(ctx, qp_module_get_dict(en->owner), QP_ATOM(ctx, __name__),
                                  &modname) <= 0 || !JS_IsString(modname) ||
            !JS_IsString(t->qualname)) {
            JS_FreeValue(ctx, modname);
            return 0;
        }
        mod = qp_str_utf8(ctx, modname, NULL);
        qn = mod ? qp_str_utf8(ctx, t->qualname, NULL) : NULL;
        if (qn) {
            len = strlen(mod) + strlen(qn) + 4;
            key = js_malloc(ctx, len);
            if (key)
                snprintf(key, len, "C:%s:%s", mod, qn);
        }
        if (qn)
            JS_FreeCString(ctx, qn);
        if (mod)
            JS_FreeCString(ctx, mod);
        JS_FreeValue(ctx, modname);
        if (!key)
            return -1;
    } else {
        return 0;
    }
    *pkey = key;
    return 1;
}

/* python/snapshot.c: the subclass list of a type omits the types saved by
   reference (they link themselves again when their module runs) */
static int qp_capi_snap_type_skip_sub(SnapWriter *w, QPType *sub)
{
    char *key;
    int ret = qp_capi_snap_type_key(w, sub->obj, &key);
    js_free(w->ctx, key);
    return ret;
}

/* u8 2 + key for a type made by C, 1 for other types */
static int qp_capi_snap_write_type_ref(SnapWriter *w, JSObject *p)
{
    JSContext *ctx = w->ctx;
    QPCapiSnapWrite *sw;
    char *key;
    int i, ret;

    ret = qp_capi_snap_type_key(w, p, &key);
    if (ret <= 0)
        return ret < 0 ? -1 : 1;
    sw = qp_capi_snap_write_state(w);
    if (!sw) {
        js_free(ctx, key);
        return -1;
    }
    for (i = 0; i < sw->key_count; i++) {
        if (!strcmp(sw->keys[i].key, key) && sw->keys[i].obj != p) {
            JS_ThrowTypeError(ctx, "snapshot: cannot save type '%s': two types made by C "
                              "extensions have this name", key + 2);
            js_free(ctx, key);
            return -1;
        }
    }
    if (snap_resize(w->rt, (void **)&sw->keys, sizeof(sw->keys[0]), &sw->key_size,
                    sw->key_count + 1)) {
        js_free(ctx, key);
        return snap_writer_oom(w);
    }
    sw->keys[sw->key_count].key = key;
    sw->keys[sw->key_count].obj = p;
    sw->key_count++;
    bc_put_u8(&w->bc, 2);
    return qp_capi_snap_put_key(w, key);
}

/* definition made by the C API (method registry, or a wrapped C slot
   function of a C type namespace) */
static BOOL qp_capi_snap_is_capi_def(SnapWriter *w, const void *def, int kind)
{
    QPCapiSnapWrite *sw;
    QPCapiDef *d;

    if (!qp_capi_snap_active())
        return FALSE;
    switch (kind) {
    case QP_SNAP_DEF_METHOD:
        d = qp_capi_def_at(((const QPMethodDef *)def)->magic);
        if (d && &d->qml == def)
            return TRUE;
        break;
    case QP_SNAP_DEF_GETSET:
        d = qp_capi_def_at(((const QPGetSetDef *)def)->magic);
        if (d && &d->qgs == def)
            return TRUE;
        break;
    default:
        break;
    }
    /* copies in the spec arrays of C types, wrapped C slot functions */
    sw = qp_capi_snap_write_state(w);
    if (!sw || qp_capi_snap_scan(w, sw)) {
        JS_FreeValue(w->ctx, JS_GetException(w->ctx));
        return FALSE;
    }
    return snap_map_find(&sw->def_map, def) != NULL;
}

static int qp_capi_snap_put_def(SnapWriter *w, const void *def, int kind)
{
    QPCapiSnapWrite *sw = qp_capi_snap_write_state(w);
    SnapMapEntry *me;
    QPCapiSnapEntry *en;

    if (!sw || qp_capi_snap_scan(w, sw))
        return -1;
    me = snap_map_find(&sw->def_map, def);
    if (!me) {
        const char *name = kind == QP_SNAP_DEF_METHOD ? ((const QPMethodDef *)def)->name :
            kind == QP_SNAP_DEF_GETSET ? ((const QPGetSetDef *)def)->name : "?";
        JS_ThrowTypeError(w->ctx, "snapshot: cannot save C function or descriptor '%s': its "
                          "definition is not an attribute of a C module or C type",
                          name ? name : "?");
        return -1;
    }
    en = &sw->entries[me->val];
    if (snap_put_value(w, en->owner))
        return -1;
    return bc_put_atom(&w->bc, en->name);
}

/* CPython object_getstate_default(required): an object of a C type with
   C fields, without __reduce__, __reduce_ex__, __getstate__ or
   __getnewargs(_ex)__ of its own, cannot be pickled */
static BOOL qp_capi_snap_default_unpicklable(JSContext *ctx, QPType *t)
{
    static const QPAtomId own[] = {
        QP_ATOM___reduce_ex__, QP_ATOM___reduce__, QP_ATOM___getstate__,
    };
    QPType *obj = QP_TYPE(ctx, object), *b;
    PyTypeObject *tp = NULL;
    Py_ssize_t size;
    int i;

    for (i = 0; i < (int)countof(own); i++) {
        JSAtom a = QP_STATE(ctx)->atoms[own[i]];
        JSValue v = qp_type_lookup(ctx, t, a);
        if (QP_IS_NULL(v) || !js_same_value(ctx, v, qp_type_lookup(ctx, obj, a)))
            return FALSE;
    }
    {
        static const char * const names[] = { "__getnewargs_ex__", "__getnewargs__" };
        for (i = 0; i < (int)countof(names); i++) {
            JSAtom a = JS_NewAtom(ctx, names[i]);
            BOOL found = a != JS_ATOM_NULL && !QP_IS_NULL(qp_type_lookup(ctx, t, a));
            JS_FreeAtom(ctx, a);
            if (found)
                return FALSE;
        }
    }
    for (b = t; b && !tp; b = b->base)
        tp = qp_capi_snap_ctype(b);
    if (!tp || tp->tp_itemsize != 0)
        return FALSE;
    size = sizeof(PyObject);
    if (tp->tp_dictoffset)
        size += sizeof(PyObject *);
    if (tp->tp_weaklistoffset)
        size += sizeof(PyObject *);
    return tp->tp_basicsize > size;
}

/* snapshot reducer of a C type CPython cannot pickle; *ppre: a reduce
   value computed here (QP_NULL: none) */
typedef struct {
    PyObject_HEAD
    Py_ssize_t cnt;
    PyObject *long_cnt;
    PyObject *long_step;
} QPCapiSnapCount;              /* itertoolsmodule.c countobject */

static int qp_capi_snap_special(JSContext *ctx, JSValueConst obj, QPType *t, JSValue *ppre)
{
    PyTypeObject *tp = qp_capi_snap_ctype(t);
    PyObject *op, *rv;

    *ppre = QP_NULL;
    if (!tp)
        return 0;
    if (!strcmp(tp->tp_name, "_json.Scanner"))
        return 1;
    if (!strcmp(tp->tp_name, "functools._lru_cache_wrapper"))
        return 2;
    if (!strcmp(tp->tp_name, "re.Pattern"))
        return 3;
    if (!strcmp(tp->tp_name, "_csv.Dialect"))
        return 4;
    if (strcmp(tp->tp_name, "itertools.count") || tp->tp_basicsize != sizeof(QPCapiSnapCount))
        return 0;
    op = qp_capi_new_ref(ctx, obj);
    if (!op)
        return -1;
    if (Py_TYPE(op) != tp) {
        Py_DECREF(op);
        return 0;
    }
    {
        QPCapiSnapCount *c = (QPCapiSnapCount *)op;
        if (c->long_cnt)
            rv = Py_BuildValue("O(OO)", (PyObject *)tp, c->long_cnt, c->long_step);
        else
            rv = Py_BuildValue("O(n)", (PyObject *)tp, c->cnt);
    }
    Py_DECREF(op);
    if (!rv)
        return -1;
    *ppre = qp_capi_to_js(ctx, rv);
    Py_DECREF(rv);
    return JS_IsException(*ppre) ? -1 : 0;
}

/* re-throw the pending exception as "snapshot: cannot save ..." */
static int qp_capi_snap_write_error(SnapWriter *w, JSObject *p)
{
    JSContext *ctx = w->ctx;
    char buf[ATOM_GET_STR_BUF_SIZE];
    JSValue exc = JS_GetException(ctx), s;
    const char *msg = NULL;

    s = qp_str(ctx, exc);
    if (!JS_IsException(s))
        msg = qp_str_utf8(ctx, s, NULL);
    if (!msg)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_ThrowTypeError(ctx, "snapshot: cannot save C extension object '%s': %s",
                      qp_snap_type_name(w->rt, p, buf, sizeof(buf)), msg ? msg : "error");
    if (msg)
        JS_FreeCString(ctx, msg);
    if (!JS_IsException(s))
        JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, exc);
    return -1;
}

/* Record payload (after the object part: prototype, flags, no property):
     u8 1, value owner, atom name               attribute of a C namespace
     u8 2, value func, args, state, listitems, dictitems, state_setter
                                                __reduce_ex__ value */
static int qp_capi_snap_write_record(SnapWriter *w, JSObject *p)
{
    JSContext *ctx = w->ctx;
    QPCapiSnapWrite *sw;
    SnapMapEntry *me;
    JSValue obj = JS_MKPTR(JS_TAG_OBJECT, p), f, rv, pre;
    JSValueConst args[4];
    QPType *t;
    int special, i;

    if (p->class_id == JS_CLASS_PY_TYPE) {
        /* a type saved by reference: no property (the type is made
           again; its properties may hold values of the writer process,
           and a base snapshot must be the same bytes in every process) */
        char *key;
        int ret = qp_capi_snap_type_key(w, p, &key), flags = 0;
        js_free(ctx, key);
        if (ret <= 0)
            return ret < 0 ? -1 : 1;
        if (snap_put_obj(w, p->shape->proto))
            return -1;
        if (p->extensible)
            flags |= SNAP_OBJ_EXTENSIBLE;
        if (p->has_immutable_prototype)
            flags |= SNAP_OBJ_IMMUTABLE_PROTO;
        if (p->is_constructor)
            flags |= SNAP_OBJ_CONSTRUCTOR;
        if (p->py_dict_mode)
            flags |= SNAP_OBJ_PY_DICT_MODE;
        bc_put_u8(&w->bc, flags);
        if (bc_put_atom(&w->bc, JS_ATOM_NULL))
            return -1;
        return snap_write_payload(w, p);
    }
    if (!qp_capi_snap_is_record(p))
        return 1;
    if (w->cur_idx < w->base_cell_count && !snap_writer_cell(w, w->cur_idx)->py_record) {
        JS_ThrowTypeError(ctx, "snapshot: an object of the base snapshot became a C "
                          "extension object; write a full snapshot");
        return -1;
    }
    if (!ctx->py)
        return qp_snap_reject(w, p, "C extension object without Python state");
    sw = qp_capi_snap_write_state(w);
    if (!sw || qp_capi_snap_scan(w, sw))
        return -1;
    qp_capi_snap_mark_cell(w);
    if (snap_put_obj(w, p->shape->proto))
        return -1;
    bc_put_u8(&w->bc, p->extensible ? SNAP_OBJ_EXTENSIBLE : 0);
    if (bc_put_atom(&w->bc, JS_ATOM_NULL))
        return -1;

    me = snap_map_find(&sw->attr_map, p);
    if (me) {
        QPCapiSnapEntry *en = &sw->entries[me->val];
        bc_put_u8(&w->bc, 1);
        if (snap_put_value(w, en->owner))
            return -1;
        return bc_put_atom(&w->bc, en->name);
    }

    t = qp_capi_snap_type_of_obj(p);
    if (!t)
        return qp_snap_reject(w, p, "C extension object without a type");
    special = qp_capi_snap_special(ctx, obj, t, &pre);
    if (special < 0)
        return qp_capi_snap_write_error(w, p);
    f = qp_capi_snap_helper(ctx, &sw->helpers, "reduce");
    if (JS_IsException(f)) {
        JS_FreeValue(ctx, pre);
        return qp_capi_snap_write_error(w, p);
    }
    args[0] = obj;
    args[1] = JS_NewInt32(ctx, special);
    args[2] = QP_IS_NULL(pre) ? JS_UNDEFINED : pre;
    args[3] = JS_NewBool(ctx, special == 0 && QP_IS_NULL(pre) &&
                         qp_capi_snap_default_unpicklable(ctx, t));
    rv = qp_vectorcall(ctx, f, args, 4, QP_NULL);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, pre);
    if (JS_IsException(rv))
        return qp_capi_snap_write_error(w, p);
    /* the reduce value is referenced by address until the end */
    if (snap_keep(w, rv))
        return -1;
    bc_put_u8(&w->bc, 2);
    for (i = 0; i < 6; i++) {
        if (snap_put_value(w, QP_TUPLE_ITEMS(rv)[i]))
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Reader                                                                    */
/* ========================================================================= */

enum {
    QP_CAPI_SNAP_REC_ATTR = 1,
    QP_CAPI_SNAP_REC_REDUCE = 2,
};

typedef struct QPCapiSnapRec {
    JSObject *obj;              /* placeholder (reader cell) */
    uint8_t kind;               /* QP_CAPI_SNAP_REC_* */
    uint8_t state;              /* 0 pending, 1 in progress, 2 done */
    JSAtom name;                /* ATTR */
    JSValue v[6];               /* ATTR: v[0] owner; REDUCE: reduce value */
} QPCapiSnapRec;

typedef struct QPCapiSnapDef {
    const void **pdef;          /* field of a reader cell's payload */
    JSValue owner;
    JSAtom name;
    uint8_t kind;
    uint8_t done;
} QPCapiSnapDef;

typedef struct QPCapiSnapMod {
    JSObject *obj;
    uint32_t seq;
    char *name;                 /* inittab entry */
} QPCapiSnapMod;

typedef struct QPCapiSnapRead {
    QPCapiSnapRec *recs;
    int rec_count, rec_size;
    SnapMap rec_map;            /* placeholder -> index + 1 */
    QPCapiSnapDef *defs;
    int def_count, def_size;
    QPCapiSnapMod *mods;
    int mod_count, mod_size;
    QPCapiSnapTypeKey *types;   /* types saved by reference, obj NULL once adopted */
    int type_count, type_size;
    const char *cur_module;     /* name of the C module being executed */
    JSValue helpers;
    JSValue merges;             /* list of (new dict, saved dict), see
                                   qp_capi_snap_restore_ns */
} QPCapiSnapRead;

/* restore in progress (qp_capi_snap_finish) */
static QPCapiSnapRead *qp_capi_snap_cur;

static QPCapiSnapRead *qp_capi_snap_read_state(SnapReader *r)
{
    QPSnapRead *s = qp_snap_read_state_get(r);
    if (!s)
        return NULL;
    if (!s->capi) {
        s->capi = js_mallocz_rt(r->rt, sizeof(QPCapiSnapRead));
        if (!s->capi) {
            JS_ThrowOutOfMemory(r->ctx);
            return NULL;
        }
        s->capi->helpers = QP_NULL;
        s->capi->merges = QP_NULL;
    }
    return s->capi;
}

static void qp_capi_snap_reader_free(SnapReader *r)
{
    QPSnapRead *s = r->py;
    QPCapiSnapRead *cs = s ? s->capi : NULL;
    JSRuntime *rt = r->rt;
    int i, j;

    if (!cs)
        return;
    if (qp_capi_snap_cur == cs)
        qp_capi_snap_cur = NULL;
    for (i = 0; i < cs->rec_count; i++) {
        for (j = 0; j < 6; j++)
            JS_FreeValueRT(rt, cs->recs[i].v[j]);
        if (cs->recs[i].name != JS_ATOM_NULL)
            JS_FreeAtomRT(rt, cs->recs[i].name);
    }
    for (i = 0; i < cs->def_count; i++) {
        JS_FreeValueRT(rt, cs->defs[i].owner);
        JS_FreeAtomRT(rt, cs->defs[i].name);
    }
    for (i = 0; i < cs->type_count; i++)
        js_free_rt(rt, cs->types[i].key);
    for (i = 0; i < cs->mod_count; i++)
        js_free_rt(rt, cs->mods[i].name);
    js_free_rt(rt, cs->recs);
    js_free_rt(rt, cs->defs);
    js_free_rt(rt, cs->mods);
    js_free_rt(rt, cs->types);
    snap_map_free(rt, &cs->rec_map);
    JS_FreeValueRT(rt, cs->helpers);
    JS_FreeValueRT(rt, cs->merges);
    js_free_rt(rt, cs);
    s->capi = NULL;
}

/* length-prefixed string of the record or reference being read (owned) */
static char *qp_capi_snap_get_str(SnapReader *r)
{
    uint32_t len;
    char *s;

    if (snap_get_leb(r, &len))
        return NULL;
    if (len == 0 || len > 4096 || len > r->bc.buf_end - r->bc.ptr) {
        snap_error(r, "invalid C extension reference");
        return NULL;
    }
    s = js_malloc_rt(r->rt, len + 1);
    if (!s) {
        snap_error(r, "out of memory");
        return NULL;
    }
    memcpy(s, r->bc.ptr, len);
    s[len] = '\0';
    r->bc.ptr += len;
    return s;
}

/* module payload of a C module: u8 2, leb order, inittab entry, then the
   usual payload (python/snapshot_buf.c) */
static int qp_capi_snap_module_write(SnapWriter *w, QPModule *m)
{
    const char *name = m->capi_name ? m->capi_name : "";
    bc_put_leb128(&w->bc, m->capi_seq);
    snap_put_bytes(&w->bc, (const uint8_t *)name, strlen(name));
    return 0;
}

static int qp_capi_snap_module_read(SnapReader *r, JSObject *p)
{
    QPCapiSnapRead *cs = qp_capi_snap_read_state(r);
    uint32_t seq;

    if (!cs || snap_get_leb(r, &seq))
        return -1;
    if (!seq)
        return snap_error(r, "invalid C extension module");
    if (snap_resize(r->rt, (void **)&cs->mods, sizeof(cs->mods[0]), &cs->mod_size,
                    cs->mod_count + 1))
        return snap_error(r, "out of memory");
    cs->mods[cs->mod_count].obj = p;
    cs->mods[cs->mod_count].seq = seq;
    cs->mods[cs->mod_count].name = qp_capi_snap_get_str(r);
    if (!cs->mods[cs->mod_count].name)
        return -1;
    cs->mod_count++;
    return 0;
}

static int qp_capi_snap_read_type_ref(SnapReader *r, JSObject *p)
{
    QPCapiSnapRead *cs;
    char *key;

    snap_payload_ready(r);
    if (!qp_snap_type_ensure(r, p) || !(cs = qp_capi_snap_read_state(r)))
        return -1;
    if (snap_resize(r->rt, (void **)&cs->types, sizeof(cs->types[0]), &cs->type_size,
                    cs->type_count + 1))
        return snap_error(r, "out of memory");
    key = qp_capi_snap_get_str(r);
    if (!key)
        return -1;
    if (strncmp(key, "T:", 2) && strncmp(key, "C:", 2)) {
        js_free_rt(r->rt, key);
        return snap_error(r, "invalid C type reference");
    }
    cs->types[cs->type_count].key = key;
    cs->types[cs->type_count].obj = p;
    cs->type_count++;
    return 0;
}

static int qp_capi_snap_get_def(SnapReader *r, const void **pdef, int kind)
{
    QPCapiSnapRead *cs = qp_capi_snap_read_state(r);
    QPCapiSnapDef *d;

    if (!cs)
        return -1;
    if (snap_resize(r->rt, (void **)&cs->defs, sizeof(cs->defs[0]), &cs->def_size,
                    cs->def_count + 1))
        return snap_error(r, "out of memory");
    d = &cs->defs[cs->def_count];
    d->pdef = pdef;
    d->owner = JS_UNDEFINED;
    d->name = JS_ATOM_NULL;
    d->kind = kind;
    d->done = 0;
    cs->def_count++;
    if (snap_get_value(r, &d->owner) || bc_get_atom(&r->bc, &d->name))
        return -1;
    return 0;
}

static int qp_capi_snap_read_record(SnapReader *r, JSObject *p)
{
    QPCapiSnapRead *cs = qp_capi_snap_read_state(r);
    QPCapiSnapRec *rec;
    uint8_t kind;
    int i, n;

    snap_payload_ready(r);
    if (!cs || bc_get_u8(&r->bc, &kind))
        return -1;
    if (kind != QP_CAPI_SNAP_REC_ATTR && kind != QP_CAPI_SNAP_REC_REDUCE)
        return snap_error(r, "invalid C extension object record");
    if (snap_resize(r->rt, (void **)&cs->recs, sizeof(cs->recs[0]), &cs->rec_size,
                    cs->rec_count + 1) ||
        snap_map_add(r->rt, &cs->rec_map, p, cs->rec_count + 1))
        return snap_error(r, "out of memory");
    rec = &cs->recs[cs->rec_count++];
    memset(rec, 0, sizeof(*rec));
    rec->obj = p;
    rec->kind = kind;
    rec->name = JS_ATOM_NULL;
    for (i = 0; i < 6; i++)
        rec->v[i] = JS_UNDEFINED;
    n = kind == QP_CAPI_SNAP_REC_ATTR ? 1 : 6;
    for (i = 0; i < n; i++) {
        JSValue v;
        if (snap_get_value(r, &v))
            return -1;
        rec = &cs->recs[cs->rec_count - 1];
        rec->v[i] = v;
    }
    if (kind == QP_CAPI_SNAP_REC_ATTR &&
        bc_get_atom(&r->bc, &cs->recs[cs->rec_count - 1].name))
        return -1;
    return 0;
}

/* ---- adoption of the types saved by reference ---------------------------- */

static BOOL qp_capi_snap_adopting(void)
{
    return qp_capi_snap_cur != NULL;
}

static const char *qp_capi_snap_adopt_module(void)
{
    return qp_capi_snap_cur ? qp_capi_snap_cur->cur_module : NULL;
}

static JSObject *qp_capi_snap_adopt_take(JSContext *ctx, const char *key)
{
    QPCapiSnapRead *cs = qp_capi_snap_cur;
    int i;

    if (!cs)
        return NULL;
    for (i = 0; i < cs->type_count; i++) {
        JSObject *p = cs->types[i].obj;
        if (p && !strcmp(cs->types[i].key, key)) {
            cs->types[i].obj = NULL;
            return p;
        }
    }
    return NULL;
}

static BOOL qp_capi_snap_type_pending(QPCapiSnapRead *cs, JSObject *p)
{
    int i;
    for (i = 0; i < cs->type_count; i++) {
        if (cs->types[i].obj == p)
            return TRUE;
    }
    return FALSE;
}

/* ---- finish -------------------------------------------------------------- */

/* the pending exception as the read error */
static int qp_capi_snap_fail(SnapReader *r, JSContext *ctx, const char *what, JSAtom name)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    JSValue exc = JS_GetException(ctx), s = JS_EXCEPTION;
    const char *msg = NULL;

    if (!JS_IsUndefined(exc) && !JS_IsNull(exc) && !JS_IsUninitialized(exc)) {
        s = qp_str(ctx, exc);
        if (!JS_IsException(s))
            msg = qp_str_utf8(ctx, s, NULL);
        if (!msg)
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    snap_error(r, "restoring C extension state failed: %s%s%s: %s", what,
               name != JS_ATOM_NULL ? " " : "",
               name != JS_ATOM_NULL ? JS_AtomGetStr(ctx, buf, sizeof(buf), name) : "",
               msg ? msg : "error");
    if (msg)
        JS_FreeCString(ctx, msg);
    if (!JS_IsException(s))
        JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, exc);
    return -1;
}

/* namespace of a definition / attribute owner, or QP_NULL while the owner
   is not restored yet */
static JSValue qp_capi_snap_owner_dict(QPCapiSnapRead *cs, JSValueConst owner)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(owner) != JS_TAG_OBJECT)
        return QP_NULL;
    p = JS_VALUE_GET_OBJ(owner);
    if (p->class_id == JS_CLASS_PY_MODULE)
        return qp_capi_snap_is_capi_module(owner) ? qp_module_get_dict(owner) : QP_NULL;
    if (p->class_id == JS_CLASS_PY_TYPE && p->u.opaque && !qp_capi_snap_type_pending(cs, p)) {
        QPType *t = p->u.opaque;
        return JS_VALUE_GET_TAG(t->dict) == JS_TAG_OBJECT ? t->dict : QP_NULL;
    }
    return QP_NULL;
}

/* definitions whose owner ran again */
static int qp_capi_snap_resolve_defs(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs,
                                     BOOL all)
{
    int i, ret;

    for (i = 0; i < cs->def_count; i++) {
        QPCapiSnapDef *d = &cs->defs[i];
        JSValue dict, v;
        const void *def;
        if (d->done)
            continue;
        dict = qp_capi_snap_owner_dict(cs, d->owner);
        if (QP_IS_NULL(dict)) {
            if (!all)
                continue;
            return snap_error(r, "restoring C extension state failed: the owner of the C "
                              "definition %s was not restored",
                              JS_AtomGetStr(ctx, (char[ATOM_GET_STR_BUF_SIZE]){0},
                                            ATOM_GET_STR_BUF_SIZE, d->name));
        }
        ret = qp_dict_get_item_atom(ctx, dict, d->name, &v);
        if (ret < 0)
            return qp_capi_snap_fail(r, ctx, "C definition", d->name);
        def = ret > 0 ? qp_capi_snap_def_of(v, d->kind) : NULL;
        JS_FreeValue(ctx, v);
        if (!def) {
            char buf[ATOM_GET_STR_BUF_SIZE];
            return snap_error(r, "restoring C extension state failed: the C definition "
                              "of '%s' is gone after the C module ran again",
                              JS_AtomGetStr(ctx, buf, sizeof(buf), d->name));
        }
        *d->pdef = def;
        d->done = 1;
    }
    return 0;
}

/* P (an empty PY_NATIVE placeholder) becomes the object `rv` (owned):
   shape, payload and mirror move to P.  If something else holds rv, rv
   stays an alias of the same C object (C-extension instances only). */
static int qp_capi_snap_bind(JSContext *ctx, JSObject *P, JSValue rv)
{
    JSObject *R;
    QPCapiHead *h;
    PyObject *op = NULL;
    JSShape *sh;
    JSProperty *prop;
    BOOL owns, shared;

    if (JS_VALUE_GET_TAG(rv) != JS_TAG_OBJECT) {
        JS_FreeValue(ctx, rv);
        qp_raise_type_error(ctx, "the reconstructor did not return a C extension object");
        return -1;
    }
    R = JS_VALUE_GET_OBJ(rv);
    if (R == P) {
        JS_FreeValue(ctx, rv);
        return 0;
    }
    if (!QP_IS_PY_CLASS(R->class_id) || R->class_id == JS_CLASS_PY_TYPE ||
        R->class_id == JS_CLASS_PY_MODULE) {
        JS_FreeValue(ctx, rv);
        qp_raise_type_error(ctx, "the reconstructor returned an object of an unsupported class");
        return -1;
    }
    h = R->py_has_mirror ? qp_capi_head_of_jobj(R) : NULL;
    if (R->class_id == JS_CLASS_PY_NATIVE && !h) {
        /* static instance (its JS object is owned by qp_capi.static_map)
           or a native without payload: P aliases its C object */
        op = R->u.opaque;
        if (!op) {
            JS_FreeValue(ctx, rv);
            qp_raise_type_error(ctx, "the reconstructor returned an empty C extension object");
            return -1;
        }
        if (JS_SetPrototypeInternal(ctx, JS_MKPTR(JS_TAG_OBJECT, P),
                                    JS_MKPTR(JS_TAG_OBJECT, R->shape->proto), FALSE) < 0) {
            JS_FreeValue(ctx, rv);
            return -1;
        }
        Py_INCREF(op);      /* the placeholder has no finalizer: kept */
        P->u.opaque = op;
        JS_FreeValue(ctx, rv);
        return 0;
    }
    owns = h && (h->flags & QP_CAPI_F_OWNS_V);
    shared = js_rc(R)->ref_count > 1 + (owns ? 1 : 0);
    if (shared && R->class_id != JS_CLASS_PY_NATIVE) {
        JS_FreeValue(ctx, rv);
        qp_raise_type_error(ctx, "the reconstructor returned a shared object");
        return -1;
    }
    if (h)
        op = QP_CAPI_OBJ(h);
    /* move the object */
    sh = P->shape;
    prop = P->prop;
    P->shape = R->shape;
    P->prop = R->prop;
    R->shape = sh;
    R->prop = prop;
    P->class_id = R->class_id;
    P->u = R->u;
    P->extensible = R->extensible;
    P->is_exotic = R->is_exotic;
    P->fast_array = R->fast_array;
    P->is_constructor = R->is_constructor;
    P->py_dict_mode = R->py_dict_mode;
    P->py_finalized = R->py_finalized;
    R->class_id = JS_CLASS_PY_NATIVE;
    memset(&R->u, 0, sizeof(R->u));
    R->is_exotic = 0;
    R->fast_array = 0;
    R->py_dict_mode = 0;
    R->py_finalized = 0;
    if (h) {
        if (P->class_id != JS_CLASS_PY_NATIVE) {
            qp_capi_map_del(qp_capi.obj_map, (uintptr_t)R);
            if (qp_capi_map_put(qp_capi.obj_map, (uintptr_t)P, h) < 0) {
                JS_FreeValue(ctx, rv);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        }
        R->py_has_mirror = 0;
        P->py_has_mirror = 1;
        h->v = JS_MKPTR(JS_TAG_OBJECT, P);
        if (owns) {
            JS_DupValue(ctx, h->v);
            JS_FreeValue(ctx, rv);      /* the mirror's reference to R */
        }
    }
    if (shared && op) {
        /* the other holders keep R: an alias of the same C object */
        Py_INCREF(op);
        R->u.opaque = op;
    }
    JS_FreeValue(ctx, rv);
    return 0;
}

static int qp_capi_snap_resolve_attr(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs,
                                     QPCapiSnapRec *rec)
{
    JSValue dict = qp_capi_snap_owner_dict(cs, rec->v[0]), v;
    int ret;

    if (QP_IS_NULL(dict))
        return 0;
    ret = qp_dict_get_item_atom(ctx, dict, rec->name, &v);
    if (ret <= 0) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        if (ret < 0)
            return qp_capi_snap_fail(r, ctx, "C extension object", rec->name);
        return snap_error(r, "restoring C extension state failed: attribute '%s' is gone "
                          "after the C module ran again",
                          JS_AtomGetStr(ctx, buf, sizeof(buf), rec->name));
    }
    /* the namespace holds the placeholder from now on */
    if (qp_dict_set_item_atom(ctx, dict, rec->name, JS_MKPTR(JS_TAG_OBJECT, rec->obj)) < 0) {
        JS_FreeValue(ctx, v);
        return qp_capi_snap_fail(r, ctx, "C extension object", rec->name);
    }
    if (JS_VALUE_GET_OBJ(rec->v[0])->class_id == JS_CLASS_PY_TYPE)
        qp_type_modified(ctx, JS_VALUE_GET_OBJ(rec->v[0])->u.opaque);
    rec->state = 2;
    if (qp_capi_snap_bind(ctx, rec->obj, v) < 0)
        return qp_capi_snap_fail(r, ctx, "C extension object", rec->name);
    return 0;
}

static int qp_capi_snap_resolve_attrs(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs)
{
    int i;
    for (i = 0; i < cs->rec_count; i++) {
        QPCapiSnapRec *rec = &cs->recs[i];
        if (rec->kind == QP_CAPI_SNAP_REC_ATTR && rec->state == 0 &&
            qp_capi_snap_resolve_attr(r, ctx, cs, rec))
            return -1;
    }
    return 0;
}

static int qp_capi_snap_resolve(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs,
                                QPCapiSnapRec *rec);

/* records that the values of v (containers, recursively) reference */
static int qp_capi_snap_resolve_deps(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs,
                                     JSValueConst v, int depth, BOOL strict)
{
    JSObject *p;
    SnapMapEntry *me;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || depth > 64)
        return 0;
    p = JS_VALUE_GET_OBJ(v);
    me = snap_map_find(&cs->rec_map, p);
    if (me) {
        QPCapiSnapRec *rec = &cs->recs[me->val - 1];
        if (rec->state == 2)
            return 0;
        if (rec->state == 1) {
            if (!strict)
                return 0;
            return snap_error(r, "restoring C extension state failed: a C extension object "
                              "is needed to reconstruct itself");
        }
        return qp_capi_snap_resolve(r, ctx, cs, rec);
    }
    switch (p->class_id) {
    case JS_CLASS_PY_TUPLE:
        {
            QPTuple *t = p->u.opaque;
            uint32_t i;
            for (i = 0; t && i < t->len; i++) {
                if (qp_capi_snap_resolve_deps(r, ctx, cs, t->items[i], depth + 1, strict))
                    return -1;
            }
        }
        break;
    case JS_CLASS_PY_DICT:
        {
            Py_ssize_t pos = 0;
            JSValue k, val;
            while (qp_dict_next(v, &pos, &k, &val)) {
                if (qp_capi_snap_resolve_deps(r, ctx, cs, k, depth + 1, strict) ||
                    qp_capi_snap_resolve_deps(r, ctx, cs, val, depth + 1, strict))
                    return -1;
            }
        }
        break;
    default:
        break;
    }
    if (qp_list_check(ctx, v)) {
        Py_ssize_t i, n;
        for (i = 0; qp_list_items(v, &n) && i < n; i++) {
            if (qp_capi_snap_resolve_deps(r, ctx, cs, qp_list_items(v, &n)[i], depth + 1,
                                          strict))
                return -1;
        }
    }
    return 0;
}

static int qp_capi_snap_resolve(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs,
                                QPCapiSnapRec *rec)
{
    JSValue f, res, args;
    JSValueConst a[5];
    char tbuf[ATOM_GET_STR_BUF_SIZE], what[ATOM_GET_STR_BUF_SIZE + 64];
    int i, idx = rec - cs->recs;

    if (rec->state == 2)
        return 0;
    snprintf(what, sizeof(what), "reconstructing C extension object '%s'",
             qp_snap_type_name(r->rt, rec->obj, tbuf, sizeof(tbuf)));
    if (rec->kind == QP_CAPI_SNAP_REC_ATTR)
        return snap_error(r, "restoring C extension state failed: attribute %s of a C "
                          "namespace was not restored",
                          JS_AtomGetStr(ctx, (char[ATOM_GET_STR_BUF_SIZE]){0},
                                        ATOM_GET_STR_BUF_SIZE, rec->name));
    rec->state = 1;
    /* the reconstructor and its arguments first */
    for (i = 0; i < 2; i++) {
        if (qp_capi_snap_resolve_deps(r, ctx, cs, cs->recs[idx].v[i], 0, TRUE))
            return -1;
    }
    rec = &cs->recs[idx];
    args = rec->v[1];
    if (JS_VALUE_GET_TAG(args) != JS_TAG_OBJECT ||
        JS_VALUE_GET_OBJ(args)->class_id != JS_CLASS_PY_TUPLE)
        return snap_error(r, "invalid C extension object record");
    res = qp_call_tuple(ctx, rec->v[0], args, QP_NULL);
    if (JS_IsException(res))
        return qp_capi_snap_fail(r, ctx, what, JS_ATOM_NULL);
    if (JS_VALUE_GET_TAG(res) == JS_TAG_OBJECT) {
        /* a cache restored with the snapshot returned an object being
           restored: it is reconstructed first */
        SnapMapEntry *me = snap_map_find(&cs->rec_map, JS_VALUE_GET_OBJ(res));
        if (me && cs->recs[me->val - 1].state != 2) {
            if (JS_VALUE_GET_OBJ(res) == cs->recs[idx].obj) {
                JS_FreeValue(ctx, res);
                return snap_error(r, "restoring C extension state failed: %s: the "
                                  "reconstructor returned the object being restored "
                                  "(from a cache saved with it)", what);
            }
            if (qp_capi_snap_resolve(r, ctx, cs, &cs->recs[me->val - 1])) {
                JS_FreeValue(ctx, res);
                return -1;
            }
        }
    }
    if (qp_capi_snap_bind(ctx, rec->obj, res) < 0)
        return qp_capi_snap_fail(r, ctx, what, JS_ATOM_NULL);
    /* items and state: may reference objects being reconstructed */
    for (i = 2; i < 5; i++) {
        if (qp_capi_snap_resolve_deps(r, ctx, cs, cs->recs[idx].v[i], 0, FALSE))
            return -1;
    }
    rec = &cs->recs[idx];
    if (!JS_IsUndefined(rec->v[2]) || !JS_IsUndefined(rec->v[3]) || !JS_IsUndefined(rec->v[4])) {
        f = qp_capi_snap_helper(ctx, &cs->helpers, "build");
        if (JS_IsException(f))
            return qp_capi_snap_fail(r, ctx, what, JS_ATOM_NULL);
        rec = &cs->recs[idx];
        a[0] = JS_MKPTR(JS_TAG_OBJECT, rec->obj);
        a[1] = rec->v[2];
        a[2] = rec->v[3];
        a[3] = rec->v[4];
        a[4] = rec->v[5];
        res = qp_vectorcall(ctx, f, a, 5, QP_NULL);
        JS_FreeValue(ctx, f);
        if (JS_IsException(res))
            return qp_capi_snap_fail(r, ctx, what, JS_ATOM_NULL);
        JS_FreeValue(ctx, res);
    }
    cs->recs[idx].state = 2;
    return 0;
}

static int qp_capi_snap_mod_cmp(const void *a, const void *b)
{
    uint32_t x = ((const QPCapiSnapMod *)a)->seq, y = ((const QPCapiSnapMod *)b)->seq;
    return x < y ? -1 : x > y;
}

/* after the exec of module m: its saved functions and objects keep their
   identity in its namespace (`saved` is the namespace before the exec).
   A dict of the module state that the namespace shows (_csv._dialects:
   the registry of csv.register_dialect) is made again empty by the exec:
   the saved entries go into the new dict once the C objects they hold are
   reconstructed (qp_capi_snap_merge), as if registered again. */
static int qp_capi_snap_restore_ns(JSContext *ctx, QPCapiSnapRead *cs, JSValueConst dict,
                                   JSValueConst saved)
{
    Py_ssize_t pos = 0;
    JSValue k, old;

    while (qp_dict_next(saved, &pos, &k, &old)) {
        JSValue cur;
        int r;
        JSObject *p;
        if (JS_VALUE_GET_TAG(old) != JS_TAG_OBJECT)
            continue;
        p = JS_VALUE_GET_OBJ(old);
        if (p->class_id != JS_CLASS_PY_CFUNCTION && !qp_dict_check_exact(ctx, old))
            continue;
        r = qp_dict_get_item(ctx, dict, k, &cur);
        if (r < 0)
            return -1;
        if (r == 0)
            continue;
        if (p->class_id != JS_CLASS_PY_CFUNCTION) {
            JSValue pair;
            JSValueConst two[2];
            if (!qp_dict_check_exact(ctx, cur) || JS_VALUE_GET_OBJ(cur) == p ||
                qp_dict_size(old) == 0) {
                JS_FreeValue(ctx, cur);
                continue;
            }
            if (QP_IS_NULL(cs->merges)) {
                cs->merges = qp_list_new(ctx, 0);
                if (JS_IsException(cs->merges)) {
                    cs->merges = QP_NULL;
                    JS_FreeValue(ctx, cur);
                    return -1;
                }
            }
            two[0] = cur;
            two[1] = old;
            pair = qp_tuple_pack(ctx, 2, two);
            JS_FreeValue(ctx, cur);
            if (JS_IsException(pair))
                return -1;
            r = qp_list_append(ctx, cs->merges, pair);
            JS_FreeValue(ctx, pair);
            if (r < 0)
                return -1;
            continue;
        }
        if (JS_VALUE_GET_TAG(cur) == JS_TAG_OBJECT &&
            JS_VALUE_GET_OBJ(cur)->class_id == JS_CLASS_PY_CFUNCTION &&
            JS_VALUE_GET_OBJ(cur) != p) {
            QPCFunctionObject *fo = p->u.opaque, *fn = JS_VALUE_GET_OBJ(cur)->u.opaque;
            /* same C function: the saved object (its definition is
               resolved from this namespace) */
            if (fo && fn && (!fo->ml || fo->ml == fn->ml) &&
                qp_dict_set_item(ctx, dict, k, old) < 0) {
                JS_FreeValue(ctx, cur);
                return -1;
            }
        }
        JS_FreeValue(ctx, cur);
    }
    return 0;
}

/* the saved entries of the module state dicts (qp_capi_snap_restore_ns),
   keys hashed again in this process */
static int qp_capi_snap_merge(SnapReader *r, JSContext *ctx, QPCapiSnapRead *cs)
{
    JSValue *items;
    Py_ssize_t i, n;

    if (QP_IS_NULL(cs->merges))
        return 0;
    items = qp_list_items(cs->merges, &n);
    for (i = 0; i < n; i++) {
        JSValueConst d = QP_TUPLE_ITEMS(items[i])[0], saved = QP_TUPLE_ITEMS(items[i])[1];
        Py_ssize_t pos = 0;
        JSValue k, v;
        while (qp_dict_next(saved, &pos, &k, &v)) {
            if (qp_dict_set_item(ctx, d, k, v) < 0)
                return qp_capi_snap_fail(r, ctx, "C module state", JS_ATOM_NULL);
        }
    }
    return 0;
}

static int qp_capi_snap_finish(SnapReader *r, JSContext *ctx)
{
    QPSnapRead *s = r->py;
    QPCapiSnapRead *cs = s->capi;
    int i, j, ret = -1;

    if (!cs->mod_count && !cs->type_count && !cs->def_count && !cs->rec_count)
        return 0;
    if (qp_capi_snap_cur)
        return snap_error(r, "C extension state is being restored already");
    qp_capi_snap_cur = cs;
    if (qp_capi_init(ctx) < 0) {
        qp_capi_snap_fail(r, ctx, "C API", JS_ATOM_NULL);
        goto done;
    }
    /* C modules, in their execution order */
    qsort(cs->mods, cs->mod_count, sizeof(cs->mods[0]), qp_capi_snap_mod_cmp);
    for (i = 0; i < cs->mod_count; i++) {
        JSValue m = JS_MKPTR(JS_TAG_OBJECT, cs->mods[i].obj), saved, name = JS_UNDEFINED;
        JSValue dict = qp_module_get_dict(m);
        int rr;
        if (QP_IS_NULL(dict)) {
            snap_error(r, "C extension module without a namespace");
            goto done;
        }
        saved = qp_dict_copy(ctx, dict);
        if (JS_IsException(saved)) {
            qp_capi_snap_fail(r, ctx, "C module", JS_ATOM_NULL);
            goto done;
        }
        rr = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __name__), &name);
        cs->cur_module = rr > 0 ? qp_str_utf8(ctx, name, NULL) : NULL;
        if (!cs->cur_module) {
            JS_FreeValue(ctx, saved);
            JS_FreeValue(ctx, name);
            if (!JS_HasException(ctx))
                qp_raise_system_error(ctx, "nameless C module");
            qp_capi_snap_fail(r, ctx, "C module", JS_ATOM_NULL);
            goto done;
        }
        rr = qp_capi_module_exec_as(ctx, m, cs->mods[i].name);
        if (rr == 0) {
            if (qp_capi_snap_resolve_defs(r, ctx, cs, FALSE) ||
                qp_capi_snap_resolve_attrs(r, ctx, cs)) {
                rr = -2;
            } else {
                rr = qp_capi_snap_restore_ns(ctx, cs, dict, saved);
            }
        }
        JS_FreeValue(ctx, saved);
        if (rr == -1) {
            JSAtom a = JS_ValueToAtom(ctx, name);
            qp_capi_snap_fail(r, ctx, "C module", a);
            JS_FreeAtom(ctx, a);
        }
        JS_FreeCString(ctx, cs->cur_module);
        cs->cur_module = NULL;
        JS_FreeValue(ctx, name);
        if (rr < 0)
            goto done;
    }
    /* every type saved by reference was made again */
    for (i = 0; i < cs->type_count; i++) {
        if (cs->types[i].obj) {
            snap_error(r, "restoring C extension state failed: type '%s' was not created "
                       "again by its C module", cs->types[i].key + 2);
            goto done;
        }
    }
    if (qp_capi_snap_resolve_defs(r, ctx, cs, TRUE) || qp_capi_snap_resolve_attrs(r, ctx, cs))
        goto done;
    /* Python subclasses of the types made again */
    for (i = 0; i < r->cell_count; i++) {
        JSObject *p;
        QPType *t;
        QPTypeExt *e;
        if (r->cells[i].type != SNAP_CELL_OBJECT)
            continue;
        p = r->cells[i].ptr;
        if (p->class_id != JS_CLASS_PY_TYPE || !(t = p->u.opaque))
            continue;
        e = QP_TEXT(t);
        for (j = 0; j < e->nbases_raw; j++) {
            QPType *b = e->bases_raw[j];
            int k;
            if (!b)
                continue;
            for (k = 0; k < b->subclass_count && b->subclasses[k] != t; k++)
                continue;
            if (k == b->subclass_count && qp_type_add_subclass(ctx, b, t) < 0) {
                snap_error(r, "out of memory");
                goto done;
            }
        }
    }
    /* reduce records */
    for (i = 0; i < cs->rec_count; i++) {
        if (qp_capi_snap_resolve(r, ctx, cs, &cs->recs[i]))
            goto done;
    }
    if (qp_capi_snap_merge(r, ctx, cs))
        goto done;
    ret = 0;
done:
    qp_capi_snap_cur = NULL;
    return ret;
}
