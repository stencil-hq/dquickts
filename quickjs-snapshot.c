/*
 * QuickJS heap snapshots
 *
 * JS_WriteSnapshot() serializes the complete state of a runtime and
 * JS_ReadSnapshot() rebuilds it in a fresh runtime of the same engine
 * build, at arbitrary addresses and in any process. This file is included
 * at the end of quickjs.c because it needs the engine internals.
 *
 * Format (host byte order, like the bytecode writer):
 *
 *   header   magic, format version, kind (full or delta), base id,
 *            build signature, class table
 *   cells    one header per cell: what is needed to allocate it
 *   changed  (delta) indexes of the base cells whose body changed
 *   atoms    atoms that are not predefined, with their kind
 *   strings  string values (deduplicated by identity)
 *   roots    contexts, jobs, host roots, suspended execution
 *   bodies   one length-prefixed body per cell (full) or per changed
 *            and new cell (delta); a changed base cell can be a patch of
 *            its base body: the lengths of the prefix and suffix kept
 *            from the base body, then the bytes between them
 *
 * Cells (objects, contexts, closure variables, heap frames, modules,
 * function bytecode, shared promise resolution records) are encoded as
 * indexes. The reader allocates every cell from its header before reading
 * any body, so bodies may reference cells in any order and cycles need no
 * special handling. A body only contains values, cell indexes and table
 * indexes (atoms, strings, natives): it never defines another entity, so
 * it can be skipped or compared byte for byte.
 *
 * Incremental snapshots: a base (JS_SNAPSHOT_BASE) keeps, in the runtime,
 * its cells, atoms and strings with their indexes and the bytes of every
 * body (JSSnapshotBase). A delta numbers the base entities with their
 * base indexes and the new ones after them, so that the body of an
 * unmodified base cell is identical to its base body: the delta only
 * contains the bodies that differ and the new entities. The reader of a
 * delta reads the base, skipping the base bodies replaced by the delta,
 * then the delta.
 *
 * A base is reproducible: the same build with the same host setup
 * writes the same base bytes in every process, so a host may rebuild a
 * base instead of storing it. The state of Math.random() and the time
 * origin of performance.now() are therefore not part of the context
 * cells: full snapshots and deltas carry them in their roots (the elapsed
 * time, so that performance.now() continues from its value at the write),
 * and a base read without a delta gets a fresh seed and time origin.
 *
 * Code pointers stored in the heap (C functions, job functions, lazily
 * instantiated function list entries...) are encoded as indexes in a
 * registry built from the engine function lists and the host natives of
 * JSSnapshotHost. The registry content is hashed into the build signature
 * so that a snapshot is only accepted by a compatible engine and host.
 *
 * Weak references are written as normal references unless their target
 * is already dead. Objects that are only weakly reachable are then freed
 * by the restored runtime as soon as the reader drops its own references.
 */

static const uint8_t snap_magic[8] = { 'Q', 'J', 'S', 'S', 'N', 'A', 'P', '\n' };
#define SNAP_FORMAT_VERSION 8

typedef enum {
    SNAP_KIND_FULL,
    SNAP_KIND_DELTA,
} SnapKind;

typedef enum {
    SNAP_CELL_CONTEXT,
    SNAP_CELL_OBJECT,
    SNAP_CELL_VAR_REF,
    SNAP_CELL_FRAME, /* JSAsyncFunctionState */
    SNAP_CELL_MODULE,
    SNAP_CELL_BYTECODE, /* JSFunctionBytecode */
    SNAP_CELL_RESOLVED, /* JSPromiseFunctionDataResolved */
    SNAP_CELL_TYPE_COUNT,
} SnapCellType;

static const char * const snap_cell_type_names[SNAP_CELL_TYPE_COUNT] = {
    "context", "object", "variable reference", "frame", "module",
    "bytecode", "promise resolution",
};

typedef enum {
    SNAP_VAL_UNDEFINED,
    SNAP_VAL_NULL,
    SNAP_VAL_FALSE,
    SNAP_VAL_TRUE,
    SNAP_VAL_UNINITIALIZED,
    SNAP_VAL_INT32,
    SNAP_VAL_FLOAT64,
    SNAP_VAL_CATCH_OFFSET,
    SNAP_VAL_STRING,
    SNAP_VAL_SYMBOL,
    SNAP_VAL_BIG_INT,
    SNAP_VAL_OBJECT,
    SNAP_VAL_MODULE,
    SNAP_VAL_BYTECODE,
    SNAP_VAL_DEAD,         /* weak reference to a collected target */
} SnapValueTag;

typedef enum {
    SNAP_ATOM_STRING,
    SNAP_ATOM_GLOBAL_SYMBOL,
    SNAP_ATOM_SYMBOL,
    SNAP_ATOM_SYMBOL_NO_DESC, /* Symbol() */
    SNAP_ATOM_PRIVATE,
} SnapAtomKind;

/* JSObject flags restored once every body is read (add_property() may
   update some of them while the heap is rebuilt) */
#define SNAP_OBJ_EXTENSIBLE      (1 << 0)
#define SNAP_OBJ_STD_ARRAY_PROTO (1 << 1)
#define SNAP_OBJ_IMMUTABLE_PROTO (1 << 2)
#define SNAP_OBJ_CONSTRUCTOR     (1 << 3)
#define SNAP_OBJ_HTMLDDA         (1 << 4)
#define SNAP_OBJ_FAST_ARRAY      (1 << 5)

typedef struct SnapWriter SnapWriter;
typedef struct SnapReader SnapReader;
typedef struct SnapNatives SnapNatives;

/* Class families (quickjs-snapshot-data.c, quickjs-snapshot-control.c):
   snap_write_<family>_payload() / snap_read_<family>_payload() return 1
   when the class does not belong to the family, 0 on success and -1 on
   error. snap_add_<family>_natives() registers the code pointers the
   family stores in the heap. */
static int snap_add_data_natives(JSRuntime *rt, SnapNatives *n);
static int snap_write_data_payload(SnapWriter *w, JSObject *p);
static int snap_read_data_payload(SnapReader *r, JSObject *p);
static int snap_add_control_natives(JSRuntime *rt, SnapNatives *n);
static int snap_write_control_payload(SnapWriter *w, JSObject *p);
static int snap_read_control_payload(SnapReader *r, JSObject *p);
/* module records (quickjs-snapshot-control.c) */
static int snap_write_module(SnapWriter *w, JSModuleDef *m);
static int snap_read_module(SnapReader *r, JSModuleDef *m);
static JSModuleDef *snap_new_module(SnapReader *r);
/* 'already resolved' records shared by a pair of promise resolving
   functions (quickjs-snapshot-control.c) */
static int snap_write_resolved(SnapWriter *w, JSPromiseFunctionDataResolved *sr);
static int snap_read_resolved(SnapReader *r, JSPromiseFunctionDataResolved *sr);
static JSPromiseFunctionDataResolved *snap_new_resolved(SnapReader *r);

/* realloc an array with the runtime allocator (no context is available
   while a snapshot header is read) */
static int snap_resize(JSRuntime *rt, void **parray, size_t elem_size,
                       int *psize, int req_size)
{
    int new_size;
    void *new_array;

    if (req_size <= *psize)
        return 0;
    new_size = max_int(req_size, *psize * 3 / 2);
    new_size = max_int(new_size, 16);
    new_array = js_realloc_rt(rt, *parray, new_size * elem_size);
    if (!new_array)
        return -1;
    *parray = new_array;
    *psize = new_size;
    return 0;
}

/* pointer -> index map (open addressing, NULL keys are not allowed) */
typedef struct {
    const void *key;
    uint32_t val;
} SnapMapEntry;

typedef struct {
    SnapMapEntry *tab; /* NULL when empty */
    uint32_t mask;
    uint32_t count;
} SnapMap;

static inline uint32_t snap_ptr_hash(const void *p)
{
    uint64_t h = (uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

static SnapMapEntry *snap_map_find(const SnapMap *m, const void *key)
{
    uint32_t i;
    if (!m->tab)
        return NULL;
    for(i = snap_ptr_hash(key) & m->mask; m->tab[i].key; i = (i + 1) & m->mask) {
        if (m->tab[i].key == key)
            return &m->tab[i];
    }
    return NULL;
}

static void snap_map_insert(SnapMapEntry *tab, uint32_t mask,
                            const void *key, uint32_t val)
{
    uint32_t i;
    for(i = snap_ptr_hash(key) & mask; tab[i].key; i = (i + 1) & mask)
        continue;
    tab[i].key = key;
    tab[i].val = val;
}

/* 'key' must not be present */
static int snap_map_add(JSRuntime *rt, SnapMap *m, const void *key, uint32_t val)
{
    uint32_t size = m->tab ? m->mask + 1 : 0;
    if ((m->count + 1) * 2 > size) {
        uint32_t i, new_size = size ? size * 2 : 256;
        SnapMapEntry *tab = js_mallocz_rt(rt, sizeof(tab[0]) * new_size);
        if (!tab)
            return -1;
        for(i = 0; i < size; i++) {
            if (m->tab[i].key)
                snap_map_insert(tab, new_size - 1, m->tab[i].key, m->tab[i].val);
        }
        js_free_rt(rt, m->tab);
        m->tab = tab;
        m->mask = new_size - 1;
    }
    snap_map_insert(m->tab, m->mask, key, val);
    m->count++;
    return 0;
}

static void snap_map_free(JSRuntime *rt, SnapMap *m)
{
    js_free_rt(rt, m->tab);
    m->tab = NULL;
    m->count = 0;
}

/*******************************************************************/
/* native code registry */

/* function lists of the engine. Lazily instantiated properties keep a
   pointer to their entry, and the C functions created from them keep the
   entry's function pointers. */
static const struct {
    const JSCFunctionListEntry *tab;
    int len;
} snap_builtin_lists[] = {
#define DEF(tab) { tab, countof(tab) }
    DEF(js_object_funcs),
    DEF(js_object_proto_funcs),
    DEF(js_function_proto_funcs),
    DEF(js_error_proto_funcs),
    DEF(js_native_error_proto_funcs),
    DEF(js_error_funcs),
    DEF(js_callsite_proto_funcs),
    DEF(js_array_funcs),
    DEF(js_iterator_wrap_proto_funcs),
    DEF(js_iterator_funcs),
    DEF(js_iterator_proto_funcs),
    DEF(js_iterator_helper_proto_funcs),
    DEF(js_array_unscopables_funcs),
    DEF(js_array_proto_funcs),
    DEF(js_array_iterator_proto_funcs),
    DEF(js_number_funcs),
    DEF(js_number_proto_funcs),
    DEF(js_boolean_proto_funcs),
    DEF(js_string_funcs),
    DEF(js_string_proto_funcs),
    DEF(js_string_iterator_proto_funcs),
    DEF(js_string_proto_normalize),
    DEF(js_math_funcs),
    DEF(js_math_obj),
    DEF(js_regexp_funcs),
    DEF(js_regexp_proto_funcs),
    DEF(js_regexp_string_iterator_proto_funcs),
    DEF(js_json_funcs),
    DEF(js_json_obj),
    DEF(js_reflect_funcs),
    DEF(js_reflect_obj),
    DEF(js_proxy_funcs),
    DEF(js_symbol_proto_funcs),
    DEF(js_symbol_funcs),
    DEF(js_map_funcs),
    DEF(js_map_proto_funcs),
    DEF(js_map_iterator_proto_funcs),
    DEF(js_set_proto_funcs),
    DEF(js_set_iterator_proto_funcs),
    DEF(js_weak_map_proto_funcs),
    DEF(js_weak_set_proto_funcs),
    DEF(js_generator_function_proto_funcs),
    DEF(js_generator_proto_funcs),
    DEF(js_promise_funcs),
    DEF(js_promise_proto_funcs),
    DEF(js_async_function_proto_funcs),
    DEF(js_async_iterator_proto_funcs),
    DEF(js_disposable_stack_proto_funcs),
    DEF(js_async_disposable_stack_proto_funcs),
    DEF(js_async_from_sync_iterator_proto_funcs),
    DEF(js_async_generator_function_proto_funcs),
    DEF(js_async_generator_proto_funcs),
    DEF(js_global_funcs),
    DEF(js_performance_funcs),
    DEF(js_performance_obj),
    DEF(js_date_funcs),
    DEF(js_date_proto_funcs),
    DEF(js_bigint_funcs),
    DEF(js_bigint_proto_funcs),
    DEF(js_array_buffer_funcs),
    DEF(js_array_buffer_proto_funcs),
    DEF(js_shared_array_buffer_funcs),
    DEF(js_shared_array_buffer_proto_funcs),
    DEF(js_typed_array_base_funcs),
    DEF(js_typed_array_base_proto_funcs),
    DEF(js_typed_array_funcs),
    DEF(js_uint8array_proto_funcs),
    DEF(js_uint8array_funcs),
    DEF(js_dataview_proto_funcs),
#ifdef CONFIG_ATOMICS
    DEF(js_atomics_funcs),
    DEF(js_atomics_obj),
#endif
    DEF(js_weakref_proto_funcs),
    DEF(js_finrec_proto_funcs),
    DEF(js_domexception_proto_funcs),
    DEF(js_base64_funcs),
#undef DEF
};

/* engine code pointers stored in the heap that do not come from a
   function list */
static const void * const snap_builtin_natives[] = {
    /* constructors */
    js_object_constructor,
    js_function_constructor,
    js_function_proto,
    js_throw_type_error,
    js_error_constructor,
    js_array_constructor,
    js_iterator_constructor,
    js_iterator_constructor_getset,
    js_number_constructor,
    js_boolean_constructor,
    js_string_constructor,
    js_symbol_constructor,
    js_regexp_constructor,
    js_proxy_constructor,
    js_map_constructor,
    js_promise_constructor,
    js_date_constructor,
    js_bigint_constructor,
    js_array_buffer_constructor,
    js_shared_array_buffer_constructor,
    js_typed_array_base_constructor,
    js_typed_array_constructor,
    js_dataview_constructor,
    js_weakref_constructor,
    js_finrec_constructor,
    js_domexception_constructor,
    js_disposable_stack_constructor,
    /* functions created at run time */
    js_create_array_iterator,
    js_array_iterator_next,
    js_proxy_revoke,
    js_promise_executor,
    js_promise_all_resolve_element,
    js_promise_allKeyed_resolve_element,
    js_promise_finally_value_thunk,
    js_promise_finally_thrower,
    js_promise_then_finally_func,
    js_async_from_sync_iterator_unwrap,
    js_async_from_sync_iterator_close_wrap,
    js_async_generator_resolve_function,
    js_load_module_fulfilled,
    js_load_module_rejected,
    js_async_module_execution_fulfilled,
    js_async_module_execution_rejected,
    array_from_async_handler,
    js_decorator_add_initializer,
    js_decorator_access,
    js_decorator_param,
    js_dispose_sync_method,
    js_async_disposable_stack_handler,
    js_async_iterator_dispose_unwrap,
    /* job functions */
    promise_reaction_job,
    js_promise_resolve_thenable_job,
    js_dynamic_import_job,
    js_microtask_job,
};

struct SnapNatives {
    const void **tab;
    int count;
    int size;
    SnapMap map; /* pointer -> index (writer only) */
    uint32_t signature;
};

static void snap_hash(uint32_t *h, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--) {
        *h ^= *p++;
        *h *= 16777619;
    }
}

static void snap_hash_u32(uint32_t *h, uint32_t v)
{
    snap_hash(h, &v, sizeof(v));
}

static void snap_hash_str(uint32_t *h, const char *str)
{
    if (str)
        snap_hash(h, str, strlen(str) + 1);
    else
        snap_hash_u32(h, 0);
}

static int snap_natives_push(JSRuntime *rt, SnapNatives *n, const void *ptr)
{
    if (snap_resize(rt, (void **)&n->tab, sizeof(n->tab[0]), &n->size, n->count + 1))
        return -1;
    n->tab[n->count++] = ptr;
    return 0;
}

static int snap_natives_add_list(JSRuntime *rt, SnapNatives *n,
                                 const JSCFunctionListEntry *tab, int len)
{
    int i;
    for(i = 0; i < len; i++) {
        const JSCFunctionListEntry *e = &tab[i];
        snap_hash_str(&n->signature, e->name);
        snap_hash_u32(&n->signature, e->def_type | ((uint32_t)(uint16_t)e->magic << 8));
        if (snap_natives_push(rt, n, e))
            return -1;
        switch(e->def_type) {
        case JS_DEF_CFUNC:
            if (snap_natives_push(rt, n, e->u.func.cfunc.generic))
                return -1;
            break;
        case JS_DEF_CGETSET:
        case JS_DEF_CGETSET_MAGIC:
            if (snap_natives_push(rt, n, e->u.getset.get.generic) ||
                snap_natives_push(rt, n, e->u.getset.set.generic))
                return -1;
            break;
        case JS_DEF_OBJECT:
            if (snap_natives_add_list(rt, n, e->u.prop_list.tab, e->u.prop_list.len))
                return -1;
            break;
        default:
            break;
        }
    }
    return 0;
}

static void snap_natives_free(JSRuntime *rt, SnapNatives *n)
{
    js_free_rt(rt, n->tab);
    n->tab = NULL;
    n->count = n->size = 0;
    snap_map_free(rt, &n->map);
}

/* the registry (and its signature) only depends on the engine build and
   on 'host', so the writer and the reader build the same indexes */
static int snap_natives_init(JSRuntime *rt, SnapNatives *n,
                             const JSSnapshotHost *host, BOOL with_map)
{
    int i;

    memset(n, 0, sizeof(*n));
    n->signature = 2166136261u;
    snap_hash_str(&n->signature, CONFIG_VERSION);
    snap_hash_u32(&n->signature, SNAP_FORMAT_VERSION);
    snap_hash_u32(&n->signature, BC_VERSION);
    snap_hash_u32(&n->signature, JS_ATOM_END);
    snap_hash_u32(&n->signature, JS_CLASS_INIT_COUNT);
    snap_hash_u32(&n->signature, sizeof(JSValue));
    snap_hash_u32(&n->signature, JS_LIMB_BITS);
    for(i = 0; i < countof(snap_builtin_lists); i++) {
        if (snap_natives_add_list(rt, n, snap_builtin_lists[i].tab,
                                  snap_builtin_lists[i].len))
            goto fail;
    }
    snap_hash_u32(&n->signature, countof(snap_builtin_natives));
    for(i = 0; i < countof(snap_builtin_natives); i++) {
        if (snap_natives_push(rt, n, snap_builtin_natives[i]))
            goto fail;
    }
    if (snap_add_data_natives(rt, n) || snap_add_control_natives(rt, n))
        goto fail;
    snap_hash_u32(&n->signature, n->count);
    if (host) {
        snap_hash_u32(&n->signature, host->native_count);
        for(i = 0; i < host->native_count; i++) {
            if (snap_natives_push(rt, n, host->natives[i]))
                goto fail;
        }
        snap_hash_u32(&n->signature, host->func_list_count);
        for(i = 0; i < host->func_list_count; i++) {
            if (snap_natives_add_list(rt, n, host->func_lists[i],
                                      host->func_list_lens[i]))
                goto fail;
        }
    }
    if (with_map) {
        for(i = 0; i < n->count; i++) {
            if (n->tab[i] && !snap_map_find(&n->map, n->tab[i])) {
                if (snap_map_add(rt, &n->map, n->tab[i], i))
                    goto fail;
            }
        }
    }
    return 0;
 fail:
    snap_natives_free(rt, n);
    return -1;
}

/*******************************************************************/
/* cells and base of the incremental snapshots */

typedef struct {
    void *ptr;
    SnapCellType type;
} SnapCell;

static void snap_cell_dup(SnapCell *c)
{
    if (c->type == SNAP_CELL_RESOLVED)
        ((JSPromiseFunctionDataResolved *)c->ptr)->ref_count++;
    else
        js_rc(c->ptr)->ref_count++;
}

static void snap_cell_free(JSRuntime *rt, SnapCell *c)
{
    switch(c->type) {
    case SNAP_CELL_CONTEXT:
        JS_FreeContext(c->ptr);
        break;
    case SNAP_CELL_OBJECT:
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, c->ptr));
        break;
    case SNAP_CELL_VAR_REF:
        free_var_ref(rt, c->ptr);
        break;
    case SNAP_CELL_FRAME:
        async_func_free(rt, c->ptr);
        break;
    case SNAP_CELL_MODULE:
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_MODULE, c->ptr));
        break;
    case SNAP_CELL_BYTECODE:
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, c->ptr));
        break;
    case SNAP_CELL_RESOLVED:
        js_promise_resolve_function_free_resolved(rt, c->ptr);
        break;
    default:
        abort();
    }
}

/* Base of the incremental snapshots, attached to the runtime. Its
   cells, atoms and strings keep the indexes they have in the base
   snapshot, and it holds a reference to each of them: a cell of the base
   cannot be freed and its address reused by another cell. */
typedef struct JSSnapshotBase {
    uint64_t id; /* identifies the base snapshot in its deltas */
    SnapCell *cells;
    int cell_count;
    SnapMap cell_map; /* cell -> index */
    uint8_t *bodies; /* the base body of every cell, concatenated */
    uint32_t *body_offsets; /* cell_count + 1 offsets in 'bodies' */
    JSAtom *atoms; /* atom table */
    int atom_count;
    JSValue *strs; /* string table: the string or rope values of the heap */
    int str_count;
    SnapMap str_map; /* string or rope -> index */
} JSSnapshotBase;

void JS_DropSnapshotBase(JSRuntime *rt)
{
    JSSnapshotBase *base = rt->snapshot_base;
    int i;

    if (!base)
        return;
    rt->snapshot_base = NULL;
    for(i = 0; i < base->cell_count; i++)
        snap_cell_free(rt, &base->cells[i]);
    for(i = 0; i < base->atom_count; i++)
        JS_FreeAtomRT(rt, base->atoms[i]);
    for(i = 0; i < base->str_count; i++)
        JS_FreeValueRT(rt, base->strs[i]);
    js_free_rt(rt, base->cells);
    js_free_rt(rt, base->bodies);
    js_free_rt(rt, base->body_offsets);
    js_free_rt(rt, base->atoms);
    js_free_rt(rt, base->strs);
    snap_map_free(rt, &base->cell_map);
    snap_map_free(rt, &base->str_map);
    js_free_rt(rt, base);
}

/*******************************************************************/
/* writer */

typedef struct {
    JSValue key; /* the string or rope value referenced by the heap */
    JSString *str; /* its flattened content */
} SnapString;

struct SnapWriter {
    BCWriterState bc; /* must be first: current output and atom table */
    JSContext *ctx;
    JSRuntime *rt;
    const JSSnapshotHost *host;
    SnapNatives natives;
    int flags; /* JS_SNAPSHOT_x */
    /* JS_SNAPSHOT_DELTA: the entities of the base keep their indexes,
       the new ones are numbered after them */
    JSSnapshotBase *base;
    int base_cell_count;
    int base_str_count;
    uint8_t *base_queued; /* base cells already queued */
    /* cells that are not in the base (index base_cell_count + i). The
       writer holds a reference to each: a GC triggered while writing
       (e.g. by a host save_opaque() hook) cannot free them. */
    SnapMap cell_map;
    SnapCell *cells;
    int cell_count;
    int cell_size;
    /* cells whose body must be written, by index */
    int *queue;
    int queue_count;
    int queue_size;
    /* strings that are not in the base (index base_str_count + i), the
       writer holds a reference to their key and content */
    SnapMap str_map; /* key -> index - base_str_count */
    SnapString *strs;
    int str_count;
    int str_size;
    JSValue *keep; /* values created during the write, kept alive until the end */
    int keep_count;
    int keep_size;
    DynBuf scratch; /* body being written */
    DynBuf bodies; /* the bodies section */
    int body_count;
    int *changed; /* JS_SNAPSHOT_DELTA: base cells with a new body */
    int changed_count;
    int changed_size;
    /* JS_SNAPSHOT_BASE: bodies of the cells without length prefix */
    DynBuf base_bodies;
    uint32_t *base_offsets;
    int base_offsets_size;
};

static int snap_put_value(SnapWriter *w, JSValueConst val);

static int snap_writer_oom(SnapWriter *w)
{
    JS_ThrowOutOfMemory(w->ctx);
    return -1;
}

static SnapCell *snap_writer_cell(SnapWriter *w, int idx)
{
    if (idx < w->base_cell_count)
        return &w->base->cells[idx];
    return &w->cells[idx - w->base_cell_count];
}

static int snap_queue(SnapWriter *w, int idx)
{
    if (snap_resize(w->rt, (void **)&w->queue, sizeof(w->queue[0]),
                    &w->queue_size, w->queue_count + 1))
        return snap_writer_oom(w);
    w->queue[w->queue_count++] = idx;
    return 0;
}

/* write a reference to a cell (0 = NULL) and queue the cell if it was
   not referenced before */
static int snap_put_ref(SnapWriter *w, SnapCellType type, void *ptr)
{
    SnapMapEntry *e;
    int idx;

    if (!ptr) {
        bc_put_leb128(&w->bc, 0);
        return 0;
    }
    if (w->base && (e = snap_map_find(&w->base->cell_map, ptr))) {
        idx = e->val;
        if (!w->base_queued[idx]) {
            w->base_queued[idx] = TRUE;
            if (snap_queue(w, idx))
                return -1;
        }
    } else if ((e = snap_map_find(&w->cell_map, ptr))) {
        idx = w->base_cell_count + e->val;
    } else {
        SnapCell *c;
        if (snap_resize(w->rt, (void **)&w->cells, sizeof(w->cells[0]),
                        &w->cell_size, w->cell_count + 1) ||
            snap_map_add(w->rt, &w->cell_map, ptr, w->cell_count))
            return snap_writer_oom(w);
        c = &w->cells[w->cell_count++];
        c->ptr = ptr;
        c->type = type;
        snap_cell_dup(c);
        idx = w->base_cell_count + w->cell_count - 1;
        if (snap_queue(w, idx))
            return -1;
    }
    if (snap_writer_cell(w, idx)->type != type) {
        JS_ThrowInternalError(w->ctx, "snapshot: inconsistent %s reference",
                              snap_cell_type_names[type]);
        return -1;
    }
    bc_put_leb128(&w->bc, idx + 1);
    return 0;
}

static int snap_put_obj(SnapWriter *w, JSObject *p)
{
    return snap_put_ref(w, SNAP_CELL_OBJECT, p);
}

/* write a code pointer (0 = NULL) */
static int snap_put_native(SnapWriter *w, const void *ptr)
{
    SnapMapEntry *e;

    if (!ptr) {
        bc_put_leb128(&w->bc, 0);
        return 0;
    }
    e = snap_map_find(&w->natives.map, ptr);
    if (!e) {
        JS_ThrowTypeError(w->ctx, "snapshot: unregistered native code pointer %p", ptr);
        return -1;
    }
    bc_put_leb128(&w->bc, e->val + 1);
    return 0;
}

static int snap_put_string(SnapWriter *w, JSValueConst val)
{
    void *key = JS_VALUE_GET_PTR(val);
    SnapMapEntry *e;
    int idx;

    if (w->base && (e = snap_map_find(&w->base->str_map, key))) {
        idx = e->val;
    } else if ((e = snap_map_find(&w->str_map, key))) {
        idx = w->base_str_count + e->val;
    } else {
        JSValue str;
        if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING_ROPE) {
            str = JS_ToString(w->ctx, val); /* flatten */
            if (JS_IsException(str))
                return -1;
        } else {
            str = JS_DupValue(w->ctx, val);
        }
        if (snap_resize(w->rt, (void **)&w->strs, sizeof(w->strs[0]),
                        &w->str_size, w->str_count + 1) ||
            snap_map_add(w->rt, &w->str_map, key, w->str_count)) {
            JS_FreeValue(w->ctx, str);
            return snap_writer_oom(w);
        }
        w->strs[w->str_count].key = JS_DupValue(w->ctx, val);
        w->strs[w->str_count].str = JS_VALUE_GET_STRING(str);
        idx = w->base_str_count + w->str_count++;
    }
    bc_put_u8(&w->bc, SNAP_VAL_STRING);
    bc_put_leb128(&w->bc, idx);
    return 0;
}

static int snap_put_bytecode(SnapWriter *w, JSFunctionBytecode *b)
{
    bc_put_u8(&w->bc, SNAP_VAL_BYTECODE);
    return snap_put_ref(w, SNAP_CELL_BYTECODE, b);
}

/* body of a bytecode cell: the bytecode writer output (the constant pool
   goes through snap_put_value_hook()), then the realm */
static int snap_write_bytecode(SnapWriter *w, JSFunctionBytecode *b)
{
    if (JS_WriteFunctionTag(&w->bc, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b)))
        return -1;
    return snap_put_ref(w, SNAP_CELL_CONTEXT, b->realm);
}

static int snap_put_value(SnapWriter *w, JSValueConst val)
{
    BCWriterState *s = &w->bc;

    switch(JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_UNDEFINED:
        bc_put_u8(s, SNAP_VAL_UNDEFINED);
        break;
    case JS_TAG_NULL:
        bc_put_u8(s, SNAP_VAL_NULL);
        break;
    case JS_TAG_BOOL:
        bc_put_u8(s, JS_VALUE_GET_BOOL(val) ? SNAP_VAL_TRUE : SNAP_VAL_FALSE);
        break;
    case JS_TAG_UNINITIALIZED:
        bc_put_u8(s, SNAP_VAL_UNINITIALIZED);
        break;
    case JS_TAG_INT:
        bc_put_u8(s, SNAP_VAL_INT32);
        bc_put_sleb128(s, JS_VALUE_GET_INT(val));
        break;
    case JS_TAG_FLOAT64:
        {
            JSFloat64Union u;
            u.d = JS_VALUE_GET_FLOAT64(val);
            bc_put_u8(s, SNAP_VAL_FLOAT64);
            bc_put_u64(s, u.u64);
        }
        break;
    case JS_TAG_CATCH_OFFSET:
        bc_put_u8(s, SNAP_VAL_CATCH_OFFSET);
        bc_put_sleb128(s, JS_VALUE_GET_INT(val));
        break;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return snap_put_string(w, val);
    case JS_TAG_SYMBOL:
        bc_put_u8(s, SNAP_VAL_SYMBOL);
        return bc_put_atom(s, js_get_atom_index(w->rt, JS_VALUE_GET_PTR(val)));
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        bc_put_u8(s, SNAP_VAL_BIG_INT);
        return JS_WriteBigInt(s, val);
    case JS_TAG_OBJECT:
        bc_put_u8(s, SNAP_VAL_OBJECT);
        return snap_put_obj(w, JS_VALUE_GET_OBJ(val));
    case JS_TAG_MODULE:
        bc_put_u8(s, SNAP_VAL_MODULE);
        return snap_put_ref(w, SNAP_CELL_MODULE, JS_VALUE_GET_PTR(val));
    case JS_TAG_FUNCTION_BYTECODE:
        return snap_put_bytecode(w, JS_VALUE_GET_PTR(val));
    default:
        JS_ThrowInternalError(w->ctx, "snapshot: unsupported value tag %d",
                              JS_VALUE_GET_TAG(val));
        return -1;
    }
    return 0;
}

static int snap_put_value_hook(BCWriterState *s, JSValueConst val)
{
    return snap_put_value((SnapWriter *)s, val);
}

/* weak reference: written as a normal value unless the target is dead */
static int snap_put_weak_value(SnapWriter *w, JSValueConst val)
{
    if (js_weakref_is_target(val) && !js_weakref_is_live(val)) {
        bc_put_u8(&w->bc, SNAP_VAL_DEAD);
        return 0;
    }
    return snap_put_value(w, val);
}

/* keep 'val' alive until the end of the write (its cells are referenced
   by address). 'val' is freed on error. */
static int snap_keep(SnapWriter *w, JSValue val)
{
    if (snap_resize(w->rt, (void **)&w->keep, sizeof(w->keep[0]),
                    &w->keep_size, w->keep_count + 1)) {
        JS_FreeValue(w->ctx, val);
        return snap_writer_oom(w);
    }
    w->keep[w->keep_count++] = val;
    return 0;
}

/* shapes cached in a context: prototype and properties */
static int snap_put_shape(SnapWriter *w, JSShape *sh)
{
    JSShapeProperty *prs;
    int i, n;

    if (!sh) {
        bc_put_u8(&w->bc, 0);
        return 0;
    }
    bc_put_u8(&w->bc, 1);
    if (snap_put_obj(w, sh->proto))
        return -1;
    n = 0;
    for(i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        if (prs->atom != JS_ATOM_NULL)
            n++;
    }
    bc_put_leb128(&w->bc, n);
    for(i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        if (prs->atom != JS_ATOM_NULL) {
            if (bc_put_atom(&w->bc, prs->atom))
                return -1;
            bc_put_u8(&w->bc, prs->flags);
        }
    }
    return 0;
}

static int snap_write_context(SnapWriter *w, JSContext *ctx)
{
    struct list_head *el;
    int i, n;

    bc_put_leb128(&w->bc, w->rt->class_count);
    for(i = 0; i < w->rt->class_count; i++) {
        if (snap_put_value(w, ctx->class_proto[i]))
            return -1;
    }
    if (snap_put_value(w, ctx->function_proto) ||
        snap_put_value(w, ctx->function_ctor) ||
        snap_put_value(w, ctx->array_ctor) ||
        snap_put_value(w, ctx->regexp_ctor) ||
        snap_put_value(w, ctx->promise_ctor) ||
        snap_put_value(w, ctx->error_ctor))
        return -1;
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        if (snap_put_value(w, ctx->native_error_proto[i]))
            return -1;
    }
    if (snap_put_value(w, ctx->iterator_ctor) ||
        snap_put_value(w, ctx->async_iterator_proto) ||
        snap_put_value(w, ctx->array_proto_values) ||
        snap_put_value(w, ctx->throw_type_error) ||
        snap_put_value(w, ctx->eval_obj))
        return -1;
    for(i = 0; i < JS_CONTEXT_VALUE_COUNT; i++) {
        if (snap_put_value(w, ctx->user_values[i]))
            return -1;
    }
    if (snap_put_value(w, ctx->global_obj) ||
        snap_put_value(w, ctx->global_var_obj))
        return -1;
    bc_put_leb128(&w->bc, ctx->binary_object_count);
    bc_put_leb128(&w->bc, ctx->binary_object_size);
    bc_put_u8(&w->bc, ctx->compile_regexp != NULL);
    bc_put_u8(&w->bc, ctx->eval_internal != NULL);
    if (snap_put_shape(w, ctx->array_shape) ||
        snap_put_shape(w, ctx->arguments_shape) ||
        snap_put_shape(w, ctx->mapped_arguments_shape) ||
        snap_put_shape(w, ctx->regexp_shape) ||
        snap_put_shape(w, ctx->regexp_result_shape))
        return -1;
    n = 0;
    list_for_each(el, &ctx->loaded_modules)
        n++;
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        if (snap_put_ref(w, SNAP_CELL_MODULE, m))
            return -1;
    }
    return 0;
}

static int snap_write_var_ref(SnapWriter *w, JSVarRef *var_ref)
{
    bc_put_u8(&w->bc, var_ref->is_detached);
    bc_put_u8(&w->bc, var_ref->is_lexical);
    bc_put_u8(&w->bc, var_ref->is_const);
    if (var_ref->is_detached) {
        return snap_put_value(w, var_ref->value);
    } else {
        JSStackFrame *sf = var_ref->stack_frame;
        JSAsyncFunctionState *s;
        BOOL is_arg;
        uint32_t idx;

        if (!(sf->js_mode & JS_MODE_ASYNC)) {
            JS_ThrowTypeError(w->ctx, "snapshot: variable of a running function");
            return -1;
        }
        s = container_of(sf, JSAsyncFunctionState, frame);
        is_arg = (var_ref->pvalue >= sf->arg_buf &&
                  var_ref->pvalue < sf->arg_buf + sf->arg_count);
        idx = var_ref->pvalue - (is_arg ? sf->arg_buf : sf->var_buf);
        if (snap_put_ref(w, SNAP_CELL_FRAME, s))
            return -1;
        bc_put_leb128(&w->bc, var_ref->var_ref_idx);
        bc_put_u8(&w->bc, is_arg);
        bc_put_leb128(&w->bc, idx);
    }
    return 0;
}

static BOOL snap_is_bytecode_function(JSClassID class_id)
{
    return class_id == JS_CLASS_BYTECODE_FUNCTION ||
        class_id == JS_CLASS_GENERATOR_FUNCTION ||
        class_id == JS_CLASS_ASYNC_FUNCTION ||
        class_id == JS_CLASS_ASYNC_GENERATOR_FUNCTION;
}

static JSFunctionBytecode *snap_frame_bytecode(JSStackFrame *sf)
{
    return JS_VALUE_GET_OBJ(sf->cur_func)->u.func.function_bytecode;
}

static int snap_write_frame(SnapWriter *w, JSAsyncFunctionState *s)
{
    JSStackFrame *sf = &s->frame;
    JSValue *sp;

    bc_put_u8(&w->bc, s->is_completed);
    bc_put_u8(&w->bc, s->throw_flag);
    if (snap_put_value(w, s->resolving_funcs[0]) ||
        snap_put_value(w, s->resolving_funcs[1]))
        return -1;
    if (!s->is_completed) {
        JSFunctionBytecode *b = snap_frame_bytecode(sf);
        if (!sf->cur_sp) {
            JS_ThrowTypeError(w->ctx, "snapshot: running async function or generator");
            return -1;
        }
        /* the bytecode is written explicitly: the body of the function
           object may be read after the frame */
        if (snap_put_bytecode(w, b) ||
            snap_put_value(w, sf->cur_func) ||
            snap_put_value(w, s->this_val))
            return -1;
        bc_put_leb128(&w->bc, s->argc);
        bc_put_leb128(&w->bc, sf->js_mode);
        bc_put_leb128(&w->bc, sf->cur_pc - b->byte_code_buf);
        bc_put_leb128(&w->bc, sf->cur_sp - sf->arg_buf);
        for(sp = sf->arg_buf; sp < sf->cur_sp; sp++) {
            if (snap_put_value(w, *sp))
                return -1;
        }
        /* suspended execution (JS_IsSuspended()) */
        bc_put_u8(&w->bc, s->is_suspended_frame);
        if (snap_put_value(w, s->new_target) ||
            snap_put_ref(w, SNAP_CELL_FRAME, s->suspended_child))
            return -1;
        bc_put_u8(&w->bc, s->pending_native);
        bc_put_u8(&w->bc, s->pending_opcode);
        bc_put_leb128(&w->bc, s->pending_argc);
    }
    return 0;
}

static int snap_write_fast_array(SnapWriter *w, JSObject *p)
{
    uint32_t i;
    bc_put_leb128(&w->bc, p->u.array.count);
    for(i = 0; i < p->u.array.count; i++) {
        if (snap_put_value(w, p->u.array.u.values[i]))
            return -1;
    }
    return 0;
}

static int snap_write_payload(SnapWriter *w, JSObject *p)
{
    JSContext *ctx = w->ctx;
    uint32_t i;

    switch(p->class_id) {
    case JS_CLASS_OBJECT:
    case JS_CLASS_MODULE_NS:
    case JS_CLASS_ITERATOR:
    case JS_CLASS_RAWJSON:
        break;
    case JS_CLASS_ARRAY:
    case JS_CLASS_ARGUMENTS:
        if (p->fast_array)
            return snap_write_fast_array(w, p);
        break;
    case JS_CLASS_MAPPED_ARGUMENTS:
        if (p->fast_array) {
            bc_put_leb128(&w->bc, p->u.array.count);
            for(i = 0; i < p->u.array.count; i++) {
                if (snap_put_ref(w, SNAP_CELL_VAR_REF, p->u.array.u.var_refs[i]))
                    return -1;
            }
        }
        break;
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_DATE:
    case JS_CLASS_BIG_INT:
        return snap_put_value(w, p->u.object_data);
    case JS_CLASS_ERROR: /* stack trace, uninitialized if not captured */
        if (p->u.error.backtrace) {
            /* stack trace not formatted yet: the string is written
               without modifying the heap (the string is kept by
               snap_put_string()). The write fails if it cannot be
               formatted (no null stack trace saved instead) */
            JSValue stack = js_backtrace_format(ctx, p->u.error.backtrace);
            int ret;
            if (JS_IsException(stack))
                return -1;
            ret = snap_put_value(w, stack);
            JS_FreeValue(ctx, stack);
            return ret;
        }
        return snap_put_value(w, p->u.error.stack);
    case JS_CLASS_CALL_SITE:
        {
            JSCallSiteData *csd = p->u.opaque;
            bc_put_u8(&w->bc, csd != NULL);
            if (!csd)
                break;
            if (snap_put_value(w, csd->filename) ||
                snap_put_value(w, csd->func) ||
                snap_put_value(w, csd->func_name))
                return -1;
            bc_put_leb128(&w->bc, csd->line_num);
            bc_put_leb128(&w->bc, csd->col_num);
            bc_put_u8(&w->bc, csd->native | (csd->constructor << 1));
        }
        break;
    case JS_CLASS_C_FUNCTION:
        if (snap_put_ref(w, SNAP_CELL_CONTEXT, p->u.cfunc.realm) ||
            snap_put_native(w, p->u.cfunc.c_function.generic))
            return -1;
        bc_put_u8(&w->bc, p->u.cfunc.length);
        bc_put_u8(&w->bc, p->u.cfunc.cproto);
        bc_put_sleb128(&w->bc, p->u.cfunc.magic);
        break;
    case JS_CLASS_BYTECODE_FUNCTION:
    case JS_CLASS_GENERATOR_FUNCTION:
    case JS_CLASS_ASYNC_FUNCTION:
    case JS_CLASS_ASYNC_GENERATOR_FUNCTION:
        {
            JSFunctionBytecode *b = p->u.func.function_bytecode;
            if (snap_put_bytecode(w, b) ||
                snap_put_obj(w, p->u.func.home_object))
                return -1;
            for(i = 0; i < b->closure_var_count; i++) {
                if (snap_put_ref(w, SNAP_CELL_VAR_REF,
                                 p->u.func.var_refs ? p->u.func.var_refs[i] : NULL))
                    return -1;
            }
        }
        break;
    case JS_CLASS_BOUND_FUNCTION:
        {
            JSBoundFunction *bf = p->u.bound_function;
            if (snap_put_value(w, bf->func_obj) ||
                snap_put_value(w, bf->this_val))
                return -1;
            bc_put_leb128(&w->bc, bf->argc);
            for(i = 0; i < bf->argc; i++) {
                if (snap_put_value(w, bf->argv[i]))
                    return -1;
            }
        }
        break;
    case JS_CLASS_C_FUNCTION_DATA:
        {
            JSCFunctionDataRecord *s = p->u.c_function_data_record;
            if (snap_put_native(w, s->func))
                return -1;
            bc_put_u8(&w->bc, s->length);
            bc_put_u8(&w->bc, s->data_len);
            bc_put_leb128(&w->bc, s->magic);
            for(i = 0; i < s->data_len; i++) {
                if (snap_put_value(w, s->data[i]))
                    return -1;
            }
        }
        break;
    case JS_CLASS_C_CLOSURE:
        /* the opaque pointer and its finalizer are host state */
        JS_ThrowTypeError(ctx, "snapshot: unsupported C closure (JS_NewCClosure())");
        return -1;
    case JS_CLASS_GLOBAL_OBJECT:
        return snap_put_value(w, p->u.global_object.uninitialized_vars);
    case JS_CLASS_REGEXP:
        if (snap_put_value(w, JS_MKPTR(JS_TAG_STRING, p->u.regexp.pattern)) ||
            snap_put_value(w, JS_MKPTR(JS_TAG_STRING, p->u.regexp.bytecode)))
            return -1;
        break;
    default:
        {
            int ret = snap_write_data_payload(w, p);
            if (ret <= 0)
                return ret;
            ret = snap_write_control_payload(w, p);
            if (ret <= 0)
                return ret;
        }
        if (p->class_id >= JS_CLASS_INIT_COUNT &&
            p->class_id < w->rt->class_count) {
            /* host class: the host describes its opaque data */
            JSValue saved;
            if (!p->u.opaque) {
                bc_put_u8(&w->bc, 0);
                break;
            }
            if (!w->host || !w->host->save_opaque)
                goto unsupported;
            saved = w->host->save_opaque(ctx, JS_MKPTR(JS_TAG_OBJECT, p),
                                         w->host->opaque);
            if (JS_IsException(saved) || snap_keep(w, saved))
                return -1;
            bc_put_u8(&w->bc, 1);
            return snap_put_value(w, saved);
        }
    unsupported:
        {
            char buf[ATOM_GET_STR_BUF_SIZE];
            JS_ThrowTypeError(ctx, "snapshot: unsupported object class '%s'",
                              JS_AtomGetStr(ctx, buf, sizeof(buf),
                                            w->rt->class_array[p->class_id].class_name));
        }
        return -1;
    }
    return 0;
}

static int snap_write_object(SnapWriter *w, JSObject *p)
{
    JSShape *sh = p->shape;
    JSShapeProperty *prs;
    JSProperty *pr;
    int i, flags;

    if (snap_put_obj(w, sh->proto))
        return -1;
    flags = 0;
    if (p->extensible)
        flags |= SNAP_OBJ_EXTENSIBLE;
    if (p->is_std_array_prototype)
        flags |= SNAP_OBJ_STD_ARRAY_PROTO;
    if (p->has_immutable_prototype)
        flags |= SNAP_OBJ_IMMUTABLE_PROTO;
    if (p->is_constructor)
        flags |= SNAP_OBJ_CONSTRUCTOR;
    if (p->is_HTMLDDA)
        flags |= SNAP_OBJ_HTMLDDA;
    if (p->fast_array)
        flags |= SNAP_OBJ_FAST_ARRAY;
    bc_put_u8(&w->bc, flags);

    /* the properties end with JS_ATOM_NULL rather than starting with
       their count, so that adding one only changes the end of the
       property list (see snap_put_changed_body()) */
    for(i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        if (prs->atom == JS_ATOM_NULL)
            continue;
        pr = &p->prop[i];
        if (bc_put_atom(&w->bc, prs->atom))
            return -1;
        bc_put_u8(&w->bc, prs->flags);
        switch(prs->flags & JS_PROP_TMASK) {
        case JS_PROP_NORMAL:
            if (snap_put_value(w, pr->u.value))
                return -1;
            break;
        case JS_PROP_GETSET:
            if (snap_put_obj(w, pr->u.getset.getter) ||
                snap_put_obj(w, pr->u.getset.setter))
                return -1;
            break;
        case JS_PROP_VARREF:
            if (snap_put_ref(w, SNAP_CELL_VAR_REF, pr->u.var_ref))
                return -1;
            break;
        case JS_PROP_AUTOINIT:
            {
                JSAutoInitIDEnum id = js_autoinit_get_id(pr);
                if (snap_put_ref(w, SNAP_CELL_CONTEXT, js_autoinit_get_realm(pr)))
                    return -1;
                bc_put_u8(&w->bc, id);
                switch(id) {
                case JS_AUTOINIT_ID_PROTOTYPE:
                    break;
                case JS_AUTOINIT_ID_MODULE_NS:
                    if (snap_put_ref(w, SNAP_CELL_MODULE, pr->u.init.opaque))
                        return -1;
                    break;
                case JS_AUTOINIT_ID_PROP:
                    if (snap_put_native(w, pr->u.init.opaque))
                        return -1;
                    break;
                default:
                    abort();
                }
            }
            break;
        }
    }
    if (bc_put_atom(&w->bc, JS_ATOM_NULL))
        return -1;
    return snap_write_payload(w, p);
}

static int snap_write_cell(SnapWriter *w, SnapCell *c)
{
    switch(c->type) {
    case SNAP_CELL_CONTEXT:
        return snap_write_context(w, c->ptr);
    case SNAP_CELL_OBJECT:
        return snap_write_object(w, c->ptr);
    case SNAP_CELL_VAR_REF:
        return snap_write_var_ref(w, c->ptr);
    case SNAP_CELL_FRAME:
        return snap_write_frame(w, c->ptr);
    case SNAP_CELL_MODULE:
        return snap_write_module(w, c->ptr);
    case SNAP_CELL_BYTECODE:
        return snap_write_bytecode(w, c->ptr);
    case SNAP_CELL_RESOLVED:
        return snap_write_resolved(w, c->ptr);
    default:
        abort();
    }
}

static int snap_leb128_size(uint32_t v)
{
    int n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

/* Write the body of the changed base cell 'idx' in a delta. A changed
   body usually differs from its base body in a few bytes (a property
   added or set): only the bytes between their common prefix and suffix
   are written when that is shorter. The length of a delta body is
   shifted left by one, its low bit set for a patch. */
static void snap_put_changed_body(SnapWriter *w, int idx, const DynBuf *body,
                                  const uint8_t *base_body, uint32_t base_len)
{
    uint32_t len = body->size, max = min_uint32(len, base_len);
    uint32_t prefix = 0, suffix = 0, patch_len;

    while (prefix < max && body->buf[prefix] == base_body[prefix])
        prefix++;
    while (suffix < max - prefix &&
           body->buf[len - 1 - suffix] == base_body[base_len - 1 - suffix])
        suffix++;
    patch_len = snap_leb128_size(prefix) + snap_leb128_size(suffix) +
        len - prefix - suffix;
    dbuf_put_leb128(&w->bodies, idx);
    if (patch_len < len) {
        dbuf_put_leb128(&w->bodies, (patch_len << 1) | 1);
        dbuf_put_leb128(&w->bodies, prefix);
        dbuf_put_leb128(&w->bodies, suffix);
        dbuf_put(&w->bodies, body->buf + prefix, len - prefix - suffix);
    } else {
        dbuf_put_leb128(&w->bodies, len << 1);
        dbuf_put(&w->bodies, body->buf, len);
    }
    w->body_count++;
}

/* Write the body of the cell 'idx' in the bodies section, unless it is a
   base cell whose body is the same as in the base. */
static int snap_write_body(SnapWriter *w, int idx)
{
    SnapCell c = *snap_writer_cell(w, idx);
    DynBuf out = w->bc.dbuf;
    DynBuf *body;
    int ret;

    /* the body is written in its own buffer to compare and measure it */
    w->bc.dbuf = w->scratch;
    w->bc.dbuf.size = 0;
    ret = snap_write_cell(w, &c);
    w->scratch = w->bc.dbuf;
    w->bc.dbuf = out;
    body = &w->scratch;
    if (ret)
        return -1;
    if (dbuf_error(body))
        return snap_writer_oom(w);

    if (idx < w->base_cell_count) {
        const JSSnapshotBase *base = w->base;
        uint32_t off = base->body_offsets[idx];
        uint32_t base_len = base->body_offsets[idx + 1] - off;
        if (body->size == base_len &&
            !memcmp(body->buf, base->bodies + off, body->size))
            return 0; /* unchanged */
        if (snap_resize(w->rt, (void **)&w->changed, sizeof(w->changed[0]),
                        &w->changed_size, w->changed_count + 1))
            return snap_writer_oom(w);
        w->changed[w->changed_count++] = idx;
        snap_put_changed_body(w, idx, body, base->bodies + off, base_len);
        return 0;
    }
    if (w->flags & JS_SNAPSHOT_DELTA) {
        dbuf_put_leb128(&w->bodies, idx);
        dbuf_put_leb128(&w->bodies, body->size << 1);
    } else {
        dbuf_put_leb128(&w->bodies, body->size);
    }
    dbuf_put(&w->bodies, body->buf, body->size);
    w->body_count++;
    if (w->flags & JS_SNAPSHOT_BASE) {
        /* full snapshot: the bodies are written in cell order */
        if (snap_resize(w->rt, (void **)&w->base_offsets, sizeof(w->base_offsets[0]),
                        &w->base_offsets_size, idx + 2))
            return snap_writer_oom(w);
        w->base_offsets[idx] = w->base_bodies.size;
        dbuf_put(&w->base_bodies, body->buf, body->size);
        w->base_offsets[idx + 1] = w->base_bodies.size;
    }
    return 0;
}

static void snap_put_bytes(BCWriterState *s, const void *buf, size_t len)
{
    bc_put_leb128(s, len);
    dbuf_put(&s->dbuf, buf, len);
}

static void snap_write_cell_header(SnapWriter *w, SnapCell *c)
{
    bc_put_u8(&w->bc, c->type);
    switch(c->type) {
    case SNAP_CELL_OBJECT:
        bc_put_leb128(&w->bc, ((JSObject *)c->ptr)->class_id);
        break;
    case SNAP_CELL_FRAME:
        {
            JSAsyncFunctionState *s = c->ptr;
            if (s->is_completed) {
                bc_put_leb128(&w->bc, 0);
                bc_put_leb128(&w->bc, 0);
                bc_put_leb128(&w->bc, 0);
                bc_put_leb128(&w->bc, 0);
            } else {
                JSFunctionBytecode *b = snap_frame_bytecode(&s->frame);
                bc_put_leb128(&w->bc, s->frame.arg_count);
                bc_put_leb128(&w->bc, b->var_count);
                bc_put_leb128(&w->bc, b->stack_size);
                bc_put_leb128(&w->bc, b->var_ref_count);
            }
        }
        break;
    case SNAP_CELL_BYTECODE:
        {
            /* what js_alloc_function_bytecode() needs */
            JSFunctionBytecode *b = c->ptr;
            bc_put_u8(&w->bc, b->has_debug);
            bc_put_leb128(&w->bc, b->arg_count);
            bc_put_leb128(&w->bc, b->var_count);
            bc_put_leb128(&w->bc, b->stack_size);
            bc_put_leb128(&w->bc, b->var_ref_count);
            bc_put_leb128(&w->bc, b->closure_var_count);
            bc_put_leb128(&w->bc, b->cpool_count);
            bc_put_leb128(&w->bc, b->vardefs ? b->arg_count + b->var_count : 0);
            bc_put_leb128(&w->bc, b->byte_code_len);
        }
        break;
    default:
        break;
    }
}

static void snap_put_atom_def(BCWriterState *s, JSAtomStruct *p)
{
    if (p->atom_type == JS_ATOM_TYPE_STRING) {
        bc_put_u8(s, SNAP_ATOM_STRING);
    } else if (p->atom_type == JS_ATOM_TYPE_GLOBAL_SYMBOL) {
        bc_put_u8(s, SNAP_ATOM_GLOBAL_SYMBOL);
    } else if (p->hash == JS_ATOM_HASH_PRIVATE) {
        bc_put_u8(s, SNAP_ATOM_PRIVATE);
    } else if (p->len == 0 && p->is_wide_char) {
        bc_put_u8(s, SNAP_ATOM_SYMBOL_NO_DESC);
        return;
    } else {
        bc_put_u8(s, SNAP_ATOM_SYMBOL);
    }
    JS_WriteString(s, p);
}

static uint64_t snap_hash64(const uint8_t *buf, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (len--) {
        h ^= *buf++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Assemble the snapshot: everything but the roots and the bodies is
   only known once every body is written. For a full snapshot with
   JS_SNAPSHOT_BASE, *pid is set to its base id. */
static int snap_write_output(SnapWriter *w, DynBuf *roots, uint64_t *pid)
{
    JSRuntime *rt = w->rt;
    BCWriterState *s = &w->bc;
    size_t id_pos;
    int i, base_atom_count;

    js_dbuf_init(w->ctx, &s->dbuf);
    dbuf_put(&s->dbuf, snap_magic, sizeof(snap_magic));
    bc_put_leb128(s, SNAP_FORMAT_VERSION);
    bc_put_u8(s, w->base ? SNAP_KIND_DELTA : SNAP_KIND_FULL);
    id_pos = s->dbuf.size;
    bc_put_u64(s, w->base ? w->base->id : 0);
    bc_put_u32(s, w->natives.signature);

    /* host classes must be registered with the same ids */
    bc_put_leb128(s, rt->class_count);
    for(i = JS_CLASS_INIT_COUNT; i < rt->class_count; i++) {
        if (JS_IsRegisteredClass(rt, i)) {
            char buf[ATOM_GET_STR_BUF_SIZE];
            const char *name = JS_AtomGetStrRT(rt, buf, sizeof(buf),
                                               rt->class_array[i].class_name);
            bc_put_u8(s, 1);
            snap_put_bytes(s, name, strlen(name));
        } else {
            bc_put_u8(s, 0);
        }
    }

    /* cells that are not in the base */
    bc_put_leb128(s, w->cell_count);
    for(i = 0; i < w->cell_count; i++)
        snap_write_cell_header(w, &w->cells[i]);

    if (w->base) {
        bc_put_leb128(s, w->changed_count);
        for(i = 0; i < w->changed_count; i++)
            bc_put_leb128(s, w->changed[i]);
    }

    base_atom_count = w->base ? w->base->atom_count : 0;
    bc_put_leb128(s, s->idx_to_atom_count - base_atom_count);
    for(i = base_atom_count; i < s->idx_to_atom_count; i++)
        snap_put_atom_def(s, rt->atom_array[s->idx_to_atom[i]]);

    bc_put_leb128(s, w->str_count);
    for(i = 0; i < w->str_count; i++)
        JS_WriteString(s, w->strs[i].str);

    bc_put_leb128(s, roots->size);
    dbuf_put(&s->dbuf, roots->buf, roots->size);
    bc_put_leb128(s, w->body_count);
    dbuf_put(&s->dbuf, w->bodies.buf, w->bodies.size);
    if (dbuf_error(&s->dbuf))
        return snap_writer_oom(w);

    if (w->flags & JS_SNAPSHOT_BASE) {
        /* the id is the hash of the snapshot with a null id */
        uint64_t id = snap_hash64(s->dbuf.buf, s->dbuf.size) | 1;
        *pid = id;
        if (is_be())
            id = bswap64(id);
        memcpy(s->dbuf.buf + id_pos, &id, sizeof(id));
    }
    return 0;
}

/* JS_SNAPSHOT_BASE: the cells, atoms and strings of the snapshot become
   the base of the runtime. The references of the writer are transferred
   to the base. */
static int snap_attach_written_base(SnapWriter *w, uint64_t id)
{
    JSRuntime *rt = w->rt;
    JSSnapshotBase *base;
    int i;

    base = js_mallocz_rt(rt, sizeof(*base));
    if (!base)
        return snap_writer_oom(w);
    base->id = id;
    base->atoms = js_malloc_rt(rt, sizeof(base->atoms[0]) * max_int(w->bc.idx_to_atom_count, 1));
    base->strs = js_malloc_rt(rt, sizeof(base->strs[0]) * max_int(w->str_count, 1));
    if (!base->atoms || !base->strs) {
        js_free_rt(rt, base->atoms);
        js_free_rt(rt, base->strs);
        js_free_rt(rt, base);
        return snap_writer_oom(w);
    }
    for(i = 0; i < w->bc.idx_to_atom_count; i++)
        base->atoms[i] = JS_DupAtom(w->ctx, w->bc.idx_to_atom[i]);
    base->atom_count = w->bc.idx_to_atom_count;
    for(i = 0; i < w->str_count; i++) {
        base->strs[i] = w->strs[i].key;
        w->strs[i].key = JS_UNDEFINED;
    }
    base->str_count = w->str_count;
    base->str_map = w->str_map;
    memset(&w->str_map, 0, sizeof(w->str_map));
    base->cells = w->cells;
    base->cell_count = w->cell_count;
    base->cell_map = w->cell_map;
    w->cells = NULL;
    w->cell_count = 0;
    memset(&w->cell_map, 0, sizeof(w->cell_map));
    base->bodies = w->base_bodies.buf;
    base->body_offsets = w->base_offsets;
    w->base_bodies.buf = NULL;
    w->base_offsets = NULL;
    if (base->cell_count == 0) {
        /* no cell: a single offset */
        base->body_offsets = js_mallocz_rt(rt, sizeof(base->body_offsets[0]));
    }
    JS_DropSnapshotBase(rt);
    rt->snapshot_base = base;
    return 0;
}

static void snap_writer_free(SnapWriter *w)
{
    JSRuntime *rt = w->rt;
    int i;

    for(i = 0; i < w->str_count; i++) {
        JS_FreeValueRT(rt, w->strs[i].key);
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, w->strs[i].str));
    }
    for(i = 0; i < w->keep_count; i++)
        JS_FreeValueRT(rt, w->keep[i]);
    js_free_rt(rt, w->strs);
    js_free_rt(rt, w->keep);
    for(i = 0; i < w->cell_count; i++)
        snap_cell_free(rt, &w->cells[i]);
    js_free_rt(rt, w->cells);
    js_free_rt(rt, w->queue);
    js_free_rt(rt, w->changed);
    js_free_rt(rt, w->base_queued);
    js_free_rt(rt, w->base_offsets);
    dbuf_free(&w->scratch);
    dbuf_free(&w->bodies);
    dbuf_free(&w->base_bodies);
    snap_map_free(rt, &w->cell_map);
    snap_map_free(rt, &w->str_map);
    snap_natives_free(rt, &w->natives);
    js_free_rt(rt, w->bc.atom_to_idx);
    js_free_rt(rt, w->bc.idx_to_atom);
}

uint8_t *JS_WriteSnapshot(JSContext *ctx, size_t *psize,
                          const JSSnapshotHost *host,
                          JSValueConst *roots, int root_count, int flags)
{
    JSRuntime *rt = ctx->rt;
    SnapWriter ww, *w = &ww;
    struct list_head *el;
    DynBuf roots_buf;
    uint64_t id = 0;
    int i, n;

    *psize = 0;
    memset(w, 0, sizeof(*w));
    w->ctx = ctx;
    w->rt = rt;
    w->host = host;
    w->flags = flags;
    w->bc.ctx = ctx;
    w->bc.allow_bytecode = TRUE;
    w->bc.is_snapshot = TRUE;
    w->bc.first_atom = JS_ATOM_END;
    js_dbuf_init(ctx, &w->bc.dbuf);
    js_dbuf_init(ctx, &w->scratch);
    js_dbuf_init(ctx, &w->bodies);
    js_dbuf_init(ctx, &w->base_bodies);

    if ((flags & ~(JS_SNAPSHOT_BASE | JS_SNAPSHOT_DELTA)) ||
        (flags & JS_SNAPSHOT_BASE && flags & JS_SNAPSHOT_DELTA)) {
        JS_ThrowTypeError(ctx, "snapshot: invalid flags");
        goto fail;
    }
    if (rt->current_stack_frame) {
        JS_ThrowTypeError(ctx, "snapshot: JS code is running");
        goto fail;
    }
    if (snap_natives_init(rt, &w->natives, host, TRUE)) {
        snap_writer_oom(w);
        goto fail;
    }
    if (flags & JS_SNAPSHOT_DELTA) {
        JSSnapshotBase *base = rt->snapshot_base;
        if (!base) {
            JS_ThrowTypeError(ctx, "snapshot: no base (JS_SNAPSHOT_BASE)");
            goto fail;
        }
        w->base = base;
        w->base_cell_count = base->cell_count;
        w->base_str_count = base->str_count;
        w->base_queued = js_mallocz_rt(rt, max_int(base->cell_count, 1));
        if (!w->base_queued) {
            snap_writer_oom(w);
            goto fail;
        }
        /* the base atoms keep their indexes */
        for(i = 0; i < base->atom_count; i++) {
            uint32_t v;
            if (bc_atom_to_idx(&w->bc, &v, base->atoms[i]))
                goto fail;
        }
        /* every base cell is compared, even if it is no longer reachable:
           the reader restores all of them, and a reachable cell can
           still modify them (e.g. detaching an ArrayBuffer clears its
           views) */
        for(i = 0; i < base->cell_count; i++) {
            w->base_queued[i] = TRUE;
            if (snap_queue(w, i))
                goto fail;
        }
    }

    /* roots */
    n = 0;
    list_for_each(el, &rt->context_list)
        n++;
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &rt->context_list) {
        JSContext *ctx1 = list_entry(el, JSContext, link);
        if (snap_put_ref(w, SNAP_CELL_CONTEXT, ctx1))
            goto fail;
        /* a base is the same for every seed and clock */
        if (!(flags & JS_SNAPSHOT_BASE)) {
            JSFloat64Union u;
            bc_put_u64(&w->bc, ctx1->random_state);
            /* performance.now() continues from its value at the write */
            u.d = ctx1->time_origin;
            bc_put_u64(&w->bc, u.u64);
            bc_put_u64(&w->bc, js_performance_elapsed_ns(ctx1));
        }
    }
    n = 0;
    list_for_each(el, &rt->job_list)
        n++;
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &rt->job_list) {
        JSJobEntry *e = list_entry(el, JSJobEntry, link);
        if (snap_put_ref(w, SNAP_CELL_CONTEXT, e->realm) ||
            snap_put_native(w, e->job_func))
            goto fail;
        bc_put_leb128(&w->bc, e->argc);
        for(i = 0; i < e->argc; i++) {
            if (snap_put_value(w, e->argv[i]))
                goto fail;
        }
    }
    bc_put_u64(&w->bc, rt->module_async_evaluation_next_timestamp);
    bc_put_leb128(&w->bc, root_count);
    for(i = 0; i < root_count; i++) {
        if (snap_put_value(w, roots[i]))
            goto fail;
    }
    /* outermost frame of the suspended execution */
    if (snap_put_ref(w, SNAP_CELL_FRAME, rt->suspended))
        goto fail;
    roots_buf = w->bc.dbuf;
    js_dbuf_init(ctx, &w->bc.dbuf);
    if (dbuf_error(&roots_buf)) {
        dbuf_free(&roots_buf);
        snap_writer_oom(w);
        goto fail;
    }

    /* cell bodies; writing a body can queue new cells */
    for(i = 0; i < w->queue_count; i++) {
        if (snap_write_body(w, w->queue[i])) {
            dbuf_free(&roots_buf);
            goto fail;
        }
    }
    if (dbuf_error(&w->bodies) || dbuf_error(&w->base_bodies)) {
        dbuf_free(&roots_buf);
        snap_writer_oom(w);
        goto fail;
    }

    dbuf_free(&w->bc.dbuf);
    if (snap_write_output(w, &roots_buf, &id)) {
        dbuf_free(&roots_buf);
        goto fail;
    }
    dbuf_free(&roots_buf);
    if ((flags & JS_SNAPSHOT_BASE) && snap_attach_written_base(w, id))
        goto fail;
    *psize = w->bc.dbuf.size;
    snap_writer_free(w);
    return w->bc.dbuf.buf;
 fail:
    dbuf_free(&w->bc.dbuf);
    snap_writer_free(w);
    return NULL;
}

/*******************************************************************/
/* reader */

typedef struct {
    void *ptr;
    SnapCellType type;
    uint8_t done; /* body read */
    uint8_t changed; /* base cell whose body is in the delta */
    union {
        JSClassID class_id; /* SNAP_CELL_OBJECT */
        struct { /* SNAP_CELL_FRAME */
            uint32_t arg_buf_len;
            uint32_t var_count;
            uint32_t stack_size;
            uint32_t var_ref_count;
        } frame;
        struct { /* SNAP_CELL_BYTECODE */
            uint32_t has_debug;
            uint32_t arg_count;
            uint32_t var_count;
            uint32_t stack_size;
            uint32_t var_ref_count;
            uint32_t closure_var_count;
            uint32_t cpool_count;
            uint32_t local_count;
            uint32_t byte_code_len;
        } bytecode;
    } u;
} SnapRCell;

typedef struct {
    JSObject *obj;
    JSValue saved;
} SnapOpaque;

/* Deferred work, called exactly once, in reverse registration order:
   after every cell body is read (GC still disabled) with 'abort' = FALSE, or with 'abort'
   = TRUE if the read failed, in which case it must only release 'opaque'
   and leave the objects it was going to complete safe to free. */
typedef int SnapFixupFunc(SnapReader *r, void *opaque, BOOL abort);

typedef struct {
    SnapFixupFunc *func;
    void *opaque;
} SnapFixup;

/* a section of the base or of the delta */
typedef struct {
    const uint8_t *start; /* snapshot start (for error positions) */
    const uint8_t *ptr;
    const uint8_t *end;
} SnapInput;

struct SnapReader {
    BCReaderState bc; /* must be first: current input and atom table */
    JSRuntime *rt;
    JSContext *ctx; /* first context: used for allocations and errors */
    const JSSnapshotHost *host;
    SnapNatives natives;
    SnapRCell *cells; /* base cells, then the new cells of the delta */
    int cell_count;
    int cell_size;
    int base_cell_count;
    uint8_t *obj_flags; /* per cell, SNAP_OBJ_x */
    JSString **strs; /* base strings, then delta strings */
    int str_count;
    int str_size;
    int base_atom_count;
    int base_str_count;
    uint64_t base_id; /* id of the full snapshot, 0 if it is not a base */
    BOOL roots_seeded; /* the roots hold the Math.random() states */
    SnapInput base_roots, delta_roots, base_bodies, delta_bodies;
    /* base body of each base cell (to attach the base to the runtime) */
    const uint8_t **base_body_ptrs;
    uint32_t *base_body_lens;
    SnapOpaque *opaques; /* host-class data, loaded at the end */
    int opaque_count;
    int opaque_size;
    SnapFixup *fixups;
    int fixup_count;
    int fixup_size;
    /* object whose payload is being read: if the read fails, its
       finalizer runs only once its payload is consistent (see
       snap_payload_ready()) */
    JSObject *cur_obj;
    BOOL cur_obj_ready;
    char *errbuf;
    size_t errbuf_size;
};

/* Called by a payload reader once the payload of the object being read
   can be finalized (every field the finalizer uses is initialized). If
   the read fails later, the class finalizer then releases what was read;
   before this call, the object is turned into a plain object instead. */
static void snap_payload_ready(SnapReader *r)
{
    r->cur_obj_ready = TRUE;
}

static int snap_get_value(SnapReader *r, JSValue *pval);

static int __attribute__((format(printf, 2, 3))) snap_error(SnapReader *r, const char *fmt, ...)
{
    va_list ap;
    if (r->errbuf_size && !r->errbuf[0]) {
        va_start(ap, fmt);
        vsnprintf(r->errbuf, r->errbuf_size, fmt, ap);
        va_end(ap);
    }
    r->bc.error_state = -1;
    return -1;
}

static int snap_add_fixup(SnapReader *r, SnapFixupFunc *func, void *opaque)
{
    if (snap_resize(r->rt, (void **)&r->fixups, sizeof(r->fixups[0]),
                    &r->fixup_size, r->fixup_count + 1)) {
        func(r, opaque, TRUE);
        JS_ThrowOutOfMemory(r->ctx);
        return -1;
    }
    r->fixups[r->fixup_count].func = func;
    r->fixups[r->fixup_count].opaque = opaque;
    r->fixup_count++;
    return 0;
}

/* header readers: no context exists yet, errors go to 'errbuf' */
static int snap_hdr_u8(SnapReader *r, uint8_t *pval)
{
    if (r->bc.ptr >= r->bc.buf_end) {
        *pval = 0; /* avoid warning */
        return snap_error(r, "truncated snapshot");
    }
    *pval = *r->bc.ptr++;
    return 0;
}

static int snap_hdr_leb(SnapReader *r, uint32_t *pval)
{
    int ret = get_leb128(pval, r->bc.ptr, r->bc.buf_end);
    if (ret < 0)
        return snap_error(r, "truncated snapshot");
    r->bc.ptr += ret;
    return 0;
}

static int snap_get_leb(SnapReader *r, uint32_t *pval)
{
    return bc_get_leb128(&r->bc, pval);
}

static void snap_set_input(SnapReader *r, const SnapInput *in)
{
    r->bc.buf_start = in->start;
    r->bc.ptr = in->ptr;
    r->bc.buf_end = in->end;
}

/* read a length-prefixed section (header reader) */
static int snap_hdr_section(SnapReader *r, SnapInput *in)
{
    uint32_t len;
    if (snap_hdr_leb(r, &len))
        return -1;
    if (len > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    in->start = r->bc.buf_start;
    in->ptr = r->bc.ptr;
    in->end = r->bc.ptr + len;
    r->bc.ptr += len;
    return 0;
}

/* read a reference to a cell of type 'type' (borrowed, may be NULL) */
static int snap_get_ref(SnapReader *r, SnapCellType type, void **pptr)
{
    uint32_t v;
    if (snap_get_leb(r, &v))
        return -1;
    if (v == 0) {
        *pptr = NULL;
        return 0;
    }
    v--;
    if (v >= r->cell_count || r->cells[v].type != type) {
        *pptr = NULL; /* avoid warning */
        return snap_error(r, "invalid %s reference", snap_cell_type_names[type]);
    }
    *pptr = r->cells[v].ptr;
    return 0;
}

static int snap_get_obj(SnapReader *r, JSObject **pp)
{
    return snap_get_ref(r, SNAP_CELL_OBJECT, (void **)pp);
}

/* new reference to a context (may be NULL) */
static int snap_get_context(SnapReader *r, JSContext **pctx)
{
    JSContext *ctx;
    if (snap_get_ref(r, SNAP_CELL_CONTEXT, (void **)&ctx))
        return -1;
    *pctx = ctx ? JS_DupContext(ctx) : NULL;
    return 0;
}

static int snap_get_native(SnapReader *r, void **pptr)
{
    uint32_t v;
    if (snap_get_leb(r, &v))
        return -1;
    if (v == 0) {
        *pptr = NULL;
        return 0;
    }
    if (v - 1 >= r->natives.count || !r->natives.tab[v - 1])
        return snap_error(r, "invalid native code index");
    *pptr = (void *)r->natives.tab[v - 1];
    return 0;
}

/* body of a bytecode cell: the bytecode reader fills the shell allocated
   with the sizes of the cell header */
static int snap_read_bytecode(SnapReader *r, SnapRCell *c)
{
    JSFunctionBytecode *b = c->ptr, bc;
    int local_count;
    uint8_t tag;

    if (bc_get_u8(&r->bc, &tag))
        return -1;
    if (tag != BC_TAG_FUNCTION_BYTECODE)
        return snap_error(r, "invalid bytecode");
    if (JS_ReadFunctionHeader(&r->bc, &bc, &local_count)) {
        JS_FreeAtom(r->ctx, bc.func_name);
        return -1;
    }
    if (bc.has_debug != b->has_debug || bc.arg_count != b->arg_count ||
        bc.var_count != b->var_count || bc.stack_size != b->stack_size ||
        bc.var_ref_count != b->var_ref_count ||
        bc.closure_var_count != b->closure_var_count ||
        bc.cpool_count != b->cpool_count ||
        bc.byte_code_len != b->byte_code_len ||
        local_count != c->u.bytecode.local_count) {
        JS_FreeAtom(r->ctx, bc.func_name);
        return snap_error(r, "bytecode layout mismatch");
    }
    /* the fields that the cell header does not give */
    b->func_name = bc.func_name;
    b->js_mode = bc.js_mode;
    b->has_prototype = bc.has_prototype;
    b->has_simple_parameter_list = bc.has_simple_parameter_list;
    b->is_derived_class_constructor = bc.is_derived_class_constructor;
    b->need_home_object = bc.need_home_object;
    b->func_kind = bc.func_kind;
    b->new_target_allowed = bc.new_target_allowed;
    b->super_call_allowed = bc.super_call_allowed;
    b->super_allowed = bc.super_allowed;
    b->arguments_allowed = bc.arguments_allowed;
    b->is_direct_or_indirect_eval = bc.is_direct_or_indirect_eval;
    b->defined_arg_count = bc.defined_arg_count;
    if (JS_ReadFunctionBody(&r->bc, b, local_count))
        return -1;
    return snap_get_context(r, &b->realm);
}

static int snap_get_value(SnapReader *r, JSValue *pval)
{
    JSContext *ctx = r->ctx;
    uint8_t tag;
    uint32_t idx;
    int32_t v;
    void *ptr;

    *pval = JS_UNDEFINED;
    if (bc_get_u8(&r->bc, &tag))
        return -1;
    switch(tag) {
    case SNAP_VAL_UNDEFINED:
        break;
    case SNAP_VAL_NULL:
        *pval = JS_NULL;
        break;
    case SNAP_VAL_FALSE:
        *pval = JS_FALSE;
        break;
    case SNAP_VAL_TRUE:
        *pval = JS_TRUE;
        break;
    case SNAP_VAL_UNINITIALIZED:
        *pval = JS_UNINITIALIZED;
        break;
    case SNAP_VAL_INT32:
        if (bc_get_sleb128(&r->bc, &v))
            return -1;
        *pval = JS_NewInt32(ctx, v);
        break;
    case SNAP_VAL_FLOAT64:
        {
            JSFloat64Union u;
            if (bc_get_u64(&r->bc, &u.u64))
                return -1;
            *pval = __JS_NewFloat64(ctx, u.d);
        }
        break;
    case SNAP_VAL_CATCH_OFFSET:
        if (bc_get_sleb128(&r->bc, &v))
            return -1;
        *pval = JS_MKVAL(JS_TAG_CATCH_OFFSET, v);
        break;
    case SNAP_VAL_STRING:
        if (snap_get_leb(r, &idx))
            return -1;
        if (idx >= r->str_count)
            return snap_error(r, "invalid string index");
        *pval = JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, r->strs[idx]));
        break;
    case SNAP_VAL_SYMBOL:
        {
            JSAtom atom;
            JSAtomStruct *p;
            if (bc_get_atom(&r->bc, &atom))
                return -1;
            if (__JS_AtomIsTaggedInt(atom) ||
                (p = r->rt->atom_array[atom])->atom_type == JS_ATOM_TYPE_STRING) {
                JS_FreeAtom(ctx, atom);
                return snap_error(r, "invalid symbol");
            }
            /* symbol values are refcounted even for predefined atoms */
            *pval = JS_DupValue(ctx, JS_MKPTR(JS_TAG_SYMBOL, p));
            JS_FreeAtom(ctx, atom);
        }
        break;
    case SNAP_VAL_BIG_INT:
        {
            JSValue val;
            uint8_t tag1;
            if (bc_get_u8(&r->bc, &tag1))
                return -1;
            if (tag1 != BC_TAG_BIG_INT)
                return snap_error(r, "invalid bigint");
            val = JS_ReadBigInt(&r->bc);
            if (JS_IsException(val))
                return -1;
            *pval = val;
        }
        break;
    case SNAP_VAL_OBJECT:
        if (snap_get_ref(r, SNAP_CELL_OBJECT, &ptr))
            return -1;
        if (!ptr)
            return snap_error(r, "invalid object");
        *pval = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ptr));
        break;
    case SNAP_VAL_MODULE:
        if (snap_get_ref(r, SNAP_CELL_MODULE, &ptr))
            return -1;
        if (!ptr)
            return snap_error(r, "invalid module");
        *pval = JS_DupValue(ctx, JS_MKPTR(JS_TAG_MODULE, ptr));
        break;
    case SNAP_VAL_BYTECODE:
        if (snap_get_ref(r, SNAP_CELL_BYTECODE, &ptr))
            return -1;
        if (!ptr)
            return snap_error(r, "invalid bytecode");
        *pval = JS_DupValue(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, ptr));
        break;
    case SNAP_VAL_DEAD:
        return snap_error(r, "unexpected dead weak reference");
    default:
        return snap_error(r, "invalid value tag %d", tag);
    }
    return 0;
}

static JSValue snap_get_value_hook(BCReaderState *s)
{
    JSValue val;
    if (snap_get_value((SnapReader *)s, &val))
        return JS_EXCEPTION;
    return val;
}

/* weak reference: *pdead is set if the target was collected */
static int snap_get_weak_value(SnapReader *r, JSValue *pval, BOOL *pdead)
{
    if (r->bc.ptr < r->bc.buf_end && *r->bc.ptr == SNAP_VAL_DEAD) {
        r->bc.ptr++;
        *pval = JS_UNDEFINED;
        *pdead = TRUE;
        return 0;
    }
    *pdead = FALSE;
    return snap_get_value(r, pval);
}

/* read a value that must be an object (new reference) or null (NULL) */
static int snap_get_obj_value(SnapReader *r, JSObject **pp)
{
    JSObject *p;
    if (snap_get_obj(r, &p))
        return -1;
    if (p)
        JS_DupValue(r->ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    *pp = p;
    return 0;
}

static int snap_get_shape(SnapReader *r, JSShape **psh)
{
    JSContext *ctx = r->ctx;
    JSObject *proto;
    JSShape *sh;
    uint32_t i, n;
    uint8_t present, flags;
    JSAtom atom;

    *psh = NULL;
    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_obj(r, &proto) || snap_get_leb(r, &n))
        return -1;
    sh = js_new_shape2(ctx, proto, JS_PROP_INITIAL_HASH_SIZE, max_int(n, 1));
    if (!sh)
        return -1;
    for(i = 0; i < n; i++) {
        if (bc_get_atom(&r->bc, &atom))
            goto fail;
        if (bc_get_u8(&r->bc, &flags)) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        if (add_shape_property(ctx, &sh, NULL, atom, flags)) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        JS_FreeAtom(ctx, atom);
    }
    *psh = sh;
    return 0;
 fail:
    js_free_shape(r->rt, sh);
    return -1;
}

static int snap_read_context(SnapReader *r, JSContext *ctx)
{
    JSRuntime *rt = r->rt;
    uint32_t i, n;
    uint8_t has_regexp, has_eval;

    if (snap_get_leb(r, &n))
        return -1;
    if (n > rt->class_count)
        return snap_error(r, "class count mismatch");
    for(i = 0; i < n; i++) {
        if (snap_get_value(r, &ctx->class_proto[i]))
            return -1;
    }
    if (snap_get_value(r, &ctx->function_proto) ||
        snap_get_value(r, &ctx->function_ctor) ||
        snap_get_value(r, &ctx->array_ctor) ||
        snap_get_value(r, &ctx->regexp_ctor) ||
        snap_get_value(r, &ctx->promise_ctor) ||
        snap_get_value(r, &ctx->error_ctor))
        return -1;
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        if (snap_get_value(r, &ctx->native_error_proto[i]))
            return -1;
    }
    if (snap_get_value(r, &ctx->iterator_ctor) ||
        snap_get_value(r, &ctx->async_iterator_proto) ||
        snap_get_value(r, &ctx->array_proto_values) ||
        snap_get_value(r, &ctx->throw_type_error) ||
        snap_get_value(r, &ctx->eval_obj))
        return -1;
    for(i = 0; i < JS_CONTEXT_VALUE_COUNT; i++) {
        if (snap_get_value(r, &ctx->user_values[i]))
            return -1;
    }
    if (snap_get_value(r, &ctx->global_obj) ||
        snap_get_value(r, &ctx->global_var_obj))
        return -1;
    if (snap_get_leb(r, &n))
        return -1;
    ctx->binary_object_count = n;
    if (bc_get_leb128_int(&r->bc, &ctx->binary_object_size))
        return -1;
    if (bc_get_u8(&r->bc, &has_regexp) || bc_get_u8(&r->bc, &has_eval))
        return -1;
    ctx->compile_regexp = has_regexp ? js_compile_regexp : NULL;
    ctx->eval_internal = has_eval ? __JS_EvalInternal : NULL;
    if (snap_get_shape(r, &ctx->array_shape) ||
        snap_get_shape(r, &ctx->arguments_shape) ||
        snap_get_shape(r, &ctx->mapped_arguments_shape) ||
        snap_get_shape(r, &ctx->regexp_shape) ||
        snap_get_shape(r, &ctx->regexp_result_shape))
        return -1;
    if (snap_get_leb(r, &n))
        return -1;
    for(i = 0; i < n; i++) {
        JSModuleDef *m;
        if (snap_get_ref(r, SNAP_CELL_MODULE, (void **)&m))
            return -1;
        if (!m || m->link.next)
            return snap_error(r, "invalid module list");
        JS_DupValue(ctx, JS_MKPTR(JS_TAG_MODULE, m));
        list_add_tail(&m->link, &ctx->loaded_modules);
    }
    return 0;
}

static int snap_read_var_ref(SnapReader *r, JSVarRef *var_ref)
{
    uint8_t is_detached, is_lexical, is_const, is_arg;

    if (bc_get_u8(&r->bc, &is_detached) ||
        bc_get_u8(&r->bc, &is_lexical) ||
        bc_get_u8(&r->bc, &is_const))
        return -1;
    var_ref->is_lexical = is_lexical;
    var_ref->is_const = is_const;
    if (is_detached) {
        return snap_get_value(r, &var_ref->value);
    } else {
        JSAsyncFunctionState *s;
        SnapRCell *c;
        uint32_t frame_idx, var_ref_idx, idx;

        if (snap_get_leb(r, &frame_idx) ||
            snap_get_leb(r, &var_ref_idx) ||
            bc_get_u8(&r->bc, &is_arg) ||
            snap_get_leb(r, &idx))
            return -1;
        if (frame_idx == 0 || frame_idx - 1 >= r->cell_count ||
            (c = &r->cells[frame_idx - 1])->type != SNAP_CELL_FRAME)
            return snap_error(r, "invalid frame reference");
        s = c->ptr;
        if (var_ref_idx >= c->u.frame.var_ref_count ||
            idx >= (is_arg ? c->u.frame.arg_buf_len : c->u.frame.var_count) ||
            s->frame.var_refs[var_ref_idx])
            return snap_error(r, "invalid variable reference");
        /* same bookkeeping as get_var_ref() for a heap frame */
        var_ref->is_detached = FALSE;
        var_ref->var_ref_idx = var_ref_idx;
        var_ref->stack_frame = &s->frame;
        var_ref->pvalue = (is_arg ? s->frame.arg_buf : s->frame.var_buf) + idx;
        s->frame.var_refs[var_ref_idx] = var_ref;
        js_rc(s)->ref_count++;
    }
    return 0;
}

static int snap_read_frame(SnapReader *r, SnapRCell *c)
{
    JSAsyncFunctionState *s = c->ptr;
    JSStackFrame *sf = &s->frame;
    JSFunctionBytecode *b;
    JSValue bval;
    uint8_t is_completed, throw_flag;
    uint32_t argc, js_mode, pc_pos, sp_pos, i;

    if (bc_get_u8(&r->bc, &is_completed) ||
        bc_get_u8(&r->bc, &throw_flag))
        return -1;
    s->throw_flag = throw_flag;
    if (snap_get_value(r, &s->resolving_funcs[0]) ||
        snap_get_value(r, &s->resolving_funcs[1]))
        return -1;
    if (is_completed)
        return 0;
    if (snap_get_value(r, &bval))
        return -1;
    if (JS_VALUE_GET_TAG(bval) != JS_TAG_FUNCTION_BYTECODE) {
        JS_FreeValue(r->ctx, bval);
        return snap_error(r, "invalid frame bytecode");
    }
    /* the reader's cell table keeps 'b' alive. Its body may not be read
       yet, but its shell has the sizes. */
    b = JS_VALUE_GET_PTR(bval);
    JS_FreeValue(r->ctx, bval);
    /* from now on, a failed read frees what was read (cur_func, this_val
       and the values up to cur_sp are JS_UNDEFINED in the shell) */
    sf->cur_sp = sf->arg_buf;
    s->is_completed = FALSE;
    if (snap_get_value(r, &sf->cur_func))
        return -1;
    if (JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT ||
        !snap_is_bytecode_function(JS_VALUE_GET_OBJ(sf->cur_func)->class_id))
        return snap_error(r, "invalid frame function");
    if (c->u.frame.arg_buf_len < b->arg_count ||
        b->var_count != c->u.frame.var_count ||
        b->stack_size != c->u.frame.stack_size ||
        b->var_ref_count != c->u.frame.var_ref_count)
        return snap_error(r, "frame layout mismatch");
    if (snap_get_value(r, &s->this_val) ||
        snap_get_leb(r, &argc) ||
        snap_get_leb(r, &js_mode) ||
        snap_get_leb(r, &pc_pos) ||
        snap_get_leb(r, &sp_pos))
        return -1;
    if (pc_pos > b->byte_code_len ||
        sp_pos > c->u.frame.arg_buf_len + c->u.frame.var_count + c->u.frame.stack_size)
        return snap_error(r, "invalid frame state");
    s->argc = argc;
    sf->js_mode = js_mode | JS_MODE_ASYNC;
    sf->cur_pc = b->byte_code_buf + pc_pos;
    /* values beyond cur_sp are not initialized */
    for(i = 0; i < sp_pos; i++) {
        if (snap_get_value(r, &sf->arg_buf[i])) {
            sf->cur_sp = sf->arg_buf + i;
            return -1;
        }
    }
    sf->cur_sp = sf->arg_buf + sp_pos;

    {
        JSAsyncFunctionState *child;
        uint8_t is_suspended_frame, pending_native, pending_opcode;
        uint32_t pending_argc;
        if (bc_get_u8(&r->bc, &is_suspended_frame) ||
            snap_get_value(r, &s->new_target) ||
            snap_get_ref(r, SNAP_CELL_FRAME, (void **)&child) ||
            bc_get_u8(&r->bc, &pending_native) ||
            bc_get_u8(&r->bc, &pending_opcode) ||
            snap_get_leb(r, &pending_argc))
            return -1;
        if (child && (!is_suspended_frame || child == s || pending_native))
            return snap_error(r, "invalid suspended frame");
        if (pending_native && !is_suspended_frame)
            return snap_error(r, "invalid suspended frame");
        s->is_suspended_frame = is_suspended_frame;
        s->pending_native = pending_native;
        s->pending_opcode = pending_opcode;
        s->pending_argc = pending_argc;
        if (child) {
            js_rc(child)->ref_count++;
            s->suspended_child = child;
        }
    }
    return 0;
}

static int snap_read_fast_array(SnapReader *r, JSObject *p)
{
    JSValue *tab;
    uint32_t i, count;

    if (snap_get_leb(r, &count))
        return -1;
    if (count == 0)
        return 0;
    if (count > INT32_MAX / sizeof(JSValue))
        return snap_error(r, "invalid array length");
    tab = js_malloc(r->ctx, sizeof(tab[0]) * count);
    if (!tab)
        return -1;
    for(i = 0; i < count; i++)
        tab[i] = JS_UNDEFINED;
    p->u.array.u.values = tab;
    p->u.array.count = count;
    p->u.array.offset = 0;
    if (p->class_id == JS_CLASS_ARRAY)
        p->u.array.u1.size = count;
    for(i = 0; i < count; i++) {
        if (snap_get_value(r, &tab[i]))
            return -1;
    }
    return 0;
}

static int snap_read_payload(SnapReader *r, JSObject *p, int flags)
{
    JSContext *ctx = r->ctx;
    uint32_t i, n;

    switch(p->class_id) {
    case JS_CLASS_OBJECT:
    case JS_CLASS_MODULE_NS:
    case JS_CLASS_ITERATOR:
    case JS_CLASS_RAWJSON:
        break;
    case JS_CLASS_ARRAY:
    case JS_CLASS_ARGUMENTS:
        snap_payload_ready(r); /* empty array part */
        if (flags & SNAP_OBJ_FAST_ARRAY)
            return snap_read_fast_array(r, p);
        p->fast_array = 0;
        break;
    case JS_CLASS_MAPPED_ARGUMENTS:
        snap_payload_ready(r); /* empty array part */
        if (flags & SNAP_OBJ_FAST_ARRAY) {
            JSVarRef **tab;
            if (snap_get_leb(r, &n))
                return -1;
            if (n == 0)
                break;
            if (n > INT32_MAX / sizeof(tab[0]))
                return snap_error(r, "invalid arguments length");
            tab = js_mallocz(ctx, sizeof(tab[0]) * n);
            if (!tab)
                return -1;
            for(i = 0; i < n; i++) {
                JSVarRef *var_ref;
                if (snap_get_ref(r, SNAP_CELL_VAR_REF, (void **)&var_ref) || !var_ref) {
                    for(n = 0; n < i; n++)
                        free_var_ref(r->rt, tab[n]);
                    js_free(ctx, tab);
                    return snap_error(r, "invalid mapped arguments");
                }
                js_rc(var_ref)->ref_count++;
                tab[i] = var_ref;
            }
            p->u.array.u.var_refs = tab;
            p->u.array.count = n;
        } else {
            p->fast_array = 0;
        }
        break;
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_DATE:
    case JS_CLASS_BIG_INT:
        snap_payload_ready(r); /* object_data is not a reference */
        return snap_get_value(r, &p->u.object_data);
    case JS_CLASS_ERROR:
        snap_payload_ready(r); /* the stack is not a reference */
        return snap_get_value(r, &p->u.error.stack);
    case JS_CLASS_CALL_SITE:
        {
            JSCallSiteData *csd;
            uint8_t present, bits;
            if (bc_get_u8(&r->bc, &present))
                return -1;
            if (!present)
                break;
            csd = js_mallocz(ctx, sizeof(*csd));
            if (!csd)
                return -1;
            csd->filename = JS_NULL;
            csd->func = JS_NULL;
            csd->func_name = JS_NULL;
            p->u.opaque = csd;
            snap_payload_ready(r);
            if (snap_get_value(r, &csd->filename) ||
                snap_get_value(r, &csd->func) ||
                snap_get_value(r, &csd->func_name) ||
                bc_get_leb128_int(&r->bc, &csd->line_num) ||
                bc_get_leb128_int(&r->bc, &csd->col_num) ||
                bc_get_u8(&r->bc, &bits))
                return -1;
            csd->native = bits & 1;
            csd->constructor = (bits >> 1) & 1;
        }
        break;
    case JS_CLASS_C_FUNCTION:
        {
            void *func;
            uint8_t length, cproto;
            int32_t magic;
            p->u.cfunc.realm = NULL;
            snap_payload_ready(r);
            if (snap_get_context(r, &p->u.cfunc.realm) ||
                snap_get_native(r, &func) ||
                bc_get_u8(&r->bc, &length) ||
                bc_get_u8(&r->bc, &cproto) ||
                bc_get_sleb128(&r->bc, &magic))
                return -1;
            p->u.cfunc.c_function.generic = func;
            p->u.cfunc.length = length;
            p->u.cfunc.cproto = cproto;
            p->u.cfunc.magic = magic;
        }
        break;
    case JS_CLASS_BYTECODE_FUNCTION:
    case JS_CLASS_GENERATOR_FUNCTION:
    case JS_CLASS_ASYNC_FUNCTION:
    case JS_CLASS_ASYNC_GENERATOR_FUNCTION:
        {
            JSValue bval;
            JSFunctionBytecode *b;
            /* the shell only cleared u.opaque */
            p->u.func.function_bytecode = NULL;
            p->u.func.var_refs = NULL;
            p->u.func.home_object = NULL;
            snap_payload_ready(r);
            if (snap_get_value(r, &bval))
                return -1;
            if (JS_VALUE_GET_TAG(bval) != JS_TAG_FUNCTION_BYTECODE) {
                JS_FreeValue(ctx, bval);
                return snap_error(r, "invalid function bytecode");
            }
            b = JS_VALUE_GET_PTR(bval);
            p->u.func.function_bytecode = b;
            if (b->closure_var_count) {
                p->u.func.var_refs = js_mallocz(ctx, sizeof(JSVarRef *) * b->closure_var_count);
                if (!p->u.func.var_refs)
                    return -1;
            }
            if (snap_get_obj_value(r, &p->u.func.home_object))
                return -1;
            for(i = 0; i < b->closure_var_count; i++) {
                JSVarRef *var_ref;
                if (snap_get_ref(r, SNAP_CELL_VAR_REF, (void **)&var_ref))
                    return -1;
                if (var_ref)
                    js_rc(var_ref)->ref_count++;
                p->u.func.var_refs[i] = var_ref;
            }
        }
        break;
    case JS_CLASS_BOUND_FUNCTION:
        {
            JSValue func_obj, this_val;
            JSBoundFunction *bf;
            if (snap_get_value(r, &func_obj))
                return -1;
            if (snap_get_value(r, &this_val)) {
                JS_FreeValue(ctx, func_obj);
                return -1;
            }
            if (snap_get_leb(r, &n) || n > JS_MAX_LOCAL_VARS ||
                !(bf = js_malloc(ctx, sizeof(*bf) + n * sizeof(JSValue)))) {
                JS_FreeValue(ctx, func_obj);
                JS_FreeValue(ctx, this_val);
                return snap_error(r, "invalid bound function");
            }
            bf->func_obj = func_obj;
            bf->this_val = this_val;
            bf->argc = n;
            for(i = 0; i < n; i++)
                bf->argv[i] = JS_UNDEFINED;
            p->u.bound_function = bf;
            snap_payload_ready(r);
            for(i = 0; i < n; i++) {
                if (snap_get_value(r, &bf->argv[i]))
                    return -1;
            }
        }
        break;
    case JS_CLASS_C_FUNCTION_DATA:
        {
            JSCFunctionDataRecord *s;
            void *func;
            uint8_t length, data_len;
            uint32_t magic;
            snap_payload_ready(r); /* the finalizer accepts a NULL record */
            if (snap_get_native(r, &func) ||
                bc_get_u8(&r->bc, &length) ||
                bc_get_u8(&r->bc, &data_len) ||
                snap_get_leb(r, &magic))
                return -1;
            s = js_malloc(ctx, sizeof(*s) + data_len * sizeof(JSValue));
            if (!s)
                return -1;
            s->func = func;
            s->length = length;
            s->data_len = data_len;
            s->magic = magic;
            for(i = 0; i < data_len; i++)
                s->data[i] = JS_UNDEFINED;
            p->u.c_function_data_record = s;
            for(i = 0; i < data_len; i++) {
                if (snap_get_value(r, &s->data[i]))
                    return -1;
            }
        }
        break;
    case JS_CLASS_GLOBAL_OBJECT:
        snap_payload_ready(r); /* uninitialized_vars is JS_UNDEFINED */
        return snap_get_value(r, &p->u.global_object.uninitialized_vars);
    case JS_CLASS_REGEXP:
        {
            JSValue pattern, bytecode;
            if (snap_get_value(r, &pattern))
                return -1;
            if (snap_get_value(r, &bytecode)) {
                JS_FreeValue(ctx, pattern);
                return -1;
            }
            if (JS_VALUE_GET_TAG(pattern) != JS_TAG_STRING ||
                JS_VALUE_GET_TAG(bytecode) != JS_TAG_STRING) {
                JS_FreeValue(ctx, pattern);
                JS_FreeValue(ctx, bytecode);
                return snap_error(r, "invalid regexp");
            }
            p->u.regexp.pattern = JS_VALUE_GET_STRING(pattern);
            p->u.regexp.bytecode = JS_VALUE_GET_STRING(bytecode);
        }
        break;
    default:
        {
            int ret = snap_read_data_payload(r, p);
            if (ret <= 0)
                return ret;
            ret = snap_read_control_payload(r, p);
            if (ret <= 0)
                return ret;
        }
        if (p->class_id >= JS_CLASS_INIT_COUNT) {
            uint8_t has_opaque;
            SnapOpaque *o;
            if (bc_get_u8(&r->bc, &has_opaque))
                return -1;
            if (!has_opaque)
                break;
            if (!r->host || !r->host->load_opaque)
                return snap_error(r, "host class data without load_opaque hook");
            if (snap_resize(r->rt, (void **)&r->opaques, sizeof(r->opaques[0]),
                            &r->opaque_size, r->opaque_count + 1)) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            o = &r->opaques[r->opaque_count++];
            o->obj = p;
            o->saved = JS_UNDEFINED;
            return snap_get_value(r, &o->saved);
        }
        return snap_error(r, "unsupported object class %d", p->class_id);
    }
    return 0;
}

/* Read the property 'atom' of 'p' (the reference to 'atom' is freed) and
   add it. The payload is read before the property is created so that 'p'
   stays consistent on error. */
static int snap_read_property(SnapReader *r, JSObject *p, JSAtom atom)
{
    JSContext *ctx = r->ctx;
    uint8_t flags, id;
    JSProperty *pr, prop;
    JSContext *realm;
    void *opaque;

    if (bc_get_u8(&r->bc, &flags))
        goto fail;
    if (flags & ~(JS_PROP_C_W_E | JS_PROP_LENGTH | JS_PROP_TMASK)) {
        snap_error(r, "invalid property flags");
        goto fail;
    }
    switch(flags & JS_PROP_TMASK) {
    case JS_PROP_NORMAL:
        if (snap_get_value(r, &prop.u.value))
            goto fail;
        break;
    case JS_PROP_GETSET:
        if (snap_get_obj_value(r, &prop.u.getset.getter))
            goto fail;
        if (snap_get_obj_value(r, &prop.u.getset.setter)) {
            if (prop.u.getset.getter)
                JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, prop.u.getset.getter));
            goto fail;
        }
        break;
    case JS_PROP_VARREF:
        if (snap_get_ref(r, SNAP_CELL_VAR_REF, (void **)&prop.u.var_ref))
            goto fail;
        if (!prop.u.var_ref) {
            snap_error(r, "invalid variable property");
            goto fail;
        }
        js_rc(prop.u.var_ref)->ref_count++;
        break;
    case JS_PROP_AUTOINIT:
        if (snap_get_context(r, &realm))
            goto fail;
        if (!realm || bc_get_u8(&r->bc, &id) || id > JS_AUTOINIT_ID_PROP) {
            if (realm)
                JS_FreeContext(realm);
            snap_error(r, "invalid lazy property");
            goto fail;
        }
        opaque = NULL;
        if (id == JS_AUTOINIT_ID_MODULE_NS) {
            if (snap_get_ref(r, SNAP_CELL_MODULE, &opaque) || !opaque) {
                JS_FreeContext(realm);
                snap_error(r, "invalid lazy property");
                goto fail;
            }
        } else if (id == JS_AUTOINIT_ID_PROP) {
            if (snap_get_native(r, &opaque) || !opaque) {
                JS_FreeContext(realm);
                snap_error(r, "invalid lazy property");
                goto fail;
            }
        }
        prop.u.init.realm_and_id = (uintptr_t)realm | id;
        prop.u.init.opaque = opaque;
        break;
    }
    pr = add_property(ctx, p, atom, flags);
    if (!pr) {
        free_property(r->rt, &prop, flags);
        goto fail;
    }
    *pr = prop;
    JS_FreeAtom(ctx, atom);
    return 0;
 fail:
    JS_FreeAtom(ctx, atom);
    return -1;
}

static int snap_read_object(SnapReader *r, int idx)
{
    JSContext *ctx = r->ctx;
    JSObject *p = r->cells[idx].ptr, *proto;
    JSShape *sh;
    JSAtom atom;
    uint8_t flags;

    if (snap_get_obj(r, &proto) || bc_get_u8(&r->bc, &flags))
        return -1;
    r->obj_flags[idx] = flags;

    /* switch from the empty shell shape to the prototype's initial shape */
    sh = find_hashed_shape_proto(r->rt, proto);
    if (sh) {
        sh = js_dup_shape(sh);
    } else {
        sh = js_new_shape(ctx, proto);
        if (!sh)
            return -1;
    }
    if (sh->prop_size != p->shape->prop_size) {
        JSProperty *new_prop = js_realloc(ctx, p->prop, sizeof(p->prop[0]) * sh->prop_size);
        if (!new_prop) {
            js_free_shape(r->rt, sh);
            return -1;
        }
        p->prop = new_prop;
    }
    js_free_shape(r->rt, p->shape);
    p->shape = sh;

    for(;;) {
        if (bc_get_atom(&r->bc, &atom))
            return -1;
        if (atom == JS_ATOM_NULL)
            break;
        if (snap_read_property(r, p, atom))
            return -1;
    }
    r->cur_obj = p;
    r->cur_obj_ready = FALSE;
    if (snap_read_payload(r, p, flags))
        return -1;
    r->cur_obj = NULL;
    return 0;
}

static int snap_read_cell(SnapReader *r, int idx)
{
    SnapRCell *c = &r->cells[idx];
    switch(c->type) {
    case SNAP_CELL_CONTEXT:
        return snap_read_context(r, c->ptr);
    case SNAP_CELL_OBJECT:
        return snap_read_object(r, idx);
    case SNAP_CELL_VAR_REF:
        return snap_read_var_ref(r, c->ptr);
    case SNAP_CELL_FRAME:
        return snap_read_frame(r, c);
    case SNAP_CELL_MODULE:
        return snap_read_module(r, c->ptr);
    case SNAP_CELL_BYTECODE:
        return snap_read_bytecode(r, c);
    case SNAP_CELL_RESOLVED:
        return snap_read_resolved(r, c->ptr);
    default:
        abort();
    }
}

/* allocate a context without intrinsics. Every field is set when its
   body is read. */
static JSContext *snap_new_context(JSRuntime *rt)
{
    JSContext *ctx;
    int i;

    ctx = js_mallocz_rt(rt, sizeof(JSContext));
    if (!ctx)
        return NULL;
    ctx->class_proto = js_malloc_rt(rt, sizeof(ctx->class_proto[0]) * rt->class_count);
    if (!ctx->class_proto) {
        js_free_rt(rt, ctx);
        return NULL;
    }
    js_rc(ctx)->ref_count = 1;
    add_gc_object(rt, &ctx->header, JS_GC_OBJ_TYPE_JS_CONTEXT);
    ctx->rt = rt;
    list_add_tail(&ctx->link, &rt->context_list);
    for(i = 0; i < rt->class_count; i++)
        ctx->class_proto[i] = JS_NULL;
    ctx->function_proto = JS_NULL;
    ctx->function_ctor = JS_NULL;
    ctx->array_ctor = JS_NULL;
    ctx->regexp_ctor = JS_NULL;
    ctx->promise_ctor = JS_NULL;
    ctx->error_ctor = JS_NULL;
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++)
        ctx->native_error_proto[i] = JS_NULL;
    ctx->iterator_ctor = JS_NULL;
    ctx->async_iterator_proto = JS_NULL;
    ctx->array_proto_values = JS_NULL;
    ctx->throw_type_error = JS_NULL;
    ctx->eval_obj = JS_NULL;
    for(i = 0; i < JS_CONTEXT_VALUE_COUNT; i++)
        ctx->user_values[i] = JS_NULL;
    ctx->global_obj = JS_NULL;
    ctx->global_var_obj = JS_NULL;
    ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT;
    init_list_head(&ctx->loaded_modules);
    return ctx;
}

/* allocate a cell from its header. The cell is valid (it can be freed)
   but empty. */
static int snap_new_cell(SnapReader *r, SnapRCell *c)
{
    JSContext *ctx = r->ctx;
    JSRuntime *rt = r->rt;

    switch(c->type) {
    case SNAP_CELL_OBJECT:
        {
            JSShape *sh;
            JSProperty dummy;
            JSValue obj;

            if (c->u.class_id >= rt->class_count ||
                !JS_IsRegisteredClass(rt, c->u.class_id))
                return snap_error(r, "unregistered class %d", c->u.class_id);
            sh = find_hashed_shape_proto(rt, NULL);
            if (sh) {
                sh = js_dup_shape(sh);
            } else {
                sh = js_new_shape(ctx, NULL);
                if (!sh)
                    return -1;
            }
            /* non NULL props: no implicit 'length' property for arrays */
            obj = JS_NewObjectFromShape(ctx, sh, c->u.class_id, &dummy);
            if (JS_IsException(obj))
                return -1;
            c->ptr = JS_VALUE_GET_OBJ(obj);
        }
        break;
    case SNAP_CELL_VAR_REF:
        c->ptr = js_create_var_ref(ctx, FALSE);
        if (!c->ptr)
            return -1;
        break;
    case SNAP_CELL_FRAME:
        {
            JSAsyncFunctionState *s;
            JSStackFrame *sf;
            uint64_t n_values, size;
            uint32_t i;

            n_values = (uint64_t)c->u.frame.arg_buf_len + c->u.frame.var_count +
                c->u.frame.stack_size;
            size = sizeof(*s) + sizeof(JSValue) * n_values +
                sizeof(JSVarRef *) * (uint64_t)c->u.frame.var_ref_count;
            if (size > INT32_MAX)
                return snap_error(r, "invalid frame size");
            /* same layout as async_func_init() */
            s = js_mallocz(ctx, size);
            if (!s)
                return -1;
            js_rc(s)->ref_count = 1;
            add_gc_object(rt, &s->header, JS_GC_OBJ_TYPE_ASYNC_FUNCTION);
            s->is_completed = TRUE; /* until the frame body is read */
            s->this_val = JS_UNDEFINED;
            s->new_target = JS_UNDEFINED;
            s->resolving_funcs[0] = JS_UNDEFINED;
            s->resolving_funcs[1] = JS_UNDEFINED;
            sf = &s->frame;
            sf->cur_func = JS_UNDEFINED;
            sf->js_mode = JS_MODE_ASYNC;
            sf->arg_buf = (JSValue *)(s + 1);
            sf->arg_count = c->u.frame.arg_buf_len;
            sf->var_buf = sf->arg_buf + c->u.frame.arg_buf_len;
            sf->cur_sp = sf->arg_buf;
            sf->var_refs = (JSVarRef **)(sf->arg_buf + n_values);
            for(i = 0; i < c->u.frame.var_ref_count; i++)
                sf->var_refs[i] = NULL;
            c->ptr = s;
        }
        break;
    case SNAP_CELL_MODULE:
        c->ptr = snap_new_module(r);
        if (!c->ptr)
            return -1;
        break;
    case SNAP_CELL_BYTECODE:
        {
            JSFunctionBytecode bc;
            memset(&bc, 0, sizeof(bc));
            bc.has_debug = c->u.bytecode.has_debug;
            bc.arg_count = c->u.bytecode.arg_count;
            bc.var_count = c->u.bytecode.var_count;
            bc.stack_size = c->u.bytecode.stack_size;
            bc.var_ref_count = c->u.bytecode.var_ref_count;
            bc.closure_var_count = c->u.bytecode.closure_var_count;
            bc.cpool_count = c->u.bytecode.cpool_count;
            bc.byte_code_len = c->u.bytecode.byte_code_len;
            c->ptr = js_alloc_function_bytecode(ctx, &bc, c->u.bytecode.local_count);
            if (!c->ptr)
                return -1;
        }
        break;
    case SNAP_CELL_RESOLVED:
        c->ptr = snap_new_resolved(r);
        if (!c->ptr)
            return -1;
        break;
    default:
        abort();
    }
    return 0;
}

/* magic, version, kind, base id, signature and class table */
static int snap_read_header(SnapReader *r, SnapKind kind, uint64_t *pid)
{
    JSRuntime *rt = r->rt;
    uint32_t version, signature, class_count, i, len;
    uint64_t id;
    uint8_t registered, kind1;

    if (r->bc.buf_end - r->bc.ptr < sizeof(snap_magic) ||
        memcmp(r->bc.ptr, snap_magic, sizeof(snap_magic)) != 0)
        return snap_error(r, "not a QuickJS snapshot");
    r->bc.ptr += sizeof(snap_magic);
    if (snap_hdr_leb(r, &version))
        return -1;
    if (version != SNAP_FORMAT_VERSION)
        return snap_error(r, "unsupported snapshot version %u", version);
    if (snap_hdr_u8(r, &kind1))
        return -1;
    if (kind1 != kind)
        return snap_error(r, kind == SNAP_KIND_FULL ? "not a full snapshot" : "not a delta snapshot");
    if (r->bc.buf_end - r->bc.ptr < 8 + 4)
        return snap_error(r, "truncated snapshot");
    id = get_u64(r->bc.ptr);
    if (is_be())
        id = bswap64(id);
    *pid = id;
    r->bc.ptr += 8;
    signature = get_u32(r->bc.ptr);
    if (is_be())
        signature = bswap32(signature);
    r->bc.ptr += 4;
    if (signature != r->natives.signature)
        return snap_error(r, "snapshot was written by another engine build or with other host natives");

    if (snap_hdr_leb(r, &class_count))
        return -1;
    if (class_count > rt->class_count)
        return snap_error(r, "snapshot uses %u classes, runtime has %d", class_count, rt->class_count);
    for(i = JS_CLASS_INIT_COUNT; i < class_count; i++) {
        if (snap_hdr_u8(r, &registered))
            return -1;
        if (registered) {
            char buf[ATOM_GET_STR_BUF_SIZE];
            const char *name;
            if (snap_hdr_leb(r, &len) || len > r->bc.buf_end - r->bc.ptr)
                return snap_error(r, "truncated snapshot");
            if (!JS_IsRegisteredClass(rt, i))
                return snap_error(r, "class %u is not registered", i);
            name = JS_AtomGetStrRT(rt, buf, sizeof(buf), rt->class_array[i].class_name);
            if (strlen(name) != len || memcmp(name, r->bc.ptr, len) != 0)
                return snap_error(r, "class %u is '%s', snapshot expects '%.*s'",
                                  i, name, (int)len, r->bc.ptr);
            r->bc.ptr += len;
        }
    }
    return 0;
}

/* cell headers, appended to r->cells (the cells are allocated later) */
static int snap_read_cell_headers(SnapReader *r)
{
    uint32_t i, n;
    uint8_t type;

    if (snap_hdr_leb(r, &n))
        return -1;
    if (n > (r->bc.buf_end - r->bc.ptr))
        return snap_error(r, "truncated snapshot");
    if (snap_resize(r->rt, (void **)&r->cells, sizeof(r->cells[0]),
                    &r->cell_size, r->cell_count + n))
        return snap_error(r, "out of memory");
    for(i = 0; i < n; i++) {
        SnapRCell *c = &r->cells[r->cell_count];
        memset(c, 0, sizeof(*c));
        if (snap_hdr_u8(r, &type))
            return -1;
        if (type >= SNAP_CELL_TYPE_COUNT)
            return snap_error(r, "invalid cell type");
        c->type = type;
        switch(type) {
        case SNAP_CELL_OBJECT:
            if (snap_hdr_leb(r, &c->u.class_id))
                return -1;
            break;
        case SNAP_CELL_FRAME:
            if (snap_hdr_leb(r, &c->u.frame.arg_buf_len) ||
                snap_hdr_leb(r, &c->u.frame.var_count) ||
                snap_hdr_leb(r, &c->u.frame.stack_size) ||
                snap_hdr_leb(r, &c->u.frame.var_ref_count))
                return -1;
            if ((c->u.frame.arg_buf_len | c->u.frame.var_count |
                 c->u.frame.stack_size | c->u.frame.var_ref_count) > JS_MAX_LOCAL_VARS)
                return snap_error(r, "invalid frame size");
            break;
        case SNAP_CELL_BYTECODE:
            {
                uint8_t has_debug;
                if (snap_hdr_u8(r, &has_debug) ||
                    snap_hdr_leb(r, &c->u.bytecode.arg_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.var_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.stack_size) ||
                    snap_hdr_leb(r, &c->u.bytecode.var_ref_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.closure_var_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.cpool_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.local_count) ||
                    snap_hdr_leb(r, &c->u.bytecode.byte_code_len))
                    return -1;
                c->u.bytecode.has_debug = has_debug != 0;
                if ((c->u.bytecode.arg_count | c->u.bytecode.var_count |
                     c->u.bytecode.stack_size | c->u.bytecode.var_ref_count) > JS_MAX_LOCAL_VARS ||
                    (c->u.bytecode.closure_var_count | c->u.bytecode.cpool_count |
                     c->u.bytecode.local_count | c->u.bytecode.byte_code_len) > INT32_MAX)
                    return snap_error(r, "invalid bytecode size");
            }
            break;
        default:
            break;
        }
        r->cell_count++;
    }
    return 0;
}

/* create the shells of the cells [start, r->cell_count), contexts first:
   one is needed to allocate the others */
static int snap_new_cells(SnapReader *r, int start)
{
    int i, pass;

    for(pass = 0; pass < 2; pass++) {
        for(i = start; i < r->cell_count; i++) {
            SnapRCell *c = &r->cells[i];
            if ((c->type == SNAP_CELL_CONTEXT) != (pass == 0))
                continue;
            if (c->type == SNAP_CELL_CONTEXT) {
                c->ptr = snap_new_context(r->rt);
                if (!c->ptr)
                    return snap_error(r, "out of memory");
                if (!r->ctx) {
                    r->ctx = c->ptr;
                    r->bc.ctx = r->ctx;
                }
            } else {
                if (!r->ctx)
                    return snap_error(r, "snapshot without context");
                if (snap_new_cell(r, c))
                    return -1;
            }
        }
    }
    return 0;
}

/* atom table, appended to the current one */
static int snap_read_atoms(SnapReader *r)
{
    BCReaderState *s = &r->bc;
    JSString *str;
    JSAtom atom;
    uint32_t i, n;
    uint8_t kind;
    JSAtom *tab;

    if (snap_get_leb(r, &n))
        return -1;
    if (n > s->buf_end - s->ptr)
        return snap_error(r, "truncated snapshot");
    tab = js_realloc_rt(r->rt, s->idx_to_atom,
                        sizeof(s->idx_to_atom[0]) * max_int(s->idx_to_atom_count + n, 1));
    if (!tab)
        return snap_error(r, "out of memory");
    s->idx_to_atom = tab;
    for(i = 0; i < n; i++) {
        if (bc_get_u8(s, &kind))
            return -1;
        str = NULL;
        if (kind != SNAP_ATOM_SYMBOL_NO_DESC) {
            str = JS_ReadString(s);
            if (!str)
                return -1;
        }
        switch(kind) {
        case SNAP_ATOM_STRING:
            atom = __JS_NewAtom(r->rt, str, JS_ATOM_TYPE_STRING);
            break;
        case SNAP_ATOM_GLOBAL_SYMBOL:
            atom = __JS_NewAtom(r->rt, str, JS_ATOM_TYPE_GLOBAL_SYMBOL);
            break;
        case SNAP_ATOM_SYMBOL:
        case SNAP_ATOM_SYMBOL_NO_DESC:
            atom = __JS_NewAtom(r->rt, str, JS_ATOM_TYPE_SYMBOL);
            break;
        case SNAP_ATOM_PRIVATE:
            atom = __JS_NewAtom(r->rt, str, JS_ATOM_TYPE_PRIVATE);
            break;
        default:
            if (str)
                js_free_string(r->rt, str);
            return snap_error(r, "invalid atom kind");
        }
        if (atom == JS_ATOM_NULL)
            return snap_error(r, "out of memory");
        s->idx_to_atom[s->idx_to_atom_count++] = atom;
    }
    return 0;
}

/* string table, appended to the current one */
static int snap_read_strings(SnapReader *r)
{
    uint32_t i, n;

    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    if (snap_resize(r->rt, (void **)&r->strs, sizeof(r->strs[0]),
                    &r->str_size, r->str_count + n))
        return snap_error(r, "out of memory");
    for(i = 0; i < n; i++) {
        JSString *p = JS_ReadString(&r->bc);
        if (!p)
            return -1;
        r->strs[r->str_count++] = p;
    }
    return 0;
}

static int snap_read_roots(SnapReader *r, JSSnapshotResult *res)
{
    JSRuntime *rt = r->rt;
    uint32_t i, j, n, argc;

    /* contexts, in creation order: moving each one to the end of the
       list restores the order */
    if (snap_get_leb(r, &n))
        return -1;
    if (n == 0 || n > r->cell_count)
        return snap_error(r, "invalid context list");
    res->contexts = js_mallocz_rt(rt, sizeof(res->contexts[0]) * n);
    if (!res->contexts)
        return snap_error(r, "out of memory");
    for(i = 0; i < n; i++) {
        JSContext *ctx;
        if (snap_get_ref(r, SNAP_CELL_CONTEXT, (void **)&ctx))
            return -1;
        if (!ctx)
            return snap_error(r, "invalid context list");
        if (r->roots_seeded) {
            JSFloat64Union u;
            uint64_t elapsed_ns;
            if (bc_get_u64(&r->bc, &ctx->random_state))
                return -1;
            if (ctx->random_state == 0)
                return snap_error(r, "invalid random state");
            if (bc_get_u64(&r->bc, &u.u64) ||
                bc_get_u64(&r->bc, &elapsed_ns))
                return -1;
            ctx->time_origin = u.d;
            ctx->time_origin_ns = js_hrtime_ns() - (int64_t)elapsed_ns;
        } else {
            js_random_init(ctx);
            js_performance_init(ctx);
        }
        list_del(&ctx->link);
        list_add_tail(&ctx->link, &rt->context_list);
        res->contexts[res->context_count++] = JS_DupContext(ctx);
    }

    /* pending jobs */
    if (snap_get_leb(r, &n))
        return -1;
    for(i = 0; i < n; i++) {
        JSContext *realm;
        void *func;
        JSJobEntry *e;

        if (snap_get_context(r, &realm))
            return -1;
        if (!realm || snap_get_native(r, &func) || !func ||
            snap_get_leb(r, &argc) || argc > JS_MAX_LOCAL_VARS) {
            if (realm)
                JS_FreeContext(realm);
            return snap_error(r, "invalid job");
        }
        e = js_malloc_rt(rt, sizeof(*e) + argc * sizeof(JSValue));
        if (!e) {
            JS_FreeContext(realm);
            return snap_error(r, "out of memory");
        }
        e->realm = realm;
        e->job_func = func;
        e->argc = argc;
        for(j = 0; j < argc; j++)
            e->argv[j] = JS_UNDEFINED;
        list_add_tail(&e->link, &rt->job_list);
        for(j = 0; j < argc; j++) {
            if (snap_get_value(r, &e->argv[j]))
                return -1;
        }
    }

    {
        uint64_t ts;
        if (bc_get_u64(&r->bc, &ts))
            return -1;
        rt->module_async_evaluation_next_timestamp = ts;
    }

    /* host roots */
    if (snap_get_leb(r, &n))
        return -1;
    if (n > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    res->roots = js_mallocz_rt(rt, sizeof(res->roots[0]) * max_int(n, 1));
    if (!res->roots)
        return snap_error(r, "out of memory");
    for(i = 0; i < n; i++) {
        res->roots[i] = JS_UNDEFINED;
        res->root_count++;
        if (snap_get_value(r, &res->roots[i]))
            return -1;
    }

    /* suspended execution */
    {
        JSAsyncFunctionState *s;
        if (snap_get_ref(r, SNAP_CELL_FRAME, (void **)&s))
            return -1;
        if (s) {
            js_rc(s)->ref_count++;
            rt->suspended = s;
        }
    }
    return 0;
}

/* make the cells whose body was not read safe to free */
static void snap_neutralize_cells(SnapReader *r)
{
    int i;
    for(i = 0; i < r->cell_count; i++) {
        SnapRCell *c = &r->cells[i];
        if (!c->ptr || c->done)
            continue;
        if (c->type == SNAP_CELL_OBJECT &&
            !(c->ptr == r->cur_obj && r->cur_obj_ready)) {
            /* no finalizer for a possibly incomplete class payload */
            JSObject *p = c->ptr;
            p->class_id = JS_CLASS_OBJECT;
            p->fast_array = 0;
            p->is_exotic = 0;
        }
    }
}

/* read the body of the cell 'idx': the next 'len' bytes of the input */
static int snap_read_body(SnapReader *r, int idx, uint32_t len)
{
    const uint8_t *end = r->bc.buf_end;
    SnapRCell *c = &r->cells[idx];

    if (len > end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    if (c->done)
        return snap_error(r, "%s #%d has two bodies", snap_cell_type_names[c->type], idx);
    r->bc.buf_end = r->bc.ptr + len;
    if (snap_read_cell(r, idx))
        return -1;
    if (r->bc.ptr != r->bc.buf_end)
        return snap_error(r, "corrupted %s #%d", snap_cell_type_names[c->type], idx);
    r->bc.buf_end = end;
    c->done = TRUE;
    return 0;
}

/* bodies of the base cells, except the ones replaced by the delta */
static int snap_read_base_bodies(SnapReader *r)
{
    uint32_t i, n, len;

    snap_set_input(r, &r->base_bodies);
    if (snap_get_leb(r, &n))
        return -1;
    if (n != r->base_cell_count)
        return snap_error(r, "invalid body count");
    for(i = 0; i < n; i++) {
        if (snap_get_leb(r, &len))
            return -1;
        if (len > r->bc.buf_end - r->bc.ptr)
            return snap_error(r, "truncated snapshot");
        r->base_body_ptrs[i] = r->bc.ptr;
        r->base_body_lens[i] = len;
        if (r->cells[i].changed) {
            r->bc.ptr += len;
        } else {
            if (snap_read_body(r, i, len))
                return -1;
        }
    }
    if (r->bc.ptr != r->bc.buf_end)
        return snap_error(r, "trailing data");
    return 0;
}

/* Read the body of the changed base cell 'idx' from its patch, the next
   'len' bytes (see snap_put_changed_body()): the body is rebuilt from its
   base body, then read. */
static int snap_read_patched_body(SnapReader *r, int idx, uint32_t len)
{
    const uint8_t *base_body = r->base_body_ptrs[idx];
    const uint8_t *start = r->bc.buf_start, *end = r->bc.buf_end, *patch_end;
    uint32_t base_len = r->base_body_lens[idx], prefix, suffix, mid, size;
    uint8_t *body;
    int ret;

    if (len > end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    patch_end = r->bc.ptr + len;
    r->bc.buf_end = patch_end;
    if (snap_get_leb(r, &prefix) || snap_get_leb(r, &suffix))
        return -1;
    if (prefix > base_len || suffix > base_len - prefix)
        return snap_error(r, "invalid delta body");
    mid = patch_end - r->bc.ptr;
    size = prefix + mid + suffix;
    body = js_malloc_rt(r->rt, max_int(size, 1));
    if (!body)
        return snap_error(r, "out of memory");
    memcpy(body, base_body, prefix);
    memcpy(body + prefix, r->bc.ptr, mid);
    memcpy(body + prefix + mid, base_body + base_len - suffix, suffix);
    r->bc.buf_start = r->bc.ptr = body;
    r->bc.buf_end = body + size;
    ret = snap_read_body(r, idx, size);
    js_free_rt(r->rt, body);
    r->bc.buf_start = start;
    r->bc.ptr = patch_end;
    r->bc.buf_end = end;
    return ret;
}

/* bodies of the changed base cells and of the new cells */
static int snap_read_delta_bodies(SnapReader *r)
{
    uint32_t i, n, idx, len;

    snap_set_input(r, &r->delta_bodies);
    if (snap_get_leb(r, &n))
        return -1;
    for(i = 0; i < n; i++) {
        if (snap_get_leb(r, &idx) || snap_get_leb(r, &len))
            return -1;
        if (idx >= r->cell_count ||
            (idx < r->base_cell_count && !r->cells[idx].changed) ||
            ((len & 1) && idx >= r->base_cell_count))
            return snap_error(r, "invalid delta body");
        if ((len & 1) ? snap_read_patched_body(r, idx, len >> 1) :
            snap_read_body(r, idx, len >> 1))
            return -1;
    }
    if (r->bc.ptr != r->bc.buf_end)
        return snap_error(r, "trailing data");
    return 0;
}

/* the snapshot that was read is a base: attach it to the runtime with
   the base bodies, so that the runtime can write deltas */
static int snap_attach_read_base(SnapReader *r)
{
    JSRuntime *rt = r->rt;
    JSSnapshotBase *base;
    uint32_t off;
    int i, n = r->base_cell_count;

    base = js_mallocz_rt(rt, sizeof(*base));
    if (!base)
        return snap_error(r, "out of memory");
    rt->snapshot_base = base; /* freed by JS_DropSnapshotBase() on error */
    base->id = r->base_id;
    off = 0;
    for(i = 0; i < n; i++)
        off += r->base_body_lens[i];
    base->cells = js_malloc_rt(rt, sizeof(base->cells[0]) * max_int(n, 1));
    base->body_offsets = js_malloc_rt(rt, sizeof(base->body_offsets[0]) * (n + 1));
    base->bodies = js_malloc_rt(rt, max_int(off, 1));
    base->atoms = js_malloc_rt(rt, sizeof(base->atoms[0]) * max_int(r->base_atom_count, 1));
    base->strs = js_malloc_rt(rt, sizeof(base->strs[0]) * max_int(r->base_str_count, 1));
    if (!base->cells || !base->body_offsets || !base->bodies || !base->atoms || !base->strs)
        goto fail;
    off = 0;
    for(i = 0; i < n; i++) {
        SnapCell *c = &base->cells[i];
        c->ptr = r->cells[i].ptr;
        c->type = r->cells[i].type;
        snap_cell_dup(c);
        base->cell_count++;
        if (snap_map_add(rt, &base->cell_map, c->ptr, i))
            goto fail;
        base->body_offsets[i] = off;
        memcpy(base->bodies + off, r->base_body_ptrs[i], r->base_body_lens[i]);
        off += r->base_body_lens[i];
    }
    base->body_offsets[n] = off;
    for(i = 0; i < r->base_atom_count; i++)
        base->atoms[base->atom_count++] = JS_DupAtom(r->ctx, r->bc.idx_to_atom[i]);
    for(i = 0; i < r->base_str_count; i++) {
        base->strs[base->str_count++] = JS_DupValue(r->ctx, JS_MKPTR(JS_TAG_STRING, r->strs[i]));
        if (snap_map_add(rt, &base->str_map, r->strs[i], i))
            goto fail;
    }
    return 0;
 fail:
    JS_DropSnapshotBase(rt);
    return snap_error(r, "out of memory");
}

/* drop the references held by the reader */
static void snap_reader_free(SnapReader *r)
{
    JSRuntime *rt = r->rt;
    int i;

    for(i = 0; i < r->opaque_count; i++)
        JS_FreeValueRT(rt, r->opaques[i].saved);
    js_free_rt(rt, r->opaques);
    js_free_rt(rt, r->fixups);
    for(i = 0; i < r->cell_count; i++) {
        SnapRCell *c = &r->cells[i];
        if (c->ptr) {
            SnapCell c1 = { c->ptr, c->type };
            snap_cell_free(rt, &c1);
        }
    }
    js_free_rt(rt, r->cells);
    js_free_rt(rt, r->obj_flags);
    js_free_rt(rt, r->base_body_ptrs);
    js_free_rt(rt, r->base_body_lens);
    for(i = 0; i < r->str_count; i++)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, r->strs[i]));
    js_free_rt(rt, r->strs);
    for(i = 0; i < r->bc.idx_to_atom_count; i++)
        JS_FreeAtomRT(rt, r->bc.idx_to_atom[i]);
    js_free_rt(rt, r->bc.idx_to_atom);
    snap_natives_free(rt, &r->natives);
}

void JS_FreeSnapshotResult(JSRuntime *rt, JSSnapshotResult *res)
{
    js_free_rt(rt, res->contexts);
    js_free_rt(rt, res->roots);
    memset(res, 0, sizeof(*res));
}

int JS_ReadSnapshot(JSRuntime *rt, const uint8_t *buf, size_t buf_len,
                    const uint8_t *delta, size_t delta_len,
                    const JSSnapshotHost *host, JSSnapshotResult *res,
                    char *errbuf, size_t errbuf_size)
{
    SnapReader rr, *r = &rr;
    SnapInput in;
    size_t gc_threshold;
    int i, ret = -1;

    memset(r, 0, sizeof(*r));
    memset(res, 0, sizeof(*res));
    if (errbuf_size)
        errbuf[0] = '\0';
    r->rt = rt;
    r->host = host;
    r->errbuf = errbuf;
    r->errbuf_size = errbuf_size;
    r->bc.allow_bytecode = TRUE;
    r->bc.is_snapshot = TRUE;
    r->bc.first_atom = JS_ATOM_END;
    /* the heap is inconsistent until the end: no GC */
    gc_threshold = rt->malloc_gc_threshold;
    rt->malloc_gc_threshold = SIZE_MAX;

    if (!list_empty(&rt->context_list) || !list_empty(&rt->job_list)) {
        snap_error(r, "the runtime must be empty");
        goto done;
    }
    if (js_init_proxy_class(rt) || js_init_async_classes(rt) ||
        js_init_weakref_classes(rt) || js_init_domexception_class(rt) ||
        snap_natives_init(rt, &r->natives, host, FALSE)) {
        snap_error(r, "out of memory");
        goto done;
    }
    /* base: cells, atoms, strings; the roots and bodies are read later */
    in.start = in.ptr = buf;
    in.end = buf + buf_len;
    snap_set_input(r, &in);
    if (snap_read_header(r, SNAP_KIND_FULL, &r->base_id) ||
        snap_read_cell_headers(r))
        goto fail;
    r->base_cell_count = r->cell_count;
    r->base_body_ptrs = js_mallocz_rt(rt, sizeof(r->base_body_ptrs[0]) * max_int(r->cell_count, 1));
    r->base_body_lens = js_mallocz_rt(rt, sizeof(r->base_body_lens[0]) * max_int(r->cell_count, 1));
    if (!r->base_body_ptrs || !r->base_body_lens) {
        snap_error(r, "out of memory");
        goto fail;
    }
    if (snap_new_cells(r, 0) || snap_read_atoms(r) || snap_read_strings(r) ||
        snap_hdr_section(r, &r->base_roots))
        goto fail;
    r->base_atom_count = r->bc.idx_to_atom_count;
    r->base_str_count = r->str_count;
    r->base_bodies.start = buf;
    r->base_bodies.ptr = r->bc.ptr;
    r->base_bodies.end = buf + buf_len;

    if (delta) {
        uint64_t id;
        uint32_t n, idx;
        int start = r->cell_count;

        in.start = in.ptr = delta;
        in.end = delta + delta_len;
        snap_set_input(r, &in);
        if (snap_read_header(r, SNAP_KIND_DELTA, &id))
            goto fail;
        if (!r->base_id || id != r->base_id) {
            snap_error(r, "the delta was not written from this base");
            goto fail;
        }
        if (snap_read_cell_headers(r) || snap_new_cells(r, start))
            goto fail;
        if (snap_hdr_leb(r, &n))
            goto fail;
        for(i = 0; i < n; i++) {
            if (snap_hdr_leb(r, &idx))
                goto fail;
            if (idx >= r->base_cell_count || r->cells[idx].changed) {
                snap_error(r, "invalid changed cell");
                goto fail;
            }
            r->cells[idx].changed = TRUE;
        }
        if (snap_read_atoms(r) || snap_read_strings(r) ||
            snap_hdr_section(r, &r->delta_roots))
            goto fail;
        r->delta_bodies.start = delta;
        r->delta_bodies.ptr = r->bc.ptr;
        r->delta_bodies.end = delta + delta_len;
    }

    r->obj_flags = js_mallocz_rt(rt, max_int(r->cell_count, 1));
    if (!r->obj_flags) {
        snap_error(r, "out of memory");
        goto fail;
    }
    snap_set_input(r, delta ? &r->delta_roots : &r->base_roots);
    r->roots_seeded = delta || !r->base_id;
    if (snap_read_roots(r, res))
        goto fail;
    if (r->bc.ptr != r->bc.buf_end) {
        snap_error(r, "corrupted roots");
        goto fail;
    }
    if (snap_read_base_bodies(r) || (delta && snap_read_delta_bodies(r)))
        goto fail;
    for(i = 0; i < r->cell_count; i++) {
        if (!r->cells[i].done) {
            snap_error(r, "%s #%d has no body", snap_cell_type_names[r->cells[i].type], i);
            goto fail;
        }
    }
    while (r->fixup_count > 0) {
        SnapFixup *f = &r->fixups[--r->fixup_count];
        if (f->func(r, f->opaque, FALSE))
            goto fail;
    }

    /* flags that add_property() may have changed while rebuilding */
    for(i = 0; i < r->cell_count; i++) {
        if (r->cells[i].type == SNAP_CELL_OBJECT) {
            JSObject *p = r->cells[i].ptr;
            int flags = r->obj_flags[i];
            p->extensible = (flags & SNAP_OBJ_EXTENSIBLE) != 0;
            p->is_std_array_prototype = (flags & SNAP_OBJ_STD_ARRAY_PROTO) != 0;
            p->has_immutable_prototype = (flags & SNAP_OBJ_IMMUTABLE_PROTO) != 0;
            p->is_constructor = (flags & SNAP_OBJ_CONSTRUCTOR) != 0;
            p->is_HTMLDDA = (flags & SNAP_OBJ_HTMLDDA) != 0;
        }
    }
    rt->malloc_gc_threshold = gc_threshold;

    /* host-class data, now that the heap is complete */
    for(i = 0; i < r->opaque_count; i++) {
        SnapOpaque *o = &r->opaques[i];
        if (host->load_opaque(r->ctx, JS_MKPTR(JS_TAG_OBJECT, o->obj), o->saved,
                              host->opaque) < 0)
            goto fail;
    }
    if (r->base_id && snap_attach_read_base(r))
        goto fail;
    ret = 0;
    goto done;
 fail:
    while (r->fixup_count > 0) {
        SnapFixup *f = &r->fixups[--r->fixup_count];
        f->func(r, f->opaque, TRUE);
    }
    snap_neutralize_cells(r);
    if (errbuf_size && !errbuf[0]) {
        /* error thrown by the engine while reading */
        JSValue exc = r->ctx ? JS_GetException(r->ctx) : JS_UNINITIALIZED;
        const char *msg = NULL;
        if (JS_IsObject(exc)) {
            JSValue m = JS_GetPropertyStr(r->ctx, exc, "message");
            msg = JS_ToCString(r->ctx, m);
            JS_FreeValue(r->ctx, m);
        }
        snprintf(errbuf, errbuf_size, "%s", msg ? msg : "invalid snapshot");
        if (msg)
            JS_FreeCString(r->ctx, msg);
        JS_FreeValue(r->ctx, exc);
    }
    for(i = 0; i < res->context_count; i++)
        JS_FreeContext(res->contexts[i]);
    for(i = 0; i < res->root_count; i++)
        JS_FreeValueRT(rt, res->roots[i]);
    JS_FreeSnapshotResult(rt, res);
 done:
    rt->malloc_gc_threshold = gc_threshold;
    snap_reader_free(r);
    return ret;
}

#include "quickjs-snapshot-data.c"
#include "quickjs-snapshot-control.c"
